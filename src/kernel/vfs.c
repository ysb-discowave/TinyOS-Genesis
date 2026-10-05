#include "vfs.h"
#include "mm.h"
#include "libc.h"
#include "console.h"
#include "inflate.h"
#include "fs/tinyfs.h"

static vfs_node_t *g_root = NULL;
/* 是否在修改时同步到磁盘（安装/装载过程中临时关闭） */
static int g_persist = 1;

static vfs_node_t *new_node(const char *name, vfs_type t) {
    vfs_node_t *n = (vfs_node_t*)kzalloc(sizeof(vfs_node_t));
    if (!n) return NULL;
    strncpy(n->name, name, 63);
    n->name[63] = 0;
    n->type = t;
    /* 默认归属 root；目录默认可进入、文件默认可读 */
    n->owner = 0;
    n->mode = (u16)((t == VFS_DIR) ? 0755 : 0644);
    return n;
}

int vfs_get_owner(const char *path, int *uid, int *mode) {
    vfs_node_t *n = vfs_resolve(path);
    if (!n) return -1;
    if (uid)  *uid  = (int)n->owner;
    if (mode) *mode = (int)n->mode;
    return 0;
}

int vfs_set_owner(const char *path, int uid, int mode) {
    vfs_node_t *n = vfs_resolve(path);
    if (!n) return -1;
    if (uid >= 0) n->owner = (u16)uid;
    if (mode >= 0) n->mode = (u16)mode;
    return 0;
}

void vfs_init(void) {
    g_root = new_node("/", VFS_DIR);
    vfs_mkdir("/bin");
    vfs_mkdir("/home");
    vfs_mkdir("/sys");
    /* /etc 必须在 romfs 装载前就存在，否则 romfs 里的
     * /etc/passwd、/etc/shadow 会因为父目录缺失而被静默丢弃。 */
    vfs_mkdir("/etc");
    vfs_mkdir("/root");
}

vfs_node_t *vfs_root(void) { return g_root; }

vfs_node_t *vfs_resolve(const char *path) {
    if (!g_root || !path || path[0] != '/') return NULL;
    vfs_node_t *cur = g_root;
    const char *p = path + 1;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[64]; int i = 0;
        while (*p && *p != '/' && i < 63) comp[i++] = *p++;
        comp[i] = 0;
        if (cur->type != VFS_DIR) return NULL;
        vfs_node_t *ch = cur->children;
        int matched = 0;
        while (ch) {
            if (strcmp(ch->name, comp) == 0) { cur = ch; matched = 1; break; }
            ch = ch->next;
        }
        if (!matched) return NULL;
    }
    return cur;
}

/* 由父目录 + 名字取回节点（不存在则返回 NULL） */
static vfs_node_t *find_child(vfs_node_t *par, const char *name) {
    for (vfs_node_t *c = par->children; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

/* 拆分路径为父目录节点 + 末段名字
   名字必须写进调用者的缓冲：若返回指向本函数栈上 parent[] 的指针，
   函数一返回该内存即失效（use-after-return）。 */
static int split_parent(const char *path, vfs_node_t **par_out,
                        char *namebuf, size_t n) {
    if (!path || path[0] != '/') return -1;
    if (n == 0) return -1;
    char parent[256];
    strncpy(parent, path, 255); parent[255] = 0;
    char *slash = strrchr(parent, '/');
    if (!slash) return -1;
    *slash = 0;
    vfs_node_t *par = vfs_resolve(parent[0] ? parent : "/");
    if (!par || par->type != VFS_DIR) return -1;
    if (!slash[1]) return -1;
    strncpy(namebuf, slash + 1, n - 1);
    namebuf[n - 1] = 0;
    *par_out = par;
    return 0;
}

/* 规范化绝对路径（去掉末尾 '/'，双斜杠折叠） */
static void norm_path(const char *in, char *out, int n) {
    int j = 0;
    for (int i = 0; in[i] && j < n - 1; i++) {
        if (in[i] == '/' && j > 0 && out[j - 1] == '/') continue;
        out[j++] = in[i];
    }
    while (j > 1 && out[j - 1] == '/') j--;
    out[j] = 0;
}

int vfs_mkdir(const char *path) {
    vfs_node_t *par; char name[64];
    if (split_parent(path, &par, name, sizeof(name)) != 0) return -1;
    if (find_child(par, name)) return 0;

    vfs_node_t *n = new_node(name, VFS_DIR);
    if (!n) return -1;
    n->parent = par;
    n->next = par->children;
    par->children = n;

    if (g_persist && tfs_mounted()) {
        char np[256]; norm_path(path, np, sizeof(np));
        if (tfs_write(np, TFS_TYPE_DIR, NULL, 0) == 0) n->on_disk = 1;
    }
    return 0;
}

int vfs_write_file(const char *path, const u8 *data, u32 size) {
    vfs_node_t *par; char name[64];
    if (split_parent(path, &par, name, sizeof(name)) != 0) return -1;

    vfs_node_t *ch = find_child(par, name);
    if (ch) {
        if (ch->type != VFS_FILE) return -1;
        u8 *nb = (u8*)kmalloc(size ? size : 1);
        if (!nb) return -1;
        if (size) memcpy(nb, data, size);
        if (ch->data) kfree(ch->data);
        ch->data = nb;
        ch->size = size;
    } else {
        vfs_node_t *n = new_node(name, VFS_FILE);
        if (!n) return -1;
        n->data = (u8*)kmalloc(size ? size : 1);
        if (!n->data) { kfree(n); return -1; }
        if (size) memcpy(n->data, data, size);
        n->size = size;
        n->parent = par;
        n->next = par->children;
        par->children = n;
        ch = n;
    }

    if (g_persist && tfs_mounted()) {
        char np[256]; norm_path(path, np, sizeof(np));
        if (tfs_write(np, TFS_TYPE_FILE, data, size) == 0) ch->on_disk = 1;
        else { kprintf("vfs: disk write failed: %s (disk full?)\n", np); return -1; }
    }
    return 0;
}

const u8 *vfs_read_file(const char *path, u32 *size) {
    vfs_node_t *n = vfs_resolve(path);
    if (!n || n->type != VFS_FILE) return NULL;
    if (size) *size = n->size;
    return n->data;
}

int vfs_delete(const char *path) {
    vfs_node_t *n = vfs_resolve(path);
    if (!n || n == g_root) return -1;

    /* 目录：先递归删除子节点 */
    if (n->type == VFS_DIR) {
        while (n->children) {
            /* 构造子路径 */
            vfs_node_t *c = n->children;
            char sub[512];
            snprintf(sub, sizeof(sub), "%s%s%s", path,
                     path[strlen(path) - 1] == '/' ? "" : "/", c->name);
            if (vfs_delete(sub) != 0) {
                /* 兜底：直接摘链 */
                n->children = c->next;
                if (c->data) kfree(c->data);
                kfree(c);
            }
        }
    }

    vfs_node_t *par = n->parent;
    if (par) {
        vfs_node_t **pp = &par->children;
        while (*pp) { if (*pp == n) { *pp = n->next; break; } pp = &(*pp)->next; }
    }
    if (n->data) kfree(n->data);
    kfree(n);

    if (g_persist && tfs_mounted()) {
        char np[256]; norm_path(path, np, sizeof(np));
        tfs_delete(np);
    }
    return 0;
}

void vfs_list(const char *path, void (*cb)(const char *name, vfs_type t, void *arg), void *arg) {
    vfs_node_t *d = vfs_resolve(path);
    if (!d || d->type != VFS_DIR) return;
    vfs_node_t *ch = d->children;
    while (ch) { cb(ch->name, ch->type, arg); ch = ch->next; }
}

/* ------------------------------------------------------------------ */
/* 递归遍历                                                           */
/* ------------------------------------------------------------------ */
struct walk_ctx {
    void (*cb)(const char *path, vfs_type t, const u8 *data, u32 size, void *arg);
    void *arg;
    char path[512];
};

static void walk_rec(vfs_node_t *n, struct walk_ctx *c) {
    for (vfs_node_t *ch = n->children; ch; ch = ch->next) {
        char save[512];
        strncpy(save, c->path, sizeof(save) - 1); save[sizeof(save) - 1] = 0;

        /* 追加 "/" + 名字 */
        size_t len = strlen(c->path);
        if (len + strlen(ch->name) + 2 < sizeof(c->path)) {
            c->path[len] = '/';
            c->path[len + 1] = 0;
            strncat(c->path, ch->name, sizeof(c->path) - len - 2);
        }

        c->cb(c->path, ch->type, ch->data, ch->size, c->arg);

        if (ch->type == VFS_DIR) walk_rec(ch, c);

        strncpy(c->path, save, sizeof(c->path) - 1); c->path[sizeof(c->path) - 1] = 0;
    }
}

void vfs_walk(void (*cb)(const char *path, vfs_type t, const u8 *data, u32 size, void *arg),
              void *arg) {
    if (!g_root) return;
    struct walk_ctx c;
    c.cb = cb; c.arg = arg;
    strcpy(c.path, "");
    walk_rec(g_root, &c);
}

/* ------------------------------------------------------------------ */
/* 持久化                                                             */
/* ------------------------------------------------------------------ */
static void install_cb(const char *path, vfs_type t, const u8 *data, u32 size, void *arg) {
    int *cnt = (int*)arg;
    u8 ty = (t == VFS_DIR) ? TFS_TYPE_DIR : TFS_TYPE_FILE;
    if (tfs_write(path, ty, data, size) == 0) (*cnt)++;
}

int vfs_install_disk(void) {
    if (!tfs_mounted()) return -1;
    int saved = g_persist;
    g_persist = 0;                       /* 避免逐文件重复回写 */
    int cnt = 0;
    vfs_walk(install_cb, &cnt);
    g_persist = saved;
    return cnt;
}

/* ---- 按组件过滤的落盘（安装向导用）---- */
struct finst_ctx {
    int  cnt;
    int  skipped;
    int (*keep)(const char *path, void *arg);
    void *arg;
};

static void install_cb_filtered(const char *path, vfs_type t, const u8 *data, u32 size,
                                void *arg) {
    struct finst_ctx *c = (struct finst_ctx *)arg;
    if (c->keep && !c->keep(path, c->arg)) { c->skipped++; return; }
    u8 ty = (t == VFS_DIR) ? TFS_TYPE_DIR : TFS_TYPE_FILE;
    if (tfs_write(path, ty, data, size) == 0) c->cnt++;
}

int vfs_install_disk_filter(int (*keep)(const char *path, void *arg), void *arg) {
    if (!tfs_mounted()) return -1;
    int saved = g_persist;
    g_persist = 0;
    struct finst_ctx c;
    c.cnt = 0; c.skipped = 0; c.keep = keep; c.arg = arg;
    vfs_walk(install_cb_filtered, &c);
    g_persist = saved;
    return c.cnt;
}

struct load_ctx { int files; int dirs; };

static void load_cb(const char *path, u8 type, u32 size, void *arg) {
    struct load_ctx *lc = (struct load_ctx*)arg;
    int saved = g_persist;
    g_persist = 0;                       /* 从磁盘装入时不要再写回 */
    if (type == TFS_TYPE_DIR) {
        if (vfs_mkdir(path) == 0) lc->dirs++;
    } else {
        u8 *buf = NULL; u32 sz = 0;
        if (tfs_read(path, &buf, &sz) == 0) {
            if (vfs_write_file(path, buf, sz) == 0) {
                vfs_node_t *n = vfs_resolve(path);
                if (n) n->on_disk = 1;
                lc->files++;
            }
            kfree(buf);
        }
    }
    g_persist = saved;
}

int vfs_load_disk(void) {
    if (!tfs_mounted()) return -1;
    struct load_ctx lc;
    lc.files = 0; lc.dirs = 0;
    tfs_list(load_cb, &lc);
    return lc.files;
}

void vfs_persist_enable(int on) { g_persist = on ? 1 : 0; }

/* romfs 记录（2.0 格式）：
 *   [u32 namelen][name][u8 flag][u32 orig_len][u32 stored_len][data]
 *   结束于 namelen=0。
 * flag=0：data 即原文件；flag=1：data 是 zlib raw-deflate 流，
 *         先 inflate 到 orig_len 字节再挂 VFS。 */
void vfs_load_romfs(const u8 *data, u32 len) {
    u32 off = 0;
#define RD32() ({ u32 v = *(const u32*)(data + off); off += 4; v; })
    while (off + 4 <= len) {
        u32 nl = RD32();
        if (nl == 0) break;
        if (off + nl + 1 + 8 > len) break;
        char name[256]; u32 i;
        for (i = 0; i < nl && i < 255; i++) name[i] = (char)data[off++];
        name[i] = 0;
        u8 flag = data[off++];
        u32 orig = RD32();
        u32 dl = RD32();
        if (off + dl > len) break;

        if (flag == 1) {
            u8 *buf = (u8*)kzalloc(orig ? orig : 1);
            if (!buf) break;
            u32 got = inflate_raw(data + off, dl, buf, orig ? orig : 1);
            if (got == (u32)-1 || got != orig) {
                kfree(buf);
                kprintf("romfs: inflate failed: %s (orig=%u stored=%u)\n",
                        name, orig, dl);
                off += dl;
                continue;
            }
            vfs_write_file(name, buf, orig);
            kfree(buf);
        } else {
            vfs_write_file(name, data + off, dl);
        }
        off += dl;
    }
#undef RD32
}
