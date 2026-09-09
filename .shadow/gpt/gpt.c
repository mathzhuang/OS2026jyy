// ============================================================================
// M6: GPT-2 并行推理 (gpt.c)
// ----------------------------------------------------------------------------
// 实验目标：框架给出的是从 llm.c (https://github.com/karpathy/llm.c) 裁剪出来的
//           串行 GPT-2 推理实现，只能利用单个处理器。我们需要找出其中"可并行且
//           有收益"的部分，把它改造成并行，从而在 k 个处理器的机器上获得近似
//           线性的加速比（Online Judge 评测时 k <= 4）。
//
// 并行化思路（总览）
// ----------------------------------------------------------------------------
// 1. 找出热点：整个神经网络推理 = 若干层的前向计算，层与层之间是严格串行的
//    （后一层依赖前一层的输出）。但"每一层内部"都包含大量彼此独立的计算：
//    - matmul（矩阵乘法）：输出张量的每个元素 = 一个独立的点积，互不依赖；
//    - attention（自注意力）：每个 (b, t, h)（batch, 位置, 注意力头）独立计算；
//    - layernorm / softmax / encoder / gelu / residual：每个位置或元素独立。
//    这些"元素级并行"正是我们要利用的并行度。
//
// 2. 实现机制：采用课堂上的"生产者-消费者"模型。
//    - 主线程是"生产者"：它顺序地推进每一层，当遇到一层时，把该层的"工作描述"
//      发布给一个全局任务槽，然后唤醒 4 个 worker 线程去计算；
//    - 4 个 worker 线程是"消费者"：它们通过一个原子计数器动态抓取一批下标，
//      各自计算属于自己的一批输出元素，算完该层后汇报；
//    - 主线程等所有 worker 都做完一层，才继续下一层（层间靠"全量汇合"保持顺序）。
//
// 3. 正确性：因为每个输出元素都由某一个 worker 用与串行完全相同的循环计算出来
//    （我们没有改变任何一层内部的算术），所以结果与串行版本逐位（bit-identical）
//    一致，最终输出的 token 序列自然与串行程序完全一致。
//
// 4. 为什么不用更复杂的手段：
//    - 层间流水线（pipeline）并行：因为自注意力是"因果"的，且层与层必须依次
//      推进，做层间流水会严重增加复杂度而收益有限，这里不做；
//    - 把点积拆给多个 worker：会破坏累加顺序、改变浮点结果，没必要，且会引入
//      额外同步。我们只在"输出元素"这一层粒度上并行。
//
// 模型加载说明：本仓库框架预期 checkpoint 头版本为 1（wte 未填充）。而我们手动
// 下载到的 llm.c 新版 checkpoint 是版本 3（头部多了 Vp=50304 的"填充词表大小"，
// wte 从 50257 行填充到 50304 行）。两个版本数学上完全等价（填充行永远是 0，
// 且代码从不访问 [V, Vp) 这些行）。因此我们把加载函数改成自适应版本号，本地
// 评测可用版本 3 文件，Online Judge 用版本 1 文件也完全不受影响。
// ============================================================================

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

#include "thread.h"
#include "thread-sync.h"

// ============================================================================
// 并行运行时：worker 线程池 + parallel_for
// ----------------------------------------------------------------------------
// 下面的设计对应课程里"生产者-消费者"的经典问题：
//   - sem_go   ："有活干了"信号。主线程每发布一层任务就 post nworkers 次，
//                  worker 线程 post 一次唤醒一个、消费一个；
//   - sem_done ："活干完了"信号。每个 worker 算完一层就 post 一次，
//                  主线程收满 nworkers 次才继续推进下一层。
// 这样 worker 不会"抢跑"到下一层，层与层之间保持严格串行。
// ============================================================================

#define NWORKERS 4            // 静态分配的 worker 线程数（OJ 评测 k <= 4）

static sem_t sem_go;          // 生产者 -> 消费者："有新任务了"
static sem_t sem_done;        // 消费者 -> 生产者："这一层我干完了"
static pthread_t worker_tids[NWORKERS];
static int nworkers = NWORKERS;

// 一个"任务"：把整数下标区间 [0, n) 交给 worker 们去算。
// fn(i, ctx) 负责计算第 i 个输出元素；ctx 是各层自己携带的参数包。
typedef struct {
    int n;                          // 下标总数（= 该层输出元素的个数）
    int chunk;                      // 每个 worker 一次抓取的下标块大小
    int next;                       // 下一个待抓取的下标（原子，供竞争抓取）
    void (*fn)(int idx, void *ctx); // 计算单个输出元素的函数
    void *ctx;                      // 该层的参数包
} ParallelJob;

static ParallelJob pjob;

// worker 主循环：等任务 -> 分块抓下标 -> 计算 -> 汇报，如此往复。
static void *parallel_worker(void *arg) {
    (void)arg; // 线程编号在这里用不到（任务通过全局 pjob 分发）
    for (;;) {
        P(&sem_go);                 // 阻塞等待主线程发布新任务
        // 动态抓取：所有 worker 竞争同一个原子计数器 pjob.next，
        // 一次抓 chunk 个下标，抓完为止。这样负载会自动均衡。
        for (;;) {
            int i = __atomic_fetch_add(&pjob.next, pjob.chunk, __ATOMIC_RELAXED);
            if (i >= pjob.n) break;                 // 任务已被抢完
            int end = i + pjob.chunk;
            if (end > pjob.n) end = pjob.n;         // 最后一个块可能不满
            for (int k = i; k < end; k++) pjob.fn(k, pjob.ctx);
        }
        V(&sem_done);               // 通知主线程：本 worker 已干完这一层
    }
    return NULL;
}

// 并行执行 fn(0..n-1, ctx)。主线程在此阻塞，直到全部 worker 完成。
static void parallel_for(int n, void (*fn)(int, void *), void *ctx) {
    if (n <= 1) {                   // 工作量太小，不值得开线程（摊还调度开销）
        if (n == 1) fn(0, ctx);
        return;
    }
    pjob.n = n;
    pjob.fn = fn;
    pjob.ctx = ctx;
    // 让每个 worker 大约抓 4 块就抓完：既减少锁/原子竞争，又能负载均衡。
    pjob.chunk = n / (nworkers * 4);
    if (pjob.chunk < 1) pjob.chunk = 1;
    __atomic_store_n(&pjob.next, 0, __ATOMIC_RELAXED);
    for (int i = 0; i < nworkers; i++) V(&sem_go);  // 广播任务
    for (int i = 0; i < nworkers; i++) P(&sem_done); // 等所有人干完
}

// 在 main 中调用一次：创建 4 个常驻 worker 线程。
// 注意：这里刻意用裸 pthread_create，而不是课堂 thread.h 的 spawn()。
// 因为 spawn() 会把线程登记到 threads_[] 里，而 thread.h 的 atexit(join) 会
// 试图 join 所有 live 线程——我们的 worker 是永不退出的"常驻线程"，那样会
// 在程序退出时死锁。用裸 pthread_create 则不会进 join 名单。
// （没有设为 static，便于测试/基准程序显式地调用它。）
void parallel_init(void) {
    sem_init(&sem_go, 0, 0);
    sem_init(&sem_done, 0, 0);
    for (int i = 0; i < nworkers; i++) {
        pthread_create(&worker_tids[i], NULL, parallel_worker, NULL);
    }
}

// ============================================================================
// GPT-2 各层的前向计算（并行版本）
// ----------------------------------------------------------------------------
// 每个层的函数签名与串行版本完全一致，只是把最外层循环按"输出元素下标"拆分，
// 交给 parallel_for 去并行。每个 worker 只负责计算其中一部分输出元素，且单个
// 输出元素的内部算法与串行实现一模一样，因此浮点结果逐位不变。
// ============================================================================

// ---------------- encoder（token + position embedding 求和） ----------------
// 输出 out(B,T,C)：每个 (b,t) 位置 = wte[inp[b,t]] + wpe[t]。
// 不同 (b,t) 之间互相独立，故按下标 idx = b*T + t 并行。
typedef struct {
    float *out, *wte, *wpe;
    int *inp;
    int T, C;
} EncoderCtx;

static void encoder_idx(int idx, void *arg) {
    EncoderCtx *c = arg;
    int T = c->T, C = c->C;
    int t = idx % T;                 // 位置 t
    // 注意：out + b*T*C + t*C == out + (b*T+t)*C == out + idx*C，
    // 所以这里不需要单独解出 b 和 t 就能定位输出行。
    float *out_bt = c->out + idx * C;
    int ix = c->inp[idx];            // inp[b*T+t]
    float *wte_ix = c->wte + ix * C; // token 对应的 embedding 行
    float *wpe_t = c->wpe + t * C;   // 位置对应的 embedding 行
    for (int i = 0; i < C; i++) {
        out_bt[i] = wte_ix[i] + wpe_t[i];
    }
}

void encoder_forward(float *out, int *inp, float *wte, float *wpe,
                     int B, int T, int C) {
    EncoderCtx ctx = { .out = out, .inp = inp, .wte = wte, .wpe = wpe, .T = T, .C = C };
    parallel_for(B * T, encoder_idx, &ctx);
}

// ---------------- layernorm（层归一化） ----------------
// 输出 out(B,T,C)、mean/rstd(B,T)：每个 (b,t) 位置对 C 维向量做归一化。
// 不同 (b,t) 独立，故按 idx = b*T + t 并行。
typedef struct {
    float *out, *mean, *rstd, *inp, *weight, *bias;
    int T, C;
} LayernormCtx;

static void layernorm_idx(int idx, void *arg) {
    LayernormCtx *c = arg;
    int C = c->C;
    float *x = c->inp + idx * C;     // 该 (b,t) 的输入向量
    float eps = 1e-5f;
    float m = 0.0f;
    for (int i = 0; i < C; i++) m += x[i];
    m = m / C;
    float v = 0.0f;
    for (int i = 0; i < C; i++) {
        float xshift = x[i] - m;
        v += xshift * xshift;
    }
    v = v / C;
    float s = 1.0f / sqrtf(v + eps); // 标准差的倒数
    float *out_bt = c->out + idx * C;
    for (int i = 0; i < C; i++) {
        float n = s * (x[i] - m);    // 归一化
        out_bt[i] = n * c->weight[i] + c->bias[i]; // 缩放 + 平移
    }
    c->mean[idx] = m;
    c->rstd[idx] = s;
}

void layernorm_forward(float *out, float *mean, float *rstd,
                       float *inp, float *weight, float *bias,
                       int B, int T, int C) {
    LayernormCtx ctx = { .out = out, .mean = mean, .rstd = rstd,
                         .inp = inp, .weight = weight, .bias = bias, .T = T, .C = C };
    parallel_for(B * T, layernorm_idx, &ctx);
}

// ---------------- matmul（矩阵乘法，整个推理的最大热点） ----------------
// 输出 out(B,T,OC)：每个元素 out[b,t,o] = dot(inp[b,t,:], weight[o,:]) + bias[o]。
// 共 B*T*OC 个元素，每个都是一次独立点积，按 idx = bt*OC + o 并行。
// 注意点积内部仍是同一个 worker 串行累加，所以结果和串行版本逐位一致。
typedef struct {
    float *out, *inp, *weight, *bias;
    int T, C, OC;
} MatmulCtx;

static void matmul_idx(int idx, void *arg) {
    MatmulCtx *c = arg;
    int OC = c->OC, C = c->C;
    int o = idx % OC;                // 输出通道
    int bt = idx / OC;               // b*T + t
    float *out_bt = c->out + bt * OC;
    float *inp_bt = c->inp + bt * C;
    float *wrow = c->weight + o * C;
    float val = c->bias ? c->bias[o] : 0.0f; // bias 可能为 NULL（如 logits 层）
    for (int i = 0; i < C; i++) {
        val += inp_bt[i] * wrow[i];
    }
    out_bt[o] = val;
}

void matmul_forward(float *out, float *inp, float *weight, float *bias,
                    int B, int T, int C, int OC) {
    MatmulCtx ctx = { .out = out, .inp = inp, .weight = weight, .bias = bias,
                      .T = T, .C = C, .OC = OC };
    parallel_for(B * T * OC, matmul_idx, &ctx);
}

// ---------------- attention（自注意力，因果掩码） ----------------
// 每个 (b, t, h)（batch, 位置, 注意力头）独立完成四遍扫描：算 Q·K、softmax、
// 加权 V 累加。头与头、位置与位置之间读写的内存区域不相交，故按
// idx = (b*NH + h)*T + t 并行。内部算法与串行完全一致。
typedef struct {
    float *out, *preatt, *att, *inp;
    int T, C, NH;
} AttentionCtx;

static void attention_idx(int idx, void *arg) {
    AttentionCtx *c = arg;
    int T = c->T, C = c->C, NH = c->NH;
    int C3 = C * 3;                  // QKV 拼在一起的通道数
    int hs = C / NH;                 // 每个头的维度
    float scale = 1.0f / sqrtf(hs);
    int t = idx % T;                 // 位置
    int rest = idx / T;              // b*NH + h
    int h = rest % NH;               // 头
    int b = rest / NH;               // batch

    float *query_t = c->inp + b * T * C3 + t * C3 + h * hs;
    float *preatt_bth = c->preatt + b * NH * T * T + h * T * T + t * T;
    float *att_bth = c->att + b * NH * T * T + h * T * T + t * T;

    // pass 1: query 点乘所有 key，并记录最大值（数值稳定用）
    float maxval = -10000.0f;
    for (int t2 = 0; t2 <= t; t2++) {
        float *key_t2 = c->inp + b * T * C3 + t2 * C3 + h * hs + C; // +C 因为它是 key
        float val = 0.0f;
        for (int i = 0; i < hs; i++) val += query_t[i] * key_t2[i];
        val *= scale;
        if (val > maxval) maxval = val;
        preatt_bth[t2] = val;
    }

    // pass 2: exp 并求和（softmax 分母）
    float expsum = 0.0f;
    for (int t2 = 0; t2 <= t; t2++) {
        float expv = expf(preatt_bth[t2] - maxval);
        expsum += expv;
        att_bth[t2] = expv;
    }
    float expsum_inv = expsum == 0.0f ? 0.0f : 1.0f / expsum;

    // pass 3: 归一化得到注意力权重，t2 > t 处置 0（因果掩码）
    for (int t2 = 0; t2 < T; t2++) {
        att_bth[t2] = (t2 <= t) ? att_bth[t2] * expsum_inv : 0.0f;
    }

    // pass 4: 用权重累加 value，得到注意力输出
    float *out_bth = c->out + b * T * C + t * C + h * hs;
    for (int i = 0; i < hs; i++) out_bth[i] = 0.0f;
    for (int t2 = 0; t2 <= t; t2++) {
        float *value_t2 = c->inp + b * T * C3 + t2 * C3 + h * hs + C * 2; // +2C 因为它是 value
        float a = att_bth[t2];
        for (int i = 0; i < hs; i++) out_bth[i] += a * value_t2[i];
    }
}

void attention_forward(float *out, float *preatt, float *att, float *inp,
                       int B, int T, int C, int NH) {
    AttentionCtx ctx = { .out = out, .preatt = preatt, .att = att, .inp = inp,
                         .T = T, .C = C, .NH = NH };
    parallel_for(B * T * NH, attention_idx, &ctx);
}

// ---------------- gelu（激活函数，逐元素） ----------------
#define GELU_SCALING_FACTOR sqrtf(2.0f / M_PI)

typedef struct { float *out, *inp; } GeluCtx;

static void gelu_idx(int idx, void *arg) {
    GeluCtx *c = arg;
    float x = c->inp[idx];
    float cube = 0.044715f * x * x * x;
    c->out[idx] = 0.5f * x * (1.0f + tanhf(GELU_SCALING_FACTOR * (x + cube)));
}

void gelu_forward(float *out, float *inp, int N) {
    GeluCtx ctx = { .out = out, .inp = inp };
    parallel_for(N, gelu_idx, &ctx);
}

// ---------------- residual（残差连接，逐元素） ----------------
typedef struct { float *out, *inp1, *inp2; } ResidualCtx;

static void residual_idx(int idx, void *arg) {
    ResidualCtx *c = arg;
    c->out[idx] = c->inp1[idx] + c->inp2[idx];
}

void residual_forward(float *out, float *inp1, float *inp2, int N) {
    ResidualCtx ctx = { .out = out, .inp1 = inp1, .inp2 = inp2 };
    parallel_for(N, residual_idx, &ctx);
}

// ---------------- softmax（最后把 logits 变成概率，逐位置） ----------------
// 每个 (b,t) 位置对 V 维 logits 做一次 softmax，共 B*T 个位置，按 idx = b*T + t 并行。
typedef struct { float *probs, *logits; int V; } SoftmaxCtx;

static void softmax_idx(int idx, void *arg) {
    SoftmaxCtx *c = arg;
    int V = c->V;
    float *logits_bt = c->logits + idx * V;
    float *probs_bt = c->probs + idx * V;
    float maxval = -10000.0f;        // 减最大值，数值稳定
    for (int i = 0; i < V; i++) {
        if (logits_bt[i] > maxval) maxval = logits_bt[i];
    }
    float sum = 0.0f;
    for (int i = 0; i < V; i++) {
        probs_bt[i] = expf(logits_bt[i] - maxval);
        sum += probs_bt[i];
    }
    for (int i = 0; i < V; i++) probs_bt[i] /= sum;
}

void softmax_forward(float *probs, float *logits, int B, int T, int V) {
    SoftmaxCtx ctx = { .probs = probs, .logits = logits, .V = V };
    parallel_for(B * T, softmax_idx, &ctx);
}

// ============================================================================
// GPT-2 模型定义
// ============================================================================

// 模型全部参数张量（与串行版本一致）
#define NUM_PARAMETER_TENSORS 16
typedef struct {
    float *wte;      // (V, C)
    float *wpe;      // (maxT, C)
    float *ln1w;     // (L, C)
    float *ln1b;     // (L, C)
    float *qkvw;     // (L, 3*C, C)
    float *qkvb;     // (L, 3*C)
    float *attprojw; // (L, C, C)
    float *attprojb; // (L, C)
    float *ln2w;     // (L, C)
    float *ln2b;     // (L, C)
    float *fcw;      // (L, 4*C, C)
    float *fcb;      // (L, 4*C)
    float *fcprojw;  // (L, C, 4*C)
    float *fcprojb;  // (L, C)
    float *lnfw;     // (C)
    float *lnfb;     // (C)
} ParameterTensors;

// 一次性 malloc 所有参数，并把每个张量指针指向正确位置
float *malloc_and_point_parameters(ParameterTensors *params, size_t *param_sizes) {
    size_t num_parameters = 0;
    for (size_t i = 0; i < NUM_PARAMETER_TENSORS; i++) {
        num_parameters += param_sizes[i];
    }
    float *params_memory = (float *)malloc(num_parameters * sizeof(float));
    float **ptrs[] = {
        &params->wte, &params->wpe, &params->ln1w, &params->ln1b, &params->qkvw, &params->qkvb,
        &params->attprojw, &params->attprojb, &params->ln2w, &params->ln2b, &params->fcw, &params->fcb,
        &params->fcprojw, &params->fcprojb, &params->lnfw, &params->lnfb
    };
    float *params_memory_iterator = params_memory;
    for (size_t i = 0; i < NUM_PARAMETER_TENSORS; i++) {
        *(ptrs[i]) = params_memory_iterator;
        params_memory_iterator += param_sizes[i];
    }
    return params_memory;
}

// 激活（中间结果）张量
#define NUM_ACTIVATION_TENSORS 23
typedef struct {
    float *encoded;   // (B, T, C)
    float *ln1;       // (L, B, T, C)
    float *ln1_mean;  // (L, B, T)
    float *ln1_rstd;  // (L, B, T)
    float *qkv;       // (L, B, T, 3*C)
    float *atty;      // (L, B, T, C)
    float *preatt;    // (L, B, NH, T, T)
    float *att;       // (L, B, NH, T, T)
    float *attproj;   // (L, B, T, C)
    float *residual2; // (L, B, T, C)
    float *ln2;       // (L, B, T, C)
    float *ln2_mean;  // (L, B, T)
    float *ln2_rstd;  // (L, B, T)
    float *fch;       // (L, B, T, 4*C)
    float *fch_gelu;  // (L, B, T, 4*C)
    float *fcproj;    // (L, B, T, C)
    float *residual3; // (L, B, T, C)
    float *lnf;       // (B, T, C)
    float *lnf_mean;  // (B, T)
    float *lnf_rstd;  // (B, T)
    float *logits;    // (B, T, V)
    float *probs;     // (B, T, V)
    float *losses;    // (B, T)
} ActivationTensors;

float *malloc_and_point_activations(ActivationTensors *acts, size_t *act_sizes) {
    size_t num_activations = 0;
    for (size_t i = 0; i < NUM_ACTIVATION_TENSORS; i++) {
        num_activations += act_sizes[i];
    }
    float *acts_memory = (float *)malloc(num_activations * sizeof(float));
    float **ptrs[] = {
        &acts->encoded, &acts->ln1, &acts->ln1_mean, &acts->ln1_rstd, &acts->qkv, &acts->atty,
        &acts->preatt, &acts->att, &acts->attproj, &acts->residual2, &acts->ln2, &acts->ln2_mean,
        &acts->ln2_rstd, &acts->fch, &acts->fch_gelu, &acts->fcproj, &acts->residual3, &acts->lnf,
        &acts->lnf_mean, &acts->lnf_rstd, &acts->logits, &acts->probs, &acts->losses
    };
    float *acts_memory_iterator = acts_memory;
    for (size_t i = 0; i < NUM_ACTIVATION_TENSORS; i++) {
        *(ptrs[i]) = acts_memory_iterator;
        acts_memory_iterator += act_sizes[i];
    }
    return acts_memory;
}

// GPT-2 超参数
typedef struct {
    int max_seq_len; // 最大序列长度，如 1024
    int vocab_size;  // 词表大小，如 50257
    int num_layers;  // Transformer 层数，如 12
    int num_heads;   // 注意力头数，如 12
    int channels;    // 通道数，如 768
} GPT2Config;

// 模型整体
typedef struct {
    GPT2Config config;
    ParameterTensors params;      // 权重
    size_t param_sizes[NUM_PARAMETER_TENSORS];
    float *params_memory;
    int num_parameters;
    ParameterTensors grads;       // 梯度（推理用不到）
    float *grads_memory;
    float *m_memory;              // AdamW 优化器状态（推理用不到）
    float *v_memory;
    ActivationTensors acts;       // 激活值
    size_t act_sizes[NUM_ACTIVATION_TENSORS];
    float *acts_memory;
    int num_activations;
    ActivationTensors grads_acts;
    float *grads_acts_memory;
    int batch_size;               // 当前前向的 B
    int seq_len;                  // 当前前向的 T
    int *inputs;
    int *targets;
    float mean_loss;
} GPT2;

// ============================================================================
// 从 checkpoint 文件加载模型
// ----------------------------------------------------------------------------
// 自适应版本号：兼容 llm.c 的 version 1/2/3。
//  version 1：头部无 Vp 字段，wte 按真实词表 V 存储（50257 行）；
//  version 2/3：头部多出 Vp（int[7]）字段，wte 填充到 Vp（50257 -> 50304 行）。
// 填充行永远是 0，且本代码从不访问 [V, Vp) 这些行，所以两种文件数学等价。
// ============================================================================
void gpt2_build_from_checkpoint(GPT2 *model, char *checkpoint_path) {
    FILE *model_file = fopen(checkpoint_path, "rb");
    if (model_file == NULL) { printf("Error opening model file\n"); exit(1); }
    int model_header[256];
    if (fread(model_header, sizeof(int), 256, model_file) != 256) {
        printf("Error reading model header\n"); exit(1);
    }
    if (model_header[0] != 20240326) { printf("Bad magic model file"); exit(1); }
    if (model_header[1] != 1 && model_header[1] != 2 && model_header[1] != 3) {
        printf("Unsupported checkpoint version: %d\n", model_header[1]); exit(1);
    }

    // 读取超参数
    int maxT, V, L, NH, C;
    model->config.max_seq_len = maxT = model_header[2];
    model->config.vocab_size = V = model_header[3];
    model->config.num_layers = L = model_header[4];
    model->config.num_heads = NH = model_header[5];
    model->config.channels = C = model_header[6];

    // 版本 >= 2 的 checkpoint 把词表填充到 Vp（64 的倍数），wte 按 Vp 行存储
    int Vp = (model_header[1] >= 2) ? model_header[7] : V;

    // 各参数张量的大小（wte 用 Vp，其余与串行版本一致）
    model->param_sizes[0] = Vp * C;          // wte（填充后）
    model->param_sizes[1] = maxT * C;        // wpe
    model->param_sizes[2] = L * C;           // ln1w
    model->param_sizes[3] = L * C;           // ln1b
    model->param_sizes[4] = L * (3 * C) * C; // qkvw
    model->param_sizes[5] = L * (3 * C);     // qkvb
    model->param_sizes[6] = L * C * C;       // attprojw
    model->param_sizes[7] = L * C;           // attprojb
    model->param_sizes[8] = L * C;           // ln2w
    model->param_sizes[9] = L * C;           // ln2b
    model->param_sizes[10] = L * (4 * C) * C; // fcw
    model->param_sizes[11] = L * (4 * C);    // fcb
    model->param_sizes[12] = L * C * (4 * C); // fcprojw
    model->param_sizes[13] = L * C;          // fcprojb
    model->param_sizes[14] = C;              // lnfw
    model->param_sizes[15] = C;              // lnfb

    size_t num_parameters = 0;
    for (size_t i = 0; i < NUM_PARAMETER_TENSORS; i++) {
        num_parameters += model->param_sizes[i];
    }
    model->num_parameters = num_parameters;

    // 一次性读出全部参数
    model->params_memory = malloc_and_point_parameters(&model->params, model->param_sizes);
    if (fread(model->params_memory, sizeof(float), num_parameters, model_file) != num_parameters) {
        printf("Error reading model weights\n"); exit(1);
    }
    fclose(model_file);

    // 其余运行状态初始化
    model->acts_memory = NULL;
    model->grads_memory = NULL;
    model->m_memory = NULL;
    model->v_memory = NULL;
    model->grads_acts_memory = NULL;
    model->inputs = NULL;
    model->targets = NULL;
    model->batch_size = 0;
    model->seq_len = 0;
    model->mean_loss = -1.0f;
}

// ============================================================================
// 前向推理：把长度为 T 的 token 序列过一次 Transformer，得到各位置的概率分布。
// 层与层之间严格串行，但每层内部已经并行化了（见上面的各层实现）。
// ============================================================================
void gpt2_forward(GPT2 *model, int *inputs, int B, int T) {
    int V = model->config.vocab_size;
    int L = model->config.num_layers;
    int NH = model->config.num_heads;
    int C = model->config.channels;

    model->batch_size = B;
    model->seq_len = T;

    // 按当前 B、T 计算各激活张量的大小并分配
    model->act_sizes[0] = B * T * C;              // encoded
    model->act_sizes[1] = L * B * T * C;          // ln1
    model->act_sizes[2] = L * B * T;              // ln1_mean
    model->act_sizes[3] = L * B * T;              // ln1_rstd
    model->act_sizes[4] = L * B * T * 3 * C;      // qkv
    model->act_sizes[5] = L * B * T * C;          // atty
    model->act_sizes[6] = L * B * NH * T * T;     // preatt
    model->act_sizes[7] = L * B * NH * T * T;     // att
    model->act_sizes[8] = L * B * T * C;          // attproj
    model->act_sizes[9] = L * B * T * C;          // residual2
    model->act_sizes[10] = L * B * T * C;         // ln2
    model->act_sizes[11] = L * B * T;             // ln2_mean
    model->act_sizes[12] = L * B * T;             // ln2_rstd
    model->act_sizes[13] = L * B * T * 4 * C;     // fch
    model->act_sizes[14] = L * B * T * 4 * C;     // fch_gelu
    model->act_sizes[15] = L * B * T * C;         // fcproj
    model->act_sizes[16] = L * B * T * C;         // residual3
    model->act_sizes[17] = B * T * C;             // lnf
    model->act_sizes[18] = B * T;                 // lnf_mean
    model->act_sizes[19] = B * T;                 // lnf_rstd
    model->act_sizes[20] = B * T * V;             // logits
    model->act_sizes[21] = B * T * V;             // probs
    model->act_sizes[22] = B * T;                 // losses
    size_t num_activations = 0;
    for (size_t i = 0; i < NUM_ACTIVATION_TENSORS; i++) {
        num_activations += model->act_sizes[i];
    }
    model->num_activations = num_activations;

    if (model->acts_memory) {
        free(model->acts_memory);
        model->acts_memory = NULL;
    }
    model->acts_memory = malloc_and_point_activations(&model->acts, model->act_sizes);

    if (model->inputs) free(model->inputs);
    model->inputs = (int *)malloc(B * T * sizeof(int));
    memcpy(model->inputs, inputs, B * T * sizeof(int));

    // ---- 逐层前向（每层内部已并行）----
    ParameterTensors params = model->params;
    ActivationTensors acts = model->acts;
    float *residual;

    // 第一层：token embedding + position embedding
    encoder_forward(acts.encoded, inputs, params.wte, params.wpe, B, T, C);

    for (int l = 0; l < L; l++) {
        // 本层的输入残差：第 0 层用 encoded，之后用上一层 residual3
        residual = l == 0 ? acts.encoded : acts.residual3 + (l - 1) * B * T * C;

        // 本层各权重指针
        float *l_ln1w = params.ln1w + l * C;
        float *l_ln1b = params.ln1b + l * C;
        float *l_qkvw = params.qkvw + l * 3 * C * C;
        float *l_qkvb = params.qkvb + l * 3 * C;
        float *l_attprojw = params.attprojw + l * C * C;
        float *l_attprojb = params.attprojb + l * C;
        float *l_ln2w = params.ln2w + l * C;
        float *l_ln2b = params.ln2b + l * C;
        float *l_fcw = params.fcw + l * 4 * C * C;
        float *l_fcb = params.fcb + l * 4 * C;
        float *l_fcprojw = params.fcprojw + l * C * 4 * C;
        float *l_fcprojb = params.fcprojb + l * C;

        // 本层各激活指针
        float *l_ln1 = acts.ln1 + l * B * T * C;
        float *l_ln1_mean = acts.ln1_mean + l * B * T;
        float *l_ln1_rstd = acts.ln1_rstd + l * B * T;
        float *l_qkv = acts.qkv + l * B * T * 3 * C;
        float *l_atty = acts.atty + l * B * T * C;
        float *l_preatt = acts.preatt + l * B * NH * T * T;
        float *l_att = acts.att + l * B * NH * T * T;
        float *l_attproj = acts.attproj + l * B * T * C;
        float *l_residual2 = acts.residual2 + l * B * T * C;
        float *l_ln2 = acts.ln2 + l * B * T * C;
        float *l_ln2_mean = acts.ln2_mean + l * B * T;
        float *l_ln2_rstd = acts.ln2_rstd + l * B * T;
        float *l_fch = acts.fch + l * B * T * 4 * C;
        float *l_fch_gelu = acts.fch_gelu + l * B * T * 4 * C;
        float *l_fcproj = acts.fcproj + l * B * T * C;
        float *l_residual3 = acts.residual3 + l * B * T * C;

        // 一个 Transformer 块：
        //   LN -> QKV 投影 -> Attention -> 输出投影 -> 残差
        //   -> LN -> 全连接(放大4倍) -> GELU -> 全连接(缩回) -> 残差
        layernorm_forward(l_ln1, l_ln1_mean, l_ln1_rstd, residual, l_ln1w, l_ln1b, B, T, C);
        matmul_forward(l_qkv, l_ln1, l_qkvw, l_qkvb, B, T, C, 3 * C);
        attention_forward(l_atty, l_preatt, l_att, l_qkv, B, T, C, NH);
        matmul_forward(l_attproj, l_atty, l_attprojw, l_attprojb, B, T, C, C);
        residual_forward(l_residual2, residual, l_attproj, B * T * C);
        layernorm_forward(l_ln2, l_ln2_mean, l_ln2_rstd, l_residual2, l_ln2w, l_ln2b, B, T, C);
        matmul_forward(l_fch, l_ln2, l_fcw, l_fcb, B, T, C, 4 * C);
        gelu_forward(l_fch_gelu, l_fch, B * T * 4 * C);
        matmul_forward(l_fcproj, l_fch_gelu, l_fcprojw, l_fcprojb, B, T, 4 * C, C);
        residual_forward(l_residual3, l_residual2, l_fcproj, B * T * C);
    }

    // 最后一个残差 + 最终 LayerNorm + logits（复用 wte 作为输出投影） + softmax
    residual = acts.residual3 + (L - 1) * B * T * C;
    layernorm_forward(acts.lnf, acts.lnf_mean, acts.lnf_rstd, residual, params.lnfw, params.lnfb, B, T, C);
    matmul_forward(acts.logits, acts.lnf, params.wte, NULL, B, T, C, V);
    softmax_forward(acts.probs, acts.logits, B, T, V);
}

void gpt2_zero_grad(GPT2 *model) {
    if (model->grads_memory != NULL) { memset(model->grads_memory, 0, model->num_parameters * sizeof(float)); }
    if (model->grads_acts_memory != NULL) { memset(model->grads_acts_memory, 0, model->num_activations * sizeof(float)); }
}

void gpt2_free(GPT2 *model) {
    free(model->params_memory);
    free(model->grads_memory);
    free(model->m_memory);
    free(model->v_memory);
    free(model->acts_memory);
    free(model->grads_acts_memory);
    free(model->inputs);
    free(model->targets);
}

// 按概率分布采样下一个 token（框架给定，固定 coin=0.5，保证可复现）
int sample_mult(float *probabilities, int n) {
    float cdf = 0.0f, coin = 0.5f;
    for (int i = 0; i < n; i++) {
        cdf += probabilities[i];
        if (coin < cdf) {
            return i;
        }
    }
    return n - 1; // 舍入误差兜底
}

// GPT-2 的 end-of-text token id
#define GPT2_EOT 50256

int main(int argc, char **argv) {
    // 加载模型（这一阶段以磁盘 I/O 为主，不属于"推理"本身，
    // 评测的加速比也明确排除模型加载时间，因此保持串行）
    GPT2 model;
    gpt2_build_from_checkpoint(&model, "gpt2_124M.bin");

    // 创建 4 个常驻 worker 线程，之后所有层都通过它们并行计算
    parallel_init();

    const int n = 10; // 总共输出 n 个 token（含输入）

    // ---- 解析命令行 token ----
    // 正常用法是 ./gpt <token1> <token2> ...：argv[0] 是程序名，token 从 argv[1] 起。
    // 但 testkit 评测时是直接调用 main(argc, argv)，argv 里没有程序名占位——
    // 此时 argv[0] 本身就是第一个 token。这里通过"argv[0] 是不是纯数字"来区分
    // 这两种情况，从而两种调用方式都能得到正确结果。
    int first = 1; // token 从 argv[first] 开始
    if (argc > 1) {
        // 只关心"argv[0] 能否被完整解析为一个整数"，返回值本身用不到
        char *end = NULL;
        strtol(argv[0], &end, 10);
        if (end != argv[0] && *end == '\0') first = 0; // argv[0] 是整数 → 它也是 token
    }
    int ntokens = argc - first; // 实际给出的 token 个数

    if (ntokens == 0) {
        printf("Provide at least one token.\n");
        exit(1);
    }
    if (ntokens >= n) {
        printf("Too many tokens.\n");
        exit(1);
    }

    int tokens[n];
    for (int i = 0; i < n; i++) {
        if (i < ntokens) {
            tokens[i] = strtol(argv[first + i], NULL, 10);
        } else {
            tokens[i] = GPT2_EOT; // 不够的位置用 <|endoftext|> 补齐
        }
    }

    // ---- 自回归生成 ----
    // 每一步只看前面 t 个 token，取最后一个位置的概率分布采样下一个 token。
    // 这一步是严格串行的——下一个 token 依赖前一个 token 的输出。
    // 我们的并行度全部来自"单次前向内部的逐层并行"。
    for (int t = ntokens; t < n; t++) {
        gpt2_forward(&model, tokens, 1, t);
        float *probs = model.acts.probs + (t - 1) * model.config.vocab_size;
        int next_token = sample_mult(probs, model.config.vocab_size);
        tokens[t] = next_token;

        printf("%d\n", tokens[t]);
        fflush(stdout);
    }

    gpt2_free(&model);

    return 0;
}
