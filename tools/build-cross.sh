#!/usr/bin/env bash
# 从源码构建 i686-elf 交叉编译器（当系统未提供时调用）
# 产物安装在 $PREFIX（默认 /opt/cross），需在 PATH 中。
set -e
PREFIX="${PREFIX:-/opt/cross}"
TARGET=i686-elf
BINUTILS_VER=2.41
GCC_VER=13.2.0
JOBS="$(nproc 2>/dev/null || echo 4)"
WORK="$(mktemp -d)"
export PATH="$PREFIX/bin:$PATH"

sudo mkdir -p "$PREFIX"
echo "安装前缀: $PREFIX  目标: $TARGET"

cd "$WORK"
echo "下载 binutils $BINUTILS_VER ..."
wget -q "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz"
tar xf "binutils-$BINUTILS_VER.tar.xz"
mkdir build-binutils && cd build-binutils
../"binutils-$BINUTILS_VER"/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
make -j"$JOBS"
sudo make install

cd "$WORK"
echo "下载 gcc $GCC_VER ..."
wget -q "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz"
tar xf "gcc-$GCC_VER.tar.xz"
mkdir build-gcc && cd build-gcc
../"gcc-$GCC_VER"/configure --target=$TARGET --prefix="$PREFIX" --disable-nls \
      --enable-languages=c,c++ --without-headers --disable-libssp --disable-libquadmath
make -j"$JOBS" all-gcc
sudo make install-gcc

echo "交叉编译器已安装到 $PREFIX/bin。请将其加入 PATH："
echo "  export PATH=\"$PREFIX/bin:\$PATH\""
