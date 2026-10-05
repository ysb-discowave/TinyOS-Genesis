/* ============================================================
 * http.c — 最小 HTTP/1.1 客户端（仅 GET）+ HTTP CONNECT 代理
 * ------------------------------------------------------------
 * 复用内核已有的 DNS（dns_resolve）与 TCP 栈（tcp_connect 等），
 * 不重写底层网络。照 ftp.c 的"主动连接外网"方式写。
 *
 * 【更正】早先这里写着"实测 raw.githubusercontent.com 明文可用" ——
 * 那是宿主代理造成的假象。直连 GitHub 真实 IP 测得的是 301 跳 HTTPS，
 * 明文拿不到包。详见 include/http.h 的详细说明。
 *
 * 代理路径：CONNECT 隧道可用；隧道之后需要 TLS，而内核没有 TLS 栈，
 * 因此直连 HTTPS 明确返回 HTTP_E_NOTLS，不做"跳过证书验证"的假 TLS。
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
        case HTTP_E_PROXY:  return "proxy refused the CONNECT tunnel";
        case HTTP_E_NOTLS:  return "https needs TLS, which the kernel does not implement yet";
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

/* http_head_len 已移除：旧实现用 CONNECT 探测长度对普通 HTTP 服务器无效，
 * 且 Cloudflare 对大文件常走 chunked、不返回 Content-Length。
 * http_get_file 现改为单连接 GET（Connection: close）读到 EOF 即完整 body。 */

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

    /* 单连接、无 Range 的 GET（Connection: close）。服务端在我们发
       Connection: close 后会主动关闭连接，内核读到 EOF 即可拿到完整 body，
       不再依赖响应里的 Content-Length —— Cloudflare 对大文件常走 chunked，
       并不总返回 Content-Length，旧的 http_head_len 因此总是拿不到长度。
       上限 256 KiB 覆盖当前最大包（LUA 约 210 KiB）并留有余量。 */
    const int MAXDL = 262144;   /* 256 KiB */
    u8 *full = (u8 *)kmalloc((u32)MAXDL);
    if (!full) return HTTP_E_NOMEM;

    tcp_conn_t *c = tcp_connect(ip, (u16)port);
    if (!c) { kfree(full); return HTTP_E_CONN; }
    if (tcp_wait_established(c, 5000) != 0) { tcp_close(c); kfree(full); return HTTP_E_TIMEOUT; }

    int got = 0;
    int rc = http_do(c, host, path, 0, 0, full, MAXDL, &got);
    tcp_close(c);
    if (rc == 0) {
        if (got <= 0) rc = HTTP_E_RECV;
        else if (vfs_write_file(local, full, (u32)got) != 0) rc = HTTP_E_WRITE;
    }
    kfree(full);
    return rc;
}

/* ==========================================================================
 * HTTP CONNECT 代理
 * ========================================================================== */

/* 读一条响应头（到 

 为止）并解析状态码。
 * 成功返回 0 且 *code 收到状态码；失败返回 HTTP_E_*。 */
static int read_status(tcp_conn_t *c, u8 *buf, int max, int *code) {
    int total = 0;
    u32 waited = 0;
    for (;;) {
        int n = tcp_recv(c, buf + total, (u32)(max - total));
        if (n > 0) {
            total += n; waited = 0;
            if (total >= max) return HTTP_E_HDRBIG;
            /* 头结束符 */
            for (int i = 3; i < total; i++)
                if (buf[i-3]=='\r' && buf[i-2]=='\n' && buf[i-1]=='\r' && buf[i]=='\n') {
                    int sp = 0;
                    while (sp < i && buf[sp] != ' ') sp++;
                    if (sp + 3 >= i) return HTTP_E_BADST;
                    *code = (buf[sp+1]-'0')*100 + (buf[sp+2]-'0')*10 + (buf[sp+3]-'0');
                    return 0;
                }
            continue;
        }
        if (tcp_closed_by_peer(c)) return HTTP_E_RECV;
        net_poll(); pit_sleep(1);
        if (++waited >= 5000) return HTTP_E_TIMEOUT;
    }
}

int http_connect_tunnel(const char *proxy_host, int proxy_port,
                        const char *host, int port) {
    if (!proxy_host || !proxy_host[0] || !host) return HTTP_E_INVAL;
    if (proxy_port <= 0) proxy_port = 8080;
    if (port <= 0) port = 443;

    u32 ip = host_to_ip(proxy_host);
    if (ip == 0) return HTTP_E_DNS;

    tcp_conn_t *c = tcp_connect(ip, (u16)proxy_port);
    if (!c) return HTTP_E_CONN;
    if (tcp_wait_established(c, 5000) != 0) { tcp_close(c); return HTTP_E_TIMEOUT; }

    char req[256];
    int rl = snprintf(req, sizeof req,
        "CONNECT %s:%d HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: %s\r\n"
        "Proxy-Connection: keep-alive\r\n"
        "\r\n", host, port, host, port, HTTP_UA);
    if (rl <= 0 || rl >= (int)sizeof req) { tcp_close(c); return HTTP_E_INVAL; }
    if (tcp_send(c, (const u8 *)req, (u32)rl) != rl) { tcp_close(c); return HTTP_E_SEND; }

    u8 hdr[512];
    int code = 0;
    int r = read_status(c, hdr, (int)sizeof hdr, &code);
    if (r != 0) { tcp_close(c); return r; }
    if (code != 200) { tcp_close(c); return HTTP_E_PROXY; }

    /* 隧道已建立，但接下来是 TLS 密文 —— 内核没有 TLS 栈。
     * 如实返回明确错误，绝不"跳过证书验证"假装加密。 */
    tcp_close(c);
    return HTTP_E_NOTLS;
}

int http_get_proxy(const char *host, int port, const char *path,
                   const char *proxy_host, int proxy_port,
                   void *buf, int max, int *out_len) {
    /* 没配代理就退化成原来的明文直连，行为不变（向后兼容）。 */
    if (!proxy_host || !proxy_host[0])
        return http_get(host, port, path, buf, max, out_len);
    return HTTP_E_NOTLS;
}

int http_get_file_proxy(const char *host, int port, const char *path,
                        const char *local,
                        const char *proxy_host, int proxy_port) {
    if (!proxy_host || !proxy_host[0])
        return http_get_file(host, port, path, local);
    return HTTP_E_NOTLS;
}
