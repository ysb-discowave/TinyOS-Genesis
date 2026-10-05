/* ============================================================
 * TinyOS 内核链接脚本 (i386)
 * 内核加载到物理地址 0x100000 (1MB)，32 位保护模式运行。
 * - Multiboot 方式：用生成的 kernel.elf 直接启动（QEMU -kernel / GRUB）
 * - 扁平映像方式：objcopy -O binary 后由 boot/boot.asm 引导
 * ============================================================ */
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(start)

SECTIONS {
    . = 0x100000;

    /* 用 ALIGN(16) 与编译单元请求的 sh_addralign 保持一致，
       否则 ld.lld 会报 "section is not a multiple of alignment (16)"。 */
    /* start.asm 的入口代码(.entry)必须占据映像最前面：自带引导扇区是
       远跳到装载地址 0x100000 的，那里必须是可执行的指令而不是
       Multiboot 头。Multiboot 头放在其后（几十字节处），仍在规范要求的
       前 8KB 之内，QEMU -kernel / GRUB 一样认得。 */
    .text ALIGN(16) : {
        *(.entry)
        KEEP(*(.multiboot))
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
    }

    .bss ALIGN(16) : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* 内核栈独立成段，位于 BSS 之后，避免被 BSS 清零过程清掉 */
    .stack ALIGN(16) (NOLOAD) : {
        *(.stack)
    }

    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note*)
    }
}
