/* TinyOS 裸机 shim：stdlib.h 最小声明。malloc/free/realloc 走 lua_shim.c
 * 的固定 arena（Lua 状态独占 256KB）。 */
#ifndef TINYOS_SHIM_STDLIB_H
#define TINYOS_SHIM_STDLIB_H

#include <stddef.h>

typedef long          long_t;
typedef unsigned long ulong_t;
typedef unsigned int  uint_t;
typedef int           int32_t_shim;

void  *malloc(unsigned long n);
void  *calloc(unsigned long n, unsigned long sz);
void  *realloc(void *p, unsigned long n);
void   free(void *p);

int    rand(void);
void   srand(unsigned int seed);

int    abs(int x);
long   labs(long x);

long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float  strtof(const char *s, char **end);

char  *strerror(int err);
void   exit(int code);
int    atoi(const char *s);
unsigned long atoul(const char *s);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* os 库用到的其他系统调用（裸机占位） */
char *getenv(const char *name);
int   remove(const char *path);
int   rename(const char *old, const char *new);
char *tmpnam(char *s);
char *tempnam(const char *dir, const char *prefix);
int   system(const char *cmd);
#define L_tmpnam 64

#endif
