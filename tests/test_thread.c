#include "../include/malloc.h"
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#define NUM_THREADS 4
#define NUM_ALLOCATIONS 1000

void *thread_function(void *arg) {
    int thread_id = *(int *)arg;
    printf("Thread %d started\n", thread_id);

    // 存储分配的内存指针，以便后续释放
    void *allocated_ptrs[NUM_ALLOCATIONS];
    int allocated_count = 0;

    // 执行一系列内存分配和释放操作
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        size_t size = (i % 100) + 1;  // 分配 1-100 字节
        void *ptr = malloc(size);
        assert(ptr != NULL);
        allocated_ptrs[allocated_count++] = ptr;

        // 使用分配的内存
        memset(ptr, thread_id, size);

        // 随机释放一些内存
        if (i % 3 == 0 && allocated_count > 0) {
            int index = allocated_count - 1;
            free(allocated_ptrs[index]);
            allocated_ptrs[index] = NULL;
            allocated_count--;
        }
    }

    // 释放所有剩余的内存
    for (int i = 0; i < allocated_count; i++) {
        if (allocated_ptrs[i] != NULL) {
            free(allocated_ptrs[i]);
        }
    }

    printf("Thread %d completed\n", thread_id);
    return NULL;
}

// 声明 malloc_init 函数
void malloc_init(void);

int main() {
    // 初始化分配器
    malloc_init();

    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    // 创建多个线程
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, thread_function, &thread_ids[i]);
    }

    // 等待所有线程完成
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("All threads completed successfully!\n");

    // 执行一些额外的测试
    printf("Testing basic allocation after multithreading...\n");

    void *ptr1 = malloc(16);
    assert(ptr1 != NULL);
    printf("Allocated 16 bytes at %p\n", ptr1);

    void *ptr2 = malloc(32);
    assert(ptr2 != NULL);
    printf("Allocated 32 bytes at %p\n", ptr2);

    free(ptr1);
    free(ptr2);

    printf("All tests passed!\n");
    return 0;
}