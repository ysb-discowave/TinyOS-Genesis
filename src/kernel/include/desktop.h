#ifndef TINYOS_DESKTOP_H
#define TINYOS_DESKTOP_H

#include "types.h"

/* 桌面是否处于激活状态（GUI 程序据此决定弹窗方式） */
extern int g_desktop_active;

/* 进入图形化桌面（由 DESKTOP.TNCR 或 `desktop` 命令调用） */
void desktop_enter(void);

/* 在桌面上弹出一个图形窗口（由 gui_open 在通过会话规则校验后调用） */
void desktop_show_window(const char *title, const char *content);

#endif
