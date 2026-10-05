#ifndef TINYOS_INSTIMG_H
#define TINYOS_INSTIMG_H

#include "types.h"

/* ============================================================
 * 内嵌的安装载荷
 * ------------------------------------------------------------
 * 安装程序要把 TinyOS 写到硬盘上并使它可引导，因此内核必须携带：
 *   boot_img   512 字节引导扇区（boot/boot.asm 汇编产物）
 *   kernel_img 扁平内核映像（objcopy -O binary 的产物，装载到 kernel_img_load）
 *
 * 这两个数组由 tools/gen_instimg.py 生成到 kernel/instimg.c。
 * 构建时先链接一个"载荷内核"（不含生成的 instimg.c，而含 instimg_stub.c），
 * 再把这个载荷内核本身嵌进正式内核 —— 见 kernel/instimg_stub.c 的说明。
 * ============================================================ */

extern const u8  boot_img[];
extern const u32 boot_img_len;

extern const u8  kernel_img[];
extern const u32 kernel_img_len;
extern const u32 kernel_img_load;      /* 内核链接/装载地址（0x100000） */

/* 安装载荷在磁盘上的布局 */
#define INST_BOOT_LBA     0u           /* 引导扇区 */
#define INST_KERNEL_LBA   1u           /* 内核映像紧跟其后 */
#define INST_FS_LBA       2048u        /* 文件系统基址（1 MiB 对齐） */

/* 安装程序入口（shell 的 install 命令调用） */
int installer_run(const char *arg);

/* 已安装服务的开关：读 /etc/sshd.conf 的 autostart 字段。
 * 由安装程序写入，shell 在登录后据此决定是否自动拉起 SSH 服务。 */
int sshd_autostart(void);

#endif
