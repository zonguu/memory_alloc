#include "../include/malloc.h"
#include <stdio.h>
#include <assert.h>

void test_basic_allocation() {
    printf("Testing basic allocation...\n");

    void *ptr1 = malloc(16);
    assert(ptr1 != NULL);
    printf("Allocated 16 bytes at %p\n", ptr1);

    void *ptr2 = malloc(32);
    assert(ptr2 != NULL);
    printf("Allocated 32 bytes at %p\n", ptr2);

    void *ptr3 = malloc(64);
    assert(ptr3 != NULL);
    printf("Allocated 64 bytes at %p\n", ptr3);

    free(ptr1);
    free(ptr2);
    free(ptr3);
    printf("Basic allocation test passed\n");
}

void test_calloc() {
    printf("Testing calloc...\n");

    void *ptr = calloc(4, 16);
    assert(ptr != NULL);
    printf("Allocated 64 bytes with calloc at %p\n", ptr);

    // 检查内存是否被清零
    char *data = (char *)ptr;
    for (int i = 0; i < 64; i++) {
        assert(data[i] == 0);
    }

    free(ptr);
    printf("Calloc test passed\n");
}

void test_realloc() {
    printf("Testing realloc...\n");

    void *ptr = malloc(16);
    assert(ptr != NULL);

    // 扩大内存
    void *new_ptr = realloc(ptr, 32);
    assert(new_ptr != NULL);
    printf("Reallocated from 16 to 32 bytes: %p -> %p\n", ptr, new_ptr);

    // 缩小内存
    void *small_ptr = realloc(new_ptr, 8);
    assert(small_ptr != NULL);
    printf("Reallocated from 32 to 8 bytes: %p -> %p\n", new_ptr, small_ptr);

    free(small_ptr);
    printf("Realloc test passed\n");
}

void test_free_null() {
    printf("Testing free(NULL)...\n");
    free(NULL);  // 应该不会崩溃
    printf("Free NULL test passed\n");
}

void test_large_allocation() {
    printf("Testing large allocation...\n");

    // 分配超过 4KB 的内存，应该使用 mmap
    void *ptr = malloc(8192);
    assert(ptr != NULL);
    printf("Allocated 8192 bytes at %p\n", ptr);

    free(ptr);
    printf("Large allocation test passed\n");
}

int main() {
    // 初始化分配器
    malloc_init();

    test_basic_allocation();
    test_calloc();
    test_realloc();
    test_free_null();
    test_large_allocation();

    printf("\nAll tests passed successfully!\n");
    return 0;
}