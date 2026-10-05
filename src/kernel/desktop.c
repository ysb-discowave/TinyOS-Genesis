#include "desktop.h"
#include "vga.h"
#include "console.h"
#include "process.h"
#include "shell.h"
#include "mouse.h"
#include "pit.h"
#include "vfs.h"
#include "tncr.h"
#include "user.h"
#include "net/net.h"
#include "libc.h"

#define W 80
#define H 25

int g_desktop_active = 0;

static u16 screen_save[W * H];

/* ---------------- 基础绘制 ---------------- */
static void fill(int x, int y, int w, int h, u8 fg, u8 bg) {
    u16 attr = (u16)(((u16)((bg << 4) | fg)) << 8);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int xx = x + i, yy = y + j;
            if (xx >= 0 && yy >= 0 && xx < W && yy < H)
                vga_buffer()[yy * W + xx] = (u16)(attr | ' ');
        }
}
static void box(int x, int y, int w, int h, u8 fg, u8 bg, const char *title) {
    fill(x, y, w, h, fg, bg);
    for (int i = 0; i < w; i++) {
        vga_draw_str(x + i, y, "\xC4", fg, bg);
        vga_draw_str(x + i, y + h - 1, "\xC4", fg, bg);
    }
    for (int j = 0; j < h; j++) {
        vga_draw_str(x, y + j, "\xB3", fg, bg);
        vga_draw_str(x + w - 1, y + j, "\xB3", fg, bg);
    }
    vga_draw_str(x, y, "\xDA", fg, bg);
    vga_draw_str(x + w - 1, y, "\xBF", fg, bg);
    vga_draw_str(x, y + h - 1, "\xC0", fg, bg);
    vga_draw_str(x + w - 1, y + h - 1, "\xD9", fg, bg);
    if (title && title[0]) {
        /* 标题栏（反色） */
        fill(x + 2, y, w - 4, 1, bg, fg);
        vga_draw_str(x + 3, y, title, bg, fg);
    }
}
static void save_screen(void)    { for (int i = 0; i < W * H; i++) screen_save[i] = vga_buffer()[i]; }
static void restore_screen(void) { for (int i = 0; i < W * H; i++) vga_buffer()[i] = screen_save[i]; }

/* ---------------- 鼠标光标 ----------------
 * 文本模式没有"指针图形"，惯例做法是把指针所在的那一个字符格反色。
 * vga_invert_cell() 本来就是为此准备的，但桌面以前从没调用它 ——
 * 结果用户移动鼠标时屏幕上毫无反馈，根本没法把指针瞄准图标。 */
static int cur_cell = -1;
static u16 cur_saved = 0;

static void cursor_erase(void) {
    if (cur_cell >= 0) {
        vga_buffer()[cur_cell] = cur_saved;
        cur_cell = -1;
    }
}

static void cursor_paint(int mx, int my) {
    if (mx < 0 || my < 0 || mx >= W || my >= H) return;
    int c = my * W + mx;
    if (c == cur_cell) return;          /* 没动就不重画，避免闪烁 */
    cursor_erase();
    cur_saved = vga_buffer()[c];
    vga_invert_cell(mx, my);
    cur_cell = c;
}

/* ---------------- 退出桌面：把 Shell 的屏幕原样还回去 ----------------
 * 如果不还原，Shell 会带着桌面的残留画面、并以一个陈旧的文本光标位置继续输出，
 * 结果提示符直接叠在桌面底部提示条上（实测可见）。 */
static u16 desk_save[W * H];
static int desk_sx = 0, desk_sy = 0;

static void desktop_leave(void) {
    cursor_erase();                     /* 先把光标借走的那一格还回去 */
    cur_cell = -1;
    vga_win_disable();
    for (int i = 0; i < W * H; i++) vga_buffer()[i] = desk_save[i];
    vga_setcursor(desk_sx, desk_sy);
    g_desktop_active = 0;
}

/* ---------------- 桌面图标 ---------------- */
#define ICON_X 4
#define ICON_Y 4
#define ICON_W 18
#define ICON_H 5

static void draw_icon(int selected) {
    u8 fg = selected ? 0x00 : 0x0F;
    u8 bg = selected ? 0x0F : 0x01;
    box(ICON_X, ICON_Y, ICON_W, ICON_H, fg, bg, NULL);
    vga_draw_str(ICON_X + 3, ICON_Y + 1, "[>_]  ", fg, bg);
    vga_draw_str(ICON_X + 3, ICON_Y + 2, "Terminal", fg, bg);
}

static void draw_desktop(int selected) {
    fill(0, 0, W, H, 0x0F, 0x01);          /* 桌面背景：蓝底白字 */
    /* 顶栏 */
    fill(0, 0, W, 1, 0x0F, 0x07);
    vga_draw_str(2, 0, "TinyOS Genesis v0.1  Desktop", 0x0F, 0x07);

    draw_icon(selected);

    /* 底部提示：ASCII —— 80x25 文本模式的字形表是 CP437，
     * 画不了汉字（汉字会变成乱码，而且一个汉字要占 3 格，直接顶出边界）。
     * 这几行是"必须看得懂"的导航提示，所以用英文。 */
    fill(0, H - 1, W, 1, 0x0E, 0x01);
    vga_draw_str(2, H - 1,
                 "Click the icon, or press ENTER / SPACE, to open a terminal   (ESC = shell)",
                 0x0E, 0x01);
}

/* ---------------- 弹窗（GUI 程序窗口） ---------------- */
void desktop_show_window(const char *title, const char *content) {
    save_screen();
    int win_was = vga_win_active();
    int wx = 0, wy = 0, ww = 0, wh = 0;
    if (win_was) { vga_win_params(&wx, &wy, &ww, &wh); vga_win_disable(); }

    int bw = 54, bh = 11;
    if (bw > W - 4) bw = W - 4;
    int bx = (W - bw) / 2, by = (H - bh) / 2;
    box(bx, by, bw, bh, 0x0E, 0x04, title ? title : "window");

    /* 内容（简单按行切分） */
    char line[120];
    int li = 0, cx = bx + 2, cy = by + 2, li2 = 0;
    const char *p = content ? content : "";
    while (*p && cy < by + bh - 3) {
        if (*p == '\n' || li2 >= bw - 4) {
            line[li] = 0;
            vga_draw_str(cx, cy, line, 0x0F, 0x04);
            cy++; li = 0; li2 = 0;
            if (*p == '\n') { p++; continue; }
            continue;
        }
        line[li++] = *p++;
        li2++;
    }
    if (li > 0 && cy < by + bh - 2) { line[li] = 0; vga_draw_str(cx, cy, line, 0x0F, 0x04); }
    vga_draw_str(bx + 2, by + bh - 2, "[ press any key / click to close ]", 0x07, 0x04);

    /* 等待关闭 */
    for (;;) {
        net_poll_all();
        if (cons_getchar() >= 0) break;
        if (mouse_consume_click()) break;
        __asm__ volatile("hlt");
    }

    restore_screen();
    if (win_was) vga_win_enable(wx, wy, ww, wh);
}

/* ---------------- 终端窗口 ---------------- */
static void terminal_run(void) {
    cursor_erase();                       /* 进终端就收掉桌面光标 */
    vga_cursor_show(1);                  /* 打开硬件文本光标（提示符光标） */

    /* 会话内的身份：桌面是从已登录的 Shell 进来的，所以通常已经登录；
     * 若没有（例如被别人以未登录状态直接拉起桌面），先要求登录。 */
    if (!user_logged_in()) {
        vga_win_disable();
        shell_login_loop();
    }

    int sid = proc_new_session(SESS_TERMINAL, "terminal");
    int pid = proc_spawn("terminal", sid);

    /* 画终端窗口 */
    int tx = 1, ty = 3, tw = 78, th = 21;
    box(tx, ty, tw, th, 0x0F, 0x00, "TinyOS Terminal");
    vga_win_enable(tx + 1, ty + 1, tw - 2, th - 2);
    vga_win_disable();               /* 先画提示行到全局 */
    vga_draw_str(tx + 2, ty + th - 2,
                 "type help | GUI apps pop up a window here | 'exit' closes", 0x0B, 0x00);
    vga_win_enable(tx + 1, ty + 1, tw - 2, th - 2);
    { int wx = 0, wy = 0, ww = 0, wh = 0; vga_win_params(&wx, &wy, &ww, &wh); vga_setcursor(wx, wy); }

    cursor_erase();                  /* 写欢迎语前先归还可能借走的光标格 */
    kprintf("TinyOS terminal ready (session %d, user %s, uid %d).\n",
            sid, user_name_of(proc_session_uid(sid)), proc_session_uid(sid));
    kprintf("hint: running a GUI .TNCR program here opens a window in this session.\n");

    char line[160];
    for (;;) {
        net_poll_all();
        char pr[160];
        shell_prompt(pr, sizeof(pr));
        cursor_erase();              /* 提示符前收掉鼠标光标格，避免它盖住文字 */
        vga_setcolor(0x0A, 0x00);
        kputs(pr);
        vga_setcolor(0x0F, 0x00);

        int n = 0;
        int closed = 0;
        for (;;) {
            net_poll_all();
            int c = cons_getchar();
            if (c < 0) {
                /* 空闲时让鼠标指针（反色一格）跟手，用户才知道指针在哪 */
                int mxx = 0, myy = 0; u8 mbt = 0;
                mouse_state(&mxx, &myy, &mbt);
                if (mouse_present()) cursor_paint(mxx, myy);
                __asm__ volatile("hlt"); continue;
            }
            if (c == 27) { closed = 1; break; }                    /* ESC */
            /* 调试：TAB 把当前画面导出到串口。终端里 's' 是正常输入字符，
             * 所以这里换用 TAB 触发，方便无窗口环境下截图。 */
            if (c == '\t') { shell_shot(); continue; }
            if (c == '\r' || c == '\n') { cursor_erase(); kputc('\n'); line[n] = 0; break; }
            if (c == '\b' || c == 127) { cursor_erase(); if (n > 0) { n--; kputs("\b \b"); } continue; }
            if (c >= 32 && c < 127 && n < (int)sizeof(line) - 1) {
                cursor_erase();
                line[n++] = (char)c; kputc((char)c);
            }
        }
        if (closed) break;
        if (n == 0) continue;
        if (strcmp(line, "exit") == 0) break;
        if (strcmp(line, "desktop") == 0) { cursor_erase(); shell_exec(line, sid); continue; }
        cursor_erase();
        shell_exec(line, sid);       /* 在会话 sid 中执行 → GUI 程序可弹窗 */
    }

    proc_finish(pid);
    vga_win_disable();
    cursor_erase();
    cur_cell = -1;                 /* 窗口内容已把屏幕改了，光标记录作废 */
    vga_cursor_show(0);            /* 回到桌面时不留游离的硬件光标 */
}

/* ---------------- 桌面主循环 ---------------- */
void desktop_enter(void) {
    g_desktop_active = 1;
    vga_cursor_show(0);                  /* 桌面用自绘鼠标光标，关掉游离的硬件光标 */
    int selected = 1;

    /* 存档 Shell 的画面与光标位置，退出桌面时原样恢复 */
    for (int i = 0; i < W * H; i++) desk_save[i] = vga_buffer()[i];
    vga_getcursor(&desk_sx, &desk_sy);

    /* 让顶栏右侧显示实际地址 */
    char label[48];
    snprintf(label, sizeof(label), "IP %u.%u.%u.%u   ENTER=terminal",
             (g_ip >> 24) & 0xFF, (g_ip >> 16) & 0xFF, (g_ip >> 8) & 0xFF, g_ip & 0xFF);

    for (;;) {
        draw_desktop(selected);
        cur_cell = -1;                 /* 整屏重画过，光标位置失效 */
        vga_draw_str(W - 30, 0, label, 0x00, 0x07);

        /* 等待“打开”动作 */
        int open = 0;
        for (;;) {
            net_poll_all();

            /* 键盘 */
            int c = cons_getchar();
            if (c == 27) { desktop_leave(); return; }   /* ESC 返回 Shell */
            if (c == '\r' || c == '\n' || c == ' ') { cursor_erase(); open = 1; break; }
            if (c == 's' || c == 'S') { shell_shot(); continue; }   /* 调试：把桌面导出到串口 */

            /* 鼠标：先把指针画出来，用户才知道自己在指哪 */
            int mx, my; u8 btns;
            mouse_state(&mx, &my, &btns);
            if (mouse_present()) cursor_paint(mx, my);

            int in_icon = (mx >= ICON_X && mx < ICON_X + ICON_W &&
                           my >= ICON_Y && my < ICON_Y + ICON_H);
            if (in_icon != selected) {          /* 悬停高亮 */
                cursor_erase();
                selected = in_icon;
                draw_desktop(selected);
                vga_draw_str(W - 30, 0, label, 0x00, 0x07);
            }
            /* 单击即打开：以前要求「同一格、0.6 秒内双击」，
             * 但没有指针反馈时这几乎不可能命中，用户会以为终端打不开。 */
            if (mouse_consume_click() && in_icon) { cursor_erase(); open = 1; break; }

            __asm__ volatile("hlt");
        }

        if (open) {
            terminal_run();
        }
        if (!g_desktop_active) { desktop_leave(); return; }
    }
}
