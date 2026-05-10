#include "../include/malloc.h"
#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <unistd.h>

// 使用命名空间别名，方便测试
// namespace my_alloc = memory_allocator;  // 移除，因为memory_allocator不是命名空间

// 基本分配测试
TEST(MemoryAllocator, BasicAllocation) {
    void *ptr1 = my_malloc(16);
    ASSERT_NE(ptr1, nullptr);
    EXPECT_GE(get_chunk_size((chunk_t *)((char *)ptr1 - sizeof(chunk_t))), 16);

    void *ptr2 = my_malloc(32);
    ASSERT_NE(ptr2, nullptr);
    EXPECT_GE(get_chunk_size((chunk_t *)((char *)ptr2 - sizeof(chunk_t))), 32);

    void *ptr3 = my_malloc(64);
    ASSERT_NE(ptr3, nullptr);
    EXPECT_GE(get_chunk_size((chunk_t *)((char *)ptr3 - sizeof(chunk_t))), 64);

    my_free(ptr1);
    my_free(ptr2);
    my_free(ptr3);
}

// my_calloc测试
TEST(MemoryAllocator, Calloc) {
    void *ptr = my_calloc(4, 16);
    ASSERT_NE(ptr, nullptr);

    // 检查内存是否被清零
    char *data = (char *)ptr;
    for (int i = 0; i < 64; i++) {
        EXPECT_EQ(data[i], 0);
    }

    my_free(ptr);
}

// my_realloc测试
TEST(MemoryAllocator, Realloc) {
    void *ptr = my_malloc(16);
    ASSERT_NE(ptr, nullptr);

    // 扩大内存
    void *new_ptr = my_realloc(ptr, 32);
    ASSERT_NE(new_ptr, nullptr);
    EXPECT_GE(get_chunk_size((chunk_t *)((char *)new_ptr - sizeof(chunk_t))), 32);

    // 缩小内存
    void *small_ptr = my_realloc(new_ptr, 8);
    ASSERT_NE(small_ptr, nullptr);
    EXPECT_GE(get_chunk_size((chunk_t *)((char *)small_ptr - sizeof(chunk_t))), 8);

    my_free(small_ptr);
}

// my_free(NULL)测试
TEST(MemoryAllocator, FreeNull) {
    // 应该不会崩溃
    my_free(nullptr);
}

// 大内存分配测试
TEST(MemoryAllocator, LargeAllocation) {
    // 分配超过 4KB 的内存，应该使用 mmap
    void *ptr = my_malloc(8192);
    ASSERT_NE(ptr, nullptr);
    EXPECT_GE(get_chunk_size((chunk_t *)((char *)ptr - sizeof(chunk_t))), 8192);

    my_free(ptr);
}

// 多线程测试
TEST(MemoryAllocator, MultiThread) {
    const int NUM_THREADS = 4;
    const int NUM_ALLOCATIONS = 100;

    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    // 创建多个线程
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, [](void *arg) -> void* {
            int thread_id = *(int *)arg;

            // 执行一系列内存分配和释放操作
            for (int j = 0; j < NUM_ALLOCATIONS; j++) {
                size_t size = (j % 100) + 1;  // 分配 1-100 字节
                void *ptr = my_malloc(size);
                (void)ptr;  // 确保ptr被使用，避免未使用变量警告

                // 使用分配的内存
                memset(ptr, thread_id, size);

                // 随机释放一些内存
                if (j % 3 == 0) {
                    my_free(ptr);
                }
            }
            return nullptr;
        }, &thread_ids[i]);
    }

    // 等待所有线程完成
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // 执行一些额外的测试
    void *ptr1 = my_malloc(16);
    ASSERT_NE(ptr1, nullptr);
    my_free(ptr1);
}

// 内存泄漏检测测试
TEST(MemoryAllocator, MemoryLeakDetection) {
    // 分配一些内存但不释放
    void *ptr1 = my_malloc(100);
    void *ptr2 = my_malloc(200);

    // 这里应该检测到内存泄漏
    // 注意：gtest不会自动检测内存泄漏，需要手动检查
    // 在实际应用中，可以使用 valgrind 或其他工具

    // 清理以避免影响其他测试
    my_free(ptr1);
    my_free(ptr2);
}

// 性能测试
TEST(MemoryAllocator, Performance) {
    const int NUM_ALLOCATIONS = 1000;
    clock_t start, end;
    double cpu_time_used;

    start = clock();

    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        size_t size = (i % 100) + 1;  // 分配 1-100 字节
        void *ptr = my_malloc(size);
        ASSERT_NE(ptr, nullptr);
        my_free(ptr);
    }

    end = clock();
    cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;

    EXPECT_LT(cpu_time_used, 1.0);  // 应该在1秒内完成
}

// 边界条件测试
TEST(MemoryAllocator, BoundaryConditions) {
    // 测试零大小分配
    void *ptr = my_malloc(0);
    EXPECT_EQ(ptr, nullptr);

    // 测试非常大的分配
    void *large_ptr = my_malloc(SIZE_MAX);
    EXPECT_EQ(large_ptr, nullptr);

    // 测试负大小分配
    void *negative_ptr = my_malloc(-1);
    EXPECT_EQ(negative_ptr, nullptr);
}

// 线程安全测试
TEST(MemoryAllocator, ThreadSafety) {
    const int NUM_THREADS = 8;
    const int NUM_OPERATIONS = 1000;

    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, [](void *arg) -> void* {
            int thread_id = *(int *)arg;

            for (int j = 0; j < NUM_OPERATIONS; j++) {
                size_t size = (j % 50) + 1;  // 分配 1-50 字节
                void *ptr = my_malloc(size);
                (void)ptr;  // 确保ptr被使用，避免未使用变量警告
                if (ptr) {
                    memset(ptr, thread_id, size);
                    my_free(ptr);
                }
            }
            return nullptr;
        }, &thread_ids[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
}

// 内存碎片测试
TEST(MemoryAllocator, MemoryFragmentation) {
    const int NUM_ALLOCATIONS = 100;
    void *pointers[NUM_ALLOCATIONS];

    // 分配和释放交替进行，制造内存碎片
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        size_t size = (i % 20) + 1;  // 分配 1-20 字节
        pointers[i] = my_malloc(size);
        ASSERT_NE(pointers[i], nullptr);
    }

    // 释放偶数索引的内存
    for (int i = 0; i < NUM_ALLOCATIONS; i += 2) {
        my_free(pointers[i]);
    }

    // 重新分配，应该能够重用释放的内存
    for (int i = 0; i < NUM_ALLOCATIONS / 2; i++) {
        size_t size = (i % 20) + 1;
        pointers[i] = my_malloc(size);
        ASSERT_NE(pointers[i], nullptr);
    }

    // 清理
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        if (pointers[i]) {
            my_free(pointers[i]);
        }
    }
}

// mmap分配测试
TEST(MemoryAllocator, MmapAllocation) {
    // 分配大内存，应该使用mmap
    void *ptr = my_malloc(5000);  // 超过4KB
    ASSERT_NE(ptr, nullptr);

    // 检查是否是mmap分配的
    chunk_t *chunk = (chunk_t *)((char *)ptr - sizeof(chunk_t));
    EXPECT_TRUE(is_mmap_chunk(chunk));

    my_free(ptr);
}

// 内存对齐测试
TEST(MemoryAllocator, MemoryAlignment) {
    // 测试不同大小的对齐
    for (int i = 1; i <= 100; i++) {
        void *ptr = my_malloc(i);
        ASSERT_NE(ptr, nullptr);

        // 检查对齐
        uintptr_t address = (uintptr_t)ptr;
        EXPECT_EQ(address % ALIGNMENT, 0);

        my_free(ptr);
    }
}

// 快速分配测试
TEST(MemoryAllocator, FastAllocation) {
    const int NUM_ALLOCATIONS = 1000;

    // 快速分配小内存
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        size_t size = (i % 64) + 1;  // 1-64字节，应该在fast bin中
        void *ptr = my_malloc(size);
        ASSERT_NE(ptr, nullptr);
        my_free(ptr);
    }
}

// 失败测试示例
TEST(MemoryAllocator, FailureTests) {
    // 这些测试应该失败，用于验证错误处理

    // 分配过大内存（应该返回nullptr）
    void *large_ptr = my_malloc(SIZE_MAX);
    EXPECT_EQ(large_ptr, nullptr);

    // 释放nullptr（应该不崩溃）
    my_free(nullptr);

    // 重新分配nullptr（等同于my_malloc）
    void *my_realloc_null = my_realloc(nullptr, 100);
    ASSERT_NE(my_realloc_null, nullptr);
    my_free(my_realloc_null);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}