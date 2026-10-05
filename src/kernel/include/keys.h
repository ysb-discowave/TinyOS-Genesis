#ifndef TINYOS_KEYS_H
#define TINYOS_KEYS_H

/* ============================================================
 * 统一键值约定
 *   0x00 .. 0xFF  : ASCII / 控制码（Ctrl+A..Z = 0x01..0x1A）
 *   0x100 以上    : 扩展键（方向键、Home/End、PgUp/PgDn、Del 等）
 * ============================================================ */

#define KEY_ESC     0x1B
#define KEY_TAB     0x09
#define KEY_ENTER   0x0D
#define KEY_BACKSP  0x08

#define KEY_CTRL(c) ((c) & 0x1F)
#define KEY_CTRL_C  0x03
#define KEY_CTRL_Q  0x11
#define KEY_CTRL_S  0x13
#define KEY_CTRL_O  0x0F
#define KEY_CTRL_X  0x18
#define KEY_CTRL_Z  0x1A

#define KEY_UP      0x101
#define KEY_DOWN    0x102
#define KEY_LEFT    0x103
#define KEY_RIGHT   0x104
#define KEY_HOME    0x105
#define KEY_END     0x106
#define KEY_PGUP    0x107
#define KEY_PGDN    0x108
#define KEY_DEL     0x109
#define KEY_INS     0x10A
#define KEY_F1      0x111
#define KEY_F2      0x112
#define KEY_F3      0x113
#define KEY_F4      0x114
#define KEY_F5      0x115
#define KEY_F6      0x116
#define KEY_F7      0x117
#define KEY_F8      0x118
#define KEY_F9      0x119
#define KEY_F10     0x11A

#endif
