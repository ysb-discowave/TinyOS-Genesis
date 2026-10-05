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

    /* 拷贝代码到约定装载地址（程序按此地址链接，故绝对引用有效） */
    u8 *mem = (u8*)load_addr;
    memcpy(mem, data + TNCR_HDR_SIZE, code_size);
    if (bss_size) memset(mem + code_size, 0, bss_size);

    user_entry_t fn = (user_entry_t)(load_addr + entry_off);

    int pid = proc_spawn(path, session_id);
    proc_set_session(session_id);

    if (flags & TNCR_F_DESKTOP) {
        /* 桌面启动器：直接进入图形化桌面（DESKTOP.TNCR） */
        desktop_enter();
    } else {
        fn(&g_api);
    }

    proc_set_session(0);
    proc_finish(pid);
    return 0;
}
