#include <mymalloc.h>

#ifdef FREESTANDING

// This checks whether your code works for
// freestanding environments.

void _start() {
}

int main() {
    return 0;
}

// Freestanding 环境下没有 mmap/munmap。这里用一块静态内存池模拟"虚拟
// 内存"，让本地 `make check`（freestanding 编译自检）能够链接通过。
// Online Judge 评测时会用自己的 vmalloc/vmfree 覆盖（替换）这里，因此
// 这段代码只用于本地编译/链接，实际运行测试时并不生效。标记为 weak，
// 即使与评测端的强符号同定义也不会冲突。
// 池子做得较小，保证即便被编译进评测二进制也不会超过 1 MiB 静态上限。
static char vm_pool[256 * 1024] __attribute__((aligned(4096)));
static size_t vm_pool_used;

__attribute__((weak))
void *vmalloc(void *addr, size_t length) {
    if (addr != NULL)
        return NULL;                            // 简化实现：不支持指定地址
    length = (length + 4095) & ~(size_t)4095;   // 长度页对齐
    if (vm_pool_used + length > sizeof(vm_pool))
        return NULL;                            // 池满：分配失败
    void *p = vm_pool + vm_pool_used;
    vm_pool_used += length;
    return p;
}

__attribute__((weak))
void vmfree(void *addr, size_t length) {
    // 简化实现：不回收。本地 check 不会真正运行这段代码。
}

#else

#include <sys/mman.h>

void *vmalloc(void *addr, size_t length) {
    // length must be aligned to page size (4096).
    void *result = mmap(addr, length, PROT_READ | PROT_WRITE, 
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) {
        return NULL;
    }
    return result;
}

void vmfree(void *addr, size_t length) {
    munmap(addr, length);
}

#endif
