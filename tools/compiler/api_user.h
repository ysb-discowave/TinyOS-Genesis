#ifndef TINYOS_API_USER_H
#define TINYOS_API_USER_H

/* ============================================================
 * TinyOS 用户程序 API（宿主侧编译时使用）
 * 与内核 kernel/include/api.h 中的 tinyos_api_t 必须完全一致。
 *
 * 用户程序入口:
 *     void user_main(tinyos_api_t *api);
 * ============================================================ */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

typedef struct tinyos_api {
    void (*print)(const char *s);
    void (*println)(const char *s);
    int  (*readline)(char *buf, int max);

    /* 弹出图形窗口。必须在桌面双击打开的终端会话内调用，
     * 否则内核抛异常并返回 -1。 */
    int  (*gui_open)(const char *title, const char *content);
    void (*desktop)(void);
    int  (*get_session)(void);

    int  (*ftp_get)(const char *host, int port, const char *user, const char *pass,
                    const char *remote, const char *local);
    int  (*ftp_put)(const char *host, int port, const char *user, const char *pass,
                    const char *local, const char *remote);
    int  (*smb_get)(const char *host, int port, const char *share, const char *user,
                    const char *pass, const char *remote, const char *local);
    int  (*smb_put)(const char *host, int port, const char *share, const char *user,
                    const char *pass, const char *local, const char *remote);
    void (*net_info)(void);
    void (*itoa)(int v, char *buf);

    /* ---- TCP 套接字（句柄化） ---- */
    int  (*sock_listen)(int port);
    int  (*sock_accept)(int lh, int timeout_ms);
    int  (*sock_recv)(int h, u8 *buf, int max, int timeout_ms);
    int  (*sock_poll_recv)(int h, u8 *buf, int max);
    int  (*sock_send)(int h, const u8 *buf, int n);
    void (*sock_close)(int h);
    int  (*sock_closed)(int h);
    int  (*sock_readable)(int h);
    u32  (*sock_local_ip)(void);

    /* ---- 用户与口令 ---- */
    int  (*user_verify)(const char *name, const char *pass);
    int  (*user_uid)(const char *name);
    int  (*user_current)(void);
    void (*user_set_current)(int uid);
    int  (*user_name_of)(int uid, char *buf, int max);

    /* ---- 命令执行 + 输出捕获 ---- */
    int  (*exec_capture)(const char *cmd, char *buf, int max);

    /* ---- 简单持久化 ---- */
    int  (*file_read)(const char *path, char *buf, int max);
    int  (*file_write)(const char *path, const char *data, int len);

    /* ---- 随机与计时 ---- */
    void (*rand_bytes)(u8 *buf, int n);
    u32  (*ticks)(void);

    /* ---- 本地控制台非阻塞输入（服务主循环的逃生舱） ---- */
    int  (*local_kbhit)(void);
    int  (*local_getc)(void);

    /* ---- 全屏程序（编辑器）需要的控制台原语 ---- */
    void (*draw)(int x, int y, const char *s, u8 fg, u8 bg);
    void (*clearscr)(void);
    void (*gotoxy)(int x, int y);
    void (*setcolor)(u8 fg, u8 bg);
    int  (*getkey)(void);

    /* ---- 命令行参数 / 当前目录（TNCR 没有 argc/argv，由 Shell 代传） ---- */
    int  (*cmdarg)(char *buf, int max);   /* 启动本程序的参数（已去掉命令名） */
    int  (*cwd)(char *buf, int max);      /* 当前工作目录，用于解析相对路径 */

    /* ================================================================
     * Genesis v0.1 扩展（追加在末尾，已在磁盘上的旧 TNCR 不受影响）。
     * 供 tinysh 等用户态 shell 使用：文件系统/进程/硬件寄存器/系统信息。
     * 必须与 kernel/include/api.h 完全一致。
     * ================================================================ */
    int  (*mkdir)(const char *path);
    int  (*rm_file)(const char *path);    /* 删除文件或空目录 */
    int  (*fs_list)(const char *path,
                    void (*cb)(const char *name, int is_dir, long size, void *arg),
                    void *arg);
    int  (*stat)(const char *path, int *is_dir, long *size);
    int  (*set_cwd)(const char *path);
    void (*proc_list_all)(void);
    int  (*proc_kill)(int pid);
    void (*dev_list_all)(void (*cb)(const char *name, u16 io_base, u16 io_size, void *arg),
                         void *arg);
    int  (*dev_read)(const char *name, u32 off, u8 *buf, int n);
    int  (*dev_write)(const char *name, u32 off, const u8 *buf, int n);
    void (*sysinfo)(char *buf, int n);
    void (*date)(char *buf, int n);
    void (*version)(char *buf, int n);
} tinyos_api_t;

/* 用户程序入口。
 * 必须声明为 C 链接：否则 C++ 会把 user_main 名字改编(mangle)成
 * _Z9user_mainP11tinyos_api_t，链接时 --entry=user_main 找不到符号，
 * 内核 TNCR 加载器也定位不到入口。
 * 只要先有这条 extern "C" 声明，后面同名定义会自动继承 C 链接。 */
#ifdef __cplusplus
extern "C" {
#endif
void user_main(tinyos_api_t *api);
#ifdef __cplusplus
}
#endif

/* ---- 便利宏（用户程序可用） ---- */
#define API_PRINT(api, s)     ((api)->print(s))
#define API_PRINTLN(api, s)   ((api)->println(s))
#define API_GUI(api, t, c)    ((api)->gui_open((t), (c)))

#endif
