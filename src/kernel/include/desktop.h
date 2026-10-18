#ifndef TINYOS_DESKTOP_H
#define TINYOS_DESKTOP_H

#include "types.h"
#include "keys.h"

/* ============================================================
 * TinyOS Genesis 图形化桌面 / 窗口管理器
 * ------------------------------------------------------------
 * 80x25 VGA 文本模式 + CP437 字形。窗口管理器负责：
 *   · 多窗口叠放（z 序）、焦点、最小化
 *   · 鼠标拖动标题栏移动、右下角手柄缩放、双击标题栏最大化
 *   · 布局管理：cascade / tile / stack
 *   · 把键盘/鼠标事件路由给焦点窗口的客户端
 * 客户端（终端 / 文件管理器 / 关于）通过 client_vt 接入：
 * 只负责客户区内容，边框/标题栏/任务栏由 WM 绘制。
 * ============================================================ */

#define WM_MAX_WINDOWS 8

/* 桌面是否处于激活状态（GUI 程序据此决定弹窗方式） */
extern int g_desktop_active;

/* 进入图形化桌面（由 DESKTOP.TNCR 或 `desktop` 命令调用） */
void desktop_enter(void);

/* 模态弹窗（gui_open 用：保存/恢复整屏，任何键或点击关闭）。
 * 保留旧行为：GUI TNCR 程序（必须在桌面终端会话内）弹内容窗口。 */
void desktop_show_window(const char *title, const char *content);

/* ---------------- 窗口与客户端 ---------------- */
typedef enum { WC_TERMINAL = 0, WC_FILEMGR = 1, WC_ABOUT = 2 } wclient_type;

struct client_vt;   /* 前向声明（win_t 内引用） */

typedef struct win {
    int  id;
    int  x, y, w, h;          /* 含边框的绝对坐标与尺寸（字符格） */
    char title[40];
    int  minimized;           /* 1 = 最小化（只在任务栏显示按钮） */
    int  z;                   /* 叠放层次，越大越靠上 */
    int  client_w, client_h;  /* 客户区尺寸（不含边框/标题栏） */
    int  maximized;           /* 1 = 最大化（双击标题栏切换） */
    int  save_x, save_y, save_w, save_h;  /* 最大化前的几何 */
    int  user_w, user_h;      /* 用户亲自设定的尺寸：layout 改完，cascade 要还回来 */
    wclient_type type;
    const struct client_vt *vt;  /* 客户端接口（wm_open 时登记） */
    void *client;             /* 客户端私有状态 */
    struct win *next;
} win_t;

typedef struct client_vt {
    void (*draw)(win_t *w);              /* 重绘客户区（坐标相对客户区左上） */
    void (*key)(win_t *w, int c);        /* 键盘输入（统一键值，见 keys.h） */
    void (*click)(win_t *w, int cx, int cy); /* 客户区内鼠标点击 */
    void (*close)(win_t *w);             /* 关闭前清理（可 NULL） */
    int  (*busy)(win_t *w);              /* 返回非 0 = 忙碌（光标变 BUSY） */
} client_vt;

/* ---- WM 对外 API（客户端用）---- */
win_t *wm_open(const client_vt *vt, wclient_type type, const char *title,
               int w, int h);                 /* 打开并聚焦一个新窗口 */
void   wm_close(win_t *w);                    /* 关闭窗口（调用 client->close） */
win_t *wm_focused(void);                      /* 当前焦点窗口（无则 NULL）*/
void   wm_focus(win_t *w);                    /* 置焦并提到最上层 */
void   wm_minimize(win_t *w);
void   wm_invalidate(void);          /* 请求下一帧整屏重绘（客户端改状态后必调）*/
void   wm_layout(int mode);                   /* 0=cascade 1=tile 2=stack */
int    wm_layout_mode(void);

/* ---- 客户区绘制助手（坐标相对客户区左上，自动裁剪）---- */
void wput(win_t *w, int cx, int cy, char ch, u8 fg, u8 bg);
void wstr(win_t *w, int cx, int cy, const char *s, u8 fg, u8 bg);
void wstrn(win_t *w, int cx, int cy, int maxw, const char *s, u8 fg, u8 bg);
void wfill(win_t *w, int cx, int cy, int cw, int chh, u8 fg, u8 bg);

/* ---- 文件管理器（fm.c）---- */
void fm_open_window(void);     /* 打开一个文件管理器窗口 */
int  fm_busy(void);            /* 有文件操作在进行中（忙碌光标）*/

#endif
