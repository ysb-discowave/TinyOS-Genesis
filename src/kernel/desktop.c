/* ================================================================
 * TinyOS Genesis 图形化桌面 / 窗口管理器（80x25 VGA 文本模式 + CP437）
 * ---------------------------------------------------------------
 * 文本模式没有位图可用，但 CP437 自带双线框、阴影块、方块字符，
 * 足够拼出一个"有设计感"的桌面：
 *   · 斜向抖动渐变壁纸（fg/bg 双色 ░ 点阵）
 *   · 多窗口叠放（z 序）、焦点、最小化、最大化
 *   · 双线边框 + 投影 + 立体标题栏的窗口；标题栏可拖动，
 *     右下角 ▒ 把手可缩放，双击标题栏最大化/还原
 *   · 布局管理：cascade（层叠）/ tile（平铺）/ stack（堆叠）
 *   · 图标网格（悬停/选中高亮）+ 底部任务栏（窗口按钮 + 布局切换）
 *   · 客户端（终端 / 文件管理器 / 关于）经 client_vt 接入：
 *     只负责客户区内容，边框/标题栏/任务栏由 WM 绘制
 * 约定：所有可见文案用 ASCII —— VGA 文本模式字形表是 CP437，画不了汉字。
 * ================================================================ */
#include "desktop.h"
#include "vga.h"
#include "console.h"
#include "process.h"
#include "shell.h"
#include "mouse.h"
#include "pit.h"
#include "user.h"
#include "net/net.h"
#include "libc.h"
#include "keys.h"
#include "task.h"

#define W 80
#define H 25
#define TASKBAR_Y (H - 1)   /* 24：任务栏独占底行 */
#define DESK_H    (H - 1)   /* 桌面可用高度（窗口不得越过任务栏）*/

/* ---- 配色（经典 16 色） ---- */
#define CLR_WALL_BG  0x01   /* 深蓝壁纸 */
#define CLR_WALL_FG  0x09   /* 亮蓝，做抖动纹理 */
#define CLR_BAR_BG   0x00   /* 任务栏/窗口底：黑 */
#define CLR_BAR_FG   0x0F   /* 任务栏字：白 */
#define CLR_ACCENT   0x0B   /* 强调色：亮青 */
#define CLR_ACCENT2  0x09   /* 次强调：亮蓝 */
#define CLR_WIN_FG   0x0F   /* 窗口边框（焦点）：白 */
#define CLR_WIN_DIM  0x08   /* 窗口边框（失焦）：深灰 */
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
#define CH_DH 0xCD   /* ═ 双横 */
#define CH_DV 0xBA   /* ║ 双竖 */
#define CH_SHADE_L 0xB0 /* ░ */
#define CH_BLOCK   0xDB /* █ */
#define CH_GRIP    0xB2 /* ▒ 缩放手柄 */

int g_desktop_active = 0;

/* ================= 基础绘制 ================= */
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
/* 带宽度裁剪的字符串（vga_draw_str 只裁屏幕边缘，不裁窗口） */
static void strc(int x, int y, int maxw, const char *s, u8 fg, u8 bg) {
    int n = 0;
    while (s[n] && n < maxw) { cell(x + n, y, s[n], fg, bg); n++; }
}

/* 斜向抖动渐变壁纸：随 (x+y) 用 ░ 点阵，亮蓝浮在深蓝上，像一层光晕 */
static void draw_wallpaper(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int d = (x * 2 + y) % 7;
            if (d == 0) cell(x, y, (char)CH_SHADE_L, CLR_WALL_FG, CLR_WALL_BG);
            else        cell(x, y, ' ', CLR_WALL_FG, CLR_WALL_BG);
        }
}

/* ================= 窗口池 ================= */
static win_t  g_pool[WM_MAX_WINDOWS];
static int    g_ztop   = 0;      /* 已发出的最大 z 值 */
static int    g_next_id = 1;
static win_t *g_focus  = NULL;
static int    g_dirty  = 1;      /* 需要重绘整屏 */
static int    g_layout = 0;      /* 0=cascade 1=tile 2=stack */

static const char *g_layout_name[3] = { "cascade", "tile", "stack" };

/* ================= 鼠标交互状态 ================= */
enum { DRAG_NONE = 0, DRAG_MOVE, DRAG_RESIZE };
static int    g_drag       = DRAG_NONE;
static win_t *g_drag_win   = NULL;
static int    g_drag_dx, g_drag_dy;
static win_t *g_dclick_win = NULL;
static u32    g_dclick_tick = 0;
static int    g_last_mx = -1, g_last_my = -1;
static int    g_hover_icon = -1;
static int    g_sel_icon   = 0;

/* 任务栏可点区域（绘制时登记，命中测试时使用）*/
typedef struct { int x, w; win_t *win; int kind; } tbtn_t;  /* kind 0=窗口 1=布局 */
#define TB_MAX 12
static tbtn_t g_tb[TB_MAX];
static int    g_tb_n = 0;

/* ================= 桌面图标 ================= */
#define ICON_W 16
#define ICON_H 6
#define ICON_GAP 19
#define ICON_X0 4
#define ICON_Y  3
#define N_ICONS 3

static const char *g_icon_glyph[N_ICONS] = { "  ______  ", " | .  . | ", "  \\_||_/  " };
static const char *g_icon_label[N_ICONS] = { "Terminal", "Files", "Help" };

static int icon_hit(int mx, int my) {
    for (int i = 0; i < N_ICONS; i++) {
        int x = ICON_X0 + i * ICON_GAP;
        if (mx >= x && mx < x + ICON_W && my >= ICON_Y && my < ICON_Y + ICON_H)
            return i;
    }
    return -1;
}

static void draw_icon(int idx, int selected) {
    int x = ICON_X0 + idx * ICON_GAP;
    int y = ICON_Y;
    u8 fg = selected ? CLR_SEL_FG : CLR_BAR_FG;
    u8 bg = selected ? CLR_SEL_BG : 0x00;
    if (selected) fill(x, y, ICON_W, ICON_H, fg, bg);
    hline(x + 1, y,         ICON_W - 2, (char)CH_DH, fg, bg);
    hline(x + 1, y + 3,     ICON_W - 2, (char)CH_DH, fg, bg);
    vline(x,     y,         4,          (char)CH_DV, fg, bg);
    vline(x + ICON_W - 1, y, 4,         (char)CH_DV, fg, bg);
    cell(x,     y,     (char)CH_TL, fg, bg);
    cell(x + ICON_W - 1, y,     (char)CH_TR, fg, bg);
    cell(x,     y + 3, (char)CH_BL, fg, bg);
    cell(x + ICON_W - 1, y + 3, (char)CH_BR, fg, bg);
    vga_draw_str(x + 3, y + 2, g_icon_glyph[idx], fg, bg);
    int lx = x + (ICON_W - (int)strlen(g_icon_label[idx])) / 2;
    vga_draw_str(lx, y + 4, g_icon_label[idx], fg, bg);
}

/* ================= 终端客户端 ================= */
#define TERM_LINES 40
#define TERM_COLS  76

typedef struct {
    int  session;
    int  pid;
    char lines[TERM_LINES][TERM_COLS + 1];
    int  nlines;
    int  cur;              /* 最后一行的当前列 */
    int  view;             /* 回滚偏移（0 = 看最新）*/
    char in[128];
    int  inlen;
} term_st;

static term_st  g_terms[WM_MAX_WINDOWS];
static term_st *g_sink_term = NULL;    /* 当前接收控制台输出的终端 */

static void term_push_line(term_st *t) {
    if (t->nlines < TERM_LINES) {
        t->lines[t->nlines][0] = 0;
        t->nlines++;
    } else {
        for (int i = 1; i < TERM_LINES; i++)
            strncpy(t->lines[i - 1], t->lines[i], TERM_COLS);
        t->lines[TERM_LINES - 1][0] = 0;
    }
    t->cur = 0;
}

/* 控制台输出 sink：kprintf/kputc 的字符进终端滚动缓冲 */
static void term_sink(char c) {
    term_st *t = g_sink_term;
    if (!t) return;
    if (t->nlines == 0) term_push_line(t);
    if (c == '\n') { term_push_line(t); return; }
    if (c == '\r') { t->cur = 0; return; }
    if (c == '\b') {
        if (t->cur > 0) { t->cur--; t->lines[t->nlines - 1][t->cur] = 0; }
        return;
    }
    if (c == '\t') { for (int i = 0; i < 4; i++) term_sink(' '); return; }
    if (c < 32) return;
    if (t->cur < TERM_COLS) {
        t->lines[t->nlines - 1][t->cur] = (char)c;
        t->cur++;
        t->lines[t->nlines - 1][t->cur] = 0;
    }
}

static void term_draw(win_t *w) {
    term_st *t = w->client;
    if (!t) return;
    int rows = w->client_h - 1;            /* 最后一行留给输入行 */
    if (rows < 1) rows = 1;
    int start = t->nlines - rows - t->view;
    if (start < 0) start = 0;
    for (int r = 0; r < rows; r++) {
        int li = start + r;
        const char *s = (li >= 0 && li < t->nlines) ? t->lines[li] : "";
        wstrn(w, 0, r, w->client_w, s, 0x0F, 0x00);
    }
    /* 输入行：提示符 + 已键入内容 + 块光标 */
    int ir = w->client_h - 1;
    wfill(w, 0, ir, w->client_w, 1, 0x0F, 0x00);
    char pr[80];
    shell_prompt(pr, sizeof pr);
    wstrn(w, 0, ir, w->client_w, pr, 0x0A, 0x00);
    int pl = (int)strlen(pr);
    wstrn(w, pl, ir, w->client_w, t->in, 0x0F, 0x00);
    int cx = pl + t->inlen;
    if (cx >= 0 && cx < w->client_w) wput(w, cx, ir, (char)CH_BLOCK, 0x0F, 0x00);
}

static void term_key(win_t *w, int c) {
    term_st *t = w->client;
    if (!t) return;
    switch (c) {
    case KEY_ESC:                                   /* ESC 关窗（旧行为）*/
        wm_close(w);
        return;
    case KEY_UP:
        if (t->view < t->nlines) t->view++;
        g_dirty = 1; return;
    case KEY_DOWN:
        if (t->view > 0) t->view--;
        g_dirty = 1; return;
    case KEY_PGUP:
        t->view += 5;
        if (t->view > t->nlines) t->view = t->nlines;
        g_dirty = 1; return;
    case KEY_PGDN:
        t->view -= 5;
        if (t->view < 0) t->view = 0;
        g_dirty = 1; return;
    case '\r':
    case '\n': {
        /* 把 "提示符+输入" 回显进滚动缓冲，再执行 */
        char pr[80];
        shell_prompt(pr, sizeof pr);
        kputs(pr);
        if (t->inlen > 0) kputs(t->in);
        kputc('\n');
        if (t->inlen > 0) {
            t->in[t->inlen] = 0;
            if (strcmp(t->in, "exit") == 0) { wm_close(w); return; }
            shell_exec(t->in, t->session);
        }
        t->inlen = 0; t->in[0] = 0; t->view = 0;
        g_dirty = 1;
        return;
    }
    case '\b':
    case 127:
        if (t->inlen > 0) { t->inlen--; t->in[t->inlen] = 0; }
        g_dirty = 1; return;
    default:
        if (c >= 32 && c < 127 && t->inlen < (int)sizeof(t->in) - 1) {
            t->in[t->inlen++] = (char)c;
            t->in[t->inlen] = 0;
        }
        g_dirty = 1; return;
    }
}

static const client_vt term_vt = {
    .draw  = term_draw,
    .key   = term_key,
    .click = NULL,
    .close = NULL,
    .busy  = NULL,
};

/* ================= 关于客户端 ================= */
static void about_draw(win_t *w) {
    char buf[600];
    snprintf(buf, sizeof buf,
             "TinyOS Genesis v0.1\n"
             "i386 protected mode, bare metal\n"
             "\n"
             "  WM       : multi-window, drag/resize, cascade/tile/stack\n"
             "  Tasks    : cooperative scheduler, IPC, watchdog, respawn\n"
             "  Shell    : tinysh (TNCR)\n"
             "  Packages : pkg / pack\n"
             "  Sessions : %d\n"
             "\n"
             "  IP  %u.%u.%u.%u\n"
             "  GW  %u.%u.%u.%u\n"
             "  Uptime %us\n"
             "\n"
             "  Mouse: drag title bar to move, bottom-right grip to\n"
             "  resize, double-click title bar to maximize.\n"
             "  Keys : F2 or Ctrl+L = layout, Ctrl+W = close, ESC = back.",
             proc_count(),
             (g_ip >> 24) & 0xFF, (g_ip >> 16) & 0xFF, (g_ip >> 8) & 0xFF, g_ip & 0xFF,
             (g_gw >> 24) & 0xFF, (g_gw >> 16) & 0xFF, (g_gw >> 8) & 0xFF, g_gw & 0xFF,
             (u32)pit_seconds());
    /* 按客户区宽度折行 */
    char line[100];
    int li = 0, cy = 0;
    const char *p = buf;
    while (*p && cy < w->client_h) {
        if (*p == '\n' || li >= w->client_w) {
            line[li] = 0;
            wstr(w, 0, cy, line, 0x0F, 0x00);
            cy++; li = 0;
            if (*p == '\n') { p++; continue; }
            continue;
        }
        line[li++] = *p++;
    }
    if (li > 0 && cy < w->client_h) { line[li] = 0; wstr(w, 0, cy, line, 0x0F, 0x00); }
}

static const client_vt about_vt = {
    .draw  = about_draw,
    .key   = NULL,
    .click = NULL,
    .close = NULL,
    .busy  = NULL,
};

/* ================= WM 核心 ================= */
static int wm_count(void) {
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_pool[i].id) n++;
    return n;
}

static win_t *wm_top_visible(void) {
    win_t *best = NULL;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        win_t *w = &g_pool[i];
        if (!w->id || w->minimized) continue;
        if (!best || w->z > best->z) best = w;
    }
    return best;
}

/* 焦点/终端 sink 一致性：焦点是终端则用它，否则退回任意一个终端 */
static void refresh_sink(void) {
    if (g_focus && g_focus->id && g_focus->type == WC_TERMINAL && g_focus->client) {
        g_sink_term = g_focus->client;
        return;
    }
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        win_t *w = &g_pool[i];
        if (w->id && w->type == WC_TERMINAL && w->client) { g_sink_term = w->client; return; }
    }
    g_sink_term = NULL;
}

win_t *wm_open(const client_vt *vt, wclient_type type, const char *title,
               int w, int h) {
    win_t *win = NULL;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (!g_pool[i].id) { win = &g_pool[i]; break; }
    if (!win) return NULL;

    /* 层联初始位置：每开一个窗偏移一点，越界回拉。
     * 计数必须在分配 id 之前做（wm_count 按 id 判在用），
     * 否则会把正在创建的这个窗口也算进去，首个窗口就偏移一步。 */
    int n = wm_count();

    memset(win, 0, sizeof *win);
    win->id   = g_next_id++;
    win->w    = w > W ? W : w;
    win->h    = h > DESK_H ? DESK_H : h;
    win->client_w = win->w - 2;
    win->client_h = win->h - 3;
    strncpy(win->title, title ? title : "Window", sizeof win->title - 1);
    win->type = type;
    win->vt   = vt;

    win->x = 2 + 3 * n;
    win->y = 1 + n;
    if (win->x + win->w > W)      win->x = W - win->w;
    if (win->y + win->h > DESK_H) win->y = DESK_H - win->h;
    if (win->x < 0) win->x = 0;
    if (win->y < 0) win->y = 0;
    win->z = ++g_ztop;
    win->user_w = win->w;         /* 初始尺寸即"用户尺寸"，layout 之后要还回来 */
    win->user_h = win->h;

    if (type == WC_TERMINAL) {
        term_st *t = &g_terms[win - g_pool];
        memset(t, 0, sizeof *t);
        win->client = t;
        g_focus = win;
        if (!user_logged_in()) {
            /* 登录交互要走裸 VGA（桌面还没合成，sink 此时无处可去）：
             * 临时摘掉 sink，让登录提示直接可见，登完再装回。 */
            console_set_sink(NULL);
            shell_login_loop();
            console_set_sink(term_sink);
        }
        g_sink_term = t;                 /* 之后的输出去这个窗口 */
        t->session = proc_new_session(SESS_TERMINAL, "terminal");
        t->pid     = proc_spawn("terminal", t->session);
        term_push_line(t);
        kputs("TinyOS terminal - type 'help'; ESC closes the window.\n");
    }

    g_focus = win;
    refresh_sink();
    g_dirty = 1;
    return win;
}

void wm_close(win_t *w) {
    if (!w || !w->id) return;
    if (w->vt && w->vt->close) w->vt->close(w);
    if (w->type == WC_TERMINAL && w->client) {
        term_st *t = w->client;
        if (t->pid > 0) proc_finish(t->pid);
    }
    int was_focus = (g_focus == w);
    w->id = 0;
    w->client = NULL;
    w->vt = NULL;
    if (was_focus) {
        g_focus = wm_top_visible();
        refresh_sink();
    } else {
        refresh_sink();
    }
    g_dirty = 1;
}

win_t *wm_focused(void) { return g_focus; }

void wm_focus(win_t *w) {
    if (!w || !w->id) return;
    w->minimized = 0;
    w->z = ++g_ztop;
    g_focus = w;
    refresh_sink();
    g_dirty = 1;
}

void wm_minimize(win_t *w) {
    if (!w || !w->id) return;
    w->minimized = 1;
    if (g_focus == w) {
        g_focus = wm_top_visible();
        refresh_sink();
    }
    g_dirty = 1;
}

/* 客户端（fm 等）改完自己的状态后调这个：否则桌面循环不知道要重绘，
 * fm_poll 也就不会跑——排队里的文件任务永远分派不出去。 */
void wm_invalidate(void) { g_dirty = 1; }

static void wm_toggle_max(win_t *w) {
    if (!w || !w->id) return;
    if (w->maximized) {
        w->x = w->save_x; w->y = w->save_y;
        w->w = w->save_w; w->h = w->save_h;
        w->maximized = 0;
    } else {
        w->save_x = w->x; w->save_y = w->y;
        w->save_w = w->w; w->save_h = w->h;
        w->user_w = w->w; w->user_h = w->h;   /* 最大化前这一刻的尺寸就是用户尺寸 */
        w->x = 0; w->y = 0; w->w = W; w->h = DESK_H;
        w->maximized = 1;
    }
    w->client_w = w->w - 2;
    w->client_h = w->h - 3;
    g_dirty = 1;
}

void wm_layout(int mode) {
    g_layout = ((mode % 3) + 3) % 3;
    win_t *list[WM_MAX_WINDOWS];
    int cnt = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_pool[i].id && !g_pool[i].minimized) list[cnt++] = &g_pool[i];
    if (cnt == 0) { g_dirty = 1; return; }

    if (g_layout == 0) {                 /* cascade：逐个错开 */
        for (int i = 0; i < cnt; i++) {
            win_t *w = list[i];
            w->maximized = 0;
            /* tile/stack 会改掉 w/h，这里必须把用户亲手拉过的尺寸还回来，
             * 否则切一圈布局，用户 resize 的结果就永久丢了。 */
            if (w->user_w > 0) { w->w = w->user_w; w->h = w->user_h; }
            w->x = 2 + 3 * i;
            w->y = 1 + i;
            if (w->x + w->w > W)      w->x = W - w->w;
            if (w->y + w->h > DESK_H) w->y = DESK_H - w->h;
            if (w->x < 0) w->x = 0;
            if (w->y < 0) w->y = 0;
            w->client_w = w->w - 2;
            w->client_h = w->h - 3;
            w->z = ++g_ztop;
        }
    } else if (g_layout == 1) {          /* tile：网格均分 */
        int cols = 1;
        while (cols * cols < cnt) cols++;
        int rows = (cnt + cols - 1) / cols;
        int cw = W / cols;
        int chh = DESK_H / rows;
        if (cw < 20) cw = 20;
        if (chh < 8) chh = 8;
        for (int i = 0; i < cnt; i++) {
            win_t *w = list[i];
            int cx = i % cols, cy = i / cols;
            w->maximized = 0;
            w->x = cx * cw;
            w->y = cy * chh;
            w->w = cw;
            w->h = chh;
            w->client_w = w->w - 2;
            w->client_h = w->h - 3;
            w->z = ++g_ztop;
        }
    } else {                             /* stack：全部铺满，按 z 叠 */
        for (int i = 0; i < cnt; i++) {
            win_t *w = list[i];
            w->maximized = 0;
            w->x = 0; w->y = 0; w->w = W; w->h = DESK_H;
            w->client_w = w->w - 2;
            w->client_h = w->h - 3;
            w->z = ++g_ztop;
        }
    }
    g_dirty = 1;
}

int wm_layout_mode(void) { return g_layout; }

/* ================= 客户区绘制助手 ================= */
void wput(win_t *w, int cx, int cy, char ch, u8 fg, u8 bg) {
    if (!w || cx < 0 || cy < 0 || cx >= w->client_w || cy >= w->client_h) return;
    cell(w->x + 1 + cx, w->y + 2 + cy, ch, fg, bg);
}
void wstr(win_t *w, int cx, int cy, const char *s, u8 fg, u8 bg) {
    if (!w || !s) return;
    while (*s && cx < w->client_w) { wput(w, cx, cy, *s++, fg, bg); cx++; }
}
void wstrn(win_t *w, int cx, int cy, int maxw, const char *s, u8 fg, u8 bg) {
    if (!w || !s) return;
    int n = 0;
    while (s[n] && n < maxw) { wput(w, cx + n, cy, s[n], fg, bg); n++; }
}
void wfill(win_t *w, int cx, int cy, int cw, int chh, u8 fg, u8 bg) {
    if (!w) return;
    for (int j = 0; j < chh; j++)
        for (int i = 0; i < cw; i++)
            wput(w, cx + i, cy + j, ' ', fg, bg);
}

/* ================= 窗口绘制 ================= */
static void draw_window(win_t *w) {
    int focused = (w == g_focus);
    u8 fb  = focused ? CLR_WIN_FG : CLR_WIN_DIM;   /* 边框 */
    u8 tbg = focused ? CLR_ACCENT2 : 0x08;         /* 标题栏底 */
    u8 tfg = focused ? CLR_SEL_FG : 0x07;          /* 标题栏字 */

    /* 投影（右下偏移一格；最大化时不画）*/
    if (!w->maximized) {
        fill(w->x + 2, w->y + w->h, w->w - 2, 1, CLR_SHADOW, CLR_SHADOW);
        fill(w->x + w->w, w->y + 1, 2, w->h, CLR_SHADOW, CLR_SHADOW);
    }

    /* 双线框 */
    hline(w->x, w->y,             w->w, (char)CH_DH, fb, CLR_BAR_BG);
    hline(w->x, w->y + w->h - 1,  w->w, (char)CH_DH, fb, CLR_BAR_BG);
    vline(w->x,         w->y, w->h, (char)CH_DV, fb, CLR_BAR_BG);
    vline(w->x + w->w - 1, w->y, w->h, (char)CH_DV, fb, CLR_BAR_BG);
    cell(w->x,                 w->y,             (char)CH_TL, fb, CLR_BAR_BG);
    cell(w->x + w->w - 1,       w->y,             (char)CH_TR, fb, CLR_BAR_BG);
    cell(w->x,                 w->y + w->h - 1, (char)CH_BL, fb, CLR_BAR_BG);
    cell(w->x + w->w - 1,       w->y + w->h - 1, (char)CH_BR, fb, CLR_BAR_BG);

    /* 标题栏：标题 + [─][□][x] */
    fill(w->x + 1, w->y + 1, w->w - 2, 1, tfg, tbg);
    strc(w->x + 2, w->y + 1, w->w - 9, w->title, tfg, tbg);
    int bx = w->x + w->w - 6;
    cell(bx,     w->y + 1, '-',           tfg, tbg);
    cell(bx + 2, w->y + 1, (char)0xFE,    tfg, tbg);   /* □ */
    cell(bx + 4, w->y + 1, 'x',           tfg, tbg);

    /* 缩放手柄：底排右四格画 ▒ */
    if (!w->maximized) {
        for (int i = 1; i <= 4 && i < w->w; i++)
            cell(w->x + w->w - i, w->y + w->h - 1, (char)CH_GRIP, CLR_ACCENT, CLR_BAR_BG);
    }

    /* 客户区 */
    if (w->vt && w->vt->draw) w->vt->draw(w);
}

/* ================= 任务栏 ================= */
static void draw_taskbar(void) {
    int ty = TASKBAR_Y;
    fill(0, ty, W, 1, CLR_BAR_FG, CLR_BAR_BG);
    vga_draw_str(1, ty, "[Genesis]", 0x00, CLR_ACCENT);

    int x = 12;
    g_tb_n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS && g_tb_n < TB_MAX; i++) {
        win_t *w = &g_pool[i];
        if (!w->id) continue;
        char b[48];
        snprintf(b, sizeof b, "%s%d:%s", w->minimized ? "*" : "", w->id, w->title);
        int bw = (int)strlen(b) + 2;
        if (x + bw > W - 26) break;
        vga_draw_str(x + 1, ty, b,
                     w == g_focus ? 0x00 : 0x07,
                     w == g_focus ? CLR_ACCENT : 0x07);
        g_tb[g_tb_n].x = x; g_tb[g_tb_n].w = bw;
        g_tb[g_tb_n].win = w; g_tb[g_tb_n].kind = 0;
        g_tb_n++;
        x += bw + 1;
    }

    /* 布局切换按钮 */
    char lb[40];
    snprintf(lb, sizeof lb, "[Layout: %s]", g_layout_name[g_layout]);
    int lw = (int)strlen(lb);
    int lx = W - 24 - lw;
    if (lx > x + 1) {
        vga_draw_str(lx, ty, lb, 0x00, CLR_ACCENT);
        if (g_tb_n < TB_MAX) {
            g_tb[g_tb_n].x = lx; g_tb[g_tb_n].w = lw;
            g_tb[g_tb_n].win = NULL; g_tb[g_tb_n].kind = 1;
            g_tb_n++;
        }
    }

    /* 右侧状态 */
    char st[48];
    snprintf(st, sizeof st, "IP %u.%u.%u.%u up %us",
             (g_ip >> 24) & 0xFF, (g_ip >> 16) & 0xFF, (g_ip >> 8) & 0xFF, g_ip & 0xFF,
             (u32)pit_seconds());
    vga_draw_str(W - (int)strlen(st) - 1, ty, st, CLR_BAR_FG, CLR_BAR_BG);
}

/* ================= 整屏合成 ================= */
static void compose(void) {
    mouse_cursor_erase();
    draw_wallpaper();
    for (int i = 0; i < N_ICONS; i++)
        draw_icon(i, g_sel_icon == i || g_hover_icon == i);
    /* 窗口按 z 从下到上画 */
    for (int lvl = 1; lvl <= g_ztop; lvl++)
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            win_t *w = &g_pool[i];
            if (w->id && !w->minimized && w->z == lvl) draw_window(w);
        }
    draw_taskbar();
    mouse_cursor_paint();
}

/* ================= 打开窗口的入口 ================= */
static void open_terminal_window(void) {
    wm_open(&term_vt, WC_TERMINAL, "Terminal", 70, 18);
}

static void open_about_window(void) {
    wm_open(&about_vt, WC_ABOUT, "About", 60, 16);
}

static void desktop_open_icon(int idx) {
    if (idx == 0)      open_terminal_window();
    else if (idx == 1) fm_open_window();
    else               open_about_window();
}

/* ================= 鼠标事件处理 ================= */
static win_t *win_at(int mx, int my) {
    for (int lvl = g_ztop; lvl >= 1; lvl--)
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            win_t *w = &g_pool[i];
            if (!w->id || w->minimized || w->z != lvl) continue;
            if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h)
                return w;
        }
    return NULL;
}

static void handle_mouse(void) {
    int mx, my; u8 btns;
    mouse_state(&mx, &my, &btns);
    int click = mouse_consume_click();

    /* 拖动/缩放进行中：跟着指针走，松手结束 */
    if (g_drag) {
        if (btns & 1) {
            win_t *w = g_drag_win;
            if (w && w->id) {
                if (g_drag == DRAG_MOVE) {
                    w->x = mx - g_drag_dx;
                    w->y = my - g_drag_dy;
                    if (w->x < 0) w->x = 0;
                    if (w->y < 0) w->y = 0;
                    if (w->x + w->w > W)      w->x = W - w->w;
                    if (w->y + w->h > DESK_H) w->y = DESK_H - w->h;
                } else {
                    int nw = mx - w->x + 1;
                    int nh = my - w->y + 1;
                    if (nw < 20) nw = 20;
                    if (nh < 8)  nh = 8;
                    if (w->x + nw > W)      nw = W - w->x;
                    if (w->y + nh > DESK_H) nh = DESK_H - w->y;
                    w->w = nw; w->h = nh;
                    w->user_w = nw; w->user_h = nh;   /* 记录用户尺寸 */
                    w->client_w = nw - 2;
                    w->client_h = nh - 3;
                }
            }
            g_dirty = 1;
        } else {
            g_drag = DRAG_NONE;
            g_drag_win = NULL;
        }
        return;
    }

    if (!click) return;

    /* 任务栏按钮 */
    if (my == TASKBAR_Y) {
        for (int i = 0; i < g_tb_n; i++) {
            if (mx >= g_tb[i].x && mx < g_tb[i].x + g_tb[i].w) {
                if (g_tb[i].kind == 1)      wm_layout(g_layout + 1);
                else if (g_tb[i].win)       wm_focus(g_tb[i].win);
                return;
            }
        }
        if (mx >= 1 && mx < 10) { open_about_window(); return; }   /* [Genesis] */
        return;
    }

    /* 窗口（最上层优先）*/
    win_t *hit = win_at(mx, my);
    if (hit) {
        wm_focus(hit);
        int lx = mx - hit->x, ly = my - hit->y;
        if (ly == 1) {                                   /* 标题栏 */
            if (lx == hit->w - 2) { wm_close(hit); return; }        /* x */
            if (lx == hit->w - 4) { wm_toggle_max(hit); return; }   /* □ */
            if (lx == hit->w - 6) { wm_minimize(hit); return; }     /* ─ */
            u32 now = pit_ticks();
            if (g_dclick_win == hit && (u32)(now - g_dclick_tick) < 50) {
                wm_toggle_max(hit);                      /* 双击 = 最大化切换 */
                g_dclick_win = NULL;
            } else {
                g_dclick_win  = hit;
                g_dclick_tick = now;
                g_drag     = DRAG_MOVE;
                g_drag_win = hit;
                g_drag_dx  = lx;
                g_drag_dy  = ly;
            }
        } else if (ly == hit->h - 1 && lx >= hit->w - 4) {          /* 缩放手柄 */
            g_drag     = DRAG_RESIZE;
            g_drag_win = hit;
        } else if (ly >= 2 && ly <= hit->h - 2) {                   /* 客户区 */
            if (hit->vt && hit->vt->click)
                hit->vt->click(hit, lx - 1, ly - 2);
        }
        return;
    }

    /* 桌面图标 */
    int ic = icon_hit(mx, my);
    if (ic >= 0) {
        g_sel_icon = ic;
        desktop_open_icon(ic);
    }
}

/* ================= 模态弹窗（gui_open 用） =================
 * 保存/恢复整屏；任何键或点击关闭。桌面激活时由 FM 的属性窗和
 * GUI TNCR 程序（经 api->gui_open）使用。 */
void desktop_show_window(const char *title, const char *content) {
    static u16 save[W * H];
    mouse_cursor_erase();                 /* 先擦光标，保证快照是纯净桌面 */
    for (int i = 0; i < W * H; i++) save[i] = vga_buffer()[i];

    int bw = 56, bh = 13;
    if (bw > W - 6) bw = W - 6;
    if (bh > H - 4) bh = H - 4;
    int bx = (W - bw) / 2, by = 3 + (H - 2 - bh) / 2;

    /* 投影 + 双线框 + 标题栏 */
    fill(bx + 2, by + bh, bw - 2, 1, CLR_SHADOW, CLR_SHADOW);
    fill(bx + bw, by + 1, 2, bh, CLR_SHADOW, CLR_SHADOW);
    hline(bx, by,         bw, (char)CH_DH, CLR_WIN_FG, CLR_BAR_BG);
    hline(bx, by + bh - 1, bw, (char)CH_DH, CLR_WIN_FG, CLR_BAR_BG);
    vline(bx,         by, bh, (char)CH_DV, CLR_WIN_FG, CLR_BAR_BG);
    vline(bx + bw - 1, by, bh, (char)CH_DV, CLR_WIN_FG, CLR_BAR_BG);
    cell(bx,         by,         (char)CH_TL, CLR_WIN_FG, CLR_BAR_BG);
    cell(bx + bw - 1, by,         (char)CH_TR, CLR_WIN_FG, CLR_BAR_BG);
    cell(bx,         by + bh - 1, (char)CH_BL, CLR_WIN_FG, CLR_BAR_BG);
    cell(bx + bw - 1, by + bh - 1, (char)CH_BR, CLR_WIN_FG, CLR_BAR_BG);
    fill(bx + 1, by + 1, bw - 2, 1, CLR_ACCENT2, CLR_ACCENT2);
    vga_draw_str(bx + 2, by + 1, title ? title : "Window", CLR_SEL_FG, CLR_ACCENT2);

    /* 内容（按宽度切行）*/
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

    for (;;) {
        net_poll_all();
        if (cons_getchar() >= 0) break;
        if (mouse_consume_click()) break;
        mouse_cursor_erase();
        mouse_cursor_paint();
        __asm__ volatile("hlt");
    }
    mouse_cursor_erase();
    for (int i = 0; i < W * H; i++) vga_buffer()[i] = save[i];
    mouse_cursor_paint();
    g_dirty = 1;
}

/* ================= 桌面主循环 ================= */
void desktop_enter(void) {
    g_desktop_active = 1;
    vga_cursor_show(0);
    g_sel_icon = 0;
    g_hover_icon = -1;

    static u16 desk_save[W * H];
    int desk_sx = 0, desk_sy = 0;
    for (int i = 0; i < W * H; i++) desk_save[i] = vga_buffer()[i];
    vga_getcursor(&desk_sx, &desk_sy);

    console_set_sink(term_sink);          /* 终端输出进窗口缓冲，串口仍镜像 */
    mouse_set_style(MOUSE_STYLE_INVERT);
    {
        int mx, my; u8 bt;
        mouse_state(&mx, &my, &bt);
        g_last_mx = mx; g_last_my = my;
    }
    g_dirty = 1;

    for (;;) {
        net_poll_all();
        sched_run();                      /* 给 worker 任务（文件操作）CPU */

        /* 忙碌光标：任一客户端忙碌 -> ¤ */
        int busy = 0;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            win_t *w = &g_pool[i];
            if (w->id && w->vt && w->vt->busy && w->vt->busy(w)) { busy = 1; break; }
        }
        mouse_set_style(busy ? MOUSE_STYLE_BUSY : MOUSE_STYLE_INVERT);
        if (busy) g_dirty = 1;            /* 进度条/状态持续刷新 */

        /* ---- 键盘 ---- */
        int k = cons_getkey();
        if (k >= 0) {
            if (k == '\t') {
                shell_shot();                        /* Tab = 串口截图（全局）*/
            } else if (k == KEY_F2 || k == KEY_CTRL('L')) {
                wm_layout(g_layout + 1);              /* 布局循环 */
            } else if (k == KEY_CTRL('W') && g_focus) {
                wm_close(g_focus);                    /* 关焦点窗 */
            } else if (k == KEY_CTRL('Q')) {
                goto exit_desktop;                    /* 退出桌面 */
            } else if (g_focus && g_focus->vt && g_focus->vt->key) {
                g_focus->vt->key(g_focus, k);
            } else {
                switch (k) {                          /* 桌面级按键 */
                case '\t':
                    g_sel_icon = (g_sel_icon + 1) % N_ICONS;
                    g_dirty = 1;
                    break;
                case '\r':
                case ' ':
                    desktop_open_icon(g_sel_icon);
                    break;
                case 's':
                case 'S':
                    shell_shot();
                    break;
                case KEY_ESC:
                    goto exit_desktop;
                default:
                    break;
                }
            }
        }

        /* ---- 鼠标 ---- */
        handle_mouse();

        /* ---- 渲染 ---- */
        if (g_dirty) {
            g_dirty = 0;
            compose();
        } else {
            int mx, my; u8 bt;
            mouse_state(&mx, &my, &bt);
            if (mx != g_last_mx || my != g_last_my) {
                g_last_mx = mx; g_last_my = my;
                int hv = icon_hit(mx, my);
                if (hv != g_hover_icon) { g_hover_icon = hv; g_dirty = 1; }
                else { mouse_cursor_erase(); mouse_cursor_paint(); }
            }
        }
        __asm__ volatile("hlt");
    }

exit_desktop:
    /* 收尾：关所有窗口（终端进程/文件 worker 由各自 close 回收），
     * 恢复 Shell 画面与默认控制台行为。 */
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_pool[i].id) wm_close(&g_pool[i]);
    console_set_sink(NULL);
    mouse_cursor_erase();
    for (int i = 0; i < W * H; i++) vga_buffer()[i] = desk_save[i];
    vga_setcursor(desk_sx, desk_sy);
    vga_cursor_show(1);
    g_desktop_active = 0;
    g_focus = NULL;
    g_sink_term = NULL;
}
