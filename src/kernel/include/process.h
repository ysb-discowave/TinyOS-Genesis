#ifndef TINYOS_PROCESS_H
#define TINYOS_PROCESS_H

#include "types.h"

typedef enum { SESS_NONE = 0, SESS_TERMINAL, SESS_DESKTOP } sess_type;
typedef enum { PROC_FREE = 0, PROC_READY, PROC_RUNNING, PROC_DONE } proc_state;

typedef struct session {
    int  id;
    sess_type type;
    char title[48];
    int  uid;                  /* 建立该会话的用户 uid（多用户） */
    int  windows;              /* 该会话已打开的窗口数 */
    struct session *next;
} session_t;

typedef struct proc {
    int  pid;
    char name[32];
    int  session;              /* 所属会话 id，0 = 普通 Shell */
    int  uid;                  /* 发起该进程的用户 uid */
    proc_state state;
    u32  started;              /* 启动 tick */
    struct proc *next;
} proc_t;

int  proc_kill_pid(int pid);   /* 终止指定 PID；0 成功，-1 不存在 */
int  proc_count(void);          /* 当前进程总数（含已结束） */
void proc_init(void);

/* ---- 会话 ---- */
int  proc_new_session(sess_type t, const char *title);
int  proc_current_session(void);
sess_type proc_session_type(int id);
void proc_set_session(int id);
session_t *proc_session(int id);
int  proc_session_uid(int id);      /* 会话的 uid（不存在返回 -1） */
int  proc_current_uid(void);        /* 当前会话的 uid */

/* ---- 进程 ---- */
int  proc_spawn(const char *name, int session);
void proc_finish(int pid);
void proc_list(void);

/* ---- GUI 规则 ----
 * 打开图形窗口。强制：GUI 程序必须在“桌面双击打开的终端”会话内运行，
 * 否则内核抛出异常（打印 [TinyOS 异常] 并拒绝创建窗口），返回 -1。*/
int  gui_open(const char *title, const char *content);

#endif
