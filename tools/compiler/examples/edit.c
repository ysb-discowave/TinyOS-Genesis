/* ============================================================
 * EDIT.TNCR -- TinyOS 全屏文本编辑器
 * Language: C (cross-compiled by tcc -> TNCR)
 * Build:    tcc edit.c -o EDIT.TNCR
 * Run:      edit /home/notes.txt        (shell passes the path as cmdarg)
 *
 * 说明
 * ----
 * 内核只给了 print/println 时是画不出可编辑窗口的，所以本程序使用
 * api->draw / gotoxy / getkey / clearscr 这组全屏原语：
 *   draw()   直接写 VGA 文本缓冲，不经过串口（否则每次重绘几千字符
 *            会把串口日志淹掉，自动化测试全靠读串口）
 *   getkey() 返回统一键值：可打印字符 = ASCII，方向键/Home/End/PgUp/
 *            PgDn/Del 在 0x100 以上，F1..F10 = 0x111..0x11A
 *
 * 缓冲区模型
 * ----------
 * 采用"扁平缓冲 + 行首索引"：tx[] 存整份文本（'\n' 分隔），lstart[]
 * 存每行起始偏移。每次编辑后 reindex() 重建索引。这样插入/删除就是
 * 一次 memmove，不需要维护链表，也不受"每行定长"的限制。
 * ============================================================ */
#include "api_user.h"

#define SCRW        80
#define SCRH        25
#define ROW_TITLE   0
#define ROW_TOP     1
#define EDIT_ROWS   22
#define ROW_STATUS  (ROW_TOP + EDIT_ROWS)   /* 23 */
#define ROW_MSG     24
#define GUTTER      5                        /* 行号栏宽度 */
#define TEXTW       (SCRW - GUTTER)          /* 文本可视宽度 75 */

#define MAXTEXT     30000
#define MAXLINES    1200

/* 配色（VGA 文本属性：0x0=黑 0x1=蓝 0x7=浅灰 0x8=深灰 0xF=白） */
#define C_TITLE_FG  0x0F
#define C_TITLE_BG  0x01
#define C_TEXT_FG   0x07
#define C_TEXT_BG   0x00
#define C_GUT_FG    0x08
#define C_GUT_BG    0x00
#define C_ST_FG     0x00
#define C_ST_BG     0x07
#define C_MSG_FG    0x0F
#define C_MSG_BG    0x00
#define C_BOX_FG    0x1E                     /* 帮助框：黄字蓝底 */

/* 统一键值（与 kernel/include/keys.h 保持一致，用户态拿不到内核头） */
#define K_ESC       0x1B
#define K_TAB       0x09
#define K_ENTER     0x0D
#define K_BACKSP    0x08
#define K_DEL127    0x7F
#define K_UP        0x101
#define K_DOWN      0x102
#define K_LEFT      0x103
#define K_RIGHT     0x104
#define K_HOME      0x105
#define K_END       0x106
#define K_PGUP      0x107
#define K_PGDN      0x108
#define K_DEL       0x109
#define K_F1        0x111
#define K_F2        0x112
#define K_F3        0x113
#define K_F10       0x11A

#define CTRL_A  0x01
#define CTRL_E  0x05
#define CTRL_K  0x0B
#define CTRL_O  0x0F
#define CTRL_Q  0x11
#define CTRL_S  0x13
#define CTRL_X  0x18

/* ---------------------------------------------------------------- 状态 */
static tinyos_api_t *A;

static char tx[MAXTEXT];
static int  lstart[MAXLINES + 2];
static int  nlines;
static int  tlen;

static char path[256];
static int  modified;
static int  curline, curcol;         /* 光标（行列） */
static int  wantcol;                 /* 上下移动时记住的期望列 */
static int  top, left;               /* 视口滚动 */
static int  help_on;
static char msg[100];

/* ------------------------------------------------------------ 字符串工具 */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void scpy(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; }

static int sput(char *d, const char *s) { int i = 0; while (s[i]) { d[i] = s[i]; i++; } return i; }

static int spad(char *d, int n, int w) { while (n < w) { d[n] = ' '; n++; } d[n] = 0; return n; }

static int snum(int v, char *d) {
    char t[16];
    int i = 0, n = 0;
    if (v < 0) { d[n++] = '-'; v = -v; }
    if (v == 0) t[i++] = '0';
    while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) d[n++] = t[--i];
    return n;
}

/* ------------------------------------------------------------ 缓冲区操作 */
static void reindex(void) {
    nlines = 1;
    lstart[0] = 0;
    for (int i = 0; i < tlen; i++) {
        if (tx[i] == '\n') {
            if (nlines >= MAXLINES) break;
            lstart[nlines++] = i + 1;
        }
    }
    lstart[nlines] = tlen;
}

/* 第 ln 行的内容长度（不含行尾 '\n'） */
static int lend(int ln) {
    int s = lstart[ln], e = lstart[ln + 1];
    if (e > s && tx[e - 1] == '\n') e--;
    return e - s;
}

static int curpos(void) { return lstart[curline] + curcol; }

static void clamp_cur(void) {
    if (curline < 0) curline = 0;
    if (curline >= nlines) curline = nlines - 1;
    if (curline < 0) curline = 0;
    int L = lend(curline);
    if (curcol < 0) curcol = 0;
    if (curcol > L) curcol = L;
}

static void insert_at(int pos, char c) {
    if (tlen + 1 >= MAXTEXT) return;
    for (int i = tlen; i > pos; i--) tx[i] = tx[i - 1];
    tx[pos] = c;
    tlen++;
    modified = 1;
}

static void delete_range(int pos, int n) {
    if (n <= 0 || pos < 0 || pos + n > tlen) return;
    for (int i = pos; i + n < tlen; i++) tx[i] = tx[i + n];
    tlen -= n;
    modified = 1;
}

/* ------------------------------------------------------------ 文件 I/O */
static int load_file(const char *p) {
    int n = A->file_read(p, tx, MAXTEXT);
    if (n < 0) { tlen = 0; tx[0] = 0; reindex(); return -1; }
    tlen = n;
    /* 去掉 CR，统一成 '\n' 行尾 */
    int w = 0;
    for (int i = 0; i < tlen; i++) if (tx[i] != '\r') tx[w++] = tx[i];
    tlen = w;
    /* 去掉文件末尾多余换行之外的控制字符，避免把 VGA 弄花 */
    for (int i = 0; i < tlen; i++)
        if (tx[i] && tx[i] != '\n' && (unsigned char)tx[i] < 32 && tx[i] != '\t')
            tx[i] = ' ';
    reindex();
    curline = 0; curcol = 0; top = 0; left = 0;
    modified = 0;
    return 0;
}

static int save_file(const char *p) {
    if (!p[0]) return -1;
    if (A->file_write(p, tx, tlen) != 0) return -1;
    modified = 0;
    return 0;
}

/* ------------------------------------------------------------ 界面绘制 */
static void row_text(int ln, char *out) {
    int s = lstart[ln], e = lstart[ln + 1];
    if (e > s && tx[e - 1] == '\n') e--;
    int i = 0, p = s + left;
    while (p < e && i < TEXTW) {
        char c = tx[p++];
        if (c == '\t') c = ' ';          /* 文本模式没有制表符字形 */
        out[i++] = c;
    }
    while (i < TEXTW) out[i++] = ' ';
    out[TEXTW] = 0;
}

static void draw_screen(void) {
    char buf[SCRW + 2];
    char gbuf[8];

    /* 标题栏 */
    int n = sput(buf, " EDIT  ");
    n += sput(buf + n, path[0] ? path : "(untitled)");
    n = spad(buf, n, SCRW);
    A->draw(0, ROW_TITLE, buf, C_TITLE_FG, C_TITLE_BG);

    /* 正文区 */
    for (int r = 0; r < EDIT_ROWS; r++) {
        int ln = top + r;
        int y = ROW_TOP + r;
        if (ln < nlines) {
            int k = 0;
            /* 行号右对齐到 4 位 + 一个空格 */
            char num[8];
            int m = snum(ln + 1, num);
            for (int i = 0; i < 4 - m; i++) gbuf[k++] = ' ';
            for (int i = 0; i < m; i++) gbuf[k++] = num[i];
            gbuf[k++] = (ln == curline) ? '>' : ' ';
            gbuf[k] = 0;
            A->draw(0, y, gbuf, C_GUT_FG, C_GUT_BG);
            row_text(ln, buf);
            A->draw(GUTTER, y, buf, C_TEXT_FG, C_TEXT_BG);
        } else {
            A->draw(0, y, "     ", C_GUT_FG, C_GUT_BG);
            for (int i = 0; i < TEXTW; i++) buf[i] = ' ';
            buf[TEXTW] = 0;
            A->draw(GUTTER, y, buf, C_TEXT_FG, C_TEXT_BG);
        }
    }

    /* 状态栏 */
    n = sput(buf, " Ln ");
    n += snum(curline + 1, buf + n);
    n += sput(buf + n, "/");
    n += snum(nlines, buf + n);
    n += sput(buf + n, "  Col ");
    n += snum(curcol + 1, buf + n);
    n += sput(buf + n, modified ? "   [modified]" : "   [saved]");
    n = spad(buf, n, SCRW - 14);
    n += sput(buf + n, "F1 help  ");
    n = spad(buf, n, SCRW);
    A->draw(0, ROW_STATUS, buf, C_ST_FG, C_ST_BG);

    /* 消息行 */
    n = sput(buf, " ");
    n += sput(buf + n, msg);
    n = spad(buf, n, SCRW);
    A->draw(0, ROW_MSG, buf, C_MSG_FG, C_MSG_BG);
}

static void draw_help(void) {
    static const char *L[] = {
        "  TinyOS EDIT -- key reference                                            ",
        "                                                                          ",
        "  Up/Down/Left/Right   move the cursor                                    ",
        "  Home / End           start / end of line                                ",
        "  PgUp / PgDn          scroll one screen                                  ",
        "  Enter                split the line at the cursor                       ",
        "  Backspace / Del      delete before / under the cursor                   ",
        "  Tab                  insert spaces up to the next tab stop (4)          ",
        "                                                                          ",
        "  ^S  or  F2           save the file                                      ",
        "  ^O  or  F3           open another file                                  ",
        "  ^A / ^E              start / end of line                                ",
        "  ^K                   delete to end of line                              ",
        "  ^X  or  F10          quit (asks to save if modified)                    ",
        "                                                                          ",
        "  press any key to close                                                  ",
    };
    int x = 8, y = 4, w = 66, h = 17;
    char blank[80];
    for (int i = 0; i < w; i++) blank[i] = ' ';
    blank[w] = 0;
    for (int r = 0; r < h; r++) {
        const char *s = (r < (int)(sizeof(L) / sizeof(L[0]))) ? L[r] : blank;
        A->draw(x, y + r, s, C_BOX_FG, C_TITLE_BG);
    }
}

static void set_msg(const char *s) { scpy(msg, s); }

/* ------------------------------------------------------------ 视口跟随 */
static void follow(void) {
    if (curline < top) top = curline;
    if (curline > top + EDIT_ROWS - 1) top = curline - EDIT_ROWS + 1;
    if (top < 0) top = 0;
    if (curcol < left) left = curcol;
    if (curcol > left + TEXTW - 1) left = curcol - TEXTW + 1;
    if (left < 0) left = 0;
}

/* ------------------------------------------------------------ 行内输入 */
/* 在消息行就地提问；返回 1 = 已确认（buf 有内容），0 = 取消 */
static int ask(const char *prompt, char *buf, int max) {
    int n = 0;
    buf[0] = 0;
    for (;;) {
        char line[SCRW + 2];
        int k = sput(line, " ");
        k += sput(line + k, prompt);
        k += sput(line + k, buf);
        k = spad(line, k, SCRW);
        A->draw(0, ROW_MSG, line, 0x0F, C_TITLE_BG);
        A->gotoxy(1 + (int)slen(prompt) + n, ROW_MSG);

        int c = A->getkey();
        if (c == K_ESC) return 0;
        if (c == K_ENTER || c == '\n') { buf[n] = 0; return 1; }
        if ((c == K_BACKSP || c == K_DEL127) && n > 0) { n--; buf[n] = 0; continue; }
        if (c >= 32 && c < 127 && n < max - 1) { buf[n++] = (char)c; buf[n] = 0; }
    }
}

/* ------------------------------------------------------------ 编辑动作 */
static void do_enter(void) {
    insert_at(curpos(), '\n');
    reindex();
    curline++;
    curcol = 0;
    wantcol = 0;
}

static void do_backspace(void) {
    if (curcol > 0) {
        delete_range(curpos() - 1, 1);
        reindex();
        curcol--;
        wantcol = curcol;
    } else if (curline > 0) {
        int pos = curpos();
        delete_range(pos - 1, 1);           /* 吃掉上一行的 '\n' */
        reindex();
        curline--;
        curcol = lend(curline);
        wantcol = curcol;
    }
}

static void do_delete(void) {
    int L = lend(curline);
    if (curcol < L) {
        delete_range(curpos(), 1);
        reindex();
    } else if (curline + 1 < nlines) {
        delete_range(curpos(), 1);          /* 合并下一行 */
        reindex();
    }
}

static void do_tab(void) {
    int nsp = 4 - (curcol % 4);
    for (int i = 0; i < nsp; i++) insert_at(curpos(), ' ');
    reindex();
    curcol += nsp;
    wantcol = curcol;
}

static void do_open(void) {
    char nb[256];
    if (!ask("open file: ", nb, sizeof(nb))) { set_msg("open cancelled"); return; }
    if (!nb[0]) { set_msg("open cancelled"); return; }
    if (load_file(nb) != 0) {
        scpy(path, nb);
        set_msg("new file (will be created on save)");
    } else {
        scpy(path, nb);
        char m[110];
        int k = sput(m, "loaded ");
        k += snum(tlen, m + k);
        k += sput(m + k, " bytes");
        m[k] = 0;
        set_msg(m);
    }
}

static void do_save(int ask_name) {
    char p[256];
    scpy(p, path);
    if (ask_name || !p[0]) {
        char nb[256];
        if (!ask("save as: ", nb, sizeof(nb))) { set_msg("save cancelled"); return; }
        if (!nb[0]) { set_msg("save cancelled"); return; }
        scpy(p, nb);
    }
    if (save_file(p) != 0) {
        set_msg("SAVE FAILED (path not writable?)");
        return;
    }
    scpy(path, p);
    char m[110];
    int k = sput(m, "saved ");
    k += snum(tlen, m + k);
    k += sput(m + k, " bytes -> ");
    k += sput(m + k, path);
    m[k] = 0;
    set_msg(m);
}

/* 修改过退出时的三选一：y=保存并退出 n=直接退出 c=取消 */
static int confirm_quit(void) {
    for (;;) {
        char line[SCRW + 2];
        int k = sput(line, " ");
        k += sput(line + k, "buffer modified: save before quit? (y/n/esc) ");
        k = spad(line, k, SCRW);
        A->draw(0, ROW_MSG, line, 0x0F, 0x04);
        int c = A->getkey();
        if (c == 'y' || c == 'Y') return 1;
        if (c == 'n' || c == 'N') return 2;
        if (c == K_ESC) return 0;
    }
}

/* ------------------------------------------------------------ 主循环 */
void user_main(tinyos_api_t *api) {
    A = api;

    char arg[256];
    arg[0] = 0;
    A->cmdarg(arg, sizeof(arg));

    path[0] = 0;
    modified = 0;
    curline = 0; curcol = 0; wantcol = 0;
    top = 0; left = 0; help_on = 0;
    msg[0] = 0;

    if (arg[0]) {
        scpy(path, arg);
        if (load_file(path) != 0)
            set_msg("new file (will be created on save)");
        else
            set_msg("loaded; ^O open  ^S save  ^X quit  F1 help");
    } else {
        tlen = 0; tx[0] = 0; reindex();
        set_msg("new buffer; ^O open  ^S save  ^X quit  F1 help");
    }

    A->clearscr();
    draw_screen();

    for (;;) {
        follow();
        clamp_cur();
        draw_screen();
        if (help_on) draw_help();
        A->gotoxy(GUTTER + (curcol - left), ROW_TOP + (curline - top));

        int c = A->getkey();

        if (help_on) { help_on = 0; continue; }

        if (c == K_F1) { help_on = 1; continue; }
        if (c == CTRL_S || c == K_F2) { do_save(0); continue; }
        if (c == CTRL_O || c == K_F3) { do_open(); continue; }
        if (c == CTRL_X || c == K_F10 || c == CTRL_Q) {
            if (modified) {
                int r = confirm_quit();
                if (r == 0) { set_msg("quit cancelled"); continue; }
                if (r == 1) { do_save(0); if (modified) continue; }
            }
            break;
        }

        switch (c) {
        case K_UP:
            if (curline > 0) { curline--; curcol = wantcol; clamp_cur(); }
            break;
        case K_DOWN:
            if (curline + 1 < nlines) { curline++; curcol = wantcol; clamp_cur(); }
            break;
        case K_LEFT:
            if (curcol > 0) curcol--;
            else if (curline > 0) { curline--; curcol = lend(curline); }
            wantcol = curcol;
            break;
        case K_RIGHT:
            if (curcol < lend(curline)) curcol++;
            else if (curline + 1 < nlines) { curline++; curcol = 0; }
            wantcol = curcol;
            break;
        case K_HOME:
        case CTRL_A:
            curcol = 0; wantcol = 0; left = 0;
            break;
        case K_END:
        case CTRL_E:
            curcol = lend(curline); wantcol = curcol;
            break;
        case K_PGUP:
            curline -= EDIT_ROWS; wantcol = curcol; clamp_cur();
            break;
        case K_PGDN:
            curline += EDIT_ROWS; wantcol = curcol; clamp_cur();
            break;
        case K_DEL:
            do_delete();
            break;
        case K_DEL127:
        case K_BACKSP:
            do_backspace();
            break;
        case K_ENTER:
        case '\n':
            do_enter();
            break;
        case K_TAB:
            do_tab();
            break;
        case CTRL_K: {
            int L = lend(curline);
            if (curcol < L) delete_range(curpos(), L - curcol);
            else if (curline + 1 < nlines) delete_range(curpos(), 1);
            reindex();
            break;
        }
        default:
            if (c >= 32 && c < 127) {
                insert_at(curpos(), (char)c);
                reindex();
                curcol++;
                wantcol = curcol;
                int k = 0;
                char m[40];
                k += sput(m, "editing");
                m[k] = 0;
                set_msg(m);
            }
            break;
        }
    }

    /* 退出前把控制台恢复成白字黑底并清屏，否则 shell 的提示符会继承
     * 编辑器留下的颜色/光标位置。 */
    A->setcolor(0x07, 0x00);
    A->clearscr();
}
