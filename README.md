# TinyOS Genesis

> 自研轻量通用操作系统 · 首个正式发行版 **v0.1**

TinyOS Genesis 是一套**自包含**的裸机操作系统：i386 保护模式内核、系统 Shell、
原生可执行格式（TNCR）、包管理器与基础网络服务全部自研。

**本仓库不依赖任何外部仓库** —— 内核源码、用户程序构建链、Lua 源码都在仓库内，
克隆后即可独立构建出可启动镜像。

## 快速开始

### Windows

```bat
:: 1) 准备工具链（nasm + zig cc）
powershell -ExecutionPolicy Bypass -File tools\setup-toolchain.ps1

:: 2) 构建：用户程序 -> romfs -> 内核 -> 可启动镜像
python tools\build_users.py
powershell -ExecutionPolicy Bypass -File build.ps1

:: 3) 启动（双击也行）
start.bat
```

首次启动会创建 64MB 磁盘并运行 5 步安装向导（disk / network / software /
accounts / confirm）。第 4 步要**输入 root 密码**（至少 3 位，输入两遍），
那是你之后登录用的密码。

### Linux / macOS

```bash
./scripts/build.sh system     # 完整构建
```

内核链接与镜像打包目前依赖 Windows 下的 PowerShell + nasm（见 `build.ps1`）；
在其它系统上可直接调用 `zig cc` / `nasm`，参数见 `build.ps1`。

### 登录后

**登录即进入 tinysh** —— 它是这个系统的默认 shell，一个真正的用户态 TNCR 程序。

| 操作 | 说明 |
| --- | --- |
| `help` `ls` `ps` `devlist` `sysinfo` | tinysh 内置命令（共 19 条）|
| `exit` | 退回内核 shell（可继续用 `uname` / `mem` / `disk` / `net` / `lua`）|
| `login` / `logout` | 重新认证，会再次进入 tinysh |

内核 shell 里敲 `tinysh` 也能随时手动进入。

## 核心组成

| 组件 | 路径 | 状态 |
| --- | --- | --- |
| 内核 | `src/kernel/` | ✅ 完整源码（34 个 .c + 2 个 .asm）|
| 引导器 | `src/boot/boot.asm` | ✅ MBR + 引导扇区 |
| tinysh | `src/tinysh/` | ✅ 用户态 Shell，19 条命令，27 项自动化测试通过 |
| Lua 5.4.7 | `thirdparty/lua/` | ✅ 交叉编译为 `/bin/LUA.TNCR` |
| 构建链 | `tools/`、`build.ps1` | ✅ 自包含（nasm + zig cc + Python）|
| tncr 编译器 | `src/tncr/` | 🚧 规划中（TNCR 运行时已在内核里可用）|
| web-admin-panel | `src/web-admin-panel/` | 🚧 规划中 |

## 体积

| 项目 | Genesis v0.1 |
| --- | --- |
| romfs（压缩后只读文件系统）| **56 KB** |
| 内核 `kernel.elf` | **约 615 KB** |
| 可引导镜像 `tinyos.img` | **约 612 KB** |

## 统一文件体系（v0.1 定稿）

- **`.tncr`** — TinyOS 原生可执行二进制，内核直接加载运行。`LUA.TNCR`、`tinysh.TNCR` 均为此类
- **`.tsh`** — tinysh 系统 Shell 脚本
- **`.tscs`** — TNCR 编程语言源码（编译器规划中）

## 设计要点

- **用户程序只能经 API 表访问内核**。`.TNCR` 通过 `tinyos_api_t` 函数指针表调用内核，
  不能直接调 `vfs_*` / `proc_*` / `inb/outb`。Genesis v0.1 在表末尾追加了
  文件系统 / 进程 / 硬件寄存器 / 系统信息扩展字段，**不破坏磁盘上的旧 TNCR**。
- **界面文案一律英文 ASCII**。VGA 文本模式用 CP437 字形表，显示不了汉字
  （且一个汉字占 3 格会把整行顶出 80 列），所以所有上屏文案都是英文。
- **硬件寄存器可直接读写**。`devlist` / `readdev` / `writedev` 能访问 ISA 设备
  （com1 0x3F8、pit 0x40、kbd 0x60、vga 0x3D4 等）。
- **自动化测试**：全部由 QEMU 串口驱动，无人工点击。

## 文档

- `docs/genesis_overview.md` — 发行说明 / 总览
- `docs/tinysh_manual.md` — tinysh 使用手册
- `docs/tncr_manual.md` — TNCR 格式手册
- `docs/pkg_spec.md` — 包管理器与 manifest 规范
- `src/kernel/README.md` — 内核源码导览
- `site/` — 官方网站（纯静态，双击 `site/index.html` 即可打开）

## 路线图（规划中，v0.1 不包含）

多语言编译环境、C/C++/Python/Java 工具链、SMB/FTP 文件服务、TNCR 自举、
Docker 镜像、浏览器在线体验等。详见 `docs/genesis_overview.md` 第 8 节。

## 许可证

见 `LICENSE`（MIT）。
