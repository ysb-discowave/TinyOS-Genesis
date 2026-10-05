#ifndef TINYOS_SMB_H
#define TINYOS_SMB_H

#include "types.h"

/* ============================================================
 * TinySMB —— 简化的 SMB 风格文件共享协议（TCP 445）
 * ------------------------------------------------------------
 * 保留 SMB 的核心语义：协商(Negotiate) → 会话(Session Setup)
 *                     → 树连接(Tree Connect) → 文件操作 → 断开
 * 报文结构（多字节整数字节序为小端）：
 *   请求: magic"TSMB" | op(1) | tid(2) | fid(2) | namelen(2) | datalen(4) | name | data
 *   响应: magic"TSMB" | status(1) | datalen(4) | data
 * 注意：这是自包含的简化实现，并非与 Windows SMB 线兼容。
 * ============================================================ */
#define TSMB_MAGIC "TSMB"

#define TSMB_OP_NEGOTIATE   0x01
#define TSMB_OP_SESSION     0x02
#define TSMB_OP_TREECONN    0x03
#define TSMB_OP_TREEDISC    0x04
#define TSMB_OP_LOGOFF      0x05
#define TSMB_OP_PUT         0x10
#define TSMB_OP_GET         0x11
#define TSMB_OP_LIST        0x12
#define TSMB_OP_DELETE      0x13

/* 客户端：本地 VFS <-> 远端共享 */
int  smb_put(const char *host, int port, const char *share, const char *user,
             const char *pass, const char *local, const char *remote);
int  smb_get(const char *host, int port, const char *share, const char *user,
             const char *pass, const char *remote, const char *local);

/* 服务端 */
int  smb_server_start(int port);
void smb_server_poll(void);
int  smb_server_active(void);

#endif
