/* TinyOS 裸机 shim：time.h（Lua os 库需要） */
#ifndef TINYOS_SHIM_TIME_H
#define TINYOS_SHIM_TIME_H

typedef unsigned long time_t;
typedef long           clock_t;
#define CLOCKS_PER_SEC 1000

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;     /* 0-11 */
    int tm_year;    /* 距 1900 */
    int tm_wday;    /* 0-6，0=周日 */
    int tm_yday;    /* 0-364 */
    int tm_isdst;
};

time_t     time(time_t *t);
const char *ctime(const time_t *t);
struct tm *localtime(const time_t *t);
struct tm *gmtime(const time_t *t);
time_t     mktime(struct tm *t);
double     difftime(time_t t1, time_t t2);
clock_t    clock(void);
unsigned long strftime(char *dst, unsigned long maxsz, const char *fmt, const struct tm *t);

#endif
