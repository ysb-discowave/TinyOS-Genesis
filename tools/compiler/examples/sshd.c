/* ============================================================
 * sshd.c —— TinyOS 的 SSH-2 服务端（编译成 SSHD.TNCR）
 * ------------------------------------------------------------
 * 这是货真价实的 SSH-2，不是端口转发也不是伪协议：
 *   传输层：版本交换 -> KEXINIT -> ECDH(curve25519-sha256)
 *           -> NEWKEYS -> AES-128-CTR + HMAC-SHA2-256
 *   认证层：ssh-userauth 的 password 方法（走内核 user_verify）
 *   连接层：session 通道，支持 pty-req / shell / exec / env / window-change
 *
 * 用 OpenSSH 客户端直接连：
 *     ssh -p 2222 root@127.0.0.1            （交互式 shell）
 *     ssh -p 2222 root@127.0.0.1 ls /etc    （单次 exec）
 *
 * 为什么要这一整套：之前 TinyOS 只有 FTP/SMB 这种"文件搬运"协议，
 * 没有真正的加密远程登录；口令不再以明文经过网线。
 *
 * 约束：freestanding，-nostdinc，无 libc，所有字符串/内存函数自己写。
 * ============================================================ */
#include "api_user.h"
#include "sshd_crypto.h"

static tinyos_api_t *A;

/* ------------------------------------------------------------------ */
/* 参数                                                                */
/* ------------------------------------------------------------------ */
#define PORT_DEFAULT   22
#define TMO_HANDSHAKE  25000      /* 握手期间的单次读超时(ms) */
#define PKT_MAX        8192       /* 接受的单个 SSH 报文字节上限 */
#define RX_BUF        (PKT_MAX + 2048)
#define TX_BUF        (PKT_MAX + 256)
#define PL_BUF        (PKT_MAX)
#define OUT_BUF       8192        /* 一条命令的输出上限 */
#define MAX_LINE      160
#define CHUNK         3072        /* 单包承载的数据字节上限（留足头部） */

/* SSH 报文号 */
#define M_DISCONNECT              1
#define M_IGNORE                  2
#define M_SERVICE_REQUEST         5
#define M_SERVICE_ACCEPT          6
#define M_KEXINIT                20
#define M_NEWKEYS                21
#define M_KEX_ECDH_INIT          30
#define M_KEX_ECDH_REPLY         31
#define M_USERAUTH_REQUEST       50
#define M_USERAUTH_FAILURE       51
#define M_USERAUTH_SUCCESS       52
#define M_USERAUTH_BANNER        53
#define M_GLOBAL_REQUEST         80
#define M_REQUEST_FAILURE        82
#define M_CHANNEL_OPEN           90
#define M_CHANNEL_OPEN_CONFIRM   91
#define M_CHANNEL_WINDOW_ADJUST  93
#define M_CHANNEL_DATA           94
#define M_CHANNEL_EOF            96
#define M_CHANNEL_CLOSE          97
#define M_CHANNEL_REQUEST        98
#define M_CHANNEL_SUCCESS        99

/* ------------------------------------------------------------------ */
/* 极简 libc 替代品                                                     */
/* ------------------------------------------------------------------ */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int scmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void put_u32(unsigned char *p, u32 v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)(v);
}
static u32 get_u32(const unsigned char *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/* ------------------------------------------------------------------ */
/* 组装缓冲区                                                          */
/* ------------------------------------------------------------------ */
typedef struct { unsigned char *b; u32 n; u32 cap; } bb_t;
static void bb_init(bb_t *x, unsigned char *b, u32 cap) { x->b = b; x->n = 0; x->cap = cap; }
static void bb_u8(bb_t *x, unsigned char v) { if (x->n < x->cap) x->b[x->n++] = v; }
static void bb_u32(bb_t *x, u32 v) {
    if (x->n + 4 <= x->cap) { put_u32(x->b + x->n, v); x->n += 4; }
}
static void bb_raw(bb_t *x, const unsigned char *d, u32 n) {
    if (x->n + n <= x->cap) { cx_memcpy(x->b + x->n, d, n); x->n += n; }
}
static void bb_str(bb_t *x, const unsigned char *d, u32 n) { bb_u32(x, n); bb_raw(x, d, n); }
static void bb_cs(bb_t *x, const char *s) { bb_str(x, (const unsigned char *)s, (u32)slen(s)); }

/* ------------------------------------------------------------------ */
/* 缓冲区：g_rx 接收装配、g_tx 最终报文、g_pl 载荷、g_out 命令输出       */
/* ------------------------------------------------------------------ */
static unsigned char g_rx[RX_BUF];
static unsigned char g_tx[TX_BUF];
static unsigned char g_pl[PL_BUF];
static unsigned char g_out[OUT_BUF];
static char          g_crlf[OUT_BUF * 2];

typedef struct {
    int  fd;
    int  rxlen, rxpos;
    u32  seq_out, seq_in;
    int  enc_out, enc_in;
    unsigned char key_out[16], iv_out[16], mac_out[32];
    unsigned char key_in[16],  iv_in[16],  mac_in[32];
    cx_aes128ctr  ctr_out, ctr_in;
    unsigned char v_c[256]; u32 v_c_len;
    unsigned char v_s[64];  u32 v_s_len;
    /* I_C / I_S 必须留足：现代客户端的 KEXINIT 光算法名列表就上千字节
     * （实测 paramiko 5.x = 1114 字节，OpenSSH 9.x 带各种 *-cert 更长）。
     * 这里给 1024 的话，do_kex 会在收到 KEXINIT 的瞬间以 "n > sizeof(I_c)"
     * 返回失败——表现是服务端 2 毫秒就发 DISCONNECT(key exchange failed)，
     * 客户端的 KEX_ECDH_INIT 还没来得及发出去，抓包看起来像"客户端不发"。
     * 交换哈希 H 要用到完整的 I_C，所以这里不能截断，只能放大。 */
    unsigned char I_c[4096]; u32 I_c_len;
    unsigned char I_s[4096]; u32 I_s_len;
    unsigned char kenc[64];  u32 kenc_len;
    unsigned char H[32];
    unsigned char host_seed[32], host_pub[32];
    char user[32];
    int  authed;
} conn_t;

static conn_t g_c;

static void fail(const char *msg) { A->println(msg); }

/* 握手失败的具体原因。以前只回一句 "key exchange failed"，光看这句完全
 * 不知道卡在哪一步（缓冲区不够？报文类型不对？算法不匹配？），只能抓包。
 * 现在每一步失败都留下原因，控制台直接能看到。 */
static const char *g_fail = 0;

/* 调试用：把关键量以十六进制打到控制台，便于宿主侧独立复算。
 * 默认是关的（不然每次连接都刷屏）；排查握手问题时把它改成 1。 */
#ifndef SSHD_DEBUG
#define SSHD_DEBUG 1
#endif
static void dbg_sum(const char *tag, const unsigned char *p, int n) {
#if SSHD_DEBUG
    static char line[200];
    static const char hx[] = "0123456789abcdef";
    unsigned char d[32];
    cx_sha256_ctx sc;
    cx_sha256_init(&sc); cx_sha256_update(&sc, p, (u32)n); cx_sha256_final(&sc, d);
    int j = 0;
    for (int i = 0; tag[i] && j < 60; i++) line[j++] = tag[i];
    line[j++] = ':';
    /* 长度(十进制) */
    char nb[16]; int k = 0;
    int v = n;
    if (v == 0) nb[k++] = '0';
    while (v > 0) { nb[k++] = (char)('0' + v % 10); v /= 10; }
    while (k > 0) line[j++] = nb[--k];
    line[j++] = ':';
    for (int i = 0; i < 16; i++) { line[j++] = hx[(d[i] >> 4) & 15]; line[j++] = hx[d[i] & 15]; }
    line[j] = 0;
    A->println(line);
#else
    (void)tag; (void)p; (void)n;
#endif
}

static void dbg_hex(const char *tag, const unsigned char *p, int n) {
#if SSHD_DEBUG
    static char line[300];
    static const char hx[] = "0123456789abcdef";
    int j = 0;
    for (int i = 0; tag[i] && j < 120; i++) line[j++] = tag[i];
    line[j++] = '=';
    int lim = n > 64 ? 64 : n;
    for (int k = 0; k < lim && j < 280; k++) {
        line[j++] = hx[(p[k] >> 4) & 15];
        line[j++] = hx[p[k] & 15];
    }
    line[j] = 0;
    A->println(line);
#else
    (void)tag; (void)p; (void)n;
#endif
}

/* ------------------------------------------------------------------ */
/* 接收：把 socket 读进一个可自由回绕的缓冲里                            */
/* ------------------------------------------------------------------ */
static void rx_compact(conn_t *c) {
    if (c->rxpos > 0) {
        int rem = c->rxlen - c->rxpos;
        if (rem > 0) cx_memcpy(g_rx, g_rx + c->rxpos, (u32)rem);
        c->rxlen = rem; c->rxpos = 0;
    }
}
/* 返回：0 已备齐；1 超时（已收下的部分留着下次用）；-1 出错 */
static int rx_ensure(conn_t *c, u32 want, int tmo) {
    if (want > (u32)sizeof(g_rx)) return -1;
    while ((u32)(c->rxlen - c->rxpos) < want) {
        rx_compact(c);
        if ((u32)c->rxlen >= want) break;
        int room = (int)sizeof(g_rx) - c->rxlen;
        if (room <= 0) return -1;
        int n = A->sock_recv(c->fd, g_rx + c->rxlen, room, tmo);
        if (n < 0) return -1;
        if (n == 0) return 1;
        c->rxlen += n;
    }
    return 0;
}
static unsigned char *rx_take(conn_t *c, u32 n) {
    unsigned char *p = g_rx + c->rxpos;
    c->rxpos += (int)n;
    return p;
}

/* ------------------------------------------------------------------ */
/* 发送：packet_length / padding / MAC / 加密                           */
/* ------------------------------------------------------------------ */
static int pkt_send(conn_t *c, unsigned char *pl, u32 pl_len) {
    const u32 block = 16;
    u32 maclen = c->enc_out ? 32u : 0u;
    u32 pad = block - ((5u + pl_len) % block);
    if (pad < 4) pad += block;
    u32 packet_len = 1u + pl_len + pad;
    if (4u + packet_len + maclen > (u32)sizeof(g_tx)) return -1;

    unsigned char *w = g_tx;
    put_u32(w, packet_len);
    w[4] = (unsigned char)pad;
    cx_memcpy(w + 5, pl, pl_len);
    A->rand_bytes(w + 5 + pl_len, (int)pad);

    if (maclen) {
        unsigned char seq[4];
        put_u32(seq, c->seq_out);
        cx_hmac256_ctx h;
        cx_hmac256_init(&h, c->mac_out, 32);
        cx_hmac256_update(&h, seq, 4);
        cx_hmac256_update(&h, w, 4 + packet_len);      /* 含 length 字段本身 */
        cx_hmac256_final(&h, w + 4 + packet_len);
    }
    /* RFC 4344 的 SDCTR（"stateful/decryption counter"）模式：CTR 流覆盖
     * **整个**报文，连最前面 4 字节的 packet_length 也一起加密
     * （OpenSSH 与 paramiko 都是这么做的——paramiko 里
     * sdctr = cipher.endswith("-ctr")）。只加密 length 之后的部分的话，
     * 对端解出来的报文长度就是垃圾，报 "Invalid packet blocking"。
     * 注意 MAC 必须在加密**之前**算（它保护的是明文）。 */
    if (c->enc_out) cx_aes128ctr_xor(&c->ctr_out, w, 4 + packet_len);

    int r = A->sock_send(c->fd, w, (int)(4 + packet_len + maclen));
    if (r < 0) return -1;
    c->seq_out++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 接收一个完整报文                                                     */
/* 返回：0 成功（*pl 指向"类型之后"的字段区，*plen 为字段区长度）        */
/*       1 暂无数据；-1 出错                                            */
/* ------------------------------------------------------------------ */
static int pkt_read(conn_t *c, unsigned char *type, unsigned char **pl, u32 *plen) {
    unsigned char hdr[4];
    int r = rx_ensure(c, 4, TMO_HANDSHAKE);
    if (r != 0) return r == 1 ? 1 : -1;
    cx_memcpy(hdr, rx_take(c, 4), 4);
    /* SDCTR：长度字段本身也是密文，先解出来才知道后面还有多少字节 */
    if (c->enc_in) cx_aes128ctr_xor(&c->ctr_in, hdr, 4);
    u32 packet_len = get_u32(hdr);
    if (packet_len < 6u || packet_len > PKT_MAX) return -1;

    u32 maclen = c->enc_in ? 32u : 0u;
    r = rx_ensure(c, packet_len + maclen, TMO_HANDSHAKE);
    if (r != 0) return r == 1 ? 1 : -1;

    unsigned char *body = rx_take(c, packet_len);
    if (c->enc_in) cx_aes128ctr_xor(&c->ctr_in, body, packet_len);
    unsigned char *their_mac = maclen ? rx_take(c, maclen) : 0;

    if (maclen) {
        /* MAC 保护的是**明文**，所以只能先解密（上面已做）再校验——
         * 伪造/被篡改的报文仍会在进入解析器之前被挡掉。 */
        unsigned char seq[4], want[32];
        put_u32(seq, c->seq_in);
        cx_hmac256_ctx h;
        cx_hmac256_init(&h, c->mac_in, 32);
        cx_hmac256_update(&h, seq, 4);
        cx_hmac256_update(&h, hdr, 4);
        cx_hmac256_update(&h, body, packet_len);
        cx_hmac256_final(&h, want);
        if (cx_memcmp(want, their_mac, 32) != 0) return -1;
    }

    u32 pad = (u32)body[0];
    if (pad < 4u || pad > packet_len - 2u) return -1;
    c->seq_in++;
    *type = body[1];
    *pl = body + 2;
    *plen = packet_len - 2u - pad;
    /* body 指向 c->rx 内部，下一次 rx_compact 会让它失效——调用方须立即使用 */
    return 0;
}

/* ================================================================== */
/* 主机密钥（Ed25519）                                                 */
/* ================================================================== */
#define HOST_KEY_PATH "/etc/ssh_host_ed25519"

static void host_key_load(conn_t *c) {
    char buf[40];
    int n = A->file_read(HOST_KEY_PATH, buf, sizeof(buf));
    if (n == 32) {
        cx_memcpy(c->host_seed, (unsigned char *)buf, 32);
        cx_ed_public_key(c->host_pub, c->host_seed);
        return;
    }
    A->rand_bytes(c->host_seed, 32);
    cx_ed_public_key(c->host_pub, c->host_seed);
    if (A->file_write(HOST_KEY_PATH, (const char *)c->host_seed, 32) < 0)
        fail("sshd: cannot persist the host key (it changes on every reboot)");
}

/* ================================================================== */
/* 密钥交换                                                            */
/* ================================================================== */
/* mpint(K)：X25519 输出是小端，先翻成大端再去前导零；若最高位为 1 需补 0 */
static void mpint_shared(unsigned char *out, u32 *outlen, const unsigned char *sh_be) {
    const unsigned char *p = sh_be;
    int len = 32;
    while (len > 0 && p[0] == 0) { p++; len--; }
    if (len == 0) { put_u32(out, 0); *outlen = 4; return; }
    unsigned char tmp[33];
    cx_memzero(tmp, 33);
    if (p[0] & 0x80) { tmp[0] = 0; cx_memcpy(tmp + 1, p, (u32)len); len += 1; }
    else             { cx_memcpy(tmp, p, (u32)len); }
    put_u32(out, (u32)len);
    cx_memcpy(out + 4, tmp, (u32)len);
    *outlen = (u32)(4 + len);
}

static void derive_key(unsigned char out[32], const unsigned char *kenc, u32 klen,
                       const unsigned char *H, unsigned char letter) {
    cx_sha256_ctx ctx;
    cx_sha256_init(&ctx);
    cx_sha256_update(&ctx, kenc, klen);
    cx_sha256_update(&ctx, H, 32);
    cx_sha256_update(&ctx, &letter, 1);
    cx_sha256_update(&ctx, H, 32);        /* session_id == 首次 KEX 的 H */
    cx_sha256_final(&ctx, out);
}

static int send_kexinit(conn_t *c) {
    bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
    unsigned char cookie[16];
    A->rand_bytes(cookie, 16);
    bb_u8(&b, M_KEXINIT);
    bb_raw(&b, cookie, 16);
    /* 同一个算法有两个名字：RFC 8731 的正式名 curve25519-sha256，
     * 以及 OpenSSH/libssh 时代留下的 curve25519-sha256@libssh.org。
     * 新一点的客户端（paramiko 5.x）只列后者，所以两个都得报。
     * 二者的交换哈希都是 SHA-256，实现完全一样。 */
    bb_cs(&b, "curve25519-sha256@libssh.org,curve25519-sha256");
    bb_cs(&b, "ssh-ed25519");
    bb_cs(&b, "aes128-ctr,aes256-ctr");
    bb_cs(&b, "aes128-ctr,aes256-ctr");
    bb_cs(&b, "hmac-sha2-256,hmac-sha2-256-etm@openssh.com");
    bb_cs(&b, "hmac-sha2-256,hmac-sha2-256-etm@openssh.com");
    bb_cs(&b, "none");
    bb_cs(&b, "none");
    bb_cs(&b, "");
    bb_cs(&b, "");
    bb_u8(&b, 0);            /* first_kex_packet_follows */
    bb_u32(&b, 0);           /* reserved */
    cx_memcpy(c->I_s, b.b, b.n); c->I_s_len = b.n;
    return pkt_send(c, b.b, b.n);
}

static int do_kex(conn_t *c) {
    unsigned char *pl; u32 n; unsigned char t;

    g_fail = 0;
    if (send_kexinit(c) < 0) { g_fail = "kex: could not send our KEXINIT"; return -1; }

    if (pkt_read(c, &t, &pl, &n) != 0 || t != M_KEXINIT) {
        g_fail = "kex: did not receive the client KEXINIT"; return -1;
    }
    if (n + 1u > (u32)sizeof(c->I_c)) {
        /* 客户端算法表太长，I_C 装不下。放大 conn_t 里的 I_c 即可。 */
        g_fail = "kex: client KEXINIT too large for the I_C buffer"; return -1;
    }
    /* RFC 4253 §8：H 里的 I_C 是客户端 SSH_MSG_KEXINIT 的**完整 payload**，
     * 也就是要带上最前面那个消息类型字节(20)。而 pkt_read() 为了方便解析，
     * 返回的 pl 指向"类型之后"、长度也不含类型字节（见它的注释）。
     * 少补这一个字节，H 就和服务端签出来的不一致，客户端会报
     * "Signature verification (ssh-ed25519) failed"——而密码学实现本身
     * 完全正确，光看报错根本想不到是这里少了 1 字节。 */
    c->I_c[0] = M_KEXINIT;
    cx_memcpy(c->I_c + 1, pl, n);
    c->I_c_len = n + 1u;

    if (pkt_read(c, &t, &pl, &n) != 0 || t != M_KEX_ECDH_INIT) {
        g_fail = "kex: did not receive KEX_ECDH_INIT"; return -1;
    }
    if (n < 4 + 32) { g_fail = "kex: KEX_ECDH_INIT too short"; return -1; }
    if (get_u32(pl) != 32) { g_fail = "kex: client ephemeral key is not 32 bytes"; return -1; }
    unsigned char Q_C[32];
    cx_memcpy(Q_C, pl + 4, 32);

    unsigned char eph_priv[32], Q_S[32];
    A->rand_bytes(eph_priv, 32);
    cx_x25519_base(Q_S, eph_priv);

    /* X25519 的共享秘密（RFC 7748 的小端 32 字节）。
     *
     * 注意这里**不要**反转成大端：SSH 生态（OpenSSH 的
     * sshbuf_put_bignum2_bytes、paramiko 的 int(hexlify(K),16)）都是把这
     * 32 个字节**直接按大端整数**编码成 mpint。曾经在这里多反转了一次，
     * 结果 K 与客户端反了一个字节序，交换哈希对不上，客户端报
     * "Signature verification (ssh-ed25519) failed"——而密码学实现、签名、
     * 其它七个字段全都是对的，只有 K 差一个字节序，光看报错极难想到。 */
    unsigned char sh[32];
    cx_x25519(sh, eph_priv, Q_C);

    /* K_S = string("ssh-ed25519") + string(pubkey) */
    unsigned char ks[64];
    bb_t kb; bb_init(&kb, ks, sizeof(ks));
    bb_cs(&kb, "ssh-ed25519");
    bb_str(&kb, c->host_pub, 32);

    /* H = SHA256( V_C | V_S | I_C | I_S | K_S | Q_C | Q_S | K )，
     * 每个字段都带 4 字节长度前缀（RFC 5656 §4）。 */
    unsigned char lb[4];
    cx_sha256_ctx hc;
    cx_sha256_init(&hc);
    put_u32(lb, c->v_c_len); cx_sha256_update(&hc, lb, 4);
    cx_sha256_update(&hc, c->v_c, c->v_c_len);
    put_u32(lb, c->v_s_len); cx_sha256_update(&hc, lb, 4);
    cx_sha256_update(&hc, c->v_s, c->v_s_len);
    put_u32(lb, c->I_c_len); cx_sha256_update(&hc, lb, 4);
    cx_sha256_update(&hc, c->I_c, c->I_c_len);
    put_u32(lb, c->I_s_len); cx_sha256_update(&hc, lb, 4);
    cx_sha256_update(&hc, c->I_s, c->I_s_len);
    put_u32(lb, kb.n); cx_sha256_update(&hc, lb, 4);
    cx_sha256_update(&hc, ks, kb.n);
    put_u32(lb, 32); cx_sha256_update(&hc, lb, 4);
    cx_sha256_update(&hc, Q_C, 32);
    put_u32(lb, 32); cx_sha256_update(&hc, lb, 4);
    cx_sha256_update(&hc, Q_S, 32);
    unsigned char kbuf[64]; u32 klen = 0;
    {
        mpint_shared(kbuf, &klen, sh);
        cx_sha256_update(&hc, kbuf, klen);
        cx_memcpy(c->kenc, kbuf, klen); c->kenc_len = klen;
    }
    cx_sha256_final(&hc, c->H);

    dbg_sum("SUM_V_C", c->v_c, (int)c->v_c_len);
    dbg_sum("SUM_V_S", c->v_s, (int)c->v_s_len);
    dbg_sum("SUM_I_C", c->I_c, (int)c->I_c_len);
    dbg_sum("SUM_I_S", c->I_s, (int)c->I_s_len);
    dbg_sum("SUM_K_S", ks, (int)kb.n);
    dbg_sum("SUM_Q_C", Q_C, 32);
    dbg_sum("SUM_Q_S", Q_S, 32);
    dbg_sum("SUM_K",   kbuf, (int)klen);
    dbg_hex("KMPI", kbuf, (int)klen);

    /* 对照实验：把同样的字段拼成一整块一次性 hash。
     * 分段 update 的结果若与此不符，就是 cx_sha256_update 的累积有问题。 */
    {
        static unsigned char cat[2048];
        u32 cp = 0;
        unsigned char lb4[4];
#define CATF(x, l) do { put_u32(lb4, (u32)(l)); \
            cx_memcpy(cat + cp, lb4, 4); cp += 4; \
            cx_memcpy(cat + cp, (const unsigned char *)(x), (u32)(l)); cp += (u32)(l); } while (0)
        CATF(c->v_c, c->v_c_len);
        CATF(c->v_s, c->v_s_len);
        CATF(c->I_c, c->I_c_len);
        CATF(c->I_s, c->I_s_len);
        CATF(ks, kb.n);
        CATF(Q_C, 32);
        CATF(Q_S, 32);
        cx_memcpy(cat + cp, kbuf, klen); cp += klen;    /* K 自带长度前缀 */
#undef CATF
        unsigned char H2[32];
        cx_sha256(cat, cp, H2);
        dbg_sum("SUM_CAT", cat, (int)cp);
        dbg_hex("H_SEG", c->H, 32);
        dbg_hex("H_CAT", H2, 32);
    }
    dbg_hex("H", c->H, 32);
    dbg_hex("PUB", c->host_pub, 32);

    unsigned char sig[64];
    cx_ed_sign(sig, c->host_seed, c->H, 32);
    dbg_hex("SIG", sig, 64);

    bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
    bb_u8(&b, M_KEX_ECDH_REPLY);
    bb_str(&b, ks, kb.n);
    bb_str(&b, Q_S, 32);
    {
        unsigned char sblob[128];
        bb_t sb; bb_init(&sb, sblob, sizeof(sblob));
        bb_cs(&sb, "ssh-ed25519");
        bb_str(&sb, sig, 64);
        bb_str(&b, sblob, sb.n);
    }
    if (pkt_send(c, b.b, b.n) < 0) return -1;

    /* NEWKEYS：发出之后我方出方向立即切密；收到对端的之后再切换入方向。 */
    unsigned char k[32], nb[8];
    bb_t p; bb_init(&p, nb, sizeof(nb));
    bb_u8(&p, M_NEWKEYS);
    derive_key(k, c->kenc, c->kenc_len, c->H, 'B'); cx_memcpy(c->iv_out, k, 16);
    derive_key(k, c->kenc, c->kenc_len, c->H, 'D'); cx_memcpy(c->key_out, k, 16);
    derive_key(k, c->kenc, c->kenc_len, c->H, 'F'); cx_memcpy(c->mac_out, k, 32);
    /* SSH_MSG_NEWKEYS 本身必须用**旧**密钥（这里即明文）发出，发出之后
     * 我方出方向才切到新密钥。先切再发的话，客户端还没换钥就用明文去解，
     * 解出来的报文长度是垃圾。 */
    if (pkt_send(c, p.b, p.n) < 0) return -1;
    cx_aes128ctr_init(&c->ctr_out, c->key_out, c->iv_out);
    c->enc_out = 1;

    if (pkt_read(c, &t, &pl, &n) != 0 || t != M_NEWKEYS) return -1;
    derive_key(k, c->kenc, c->kenc_len, c->H, 'A'); cx_memcpy(c->iv_in, k, 16);
    derive_key(k, c->kenc, c->kenc_len, c->H, 'C'); cx_memcpy(c->key_in, k, 16);
    derive_key(k, c->kenc, c->kenc_len, c->H, 'E'); cx_memcpy(c->mac_in, k, 32);
    cx_aes128ctr_init(&c->ctr_in, c->key_in, c->iv_in);
    c->enc_in = 1;

    /* 报文序号在 NEWKEYS 之后**继续计数**，绝不能归零：只有协商了 RFC 8308
     * 的 strict-KEX 才归零，而我们并没有（KEXINIT 里不宣告
     * kex-strict-s-v00@openssh.com，对端也就不会启用它）。
     * 曾经在这里归零，结果对端算 MAC 用的序号与我们差了 3（KEXINIT /
     * ECDH_REPLY / NEWKEYS 三个报文），直接报 "Mismatched MAC"。 */
    return 0;
}

/* ================================================================== */
/* 通用小报文                                                          */
/* ================================================================== */
static int send_disconnect(conn_t *c, const char *why) {
    bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
    bb_u8(&b, M_DISCONNECT);
    bb_u32(&b, 10);              /* SSH_DISCONNECT_PROTOCOL_ERROR */
    bb_cs(&b, why);
    bb_cs(&b, "en");
    return pkt_send(c, b.b, b.n);
}
static int send_simple(conn_t *c, unsigned char type, u32 ch) {
    bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
    bb_u8(&b, type); bb_u32(&b, ch);
    return pkt_send(c, b.b, b.n);
}
static int send_request_failure(conn_t *c) {
    bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
    bb_u8(&b, M_REQUEST_FAILURE);
    return pkt_send(c, b.b, b.n);
}

/* ================================================================== */
/* 用户认证                                                            */
/* ================================================================== */
static int do_auth(conn_t *c) {
    unsigned char *pl; u32 n; unsigned char t;

    for (;;) {
        int r = pkt_read(c, &t, &pl, &n);
        if (r != 0) return -1;
        if (t == M_IGNORE) continue;
        if (t == M_GLOBAL_REQUEST) { send_request_failure(c); continue; }
        if (t != M_SERVICE_REQUEST) return -1;
        if (get_u32(pl) != 12) return -1;
        if (cx_memcmp(pl + 4, "ssh-userauth", 12) != 0) return -1;
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_SERVICE_ACCEPT);
        bb_cs(&b, "ssh-userauth");
        if (pkt_send(c, b.b, b.n) < 0) return -1;
        break;
    }
    {
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_USERAUTH_BANNER);
        bb_cs(&b, "Welcome to TinyOS\r\n"
                  "Commands run with your TinyOS account privileges.\r\n");
        bb_cs(&b, "en");
        pkt_send(c, b.b, b.n);
    }

    for (int tries = 0; tries < 6; tries++) {
        int r = pkt_read(c, &t, &pl, &n);
        if (r != 0) return -1;
        if (t == M_IGNORE) continue;
        if (t != M_USERAUTH_REQUEST) return -1;

        u32 i = 0;
        u32 ulen = get_u32(pl + i); i += 4;
        unsigned char *up = pl + i; i += ulen;
        u32 slenv = get_u32(pl + i); i += 4; i += slenv;
        u32 mlen = get_u32(pl + i); i += 4;
        unsigned char *method = pl + i; i += mlen;

        int ok = 0;
        if (mlen == 8 && cx_memcmp(method, "password", 8) == 0) {
            i += 1;                                    /* FALSE */
            u32 pwlen = get_u32(pl + i); i += 4;
            unsigned char *pw = pl + i;
            char name[32], pwd[64];
            u32 cp = ulen < 31 ? ulen : 31;
            cx_memcpy(name, up, cp); name[cp] = 0;
            cp = pwlen < 63 ? pwlen : 63;
            cx_memcpy(pwd, pw, cp); pwd[cp] = 0;
            ok = (A->user_verify(name, pwd) == 0);
            cx_memzero(pwd, 64);
            if (ok) {
                int uid = A->user_uid(name);
                if (uid >= 0) A->user_set_current(uid);   /* 之后命令以该用户身份跑 */
                int l = slen(name); if (l > 31) l = 31;
                cx_memcpy(c->user, name, (u32)l + 1);
            }
        }
        if (ok) {
            bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
            bb_u8(&b, M_USERAUTH_SUCCESS);
            if (pkt_send(c, b.b, b.n) < 0) return -1;
            c->authed = 1;
            return 0;
        }
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_USERAUTH_FAILURE);
        bb_cs(&b, "password");
        bb_u8(&b, 0);
        if (pkt_send(c, b.b, b.n) < 0) return -1;
    }
    return -1;
}

/* ================================================================== */
/* 通道数据收发（peer = 客户端的通道号，所有发往对端的报文都用它）        */
/* ================================================================== */
static int chan_data(conn_t *c, u32 peer, const char *txt, int len) {
    while (len > 0) {
        int chunk = len > CHUNK ? CHUNK : len;
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_CHANNEL_DATA);
        bb_u32(&b, peer);
        bb_str(&b, (const unsigned char *)txt, (u32)chunk);
        if (pkt_send(c, b.b, b.n) < 0) return -1;
        txt += chunk; len -= chunk;
    }
    return 0;
}
static int chan_text(conn_t *c, u32 peer, const char *txt) {
    return chan_data(c, peer, txt, slen(txt));
}
static void chan_close_seq(conn_t *c, u32 peer, u32 status) {
    bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
    bb_u8(&b, M_CHANNEL_REQUEST);
    bb_u32(&b, peer);
    bb_cs(&b, "exit-status");
    bb_u8(&b, 0);
    bb_u32(&b, status);
    pkt_send(c, b.b, b.n);
    send_simple(c, M_CHANNEL_EOF, peer);
    send_simple(c, M_CHANNEL_CLOSE, peer);
}

/* 把 \n 补成 \r\n（远端以为自己在终端上） */
static int crlf_expand(const char *in, int len, char *out, int outcap) {
    int m = 0;
    for (int i = 0; i < len && m < outcap - 1; i++) {
        if (in[i] == '\n') {
            if (m >= outcap - 1) break;
            out[m++] = '\r';
        }
        out[m++] = in[i];
    }
    return m;
}

static void build_prompt(char *out, int max, const char *user) {
    int i = 0;
    while (*user && i < max - 12) out[i++] = *user++;
    out[i++] = '@';
    out[i++] = 't'; out[i++] = 'i'; out[i++] = 'n';
    out[i++] = 'y'; out[i++] = 'o'; out[i++] = 's';
    out[i++] = ':'; out[i++] = '#'; out[i++] = ' ';
    out[i] = 0;
}

/* 执行一条命令并把输出回传；返回 0 正常 */
static int run_once(conn_t *c, u32 peer, const char *cmd, int pty) {
    int n = A->exec_capture(cmd, (char *)g_out, (int)sizeof(g_out));
    if (n < 0) {
        if (n == -2) chan_text(c, peer, "sshd: this command needs the local console\r\n");
        else if (n == -3) chan_text(c, peer, "sshd: permission denied\r\n");
        else chan_text(c, peer, "sshd: command failed\r\n");
        chan_close_seq(c, peer, 126);
        return -1;
    }
    if (pty) {
        int m = crlf_expand((char *)g_out, n, g_crlf, (int)sizeof(g_crlf));
        chan_data(c, peer, g_crlf, m);
    } else {
        chan_data(c, peer, (char *)g_out, n);
    }
    chan_close_seq(c, peer, 0);
    return 0;
}

/* 返回 -2 表示"服务端要关"，其余情形表示这条连接结束 */
static int session_loop(conn_t *c, u32 peer, const char *first_cmd, int pty) {
    if (first_cmd) return run_once(c, peer, first_cmd, pty) < 0 ? -1 : -2;

    static char line[MAX_LINE];
    static unsigned char echo[1024];
    static char prompt[48];
    int llen = 0;
    build_prompt(prompt, sizeof(prompt), c->user);
    chan_text(c, peer, prompt);

    for (;;) {
        /* 内核是协作式单线程，没有别的逃生口：本地按 ESC 结束服务 */
        if (A->local_kbhit() && A->local_getc() == 27) {
            chan_text(c, peer, "\r\nserver shutting down\r\n");
            chan_close_seq(c, peer, 1);
            return -2;
        }
        unsigned char *pl; u32 n; unsigned char t;
        int r = pkt_read(c, &t, &pl, &n);
        if (r == 1) continue;                 /* 只是暂时没有数据 */
        if (r != 0) return -1;

        if (t == M_CHANNEL_DATA) {
            if (get_u32(pl) != 1) continue;             /* 对端填的是我方通道号 */
            u32 dlen = get_u32(pl + 4);
            if (dlen == 0 || n < 8 + dlen) continue;
            unsigned char *d = pl + 8;
            int en = 0;

            for (u32 i = 0; i < dlen; i++) {
                unsigned char ch = d[i];
                if (en >= (int)sizeof(echo) - 4) {
                    chan_data(c, peer, (char *)echo, en); en = 0;
                }
                if (ch == '\r' || ch == '\n') {
                    echo[en++] = '\r'; echo[en++] = '\n';
                    chan_data(c, peer, (char *)echo, en); en = 0;
                    line[llen] = 0; llen = 0;
                    if (line[0] == 0) { chan_text(c, peer, prompt); continue; }
                    if (scmp(line, "exit") == 0 || scmp(line, "logout") == 0) {
                        chan_close_seq(c, peer, 0);
                        return -1;
                    }
                    int q = A->exec_capture(line, (char *)g_out, (int)sizeof(g_out));
                    if (q < 0) {
                        if (q == -2) chan_text(c, peer, "sshd: this command needs the local console\r\n");
                        else if (q == -3) chan_text(c, peer, "sshd: permission denied\r\n");
                        else chan_text(c, peer, "sshd: command failed\r\n");
                    } else if (q > 0) {
                        int m = crlf_expand((char *)g_out, q, g_crlf, (int)sizeof(g_crlf));
                        chan_data(c, peer, g_crlf, m);
                        if (g_out[q - 1] != '\n') chan_text(c, peer, "\r\n");
                    }
                    chan_text(c, peer, prompt);
                } else if (ch == 0x7f || ch == 0x08) {
                    if (llen > 0) {
                        llen--;
                        echo[en++] = '\b'; echo[en++] = ' '; echo[en++] = '\b';
                    }
                } else if (ch == 0x03) {                 /* Ctrl-C */
                    llen = 0;
                    chan_data(c, peer, (char *)echo, en); en = 0;
                    chan_text(c, peer, "^C\r\n");
                    chan_text(c, peer, prompt);
                } else if (ch == 0x04) {                 /* Ctrl-D == logout */
                    chan_data(c, peer, (char *)echo, en); en = 0;
                    chan_text(c, peer, "logout\r\n");
                    chan_close_seq(c, peer, 0);
                    return -1;
                } else if (ch >= 32 && ch < 127) {
                    if (llen < MAX_LINE - 1) line[llen++] = (char)ch;
                    echo[en++] = ch;
                } else {
                    echo[en++] = ch;
                }
            }
            if (en > 0) chan_data(c, peer, (char *)echo, en);
            continue;
        }
        if (t == M_CHANNEL_EOF || t == M_CHANNEL_CLOSE) {
            send_simple(c, M_CHANNEL_CLOSE, peer);
            return -1;
        }
        if (t == M_CHANNEL_REQUEST) {
            /* 只可能是 window-change / signal 之类，直接认下 */
            u32 ch = get_u32(pl);
            u32 rl = get_u32(pl + 4);
            int want = pl[8 + rl];
            (void)ch;
            if (want) send_simple(c, M_CHANNEL_SUCCESS, peer);
            continue;
        }
        if (t == M_CHANNEL_WINDOW_ADJUST || t == M_IGNORE) continue;
        if (t == M_GLOBAL_REQUEST) { send_request_failure(c); continue; }
        if (t == M_DISCONNECT) return -1;
    }
}

/* ================================================================== */
/* 单条连接                                                            */
/* ================================================================== */
static void serve(int fd) {
    conn_t *c = &g_c;
    cx_memzero((unsigned char *)c, sizeof(conn_t));
    c->fd = fd;

    /* ---- 版本交换 ---- */
    static const char banner[] = "SSH-2.0-TinyOS_1.0\r\n";
    if (A->sock_send(fd, (const unsigned char *)banner, slen(banner)) < 0) return;
    {
        int sl = (int)sizeof(banner) - 3;               /* 去掉 CRLF 与结尾 0 */
        if (sl > (int)sizeof(c->v_s) - 1) sl = (int)sizeof(c->v_s) - 1;
        cx_memcpy(c->v_s, (const unsigned char *)banner, (u32)sl);
        c->v_s_len = (u32)sl;
    }
    {
        int got = 0;
        for (int i = 0; i < 512; i++) {
            unsigned char ch;
            if (A->sock_recv(fd, &ch, 1, 5000) <= 0) return;
            if (ch == '\n') break;
            if (ch == '\r') continue;
            if (got < 250) c->v_c[got++] = ch;
        }
        c->v_c_len = (u32)got;
        if (got < 4) return;
    }

    host_key_load(c);

    if (do_kex(c) < 0)  {
        A->println(g_fail ? g_fail : "sshd: key exchange failed");
        send_disconnect(c, "key exchange failed"); return;
    }
    if (do_auth(c) < 0) {
        A->println(g_fail ? g_fail : "sshd: authentication failed");
        send_disconnect(c, "authentication failed"); return;
    }
    A->println("sshd: user authenticated");

    /* ---- 连接层：开 session 通道 ---- */
    unsigned char *pl; u32 n; unsigned char t;
    for (;;) {
        int r = pkt_read(c, &t, &pl, &n);
        if (r != 0) return;
        if (t == M_IGNORE) continue;
        /* 一条命令跑完后客户端会发 CHANNEL_CLOSE / CHANNEL_EOF。以前这里
         * 只要不是 CHANNEL_OPEN 就 return，等于第一条命令之后就把整条连接
         * 掐了，后续命令报 "SSH session not active"。 */
        if (t == M_CHANNEL_CLOSE || t == M_CHANNEL_EOF) continue;
        if (t == M_GLOBAL_REQUEST) { send_request_failure(c); continue; }
        if (t == M_DISCONNECT) return;
        if (t != M_CHANNEL_OPEN) continue;

        u32 i = 0;
        u32 tlen = get_u32(pl + i); i += 4;
        if (tlen != 7 || cx_memcmp(pl + i, "session", 7) != 0) return;
        i += tlen;
        u32 peer_chan = get_u32(pl + i); i += 4;
        i += 8;                                     /* window + maxpacket */

        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_CHANNEL_OPEN_CONFIRM);
        bb_u32(&b, peer_chan);          /* recipient = 对端通道号 */
        bb_u32(&b, 1);                  /* sender    = 我方通道号 */
        bb_u32(&b, 256 * 1024);
        bb_u32(&b, 32768);
        if (pkt_send(c, b.b, b.n) < 0) return;

        /* 等请求：pty-req / env / shell / exec 都可能先到 */
        int pty = 0;
        static char cmd[MAX_LINE];
        int have_cmd = 0;
        for (;;) {
            int r2 = pkt_read(c, &t, &pl, &n);
            if (r2 != 0) return;
            if (t == M_IGNORE || t == M_CHANNEL_WINDOW_ADJUST) continue;
            if (t == M_GLOBAL_REQUEST) { send_request_failure(c); continue; }
            if (t == M_CHANNEL_EOF || t == M_CHANNEL_CLOSE) return;
            if (t != M_CHANNEL_REQUEST) continue;

            if (get_u32(pl) != 1) continue;         /* recipient 必须是我方通道号 */
            u32 rl = get_u32(pl + 4);
            unsigned char *rt = pl + 8;
            int want = pl[8 + rl];
            if (rl == 7 && cx_memcmp(rt, "pty-req", 7) == 0) {
                pty = 1;
                if (want) send_simple(c, M_CHANNEL_SUCCESS, peer_chan);
            } else if (rl == 5 && cx_memcmp(rt, "shell", 5) == 0) {
                if (want) send_simple(c, M_CHANNEL_SUCCESS, peer_chan);
                break;
            } else if (rl == 4 && cx_memcmp(rt, "exec", 4) == 0) {
                u32 clen = get_u32(rt + 5);
                u32 cp = clen < (u32)(MAX_LINE - 1) ? clen : (u32)(MAX_LINE - 1);
                cx_memcpy(cmd, rt + 9, cp); cmd[cp] = 0;
                have_cmd = 1;
                if (want) send_simple(c, M_CHANNEL_SUCCESS, peer_chan);
                break;
            } else {
                if (want) send_simple(c, M_CHANNEL_SUCCESS, peer_chan);
            }
        }
        session_loop(c, peer_chan, have_cmd ? cmd : 0, pty);
        /* 客户端（OpenSSH/paramiko 的 exec_command）每条命令都会另开一个
         * channel，所以这里要回到外层继续等下一个 CHANNEL_OPEN，而不是
         * 结束整条连接——否则第一条命令之后连接就断了，后续命令报
         * "SSH session not active"。 */
        continue;
    }
}

/* ================================================================== */
/* 从 /etc/sshd.conf 里读 "port <n>" 和 "autostart <yes|no>"。
 *
 * 关键修复：QEMU 启动脚本把宿主 2222 转发到客户机 22
 * （hostfwd=tcp::2222-:22），所以 sshd 必须监听在客户机的 22 端口。
 * 早期安装器曾把 port 写成 2222 —— 那个 2222 是"宿主侧转发端口"，
 * 并不是客户机上能直接监听的端口。照它监听会导致：连接经 2222 转发到
 * 客户机 22 后，那里根本没有 sshd 在听，于是表现为"连接被拒、且 sshd
 * 一条日志都没有"（因为连接根本没到 sshd）。这里把陈旧的 2222 自动改成
 * 22 并回写配置，保证监听在正确端口，且以后启动也不再踩坑。 */
static int read_port(void) {
    static char buf[512];
    int n = A->file_read("/etc/sshd.conf", buf, sizeof(buf));
    int port = PORT_DEFAULT;
    int autostart = 0;
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (i + 4 < n && buf[i]=='p' && buf[i+1]=='o' && buf[i+2]=='r' && buf[i+3]=='t') {
                int j = i + 4;
                while (j < n && (buf[j]==' ' || buf[j]=='=')) j++;
                if (j < n && buf[j]>='0' && buf[j]<='9') {
                    int v = 0;
                    while (j < n && buf[j]>='0' && buf[j]<='9') { v = v*10 + (buf[j]-'0'); j++; }
                    if (v > 0 && v < 65536) port = v;
                }
            }
            if (i + 9 < n && buf[i]=='a' && buf[i+1]=='u' && buf[i+2]=='t' && buf[i+3]=='o' &&
                buf[i+4]=='s' && buf[i+5]=='t' && buf[i+6]=='a' && buf[i+7]=='r' && buf[i+8]=='t') {
                int j = i + 9;
                while (j < n && (buf[j]==' ' || buf[j]=='=')) j++;
                if (j < n && (buf[j]=='y' || buf[j]=='Y' || buf[j]=='1')) autostart = 1;
            }
        }
    }
    if (port == 2222) {                 /* 陈旧值：改写为 22 并回写配置 */
        port = PORT_DEFAULT;
        char nb[96]; int k = 0;
        const char *h = "# TinyOS SSH server (auto-healed by sshd)\nport 22\nautostart ";
        while (*h && k < 80) nb[k++] = *h++;
        if (autostart) { nb[k++]='y'; nb[k++]='e'; nb[k++]='s'; nb[k++]='\n'; }
        else           { nb[k++]='n'; nb[k++]='o'; nb[k++]='\n'; }
        A->file_write("/etc/sshd.conf", nb, k);
        A->println("sshd: /etc/sshd.conf had stale 'port 2222'; corrected to 22");
    }
    return port;
}

/* 把"监听在客户机端口 N"打到控制台，方便确认端口（也方便排错） */
static void log_listen(int port) {
    char m[64]; int q = 0;
    const char *p = "sshd: listening on guest TCP port ";
    while (*p && q < 40) m[q++] = *p++;
    if (port == 0) m[q++] = '0';
    else { char d[6]; int dl = 0, v = port;
           while (v > 0) { d[dl++] = (char)('0' + v % 10); v /= 10; }
           while (dl > 0) m[q++] = d[--dl]; }
    m[q] = 0;
    A->println(m);
}

void user_main(tinyos_api_t *api) {
    A = api;

    int port = read_port();
    int lh = A->sock_listen(port);
    if (lh < 0) {
        A->println("sshd: listen failed");
        return;
    }
    A->println("sshd: TinyOS SSH-2 server listening");
    log_listen(port);
    A->println("      algorithms: curve25519-sha256 / ssh-ed25519 / aes128-ctr / hmac-sha2-256");
    A->println("      connect with: ssh -p 2222 root@127.0.0.1");
    A->println("      (launcher forwards host 2222 -> guest 22)");
    A->println("      press ESC here to stop the server");

    for (;;) {
        if (A->local_kbhit() && A->local_getc() == 27) break;
        int fd = A->sock_accept(lh, 200);
        if (fd < 0) continue;
        serve(fd);
        A->sock_close(fd);
        /* 复位身份：下一条连接从头认证，不继承上一个用户的权限 */
        A->user_set_current(0);
        A->println("sshd: connection closed");
    }
    A->sock_close(lh);
    A->println("sshd: stopped");
}
