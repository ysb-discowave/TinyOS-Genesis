#!/usr/bin/env bash
# TinyOS Genesis 构建脚本（首发行版 v0.1）
#
# 用法：
#   ./scripts/build.sh host        # 编译宿主机可运行版 tinysh（开发自测）
#   ./scripts/build.sh tinyos     # 交叉编译 /bin/tinysh.tncr（需 TinyOS 2.0 工具链）
#   ./scripts/build.sh test        # 运行 host 自测会话
#
# 默认（无参数）= host
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ZIG="${ZIG:-zig}"

mode="${1:-host}"

case "$mode" in
  host)
    echo "[build] host tinysh (kernel_api_host.c)"
    "$ZIG" cc -std=c11 -O2 -Wall -Wextra \
      -o build/tinysh_host src/tinysh/tinysh.c src/tinysh/kernel_api_host.c
    echo "[build] -> build/tinysh_host"
    ;;
  tinyos)
    # 产品构建：链接 kernel_api_tinyos.c，定义 TINYOS_USER 启用 user_main 入口。
    # 需提供：TinyOS 2.0 内核 include 目录（-DTINYOS_KERNEL_ROOT=...）
    #        以及 TNCR stdio shim（将 printf/fgets 映射到 tinyos_api_t）。
    : "${TINYOS_KERNEL_ROOT:?set TINYOS_KERNEL_ROOT to TinyOS-2.0/kernel/include}"
    echo "[build] tinyos tinysh.tncr (kernel_api_tinyos.c)"
    "$ZIG" cc -std=c11 -O2 -DTINYOS_USER \
      -DTINYOS_KERNEL_ROOT="$TINYOS_KERNEL_ROOT" \
      -I"$TINYOS_KERNEL_ROOT" \
      -o build/tinysh.tncr src/tinysh/tinysh.c src/tinysh/kernel_api_tinyos.c
    echo "[build] -> build/tinysh.tncr"
    ;;
  test)
    mkdir -p build
    "$ZIG" cc -std=c11 -O2 -o build/tinysh_host src/tinysh/tinysh.c src/tinysh/kernel_api_host.c
    echo "[test] running self-test session"
    ./build/tinysh_host < src/tinysh/test_session.txt
    ;;
  *)
    echo "usage: $0 {host|tinyos|test}" >&2
    exit 1
    ;;
esac
