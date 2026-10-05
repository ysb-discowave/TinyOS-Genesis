; ============================================================
; TinyOS 内核入口 (i386, 32 位保护模式)
; ------------------------------------------------------------
; 支持两种装载方式：
;   1) Multiboot (GRUB / QEMU -kernel)：由引导程序直接进入 32 位模式并跳转到这里，
;      ebx = multiboot 信息结构指针。
;   2) 自带引导扇区 (boot/boot.asm) 把内核扁平映像读到 0x100000 后远跳至此。
; 本文件负责：建立内核栈(GDT 已由引导程序或自带 bootloader 建立)、清零 BSS、
; 保存 multiboot 指针，然后进入 C 内核 kernel_main()。
; ============================================================
[bits 32]

; ---- Multiboot v1 头（必须位于映像前 8KB 内）----
MB_MAGIC  equ 0x1BADB002
MB_FLAGS  equ 0x00000003          ; bit0=模块按页对齐, bit1=提供内存信息
MB_CKSUM  equ -(MB_MAGIC + MB_FLAGS)

; ---------------------------------------------------------------
; 入口代码必须排在**映像最前面**。
; 自带引导扇区（boot/boot.asm）读完扁平映像后是远跳到 0x100000 的，
; 也就是说映像的第 1 个字节就得是可执行指令。以前 .multiboot 排在
; 前面，0x100000 处的 02 B0 AD 1B 被 CPU 解码成
;   add dh,[eax+3]  ->  eax 装着 cr0 的值，读未映射内存 -> #PF -> 三重故障
; 于是自举启动的内核永远活不到 kernel_main（串口一个字节都没有）。
; 把入口挪到 .entry 段、Multiboot 头放它后面（偏移几十字节，仍在
; 8KB 之内，完全符合 Multiboot 规范），两条装载路径就都对了。
; ---------------------------------------------------------------
section .entry align=16
global start
extern kernel_main
extern __bss_start
extern __bss_end

start:
    cli                         ; 引导阶段先关中断
    mov esp, stack_top          ; 建立内核栈
    mov ebp, 0

    ; ---- 启用 FPU（保护模式下 x87 默认禁用）----
    ; CR0.EM(bit2)=1  -> 任何 FPU 指令都触发 #DE；
    ; CR0.MP(bit1)=1  -> "monitor co-processor" 模式，FPU 结果不更新寄存器。
    ; QEMU 经 multiboot 进 32 位模式时这两位是置位的，必须清掉才能用
    ; x87。清掉后用户程序里的 double 运算（Lua 的 2^10 等）才不炸。
    mov eax, cr0
    and eax, 4294967287         ; 0xFFFFFFF7：清 bit1(MP) 和 bit2(EM)
    mov cr0, eax

    finit                       ; 清 x87 寄存器栈 + 设默认控制字
    emms                        ; 清 MMX 状态（若有 80387EX）

    ; 保存 multiboot 信息指针（edi 会被 rep stosb 使用，故先存到 esi）
    mov esi, ebx

    ; 清零 BSS（stack 位于 .bss 之后的独立段，不会被清）
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    push esi                    ; 第 1 个参数：multiboot 信息指针
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

; ---- Multiboot v1 头：紧跟入口代码之后，仍在映像前 8KB 之内 ----
section .multiboot align=4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CKSUM

; ---- 内核栈 64KB（独立段，链接脚本置于 .bss 之后，故不被清零）----
; TNCR 用户程序（全屏编辑器 EDIT.TNCR、MiniC 编译器 CC.TNCR 的递归下降
; 语法分析）也跑在这个栈上，16KB 不够用，扩到 64KB。
section .stack nobits
align 16
stack_bottom:
    resb 0x10000
stack_top:
