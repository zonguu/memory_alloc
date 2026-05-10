#ifndef MALLOC_H
#define MALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>

// Chunk 结构
typedef struct chunk {
    size_t prev_size;      // 前一个 chunk 的大小（如果前一个 chunk 是空闲的）
    size_t size;           // 当前 chunk 的大小和标志位
    struct chunk *fd;      // 双向链表的前向指针（用于 bin 链表）
    struct chunk *bk;      // 双向链表的后向指针（用于 bin 链表）
    // 用户数据区域开始
} chunk_t;

// Arena 结构
typedef struct arena {
    chunk_t *top;          // 当前 arena 的 top chunk
    size_t system_mem;     // 从系统分配的总内存
    struct arena *next;    // 下一个 arena
    pthread_mutex_t lock;  // 线程安全锁
    size_t allocation_count; // 该 arena 的分配次数
    size_t free_count;     // 该 arena 的释放次数
} arena_t;

// 内存分配器状态
typedef struct malloc_state {
    arena_t *main_arena;   // 主 arena
    chunk_t *bins[64];     // 64 个 bin 指针
    pthread_mutex_t global_lock;  // 全局锁
    arena_t *current_arena;      // 当前线程的 arena
    size_t total_allocations;    // 总分配次数
    size_t total_frees;         // 总释放次数
    size_t current_allocations;  // 当前活跃分配数
    size_t total_bytes_allocated;// 总分配字节数
    size_t total_bytes_freed;   // 总释放字节数
} malloc_state_t;

// 调试信息结构
typedef struct debug_info {
    size_t allocation_id;      // 分配 ID
    const char *file;          // 分配时的文件名
    int line;                 // 分配时的行号
    size_t size;              // 分配大小
    void *ptr;                // 分配的指针
    int freed;                // 是否已释放
} debug_info_t;

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 调试和诊断配置 ====================
// 调试模式开关
#define DEBUG_MODE 1

// 内存对齐大小
#define ALIGNMENT 16
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

// Chunk 标志位
#define PREV_INUSE 0x1
#define IS_MMAP 0x2

// Bin 索引计算
// 设计原理：根据 chunk 大小将空闲 chunk 分类到不同 bin，加速大小匹配。
// 分类策略：小内存（≤64B）粒度为 16B；中等内存（≤512B）粒度为 256B；
// 大内存（≤4096B）粒度为 1024B；超大内存（>4096B）统一归入 bin 62。
// 注意：BIN_INDEX 的分段粒度是不均匀的，小内存分段更细以降低内碎片。
// 缺陷：没有 fast bin（单链表、不合并）和 unsorted bin（缓存最近释放的 chunk），
// 导致小内存频繁分配释放时性能较差。
#define BIN_INDEX(size) ((size) <= 64 ? ((size) >> 4) : \
                       (size) <= 512 ? (((size) >> 8) + 4) : \
                       (size) <= 4096 ? (((size) >> 10) + 8) : \
                       62)

// Fast bin 索引（用于快速小内存分配）
#define FASTBIN_INDEX(size) ((size) >> 3)

// Unsorted bin 索引（用于未分类的 chunk）
#define UNSORTED_BIN_INDEX 62

// 公共接口
void *my_malloc(size_t size);
void my_free(void *ptr);
void *my_calloc(size_t nmemb, size_t size);
void *my_realloc(void *ptr, size_t size);

// 辅助函数（用于测试）
size_t get_chunk_size(chunk_t *chunk);
void set_chunk_size(chunk_t *chunk, size_t size, int flags);
int is_mmap_chunk(chunk_t *chunk);

// 初始化
void malloc_init();

// 调试接口
void malloc_dump_stats();
void malloc_dump_bins();
void malloc_dump_arenas();
void malloc_check_leaks();

// 线程局部存储键（用于存储每个线程的 arena）
extern pthread_key_t arena_key;

#ifdef __cplusplus
}
#endif

#endif // MALLOC_H