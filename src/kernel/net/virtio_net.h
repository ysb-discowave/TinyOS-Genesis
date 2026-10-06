#ifndef TINYOS_VIRTIO_NET_H
#define TINYOS_VIRTIO_NET_H

/* virtio-net —— 半虚拟化网卡（QEMU/KVM/云主机最常见的 guest 网卡）。
 * 走统一网卡驱动注册表（见 nic.h）。 */
void virtio_net_register(void);

#endif
