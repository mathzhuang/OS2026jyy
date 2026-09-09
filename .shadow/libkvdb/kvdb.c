/*
 * kvdb.c —— M9: Key-value Database (libkvdb) 的实现
 *
 * 设计目标（按实验文档 M9）：
 *   1. 支持 kvdb_open / kvdb_put / kvdb_get / kvdb_close 四个接口；
 *   2. 线程安全 + 进程安全：允许"多个进程中的多个线程"同时打开、并发访问同一个
 *      数据库，一个线程/进程 put 的 key 必须能被另一个线程/进程 get 到；
 *   3. 崩溃一致性 (crash consistency)：在任意时刻崩溃（断电、kill -9、panic）
 *      之后，数据库要么回到上一次"已提交"的状态，要么就是当前状态——绝不出现
 *      半截写入、文件损坏、或"已返回成功但丢失"的情况。
 *
 * ─────────────────────────────────────────────────────────────────────
 * 一、数据格式（磁盘上只有一个"快照"文件，无独立日志区）
 * ─────────────────────────────────────────────────────────────────────
 *   [8 字节魔数 "KVDBSNAP"][(klen:u32)(key)(vlen:u32)(value)] × N
 *
 *   魔数用于区分"我们写过的数据库"与"空文件 / 陌生文件"：
 *     - 0 字节文件  == 空数据库（kvdb_open 在路径不存在时创建的就是它）；
 *     - 非空文件首 8 字节必须等于魔数，否则视为损坏。
 *   u32 长度用 memcpy 打包/解包（避免对齐问题），按宿主机端序存放——读写都
 *   在本机完成，端序无关紧要。因为 put/get 的 key/value 都是 C 字符串
 *   （不含内嵌 '\0'），所以记录里可以直接按 strlen 定长存取。
 *
 * ─────────────────────────────────────────────────────────────────────
 * 二、崩溃一致性：写时复制 + fsync + 原子 rename（单文件快照替换）
 * ─────────────────────────────────────────────────────────────────────
 *   一次 kvdb_put 只做一件事：把"旧的全部记录读进来 → 改/加一条 → 序列化成
 *   一份完整的新文件字节" 写到临时文件 <path>.tmp，然后：
 *
 *       write(tmp) → fsync(tmp) → rename(tmp, path) → fsync(所在目录)
 *
 *   - rename 在 POSIX 上是"原子"的：目录项要么指向旧 inode、要么指向新 inode，
 *     绝不存在"指向半截内容"的中间态；
 *   - 新文件在 rename 之前已经 fsync，数据先落盘；因此崩溃只可能发生在
 *     (a) rename 之前 → path 仍是完整旧库（本次 put 未提交，可以丢弃）；
 *     (b) rename 之后 → path 是完整新库（put 已提交）。
 *     无论哪种，path 指向的都是一份"完整、一致"的快照，绝不会有撕裂的库文件。
 *   - 只有 kvdb_put 在 fsync + rename 全部完成后才返回 0，所以"已提交"的操作
 *     一定会持久化，不会丢失。崩溃后残留的 <path>.tmp 是未完成写操作留下的，
 *     在下次 kvdb_open / kvdb_put 时会被清理/截断，不影响正确性。
 *   - 最后对父目录做一次 fsync（尽力而为）是为了让"新名字 → 新 inode"的目录
 *     项映射本身也落盘，否则极端情况下（机器重启）rename 可能"倒退"。
 *
 * ─────────────────────────────────────────────────────────────────────
 * 三、进程安全 + 线程安全：用独立锁文件的 flock 作"一把大锁"
 * ─────────────────────────────────────────────────────────────────────
 *   因为上面的实现每次 put 都会用 rename 换掉数据库 inode，所以不能拿数据库
 *   文件本身来 flock：一个进程先前打开的 fd 指向旧 inode，另一个进程新打开的
 *   fd 指向新 inode，两把锁会锁到不同的 inode 上 → 互斥失效。
 *
 *   解决办法是引入一个**永不 rename** 的旁路锁文件 <path>.lock：
 *   每个操作（open/put/get）都临时 open 它、flock(LOCK_EX)，临界区结束后
 *   flock(LOCK_UN) + close。要点：
 *     - flock 的锁挂在"打开文件描述 (open file description)"上；两个线程哪怕在
 *       同一个进程里、共用同一个 kvdb_t，只要各自 open 一次锁文件就会得到不同
 *       的描述 → 它们的 flock 会真正互相阻塞（已在实验里验证）；
 *     - 跨进程同理，天然互斥；
 *     - flock 由内核管理：进程崩溃/退出时锁自动释放，没有"死锁残留"问题；
 *     - 锁文件从不被删除/改名，锁的"标的"始终稳定。
 *   于是"多进程 × 多线程"的全部互斥只需要这一把 flock 大锁即可，无需 pthread
 *   互斥量、也无需给 struct kvdb_t 增加字段（kvdb.h 保持原样，只用到它已有的
 *   path / fd 两个字段）。
 *
 *   由于所有 put/get 都在这把排他锁的临界区内完成"读 → 改 → 写"，任意两个
 *   并发操作都存在一个全局顺序（= 加锁顺序），满足可序列化：并发 get 看到的
 *   总是"最近一次已提交 put"的值；对同一 key 的并发 put，最终落盘的是最后
 *   取得锁的那一次，之后所有 get 看到的值一致。
 *
 *   "性能不是本实验重点，使用一把大锁即可通过测试" —— 本实现即为大锁方案，
 *   正确性优先，性能次之。
 */

#include <kvdb.h>

#include <stdint.h>    /* uint32_t 等定宽类型 */
#include <stdio.h>     /* rename 的声明 */
#include <stdlib.h>    /* malloc / free / realloc */
#include <string.h>    /* memcpy / strlen / strrchr */
#include <unistd.h>    /* read / write / close / fsync */
#include <fcntl.h>     /* open 及 O_* 标志 */
#include <sys/file.h>  /* flock */
#include <errno.h>     /* errno, EINTR, ENOENT */

/* 文件头 8 字节魔数："我们是 kvdb 快照文件"。 */
static const char DB_MAGIC[8] = {'K', 'V', 'D', 'B', 'S', 'N', 'A', 'P'};

/* 旁路锁文件 / 临时文件的路径后缀。 */
static const char LOCK_SUFFIX[] = ".lock";
static const char TMP_SUFFIX[] = ".tmp";

/* 一条 key-value 记录（内存中）。key/value 都是 NUL 结尾的 C 字符串，
 * klen/vlen 是去掉结尾 '\0' 后的真实字节数（字符串内不可能有内嵌 '\0'）。 */
struct rec {
    char  *key;          /* 堆上副本 */
    size_t klen;
    char  *val;          /* 堆上副本 */
    size_t vlen;
};

/* 记录动态数组，充当"整个数据库的内存视图"。 */
struct kvvec {
    struct rec *r;
    size_t      n;       /* 已有记录数 */
    size_t      cap;     /* 容量 */
};

/* ---------- 工具：路径拼接 ---------- */

/* 返回 malloc 的 "<path><suffix>"，失败返回 NULL。 */
static char *suffix_path(const char *path, const char *suffix)
{
    size_t plen = strlen(path), slen = strlen(suffix);
    char *out = malloc(plen + slen + 1);
    if (!out)
        return NULL;
    memcpy(out, path, plen);
    memcpy(out + plen, suffix, slen + 1);
    return out;
}

/* ---------- 工具：一把"大锁"（跨进程、跨线程的 flock） ---------- */

/* 打开 <path>.lock 并对其加排他锁；返回锁 fd（调用方随后把它传给
 * lock_release），失败返回 -1。锁文件用 O_CREAT 保证存在，绝不被删除。 */
static int lock_acquire(const char *path)
{
    char *lp = suffix_path(path, LOCK_SUFFIX);
    if (!lp)
        return -1;

    int fd = open(lp, O_CREAT | O_RDWR, 0600);
    free(lp);
    if (fd < 0)
        return -1;

    if (flock(fd, LOCK_EX) < 0) {   /* 拿不到就阻塞等；进程死了内核自动放锁 */
        close(fd);
        return -1;
    }
    return fd;
}

/* 释放锁并关闭 fd。 */
static void lock_release(int fd)
{
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

/* ---------- 工具：记录数组 ---------- */

/* 释放数组占用的全部内存，并把结构体复位成空（可安全重复调用）。 */
static void vec_free(struct kvvec *v)
{
    for (size_t i = 0; i < v->n; i++) {
        free(v->r[i].key);
        free(v->r[i].val);
    }
    free(v->r);
    v->r = NULL;
    v->n = v->cap = 0;
}

/* 追加一条新记录：把 key/value 深拷贝进堆。返回 0 成功，-1 失败。 */
static int vec_push(struct kvvec *v,
                    const char *key, size_t klen,
                    const char *val, size_t vlen)
{
    if (v->n == v->cap) {                       /* 容量不够则翻倍扩容 */
        size_t ncap = v->cap ? v->cap * 2 : 8;
        struct rec *nr = realloc(v->r, ncap * sizeof(struct rec));
        if (!nr)
            return -1;
        v->r = nr;
        v->cap = ncap;
    }

    char *k = malloc(klen + 1);
    char *vl = malloc(vlen + 1);
    if (!k || !vl) {
        free(k);
        free(vl);
        return -1;
    }
    memcpy(k, key, klen);
    k[klen] = '\0';
    memcpy(vl, val, vlen);
    vl[vlen] = '\0';

    v->r[v->n].key = k;
    v->r[v->n].klen = klen;
    v->r[v->n].val = vl;
    v->r[v->n].vlen = vlen;
    v->n++;
    return 0;
}

/* 在数组中找 key（比较长度 + 字节内容）；找到返回下标，找不到返回 -1。 */
static long vec_find(const struct kvvec *v, const char *key)
{
    size_t klen = strlen(key);
    for (size_t i = 0; i < v->n; i++) {
        if (v->r[i].klen == klen &&
            memcmp(v->r[i].key, key, klen) == 0)
            return (long)i;
    }
    return -1;
}

/* ---------- 磁盘快照的装载 / 序列化 ---------- */

/* 读入 <path> 的当前完整内容并解析成记录数组。
 *   文件不存在或为空（0 字节）→ 空数据库，返回 0；
 *   内容非空但魔数不符 / 越界 → 视为损坏，返回 -1（并清空 v）。 */
static int snapshot_load(const char *path, struct kvvec *v)
{
    v->r = NULL;
    v->n = v->cap = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT)
            return 0;                   /* 文件都还没有 → 空库 */
        return -1;
    }

    /* 把整个文件读进内存（库通常很小，性能不是重点）。 */
    size_t cap = 4096, len = 0;
    char *data = malloc(cap);
    if (!data) {
        close(fd);
        return -1;
    }
    int rc = -1;
    for (;;) {
        if (len == cap) {               /* 缓冲满 → 翻倍后继续读 */
            char *nd = realloc(data, cap * 2);
            if (!nd)
                goto done;
            data = nd;
            cap *= 2;
        }
        ssize_t n = read(fd, data + len, cap - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            goto done;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }

    if (len == 0) {                     /* 空文件 = 空库 */
        rc = 0;
        goto done;
    }
    if (len < 8 || memcmp(data, DB_MAGIC, 8) != 0) {   /* 陌生内容 */
        rc = -1;
        goto done;
    }

    /* 顺序解析记录：每条 = u32 keylen + key + u32 vallen + value
     * （与 snapshot_build 的写出顺序一致）。 */
    rc = 0;
    size_t off = 8;
    while (off < len) {
        uint32_t kl, vl;
        if (len - off < 4) {            /* 连 keylen 域都不完整 */
            rc = -1;
            break;
        }
        memcpy(&kl, data + off, 4); off += 4;
        if ((size_t)kl > len - off) {   /* key 越界 */
            rc = -1;
            break;
        }
        off += kl;
        if (len - off < 4) {            /* 连 vallen 域都不完整 */
            rc = -1;
            break;
        }
        memcpy(&vl, data + off, 4); off += 4;
        if ((size_t)vl > len - off) {   /* value 越界 */
            rc = -1;
            break;
        }
        if (vec_push(v, data + off - kl - 4, kl,
                        data + off, vl) != 0) {
            rc = -1;
            break;
        }
        off += vl;
    }

done:
    free(data);
    close(fd);
    if (rc != 0)
        vec_free(v);                    /* 出错时清掉可能已解析出的部分 */
    return rc;
}

/* 把内存中的记录数组序列化成一份完整快照字节，写入 *out（malloc），
 * 长度写入 *out_len。返回 0 成功，-1 失败。 */
static int snapshot_build(const struct kvvec *v, char **out, size_t *out_len)
{
    size_t need = 8;                    /* 魔数 */
    for (size_t i = 0; i < v->n; i++)
        need += 8 + v->r[i].klen + v->r[i].vlen;   /* 两个长度域 + 内容 */

    char *buf = malloc(need);
    if (!buf)
        return -1;

    memcpy(buf, DB_MAGIC, 8);
    size_t off = 8;
    for (size_t i = 0; i < v->n; i++) {
        uint32_t kl = (uint32_t)v->r[i].klen;
        uint32_t vl = (uint32_t)v->r[i].vlen;
        memcpy(buf + off, &kl, 4); off += 4;
        memcpy(buf + off, v->r[i].key, kl); off += kl;
        memcpy(buf + off, &vl, 4); off += 4;
        memcpy(buf + off, v->r[i].val, vl); off += vl;
    }

    *out = buf;
    *out_len = off;
    return 0;
}

/* ---------- 崩溃一致性的"原子提交" ---------- */

/* 尽力 fsync path 的父目录，让 rename 的目录项变更真正落盘。
 * 失败仅忽略：文件数据已经 fsync 过，进程被 kill 的模型下已足够。 */
static void fsync_parent_dir(const char *path)
{
    char *p = strdup(path);
    if (!p)
        return;

    const char *dir;
    char *slash = strrchr(p, '/');
    if (!slash) {
        dir = ".";                      /* 无路径分隔符 → 当前目录 */
    } else if (slash == p) {
        dir = "/";                      /* 形如 "/foo" */
    } else {
        *slash = '\0';
        dir = p;
    }

    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        fsync(fd);                      /* 结果尽力而为，不检查 */
        close(fd);
    }
    free(p);
}

/* 把 data[0..len) 作为一份完整新库原子地替换 path：
 *   写 <path>.tmp → fsync → rename → fsync 父目录。
 * 返回 0 成功，-1 失败。O_TRUNC 顺便清掉上次崩溃留下的半截 tmp。 */
static int commit_file(const char *path, const char *data, size_t len)
{
    char *tmp = suffix_path(path, TMP_SUFFIX);
    if (!tmp)
        return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        free(tmp);
        return -1;
    }

    size_t off = 0;
    int rc = -1;
    while (off < len) {                 /* 循环写，处理短写 */
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            goto out;
        }
        off += (size_t)n;
    }
    if (fsync(fd) != 0)                 /* 数据必须先落盘，才能 rename */
        goto out;
    rc = 0;

out:
    if (close(fd) != 0 && rc == 0)
        rc = -1;
    if (rc == 0) {
        if (rename(tmp, path) != 0)     /* 原子替换（在锁内，安全） */
            rc = -1;
        else
            fsync_parent_dir(path);
    }
    free(tmp);
    return rc;
}

/* ---------- 四个对外接口 ---------- */

int kvdb_open(struct kvdb_t *db, const char *path)
{
    if (!db || !path)
        return -1;

    /* 路径可能来自栈上/临时缓冲，必须自己保留一份（close 时释放）。 */
    char *dpath = strdup(path);
    if (!dpath)
        return -1;

    /* 加锁做初始化：保证"创建库文件 + 清理残留 tmp"不与其他进程的操作交错。 */
    int lfd = lock_acquire(path);
    if (lfd < 0) {
        free(dpath);
        return -1;
    }

    /* 路径不存在则创建空库文件（0 字节 = 空数据库）；已存在则不动内容。 */
    int f = open(path, O_RDWR | O_CREAT, 0600);
    if (f < 0) {
        lock_release(lfd);
        free(dpath);
        return -1;
    }
    close(f);

    /* 清理上次崩溃可能留下的半截临时文件（此刻持锁，无人在写）。 */
    char *tmp = suffix_path(path, TMP_SUFFIX);
    if (tmp) {
        unlink(tmp);
        free(tmp);
    }

    lock_release(lfd);

    db->path = dpath;   /* struct 里 path 字段现在指向我们的私有副本 */
    db->fd = -1;        /* fd 字段不使用：每次操作都临时 open，避免 inode 变旧 */
    return 0;
}

int kvdb_put(struct kvdb_t *db, const char *key, const char *value)
{
    if (!db || !db->path || !key || !value)
        return -1;
    const char *path = db->path;

    int lfd = lock_acquire(path);       /* —— 进入临界区（排他锁） —— */
    if (lfd < 0)
        return -1;

    struct kvvec v;
    int rc = -1;
    if (snapshot_load(path, &v) != 0)   /* 读当前已提交状态 */
        goto out;

    long idx = vec_find(&v, key);
    if (idx >= 0) {
        /* key 已存在 → 覆盖 value（复用原 key 副本，只换 value）。 */
        struct rec *r = &v.r[idx];
        char *nv = malloc(strlen(value) + 1);
        if (!nv)
            goto out;
        strcpy(nv, value);
        free(r->val);
        r->val = nv;
        r->vlen = strlen(value);
    } else {
        /* key 不存在 → 追加一条新记录。 */
        size_t klen = strlen(key), vlen = strlen(value);
        if (vec_push(&v, key, klen, value, vlen) != 0)
            goto out;
    }

    char *blob;
    size_t bloblen;
    if (snapshot_build(&v, &blob, &bloblen) != 0)
        goto out;
    rc = commit_file(path, blob, bloblen);  /* 原子替换，此时才算提交 */
    free(blob);

out:
    vec_free(&v);
    lock_release(lfd);                  /* —— 离开临界区 —— */
    return rc;
}

int kvdb_get(struct kvdb_t *db, const char *key, char *buf, size_t length)
{
    if (!db || !db->path || !key)
        return -1;
    const char *path = db->path;

    int lfd = lock_acquire(path);       /* —— 进入临界区（排他锁） —— */
    if (lfd < 0)
        return -1;

    struct kvvec v;
    int rc = -1;
    if (snapshot_load(path, &v) != 0)
        goto out;

    long idx = vec_find(&v, key);
    if (idx < 0)                        /* 未找到 → -1 */
        goto out;

    struct rec *r = &v.r[idx];
    size_t copied;
    if (length > 0) {
        /* 最多拷 length-1 字节，并补一个结尾 '\0'。 */
        size_t avail = length - 1;
        copied = (r->vlen < avail) ? r->vlen : avail;
        if (buf && copied > 0)
            memcpy(buf, r->val, copied);
        if (buf)
            buf[copied] = '\0';
    } else {
        /* length == 0：什么都拷不了，按"拷了 0 字节"返回 0
         * （框架示例即以 get(..., NULL, 0) == 0 表示"存在该 key"）。 */
        copied = 0;
    }
    rc = (int)copied;

out:
    vec_free(&v);
    lock_release(lfd);                  /* —— 离开临界区 —— */
    return rc;
}

int kvdb_close(struct kvdb_t *db)
{
    if (!db)
        return -1;
    /* 释放路径副本。每次操作都临时 open，没有常驻 fd / 锁需要关闭。 */
    free((void *)db->path);
    db->path = NULL;
    db->fd = -1;
    return 0;
}
