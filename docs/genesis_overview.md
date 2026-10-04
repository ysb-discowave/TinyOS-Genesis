# TinyOS Genesis v0.1 发行说明

首个正式发行版 · 定稿（v0.1 实际功能范围）

## 0. 总览

TinyOS Genesis v0.1 是自研轻量通用操作系统首个正式发行版。整套系统采用全自研体系：
内核、Shell、原生可执行格式、包管理器、基础 Web 服务均为自研 / 原生集成。

发行版核心定位（v0.1 已实现）：

- 本地源码编译、自托管
- 自带基础服务生态（pkg 包管理、service 服务管理、基础 Web 面板）
- 统一原生可执行格式 `.tncr`

> 说明：多语言编译环境（C/C++/Python/Java）、SMB/FTP 文件服务、Docker 镜像、
> 网页在线体验、TNCR 自举等高级特性尚在规划中，不在 v0.1 范围内（见第 8 节路线图）。

## 1. 统一文件体系（定稿、永不改动）

三种系统专属后缀：

- **`.tncr`** — TinyOS 原生可执行二进制文件，内核直接加载运行（替代旧 `.tle`）。
  注：`LUA.TNCR` 即内置 Lua 解释器。
- **`.tsh`** — tinysh 系统 Shell 脚本文件，由 `/bin/tinysh.tncr` 解释执行。
- **`.tncs`** — TNCR 编程语言源码文件，由 tncr 编译器产出 `.tncr` 原生程序
  （自举 / 自编译为规划中特性，见第 8 节）。

## 2. 两大核心系统程序

### 2.1 tinysh（系统默认 Shell）

- 程序路径：`/bin/tinysh.tncr`
- 作用：
  - 整机唯一默认交互式终端
  - 解析命令与参数、执行 `.tsh` 脚本
  - 提供 Tab 命令名补全、命令历史、内置系统命令
  - 调度所有系统程序：`tncr` / `pkg` / `service` / 所有 `.tncr`
- 内置能力（v0.1）：`cd pwd exit help history`，以及第 4 节全部内置命令。
- v0.1 明确不支持：管道 `|`、输出重定向 `>`、后台任务 `&`、引号包裹带空格字符串、
  通配符 `*`、环境变量 `$PATH`、命令别名 `alias`。
- 补全范围：v0.1 仅补全命令名，暂不实现文件路径补全。

### 2.2 tncr（编译器 + 运行时）

- 程序路径：`/bin/tncr.tncr`
- 不属于 Shell，是独立编译器 / 运行时。两大核心模式：
  - 编译模式：`tncr build xxx.tncs -o out.tncr` —— 将 TNCR 源码编译为原生 `.tncr` 可执行
  - 解释运行模式：`tncr run xxx.tncs` —— 直接运行源码，无需编译
- 说明：TNCR 自举（tncr 编译自身源码生成新版 tncr）为规划中特性，v0.1 不包含。

## 3. 系统预装生态（v0.1 实际）

内置服务端（v0.1）：

- 自研 Web 引擎（基础 HTTP 服务，含官方 web-admin-panel 示例面板）

系统官方工具（v0.1）：

- `pkg` 官方包管理器
- `service` 系统服务管理器
- `/bin/tinysh.tncr`、`/bin/tncr.tncr`、`/bin/LUA.TNCR`（内置 Lua 解释器）

> 说明：SMB/FTP 文件服务、Java（JVM）/C/C++/Python 编译环境在 v0.1 不包含，见路线图。

## 4. 包管理器架构（GitHub 托管）

### 4.1 包仓库地址（官方源）

直接使用 GitHub Raw 作为官方软件源，无需自建服务器、永久免费。

```
https://raw.githubusercontent.com/ysb-discowave/tinyos-genesis/main/packages
```

### 4.2 pkg 工作流程

- `pkg update` 拉取仓库索引
- `pkg install xxx`：下载源码 → 读取 manifest 编译规则 → 调用对应语言编译器
  → 输出 `.tncr` 到系统目录

### 4.3 标准包结构

```
packages/xxx/
├─ xxx.manifest
└─ src/            源码目录
```

### 4.4 manifest 统一规范

含：包名、版本、描述、依赖列表、最低编译器版本、SHA256 哈希校验、构建指令（输出 `.tncr`）。

## 5. GitHub 仓库整体结构

```
tinyos-genesis/
├─ README.md
├─ LICENSE
├─ docs/   (genesis_overview.md / tinysh_manual.md / tncr_manual.md / pkg_spec.md)
├─ src/
│  ├─ kernel/        # TinyOS 内核源码
│  ├─ tinysh/         # 默认 Shell 源码
│  ├─ tncr/           # TNCR 编译器源码
│  └─ web-admin-panel/
├─ packages/          # 官方 pkg 包 (tinysh / tncr / web-admin-panel / demos)
└─ scripts/           # 系统构建脚本
```

（注：`docker/` 目录为路线图项，v0.1 仓库不含。）

## 6. 系统启动流程

1. 内核初始化：硬件、网络栈、VFS、驱动
2. 启动用户空间默认终端 `/bin/tinysh.tncr`
3. 用户输入命令由 tinysh 解析；内部命令直接执行，外部程序由内核加载 `.tncr` 执行
4. 用户可调用 `tncr` 编译源码生成新 `.tncr`
5. 支持通过 `pkg` 动态安装官方仓库软件

## 7. 版本发布规则

- 主发行版代号：**Genesis**
- 首发版本：**v0.1**；所有组件版本统一跟随发行版
- 二进制成品发布于 GitHub Release；源码持续合并到 `main` 分支

## 8. 路线图（规划中，v0.1 不包含）

以下功能已在设计中，将在后续版本逐步落地：

- C / C++ / Python / Java（含 JVM）多语言编译与运行环境
- SMB / FTP 文件服务端与客户端
- TNCR 自举（tncr 编译自身源码）
- Docker 官方镜像（full / slim / dev 变体）
- 浏览器在线体验（WebSocket → ttyd → tinysh；内置 Web 管理面板 iframe）
- 网页一键在线演示整系统
- tinysh 后续增强：脚本执行、环境变量、通配符、管道与重定向（见 tinysh 手册 v0.2 规划）

## 9. 核心特色总结（v0.1 已实现）

- 全自研可执行体系 `.tncr` / `.tsh` / `.tncs`
- 独立 Shell（tinysh）+ 独立编译器（tncr）解耦架构
- 统一原生可执行格式，内核直接加载
- 内置 Lua 解释器（`LUA.TNCR`）
- 官方 `pkg` 包管理器 + `service` 服务管理器
- GitHub 原生软件源，支持自托管与动态安装

## 10. 一句话总结

TinyOS Genesis v0.1 是一套拥有自研内核、自研 Shell、自研可执行格式与编译语言、
官方包管理生态的完整自研操作系统首个正式发行版；多语言工具链、文件服务与在线体验
等高级特性将在后续版本逐步开放。

> 本文档为 v0.1 实际功能范围的修订定稿；凡标注“路线图”的均为未实现特性，
> 以 tinysh 使用手册与源码为准。
