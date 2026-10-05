/* TinyOS 裸机 shim：stdio.h
 * FILE 是"内存文件"（数据从 VFS 读进 Lua 堆），lua_main.c 里实现全部
 * FILE 操作。原型签名必须与 lua_main.c / lua_shim.c 的定义一致
 * （size 参数统一用 unsigned int，对应 u32）。 */
#ifndef TINYOS_SHIM_STDIO_H
#define TINYOS_SHIM_STDIO_H

#include <stdarg.h>

#define EOF   (-1)
#define BUFSIZ 256
#define _IONBF 0
#define _IOFBF 1
#define _IOLBF 2
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#ifndef NULL
#define NULL 0
#endif

typedef struct FILE {
    char        *data;     /* VFS 读进来的缓冲 */
    unsigned int pos, len;
    int          owner;    /* 1 = close 时 free */
} FILE;

FILE *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
int    fgetc(FILE *f);
int    ungetc(int c, FILE *f);
int    getc(FILE *f);
int    putc(int c, FILE *f);
int    fputc(int c, FILE *f);
FILE  *freopen(const char *path, const char *mode, FILE *f);
long   fread(void *dst, unsigned int sz, unsigned int n, FILE *f);
long   fwrite(const void *src, unsigned int sz, unsigned int n, FILE *f);
char  *fgets(char *s, int n, FILE *f);
long   ftell(FILE *f);
int    fseek(FILE *f, long off, int whence);
int    fflush(FILE *f);
void   clearerr(FILE *f);
int    fileno(FILE *f);
int    setvbuf(FILE *f, char *buf, int mode, unsigned int sz);
FILE  *tmpfile(void);

int printf(const char *fmt, ...);
int snprintf(char *dst, unsigned int nmax, const char *fmt, ...);
int sprintf(char *dst, const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);

extern FILE *stdout;
extern FILE *stderr;
extern FILE *stdin;

#endif
