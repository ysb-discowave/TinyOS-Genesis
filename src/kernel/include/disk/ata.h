#ifndef TINYOS_ATA_H
#define TINYOS_ATA_H

#include "types.h"

/* ============================================================
 * ATA PIO 磁盘驱动（LBA28）
 * ------------------------------------------------------------
 * 支持两条总线（primary 0x1F0 / secondary 0x170）各两个设备。
 * ata_init() 扫描并选中第一块“已连接且非引导盘”的硬盘作为数据盘；
 * 若只有引导盘存在，则退回选择引导盘（便于单盘场景）。
 * ============================================================ */

#define ATA_SECTOR_SIZE 512
#define ATA_MAX_SECTORS 255          /* 单次传输上限（8 位扇区计数） */

int  ata_init(void);                 /* 返回 0 = 找到可用磁盘，-1 = 无 */
int  ata_present(void);

u32  ata_sectors(void);              /* 选中磁盘的总扇区数 */
const char *ata_model(void);         /* 型号字符串 */
int  ata_is_boot_disk(void);         /* 选中的是否引导盘 */

int  ata_read (u32 lba, u32 count, void *buf);   /* 0 = 成功 */
int  ata_write(u32 lba, u32 count, const void *buf);
void ata_info(void);                             /* 打印磁盘信息 */

/* 引导扇区通过 0x500 处的小结构告知内核：引导盘号 */
typedef struct {
    u32 magic;        /* 'TINY' = 0x54494E59 */
    u8  boot_drive;   /* BIOS 驱动器号，0x80 = 第一块硬盘 */
} boot_info_t;

#define BOOT_INFO_ADDR 0x500u

#endif
