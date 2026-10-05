/* TinyOS 裸机 shim：stddef.h */
#ifndef TINYOS_SHIM_STDDEF_H
#define TINYOS_SHIM_STDDEF_H

typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef int wchar_t;
typedef struct { int __align[2]; } va_list_dummy;

#define NULL 0
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
