#include "smb.h"
#include "net.h"
#include "vfs.h"
#include "console.h"
#include "mm.h"
#include "pit.h"
#include "libc.h"

#define TSMB_MAX (128u * 1024u)

/* ---------------- 报文编解码 ---------------- */
static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void wr32(u8 *p, u32 v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* 发送请求：返回 0 成功 */
static int send_req(tcp_conn_t *c, u8 op, u16 tid, u16 fid,
                    const char *name, const u8 *data, u32 datalen) {
    u16 nl = name ? (u16)strlen(name) : 0;
    u32 total = 4 + 1 + 2 + 2 + 2 + 4 + nl + datalen;
    u8 *b = (u8*)kmalloc(total);
    if (!b) return -1;
    u32 o = 0;
    memcpy(b, TSMB_MAGIC, 4); o = 4;
    b[o++] = op;
    b[o++] = tid & 0xFF; b[o++] = (tid >> 8) & 0xFF;
    b[o++] = fid & 0xFF; b[o++] = (fid >> 8) & 0xFF;
    b[o++] = nl & 0xFF;  b[o++] = (nl >> 8) & 0xFF;
    wr32(b + o, datalen); o += 4;
    if (nl) { memcpy(b + o, name, nl); o += nl; }
    if (datalen && data) { memcpy(b + o, data, datalen); o += datalen; }
    int r = tcp_send(c, b, total);
    kfree(b);
    return (r > 0) ? 0 : -1;
}

/* 读取响应头，返回 status（<0 表示失败），并给出后续数据长度 */
static int recv_resp(tcp_conn_t *c, u32 *datalen, u32 timeout_ms) {
    u8 h[9];
    if (tcp_read_exact(c, h, 9, timeout_ms) != 9) return -1;
    if (memcmp(h, TSMB_MAGIC, 4) != 0) return -1;
    *datalen = rd32(h + 5);
    return (int)h[4];
}

/* ---------------- 客户端 ---------------- */
static int smb_session(tcp_conn_t *c, const char *share, const char *user,
                       const char *pass, u16 *out_tid, u32 timeout_ms) {
    u32 dl;
    char cred[160];
    snprintf(cred, sizeof(cred), "%s\n%s", user ? user : "", pass ? pass : "");
    if (send_req(c, TSMB_OP_NEGOTIATE, 0, 0, "TinyOS-SMB/1.0", NULL, 0) != 0) return -1;
    if (recv_resp(c, &dl, timeout_ms) != 0) return -1;
    if (send_req(c, TSMB_OP_SESSION, 0, 0, cred, NULL, 0) != 0) return -1;
    if (recv_resp(c, &dl, timeout_ms) != 0) return -1;
    if (send_req(c, TSMB_OP_TREECONN, 0, 0, share ? share : "share", NULL, 0) != 0) return -1;
    if (recv_resp(c, &dl, timeout_ms) != 0) return -1;
    *out_tid = (u16)(dl & 0xFFFF);
    return 0;
}

static int smb_client_open(const char *host, int port, const char *share,
                           const char *user, const char *pass, u16 *tid) {
    u32 ip = ip_parse(host);
    if (port <= 0) port = 445;
    tcp_conn_t *c = tcp_connect(ip, (u16)port);
    if (!c) return -1;
    if (tcp_wait_established(c, 5000) != 0) { tcp_close((tcp_conn_t*)c); return -1; }
    if (smb_session(c, share, user, pass, tid, 4000) != 0) {
        tcp_close((tcp_conn_t*)c);
        return -1;
    }
    return (int)(size_t)c;
}

int smb_put(const char *host, int port, const char *share, const char *user,
            const char *pass, const char *local, const char *remote) {
    u32 sz = 0;
    const u8 *data = vfs_read_file(local, &sz);
    if (!data) { kprintf("smb: local file not found: %s\n", local); return -1; }
    if (sz > TSMB_MAX) { kprintf("smb: file too large\n"); return -1; }

    u16 tid = 0;
    int ch = smb_client_open(host, port, share, user, pass, &tid);
    if (ch < 0) { kprintf("smb: session/tree connect failed\n"); return -1; }
    tcp_conn_t *c = (tcp_conn_t*)(size_t)ch;

    if (send_req(c, TSMB_OP_PUT, tid, 0, remote, data, sz) != 0) { tcp_close(c); return -1; }
    u32 dl;
    int st = recv_resp(c, &dl, 6000);
    send_req(c, TSMB_OP_TREEDISC, tid, 0, NULL, NULL, 0); recv_resp(c, &dl, 2000);
    send_req(c, TSMB_OP_LOGOFF, 0, 0, NULL, NULL, 0); recv_resp(c, &dl, 2000);
    tcp_close(c);
    return (st == 0) ? 0 : -1;
}

int smb_get(const char *host, int port, const char *share, const char *user,
            const char *pass, const char *remote, const char *local) {
    u16 tid = 0;
    int ch = smb_client_open(host, port, share, user, pass, &tid);
    if (ch < 0) { kprintf("smb: session/tree connect failed\n"); return -1; }
    tcp_conn_t *c = (tcp_conn_t*)(size_t)ch;

    if (send_req(c, TSMB_OP_GET, tid, 0, remote, NULL, 0) != 0) { tcp_close(c); return -1; }
    u32 dl;
    int st = recv_resp(c, &dl, 6000);
    if (st != 0 || dl == 0 || dl > TSMB_MAX) {
        send_req(c, TSMB_OP_TREEDISC, tid, 0, NULL, NULL, 0); recv_resp(c, &dl, 2000);
        tcp_close(c);
        return -1;
    }
    u8 *buf = (u8*)kmalloc(dl);
    if (!buf) { tcp_close(c); return -1; }
    int got = tcp_read_exact(c, buf, dl, 6000);
    if (got > 0) vfs_write_file(local, buf, (u32)got);
    kfree(buf);
    send_req(c, TSMB_OP_TREEDISC, tid, 0, NULL, NULL, 0); recv_resp(c, &dl, 2000);
    tcp_close(c);
    return (got == (int)dl) ? 0 : -1;
}

/* ---------------- 服务端 ---------------- */
static tcp_conn_t *g_smb_listener = NULL;

static void smb_handle(tcp_conn_t *c) {
    u16 tid = 0x0001;
    char cur_share[64] = "share";
    for (;;) {
        u8 h[15];
        if (tcp_read_exact(c, h, 15, 30000) != 15) return;
        if (memcmp(h, TSMB_MAGIC, 4) != 0) return;
        u8  op   = h[4];
        u16 rtid = (u16)(h[5] | (h[6] << 8));
        u16 nl   = (u16)(h[9] | (h[10] << 8));
        u32 dl   = rd32(h + 11);
        char name[256]; name[0] = 0;
        if (nl) {
            if (nl > 255) return;
            if (tcp_read_exact(c, (u8*)name, nl, 5000) != (int)nl) return;
            name[nl] = 0;
        }
        u8 *data = NULL;
        if (dl) {
            if (dl > TSMB_MAX) return;
            data = (u8*)kmalloc(dl);
            if (!data) return;
            if (tcp_read_exact(c, data, dl, 8000) != (int)dl) { kfree(data); return; }
        }

        u8 resp[9];
        memcpy(resp, TSMB_MAGIC, 4);
        resp[4] = 0;
        wr32(resp + 5, 0);

        switch (op) {
        case TSMB_OP_NEGOTIATE:
            tcp_send(c, resp, 9);
            break;
        case TSMB_OP_SESSION:
            tcp_send(c, resp, 9);
            break;
        case TSMB_OP_TREECONN:
            strncpy(cur_share, name[0] ? name : "share", 63);
            wr32(resp + 5, tid);           /* 响应数据 = tid */
            tcp_send(c, resp, 9);
            break;
        case TSMB_OP_PUT:
            if (vfs_write_file(name, data, dl) == 0) { resp[4] = 0; }
            else resp[4] = 0x02;
            tcp_send(c, resp, 9);
            break;
        case TSMB_OP_GET: {
            u32 sz = 0;
            const u8 *f = vfs_read_file(name, &sz);
            if (!f) { resp[4] = 0x03; tcp_send(c, resp, 9); break; }
            wr32(resp + 5, sz);
            tcp_send(c, resp, 9);
            if (sz) { tcp_send(c, f, sz); }
            break;
        }
        case TSMB_OP_LIST: {
            char out[256];
            snprintf(out, sizeof(out), "share=%s  (TinyOS in-memory filesystem root)\r\n", cur_share);
            u32 l = (u32)strlen(out);
            wr32(resp + 5, l);
            tcp_send(c, resp, 9);
            tcp_send(c, (const u8*)out, l);
            break;
        }
        case TSMB_OP_DELETE:
            resp[4] = (vfs_delete(name) == 0) ? 0 : 0x04;
            tcp_send(c, resp, 9);
            break;
        case TSMB_OP_TREEDISC:
        case TSMB_OP_LOGOFF:
            tcp_send(c, resp, 9);
            kfree(data);
            return;
        default:
            resp[4] = 0xFF;
            tcp_send(c, resp, 9);
            break;
        }
        kfree(data);
        (void)rtid;
    }
}

int smb_server_start(int port) {
    if (g_smb_listener) return 0;
    g_smb_listener = tcp_listen((u16)(port > 0 ? port : 445));
    return g_smb_listener ? 0 : -1;
}
int smb_server_active(void) { return g_smb_listener != NULL; }
void smb_server_poll(void) {
    if (!g_smb_listener) return;
    tcp_conn_t *c = tcp_accept(g_smb_listener);
    if (c) { smb_handle(c); tcp_close(c); }
}
