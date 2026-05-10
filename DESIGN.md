# 设计原理 —— ptmalloc2 简化实现

## 概述

本项目实现了一个简化的 ptmalloc2 风格堆内存分配器，支持 `malloc`、`free`、`calloc`、`realloc` 核心接口。设计目标是在理解 glibc malloc 核心机制的基础上，实现一个功能正确、结构清晰的教学级分配器。

## 核心技术

### 1. Chunk 结构与边界标签（Boundary Tag）

```
┌──────────────────────────────┐
│ prev_size  (8 bytes)         │ ← 前一个 chunk 的大小（如果前一个空闲）
├──────────────────────────────┤
│ size       (8 bytes)         │ ← 当前 chunk 大小 + 标志位（低 3 位）
├──────────────────────────────┤
│ fd         (8 bytes)         │ ← 双向链表前向指针（仅空闲时有效）
├──────────────────────────────┤
│ bk         (8 bytes)         │ ← 双向链表后向指针（仅空闲时有效）
├──────────────────────────────┤
│ ... 用户数据 ...              │
└──────────────────────────────┘
```

**边界标签**是分配器的核心数据结构。每个内存块（chunk）的头部包含 `prev_size` 和 `size` 两个字段：

- **prev_size**：记录前一个 chunk 的大小。只有当当前 chunk 的 `PREV_INUSE` 标志位为 0 时，prev_size 才有效（表示前一个 chunk 是空闲的）。该设计使得**向后合并**时无需遍历链表即可定位前一个 chunk。
- **size**：记录当前 chunk 的大小（对齐到 16 字节），低 3 位用作标志位：
  - `PREV_INUSE (0x1)`：前一个 chunk 是否在使用中
  - `IS_MMAP (0x2)`：当前 chunk 是否通过 mmap 分配
  - 剩余 1 位保留

chunk 在使用中时，fd 和 bk 字段被用户数据覆盖，不额外占用空间。

### 2. 对齐（Alignment）

所有返回给用户的内存指针必须对齐到 16 字节（`ALIGNMENT = 16`）。对齐保证了：

- 符合大多数 CPU 的数据访问要求（SSE 指令需要 16 字节对齐）
- 减少内存碎片，使所有 chunk 大小均为 16 的倍数

对齐公式：`((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1)`

### 3. Bin 系统

空闲 chunk 按大小分类存储到 64 个 bin 中，以加速大小匹配。分类策略：

| 大小范围  | 粒度   | Bin 索引范围 | 说明                |
|-----------|--------|-------------|-------------------|
| ≤64B      | 16B    | 0-3         | 小内存精细分类       |
| 65-512B   | 256B   | 4-6         | 中等内存            |
| 513-4096B | 1024B  | 7-11        | 大内存              |
| >4096B    | -      | 62          | 统一归入未分类 bin   |

每个 bin 是一个**双向循环链表**。分配时从对应 bin 头部取出，释放时将 chunk 插入对应 bin。

**缺陷**：没有 fast bin（单链表，不触发合并）和 unsorted bin（最近释放的 chunk 缓存），导致小内存频繁分配释放时性能较差。

### 4. 分割（Split）

当从 bin 中取出一个 chunk 或从 top chunk 分配时，如果 chunk 大小显著大于请求大小（至少额外多出一个 chunk 头和 16 字节对齐），则将 chunk 分割为两部分：

```
┌──────────┬──────────────┐
│  已分配   │  剩余空闲     │
│  (size)  │  (remaining)  │
└──────────┴──────────────┘
```

关键约束：
- 剩余部分必须 ≥ `sizeof(chunk_t) + ALIGNMENT`，否则无法作为独立 chunk 管理
- 剩余部分的 `PREV_INUSE` 必须设为 1（因为左侧 chunk 正在使用）
- 分割后更新剩余部分后面 chunk 的 `prev_size`，维持边界标签一致性

### 5. 合并（Coalesce）

释放 chunk 时，检查相邻 chunk 是否空闲，如果空闲则合并为更大的空闲 chunk。合并分为两个方向：

**向后合并（与上一个 chunk）：**

```c
// 检查当前 chunk 的 PREV_INUSE 标志
if (!(chunk->size & PREV_INUSE) && chunk->prev_size > 0) {
    prev = (chunk_t *)((char *)chunk - chunk->prev_size);
    // prev 为空闲，从 bin 中移除并合并
    merged_size = prev_size + current_size;
    next->prev_size = merged_size;  // 更新下一个 chunk 的 prev_size
    set_chunk_size(prev, merged_size, PREV_INUSE);
}
```

**向前合并（与下一个 chunk）：**

```c
if (!(next->size & PREV_INUSE) && get_chunk_size(next) > 0) {
    // next 为空闲，从 bin 中移除并合并
    merged_size = current_size + next_size;
    next2->prev_size = merged_size;  // 更新再下一个 chunk 的 prev_size
}
```

关键要点：
- 向后合并必须检查 `prev_size > 0`，否则 `chunk - 0 == chunk`，导致与自身合并
- 向前合并必须检查 `get_chunk_size(next) > 0`，防止与 size=0 的无效 chunk 合并（如新 top chunk 未初始化）
- 合并后必须更新相邻 chunk 的 prev_size，维持边界标签一致性

### 6. Arena 管理

Arena 是分配器管理的内存区域，每个 arena 包含一个 top chunk 和统计信息。

**Top Chunk：** arena 顶部的最后一个 chunk，其上方是未映射的内存。当 top chunk 不够大时，通过 sbrk 扩展堆。

**扩展机制：**

1. 计算需要的大小：`needed = size - top_size`
2. 调用 `sbrk(needed + sizeof(chunk_t))` 扩展堆
   - 额外 `sizeof(chunk_t)` 用于新 top 的 chunk 头
3. 将旧的 top chunk 标记为已分配
4. 在扩展区域的末尾设置新的 top chunk，包含有效的 `size` 和 `prev_size`

**为什么不能分配小于 sizeof(chunk_t) 的 top？** 因为后续分配操作需要读取 `top->size`，如果 top 头写入到 program break 之外，读取时将触发段错误。

**当前局限：** 所有线程共享一个 main_arena，存在锁竞争。完整实现应为每个 CPU 分配独立 arena。

### 7. mmap 大内存分配

对于大小超过 4096 字节的分配请求，且 bin 和 arena 都无法满足时，使用 mmap 分配：

- mmap 分配的 chunk 设置 `IS_MMAP` 标志
- 释放时直接调用 `munmap`，不经过 coalesce/bin 系统
- 优点：大内存释放后立刻归还操作系统，不产生堆碎片
- 缺点：mmap/unmap 是系统调用，开销较大

### 8. 初始化和循环依赖

分配器初始化存在一个微妙的循环依赖问题：

```
init_malloc() → init_arena() → sbrk() 分配 arena 结构
                            ↓
                    不能调用 my_malloc()
```

`my_malloc()` 会检查 `malloc_state.main_arena` 是否为空，如果为空则调用 `init_malloc()`。如果 `init_arena()` 内部调用 `my_malloc()` 来分配 arena 结构，会导致无限递归。

**解决：** arena 结构体通过 `sbrk()` 直接分配，不经过 my_malloc。

### 9. sbrk 与 64 位兼容性

在 64 位 Linux 系统上，`sbrk()` 的隐含声明（返回 `int`）会导致指针截断：

- `sbrk()` 实际返回 `void *`（64 位地址）
- 隐含声明假定返回 `int`（32 位），高 32 位被丢弃
- 结果：分配器在低 4GB 地址空间可能工作，超出则崩溃

**解决：** 显式声明 `extern void *sbrk(intptr_t increment);` 并包含 `<unistd.h>`。

## 数据流

### 分配路径（my_malloc）

```
my_malloc(size)
  │
  ├─ size == 0 → return NULL
  │
  ├─ 首次调用 → init_malloc() → init_arena()
  │
  ├─ 在对应 bin 中查找
  │   ├─ 找到 → remove_from_bin()
  │   │         ├─ chunk 太大 → split_chunk()，剩余放回 bin
  │   │         └→ 返回用户指针
  │   │
  │   └─ 未找到 → get_chunk_from_arena()
  │               ├─ top 够大 → split_chunk() if needed
  │               ├─ top 不够 → sbrk 扩展
  │               └─ 超大请求 → mmap
  │
  └─ 更新统计信息 → return ptr
```

### 释放路径（my_free）

```
my_free(ptr)
  │
  ├─ ptr == NULL → return
  │
  ├─ IS_MMAP chunk → munmap → return
  │
  ├─ coalesce_chunk() 合并相邻空闲 chunk
  │   ├─ 向后合并（prev 空闲 → 合并）
  │   └─ 向前合并（next 空闲 → 合并）
  │
  └─ insert_into_bin() 放入对应 bin
```

## 调试系统

分配器内置了调试功能：

- **分配追踪**：每次 my_malloc 记录分配信息到 `debug_info` 数组（最多 10000 条）
- **释放追踪**：my_free 时标记对应记录为已释放
- **泄漏检测**：`malloc_check_leaks()` 遍历所有记录，找出未释放的分配
- **统计信息**：`malloc_dump_stats()` 打印总分配/释放次数、当前活跃分配数、总字节数

**局限**：`record_allocation` 使用 `__FILE__` 和 `__LINE__` 时记录的是 malloc.c 内部的行号，不是调用者的位置。需要宏包装（如 `#define my_malloc(s) my_malloc_tag(s, __FILE__, __LINE__)`）才能追踪真实调用点。
