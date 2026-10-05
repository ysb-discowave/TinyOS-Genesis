#!/usr/bin/env bash
# TinyOS 交叉工具链安装脚本（在 Linux / WSL / Cygwin 上运行）
# 安装：i686-elf-gcc、nasm、qemu-system-i386、grub-mkrescue
# 仅安装“构建与运行”所需；不修改系统全局，除 apt/dnf/pacman 包管理调用。
set -e

echo "== TinyOS 工具链安装 =="

if command -v apt-get >/dev/null 2>&1; then
  echo "[apt] 更新并安装构建依赖..."
  sudo apt-get update
  sudo apt-get install -y build-essential nasm qemu-system-x86 grub-pc-bin xorriso \
       bison flex libgmp-dev libmpfr-dev libmpc-dev texinfo wget
  if ! command -v i686-elf-gcc >/dev/null 2>&1; then
    echo "[apt] 未提供 i686-elf-gcc，开始从源码构建交叉编译器..."
    ./tools/build-cross.sh
  fi
elif command -v dnf >/dev/null 2>&1; then
  sudo dnf install -y gcc make nasm qemu-system-x86 grub2-pc-modules xorriso \
       gmp-devel mpfr-devel libmpc-devel texinfo wget
  if ! command -v i686-elf-gcc >/dev/null 2>&1; then ./tools/build-cross.sh; fi
elif command -v pacman >/dev/null 2>&1; then
  sudo pacman -S --needed base-devel nasm qemu grub libisoburn
  if ! command -v i686-elf-gcc >/dev/null 2>&1; then ./tools/build-cross.sh; fi
else
  echo "未识别的包管理器。请手动安装：i686-elf-gcc, nasm, qemu-system-i386, grub-mkrescue"
  exit 1
fi

echo "== 完成。运行 'make' 与 'make run' 即可启动 TinyOS =="
