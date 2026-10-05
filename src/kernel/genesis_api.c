/* ============================================================================
 * genesis_api.c -- Genesis v0.1 内核侧扩展
 * ------------------------------------------------------------
 * 把 tinysh 等用户态 shell 需要的"文件系统 / 进程 / 硬件寄存器 / 系统信息"
 * 能力，通过 tinyos_api_t 末尾追加的 Genesis 扩展字段导出。旧 TNCR 只读前面
 * 的字段，不受影响。
 *
 * 各处理函数以 k_* 全局名导出，由 api.c 直接填进 g_api 的静态初始化器
 * （见 kernel/include/api.h 末尾的声明）—— 编译期绑定，不依赖初始化顺序。
 * ============================================================================ */
#include "api.h"
#include "vfs.h"
#include "process.h"
#include "shell.h"
#include "mm.h"
#include "pit.h"
#include "io.h"
#include "libc.h"
#include "user.h"

/* ----------------------------------------------------------------
 * 设备表（devlist / readdev / writedev 用）
 * 真实 ISA 设备的 IO 基址，按 educational 命名暴露给 shell。
 * ---------------------------------------------------------------- */
typedef struct { const char *name; u16 base; u16 size; } dev_reg_t;
static const dev_reg_t g_devs[] = {
    { "com1", 0x3F8, 8 },   /* 串口 1 */
    { "com2", 0x2F8, 8 },   /* 串口 2 */
    { "pic",  0x20, 2 },    /* 主 PIC */
    { "pit",  0x40, 4 },    /* 定时器 */
    { "vga",  0x3D4, 2 },   /* VGA CRT 索引/数据 */
    { "kbd",  0x60, 1 },    /* 键盘数据/命令 */
    { "ide0", 0x1F0, 8 },   /* 主 IDE */
};
static const int g_ndevs = (int)(sizeof(g_devs) / sizeof(g_devs[0]));

/* ================================================================
 * 文件系统
 * ================================================================ */
struct fs_ctx { void (*cb)(const char*, int, long, void*); void *arg; const char *base; };

static void fs_inner(const char *name, vfs_type t, void *a) {
    struct fs_ctx *c = (struct fs_ctx*)a;
    char full[512];
    int bl = (int)strlen(c->base);
    snprintf(full, sizeof full, "%s%s%s", c->base,
             (bl && c->base[bl - 1] == '/') ? "" : "/", name);
    vfs_node_t *n = vfs_resolve(full);
    long sz = (n && n->type == VFS_FILE) ? (long)n->size : 0;
    c->cb(name, (t == VFS_DIR) ? 1 : 0, sz, c->arg);
}

int k_mkdir(const char *path) {
    int r = vfs_mkdir(path);
    if (r == 0) {
        int uid = user_current() < 0 ? 0 : user_current();
        vfs_set_owner(path, uid, 0755);
    }
    return r;
}
int k_rm_file(const char *path) {
    return vfs_delete(path);
}
int k_fs_list(const char *path,
                     void (*cb)(const char*, int, long, void*), void *arg) {
    struct fs_ctx c; c.cb = cb; c.arg = arg; c.base = path;
    vfs_list(path, fs_inner, &c);
    return 0;
}
int k_stat(const char *path, int *is_dir, long *size) {
    vfs_node_t *n = vfs_resolve(path);
    if (!n) return -1;
    if (is_dir) *is_dir = (n->type == VFS_DIR) ? 1 : 0;
    if (size)   *size  = (long)n->size;
    return 0;
}
int k_set_cwd(const char *path) {
    shell_setcwd(path);
    return 0;
}

/* ================================================================
 * 进程
 * ================================================================ */
void k_proc_list_all(void) { proc_list(); }
int  k_proc_kill(int pid)  { return proc_kill_pid(pid); }   /* 0 / -1 */

/* ================================================================
 * 硬件寄存器（设备表 + 端口 IO）
 * ================================================================ */
static const dev_reg_t *dev_find(const char *name) {
    if (name[0] == '/') name++;            /* 允许 /dev/xxx 或 xxx */
    if (!strncmp(name, "dev/", 4)) name += 4;
    for (int i = 0; i < g_ndevs; i++) {
        if (!strcmp(name, g_devs[i].name)) return &g_devs[i];
    }
    return 0;
}
void k_dev_list_all(void (*cb)(const char*, u16, u16, void*), void *arg) {
    for (int i = 0; i < g_ndevs; i++)
        cb(g_devs[i].name, g_devs[i].base, g_devs[i].size, arg);
}
int k_dev_read(const char *name, u32 off, u8 *buf, int n) {
    const dev_reg_t *d = dev_find(name);
    if (!d || n < 0) return -1;
    for (int i = 0; i < n; i++) buf[i] = inb((u16)(d->base + off + (u32)i));
    return n;
}
int k_dev_write(const char *name, u32 off, const u8 *buf, int n) {
    const dev_reg_t *d = dev_find(name);
    if (!d || n < 0) return -1;
    for (int i = 0; i < n; i++) outb((u16)(d->base + off + (u32)i), buf[i]);
    return n;
}

/* ================================================================
 * 系统信息
 * ================================================================ */
void k_sysinfo(char *buf, int n) {
    u32 total = mm_total(), free = mm_free_bytes();
    u32 up = pit_seconds();
    int pc = proc_count();
    snprintf(buf, n,
             "CPU: i386 (TinyOS protected mode)\n"
             "Mem total: %u KB, Free: %u KB\n"
             "Uptime: %u s\n"
             "Process count: %d\n",
             total / 1024u, free / 1024u, up, pc);
}
void k_date(char *buf, int n) {
    u32 up = pit_seconds();
    u32 hh = (up / 3600u) % 24u, mm = (up / 60u) % 60u, ss = up % 60u;
    snprintf(buf, n, "2026-01-01 %02u:%02u:%02u (uptime %us)\n", hh, mm, ss, up);
}
void k_version(char *buf, int n) {
    snprintf(buf, n,
             "TinyOS Genesis v0.1 | tinysh v0.1 | i386 protected mode\n");
}
