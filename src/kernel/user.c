#include "user.h"
#include "vfs.h"
#include "console.h"
#include "sha256.h"
#include "pit.h"
#include "libc.h"

/* ============================================================
 * 多用户子系统实现
 * ============================================================ */

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"

static user_t g_users[USER_MAX];
static int    g_uid_cur = -1;      /* -1 = 未登录 */
static int    g_loaded  = 0;

/* 默认口令。裸机玩具系统，出厂口令写在文档里即可；
 * 安装程序会要求现场设置 root 口令，登录后可用 passwd 修改。 */
#define DEF_ROOT_PASS  "root"
#define DEF_GUEST_PASS "guest"

/* ---------------------------------------------------------------- 工具 */
static int ct_eq(const char *a, const char *b) {
    size_t n = strlen(a);
    if (n != strlen(b)) return 0;
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

static void make_salt(char out[17]) {
    static u32 ctr = 0;
    ++ctr;
    u32 a = pit_ticks() * 2654435761u + ctr * 40503u;
    u32 b = 0x9E3779B9u ^ (pit_ticks() << 7) ^ (ctr * 2246822519u);
    snprintf(out, 17, "%08x%08x", a, b);
}

static void hash_pass(const char *salt, const char *pass, char out[65]) {
    char buf[192];
    snprintf(buf, sizeof(buf), "%s%s", salt, pass);
    sha256_hex((const u8*)buf, (u32)strlen(buf), out);
}

static int load_text(const char *path, char *buf, int n) {
    u32 sz = 0;
    const u8 *d = vfs_read_file(path, &sz);
    if (!d) return -1;
    int k = (int)sz < n - 1 ? (int)sz : n - 1;
    if (k > 0) memcpy(buf, d, (size_t)k);
    buf[k] = 0;
    return k;
}

/* 逐行遍历一块文本；返回去掉 CR/尾空格后的行指针，并原地切开 */
static char *next_line(char **pp) {
    char *s = *pp;
    if (!s || !*s) return NULL;
    char *nl = strchr(s, '\n');
    if (nl) { *nl = 0; *pp = nl + 1; } else { *pp = s + strlen(s); }
    size_t L = strlen(s);
    while (L && (s[L - 1] == '\r' || s[L - 1] == ' ' || s[L - 1] == '\t')) s[--L] = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '#') return next_line(pp);      /* 跳过空行与注释 */
    return s;
}

/* 取第 idx 个 ':' 分隔字段到 out；返回 0 成功 */
static int field(const char *line, int idx, char *out, int n) {
    const char *p = line;
    for (int i = 0; i < idx; i++) {
        p = strchr(p, ':');
        if (!p) return -1;
        p++;
    }
    const char *e = strchr(p, ':');
    int len = e ? (int)(e - p) : (int)strlen(p);
    if (len >= n) len = n - 1;
    memcpy(out, p, (size_t)len);
    out[len] = 0;
    return 0;
}

/* ---------------------------------------------------------------- 查询 */
int user_count(void) {
    int n = 0;
    for (int i = 0; i < USER_MAX; i++) if (g_users[i].used) n++;
    return n;
}
user_t *user_at(int idx) { return (idx >= 0 && idx < USER_MAX) ? &g_users[idx] : NULL; }

user_t *user_by_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < USER_MAX; i++)
        if (g_users[i].used && strcmp(g_users[i].name, name) == 0) return &g_users[i];
    return NULL;
}
user_t *user_by_uid(int uid) {
    for (int i = 0; i < USER_MAX; i++)
        if (g_users[i].used && g_users[i].uid == uid) return &g_users[i];
    return NULL;
}
const char *user_name_of(int uid) {
    user_t *u = user_by_uid(uid);
    return u ? u->name : "?";
}

/* ---------------------------------------------------------------- 落盘 */
static int emit_users(char *out, int n, int shadow) {
    int k = 0;
    if (shadow)
        k += snprintf(out + k, n - k, "# TinyOS shadow: name:salt:sha256(salt+password)\n");
    else
        k += snprintf(out + k, n - k, "# TinyOS user database: name:uid:gid:home\n");
    for (int i = 0; i < USER_MAX && k < n - 120; i++) {
        user_t *u = &g_users[i];
        if (!u->used) continue;
        if (shadow)
            k += snprintf(out + k, n - k, "%s:%s:%s\n", u->name, u->salt, u->hash);
        else
            k += snprintf(out + k, n - k, "%s:%d:%d:%s\n", u->name, u->uid, u->gid, u->home);
    }
    return k;
}

void user_save(void) {
    static char buf[4096];
    emit_users(buf, sizeof(buf), 0);
    vfs_write_file(PASSWD_PATH, (const u8*)buf, (u32)strlen(buf));
    emit_users(buf, sizeof(buf), 1);
    vfs_write_file(SHADOW_PATH, (const u8*)buf, (u32)strlen(buf));
}

static void seed_user(const char *name, int uid, int gid, const char *home, const char *pass) {
    for (int i = 0; i < USER_MAX; i++) {
        if (g_users[i].used) continue;
        user_t *u = &g_users[i];
        memset(u, 0, sizeof(*u));
        u->used = 1;
        strncpy(u->name, name, USER_NAME_MAX - 1);
        u->uid = uid;
        u->gid = gid;
        strncpy(u->home, home, sizeof(u->home) - 1);
        make_salt(u->salt);
        hash_pass(u->salt, pass, u->hash);
        return;
    }
}

/* ---------------------------------------------------------------- 装载 */
static void apply_fs_policy(void) {
    /* /etc/shadow 只有 root 能读；其余 /etc 文件非 root 只读 */
    vfs_set_owner("/etc", 0, 0755);
    vfs_set_owner(PASSWD_PATH, 0, 0644);
    vfs_set_owner(SHADOW_PATH, 0, 0600);
    vfs_set_owner("/bin", 0, 0755);
    vfs_set_owner("/sys", 0, 0555);
    for (int i = 0; i < USER_MAX; i++) {
        if (!g_users[i].used) continue;
        char d[128];
        snprintf(d, sizeof(d), "%s", g_users[i].home);
        if (vfs_resolve(d)) vfs_set_owner(d, g_users[i].uid, 0700);
    }
}

void user_init(void) {
    if (g_loaded) return;
    memset(g_users, 0, sizeof(g_users));

    vfs_mkdir("/etc");
    vfs_mkdir("/root");

    static char buf[4096];

    /* --- passwd --- */
    int have_passwd = (load_text(PASSWD_PATH, buf, sizeof(buf)) >= 0);
    if (have_passwd) {
        char *p = buf, *ln;
        while ((ln = next_line(&p))) {
            char nm[USER_NAME_MAX], s_uid[16], s_gid[16], home[96];
            if (field(ln, 0, nm, sizeof(nm)) != 0) continue;
            if (field(ln, 1, s_uid, sizeof(s_uid)) != 0) continue;
            if (field(ln, 2, s_gid, sizeof(s_gid)) != 0) continue;
            if (field(ln, 3, home, sizeof(home)) != 0) continue;
            for (int i = 0; i < USER_MAX; i++) {
                if (g_users[i].used) continue;
                g_users[i].used = 1;
                strncpy(g_users[i].name, nm, USER_NAME_MAX - 1);
                g_users[i].uid = atoi(s_uid);
                g_users[i].gid = atoi(s_gid);
                strncpy(g_users[i].home, home, sizeof(g_users[i].home) - 1);
                break;
            }
        }
    }

    /* --- shadow --- */
    static char sbuf[4096];
    if (load_text(SHADOW_PATH, sbuf, sizeof(sbuf)) >= 0) {
        char *p = sbuf, *ln;
        while ((ln = next_line(&p))) {
            char nm[USER_NAME_MAX], salt[24], h[72];
            if (field(ln, 0, nm, sizeof(nm)) != 0) continue;
            if (field(ln, 1, salt, sizeof(salt)) != 0) continue;
            if (field(ln, 2, h, sizeof(h)) != 0) continue;
            user_t *u = user_by_name(nm);
            if (!u) continue;
            strncpy(u->salt, salt, 16); u->salt[16] = 0;
            strncpy(u->hash, h, 64);    u->hash[64] = 0;
        }
    }

    /* 首次启动（或用户库被清空）：写入出厂账户 */
    if (user_count() == 0) {
        seed_user("root",  0,    0,    "/root",       DEF_ROOT_PASS);
        seed_user("guest", 1000, 1000, "/home/guest", DEF_GUEST_PASS);
        kprintf("[user] no user database found: created default accounts\n");
        kprintf("[user]   root/root (uid 0) and guest/guest (uid 1000) - change with 'passwd'\n");
        /* 保证缺失的口令哈希一定被写出来 */
        for (int i = 0; i < USER_MAX; i++) {
            if (!g_users[i].used) continue;
            if (!g_users[i].hash[0]) {
                make_salt(g_users[i].salt);
                hash_pass(g_users[i].salt, "changeme", g_users[i].hash);
            }
        }
    }

    /* 家目录 */
    for (int i = 0; i < USER_MAX; i++) {
        if (!g_users[i].used) continue;
        if (g_users[i].home[0] == '/') vfs_mkdir(g_users[i].home);
    }
    vfs_mkdir("/home/guest");

    /* 只有缺文件时才回写，避免每次启动重写磁盘 */
    if (!have_passwd) user_save();
    if (load_text(SHADOW_PATH, sbuf, sizeof(sbuf)) < 0) user_save();

    apply_fs_policy();
    g_loaded = 1;
}

/* ---------------------------------------------------------------- 身份 */
int  user_current(void)   { return g_uid_cur; }
void user_set_current(int uid) { g_uid_cur = uid; }
int  user_logged_in(void) { return g_uid_cur >= 0; }

/* ---------------------------------------------------------------- 认证 */
int user_verify(const char *name, const char *pass) {
    user_t *u = user_by_name(name);
    if (!u) {
        /* 用户名不存在也走一遍哈希，避免用时间差枚举用户名 */
        char dummy[65];
        hash_pass("0000000000000000", pass ? pass : "", dummy);
        return -1;
    }
    char got[65];
    hash_pass(u->salt, pass ? pass : "", got);
    return ct_eq(got, u->hash) ? 0 : -1;
}

int user_login_prompt(void) {
    if (user_count() == 0) {          /* 没有用户库：放行，避免把自己锁在外面 */
        kprintf("[user] no accounts available; continuing as root\n");
        user_set_current(0);
        return 0;
    }
    char name[USER_NAME_MAX];
    char pass[64];
    for (int tries = 0; tries < 3; tries++) {
        kprintf("TinyOS Genesis v0.1  login: ");
        int n = cons_readline(name, sizeof(name));
        if (n < 0) n = 0;
        name[n] = 0;
        if (!name[0]) { tries--; continue; }

        kprintf("Password: ");
        int m = cons_readline_masked(pass, sizeof(pass));
        if (m < 0) m = 0;
        pass[m] = 0;
        kprintf("\n");

        if (user_verify(name, pass) == 0) {
            user_t *u = user_by_name(name);
            user_set_current(u->uid);
            kprintf("Welcome to TinyOS, %s (uid %d, home %s)\n", u->name, u->uid, u->home);
            if (u->uid == 0 && !strcmp(pass, DEF_ROOT_PASS))
                kprintf("warning: you are using the default root password; run 'passwd'\n");
            return u->uid;
        }
        kprintf("login incorrect\n");
    }
    kprintf("login: 3 incorrect attempts\n");
    return -1;
}

int user_change_pass(const char *name, const char *newpass) {
    user_t *u = user_by_name(name);
    if (!u) return -1;
    make_salt(u->salt);
    hash_pass(u->salt, newpass, u->hash);
    user_save();
    return 0;
}

int user_add(const char *name, const char *pass, int uid, const char *home) {
    if (!name || !name[0]) return -1;
    if (user_by_name(name)) return -1;
    if (strlen(name) >= USER_NAME_MAX) return -1;
    if (uid < 0) {
        uid = 1000;
        while (user_by_uid(uid)) uid++;
    }
    char h[96];
    if (home && home[0]) snprintf(h, sizeof(h), "%s", home);
    else                 snprintf(h, sizeof(h), "/home/%s", name);
    seed_user(name, uid, uid, h, pass ? pass : "changeme");
    vfs_mkdir(h);
    user_save();
    apply_fs_policy();
    return 0;
}

int user_del(const char *name) {
    user_t *u = user_by_name(name);
    if (!u) return -1;
    if (u->uid == 0) return -2;          /* 不许删 root */
    memset(u, 0, sizeof(*u));
    user_save();
    return 0;
}

void user_list(void) {
    kprintf("NAME             UID   GID   HOME\n");
    for (int i = 0; i < USER_MAX; i++) {
        if (!g_users[i].used) continue;
        kprintf("%-16s %-5d %-5d %s\n", g_users[i].name, g_users[i].uid,
                g_users[i].gid, g_users[i].home);
    }
}

/* ---------------------------------------------------------------- 权限 */
static void parent_of(const char *path, char *out, int n) {
    strncpy(out, path, n - 1); out[n - 1] = 0;
    char *s = strrchr(out, '/');
    if (!s) { strcpy(out, "/"); return; }
    if (s == out) { out[1] = 0; return; }
    *s = 0;
}

const char *user_denied_msg(void) {
    return "permission denied (you are not the owner; try 'su root')";
}

int user_can_write(const char *path) {
    int uid = user_current();
    if (uid == 0) return 1;                                  /* root 全放行 */
    if (!path || path[0] != '/') return 0;

    /* 系统目录对非 root 只读 */
    if (!strncmp(path, "/etc", 4) && (path[4] == 0 || path[4] == '/')) return 0;
    if (!strncmp(path, "/bin", 4) && (path[4] == 0 || path[4] == '/')) return 0;
    if (!strncmp(path, "/sys", 4) && (path[4] == 0 || path[4] == '/')) return 0;

    vfs_node_t *n = vfs_resolve(path);
    if (n) {
        int o = 0, m = 0;
        vfs_get_owner(path, &o, &m);
        if (o == uid) return 1;
        /* 父目录所有者也能写自己目录下的东西 */
        char par[256];
        parent_of(path, par, sizeof(par));
        o = 0; m = 0;
        vfs_get_owner(par, &o, &m);
        return (o == uid);
    }

    /* 新建节点：看父目录归属 */
    char par[256];
    parent_of(path, par, sizeof(par));
    int o = 0, m = 0;
    vfs_get_owner(par, &o, &m);
    return (o == uid);
}

int user_can_read(const char *path) {
    int uid = user_current();
    if (uid == 0) return 1;                                   /* root 全放行 */
    if (!path) return 0;
    if (!strcmp(path, SHADOW_PATH)) return 0;                  /* 口令库仅 root 可读 */
    int o = 0, m = 0;
    if (vfs_get_owner(path, &o, &m) != 0) return 1;            /* 不存在：交给上层报错 */
    if (o == uid) return (m & 0400) ? 1 : 0;                   /* 属主看 owner 读位 */
    return (m & 0004) ? 1 : 0;                                 /* 其它人看 other 读位 */
}
