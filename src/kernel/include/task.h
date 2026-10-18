#ifndef TINYOS_TASK_H
#define TINYOS_TASK_H

#include "types.h"

/* ============================================================
 * TinyOS Genesis 多进程任务系统
 * ------------------------------------------------------------
 * 每个任务 = 一个独立的 TNCR 映像实例：
 *   · 映像装载到独立的槽地址（TASK_IMG_BASE 起，每槽 512KB），
 *     由 TNCR 内嵌的重定位表在装载时应用（见 tools/compiler/tcc.py
 *     的 --emit-relocs 支持），因此同一个程序可以起 N 个互不覆盖的实例；
 *   · 独立内核栈（kmalloc，TASK_STACK_SIZE）；
 *   · 独立 IPC 邮箱（定长消息队列）。
 *
 * 调度模型（协作式时间片 + 看门狗）：
 *   · 内核上下文（桌面/Shell）在安全点调用 sched_run() 把 CPU 交给任务；
 *   · 任务在自己的运算点调用 task_yield() 主动让出；
 *   · 定时器看门狗：任务超过 TASK_WATCHDOG_TICKS 不让出也不退出 →
 *     判定为卡死，标记 FAULTED(fault_vec=0xFE) 并切回内核上下文，
 *     绝不恢复该任务（其半个内核调用被丢弃，由主进程善后）；
 *   · CPU 异常（#PF/#GP 等）发生在任务内 → 同样标记 FAULTED 并切回，
 *     不再拖垮整个内核。
 *
 * 上下文切换借道中断帧：int $TASK_YIELD_VEC 与定时器 IRQ 都经过
 * idt.asm 尾部的 task_after_irq_tail()，由它保存/恢复 esp。
 * ============================================================ */
#define TASK_MAX         8
#define TASK_STACK_SIZE  16384u
#define TASK_IMG_BASE    0x02000000u
#define TASK_IMG_SLOT    (512u * 1024u)

/* 主动让出/切入用的软中断向量（0x50，避开异常 0..31 与 IRQ 32..47） */
#define TASK_YIELD_VEC   0x50

/* 看门狗：100Hz 下 10 秒 */
#define TASK_WATCHDOG_TICKS 1000u
/* 自动重启上限（respawn 打开时） */
#define TASK_MAX_RESTARTS  5

typedef enum {
    TASK_FREE = 0,
    TASK_READY,      /* 可运行 */
    TASK_RUNNING,    /* 正在运行（或被抢占中） */
    TASK_BLOCKED,    /* 阻塞在 IPC */
    TASK_DONE,       /* 正常/被 kill 退出 */
    TASK_FAULTED     /* 异常退出（CPU 异常或看门狗） */
} task_state;

/* ---- IPC 消息 ----
 * 定长信箱：每任务 IPC_QUEUE_DEPTH 条、每条最多 IPC_MSG_MAX 字节。 */
#define IPC_MSG_MAX     96
#define IPC_QUEUE_DEPTH 8

typedef struct task task_t;

/* ---- 任务管理 ---- */
void task_system_init(void);                      /* 内核启动时调用一次 */
int  task_spawn(const char *path);                /* 返回 pid（>0），0 = 失败 */
int  task_spawn_arg(const char *path, const char *arg);
int  task_self(void);                             /* 当前任务 pid；内核上下文返回 0 */
int  task_parent(void);                           /* 父 pid；0 = 无 */
void task_exit(int code);                         /* 结束当前任务（不返回） */
void task_yield(void);                            /* 让出 CPU */
int  task_kill(int pid);                          /* 0 = 已安排终止 */
int  task_state_of(int pid);                      /* task_state；-1 = 无此任务 */
int  task_exit_code(int pid);                     /* 退出码（负数=异常/信号） */
int  task_fault_vec(int pid);                     /* 0=非异常退出；0xFE=看门狗；其它=CPU 异常号 */
int  task_restarts(int pid);                      /* 已被自动重启次数 */
void task_set_respawn(int pid, int on);           /* 异常退出后自动重启 */
void task_reap(int pid);                          /* 回收 DONE/FAULTED 任务（释放栈与槽） */
int  task_count(void);                            /* 非 FREE 任务数 */
void task_list(void);                             /* 串口打印任务表 */

/* ---- 调度 ---- */
/* 把 CPU 交给就绪任务跑一会儿；没有就绪任务时立即返回。
 * 内核上下文（桌面主循环、Shell 循环）应在每次循环迭代里调用。 */
void sched_run(void);
/* 强制在下一次中断返回时切换上下文（异常逃逸用，由 idt.c 调用）*/
void task_force_switch(void);
/* 当前是否运行在任务里（0 = 内核上下文）；调度系统是否已启用 */
int  task_current_pid(void);
int  task_cur_parent(void);                 /* 当前任务的父 pid（内核上下文返回 0）*/
int  task_sched_enabled(void);
/* 把当前任务标记为"因 CPU 异常退出"（由 idt.c 的异常路径调用，
 * 之后应配 task_force_switch() 把上下文切回内核，故障任务永不再恢复）*/
void task_note_fault(int vec, u32 eip);
/* 切换钩子：由 idt.asm 在每个中断处理完后调用。
 * stub_esp = 中断帧顶（gs 槽）地址；返回 0 = 不切换，否则返回新 esp。 */
u32  task_after_irq(u32 stub_esp);

/* ---- IPC ---- */
int  ipc_send(int dst, const void *msg, int n);   /* 0 = 成功；-1 = 目标不存在/队列满 */
int  ipc_recv(void *msg, int max, int *from, int timeout_ms);
     /* >0 = 收到字节数；0 = 超时；-1 = 出错。timeout_ms < 0 = 一直等 */
int  ipc_pending(int pid);                        /* 待收消息条数 */

#endif
