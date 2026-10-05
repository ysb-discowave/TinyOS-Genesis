#include "mm.h"
#include "libc.h"
#include "console.h"

/* ============================================================
 * 内核堆分配器
 * ------------------------------------------------------------
 * 池的基址 **不能写死**：内核映像里嵌着 romfs（编译好的 TNCR 程序），
 * romfs 一大，.rodata/.bss/.stack 就会越过 2MB。曾经 MM_BASE 写死成
 * 0x200000，于是堆的第一次分配就落在内核自己的 .bss/栈上——表现为
 * "启动到 net_init 就 fatal exception #6，eip=3"（返回地址被踩）。
 * 现在改为按链接脚本导出的 __kernel_end 向上取 1MB 对齐，并设 4MB 下限。
 * 结构：每块前置 16 字节块头；空闲块串成按地址升序的链表；
 *       分配时 first-fit 并可分裂；释放时与前后相邻空闲块合并。
 * ============================================================ */

extern u8 __kernel_end[];             /* link.ld：内核映像末尾（含 .stack） */

#define MM_SIZE (8u * 1024u * 1024u)
#define MM_MAGIC 0x4B4C424Du          /* 'KBLM' */

static u32 mm_base(void) {
    u32 e = (u32)__kernel_end;
    u32 b = (e + 0xFFFFFu) & ~0xFFFFFu;      /* 向上取整到 1MB 边界 */
    if (b < 0x400000u) b = 0x400000u;        /* 至少从 4MB 起，留出余量 */
    return b;
}

typedef struct blk {
    u32  size;            /* 有效数据字节数（不含块头） */
    u32  magic;
    struct blk *next;     /* 仅空闲块使用：下一个空闲块 */
    u8   free;
    u8   pad[3];
} blk_t;                  /* 16 字节 */

#define HDR ((u32)sizeof(blk_t))
#define ALIGN16(x) (((x) + 15u) & ~(size_t)15)

static u8    *g_pool;
static blk_t *g_free;        /* 空闲链表（按地址升序） */
static u32    g_used;        /* 已分配数据字节数 */
static u32    g_free_bytes;  /* 空闲数据字节数 */

static u32 g_base;

void mm_init(void) {
    g_base = mm_base();
    g_pool = (u8*)g_base;
    blk_t *b = (blk_t*)g_pool;
    b->size = MM_SIZE - HDR;
    b->magic = MM_MAGIC;
    b->next = NULL;
    b->free = 1;
    g_free = b;
    g_used = 0;
    g_free_bytes = b->size;
}

static int in_pool(void *p) {
    u8 *q = (u8*)p;
    return q >= g_pool && q < g_pool + MM_SIZE;
}

void *kmalloc(size_t n) {
    if (n == 0) n = 1;
    n = ALIGN16(n);

    /* 空闲链头块一旦损坏（magic 错 / size 为 0），绝不继续在坏链上分配 */
    if (g_free && (g_free->magic != MM_MAGIC || g_free->size == 0)) {
        kprintf("[mm] heap corrupt: free-list head %p size=%u magic=%x (request %u)\n",
                (void*)g_free, g_free->size, g_free->magic, (u32)n);
        return NULL;
    }

    blk_t *prev = NULL, *b = g_free;
    while (b && b->size < n) { prev = b; b = b->next; }
    if (!b) {
        blk_t *h = (blk_t*)g_pool;
        kprintf("[mm] kmalloc(%u) FAILED: g_free=%p size=%u | pool head %p size=%u magic=%x free=%u\n",
                (u32)n, (void*)g_free, g_free ? g_free->size : 0,
                (void*)h, h->size, h->magic, (u32)h->free);
        return NULL;
    }

    /* 够大就分裂出尾部空闲块 */
    if (b->size >= n + HDR + 16) {
        blk_t *nb = (blk_t*)((u8*)b + HDR + n);
        nb->size = b->size - n - HDR;
        nb->magic = MM_MAGIC;
        nb->free = 1;
        nb->next = b->next;
        if (prev) prev->next = nb; else g_free = nb;
        g_free_bytes -= HDR;
        b->size = n;
    } else {
        if (prev) prev->next = b->next; else g_free = b->next;
    }

    b->free = 0;
    b->next = NULL;
    g_used += b->size;
    g_free_bytes -= b->size;
    return (void*)((u8*)b + HDR);
}

void *kzalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, (n + 15u) & ~(size_t)15);
    return p;
}

void *krealloc(void *p, size_t n) {
    if (!p) return kmalloc(n);
    blk_t *b = (blk_t*)((u8*)p - HDR);
    if (b->magic != MM_MAGIC) return NULL;
    if (b->size >= n) return p;               /* 现有空间足够 */
    void *np = kmalloc(n);
    if (!np) return NULL;
    memcpy(np, p, b->size);
    kfree(p);
    return np;
}

void kfree(void *p) {
    if (!p || !in_pool(p)) return;
    blk_t *b = (blk_t*)((u8*)p - HDR);
    if (b->magic != MM_MAGIC) return;
    if (b->free) return;                       /* 重复释放保护 */

    b->free = 1;
    g_used -= b->size;
    g_free_bytes += b->size;

    /* 按地址顺序插入空闲链表 */
    blk_t *prev = NULL, *cur = g_free;
    while (cur && (u8*)cur < (u8*)b) { prev = cur; cur = cur->next; }
    b->next = cur;
    if (prev) prev->next = b; else g_free = b;

    /* 与后继合并 */
    if (b->next && (u8*)b + HDR + b->size == (u8*)b->next) {
        b->size += HDR + b->next->size;
        g_free_bytes += HDR;
        b->next = b->next->next;
    }
    /* 与前驱合并 */
    if (prev && (u8*)prev + HDR + prev->size == (u8*)b) {
        prev->size += HDR + b->size;
        g_free_bytes += HDR;
        prev->next = b->next;
    }
}

u32 mm_used(void)       { return g_used; }
u32 mm_total(void)      { return MM_SIZE; }
u32 mm_free_bytes(void) { return g_free_bytes; }

u32 mm_free_blocks(void) {
    u32 n = 0;
    for (blk_t *b = g_free; b; b = b->next) n++;
    return n;
}

/* 一致性检查：既查空闲链，也把整池按块头物理走一遍。
   后者才是关键——只查链表无法发现「块头被写坏、但链本身看着正常」的情况。 */
int mm_check(void) {
    int errs = 0;
    blk_t *prev = NULL;

    /* 1) 空闲链：magic / free / 在池内 / size 非 0 / 地址递增且不重叠 */
    for (blk_t *b = g_free; b; b = b->next) {
        if (b->magic != MM_MAGIC) { errs++; break; }
        if (!b->free) { errs++; break; }
        if (!in_pool(b)) { errs++; break; }
        if (b->size == 0) { errs++; break; }
        if (prev && (u8*)prev + HDR + prev->size > (u8*)b) { errs++; break; }
        prev = b;
    }
    if (errs) return errs;

    /* 2) 物理遍历：整池必须被「块头 + size」无缝且不越界地铺满，
          且统计出的已用/空闲字节数必须与记账一致 */
    u8 *a = g_pool;
    u32 used = 0, free_bytes = 0;
    while (a + HDR <= g_pool + MM_SIZE) {
        blk_t *b = (blk_t*)a;
        if (b->magic != MM_MAGIC || b->size == 0 ||
            a + HDR + b->size > g_pool + MM_SIZE) { errs++; break; }
        if (b->free) free_bytes += b->size; else used += b->size;
        a += HDR + b->size;
    }
    if (!errs && a != g_pool + MM_SIZE) errs++;
    if (!errs && (used != g_used || free_bytes != g_free_bytes)) errs++;
    return errs;
}

void mm_info(void) {
    kprintf("kernel heap: base %x  total %u bytes  (kernel image ends at %x)\n",
            g_base, MM_SIZE, (u32)__kernel_end);
    kprintf("  used %u bytes  free %u bytes  free blocks %u  consistency %s\n",
            g_used, g_free_bytes, mm_free_blocks(),
            mm_check() == 0 ? "OK" : "ERROR");
    int n = 0;
    for (blk_t *b = g_free; b && n < 4; b = b->next, n++) {
        kprintf("    free blk %d: addr %x  size %u  magic %x\n",
                n, (u32)(size_t)b, b->size, b->magic);
        if (b->next == b) { kprintf("    (SELF-LOOP!)\n"); break; }
    }
}
