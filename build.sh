#!/bin/bash

# 内存分配器构建脚本

set -e  # 遇到错误时退出

OUTPUT_DIR="output"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'  # No Color

# 打印带颜色的消息
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 检查命令是否存在
check_command() {
    if ! command -v "$1" &> /dev/null; then
        print_error "Command '$1' not found. Please install it first."
        exit 1
    fi
}

# 清理构建文件
clean() {
    print_info "Cleaning build files..."
    rm -rf "${OUTPUT_DIR:?}"/*
    print_success "Clean completed."
}

# 编译内存分配器
build() {
    print_info "Building memory allocator..."

    # 检查编译器
    check_command gcc

    # 创建输出目录
    mkdir -p "${OUTPUT_DIR}"

    # 编译测试程序
    echo "  [1/4] test_malloc..."
    gcc -Wall -Wextra -std=c17 -O2 -Iinclude -o "${OUTPUT_DIR}/test_malloc" src/malloc.c tests/test_malloc.c

    echo "  [2/4] test_thread..."
    gcc -Wall -Wextra -std=c17 -O2 -Iinclude -o "${OUTPUT_DIR}/test_thread" src/malloc.c tests/test_thread.c -lpthread

    echo "  [3/4] test_debug..."
    gcc -Wall -Wextra -std=c17 -O2 -Iinclude -o "${OUTPUT_DIR}/test_debug" src/malloc.c tests/test_debug.c

    echo "  [4/4] debug_malloc..."
    gcc -Wall -Wextra -std=c17 -O2 -Iinclude -o "${OUTPUT_DIR}/debug_malloc" src/malloc.c tests/debug_malloc.c -lpthread

    if [ $? -eq 0 ]; then
        print_success "Build completed successfully. All binaries in ${OUTPUT_DIR}/"
    else
        print_error "Build failed."
        exit 1
    fi
}

# 运行测试
test() {
    print_info "Running tests..."

    if [ ! -f "${OUTPUT_DIR}/test_malloc" ]; then
        print_warning "Binary not found, building first..."
        build
    fi

    ./"${OUTPUT_DIR}/test_malloc"

    if [ $? -eq 0 ]; then
        print_success "All tests passed!"
    else
        print_error "Tests failed."
        exit 1
    fi
}

# 运行 valgrind 检查
valgrind_check() {
    print_info "Running valgrind check..."

    if [ ! -f "${OUTPUT_DIR}/test_malloc" ]; then
        print_warning "Binary not found, building first..."
        build
    fi

    valgrind --leak-check=full --show-reachable=yes --track-origins=yes ./"${OUTPUT_DIR}/test_malloc"

    if [ $? -eq 0 ]; then
        print_success "Valgrind check completed."
    else
        print_error "Valgrind check failed."
        exit 1
    fi
}

# 显示帮助信息
show_help() {
    echo "Usage: $0 [command]"
    echo ""
    echo "Commands:"
    echo "  clean     Clean build files"
    echo "  build     Build the memory allocator"
    echo "  test      Run tests"
    echo "  valgrind  Run tests with valgrind"
    echo "  all       Build and run tests"
    echo "  help      Show this help message"
    echo ""
    echo "Example:"
    echo "  $0 all"
}

# 主函数
main() {
    case "$1" in
        clean)
            clean
            ;;
        build)
            build
            ;;
        test)
            test
            ;;
        valgrind)
            valgrind_check
            ;;
        all)
            clean
            build
            test
            valgrind_check
            ;;
        help|*)
            show_help
            ;;
    esac
}

# 执行主函数
main "$@"