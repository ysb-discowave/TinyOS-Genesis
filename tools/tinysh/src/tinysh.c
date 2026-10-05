/* tinysh.c — TinyOS Genesis v0.1 系统默认 Shell
 *
 * 严格按《tinysh 使用手册》实现：
 *   §1 特性：单行解析、内置命令、历史(32)、Tab 命令名补全（宿主端）、
 *            大小写敏感、注释(#)、v0.1 不支持管道/重定向/后台
 *   §4 全部内置命令
 *   §5 错误码
 *   §6 REPL 主循环
 *
 * 后端无关：所有底层能力经 kernel_api.h 声明、由各后端实现
 *   （kernel_api_host.c / kernel_api_tinyos.c）。本文件不调用任何内核符号，
 *   也不直接触碰 TTY —— 行输入统一走 ksh_readline()。
 */
#include "tinysh.h"
#include "kernel_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* strtok 在裸机 shim 的 string.h 里未声明；这里补声明以消隐式声明警告。
 * 与宿主系统 string.h 的声明签名一致，重复声明无害。 */
char *strtok(char *s, const char *delim);

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
        for (int i = 0; i < HISTORY_MAX - 1; i++)
            memcpy(g_hist[i], g_hist[i + 1], LINE_MAX);
        strncpy(g_hist[HISTORY_MAX - 1], line, LINE_MAX - 1);
        g_hist[HISTORY_MAX - 1][LINE_MAX - 1] = 0;
    }
    g_hist_cur = g_hist_len;
}

const char *hist_prev(void) {
    if (g_hist_len == 0) return NULL;
    if (g_hist_cur > 0) g_hist_cur--;
    return g_hist[g_hist_cur];
}
const char *hist_next(void) {
    if (g_hist_cur < g_hist_len - 1) g_hist_cur++;
    else g_hist_cur = g_hist_len;
    return (g_hist_cur < g_hist_len) ? g_hist[g_hist_cur] : "";
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
            printf("  %-9s %s\n", c->name, c->desc);
        }
        return E_OK;
    }
    const cmd_t *c = cmd_find(argv[1]);
    if (!c) return E_UNKNOWN;
    printf("%s\n  usage: %s\n  desc : %s\n", c->name, c->usage, c->desc);
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
    for (int i = 1; i < argc; i++) { if (i > 1) printf(" "); printf("%s", argv[i]); }
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

/* ===================== 命令注册表 =====================
 * 注意：desc / usage 是会打到 VGA 文本模式（CP437 字形表）上的用户可见文案，
 * 必须纯 ASCII —— 汉字会乱码且占 3 格把整行顶出 80 列。 */
static const cmd_t g_cmds[] = {
    {"help",     cmd_help,     "help [command]",            "show help for all or one command"},
    {"version",  cmd_version,  "version",                   "show system and component versions"},
    {"sysinfo",  cmd_sysinfo,  "sysinfo",                   "show CPU, memory, uptime, processes"},
    {"date",     cmd_date,     "date",                      "show the system timestamp"},
    {"ls",       cmd_ls,       "ls [path]",                 "list directory contents"},
    {"cd",       cmd_cd,       "cd <target_path>",          "change the working directory"},
    {"pwd",      cmd_pwd,      "pwd",                       "print the working directory"},
    {"cat",      cmd_cat,      "cat <filepath>",           "print a text file"},
    {"mkdir",    cmd_mkdir,    "mkdir <dir_name>",          "create a directory"},
    {"rm",       cmd_rm,       "rm <filepath>",            "remove a regular file"},
    {"touch",    cmd_touch,    "touch <filename>",          "create an empty file"},
    {"ps",       cmd_ps,       "ps",                        "list running processes"},
    {"kill",     cmd_kill,     "kill <pid>",                "terminate a process by pid"},
    {"devlist",  cmd_devlist,  "devlist",                   "list hardware devices"},
    {"readdev",  cmd_readdev,  "readdev <dev> <addr>",      "read a hardware register"},
    {"writedev", cmd_writedev, "writedev <dev> <addr> <val>","write a hardware register"},
    {"echo",     cmd_echo,     "echo <text>",               "print text"},
    {"clear",    cmd_clear,    "clear",                     "clear the screen"},
    {"exit",     cmd_exit,     "exit",                      "leave the tinysh session"},
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
        printf("%s", TINYSH_PROMPT);
        int len = ksh_readline(line, sizeof line);
        if (len < 0) break;                       /* EOF */
        line[len < (int)sizeof line ? len : (int)sizeof line - 1] = 0;

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
