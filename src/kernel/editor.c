#include "editor.h"
#include "vga.h"
#include "console.h"
#include "vfs.h"
#include "mm.h"
#include "libc.h"
#include "keys.h"

/* ============================================================
 * 屏幕布局（80x25 文本模式）
 *   第 0 行          标题栏（反色）：文件名 / 修改标记 / 行号列号
 *   第 1 .. 23 行    文本区（左侧 5 列行号槽 + 75 列正文）
 *   第 24 行         状态栏 / 命令行（反色）
 *
 * 编辑器的界面文字使用 ASCII —— VGA 文本模式只有 CP437 字形，
 * 中文提示只出现在串口日志里（kprintf），以免屏幕上出现乱码。
 * ============================================================ */

#define ED_GUTTER  5
#define ED_TEXTW   (VGA_WIDTH - ED_GUTTER)   /* 75 */
#define ED_TOP     1
#define ED_ROWS    (VGA_HEIGHT - 2)          /* 23 */
#define ED_STATUS  (VGA_HEIGHT - 1)          /* 24 */
#define ED_TAB     4
#define ED_LINE_MAX 4000                     /* 单行字符上限 */

/* 颜色属性 */
#define C_TITLE    0x70   /* 黑底亮灰块上的白字 */
#define C_TITLE_TX 0x0F
#define C_TEXT     0x07   /* 浅灰 */
#define C_GUTTER   0x08   /* 深灰 */
#define C_STATUS   0x70
#define C_OK       0x0A
#define C_ERR      0x0C
#define C_HINT     0x0E

/* ------------------------------------------------------------------ */
/* 行缓冲                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    char **line;      /* 行指针数组 */
    int    n;         /* 已用行数 */
    int    cap;       /* 数组容量 */
} ed_buf;

static ed_buf B;
static char fname[256];          /* 绝对路径；空 = 未命名 */
static int  cx, cy;              /* 光标：列（字符下标）、行 */
static int  top, left;           /* 屏幕滚动：首行、水平偏移 */
static int  dirty;               /* 有未保存修改 */
static char msg[96];             /* 状态栏消息（下一次按键后清除） */
static u8   msg_color = C_OK;
static char clip[1024];          /* 行剪贴板 */

static int ed_save_as(void);     /* 前置声明 */

/* ------------------------------------------------------------------ */
/* 底层绘制（直接写 VGA 文本缓冲）                                     */
/* ------------------------------------------------------------------ */
static void ed_put(int x, int y, char c, u8 fg, u8 bg) {
    if (x < 0 || y < 0 || x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    vga_buffer()[y * VGA_WIDTH + x] = (u16)((((u16)((bg << 4) | fg)) << 8) | (u8)c);
}

static void ed_str(int x, int y, const char *s, u8 fg, u8 bg) {
    while (*s) ed_put(x++, y, *s++, fg, bg);
}

static void ed_fill(int x, int y, int w, u8 fg, u8 bg) {
    for (int i = 0; i < w; i++) ed_put(x + i, y, ' ', fg, bg);
}

/* 右对齐 4 位行号 + 空格，共 5 字符 */
static void ed_num5(char *o, int v) {
    if (v > 9999) v = 9999;
    if (v < 0) v = 0;
    o[0] = (v >= 1000) ? (char)('0' + (v / 1000) % 10) : ' ';
    o[1] = (v >= 100)  ? (char)('0' + (v / 100) % 10)  : ' ';
    o[2] = (v >= 10)   ? (char)('0' + (v / 10) % 10)   : ' ';
    o[3] = (char)('0' + (v % 10));
    o[4] = ' ';
    o[5] = 0;
}

/* 行内前 n 个字符占用的显示列数（制表符按 4 对齐展开） */
static int vis_col(const char *s, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\t') c = (c + ED_TAB) & ~(ED_TAB - 1);
        else c++;
    }
    return c;
}

static void ed_msg(const char *m, u8 color) {
    strncpy(msg, m, sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = 0;
    msg_color = color;
}

/* ------------------------------------------------------------------ */
/* 行缓冲操作                                                         */
/* ------------------------------------------------------------------ */
static int ed_ensure(int need) {
    if (need <= B.cap) return 0;
    int nc = B.cap ? B.cap : 32;
    while (nc < need) nc *= 2;
    char **nl = (char**)krealloc(B.line, (size_t)nc * sizeof(char*));
    if (!nl) return -1;
    B.line = nl;
    B.cap = nc;
    return 0;
}

static char *dup_n(const char *s, int n) {
    char *p = (char*)kmalloc((size_t)n + 1);
    if (!p) return NULL;
    if (n > 0) memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}

static int ed_insert_row(int i, const char *s, int n) {
    if (ed_ensure(B.n + 1) != 0) return -1;
    char *p = dup_n(s, n);
    if (!p) return -1;
    for (int j = B.n; j > i; j--) B.line[j] = B.line[j - 1];
    B.line[i] = p;
    B.n++;
    return 0;
}

static void ed_remove_row(int i) {
    if (i < 0 || i >= B.n) return;
    kfree(B.line[i]);
    for (int j = i; j < B.n - 1; j++) B.line[j] = B.line[j + 1];
    B.n--;
}

static void ed_set_row(int i, const char *s, int n) {
    char *p = dup_n(s, n);
    if (!p) { ed_msg("Out of memory", C_ERR); return; }
    kfree(B.line[i]);
    B.line[i] = p;
}

static void ed_free_all(void) {
    for (int i = 0; i < B.n; i++) kfree(B.line[i]);
    if (B.line) kfree(B.line);
    B.line = NULL; B.n = 0; B.cap = 0;
}

static int line_len(int i) {
    if (i < 0 || i >= B.n) return 0;
    return (int)strlen(B.line[i]);
}

/* 文本 -> 行数组 */
static int ed_load(const u8 *data, u32 size) {
    int start = 0;
    for (u32 i = 0; i <= size; i++) {
        if (i == size || data[i] == '\n') {
            int e = (int)i;
            if (e > start && data[e - 1] == '\r') e--;   /* 兼容 CRLF */
            if (ed_insert_row(B.n, (const char*)data + start, e - start) != 0)
                return -1;
            start = (int)i + 1;
        }
    }
    if (B.n == 0) ed_insert_row(0, "", 0);
    return 0;
}

/* 行数组 -> 文本（调用者 kfree） */
static u8 *ed_serialize(u32 *out) {
    u32 total = 0;
    for (int i = 0; i < B.n; i++) total += (u32)line_len(i) + 1;
    if (total) total--;                       /* 末尾不加换行 */
    u8 *buf = (u8*)kmalloc(total ? total : 1);
    if (!buf) return NULL;
    u32 o = 0;
    for (int i = 0; i < B.n; i++) {
        int l = line_len(i);
        if (l) { memcpy(buf + o, B.line[i], (size_t)l); o += (u32)l; }
        if (i != B.n - 1) buf[o++] = '\n';
    }
    *out = o;
    return buf;
}

/* ------------------------------------------------------------------ */
/* 编辑动作                                                           */
/* ------------------------------------------------------------------ */
static void ed_insert_char(char c) {
    int l = line_len(cy);
    if (l >= ED_LINE_MAX) { ed_msg("Line too long", C_ERR); return; }
    char *p = (char*)kmalloc((size_t)l + 2);
    if (!p) { ed_msg("Out of memory", C_ERR); return; }
    if (cx > 0) memcpy(p, B.line[cy], (size_t)cx);
    p[cx] = c;
    if (l - cx > 0) memcpy(p + cx + 1, B.line[cy] + cx, (size_t)(l - cx));
    p[l + 1] = 0;
    kfree(B.line[cy]);
    B.line[cy] = p;
    cx++;
    dirty = 1;
}

static void ed_backspace(void) {
    if (cx > 0) {
        int l = line_len(cy);
        char *p = (char*)kmalloc((size_t)l);
        if (!p) return;
        if (cx - 1 > 0) memcpy(p, B.line[cy], (size_t)(cx - 1));
        if (l - cx > 0) memcpy(p + cx - 1, B.line[cy] + cx, (size_t)(l - cx));
        p[l - 1] = 0;
        kfree(B.line[cy]);
        B.line[cy] = p;
        cx--;
        dirty = 1;
    } else if (cy > 0) {
        int pl = line_len(cy - 1);
        int l  = line_len(cy);
        char *p = (char*)kmalloc((size_t)pl + l + 1);
        if (!p) return;
        if (pl) memcpy(p, B.line[cy - 1], (size_t)pl);
        if (l)  memcpy(p + pl, B.line[cy], (size_t)l);
        p[pl + l] = 0;
        kfree(B.line[cy - 1]);
        B.line[cy - 1] = p;
        ed_remove_row(cy);
        cy--;
        cx = pl;
        dirty = 1;
    }
}

static void ed_delete(void) {
    int l = line_len(cy);
    if (cx < l) {
        char *p = (char*)kmalloc((size_t)l);
        if (!p) return;
        if (cx > 0) memcpy(p, B.line[cy], (size_t)cx);
        if (l - cx - 1 > 0) memcpy(p + cx, B.line[cy] + cx + 1, (size_t)(l - cx - 1));
        p[l - 1] = 0;
        kfree(B.line[cy]);
        B.line[cy] = p;
        dirty = 1;
    } else if (cy + 1 < B.n) {
        int nl = line_len(cy + 1);
        char *p = (char*)kmalloc((size_t)l + nl + 1);
        if (!p) return;
        if (l)  memcpy(p, B.line[cy], (size_t)l);
        if (nl) memcpy(p + l, B.line[cy + 1], (size_t)nl);
        p[l + nl] = 0;
        kfree(B.line[cy]);
        B.line[cy] = p;
        ed_remove_row(cy + 1);
        dirty = 1;
    }
}

static void ed_newline(void) {
    int l = line_len(cy);
    if (B.n >= 20000) { ed_msg("Too many lines", C_ERR); return; }
    if (ed_ensure(B.n + 1) != 0) { ed_msg("Out of memory", C_ERR); return; }

    /* 先把光标后的后半段复制成新行（ed_insert_row 内部会复制） */
    if (ed_insert_row(cy + 1, B.line[cy] + cx, l - cx) != 0) {
        ed_msg("Out of memory", C_ERR);
        return;
    }
    /* 再把当前行截断到光标处 */
    char *p = (char*)kmalloc((size_t)cx + 1);
    if (p) {
        if (cx > 0) memcpy(p, B.line[cy], (size_t)cx);
        p[cx] = 0;
        kfree(B.line[cy]);
        B.line[cy] = p;
    }
    cy++;
    cx = 0;
    dirty = 1;
}

static void ed_tab(void) {
    int n = ED_TAB - (vis_col(B.line[cy], cx) % ED_TAB);
    for (int i = 0; i < n; i++) ed_insert_char(' ');
}

static void ed_cut_line(void) {
    strncpy(clip, B.line[cy], sizeof(clip) - 1);
    clip[sizeof(clip) - 1] = 0;
    if (B.n == 1) {
        ed_set_row(0, "", 0);
        cx = 0;
    } else {
        ed_remove_row(cy);
        if (cy >= B.n) cy = B.n - 1;
        int l = line_len(cy);
        if (cx > l) cx = l;
    }
    dirty = 1;
    ed_msg("Cut line", C_HINT);
}

static void ed_paste(void) {
    if (!clip[0]) { ed_msg("Clipboard empty", C_HINT); return; }
    for (const char *p = clip; *p; p++) ed_insert_char(*p);
    ed_msg("Pasted", C_HINT);
}

/* ------------------------------------------------------------------ */
/* 视图                                                               */
/* ------------------------------------------------------------------ */
static void ed_scroll_into_view(void) {
    int vc = vis_col(B.line[cy], cx);
    if (vc < left) left = vc;
    if (vc >= left + ED_TEXTW) left = vc - ED_TEXTW + 1;
    if (left < 0) left = 0;
    if (cy < top) top = cy;
    if (cy >= top + ED_ROWS) top = cy - ED_ROWS + 1;
    if (top < 0) top = 0;
}

static const char *display_name(void) {
    return fname[0] ? fname : "[No Name]";
}

static void ed_render(void) {
    /* --- 标题栏 --- */
    char t[120];
    snprintf(t, sizeof(t), " TinyOS Edit  %s%s   ln %d/%d  col %d ",
             display_name(), dirty ? "  *modified*" : "",
             cy + 1, B.n, cx + 1);
    ed_fill(0, 0, VGA_WIDTH, C_TITLE_TX, C_TITLE);
    ed_str(0, 0, t, C_TITLE_TX, C_TITLE);

    /* --- 文本区 --- */
    for (int i = 0; i < ED_ROWS; i++) {
        int li = top + i;
        int sy = ED_TOP + i;
        ed_fill(0, sy, VGA_WIDTH, C_TEXT, 0x00);
        if (li >= B.n) {
            ed_str(0, sy, "   ~ ", C_GUTTER, 0x00);
            continue;
        }
        char g[8];
        ed_num5(g, li + 1);
        ed_str(0, sy, g, C_GUTTER, 0x00);

        const char *s = B.line[li];
        int l = line_len(li);
        int scol = 0;
        for (int x = 0; x < l; x++) {
            if (scol - left >= ED_TEXTW) break;
            char ch = s[x];
            if (ch == '\t') {
                int nx = (scol + ED_TAB) & ~(ED_TAB - 1);
                for (; scol < nx; scol++) {
                    int dx = ED_GUTTER + scol - left;
                    if (dx >= ED_GUTTER) ed_put(dx, sy, ' ', C_TEXT, 0x00);
                }
                continue;
            }
            if ((u8)ch < 32) ch = (char)0xFE;      /* 控制字符 -> 实心块 */
            int dx = ED_GUTTER + scol - left;
            if (dx >= ED_GUTTER) ed_put(dx, sy, ch, C_TEXT, 0x00);
            scol++;
        }
    }

    /* --- 状态栏 --- */
    ed_fill(0, ED_STATUS, VGA_WIDTH, C_TITLE_TX, C_STATUS);
    if (msg[0]) {
        ed_str(0, ED_STATUS, msg, msg_color, C_STATUS);
    } else {
        ed_str(0, ED_STATUS,
               " ^O/F2 Save   ^X/F10 Exit   ^K Cut   ^U Paste   ^G Help",
               C_HINT, C_STATUS);
    }

    /* --- 硬件光标 --- */
    ed_scroll_into_view();
    int sx = ED_GUTTER + vis_col(B.line[cy], cx) - left;
    if (sx < ED_GUTTER) sx = ED_GUTTER;
    if (sx >= VGA_WIDTH) sx = VGA_WIDTH - 1;
    int sy = ED_TOP + (cy - top);
    if (sy < ED_TOP) sy = ED_TOP;
    if (sy > ED_STATUS - 1) sy = ED_STATUS - 1;
    vga_setcursor(sx, sy);
}

/* ------------------------------------------------------------------ */
/* 交互：状态栏单行输入 / 确认框 / 帮助页                              */
/* ------------------------------------------------------------------ */
static int ed_prompt(const char *label, const char *init, char *out, int max) {
    int n = 0;
    out[0] = 0;
    if (init) {
        strncpy(out, init, (size_t)max - 1);
        out[max - 1] = 0;
        n = (int)strlen(out);
    }
    for (;;) {
        ed_fill(0, ED_STATUS, VGA_WIDTH, C_TITLE_TX, C_STATUS);
        char p[160];
        snprintf(p, sizeof(p), " %s%s", label, out);
        ed_str(0, ED_STATUS, p, C_TITLE_TX, C_STATUS);
        int px = (int)strlen(p);
        vga_setcursor(px < VGA_WIDTH ? px : VGA_WIDTH - 1, ED_STATUS);

        int k = cons_waitkey();
        if (k == KEY_ESC) { out[0] = 0; return -1; }
        if (k == KEY_ENTER || k == '\n') { out[n] = 0; return n; }
        if (k == KEY_BACKSP || k == 127) { if (n > 0) out[--n] = 0; continue; }
        if (k >= 32 && k < 127 && n < max - 1) { out[n++] = (char)k; out[n] = 0; }
    }
}

static int ed_confirm(const char *q) {
    ed_fill(0, ED_STATUS, VGA_WIDTH, C_TITLE_TX, C_STATUS);
    char p[120];
    snprintf(p, sizeof(p), " %s (y/n) ", q);
    ed_str(0, ED_STATUS, p, C_TITLE_TX, C_STATUS);
    for (;;) {
        int k = cons_waitkey();
        if (k == 'y' || k == 'Y') return 1;
        if (k == 'n' || k == 'N' || k == KEY_ESC) return 0;
    }
}

static void ed_help(void) {
    static const char *rows[] = {
        "TinyOS Edit -- built-in text editor",
        "",
        "  move    arrows / Home / End / PgUp / PgDn",
        "  input   Enter=newline, Backspace, Del deletes, Tab=indent (4 spaces)",
        "",
        "  ^O  F2   save",
        "  ^X  F10  exit (asks first if modified)",
        "  ^W  F3   save as",
        "  ^K       cut current line to clipboard",
        "  ^U       paste clipboard at cursor",
        "  ^G       this help page",
        "  ^L       redraw screen",
        "  Esc      cancel current input",
        "",
        "  Saving writes to the VFS; with TinyFS mounted it also flushes to disk,",
        "  files survive a reboot (permanent storage).",
        "",
        "  press any key to return to the editor ...",
    };
    int n = (int)(sizeof(rows) / sizeof(rows[0]));
    for (int y = 0; y < VGA_HEIGHT; y++) {
        ed_fill(0, y, VGA_WIDTH, C_TEXT, 0x00);
        if (y < n) ed_str(0, y, rows[y], y == 0 ? 0x0E : C_TEXT, 0x00);
    }
    cons_waitkey();
}

/* ------------------------------------------------------------------ */
/* 保存                                                               */
/* ------------------------------------------------------------------ */
static void ed_abspath(const char *in, char *out, int n) {
    if (in[0] == '/') { strncpy(out, in, (size_t)n - 1); out[n - 1] = 0; return; }
    char dir[256];
    if (fname[0] == '/') {
        strncpy(dir, fname, 255); dir[255] = 0;
        char *s = strrchr(dir, '/');
        if (s) s[1] = 0; else strcpy(dir, "/home/");
    } else {
        strcpy(dir, "/home/");
    }
    snprintf(out, (size_t)n, "%s%s", dir, in);
}

static int ed_do_save(const char *path) {
    u32 sz = 0;
    u8 *buf = ed_serialize(&sz);
    if (!buf) { ed_msg("Save failed: out of memory", C_ERR); return -1; }
    int r = vfs_write_file(path, buf, sz);
    kfree(buf);
    if (r != 0) { ed_msg("Save failed (disk full / bad path?)", C_ERR); return -1; }

    strncpy(fname, path, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = 0;
    dirty = 0;
    char m[96];
    snprintf(m, sizeof(m), "Wrote %u bytes to %s", sz, path);
    ed_msg(m, C_OK);
    kprintf("[edit] saved %s (%u bytes)\n", path, sz);
    return 0;
}

static int ed_save(void) {
    if (!fname[0]) return ed_save_as();
    return ed_do_save(fname);
}

static int ed_save_as(void) {
    ed_fill(0, ED_STATUS, VGA_WIDTH, C_TITLE_TX, C_STATUS);
    ed_str(0, ED_STATUS, " File name to write: ", C_TITLE_TX, C_STATUS);
    char in[160];
    if (ed_prompt("File name to write: ", fname[0] ? fname : "", in, sizeof(in)) < 0)
        return -1;
    if (!in[0]) { ed_msg("Save cancelled", C_HINT); return -1; }
    char path[256];
    ed_abspath(in, path, sizeof(path));
    return ed_do_save(path);
}

/* ------------------------------------------------------------------ */
/* 主入口                                                             */
/* ------------------------------------------------------------------ */
int editor_open(const char *path) {
    memset(&B, 0, sizeof(B));
    cx = cy = 0;
    top = left = 0;
    dirty = 0;
    msg[0] = 0;
    clip[0] = 0;
    fname[0] = 0;
    if (path && path[0]) { strncpy(fname, path, sizeof(fname) - 1); fname[sizeof(fname) - 1] = 0; }

    if (ed_ensure(32) != 0) { kprintf("edit: out of memory\n"); return -1; }

    u32 sz = 0;
    const u8 *d = fname[0] ? vfs_read_file(fname, &sz) : NULL;
    if (d) {
        if (ed_load(d, sz) != 0) {
            ed_free_all();
            kprintf("edit: load failed (out of memory)\n");
            return -1;
        }
        char m[80];
        snprintf(m, sizeof(m), "Loaded %u bytes from %s", sz, fname);
        ed_msg(m, C_OK);
        kprintf("[edit] opened %s (%u bytes)\n", fname, sz);
    } else {
        if (ed_insert_row(0, "", 0) != 0) { kprintf("edit: out of memory\n"); return -1; }
        if (fname[0]) ed_msg("New file", C_HINT);
    }

    /* 进入前清屏，退出后清屏 —— 避免残留编辑器画面 */
    vga_clear(0x00);
    vga_setcolor(0x0F, 0x00);

    int done = 0;
    while (!done) {
        ed_render();
        int k = cons_waitkey();
        msg[0] = 0;                          /* 状态消息只显示一轮 */

        switch (k) {
        case KEY_UP:
            if (cy > 0) cy--;
            if (cx > line_len(cy)) cx = line_len(cy);
            break;
        case KEY_DOWN:
            if (cy < B.n - 1) cy++;
            if (cx > line_len(cy)) cx = line_len(cy);
            break;
        case KEY_LEFT:
            if (cx > 0) cx--;
            else if (cy > 0) { cy--; cx = line_len(cy); }
            break;
        case KEY_RIGHT:
            if (cx < line_len(cy)) cx++;
            else if (cy < B.n - 1) { cy++; cx = 0; }
            break;
        case KEY_HOME: cx = 0; break;
        case KEY_END:  cx = line_len(cy); break;
        case KEY_PGUP:
            cy -= ED_ROWS;
            if (cy < 0) cy = 0;
            if (cx > line_len(cy)) cx = line_len(cy);
            break;
        case KEY_PGDN:
            cy += ED_ROWS;
            if (cy > B.n - 1) cy = B.n - 1;
            if (cx > line_len(cy)) cx = line_len(cy);
            break;
        case KEY_BACKSP:
        case 127:
            ed_backspace();
            break;
        case KEY_DEL:
            ed_delete();
            break;
        case KEY_ENTER:
        case '\n':
            ed_newline();
            break;
        case KEY_TAB:
            ed_tab();
            break;
        case KEY_F2:
        case KEY_CTRL_S:                     /* ^S */
        case KEY_CTRL_O:                     /* ^O */
            ed_save();
            break;
        case KEY_F3:
        case 0x17:                           /* ^W */
            ed_save_as();
            break;
        case KEY_F10:
        case KEY_CTRL_X:                     /* ^X */
        case KEY_CTRL_Q:                     /* ^Q */
            if (dirty) {
                if (!ed_confirm("Save modified buffer?")) break;
                if (ed_save() != 0) break;
            }
            done = 1;
            break;
        case 0x0B:                           /* ^K 剪切行 */
            ed_cut_line();
            break;
        case 0x15:                           /* ^U 粘贴 */
            ed_paste();
            break;
        case 0x07:                           /* ^G 帮助 */
            ed_help();
            break;
        case 0x0C:                           /* ^L 重画 */
            break;
        default:
            if (k >= 32 && k < 127) ed_insert_char((char)k);
            break;
        }
    }

    ed_free_all();
    vga_clear(0x00);
    vga_setcolor(0x0F, 0x00);
    vga_setcursor(0, 0);
    kprintf("[edit] editor closed\n");
    return 0;
}
