/* kernel_api_tinyos.c — TinyOS 2.0 用户态后端（产品用）
 *
 * ============================================================
 * 关键事实（已在 TinyOS-2.0/kernel/include/api.h 确认）：
 *   TinyOS 2.0 的用户态 .TNCR 程序【只能】通过内核导出的 tinyos_api_t
 *   函数指针表访问内核，不能直接调用 vfs_* / proc_* / inb / outb 等内核
 *   内部符号（它们是内核私有，不会导出给用户程序）。
 *
 *   tinyos_api_t 基础字段（api.h）：
 *     print / println / readline            —— 文本 I/O
 *     file_read / file_write                —— 文件读写
 *     cwd                                   —— 取当前工作目录
 *     ...（网络/socket/用户/控制台等）
 *
 *   api.h 注释明确：可在 tinyos_api_t 末尾【追加】字段，旧 TNCR 只读前面
 *   的字段不受影响。Genesis v0.1 借此扩展出手册 §7 所需的
 *   fs_list / fs_mkdir / fs_delete / fs_stat、
 *   proc_spawn / proc_list / proc_kill、
 *   hw_devlist / hw_read / hw_write、
 *   hw_version / hw_sysinfo / hw_date。
 * ============================================================
 *
 * 输出通道：tinysh.c 里所有 printf/fputs 在 TinyOS 构建下由 TNCR stdio
 * shim 接管（类似 Lua 5.4.7 的 lua_shim.c），shim 把 stdio 映射到
 * api->print / api->println。因此本文件只负责 stdio 之外的内核调用。
 *
 * 构建：随 TinyOS 2.0 用户态 + shim 由 zig 交叉编为 /bin/tinysh.tncr。
 *       通过 -DTINYOS_KERNEL_ROOT=... 指定内核 include 目录（含 api.h），
 *       且定义 -DTINYOS_USER 启用 user_main 入口（见 tinysh.c）。
 */
#ifndef TINYOS_KERNEL_ROOT
#define TINYOS_KERNEL_ROOT "../../../TinyOS-2.0/kernel/include"
#endif

#include "kernel_api.h"
#include "tinysh.h"
#include <string.h>
#include "api.h"   /* tinyos_api_t */

/* ---- Genesis v0.1 内核 API 扩展（追加在 tinyos_api_t 末尾，向后兼容）----
 * 前 sizeof(tinyos_api_t) 字节与 api.h 的 tinyos_api_t 完全一致，
 * 因此旧内核只读到基础字段，新内核读到全部扩展字段。 */
typedef struct genesis_api {
    tinyos_api_t base;          /* 与 api.h 的 tinyos_api_t 逐字段一致 */

    /* ---- fs（manual §7 fs_api）---- */
    int  (*fs_list)(const char *path,
                    void (*cb)(const char *name, int is_dir, long size, void *arg),
                    void *arg);
    int  (*fs_mkdir)(const char *path);
    int  (*fs_delete)(const char *path);
    int  (*fs_stat)(const char *path, int *is_dir, long *size);

    /* ---- proc（manual §7 proc_api）---- */
    int  (*proc_spawn)(const char *name, int session);
    void (*proc_list)(void);
    int  (*proc_kill)(int pid);

    /* ---- hw（manual §7 hw_api）---- */
    int  (*hw_devlist)(const char **out, int *count);
    int  (*hw_read)(const char *dev, unsigned addr, unsigned *val);
    int  (*hw_write)(const char *dev, unsigned addr, unsigned val);

    /* ---- 系统信息（manual §7）---- */
    void (*hw_version)(char *buf, int n);
    void (*hw_sysinfo)(char *buf, int n);
    void (*hw_date)(char *buf, int n);
} genesis_api_t;

static tinyos_api_t  *g_api = NULL;   /* 基础表：print/println/readline/cwd/file_* */
static genesis_api_t *g_gen = NULL;   /* Genesis 扩展表（旧内核为 NULL 语义）*/
static int            g_has_gen = 0;  /* 运行内核是否提供扩展 */

/* 由 TNCR 加载器在 user_main 里第一时间调用（见 tinysh.c） */
void tinysh_init_api(tinyos_api_t *api) {
    g_api = api;
    g_gen = (genesis_api_t *)api;
    /* 简单探测：扩展表提供了 fs_list 即视为 Genesis 内核 */
    g_has_gen = (g_gen && g_gen->fs_list != NULL);
}

/* ===================== fs_api ===================== */
/* 工作目录由 tinysh 自己维护（内核基础 api 只有 cwd() 取，无 chdir() 设）；
 * 启动时用 api->cwd 把会话当前目录种子化，之后所有路径解析成绝对路径再
 * 交给内核，保证与 host 后端行为一致。 */
static char g_cwd[512] = "/";

static void resolve_abs(const char *path, char *out, int n) {
    if (path && path[0] == '/') {
        strncpy(out, path, n - 1);
    } else if (!path || !*path) {
        strncpy(out, g_cwd, n - 1);
    } else if (!strcmp(g_cwd, "/")) {
        snprintf(out, n, "/%s", path);
    } else {
        snprintf(out, n, "%s/%s", g_cwd, path);
    }
    out[n - 1] = 0;
}

int fs_getcwd(char *buf, int n) {
    strncpy(buf, g_cwd, n - 1); buf[n - 1] = 0;
    return 0;
}

int fs_chdir(const char *path) {
    char ap[512]; resolve_abs(path, ap, sizeof ap);
    int is_dir = 0; long sz = 0;
    if (g_has_gen && g_gen->fs_stat) {
        if (g_gen->fs_stat(ap, &is_dir, &sz) != 0) return E_NOENT;
        if (!is_dir) return E_NOENT;
    } else {
        /* 旧内核无 stat：无法校验，直接信任路径存在 */
    }
    strncpy(g_cwd, ap, sizeof g_cwd - 1); g_cwd[sizeof g_cwd - 1] = 0;
    return 0;
}

static fs_entry g_arr[1024];
static int      g_n;
static void gen_list_cb(const char *name, int is_dir, long size, void *arg) {
    (void)arg;
    if (g_n < 1024) {
        strncpy(g_arr[g_n].name, name, 63); g_arr[g_n].name[63] = 0;
        g_arr[g_n].is_dir = is_dir;
        g_arr[g_n].size   = size;
        g_n++;
    }
}

int fs_list(const char *path, fs_entry **out, int *count) {
    char ap[512]; resolve_abs(path, ap, sizeof ap);
    if (!g_has_gen || !g_gen->fs_list) { *out = g_arr; *count = 0; return E_IO; }
    g_n = 0;
    g_gen->fs_list(ap, gen_list_cb, NULL);
    *out = g_arr; *count = g_n;
    return 0;
}

long fs_read(const char *path, char *buf, long n) {
    char ap[512]; resolve_abs(path, ap, sizeof ap);
    if (!g_api || !g_api->file_read) return -(long)E_IO;
    int r = g_api->file_read(ap, buf, (int)n);   /* 返回读取字节数(>=0) 或负=错误 */
    if (r < 0) return -(long)E_NOENT;
    return (long)r;
}

int fs_mkdir(const char *name) {
    char ap[512]; resolve_abs(name, ap, sizeof ap);
    if (!g_has_gen || !g_gen->fs_mkdir) return E_IO;
    return g_gen->fs_mkdir(ap) == 0 ? 0 : E_IO;
}

int fs_remove(const char *path) {
    char ap[512]; resolve_abs(path, ap, sizeof ap);
    if (!g_has_gen || !g_gen->fs_delete) return E_IO;
    /* fs_stat 校验不是目录（v0.1 禁止删目录）*/
    int is_dir = 0; long sz = 0;
    if (g_gen->fs_stat && g_gen->fs_stat(ap, &is_dir, &sz) == 0 && is_dir) return E_PERM;
    return g_gen->fs_delete(ap) == 0 ? 0 : E_IO;
}

int fs_touch(const char *name) {
    char ap[512]; resolve_abs(name, ap, sizeof ap);
    if (!g_api || !g_api->file_write) return E_IO;
    return g_api->file_write(ap, "", 0) == 0 ? 0 : E_IO;
}

/* ===================== proc_api ===================== */
void proc_list_print(void) {
    if (g_has_gen && g_gen->proc_list) { g_gen->proc_list(); return; }
    /* 旧内核：基础 api 无 proc 视图，给出占位说明（通过 stdio shim 输出）*/
    printf("(Genesis 扩展内核才提供进程表；当前内核无 proc 视图)\n");
}

int proc_kill(int pid) {
    (void)pid;
    if (!g_has_gen || !g_gen->proc_kill) return E_NOPID;
    return g_gen->proc_kill(pid) == 0 ? 0 : E_NOPID;
}

/* ===================== hw_api ===================== */
int hw_devlist(hw_dev **out, int *count) {
    static hw_dev arr[8];
    int n = 0;
    if (g_has_gen && g_gen->hw_devlist) {
        const char *names[8]; int k = 0;
        if (g_gen->hw_devlist(names, &k) == 0) {
            for (int i = 0; i < k && n < 8; i++) {
                strncpy(arr[n].path, names[i], 31); arr[n].path[31] = 0; n++;
            }
        }
    }
    *out = arr; *count = n;
    return 0;
}

int hw_readdev(const char *dev, unsigned addr, unsigned *val) {
    if (!g_has_gen || !g_gen->hw_read) return E_IO;
    return g_gen->hw_read(dev, addr, val) == 0 ? 0 : E_IO;
}

int hw_writedev(const char *dev, unsigned addr, unsigned val) {
    if (!g_has_gen || !g_gen->hw_write) return E_IO;
    return g_gen->hw_write(dev, addr, val) == 0 ? 0 : E_IO;
}

/* ===================== 系统信息 ===================== */
void sys_version(char *buf, int n) {
    if (g_has_gen && g_gen->hw_version) { g_gen->hw_version(buf, n); return; }
    snprintf(buf, n, "TinyOS Genesis v0.1  |  tinysh v0.1  |  (内核未提供版本接口)");
}
void sys_sysinfo(char *buf, int n) {
    if (g_has_gen && g_gen->hw_sysinfo) { g_gen->hw_sysinfo(buf, n); return; }
    snprintf(buf, n, "CPU: TinyCore  Mem: -  Uptime: -  Procs: -");
}
void sys_date(char *buf, int n) {
    if (g_has_gen && g_gen->hw_date) { g_gen->hw_date(buf, n); return; }
    snprintf(buf, n, "1970-01-01 00:00:00 (内核未提供时钟接口)");
}
