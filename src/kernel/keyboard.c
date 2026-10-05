#include "keyboard.h"
#include "keys.h"
#include "io.h"
#include "idt.h"
#include "libc.h"

/* ============================================================
 * PS/2 键盘驱动（扫描码集 1）
 *  - 支持 Shift / Ctrl / Alt 修饰键
 *  - 支持 0xE0 前缀的扩展键（方向键、Home/End、PgUp/PgDn、Del、Ins、F1..）
 *  - 输出统一键值（见 keys.h）
 * ============================================================ */

#define KBD_BUF_SIZE 128
static u16 kbuf[KBD_BUF_SIZE];
static volatile int khead = 0, ktail = 0;
static u8 shift_state = 0;
static u8 ctrl_state = 0;
static u8 alt_state = 0;
static u8 e0_prefix = 0;

/* 扫描码集 1 → ASCII（make code） */
static const char sc_ascii[] = {
    0,0, '1','2','3','4','5','6','7','8','9','0','-','=',0,0,
    'q','w','e','r','t','y','u','i','o','p','[',']',0,0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,
    '\\','z','x','c','v','b','n','m',',','.','/',0,0,0,' '
};
static const char sc_ascii_shift[] = {
    0,0, '!','@','#','$','%','^','&','*','(',')','_','+',0,0,
    'Q','W','E','R','T','Y','U','I','O','P','{','}',0,0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,
    '|','Z','X','C','V','B','N','M','<','>','?',0,0,0,' '
};

static void push_key(u16 k) {
    int next = (khead + 1) % KBD_BUF_SIZE;
    if (next != ktail) { kbuf[khead] = k; khead = next; }
}

/* 扩展键（0xE0 之后的 make code） */
static u16 ext_key(u8 sc) {
    switch (sc) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;
    case 0x4F: return KEY_END;
    case 0x49: return KEY_PGUP;
    case 0x51: return KEY_PGDN;
    case 0x53: return KEY_DEL;
    case 0x52: return KEY_INS;
    default:   return 0;
    }
}

void kbd_isr(void) {
    /* 8042 状态口 bit5=1 表示这个字节来自鼠标。
     * 不检查就 inb(0x60)，会把鼠标数据当扫描码吞掉；反之亦然。
     * 初始化阶段（mouse_init）还会临时屏蔽 IRQ1，避免抢走控制器的应答字节。 */
    u8 st = inb(0x64);
    if (!(st & 0x01)) return;          /* 没有数据 */
    if (st & 0x20) return;             /* 是鼠标数据，交给鼠标驱动 */

    u8 sc = inb(0x60);

    if (sc == 0xE0) { e0_prefix = 1; return; }

    /* 修饰键（make/break 都需要处理） */
    if (sc == 0x1D) { ctrl_state = 1; e0_prefix = 0; return; }
    if (sc == 0x9D) { ctrl_state = 0; e0_prefix = 0; return; }
    if (sc == 0x2A || sc == 0x36) { shift_state = 1; e0_prefix = 0; return; }
    if (sc == 0xAA || sc == 0xB6) { shift_state = 0; e0_prefix = 0; return; }
    if (sc == 0x38) { alt_state = 1; e0_prefix = 0; return; }
    if (sc == 0xB8) { alt_state = 0; e0_prefix = 0; return; }

    if (sc & 0x80) { e0_prefix = 0; return; }   /* 其他 break code */

    if (e0_prefix) {
        e0_prefix = 0;
        u16 k = ext_key(sc);
        if (k) push_key(k);
        return;
    }

    /* 普通键 */
    if (sc == 0x01) { push_key(KEY_ESC); return; }          /* Esc */
    if (sc == 0x0E) { push_key(KEY_BACKSP); return; }       /* Backspace */
    if (sc == 0x0F) { push_key(KEY_TAB); return; }
    if (sc == 0x1C || sc == 0xE0) { push_key(KEY_ENTER); return; }
    if (sc == 0x39) { push_key(' '); return; }
    if (sc >= 0x3B && sc <= 0x44) {                          /* F1..F10 */
        push_key((u16)(KEY_F1 + (sc - 0x3B)));
        return;
    }

    char c = 0;
    if (sc < sizeof(sc_ascii)) c = shift_state ? sc_ascii_shift[sc] : sc_ascii[sc];
    if (!c) { (void)alt_state; return; }

    /* Ctrl + 字母 → 控制码 */
    if (ctrl_state && c >= 'a' && c <= 'z') { push_key((u16)KEY_CTRL(c)); return; }
    if (ctrl_state && c >= 'A' && c <= 'Z') { push_key((u16)KEY_CTRL(c + 32)); return; }

    push_key((u16)(u8)c);
}

static void kbd_irq(regs_t *r) {
    (void)r;
    kbd_isr();
}

void kbd_init(void) {
    outb(0x64, 0xAE);            /* 使能键盘 */
    khead = ktail = 0;
    shift_state = ctrl_state = alt_state = e0_prefix = 0;
    register_irq(1, kbd_irq);
}

int kbd_getkey(void) {
    if (khead == ktail) return -1;
    u16 k = kbuf[ktail];
    ktail = (ktail + 1) % KBD_BUF_SIZE;
    return (int)k;
}

int kbd_haschar(void) { return khead != ktail; }

int kbd_getchar(void) {
    /* 只返回 ASCII；扩展键丢弃（编辑器请用 kbd_getkey/cons_getkey） */
    for (;;) {
        int k = kbd_getkey();
        if (k < 0) return -1;
        if (k < 0x100) return k;
    }
}
