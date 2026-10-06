#ifndef TINYOS_NIC_H
#define TINYOS_NIC_H

#include "types.h"

/* 网卡驱动注册表 —— 与磁盘层 disk.c 的 disk_register() 同一套思路。
 *
 * 背景：所有 TNCR 无关，这里只管内核网卡。net.c 只跟本文件抽象打交道，
 * 因此新增一个网卡型号 = 新写一个驱动 + nic_register()，**不用改 net.c**。
 *
 * 每个驱动自己负责：PCI 枚举 -> 使能总线主控 -> 建环 -> 读 MAC。
 * 驱动之间互不知晓，注册顺序即探测优先级，第一个 init() 返回 0 的成为活动网卡。 */

#define NIC_MAX 4

typedef struct nic_driver {
    const char *name;              /* 用户可见名（ASCII，<80 列） */
    const char *model;             /* 型号/描述，init() 里可按 devid 改写（指向驱动内静态缓冲） */
    u16         vendor;            /* PCI vendor id */
    const u16  *devids;            /* 支持的 device id 列表（以 0 结尾） */
    int  (*init)(void);            /* 探测+初始化；成功返回 0，失败非 0 */
    void (*send)(const u8 *frame, u32 len);          /* 发一帧（含 MAC 头） */
    int  (*poll)(void (*cb)(const u8 *frame, u32 len)); /* 收帧轮询，返回帧数 */
    void (*get_mac)(u8 mac[6]);
} nic_driver_t;

/* 每个驱动内嵌一块缓冲，init() 按命中的 device id 填型号，net info 会显示 */
#define NIC_MODEL_MAX 40

/* 注册一个驱动（存副本）。满表/参数不全返回 -1。 */
int  nic_register(const nic_driver_t *drv);
/* 按注册顺序探测，激活第一个成功的。0 = 有可用网卡。 */
int  nic_init(void);
int  nic_count(void);                  /* 已注册驱动数 */
const char *nic_name(void);            /* 当前活动驱动名（无则 NULL） */
const char *nic_model(void);           /* 当前活动驱动型号（无则 NULL） */
void nic_send(const u8 *frame, u32 len);
int  nic_poll(void (*cb)(const u8 *frame, u32 len));
void nic_get_mac(u8 mac[6]);
void nic_list(void);                   /* 列出全部已注册驱动 + 活动者 */

/* ---- 各网卡驱动共用的小工具（放这里避免每个驱动重复实现） ---- */
u32  pci_cfg_read(u8 bus, u8 dev, u8 fn, u8 off);
void pci_cfg_write(u8 bus, u8 dev, u8 fn, u8 off, u32 val);
/* 在总线上找 vendor+devids[]（0 结尾）匹配的设备；找到填 bus/dev/fn 返回 0 */
int  pci_find(u16 vendor, const u16 *devids, u8 *bus, u8 *dev, u8 *fn);
/* 使能 I/O + bus master + memory space */
void pci_enable(u8 bus, u8 dev, u8 fn);
/* 取 BAR 基址；bar=0..5。返回 0 表示该 BAR 未实现 */
u32  pci_bar(u8 bus, u8 dev, u8 fn, u8 bar);
/* BAR 是内存空间还是 IO 空间 */
int  pci_bar_is_mem(u8 bus, u8 dev, u8 fn, u8 bar);
/* 找到第一个 I/O 空间 BAR 的基址（0..5 全扫）。i8255x 的端口在 BAR1 而非 BAR0，
 * 所以不能假定第一个 BAR。找不到返回 0。 */
u32  pci_find_io_bar(u8 bus, u8 dev, u8 fn);
/* 找到第一个内存空间 BAR 的基址（0..5 全扫）。找不到返回 0。 */
u32  pci_find_mem_bar(u8 bus, u8 dev, u8 fn);

#endif /* TINYOS_NIC_H */
