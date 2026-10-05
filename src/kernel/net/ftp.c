#include "ftp.h"
#include "net.h"
#include "vfs.h"
#include "console.h"
#include "mm.h"
#include "pit.h"
#include "libc.h"

#define FTP_MAX_XFER (128u * 1024u)   /* 单次传输上限（VFS 为内存文件系统） */

/* 置 1 可把每条 FTP 命令/阶段打印到控制台（排错用），默认关闭避免刷屏 */
#define FTP_DEBUG 0
#if FTP_DEBUG
#define FTP_LOG(...) kprintf(__VA_ARGS__)
#else
#define FTP_LOG(...) ((void)0)
#endif

static int startswith(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static const char *skip_sp(const char *s) { while (*s == ' ') s++; return s; }

/* 发送全部数据后等待对端确认（保证 FIN 之前的字节已送达） */
static void flush_send(tcp_conn_t *c, u32 timeout_ms) {
    for (u32 t = 0; t < timeout_ms; t++) {
        net_poll();
        if (tcp_sent_all_acked(c)) return;
        pit_sleep(1);
    }
}

/* 从数据连接读取直到对端关闭（或超时），返回字节数并写入 out 缓冲 */
static u32 recv_until_eof(tcp_conn_t *d, u8 *out, u32 cap, u32 timeout_ms) {
    u32 total = 0, waited = 0;
    for (;;) {
        int n = tcp_recv(d, out + total, cap - total);
        if (n > 0) { total += (u32)n; waited = 0; if (total >= cap) break; continue; }
        if (tcp_closed_by_peer(d)) { net_poll(); int n2 = tcp_recv(d, out + total, cap - total); if (n2 > 0) total += (u32)n2; break; }
        net_poll(); pit_sleep(1);
        if (++waited >= timeout_ms) break;
    }
    return total;
}

/* ============================================================
 * 客户端：被动模式（PASV）
 * ============================================================ */
static int pasv_open(u32 ip, tcp_conn_t *ctl, char *line, u32 lsz, tcp_conn_t **out) {
    tcp_writeline(ctl, "PASV");
    if (tcp_readline(ctl, line, lsz, 3000) < 0) return -1;
    if (line[0] != '2') return -1;
    /* 解析 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2) */
    const char *p = strchr(line, '(');
    if (!p) return -1;
    p++;
    u32 v[6] = {0,0,0,0,0,0};
    int i = 0;
    while (*p && *p != ')' && i < 6) {
        if (*p >= '0' && *p <= '9') v[i] = v[i] * 10 + (u32)(*p - '0');
        else if (*p == ',') i++;
        p++;
    }
    u32 dip = (v[0]<<24)|(v[1]<<16)|(v[2]<<8)|v[3];
    if (dip == 0) dip = ip;
    u16 dport = (u16)((v[4] << 8) | v[5]);
    tcp_conn_t *d = tcp_connect(dip, dport);
    if (!d || tcp_wait_established(d, 4000) != 0) { tcp_close(d); return -1; }
    *out = d;
    return 0;
}

int ftp_get(const char *host, int port, const char *user, const char *pass,
            const char *remote, const char *local) {
    if (port <= 0) port = 21;
    u32 ip = ip_parse(host);
    char line[256];
    tcp_conn_t *c = tcp_connect(ip, (u16)port);
    if (!c) return -1;
    if (tcp_wait_established(c, 5000) != 0) { tcp_close(c); return -1; }
    if (tcp_readline(c, line, sizeof(line), 5000) < 0) { tcp_close(c); return -1; }

    char b[96];
    snprintf(b, sizeof(b), "USER %s", (user && *user) ? user : "anonymous");
    tcp_writeline(c, b); tcp_readline(c, line, sizeof(line), 5000);
    snprintf(b, sizeof(b), "PASS %s", (pass && *pass) ? pass : "anonymous");
    tcp_writeline(c, b); tcp_readline(c, line, sizeof(line), 5000);
    if (line[0] != '2') { tcp_close(c); return -1; }

    tcp_writeline(c, "TYPE I");
    tcp_readline(c, line, sizeof(line), 3000);
    tcp_writeline(c, "PWD");
    tcp_readline(c, line, sizeof(line), 3000);

    tcp_conn_t *d = NULL;
    if (pasv_open(ip, c, line, sizeof(line), &d) != 0) { tcp_close(c); return -1; }

    snprintf(b, sizeof(b), "RETR %s", remote);
    tcp_writeline(c, b);
    if (tcp_readline(c, line, sizeof(line), 5000) < 0 || line[0] != '1') {
        tcp_close(d); tcp_close(c); return -1;
    }

    u8 *buf = (u8*)kmalloc(FTP_MAX_XFER);
    if (!buf) { tcp_close(d); tcp_close(c); return -1; }
    u32 got = recv_until_eof(d, buf, FTP_MAX_XFER, 4000);
    int rc = (got > 0) ? vfs_write_file(local, buf, got) : -1;
    kfree(buf);
    tcp_close(d);
    tcp_readline(c, line, sizeof(line), 3000);   /* 226 */
    tcp_writeline(c, "QUIT");
    tcp_close(c);
    return (rc == 0) ? 0 : -1;
}

int ftp_put(const char *host, int port, const char *user, const char *pass,
            const char *local, const char *remote) {
    if (port <= 0) port = 21;
    u32 sz = 0;
    const u8 *data = vfs_read_file(local, &sz);
    if (!data) return -1;
    if (sz > FTP_MAX_XFER) return -1;
    u32 ip = ip_parse(host);
    char line[256], b[96];

    tcp_conn_t *c = tcp_connect(ip, (u16)port);
    if (!c) return -1;
    if (tcp_wait_established(c, 5000) != 0) { tcp_close(c); return -1; }
    if (tcp_readline(c, line, sizeof(line), 5000) < 0) { tcp_close(c); return -1; }

    snprintf(b, sizeof(b), "USER %s", (user && *user) ? user : "anonymous");
    tcp_writeline(c, b); tcp_readline(c, line, sizeof(line), 5000);
    snprintf(b, sizeof(b), "PASS %s", (pass && *pass) ? pass : "anonymous");
    tcp_writeline(c, b); tcp_readline(c, line, sizeof(line), 5000);
    if (line[0] != '2') { tcp_close(c); return -1; }

    tcp_writeline(c, "TYPE I");
    tcp_readline(c, line, sizeof(line), 3000);

    tcp_conn_t *d = NULL;
    if (pasv_open(ip, c, line, sizeof(line), &d) != 0) { tcp_close(c); return -1; }

    snprintf(b, sizeof(b), "STOR %s", remote);
    tcp_writeline(c, b);
    if (tcp_readline(c, line, sizeof(line), 5000) < 0 || line[0] != '1') {
        tcp_close(d); tcp_close(c); return -1;
    }
    tcp_send(d, data, sz);
    flush_send(d, 4000);
    tcp_close(d);                                /* 发 FIN 通知对端 EOF */
    tcp_readline(c, line, sizeof(line), 4000);   /* 226 */
    tcp_writeline(c, "QUIT");
    tcp_close(c);
    return 0;
}

/* ============================================================
 * 服务端
 * ============================================================ */
static tcp_conn_t *g_ctl_listener = NULL;

/* PASV 数据端口固定为 21000：QEMU SLIRP 需要为每个端口单独配 hostfwd，
 * 固定单端口让启动脚本只需转发一条规则（21000->21000）。
 * 通告地址写 127.0.0.1：SLIRP 下客户端只能来自宿主回环（经 hostfwd），
 * 通告客户机内网 IP（10.0.2.15）宿主永远路由不到，会导致数据连接超时。 */
#define FTP_PASV_PORT 21000

/* LIST 的遍历上下文（C 无闭包，用文件级变量传递） */
static tcp_conn_t *g_list_d = NULL;
static int g_list_count = 0;
static void list_cb(const char *name, vfs_type t, void *arg) {
    (void)arg;
    char line[128];
    u32 sz = 0;
    if (t == VFS_FILE) vfs_read_file(name, &sz);
    snprintf(line, sizeof(line), "%s %8u  %s%s\r\n",
             (t == VFS_DIR) ? "drwxr-xr-x" : "-rw-r--r--", sz, name, (t == VFS_DIR) ? "/" : "");
    tcp_writestr(g_list_d, line);
    g_list_count++;
}

/* 被动模式监听器：PASV/EPSV 只登记，不阻塞；真正传数据的命令（LIST/RETR/STOR…）
   到来时才 accept。这样客户端在 PASV 之后夹带 TYPE/PWD 等普通命令也不会被打乱，
   Windows 资源管理器的行为就落在这个区间里。 */
static tcp_conn_t *g_pasv_listener = NULL;

static int is_data_cmd(const char *line) {
    return startswith(line, "LIST") || startswith(line, "NLST") ||
           startswith(line, "RETR") || startswith(line, "STOR") ||
           startswith(line, "APPE");
}
static void close_pasv(void) {
    if (!g_pasv_listener) return;
    tcp_close(g_pasv_listener);
    g_pasv_listener = NULL;
    tcp_drop_pending(FTP_PASV_PORT);   /* 顺带回收该端口上没人认领的数据子连接 */
}

/* 执行一条数据连接命令：先取本会话的 PASV 监听器，等数据连接，再传数据。 */
static void run_data_cmd(tcp_conn_t *c, const char *line) {
    char b[160];
    tcp_conn_t *dl = g_pasv_listener;
    if (!dl) { tcp_writeline(c, "425 Use PASV or EPSV first"); return; }
    g_pasv_listener = NULL;                 /* 本命令消费掉这个监听器 */
    u16 dport = FTP_PASV_PORT;

    tcp_conn_t *d = tcp_accept_wait(dl, 5000);
    tcp_close(dl);
    FTP_LOG("[ftp] data accept: %s\n", d ? "ok" : "TIMEOUT");
    if (!d) { tcp_drop_pending(dport); tcp_writeline(c, "425 Can't open data connection"); return; }

    u32 sz = 0;
    if (startswith(line, "RETR")) {
        const char *f = skip_sp(line + 4);
        const u8 *data = vfs_read_file(f, &sz);
        if (!data) { tcp_close(d); tcp_drop_pending(dport); tcp_writeline(c, "550 File not found"); return; }
        snprintf(b, sizeof(b), "150 Opening BINARY mode data connection (%u bytes)", sz);
        tcp_writeline(c, b);
        tcp_send(d, data, sz); flush_send(d, 5000); tcp_close(d);
        tcp_writeline(c, "226 Transfer complete");
    } else if (startswith(line, "STOR") || startswith(line, "APPE")) {
        const char *f = skip_sp(line + 4);
        tcp_writeline(c, "150 OK to send data");
        u8 *buf = (u8*)kmalloc(FTP_MAX_XFER);
        if (buf) {
            u32 n = recv_until_eof(d, buf, FTP_MAX_XFER, 5000);
            vfs_write_file(f, buf, n);
            kfree(buf);
        }
        tcp_close(d);
        tcp_writeline(c, "226 Transfer complete");
    } else if (startswith(line, "LIST") || startswith(line, "NLST")) {
        tcp_writeline(c, "150 Here comes the directory listing");
        g_list_d = d; g_list_count = 0;
        vfs_list("/", list_cb, NULL);
        char tail[64];
        snprintf(tail, sizeof(tail), "total %d\r\n", g_list_count);
        tcp_writestr(d, tail);
        flush_send(d, 3000);
        tcp_close(d);
        tcp_writeline(c, "226 Directory send OK");
        FTP_LOG("[ftp] LIST done, %d entries\n", g_list_count);
    } else {
        tcp_writeline(c, "500 Unknown data command");
        tcp_close(d);
    }
    tcp_drop_pending(dport);        /* 回收该端口上多余/残留的数据子连接 */
}

static void handle_ctl(tcp_conn_t *c) {
    char line[256], b[160];
    FTP_LOG("[ftp] control session open\n");
    tcp_writeline(c, "220 TinyOS FTP Server ready");
    for (;;) {
        if (tcp_readline(c, line, sizeof(line), 30000) < 0) { FTP_LOG("[ftp] control read timeout/close\n"); break; }
        if (!line[0]) continue;
        FTP_LOG("[ftp] cmd: %s\n", line);

        if (startswith(line, "USER"))       tcp_writeline(c, "331 Password required");
        else if (startswith(line, "PASS"))  tcp_writeline(c, "230 Login successful");
        else if (startswith(line, "SYST"))  tcp_writeline(c, "215 UNIX Type: L8");
        else if (startswith(line, "FEAT")) { tcp_writeline(c, "211-Features:");
                                             tcp_writeline(c, " SIZE");
                                             tcp_writeline(c, "211 End"); }
        else if (startswith(line, "OPTS"))  tcp_writeline(c, "200 OK");
        else if (startswith(line, "TYPE"))  tcp_writeline(c, "200 Type set");
        else if (startswith(line, "NOOP"))  tcp_writeline(c, "200 OK");
        else if (startswith(line, "PWD") || startswith(line, "XPWD"))
                                            tcp_writeline(c, "257 \"/\" is current directory");
        else if (startswith(line, "CWD"))   tcp_writeline(c, "250 OK");
        else if (startswith(line, "PASV")) {
            u16 dp = FTP_PASV_PORT;
            u16 pl = (u16)(dp & 0xFF), ph = (u16)(dp >> 8);
            close_pasv();
            g_pasv_listener = tcp_listen(dp);
            if (!g_pasv_listener) { tcp_writeline(c, "425 Can't open data connection"); continue; }
            snprintf(b, sizeof(b), "227 Entering Passive Mode (127,0,0,1,%u,%u)", ph, pl);
            tcp_writeline(c, b);
        }
        else if (startswith(line, "EPSV")) {
            /* 扩展被动模式，端口固定；回复格式 229 (|||port|) */
            u16 dp = FTP_PASV_PORT;
            close_pasv();
            g_pasv_listener = tcp_listen(dp);
            if (!g_pasv_listener) { tcp_writeline(c, "425 Can't open data connection"); continue; }
            snprintf(b, sizeof(b), "229 Entering Extended Passive Mode (|||%u|)", dp);
            tcp_writeline(c, b);
        }
        else if (startswith(line, "PORT") || startswith(line, "EPRT")) {
            /* 主动模式需要 guest 回连宿主，QEMU 用户态网络下不可用 */
            tcp_writeline(c, "502 Active mode not supported, use PASV/EPSV");
        }
        else if (is_data_cmd(line)) run_data_cmd(c, line);
        else if (startswith(line, "SIZE")) {
            u32 sz = 0;
            vfs_read_file(skip_sp(line + 4), &sz);
            snprintf(b, sizeof(b), "213 %u", sz);
            tcp_writeline(c, b);
        }
        else if (startswith(line, "DELE")) {
            if (vfs_delete(skip_sp(line + 4)) == 0) tcp_writeline(c, "250 Deleted");
            else tcp_writeline(c, "550 Delete failed");
        }
        else if (startswith(line, "MKD")) {
            const char *f = skip_sp(line + 3);
            if (!*f) { tcp_writeline(c, "501 Missing directory name"); }
            else if (vfs_mkdir(f) == 0) { snprintf(b, sizeof(b), "257 \"%s\" created", f); tcp_writeline(c, b); }
            else tcp_writeline(c, "550 Create directory failed");
        }
        else if (startswith(line, "RMD")) {
            if (vfs_delete(skip_sp(line + 3)) == 0) tcp_writeline(c, "250 Directory removed");
            else tcp_writeline(c, "550 Remove directory failed");
        }
        else if (startswith(line, "ABOR")) { close_pasv(); tcp_writeline(c, "226 No transfer to abort"); }
        else if (startswith(line, "QUIT")) { tcp_writeline(c, "221 Bye"); break; }
        else tcp_writeline(c, "502 Command not implemented");
    }
    close_pasv();          /* 控制会话结束，释放可能残留的数据监听器 */
}

int ftp_server_start(int port) {
    if (g_ctl_listener) return 0;
    g_ctl_listener = tcp_listen((u16)port);
    return g_ctl_listener ? 0 : -1;
}
int ftp_server_active(void) { return g_ctl_listener != NULL; }
void ftp_server_poll(void) {
    if (!g_ctl_listener) return;
    tcp_conn_t *c = tcp_accept(g_ctl_listener);
    if (c) { handle_ctl(c); tcp_close(c); }
}
