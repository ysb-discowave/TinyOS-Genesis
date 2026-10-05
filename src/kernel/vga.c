#include "vga.h"
#include "io.h"
#include "libc.h"

static u16 *vga_mem = (u16*)0xB8000;
static int cur_x = 0, cur_y = 0;
static u8 cur_fg = 0x0F, cur_bg = 0x00;
static int cursor_on = 1;

/* 把文本光标位置/形状写入 CRTC（0x3D4/0x3D5），否则屏幕上没有光标 */
static void vga_hw_cursor(void) {
    if (!cursor_on) {
        outb(0x3D4, 0x0A); outb(0x3D5, 0x20);   /* 关闭光标 */
        return;
    }
    u16 pos = (u16)(cur_y * VGA_WIDTH + cur_x);
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)(pos >> 8));
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0A); outb(0x3D5, 0x0E);       /* 起始扫描线 */
    outb(0x3D4, 0x0B); outb(0x3D5, 0x0F);       /* 结束扫描线 */
}

/* 窗口控制台模式：激活时所有输出限制在 (wx,wy,w,h) 区域内 */
static struct {
    int active;
    int x, y, w, h;
    int cx, cy;   /* 窗口内光标 */
} win = {0};

void vga_init(void) { cur_x = cur_y = 0; vga_clear(0x00); win.active = 0; vga_hw_cursor(); }

/* 显示/隐藏硬件文本光标（桌面自绘光标时可关闭） */
void vga_cursor_show(int on) { cursor_on = on; vga_hw_cursor(); }

void vga_clear(u8 color) {
    u16 v = (u16)(((color << 4) | 0x0F) << 8);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) vga_mem[i] = v;
    cur_x = cur_y = 0; win.active = 0;
    vga_hw_cursor();
}

static u16 cell(u8 c) { return (u16)(((cur_bg << 4) | cur_fg) << 8) | (u8)c; }

void vga_scroll(void) {
    for (int y = 1; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            vga_mem[(y - 1) * VGA_WIDTH + x] = vga_mem[y * VGA_WIDTH + x];
    for (int x = 0; x < VGA_WIDTH; x++)
        vga_mem[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
            (u16)((((cur_bg << 4) | cur_fg) << 8) | ' ');
    cur_y = VGA_HEIGHT - 1; cur_x = 0;
}

void vga_scroll_region(int top, int bottom) {
    for (int y = top + 1; y <= bottom; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            vga_mem[(y - 1) * VGA_WIDTH + x] = vga_mem[y * VGA_WIDTH + x];
    for (int x = 0; x < VGA_WIDTH; x++)
        vga_mem[bottom * VGA_WIDTH + x] =
            (u16)((((cur_bg << 4) | cur_fg) << 8) | ' ');
}

void vga_putc(char c) {
    if (win.active) {
        if (c == '\n') { win.cx = 0; if (++win.cy >= win.h) { vga_scroll_region(win.y, win.y + win.h - 1); win.cy = win.h - 1; } }
        else if (c == '\r') { win.cx = 0; }
        else if (c == '\t') { win.cx = (win.cx + 8) & ~7; if (win.cx >= win.w) { win.cx = 0; if(++win.cy>=win.h){vga_scroll_region(win.y,win.y+win.h-1);win.cy=win.h-1;} } }
        else if (c == '\b') { if (win.cx > 0) win.cx--; }
        else {
            int gx = win.x + win.cx, gy = win.y + win.cy;
            if (gx >= 0 && gy >= 0 && gx < VGA_WIDTH && gy < VGA_HEIGHT)
                vga_mem[gy * VGA_WIDTH + gx] = cell(c);
            if (++win.cx >= win.w) { win.cx = 0; if (++win.cy >= win.h) { vga_scroll_region(win.y, win.y + win.h - 1); win.cy = win.h - 1; } }
        }
        /* 让硬件文本光标跟随窗口内光标位置，GUI 终端里才能看到提示符光标 */
        cur_x = win.x + win.cx;
        cur_y = win.y + win.cy;
        vga_hw_cursor();
        return;
    }
    if (c == '\n') { cur_x = 0; if (++cur_y >= VGA_HEIGHT) vga_scroll(); }
    else if (c == '\r') { cur_x = 0; }
    else if (c == '\t') { cur_x = (cur_x + 8) & ~7; if (cur_x >= VGA_WIDTH){cur_x=0;cur_y++;} }
    else if (c == '\b') { if (cur_x>0) cur_x--; }
    else {
        vga_mem[cur_y * VGA_WIDTH + cur_x] = cell(c);
        if (++cur_x >= VGA_WIDTH) { cur_x = 0; if (++cur_y >= VGA_HEIGHT) vga_scroll(); }
    }
    vga_hw_cursor();
}

void vga_puts(const char *s) { while (*s) vga_putc(*s++); }
void vga_putsn(const char *s, size_t n) { for (size_t i=0;i<n;i++) vga_putc(s[i]); }

void vga_setcolor(u8 fg, u8 bg) { cur_fg = fg; cur_bg = bg; }

void vga_getcursor(int *x, int *y) { *x = cur_x; *y = cur_y; }
void vga_setcursor(int x, int y) { cur_x = x; cur_y = y; vga_hw_cursor(); }

void vga_invert_cell(int x, int y) {
    if (x < 0 || y < 0 || x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    u16 v = vga_mem[y * VGA_WIDTH + x];
    u8 ch = v & 0xFF;
    u8 attr = (v >> 8) & 0xFF;
    u8 fg = attr & 0x0F, bg = (attr >> 4) & 0x0F;
    attr = (fg << 4) | bg;
    vga_mem[y * VGA_WIDTH + x] = (u16)(attr << 8) | ch;
}

void vga_fill_rect_cells(int x, int y, int w, int h, u8 color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int xx = x + i, yy = y + j;
            if (xx < 0 || yy < 0 || xx >= VGA_WIDTH || yy >= VGA_HEIGHT) continue;
            vga_mem[yy * VGA_WIDTH + xx] = (u16)((color << 8) | ' ');
        }
}

/* 窗口控制台：在桌面终端/弹窗中使用 */
void vga_win_enable(int x, int y, int w, int h) {
    win.active = 1; win.x = x; win.y = y; win.w = w; win.h = h; win.cx = 0; win.cy = 0;
}
void vga_win_disable(void) { win.active = 0; }
void vga_win_setcursor(int cx, int cy) { win.cx = cx; win.cy = cy; }
void vga_win_getcursor(int *cx, int *cy) { *cx = win.cx; *cy = win.cy; }
int  vga_win_active(void) { return win.active; }
void vga_win_params(int *x, int *y, int *w, int *h) {
    *x = win.x; *y = win.y; *w = win.w; *h = win.h;
}

u16 *vga_buffer(void) { return vga_mem; }
int  vga_text_width(void) { return VGA_WIDTH; }
int  vga_text_height(void) { return VGA_HEIGHT; }

void vga_draw_str(int x, int y, const char *s, u8 fg, u8 bg) {
    int cx = x;
    if (y < 0 || y >= VGA_HEIGHT) return;
    while (*s) {
        if (cx >= 0 && cx < VGA_WIDTH)
            vga_mem[y * VGA_WIDTH + cx] = (u16)(((u16)((bg << 4) | fg)) << 8) | (u8)*s;
        cx++; s++;
    }
}
