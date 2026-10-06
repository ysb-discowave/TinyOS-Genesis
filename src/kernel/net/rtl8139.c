#include "rtl8139.h"
#include "nic.h"
#include "io.h"
#include "mm.h"
#include "libc.h"
#include "console.h"

/* 诊断开关：默认 0 */
#define RTL_DEBUG 0

#define RTL_VEND  0x10EC
static const u16 g_rtl_devids[] = { 0x8139, 0 };

/* ---- I/O 端口寄存器（相对 BAR0 的 I/O 基址） ---- */
#define REG_IDR0    0x00   /* MAC 0..5 */
#define REG_TSD     0x10   /* Transmit Status (32 位, 4 个缓冲) */
#define REG_TSAD    0x20   /* Transmit Start Address (32 位 ×4) */
#define REG_RBTR    0x20   /* Receive Buffer Trigger (16 位) —— 与 TSAD 同址低字节区 */
#define REG_CAPR    0x30   /* Current Address Packet Register (16 位) */
#define REG_CBR     0x32   /* Current Buffer Register (16 位) */
#define REG_IMR     0x3C
#define REG_ISR     0x3E
#define REG_TCR     0x40
#define REG_RCR     0x44
#define REG_CONFIG  0x52
#define REG_RBSTART 0x60
#define REG_RBEND   0x64
#define REG_CR      0x37

/* Command Register 位 */
#define CR_RST      0x10
#define CR_TEN      0x04
#define CR_REN      0x08

/* RTL8139 接收缓冲布局：8KB(0x2000) 头 + 16*2 头尾 + 8K 数据 = 8384 字节。
 * 硬件要求 RBSTART..RBEND 覆盖 8192+16+16。 */
#define RX_HDR_SIZE  (8192 + 16)
#define RX_BUF_LEN   8384
#define TX_BUF_SIZE  1792   /* 每帧上界（含 4 字节 CRC），MTU 1500 足够 */
#define TX_SLOTS     4

static u16 iobase = 0;
static u8 g_mac[6] = {0x52,0x54,0x00,0x12,0x34,0x57};
static char g_model[NIC_MODEL_MAX] = "RTL8139";
static u8 *g_rxbuf = NULL;                 /* 一次分配，RX + 4 个 TX 缓冲都放这里 */
static u32 g_rxbuf_phys = 0;
static u16 g_rx_cur = 0;                   /* 软件维护的读游标（CAPR 之前） */
static int g_tx_rr = 0;

static u8 rb8(u16 off) { return inb((u16)(iobase + off)); }
static void wb8(u16 off, u8 v) { outb((u16)(iobase + off), v); }
static u16 rw16(u16 off) { return (u16)(inb((u16)(iobase+off)) | (inb((u16)(iobase+off+1)) << 8)); }
static void ww16(u16 off, u16 v) { outb((u16)(iobase+off), (u8)(v & 0xFF)); outb((u16)(iobase+off+1), (u8)(v >> 8)); }
static void wl32(u16 off, u32 v) { outl((u16)(iobase + off), v); }

/* 收一个包。RTL8139 接收缓冲里每帧前 4 字节是
 *   [0..1] status (bit0 = ROK 即"包有效"), [2..3] length (低 11 位有效, 含 CRC)
 * 两字段都是独立的 16 位小端值。 */
static u16 rtl_pkt_len_at(u16 cur) {
    u16 sl = (u16)(g_rxbuf[(u16)(cur + 2) % RX_BUF_LEN] |
                   (u16)(g_rxbuf[(u16)(cur + 3) % RX_BUF_LEN] << 8));
    return (u16)(sl & 0x07FF);   /* 长度占低 11 位 */
}

static int rtl8139_init(void) {
    u8 bus, dev, fn;
    if (pci_find(RTL_VEND, g_rtl_devids, &bus, &dev, &fn) != 0) return -1;

    if (pci_bar_is_mem(bus, dev, fn, 0)) return -1;
    u32 bar = pci_bar(bus, dev, fn, 0);
    if (!bar) return -1;
    iobase = (u16)(bar & 0xFFFF);
    pci_enable(bus, dev, fn);
#if RTL_DEBUG
    kprintf("[rtl8139] bus=%d dev=%d iobase=0x%04X\n", (u32)bus, (u32)dev, iobase);
#endif

    for (int i = 0; i < 6; i++) g_mac[i] = rb8((u16)(REG_IDR0 + i));
#if RTL_DEBUG
    kprintf("[rtl8139] mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
            g_mac[0],g_mac[1],g_mac[2],g_mac[3],g_mac[4],g_mac[5]);
#endif

    /* 一块连续内存：接收缓冲 + 4 个发送缓冲 */
    g_rxbuf = (u8*)kzalloc(RX_BUF_LEN + TX_BUF_SIZE * TX_SLOTS);
    if (!g_rxbuf) return -1;
    g_rxbuf_phys = (u32)(u32)g_rxbuf;

    /* 软件复位：CR 置 RST 后须自清 */
    wb8(REG_CR, CR_RST);
    for (int i = 0; i < 200000 && (rb8(REG_CR) & CR_RST); i++) io_wait();
    if (rb8(REG_CR) & CR_RST) return -1;

    /* 接收缓冲起止（物理地址） */
    wl32(REG_RBSTART, g_rxbuf_phys);
    wl32(REG_RBEND,   g_rxbuf_phys + RX_BUF_LEN - 1);
    /* 软件读游标 = 0；对应 CAPR = 0 + 16 */
    g_rx_cur = 0;
    u16 capr0 = 16;
    wb8(REG_CAPR, (u8)(capr0 & 0xFF));
    wb8((u16)(REG_CAPR + 1), (u8)((capr0 >> 8) & 0x1F));
    /* 接收配置：0x000F = 收广播+组播、8 字节阈值、不环绕。
     * 不能写 0 —— 阈值位为 00 且关闭了 AB/AM，网卡可能整包不收。 */
    ww16(REG_RCR, 0x000F);

    /* 4 个发送缓冲的物理地址，挂在 RX 区之后的连续内存 */
    for (int i = 0; i < TX_SLOTS; i++)
        wl32(REG_TSAD + i * 4, g_rxbuf_phys + RX_BUF_LEN + (u32)(TX_BUF_SIZE * i));

    /* 置 100Mbps 全双工（CONFIG 0x10 = 0x100） */
    wb8(REG_CONFIG, 0x00);

    /* 使能 TX/RX；清一次 ISR */
    wb8(REG_ISR, 0xFF);
    wb8(REG_CR, CR_TEN | CR_REN);
    return 0;
}

static void rtl8139_get_mac(u8 mac[6]) { memcpy(mac, g_mac, 6); }

static void rtl8139_send(const u8 *frame, u32 len) {
    if (!iobase || !g_rxbuf || len > TX_BUF_SIZE) return;
    int slot = g_tx_rr; g_tx_rr = (g_tx_rr + 1) % TX_SLOTS;
    u8 *tb = (u8*)(g_rxbuf + RX_BUF_LEN + TX_BUF_SIZE * slot);
    memcpy(tb, frame, len);
    /* 该槽的 TSAD 寄存器在 0x20 + 4*slot；写此槽地址即启动发送 */
    wl32(REG_TSAD + slot * 4, g_rxbuf_phys + RX_BUF_LEN + (u32)(TX_BUF_SIZE * slot));
}

static int rtl8139_poll(void (*cb)(const u8 *frame, u32 len)) {
    if (!g_rxbuf) return 0;
    /* RTL8139 标准惯例：CAPR 保存"下一待读帧起点 − 16"，
     * 所以当前应读位置 = (CAPR − 16) & mask。 */
    u16 capr = (u16)(rw16(REG_CAPR) & 0x1FFF);
    u16 hw_next = (u16)((capr + RX_BUF_LEN - 16) % RX_BUF_LEN);
    if (hw_next == g_rx_cur) return 0;

    int got = 0;
    while (g_rx_cur != hw_next) {
        u16 start = g_rx_cur;
        u8  status = g_rxbuf[start];               /* bit0 = ROK */
        u16 plen  = rtl_pkt_len_at(start);          /* 含 4 字节头 + CRC */
        if (plen < 4 || plen > 1522 || !(status & 0x01)) {
            g_rx_cur = (u16)((start + 16) % RX_BUF_LEN);
            if (g_rx_cur == hw_next) break;
            continue;
        }
        u16 data_len = (u16)(plen - 4);
        if (cb && data_len) cb((const u8*)(g_rxbuf + ((start + 4) % RX_BUF_LEN)), data_len);
        got++;
        u16 adv = (u16)((start + 4 + ((data_len + 3) & ~3u)) % RX_BUF_LEN);
        g_rx_cur = adv;
    }
    /* 通知硬件：CAPR = (下一待读位置 + 16) & mask */
    u16 capr_new = (u16)((g_rx_cur + 16) % RX_BUF_LEN);
    wb8(REG_CAPR, (u8)(capr_new & 0xFF));
    wb8((u16)(REG_CAPR + 1), (u8)((capr_new >> 8) & 0x1F));
    wb8(REG_ISR, 0x01);
    return got;
}

static const nic_driver_t g_rtl8139_drv = {
    "rtl8139", g_model, RTL_VEND, g_rtl_devids,
    rtl8139_init, rtl8139_send, rtl8139_poll, rtl8139_get_mac
};

void rtl8139_register(void) { nic_register(&g_rtl8139_drv); }
