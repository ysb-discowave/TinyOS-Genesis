/* ============================================================
 * TinyOS Genesis 文件管理器（Files 桌面客户端）
 * ------------------------------------------------------------
 * 窗口客户区 = 62 x 15（64x18 窗口，含边框/标题栏/底栏）。
 * 文件操作（copy/move/delete/mkdir/touch）一律交给独立的
 * worker 进程（/bin/FW.TNCR，TNCR 任务）去执行，本模块只负责：
 *   · 派生并监管 4 个 worker（IPC + 状态聚合 + 崩溃/重启处理）
 *   · 目录浏览（VFS 直连，主端）
 *   · 列表/图标两种视图、键盘/鼠标交互
 * 协议字节结构见下方 FW_* 常量与结构体（与 worker 端约定一致）。
 * 全程不使用 malloc，所有状态用 static 保存。
 * ============================================================ */

#include "desktop.h"
#include "task.h"
#include "vfs.h"
#include "keys.h"
#include "libc.h"

/* ---- worker 协议（字节精确，与 /bin/FW.TNCR 约定）---- */
#define FW_MAGIC_JOB  0x464A4F42u   /* 'FJOB' master -> worker */
#define FW_MAGIC_BYE  0x46425945u   /* 'FBYE' master -> worker: 退出 */
#define FW_MAGIC_PROG 0x46505247u   /* 'FPRG' worker -> master */
#define FW_OP_COPY   1
#define FW_OP_MOVE   2
#define FW_OP_DELETE 3
#define FW_OP_MKDIR  4
#define FW_OP_TOUCH  5
#define FW_OP_FAULT  7   /* TEST ONLY: worker 故意读 *0 触发 #PF */

typedef struct {
    u32  magic;
    u32  job_id;
    u8   op;
    u8   pad[3];
    char src[36];
    char dst[36];
} fw_cmd;                          /* 与 worker 端同为 sizeof == 84 */

typedef struct {
    u32  magic;
    u32  job_id;
    u32  done;
    u32  total;
    u32  fin;
    int  err;
    char msg[40];
} fw_prog;                         /* sizeof == 64 */

/* ---- 本地常量 ---- */
#define FM_MAXENT  64
#define FM_MAXJOBS 8
#define FW_N       4
#define ICON_COLS  7
#define ICON_W     8
#define ICON_H     3
#define ICON_Y0    2

/* ---- 目录条目（仅名字+类型，大小随后经 vfs_resolve 补）---- */
typedef struct {
    char name[40];
    int  is_dir;
    long size;
} fm_ent;

typedef struct {
    fm_ent e[FM_MAXENT];
    int    n;
} fm_list_st;

/* ---- 任务（job）状态 ---- */
enum { JOB_FREE=0, JOB_QUEUED, JOB_RUN, JOB_DONE, JOB_FAIL };

typedef struct {
    int   in_use;
    u32   job_id;
    u8    op;
    char  src[36];
    char  dst[36];
    int   state;
    u32   done;
    u32   total;
    int   err;
    char  msg[40];
    int   worker;          /* 分配的 worker 下标，-1 表示还在排队 */
} fm_job;

/* ---- worker 状态 ---- */
enum { WS_IDLE=0, WS_BUSY };

/* ===================== 静态状态 ===================== */
static int         fm_opened = 0;
static win_t      *fm_win    = NULL;

static char        cwd[128] = "/";

static fm_list_st  st;
static int         filt[FM_MAXENT];
static int         filtn = 0;

static int         sel    = 0;     /* 指向 filt[] 的可见下标 */
static int         scroll = 0;     /* 列表视图滚动偏移 */
static int         icon_view = 0;

static int         search_active = 0;

/* 提示/确认模式 */
enum {
    PM_NONE=0, PM_COPY, PM_MOVE, PM_DEL, PM_RENAME,
    PM_NEWFILE, PM_NEWDIR, PM_SEARCH, PM_PATH
};
static int  pmode = PM_NONE;
static char pbuf[52];
static int  plen  = 0;

static char fm_status[64];

/* worker 池 */
static int fw_pid[FW_N];
static int fw_state[FW_N];
static int fw_job[FW_N];      /* 该 worker 正在跑的 job 下标，-1 空闲 */
static int fw_restarts[FW_N]; /* 上次轮询记录的重启次数 */

/* 任务数组与计数 */
static fm_job jobs[FM_MAXJOBS];
static u32    fm_jobid = 0;
static int    fm_done  = 0;
static int    fm_fail  = 0;

/* ===================== 小工具 ===================== */
/* 拼接绝对路径：dir + "/" + name（dir=="/" 时不产生双斜杠）*/
static void fm_join(char *out, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
        snprintf(out, 128, "/%s", name);
    else
        snprintf(out, 128, "%s/%s", dir, name);
}

/* 取路径的父目录写入 out */
static void fm_dir_of(const char *path, char *out, int outsz)
{
    strncpy(out, path, outsz - 1);
    out[outsz - 1] = 0;
    char *sl = strrchr(out, '/');
    if (sl) {
        if (sl == out) out[1] = 0;   /* 根目录 */
        else *sl = 0;
    } else {
        out[0] = 0;
    }
}

/* 大小写敏感子串匹配（libc 未提供 strstr）*/
static int fm_contains(const char *hay, const char *needle)
{
    if (!*needle) return 1;
    int hn = (int)strlen(hay), nn = (int)strlen(needle);
    for (int i = 0; i + nn <= hn; i++) {
        int ok = 1;
        for (int j = 0; j < nn; j++)
            if (hay[i + j] != needle[j]) { ok = 0; break; }
        if (ok) return 1;
    }
    return 0;
}

static void fm_set_status(const char *s)
{
    strncpy(fm_status, s, sizeof fm_status - 1);
    fm_status[sizeof fm_status - 1] = 0;
}

/* ===================== 目录加载 ===================== */
/* 前向声明（被下方较早调用的函数）*/
static void fm_build_filt(void);
static void fm_info(void);

static void fm_list_cb(const char *name, vfs_type t, void *arg)
{
    fm_list_st *s = (fm_list_st *)arg;
    if (s->n >= FM_MAXENT) return;
    strncpy(s->e[s->n].name, name, 39);
    s->e[s->n].name[39] = 0;
    s->e[s->n].is_dir = (t == VFS_DIR);
    s->e[s->n].size   = 0;
    s->n++;
}

/* 插入排序：目录在前、文件在后；同类按名字升序。
 * [start, n) 已就绪，把新元素插入；".." 始终放在下标 0，不参与排序。*/
static void fm_sort_range(int start)
{
    for (int i = start + 1; i < st.n; i++) {
        fm_ent tmp = st.e[i];
        int j = i - 1;
        while (j >= start) {
            int less;
            if (tmp.is_dir && !st.e[j].is_dir) less = 1;       /* 目录优先 */
            else if (!tmp.is_dir && st.e[j].is_dir) less = 0;
            else less = (strcmp(tmp.name, st.e[j].name) < 0);
            if (!less) break;
            st.e[j + 1] = st.e[j];
            j--;
        }
        st.e[j + 1] = tmp;
    }
}

/* 重新读取当前目录并排序；cwd != "/" 时在开头插入合成的 ".." */
static void fm_reload(void)
{
    st.n = 0;
    vfs_list(cwd, fm_list_cb, &st);

    for (int i = 0; i < st.n; i++) {
        char path[128];
        fm_join(path, cwd, st.e[i].name);
        vfs_node_t *nd = vfs_resolve(path);
        if (nd) {
            st.e[i].is_dir = (nd->type == VFS_DIR);
            st.e[i].size   = (long)nd->size;
        }
    }

    /* 插入 ".." */
    if (strcmp(cwd, "/") != 0 && st.n < FM_MAXENT) {
        for (int i = st.n; i > 0; i--) st.e[i] = st.e[i - 1];
        strncpy(st.e[0].name, "..", 39);
        st.e[0].is_dir = 1;
        st.e[0].size   = 0;
        st.n++;
    }

    fm_sort_range((strcmp(cwd, "/") != 0) ? 1 : 0);
    fm_build_filt();
}

/* 根据搜索过滤构建可见下标表 filt[] */
static void fm_build_filt(void)
{
    filtn = 0;
    for (int i = 0; i < st.n; i++) {
        if (search_active && pbuf[0]) {
            if (strcmp(st.e[i].name, "..") == 0) {   /* ".." 始终保留 */
                filt[filtn++] = i;
                continue;
            }
            if (!fm_contains(st.e[i].name, pbuf)) continue;
        }
        filt[filtn++] = i;
    }
}

static fm_ent *fm_visible(int i)
{
    if (i < 0 || i >= filtn) return NULL;
    return &st.e[filt[i]];
}

/* ===================== 任务（job）管理 ===================== */
static const char *fm_opname(u8 op)
{
    switch (op) {
        case FW_OP_COPY:   return "copy";
        case FW_OP_MOVE:   return "move";
        case FW_OP_DELETE: return "delete";
        case FW_OP_MKDIR:  return "mkdir";
        case FW_OP_TOUCH:  return "touch";
        default:           return "job";
    }
}

static int fm_run_count(void)
{
    int r = 0;
    for (int i = 0; i < FM_MAXJOBS; i++)
        if (jobs[i].in_use && jobs[i].state == JOB_RUN) r++;
    return r;
}

/* 新建一个排队任务；队列满返回 -1 */
static int fm_new_job(u8 op, const char *src, const char *dst)
{
    for (int i = 0; i < FM_MAXJOBS; i++) {
        if (jobs[i].in_use) continue;
        memset(&jobs[i], 0, sizeof jobs[i]);
        jobs[i].in_use = 1;
        jobs[i].state  = JOB_QUEUED;
        jobs[i].op     = op;
        jobs[i].worker = -1;
        jobs[i].job_id = ++fm_jobid;
        strncpy(jobs[i].src, src, 35);
        jobs[i].src[35] = 0;
        strncpy(jobs[i].dst, dst, 35);
        jobs[i].dst[35] = 0;
        return i;
    }
    return -1;
}

/* 测试专用：让一个 worker 故意崩溃（读 *0），
 * 用来验证「异常检测 -> 标记 FAULTED -> 自动重启 -> 恢复服务」整条链路。
 * 崩溃的 worker 在途任务会被判失败，新实例经 task_set_respawn 顶上空缺。 */
static void fw_crash_test(void)
{
    int wid = -1;
    for (int k = 0; k < FW_N; k++)
        if (fw_pid[k] > 0 && fw_state[k] == WS_IDLE) { wid = k; break; }
    if (wid < 0) { fm_set_status("crash test: no idle worker"); return; }

    int j = fm_new_job(FW_OP_FAULT, "", "");
    if (j < 0) { fm_set_status("crash test: job queue full"); return; }
    fm_set_status("crash test: worker faulting...");
}

/* 把排队的任务分派给空闲 worker（轮转）*/
static void fm_dispatch(void)
{
    static int rr = 0;
    for (int j = 0; j < FM_MAXJOBS; j++) {
        if (!jobs[j].in_use || jobs[j].state != JOB_QUEUED) continue;

        int wid = -1;
        for (int k = 0; k < FW_N; k++) {
            int i = (rr + k) % FW_N;
            if (fw_pid[i] > 0 && fw_state[i] == WS_IDLE) { wid = i; break; }
        }
        if (wid < 0) break;        /* 没有空闲 worker，稍后再试 */

        fw_cmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.magic  = FW_MAGIC_JOB;
        cmd.job_id = jobs[j].job_id;
        cmd.op     = jobs[j].op;
        strncpy(cmd.src, jobs[j].src, 35);
        strncpy(cmd.dst, jobs[j].dst, 35);

        if (ipc_send(fw_pid[wid], &cmd, sizeof cmd) == 0) {
            jobs[j].state  = JOB_RUN;
            jobs[j].worker = wid;
            fw_state[wid]  = WS_BUSY;
            fw_job[wid]    = j;
            rr = (wid + 1) % FW_N;
        } else {
            break;                 /* 发送失败（worker 暂时不可用），保留排队 */
        }
    }
}

/* 判断任务是否触碰了当前目录，需要刷新列表 */
static int fm_touches_cwd(fm_job *j)
{
    char d[128];
    if (j->src[0]) {
        fm_dir_of(j->src, d, sizeof d);
        if (strcmp(d, cwd) == 0) return 1;
    }
    if (j->dst[0]) {
        fm_dir_of(j->dst, d, sizeof d);
        if (strcmp(d, cwd) == 0) return 1;
    }
    return 0;
}

/* 每帧轮询：收进度、检测崩溃、分派任务 */
static void fm_poll(win_t *w)
{
    (void)w;

    /* 1) 排空内核邮箱（pid 0）里 worker 回报的进度 */
    fw_prog prog;
    int from;
    while (ipc_recv(&prog, sizeof prog, &from, 0) > 0) {
        if (prog.magic != FW_MAGIC_PROG) continue;
        for (int i = 0; i < FM_MAXJOBS; i++) {
            if (!jobs[i].in_use || jobs[i].job_id != prog.job_id) continue;
            jobs[i].done  = prog.done;
            jobs[i].total = prog.total;
            if (prog.fin) {
                if (prog.err != 0) {
                    jobs[i].state = JOB_FAIL;
                    strncpy(jobs[i].msg, prog.msg, 39);
                    jobs[i].msg[39] = 0;
                    fm_fail++;
                    /* 把 worker 的原话 + 出错的路径显示出来，否则用户只看到
                     * 计数+1，根本不知道是 stat 失败还是读失败、错在哪个文件。 */
                    {
                        char m[64];
                        snprintf(m, sizeof m, "%s failed: %s (%s)",
                                 fm_opname(jobs[i].op), jobs[i].msg, jobs[i].src);
                        fm_set_status(m);
                    }
                } else {
                    jobs[i].state = JOB_DONE;
                    fm_done++;
                    {
                        char m[64];
                        snprintf(m, sizeof m, "%s ok", fm_opname(jobs[i].op));
                        fm_set_status(m);
                    }
                }
                /* 释放对应 worker */
                int wid = jobs[i].worker;
                if (wid >= 0 && wid < FW_N) {
                    fw_state[wid] = WS_IDLE;
                    fw_job[wid]   = -1;
                }
                if (fm_touches_cwd(&jobs[i])) fm_reload();
                jobs[i].in_use = 0;   /* 回收槽位（计数已累计）*/
            }
            break;
        }
    }

    /* 2) 崩溃检测 + 重启识别
     * 注意：#PF 一进来 isr_handler 就打上 fault_vec，但同一个中断返回路上
     * task_after_irq -> maybe_respawn 会立刻把 fault_vec 清 0 并原地重启，
     * 等桌面循环再跑 fm_poll 时 fault_vec 已经看不见了。所以重启计数
     * (task_restarts) 才是可靠的"死过一次"信号：只要它在涨，就说明上一任
     * 实例没了，它在途的任务必须判失败，否则任务永远停在 JOB_RUN。 */
    for (int i = 0; i < FW_N; i++) {
        if (fw_pid[i] <= 0) continue;
        int v = task_fault_vec(fw_pid[i]);
        int r = task_restarts(fw_pid[i]);
        if (v != 0 || r != fw_restarts[i]) {
            /* 该 worker 的在途任务失败 */
            int j = fw_job[i];
            if (j >= 0 && jobs[j].in_use && jobs[j].state == JOB_RUN) {
                char m[40];
                snprintf(m, sizeof m, "worker crashed (vec 0x%X)",
                         (u32)(v ? (u32)v : 0xFEu));
                strncpy(jobs[j].msg, m, 39);
                jobs[j].msg[39] = 0;
                jobs[j].state = JOB_FAIL;
                jobs[j].in_use = 0;
                fm_fail++;
                fm_set_status(m);
            }
            fw_state[i] = WS_IDLE;
            fw_job[i]   = -1;
            fw_restarts[i] = r;
        }
    }

    /* 3) 分派排队任务 */
    fm_dispatch();
    /* 还有活没干完（在跑或排队）就保持刷新：否则进度条/状态栏会停住，
     * 而且排队任务也等不到下一次 fm_poll 去分派。 */
    for (int i = 0; i < FM_MAXJOBS; i++) {
        if (jobs[i].in_use &&
            (jobs[i].state == JOB_RUN || jobs[i].state == JOB_QUEUED)) {
            wm_invalidate();
            break;
        }
    }
}

/* ===================== 绘制 ===================== */
static void fm_draw_list(win_t *w)
{
    wstr(w, 0, 1, "NAME                       TYPE   SIZE", 0x0F, 0x00);

    int rows = 11;   /* 行 2..12 */
    for (int i = 0; i < rows; i++) {
        int idx = scroll + i;
        int ry  = 2 + i;
        if (idx >= filtn) {
            wfill(w, 0, ry, w->client_w, 1, 0x07, 0x00);
            continue;
        }
        fm_ent *e = &st.e[filt[idx]];
        int hl = (idx == sel);
        u8 fg = hl ? 0x00 : 0x07;
        u8 bg = hl ? 0x07 : 0x00;

        char nm[25];
        strncpy(nm, e->name, 24);
        nm[24] = 0;
        const char *ty = e->is_dir ? "<DIR>" : "file";
        char sz[12];
        if (e->is_dir) snprintf(sz, sizeof sz, "-");
        else           snprintf(sz, sizeof sz, "%ld", e->size);

        char line[64];
        snprintf(line, sizeof line, "%-24s %-5s %10s", nm, ty, sz);
        wstr(w, 0, ry, line, fg, bg);
    }
}

static void fm_draw_icons(win_t *w)
{
    for (int i = 0; i < filtn; i++) {
        int band = i / ICON_COLS;
        int col  = i % ICON_COLS;
        int x0 = col * ICON_W;
        int y0 = ICON_Y0 + band * ICON_H;
        if (y0 + ICON_H > w->client_h) break;

        fm_ent *e = &st.e[filt[i]];
        int hl = (i == sel);
        u8 fg = hl ? 0x00 : 0x07;
        u8 bg = hl ? 0x07 : 0x00;

        /* 小图标：CP437 阴影块 0xB2 + 下方名字（截断 8）*/
        wput(w, x0, y0, (char)0xB2, fg, bg);
        wstrn(w, x0, y0 + 1, 8, e->name, fg, bg);
    }
}

static void fm_draw_status(win_t *w)
{
    int dot = (filtn > 0 && strcmp(st.e[filt[0]].name, "..") == 0) ? 1 : 0;
    int real = filtn - dot;

    int run = fm_run_count();
    const char *opn = "idle";
    int pct = 0;
    for (int i = 0; i < FM_MAXJOBS; i++) {
        if (jobs[i].in_use && jobs[i].state == JOB_RUN) {
            opn = fm_opname(jobs[i].op);
            if (jobs[i].total > 0)
                pct = (int)(jobs[i].done * 100u / jobs[i].total);
            break;
        }
    }

    char sb[64];
    snprintf(sb, sizeof sb,
             "%d entries   jobs: %d run %d done %d fail   [%s %d%%]",
             real, run, fm_done, fm_fail, opn, pct);
    wfill(w, 0, 14, w->client_w, 1, 0x00, 0x0B);
    wstr(w, 0, 14, sb, 0x00, 0x0B);
}

static const char *pm_label(void)
{
    switch (pmode) {
        case PM_COPY:    return "Copy to:";
        case PM_MOVE:    return "Move to:";
        case PM_RENAME:  return "New name:";
        case PM_NEWFILE: return "New file:";
        case PM_NEWDIR:  return "New dir:";
        case PM_SEARCH:  return "Search:";
        case PM_PATH:    return "Path:";
        default:         return "";
    }
}

static void fm_draw_prompt(win_t *w)
{
    if (pmode == PM_DEL) {
        fm_ent *e = fm_visible(sel);
        char s[48];
        snprintf(s, sizeof s, "Delete %s? (y/n)",
                 e ? e->name : "?");
        wfill(w, 0, 14, w->client_w, 1, 0x00, 0x0B);
        wstr(w, 0, 14, s, 0x00, 0x0B);
        return;
    }
    char s[64];
    snprintf(s, sizeof s, "%s %s", pm_label(), pbuf);
    wfill(w, 0, 14, w->client_w, 1, 0x00, 0x0B);
    wstr(w, 0, 14, s, 0x00, 0x0B);
    int col = (int)strlen(s);
    if (col < w->client_w)
        wput(w, col, 14, (char)0xDB, 0x0B, 0x00);  /* 块状光标 */
}

static void fm_draw(win_t *w)
{
    fm_poll(w);
    fm_build_filt();

    /* 钳制选择/滚动 */
    if (filtn == 0) sel = 0;
    else if (sel >= filtn) sel = filtn - 1;
    if (sel < 0) sel = 0;
    if (!icon_view) {
        if (sel < scroll) scroll = sel;
        if (sel > scroll + 10) scroll = sel - 10;
        if (scroll < 0) scroll = 0;
    }

    /* 清屏（默认 浅灰/黑）*/
    wfill(w, 0, 0, w->client_w, w->client_h, 0x07, 0x00);

    /* 行 0：路径栏（反白）*/
    {
        char bar[64];
        snprintf(bar, sizeof bar, " %s ", cwd);
        wfill(w, 0, 0, w->client_w, 1, 0x00, 0x0B);
        wstr(w, 0, 0, bar, 0x00, 0x0B);
    }

    /* 行 1：列头（仅列表视图有意义）*/
    if (!icon_view) fm_draw_list(w);
    else            fm_draw_icons(w);

    /* 行 13：提示 / 临时状态（暗色）*/
    if (fm_status[0]) {
        wstr(w, 0, 13, fm_status, 0x08, 0x00);
    } else {
        wstr(w, 0, 13,
             "Enter open  c copy  m move  d del  r ren  n new  N dir  / find  g go  v view  i info  X crash-test",
             0x08, 0x00);
    }

    /* 行 14：状态栏 / 提示输入 */
    if (pmode != PM_NONE) fm_draw_prompt(w);
    else                  fm_draw_status(w);
}

/* ===================== 交互动作 ===================== */
static void fm_move_sel(int d) { sel += d; }

static void fm_parent(void)
{
    if (strcmp(cwd, "/") == 0) return;
    char d[128];
    fm_dir_of(cwd, d, sizeof d);
    if (d[0] == 0) strncpy(d, "/", 2);
    strncpy(cwd, d, 127);
    cwd[127] = 0;
    sel = 0; scroll = 0;
    fm_reload();
}

static void fm_activate(void)
{
    fm_ent *e = fm_visible(sel);
    if (!e) return;
    if (strcmp(e->name, "..") == 0) { fm_parent(); return; }
    if (e->is_dir) {
        fm_join(cwd, cwd, e->name);
        sel = 0; scroll = 0;
        fm_reload();
    } else {
        fm_info();   /* 文件 -> 属性弹窗 */
    }
}

static void fm_info(void)
{
    fm_ent *e = fm_visible(sel);
    if (!e) return;
    char path[128];
    fm_join(path, cwd, e->name);
    char txt[128];
    snprintf(txt, sizeof txt,
             "Name: %s\nType: %s\nSize: %ld bytes\nPath: %s\n",
             e->name,
             e->is_dir ? "Directory" : "File",
             e->size, path);
    desktop_show_window("Properties", txt);
}

static void fm_do_delete(void)
{
    fm_ent *e = fm_visible(sel);
    if (!e) return;
    if (strcmp(e->name, "..") == 0) return;
    char src[128];
    fm_join(src, cwd, e->name);
    if (fm_new_job(FW_OP_DELETE, src, "") < 0)
        fm_set_status("queue full (max 8)");
}

static void fm_start_copy(void)
{
    fm_ent *e = fm_visible(sel);
    if (!e || strcmp(e->name, "..") == 0) return;
    char full[128];
    fm_join(full, cwd, e->name);
    char def[60];
    snprintf(def, sizeof def, "%s.copy", full);
    strncpy(pbuf, def, 50); pbuf[50] = 0; plen = (int)strlen(pbuf);
    pmode = PM_COPY;
}

static void fm_start_move(void)
{
    fm_ent *e = fm_visible(sel);
    if (!e || strcmp(e->name, "..") == 0) return;
    char full[128];
    fm_join(full, cwd, e->name);
    strncpy(pbuf, full, 50); pbuf[50] = 0; plen = (int)strlen(pbuf);
    pmode = PM_MOVE;
}

static void fm_start_del(void)
{
    fm_ent *e = fm_visible(sel);
    if (!e || strcmp(e->name, "..") == 0) return;
    pmode = PM_DEL;
}

static void fm_start_rename(void)
{
    fm_ent *e = fm_visible(sel);
    if (!e || strcmp(e->name, "..") == 0) return;
    strncpy(pbuf, e->name, 50); pbuf[50] = 0; plen = (int)strlen(pbuf);
    pmode = PM_RENAME;
}

static void fm_start_newfile(void)
{
    pbuf[0] = 0; plen = 0; pmode = PM_NEWFILE;
}

static void fm_start_newdir(void)
{
    pbuf[0] = 0; plen = 0; pmode = PM_NEWDIR;
}

static void fm_start_search(void)
{
    pbuf[0] = 0; plen = 0; pmode = PM_SEARCH; search_active = 1;
}

static void fm_start_path(void)
{
    strncpy(pbuf, cwd, 50); pbuf[50] = 0; plen = (int)strlen(pbuf);
    pmode = PM_PATH;
}

/* 确认提示（Enter）：按模式执行 */
static void fm_confirm_prompt(void)
{
    fm_ent *e = fm_visible(sel);
    switch (pmode) {
        case PM_COPY:
            if (e) {
                char src[128];
                fm_join(src, cwd, e->name);
                if (fm_new_job(FW_OP_COPY, src, pbuf) < 0)
                    fm_set_status("queue full (max 8)");
            }
            break;
        case PM_MOVE:
            if (e) {
                char src[128];
                fm_join(src, cwd, e->name);
                if (fm_new_job(FW_OP_MOVE, src, pbuf) < 0)
                    fm_set_status("queue full (max 8)");
            }
            break;
        case PM_RENAME:
            if (e && !e->is_dir) {     /* 主端改名：读->写->删 */
                char src[128], dst[128];
                fm_join(src, cwd, e->name);
                fm_join(dst, cwd, pbuf);
                u32 sz = 0;
                const u8 *d = vfs_read_file(src, &sz);
                if (d) { vfs_write_file(dst, d, sz); vfs_delete(src); }
                fm_reload();
            }
            break;
        case PM_NEWFILE: {
            char p[128];
            fm_join(p, cwd, pbuf);
            u8 z = 0;
            vfs_write_file(p, &z, 0);   /* 空文件 */
            fm_reload();
            break;
        }
        case PM_NEWDIR: {
            char p[128];
            fm_join(p, cwd, pbuf);
            vfs_mkdir(p);
            fm_reload();
            break;
        }
        case PM_PATH: {
            vfs_node_t *nd = vfs_resolve(pbuf);
            if (nd && nd->type == VFS_DIR) {
                strncpy(cwd, pbuf, 127); cwd[127] = 0;
                sel = 0; scroll = 0;
                fm_reload();
            } else {
                fm_set_status("no such directory");
            }
            break;
        }
        case PM_SEARCH:
            /* 保留过滤，search_active 仍为 1 */
            break;
        default:
            break;
    }
    if (pmode != PM_SEARCH) plen = 0;
    pmode = PM_NONE;
}

/* 提示/确认模式下的按键处理 */
static void fm_key_prompt(win_t *w, int c)
{
    (void)w;
    if (pmode == PM_DEL) {
        if (c == 'y' || c == 'Y') fm_do_delete();
        /* 其它（n/N/Esc）即取消 */
        pmode = PM_NONE; plen = 0;
        wm_invalidate();
        return;
    }
    if (c == KEY_ESC) {
        if (pmode == PM_SEARCH) { search_active = 0; pbuf[0] = 0; }
        pmode = PM_NONE; plen = 0;
        wm_invalidate();
        return;
    }
    if (c == KEY_ENTER) {
        pbuf[plen] = 0;
        fm_confirm_prompt();
        wm_invalidate();
        return;
    }
    if (c == KEY_BACKSP) {
        if (plen > 0) plen--;
        wm_invalidate();
        return;
    }
    if (c >= 0x20 && c < 0x7F) {       /* 可打印 ASCII */
        if (plen < 50) pbuf[plen++] = (char)c;
        wm_invalidate();
        return;
    }
}

static void fm_key(win_t *w, int c)
{
    (void)w;
    if (pmode != PM_NONE) { fm_key_prompt(w, c); return; }

    switch (c) {
        case KEY_UP:
            fm_move_sel(icon_view ? -ICON_COLS : -1);
            break;
        case KEY_DOWN:
            fm_move_sel(icon_view ? ICON_COLS : 1);
            break;
        case KEY_LEFT:
            if (icon_view) fm_move_sel(-1);
            break;
        case KEY_RIGHT:
            if (icon_view) fm_move_sel(1);
            break;
        case KEY_ENTER:
            fm_activate();
            break;
        case KEY_BACKSP:
            fm_parent();
            break;
        case 'c': fm_start_copy();   break;
        case 'm': fm_start_move();   break;
        case 'd':
        case KEY_DEL: fm_start_del(); break;
        case 'r': fm_start_rename(); break;
        case 'n': fm_start_newfile(); break;
        case 'N': fm_start_newdir(); break;
        case '/': fm_start_search(); break;
        case 'g': fm_start_path();   break;
        case 'i': fm_info();         break;
        case 'X': fw_crash_test();   break;   /* 测试：制造一次 worker 崩溃 */
        case 'v': icon_view = !icon_view; break;
        case KEY_ESC:
            if (search_active) { search_active = 0; pbuf[0] = 0; }
            break;
        default:
            break;
    }
    /* 无论命中哪个分支，状态都可能变了（路径栏/光标/提示行）。
     * 必须请求重绘：fm_poll（任务分派就藏在里面）只在 fm_draw 里跑，
     * 不 invalidate 的话排队中的任务永远派不出去。 */
    wm_invalidate();
}

static void fm_click(win_t *w, int cx, int cy)
{
    (void)w;
    if (pmode != PM_NONE) return;

    if (icon_view) {
        if (cy < ICON_Y0) return;
        int band = (cy - ICON_Y0) / ICON_H;
        int col  = cx / ICON_W;
        if (col >= ICON_COLS) return;
        int idx = band * ICON_COLS + col;
        if (idx < 0 || idx >= filtn) return;
        if (idx == sel) fm_activate();
        else sel = idx;
    } else {
        if (cy < 2 || cy > 12) return;
        int idx = scroll + (cy - 2);
        if (idx < 0 || idx >= filtn) return;
        if (idx == sel) fm_activate();
        else sel = idx;
    }
}

/* 关闭：通知 worker 退出并回收，取消在途操作 */
static void fm_close(win_t *w)
{
    (void)w;
    fw_cmd bye;
    memset(&bye, 0, sizeof bye);
    bye.magic = FW_MAGIC_BYE;
    for (int i = 0; i < FW_N; i++) {
        if (fw_pid[i] > 0) {
            ipc_send(fw_pid[i], &bye, sizeof bye);
            task_kill(fw_pid[i]);
            task_reap(fw_pid[i]);
            fw_pid[i] = 0;
        }
    }
    fm_opened = 0;
    fm_win    = NULL;
}

/* ===================== 对外 API ===================== */
static int fm_busy_cb(win_t *w)
{
    (void)w;
    return fm_busy();
}

static client_vt fm_vt = {
    .draw  = fm_draw,
    .key   = fm_key,
    .click = fm_click,
    .close = fm_close,
    .busy  = fm_busy_cb,
};

int fm_busy(void)
{
    return fm_run_count() > 0 ? 1 : 0;
}

void fm_open_window(void)
{
    if (!fm_opened) {
        fm_opened = 1;

        /* 首次打开：重置状态 + 派生 4 个常驻 worker */
        memset(jobs, 0, sizeof jobs);
        fm_done = 0; fm_fail = 0;
        sel = 0; scroll = 0; icon_view = 0;
        search_active = 0; pmode = PM_NONE; plen = 0;
        pbuf[0] = 0; fm_status[0] = 0;
        cwd[0] = '/'; cwd[1] = 0;

        for (int i = 0; i < FW_N; i++) {
            fw_state[i]    = WS_IDLE;
            fw_job[i]      = -1;
            fw_restarts[i] = 0;
            int pid = task_spawn("/bin/FW.TNCR");
            fw_pid[i] = pid;
            if (pid > 0) task_set_respawn(pid, 1);
        }
        fm_reload();
    }

    if (fm_win) {
        wm_focus(fm_win);          /* 已存在 -> 重新聚焦 */
    } else {
        fm_win = wm_open(&fm_vt, WC_FILEMGR, "Files", 64, 18);
    }
}
