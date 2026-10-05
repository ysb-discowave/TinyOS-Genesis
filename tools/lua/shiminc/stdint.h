/* TinyOS 裸机 shim：stdint.h（Lua llimits.h 需要） */
#ifndef TINYOS_SHIM_STDINT_H
#define TINYOS_SHIM_STDINT_H

typedef unsigned char  uint8_t;
typedef signed char    int8_t;
typedef unsigned short uint16_t;
typedef signed short   int16_t;
typedef unsigned int   uint32_t;
typedef signed int     int32_t;
typedef unsigned long  uint64_t;
typedef signed long    int64_t;

typedef unsigned int   uintptr_t;
typedef signed int     intptr_t;
typedef unsigned long  uintmax_t;
typedef signed long    intmax_t;

#define UINT8_MAX  255U
#define INT8_MAX   127
#define UINT16_MAX 65535U
#define INT16_MAX  32767
#define UINT32_MAX 4294967295U
#define INT32_MAX  2147483647
#define UINT64_MAX 4294967295UL
#define UINTPTR_MAX 4294967295U
#define UINT_MAX   4294967295U
#define INT_MAX    2147483647

#endif
