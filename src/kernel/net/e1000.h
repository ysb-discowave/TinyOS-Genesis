#ifndef TINYOS_E1000_H
#define TINYOS_E1000_H

#include "types.h"

/* 初始化 e1000（QEMU 默认 82540EM，设备号 0x100E）。
 * 成功返回 0，失败返回 -1。发送/接收使用物理地址直接映射（未开启分页）。 */
int  e1000_init(void);
/* 发送一帧以太网数据（data 为完整帧，含 14 字节 MAC 头，len 为总长） */
void e1000_send(const u8 *frame, u32 len);
/* 轮询接收：每收到一帧调用 cb(frame, len)。返回收到的帧数 */
int  e1000_poll(void (*cb)(const u8 *frame, u32 len));
/* 读取 MAC 地址（6 字节） */
void e1000_get_mac(u8 mac[6]);

#endif
