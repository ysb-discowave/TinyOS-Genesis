#ifndef TINYOS_VFS_H
#define TINYOS_VFS_H

#include "types.h"

typedef enum { VFS_FILE = 0, VFS_DIR = 1 } vfs_type;

typedef struct vfs_node {
    char name[64];
    vfs_type type;
    u8  *data;
    u32  size;
    u8   on_disk;                /* 已持久化到 TinyFS */
    u16  owner;                  /* 所有者 uid */
    u16  mode;                   /* 权限位（八进制 0..0777） */
    struct vfs_node *parent;
    struct vfs_node *children;   /* 子节点链表头 */
    struct vfs_node *next;       /* 同级下一个 */
} vfs_node_t;

/* 权限：读取/设置节点所有者与权限位。路径不存在时返回 -1。 */
int  vfs_get_owner(const char *path, int *uid, int *mode);
int  vfs_set_owner(const char *path, int uid, int mode);

void    vfs_init(void);
void    vfs_load_romfs(const u8 *data, u32 len);

vfs_node_t *vfs_root(void);
vfs_node_t *vfs_resolve(const char *path);
int     vfs_mkdir(const char *path);
int     vfs_write_file(const char *path, const u8 *data, u32 size);
const u8 *vfs_read_file(const char *path, u32 *size);
int     vfs_delete(const char *path);
void    vfs_list(const char *path, void (*cb)(const char *name, vfs_type t, void *arg), void *arg);

/* 递归遍历（path 为绝对路径）；size 为文件字节数 */
void    vfs_walk(void (*cb)(const char *path, vfs_type t, const u8 *data, u32 size, void *arg),
                 void *arg);

/* 持久化：把当前 VFS 全部内容写入磁盘（首次格式化后使用） */
int     vfs_install_disk(void);
/* 持久化（按组件过滤）：keep 返回 0 的路径不写盘；安装向导用。
 * 返回写入的条目数。 */
int     vfs_install_disk_filter(int (*keep)(const char *path, void *arg), void *arg);
/* 持久化：从磁盘读入全部条目，覆盖/新建到 VFS */
int     vfs_load_disk(void);
/* 打开/关闭「修改即落盘」模式（装载 romfs 基线时临时关闭） */
void    vfs_persist_enable(int on);

#endif
