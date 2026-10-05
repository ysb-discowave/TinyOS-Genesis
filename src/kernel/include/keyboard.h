#ifndef TINYOS_KEYBOARD_H
#define TINYOS_KEYBOARD_H

#include "types.h"

void kbd_init(void);
/* 非阻塞：有键返回统一键值（见 keys.h），否则 -1 */
int  kbd_getkey(void);
/* 非阻塞：只返回 ASCII（< 0x100），扩展键被丢弃 */
int  kbd_getchar(void);
/* 环形缓冲中是否有待读按键 */
int  kbd_haschar(void);

#endif
