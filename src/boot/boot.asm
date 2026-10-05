; ============================================================================
; TinyOS 引导扇区 (16 位实模式, 512 字节)
; ----------------------------------------------------------------------------
; 功能：
;   1) 初始化段寄存器与栈
;   2) 启用 A20 地址线
;   3) 用 BIOS 扩展读(INT 13h AH=42h) 分块把内核扁平映像读到物理地址 0x100000
;   4) 装载平坦 GDT，打开 CR0.PE 进入 32 位保护模式
;   5) 远跳转到 0x100000 的内核入口 (kernel/start.asm 中的 start)
;
; 磁盘布局：LBA 0 = 本引导扇区；LBA 1.. = kernel.bin（由构建脚本拼接）
;
; ---------------------------------------------------------------------------
; 踩过的坑，改动时务必注意：
;
;  A) 实模式下 seg:off 最多只能寻址到 0x10FFEF。BIOS 的 INT 13h 只能用
;     seg:off 当缓冲区地址，所以**没法**让 BIOS 直接把内核写进 1MB 以上。
;     本扇区的做法：每次用 BIOS 读 32KB 到低端暂存区 STAGE(0x20000)，
;     再进一次保护模式把这块数据搬到 0x100000 之上。
;
;  B) DAP（磁盘地址包）必须是标准 16 字节：
;       db 0x10 / db 0 / dw 扇区数 / dw 缓冲区偏移 / dw 缓冲区段 / dq LBA
;     曾经在"缓冲区"和"LBA"之间多写了一个 dd，BIOS 于是把 LBA 当成 0，
;     从 0 号扇区开始读，装载内容完全错位。
;
;  C) BIOS 只把第 1 个扇区（512 字节）装进 0x7C00，所有代码——包括
;     [bits 32] 的 pm_entry——都必须落在 0xAA55 签名之前。写在签名之后的
;     代码 BIOS 根本不会装载，jmp 过去就是一片没内容的内存。
;
;  D) 扇区 0 同时是 MBR：0x1BE..0x1FD 这 64 字节是**标准分区表**，
;     由安装向导填写（类型 0x7E = 内核分区，0x7F = TinyFS 分区）。
;     所以代码 + 数据总共只有 446 字节可用，本扇区是按这个预算写的：
;       * DAP 里"不变"的字段本来就是静态 0，不再现场写；
;       * 直接用 dap_lba 当读指针，不再另设 cur_lba 变量；
;       * 提示字符串压到最短；回实模式后不再重装 ss/sp（压根没动过它们）。
;
;  E) 远跳转的编码必须显式写，别指望 nasm 的前缀推断：
;       * 从**实模式**(16 位默认) 跳进保护模式：偏移给 32 位 -> 0x66 + EA；
;       * 在**32 位段**里跳（含跳内核）：偏移 32 位但**不能**带 0x66——带上
;         就被当 16 位操作数，CPU 只吃掉 5 字节，剩下 3 字节当成指令执行，
;         EIP 直接飞掉（这个 bug 让整机静默死机）。
;
;  E2) 回实模式**必须**先跳进一个 16 位代码段（GDT 0x18）再清 CR0.PE。
;      试过在 32 位段里直接清 PE，然后用 0x66 前缀的远跳（off16:seg16）
;      重装 CS —— 纸面上说得通，实测 QEMU 上连第一个字符都发不出来，
;      整机直接跑飞。多花 26 个字节换一条稳的路，值。
;
;  F) SeaBIOS 自己跑在 big-real-mode 里，用的是它自己的 GDT。我们的 lgdt 会
;     把 GDTR 改掉，所以进保护模式前要 sgdt 存一份、回来 lgdt 还回去，
;     否则第 2 次 INT 13h 会在 BIOS 内部崩掉。
; ============================================================================
[org 0x7C00]
[bits 16]

KERNEL_LBA   equ 1              ; 内核起始扇区
KERNEL_ADDR  equ 0x100000       ; 装载目标线性地址
KERNEL_MAX   equ 2048           ; 最多读 2048 扇区 = 1MB（后面是 TinyFS）
CHUNK        equ 64             ; 单次读取 64 扇区 = 32KB
STAGE_SEG    equ 0x2000         ; 暂存区段 -> 线性 0x20000（32KB）
STAGE_LIN    equ 0x20000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov [boot_drive], dl        ; BIOS 传入的启动盘号

    ; ---- 告诉内核：我是从这块盘被引导的 ----
    ; 内核 ata_init() 会读 0x500 处的 boot_info_t（magic 'TINY' + 盘号）来
    ; 区分"引导盘"和"数据盘"。缺少这一步，自举启动的内核只能盲猜设备，
    ; 单盘安装的系统就找不到自己的文件系统。
    mov dword [0x500], 0x54494E59     ; 'TINY' (little endian: 59 4E 49 54)
    mov [0x504], dl

    ; ---- COM1 初始化（内联，省 call/ret 的 4 个字节）----
    mov dx, 0x3FB
    mov al, 0x80                ; DLAB=1
    out dx, al
    mov dx, 0x3F8
    mov al, 0x01                ; 分频 1 -> 115200
    out dx, al
    mov dx, 0x3F9
    mov al, 0x00
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03                ; 8N1，关闭 DLAB
    out dx, al

    mov si, msg_boot
    call puts

    ; ---- 开 A20（内联）----
    in  al, 0x92               ; Fast A20
    or  al, 2
    and al, 0xFE               ; 不要顺手 reset
    out 0x92, al

    ; ---------------- 一次性填好 DAP 里不变的字段 ----------------
    ; 只有缓冲区"段"要现场写；缓冲区偏移和 LBA 高 32 位在数据区里本来就是
    ; 静态的 0，再写一遍纯属浪费本就紧张的字节预算。
    mov word [dap_buf + 2], STAGE_SEG
    mov word [dap_lba], KERNEL_LBA

    ; 实际扇区数由构建脚本写进本扇区的 kernel_sectors 字段（见下方 'KSCN'）。
    ; 硬编码上限会在小磁盘（如安装介质）上读到盘尾之外，BIOS 直接报读盘错误。
    mov ax, [kernel_sectors]
    or  ax, ax
    jnz have_count
    mov ax, KERNEL_MAX
have_count:
    mov word [sectors_left], ax
    mov dword [cur_addr], KERNEL_ADDR

read_loop:
    mov ax, [sectors_left]
    or  ax, ax
    jz  read_done

    mov cx, CHUNK
    cmp ax, CHUNK
    jae cnt_ok
    mov cx, ax                  ; 最后一块可能不足 64 扇区
cnt_ok:
    mov word [dap_count], cx
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc  disk_error

    ; ---- 进保护模式把这块搬到高端内存 ----
    cli
    sgdt [gdt_save]             ; 先把 BIOS 的 GDTR 存下来（见说明 F）
    lgdt [gdt_ptr]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    db 0x66, 0xEA               ; 实模式 -> 保护模式：偏移 32 位
    dd pm_copy
    dw 0x0008

[bits 32]
pm_copy:
    mov ax, 0x10                ; flat 数据段：base 0 / limit 4G
    mov ds, ax
    mov es, ax
    mov esi, STAGE_LIN          ; 源：低端暂存区
    mov edi, [cur_addr]         ; 目的：0x100000 + 已搬字节数
    movzx ecx, word [dap_count]
    shl ecx, 9                  ; 字节数 = 扇区数 * 512
    ; rep movsb 会把 ecx 减到 0，"add [cur_addr], ecx" 等于没加——
    ; 于是每一块都拷到同一个 0x100000，后面的块把前面的全覆盖了。
    ; 字节数必须提前另存一份。
    mov edx, ecx
    rep movsb
    add dword [cur_addr], edx
    ; ---- 回实模式，继续下一块（见说明 E2：必须先跳 16 位代码段）----
    db 0xEA
    dd rm16
    dw 0x0018

[bits 16]
rm16:
    mov eax, cr0
    and eax, 0x7FFFFFFE         ; 清 PE（顺手清 PG）
    mov cr0, eax
    db 0xEA                     ; 实模式远跳：重装 CS
    dw rm_next
    dw 0x0000

rm_next:
    lgdt [gdt_save]             ; 把 BIOS 的 GDTR 还回去
    xor ax, ax                  ; 回到干净的实模式段（BIOS 需要 DS=0 取 DAP）
    mov ds, ax
    mov es, ax
    movzx eax, word [dap_count]
    add dword [dap_lba], eax    ; dap_lba 本身就是读指针
    sub word [sectors_left], ax
    jmp read_loop

read_done:
    mov si, msg_loaded
    call puts

    ; ---------------- 真正进入保护模式，跳内核 ----------------
    cli
    lgdt [gdt_ptr]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    db 0x66, 0xEA               ; 实模式 -> 保护模式：偏移 32 位
    dd pm_entry
    dw 0x0008

; ---------------------------------------------------------------------------
; 32 位入口：必须和上面同处一个 512 字节的扇区里（见文件头说明 C）
; ---------------------------------------------------------------------------
[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000            ; 临时栈（1MB 以下空闲区）
    db 0xEA                     ; 保护模式内远跳：偏移 32 位，不带 0x66
    dd KERNEL_ADDR
    dw 0x0008                   ; -> 跳入内核 start

; ---------------- 16 位辅助（同样必须在前 446 字节内）----------------
[bits 16]
disk_error:
    mov si, msg_disk_err
    call puts
    cli
    hlt
    jmp disk_error

; ---- 同时写屏幕(INT 10h)和 COM1 ----
; 屏幕在 -display none 下看不见，串口在真机上没人接，所以两边都写。
puts:
    mov bx, 0x0007          ; INT 10h 不会破坏 bx，提到循环外
.next:
    lodsb
    or al, al
    jz .done
    ; 先走串口：INT 10h 的 0Eh **不保证**保留 AL（实测 SeaBIOS 会改掉它），
    ; 所以不能"先打屏幕再打串口"——那样串口收到的全是乱码。
    push ax
    mov dx, 0x3FD
.swait:
    in  al, dx
    test al, 0x20               ; THRE：发送保持寄存器空
    jz  .swait
    mov dx, 0x3F8
    pop ax
    out dx, al
    mov ah, 0x0E
    int 0x10
    jmp .next
.done:
    ret

; ---------------- 数据 ----------------
boot_drive    db 0
gdt_save      dw 0, 0, 0    ; 6 字节：BIOS 的 GDTR（进保护模式前存，回来要还）
sectors_left  dw 0
cur_addr      dd 0
msg_boot      db "TinyOS", 0x0A, 0
msg_loaded    db "ok", 0x0A, 0
msg_disk_err  db "e", 0

; 内核扇区数：'KSCN' 是给构建脚本用的定位标记（tools/gen_instimg.py 会把
; 实际扇区数写到标记之后的 2 字节里）。
kscn_marker   db "KSCN"
kernel_sectors dw 0

dap:                            ; 标准 16 字节，顺序不能动（见说明 B）
    db 0x10                     ; 结构大小
    db 0                        ; 保留
dap_count dw 0                  ; 扇区数
dap_buf   dw 0                  ; 缓冲区偏移(16 位)
dap_seg   dw 0                  ; 缓冲区段(16 位)
dap_lba   dd 0                  ; LBA 低 32 位
          dd 0                  ; LBA 高 32 位

; ---------------- 临时平坦 GDT ----------------
gdt_start:
    dd 0, 0
    dd 0x0000FFFF, 0x00CF9A00  ; 0x08 32 位代码段 base=0 limit=4G
    dd 0x0000FFFF, 0x00CF9200  ; 0x10 32 位数据段 base=0 limit=4G
    dd 0x0000FFFF, 0x00009A00  ; 0x18 16 位代码段 base=0 limit=64K（回实模式用）
gdt_end:
gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; ---------------- MBR 分区表 ----------------
; 0x1BE..0x1FD 是标准 MBR 的 4 个分区表项（每项 16 字节）。
; 安装向导会按用户的选择把它们填进引导扇区再写盘；内核开机时读扇区 0
; 解析它们，才知道 TinyFS 分区在哪（这样自定义分区也能自动挂载）。
; 这里先全 0 占位——全 0 的项表示"未使用"，内核会退回默认基址 2048。
;
; 若上面的代码 + 数据超出 0x1BE 字节，nasm 会因为 TIMES 为负而报错——
; 这正是"代码被挤到分区表/签名之后"的自动保险。
times 0x1BE - ($ - $$) db 0
part_table:
times 64 db 0
dw 0xAA55
