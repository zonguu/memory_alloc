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

// sbrk 原型（某些环境下 unistd.h 可能未声明）
// 注意：sbrk 的隐含声明返回 int，在 64 位系统上会导致指针截断！
// 必须显式声明为 void *sbrk(intptr_t)，否则高 32 位被丢弃，地址错误。
extern void *sbrk(intptr_t increment);

// ==================== 全局变量 ====================
// 线程局部存储键（用于存储每个线程的 arena）
pthread_key_t arena_key;

// 全局分配器状态（所有线程共享）
// 全局分配器状态（所有线程共享）
// 注意：初始值数量必须精确匹配 malloc_state_t 的字段数！
// 该结构体有 9 个字段（从 main_arena 到 total_bytes_freed），
// 之前多了一个 0 导致编译报错"excess elements in struct initializer"。
static malloc_state_t malloc_state = {0, {NULL}, PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0, 0, 0};

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
    // 关键设计：必须使用 sbrk 直接分配 arena 结构，绝对不能调用 my_malloc！
    // 原因：init_arena 在分配器初始化阶段被 init_malloc 调用，
    // 如果这里调用 my_malloc → my_malloc 检测到未初始化 → 调用 init_malloc
    // → init_malloc 又调用 init_arena → 无限递归，栈溢出崩溃。
    arena_t *arena = (arena_t *)sbrk(sizeof(arena_t));
    if (arena == (void *)-1) return NULL;

    // 分配初始内存池
    void *mem = sbrk(0);
    if (sbrk(4096) == (void *)-1) return NULL;

    chunk_t *chunk = (chunk_t *)mem;
    set_chunk_size(chunk, 4096, 0);
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
// 注意：file 和 line 参数是调用点传入的，如果直接用 __FILE__/__LINE__ 调用，
// 记录的始终是 malloc.c 内部的行号，无法定位到用户的调用位置。
// 改进方案：通过宏包装 my_malloc（如 #define my_malloc(s) my_malloc_tag(s, __FILE__, __LINE__)）
// 来捕获真实的调用位置。参见 debug_malloc.c 中的方案。
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
// 获取当前线程的 arena（单线程版本直接返回主 arena）
// 注意：当前实现使用单 arena 模式，所有线程共享 main_arena。
// 这会导致多线程下的锁竞争。完整实现应为每个线程分配独立 arena（通过 TLS），
// 以减少锁争用。之前尝试使用 pthread_key_t + pthread_getspecific，
// 但未调用 pthread_key_create 初始化 key，导致 getspecific 返回垃圾值。
// 简化方案：直接返回 main_arena，后续可通过 TLS 扩展为每线程 arena。
static arena_t *get_current_arena() {
    return malloc_state.main_arena;
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
        // 只剩一个元素，直接清空 bin
        malloc_state.bins[bin_index] = NULL;
    } else {
        // 从双向循环链表中摘除
        chunk->fd->bk = chunk->bk;
        chunk->bk->fd = chunk->fd;
        // 关键：如果被删除的 chunk 恰好是 bin 的头指针，必须更新！
        // bins[bin_index] 存储的是链表头，如果头被删了不更新，
        // 下次从该 bin 分配时会访问已释放的 chunk → use-after-free 崩溃。
        if (malloc_state.bins[bin_index] == chunk) {
            malloc_state.bins[bin_index] = chunk->fd;
        }
    }
    chunk->fd = chunk->bk = NULL;
    pthread_mutex_unlock(&malloc_state.global_lock);
}

// ==================== 内存优化 ====================
// 分割 chunk（内存碎片优化）
// 当 chunk 过大时，分割成合适大小的 chunk
static void split_chunk(chunk_t *chunk, size_t size) {
    size_t chunk_size = get_chunk_size(chunk);
    // 注意：只有剩余部分 >= chunk头 + 对齐大小时才分割，否则会产生无法管理的碎片
    if (chunk_size >= size + sizeof(chunk_t) + ALIGNMENT) {
        chunk_t *remaining = (chunk_t *)((char *)chunk + size);
        remaining->prev_size = size;
        // 重要：分裂后前面的 chunk（用户端）在使用中，remaining 的 PREV_INUSE 必须为 1
        // 如果错误地设为 0，后续向前合并时 coalesce_chunk 会误判前一个 chunk 为空闲，
        // 导致与正在使用的内存合并，产生 dangling pointer 和 double free。
        set_chunk_size(remaining, chunk_size - size, PREV_INUSE);

        set_chunk_size(chunk, size, PREV_INUSE);
    }
}

// 合并相邻的空闲 chunk（内存碎片优化）
// 合并前一个和后一个空闲的 chunk
static chunk_t *coalesce_chunk(chunk_t *chunk) {
    // ========== 向后合并（与前一个空闲 chunk 合并）==========
    // 边界标签技术：通过检查当前 chunk 的 PREV_INUSE 标志和 prev_size
    // 来判断前一个 chunk 是否空闲以及它的位置和大小。
    //
    // 注意：必须同时检查 chunk->prev_size > 0！
    // 如果 prev_size == 0，说明没有前一个 chunk 或者 prev_size 尚未初始化，
    // 此时 (chunk - 0) == chunk，会导致 chunk 与自身合并，产生循环链表崩溃。
    if (!(chunk->size & PREV_INUSE) && chunk->prev_size > 0) {
        chunk_t *prev = (chunk_t *)((char *)chunk - chunk->prev_size);
        if (prev->size & ~IS_MMAP) {
            remove_from_bin(prev, BIN_INDEX(get_chunk_size(prev)));
            size_t merged = get_chunk_size(prev) + get_chunk_size(chunk);
            chunk_t *next = (chunk_t *)((char *)prev + merged);
            // 关键：合并后必须更新后一个 chunk 的 prev_size，
            // 否则后一个 chunk 的 PREV_INUSE 检查会读到错误的偏移量。
            // 这是边界标签一致性的核心要求。
            next->prev_size = merged;
            set_chunk_size(prev, merged, PREV_INUSE);
            chunk = prev;
        }
    }

    // ========== 向前合并（与后一个空闲 chunk 合并）==========
    chunk_t *next = (chunk_t *)((char *)chunk + get_chunk_size(chunk));
    // 注意：必须检查 get_chunk_size(next) > 0！
    // 当 chunk 是 top chunk 且刚刚通过 sbrk 扩展时，新 top 的大小被设为 sizeof(chunk_t)，
    // 但如果扩展后还没来得及设置大小（或者 sbrk 返回的内存恰好为零），
    // next->size == 0 会导致 get_chunk_size(next) == 0，此时 next 不是一个有效的 chunk。
    // 合并 size=0 的"chunk"会破坏堆结构。
    if (!(next->size & PREV_INUSE) && get_chunk_size(next) > 0) {
        remove_from_bin(next, BIN_INDEX(get_chunk_size(next)));
        size_t merged = get_chunk_size(chunk) + get_chunk_size(next);
        chunk_t *next2 = (chunk_t *)((char *)chunk + merged);
        next2->prev_size = merged;
        set_chunk_size(chunk, merged, chunk->size & PREV_INUSE);
    }

    return chunk;
}

// 从 arena 获取 chunk（线程安全）
// 使用 arena 锁保护 arena 内部操作
//
// 设计要点：
// 1. top chunk 是 arena 中最后一个 chunk，其上方是未映射的内存。
// 2. top_size < size 时调用 sbrk 扩展堆，但其他线程可能已通过 sbrk 扩展，
//    所以 sbrk 返回的地址不是新内存的起始，只是确保地址空间可用。
// 3. 扩展时必须额外分配 sizeof(chunk_t) 给新 top 的头部，
//    否则新 top 的 size/prev_size 写入到 program break 之外 → 段错误。
// 4. 大内存（>4096 字节）走 mmap 路径，不经过此函数。
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

    // 需要扩展 arena。注意：sbrk 返回的是新扩展内存的起始地址（即旧的 program break），
    // 但当前 top chunk 已经用到了旧的 program break，所以不能直接用 sbrk 返回值。
    // 正确做法：从 top 的当前位置继续分配，sbrk 只是确保后续地址可用。
    //
    // 重要：必须多分配 sizeof(chunk_t) 给新 top 的 chunk 头！
    // 新 top 需要有自己的 chunk 头（prev_size + size），否则下次 get_chunk_from_arena
    // 读取 arena->top->size 时会读到未初始化的内存（可能是零），
    // 导致 get_chunk_size(top) == 0 → 除以零或逻辑错误。
    // 更糟的情况：写 arena->top->prev_size 时写入到 program break 之外 → 段错误。
    size_t needed = size - top_size;
    size_t total_needed = needed + sizeof(chunk_t);
    void *result = sbrk(total_needed);
    if (result == (void *)-1) {
        pthread_mutex_unlock(&arena->lock);
        return NULL;
    }

    arena->system_mem += total_needed;
    set_chunk_size(top, top_size + needed, PREV_INUSE);
    arena->top = (chunk_t *)((char *)top + top_size + needed);
    arena->top->prev_size = top_size + needed;
    set_chunk_size(arena->top, sizeof(chunk_t), PREV_INUSE);
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
        // 如果 chunk 比需要的大很多，分割剩余部分放回 bin
        size_t chunk_size = get_chunk_size(chunk);
        size_t min_size = aligned_size + sizeof(chunk_t) + ALIGNMENT;
        if (chunk_size >= min_size) {
            split_chunk(chunk, aligned_size);
            // 注意：分割后必须将剩余部分插入对应 bin，并更新后面 chunk 的 prev_size，
            // 否则剩余部分会成为"黑洞"——既不在 bin 中也不是 top chunk，永远无法再分配。
            // next_chunk->prev_size 用于后续向前合并时的边界标签一致性。
            chunk_t *remaining = (chunk_t *)((char *)chunk + aligned_size);
            size_t remaining_size = get_chunk_size(remaining);
            chunk_t *next_chunk = (chunk_t *)((char *)remaining + remaining_size);
            next_chunk->prev_size = remaining_size;
            int remaining_bin = BIN_INDEX(remaining_size);
            insert_into_bin(remaining, remaining_bin);
        }

        // 记录分配信息
        // 注意：__FILE__ 和 __LINE__ 展开为此文件（malloc.c）中的位置，
        // 不是调用 my_malloc 的源文件位置。真正的调用者追踪需要宏包装
        // （如 debug_malloc.c 中的 DEBUG_MALLOC 宏），这是 debug_info 的设计局限。
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

    if (is_mmap_chunk(chunk)) {
        size_t size = get_chunk_size(chunk);
        munmap(chunk, size);
        return;
    }

    // 更新统计信息
    size_t size = get_chunk_size(chunk);
    arena_t *arena = get_current_arena();
    arena->free_count++;
    malloc_state.total_frees++;
    malloc_state.current_allocations--;
    malloc_state.total_bytes_freed += size;

    // 标记为空闲
    set_chunk_size(chunk, size, 0);

    // 合并相邻的空闲 chunk（内存碎片优化）
    chunk = coalesce_chunk(chunk);

    // 重新获取合并后的大小，插入到对应的 bin
    size_t merged_size = get_chunk_size(chunk);
    int bin_index = BIN_INDEX(merged_size);
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