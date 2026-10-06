#include "e1000.h"
#include "nic.h"
#include "mm.h"
#include "libc.h"
#include "console.h"

/* 诊断开关：默认 0。置 1 可在串口打印网卡初始化/发送的原始寄存器值。 */
#define E1000_DEBUG 0

/* Intel 的 PCI vendor id */
#define E1000_VEND 0x8086

/* 本驱动只覆盖**传统 e1000**（8254x/8255x + ICH8/ICH9 集成 82562/63/64）。
 * 这些型号共用同一套 legacy 描述符 + 寄存器布局。
 *
 * ⚠️ 8257x/8256x-PCIe 系列（82571/72/73/74/75/76/77/78/79/83/86…）虽然同属
 * Intel、同 vendor id 0x8086，但寄存器布局与 MAC 复位流程**不兼容**，
 * 归 e1000e 驱动管（见 e1000e.c）。两个驱动的 device id 列表必须互不重叠，
 * 否则先注册的 e1000 会抢走 e1000e 的设备并初始化失败。 */
static const u16 g_e1000_devids[] = {
    0x1002, /*82540C*/ 0x1004, /*82544GC*/ 0x1008, /*82544GF*/ 0x1009, /*82544GA*/
    0x100E, /*82540EM*/ 0x100F, /*82545EM*/ 0x1010, /*82542*/ 0x1012, /*82543*/
    0x1013, /*82544*/  0x1014, /*82545EM*/ 0x1015, /*82545EM*/ 0x1016, /*82545EM*/
    0x1026, /*82546*/  0x1027, /*82547*/  0x1028, /*82548*/  0x1029, /*82549*/
    0x102A, /*82546D*/ 0x102B, /*82549B*/ 0x102C, /*82549*/  0x102D, /*82549*/
    0x102E, /*82549*/  0x1032, /*82564L*/ 0x1038, /*82562*/  0x1048, /*82563*/
    0x1A0C, /*82550*/  0x1A0D, /*82551*/
    0x10D8, /*82563*/  0x10D9, /*82562*/  0x10E6, /*82564*/
    0
};

/* device id -> 型号名，让 net info 能报出具体型号 */
static const char *e1000_model_of(u16 did) {
    switch (did) {
    case 0x1002: return "82540C";
    case 0x1004: return "82544GC";
    case 0x1008: return "82544GF";
    case 0x1009: return "82544GA";
    case 0x100E: return "82540EM";
    case 0x100F: return "82545EM";
    case 0x1010: return "82542";
    case 0x1012: return "82543";
    case 0x1013: return "82544";
    case 0x1014: return "82545EM";
    case 0x1015: return "82545EM";
    case 0x1016: return "82545EM";
    case 0x1026: return "82546";
    case 0x1027: return "82547";
    case 0x1028: return "82548";
    case 0x1029: return "82549";
    case 0x102A: return "82546D";
    case 0x102B: return "82549B";
    case 0x102C: return "82549";
    case 0x102D: return "82549";
    case 0x102E: return "82549";
    case 0x1032: return "82564L";
    case 0x1038: return "82562";
    case 0x1048: return "82563";
    case 0x1A0C: return "82550";
    case 0x1A0D: return "82551";
    case 0x10D8: return "82563";
    case 0x10D9: return "82562";
    case 0x10E6: return "82564";
    default:     return "e1000";
    }
}

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
/* 型号缓冲：init() 命中具体 device id 后写入，net info 读它 */
static char g_model[NIC_MODEL_MAX] = "e1000";

static desc_t *tx_descs, *rx_descs;
static u8 *tx_bufs[TX_DESCS], *rx_bufs[RX_DESCS];
static int tx_next = 0, rx_next = 0;

static u32 rd(u32 r) { return mmio[r/4]; }
static void wr(u32 r, u32 v) { mmio[r/4] = v; }

static int e1000_init(void) {
    u8 bus, dev, fn;
    if (pci_find(E1000_VEND, g_e1000_devids, &bus, &dev, &fn) != 0) return -1;

    u16 did = (u16)((pci_cfg_read(bus, dev, fn, 0x00) >> 16) & 0xFFFF);
    const char *m = e1000_model_of(did);
    strncpy(g_model, m, NIC_MODEL_MAX - 1);
    g_model[NIC_MODEL_MAX - 1] = 0;

    /* 内存 BAR：本驱动假定 BAR0 是 32bit 内存 BAR（内核未开分页，直接用物理地址） */
    if (!pci_bar_is_mem(bus, dev, fn, 0)) return -1;
    u32 bar0 = pci_bar(bus, dev, fn, 0);
    if (!bar0) return -1;
    mmio = (volatile u32*)(u32)bar0;
    pci_enable(bus, dev, fn);
#if E1000_DEBUG
    kprintf("[e1000] %s at bus=%d dev=%d fn=%d bar0=0x%08X\n",
            g_model, (u32)bus, (u32)dev, (u32)fn, bar0);
#endif

    /* 软复位 */
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    while (rd(REG_CTRL) & CTRL_RST);

    /* 分配描述符环与缓冲 */
    tx_descs = (desc_t*)kzalloc(sizeof(desc_t) * TX_DESCS);
    rx_descs = (desc_t*)kzalloc(sizeof(desc_t) * RX_DESCS);
    if (!tx_descs || !rx_descs) return -1;
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

    /* 读取 MAC（RAH/RAL 已含 QEMU/固件写的默认地址） */
    u32 ral = rd(0x5400), rah = rd(0x5404);
    u8 m0 = ral & 0xFF, m1 = (ral>>8)&0xFF, m2 = (ral>>16)&0xFF, m3 = (ral>>24)&0xFF;
    u8 m4 = rah & 0xFF, m5 = (rah>>8)&0xFF;
    u32 acc = (u32)m0|((u32)m1<<8)|((u32)m2<<16)|((u32)m3<<24);
    if (acc != 0 && acc != 0xFFFFFFFFu) {
        g_mac[0]=m0; g_mac[1]=m1; g_mac[2]=m2; g_mac[3]=m3; g_mac[4]=m4; g_mac[5]=m5;
    }
    return 0;
}

static void e1000_get_mac(u8 mac[6]) { memcpy(mac, g_mac, 6); }

static void e1000_send(const u8 *frame, u32 len) {
    if (!mmio || len > BUF_SIZE) return;
    int i = tx_next;
    memcpy(tx_bufs[i], frame, len);
    tx_descs[i].len = (u16)len;
    tx_descs[i].cmd = 0x0B;   /* EOP | IFCS | RS */
    tx_descs[i].status = 0;
    tx_next = (tx_next + 1) % TX_DESCS;
    wr(REG_TDT, tx_next);
}

static int e1000_poll(void (*cb)(const u8 *frame, u32 len)) {
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

static const nic_driver_t g_e1000_drv = {
    "e1000", g_model, E1000_VEND, g_e1000_devids,
    e1000_init, e1000_send, e1000_poll, e1000_get_mac
};

void e1000_register(void) { nic_register(&g_e1000_drv); }
