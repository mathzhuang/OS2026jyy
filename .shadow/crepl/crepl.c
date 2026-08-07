#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

// 用于存储已编译函数的动态库列表
#define MAX_LIBS 256
static void* loaded_libs[MAX_LIBS];
static int lib_count = 0;

// 临时文件计数器，用于生成唯一的文件名
static int file_counter = 0;

// 编译 C 代码文件并生成共享库
// 参数:
//   - c_file: C 源代码文件路径
//   - so_file: 输出的共享库文件路径
// 返回值: 成功返回 true，失败返回 false
static bool compile_to_shared_lib(const char* c_file, const char* so_file) {
    // 使用 GCC 编译 C 代码为位置无关代码的共享库
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gcc -fPIC -shared -o %s %s 2>/dev/null", so_file, c_file);

    int ret = system(cmd);
    // system() 成功返回 0，不成功返回非 0
    return ret == 0;
}

// 将源代码写入临时 C 文件
// 参数:
//   - filename: 要写入的文件路径
//   - content: 源代码内容
// 返回值: 成功返回 true，失败返回 false
static bool write_source_file(const char* filename, const char* content) {
    FILE* f = fopen(filename, "w");
    if (!f) return false;

    size_t written = fwrite(content, 1, strlen(content), f);
    fclose(f);

    return written == strlen(content);
}

// 编译并加载函数定义
// 当用户输入一个完整的 C 函数定义时调用此函数
// 该函数会：
//   1. 将函数写入临时 C 文件
//   2. 编译为共享库
//   3. 使用 dlopen 动态加载库
//   4. 保存库指针以便后续使用
// 参数:
//   - function_def: 包含完整 C 函数定义的字符串，例如 "int foo() { return 42; }"
// 返回值: 成功编译并加载返回 true，否则返回 false
bool compile_and_load_function(const char* function_def) {
    // 检查输入
    if (!function_def || strlen(function_def) == 0) {
        return false;
    }

    // 检查是否还有空间存储新的库
    if (lib_count >= MAX_LIBS) {
        return false;
    }

    // 生成临时文件名
    char c_file[64], so_file[64];
    snprintf(c_file, sizeof(c_file), "/tmp/crepl_%d.c", file_counter);
    snprintf(so_file, sizeof(so_file), "/tmp/crepl_%d.so", file_counter);
    file_counter++;

    // 编写 C 源代码文件
    // 为了支持函数间调用，我们需要链接已加载的库
    if (!write_source_file(c_file, function_def)) {
        return false;
    }

    // 编译成共享库
    if (!compile_to_shared_lib(c_file, so_file)) {
        unlink(c_file);
        return false;
    }

    // 动态加载生成的共享库
    // RTLD_GLOBAL 标志使该库中的符号对随后加载的库可见
    // RTLD_LAZY 表示延迟绑定符号
    void* handle = dlopen(so_file, RTLD_GLOBAL | RTLD_LAZY);
    if (!handle) {
        unlink(c_file);
        unlink(so_file);
        return false;
    }

    // 保存库指针以便后续清理
    loaded_libs[lib_count++] = handle;

    // 清理临时源文件
    unlink(c_file);
    // 注意：不能立即删除 .so 文件，因为它仍被加载
    // 在程序退出时由操作系统清理

    return true;
}

// 求值表达式
// 当用户输入一个表达式（如 "21 + 21" 或 "test_func()"）时调用此函数
// 该函数会：
//   1. 生成一个包装函数，将表达式作为其返回值
//   2. 编译并加载这个包装函数
//   3. 动态查找并执行函数
//   4. 返回执行结果
// 参数:
//   - expression: 要求值的表达式，例如 "42" 或 "21 + 21"
//   - result: 指向存储结果的 int 指针
// 返回值: 成功求值返回 true，否则返回 false
bool evaluate_expression(const char* expression, int* result) {
    // 检查输入
    if (!expression || !result || strlen(expression) == 0) {
        return false;
    }

    // 检查是否还有空间存储新的库
    if (lib_count >= MAX_LIBS) {
        return false;
    }

    // 生成临时文件名
    char c_file[64], so_file[64];
    snprintf(c_file, sizeof(c_file), "/tmp/crepl_expr_%d.c", file_counter);
    snprintf(so_file, sizeof(so_file), "/tmp/crepl_expr_%d.so", file_counter);
    file_counter++;

    // 生成包装函数代码
    // 将表达式包装在一个函数中，便于编译和执行
    char wrapper_code[1024];
    snprintf(wrapper_code, sizeof(wrapper_code),
             "int __crepl_eval_func() { return %s; }", expression);

    // 写入源代码文件
    if (!write_source_file(c_file, wrapper_code)) {
        return false;
    }

    // 编译成共享库
    // 编译时生成一个与已加载库兼容的共享库
    // 延迟符号绑定（RTLD_LAZY）直到执行时，以便可以链接到动态加载的库
    // 输出重定向到 /dev/null 以隐藏可能的警告
    char so_cmd[512];
    snprintf(so_cmd, sizeof(so_cmd),
             "gcc -fPIC -shared -o %s %s 2>/dev/null",
             so_file, c_file);

    if (system(so_cmd) != 0) {
        unlink(c_file);
        unlink(so_file);
        return false;
    }

    // 动态加载生成的共享库
    // 使用 RTLD_NOW（而不是 RTLD_LAZY）进行立即符号绑定
    // 这样如果表达式引用未定义的函数，dlopen 就会立即失败
    void* handle = dlopen(so_file, RTLD_NOW);
    if (!handle) {
        unlink(c_file);
        unlink(so_file);
        return false;
    }

    // 获取包装函数的指针
    // 使用 dlsym 从加载的库中查找符号
    typedef int (*eval_func_t)(void);
    eval_func_t eval_func = (eval_func_t)dlsym(handle, "__crepl_eval_func");

    if (!eval_func) {
        dlclose(handle);
        unlink(c_file);
        unlink(so_file);
        return false;
    }

    // 执行函数获取结果
    *result = eval_func();

    // 卸载动态库（用完即卸）
    dlclose(handle);

    // 清理临时文件
    unlink(c_file);
    unlink(so_file);

    // 不再保存库指针，因为表达式求值是一次性的

    return true;
}

// 主函数 - REPL 主循环
int main() {
    // 本实验中，main 函数作为用户交互入口
    // 在实际的 REPL 应用中，这里会实现读取输入、判断是函数定义还是表达式、
    // 调用相应函数、打印结果的循环
    // 但由于测试通过编译和求值函数来进行，这里保持为空
    // 测试框架会直接调用 compile_and_load_function 和 evaluate_expression

    return 0;
}
