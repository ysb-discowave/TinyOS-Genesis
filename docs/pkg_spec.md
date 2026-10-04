# 包管理器（pkg）与 manifest 规范

> 对应发行说明（genesis_overview.md §4）。v0.1 官方源为 GitHub Raw。

## 1. 官方软件源

```
https://raw.githubusercontent.com/ysb-discowave/tinyos-genesis/main/packages
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
