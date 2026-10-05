#include "libc.h"

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
size_t strnlen(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}
static char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
int strcasecmp(const char *a, const char *b) {
    while (*a && lower_ascii(*a) == lower_ascii(*b)) { a++; b++; }
    return (unsigned char)lower_ascii(*a) - (unsigned char)lower_ascii(*b);
}
char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    while (i < n) dst[i++] = 0;
    return dst;
}
char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}
char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i = 0;
    while (i < n && src[i]) { d[i] = src[i]; i++; }
    d[i] = 0;
    return dst;
}
char *strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char*)s; s++; }
    return (c == 0) ? (char*)s : NULL;
}
char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    if (c == 0) return (char*)s;
    return (char*)last;
}
void *memchr(const void *p, int c, size_t n) {
    const unsigned char *x = (const unsigned char*)p;
    while (n--) { if (*x == (unsigned char)c) return (void*)x; x++; }
    return NULL;
}
void memset(void *p, int v, size_t n) {
    unsigned char *d = (unsigned char*)p;
    while (n--) *d++ = (unsigned char)v;
}
void memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char*)dst;
    const unsigned char *s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}
void memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char*)dst;
    const unsigned char *s = (const unsigned char*)src;
    if (d == s || n == 0) return;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char*)a, *y = (const unsigned char*)b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}
int atoi(const char *s) {
    int sign = 1; long v = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return (int)(sign * v);
}
u32 strtoul(const char *s, const char **endp, int base) {
    u32 v = 0; int any = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;;) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (u32)base + (u32)d;
        s++; any = 1;
    }
    if (endp) *endp = any ? s : NULL;
    return v;
}
char *itoa(int v, char *buf, int base) {
    static const char digits[] = "0123456789ABCDEF";
    char tmp[34]; int i = 0, neg = 0;
    unsigned int u = (unsigned int)v;
    if (v < 0 && base == 10) { neg = 1; u = (unsigned int)(-(long)v); }
    if (u == 0) tmp[i++] = '0';
    while (u) { tmp[i++] = digits[u % (unsigned)base]; u /= (unsigned)base; }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}
char *utoa_hex(u32 v, char *buf, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[16]; int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = digits[v & 0xF]; v >>= 4; }
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

/* ---------------- 格式化输出 ---------------- */
typedef struct { char *buf; size_t size; size_t n; } out_t;

static void out_ch(out_t *o, char c) {
    if (o->n + 1 < o->size) o->buf[o->n] = c;
    o->n++;
}

static void emit_padded(out_t *o, const char *s, int len, int width, int left, char pad) {
    int padn = (width > len) ? width - len : 0;
    int i;
    if (!left) for (i = 0; i < padn; i++) out_ch(o, pad);
    for (i = 0; i < len; i++) out_ch(o, s[i]);
    if (left) for (i = 0; i < padn; i++) out_ch(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap) {
    out_t o;
    o.buf = buf; o.size = size; o.n = 0;

    char tmp[64];

    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { out_ch(&o, *f); continue; }
        f++;
        if (*f == '%') { out_ch(&o, '%'); continue; }

        int left = 0, zero = 0, width = 0, is_long = 0;
        /* 标志 */
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+' || *f == ' ' || *f == '#') { /* 忽略 */ }
            else break;
        }
        /* 宽度 */
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        /* 长度修饰 */
        if (*f == 'l') { is_long = 1; f++; }
        if (*f == 'l') { is_long = 1; f++; }

        char pad = (zero && !left) ? '0' : ' ';
        int len = 0;

        switch (*f) {
        case 'd': case 'i': {
            int v = __builtin_va_arg(ap, int);
            itoa(v, tmp, 10);
            len = (int)strlen(tmp);
            if (zero && !left && tmp[0] == '-') {
                /* 零填充时符号在前 */
                out_ch(&o, '-');
                emit_padded(&o, tmp + 1, len - 1, width > 0 ? width - 1 : 0, 0, '0');
                continue;
            }
            emit_padded(&o, tmp, len, width, left, pad);
            continue;
        }
        case 'u': {
            unsigned int v = is_long ? (unsigned int)__builtin_va_arg(ap, unsigned long)
                                     : __builtin_va_arg(ap, unsigned int);
            char rev[12]; int i = 0;
            if (v == 0) rev[i++] = '0';
            while (v) { rev[i++] = (char)('0' + (v % 10u)); v /= 10u; }
            len = i;
            for (int j = 0; j < i; j++) tmp[j] = rev[i - 1 - j];
            tmp[i] = 0;
            emit_padded(&o, tmp, len, width, left, pad);
            continue;
        }
        case 'x': case 'X': {
            unsigned int v = is_long ? (unsigned int)__builtin_va_arg(ap, unsigned long)
                                     : __builtin_va_arg(ap, unsigned int);
            utoa_hex(v, tmp, *f == 'X'); len = (int)strlen(tmp);
            emit_padded(&o, tmp, len, width, left, pad);
            continue;
        }
        case 'p': {
            unsigned int v = (unsigned int)(size_t)__builtin_va_arg(ap, void*);
            tmp[0] = '0'; tmp[1] = 'x';
            utoa_hex(v, tmp + 2, 0); len = (int)strlen(tmp);
            emit_padded(&o, tmp, len, width, left, pad);
            continue;
        }
        case 's': {
            const char *s = __builtin_va_arg(ap, const char*);
            if (!s) s = "(null)";
            len = (int)strlen(s);
            emit_padded(&o, s, len, width, left, ' ');
            continue;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(ap, int);
            emit_padded(&o, &c, 1, width, left, ' ');
            continue;
        }
        default:
            out_ch(&o, '%');
            if (*f) out_ch(&o, *f);
            continue;
        }
    }
    if (o.size) {
        size_t end = o.n < o.size ? o.n : o.size - 1;
        o.buf[end] = 0;
    }
    return (int)o.n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return r;
}
