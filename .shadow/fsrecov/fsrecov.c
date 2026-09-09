/*
 * fsrecov.c —— 从"快速格式化"后的 FAT32 文件系统镜像中恢复 BMP 图片
 *
 * 实验背景
 * ========
 * mkfs.fat 的"快速格式化"只会做两件事：
 *   1. 重建引导扇区（BPB），并把文件分配表 FAT 全部清零；
 *   2. 清空根目录簇。
 * 数据区里绝大多数簇的内容原封不动地保留了下来。因此，只要我们能
 *   1. 从数据簇中"嗅探"出旧目录簇（目录项里存着文件名、起始簇、大小）；
 *   2. 根据目录项提供的起始簇与文件大小，把文件内容从数据区拼出来；
 * 就能把大部分图片抢救回来。
 *
 * 输出格式（OJ 只解析这种行）
 * ==========================
 *   <40 位十六进制 sha1 校验和>  <文件名>
 * 例如：
 *   d60e7d3d2b47d19418af5b0ba52406b86ec6ef83  0M15CwG1yP32UPCp.bmp
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "fat32.h"

/* ------------------------------------------------------------------ */
/* 全局几何信息（从引导扇区读出后缓存）                                 */
/* ------------------------------------------------------------------ */
static u8 *disk;                 /* mmap 后的磁盘镜像基址                */
static u32 bps, spc, rsvd, numfats, fatsz;   /* BPB 关键字段           */
static u32 fat_off;              /* FAT 区起始字节偏移                    */
static u32 data_off;             /* 数据区起始字节偏移                   */
static u32 clus_size;            /* 一个簇的字节数 = spc * bps          */
static u32 nclusters;            /* 数据区总共的簇数                     */

/* 簇号 c 在数据区中的字节指针（FAT32 中簇号从 2 开始）                 */
static inline u8 *clus_ptr(u32 c) {
    return disk + data_off + (size_t)(c - 2) * clus_size;
}

/* ------------------------------------------------------------------ */
/* BMP 文件头解析                                                      */
/* ------------------------------------------------------------------ */
struct bmpinfo {
    u32 data_off;    /* 像素数据起始偏移（一般 54）                     */
    u32 width;       /* 位图宽（像素）                                  */
    u32 height;      /* 位图高（像素，取绝对值）                        */
    u16 bpp;         /* 每像素位数（实验里都是 24）                     */
};

/* 解析簇 c 开头的 BMP 头；不是合法 BMP 时返回 0                         */
static int bmp_parse(u32 c, struct bmpinfo *b) {
    u8 *p = clus_ptr(c);
    if (p[0] != 'B' || p[1] != 'M') return 0;      /* "BM" 魔数          */
    memcpy(&b->data_off, p + 10, 4);
    memcpy(&b->width,   p + 18, 4);
    int32_t h; memcpy(&h, p + 22, 4);
    b->height = h < 0 ? (u32)(-h) : (u32)h;         /* 有符号高度取绝对值 */
    memcpy(&b->bpp, p + 28, 2);
    /* 宽高加合理上限：既排除明显损坏的头部，也防止后续像素循环爆炸     */
    return (b->width > 0 && b->width < 100000
         && b->height > 0 && b->height < 100000
         && b->bpp == 24) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 簇分类标记                                                          */
/*   对每个数据簇打三类标记，供后续"找目录 / 找文件头 / 找空闲"使用      */
/* ------------------------------------------------------------------ */
static u8 *is_header;    /* 以合法 BMP 头开头的簇（多半是一个文件的起始） */
static u8 *is_dir;       /* 长得像目录（含大量合法 32 字节目录项）的簇    */
static u8 *is_free;      /* 全零簇（格式化后从未被写过的空闲簇）          */

/* 判断簇 c 是不是一个合法的目录簇：
 *   目录 = 一串 32 字节目录项。一个目录簇里通常存了很多 .bmp 文件，
 *   因此簇内容里会出现很多次 "BMP"（8.3 短名后缀 + LFN 小写 "bmp"），
 *   这是像素数据里极少出现的特征。我们据此做粗略但稳健的分类。        */
static int dir_cluster_score(u32 c) {
    u8 *p = clus_ptr(c);
    int bmp_cnt = 0;
    for (u32 i = 0; i + 3 < clus_size; i++)
        if ((p[i]=='B'||p[i]=='b') && (p[i+1]=='M'||p[i+1]=='m')
            && (p[i+2]=='P'||p[i+2]=='p')) bmp_cnt++;
    /* 一个普通文件项至少带来 1~2 次 "bmp"；目录通常含几十个文件项 */
    if (bmp_cnt < 3) return 0;

    /* 进一步校验：簇内 32 字节对齐处应当能解析出若干合法目录项 */
    int valid = 0;
    for (u32 off = 0; off < clus_size; off += 32) {
        u8 b0 = p[off];
        if (b0 == 0x00) { valid++; break; }        /* 结束标记           */
        if (b0 == 0xe5 || b0 == 0x2e) { valid++; continue; }  /* 已删/点项 */
        u8 attr = p[off + 11];
        if (attr == 0x0f) { valid++; continue; }   /* LFN 项             */
        /* 短名项：名字应全为可打印字符，属性位应合法 */
        int ok = 1;
        for (int k = 0; k < 11; k++) {
            u8 ch = p[off + k];
            if (!(ch == ' ' || (ch >= 0x21 && ch <= 0x7e))) { ok = 0; break; }
        }
        if (ok && (attr & ~0x3f) == 0) valid++;
    }
    return valid >= 2 ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 目录项 / 长文件名（LFN）解析                                        */
/* ------------------------------------------------------------------ */
#define MAX_FILES 2048
struct fileent {
    char name[80];     /* 恢复出的完整文件名                             */
    u32 start;         /* 文件数据起始簇号                               */
    u32 size;          /* 文件字节数                                     */
};
static struct fileent files[MAX_FILES];
static int nfiles = 0;

/* LFN 缓冲跨簇保持：一个文件的 LFN 项可能跨目录簇边界存放
 * （例如 seq2 在簇 A 末尾、seq1 在簇 B 开头），因此这里用 static，
 * 只在目录簇之间延续，不随 scan_dir_cluster 的调用而清空。          */
static u8 lfnbuf[64][32];
static int nlfn = 0;

/* 收集目录簇 c 中的所有文件项（含 LFN 重建完整文件名）                  */
static void scan_dir_cluster(u32 c) {
    u8 *p = clus_ptr(c);

    for (u32 off = 0; off < clus_size; off += 32) {
        u8 b0 = p[off];
        if (b0 == 0x00) break;                      /* 目录结束           */
        u8 attr = p[off + 11];
        if (b0 == 0xe5) { nlfn = 0; continue; }     /* 已删除项：丢弃 LFN */
        if (attr == 0x0f) {                         /* 长文件名项         */
            if (nlfn < 64) memcpy(lfnbuf[nlfn++], p + off, 32);
            continue;
        }
        if (b0 == 0x2e) { nlfn = 0; continue; }     /* "." 或 ".."        */

        struct fat32dent *de = (struct fat32dent *)(p + off);
        u32 start = ((u32)de->DIR_FstClusHI << 16) | de->DIR_FstClusLO;
        u32 size  = de->DIR_FileSize;

        char name[80];
        int len = 0;
        if (nlfn > 0) {
            /* LFN 项按序列号 1..N 顺序拼接（序列号小的先出现）           */
            for (int s = 1; s <= 20; s++) {
                int found = 0;
                for (int k = 0; k < nlfn; k++)
                    if ((lfnbuf[k][0] & 0x3f) == s) { found = 1; break; }
                if (!found) break;                  /* 序列号不连续则停    */
                for (int k = 0; k < nlfn; k++) {
                    if ((lfnbuf[k][0] & 0x3f) != s) continue;
                    u8 *e = lfnbuf[k];
                    /* 一个 LFN 项含 13 个 UTF-16LE 字符，分三段存放      */
                    const u8 *part[3] = { e+1, e+14, e+28 };
                    const int  plen[3] = { 10, 12, 4 };
                    for (int pi = 0; pi < 3; pi++)
                        for (int j = 0; j < plen[pi]; j += 2) {
                            u16 ch = part[pi][j] | (part[pi][j+1] << 8);
                            if (ch == 0 || ch == 0xffff) goto lfn_done;
                            if (ch < 0x80 && len < 79) name[len++] = (char)ch;
                        }
                    break;
                }
            }
        lfn_done:;
        } else {
            /* 没有 LFN：直接用 8.3 短名 */
            char base[9] = {0}, ext[4] = {0};
            memcpy(base, de->DIR_Name, 8);
            memcpy(ext,  de->DIR_Name + 8, 3);
            for (int k = 7; k >= 0 && base[k] == ' '; k--) base[k] = 0;
            for (int k = 2; k >= 0 && ext[k]  == ' '; k--) ext[k]  = 0;
            len = sprintf(name, "%s%s%s", base, ext[0] ? "." : "", ext);
        }
        name[len] = 0;
        nlfn = 0;

        /* 只保留看起来像 BMP 的文件，并做基本合法性检查                 */
        int is_bmp = 0;
        {
            size_t l = strlen(name);
            if (l > 4 && strcmp(name + l - 4, ".bmp") == 0) is_bmp = 1;
            if (l > 4 && strcmp(name + l - 4, ".BMP") == 0) is_bmp = 1;
        }
        if (!is_bmp) continue;
        if (start < 2 || start > nclusters + 1) continue;   /* 簇号越界   */
        /* 文件不可能比整个数据区还大（分段的文件会"绕回"，故不能按
         * 起始簇到盘尾的连续空间判断，只能按数据区总大小兜底）        */
        if (size == 0 || size > (size_t)nclusters * clus_size) continue;
        /* 起始簇必须是合法 BMP 头，否则不是可恢复的图片                 */
        struct bmpinfo b;
        if (!bmp_parse(start, &b)) continue;
        /* 去重（同一文件可能被多个目录簇同时引用）                      */
        for (int k = 0; k < nfiles; k++)
            if (strcmp(files[k].name, name) == 0) goto dup;
        if (nfiles < MAX_FILES) {
            strncpy(files[nfiles].name, name, 79);
            files[nfiles].start = start;
            files[nfiles].size  = size;
            nfiles++;
        }
    dup:;
    }
}

/* ------------------------------------------------------------------ */
/* 行对齐的像素连续性度量（用于贪心链重构）                             */
/* ------------------------------------------------------------------ */
static struct bmpinfo cur_bmp;   /* 当前正在恢复的文件的 BMP 信息        */

/* 一个簇的"行内部平滑度"：把该簇按目标图片的行结构重新切行，
 * 计算行内相邻像素的平均色差。真实属于本图片的簇，重新切行后行内像素
 * 平滑（色差小）；属于别的图片（宽度不同）的簇，重新切行会把原图行切碎，
 * 行内色差很大。phase 是"簇首字节相对行首的相位"。                     */
static double row_internal_diff(u32 c, u32 phase) {
    u32 rb = (cur_bmp.width * 3 + 3) & ~3u;   /* 每行字节数（含填充）     */
    u8 *cl = clus_ptr(c);
    double total = 0; u32 cnt = 0;
    for (u32 k = 0; k < clus_size / rb + 2; k++) {
        long ro = (long)k * rb - (long)phase;   /* 行 k 在簇内偏移        */
        if (ro < 0) continue;
        if (ro + (long)cur_bmp.width * 3 > (long)clus_size) break;
        for (u32 x = 0; x + 1 < cur_bmp.width; x += 2) {   /* 隔列采样提速 */
            u8 *q = cl + ro + 3 * x;
            total += abs((int)q[0]-(int)q[3]) + abs((int)q[1]-(int)q[4])
                   + abs((int)q[2]-(int)q[5]);
            cnt++;
        }
    }
    return cnt ? total / (3.0 * cnt) : 1e9;
}

/* 簇边界处的像素连续性：比较"紧邻字节偏移 b 两边的两个真实像素"。
 * b 是文件内部字节偏移（第 pos 个簇的起点 = pos * clus_size）。
 * 真实后继簇的这两个像素是图片中相邻的像素（横向或纵向），色差小；
 * 错误簇的像素来自别的图，色差大。注意 BMP 每行末尾有填充字节，
 * 必须按行结构定位像素，不能把数据当作无填充的连续像素流。           */
static double boundary_diff(u32 u, u32 v, u32 b) {
    u32 rb = (cur_bmp.width * 3 + 3) & ~3u;
    u32 delta = b - cur_bmp.data_off;
    /* 像素按行优先编号 j：其起始字节 = data_off + (j/W)*rb + 3*(j%W)   */
    u32 jA = (delta / rb) * cur_bmp.width + (delta % rb) / 3;
    u32 jArow = jA / cur_bmp.width, jAcol = jA % cur_bmp.width;
    u32 startA = cur_bmp.data_off + jArow * rb + 3 * jAcol;
    if (startA >= b) {   /* 越过了 b，回退一个像素                       */
        jA--; jArow = jA / cur_bmp.width; jAcol = jA % cur_bmp.width;
        startA = cur_bmp.data_off + jArow * rb + 3 * jAcol;
    }
    u32 jB = jA + 1;
    u32 jBrow = jB / cur_bmp.width, jBcol = jB % cur_bmp.width;
    u32 startB = cur_bmp.data_off + jBrow * rb + 3 * jBcol;
    if (startB + 3 > b + clus_size) return 1e9;   /* 越界保护            */

    /* 用"u 的尾部 + v 的头部"拼接缓冲，取出两个像素的 RGB。
     * 缓冲需覆盖 [b-12, b+12)：jA 的像素起始在 [b-3, b)，jB 起始在
     * [b, b+6)（行末跳下一行最多加 6 字节），加 2 字节尾巴取 3 字节像素。 */
    u8 buf[24];
    int su = (int)b - 12;
    u8 *pu = clus_ptr(u), *pv = clus_ptr(v);
    for (int i = 0; i < 24; i++) {
        u32 p = (u32)(su + i);
        buf[i] = (p < b) ? pu[p - (b - clus_size)] : pv[p - b];
    }
    u8 ca[3], cb[3];
    for (int k = 0; k < 3; k++) ca[k] = buf[(int)(startA + k) - su];
    for (int k = 0; k < 3; k++) cb[k] = buf[(int)(startB + k) - su];
    int dif = abs((int)ca[0]-(int)cb[0]) + abs((int)ca[1]-(int)cb[1])
            + abs((int)ca[2]-(int)cb[2]);
    return dif / 3.0;
}

/* ------------------------------------------------------------------ */
/* 图片数据恢复                                                        */
/* ------------------------------------------------------------------ */

/* 贪心链重构：从起始簇开始，逐步确定后续簇，直到凑齐 need 个簇。
 *   1) 物理相邻簇（cur+1）通常是本文件的延续：若边界平滑就取它；
 *      若边界陡峭但该簇按本图行结构看内部平滑，则视为图片里的"硬边"，
 *      同样取它。
 *   2) 否则说明当前簇到了"运行段"尽头（文件在磁盘上是分段的），
 *      需要跳到别的空闲簇：在所有未占用的簇里，找边界连续且行对齐
 *      最好的那个。
 * 返回恢复出的簇链长度；链本身写入 out[]。
 *
 * 注意：镜像里文件是"并发写入"导致大量分段的（参考镜像里只有约一半
 * 文件在磁盘上连续存放），因此单靠本函数并不能恢复全部文件——分段
 * 文件的跳转目标只能靠内容连续性猜测，存在一定的误判率。所以调用方
 * 会把"纯连续读"的结果与本函数结果比较，取更可信的一个。            */
static int greedy_chain(u32 start, u32 need, u32 *out, u8 *taken) {
    out[0] = start;
    taken[start] = 1;
    u32 cur = start;
    for (u32 pos = 1; pos < need; pos++) {
        u32 byte_pos = pos * clus_size;
        u32 rb = (cur_bmp.width * 3 + 3) & ~3u;
        u32 phase = (byte_pos - cur_bmp.data_off) % rb;
        u32 best = 0;
        /* 1) 物理相邻簇 */
        if (cur + 1 <= nclusters + 1 && !taken[cur + 1]) {
            double d = boundary_diff(cur, cur + 1, byte_pos);
            if (d < 25.0) best = cur + 1;                     /* 平滑    */
            else if (d < 150.0 && row_internal_diff(cur + 1, phase) < 45.0)
                best = cur + 1;                               /* 硬边    */
        }
        /* 2) 跳转：在所有未占用且非空闲的簇里找最像后继的 */
        if (best == 0) {
            double bi = 1e18;
            for (u32 v = 2; v <= nclusters + 1; v++) {
                if (taken[v] || is_header[v] || is_dir[v] || is_free[v])
                    continue;
                double d = boundary_diff(cur, v, byte_pos);
                if (d < 40.0) {
                    double ri = row_internal_diff(v, phase);
                    if (ri < bi) { bi = ri; best = v; }
                }
            }
            if (best == 0) {             /* 实在找不到后继，放弃        */
                for (u32 k = 1; k < pos; k++) taken[out[k]] = 0;
                taken[start] = 0;
                return (int)pos;
            }
        }
        out[pos] = best;
        taken[best] = 1;
        cur = best;
    }
    return (int)need;
}

/* 整链可信度：把所有簇按目标行结构重新切行，求平均行内色差。
 * 值越小说明这一串簇越像一张行结构一致的图片（很可能是正确恢复）。   */
static double chain_score(u32 *chain, int n) {
    u32 rb = (cur_bmp.width * 3 + 3) & ~3u;
    double s = 0;
    for (int i = 0; i < n; i++) {
        u32 phase = ((u32)(i + 1) * clus_size - cur_bmp.data_off) % rb;
        s += row_internal_diff(chain[i], phase);
    }
    return s / n;
}

/* 恢复单个文件：把内容写进 tmp 文件，用 sha1sum 算校验和并打印         */
static void emit_file(const struct fileent *f) {
    u32 need = (f->size + clus_size - 1) / clus_size;
    if (need == 0 || need > 4096) return;

    /* 方案 A：贪心链重构（处理分段文件）                               */
    u8 *taken = calloc(nclusters + 2, 1);
    u32 chain[4096];
    int n = greedy_chain(f->start, need, chain, taken);

    /* 方案 B：纯连续读（文件在磁盘上连续时的最简单恢复）                */
    u32 contig[4096];
    int contig_ok = 1;
    contig[0] = f->start;                 /* 起始簇就是本文件的头，不算"占位" */
    for (int i = 1; i < (int)need; i++) {
        u32 c = f->start + i;
        if (c > nclusters + 1 || is_header[c] || is_dir[c] || is_free[c]) {
            contig_ok = 0; break;         /* 连续路径上撞到别的文件头/目录/空闲 */
        }
        contig[i] = c;
    }

    /* 二选一：连续链可信就信连续（强先验）；连续明显不对时看贪心       */
    u32 *chosen = NULL;
    if (contig_ok) {
        double sc_c = chain_score(contig, (int)need);
        if (sc_c < 200.0) chosen = contig;         /* 连续链可信就信连续 */
        else if (n == (int)need) {
            double sc_g = chain_score(chain, (int)need);
            if (sc_g < 200.0 && sc_g < sc_c) chosen = chain;   /* 贪心明显更好 */
            else chosen = contig;
        } else chosen = contig;
    } else if (n == (int)need) {
        chosen = chain;
    }
    free(taken);
    if (!chosen) return;

    /* 沿所选链把文件字节拼出来 */
    u8 *data = malloc(f->size);
    if (!data) return;
    size_t got = 0;
    for (int i = 0; i < (int)need && got < f->size; i++) {
        size_t take = f->size - got;
        if (take > clus_size) take = clus_size;
        memcpy(data + got, clus_ptr(chosen[i]), take);
        got += take;
    }

    /* 写入临时文件并计算 sha1 */
    char tmpl[] = "/tmp/fsrecov.XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { free(data); return; }
    ssize_t w = write(fd, data, f->size);
    close(fd);
    free(data);
    if (w != (ssize_t)f->size) { unlink(tmpl); return; }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "sha1sum %s", tmpl);
    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(tmpl); return; }
    char hash[64];
    if (fscanf(fp, "%63s", hash) == 1)
        printf("%s  %s\n", hash, f->name);
    pclose(fp);
    unlink(tmpl);
}

/* ------------------------------------------------------------------ */
/* 入口                                                               */
/* ------------------------------------------------------------------ */
void *map_disk(const char *fname);

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s fs-image\n", argv[0]);
        exit(1);
    }
    setbuf(stdout, NULL);
    assert(sizeof(struct fat32hdr) == 512);

    struct fat32hdr *hdr = map_disk(argv[1]);
    disk = (u8 *)hdr;

    /* 从引导扇区提取几何信息 */
    bps     = hdr->BPB_BytsPerSec;
    spc     = hdr->BPB_SecPerClus;
    rsvd    = hdr->BPB_RsvdSecCnt;
    numfats = hdr->BPB_NumFATs;
    fatsz   = hdr->BPB_FATSz32;
    fat_off  = rsvd * bps;
    data_off = (rsvd + numfats * fatsz) * bps;
    clus_size = spc * bps;
    nclusters = (hdr->BPB_TotSec32 * bps - data_off) / clus_size;

    /* 1. 扫描数据区，给每个簇打分类标记 */
    is_header = calloc(nclusters + 2, 1);
    is_dir    = calloc(nclusters + 2, 1);
    is_free   = calloc(nclusters + 2, 1);
    for (u32 c = 2; c <= nclusters + 1; c++) {
        u8 *p = clus_ptr(c);
        struct bmpinfo b;
        if (bmp_parse(c, &b)) is_header[c] = 1;
        else if (dir_cluster_score(c)) is_dir[c] = 1;
        else if (p[0] == 0 && memcmp(p, p + 1, clus_size - 1) == 0)
            is_free[c] = 1;
    }

    /* 2. 从目录簇里解析出所有文件项 */
    for (u32 c = 2; c <= nclusters + 1; c++)
        if (is_dir[c]) scan_dir_cluster(c);

    /* 3. 逐文件恢复并输出 */
    for (int i = 0; i < nfiles; i++) {
        struct bmpinfo b;
        if (!bmp_parse(files[i].start, &b)) continue;
        cur_bmp = b;
        emit_file(&files[i]);
    }

    free(is_header); free(is_dir); free(is_free);
    munmap(hdr, hdr->BPB_TotSec32 * hdr->BPB_BytsPerSec);
    return 0;
}

void *map_disk(const char *fname) {
    int fd = open(fname, O_RDONLY);
    if (fd < 0) { perror(fname); goto release; }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size == -1) { perror(fname); goto release; }
    struct fat32hdr *hdr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (hdr == (void *)-1) goto release;
    if (hdr->Signature_word != 0xaa55 ||
            hdr->BPB_TotSec32 * hdr->BPB_BytsPerSec != size) {
        fprintf(stderr, "%s: Not a FAT file image\n", fname);
        munmap(hdr, size);
        goto release;
    }
    close(fd);
    return hdr;
release:
    if (fd >= 0) close(fd);
    exit(1);
}
