#include "console.h"
#include "vga.h"
#include "serial.h"
#include "keyboard.h"
#include "libc.h"

/* ---- 输出：VGA + 串口 双写 ----
 * 另外支持"重定向到内存缓冲"：SSH 服务端执行用户命令时，需要把
 * shell 输出收进缓冲区再通过 SSH 通道发回去，而不是打到本地控制台。
 * 捕获期间本地控制台保持安静（相当于 > /dev/null），否则本地提示符
 * 会和远端命令输出互相穿插。 */
static char *g_cap_buf = 0;
static int   g_cap_max = 0;
static int   g_cap_len = 0;
static int   g_cap_ovf = 0;

void cons_capture_begin(char *buf, int max) {
    g_cap_buf = buf; g_cap_max = max; g_cap_len = 0; g_cap_ovf = 0;
    if (buf && max > 0) buf[0] = 0;
}
int cons_capture_len(void) { return g_cap_len; }
int cons_capture_overflowed(void) { return g_cap_ovf; }
void cons_capture_end(void) {
    if (g_cap_buf && g_cap_len < g_cap_max) g_cap_buf[g_cap_len] = 0;
    g_cap_buf = 0;
}
int cons_capture_active(void) { return g_cap_buf != 0; }

void kputc(char c) {
    if (g_cap_buf) {
        if (g_cap_len < g_cap_max - 1) g_cap_buf[g_cap_len++] = c;
        else g_cap_ovf = 1;
        return;
    }
    vga_putc(c);
    serial_putc(c);
}

void kputs(const char *s) {
    while (*s) kputc(*s++);
}

int kprintf(const char *fmt, ...) {
    char b[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    __builtin_va_end(ap);
    kputs(b);
    return n;
}

/* ---- 输入：串口优先（便于脚本控制），其次键盘 ---- */
int cons_haschar(void) {
    return serial_haschar() || kbd_haschar();
}

int cons_getchar(void) {
    int c = serial_getchar();
    if (c >= 0) return c;
    c = kbd_getchar();
    if (c >= 0) return c;
    return -1;
}

/* 统一键值：串口优先（脚本可控），其次键盘 */
int cons_haskey(void) {
    return serial_haschar() || kbd_haschar();
}

int cons_getkey(void) {
    int k = serial_getkey();
    if (k >= 0) return k;
    k = kbd_getkey();
    if (k >= 0) return k;
    return -1;
}

int cons_waitkey(void) {
    for (;;) {
        int k = cons_getkey();
        if (k >= 0) return k;
        __asm__ volatile("hlt");
    }
}

int cons_readline(char *buf, int max) {
    int i = 0;
    for (;;) {
        int c = cons_getchar();
        if (c < 0) {
            __asm__ volatile("hlt");   /* 等中断（键盘/串口） */
            continue;
        }
        if (c == '\r' || c == '\n') {
            kputc('\n');
            buf[i] = 0;
            return i;
        }
        if (c == '\b' || c == 127) {
            if (i > 0) { i--; kputs("\b \b"); }
            continue;
        }
        if (c >= 32 && c < 127) {
            if (i < max - 1) { buf[i++] = (char)c; kputc((char)c); }
        }
    }
}

void cons_clear(void) {
    vga_clear(0x0F);
}

void cons_setcolor(u8 fg, u8 bg) {
    vga_setcolor(fg, bg);
}

/* 口令输入：每个字符回显 '*'，避免旁观者读屏；
 * 退格时用 "\b \b" 把星号擦掉。缓冲区里存的仍是真实字符。 */
int cons_readline_masked(char *buf, int max) {
    int i = 0;
    for (;;) {
        int c = cons_getchar();
        if (c < 0) { __asm__ volatile("hlt"); continue; }
        if (c == '\r' || c == '\n') { buf[i] = 0; return i; }
        if (c == '\b' || c == 127) {
            if (i > 0) { i--; kputs("\b \b"); }
            continue;
        }
        if (c >= 32 && c < 127) {
            if (i < max - 1) { buf[i++] = (char)c; kputc('*'); }
        }
    }
}
