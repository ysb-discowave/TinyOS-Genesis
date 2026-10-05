#ifndef TINYOS_MM_H
#define TINYOS_MM_H

#include "types.h"

/* 内核堆：带块头 + 空闲链表 + 相邻合并的 first-fit 分配器 */
void  mm_init(void);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void *krealloc(void *p, size_t n);
void  kfree(void *p);

/* 统计 */
u32   mm_used(void);
u32   mm_total(void);
u32   mm_free_bytes(void);
u32   mm_free_blocks(void);
void  mm_info(void);

/* 诊断：遍历堆，检查一致性（返回错误数） */
int   mm_check(void);

/* TNCR 用户程序的固定装载/执行地址（用户程序按此地址链接） */
#define TNCR_LOAD_ADDR 0x01000000u
#define TNCR_LOAD_MAX  (512u * 1024u)

#endif
