/* ============================================================================
 * http.c -- 最小 HTTP/1.1 客户端（只支持 GET）+ HTTP CONNECT 代理
 * ----------------------------------------------------------------------------
 * 用途：让用户态的 pkg 直接从 raw.githubusercontent.com 拉软件包，
 *       不需要用户先把仓库克隆到本地。
 *
 * 【重要更正：明文 HTTP 走不通】
 *   本文件早先的注释断言"raw.githubusercontent.com 的明文端点可访问，
 *   包完整性由 SHA256 保证，因此不需要传输层加密"。**那个结论是错的**，
 *   它来自一次被宿主代理污染的测试。实测直连 GitHub 的真实 IP：
 *
 *     GET /... Host: raw.githubusercontent.com        (明文 80)
 *     -> HTTP 301, Location: https://raw.githubusercontent.com/...
 *
 *   GitHub 已全面强制 HTTPS，明文一律 301 跳转。所以 pkg 走 http:// 拿不到
 *   包。这不是实现问题，是服务端策略。
 *
 * 两条可行路径：
 *   1. 走 HTTPS 代理（本文件支持的 CONNECT 隧道）。代理终结 TLS，
 *      TinyOS 只发明文 HTTP 给代理 —— 这是常见的企业网络形态。
 *   2. 直连 HTTPS，需要内核长出完整 TLS 栈（见下）。
 *
 * 【关于 TLS —— 明确未实现，不是遗漏】
 *   内核只有 SHA-256（src/kernel/sha256.c），**没有** AES / GCM /
 *   ChaCha20 / X25519 / Poly1305，也**没有** X.509 解析与主机名校验。
 *   一套能用的 TLS 1.3 客户端约需 1500+ 行裸机密码学代码，而且
 *   **必须**校验证书链到内置根 CA 并核对主机名，否则等于给中间人开门。
 *   pkg 用来下载并执行代码，这个洞的后果比明文更严重。
 *   所以本文件选择：CONNECT 隧道可用；直连 HTTPS 明确返回
 *   HTTP_E_NOTLS，并在 pkg 输出与官网里如实标注。
 *   **绝不提供"跳过证书验证"的伪 TLS。**
 *
 * 复用内核已有的网络栈，不重写任何底层：
 *   host_to_ip()          域名解析（自动区分 IP 字面量与域名）
 *   tcp_connect/tcp_send/tcp_readline/tcp_wait_established
 *
 * 错误码（都取负值，便于 tinysh 打印有意义的信息）：
 *   -1 参数错误   -2 DNS 失败      -3 TCP 连接失败   -4 连接超时
 *   -5 发送失败   -6 响应读取失败 -7 非 200 状态    -8 响应头过大
 *   -9 body 过大  -10 状态行异常  -11 内存不足     -12 写文件失败
 *   -13 代理拒绝 CONNECT       -14 需要 TLS 但内核尚未实现
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
#define HTTP_E_PROXY   -13   /* 代理拒绝了 CONNECT（非 200）*/
#define HTTP_E_NOTLS   -14   /* 需要 TLS，但内核尚未实现 TLS 栈*/

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

/* ---------------------------------------------------------------------------
 * 代理支持
 * ------------------------------------------------------------------------ */

/* 通过 HTTP 代理为 (host, port) 建立 CONNECT 隧道。
 *
 *   proxy_host/proxy_port  代理地址（域名或 IP 都行）
 *   host/port              目标地址
 *
 * 流程：解析代理 -> TCP 连代理 -> 发 CONNECT -> 校验响应必须是 200。
 * 成功后这条连接上传输的是**目标 TLS 密文**，不要在它上面发普通 HTTP。
 *
 * 代理不可用或拒绝时返回 HTTP_E_CONN / HTTP_E_PROXY。
 * 隧道建立成功但内核没有 TLS 栈时返回 HTTP_E_NOTLS —— 这是当前的实际
 * 情况，如实上报而不是假装加密。
 */
int http_connect_tunnel(const char *proxy_host, int proxy_port,
                        const char *host, int port);

/* 走代理的 GET。代理参数为空时退化为 http_get（明文直连）。
 * 语义与 http_get 完全一致。 */
int http_get_proxy(const char *host, int port, const char *path,
                   const char *proxy_host, int proxy_port,
                   void *buf, int max, int *out_len);

/* 走代理的分块下载（语义与 http_get_file 一致）。 */
int http_get_file_proxy(const char *host, int port, const char *path,
                        const char *local,
                        const char *proxy_host, int proxy_port);

#endif
