/* TinyOS 裸机 shim：float.h */
#ifndef TINYOS_SHIM_FLOAT_H
#define TINYOS_SHIM_FLOAT_H

/* x87 80-bit 扩展精度（i686 默认实数即 80-bit，double 存取按 IEEE-754 64-bit） */
#define FLT_MANT_DIG  23
#define DBL_MANT_DIG  53
#define DBL_MIN       2.2250738585072014e-308
#define DBL_MAX       1.7976931348623158e308
#define DBL_MIN_EXP   (-1021)
#define DBL_MAX_10_EXP 308
#define DBL_MIN_10_EXP (-307)
#define DBL_EPSILON   2.2204460492503131e-16
#define FLT_EPSILON   1.19209290e-7F
#define LDBL_MANT_DIG 64
#define LDBL_EPSILON  1.0842021724855044e-19

#endif
