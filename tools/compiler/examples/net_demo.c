/* ============================================================
 * TinyOS sample program 4: net_demo
 * Language: C -- demonstrates TinyOS networking (FTP download + NIC state)
 *
 * Usage (inside TinyOS):
 *   net_demo                 download hello.txt
 *   net_demo <host> <port> <remote> <local>
 * ============================================================ */
#include "api_user.h"

void user_main(tinyos_api_t *api) {
    api->println("net_demo: TinyOS networking demo");
    api->net_info();
    api->println("FTP: fetch hello.txt from 10.0.2.2:2121 -> /home/dl.txt ...");

    int r = api->ftp_get("10.0.2.2", 2121, "tinyos", "tinyos", "hello.txt", "/home/dl.txt");
    api->println(r == 0 ? "result: download OK" : "result: download FAILED");

    if (r == 0) {
        api->println("Now uploading it back with the net command:");
        int r2 = api->ftp_put("10.0.2.2", 2121, "tinyos", "tinyos", "/home/dl.txt", "up.txt");
        api->println(r2 == 0 ? "result: upload OK" : "result: upload FAILED");
    }
}
