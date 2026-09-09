/*
 * mymalloc.c —— M5 并行内存分配器（slab 分配器 + 线程本地缓存）
 *
 * ═══════════════════════════════════════════════════════════════════
 * 设计总览
 * ═══════════════════════════════════════════════════════════════════
 * 现代 malloc（tcmalloc / jemalloc）的核心思想是把分配路径分成：
 *   • fast path —— 在【线程本地】完成：只读写本线程自己的缓存，不碰任何
 *     共享状态、不需要加锁，因此多个处理器上的 malloc/free 可以真正并行；
 *   • slow path —— 只有线程本地缓存为空/溢出时才触发：去访问受锁保护的
 *     全局数据结构（各大小类的 slab 链表）。
 *
 * 内存来源：
 *   • vmalloc() 一次申请 1 MiB 的"大区间"，再把它切成 4 KiB 的页；
 *   • 每一页作为一个 slab（板），被切成若干相同大小的槽位（对象）；
 *   • 大于 3072 字节的请求不进 slab，直接用 vmalloc 分配整块区间。
 *
 * 大小分级（size class）：
 *   • 16, 32, 48, ..., 512（步长 16，共 32 个"小对象类"）；
 *   • 1024, 1536, 2048, 2560, 3072（共 5 个"中对象类"）。
 *   请求大小向上取整到所属类，同类对象大小完全相同，保证一页可以切分成
 *   均匀槽位，不会产生内部碎片累积。
 *
 * ═══════════════════════════════════════════════════════════════════
 * 并发一致性（关键，务必理解）
 * ═══════════════════════════════════════════════════════════════════
 * 每个槽位在任意时刻【恰好】位于下列三处之一：
 *    1. 用户手中（被 mymalloc 返回、尚未 myfree）；
 *    2. 某个线程的本地缓存（被 myfree 放入、尚未被再分配）；
 *    3. 某个 slab 的页内空闲链表。
 *
 * 对每个 slab 维护：
 *   • used   —— 离开页内空闲链表的槽位数 = 用户手中 + 线程缓存中；
 *   • cached —— 其中"线程缓存中"的槽位数（由 fast path 用原子操作维护）；
 *   • free   —— 页内空闲链表。
 *   恒有：used + |free| = capacity，且 cached ≤ used。
 *
 * 页面"可以复用"（进入 empty 链表、交给别的大小类重新初始化）的判据
 * 必须是 【used == 0 且 cached == 0】：只有所有槽位都回到页内空闲链表、
 * 且没有任何线程缓存仍引用本页槽位时，这一页才能安全复用。
 * 若只检查 used == 0，一旦 used 在"页面被复用 / 旧化身"之间发生欠计数，
 * 就会出现"页面过早复用而槽位仍存活"的并发灾难：新 slab 的 slab_init
 * 会改写仍被用户持有的槽位，并把活槽位重新链入空闲链表，导致同一地址
 * 被重复分配、缓存链表成环、甚至死循环。cached 由 fast path 原子维护，
 * 因此这个判据是可靠的——这是本次实现排掉的最大一个坑。
 *
 * 各共享结构的加锁保护：
 *   • partial[ci]（每类 slab 链表）与 slab 的 used/free/cached —— 只在
 *     持 class_locks[ci] 时修改；
 *   • empty_list（可复用页）—— empty_lock；
 *   • vm 大区间切页 —— vm_lock；
 *   • tcb 注册表 —— tcb_lock（只在注册/查找慢路径持有）。
 *   锁获取顺序约定为 class_lock → empty_lock，绝无反向嵌套，因此无死锁。
 *
 * ═══════════════════════════════════════════════════════════════════
 * 兼容性
 * ═══════════════════════════════════════════════════════════════════
 * 本文件不使用任何 libc / pthread 接口，可编译进 freestanding 环境
 * （例如操作系统内核）。唯一的系统调用是 gettid（Linux x86_64 = 186），
 * 用于获得当前线程的标识，从而索引到本线程的缓存。
 */

#include <mymalloc.h>

/* ============================ 基础常量 ============================ */

#define PAGE_SIZE      4096UL        /* 一页的大小（vmalloc 长度必须是它的倍数） */
#define SMALL_STRIDE   16UL          /* 小对象类的步长 */
#define SMALL_MAX      512UL         /* 小对象的最大请求大小 */
#define SMALL_CLASSES  (int)(SMALL_MAX / SMALL_STRIDE)  /* 32 个小类 */
#define MEDIUM_CLASSES 5             /* 中对象的类数 */
#define CLASS_COUNT    (SMALL_CLASSES + MEDIUM_CLASSES) /* 37 个类 */

#define NREG           16384         /* tcb 哈希桶数量 */

#define REGION_SIZE    (1UL << 20)   /* 每次向 vmalloc 申请 1 MiB 大区间 */

/* 页头魔数：用于在 myfree 时区分"slab 页"与"大对象区间"。
 * 大对象区间在数据页开头也写入 VM_MAGIC，因此 myfree 只需检查目标页的
 * 第一个字即可分类，绝不会误读用户数据。 */
#define SLAB_MAGIC     0x5A1AB5A25A1AB5A2ULL
#define VM_MAGIC       0xA5A5A5A55A5A5A5AULL

/* ======================= 数据结构定义 ======================= */

/* 空闲槽位链表结点：侵入式，槽位空闲时其头 8 字节借来存 next 指针；
 * 槽位被分配出去后，这块内存完全属于用户，分配器不再触碰。 */
typedef struct slot {
    struct slot *next;
} slot;

/* slab：占据一个 4 KiB 页，页开头存放该结构体，其余空间切成槽位。 */
typedef struct slab {
    uintptr_t    magic;       /* SLAB_MAGIC，标志这是一个 slab 页 */
    struct slab *next;        /* 挂接在 partial / empty 链表上 */
    int          size_class;  /* 属于哪个大小类 */
    int          capacity;    /* 本页最多容纳的槽位数 */
    int          used;        /* 离开页内空闲链表的槽位数（用户手中 + 缓存中） */
    int          cached;      /* 其中位于"线程缓存"的槽位数（原子维护） */
    int          on_partial;  /* 当前是否挂在 partial 链表上（1/0） */
    struct slot *free;        /* 页内空闲槽位链表 */
} slab;

/* vmregion：vmalloc 得到的一段大区间，供切页使用。 */
typedef struct vmregion {
    uintptr_t magic;      /* VM_MAGIC（第 0 页用作区头，不作为槽位分配） */
    size_t    total;      /* 区间总字节数 */
    char     *next_page;  /* 区间内下一个可切出的页 */
    char     *end;        /* 区间末尾 */
} vmregion;

/* tcb：线程控制块，即"线程本地缓存"。每个线程一份，懒分配，存于 tcb_reg。 */
typedef struct tcb {
    long         tid;                 /* 线程标识（gettid 返回值） */
    struct slot *free[CLASS_COUNT];   /* 每类大小的线程本地空闲链表 */
    int          count[CLASS_COUNT];  /* 上述链表长度，用于判断是否该归还 */
    struct tcb  *next;                /* 哈希冲突时的链式指针 */
} tcb;

/* ======================= 全局共享状态 ======================= */

/* 每个大小类的"部分使用"slab 链表及其锁 */
static spinlock_t   class_locks[CLASS_COUNT];
static struct slab *partial[CLASS_COUNT];

/* 完全空闲、可复用的 slab 页链表及其锁 */
static spinlock_t   empty_lock;
static struct slab *empty_list;

/* vmalloc 大区间的切页状态及其锁 */
static spinlock_t   vm_lock;
static vmregion    *vm_cur;

/* 线程缓存哈希表：懒初始化，桶数组本身也用 vmalloc 动态分配 */
static spinlock_t  tcb_lock;
static struct tcb **tcb_reg;          /* 大小为 NREG 的指针数组 */

/* ======================= 小工具函数 ======================= */

/* 获取当前线程的标识。Freestanding 环境下没有 libc，直接做 Linux syscall。 */
#if defined(__x86_64__)
#define SYS_gettid 186
#elif defined(__aarch64__)
#define SYS_gettid 178
#else
#error "Unsupported architecture"
#endif

static inline long gettid(void) {
    long tid;
    __asm__ volatile ("syscall"
                      : "=a"(tid)
                      : "0"((long)SYS_gettid)
                      : "rcx", "r11", "memory");
    return tid;
}

/* 每类对象的字节大小表。请求大小向上取整到表中最近且不小于它的值。 */
static const size_t class_size[CLASS_COUNT] = {
    /* 小对象类：16, 32, ..., 512 */
    16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256,
    272, 288, 304, 320, 336, 352, 368, 384, 400, 416, 432, 448, 464, 480, 496, 512,
    /* 中对象类：1024, 1536, 2048, 2560, 3072 */
    1024, 1536, 2048, 2560, 3072,
};

/* 请求大小 → 大小类下标。返回 -1 表示"大对象"，应直接走 vmalloc。 */
static inline int size_to_class(size_t size) {
    if (size <= SMALL_MAX) {
        /* mymalloc(0) 也分配最小的槽位（16 字节），返回合法指针 */
        size_t s = (size < SMALL_STRIDE) ? SMALL_STRIDE : size;
        return (int)((s + SMALL_STRIDE - 1) / SMALL_STRIDE) - 1;
    }
    /* 中对象类只有 5 个，顺序扫描足够快（大请求本就罕见） */
    for (int i = 0; i < MEDIUM_CLASSES; i++)
        if (size <= class_size[SMALL_CLASSES + i])
            return SMALL_CLASSES + i;
    return -1;
}

/* 某一类 slab 页能容纳的槽位数 */
static inline int slab_capacity(int ci) {
    return (int)((PAGE_SIZE - sizeof(struct slab)) / class_size[ci]);
}

/* ======================= 页与 slab 的管理 ======================= */

/* 从 vm_cur 大区间切出一个新页；区间耗尽则重新 vmalloc。 */
static void *page_alloc(void) {
    spin_lock(&vm_lock);
    if (vm_cur == NULL || vm_cur->next_page >= vm_cur->end) {
        vmregion *r = (vmregion *)vmalloc(NULL, REGION_SIZE);
        if (r == NULL) {
            spin_unlock(&vm_lock);
            return NULL;
        }
        r->magic     = VM_MAGIC;
        r->total     = REGION_SIZE;
        r->next_page = (char *)r + PAGE_SIZE;   /* 第 0 页存区头，从第 1 页切 */
        r->end       = (char *)r + REGION_SIZE;
        vm_cur = r;
    }
    void *p = vm_cur->next_page;
    vm_cur->next_page += PAGE_SIZE;
    spin_unlock(&vm_lock);
    return p;
}

/* 取一个可用于充当 slab 的页：优先复用完全空闲的 empty 页，否则切新页。 */
static struct slab *slab_page_get(void) {
    spin_lock(&empty_lock);
    struct slab *s = empty_list;
    if (s != NULL)
        empty_list = s->next;
    spin_unlock(&empty_lock);
    if (s != NULL)
        return s;
    return (struct slab *)page_alloc();
}

/* 把一页初始化成第 ci 类的 slab：切槽位、串成空闲链表。
 * 只有 used==0 且 cached==0 的页（即所有槽位都已归还）才会被调用，
 * 因此这里的写入不会破坏任何仍被持有的槽位。 */
static void slab_init(struct slab *s, int ci) {
    size_t sz = class_size[ci];
    char  *base = (char *)s;
    s->magic      = SLAB_MAGIC;
    s->next       = NULL;
    s->size_class = ci;
    s->capacity   = slab_capacity(ci);
    s->used       = 0;
    s->cached     = 0;
    s->on_partial = 0;
    s->free       = NULL;
    /* 页内第 sizeof(slab) 字节起的空间，切成 capacity 个大小为 sz 的槽位 */
    struct slot *head = NULL;
    for (int i = s->capacity - 1; i >= 0; i--) {
        slot *sl = (slot *)(base + sizeof(struct slab) + (size_t)i * sz);
        sl->next = head;
        head = sl;
    }
    s->free = head;
}

/* 从 partial[ci] 链表摘下 s（调用者需持有 class_locks[ci]）。
 * 链表理论上无环，护栏仅为防御，避免极端错误导致死循环。 */
static void unlink_partial(struct slab *s) {
    struct slab **pp = &partial[s->size_class];
    long guard = 0;
    while (*pp != NULL && *pp != s) {
        if (++guard > 1000000)
            return;
        pp = &(*pp)->next;
    }
    if (*pp != NULL)
        *pp = s->next;
}

/* ======================= 线程本地缓存 ======================= */

/* 找到当前线程的 tcb（不存在则创建）。
 * 常见情况：一次 gettid + 一次哈希桶读取即可命中，全程无锁。
 * tcb_reg 懒初始化：首次调用时为 NULL，必须先判空再索引，
 * 否则等于解引用 NULL+偏移，必然段错误。 */
static struct tcb *get_tcb(void) {
    long tid = gettid();
    unsigned idx = (unsigned)((unsigned long)tid % NREG);

    /* fast path：桶头直接命中（发布用 release，读取用 acquire，
     * 保证看到的是完全初始化的 tcb） */
    struct tcb *t = NULL;
    struct tcb **reg = (struct tcb **)__atomic_load_n(&tcb_reg, __ATOMIC_ACQUIRE);
    if (reg != NULL) {
        t = (struct tcb *)__atomic_load_n(&reg[idx], __ATOMIC_ACQUIRE);
        if (t != NULL && t->tid == tid)
            return t;
    }

    /* slow path：哈希冲突或首次调用，加锁查找/创建 */
    spin_lock(&tcb_lock);
    if (tcb_reg == NULL) {
        tcb_reg = (struct tcb **)vmalloc(NULL, NREG * sizeof(struct tcb *));
        if (tcb_reg != NULL)
            for (unsigned i = 0; i < NREG; i++)
                tcb_reg[i] = NULL;
        else {
            spin_unlock(&tcb_lock);
            return NULL;
        }
    }
    t = tcb_reg[idx];
    {
        /* 有界化：tcb 链理论无环，护栏仅为防御 */
        long guard = 0;
        while (t != NULL && t->tid != tid) {
            if (++guard > 100000)
                break;
            t = t->next;
        }
    }
    if (t == NULL) {
        /* 一个 tcb 只占几百字节，直接给它一页（懒分配，避免静态占用） */
        t = (struct tcb *)vmalloc(NULL, PAGE_SIZE);
        if (t != NULL) {
            t->tid = tid;
            for (int i = 0; i < CLASS_COUNT; i++)
                t->free[i] = NULL;
            for (int i = 0; i < CLASS_COUNT; i++)
                t->count[i] = 0;
            t->next = tcb_reg[idx];
            __atomic_store_n(&tcb_reg[idx], t, __ATOMIC_RELEASE);
        }
    }
    spin_unlock(&tcb_lock);
    return t;
}

/* 把线程缓存里第 ci 类的全部槽位归还给它们各自的 slab。
 * 借此实现"无内存泄漏 / 内存可复用"，并让真正空掉的页进入 empty 链表。
 * 调用者需持有 class_locks[ci]（本函数内部会嵌套获取 empty_lock）。 */
static void thread_cache_flush(struct tcb *t, int ci) {
    spin_lock(&class_locks[ci]);
    struct slot *head = t->free[ci];
    t->free[ci] = NULL;
    t->count[ci] = 0;
    /* 有界化：缓存链表若因故成环会死循环，护栏兜底（正常情况到不了） */
    long guard = 0;
    while (head != NULL) {
        if (++guard > 10000000) { head = NULL; break; }
        struct slot *next = head->next;
        /* 从槽位地址反推它所属的 slab 页 */
        struct slab *s = (struct slab *)((uintptr_t)head & ~(PAGE_SIZE - 1));
        head->next = s->free;
        s->free = head;
        s->used--;
        /* 槽位离开线程缓存 → cached 减一（flush 持类锁执行，但 fast path
         * 的 cached++ 是无锁的，故这里仍需原子操作） */
        __atomic_fetch_sub(&s->cached, 1, __ATOMIC_RELAXED);
        /* 判空：used==0 且 cached==0 才真正空。cached==0 保证没有任何线程
         * 缓存仍引用本页槽位，杜绝"页面过早复用而槽位仍存活"。 */
        if (s->used == 0 &&
            __atomic_load_n(&s->cached, __ATOMIC_RELAXED) == 0) {
            /* 整个 slab 空下来了：移出 partial，进入 empty 链表等待复用 */
            if (s->on_partial)
                unlink_partial(s);
            s->on_partial = 0;
            spin_lock(&empty_lock);
            s->next = empty_list;
            empty_list = s;
            spin_unlock(&empty_lock);
        } else if (s->used == s->capacity - 1 && !s->on_partial) {
            /* 之前"满"，现在又有了空闲槽位 → 重新回到 partial 链表 */
            s->next = partial[ci];
            partial[ci] = s;
            s->on_partial = 1;
        }
        head = next;
    }
    spin_unlock(&class_locks[ci]);
}

/* ======================= 对外接口 ======================= */

/* mymalloc：分配 size 字节，返回 8 字节对齐、互不重叠的唯一内存区。 */
void *mymalloc(size_t size) {
    int ci = size_to_class(size);

    /* 大对象：直接用 vmalloc 分配"一页头 + 数据页"。
     * 布局：页 0 = {VM_MAGIC, total}；数据页开头 8 字节 = VM_MAGIC（供
     *       myfree 识别，绝不触碰用户数据），数据从数据页 + 8 开始返回。
     * 注意 data 必须把"8 字节识别魔数"也向上对齐到页：否则当请求大小恰为
     * 页大小整数倍时，用户可用的实际空间会比请求少 8 字节，写满即越界
     * 溢出到 mmap 区域之外、破坏相邻内存。data - 8 = 真实可用字节 ≥ size。 */
    if (ci < 0) {
        size_t data  = (size + 8 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        size_t total = data + PAGE_SIZE;
        void *hdr = vmalloc(NULL, total);
        if (hdr == NULL)
            return NULL;
        *(uintptr_t *)hdr                = VM_MAGIC;
        *(size_t *)((char *)hdr + 8)     = total;
        *(uintptr_t *)((char *)hdr + PAGE_SIZE) = VM_MAGIC;
        return (char *)hdr + PAGE_SIZE + 8;
    }

    struct tcb *t = get_tcb();
    if (t == NULL)
        return NULL;

    /* fast path：从线程本地空闲链表弹出，无锁、无共享状态访问 */
    struct slot *p = t->free[ci];
    if (p != NULL) {
        t->free[ci] = p->next;
        t->count[ci]--;
        /* 槽位离开线程缓存 → 其所属 slab 的 cached 减一（relaxed 原子即可：
         * 该计数只用于"是否仍有缓存槽位"的判据，精确顺序无关紧要）。
         * 此页必然未被复用（否则 cached>0 会阻止复用），s 必为正确 slab。 */
        struct slab *ps = (struct slab *)((uintptr_t)p & ~(PAGE_SIZE - 1));
        __atomic_fetch_sub(&ps->cached, 1, __ATOMIC_RELAXED);
        return p;
    }

    /* slow path：线程缓存为空，去全局 partial 链表取对象 */
    spin_lock(&class_locks[ci]);
    struct slab *s = partial[ci];
    if (s != NULL) {
        struct slot *sl = s->free;
        s->free = sl->next;
        s->used++;
        if (s->free == NULL) {        /* 最后一个槽位被取走 → 变满，移出 partial */
            partial[ci] = s->next;
            s->on_partial = 0;
        }
        spin_unlock(&class_locks[ci]);
        return sl;
    }
    spin_unlock(&class_locks[ci]);

    /* 连 partial 都是空的：新建一个 slab（优先复用 empty 页）。
     * 这里先释放类锁再取页，避免持锁做耗时的 vmalloc；即使两个线程同时
     * 各建一个 slab 也无妨，多出的页仍会被后续分配使用。 */
    struct slab *ns = slab_page_get();
    if (ns == NULL)
        return NULL;
    slab_init(ns, ci);
    spin_lock(&class_locks[ci]);
    ns->next = partial[ci];
    partial[ci] = ns;
    ns->on_partial = 1;
    struct slot *sl = ns->free;
    ns->free = sl->next;
    ns->used = 1;
    if (ns->free == NULL) {           /* capacity == 1 的类取走即满 */
        partial[ci] = ns->next;
        ns->on_partial = 0;
    }
    spin_unlock(&class_locks[ci]);
    return sl;
}

/* myfree：释放 ptr 指向的内存。
 * - 空指针：无操作（与 POSIX free(NULL) 语义一致）；
 * - slab 槽位：push 进当前线程缓存（fast path，无锁）；
 * - 大对象区间：整段归还给 vmalloc。
 * 由页开头的魔数区分两者，绝不读取用户数据。 */
void myfree(void *ptr) {
    if (ptr == NULL)
        return;

    uintptr_t page = (uintptr_t)ptr & ~(PAGE_SIZE - 1);
    if (*(uintptr_t *)page == SLAB_MAGIC) {
        /* slab 槽位 → 进线程缓存 */
        struct slab *s = (struct slab *)page;
        int ci = s->size_class;
        struct tcb *t = get_tcb();
        if (t == NULL)
            return;                   /* 极端情况（OOM）：宁可泄漏也不崩溃 */
        struct slot *sl = (struct slot *)ptr;
        /* 槽位进入线程缓存 → 其所属 slab 的 cached 加一（fast path 无锁） */
        __atomic_fetch_add(&s->cached, 1, __ATOMIC_RELAXED);
        sl->next = t->free[ci];
        t->free[ci] = sl;
        /* 缓存超过一个 slab 的容量 → 批量归还，防止线程缓存无界膨胀 */
        if (++t->count[ci] > slab_capacity(ci))
            thread_cache_flush(t, ci);
    } else {
        /* 大对象 → 整段归还 */
        uintptr_t hdr = page - PAGE_SIZE;
        size_t total = *(size_t *)(hdr + 8);
        vmfree((void *)hdr, total);
    }
}
