#include "e1000e.h"
#include "nic.h"
#include "io.h"
#include "mm.h"
#include "libc.h"
#include "console.h"

/* 诊断开关：默认 0 */
#define E1000E_DEBUG 0

#define E1000E_VEND 0x8086

/* 8257x/8256x-PCIe 系列 device id：
 *   82571        0x10D3      82572        0x10E7
 *   82573        0x10D0      82574        0x10D1   <- QEMU e1000e
 *   82575        0x10A2      82576        0x10A3
 *   82577        0x1076      82578        0x1077
 *   82579        0x10D4      82583        0x10D6
 *   82586        0x10D7      82564L       0x10A6
 *   82562(ICH8)  0x10D9      82563(ICH8)  0x10D8
 *   82564(ICH9)  0x10E6
 * 注意：与 e1000.c 的列表必须互不重叠（这里只放走 e1000e 布局的型号）。 */
static const u16 g_e1000e_devids[] = {
    0x10D3, /*82571*/ 0x10E7, /*82572*/ 0x10D0, /*82573*/ 0x10D1, /*82574*/
    0x10A2, /*82575*/ 0x10A3, /*82576*/ 0x1076, /*82577*/ 0x1077, /*82578*/
    0x10D4, /*82579*/ 0x10D6, /*82583*/ 0x10D7, /*82586*/ 0x10A6, /*82564L*/
    0
};

static const char *e1000e_model_of(u16 did) {
    switch (did) {
    case 0x10D3: return "82571";
    case 0x10E7: return "82572";
    case 0x10D0: return "82573";
    case 0x10D1: return "82574";
    case 0x10A2: return "82575";
    case 0x10A3: return "82576";
    case 0x1076: return "82577";
    case 0x1077: return "82578";
    case 0x10D4: return "82579";
    case 0x10D6: return "82583";
    case 0x10D7: return "82586";
    case 0x10A6: return "82564L";
    default:     return "e1000e";
    }
}

/* ---- 寄存器偏移（e1000e / 8257x 布局） ---- */
#define REG_CTRL     0x00000   /* 设备控制 */
#define REG_STATUS   0x00008
#define REG_EECD     0x00010   /* EEPROM 控制 */
#define REG_EERD     0x00014
#define REG_RCTL     0x00100   /* 接收控制 */
#define REG_RDBAL    0x02800   /* 接收描述符环基址（低） */
#define REG_RDBAH    0x02804
#define REG_RDLEN    0x02808
#define REG_RDH      0x02810
#define REG_RDT      0x02818
#define REG_TCTL     0x00400   /* 发送控制 */
#define REG_TDBAL    0x03800
#define REG_TDBAH    0x03804
#define REG_TDLEN    0x03808
#define REG_TDH      0x03810
#define REG_TDT      0x03818
#define REG_TIPG     0x00410
#define REG_RAL      0x05400   /* MAC 地址低 32 位 */
#define REG_RAH      0x05404   /* MAC 地址高 16 位 */

#define CTRL_RST     (1u << 26)
#define CTRL_PHY_RST (1u << 31)
#define RCTL_EN      (1u << 1)
#define RCTL_BAM     (1u << 15)
#define RCTL_SECRC   (1u << 26)
#define TCTL_EN      (1u << 1)
#define TCTL_PSP     (1u << 3)
#define RAH_AV       (1u << 16) /* RAH bit16 = 地址有效 */

#define TX_DESCS 32
#define RX_DESCS 32
#define BUF_SIZE 2048

/* 8257x 传统（legacy）描述符：16 字节，布局与 8254x 兼容 */
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
static char g_model[NIC_MODEL_MAX] = "e1000e";

static desc_t *tx_descs, *rx_descs;
static u8 *tx_bufs[TX_DESCS], *rx_bufs[RX_DESCS];
static int tx_next = 0, rx_next = 0;

static u32 rd(u32 r) { return mmio[r/4]; }
static void wr(u32 r, u32 v) { mmio[r/4] = v; }

static int e1000e_init(void) {
    u8 bus, dev, fn;
    if (pci_find(E1000E_VEND, g_e1000e_devids, &bus, &dev, &fn) != 0) return -1;

    u16 did = (u16)((pci_cfg_read(bus, dev, fn, 0x00) >> 16) & 0xFFFF);
    const char *m = e1000e_model_of(did);
    strncpy(g_model, m, NIC_MODEL_MAX - 1);
    g_model[NIC_MODEL_MAX - 1] = 0;

    if (!pci_bar_is_mem(bus, dev, fn, 0)) return -1;
    u32 bar0 = pci_bar(bus, dev, fn, 0);
    if (!bar0) return -1;
    mmio = (volatile u32*)(u32)bar0;
    pci_enable(bus, dev, fn);
#if E1000E_DEBUG
    kprintf("[e1000e] %s at bus=%d dev=%d fn=%d bar0=0x%08X\n",
            g_model, (u32)bus, (u32)dev, (u32)fn, bar0);
#endif

    /* 8257x 复位序列与 8254x 不同：先 PHY 复位再全局软复位，
     * 且全局复位位读回为 1 表示复位完成（8254x 是复位中为 1）。 */
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_PHY_RST);
    io_wait();
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    while (rd(REG_CTRL) & CTRL_RST) io_wait();

    /* 清中断 */
    wr(REG_STATUS, 0xFFFFFFFFu);

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
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    /* 发送环 */
    wr(REG_TDBAL, (u32)(u32)tx_descs);
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, sizeof(desc_t) * TX_DESCS);
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10 << 4) | (0x40 << 12));
    wr(REG_TIPG, 0x0060200A);

    /* 读 MAC：8257x 需要先确保 RAH 的 AV 位置 1 才会锁存地址 */
    u32 ral = rd(REG_RAL);
    wr(REG_RAH, rd(REG_RAH) | RAH_AV);
    u32 rah = rd(REG_RAH);
#if E1000E_DEBUG
    kprintf("[e1000e] ral=0x%08X rah=0x%08X\n", ral, rah);
#endif
    u8 m0 = ral & 0xFF, m1 = (ral>>8)&0xFF, m2 = (ral>>16)&0xFF, m3 = (ral>>24)&0xFF;
    u8 m4 = rah & 0xFF, m5 = (rah>>8)&0xFF;
    u32 acc = (u32)m0|((u32)m1<<8)|((u32)m2<<16)|((u32)m3<<24);
    if (acc != 0 && acc != 0xFFFFFFFFu) {
        g_mac[0]=m0; g_mac[1]=m1; g_mac[2]=m2; g_mac[3]=m3; g_mac[4]=m4; g_mac[5]=m5;
    }
    return 0;
}

static void e1000e_get_mac(u8 mac[6]) { memcpy(mac, g_mac, 6); }

static void e1000e_send(const u8 *frame, u32 len) {
    if (!mmio || len > BUF_SIZE) return;
    int i = tx_next;
    memcpy(tx_bufs[i], frame, len);
    tx_descs[i].len = (u16)len;
    tx_descs[i].cmd = 0x0B;   /* EOP | IFCS | RS */
    tx_descs[i].status = 0;
    tx_next = (tx_next + 1) % TX_DESCS;
    wr(REG_TDT, tx_next);
}

static int e1000e_poll(void (*cb)(const u8 *frame, u32 len)) {
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

static const nic_driver_t g_e1000e_drv = {
    "e1000e", g_model, E1000E_VEND, g_e1000e_devids,
    e1000e_init, e1000e_send, e1000e_poll, e1000e_get_mac
};

void e1000e_register(void) { nic_register(&g_e1000e_drv); }
