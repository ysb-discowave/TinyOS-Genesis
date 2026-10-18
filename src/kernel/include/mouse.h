#ifndef TINYOS_MOUSE_H
#define TINYOS_MOUSE_H

#include "types.h"

/* 初始化并检测鼠标。返回 0 = 鼠标在位且已打开上报，-1 = 未检测到。 */
int  mouse_init(void);
/* 鼠标是否可用（mouse_init 成功后才为真） */
int  mouse_present(void);
/* 返回当前单元格坐标与按键状态 */
void mouse_state(int *x, int *y, u8 *buttons);
/* 直接设置指针位置（测试/初始化用） */
void mouse_set_position(int x, int y);
/* 自上次调用以来是否发生一次左键点击（边沿触发） */
int  mouse_consume_click(void);
/* 自上次调用以来是否发生一次右键点击 */
int  mouse_consume_rclick(void);
int  mouse_middle_down(void);

/* 光标样式 */
#define MOUSE_STYLE_INVERT 0   /* 反色一格（默认，任何底色可见） */
#define MOUSE_STYLE_BLOCK   1   /* 实心方块 █ */
#define MOUSE_STYLE_ARROW   2   /* 右箭头 ► (CP437 0x10) */
#define MOUSE_STYLE_CROSS   3   /* 十字 + */
#define MOUSE_STYLE_BUSY    4   /* 忙碌 ¤ (CP437 0xF5) */
#define MOUSE_STYLE_TEXT    5   /* 文本 I 形 (CP437 0xB3) */

void mouse_set_style(int style);   /* 设置样式（越界忽略） */
int  mouse_style(void);            /* 当前样式 */
/* 光标渲染：paint 前会先 erase；重复 paint 同一格是空操作 */
void mouse_cursor_paint(void);     /* 在当前位置画光标（保存被覆盖的格） */
void mouse_cursor_erase(void);     /* 恢复被覆盖的格 */

/* 诊断计数：收到的完整帧数 / 被丢弃的错位帧数 */
u32  mouse_packets_ok(void);
u32  mouse_packets_bad(void);

/* mouse_init 走到/卡在哪一步（0=未开始，9=完整复位路径成功，8=兼容路径成功，
 * -1 已返回；3=读配置失败，4/5/6/7=对应命令握手失败）。 */
int  mouse_init_step(void);

#endif
