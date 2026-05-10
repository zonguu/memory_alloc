# 可实现的优化特性

本文档列出当前分配器可以添加的优化特性，按优先级排序。

## 1. 多线程 Arena 扩展（高优先级）

**当前状态：** 所有线程共享一个 main_arena，全局锁保护 bin 操作，arena 锁保护 top chunk 操作。多线程下锁竞争严重。

**实现方案：**

```
每个线程通过 TLS（线程局部存储）持有独立的 arena
├─ 线程首次调用 my_malloc → 创建新 arena
├─ 减少锁冲突：线程主要操作自己的 arena
└─ 内存不足时从其他 arena "偷"内存（locking）
```

**涉及的代码区域：**
- `get_current_arena()` → 改为 TLS key 查询
- 需要 `pthread_key_create()` 初始化 arena key
- 需要线程退出时注册析构函数释放 arena
- 参考 glibc 的 `arena_get()` / `arena_put()` 机制

**参考实现：** glibc malloc 的 per-thread arena 设计，每个 arena 有独立锁，线程尝试 lock arena，失败则尝试下一个。

## 2. Fast Bin（高优先级）

**当前状态：** 小内存（≤64B）释放时触发 coalesce，合并后下次分配需要再从 bin 中分割，开销大。

**实现方案：**

```c
#define NBINS 64
#define FASTBIN_MAX 72  // 最大 fast bin 大小（不含 chunk 头）

// Fast bin 使用单链表（而非双向循环链表）
// 插入：LIFO 策略，头插法
// 取出：直接取链表头
// 关键：fast bin 中的 chunk 不设置 PREV_INUSE=0（不触发 coalesce）
```

**优点：**
- 小内存分配释放 O(1) 时间复杂度
- 减少内存抖动，避免小 chunk 频繁合并-分割的开销

**参考：** glibc 中最多 10 个 fast bin，每个间隔 16 字节，LIFO 策略。

## 3. Unsorted Bin（中优先级）

**当前状态：** 释放的 chunk 直接插入到对应大小 bin。如果请求的大小没有精确匹配的 bin chunk，需要遍历所有 bin 查找可用 chunk。

**实现方案：**

```
释放时 chunk 先进入 unsorted bin（bin 63）
分配时优先检查 unsorted bin：
├─ 精确匹配 → 直接使用
├─ 大于请求 → 分割，剩余放回 unsorted bin
└─ 不匹配 → 移入对应大小的 regular bin
```

**优点：**
- 最近释放的内存可以快速重用（时间局部性）
- 减少精确匹配的开销
- 分配时一次性将 unsorted bin 中的 chunk 分类到 regular bin

## 4. Last Remainder 优化（中优先级）

**当前状态：** 每次从 top chunk 分配后，剩余部分保持为 top。连续分配会不断从 top 切分，产生大量碎片。

**实现方案：**

```
如果从 unsorted bin 中分割了一个 chunk，剩余部分标记为 last remainder
后续小内存分配优先从 last remainder 中分割
```

**优点：**
- 减少 top chunk 的频繁扩展
- 提高连续小分配的空间局部性（地址相邻）

## 5. 内存碎片整理（中优先级）

**当前状态：** 分配器只做基本的 coalesce（合并相邻空闲 chunk），无法处理非相邻的碎片。运行时间越长，碎片越严重。

**实现方案：**

```
A. 内存 compaction：
   ├─ 定期整理堆，移动已分配 chunk 以合并空闲区域
   └─ 更新所有用户指针（类似 stop-the-world GC）

B. 统计追踪：
   ├─ 记录 fragmentation ratio = (总空闲 / 最大连续空闲)
   └─ fragmentation > 阈值时触发整理

C. 阈值限制：
   └─ 合并后 chunk 超过 128KB 时归还给操作系统（通过 madvise 或 sbrk）
```

**注意：** 方案 A 实现复杂且需要 GC 风格的根集扫描，在纯 C 中实现较为困难。方案 B 和 C 相对简单。

## 6. 自动内存泄漏检测（低优先级）

**当前状态：** 泄漏检测需要手动调用 `malloc_check_leaks()`，且只能检测程序退出时的泄漏。

**扩展方案：**

```
A. atexit 注册：
   ├─ 程序退出时自动调用 malloc_check_leaks()
   └─ 有泄漏时打印详细报告并在 stderr 输出
   └─ 可配置是否触发 abort()

B. 循环检测告警：
   ├─ 跟踪当前活跃分配数 current_allocations
   └─ 当活跃分配数持续增长超过阈值时告警

C. 调用者追踪：
   ├─ 通过宏包装捕获真实调用位置
   ├─ #define my_malloc(s) my_malloc_tag(s, __FILE__, __LINE__)
   └─ debug_info 记录真实文件名和行号
```

## 7. 平台兼容性（低优先级）

**当前状态：** 仅支持 Linux（依赖 sbrk 和 mmap）。

**扩展方案：**

| 平台   | 替代 API                      | 注意事项                       |
|--------|------------------------------|-------------------------------|
| macOS  | mach_vm_allocate             | sbrk 已废弃，需用 mach VM API    |
| Windows| VirtualAlloc / HeapAlloc     | 没有 sbrk，需用 VirtualAlloc 替代|
| WASM   | 无系统调用                   | 需预分配静态堆 + 不同编译配置     |

## 8. 性能优化（低优先级）

**当前状态：** 基础实现，未做性能优化。

**可以实现的优化：**

```
A. 锁细化：
   ├─ 当前：全局锁保护所有 bin 操作
   ├─ 优化：每个 bin 独立自旋锁
   └─ 减少无关操作的锁竞争

B. 批量调用优化：
   └─ sbrk 扩展时一次性分配更大块（如 64KB），减少系统调用次数

C. 编译优化：
   ├─ 关键函数使用 always_inline
   ├─ 使用 likely/unlikely 分支预测提示
   └─ 减少函数调用开销

D. CPU 缓存优化：
   ├─ 优先分配最近释放的 chunk（缓存热度高）
   ├─ chunk 大小按 cache line（64B）对齐
   └─ 避免 false sharing（多线程相邻内存访问冲突）
```

## 9. 安全加固（低优先级）

**当前状态：** 分配器不做任何安全检查。

**可以加固的方面：**

```c
// A. 释放后标记
#define FREED_PATTERN 0xDEADBEAF

void my_free(void *ptr) {
    // ...
    memset(ptr, 0xDEADBEAF, size);  // 写标记模式
}

// B. 分配前检查
// - 检测 ptr 是否在合法的堆范围内
// - 检测 chunk 的 magic number

// C. 溢出检测
// - 在用户数据前后添加 canary（金丝雀值）
// - my_free 时检查 canary 是否被修改
```

## 10. 用户态统计与调优接口（低优先级）

```c
// 运行时配置
typedef struct malloc_config {
    size_t max_arena_size;     // arena 最大大小
    int enable_fast_bin;       // 启用 fast bin
    int enable_leak_detection; // 启用泄漏检测
    int compaction_threshold;  // 碎片整理阈值 (%)
} malloc_config_t;

void malloc_set_config(malloc_config_t *config);
void malloc_get_stats(malloc_stats_t *stats);  // 导出详细统计
```

## 优先级总结

| 特性             | 优先级   | 复杂度 | 收益               |
|------------------|---------|--------|-------------------|
| 多线程 Arena     | 高      | 高     | 多线程性能大幅提升   |
| Fast Bin         | 高      | 低     | 小内存分配 O(1)    |
| Unsorted Bin     | 中      | 中     | 分配灵活性提升      |
| Last Remainder   | 中      | 低     | 减少 top 扩展      |
| 碎片整理         | 中      | 高     | 长期运行稳定性      |
| 自动泄漏检测     | 低      | 低     | 调试效率提升        |
| 平台兼容性       | 低      | 中     | 跨平台支持          |
| 性能优化         | 低      | 中     | 提升运行速度        |
| 安全加固         | 低      | 低     | 防御内存错误        |
| 调优接口         | 低      | 低     | 可观测性提升        |

**建议开发顺序：** Fast Bin → Unsorted Bin → 多线程 Arena → Last Remainder → 自动泄漏检测 → 其余按需。
