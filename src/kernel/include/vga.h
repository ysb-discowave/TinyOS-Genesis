#ifndef TINYOS_VGA_H
#define TINYOS_VGA_H

#include "types.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

void vga_init(void);
void vga_clear(u8 color);
void vga_putc(char c);
void vga_puts(const char *s);
void vga_putsn(const char *s, size_t n);
void vga_setcolor(u8 fg, u8 bg);
void vga_getcursor(int *x, int *y);
void vga_setcursor(int x, int y);
/* 显示/隐藏硬件文本光标（桌面自绘光标时可关闭） */
void vga_cursor_show(int on);
void vga_scroll(void);
void vga_scroll_region(int top, int bottom);
/* 在指定字符格绘制/清除反色光标（供鼠标使用） */
void vga_invert_cell(int x, int y);
/* 文本模式矩形取景：在 (x,y) 起 size 区域填充背景色，用于窗口 */
void vga_fill_rect_cells(int x, int y, int w, int h, u8 color);

/* 窗口控制台模式：激活后所有 vga_putc 输出限制在窗口区域内 */
void vga_win_enable(int x, int y, int w, int h);
void vga_win_disable(void);
void vga_win_setcursor(int cx, int cy);
void vga_win_getcursor(int *cx, int *cy);
int  vga_win_active(void);
void vga_win_params(int *x, int *y, int *w, int *h);

/* 直接操作文本缓冲（供桌面引擎绘制窗口/保存恢复屏幕） */
u16 *vga_buffer(void);
void vga_draw_str(int x, int y, const char *s, u8 fg, u8 bg);
int  vga_text_width(void);
int  vga_text_height(void);

#endif /* TINYOS_VGA_H */
