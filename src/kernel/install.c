#include "instimg.h"
#include "install.h"
#include "disk/ata.h"
#include "fs/tinyfs.h"
#include "mbr.h"
#include "net/net.h"
#include "vfs.h"
#include "console.h"
#include "user.h"
#include "libc.h"

/* ============================================================
 * TinyOS 安装程序
 * ------------------------------------------------------------
 * 目标：把当前内核写到一块硬盘上，并让它脱离 Multiboot 装载器也能启动。
 *
 * 磁盘布局（安装后）：
 *   LBA 0               引导扇区（boot/boot.asm 512 字节，自带 A20/保护模式切换）
 *   LBA 1 .. 1+K-1      扁平内核镜像（boot 扇区分块读到 0x100000）
 *   LBA 2048 ..          TinyFS 文件系统（1 MiB 对齐，避开前 1 MiB）
 *
 * 用法：
 *   install                     交互式（会打印确认与口令提示）
 *   install yes                 跳过确认（仍会问 root 口令）
 *   install yes --rootpass P    跳过确认并指定 root 口令（脚本化安装）
 *   install yes --rootpass P --with-ssh     同时启用 SSH 服务
 *   install yes --rootpass P --no-ssh       明确不启用 SSH 服务
 *   install yes --rootpass P --with-jvm     同时安装 JVM 运行时 (/bin/JVM.TNCR)
 *   install yes --rootpass P --no-jvm       明确不安装 JVM 运行时
 * ============================================================ */

#define SSHD_CONF "/etc/sshd.conf"

static int write_blob(disk_t *dst, u32 lba, const u8 *data, u32 len) {
    static u8 sec[512];
    u32 off = 0, L = lba;
    while (off < len) {
        u32 n = len - off;
        if (n > 512) n = 512;
        memset(sec, 0, 512);
        memcpy(sec, data + off, n);
        if (disk_write(dst, L, 1, sec) != 0) return -1;
        off += n;
        L++;
    }
    return 0;
}

static int confirm(const char *prompt) {
    kprintf("%s", prompt);
    char a[16];
    int n = cons_readline(a, sizeof(a));
    if (n < 0) n = 0;
    if (n == 1 && (a[0] == 'y' || a[0] == 'Y')) return 1;
    if (n >= 3 && (a[0] == 'y' || a[0] == 'Y') && a[1] == 'e' && a[2] == 's') return 1;
    return 0;
}

/* 取出形如 --flag 的参数值；找不到返回 NULL */
static const char *flag_val(const char *arg, const char *flag) {
    size_t fl = strlen(flag);
    const char *p = arg;
    while (p && *p) {
        if (strncmp(p, flag, fl) == 0) return p + fl;
        p = strchr(p, ' ');
        if (p) while (*p == ' ') p++;
    }
    return NULL;
}
/* 是否存在 --flag（内核 libc 没有 strstr，这里手写一个子串查找） */
static int has_flag(const char *arg, const char *flag) {
    size_t fl = strlen(flag);
    if (!fl) return 1;
    for (const char *p = arg; *p; p++)
        if (*p == flag[0] && strncmp(p, flag, fl) == 0) return 1;
    return 0;
}

/* 安装时写入的 SSH 服务开关；shell 启动时读它决定是否自动拉起 sshd */
static void write_sshd_conf(int enabled) {
    char buf[192];
    snprintf(buf, sizeof(buf),
             "# TinyOS SSH server (written by 'install')\n"
             "# autostart=yes makes the shell launch /bin/SSHD.TNCR right after login.\n"
             "port 22\n"
             "autostart %s\n",
             enabled ? "yes" : "no");
    vfs_write_file(SSHD_CONF, (const u8*)buf, (u32)strlen(buf));
}

int sshd_autostart(void) {
    u32 sz = 0;
    const u8 *d = vfs_read_file(SSHD_CONF, &sz);
    if (!d || !sz) return 0;
    char buf[256];
    u32 n = sz < sizeof(buf) - 1 ? sz : (u32)sizeof(buf) - 1;
    memcpy(buf, d, n);
    buf[n] = 0;
    char *p = buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (!strncmp(p, "autostart", 9)) {
            char *v = strchr(p, ' ');
            while (v && *v == ' ') v++;
            if (v && (v[0] == 'y' || v[0] == 'Y' || v[0] == '1')) return 1;
        }
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

/* 安装后自检：引导扇区签名 + 分区表 + 文件系统条目数（在目标盘上） */
static void verify_install(disk_t *dst, u32 fs_lba) {
    u8 sec[512];
    kprintf("install: verifying...\n");

    if (disk_read(dst, INST_BOOT_LBA, 1, sec) != 0) { kprintf("  boot sector: READ FAILED\n"); return; }
    int sig = (sec[510] == 0x55 && sec[511] == 0xAA);
    kprintf("  boot sector @LBA %u: signature %02X%02X %s\n",
            INST_BOOT_LBA, sec[511], sec[510], sig ? "(bootable)" : "(NOT BOOTABLE!)");

    if (disk_read(dst, INST_KERNEL_LBA, 1, sec) != 0) { kprintf("  kernel: READ FAILED\n"); return; }
    kprintf("  kernel @LBA %u: first bytes %02X %02X %02X %02X\n",
            INST_KERNEL_LBA, sec[0], sec[1], sec[2], sec[3]);

    u32 plba = 0, psec = 0;
    /* 读目标盘自己的 MBR（旧代码读的是默认盘，多盘时可能根本不是刚写的那块） */
    if (disk_read(dst, 0, 1, sec) == 0 && mbr_find(sec, PART_TYPE_TINYOS_FS, &plba, &psec))
        kprintf("  partition 2: TinyFS @LBA %u, %u sectors (%u MB)\n", plba, psec, psec / 2048);
    else
        kprintf("  partition table: none (the kernel will fall back to LBA %u)\n", INST_FS_LBA);

    if (disk_read(dst, fs_lba, 1, sec) != 0) { kprintf("  filesystem: READ FAILED\n"); return; }
    char magic[9];
    memcpy(magic, sec, 8);
    magic[8] = 0;
    kprintf("  TinyFS @LBA %u: magic %s  %s\n", fs_lba, magic,
            memcmp(sec, "TINYFS01", 8) == 0 ? "(valid)" : "(INVALID!)");
}

/* ============================================================
 * 安装向导（5 步：磁盘 -> 网络 -> 组件 -> 账户 -> 确认并安装）
 * ------------------------------------------------------------
 * 每一步：直接回车 = 采用默认值；输入 s / skip = 跳过该步。
 * 第一步输入 s 则整个向导放弃，回到 Shell。
 * ============================================================ */

#define NETCONF "/etc/net.conf"

/* 组件清单：/bin 里哪些文件属于"示例程序" */
static const char *const DEMO_FILES[] = {
    "/bin/hello.TNCR",    "/bin/count.TNCR",     "/bin/gui_demo.TNCR",
    "/bin/net_demo.TNCR", "/bin/tiny_demo.TNCR", "/bin/basic_demo.TNCR",
    "/bin/minic_demo.TNCR", "/bin/cpp_demo.TNCR",
};
#define NDEMO ((int)(sizeof(DEMO_FILES) / sizeof(DEMO_FILES[0])))

typedef struct {
    int  disk_mode;      /* 0 全新安装 / 1 仅格式化 / 2 不写盘 */
    disk_t *dst;         /* 目标磁盘（多盘时可在向导里选） */
    u32  fs_lba;         /* TinyFS 分区起始 LBA（分区表第 2 项） */
    u32  fs_secs;        /* 分区大小（扇区），0 = 一直用到盘尾 */
    int  net_mode;       /* 0 自动 / 1 手动 */
    char ip[24], mask[24], gw[24], dns[24];
    int  comp_ssh, comp_tools, comp_demos, comp_docs, comp_desktop, comp_jvm;
    char rootpw[64];
    char newuser[USER_NAME_MAX];     /* 向导里新建的普通用户（空串 = 不建） */
    char newpass[64];
    int  ssh_bin_present;
    int  jvm_bin_present;
} wizard_t;

static int is_skip(const char *s) {
    if (!s || !s[0]) return 0;
    if (s[1] == 0) return (s[0] == 's' || s[0] == 'S');
    return (s[0] == 's' || s[0] == 'S') && s[1] == 'k' && s[2] == 'i' && s[3] == 'p';
}

/* 读一行；空串表示"用默认值" */
static void ask_str(const char *prompt, char *buf, int n) {
    kprintf("%s", prompt);
    int r = cons_readline(buf, n);
    if (r < 0) r = 0;
    buf[r] = 0;
}

/* 带默认值的提问：直接回车就用 def */
static void ask_default(const char *prompt, const char *def, char *buf, int n) {
    kprintf("%s [%s]: ", prompt, def);
    int r = cons_readline(buf, n);
    if (r < 0) r = 0;
    buf[r] = 0;
    if (!buf[0]) snprintf(buf, n, "%s", def);
}

/* 选择一项：返回 1..max，默认 def；返回 -1 表示跳过本步 */
static int ask_choice(const char *prompt, int def, int max) {
    char b[24];
    ask_str(prompt, b, sizeof(b));
    if (is_skip(b)) return -1;
    if (!b[0]) return def;
    if (b[0] >= '1' && b[0] < '1' + max) return b[0] - '0';
    return def;
}

/* 组件过滤：决定某个路径要不要写进磁盘 */
static int wiz_keep(const char *path, void *arg) {
    wizard_t *w = (wizard_t *)arg;
    if (!path || !path[0]) return 1;
    if (!strcmp(path, "/bin/SSHD.TNCR")) return w->comp_ssh;
    if (!strcmp(path, "/bin/JVM.TNCR")) return w->comp_jvm;
    if (!strcmp(path, "/bin/EDIT.TNCR") || !strcmp(path, "/bin/CC.TNCR"))
        return w->comp_tools;
    if (!strcmp(path, "/DESKTOP.TNCR")) return w->comp_desktop;
    for (int i = 0; i < NDEMO; i++)
        if (!strcmp(path, DEMO_FILES[i])) return w->comp_demos;
    if (!strcmp(path, "/home/readme.txt") || !strcmp(path, "/home/notes.txt") ||
        !strcmp(path, "/home/hello.mc"))
        return w->comp_docs;
    return 1;
}

static void write_netconf(const wizard_t *w) {
    char buf[320];
    const char *mode = w->net_mode ? "manual" : "auto";
    snprintf(buf, sizeof(buf),
             "# TinyOS network configuration (written by the setup wizard)\n"
             "mode %s\n"
             "ip %s\n"
             "mask %s\n"
             "gateway %s\n"
             "dns %s\n",
             mode, w->ip, w->mask, w->gw, w->dns);
    vfs_write_file(NETCONF, (const u8 *)buf, (u32)strlen(buf));
}

/* 读 /etc/net.conf 并应用到 g_ip / g_gw（开机或装完后调用） */
void netconf_apply(void) {
    u32 sz = 0;
    const u8 *d = vfs_read_file(NETCONF, &sz);
    if (!d || !sz) return;
    char buf[320];
    u32 n = sz < sizeof(buf) - 1 ? sz : (u32)sizeof(buf) - 1;
    memcpy(buf, d, n);
    buf[n] = 0;
    char *p = buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *v = strchr(p, ' ');
        if (v) { while (*v == ' ') v++; }
        if (v && *v) {
            if (!strncmp(p, "ip ", 3))       g_ip = ip_parse(v);
            else if (!strncmp(p, "gateway ", 8)) g_gw = ip_parse(v);
        }
        p = nl ? nl + 1 : NULL;
    }
}

void netconf_print(void) {
    u32 sz = 0;
    const u8 *d = vfs_read_file(NETCONF, &sz);
    if (!d || !sz) { kprintf("network: using built-in defaults (no " NETCONF ")\n"); return; }
    char buf[320];
    u32 n = sz < sizeof(buf) - 1 ? sz : (u32)sizeof(buf) - 1;
    memcpy(buf, d, n);
    buf[n] = 0;
    kprintf("network configuration (" NETCONF "):\n");
    char *p = buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (*p && *p != '#') kprintf("  %s\n", p);
        p = nl ? nl + 1 : NULL;
    }
}

int installed_marker_present(void) { return vfs_resolve("/etc/installed") != 0; }

/* 本内核是不是"安装介质"（CD/DVD）？
 * 由构建脚本在编译安装介质时定义 TINYOS_INSTALL_MEDIA。
 * 安装介质开机无条件进入安装向导，而且启动阶段不许动目标盘。 */
int booted_from_install_media(void) {
#ifdef TINYOS_INSTALL_MEDIA
    return 1;
#else
    return 0;
#endif
}

int wizard_should_autostart(void) {
    /* 只有在"有硬盘但硬盘上还没有系统"时才自动启动向导 */
    if (!ata_present()) return 0;
    return !installed_marker_present();
}

/* ---- 各步骤 ---- */

/* 返回 0 正常，-1 用户跳过整个向导 */
/* ---- 分区 ----
 * 默认布局（引导扇区 + 内核 + 之后全是文件系统）已经够用，所以只提供两个
 * 真正会被用到的自由度：文件系统分区从哪个 LBA 开始、占多大。
 * 结果写进扇区 0 的标准 MBR 分区表，内核开机时靠它定位 TinyFS ——
 * 少了这一步，自定义分区的盘重启后会在默认 LBA 2048 上"认不出文件系统"，
 * 然后重新格式化出一个空的，看起来就像数据丢了。 */
/* 让向导列出全部磁盘并让用户挑一块作为安装目标；
 * 默认（回车）= 当前默认盘。返回选中的 disk_t*（保证非 NULL 的注册盘）。 */
static disk_t *wiz_pick_disk(wizard_t *w) {
    int n = disk_count();
    disk_t *def = disk_default();
    if (n == 1) {
        w->dst = def;
        return def;
    }
    kprintf("\n  Target disk (this disk's contents will be ERASED):");
    int defidx = 0;
    for (int i = 0; i < n; i++) {
        disk_t *d = disk_get(i);
        if (!d) continue;
        const char *mark = (d == def) ? "  [default]" : "";
        kprintf("\n    %d) %s  %s  %s  %u KB%s",
                i + 1, d->name, d->model[0] ? d->model : "(unknown)",
                d->loc[0] ? d->loc : "-", d->sectors / 2, mark);
        if (d == def) defidx = i + 1;
    }
    kprintf("\n");
    char b[8];
    ask_str("  Pick a disk [1]: ", b, sizeof(b));
    int pick = 1;
    if (b[0] >= '1' && b[0] <= '9') pick = b[0] - '0';
    if (pick < 1 || pick > n) pick = defidx;
    w->dst = disk_get(pick - 1);
    return w->dst;
}

static void wiz_layout_default(wizard_t *w) {
    u32 secs = disk_sectors(w->dst);
    w->fs_lba  = INST_FS_LBA;
    w->fs_secs = (secs > INST_FS_LBA) ? (secs - INST_FS_LBA) : 0;
}

static void wiz_print_layout(const wizard_t *w) {
    u32 secs = disk_sectors(w->dst);
    if (!w->fs_secs) {
        kprintf("  filesystem: LBA %u .. end of disk (%u MB)\n",
                w->fs_lba, (secs - w->fs_lba) / 2048);
        return;
    }
    u32 end = w->fs_lba + w->fs_secs - 1;
    kprintf("  filesystem: LBA %u..%u (%u MB)", w->fs_lba, end, w->fs_secs / 2048);
    if (secs > end + 1)
        kprintf(", free space after it: %u sectors (%u MB)",
                secs - end - 1, (secs - end - 1) / 2048);
    kprintf("\n");
}

/* 引导扇区 = 内嵌的 boot_img + 本次分区选择填好的 MBR 分区表 */
static void wiz_make_boot_sector(u8 *out, const wizard_t *w, u32 ksec) {
    memcpy(out, boot_img, 512);
    mbr_clear(out);
    mbr_set(out, 0, PART_TYPE_TINYOS_SYS, INST_KERNEL_LBA, ksec, 1);   /* 活动分区 */
    mbr_set(out, 1, PART_TYPE_TINYOS_FS,  w->fs_lba, w->fs_secs, 0);
    out[510] = 0x55;
    out[511] = 0xAA;
}

/* 返回 0 正常，-1 出错 */
static int wiz_ask_partitions(wizard_t *w, u32 ksec) {
    u32 secs = disk_sectors(w->dst);
    u32 kernel_end = INST_KERNEL_LBA + ksec;      /* 内核之后的第一个空闲 LBA */
    char def[24], in[24];

    if (w->disk_mode == 1) {
        /* 只格式化：引导扇区不重写，分区表也就改不了，
         * 所以直接用现有分区表里的位置（没有就用默认）。 */
        u32 lba = 0, cnt = 0;
        if (mbr_lookup2(w->dst, PART_TYPE_TINYOS_FS, &lba, &cnt) && lba) {
            w->fs_lba = lba; w->fs_secs = cnt;
            kprintf("\n  Partitioning: using the existing TinyFS partition\n");
        } else {
            wiz_layout_default(w);
            kprintf("\n  Partitioning: no partition table yet, using the default layout\n");
        }
        wiz_print_layout(w);
        return 0;
    }

    wiz_layout_default(w);
    kprintf("\n  Partitioning\n");
    kprintf("  1) Whole disk: the filesystem takes everything after the kernel  [default]\n");
    kprintf("  2) Custom: choose the filesystem start LBA and its size\n");
    int c = ask_choice("  Choice [1] (s = keep the default): ", 1, 2);
    if (c < 0 || c == 1) {
        wiz_print_layout(w);
        return 0;
    }

    /* 起始 LBA 不能压到内核头上 */
    snprintf(def, sizeof(def), "%u", INST_FS_LBA);
    for (;;) {
        ask_default("  Filesystem start LBA", def, in, sizeof(in));
        u32 v = (u32)atoi(in);
        if (v >= kernel_end && v < secs) { w->fs_lba = v; break; }
        kprintf("  please use %u..%u (the kernel occupies LBA %u..%u)\n",
                kernel_end, secs - 1, INST_KERNEL_LBA, kernel_end - 1);
    }

    /* 大小按 MB 问（1 MB = 2048 扇区）；0 或回车 = 用到底 */
    u32 avail = secs - w->fs_lba;
    snprintf(def, sizeof(def), "%u", avail / 2048);
    for (;;) {
        ask_default("  Filesystem size in MB (0 = rest of the disk)", def, in, sizeof(in));
        u32 mb = (u32)atoi(in);
        if (mb == 0) { w->fs_secs = avail; break; }
        if (mb <= avail / 2048u) { w->fs_secs = mb * 2048u; break; }
        kprintf("  please use 1..%u MB (only %u MB available from LBA %u)\n",
                avail / 2048, avail / 2048, w->fs_lba);
    }

    if (w->fs_secs < 128) {
        kprintf("  !! the filesystem needs at least 128 sectors\n");
        return -1;
    }
    wiz_print_layout(w);
    return 0;
}

static int wiz_step_disk(wizard_t *w, u32 *ksec_out) {
    /* 多盘：先让用户挑安装目标（单盘时直接就是那块盘） */
    disk_t *dst = wiz_pick_disk(w);
    u32 secs = disk_sectors(dst);
    u32 ksec = (kernel_img_len + 511) / 512;
    if (ksec_out) *ksec_out = ksec;

    kprintf("\n[1/5] Disk\n");
    kprintf("  Disk   : %s %s\n", dst->name, dst->model[0] ? dst->model : "(unknown)");
    kprintf("           %u sectors (%u KB)%s\n", secs, secs / 2,
            dst->is_boot ? "  [BIOS boot disk]" : "");
    kprintf("  Layout : LBA %u boot sector | LBA %u..%u kernel (%u sectors)\n",
            INST_BOOT_LBA, INST_KERNEL_LBA, INST_KERNEL_LBA + ksec - 1, ksec);
    kprintf("           LBA %u.. TinyFS filesystem\n", INST_FS_LBA);
    if (secs < INST_FS_LBA + 1024) {
        kprintf("  !! disk too small (at least %u sectors needed), cannot install\n", INST_FS_LBA + 1024);
        return -2;
    }
    kprintf("  1) Fresh install: write boot sector + kernel + format filesystem  [default]\n");
    kprintf("  2) Format the filesystem only (keep the current boot block/kernel)\n");
    kprintf("  3) Skip: do not write anything to the disk\n");
    int c = ask_choice("  Choice [1] (s = skip the whole wizard): ", 1, 3);
    if (c < 0) return -1;
    w->disk_mode = c - 1;              /* 1->0 全新, 2->1 仅格式化, 3->2 不写盘 */
    if (w->disk_mode == 2) {           /* 不写盘：布局无所谓，填个默认免得后面乱 */
        wiz_layout_default(w);
        return 0;
    }
    return wiz_ask_partitions(w, ksec);
}

static int wiz_step_network(wizard_t *w) {
    char cur[20];
    ip_to_str(g_ip, cur);
    kprintf("\n[2/5] Network\n");
    kprintf("  NIC    : ");
    if (g_net_mac[0] || g_net_mac[1] || g_net_mac[2]) {
        kprintf("e1000 %02X:%02X:%02X:%02X:%02X:%02X\n",
                g_net_mac[0], g_net_mac[1], g_net_mac[2],
                g_net_mac[3], g_net_mac[4], g_net_mac[5]);
    } else {
        kprintf("no NIC detected (skipping network setup)\n");
        w->net_mode = 0;
        return 0;
    }
    kprintf("  1) Automatic: use the current IP %s, gateway ", cur);
    { char g[20]; ip_to_str(g_gw, g); kprintf("%s   [default]\n", g); }
    kprintf("  2) Manual: enter IP / netmask / gateway / DNS yourself\n");
    int c = ask_choice("  Choice [1] (s = skip): ", 1, 2);
    if (c < 0) {                       /* 跳过：保留当前值 */
        w->net_mode = 0;
        ip_to_str(g_ip, w->ip);
        ip_to_str(g_gw, w->gw);
        snprintf(w->mask, sizeof(w->mask), "255.255.255.0");
        snprintf(w->dns,  sizeof(w->dns),  "10.0.2.3");
        return 0;
    }
    w->net_mode = (c == 2) ? 1 : 0;
    if (!w->net_mode) {
        ip_to_str(g_ip, w->ip);
        ip_to_str(g_gw, w->gw);
        snprintf(w->mask, sizeof(w->mask), "255.255.255.0");
        snprintf(w->dns,  sizeof(w->dns),  "10.0.2.3");
        return 0;
    }
    char defip[20], defgw[20];
    ip_to_str(g_ip, defip);
    ip_to_str(g_gw, defgw);
    for (;;) {
        ask_default("  IP address", defip, w->ip, sizeof(w->ip));
        if (is_skip(w->ip)) { snprintf(w->ip, sizeof(w->ip), "%s", defip); break; }
        if (ip_parse(w->ip) != 0) break;
        kprintf("  invalid address, please retype it (e.g. 10.0.2.15)\n");
    }
    ask_default("  Netmask", "255.255.255.0", w->mask, sizeof(w->mask));
    ask_default("  Gateway", defgw, w->gw, sizeof(w->gw));
    ask_default("  DNS", "10.0.2.3", w->dns, sizeof(w->dns));
    return 0;
}

static void wiz_show_components(const wizard_t *w) {
    kprintf("  [x] 1) Core system: kernel + shell + account database (required)\n");
    kprintf("  [%s] 2) SSH server (SSHD.TNCR)%s\n",
            w->comp_ssh ? "x" : " ",
            w->ssh_bin_present ? "" : "   (not present in this image)");
    kprintf("  [%s] 3) Editor and compiler (EDIT.TNCR / CC.TNCR)\n", w->comp_tools ? "x" : " ");
    kprintf("  [%s] 4) Demo programs (hello/count/gui_demo/... %d files)\n",
            w->comp_demos ? "x" : " ", NDEMO);
    kprintf("  [%s] 5) Docs and sample sources (readme/notes/hello.mc)\n", w->comp_docs ? "x" : " ");
    kprintf("  [%s] 6) Graphical desktop (DESKTOP.TNCR)\n", w->comp_desktop ? "x" : " ");
    kprintf("  [%s] 7) JVM runtime (JVM.TNCR - runs Hello.class in TinyOS)%s\n",
            w->comp_jvm ? "x" : " ",
            w->jvm_bin_present ? "" : "   (not present in this image)");
}

static void wiz_toggle(wizard_t *w, int n) {
    switch (n) {
    case 2: if (w->ssh_bin_present)  w->comp_ssh     = !w->comp_ssh;     break;
    case 3: w->comp_tools   = !w->comp_tools;   break;
    case 4: w->comp_demos   = !w->comp_demos;   break;
    case 5: w->comp_docs    = !w->comp_docs;    break;
    case 6: w->comp_desktop = !w->comp_desktop; break;
    case 7: if (w->jvm_bin_present) w->comp_jvm = !w->comp_jvm; break;
    default: break;
    }
}

static int wiz_step_components(wizard_t *w) {
    kprintf("\n[3/5] Software to install\n");
    for (;;) {
        wiz_show_components(w);
        char b[48];
        ask_str("  Type item numbers to toggle (e.g. 2 4), Enter to finish (s = skip): ", b, sizeof(b));
        if (is_skip(b)) return 0;
        if (!b[0]) break;
        int any = 0;
        for (const char *q = b; *q; q++) {
            if (*q >= '2' && *q <= '7') { wiz_toggle(w, *q - '0'); any = 1; }
        }
        if (!any) break;
        kprintf("\n");
    }
    return 0;
}

static int wiz_step_accounts(wizard_t *w) {
    kprintf("\n[4/5] Accounts\n");
    for (;;) {
        kprintf("  Password for root (min 3 characters, s = keep the default): ");
        int n = cons_readline_masked(w->rootpw, sizeof(w->rootpw));
        w->rootpw[n < 0 ? 0 : n] = 0;
        kprintf("\n");
        if (is_skip(w->rootpw)) { w->rootpw[0] = 0; return 0; }
        if (strlen(w->rootpw) >= 3) break;
        kprintf("  password too short, please retype\n");
    }
    char again[64];
    kprintf("  Retype password: ");
    int m = cons_readline_masked(again, sizeof(again));
    again[m < 0 ? 0 : m] = 0;
    kprintf("\n");
    if (strcmp(w->rootpw, again) != 0) {
        kprintf("  the two do not match, skipping this step (default password kept)\n");
        w->rootpw[0] = 0;
    }

    /* 新建普通用户：账号写在 /etc/passwd + /etc/shadow，会随其它文件一起
     * 落盘，所以装完后从硬盘启动时它还在。 */
    char b[USER_NAME_MAX];
    ask_str("  Create a normal user? Type a user name (Enter = none, s = skip): ", b, sizeof(b));
    if (is_skip(b) || !b[0]) { w->newuser[0] = 0; return 0; }
    if (user_by_name(b)) {
        kprintf("  user %s already exists, not creating it again\n", b);
        w->newuser[0] = 0;
        return 0;
    }
    snprintf(w->newuser, sizeof(w->newuser), "%s", b);
    for (;;) {
        kprintf("  Password for %s (min 3 characters, s = cancel): ", w->newuser);
        int n = cons_readline_masked(w->newpass, sizeof(w->newpass));
        w->newpass[n < 0 ? 0 : n] = 0;
        kprintf("\n");
        if (is_skip(w->newpass)) { w->newuser[0] = 0; return 0; }
        if (strlen(w->newpass) >= 3) break;
        kprintf("  password too short, please retype\n");
    }
    return 0;
}

static int wiz_confirm_and_install(wizard_t *w, u32 ksec) {
    kprintf("\n[5/5] Confirm and install\n");
    kprintf("  Disk     : %s\n",
            w->disk_mode == 0 ? "fresh install (boot sector + kernel + filesystem)" :
            w->disk_mode == 1 ? "format the filesystem only" : "no disk write (in-memory system only)");
    kprintf("  Network  : %s  IP %s  netmask %s  gateway %s  DNS %s\n",
            w->net_mode ? "manual" : "automatic", w->ip, w->mask, w->gw, w->dns);
    kprintf("  Software : core system%s%s%s%s%s%s\n",
            w->comp_ssh ? " + SSH" : "",
            w->comp_tools ? " + editor/compiler" : "",
            w->comp_demos ? " + demos" : "",
            w->comp_docs ? " + docs" : "",
            w->comp_desktop ? " + desktop" : "",
            w->comp_jvm ? " + JVM" : "");
    kprintf("  Layout   : LBA 0 boot | LBA %u..%u kernel (%u sectors)\n",
            INST_KERNEL_LBA, INST_KERNEL_LBA + ksec - 1, ksec);
    wiz_print_layout(w);
    kprintf("  root     : %s\n", w->rootpw[0] ? "a new password will be set" : "keep the default password");

    char b[24];
    ask_str("  Type yes to start the installation (anything else = cancel): ", b, sizeof(b));
    if (!(b[0] == 'y' || b[0] == 'Y')) {
        kprintf("  cancelled, nothing was changed\n");
        return 1;
    }

    /* ---- 网络配置先落地（不管有没有盘都写进内存文件系统）---- */
    write_netconf(w);
    g_ip = ip_parse(w->ip);
    g_gw = ip_parse(w->gw);

    /* ---- 磁盘：全部操作落在 w->dst（用户选定的目标盘） ----
     * 格式化期间把"默认盘"指到目标盘，tfs_format 走默认盘，
     * 完事立刻指回，不影响正在运行的这块盘上的系统。 */
    int have_fs = 0;
    disk_t *orig_default = disk_default();
    if (w->disk_mode == 0) {
        /* 引导扇区要带上本次的分区表：内核开机靠它找 TinyFS */
        u8 bs[512];
        wiz_make_boot_sector(bs, w, ksec);
        kprintf("\n  writing boot sector @LBA %u (with the partition table) ...\n", INST_BOOT_LBA);
        if (write_blob(w->dst, INST_BOOT_LBA, bs, 512) != 0) {
            kprintf("  FAILED: could not write the boot sector\n"); return -1;
        }
        kprintf("  writing kernel image (%u bytes) @LBA %u ...\n", kernel_img_len, INST_KERNEL_LBA);
        if (write_blob(w->dst, INST_KERNEL_LBA, kernel_img, kernel_img_len) != 0) {
            kprintf("  FAILED: could not write the kernel image\n"); return -1;
        }
    }
    if (w->disk_mode == 0 || w->disk_mode == 1) {
        kprintf("  formatting TinyFS @LBA %u on %s ...\n", w->fs_lba, w->dst->name);
        disk_set_default(w->dst);
        tfs_set_base(w->fs_lba);
        tfs_set_limit(w->fs_secs);      /* 自定义分区大小：不许越过分区边界 */
        int fr = tfs_format();
        disk_set_default(orig_default);
        if (fr != 0) { kprintf("  FAILED: could not format the filesystem\n"); return -1; }
        have_fs = 1;
    }

    /* ---- 按组件写文件 ---- */
    if (have_fs) {
        kprintf("  writing files ...\n");
        int nfiles = vfs_install_disk_filter(wiz_keep, w);
        kprintf("    %d entries written to disk\n", nfiles);
    } else {
        kprintf("  (disk write skipped, no file was written)\n");
    }

    /* ---- 账户 ---- */
    if (w->rootpw[0]) {
        if (user_change_pass("root", w->rootpw) != 0)
            kprintf("  warning: could not set the root password\n");
        else
            kprintf("  root password set\n");
    }
    if (!user_by_name("guest")) {
        vfs_mkdir("/home/guest");
        if (user_add("guest", "guest", 1000, "/home/guest") == 0)
            kprintf("  created the normal user guest (password guest, change it with 'passwd')\n");
    }
    if (w->newuser[0]) {
        char home[128];
        snprintf(home, sizeof(home), "/home/%s", w->newuser);
        vfs_mkdir(home);
        int uid = 1001;
        while (user_by_uid(uid) && uid < 60000) uid++;
        if (user_add(w->newuser, w->newpass, uid, home) == 0)
            kprintf("  created user %s (uid %d, home %s) - it is written to /etc/passwd on disk,\n"
                    "    so it can log in again after rebooting from the hard disk.\n", w->newuser, uid, home);
        else
            kprintf("  warning: could not create user %s\n", w->newuser);
    }
    vfs_mkdir("/root"); vfs_mkdir("/home/guest"); vfs_mkdir("/etc");

    /* ---- 系统文件 ---- */
    write_sshd_conf(w->comp_ssh && w->ssh_bin_present);
    if (have_fs)
        vfs_write_file("/etc/installed",
                       (const u8 *)"TinyOS Genesis v0.1 installed by the setup wizard\n", 50);

    if (have_fs) verify_install(w->dst, w->fs_lba);

    /* 目标盘 ≠ 正在运行的盘时：格式化已经把 TFS 内存态切到了目标盘，
     * 正在运行的原盘被完整保留、一个字节都没动（它的元数据不再被内存
     * 缓存引用，重启后照常挂载）。这是多盘场景最安全的默认行为。 */
    if (have_fs && w->dst != orig_default && orig_default)
        kprintf("  note: the disk the system is running from was left untouched.\n");

    kprintf("\nInstallation complete. %u KB filesystem, %d entries.\n",
            tfs_capacity_kb(), tfs_file_count());
    kprintf("  layout  : LBA 0 boot | LBA %u..%u kernel | LBA %u.. TinyFS\n",
            INST_KERNEL_LBA, INST_KERNEL_LBA + ksec - 1, w->fs_lba);
    kprintf("  network: %s / %s  gateway %s\n", w->ip, w->mask, w->gw);
    kprintf("  SSH   : %s\n",
            (w->comp_ssh && w->ssh_bin_present) ? "installed and enabled at boot" : "not installed");
    kprintf("  JVM   : %s\n",
            (w->comp_jvm && w->jvm_bin_present) ? "installed (run /bin/JVM.TNCR)" : "not installed");
    kprintf("  You can now reboot and boot from the hard disk, or run 'setup' again.\n");
    (void)ksec;
    return 0;
}

int installer_wizard(const char *arg) {
    (void)arg;
    wizard_t w;
    memset(&w, 0, sizeof(w));

    if (boot_img_len < 512 || kernel_img_len < 1024) {
        kprintf("setup: this kernel carries no embedded install image, cannot install to disk.\n");
        kprintf("       rebuild it with:  build.ps1 -Img\n");
        return -1;
    }
    w.ssh_bin_present = vfs_resolve("/bin/SSHD.TNCR") != 0;
    w.jvm_bin_present = vfs_resolve("/bin/JVM.TNCR") != 0;
    w.comp_tools = 1; w.comp_demos = 1; w.comp_docs = 1; w.comp_desktop = 1;
    /* SSH 服务端默认不装：它会在登录后接管控制台（要按 ESC 才交还），
     * 第一次装系统的人多半不想要。需要的话在组件那一步按 2 打开。 */
    w.comp_ssh = 0;
    /* JVM 运行时（JVM.TNCR）默认不装：它较大且只有装 romfs 时构建了
     * TinyOS-JVM 才有。需要 Java 的在组件那一步按 7 打开。 */
    w.comp_jvm = 0;

    kprintf("\n================ TinyOS Setup Wizard ================\n");
    kprintf("5 steps: disk / network / software / accounts / confirm and install.\n");
    kprintf("Press Enter to accept the default; type s to skip a step.\n");
    if (installed_marker_present())
        kprintf("NOTE: a system is already installed on the disk; continuing reformats it.\n");
    kprintf("=================================================\n");

    if (!ata_present()) {
        kprintf("\nNo hard disk detected. You can still configure the network and software,\n");
        kprintf("but nothing will be written to disk (the disk step is skipped).\n");
    }

    u32 ksec = 0;
    if (ata_present()) {
        int r = wiz_step_disk(&w, &ksec);
        if (r == -1) { kprintf("\nSetup wizard skipped.\n"); return 1; }
        if (r == -2) return -1;
    } else {
        w.disk_mode = 2;
        w.fs_lba = INST_FS_LBA;
    }

    wiz_step_network(&w);
    wiz_step_components(&w);
    wiz_step_accounts(&w);
    return wiz_confirm_and_install(&w, ksec);
}

int installer_run(const char *arg) {
    if (!arg) arg = "";

    /* ---- 载荷完整性 ---- */
    if (boot_img_len < 512 || kernel_img_len < 1024) {
        kprintf("install: this kernel carries no embedded install image.\n");
        kprintf("         build it with:  build.ps1 -Img\n");
        kprintf("         (the payload is generated into kernel/instimg.c)\n");
        return -1;
    }

    if (!ata_present()) {
        kprintf("install: no ATA disk detected. Attach a hard disk and retry.\n");
        kprintf("         QEMU example: -drive file=tinyos-disk.img,format=raw,if=ide,index=0\n");
        return -1;
    }

    /* 多盘：--disk <name> 选目标盘，缺省 = 默认盘 */
    disk_t *dst = disk_default();
    const char *dv = flag_val(arg, "--disk ");
    if (dv) {
        while (*dv == ' ') dv++;
        char dn[DISK_NAME_MAX]; int i = 0;
        while (*dv && *dv != ' ' && i < (int)sizeof(dn) - 1) { dn[i++] = *dv++; dn[i] = 0; }
        int idx = disk_find_name(dn);
        if (idx < 0) {
            kprintf("install: no such disk '%s'. available:\n", dn);
            disk_list();
            return -1;
        }
        dst = disk_get(idx);
        kprintf("install: target disk selected: %s\n", dst->name);
    }

    u32 secs = disk_sectors(dst);
    u32 ksec = (kernel_img_len + 511) / 512;

    kprintf("\nTinyOS installer\n");
    kprintf("================\n");
    kprintf("Target disk : %s (%s)\n", dst->name, dst->model[0] ? dst->model : "(unknown)");
    kprintf("              %u sectors (%u KB)%s\n", secs, secs / 2,
            dst->is_boot ? "  [BIOS boot disk]" : "");
    kprintf("Layout      : LBA %u boot sector | LBA %u..%u kernel (%u sectors)\n",
            INST_BOOT_LBA, INST_KERNEL_LBA, INST_KERNEL_LBA + ksec - 1, ksec);
    kprintf("              LBA %u.. TinyFS\n", INST_FS_LBA);
    kprintf("Image       : boot %u bytes, kernel %u bytes (load 0x%X)\n",
            boot_img_len, kernel_img_len, kernel_img_load);

    if (secs < INST_FS_LBA + 1024) {
        kprintf("install: disk too small (need at least %u sectors).\n", INST_FS_LBA + 1024);
        return -1;
    }

    kprintf("\n!! EVERYTHING ON THIS DISK WILL BE ERASED !!\n");
    if (!has_flag(arg, "yes") && !has_flag(arg, "--yes")) {
        if (!confirm("Type 'yes' to install: ")) {
            kprintf("install: cancelled\n");
            return -1;
        }
    } else {
        kprintf("(confirmation skipped: 'yes' given on the command line)\n");
    }

    /* ---- root 口令 ---- */
    char rootpw[64];
    const char *pv = flag_val(arg, "--rootpass ");
    if (pv) {
        int i = 0;
        while (pv[i] && pv[i] != ' ' && i < (int)sizeof(rootpw) - 1) { rootpw[i] = pv[i]; i++; }
        rootpw[i] = 0;
        kprintf("root password: taken from --rootpass\n");
    } else {
        kprintf("New password for root: ");
        int n = cons_readline_masked(rootpw, sizeof(rootpw));
        rootpw[n < 0 ? 0 : n] = 0;
        kprintf("\n");
        if (strlen(rootpw) < 3) {
            kprintf("install: password too short (minimum 3 characters)\n");
            return -1;
        }
        kprintf("Retype password: ");
        char again[64];
        int m = cons_readline_masked(again, sizeof(again));
        again[m < 0 ? 0 : m] = 0;
        kprintf("\n");
        if (strcmp(rootpw, again) != 0) {
            kprintf("install: passwords do not match\n");
            return -1;
        }
    }

    /* ---- 可选组件：OpenSSH 服务端 ---- */
    int with_ssh;
    int ssh_bin_present = vfs_resolve("/bin/SSHD.TNCR") != 0;
    if (has_flag(arg, "--with-ssh"))      with_ssh = 1;
    else if (has_flag(arg, "--no-ssh"))   with_ssh = 0;
    else if (!ssh_bin_present)            with_ssh = 0;
    else                                  with_ssh = confirm("Install the SSH server (SSHD.TNCR) and enable autostart? [y/N]: ");

    if (with_ssh && !ssh_bin_present) {
        kprintf("install: SSHD.TNCR is missing from this kernel image; skipping the SSH server\n");
        kprintf("         rebuild with -Users so tools/build_users.py produces it\n");
        with_ssh = 0;
    }

    /* ---- 可选组件：JVM 运行时（romfs 里构建了 TinyOS-JVM 时才有）---- */
    int with_jvm;
    int jvm_bin_present = vfs_resolve("/bin/JVM.TNCR") != 0;
    if (has_flag(arg, "--with-jvm"))      with_jvm = 1;
    else if (has_flag(arg, "--no-jvm"))   with_jvm = 0;
    else if (!jvm_bin_present)           with_jvm = 0;
    else                                 with_jvm = confirm("Install the JVM runtime (JVM.TNCR)? [y/N]: ");
    if (with_jvm && !jvm_bin_present) {
        kprintf("install: JVM.TNCR is missing from this kernel image; skipping the JVM runtime\n");
        kprintf("         rebuild with -Users so tools/build_users.py builds TinyOS-JVM into romfs\n");
        with_jvm = 0;
    }

    /* ---- 分区：命令行安装器一律用默认布局，但也要把分区表写进去 ---- */
    u32 fs_lba  = INST_FS_LBA;
    u32 fs_secs = (secs > fs_lba) ? (secs - fs_lba) : 0;

    /* ---- 开始写盘 ---- */
    u8 bs[512];
    memcpy(bs, boot_img, 512);
    mbr_clear(bs);
    mbr_set(bs, 0, PART_TYPE_TINYOS_SYS, INST_KERNEL_LBA, ksec, 1);
    mbr_set(bs, 1, PART_TYPE_TINYOS_FS,  fs_lba, fs_secs, 0);
    bs[510] = 0x55; bs[511] = 0xAA;

    /* 格式化前把默认盘切到目标盘（tfs_format 走默认盘），写完后切回 */
    disk_t *orig_default = disk_default();

    kprintf("\ninstall: writing boot sector @LBA %u (with the partition table) ...\n", INST_BOOT_LBA);
    if (write_blob(dst, INST_BOOT_LBA, bs, 512) != 0) {
        kprintf("install: FAILED to write the boot sector\n");
        return -1;
    }

    kprintf("install: writing kernel image (%u bytes) @LBA %u ...\n",
            kernel_img_len, INST_KERNEL_LBA);
    if (write_blob(dst, INST_KERNEL_LBA, kernel_img, kernel_img_len) != 0) {
        kprintf("install: FAILED to write the kernel image\n");
        return -1;
    }

    kprintf("install: formatting TinyFS @LBA %u on %s ...\n", fs_lba, dst->name);
    disk_set_default(dst);
    tfs_set_base(fs_lba);
    tfs_set_limit(fs_secs);
    if (tfs_format() != 0) {
        disk_set_default(orig_default);
        kprintf("install: FAILED to format the filesystem\n");
        return -1;
    }

    kprintf("install: installing files ...\n");
    int nfiles = vfs_install_disk();
    kprintf("  %d entries written\n", nfiles);

    /* JVM 组件：未勾选时把刚写进 VFS/磁盘的 /bin/JVM.TNCR 删掉 */
    if (!with_jvm && jvm_bin_present)
        vfs_delete("/bin/JVM.TNCR");

    /* ---- 系统文件与账户 ---- */
    if (user_change_pass("root", rootpw) != 0)
        kprintf("install: warning: could not set the root password\n");
    else
        kprintf("  root password set\n");

    write_sshd_conf(with_ssh);
    vfs_write_file("/etc/installed", (const u8*)"TinyOS Genesis v0.1 installed by the built-in installer\n", 56);
    kprintf("  SSH server: %s\n", with_ssh ? "installed + autostart enabled" : "not installed");
    kprintf("  JVM runtime: %s\n", with_jvm ? "installed (/bin/JVM.TNCR)" : "not installed");

    /* 确保目录齐全（镜像里可能没有空目录） */
    vfs_mkdir("/root");
    vfs_mkdir("/home/guest");
    vfs_mkdir("/etc");

    /* 格式化期间默认盘指向目标盘，完事后指回原来那块（同一块盘则不变） */
    disk_set_default(orig_default);

    verify_install(dst, fs_lba);

    if (dst != orig_default && orig_default)
        kprintf("note: the disk this system is running from was left untouched.\n");

    kprintf("\ninstall: done. %u KB filesystem, %d entries.\n",
            tfs_capacity_kb(), tfs_file_count());
    kprintf("Remove the CD/kernel image and boot this disk (BIOS boot order), or:\n");
    kprintf("  qemu-system-i386 -drive file=<disk.img>,format=raw,if=ide,index=0\n");
    return 0;
}
