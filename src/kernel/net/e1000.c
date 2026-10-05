#include "e1000.h"
#include "io.h"
#include "mm.h"
#include "libc.h"
#include "console.h"

/* 诊断开关：默认 0。置 1 可在串口打印网卡初始化/发送的原始寄存器值，
 * 用来回答"帧到底有没有交给硬件"这类问题。 */
#define E1000_DEBUG 0

#define E1000_VEND 0x8086
#define E1000_DEVID 0x100E   /* 82540EM，QEMU 默认 */

/* 寄存器偏移 */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_RCTRL   0x0100
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818

#define CTRL_RST    (1u << 26)
#define RCTL_EN     (1u << 1)
#define RCTL_BAM    (1u << 15)
#define RCTL_SECRC  (1u << 26)
#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)

#define TX_DESCS 32
#define RX_DESCS 32
#define BUF_SIZE 2048

/* 8254x 传统（legacy）描述符：16 字节 */
typedef struct {
    u64 addr;
    u16 len;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} __attribute__((packed)) desc_t;

static volatile u32 *mmio = NULL;
static u8 g_mac[6] = {0x52,0x54,0x00,0x12,0x34,0x56};

static desc_t *tx_descs, *rx_descs;
static u8 *tx_bufs[TX_DESCS], *rx_bufs[RX_DESCS];
static int tx_next = 0, rx_next = 0;

static u32 pci_read(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 addr = 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 val) {
    u32 addr = 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

static u32 rd(u32 r) { return mmio[r/4]; }
static void wr(u32 r, u32 v) { mmio[r/4] = v; }

int e1000_init(void) {
    /* 扫描 PCI 总线找 e1000。
     * 曾经的坑：外层 for 的 bus++ 在内层 break 之后依然会执行，
     * 于是拿着已经自增过的 bus 去读 BAR，读到 0xFFFFFFFF（不存在），
     * 结果网卡 MMIO 指向 0xFFFFFFF0（绕回物理内存低端），
     * 寄存器读写全是垃圾——表现为"能探测到网卡但一帧也发不出去"。
     * 因此这里把命中的 bus/dev 单独存下来，不再复用循环变量。 */
    int found = 0, fbus = 0, fdev = 0;
    for (int b = 0; b < 8 && !found; b++)
        for (int d = 0; d < 32; d++) {
            u32 vid = pci_read((u8)b, (u8)d, 0, 0);
            if ((vid & 0xFFFF) != E1000_VEND) continue;
            u32 did = (pci_read((u8)b, (u8)d, 0, 0) >> 16) & 0xFFFF;
#if E1000_DEBUG
            kprintf("[e1000] scan bus=%d dev=%d vid=%04X did=%04X\n", (u32)b, (u32)d,
                    (u32)(vid & 0xFFFF), (u32)did);
#endif
            if (did == E1000_DEVID) { found = 1; fbus = b; fdev = d; break; }
        }
    if (!found) return -1;
    u32 bar0raw = pci_read((u8)fbus, (u8)fdev, 0, 0x10);
    u32 bar0 = bar0raw & 0xFFFFFFF0;
    if (!bar0) return -1;
    mmio = (volatile u32*)(u32)bar0;
    /* 使能总线主控 */
    u32 cmd = pci_read((u8)fbus, (u8)fdev, 0, 0x04);
    pci_write((u8)fbus, (u8)fdev, 0, 0x04, cmd | 0x06);
#if E1000_DEBUG
    kprintf("[e1000] found at bus=%d dev=%d bar0raw=0x%08X cmd=0x%04X\n",
            (u32)fbus, (u32)fdev, bar0raw, (u32)(cmd & 0xFFFF));
#endif

    /* 软复位 */
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    while (rd(REG_CTRL) & CTRL_RST);

    /* 分配描述符环与缓冲 */
    tx_descs = (desc_t*)kzalloc(sizeof(desc_t) * TX_DESCS);
    rx_descs = (desc_t*)kzalloc(sizeof(desc_t) * RX_DESCS);
    for (int i = 0; i < TX_DESCS; i++) { tx_bufs[i] = (u8*)kzalloc(BUF_SIZE); tx_descs[i].addr = (u64)(u32)tx_bufs[i]; }
    for (int i = 0; i < RX_DESCS; i++) { rx_bufs[i] = (u8*)kzalloc(BUF_SIZE); rx_descs[i].addr = (u64)(u32)rx_bufs[i]; rx_descs[i].status = 0; }

    /* 接收环 */
    wr(REG_RDBAL, (u32)(u32)rx_descs);
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, sizeof(desc_t) * RX_DESCS);
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_DESCS - 1);
    wr(REG_RCTRL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    /* 发送环 */
    wr(REG_TDBAL, (u32)(u32)tx_descs);
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, sizeof(desc_t) * TX_DESCS);
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10 << 4) | (0x40 << 12));
    wr(REG_TIPG, 0x0060200A);

    /* 读取 MAC（RAH/RAL 已含 QEMU 分配的默认地址） */
    u32 ral = rd(0x5400), rah = rd(0x5404);
#if E1000_DEBUG
    kprintf("[e1000] bar0=0x%08X status=0x%08X ctrl=0x%08X tctl=0x%08X rctl=0x%08X\n",
            bar0, rd(REG_STATUS), rd(REG_CTRL), rd(REG_TCTL), rd(REG_RCTRL));
    kprintf("[e1000] ral=0x%08X rah=0x%08X rx=0x%08X tx=0x%08X\n",
            ral, rah, (u32)(u32)rx_descs, (u32)(u32)tx_descs);
#endif
    u8 m0 = ral & 0xFF, m1 = (ral>>8)&0xFF, m2 = (ral>>16)&0xFF, m3 = (ral>>24)&0xFF;
    u8 m4 = rah & 0xFF, m5 = (rah>>8)&0xFF;
    u32 acc = (u32)m0|((u32)m1<<8)|((u32)m2<<16)|((u32)m3<<24);
    if (acc != 0 && acc != 0xFFFFFFFFu) {
        g_mac[0]=m0; g_mac[1]=m1; g_mac[2]=m2; g_mac[3]=m3; g_mac[4]=m4; g_mac[5]=m5;
    }
    return 0;
}

void e1000_get_mac(u8 mac[6]) { memcpy(mac, g_mac, 6); }

void e1000_send(const u8 *frame, u32 len) {
    if (!mmio || len > BUF_SIZE) return;
    int i = tx_next;
    memcpy(tx_bufs[i], frame, len);
    tx_descs[i].len = (u16)len;
    tx_descs[i].cmd = 0x0B;   /* EOP | IFCS | RS */
    tx_descs[i].status = 0;
    tx_next = (tx_next + 1) % TX_DESCS;
    wr(REG_TDT, tx_next);
#if E1000_DEBUG
    kprintf("[e1000] TX d=%d len=%u cmd=0x%02X buf=0x%08X tdt=%d\n",
            i, (u32)len, (u32)tx_descs[i].cmd, (u32)(u32)tx_bufs[i], tx_next);
#endif
}

int e1000_poll(void (*cb)(const u8 *frame, u32 len)) {
    int got = 0;
    while (rx_descs[rx_next].status & 0x01) {
        u16 len = rx_descs[rx_next].len;
        if (cb) cb(rx_bufs[rx_next], len);
        got++;
        rx_descs[rx_next].status = 0;
        int tail = (rx_next - 1 + RX_DESCS) % RX_DESCS;
        wr(REG_RDT, tail);
        rx_next = (rx_next + 1) % RX_DESCS;
    }
    return got;
}
