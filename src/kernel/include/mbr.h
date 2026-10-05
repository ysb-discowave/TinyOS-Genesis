#ifndef TINYOS_MBR_H
#define TINYOS_MBR_H

#include "types.h"
#include "disk/disk.h"

/* ============================================================
 * MBR 分区表（扇区 0 的 0x1BE..0x1FD）
 * ------------------------------------------------------------
 * 引导扇区只有 512 字节，其中最后 66 字节是"签名 + 分区表"，
 * 所以 boot/boot.asm 的代码和数据总共只有 446 字节可用。
 * 0x1BE 起是 4 个标准 16 字节分区表项：
 *   +0x0  u8  状态（0x80 = 可引导）
 *   +0x1  3B  起始 CHS（我们一律留 0，只用 LBA）
 *   +0x4  u8  分区类型
 *   +0x5  3B  结束 CHS（同上留 0）
 *   +0x8  u32 起始 LBA
 *   +0xC  u32 扇区数
 *
 * 为什么要真的写一张标准分区表：
 *   1) 装系统时用户可以自定义文件系统分区的位置和大小，
 *      这张表就是唯一的持久化记录；
 *   2) 内核开机时读扇区 0 解析它，才知道 TinyFS 在哪个 LBA ——
 *      否则自定义分区会在默认 LBA 2048 上被重新格式化成一个空文件系统；
 *   3) 它是标准 MBR，别的系统（fdisk / 磁盘工具）也能认出这块盘的布局。
 *
 * 分区类型号挑了 0x7E / 0x7F（MBR 里这一带各家自用，撞车概率低）。
 * ============================================================ */

#define MBR_PART_OFFSET  0x1BEu
#define MBR_PART_COUNT   4
#define MBR_PART_SIZE    16

#define PART_TYPE_TINYOS_SYS  0x7Eu   /* TinyOS 内核分区   */
#define PART_TYPE_TINYOS_FS   0x7Fu   /* TinyFS 文件系统分区 */

/* 扇区是不是一张有效的 MBR（看 0xAA55 签名） */
int  mbr_valid(const u8 *sec);

/* 在一张已读入内存的 MBR 里写 / 找分区表项。返回 0 = 未找到。 */
void mbr_set(u8 *sec, int idx, u8 type, u32 lba, u32 secs, int active);
int  mbr_find(const u8 *sec, u8 type, u32 *lba, u32 *secs);
void mbr_clear(u8 *sec);

/* 直接从磁盘 0 号扇区读分区表，找 TinyFS 分区；返回 0 = 没找到（用默认基址）
 * d = NULL 时用默认盘；mbr_lookup = mbr_lookup2(NULL, ...) */
int  mbr_lookup2(disk_t *d, u8 type, u32 *lba, u32 *secs);
int  mbr_lookup(u8 type, u32 *lba, u32 *secs);

/* 打印指定磁盘的分区表（d = NULL 时用默认盘） */
void mbr_print2(disk_t *d);
void mbr_print(void);

#endif
