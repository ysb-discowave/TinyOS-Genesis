/* ============================================================
 * CC.TNCR -- TinyOS 自带的 MiniC 编译器（跑在系统里的编译器）
 * Language: C (cross-compiled by the host tcc.py -> TNCR)
 * Build:    tcc cc.c -o CC.TNCR
 * Run:      cc /home/hello.mc -o /bin/hello.TNCR
 *           run /bin/hello.TNCR
 *
 * 为什么要有这个东西
 * ------------------
 * 主机侧的 tcc.py 依赖 Python + zig，裸机上一样都没有。想让 TinyOS 能
 * "自己编译自己的程序"，就只能把编译器本身也做成 TNCR：
 *   词法 -> 递归下降语法分析 -> 直接发射 i386 机器码 -> 拼 TNCR 头 -> 落盘
 * 生成物装载在 0x01000000，入口偏移 0；入口桩把 api 指针存进数据区的
 * APISLOT，然后 call main。程序里写 api->println("hi") 即可调内核。
 *
 * 支持的 MiniC 子集
 * ------------------
 *   类型    int / char / void / 指针 / 一维数组
 *   声明    全局与局部变量、字符串与常量初始化、int a[] = {1,2,3}
 *   函数    参数、递归、前向调用、return
 *   语句    if/else while for do-while break continue return 复合语句
 *   表达式  算术 关系 逻辑 位运算 ++/--(前后缀) 复合赋值
 *           &取址 *解引用 []下标 函数调用
 *   内核调用 api->println(...) 等（编译器内置 api 成员偏移表）
 *   注释    // 行注释、斜杠星号块注释；# 开头的行整行忽略
 *
 * 代码生成约定
 * ------------
 * 表达式值一律留在 eax：二元运算 "算左值 -> push eax -> 算右值 -> 放进
 * ebx -> pop eax -> 运算 -> 结果在 eax"。调用实参先从左到右算好存进数据区
 * 的参数槽，再按 cdecl 从右往左压栈（参数槽按调用嵌套深度分区，避免内层
 * 调用覆盖外层已算好的实参）。
 * ============================================================ */
#include "api_user.h"

/* ------------------------------------------------------------ 容量配置 */
#define MAXSRC       12000
#define MAXCODE      20000
#define MAXDATA       8000
#define MAXSYM         400
#define MAXLOC          96
#define MAXFIX        1200
#define MAXLAB         400
#define MAXSTR         200
#define MAXSTRCH       256
#define MAXARG           8
#define ARGDEPTH         8
#define ARGSPERDEPTH    16
#define NAMELEN         32
#define MAXINIT        256

#define LOAD_ADDR  0x01000000u
#define OUT_MAX    (24 + MAXCODE + MAXDATA)

/* ------------------------------------------------------------ 小工具 */
static int xstrlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int xstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void xstrcpy(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; }
static void xstrncpy(char *d, const char *s, int n) {
    int i = 0;
    while (s[i] && i < n - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void xitoa(int v, char *d) {
    char t[16];
    int i = 0, n = 0;
    if (v < 0) { d[n++] = '-'; v = -v; }
    if (v == 0) t[i++] = '0';
    while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) d[n++] = t[--i];
    d[n] = 0;
}

/* ------------------------------------------------------------ 全局状态 */
static tinyos_api_t *A;

static int   failed;
static char  errmsg[120];
static int   errline;

static void estrcpy(const char *s) {
    int i = 0;
    while (s[i] && i < 118) { errmsg[i] = s[i]; i++; }
    errmsg[i] = 0;
}
static void die(const char *m) { if (!failed) { failed = 1; estrcpy(m); } }

static char src[MAXSRC];
static int  slen_src;
static int  spos;      /* 下一个待读字符的位置 */
static int  tpos;      /* 当前记号 tk 在源码里的起点（回退/重解析时用） */

static u8   code[MAXCODE];
static int  clen;
static u8   data[MAXDATA];
static int  dlen;
static u8   out[OUT_MAX];

/* ------------------------------------------------------------ 类型 */
#define TY_VOID 0
#define TY_INT  1
#define TY_CHAR 2

typedef struct { int b; int p; int n; } Type;   /* 基础类型 / 指针层数 / 数组元素数 */

static void ty_set(Type *t, int b, int p, int n) { t->b = b; t->p = p; t->n = n; }
static int  ty_base(Type *t) { return (t->b == TY_CHAR) ? 1 : 4; }
static int  ty_size(Type *t) {
    if (t->p > 0) return 4;
    if (t->n > 0) return t->n * ty_base(t);
    return ty_base(t);
}
static int  ty_isbyte(Type *t) { return (t->p == 0 && t->n == 0 && t->b == TY_CHAR); }
static void ty_decay(Type *t) { if (t->p == 0 && t->n > 0) { t->n = 0; t->p = 1; } }
static void ty_deref(Type *t) {
    if (t->p > 0) t->p--;
    else if (t->n > 0) t->n = 0;
}
static void ty_addr(Type *t) {
    if (t->n > 0) { t->n = 0; t->p = 1; }
    else t->p++;
}

/* ------------------------------------------------------------ 代码发射 */
static void eb(u8 b) {
    if (clen >= MAXCODE) { die("generated code too large"); return; }
    code[clen++] = b;
}
static void e32(u32 v) {
    eb((u8)(v & 0xFF)); eb((u8)((v >> 8) & 0xFF));
    eb((u8)((v >> 16) & 0xFF)); eb((u8)((v >> 24) & 0xFF));
}
static void put32at(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xFF);         p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF); p[3] = (u8)((v >> 24) & 0xFF);
}

/* ------------------------------------------------------------ 重定位 */
#define FX_CALL 1
#define FX_JMP  2
#define FX_ABS  3
#define FX_DAT  4
typedef struct { u8 kind; int pos; int a; } Fix;
static Fix fixes[MAXFIX];
static int nfix;

static void add_fix(int kind, int pos, int a) {
    if (nfix >= MAXFIX) { die("too many relocations"); return; }
    fixes[nfix].kind = (u8)kind; fixes[nfix].pos = pos; fixes[nfix].a = a;
    nfix++;
}

static int labs[MAXLAB];
static int nlab;
static int new_lab(void) {
    if (nlab >= MAXLAB) { die("too many labels"); return 0; }
    labs[nlab] = -1;
    return nlab++;
}
static void lab_here(int l) { labs[l] = clen; }
static void emit_jmp(int l) { eb(0xE9); add_fix(FX_JMP, clen, l); e32(0); }
static void emit_jcc(int op, int l) { eb(0x0F); eb((u8)op); add_fix(FX_JMP, clen, l); e32(0); }
static void emit_call(int s) { eb(0xE8); add_fix(FX_CALL, clen, s); e32(0); }

/* ------------------------------------------------------------ 指令速记 */
static void i_mov_eax_imm(u32 v)  { eb(0xB8); e32(v); }
static void i_push_eax(void)      { eb(0x50); }
static void i_pop_eax(void)       { eb(0x58); }
static void i_pop_ebx(void)       { eb(0x5B); }
static void i_mov_ebx_eax(void)   { eb(0x89); eb(0xC3); }   /* mov ebx,eax */
static void i_mov_eax_ebx(void)   { eb(0x89); eb(0xD8); }   /* mov eax,ebx */
static void i_mov_ecx_eax(void)   { eb(0x89); eb(0xC1); }   /* mov ecx,eax */
static void i_mov_eax_ecx(void)   { eb(0x89); eb(0xC8); }   /* mov eax,ecx */
static void i_mov_ecx_ebx(void)   { eb(0x89); eb(0xD9); }   /* mov ecx,ebx */
static void i_mov_eax_edx(void)   { eb(0x89); eb(0xD0); }   /* mov eax,edx */
static void i_mov_eax_esp0(void)  { eb(0x8B); eb(0x04); eb(0x24); }
static void i_add_eax_ebx(void)   { eb(0x01); eb(0xD8); }
static void i_sub_eax_ebx(void)   { eb(0x29); eb(0xD8); }
static void i_and_eax_ebx(void)   { eb(0x21); eb(0xD8); }
static void i_or_eax_ebx(void)    { eb(0x09); eb(0xD8); }
static void i_xor_eax_ebx(void)   { eb(0x31); eb(0xD8); }
static void i_cmp_eax_ebx(void)   { eb(0x39); eb(0xD8); }
static void i_imul_ebx(void)      { eb(0x0F); eb(0xAF); eb(0xC3); }
static void i_imul_eax_imm(u32 v) { eb(0x69); eb(0xC0); e32(v); }
static void i_test_eax(void)      { eb(0x85); eb(0xC0); }
static void i_cdq(void)           { eb(0x99); }
static void i_idiv_ebx(void)      { eb(0xF7); eb(0xFB); }
static void i_neg_eax(void)       { eb(0xF7); eb(0xD8); }
static void i_not_eax(void)       { eb(0xF7); eb(0xD0); }
static void i_movzx_al(void)      { eb(0x0F); eb(0xB6); eb(0xC0); }
static void i_setcc(u8 op)        { eb(0x0F); eb(op); eb(0xC0); i_movzx_al(); }
static void i_inc_eax(void)       { eb(0x40); }
static void i_dec_eax(void)       { eb(0x48); }

static void i_add_esp(int n) {
    if (n <= 0) return;
    if (n < 128) { eb(0x83); eb(0xC4); eb((u8)n); }
    else { eb(0x81); eb(0xC4); e32((u32)n); }
}
static void i_sub_esp(u32 n) { eb(0x81); eb(0xEC); e32(n); }

/* eax = [eax]，按类型决定宽度 */
static void i_load(Type *t) {
    if (ty_isbyte(t)) { eb(0x8A); eb(0x00); i_movzx_al(); }
    else              { eb(0x8B); eb(0x00); }
}
/* [eax] = ebx */
static void i_store(Type *t) {
    if (ty_isbyte(t)) { eb(0x88); eb(0x18); }
    else              { eb(0x89); eb(0x18); }
}

/* 局部/参数：off 为相对 ebp 的位移 */
static void i_local_val(int off, Type *t) {
    if (t->p == 0 && t->n > 0) { eb(0x8D); eb(0x85); e32((u32)off); return; } /* 数组取地址 */
    eb(0x8B); eb(0x85); e32((u32)off);
    if (ty_isbyte(t)) i_movzx_al();
}
static void i_local_addr(int off) { eb(0x8D); eb(0x85); e32((u32)off); }
/* 全局：off 是数据区偏移，绝对地址要重定位 */
static void i_global_val(int off, Type *t) {
    if (t->p == 0 && t->n > 0) { eb(0xB8); add_fix(FX_ABS, clen, off); e32(0); return; }
    eb(0xA1); add_fix(FX_ABS, clen, off); e32(0);
    if (ty_isbyte(t)) i_movzx_al();
}
static void i_global_addr(int off) { eb(0xB8); add_fix(FX_ABS, clen, off); e32(0); }

/* ------------------------------------------------------------ 数据区 */
static int dall(int n, int align) {
    dlen = (dlen + align - 1) & ~(align - 1);
    int r = dlen;
    dlen += n;
    if (dlen > MAXDATA) die("data section too large");
    return r;
}
static void dput32(int off, u32 v) { put32at(data + off, v); }
static void dput8(int off, u8 v)   { data[off] = v; }

static int api_slot;
static int argslot_off;

static int stroff[MAXSTR];
static int strcnt;
static int add_string(const char *s, int n) {
    for (int i = 0; i < strcnt; i++) {
        const char *p = (const char *)data + stroff[i];
        int j = 0, same = 1;
        while (j < n) { if (p[j] != s[j]) { same = 0; break; } j++; }
        if (same && p[j] == 0) return stroff[i];
    }
    if (strcnt >= MAXSTR) { die("too many string literals"); return 0; }
    int off = dall(n + 1, 1);
    for (int i = 0; i < n; i++) dput8(off + i, (u8)s[i]);
    dput8(off + n, 0);
    stroff[strcnt++] = off;
    return off;
}

/* ------------------------------------------------------------ api 偏移表
 * 必须与 tools/compiler/api_user.h 里 tinyos_api_t 的成员顺序逐项对应，
 * 构建时用 tools/compiler/apicheck.py 校验。 */
typedef struct { const char *name; int off; } ApiEnt;
static const ApiEnt API_TAB[] = {
    { "print",            0 }, { "println",          4 },
    { "readline",         8 }, { "gui_open",        12 },
    { "desktop",         16 }, { "get_session",     20 },
    { "ftp_get",         24 }, { "ftp_put",         28 },
    { "smb_get",         32 }, { "smb_put",         36 },
    { "net_info",        40 }, { "itoa",            44 },
    { "sock_listen",     48 }, { "sock_accept",     52 },
    { "sock_recv",       56 }, { "sock_poll_recv",  60 },
    { "sock_send",       64 }, { "sock_close",      68 },
    { "sock_closed",     72 }, { "sock_readable",   76 },
    { "sock_local_ip",   80 }, { "user_verify",     84 },
    { "user_uid",        88 }, { "user_current",    92 },
    { "user_set_current", 96 }, { "user_name_of",  100 },
    { "exec_capture",   104 }, { "file_read",      108 },
    { "file_write",     112 }, { "rand_bytes",     116 },
    { "ticks",          120 }, { "local_kbhit",    124 },
    { "local_getc",     128 }, { "draw",           132 },
    { "clearscr",       136 }, { "gotoxy",         140 },
    { "setcolor",       144 }, { "getkey",         148 },
    { "cmdarg",         152 }, { "cwd",            156 },
};
#define API_TAB_N ((int)(sizeof(API_TAB) / sizeof(API_TAB[0])))

static int api_off(const char *n) {
    for (int i = 0; i < API_TAB_N; i++)
        if (!xstrcmp(API_TAB[i].name, n)) return API_TAB[i].off;
    return -1;
}

/* ------------------------------------------------------------ 符号表 */
typedef struct {
    char name[NAMELEN];
    int  isfunc;
    int  defined;
    Type ty;
    int  off;         /* 变量：数据区偏移；函数：代码偏移（-1 = 只有声明） */
    int  nparams;
} Sym;
static Sym syms[MAXSYM];
static int nsym;

typedef struct { char name[NAMELEN]; Type ty; int off; } Loc;
static Loc locs[MAXLOC];
static int nloc;
static int frame;
static int sub_pos;          /* 函数序言里 sub esp,imm32 的立即数位置 */

static int find_sym(const char *n) {
    for (int i = 0; i < nsym; i++)
        if (!syms[i].isfunc && !xstrcmp(syms[i].name, n)) return i;
    return -1;
}
static int find_func(const char *n) {
    for (int i = 0; i < nsym; i++)
        if (syms[i].isfunc && !xstrcmp(syms[i].name, n)) return i;
    return -1;
}
static int add_sym(const char *n, int isfunc) {
    for (int i = 0; i < nsym; i++)
        if (syms[i].isfunc == isfunc && !xstrcmp(syms[i].name, n)) return i;
    if (nsym >= MAXSYM) { die("too many symbols"); return -1; }
    xstrncpy(syms[nsym].name, n, NAMELEN);
    syms[nsym].isfunc = isfunc;
    syms[nsym].defined = 0;
    ty_set(&syms[nsym].ty, TY_INT, 0, 0);
    syms[nsym].off = -1;
    syms[nsym].nparams = 0;
    return nsym++;
}
static int find_loc(const char *n) {
    for (int i = nloc - 1; i >= 0; i--)
        if (!xstrcmp(locs[i].name, n)) return i;
    return -1;
}
static int add_loc(const char *n, Type *t) {
    if (nloc >= MAXLOC) { die("too many local variables"); return -1; }
    xstrncpy(locs[nloc].name, n, NAMELEN);
    locs[nloc].ty = *t;
    int sz = ty_size(t);
    if (sz < 4) sz = 4;
    sz = (sz + 3) & ~3;
    frame += sz;
    locs[nloc].off = -frame;
    return nloc++;
}
static void add_param(const char *n, Type *t, int idx) {
    if (nloc >= MAXLOC) { die("too many parameters"); return; }
    xstrncpy(locs[nloc].name, n, NAMELEN);
    locs[nloc].ty = *t;
    locs[nloc].off = 8 + 4 * idx;
    nloc++;
}

/* ------------------------------------------------------------ 词法 */
#define TK_EOF   0
#define TK_ID    1
#define TK_NUM   2
#define TK_STR   3
#define TK_KW    4
#define TK_PUNCT 5

#define KW_INT 1
#define KW_CHAR 2
#define KW_VOID 3
#define KW_IF 4
#define KW_ELSE 5
#define KW_WHILE 6
#define KW_FOR 7
#define KW_DO 8
#define KW_RETURN 9
#define KW_BREAK 10
#define KW_CONTINUE 11
#define KW_SIZEOF 12
#define KW_EXTERN 13
#define KW_STATIC 14
#define KW_CONST 15
#define KW_UNSIGNED 16

#define P_ARROW  300
#define P_INC    301
#define P_DEC    302
#define P_SHL    303
#define P_SHR    304
#define P_LE     305
#define P_GE     306
#define P_EQ     307
#define P_NE     308
#define P_ANDAND 309
#define P_OROR   310
#define P_ADDEQ  311
#define P_SUBEQ  312
#define P_MULEQ  313
#define P_DIVEQ  314
#define P_MODEQ  315
#define P_ANDEQ  316
#define P_OREQ   317
#define P_XOREQ  318
#define P_SHLEQ  319
#define P_SHREQ  320

static int  tk;
static int  tk_val;
static char tk_name[NAMELEN];
static char tk_str[MAXSTRCH];
static int  tk_strlen;

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
static int is_dig(int c)   { return c >= '0' && c <= '9'; }
static int is_al(int c)    { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_alnum(int c) { return is_al(c) || is_dig(c); }

static int kw_of(const char *s) {
    if (!xstrcmp(s, "int"))      return KW_INT;
    if (!xstrcmp(s, "char"))     return KW_CHAR;
    if (!xstrcmp(s, "void"))     return KW_VOID;
    if (!xstrcmp(s, "if"))       return KW_IF;
    if (!xstrcmp(s, "else"))     return KW_ELSE;
    if (!xstrcmp(s, "while"))    return KW_WHILE;
    if (!xstrcmp(s, "for"))      return KW_FOR;
    if (!xstrcmp(s, "do"))       return KW_DO;
    if (!xstrcmp(s, "return"))   return KW_RETURN;
    if (!xstrcmp(s, "break"))    return KW_BREAK;
    if (!xstrcmp(s, "continue")) return KW_CONTINUE;
    if (!xstrcmp(s, "sizeof"))   return KW_SIZEOF;
    if (!xstrcmp(s, "extern"))   return KW_EXTERN;
    if (!xstrcmp(s, "static"))   return KW_STATIC;
    if (!xstrcmp(s, "const"))    return KW_CONST;
    if (!xstrcmp(s, "unsigned")) return KW_UNSIGNED;
    if (!xstrcmp(s, "signed"))   return KW_INT;
    return 0;
}

static int esc(int c) {
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return 0;
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    default: return c;
    }
}

static void lex(void) {
    for (;;) {
        while (spos < slen_src && is_space(src[spos])) {
            if (src[spos] == '\n') errline++;
            spos++;
        }
        if (spos + 1 < slen_src && src[spos] == '/' && src[spos + 1] == '/') {
            while (spos < slen_src && src[spos] != '\n') spos++;
            continue;
        }
        if (spos + 1 < slen_src && src[spos] == '/' && src[spos + 1] == '*') {
            spos += 2;
            while (spos + 1 < slen_src && !(src[spos] == '*' && src[spos + 1] == '/')) {
                if (src[spos] == '\n') errline++;
                spos++;
            }
            spos += 2;
            continue;
        }
        if (spos < slen_src && src[spos] == '#') {     /* 预处理行整行忽略 */
            while (spos < slen_src && src[spos] != '\n') spos++;
            continue;
        }
        break;
    }
    tpos = spos;                      /* 本记号的起点 */
    if (spos >= slen_src) { tk = TK_EOF; return; }

    int c = (unsigned char)src[spos];

    if (is_dig(c)) {
        int v = 0;
        if (c == '0' && spos + 1 < slen_src && (src[spos + 1] == 'x' || src[spos + 1] == 'X')) {
            spos += 2;
            while (spos < slen_src) {
                int h = (unsigned char)src[spos], d;
                if (h >= '0' && h <= '9') d = h - '0';
                else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                else break;
                v = v * 16 + d;
                spos++;
            }
        } else {
            while (spos < slen_src && is_dig((unsigned char)src[spos])) {
                v = v * 10 + (src[spos] - '0');
                spos++;
            }
        }
        tk = TK_NUM; tk_val = v; return;
    }

    if (is_al(c)) {
        int i = 0;
        while (spos < slen_src && is_alnum((unsigned char)src[spos]) && i < NAMELEN - 1)
            tk_name[i++] = src[spos++];
        tk_name[i] = 0;
        int k = kw_of(tk_name);
        tk = k ? TK_KW : TK_ID;
        tk_val = k;
        return;
    }

    if (c == '"') {
        spos++;
        int i = 0;
        while (spos < slen_src && src[spos] != '"' && i < MAXSTRCH - 1) {
            int ch = (unsigned char)src[spos++];
            if (ch == '\\' && spos < slen_src) ch = esc((unsigned char)src[spos++]);
            tk_str[i++] = (char)ch;
        }
        if (spos < slen_src) spos++;
        tk_str[i] = 0;
        tk_strlen = i;
        tk = TK_STR;
        return;
    }

    if (c == '\'') {
        spos++;
        int ch = 0;
        if (spos < slen_src) {
            ch = (unsigned char)src[spos++];
            if (ch == '\\' && spos < slen_src) ch = esc((unsigned char)src[spos++]);
        }
        if (spos < slen_src && src[spos] == '\'') spos++;
        tk = TK_NUM; tk_val = ch;
        return;
    }

    spos++;
    if (spos < slen_src) {
        int d = (unsigned char)src[spos];
        int two = c * 256 + d;
        switch (two) {
        case ('-' * 256 + '>'): spos++; tk = TK_PUNCT; tk_val = P_ARROW;  return;
        case ('+' * 256 + '+'): spos++; tk = TK_PUNCT; tk_val = P_INC;    return;
        case ('-' * 256 + '-'): spos++; tk = TK_PUNCT; tk_val = P_DEC;    return;
        case ('<' * 256 + '<'):
            if (spos + 1 < slen_src && src[spos + 1] == '=') { spos += 2; tk = TK_PUNCT; tk_val = P_SHLEQ; return; }
            spos++; tk = TK_PUNCT; tk_val = P_SHL; return;
        case ('>' * 256 + '>'):
            if (spos + 1 < slen_src && src[spos + 1] == '=') { spos += 2; tk = TK_PUNCT; tk_val = P_SHREQ; return; }
            spos++; tk = TK_PUNCT; tk_val = P_SHR; return;
        case ('<' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_LE;     return;
        case ('>' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_GE;     return;
        case ('=' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_EQ;     return;
        case ('!' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_NE;     return;
        case ('&' * 256 + '&'): spos++; tk = TK_PUNCT; tk_val = P_ANDAND; return;
        case ('|' * 256 + '|'): spos++; tk = TK_PUNCT; tk_val = P_OROR;   return;
        case ('+' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_ADDEQ;  return;
        case ('-' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_SUBEQ;  return;
        case ('*' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_MULEQ;  return;
        case ('/' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_DIVEQ;  return;
        case ('%' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_MODEQ;  return;
        case ('&' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_ANDEQ;  return;
        case ('|' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_OREQ;   return;
        case ('^' * 256 + '='): spos++; tk = TK_PUNCT; tk_val = P_XOREQ;  return;
        default: break;
        }
    }
    tk = TK_PUNCT; tk_val = c;
}

static int is_punct(int v) { return (tk == TK_PUNCT && tk_val == v); }
static int is_kw(int k)    { return (tk == TK_KW && tk_val == k); }

static void expect(int p) {
    if (is_punct(p)) { lex(); return; }
    die("syntax error: unexpected token");
}
static int is_assign_op(int v) {
    return v == '=' || v == P_ADDEQ || v == P_SUBEQ || v == P_MULEQ ||
           v == P_DIVEQ || v == P_MODEQ || v == P_ANDEQ || v == P_OREQ ||
           v == P_XOREQ || v == P_SHLEQ || v == P_SHREQ;
}

/* 带当前记号信息的报错，便于定位语法问题 */
static void xstrcat_(char *d, const char *s) {
    int i = 0;
    while (d[i]) i++;
    while (*s && i < 168) d[i++] = *s++;
    d[i] = 0;
}
static void die_tok(const char *m) {
    if (failed) return;
    char b[180], n[16];
    xstrncpy(b, m, 120);
    xstrcat_(b, " [tk=");
    if (tk == TK_EOF)        xstrcat_(b, "eof");
    else if (tk == TK_ID)  { xstrcat_(b, "id:"); xstrcat_(b, tk_name); }
    else if (tk == TK_NUM) { xitoa(tk_val, n);  xstrcat_(b, "num:"); xstrcat_(b, n); }
    else if (tk == TK_STR)   xstrcat_(b, "str");
    else if (tk == TK_KW)  { xitoa(tk_val, n);  xstrcat_(b, "kw:");  xstrcat_(b, n); }
    else                   { xitoa(tk_val, n);  xstrcat_(b, "punct:"); xstrcat_(b, n); }
    xstrcat_(b, " spos=");
    xitoa(spos, n); xstrcat_(b, n);
    xstrcat_(b, "]");
    die(b);
}

/* ------------------------------------------------------------ 表达式
 * 约定：每个 gen_* 把值留在 eax，并把结果类型写进 expty。 */
static Type expty;

static void gen_expr(void);
static void gen_assign(void);
static void gen_stmt(void);
static void gen_unary(void);
static void gen_postfix(void);
static void gen_primary(void);
static void gen_lor(void);
static void gen_land(void);
static void gen_bor(void);
static void gen_bxor(void);
static void gen_band(void);
static void gen_eq(void);
static void gen_rel(void);
static void gen_shift(void);
static void gen_add(void);
static void gen_mul(void);

static int argdepth;

/* 解析并压栈实参；返回实参个数。
 * 实参先按顺序算出来存进参数槽，再按 cdecl 从右往左 push。 */
static void gen_args(int *nout) {
    int d = argdepth++;
    int n = 0;
    if (!is_punct(')')) {
        for (;;) {
            gen_assign();
            if (failed) return;
            if (n >= MAXARG) { die("too many call arguments"); return; }
            int slot = argslot_off + (d * ARGSPERDEPTH + n) * 4;
            eb(0xA3); add_fix(FX_ABS, clen, slot); e32(0);   /* mov [slot],eax */
            n++;
            if (is_punct(',')) { lex(); continue; }
            break;
        }
    }
    expect(')');
    for (int i = n - 1; i >= 0; i--) {
        int slot = argslot_off + (d * ARGSPERDEPTH + i) * 4;
        eb(0xA1); add_fix(FX_ABS, clen, slot); e32(0);       /* mov eax,[slot] */
        i_push_eax();
    }
    argdepth--;
    *nout = n;
}

static void gen_primary(void) {
    if (failed) return;
    if (tk == TK_NUM) {
        i_mov_eax_imm((u32)tk_val);
        ty_set(&expty, TY_INT, 0, 0);
        lex();
        return;
    }
    if (tk == TK_STR) {
        int off = add_string(tk_str, tk_strlen);
        eb(0xB8); add_fix(FX_ABS, clen, off); e32(0);
        ty_set(&expty, TY_CHAR, 1, 0);
        lex();
        return;
    }
    if (is_punct('(')) {
        lex();
        gen_expr();
        expect(')');
        return;
    }
    if (tk == TK_ID) {
        char nm[NAMELEN];
        xstrncpy(nm, tk_name, NAMELEN);
        lex();

        if (is_punct(P_ARROW)) {
            if (xstrcmp(nm, "api")) { die("'->' is only supported on 'api'"); return; }
            lex();
            if (tk != TK_ID) { die("expected an api member after '->'"); return; }
            char mem[NAMELEN];
            xstrncpy(mem, tk_name, NAMELEN);
            lex();
            expect('(');
            int n = 0;
            gen_args(&n);
            if (failed) return;
            int off = api_off(mem);
            if (off < 0) { die("unknown api member"); return; }
            eb(0xA1); add_fix(FX_ABS, clen, api_slot); e32(0);  /* mov eax,[api] */
            eb(0xFF); eb(0x90); e32((u32)off);                  /* call [eax+off] */
            i_add_esp(n * 4);
            ty_set(&expty, TY_INT, 0, 0);
            return;
        }

        if (is_punct('(')) {
            lex();
            int si = find_func(nm);
            if (si < 0) si = add_sym(nm, 1);
            if (si < 0) return;
            int n = 0;
            gen_args(&n);
            if (failed) return;
            emit_call(si);
            i_add_esp(n * 4);
            ty_set(&expty, TY_INT, 0, 0);
            return;
        }

        int li = find_loc(nm);
        if (li >= 0) {
            i_local_val(locs[li].off, &locs[li].ty);
            expty = locs[li].ty;
            ty_decay(&expty);
            return;
        }
        int gi = find_sym(nm);
        if (gi >= 0) {
            i_global_val(syms[gi].off, &syms[gi].ty);
            expty = syms[gi].ty;
            ty_decay(&expty);
            return;
        }
        die("undefined identifier");
        return;
    }
    die_tok("syntax error in expression");
}

static void gen_postfix(void) {
    if (failed) return;
    gen_primary();
    while (!failed) {
        if (is_punct('[')) {
            Type bt = expty;
            Type et = bt; ty_deref(&et);
            int esz = ty_base(&et);
            lex();
            i_push_eax();                                  /* 基址 */
            gen_expr();                                    /* 下标 -> eax */
            if (failed) return;
            if (esz > 1) i_imul_eax_imm((u32)esz);
            i_mov_ebx_eax();
            i_pop_eax();                                   /* 基址 */
            i_add_eax_ebx();
            expect(']');
            i_load(&et);
            expty = et;
            continue;
        }
        if (is_punct(P_INC) || is_punct(P_DEC)) {
            die("postfix ++/-- is only supported on a simple variable");
            return;
        }
        break;
    }
}

/* 取某个标识符的地址到 eax（局部变量 lea，全局变量立即数地址） */
static void emit_var_addr(int li, int gi) {
    if (li >= 0) i_local_addr(locs[li].off);
    else         i_global_addr(syms[gi].off);
}

static void parse_type_later(Type *t);

static void gen_unary(void) {
    if (failed) return;

    if (is_punct(P_INC) || is_punct(P_DEC)) {
        int op = tk_val;
        lex();
        if (tk != TK_ID) { die("++ / -- need a variable"); return; }
        char nm[NAMELEN]; xstrncpy(nm, tk_name, NAMELEN);
        lex();
        int li = find_loc(nm), gi = -1;
        Type t;
        if (li >= 0) t = locs[li].ty;
        else {
            gi = find_sym(nm);
            if (gi < 0) { die("undefined variable in ++/--"); return; }
            t = syms[gi].ty;
        }
        if (t.p != 0 || t.n > 0) { die("++ / -- need a scalar"); return; }
        emit_var_addr(li, gi);
        i_push_eax();
        i_load(&t);                       /* eax = 旧值 */
        if (op == P_INC) i_inc_eax(); else i_dec_eax();
        i_mov_ebx_eax();
        i_pop_eax();
        i_store(&t);                      /* 前缀：结果就是新值，仍在 eax */
        ty_set(&expty, TY_INT, 0, 0);
        return;
    }

    if (is_punct('&')) {
        lex();
        if (tk != TK_ID) { die("'&' needs a variable"); return; }
        char nm[NAMELEN]; xstrncpy(nm, tk_name, NAMELEN);
        lex();
        int li = find_loc(nm), gi = find_sym(nm);
        if (li < 0 && gi < 0) { die("undefined variable in '&'"); return; }
        Type t = (li >= 0) ? locs[li].ty : syms[gi].ty;
        if (is_punct('[')) {              /* &a[i] */
            Type et = t; ty_deref(&et);
            int esz = ty_base(&et);
            lex();
            i_push_eax();
            gen_expr();
            if (failed) return;
            if (esz > 1) i_imul_eax_imm((u32)esz);
            i_mov_ebx_eax();
            i_pop_eax();
            /* eax 现在是垃圾：重新取基址 */
            if (li >= 0) { i_local_val(locs[li].off, &t); }
            else         { i_global_val(syms[gi].off, &t); }
            i_add_eax_ebx();
            expect(']');
            ty_addr(&et);
            expty = et;
            return;
        }
        emit_var_addr(li, gi);
        ty_addr(&t);
        expty = t;
        return;
    }

    if (is_punct('*')) {
        lex();
        gen_unary();                      /* eax = 指针值，expty = 指针类型 */
        if (failed) return;
        Type et = expty;
        ty_deref(&et);
        i_load(&et);
        expty = et;
        return;
    }

    if (is_punct('-')) {
        lex(); gen_unary();
        if (failed) return;
        i_neg_eax();
        ty_set(&expty, TY_INT, 0, 0);
        return;
    }
    if (is_punct('+')) { lex(); gen_unary(); return; }
    if (is_punct('~')) {
        lex(); gen_unary();
        if (failed) return;
        i_not_eax();
        ty_set(&expty, TY_INT, 0, 0);
        return;
    }
    if (is_punct('!')) {
        lex(); gen_unary();
        if (failed) return;
        i_test_eax();
        i_setcc(0x94);                    /* sete al */
        ty_set(&expty, TY_INT, 0, 0);
        return;
    }
    if (is_kw(KW_SIZEOF)) {
        lex();
        if (is_punct('(')) {
            int save = spos, sl = errline;
            lex();
            if (is_kw(KW_INT) || is_kw(KW_CHAR) || is_kw(KW_VOID)) {
                Type t;
                parse_type_later(&t);
                expect(')');
                i_mov_eax_imm((u32)ty_size(&t));
                ty_set(&expty, TY_INT, 0, 0);
                return;
            }
            spos = save; errline = sl; tk = TK_PUNCT; tk_val = '(';
        }
        die("sizeof: only sizeof(int) / sizeof(char) / sizeof(void) are supported");
        return;
    }

    gen_postfix();
}

static void emit_binop(int op, Type *t) {
    (void)t;
    switch (op) {
    case '+': i_add_eax_ebx(); break;
    case '-': i_sub_eax_ebx(); break;
    case '*': i_imul_ebx();    break;
    case '/': i_cdq(); i_idiv_ebx(); break;
    case '%': i_cdq(); i_idiv_ebx(); i_mov_eax_edx(); break;
    case '&': i_and_eax_ebx(); break;
    case '|': i_or_eax_ebx();  break;
    case '^': i_xor_eax_ebx(); break;
    case P_SHL: i_mov_ecx_ebx(); eb(0xD3); eb(0xE0); break;   /* shl eax,cl */
    case P_SHR: i_mov_ecx_ebx(); eb(0xD3); eb(0xE8); break;   /* shr eax,cl */
    default: die("unsupported operator"); break;
    }
}

static void gen_mul(void) {
    gen_unary();
    while (!failed) {
        int op = 0;
        if (is_punct('*')) op = '*';
        else if (is_punct('/')) op = '/';
        else if (is_punct('%')) op = '%';
        else break;
        lex();
        i_push_eax();
        gen_unary();
        if (failed) return;
        i_mov_ebx_eax();
        i_pop_eax();
        emit_binop(op, &expty);
        ty_set(&expty, TY_INT, 0, 0);
    }
}

static void gen_add(void) {
    gen_mul();
    while (!failed) {
        int op = 0;
        if (is_punct('+')) op = '+';
        else if (is_punct('-')) op = '-';
        else break;
        Type lt = expty;
        lex();
        i_push_eax();
        gen_mul();
        if (failed) return;
        if (lt.p > 0) {                                   /* 指针算术：按元素大小缩放 */
            Type et = lt; ty_deref(&et);
            int esz = ty_base(&et);
            if (esz > 1) i_imul_eax_imm((u32)esz);
        }
        i_mov_ebx_eax();
        i_pop_eax();
        emit_binop(op, &lt);
        expty = lt;
    }
}

static void gen_shift(void) {
    gen_add();
    while (!failed) {
        int op = 0;
        if (is_punct(P_SHL)) op = P_SHL;
        else if (is_punct(P_SHR)) op = P_SHR;
        else break;
        lex();
        i_push_eax();
        gen_add();
        if (failed) return;
        i_mov_ebx_eax();
        i_pop_eax();
        emit_binop(op, &expty);
        ty_set(&expty, TY_INT, 0, 0);
    }
}

static void gen_rel(void) {
    gen_shift();
    while (!failed) {
        u8 cc = 0;
        if (is_punct('<'))       { cc = 0x9C; }   /* setl  */
        else if (is_punct('>'))  { cc = 0x9F; }   /* setg  */
        else if (is_punct(P_LE)) { cc = 0x9E; }   /* setle */
        else if (is_punct(P_GE)) { cc = 0x9D; }   /* setge */
        else break;
        lex();
        i_push_eax();
        gen_shift();
        if (failed) return;
        i_mov_ebx_eax();
        i_pop_eax();
        i_cmp_eax_ebx();
        i_setcc(cc);
        ty_set(&expty, TY_INT, 0, 0);
    }
}

static void gen_eq(void) {
    gen_rel();
    while (!failed) {
        u8 cc;
        if (is_punct(P_EQ)) cc = 0x94;                       /* sete  */
        else if (is_punct(P_NE)) cc = 0x95;                  /* setne */
        else break;
        lex();
        i_push_eax();
        gen_rel();
        if (failed) return;
        i_mov_ebx_eax();
        i_pop_eax();
        i_cmp_eax_ebx();
        i_setcc(cc);
        ty_set(&expty, TY_INT, 0, 0);
    }
}

static void gen_band(void) {
    gen_eq();
    while (!failed) {
        if (!is_punct('&')) break;
        lex();
        i_push_eax();
        gen_eq();
        if (failed) return;
        i_mov_ebx_eax();
        i_pop_eax();
        i_and_eax_ebx();
        ty_set(&expty, TY_INT, 0, 0);
    }
}

static void gen_bxor(void) {
    gen_band();
    while (!failed) {
        if (!is_punct('^')) break;
        lex();
        i_push_eax();
        gen_band();
        if (failed) return;
        i_mov_ebx_eax();
        i_pop_eax();
        i_xor_eax_ebx();
        ty_set(&expty, TY_INT, 0, 0);
    }
}

static void gen_bor(void) {
    gen_bxor();
    while (!failed) {
        if (!is_punct('|')) break;
        lex();
        i_push_eax();
        gen_bxor();
        if (failed) return;
        i_mov_ebx_eax();
        i_pop_eax();
        i_or_eax_ebx();
        ty_set(&expty, TY_INT, 0, 0);
    }
}

static void gen_land(void) {
    gen_bor();
    while (!failed) {
        if (!is_punct(P_ANDAND)) break;
        int lf = new_lab(), le = new_lab();
        lex();
        i_test_eax();
        emit_jcc(0x84, lf);               /* je  -> false */
        gen_bor();
        if (failed) return;
        i_test_eax();
        emit_jcc(0x84, lf);
        i_mov_eax_imm(1);
        emit_jmp(le);
        lab_here(lf);
        i_mov_eax_imm(0);
        lab_here(le);
        ty_set(&expty, TY_INT, 0, 0);
    }
}

static void gen_lor(void) {
    gen_land();
    while (!failed) {
        if (!is_punct(P_OROR)) break;
        int lt = new_lab(), le = new_lab();
        lex();
        i_test_eax();
        emit_jcc(0x85, lt);               /* jne -> true */
        gen_land();
        if (failed) return;
        i_test_eax();
        emit_jcc(0x85, lt);
        i_mov_eax_imm(0);
        emit_jmp(le);
        lab_here(lt);
        i_mov_eax_imm(1);
        lab_here(le);
        ty_set(&expty, TY_INT, 0, 0);
    }
}

/* ------------------------------------------------------------ 赋值
 * 统一套路：先把左值地址算到 eax 并 push，再算右值；
 * 复合赋值时用 [esp] 偷看地址取出旧值参与运算，最后 ebx=新值 / eax=地址 存储。 */
static void gen_assign(void) {
    if (failed) return;

    /* *p = ... / *p 作为右值 */
    if (is_punct('*')) {
        lex();
        gen_unary();                       /* eax = 地址，expty = 指针类型 */
        if (failed) return;
        Type et = expty;
        ty_deref(&et);
        if (tk == TK_PUNCT && is_assign_op(tk_val)) {
            int op = tk_val;
            lex();
            i_push_eax();                  /* 地址 */
            gen_assign();
            if (failed) return;
            if (op != '=') {
                i_mov_ecx_eax();           /* ecx = 右值 */
                i_mov_eax_esp0();          /* eax = 地址 */
                i_load(&et);               /* eax = 旧值 */
                i_mov_ebx_eax();           /* ebx = 旧值 */
                i_mov_eax_ecx();           /* eax = 右值 */
                emit_binop(op, &et);
            }
            i_mov_ebx_eax();
            i_pop_eax();
            i_store(&et);
            ty_set(&expty, et.b, 0, 0);
            return;
        }
        i_load(&et);
        expty = et;
        return;
    }

    if (tk == TK_ID) {
        int savep = spos, sln = errline;
        char nm[NAMELEN];
        xstrncpy(nm, tk_name, NAMELEN);
        lex();

        /* 后缀 ++ / -- */
        if (is_punct(P_INC) || is_punct(P_DEC)) {
            int op = tk_val;
            lex();
            int li = find_loc(nm), gi = -1;
            Type t;
            if (li >= 0) t = locs[li].ty;
            else {
                gi = find_sym(nm);
                if (gi < 0) { die("undefined variable in ++/--"); return; }
                t = syms[gi].ty;
            }
            if (t.p != 0 || t.n > 0) { die("++ / -- need a scalar"); return; }
            emit_var_addr(li, gi);
            i_push_eax();
            i_load(&t);                    /* eax = 旧值 */
            i_mov_ecx_eax();               /* ecx = 旧值（后缀的结果） */
            if (op == P_INC) i_inc_eax(); else i_dec_eax();
            i_mov_ebx_eax();
            i_pop_eax();
            i_store(&t);
            i_mov_eax_ecx();
            ty_set(&expty, TY_INT, 0, 0);
            return;
        }

        /* a[i] ... */
        if (is_punct('[')) {
            int li = find_loc(nm), gi = find_sym(nm);
            if (li < 0 && gi < 0) { die("undefined array"); return; }
            Type at = (li >= 0) ? locs[li].ty : syms[gi].ty;
            Type et = at; ty_deref(&et);
            int esz = ty_base(&et);
            lex();
            i_push_eax();
            gen_expr();
            if (failed) return;
            if (esz > 1) i_imul_eax_imm((u32)esz);
            i_mov_ebx_eax();               /* ebx = 下标 * esz */
            i_pop_eax();
            if (li >= 0) i_local_val(locs[li].off, &at);
            else         i_global_val(syms[gi].off, &at);
            i_add_eax_ebx();               /* eax = 元素地址 */
            expect(']');
            if (tk == TK_PUNCT && is_assign_op(tk_val)) {
                int op = tk_val;
                lex();
                i_push_eax();
                gen_assign();
                if (failed) return;
                if (op != '=') {
                    i_mov_ecx_eax();
                    i_mov_eax_esp0();
                    i_load(&et);
                    i_mov_ebx_eax();
                    i_mov_eax_ecx();
                    emit_binop(op, &et);
                }
                i_mov_ebx_eax();
                i_pop_eax();
                i_store(&et);
                ty_set(&expty, et.b, 0, 0);
                return;
            }
            i_load(&et);
            expty = et;
            return;
        }

        /* 普通变量赋值 */
        if (tk == TK_PUNCT && is_assign_op(tk_val)) {
            int op = tk_val;
            lex();
            int li = find_loc(nm), gi = -1;
            Type t;
            if (li >= 0) t = locs[li].ty;
            else {
                gi = find_sym(nm);
                if (gi < 0) { die("assignment to an undeclared variable"); return; }
                t = syms[gi].ty;
            }
            if (t.n > 0) { die("cannot assign to an array"); return; }
            emit_var_addr(li, gi);
            i_push_eax();
            gen_assign();
            if (failed) return;
            if (op != '=') {
                i_mov_ecx_eax();
                i_mov_eax_esp0();
                i_load(&t);
                i_mov_ebx_eax();
                i_mov_eax_ecx();
                emit_binop(op, &t);
            }
            i_mov_ebx_eax();
            i_pop_eax();
            i_store(&t);
            ty_set(&expty, t.b, 0, 0);
            return;
        }

        /* 既不是赋值也不是下标：回退成普通表达式 */
        spos = savep; errline = sln; tk = TK_ID; tk_val = 0;
        xstrncpy(tk_name, nm, NAMELEN);
        gen_lor();
        return;
    }

    gen_lor();
}

static void gen_expr(void) { gen_assign(); }

/* ------------------------------------------------------------ 语句 */
static int brk_stk[16], cont_stk[16];
static int loop_depth;

static void parse_type_later(Type *t) {
    int b = TY_INT;
    if (is_kw(KW_INT))      { b = TY_INT;  lex(); }
    else if (is_kw(KW_CHAR)) { b = TY_CHAR; lex(); }
    else if (is_kw(KW_VOID)) { b = TY_VOID; lex(); }
    ty_set(t, b, 0, 0);
    while (is_punct('*')) { lex(); t->p++; }
}

static void parse_type(Type *t) {
    for (;;) {
        if (is_kw(KW_CONST) || is_kw(KW_STATIC) || is_kw(KW_EXTERN)) { lex(); continue; }
        if (is_kw(KW_UNSIGNED)) { lex(); continue; }
        break;
    }
    parse_type_later(t);
}

static int is_type_start(void) {
    if (tk != TK_KW) return 0;
    return tk_val == KW_INT || tk_val == KW_CHAR || tk_val == KW_VOID ||
           tk_val == KW_UNSIGNED || tk_val == KW_CONST || tk_val == KW_STATIC ||
           tk_val == KW_EXTERN;
}

static void gen_local_decl(void) {
    Type bt;
    parse_type(&bt);
    if (failed) return;
    for (;;) {
        if (tk != TK_ID) { die("expected a variable name"); return; }
        char nm[NAMELEN];
        xstrncpy(nm, tk_name, NAMELEN);
        lex();
        Type t = bt;
        int n = 0;
        if (is_punct('[')) {
            lex();
            if (tk != TK_NUM) { die("local arrays need a constant size"); return; }
            n = tk_val;
            lex();
            expect(']');
            t.n = n;
        }
        if (is_punct('=')) {
            lex();
            if (t.p == 0 && t.n > 0) {
                if (t.b == TY_CHAR && tk == TK_STR) {          /* char buf[N] = "abc" */
                    int li = add_loc(nm, &t);
                    if (li < 0) return;
                    int cnt = (n < tk_strlen + 1) ? n : tk_strlen + 1;
                    /* 用一串 mov byte [ebp+off], imm8 填进去 */
                    int base = locs[li].off;
                    for (int i = 0; i < cnt; i++) {
                        eb(0xC6); eb(0x85); e32((u32)(base + i)); eb((u8)tk_str[i]);
                    }
                    lex();
                } else { die("array initializers must be string or omitted"); return; }
            } else if (tk == TK_STR) {                          /* char *p = "abc" */
                int off = add_string(tk_str, tk_strlen);
                int li = add_loc(nm, &t);
                if (li < 0) return;
                eb(0xB8); add_fix(FX_ABS, clen, off); e32(0);
                eb(0x89); eb(0x85); e32((u32)locs[li].off);    /* mov [ebp+off],eax */
                lex();
            } else {                                            /* 常量或表达式 */
                int li = add_loc(nm, &t);
                if (li < 0) return;
                gen_assign();
                if (failed) return;
                eb(0x89); eb(0x85); e32((u32)locs[li].off);    /* mov [ebp+off],eax */
            }
        } else {
            if (add_loc(nm, &t) < 0) return;
        }
        if (is_punct(',')) { lex(); continue; }
        break;
    }
    expect(';');
}

static void gen_block(void) {
    expect('{');
    int saved = nloc;
    while (!failed && !is_punct('}')) gen_stmt();
    expect('}');
    nloc = saved;                 /* 块作用域结束，弹出本块的局部变量 */
}

static void gen_stmt(void) {
    if (failed) return;

    if (is_punct('{')) { gen_block(); return; }
    if (is_punct(';')) { lex(); return; }

    if (is_kw(KW_IF)) {
        lex();
        expect('(');
        gen_expr();
        expect(')');
        if (failed) return;
        int lelse = new_lab(), lend = new_lab();
        i_test_eax();
        emit_jcc(0x84, lelse);
        gen_stmt();
        if (failed) return;
        if (is_kw(KW_ELSE)) {
            lex();
            emit_jmp(lend);
            lab_here(lelse);
            gen_stmt();
            lab_here(lend);
        } else {
            lab_here(lelse);
        }
        return;
    }

    if (is_kw(KW_WHILE)) {
        lex();
        expect('(');
        int ltop = new_lab(), lend = new_lab();
        lab_here(ltop);
        gen_expr();
        expect(')');
        if (failed) return;
        i_test_eax();
        emit_jcc(0x84, lend);
        if (loop_depth >= 16) { die("loops nested too deeply"); return; }
        brk_stk[loop_depth] = lend; cont_stk[loop_depth] = ltop; loop_depth++;
        gen_stmt();
        loop_depth--;
        if (failed) return;
        emit_jmp(ltop);
        lab_here(lend);
        return;
    }

    if (is_kw(KW_DO)) {
        lex();
        int ltop = new_lab(), lcont = new_lab(), lend = new_lab();
        lab_here(ltop);
        if (loop_depth >= 16) { die("loops nested too deeply"); return; }
        brk_stk[loop_depth] = lend; cont_stk[loop_depth] = lcont; loop_depth++;
        gen_stmt();
        loop_depth--;
        if (failed) return;
        lab_here(lcont);
        if (!is_kw(KW_WHILE)) { die("expected 'while' after do-block"); return; }
        lex();
        expect('(');
        gen_expr();
        expect(')');
        if (failed) return;
        i_test_eax();
        emit_jcc(0x85, ltop);
        lab_here(lend);
        expect(';');
        return;
    }

    if (is_kw(KW_FOR)) {
        lex();
        expect('(');
        /* 用一个作用域包住循环体，好让 for 里声明的变量用完就丢 */
        if (is_type_start()) gen_local_decl();
        else if (!is_punct(';')) { gen_expr(); expect(';'); }
        else lex();
        if (failed) return;
        int ltop = new_lab(), lcont = new_lab(), lend = new_lab();
        lab_here(ltop);
        if (!is_punct(';')) {
            gen_expr();
            if (failed) return;
            i_test_eax();
            emit_jcc(0x84, lend);
        }
        /* 此刻 tk 是第二个 ';'、spos 正好停在它后面——这就是步进表达式的
         * 起点，必须在 expect(';') 把它吃掉之前记下来。 */
        int step_pos = spos, step_line = errline;
        expect(';');

        /* 步进表达式要生成在循环体之后，但它在源码里出现在循环体之前。
         * 办法：记下步进的源码位置，先把记号一路扫到匹配的 ')'，等函数体
         * 生成完再把词法位置倒回去重新生成一遍步进。 */
        {
            int depth = 0;
            for (;;) {
                if (tk == TK_EOF) { die("unterminated for-header"); return; }
                if (is_punct('(')) depth++;
                else if (is_punct(')')) { if (depth == 0) break; depth--; }
                lex();
            }
        }
        expect(')');

        if (loop_depth >= 16) { die("loops nested too deeply"); return; }
        brk_stk[loop_depth] = lend; cont_stk[loop_depth] = lcont; loop_depth++;
        gen_stmt();
        loop_depth--;
        if (failed) return;
        /* 记下"循环体之后那个记号"的源码起点：下面要回退源码重新生成步进，
         * 生成完必须回到这里，否则外层会把循环体再解析（并再生成）一遍。
         * 注意用 tpos（记号起点）而不是 spos（记号读完之后的位置）。 */
        int after_body = tpos, after_line = errline;

        lab_here(lcont);
        spos = step_pos; errline = step_line;
        lex();
        if (!is_punct(')')) {
            gen_expr();
            if (failed) return;
        }
        /* 回退之后 ')' 又回到记号流里，必须再吃掉一次 */
        expect(')');
        if (failed) return;
        emit_jmp(ltop);
        lab_here(lend);

        spos = after_body; errline = after_line;
        lex();
        return;
    }

    if (is_kw(KW_RETURN)) {
        lex();
        if (!is_punct(';')) {
            gen_expr();
            if (failed) return;
        }
        expect(';');
        eb(0x89); eb(0xEC);       /* mov esp,ebp */
        eb(0x5D);                 /* pop ebp */
        eb(0xC3);                 /* ret */
        return;
    }

    if (is_kw(KW_BREAK)) {
        lex();
        expect(';');
        if (loop_depth == 0) { die("break outside of a loop"); return; }
        emit_jmp(brk_stk[loop_depth - 1]);
        return;
    }
    if (is_kw(KW_CONTINUE)) {
        lex();
        expect(';');
        if (loop_depth == 0) { die("continue outside of a loop"); return; }
        emit_jmp(cont_stk[loop_depth - 1]);
        return;
    }

    if (is_type_start()) { gen_local_decl(); return; }

    gen_expr();
    if (failed) return;
    expect(';');
}

/* ------------------------------------------------------------ 顶层声明 */
static int initvals[MAXINIT];

/* 已知的全局变量初始化 */
static void global_init(int off, Type *t) {
    if (tk == TK_STR) {
        if (t->p == 0 && t->n > 0 && t->b == TY_CHAR) {
            int cnt = tk_strlen + 1;
            if (cnt > t->n) cnt = t->n;
            for (int i = 0; i < cnt; i++) dput8(off + i, (u8)tk_str[i]);
            lex();
            return;
        }
        if (t->p == 1) {
            int so = add_string(tk_str, tk_strlen);
            add_fix(FX_DAT, off, so);
            dput32(off, 0);
            lex();
            return;
        }
        die("string initializer needs char[] or char*");
        return;
    }
    if (is_punct('{')) {
        lex();
        int n = 0;
        if (!is_punct('}')) {
            for (;;) {
                if (tk != TK_NUM) { die("array initializers must be constants"); return; }
                if (n < MAXINIT) initvals[n] = tk_val;
                n++;
                lex();
                if (is_punct(',')) { lex(); continue; }
                break;
            }
        }
        expect('}');
        int esz = ty_base(t);
        int cap = (t->n > 0) ? t->n : n;
        for (int i = 0; i < n && i < cap; i++) {
            if (esz == 1) dput8(off + i, (u8)initvals[i]);
            else          dput32(off + i * 4, (u32)initvals[i]);
        }
        return;
    }
    /* 常量（可带前导负号） */
    int neg = 0;
    if (is_punct('-')) { neg = 1; lex(); }
    if (tk != TK_NUM) { die("global initializers must be constants"); return; }
    int v = neg ? -tk_val : tk_val;
    lex();
    if (ty_isbyte(t)) dput8(off, (u8)v);
    else              dput32(off, (u32)v);
}

static void parse_global(void) {
    Type bt;
    parse_type(&bt);
    if (failed) return;
    for (;;) {
        if (tk != TK_ID) { die("expected a declaration name"); return; }
        char nm[NAMELEN];
        xstrncpy(nm, tk_name, NAMELEN);
        lex();

        if (is_punct('(')) {              /* 函数 */
            lex();
            int si = add_sym(nm, 1);
            if (si < 0) return;
            nloc = 0; frame = 0;
            int np = 0;
            if (!is_punct(')')) {
                for (;;) {
                    Type pt;
                    parse_type(&pt);
                    if (failed) return;
                    if (tk != TK_ID) { die("expected a parameter name"); return; }
                    char pn[NAMELEN];
                    xstrncpy(pn, tk_name, NAMELEN);
                    lex();
                    if (pt.n > 0) { pt.n = 0; pt.p++; }      /* 数组参数退化为指针 */
                    add_param(pn, &pt, np);
                    np++;
                    if (is_punct(',')) { lex(); continue; }
                    break;
                }
            }
            expect(')');
            if (failed) return;
            syms[si].nparams = np;
            if (is_punct(';')) { lex(); return; }            /* 只是声明 */
            if (!is_punct('{')) { die("expected a function body"); return; }

            syms[si].defined = 1;
            syms[si].off = clen;
            eb(0x55);                                        /* push ebp */
            eb(0x89); eb(0xE5);                              /* mov ebp,esp */
            sub_pos = clen + 2;
            i_sub_esp(0);                                    /* 占位，函数末尾回填 */
            gen_block();
            if (failed) return;
            put32at(code + sub_pos, (u32)((frame + 15) & ~15));
            eb(0x89); eb(0xEC);                              /* mov esp,ebp */
            eb(0x5D);                                        /* pop ebp */
            eb(0xC3);                                        /* ret */
            return;
        }

        /* 全局变量 */
        Type t = bt;
        int unknown = 0;
        if (is_punct('[')) {
            lex();
            if (tk == TK_NUM) { t.n = tk_val; lex(); }
            else { t.n = 0; unknown = 1; }
            expect(']');
        }
        int off;
        if (unknown) {
            if (!is_punct('=')) { die("an array of unknown size needs an initializer"); return; }
            lex();
            if (tk == TK_STR) {
                int cnt = tk_strlen + 1;
                off = dall(cnt, 1);
                for (int i = 0; i < cnt; i++) dput8(off + i, (u8)tk_str[i]);
                t.n = cnt;
                lex();
            } else if (is_punct('{')) {
                lex();
                int n = 0;
                if (!is_punct('}')) {
                    for (;;) {
                        if (tk != TK_NUM) { die("array initializers must be constants"); return; }
                        if (n < MAXINIT) initvals[n] = tk_val;
                        n++;
                        lex();
                        if (is_punct(',')) { lex(); continue; }
                        break;
                    }
                }
                expect('}');
                int esz = ty_base(&t);
                off = dall(n * esz, 4);
                for (int i = 0; i < n; i++) {
                    if (esz == 1) dput8(off + i, (u8)initvals[i]);
                    else          dput32(off + i * 4, (u32)initvals[i]);
                }
                t.n = n;
            } else { die("an array of unknown size needs an initializer"); return; }
        } else {
            off = dall(ty_size(&t) < 4 ? 4 : ty_size(&t), 4);
            if (is_punct('=')) { lex(); global_init(off, &t); }
        }
        if (failed) return;
        int si = find_sym(nm);
        if (si >= 0) { die("duplicate global variable"); return; }
        si = add_sym(nm, 0);
        if (si < 0) return;
        syms[si].ty = t;
        syms[si].off = off;
        syms[si].defined = 1;

        if (is_punct(',')) { lex(); continue; }
        break;
    }
    expect(';');
}

/* ------------------------------------------------------------ 编译驱动 */
static int compile(void) {
    clen = 0; dlen = 0; nsym = 0; nfix = 0; nlab = 0; strcnt = 0;
    nloc = 0; frame = 0; argdepth = 0; loop_depth = 0;
    failed = 0; errline = 1;
    errmsg[0] = 0;
    ty_set(&expty, TY_INT, 0, 0);

    api_slot    = dall(4, 4);
    argslot_off = dall(ARGDEPTH * ARGSPERDEPTH * 4, 4);

    /* 入口桩（entry_off = 0）：
     *   mov eax,[esp+4]      ; api 指针
     *   mov [APISLOT],eax
     *   call main
     *   ret                  ; cdecl：参数由调用方（内核）清栈 */
    eb(0x8B); eb(0x44); eb(0x24); eb(0x04);
    eb(0xA3); add_fix(FX_ABS, clen, api_slot); e32(0);
    int msym = add_sym("main", 1);
    if (msym < 0) return -1;
    emit_call(msym);
    eb(0xC3);

    lex();
    while (tk != TK_EOF && !failed) parse_global();
    if (failed) return -1;
    if (!syms[msym].defined) { die("the program has no main()"); return -1; }

    for (int i = 0; i < nfix; i++) {
        int p = fixes[i].pos;
        if (fixes[i].kind == FX_CALL) {
            int s = fixes[i].a;
            if (syms[s].off < 0) { die("call to a function that is never defined"); return -1; }
            put32at(code + p, (u32)(syms[s].off - (p + 4)));
        } else if (fixes[i].kind == FX_JMP) {
            int l = fixes[i].a;
            if (labs[l] < 0) { die("internal error: unresolved label"); return -1; }
            put32at(code + p, (u32)(labs[l] - (p + 4)));
        } else if (fixes[i].kind == FX_ABS) {
            put32at(code + p, LOAD_ADDR + (u32)clen + (u32)fixes[i].a);
        } else if (fixes[i].kind == FX_DAT) {
            put32at(data + p, LOAD_ADDR + (u32)clen + (u32)fixes[i].a);
        }
    }
    return 0;
}

/* ------------------------------------------------------------ 路径与参数 */
static void resolve_path(const char *in, char *opath) {
    if (in[0] == '/') { xstrncpy(opath, in, 256); return; }
    char cwd[200];
    cwd[0] = 0;
    A->cwd(cwd, sizeof(cwd));
    if (!cwd[0]) xstrcpy(cwd, "/");
    int k = 0;
    for (int i = 0; cwd[i] && k < 250; i++) opath[k++] = cwd[i];
    if (k && opath[k - 1] != '/' && k < 250) opath[k++] = '/';
    for (int i = 0; in[i] && k < 250; i++) opath[k++] = in[i];
    opath[k] = 0;
}

/* 从参数串里依次取出一个词 */
static int next_word(const char *s, int i, char *w, int max) {
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (!s[i]) return -1;
    int k = 0;
    while (s[i] && s[i] != ' ' && s[i] != '\t' && k < max - 1) w[k++] = s[i++];
    w[k] = 0;
    return i;
}

static void base_name(const char *p, char *b) {
    int last = -1;
    for (int i = 0; p[i]; i++) if (p[i] == '/') last = i;
    int k = 0;
    const char *s = p + last + 1;
    while (s[k] && s[k] != '.') { b[k] = s[k]; k++; }
    b[k] = 0;
}

/* ------------------------------------------------------------ 入口 */
void user_main(tinyos_api_t *api) {
    A = api;
    A->println("TinyOS MiniC compiler  (CC.TNCR)");

    char arg[256];
    arg[0] = 0;
    A->cmdarg(arg, sizeof(arg));

    char word[256];
    int i = next_word(arg, 0, word, sizeof(word));
    if (i < 0) {
        A->println("usage: cc <source.mc> [-o <out.TNCR>]");
        A->println("  example: cc /home/hello.mc -o /bin/hello.TNCR");
        A->println("           run /bin/hello.TNCR");
        A->println("  in a program use api->println(\"hi\") to call the kernel.");
        return;
    }

    char srcpath[256], outpath[256];
    resolve_path(word, srcpath);
    int have_o = 0;

    for (;;) {
        int j = next_word(arg, i, word, sizeof(word));
        if (j < 0) break;
        i = j;
        if (!xstrcmp(word, "-o")) {
            int k = next_word(arg, i, word, sizeof(word));
            if (k < 0) { A->println("cc: -o needs a file name"); return; }
            i = k;
            resolve_path(word, outpath);
            have_o = 1;
        }
    }
    if (!have_o) {
        char bn[64];
        base_name(srcpath, bn);
        char tmp[256];
        int k = 0;
        const char *pre = "/bin/";
        for (int q = 0; pre[q]; q++) tmp[k++] = pre[q];
        for (int q = 0; bn[q]; q++) tmp[k++] = bn[q];
        const char *sfx = ".TNCR";
        for (int q = 0; sfx[q]; q++) tmp[k++] = sfx[q];
        tmp[k] = 0;
        xstrcpy(outpath, tmp);
    }

    int n = A->file_read(srcpath, src, MAXSRC);
    if (n < 0) {
        A->print("cc: cannot read ");
        A->println(srcpath);
        return;
    }
    slen_src = n;
    if (slen_src >= MAXSRC) slen_src = MAXSRC - 1;
    src[slen_src] = 0;
    spos = 0;

    if (compile() != 0) {
        char line[24];
        xitoa(errline, line);
        A->print("cc: ");
        A->print(errmsg);
        A->print(" (line ");
        A->print(line);
        A->println(")");
        return;
    }

    int total = 24 + clen + dlen;
    /* TNCR 头 */
    out[0] = 'T'; out[1] = 'N'; out[2] = 'C'; out[3] = 'R';
    put32at(out + 4,  0);
    put32at(out + 8,  LOAD_ADDR);
    put32at(out + 12, 0);
    put32at(out + 16, (u32)(clen + dlen));
    put32at(out + 20, 0);
    for (int q = 0; q < clen; q++) out[24 + q] = code[q];
    for (int q = 0; q < dlen; q++) out[24 + clen + q] = data[q];

    if (A->file_write(outpath, (const char *)out, total) != 0) {
        A->print("cc: cannot write ");
        A->println(outpath);
        return;
    }

    char num[24];
    A->print("  source: ");
    A->print(srcpath);
    A->print("  (");
    xitoa(slen_src, num);
    A->print(num);
    A->println(" bytes)");

    A->print("  output: ");
    A->print(outpath);
    A->print("  (");
    xitoa(total, num);
    A->print(num);
    A->print(" bytes: code ");
    xitoa(clen, num);
    A->print(num);
    A->print(", data ");
    xitoa(dlen, num);
    A->print(num);
    A->println(")");

    A->println("  ok - run it with: run <out.TNCR>");
}
