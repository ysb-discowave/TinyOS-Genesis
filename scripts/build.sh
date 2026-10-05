#!/usr/bin/env bash
# TinyOS Genesis 构建脚本（独立发行版 v0.1）
#
# 本仓库自包含：不依赖任何外部仓库，内核源码在 src/kernel，
# 用户程序构建链在 tools/，Lua 源码在 thirdparty/lua。
#
# 用法：
#   ./scripts/build.sh system     # 完整构建：用户程序 -> romfs -> 内核 -> 可启动镜像
#   ./scripts/build.sh host       # 编译宿主机可运行版 tinysh（开发自测）
#   ./scripts/build.sh tinyos     # 只交叉编译 tinysh.TNCR
#   ./scripts/build.sh test       # 运行 host 自测会话
#
# 默认（无参数）= host
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ZIG="${ZIG:-zig}"

mode="${1:-host}"

case "$mode" in
  system)
    # 完整构建。Windows 上内核链接/打包交给 build.ps1（需要 nasm + zig）。
    # 这里先跑用户程序链（生成 src/kernel/romfs.c），再调 build.ps1。
    echo "[build] user programs (tinysh / Lua) -> romfs"
    python3 "$ROOT/tools/build_users.py"
    if command -v powershell.exe >/dev/null 2>&1; then
      echo "[build] kernel + bootable image (build.ps1)"
      powershell.exe -ExecutionPolicy Bypass -File "$ROOT/build.ps1"
    else
      echo "error: build.ps1 needs PowerShell (Windows)." >&2
      echo "       On Linux/macOS use: zig cc / nasm directly (see docs/)." >&2
      exit 1
    fi
    echo "[build] done -> $ROOT/tinyos.img"
    ;;
  host)
    echo "[build] host tinysh (kernel_api_host.c)"
    "$ZIG" cc -std=c11 -O2 -Wall -Wextra \
      -o build/tinysh_host src/tinysh/tinysh.c src/tinysh/kernel_api_host.c
    echo "[build] -> build/tinysh_host"
    ;;
  tinyos)
    # 产品构建：链接 kernel_api_tinyos.c 产出 /bin/tinysh.TNCR。
    # 内核头文件就在本仓库 src/kernel/include，无需外部 TinyOS-2.0。
    KERNEL_INC="$ROOT/src/kernel/include"
    : "${ZIG:?set ZIG to the zig executable}"
    if [ ! -d "$KERNEL_INC" ]; then
      echo "error: kernel headers not found at $KERNEL_INC" >&2
      exit 1
    fi
    echo "[build] tinyos tinysh.TNCR (kernel_api_tinyos.c)"
    # 真正的 TNCR 打包由 tools/tinysh/build_tinysh.py 完成
    # （它会调 zig 交叉编译 + elf32_flatten + pack_tncr）。
    "$ZIG" version >/dev/null
    python3 "$ROOT/tools/tinysh/build_tinysh.py"
    echo "[build] -> tinysh.TNCR (see \$TINYOS_OUT/tncr/)"
    ;;
  test)
    mkdir -p build
    "$ZIG" cc -std=c11 -O2 -o build/tinysh_host src/tinysh/tinysh.c src/tinysh/kernel_api_host.c
    echo "[test] running self-test session"
    ./build/tinysh_host < src/tinysh/test_session.txt
    ;;
  *)
    echo "usage: $0 {system|host|tinyos|test}" >&2
    exit 1
    ;;
esac
