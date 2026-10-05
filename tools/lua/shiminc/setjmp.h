/* TinyOS 裸机 shim：setjmp.h
 * jmp_buf 用足够大的数组，交给 GCC 内建 __builtin_setjmp / __builtin_longjmp
 * 按其自身布局读写（-fno-omit-frame-pointer 保证帧指针可用，x87 一并保存）。 */
#ifndef TINYOS_SHIM_SETJMP_H
#define TINYOS_SHIM_SETJMP_H

/* 数组类型：作为参数传递时退化为指针（标准 C jmp_buf 语义，Lua 宏依赖它）。
   40 个 u32 = 160 字节，足够 __builtin_setjmp 保存全部 GPR + x87 状态。 */
typedef unsigned int jmp_buf[40];

/* glibc 风格：参数为 jmp_buf（数组退化为指针）。Lua 的 LUAI_TRY/THROW 宏
   直接 `setjmp((c)->b)` / `longjmp((c)->b, 1)`，b 即 luai_jmpbuf。 */
int  setjmp(jmp_buf b);
void longjmp(jmp_buf b, int v);
void abort(void);

#endif
