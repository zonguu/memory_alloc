#ifdef __cplusplus
extern "C" {
#endif

#include "malloc.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>

// ==================== 全局变量 ====================
// 线程局部存储键（用于存储每个线程的 arena）
pthread_key_t arena_key;

// 全局分配器状态（所有线程共享）
static malloc_state_t malloc_state = {0, {NULL}, PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0, 0, 0, 0};

// 调试信息数组（用于内存泄漏检测）
#define MAX_DEBUG_INFO 10000
static debug_info_t debug_info[MAX_DEBUG_INFO];
static size_t debug_info_count = 0;
static size_t next_allocation_id = 1;

// ==================== 内存对齐 ====================
// 内存对齐函数，确保指针地址是 ALIGNMENT 的倍数
static inline void *align_ptr(void *ptr) {
    return (void *)(((uintptr_t)ptr + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1));
}

// 获取 chunk 的大小（去除标志位）
inline size_t get_chunk_size(chunk_t *chunk) {
    return chunk->size & ~0x7;
}

// 设置 chunk 的大小和标志位
inline void set_chunk_size(chunk_t *chunk, size_t size, int flags) {
    chunk->size = size | flags;
}

// 检查 chunk 是否是 mmap 分配的
inline int is_mmap_chunk(chunk_t *chunk) {
    return (chunk->size & IS_MMAP) != 0;
}

// ==================== Arena 初始化 ====================
// 初始化一个新的 arena
// 每个 arena 管理一个独立的内存区域，用于多线程环境
static arena_t *init_arena() {
    arena_t *arena = (arena_t *)my_malloc(sizeof(arena_t));
    if (!arena) return NULL;

    // 分配初始内存块
    void *mem = sbrk(0);
    if (mem == (void *)-1) {
        my_free(arena);
        return NULL;
    }

    if (sbrk(4096) == (void *)-1) {
        my_free(arena);
        return NULL;
    }

    chunk_t *chunk = (chunk_t *)mem;
    set_chunk_size(chunk, 4096 - sizeof(arena_t), 0);
    chunk->prev_size = 0;
    chunk->fd = chunk->bk = NULL;

    arena->top = chunk;          // 设置 top chunk
    arena->system_mem = 4096;    // 初始化系统内存大小
    arena->next = NULL;         // 初始化 arena 链表
    pthread_mutex_init(&arena->lock, NULL);  // 初始化 arena 锁
    arena->allocation_count = 0;  // 初始化分配计数
    arena->free_count = 0;      // 初始化释放计数

    return arena;
}

// ==================== 分配器初始化 ====================
// 初始化全局内存分配器
static void init_malloc() {
    malloc_state.main_arena = init_arena();
    if (!malloc_state.main_arena) {
        fprintf(stderr, "Failed to initialize memory allocator\n");
        exit(1);
    }

    // 初始化所有 bin 为 NULL
    for (int i = 0; i < 64; i++) {
        malloc_state.bins[i] = NULL;
    }

    // 初始化全局锁（保护共享资源）
    pthread_mutex_init(&malloc_state.global_lock, NULL);

    // 初始化统计信息
    malloc_state.total_allocations = 0;
    malloc_state.total_frees = 0;
    malloc_state.current_allocations = 0;
    malloc_state.total_bytes_allocated = 0;
    malloc_state.total_bytes_freed = 0;
}

// ==================== 调试和诊断功能 ====================
// 记录分配信息（用于内存泄漏检测）
static void record_allocation(const char *file, int line, size_t size, void *ptr) {
    if (debug_info_count >= MAX_DEBUG_INFO) {
        fprintf(stderr, "Debug info buffer full\n");
        return;
    }

    debug_info[debug_info_count].allocation_id = next_allocation_id++;
    debug_info[debug_info_count].file = file;
    debug_info[debug_info_count].line = line;
    debug_info[debug_info_count].size = size;
    debug_info[debug_info_count].ptr = ptr;
    debug_info[debug_info_count].freed = 0;
    debug_info_count++;
}

// 记录释放信息
static void record_free(void *ptr) {
    for (size_t i = 0; i < debug_info_count; i++) {
        if (debug_info[i].ptr == ptr && !debug_info[i].freed) {
            debug_info[i].freed = 1;
            break;
        }
    }
}

// 打印分配统计信息
void malloc_dump_stats() {
    printf("=== Memory Allocator Statistics ===\n");
    printf("Total allocations: %zu\n", malloc_state.total_allocations);
    printf("Total frees: %zu\n", malloc_state.total_frees);
    printf("Current active allocations: %zu\n", malloc_state.current_allocations);
    printf("Total bytes allocated: %zu\n", malloc_state.total_bytes_allocated);
    printf("Total bytes freed: %zu\n", malloc_state.total_bytes_freed);
    printf("Current memory usage: %zu bytes\n",
           malloc_state.total_bytes_allocated - malloc_state.total_bytes_freed);
}

// 打印 bin 信息
void malloc_dump_bins() {
    printf("=== Bin Information ===\n");
    for (int i = 0; i < 64; i++) {
        if (malloc_state.bins[i]) {
            printf("Bin %d: %p\n", i, malloc_state.bins[i]);
        }
    }
}

// 打印 arena 信息
void malloc_dump_arenas() {
    printf("=== Arena Information ===\n");
    arena_t *arena = malloc_state.main_arena;
    while (arena) {
        printf("Arena: %p, Top: %p, System mem: %zu, Allocations: %zu, Frees: %zu\n",
               arena, arena->top, arena->system_mem, arena->allocation_count, arena->free_count);
        arena = arena->next;
    }
}

// 检查内存泄漏
void malloc_check_leaks() {
    printf("=== Memory Leak Check ===\n");
    size_t leak_count = 0;
    size_t leak_bytes = 0;

    for (size_t i = 0; i < debug_info_count; i++) {
        if (!debug_info[i].freed) {
            leak_count++;
            leak_bytes += debug_info[i].size;
            printf("Leak %zu: %p (size: %zu bytes) allocated at %s:%d\n",
                   debug_info[i].allocation_id, debug_info[i].ptr, debug_info[i].size,
                   debug_info[i].file, debug_info[i].line);
        }
    }

    printf("Total leaks: %zu, Total leaked bytes: %zu\n", leak_count, leak_bytes);
}

// ==================== 多线程支持 ====================
// 获取当前线程的 arena（线程局部存储）
// 如果当前线程没有 arena，则创建一个新的
static arena_t *get_current_arena() {
    arena_t *arena = (arena_t *)pthread_getspecific(arena_key);
    if (!arena) {
        // 如果没有 arena，创建一个新的
        arena = init_arena();
        pthread_setspecific(arena_key, arena);
    }
    return arena;
}

// ==================== Bin 操作 ====================
// 在 bin 中插入 chunk（线程安全）
// 使用全局锁保护 bin 操作
static void insert_into_bin(chunk_t *chunk, int bin_index) {
    pthread_mutex_lock(&malloc_state.global_lock);
    chunk_t *bin = malloc_state.bins[bin_index];

    if (!bin) {
        malloc_state.bins[bin_index] = chunk;
        chunk->fd = chunk->bk = chunk;  // 自循环
    } else {
        chunk->fd = bin;
        chunk->bk = bin->bk;
        bin->bk->fd = chunk;
        bin->bk = chunk;
    }
    pthread_mutex_unlock(&malloc_state.global_lock);
}

// 从 bin 中移除 chunk（线程安全）
static void remove_from_bin(chunk_t *chunk, int bin_index) {
    pthread_mutex_lock(&malloc_state.global_lock);
    if (chunk->fd == chunk && chunk->bk == chunk) {
        malloc_state.bins[bin_index] = NULL;
    } else {
        chunk->fd->bk = chunk->bk;
        chunk->bk->fd = chunk->fd;
    }
    chunk->fd = chunk->bk = NULL;
    pthread_mutex_unlock(&malloc_state.global_lock);
}

// ==================== 内存优化 ====================
// 分割 chunk（内存碎片优化）
// 当 chunk 过大时，分割成合适大小的 chunk
static void split_chunk(chunk_t *chunk, size_t size) {
    size_t chunk_size = get_chunk_size(chunk);
    if (chunk_size >= size + sizeof(chunk_t) + ALIGNMENT) {
        chunk_t *remaining = (chunk_t *)((char *)chunk + size);
        remaining->prev_size = size;
        set_chunk_size(remaining, chunk_size - size, chunk->size & PREV_INUSE);

        chunk_t *next = (chunk_t *)((char *)remaining + get_chunk_size(remaining));
        if (next->size & PREV_INUSE) {
            next->prev_size = get_chunk_size(remaining);
        }

        set_chunk_size(chunk, size, chunk->size & PREV_INUSE);
    }
}

// 合并相邻的空闲 chunk（内存碎片优化）
// 合并前一个和后一个空闲的 chunk
static chunk_t *coalesce_chunk(chunk_t *chunk) {
    size_t prev_inuse = chunk->size & PREV_INUSE;
    chunk_t *prev = (chunk_t *)((char *)chunk - chunk->prev_size);

    if (!prev_inuse && prev->size & ~IS_MMAP) {
        remove_from_bin(prev, BIN_INDEX(get_chunk_size(prev)));
        chunk->prev_size += get_chunk_size(prev);
        chunk = prev;
    }

    chunk_t *next = (chunk_t *)((char *)chunk + get_chunk_size(chunk));
    if (!(next->size & PREV_INUSE)) {
        remove_from_bin(next, BIN_INDEX(get_chunk_size(next)));
        chunk->size += get_chunk_size(next);
    }

    return chunk;
}

// 从 arena 获取 chunk（线程安全）
// 使用 arena 锁保护 arena 内部操作
static chunk_t *get_chunk_from_arena(arena_t *arena, size_t size) {
    pthread_mutex_lock(&arena->lock);
    chunk_t *top = arena->top;
    size_t top_size = get_chunk_size(top);

    if (top_size >= size) {
        if (top_size >= size + sizeof(chunk_t) + ALIGNMENT) {
            split_chunk(top, size);
        }
        set_chunk_size(top, size, PREV_INUSE);
        arena->top = (chunk_t *)((char *)top + size);
        arena->top->prev_size = size;
        pthread_mutex_unlock(&arena->lock);
        return top;
    }

    // 需要扩展 arena
    size_t needed = size - top_size;
    void *result = sbrk(needed);
    if (result == (void *)-1) {
        pthread_mutex_unlock(&arena->lock);
        return NULL;
    }

    arena->system_mem += needed;
    set_chunk_size(top, top_size + needed, PREV_INUSE);
    arena->top = (chunk_t *)((char *)top + top_size + needed);
    arena->top->prev_size = top_size + needed;
    pthread_mutex_unlock(&arena->lock);

    return top;
}

// ==================== 内存分配接口 ====================
// 分配内存（多线程安全）
void *my_malloc(size_t size) {
    if (size == 0) return NULL;

    // 初始化分配器（如果尚未初始化）
    if (!malloc_state.main_arena) {
        init_malloc();
    }

    size_t aligned_size = ALIGN(size + sizeof(chunk_t));
    int bin_index = BIN_INDEX(aligned_size);

    // 获取当前线程的 arena（线程局部存储）
    arena_t *arena = get_current_arena();

    // 首先检查对应的 bin（快速分配小内存）
    chunk_t *chunk = malloc_state.bins[bin_index];
    if (chunk) {
        remove_from_bin(chunk, bin_index);
        set_chunk_size(chunk, aligned_size, PREV_INUSE);

        // 记录分配信息
        record_allocation(__FILE__, __LINE__, size, (void *)((char *)chunk + sizeof(chunk_t)));

        // 更新统计信息
        arena->allocation_count++;
        malloc_state.total_allocations++;
        malloc_state.current_allocations++;
        malloc_state.total_bytes_allocated += size;

        return (void *)((char *)chunk + sizeof(chunk_t));
    }

    // 如果 bin 中没有合适的 chunk，从 arena 获取
    chunk = get_chunk_from_arena(arena, aligned_size);
    if (!chunk) {
        // 对于大内存，使用 mmap（避免 arena 扩展）
        if (aligned_size > 4096) {
            size_t mmap_size = aligned_size;
            chunk_t *mmap_chunk = (chunk_t *)mmap(NULL, mmap_size,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mmap_chunk == MAP_FAILED) {
                return NULL;
            }
            set_chunk_size(mmap_chunk, mmap_size, PREV_INUSE | IS_MMAP);

            // 记录分配信息
            record_allocation(__FILE__, __LINE__, size, (void *)((char *)mmap_chunk + sizeof(chunk_t)));

            // 更新统计信息
            arena->allocation_count++;
            malloc_state.total_allocations++;
            malloc_state.current_allocations++;
            malloc_state.total_bytes_allocated += size;

            return (void *)((char *)mmap_chunk + sizeof(chunk_t));
        }
        return NULL;
    }

    // 记录分配信息
    record_allocation(__FILE__, __LINE__, size, (void *)((char *)chunk + sizeof(chunk_t)));

    // 更新统计信息
    arena->allocation_count++;
    malloc_state.total_allocations++;
    malloc_state.current_allocations++;
    malloc_state.total_bytes_allocated += size;

    return (void *)((char *)chunk + sizeof(chunk_t));
}

// 释放内存（多线程安全）
void my_free(void *ptr) {
    if (!ptr) return;

    // 记录释放信息
    record_free(ptr);

    chunk_t *chunk = (chunk_t *)((char *)ptr - sizeof(chunk_t));
    size_t size = get_chunk_size(chunk);

    if (is_mmap_chunk(chunk)) {
        munmap(chunk, size);
        return;
    }

    // 更新统计信息
    arena_t *arena = get_current_arena();
    arena->free_count++;
    malloc_state.total_frees++;
    malloc_state.current_allocations--;
    malloc_state.total_bytes_freed += size;

    // 标记为空闲
    set_chunk_size(chunk, size, 0);

    // 合并相邻的空闲 chunk（内存碎片优化）
    chunk = coalesce_chunk(chunk);

    // 插入到对应的 bin
    int bin_index = BIN_INDEX(size);
    insert_into_bin(chunk, bin_index);
}

// 分配并清零内存
void *my_calloc(size_t nmemb, size_t size) {
    size_t total_size = nmemb * size;
    void *ptr = my_malloc(total_size);
    if (ptr) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

// 重新分配内存
void *my_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return my_malloc(size);
    }

    if (size == 0) {
        my_free(ptr);
        return NULL;
    }

    chunk_t *old_chunk = (chunk_t *)((char *)ptr - sizeof(chunk_t));
    size_t old_size = get_chunk_size(old_chunk);
    size_t aligned_size = ALIGN(size + sizeof(chunk_t));

    if (aligned_size <= old_size) {
        // 缩小内存，分割 chunk
        split_chunk(old_chunk, aligned_size);
        return ptr;
    }

    // 扩大内存
    chunk_t *next = (chunk_t *)((char *)old_chunk + old_size);
    if (!(next->size & PREV_INUSE) && get_chunk_size(next) + old_size >= aligned_size) {
        // 可以合并下一个 chunk
        remove_from_bin(next, BIN_INDEX(get_chunk_size(next)));
        set_chunk_size(old_chunk, old_size + get_chunk_size(next), PREV_INUSE);
        split_chunk(old_chunk, aligned_size);
        return ptr;
    }

    // 需要分配新内存并复制数据
    void *new_ptr = my_malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, old_size - sizeof(chunk_t));
        my_free(ptr);
    }
    return new_ptr;
}

// 初始化分配器（供外部调用）
void malloc_init() {
    init_malloc();
}

#ifdef __cplusplus
}
#endif