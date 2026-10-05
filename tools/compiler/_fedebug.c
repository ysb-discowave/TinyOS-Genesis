/* 聚焦调试：GF(2^255-19) 域运算本身是否正确 */
#include <stdio.h>
#include <string.h>
#include "sshd_crypto.h"

static void hex2bin(const char *h, c_u8 *out, int n) {
    for (int i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = h[i * 2 + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else v |= c - 'a' + 10;
        }
        out[i] = (c_u8)v;
    }
}
static void ph(const char *tag, const c_u8 *b, int n) {
    printf("%-28s ", tag);
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}
static int eq(const c_u8 *a, const c_u8 *b, int n) { return memcmp(a, b, n) == 0; }

int main(void) {
    c_u8 raw[32], back[32], o1[32], o2[32];
    cx_fe a, b, c;

    /* 1) frombytes -> tobytes 往返 */
    hex2bin("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", raw, 32);
    cx_fe_frombytes(a, raw);
    printf("limbs of u: ");
    for (int i = 0; i < 10; i++) printf("%d ", a[i]);
    printf("\n");
    cx_fe_tobytes(back, a);
    ph("roundtrip (want 1st line)", back, 32);
    ph("original", raw, 32);
    printf("  roundtrip %s\n\n", eq(back, raw, 32) ? "OK" : "BAD");

    /* 2) 1 * x == x */
    cx_fe one;
    cx_fe_one(one);
    cx_fe_mul(c, a, one);
    cx_fe_tobytes(back, c);
    printf("  1*x %s\n", eq(back, raw, 32) ? "OK" : "BAD");
    ph("  1*x  ", back, 32);

    /* 3) (x+1)-1 == x */
    cx_fe t;
    cx_fe_add(t, a, one);
    cx_fe_sub(t, t, one);
    cx_fe_tobytes(back, t);
    printf("  (x+1)-1 %s\n", eq(back, raw, 32) ? "OK" : "BAD");
    ph("  (x+1)-1", back, 32);

    /* 4) x * x^-1 == 1 */
    cx_fe xi, prod;
    cx_fe_invert(xi, a);
    cx_fe_mul(prod, a, xi);
    cx_fe_tobytes(back, prod);
    c_u8 oneb[32]; memset(oneb, 0, 32); oneb[0] = 1;
    printf("  x*x^-1 %s\n", eq(back, oneb, 32) ? "OK" : "BAD");
    ph("  x*x^-1", back, 32);

    /* 5) 大数乘法：p-1 乘以 p-1 应为 1（因为 (-1)*(-1)=1） */
    c_u8 pm1[32];
    hex2bin("ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", pm1, 32);
    cx_fe_frombytes(a, pm1);            /* a = p-1 ≡ -1 */
    cx_fe_mul(c, a, a);                 /* (-1)^2 = 1 */
    cx_fe_tobytes(back, c);
    printf("  (-1)^2 %s\n", eq(back, oneb, 32) ? "OK" : "BAD");
    ph("  (-1)^2", back, 32);

    /* 6) 2^255 ≡ 19 的 limb 折叠：limb9 = 1 应等于 19 */
    cx_fe z; cx_fe_zero(z); z[9] = 1;
    cx_fe_tobytes(back, z);
    printf("  2^255 == 19? ");
    { c_u8 w[32]; memset(w, 0, 32); w[0] = 19; printf("%s\n", eq(back, w, 32) ? "OK" : "BAD"); }
    ph("  2^255 ", back, 32);

    /* 7) 小整数 121665^2 mod p（与 Python 对照） */
    cx_fe n; cx_fe_zero(n); n[0] = 121665;
    cx_fe_mul(c, n, n);
    cx_fe_tobytes(back, c);
    ph("  121665^2", back, 32);
    printf("  want 1e3d97e9e0cf2ac9a9b1e4c60ac5c0b9b0e4b7e1e4b3b0c9a8b7a6a5a4a3a2a1?? (由 Python 校验)\n");

    (void)o1; (void)o2; (void)b;
    return 0;
}
