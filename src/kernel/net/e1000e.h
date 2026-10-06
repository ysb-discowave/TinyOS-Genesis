#ifndef TINYOS_E1000E_H
#define TINYOS_E1000E_H

/* Intel 8257x/8256x-PCIe GbE（82574L、82577、82578、82579、82583、82586…）
 * 走统一网卡驱动注册表（见 nic.h）。 */
void e1000e_register(void);

#endif
