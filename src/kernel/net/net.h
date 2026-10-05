#ifndef TINYOS_NET_H
#define TINYOS_NET_H

#include "types.h"

#define ETH_TYPE_ARP 0x0806
#define ETH_TYPE_IP  0x0800
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

/* 网络配置（QEMU user-net 默认 guest 网段 10.0.2.0/24，网关 10.0.2.2） */
extern u32 g_ip;          /* 本机 IP（主机序） */
extern u32 g_gw;          /* 网关 IP（主机序） */
extern u32 g_dns;         /* DNS 服务器（主机序，默认 10.0.2.3 = QEMU SLIRP 内置转发） */
extern u8  g_net_mac[6];  /* 本机 MAC */

void net_init(void);
int  net_up(void);
void net_poll(void);      /* 处理接收队列并分派（协作式轮询） */
void net_poll_all(void);  /* net_poll + 已注册的网络服务（FTP/SMB） */
void net_info(void);      /* 打印网卡/IP/ARP 状态 */

/* 以太网发送 */
void eth_send(const u8 dst[6], u16 ethertype, const u8 *payload, u32 len);
/* ARP：返回 1 表示已解析并填入 mac；否则发请求并返回 0 */
int  arp_resolve(u32 ip, u8 mac[6]);
/* 阻塞式 ARP（内部轮询 net_poll，最多等待 timeout_ms） */
int  arp_resolve_wait(u32 ip, u8 mac[6], u32 timeout_ms);
/* IPv4 发送：返回 0 成功，-1 表示 ARP 未就绪（应重试） */
int  ip_send(u32 dst, u8 proto, const u8 *payload, u32 len);

/* UDP */
int  udp_bind(u16 port, void (*cb)(u32 src_ip, u16 src_port, const u8 *data, u32 len));
void udp_send(u32 dst, u16 sport, u16 dport, const u8 *data, u32 len);
void udp_dispatch(u32 src, u16 sport, u16 dport, const u8 *data, u32 len);
void udp_unbind(u16 port);

/* 最小 TCP（链路可靠假设：QEMU 本地以太网无丢包） */
typedef struct tcp_conn tcp_conn_t;
tcp_conn_t *tcp_connect(u32 ip, u16 port);
tcp_conn_t *tcp_listen(u16 port);
tcp_conn_t *tcp_accept(tcp_conn_t *listener);              /* 非阻塞 */
tcp_conn_t *tcp_accept_wait(tcp_conn_t *listener, u32 timeout_ms);
int  tcp_established(tcp_conn_t *c);
int  tcp_wait_established(tcp_conn_t *c, u32 timeout_ms);
int  tcp_send(tcp_conn_t *c, const u8 *data, u32 len);
int  tcp_recv(tcp_conn_t *c, u8 *buf, u32 max);           /* 返回已可读字节数（0 无） */
int  tcp_read_exact(tcp_conn_t *c, u8 *buf, u32 n, u32 timeout_ms);
void tcp_close(tcp_conn_t *c);
/* 关闭并摘除监听端口 lport 上尚未被 accept 的子连接（清理 PASV 残连，防 TCB 泄漏） */
void tcp_drop_pending(u16 lport);
void tcp_input(u32 src, const u8 *seg, u32 len);
/* 对端是否已关闭（收到 FIN） */
int  tcp_closed_by_peer(tcp_conn_t *c);
/* 本端发出的数据是否全部被确认 */
int  tcp_sent_all_acked(tcp_conn_t *c);
/* 接收缓冲里当前可读字节数（不消费） */
int  tcp_recv_avail(tcp_conn_t *c);
/* 对端 IP（主机序）；无连接返回 0 */
u32  tcp_peer_ip(tcp_conn_t *c);

/* 文本协议辅助 */
int  tcp_readline(tcp_conn_t *c, char *buf, u32 max, u32 timeout_ms);
void tcp_writeline(tcp_conn_t *c, const char *s);
void tcp_writestr(tcp_conn_t *c, const char *s);

/* 工具 */
u32  ip_parse(const char *s);      /* 点分十进制 -> 主机序 u32 */
void ip_to_str(u32 ip, char *out); /* 主机序 u32 -> 点分十进制 */
int  net_ping(u32 ip);             /* ICMP Echo，0 = 收到应答 */
int  dns_resolve(const char *name, u32 *out_ip); /* 域名解析，0 = 成功 */
u32  host_to_ip(const char *s);    /* 自动区分 IP/主机名：纯数字点分走 ip_parse，否则走 DNS */
int  net_tcping(u32 ip, u16 port, u32 timeout_ms); /* TCP 连通性探测，0 = 连通（SLIRP 下比 ICMP 可靠） */

#endif
