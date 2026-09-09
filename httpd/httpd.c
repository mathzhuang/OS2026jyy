/*
 * M7: HTTP Daemon (httpd)
 * -----------------------------------------------
 * 一个支持多线程的 HTTP 服务器：
 *   1. 对 /cgi-bin/ 开头的 URL 执行 CGI 程序，返回动态内容；
 *   2. 并发到达的请求被尽快接收、并行处理，但并行执行的请求数不超过 4；
 *   3. 日志严格按"请求到达顺序"输出到标准输出。
 *
 * 整体架构（生产者 - 消费者）：
 *   - 主线程 = 生产者：只负责 accept() 新连接，为每个连接分配一个"到达序号"
 *     (seq)，然后把请求放入一个**有界 FIFO 队列**。主线程从不阻塞在慢速的
 *     CGI 执行上，因此能"尽快接收"并发到达的连接。
 *   - 4 个 worker 线程 = 消费者：从队列取一个请求，解析 HTTP 请求、执行
 *     CGI / 返回错误页、把响应写回客户端，最后把日志按 seq 顺序打印出来。
 *   - "按到达顺序打印日志"用一个序号环形缓冲 + 条件变量实现：每个请求处理
 *     完之后必须等 seq 比它小的请求都打印完毕，才允许打印自己。这样即使
 *     后到的请求先执行完，日志顺序也严格等于到达顺序。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>

// Don't include these in another file.
#include "thread.h"
#include "thread-sync.h"

/* 框架提供的日志函数，实现在文件末尾；在这里先声明以允许前向引用。 */
void log_request(const char *method, const char *path, int status_code);

/* ====================================================================
 * 配置常量
 * ================================================================== */

#define BUFFER_SIZE      4096     /* 读取 HTTP 请求头的缓冲区大小        */
#define MAX_PATH_LENGTH  1024     /* 路径 / 脚本名 / query 的最大长度     */
#define DEFAULT_PORT     8080     /* 默认监听端口                        */
#define NUM_WORKERS      4        /* 并行执行请求的 worker 线程数（上限）*/
#define MAX_QUEUE        64       /* 待处理请求队列容量（有界）          */
#define LOG_WINDOW       256      /* 日志序号环形缓冲大小                */
#define MAX_CGI_OUTPUT   (1 << 22)/* 捕获 CGI 输出的上限（4 MB）         */
#define RESPONSE_TIMEOUT 30       /* 客户端 socket 读写超时（秒）        */

/* ====================================================================
 * 请求描述结构体
 *
 * 主线程 accept 后只填充 socket 和 seq 就立刻入队；
 * method / path / script / query 由 worker 在解析时填充。
 * ================================================================== */

struct request {
    int socket;                    /* 客户端 socket 文件描述符           */
    int seq;                       /* 到达序号（0, 1, 2, ...），日志排序依据 */
    char method[16];               /* 请求方法，如 GET                   */
    char path[MAX_PATH_LENGTH];    /* 请求路径（不含 query string），日志用 */
    char script[MAX_PATH_LENGTH];  /* 要执行的 CGI 脚本相对路径 cgi-bin/xxx */
    char query[MAX_PATH_LENGTH];   /* query string（URL 中 '?' 之后的部分） */
    int  status;                   /* 处理完成后得到的 HTTP 状态码       */
};

/* ====================================================================
 * 工具函数
 * ================================================================== */

/* 把 len 字节全部写入 socket（处理部分写、被信号打断等情况）。
 * MSG_NOSIGNAL：避免对端断开时触发 SIGPIPE 杀死整个进程
 *（虽然 main 里也设了 SIG_IGN，双保险）。 */
static void write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;          /* 被信号打断，重试 */
            break;                 /* 连接已断开，放弃剩余数据 */
        }
        p += n;
        len -= (size_t)n;
    }
}

/* 发送一个由服务器自己构造的简单响应（404 / 500 / 400 ...）。
 * CGI 程序负责输出它自己的完整 HTTP 响应；而"脚本不存在"、
 * "脚本执行失败"这类错误则由服务器兜底生成响应。 */
static void send_simple_response(int fd, int code, const char *reason,
                                 const char *body) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, reason, strlen(body));
    write_all(fd, header, (size_t)hlen);
    write_all(fd, body, strlen(body));
}

/* 从 CGI 输出的第一行解析 HTTP 状态码。
 *   行形如 "HTTP/1.1 200 OK" / "HTTP/1.0 403 Forbidden"
 * 若行不以 "HTTP/" 开头或没有合法的三位状态码，返回 -1，
 * 由调用方决定默认值。 */
static int parse_status_line(const char *line) {
    if (strncmp(line, "HTTP/", 5) != 0)
        return -1;
    const char *p = strchr(line, ' ');
    if (!p)
        return -1;
    p++;                           /* 跳过空格，指向状态码 */
    if (!isdigit((unsigned char)p[0]) ||
        !isdigit((unsigned char)p[1]) ||
        !isdigit((unsigned char)p[2]))
        return -1;
    return (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
}

/* ====================================================================
 * 有界 FIFO 请求队列（生产者-消费者同步）
 *
 * q_not_empty / q_not_full 两个条件变量分别表示"有请求可取"和
 * "有空位可放"，配合互斥锁保证队列操作原子、阻塞时正确睡眠。
 * 主线程（生产者）入队，worker（消费者）出队。
 * ================================================================== */

static struct request queue[MAX_QUEUE];
static int q_head = 0, q_tail = 0, q_count = 0;
static pthread_mutex_t q_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  q_not_full  = PTHREAD_COND_INITIALIZER;

/* 入队：若队列满则阻塞等待空位（背压，保护有界队列）。 */
static void enqueue(struct request *req) {
    pthread_mutex_lock(&q_mutex);
    while (q_count == MAX_QUEUE)
        pthread_cond_wait(&q_not_full, &q_mutex);
    queue[q_tail] = *req;
    q_tail = (q_tail + 1) % MAX_QUEUE;
    q_count++;
    pthread_cond_signal(&q_not_empty);
    pthread_mutex_unlock(&q_mutex);
}

/* 出队：若队列空则阻塞等待新请求。 */
static void dequeue(struct request *req) {
    pthread_mutex_lock(&q_mutex);
    while (q_count == 0)
        pthread_cond_wait(&q_not_empty, &q_mutex);
    *req = queue[q_head];
    q_head = (q_head + 1) % MAX_QUEUE;
    q_count--;
    pthread_cond_signal(&q_not_full);
    pthread_mutex_unlock(&q_mutex);
}

/* ====================================================================
 * 按"到达顺序"输出的日志
 *
 * 核心难点：请求是并行处理的，先到的请求可能后完成，但日志必须严格按
 * 到达顺序打印。做法：
 *   - 每个请求在 accept 时拿到全局递增的 seq（即到达序号）；
 *   - 日志槽位按 seq % LOG_WINDOW 索引（环形缓冲）；
 *   - worker 处理完一个请求后调用 log_finish()：先把结果写进自己的槽位，
 *     然后**阻塞等待** seq 小于自己的请求都打印完（log_next 前进到自己），
 *     再打印自己，并顺带把后面所有"已完成的连续请求"一并打印。
 *
 * 为什么阻塞等待不会死锁：worker 出队是 FIFO 的，所以"seq 最小的未完成
 * 请求"一定正在某个 worker 上执行（而不会孤零零躺在队列里）。等它执行
 * 完、打印完，就会唤醒所有等待者，形成前进步伐。由于 worker 打印完才去
 * 取下一个请求，"已接收但未打印"的请求数被队列容量 + worker 数严格限制
 * （<= MAX_QUEUE + NUM_WORKERS），小于 LOG_WINDOW，环形缓冲不会撞车。
 * ================================================================== */

struct log_entry {
    int seq;                       /* 到达序号                      */
    char method[16];               /* 请求方法                      */
    char path[MAX_PATH_LENGTH];    /* 请求路径                      */
    int  status;                   /* 状态码                        */
    int  done;                     /* 1 = 已处理完、等待打印        */
};

static struct log_entry logs[LOG_WINDOW];
static int log_next = 0;                       /* 下一个待打印的 seq */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  log_cond  = PTHREAD_COND_INITIALIZER;

/* 前提：持有 log_mutex。
 * 按 seq 递增顺序打印所有"连续已完成的"日志条目，并唤醒其他等待者。 */
static void log_flush_locked(void) {
    while (1) {
        struct log_entry *e = &logs[log_next % LOG_WINDOW];
        if (!e->done || e->seq != log_next)
            break;                 /* 下一个还没就绪，停止 */
        /* 这就是框架提供的 log_request，内部已调用 fflush(stdout)。
         * 在 log_mutex 保护下调用，保证多线程日志不交错。 */
        log_request(e->method, e->path, e->status);
        e->done = 0;               /* 槽位归还，可被复用 */
        log_next++;
    }
    pthread_cond_broadcast(&log_cond);         /* 唤醒正在等待打印的 worker */
}

/* worker 在处理完一个请求后调用：登记结果并确保日志按序输出。 */
static void log_finish(struct request *req) {
    pthread_mutex_lock(&log_mutex);

    struct log_entry *e = &logs[req->seq % LOG_WINDOW];
    /* 防御：若该槽位已有未打印的其它 seq 条目，说明环形缓冲溢出（理论
     * 上不会发生，因为并发在途请求数被队列上限约束）。 */
    assert(!e->done || e->seq == req->seq);
    e->seq = req->seq;
    e->done = 1;
    e->status = req->status;
    /* req->method / req->path 均保证以 NUL 结尾且不超过目标长度，
     * 直接 strcpy 安全。 */
    strcpy(e->method, req->method);
    strcpy(e->path,   req->path);

    /* 阻塞等待轮到自己（seq 小于自己的请求都打印完）。 */
    while (req->seq != log_next) {
        log_flush_locked();        /* 先把前面已完成的尽量打印 */
        if (req->seq == log_next)
            break;
        pthread_cond_wait(&log_cond, &log_mutex);   /* 释放锁等待唤醒 */
    }

    /* 轮到了：打印自己，并级联打印后续已完成的请求。 */
    log_flush_locked();
    pthread_mutex_unlock(&log_mutex);
}

/* ====================================================================
 * CGI 执行
 *
 * 步骤：
 *   1. 检查脚本是否存在（不存在 → 404）、是否可执行（不可执行 → 500）；
 *   2. pipe() 建一条管道，fork() 出子进程；
 *   3. 子进程：stdout 重定向到管道写端（父进程捕获）、stdin 重定向到
 *      客户端 socket（CGI 可读取请求体）、stderr 丢弃，设置环境变量
 *      REQUEST_METHOD / QUERY_STRING，然后 exec 脚本；
 *   4. 父进程：从管道读子进程的完整输出（这就是 CGI 生成的 HTTP 响应），
 *      从第一行解析出状态码，再原样转发给客户端；最后 waitpid 回收子进程。
 *
 * 状态码的约定（与实验文档一致）：
 *   - 脚本不存在 / 路径非法 → 404（服务器生成响应）
 *   - 脚本存在但不可执行、fork/pipe 失败 → 500
 *   - 脚本正常输出带 "HTTP/1.x XXX" 状态行的响应 → 转发并记录 XXX
 *   - 脚本输出不带状态行（如直接以 "Content-Type: ..." 开头）→ 视为 200
 *   - 脚本什么都没输出 → 500
 * ================================================================== */

static int run_cgi(struct request *req) {
    int fd = req->socket;

    /* ---- 脚本存在性 / 可执行性检查 ---- */
    if (access(req->script, F_OK) != 0) {
        send_simple_response(fd, 404, "404 Not Found", "404 Not Found");
        return 404;
    }
    if (access(req->script, X_OK) != 0) {
        send_simple_response(fd, 500, "500 Internal Server Error",
                             "500 Internal Server Error");
        return 500;
    }

    /* ---- 建管道捕获 CGI 输出 ---- */
    int fds[2];
    if (pipe(fds) < 0) {
        send_simple_response(fd, 500, "500 Internal Server Error",
                             "500 Internal Server Error");
        return 500;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        send_simple_response(fd, 500, "500 Internal Server Error",
                             "500 Internal Server Error");
        return 500;
    }

    if (pid == 0) {
        /* ================== 子进程 ================== */
        close(fds[0]);                        /* 不需要管道读端 */

        /* stdout → 管道写端：CGI 的完整 HTTP 响应由父进程捕获后转发 */
        dup2(fds[1], STDOUT_FILENO);
        /* stdin ← 客户端 socket：CGI 可以读到请求体（如 POST 数据） */
        dup2(fd, STDIN_FILENO);
        /* stderr → /dev/null：避免错误信息污染被转发的响应 */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(fds[1]);                        /* dup2 之后的原始写端 */
        close(fd);                            /* stdin 已别名该 socket */

        /* 通过环境变量把请求信息传给 CGI（实验只要求这两个） */
        setenv("REQUEST_METHOD", req->method, 1);
        setenv("QUERY_STRING",   req->query,  1);

        /* argv[0] 使用脚本名（去掉路径前缀） */
        const char *name = strrchr(req->script, '/');
        name = name ? name + 1 : req->script;
        execl(req->script, name, (char *)NULL);

        /* exec 失败（例如脚本缺执行权限、解释器缺失）：向管道写一个
         * 500 响应，父进程会把它转发给客户端。 */
        dprintf(STDOUT_FILENO,
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 21\r\n"
                "Connection: close\r\n"
                "\r\n"
                "500 Internal Server Error");
        _exit(126);
    }

    /* ================== 父进程 ================== */
    close(fds[1]);                            /* 不需要写端 */

    /* 读完 CGI 的全部输出（上限 MAX_CGI_OUTPUT，防止恶意脚本吃内存） */
    char *output = NULL;
    size_t out_len = 0, out_cap = 0;
    char rbuf[8192];
    ssize_t n;

    while ((n = read(fds[0], rbuf, sizeof(rbuf))) > 0) {
        /* 确保 out_len + n + 1（留一个 '\0' 的位置）放得下。
         * 注意：单次 read 可能一次返回整个管道缓冲（64KB），所以扩容
         * 必须直接扩到 out_len + n + 1，而不能只做小步倍增——否则容量
         * 不足会把大输出误判成"空输出"而返回 500。 */
        size_t need = out_len + (size_t)n + 1;
        if (need > out_cap) {
            size_t ncap = need;
            if (ncap > MAX_CGI_OUTPUT)
                ncap = MAX_CGI_OUTPUT;
            if (ncap < need) {
                close(fds[0]);                /* 超出上限：截断，通知子进程 */
                break;
            }
            char *np = (char *)realloc(output, ncap);
            if (!np) {
                close(fds[0]);                /* 内存不足：同样截断 */
                break;
            }
            output = np;
            out_cap = ncap;
        }
        memcpy(output + out_len, rbuf, (size_t)n);
        out_len += (size_t)n;
    }

    int status;
    if (out_len > 0) {
        output[out_len] = '\0';

        /* 提取第一行（不修改 output 内容）用于解析状态码 */
        char first_line[1024];
        size_t fl = 0;
        for (size_t i = 0; i < out_len && fl < sizeof(first_line) - 1; i++) {
            if (output[i] == '\n')
                break;
            first_line[fl++] = output[i];
        }
        first_line[fl] = '\0';
        while (fl > 0 && (first_line[fl - 1] == '\r' || first_line[fl - 1] == '\n'))
            first_line[--fl] = '\0';

        status = parse_status_line(first_line);
        if (status < 0) {
            /* 输出不以状态行开头（如直接 "Content-Type: ..."）：补一个
             * HTTP/1.1 200 OK，与实验文档给出的例子行为一致。 */
            status = 200;
            write_all(fd, "HTTP/1.1 200 OK\r\n", 17);
        }
        /* 原样转发 CGI 的响应体 */
        write_all(fd, output, out_len);
    } else {
        /* CGI 没有输出任何内容：视为执行失败 → 500 */
        status = 500;
        send_simple_response(fd, 500, "500 Internal Server Error",
                             "500 Internal Server Error");
    }

    free(output);
    close(fds[0]);

    /* 回收子进程（若 CGI 运行时间很长，worker 在此阻塞等待——这符合
     * 评测场景：评测时 CGI 脚本的执行时间可能很长）。
     * 被信号打断时（EINTR）重试，避免子进程残留为僵尸。 */
    int wstatus;
    while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR)
        ;
    return status;
}

/* ====================================================================
 * 处理单个 HTTP 请求
 *
 * 1. 读取请求头（直到 \r\n\r\n）；
 * 2. 解析请求行 "METHOD SP TARGET SP VERSION"；
 * 3. 把 TARGET 拆成 path 和 query string；
 * 4. 若 path 以 /cgi-bin/ 开头且合法 → 执行 CGI；否则返回 404。
 * ================================================================== */

static int handle_request(struct request *req) {
    int fd = req->socket;
    char buf[BUFFER_SIZE];

    /* 默认值：保证解析失败时日志内容也是有定义的 */
    strcpy(req->method, "?");
    strcpy(req->path, "?");
    req->query[0] = '\0';

    /* ---- 读取请求头，直到遇见 \r\n\r\n 或缓冲区满 ---- */
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + total, (size_t)(sizeof(buf) - 1 - total), 0);
        if (n <= 0) {
            if (total == 0)
                return 400;        /* 对端关闭 / 超时，没有任何请求数据 */
            break;
        }
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL)
            break;                 /* 头部结束 */
    }
    if (total <= 0)
        return 400;

    /* ---- 解析请求行 ---- */
    char *line = buf;
    char *line_end = strstr(line, "\r\n");
    if (!line_end)
        line_end = strchr(line, '\n');
    if (!line_end) {
        send_simple_response(fd, 400, "400 Bad Request", "400 Bad Request");
        return 400;
    }
    *line_end = '\0';

    char *saveptr;
    char *method = strtok_r(line, " ", &saveptr);
    char *target = strtok_r(NULL, " ", &saveptr);
    if (!method || !target) {
        send_simple_response(fd, 400, "400 Bad Request", "400 Bad Request");
        return 400;
    }
    snprintf(req->method, sizeof(req->method), "%s", method);

    /* ---- 把 TARGET 拆成 path 和 query string ---- */
    char *path = target;
    char *qmark = strchr(path, '?');
    if (qmark) {
        *qmark = '\0';
        snprintf(req->query, sizeof(req->query), "%s", qmark + 1);
    } else {
        req->query[0] = '\0';
    }
    snprintf(req->path, sizeof(req->path), "%s", path);

    /* ---- 分发 ---- */
    if (strncmp(path, "/cgi-bin/", 9) == 0) {
        const char *name = path + 9;
        /* 路径合法性检查：不允许为空、含 '/' 或 ".."（防目录穿越，
         * 例如 /cgi-bin/../../etc/passwd）。 */
        if (name[0] == '\0' || strchr(name, '/') != NULL ||
            strstr(name, "..") != NULL) {
            send_simple_response(fd, 404, "404 Not Found", "404 Not Found");
            return 404;
        }
        snprintf(req->script, sizeof(req->script), "cgi-bin/%s", name);
        return run_cgi(req);
    } else {
        /* 非 /cgi-bin/ 路径：视为非法路径，返回 404 */
        send_simple_response(fd, 404, "404 Not Found", "404 Not Found");
        return 404;
    }
}

/* ====================================================================
 * worker 线程主循环
 *
 * 不断从队列取一个请求 → 处理 → 按序打印日志 → 关闭连接。
 * 由于只有 NUM_WORKERS(=4) 个 worker，任何时刻并行执行的请求数
 * 一定不超过 4。
 * ================================================================== */

static void *worker_main(void *arg) {
    (void)arg;
    while (1) {
        struct request req;
        dequeue(&req);                       /* 没有请求时在此阻塞 */
        req.status = handle_request(&req);
        log_finish(&req);                    /* 按到达顺序打印日志 */
        close(req.socket);
    }
    return NULL;
}

/* ====================================================================
 * main：socket 初始化 + 创建 worker 池 + 主线程 accept 循环
 * ================================================================== */

int main(int argc, char *argv[]) {
    /* Socket 变量 */
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;

    /* 从命令行取端口，默认 8080 */
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;

    /* 客户端中途断开（RST）会触发 SIGPIPE，忽略它以让 send/recv
     * 返回错误而不是杀死整个服务器进程 */
    signal(SIGPIPE, SIG_IGN);

    /* 创建 socket */
    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    /* 允许地址重用：服务器重启后不会报 "Address already in use" */
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt failed");
        exit(EXIT_FAILURE);
    }

    /* 配置服务器地址 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         /* IPv4 */
    server_addr.sin_addr.s_addr = INADDR_ANY; /* 监听所有网卡 */
    server_addr.sin_port = htons((uint16_t)port);  /* 转网络字节序 */

    /* 绑定地址和端口 */
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    /* 开始监听，backlog 用系统上限 */
    if (listen(server_socket, SOMAXCONN) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", port);

    /* 创建 NUM_WORKERS 个 worker 线程。用裸 pthread_create 而非框架
     * 提供的 spawn()：因为服务器永不退出，不需要 join() 等待线程结束。 */
    pthread_t workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (pthread_create(&workers[i], NULL, worker_main, NULL) != 0) {
            perror("pthread_create failed");
            exit(EXIT_FAILURE);
        }
    }

    /* 主线程 = 生产者：只负责 accept 并立刻入队，绝不在慢速 CGI 上阻塞。
     * 每个连接在 accept 时分配全局递增的 seq，即"到达序号"。 */
    int seq_counter = 0;
    while (1) {
        client_len = sizeof(client_addr);
        if ((client_socket = accept(server_socket,
                                    (struct sockaddr *)&client_addr,
                                    &client_len)) < 0) {
            if (errno == EINTR)
                continue;                    /* 被信号打断，继续 accept */
            perror("Accept failed");
            continue;
        }

        /* 读写超时，防止慢速 / 死掉的连接长时间占用 worker */
        struct timeval timeout;
        timeout.tv_sec = RESPONSE_TIMEOUT;
        timeout.tv_usec = 0;
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&timeout, sizeof(timeout));
        setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO,
                   (const char *)&timeout, sizeof(timeout));

        /* 构造请求并入队；worker 会解析真正的 method/path/query */
        struct request req;
        memset(&req, 0, sizeof(req));        /* 先清零，未填充的字段确定 */
        req.socket = client_socket;
        req.seq = seq_counter++;             /* 主线程独占，无需加锁 */
        enqueue(&req);
    }

    /* 不可达：服务器是无限循环 */
    close(server_socket);
    return 0;
}

/* ====================================================================
 * 框架要求保留的 log_request
 * ================================================================== */

void log_request(const char *method, const char *path, int status_code) {
    time_t now;
    struct tm *tm_info;
    char timestamp[26];

    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, 26, "%Y-%m-%d %H:%M:%S", tm_info);

    /* In real systems, we write to a log file,
     * like /var/log/nginx/access.log */
    printf("[%s] [%s] [%s] [%d]\n", timestamp, method, path, status_code);
    fflush(stdout);
}
