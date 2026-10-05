#ifndef TINYOS_TYPES_H
#define TINYOS_TYPES_H

/* 基础类型（freestanding 环境，无标准库） */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

typedef signed char    s8;
typedef short          s16;
typedef int            s32;
typedef long long      s64;

typedef u32 size_t;
typedef s32 ssize_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define TRUE  1
#define FALSE 0
typedef u8 bool;

#define PANIC(msg) panic(__FILE__, __LINE__, msg)

/* 致命错误停摆（在 kernel.c 中实现） */
void panic(const char *file, int line, const char *msg);

/* 编译器屏障 */
#define barrier() __asm__ volatile("" ::: "memory")

#endif /* TINYOS_TYPES_H */
