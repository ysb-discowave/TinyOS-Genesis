#ifndef TINYOS_API_H
#define TINYOS_API_H

#include "types.h"

/* ============================================================
 * 内核导出给用户态 TNCR 程序的系统调用接口（函数指针表）。
 * 用户程序入口为 user_main(tinyos_api_t *api)，只能通过此表访问内核。
 * 注意：此结构必须与 tools/compiler/api_user.h 完全一致。
 * ============================================================ */
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

    /* ================================================================
     * 命令查找链（追加在末尾，已在磁盘上的旧 TNCR 不受影响）。
     * 供 tinysh 等用户态 shell 使用：一条命令若不是 tinysh 内置，
     * 先问内核有没有同名命令，没有再去看 /bin/<name>.TNCR 这种
     * pkg 安装的软件。优先级：内置 -> 内核命令 -> pkg 软件。
     * 必须与 tools/compiler/api_user.h 完全一致。
     * ================================================================ */
    int  (*kcmd_exists)(const char *name);   /* 1=是内核命令 0=不是（纯查询，不执行） */
    int  (*kcmd_exec)(const char *name, char *args, int n);  /* 把 name + args 交给 shell_exec */
    int  (*prog_exec)(const char *path, char *args, int n);   /* 跑 /bin 下的 TNCR 程序 */

    /* ================================================================
     * Genesis v0.1 http 扩展（追加在末尾，已在磁盘上的旧 TNCR 不受影响）。
     * 最小 HTTP/1.1 客户端：pkg 直接从 GitHub raw 源拉包，省去手动克隆。
     * 必须与 tools/compiler/api_user.h 完全一致。
     * ================================================================ */
    int  (*http_get)(const char *host, int port, const char *path,
                     void *buf, int max, int *out_len);
    int  (*http_get_file)(const char *host, int port, const char *path,
                          const char *local);   /* 分块下载整文件到本地 VFS 路径 */
    /* 走 HTTP CONNECT 代理。GitHub 全面强制 HTTPS（明文一律 301 跳转），
     * 企业网络里通常由代理终结 TLS。proxy_host 为空则退化为明文直连。 */
    int  (*http_get_proxy)(const char *host, int port, const char *path,
                           const char *proxy_host, int proxy_port,
                           void *buf, int max, int *out_len);
    int  (*http_get_file_proxy)(const char *host, int port, const char *path,
                                const char *local,
                                const char *proxy_host, int proxy_port);
    /* 探测代理能否为目标建立 CONNECT 隧道。
     * 返回 0 隧道可用；HTTP_E_NOTLS 表示隧道通了但内核缺 TLS 栈。 */
    int  (*http_tunnel)(const char *proxy_host, int proxy_port,
                        const char *host, int port);

    /* ---- Genesis v0.1 client TCP 扩展：主动发起连接（SSH 客户端等用）----
     * 复用内核 net 层的 tcp_connect + tcp_wait_established：发 SYN 后立即
     * 返回，之后轮询直到对端回 SYN-ACK 完成三次握手（或超时）。
     * 返回 >=0 的句柄（与 sock_accept 同句柄空间），失败返回 -1。 */
    int  (*sock_connect)(u32 ip, u16 port, int timeout_ms);
} tinyos_api_t;

/* 用户程序入口（由 TNCR 加载器调用） */
typedef void (*user_entry_t)(tinyos_api_t *api);

/* ================================================================
 * Genesis v0.1 扩展字段的内核实现（kernel/genesis_api.c）。
 * 声明在这里，好让 api.c 能把它们填进 g_api 的静态初始化器 ——
 * 编译期就绑定好，不依赖任何运行期初始化顺序。
 * ================================================================ */
int  k_mkdir(const char *path);
int  k_rm_file(const char *path);
int  k_fs_list(const char *path,
               void (*cb)(const char *name, int is_dir, long size, void *arg),
               void *arg);
int  k_stat(const char *path, int *is_dir, long *size);
int  k_set_cwd(const char *path);
void k_proc_list_all(void);
int  k_proc_kill(int pid);
void k_dev_list_all(void (*cb)(const char *name, u16 io_base, u16 io_size, void *arg),
                    void *arg);
int  k_dev_read(const char *name, u32 off, u8 *buf, int n);
int  k_dev_write(const char *name, u32 off, const u8 *buf, int n);
void k_sysinfo(char *buf, int n);
void k_date(char *buf, int n);
void k_version(char *buf, int n);
int  k_sha256_file(const char *path, char *out, int n);

/* ---- 命令查找链（见 tinyos_api_t 末尾字段）---- */
int  k_cmd_exists(const char *name);                 /* 纯查询：name 是否为内核命令 */
int  k_cmd_exec(const char *name, char *args, int n); /* 拼成命令行交给 shell_exec */
int  k_prog_exec(const char *path, char *args, int n);/* 跑 /bin 下的 TNCR 程序 */

/* ---- Genesis v0.1 http 扩展（见 tinyos_api_t 末尾字段）---- */
int  k_http_get(const char *host, int port, const char *path,
                void *buf, int max, int *out_len);    /* 薄封装 http_get */
int  k_http_get_file(const char *host, int port, const char *path,
                     const char *local);              /* 薄封装 http_get_file */
int  k_http_get_proxy(const char *host, int port, const char *path,
                      const char *proxy_host, int proxy_port,
                      void *buf, int max, int *out_len);
int  k_http_get_file_proxy(const char *host, int port, const char *path,
                           const char *local,
                           const char *proxy_host, int proxy_port);
int  k_http_tunnel(const char *proxy_host, int proxy_port,
                   const char *host, int port);

#endif
