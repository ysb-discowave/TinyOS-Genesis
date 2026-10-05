# packages/tncr/src

TNCR 编译器前端源码（**规划中，尚未实现**）。

## 规划做什么

`tncr` 包目标是提供 **TNCR 编译器**，把 TNCR 源码（`.tncs`）编译为内核
可直接加载运行的原生可执行 `.tncr`。设计上的两大模式见 `docs/tncr_manual.md`：

- 编译模式：`tncr build xxx.tncs -o out.tncr`
- 解释运行模式：`tncr run xxx.tncs`

## 现状（为什么 manifest 里 sha256 仍是 `__BUILD_FILL__`）

- **TNCR 运行时已经可用**：内核侧的 `src/kernel/tncr.c` 已经能够加载并运行
  `.tncr` 二进制，romfs 里现有一批真实 `.tncr`（hello / count / net_demo /
  gui_demo / tiny_demo / basic_demo / minic_demo / cpp_demo 等）就是运行时
  跑起来的。也就是说「跑 `.tncr`」这条路已经通。
- **缺的是编译器前端**：目前**没有**能把 `.tncs` 源码产出 `.tncr` 的编译器
  程序（即 `/bin/tncr.tncr` 这个编译器本身还不存在），也没有 `src/main.tncs`
  等前端源码。因此本包**没有可安装的产物**，manifest 的 `build.output`
  （`/bin/tncr.tncr`）无法生成，`sha256` 暂无可校验对象，保持 `__BUILD_FILL__`。

## 要补齐需要做什么

1. 实现编译器前端（词法 / 语法 / 语义 → 中端 IR → 后端代码生成 → 链接出
   `.tncr`），产出 `/bin/tncr.tncr`。
2. 在 `src/` 下补齐前端源码（如 `src/main.tncs` 或对应的 C/TinyLang 实现），
   并把 `tncr.manifest` 的 `build.entry` / `build.cmd` 写成真实可执行的构建命令。
3. 真正构建出 `.tncr` 后，用
   `python -c "import hashlib;print(hashlib.sha256(open('tncr.tncr','rb').read()).hexdigest())"`
   算出 sha256 回填到 `tncr.manifest` 与 `packages/repo/tncr.manifest`，
   再把 `tncr.tncr` 放进 `packages/repo/`，即可 `pkg install tncr`。

> 注意：`tncr` 包只负责**编译器**；运行时属于内核（`src/kernel/tncr.c`），
> 不在此包内。
