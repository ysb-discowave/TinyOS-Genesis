/* ============================================================
 * TinyOS sample program 3: gui_demo
 * Language: C -- a program that needs a graphical window
 *
 * Key rule being demonstrated:
 *   . run from a terminal opened on the desktop -> a GUI window pops up
 *   . run from the plain command shell         -> the kernel raises an
 *                                                 exception and refuses
 * ============================================================ */
#include "api_user.h"

void user_main(tinyos_api_t *api) {
    api->println("gui_demo: requesting a GUI window ...");
    api->print("  current session id = ");
    { char t[16]; api->itoa(api->get_session(), t); api->println(t); }

    int r = api->gui_open("GUI window demo",
                          "This window was opened by a user program via a TinyOS syscall.\n"
                          "If you can read this, you are in a desktop terminal session.\n"
                          "Press any key to close this window.");

    if (r == 0) api->println("gui_demo: window shown and closed.");
    else        api->println("gui_demo: kernel refused the window (not a desktop session).");
}
