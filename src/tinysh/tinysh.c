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
        printf("command lookup chain:\n");
        printf("  1) tinysh builtin (above)  2) kernel command  3) /bin/<name>.TNCR\n");
        printf("  e.g. `uname`, `net info`, `disk`, `useradd` reach the kernel.\n");
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

/* ===================== pkg —— 软件包管理（用户态，Genesis v0.1） =====================
 * pkg 是用户态工具：内核只提供基础能力（fs_read/fs_write/sha256_file/ftp_fetch），
 * 其余逻辑都在这里。所有路径走 kernel_api.h，绝不碰内核符号。 */
#define PKG_BIN_DIR  "/bin"
#define PKG_DB_DIR   "/etc/packages.d"
#define PKG_CONF     "/etc/pkg.conf"
#define PKG_DEF_SRC  "/home/pkgrepo"
#define PKG_TMP_DIR  "/tmp"

static int pk_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (ca < cb) ? -1 : 1;
        a++; b++;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

/* 从 key=value 文本取 key 的值（容忍前导空白） */
static int pkg_get_kv(const char *buf, int len, const char *key, char *out, int n) {
    out[0] = 0;
    int klen = (int)strlen(key);
    int i = 0;
    while (i < len) {
        int line = i;
        while (i < len && buf[i] != '\n' && buf[i] != '\r' && buf[i] != 0) i++;
        int ll = i - line;
        int s = line;
        while (s < line + ll && (buf[s] == ' ' || buf[s] == '\t')) s++;
        int avail = (line + ll) - s;
        if (avail > klen && strncmp(buf + s, key, klen) == 0 && buf[s + klen] == '=') {
            int v = s + klen + 1;
            while (v < line + ll && (buf[v] == ' ' || buf[v] == '\t')) v++;
            int ve = line + ll;
            while (ve > v && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t')) ve--;
            int vl = ve - v;
            if (vl >= n) vl = n - 1;
            memcpy(out, buf + v, vl); out[vl] = 0;
            return 0;
        }
        while (i < len && (buf[i] == '\n' || buf[i] == '\r' || buf[i] == 0)) i++;
    }
    return -1;
}

/* 读整个文件到 malloc 缓冲，返回字节数（<0 表示错误） */
static long pkg_read_all(const char *path, char **out) {
    long cap = 1 << 20;   /* 包上限约 1MB，足够教育用途 */
    char *buf = (char*)malloc((unsigned long)cap);
    if (!buf) return -E_IO;
    long n = fs_read(path, buf, (int)cap);
    if (n < 0) { free(buf); return n; }
    *out = buf;
    return n;
}

/* 读 /etc/pkg.conf 的一个配置项（缺省值见上方宏） */
static void pkg_conf_get(const char *key, char *out, int n, const char *def) {
    static char conf[4096];
    long r = fs_read(PKG_CONF, conf, (int)sizeof(conf) - 1);
    if (r < 0) { strncpy(out, def, n - 1); out[n - 1] = 0; return; }
    conf[r] = 0;
    if (pkg_get_kv(conf, (int)r, key, out, n) != 0) {
        strncpy(out, def, n - 1); out[n - 1] = 0;
    }
}

static void pkg_print_help(void) {
    printf("pkg: package manager (v0.1, local + FTP source)\n");
    printf("usage:\n");
    printf("  pkg list                 list installed packages\n");
    printf("  pkg info <name>          show details of a package\n");
    printf("  pkg verify [name]        verify sha256 of package(s)\n");
    printf("  pkg install <name>       install from local source dir or FTP\n");
    printf("  pkg remove <name>        remove an installed package\n");
    printf("  pkg source [set <path>]  show / set the source dir\n");
    printf("  pkg help                 this message\n");
    printf("notes:\n");
    printf("  install reads <name>.manifest + <name>.tncr from the source dir,\n");
    printf("  checks sha256, then writes /bin/<name>.TNCR and registers it.\n");
    printf("  if the local source is missing, it falls back to the FTP source.\n");
}

static int cmd_pkg(int argc, char **argv) {
    if (argc < 2 || pk_stricmp(argv[1], "help") == 0) {
        pkg_print_help();
        return E_OK;
    }
    const char *sub = argv[1];

    if (pk_stricmp(sub, "list") == 0) {
        fs_entry *e; int n;
        if (fs_list(PKG_BIN_DIR, &e, &n) != 0) {
            printf("pkg: cannot list %s\n", PKG_BIN_DIR); return E_IO;
        }
        printf("%-16s %-8s %10s\n", "NAME", "VERSION", "SIZE");
        int cnt = 0;
        for (int i = 0; i < n; i++) {
            int nl = (int)strlen(e[i].name);
            if (e[i].is_dir || nl < 5) continue;
            if (pk_stricmp(e[i].name + nl - 5, ".TNCR") != 0) continue;
            char name[64]; int k = 0;
            for (int j = 0; j < nl - 5 && k < 63; j++) name[k++] = e[i].name[j];
            name[k] = 0;
            cnt++;
            char rec[256]; snprintf(rec, sizeof rec, "%s/%s", PKG_DB_DIR, name);
            char ver[32]; strcpy(ver, "?");
            static char rb[4096];
            long rr = fs_read(rec, rb, (int)sizeof rb - 1);
            if (rr >= 0) { rb[rr] = 0; pkg_get_kv(rb, (int)rr, "version", ver, sizeof ver); }
            printf("%-16s %-8s %10ld\n", name, ver, e[i].size);
        }
        if (!cnt) printf("(no packages installed)\n");
        return E_OK;
    }

    if (pk_stricmp(sub, "info") == 0) {
        if (argc < 3) { printf("usage: pkg info <name>\n"); return E_ARGC; }
        const char *name = argv[2];
        char binp[256], recp[256];
        snprintf(binp, sizeof binp, "%s/%s.TNCR", PKG_BIN_DIR, name);
        snprintf(recp, sizeof recp, "%s/%s", PKG_DB_DIR, name);
        char *bd = 0; long bsz = pkg_read_all(binp, &bd);
        static char rb[4096]; long rrsz = fs_read(recp, rb, (int)sizeof rb - 1);
        if (bsz < 0 && rrsz < 0) {
            printf("pkg: package not found: %s\n", name); return E_NOENT;
        }
        printf("package: %s\n", name);
        if (rrsz >= 0) {
            rb[rrsz] = 0;
            char v[32], src[128], sh[65], desc[128];
            v[0] = src[0] = sh[0] = desc[0] = 0;
            pkg_get_kv(rb, (int)rrsz, "version", v, sizeof v);
            pkg_get_kv(rb, (int)rrsz, "source", src, sizeof src);
            pkg_get_kv(rb, (int)rrsz, "sha256", sh, sizeof sh);
            pkg_get_kv(rb, (int)rrsz, "desc", desc, sizeof desc);
            printf("  version : %s\n", v[0] ? v : "?");
            printf("  source  : %s\n", src[0] ? src : "(unregistered)");
            printf("  sha256  : %s\n", sh[0] ? sh : "(none)");
            if (desc[0]) printf("  desc    : %s\n", desc);
        } else {
            printf("  version : ?\n");
            printf("  source  : (unregistered: only raw /bin file present)\n");
        }
        if (bsz >= 0) {
            printf("  size    : %ld bytes\n", bsz);
            char act[65];
            if (net_sha256_file(binp, act, sizeof act) == 0) {
                printf("  sha256  : %s\n", act);
                static char rb2[4096];
                long rr2 = fs_read(recp, rb2, (int)sizeof rb2 - 1);
                char exp[65]; exp[0] = 0;
                if (rr2 >= 0) { rb2[rr2] = 0; pkg_get_kv(rb2, (int)rr2, "sha256", exp, sizeof exp); }
                if (exp[0]) printf("  verify  : %s\n",
                                   pk_stricmp(act, exp) == 0 ? "OK" : "MISMATCH");
                else printf("  verify  : no recorded sha256\n");
            } else {
                printf("  sha256  : (unavailable)\n");
            }
            free(bd);
        } else {
            printf("  note    : binary %s.TNCR is missing; only metadata present\n", name);
        }
        return E_OK;
    }

    if (pk_stricmp(sub, "verify") == 0) {
        if (argc >= 3) {
            const char *name = argv[2];
            char binp[256]; snprintf(binp, sizeof binp, "%s/%s.TNCR", PKG_BIN_DIR, name);
            char act[65];
            if (net_sha256_file(binp, act, sizeof act) != 0) {
                printf("pkg: cannot read: %s\n", name); return E_NOENT;
            }
            char recp[256]; snprintf(recp, sizeof recp, "%s/%s", PKG_DB_DIR, name);
            static char rb[4096]; long rr = fs_read(recp, rb, (int)sizeof rb - 1);
            char exp[65]; exp[0] = 0;
            if (rr >= 0) { rb[rr] = 0; pkg_get_kv(rb, (int)rr, "sha256", exp, sizeof exp); }
            if (!exp[0]) printf("%-16s no sha256 recorded (actual %s)\n", name, act);
            else if (pk_stricmp(act, exp) == 0) printf("%-16s OK\n", name);
            else printf("%-16s MISMATCH (expected %s)\n", name, exp);
            return E_OK;
        }
        fs_entry *e; int n;
        if (fs_list(PKG_BIN_DIR, &e, &n) != 0) return E_IO;
        printf("%-16s %s\n", "NAME", "RESULT");
        int cnt = 0;
        for (int i = 0; i < n; i++) {
            int nl = (int)strlen(e[i].name);
            if (e[i].is_dir || nl < 5) continue;
            if (pk_stricmp(e[i].name + nl - 5, ".TNCR") != 0) continue;
            char name[64]; int k = 0;
            for (int j = 0; j < nl - 5 && k < 63; j++) name[k++] = e[i].name[j];
            name[k] = 0;
            char binp[256]; snprintf(binp, sizeof binp, "%s/%s.TNCR", PKG_BIN_DIR, name);
            char act[65];
            if (net_sha256_file(binp, act, sizeof act) != 0) continue;
            char recp[256]; snprintf(recp, sizeof recp, "%s/%s", PKG_DB_DIR, name);
            static char rb[4096]; long rr = fs_read(recp, rb, (int)sizeof rb - 1);
            char exp[65]; exp[0] = 0;
            if (rr >= 0) { rb[rr] = 0; pkg_get_kv(rb, (int)rr, "sha256", exp, sizeof exp); }
            cnt++;
            if (!exp[0]) printf("%-16s no sha256 recorded (actual %s)\n", name, act);
            else if (pk_stricmp(act, exp) == 0) printf("%-16s OK\n", name);
            else printf("%-16s MISMATCH (expected %s)\n", name, exp);
        }
        if (!cnt) printf("(no packages to verify)\n");
        return E_OK;
    }

    if (pk_stricmp(sub, "remove") == 0) {
        if (argc < 3) { printf("usage: pkg remove <name>\n"); return E_ARGC; }
        const char *name = argv[2];
        char binp[256], recp[256];
        snprintf(binp, sizeof binp, "%s/%s.TNCR", PKG_BIN_DIR, name);
        snprintf(recp, sizeof recp, "%s/%s", PKG_DB_DIR, name);
        int did = 0;
        if (fs_remove(binp) == 0) did = 1;
        if (fs_remove(recp) == 0) did = 1;
        printf(did ? "pkg: removed %s\n" : "pkg: nothing removed\n", name);
        return E_OK;
    }

    if (pk_stricmp(sub, "source") == 0) {
        const char *r = (argc >= 3) ? argv[2] : "";
        if (pk_stricmp(r, "set") == 0) {
            if (argc < 4) { printf("usage: pkg source set <path>\n"); return E_ARGC; }
            const char *path = argv[3];
            static char conf[4096];
            long cr = fs_read(PKG_CONF, conf, (int)sizeof conf - 1);
            static char newc[4096]; int w = 0;
            if (cr >= 0) {
                conf[cr] = 0;
                int i = 0;
                while (i < cr) {
                    int line = i;
                    while (i < cr && conf[i] != '\n' && conf[i] != '\r' && conf[i] != 0) i++;
                    int ll = i - line;
                    int s = line;
                    while (s < line + ll && (conf[s] == ' ' || conf[s] == '\t')) s++;
                    int is_src = ((line + ll - s) > 9) &&
                                 strncmp(conf + s, "sourcedir", 9) == 0 && conf[s + 9] == '=';
                    if (!is_src) {
                        memcpy(newc + w, conf + line, ll); w += ll;
                        if (i < cr && conf[i] != 0) newc[w++] = '\n';
                    }
                    while (i < cr && (conf[i] == '\n' || conf[i] == '\r' || conf[i] == 0)) i++;
                }
            }
            int pl = (int)strlen(path);
            if (w > 0 && newc[w - 1] != '\n') newc[w++] = '\n';
            if (w + pl + 13 < (int)sizeof newc)
                w += snprintf(newc + w, sizeof newc - w, "sourcedir=%s\n", path);
            if (fs_write(PKG_CONF, newc, w) != 0) {
                printf("pkg: failed to write %s\n", PKG_CONF); return E_IO;
            }
            printf("pkg: source set to %s\n", path);
            return E_OK;
        }
        char sd[256], fh[64], fp[16], fu[64], fpass[64], fpath[256];
        pkg_conf_get("sourcedir", sd, sizeof sd, PKG_DEF_SRC);
        pkg_conf_get("ftp_host", fh, sizeof fh, "10.0.2.2");
        pkg_conf_get("ftp_port", fp, sizeof fp, "21");
        pkg_conf_get("ftp_user", fu, sizeof fu, "anonymous");
        pkg_conf_get("ftp_pass", fpass, sizeof fpass, "");
        pkg_conf_get("ftp_path", fpath, sizeof fpath, "/packages/repo");
        printf("pkg: source dir : %s\n", sd);
        printf("pkg: ftp source : %s:%s user=%s pass=%s path=%s\n",
               fh, fp, fu, fpass, fpath);
        return E_OK;
    }

    if (pk_stricmp(sub, "install") == 0) {
        if (argc < 3) {
            printf("usage: pkg install <name>\n");
            return E_ARGC;
        }
        const char *name = argv[2];
        char sd[256], fh[64], fp[16], fu[64], fpass[64], fpath[256];
        pkg_conf_get("sourcedir", sd, sizeof sd, PKG_DEF_SRC);
        pkg_conf_get("ftp_host", fh, sizeof fh, "10.0.2.2");
        pkg_conf_get("ftp_port", fp, sizeof fp, "21");
        pkg_conf_get("ftp_user", fu, sizeof fu, "anonymous");
        pkg_conf_get("ftp_pass", fpass, sizeof fpass, "");
        pkg_conf_get("ftp_path", fpath, sizeof fpath, "/packages/repo");

        char mpath[512];
        char bpath[512];
        int from_ftp = 0;

        /* 先查本地源目录 */
        snprintf(mpath, sizeof mpath, "%s/%s.manifest", sd, name);
        static char md[8192];
        long mlen = fs_read(mpath, md, (int)sizeof md - 1);
        if (mlen < 0) {
            /* 本地没有 -> 试 FTP 源 */
            fs_mkdir(PKG_TMP_DIR);
            char rmt_man[512], rmt_tnc[512];
            int port = atoi(fp);
            snprintf(rmt_man, sizeof rmt_man, "%s/%s.manifest", fpath, name);
            snprintf(rmt_tnc, sizeof rmt_tnc, "%s/%s.tncr", fpath, name);
            snprintf(mpath, sizeof mpath, "%s/pkg_%s.manifest", PKG_TMP_DIR, name);
            snprintf(bpath, sizeof bpath, "%s/pkg_%s.tncr", PKG_TMP_DIR, name);
            printf("pkg: trying FTP source %s:%d ...\n", fh, port);
            if (net_ftp_fetch(fh, port, fu, fpass, rmt_man, mpath) != 0) {
                printf("pkg: manifest not found locally and FTP fetch failed: %s\n", name);
                return E_NOENT;
            }
            mlen = fs_read(mpath, md, (int)sizeof md - 1);
            if (mlen < 0) { printf("pkg: failed to read fetched manifest\n"); return E_IO; }
            if (net_ftp_fetch(fh, port, fu, fpass, rmt_tnc, bpath) != 0) {
                printf("pkg: failed to fetch package binary via FTP: %s\n", name);
                fs_remove(mpath);
                return E_IO;
            }
            from_ftp = 1;
        } else {
            /* 本地源二进制：优先小写 .tncr，回退大写 .TNCR */
            snprintf(bpath, sizeof bpath, "%s/%s.tncr", sd, name);
            static char probe[4];
            if (fs_read(bpath, probe, 1) < 0)
                snprintf(bpath, sizeof bpath, "%s/%s.TNCR", sd, name);
        }
        md[mlen] = 0;

        /* sha256 校验 */
        char exp[65]; exp[0] = 0;
        pkg_get_kv(md, (int)mlen, "sha256", exp, sizeof exp);
        char act[65];
        if (net_sha256_file(bpath, act, sizeof act) != 0) {
            printf("pkg: cannot read package binary: %s\n", bpath);
            if (from_ftp) { fs_remove(mpath); fs_remove(bpath); }
            return E_IO;
        }
        if (exp[0]) {
            if (pk_stricmp(act, exp) != 0) {
                printf("pkg: sha256 mismatch: %s\n", name);
                printf("  expected %s\n  actual   %s\n", exp, act);
                if (from_ftp) { fs_remove(mpath); fs_remove(bpath); }
                return E_IO;
            }
        } else {
            printf("pkg: warning: no sha256 in manifest, skipping verification\n");
        }

        /* 读二进制内容并写入 /bin */
        char *bd = 0; long bsz = pkg_read_all(bpath, &bd);
        if (bsz < 0) {
            printf("pkg: failed to read package binary: %s\n", bpath);
            if (from_ftp) { fs_remove(mpath); fs_remove(bpath); }
            return E_IO;
        }
        char binp[256]; snprintf(binp, sizeof binp, "%s/%s.TNCR", PKG_BIN_DIR, name);
        if (fs_write(binp, bd, (int)bsz) != 0) {
            printf("pkg: failed to write %s (permission?)\n", binp);
            free(bd);
            if (from_ftp) { fs_remove(mpath); fs_remove(bpath); }
            return E_PERM;
        }
        free(bd);

        /* 登记到 /etc/packages.d/<name> */
        fs_mkdir(PKG_DB_DIR);
        char recp[256]; snprintf(recp, sizeof recp, "%s/%s", PKG_DB_DIR, name);
        if (fs_write(recp, md, (int)mlen) != 0)
            printf("pkg: warning: installed but failed to write record %s\n", recp);

        if (from_ftp) { fs_remove(mpath); fs_remove(bpath); }

        char ver[32]; ver[0] = 0;
        pkg_get_kv(md, (int)mlen, "version", ver, sizeof ver);
        printf("pkg: installed %s (version %s, %ld bytes) -> %s\n",
               name, ver[0] ? ver : "?", bsz, binp);
        return E_OK;
    }

    printf("pkg: unknown subcommand: %s (try 'pkg help')\n", sub);
    return E_OK;
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
    {"pkg",      cmd_pkg,      "pkg <subcommand>",          "package manager (list/info/verify/install/remove/source)"},
};

const cmd_t *cmd_find(const char *name) {
    for (int i = 0; i < cmd_count(); i++)
        if (strcmp(g_cmds[i].name, name) == 0) return &g_cmds[i];
    return NULL;
}
int cmd_count(void) { return (int)(sizeof(g_cmds) / sizeof(g_cmds[0])); }
const cmd_t *cmd_get(int i) { return &g_cmds[i]; }

/* 把 argv[1..] 用空格拼回一个字符串（跳过 argv[0] 命令名），
 * 供转发给内核命令 / 外部 TNCR 程序时使用。 */
static void join_args(int argc, char **argv, char *buf, int n) {
    int w = 0;
    buf[0] = 0;
    for (int i = 1; i < argc && w < n - 1; i++) {
        if (i > 1) { if (w < n - 1) buf[w++] = ' '; }
        const char *s = argv[i];
        while (*s && w < n - 1) buf[w++] = *s++;
    }
    buf[w] = 0;
}

/* 检查 /bin/<name>.TNCR 是否安装（pkg 软件）。
 * 用 fs_read 探 1 字节：文件缺失返回 -E_NOENT，存在（含空文件）返回 >=0。 */
static int pkg_prog_exists(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.TNCR", PKG_BIN_DIR, name);
    char probe;
    long r = fs_read(path, &probe, 1);
    return r >= 0;
}

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
        if (c) {
            /* 第 1 级：tinysh 内置命令（优先，遮蔽同名内核命令） */
            int r = c->func(argc, argv);
            if (r != E_OK) printf("tinysh: %s\n", errmsg(r));
            if (g_exit) break;
            continue;
        }
        /* 第 2 级：内核命令（如 uname / net / disk / useradd ...） */
        if (kcmd_exists(argv[0])) {
            char args[LINE_MAX];
            join_args(argc, argv, args, sizeof args);
            kcmd_exec(argv[0], args);
            continue;
        }
        /* 第 3 级：pkg 安装的软件 /bin/<name>.TNCR */
        if (pkg_prog_exists(argv[0])) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s.TNCR", PKG_BIN_DIR, argv[0]);
            char args[LINE_MAX];
            join_args(argc, argv, args, sizeof args);
            prog_exec(path, args);
            continue;
        }
        printf("tinysh: unknown command: %s\n", argv[0]);
    }
    printf("tinysh terminated\n");
    return 0;
}
