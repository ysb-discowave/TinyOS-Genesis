// ============================================================
// TinyOS sample program 8: hello.cpp
// Language: C++ (freestanding, no exceptions / no RTTI / no stdlib)
// Build: tcc hello.cpp -o cpp_demo.TNCR --lang cpp
// ============================================================
#include "api_user.h"

// minimal integer-to-string without a standard library
static char *u_itoa(int v, char *b) {
    char t[16]; int i = 0, neg = 0;
    unsigned u = (unsigned)v;
    if (v < 0) { neg = 1; u = (unsigned)(-v); }
    if (!u) t[i++] = '0';
    while (u) { t[i++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) t[i++] = '-';
    int j = 0;
    while (i) b[j++] = t[--i];
    b[j] = 0;
    return b;
}

// templates work in user space too: code generated at compile time
template <typename T>
static T square(T x) { return x * x; }

void user_main(tinyos_api_t *api) {
    char b[16];
    api->println("--- C++ (freestanding) demo ---");
    api->println("A template function expanded at compile time:");
    api->print("square(12) = ");
    api->println(u_itoa(square(12), b));
    api->print("square(7)  = ");
    api->println(u_itoa(square(7), b));
    api->print("constant-folded square(100) = ");
    api->println(u_itoa(square(100), b));
    api->println("C++ user program is running fine.");
}
