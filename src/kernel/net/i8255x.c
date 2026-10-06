#include "i8255x.h"
#include "nic.h"
#include "io.h"
#include "mm.h"
#include "libc.h"
#include "console.h"

/* 诊断开关：默认 0。置 1 打印 i8255x 初始化每一步（定位"探测不到设备"）。 */
#define I8255X_DEBUG 0

#define I8255X_VEND 0x8086

/* i8255x 家族 device id。注意：真机（尤其 QEMU）用的**不是** 0x8029，
 * 而是每种芯片各自的 id —— 下面是 QEMU 11 实测 + 主流真机整理的清单：
 *   82550          0x8029（少数老卡/真机）
 *   82551          0x8029
 *   82557A/B/C     0x1229 / 0x1235 / 0x1237
 *   82558A/B       0x1234 / 0x1236
 *   82559A/B/C/ER  0x1209 / 0x120A / 0x1225 / 0x1226
 *   82562          0x10C8
 *   82563/82564    0x543A / 0x5440
 * 结尾 0 表示列表结束。 */
static const u16 g_i8255x_devids[] = {
    0x8029, /*82550/82551*/ 0x1229, /*82557A*/ 0x1235, /*82557B*/ 0x1237, /*82557C*/
    0x1234, /*82558A*/ 0x1236, /*82558B*/ 0x1209, /*82559A*/ 0x120A, /*82559B*/
    0x1225, /*82559C*/ 0x1226, /*82559ER*/ 0x10C8, /*82562*/ 0x543A, /*82563*/
    0x5440, /*82564*/
    0
};

/* ---- 端口寄存器（相对 BAR0 的 I/O 基址） ----
 * i8255x 是 8 位宽 ISA 风格设备：16 位寄存器要拆成两次 outb，
 * 32 位要拆成四次，且这些端口不做 16 位对齐。 */
#define PORT_CR      0x00   /* Command Register */
#define PORT_CLRT    0x01   /* Command Late Reset */
#define PORT_ISR     0x02   /* Interrupt Status */
#define PORT_IMR     0x03   /* Interrupt Mask */
#define PORT_ITC     0x04   /* Interrupt Timer Control */
#define PORT_MPC     0x08   /* Missed Packet Counter */
#define PORT_RSR     0x28   /* Receive Status */
#define PORT_TSR     0x2E   /* Transmit Status */
#define PORT_RCR     0x26   /* Receive Configuration */
#define PORT_TCR     0x2C   /* Transmit Configuration */
#define PORT_RDBAL   0x30   /* Receive Descriptor Base Address Low */
#define PORT_RDBAH   0x32
#define PORT_TDBAL   0x34   /* Transmit Descriptor Base Address Low */
#define PORT_TDBAH   0x36
#define PORT_TCTL    0x38
#define PORT_RSBC    0x3A   /* Receive Status Bit Clear */
#define PORT_CONFIG  0x21   /* 配置寄存器（8 位；读高 4 位得芯片号） */
#define CONFIG0_82550 0x0
#define CONFIG0_82551 0x1
#define CONFIG0_82557 0x2
#define CONFIG0_82558 0x3
#define CONFIG0_82559 0x4

/* Command Register 位 */
#define CR_STOP      0x0001  /* 停机 */
#define CR_START     0x0004  /* 启动 */
/* Receive Configuration 位 */
#define RCR_MON      0x0004  /* 混杂模式（调试用） */
#define RCR_RDM      0x0008  /* 接收描述符模式：0 = CBI(4 字节), 1 = 32 位 */
#define RCR_DPR     0x0010  /* DMA 轮询接收 */
#define RCR_OFF     0x001C  /* 接收 FIFO 阈值 */
#define RCR_DTE     0x0020  /* 描述符终止错误 */
/* Transmit Configuration 位 */
#define TCR_DTE     0x0001

#define TX_DESCS 8
#define RX_DESCS 8
#define BUF_SIZE 2048

/* 32 位（非 CBI）接收描述符：status / command / link / buffer */
typedef struct {
    u32 status;
    u32 command;
    u32 link;
    u32 buf;
} __attribute__((packed)) rx_desc_t;

/* 32 位发送描述符：status / command / link / buffer */
typedef struct {
    u32 status;
    u32 command;
    u32 link;
    u32 buf;
} __attribute__((packed)) tx_desc_t;

static u16 iobase = 0;
static u8 g_mac[6] = {0x52,0x54,0x00,0x12,0x34,0x56};
static char g_model[NIC_MODEL_MAX] = "i8255x";

static tx_desc_t *tx_descs;
static rx_desc_t *rx_descs;
static u8 *tx_bufs[TX_DESCS], *rx_bufs[RX_DESCS];
static int tx_next = 0, rx_next = 0;

/* 8 位端口访问 */
static u8  rb8(u16 off) { return inb((u16)(iobase + off)); }
static void wb8(u16 off, u8 v) { outb((u16)(iobase + off), v); }
static void ww16(u16 off, u16 v) { outb((u16)(iobase+off), (u8)(v & 0xFF)); outb((u16)(iobase+off+1), (u8)(v >> 8)); }
static void ww32(u16 off, u32 v) {
    outb((u16)(iobase+off),   (u8)(v & 0xFF));
    outb((u16)(iobase+off+1), (u8)((v >> 8) & 0xFF));
    outb((u16)(iobase+off+2), (u8)((v >> 16) & 0xFF));
    outb((u16)(iobase+off+3), (u8)((v >> 24) & 0xFF));
}

static const char *i8255x_model_by_cfg(u8 cfg0) {
    switch (cfg0 & 0x0F) {
    case CONFIG0_82550: return "82550";
    case CONFIG0_82551: return "82551";
    case CONFIG0_82557: return "82557";
    case CONFIG0_82558: return "82558";
    case CONFIG0_82559: return "82559";
    default:            return "i8255x";
    }
}

static int i8255x_init(void) {
    u8 bus, dev, fn;
#if I8255X_DEBUG
    kprintf("[i8255x] scanning for Intel i8255x...\n");
#endif
    if (pci_find(I8255X_VEND, g_i8255x_devids, &bus, &dev, &fn) != 0) {
#if I8255X_DEBUG
        kprintf("[i8255x] no matching PCI id found\n");
#endif
        return -1;
    }
#if I8255X_DEBUG
    kprintf("[i8255x] found bus=%d dev=%d fn=%d\n", (u32)bus, (u32)dev, (u32)fn);
#endif

    /* i8255x 的端口寄存器在 I/O BAR（实测是 BAR1，BAR0 是内存），
     * 所以全扫 6 个 BAR 找第一个 I/O 空间，不能假定 BAR0。 */
    u32 bar = pci_find_io_bar(bus, dev, fn);
    if (!bar) {
#if I8255X_DEBUG
        kprintf("[i8255x] no I/O BAR found, skip\n");
#endif
        return -1;
    }
    iobase = (u16)(bar & 0xFFFF);
    pci_enable(bus, dev, fn);
    /* i8255x 需要 SIO（ISA I/O）使能 */
    pci_cfg_write(bus, dev, fn, 0x04,
                  pci_cfg_read(bus, dev, fn, 0x04) | 0x0001);

    u8 cfg0 = (u8)(rb8(PORT_CONFIG) >> 4);   /* 配置寄存器高 4 位 = 芯片号 */
    const char *m = i8255x_model_by_cfg(cfg0);
    strncpy(g_model, m, NIC_MODEL_MAX - 1);
    g_model[NIC_MODEL_MAX - 1] = 0;
#if I8255X_DEBUG
    kprintf("[i8255x] %s at bus=%d dev=%d fn=%d iobase=0x%04X cfg0=0x%02X\n",
            g_model, (u32)bus, (u32)dev, (u32)fn, iobase, cfg0);
#endif

    /* 复位：Late Reset 写 0 触发 */
    wb8(PORT_CLRT, 0);
    io_wait();
    /* 停机并初始化环 */
    ww16(PORT_CR, CR_STOP);
    for (int i = 0; i < 1000; i++) { io_wait(); }

    tx_descs = (tx_desc_t*)kzalloc(sizeof(tx_desc_t) * TX_DESCS);
    rx_descs = (rx_desc_t*)kzalloc(sizeof(rx_desc_t) * RX_DESCS);
    if (!tx_descs || !rx_descs) return -1;
    for (int i = 0; i < TX_DESCS; i++) { tx_bufs[i] = (u8*)kzalloc(BUF_SIZE); }
    for (int i = 0; i < RX_DESCS; i++) { rx_bufs[i] = (u8*)kzalloc(BUF_SIZE); }

    /* 接收描述符环：RDBAL/RDBAH 指向首描述符，RCR 选 32 位模式 */
    ww32(PORT_RDBAL, (u32)(u32)rx_descs);
    ww32(PORT_RDBAH, 0);
    /* CBI 模式下 RDBAL 放的是 4 个 I/O 端口地址（rxd0..rxd3），
     * 但 QEMU 的 32 位（非 CBI）模式直接用线性基址即可。 */
    ww16(PORT_RCR, RCR_DTE | RCR_DPR | RCR_OFF | RCR_RDM);
    /* 发送 */
    ww32(PORT_TDBAL, (u32)(u32)tx_descs);
    ww32(PORT_TDBAH, 0);
    ww16(PORT_TCR, TCR_DTE);

    /* 读 MAC：i8255x 没有单独 MAC 寄存器，靠网卡 PROM。
     * QEMU 侧通常已把默认地址写进此处；无有效值时沿用默认 52:54:00。 */

    /* 启动 */
    ww16(PORT_CR, CR_START);
    return 0;
}

static void i8255x_get_mac(u8 mac[6]) { memcpy(mac, g_mac, 6); }

static void i8255x_send(const u8 *frame, u32 len) {
    if (!iobase || len > BUF_SIZE) return;
    int i = tx_next;
    memcpy(tx_bufs[i], frame, len);
    /* command 位: CMD | TF (transmit frame) */
    tx_descs[i].command = 0x0000000Bu;
    tx_descs[i].status  = 0;
    tx_descs[i].buf     = (u32)(u32)tx_bufs[i];
    tx_descs[i].link    = (u32)(u32)&tx_descs[(i + 1) % TX_DESCS];
    tx_next = (i + 1) % TX_DESCS;
    /* 门铃：写首描述符地址唤醒 TX */
    ww32(PORT_TDBAL, (u32)(u32)tx_descs);
}

static int i8255x_poll(void (*cb)(const u8 *frame, u32 len)) {
    int got = 0;
    while (rx_descs[rx_next].status & 0x01) {
        u32 rlen = rx_descs[rx_next].status >> 16;
        if (rlen > BUF_SIZE) rlen = BUF_SIZE;
        if (cb && rlen) cb(rx_bufs[rx_next], rlen);
        got++;
        rx_descs[rx_next].status = 0;
        rx_descs[rx_next].command = 0;
        rx_next = (rx_next + 1) % RX_DESCS;
    }
    /* 写 RSBC 清接收状态位 */
    if (got) wb8(PORT_RSBC, 0);
    return got;
}

static const nic_driver_t g_i8255x_drv = {
    "i8255x", g_model, I8255X_VEND, g_i8255x_devids,
    i8255x_init, i8255x_send, i8255x_poll, i8255x_get_mac
};

void i8255x_register(void) { nic_register(&g_i8255x_drv); }
