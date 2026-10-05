/* ============================================================================
 * lua_shim.c -- 裸机 libc shim：让原版 Lua 5.4 核心在 TinyOS 里编译通过。
 *
 * Lua 源码零改动（lmem.c 的 malloc 钩子原样工作），所有"系统"能力收敛到
 * 本文件 + lua_main.c。
 *
 * 内存布局（无 MMU，物理直映射）：
 *   0x01000000  本 TNCR 代码映像（<=512KB，TNCR_LOAD_MAX）
 *   0x01400000  Lua 堆（arena，16MB；首块空闲）
 * ============================================================================ */
#include "api_user.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

extern tinyos_api_t *g_lua_api;      /* lua_main.c：内核 API 表 */
extern u32  g_lua_ticks(void);       /* lua_main.c：转发 api->ticks() */

typedef unsigned long time_t_;       /* = time_t（shim） */
typedef long          clock_t;

/* 本文件后段才定义，先声明（单 TU 内前向调用） */
unsigned long strlen(const char *s);
void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
int snprintf(char *dst, unsigned int nmax, const char *fmt, ...);

/* ====================================================================
 * 内存：固定 arena
 * ==================================================================== */
#define HEAP_BASE   0x01400000u
#define HEAP_SIZE   (16u * 1024u * 1024u)
#define ALIGN16(x)  (((x) + 15u) & ~15u)

static u32 g_heap_used = 0;

void *malloc(unsigned long n) {
    if (n == 0) n = 1;
    u32 total = (u32)ALIGN16(n + 4);
    if (g_heap_used + total > HEAP_SIZE) return 0;
    u8 *p = (u8*)(HEAP_BASE + g_heap_used);
    g_heap_used += total;
    *(u32*)p = (u32)n;
    return p + 4;
}
void *calloc(unsigned long n, unsigned long sz) {
    unsigned long t = n * sz;
    void *p = malloc(t ? t : 1);
    if (p) memset(p, 0, t);
    return p;
}
void *realloc(void *p, unsigned long n) {
    if (!p) return malloc(n);
    u32 old = *(u32*)((u8*)p - 4);
    if ((u32)n <= old) return p;
    void *q = malloc(n);
    if (q) memcpy(q, p, old);
    return q;
}
void free(void *p) { /* arena：不真回收 */ }

/* ====================================================================
 * 字符串
 * ==================================================================== */
unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
void *memcpy(void *d, const void *s, unsigned long n) {
    u8 *dd = d; const u8 *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, unsigned long n) {
    u8 *dd = d;
    while (n--) *dd++ = (u8)c;
    return d;
}
const void *memchr(const void *s, int c, unsigned long n) {
    const u8 *p = s;
    while (n--) { if (*p == (u8)c) return p; p++; }
    return 0;
}
int memcmp(const void *a, const void *b, unsigned long n) {
    const u8 *pa = a, *pb = b;
    while (n--) { if (*pa != *pb) return (int)*pa - (int)*pb; pa++; pb++; }
    return 0;
}
char *strcpy(char *d, const char *s) { char *o = d; while ((*d++ = *s++)); return o; }
char *strncpy(char *d, const char *s, unsigned long n) {
    char *o = d;
    while (n-- && *s) *d++ = *s++;
    while (n--) *d++ = 0;
    return o;
}
char *strcat(char *d, const char *s) { d += strlen(d); return strcpy(d, s); }
char *strchr(const char *s, int c) {
    while (*s && *s != c) s++;
    return (*s == c) ? (char*)s : 0;
}
char *strrchr(const char *s, int c) {
    char *last = 0;
    while (*s) { if (*s == c) last = (char*)s; s++; }
    return last;
}
char *strdup(const char *s) {
    unsigned long n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}
int strncmp(const char *a, const char *b, unsigned long n) {
    /* 注意：不能用 while(n-- && ...) + if(n==0) 的写法。
     * 后置递减会在退出循环时把 n 减到下溢值，导致 n==0 判定失效，
     * 于是"前 n 字符全匹配"这种最常见的情况反而返回非零
     * （如 strncmp("name=x","name",4) 返回 'x'-0），把所有
     * key=value 解析全部判成"不匹配"。这里显式先判 n>0。 */
    while (n > 0) {
        if (*a != *b) return (int)(u8)*a - (int)(u8)*b;
        if (*a == 0) return 0;          /* 两边同时到串尾 */
        a++; b++;
        n--;
    }
    return 0;
}
static int in_set(int c, const char *set) {
    const char *p = set;
    while (*p) { if (*p == c) return 1; p++; }
    return 0;
}
char *strpbrk(const char *s, const char *set) {
    for (; *s; s++) if (in_set(*s, set)) return (char*)s;
    return 0;
}
unsigned long strspn(const char *s, const char *set) {
    unsigned long n = 0;
    while (*s && in_set(*s, set)) { s++; n++; }
    return n;
}
unsigned long strcspn(const char *s, const char *set) {
    unsigned long n = 0;
    while (*s && !in_set(*s, set)) { s++; n++; }
    return n;
}
int strcoll(const char *a, const char *b) { return strcmp(a, b); }
const char *strstr(const char *hay, const char *needle) {
    unsigned long nl = strlen(needle);
    if (nl == 0) return hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*n && *h == *n) { h++; n++; }
        if (*n == 0) return hay;
    }
    return 0;
}

/* ====================================================================
 * ctype
 * ==================================================================== */
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c)  { return isdigit(c) || isalpha(c); }
int isspace(int c)  { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isxdigit(int c) { return isdigit(c) || (c>='a'&&c<='f') || (c>='A'&&c<='F'); }
int isprint(int c)  { return c >= 0x20 && c < 0x7F; }
int ispunct(int c)  { return isprint(c) && !isalnum(c) && c != ' '; }
int iscntrl(int c)  { return (c < 0x20 || c == 0x7F); }
int isgraph(int c)  { return isprint(c) && c != ' '; }
int isblank(int c)  { return c == ' ' || c == '\t'; }
int tolower(int c)  { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c)  { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* ====================================================================
 * 整数 / 浮点解析
 * ==================================================================== */
int abs(int x)      { return x < 0 ? -x : x; }
long labs(long x)   { return x < 0 ? -x : x; }
long strtol(const char *s, char **end, int base) {
    while (isspace(*s)) s++;
    int neg = 0;
    if (*s == '+') s++; else if (*s == '-') { neg = 1; s++; }
    if (base == 0) {
        if (s[0] == '0') base = (s[1] == 'x' || s[1] == 'X') ? 16 : 8;
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    long v = 0;
    for (;;) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char*)s;
    return neg ? -v : v;
}
unsigned long strtoul(const char *s, char **end, int base) {
    return (unsigned long)strtol(s, end, base);
}
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
unsigned long atoul(const char *s) { return strtoul(s, 0, 10); }

/* ====================================================================
 * os 库系统桩（裸机：无真实日历，统一用 2026-01-01 为纪元近似）
 * ==================================================================== */
static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int days_in_month(int m, int y) {
    static const int d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && is_leap(y)) return 29;
    return d[m];
}
/* 把 tm（year 距 1900）换算成 2026-01-01 纪元的秒数（近似，够 os.time 用） */
time_t_ g_shim_mktime(struct tm *t) {
    u32 days = 0;
    int y0 = 1900, y1 = 1900 + t->tm_year;
    for (int y = y0; y < y1; y++) days += is_leap(y) ? 366 : 365;
    for (int m = 0; m < t->tm_mon; m++) days += days_in_month(m, y1);
    days += (u32)(t->tm_mday - 1);
    u32 secs = days * 86400u + (u32)(t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec);
    return secs;
}
void g_shim_fill_tm(u32 secs, struct tm *t, int *wday_out) {
    t->tm_sec = secs % 60; secs /= 60;
    t->tm_min = secs % 60; secs /= 60;
    t->tm_hour = secs % 24; secs /= 24;
    u32 days = secs;
    int y = 1900;
    for (;;) {
        int dl = is_leap(y) ? 366 : 365;
        if (days < dl) break;
        days -= dl; y++;
    }
    t->tm_year = y - 1900;
    int m = 0;
    for (; m < 12; m++) {
        int dl = days_in_month(m, y);
        if (days < dl) break;
        days -= dl;
    }
    t->tm_mon = m;
    t->tm_mday = (int)days + 1;
    t->tm_isdst = 0;
    t->tm_yday = 0;
    int yd = 0;
    for (int mm = 0; mm < m; mm++) yd += days_in_month(mm, y);
    t->tm_yday = yd + (int)days;
    t->tm_wday = (int)((days + 5) % 7); /* 2026-01-01 是周四=4，近似 5 起步 */
    if (wday_out) *wday_out = t->tm_wday;
}
static struct tm g_tm_buf;
struct tm *localtime(const time_t_ *t) {
    if (!t) return 0;
    g_shim_fill_tm(*t, &g_tm_buf, 0);
    return &g_tm_buf;
}
struct tm *gmtime(const time_t_ *t) { return localtime(t); }
time_t_ mktime(struct tm *t) { return g_shim_mktime(t); }
double difftime(time_t_ t1, time_t_ t2) { return (double)(t1 - t2); }
clock_t clock(void) { return (clock_t)g_lua_ticks(); }

/* 其他 os 系统调用（裸机不可用，返回安全值） */
char *getenv(const char *name) { (void)name; return 0; }
int system(const char *cmd) { (void)cmd; return -1; }
int remove(const char *path) { (void)path; return -1; }
int rename(const char *o, const char *n) { (void)o; (void)n; return -1; }
char *tmpnam(char *s) { if (s) s[0] = 0; return 0; }
char *tempnam(const char *dir, const char *p) { (void)dir; (void)p; return 0; }

double strtod(const char *s, char **end) {
    while (isspace(*s)) s++;
    int neg = 0;
    if (*s == '+') s++; else if (*s == '-') { neg = 1; s++; }
    if (*s == 'i' || *s == 'I') {
        if ((s[1]=='n'&&s[2]=='a'&&s[3]=='n'&&s[4]==0) ||
            (s[1]=='N'&&s[2]=='A'&&s[3]=='N'&&s[4]==0))
            return 0.0 / 0.0;
        if ((s[1]=='n'&&s[2]=='f'&&s[3]==0) || (s[1]=='N'&&s[2]=='F'&&s[3]==0))
            return neg ? -((double)1e308 * 1e308) : ((double)1e308 * 1e308);
    }
    double v = 0;
    int any = 0;
    while (isdigit(*s)) { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (*s == '.') {
        s++;
        double f = 0.1;
        while (isdigit(*s)) { v += (*s - '0') * f; f *= 0.1; s++; any = 1; }
    }
    if (any && (*s == 'e' || *s == 'E')) {
        s++;
        int eneg = 0;
        if (*s == '+') s++; else if (*s == '-') { eneg = 1; s++; }
        int e = 0;
        while (isdigit(*s)) { e = e * 10 + (*s - '0'); s++; }
        for (int i = 0; i < e; i++) { if (eneg) v /= 10.0; else v *= 10.0; }
    }
    if (end) *end = (char*)s;
    return neg ? -v : v;
}
float strtof(const char *s, char **end) { return (float)strtod(s, end); }

/* ====================================================================
 * 浮点基础 + 高级数学（Lua math 库需要）
 * 全部 x87 / 纯 C 实现，不依赖 libc。
 * ==================================================================== */
static const double LN2 = 0.6931471805599453;
static const double LOG2E = 1.4426950408889634;
static const double PI  = 3.14159265358979323846;
static const double TAU = 6.28318530717958647692;

double fabs(double x)  { return x < 0 ? -x : x; }
/* 取整：不用 frndint（与编译器 x87 栈管理冲突会炸栈）。
   直接用 C 语言 double→long→double 转换，编译器生成正确的 x87
   指令（fstptoi/fisttp 或 fptan），已验证安全（1.0+2.0→3.0 正常）。 */
double trunc(double x) {
    if (x >= 0) return (double)(long long)x;
    else        return (double)((long long)(-x)) * -1.0;
}
double floor(double x) {
    double t = trunc(x);
    if (t > x) t -= 1.0;
    return t;
}
double ceil(double x) {
    double t = trunc(x);
    if (t < x) t += 1.0;
    return t;
}
double round(double x) { return floor(x + 0.5); }
double fmod(double a, double b) { double q; q = floor(a / b); return a - q * b; }
double modf(double x, double *ip) { double f = trunc(x); *ip = f; return x - f; }

/* sqrt：x87 硬件指令（fsqrt 对 st(0)），精确 */
double sqrt(double x) {
    if (x < 0) return 0.0 / 0.0;
    double r = x;
    __asm__ __volatile__("fldl %0\n\tfsqrt\n\tfstpl %0" : "+m"(r) : : "st");
    return r;
}

/* frexp：x = m * 2^e，m ∈ [0.5, 1) */
double frexp(double x, int *exp) {
    if (x == 0.0 || x != x || x == (double)1e308*1e308 || x == -(double)1e308*1e308) {
        *exp = 0; return x;
    }
    double ax = fabs(x);
    int e = 0;
    while (ax >= 1.0)  { ax *= 0.5; e++; }
    while (ax < 0.5)   { ax *= 2.0; e--; }
    *exp = e;
    return (x < 0) ? -ax : ax;
}
double ldexp(double x, int exp) {
    if (exp >= 0) { for (int i = 0; i < exp; i++) x *= 2.0; }
    else          { for (int i = 0; i > exp; i--) x *= 0.5; }
    return x;
}
int ilogb(double x) { int e; frexp(x, &e); return e - 1; }

/* log：分解 x = m*2^e (m∈[1,2))，log(x) = e*ln2 + log(m)
   log(m) 用 t = (m-1)/(m+1)，log(m) = 2*(t + t^3/3 + t^5/5 + ...) */
static double log_range(double m) {
    /* m ∈ [1, 2) */
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double s = t, term = t;
    for (int i = 1; i < 30; i++) {
        term *= t2;
        double add = term / (2.0 * i + 1.0);
        s += add;
        if (add < 1e-16) break;
    }
    return 2.0 * s;
}
double log(double x) {
    if (x <= 0) return (x < 0) ? 0.0/0.0 : -(1e308*1e308);
    int e;
    double m = frexp(x, &e);
    m *= 2.0;  /* 现在 m ∈ [1, 2) */
    e--;
    return e * LN2 + log_range(m);
}
double log2(double x)  { return log(x) * LOG2E; }
double log10(double x) { return log(x) / 2.3025850929940457; }

/* exp：x = k*ln2 + r (r ∈ [-ln2/2, ln2/2))，exp(x) = 2^k * exp(r)
   exp(r) 用 Taylor 级数，|r| <= 0.35，收敛极快 */
static double exp_small(double r) {
    double term = 1.0, sum = 1.0, x = r;
    for (int i = 1; i <= 30; i++) {
        x *= r;
        term *= x / (double)i;
        sum += term;
        if (fabs(term) < 1e-16) break;
    }
    return sum;
}
double exp(double x) {
    if (x > 709.78) return 1e308*1e308;
    if (x < -745.13) return 0.0;
    double k = floor(x / LN2 + 0.5);
    double r = x - k * LN2;
    return exp_small(r) * ldexp(1.0, (int)k);
}
double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return (y > 0) ? 0.0 : (1e308*1e308);
    if (x < 0) {
        /* 负底数：只有整数指数有意义 */
        double i;
        if (modf(y, &i) != 0.0) return 0.0/0.0;
        int n = (int)i;
        double r = 1.0;
        double ax = -x;
        for (int k2 = 0; k2 < (n < 0 ? -n : n); k2++) r *= ax;
        if (n < 0) r = 1.0 / r;
        return ((n & 1) && x < 0) ? -r : r;
    }
    return exp(y * log(x));
}
double cbrt(double x) {
    if (x == 0) return 0.0;
    int neg = x < 0;
    double ax = neg ? -x : x;
    double g = ax;
    for (int i = 0; i < 64; i++) {
        double g2 = g * g;
        double d = g2 * g - ax;
        double dg = 3.0 * g2;
        if (dg == 0) break;
        double ng = g - d / dg;
        if (ng <= 0 || fabs(ng - g) < 1e-15 * g) { g = ng; break; }
        g = ng;
    }
    return neg ? -g : g;
}
double hypot(double x, double y) {
    double ax = fabs(x), ay = fabs(y);
    if (ax > ay) return ax * sqrt(1.0 + (ay/ax)*(ay/ax));
    return (ay == 0) ? ax : ay * sqrt(1.0 + (ax/ay)*(ax/ay));
}

/* sin/cos：把 x 缩减到 n*PI + r（|r| <= PI/2），
   sin(n*PI+r) 与 cos(n*PI+r) 由 n mod 4 定符号/互换，
   sin(r)/cos(r) 用 Taylor 级数（|r|<=PI/2 收敛快）。 */
static void sincos_range(double x, double *sp, double *cp) {
    double x2 = x * x;
    double s = x, c = 1.0, term_s = x, term_c = 1.0;
    for (int i = 1; i < 20; i++) {
        term_s *= -x2 / ((2.0*i) * (2.0*i + 1.0));
        s += term_s;
        term_c *= -x2 / ((2.0*i - 1.0) * (2.0*i));
        c += term_c;
        if (fabs(term_s) < 1e-16 && fabs(term_c) < 1e-16) break;
    }
    if (sp) *sp = s;
    if (cp) *cp = c;
}
double sin(double x) {
    double n = floor(x / PI + 0.5);
    double r = x - n * PI;
    double s, c;
    sincos_range(r, &s, &c);
    int qn = (int)n & 3;
    switch (qn) {
    case 0: return s;
    case 1: return c;
    case 2: return -s;
    default: return -c;
    }
}
double cos(double x) {
    double n = floor(x / PI + 0.5);
    double r = x - n * PI;
    double s, c;
    sincos_range(r, &s, &c);
    int qn = (int)n & 3;
    switch (qn) {
    case 0: return c;
    case 1: return -s;
    case 2: return -c;
    default: return s;
    }
}
double tan(double x) { return sin(x) / cos(x); }

/* atan：|x| <= 1，级数 atan(x) = x - x^3/3 + x^5/5 - ... */
static double atan_unit(double x) {
    double x2 = x * x;
    double term = x, sum = x;
    for (int i = 1; i < 60; i++) {
        term *= -x2;
        double add = term / (2.0*i + 1.0);
        sum += add;
        if (fabs(add) < 1e-16) break;
    }
    return sum;
}
double atan(double x) {
    if (x > 1.0) return PI / 2.0 - atan_unit(1.0 / x);
    if (x < -1.0) return -PI / 2.0 - atan_unit(1.0 / x);
    return atan_unit(x);
}
double asin(double x) {
    if (x < -1) x = -1; else if (x > 1) x = 1;
    return atan(x / sqrt(1.0 - x * x));
}
double acos(double x) {
    if (x < -1) x = -1; else if (x > 1) x = 1;
    return PI / 2.0 - asin(x);
}
double atan2(double y, double x) {
    if (x == 0 && y == 0) return 0.0;
    if (x > 0) return atan(y / x);
    if (x < 0 && y == 0) return PI;
    if (x < 0) {
        double r = atan(y / x);
        return (y < 0) ? r - PI : r + PI;
    }
    /* x == 0 */
    return (y > 0) ? PI / 2.0 : -PI / 2.0;
}

/* rand / srand */
static u32 g_rand_state = 0x12345678u;
void srand(unsigned int seed) { g_rand_state = seed ? seed : 1; }
int rand(void) {
    g_rand_state = g_rand_state * 1103515245u + 12345u;
    return (int)((g_rand_state >> 16) & 0x7FFF);
}

/* ====================================================================
 * 浮点 -> 十进制串（%f %e %g 共用）
 * 算法：用 10^7 调整幅度到 [1, 1e7)，整数部分取 int，小数部分逐位乘 10。
 * 15 位有效数字内误差可忽略；超出 int 范围的值只用于 %e（Lua tostring
 * 走 %.14g，大数会自动落到科学计数法）。
 * ==================================================================== */
#define DEC_SIG 16
typedef struct { char d[DEC_SIG + 8]; int n; int exp10; int neg; } dec_t;

static void dec_norm(double v, dec_t *o) {
    o->n = 0; o->neg = 0; o->exp10 = 0;
    if (v != v) { o->d[0]='n'; o->d[1]='a'; o->d[2]='n'; o->n = 3; return; }
    if (v < 0) { o->neg = 1; v = -v; }
    if (v == 0) { o->d[0] = '0'; o->n = 1; return; }
    const double M7 = 1e7;
    int e = 0;
    while (v >= M7) { v /= M7; e += 7; }
    while (v < 1.0)  { v *= M7; e -= 7; if (v == 0) break; }
    u32 ip = (u32)v;
    double fr = v - (double)ip;
    /* 整数部分（<= 1e7，7 位） */
    char ib[16]; int in = 0;
    if (ip == 0) ib[in++] = '0';
    while (ip) { ib[in++] = (char)('0' + ip % 10); ip /= 10; }
    int k = in - 1;
    while (k >= 0 && o->n < DEC_SIG) o->d[o->n++] = ib[k--];
    /* 整数部分位数就是 o->n（此刻小数部分还没填）。不能用 strlen(o->d)：
       o.d 只有 o.n 字节有效，strlen 会读到栈上脏字节 -> exp10 随机错
       （例如 2.5 被当成 250）。 */
    o->exp10 = e + o->n - 1;
    /* 小数部分 */
    for (int i = 0; o->n < DEC_SIG && fr > 0; i++) {
        fr *= 10.0;
        u32 d = (u32)fr;
        if (d > 9) d = 9;
        fr -= (double)d;
        o->d[o->n++] = (char)('0' + d);
    }
}

/* ====================================================================
 * time
 * ==================================================================== */
time_t_ time(time_t_ *t) {
    time_t_ v = (time_t_)(g_lua_ticks() / 1000);
    if (t) *t = v;
    return v;
}
static const char g_month[13][4] = {"Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec",""};
static const char g_day[7][3]    = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static char g_ctime_buf[64];
const char *ctime(const time_t_ *t) {
    time_t_ d = *t;
    int sec = (int)(d % 60); d /= 60;
    int min = (int)(d % 60); d /= 60;
    int hr  = (int)(d % 24); d /= 24;
    int wday = (int)((d + 4) % 7);
    snprintf(g_ctime_buf, sizeof g_ctime_buf, "%.3s %.3s %2d %02d:%02d:%02d 2026\n",
             g_day[wday], g_month[1], 1 + (int)(d % 31), hr, min, sec);
    return g_ctime_buf;
}
/* strftime：实现 os.date 常用的格式符（Lua 自带 checkoption 已校验过） */
unsigned long strftime(char *dst, unsigned long maxsz, const char *fmt, const struct tm *t) {
    unsigned long p = 0;
    #define P(c) do { if (p + 1 < maxsz) dst[p++] = (c); } while (0)
    #define PN(s) do { const char *_s=(s); int _i=0; while (_s[_i]) { P(_s[_i++]); } } while (0)
    const char *f = fmt;
    while (*f) {
        if (*f != '%') { P(*f++); continue; }
        f++;
        char c = *f++;
        switch (c) {
        case '%': P('%'); break;
        case 'Y': { char b[8]; snprintf(b,8,"%04d",1900+t->tm_year); PN(b); } break;
        case 'y': { char b[8]; snprintf(b,8,"%02d",(1900+t->tm_year)%100); PN(b); } break;
        case 'm': { char b[8]; snprintf(b,8,"%02d",t->tm_mon+1); PN(b); } break;
        case 'd': { char b[8]; snprintf(b,8,"%02d",t->tm_mday); PN(b); } break;
        case 'H': { char b[8]; snprintf(b,8,"%02d",t->tm_hour); PN(b); } break;
        case 'M': { char b[8]; snprintf(b,8,"%02d",t->tm_min); PN(b); } break;
        case 'S': { char b[8]; snprintf(b,8,"%02d",t->tm_sec); PN(b); } break;
        case 'I': { char b[8]; int h=t->tm_hour%12; if(h==0)h=12; snprintf(b,8,"%02d",h); PN(b); } break;
        case 'p': PN(t->tm_hour < 12 ? "AM" : "PM"); break;
        case 'a': PN(g_day[t->tm_wday]); break;
        case 'A': PN(g_day[t->tm_wday]); break;
        case 'b': PN(g_month[t->tm_mon]); break;
        case 'B': PN(g_month[t->tm_mon]); break;
        case 'j': { char b[8]; snprintf(b,8,"%03d",t->tm_yday+1); PN(b); } break;
        case 'w': P((char)('0'+t->tm_wday)); break;
        case 'U': case 'W': { char b[8]; snprintf(b,8,"%02d",(t->tm_yday+1)/7); PN(b); } break;
        case 'Z': PN("UTC"); break;
        case 'z': P('0'); P('0'); break;
        case 's': { char b[24]; snprintf(b,24,"%lu",(unsigned long)g_shim_mktime(t)); PN(b); } break;
        case 'c': case 'x': case 'X': {
            char b[32];
            snprintf(b,sizeof b,"%02d:%02d:%02d",t->tm_hour,t->tm_min,t->tm_sec);
            if (c=='x') { snprintf(b,sizeof b,"%02d/%02d/%02d",(t->tm_mon+1),t->tm_mday,(1900+t->tm_year)%100); }
            if (c=='c') {
                char b2[48];
                snprintf(b2,sizeof b2,"%02d/%02d/%04d %02d:%02d:%02d",
                    t->tm_mon+1,t->tm_mday,1900+t->tm_year,t->tm_hour,t->tm_min,t->tm_sec);
                PN(b2);
                continue;
            }
            PN(b);
        } break;
        default: P('%'); P(c); break;
        }
    }
    if (maxsz > p) dst[p] = 0; else if (maxsz > 0) dst[maxsz-1] = 0;
    #undef P
    #undef PN
    return p;
}

/* ====================================================================
 * printf 族
 * 支持：%s %c %d %i %u %ld %lu %x %X %p %f %.Nf %e %.N%g
 * 32 位 cdecl：int/指针 1 栈槽；float/double 2 栈槽（lo 在前）。
 * ==================================================================== */
typedef struct { char *dst; u32 nmax; int pos; } fmt_ctx;

static int f_put(fmt_ctx *c, char ch) {
    if ((u32)c->pos < c->nmax) c->dst[c->pos] = ch;
    c->pos++;
    return 0;
}
static void f_ufmt(fmt_ctx *c, u32 v, int base, int upper, int minw, int neg, int pad0) {
    char tmp[16];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) {
        int d = (int)(v % (u32)base);
        tmp[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= (u32)base;
    }
    int total = n + (neg ? 1 : 0);
    int pad = minw - total;
    if (pad < 0) pad = 0;
    char fill = pad0 ? '0' : ' ';
    for (int i = 0; i < pad; i++) f_put(c, fill);
    if (neg) f_put(c, '-');
    for (int i = n - 1; i >= 0; i--) f_put(c, tmp[i]);
}
static void f_sfmt(fmt_ctx *c, const char *s, int minw, int prec) {
    if (!s) s = "(nil)";
    int len = 0;
    while (s[len] && (prec < 0 || len < prec)) len++;
    int pad = minw - len;
    if (pad < 0) pad = 0;
    for (int i = 0; i < pad; i++) f_put(c, ' ');
    for (int i = 0; i < len; i++) f_put(c, s[i]);
}
static void f_ffmt(fmt_ctx *c, double v, int minw, int prec, char conv) {
    char out[96];
    int total = 0;
    int neg = 0;
    if (v != v) {
        out[total++] = 'n'; out[total++] = 'a'; out[total++] = 'n';
    } else {
        dec_t d;
        dec_norm(v, &d);
        neg = d.neg;
        if (conv == 'e') {
            if (prec < 0) prec = 6;
            int mant = prec + 1;
            if (mant > d.n) mant = d.n;
            out[total++] = d.d[0];
            for (int i = 1; i < mant; i++) out[total++] = d.d[i];
            /* 去尾零（保留首数字） */
            while (total > 1 && out[total - 1] == '0' && !(total == 1)) total--;
            out[total++] = 'e';
            int ee = d.exp10;
            out[total++] = (ee < 0) ? '-' : '+';
            int ea = (ee < 0) ? -ee : ee;
            char eb[8]; int en = 0;
            if (ea == 0) eb[en++] = '0';
            while (ea) { eb[en++] = (char)('0' + ea % 10); ea /= 10; }
            for (int i = en - 1; i >= 0; i--) out[total++] = eb[i];
        } else {
            /* %f / %g 定点。%g 的精度语义：有效位数（默认 6）；
               exp10 < -4 或 >= 有效位数时切回 %e（C 标准 %g 行为）。 */
            if (prec < 0) prec = 6;
            int use_e = (conv == 'g') && (d.exp10 < -4 || d.exp10 >= prec);
            if (use_e) {
                int mant = prec;
                if (mant > d.n) mant = d.n;
                out[total++] = d.d[0];
                for (int i = 1; i < mant; i++) out[total++] = d.d[i];
                while (total > 1 && out[total - 1] == '0') total--;
                out[total++] = 'e';
                int ee = d.exp10;
                out[total++] = (ee < 0) ? '-' : '+';
                int ea = (ee < 0) ? -ee : ee;
                char eb[8]; int en = 0;
                if (ea == 0) eb[en++] = '0';
                while (ea) { eb[en++] = (char)('0' + ea % 10); ea /= 10; }
                for (int i = en - 1; i >= 0; i--) out[total++] = eb[i];
            } else {
                int dp = d.exp10 + 1;          /* 小数点前数字个数 */
                if (dp < 0) dp = 0;
                if (dp == 0) out[total++] = '0';
                for (int i = 0; i < dp; i++) out[total++] = (i < d.n) ? d.d[i] : '0';
                out[total++] = '.';
                int frac = (conv == 'g') ? 0 : prec;
                /* %g：小数部分输出去掉尾零后的剩余位 */
                if (conv == 'g') {
                    for (int i = 0; dp + i < d.n; i++) {
                        char ch = d.d[dp + i];
                        out[total++] = ch;
                    }
                    while (total > 0 && out[total - 1] == '0') total--;
                    if (total > 0 && out[total - 1] == '.') total--;
                    if (dp == 0 && total > 0 && out[total - 1] == '0' && total == 1) {
                        /* 0. -> 0 */
                        /* 保持 "0"，去掉后面空小数（上面已处理） */
                    }
                } else {
                    for (int i = 0; i < frac; i++)
                        out[total++] = (dp + i < d.n) ? d.d[dp + i] : '0';
                }
            }
        }
    }
    int pad = minw - total;
    if (pad > 0) for (int i = 0; i < pad; i++) f_put(c, ' ');
    if (neg) f_put(c, '-');
    for (int i = 0; i < total; i++) f_put(c, out[i]);
}

static int vsnprintf_core(char *dst, u32 nmax, const char *fmt, va_list ap) {
    fmt_ctx c = { dst, nmax, 0 };
    const char *p = fmt;
    while (*p) {
        if (*p != '%') { f_put(&c, *p++); continue; }
        p++;
        int flags_zero = 0, minw = 0, prec = -1, islong = 0;
        while (*p == '-' || *p == '0') { if (*p == '0') flags_zero = 1; p++; }
        if (*p == '*') { minw = va_arg(ap, int); p++; }
        else while (isdigit(*p)) { minw = minw * 10 + (*p - '0'); p++; }
        if (*p == '.') {
            p++;
            if (*p == '*') { prec = va_arg(ap, int); p++; }
            else { prec = 0; while (isdigit(*p)) { prec = prec * 10 + (*p - '0'); p++; } }
        }
        while (*p == 'l' || *p == 'h') { if (*p == 'l') islong = 1; p++; }
        char conv = *p++;
        switch (conv) {
        case '%': f_put(&c, '%'); break;
        case 'c': f_put(&c, (char)va_arg(ap, int)); break;
        case 's': f_sfmt(&c, va_arg(ap, const char*), minw, prec); break;
        case 'd': case 'i': {
            long v;
            if (islong) v = va_arg(ap, long);
            else v = (long)va_arg(ap, int);
            u32 uv = (u32)(v < 0 ? ((long)0 - v) : v);
            f_ufmt(&c, uv, 10, 0, minw, v < 0, flags_zero);
            break;
        }
        case 'u': {
            u32 v;
            if (islong) v = (u32)va_arg(ap, long);
            else v = (u32)va_arg(ap, int);
            f_ufmt(&c, v, 10, 0, minw, 0, flags_zero);
            break;
        }
        case 'x': case 'X': {
            u32 v;
            if (islong) v = (u32)va_arg(ap, long);
            else v = (u32)va_arg(ap, int);
            f_ufmt(&c, v, 16, conv == 'X', minw, 0, flags_zero);
            break;
        }
        case 'p': {
            u32 v = (u32)va_arg(ap, void*);
            f_put(&c, '0'); f_put(&c, 'x');
            f_ufmt(&c, v, 16, 0, 0, 0, 0);
            break;
        }
        case 'f': case 'e': case 'g': {
            double dv;
            /* cdecl 32 位变参里 double 按 8 字节连续存放；32 位分读重组 */
            u32 lo = va_arg(ap, u32);
            u32 hi = va_arg(ap, u32);
            *(u32*)&dv = lo;
            *((u32*)&dv + 1) = hi;
            f_ffmt(&c, dv, minw, prec, conv);
            break;
        }
        default: f_put(&c, '%'); f_put(&c, conv); break;
        }
    }
    if ((u32)c.pos < nmax) dst[c.pos] = 0;
    return c.pos;
}

/* 变参起点 = 最后一个命名参数之后。i386 cdecl：esp->返回地址，esp+4 起是参数。
   所以变参偏移 = 4(返回地址) + N*sizeof(命名参数)。 */
int snprintf(char *dst, u32 nmax, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf_core(dst, nmax, fmt, ap);
    va_end(ap);
    return r;
}
int sprintf(char *dst, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf_core(dst, 4096, fmt, ap);
    va_end(ap);
    return r;
}
int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf_core(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (g_lua_api) g_lua_api->print(tmp);
    return n;
}
/* stderr + fprintf/vfprintf：lauxlib 的 luai_writestringerror 宏默认走 fprintf(stderr) */
static FILE g_stderr_file = { 0, 0, 0, 0 };
static FILE g_stdout_file = { 0, 0, 0, 0 };
FILE *stderr = &g_stderr_file;
FILE *stdout = &g_stdout_file;
FILE *stdin  = &g_stdout_file;
int fprintf(FILE *f, const char *fmt, ...) {
    (void)f;
    va_list ap; va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf_core(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (g_lua_api) g_lua_api->print(tmp);
    return n;
}
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    (void)f;
    char tmp[1024];
    int n = vsnprintf_core(tmp, sizeof tmp, fmt, ap);
    if (g_lua_api) g_lua_api->print(tmp);
    return n;
}
/* fwrite / fflush：lua_writestring / lua_writeline 展开到 stdout。
   裸机下 stdout 无真实后端，直接转发到 api->print。 */
long fwrite(const void *src, unsigned int sz, unsigned int n, FILE *f) {
    (void)f;
    const char *s = (const char*)src;
    unsigned int total = sz * n;
    if (total == 0 || !s) return 0;
    if (g_lua_api) g_lua_api->print(s);
    return total;
}
/* fflush / fopen / fgetc 等由 lua_main.c 提供（VFS 文件桥） */
int getc(FILE *f) { return fgetc(f); }
int putc(int c, FILE *f) { (void)f; return c; }
FILE *freopen(const char *path, const char *mode, FILE *f) { (void)path; (void)mode; return f; }
int setvbuf(FILE *f, char *buf, int mode, unsigned int sz) { (void)f;(void)buf;(void)mode;(void)sz; return 0; }
FILE *tmpfile(void) { return 0; }
int fileno(FILE *f) { (void)f; return -1; }

/* ====================================================================
 * setjmp / longjmp（Lua 的 LUAI_TRY / LUAI_THROW）
 * Lua 只在 C 函数边界（ST 栈空）调用 setjmp，故保存 GPR + FOP/FCTRL
 * 即安全；恢复后从保存的 EIP 远跳。
 * ==================================================================== */
/* 完整 x87 状态（fsave/fldsave，64 字节，16 字节对齐）+ GPR + 返回地址。
   依赖 -fno-omit-frame-pointer：setjmp 里 [ebp+4] 即调用者返回地址。
   longjmp 恢复后令 esp=ebp+8（调用者 call 之前的 esp）、eax=v，再远跳到返回
   地址 —— 即"setjmp 返回 v"。 */
/* 手写 setjmp/longjmp：保存/恢复 GPR + 返回地址 + 完整 x87 状态（fsave/frstor，
   96 字节）。依赖 -fno-omit-frame-pointer 读 [ebp+4]。
   longjmp 恢复后令 esp=ebp+8、eax=v，远跳返回地址 —— 即"setjmp 返回 v"。 */
#include <setjmp.h>
/* GCC 内建会生成保存/恢复全部寄存器（含 x87）的指令。
   Lua 的 LUAI_THROW 宏恒为 `longjmp(b, 1)`，故内建 longjmp 的
   常量参数传 1 即可（内建要求第二参数为编译期常量）。 */
/* glibc 风格：参数按值（jmp_buf 为数组类型，退化为指针）。
   GCC 内建按自身布局读写，需 void**；jmp_buf(160B) 足够容纳全部寄存器+x87。 */
int setjmp(jmp_buf b) { return __builtin_setjmp((void**)b); }
void longjmp(jmp_buf b, int v) {
    (void)v;
    __builtin_longjmp((void**)b, 1);
}
void abort(void) {
    if (g_lua_api) g_lua_api->print("lua abort");
    for (;;) { __asm__ __volatile__("hlt"); }
}

/* ====================================================================
 * 其他占位（FILE 族全部在 lua_main.c 里走 VFS）
 * ==================================================================== */
int errno = 0;
char *strerror(int err) {
    switch (err) {
    case 0:  return "no error";
    case 2:  return "no such file or directory";
    case 12: return "out of memory";
    case 13: return "permission denied";
    default: return "unknown error";
    }
}
void exit(int code) { (void)code; for (;;); }

/* locale */
static const char *g_locale = "C";
const char *setlocale(int cat, const char *loc) { (void)cat; if (loc) g_locale = loc; return g_locale; }
struct lconv { char *decimal_point; char *thousands_sep; };
static struct lconv g_lconv = { ".", "" };
struct lconv *localeconv(void) { return &g_lconv; }
