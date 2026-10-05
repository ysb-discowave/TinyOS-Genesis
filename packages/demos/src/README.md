# packages/demos/src

TinyOS Genesis 演示程序集合（**说明包，无需编译**）。

本包不产出可安装的 `.tncr`。下面列出的 8 个示例程序已经由
`tools/build_users.py` 编译，并随内核 romfs 打包进 `/bin`，开机即可运行。
本包的作用是**把它们编成一份清单和说明**，方便用户快速体验 tinysh 与
TinyOS 的多语言/多能力工具链。

## 每个示例演示什么

| 程序 | 语言 | 大小(字节) | 演示内容 |
| --- | --- | --- | --- |
| `hello.TNCR` | C | 214 | 最小化 C 程序：验证 C 工具链与 TNCR 运行时能正常加载运行 |
| `count.TNCR` | C | 877 | 循环与计数器：演示 C 程序的基本控制流与终端输出 |
| `basic_demo.TNCR` | BASIC | 204 | 由 TinyBASIC 解释器运行的 BASIC 循环示例 |
| `cpp_demo.TNCR` | C++ | 339 | 最小化 C++ 程序：验证 C++ 工具链可用性 |
| `minic_demo.TNCR` | MiniC | 500 | MiniC 编译器示例（calc）：演示 MiniC 源到 `.tncr` 的编译 |
| `net_demo.TNCR` | C | 467 | 网络栈演示：使用 TinyOS 的 ARP/IPv4/TCP 能力 |
| `gui_demo.TNCR` | C | 470 | 桌面 / GUI 演示（`--gui`）：展示窗口与图形输出 |
| `tiny_demo.TNCR` | TinyLang | 238 | TinyLang 源程序示例：验证 TinyLang 工具链 |

## 怎么跑

在 TinyOS 的 tinysh 里直接运行（romfs 已挂载）：

```
run /bin/hello.TNCR
run /bin/net_demo.TNCR
run /bin/gui_demo.TNCR      # 需要桌面引擎
```

## 现状（为什么 manifest 里 sha256 仍是 `__BUILD_FILL__`）

这些 `.tncr` 由 `tools/build_users.py` 在构建内核时产出（输出到
`~/Desktop/tinyYY/tncr/`），并被打包进 romfs，不在本包内编译、也没有
单独的「安装产物」。因此本包 `demos.manifest` 没有可供 `pkg install`
落盘并校验 sha256 的 `.tncr`，`sha256` 保持 `__BUILD_FILL__`。

## 要补齐需要做什么（若想让 demos 也能 `pkg install`）

1. 决定交付形态：是把这 8 个 `.tncr` 作为数据文件随包发布，还是把源码
   放进 `src/` 让 `pkg` 在目标机重新编译。
2. 若随包发布：在 `packages/repo/` 下为每个 `.tncr` 准备 `<name>.tncr`
   + `<name>.manifest`，并回填各自 sha256。
3. 若源编译：在 `src/` 放入 `examples/` 下的源码，并把 `demos.manifest`
   的 `build.entry` / `build.cmd` 写成真实命令，构建后回填 sha256。
