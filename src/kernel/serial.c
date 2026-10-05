#include "serial.h"
#include "keys.h"
#include "io.h"
#include "idt.h"
#include "pit.h"

#define COM1 0x3F8

#define UART_DATA  (COM1 + 0)
#define UART_IER   (COM1 + 1)
#define UART_FCR   (COM1 + 2)
#define UART_LCR   (COM1 + 3)
#define UART_MCR   (COM1 + 4)
#define UART_LSR   (COM1 + 5)

#define LSR_DR   0x01   /* data ready */
#define LSR_THRE 0x20   /* transmit holding register empty */

#define SER_BUF_SIZE 512
static volatile char sbuf[SER_BUF_SIZE];
static volatile int shead = 0, stail = 0;

void serial_init(void) {
    outb(UART_IER, 0x00);   /* 关闭中断（配置期间） */
    outb(UART_LCR, 0x80);   /* 打开 DLAB */
    outb(UART_DATA, 0x03);  /* 除数低字节：38400 baud (115200/3) */
    outb(UART_IER,  0x00);  /* 除数高字节 */
    outb(UART_LCR, 0x03);   /* 8N1, 关闭 DLAB */
    outb(UART_FCR, 0xC7);   /* 使能 FIFO、清空、14 字节阈值 */
    outb(UART_MCR, 0x0B);   /* RTS/DSR 就绪, OUT2=1 以放行 IRQ */
    outb(UART_IER, 0x01);   /* 使能“接收数据可用”中断 */
    shead = stail = 0;
}

void serial_putc(char c) {
    /* 等待发送保持寄存器空 */
    while (!(inb(UART_LSR) & LSR_THRE)) { }
    outb(UART_DATA, (u8)c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

int serial_haschar(void) {
    return shead != stail;
}

int serial_getchar(void) {
    if (shead == stail) return -1;
    char c = sbuf[stail];
    stail = (stail + 1) % SER_BUF_SIZE;
    return (unsigned char)c;
}

int serial_isr(void) {
    /* 一次性排空 UART 接收 FIFO */
    while (inb(UART_LSR) & LSR_DR) {
        char c = (char)inb(UART_DATA);
        int next = (shead + 1) % SER_BUF_SIZE;
        if (next != stail) { sbuf[shead] = c; shead = next; }
    }
    return 0;
}

/* ---- 统一键值读取：解析 ANSI 转义序列（方向键等） ---- */
static int ser_wait(u32 ms, unsigned char *out) {
    u32 t0 = pit_ticks();
    while ((u32)(pit_ticks() - t0) < ms) {
        if (shead != stail) {
            *out = (unsigned char)sbuf[stail];
            stail = (stail + 1) % SER_BUF_SIZE;
            return 1;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

int serial_getkey(void) {
    int c = serial_getchar();
    if (c < 0) return -1;
    if (c != KEY_ESC) return c;               /* 普通字符/控制码 */

    unsigned char c2;
    if (!ser_wait(60, &c2)) return KEY_ESC;
    if (c2 != '[' && c2 != 'O') return KEY_ESC;

    unsigned char c3;
    if (!ser_wait(60, &c3)) return KEY_ESC;
    switch (c3) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '1': case '2': case '3': case '4': case '5': case '6': {
        unsigned char c4;
        if (!ser_wait(60, &c4)) return KEY_ESC;
        if (c4 != '~') return KEY_ESC;
        switch (c3) {
        case '1': return KEY_HOME;
        case '2': return KEY_INS;
        case '3': return KEY_DEL;
        case '4': return KEY_END;
        case '5': return KEY_PGUP;
        case '6': return KEY_PGDN;
        default:  break;
        }
        break;
    }
    default: break;
    }
    return KEY_ESC;
}

static void serial_irq(regs_t *r) {
    (void)r;
    serial_isr();
}

void serial_enable_irq(void) {
    register_irq(4, serial_irq);   /* COM1 = IRQ4 */
    enable_irq(4);
}
