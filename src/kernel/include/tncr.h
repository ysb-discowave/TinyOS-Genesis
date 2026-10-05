#ifndef TINYOS_TNCR_H
#define TINYOS_TNCR_H

#include "types.h"

/* ============================================================
 * TNCR 可执行文件格式（TinyOS Native Code Runnable）
 * ------------------------------------------------------------
 * 偏移  大小  字段
 *  0     4    magic "TNCR"
 *  4     4    flags   bit0 = 需要图形窗口(GUI)   bit1 = 桌面启动器(DESKTOP)
 *  8     4    load_addr  代码链接地址（用户程序按此地址链接）
 * 12     4    entry_off  入口 = load_addr + entry_off
 * 16     4    code_size  需从文件拷入 load_addr 的字节数
 * 20     4    bss_size   代码之后需清零的字节数
 * 24     N    payload    代码 + 只读数据 + 已初始化数据（按链接地址顺序）
 * ============================================================ */
#define TNCR_MAGIC "TNCR"
#define TNCR_HDR_SIZE 24

#define TNCR_F_GUI     (1u << 0)
#define TNCR_F_DESKTOP (1u << 1)

/* 运行一个 TNCR 程序；session_id 为发起运行的终端会话（0 = 普通 Shell）。
 * GUI 程序若不在桌面终端会话内运行，将通过 api->gui_open 触发内核异常。*/
int tncr_run(const char *path, int session_id);

#endif
