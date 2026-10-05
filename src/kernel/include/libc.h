#ifndef TINYOS_LIBC_H
#define TINYOS_LIBC_H

#include "types.h"

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
void  *memchr(const void *p, int c, size_t n);
void   memset(void *p, int v, size_t n);
void   memcpy(void *dst, const void *src, size_t n);
void   memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
int    atoi(const char *s);
u32    strtoul(const char *s, const char **endp, int base);
char  *itoa(int v, char *buf, int base);
char  *utoa_hex(u32 v, char *buf, int upper);

/* 格式化：使用编译器内建变参，支持 %d %i %u %x %X %p %s %c %%，
 * 以及 '-' 左对齐、'0' 零填充、十进制宽度、'l' 长度修饰。 */
int    vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
int    snprintf(char *buf, size_t size, const char *fmt, ...);

#endif /* TINYOS_LIBC_H */
