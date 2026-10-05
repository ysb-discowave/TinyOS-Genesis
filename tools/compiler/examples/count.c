/* ============================================================
 * TinyOS sample program 2: count
 * Language: C -- demonstrates loops, integer formatting, the syscall table
 * ============================================================ */
#include "api_user.h"

void user_main(tinyos_api_t *api) {
    char b[16];
    api->println("Counting from 1 to 5:");
    for (int i = 1; i <= 5; i++) {
        api->print("  item ");
        api->itoa(i, b);
        api->print(b);
        api->println("");
    }
    api->println("Loop finished.");

    /* bubble sort -- proves a user program has real memory and arithmetic */
    int a[8] = { 42, 7, 19, 3, 88, 1, 56, 23 };
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 7 - i; j++)
            if (a[j] > a[j + 1]) { int t = a[j]; a[j] = a[j + 1]; a[j + 1] = t; }
    api->println("Sorted:");
    for (int i = 0; i < 8; i++) { api->itoa(a[i], b); api->print(b); api->print(" "); }
    api->println("");
}
