#ifndef TINYOS_SERIAL_H
#define TINYOS_SERIAL_H

#include "types.h"

/* COM1 (0x3F8) 串口驱动。
 * 作用：
 *   1) 作为内核日志通道（输出镜像到 VGA，便于在无显示器/自动化环境下取回日志）；
 *   2) 作为输入通道之一（与 PS/2 键盘并列），使 TinyOS 可被脚本化控制。 */

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
int  serial_haschar(void);      /* 环形缓冲中是否有字节 */
int  serial_getchar(void);      /* 非阻塞；无数据返回 -1 */
/* 非阻塞；返回统一键值（解析 ANSI 转义序列 → 方向键等），无数据返回 -1 */
int  serial_getkey(void);

/* 中断处理（IRQ4）：把到达的字节放进环形缓冲 */
int  serial_isr(void);

/* 注册 IRQ4 处理函数并解除屏蔽 */
void serial_enable_irq(void);

#endif /* TINYOS_SERIAL_H */
