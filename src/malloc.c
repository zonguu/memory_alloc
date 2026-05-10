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
// 注意：初始值数量必须精确匹配 malloc_state_t 的字段数！
// 该结构体目前有 main_arena + bins + fastbins + global_lock + 5 个统计字段 = 8 个成员
// （bins[64] 和 fastbins[4] 是数组，各算一个成员；global_lock 是一个成员）
// 之前多了一个 0 导致编译报错"excess elements in struct initializer"。
static malloc_state_t malloc_state = {0, {NULL}, {NULL}, PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0, 0};

// 全局 arena 链表（所有 arena 加入此链表，用于 ptr→arena 查找）
static arena_t *arena_list = NULL;

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
// 初始化一个新的 arena（使用 mmap 而非 sbrk）
// 每个 arena 管理一个独立的内存区域，用于多线程环境
//
// 使用 mmap 的原因：
// 1. sbrk 只能线性扩展堆，所有线程共享一个 program break，不适合多线程独立 arena。
// 2. mmap 分配的独立区域可以用作用户态堆，互不干扰。
// 3. 避免了 init_arena 与 my_malloc 之间的循环依赖问题（不需要调用 my_malloc 本身）。
//
// 布局：[arena_t 头部 | top chunk 头部 | top chunk 数据区...]
static arena_t *init_arena(size_t pool_size) {
    void *mem = mmap(NULL, pool_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return NULL;

    // arena 结构体放在 mmap 区域的开头
    arena_t *arena = (arena_t *)mem;
    arena->top = NULL;
    arena->system_mem = 0;
    arena->next = NULL;
    arena->pool_base = mem;
    arena->pool_size = pool_size;
    arena->global_next = NULL;
    pthread_mutex_init(&arena->lock, NULL);
    arena->allocation_count = 0;
    arena->free_count = 0;

    // 对齐 arena 头部大小，确保 top chunk 的起始地址正确
    // 注意：sizeof(arena_t) 因平台 pthread_mutex_t 大小不同而变化
    size_t arena_hdr_size = ALIGN(sizeof(arena_t));
    if (arena_hdr_size + sizeof(chunk_t) > pool_size) {
        // 池太小，连一个 chunk 都放不下
        munmap(mem, pool_size);
        return NULL;
    }

    // 初始化 top chunk（占 arena_hdr_size 到 pool_size 之间所有空间）
    chunk_t *chunk = (chunk_t *)((char *)mem + arena_hdr_size);
    size_t chunk_capacity = pool_size - arena_hdr_size;
    set_chunk_size(chunk, chunk_capacity, 0);  // top chunk 初始无标志
    chunk->prev_size = 0;
    chunk->fd = chunk->bk = NULL;

    arena->top = chunk;
    arena->system_mem = chunk_capacity;

    return arena;
}

// ==================== 分配器初始化 ====================
// 初始化全局内存分配器（只调用一次）
static void init_malloc() {
    // 创建 TLS key（用于 per-thread arena）
    // 注意：必须在第一次 my_malloc 时调用，否则 get_current_arena 中
    // pthread_getspecific 会使用未初始化的 key 返回未定义行为。
    pthread_key_create(&arena_key, NULL);

    // 创建主 arena（同时也是第一个 arena，加入全局链表）
    malloc_state.main_arena = init_arena(ARENA_POOL_SIZE);
    if (!malloc_state.main_arena) {
        fprintf(stderr, "Failed to initialize memory allocator\n");
        exit(1);
    }
    arena_list = malloc_state.main_arena;  // 主 arena 是链表头

    // 初始化所有 bin 为 NULL
    for (int i = 0; i < 64; i++) {
        malloc_state.bins[i] = NULL;
    }

    // 初始化所有 fast bin 为 NULL
    for (int i = 0; i < NUM_FASTBINS; i++) {
        malloc_state.fastbins[i] = NULL;
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
    printf("=== Fast Bin Information ===\n");
    for (int i = 0; i < NUM_FASTBINS; i++) {
        if (malloc_state.fastbins[i]) {
            printf("FastBin %d: %p\n", i, malloc_state.fastbins[i]);
        }
    }
}

// 打印 arena 信息
void malloc_dump_arenas() {
    printf("=== Arena Information ===\n");
    // 通过全局 arena_list 遍历所有 arena（包括主 arena 和线程 arena）
    arena_t *arena = arena_list;
    while (arena) {
        printf("Arena: %p, Top: %p, Pool: %p (%zu KB), Allocations: %zu, Frees: %zu\n",
               arena, arena->top, arena->pool_base, arena->pool_size / 1024,
               arena->allocation_count, arena->free_count);
        arena = arena->global_next;
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
// 获取当前线程的 arena（通过线程局部存储 TLS）
//
// 设计原理：
// 每个线程拥有独立的 arena（mmap 内存池），减少多线程下的锁竞争。
// 主线程使用 main_arena，子线程首次调用时创建自己的 arena。
//
// 注意：
// 1. 必须确保 init_malloc 已调用（pthread_key_create 已完成）。
// 2. 如果 mmap 创建新 arena 失败，回退到 main_arena。
// 3. 新创建的 arena 自动加入全局 arena_list，用于 my_free 中的 ptr→arena 查找。
static arena_t *get_current_arena() {
    // 先检查 TLS 中是否已有 arena
    arena_t *arena = (arena_t *)pthread_getspecific(arena_key);
    if (arena) return arena;

    // 线程首次调用：创建新 arena
    arena = init_arena(ARENA_POOL_SIZE);
    if (!arena) {
        // 创建失败，回退到主 arena
        return malloc_state.main_arena;
    }

    // 加入全局 arena 链表（用于 ptr→arena 查找和 fallback 分配）
    // 这里加全局锁保护 arena_list 的更新
    pthread_mutex_lock(&malloc_state.global_lock);
    arena->global_next = arena_list;
    arena_list = arena;
    pthread_mutex_unlock(&malloc_state.global_lock);

    // 存入 TLS
    pthread_setspecific(arena_key, arena);

    return arena;
}

// 根据用户指针查找所属的 arena（用于统计信息更新）
// 遍历全局 arena_list，检查 ptr 是否在某个 arena 的 mmap 范围内。
// 注意：arena_list 是追加写（不删除），读时不需要锁——即使读到不全的链表，
// 最坏情况是找不到 arena（统计跳过），不影响正确性。
static arena_t *find_arena_by_ptr(void *ptr) {
    arena_t *a = arena_list;
    while (a) {
        if (ptr >= a->pool_base &&
            ptr < (void *)((char *)a->pool_base + a->pool_size)) {
            return a;
        }
        a = a->global_next;
    }
    return NULL;
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
    // 当 chunk 是 top chunk 时，如果池内存已用尽，top 后面的内存不属于本池，
    // next->size 可能是随机值或零。get_chunk_size(next) == 0 时 next 不是有效 chunk。
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

// 将 fast bin 中的 chunk 合并到常规 bin
// 在常规分配无法找到合适内存时调用，将 fast bin 中积压的 chunk
// 解除冻结状态（清除 PREV_INUSE），插入常规 bin。
//
// 为什么需要 consolidation？
// fast bin 中的 chunk 设置了 PREV_INUSE=1，阻止了相邻 chunk 的合并。
// 当系统内存不足时，需要将这些 chunk "解冻"使其参与正常的分配和合并。
//
// 设计要点：
// 1. 先加全局锁收集所有 fast bin chunk 到临时链表，然后释放锁。
// 2. 逐个处理：清除 PREV_INUSE → coalesce → 插入常规 bin。
// 3. 不能持有全局锁调用 coalesce_chunk 或 insert_into_bin（它们会尝试获取全局锁 → 死锁）。
static void malloc_consolidate() {
    chunk_t *consolidate_list = NULL;

    // Step 1: 在全局锁保护下收集所有 fast bin chunk
    pthread_mutex_lock(&malloc_state.global_lock);
    for (int i = 0; i < NUM_FASTBINS; i++) {
        chunk_t *fb = malloc_state.fastbins[i];
        if (!fb) continue;
        // 将整个 fast bin 链表挂到 consolidate_list 末尾
        // 先找链表末尾
        chunk_t *tail = fb;
        while (tail->fd) tail = tail->fd;
        // 将当前 fast bin 追加到 consolidate_list
        if (consolidate_list) {
            tail->fd = consolidate_list;
            consolidate_list = fb;
        } else {
            consolidate_list = fb;
        }
        malloc_state.fastbins[i] = NULL;  // 清空该 fast bin
    }
    pthread_mutex_unlock(&malloc_state.global_lock);

    // Step 2: 逐个处理（不再持有全局锁）
    chunk_t *fb_chunk = consolidate_list;
    while (fb_chunk) {
        chunk_t *next = fb_chunk->fd;  // 保存下一个，因为 fd 可能被 coalesce 修改

        size_t fb_size = get_chunk_size(fb_chunk);
        // 清除 PREV_INUSE：允许前后 chunk 与它合并
        set_chunk_size(fb_chunk, fb_size, 0);

        // 尝试合并相邻空闲 chunk（注意：coalesce_chunk 内部会获取/释放全局锁进行 bin 操作）
        chunk_t *merged = coalesce_chunk(fb_chunk);
        size_t merged_size = get_chunk_size(merged);
        insert_into_bin(merged, BIN_INDEX(merged_size));

        fb_chunk = next;
    }
}

// 从 arena 获取 chunk（线程安全）
// 使用 arena 锁保护 arena 内部操作
//
// 设计要点：
// 1. top chunk 是 arena 中最后一个 chunk，其上方是池的末尾。
// 2. top_size < size 时无法扩展（mmap 池大小固定），返回 NULL。
// 3. 调用者（my_malloc）负责在返回 NULL 时尝试其他 arena 或直接 mmap。
// 4. 当剩余空间不足以分割时，直接将整个 top chunk 分配出去（arena 用尽）。
static chunk_t *get_chunk_from_arena(arena_t *arena, size_t size) {
    if (!arena) return NULL;

    pthread_mutex_lock(&arena->lock);

    // arena 池用尽
    if (!arena->top) {
        pthread_mutex_unlock(&arena->lock);
        return NULL;
    }

    chunk_t *top = arena->top;
    size_t top_size = get_chunk_size(top);

    if (top_size < size) {
        pthread_mutex_unlock(&arena->lock);
        return NULL;  // 池中剩余空间不足
    }

    // 有足够空间
    if (top_size >= size + sizeof(chunk_t) + ALIGNMENT) {
        // 可以分割：前部分给用户，后部分成为新 top
        split_chunk(top, size);
        // split_chunk 设置了 remaining->prev_size、remaining->size 和 chunk->size
        // 需要更新 arena->top
        chunk_t *remaining = (chunk_t *)((char *)top + size);
        arena->top = remaining;
    } else {
        // 剩余空间太小不足以构成有效 chunk，直接把整个 top 给用户
        // arena 用尽，设 top = NULL
        set_chunk_size(top, top_size, PREV_INUSE);
        arena->top = NULL;
    }

    pthread_mutex_unlock(&arena->lock);
    return top;
}

// 从其他 arena 获取 chunk（非阻塞锁，避免死锁）
// 遍历全局 arena 链表，尝试从非当前线程的 arena 分配。
// 使用 pthread_mutex_trylock 避免死锁（如果持有一个 arena 锁再请求另一个）。
static chunk_t *get_chunk_from_other_arenas(size_t size, arena_t *skip_arena) {
    // 先快照 arena_list（无锁读，追加写是安全的）
    pthread_mutex_lock(&malloc_state.global_lock);
    arena_t *list = arena_list;
    pthread_mutex_unlock(&malloc_state.global_lock);

    for (arena_t *a = list; a; a = a->global_next) {
        if (a == skip_arena) continue;
        if (a == NULL) continue;
        // 使用 trylock，不阻塞等待其他线程释放 arena 锁
        if (pthread_mutex_trylock(&a->lock) != 0) continue;

        if (a->top) {
            chunk_t *top = a->top;
            size_t top_size = get_chunk_size(top);
            if (top_size >= size) {
                // 分配逻辑与 get_chunk_from_arena 相同
                if (top_size >= size + sizeof(chunk_t) + ALIGNMENT) {
                    split_chunk(top, size);
                    chunk_t *remaining = (chunk_t *)((char *)top + size);
                    a->top = remaining;
                } else {
                    set_chunk_size(top, top_size, PREV_INUSE);
                    a->top = NULL;  // 该 arena 用尽
                }
                pthread_mutex_unlock(&a->lock);
                return top;
            }
        }
        pthread_mutex_unlock(&a->lock);
    }
    return NULL;
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
    void *user_ptr = NULL;
    chunk_t *chunk = NULL;

    // ===== 1. 尝试从 fast bin 分配（快速小内存路径） =====
    // Fast bin 使用单链表（LIFO），仅适用于小内存（≤96 字节 chunk 大小）。
    // 优点：不触发合并、不修改标志位，O(1) 分配释放。
    if (aligned_size <= FASTBIN_CHUNK_MAX) {
        int fb_idx = FASTBIN_IDX(aligned_size);
        // fast bin 操作需要加锁（多线程安全）
        pthread_mutex_lock(&malloc_state.global_lock);
        chunk = malloc_state.fastbins[fb_idx];
        if (chunk) {
            // LIFO：从链表头取出，fd 作为 next 指针
            malloc_state.fastbins[fb_idx] = chunk->fd;
            pthread_mutex_unlock(&malloc_state.global_lock);
            user_ptr = (void *)((char *)chunk + sizeof(chunk_t));

            // 更新统计信息
            arena_t *arena = find_arena_by_ptr(user_ptr);
            if (arena) arena->allocation_count++;
            malloc_state.total_allocations++;
            malloc_state.current_allocations++;
            malloc_state.total_bytes_allocated += size;

            record_allocation(__FILE__, __LINE__, size, user_ptr);
            return user_ptr;
        }
        pthread_mutex_unlock(&malloc_state.global_lock);
    }

    // ===== 2. 尝试从常规 bin 分配 =====
    int bin_index = BIN_INDEX(aligned_size);
    chunk = malloc_state.bins[bin_index];
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
        user_ptr = (void *)((char *)chunk + sizeof(chunk_t));
    }

    // ===== 3. 尝试从当前线程的 arena 分配 =====
    if (!chunk) {
        arena_t *arena = get_current_arena();
        chunk = get_chunk_from_arena(arena, aligned_size);

        // ===== 4. 尝试从其他 arena 分配 =====
        if (!chunk) {
            chunk = get_chunk_from_other_arenas(aligned_size, arena);
        }

        // ===== 5. Consolidate fast bin 后重试 =====
        if (!chunk) {
            malloc_consolidate();
            // 再次尝试当前 arena（合并后可能腾出空间）
            chunk = get_chunk_from_arena(arena, aligned_size);
            if (!chunk) {
                // 再尝试其他 arena
                chunk = get_chunk_from_other_arenas(aligned_size, arena);
            }
        }

        // ===== 6. 大内存走直接 mmap =====
        if (!chunk && aligned_size > 4096) {
            size_t mmap_size = aligned_size;
            chunk_t *mmap_chunk = (chunk_t *)mmap(NULL, mmap_size,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mmap_chunk != MAP_FAILED) {
                set_chunk_size(mmap_chunk, mmap_size, PREV_INUSE | IS_MMAP);
                chunk = mmap_chunk;
            }
        }

        if (!chunk) return NULL;  // 所有路径都失败

        user_ptr = (void *)((char *)chunk + sizeof(chunk_t));
    }

    // ===== 统一更新统计信息 =====
    // 注意：__FILE__ 和 __LINE__ 展开为此文件（malloc.c）中的位置，
    // 不是调用 my_malloc 的源文件位置。真正的调用者追踪需要宏包装
    // （如 debug_malloc.c 中的 DEBUG_MALLOC 宏），这是 debug_info 的设计局限。
    record_allocation(__FILE__, __LINE__, size, user_ptr);

    arena_t *owner_arena = find_arena_by_ptr(user_ptr);
    if (owner_arena) owner_arena->allocation_count++;
    malloc_state.total_allocations++;
    malloc_state.current_allocations++;
    malloc_state.total_bytes_allocated += size;

    return user_ptr;
}

// 释放内存（多线程安全）
void my_free(void *ptr) {
    if (!ptr) return;

    // 记录释放信息
    record_free(ptr);

    chunk_t *chunk = (chunk_t *)((char *)ptr - sizeof(chunk_t));

    // mmap 分配的直接归还系统
    if (is_mmap_chunk(chunk)) {
        size_t size = get_chunk_size(chunk);
        munmap(chunk, size);
        return;
    }

    size_t size = get_chunk_size(chunk);

    // ===== Fast bin 路径（小内存快速释放） =====
    // 如果 chunk 大小 ≤ FASTBIN_CHUNK_MAX，放入 fast bin 而非常规 bin。
    // Fast bin 中的 chunk 保持 PREV_INUSE=1（不触发合并），
    // 且不设置 PREV_INUSE=0（避免 my_free 写 prev_size 到相邻 chunk）。
    // 这样相邻 chunk 认为前一个 chunk（刚释放的）仍在用 → 跳过合并。
    if (size <= FASTBIN_CHUNK_MAX && !is_mmap_chunk(chunk)) {
        int fb_idx = FASTBIN_IDX(size);
        // LIFO 插入：新释放的 chunk 成为链表头（加锁保护多线程安全）
        pthread_mutex_lock(&malloc_state.global_lock);
        chunk->fd = malloc_state.fastbins[fb_idx];
        malloc_state.fastbins[fb_idx] = chunk;
        pthread_mutex_unlock(&malloc_state.global_lock);
        // 不设置 PREV_INUSE=0！保持原标志不变（PREV_INUSE 仍为 1）
        // 这样前后的 chunk 不会尝试与这个 chunk 合并。

        // 更新统计信息
        malloc_state.total_frees++;
        malloc_state.current_allocations--;
        malloc_state.total_bytes_freed += size;
        arena_t *arena = find_arena_by_ptr(ptr);
        if (arena) arena->free_count++;

        return;
    }

    // ===== 常规释放路径 =====
    // 更新统计信息
    arena_t *arena = find_arena_by_ptr(ptr);
    if (arena) arena->free_count++;
    malloc_state.total_frees++;
    malloc_state.current_allocations--;
    malloc_state.total_bytes_freed += size;

    // 标记为空闲（清除所有标志位，包括 PREV_INUSE）
    // 这样下一个 chunk 的 PREV_INUSE 标志可以被正确检查
    set_chunk_size(chunk, size, 0);

    // 合并相邻的空闲 chunk（内存碎片优化）
    chunk = coalesce_chunk(chunk);

    // 重新获取合并后的大小，插入到对应的 bin
    size_t merged_size = get_chunk_size(chunk);
    int bin_idx = BIN_INDEX(merged_size);
    insert_into_bin(chunk, bin_idx);
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
