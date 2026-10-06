#include "tncr.h"
#include "vfs.h"
#include "process.h"
#include "api.h"
#include "console.h"
#include "vga.h"
#include "mm.h"
#include "desktop.h"
#include "libc.h"

extern tinyos_api_t g_api;

/* 常驻程序：当前装载在 TNCR_LOAD_ADDR 的程序路径。
 *
 * 所有 TNCR（含 tinysh 与全部 app）都链接/装载到同一个固定地址 0x01000000，
 * 因此启动一个 app 会把 tinysh 的代码/数据/bss 覆盖掉。app 的入口返回后必须
 * 把“父程序”的映像原样恢复回该地址，否则返回到 tinysh 时执行的是 app 残留
 * 字节；更隐蔽的是：若“从 romfs 重新装载”父程序，会把 g_api / g_lua_api /
 * g_cwd / 历史 等运行时全局量清零（它们的文件初值就是 0），导致 tinysh 的
 * REPL 一恢复就 ksh_readline 拿到 g_api==NULL 而 EOF 退出 —— 表现正是
 * “推出应用后根本没有回到 tinysh / 掉回内核 shell”。
 *
 * 正确做法：在装载子程序之前，把父程序当前在 0x01000000 的整段映像原地快照，
 * 子程序返回后再把这段字节原样写回去。这样父程序的运行时全局量一个字节都不丢。
 * 注意：栈在独立的内核栈上，不在这段映像里，子程序运行不会动到它。
 *
 * 初始为 /bin/tinysh.TNCR：内核启动后第一个跑的就是它，且它永远是
 * 普通 Shell 会话（session 0）的父程序。嵌套场景（如桌面里再起一个 app）
 * 由下面 tncr_run 里的 parent/resident 配对自动处理。 */
static char g_resident[256] = "/bin/tinysh.TNCR";

/* 仅把 path 指向的 TNCR 代码/数据/bss 装载到 TNCR_LOAD_ADDR（不做会话/进程
 * 记账，也不调用入口）。返回 0 成功，-1 失败。仅在拿不到快照时作为兜底使用，
 * 因为从 romfs 重载会把运行时全局量（g_api 等）清零。 */
static int tncr_load_at(const char *path) {
    u32 size = 0;
    const u8 *data = vfs_read_file(path, &size);
    if (!data) return -1;
    if (size < TNCR_HDR_SIZE || memcmp(data, TNCR_MAGIC, 4) != 0) return -1;
    u32 load_addr = *(const u32*)(data + 8);
    u32 code_size = *(const u32*)(data + 16);
    u32 bss_size  = *(const u32*)(data + 20);
    if (load_addr == 0) load_addr = TNCR_LOAD_ADDR;
    if (code_size > TNCR_LOAD_MAX || TNCR_HDR_SIZE + code_size > size) return -1;
    u8 *mem = (u8*)load_addr;
    memcpy(mem, data + TNCR_HDR_SIZE, code_size);
    if (bss_size) memset(mem + code_size, 0, bss_size);
    return 0;
}

/* 取一个 TNCR 程序映像的总字节数（code_size + bss_size）。取不到返回 0。 */
static u32 tncr_image_size(const char *path) {
    u32 size = 0;
    const u8 *data = vfs_read_file(path, &size);
    if (!data || size < TNCR_HDR_SIZE || memcmp(data, TNCR_MAGIC, 4) != 0) return 0;
    u32 code = *(const u32*)(data + 16);
    u32 bss  = *(const u32*)(data + 20);
    if (code > TNCR_LOAD_MAX) return 0;
    return code + bss;
}

int tncr_run(const char *path, int session_id) {
    u32 size = 0;
    const u8 *data = vfs_read_file(path, &size);
    if (!data) {
        kprintf("TinyOS: executable not found: %s\n", path);
        return -1;
    }
    if (size < TNCR_HDR_SIZE || memcmp(data, TNCR_MAGIC, 4) != 0) {
        kprintf("TinyOS: invalid TNCR file: %s\n", path);
        return -1;
    }
    u32 flags     = *(const u32*)(data + 4);
    u32 load_addr = *(const u32*)(data + 8);
    u32 entry_off = *(const u32*)(data + 12);
    u32 code_size = *(const u32*)(data + 16);
    u32 bss_size  = *(const u32*)(data + 20);

    if (load_addr == 0) load_addr = TNCR_LOAD_ADDR;
    if (code_size > TNCR_LOAD_MAX || TNCR_HDR_SIZE + code_size > size) {
        kprintf("TinyOS: bad TNCR segment size (%u bytes): %s\n", code_size, path);
        return -1;
    }

    u32 child_img = code_size + bss_size;

    /* 记录“父程序”（当前常驻者），子程序退出后恢复它。 */
    char parent[256];
    strncpy(parent, g_resident, sizeof parent - 1);
    parent[sizeof parent - 1] = 0;

    /* 父程序映像大小，用于回滚后清理“子程序比父程序大”的残留段。 */
    u32 parent_img = tncr_image_size(parent);

    /* 子程序会覆盖 [0, child_img) 这段常驻映像，先把当前（父程序）的这段
     * 字节原地快照下来；子程序返回后用快照回滚，这样父程序的运行时全局量
     * （g_api / g_lua_api / g_cwd / 历史 等）一个字节都不丢，REPL 才能继续。
     * 取 max(child_img, parent_img) 以覆盖子程序可能比父程序大的情形。 */
    u32 snap = child_img > parent_img ? child_img : parent_img;
    u8 *saved = NULL;
    if (snap > 0 && snap <= TNCR_LOAD_MAX) {
        saved = (u8*)kmalloc(snap);
        if (saved) memcpy(saved, (const void*)load_addr, snap);
    }

    /* 拷贝子程序代码/数据到约定装载地址（程序按此地址链接，故绝对引用有效） */
    u8 *mem = (u8*)load_addr;
    memcpy(mem, data + TNCR_HDR_SIZE, code_size);
    if (bss_size) memset(mem + code_size, 0, bss_size);

    user_entry_t fn = (user_entry_t)(load_addr + entry_off);

    int pid = proc_spawn(path, session_id);
    proc_set_session(session_id);

    /* path 现在占了装载地址，更新常驻记录（供嵌套场景正确配对 parent）。 */
    strncpy(g_resident, path, sizeof g_resident - 1);
    g_resident[sizeof g_resident - 1] = 0;

    if (flags & TNCR_F_DESKTOP) {
        /* 桌面启动器：直接进入图形化桌面（DESKTOP.TNCR） */
        desktop_enter();
    } else {
        fn(&g_api);
    }

    proc_set_session(0);
    proc_finish(pid);

    /* 子程序返回：把常驻映像回滚成父程序（用快照，而非从 romfs 重载——
     * 重载会把 g_api 等运行时全局量清零，导致父程序（tinysh）REPL 因
     * g_api==NULL 而立刻 EOF 退出）。 */
    strncpy(g_resident, parent, sizeof g_resident - 1);
    g_resident[sizeof g_resident - 1] = 0;
    if (saved) {
        memcpy((void*)load_addr, saved, snap);
        /* 若子程序映像比父程序大，超出父映像的那段是子程序残留，清零。 */
        if (snap > parent_img)
            memset((void*)(load_addr + parent_img), 0, snap - parent_img);
        kfree(saved);
    } else {
        tncr_load_at(parent);   /* 兜底：没拿到快照才从 romfs 重载 */
    }
    return 0;
}
