/* ============================================================
 * http.c — 最小 HTTP/1.1 客户端（仅 GET，明文，无 TLS）
 * ------------------------------------------------------------
 * 复用内核已有的 DNS（dns_resolve）与 TCP 栈（tcp_connect 等），
 * 不重写底层网络。照 ftp.c 的"主动连接外网"方式写。
 *
 * 实测前提：raw.githubusercontent.com 明文 HTTP 可用，返回 200 且
 * Content-Length 正确，因此本文件不实现 TLS。
 * ============================================================ */
#include "net.h"
#include "http.h"
#include "vfs.h"
#include "libc.h"
#include "mm.h"
#include "pit.h"

#define HTTP_UA  "TinyOS-Genesis/0.1"
#define HTTP_PORT 80

const char *http_strerror(int code) {
    switch (code) {
        case 0:             return "ok";
        case HTTP_E_INVAL:  return "invalid argument";
        case HTTP_E_DNS:    return "dns resolve failed";
        case HTTP_E_CONN:   return "tcp connect failed";
        case HTTP_E_TIMEOUT:return "timed out";
        case HTTP_E_SEND:   return "send failed";
        case HTTP_E_RECV:   return "failed to read response";
        case HTTP_E_STATUS: return "server returned an error status";
        case HTTP_E_HDRBIG: return "response headers too large";
        case HTTP_E_BODYBIG:return "body larger than the buffer";
        case HTTP_E_BADST:  return "malformed status line";
        case HTTP_E_NOMEM:  return "out of memory";
        case HTTP_E_WRITE:  return "failed to write the local file";
        default:            return "unknown error";
    }
}

/* 不区分大小写匹配头字段名（例如 "content-length:"），返回冒号后第一个非空白字符的下标，
 * 找不到返回 -1。h/hlen 是待搜索的头文本（不含 body）。 */
static int hdr_find(const u8 *h, int hlen, const char *key) {
    int kl = (int)strlen(key);
    for (int i = 0; i + kl < hlen; i++) {
        int ok = 1;
        for (int k = 0; k < kl; k++) {
            char a = (char)h[i + k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != key[k]) { ok = 0; break; }
        }
        if (ok) {
            int p = i + kl;
            while (p < hlen && (h[p] == ' ' || h[p] == '\t')) p++;
            return p;
        }
    }
    return -1;
}

/* 从头文本解析 Content-Length（十进制）。找不到返回 0。 */
static long hdr_content_length(const u8 *h, int hlen) {
    int p = hdr_find(h, hlen, "content-length:");
    if (p < 0) return 0;
    long v = 0; int any = 0;
    while (p < hlen && h[p] >= '0' && h[p] <= '9') { v = v * 10 + (h[p] - '0'); p++; any = 1; }
    return any ? v : 0;
}

/* 在一个已建立的连接上发送一次 GET（可带 Range）并读取响应。
 * out/max：调用方提供的接收缓冲（头+body 都先收在这里，再裁掉头）。
 * off/rlen：分块下载参数。rlen<=0 表示整文件 GET；否则发
 *   "Range: bytes=off-(off+rlen-1)"。
 * 返回 0 且 *out_len = body 字节数；否则负数错误码。 */
static int http_do(tcp_conn_t *c, const char *host, const char *path,
                   long off, long rlen, u8 *out, int max, int *out_len) {
    char req[512];
    int rl;
    if (rlen > 0) {
        rl = snprintf(req, sizeof req,
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "Range: bytes=%ld-%ld\r\n\r\n",
            path, host, HTTP_UA, off, off + rlen - 1);
    } else {
        rl = snprintf(req, sizeof req,
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n\r\n",
            path, host, HTTP_UA);
    }
    if (rl <= 0 || rl >= (int)sizeof req) return HTTP_E_SEND;
    if (tcp_send(c, (const u8 *)req, (u32)rl) != rl) return HTTP_E_SEND;

    /* 收响应：头 + body 一起收进 out，直到找到 \r\n\r\n、缓冲收满、或超时。 */
    int total = 0;
    u32 waited = 0;
    for (;;) {
        int n = tcp_recv(c, out + total, (u32)(max - total));
        if (n > 0) { total += n; waited = 0; if (total >= max) break; continue; }
        if (tcp_closed_by_peer(c)) {
            net_poll();
            int n2 = tcp_recv(c, out + total, (u32)(max - total));
            if (n2 > 0) { total += n2; waited = 0; }
            break;
        }
        net_poll(); pit_sleep(1);
        if (++waited >= 8000) break;   /* ~8s 收尾超时 */
    }
    if (total == 0) return HTTP_E_RECV;

    /* 找头结束符 \r\n\r\n */
    int hdr_end = -1;
    for (int i = 3; i < total; i++)
        if (out[i-3]=='\r' && out[i-2]=='\n' && out[i-1]=='\r' && out[i]=='\n') {
            hdr_end = i + 1; break;
        }
    if (hdr_end < 0) return HTTP_E_BADST;

    /* 解析状态行 HTTP/1.x NNN */
    if (total < 12 || out[0]!='H' || out[1]!='T' || out[2]!='T' || out[3]!='P')
        return HTTP_E_BADST;
    int sp = 0; while (sp < hdr_end && out[sp] != ' ') sp++;
    if (sp > hdr_end - 4) return HTTP_E_BADST;
    if (!(out[sp+1]>='0'&&out[sp+1]<='9'&&out[sp+2]>='0'&&out[sp+2]<='9'&&out[sp+3]>='0'&&out[sp+3]<='9'))
        return HTTP_E_BADST;
    int code = (out[sp+1]-'0')*100 + (out[sp+2]-'0')*10 + (out[sp+3]-'0');

    if (rlen > 0) { if (code != 200 && code != 206) return HTTP_E_STATUS; }
    else          { if (code != 200) return HTTP_E_STATUS; }

    /* 整文件 GET 且响应体超过调用方缓冲：直接报错，提示用 http_get_file */
    if (rlen <= 0) {
        long cl = hdr_content_length(out, hdr_end);
        if (cl > 0 && cl + hdr_end > max) return HTTP_E_BODYBIG;
    }

    int body_len = total - hdr_end;
    memmove(out, out + hdr_end, body_len);
    *out_len = body_len;
    return 0;
}

/* 连接并仅读取响应头，返回 Content-Length（>0）。失败返回 0（GH raw 必有）*/
static long http_head_len(u32 ip, int port, const char *host, const char *path) {
    tcp_conn_t *c = tcp_connect(ip, (u16)port);
    if (!c) return 0;
    if (tcp_wait_established(c, 5000) != 0) { tcp_close(c); return 0; }
    char req[512];
    int rl = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
        "Accept: */*\r\nConnection: close\r\n\r\n", path, host, HTTP_UA);
    if (rl <= 0 || rl >= (int)sizeof req || tcp_send(c, (const u8 *)req, (u32)rl) != rl) {
        tcp_close(c); return 0;
    }
    u8 hbuf[2048];
    int htot = 0, hdr_end = -1;
    u32 waited = 0;
    while (htot < (int)sizeof hbuf) {
        int n = tcp_recv(c, hbuf + htot, (u32)(sizeof hbuf - htot));
        if (n > 0) {
            htot += n;
            for (int i = 3; i < htot; i++)
                if (hbuf[i-3]=='\r' && hbuf[i-2]=='\n' && hbuf[i-1]=='\r' && hbuf[i]=='\n') {
                    hdr_end = i + 1; break;
                }
        }
        if (hdr_end >= 0) break;
        if (tcp_closed_by_peer(c)) break;
        net_poll(); pit_sleep(1);
        if (++waited >= 3000) break;
    }
    tcp_close(c);
    if (hdr_end < 0) return 0;
    return hdr_content_length(hbuf, hdr_end);
}

int http_get(const char *host, int port, const char *path,
             void *buf, int max, int *out_len) {
    if (!host || !path || !buf || !out_len || port <= 0 || max <= 0) return HTTP_E_INVAL;
    u32 ip = 0;
    if (dns_resolve(host, &ip) != 0) return HTTP_E_DNS;
    tcp_conn_t *c = tcp_connect(ip, (u16)port);
    if (!c) return HTTP_E_CONN;
    if (tcp_wait_established(c, 5000) != 0) { tcp_close(c); return HTTP_E_TIMEOUT; }
    int r = http_do(c, host, path, 0, 0, (u8 *)buf, max, out_len);
    tcp_close(c);
    return r;
}

int http_get_file(const char *host, int port, const char *path, const char *local) {
    if (!host || !path || !local || port <= 0) return HTTP_E_INVAL;
    u32 ip = 0;
    if (dns_resolve(host, &ip) != 0) return HTTP_E_DNS;

    long total = http_head_len(ip, port, host, path);
    if (total <= 0) return HTTP_E_BADST;

    u8 *full = (u8 *)kmalloc((u32)total);
    if (!full) return HTTP_E_NOMEM;

    const int CHUNK = 16384;   /* 每块 16KB；TCB 接收窗 32KB，留有余量 */
    long off = 0;
    int rc = 0;
    while (off < total) {
        long want = (total - off < CHUNK) ? (total - off) : CHUNK;
        tcp_conn_t *c = tcp_connect(ip, (u16)port);
        if (!c) { rc = HTTP_E_CONN; break; }
        if (tcp_wait_established(c, 5000) != 0) { tcp_close(c); rc = HTTP_E_TIMEOUT; break; }
        int got = 0;
        int r = http_do(c, host, path, off, want, full + off, (int)want, &got);
        tcp_close(c);
        if (r != 0) { rc = r; break; }
        off += got;
        if (got < want) break;   /* 服务端给少了，按实际推进后退出 */
    }
    if (rc == 0 && off != total) rc = HTTP_E_RECV;
    if (rc == 0) {
        if (vfs_write_file(local, full, (u32)total) != 0) rc = HTTP_E_WRITE;
    }
    kfree(full);
    return rc;
}
