# src/kernel

TinyOS 2.0 内核源码（本仓库引用 / 子模块）。

提供 `tinyos_api_t` 用户态接口（见 `kernel/include/api.h`），`.tncr` 程序只能
通过该表访问内核。Genesis v0.1 在表末尾追加了 fs/proc/hw/系统信息 扩展字段。

> 内核源码不在本发行版仓库内联，以子模块或独立仓库方式提供。
