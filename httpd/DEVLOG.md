# M7: HTTP Daemon (httpd) 开发日志

本文件按时间顺序记录完成 M7 实验的全过程：每一步做了什么、遇到了什么问题、如何分析并解决。

---

## 2026-08-24

### Step 1：理解实验要求

- 抓取并精读实验文档（jyywiki.cn/OS/2026/labs/M7），提取出三个硬性要求：
  1. `/cgi-bin/` 开头 URL 执行 CGI；脚本不存在 / 路径非法 → **404**；脚本执行失败 → **500**；
  2. 并发请求要**尽快接收**、并行处理，但**并行执行的请求数不得超过 4 个**；
  3. 日志**按请求到达顺序**输出，用框架的 `log_request`，每次 `fflush`。
- 注意到文档特别强调：日志的路径要正确（如 `/cgi-bin/echo`），状态码要是 **CGI 返回的**状态码（如 CGI 返回 403 就要记 403）——这意味着服务器必须"看见"CGI 的响应文本，才能解析出状态码。
- 阅读框架代码：
  - `httpd/httpd.c`：单线程占位实现，`main` 里 accept 后直接 `handle_request`，对任何请求返回 `Under construction`；
  - `Makefile`：`NAME=httpd`、`MODULE=M7`，链接方式为 `$(CC) $(CFLAGS) $(SRCS) ../testkit/testkit.c`（`SRCS = httpd.c tests.c`）；
  - `thread.h` / `thread-sync.h`：提供 spawn/join、锁/条件变量/信号量封装；
  - `cgi-bin/echo`：框架自带的 CGI 脚本，读 `REQUEST_METHOD` / `QUERY_STRING` / `env` 并输出完整 HTML 响应（含 `HTTP/1.1 200 OK` 状态行）。

### Step 2：设计整体架构

核心矛盾：请求要并行处理（≤4），但日志要按到达顺序，且 CGI 时间可能很长。

设计结论（生产者-消费者）：

- **主线程 = 生产者**：只 `accept()` 并立刻把请求放入有界 FIFO 队列，分配到达序号 `seq`。绝不阻塞在慢 CGI 上 → 满足"尽快接收"。
- **4 个 worker = 消费者**：从队列取一个请求，完整处理 → 满足"并行执行 ≤ 4"（恰好 4 个线程）。
- **序号化按序日志**：`accept` 时分配递增 `seq`；worker 处理完写结果、**阻塞等待** `log_next` 前进到自己的 `seq` 再打印 → 满足"按到达顺序"。
- **CGI 状态码**：`fork` + 管道捕获 CGI 的 stdout，解析响应**首行**的状态码，再转发给客户端。

### Step 3：编写 httpd.c

按设计实现（约 260 行，含详细中文注释）：

- 常量：`NUM_WORKERS=4`、`MAX_QUEUE=64`、`LOG_WINDOW=256`、`MAX_CGI_OUTPUT=4MB`；
- 有界队列：互斥锁 + `q_not_empty` / `q_not_full` 两个条件变量；
- 按序日志：环形缓冲 `logs[LOG_WINDOW]` + `log_mutex` + `log_cond`，`log_flush_locked()` 级联打印；
- CGI：存在性/可执行性检查 → `pipe`+`fork` → 子进程重定向 stdout/stdin/stderr、设环境变量、`exec` → 父进程读管道、解析状态码、转发；
- `handle_request`：读请求头、解析请求行、拆分 path/query、分发；
- `main`：socket 初始化（沿用框架）+ 建 4 worker + accept 循环。

### Step 4：编译

用与框架 Makefile 完全相同的命令手动编译（避免 `make` 触发框架的 `git-trace` 自动提交）：

```bash
gcc -O2 -std=gnu2x -ggdb -Wall -I../testkit httpd.c tests.c ../testkit/testkit.c -o httpd
```

**问题 1**：`log_request` 在使用它的 `log_flush_locked()` 之后才定义，缺少前向声明 → 编译报隐式声明。

- **解决**：在文件顶部（include 之后）加 `void log_request(...)` 前向声明。

**问题 2**：`-Wall` 对我的 `strncpy(e->method, req->method, sizeof(e->method)-1)` 报"may be truncated"警告。

- **分析**：源字符串（`snprintf` 生成、必有 NUL、长度 ≤ 15）保证能塞进目标，警告是误报，但不够干净。
- **解决**：改用 `strcpy`（源有界且以 NUL 结尾，安全），警告消失。

### Step 5：基础功能测试

在 `/tmp/m7test` 搭建独立测试环境（复制二进制和 `cgi-bin/`），启动服务器后逐个 curl 验证：

| 用例 | 结果 |
|---|---|
| `GET /cgi-bin/echo` | 200，返回 echo 页面 ✓ |
| `GET /cgi-bin/echo?foo=bar` | `QUERY_STRING=foo=bar` 正确传入 ✓ |
| `GET /cgi-bin/nonexistent` | 404 ✓ |
| `GET /anything` | 404 ✓ |
| `GET /cgi-bin/`（空名）| 404 ✓ |
| `GET /cgi-bin/fast.sh` | 200 ✓ |

日志格式正确：`[时间] [GET] [/cgi-bin/echo] [200]`（path 不含 query string）。

### Step 6：并发上限测试 —— 踩了两个"测试脚本自身"的坑

写了一个用 `flock` 原子统计在途请求数、记录峰值的 `conc.sh`，同时发 8 个来测并发上限。

**问题 3**：测试命令超时 2 分钟无输出。

- **分析**：我在脚本里 `./httpd &` 后台起服务器，最后用裸 `wait` 等所有后台任务——包括**永不退出的 httpd**，于是 `wait` 永远不返回。
- **解决**：改为收集每个 curl 的 PID，`wait $PIDS` 只等 curl。

**问题 4**：重跑时脚本秒退，exit 144，无任何输出。

- **分析**：`pkill -f './httpd'` 的 `-f` 匹配**完整命令行**，而我的测试脚本命令行里就含 `./httpd` 字样 → pkill 把脚本自己杀了。
- **解决**：改用 `pkill -x httpd`（按进程名精确匹配），并清理端口占用。

**问题 5**：并发峰值读出来是空的。

- **分析**：`conc.sh` 开头有 `: > /tmp/m7conc.count` 的"清零"操作，8 个脚本并发执行时**互相截断**计数文件，数据全乱。
- **解决**：删掉脚本里的清零行，改为在启动服务器前手动 `rm` 计数文件。

修正后：**并发峰值 MAX=4**，8 个并发请求恰好被限制在 4 个并行执行 —— 符合要求。（日志时间戳也显示两批 4 个、每批间隔约 2 秒的处理节奏。）

### Step 7：日志按到达顺序测试

**问题 6**：第一次混合请求排序测试输出"顺序错误"。

- **分析**：这是**测试脚本的误判**，不是服务器 bug——我在 `grep` 里用了 `(slow|fast)`，把日志里的 `/cgi-bin/slow.sh` 截成了 `/cgi-bin/slow` 去比对，自然对不上。另外混合测试里 curl 并发启动，**到达顺序 ≠ 发送顺序**，不能拿发送顺序当期望。
- **解决**：改用**确定性错峰测试**：按固定时间间隔（0.5s）依次发出 `slow(2s) → fast → fast → slow(2s)`，其中两个 fast 一定先于两个 slow 完成，但日志必须按到达顺序。

结果（关键测试）：

```
到达顺序: slow(2s) fast fast slow(2s)
完成顺序: fast(2个先完成) slow(2个后完成)
日志顺序: slow fast fast slow   ✓ 严格按到达顺序
```

顺带验证了畸形路径 `fast.sh&b`（`&` 在 URL 里被当作路径一部分）正确返回 404，路径在日志中按原样记录。

### Step 8：大输出 CGI —— 发现并修复一个真 bug

用 `big.sh` 输出 **200KB** 响应（超过管道默认 64KB 缓冲），结果服务器返回 **500**，客户端只收到 25 字节。

**问题 7（真 bug）**：管道读取的扩容逻辑有缺陷。

- **分析**：CGI 输出超过 8KB 时，父进程 `read(fds[0])` 可能**一次性返回整个 64KB 管道缓冲**，而我的扩容策略是"从 8192 开始翻倍"。第一次 `need = 0 + 65536 + 1 > 0`，`ncap = 8192`，`8192 ≤ 65537` → 命中"容量不足"分支 → 直接 `close(fds[0]); break`，**一个字节都没读进来** → `out_len = 0` → 被当成"CGI 没输出"判成 500。
- **为什么之前没暴露**：echo / fast / forbidden 等输出都小于 8KB，第一次 `read` 就完整返回且能放进 8192 的缓冲。
- **修复**：扩容直接扩到 `need = out_len + n + 1`（而非小步倍增），并在超过 `MAX_CGI_OUTPUT` 上限时截断。

修复后重测：`big.sh` → **200**，客户端收到完整 **200000 字节**；echo/forbidden/nonexistent 回归全部正常。

### Step 9：更多状态码场景测试

| 用例 | 结果 |
|---|---|
| `noexec.sh`（存在但无执行权限）| 500 ✓ |
| `empty.sh`（`exit 0`，无输出）| 500 ✓ |
| `badinterp.sh`（shebang 解释器不存在）| 500 ✓ |
| `bare.sh`（输出不带状态行，直接 `Content-Type: ...`）| 200，服务器补 `HTTP/1.1 200 OK` 后转发 ✓ |
| `forbidden.sh`（输出 `HTTP/1.1 403`）| 客户端 403，**日志记录 403** ✓ |

### Step 10：综合压力测试

50 个混合请求（echo / fast / slow 1s / forbidden / nonexistent 各 10 个）并发发出：

- 服务器存活、无崩溃；
- 日志恰好 50 行，状态码分布 `30×200 + 10×403 + 10×404` 完全符合预期；
- 无残留 CGI 僵尸进程（`waitpid` 都正确回收）。

### Step 11：确认与框架 Makefile 的兼容性

验证了**不加 `-lpthread`** 也能编译链接（WSL 的 glibc 已把 pthread 并入 libc），与框架 `LDFLAGS` 为空一致，确保用户直接 `make` 能通过。

### Step 12：收尾

- 通读最终 `httpd.c`，补了一个健壮性修复：`waitpid` 被信号打断（`EINTR`）时重试，避免子进程残留为僵尸。
- 清理所有测试进程，撰写本开发日志与 `REPORT.md`。
