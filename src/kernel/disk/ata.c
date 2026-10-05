#include "disk/ata.h"
#include "disk/disk.h"
#include "io.h"
#include "libc.h"
#include "console.h"

/* ============================================================
 * ATA PIO 磁盘驱动（LBA28，多盘）
 * ------------------------------------------------------------
 * 两条总线（primary 0x1F0 / secondary 0x170）各挂两个设备
 * （master/slave），共最多 4 块盘。ata_init() 枚举全部设备，
 * 把每一块识别出来的盘注册进磁盘抽象层（disk.h），名字
 * ata0..ata3，默认盘选择逻辑保持旧版行为：优先"非引导盘"，
 * 只有引导盘时退回引导盘（单盘场景）。
 *
 * 每一块盘有独立的寄存器状态（io 基址 / 选择字节），互不干扰。
 * ============================================================ */

#define R_DATA   0
#define R_ERR    1
#define R_FEAT   1
#define R_SECC   2
#define R_LBA0   3
#define R_LBA1   4
#define R_LBA2   5
#define R_HD     6
#define R_CMD    7
#define R_STAT   7

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define CMD_READ  0x20
#define CMD_WRITE 0x30
#define CMD_IDENT 0xEC

#define ATA_NDEVS 4

/* 单台设备的全部状态。disk 层的 drv 指针就是它的地址。 */
typedef struct {
    u16 io;       /* 数据口基址：0x1F0 primary / 0x170 secondary */
    u16 ctrl;     /* 控制口（alt status）：0x3F6 / 0x376 */
    u8  sel;      /* 设备选择字节基值：0xE0 master / 0xF0 slave */
    u8  bus;      /* 0 = primary, 1 = secondary */
    u8  dev;      /* 0 = master,  1 = slave */
    u32 sectors;  /* LBA28 总扇区数 */
    int present;
} ata_dev_t;

static ata_dev_t g_devs[ATA_NDEVS];

static void ata_delay400(ata_dev_t *d) { for (int i = 0; i < 4; i++) inb(d->ctrl); }

static int wait_bsy(ata_dev_t *d) {
    for (u32 t = 0; t < 2000000u; t++)
        if (!(inb(d->io + R_STAT) & ST_BSY)) return 0;
    return -1;
}

static int wait_drq(ata_dev_t *d) {
    for (u32 t = 0; t < 2000000u; t++) {
        u8 s = inb(d->io + R_STAT);
        if (s & ST_ERR) return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ)) return 0;
    }
    return -1;
}

/* 对某台设备发 IDENTIFY，成功返回 0 并填 sectors/model */
static int identify(ata_dev_t *d, u32 *sectors, char model[41]) {
    outb(d->io + R_HD, d->sel);
    ata_delay400(d);
    outb(d->io + R_SECC, 0);
    outb(d->io + R_LBA0, 0);
    outb(d->io + R_LBA1, 0);
    outb(d->io + R_LBA2, 0);
    outb(d->io + R_CMD, CMD_IDENT);
    ata_delay400(d);

    u8 st = inb(d->io + R_STAT);
    if (st == 0) return -1;                 /* 无设备 */
    if (wait_bsy(d) != 0) return -1;
    if (inb(d->io + R_LBA1) != 0 || inb(d->io + R_LBA2) != 0) return -1;  /* 非 ATA */
    if (wait_drq(d) != 0) return -1;

    u16 id[256];
    insw(d->io + R_DATA, id, 256);
    ata_delay400(d);

    /* words 60..61 = LBA28 总扇区数 */
    *sectors = ((u32)id[61] << 16) | id[60];
    /* words 27..46 = 型号（大端字序） */
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--) model[i] = 0;
    return (*sectors > 0) ? 0 : -1;
}

/* 把 BIOS 引导盘号映射为 (总线, 设备) */
static void boot_drive_bus_dev(u8 drv, int *bus, int *dev) {
    u8 n = drv >= 0x80 ? (u8)(drv - 0x80) : 0xFF;
    if (n <= 3) { *bus = n >> 1; *dev = n & 1; }
    else        { *bus = -1; *dev = -1; }
}

/* ---- disk_ops 实现（drv = ata_dev_t*） ---- */

static int setup_lba(ata_dev_t *d, u32 lba, u32 count) {
    if (!d->present) return -1;
    if (lba >= 0x10000000u) return -1;                 /* LBA28 上限 */
    if (count == 0 || count > ATA_MAX_SECTORS) return -1;

    if (wait_bsy(d) != 0) return -1;

    outb(d->io + R_HD, (u8)(d->sel | (u8)((lba >> 24) & 0x0F)));
    ata_delay400(d);
    outb(d->io + R_FEAT, 0);
    outb(d->io + R_SECC, (u8)count);
    outb(d->io + R_LBA0, (u8)(lba & 0xFF));
    outb(d->io + R_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(d->io + R_LBA2, (u8)((lba >> 16) & 0xFF));
    return 0;
}

static int ata_hw_read(void *drv, u32 lba, u32 count, void *buf) {
    ata_dev_t *d = (ata_dev_t*)drv;
    u8 *p = (u8*)buf;
    while (count) {
        u32 n = count > ATA_MAX_SECTORS ? ATA_MAX_SECTORS : count;
        if (setup_lba(d, lba, n) != 0) {
            kprintf("[ata] read: setup_lba(%u,%u) FAILED status=0x%02X\n",
                    lba, n, inb(d->io + R_STAT));
            return -1;
        }
        outb(d->io + R_CMD, CMD_READ);
        ata_delay400(d);

        for (u32 s = 0; s < n; s++) {
            if (wait_bsy(d) != 0) {
                kprintf("[ata] read: BSY timeout lba=%u s=%u status=0x%02X\n",
                        lba, s, inb(d->io + R_STAT));
                return -1;
            }
            if (wait_drq(d) != 0) {
                kprintf("[ata] read: DRQ timeout lba=%u s=%u status=0x%02X error=0x%02X\n",
                        lba, s, inb(d->io + R_STAT), inb(d->io + R_ERR));
                return -1;
            }
            insw(d->io + R_DATA, p, 256);
            p += 512;
        }
        ata_delay400(d);
        lba += n;
        count -= n;
    }
    return 0;
}

static int ata_hw_write(void *drv, u32 lba, u32 count, const void *buf) {
    ata_dev_t *d = (ata_dev_t*)drv;
    const u8 *p = (const u8*)buf;
    while (count) {
        u32 n = count > ATA_MAX_SECTORS ? ATA_MAX_SECTORS : count;
        if (setup_lba(d, lba, n) != 0) return -1;
        outb(d->io + R_CMD, CMD_WRITE);
        ata_delay400(d);

        for (u32 s = 0; s < n; s++) {
            if (wait_bsy(d) != 0) {
                kprintf("[ata] write: BSY timeout lba=%u s=%u status=0x%02X\n",
                        lba, s, inb(d->io + R_STAT));
                return -1;
            }
            if (wait_drq(d) != 0) {
                kprintf("[ata] write: DRQ timeout lba=%u s=%u status=0x%02X error=0x%02X\n",
                        lba, s, inb(d->io + R_STAT), inb(d->io + R_ERR));
                return -1;
            }
            outsw(d->io + R_DATA, p, 256);
            p += 512;
            ata_delay400(d);
        }
        /* 等待写入完成 */
        if (wait_bsy(d) != 0) return -1;
        outb(d->io + R_CMD, 0xE7);           /* FLUSH CACHE */
        if (wait_bsy(d) != 0) return -1;
        lba += n;
        count -= n;
    }
    return 0;
}

static const disk_ops_t g_ata_ops = { ata_hw_read, ata_hw_write };

int ata_init(void) {
    for (int i = 0; i < ATA_NDEVS; i++) {
        g_devs[i].bus = (u8)(i >> 1);
        g_devs[i].dev = (u8)(i & 1);
        g_devs[i].io    = g_devs[i].bus ? 0x170 : 0x1F0;
        g_devs[i].ctrl  = g_devs[i].bus ? 0x376 : 0x3F6;
        g_devs[i].sel   = g_devs[i].dev ? 0xF0 : 0xE0;
        g_devs[i].sectors = 0;
        g_devs[i].present = 0;
    }

    int bbus = -1, bdev = -1;
    boot_info_t *bi = (boot_info_t*)BOOT_INFO_ADDR;
    if (bi->magic == 0x54494E59u) boot_drive_bus_dev(bi->boot_drive, &bbus, &bdev);

    int n = 0, bidx = -1;
    for (int i = 0; i < ATA_NDEVS; i++) {
        char model[41] = "";
        u32 sec = 0;
        if (identify(&g_devs[i], &sec, model) != 0) continue;
        g_devs[i].sectors = sec;
        g_devs[i].present = 1;
        n++;
        if (i == ((bbus >= 0 ? bbus : 0) * 2 + (bdev >= 0 ? bdev : 0))) bidx = i;
        char name[8], loc[24];
        snprintf(name, sizeof(name), "ata%d", i);
        snprintf(loc, sizeof(loc), "%s %s",
                 g_devs[i].bus ? "secondary" : "primary",
                 g_devs[i].dev ? "slave" : "master");
        if (disk_register(&g_ata_ops, &g_devs[i], name, loc, model, sec,
                          (i == bidx)) < 0)
            kprintf("[ata] register %s failed (disk table full?)\n", name);
    }

    if (n == 0) return -1;

    /* 默认盘选择沿用旧行为：优先非引导盘（把引导盘留给安装介质场景），
     * 只有引导盘存在时再用它。 */
    disk_t *def = NULL;
    for (int i = 0; i < ATA_NDEVS; i++) {
        disk_t *d = disk_get(i);
        if (!d || !g_devs[i].present) continue;
        if (!d->is_boot) { def = d; break; }
    }
    if (!def && bidx >= 0) def = disk_get(bidx);
    if (def) disk_set_default(def);
    return 0;
}

/* ---- 兼容 API：全部转发到默认盘 ---- */

int ata_present(void) { return disk_default() != NULL; }
u32 ata_sectors(void) { return disk_sectors(disk_default()); }
const char *ata_model(void) {
    disk_t *d = disk_default();
    return d ? d->model : "";
}
int ata_is_boot_disk(void) {
    disk_t *d = disk_default();
    return d ? d->is_boot : 0;
}
int ata_read(u32 lba, u32 count, void *buf) {
    return disk_read(disk_default(), lba, count, buf);
}
int ata_write(u32 lba, u32 count, const void *buf) {
    return disk_write(disk_default(), lba, count, buf);
}

void ata_info(void) {
    if (!disk_default()) { kprintf("ATA: no disk detected\n"); return; }
    disk_t *d = disk_default();
    kprintf("ATA disk: %s\n", d->model[0] ? d->model : "(unknown model)");
    kprintf("  position: %s   sectors: %u   size: %u KB%s\n",
            d->loc[0] ? d->loc : "-",
            d->sectors, d->sectors / 2,
            d->is_boot ? "  (boot disk)" : "  (data disk)");
}
