/* ============================================================
 * sshd_crypto.h —— SSH 服务端需要的密码学原语（header-only）
 * ------------------------------------------------------------
 * 为什么是 header-only：同一份代码要被两处使用 ——
 *   1) SSHD.TNCR（TinyOS 用户态，-nostdinc、无 libc）
 *   2) 宿主上的单元测试（tools/compiler/sshd_crypto_test.c）
 * 因此这里不包含任何系统头文件，也不用 64 位除法/取模
 * （32 位 freestanding 目标下 __udivdi3 之类的 libgcc 助手并不存在）。
 *
 * 提供：
 *   SHA-256 / SHA-512 / HMAC-SHA256          —— KEX 与 MAC
 *   AES-128 加密 + CTR 模式                   —— 传输加密
 *   X25519 (Curve25519 ECDH)                  —— curve25519-sha256
 *   Ed25519 (签名/验签)                       —— ssh-ed25519 主机密钥
 * ============================================================ */
#ifndef TINYOS_SSHD_CRYPTO_H
#define TINYOS_SSHD_CRYPTO_H

typedef unsigned char      c_u8;
typedef unsigned short     c_u16;
typedef unsigned int       c_u32;
typedef unsigned long long c_u64;

static void cx_memzero(void *p, c_u32 n) {
    c_u8 *q = (c_u8*)p;
    while (n--) *q++ = 0;
}
static void cx_memcpy(void *d, const void *s, c_u32 n) {
    c_u8 *a = (c_u8*)d;
    const c_u8 *b = (const c_u8*)s;
    while (n--) *a++ = *b++;
}
static int cx_memcmp(const void *a, const void *b, c_u32 n) {
    const c_u8 *x = (const c_u8*)a, *y = (const c_u8*)b;
    while (n--) { if (*x != *y) return (*x < *y) ? -1 : 1; x++; y++; }
    return 0;
}

/* ======================= SHA-256 ======================= */
typedef struct { c_u32 s[8]; c_u64 bitlen; c_u32 buflen; c_u8 buf[64]; } cx_sha256_ctx;

static const c_u32 CX_K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
#define CX_ROR32(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CX_S256_0(x) (CX_ROR32(x,2) ^ CX_ROR32(x,13) ^ CX_ROR32(x,22))
#define CX_S256_1(x) (CX_ROR32(x,6) ^ CX_ROR32(x,11) ^ CX_ROR32(x,25))
#define CX_s256_0(x) (CX_ROR32(x,7) ^ CX_ROR32(x,18) ^ ((x) >> 3))
#define CX_s256_1(x) (CX_ROR32(x,17) ^ CX_ROR32(x,19) ^ ((x) >> 10))

static void cx_sha256_block(cx_sha256_ctx *c, const c_u8 *p) {
    c_u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((c_u32)p[i*4] << 24) | ((c_u32)p[i*4+1] << 16) |
               ((c_u32)p[i*4+2] << 8) | (c_u32)p[i*4+3];
    for (int i = 16; i < 64; i++)
        w[i] = CX_s256_1(w[i-2]) + w[i-7] + CX_s256_0(w[i-15]) + w[i-16];
    c_u32 a=c->s[0],b=c->s[1],cc=c->s[2],d=c->s[3],e=c->s[4],f=c->s[5],g=c->s[6],h=c->s[7];
    for (int i = 0; i < 64; i++) {
        c_u32 t1 = h + CX_S256_1(e) + ((e & f) ^ ((~e) & g)) + CX_K256[i] + w[i];
        c_u32 t2 = CX_S256_0(a) + ((a & b) ^ (a & cc) ^ (b & cc));
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->s[0]+=a; c->s[1]+=b; c->s[2]+=cc; c->s[3]+=d;
    c->s[4]+=e; c->s[5]+=f; c->s[6]+=g; c->s[7]+=h;
}
static void cx_sha256_init(cx_sha256_ctx *c) {
    c->s[0]=0x6a09e667u; c->s[1]=0xbb67ae85u; c->s[2]=0x3c6ef372u; c->s[3]=0xa54ff53au;
    c->s[4]=0x510e527fu; c->s[5]=0x9b05688cu; c->s[6]=0x1f83d9abu; c->s[7]=0x5be0cd19u;
    c->bitlen = 0; c->buflen = 0;
}
static void cx_sha256_update(cx_sha256_ctx *c, const c_u8 *d, c_u32 n) {
    for (c_u32 i = 0; i < n; i++) {
        c->buf[c->buflen++] = d[i];
        if (c->buflen == 64) { cx_sha256_block(c, c->buf); c->bitlen += 512; c->buflen = 0; }
    }
}
static void cx_sha256_final(cx_sha256_ctx *c, c_u8 out[32]) {
    c_u32 i = c->buflen;
    c->buf[i++] = 0x80;
    if (i > 56) { while (i < 64) c->buf[i++] = 0; cx_sha256_block(c, c->buf); i = 0; }
    while (i < 56) c->buf[i++] = 0;
    c->bitlen += (c_u64)c->buflen * 8;
    for (int k = 0; k < 8; k++) c->buf[56+k] = (c_u8)((c->bitlen >> (56 - k*8)) & 0xFF);
    cx_sha256_block(c, c->buf);
    for (int k = 0; k < 8; k++) {
        out[k*4]   = (c_u8)(c->s[k] >> 24);
        out[k*4+1] = (c_u8)(c->s[k] >> 16);
        out[k*4+2] = (c_u8)(c->s[k] >> 8);
        out[k*4+3] = (c_u8)(c->s[k]);
    }
}
static void cx_sha256(const c_u8 *d, c_u32 n, c_u8 out[32]) {
    cx_sha256_ctx c; cx_sha256_init(&c); cx_sha256_update(&c, d, n); cx_sha256_final(&c, out);
}

/* ======================= SHA-512 ======================= */
typedef struct { c_u64 s[8]; c_u64 bitlen; c_u32 buflen; c_u8 buf[128]; } cx_sha512_ctx;

static const c_u64 CX_K512[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};
#define CX_ROR64(x,n) (((x) >> (n)) | ((x) << (64 - (n))))
#define CX_S512_0(x) (CX_ROR64(x,28) ^ CX_ROR64(x,34) ^ CX_ROR64(x,39))
#define CX_S512_1(x) (CX_ROR64(x,14) ^ CX_ROR64(x,18) ^ CX_ROR64(x,41))
#define CX_s512_0(x) (CX_ROR64(x,1) ^ CX_ROR64(x,8) ^ ((x) >> 7))
#define CX_s512_1(x) (CX_ROR64(x,19) ^ CX_ROR64(x,61) ^ ((x) >> 6))

static void cx_sha512_block(cx_sha512_ctx *c, const c_u8 *p) {
    c_u64 w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int k = 0; k < 8; k++) w[i] = (w[i] << 8) | p[i*8+k];
    }
    for (int i = 16; i < 80; i++)
        w[i] = CX_s512_1(w[i-2]) + w[i-7] + CX_s512_0(w[i-15]) + w[i-16];
    c_u64 a=c->s[0],b=c->s[1],cc=c->s[2],d=c->s[3],e=c->s[4],f=c->s[5],g=c->s[6],h=c->s[7];
    for (int i = 0; i < 80; i++) {
        c_u64 t1 = h + CX_S512_1(e) + ((e & f) ^ ((~e) & g)) + CX_K512[i] + w[i];
        c_u64 t2 = CX_S512_0(a) + ((a & b) ^ (a & cc) ^ (b & cc));
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->s[0]+=a; c->s[1]+=b; c->s[2]+=cc; c->s[3]+=d;
    c->s[4]+=e; c->s[5]+=f; c->s[6]+=g; c->s[7]+=h;
}
static void cx_sha512_init(cx_sha512_ctx *c) {
    c->s[0]=0x6a09e667f3bcc908ULL; c->s[1]=0xbb67ae8584caa73bULL;
    c->s[2]=0x3c6ef372fe94f82bULL; c->s[3]=0xa54ff53a5f1d36f1ULL;
    c->s[4]=0x510e527fade682d1ULL; c->s[5]=0x9b05688c2b3e6c1fULL;
    c->s[6]=0x1f83d9abfb41bd6bULL; c->s[7]=0x5be0cd19137e2179ULL;
    c->bitlen = 0; c->buflen = 0;
}
static void cx_sha512_update(cx_sha512_ctx *c, const c_u8 *d, c_u32 n) {
    for (c_u32 i = 0; i < n; i++) {
        c->buf[c->buflen++] = d[i];
        if (c->buflen == 128) { cx_sha512_block(c, c->buf); c->bitlen += 1024; c->buflen = 0; }
    }
}
static void cx_sha512_final(cx_sha512_ctx *c, c_u8 out[64]) {
    c_u32 i = c->buflen;
    c->buf[i++] = 0x80;
    if (i > 112) { while (i < 128) c->buf[i++] = 0; cx_sha512_block(c, c->buf); i = 0; }
    while (i < 112) c->buf[i++] = 0;
    c->bitlen += (c_u64)c->buflen * 8;
    /* 128 位长度字段；高 64 位在 32 位实现里恒为 0（消息长度 < 2^61 位） */
    for (int k = 0; k < 8; k++) c->buf[112+k] = 0;
    for (int k = 0; k < 8; k++) c->buf[120+k] = (c_u8)((c->bitlen >> (56 - k*8)) & 0xFF);
    cx_sha512_block(c, c->buf);
    for (int k = 0; k < 8; k++)
        for (int j = 0; j < 8; j++)
            out[k*8+j] = (c_u8)(c->s[k] >> (56 - j*8));
}
static void cx_sha512(const c_u8 *d, c_u32 n, c_u8 out[64]) {
    cx_sha512_ctx c; cx_sha512_init(&c); cx_sha512_update(&c, d, n); cx_sha512_final(&c, out);
}

/* ======================= HMAC-SHA256 ======================= */
typedef struct { cx_sha256_ctx inner; c_u8 opad[64]; } cx_hmac256_ctx;

static void cx_hmac256_init(cx_hmac256_ctx *h, const c_u8 *key, c_u32 klen) {
    c_u8 k[64], ipad[64];
    cx_memzero(k, 64);
    if (klen > 64) cx_sha256(key, klen, k);          /* 长于块长先哈希 */
    else           cx_memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = (c_u8)(k[i] ^ 0x36); h->opad[i] = (c_u8)(k[i] ^ 0x5C); }
    cx_sha256_init(&h->inner);
    cx_sha256_update(&h->inner, ipad, 64);
    cx_memzero(k, 64); cx_memzero(ipad, 64);
}
static void cx_hmac256_update(cx_hmac256_ctx *h, const c_u8 *d, c_u32 n) {
    cx_sha256_update(&h->inner, d, n);
}
static void cx_hmac256_final(cx_hmac256_ctx *h, c_u8 out[32]) {
    c_u8 ih[32];
    cx_sha256_final(&h->inner, ih);
    cx_sha256_ctx ctx;
    cx_sha256_init(&ctx);
    cx_sha256_update(&ctx, h->opad, 64);
    cx_sha256_update(&ctx, ih, 32);
    cx_sha256_final(&ctx, out);
    cx_memzero(ih, 32);
}
static void cx_hmac256(const c_u8 *key, c_u32 klen, const c_u8 *d, c_u32 n, c_u8 out[32]) {
    cx_hmac256_ctx h; cx_hmac256_init(&h, key, klen); cx_hmac256_update(&h, d, n); cx_hmac256_final(&h, out);
}

/* ======================= AES-128 + CTR ======================= */
static const c_u8 CX_SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
static const c_u8 CX_RCON[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36};

typedef struct { c_u32 rk[44]; } cx_aes128;

static c_u32 cx_aes_subword(c_u32 w) {
    return ((c_u32)CX_SBOX[(w >> 24) & 0xFF] << 24) |
           ((c_u32)CX_SBOX[(w >> 16) & 0xFF] << 16) |
           ((c_u32)CX_SBOX[(w >> 8) & 0xFF] << 8) |
           ((c_u32)CX_SBOX[w & 0xFF]);
}
static void cx_aes128_init(cx_aes128 *a, const c_u8 key[16]) {
    for (int i = 0; i < 4; i++)
        a->rk[i] = ((c_u32)key[i*4] << 24) | ((c_u32)key[i*4+1] << 16) |
                   ((c_u32)key[i*4+2] << 8) | (c_u32)key[i*4+3];
    for (int i = 4; i < 44; i++) {
        c_u32 t = a->rk[i-1];
        if (i % 4 == 0) t = cx_aes_subword((t << 8) | (t >> 24)) ^ ((c_u32)CX_RCON[i/4] << 24);
        a->rk[i] = a->rk[i-4] ^ t;
    }
}
#define CX_XT(x) (((x) << 1) ^ ((((x) >> 7) & 1) * 0x1B))
static void cx_aes128_encrypt_block(const cx_aes128 *a, const c_u8 in[16], c_u8 out[16]) {
    c_u8 st[16];
    for (int i = 0; i < 16; i++) st[i] = (c_u8)(in[i] ^ (c_u8)(a->rk[i/4] >> (24 - (i%4)*8)));
    for (int round = 1; round <= 10; round++) {
        c_u8 t[16];
        for (int i = 0; i < 16; i++) t[i] = CX_SBOX[st[i]];               /* SubBytes */
        {   /* ShiftRows（列主序状态下，行 r 左移 r） */
            c_u8 s[16];
            s[0]=t[0];  s[1]=t[5];  s[2]=t[10]; s[3]=t[15];
            s[4]=t[4];  s[5]=t[9];  s[6]=t[14]; s[7]=t[3];
            s[8]=t[8];  s[9]=t[13]; s[10]=t[2]; s[11]=t[7];
            s[12]=t[12];s[13]=t[1]; s[14]=t[6]; s[15]=t[11];
            for (int i = 0; i < 16; i++) t[i] = s[i];
        }
        if (round != 10) {                                                /* MixColumns */
            for (int c = 0; c < 4; c++) {
                c_u8 *p = &t[c*4];
                c_u8 a0=p[0],a1=p[1],a2=p[2],a3=p[3];
                c_u8 x = (c_u8)(a0 ^ a1 ^ a2 ^ a3);
                p[0] = (c_u8)(a0 ^ x ^ CX_XT((c_u8)(a0 ^ a1)));
                p[1] = (c_u8)(a1 ^ x ^ CX_XT((c_u8)(a1 ^ a2)));
                p[2] = (c_u8)(a2 ^ x ^ CX_XT((c_u8)(a2 ^ a3)));
                p[3] = (c_u8)(a3 ^ x ^ CX_XT((c_u8)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) st[i] = (c_u8)(t[i] ^ (c_u8)(a->rk[round*4 + i/4] >> (24 - (i%4)*8)));
    }
    for (int i = 0; i < 16; i++) out[i] = st[i];
}

/* CTR：SSH 的 aes128-ctr，初始计数器 = IV，每块整体当作 128 位大端加一 */
typedef struct { cx_aes128 aes; c_u8 ctr[16]; c_u8 stream[16]; c_u32 used; } cx_aes128ctr;

static void cx_ctr_inc(c_u8 c[16]) {
    for (int i = 15; i >= 0; i--) { if (++c[i]) break; }
}
static void cx_aes128ctr_init(cx_aes128ctr *s, const c_u8 key[16], const c_u8 iv[16]) {
    cx_aes128_init(&s->aes, key);
    cx_memcpy(s->ctr, iv, 16);
    s->used = 16;
}
static void cx_aes128ctr_xor(cx_aes128ctr *s, c_u8 *buf, c_u32 n) {
    c_u32 i = 0;
    while (i < n) {
        if (s->used == 16) {
            cx_aes128_encrypt_block(&s->aes, s->ctr, s->stream);
            cx_ctr_inc(s->ctr);
            s->used = 0;
        }
        buf[i++] ^= s->stream[s->used++];
    }
}

/* ============================================================
 * GF(2^255-19)  —— X25519 与 Ed25519 共用的素域
 * ------------------------------------------------------------
 * 表示：10 个 limb，radix 2^25.5（偶数下标 26 位，奇数下标 25 位），
 * 即 v = s[0] + s[1]·2^26 + s[2]·2^51 + s[3]·2^77 + …
 * 这是 ref10/donna 的经典表示：乘法只需 int64 累加，
 * **不需要任何 64 位除法/取模**（32 位 freestanding 下没有 __udivdi3）。
 * limb 允许为负（加减后不做强制非负），进位链用"四舍五入"式收敛。
 * ============================================================ */
typedef int    cx_i32;
typedef long long cx_i64;
typedef cx_i32 cx_fe[10];

/* 进位链：把 10 个（可能为大负数/大正数的）int64 limb 收敛到
 * 每个 limb 的绝对值 < 2^25 量级，同时保持数值 mod (2^255-19) 不变。 */
static void cx_carry10(cx_i64 *t) {
    cx_i64 c;
    c = (t[0] + ((cx_i64)1 << 25)) >> 26; t[1] += c; t[0] -= c << 26;
    c = (t[4] + ((cx_i64)1 << 25)) >> 26; t[5] += c; t[4] -= c << 26;
    c = (t[1] + ((cx_i64)1 << 24)) >> 25; t[2] += c; t[1] -= c << 25;
    c = (t[5] + ((cx_i64)1 << 24)) >> 25; t[6] += c; t[5] -= c << 25;
    c = (t[2] + ((cx_i64)1 << 25)) >> 26; t[3] += c; t[2] -= c << 26;
    c = (t[6] + ((cx_i64)1 << 25)) >> 26; t[7] += c; t[6] -= c << 26;
    c = (t[3] + ((cx_i64)1 << 24)) >> 25; t[4] += c; t[3] -= c << 25;
    c = (t[7] + ((cx_i64)1 << 24)) >> 25; t[8] += c; t[7] -= c << 25;
    c = (t[4] + ((cx_i64)1 << 25)) >> 26; t[5] += c; t[4] -= c << 26;
    c = (t[8] + ((cx_i64)1 << 25)) >> 26; t[9] += c; t[8] -= c << 26;
    c = (t[9] + ((cx_i64)1 << 24)) >> 25; t[0] += c * 19; t[9] -= c << 25;
    c = (t[0] + ((cx_i64)1 << 25)) >> 26; t[1] += c; t[0] -= c << 26;
}

static void cx_fe_zero(cx_fe h) { for (int i = 0; i < 10; i++) h[i] = 0; }
static void cx_fe_one(cx_fe h)  { cx_fe_zero(h); h[0] = 1; }
static void cx_fe_copy(cx_fe h, const cx_fe f) { for (int i = 0; i < 10; i++) h[i] = f[i]; }

static void cx_fe_add(cx_fe h, const cx_fe f, const cx_fe g) {
    cx_i64 t[10];
    for (int i = 0; i < 10; i++) t[i] = (cx_i64)f[i] + (cx_i64)g[i];
    cx_carry10(t);
    for (int i = 0; i < 10; i++) h[i] = (cx_i32)t[i];
}
static void cx_fe_sub(cx_fe h, const cx_fe f, const cx_fe g) {
    cx_i64 t[10];
    for (int i = 0; i < 10; i++) t[i] = (cx_i64)f[i] - (cx_i64)g[i];
    cx_carry10(t);
    for (int i = 0; i < 10; i++) h[i] = (cx_i32)t[i];
}
static void cx_fe_neg(cx_fe h, const cx_fe f) { for (int i = 0; i < 10; i++) h[i] = -f[i]; }

/* 乘一个小的正整数（例如 2、121665）：仍走 int64 累加 + 进位链 */
static void cx_fe_mul_small(cx_fe h, const cx_fe f, cx_i32 k) {
    cx_i64 t[10];
    for (int i = 0; i < 10; i++) t[i] = (cx_i64)f[i] * (cx_i64)k;
    cx_carry10(t);
    for (int i = 0; i < 10; i++) h[i] = (cx_i32)t[i];
}

/* 卷积：x = 2^25.5，limb i 的权重是 26·⌈i/2⌉ + 25·⌊i/2⌋。
 *
 * 一个容易踩的坑：权重序列是 0,26,51,77,102,…，**不是**等差的 25.5i，
 * 因此 W[i]+W[j] 与 W[i+j] 之间存在一个可正可负的偏差。逐一推导可得
 *     W[i]+W[j] = W[i+j] + (1 当且仅当 i、j 同为奇数)
 * 也就是说"两个奇数位 limb 相乘"会天然多出一个 2 的因子，必须显式补上
 * —— 少了这一步，2^26 × 2^26 会算成 2^51 而不是 2^52（本项目就是这样
 * 先写错、再用 Python 镜像算法逐位对照才定位到的）。
 *
 * 折回时用到 2^255 ≡ 19：下标 k ≥ 10 的项挪到 k-10 并乘 19。
 * 累加器量级：≤ 10 项 × 19 × 2·(2^25)² ≈ 2^58.7，int64 安全。 */
static void cx_fe_mul(cx_fe h, const cx_fe f, const cx_fe g) {
    cx_i64 t[19];
    for (int i = 0; i < 19; i++) t[i] = 0;
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 10; j++) {
            cx_i64 p = (cx_i64)f[i] * (cx_i64)g[j];
            if ((i & 1) && (j & 1)) p += p;      /* 奇数位×奇数位：补上因子 2 */
            int k = i + j;
            if (k < 10) t[k] += p;
            else        t[k - 10] += 19 * p;
        }
    }
    cx_carry10(t);
    for (int i = 0; i < 10; i++) h[i] = (cx_i32)t[i];
}
static void cx_fe_sq(cx_fe h, const cx_fe f) { cx_fe_mul(h, f, f); }

/* 字节 <-> limb：逐位装配，避免手工推导错位的移位常量 */
static void cx_fe_frombytes(cx_fe h, const c_u8 *s) {
    c_u32 v[10];
    int bi = 0, nbits = 0;
    c_u64 acc = 0;
    for (int i = 0; i < 10; i++) {
        int want = (i & 1) ? 25 : 26;
        while (nbits < want) { acc |= (c_u64)s[bi++] << nbits; nbits += 8; }
        v[i] = (c_u32)(acc & (((c_u64)1 << want) - 1));
        acc >>= want; nbits -= want;
    }
    for (int i = 0; i < 10; i++) h[i] = (cx_i32)v[i];
}

static const c_u8 CX_P_BYTES[32] = {
    0xed,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f
};

/* 把 limb 规约成 [0, p) 的规范形式再输出 32 字节小端。
 * 思路：先做"向下取整"的进位链让所有 limb 非负，再把第 9 个 limb 的
 * 溢出按 2^255≡19 折回低位；此时数值 < 2^255，直接位装配成 32 字节，
 * 最后最多 2 次条件减去 p 即可（因为数值 < 2^255+40 < 2p）。 */
static void cx_fe_tobytes(c_u8 *out, const cx_fe f) {
    cx_i64 t[10];
    for (int i = 0; i < 10; i++) t[i] = f[i];

    for (int pass = 0; pass < 3; pass++) {
        cx_i64 c;
        c = t[0] >> 26; t[1] += c; t[0] -= c << 26;
        c = t[1] >> 25; t[2] += c; t[1] -= c << 25;
        c = t[2] >> 26; t[3] += c; t[2] -= c << 26;
        c = t[3] >> 25; t[4] += c; t[3] -= c << 25;
        c = t[4] >> 26; t[5] += c; t[4] -= c << 26;
        c = t[5] >> 25; t[6] += c; t[5] -= c << 25;
        c = t[6] >> 26; t[7] += c; t[6] -= c << 26;
        c = t[7] >> 25; t[8] += c; t[7] -= c << 25;
        c = t[8] >> 26; t[9] += c; t[8] -= c << 26;
        c = t[9] >> 25; t[0] += c * 19; t[9] -= c << 25;
    }

    for (int i = 0; i < 32; i++) out[i] = 0;
    int oi = 0, nbits = 0;
    c_u64 acc = 0;
    for (int i = 0; i < 10; i++) {
        int have = (i & 1) ? 25 : 26;
        acc |= ((c_u64)(c_u32)t[i]) << nbits;
        nbits += have;
        while (nbits >= 8) { out[oi++] = (c_u8)(acc & 0xFF); acc >>= 8; nbits -= 8; }
    }
    if (nbits > 0 && oi < 32) out[oi++] = (c_u8)(acc & 0xFF);

    for (int guard = 0; guard < 4; guard++) {
        int ge = 1;
        for (int i = 31; i >= 0; i--) {
            if (out[i] > CX_P_BYTES[i]) { ge = 1; break; }
            if (out[i] < CX_P_BYTES[i]) { ge = 0; break; }
        }
        if (!ge) break;
        int borrow = 0;
        for (int i = 0; i < 32; i++) {
            int d = (int)out[i] - (int)CX_P_BYTES[i] - borrow;
            borrow = (d < 0);
            out[i] = (c_u8)(d & 0xFF);
        }
    }
}

/* 是否为零（检查所有 limb 的规约形式都为 0） */
static int cx_fe_iszero(const cx_fe f) {
    c_u8 b[32];
    cx_fe_tobytes(b, f);
    for (int i = 0; i < 32; i++) if (b[i]) return 0;
    return 1;
}

/* 求逆：x^(p-2)。指数 (p-2) 的小端字节 = EB FF…FF 7F，
 * 用最朴素的二进制平方-乘（255 次平方 + ~250 次乘），
 * 在 32 位机器上也不过几万次 limb 乘法，完全可接受。 */
static void cx_fe_invert(cx_fe out, const cx_fe z) {
    static const c_u8 EXP[32] = {
        0xeb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f
    };
    cx_fe r, base;
    cx_fe_one(r);
    cx_fe_copy(base, z);
    for (int i = 0; i < 255; i++) {
        int bit = (EXP[i >> 3] >> (i & 7)) & 1;
        if (bit) cx_fe_mul(r, r, base);
        cx_fe_sq(base, base);
    }
    /* i==255 时 EXP[31] 的 bit7 = 0，循环到 254 已覆盖全部 1 位 */
    cx_fe_copy(out, r);
}

/* ======================= X25519 (Curve25519 ECDH) =======================
 * 标准 Montgomery 梯度（RFC 7748），a24 = (486662-2)/4 = 121665。
 * 非恒定时间（本项目的定位是"能用真 ssh 客户端连上"的教学实现），
 * 但每一步的公式与 RFC 7748 §5 逐条对应。
 */
static void cx_x25519(c_u8 out[32], const c_u8 scalar[32], const c_u8 upoint[32]) {
    c_u8 k[32];
    cx_memcpy(k, scalar, 32);
    k[0] &= 248; k[31] &= 127; k[31] |= 64;      /* clamp */

    cx_fe x1; cx_fe_frombytes(x1, upoint);
    cx_fe x2, z2, x3, z3;
    cx_fe_one(x2); cx_fe_zero(z2);
    cx_fe_copy(x3, x1); cx_fe_one(z3);

    int swap = 0;
    for (int t = 254; t >= 0; t--) {
        int kt = (k[t >> 3] >> (t & 7)) & 1;
        swap ^= kt;
        if (swap) {
            cx_fe tmp;
            cx_fe_copy(tmp, x2); cx_fe_copy(x2, x3); cx_fe_copy(x3, tmp);
            cx_fe_copy(tmp, z2); cx_fe_copy(z2, z3); cx_fe_copy(z3, tmp);
        }
        swap = kt;

        cx_fe A, AA, B, BB, E, C, D, DA, CB, t0, t1;
        cx_fe_add(A, x2, z2);  cx_fe_sq(AA, A);
        cx_fe_sub(B, x2, z2);  cx_fe_sq(BB, B);
        cx_fe_sub(E, AA, BB);
        cx_fe_add(C, x3, z3);
        cx_fe_sub(D, x3, z3);
        cx_fe_mul(DA, D, A);
        cx_fe_mul(CB, C, B);
        cx_fe_add(t0, DA, CB); cx_fe_sq(x3, t0);
        cx_fe_sub(t1, DA, CB); cx_fe_sq(t1, t1); cx_fe_mul(z3, x1, t1);
        cx_fe_mul(x2, AA, BB);
        cx_fe_mul_small(t0, E, 121665);
        cx_fe_add(t0, AA, t0);
        cx_fe_mul(z2, E, t0);
    }
    if (swap) {
        cx_fe tmp;
        cx_fe_copy(tmp, x2); cx_fe_copy(x2, x3); cx_fe_copy(x3, tmp);
        cx_fe_copy(tmp, z2); cx_fe_copy(z2, z3); cx_fe_copy(z3, tmp);
    }
    cx_fe zi, res;
    cx_fe_invert(zi, z2);
    cx_fe_mul(res, x2, zi);
    cx_fe_tobytes(out, res);

    /* 擦除中间状态（口令/密钥相关） */
    cx_memzero(k, 32);
}

/* 把 32 字节小端当作 X25519 的 u 坐标时，最高位必须被忽略（RFC 7748） */
static void cx_x25519_base(c_u8 out[32], const c_u8 scalar[32]) {
    c_u8 nine[32];
    cx_memzero(nine, 32);
    nine[0] = 9;
    cx_x25519(out, scalar, nine);
}

/* ======================= Ed25519（只做签名，SSH 主机密钥用） =======================
 * 采用扩展扭曲爱德华兹坐标 (X:Y:Z:T) 与 a=−1 的完备加法公式，
 * 因此不需要处理任何特例点。签名是确定性的：同一密钥 + 同一消息
 * 必须逐字节得到 RFC 8032 的结果，这正是我们做单测的方式。
 */
typedef struct { cx_fe X, Y, Z, T; } cx_ed_pt;

static cx_fe cx_ed_d;        /* 曲线常数 d = -121665/121666 mod p */
static cx_fe cx_ed_d2;       /* 2d */

static void cx_ed_pt_identity(cx_ed_pt *p) {
    cx_fe_zero(p->X); cx_fe_one(p->Y); cx_fe_one(p->Z); cx_fe_zero(p->T);
}

static void cx_ed_pt_add(cx_ed_pt *r, const cx_ed_pt *p, const cx_ed_pt *q) {
    cx_fe A, B, C, D, E, F, G, H, t0, t1;
    cx_fe_sub(t0, p->Y, p->X); cx_fe_sub(t1, q->Y, q->X); cx_fe_mul(A, t0, t1);
    cx_fe_add(t0, p->Y, p->X); cx_fe_add(t1, q->Y, q->X); cx_fe_mul(B, t0, t1);
    cx_fe_mul(C, p->T, q->T);  cx_fe_mul(C, C, cx_ed_d2);
    cx_fe_mul(D, p->Z, q->Z);  cx_fe_add(D, D, D);
    cx_fe_sub(E, B, A);
    cx_fe_sub(F, D, C);
    cx_fe_add(G, D, C);
    cx_fe_add(H, B, A);
    cx_fe_mul(r->X, E, F);
    cx_fe_mul(r->Y, G, H);
    cx_fe_mul(r->T, E, H);
    cx_fe_mul(r->Z, F, G);
}

/* 倍点（dbl-2008-hwcd，a=−1）：比 add(p,p) 少一次乘法且不依赖 T */
static void cx_ed_pt_dbl(cx_ed_pt *r, const cx_ed_pt *p) {
    cx_fe A, B, C, D, E, F, G, H, t0;
    cx_fe_sq(A, p->X);
    cx_fe_sq(B, p->Y);
    cx_fe_sq(C, p->Z); cx_fe_add(C, C, C);      /* 2Z^2 */
    cx_fe_neg(D, A);                            /* a*A, a = -1 */
    cx_fe_add(t0, p->X, p->Y); cx_fe_sq(t0, t0);
    cx_fe_sub(t0, t0, A); cx_fe_sub(E, t0, B);  /* (X+Y)^2 - A - B */
    cx_fe_add(G, D, B);
    cx_fe_sub(F, G, C);
    cx_fe_sub(H, D, B);
    cx_fe_mul(r->X, E, F);
    cx_fe_mul(r->Y, G, H);
    cx_fe_mul(r->T, E, H);
    cx_fe_mul(r->Z, F, G);
}

/* 标量乘（左到右 double-and-add）。scalar 为 32 字节小端。 */
static void cx_ed_scalarmult(cx_ed_pt *r, const c_u8 scalar[32], const cx_ed_pt *p) {
    cx_ed_pt acc;
    cx_ed_pt_identity(&acc);
    for (int i = 255; i >= 0; i--) {
        cx_ed_pt_dbl(&acc, &acc);
        if ((scalar[i >> 3] >> (i & 7)) & 1) cx_ed_pt_add(&acc, &acc, p);
    }
    *r = acc;
}

/* 压缩点 -> 32 字节：y 的小端，最高位放 x 的奇偶性 */
static void cx_ed_pt_encode(c_u8 out[32], const cx_ed_pt *p) {
    cx_fe zi, x, y;
    cx_fe_invert(zi, p->Z);
    cx_fe_mul(x, p->X, zi);
    cx_fe_mul(y, p->Y, zi);
    cx_fe_tobytes(out, y);
    c_u8 xb[32];
    cx_fe_tobytes(xb, x);
    out[31] = (c_u8)((out[31] & 0x7F) | ((xb[0] & 1) << 7));
    cx_memzero(xb, 32);
}

/* ---- 标量模 L 运算（L = 2^252 + 27742317777372353535851937790883648493）----
 * 故意使用最简单的"逐位双倍 + 条件减"算法：512 次迭代、每次只做几个
 * 32 位加减，总共几万条指令，比照抄 ref10 那段 24-limb 的巨型展开
 * sc_reduce 更不容易写错 —— 正确性在这里比速度重要得多。 */
static const c_u32 CX_L[8] = {
    0x5CF5D3EDu, 0x5812631Au, 0xA2F79CD6u, 0x14DEF9DEu,
    0x00000000u, 0x00000000u, 0x00000000u, 0x10000000u
};

static int cx_u32_ge8(const c_u32 *a, const c_u32 *b) {
    for (int i = 7; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return 0;
    }
    return 1;
}
static void cx_u32_sub8(c_u32 *a, const c_u32 *b) {
    c_u32 borrow = 0;
    for (int i = 0; i < 8; i++) {
        c_u64 d = (c_u64)a[i] - (c_u64)b[i] - (c_u64)borrow;
        a[i] = (c_u32)(d & 0xFFFFFFFFu);
        borrow = (c_u32)((d >> 32) & 1u);
    }
}

/* 512 位（64 字节小端）数对 L 取模，结果写进 32 字节 */
static void cx_sc_reduce(c_u8 out[32], const c_u8 in[64]) {
    c_u32 r[8];
    for (int i = 0; i < 8; i++) r[i] = 0;

    for (int bit = 511; bit >= 0; bit--) {
        c_u32 carry = (c_u32)((in[bit >> 3] >> (bit & 7)) & 1);
        for (int j = 0; j < 8; j++) {          /* r = 2r + bit */
            c_u32 nv = (c_u32)((r[j] << 1) | carry);
            carry = r[j] >> 31;
            r[j] = nv;
        }
        while (cx_u32_ge8(r, CX_L)) cx_u32_sub8(r, CX_L);
    }
    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (c_u8)(r[i]);
        out[i * 4 + 1] = (c_u8)(r[i] >> 8);
        out[i * 4 + 2] = (c_u8)(r[i] >> 16);
        out[i * 4 + 3] = (c_u8)(r[i] >> 24);
    }
}

/* out = (a*b + c) mod L，全部输入为 32 字节小端标量 */
static void cx_sc_muladd(c_u8 out[32], const c_u8 a[32], const c_u8 b[32], const c_u8 c[32]) {
    c_u32 x[8], y[8];
    for (int i = 0; i < 8; i++) {
        x[i] = (c_u32)a[i*4] | ((c_u32)a[i*4+1] << 8) | ((c_u32)a[i*4+2] << 16) | ((c_u32)a[i*4+3] << 24);
        y[i] = (c_u32)b[i*4] | ((c_u32)b[i*4+1] << 8) | ((c_u32)b[i*4+2] << 16) | ((c_u32)b[i*4+3] << 24);
    }
    c_u32 p[16];
    for (int i = 0; i < 16; i++) p[i] = 0;

    for (int i = 0; i < 8; i++) {                 /* 8x8 -> 16 limb 小学校乘法 */
        c_u64 carry = 0;
        for (int j = 0; j < 8; j++) {
            c_u64 t = (c_u64)x[i] * (c_u64)y[j] + (c_u64)p[i + j] + carry;
            p[i + j] = (c_u32)t;
            carry = t >> 32;
        }
        int k = i + 8;
        while (carry && k < 16) {
            c_u64 t = (c_u64)p[k] + carry;
            p[k] = (c_u32)t;
            carry = t >> 32;
            k++;
        }
    }
    {   /* 加上 c（< L < 2^253，不会溢出 16 limb） */
        c_u64 carry = 0;
        for (int i = 0; i < 8; i++) {
            c_u32 ci = (c_u32)c[i*4] | ((c_u32)c[i*4+1] << 8) | ((c_u32)c[i*4+2] << 16) | ((c_u32)c[i*4+3] << 24);
            c_u64 t = (c_u64)p[i] + (c_u64)ci + carry;
            p[i] = (c_u32)t;
            carry = t >> 32;
        }
        int k = 8;
        while (carry && k < 16) {
            c_u64 t = (c_u64)p[k] + carry;
            p[k] = (c_u32)t;
            carry = t >> 32;
            k++;
        }
    }
    c_u8 raw[64];
    for (int i = 0; i < 16; i++) {
        raw[i*4 + 0] = (c_u8)(p[i]);
        raw[i*4 + 1] = (c_u8)(p[i] >> 8);
        raw[i*4 + 2] = (c_u8)(p[i] >> 16);
        raw[i*4 + 3] = (c_u8)(p[i] >> 24);
    }
    cx_sc_reduce(out, raw);
}

/* 惰性初始化曲线常数：d = -121665/121666 mod p；d2 = 2d。
 * 直接"算"而不是硬编码 ref10 的 10-limb 常数表，
 * 可以避免抄错常数这种最难查的 bug。 */
static void cx_ed_init(void) {
    static int done = 0;
    if (done) return;
    cx_fe num, den, dinv;
    cx_fe_zero(num); num[0] = 121665;
    cx_fe_zero(den); den[0] = 121666;
    cx_fe_invert(dinv, den);
    cx_fe_mul(cx_ed_d, num, dinv);
    cx_fe_neg(cx_ed_d, cx_ed_d);
    cx_fe_add(cx_ed_d2, cx_ed_d, cx_ed_d);
    done = 1;
}

/* RFC 8032 的 Ed25519 基点 B（小端压缩形式）
 *   x = 0x216936D3CD6E53FEC0A4E231FDD6DC5C692CC7609525A7B2C9562D608F25D51A
 *   y = 0x6666666666666666666666666666666666666666666666666666666666666658
 * 下面就是它们的小端字节序。 */
static const c_u8 CX_ED_BX[32] = {
    0x1a,0xd5,0x25,0x8f,0x60,0x2d,0x56,0xc9,0xb2,0xa7,0x25,0x95,0x60,0xc7,0x2c,0x69,
    0x5c,0xdc,0xd6,0xfd,0x31,0xe2,0xa4,0xc0,0xfe,0x53,0x6e,0xcd,0xd3,0x36,0x69,0x21
};
static const c_u8 CX_ED_BY[32] = {
    0x58,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
    0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66
};

static void cx_ed_base(cx_ed_pt *p) {
    cx_ed_init();
    cx_fe_frombytes(p->X, CX_ED_BX);
    cx_fe_frombytes(p->Y, CX_ED_BY);
    cx_fe_one(p->Z);
    cx_fe_mul(p->T, p->X, p->Y);      /* T = X*Y（因为 Z=1） */
}

/* 从 32 字节私钥种子推导 32 字节公钥（= 压缩的 a·B） */
static void cx_ed_public_key(c_u8 pub[32], const c_u8 seed[32]) {
    cx_ed_init();
    c_u8 h[64];
    cx_sha512(seed, 32, h);
    c_u8 a[32];
    cx_memcpy(a, h, 32);
    a[0] &= 248; a[31] &= 127; a[31] |= 64;
    cx_ed_pt B, A;
    cx_ed_base(&B);
    cx_ed_scalarmult(&A, a, &B);
    cx_ed_pt_encode(pub, &A);
    cx_memzero(h, 64); cx_memzero(a, 32);
}

/* 确定性签名：sig[64] = R || S，与 RFC 8032 §5.1.6 完全一致 */
static void cx_ed_sign(c_u8 sig[64], const c_u8 seed[32], const c_u8 *msg, c_u32 mlen) {
    cx_ed_init();
    c_u8 h[64];
    cx_sha512(seed, 32, h);

    c_u8 a[32], prefix[32];
    cx_memcpy(a, h, 32);
    cx_memcpy(prefix, h + 32, 32);
    a[0] &= 248; a[31] &= 127; a[31] |= 64;

    cx_ed_pt B, A;
    cx_ed_base(&B);
    cx_ed_scalarmult(&A, a, &B);
    c_u8 Aenc[32];
    cx_ed_pt_encode(Aenc, &A);

    /* r = SHA512(prefix || M) mod L  —— 流式喂入，不必拼接缓冲 */
    cx_sha512_ctx sc;
    cx_sha512_init(&sc);
    cx_sha512_update(&sc, prefix, 32);
    cx_sha512_update(&sc, msg, mlen);
    c_u8 rh[64];
    cx_sha512_final(&sc, rh);
    c_u8 r[32];
    cx_sc_reduce(r, rh);

    cx_ed_pt R;
    cx_ed_scalarmult(&R, r, &B);
    cx_ed_pt_encode(sig, &R);

    /* k = SHA512(R || A || M) mod L */
    cx_sha512_init(&sc);
    cx_sha512_update(&sc, sig, 32);
    cx_sha512_update(&sc, Aenc, 32);
    cx_sha512_update(&sc, msg, mlen);
    c_u8 kh[64];
    cx_sha512_final(&sc, kh);
    c_u8 k[32];
    cx_sc_reduce(k, kh);

    /* S = (r + k*a) mod L */
    cx_sc_muladd(sig + 32, k, a, r);

    cx_memzero(h, 64); cx_memzero(a, 32); cx_memzero(prefix, 32);
    cx_memzero(rh, 64); cx_memzero(kh, 64); cx_memzero(r, 32); cx_memzero(k, 32);
}

#endif /* TINYOS_SSHD_CRYPTO_H */
