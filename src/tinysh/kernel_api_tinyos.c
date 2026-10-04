/* ============================================================================
 * kernel_api_tinyos.c -- tinysh 的 TinyOS 2.0 用户态后端（TNCR）
 * ----------------------------------------------------------------------------
 * 通过 tinyos_api_t（基座 + Genesis v0.1 扩展）实现 kernel_api.h 的全部接口。
 * 裸机 libc（printf / fwrite / malloc / 字符串 ...）由 lua_shim.c 提供，
 * 其 printf 转发到 g_lua_api->print，故本文件只需把 g_lua_api 指向内核表。
 *
 * 入口：user_main(tinyos_api_t*) 由 TNCR 加载器以 fn(&g_api) 调用。
 * ============================================================================ */
#include "kernel_api.h"
#include "api_user.h"
#include "tinysh.h"
#include <string.h>
#include <stdio.h>   /* FILE / EOF：仅为 fgetc 桩 */

/* 本后端维护自己的 cwd（与内核 g_cwd 解耦，符合手册 §6 “初始 cwd = /”）*/
static tinyos_api_t *g_api = 0;
/* ---- 供 lua_shim.c 的 printf/fwrite 使用的内核 API 表 ---- */
tinyos_api_t *g_lua_api = 0;
/* lua_shim.c 的时间相关函数需要它 */
u32 g_lua_ticks(void) { return g_api ? g_api->ticks() : 0; }

static char            g_cwd[256] = "/";

/* 静态结果缓冲（fs_list / hw_devlist 返回指针，调用方立即消费）*/
static fs_entry g_fs_buf[128];
static hw_dev   g_devs[32];

/* ------------------------------------------------------------------ */
/* 路径解析：相对 g_cwd 解析为绝对路径（处理 . 与 ..），写入 out        */
/* ------------------------------------------------------------------ */
static void resolve_path(const char *in, char *out, int n) {
    char tmp[512];
    if (!in || !*in) {
        strncpy(tmp, g_cwd, 511); tmp[511] = 0;
    } else if (in[0] == '/') {
        strncpy(tmp, in, 511); tmp[511] = 0;
    } else {
        int cl = (int)strlen(g_cwd);
        int i = 0;
        for (i = 0; i < cl && i < 510; i++) tmp[i] = g_cwd[i];
        if (cl > 0 && g_cwd[cl - 1] != '/') { if (i < 510) tmp[i++] = '/'; }
        for (int j = 0; in[j] && i < 510; j++) tmp[i++] = in[j];
        tmp[i] = 0;
    }

    /* 归一化：逐段拼回绝对路径，处理 . 与 .. */
    char norm[512];
    int  ni = 0;
    norm[ni++] = '/';
    char seg[64]; int si = 0;
    int  tl = (int)strlen(tmp);
    for (int i = 0; i <= tl; i++) {
        char c = (i < tl) ? tmp[i] : 0;
        if (c == '/' || c == 0) {
            if (si > 0) {
                seg[si] = 0;
                if (strcmp(seg, ".") == 0) {
                    si = 0;
                } else if (strcmp(seg, "..") == 0) {
                    if (ni > 1) {
                        ni--;
                        while (ni > 1 && norm[ni - 1] != '/') ni--;
                    }
                    si = 0;
                } else {
                    if (ni > 1) norm[ni++] = '/';
                    for (int k = 0; k < si && ni < 511; k++) norm[ni++] = seg[k];
                    si = 0;
                }
            }
        } else {
            if (si < 63) seg[si++] = c;
        }
    }
    if (ni == 0) { norm[0] = '/'; ni = 1; }
    norm[ni] = 0;
    strncpy(out, norm, n - 1); out[n - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* 行输入：直接走内核 readline（带行编辑/回显），EOF 返回 -1           */
/* ------------------------------------------------------------------ */
int ksh_readline(char *buf, int n) {
    if (!g_api) return -1;
    return g_api->readline(buf, n);
}

/* ------------------------------------------------------------------ */
/* 系统信息                                                            */
/* ------------------------------------------------------------------ */
void sys_version(char *buf, int n) { if (g_api) g_api->version(buf, n); }
void sys_sysinfo(char *buf, int n) { if (g_api) g_api->sysinfo(buf, n); }
void sys_date(char *buf, int n)    { if (g_api) g_api->date(buf, n); }

/* ------------------------------------------------------------------ */
/* 文件系统                                                            */
/* ------------------------------------------------------------------ */
static void fs_cb(const char *name, int is_dir, long size, void *arg) {
    int *cnt = (int *)arg;
    if (*cnt >= 128) return;
    fs_entry *e = &g_fs_buf[*cnt];
    strncpy(e->name, name, 63); e->name[63] = 0;
    e->is_dir = is_dir;
    e->size   = size;
    (*cnt)++;
}

int fs_list(const char *path, fs_entry **out, int *n) {
    if (!g_api) return -E_IO;
    char abs[512];
    resolve_path(path, abs, sizeof abs);
    int cnt = 0;
    g_api->fs_list(abs, fs_cb, &cnt);
    *out = g_fs_buf;
    *n = cnt;
    return 0;
}

int fs_getcwd(char *buf, int n) {
    strncpy(buf, g_cwd, n - 1); buf[n - 1] = 0;
    return 0;
}

int fs_chdir(const char *path) {
    if (!g_api) return -E_IO;
    char abs[512];
    resolve_path(path, abs, sizeof abs);
    int is_dir = 0; long sz = 0;
    if (g_api->stat(abs, &is_dir, &sz) != 0) return -E_NOENT;
    if (!is_dir) return -E_NOENT;            /* 不能 cd 进文件 */
    strncpy(g_cwd, abs, 255); g_cwd[255] = 0;
    return 0;
}

long fs_read(const char *path, char *buf, int n) {
    if (!g_api) return -E_IO;
    char abs[512];
    resolve_path(path, abs, sizeof abs);
    int r = g_api->file_read(abs, buf, n);
    if (r < 0) return -E_NOENT;
    return (long)r;
}

int fs_mkdir(const char *path) {
    if (!g_api) return -E_IO;
    char abs[512];
    resolve_path(path, abs, sizeof abs);
    int r = g_api->mkdir(abs);
    return (r == 0) ? 0 : -E_IO;
}

int fs_remove(const char *path) {
    if (!g_api) return -E_IO;
    char abs[512];
    resolve_path(path, abs, sizeof abs);
    int is_dir = 0, sz = 0;
    if (g_api->stat(abs, &is_dir, &sz) != 0) return -E_NOENT;
    if (is_dir) return -E_PERM;             /* 手册 §4：v0.1 不能删目录 */
    int r = g_api->rm_file(abs);
    return (r == 0) ? 0 : -E_IO;
}

int fs_touch(const char *path) {
    if (!g_api) return -E_IO;
    char abs[512];
    resolve_path(path, abs, sizeof abs);
    int r = g_api->file_write(abs, "", 0);  /* 空写 -> 创建/更新文件 */
    return (r == 0) ? 0 : -E_IO;
}

/* ------------------------------------------------------------------ */
/* 进程                                                                */
/* ------------------------------------------------------------------ */
void proc_list_print(void) { if (g_api) g_api->proc_list_all(); }
int  proc_kill(int pid) {
    if (!g_api) return -E_NOPID;
    int r = g_api->proc_kill(pid);
    return (r == 0) ? 0 : -E_NOPID;
}

/* ------------------------------------------------------------------ */
/* 硬件寄存器                                                          */
/* ------------------------------------------------------------------ */
static void dev_cb(const char *name, u16 base, u16 size, void *arg) {
    int *cnt = (int *)arg;
    if (*cnt >= 32) return;
    hw_dev *d = &g_devs[*cnt];
    strncpy(d->path, name, 39); d->path[39] = 0;
    d->io_base = base;
    d->io_size = size;
    (*cnt)++;
}

int hw_devlist(hw_dev **out, int *n) {
    if (!g_api) { *out = 0; *n = 0; return -E_IO; }
    int cnt = 0;
    g_api->dev_list_all(dev_cb, &cnt);
    *out = g_devs;
    *n = cnt;
    return 0;
}

int hw_readdev(const char *dev, unsigned addr, unsigned *val) {
    if (!g_api) return -E_IO;
    u8 b = 0;
    int r = g_api->dev_read(dev, (u32)addr, &b, 1);
    if (r < 0) return -E_IO;
    *val = b;
    return 0;
}

int hw_writedev(const char *dev, unsigned addr, unsigned val) {
    if (!g_api) return -E_IO;
    u8 b = (u8)val;
    int r = g_api->dev_write(dev, (u32)addr, &b, 1);
    return (r < 0) ? -E_IO : 0;
}

/* ------------------------------------------------------------------ */
/* 裸机 shim 缺的符号（lua_shim.c 未提供 / 原本由 Lua 的 lua_main.c 提供）*/
/* ------------------------------------------------------------------ */
/* tinysh 不用 stdio 的 FILE 流（文件读写走 tinyos_api_t 的
 * file_read / file_write），故这里只提供 getc() 转发所需的 fgetc，
 * 一律返回 EOF。 */
int fgetc(FILE *f) { (void)f; return EOF; }

char *strtok(char *s, const char *delim) {
    static char *next = 0;
    if (s) next = s;
    if (!next) return 0;
    /* 跳过前导分隔符 */
    while (*next && strchr(delim, *next)) next++;
    if (!*next) { next = 0; return 0; }
    char *start = next;
    while (*next && !strchr(delim, *next)) next++;
    if (*next) { *next = 0; next++; } else { next = 0; }
    return start;
}
void *memmove(void *d, const void *s, unsigned long n) {
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}

/* ------------------------------------------------------------------ */
/* TNCR 入口                                                          */
/* ------------------------------------------------------------------ */
void user_main(tinyos_api_t *api) {
    g_api = api;
    g_lua_api = api;
    g_cwd[0] = '/'; g_cwd[1] = 0;
    tinysh_run();
}
