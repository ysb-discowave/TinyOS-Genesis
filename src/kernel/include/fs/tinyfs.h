#ifndef TINYOS_TINYFS_H
#define TINYOS_TINYFS_H

#include "types.h"

/* ============================================================
 * TinyFS —— TinyOS 磁盘文件系统（持久化存储）
 * ------------------------------------------------------------
 * 磁盘布局（每块 = 1 扇区 = 512 字节），基址为 base_lba：
 *   块 0            超级块
 *   块 1 .. 1+B-1   块位图（1 位/块，1 = 已用）
 *   ...             ！目录表（定长表项，路径为绝对路径）
 *   ... 末尾        数据区（文件按连续块分配）
 *
 * base_lba 的意义：安装到硬盘时 LBA 0 必须是引导扇区、LBA 1..K 是内核镜像，
 * 文件系统不能从 0 开始，否则格式化会把引导代码覆盖掉。
 * 默认 TFS_DEFAULT_BASE_LBA = 2048（1 MiB 对齐，和常见分区习惯一致）。
 *
 * 目录表项固定 128 字节，最多 128 个条目（文件或目录）；
 * 目录本身不占数据块，仅由路径前缀表达从属关系。
 * 所有修改立即回写（write-through），掉电后仍可读。
 * ============================================================ */

#define TFS_ENT_SIZE   128
#define TFS_NAME_MAX   108
#define TFS_MAXFILES   128

#define TFS_DEFAULT_BASE_LBA 2048u

typedef struct __attribute__((packed)) {
    char magic[8];          /* "TINYFS01" */
    u32  version;
    u32  block_size;
    u32  total_blocks;
    u32  bitmap_start;
    u32  bitmap_blocks;
    u32  table_start;
    u32  table_blocks;
    u32  data_start;
    u32  free_blocks;
    u32  file_count;
    u32  base_lba;          /* 文件系统在磁盘上的起始 LBA */
    u32  reserved[99];
} tfs_super_t;

typedef struct __attribute__((packed)) {
    u8   used;
    u8   type;              /* 0 = 文件, 1 = 目录 */
    u8   namelen;
    u8   rsv;
    u32  first;             /* 起始块 */
    u32  size;              /* 字节数 */
    char name[TFS_NAME_MAX];/* 绝对路径 */
    u8   pad[TFS_ENT_SIZE - 12 - TFS_NAME_MAX];
} tfs_ent_t;

#define TFS_TYPE_FILE 0
#define TFS_TYPE_DIR  1

/* 设置文件系统基址（必须在 tfs_init/tfs_format 之前调用） */
void tfs_set_base(u32 lba);
u32  tfs_base(void);

/* 设置分区大小上限（块数）：0 = 一直用到盘尾。
 * 安装向导自定义分区时用，避免把用户留出来的空间也划进文件系统。 */
void tfs_set_limit(u32 blocks);
u32  tfs_limit(void);

/* 返回：0 = 挂载已有文件系统；1 = 新建（已格式化）；-1 = 无磁盘或失败 */
int  tfs_init(void);
/* 同上，但 allow_format = 0 时绝不格式化（安装介质：不许擅自动目标盘） */
int  tfs_init2(int allow_format);
int  tfs_format(void);
int  tfs_mounted(void);

/* 写入（覆盖）文件或登记目录；返回 0 = 成功 */
int  tfs_write(const char *path, u8 type, const u8 *data, u32 size);
/* 读取文件到新分配的缓冲区（调用者负责 kfree）；返回 0 = 成功 */
int  tfs_read(const char *path, u8 **out, u32 *size);
/* 删除文件或目录（目录会连同其下所有条目一起删除） */
int  tfs_delete(const char *path);
int  tfs_exists(const char *path);
/* 列出全部条目（用于启动时装入 VFS） */
void tfs_list(void (*cb)(const char *path, u8 type, u32 size, void *arg), void *arg);

u32  tfs_total_blocks(void);
u32  tfs_free_blocks(void);
u32  tfs_file_count(void);
u32  tfs_capacity_kb(void);
void tfs_info(void);

#endif
