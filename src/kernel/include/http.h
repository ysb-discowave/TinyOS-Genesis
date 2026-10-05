/* ============================================================================
 * http.c -- 最小 HTTP/1.1 客户端（只支持 GET）
 * ----------------------------------------------------------------------------
 * 用途：让用户态的 pkg 直接从 raw.githubusercontent.com 拉软件包，
 *       不需要用户先把仓库克隆到本地。
 *
 * 为什么是明文 HTTP 而不是 HTTPS：
 *   内核没有任何加密栈（没有 TLS/AES/X25519），实现 TLS 意味着 1500+ 行
 *   裸机密码学代码，安全风险极高。而 raw.githubusercontent.com 的明文
 *   HTTP 端点是可访问的，**包完整性由 SHA256 保证，不依赖传输层加密**——
 *   这正是包管理器该用的模型：不可信的链路 + 可信的校验和。
 *
 * 复用内核已有的网络栈，不重写任何底层：
 *   dns_resolve()      域名解析（net.h 公开 API）
 *   tcp_connect/tcp_wait_established/tcp_send/tcp_readline/tcp_read_exact
 *   ip_parse()         纯 IP 字面量
 *
 * 错误码（都取负值，便于 tinysh 打印有意义的信息）：
 *   -1 参数错误   -2 DNS 失败      -3 TCP 连接失败   -4 连接超时
 *   -5 发送失败   -6 响应读取失败 -7 非 200 状态    -8 响应头过大
 *   -9 body 过大  -10 状态行异常
 * ========================================================================== */
#ifndef TINYOS_HTTP_H
#define TINYOS_HTTP_H

#include "types.h"

/* HTTP 错误码（负值） */
#define HTTP_E_INVAL   -1    /* 参数错误 */
#define HTTP_E_DNS     -2    /* 域名解析失败 */
#define HTTP_E_CONN    -3    /* TCP 连接失败 */
#define HTTP_E_TIMEOUT -4    /* 连接/读取超时 */
#define HTTP_E_SEND    -5    /* 发送失败 */
#define HTTP_E_RECV    -6    /* 读取响应失败 */
#define HTTP_E_STATUS  -7    /* HTTP 状态码非 200 */
#define HTTP_E_HDRBIG  -8    /* 响应头过大 */
#define HTTP_E_BODYBIG -9    /* body 超出调用者给的 max */
#define HTTP_E_BADST   -10   /* 状态行格式异常 */
#define HTTP_E_NOMEM   -11   /* 内存不足（无法分配下载缓冲）*/
#define HTTP_E_WRITE   -12   /* 写本地文件失败 */

/* 把错误码翻成一句英文说明（供用户态直接打印）。
 * 返回值是字符串常量，不要释放。 */
const char *http_strerror(int code);

/* 发起一次 HTTP GET。
 *
 *   host      域名或点分十进制 IP
 *   port      端口（<=0 时用 80）
 *   path      以 '/' 开头的请求路径，例如
 *             /ysb-discowave/TinyOS-Genesis/main/packages/repo/tinysh.TNCR
 *   buf       接收 body 的缓冲区
 *   max       buf 容量；body 超过它就返回 HTTP_E_BODYBIG（不截断，
 *             截断的包会被 sha256 判定为损坏，截断没有意义）
 *   out_len   实际 body 长度（可为 NULL）
 *
 * 返回 0 成功，负值见上面的错误码。
 *
 * 每次调用都是独立连接：Connection: close，不做 keep-alive。
 * 这一点是刻意的 —— 内核是协作式单线程，keep-alive 会占住连接，
 * 导致其它请求饿死。
 */
int http_get(const char *host, int port, const char *path,
             void *buf, int max, int *out_len);

/* 下载一个文件到 TinyOS 的 VFS（自动分块，适合上百 KB 的包）。
 *
 * 先用一次请求读 Content-Length，然后按 16KB 一块、带 Range 头逐块拉取，
 * 最后 vfs_write_file 落盘。TNCR 程序动辄上百 KB（CC.TNCR 162KB、
 * LUA.TNCR 210KB），单次 GET 装不进调用者的栈缓冲，所以走这条路。
 *
 *   local  目标路径，例如 "/tmp/pkg.tncr"
 *
 * 返回 0 成功，负值见上面的错误码。失败时不保证 local 被删除，
 * 调用者应负责清理。
 */
int http_get_file(const char *host, int port, const char *path,
                  const char *local);

#endif
