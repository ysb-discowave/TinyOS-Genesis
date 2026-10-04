/* tinysh.c — TinyOS Genesis v0.1 系统默认 Shell
 *
 * 严格按《tinysh 使用手册》实现：
 *   §1 特性：单行解析、内置命令、历史(32)、Tab 命令名补全、大小写敏感、
 *            注释(#)、v0.1 不支持管道/重定向/后台
 *   §4 全部内置命令
 *   §5 错误码
 *   §6 REPL 主循环
 *   §7 内核 API 分组（见 kernel_api_*.c）
 *
 * 编译：默认链接 kernel_api_host.c（宿主机可运行验证）；产品构建改链
 *       kernel_api_tinyos.c（见 Makefile）。
 */
#include "tinysh.h"
#include "kernel_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================== 历史记录（最多 32 条） ===================== */
static char g_hist[HISTORY_MAX][LINE_MAX];
static int  g_hist_len = 0;
static int  g_hist_cur = 0;   /* 当前游标：=g_hist_len 表示“新行” */

void hist_init(void) { g_hist_len = 0; g_hist_cur = g_hist_len; }

void hist_add(const char *line) {
    if (!line || !*line) return;
    if (g_hist_len > 0 && strcmp(g_hist[g_hist_len - 1], line) == 0) return;
    if (g_hist_len < HISTORY_MAX) {
        strncpy(g_hist[g_hist_len], line, LINE_MAX - 1);
        g_hist[g_hist_len][LINE_MAX - 1] = 0;
        g_hist_len++;
    } else {
        memmove(g_hist[0], g_hist[1], (HISTORY_MAX - 1) * LINE_MAX);
        strncpy(g_hist[HISTORY_MAX - 1], line, LINE_MAX - 1);
        g_hist[HISTORY_MAX - 1][LINE_MAX - 1] = 0;
    }
    g_hist_cur = g_hist_len;
}

static const char *hist_prev(void) {
    if (g_hist_len == 0) return NULL;
    if (g_hist_cur > 0) g_hist_cur--;
    return g_hist[g_hist_cur];
}
static const char *hist_next(void) {
    if (g_hist_cur < g_hist_len - 1) g_hist_cur++;
    else g_hist_cur = g_hist_len;
    return (g_hist_cur < g_hist_len) ? g_hist[g_hist_cur] : "";
}

/* ===================== 行读取（TTY 感知） ===================== */
#ifdef _WIN32
#  include <conio.h>
#  include <io.h>
#  include <windows.h>
static DWORD g_old_mode;
#else
#  include <termios.h>
#  include <unistd.h>
static struct termios g_old_term;
#endif
static int g_raw = 0;

static int tty_isatty(void) {
#ifdef _WIN32
    return _isatty(_fileno(stdin));
#else
    return isatty(0);
#endif
}
static void raw_on(void) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(h, &g_old_mode);
    SetConsoleMode(h, g_old_mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT));
#else
    if (isatty(0)) {
        tcgetattr(0, &g_old_term);
        struct termios t = g_old_term;
        t.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(0, TCSANOW, &t);
    }
#endif
    g_raw = 1;
}
static void raw_off(void) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    SetConsoleMode(h, g_old_mode);
#else
    if (g_raw) tcsetattr(0, TCSANOW, &g_old_term);
#endif
    g_raw = 0;
}
static int raw_getch(void) {
#ifdef _WIN32
    return _getch();
#else
    return getchar();
#endif
}

/* 用 s 替换当前已输入内容（历史翻页时调用）*/
static void line_replace(char *buf, int *pos, const char *s) {
    for (int i = 0; i < *pos; i++) printf("\b \b");
    strncpy(buf, s, LINE_MAX - 1); buf[LINE_MAX - 1] = 0;
    *pos = (int)strlen(buf);
    fputs(buf, stdout); fflush(stdout);
}

/* Tab：仅补全命令名（v0.1 不做路径补全）*/
static void tab_complete(char *buf, int *pos) {
    for (int i = 0; i < *pos; i++)
        if (buf[i] == ' ') return;   /* 已有空格 -> 不补全 */
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

/* 返回读取长度；-1 表示 EOF */
int read_line(char *buf, int n) {
    if (!tty_isatty()) {                       /* 非交互：逐行读取（便于自动化测试）*/
        if (!fgets(buf, n, stdin)) return -1;
        int len = (int)strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = 0;
        return len;
    }
    raw_on();
    int pos = 0; buf[0] = 0;
    for (;;) {
        int c = raw_getch();
        if (c == '\r' || c == '\n') { printf("\r\n"); break; }
        if (c == 3)  { printf("^C\r\n"); pos = 0; buf[0] = 0; continue; }  /* Ctrl+C */
        if (c == 4)  { break; }                                                 /* Ctrl+D */
        if (c == '\b' || c == 127) {
            if (pos > 0) { pos--; printf("\b \b"); buf[pos] = 0; }
            continue;
        }
        /* 方向键：Windows=0/224 前缀；POSIX=ESC [ */
        if (c == 0 || c == 224) {
            int k = raw_getch();
            if (k == 72 || k == 65) { const char *s = hist_prev(); if (s) line_replace(buf, &pos, s); }
            else if (k == 80 || k == 66) { const char *s = hist_next(); if (s) line_replace(buf, &pos, s); }
            continue;
        }
        if (c == 27) {
            if (raw_getch() == '[') {
                int k = raw_getch();
                if (k == 'A') { const char *s = hist_prev(); if (s) line_replace(buf, &pos, s); }
                else if (k == 'B') { const char *s = hist_next(); if (s) line_replace(buf, &pos, s); }
            }
            continue;
        }
        if (c == '\t') { tab_complete(buf, &pos); continue; }
        if (c >= 32 && pos < n - 1) { buf[pos++] = (char)c; buf[pos] = 0; putchar(c); fflush(stdout); }
    }
    raw_off();
    buf[pos] = 0;
    return pos;
}

/* ===================== 错误码 ===================== */
const char *errmsg(int code) {
    switch (code) {
        case E_OK:      return "OK";
        case E_UNKNOWN: return "unknown command";
        case E_ARGC:    return "wrong number of arguments";
        case E_NOENT:   return "file or path not found";
        case E_PERM:    return "permission denied";
        case E_INVAL:   return "invalid argument";
        case E_IO:      return "I/O error";
        case E_NOPID:   return "process PID not found";
        default:        return "error";
    }
}

/* ===================== 内置命令 ===================== */
static int g_exit = 0;

static int cmd_help(int argc, char **argv) {
    if (argc <= 1) {
        for (int i = 0; i < cmd_count(); i++) {
            const cmd_t *c = cmd_get(i);
            printf("  %-8s %s\n", c->name, c->desc);
        }
        return E_OK;
    }
    const cmd_t *c = cmd_find(argv[1]);
    if (!c) return E_UNKNOWN;
    printf("%s\n  用法: %s\n  说明: %s\n", c->name, c->usage, c->desc);
    return E_OK;
}
static int cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    char b[256]; sys_version(b, sizeof b); printf("%s\n", b); return E_OK;
}
static int cmd_sysinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    char b[256]; sys_sysinfo(b, sizeof b); printf("%s\n", b); return E_OK;
}
static int cmd_date(int argc, char **argv) {
    (void)argc; (void)argv;
    char b[256]; sys_date(b, sizeof b); printf("%s\n", b); return E_OK;
}
static int cmd_ls(int argc, char **argv) {
    char cwd[512]; fs_getcwd(cwd, sizeof cwd);
    const char *target = (argc >= 2) ? argv[1] : cwd;
    fs_entry *e; int n;
    int r = fs_list(target, &e, &n);
    if (r != 0) return r;
    for (int i = 0; i < n; i++)
        printf("%-16s %s %ld\n", e[i].name, e[i].is_dir ? "<DIR>" : "<FILE>", e[i].size);
    return E_OK;
}
static int cmd_cd(int argc, char **argv) {
    if (argc < 2) return E_ARGC;
    return fs_chdir(argv[1]);
}
static int cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char b[512]; fs_getcwd(b, sizeof b); printf("%s\n", b); return E_OK;
}
static int cmd_cat(int argc, char **argv) {
    if (argc < 2) return E_ARGC;
    char buf[8192];
    long r = fs_read(argv[1], buf, sizeof buf - 1);
    if (r < 0) return (int)(-r);
    fwrite(buf, 1, (size_t)r, stdout);
    return E_OK;
}
static int cmd_mkdir(int argc, char **argv) {
    if (argc < 2) return E_ARGC;
    return fs_mkdir(argv[1]);
}
static int cmd_rm(int argc, char **argv) {
    if (argc < 2) return E_ARGC;
    return fs_remove(argv[1]);
}
static int cmd_touch(int argc, char **argv) {
    if (argc < 2) return E_ARGC;
    return fs_touch(argv[1]);
}
static int cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    proc_list_print(); return E_OK;
}
static int cmd_kill(int argc, char **argv) {
    if (argc < 2) return E_ARGC;
    char *end; long pid = strtol(argv[1], &end, 10);
    if (*end != 0) return E_INVAL;
    return proc_kill((int)pid);
}
static int cmd_devlist(int argc, char **argv) {
    (void)argc; (void)argv;
    hw_dev *d; int n; hw_devlist(&d, &n);
    for (int i = 0; i < n; i++) printf("%s\n", d[i].path);
    return E_OK;
}
static int parse_addr(const char *s, unsigned *out) {
    char *e; unsigned long v = strtoul(s, &e, 0);   /* 0x.. 或十进制 */
    if (*e != 0) return 0;
    *out = (unsigned)v; return 1;
}
static int cmd_readdev(int argc, char **argv) {
    if (argc < 3) return E_ARGC;
    unsigned addr;
    if (!parse_addr(argv[2], &addr)) return E_INVAL;
    unsigned val = 0;
    int r = hw_readdev(argv[1], addr, &val);
    if (r != 0) return r;
    printf("0x%X = 0x%X (%u)\n", addr, val, val);
    return E_OK;
}
static int cmd_writedev(int argc, char **argv) {
    if (argc < 4) return E_ARGC;
    unsigned addr, val;
    if (!parse_addr(argv[2], &addr)) return E_INVAL;
    if (!parse_addr(argv[3], &val)) return E_INVAL;
    return hw_writedev(argv[1], addr, val);
}
static int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) { if (i > 1) putchar(' '); fputs(argv[i], stdout); }
    printf("\n");
    return E_OK;
}
static int cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("\033[2J\033[H");
    return E_OK;
}
static int cmd_exit(int argc, char **argv) {
    (void)argc; (void)argv;
    g_exit = 1; return E_OK;
}

/* ===================== 命令注册表 ===================== */
static const cmd_t g_cmds[] = {
    {"help",     cmd_help,     "help [command]",            "查看命令帮助"},
    {"version",  cmd_version,  "version",                   "查看系统版本"},
    {"sysinfo",  cmd_sysinfo,  "sysinfo",                   "系统状态概览"},
    {"date",     cmd_date,     "date",                      "获取系统时间戳"},
    {"ls",       cmd_ls,       "ls [path]",                 "列出目录内容"},
    {"cd",       cmd_cd,       "cd <target_path>",          "切换工作目录"},
    {"pwd",      cmd_pwd,      "pwd",                       "打印当前工作目录"},
    {"cat",      cmd_cat,      "cat <filepath>",            "读取并输出文本文件"},
    {"mkdir",    cmd_mkdir,    "mkdir <dir_name>",          "创建单层目录"},
    {"rm",       cmd_rm,       "rm <filepath>",             "删除普通文件"},
    {"touch",    cmd_touch,    "touch <filename>",          "创建空文件"},
    {"ps",       cmd_ps,       "ps",                        "查看进程列表"},
    {"kill",     cmd_kill,     "kill <pid>",                "终止指定进程"},
    {"devlist",  cmd_devlist,  "devlist",                   "枚举硬件设备"},
    {"readdev",  cmd_readdev,  "readdev <dev> <addr>",      "读取硬件寄存器"},
    {"writedev", cmd_writedev, "writedev <dev> <addr> <val>","写入硬件寄存器"},
    {"echo",     cmd_echo,     "echo <text>",               "文本输出"},
    {"clear",    cmd_clear,    "clear",                     "清屏"},
    {"exit",     cmd_exit,     "exit",                      "退出 tinysh 会话"},
};

const cmd_t *cmd_find(const char *name) {
    for (int i = 0; i < cmd_count(); i++)
        if (strcmp(g_cmds[i].name, name) == 0) return &g_cmds[i];
    return NULL;
}
int cmd_count(void) { return (int)(sizeof(g_cmds) / sizeof(g_cmds[0])); }
const cmd_t *cmd_get(int i) { return &g_cmds[i]; }

/* ===================== REPL 主循环（§6） ===================== */
int tinysh_run(void) {
    hist_init();
    printf("%s\n", TINYSH_BANNER);

    char line[LINE_MAX];
    for (;;) {
        fputs(TINYSH_PROMPT, stdout); fflush(stdout);
        int len = read_line(line, sizeof line);
        if (len < 0) break;                       /* EOF */

        char *start = line;
        while (*start == ' ' || *start == '\t') start++;
        if (*start == 0) continue;               /* 空行 */
        if (*start == '#') continue;             /* 注释 */

        char *endp = start + strlen(start);
        while (endp > start && (endp[-1] == ' ' || endp[-1] == '\t')) endp--;
        *endp = 0;

        char *argv[ARG_MAX]; int argc = 0;
        char *tk = strtok(start, " ");
        while (tk && argc < ARG_MAX) { argv[argc++] = tk; tk = strtok(NULL, " "); }
        if (argc == 0) continue;

        hist_add(start);

        const cmd_t *c = cmd_find(argv[0]);
        if (!c) { printf("tinysh: %s\n", errmsg(E_UNKNOWN)); continue; }

        int r = c->func(argc, argv);
        if (r != E_OK) printf("tinysh: %s\n", errmsg(r));
        if (g_exit) break;
    }
    printf("tinysh terminated\n");
    return 0;
}

/* host 构建：标准 main 入口 */
int main(void) {
    return tinysh_run();
}

/* TinyOS 2.0 用户态构建：TNCR 加载器入口。stdio（printf/fgets…）由
 * TNCR stdio shim 接管并映射到 tinyos_api_t->print/println/readline，
 * 因此 tinysh.c 的打印/输入逻辑无需改动即可在裸机上运行。 */
#ifdef TINYOS_USER
#  include "api.h"
extern void tinysh_init_api(tinyos_api_t *api);
void user_main(tinyos_api_t *api) {
    tinysh_init_api(api);
    tinysh_run();
}
#endif
