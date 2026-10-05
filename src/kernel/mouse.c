#include "mouse.h"
#include "io.h"
#include "idt.h"
#include "vga.h"
#include "libc.h"

/* ============================================================
 * PS/2 鼠标驱动（8042 控制器）
 * ------------------------------------------------------------
 * 8042 状态口 0x64 的三个关键位：
 *   bit0 = 输出缓冲满（数据可读，读 0x60）
 *   bit1 = 输入缓冲满（此时不能写 0x60/0x64）
 *   bit5 = 该字节来自辅助设备（鼠标），而不是键盘
 *
 * 旧实现把"等可写"和"等可读"两个条件写反了（while(inb&1) / while(!(inb&2))），
 * 只能靠超时兜底当成延时用；这里改成标准写法并让读取走超时判定。
 * ============================================================ */

static int mx = 40, my = 12;
static volatile u8 btns = 0;
static u8 prev_btns = 0;
static u8 packet[3];
static int pidx = 0;
static int g_present = 0;
static u32 g_pkt_ok = 0, g_pkt_bad = 0;

#define PS2_TIMEOUT 200000u

/* 等输出缓冲有数据可读 */
static int ps2_read(u8 *out) {
    for (u32 t = 0; t < PS2_TIMEOUT; t++) {
        if (inb(0x64) & 0x01) { *out = inb(0x60); return 0; }
    }
    return -1;
}
/* 等输入缓冲空，然后写命令口 */
static int ps2_cmd(u8 b) {
    for (u32 t = 0; t < PS2_TIMEOUT; t++) {
        if (!(inb(0x64) & 0x02)) { outb(0x64, b); return 0; }
    }
    return -1;
}
/* 等输入缓冲空，然后把 b 当作"下一个鼠标字节"发给 8042 */
static int ps2_data(u8 b) {
    for (u32 t = 0; t < PS2_TIMEOUT; t++) {
        if (!(inb(0x64) & 0x02)) { outb(0x60, b); return 0; }
    }
    return -1;
}
/* 丢弃输出缓冲里遗留的字节（键盘残留、上次 ACK 等） */
static void ps2_flush(void) {
    for (int i = 0; i < 64 && (inb(0x64) & 0x01); i++) (void)inb(0x60);
}

static int mouse_send(u8 b) { return ps2_cmd(0xD4) == 0 ? ps2_data(b) : -1; }

/* 读取一个字节；超时返回 -1 */
static int mouse_read(u8 *out) { return ps2_read(out); }

/* 发一条鼠标命令并等待 ACK(0xFA)。0xFE = resend，重发；
 * 其它字节（例如上一帧残留）先丢掉再看一两个字节。 */
static int mouse_cmd_ack(u8 cmd, int tries) {
    for (int t = 0; t < tries; t++) {
        if (mouse_send(cmd) != 0) continue;
        for (int k = 0; k < 4; k++) {
            u8 v;
            if (mouse_read(&v) != 0) break;
            if (v == 0xFA) return 0;
            if (v == 0xFE) break;          /* 要求重发 */
        }
    }
    return -1;
}

/* 初始化失败发生在哪一步（诊断用）。见 mouse_init_step() 的取值说明。 */
static int g_step = 0;
int mouse_init_step(void) { return g_step; }

static void mouse_irq(regs_t *r) {
    (void)r;
    u8 st = inb(0x64);
    if (!(st & 0x01)) return;       /* 没有数据（伪中断） */
    if (!(st & 0x20)) return;       /* bit5=0 说明是键盘数据，交给键盘驱动 */
    u8 d = inb(0x60);

    switch (pidx) {
    case 0:
        /* 首字节 bit3 必须为 1；否则是错位的数据流，丢帧重新同步 */
        if (!(d & 0x08)) { g_pkt_bad++; return; }
        packet[0] = d; pidx = 1;
        break;
    case 1:
        packet[1] = d; pidx = 2;
        break;
    default: {
        packet[2] = d; pidx = 0;
        g_pkt_ok++;
        int dx = (int)packet[1];
        int dy = (int)packet[2];
        if (packet[0] & 0x10) dx |= ~0xFF;   /* 符号扩展 */
        if (packet[0] & 0x20) dy |= ~0xFF;
        /* bit6/bit7 是溢出标志：此时位移已不可信，整帧丢弃 */
        if (!(packet[0] & 0xC0)) {
            mx += dx; my -= dy;
            if (mx < 0) mx = 0; if (mx >= VGA_WIDTH) mx = VGA_WIDTH - 1;
            if (my < 0) my = 0; if (my >= VGA_HEIGHT) my = VGA_HEIGHT - 1;
        }
        btns = (u8)(packet[0] & 0x07);
        break;
    }
    }
}

/* ---------------------------------------------------------------
 * 初始化期间的 8042 握手必须独占输入流。
 *
 * 关键坑：命令 0x20（读配置字节）的应答是**键盘侧**字节（QEMU 里
 * kbd_queue(mode, aux=0)），会触发 IRQ1。而 kbd_init() 已经把我们自己的
 * kbd_isr 挂上了 IRQ1，它一进中断就 inb(0x60) 把配置字节吃掉 ——
 * 于是我们这边轮询 0x64 的 bit0 永远等不到数据，握手直接超时。
 *
 * 解决办法：握手期间临时屏蔽 IRQ1，用纯轮询完成（所有等待本来就有超时，
 * 不依赖中断）。鼠标自己的应答（aux=1）此时 IRQ12 还没放行，天然安全。
 * --------------------------------------------------------------- */
static int mouse_handshake(void) {
    u8 cfg;
    for (int attempt = 0; attempt < 3; attempt++) {
        ps2_flush();

        if (ps2_cmd(0xA8) != 0) { g_step = 2; continue; }   /* 使能辅助端口 */

        g_step = 3;
        if (ps2_cmd(0x20) != 0) continue;                   /* 读配置字节 */
        if (ps2_read(&cfg) != 0) {
            /* 再给一次机会：可能刚被别的字节塞满 */
            if (ps2_read(&cfg) != 0) continue;
        }

        g_step = 4;
        u8 nb = (u8)(cfg | 0x02);          /* bit1: 打开 IRQ12 */
        nb &= (u8)~0x20;                   /* bit5: 0 = 鼠标时钟开启 */
        nb &= (u8)~0x10;                   /* bit4: 0 = 键盘时钟开启（别误伤键盘） */
        if (ps2_cmd(0x60) != 0) continue;                   /* 写配置字节 */
        if (ps2_data(nb) != 0) continue;
        return 0;
    }
    return -1;
}

/* 返回 0 = 检测到鼠标并成功启用上报 */
int mouse_init(void) {
    pidx = 0; btns = 0; prev_btns = 0; g_present = 0;
    g_pkt_ok = 0; g_pkt_bad = 0;
    g_step = 1;

    /* 握手期间让键盘中断闭嘴，避免它抢走 8042 的应答字节 */
    disable_irq(1);

    int rc = -1;
    if (mouse_handshake() != 0) goto out;

    /* ---- 先把鼠标复位到确定状态（0xFF -> ACK/自检通过/设备ID），
     *      失败再退回"直接设默认参数"的老路。 ---- */
    g_step = 5;
    int reset_ok = 0;
    if (mouse_send(0xFF) == 0) {
        u8 v;
        int got_ack = 0, got_pass = 0;
        for (int k = 0; k < 8; k++) {
            if (mouse_read(&v) != 0) break;
            if (v == 0xFA && !got_ack) { got_ack = 1; continue; }
            if (v == 0xAA && got_ack)  { got_pass = 1; continue; }
            if (v == 0x00 && got_pass) { got_pass = 2; break; }   /* 设备 ID */
        }
        reset_ok = (got_pass == 2);
        if (!reset_ok) ps2_flush();
    }

    g_step = 6;
    if (mouse_cmd_ack(0xF6, 3) != 0) { ps2_flush(); goto out; }   /* 默认参数 */

    g_step = 7;
    if (mouse_cmd_ack(0xF4, 3) != 0) { ps2_flush(); goto out; }   /* 打开上报 */

    g_step = reset_ok ? 9 : 8;      /* 8 = 走兼容路径，9 = 完整复位路径 */
    mx = VGA_WIDTH / 2; my = VGA_HEIGHT / 2;
    register_irq(12, mouse_irq);
    g_present = 1;
    rc = 0;

out:
    ps2_flush();
    enable_irq(1);                 /* 恢复键盘中断 */
    return rc;
}

int mouse_present(void) { return g_present; }

void mouse_state(int *x, int *y, u8 *buttons) {
    if (x) *x = mx;
    if (y) *y = my;
    if (buttons) *buttons = btns;
}

void mouse_set_position(int x, int y) {
    if (x < 0) x = 0; if (x >= VGA_WIDTH) x = VGA_WIDTH - 1;
    if (y < 0) y = 0; if (y >= VGA_HEIGHT) y = VGA_HEIGHT - 1;
    mx = x; my = y;
}

u32 mouse_packets_ok(void)  { return g_pkt_ok; }
u32 mouse_packets_bad(void) { return g_pkt_bad; }

static int edge(u8 mask) {
    int c = (btns & mask) && !(prev_btns & mask);
    prev_btns = btns;
    return c;
}
int mouse_consume_click(void)  { return edge(1); }
int mouse_consume_rclick(void) { return edge(2); }
int mouse_middle_down(void)    { return (btns & 4) ? 1 : 0; }
