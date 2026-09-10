#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>

#define PSTREE_VERSION "1.0.0"

void print_usage(const char *prog) {
    printf("Usage: %s [OPTION]...\n", prog);
    printf("Print the process tree for the current process.\n\n");
    printf("Options:\n");
    printf("  -p, --show-pids       Show process IDs\n");
    printf("  -n, --numeric-sort    Sort children by PID\n");
    printf("  -V, --version         Show version information\n");
    printf("  -h, --help            Show this help message\n");
}

void print_version(void) {
    printf("pstree version %s\n", PSTREE_VERSION);
}

/* 进程信息读取 */

/**
 * read_comm - 从/proc/{pid}/comm中读取进程名称
 * @pid: 进程ID
 * @buf: 用于存储进程名称的缓冲区
 * @n: 缓冲区大小
 *
 * 返回值：成功返回0，失败返回-1
 */
static int read_comm(pid_t pid, char *buf, size_t n) {
    char path[64];
    // 构造/proc/{pid}/comm文件路径
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    // 从comm文件中读取进程名称
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }

    // 移除末尾的换行符
    buf[strcspn(buf, "\n")] = 0;
    fclose(f);
    return 0;
}

/**
 * get_ppid_from_stat - 从/proc/{pid}/stat中获取父进程ID
 * @pid: 进程ID
 * @ppid_out: 输出参数，用于返回父进程ID
 *
 * stat文件格式: pid (comm) state ppid ...
 * 需要解析出ppid字段（第4个字段）
 *
 * 返回值：成功返回0，失败返回-1
 */
static int get_ppid_from_stat(pid_t pid, pid_t *ppid_out) {
    char path[64], line[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    // 解析stat文件格式
    // 格式: pid (comm) state ppid ...
    int id, ppid;
    char comm[256], state;
    if (sscanf(line, "%d (%255[^)]) %c %d", &id, comm, &state, &ppid) != 4) {
        return -1;
    }

    *ppid_out = (pid_t)ppid;
    return 0;
}

/* 进程树结构和排序  */

/**
 * 进程信息结构体，用于存储单个进程的信息
 */
typedef struct {
    pid_t pid;           // 进程ID
    pid_t ppid;          // 父进程ID
    char comm[256];      // 进程名称
} ProcessInfo;

/**
 * cmp_pid - qsort回调函数，按PID进行数值排序
 */
static int cmp_pid(const void *a, const void *b) {
    const ProcessInfo *pa = (const ProcessInfo *)a;
    const ProcessInfo *pb = (const ProcessInfo *)b;
    return pa->pid - pb->pid;
}

/**
 * cmp_name - qsort回调函数，按进程名称进行字母排序
 */
static int cmp_name(const void *a, const void *b) {
    const ProcessInfo *pa = (const ProcessInfo *)a;
    const ProcessInfo *pb = (const ProcessInfo *)b;
    return strcmp(pa->comm, pb->comm);
}

/*  进程树打印函数  */

/**
 * print_tree - 递归打印进程树
 * @processes: 进程信息数组
 * @count: 进程总数
 * @parent_pid: 当前要打印的父进程ID
 * @current_pid: 当前进程ID（用于标记）
 * @indent: 缩进字符串，用于显示树结构
 * @is_last: 标记是否为最后一个子进程
 * @show_pids: 是否显示进程ID
 * @numeric_sort: 是否按PID排序（否则按名称排序）
 */
static void print_tree(ProcessInfo *processes, int count, pid_t parent_pid,
                      pid_t current_pid, const char *indent, int is_last,
                      int show_pids, int numeric_sort) {
    // 找出父进程为parent_pid的所有子进程
    ProcessInfo *children = malloc(count * sizeof(ProcessInfo));
    int child_count = 0;

    for (int i = 0; i < count; i++) {
        if (processes[i].ppid == parent_pid) {
            children[child_count++] = processes[i];
        }
    }

    // 根据选项对子进程进行排序
    if (child_count > 0) {
        if (numeric_sort) {
            qsort(children, child_count, sizeof(ProcessInfo), cmp_pid);
        } else {
            qsort(children, child_count, sizeof(ProcessInfo), cmp_name);
        }
    }

    // 递归打印每个子进程及其子树
    for (int i = 0; i < child_count; i++) {
        ProcessInfo *child = &children[i];
        int is_last_child = (i == child_count - 1);

        // 打印树形符号
        printf("%s", indent);
        printf("%s", is_last_child ? "└── " : "├── ");

        // 打印进程名称
        printf("%s", child->comm);

        // 如果启用了--show-pids选项，显示PID
        if (show_pids) {
            printf("(%d)", child->pid);
        }

        // 标记当前进程（主进程）
        if (child->pid == current_pid) {
            printf("  <== me");
        }

        printf("\n");

        // 构造下一级缩进
        char new_indent[1024];
        snprintf(new_indent, sizeof(new_indent), "%s%s", indent,
                is_last_child ? "    " : "│   ");

        // 递归打印子进程的子树
        print_tree(processes, count, child->pid, current_pid, new_indent,
                  is_last_child, show_pids, numeric_sort);
    }

    free(children);
}

int main(int argc, char *argv[]) {
    int show_pids = 0;      // -p 选项标志
    int numeric_sort = 0;   // -n 选项标志

    // 定义长选项数组
    static struct option long_opts[] = {
        {"show-pids",   no_argument, NULL, 'p'},
        {"numeric-sort", no_argument, NULL, 'n'},
        {"version",     no_argument, NULL, 'V'},
        {"help",        no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    // 解析命令行选项
    int opt;
    while ((opt = getopt_long(argc, argv, "pnVh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p':
                show_pids = 1;
                break;
            case 'n':
                numeric_sort = 1;
                break;
            case 'V':
                print_version();
                return 0;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                fprintf(stderr, "Invalid option: %c\n", opt);
                print_usage(argv[0]);
                return 1;
        }
    }

    // 检查是否有额外的非选项参数
    if (optind < argc) {
        fprintf(stderr, "Invalid option: %s\n", argv[optind]);
        print_usage(argv[0]);
        return 1;
    }

    // 获取当前进程和父进程信息
    pid_t self = getpid();
    pid_t parent = getppid();

    // 读取父进程的名称
    char parent_comm[256] = "?";
    read_comm(parent, parent_comm, sizeof parent_comm);

    // 打印根进程（当前进程的父进程）
    printf("%s", parent_comm);
    if (show_pids) {
        printf("(%d)", parent);
    }
    printf("\n");

    // 打开/proc目录，扫描所有进程
    DIR *d = opendir("/proc");
    if (!d) {
        perror("opendir /proc");
        return 1;
    }

    // 读取所有进程信息
    ProcessInfo *processes = malloc(10000 * sizeof(ProcessInfo));
    int process_count = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        // 跳过非数字目录（非进程目录）
        if (!isdigit((unsigned char)de->d_name[0])) continue;

        pid_t pid = (pid_t)atoi(de->d_name);

        // 从stat文件中获取父进程ID
        pid_t ppid;
        if (get_ppid_from_stat(pid, &ppid) != 0) continue;

        // 只收集当前进程的父进程下的所有进程
        // 这样可以只显示与当前进程相关的进程树
        // 注：这里需要递归处理所有后代

        // 读取进程名称
        char comm[256] = "?";
        read_comm(pid, comm, sizeof comm);

        // 存储进程信息
        processes[process_count].pid = pid;
        processes[process_count].ppid = ppid;
        strncpy(processes[process_count].comm, comm, sizeof(processes[process_count].comm) - 1);
        processes[process_count].comm[sizeof(processes[process_count].comm) - 1] = 0;

        process_count++;
    }

    closedir(d);

    // 打印进程树
    print_tree(processes, process_count, parent, self, "", 1, show_pids, numeric_sort);

    // 清理
    free(processes);
    return 0;
}
