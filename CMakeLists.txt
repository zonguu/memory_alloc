cmake_minimum_required(VERSION 3.10)
project(MemoryAllocator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 启用C语言
enable_language(C)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output)

# 查找GTest
find_package(GTest REQUIRED)

# 包含目录
include_directories(include)

# 源文件
set(SOURCES
    src/malloc.c
    tests/test_all.cpp
)

# 创建可执行文件
add_executable(memory_allocator_tests ${SOURCES})

# 设置 malloc.c 的编译语言为 C
set_source_files_properties(src/malloc.c PROPERTIES LANGUAGE C)

# 链接GTest库
target_link_libraries(memory_allocator_tests GTest::gtest GTest::gtest_main pthread)

# 添加测试
enable_testing()
add_test(NAME MemoryAllocatorTests COMMAND memory_allocator_tests)