# tinysh — TinyOS Genesis v0.1 系统默认 Shell

`tinysh` 是 TinyOS Genesis v0.1 的内置交互式命令解释器（Shell 终端），
作为用户与内核交互的入口。本目录为其源码，严格按《tinysh 使用手册》实现。

## 特性（手册 §1）

- 单行命令解析，空格分割参数
- 内置命令执行，统一返回 `int` 错误码（§5）
- 命令历史记录（最多 32 条，↑/↓ 翻阅）
- Tab 命令名自动补全（v0.1 不做路径补全）
- 大小写敏感
- 行首 `#` 为注释
- **v0.1 不支持**：管道 `|`、输出重定向 `>`、后台任务 `&`、引号包裹带空格字符串、通配符 `*`

## 目录文件

| 文件 | 说明 |
|------|------|
| `tinysh.h`          | 公共头：错误码、命令表结构、历史/行读取接口 |
| `tinysh.c`          | 主程序：REPL（§6）、解析、历史、Tab 补全、全部内置命令 |
| `kernel_api.h`      | tinysh 与内核之间的接口约定（§7）|
| `kernel_api_host.c` | 宿主机后端（开发/自测，可独立运行验证）|
| `kernel_api_tinyos.c` | TinyOS 2.0 用户态后端（产品用，走 `tinyos_api_t`）|

## 构建与运行（宿主机自测）

默认用 **zig**（TinyOS 工具链自带）编译宿主机版本：

```bash
zig cc -std=c11 -O2 -o tinysh tinysh.c kernel_api_host.c
./tinysh                      # 交互
./tinysh < test_session.txt   # 非交互自测
```

Windows 用户可用 `Desktop/tinyYY/toolchain/zig-windows-x86_64-0.14.0/zig.exe`。

## 接入真实内核（产品后端，TinyOS 2.0）

> ⚠️ 关键事实：TinyOS 2.0 的用户态 `.tncr` 程序**只能**通过内核导出的
> `tinyos_api_t` 函数指针表访问内核（见 `TinyOS-2.0/kernel/include/api.h`），
> **不能直接调用** `vfs_*` / `proc_*` / `inb` / `outb` 等内核内部符号。

`kernel_api_tinyos.c` 严格走该表：

- 文本 I/O：`print` / `println`（tinysh.c 的 `printf`/`fputs` 在 TinyOS 构建下由
  TNCR stdio shim 接管并映射到这些字段）
- 文件读写：`file_read` / `file_write`（基础表已提供）
- 工作目录：`cwd`（基础表已提供，`fs_chdir` 由 tinysh 本地维护 cwd 字符串）
- 手册 §7 所需的 `fs_list` / `fs_mkdir` / `fs_delete` / `fs_stat`、
  `proc_spawn` / `proc_list` / `proc_kill`、`hw_*`、版本/系统信息 等：
  为 **Genesis v0.1 在 `tinyos_api_t` 末尾追加的扩展字段**（`genesis_api_t`），
  旧内核只读前面的字段不受影响（api.h 注释明说可末尾追加）。

编译方式与项目内 `LUA.TNCR` 一致：随 TinyOS 2.0 用户态 + stdio shim 由 zig
交叉编译为 `/bin/tinysh.tncr`，并定义 `-DTINYOS_USER` 启用 `user_main` 入口：

```bash
zig cc -std=c11 -DTINYOS_USER \
       -DTINYOS_KERNEL_ROOT=.../TinyOS-2.0/kernel/include \
       -I.../TinyOS-2.0/kernel/include \
       -o tinysh.tncr tinysh.c kernel_api_tinyos.c
```

执行 `make tinyos` 等价。

## 验证状态

- 宿主机后端：已编译通过并跑通 `test_session.txt`，输出与手册 §9 示例一致
  （注释忽略、参数错/文件不存在/未知命令/IO 错误 的错误码全部正确）。
- TinyOS 后端：`tinyos_api_t` 字段引用经真实 `api.h` 语法核对无误；需配合
  TNCR stdio shim 与 Genesis 扩展内核方可整体编出 `.tncr`。
