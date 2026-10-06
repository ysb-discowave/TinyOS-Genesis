#include "nic.h"
#include "io.h"
#include "libc.h"
#include "console.h"

/* ================= PCI 配置空间访问（0xCF8/0xCFC 传统 I/O 端口） ================= */
u32 pci_cfg_read(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
               ((u32)fn << 8) | (u32)(off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
void pci_cfg_write(u8 bus, u8 dev, u8 fn, u8 off, u32 val) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
               ((u32)fn << 8) | (u32)(off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

void pci_enable(u8 bus, u8 dev, u8 fn) {
    u16 cmd = (u16)(pci_cfg_read(bus, dev, fn, 0x04) & 0xFFFF);
    cmd |= 0x0001;   /* I/O Space Enable */
    cmd |= 0x0002;   /* Memory Space Enable */
    cmd |= 0x0004;   /* Bus Master Enable —— DMA 收发必需，漏了就是"装上了但发不出帧" */
    pci_cfg_write(bus, dev, fn, 0x04, cmd);
}

int pci_bar_is_mem(u8 bus, u8 dev, u8 fn, u8 bar) {
    return (pci_cfg_read(bus, dev, fn, 0x10 + bar * 4) & 1u) ? 0 : 1;
}

u32 pci_bar(u8 bus, u8 dev, u8 fn, u8 bar) {
    u32 v = pci_cfg_read(bus, dev, fn, 0x10 + bar * 4);
    if (v & 1u) return v & 0xFFFFFFFCu;          /* I/O 空间：低 2 位不是地址 */
    return v & 0xFFFFFFF0u;                     /* 内存空间：低 4 位不可解码 */
}

u32 pci_find_io_bar(u8 bus, u8 dev, u8 fn) {
    for (u32 i = 0; i < 6; i++) {
        u32 v = pci_cfg_read(bus, dev, fn, 0x10 + i * 4);
        /* bit0=1 表示 I/O 空间；地址非 0 才算实现 */
        if ((v & 1u) && (v & 0xFFFFFFFCu)) return v & 0xFFFFFFFCu;
    }
    return 0;
}

u32 pci_find_mem_bar(u8 bus, u8 dev, u8 fn) {
    for (u32 i = 0; i < 6; i++) {
        u32 v = pci_cfg_read(bus, dev, fn, 0x10 + i * 4);
        /* bit0=0 表示内存空间；prefetch bit3 也允许，地址非 0 才算实现 */
        if (!(v & 1u) && (v & 0xFFFFFFF0u)) return v & 0xFFFFFFF0u;
    }
    return 0;
}

int pci_find(u16 vendor, const u16 *devids, u8 *bus, u8 *dev, u8 *fn) {
    if (!devids) return -1;
    for (u32 b = 0; b < 8; b++) {
        for (u32 d = 0; d < 32; d++) {
            for (u32 f = 0; f < 8; f++) {
                u32 id = pci_cfg_read((u8)b, (u8)d, (u8)f, 0x00);
                u16 vid = (u16)(id & 0xFFFF);
                /* 跳过空插槽 / 读不到的槽位：厂商号为 0x0000 或 0xFFFF 都说明
                 * 这里没有有效设备。不加这句会误命中 dev0 主机桥（它的高端口
                 * 会别名出垃圾 vendor/device），表现为"探测到网卡但 BAR 不对"。 */
                if (vid == 0x0000 || vid == 0xFFFF) continue;
                if (vid != vendor) continue;
                u16 did = (u16)((id >> 16) & 0xFFFF);
                /* ⚠️ 必须再校验 class code：PCI 设备号存在真实冲突 ——
                 * 440FX 主机桥与 82557C 网卡**同为 8086:1237**。只比对
                 * vendor/device 会把 dev0 的主机桥当成网卡（它没有 I/O BAR，
                 * 于是表现为"探测到网卡却发不出帧"，极难查）。
                 * 网卡 = class 0x02(Network)；主机桥 = class 0x06(Bridge)。 */
                u32 classreg = pci_cfg_read((u8)b, (u8)d, (u8)f, 0x08);
                u8  class_   = (u8)((classreg >> 24) & 0xFF);
                if (class_ != 0x02) continue;   /* 只要 Network 类即可 */
                for (u32 i = 0; devids[i]; i++) {
                    if (devids[i] != did) continue;
                    if (bus) *bus = (u8)b;
                    if (dev) *dev = (u8)d;
                    if (fn)  *fn  = (u8)f;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ================= 驱动注册表 ================= */
static nic_driver_t g_nics[NIC_MAX];
static int g_reg_count = 0;
static int g_active = -1;

int nic_register(const nic_driver_t *drv) {
    if (!drv || !drv->name || !drv->init || !drv->send || !drv->poll || !drv->get_mac)
        return -1;
    if (g_reg_count >= NIC_MAX) return -1;
    g_nics[g_reg_count++] = *drv;
    return 0;
}

int nic_init(void) {
    g_active = -1;
    for (int i = 0; i < g_reg_count; i++) {
        if (g_nics[i].init() == 0) { g_active = i; return 0; }
    }
    return -1;
}

int  nic_count(void) { return g_reg_count; }
const char *nic_name(void) {
    return (g_active >= 0) ? g_nics[g_active].name : NULL;
}
const char *nic_model(void) {
    return (g_active >= 0) ? g_nics[g_active].model : NULL;
}
void nic_send(const u8 *frame, u32 len) {
    if (g_active >= 0) g_nics[g_active].send(frame, len);
}
int nic_poll(void (*cb)(const u8 *frame, u32 len)) {
    return (g_active >= 0) ? g_nics[g_active].poll(cb) : 0;
}
void nic_get_mac(u8 mac[6]) {
    if (g_active >= 0) g_nics[g_active].get_mac(mac);
}

void nic_list(void) {
    if (!g_reg_count) { kprintf("  (no NIC drivers compiled in)\n"); return; }
    for (int i = 0; i < g_reg_count; i++)
        kprintf("  %-10s  PCI %04X:%04X  %s\n", g_nics[i].name,
                g_nics[i].vendor, g_nics[i].devids ? g_nics[i].devids[0] : 0,
                (i == g_active) ? "[active]" : "");
}
