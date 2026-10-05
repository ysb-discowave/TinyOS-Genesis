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

    /* 弹出图形窗口。必须在“桌面双击打开的终端”会话内调用，
     * 否则内核抛异常（返回 -1 并在控制台打印 [TinyOS 异常]）。 */
    int  (*gui_open)(const char *title, const char *content);
    /* 进入图形化桌面（DESKTOP.TNCR 使用） */
    void (*desktop)(void);
    /* 当前会话 id（0 = 不在任何桌面终端会话中） */
    int  (*get_session)(void);
    /* 网络：返回 0 成功，非 0 失败 */
    int  (*ftp_get)(const char *host, int port, const char *user, const char *pass,
                    const char *remote, const char *local);
    int  (*ftp_put)(const char *host, int port, const char *user, const char *pass,
                    const char *local, const char *remote);
    int  (*smb_get)(const char *host, int port, const char *share, const char *user,
                    const char *pass, const char *remote, const char *local);
    int  (*smb_put)(const char *host, int port, const char *share, const char *user,
                    const char *pass, const char *local, const char *remote);
    void (*net_info)(void);
    /* 简易整数格式化 */
    void (*itoa)(int v, char *buf);

    /* ================================================================
     * 以下为网络服务型用户程序（SSHD.TNCR）所需的能力。
     * 追加在末尾：已在磁盘上的旧 TNCR 不受影响（它们只读前面的字段）。
     * ================================================================ */

    /* ---- TCP 套接字（句柄化，见 kernel/net/sock.h） ---- */
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
    int  (*user_verify)(const char *name, const char *pass);   /* 0 = 通过 */
    int  (*user_uid)(const char *name);                        /* -1 = 不存在 */
    int  (*user_current)(void);
    void (*user_set_current)(int uid);
    int  (*user_name_of)(int uid, char *buf, int max);

    /* ---- 命令执行：把 shell 输出捕获进 buf（SSHD 的 exec / shell 通道用） ---- */
    int  (*exec_capture)(const char *cmd, char *buf, int max);

    /* ---- 简单的持久化（保存 SSH 主机密钥，避免每次启动都换） ---- */
    int  (*file_read)(const char *path, char *buf, int max);
    int  (*file_write)(const char *path, const char *data, int len);

    /* ---- 随机与计时 ---- */
    void (*rand_bytes)(u8 *buf, int n);
    u32  (*ticks)(void);

    /* ---- 本地控制台的非阻塞输入 ----
     * 前台运行的服务程序（SSHD）需要一个逃生舱：内核是协作式单线程，
     * 一旦进入自己的主循环，本地键盘就没有别的机会被读到了。 */
    int  (*local_kbhit)(void);
    int  (*local_getc)(void);

    /* ---- 全屏程序（EDIT.TNCR 这类编辑器）需要的控制台原语 ----
     * 只有 print/println 是画不出可编辑窗口的：需要定位光标、收方向键。
     * getkey 返回 keys.h 里的统一键值（方向键 = 0x101..0x104）。 */
    /* draw() 只写 VGA 不写串口：全屏程序每次重绘会刷上千个字符，
     * 走 kputc 的话会把串口日志淹掉（自动化测试全靠读串口）。 */
    void (*draw)(int x, int y, const char *s, u8 fg, u8 bg);
    void (*clearscr)(void);
    void (*gotoxy)(int x, int y);
    void (*setcolor)(u8 fg, u8 bg);
    int  (*getkey)(void);

    /* ---- 命令行参数 / 当前目录 ----
     * TNCR 程序是被 fn(&g_api) 直接调起来的，没有 argc/argv。
     * 想让 `edit /home/a.txt`、`cc src.mc -o out.TNCR` 这类命令把参数递进去，
     * 只能由 Shell 在 tncr_run() 之前把参数串存下来，程序启动后自己来取。 */
    int  (*cmdarg)(char *buf, int max);   /* 启动本程序的参数（已去掉命令名） */
    int  (*cwd)(char *buf, int max);      /* 当前工作目录，用于解析相对路径 */

    /* ================================================================
     * Genesis v0.1 扩展（追加在末尾，已在磁盘上的旧 TNCR 不受影响）。
     * 供 tinysh 等用户态 shell 使用：文件系统/进程/硬件寄存器/系统信息。
     * 必须与 tools/compiler/api_user.h 完全一致。
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

    /* ---- Genesis v0.1 pkg 扩展：把内核的 sha256 能力以"文件级"接口导出，
     * 避免把 sha256_ctx 这种内核结构体泄漏给用户态。返回 0 成功，
     * out 写入 64 字符小写十六进制 + 结尾 0（需 >=65 字节）。 ---- */
    int  (*sha256_file)(const char *path, char *out, int n);
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
