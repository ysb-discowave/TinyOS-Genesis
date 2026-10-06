#include "sock.h"
#include "net.h"
#include "pit.h"

typedef struct {
    tcp_conn_t *c;
    int         used;
    int         listen;
} sock_ent_t;

static sock_ent_t g_socks[SOCK_MAX];

static int sock_alloc(tcp_conn_t *c, int listen) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!g_socks[i].used) {
            g_socks[i].used = 1; g_socks[i].c = c; g_socks[i].listen = listen;
            return i;
        }
    }
    return -1;
}
static sock_ent_t *sock_get(int h) {
    if (h < 0 || h >= SOCK_MAX || !g_socks[h].used) return 0;
    return &g_socks[h];
}

int sock_listen(int port) {
    if (port <= 0 || port > 65535) return -1;
    tcp_conn_t *l = tcp_listen((u16)port);
    if (!l) return -1;
    int h = sock_alloc(l, 1);
    if (h < 0) return -1;
    return h;
}

int sock_connect(u32 ip, u16 port, int timeout_ms) {
    if (ip == 0 || port <= 0 || port > 65535) return -1;
    if (timeout_ms < 0) timeout_ms = 8000;
    tcp_conn_t *c = tcp_connect(ip, port);
    if (!c) return -1;
    if (tcp_wait_established(c, (u32)timeout_ms) != 0) {
        tcp_close(c);
        return -1;
    }
    int h = sock_alloc(c, 0);
    if (h < 0) { tcp_close(c); return -1; }
    return h;
}

int sock_accept(int lh, int timeout_ms) {
    sock_ent_t *e = sock_get(lh);
    if (!e || !e->listen) return -1;
    u32 waited = 0;
    for (;;) {
        tcp_conn_t *c = tcp_accept(e->c);
        if (c) {
            if (tcp_wait_established(c, 3000) != 0) { tcp_close(c); continue; }
            return sock_alloc(c, 0);
        }
        net_poll();
        if (timeout_ms >= 0 && waited >= (u32)timeout_ms) return -1;
        pit_sleep(1); waited++;
    }
}

int sock_recv(int h, u8 *buf, int max, int timeout_ms) {
    sock_ent_t *e = sock_get(h);
    if (!e || max <= 0) return -2;
    u32 waited = 0;
    for (;;) {
        int n = tcp_recv(e->c, buf, (u32)max);
        if (n > 0) return n;
        if (tcp_closed_by_peer(e->c)) {
            net_poll();
            n = tcp_recv(e->c, buf, (u32)max);
            return n > 0 ? n : -1;
        }
        if (timeout_ms >= 0 && waited >= (u32)timeout_ms) return 0;
        net_poll();
        pit_sleep(1); waited++;
    }
}

int sock_poll_recv(int h, u8 *buf, int max) {
    sock_ent_t *e = sock_get(h);
    if (!e || max <= 0) return -2;
    net_poll();
    return tcp_recv(e->c, buf, (u32)max);
}

int sock_send(int h, const u8 *buf, int n) {
    sock_ent_t *e = sock_get(h);
    if (!e || n <= 0) return -2;
    int r = tcp_send(e->c, buf, (u32)n);
    if (r < 0) return -1;
    /* tcp_send 内部分片已保证整段写入（除非超时） */
    return r;
}

void sock_close(int h) {
    sock_ent_t *e = sock_get(h);
    if (!e) return;
    tcp_close(e->c);
    e->used = 0; e->c = 0; e->listen = 0;
}

int sock_closed(int h) {
    sock_ent_t *e = sock_get(h);
    if (!e) return 1;
    return tcp_closed_by_peer(e->c);
}

int sock_readable(int h) {
    sock_ent_t *e = sock_get(h);
    if (!e) return 0;
    net_poll();
    return tcp_recv_avail(e->c) > 0;
}

u32 sock_peer_ip(int h) {
    sock_ent_t *e = sock_get(h);
    if (!e) return 0;
    return tcp_peer_ip(e->c);
}
u32 sock_local_ip(void) { return g_ip; }
int sock_net_up(void) { return g_net_mac[0] != 0; }
