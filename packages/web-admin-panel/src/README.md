# packages/web-admin-panel/src

TinyOS 官方 Web 管理面板示例（**规划中，尚未实现**）。

## 规划做什么

提供一个可通过 TinyOS 自研 Web 引擎托管的官方管理面板，让用户用浏览器
管理 TinyOS 主机（查看系统状态、文件、网络等）。计划产出 `/bin/web-admin-panel.tncr`
作为面板宿主程序，并附带静态资源（HTML / CSS / JS，由 TinyOS Web 引擎渲染）。

## 现状（为什么 manifest 里 sha256 仍是 `__BUILD_FILL__`）

- 目前 `src/` 下**没有任何源码或静态资源**，`manifest` 里的
  `build.entry`（`src/index.tncr`）和 `build.output`（`/bin/web-admin-panel.tncr`）
  都还不存在。
- TinyOS 的 Web 引擎（内核侧 HTTP 服务 + 页面渲染）能力是否就绪、面板
  宿主程序如何与内核 API 对接，尚待确认。
- 因此本包**没有可安装的 `.tncr` 产物**，`sha256` 暂无可校验对象，保持
  `__BUILD_FILL__`。

## 要补齐需要做什么

1. 确认内核 Web 引擎的托管方式（HTTP 服务入口、页面根目录约定、与
   `/bin/web-admin-panel.tncr` 的关系）。
2. 在 `src/` 下补齐面板宿主程序与静态资源（页面、脚本、样式）。
3. 把 `web-admin-panel.manifest` 的 `build.entry` / `build.cmd` 写成真实
   可执行的构建命令，并产出 `/bin/web-admin-panel.tncr`。
4. 构建后用 `hashlib.sha256` 算出 sha256 回填 manifest 并加入 `packages/repo/`，
   即可 `pkg install web-admin-panel`。
