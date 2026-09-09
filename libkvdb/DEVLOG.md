# M9: Key-value Database (libkvdb) 开发日志

本文件按时间顺序记录完成 M9 实验的全过程：每一步做了什么、遇到什么问题、如何分析并解决。

---

## 2026-09-05

### Step 1：理解实验要求

- 抓取并精读实验文档（`jyywiki.cn/OS/2026/labs/M9`）。核心要求：
  1. 实现四个接口 `kvdb_open / kvdb_put / kvdb_get / kvdb_close`，数据库内容持久化在一个 regular file 里；
  2. **进程安全 + 线程安全**：多进程、多线程可同时打开并并发访问同一数据库，满足可序列化（最近写可见、happens-before 保序、并发写收敛到同一结果）；
  3. **崩溃一致性**：任意时刻崩溃后数据库一致、不损坏、已提交（返回成功）的写不丢失；文档明确"性能不是重点，一把大锁即可通过测试"，但需要严格的互斥 + 崩溃一致性；
  4. 文档提示的两种实现方向：互斥可用 **flock**；崩溃一致性可用 **write-ahead logging**（也可理解为"日志先落盘、再原子切换"）。
- 阅读框架代码：
  - `kvdb.h`：`struct kvdb_t { const char *path; int fd; };`，注释写着 "Modify this struct."；
  - `kvdb.c`：四个接口全部 `return -1` 占位；
  - `Makefile`：`LIB_SRCS = ./kvdb.c`、`TEST_SRCS = tests/*.c + ../testkit/testkit.c`，构建 `libkvdb.so` + `libkvdb_test`；
  - `tests/test_main.c`：两个 SystemTest，其中 `test_kvdb_put_get` 用 `kvdb_get(&db, "key", NULL, 0) == 0` 判定"key 存在"。
- 决定**不改动 `kvdb.h`**（用户要求只改写 `kvdb.c`）：若设计得当，结构体只需要保存 path 字符串，`fd` 字段可以不用，从而保持头文件原样。

### Step 2：先想清楚"两件难事"，再写代码

**难事一：锁。** 线程互斥用 futex（共享内存地址）；跨进程默认地址空间隔离。Linux 的 flock 把锁挂在**文件**上，天然跨进程。但马上意识到一个陷阱：如果每次 put 都用 `rename` 把数据库文件换成**新 inode**，那么一个进程先前 `open` 得到的 fd 指向旧 inode、另一个进程新 `open` 指向新 inode——两把 flock 锁会锁到**不同 inode** 上，互斥失效。

**难事二：崩溃一致性。** `write` 可能只写一半；文档建议 WAL（先写日志、fsync，再更新数据）。但我打算用更简单的等价方案：**写时复制 + 原子 rename**——每次 put 把全库写成一个完整新快照到 `.tmp`，`fsync` 后再 `rename` 覆盖原文件。rename 原子，故库文件永远指向"完整旧版或完整新版"，天然无撕裂；`fsync` 保证 rename 前数据已落盘。

**关键决策（来自上面两个分析）：**
- 数据库文件每次都被 rename 换 inode → **不能锁库文件本身**；
- 于是引入**永不 rename、永不删除**的旁路锁文件 `<path>.lock`，所有操作都锁它；
- 每次操作**临时 `open` 锁文件 + 库文件**，用完即关 → 永远锁到同一个 `.lock` inode、永远读到最新库 inode，也不存在"同一 fd 两次 flock 自锁放行"的问题。

### Step 3：实测验证 flock 语义（关键假设不能拍脑袋）

写了个 10 行小程序验证三个猜想：

```
[1] 同一 fd 上重复 flock(LOCK_EX)  → 立即成功（不会自己锁死自己）
[2] 同一进程两个不同 fd 分别 flock → 互相阻塞（LOCK_NB 直接 EWOULDBLOCK）
[3] 解锁后再 flock 第二个 fd       → 成功
```

结论：**两个 fd（两个 open file description）即便在同一进程内也真正互斥**。这证明了"即使两个线程共用同一个 `struct kvdb_t`，只要每次操作各自 `open` 一次锁文件，flock 就能互斥"——所以**不需要 pthread 互斥量，也不需要给 `kvdb.h` 加字段**。设计定稿。

### Step 4：编写 kvdb.c

文件约 540 行，含详细中文注释。结构：

```
锁   ：lock_acquire/lock_release        → open(<path>.lock)+flock(LOCK_EX/UN)
装载 ：snapshot_load                    → 读库文件、解析记录数组（0 字节=空库）
序列化：snapshot_build                  → 记录数组 → 完整快照字节
提交 ：commit_file                      → 写.tmp → fsync → rename → fsync 父目录
接口 ：kvdb_open/put/get/close
```

数据格式：`[8字节魔数 "KVDBSNAP"][(u32 keylen)(key)(u32 vallen)(value)] × N`。

### Step 5：问题 1 —— 框架示例 `test_kvdb_put_get` FAIL

首次编译运行：

```
- [PASS] test_kvdb_open
- [FAIL] test_kvdb_put_get    (kvdb_get(&db,"key",NULL,0) == 0 断言失败)
```

分析：put 显然成功了（`.tmp` 的库文件有 24 字节：8 魔数 + 3+4+5…），但 get 读不到。用十六进制看文件、对照写出顺序，发现**序列化与解析的字段顺序不一致**：

- 写端（`snapshot_build`）：`[klen][key][vlen][value]`；
- 读端（`snapshot_load` 初版）：误写成 `[klen][vlen][key][value]`，把 value 长度当成了 key 内容。

解决：统一解析顺序为 `[klen][key][vlen][value]`，按字节偏移逐步推进并逐段做越界检查。修复后 2/2 通过。

### Step 6：问题 2 —— `rename` 隐式声明警告

编译报 `implicit declaration of function 'rename'`。`rename` 声明在 `<stdio.h>`，漏包含。补上后 `-Wall -Wextra` 编译零告警（仅框架 testkit 自身有历史告警，与本次代码无关）。

### Step 7：语义自测（basic）

写了一个含多分支的测试程序验证语义：

- 多个 key put/get、重复 put 覆盖、不存在的 key 返回 -1；
- 小缓冲区截断：`get(..., tiny, 4)` 返回 3 且 `tiny[3]=='\0'`；
- `get(..., NULL, 0)` 返回 0（框架判"存在"约定）；
- **close 后重新 open 数据仍在**（持久化）。

全部通过。

### Step 8：多线程并发（threads）——共用同一个 struct

担心点：两个线程**共用同一个 `struct kvdb_t`**（共享同一个对象、没有各自的 pthread 互斥量）时是否安全。测试 6 线程（4 写 + 2 读）共用同一个结构体、各自写入互不相同的 key，结束后全部读回校验。通过 → 印证了 Step 3 的 flock 结论。**这也让 `kvdb.h` 完全可以保持原样**。

### Step 9：多进程并发（proc）——验证没有"读改写丢更新"

6 个子进程各自 open 同一个库、并发写 900 个互不相同的 key；父进程等全部结束后重新 open，逐一校验 900 个 key 全部存在。通过（约 6.7s，因为每次 put 全量重写 + fsync）。这验证了"读→改→写"全程在排他锁内，**不会出现两个进程基于同一旧快照改写、互相覆盖丢数据**的情况。

### Step 10：崩溃一致性（crash）——随机 kill -9

设计思路：

- 5 个子进程不断 put 唯一 key，**在 put 返回成功之后**才把 key 追加进自己的日志 → 日志里只有"已确认提交"的操作；
- 父进程随机挑存活子进程，用 `usleep(随机 0~30ms)` 制造任意时刻（含"提交中途"）后 `SIGKILL`，直到全部杀完；
- 收尾：重新 `kvdb_open` → 必须能打开、能完整解析（不损坏）；`.tmp` 残留必须被清理；**日志中每个已提交 key 都必须存在且值正确**。

**问题 3：初版测试"秒过"无意义。** 父进程 kill 得太快（间隔 0~4ms），子进程几乎还没来得及产生任何已提交操作就全被杀，校验集为空。解决：先给子进程 **200ms 起步时间**积累真实写入，再把 kill 间隔放宽到随机 30ms。改进后库文件有真实内容（1.5KB 量级），重复 4 轮全部通过。

### Step 11：内存安全 + 残留清理验证

- `basic` / `threads` 在 `-fsanitize=address,undefined` 下运行：无报错、无泄漏；
- 手工制造一个垃圾 `.tmp` 文件再 `kvdb_open`：确认被删除（残留清理的确定性验证）。

### Step 12：撰写文档

- 编写 `REPORT.md`（实验报告）与本 `DEVLOG.md`（开发日志）。

---

## 总结

最终交付的 `kvdb.c`（约 540 行、详细中文注释）实现了：

1. **崩溃一致性**：单文件快照，每次 put = "读旧全量 → 改 → 写 `.tmp` → fsync → rename → fsync 父目录"，利用 rename 的原子性保证库文件永远是完整快照、已提交写必持久化；
2. **进程/线程安全**：永不改名的 `<path>.lock` 旁路锁文件 + 每次操作临时 open 后 flock 的"一把大锁"，覆盖同一进程多线程（甚至共用同一结构体）与多进程场景；
3. **接口语义**：`get` 按"实际拷贝字节数"返回（含 `length==0 → 0` 的框架约定），未找到返回 -1；
4. **不改动 `kvdb.h`**：结构体只复用原有 `path` 字段，`fd` 不用。

验证结果：框架自带 2 个用例通过；语义 / 多线程共用结构体 / 多进程 900 key 并发 / 随机 kill -9 崩溃恢复 / ASan 多轮测试全部通过。
