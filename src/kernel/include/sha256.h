#ifndef TINYOS_SHA256_H
#define TINYOS_SHA256_H

#include "types.h"

/* 增量式 SHA-256（FIPS 180-4） */
typedef struct {
    u32 state[8];
    u64 bitlen;
    u32 buflen;
    u8  buf[64];
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const u8 *data, u32 len);
void sha256_final(sha256_ctx *c, u8 out[32]);

/* 一次性接口 */
void sha256(const u8 *data, u32 len, u8 out[32]);
/* 转小写十六进制（写入 65 字节，含结尾 0） */
void sha256_hex(const u8 *data, u32 len, char out[65]);

#endif
