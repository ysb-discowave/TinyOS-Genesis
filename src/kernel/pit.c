#include "pit.h"
#include "io.h"
#include "idt.h"

static volatile u32 ticks = 0;

static void timer_isr(regs_t *r) {
    (void)r;
    ticks++;
}

void pit_init(void) {
    u32 divisor = 1193182 / 100; /* 100Hz */
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    register_irq(0, timer_isr);
}

u32 pit_ticks(void) { return ticks; }
u32 pit_seconds(void) { return ticks / 100; }

void pit_sleep(u32 ms) {
    u32 target = ticks + (ms * 100) / 1000 + 1;
    while (ticks < target) __asm__ volatile("hlt");
}
