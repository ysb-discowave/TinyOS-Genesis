#include "fs/tinyfs.h"
#include "disk/disk.h"
#include "mm.h"
#include "libc.h"
#include "console.h"

#define BS 512u

static tfs_super_t g_sb;
static u8   *g_bitmap = NULL;      /* bitmap_blocks * 512 字节 */
static u32   g_bitmap_bytes = 0;
static tfs_ent_t *g_tab = NULL;    /* TFS_MAXFILES 个表项 */
static int   g_ok = 0;

/* 文件系统在磁盘上的起始 LBA。安装到硬盘时 LBA0 是引导扇区、LBA1.. 是内核，
 * 所有 TinyFS 的块读写都必须加上这个偏移，否则格式化会覆盖引导代码。 */
static u32 g_base = TFS_DEFAULT_BASE_LBA;

/* 分区大小上限（块数）。0 = 一直用到盘尾。
 * 安装向导允许用户自定义文件系统分区的大小，这个上限就是那个"分区边界"：
 * 不设的话 tfs_format 会把分区之后的空间也划进文件系统，
 * 用户留出来给别的用途的空间就被吞掉了。 */
static u32 g_limit = 0;

void tfs_set_base(u32 lba) { g_base = lba; g_ok = 0; }
u32  tfs_base(void)        { return g_base; }

void tfs_set_limit(u32 blocks) { g_limit = blocks; g_ok = 0; }
u32  tfs_limit(void)           { return g_limit; }

/* 带基址偏移的块读写（走当前默认盘） */
static int dsk_read(u32 blk, u32 n, void *buf)  {
    return disk_read(disk_default(), g_base + blk, n, buf);
}
static int dsk_write(u32 blk, u32 n, const void *buf) {
    return disk_write(disk_default(), g_base + blk, n, buf);
}

/* ------------------------------------------------------------------ */
/* 位图                                                               */
/* ------------------------------------------------------------------ */
static int bm_test(u32 b) { return (g_bitmap[b >> 3] >> (b & 7)) & 1; }
static void bm_set(u32 b) { g_bitmap[b >> 3] |= (u8)(1u << (b & 7)); }
static void bm_clr(u32 b) { g_bitmap[b >> 3] &= (u8)~(1u << (b & 7)); }

static void bm_mark_range(u32 first, u32 n, int used) {
    for (u32 i = 0; i < n; i++) {
        u32 b = first + i;
        if (b >= g_sb.total_blocks) break;
        if (used) bm_set(b); else bm_clr(b);
    }
}

/* 连续分配 n 块，返回首块号；失败返回 0（块 0 是超级块，可作错误值） */
static u32 alloc_blocks(u32 n) {
    if (n == 0) return 0;
    if (n > g_sb.free_blocks) return 0;
    u32 run = 0, start = 0;
    for (u32 b = g_sb.data_start; b < g_sb.total_blocks; b++) {
        if (!bm_test(b)) {
            if (run == 0) start = b;
            if (++run == n) { bm_mark_range(start, n, 1); return start; }
        } else {
            run = 0;
        }
    }
    return 0;
}

static void free_blocks(u32 first, u32 n) {
    if (!first || !n) return;
    for (u32 i = 0; i < n; i++) {
        u32 b = first + i;
        if (b < g_sb.total_blocks && bm_test(b)) { bm_clr(b); g_sb.free_blocks++; }
    }
}

static u32 blocks_for(u32 bytes) { return (bytes + BS - 1) / BS; }

/* ------------------------------------------------------------------ */
/* 回写                                                               */
/* ------------------------------------------------------------------ */
static int flush_sb(void) {
    u8 buf[BS];
    memset(buf, 0, BS);
    memcpy(buf, &g_sb, sizeof(g_sb));
    return dsk_write(0, 1, buf);
}

static int flush_bitmap(void) {
    return dsk_write(g_sb.bitmap_start, g_sb.bitmap_blocks, g_bitmap);
}

static int flush_table(void) {
    return dsk_write(g_sb.table_start, g_sb.table_blocks, g_tab);
}

static int flush_all(void) {
    if (flush_bitmap() != 0) return -1;
    if (flush_table() != 0) return -1;
    if (flush_sb() != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 路径工具                                                           */
/* ------------------------------------------------------------------ */
static int path_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* a 是否位于目录 dir 之下（前缀匹配且下一字符为 '/'） */
static int path_under(const char *p, const char *dir) {
    size_t n = strlen(dir);
    if (n == 0 || n >= strlen(p)) return 0;
    if (strncmp(p, dir, n) != 0) return 0;
    return p[n] == '/';
}

/* ------------------------------------------------------------------ */
/* 目录表                                                             */
/* ------------------------------------------------------------------ */
static tfs_ent_t *ent_find(const char *path) {
    for (int i = 0; i < TFS_MAXFILES; i++) {
        if (!g_tab[i].used) continue;
        if (path_eq(g_tab[i].name, path)) return &g_tab[i];
    }
    return NULL;
}

static tfs_ent_t *ent_free_slot(void) {
    for (int i = 0; i < TFS_MAXFILES; i++) if (!g_tab[i].used) return &g_tab[i];
    return NULL;
}

static void ent_clear(tfs_ent_t *e) {
    memset(e, 0, sizeof(*e));
}

int tfs_exists(const char *path) { return ent_find(path) != NULL; }

/* ------------------------------------------------------------------ */
/* 格式化 / 挂载                                                      */
/* ------------------------------------------------------------------ */
int tfs_format(void) {
    if (!disk_default()) return -1;
    u32 secs = disk_sectors(disk_default());
    if (g_base >= secs) return -1;
    u32 total = secs - g_base;               /* 文件系统从 g_base 起算 */
    if (g_limit && g_limit < total) total = g_limit;   /* 分区大小上限 */
    if (total < 128) return -1;

    memset(&g_sb, 0, sizeof(g_sb));
    memcpy(g_sb.magic, "TINYFS01", 8);
    g_sb.version      = 2;                    /* v2: 增加 base_lba */
    g_sb.block_size   = BS;
    g_sb.total_blocks = total;
    g_sb.base_lba     = g_base;

    u32 bm_bytes = (total + 7) / 8;
    g_sb.bitmap_start  = 1;
    g_sb.bitmap_blocks = (bm_bytes + BS - 1) / BS;
    g_sb.table_start   = g_sb.bitmap_start + g_sb.bitmap_blocks;
    g_sb.table_blocks  = (TFS_MAXFILES * TFS_ENT_SIZE + BS - 1) / BS;
    g_sb.data_start    = g_sb.table_start + g_sb.table_blocks;
    g_sb.free_blocks   = total - g_sb.data_start;
    g_sb.file_count    = 0;

    /* 重新分配内存结构 */
    if (g_bitmap) kfree(g_bitmap);
    if (g_tab) kfree(g_tab);
    g_bitmap = (u8*)kzalloc(g_sb.bitmap_blocks * BS);
    g_tab    = (tfs_ent_t*)kzalloc(TFS_MAXFILES * sizeof(tfs_ent_t));
    if (!g_bitmap || !g_tab) {
        kprintf("[tfs] out of memory: bitmap=%p tab=%p (need %u + %u bytes)\n",
                (void*)g_bitmap, (void*)g_tab,
                g_sb.bitmap_blocks * BS, (u32)(TFS_MAXFILES * sizeof(tfs_ent_t)));
        return -1;
    }
    g_bitmap_bytes = g_sb.bitmap_blocks * BS;

    /* 元数据区标记为已用 */
    bm_mark_range(0, g_sb.data_start, 1);

    kprintf("[tfs] layout: base LBA %u, total %u bitmap %u table %u data start %u free %u\n",
            g_base, g_sb.total_blocks, g_sb.bitmap_blocks, g_sb.table_blocks,
            g_sb.data_start, g_sb.free_blocks);

    int fb = flush_bitmap();
    int ft = flush_table();
    int fsc = flush_sb();
    if (fb != 0 || ft != 0 || fsc != 0) {
        kprintf("[tfs] disk write failed: bitmap=%d dirtab=%d superblock=%d\n", fb, ft, fsc);
        g_ok = 0;
        return -1;
    }
    g_ok = 1;
    return 0;
}

/* allow_format = 0 时遇到未格式化的磁盘不格式化，直接返回 -1
 * （安装介质启动时需要：目标盘要等用户在向导里确认之后才能动）。 */
int tfs_init2(int allow_format) {
    g_ok = 0;
    if (!disk_default()) return -1;

    u8 buf[BS];
    if (dsk_read(0, 1, buf) != 0) return -1;

    tfs_super_t sb;
    memcpy(&sb, buf, sizeof(sb));

    if (memcmp(sb.magic, "TINYFS01", 8) != 0 ||
        sb.block_size != BS || sb.total_blocks == 0 ||
        sb.data_start == 0 || sb.data_start >= sb.total_blocks ||
        sb.base_lba != g_base) {
        /* 未格式化（或基址变了）→ 新建 */
        if (!allow_format) return -1;
        if (tfs_format() != 0) return -1;
        return 1;
    }

    g_sb = sb;
    g_bitmap = (u8*)kzalloc(g_sb.bitmap_blocks * BS);
    g_tab    = (tfs_ent_t*)kzalloc(TFS_MAXFILES * sizeof(tfs_ent_t));
    if (!g_bitmap || !g_tab) return -1;
    g_bitmap_bytes = g_sb.bitmap_blocks * BS;

    if (dsk_read(g_sb.bitmap_start, g_sb.bitmap_blocks, g_bitmap) != 0) return -1;
    if (dsk_read(g_sb.table_start, g_sb.table_blocks, g_tab) != 0) return -1;

    g_ok = 1;
    return 0;
}

int tfs_init(void) { return tfs_init2(1); }

int tfs_mounted(void) { return g_ok; }

/* ------------------------------------------------------------------ */
/* 读写删                                                             */
/* ------------------------------------------------------------------ */
int tfs_write(const char *path, u8 type, const u8 *data, u32 size) {
    if (!g_ok || !path || !path[0]) return -1;
    if (strlen(path) >= TFS_NAME_MAX) return -1;

    tfs_ent_t *e = ent_find(path);
    if (e && e->type != type) {
        /* 类型变化：先释放旧内容 */
        if (e->used && e->first) free_blocks(e->first, blocks_for(e->size));
        e->first = 0; e->size = 0;
    }
    if (!e) {
        e = ent_free_slot();
        if (!e) return -1;
        ent_clear(e);
        e->used = 1;
        e->type = type;
        strncpy(e->name, path, TFS_NAME_MAX - 1);
        e->namelen = (u8)strlen(e->name);
        g_sb.file_count++;
    }

    if (type == TFS_TYPE_DIR) {
        e->first = 0;
        e->size = 0;
        if (flush_table() != 0 || flush_sb() != 0) return -1;
        return 0;
    }

    u32 need = blocks_for(size);
    /* 释放旧数据块 */
    if (e->first && e->size) free_blocks(e->first, blocks_for(e->size));
    e->first = 0;
    e->size = 0;

    if (need) {
        u32 first = alloc_blocks(need);
        if (!first) { flush_all(); return -1; }     /* 空间不足 */
        if (size) {
            /* 分块写入，最后一块补零 */
            u8 tmp[BS];
            u32 remain = size, lba = first;
            const u8 *p = data;
            while (remain) {
                u32 n = remain >= BS ? BS : remain;
                if (n == BS) {
                    if (dsk_write(lba, 1, p) != 0) return -1;
                } else {
                    memset(tmp, 0, BS);
                    memcpy(tmp, p, n);
                    if (dsk_write(lba, 1, tmp) != 0) return -1;
                }
                p += n; remain -= n; lba++;
            }
        }
        e->first = first;
        e->size  = size;
    } else {
        e->first = 0;
        e->size  = 0;
    }

    if (flush_all() != 0) return -1;
    return 0;
}

int tfs_read(const char *path, u8 **out, u32 *size) {
    if (!g_ok) return -1;
    tfs_ent_t *e = ent_find(path);
    if (!e || e->type != TFS_TYPE_FILE) return -1;
    u32 sz = e->size;
    /* 磁盘按整扇区读，缓冲区必须足够容纳 blocks_for(sz) 个扇区，
       否则 ata_read 会溢出并踩坏堆中相邻块的块头 */
    u32 need = blocks_for(sz) * BS;
    u8 *buf = (u8*)kmalloc(need ? need : 1);
    if (!buf) return -1;
    if (sz && dsk_read(e->first, blocks_for(sz), buf) != 0) { kfree(buf); return -1; }
    if (out) *out = buf; else kfree(buf);
    if (size) *size = sz;
    return 0;
}

int tfs_delete(const char *path) {
    if (!g_ok) return -1;
    tfs_ent_t *e = ent_find(path);
    if (!e) return -1;

    if (e->type == TFS_TYPE_DIR) {
        /* 递归删除其下所有条目 */
        for (int i = 0; i < TFS_MAXFILES; i++) {
            if (!g_tab[i].used) continue;
            if (path_eq(g_tab[i].name, path)) continue;
            if (!path_under(g_tab[i].name, path)) continue;
            if (g_tab[i].type == TFS_TYPE_FILE && g_tab[i].first)
                free_blocks(g_tab[i].first, blocks_for(g_tab[i].size));
            ent_clear(&g_tab[i]);
            if (g_sb.file_count) g_sb.file_count--;
        }
    } else {
        if (e->first) free_blocks(e->first, blocks_for(e->size));
    }
    ent_clear(e);
    if (g_sb.file_count) g_sb.file_count--;
    return flush_all();
}

void tfs_list(void (*cb)(const char *path, u8 type, u32 size, void *arg), void *arg) {
    if (!g_ok) return;
    for (int i = 0; i < TFS_MAXFILES; i++) {
        if (!g_tab[i].used) continue;
        cb(g_tab[i].name, g_tab[i].type, g_tab[i].size, arg);
    }
}

u32 tfs_total_blocks(void) { return g_ok ? g_sb.total_blocks : 0; }
u32 tfs_free_blocks(void)  { return g_ok ? g_sb.free_blocks : 0; }
u32 tfs_file_count(void)   { return g_ok ? g_sb.file_count : 0; }
u32 tfs_capacity_kb(void)  { return g_ok ? (g_sb.total_blocks / 2) : 0; }

void tfs_info(void) {
    if (!g_ok) { kprintf("TinyFS: not mounted\n"); return; }
    kprintf("TinyFS mounted (version %u, block size %u)\n", g_sb.version, g_sb.block_size);
    kprintf("  total %u blocks (%u KB)   free %u   used %u\n",
            g_sb.total_blocks, tfs_capacity_kb(),
            g_sb.free_blocks, g_sb.total_blocks - g_sb.free_blocks);
    kprintf("  entries %u / %u   data starts at block %u\n",
            g_sb.file_count, (u32)TFS_MAXFILES, g_sb.data_start);
    {
        disk_t *d = disk_default();
        kprintf("  disk: %s (%s)\n",
                d ? d->name : "-", d ? d->model : "");
    }
    (void)g_bitmap_bytes;
}
