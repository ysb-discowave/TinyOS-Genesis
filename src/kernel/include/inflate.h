#ifndef TINYOS_INFLATE_H
#define TINYOS_INFLATE_H

#include "types.h"

/* 纯 C DEFLATE (RFC1951 raw 流) 解码器，kernel/inflate.c。
 * src 是 raw-deflate 字节流（无 zlib 2 字节头、无 adler32），
 * Python 侧用 zlib.compressobj(level, zlib.DEFLATED, -15) 生成。
 * dst 由调用者给足（TNCR 解压 = 分配 orig_len 字节即可）。
 * 返回写入 dst 的字节数；失败 = (u32)-1。 */
u32 inflate_raw(const u8 *src, u32 src_len, u8 *dst, u32 dst_cap);

#endif
