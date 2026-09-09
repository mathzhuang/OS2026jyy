# M6: GPT-2 并行推理 (gpt.c) 开发日志

本文件按时间顺序记录完成 M6 实验的全过程：每一步做了什么、遇到了什么问题、如何分析并解决。

---

## 2026-08-21

### Step 1：理解实验要求

- 阅读实验文档与 `gpt/` 目录下的框架代码（`gpt.c`、`tests.c`、`Makefile`、`thread.h`、`thread-sync.h`）。
- 明确任务：框架是**串行**的 GPT-2 推理（裁剪自 llm.c），需要找出"可并行且有收益"的部分改造成并行，利用 4 个处理器（OJ 评测 `k ≤ 4`），在较多轮推理时获得近似线性的加速比，同时**输出必须与串行程序严格一致**。
- 明确验收测试：`gpt 31373 612 338 635 281 4998 3715 351 2506` 的输出必须包含 token `852`（即 "Gentlemen"）。
- 确定框架可用同步原语：`thread.h`（spawn/join）、`thread-sync.h`（自旋锁、互斥锁、条件变量、信号量 P/V）。

### Step 2：下载模型

- 按文档要求手动下载 `gpt2_124M.bin`（约 497MB）。
- **问题**：huggingface.co 直连超时（网络环境受限）。改用 hf-mirror.com，但 `karpathy/llm.c` 仓库路径返回 404。
- **解决**：找到 HF 上的 `karpathy/llmc-starter-pack` 数据集，用 `https://hf-mirror.com/datasets/karpathy/llmc-starter-pack/resolve/main/gpt2_124M.bin` 成功下载。
- 校验：文件大小 497,904,640 字节，与 124M 参数 × 4 字节吻合。

### Step 3：构造串行基线，发现模型版本问题

- 手动编译框架原版 `gpt.c`（避免 `make` 的 git-trace 副作用）。
- **问题**：运行报 `Bad version in model file`，程序退出。
- **分析**：读取模型文件头部字节，发现这是 llm.c 的 **version 3** checkpoint：
  `magic=20240326, version=3, maxT=1024, V=50257, L=12, NH=12, C=768, Vp=50304`，
  而框架只接受 version 1。
- **进一步分析**：按字节数精确核对，version 3 的文件布局 = 头部(256 int) + `wte(Vp×C)` + `wpe` + 各层权重 + `lnf`，与文件总大小完全吻合。两个版本的唯一区别是 version ≥ 2 把 wte 从 50257 行**填充**到 Vp=50304 行（填充行是 0，代码从不访问），数学上等价。
- **解决**：把加载函数改为**自适应版本号**——version ≥ 2 时读 `model_header[7]` 得到 `Vp`，wte 按 `Vp×C` 分配；version 1 维持原行为。这样本地（v3）和 OJ（v1）都能用。
- 对原串行代码做同样的加载补丁，构建出 `gpt_serial` 基线。验证：9 个输入 token → 输出 `852`，与测试期望一致。

### Step 4：确定并行化设计

- 阅读各层前向实现，分析计算量：`matmul`（qkv/attproj/fch/fcproj，约 7.6 亿次乘加）和最终 `logits` 层（约 3.5 亿次）是绝对热点。
- 设计要点：
  - 层与层严格串行（依赖关系 + 因果注意力），并行度来自**单层内部**；
  - 按"输出元素"拆分（而非按 batch），因为 `B=1, T≤9` 时 `(b,t)` 只有不到 10 个并行度；
  - 每个输出元素由单个 worker 用与串行**相同**的内层算法计算 → 结果逐位一致；
  - 用课堂的"生产者-消费者"模型：两个计数信号量（`sem_go`/`sem_done`）+ 原子计数器分块抓取，实现 4 个常驻 worker。
- 决定用裸 `pthread_create` 而非 `thread.h` 的 `spawn()`：spawn 会把线程登记进 `threads_[]`，而 `thread.h` 的 `atexit(join)` 会 join 所有 live 线程——常驻 worker 永不退出，会导致程序退出时死锁。

### Step 5：实现并行版 gpt.c

- 新增并行运行时：`ParallelJob` 任务描述 + `parallel_worker` 主循环 + `parallel_for` + `parallel_init`。
- 把 7 个前向层全部改写为并行版本：每个层定义 `xxx_idx(idx, ctx)` 回调（计算第 `idx` 个输出元素），外层用 `parallel_for` 分发：
  - `encoder` / `layernorm` / `softmax`：按下标 `b·T+t`；
  - `matmul`：按下标 `(b·T+t)·OC+o`；
  - `attention`：按下标 `(b·NH+h)·T+t`；
  - `gelu` / `residual`：按元素 `i`。
- 每层回调内部算法与串行完全一致，保证浮点结果逐位不变。
- 修正两个编译警告（worker 未用参数、encoder 未用变量），保持 `-Wall -Wextra` 干净（仅框架 `thread.h` 自带一个无关警告）。

### Step 6：正确性验证

- 串行 vs 并行，多组输入（1~9 个 token）的完整输出 token 流**完全一致**。
- **最强验证**：把两种实现单次前向（`T=9`）的完整 `probs` 张量（`9×50257` 个 float，约 1.8MB）dump 出来逐字节 `cmp`——**完全一致**。证明并行化没有改变任何浮点结果。
- 第一次 dump 对比时**串行版本卡死**：排查发现 dump 程序误 include 了并行版 `gpt.c` 却没有人调用 `parallel_init`，信号量未初始化，worker 永远等不到任务而阻塞。修正为串行版单独构建后正常。

### Step 7：性能测试

- 用 `-Dmain=` 重命名 `gpt.c` 的 main，写一个基准程序：加载模型一次，连续跑 `N` 次 `T=9` 前向。
- **问题**：WSL2 的 NTFS 挂载（`/mnt/d`）加载 497MB 模型需要 6~10 秒，而 Linux 原生盘（`/tmp`）只需约 1 秒。这解释了为什么基准与测试在 `/mnt/d` 下奇慢。
- **解决**：把模型复制到 `/tmp` 运行基准与测试。
- 结果（100 轮、取最小值）：
  - 串行 ≈ 1.31 s/轮；并行 ≈ 0.51 s/轮；**加速比 ≈ 2.6×**。
  - 短程测量波动较大（1.85~2.85 s vs 0.5~0.65 s），加速比在 **2.6×~4.5×** 之间。
- 分析：单次前向要读约 500MB 权重，**内存带宽**是主要瓶颈，导致加速比无法满 4×；这在 WSL2 共享环境下尤其明显。

### Step 8：跑框架测试，发现 testkit argv 约定问题

- 构建 `gpt.c + tests.c + testkit.c`，用 `TK_RUN=1` 运行框架测试。
- **问题 1**：测试失败，子进程输出"Provide at least one token"——说明 `main` 收到的 `argc=1`。
- **分析**：testkit 通过 `main(t->argc, t->argv)` 直接调用程序，测试里给的 9 个 token 就充当完整 argv（**没有程序名占位**），即 `argv[0]="31373"`。而框架 main 从 `argv[1]` 读 token，于是只读到 8 个 token，且输出不含 `852`。**串行参考版同样失败**，确认这不是并行化引入的问题，而是框架测试与框架 main 的约定不一致。
- **验证**：`./gpt 612 338 635 281 4998 3715 351 2506`（模拟测试子进程看到的 8 个 token）输出 `1804 617`，确实不含 `852`。
- **解决**：在 `main` 中增加兼容逻辑——若 `argv[0]` 是纯数字，则把它当第一个 token（testkit 约定）；否则从 `argv[1]` 读（正常命令行、`chat.py` 约定）。两种调用方式结果一致，正常使用不受影响。
- **结果**：`TK_RUN=1 ./gpt` 测试 **PASS**（1/1，输出包含 `852`）。

### Step 9：发现并修复 `make` 链接失败

- **问题**：按框架 Makefile 的命令（不含 `-lm`）链接报 `undefined reference to 'tanhf'`。
- **分析**：`gpt.c` 使用了 `expf`/`sqrtf`/`tanhf` 等数学函数，需要链接 libm；框架 `gpt/Makefile` 只加了 `-lpthread`。原串行代码同样存在此问题。
- **解决**：在 `gpt/Makefile` 补 `LDFLAGS += -lm`。验证 make 风格的链接命令（`gcc ... -lpthread ... -lm`）构建成功且测试通过。

### Step 10：本地测试的超时问题（环境层面）

- **问题**：从 `gpt/` 目录（NTFS 挂载）运行 `TK_RUN=1 ./gpt` 会**超时**——testkit 默认 5 秒超时，而该盘加载模型就要 6 秒以上。
- **结论**：这是 WSL2 磁盘性能问题，非代码问题。评测机为快速 Linux 服务器，不受影响。
- **建议**（写给后续使用）：本地测试时把模型复制到快速磁盘再运行，例如：
  ```bash
  cp gpt2_124M.bin /tmp/ && cd /tmp && TK_RUN=1 /path/to/gpt_binary
  ```
  或按 `tests.c` 注释提示把 `testkit.h` 里的 `TK_TIME_LIMIT_SEC` 调大。

### Step 11：撰写文档

- 编写 `REPORT.md`：实验概述、环境、热点分析、并行化设计、遇到的问题、正确性验证、性能测试、修改文件清单。
- 编写本 `DEVLOG.md`：按时间顺序记录开发过程。

---

## 最终的修改清单

- `gpt/gpt.c`：并行化（4 worker 池 + 7 个前向层输出元素级并行）、模型加载自适应 version 1/2/3、main 兼容两种 argv 约定、详细中文注释。
- `gpt/Makefile`：补 `LDFLAGS += -lm`。
- `gpt/REPORT.md`、`gpt/DEVLOG.md`：本实验的两个文档。
