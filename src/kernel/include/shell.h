#ifndef TINYOS_SHELL_H
#define TINYOS_SHELL_H

#include "types.h"

/* 执行一条命令；session_id 为调用方所属会话（0=普通 Shell） */
int  shell_exec(const char *cmd, int session_id);
/* 交互式 Shell 主循环（协作式轮询网络） */
void shell_main(void);

/* 当前工作目录 / 相对路径解析（桌面终端共用） */
int  shell_cwd(char *out, int n);
void shell_setcwd(const char *p);
/* 传给下一个 TNCR 程序的命令行参数（TNCR 没有 argv，由 Shell 代传） */
void shell_set_arg(const char *s);
int  shell_arg(char *out, int n);
void shell_resolve(const char *in, char *out, int n);
void shell_prompt(char *out, int n);   /* 生成 "user@tinyos:/path# " 提示符 */
void shell_login_loop(void);           /* 阻塞式登录（三次失败后可回车重试） */

/* 把当前 VGA 文本屏幕（0xB8000 的 80x25 字符+属性）导出到串口。
 * 只写串口，不动屏幕；无显示器环境下也能核对屏幕内容。 */
void shell_shot(void);

#endif
