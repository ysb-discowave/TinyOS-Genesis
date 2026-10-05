#include "net.h"
#include "e1000.h"
#include "ftp.h"
#include "smb.h"
#include "libc.h"
#include "mm.h"
#include "console.h"
#include "pit.h"

u32 g_ip = 0x0A00020F;     /* 10.0.2.15 */
u32 g_gw = 0x0A000202;     /* 10.0.2.2  (QEMU user-net 网关) */
u32 g_dns = 0x0A000203;    /* 10.0.2.3  (QEMU SLIRP 内置 DNS 转发，把查询转给宿主机 DNS) */
u8  g_net_mac[6];

static int g_up = 0;
static u32 g_ping_wait = 0xFFFFFFFFu;
static u32 g_arp_waiting = 0;

/* ---------- 字节序 ---------- */
static u16 htons(u16 v){ return (u16)((v>>8)|(v<<8)); }
static u16 ntohs(u16 v){ return htons(v); }
static u32 htonl(u32 v){ return ((v&0xFF)<<24)|((v&0xFF00)<<8)|((v>>8)&0xFF00)|((v>>24)&0xFF); }
static u32 ntohl(u32 v){ return htonl(v); }

static u16 cksum(const u8 *p, u32 len) {
    u32 sum = 0;
    for (u32 i = 0; i < len; i += 2)
        sum += (u16)((p[i] << 8) | (i + 1 < len ? p[i+1] : 0));
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static void tcp_input_internal(u32 src, const u8 *seg, u32 len);

/* ---------------------------------------------------------------
 * 校验和：两个坑都在这里踩过，务必照此写法
 *   1) cksum() 按"大端字"累加，返回的是"逻辑值"，而 x86 是小端，
 *      直接 *(u16*)p = 值 会把两个字节颠倒，对端一验就错
 *      （表现：ARP 通、但 SYN-ACK/ICMP 全被对端静默丢弃）。
 *      所以必须按大端逐字节写回：高字节在前。
 *   2) TCP 伪头部必须用和 TCP 段"同一套字节序"累加。曾经把伪头部
 *      写成 u16 数组后用主机序相加，结果伪头部与段用了两种字节序，
 *      校验和恒错。这里统一成"逐字节大端读"。
 * --------------------------------------------------------------- */
static void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)(v & 0xFF); }

/* 伪头部(12B) + 段，统一大端累加 */
static u16 tcp_cksum(u32 src, u32 dst, u8 proto, const u8 *seg, u32 len) {
    u8 ph[12];
    *(u32*)&ph[0] = htonl(src);
    *(u32*)&ph[4] = htonl(dst);
    ph[8] = 0; ph[9] = proto;
    put_be16(ph + 10, (u16)len);
    u32 sum = 0;
    for (u32 i = 0; i < 12; i += 2) sum += (u16)((ph[i] << 8) | ph[i+1]);
    for (u32 i = 0; i < len; i += 2)
        sum += (u16)((seg[i] << 8) | (i + 1 < len ? seg[i+1] : 0));
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

/* ================= ARP ================= */
typedef struct { u32 ip; u8 mac[6]; int valid; } arp_ent_t;
static arp_ent_t g_arp[16];

static int arp_find(u32 ip) {
    for (int i = 0; i < 16; i++) if (g_arp[i].valid && g_arp[i].ip == ip) return i;
    return -1;
}
static void arp_request(u32 ip) {
    u8 a[28];
    *(u16*)&a[0] = htons(1);
    *(u16*)&a[2] = htons(ETH_TYPE_IP);
    a[4] = 6; a[5] = 4;
    *(u16*)&a[6] = htons(1);
    memcpy(&a[8], g_net_mac, 6);
    *(u32*)&a[14] = htonl(g_ip);
    memset(&a[18], 0, 6);
    *(u32*)&a[24] = htonl(ip);
    eth_send((u8[6]){0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, ETH_TYPE_ARP, a, 28);
}
int arp_resolve(u32 ip, u8 mac[6]) {
    int i = arp_find(ip);
    if (i >= 0) { memcpy(mac, g_arp[i].mac, 6); return 1; }
    if (!g_arp_waiting) arp_request(ip);
    return 0;
}
int arp_resolve_wait(u32 ip, u8 mac[6], u32 timeout_ms) {
    int i = arp_find(ip);
    if (i >= 0) { memcpy(mac, g_arp[i].mac, 6); return 1; }
    int saved = (int)g_arp_waiting;
    g_arp_waiting = 1;
    arp_request(ip);
    u32 waited = 0;
    while (waited < timeout_ms) {
        net_poll();
        i = arp_find(ip);
        if (i >= 0) { g_arp_waiting = (u32)saved; memcpy(mac, g_arp[i].mac, 6); return 1; }
        pit_sleep(2); waited += 2;
    }
    g_arp_waiting = (u32)saved;
    return 0;
}
static void arp_input(const u8 *a, u32 len) {
    if (len < 28) return;
    u16 op = ntohs(*(u16*)&a[6]);
    u32 tip = ntohl(*(u32*)&a[24]);
    u32 sip = ntohl(*(u32*)&a[14]);
    if (op == 2 && tip != g_ip) return;
    int i = arp_find(sip);
    if (i < 0) { for (i = 0; i < 16; i++) if (!g_arp[i].valid) break; }
    if (i >= 16) return;
    memcpy(g_arp[i].mac, &a[8], 6);
    g_arp[i].ip = sip; g_arp[i].valid = 1;
    if (op == 1) {
        u8 r[28];
        *(u16*)&r[0] = htons(1); *(u16*)&r[2] = htons(ETH_TYPE_IP);
        r[4] = 6; r[5] = 4; *(u16*)&r[6] = htons(2);
        memcpy(&r[8], g_net_mac, 6); *(u32*)&r[14] = htonl(g_ip);
        memcpy(&r[18], &a[8], 6);    *(u32*)&r[24] = htonl(sip);
        eth_send(a + 8, ETH_TYPE_ARP, r, 28);
    }
}

/* ================= 以太网 / IP ================= */
void eth_send(const u8 dst[6], u16 ethertype, const u8 *payload, u32 len) {
    u8 *f = (u8*)kmalloc(len + 14);
    if (!f) return;
    memcpy(f, dst, 6); memcpy(f + 6, g_net_mac, 6);
    *(u16*)&f[12] = htons(ethertype);
    memcpy(f + 14, payload, len);
    e1000_send(f, len + 14);
    kfree(f);
}

int ip_send(u32 dst, u8 proto, const u8 *payload, u32 len) {
    u8 mac[6];
    u32 nh = ((dst & 0xFFFFFF00u) == (g_ip & 0xFFFFFF00u)) ? dst : g_gw;
    if (!arp_resolve(nh, mac)) {
        if (!arp_resolve_wait(nh, mac, 500)) return -1;
    }
    u8 *p = (u8*)kmalloc(len + 20);
    if (!p) return -1;
    p[0] = 0x45; p[1] = 0;
    *(u16*)&p[2] = htons((u16)(len + 20));
    *(u16*)&p[4] = 0; *(u16*)&p[6] = 0;
    p[8] = 64; p[9] = proto; p[10] = 0; p[11] = 0;
    *(u32*)&p[12] = htonl(g_ip);
    *(u32*)&p[16] = htonl(dst);
    put_be16(p + 10, cksum(p, 20));
    memcpy(p + 20, payload, len);
    eth_send(mac, ETH_TYPE_IP, p, len + 20);
    kfree(p);
    return 0;
}

static void icmp_input(const u8 *pl, u32 plen, u32 src) {
    if (plen < 8) return;
    if (pl[0] == 8) {
        u8 *r = (u8*)kmalloc(plen);
        if (!r) return;
        memcpy(r, pl, plen);
        r[0] = 0; r[1] = 0;
        r[2] = 0; r[3] = 0;
        put_be16(r + 2, cksum(r, plen));
        ip_send(src, IPPROTO_ICMP, r, plen);
        kfree(r);
    } else if (pl[0] == 0) {
        if (src == g_ping_wait) g_ping_wait = 0xFFFFFFFFu;
    }
}

static void ip_input(const u8 *pkt, u32 len) {
    if (len < 20) return;
    if ((pkt[0] >> 4) != 4) return;
    u32 ihl = (u32)(pkt[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > len) return;
    u32 total = ntohs(*(u16*)&pkt[2]);
    if (total > len) total = len;
    u8 proto = pkt[9];
    u32 dst = ntohl(*(u32*)&pkt[16]);
    u32 src = ntohl(*(u32*)&pkt[12]);
    if (dst != g_ip && dst != 0xFFFFFFFFu) return;
    const u8 *pl = pkt + ihl;
    u32 plen = total > ihl ? total - ihl : 0;

    if (proto == IPPROTO_ICMP)      icmp_input(pl, plen, src);
    else if (proto == IPPROTO_UDP) {
        if (plen < 8) return;
        udp_dispatch(src, ntohs(*(u16*)&pl[0]), ntohs(*(u16*)&pl[2]), pl + 8, plen - 8);
    } else if (proto == IPPROTO_TCP) {
        if (plen >= 20) tcp_input_internal(src, pl, plen);
    }
}

/* ================= UDP ================= */
static struct { u16 port; void (*cb)(u32, u16, const u8*, u32); } g_udp[8];
int udp_bind(u16 port, void (*cb)(u32, u16, const u8*, u32)) {
    for (int i = 0; i < 8; i++)
        if (g_udp[i].port == 0) { g_udp[i].port = port; g_udp[i].cb = cb; return 0; }
    return -1;
}
void udp_dispatch(u32 src, u16 sport, u16 dport, const u8 *data, u32 len) {
    for (int i = 0; i < 8; i++)
        if (g_udp[i].port == dport && g_udp[i].cb) { g_udp[i].cb(src, sport, data, len); return; }
}
void udp_unbind(u16 port) {
    for (int i = 0; i < 8; i++)
        if (g_udp[i].port == port) { g_udp[i].port = 0; g_udp[i].cb = NULL; }
}
void udp_send(u32 dst, u16 sport, u16 dport, const u8 *data, u32 len) {
    u8 *p = (u8*)kmalloc(len + 8);
    if (!p) return;
    *(u16*)&p[0] = htons(sport); *(u16*)&p[2] = htons(dport);
    *(u16*)&p[4] = htons((u16)(len + 8)); p[6] = 0; p[7] = 0;
    memcpy(p + 8, data, len);
    ip_send(dst, IPPROTO_UDP, p, len + 8);
    kfree(p);
}

/* ================= TCP ================= */
typedef enum { TC_CLOSED, TC_SYN_SENT, TC_LISTEN, TC_SYN_RCVD, TC_ESTAB, TC_FIN_WAIT } tc_state;

#define RCVBUF 32768u

struct tcp_conn {
    tc_state state;
    u32 rip; u16 rport; u16 lport;
    u32 iss;
    u32 seq;             /* 下一个要发送的序号 */
    u32 snd_una;         /* 已被对端确认的序号 */
    u32 ack;             /* 期望收到的下一个序号 */
    u16 peer_win;        /* 对端通告窗口 */
    int peer_fin;        /* 对端已关闭写端 */
    u8  recv_buf[RCVBUF];
    u32 recv_len;
    struct tcp_conn *next;
};
static tcp_conn_t g_tcbs[16];
static tcp_conn_t *g_listeners = NULL;
static tcp_conn_t *g_acceptq = NULL;
static u16 g_port_seed = 40000;

/* 该 TCB 是否仍挂在监听表 / accept 队列里。
   遍历用步数上限兜底：即使链表已损坏成环也不会把内核卡死。 */
static int tcp_linked(tcp_conn_t *x) {
    tcp_conn_t *p = g_listeners;
    for (int i = 0; p && i < 16; i++, p = p->next) if (p == x) return 1;
    p = g_acceptq;
    for (int i = 0; p && i < 16; i++, p = p->next) if (p == x) return 1;
    return 0;
}
/* 把一个 TCB 从所有链表里摘掉，并把 next 归零 */
static void tcp_unlink(tcp_conn_t *c) {
    for (tcp_conn_t **pp = &g_listeners; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    for (tcp_conn_t **pp = &g_acceptq; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    c->next = NULL;
}
/* 只回收「已关闭 且 确实不在任何链表里」的 TCB。
   之前的版本只看 state，会把仍挂在 g_listeners 里的旧监听器再分配一次，
   于是 tcp_listen() 里 c->next = g_listeners 变成 c->next = c —— 链表自环，
   下一个收到的包会在 tcp_input_internal 的遍历里死循环（本次 FTP 卡死的真凶）。 */
static tcp_conn_t *tcp_alloc(void) {
    for (int i = 0; i < 16; i++)
        if (g_tcbs[i].state == TC_CLOSED && !tcp_linked(&g_tcbs[i])) {
            memset(&g_tcbs[i], 0, sizeof(tcp_conn_t));
            return &g_tcbs[i];
        }
    return NULL;
}
static u16 new_port(void) { if (++g_port_seed > 60000) g_port_seed = 40000; return g_port_seed; }
static u32 new_isn(void) { return 0x10000000u + ((u32)pit_ticks() << 9) + (u32)(g_port_seed & 0xFF); }

static void tcp_send_seg(tcp_conn_t *c, u8 flags, const u8 *data, u32 len) {
    u32 seg = len + 20;
    u8 *p = (u8*)kmalloc(seg);
    if (!p) return;
    *(u16*)&p[0] = htons(c->lport);
    *(u16*)&p[2] = htons(c->rport);
    *(u32*)&p[4] = htonl(c->seq);
    *(u32*)&p[8] = htonl(c->ack);
    p[12] = 5 << 4; p[13] = flags;
    u16 win = (u16)(RCVBUF - c->recv_len);
    if (win > 16384) win = 16384;
    *(u16*)&p[14] = htons(win);
    *(u16*)&p[16] = 0; *(u16*)&p[18] = 0;
    if (data && len) memcpy(p + 20, data, len);

    /* 校验和 = 伪头部 + TCP 段，统一大端累加（见 tcp_cksum 上方的说明） */
    put_be16(p + 16, tcp_cksum(g_ip, c->rip, IPPROTO_TCP, p, seg));

    c->seq += len + ((flags & 0x02) ? 1 : 0) + ((flags & 0x01) ? 1 : 0);
    ip_send(c->rip, IPPROTO_TCP, p, seg);
    kfree(p);
}

tcp_conn_t *tcp_connect(u32 ip, u16 port) {
    tcp_conn_t *c = tcp_alloc();
    if (!c) return NULL;
    c->state = TC_SYN_SENT; c->rip = ip; c->rport = port; c->lport = new_port();
    c->iss = new_isn(); c->seq = c->iss; c->snd_una = c->iss; c->ack = 0;
    c->peer_win = 8192; c->recv_len = 0;
    u8 mac[6];
    if (arp_resolve_wait(ip, mac, 800)) tcp_send_seg(c, 0x02, NULL, 0);
    return c;
}
tcp_conn_t *tcp_listen(u16 port) {
    /* 同一端口若已有旧监听（例如上一轮 PASV 的监听器没被摘链），
       先正规关闭并摘链，避免 TCB 复用后链表自环。 */
    for (int i = 0; i < 16; i++)
        if (g_tcbs[i].state == TC_LISTEN && g_tcbs[i].lport == port)
            tcp_close(&g_tcbs[i]);
    tcp_conn_t *c = tcp_alloc();
    if (!c) return NULL;
    c->state = TC_LISTEN; c->lport = port;
    c->next = g_listeners; g_listeners = c;
    return c;
}
tcp_conn_t *tcp_accept(tcp_conn_t *l) {
    if (!l) return NULL;
    u16 want = l->lport;
    tcp_conn_t **pp = &g_acceptq;
    while (*pp) {
        if ((*pp)->lport == want) {          /* 只领取本监听端口上的子连接 */
            tcp_conn_t *c = *pp;
            *pp = c->next; c->next = NULL;
            return c;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}
tcp_conn_t *tcp_accept_wait(tcp_conn_t *l, u32 timeout_ms) {
    u32 waited = 0;
    for (;;) {
        net_poll();
        tcp_conn_t *c = tcp_accept(l);
        if (c) return c;
        if (waited >= timeout_ms) return NULL;
        pit_sleep(2); waited += 2;
    }
}
int tcp_established(tcp_conn_t *c) { return c && c->state == TC_ESTAB; }
int tcp_closed_by_peer(tcp_conn_t *c) { return c && (c->state == TC_FIN_WAIT || c->peer_fin); }
int tcp_recv_avail(tcp_conn_t *c) { return c ? (int)c->recv_len : 0; }
u32 tcp_peer_ip(tcp_conn_t *c) { return c ? c->rip : 0; }
int tcp_sent_all_acked(tcp_conn_t *c) { return !c || c->snd_una >= c->seq; }
int tcp_wait_established(tcp_conn_t *c, u32 timeout_ms) {
    if (!c) return -1;
    u32 waited = 0, last_retx = 0;
    while (waited < timeout_ms) {
        net_poll();
        if (c->state == TC_ESTAB) return 0;
        if (c->state == TC_SYN_SENT && waited - last_retx >= 400) {
            c->seq = c->iss;
            tcp_send_seg(c, 0x02, NULL, 0);
            last_retx = waited;
        }
        pit_sleep(2); waited += 2;
    }
    return -1;
}

static tcp_conn_t *tcp_lookup(u16 lport, u32 rip, u16 rport) {
    for (int i = 0; i < 16; i++)
        if (g_tcbs[i].state != TC_CLOSED && g_tcbs[i].lport == lport &&
            g_tcbs[i].rip == rip && g_tcbs[i].rport == rport) return &g_tcbs[i];
    return NULL;
}

static void tcp_input_internal(u32 src, const u8 *seg, u32 len) {
    u16 sport = ntohs(*(u16*)&seg[0]);
    u16 dport = ntohs(*(u16*)&seg[2]);
    u32 sseq  = ntohl(*(u32*)&seg[4]);
    u32 sack  = ntohl(*(u32*)&seg[8]);
    u8  flags = seg[13];
    u16 peerw = ntohs(*(u16*)&seg[14]);
    u32 hdr   = (u32)(seg[12] >> 4) * 4;
    if (hdr < 20 || hdr > len) return;
    u32 plen  = len - hdr;
    const u8 *data = seg + hdr;

    /* 监听端口收到 SYN → 建立子连接。
       遍历加步数上限：即便链表被外部原因搞坏也不会在此死循环。 */
    int guard = 0;
    for (tcp_conn_t *p = g_listeners; p && guard < 16; p = p->next, guard++) {
        if (p->lport == dport && p->state == TC_LISTEN && (flags & 0x02) && !(flags & 0x10)) {
            tcp_conn_t *c = tcp_alloc();
            if (c) {
                c->state = TC_SYN_RCVD; c->rip = src; c->rport = sport; c->lport = dport;
                c->ack = sseq + 1;
                c->iss = new_isn(); c->seq = c->iss; c->snd_una = c->iss;
                c->peer_win = peerw ? peerw : 8192;
                tcp_send_seg(c, 0x12, NULL, 0);
            }
            return;
        }
    }

    tcp_conn_t *c = tcp_lookup(dport, src, sport);
    if (!c) return;

    if (flags & 0x10) {                       /* 收到 ACK：更新发送窗口 */
        if (sack > c->snd_una && sack <= c->seq) c->snd_una = sack;
        c->peer_win = peerw;
    }

    if (c->state == TC_SYN_SENT && (flags & 0x12) == 0x12) {
        c->ack = sseq + 1;
        c->seq = c->iss + 1;
        c->snd_una = c->iss + 1;
        tcp_send_seg(c, 0x10, NULL, 0);
        c->state = TC_ESTAB;
    } else if (c->state == TC_SYN_RCVD && (flags & 0x10) && !(flags & 0x02)) {
        c->state = TC_ESTAB;
        c->next = g_acceptq; g_acceptq = c;
    } else if (c->state == TC_ESTAB || c->state == TC_FIN_WAIT) {
        if (plen > 0) {
            if (sseq == c->ack) {
                u32 n = plen;
                if (n > RCVBUF - c->recv_len) n = RCVBUF - c->recv_len;
                if (n) { memcpy(c->recv_buf + c->recv_len, data, n); c->recv_len += n; }
                c->ack = sseq + plen;
            }
            tcp_send_seg(c, 0x10, NULL, 0);
        }
        if (flags & 0x01) {                   /* FIN */
            c->peer_fin = 1;
            c->ack = sseq + plen + 1;
            if (c->state != TC_FIN_WAIT) { tcp_send_seg(c, 0x11, NULL, 0); c->state = TC_FIN_WAIT; }
            else tcp_send_seg(c, 0x10, NULL, 0);
        }
    }
}

static int tcp_can_send(tcp_conn_t *c) {
    u32 outstanding = c->seq - c->snd_una;
    u32 win = c->peer_win ? c->peer_win : 4096;
    if (win > 16384) win = 16384;
    return outstanding < win;
}

int tcp_send(tcp_conn_t *c, const u8 *data, u32 len) {
    if (!c || (c->state != TC_ESTAB && c->state != TC_FIN_WAIT)) return -1;
    u32 off = 0;
    while (off < len) {
        u32 waited = 0;
        while (!tcp_can_send(c)) {
            net_poll(); pit_sleep(1);
            if (++waited > 8000) return (int)off;    /* 超时 */
        }
        u32 n = len - off;
        u32 room = (c->peer_win ? c->peer_win : 4096);
        if (room > 1024) room = 1024;
        if (n > room) n = room;
        tcp_send_seg(c, 0x18, data + off, n);
        off += n;
        net_poll();
    }
    return (int)len;
}
int tcp_recv(tcp_conn_t *c, u8 *buf, u32 max) {
    if (!c || c->recv_len == 0) return 0;
    u16 before = (u16)(RCVBUF - c->recv_len);
    u32 n = c->recv_len; if (n > max) n = max;
    memcpy(buf, c->recv_buf, n);
    c->recv_len -= n;
    if (c->recv_len) memmove(c->recv_buf, c->recv_buf + n, c->recv_len);
    /* 窗口从 0 恢复时发送窗口更新 */
    if (before == 0 && c->recv_len < RCVBUF && c->state != TC_CLOSED)
        tcp_send_seg(c, 0x10, NULL, 0);
    return (int)n;
}
int tcp_read_exact(tcp_conn_t *c, u8 *buf, u32 n, u32 timeout_ms) {
    u32 got = 0, waited = 0;
    while (got < n) {
        int r = tcp_recv(c, buf + got, n - got);
        if (r > 0) { got += (u32)r; waited = 0; continue; }
        if (tcp_closed_by_peer(c)) break;
        if (waited >= timeout_ms) break;
        net_poll(); pit_sleep(1); waited++;
    }
    return (int)got;
}
void tcp_close(tcp_conn_t *c) {
    if (!c) return;
    if (c->state == TC_ESTAB || c->state == TC_SYN_RCVD) tcp_send_seg(c, 0x11, NULL, 0);
    tcp_unlink(c);          /* 关键：关闭时必须摘链，否则该 TCB 会被 tcp_alloc 复用成自环 */
    c->state = TC_CLOSED;
}
/* 关闭并摘除监听端口 lport 上尚未被 accept 的子连接。
   PASV 场景下客户端可能先连数据端口却又不发数据命令（例如 Explorer 的 DELE/RMD），
   这些子连接会一直挂在 accept 队列里占着 TCB；此处统一回收。 */
void tcp_drop_pending(u16 lport) {
    for (int i = 0; i < 16; i++) {
        tcp_conn_t *x = &g_tcbs[i];
        if (x->state != TC_CLOSED && x->state != TC_LISTEN && x->lport == lport)
            tcp_close(x);
    }
}
int tcp_readline(tcp_conn_t *c, char *buf, u32 max, u32 timeout_ms) {
    u32 i = 0, waited = 0;
    for (;;) {
        u8 b;
        int n = tcp_recv(c, &b, 1);
        if (n == 1) {
            waited = 0;
            if (b == '\n') { if (i > 0 && buf[i-1] == '\r') i--; buf[i] = 0; return (int)i; }
            if (i < max - 1) buf[i++] = (char)b;
        } else {
            net_poll(); pit_sleep(1);
            if (++waited >= timeout_ms) { buf[i] = 0; return -1; }
        }
    }
}
void tcp_writestr(tcp_conn_t *c, const char *s) { tcp_send(c, (const u8*)s, (u32)strlen(s)); }
void tcp_writeline(tcp_conn_t *c, const char *s) {
    u32 n = (u32)strlen(s);
    u8 *tmp = (u8*)kmalloc(n + 2);
    if (!tmp) return;
    memcpy(tmp, s, n); tmp[n] = '\r'; tmp[n+1] = '\n';
    tcp_send(c, tmp, n + 2);
    kfree(tmp);
}

/* ================= DNS ================= */
/* 最简 DNS 解析器：向 g_dns（默认 10.0.2.3，QEMU SLIRP 内置转发）发 UDP 查询，
   解析 A 记录。支持应答里的域名压缩指针（0xC0xx）。 */
static u8  g_dns_ans[4];
static int g_dns_done = 0;
static u16 g_dns_port;
static u16 g_dns_id;

static void dns_recv(u32 src, u16 sport, const u8 *data, u32 len) {
    (void)src; (void)sport;
    if (len < 12) return;
    if (ntohs(*(u16*)&data[0]) != g_dns_id) return;   /* 只认本次请求的应答 */
    u16 an = ntohs(*(u16*)&data[6]);
    if (an == 0) return;
    u32 off = 12;
    /* 跳过问题段中的域名（支持压缩指针） */
    while (off + 1 <= len) {
        u8 b = data[off];
        if (b == 0) { off += 1; break; }
        if ((b & 0xC0) == 0xC0) { off += 2; break; }
        off += b + 1;
    }
    off += 4; /* 跳过 QTYPE / QCLASS */
    for (int i = 0; i < an && off + 10 <= len; i++) {
        u8 b = data[off];
        if ((b & 0xC0) == 0xC0) off += 2;
        else { while (off < len && data[off] != 0) off += data[off] + 1; off++; }
        u16 type = ntohs(*(u16*)&data[off]); off += 2;
        u16 cls  = ntohs(*(u16*)&data[off]); off += 2;
        off += 4; /* TTL */
        u16 rdlen = ntohs(*(u16*)&data[off]); off += 2;
        if (type == 1 && cls == 1 && rdlen >= 4) {
            memcpy(g_dns_ans, data + off, 4);
            g_dns_done = 1;
        }
        off += rdlen;
    }
}

int dns_resolve(const char *name, u32 *out_ip) {
    if (!g_up) return -1;
    g_dns_done = 0;
    g_dns_port = (u16)(49152 + (g_port_seed & 0x3FFF));   /* 临时端口 49152..65535 */
    if (udp_bind(g_dns_port, dns_recv) != 0) return -1;
    g_dns_id = (u16)((g_port_seed ^ 0xABCD) & 0xFFFF);

    u8 q[300];
    u32 ql = 0;
    *(u16*)&q[ql] = htons(g_dns_id); ql += 2;   /* ID */
    *(u16*)&q[ql] = htons(0x0100);    ql += 2;  /* 标准查询，RD=1 */
    *(u16*)&q[ql] = htons(1);         ql += 2;  /* QDCOUNT=1 */
    *(u16*)&q[ql] = 0;                ql += 2;  /* ANCOUNT */
    *(u16*)&q[ql] = 0;                ql += 2;  /* NSCOUNT */
    *(u16*)&q[ql] = 0;                ql += 2;  /* ARCOUNT */
    /* QNAME：长度前缀标签，根以 0 结尾 */
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int lab = dot ? (int)(dot - p) : (int)strlen(p);
        if (lab > 63) lab = 63;
        q[ql++] = (u8)lab;
        memcpy(q + ql, p, lab); ql += lab;
        p = dot ? dot + 1 : p + lab;
    }
    q[ql++] = 0;
    *(u16*)&q[ql] = htons(1); ql += 2;          /* QTYPE = A */
    *(u16*)&q[ql] = htons(1); ql += 2;          /* QCLASS = IN */

    udp_send(g_dns, g_dns_port, 53, q, ql);

    u32 waited = 0;
    while (waited < 3000) {
        net_poll();
        if (g_dns_done) {
            *out_ip = (g_dns_ans[0]<<24)|(g_dns_ans[1]<<16)|(g_dns_ans[2]<<8)|g_dns_ans[3];
            udp_unbind(g_dns_port);
            return 0;
        }
        pit_sleep(2); waited += 2;
    }
    udp_unbind(g_dns_port);
    return -1;
}

u32 host_to_ip(const char *s) {
    int is_ip = 1;
    for (const char *c = s; *c; c++)
        if (!((*c >= '0' && *c <= '9') || *c == '.')) { is_ip = 0; break; }
    if (is_ip) return ip_parse(s);
    u32 ip = 0;
    if (dns_resolve(s, &ip) == 0) return ip;
    return 0;
}

/* TCP 连通性探测：SLIRP 下 ICMP 常被丢弃，但 TCP 一定通，用它验证外网可达性 */
int net_tcping(u32 ip, u16 port, u32 timeout_ms) {
    tcp_conn_t *c = tcp_connect(ip, port);
    if (!c) return -1;
    int r = tcp_wait_established(c, timeout_ms);
    tcp_close(c);
    return r;  /* 0 = 连通 */
}

/* ================= 工具 ================= */
u32 ip_parse(const char *s) {
    u32 v[4] = {0,0,0,0}; int n = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') v[n] = v[n]*10 + (u32)(*s - '0');
        else if (*s == '.' && n < 3) n++;
        s++;
    }
    return (v[0]<<24)|(v[1]<<16)|(v[2]<<8)|v[3];
}
void ip_to_str(u32 ip, char *out) {
    snprintf(out, 16, "%u.%u.%u.%u", (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF);
}
int net_ping(u32 ip) {
    if (!g_up) return -1;
    u8 pkt[16];
    pkt[0] = 8; pkt[1] = 0; pkt[2] = 0; pkt[3] = 0;
    pkt[4] = 0x12; pkt[5] = 0x34; pkt[6] = 0; pkt[7] = 1;
    for (int i = 8; i < 16; i++) pkt[i] = (u8)('a' + i);
    put_be16(pkt + 2, cksum(pkt, 16));
    g_ping_wait = ip;
    if (ip_send(ip, IPPROTO_ICMP, pkt, 16) != 0) { g_ping_wait = 0xFFFFFFFFu; return -1; }
    for (u32 t = 0; t < 1500; t++) {
        net_poll();
        if (g_ping_wait == 0xFFFFFFFFu) return 0;
        pit_sleep(1);
    }
    g_ping_wait = 0xFFFFFFFFu;
    return -1;
}

/* ================= 入口 ================= */
static void frame_input(const u8 *frame, u32 len) {
    if (len < 14) return;
    u16 et = ntohs(*(u16*)&frame[12]);
    if (et == ETH_TYPE_ARP) arp_input(frame + 14, len - 14);
    else if (et == ETH_TYPE_IP) ip_input(frame + 14, len - 14);
}
void net_init(void) {
    if (e1000_init() != 0) { g_up = 0; return; }
    e1000_get_mac(g_net_mac);
    g_up = 1;
}
int net_up(void) { return g_up; }
void net_poll(void) { if (g_up) e1000_poll(frame_input); }
void net_poll_all(void) {
    net_poll();
    ftp_server_poll();
    smb_server_poll();
}
void net_info(void) {
    char ip[20], gw[20];
    ip_to_str(g_ip, ip); ip_to_str(g_gw, gw);
    if (!g_up) { kprintf("network: disabled (no e1000 NIC found)\n"); return; }
    kprintf("network: e1000 enabled\n");
    kprintf("  MAC  %02X:%02X:%02X:%02X:%02X:%02X\n",
            g_net_mac[0],g_net_mac[1],g_net_mac[2],g_net_mac[3],g_net_mac[4],g_net_mac[5]);
    kprintf("  IP   %s/24    gateway %s\n", ip, gw);
    char dns[20]; ip_to_str(g_dns, dns);
    kprintf("  DNS  %s\n", dns);
    int any = 0;
    for (int i = 0; i < 16; i++) if (g_arp[i].valid) {
        if (!any) { kprintf("  ARP cache:\n"); any = 1; }
        char t[20]; ip_to_str(g_arp[i].ip, t);
        kprintf("    %-15s %02X:%02X:%02X:%02X:%02X:%02X\n", t,
                g_arp[i].mac[0],g_arp[i].mac[1],g_arp[i].mac[2],
                g_arp[i].mac[3],g_arp[i].mac[4],g_arp[i].mac[5]);
    }
    if (!any) kprintf("  ARP cache: (empty)\n");
}
