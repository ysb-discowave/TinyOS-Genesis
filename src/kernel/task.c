#include "task.h"
#include "vfs.h"
#include "mm.h"
#include "tncr.h"
#include "console.h"
#include "vga.h"
#include "libc.h"
#include "pit.h"
#include "api.h"

/* ============================================================
 * 任务系统实现（设计见 include/task.h 的头部注释）
 * ------------------------------------------------------------
 * 安全边界（本文件最重要的一条规则）：
 *   上下文切换只发生在三类点——
 *     a) 内核上下调用 sched_run()（安全点：内核此时不持任何中间状态）；
 *     b) 任务自己调 task_yield()/ipc_recv 阻塞/task_exit（安全点：
 *        此时任务不在内核调用中间态——内核调用不 yielding）；
 *     c) 定时器兜底：看门狗（10s 不让出 → 杀）与内核饥饿逃逸
 *        （内核 3s 拿不到 CPU → 强制切回内核）。b/c 两类切换绝不
 *        "任务A 内核调用中途 → 任务B"，因此 VFS/堆等共享内核状态
 *        永远不会被两个上下文并发修改。
 * ============================================================ */

extern tinyos_api_t g_api;

/* int $TASK_YIELD_VEC 的字符串化（双层宏：先展开再转字符串）*/
#define TASK_STR_(x) #x
#define TASK_STR(x) TASK_STR_(x)
static void task_switch_int(void) {
    __asm__ volatile("int $" TASK_STR(TASK_YIELD_VEC));
}

/* 任务入口包装（定义在后，先声明）*/
static void task_trampoline(void);

typedef struct mbox {
    struct { u8 data[IPC_MSG_MAX]; int len; int from; } q[IPC_QUEUE_DEPTH];
    int head, tail, count;
} mbox_t;

struct task {
    int  pid;
    int  parent;
    char name[32];
    char path[64];
    char arg[64];
    task_state state;
    int  exit_code;
    int  fault_vec;
    u32  fault_eip;
    int  restarts;
    int  respawn;
    int  kill_pending;
    u32  esp;                 /* 保存的上下文（中断帧顶 = gs 槽地址）*/
    u32  img_base;            /* 映像槽地址 */
    u32  img_size;
    u8  *stack;
    u32  entry;               /* user_main 绝对地址 */
    u32  last_yield_tick;     /* 最后一次主动让出（看门狗用）*/
    u32  recv_deadline;       /* IPC 阻塞超时滴答，0 = 无 */
    mbox_t mq;
};

static task_t g_pool[TASK_MAX];
static task_t *g_cur = NULL;          /* NULL = 内核上下文正在运行 */
static u32 g_kernel_esp = 0;          /* 内核上下文的保存帧 */
static int  g_sched_on = 0;
static u32 g_switch_req = 0;          /* sched_run 请求的切换 */
static u32 g_last_switch_tick = 0;
static u32 g_kernel_last_run = 0;     /* 内核上下文最后一次运行的 tick */
static int  g_next_pid = 1;
static u32 g_img_used[(TASK_MAX + 31) / 32];  /* 映像槽占用位图 */

/* 内核上下文信箱（pid 0）：主进程（文件管理器）与内核侧测试收消息用 */
static mbox_t g_kmq;

/* ---------------- 信箱 ---------------- */
static int mbox_push(mbox_t *m, const void *msg, int n, int from) {
    if (n <= 0 || n > IPC_MSG_MAX) return -1;
    if (m->count >= IPC_QUEUE_DEPTH) return -1;
    u8 *d = m->q[m->tail].data;
    const u8 *s = (const u8*)msg;
    for (int i = 0; i < n; i++) d[i] = s[i];
    m->q[m->tail].len = n;
    m->q[m->tail].from = from;
    m->tail = (m->tail + 1) % IPC_QUEUE_DEPTH;
    m->count++;
    return 0;
}
static int mbox_pop(mbox_t *m, void *msg, int max, int *from) {
    if (m->count <= 0) return 0;
    int n = m->q[m->head].len;
    if (n > max) n = max;
    u8 *d = (u8*)msg;
    for (int i = 0; i < n; i++) d[i] = m->q[m->head].data[i];
    if (from) *from = m->q[m->head].from;
    m->head = (m->head + 1) % IPC_QUEUE_DEPTH;
    m->count--;
    return n;
}
static mbox_t *mbox_of(int pid) {
    if (pid == 0) return &g_kmq;
    for (int i = 0; i < TASK_MAX; i++)
        if (g_pool[i].state != TASK_FREE && g_pool[i].pid == pid) return &g_pool[i].mq;
    return NULL;
}
static task_t *task_by_pid(int pid) {
    if (pid <= 0) return NULL;
    for (int i = 0; i < TASK_MAX; i++)
        if (g_pool[i].state != TASK_FREE && g_pool[i].pid == pid) return &g_pool[i];
    return NULL;
}

/* ---------------- 映像槽 ---------------- */
static int img_slot_alloc(void) {
    for (int i = 0; i < TASK_MAX; i++)
        if (!(g_img_used[i / 32] & (1u << (i % 32)))) {
            g_img_used[i / 32] |= 1u << (i % 32);
            return i;
        }
    return -1;
}
static void img_slot_free(int idx) {
    if (idx >= 0 && idx < TASK_MAX) g_img_used[idx / 32] &= ~(1u << (idx % 32));
}

/* ---------------- 初始中断帧 ----------------
 * 伪造一个"被 int $TASK_YIELD_VEC 打断"的栈帧，idt.asm 尾部的
 * pop gs..popa; add esp,8; iret 会把它还原成一次普通的中断返回，
 * 从而干净地进入 task_trampoline。布局（低→高）：
 *   gs fs es ds | eax ecx edx ebx esp ebp esi edi | int_no err |
 *   eip cs eflags useresp ss                                     */
static void build_initial_frame(task_t *t) {
    u32 top = ((u32)t->stack + TASK_STACK_SIZE) & ~15u;
    u32 *f = (u32*)top;
    *--f = 0x10;                          /* ss */
    *--f = top;                           /* useresp（环0用不到，给个合法值）*/
    *--f = 0x00000202u;                   /* eflags：IF=1 */
    *--f = 0x08;                          /* cs */
    *--f = (u32)task_trampoline;          /* eip */
    *--f = 0;                             /* err */
    *--f = TASK_YIELD_VEC;                /* int_no（被 add esp,8 跳过）*/
    *--f = 0;                             /* edi */
    *--f = 0;                             /* esi */
    *--f = 0;                             /* ebp */
    *--f = 0;                             /* esp（popa 丢弃）*/
    *--f = 0;                             /* ebx */
    *--f = 0;                             /* edx */
    *--f = 0;                             /* ecx */
    *--f = 0;                             /* eax */
    *--f = 0x10;                          /* ds */
    *--f = 0x10;                          /* es */
    *--f = 0x10;                          /* fs */
    *--f = 0x10;                          /* gs */
    t->esp = (u32)f;
}

/* 任务入口包装：捕获 user_main 的返回（TNCR 程序返回 = 退出）*/
static void task_trampoline(void) {
    task_t *t = g_cur;
    if (t && t->entry) {
        ((void(*)(tinyos_api_t*))t->entry)(&g_api);
    }
    task_exit(0);
    for (;;) __asm__ volatile("hlt");
}

/* ---------------- 装载 ---------------- */
static int task_load(task_t *t, const char *path) {
    u32 size = 0;
    const u8 *data = vfs_read_file(path, &size);
    if (!data) {
        kprintf("[task] %s: not found\n", path);
        return -1;
    }
    if (size < TNCR_HDR_SIZE || memcmp(data, TNCR_MAGIC, 4) != 0) {
        kprintf("[task] %s: not a TNCR image\n", path);
        return -1;
    }
    u32 flags     = *(const u32*)(data + 4);
    u32 load_addr = *(const u32*)(data + 8);
    u32 entry_off = *(const u32*)(data + 12);
    u32 code_size = *(const u32*)(data + 16);
    u32 bss_size  = *(const u32*)(data + 20);
    if (load_addr == 0) load_addr = TNCR_LOAD_ADDR;

    /* 多实例装载必须带重定位表，否则映像里的绝对地址全指向原基址 */
    if (!(flags & TNCR_F_RELOC)) {
        kprintf("[task] %s: no relocation table (rebuild with --reloc)\n", path);
        return -1;
    }
    if (code_size == 0 || code_size > TASK_IMG_SLOT - 4096) {
        kprintf("[task] %s: image too big (%u bytes)\n", path, code_size);
        return -1;
    }
    if (TNCR_HDR_SIZE + code_size + 4 > size) {
        kprintf("[task] %s: truncated image\n", path);
        return -1;
    }

    int slot = img_slot_alloc();
    if (slot < 0) {
        kprintf("[task] no free image slot\n");
        return -1;
    }
    u32 base = TASK_IMG_BASE + (u32)slot * TASK_IMG_SLOT;
    u8 *mem = (u8*)base;
    memcpy(mem, data + TNCR_HDR_SIZE, code_size);
    if (bss_size) memset(mem + code_size, 0, bss_size);

    /* 应用重定位表：payload 之后是 [u32 count][count x u32 offset] */
    const u8 *rt = data + TNCR_HDR_SIZE + code_size;
    u32 rc = *(const u32*)rt;
    if (TNCR_HDR_SIZE + code_size + 4 + rc * 4 > size) {
        kprintf("[task] %s: bad relocation table\n", path);
        img_slot_free(slot);
        return -1;
    }
    u32 delta = base - load_addr;
    for (u32 i = 0; i < rc; i++) {
        u32 off = *(const u32*)(rt + 4 + i * 4);
        if (off + 4 <= code_size + bss_size) {
            u32 v = *(u32*)(mem + off);
            *(u32*)(mem + off) = v + delta;
        }
    }

    t->img_base = base;
    t->img_size = code_size + bss_size;
    t->entry    = base + entry_off;
    return 0;
}

/* ----------------  spawn / 退出 ---------------- */
static int task_spawn_common(const char *path, const char *arg) {
    if (!path || !path[0]) return 0;
    task_t *t = NULL;
    for (int i = 0; i < TASK_MAX; i++)
        if (g_pool[i].state == TASK_FREE) { t = &g_pool[i]; break; }
    if (!t) {
        kprintf("[task] process table full\n");
        return 0;
    }
    memset(t, 0, sizeof(*t));
    strncpy(t->path, path, sizeof(t->path) - 1);
    {
        const char *sl = path;
        for (const char *p = path; *p; p++) if (*p == '/') sl = p + 1;
        strncpy(t->name, sl, sizeof(t->name) - 1);
    }
    if (arg) strncpy(t->arg, arg, sizeof(t->arg) - 1);

    if (task_load(t, path) != 0) return 0;

    t->stack = (u8*)kmalloc(TASK_STACK_SIZE);
    if (!t->stack) {
        kprintf("[task] out of memory for task stack\n");
        img_slot_free((int)((t->img_base - TASK_IMG_BASE) / TASK_IMG_SLOT));
        return 0;
    }
    t->pid = g_next_pid++;
    t->parent = g_cur ? g_cur->pid : 0;
    t->state = TASK_READY;
    t->last_yield_tick = pit_ticks();
    build_initial_frame(t);
    kprintf("[task] spawned pid %d (%s) at 0x%08X\n", t->pid, t->name, t->img_base);
    return t->pid;
}

int task_spawn(const char *path) { return task_spawn_common(path, NULL); }
int task_spawn_arg(const char *path, const char *arg) {
    return task_spawn_common(path, arg);
}

void task_exit(int code) {
    task_t *t = g_cur;
    if (!t) return;                        /* 内核上下文调用：无操作 */
    t->exit_code = code;
    t->state = TASK_DONE;
    t->kill_pending = 1;                   /* 永不再调度 */
    kprintf("[task] pid %d (%s) exited with %d\n", t->pid, t->name, code);
    task_switch_int();
    for (;;) __asm__ volatile("hlt");      /* 不会到达 */
}

void task_yield(void) {
    if (!g_cur) { sched_run(); return; }   /* 内核上下文：交给任务们 */
    if (g_cur->state == TASK_RUNNING) g_cur->state = TASK_READY;
    g_cur->last_yield_tick = pit_ticks();
    /* task_after_irq 只对 BLOCKED/DONE/FAULTED 或 g_switch_req 切换；
     * 不置请求位的话，READY 任务的"主动让出"会变成空操作——自旋回去
     * 继续跑，大文件拷贝会把桌面冻死直到看门狗杀任务。 */
    g_switch_req = 1;
    task_switch_int();
}

int task_kill(int pid) {
    task_t *t = task_by_pid(pid);
    if (!t) return -1;
    t->kill_pending = 1;
    t->respawn = 0;                        /* 主动 kill 不自动重启 */
    if (t->state == TASK_BLOCKED) t->state = TASK_DONE;  /* 唤醒路径由尾部收尾 */
    if (t->state != TASK_DONE && t->state != TASK_FAULTED) t->exit_code = -9;
    return 0;
}

int task_state_of(int pid)  { task_t *t = task_by_pid(pid); return t ? (int)t->state : -1; }
int task_exit_code(int pid) { task_t *t = task_by_pid(pid); return t ? t->exit_code : 0; }
int task_fault_vec(int pid) { task_t *t = task_by_pid(pid); return t ? t->fault_vec : 0; }
int task_restarts(int pid)  { task_t *t = task_by_pid(pid); return t ? t->restarts : 0; }

void task_set_respawn(int pid, int on) {
    task_t *t = task_by_pid(pid);
    if (t) t->respawn = on ? 1 : 0;
}

void task_reap(int pid) {
    task_t *t = task_by_pid(pid);
    if (!t) return;
    if (t->state != TASK_DONE && t->state != TASK_FAULTED) return;
    if (t->stack) { kfree(t->stack); t->stack = NULL; }
    img_slot_free((int)((t->img_base - TASK_IMG_BASE) / TASK_IMG_SLOT));
    t->img_base = 0;
    t->state = TASK_FREE;
    t->kill_pending = 0;
}

int task_count(void) {
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (g_pool[i].state != TASK_FREE) n++;
    return n;
}

void task_list(void) {
    static const char *names[] = { "free", "ready", "running", "blocked", "done", "FAULTED" };
    kprintf("PID  STATE    RESTART  EXIT  IMAGE     NAME\n");
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = &g_pool[i];
        if (t->state == TASK_FREE) continue;
        kprintf("%-4d %-8s %-8d %-5d 0x%08X %s\n",
                t->pid, names[t->state], t->restarts, t->exit_code, t->img_base, t->name);
    }
}

/* ---------------- 调度 ---------------- */
static task_t *pick_next(task_t *cur) {
    int start = 0;
    if (cur) {
        for (int i = 0; i < TASK_MAX; i++)
            if (&g_pool[i] == cur) { start = i + 1; break; }
    }
    for (int k = 0; k < TASK_MAX; k++) {
        task_t *t = &g_pool[(start + k) % TASK_MAX];
        if (t->state == TASK_READY) return t;
    }
    return NULL;
}

static int any_ready(void) {
    for (int i = 0; i < TASK_MAX; i++)
        if (g_pool[i].state == TASK_READY) return 1;
    return 0;
}

/* 异常退出（或被 kill）且开了 respawn 的任务：原地重启（同 pid） */
static void maybe_respawn(task_t *t) {
    if (!t) return;
    if (t->state != TASK_DONE && t->state != TASK_FAULTED) return;
    if (!t->respawn) return;
    if (t->restarts >= TASK_MAX_RESTARTS) {
        kprintf("[task] pid %d (%s): giving up after %d restarts\n",
                t->pid, t->name, t->restarts);
        t->respawn = 0;
        return;
    }
    kprintf("[task] pid %d (%s) %s -- restarting (%d/%d)\n",
            t->pid, t->name,
            t->state == TASK_FAULTED ? "faulted" : "exited",
            t->restarts + 1, TASK_MAX_RESTARTS);
    /* 先释放旧映像槽，避免每次重启漏一个槽 */
    if (t->img_base)
        img_slot_free((int)((t->img_base - TASK_IMG_BASE) / TASK_IMG_SLOT));
    t->img_base = 0;
    if (task_load(t, t->path) != 0) {
        t->state = TASK_DONE;
        t->respawn = 0;
        return;
    }
    t->restarts++;
    t->state = TASK_READY;
    t->kill_pending = 0;
    t->exit_code = 0;
    t->fault_vec = 0;
    t->last_yield_tick = pit_ticks();
    build_initial_frame(t);
}

/* 唤醒超时阻塞的任务（只改状态，不切换）*/
static void wake_timeouts(u32 now) {
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = &g_pool[i];
        if (t->state == TASK_BLOCKED && t->recv_deadline &&
            now >= t->recv_deadline) {
            t->state = TASK_READY;
            t->recv_deadline = 0;
        }
    }
}

u32 task_after_irq(u32 stub_esp) {
    if (!g_sched_on) return 0;
    task_t *cur = g_cur;
    u32 now = pit_ticks();

    wake_timeouts(now);

    int leave = g_switch_req;
    g_switch_req = 0;

    if (cur) {
        if (cur->kill_pending) {
            if (cur->state == TASK_RUNNING || cur->state == TASK_READY) {
                cur->state = TASK_DONE;
                if (cur->exit_code == 0) cur->exit_code = -9;
            }
            leave = 1;
        } else if (cur->state != TASK_RUNNING && cur->state != TASK_READY) {
            leave = 1;                          /* BLOCKED/DONE/FAULTED */
        } else if (cur->state == TASK_RUNNING) {
            /* 看门狗：长时间不让出 → 判定卡死 */
            if (now - cur->last_yield_tick > TASK_WATCHDOG_TICKS) {
                cur->state = TASK_FAULTED;
                cur->fault_vec = 0xFE;
                cur->exit_code = -0x2FE;
                cur->kill_pending = 1;
                /* 调试：帧顶 +56 = 任务被中断时的 EIP（4 段 + 8 pusha
                 * + 伪错误码 + 中断号），直接指出卡死在哪条指令。 */
                kprintf("[task] pid %d (%s) watchdog timeout -- killed (stuck eip=%X state=%d last_yield=%u now=%u)\n",
                        cur->pid, cur->name, *(u32*)(stub_esp + 56),
                        (int)cur->state, cur->last_yield_tick, now);
                leave = 1;
            }
        }
        /* 内核饥饿逃逸：内核上下文太久没拿到 CPU（任务空转不让出
         * 也不退出时），强制把 CPU 还给内核，保证桌面不冻死。 */
        if (!leave && now - g_kernel_last_run > 300) leave = 1;
    } else {
        g_kernel_last_run = now;
    }

    if (!leave) return 0;

    /* 保存当前上下文 */
    if (cur) {
        cur->esp = stub_esp;
        if (cur->state == TASK_RUNNING) cur->state = TASK_READY;
    } else {
        g_kernel_esp = stub_esp;
    }

    maybe_respawn(cur);

    task_t *next = pick_next(cur);
    if (!next) {
        g_cur = NULL;
        g_kernel_last_run = now;
        g_last_switch_tick = now;
        return g_kernel_esp;                    /* 回内核上下文 */
    }
    g_cur = next;
    next->state = TASK_RUNNING;
    /* 恢复/唤醒 = 新的看门狗预算：任务在 TASK_BLOCKED 里等消息的时间
     * 不能算"卡死不让出"。否则一个空闲 10 秒以上的 worker 一被消息唤醒，
     * 第一个 PIT 就会看到 now - last_yield_tick > 1000 而误杀（last_yield
     * 还停留在它上次阻塞的时刻）。真正自旋不吐的任务仍然会在连续跑满
     * TASK_WATCHDOG_TICKS 后被杀。 */
    next->last_yield_tick = now;
    g_last_switch_tick = now;
    return next->esp;
}

void sched_run(void) {
    if (!g_sched_on) return;
    if (!any_ready()) return;
    g_switch_req = 1;
    task_switch_int();
}

void task_force_switch(void) {
    if (g_sched_on) g_switch_req = 1;
}

int task_current_pid(void) { return g_cur ? g_cur->pid : 0; }
int task_cur_parent(void)  { return g_cur ? g_cur->parent : 0; }
int task_sched_enabled(void) { return g_sched_on; }

void task_note_fault(int vec, u32 eip) {
    task_t *t = g_cur;
    if (!t) return;
    t->fault_vec = vec;
    t->fault_eip = eip;
    t->exit_code = -(0x200 + vec);
    t->state = TASK_FAULTED;
    t->kill_pending = 1;                   /* 永不再恢复（帧已不可信）*/
}

void task_system_init(void) {
    memset(g_pool, 0, sizeof(g_pool));
    memset(&g_kmq, 0, sizeof(g_kmq));
    memset(g_img_used, 0, sizeof(g_img_used));
    g_cur = NULL;
    g_kernel_esp = 0;
    g_switch_req = 0;
    g_next_pid = 1;
    g_kernel_last_run = pit_ticks();
    g_sched_on = 1;
    kprintf("[task] process system ready (%d slots, images at 0x%08X)\n",
            TASK_MAX, TASK_IMG_BASE);
}

/* ---------------- IPC ---------------- */
int ipc_send(int dst, const void *msg, int n) {
    mbox_t *m = mbox_of(dst);
    if (!m) return -1;
    int from = g_cur ? g_cur->pid : 0;
    if (mbox_push(m, msg, n, from) != 0) return -1;
    /* 唤醒阻塞等消息的任务 */
    if (dst > 0) {
        task_t *t = task_by_pid(dst);
        if (t && t->state == TASK_BLOCKED) {
            t->state = TASK_READY;
            t->recv_deadline = 0;
        }
    }
    return 0;
}

int ipc_recv(void *msg, int max, int *from, int timeout_ms) {
    task_t *t = g_cur;
    mbox_t *m = t ? &t->mq : &g_kmq;
    for (;;) {
        int n = mbox_pop(m, msg, max, from);
        if (n > 0) return n;
        if (timeout_ms == 0) return 0;          /* 非阻塞 */
        if (!t) {
            /* 内核上下文不能真正阻塞：让任务跑，直到收到消息/超时 */
            u32 deadline = timeout_ms < 0 ? 0xFFFFFFFFu
                                          : pit_ticks() + (u32)timeout_ms / 10 + 1;
            for (;;) {
                sched_run();
                n = mbox_pop(&g_kmq, msg, max, from);
                if (n > 0) return n;
                if (pit_ticks() >= deadline) return 0;
            }
        }
        t->recv_deadline = timeout_ms < 0 ? 0
                                          : pit_ticks() + (u32)timeout_ms / 10 + 1;
        t->state = TASK_BLOCKED;
        task_yield();
        /* 被消息/超时唤醒：循环顶部重新取消息 */
    }
}

int ipc_pending(int pid) {
    mbox_t *m = mbox_of(pid);
    return m ? m->count : 0;
}
