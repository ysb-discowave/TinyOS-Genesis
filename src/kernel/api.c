#include "api.h"
#include "console.h"
#include "process.h"
#include "desktop.h"
#include "net/net.h"
#include "net/ftp.h"
#include "net/smb.h"
#include "net/sock.h"
#include "user.h"
#include "shell.h"
#include "vga.h"
#include "vfs.h"
#include "pit.h"
#include "libc.h"

/* 内核导出给用户程序的 API 实现 */
static void k_print(const char *s)   { kputs(s); }
static void k_println(const char *s) { kputs(s); kputc('\n'); }
static int  k_readline(char *buf, int max) { return cons_readline(buf, max); }
static int  k_gui_open(const char *t, const char *c) { return gui_open(t, c); }
static void k_desktop(void) { desktop_enter(); }
static int  k_get_session(void) { return proc_current_session(); }
static int  k_ftp_get(const char *h, int p, const char *u, const char *pw,
                      const char *rem, const char *loc) { return ftp_get(h, p, u, pw, rem, loc); }
static int  k_ftp_put(const char *h, int p, const char *u, const char *pw,
                      const char *loc, const char *rem) { return ftp_put(h, p, u, pw, loc, rem); }
static int  k_smb_get(const char *h, int p, const char *sh, const char *u, const char *pw,
                      const char *rem, const char *loc) { return smb_get(h, p, sh, u, pw, rem, loc); }
static int  k_smb_put(const char *h, int p, const char *sh, const char *u, const char *pw,
                      const char *loc, const char *rem) { return smb_put(h, p, sh, u, pw, loc, rem); }
static void k_net_info(void) { net_info(); }
static void k_itoa(int v, char *buf) { itoa(v, buf, 10); }

/* ================================================================
 * 网络服务型用户程序所需的 API
 * ================================================================ */

/* ---- 套接字：只是把 kernel/net/sock.c 的句柄接口导出给用户态 ---- */
static int  k_sock_listen(int port) { return sock_listen(port); }
static int  k_sock_accept(int lh, int timeout_ms) { return sock_accept(lh, timeout_ms); }
static int  k_sock_recv(int h, u8 *buf, int max, int tmo) { return sock_recv(h, buf, max, tmo); }
static int  k_sock_poll_recv(int h, u8 *buf, int max) { return sock_poll_recv(h, buf, max); }
static int  k_sock_send(int h, const u8 *buf, int n) { return sock_send(h, buf, n); }
static void k_sock_close(int h) { sock_close(h); }
static int  k_sock_closed(int h) { return sock_closed(h); }
static int  k_sock_readable(int h) { return sock_readable(h); }
static u32  k_sock_local_ip(void) { return sock_local_ip(); }
static int  k_sock_connect(u32 ip, u16 port, int timeout_ms) {
    return sock_connect(ip, port, timeout_ms);
}

/* ---- 用户 / 口令 ---- */
static int  k_user_verify(const char *n, const char *p) { return user_verify(n, p); }
static int  k_user_uid(const char *n) { user_t *u = user_by_name(n); return u ? u->uid : -1; }
static int  k_user_current(void) { return user_current(); }
static void k_user_set_current(int uid) { user_set_current(uid); }
static int  k_user_name_of(int uid, char *buf, int max) {
    const char *s = user_name_of(uid);
    int i = 0;
    while (s[i] && i < max - 1) { buf[i] = s[i]; i++; }
    buf[i] = 0;
    return i;
}

/* ---- 命令执行并把输出抓进缓冲区 ----
 * SSHD 的 exec 通道与 shell 通道都用它：命令输出必须原样回传远端，
 * 而不是打在本地屏幕上（本地屏幕属于启动守护进程的那个控制台）。
 *
 * 这里刻意挡掉"需要本地独占交互"的命令：它们会去读本地键盘，
 * 一旦从 SSH 里触发就会把守护进程卡死在本地输入上。 */
static int k_exec_capture(const char *cmd, char *buf, int max) {
    if (!buf || max <= 0) return -1;

    /* 先看命令名是否需要本地独占资源 */
    {
        const char *p = cmd;
        while (*p == ' ' || *p == '\t') p++;
        static const char *blocked[] = { "desktop", "edit", "passwd", "su",
                                         "login", "logout", "sshd", "install",
                                         "shot", "screendump", "compile", 0 };
        char word[32];
        int w = 0;
        while (*p && *p != ' ' && *p != '\t' && w < 31) word[w++] = *p++;
        word[w] = 0;
        for (int i = 0; blocked[i]; i++) {
            if (strcmp(word, blocked[i]) == 0) return -2;   /* 不允许在此环境执行 */
        }
    }

    cons_capture_begin(buf, max);
    shell_exec(cmd, proc_current_session());
    int len = cons_capture_len();
    cons_capture_end();
    return len;
}

/* ---- 简单持久化：走正常的权限检查，所以普通用户读不到 /etc/shadow ---- */
static int k_file_read(const char *path, char *buf, int max) {
    if (!buf || max <= 0) return -1;
    if (!user_can_read(path)) return -3;
    u32 size = 0;
    const u8 *d = vfs_read_file(path, &size);
    if (!d) return -1;
    int n = (int)size < max - 1 ? (int)size : max - 1;
    memcpy(buf, d, (u32)n);
    buf[n] = 0;
    return n;
}
static int k_file_write(const char *path, const char *data, int len) {
    if (!data || len < 0) return -1;
    if (!user_can_write(path)) return -3;
    return vfs_write_file(path, (const u8 *)data, (u32)len);
}

/* ---- 随机数 ----
 * 裸机没有专门的熵源，这里把 TSC 与 PIT 滴答混进 xorshift32。
 * 对 SSH 的临时 DH 私钥够用（临时密钥每次连接都新生成，
 * 且单一会话被猜出的代价等价于直接攻破 SSH 会话本身）。 */
static u32 g_rand_state = 0;
static u32 rdtsc_lo(void) {
    u32 lo;
    __asm__ volatile("rdtsc" : "=a"(lo) : : "edx");
    return lo;
}
static void k_rand_bytes(u8 *buf, int n) {
    if (g_rand_state == 0)
        g_rand_state = rdtsc_lo() ^ (pit_ticks() * 2654435761u) ^ 0x9E3779B9u;
    if (!buf || n <= 0) return;
    for (int i = 0; i < n; i++) {
        u32 x = g_rand_state ^ rdtsc_lo();
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        g_rand_state = x;
        buf[i] = (u8)((x >> 11) & 0xFF);
    }
}
static u32 k_ticks(void) { return pit_ticks(); }

static int  k_local_kbhit(void) { return cons_haschar(); }
static int  k_local_getc(void)  { return cons_getchar(); }

/* 直接写 VGA 文本缓冲：全屏程序（编辑器）每次重绘上千字符，
 * 走 kputc 会把串口日志淹掉，自动化测试全靠读串口，所以这里绕开串口。 */
static void k_draw(int x, int y, const char *s, u8 fg, u8 bg) {
    vga_draw_str(x, y, s, fg, bg);
}
static void k_clearscr(void)    { cons_clear(); }
static void k_gotoxy(int x, int y) { vga_setcursor((u32)x, (u32)y); }
static void k_setcolor(u8 fg, u8 bg) { cons_setcolor(fg, bg); }
static int  k_getkey(void)      { return cons_waitkey(); }

/* TNCR 程序没有 argc/argv：Shell 在 tncr_run() 之前把参数串存下来，
 * 程序启动后通过这两个入口取用。 */
static int k_cmdarg(char *buf, int max) { return shell_arg(buf, max); }
static int k_cwd(char *buf, int max)    { return shell_cwd(buf, max); }

tinyos_api_t g_api = {
    .print = k_print,
    .println = k_println,
    .readline = k_readline,
    .gui_open = k_gui_open,
    .desktop = k_desktop,
    .get_session = k_get_session,
    .ftp_get = k_ftp_get,
    .ftp_put = k_ftp_put,
    .smb_get = k_smb_get,
    .smb_put = k_smb_put,
    .net_info = k_net_info,
    .itoa = k_itoa,

    .sock_listen = k_sock_listen,
    .sock_accept = k_sock_accept,
    .sock_recv = k_sock_recv,
    .sock_poll_recv = k_sock_poll_recv,
    .sock_send = k_sock_send,
    .sock_close = k_sock_close,
    .sock_closed = k_sock_closed,
    .sock_readable = k_sock_readable,
    .sock_local_ip = k_sock_local_ip,
    .sock_connect  = k_sock_connect,

    .user_verify = k_user_verify,
    .user_uid = k_user_uid,
    .user_current = k_user_current,
    .user_set_current = k_user_set_current,
    .user_name_of = k_user_name_of,

    .exec_capture = k_exec_capture,
    .file_read = k_file_read,
    .file_write = k_file_write,

    .rand_bytes = k_rand_bytes,
    .ticks = k_ticks,

    .local_kbhit = k_local_kbhit,
    .local_getc = k_local_getc,

    .draw = k_draw,
    .clearscr = k_clearscr,
    .gotoxy = k_gotoxy,
    .setcolor = k_setcolor,
    .getkey = k_getkey,

    .cmdarg = k_cmdarg,
    .cwd = k_cwd,

    /* ---- Genesis v0.1 扩展（kernel/genesis_api.c，追加在末尾）---- */
    .mkdir         = k_mkdir,
    .rm_file       = k_rm_file,
    .fs_list       = k_fs_list,
    .stat          = k_stat,
    .set_cwd       = k_set_cwd,
    .proc_list_all = k_proc_list_all,
    .proc_kill     = k_proc_kill,
    .dev_list_all  = k_dev_list_all,
    .dev_read      = k_dev_read,
    .dev_write     = k_dev_write,
    .sysinfo       = k_sysinfo,
    .date          = k_date,
    .version       = k_version,

    /* ---- Genesis v0.1 pkg 扩展 ---- */
    .sha256_file   = k_sha256_file,

    /* ---- 命令查找链（供 tinysh 转发到内核命令 / 外部 TNCR 程序）---- */
    .kcmd_exists   = k_cmd_exists,
    .kcmd_exec     = k_cmd_exec,
    .prog_exec     = k_prog_exec,

    /* ---- Genesis v0.1 http 扩展（pkg 从 GitHub raw 源拉包）---- */
    .http_get      = k_http_get,
    .http_get_file = k_http_get_file,
    .http_get_proxy = k_http_get_proxy,
    .http_get_file_proxy = k_http_get_file_proxy,
    .http_tunnel = k_http_tunnel,
};
