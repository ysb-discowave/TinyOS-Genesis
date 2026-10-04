/* kernel_api.h — tinysh 与内核之间的接口（手册 §7 内核 API 对接约定）
 *
 * tinysh 只做命令解析，所有底层硬件/文件/进程操作都通过这里调用内核。
 * 提供两套实现：
 *   kernel_api_host.c    —— 宿主机（开发/测试）后端，可独立编译运行验证
 *   kernel_api_tinyos.c  —— TinyOS 2.0 用户态后端。TinyOS 的 .tncr 用户态
 *                           程序【只能】通过内核导出的 tinyos_api_t 函数指针
 *                           表访问内核（api.h：print/println/readline/
 *                           file_read/file_write/cwd)，手册 §7 所需的 fs_list/
 *                           proc_*、hw_* 等为 Genesis v0.1 在表末尾追加的扩展。
 *                           随 TinyOS 2.0 用户态 + stdio shim 一起编为 .tncr。
 */
#ifndef KERNEL_API_H
#define KERNEL_API_H

#include <stddef.h>

/* ---------- fs_api 文件系统接口 ---------- */
typedef struct {
    char  name[64];
    int   is_dir;     /* 1 = 目录, 0 = 普通文件 */
    long  size;       /* 字节数 */
} fs_entry;

/* 列出 path 下条目，*out 指向内部静态数组，*count 为条目数；返回 0 或 E_* */
int  fs_list(const char *path, fs_entry **out, int *count);
int  fs_chdir(const char *path);                 /* 切换工作目录 */
int  fs_getcwd(char *buf, int n);                /* 取当前工作目录 */
/* 读取文件到 buf（最多 n 字节）；返回读取字节数(>=0) 或 -E_* 负数 */
long fs_read(const char *path, char *buf, long n);
int  fs_mkdir(const char *name);                /* 创建单层目录 */
int  fs_remove(const char *path);               /* 删除普通文件（禁止删目录）*/
int  fs_touch(const char *name);                /* 创建空文件 */

/* ---------- proc_api 进程管理接口 ---------- */
typedef struct {
    int   pid;
    char  name[32];
    char  state[10];
    long  mem;
} proc_info;

void proc_list_print(void);    /* 按手册格式打印进程列表 */
int  proc_kill(int pid);       /* 终止指定 PID；返回 0 或 E_* */

/* ---------- hw_api 硬件寄存器访问接口 ---------- */
typedef struct {
    char path[32];             /* 如 /dev/uart、/dev/gpio */
} hw_dev;

int  hw_devlist(hw_dev **out, int *count);
int  hw_readdev(const char *dev, unsigned addr, unsigned *val);
int  hw_writedev(const char *dev, unsigned addr, unsigned val);

/* ---------- 系统信息 ---------- */
void sys_version(char *buf, int n);   /* TinyOS 版本号、编译时间、tinysh 版本 */
void sys_sysinfo(char *buf, int n);   /* CPU/内存/运行时长/进程数 */
void sys_date(char *buf, int n);      /* 内核时间戳字符串 */

#endif /* KERNEL_API_H */
