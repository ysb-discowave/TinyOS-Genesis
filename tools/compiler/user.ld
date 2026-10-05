/* ============================================================
 * TinyOS 用户程序链接脚本
 * ------------------------------------------------------------
 * 固定链接地址 0x01000000 —— 内核 TNCR 加载器会把程序原样拷贝到该地址
 * 并跳转到 user_main，因此程序中的绝对地址引用有效。
 *
 * 说明：为了便于 objcopy -O binary 生成扁平映像，这里把 .bss/.COMMON
 * 一并放进 .data（以零填充形式写进映像），程序无需再做 BSS 清零。
 * ============================================================ */
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(user_main)

SECTIONS {
    . = 0x01000000;

    .text ALIGN(16) : {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(16) : {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(16) : {
        *(.data)
        *(.data.*)
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    __user_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note*)
        *(.rel.*)
        *(.rela.*)
    }
}
