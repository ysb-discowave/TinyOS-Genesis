#include "mbr.h"
#include "disk/disk.h"
#include "console.h"
#include "libc.h"

int mbr_valid(const u8 *sec) {
    if (!sec) return 0;
    return (sec[510] == 0x55 && sec[511] == 0xAA);
}

void mbr_clear(u8 *sec) {
    if (!sec) return;
    memset(sec + MBR_PART_OFFSET, 0, MBR_PART_COUNT * MBR_PART_SIZE);
}

void mbr_set(u8 *sec, int idx, u8 type, u32 lba, u32 secs, int active) {
    if (!sec || idx < 0 || idx >= MBR_PART_COUNT) return;
    u8 *e = sec + MBR_PART_OFFSET + (u32)idx * MBR_PART_SIZE;
    memset(e, 0, MBR_PART_SIZE);
    e[0] = active ? 0x80 : 0x00;
    e[4] = type;
    /* CHS 三个字节留 0：LBA28 时代没人再算柱面/磁头，
       真要填还得先问 BIOS 磁盘几何参数，填错了反而误导工具。 */
    memcpy(e + 8,  &lba,  4);
    memcpy(e + 12, &secs, 4);
}

int mbr_find(const u8 *sec, u8 type, u32 *lba, u32 *secs) {
    if (!mbr_valid(sec)) return 0;
    for (int i = 0; i < MBR_PART_COUNT; i++) {
        const u8 *e = sec + MBR_PART_OFFSET + (u32)i * MBR_PART_SIZE;
        if (e[4] != type) continue;
        u32 l = 0, n = 0;
        memcpy(&l, e + 8, 4);
        memcpy(&n, e + 12, 4);
        if (n == 0) continue;
        if (lba)  *lba  = l;
        if (secs) *secs = n;
        return 1;
    }
    return 0;
}

/* d = NULL 时用默认盘（保持旧的无参行为） */
int mbr_lookup2(disk_t *d, u8 type, u32 *lba, u32 *secs) {
    d = d ? d : disk_default();
    if (!d) return 0;
    static u8 sec[512];
    if (disk_read(d, 0, 1, sec) != 0) return 0;
    return mbr_find(sec, type, lba, secs);
}

int mbr_lookup(u8 type, u32 *lba, u32 *secs) {
    return mbr_lookup2(NULL, type, lba, secs);
}

static const char *type_name(u8 t) {
    switch (t) {
    case PART_TYPE_TINYOS_SYS: return "TinyOS kernel";
    case PART_TYPE_TINYOS_FS:  return "TinyFS filesystem";
    default:                   return "unknown";
    }
}

/* d = NULL 时用默认盘 */
void mbr_print2(disk_t *d) {
    d = d ? d : disk_default();
    if (!d) { kprintf("part: no disk\n"); return; }
    static u8 sec[512];
    if (disk_read(d, 0, 1, sec) != 0) {
        kprintf("part: cannot read sector 0 of %s\n", d->name);
        return;
    }
    if (!mbr_valid(sec)) { kprintf("part: no MBR signature on %s\n", d->name); return; }

    kprintf("partition table (LBA 0 + 0x%X), disk %s %u sectors:\n",
            MBR_PART_OFFSET, d->name, d->sectors);
    int n = 0;
    for (int i = 0; i < MBR_PART_COUNT; i++) {
        const u8 *e = sec + MBR_PART_OFFSET + (u32)i * MBR_PART_SIZE;
        u32 l = 0, c = 0;
        memcpy(&l, e + 8, 4);
        memcpy(&c, e + 12, 4);
        if (c == 0) continue;
        n++;
        kprintf("  %d) %-18s type 0x%02X  LBA %-8u .. %-8u  %u sectors (%u MB)%s\n",
                i + 1, type_name(e[4]), e[4], l, l + c - 1, c, c / 2048,
                (e[0] & 0x80) ? "  [active]" : "");
    }
    if (!n) kprintf("  (no partitions defined)\n");
}

void mbr_print(void) { mbr_print2(NULL); }
