#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ==================== 宏定义和常量 ==================== */
#define MAX_SYSCALLS 1024      // 最多支持的不同系统调用数量
#define TOP_N 5                // 默认显示排名前5的系统调用
#define MAX_LINE 4096          // strace输出行的最大长度
#define MAX_SYSCALL_NAME 128   // 系统调用名称的最大长度

/* ==================== 数据结构定义 ==================== */

/**
 * 单个系统调用的统计信息
 */
typedef struct {
    char name[64];             // 系统调用名称 (例如: "read", "write", "open")
    long count;                // 该系统调用被调用的次数
    double time;               // 该系统调用花费的总时间(微秒)
    double max_time;           // 单次调用的最大时间
    double min_time;           // 单次调用的最小时间
} syscall_stat;

/**
 * 全局的系统调用统计集合
 */
typedef struct {
    syscall_stat stats[MAX_SYSCALLS];  // 系统调用统计数组
    int count;                          // 当前统计的不同系统调用数量
    double total_time;                  // 所有系统调用的总时间
    long total_calls;                   // 所有系统调用的总次数
} syscall_stats;

/* ==================== 帮助和版本信息 ==================== */

/**
 * 打印使用说明
 */
void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Analyze strace output and profile system calls.\n\n");
    printf("Options:\n");
    printf("  -n N, --top N         Show top N system calls (default: %d)\n", TOP_N);
    printf("  -t, --time            Sort by total time (default: count)\n");
    printf("  -s, --sort            Sort by system call name\n");
    printf("  -h, --help            Show this help message\n");
    printf("\nInput format:\n");
    printf("  Accepts strace output on stdin or from file\n");
    printf("  Example: strace -e trace=all -c <command> 2>&1 | %s\n", prog);
}

/**
 * 打印版本信息
 */
void print_version(void) {
    printf("sperf version 1.0.0 - System Call Profiler\n");
}

/* ==================== 字符串处理工具函数 ==================== */

/**
 * trim_whitespace - 移除字符串两端的空白字符
 * @str: 要修剪的字符串
 *
 * 返回值：指向修剪后字符串开始的指针
 *
 * 注意：修改了原字符串，用'\0'替换了末尾的空白
 */
static char *trim_whitespace(char *str) {
    // 跳过前导空白
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }

    // 找到字符串末尾并移除末尾空白
    char *end = str + strlen(str) - 1;
    while (end >= str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return str;
}

/**
 * find_char_outside_quotes - 在字符串中查找指定字符，忽略引号内的字符
 * @str: 要搜索的字符串
 * @ch: 要查找的字符
 *
 * 返回值：找到的位置指针，未找到返回NULL
 *
 * 用途：在strace输出中正确定位分隔符，避免字符串参数中的分隔符干扰
 */
static char *find_char_outside_quotes(const char *str, char ch) {
    int in_quotes = 0;
    for (const char *p = str; *p; p++) {
        if (*p == '"' || *p == '\'') {
            in_quotes = !in_quotes;
        } else if (*p == ch && !in_quotes) {
            return (char *)p;
        }
    }
    return NULL;
}

/* ==================== strace输出解析函数 ==================== */

/**
 * parse_strace_line - 解析strace输出中的一行
 * @line: strace输出的一行
 * @syscall_name: 输出参数，存储解析出的系统调用名称
 * @time: 输出参数，存储解析出的执行时间（微秒）
 *
 * 返回值：成功返回1，失败返回0
 *
 * strace输出格式示例1（-e trace=all）：
 *   read(3, "\x7fELF\x02\x01\x01", 832) = 832 <0.000012>
 *   write(1, "hello\n", 6)              = 6 <0.000008>
 *
 * strace输出格式示例2（-c 摘要）：
 *   % time     seconds  usecs/call     calls    errors name
 *   ------ ----------- ----------- --------- --------- ----
 *    75.00    0.001500          30        50           read
 *    25.00    0.000500          10        50           write
 *
 * 本函数处理格式1（详细跟踪模式）
 */
int parse_strace_line(char *line, char *syscall_name, double *time) {
    if (!line || !syscall_name || !time) {
        return 0;
    }

    // 跳过空行和注释
    char *trimmed = trim_whitespace(line);
    if (strlen(trimmed) == 0 || trimmed[0] == '#') {
        return 0;
    }

    // 提取系统调用名称
    // strace输出格式：syscall_name(...)
    char *paren = strchr(trimmed, '(');
    if (!paren) {
        return 0;
    }

    // 计算系统调用名称的长度
    int name_len = paren - trimmed;
    if (name_len <= 0 || name_len >= MAX_SYSCALL_NAME) {
        return 0;
    }

    // 复制系统调用名称
    strncpy(syscall_name, trimmed, name_len);
    syscall_name[name_len] = '\0';

    // 验证系统调用名称是否合法（只包含字母、数字、下划线）
    for (int i = 0; i < name_len; i++) {
        if (!isalnum((unsigned char)syscall_name[i]) && syscall_name[i] != '_') {
            return 0;
        }
    }

    // 查找时间信息
    // strace输出格式：... <0.000012>
    // 时间信息被尖括号包围
    char *time_start = strchr(paren, '<');
    if (!time_start) {
        // 如果找不到时间信息，尝试从摘要格式解析
        // 这种情况下我们无法准确获取时间，返回失败
        return 0;
    }

    time_start++;  // 跳过'<'
    char *time_end = strchr(time_start, '>');
    if (!time_end) {
        return 0;
    }

    // 提取时间字符串
    int time_len = time_end - time_start;
    if (time_len <= 0 || time_len >= 64) {
        return 0;
    }

    char time_str[64];
    strncpy(time_str, time_start, time_len);
    time_str[time_len] = '\0';

    // 解析时间值（秒），转换为微秒
    char *endptr;
    double time_seconds = strtod(time_str, &endptr);
    if (endptr == time_str) {
        // 字符串无法转换为数字
        return 0;
    }

    // 转换为微秒（1秒 = 1,000,000微秒）
    *time = time_seconds * 1000000.0;

    return 1;
}

/* ==================== 系统调用统计管理函数 ==================== */

/**
 * find_syscall - 在统计集合中查找指定名称的系统调用
 * @stats: 统计集合
 * @name: 要查找的系统调用名称
 *
 * 返回值：找到则返回索引，未找到返回-1
 */
static int find_syscall(syscall_stats *stats, const char *name) {
    for (int i = 0; i < stats->count; i++) {
        if (strcmp(stats->stats[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * add_syscall - 向统计集合中添加或更新一个系统调用的统计信息
 * @stats: 统计集合
 * @name: 系统调用名称
 * @time: 此次调用耗时（微秒）
 *
 * 功能：
 * - 如果该系统调用已存在，更新其统计数据
 * - 如果该系统调用不存在，添加新条目
 * - 更新总时间和总调用次数
 */
void add_syscall(syscall_stats *stats, const char *name, double time) {
    if (!stats || !name) {
        return;
    }

    // 查找是否已存在该系统调用
    int idx = find_syscall(stats, name);

    if (idx >= 0) {
        // 更新现有的系统调用统计
        syscall_stat *stat = &stats->stats[idx];
        stat->count++;
        stat->time += time;

        // 更新最大和最小执行时间
        if (time > stat->max_time) {
            stat->max_time = time;
        }
        if (time < stat->min_time) {
            stat->min_time = time;
        }
    } else {
        // 添加新的系统调用统计
        if (stats->count >= MAX_SYSCALLS) {
            // 已达到最大系统调用数，无法添加更多
            return;
        }

        idx = stats->count;
        syscall_stat *stat = &stats->stats[idx];
        strncpy(stat->name, name, sizeof(stat->name) - 1);
        stat->name[sizeof(stat->name) - 1] = '\0';
        stat->count = 1;
        stat->time = time;
        stat->max_time = time;
        stat->min_time = time;

        stats->count++;
    }

    // 更新全局统计
    stats->total_time += time;
    stats->total_calls++;
}

/* ==================== 排序和输出函数 ==================== */

/**
 * 比较函数：按调用次数降序排序
 */
static int cmp_count(const void *a, const void *b) {
    const syscall_stat *sa = (const syscall_stat *)a;
    const syscall_stat *sb = (const syscall_stat *)b;
    // 降序排列
    if (sb->count != sa->count) {
        return sb->count - sa->count;
    }
    // 次数相同时按名称排序
    return strcmp(sa->name, sb->name);
}

/**
 * 比较函数：按总时间降序排序
 */
static int cmp_time(const void *a, const void *b) {
    const syscall_stat *sa = (const syscall_stat *)a;
    const syscall_stat *sb = (const syscall_stat *)b;
    // 降序排列
    if (sb->time != sa->time) {
        return (sb->time > sa->time) ? 1 : -1;
    }
    // 时间相同时按名称排序
    return strcmp(sa->name, sb->name);
}

/**
 * 比较函数：按名称升序排序
 */
static int cmp_name(const void *a, const void *b) {
    const syscall_stat *sa = (const syscall_stat *)a;
    const syscall_stat *sb = (const syscall_stat *)b;
    return strcmp(sa->name, sb->name);
}

/**
 * print_top_syscalls - 打印排名前N的系统调用统计信息
 * @stats: 系统调用统计集合
 * @n: 要打印的系统调用数量（TOP N）
 * @sort_by: 排序方式 (0=count, 1=time, 2=name)
 *
 * 功能：
 * 1. 将统计数据按指定方式排序
 * 2. 打印表头
 * 3. 打印前N个系统调用的详细信息
 * 4. 打印统计摘要
 */
void print_top_syscalls(syscall_stats *stats, int n, int sort_by) {
    if (!stats || stats->count == 0) {
        printf("No system calls recorded.\n");
        return;
    }

    // 限制要打印的行数
    if (n <= 0 || n > stats->count) {
        n = stats->count;
    }

    // 创建临时副本用于排序（避免破坏原始数据）
    syscall_stat *sorted = malloc(stats->count * sizeof(syscall_stat));
    if (!sorted) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    memcpy(sorted, stats->stats, stats->count * sizeof(syscall_stat));

    // 根据排序方式选择比较函数
    switch (sort_by) {
        case 1:  // 按时间排序
            qsort(sorted, stats->count, sizeof(syscall_stat), cmp_time);
            break;
        case 2:  // 按名称排序
            qsort(sorted, stats->count, sizeof(syscall_stat), cmp_name);
            break;
        case 0:  // 默认按调用次数排序
        default:
            qsort(sorted, stats->count, sizeof(syscall_stat), cmp_count);
    }

    // 打印表头
    printf("\n");
    printf("%-20s %12s %12s %12s %12s %12s\n",
           "syscall", "calls", "total(us)", "avg(us)", "max(us)", "min(us)");
    printf("%-20s %12s %12s %12s %12s %12s\n",
           "--------------------", "------------", "------------",
           "------------", "------------", "------------");

    // 打印前N个系统调用
    for (int i = 0; i < n; i++) {
        syscall_stat *stat = &sorted[i];
        double avg_time = stat->count > 0 ? stat->time / stat->count : 0;

        printf("%-20s %12ld %12.2f %12.2f %12.2f %12.2f\n",
               stat->name,
               stat->count,
               stat->time,
               avg_time,
               stat->max_time,
               stat->min_time);
    }

    // 打印统计摘要
    printf("%-20s %12s %12s %12s\n",
           "--------------------", "------------", "------------", "------------");

    double avg_time_all = stats->total_calls > 0 ? stats->total_time / stats->total_calls : 0;
    printf("%-20s %12ld %12.2f %12.2f\n",
           "total",
           stats->total_calls,
           stats->total_time,
           avg_time_all);

    printf("\nTotal unique system calls: %d\n", stats->count);
    printf("\n");

    free(sorted);
}

/* ==================== 主函数 ==================== */

int main(int argc, char *argv[]) {
    int top_n = TOP_N;         // 默认显示前5个系统调用
    int sort_by = 0;           // 默认按调用次数排序 (0=count, 1=time, 2=name)

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--top") == 0) {
            // 获取TOP N的参数
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -n option requires an argument\n");
                print_usage(argv[0]);
                return 1;
            }
            top_n = atoi(argv[++i]);
            if (top_n <= 0) {
                fprintf(stderr, "Error: top_n must be positive\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--time") == 0) {
            // 按时间排序
            sort_by = 1;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--sort") == 0) {
            // 按名称排序
            sort_by = 2;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    // 初始化统计结构
    syscall_stats stats = {0};
    stats.total_time = 0.0;
    stats.total_calls = 0;
    stats.count = 0;

    // 读取strace输出并解析
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char syscall_name[MAX_SYSCALL_NAME];
        double time;

        // 解析当前行
        if (parse_strace_line(line, syscall_name, &time)) {
            // 添加到统计集合
            add_syscall(&stats, syscall_name, time);
        }
    }

    // 打印结果
    if (stats.count > 0) {
        print_top_syscalls(&stats, top_n, sort_by);
    } else {
        printf("No system calls found in input.\n");
        printf("Please provide strace output on stdin.\n");
        printf("Example: strace -e trace=all <command> 2>&1 | sperf\n");
    }

    return 0;
}
