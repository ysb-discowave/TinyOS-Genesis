#include "shell.h"
#include "vga.h"
#include "console.h"
#include "serial.h"
#include "vfs.h"
#include "disk/ata.h"
#include "fs/tinyfs.h"
#include "mbr.h"
#include "editor.h"
#include "tncr.h"
#include "process.h"
#include "desktop.h"
#include "mm.h"
#include "pit.h"
#include "mouse.h"
#include "idt.h"
#include "user.h"
#include "instimg.h"
#include "install.h"
#include "net/net.h"
#include "net/ftp.h"
#include "net/smb.h"
#include "libc.h"
#include "sha256.h"

static char g_cwd[256] = "/home";

/* TNCR 程序是被 fn(&g_api) 直接调起来的，拿不到 argc/argv。
 * Shell 在 tncr_run() 之前把命令后面的参数串存到 g_tncr_arg，
 * 程序启动后用 api->cmdarg() 取走（EDIT.TNCR / CC.TNCR 都靠它）。 */
static char g_tncr_arg[256] = "";

int  shell_cwd(char *out, int n) { strncpy(out, g_cwd, n - 1); out[n - 1] = 0; return 0; }
void shell_setcwd(const char *p) { strncpy(g_cwd, p, 255); g_cwd[255] = 0; }

void shell_set_arg(const char *s) {
    if (!s) s = "";
    strncpy(g_tncr_arg, s, sizeof(g_tncr_arg) - 1);
    g_tncr_arg[sizeof(g_tncr_arg) - 1] = 0;
}
int shell_arg(char *out, int n) {
    if (!out || n <= 0) return -1;
    strncpy(out, g_tncr_arg, n - 1);
    out[n - 1] = 0;
    return (int)strlen(out);
}

/* 统一的"跑一个 /bin 下的 TNCR 程序并带参数"入口 */
static int run_tncr_with_arg(const char *path, const char *arg, int session_id) {
    shell_set_arg(arg);
    return tncr_run(path, session_id);
}

/* 提示符：user@tinyos:/path#  （uid 0 用 '#'，普通用户用 '$'） */
void shell_prompt(char *out, int n) {
    int uid = user_current();
    if (uid < 0) snprintf(out, n, "nobody@tinyos:%s$ ", g_cwd);
    else         snprintf(out, n, "%s@tinyos:%s%c ", user_name_of(uid), g_cwd,
                          uid == 0 ? '#' : '$');
}

/* 登录：成功后把 cwd 落到该用户家目录；三次失败则要求回车重试 */
void shell_login_loop(void) {
    for (;;) {
        user_set_current(-1);
        int uid = user_login_prompt();
        if (uid >= 0) {
            user_t *u = user_by_uid(uid);
            if (u && u->home[0] == '/') { strncpy(g_cwd, u->home, 255); g_cwd[255] = 0; }
            else strcpy(g_cwd, "/");
            return;
        }
        kprintf("press ENTER to try again...");
        char t[8];
        cons_readline(t, sizeof(t));
    }
}

/* 相对路径 -> 绝对路径 */
void shell_resolve(const char *in, char *out, int n) {
    if (in[0] == '/') { strncpy(out, in, n - 1); out[n - 1] = 0; return; }
    strncpy(out, g_cwd, n - 1); out[n - 1] = 0;
    int len = (int)strlen(out);
    if (len > 0 && out[len - 1] != '/') { out[len++] = '/'; out[len] = 0; }
    strncat(out, in, n - 1 - len);
}

/* 权限闸门：不通过时打印统一提示并返回 0 */
static int need_write(const char *path) {
    if (user_can_write(path)) return 1;
    kprintf("%s\n", user_denied_msg());
    return 0;
}
static int need_read(const char *path) {
    if (user_can_read(path)) return 1;
    kprintf("%s\n", user_denied_msg());
    return 0;
}

static void mode_str(int m, vfs_type t, char out[11]) {
    int i = 0;
    out[i++] = (t == VFS_DIR) ? 'd' : '-';
    out[i++] = (m & 0400) ? 'r' : '-';
    out[i++] = (m & 0200) ? 'w' : '-';
    out[i++] = (m & 0100) ? 'x' : '-';
    out[i++] = (m & 0040) ? 'r' : '-';
    out[i++] = (m & 0020) ? 'w' : '-';
    out[i++] = (m & 0010) ? 'x' : '-';
    out[i++] = (m & 0004) ? 'r' : '-';
    out[i++] = (m & 0002) ? 'w' : '-';
    out[i++] = (m & 0001) ? 'x' : '-';
    out[i] = 0;
}

struct ls_arg { int cnt; int longfmt; const char *base; };

static void ls_cb(const char *name, vfs_type t, void *arg) {
    struct ls_arg *a = (struct ls_arg*)arg;
    a->cnt++;
    if (!a->longfmt) {
        kprintf("%s%s  ", name, t == VFS_DIR ? "/" : "");
        return;
    }
    char full[512];
    snprintf(full, sizeof(full), "%s%s%s", a->base,
             (a->base[0] && a->base[strlen(a->base) - 1] == '/') ? "" : "/", name);
    int uid = 0, mode = 0;
    vfs_get_owner(full, &uid, &mode);
    vfs_node_t *n = vfs_resolve(full);
    u32 sz = (n && n->type == VFS_FILE) ? n->size : 0;
    char ms[11];
    mode_str(mode, t, ms);
    kprintf("%s  %-8s %-4d %6u  %s%s\n", ms, user_name_of(uid), uid, sz, name,
            t == VFS_DIR ? "/" : "");
}

static void cmd_ls(const char *arg) {
    int longfmt = 0;
    if (arg && arg[0] == '-' && arg[1] == 'l') {
        longfmt = 1;
        arg += 2;
        while (*arg == ' ') arg++;
    }
    char path[256];
    shell_resolve(arg && arg[0] ? arg : ".", path, sizeof(path));
    vfs_node_t *d = vfs_resolve(path);
    if (!d) { kprintf("ls: no such directory: %s\n", path); return; }
    if (!need_read(path)) return;
    if (d->type == VFS_FILE) {
        if (longfmt) {
            int uid = 0, mode = 0;
            vfs_get_owner(path, &uid, &mode);
            char ms[11]; mode_str(mode, VFS_FILE, ms);
            kprintf("%s  %-8s %-4d %6u  %s\n", ms, user_name_of(uid), uid, d->size, d->name);
        } else {
            kprintf("%s  (%u bytes)\n", d->name, d->size);
        }
        return;
    }
    struct ls_arg a;
    a.cnt = 0; a.longfmt = longfmt; a.base = path;
    vfs_list(path, ls_cb, &a);
    if (!a.cnt) kprintf("(empty dir)\n");
    else if (!longfmt) kprintf("\n");
}

static void cmd_cd(const char *arg) {
    if (!arg || !arg[0]) { strcpy(g_cwd, "/"); return; }
    char path[256];
    shell_resolve(arg, path, sizeof(path));
    vfs_node_t *n = vfs_resolve(path);
    if (!n || n->type != VFS_DIR) { kprintf("cd: not a directory: %s\n", path); return; }
    /* 目录权限：非 root 且非所有者时，需要"其它"位有可进入(x)权限 */
    int uid = user_current();
    if (uid != 0) {
        int ouid = 0, mode = 0;
        vfs_get_owner(path, &ouid, &mode);
        if (ouid != uid && !(mode & 0001)) {
            kprintf("cd: %s\n", user_denied_msg());
            return;
        }
    }
    strncpy(g_cwd, path, 255); g_cwd[255] = 0;
}

static void cmd_cat(const char *arg) {
    if (!arg || !arg[0]) { kprintf("cat: missing file name\n"); return; }
    char path[256]; shell_resolve(arg, path, sizeof(path));
    if (!need_read(path)) return;
    u32 sz; const u8 *d = vfs_read_file(path, &sz);
    if (!d) { kprintf("cat: no such file: %s\n", path); return; }
    for (u32 i = 0; i < sz; i++) kputc((char)d[i]);
    kputc('\n');
}

/* net 子命令 */
static void cmd_net(const char *arg) {
    char tok[10][64];
    int i = 0;
    const char *p = arg;
    while (*p && i < 10) {
        while (*p == ' ') p++;
        if (!*p) break;
        int j = 0;
        while (*p && *p != ' ' && j < 63) tok[i][j++] = *p++;
        tok[i][j] = 0; i++;
    }
    if (i == 0) {
        kprintf("usage:\n");
        kprintf("  net info                          show NIC / IP / gateway / DNS / ARP status\n");
        kprintf("  net dns <host>                    resolve a hostname via DNS\n");
        kprintf("  net dns server <ip>               set the DNS server IP\n");
        kprintf("  net start ftp [port]              start the FTP server inside TinyOS (default 21)\n");
        kprintf("  net start smb [port]              start the TinySMB server inside TinyOS (default 445)\n");
        kprintf("  net get ftp <host> <port> <user> <pass> <remote> <local>\n");
        kprintf("  net put ftp <host> <port> <user> <pass> <local> <remote>\n");
        kprintf("  net get smb <host> <port> <share> <user> <pass> <remote> <local>\n");
        kprintf("  net put smb <host> <port> <share> <user> <pass> <local> <remote>\n");
        kprintf("  net ping <host|ip>                send ICMP echo (hostnames are DNS-resolved)\n");
        kprintf("  net tcping <host|ip> <port>       TCP connect probe (works even when ICMP is blocked)\n");
        return;
    }
    if (strcmp(tok[0], "info") == 0) { net_info(); return; }
    if (strcmp(tok[0], "dns") == 0) {
        if (i >= 2 && strcmp(tok[1], "server") == 0 && i >= 3) {
            g_dns = ip_parse(tok[2]);
            char s[20]; ip_to_str(g_dns, s);
            kprintf("DNS server set to %s\n", s);
            return;
        }
        if (i < 2) { kprintf("usage: net dns <host> | net dns server <ip>\n"); return; }
        u32 ip = 0;
        if (dns_resolve(tok[1], &ip) == 0) {
            char s[20]; ip_to_str(ip, s);
            kprintf("%s -> %s\n", tok[1], s);
        } else kprintf("dns: resolve failed for %s\n", tok[1]);
        return;
    }
    if (strcmp(tok[0], "ping") == 0) {
        if (i < 2) { kprintf("usage: net ping <host|ip>\n"); return; }
        u32 ip = host_to_ip(tok[1]);
        if (ip == 0) { kprintf("ping: cannot resolve %s (DNS not available or timed out)\n", tok[1]); return; }
        char ipstr[20]; ip_to_str(ip, ipstr);
        kprintf("PING %s (%s)\n", tok[1], ipstr);
        int r = net_ping(ip);
        kprintf(r == 0 ? "ping: reply received from %s\n" : "ping: timeout (no reply)\n", ipstr);
        if (r != 0)
            kprintf("hint: QEMU user-net (SLIRP) often drops outbound ICMP. Try 'net tcping %s 80' to verify TCP reachability.\n", tok[1]);
        return;
    }
    if (strcmp(tok[0], "tcping") == 0) {
        if (i < 3) { kprintf("usage: net tcping <host|ip> <port>\n"); return; }
        u32 ip = host_to_ip(tok[1]);
        if (ip == 0) { kprintf("tcping: cannot resolve %s\n", tok[1]); return; }
        u16 port = (u16)atoi(tok[2]);
        char ipstr[20]; ip_to_str(ip, ipstr);
        kprintf("TCP connect %s:%u ...\n", ipstr, port);
        int r = net_tcping(ip, port, 4000);
        kprintf(r == 0 ? "tcping: connected (network reachable)\n" : "tcping: failed / timeout\n");
        return;
    }
    if (strcmp(tok[0], "start") == 0 && i >= 2) {
        int port = (i >= 3) ? atoi(tok[2]) : 0;
        if (strcmp(tok[1], "ftp") == 0) {
            int cp = port ? port : 21;
            int r = ftp_server_start(cp);
            if (r == 0)
                kprintf("FTP server started on port %d (PASV data port 21000; "
                        "launcher must forward both)\n", cp);
            else
                kprintf("FTP server failed to start\n");
        } else if (strcmp(tok[1], "smb") == 0) {
            int r = smb_server_start(port ? port : 445);
            kprintf(r == 0 ? "TinySMB server started on port %d\n" : "TinySMB server failed to start\n",
                    port ? port : 445);
        } else kprintf("unknown service: %s\n", tok[1]);
        return;
    }
    /* net get|put proto host port user pass local remote */
    if (i < 8) { kprintf("missing arguments. Run 'net' for usage.\n"); return; }
    const char *mode = tok[0], *proto = tok[1];
    const char *host = tok[2];
    int port = atoi(tok[3]);
    const char *user = tok[4], *pass = tok[5];
    const char *a6 = tok[6], *a7 = tok[7];

    if (strcmp(proto, "ftp") == 0) {
        int r;
        if (strcmp(mode, "get") == 0) {
            kprintf("ftp: connect %s:%d download %s -> %s ...\n", host, port, a6, a7);
            r = ftp_get(host, port, user, pass, a6, a7);
        } else {
            kprintf("ftp: connect %s:%d upload %s -> %s ...\n", host, port, a6, a7);
            r = ftp_put(host, port, user, pass, a6, a7);
        }
        kprintf(r == 0 ? "result: OK\n" : "result: FAILED (%d)\n", r);
    } else if (strcmp(proto, "smb") == 0) {
        int r;
        if (strcmp(mode, "put") == 0) { kprintf("smb: upload %s -> %s:%s ...\n", a6, host, a7);
            r = smb_put(host, port, "share", user, pass, a6, a7); }
        else { kprintf("smb: download %s:%s -> %s ...\n", host, a7, a6);
            r = smb_get(host, port, "share", user, pass, a7, a6); }
        kprintf(r == 0 ? "result: OK\n" : "result: FAILED (%d)\n", r);
    } else kprintf("unknown protocol: %s\n", proto);
}

/* ==================================================================
 * pkg —— 软件包管理（Genesis v0.1，仅本地源目录）
 * ----------------------------------------------------------------
 * 约束对照：只改本文件；安装必做 sha256 校验（接 kernel/sha256.c）；
 * 内核没有 HTTP 客户端，所以 install 从可配置的本地源目录读取
 * <name>.tncr + <name>.manifest，校验后写入 /bin/<name>.TNCR 并登记到
 * /etc/packages.d/<name>。远程 HTTPS 源尚未实现（help 里如实写明）。
 * 不引入动态分配：全部栈上缓冲，文件内容用 VFS 读。
 * ================================================================== */
#define PKG_BIN_DIR  "/bin"
#define PKG_DB_DIR   "/etc/packages.d"
#define PKG_CONF     "/etc/pkg.conf"
#define PKG_DEF_SRC  "/home/pkgrepo"

/* 从 key=value 文本里取出 key 对应的值（容忍前导空白；不做转义） */
static int pkg_get_kv(const char *buf, u32 sz, const char *key, char *out, int n) {
    out[0] = 0;
    int klen = (int)strlen(key);
    const char *p = buf;
    const char *end = buf + sz;
    while (p < end) {
        const char *line = p;
        while (p < end && *p != '\n' && *p != '\r' && *p != 0) p++;
        int ll = (int)(p - line);
        const char *s = line;
        while (s < line + ll && (*s == ' ' || *s == '\t')) s++;
        int avail = (int)(line + ll - s);
        if (avail > klen && strncmp(s, key, klen) == 0 && s[klen] == '=') {
            const char *v = s + klen + 1;
            while (v < line + ll && (*v == ' ' || *v == '\t')) v++;
            const char *ve = line + ll;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            int vl = (int)(ve - v);
            if (vl >= n) vl = n - 1;
            memcpy(out, v, vl); out[vl] = 0;
            return 0;
        }
        while (p < end && (*p == '\n' || *p == '\r' || *p == 0)) p++;
    }
    return -1;
}

/* 读 /etc/pkg.conf，取 sourcedir（缺省 /home/pkgrepo） */
static void pkg_read_conf(char *sd, int n) {
    snprintf(sd, n, "%s", PKG_DEF_SRC);
    u32 sz = 0;
    const u8 *d = vfs_read_file(PKG_CONF, &sz);
    if (!d) return;
    pkg_get_kv((const char *)d, sz, "sourcedir", sd, n);
}

/* 校验单个已安装包的 sha256：算实际值，和记录里的比对 */
static void pkg_verify_one(const char *name) {
    char binp[256];
    snprintf(binp, sizeof(binp), "%s/%s.TNCR", PKG_BIN_DIR, name);
    vfs_node_t *bin = vfs_resolve(binp);
    if (!bin) { kprintf("pkg: package not found: %s\n", name); return; }
    char act[65];
    sha256_hex(bin->data, bin->size, act);
    char recp[256];
    snprintf(recp, sizeof(recp), "%s/%s", PKG_DB_DIR, name);
    u32 rsz = 0;
    const u8 *rd = vfs_read_file(recp, &rsz);
    char exp[65]; exp[0] = 0;
    if (rd) pkg_get_kv((const char *)rd, rsz, "sha256", exp, sizeof(exp));
    if (!exp[0])
        kprintf("%-16s no sha256 recorded (actual %s)\n", name, act);
    else if (strcasecmp(act, exp) == 0)
        kprintf("%-16s OK\n", name);
    else
        kprintf("%-16s MISMATCH (expected %s)\n", name, exp);
}

struct pkg_va { int cnt; };

static void pkg_list_cb(const char *name, vfs_type t, void *arg) {
    struct pkg_va *a = (struct pkg_va *)arg;
    int nl = (int)strlen(name);
    if (t != VFS_FILE || nl < 5) return;
    if (strcasecmp(name + nl - 5, ".TNCR") != 0) return;
    char pkg[64];
    int k = 0;
    for (int i = 0; i < nl - 5 && k < 63; i++) pkg[k++] = name[i];
    pkg[k] = 0;
    a->cnt++;
    char recp[256];
    snprintf(recp, sizeof(recp), "%s/%s", PKG_DB_DIR, pkg);
    char ver[32]; strcpy(ver, "?");
    u32 rsz = 0;
    const u8 *rd = vfs_read_file(recp, &rsz);
    if (rd) pkg_get_kv((const char *)rd, rsz, "version", ver, sizeof(ver));
    char binp[256];
    snprintf(binp, sizeof(binp), "%s/%s.TNCR", PKG_BIN_DIR, pkg);
    vfs_node_t *nd = vfs_resolve(binp);
    u32 sz = (nd && nd->type == VFS_FILE) ? nd->size : 0;
    kprintf("%-16s %-8s %10u\n", pkg, ver, sz);
}

static void pkg_verify_cb(const char *name, vfs_type t, void *arg) {
    struct pkg_va *a = (struct pkg_va *)arg;
    int nl = (int)strlen(name);
    if (t != VFS_FILE || nl < 5) return;
    if (strcasecmp(name + nl - 5, ".TNCR") != 0) return;
    char pkg[64];
    int k = 0;
    for (int i = 0; i < nl - 5 && k < 63; i++) pkg[k++] = name[i];
    pkg[k] = 0;
    a->cnt++;
    pkg_verify_one(pkg);
}

static void cmd_pkg(const char *arg) {
    /* 取子命令（第一个词）与剩余参数 */
    char sub[32]; int i = 0;
    const char *p = arg;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && i < 31) sub[i++] = *p++;
    sub[i] = 0;
    while (*p == ' ' || *p == '\t') p++;
    const char *rest = p;

    if (sub[0] == 0 || strcmp(sub, "help") == 0) {
        kprintf("pkg: package manager (v0.1, LOCAL SOURCE ONLY)\n");
        kprintf("usage:\n");
        kprintf("  pkg list                        list installed packages\n");
        kprintf("  pkg info <name>                 show details of a package\n");
        kprintf("  pkg verify [name]               verify sha256 of package(s)\n");
        kprintf("  pkg install <name>              install from local source dir\n");
        kprintf("  pkg remove <name>               remove an installed package\n");
        kprintf("  pkg source [set <path>]         show / set the local source dir\n");
        kprintf("  pkg help                        this message\n");
        kprintf("notes:\n");
        kprintf("  install reads <name>.manifest + <name>.tncr from the source dir,\n");
        kprintf("  checks sha256, then writes /bin/<name>.TNCR and registers it.\n");
        kprintf("  remote HTTP/HTTPS download is NOT implemented yet (local dir only).\n");
        return;
    }

    if (strcmp(sub, "list") == 0) {
        kprintf("%-16s %-8s %10s\n", "NAME", "VERSION", "SIZE");
        struct pkg_va a; a.cnt = 0;
        vfs_list(PKG_BIN_DIR, pkg_list_cb, &a);
        if (!a.cnt) kprintf("(no packages installed)\n");
        return;
    }

    if (strcmp(sub, "verify") == 0) {
        char name[64]; int j = 0;
        const char *q = rest;
        while (*q == ' ' || *q == '\t') q++;
        while (*q && *q != ' ' && *q != '\t' && j < 63) name[j++] = *q++;
        name[j] = 0;
        if (name[0]) {
            pkg_verify_one(name);
        } else {
            kprintf("%-16s %s\n", "NAME", "RESULT");
            struct pkg_va a; a.cnt = 0;
            vfs_list(PKG_BIN_DIR, pkg_verify_cb, &a);
            if (!a.cnt) kprintf("(no packages to verify)\n");
        }
        return;
    }

    if (strcmp(sub, "info") == 0) {
        char name[64]; int j = 0;
        const char *q = rest;
        while (*q == ' ' || *q == '\t') q++;
        while (*q && *q != ' ' && *q != '\t' && j < 63) name[j++] = *q++;
        name[j] = 0;
        if (!name[0]) { kprintf("usage: pkg info <name>\n"); return; }
        char binp[256], recp[256];
        snprintf(binp, sizeof(binp), "%s/%s.TNCR", PKG_BIN_DIR, name);
        snprintf(recp, sizeof(recp), "%s/%s", PKG_DB_DIR, name);
        vfs_node_t *bin = vfs_resolve(binp);
        vfs_node_t *rec = vfs_resolve(recp);
        if (!bin && !rec) { kprintf("pkg: package not found: %s\n", name); return; }
        kprintf("package: %s\n", name);
        if (rec) {
            u32 rsz = 0;
            const u8 *rd = vfs_read_file(recp, &rsz);
            char v[32], src[128], sh[65], desc[128];
            v[0] = src[0] = sh[0] = desc[0] = 0;
            if (rd) {
                pkg_get_kv((const char *)rd, rsz, "version", v, sizeof(v));
                pkg_get_kv((const char *)rd, rsz, "source", src, sizeof(src));
                pkg_get_kv((const char *)rd, rsz, "sha256", sh, sizeof(sh));
                pkg_get_kv((const char *)rd, rsz, "desc", desc, sizeof(desc));
            }
            kprintf("  version : %s\n", v[0] ? v : "?");
            kprintf("  source  : %s\n", src[0] ? src : "(unregistered)");
            kprintf("  sha256  : %s\n", sh[0] ? sh : "(none)");
            if (desc[0]) kprintf("  desc    : %s\n", desc);
        } else {
            kprintf("  version : ?\n");
            kprintf("  source  : (unregistered: only raw /bin file present)\n");
        }
        if (bin) {
            kprintf("  size    : %u bytes\n", bin->size);
            char act[65];
            sha256_hex(bin->data, bin->size, act);
            kprintf("  sha256  : %s\n", act);
            u32 rsz = 0;
            const u8 *rd = vfs_read_file(recp, &rsz);
            char exp[65]; exp[0] = 0;
            if (rd) pkg_get_kv((const char *)rd, rsz, "sha256", exp, sizeof(exp));
            if (exp[0]) kprintf("  verify  : %s\n", strcasecmp(act, exp) == 0 ? "OK" : "MISMATCH");
            else        kprintf("  verify  : no recorded sha256\n");
        } else {
            kprintf("  note    : binary %s.TNCR is missing; only metadata present\n", binp);
        }
        return;
    }

    if (strcmp(sub, "remove") == 0) {
        char name[64]; int j = 0;
        const char *q = rest;
        while (*q == ' ' || *q == '\t') q++;
        while (*q && *q != ' ' && *q != '\t' && j < 63) name[j++] = *q++;
        name[j] = 0;
        if (!name[0]) { kprintf("usage: pkg remove <name>\n"); return; }
        char binp[256], recp[256];
        snprintf(binp, sizeof(binp), "%s/%s.TNCR", PKG_BIN_DIR, name);
        snprintf(recp, sizeof(recp), "%s/%s", PKG_DB_DIR, name);
        if (!vfs_resolve(binp) && !vfs_resolve(recp)) {
            kprintf("pkg: package not found: %s\n", name);
            return;
        }
        int did = 0;
        if (vfs_resolve(binp)) {
            if (vfs_delete(binp) == 0) did = 1;
            else kprintf("pkg: failed to delete %s\n", binp);
        }
        if (vfs_resolve(recp)) {
            if (vfs_delete(recp) == 0) did = 1;
            else kprintf("pkg: failed to delete record %s\n", recp);
        }
        kprintf(did ? "pkg: removed %s\n" : "pkg: nothing removed\n", name);
        return;
    }

    if (strcmp(sub, "source") == 0) {
        /* pkg source set <path> */
        const char *r = rest;
        while (*r == ' ' || *r == '\t') r++;
        if (!strncmp(r, "set", 3) && (r[3] == ' ' || r[3] == '\t' || r[3] == 0)) {
            const char *q = r + 3;
            while (*q == ' ' || *q == '\t') q++;
            char path[256]; int j = 0;
            while (*q && *q != ' ' && *q != '\t' && j < 255) path[j++] = *q++;
            path[j] = 0;
            if (!path[0]) { kprintf("usage: pkg source set <path>\n"); return; }
            if (!need_write(PKG_CONF)) return;
            char conf[300];
            snprintf(conf, sizeof(conf), "sourcedir=%s\n", path);
            if (vfs_write_file(PKG_CONF, (const u8 *)conf, (u32)strlen(conf)) == 0)
                kprintf("pkg: source set to %s\n", path);
            else
                kprintf("pkg: failed to write %s\n", PKG_CONF);
            return;
        }
        char sd[256];
        pkg_read_conf(sd, sizeof(sd));
        kprintf("pkg: source dir: %s\n", sd);
        kprintf("pkg: remote HTTP/HTTPS sources are NOT implemented yet (local dir only)\n");
        return;
    }

    if (strcmp(sub, "install") == 0) {
        char name[64]; int j = 0;
        const char *q = rest;
        while (*q == ' ' || *q == '\t') q++;
        while (*q && *q != ' ' && *q != '\t' && j < 63) name[j++] = *q++;
        name[j] = 0;
        if (!name[0]) {
            kprintf("usage: pkg install <name>\n");
            kprintf("  (installs from the local source dir; remote download not yet implemented)\n");
            return;
        }
        char sd[256];
        pkg_read_conf(sd, sizeof(sd));
        vfs_node_t *sdnode = vfs_resolve(sd);
        if (!sdnode || sdnode->type != VFS_DIR) {
            kprintf("pkg: source dir missing: %s\n", sd);
            return;
        }
        /* 清单 */
        char mpath[512];
        snprintf(mpath, sizeof(mpath), "%s/%s.manifest", sd, name);
        u32 msz = 0;
        const u8 *md = vfs_read_file(mpath, &msz);
        if (!md) { kprintf("pkg: manifest not found: %s\n", mpath); return; }
        /* 二进制：优先小写 .tncr，回退大写 .TNCR */
        char bpath[512];
        u32 bsz = 0;
        const u8 *bd = NULL;
        snprintf(bpath, sizeof(bpath), "%s/%s.tncr", sd, name);
        bd = vfs_read_file(bpath, &bsz);
        if (!bd) {
            snprintf(bpath, sizeof(bpath), "%s/%s.TNCR", sd, name);
            bd = vfs_read_file(bpath, &bsz);
        }
        if (!bd) {
            kprintf("pkg: package file not found: %s/%s.[tT]NCR\n", sd, name);
            return;
        }
        /* sha256 校验 */
        char exp[65]; exp[0] = 0;
        pkg_get_kv((const char *)md, msz, "sha256", exp, sizeof(exp));
        if (exp[0]) {
            char act[65];
            sha256_hex(bd, bsz, act);
            if (strcasecmp(act, exp) != 0) {
                kprintf("pkg: sha256 mismatch: %s\n", name);
                kprintf("  expected %s\n  actual   %s\n", exp, act);
                return;
            }
        } else {
            kprintf("pkg: warning: no sha256 in manifest, skipping verification\n");
        }
        /* 写 /bin/<name>.TNCR（需要写权限） */
        char binp[256];
        snprintf(binp, sizeof(binp), "%s/%s.TNCR", PKG_BIN_DIR, name);
        if (!need_write(binp)) return;
        if (vfs_write_file(binp, bd, bsz) != 0) {
            kprintf("pkg: failed to write %s\n", binp);
            return;
        }
        /* 登记到 /etc/packages.d/<name> */
        char recp[256];
        snprintf(recp, sizeof(recp), "%s/%s", PKG_DB_DIR, name);
        if (!vfs_resolve(PKG_DB_DIR)) vfs_mkdir(PKG_DB_DIR);
        if (vfs_write_file(recp, md, msz) != 0)
            kprintf("pkg: warning: installed but failed to write record %s\n", recp);
        char ver[32]; ver[0] = 0;
        pkg_get_kv((const char *)md, msz, "version", ver, sizeof(ver));
        kprintf("pkg: installed %s (version %s, %u bytes) -> %s\n",
                name, ver[0] ? ver : "?", bsz, binp);
        return;
    }

    kprintf("pkg: unknown subcommand: %s (try 'pkg help')\n", sub);
}

/* ---- 磁盘 / 持久化文件系统 ---- */
static void cmd_fs(const char *arg) {
    char tok[32];
    int i = 0;
    const char *p = arg;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && i < 31) tok[i++] = *p++;
    tok[i] = 0;

    if (!tok[0] || strcmp(tok, "info") == 0) {
        ata_info();
        if (tfs_mounted()) tfs_info();
        else kprintf("TinyFS: not mounted (no disk, or disk not formatted)\n");
        return;
    }
    if (strcmp(tok, "sync") == 0) {           /* 内存 VFS -> 磁盘 */
        if (!tfs_mounted()) { kprintf("fs: TinyFS not mounted\n"); return; }
        int n = vfs_install_disk();
        kprintf(n >= 0 ? "fs: synced %d entries to disk\n" : "fs: sync failed\n", n);
        return;
    }
    if (strcmp(tok, "load") == 0) {           /* 磁盘 -> 内存 VFS */
        if (!tfs_mounted()) { kprintf("fs: TinyFS not mounted\n"); return; }
        int n = vfs_load_disk();
        kprintf(n >= 0 ? "fs: loaded %d files from disk\n" : "fs: load failed\n", n);
        return;
    }
    if (strcmp(tok, "format") == 0) {
        if (!ata_present()) { kprintf("fs: no usable disk\n"); return; }
        kprintf("fs: this erases ALL TinyFS data on disk! type yes to confirm: ");
        char ans[16];
        cons_readline(ans, sizeof(ans));
        if (strcmp(ans, "yes") != 0) { kprintf("fs: cancelled\n"); return; }
        if (tfs_format() != 0) { kprintf("fs: format failed\n"); return; }
        int n = vfs_install_disk();
        kprintf("fs: formatted, wrote %d built-in entries\n", n);
        return;
    }
    kprintf("usage: fs [info|sync|load|format]\n");
    kprintf("  info    show disk and TinyFS status (default)\n");
    kprintf("  sync    write all files back to disk\n");
    kprintf("  load    reload all files from disk\n");
    kprintf("  format  format TinyFS on disk (erases all data)\n");
}

/* ---- VGA 文本缓冲导出（只走串口）----
 * 文本模式的屏幕内容就在 0xB8000：80x25 格，每格 2 字节（字符 + 属性）。
 * 把它从串口吐出来，就能在无显示器（-display none）的自动化环境里精确核对
 * 屏幕上究竟显示了什么。绝不能走 kprintf —— 那会把内容再写一遍屏幕、污染现场。 */
static void ser_hex8(u8 v) {
    const char *d = "0123456789ABCDEF";
    serial_putc(d[(v >> 4) & 0xF]);
    serial_putc(d[v & 0xF]);
}

void shell_shot(void) {
    u16 *buf = vga_buffer();
    serial_puts("\n[shot] begin 80x25\n");
    for (int y = 0; y < VGA_HEIGHT; y++) {
        serial_puts("T"); ser_hex8((u8)y); serial_putc('|');
        for (int x = 0; x < VGA_WIDTH; x++) {
            u8 ch = (u8)(buf[y * VGA_WIDTH + x] & 0xFF);
            serial_putc((ch >= 32 && ch < 127) ? (char)ch : '.');
        }
        serial_puts("|\n");
        serial_puts("C"); ser_hex8((u8)y); serial_putc('|');
        for (int x = 0; x < VGA_WIDTH; x++) ser_hex8((u8)(buf[y * VGA_WIDTH + x] & 0xFF));
        serial_puts("|\n");
        serial_puts("V"); ser_hex8((u8)y); serial_putc('|');
        for (int x = 0; x < VGA_WIDTH; x++)
            ser_hex8((u8)((buf[y * VGA_WIDTH + x] >> 8) & 0xFF));
        serial_puts("|\n");
    }
    serial_puts("[shot] end\n");
}

/* 前置声明：定义在文件后部（shell_main 附近），但 shell_exec 的 login/logout
 * 分支会用到。 */
static void tinysh_autostart_once(void);

/* ---- 单条命令执行（供 Shell 与桌面终端共用）---- */
int shell_exec(const char *cmd, int session_id) {
    char buf[256]; strncpy(buf, cmd, 255); buf[255] = 0;
    char *p = buf;
    while (*p == ' ' || *p == '\t') p++;
    char *cmd0 = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) { *p++ = 0; }
    while (*p == ' ' || *p == '\t') p++;
    char *arg = p;

    if (cmd0[0] == 0) return 0;

    if (strcmp(cmd0, "help") == 0) {
        kprintf("TinyOS commands:\n");
        kprintf("  ls [-l] cd pwd mkdir rm cat write   file operations\n");
        kprintf("  edit [file]                     full-screen text editor (EDIT.TNCR)\n");
        kprintf("  cc <src.mc> [-o out.TNCR]       compile MiniC source inside TinyOS (CC.TNCR)\n");
        kprintf("  lua [script.lua]                run Lua 5.4 / interactive REPL (LUA.TNCR)\n");
        kprintf("  fs [info|sync|load|format]      disk and persistent filesystem\n");
        kprintf("  run <file.TNCR> [args]          run a TinyOS program\n");
        kprintf("  desktop                         enter the graphical desktop\n");
        kprintf("  install                         install TinyOS onto a hard disk (bootable)\n");
        kprintf("  setup                           setup wizard: disk / network / components / users\n");
        kprintf("  netconf                         show the network configuration (/etc/net.conf)\n");
        kprintf("  part [disk]                     show the MBR partition table (of a disk)\n");
        kprintf("  disk [default <name>]           list all disks / pick the default disk\n");
        kprintf("  net <...>                       network (FTP/SMB/put/get/ping)\n");
        kprintf("  pkg <...>                       package manager (list/info/verify/install/remove/source)\n");
        kprintf("  sshd                            start the SSH server (port from /etc/sshd.conf)\n");
        kprintf("  tinysh                          start the Genesis user shell (type `exit` to return)\n");
        kprintf("  ps uname mem uptime date mouse  system information\n");
        kprintf("  shot                            dump the VGA text screen to serial (debug)\n");
        kprintf("  clear echo compile exit\n");
        kprintf("users and access:\n");
        kprintf("  whoami id users                 current identity / account list\n");
        kprintf("  passwd [user] su [user] login logout\n");
        kprintf("  useradd <name> [uid] userdel <name>      (root only)\n");
        kprintf("  chmod <octal> <path> chown <user> <path>\n");
        kprintf("  tip: start QEMU with a disk attached and files survive a reboot.\n");
    } else if (strcmp(cmd0, "clear") == 0) {
        vga_clear(0x00);
    } else if (strcmp(cmd0, "echo") == 0) {
        kprintf("%s\n", arg);
    } else if (strcmp(cmd0, "pwd") == 0) {
        kprintf("%s\n", g_cwd);
    } else if (strcmp(cmd0, "ls") == 0) {
        cmd_ls(arg);
    } else if (strcmp(cmd0, "cd") == 0) {
        cmd_cd(arg);
    } else if (strcmp(cmd0, "mkdir") == 0) {
        if (!arg[0]) kprintf("mkdir: missing path\n");
        else { char path[256]; shell_resolve(arg, path, sizeof(path));
               if (!need_write(path)) return 0;
               int r = vfs_mkdir(path);
               if (r == 0) vfs_set_owner(path, user_current() < 0 ? 0 : user_current(), 0755);
               else kprintf("mkdir: failed\n"); }
    } else if (strcmp(cmd0, "rm") == 0) {
        if (!arg[0]) kprintf("rm: missing path\n");
        else { char path[256]; shell_resolve(arg, path, sizeof(path));
               if (!need_write(path)) return 0;
               kprintf(vfs_delete(path) == 0 ? "" : "rm: failed\n"); }
    } else if (strcmp(cmd0, "cat") == 0) {
        cmd_cat(arg);
    } else if (strcmp(cmd0, "write") == 0) {
        /* write <文件> <内容...> */
        char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        if (!*sp) { kprintf("usage: write <file> <content>\n"); return 0; }
        *sp = 0; char *content = sp + 1;
        char path[256]; shell_resolve(arg, path, sizeof(path));
        if (!need_write(path)) return 0;
        if (vfs_write_file(path, (const u8*)content, (u32)strlen(content)) == 0) {
            /* 新建的文件归当前用户所有，之后他能自由修改 */
            vfs_set_owner(path, user_current() < 0 ? 0 : user_current(), 0644);
            kprintf("wrote %s (%u bytes)\n", path, (u32)strlen(content));
        } else kprintf("write: failed\n");
    } else if (strcmp(cmd0, "edit") == 0) {
        /* edit [文件]  —— 全屏编辑器（EDIT.TNCR）；保存时自动落盘。
         * 优先跑 /bin/EDIT.TNCR（TNCR 版编辑器，功能与内核内置版一致且可
         * 直接在系统里改），找不到才退回内核里编译进来的 editor_open()。 */
        if (arg[0]) {
            char path[256];
            shell_resolve(arg, path, sizeof(path));
            if (!need_write(path)) return 0;
            if (!need_read(path)) return 0;
            if (vfs_resolve("/bin/EDIT.TNCR")) {
                run_tncr_with_arg("/bin/EDIT.TNCR", path, session_id);
                return 0;
            }
            editor_open(path);
        } else {
            if (vfs_resolve("/bin/EDIT.TNCR")) {
                run_tncr_with_arg("/bin/EDIT.TNCR", "", session_id);
                return 0;
            }
            editor_open(NULL);          /* 未命名缓冲，保存时询问文件名 */
        }
    } else if (strcmp(cmd0, "fs") == 0) {
        cmd_fs(arg);
    } else if (strcmp(cmd0, "run") == 0) {
        if (!arg[0]) kprintf("run: missing TNCR path\n");
        else {
            char path[256], rest[256];
            /* 取第一个词作为程序路径，剩下的整串当作参数递给程序 */
            int i = 0;
            while (arg[i] && arg[i] != ' ' && arg[i] != '\t') i++;
            char first[256];
            memcpy(first, arg, i); first[i] = 0;
            while (arg[i] == ' ' || arg[i] == '\t') i++;
            strncpy(rest, arg + i, sizeof(rest) - 1); rest[sizeof(rest) - 1] = 0;
            shell_resolve(first, path, sizeof(path));
            if (run_tncr_with_arg(path, rest, session_id) != 0)
                kprintf("run: execution failed\n");
        }
    } else if (strcmp(cmd0, "desktop") == 0) {
        desktop_enter();
    } else if (strcmp(cmd0, "shot") == 0 || strcmp(cmd0, "screendump") == 0) {
        shell_shot();
    } else if (strcmp(cmd0, "mouse") == 0) {
        /* 鼠标自检：IRQ12 计数为 0 就等于"一次中断都没收到"，
         * 配合 packet 计数能立刻区分"中断没送达"和"数据流错位"。 */
        int mx, my; u8 b;
        mouse_state(&mx, &my, &b);
        kprintf("mouse: %s  (init step=%d)\n",
                mouse_present() ? "present" : "NOT detected", mouse_init_step());
        kprintf("  pos=(%d,%d) buttons=0x%X\n", mx, my, b);
        kprintf("  IRQ12 hits=%u  packets ok=%u bad=%u\n",
                irq_hits(12), mouse_packets_ok(), mouse_packets_bad());
        {
            u8 pm, ps;
            pic_get_masks(&pm, &ps);
            kprintf("  PIC master=0x%02X slave=0x%02X  cascade IRQ2=%s\n",
                    pm, ps, (pm & 0x04) ? "MASKED" : "open");
        }
        if (arg[0] == 'm') {            /* mouse move <x> <y> */
            const char *q = arg + 4;
            while (*q == ' ') q++;
            int nx = atoi(q);
            while (*q && *q != ' ') q++;
            while (*q == ' ') q++;
            int ny = atoi(q);
            mouse_set_position(nx, ny);
            kprintf("  moved to (%d,%d)\n", nx, ny);
        }
    } else if (strcmp(cmd0, "net") == 0) {
        cmd_net(arg);
    } else if (strcmp(cmd0, "pkg") == 0) {
        cmd_pkg(arg);
    } else if (strcmp(cmd0, "whoami") == 0) {
        int uid = user_current();
        kprintf("%s\n", uid < 0 ? "nobody" : user_name_of(uid));
    } else if (strcmp(cmd0, "id") == 0) {
        int uid = user_current();
        user_t *u = user_by_uid(uid);
        kprintf("uid=%d gid=%d%s home=%s\n", uid, u ? u->gid : -1,
                uid == 0 ? " (root)" : "", u ? u->home : "-");
    } else if (strcmp(cmd0, "install") == 0) {
        installer_run(arg);
        return 0;
    } else if (strcmp(cmd0, "setup") == 0 || strcmp(cmd0, "wizard") == 0) {
        /* 分步安装向导：磁盘 -> 网络 -> 组件 -> 账户 -> 确认并安装 */
        installer_wizard(arg);
        return 0;
    } else if (strcmp(cmd0, "netconf") == 0) {
        netconf_print();
        return 0;
    } else if (strcmp(cmd0, "part") == 0 || strcmp(cmd0, "partitions") == 0) {
        /* part [disk]  —— 多盘时可指定要打印哪块的分区表 */
        char name[16] = "";
        int i;
        const char *p = arg;
        while (*p == ' ') p++;
        for (i = 0; *p && *p != ' ' && i < 15; i++) name[i] = *p++;
        name[i] = 0;
        if (name[0]) {
            int idx = disk_find_name(name);
            if (idx < 0) {
                kprintf("part: no disk named '%s'. available:\n", name);
                disk_list();
                return 0;
            }
            mbr_print2(disk_get(idx));
        } else {
            mbr_print();
        }
        return 0;
    } else if (strcmp(cmd0, "disk") == 0) {
        /* disk [default <name>]  —— 列出所有盘；可把默认盘指到某一块 */
        const char *p = arg;
        while (*p == ' ') p++;
        if (!strncmp(p, "default", 7) && (p[7] == 0 || p[7] == ' ')) {
            p += 7;
            while (*p == ' ') p++;
            if (!*p) {
                kprintf("disk: usage: disk default <name>   (name from the 'disk' listing)\n");
                return 0;
            }
            char name[16]; int i = 0;
            while (*p && *p != ' ' && i < 15) name[i++] = *p++;
            name[i] = 0;
            int idx = disk_find_name(name);
            if (idx < 0) {
                kprintf("disk: no disk named '%s'. available:\n", name);
                disk_list();
                return 0;
            }
            disk_set_default(disk_get(idx));
            return 0;
        }
        disk_list();
        return 0;
    } else if (strcmp(cmd0, "sshd") == 0) {
        /* SSH 服务端是一个 TNCR 程序（真正的 SSH-2 实现）。
         * 内核是协作式单线程，所以它在前台运行：服务期间本地控制台让位，
         * 交互都走 SSH；在 SSH 会话里按 ESC 或断开最后一个连接即可返回。 */
        if (!vfs_resolve("/bin/SSHD.TNCR")) {
            kprintf("sshd: /bin/SSHD.TNCR is not installed\n");
            kprintf("      re-run the installer with --with-ssh, or rebuild the romfs\n");
            return 0;
        }
        if (g_net_mac[0] == 0) {
            kprintf("sshd: no NIC detected; SSH needs the network\n");
            return 0;
        }
        kprintf("sshd: starting TinyOS SSH server (see the SSH session for output)\n");
        run_tncr_with_arg("/bin/SSHD.TNCR", "", session_id);
        kprintf("sshd: server stopped\n");
    } else if (strcmp(cmd0, "tinysh") == 0) {
        /* Genesis v0.1 的用户态 shell（TNCR 程序）。和 sshd 一样是
         * 协作式前台运行：它自己经 api->readline 读满一整段输入，
         * 所以期间本地控制台让位；tinysh 里敲 exit 就返回内核 shell。 */
        if (!vfs_resolve("/bin/tinysh.TNCR")) {
            kprintf("tinysh: /bin/tinysh.TNCR is not installed\n");
            kprintf("        rebuild the romfs (tools/build_users.py) to include it\n");
            return 0;
        }
        kprintf("[tinysh] starting the Genesis user shell; type `exit` to come back\n");
        run_tncr_with_arg("/bin/tinysh.TNCR", "", session_id);
        kprintf("[tinysh] exited\n");
    } else if (strcmp(cmd0, "users") == 0) {
        user_list();
    } else if (strcmp(cmd0, "su") == 0) {
        const char *t = arg[0] ? arg : "root";
        if (!user_by_name(t)) { kprintf("su: no such user: %s\n", t); return 0; }
        kprintf("Password for %s: ", t);
        char pw[64];
        int n = cons_readline_masked(pw, sizeof(pw));
        pw[n < 0 ? 0 : n] = 0;
        kprintf("\n");
        if (user_verify(t, pw) != 0) { kprintf("su: authentication failed\n"); return 0; }
        user_t *u = user_by_name(t);
        user_set_current(u->uid);
        strncpy(g_cwd, u->home, 255); g_cwd[255] = 0;
        kprintf("now acting as %s (uid %d)\n", u->name, u->uid);
    } else if (strcmp(cmd0, "passwd") == 0) {
        /* 取目标用户名（去掉尾部空格） */
        char target[USER_NAME_MAX];
        int ti = 0;
        for (const char *q = arg; *q && *q != ' ' && ti < USER_NAME_MAX - 1; q++) target[ti++] = *q;
        target[ti] = 0;
        if (!target[0]) snprintf(target, sizeof(target), "%s", user_name_of(user_current()));

        int me = user_current();
        int self = (me >= 0) && (strcmp(user_name_of(me), target) == 0);
        if (!self && me != 0) {
            kprintf("passwd: only root may change another user's password\n");
            return 0;
        }
        if (!user_by_name(target)) { kprintf("passwd: no such user: %s\n", target); return 0; }

        if (me != 0) {                       /* 普通用户改自己的口令要先验旧口令 */
            kprintf("Current password: ");
            char old[64];
            int n = cons_readline_masked(old, sizeof(old));
            old[n < 0 ? 0 : n] = 0;
            kprintf("\n");
            if (user_verify(target, old) != 0) {
                kprintf("passwd: authentication failed\n");
                return 0;
            }
        }
        kprintf("New password: ");
        char p1[64];
        int n1 = cons_readline_masked(p1, sizeof(p1)); p1[n1 < 0 ? 0 : n1] = 0;
        kprintf("\n");
        if (strlen(p1) < 3) { kprintf("passwd: too short (minimum 3 characters)\n"); return 0; }
        kprintf("Retype new password: ");
        char p2[64];
        int n2 = cons_readline_masked(p2, sizeof(p2)); p2[n2 < 0 ? 0 : n2] = 0;
        kprintf("\n");
        if (strcmp(p1, p2) != 0) { kprintf("passwd: passwords do not match\n"); return 0; }
        kprintf(user_change_pass(target, p1) == 0 ? "passwd: updated\n" : "passwd: failed\n");
    } else if (strcmp(cmd0, "useradd") == 0) {
        if (user_current() != 0) { kprintf("useradd: permission denied (root only)\n"); return 0; }
        char nm[USER_NAME_MAX];
        int i = 0;
        for (const char *q = arg; *q && *q != ' ' && i < USER_NAME_MAX - 1; q++) nm[i++] = *q;
        nm[i] = 0;
        if (!nm[0]) { kprintf("usage: useradd <name> [uid]\n"); return 0; }
        const char *sp = arg + i;
        while (*sp == ' ') sp++;
        int uid = (*sp) ? atoi(sp) : -1;
        if (user_by_name(nm)) { kprintf("useradd: user already exists: %s\n", nm); return 0; }
        kprintf("New password for %s: ", nm);
        char p1[64];
        int n1 = cons_readline_masked(p1, sizeof(p1)); p1[n1 < 0 ? 0 : n1] = 0;
        kprintf("\n");
        if (user_add(nm, p1[0] ? p1 : "changeme", uid, NULL) == 0)
            kprintf("useradd: created %s (uid %d, home /home/%s)\n", nm,
                    user_by_name(nm)->uid, nm);
        else kprintf("useradd: failed\n");
    } else if (strcmp(cmd0, "userdel") == 0) {
        if (user_current() != 0) { kprintf("userdel: permission denied (root only)\n"); return 0; }
        if (!arg[0]) { kprintf("usage: userdel <name>\n"); return 0; }
        int r = user_del(arg);
        if (r == 0)       kprintf("userdel: removed %s\n", arg);
        else if (r == -2) kprintf("userdel: refusing to remove root\n");
        else              kprintf("userdel: no such user: %s\n", arg);
    } else if (strcmp(cmd0, "chmod") == 0) {
        char m[16];
        int i = 0;
        for (const char *q = arg; *q && *q != ' ' && i < 15; q++) m[i++] = *q;
        m[i] = 0;
        const char *pp = arg + i;
        while (*pp == ' ') pp++;
        if (!m[0] || !*pp) { kprintf("usage: chmod <octal-mode> <path>\n"); return 0; }
        char path[256]; shell_resolve(pp, path, sizeof(path));
        int ouid = 0, omode = 0;
        if (vfs_get_owner(path, &ouid, &omode) != 0) {
            kprintf("chmod: no such file: %s\n", path); return 0;
        }
        if (user_current() != 0 && ouid != user_current()) {
            kprintf("chmod: %s\n", user_denied_msg()); return 0;
        }
        int mode = 0;
        for (const char *q = m; *q; q++) {
            if (*q < '0' || *q > '7') { mode = -1; break; }
            mode = mode * 8 + (*q - '0');
        }
        if (mode < 0) { kprintf("chmod: bad mode: %s\n", m); return 0; }
        vfs_set_owner(path, -1, mode);
        kprintf("chmod: %s -> 0%o\n", path, mode);
    } else if (strcmp(cmd0, "chown") == 0) {
        if (user_current() != 0) { kprintf("chown: permission denied (root only)\n"); return 0; }
        char u[USER_NAME_MAX];
        int i = 0;
        for (const char *q = arg; *q && *q != ' ' && i < USER_NAME_MAX - 1; q++) u[i++] = *q;
        u[i] = 0;
        const char *pp = arg + i;
        while (*pp == ' ') pp++;
        if (!u[0] || !*pp) { kprintf("usage: chown <user> <path>\n"); return 0; }
        user_t *tu = user_by_name(u);
        if (!tu) { kprintf("chown: no such user: %s\n", u); return 0; }
        char path[256]; shell_resolve(pp, path, sizeof(path));
        if (vfs_set_owner(path, tu->uid, -1) != 0) {
            kprintf("chown: no such file: %s\n", path); return 0;
        }
        kprintf("chown: %s -> %s\n", path, u);
    } else if (strcmp(cmd0, "login") == 0) {
        shell_login_loop();
        tinysh_autostart_once();          /* 重新登录后回到默认 shell */
    } else if (strcmp(cmd0, "logout") == 0) {
        kprintf("logout\n");
        shell_login_loop();
        tinysh_autostart_once();          /* 重新登录后回到默认 shell */
    } else if (strcmp(cmd0, "uname") == 0) {
        kprintf("TinyOS Genesis v0.1  i386 protected mode (bare metal)\n");
    } else if (strcmp(cmd0, "ps") == 0) {
        proc_list();
    } else if (strcmp(cmd0, "mem") == 0) {
        mm_info();
    } else if (strcmp(cmd0, "uptime") == 0) {
        kprintf("uptime: %u s (ticks=%u)\n", pit_seconds(), pit_ticks());
    } else if (strcmp(cmd0, "compile") == 0 || strcmp(cmd0, "cc") == 0) {
        /* cc <source> [-o <out.TNCR>] —— 系统里自带的 MiniC 编译器。
         * CC.TNCR 是编译进 romfs 的 TNCR 程序：不必回主机用 tcc.py 交叉编译，
         * 在 TinyOS 里写完源码就能直接 `cc hello.mc -o /bin/hello.TNCR`。 */
        if (!vfs_resolve("/bin/CC.TNCR")) {
            kprintf("cc: /bin/CC.TNCR is not installed\n");
            kprintf("  host fallback: tcc <source> -o <out.TNCR> (see docs/COMPILER.md)\n");
            return 0;
        }
        if (!arg[0]) {
            kprintf("usage: cc <source.mc> [-o <out.TNCR>]\n");
            kprintf("  example: cc /home/hello.mc -o /bin/hello.TNCR\n");
            kprintf("  then:    run /bin/hello.TNCR\n");
            return 0;
        }
        run_tncr_with_arg("/bin/CC.TNCR", arg, session_id);
    } else if (strcmp(cmd0, "lua") == 0) {
        /* lua [script.lua] —— 系统自带的 Lua 5.4 解释器（LUA.TNCR）。
         * 无参数进交互 REPL；带参数则执行该脚本（路径走 VFS）。 */
        if (!vfs_resolve("/bin/LUA.TNCR")) {
            kprintf("lua: /bin/LUA.TNCR is not installed\n");
            kprintf("  rebuild with tools/lua/build_lua.py, then re-run build_users.py\n");
            return 0;
        }
        run_tncr_with_arg("/bin/LUA.TNCR", arg, session_id);
    } else {
        kprintf("unknown command: %s (type help)\n", cmd0);
    }
    return 0;
}

/* ------------------------------------------------------------------
 * tinysh 自动启动
 * Genesis v0.1 的默认 shell 就是 tinysh（一个真正的用户态 TNCR 程序）。
 * 登录后直接把它拉起来；用户在 tinysh 里敲 exit 退出后，
 * 落回内核 shell 而不是直接注销，这样切回内核命令（uname/mem/disk/net）
 * 很容易。反复登录也只在每次登录后进一次 tinysh。
 * ------------------------------------------------------------------ */
static int g_tinysh_autostart_done = 0;

static void tinysh_autostart_once(void) {
    if (g_tinysh_autostart_done) return;
    g_tinysh_autostart_done = 1;
    if (!vfs_resolve("/bin/tinysh.TNCR")) return;   /* 没装就静默跳过 */
    kprintf("\n[tinysh] starting the Genesis user shell; type `exit` to come back\n");
    run_tncr_with_arg("/bin/tinysh.TNCR", "", 0);
    kprintf("[tinysh] exited -- back to the kernel shell\n\n");
}

/* ---- 交互式 Shell 主循环：协作式轮询网络 ---- */
void shell_main(void) {
    char line[160];
    int n = 0;

    shell_login_loop();                 /* 先登录，再给提示符 */

    /* 安装时选择了"启用 SSH"的话，登录后直接把服务拉起来。
     * 只尝试一次，避免服务退出后反复重启。 */
    if (sshd_autostart()) {
        if (vfs_resolve("/bin/SSHD.TNCR") && g_net_mac[0]) {
            kprintf("[sshd] autostart enabled in /etc/sshd.conf: launching the SSH server\n");
            run_tncr_with_arg("/bin/SSHD.TNCR", "", 0);
            kprintf("[sshd] server stopped; continuing with the local shell\n");
        } else {
            kprintf("[sshd] autostart is enabled but the server or the NIC is missing\n");
        }
    }

    /* Genesis 的默认 shell 是 tinysh：登录后直接进去。
     * 用户在 tinysh 里 exit 即可落回下面的内核 shell。 */
    tinysh_autostart_once();

    for (;;) {
        net_poll_all();
        char pr[160];
        shell_prompt(pr, sizeof(pr));
        kprintf("%s", pr);
        n = 0;
        for (;;) {
            net_poll_all();
            int c = cons_getchar();
            if (c < 0) { __asm__ volatile("hlt"); continue; }
            if (c == '\r' || c == '\n') { kputc('\n'); line[n] = 0; break; }
            if (c == '\b' || c == 127) { if (n > 0) { n--; kputs("\b \b"); } continue; }
            if (c >= 32 && c < 127 && n < (int)sizeof(line) - 1) { line[n++] = (char)c; kputc((char)c); }
        }
        if (n == 0) continue;
        if (strcmp(line, "exit") == 0) {          /* 退出当前登录会话 */
            kprintf("logout\n");
            shell_login_loop();
            continue;
        }
        shell_exec(line, 0);
    }
}
