#ifndef TINYOS_DISK_H
#define TINYOS_DISK_H

#include "types.h"

/* ============================================================
 * disk —— 磁盘抽象层（多盘注册表）
 * ------------------------------------------------------------
 * 每种控制器驱动（ATA PIO、将来的 AHCI/NVMe/软驱...）把自己
 * 枚举到的每一块盘注册进来：ops 里带 read/write，驱动私有
 * 状态放在 drv 指针里。注册后：
 *   - 所有使用者（TinyFS、安装器、mbr、shell）拿 disk_t* 操作
 *     指定磁盘，不再依赖"全局选中的一块盘"；
 *   - 保留一个"默认盘"指针（disk_default），旧的 ata_read 等
 *     无参 API 全部转发到它，单盘用法不用改。
 *
 * 磁盘名由注册方给定（ATA 用 "ata0".."ata3"，按总线+主从顺序）。
 * 以后加 AHCI 驱动就注册 "ahci0" 之类，disk 命令会自动列出。
 * ============================================================ */

#define DISK_MAX          16
#define DISK_NAME_MAX     16

typedef struct disk disk_t;

typedef struct {
    int  (*read)(void *drv, u32 lba, u32 count, void *buf);
    int  (*write)(void *drv, u32 lba, u32 count, const void *buf);
} disk_ops_t;

struct disk {
    int           in_use;
    char          name[DISK_NAME_MAX];  /* 唯一名，如 "ata0" */
    char          model[41];            /* IDENTIFY 出来的型号 */
    char          loc[24];              /* 位置描述，如 "primary master" */
    u32           sectors;              /* 总扇区数（LBA28 上限 2^28） */
    int           is_boot;              /* BIOS 报告的引导盘（若可知） */
    const disk_ops_t *ops;
    void          *drv;                 /* 驱动私有数据 */
};

/* 注册一块盘；满或失败返回 -1，成功返回盘号（0..N-1） */
int    disk_register(const disk_ops_t *ops, void *drv,
                     const char *name, const char *loc,
                     const char *model, u32 sectors, int is_boot);

int    disk_count(void);
disk_t *disk_get(int idx);              /* 越界返回 NULL */
int    disk_find_name(const char *name);/* 按名找盘号，找不到 -1 */

disk_t *disk_default(void);
void    disk_set_default(disk_t *d);    /* 重定向"默认盘"（安装选盘用） */

/* 便捷访问 */
int    disk_read(disk_t *d, u32 lba, u32 count, void *buf);
int    disk_write(disk_t *d, u32 lba, u32 count, const void *buf);
u32    disk_sectors(disk_t *d);

/* 列出全部磁盘（disk 命令用） */
void   disk_list(void);

#endif
