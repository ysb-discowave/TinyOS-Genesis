#include "virtio_net.h"
#include "nic.h"
#include "io.h"
#include "mm.h"
#include "libc.h"
#include "console.h"

/* virtio-net —— 半虚拟化网卡。QEMU/KVM/云主机上最常见的 guest 网卡。
 * 这里实现 legacy (0.9) PCI transport：I/O 空间寄存器 + QueuePFN 队列。
 * 队列共享内存布局（legacy 规定，物理连续、按 4096 对齐）：
 *   [描述符表 desc[Q_SIZE]] [avail ring] [padding] [used ring]
 */

#define VIRTIO_VEND 0x1AF4
static const u16 g_virtio_net_devids[] = {
    0x1000, /* legacy net */ 0x1041, /* modern net 1.0 */
    0
};

#define Q_RX 0
#define Q_TX 1
#define Q_SIZE 8

/* ---- I/O 寄存器（相对 BAR0 的 I/O 基址） ---- */
#define VIO_STATUS   0x50  /* u8 */
#define VIO_ISR      0x60  /* u8 */
#define VIO_QUEUESEL 0x12  /* u16：先写队列号，再写 0x10 处的 QueueNotify */
#define VIO_QNUM     0x0E  /* u16 队列大小 */
#define VIO_QALIGN   0x0C  /* u32 队列对齐 */
#define VIO_QPFN     0x08  /* u32 队列物理页帧号（legacy） */
#define VIO_NOTIFY_ADDR 0x10 /* u16 队列号（通知用） */

/* 设备状态位 */
#define VS_ACK        0x01
#define VS_DRIVER     0x02
#define VS_DRIVER_OK  0x04
#define VS_FEATURES_OK 0x08

/* 描述符 flag */
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

/* virtio-net 报文头：guest 填，设备读 */
struct vnet_hdr {
    u8  flags;
    u8  gso_type;
    u16 hdr_len;
    u16 gso_size;
    u16 csum_start;
    u16 csum_offset;
};
#define VNET_HDR_LEN 10

/* 描述符（legacy 16 字节） */
struct vq_desc { u64 addr; u32 len; u16 flags; u16 next; } __attribute__((packed));

/* avail ring：flags + idx + ring[] + used_event */
struct vq_avail {
    u16 flags;
    u16 idx;
    u16 ring[Q_SIZE];
    u16 used_event;
} __attribute__((packed));

/* used ring：flags + idx + ring[] + avail_event */
struct vq_used {
    u16 flags;
    u16 idx;
    struct { u32 id; u32 len; } ring[Q_SIZE];
    u16 avail_event;
} __attribute__((packed));

/* 一套队列共享内存：desc + avail + used（顺序固定，legacy 要求） */
typedef struct {
    struct vq_desc  desc[Q_SIZE];
    struct vq_avail avail;
    struct vq_used  used;
} __attribute__((packed)) vq_mem;

static u16 iobase = 0;
static u8 g_mac[6] = {0x52,0x54,0x00,0x12,0x34,0x58};
static char g_model[NIC_MODEL_MAX] = "virtio-net";

static vq_mem *g_q[2] = { 0, 0 };
static u8 *g_rxbuf[Q_SIZE] = { 0 };
static u8 *g_txbuf[Q_SIZE] = { 0 };
static struct vnet_hdr *g_rxhdr[Q_SIZE] = { 0 };
static struct vnet_hdr *g_txhdr[Q_SIZE] = { 0 };
static u16 rx_head[Q_SIZE], tx_head[Q_SIZE];
static u16 rx_last_used = 0;
static int tx_rr = 0;

static u8  rb8(u16 o) { return inb((u16)(iobase + o)); }
static void wb8(u16 o, u8 v) { outb((u16)(iobase + o), v); }
static void wl32(u16 o, u32 v) { outl((u16)(iobase + o), v); }
static void ww16(u16 o, u16 v) { outb((u16)(iobase+o), (u8)(v & 0xFF)); outb((u16)(iobase+o+1), (u8)(v >> 8)); }

static void queue_setup(u16 q, vq_mem *mem) {
    /* legacy：把队列共享内存的物理页帧号写进 QueuePFN */
    ww16(VIO_QUEUESEL, q);
    u32 phys = (u32)(u32)mem;
    wl32(VIO_QPFN, phys >> 12);
    /* 队列大小（Q_SIZE）与对齐，供设备校验 */
    ww16(VIO_QNUM, Q_SIZE);
    wl32(VIO_QALIGN, 4096);
}

static int virtio_net_init(void) {
    u8 bus, dev, fn;
    if (pci_find(VIRTIO_VEND, g_virtio_net_devids, &bus, &dev, &fn) != 0) return -1;

    /* virtio-net-pci（legacy transport）的寄存器在 **I/O 空间**（实测 BAR0 = I/O
     * at 0xc000），所以扫 6 个 BAR 找第一个 I/O BAR，不能假定 BAR0/内存 BAR。 */
    u32 bar = pci_find_io_bar(bus, dev, fn);
    if (!bar) return -1;
    iobase = (u16)(bar & 0xFFFF);
    pci_enable(bus, dev, fn);
    pci_cfg_write(bus, dev, fn, 0x04, pci_cfg_read(bus, dev, fn, 0x04) | 0x0001);

    /* 设备复位 -> ACK -> DRIVER -> FEATURES_OK -> DRIVER_OK */
    wb8(VIO_STATUS, 0);
    for (int i = 0; i < 100000; i++) io_wait();
    wb8(VIO_STATUS, VS_ACK);
    wb8(VIO_STATUS, VS_ACK | VS_DRIVER);

    /* 特性协商：清 device_feature 与 guest_feature，只用基础收发。
     * virtio-net 的基础通路（不含校验和卸载/GRO）不需要任何可选特性。 */
    wl32(0x24, 0);   /* device_feature = 0 */
    wl32(0x20, 0);   /* guest_feature  = 0 */
    wb8(VIO_STATUS, VS_ACK | VS_DRIVER | VS_FEATURES_OK);
    wb8(VIO_STATUS, VS_ACK | VS_DRIVER | VS_FEATURES_OK | VS_DRIVER_OK);

    /* 读 MAC：virtio 设备特定配置放在 PCI cfg 空间偏移 0x00.. */
    {
        u8 m[6] = {0};
        for (int i = 0; i < 6; i++)
            m[i] = (u8)pci_cfg_read(bus, dev, fn, (u8)i);
        u32 acc = (u32)m[0] | ((u32)m[1] << 8) | ((u32)m[2] << 16) | ((u32)m[3] << 24);
        if (acc != 0 && acc != 0xFFFFFFFFu) memcpy(g_mac, m, 6);
    }

    for (int q = 0; q < 2; q++) {
        g_q[q] = (vq_mem*)kzalloc(sizeof(vq_mem));
        if (!g_q[q]) return -1;
    }
    for (int i = 0; i < Q_SIZE; i++) {
        g_rxhdr[i] = (struct vnet_hdr*)kzalloc(VNET_HDR_LEN);
        g_rxbuf[i] = (u8*)kzalloc(2048);
        g_txhdr[i] = (struct vnet_hdr*)kzalloc(VNET_HDR_LEN);
        g_txbuf[i] = (u8*)kzalloc(2048);
        if (!g_rxbuf[i] || !g_txbuf[i] || !g_rxhdr[i] || !g_txhdr[i]) return -1;
    }

    /* RX 队列：每个槽 2 个描述符（hdr 由设备写、data 由设备写） */
    for (int i = 0; i < Q_SIZE; i++) {
        struct vq_desc *d = &g_q[Q_RX]->desc[i * 2];
        d[0].addr = (u32)(u32)g_rxhdr[i]; d[0].len = VNET_HDR_LEN;
        d[0].flags = VRING_DESC_F_NEXT;  d[0].next = (u16)(i * 2 + 1);
        d[1].addr = (u32)(u32)g_rxbuf[i]; d[1].len = 2048;
        d[1].flags = VRING_DESC_F_WRITE;  d[1].next = 0;
        g_q[Q_RX]->avail.ring[i] = (u16)(i * 2);
        rx_head[i] = (u16)(i * 2);
    }
    g_q[Q_RX]->avail.idx = Q_SIZE;   /* 预先挂满所有 RX 缓冲 */

    /* TX 队列：同样 2 描述符/槽（hdr + data），都只读 */
    for (int i = 0; i < Q_SIZE; i++) {
        struct vq_desc *d = &g_q[Q_TX]->desc[i * 2];
        d[0].addr = (u32)(u32)g_txhdr[i]; d[0].len = VNET_HDR_LEN;
        d[0].flags = VRING_DESC_F_NEXT;  d[0].next = (u16)(i * 2 + 1);
        d[1].addr = (u32)(u32)g_txbuf[i]; d[1].len = 0;  /* 发送时再填实际长度 */
        d[1].flags = 0;                   d[1].next = 0;
        tx_head[i] = (u16)(i * 2);
    }
    g_q[Q_TX]->avail.idx = 0;

    queue_setup(Q_RX, g_q[Q_RX]);
    queue_setup(Q_TX, g_q[Q_TX]);
    rx_last_used = 0;
    return 0;
}

static void virtio_net_get_mac(u8 mac[6]) { memcpy(mac, g_mac, 6); }

static void virtio_net_send(const u8 *frame, u32 len) {
    if (!iobase || !g_q[Q_TX] || len > 2048) return;
    int slot = tx_rr; tx_rr = (tx_rr + 1) % Q_SIZE;
    memset(g_txhdr[slot], 0, VNET_HDR_LEN);
    memcpy(g_txbuf[slot], frame, len);
    g_q[Q_TX]->desc[tx_head[slot] + 1].len = len;
    u16 aidx = g_q[Q_TX]->avail.idx;
    g_q[Q_TX]->avail.ring[aidx % Q_SIZE] = tx_head[slot];
    g_q[Q_TX]->avail.idx = (u16)(aidx + 1);
    ww16(VIO_QUEUESEL, Q_TX);
    ww16(VIO_NOTIFY_ADDR, Q_TX);
}

static int virtio_net_poll(void (*cb)(const u8 *frame, u32 len)) {
    if (!g_q[Q_RX]) return 0;
    vq_mem *q = g_q[Q_RX];
    int got = 0;
    /* 设备每收一帧就把 used.idx 推进；我们从上次位置追到现在 */
    while (rx_last_used != q->used.idx) {
        u16 pos = (u16)(rx_last_used % Q_SIZE);
        u32 id  = q->used.ring[pos].id;
        u32 ulen = q->used.ring[pos].len;
        int slot = (int)(id / 2);
        if (slot >= 0 && slot < Q_SIZE) {
            u32 datalen = (ulen > VNET_HDR_LEN) ? (ulen - VNET_HDR_LEN) : 0;
            if (cb && datalen) cb(g_rxbuf[slot], datalen);
            /* 把该槽重新挂回 avail 供设备复用 */
            u16 aidx = q->avail.idx;
            q->avail.ring[aidx % Q_SIZE] = rx_head[slot];
            q->avail.idx = (u16)(aidx + 1);
        }
        rx_last_used++;
        got++;
    }
    (void)rb8(VIO_ISR);   /* 读 ISR 清中断 */
    if (got) { ww16(VIO_QUEUESEL, Q_RX); ww16(VIO_NOTIFY_ADDR, Q_RX); }
    return got;
}

static const nic_driver_t g_virtio_drv = {
    "virtio-net", g_model, VIRTIO_VEND, g_virtio_net_devids,
    virtio_net_init, virtio_net_send, virtio_net_poll, virtio_net_get_mac
};

void virtio_net_register(void) { nic_register(&g_virtio_drv); }
