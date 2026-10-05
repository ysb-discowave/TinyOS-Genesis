/* TinyOS 裸机 shim：string.h */
#ifndef TINYOS_SHIM_STRING_H
#define TINYOS_SHIM_STRING_H

#include <stddef.h>

void   *memcpy(void *dst, const void *src, unsigned long n);
const void *memchr(const void *s, int c, unsigned long n);
int     memcmp(const void *a, const void *b, unsigned long n);
void   *memset(void *dst, int c, unsigned long n);

unsigned long strlen(const char *s);
char    *strcpy(char *dst, const char *src);
char    *strncpy(char *dst, const char *src, unsigned long n);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, unsigned long n);
char    *strcat(char *dst, const char *src);
char    *strchr(const char *s, int c);
char    *strrchr(const char *s, int c);
char    *strdup(const char *s);
char    *strerror_shim(int e); /* 仅内部使用 */
const char *strstr(const char *hay, const char *needle);
char    *strpbrk(const char *s, const char *set);
unsigned long strspn(const char *s, const char *set);
unsigned long strcspn(const char *s, const char *set);
int     strcoll(const char *a, const char *b);

#endif
