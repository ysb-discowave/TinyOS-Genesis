/* ============================================================================
 * kernel_api.h -- tinysh 后端无关接口
 * ----------------------------------------------------------------------------
 * tinysh.c 只做命令解析，所有底层能力（文件系统 / 进程 / 硬件 / 系统信息 /
 * 行输入）都通过这个头声明的函数访问。有两种后端实现：
 *   kernel_api_host.c   —— 宿主机（gcc，标准 libc），便于离线冒烟测试
 *   kernel_api_tinyos.c —— TinyOS 2.0 用户态（TNCR，链接 lua_shim.c 的裸机 libc）
 * 二者都实现本头声明的全部函数，tinysh.c 不直接依赖任一。
 * ============================================================================ */
#ifndef KERNEL_API_H
#define KERNEL_API_H

/* 目录项（fs_list 填充） */
typedef struct {
    char  name[64];
    int   is_dir;     /* 1 = 目录，0 = 普通文件 */
    long  size;       /* 文件字节数 */
} fs_entry;

/* 硬件设备项（hw_devlist 填充） */
typedef struct {
    char            path[40];   /* 设备名，如 "com1" / "pit" */
    unsigned short  io_base;    /* IO 基址 */
    unsigned short  io_size;    /* IO 窗口字节数 */
} hw_dev;

/* ---- 行输入：返回读取长度，EOF 返回 -1 ---- */
int ksh_readline(char *buf, int n);

/* ---- 系统信息 ---- */
void sys_version(char *buf, int n);
void sys_sysinfo(char *buf, int n);
void sys_date(char *buf, int n);

/* ---- 文件系统 ---- */
int  fs_list(const char *path, fs_entry **out, int *n);
int  fs_chdir(const char *path);
int  fs_getcwd(char *buf, int n);
long fs_read(const char *path, char *buf, int n);
int  fs_mkdir(const char *path);
int  fs_remove(const char *path);
int  fs_touch(const char *path);
int  fs_write(const char *path, const char *data, int n);  /* 0=ok, 写/覆盖文件 */

/* ---- 网络：pkg 安装与校验用 ---- */
/* 对文件算 SHA256（64 字符小写十六进制，out 需 >=65 字节）；返回 0 成功 */
int  net_sha256_file(const char *path, char *out, int n);
/* FTP 下载：远端 -> 本地 VFS；返回 0 成功（宿主后端返回错误） */
int  net_ftp_fetch(const char *host, int port, const char *user,
                   const char *pass, const char *remote, const char *local);

/* ---- 进程管理 ---- */
void proc_list_print(void);
int  proc_kill(int pid);

/* ---- 硬件寄存器 ---- */
int  hw_devlist(hw_dev **out, int *n);
int  hw_readdev(const char *dev, unsigned addr, unsigned *val);
int  hw_writedev(const char *dev, unsigned addr, unsigned val);

/* ---- 命令查找链（tinysh 三级查找：内置 -> 内核命令 -> pkg 软件）----
 * 仅 TinyOS 用户态后端（kernel_api_tinyos.c）真正连到内核 tinyos_api_t；
 * 宿主后端（kernel_api_host.c）无内核命令表，kcmd_exists 返回 0、
 * kcmd_exec / prog_exec 直接报错，但必须能编过。 */
int  kcmd_exists(const char *name);              /* 1=是内核命令 0=不是 */
int  kcmd_exec(const char *name, const char *args);  /* 转发给内核执行 */
int  prog_exec(const char *path, const char *args);   /* 跑 /bin 下的 TNCR 程序 */

#endif /* KERNEL_API_H */
