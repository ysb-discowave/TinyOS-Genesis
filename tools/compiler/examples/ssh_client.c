/* ============================================================
 * ssh_client.c —— TinyOS 的 SSH-2 客户端（编译成 SSHCLIENT.TNCR）
 * ------------------------------------------------------------
 * 这是货真价实的 SSH-2 客户端（与内核里 SSHD.TNCR 服务端互为镜像）：
 *   传输层：版本交换 -> KEXINIT -> ECDH(curve25519-sha256)
 *           -> NEWKEYS -> AES-128-CTR + HMAC-SHA2-256
 *   认证层：ssh-userauth 的 password 方法
 *   连接层：开 session 通道，发 exec 请求跑命令，读取远端 stdout/stderr
 *
 * 这是把 SSHD.TNCR 的单向服务端逻辑"翻成"客户端：方向全部反过来。
 * 复用了同一套密码学（tools/compiler/sshd_crypto.h）与同一套报文收发。
 *
 * 用法（从 tinysh 里）：
 *     ssh <host> [port] [command...]
 *   无 command 时进入"逐行发送"的简易交互 shell（无 pty，全屏程序用不了）；
 *   有 command 时执行单条命令后退出（类似 `ssh host cmd`）。
 *
 * 约束：freestanding，-nostdinc，无 libc，所有字符串/内存函数自己写。
 * 主机名目前只支持点分十进制 IP（DNS 未暴露给用户态）。
 *
 * 已知取舍（与任务要求一致）：
 *   * 不做 pty（远端 vim/top 这类全屏交互程序用不了）。
 *   * 不做主机密钥签名验签（计算 H 但不验签，留给远端主机密钥只是 TOFU
 *     接受）。这是为了让握手在裸机上跑通、且不强依赖 ed25519 验签实现；
 *     生产环境应在此处补上 cx_ed_verify。
 * ============================================================ */
#include "api_user.h"
#include "sshd_crypto.h"

static tinyos_api_t *A;

/* ------------------------------------------------------------------ */
/* 参数 / 缓冲                                                         */
/* ------------------------------------------------------------------ */
#define PORT_DEFAULT   22
#define TMO_HANDSHAKE  25000
#define PKT_MAX        8192
#define RX_BUF        (PKT_MAX + 2048)
#define TX_BUF        (PKT_MAX + 256)
#define PL_BUF        (PKT_MAX)
#define OUT_BUF       16384          /* 远端回显/命令输出上限 */
#define MAX_LINE      256
#define CHUNK         3072

/* SSH 报文号（与 SSHD.TNCR 一致） */
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
#define M_CHANNEL_OPEN           90
#define M_CHANNEL_OPEN_CONFIRM   91
#define M_CHANNEL_WINDOW_ADJUST  93
#define M_CHANNEL_DATA           94
#define M_CHANNEL_EXTENDED_DATA  95
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
static int sisdigit(int c) { return c >= '0' && c <= '9'; }
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
/* 全局缓冲                                                            */
/* ------------------------------------------------------------------ */
static unsigned char g_rx[RX_BUF];
static unsigned char g_tx[TX_BUF];
static unsigned char g_pl[PL_BUF];
static unsigned char g_out[OUT_BUF];

typedef struct {
    int  fd;
    int  rxlen, rxpos;
    u32  seq_out, seq_in;
    int  enc_out, enc_in;
    unsigned char key_out[16], iv_out[16], mac_out[32];
    unsigned char key_in[16],  iv_in[16],  mac_in[32];
    cx_aes128ctr  ctr_out, ctr_in;
    unsigned char v_c[256]; u32 v_c_len;     /* 我方版本串（无 CRLF） */
    unsigned char v_s[256]; u32 v_s_len;     /* 服务端版本串（无 CRLF） */
    unsigned char I_c[4096]; u32 I_c_len;
    unsigned char I_s[4096]; u32 I_s_len;
    unsigned char kenc[64];  u32 kenc_len;
    unsigned char H[32];
    unsigned char Q_C[32], Q_S[32];
    unsigned char ks[64];    u32 ks_len;     /* 服务端主机密钥 blob */
    unsigned char eph_priv[32];
    int  authed;
} conn_t;

static void perr(const char *msg) { A->println(msg); }

/* ------------------------------------------------------------------ */
/* 接收装配                                                            */
/* ------------------------------------------------------------------ */
static void rx_compact(conn_t *c) {
    if (c->rxpos > 0) {
        int rem = c->rxlen - c->rxpos;
        if (rem > 0) cx_memcpy(g_rx, g_rx + c->rxpos, (u32)rem);
        c->rxlen = rem; c->rxpos = 0;
    }
}
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
/* 发送 / 接收一个完整 SSH 报文（与 SSHD.TNCR 完全一致）                */
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
        cx_hmac256_update(&h, w, 4 + packet_len);
        cx_hmac256_final(&h, w + 4 + packet_len);
    }
    if (c->enc_out) cx_aes128ctr_xor(&c->ctr_out, w, 4 + packet_len);

    int r = A->sock_send(c->fd, w, (int)(4 + packet_len + maclen));
    if (r < 0) return -1;
    c->seq_out++;
    return 0;
}

static int pkt_read(conn_t *c, unsigned char *type, unsigned char **pl, u32 *plen) {
    unsigned char hdr[4];
    int r = rx_ensure(c, 4, TMO_HANDSHAKE);
    if (r != 0) return r == 1 ? 1 : -1;
    cx_memcpy(hdr, rx_take(c, 4), 4);
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
    return 0;
}

/* ------------------------------------------------------------------ */
/* 密钥派生 / KEX 辅助                                                 */
/* ------------------------------------------------------------------ */
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

/* 客户端方向：OUT=C->S 用 A/C/E，IN=S->C 用 B/D/F（RFC 4253 §7.2） */
static void derive_key(unsigned char out[32], const unsigned char *kenc, u32 klen,
                       const unsigned char *H, unsigned char letter) {
    cx_sha256_ctx ctx;
    cx_sha256_init(&ctx);
    cx_sha256_update(&ctx, kenc, klen);
    cx_sha256_update(&ctx, H, 32);
    cx_sha256_update(&ctx, &letter, 1);
    cx_sha256_update(&ctx, H, 32);
    cx_sha256_final(&ctx, out);
}

/* ------------------------------------------------------------------ */
/* 版本交换                                                            */
/* ------------------------------------------------------------------ */
static int do_version(conn_t *c) {
    static const char banner[] = "SSH-2.0-TinyOS\r\n";
    if (A->sock_send(c->fd, (const unsigned char *)banner, slen(banner)) < 0) return -1;
    {
        int sl = slen(banner);              /* 去掉结尾 \r\n\0 的部分 */
        int n = sl >= 2 ? sl - 2 : sl;
        if (n > (int)sizeof(c->v_c) - 1) n = (int)sizeof(c->v_c) - 1;
        cx_memcpy(c->v_c, (const unsigned char *)banner, (u32)n);
        c->v_c_len = (u32)n;
    }
    /* 读服务端版本：直到 \n，去掉 \r */
    int got = 0;
    for (int i = 0; i < 512; i++) {
        unsigned char ch;
        int n = A->sock_recv(c->fd, &ch, 1, 5000);
        if (n <= 0) return -1;
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (got < 250) c->v_s[got++] = ch;
    }
    c->v_s_len = (u32)got;
    if (got < 4) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 密钥交换（客户端）                                                  */
/* ------------------------------------------------------------------ */
static int send_kexinit(conn_t *c) {
    bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
    unsigned char cookie[16];
    A->rand_bytes(cookie, 16);
    bb_u8(&b, M_KEXINIT);
    bb_raw(&b, cookie, 16);
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
    bb_u8(&b, 0);
    bb_u32(&b, 0);
    /* I_c 必须带最前面的消息类型字节（20），与 H 的定义一致 */
    c->I_c[0] = M_KEXINIT;
    cx_memcpy(c->I_c + 1, b.b + 1, b.n - 1);
    c->I_c_len = b.n;
    return pkt_send(c, b.b, b.n);
}

static int do_kex(conn_t *c) {
    unsigned char *pl; u32 n; unsigned char t;

    if (send_kexinit(c) < 0) { perr("ssh: kex: cannot send KEXINIT"); return -1; }
    if (pkt_read(c, &t, &pl, &n) != 0 || t != M_KEXINIT) {
        perr("ssh: kex: no server KEXINIT"); return -1;
    }
    if (n + 1u > (u32)sizeof(c->I_s)) { perr("ssh: kex: server KEXINIT too large"); return -1; }
    c->I_s[0] = M_KEXINIT;
    cx_memcpy(c->I_s + 1, pl, n);
    c->I_s_len = n + 1u;

    /* 生成临时密钥对，发 KEX_ECDH_INIT */
    A->rand_bytes(c->eph_priv, 32);
    cx_x25519_base(c->Q_C, c->eph_priv);
    {
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_KEX_ECDH_INIT);
        bb_str(&b, c->Q_C, 32);
        if (pkt_send(c, b.b, b.n) < 0) { perr("ssh: kex: cannot send ECDH_INIT"); return -1; }
    }

    if (pkt_read(c, &t, &pl, &n) != 0 || t != M_KEX_ECDH_REPLY) {
        perr("ssh: kex: no ECDH_REPLY"); return -1;
    }
    /* KEX_ECDH_REPLY = string K_S, string Q_S, string signature */
    if (n < 4 + 4) { perr("ssh: kex: ECDH_REPLY too short"); return -1; }
    u32 kslen = get_u32(pl);
    if (kslen > 64 || 4u + kslen + 4u + 32u + 4u > n) { perr("ssh: kex: bad K_S"); return -1; }
    cx_memcpy(c->ks, pl + 4, kslen);
    c->ks_len = kslen;
    unsigned char *p2 = pl + 4 + kslen;
    u32 qslen = get_u32(p2);
    if (qslen != 32) { perr("ssh: kex: bad Q_S length"); return -1; }
    cx_memcpy(c->Q_S, p2 + 4, 32);

    /* 共享秘密 = x25519(我方私钥, 服务端公钥) */
    unsigned char sh[32];
    cx_x25519(sh, c->eph_priv, c->Q_S);

    /* H = SHA256( V_C | V_S | I_C | I_S | K_S | Q_C | Q_S | K ) */
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
    put_u32(lb, c->ks_len); cx_sha256_update(&hc, lb, 4);
    cx_sha256_update(&hc, c->ks, c->ks_len);
    put_u32(lb, 32); cx_sha256_update(&hc, lb, 4);
    cx_sha256_update(&hc, c->Q_C, 32);
    put_u32(lb, 32); cx_sha256_update(&hc, lb, 4);
    cx_sha256_update(&hc, c->Q_S, 32);
    {
        unsigned char kbuf[64]; u32 klen = 0;
        mpint_shared(kbuf, &klen, sh);
        cx_sha256_update(&hc, kbuf, klen);
        cx_memcpy(c->kenc, kbuf, klen); c->kenc_len = klen;
    }
    cx_sha256_final(&hc, c->H);

    /* 注：这里故意不验证服务端签名（无 cx_ed_verify）。生产实现应在此
     * 用 K_S 里的 ssh-ed25519 公钥验 H 上的签名，避免中间人。 */

    /* NEWKEYS（必须用旧密钥发出，之后才切出方向） */
    {
        unsigned char nb[8]; bb_t p; bb_init(&p, nb, sizeof(nb));
        bb_u8(&p, M_NEWKEYS);
        if (pkt_send(c, p.b, p.n) < 0) { perr("ssh: kex: cannot send NEWKEYS"); return -1; }
    }
    /* 客户端 OUT(C->S) 密钥：A=IV C=key E=mac */
    unsigned char k[32];
    derive_key(k, c->kenc, c->kenc_len, c->H, 'A'); cx_memcpy(c->iv_out, k, 16);
    derive_key(k, c->kenc, c->kenc_len, c->H, 'C'); cx_memcpy(c->key_out, k, 16);
    derive_key(k, c->kenc, c->kenc_len, c->H, 'E'); cx_memcpy(c->mac_out, k, 32);
    cx_aes128ctr_init(&c->ctr_out, c->key_out, c->iv_out);
    c->enc_out = 1;

    if (pkt_read(c, &t, &pl, &n) != 0 || t != M_NEWKEYS) {
        perr("ssh: kex: no server NEWKEYS"); return -1;
    }
    /* 客户端 IN(S->C) 密钥：B=IV D=key F=mac */
    derive_key(k, c->kenc, c->kenc_len, c->H, 'B'); cx_memcpy(c->iv_in, k, 16);
    derive_key(k, c->kenc, c->kenc_len, c->H, 'D'); cx_memcpy(c->key_in, k, 16);
    derive_key(k, c->kenc, c->kenc_len, c->H, 'F'); cx_memcpy(c->mac_in, k, 32);
    cx_aes128ctr_init(&c->ctr_in, c->key_in, c->iv_in);
    c->enc_in = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 鉴权（password）                                                    */
/* ------------------------------------------------------------------ */
static void getpass(char *buf, int max) {
    int n = 0;
    A->print("password: ");
    for (;;) {
        if (A->local_kbhit()) {
            int ch = A->local_getc();
            if (ch == '\r' || ch == '\n') break;
            if (ch == 0x7f || ch == 0x08) { if (n > 0) n--; continue; }
            if (ch >= 32 && ch < 127 && n < max - 1) buf[n++] = (char)ch;
        }
    }
    buf[n] = 0;
    A->println("");
}

static int do_auth(conn_t *c, const char *user, const char *pass) {
    unsigned char *pl; u32 n; unsigned char t;

    /* 1) SERVICE_REQUEST ssh-userauth */
    {
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_SERVICE_REQUEST);
        bb_cs(&b, "ssh-userauth");
        if (pkt_send(c, b.b, b.n) < 0) return -1;
    }
    if (pkt_read(c, &t, &pl, &n) != 0 || t != M_SERVICE_ACCEPT) {
        perr("ssh: auth: no SERVICE_ACCEPT"); return -1;
    }

    /* 2) 先试 none，拿到服务端声明的可用方法 */
    {
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_USERAUTH_REQUEST);
        bb_cs(&b, user);
        bb_cs(&b, "ssh-connection");
        bb_cs(&b, "none");
        if (pkt_send(c, b.b, b.n) < 0) return -1;
    }
    for (;;) {
        if (pkt_read(c, &t, &pl, &n) != 0) return -1;
        if (t == M_USERAUTH_BANNER) {
            /* string message, string lang —— 打印一下 */
            if (n >= 4) { u32 ml = get_u32(pl); if (ml <= n - 4) { pl[4 + ml] = 0; A->println((char*)pl + 4); } }
            continue;
        }
        if (t == M_USERAUTH_SUCCESS) { c->authed = 1; return 0; }
        if (t == M_USERAUTH_FAILURE) break;     /* 到下面用 password 重试 */
        /* 其它（理论上不会）当失败处理 */
        break;
    }

    /* 3) password 方法 */
    {
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_USERAUTH_REQUEST);
        bb_cs(&b, user);
        bb_cs(&b, "ssh-connection");
        bb_cs(&b, "password");
        bb_u8(&b, 0);                          /* FALSE：不是修改口令 */
        bb_cs(&b, pass);
        if (pkt_send(c, b.b, b.n) < 0) return -1;
    }
    for (;;) {
        if (pkt_read(c, &t, &pl, &n) != 0) return -1;
        if (t == M_USERAUTH_BANNER) continue;
        if (t == M_USERAUTH_SUCCESS) { c->authed = 1; return 0; }
        if (t == M_USERAUTH_FAILURE) {
            perr("ssh: authentication failed (check user/password)");
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 通道 / exec / 交互                                                  */
/* ------------------------------------------------------------------ */
static void print_data(const unsigned char *d, u32 len) {
    static char line[256];
    u32 off = 0;
    while (off < len) {
        u32 chunk = len - off; if (chunk > 255) chunk = 255;
        cx_memcpy(line, d + off, chunk); line[chunk] = 0;
        A->print(line);
        off += chunk;
    }
}

/* 开 session 通道，返回服务端通道号(peer)，<0 失败 */
static int open_session(conn_t *c) {
    unsigned char *pl; u32 n; unsigned char t;
    {
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_CHANNEL_OPEN);
        bb_cs(&b, "session");
        bb_u32(&b, 0);                 /* 我方通道号 */
        bb_u32(&b, 256 * 1024);        /* 初始窗口 */
        bb_u32(&b, 32768);             /* 最大包 */
        if (pkt_send(c, b.b, b.n) < 0) return -1;
    }
    for (;;) {
        if (pkt_read(c, &t, &pl, &n) != 0) return -1;
        if (t == M_IGNORE) continue;
        if (t == M_CHANNEL_OPEN_CONFIRM) {
            if (n < 8) return -1;
            u32 peer = get_u32(pl + 4);   /* sender channel = 服务端通道号 */
            return (int)peer;
        }
        if (t == M_DISCONNECT) return -1;
        /* 其它报文先忽略 */
    }
}

/* exec 模式：跑单条命令并打印输出 */
static int run_exec(conn_t *c, int peer, const char *cmd) {
    unsigned char *pl; u32 n; unsigned char t;
    {
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_CHANNEL_REQUEST);
        bb_u32(&b, (u32)peer);
        bb_cs(&b, "exec");
        bb_u8(&b, 1);                  /* want reply */
        bb_cs(&b, cmd);
        if (pkt_send(c, b.b, b.n) < 0) return -1;
    }
    for (;;) {
        int r = pkt_read(c, &t, &pl, &n);
        if (r != 0) return r == 1 ? 0 : -1;
        if (t == M_CHANNEL_DATA) {
            if (n < 8) continue;
            u32 dlen = get_u32(pl + 4);
            if (n < 8 + dlen) continue;
            print_data(pl + 8, dlen);
        } else if (t == M_CHANNEL_EXTENDED_DATA) {
            if (n < 12) continue;
            u32 dlen = get_u32(pl + 8);
            if (n < 12 + dlen) continue;
            print_data(pl + 12, dlen);
        } else if (t == M_CHANNEL_REQUEST) {
            /* exit-status 之类，want_reply 一般 0，无需回复 */
            continue;
        } else if (t == M_CHANNEL_CLOSE || t == M_CHANNEL_EOF) {
            return 0;
        } else if (t == M_CHANNEL_SUCCESS || t == M_CHANNEL_WINDOW_ADJUST
                   || t == M_CHANNEL_OPEN_CONFIRM) {
            continue;
        } else if (t == M_DISCONNECT) {
            return -1;
        }
    }
}

/* 简易交互 shell：逐行发送，打印远端回显（无 pty） */
static int run_shell(conn_t *c, int peer) {
    unsigned char *pl; u32 n; unsigned char t;
    static char line[MAX_LINE];
    {
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_CHANNEL_REQUEST);
        bb_u32(&b, (u32)peer);
        bb_cs(&b, "shell");
        bb_u8(&b, 1);
        if (pkt_send(c, b.b, b.n) < 0) return -1;
    }
    A->println("ssh: interactive (no pty). type 'exit' to quit.");
    for (;;) {
        A->print("ssh> ");
        int l = A->readline(line, MAX_LINE - 1);
        if (l < 0) break;
        line[l] = 0;
        if (scmp(line, "exit") == 0 || scmp(line, "logout") == 0) break;
        /* 发送这一行（带 CRLF）作为通道数据 */
        int ll = slen(line);
        {
            bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
            bb_u8(&b, M_CHANNEL_DATA);
            bb_u32(&b, (u32)peer);
            bb_str(&b, (const unsigned char *)line, (u32)ll);
            bb_str(&b, (const unsigned char *)"\r\n", 2);
            if (pkt_send(c, b.b, b.n) < 0) return -1;
        }
        /* 读远端回显/输出直到空闲 */
        for (int idle = 0; idle < 20; ) {
            int r = pkt_read(c, &t, &pl, &n);
            if (r == 1) { idle++; continue; }
            if (r != 0) return -1;
            idle = 0;
            if (t == M_CHANNEL_DATA) {
                if (n >= 8) { u32 dlen = get_u32(pl + 4); if (n >= 8 + dlen) print_data(pl + 8, dlen); }
            } else if (t == M_CHANNEL_CLOSE || t == M_CHANNEL_EOF) {
                return 0;
            } else if (t == M_CHANNEL_REQUEST) {
                continue;
            }
        }
    }
    /* 关闭通道 */
    {
        bb_t b; bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_CHANNEL_EOF); bb_u32(&b, (u32)peer);
        pkt_send(c, b.b, b.n);
        bb_init(&b, g_pl, sizeof(g_pl));
        bb_u8(&b, M_CHANNEL_CLOSE); bb_u32(&b, (u32)peer);
        pkt_send(c, b.b, b.n);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 参数解析                                                            */
/* ------------------------------------------------------------------ */
static u32 parse_ipv4(const char *s) {
    /* 点分十进制 -> 主机序 u32，失败返回 0 */
    int parts[4]; int cur = 0, np = 0, v = 0;
    for (int i = 0; s[i]; i++) {
        if (sisdigit(s[i])) { v = v * 10 + (s[i] - '0'); cur = 1; }
        else if (s[i] == '.') {
            if (!cur) return 0;
            if (np >= 4) return 0;
            parts[np++] = v; v = 0; cur = 0;
        } else return 0;
    }
    if (!cur || np != 3) return 0;
    parts[np++] = v;
    if (parts[0] > 255 || parts[1] > 255 || parts[2] > 255 || parts[3] > 255) return 0;
    return ((u32)parts[0] << 24) | ((u32)parts[1] << 16) | ((u32)parts[2] << 8) | (u32)parts[3];
}

void user_main(tinyos_api_t *api) {
    A = api;

    /* 取参数串：ssh 之后的部分 */
    static char args[768];
    int al = A->cmdarg(args, sizeof(args) - 1);
    if (al < 0) al = 0;
    args[al] = 0;

    /* 简单分词：取 host / port / 余下命令 */
    static char hostbuf[128];
    int port = PORT_DEFAULT;
    int p = 0, state = 0; /* 0 host, 1 port_or_cmd, 2 cmd-rest */
    static char cmd[512]; int cp = 0;
    hostbuf[0] = 0;
    while (args[p]) {
        while (args[p] == ' ' || args[p] == '\t') p++;
        if (!args[p]) break;
        int s = p;
        while (args[p] && args[p] != ' ' && args[p] != '\t') p++;
        int elen = p - s;
        char tok[256]; int k = 0;
        while (k < elen && k < 255) { tok[k] = args[s + k]; k++; }
        tok[k] = 0;

        if (state == 0) {
            int i = 0;
            while (i < k && i < 127) { hostbuf[i] = tok[i]; i++; }
            hostbuf[i] = 0;
            state = 1; continue;
        }
        if (state == 1) {
            int allnum = 1;
            for (int i = 0; tok[i]; i++) if (!sisdigit(tok[i])) allnum = 0;
            if (allnum && slen(tok) <= 5) {
                int pv = 0; for (int i = 0; tok[i]; i++) pv = pv * 10 + (tok[i] - '0');
                if (pv > 0 && pv < 65536) { port = pv; state = 2; continue; }
            }
            /* 不是端口 -> 这是命令的第一个词 */
            cx_memcpy(cmd, tok, (u32)(k + 1)); cp = k;
            state = 2; continue;
        }
        /* state == 2：命令继续 */
        if (cp + 1 < (int)sizeof(cmd)) { cmd[cp++] = ' '; }
        cx_memcpy(cmd + cp, tok, (u32)(k + 1)); cp += k;
    }
    cmd[cp] = 0;

    if (hostbuf[0] == 0) {
        A->println("usage: ssh <host> [port] [command...]");
        return;
    }

    u32 ip = parse_ipv4(hostbuf);
    if (ip == 0) {
        A->println("ssh: only dotted-IPv4 hosts are supported (no DNS in userland)");
        A->print("ssh: got host='"); A->print(hostbuf); A->println("'");
        return;
    }

    static char ipstr[16];
    /* 反解成点分以便打印（主机序 -> 点分） */
    {
        int q = 0; char d[4];
        for (int oct = 0; oct < 4; oct++) {
            int v = (ip >> (24 - oct * 8)) & 0xFF;
            int dl = 0; if (v == 0) d[dl++] = '0';
            while (v > 0) { d[dl++] = (char)('0' + v % 10); v /= 10; }
            while (dl > 0) ipstr[q++] = d[--dl];
            if (oct < 3) ipstr[q++] = '.';
        }
        ipstr[q] = 0;
    }

    static char msg[160];
    { int q = 0; const char *p1 = "ssh: connecting to "; while (*p1 && q < 120) msg[q++] = *p1++;
      int i = 0; while (ipstr[i] && q < 140) msg[q++] = ipstr[i++];
      msg[q++] = ':';
      int pv = port, dl = 0; char d[6]; if (pv==0) d[dl++]='0'; while (pv>0){d[dl++]=(char)('0'+pv%10);pv/=10;}
      while (dl>0) msg[q++]=d[--dl];
      msg[q]=0; }
    A->println(msg);

    int fd = A->sock_connect(ip, (u16)port, 8000);
    if (fd < 0) {
        perr("ssh: connection failed (is the host reachable? is port open?)");
        return;
    }
    A->println("ssh: TCP connected");

    conn_t c;
    cx_memzero((unsigned char *)&c, sizeof(c));
    c.fd = fd;

    if (do_version(&c) < 0) { perr("ssh: version exchange failed"); A->sock_close(fd); return; }
    A->println("ssh: version exchanged");

    if (do_kex(&c) < 0) { A->sock_close(fd); return; }
    A->println("ssh: key exchange + NEWKEYS done (aes128-ctr / hmac-sha2-256)");

    /* 读用户名/口令 */
    static char user[32], pass[64];
    A->print("user: ");
    int ul = A->readline(user, sizeof(user) - 1);
    if (ul < 0) ul = 0; user[ul] = 0;
    getpass(pass, sizeof(pass));

    if (do_auth(&c, user, pass) < 0) { A->sock_close(fd); return; }
    A->println("ssh: authenticated");

    int peer = open_session(&c);
    if (peer < 0) { perr("ssh: cannot open session channel"); A->sock_close(fd); return; }

    if (cmd[0]) {
        A->print("ssh: exec '"); A->print(cmd); A->println("'");
        run_exec(&c, peer, cmd);
    } else {
        run_shell(&c, peer);
    }

    A->sock_close(fd);
    A->println("ssh: session closed");
}
