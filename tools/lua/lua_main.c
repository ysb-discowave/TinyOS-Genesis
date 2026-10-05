/* ============================================================================
 * lua_main.c -- Lua 5.4 在 TinyOS 里的入口（TNCR 的 user_main）
 *
 * 两种用法：
 *   run /bin/LUA.TNCR             交互 REPL（":quit" 退出）
 *   run /bin/LUA.TNCR /home/x.lua 执行脚本后退出
 *
 * 文件访问：Lua 标准 io/os.file 经 luai_fopen（本文件定义的宏）打开
 * VFS 里的文件，数据经 api->file_read 拷进 Lua 堆；close 时释放。
 * ============================================================================ */
#include "api_user.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

tinyos_api_t *g_lua_api = 0;
static lua_State *g_L = 0;

u32 g_lua_ticks(void) { return g_lua_api ? g_lua_api->ticks() : 0; }

/* ---------------- VFS 文件 <-> C FILE ---------------- */
#define V_OPEN_MAX 32

typedef struct {
    char *data;
    u32   pos, len;
    int   owner;      /* 1 = 由 vopen 在 Lua 堆里分配，close 时 free */
} vFILE;

static const char *g_open_path[V_OPEN_MAX];

static FILE *vopen(const char *path) {
    char buf[512];
    int n = g_lua_api->file_read(path, buf, sizeof buf - 1);
    if (n <= 0) return 0;
    buf[n] = 0;
    FILE *f = (FILE*)g_lua_api->ticks(); /* 占位防优化；下面真实分配 */
    f = 0;
    u32 cap = (u32)n + 64;
    FILE *q = (FILE*)malloc(cap);
    if (!q) return 0;
    memset(q, 0, cap);
    char *data = (char*)malloc((u32)n + 1);
    if (!data) { free(q); return 0; }
    memcpy(data, buf, n);
    data[n] = 0;
    q->data = data;
    q->pos = 0;
    q->len = (u32)n;
    q->owner = 1;
    /* 记录路径（close 时不需要，仅便于调试） */
    for (int i = 0; i < V_OPEN_MAX; i++) {
        if (g_open_path[i] == 0) { g_open_path[i] = path; break; }
    }
    (void)f;
    return q;
}

FILE *fopen(const char *path, const char *mode) { (void)mode; return vopen(path); }
int feof(FILE *f) { return f ? (f->pos >= f->len) : 1; }
int fgetc(FILE *f) {
    if (!f || f->pos >= f->len) return -1;
    return (unsigned char)f->data[f->pos++];
}
int fputc(int c, FILE *f) { (void)f; (void)c; return -1; }
int ungetc(int c, FILE *f) {
    /* liolib 真正需要"放回一个字符"：read_number 的 lookahead 字符、
       test_eof 的 getc 后回退。EOF 时无操作。 */
    if (c == -1 || !f || f->pos == 0) return -1;
    f->pos--;
    return c;
}
long fread(void *dst, u32 sz, u32 n, FILE *f) {
    if (!f) return 0;
    u32 want = sz * n;
    u32 avail = f->len - f->pos;
    u32 take = want < avail ? want : avail;
    memcpy(dst, f->data + f->pos, take);
    f->pos += take;
    return take / sz;
}
int fclose(FILE *f) {
    if (!f) return -1;
    if (f->owner) { if (f->data) free(f->data); free(f); }
    return 0;
}
int ferror(FILE *f) { (void)f; return 0; }
long ftell(FILE *f) { return f ? (long)f->pos : -1; }
int fseek(FILE *f, long off, int whence) {
    if (!f) return -1;
    u32 base = (whence == 0) ? 0 : (whence == 1) ? f->pos : f->len;
    long p = (long)base + off;
    if (p < 0 || p > (long)f->len) return -1;
    f->pos = (u32)p;
    return 0;
}
char *fgets(char *s, int n, FILE *f) {
    if (!s || !f || n <= 0) return 0;
    int i = 0;
    while (i < n - 1 && f->pos < f->len) {
        char c = f->data[f->pos++];
        s[i++] = c;
        if (c == '\n') break;
    }
    s[i] = 0;
    return i ? s : 0;
}
void clearerr(FILE *f) { (void)f; }
int fflush(FILE *f) { (void)f; return 0; }

#define luai_fopen vopen
#define luai_fclose fclose

/* ---------------- 输出 ---------------- */
static void print_all(const char *s) {
    if (g_lua_api && s) g_lua_api->print(s);
}

/* ---------------- 行续接检测（REPL） ---------------- */
/* 未闭合的括号/字符串/块 使行需要续接 */
static int line_needs_cont(const char *s) {
    int paren = 0, bracket = 0, brace = 0;
    int in_str = 0, in_long = 0, in_line_c = 0, in_block_c = 0;
    int long_l = 0, long_r = 0;
    const char *p = s;
    while (*p) {
        char c = *p, nx = p[1];
        if (in_line_c) { if (c == '\n') in_line_c = 0; p++; continue; }
        if (in_block_c) {
            if (c == '*' && nx == '[') { in_block_c--; p += 2; continue; }
            p++; continue;
        }
        if (in_str) {
            if (c == '\\') { p += 2; continue; }
            if (c == '"') in_str = 0;
            p++; continue;
        }
        if (c == '-' && nx == '-') {
            if (p[2] == '[') {
                /* 长注释 / 长字符串 */
                const char *q = p + 3;
                int lvl = 0;
                while (*q == '=') { lvl++; q++; }
                if (*q == '[') {
                    long_l = lvl + 1;
                    in_long = 1;
                    p = q + 1;
                    continue;
                }
            }
            in_line_c = 1; p += 2; continue;
        }
        if (in_long) {
            /* 结束标记 ]=lvl[ */
            if (c == ']') {
                const char *q = p + 1;
                int lvl = 0;
                while (*q == '=') { lvl++; q++; }
                if (*q == '[' && lvl + 1 == long_l) { in_long = 0; p = q + 1; continue; }
            }
            p++; continue;
        }
        switch (c) {
        case '(': paren++; break;
        case ')': if (paren > 0) paren--; break;
        case '{': brace++; break;
        case '}': if (brace > 0) brace--; break;
        case '[': bracket++; break;
        case ']': if (bracket > 0) bracket--; break;
        case '"': in_str = 1; break;
        case ';': /* 语句结束标记 */ paren = brace = bracket = 0; break;
        default: break;
        }
        p++;
    }
    return paren || brace || in_str || in_long || (in_block_c > 0);
}

/* ---------------- 执行一段 Lua ---------------- */
static int run_lua(const char *code) {
    if (luaL_loadstring(g_L, code) != LUA_OK) {
        print_all("lua: ");
        print_all(lua_tostring(g_L, -1));
        print_all("\n");
        lua_pop(g_L, 1);
        return 1;
    }
    if (lua_pcall(g_L, 0, LUA_MULTRET, 0) != LUA_OK) {
        print_all("lua: ");
        print_all(lua_tostring(g_L, -1));
        print_all("\n");
        lua_pop(g_L, 1);
        return 1;
    }
    return 0;
}

/* ---------------- REPL ---------------- */
static void repl(tinyos_api_t *api) {
    char line[1024], acc[4096];
    u32 acc_len = 0;
    print_all("Lua 5.4 on TinyOS (VFS io, no math/io-sockets). :quit to exit.\n> ");
    for (;;) {
        int n = api->readline(line, sizeof line);
        if (n < 0) break;
        line[n] = 0;
        if (strcmp(line, ":quit") == 0 || strcmp(line, "quit") == 0) break;
        if (line[0] == 0) {
            if (acc_len > 0) { /* 空行结束续接 */
                if (run_lua(acc)) { acc_len = 0; }
                acc_len = 0;
                print_all("> ");
            }
            continue;
        }
        if (acc_len + n + 2 > sizeof acc) {
            print_all("line too long\n> ");
            continue;
        }
        if (acc_len) { strcat(acc, "\n"); acc_len++; }
        strcat(acc, line);
        acc_len += (u32)n;
        if (!line_needs_cont(line)) {
            run_lua(acc);
            acc_len = 0;
            print_all("> ");
        } else {
            print_all("... ");
        }
    }
}

/* ---------------- 脚本模式 ---------------- */
static int run_script(tinyos_api_t *api, const char *path) {
    char buf[512];
    int n = api->cmdarg(buf, sizeof buf);
    if (n <= 0) return 0; /* 无参数 -> REPL */
    /* 跳过空白，取第一个 token */
    while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\t')) n--;
    buf[n] = 0;
    print_all("TinyLua: running ");
    print_all(buf);
    print_all("\n");
    int r = luaL_dofile(g_L, buf);
    if (r != LUA_OK) {
        print_all("lua: ");
        print_all(lua_tostring(g_L, -1));
        print_all("\n");
        lua_pop(g_L, 1);
        return 1;
    }
    return 0;
}

/* ---------------- 入口 ---------------- */
void user_main(tinyos_api_t *api) {
    g_lua_api = api;
    g_L = luaL_newstate();
    if (!g_L) {
        print_all("TinyLua: out of memory (lua state)\n");
        return;
    }
    luaL_openlibs(g_L);

    /* os.time 已被 patch 成 time(NULL)；os.execute/exit 在裸机上无意义，清掉 */
    lua_getglobal(g_L, "os");
    if (lua_istable(g_L, -1)) {
        lua_pushstring(g_L, "execute");
        lua_pushnil(g_L);
        lua_settable(g_L, -3);
        lua_pushstring(g_L, "exit");
        lua_pushnil(g_L);
        lua_settable(g_L, -3);
    }
    lua_pop(g_L, 1);

    int have_arg = 0;
    {
        char ab[512];
        have_arg = api->cmdarg(ab, sizeof ab) > 0;
    }
    if (have_arg) {
        run_script(api, 0);
    } else {
        repl(api);
    }

    print_all("TinyLua: bye.\n");
    lua_close(g_L);
    g_L = 0;
}
