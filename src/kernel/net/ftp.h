#ifndef TINYOS_FTP_H
#define TINYOS_FTP_H

#include "types.h"

/* ---------- 客户端（TinyOS → 远端 FTP 服务器，使用 PASV 被动模式） ---------- */
int ftp_get(const char *host, int port, const char *user, const char *pass,
            const char *remote, const char *local);   /* 下载：远端 -> 本地 VFS */
int ftp_put(const char *host, int port, const char *user, const char *pass,
            const char *local, const char *remote);   /* 上传：本地 VFS -> 远端 */

/* ---------- 服务端（其他机器 / 宿主可连入 TinyOS） ---------- */
int  ftp_server_start(int port);   /* 0 = 成功，监听控制端口 */
void ftp_server_poll(void);        /* 由主循环轮询调用 */
int  ftp_server_active(void);

#endif
