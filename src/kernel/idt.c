#include "idt.h"
#include "io.h"
#include "vga.h"
#include "console.h"
#include "libc.h"

#define IDT_ENTRIES 48

typedef struct {
    u16 offset_low;
    u16 selector;
    u8  zero;
    u8  type_attr;
    u16 offset_high;
} __attribute__((packed)) idt_entry_t;

static idt_entry_t idt[IDT_ENTRIES];
static void *irq_handlers[16] = {0};

/* IRQ 命中计数：没有它就无法区分"中断没送达"和"送达了但状态没更新"，
 * 调试外设时这两者的排查方向完全相反。 */
static volatile u32 irq_count[16] = {0};

void panic(const char *file, int line, const char *msg);

static void idt_set(u8 i, u32 isr) {
    idt[i].offset_low = isr & 0xFFFF;
    idt[i].selector = 0x08;
    idt[i].zero = 0;
    idt[i].type_attr = 0x8E; /* present, ring0, 32 位中断门 */
    idt[i].offset_high = (isr >> 16) & 0xFFFF;
}

void register_irq(u8 irq, void (*handler)(regs_t*)) {
    if (irq < 16) irq_handlers[irq] = (void*)handler;
}

static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();   /* master 矢量为 0x20 */
    outb(0xA1, 0x28); io_wait();   /* slave 矢量为 0x28 */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFF); io_wait();   /* 先屏蔽全部 */
    outb(0xA1, 0xFF); io_wait();
}

/* ---------------------------------------------------------------
 * 取消屏蔽一个 IRQ。
 *
 * 关键：8259 从片（IRQ8..15）是挂在主片的 **IRQ2（级联线）** 上的。
 * 只解开从片自己的 IMR 位是不够的 —— 主片 IRQ2 若仍被屏蔽，
 * 从片的请求根本传不到 CPU。旧代码只写 0xA1，于是 IRQ12（PS/2 鼠标）
 * 永远收不到中断；而键盘在 IRQ1（主片），所以"键盘能用、鼠标完全不动"。
 * --------------------------------------------------------------- */
void enable_irq(u8 irq) {
    if (irq < 8) {
        outb(0x21, (u8)(inb(0x21) & ~(1u << irq)));
        return;
    }
    if (irq < 16) {
        outb(0x21, (u8)(inb(0x21) & ~(1u << 2)));          /* 先放行级联线 */
        outb(0xA1, (u8)(inb(0xA1) & ~(1u << (irq - 8))));  /* 再放行从片具体线 */
    }
}

void disable_irq(u8 irq) {
    if (irq < 8)      outb(0x21, (u8)(inb(0x21) | (1u << irq)));
    else if (irq < 16) outb(0xA1, (u8)(inb(0xA1) | (1u << (irq - 8))));
}

void pic_get_masks(u8 *master, u8 *slave) {
    if (master) *master = inb(0x21);
    if (slave)  *slave  = inb(0xA1);
}

u32 irq_hits(u8 irq) { return (irq < 16) ? irq_count[irq] : 0; }

void idt_init(void) {
    for (u8 i = 0; i < IDT_ENTRIES; i++)
        idt_set(i, isr_stub_addrs[i]);
    struct { u16 limit; u32 base; } __attribute__((packed)) idtp = {
        .limit = (u16)(sizeof(idt) - 1),
        .base  = (u32)idt
    };
    pic_remap();
    __asm__ volatile("lidt %0" :: "m"(idtp));
    __asm__ volatile("sti");
}

void isr_handler(regs_t *r) {
    if (r->int_no < 32) {
        /* CPU 异常：致命 */
        vga_setcolor(0x0F, 0x04);
        kprintf("TinyOS: fatal exception #%d (err=%x) eip=%x cs=%x eflags=%x\n",
                r->int_no, r->err, r->eip, r->cs, r->eflags);
        kprintf("  eax=%x ebx=%x ecx=%x edx=%x esi=%x edi=%x ebp=%x esp=%x\n",
                r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp, r->esp);
        kprintf("  int_no=%x err=%x ds=%x es=%x fs=%x gs=%x\n",
                r->int_no, r->err, r->ds, r->es, r->fs, r->gs);
        {
            char b[200];
            int n = 0;
            const char *hdr = "  irq hits: ";
            while (hdr[n]) { b[n] = hdr[n]; n++; }
            for (u8 i = 0; i < 16; i++) {
                char t[16];
                u32 v = irq_hits(i);
                int k = 0;
                if (v == 0) { t[k++] = '0'; }
                while (v > 0) { t[k++] = (char)('0' + (v % 10)); v /= 10; }
                while (k > 0) b[n++] = t[--k];
                b[n++] = (i == 15) ? '\n' : ',';
            }
            b[n] = 0;
            kprintf("%s", b);
        }
        __asm__ volatile("cli; hlt");
        for (;;);
    }
    u8 irq = (u8)(r->int_no - 32);
    if (irq < 16) {
        irq_count[irq]++;
        if (irq_handlers[irq]) {
            void (*h)(regs_t*) = (void(*)(regs_t*))irq_handlers[irq];
            h(r);
        }
    }
    /* EOI */
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
