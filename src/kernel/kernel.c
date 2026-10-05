#include "vga.h"
#include "console.h"
#include "serial.h"
#include "keyboard.h"
#include "mouse.h"
#include "pit.h"
#include "idt.h"
#include "mm.h"
#include "vfs.h"
#include "disk/ata.h"
#include "disk/disk.h"
#include "fs/tinyfs.h"
#include "mbr.h"
#include "process.h"
#include "shell.h"
#include "install.h"
#include "user.h"
#include "libc.h"
#include "net/net.h"

extern u8 romfs_data[];
extern u32 romfs_len;

void panic(const char *file, int line, const char *msg) {
    vga_setcolor(0x0F, 0x04);
    kprintf("TinyOS PANIC: %s (%s:%d)\n", msg ? msg : "?", file, line);
    __asm__ volatile("cli; hlt");
    for (;;);
}

void kernel_main(u32 mb_info) {
    (void)mb_info;

    vga_init();
    serial_init();          /* 串口日志通道（同时也作为输入通道） */

    vga_setcolor(0x0A, 0x00);
    kprintf("TinyOS Genesis v0.1  (i386 protected mode, bare metal)\n");
    vga_setcolor(0x0F, 0x00);
    kprintf("[boot] entered 32-bit protected mode, kernel base 0x100000\n");

    mm_init();
    kprintf("[boot] kernel memory pool initialized\n");

    vfs_init();
    kprintf("[boot] virtual filesystem ready\n");

    /* ---------------- 磁盘 + 持久化文件系统 ---------------- */
    if (ata_init() == 0) {
        disk_t *dd = disk_default();
        kprintf("[boot] disk(s) found, default: %s (%s)\n",
                dd->name, dd->model[0] ? dd->model : "(unknown)");
        disk_list();

        /* 读写自检：用磁盘尾部的一个扇区（远离文件系统布局区） */
        {
            static u8 wb[512], rb[512];
            u32 tail = ata_sectors() > 8 ? ata_sectors() - 8 : 1;
            for (int i = 0; i < 512; i++) wb[i] = (u8)(i * 7 + 0x5A);
            int wr = ata_write(tail, 1, wb);
            int rd = ata_read(tail, 1, rb);
            int match = (wr == 0 && rd == 0 && memcmp(wb, rb, 512) == 0);
            kprintf("[boot] selftest@LBA %u: write=%d read=%d match=%d\n", tail, wr, rd, match);
            int r0 = ata_read(0, 1, rb);
            kprintf("[boot] selftest@LBA 0:   read=%d first4=%02X %02X %02X %02X\n",
                    r0, rb[0], rb[1], rb[2], rb[3]);
            int r1 = ata_read(100, 1, rb);
            kprintf("[boot] selftest@LBA 100: read=%d first4=%02X %02X %02X %02X\n",
                    r1, rb[0], rb[1], rb[2], rb[3]);
        }

        /* 文件系统在哪，由 MBR 分区表说了算。
         * 装系统时如果用户自定义过分区（比如把 TinyFS 挪到 LBA 4096），
         * 不查分区表就会在默认的 LBA 2048 上"没找到文件系统"，
         * 然后格式化出一个全新的空文件系统 —— 用户的数据看起来就丢了。 */
        u32 plba = 0, psec = 0;
        if (mbr_lookup(PART_TYPE_TINYOS_FS, &plba, &psec) && plba) {
            tfs_set_base(plba);
            kprintf("[boot] partition table: TinyFS @LBA %u (%u sectors, %u MB)\n",
                    plba, psec, psec / 2048);
        }

        /* 安装介质（CD/DVD）启动时绝不擅自格式化目标盘：
         * 那块盘可能是用户的数据盘，要等他在向导里确认。 */
        int fr = booted_from_install_media() ? tfs_init2(0) : tfs_init();
        if (fr == 0) {
            kprintf("[boot] TinyFS mounted: %u blocks / %u free / %u entries\n",
                    tfs_total_blocks(), tfs_free_blocks(), tfs_file_count());
        } else if (fr == 1) {
            kprintf("[boot] TinyFS formatted (new filesystem)\n");
        } else if (booted_from_install_media()) {
            kprintf("[boot] install media: target disk left untouched (not mounted)\n");
        } else {
            kprintf("[boot] TinyFS init failed: this run is not persistent\n");
        }
    } else {
        kprintf("[boot] no ATA disk: this run has no persistent storage\n");
    }

    /* romfs 作为只读基线；装载时先关掉回写，避免每次启动重复写盘 */
    vfs_persist_enable(0);
    vfs_load_romfs(romfs_data, romfs_len);
    vfs_persist_enable(1);
    kprintf("[boot] romfs mounted (%u bytes)\n", romfs_len);

    if (tfs_mounted()) {
        int n = vfs_load_disk();               /* 磁盘内容覆盖同名基线文件 */
        if (n > 0) kprintf("[boot] restored %d files from disk (persistence works)\n", n);
        if (tfs_file_count() == 0) {           /* 空文件系统：写入基线内容 */
            int seeded = vfs_install_disk();
            kprintf("[boot] first run: built-in files written to disk (%d entries)\n", seeded);
        }
    }

    /* 多用户：装载 /etc/passwd + /etc/shadow（缺失时写入出厂账户） */
    user_init();
    kprintf("[user] %d account(s) in the user database\n", user_count());

    idt_init();
    kprintf("[boot] IDT/PIC ready\n");
    kbd_init();   enable_irq(1);
    kprintf("[boot] PS/2 keyboard ready\n");

    /* 顺序很重要：enable_irq(12) 会同时放行主片 IRQ2 级联线。
     * 旧代码只解从片屏蔽位，IRQ12 因此永远到不了 CPU —— 这就是"鼠标完全不动"。 */
    int mok = mouse_init();
    enable_irq(12);
    {
        u8 pm, ps;
        pic_get_masks(&pm, &ps);
        kprintf("[boot] PS/2 mouse: %s (init step=%d) | IRQ12 %s | PIC master=0x%02X slave=0x%02X | cascade IRQ2 %s\n",
                mok == 0 ? "detected" : "NOT DETECTED", mouse_init_step(),
                (ps & (1u << 4)) ? "masked" : "unmasked",
                pm, ps,
                (pm & (1u << 2)) ? "MASKED (mouse IRQ cannot reach CPU)" : "open");
    }

    pit_init();   enable_irq(0);
    serial_enable_irq();   /* IRQ4：串口输入 */
    kprintf("[boot] PIT timer 100Hz ready\n");

    proc_init();

    /* 网络配置：装过系统的磁盘上会有 /etc/net.conf，先应用它再起网卡，
     * 这样"安装向导里手动填的 IP"开机就生效。 */
    netconf_apply();

    net_init();
    if (g_net_mac[0]) {
        char ips[20], gws[20];
        ip_to_str(g_ip, ips); ip_to_str(g_gw, gws);
        kprintf("[net ] e1000 NIC: MAC %02X:%02X:%02X:%02X:%02X:%02X  IP %s/24 GW %s\n",
                g_net_mac[0], g_net_mac[1], g_net_mac[2],
                g_net_mac[3], g_net_mac[4], g_net_mac[5], ips, gws);
    } else {
        kprintf("[net ] no e1000 NIC found (networking unavailable)\n");
    }

    /* 开机自动启动安装向导：
     *  - 安装介质（CD/DVD）：无条件进入安装界面；
     *  - 普通内核：有硬盘且盘上没有系统时进入。
     * 两种情况下向导里都可以输入 s 跳过，跳过之后正常进入登录。 */
    if (booted_from_install_media()) {
        kprintf("\n[boot] booted from install media: starting the setup wizard\n");
        installer_wizard("");
        kprintf("\nThe system on the install media is for installation only.\n"
        "Remove the media and boot from the hard disk when done.\n\n");
    } else if (wizard_should_autostart()) {
        installer_wizard("");
    }

    kprintf("\nTinyOS ready. Type 'help' for commands, 'desktop' for the GUI.\n\n");
    shell_main();

    for (;;) __asm__ volatile("hlt");
}
