#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sync_site_facts.py -- 把官网里写死的数字同步成仓库真实值。

官网是纯静态 HTML/JS，数字是手写死的，代码一改就过时。
本脚本按真实产物（packages/repo/*.tncr）与源码（tinysh 命令表）重算并回填。

用法：python tools/sync_site_facts.py [--check]
"""
import os, re, sys, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---- 真实值来源 ----
def real_pkg(name):
    p = os.path.join(ROOT, "packages", "repo", name + ".tncr")
    if not os.path.isfile(p):
        return None, None
    d = open(p, "rb").read()
    return len(d), hashlib.sha256(d).hexdigest()

def real_cmd_count():
    s = open(os.path.join(ROOT, "tools", "tinysh", "src", "tinysh.c"), encoding="utf-8").read()
    m = re.search(r"static const cmd_t g_cmds\[\] = \{(.*?)\n\};", s, re.S)
    if not m:
        return None, None
    names = re.findall(r'\{"(\w+)",', m.group(1))
    return len(names), names

def real_pkg_count():
    d = os.path.join(ROOT, "packages", "repo")
    if not os.path.isdir(d):
        return 0
    return len([f for f in os.listdir(d) if f.endswith(".tncr")])

SIZE, SHA = real_pkg("tinysh")
NCMD, NAMES = real_cmd_count()
NPKG = real_pkg_count()
OLD_SHA = "a35a67bc938e91434a3c0e2ac86688e14d95ce948f3e2a7eb6442be7fdb02bda"

print("real tinysh: %s bytes, %d cmds, sha %s..." % (SIZE, NCMD, SHA[:12]))
def real_pkg_list():
    """按 PUBLISH 顺序返回 [(name, size, sha256hex)]，sha256 取自 manifest 声明值。"""
    import json
    idx = os.path.join(ROOT, "packages", "repo", "INDEX.json")
    if os.path.isfile(idx):
        try:
            d = json.load(open(idx, encoding="utf-8"))
            return [(p["name"], p["size"], p["sha256"]) for p in d.get("packages", [])]
        except Exception:
            pass
    out = []
    for f in sorted(os.listdir(os.path.join(ROOT, "packages", "repo"))):
        if f.endswith(".tncr"):
            size, sha = real_pkg(f[:-5])
            out.append((f[:-5], size, sha))
    return out

def gen_pkgtable():
    """生成 packages.html 里的真实包清单（ASCII，纯文本 code block）。"""
    rows = real_pkg_list()
    lines = []
    lines.append("packages/repo/  --  %d packages, all with real artifacts" % len(rows))
    lines.append("")
    lines.append("%-12s  %9s  %-16s  %s" % ("NAME", "SIZE", "SHA256 (head)", "MANIFEST"))
    lines.append("%-12s  %9s  %-16s  %s" % ("-" * 12, "-" * 9, "-" * 16, "-" * 28))
    for name, size, sha in rows:
        lines.append("%-12s  %9d  %-16s  %s" % (name, size, sha[:16] + "...", name + ".manifest"))
    lines.append("")
    total = sum(r[1] for r in rows)
    lines.append("total: %d packages, %d bytes (%.1f KB)" % (len(rows), total, total / 1024.0))
    return "\n".join(lines)

def inject_pkgtable():
    """把生成的清单写进 packages.html 的 PKGTABLE 标记之间。"""
    p = os.path.join(ROOT, "packages.html")
    if not os.path.isfile(p):
        return 0
    s = open(p, encoding="utf-8").read()
    BEG, END = "<!-- PKGTABLE:BEGIN", "<!-- PKGTABLE:END"
    if BEG not in s or END not in s:
        print("  packages.html: PKGTABLE markers not found, skipped")
        return 0
    i = s.index(BEG); j = s.index(END)
    block = s[i:j]
    # 保留 BEGIN 注释尾部到第一个 -->，重建中间内容
    head_end = block.index("-->") + 3
    head = block[:head_end]
    table = gen_pkgtable()
    # HTML 转义
    table = table.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    new = head + " （由 tools/sync_site_facts.py 自动生成，勿手改） -->\n        <div class=\"code-block\">\n          <pre aria-label=\"仓库包清单\"><code>" + table + "</code></pre>\n        </div>\n        "
    s = s[:i] + new + s[j:]
    open(p, "w", encoding="utf-8").write(s)
    return 1

def count_lines(path):
    """真实行数；文件不存在返回 None。"""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return sum(1 for _ in f)
    except OSError:
        return None

# 逐包元数据：用途 / 运行方式 / 真实源文件（用于生成"文件清单"与真实行数）
PKG_META = {
    "tinysh": dict(
        zh="系统默认 Shell，登录后自动进入。22 条内置命令，外加一条三级命令查找链：内置 → 内核命令（45 个）→ /bin 下的程序。",
        en="The default shell, started automatically after login. 22 built-in commands plus a three-level lookup chain: builtins, then the 45 kernel commands, then programs under /bin.",
        usageZh="直接在提示符敲命令；help 列出全部命令，exit 退回内核 shell。",
        usageEn="Just type commands; help lists everything, exit returns to the kernel shell.",
        sources=["tools/tinysh/src/tinysh.c", "tools/tinysh/src/kernel_api_tinyos.c",
                 "tools/tinysh/src/kernel_api_host.c", "tools/tinysh/src/tinysh.h",
                 "tools/tinysh/src/kernel_api.h"]),
    "LUA": dict(
        zh="Lua 5.4.7 解释器，编译成 TNCR 跑在用户态。支持 REPL 交互与脚本模式。",
        en="The Lua 5.4.7 interpreter, compiled to TNCR and running in user space. Both an interactive REPL and script mode.",
        usageZh="敲 lua 进 REPL（:quit 退出）；lua /home/x.lua 直接跑脚本。",
        usageEn="Run lua for the REPL (:quit to leave) or lua /home/x.lua to run a script.",
        sources=["tools/lua/lua_main.c", "tools/lua/lua_package.c", "tools/lua/lua_shim.c"]),
    "CC": dict(
        zh="C 编译器 / 工具链前端（tncr 交叉编译）。把 C 源码编译成 TinyOS 原生可执行文件。",
        en="The C compiler / toolchain front end (tncr cross-compiler). Compiles C sources into native TinyOS executables.",
        usageZh="安装后在 tinysh 里敲 cc 使用。",
        usageEn="Once installed, invoke it as cc from tinysh.",
        sources=["tools/compiler/examples/cc.c", "tools/compiler/tcc.py"]),
    "EDIT": dict(
        zh="文本编辑器，运行在 VGA 文本模式与 PS/2 键盘之上。",
        en="A text editor running on the VGA text mode with a PS/2 keyboard.",
        usageZh="安装后敲 edit <文件> 打开。",
        usageEn="Once installed, run edit <file>.",
        sources=["tools/compiler/examples/edit.c"]),
    "SSHD": dict(
        zh="SSH-2 服务端（curve25519-sha256 + ed25519 主机密钥），让外部机器能登进 TinyOS。",
        en="An SSH-2 server (curve25519-sha256 with an ed25519 host key) that lets an outside machine log into TinyOS.",
        usageZh="安装后由 tinysh 启动；与内核的 passwd 账户体系配合。",
        usageEn="Started from tinysh once installed; it works with the kernel passwd account system.",
        sources=["tools/compiler/examples/sshd.c"]),
    "SSHCLIENT": dict(
        zh="SSH-2 客户端：TinyOS 主动连外部 SSH 服务器。传输层与服务端同构（curve25519-sha256 ECDH、AES-128-CTR、HMAC-SHA2-256），走 ssh-userauth 的 password 认证。内核无 pty，因此不申请伪终端，只能用 exec 跑命令或简易逐行 shell。",
        en="An SSH-2 client: TinyOS dials out to an external SSH server. The transport mirrors the server side (curve25519-sha256 ECDH, AES-128-CTR, HMAC-SHA2-256) and authenticates via ssh-userauth password. The kernel has no pty, so it never requests a pseudo-terminal and can only run commands via exec or a simple line-based shell.",
        usageZh="SSHCLIENT <ip> [port] [command...] —— 给了 command 就跑完退出，不给就进简易交互 shell。主机只接受点分十进制 IPv4（内核无 DNS）。",
        usageEn="SSHCLIENT <ip> [port] [command...] — with a command it runs and exits; without one it drops into a simple interactive shell. Hosts must be dotted-quad IPv4 (the kernel has no DNS).",
        sources=["tools/compiler/examples/ssh_client.c", "tools/compiler/sshd_crypto.h"]),
    "JVM": dict(
        zh="JVM 运行时。产物由外部 TinyOS-JVM 项目的 build.py 生成，再打进 romfs。",
        en="A JVM runtime. The artifact is produced by the external TinyOS-JVM project's build.py and then embedded into romfs.",
        usageZh="由安装向导的可选组件 [7] 决定是否写盘。",
        usageEn="Whether it is written to disk is decided by optional component [7] of the install wizard.",
        sources=[]),
    "DESKTOP": dict(
        zh="图形文本桌面启动器。由 tcc 在构建时生成（带 DESKTOP 标志），进入基于 VGA 文本模式与 PS/2 键鼠的桌面。",
        en="The graphical text-mode desktop launcher. Generated by tcc at build time (with the DESKTOP flag); enters a desktop built on VGA text mode with PS/2 keyboard and mouse.",
        usageZh="在 tinysh 里敲 run DESKTOP，或直接运行 /DESKTOP.TNCR。",
        usageEn="Type run DESKTOP in tinysh, or execute /DESKTOP.TNCR directly.",
        sources=[]),
    "hello": dict(
        zh="最小化 C 程序，用来验证 C 工具链与 TNCR 运行时是否正常。",
        en="A minimal C program used to verify the C toolchain and the TNCR runtime.",
        usageZh="敲 hello 运行，应打印一行问候。",
        usageEn="Run hello; it should print a greeting.",
        sources=["tools/compiler/examples/hello.c"]),
    "count": dict(
        zh="计数演示程序，展示用户态程序的运行与输出。",
        en="A counting demo showing a user-space program running and printing.",
        usageZh="敲 count 运行。",
        usageEn="Run count.",
        sources=["tools/compiler/examples/count.c"]),
    "gui_demo": dict(
        zh="GUI 演示：带 --gui 标志构建，演示 TNCR 的 GUI 标志位。",
        en="A GUI demo built with the --gui flag, showing the TNCR GUI flag bit.",
        usageZh="敲 gui_demo 运行。",
        usageEn="Run gui_demo.",
        sources=["tools/compiler/examples/gui_demo.c"]),
    "net_demo": dict(
        zh="网络演示：展示用户态程序如何做网络 I/O。",
        en="A networking demo showing how a user-space program does network I/O.",
        usageZh="敲 net_demo 运行。",
        usageEn="Run net_demo.",
        sources=["tools/compiler/examples/net_demo.c"]),
    "tiny_demo": dict(
        zh=".tiny 语言的演示程序，由 tcc 的 tiny 前端编译。",
        en="A demo for the .tiny language, compiled by tcc's tiny front end.",
        usageZh="敲 tiny_demo 运行。",
        usageEn="Run tiny_demo.",
        sources=["tools/compiler/examples/hello.tiny"]),
    "basic_demo": dict(
        zh=".bas 语言的演示程序，由 tcc 的 basic 前端编译。",
        en="A demo for the .bas language, compiled by tcc's basic front end.",
        usageZh="敲 basic_demo 运行。",
        usageEn="Run basic_demo.",
        sources=["tools/compiler/examples/loop.bas"]),
    "minic_demo": dict(
        zh=".minic 语言的演示程序，由 tcc 的 minic 前端编译。",
        en="A demo for the .minic language, compiled by tcc's minic front end.",
        usageZh="敲 minic_demo 运行。",
        usageEn="Run minic_demo.",
        sources=["tools/compiler/examples/calc.minic"]),
    "cpp_demo": dict(
        zh="C++ 演示程序，验证 tcc 的 C++ 前端（带 TNCR_F_GUI 判定）。",
        en="A C++ demo exercising tcc's C++ front end (with the TNCR_F_GUI check).",
        usageZh="敲 cpp_demo 运行。",
        usageEn="Run cpp_demo.",
        sources=["tools/compiler/examples/hello.cpp"]),
}

def jesc(s):
    """转义成 JS 单引号字符串内容。"""
    return s.replace("\\", "\\\\").replace("'", "\\'").replace("\n", " ")

def gen_pkgrepo_js():
    """生成 pkgcat.js 的真实包数组（取自 packages/repo 的实际文件）。

    每个条目带齐 renderDetail 需要的全部字段：dir / out / size / sha /
    detailZh / detailEn / files（含真实行数），否则点开会是空面板。
    """
    rows = real_pkg_list()
    out = ["["]
    for name, size, sha in rows:
        meta = PKG_META.get(name)
        if meta is None:
            zh = "%s 程序（由 tools/build_users.py 构建）" % name
            en = "The %s program, built by tools/build_users.py." % name
            usage_zh = "安装后直接在 tinysh 里敲 %s 运行。" % name
            usage_en = "Once installed, run %s from tinysh." % name
            srcs = []
        else:
            zh, en = meta["zh"], meta["en"]
            usage_zh, usage_en = meta["usageZh"], meta["usageEn"]
            srcs = meta["sources"]

        files = [(name + ".tncr", None,
                  "已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件"),
                 (name + ".manifest",
                  count_lines(os.path.join(ROOT, "packages", "repo", name + ".manifest")),
                  "包描述：版本、依赖、min_compiler 与 sha256")]
        for sp in srcs:
            full = os.path.join(ROOT, sp)
            if os.path.isfile(full):
                files.append((sp, count_lines(full), "构建时使用的真实源文件"))
            else:
                files.append((sp, None, "源文件（不在本仓库，见上方说明）"))

        dzh = [zh,
               "安装：pkg install %s —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，"
               "下载后用 sha256 校验，通过则写入 /bin/%s.TNCR。" % (name, name),
               "用法：%s" % usage_zh,
               "产物 %d 字节，sha256 前 16 位 %s…；完整清单见上方表格与 packages/repo/INDEX.json。"
               % (size, sha[:16])]
        den = [en,
               "Install: pkg install %s — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/%s.TNCR."
               % (name, name),
               "Usage: %s" % usage_en,
               "Artifact: %d bytes, sha256 starts with %s…; the full table is above and in packages/repo/INDEX.json."
               % (size, sha[:16])]

        out.append("    { id: '%s', state: 'ready', dir: 'packages/repo/'," % name)
        out.append("      out: '/bin/%s.TNCR', size: %d," % (name, size))
        out.append("      sha: '%s'," % sha)
        out.append("      descZh: '%s', descEn: '%s'," % (jesc(zh), jesc(en)))
        out.append("      detailZh: [%s]," % ", ".join("'%s'" % jesc(x) for x in dzh))
        out.append("      detailEn: [%s]," % ", ".join("'%s'" % jesc(x) for x in den))
        out.append("      files: [")
        for i, (fn, ln, note) in enumerate(files):
            comma = "," if i < len(files) - 1 else ""
            out.append("        { n: '%s', l: %s, note: '%s' }%s"
                       % (fn, "null" if ln is None else ln, jesc(note), comma))
        out.append("      ] },")
    out.append("  ]")
    return "\n    ".join(out)

def inject_pkgrepo():
    """把真实包数组写进 pkgcat.js 的 PKGREPO 标记之间。"""
    p = os.path.join(ROOT, "pkgcat.js")
    if not os.path.isfile(p):
        return 0
    s = open(p, encoding="utf-8").read()
    BEG, END = "/* PKGREPO:BEGIN", "/* PKGREPO:END"
    if BEG not in s or END not in s:
        print("  pkgcat.js: PKGREPO markers not found, skipped")
        return 0
    i = s.index(BEG); j = s.index(END)
    block = s[i:j]
    head_end = block.index("*/") + 2
    head = block[:head_end]
    new = head + "\n  var PKGS_REPO = " + gen_pkgrepo_js() + ";\n  "
    s = s[:i] + new + s[j:]
    open(p, "w", encoding="utf-8").write(s)
    return 1

print("real packages in repo: %d" % NPKG)

CMD_LIST = " ".join(NAMES)
changes = []

def patch(fname, pairs):
    """pairs: list of (old, new) literal replacements"""
    p = os.path.join(ROOT, fname)
    if not os.path.isfile(p):
        print("  skip %s (not found)" % fname)
        return
    s = open(p, encoding="utf-8").read()
    orig = s
    n = 0
    for old, new in pairs:
        c = s.count(old)
        if c:
            s = s.replace(old, new)
            n += c
            changes.append((fname, old[:48], new[:48], c))
    if s != orig:
        open(p, "w", encoding="utf-8").write(s)
        print("  %-16s %d replacement(s)" % (fname, n))
    else:
        print("  %-16s no change" % fname)

SIZE_S = str(SIZE)
OLD_SIZE_S = "104144"

# ---- index.html ----
patch("index.html", [
    ("19 条命令的用户态 shell", "%d 条命令的用户态 shell" % NCMD),
    ("The 19-command user-space shell", "The %d-command user-space shell" % NCMD),
    ("19 条命令：help version sysinfo date ls cd pwd cat mkdir rm touch ps kill devlist readdev writedev echo clear exit。",
     "%d 条命令：%s。" % (NCMD, CMD_LIST)),
    ("19 commands: help version sysinfo date ls cd pwd cat mkdir rm touch ps kill devlist readdev writedev echo clear exit.",
     "%d commands: %s." % (NCMD, CMD_LIST)),
    ("tinysh 用户态 shell，19 条命令", "tinysh 用户态 shell，%d 条命令" % NCMD),
    ("tinysh user-space shell, 19 commands", "tinysh user-space shell, %d commands" % NCMD),
])

# ---- tinysh.html ----
patch("tinysh.html", [
    ("覆盖 19 条命令", "覆盖 %d 条命令" % NCMD),
    ("the real 19 commands", "the real %d commands" % NCMD),
    ("真实的 19 条命令", "真实的 %d 条命令" % NCMD),
    ("tinysh 的 19 条命令由", "tinysh 的 %d 条命令由" % NCMD),
    ("tinysh's 19 commands are covered", "tinysh's %d commands are covered" % NCMD),
    ("/bin/tinysh.TNCR · 104144 字节", "/bin/tinysh.TNCR · %s 字节" % SIZE_S),
    ("/bin/tinysh.TNCR · 104144 bytes", "/bin/tinysh.TNCR · %s bytes" % SIZE_S),
    ("'tinysh.TNCR': makeFile(104144)", "'tinysh.TNCR': makeFile(%s)" % SIZE_S),
])

# ---- filesystem.html ----
patch("filesystem.html", [
    (">%s<" % OLD_SIZE_S, ">%s<" % SIZE_S),
])

# ---- packages.html ----
patch("packages.html", [
    ("%s" % OLD_SIZE_S, SIZE_S),
])

# ---- pkgcat.js ----
patch("pkgcat.js", [
    ("size: %s," % OLD_SIZE_S, "size: %s," % SIZE_S),
    ("sha: '%s'" % OLD_SHA, "sha: '%s'" % SHA),
    ("命令分发与 20 条命令的实现（含 pkg）", "命令分发与 %d 条命令的实现（含 pkg / pack / run）" % NCMD),
    ("20 条内置命令", "%d 条内置命令" % NCMD),
    ("20 built-in commands", "%d built-in commands" % NCMD),
    ("tinysh 内置 21 个命令", "tinysh 内置 %d 个命令" % NCMD),
    ("tinysh has 21 built-ins", "tinysh has %d built-ins" % NCMD),
])

print("\n%d change group(s):" % len(changes))
for f, o, n, c in changes:
    print("  %-15s x%d  %r -> %r" % (f, c, o, n))

check_only = "--check" in sys.argv
if not check_only:
    if inject_pkgtable():
        print("  packages.html   PKGTABLE regenerated from packages/repo")
    if inject_pkgrepo():
        print("  pkgcat.js       PKGREPO array regenerated from packages/repo")
