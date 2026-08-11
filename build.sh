#!/usr/bin/env bash
# build.sh —— 一键编译下位机
#
# 用法：  bash build.sh
# 产物：  build/controller_node

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "==> 配置 CMake"
cmake -B build -S .

echo "==> 编译"
cmake --build build -j

echo ""
echo "==> 编译完成，可执行文件在: build/controller_node"
echo "    真机运行(需 root + 已加载 IgH 内核模块): sudo ./build/controller_node"
