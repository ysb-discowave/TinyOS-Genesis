/* ============================================================================
 * kernel_api_host.c -- tinysh 的宿主机后端（gcc，标准 libc）
 * ----------------------------------------------------------------------------
 * 仅供离线冒烟测试：用宿主 POSIX 实现 kernel_api.h 的接口，并提供带历史/
 * 方向键/Tab 补全的交互式 ksh_readline 与 main()。产品构建用
 * kernel_api_tinyos.c，不链接本文件。
 * ============================================================================ */
#include "kernel_api.h"
#include "tinysh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>

/* 宿主后端用标准 socket 真正联网（仅离线冒烟测试用，不进产品 TNCR）。 */
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define HOST_CLOSE(s)    closesocket(s)
#  define HOST_SSIZE_T     int
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  define HOST_CLOSE(s)    close(s)
#  define HOST_SSIZE_T     ssize_t
#endif

/* uname() 是 POSIX 的，MinGW / 某些沙箱 libc 没有 sys/utsname.h。
 * 缺了就退化成只报 "host"，sysinfo 仍然可用。 */
#if defined(__unix__) || defined(__APPLE__)
#  include <sys/utsname.h>
#  define HAVE_UNAME 1
#else
#  define HAVE_UNAME 0
#endif

#ifdef _WIN32
#  include <conio.h>
#  include <windows.h>
#  include <io.h>
#  define is_tty()    _isatty(_fileno(stdin))
#  define get1()      _getch()
#else
#  include <termios.h>
#  include <unistd.h>
#  define is_tty()    isatty(0)
#  define get1()      getchar()
#endif

static fs_entry g_fs_buf[128];
static hw_dev   g_devs[32];

/* ===================== 行读取（宿主，支持历史/方向键/Tab） ===================== */
#ifdef _WIN32
static void raw_on(void) {}
static void raw_off(void) {}
#else
static struct termios g_old;
static int g_raw = 0;
static void raw_on(void) {
    if (!is_tty()) return;
    tcgetattr(0, &g_old);
    struct termios t = g_old;
    t.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &t);
    g_raw = 1;
}
static void raw_off(void) {
    if (g_raw) { tcsetattr(0, TCSANOW, &g_old); g_raw = 0; }
}
#endif

static void line_replace(char *buf, int *pos, const char *s) {
    for (int i = 0; i < *pos; i++) printf("\b \b");
    strncpy(buf, s, LINE_MAX - 1); buf[LINE_MAX - 1] = 0;
    *pos = (int)strlen(buf);
    fputs(buf, stdout); fflush(stdout);
}

/* 内核命令名（仅用于宿主端 Tab 补全；产品构建由内核自己的 readline
 * 完成补全）。这里是宿主冒烟测试的候选项，和内核 genesis_api.c 的命令
 * 名单是同一事实的两份副本——产品侧以 genesis_api.c 为准。只列 tinysh
 * 没有的内核独占命令，避免和下面内置命令补全重复打印。 */
static const char *g_kcmds[] = {
    "write", "edit", "fs", "run", "desktop", "shot", "screendump", "mouse",
    "net", "whoami", "id", "install", "setup", "wizard", "netconf",
    "part", "partitions", "disk", "sshd", "tinysh", "users", "su", "passwd",
    "useradd", "userdel", "chmod", "chown", "login", "logout",
    "uname", "mem", "uptime", "compile", "cc", "lua", 0
};

static void tab_complete(char *buf, int *pos) {
    for (int i = 0; i < *pos; i++)
        if (buf[i] == ' ') return;
    int wl = *pos, n = 0; const char *first = NULL;
    for (int i = 0; i < cmd_count(); i++) {
        const cmd_t *c = cmd_get(i);
        if (strncmp(c->name, buf, wl) == 0) { n++; if (!first) first = c->name; }
    }
    for (int i = 0; g_kcmds[i]; i++) {
        if (strncmp(g_kcmds[i], buf, wl) == 0) { n++; if (!first) first = g_kcmds[i]; }
    }
    if (n == 1) {
        int L = (int)strlen(first);
        for (int i = wl; i < L && *pos < LINE_MAX - 1; i++) buf[(*pos)++] = first[i];
        buf[*pos] = 0; fputs(first + wl, stdout); fflush(stdout);
    } else if (n > 1) {
        printf("\r\n");
        for (int i = 0; i < cmd_count(); i++) {
            const cmd_t *c = cmd_get(i);
            if (strncmp(c->name, buf, wl) == 0) printf("%s ", c->name);
        }
        for (int i = 0; g_kcmds[i]; i++)
            if (strncmp(g_kcmds[i], buf, wl) == 0) printf("%s ", g_kcmds[i]);
        printf("\r\n%s%s", TINYSH_PROMPT, buf); fflush(stdout);
    }
}

int ksh_readline(char *buf, int n) {
    if (!is_tty()) {
        if (!fgets(buf, n, stdin)) return -1;
        int len = (int)strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
        return len;
    }
    raw_on();
    int pos = 0; buf[0] = 0;
    for (;;) {
        int c = get1();
#ifdef _WIN32
        if (c == 0 || c == 224) {
            int k = get1();
            if (k == 72 || k == 65) { const char *s = hist_prev(); if (s) line_replace(buf, &pos, s); }
            else if (k == 80 || k == 66) { const char *s = hist_next(); if (s) line_replace(buf, &pos, s); }
            continue;
        }
#else
        if (c == 27) {
            if (get1() == '[') {
                int k = get1();
                if (k == 'A') { const char *s = hist_prev(); if (s) line_replace(buf, &pos, s); }
                else if (k == 'B') { const char *s = hist_next(); if (s) line_replace(buf, &pos, s); }
            }
            continue;
        }
#endif
        if (c == '\r' || c == '\n') { printf("\r\n"); break; }
        if (c == 3)  { printf("^C\r\n"); pos = 0; buf[0] = 0; continue; }
        if (c == 4)  { break; }
        if (c == '\b' || c == 127) {
            if (pos > 0) { pos--; printf("\b \b"); }
            continue;
        }
        if (c == '\t') { tab_complete(buf, &pos); continue; }
        if (c >= 32 && pos < n - 1) { buf[pos++] = (char)c; buf[pos] = 0; putchar(c); fflush(stdout); }
    }
    raw_off();
    buf[pos] = 0;
    return pos;
}

/* ===================== 系统信息 ===================== */
void sys_version(char *buf, int n) {
    snprintf(buf, n, "tinysh v0.1 (host build)");
}
void sys_sysinfo(char *buf, int n) {
#if HAVE_UNAME
    struct utsname u;
    if (uname(&u) != 0) { snprintf(buf, n, "host\n"); return; }
    snprintf(buf, n, "Host: %s %s\n", u.sysname, u.machine);
#else
    snprintf(buf, n, "Host: (uname unavailable)\n");
#endif
}
void sys_date(char *buf, int n) {
    time_t t = time(0);
    struct tm *tm = localtime(&t);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S (host)\n", tm);
}

/* ===================== 文件系统 ===================== */
int fs_list(const char *path, fs_entry **out, int *n) {
    const char *p = path && *path ? path : ".";
    DIR *d = opendir(p);
    if (!d) { *out = 0; *n = 0; return -E_NOENT; }
    int cnt = 0;
    struct dirent *e;
    while ((e = readdir(d)) && cnt < 128) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[640];
        snprintf(full, sizeof full, "%s/%s", p, e->d_name);
        struct stat st;
        int is_dir = 0; long sz = 0;
        if (stat(full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            sz = (long)st.st_size;
        }
        fs_entry *f = &g_fs_buf[cnt];
        strncpy(f->name, e->d_name, 63); f->name[63] = 0;
        f->is_dir = is_dir; f->size = sz;
        cnt++;
    }
    closedir(d);
    *out = g_fs_buf; *n = cnt;
    return 0;
}
int fs_getcwd(char *buf, int n) { return getcwd(buf, n) ? 0 : -E_IO; }
int fs_chdir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -E_NOENT;
    if (!S_ISDIR(st.st_mode)) return -E_NOENT;
    return chdir(path) == 0 ? 0 : -E_IO;
}
long fs_read(const char *path, char *buf, int n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -E_NOENT;
    long r = (long)fread(buf, 1, n, f);
    fclose(f);
    return r;
}
int fs_mkdir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return -E_IO;
#if defined(_WIN32)
    return mkdir(path) == 0 ? 0 : -E_IO;
#else
    return mkdir(path, 0755) == 0 ? 0 : -E_IO;
#endif
}
int fs_remove(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -E_NOENT;
    if (S_ISDIR(st.st_mode)) return -E_PERM;
    return remove(path) == 0 ? 0 : -E_IO;
}
int fs_touch(const char *path) {
    FILE *f = fopen(path, "a");
    if (!f) return -E_IO;
    fclose(f);
    return 0;
}
int fs_write(const char *path, const char *data, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -E_IO;
    if (n > 0 && fwrite(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -E_IO; }
    fclose(f);
    return 0;
}

/* ===================== 网络（宿主仅占位，不真正联网） ===================== */
int net_sha256_file(const char *path, char *out, int n) {
    (void)path;
    /* 宿主冒烟测试用：返回固定占位值，真实实现在内核后端 */
    if (n > 0) out[0] = 0;
    return -E_IO;   /* not available on host */
}
int net_ftp_fetch(const char *host, int port, const char *user,
                  const char *pass, const char *remote, const char *local) {
    (void)host; (void)port; (void)user; (void)pass; (void)remote; (void)local;
    fprintf(stderr, "net_ftp_fetch: not available on host build\n");
    return -E_IO;
}

/* ===================== 网络（宿主：标准 socket 真连） ===================== */
/* 跨 chunk 查找 "\r\n\r\n"（响应头结束符），state 在多次调用间保持。
 * 返回该序列之后第一个字节的下标，没找到返回 -1。 */
static int find_crlfcrlf(const char *buf, int n, int *state) {
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (*state == 0)      { if (c == '\r') *state = 1; }
        else if (*state == 1) { if (c == '\n') *state = 2; else if (c == '\r') *state = 1; else *state = 0; }
        else if (*state == 2) { if (c == '\r') *state = 3; else if (c == '\n') *state = 2; else *state = 0; }
        else /* 3 */           { if (c == '\n') return i + 1; else if (c == '\r') *state = 1; else *state = 0; }
    }
    return -1;
}

/* 真正的 HTTP/1.1 GET（Connection: close）。buf!=0 时把 body 写进 buf（最多 max，
 * 仅适用小文件如 manifest）；localfile!=0 时把 body 流写入文件（大包）。
 * 二者有且只有一个非 0。返回 0 成功。 */
static int host_http_get(const char *host, int port, const char *path,
                        char *buf, int max, int *out_len, const char *localfile) {
#ifdef _WIN32
    WSADATA wd; if (WSAStartup(MAKEWORD(2,2), &wd) != 0) return -E_IO;
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char portstr[16]; snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -E_IO;
    int s = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return -E_IO; }
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) { HOST_CLOSE(s); freeaddrinfo(res); return -E_IO; }
    freeaddrinfo(res);
#else
    struct hostent *he = gethostbyname(host);
    if (!he) return -E_IO;
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr, (size_t)he->h_length);
    int s = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -E_IO;
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) { HOST_CLOSE(s); return -E_IO; }
#endif

    char req[512];
    int rl = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: TinyOS-Genesis/0.1\r\n"
        "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (send(s, req, rl, 0) != rl) { HOST_CLOSE(s); return -E_IO; }

    FILE *f = 0;
    if (localfile) { f = fopen(localfile, "wb"); if (!f) { HOST_CLOSE(s); return -E_IO; } }

    char rbuf[8192];
    int st = 0, found = 0, body_out = 0, got_any = 0;
    for (;;) {
        HOST_SSIZE_T n = recv(s, rbuf, sizeof rbuf, 0);
        if (n <= 0) break;
        got_any = 1;
        if (!found) {
            int p = find_crlfcrlf(rbuf, (int)n, &st);
            if (p >= 0) {
                found = 1;
                int bl = (int)n - p;
                const char *body = rbuf + p;
                if (f) fwrite(body, 1, (size_t)bl, f);
                else if (buf && body_out < max) {
                    int c = bl < max - body_out ? bl : max - body_out;
                    memcpy(buf + body_out, body, (size_t)c); body_out += c;
                }
            }
        } else {
            if (f) fwrite(rbuf, 1, (size_t)n, f);
            else if (buf && body_out < max) {
                int c = (int)n < max - body_out ? (int)n : max - body_out;
                memcpy(buf + body_out, rbuf, (size_t)c); body_out += c;
            }
        }
    }
    HOST_CLOSE(s);
    if (f) { fclose(f); }
    if (out_len) *out_len = body_out;
    return got_any ? 0 : -E_IO;
}

int net_http_get(const char *host, int port, const char *path,
                 void *buf, int max, int *out_len) {
    return host_http_get(host, port, path, (char *)buf, max, out_len, 0);
}
int net_http_file(const char *host, int port, const char *path,
                  const char *local) {
    return host_http_get(host, port, path, 0, 0, 0, local);
}

/* ===================== 进程 ===================== */
void proc_list_print(void) {
    printf("PID\tNAME\tSTATUS\tMEM\n");
#ifdef _WIN32
    printf("0\ttinysh\trun\t-\n");   /* MinGW 无 getpid()，占位即可 */
#else
    printf("%d\ttinysh\trun\t-\n", (int)getpid());
#endif
}
int proc_kill(int pid) {
#ifdef _WIN32
    (void)pid;
    return -E_NOPID;
#else
    if (kill(pid, SIGTERM) == 0) return 0;
    return -E_NOPID;
#endif
}

/* ===================== 硬件（宿主仅展示，不真正访问端口） ===================== */
int hw_devlist(hw_dev **out, int *n) {
    const char *names[] = { "com1", "com2", "pic", "pit", "vga", "kbd", "ide0" };
    static const unsigned short bases[] = { 0x3F8, 0x2F8, 0x20, 0x40, 0x3D4, 0x60, 0x1F0 };
    static const unsigned short sizes[] = { 8, 8, 2, 4, 2, 1, 8 };
    int cnt = 0;
    for (int i = 0; i < 7 && cnt < 32; i++) {
        strncpy(g_devs[cnt].path, names[i], 39); g_devs[cnt].path[39] = 0;
        g_devs[cnt].io_base = bases[i];
        g_devs[cnt].io_size = sizes[i];
        cnt++;
    }
    *out = g_devs; *n = cnt;
    return 0;
}
int hw_readdev(const char *dev, unsigned addr, unsigned *val) {
    (void)dev; (void)addr; (void)val;
    return -E_IO;   /* 宿主无端口权限 */
}
int hw_writedev(const char *dev, unsigned addr, unsigned val) {
    (void)dev; (void)addr; (void)val;
    return -E_IO;
}

/* ===================== 命令查找链（宿主无内核命令表） ===================== */
int kcmd_exists(const char *name) {
    (void)name;
    return 0;   /* 宿主没有内核命令表，永远当作“不是内核命令” */
}
int kcmd_exec(const char *name, const char *args) {
    (void)name; (void)args;
    fprintf(stderr, "kcmd_exec: not available on host build\n");
    return -1;
}
int prog_exec(const char *path, const char *args) {
    (void)path; (void)args;
    fprintf(stderr, "prog_exec: not available on host build\n");
    return -1;
}

/* ===================== 入口 ===================== */
int main(void) {
    return tinysh_run();
}
