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

/* 诊断计数：收到的完整帧数 / 被丢弃的错位帧数 */
u32  mouse_packets_ok(void);
u32  mouse_packets_bad(void);

/* mouse_init 走到/卡在哪一步（0=未开始，9=完整复位路径成功，8=兼容路径成功，
 * -1 已返回；3=读配置失败，4/5/6/7=对应命令握手失败）。 */
int  mouse_init_step(void);

#endif
