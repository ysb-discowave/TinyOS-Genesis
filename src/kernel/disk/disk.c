#include "disk/disk.h"
#include "libc.h"
#include "console.h"

static disk_t g_disks[DISK_MAX];
static disk_t *g_default = NULL;

int disk_register(const disk_ops_t *ops, void *drv,
                  const char *name, const char *loc,
                  const char *model, u32 sectors, int is_boot) {
    if (!ops || !ops->read || !ops->write || !name) return -1;
    for (int i = 0; i < DISK_MAX; i++) {
        if (g_disks[i].in_use) continue;
        disk_t *d = &g_disks[i];
        d->in_use  = 1;
        strncpy(d->name, name, DISK_NAME_MAX - 1); d->name[DISK_NAME_MAX - 1] = 0;
        d->model[0] = 0;
        if (model && model[0]) {
            strncpy(d->model, model, 40); d->model[40] = 0;
        }
        if (loc && loc[0]) {
            strncpy(d->loc, loc, sizeof(d->loc) - 1);
            d->loc[sizeof(d->loc) - 1] = 0;
        } else d->loc[0] = 0;
        d->sectors = sectors;
        d->is_boot = is_boot;
        d->ops     = ops;
        d->drv     = drv;
        /* 第一块盘设为默认盘，保持"没有默认盘概念"的旧行为 */
        if (!g_default) g_default = d;
        return i;
    }
    return -1;
}

int disk_count(void) {
    int n = 0;
    for (int i = 0; i < DISK_MAX; i++) if (g_disks[i].in_use) n++;
    return n;
}

disk_t *disk_get(int idx) {
    if (idx < 0 || idx >= DISK_MAX || !g_disks[idx].in_use) return NULL;
    return &g_disks[idx];
}

int disk_find_name(const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < DISK_MAX; i++)
        if (g_disks[i].in_use && strcmp(g_disks[i].name, name) == 0) return i;
    return -1;
}

disk_t *disk_default(void) { return g_default; }

void disk_set_default(disk_t *d) {
    g_default = d;
    if (d) kprintf("[disk] default disk -> %s (%s)\n", d->name,
                   d->model[0] ? d->model : "unknown model");
}

int disk_read(disk_t *d, u32 lba, u32 count, void *buf) {
    if (!d || !d->in_use || !d->ops || !d->ops->read) return -1;
    return d->ops->read(d->drv, lba, count, buf);
}

int disk_write(disk_t *d, u32 lba, u32 count, const void *buf) {
    if (!d || !d->in_use || !d->ops || !d->ops->write) return -1;
    return d->ops->write(d->drv, lba, count, buf);
}

u32 disk_sectors(disk_t *d) { return d ? d->sectors : 0; }

void disk_list(void) {
    int n = disk_count();
    if (!n) { kprintf("No disks registered.\n"); return; }
    kprintf("%d disk(s):\n", n);
    for (int i = 0; i < DISK_MAX; i++) {
        disk_t *d = &g_disks[i];
        if (!d->in_use) continue;
        kprintf("  [%d] %-6s  %-22s  %-14s  %u sectors (%u MB)%s%s\n",
                i, d->name, d->model[0] ? d->model : "(unknown)",
                d->loc[0] ? d->loc : "-",
                d->sectors, d->sectors / 2048,
                d->is_boot ? "  [BIOS boot disk]" : "",
                (d == g_default) ? "  [default]" : "");
    }
}
