# ptmalloc2 简化版内存分配器

这是一个基于 ptmalloc2 设计的简化版内存分配器实现，使用 C 语言编写。

## 特性

- **Chunk 管理**：基本的 chunk 结构，包含大小、前向/后向指针
- **Bins 系统**：64 个 bin 用于不同大小的内存块分类
- **Arena 管理**：主 arena 初始化和动态内存扩展
- **mmap 集成**：大内存块（>4KB）使用 mmap 直接映射
- **标准接口**：`malloc()`, `free()`, `calloc()`, `realloc()`

## 构建和测试

### 使用 build.sh 脚本

```bash
./build.sh [command]

# 命令选项：
#   clean     - 清理构建文件
#   build     - 构建内存分配器
#   test      - 运行测试
#   valgrind  - 使用 valgrind 运行测试
#   all       - 完整构建和测试流程
#   help      - 显示帮助信息
```

### 手动构建

```bash
make          # 编译测试程序
make test     # 运行测试
```

## 使用示例

```c
#include "include/malloc.h"

int main() {
    // 初始化分配器
    malloc_init();
    
    // 分配内存
    void *ptr1 = malloc(16);
    void *ptr2 = malloc(32);
    
    // 使用内存...
    
    // 释放内存
    free(ptr1);
    free(ptr2);
    
    return 0;
}
```

## 实现细节

### Chunk 结构

```c
typedef struct chunk {
    size_t prev_size;      // 前一个 chunk 的大小（如果前一个 chunk 是空闲的）
    size_t size;           // 当前 chunk 的大小和标志位
    struct chunk *fd;      // 双向链表的前向指针
    struct chunk *bk;      // 双向链表的后向指针
} chunk_t;
```

### Bin 索引计算

```c
#define BIN_INDEX(size) ((size) <= 64 ? ((size) >> 4) : \
                       (size) <= 512 ? (((size) >> 8) + 4) : \
                       (size) <= 4096 ? (((size) >> 10) + 8) : \
                       62)
```

### 内存对齐

```c
#define ALIGNMENT 16
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
```

## 注意事项

- 程序在 valgrind 下运行正常，但直接运行时可能出现段错误（可能是由于 sbrk 的使用方式）
- 这是一个学习性质的实现，生产环境中应使用系统提供的 malloc
- 可以根据需要扩展多线程支持、更复杂的 bin 算法等

## 缺失的特性（待实现）

1. **多线程支持**
   - 多 arena 支持（每个线程有自己的 arena）
   - 线程安全的内存分配
   - arena 锁机制

2. **更复杂的 bin 算法**
   - Fast bins 优化
   - Unsorted bin 支持
   - Last remainder chunk 优化

3. **内存优化**
   - Top chunk 优化
   - 内存碎片减少算法
   - 内存回收策略

4. **调试和诊断**
   - 内存使用统计
   - 内存泄漏检测
   - 调试模式支持

5. **平台兼容性**
   - Windows 支持（使用 VirtualAlloc）
   - macOS 支持
   - 更好的错误处理

6. **性能优化**
   - 批量分配/释放
   - 内存预分配
   - 缓存友好设计

7. **高级功能**
   - 内存池支持
   - 对齐分配
   - 内存映射文件支持

## 许可证

本项目仅供学习和研究目的使用。