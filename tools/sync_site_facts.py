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

def gen_pkgrepo_js():
    """生成 pkgcat.js 里的真实包数组（取自 packages/repo 的实际文件）。"""
    import json
    rows = real_pkg_list()
    # 从各包 manifest 取描述
    desc = {}
    for name, size, sha in rows:
        mp = os.path.join(ROOT, "packages", "repo", name + ".manifest")
        d = ""
        if os.path.isfile(mp):
            t = open(mp, encoding="utf-8").read()
            m = re.search(r'(?m)^description:\s*(.+)$', t)
            if m: d = m.group(1).strip()
        if not d:
            d = "%s 程序（由 tools/build_users.py 构建）" % name
        desc[name] = d
    out = ["["]
    for name, size, sha in rows:
        d = desc[name].replace("\\", "\\\\").replace("'", "\\'")
        out.append("    { id: '%s', state: 'ready', size: %d," % (name, size))
        out.append("      sha: '%s'," % sha)
        out.append("      descZh: '%s'," % d)
        out.append("      descEn: '%s (%d bytes, sha256 %s...)' }," % (d, size, sha[:12]))
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
