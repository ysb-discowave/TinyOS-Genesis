; TinyOS 中断桩 (32 位)
; 每个桩：push 0(伪错误码) / push 中断号 / jmp near isr_common
; isr_common 保存寄存器后调用 C 函数 isr_handler(regs_t*)
; 返回时调用 task_after_irq(帧顶 esp)：需要切换上下文时，用它返回的
; 新 esp 继续收尾（pop gs..popa; iret 作用在新任务的帧上）。
[bits 32]

global isr_stub_addrs
extern isr_handler
extern task_after_irq

%define IDT_ENTRIES 128

section .data
isr_stub_addrs:
%assign i 0
%rep IDT_ENTRIES
    dd stub_%+ i
%assign i i+1
%endrep

section .text
%assign i 0
%rep IDT_ENTRIES
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
    ; ---- 多进程切换点：把帧顶 esp 交给任务系统 ----
    mov eax, esp
    push eax
    call task_after_irq
    add esp, 4
    test eax, eax
    jz .no_switch
    mov esp, eax        ; 切换到新上下文（旧栈上的返回地址被丢弃，不再返回）
.no_switch:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8          ; 弹出 伪错误码 + 中断号
    iret
