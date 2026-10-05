#ifndef TINYOS_CONSOLE_H
#define TINYOS_CONSOLE_H

#include "types.h"

/* 统一控制台：
 *   输出 = VGA 文本显存 + COM1 串口（镜像内核日志，便于自动化回收）
 *   输入 = PS/2 键盘 或 COM1 串口（二者皆可驱动 Shell，因此可用脚本控制） */

void kputc(char c);
void kputs(const char *s);
int  kprintf(const char *fmt, ...);

int  cons_haschar(void);            /* 是否有待读输入 */
int  cons_getchar(void);            /* 非阻塞读，无输入返回 -1 */
int  cons_readline(char *buf, int max);  /* 阻塞读一行（带回显） */
/* 同 cons_readline，但把输入回显成 '*'（口令用），且不做串口回显泄露 */
int  cons_readline_masked(char *buf, int max);

/* 统一键值读入（串口 ANSI 转义已解析）：返回 keys.h 中的键值，无输入 -1 */
int  cons_getkey(void);             /* 非阻塞 */
int  cons_haskey(void);
int  cons_waitkey(void);            /* 阻塞等一个键 */
void cons_clear(void);              /* 清屏 */
void cons_setcolor(u8 fg, u8 bg);

/* ---- 输出重定向（SSH 服务端执行远端命令时收集 shell 输出） ----
 * 捕获期间 kputc 不再落到 VGA/串口，而是追加到 buf。
 * SSH 的 "exec" 通道要的就是这个：把命令输出原样发回客户端。 */
void cons_capture_begin(char *buf, int max);
int  cons_capture_len(void);
int  cons_capture_overflowed(void);
int  cons_capture_active(void);
void cons_capture_end(void);

#endif /* TINYOS_CONSOLE_H */
