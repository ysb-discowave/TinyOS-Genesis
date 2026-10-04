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

static void tab_complete(char *buf, int *pos) {
    for (int i = 0; i < *pos; i++)
        if (buf[i] == ' ') return;
    int wl = *pos, n = 0; const cmd_t *first = NULL;
    for (int i = 0; i < cmd_count(); i++) {
        const cmd_t *c = cmd_get(i);
        if (strncmp(c->name, buf, wl) == 0) { n++; if (!first) first = c; }
    }
    if (n == 1) {
        int L = (int)strlen(first->name);
        for (int i = wl; i < L && *pos < LINE_MAX - 1; i++) buf[(*pos)++] = first->name[i];
        buf[*pos] = 0; fputs(first->name + wl, stdout); fflush(stdout);
    } else if (n > 1) {
        printf("\r\n");
        for (int i = 0; i < cmd_count(); i++) {
            const cmd_t *c = cmd_get(i);
            if (strncmp(c->name, buf, wl) == 0) printf("%s ", c->name);
        }
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

/* ===================== 入口 ===================== */
int main(void) {
    return tinysh_run();
}
