#ifndef TINYOS_PIT_H
#define TINYOS_PIT_H

#include "types.h"

void pit_init(void);     /* 100Hz 系统滴答 */
u32  pit_ticks(void);
u32  pit_seconds(void);  /* ticks/100 */
void pit_sleep(u32 ms);

#endif
