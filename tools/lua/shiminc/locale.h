/* TinyOS 裸机 shim：locale.h（Lua os/locale 需要） */
#ifndef TINYOS_SHIM_LOCALE_H
#define TINYOS_SHIM_LOCALE_H

#define LC_ALL       0
#define LC_COLLATE   1
#define LC_CTYPE     2
#define LC_MONETARY  3
#define LC_NUMERIC   4
#define LC_TIME      5
#define LC_MESSAGES  6
#define LC_PAPER     7
#define LC_NAME      8
#define LC_ADDRESS   9
#define LC_TELEPHONE 10
#define LC_MEASUREMENT 11
#define LC_CATEGORY_COUNT 12

typedef struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *decimal_point2;
} lconv;

const char *setlocale(int cat, const char *loc);
lconv *localeconv(void);

#endif
