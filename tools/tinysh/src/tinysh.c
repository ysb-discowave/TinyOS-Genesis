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

/* ---- .pack 多文件软件包 ----
 * 大型软件不止一个 TNCR，需要打成 .pack 归档再安装。格式（小端，未压缩）：
 *   0  "TNPACK"   6B 魔数
 *   6  u16 ver    版本（=1）
 *   8  u32 metalen元数据字节数
 *   12 u32 nfiles 文件数
 *   16 [元数据]   key=value 行，\0 结尾
 *   .. [文件表]   每项：u16 namelen, name, u32 size, data[size]
 * 元数据键：name=包名  desc=描述  install_dir=安装目录
 *           prog.<名字>=<TNCR 绝对路径>   （写进 /bin/path 供 run 用）
 *           env.<变量>=<值>               （写进 /bin/path 供 run 查环境）
 * 注：用户态没有 inflate（内核 inflate 只服务 romfs），故 .pack 存原始字节。*/
#define PACK_MAGIC   "TNPACK"
#define PACK_MAGIC_N 6
#define PACK_VER     1
#define PATH_REG     "/bin/path"   /* prog.<名>=路径 / env.<变量>=值 */

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

/* ============ 小端读写辅助（.pack 是二进制结构体） ============ */
static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned long rd32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF); }
static void wr32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)(v & 0xFF);       p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF); p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* ============ /bin/path 注册表 ============
 * 统一存键值对，两类：
 *   prog.<名字>=<TNCR 绝对路径>   程序映射，run 命令查它
 *   env.<变量名>=<值>            环境变量，pkg install .pack 时写入
 * 文件不存在视为空表。 */

/* 读整张表到 buf（调用方保证 buf 足够大），返回字节数，缺失返回 0 */
static int path_reg_read(char *buf, int n) {
    long r = fs_read(PATH_REG, buf, n - 1);
    if (r < 0) { buf[0] = 0; return 0; }
    buf[r] = 0;
    return (int)r;
}

/* 在表中查找 key 的值（用 pkg_get_kv），找到返回 0，未找到 -1 */
static int path_reg_get(const char *key, char *out, int n) {
    static char buf[16384];
    int len = path_reg_read(buf, sizeof buf);
    return pkg_get_kv(buf, len, key, out, n);
}

/* 覆盖式写回一个 key=value（若 key 已存在则替换该行，否则追加）。
 * 表不存在则新建。 */
static int path_reg_set(const char *key, const char *val) {
    static char buf[16384];
    static char nb[16384];
    int len = path_reg_read(buf, sizeof buf);
    int klen = (int)strlen(key);
    int w = 0;
    int replaced = 0;
    int i = 0;
    while (i < len) {
        int line = i;
        while (i < len && buf[i] != '\n' && buf[i] != '\r' && buf[i] != 0) i++;
        int ll = i - line;
        int s = line;
        while (s < line + ll && (buf[s] == ' ' || buf[s] == '\t')) s++;
        /* 该行是否是目标 key？ */
        if ((line + ll) - s > klen && strncmp(buf + s, key, klen) == 0 && buf[s + klen] == '=') {
            if (!replaced) {
                w += snprintf(nb + w, sizeof nb - w, "%s=%s\n", key, val);
                replaced = 1;
            }
            /* 已存在则丢弃旧行，写新行 */
        } else if (ll > 0) {
            if (w < (int)sizeof nb - 1) { memcpy(nb + w, buf + s, (size_t)ll); w += ll; }
            if (w < (int)sizeof nb - 1) nb[w++] = '\n';
        }
        while (i < len && (buf[i] == '\n' || buf[i] == '\r' || buf[i] == 0)) i++;
    }
    if (!replaced && w < (int)sizeof nb - 1)
        w += snprintf(nb + w, sizeof nb - w, "%s=%s\n", key, val);
    nb[w] = 0;
    if (fs_write(PATH_REG, nb, w) != 0) return -E_IO;
    return 0;
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

/* 首次使用时把默认配置（Worker 中转镜像源）写入 /etc/pkg.conf，
 * 这样 `pkg source` 能看到、也能被用户 `pkg source set` 覆盖。
 * 若盘上残留旧版 GitHub raw 源（raw.githubusercontent.com），则整体
 * 重写为新源，避免旧 /etc/pkg.conf 让 pkg 永远拉不到包。 */
static void pkg_conf_ensure(void) {
    static const char *def =
        "sourcedir=/home/pkgrepo\n"
        "ftp_host=10.0.2.2\n"
        "ftp_port=21\n"
        "ftp_user=anonymous\n"
        "ftp_pass=\n"
        "ftp_path=/packages/repo\n"
        "repo_host=tinyos-pkg-down.ysbdwz.dpdns.org\n"
        "repo_port=80\n"
        "repo_path=\n"
        "https_proxy_host=\n"
        "https_proxy_port=8080\n";
    static char conf[4096];
    long r = fs_read(PKG_CONF, conf, (int)sizeof(conf) - 1);
    if (r >= 0) {
        conf[r] = 0;
        if (strstr(conf, "raw.githubusercontent.com") != NULL)
            fs_write(PKG_CONF, def, (int)strlen(def));  /* 旧源 -> Worker 中转 */
        return;
    }
    fs_write(PKG_CONF, def, (int)strlen(def));
}

/* ============ .pack 解包安装（供 pkg install 调用） ============ */

/* 确保多级目录存在：逐段创建（vfs_mkdir 只建一层） */
static int pack_mkdirs(const char *dir) {
    char tmp[256];
    int n = (int)strlen(dir);
    if (n >= (int)sizeof tmp) return -E_INVAL;
    memcpy(tmp, dir, (size_t)n); tmp[n] = 0;
    for (int i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            fs_mkdir(tmp);            /* 已存在也无所谓 */
            tmp[i] = '/';
        }
    }
    fs_mkdir(tmp);
    return E_OK;
}

/* 把归档内的一个文件（name + data）写到 dest 目录下。
 * name 可含子目录（如 bin/cc），先建目录再写。 */
static int pack_extract_file(const char *destdir, const char *name,
                             const unsigned char *data, unsigned long size) {
    char full[512];
    /* name 含 '/' 时先确保子目录存在 */
    const char *slash = 0;
    for (const char *q = name; *q; q++) if (*q == '/') { slash = q; break; }
    if (slash) {
        char sub[512];
        int sublen = (int)(slash - name);
        if ((int)strlen(destdir) + sublen + 2 < (int)sizeof sub) {
            snprintf(sub, sizeof sub, "%s/%.*s", destdir, sublen, name);
            pack_mkdirs(sub);
        }
    }
    snprintf(full, sizeof full, "%s/%s", destdir, name);
    if (fs_write(full, (const char *)data, (int)size) != 0) return -E_IO;
    return E_OK;
}

/* 解包安装一个 .pack 文件。返回 E_OK 或负错误码。 */
static int pack_install(const char *packpath) {
    char *raw = 0;
    long total = pkg_read_all(packpath, &raw);
    if (total < 0) {
        printf("pack: cannot read %s\n", packpath);
        return E_IO;
    }
    if (total < 16) { free(raw); printf("pack: file too small\n"); return E_INVAL; }
    const unsigned char *b = (const unsigned char *)raw;

    /* 校验魔数与版本 */
    if (memcmp(b, PACK_MAGIC, PACK_MAGIC_N) != 0) {
        free(raw);
        printf("pack: bad magic (not a .pack file)\n");
        return E_INVAL;
    }
    unsigned ver = rd16(b + 6);
    if (ver != PACK_VER) {
        free(raw);
        printf("pack: unsupported version %u (expect %d)\n", ver, PACK_VER);
        return E_INVAL;
    }
    unsigned long metalen = rd32(b + 8);
    unsigned long nfiles   = rd32(b + 12);
    unsigned long off = 16;
    if (off + metalen > (unsigned long)total) {
        free(raw); printf("pack: truncated metadata\n"); return E_INVAL;
    }

    /* 元数据（key=value 行，\0 结尾） */
    char meta[4096];
    unsigned long mlen = metalen < sizeof(meta) - 1 ? metalen : sizeof(meta) - 1;
    memcpy(meta, b + off, (size_t)mlen);
    meta[mlen] = 0;
    off += metalen;

    char pkgname[64] = "", desc[256] = "", installdir[256] = "";
    pkg_get_kv(meta, (int)mlen, "name", pkgname, sizeof pkgname);
    pkg_get_kv(meta, (int)mlen, "desc", desc, sizeof desc);
    if (pkg_get_kv(meta, (int)mlen, "install_dir", installdir, sizeof installdir) != 0)
        strncpy(installdir, "/opt/pkg", sizeof installdir - 1);

    printf("pack: installing %s", pkgname[0] ? pkgname : "(unnamed)");
    if (desc[0]) printf(" -- %s", desc);
    printf("\n");
    printf("pack: %lu file(s) -> %s\n", nfiles, installdir);

    pack_mkdirs(installdir);

    /* 逐个文件释放 */
    unsigned long ok = 0;
    for (unsigned long i = 0; i < nfiles; i++) {
        if (off + 2 > (unsigned long)total) { printf("pack: truncated file table\n"); break; }
        unsigned nlen = rd16(b + off); off += 2;
        if (off + nlen + 4 > (unsigned long)total) { printf("pack: truncated file entry\n"); break; }
        char name[256];
        unsigned cl = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
        memcpy(name, b + off, (size_t)cl); name[cl] = 0;
        off += nlen;
        unsigned long fsz = rd32(b + off); off += 4;
        if (off + fsz > (unsigned long)total) { printf("pack: truncated file data (%s)\n", name); break; }
        if (pack_extract_file(installdir, name, b + off, fsz) == E_OK) {
            printf("  + %-28s %lu bytes\n", name, fsz);
            ok++;
        } else {
            printf("  ! %-28s write failed\n", name);
        }
        off += fsz;
    }

    /* 处理 env.<VAR>=<值> 与 prog.<名>=<路径>：写进 /bin/path 注册表 */
    int nenv = 0, nprog = 0;
    {
        /* 逐行扫描元数据（找 env. / prog. 前缀） */
        int i = 0;
        while (i < (int)mlen) {
            int line = i;
            while (i < (int)mlen && meta[i] != '\n' && meta[i] != '\r' && meta[i] != 0) i++;
            int ll = i - line;
            int s = line;
            while (s < line + ll && (meta[s] == ' ' || meta[s] == '\t')) s++;
            int eq = -1;
            for (int q = s; q < line + ll; q++) if (meta[q] == '=') { eq = q; break; }
            if (eq > s) {
                int klen = eq - s;
                char key[160], val[512];
                if (klen < (int)sizeof key) {
                    memcpy(key, meta + s, (size_t)klen); key[klen] = 0;
                    int vs = eq + 1, ve = line + ll;
                    while (vs < ve && (meta[vs] == ' ' || meta[vs] == '\t')) vs++;
                    int vl = ve - vs;
                    if (vl >= (int)sizeof val) vl = (int)sizeof val - 1;
                    memcpy(val, meta + vs, (size_t)vl); val[vl] = 0;
                    if (strncmp(key, "env.", 4) == 0) {
                        if (path_reg_set(key, val) == E_OK) { nenv++; }
                    } else if (strncmp(key, "prog.", 5) == 0) {
                        if (path_reg_set(key, val) == E_OK) { nprog++; }
                    }
                }
            }
            while (i < (int)mlen && (meta[i] == '\n' || meta[i] == '\r' || meta[i] == 0)) i++;
        }
    }
    if (nenv)  printf("pack: registered %d env var(s) in %s\n", nenv, PATH_REG);
    if (nprog) printf("pack: registered %d program mapping(s) in %s\n", nprog, PATH_REG);

    /* 登记到 /etc/packages.d/<name> 便于 pkg list/info 识别 */
    if (pkgname[0]) {
        fs_mkdir(PKG_DB_DIR);
        char recp[256]; snprintf(recp, sizeof recp, "%s/%s", PKG_DB_DIR, pkgname);
        char rec[512];
        int rl = snprintf(rec, sizeof rec, "name=%s\nkind=pack\ninstall_dir=%s\nfiles=%lu\n",
                         pkgname, installdir, ok);
        fs_write(recp, rec, rl);
    }

    free(raw);
    printf("pack: done: %lu/%lu file(s) installed.\n", ok, nfiles);
    return E_OK;
}

static void pkg_print_help(void) {
    printf("pkg: package manager (v0.1, local + FTP + HTTP source)\n");
    printf("usage:\n");
    printf("  pkg list                 list installed packages\n");
    printf("  pkg info <name>          show details of a package\n");
    printf("  pkg verify [name]        verify sha256 of package(s)\n");
    printf("  pkg install <name>       install from local source, FTP, or HTTP\n");
    printf("  pkg remove <name>        remove an installed package\n");
    printf("  pkg source [set <path>]  show / set the source dir\n");
    printf("  pkg help                 this message\n");
    printf("notes:\n");
    printf("  install looks up the package in this order:\n");
    printf("    1) local source dir (sourcedir)\n");
    printf("    2) FTP source (ftp_host/ftp_port/ftp_path)\n");
    printf("    3) HTTP source (repo_host/repo_port/repo_path)\n");
    printf("  each source serves <name>.manifest + <name>.tncr; the manifest's\n");
    printf("  sha256 is checked against the downloaded binary before install.\n");
    printf("  multi-file .pack packages:\n");
    printf("    pkg install <name>.pack    unpack a .pack archive into its\n");
    printf("                               install_dir, register env./prog. in\n");
    printf("                               /bin/path, then run <prog> to launch.\n");
    printf("    build one with: pack <srcdir> -o <name>.pack --name <name>\n");
    printf("  The default HTTP source is the tinyos-pkg-down Worker mirror, which\n");
    printf("  terminates TLS at the edge and serves plain HTTP, so the kernel\n");
    printf("  /etc/pkg.conf:\n");
    printf("    https_proxy_host=<proxy>\n");
    printf("    https_proxy_port=<port>\n");
    printf("  To pull from raw GitHub instead, set https_proxy_host / https_proxy_port;\n");
    printf("  the CONNECT tunnel still needs a kernel TLS stack (see docs).\n");
}

static int cmd_pkg(int argc, char **argv) {
    if (argc < 2 || pk_stricmp(argv[1], "help") == 0) {
        pkg_print_help();
        return E_OK;
    }
    pkg_conf_ensure();
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
        char gh[256], gport[16], gpath[256];
        char pxh[256], pxp[16];
        pkg_conf_get("https_proxy_host", pxh, sizeof pxh, "");
        pkg_conf_get("https_proxy_port", pxp, sizeof pxp, "8080");
        pkg_conf_get("sourcedir", sd, sizeof sd, PKG_DEF_SRC);
        pkg_conf_get("ftp_host", fh, sizeof fh, "10.0.2.2");
        pkg_conf_get("ftp_port", fp, sizeof fp, "21");
        pkg_conf_get("ftp_user", fu, sizeof fu, "anonymous");
        pkg_conf_get("ftp_pass", fpass, sizeof fpass, "");
        pkg_conf_get("ftp_path", fpath, sizeof fpath, "/packages/repo");
        pkg_conf_get("repo_host", gh, sizeof gh, "tinyos-pkg-down.ysbdwz.dpdns.org");
        pkg_conf_get("repo_port", gport, sizeof gport, "80");
        pkg_conf_get("repo_path", gpath, sizeof gpath,
                     "");
        printf("pkg: source dir : %s\n", sd);
        printf("pkg: ftp source : %s:%s user=%s pass=%s path=%s\n",
               fh, fp, fu, fpass, fpath);
        printf("pkg: http source: %s:%s path=%s\n", gh, gport, gpath);
        if (pxh[0])
            printf("pkg: https proxy: %s:%s (CONNECT tunnel, for raw GitHub)\n", pxh, pxp);
        else
            printf("pkg: https proxy: (unset) -- using plain HTTP Worker mirror\n");
        return E_OK;
    }

    if (pk_stricmp(sub, "install") == 0) {
        if (argc < 3) {
            printf("usage: pkg install <name>\n");
            return E_ARGC;
        }
        const char *name = argv[2];
        /* .pack 多文件包：本地源目录里存在同名 .pack 就解包安装 */
        {
            int nl = (int)strlen(name);
            if (nl > 5 && pk_stricmp(name + nl - 5, ".pack") == 0) {
                char sd0[256];
                pkg_conf_get("sourcedir", sd0, sizeof sd0, PKG_DEF_SRC);
                char pp[512];
                snprintf(pp, sizeof pp, "%s/%s", sd0, name);
                char probe;
                if (fs_read(pp, &probe, 1) >= 0) {
                    printf("pack: found local package %s\n", pp);
                    return pack_install(pp);
                }
                /* 也允许直接给路径 */
                if (strchr(name, '/') && fs_read(name, &probe, 1) >= 0)
                    return pack_install(name);
                printf("pack: %s not found in source dir %s\n", name, sd0);
                return E_NOENT;
            }
        }
        char sd[256], fh[64], fp[16], fu[64], fpass[64], fpath[256];
        char gh[256], gport[16], gpath[256];
        char pxh[256], pxp[16];
        pkg_conf_get("https_proxy_host", pxh, sizeof pxh, "");
        pkg_conf_get("https_proxy_port", pxp, sizeof pxp, "8080");
        pkg_conf_get("sourcedir", sd, sizeof sd, PKG_DEF_SRC);
        pkg_conf_get("ftp_host", fh, sizeof fh, "10.0.2.2");
        pkg_conf_get("ftp_port", fp, sizeof fp, "21");
        pkg_conf_get("ftp_user", fu, sizeof fu, "anonymous");
        pkg_conf_get("ftp_pass", fpass, sizeof fpass, "");
        pkg_conf_get("ftp_path", fpath, sizeof fpath, "/packages/repo");
        pkg_conf_get("repo_host", gh, sizeof gh, "tinyos-pkg-down.ysbdwz.dpdns.org");
        pkg_conf_get("repo_port", gport, sizeof gport, "80");
        pkg_conf_get("repo_path", gpath, sizeof gpath,
                     "");

        char mpath[512];
        char bpath[512];
        int from_net = 0;   /* 0=local, 1=ftp, 2=github */

        /* 1) 本地源目录 */
        snprintf(mpath, sizeof mpath, "%s/%s.manifest", sd, name);
        static char md[8192];
        long mlen = fs_read(mpath, md, (int)sizeof md - 1);
        if (mlen < 0) {
            /* 2) FTP 源 */
            fs_mkdir(PKG_TMP_DIR);
            char rmt_man[512], rmt_tnc[512];
            int port = atoi(fp);
            snprintf(rmt_man, sizeof rmt_man, "%s/%s.manifest", fpath, name);
            snprintf(rmt_tnc, sizeof rmt_tnc, "%s/%s.tncr", fpath, name);
            snprintf(mpath, sizeof mpath, "%s/pkg_%s.manifest", PKG_TMP_DIR, name);
            snprintf(bpath, sizeof bpath, "%s/pkg_%s.tncr", PKG_TMP_DIR, name);
            printf("pkg: trying FTP source %s:%d ...\n", fh, port);
            if (net_ftp_fetch(fh, port, fu, fpass, rmt_man, mpath) == 0 &&
                (mlen = fs_read(mpath, md, (int)sizeof md - 1)) >= 0 &&
                net_ftp_fetch(fh, port, fu, fpass, rmt_tnc, bpath) == 0) {
                from_net = 1;
            } else {
                if (mlen >= 0) fs_remove(mpath);
                /* 3) GitHub raw 源：免手动克隆，新增的优先直连源 */
                char mbuf[4096]; int mlen2 = 0;
                char gman[512], gbin[512];
                snprintf(gman, sizeof gman, "%s/%s.manifest", gpath, name);
                printf("pkg: trying HTTP source %s:%s ...\n", gh, gport);
                                if (pxh[0]) {
                    /* 走 HTTP CONNECT 代理（真实 GitHub / 进阶用法）。
                     * 隧道之后需 TLS 栈，内核尚未实现，会返回 E_NOTLS。 */
                    int tr = net_http_tunnel(pxh, atoi(pxp), gh, atoi(gport));
                    if (tr == -E_NOTLS) {
                        printf("pkg: proxy %s:%s accepted CONNECT, but the kernel\n", pxh, pxp);
                        printf("     has no TLS stack yet, so the encrypted stream\n");
                        printf("     cannot be read. Not pretending otherwise.\n");
                    } else if (tr != 0) {
                        printf("pkg: proxy %s:%s refused CONNECT (code %d)\n", pxh, pxp, tr);
                    }
                }

                /* 默认：明文 HTTP 直连中转 Worker（无需 TLS 栈）。 */
                printf("pkg: trying HTTP source %s:%s ...\n", gh, gport);
                snprintf(bpath, sizeof bpath, "%s/pkg_%s.tncr", PKG_TMP_DIR, name);
                snprintf(gbin, sizeof gbin, "%s/%s.tncr", gpath, name);
                if (net_http_get(gh, atoi(gport), gman, mbuf, (int)sizeof mbuf, &mlen2) == 0
                    && mlen2 > 0 && mlen2 < (int)sizeof md) {
                    memcpy(md, mbuf, (size_t)mlen2); mlen = mlen2;
                    if (net_http_file(gh, atoi(gport), gbin, bpath) == 0) {
                        from_net = 2;
                    } else {
                        printf("pkg: failed to fetch binary via HTTP: %s\n", name);
                    }
                } else {
                    printf("pkg: HTTP source %s:%s unreachable (check repo_host/repo_port).\n", gh, gport);
                }
            }
        }
        if (mlen < 0) return E_NOENT;   /* 三种源都没拿到 manifest */

        if (from_net == 0) {
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
            if (from_net) { fs_remove(mpath); fs_remove(bpath); }
            return E_IO;
        }
        if (exp[0]) {
            if (pk_stricmp(act, exp) != 0) {
                printf("pkg: sha256 mismatch: %s\n", name);
                printf("  expected %s\n  actual   %s\n", exp, act);
                if (from_net) { fs_remove(mpath); fs_remove(bpath); }
                return E_IO;
            }
        } else {
            printf("pkg: warning: no sha256 in manifest, skipping verification\n");
        }

        /* 读二进制内容并写入 /bin */
        char *bd = 0; long bsz = pkg_read_all(bpath, &bd);
        if (bsz < 0) {
            printf("pkg: failed to read package binary: %s\n", bpath);
            if (from_net) { fs_remove(mpath); fs_remove(bpath); }
            return E_IO;
        }
        char binp[256]; snprintf(binp, sizeof binp, "%s/%s.TNCR", PKG_BIN_DIR, name);
        if (fs_write(binp, bd, (int)bsz) != 0) {
            printf("pkg: failed to write %s (permission?)\n", binp);
            free(bd);
            if (from_net) { fs_remove(mpath); fs_remove(bpath); }
            return E_PERM;
        }
        free(bd);

        /* 登记到 /etc/packages.d/<name> */
        fs_mkdir(PKG_DB_DIR);
        char recp[256]; snprintf(recp, sizeof recp, "%s/%s", PKG_DB_DIR, name);
        if (fs_write(recp, md, (int)mlen) != 0)
            printf("pkg: warning: installed but failed to write record %s\n", recp);

        if (from_net) { fs_remove(mpath); fs_remove(bpath); }

        char ver[32]; ver[0] = 0;
        pkg_get_kv(md, (int)mlen, "version", ver, sizeof ver);
        printf("pkg: installed %s (version %s, %ld bytes) -> %s\n",
               name, ver[0] ? ver : "?", bsz, binp);
        return E_OK;
    }

    printf("pkg: unknown subcommand: %s (try 'pkg help')\n", sub);
    return E_OK;
}

/* 前向声明：cmd_run 用到后面才定义的辅助函数 */
static void join_args(int argc, char **argv, char *buf, int n);
static int  pkg_prog_exists(const char *name);

/* 按 fs_list 报告的实际大小精确分配并读取一个文件。
 * 不用 pkg_read_all（它固定 malloc 1MB/文件，多文件大包会撑爆用户态 arena）。
 * 读不满 size 也接受（以实际返回字节为准）。失败返回 NULL。 */
static void *pack_read_sized(const char *path, long hint, long *out_n) {
    long cap = hint > 0 ? hint : 0;
    if (cap <= 0) cap = 4096;                 /* 未知大小给个起步值 */
    unsigned char *buf = (unsigned char *)malloc((unsigned long)cap + 1);
    if (!buf) return 0;
    long n = fs_read(path, (char *)buf, (int)cap);
    if (n < 0) {
        /* hint 不准（可能偏小）：翻倍重试一次 */
        if (cap < (1 << 22)) {
            unsigned char *nb = (unsigned char *)malloc((unsigned long)cap * 2 + 1);
            if (nb) {
                long n2 = fs_read(path, (char *)nb, (int)(cap * 2));
                if (n2 >= 0) { *out_n = n2; return nb; }
                free(nb);
            }
        }
        free(buf);
        return 0;
    }
    *out_n = n;
    return buf;
}

/* ===================== pack —— 把一个目录打成 .pack 软件包 =====================
 * 用法：
 *   pack <srcdir> [-o out.pack] [--name NAME] [--dir DIR] [--env K=V]...
 *           [--prog NAME=PATH]...
 * 把 srcdir 下的所有文件（递归，含子目录）打包成一个 .pack。
 * 元数据里写入 name / install_dir / env.* / prog.*，安装器据此还原。
 * 注：用户态无 inflate，故归档不压缩，存原始字节。 */
static int cmd_pack(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: pack <srcdir> [-o out.pack] [--name NAME] [--dir DIR]\n");
        printf("           [--env K=V]... [--prog NAME=PATH]...\n");
        printf("example:\n");
        printf("  pack /home/myapp -o myapp.pack --name myapp --dir /opt/myapp \\\n");
        printf("       --prog myapp=/opt/myapp/bin/myapp.TNCR --env PATH=/opt/myapp/bin\n");
        return E_ARGC;
    }
    const char *srcdir = argv[1];
    const char *outname = 0;
    char pkgname[64] = "", installdir[256] = "";
    /* 先扫参数 */
    char metas[2048]; int mw = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outname = argv[++i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            snprintf(pkgname, sizeof pkgname, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            snprintf(installdir, sizeof installdir, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--env") == 0 && i + 1 < argc) {
            mw += snprintf(metas + mw, sizeof metas - mw, "env.%s\n", argv[++i]);
        } else if (strcmp(argv[i], "--prog") == 0 && i + 1 < argc) {
            mw += snprintf(metas + mw, sizeof metas - mw, "prog.%s\n", argv[++i]);
        }
    }
    if (!pkgname[0]) {
        /* 默认包名 = 源目录最后一段 */
        const char *base = srcdir;
        for (const char *q = srcdir; *q; q++) if (*q == '/') base = q + 1;
        snprintf(pkgname, sizeof pkgname, "%s", base);
    }
    if (!installdir[0])
        snprintf(installdir, sizeof installdir, "/opt/%s", pkgname);
    if (!outname) {
        static char ob[256];
        snprintf(ob, sizeof ob, "/tmp/%s.pack", pkgname);
        outname = ob;
    }

    /* 先收集文件到内存（名字+内容）。小工具，够用即可。 */
    typedef struct { char name[256]; unsigned char *data; unsigned long size; } pfile;
    static pfile files[64];
    int nfiles = 0;

    /* 递归遍历：vfs 提供 fs_list，但为简单起见只支持一层 + 显式子目录。
     * 这里手动处理一层子目录以覆盖 "bin/" "lib/" 常见布局。 */
    fs_entry *ents = 0; int nent = 0;
    if (fs_list(srcdir, &ents, &nent) != 0) {
        printf("pack: cannot list %s\n", srcdir);
        return E_NOENT;
    }
    for (int i = 0; i < nent && nfiles < 64; i++) {
        /* 关键：fs_list 返回的是后端共享的静态缓冲，内层再调 fs_list 会覆盖它。
         * 所以先把本层用到的名字/类型拷出来，再进子目录。 */
        char ename[64];
        snprintf(ename, sizeof ename, "%s", ents[i].name);
        int edir = ents[i].is_dir;
        char full[512];
        snprintf(full, sizeof full, "%s/%s", srcdir, ename);
        if (edir) {
            /* 一层子目录：把里面的文件以 子/文件 形式收入 */
            fs_entry *sub = 0; int nsub = 0;
            if (fs_list(full, &sub, &nsub) != 0) continue;
            for (int j = 0; j < nsub && nfiles < 64; j++) {
                if (sub[j].is_dir) continue;         /* 只收一层 */
                char sname[64];
                snprintf(sname, sizeof sname, "%s", sub[j].name);  /* 同理先拷贝 */
                char sfull[512];
                snprintf(sfull, sizeof sfull, "%s/%s", full, sname);
                long sz = 0;
                unsigned char *d = (unsigned char *)pack_read_sized(sfull, sub[j].size, &sz);
                if (!d) continue;
                snprintf(files[nfiles].name, sizeof files[nfiles].name, "%s/%s", ename, sname);
                files[nfiles].data = d;
                files[nfiles].size = (unsigned long)sz;
                nfiles++;
            }
        } else {
            long sz = 0;
            unsigned char *d = (unsigned char *)pack_read_sized(full, ents[i].size, &sz);
            if (!d) continue;
            snprintf(files[nfiles].name, sizeof files[nfiles].name, "%s", ename);
            files[nfiles].data = d;
            files[nfiles].size = (unsigned long)sz;
            nfiles++;
        }
    }
    if (nfiles == 0) {
        printf("pack: no files found in %s\n", srcdir);
        return E_NOENT;
    }

    /* 组装元数据区：name / install_dir + 用户给的 env./prog. */
    char meta[2048]; int w = 0;
    w += snprintf(meta + w, sizeof meta - w, "name=%s\n", pkgname);
    w += snprintf(meta + w, sizeof meta - w, "install_dir=%s\n", installdir);
    if (mw > 0 && mw < (int)sizeof meta - w) { memcpy(meta + w, metas, (size_t)mw); w += mw; }
    meta[w] = 0;
    unsigned long metalen = (unsigned long)w;

    /* 计算总大小并分配输出缓冲 */
    unsigned long total = 16 + metalen;
    for (int i = 0; i < nfiles; i++) {
        total += 2 + strlen(files[i].name) + 4 + files[i].size;
    }
    unsigned char *ob = (unsigned char *)malloc((unsigned long)total);
    if (!ob) { printf("pack: out of memory\n"); return E_IO; }

    unsigned long o = 0;
    memcpy(ob + o, PACK_MAGIC, PACK_MAGIC_N); o += PACK_MAGIC_N;
    wr16(ob + o, PACK_VER); o += 2;
    wr32(ob + o, metalen); o += 4;
    wr32(ob + o, (unsigned long)nfiles); o += 4;
    memcpy(ob + o, meta, (size_t)metalen); o += metalen;
    for (int i = 0; i < nfiles; i++) {
        unsigned nl = (unsigned)strlen(files[i].name);
        wr16(ob + o, nl); o += 2;
        memcpy(ob + o, files[i].name, nl); o += nl;
        wr32(ob + o, files[i].size); o += 4;
        memcpy(ob + o, files[i].data, (size_t)files[i].size); o += files[i].size;
    }

    if (fs_write(outname, (const char *)ob, (int)o) != 0) {
        printf("pack: failed to write %s\n", outname);
        return E_IO;
    }
    printf("pack: wrote %s (%d file(s), %lu bytes) -> %s\n", outname, nfiles, o, outname);
    printf("pack: install with: pkg install %s.pack\n", pkgname);
    printf("pack: (place %s in the source dir, default %s)\n", outname, PKG_DEF_SRC);

    free(ob);
    for (int i = 0; i < nfiles; i++) free(files[i].data);
    return E_OK;
}

/* ===================== run —— 查 /bin/path 执行已注册程序 =====================
 * 用法：run <name> [args...]
 * 在 /bin/path 里找 prog.<name>=<TNCR路径>，找到就用 prog_exec 执行。
 * 找不到时回退到 /bin/<name>.TNCR（pkg 单文件安装的程序）。 */
static int cmd_run(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: run <name> [args...]\n");
        printf("looks up prog.<name> in %s and executes the mapped TNCR.\n", PATH_REG);
        return E_ARGC;
    }
    const char *name = argv[1];
    char key[160];
    snprintf(key, sizeof key, "prog.%s", name);
    char path[512]; path[0] = 0;
    if (path_reg_get(key, path, sizeof path) == 0 && path[0]) {
        char args[LINE_MAX];
        join_args(argc, argv, args, sizeof args);
        prog_exec(path, args);
        return E_OK;
    }
    /* 回退：/bin/<name>.TNCR */
    if (pkg_prog_exists(name)) {
        char p2[512];
        snprintf(p2, sizeof p2, "%s/%s.TNCR", PKG_BIN_DIR, name);
        char args[LINE_MAX];
        join_args(argc, argv, args, sizeof args);
        prog_exec(p2, args);
        return E_OK;
    }
    printf("run: no program '%s' in %s (and no /bin/%s.TNCR)\n", name, PATH_REG, name);
    return E_NOENT;
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
    {"pack",     cmd_pack,     "pack <dir> [opts]",         "bundle a directory into a .pack multi-file package"},
    {"run",      cmd_run,      "run <name> [args]",         "run a program registered in /bin/path"},
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
