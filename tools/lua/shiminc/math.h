/* TinyOS 裸机 shim：math.h。Lua 编译时允许 x87（-mno-mmx 只禁 64 位 SIMD，
 * 不限制 80 位 FPU），float 运算由 zig cc 默认生成 8087 指令。 */
#ifndef TINYOS_SHIM_MATH_H
#define TINYOS_SHIM_MATH_H

#include <float.h>

#define HUGE_VAL      1e308
#define HUGE_VALF     1e308f
#define HUGE_VALL     1e308
#define HUGE_VAL_DOUBLE 1e308
#define NAN  (0.0 / 0.0)
#define INFINITY (1.0 / 0.0)

double floor(double x);
double ceil(double x);
double fabs(double x);
double sqrt(double x);
double fmod(double a, double b);
double round(double x);
double trunc(double x);
double log(double x);
double log2(double x);
double log10(double x);
double exp(double x);
double pow(double base, double e);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double modf(double x, double *ip);
double hypot(double x, double y);
double cbrt(double x);
int    ilogb(double x);
double ldexp(double x, int exp);
double frexp(double x, int *exp);

static inline int isfinite_shim(double x) { return !(x != x) && x != INFINITY && x != -INFINITY; }
#define isfinite(x) isfinite_shim(x)
static inline int isnan_shim(double x) { return x != x; }
#define isnan(x) isnan_shim(x)
static inline int isinf_shim(double x) { return x == INFINITY || x == -INFINITY; }
#define isinf(x) isinf_shim(x)

#endif
