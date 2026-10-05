/* ============================================================
 * TinyOS sample program 1: hello
 * Language: C (real x86 machine code, cross-compiled by LLVM/clang)
 * Build:  tcc hello.c -o hello.TNCR
 * Run:    run /bin/hello.TNCR
 * ============================================================ */
#include "api_user.h"

void user_main(tinyos_api_t *api) {
    api->println("========================================");
    api->println("Hello from TinyOS native code!");
    api->println("A user program running on a 32-bit bare-metal kernel.");
    api->println("========================================");
}
