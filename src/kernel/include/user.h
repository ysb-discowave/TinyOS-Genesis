#ifndef TINYOS_USER_H
#define TINYOS_USER_H

#include "types.h"

/* ============================================================
 * TinyOS 多用户子系统
 * ------------------------------------------------------------
 * 存储（都在 VFS 里，因此自动享受 romfs 基线 + TinyFS 持久化）：
 *   /etc/passwd   name:uid:gid:home
 *   /etc/shadow   name:salt:sha256(salt||password)     <- 仅 root 可读
 *
 * 口令永不明文保存；salt 为 16 个十六进制字符。
 * 权限模型（刻意做得很小但"真"）：
 *   uid 0 == root，无条件放行；
 *   /etc /bin /sys 对非 root 只读；
 *   其余路径：节点所有者可写，或所在父目录所有者可写（决定新文件归属）。
 * ============================================================ */

#define USER_MAX    16
#define USER_NAME_MAX 32

typedef struct {
    int  used;
    char name[USER_NAME_MAX];
    int  uid;
    int  gid;
    char home[96];
    char salt[17];
    char hash[65];
} user_t;

void    user_init(void);                       /* 装载/创建用户库，幂等 */
int     user_count(void);
user_t *user_at(int idx);
user_t *user_by_name(const char *name);
user_t *user_by_uid(int uid);
const char *user_name_of(int uid);             /* 找不到返回 "?" */
void    user_list(void);                       /* 打印用户表 */
void    user_save(void);                       /* 写回 /etc/passwd + /etc/shadow */

/* ---- 当前会话身份 ---- */
int  user_current(void);                       /* 0 = root；-1 = 未登录 */
void user_set_current(int uid);
int  user_logged_in(void);

/* ---- 认证 ---- */
int  user_verify(const char *name, const char *pass);      /* 0 = 通过 */
int  user_login_prompt(void);                              /* 交互式登录，返回 uid 或 -1 */
int  user_change_pass(const char *name, const char *newpass);
int  user_add(const char *name, const char *pass, int uid, const char *home);
int  user_del(const char *name);

/* ---- 权限 ---- */
int  user_can_write(const char *path);         /* 当前用户对该路径是否有写权限 */
int  user_can_read(const char *path);
const char *user_denied_msg(void);             /* 统一的拒绝提示 */

#endif
