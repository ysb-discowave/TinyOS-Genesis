#ifndef TINYOS_SOCK_H
#define TINYOS_SOCK_H

#include "types.h"

/* ============================================================
 * 用户态可用的 TCP 套接字（句柄表）
 * ------------------------------------------------------------
 * 为什么需要这一层：内核的 tcp_conn_t 是内部类型，而 TNCR 用户程序
 * （比如 SSH 服务端）需要"监听 / 接受 / 收发"这几个动作。这里把
 * tcp_conn_t* 包成小整数句柄，并把"等数据"变成一次带超时的阻塞调用
 * （内部驱动 net_poll()，因为内核是协作式单线程）。
 *
 * 线程模型：所有调用都在内核线程上下文里同步完成，不需要锁。
 * 限制：最多 SOCK_MAX 个句柄（内核只有 16 个 TCB，且 FTP/SMB 也要用）。
 * ============================================================ */

#define SOCK_MAX 6

/* 监听 port（主机序）。返回 handle>=0，失败 <0。 */
int  sock_listen(int port);
/* 主动连接 ip:port（主机序）。复用 tcp_connect + tcp_wait_established，
 * 完成三次握手后返回一个普通（非监听）句柄；失败 <0。
 * timeout_ms 为握手等待上限（毫秒），<0 表示用 8000ms 默认。 */
int  sock_connect(u32 ip, u16 port, int timeout_ms);
/* 等待并接受一个连接。timeout_ms<0 表示无限等待。返回 handle，-1 超时。 */
int  sock_accept(int lh, int timeout_ms);
/* 读取数据。返回 >0 字节数、0 超时、-1 对端已关闭、-2 句柄无效。 */
int  sock_recv(int h, u8 *buf, int max, int timeout_ms);
/* 非阻塞读取：有多少就读多少（可能返回 0）。 */
int  sock_poll_recv(int h, u8 *buf, int max);
/* 发送全部数据（内部会等待窗口并按需 net_poll）。返回实际发出字节数。 */
int  sock_send(int h, const u8 *buf, int n);
/* 关闭句柄（对已建立的连接发 FIN）。 */
void sock_close(int h);
/* 对端是否已关闭。 */
int  sock_closed(int h);
/* 是否有可读数据（不消费）。 */
int  sock_readable(int h);
/* 对端 IP（主机序），无连接返回 0。 */
u32  sock_peer_ip(int h);
/* 本机 IP（主机序）。 */
u32  sock_local_ip(void);
/* 是否已有网卡。 */
int  sock_net_up(void);

#endif
