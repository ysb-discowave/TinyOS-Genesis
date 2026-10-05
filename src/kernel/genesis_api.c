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
#include "sha256.h"
#include "tncr.h"

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

/* ================================================================
 * pkg 校验：用户态只需要"对整个文件算 sha256"的结果，不需要碰
 * sha256_ctx 这种内核内部结构。包可能有几百 KB，这里用固定大小缓冲
 * 分块喂进增量式 sha256，避免把整个文件搬进栈里。
 * ================================================================ */
int k_sha256_file(const char *path, char *out, int n) {
    if (!out || n < 65) return -1;
    u32 sz = 0;
    const u8 *d = vfs_read_file(path, &sz);
    if (!d) return -1;
    sha256_ctx c;
    sha256_init(&c);
    const u32 blk = 4096;
    u32 off = 0;
    while (off < sz) {
        u32 chunk = (sz - off < blk) ? (sz - off) : blk;
        sha256_update(&c, d + off, chunk);
        off += chunk;
    }
    u8 dig[32];
    sha256_final(&c, dig);
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = H[(dig[i] >> 4) & 0xF];
        out[i * 2 + 1] = H[dig[i] & 0xF];
    }
    out[64] = 0;
    return 0;
}

/* ================================================================
 * 命令查找链（供 tinysh 转发到内核命令 / 外部 TNCR 程序）
 * ----------------------------------------------------------------
 * 严格约束只允许改少数文件，shell.c 的命令实现不能动，所以这里单独
 * 维护一份“内核命令名清单”用于纯查询。它和 shell.c 现有的分发链是
 * 同一份事实的两处副本（shell.c 那边是真正的实现 + 别名分发），
 * 新增内核命令时两处都要加。k_cmd_exists 只做 strcmp，绝不执行命令，
 * 因此没有副作用，也不会和 shell_exec 的别名逻辑打架。
 * ================================================================ */
static const char *k_shell_cmds[] = {
    /* 与 tinysh 重叠、但 tinysh 优先的内置同名命令（此处仍登记为内核命令，
     * 以便 kcmd_exists 在 tinysh 没抢走前能正确辨识） */
    "help", "ls", "cd", "pwd", "cat", "mkdir", "rm", "ps", "echo", "clear",
    /* 内核独占命令 */
    "write", "edit", "fs", "run", "desktop", "shot", "screendump", "mouse",
    "net", "whoami", "id", "install", "setup", "wizard", "netconf",
    "part", "partitions", "disk", "sshd", "tinysh", "users", "su", "passwd",
    "useradd", "userdel", "chmod", "chown", "login", "logout",
    "uname", "mem", "uptime", "compile", "cc", "lua",
    0
};

int k_cmd_exists(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; k_shell_cmds[i]; i++)
        if (strcmp(name, k_shell_cmds[i]) == 0) return 1;
    return 0;
}

int k_cmd_exec(const char *name, char *args, int n) {
    (void)n;  /* args 已经是拼好的参数字符串，n（参数个数）这里用不到 */
    if (!name || !name[0]) return -1;
    /* 拼成 “name args” 再交给 shell_exec（它自己会拆命令名与参数）。
     * 注意补一个空格，且 args 为空时不留尾随空格。 */
    char cmd[384];
    if (args && args[0])
        snprintf(cmd, sizeof cmd, "%s %s", name, args);
    else
        snprintf(cmd, sizeof cmd, "%s", name);
    return shell_exec(cmd, proc_current_session());
}

int k_prog_exec(const char *path, char *args, int n) {
    (void)n;  /* session 由 proc_current_session() 取，n 这里用不到 */
    if (!path || !path[0]) return -1;
    /* 与 shell.c 的 run_tncr_with_arg 等价：先把参数串存给下一个 TNCR 程序，
     * 再用当前会话跑它。shell_set_arg / tncr_run 都是公开符号。 */
    shell_set_arg(args ? args : "");
    return tncr_run(path, proc_current_session());
}

/* ================================================================
 * http 扩展：薄封装 src/kernel/net/http.c 的最小 HTTP/1.1 客户端。
 * pkg 用它们从 GitHub raw 源拉包。用户态经 tinyos_api_t 的 http_get /
 * http_get_file 访问，不能直接碰内核网络栈。
 * ================================================================ */
#include "http.h"

int k_http_get(const char *host, int port, const char *path,
               void *buf, int max, int *out_len) {
    return http_get(host, port, path, buf, max, out_len);
}

int k_http_get_file(const char *host, int port, const char *path,
                    const char *local) {
    return http_get_file(host, port, path, local);
}

/* --- HTTP CONNECT 代理 ---
 * GitHub 全面强制 HTTPS（明文一律 301 跳转），企业网络里通常由代理终结
 * TLS。这些只是薄封装，真正的工作在 net/http.c。 */
int k_http_get_proxy(const char *host, int port, const char *path,
                     const char *proxy_host, int proxy_port,
                     void *buf, int max, int *out_len) {
    return http_get_proxy(host, port, path, proxy_host, proxy_port,
                          buf, max, out_len);
}

int k_http_get_file_proxy(const char *host, int port, const char *path,
                          const char *local,
                          const char *proxy_host, int proxy_port) {
    return http_get_file_proxy(host, port, path, local,
                               proxy_host, proxy_port);
}

int k_http_tunnel(const char *proxy_host, int proxy_port,
                  const char *host, int port) {
    return http_connect_tunnel(proxy_host, proxy_port, host, port);
}
