#include "types.h"

/* ============================================================================
 * inflate.c -- 纯 C DEFLATE (RFC1951) 解码器
 * ---------------------------------------------------------------------------
 * 用途：romfs 里的 TNCR 以 zlib raw-deflate 压缩存储（Python 侧用
 *       zlib.compressobj(wbits=-15) 生成，无 2 字节 zlib 头、无 adler32），
 *       内核装载 romfs 时先在这里就地解压，再挂进 VFS。
 *
 * 支持：stored / fixed Huffman / dynamic Huffman，以及带重叠的 back-refer。
 * 不支持：zlib 容器头（我们只喂 raw 流）。
 * 资源：解码用的 Huffman 表是栈上数组；输出写进调用者给的缓冲。
 * 返回：成功 = 写入 dst 的字节数；失败 = (u32)-1。
 *
 * 本文件刻意只 include types.h，不碰 mm.h / console.h / libc.h，
 * 这样能在宿主端（Windows/macOS/Linux）原样编译做字节级单元测试。
 * ========================================================================== */

#define MAXBITS   15
#define MAXSYMS   288     /* 最大 literal/length 符号数 */
#define MAXDIST   32      /* 最大 distance 符号数 */

typedef struct {
    const u8 *src; u32 src_len; u32 pos;
    u32 bitbuf;  u32 bitcnt;          /* 已缓存的位（LSB 在前） */
    u8 *dst;     u32 dst_cap; u32 dst_pos;
} ctx_t;

/* ---- 位读取（DEFLATE 是 LSB-first 按位取）---- */
static int need_bits(ctx_t *c, int n, u32 *outv) {
    while (c->bitcnt < n) {
        if (c->pos >= c->src_len) return -1;     /* 流意外结束 */
        c->bitbuf |= (u32)c->src[c->pos++] << c->bitcnt;
        c->bitcnt += 8;
    }
    *outv = c->bitbuf & ((1u << n) - 1);
    c->bitbuf >>= n;
    c->bitcnt -= n;
    return 0;
}

static void align_byte(ctx_t *c) { c->bitbuf = 0; c->bitcnt = 0; }

/* ---- 规范化 Huffman 表 ----
 * lens[]: 每个符号的码长（0 = 不存在）。
 * 产出：
 *   cnt[len]   : 该长度的码的个数
 *   first[len] : 该长度第一个码的数值
 *   idx0[len]  : 该长度符号在平铺 decode_sym[] 里的起始下标
 *   decode_sym : 按 (码长, 码值) 排序后的符号
 */
typedef struct {
    u16 cnt[MAXBITS + 1];
    u16 first[MAXBITS + 1];
    u16 idx0[MAXBITS + 1];
} htree_t;

/* 把 lens[]（n 个符号）建成 htree；同时把解码符号表填进 decsyms[]。
 * 返回 0 成功；-1 过度订阅/非法。 */
static int build_tree(const u16 *lens, int n, htree_t *t, u16 *decsyms) {
    int i, len;
    for (len = 0; len <= MAXBITS; len++) t->cnt[len] = 0;
    for (i = 0; i < n; i++) t->cnt[lens[i]]++;

    /* 检查过度订阅（码空间被占满）*/
    {
        s32 left = 1;
        for (len = 1; len <= MAXBITS; len++) {
            left <<= 1;
            left -= (s32)t->cnt[len];
            if (left < 0) return -1;
        }
    }

    u16 firstval[MAXBITS + 1];
    u16 nextcode[MAXBITS + 1];
    u16 code = 0;
    for (len = 1; len <= MAXBITS; len++) {
        /* canonical：first[len] = (first[len-1] + cnt[len-1]) << 1，
         * 但 first[1] 恒为 0。len-1=0 时的 cnt[0]（不存在的符号数）
         * 绝不能计入码空间，否则整棵树的码值整体偏移——
         * fixed 树 cnt[0]==0 侥幸正确，动态树一旦有"缺失符号"就崩。 */
        u16 prev = (len > 1) ? t->cnt[len - 1] : 0;
        code = (u16)((code + prev) << 1);
        firstval[len] = code;
        t->first[len] = code;
    }
    /* idx0：该长度符号在平铺表里的起始位置 */
    {
        u16 acc = 0;
        for (len = 1; len <= MAXBITS; len++) {
            t->idx0[len] = acc;
            acc += t->cnt[len];
        }
    }
    for (len = 1; len <= MAXBITS; len++) nextcode[len] = firstval[len];

    /* 按原始符号顺序分配码值，填平铺解码表 */
    for (i = 0; i < n; i++) {
        len = lens[i];
        if (len == 0) continue;
        u16 cv = nextcode[len]++;
        decsyms[t->idx0[len] + (cv - t->first[len])] = (u16)i;
    }
    return 0;
}

/* 解一个 Huffman 符号；返回符号（0..）或 -1。
 * DEFLATE 是 LSB-first：流里先写的比特是码的最低位。
 * 累积法（同 zlib inflate）：val = (val<<1)|bit。
 * 读满 L 位后 val 恰好等于 canonical 码的整数值（因为最早读到的位
 * 落在权重 2^0，逐位左移后正好重建原码值），直接和 first[len] 比。 */
static int decode_sym(ctx_t *c, const htree_t *t, const u16 *decsyms) {
    u32 val = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        u32 bit;
        if (need_bits(c, 1, &bit) != 0) return -1;
        val = (val << 1) | bit;
        if (t->cnt[len]) {
            s32 idx = (s32)val - (s32)t->first[len];
            if (idx >= 0 && (s32)idx < (s32)t->cnt[len])
                return decsyms[t->idx0[len] + idx];
        }
    }
    return -1;
}

/* DEFLATE 长度码 257..285 的 base + extra bits */
static const u16 len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const u8 len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
/* 距离码 0..29 的 base + extra bits */
static const u16 dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const u8 dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* 动态块的码长码 16/17/18 的 extra bits */
#define LENCODE_EXTRA16 2   /* 重复前一个 3..6 次 */
#define LENCODE_EXTRA17 3   /* 重复 0  3..10 次 */
#define LENCODE_EXTRA18 7   /* 重复 0  11..138 次 */

/* 动态块里码长码的读取顺序 */
static const u8 cl_order[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

/* 固定 Huffman 表的码长 */
static void fixed_lens_lit(u16 *lens, int n) {
    int i;
    for (i = 0; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < n;   i++) lens[i] = 8;
}

/* 写一个输出字节；越界返回 -1 */
static int put_out(ctx_t *c, u8 b) {
    if (c->dst_pos >= c->dst_cap) return -1;
    c->dst[c->dst_pos++] = b;
    return 0;
}

/* 主解码：解一个完整 DEFLATE 流（可含多块）直到 BFINAL 块结束 */
u32 inflate_raw(const u8 *src, u32 src_len, u8 *dst, u32 dst_cap) {
    ctx_t c;
    c.src = src; c.src_len = src_len; c.pos = 0;
    c.bitbuf = 0; c.bitcnt = 0;
    c.dst = dst; c.dst_cap = dst_cap; c.dst_pos = 0;

    int bfinal;
    do {
        u32 h;
        if (need_bits(&c, 1, &h) != 0) return (u32)-1;
        bfinal = h;
        u32 btype;
        if (need_bits(&c, 2, &btype) != 0) return (u32)-1;

        if (btype == 0) {
            /* ---- stored：先对齐到字节边界 ---- */
            align_byte(&c);
            u32 len, nlen;
            if (need_bits(&c, 16, &len) != 0) return (u32)-1;
            align_byte(&c);
            if (need_bits(&c, 16, &nlen) != 0) return (u32)-1;
            if (((~len) & 0xFFFF) != nlen) return (u32)-1;  /* 校验 */
            for (u32 i = 0; i < len; i++) {
                if (c.pos >= c.src_len) return (u32)-1;
                if (put_out(&c, c.src[c.pos++]) != 0) return (u32)-1;
            }
        } else {
            /* ---- Huffman 块 ---- */
            htree_t lit; u16 litsyms[MAXSYMS];
            htree_t dist; u16 distsyms[MAXDIST];
            int nsym_lit, nsym_dist;

            if (btype == 1) {
                /* fixed */
                u16 lens[MAXSYMS];
                fixed_lens_lit(lens, MAXSYMS);
                if (build_tree(lens, MAXSYMS, &lit, litsyms) != 0) return (u32)-1;
                nsym_lit = MAXSYMS;
                u16 dlens[MAXDIST];
                for (int i = 0; i < MAXDIST; i++) dlens[i] = 5;
                if (build_tree(dlens, MAXDIST, &dist, distsyms) != 0) return (u32)-1;
                nsym_dist = MAXDIST;
            } else if (btype == 2) {
                /* dynamic */
                u32 hlit, hdist, hclen;
                if (need_bits(&c, 5, &hlit) != 0) return (u32)-1;
                if (need_bits(&c, 5, &hdist) != 0) return (u32)-1;
                if (need_bits(&c, 4, &hclen) != 0) return (u32)-1;
                hlit  += 257;
                hdist += 1;
                hclen += 4;
                if (hdist > MAXDIST || hclen > 19) return (u32)-1;

                /* 码长码表（最多 19 符号）*/
                u16 clens[19];
                int j;
                for (j = 0; j < 19; j++) clens[j] = 0;
                for (j = 0; j < (int)hclen; j++) {
                    u32 v;
                    if (need_bits(&c, 3, &v) != 0) return (u32)-1;
                    clens[cl_order[j]] = (u16)v;
                }
                htree_t cl; u16 clsyms[19];
                if (build_tree(clens, 19, &cl, clsyms) != 0) return (u32)-1;

                /* 读 literal/distance 的码长 */
                /* 标准 DEFLATE：码长符号流里
                   0..15      = 字面码长
                   16        = 重复上一个码长 3..6  次
                   17        = 重复 0           3..10 次
                   18        = 重复 0           11..138 次
                   共 total 个码长，但符号数更少（重复码被压缩）。 */
                /* 码长数组：literal(hlit 最大 286) + distance(hdist 最大 32)
                 * 合计最大 318，绝不能只按 MAXSYMS(288) 开，否则越界写。 */
                u16 lens[MAXSYMS + MAXDIST];
                int total = (int)hlit + (int)hdist;
                int i = 0;   /* 已填入的码长个数 */
                int k = 0;   /* 已读取的符号个数 */
                while (i < total) {
                    int sym = decode_sym(&c, &cl, clsyms);
                    if (sym < 0) return (u32)-1;
                    k++;
                    if (sym < 16) {
                        lens[i++] = (u16)sym;
                    } else {
                        u32 rep, val;
                        if (sym == 16) {
                            if (i == 0) return (u32)-1;       /* 没"上一个" */
                            val = lens[i - 1];
                            if (need_bits(&c, LENCODE_EXTRA16, &rep) != 0) return (u32)-1;
                            rep += 3;                          /* 3..6 */
                        } else if (sym == 17) {
                            val = 0;
                            if (need_bits(&c, LENCODE_EXTRA17, &rep) != 0) return (u32)-1;
                            rep += 3;                          /* 3..10 */
                        } else {
                            val = 0;
                            if (need_bits(&c, LENCODE_EXTRA18, &rep) != 0) return (u32)-1;
                            rep += 11;                         /* 11..138 */
                        }
                        if (i + (int)rep > total) return (u32)-1;  /* 越界即非法流 */
                        for (u32 r = 0; r < rep; r++) lens[i++] = (u16)val;
                    }
                }
                if (k > total) return (u32)-1;   /* 符号数不能超过码长数 */
                /* 必须有一个 0 结束码（码长 0 对应符号 256）*/
                if (lens[256] == 0) return (u32)-1;

                nsym_lit = (int)hlit;
                nsym_dist = (int)hdist;
                if (build_tree(lens, nsym_lit, &lit, litsyms) != 0) return (u32)-1;
                /* 距离树即使只有 1 个符号也照常建树：1 个符号 => 码长 1、
                 * 码值 0，build_tree / decode_sym 无需任何特判。 */
                if (build_tree(lens + hlit, nsym_dist, &dist, distsyms) != 0)
                    return (u32)-1;
            } else {
                return (u32)-1;   /* btype 11 非法 */
            }

            /* ---- 解码到块结束 ---- */
            for (;;) {
                int sym = decode_sym(&c, &lit, litsyms);
                if (sym < 0) return (u32)-1;
                if (sym < 256) {
                    if (put_out(&c, (u8)sym) != 0) return (u32)-1;
                } else if (sym == 256) {
                    break;          /* 块结束 */
                } else {
                    sym -= 257;
                    if (sym >= 29) return (u32)-1;
                    u32 lbase = len_base[sym];
                    u32 l = lbase;
                    if (len_extra[sym]) {
                        u32 eb;
                        if (need_bits(&c, len_extra[sym], &eb) != 0) return (u32)-1;
                        l += eb;
                    }
                    int dsym = decode_sym(&c, &dist, distsyms);
                    if (dsym < 0) return (u32)-1;
                    u32 dbase = dist_base[dsym];
                    u32 d = dbase;
                    if (dist_extra[dsym]) {
                        u32 eb;
                        if (need_bits(&c, dist_extra[dsym], &eb) != 0) return (u32)-1;
                        d += eb;
                    }
                    if (d > c.dst_pos) return (u32)-1;   /* 距离越界 */
                    const u8 *from = c.dst + (c.dst_pos - d);
                    for (u32 r = 0; r < l; r++) {
                        /* 允许重叠：逐字节拷，源永远落后于目标 */
                        if (put_out(&c, from[r]) != 0) return (u32)-1;
                    }
                }
            }
        }
        if (bfinal) break;
    } while (1);

    return c.dst_pos;
}
