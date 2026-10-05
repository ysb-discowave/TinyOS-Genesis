# src/kernel

TinyOS Genesis v0.1 内核源码（i386 保护模式，裸机，从零手写）。

本目录是**完整源码**，不是子模块引用 —— 发行版自包含，不依赖任何外部仓库。

## 目录结构

| 路径 | 内容 |
|---|---|
| `include/` | 头文件（`api.h` 是用户程序 API 契约） |
| `start.asm` | 32 位保护模式入口 |
| `idt.asm` / `idt.c` | 中断描述符表 |
| `kernel.c` | 内核主流程与启动横幅 |
| `mm.c` | 物理内存管理（内存池、分配器） |
| `vfs.c` `fs/` | 虚拟文件系统、romfs（只读压缩镜像）、TinyFS |
| `romfs.c` | **构建生成**，勿手改 |
| `instimg.c` | **构建生成**（安装载荷），勿手改 |
| `process.c` | 进程与 TNCR 程序加载 |
| `tncr.c` | TNCR 格式解析与执行 |
| `shell.c` | 内核 shell |
| `console.c` `vga.c` `serial.c` | 控制台 / VGA 文本模式 / 串口 |
| `keyboard.c` `mouse.c` | PS/2 键盘与鼠标 |
| `pit.c` | 定时器（100Hz） |
| `mbr.c` `disk/` | MBR 分区表与多磁盘抽象层（ATA） |
| `net/` | e1000 网卡、FTP / SMB / socket |
| `user.c` | 账号与口令 |
| `desktop.c` `editor.c` | 图形桌面与内置编辑器 |
| `install.c` | 交互式安装向导 |
| `api.c` `genesis_api.c` | 用户程序 API 表（base + Genesis v0.1 扩展） |
| `inflate.c` `sha256.c` `libc.c` | 依赖：romfs 解压、校验、libc |
| `link.ld` | 链接脚本（内核基址 0x100000） |

## 用户程序 API

用户程序（`.TNCR`）**只能**通过 `tinyos_api_t` 函数指针表访问内核
（见 `include/api.h`），不能直接调用 `vfs_*` / `proc_*` / `inb/outb`。

- **base 字段**：`print` `println` `readline` `file_read` `file_write` `cmdarg` `cwd` …
- **Genesis v0.1 扩展**（追加在表末尾，磁盘上的旧 TNCR 不受影响）：
  `mkdir` `rm_file` `fs_list` `stat` `set_cwd` `proc_list_all` `proc_kill`
  `dev_list_all` `dev_read` `dev_write` `sysinfo` `date` `version`

扩展由 `genesis_api.c` 实现，在 `api.c` 的 `g_api` **静态初始化器**里绑定
（编译期完成，不依赖运行期初始化顺序）。

## 构建

由仓库根目录的 `build.ps1` 构建（nasm + zig cc）：

1. 汇编 `start.asm` / `idt.asm`
2. 编译本目录所有 `.c`（含 `net/` `disk/` `fs/` 子目录）
3. 用 `tools/gen_instimg.py` 生成安装载荷
4. 链接成 `build/kernel.elf`，并打出可引导的 `tinyos.img`

用户程序（tinysh / Lua）先由 `tools/build_users.py` 编成 TNCR 并写进
`romfs.c`，内核启动时挂载。

**不要**单独编译本目录——走 `build.ps1`，否则 `romfs.c` / `instimg.c` 会缺失。
