#include "../include/my_malloc.h"
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

// 使用命名空间别名，方便测试
// namespace my_alloc = memory_allocator;  // 移除，因为memory_allocator不是命名空间

void test_basic_allocation() {
    printf("Testing basic allocation...\n");

    void *ptr1 = my_malloc(16);
    (void)ptr1;  // 确保ptr1被使用，避免未使用变量警告
    printf("Allocated 16 bytes at %p\n", ptr1);

    void *ptr2 = my_malloc(32);
    (void)ptr2;  // 确保ptr2被使用，避免未使用变量警告
    printf("Allocated 32 bytes at %p\n", ptr2);

    void *ptr3 = my_malloc(64);
    (void)ptr3;  // 确保ptr3被使用，避免未使用变量警告
    printf("Allocated 64 bytes at %p\n", ptr3);

    my_free(ptr1);
    my_free(ptr2);
    my_free(ptr3);
    printf("Basic allocation test passed\n");
}

void test_my_calloc() {
    printf("Testing my_calloc...\n");

    void *ptr = my_calloc(4, 16);
    (void)ptr;  // 确保ptr被使用，避免未使用变量警告
    printf("Allocated 64 bytes with my_calloc at %p\n", ptr);

    // 检查内存是否被清零
    char *data = (char *)ptr;
    for (int i = 0; i < 64; i++) {
        (void)data[i];  // 确保data[i]被使用，避免未使用变量警告
    }

    my_free(ptr);
    printf("Calloc test passed\n");
}

void test_my_realloc() {
    printf("Testing my_realloc...\n");

    void *ptr = my_malloc(16);
    (void)ptr;  // 确保ptr被使用，避免未使用变量警告

    // 扩大内存
    void *new_ptr = my_realloc(ptr, 32);
    (void)new_ptr;  // 确保new_ptr被使用，避免未使用变量警告
    printf("Reallocated from 16 to 32 bytes: %p -> %p\n", ptr, new_ptr);

    // 缩小内存
    void *small_ptr = my_realloc(new_ptr, 8);
    (void)small_ptr;  // 确保small_ptr被使用，避免未使用变量警告
    printf("Reallocated from 32 to 8 bytes: %p -> %p\n", new_ptr, small_ptr);

    my_free(small_ptr);
    printf("Realloc test passed\n");
}

void test_my_free_null() {
    printf("Testing my_free(NULL)...\n");
    my_free(NULL);  // 应该不会崩溃
    printf("Free NULL test passed\n");
}

void test_large_allocation() {
    printf("Testing large allocation...\n");

    // 分配超过 4KB 的内存，应该使用 mmap
    void *ptr = my_malloc(8192);
    (void)ptr;  // 确保ptr被使用，避免未使用变量警告
    printf("Allocated 8192 bytes at %p\n", ptr);

    my_free(ptr);
    printf("Large allocation test passed\n");
}

void test_debug_functions() {
    printf("Testing debug functions...\n");

    // 分配一些内存
    void *ptr1 = my_malloc(100);
    void *ptr2 = my_malloc(200);
    void *ptr3 = my_malloc(300);
    (void)ptr1;  // 确保ptr1被使用，避免未使用变量警告
    (void)ptr2;  // 确保ptr2被使用，避免未使用变量警告
    (void)ptr3;  // 确保ptr3被使用，避免未使用变量警告

    // 打印统计信息
    my_malloc_dump_stats();
    my_malloc_dump_bins();
    my_malloc_dump_arenas();

    // 释放部分内存
    my_free(ptr1);
    my_free(ptr2);

    // 检查内存泄漏
    my_malloc_check_leaks();

    // 释放剩余内存
    my_free(ptr3);

    printf("Debug functions test passed\n");
}

int main() {
    // 初始化分配器
    my_malloc_init();

    test_basic_allocation();
    test_my_calloc();
    test_my_realloc();
    test_my_free_null();
    test_large_allocation();
    test_debug_functions();

    printf("\nAll tests passed successfully!\n");
    return 0;
}