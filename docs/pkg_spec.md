# 包管理器（pkg）与 manifest 规范

> 对应发行说明（genesis_overview.md §4）。v0.1 官方源为 GitHub Raw。

## 1. 官方软件源

```
https://raw.githubusercontent.com/ysb-discowave/TinyOS-Genesis/main/packages
```

（`ysb-discowave` 为官方源账号。）

## 2. 标准包结构

```
packages/xxx/
├─ xxx.manifest      # 包描述与构建规则
└─ src/              # 源码目录
```

## 3. manifest 字段规范

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `name` | string | 包名（与目录名一致） |
| `version` | string | 版本，统一跟随发行版（v0.1） |
| `description` | string | 一句话描述 |
| `dependencies` | list | 依赖包名列表（可为空） |
| `min_compiler` | string | 最低编译器版本（如 `tncr>=0.1`） |
| `sha256` | string | 产出 `.tncr` 的 SHA256 校验（安装时校验） |
| `build` | object | 构建指令：入口、输出 `.tncr` 路径、参数 |

示例（tinysh 包）：

```yaml
name: tinysh
version: v0.1
description: TinyOS Genesis 默认 Shell
dependencies: []
min_compiler: tncr>=0.1
sha256: <由 scripts/build.sh 生成后回填>
build:
  entry: src/tinysh.c
  output: /bin/tinysh.tncr
  cmd: zig cc -DTINYOS_USER -o tinysh.tncr src/tinysh.c src/kernel_api_tinyos.c
```

## 4. pkg 工作流程

- `pkg update` —— 拉取仓库索引（各包 manifest）。
- `pkg install xxx` —— 下载源码 → 读取 manifest 构建规则 → 调用对应语言编译器
  → 输出 `.tncr` 到系统目录（如 `/bin/`），并校验 `sha256`。

## 5. v0.1 已发布的官方包

- `tinysh` —— 默认 Shell（已实现，见 `packages/tinysh/`）
- `tncr` —— 编译器 / 运行时（规划中）
- `web-admin-panel` —— 官方 Web 管理面板示例
- `demos` —— 演示脚本集合

## 6. 本地软件源目录（sourcedir）

v0.1 的内核没有 HTTP 客户端，因此 `pkg install` **只从本地源目录读取**，
不联网。源目录由 `/etc/pkg.conf` 的 `sourcedir` 字段决定，缺省为
`/home/pkgrepo`（可用 `pkg source set <path>` 修改）。

源目录里每个包必须同时提供两个文件：

```
<name>.manifest     # 包描述与构建规则（YAML，见第 3 节）
<name>.tncr         # 已构建好的原生可执行（也可叫 <name>.TNCR，install 会回退识别）
```

`pkg install <name>` 的流程：在 `sourcedir` 下找到 `<name>.manifest` 与
`<name>.[tT]NCR` → 校验 sha256 → 写入 `/bin/<name>.TNCR` → 登记到
`/etc/packages.d/<name>`。

本仓库提供了可直接复制的源目录模板：`packages/repo/`（含 `tinysh.manifest`
+ `tinysh.tncr` + `pkg.conf.example` + `README.md`）。把它整体拷到
TinyOS 的 `/home/pkgrepo` 后，`pkg install tinysh` 即可工作。

## 7. install 的 sha256 校验

`install` 会从 manifest 的 `sha256` 字段取出期望哈希，对源目录里的
`<name>.[tT]NCR` 计算实际 SHA256 并比对：

- 一致：继续安装。
- 不一致：报错 `pkg: sha256 mismatch` 并中止，**不会**写入 `/bin`。
- manifest 没有 `sha256`（例如仍为占位符 `__BUILD_FILL__`）：打印
  `warning: no sha256 in manifest, skipping verification` 后仍然安装，
  但不做校验。

因此发布一个可安装的包时，务必把真实 `.tncr` 的 sha256 回填进
manifest 与 `packages/repo/` 下对应的 manifest。

## 8. pkg 子命令清单（v0.1）

```
pkg list                        # 列出已安装包（来自 /bin/*.TNCR + /etc/packages.d）
pkg info <name>                 # 显示某包详情（版本 / 来源 / sha256 / 大小 / 校验结果）
pkg verify [name]               # 校验已安装包的 sha256（不给 name 则校验全部）
pkg install <name>              # 从本地源目录安装（校验 sha256 后写入 /bin）
pkg remove <name>               # 删除已安装包（/bin 文件 + 登记记录）
pkg source [set <path>]         # 显示或设置本地源目录（写入 /etc/pkg.conf 的 sourcedir）
pkg help                        # 显示帮助
```

> **远程下载（HTTP/HTTPS）尚未实现**：v0.1 的 `pkg` 只能从本地源目录
> 安装，不支持从 GitHub Raw 等远程地址拉取。第 1 节的官方源 URL 仅为
> 规划中的目标地址，当前不可用。
