# M9: Key-value Database (libkvdb) 实验报告

## 1. 实验概述

本实验要求实现一个具备**崩溃一致性 (crash consistency)** 的嵌入式 Key-Value 数据库库 `libkvdb`，对外提供四个接口：

```c
struct kvdb_t;  // 见 kvdb.h（本实现保持其原样，未改动）

int kvdb_open(struct kvdb_t *db, const char *path);   // 打开/创建数据库文件
int kvdb_put(struct kvdb_t *db, const char *key, const char *value); // 写入（覆盖）
int kvdb_get(struct kvdb_t *db, const char *key, char *buf, size_t length); // 读取
int kvdb_close(struct kvdb_t *db);                    // 关闭、释放资源
```

核心要求有两条：

1. **进程安全 + 线程安全**：允许多个进程、多个线程同时打开并并发访问**同一个**数据库文件；一个线程/进程 `put` 的 key 必须能被另一个线程/进程 `get` 到，所有操作满足可序列化（serializable）。
2. **崩溃一致性**：在任意时刻崩溃（kill -9、断电等）后，数据库必须恢复到一致状态——不损坏、不出现"部分写入"、已提交（返回成功）的操作不丢失。

正确性优先，性能不是本实验重点（文档明示"使用一把大锁即可通过测试"）。

## 2. 实验环境

| 项目 | 值 |
|---|---|
| 系统 | WSL2（Linux 6.6.87.2-microsoft-standard-WSL2）|
| 编译器 | gcc 13（`-O2 -std=gnu2x -Wall`）|
| 测试 | 框架自带 `tests/test_main.c`（testkit）+ 自定义并发/崩溃压力测试 |
| 交付 | `kvdb.c`（约 540 行，含详细中文注释）；`kvdb.h` 未改动 |

## 3. 总体设计

### 3.1 一句话概括

用 **"单文件快照 + 写时复制 + fsync + 原子 rename"** 实现崩溃一致性；用 **"独立锁文件上的 flock 大锁"** 实现跨进程、跨线程的互斥。

```
                 ┌─────────────────────────────────────────────┐
   任意 kvdb_*  → │ 加锁: open(<path>.lock) + flock(LOCK_EX)    │  ← 一把大锁
                 │ 临界区: read+parse(旧库) ── 修改 ── serialize │
                 │        ── 写 <path>.tmp ── fsync             │
                 │        ── rename(tmp→path) ── fsync(父目录)  │
                 │ 解锁: flock(LOCK_UN) + close                 │
                 └─────────────────────────────────────────────┘
```

### 3.2 数据格式：一份"完整快照"

磁盘文件自始至终保存**全量** key-value 数据（无独立日志区）：

```
[8 字节魔数 "KVDBSNAP"] [(u32 keylen)(key)(u32 vallen)(value)] × N
```

- **0 字节文件 = 空数据库**（`kvdb_open` 在路径不存在时创建的就是空文件）；
- 非空文件首 8 字节必须等于魔数，否则视为损坏；
- u32 长度用 `memcpy` 打包/解包（避免对齐问题），宿主机端序；
- key/value 都是 C 字符串（不含内嵌 `'\0'`），故可按 `strlen` 定长存取。

### 3.3 崩溃一致性：写时复制 + 原子 rename

一次 `kvdb_put` 的提交路径是：

```
把旧记录全部读入内存 → 覆盖/追加一条 → 序列化成一份完整新快照字节
   → 写入 <path>.tmp
   → fsync(<path>.tmp)          ← 新文件数据先落盘
   → rename(<path>.tmp, path)   ← 原子替换目录项
   → fsync(父目录)              ← 让"名字→新 inode"的映射也落盘
```

正确性论证：

- `rename` 在 POSIX 上是**原子**的，目录项要么指向完整旧 inode、要么指向完整新 inode，**不存在指向半截内容的中间态**；
- 新文件在 rename 前已 `fsync`，故崩溃只可能有两种结局：
  - rename 之前崩溃 → `path` 仍是完整旧库，本次 put **未提交**，可安全丢弃（残留的半截 `<path>.tmp` 在下次 open/put 时被清理）；
  - rename 之后崩溃 → `path` 是完整新库，put **已提交**。
- 只有 `fsync + rename` 全部完成后 `kvdb_put` 才返回 0，所以**返回成功的写操作必然持久化**；
- 对父目录的 `fsync` 保证极端情况（机器重启）下 rename 不会"倒退"，进一步兜底。

> 为什么不能对"正在被 rename 替换的文件"直接读写？因为进程先前打开的 fd 指向**旧 inode**，rename 后路径指向新 inode——直接沿用旧 fd 会永远读到过期数据。因此本实现**每次操作都临时 `open` 数据库文件**，永远读到最新 inode。

### 3.4 进程/线程互斥：独立锁文件上的 flock "一把大锁"

由于上面会用 rename 替换数据库文件的 inode，**不能用数据库文件本身 flock**（不同进程可能锁到新旧不同 inode 上）。解决办法是引入一个**永不 rename、永不被删除**的旁路锁文件 `<path>.lock`：

- 每个操作（open/put/get）临时 `open` 它 → `flock(fd, LOCK_EX)` → 临界区 → `flock(LOCK_UN)` + `close`；
- flock 的锁挂在**打开文件描述 (open file description)** 上：两个线程哪怕在**同一进程、共用同一个 `struct kvdb_t`**，只要各自 `open` 一次锁文件就会得到不同的描述 → flock 会真正互相阻塞（已在开发中实测验证）；
- 跨进程同理天然互斥；
- flock 由内核管理，进程崩溃/退出时**自动释放**，无"死锁残留"；
- 锁文件从不改名/删除，"锁的标的"始终稳定。

所有 put/get 都在这把排他锁的临界区内完成"读 → 改 → 写"，因此任意两个并发操作都存在全局顺序（= 加锁顺序），满足**可序列化**：

- **最近写可见性**：get 总能看到最近一次已提交 put 的结果；未写过则返回 -1；
- **顺序一致性**：同一线程先后两次操作按 happens-before 自然有序；
- **并发一致性**：对同一 key 的并发 put 被串行化，最后加锁者胜出，之后所有 get 看到同一值。

**因为只有 flock 一把锁，`struct kvdb_t` 无需新增任何字段**（如 pthread 互斥量），`kvdb.h` 保持框架原样、只用了它原有的 `path` / `fd` 两个字段——这也是本实现能做到"只改写 `kvdb.c`"的关键。

### 3.5 `kvdb_get` 的返回约定（与框架示例一致）

按文档语义："最多拷贝 `length-1` 字节并补结尾 `'\0'`，返回**实际拷贝的字节数**（不含 `'\0'`），未找到返回 -1"。

框架自带的示例测试用 `kvdb_get(&db, "key", NULL, 0) == 0` 表示"该 key 存在"，因此当 `length == 0` 时拷贝数为 0、返回 0（什么都不写）。当 `length > 0` 时拷贝 `min(vallen, length-1)` 字节并补 `'\0'`。

## 4. 关键实现点

| 函数 | 职责 |
|---|---|
| `lock_acquire` / `lock_release` | 打开 `<path>.lock` 加/放 flock 排他锁（"一把大锁"）|
| `snapshot_load` | 在锁内读入并解析当前快照 → 内存记录数组；空文件 = 空库 |
| `snapshot_build` | 把内存记录数组序列化成完整快照字节 |
| `commit_file` | **崩溃一致性核心**：写 tmp → fsync → rename → fsync 父目录 |
| `vec_push` / `vec_find` | 记录动态数组的追加与按键查找 |

`kvdb_put`：加锁 → 读旧快照 → 找到 key 覆盖 value，否则追加 → 序列化 → `commit_file` → 解锁。`kvdb_get` 同理，只是只读不写。

## 5. 测试与验证

### 5.1 框架自带测试（`tests/test_main.c`，testkit 运行）

```
TestKit
- [PASS] test_kvdb_open        // open/close 基本流程
- [PASS] test_kvdb_put_get     // put("key","value") 后 get 能读回
2/2 test cases passed.
```

### 5.2 语义测试

- 多个 key 的 put/get、重复 put（覆盖）、get 不存在的 key 返回 -1；
- 小缓冲区：`get(&db, "hello world", tiny, 4)` 返回 3 且 `tiny[3]=='\0'`（截断 + NUL 结尾）；
- `get(..., NULL, 0)` 返回 0（框架约定的"存在"判据）；
- **close 后重新 open，数据仍在**（持久化）。

### 5.3 多线程并发（共用同一个 `struct kvdb_t`）

6 个线程（4 写 + 2 读）共用**同一个** db 结构体，各写 400 个 key 后全部读回校验——通过。这验证了"同一进程内两个线程各自 `open` 锁文件 → flock 真正互斥"，无需 pthread 互斥量。

### 5.4 多进程并发（各自 open 同一个库文件）

6 个子进程并发写入互不相同的 key（共 900 个），父进程等待全部结束后打开库逐一校验全部存在——通过。这验证了"读→改→写"在锁内完成，**没有因读改写竞争而丢失任何已提交写入**。

### 5.5 崩溃一致性（随机 kill -9）

5 个子进程各自不断 `kvdb_put` 唯一 key；**父进程在随机时刻（最长等待 30ms 的随机间隔）逐个 SIGKILL 杀掉它们**。每个子进程在 put **返回成功之后**才把 key 记入自己的日志，因此日志中的 key 全部是"已确认提交"的操作。收尾校验：

- 重新 `kvdb_open` 成功，数据库可完整解析、不损坏；
- 打开后 `<path>.tmp` 残留被清理（`kvdb_open` 会删掉崩溃留下的半截临时文件）；
- **日志中每个已确认提交的 key 都存在且值正确**——已提交操作不丢失。

该测试重复多轮均通过。

### 5.6 内存安全

`basic` / `threads` 用例在 `-fsanitize=address,undefined` 下运行无报错、无泄漏。

## 6. 设计取舍

| 取舍 | 说明 |
|---|---|
| 每写必全量重写 + 多次 fsync | 每次 put 都把整个库读入、改写、重写一遍，代价是 O(n)。文档明确"性能不是重点、正确性优先、一把大锁即可"，故可接受；换来的是**极致简单的崩溃一致性论证** |
| 单文件快照，无 WAL/日志区 | 与文档介绍的 WAL 殊途同归，都是"日志/数据先落盘再原子切换"。本方案用 rename 的原子性替代"日志重放"，不需要在 open 时做恢复遍历 |
| 独立 `.lock` 文件而非锁库文件本身 | 因为库文件每次 put 都被 rename 换成新 inode，锁在库文件上会失效；锁文件永不改名，锁的标的才稳定 |
| 每次操作临时 open 而非持有 fd | 同理，避免持有指向旧 inode 的 fd 读到过期数据 |
| 不用 pthread 互斥量 | flock 在"同一进程两个 fd"间也会互斥（实测验证），一把锁覆盖所有场景，且无需改动 `kvdb.h` |
| 只 fsync 文件不 fsync 目录的替代方案 | 本实现对父目录也做一次尽力 fsync，覆盖"机器真正重启时 rename 倒退"的更极端情况 |

## 7. 遇到的问题与解决

| # | 问题 | 原因 | 解决 |
|---|---|---|---|
| 1 | 框架示例 `test_kvdb_put_get` FAIL：put 成功后 get 返回 -1 | 序列化格式为 `[klen][key][vlen][value]`，解析器却按 `[klen][vlen][key][value]` 读，字段错位 | 统一解析器与写出顺序为 `[klen][key][vlen][value]` |
| 2 | `rename` 隐式声明警告 | 漏包含声明 `rename` 的头 | 补 `#include <stdio.h>` |
| 3 | 崩溃测试一度"秒过"无意义 | 父进程杀掉子进程太快，几乎没有产生已提交操作 | 给子进程 200ms 起步时间、把 kill 间隔调到随机 30ms 内，制造真实的"提交中途被杀"窗口 |
| 4 | 初版想过锁数据库文件本身 | rename 使 inode 变化，锁会落在不同 inode 上导致互斥失效 | 改用永不改名的独立 `<path>.lock` 文件做 flock |

## 8. 总结

本实验把 OS 课程反复强调的两件事落到了实处：

1. **持久化不等于 write()**：`write` 可能只写一半；要保证崩溃一致性，必须显式 `fsync`，并用 `rename` 的**原子性**把"新版本"一次性切换上去。文件系统上的"读改写快照 + 原子替换"与共享内存里的 CAS / 双缓冲思路同构。
2. **互斥锁的进程级扩展**：线程可以用 futex，跨进程则需要内核提供的能力——flock 正是把"文件 + 锁"绑定在一起的进程间互斥原语，且随进程消亡自动释放，天然适合"随时可能被 kill"的崩溃场景。

最终交付的 `kvdb.c`（约 540 行、详细中文注释）在**不改动 `kvdb.h`** 的前提下，用"一把 flock 大锁 + 单文件快照的原子替换"实现了文档要求的两类安全：多进程多线程并发下的可序列化访问，以及任意时刻崩溃后数据库的一致、已提交不丢失。
