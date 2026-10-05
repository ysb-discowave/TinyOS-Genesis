#ifndef TINYOS_INSTALL_H
#define TINYOS_INSTALL_H

/* ============================================================
 * TinyOS 安装程序 / 安装向导
 * ============================================================ */

/* 老的命令行安装器：install [yes] [--rootpass P] [--with-ssh|--no-ssh] */
int installer_run(const char *arg);

/* 分步安装向导：磁盘 -> 网络 -> 组件 -> 账户 -> 确认并安装。
 * 每一步都能直接回车取默认值，或输入 s / skip 跳过。
 * 返回 0 = 已安装，1 = 用户放弃/跳过，-1 = 出错。 */
int installer_wizard(const char *arg);

/* 是否应该开机自动启动安装向导：
 * 有硬盘且磁盘上没有 /etc/installed（即系统还没装过）时为真。
 * 完全没硬盘时返回 0，否则无盘环境一开机就被向导挡住。 */
int wizard_should_autostart(void);

/* 读取 /etc/net.conf 并应用到当前的 IP/网关（开机时调用一次） */
void netconf_apply(void);
/* 打印 /etc/net.conf（网络配置的来源） */
void netconf_print(void);

/* /etc/installed 是否存在（系统是否已安装到磁盘） */
int installed_marker_present(void);

/* 本内核是否为"安装介质"（CD/DVD 镜像）：是则开机直接进安装界面，
 * 且启动阶段不会格式化/写入任何目标磁盘。 */
int booted_from_install_media(void);

#endif
