/* ============================================================
 * 安装载荷的"占位"定义 —— 每次都参与编译，但**不**参与最终链接
 * ------------------------------------------------------------
 * 构建流程（build.ps1）：
 *   1) 编译全部 .c（其中 instimg_stub.c 提供空载荷）
 *   2) 用「除 kernel/instimg.c 之外」的所有目标文件链接出 payload.elf
 *      —— 此时 install.o 引用到的 boot_img/kernel_img 由本文件满足
 *   3) objcopy payload.elf -> kernel.bin；nasm boot.asm -> boot.bin
 *   4) tools/gen_instimg.py 把两者写成真正的 kernel/instimg.c
 *   5) 用「除本文件之外」的所有目标文件 + 重新编译的 instimg.o 链接正式内核
 *
 * 这样两个内核都带 `install` 命令，但只有正式内核携带可引导的镜像，
 * 而且不需要"自己嵌入自己"的鸡生蛋循环，产物是确定的。
 * ============================================================ */
#include "types.h"

const u8  boot_img[]     = { 0x00 };
const u32 boot_img_len   = 0;
const u8  kernel_img[]   = { 0x00 };
const u32 kernel_img_len = 0;
const u32 kernel_img_load = 0x100000u;
