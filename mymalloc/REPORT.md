# M5 并行内存分配器（mymalloc）实验报告

## 实验概述

M5 是南京大学蒋炎岩老师操作系统（OS/2026）课程的实验，要求实现一个**多处理器安全的内存分配器** `mymalloc` / `myfree`。实验环境为 freestanding（无 libc、无 pthreads），只能使用框架提供的自旋锁实现互斥，并且代码可作为 libc 实现的一部分或操作系统内核使用。

本实验的重点不在"巧妙的算法/数据结构"，而在于 **system 式的工程方法**：把分配路径拆成 fast/slow path，使 fast path 在线程本地完成、不争抢任何锁，从而让多个处理器上的 malloc/free 真正并行。

## 实验要求（来自实验文档）

| 要求 | 说明 |
|------|------|
| 原子性 | 多个处理器同时分配/释放必须正确完成；一个处理器上分配的内存可能被另一个处理器释放 |
| 无重叠 | 返回的内存块互不重叠 |
| 对齐 | 返回地址低 3 位必须为 0（8 字节对齐） |
| 无泄漏 | 释放的内存必须可复用，不能成为"死内存" |
| 错误处理 | 无法满足分配时返回 NULL |
| 只许自旋锁 | 不得调用 pthreads 等库 |
| 静态内存 < 1 MiB | 管理堆区的数据结构必须在堆区中分配，而非静态区 |
| 碎片控制 | 实际使用内存不得超过申请内存的 4 倍 |
| 性能 | 不同处理器上的分配应能并行；全局锁 + 链表遍历会导致 hard test failure |

提供的接口：

```c
void *mymalloc(size_t size);   // 内存分配
void  myfree(void *ptr);       // 内存释放
void *vmalloc(void *addr, size_t length);  // 框架提供，mmap 的封装
void  vmfree(void *addr, size_t length);   // 框架提供，munmap 的封装
```

## 设计思路

现代内存分配器（tcmalloc / jemalloc）的核心思想：**区分 fast path 与 slow path**。

- **fast path**：在线程本地完成。线程把释放的槽位放进自己的缓存，分配时优先从自己的缓存弹出。全程不触碰任何共享状态、不加锁 → 多个处理器可以真正并行。
- **slow path**：只有线程本地缓存为空 / 溢出时才触发，去访问受锁保护的全局 slab 链表。

内存来源：`vmalloc()` 一次申请 1 MiB 的"大区间"，切成 4 KiB 的页；每页作为一个 **slab**（板），切成若干大小相同的槽位（对象）。

### 大小分级（size class）

请求大小向上取整到所属类，同类对象大小完全相同：

- 小对象类：`16, 32, 48, ..., 512`（步长 16，共 32 类）
- 中对象类：`1024, 1536, 2048, 2560, 3072`（共 5 类）
- 大于 3072 字节：走 vmalloc 大对象路径

### 数据结构

```
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│  partial[0] │───▶│  slab(页)   │───▶│  slab(页)   │  每个大小类一条
└─────────────┘    └─────────────┘    └─────────────┘   "部分使用"链表
                                             │
                                             ▼
  slab 结构：magic / next / size_class / capacity / used / cached / on_partial / free

┌─────────────┐
│  empty_list │  完全空闲、可复用的页（跨类复用）
└─────────────┘

┌─────────────┐
│  vm_cur     │  当前 vmalloc 大区间，切页用
└─────────────┘

┌─────────────┐      ┌──────────────────────────────┐
│  tcb_reg    │─────▶│ tcb：线程本地缓存              │
│  (哈希表)    │      │   free[CLASS_COUNT] 每类空闲链表│
└─────────────┘      │   count[CLASS_COUNT] 链表长度  │
                     └──────────────────────────────┘
```

- **slab**：占一页（4 KiB），页开头存元信息，其余空间切槽位。用地址页对齐特性，从任意槽位指针可反推所属 slab 页。
- **partial[ci]**：第 ci 类"还有空闲槽位"的 slab 链表，用 per-class 自旋锁保护。
- **empty_list**：完全空闲的页，供任何类复用。
- **tcb**：每个线程一份线程缓存，用 `gettid()`（Linux syscall）找到，存入按 `tid % NREG` 哈希的表。
- **大对象**：`vmalloc` 分配"页头 + 数据页"，页头存长度，数据页开头存识别魔数，数据从数据页 + 8 返回。

### 并发一致性：cached 计数器

实验中最关键、也最隐蔽的正确性问题，是"**页面过早复用**"：

> 一个 slab 只有在其 `used == 0` 时才会被回收进 empty_list 等待复用。但如果只用 `used` 判空，一旦 `used` 计数在"页面被复用 / 旧化身"之间发生欠计数，就会出现**页面在仍有槽位存活于某线程缓存时被回收**：`slab_init` 会改写仍被用户持有的槽位，并把活槽位重新链入空闲链表，导致同一地址被重复分配、缓存链表成环、甚至死循环。

为此，本实现给每个 slab 增加一个**原子 `cached` 计数器**，记录"在该 slab 中、但位于某线程缓存里的槽位数"：

- fast path 释放：`__atomic_fetch_add(&s->cached, 1)`
- fast path 分配：`__atomic_fetch_sub(&s->cached, 1)`
- flush 归还：`__atomic_fetch_sub(&s->cached, 1)`

**页面可复用的判据为 `used == 0 且 cached == 0`**：只有所有槽位都回到页内空闲链表、且没有任何线程缓存仍引用本页槽位时，才能安全复用。`cached` 由 fast path 原子维护，因此该判据是可靠的，彻底杜绝了"过早复用"这一类并发灾难。

## 实现细节

### mymalloc（分配）

```
mymalloc(size):
  ci = size_to_class(size)
  if ci < 0:                         # 大对象 → vmalloc
      ...
      return hdr + PAGE + 8
  t = get_tcb()                      # 找到本线程缓存
  if t->free[ci] 非空:                # fast path（无锁）
      p = pop(t->free[ci])
      __atomic_fetch_sub(slab(p)->cached, 1)
      return p
  lock(class_locks[ci])              # slow path
  if partial[ci] 非空:
      sl = pop(partial[ci] 头 slab 的 free)
      ...
      return sl
  unlock
  ns = slab_page_get()               # 新建 slab（复用 empty 页或切新页）
  slab_init(ns, ci)
  lock(class_locks[ci]); push ns 到 partial; pop 一个槽位; unlock
  return sl
```

### myfree（释放）

```
myfree(ptr):
  page = ptr & ~0xfff
  if *(uintptr_t*)page == SLAB_MAGIC:   # slab 槽位 → 进线程缓存
      s = (slab*)page; ci = s->size_class
      t = get_tcb()
      __atomic_fetch_add(&s->cached, 1)
      push ptr 到 t->free[ci]
      if t->count[ci] > slab_capacity(ci):
          thread_cache_flush(t, ci)      # 缓存过长，批量归还
  else:                                   # 大对象 → 整段归还
      hdr = page - PAGE
      vmfree(hdr, *(size_t*)(hdr+8))
```

### thread_cache_flush（缓存归还）

把某类缓存里的全部槽位归还给各自的 slab，并在 `used==0 且 cached==0` 时把页面移入 empty_list 供复用。

## 遇到的 Bug 与修复

实验过程中发现了两个隐蔽的 bug（详见 DEVLOG.md）：

1. **大对象路径下溢 8 字节**：数据页开头存放识别魔数（8 字节），但 `total` 只按 `align_up(size, PAGE) + PAGE` 计算，导致请求大小恰为 4096 整数倍时可用空间比请求少 8 字节，写满即越界溢出到 mmap 区域之外、破坏相邻内存。
   - **修复**：`data = align_up(size + 8, PAGE)`，保证 `data - 8 ≥ size`。

2. **页面过早复用导致并发双重分配**：`used` 计数在页面化身之间漂移，导致页面在仍有缓存槽位时被复用，`slab_init` 改写活槽位并重复分配同一地址，最终缓存链表成环、死循环。
   - **修复**：引入原子 `cached` 计数器，复用判据改为 `used == 0 && cached == 0`。

## 测试与验证

在非 freestanding 环境下使用 pthread 构造了并发压力测试（测试代码使用 pthread 只是为了让多个线程真正并行；分配器本身不依赖任何 libc / pthread 接口）：

| 测试 | 内容 | 结果 |
|------|------|------|
| 框架 demo（tests-trivial.c） | trivial / vmalloc / concurrent | 3/3 PASS |
| 基本正确性 | 各种大小的分配、8 字节对齐、malloc(0)、free(NULL) | PASS |
| 无重叠 | 50 万次分配/释放，逐个检查存活块互不重叠、canary 完整性 | PASS |
| 并发 | 4 / 16 线程并发分配/释放，canary 模式检测重叠与 use-after-free | PASS |
| 跨线程 | 一个线程分配、另一个线程释放，验证"迁移"正确性 | PASS |
| 复用 | 反复分配/释放同一大小，验证内存可复用、不无限增长 | PASS |
| 大对象 | 大小覆盖 3072 以下与 vmalloc 大对象路径，并发测试 | PASS |
| 性能 | 分配吞吐与多核扩展 | T=1: 4.9M，T=4: 11.8M ops/s（约 2.4x） |

freestanding 自检（`make check`）通过：`gcc -DFREESTANDING ... -ffreestanding -static -nostdlib` 编译链接无错误。

## 关于 demo 测试与 malloc_count

框架自带的 `tests-trivial.c` 中有一个 `concurrent` 测试依赖全局计数器 `malloc_count`（每个 malloc 原子递增，断言其等于 4N）。骨架注释明确说明"不需要这个计数器，可以移除"。**保留它会令每次分配都在一个被所有线程共享的原子计数器上竞争，恰好是本实验要求避免的扩展性反模式**，因此移除了它，并把该测试改写成不依赖计数器的并发冒烟测试。

另外，`mymalloc/Makefile` 的 `.shadow/oslab.mk` 会把 `SRCS` 覆盖为根目录 `*.c`，导致本地 `make` 无法链接 `tests/main.c`（缺少 `main`）。这是框架自带的坑，与本次实现无关；本地用 testkit 的方式手动编译测试即可（Online Judge 有自己的一套测试）。

## 总结

本次实验把 malloc/free 从"一把大锁保平安"升级为 **fast/slow path** 结构的线程本地缓存分配器：

- fast path 无锁、完全并行；
- slow path 使用 per-class 细粒度锁；
- 通过原子 `cached` 计数器保证页面复用的并发安全。

内存分配器是"机制与策略分离"的典型案例：分配接口简单，但覆盖极广的应用场景；正确的并发控制 + 合理的复用策略，远比华丽的算法更重要。
