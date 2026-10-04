# TNCR 编译器 / 运行时手册（规划中）

> 状态：v0.1 中 `tncr` 作为两大核心系统程序之一被定位，但本手册尚在编写。
> 以下为发行说明（genesis_overview.md §2.2）摘录的设计约定，供实现参考。

## 定位

`/bin/tncr.tncr` 不属于 Shell，是独立的编译器 / 运行时。

## 两大核心模式

- **编译模式**：`tncr build xxx.tncs -o out.tncr`
  —— 将 TNCR 源码（`.tncs`）编译为原生 `.tncr` 可执行。
- **解释运行模式**：`tncr run xxx.tncs`
  —— 直接运行源码，无需编译。

## 文件体系

- `.tncs` — TNCR 编程语言源码文件（由 tncr 产出 `.tncr`）。
- `.tncr` — 原生可执行二进制，内核直接加载运行。

## v0.1 范围说明

- TNCR 自举（tncr 编译自身源码生成新版 tncr）为**规划中**特性，v0.1 不包含。
- 多语言编译环境（C/C++/Python/Java）同样为路线图项，不在 v0.1。

## 待补章节（实现时填写）

1. 语言语法与类型系统
2. 编译管线（前端 → 中端 → 后端 → `.tncr` 链接）
3. 运行时 ABI 与 `tinyos_api_t` 对接
4. 标准库清单
5. 错误码与诊断

详见 `docs/genesis_overview.md` 第 8 节路线图。
