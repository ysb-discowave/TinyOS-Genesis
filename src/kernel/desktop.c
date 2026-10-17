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

/* ================================================================
 * TinyOS Genesis 图形化桌面（80x25 VGA 文本模式 + CP437 字形）
 * ---------------------------------------------------------------
 * 文本模式没有位图可用，但 CP437 自带双线框、阴影块、方块字符，
 * 足够拼出一个"有设计感"的桌面：
 *   · 斜向抖动渐变壁纸（fg/bg 双色 ░ 点阵）
 *   · 双线边框 + 投影 + 立体标题栏的窗口
 *   · 图标网格（悬停/选中高亮）
 *   · 底部任务栏（开始按钮 + 状态）
 * 约定：所有可见文案用 ASCII —— VGA 文本模式字形表是 CP437，画不了汉字。
 * ================================================================ */
#define W 80
#define H 25

/* ---- 配色（经典 16 色） ---- */
#define CLR_WALL_BG  0x01   /* 深蓝壁纸 */
#define CLR_WALL_FG  0x09   /* 亮蓝，做抖动纹理 */
#define CLR_BAR_BG   0x00   /* 任务栏/窗口底：黑 */
#define CLR_BAR_FG   0x0F   /* 任务栏字：白 */
#define CLR_ACCENT   0x0B   /* 强调色：亮青 */
#define CLR_ACCENT2  0x09   /* 次强调：亮蓝 */
#define CLR_WIN_FG   0x0F   /* 窗口边框：白 */
#define CLR_SHADOW   0x08   /* 投影：深灰 */
#define CLR_SEL_BG   0x07   /* 选中项底：亮灰 */
#define CLR_SEL_FG   0x00   /* 选中项字：黑 */
#define CLR_DIM      0x07   /* 次要说明字 */

/* CP437 字符 */
#define CH_H  0xC4   /* ─ 单横 */
#define CH_V  0xB3   /* │ 单竖 */
#define CH_TL 0xC9   /* ╔ 双线框左上 */
#define CH_TR 0xBB   /* ╗ 双线框右上 */
#define CH_BL 0xC8   /* ╚ 双线框左下 */
#define CH_BR 0xB9   /* ╝ 双线框右下 */
#define CH_DH  0xCD  /* ═ 双横 */
#define CH_DV  0xBA  /* ║ 双竖 */
#define CH_SHADE_L 0xB0 /* ░ */
#define CH_BLOCK    0xDB /* █ */

int g_desktop_active = 0;

static u16 screen_save[W * H];

/* ---------------- 基础绘制 ---------------- */
static void cell(int x, int y, char ch, u8 fg, u8 bg) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    u16 attr = (u16)(((u16)(((u16)bg << 4) | fg)) << 8);
    vga_buffer()[y * W + x] = (u16)(attr | (unsigned char)ch);
}
static void fill(int x, int y, int w, int h, u8 fg, u8 bg) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            cell(x + i, y + j, ' ', fg, bg);
}
static void hline(int x, int y, int w, char ch, u8 fg, u8 bg) {
    for (int i = 0; i < w; i++) cell(x + i, y, ch, fg, bg);
}
static void vline(int x, int y, int h, char ch, u8 fg, u8 bg) {
    for (int j = 0; j < h; j++) cell(x, y + j, ch, fg, bg);
}

/* 斜向抖动渐变壁纸：随 (x+y) 用 ░ 点阵，亮蓝浮在深蓝上，像一层光晕 */
static void draw_wallpaper(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int d = (x * 2 + y) % 7;
            if (d == 0)      cell(x, y, (char)CH_SHADE_L, CLR_WALL_FG, CLR_WALL_BG);
            else             cell(x, y, ' ', CLR_WALL_FG, CLR_WALL_BG);
        }
}

/* 底部任务栏：左边开始按钮，右边状态 */
static void draw_taskbar(const char *status) {
    int ty = H - 1;
    fill(0, ty, W, 1, CLR_BAR_FG, CLR_BAR_BG);
    /* 开始按钮 */
    vga_draw_str(1, ty, "[ Genesis ]", 0x00, CLR_ACCENT);
    /* 分隔点 */
    cell(14, ty, (char)CH_V, CLR_BAR_FG, CLR_BAR_BG);
    if (status && status[0]) vga_draw_str(16, ty, status, CLR_BAR_FG, CLR_BAR_BG);
}

/* 窗口：投影 + 双线框 + 立体标题栏 + 右上角控制按钮 */
static void draw_window(int x, int y, int w, int h, const char *title, u8 body_bg) {
    /* 投影（右下偏移一格） */
    fill(x + 2, y + h, w - 2, 1, CLR_SHADOW, CLR_SHADOW);
    fill(x + w, y + 1, 2, h, CLR_SHADOW, CLR_SHADOW);

    /* 双线框 */
    u8 fb = CLR_WIN_FG, bb = body_bg;
    hline(x,   y,     w, (char)CH_DH, fb, bb);
    hline(x,   y + h - 1, w, (char)CH_DH, fb, bb);
    vline(x,       y,     h, (char)CH_DV, fb, bb);
    vline(x + w - 1, y,   h, (char)CH_DV, fb, bb);
    cell(x,       y,       (char)CH_TL, fb, bb);
    cell(x + w - 1, y,     (char)CH_TR, fb, bb);
    cell(x,       y + h - 1, (char)CH_BL, fb, bb);
    cell(x + w - 1, y + h - 1, (char)CH_BR, fb, bb);
    fill(x + 1, y + 1, w - 2, h - 2, fb, body_bg);   /* 内容底 */

    /* 立体标题栏：强调色 + 标题 + 控制按钮 */
    fill(x + 1, y + 1, w - 2, 1, CLR_ACCENT2, CLR_ACCENT2);
    int tx = x + 3;
    if (title && title[0]) {
        vga_draw_str(tx, y + 1, title, CLR_SEL_FG, CLR_ACCENT2);
        tx += (int)strlen(title) + 1;
    }
    /* 控制按钮：─  □  x */
    int bx = x + w - 5;
    cell(bx,     y + 1, '-', CLR_SEL_FG, CLR_ACCENT2);
    cell(bx + 2, y + 1, (char)0xFE, CLR_SEL_FG, CLR_ACCENT2); /* □ */
    cell(bx + 4, y + 1, (char)0x18, CLR_SEL_FG, CLR_ACCENT2); /* x */
}

static void save_screen(void)    { for (int i = 0; i < W * H; i++) screen_save[i] = vga_buffer()[i]; }
static void restore_screen(void) { for (int i = 0; i < W * H; i++) vga_buffer()[i] = screen_save[i]; }

/* ---------------- 鼠标光标（反色一格） ---------------- */
static int cur_cell = -1;
static u16 cur_saved = 0;
static void cursor_erase(void) {
    if (cur_cell >= 0) { vga_buffer()[cur_cell] = cur_saved; cur_cell = -1; }
}
static void cursor_paint(int mx, int my) {
    if (mx < 0 || my < 0 || mx >= W || my >= H) return;
    int c = my * W + mx;
    if (c == cur_cell) return;
    cursor_erase();
    cur_saved = vga_buffer()[c];
    vga_invert_cell(mx, my);
    cur_cell = c;
}

/* ---------------- 桌面图标 ---------------- */
#define ICON_W 16
#define ICON_H 6
#define ICON_GAP 19
#define ICON_X0 4
#define ICON_Y  3
#define N_ICONS 3

/* 每个图标：顶行图形 + 标签 + 点击动作 */
static const char *g_icon_glyph[N_ICONS] = { "  ______  ", " | .  . | ", "  \\_||_/  " };
static const char *g_icon_label[N_ICONS] = { "Terminal", "Files", "Help" };

/* ---- 系统信息窗口内容（About 用） ---- */
static void window_about(void) {
    char buf[512];
    snprintf(buf, sizeof buf,
             "TinyOS Genesis v0.1\n"
             "i386 protected mode, bare metal\n\n"
             "  Shell     : tinysh (TNCR)\n"
             "  Packages  : pkg / pack\n"
             "  Sessions  : %d\n\n"
             "  IP  %u.%u.%u.%u\n"
             "  GW  %u.%u.%u.%u\n"
             "  Uptime %us",
             proc_count(),
             (g_ip >> 24) & 0xFF, (g_ip >> 16) & 0xFF, (g_ip >> 8) & 0xFF, g_ip & 0xFF,
             (g_gw >> 24) & 0xFF, (g_gw >> 16) & 0xFF, (g_gw >> 8) & 0xFF, g_gw & 0xFF,
             (u32)pit_seconds());
    desktop_show_window("About TinyOS", buf);
}

static void draw_icon(int idx, int selected) {
    int x = ICON_X0 + idx * ICON_GAP;
    int y = ICON_Y;
    u8 fg = selected ? CLR_SEL_FG : CLR_BAR_FG;
    u8 bg = selected ? CLR_SEL_BG : 0x00;
    if (selected) fill(x, y, ICON_W, ICON_H, fg, bg);
    /* 图形（双线小方框 + 内部字形） */
    hline(x + 1, y,         ICON_W - 2, (char)CH_DH, fg, bg);
    hline(x + 1, y + 3,     ICON_W - 2, (char)CH_DH, fg, bg);
    vline(x,     y,         4,          (char)CH_DV, fg, bg);
    vline(x + ICON_W - 1, y, 4,          (char)CH_DV, fg, bg);
    cell(x,     y,     (char)CH_TL, fg, bg);
    cell(x + ICON_W - 1, y,     (char)CH_TR, fg, bg);
    cell(x,     y + 3, (char)CH_BL, fg, bg);
    cell(x + ICON_W - 1, y + 3, (char)CH_BR, fg, bg);
    vga_draw_str(x + 3, y + 2, g_icon_glyph[idx], fg, bg);
    /* 标签 */
    int lx = x + (ICON_W - (int)strlen(g_icon_label[idx])) / 2;
    vga_draw_str(lx, y + 4, g_icon_label[idx], fg, bg);
}

static int icon_hit(int mx, int my) {
    for (int i = 0; i < N_ICONS; i++) {
        int x = ICON_X0 + i * ICON_GAP;
        if (mx >= x && mx < x + ICON_W && my >= ICON_Y && my < ICON_Y + ICON_H)
            return i;
    }
    return -1;
}

static void draw_desktop(int selected, const char *status) {
    draw_wallpaper();
    for (int i = 0; i < N_ICONS; i++) draw_icon(i, selected == i);
    draw_taskbar(status);
}

/* ---------------- 弹窗（GUI 程序窗口） ---------------- */
void desktop_show_window(const char *title, const char *content) {
    save_screen();
    int win_was = vga_win_active();
    int wx = 0, wy = 0, ww = 0, wh = 0;
    if (win_was) { vga_win_params(&wx, &wy, &ww, &wh); vga_win_disable(); }

    int bw = 56, bh = 13;
    if (bw > W - 6) bw = W - 6;
    if (bh > H - 4) bh = H - 4;
    int bx = (W - bw) / 2, by = 3 + (H - 2 - bh) / 2;
    draw_window(bx, by, bw, bh, title ? title : "Window", CLR_BAR_BG);

    /* 内容（按宽度切行） */
    char line[128];
    int li = 0, cx = bx + 2, cy = by + 3, li2 = 0;
    const char *p = content ? content : "";
    while (*p && cy < by + bh - 2) {
        if (*p == '\n' || li2 >= bw - 4) {
            line[li] = 0;
            vga_draw_str(cx, cy, line, CLR_BAR_FG, CLR_BAR_BG);
            cy++; li = 0; li2 = 0;
            if (*p == '\n') { p++; continue; }
            continue;
        }
        line[li++] = *p++;
        li2++;
    }
    if (li > 0 && cy < by + bh - 2) { line[li] = 0; vga_draw_str(cx, cy, line, CLR_BAR_FG, CLR_BAR_BG); }
    /* 底栏提示 */
    int fy = by + bh - 2;
    fill(bx + 1, fy, bw - 2, 1, CLR_DIM, 0x08);
    vga_draw_str(bx + 3, fy, "[ press any key / click to close ]", CLR_BAR_FG, 0x08);

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
    cursor_erase();
    vga_cursor_show(1);

    if (!user_logged_in()) {
        vga_win_disable();
        shell_login_loop();
    }

    int sid = proc_new_session(SESS_TERMINAL, "terminal");
    int pid = proc_spawn("terminal", sid);

    /* 画带边框的终端窗口；标题栏占第 1 行，内容从 ty+2 开始 */
    int tx = 2, ty = 1, tw = 76, th = H - 3;
    draw_window(tx, ty, tw, th, "Terminal", CLR_BAR_BG);
    /* 底部提示条 */
    fill(tx + 1, ty + th - 2, tw - 2, 1, CLR_DIM, 0x08);
    vga_draw_str(tx + 3, ty + th - 2,
                 "help | GUI apps pop up here | 'exit' closes  (ESC = desktop)",
                 CLR_BAR_FG, 0x08);
    /* 终端滚动区域：内容区（不含标题栏与底部提示条） */
    vga_win_enable(tx + 1, ty + 2, tw - 2, th - 4);
    { int wx = 0, wy = 0, ww = 0, wh = 0; vga_win_params(&wx, &wy, &ww, &wh); vga_setcursor(wx, wy); }

    cursor_erase();
    vga_setcolor(CLR_ACCENT, CLR_BAR_BG);
    kprintf("TinyOS terminal  (session %d, user %s)\n",
            sid, user_name_of(proc_session_uid(sid)));
    vga_setcolor(CLR_DIM, CLR_BAR_BG);
    kprintf("GUI programs open a window here; 'desktop' returns to the desktop.\n");
    vga_setcolor(CLR_BAR_FG, CLR_BAR_BG);

    char line[160];
    for (;;) {
        net_poll_all();
        char pr[160];
        shell_prompt(pr, sizeof(pr));
        cursor_erase();
        vga_setcolor(0x0A, CLR_BAR_BG);
        kputs(pr);
        vga_setcolor(CLR_BAR_FG, CLR_BAR_BG);

        int n = 0;
        int closed = 0;
        for (;;) {
            net_poll_all();
            int c = cons_getchar();
            if (c < 0) {
                int mxx = 0, myy = 0; u8 mbt = 0;
                mouse_state(&mxx, &myy, &mbt);
                if (mouse_present()) cursor_paint(mxx, myy);
                __asm__ volatile("hlt"); continue;
            }
            if (c == 27) { closed = 1; break; }
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
        shell_exec(line, sid);
    }

    proc_finish(pid);
    vga_win_disable();
    cursor_erase();
    cur_cell = -1;
    vga_cursor_show(0);
}

/* ---------------- 桌面主循环 ---------------- */
void desktop_enter(void) {
    g_desktop_active = 1;
    vga_cursor_show(0);
    int selected = 0;

    static u16 desk_save[W * H];
    int desk_sx = 0, desk_sy = 0;
    for (int i = 0; i < W * H; i++) desk_save[i] = vga_buffer()[i];
    vga_getcursor(&desk_sx, &desk_sy);

    char status[64];
    snprintf(status, sizeof(status), "IP %u.%u.%u.%u    driver %s    up %us",
             (g_ip >> 24) & 0xFF, (g_ip >> 16) & 0xFF, (g_ip >> 8) & 0xFF, g_ip & 0xFF,
             "e1000", (u32)pit_seconds());

    for (;;) {
        draw_desktop(selected, status);
        cur_cell = -1;

        int open = -1;
        for (;;) {
            net_poll_all();
            int c = cons_getchar();
            if (c == 27) {   /* ESC 回 Shell：还原 Shell 画面 */
                cursor_erase();
                vga_win_disable();
                for (int i = 0; i < W * H; i++) vga_buffer()[i] = desk_save[i];
                vga_setcursor(desk_sx, desk_sy);
                g_desktop_active = 0;
                return;
            }
            if (c == '\r' || c == '\n' || c == ' ') { cursor_erase(); open = selected; break; }
            if (c == 's' || c == 'S') { shell_shot(); continue; }
            if (c == '\t') { selected = (selected + 1) % N_ICONS; continue; }  /* Tab 换图标 */

            int mx, my; u8 btns;
            mouse_state(&mx, &my, &btns);
            if (mouse_present()) cursor_paint(mx, my);
            int hit = icon_hit(mx, my);
            if (hit >= 0 && hit != selected) {
                cursor_erase();
                selected = hit;
                draw_desktop(selected, status);
            }
            if (mouse_consume_click() && hit >= 0) { cursor_erase(); open = hit; break; }
            __asm__ volatile("hlt");
        }

        if (open == 0) {
            terminal_run();
        } else if (open == 1) {
            /* Files：说明窗口。刻意不列目录 —— 内核侧 fs_list 的签名与用户态
             * api 的 fs_entry[] 数组不同，这里避免误用。 */
            desktop_show_window("Files",
                "File browser is not wired into the desktop yet.\n\n"
                "For now, use the Terminal:\n"
                "  ls /bin        list installed programs\n"
                "  cat <file>     print a text file\n"
                "  pkg list       list installed packages\n\n"
                "Graphical browsing will follow the tinyshell/TinyBrowser work.");
        } else if (open == 2) {
            window_about();
        }
        if (!g_desktop_active) return;
    }
}