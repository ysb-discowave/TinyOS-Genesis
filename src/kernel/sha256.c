#include "sha256.h"
#include "libc.h"

/* ============================================================
 * SHA-256（FIPS 180-4）—— 内核自用
 * ------------------------------------------------------------
 * 用途：
 *   1) 口令哈希：/etc/shadow 存 sha256(salt || password) 的十六进制
 *   2) SSH 服务端的 KEX/MAC 原语（用户态有独立的一份实现）
 * 这里刻意不依赖任何标准库，只用 kernel/libc.c 里的 memcpy/memset。
 * ============================================================ */

static const u32 K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define S0(x) (ROR(x,2) ^ ROR(x,13) ^ ROR(x,22))
#define S1(x) (ROR(x,6) ^ ROR(x,11) ^ ROR(x,25))
#define s0(x) (ROR(x,7) ^ ROR(x,18) ^ ((x) >> 3))
#define s1(x) (ROR(x,17) ^ ROR(x,19) ^ ((x) >> 10))

static void transform(sha256_ctx *c, const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) |
               ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
    for (int i = 16; i < 64; i++)
        w[i] = s1(w[i - 2]) + w[i - 7] + s0(w[i - 15]) + w[i - 16];

    u32 a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    u32 e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];

    for (int i = 0; i < 64; i++) {
        u32 t1 = h + S1(e) + ((e & f) ^ ((~e) & g)) + K[i] + w[i];
        u32 t2 = S0(a) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void sha256_init(sha256_ctx *c) {
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0;
    c->buflen = 0;
}

void sha256_update(sha256_ctx *c, const u8 *data, u32 len) {
    for (u32 i = 0; i < len; i++) {
        c->buf[c->buflen++] = data[i];
        if (c->buflen == 64) { transform(c, c->buf); c->bitlen += 512; c->buflen = 0; }
    }
}

void sha256_final(sha256_ctx *c, u8 out[32]) {
    u32 i = c->buflen;
    /* 追加 0x80，然后补零到 56 mod 64 */
    c->buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) c->buf[i++] = 0;
        transform(c, c->buf);
        i = 0;
    }
    while (i < 56) c->buf[i++] = 0;
    c->bitlen += (u64)c->buflen * 8;
    for (int k = 0; k < 8; k++)
        c->buf[56 + k] = (u8)((c->bitlen >> (56 - k * 8)) & 0xFF);
    transform(c, c->buf);
    for (int k = 0; k < 8; k++) {
        out[k * 4]     = (u8)(c->state[k] >> 24);
        out[k * 4 + 1] = (u8)(c->state[k] >> 16);
        out[k * 4 + 2] = (u8)(c->state[k] >> 8);
        out[k * 4 + 3] = (u8)(c->state[k]);
    }
}

void sha256(const u8 *data, u32 len, u8 out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void sha256_hex(const u8 *data, u32 len, char out[65]) {
    static const char *H = "0123456789abcdef";
    u8 d[32];
    sha256(data, len, d);
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = H[(d[i] >> 4) & 0xF];
        out[i * 2 + 1] = H[d[i] & 0xF];
    }
    out[64] = 0;
}
