#include "../include/malloc.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

void test_basic_allocation() {
    printf("Testing basic allocation...\n");

    void *ptr1 = my_malloc(16);
    assert(ptr1 != NULL);
    printf("Allocated 16 bytes at %p\n", ptr1);

    void *ptr2 = my_malloc(32);
    assert(ptr2 != NULL);
    printf("Allocated 32 bytes at %p\n", ptr2);

    void *ptr3 = my_malloc(64);
    assert(ptr3 != NULL);
    printf("Allocated 64 bytes at %p\n", ptr3);

    my_free(ptr1);
    my_free(ptr2);
    my_free(ptr3);
    printf("Basic allocation test passed\n");
}

void test_calloc() {
    printf("Testing my_calloc...\n");

    void *ptr = my_calloc(4, 16);
    assert(ptr != NULL);
    printf("Allocated 64 bytes with my_calloc at %p\n", ptr);

    // 检查内存是否被清零
    char *data = (char *)ptr;
    for (int i = 0; i < 64; i++) {
        assert(data[i] == 0);
    }

    my_free(ptr);
    printf("Calloc test passed\n");
}

void test_realloc() {
    printf("Testing my_realloc...\n");

    void *ptr = my_malloc(16);
    assert(ptr != NULL);

    // 扩大内存
    void *new_ptr = my_realloc(ptr, 32);
    assert(new_ptr != NULL);
    printf("Reallocated from 16 to 32 bytes: %p -> %p\n", ptr, new_ptr);

    // 缩小内存
    void *small_ptr = my_realloc(new_ptr, 8);
    assert(small_ptr != NULL);
    printf("Reallocated from 32 to 8 bytes: %p -> %p\n", new_ptr, small_ptr);

    my_free(small_ptr);
    printf("Realloc test passed\n");
}

void test_free_null() {
    printf("Testing my_free(NULL)...\n");
    my_free(NULL);  // 应该不会崩溃
    printf("Free NULL test passed\n");
}

void test_large_allocation() {
    printf("Testing large allocation...\n");

    // 分配超过 4KB 的内存，应该使用 mmap
    void *ptr = my_malloc(8192);
    assert(ptr != NULL);
    printf("Allocated 8192 bytes at %p\n", ptr);

    my_free(ptr);
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