#include "process.h"
#include "console.h"
#include "vga.h"
#include "pit.h"
#include "mm.h"
#include "desktop.h"
#include "user.h"
#include "libc.h"

static session_t *g_sessions = NULL;
static proc_t    *g_procs    = NULL;
static int g_next_sid = 1;
static int g_next_pid = 1;
static int g_current = 0;   /* 当前运行程序所属会话 */

void proc_init(void) { g_sessions = NULL; g_procs = NULL; g_next_sid = 1; g_next_pid = 1; g_current = 0; }

/* ---------------- 会话 ---------------- */
int proc_new_session(sess_type t, const char *title) {
    session_t *s = (session_t*)kzalloc(sizeof(session_t));
    if (!s) return 0;
    s->id = g_next_sid++;
    s->type = t;
    s->windows = 0;
    s->uid = user_current() < 0 ? 0 : user_current();   /* 会话继承发起者的身份 */
    strncpy(s->title, title ? title : "", 47);
    s->next = g_sessions;
    g_sessions = s;
    return s->id;
}

int proc_session_uid(int id) {
    session_t *s = proc_session(id);
    return s ? s->uid : -1;
}

int proc_current_uid(void) { return proc_session_uid(g_current); }

session_t *proc_session(int id) {
    session_t *s = g_sessions;
    while (s) { if (s->id == id) return s; s = s->next; }
    return NULL;
}

int proc_current_session(void) { return g_current; }

sess_type proc_session_type(int id) {
    session_t *s = proc_session(id);
    return s ? s->type : SESS_NONE;
}

void proc_set_session(int id) { g_current = id; }

/* ---------------- 进程 ---------------- */
int proc_spawn(const char *name, int session) {
    proc_t *p = (proc_t*)kzalloc(sizeof(proc_t));
    if (!p) return 0;
    p->pid = g_next_pid++;
    strncpy(p->name, name ? name : "?", 31);
    p->session = session;
    p->uid = user_current() < 0 ? 0 : user_current();
    p->state = PROC_RUNNING;
    p->started = pit_ticks();
    p->next = g_procs;
    g_procs = p;
    return p->pid;
}

void proc_finish(int pid) {
    proc_t *p = g_procs;
    while (p) { if (p->pid == pid) { p->state = PROC_DONE; return; } p = p->next; }
}

void proc_list(void) {
    kprintf("PID  STATE    SESS  UID   NAME\n");
    proc_t *p = g_procs;
    if (!p) { kprintf("(no processes)\n"); return; }
    while (p) {
        const char *st = (p->state == PROC_RUNNING) ? "RUNNING" :
                         (p->state == PROC_READY)   ? "ready"   :
                         (p->state == PROC_DONE)    ? "done" : "free";
        kprintf("%-4d %-8s %-5d %-5d %s\n", p->pid, st, p->session, p->uid, p->name);
        p = p->next;
    }
}

/* Genesis 扩展：终止指定 PID（供 tinysh 的 kill）。
 * 内核基础进程（pid <= 1，含主 Shell 会话与 idle）不允许杀。 */
int proc_kill_pid(int pid) {
    if (pid <= 1) return -1;
    proc_t *p = g_procs;
    while (p) {
        if (p->pid == pid) {
            if (p->state == PROC_DONE) return 0;
            p->state = PROC_DONE;
            return 0;
        }
        p = p->next;
    }
    return -1;   /* PID 不存在 */
}

int proc_count(void) {
    int n = 0;
    proc_t *p = g_procs;
    while (p) { n++; p = p->next; }
    return n;
}

/* ---------------- GUI 规则 ---------------- */
int gui_open(const char *title, const char *content) {
    int sid = proc_current_session();

    /* 规则：GUI 程序必须在“桌面打开的终端”会话中运行 */
    if (sid == 0 || proc_session_type(sid) != SESS_TERMINAL) {
        vga_setcolor(0x0F, 0x04);
        kprintf("[TinyOS] GUI program must run inside a terminal opened from the desktop");
        if (sid == 0) kprintf(" (this is the plain shell, not a desktop terminal)");
        else          kprintf(" (session id=%d is not a terminal)", sid);
        kprintf("\n");
        vga_setcolor(0x0F, 0x00);
        return -1;   /* 抛出异常：不创建窗口 */
    }

    session_t *s = proc_session(sid);
    if (s) s->windows++;
    desktop_show_window(title, content);
    return 0;
}
