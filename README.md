# TinyOS Genesis

> 自研轻量通用操作系统 · 首个正式发行版 **v0.1**

TinyOS Genesis 是一套全自研体系：内核、Shell、原生可执行格式、包管理器、基础 Web
服务均为自研 / 原生集成。v0.1 已实现本地源码编译、自托管，以及基础服务生态
（pkg 包管理、service 服务管理、官方 Web 管理面板）。

## 核心组成

| 组件 | 路径 | 说明 |
| --- | --- | --- |
| 内核 | `src/kernel/` | TinyOS 2.0 内核源码（含 `tinyos_api_t` 用户态接口） |
| tinysh | `src/tinysh/` | 系统默认 Shell（本仓库已实现并通过验证） |
| tncr | `src/tncr/` | TNCR 编译器 + 运行时（规划/开发中） |
| web-admin-panel | `src/web-admin-panel/` | 官方 Web 管理面板示例 |

## 统一文件体系（v0.1 定稿）

- **`.tncr`** — TinyOS 原生可执行二进制，内核直接加载运行（替代旧 `.tle`）。`LUA.TNCR` 即内置 Lua 解释器。
- **`.tsh`** — tinysh 系统 Shell 脚本，由 `/bin/tinysh.tncr` 解释执行。
- **`.tncs`** — TNCR 编程语言源码，由 tncr 编译器产出 `.tncr`。

## 快速开始（开发自测 tinysh）

```bash
cd src/tinysh
# 用 zig（本项目工具链）编译宿主机可运行版
zig cc -std=c11 -O2 -o tinysh.exe tinysh.c kernel_api_host.c
# 或：make            # 默认 host 后端
./tinysh.exe < test_session.txt
```

> 说明：宿主机后端（`kernel_api_host.c`）仅用于开发自测；产品构建改链
> `kernel_api_tinyos.c`，随 TinyOS 2.0 用户态 + stdio shim 由 zig 交叉编译为
> `/bin/tinysh.tncr`（定义 `-DTINYOS_USER`）。

## 文档

- `docs/genesis_overview.md` — 发行说明 / 总览
- `docs/tinysh_manual.md` — tinysh 使用手册（已实现）
- `docs/tncr_manual.md` — TNCR 编译器手册（规划中）
- `docs/pkg_spec.md` — 包管理器与 manifest 规范

## 包与脚本

- `packages/` — 官方 pkg 包（tinysh / tncr / web-admin-panel / demos），每包含 `*.manifest` + `src/`
- `scripts/` — 系统构建脚本

## 路线图（规划中，v0.1 不包含）

多语言编译环境（C/C++/Python/Java）、SMB/FTP 文件服务、TNCR 自举、Docker 镜像、
浏览器在线体验等。详见 `docs/genesis_overview.md` 第 8 节。

## 许可证

见 `LICENSE`（MIT）。
