#ifndef TINYOS_IDT_H
#define TINYOS_IDT_H

#include "types.h"

/* 中断现场（与 idt.asm 的压栈顺序严格对应） */
typedef struct {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
    u32 int_no, err;
    u32 eip, cs, eflags, useresp, ss;
} regs_t;

/* 由 idt.asm 提供：48 个 stub 地址表 */
extern u32 isr_stub_addrs[48];

void idt_init(void);
/* 取消屏蔽某个 IRQ（在驱动初始化时调用）。
 * irq >= 8 时内部会连同主片的 IRQ2 级联线一起放行 —— 漏掉这一步，
 * 从片上所有中断（IRQ8..15）都会被静默丢弃。 */
void enable_irq(u8 irq);
void disable_irq(u8 irq);
/* 读取 8259 主/从屏蔽字（调试用） */
void pic_get_masks(u8 *master, u8 *slave);
/* 某个 IRQ 自启动以来被触发过多少次（调试用：0 就说明中断根本没到 CPU） */
u32  irq_hits(u8 irq);
/* 注册 IRQ(0..15) 处理函数；handler 返回后自动 EOI */
void register_irq(u8 irq, void (*handler)(regs_t*));
/* 由 idt.asm 调用 */
void isr_handler(regs_t *r);

#endif
