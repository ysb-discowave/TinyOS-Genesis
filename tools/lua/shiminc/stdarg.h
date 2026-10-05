/* TinyOS 裸机 shim：stdarg.h
 * 用 zig cc 的 GCC 内建 va 机制（__builtin_va_*），由编译器保证
 * cdecl 栈布局 / double 的 8 字节对齐，比自己手写 esp 偏移可靠。 */
#ifndef TINYOS_SHIM_STDARG_H
#define TINYOS_SHIM_STDARG_H

typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_copy(d, s)       __builtin_va_copy(d, s)

#endif
