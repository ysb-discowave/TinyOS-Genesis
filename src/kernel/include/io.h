#ifndef TINYOS_IO_H
#define TINYOS_IO_H

#include "types.h"

static inline u8 inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %%dx, %%al" : "=a"(v) : "d"(port));
    return v;
}
static inline u16 inw(u16 port) {
    u16 v;
    __asm__ volatile("inw %%dx, %%ax" : "=a"(v) : "d"(port));
    return v;
}
static inline u32 inl(u16 port) {
    u32 v;
    __asm__ volatile("inl %%dx, %%eax" : "=a"(v) : "d"(port));
    return v;
}
static inline void outb(u16 port, u8 v) {
    __asm__ volatile("outb %%al, %%dx" :: "d"(port), "a"(v));
}
static inline void outw(u16 port, u16 v) {
    __asm__ volatile("outw %%ax, %%dx" :: "d"(port), "a"(v));
}
static inline void outl(u16 port, u32 v) {
    __asm__ volatile("outl %%eax, %%dx" :: "d"(port), "a"(v));
}
static inline void io_wait(void) { outb(0x80, 0); }

/* 块传送：以 16 位为单位（ATA PIO 用） */
static inline void insw(u16 port, void *buf, u32 words) {
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(words) : "d"(port) : "memory");
}
static inline void outsw(u16 port, const void *buf, u32 words) {
    __asm__ volatile("rep outsw" : "+S"(buf), "+c"(words) : "d"(port) : "memory");
}

#endif /* TINYOS_IO_H */
