; TinyOS 中断桩 (32 位)
; 每个桩：push 0(伪错误码) / push 中断号 / jmp near isr_common
; isr_common 保存寄存器后调用 C 函数 isr_handler(regs_t*)
[bits 32]

global isr_stub_addrs
extern isr_handler

section .data
isr_stub_addrs:
%assign i 0
%rep 48
    dd stub_%+ i
%assign i i+1
%endrep

section .text
%assign i 0
%rep 48
stub_%+ i:
    push 0
    push i
    jmp near isr_common
%assign i i+1
%endrep

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov eax, esp        ; eax -> regs（栈顶为 gs）
    push eax
    call isr_handler
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8          ; 弹出 伪错误码 + 中断号
    iret
