# packages/repo —— 本地软件源目录模板

本目录是 `pkg` 命令所需的**本地软件源（source dir）**的内容模板。
内核 `pkg install` 默认从 `/home/pkgrepo` 读取，而 `/etc/pkg.conf` 里的
`sourcedir` 字段可以覆盖这个默认值。

## 目录里要放什么

`pkg install <name>` 会在 source dir 下寻找两个文件：

```
<name>.manifest     # 包描述与构建规则（YAML）
<name>.tncr         # 已构建好的原生可执行（也可叫 <name>.TNCR，install 会回退识别）
```

因此本目录目前包含：

```
repo/
├─ README.md            # 本说明
├─ pkg.conf.example     # /etc/pkg.conf 的示例内容
├─ tinysh.manifest      # 与 packages/tinysh/tinysh.manifest 完全一致
└─ tinysh.tncr          # 真实构建产物（46408 字节，sha256 见 manifest）
```

## 怎么用

1. 把本 `repo/` 目录的**整个内容**复制到 TinyOS 文件系统里的
   `/home/pkgrepo`（例如通过 FTP / SMB，或打包进数据盘镜像）。
2. 在 TinyOS 里执行：

   ```
   pkg source set /home/pkgrepo
   pkg install tinysh
   ```

   第一条把 `sourcedir=/home/pkgrepo` 写入 `/etc/pkg.conf`；
   第二条读取 `tinysh.manifest` 与 `tinysh.tncr`，校验 sha256，
   写入 `/bin/tinysh.TNCR` 并登记到 `/etc/packages.d/tinysh`。

3. 验证：

   ```
   pkg list
   pkg verify tinysh
   pkg info tinysh
   ```

## 说明

- 远程 HTTP/HTTPS 软件源在 v0.1 **尚未实现**，`pkg install` 只能从本地
  source dir 安装。
- 其他包（tncr / web-admin-panel / demos）目前没有可安装产物（没有
  对应的 `.tncr`），因此没有放进本源目录；它们各自的 manifest 里
  `sha256` 仍为 `__BUILD_FILL__` 占位符，待补齐编译器/产物后再加入。
