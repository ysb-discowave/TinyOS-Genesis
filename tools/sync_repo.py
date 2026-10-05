#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sync_repo.py -- 把构建好的 TNCR 同步到 packages/repo（远程包仓库的内容源）

为什么需要它：
  `tools/build_users.py` 把用户程序交叉编译成 <TINYOS_OUT>/tncr/*.TNCR 并打进 romfs，
  但远程包仓库（Cloudflare Worker 从 GitHub raw 拉取）需要 packages/repo/ 里的
  副本 + 正确的 sha256 + INDEX.json。手工复制容易漏改 sha256，导致
  `pkg install` 校验失败。本脚本一次做完：复制 -> 重算 sha256 -> 重生成 INDEX.json。

用法：
  python tools/sync_repo.py                 # 同步 + 校验
  python tools/sync_repo.py --check         # 只校验仓库自洽性，不改文件

发布白名单（PUBLISH）刻意写死：cc_TEST / edit_TEST 这类手工测试产物
永远不会被上传，避免把临时包混进远程仓库。
"""
import os, sys, json, hashlib, re, shutil, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.join(ROOT, "packages", "repo")
OUT  = os.path.join(os.environ.get("TINYOS_OUT",
             os.path.join(os.path.expanduser("~"), "Desktop", "tinyYY")), "tncr")

# 正式发布的包（顺序即 INDEX.json 里的顺序）
PUBLISH = ["CC", "DESKTOP", "EDIT", "JVM", "LUA", "SSHD",
           "basic_demo", "count", "cpp_demo", "gui_demo", "hello",
           "minic_demo", "net_demo", "tiny_demo", "tinysh"]

DESCRIPTIONS = {
    "tinysh": "TinyOS Genesis 默认 Shell（登录后自动进入，22 条命令 + pack/run + 三级命令查找链）",
    "LUA":    "Lua 5.4.7 解释器（REPL 交互 + lua /home/x.lua 脚本模式）",
    "CC":     "C 编译器/工具链前端（tncr 交叉编译）",
    "EDIT":   "文本编辑器",
    "SSHD":   "SSH 服务端",
    "JVM":    "JVM 运行时",
    "DESKTOP": "图形文本桌面启动器",
    "hello":  "最小化 C 程序，验证 C 工具链与 TNCR 运行时",
}

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()

def patch_manifest(mpath, name, digest, size):
    """更新 manifest 里的 sha256；没有就建一个最小 manifest。"""
    if os.path.isfile(mpath):
        txt = open(mpath, encoding="utf-8").read()
        # 替换 sha256 行（保留其余字段/描述）
        new, n = re.subn(r'(?m)^sha256:\s*[0-9a-fA-F]{64}\s*$',
                         'sha256: ' + digest, txt)
        if n == 0:
            # 没有 sha256 行就插在 min_compiler 之后
            new = re.sub(r'(?m)^(min_compiler:.*)$', r'\1\nsha256: ' + digest, txt, count=1)
        # 同步描述（如果提供了）
        if name in DESCRIPTIONS:
            new = re.sub(r'(?m)^description:.*$',
                         'description: ' + DESCRIPTIONS[name].replace('\\', '\\\\'),
                         new, count=1)
        open(mpath, "w", encoding="utf-8").write(new)
    else:
        desc = DESCRIPTIONS.get(name, "%s 程序（由 tools/build_users.py 构建）" % name)
        open(mpath, "w", encoding="utf-8").write(
            "name: %s\nversion: v0.1\ndescription: %s\ndependencies: []\n"
            "min_compiler: tncr>=0.1\nsha256: %s\nbuild:\n"
            "  entry: src/%s.TNCR\n  output: /bin/%s.TNCR\n"
            "  cmd: '# 由 tools/build_users.py 交叉编译并打进 romfs'\n"
            % (name, desc, digest, name, name))

def verify():
    """校验仓库自洽：manifest 里的 sha256 == 实际 tncr 的 sha256。"""
    ok = bad = 0
    for name in PUBLISH:
        tncr = os.path.join(REPO, name + ".tncr")
        man  = os.path.join(REPO, name + ".manifest")
        if not os.path.isfile(tncr):
            print("  MISSING tncr: %s" % name); bad += 1; continue
        real = sha256_file(tncr)
        txt = open(man, encoding="utf-8").read() if os.path.isfile(man) else ""
        m = re.search(r'sha256:\s*([0-9a-fA-F]{64})', txt)
        if not m:
            print("  NO SHA256 in manifest: %s" % name); bad += 1; continue
        if m.group(1).lower() == real:
            ok += 1
        else:
            print("  MISMATCH %s: manifest=%s actual=%s" % (name, m.group(1)[:12], real[:12]))
            bad += 1
    return ok, bad

def sync_pkg_dir(name, digest, size):
    """把产物同步进按包分的源码目录（packages/<name>/），避免它变成空壳。

    只有当 packages/<name>/ 确实存在时才做 —— tncr / web-admin-panel / demos
    这类要么仍在规划中、要么是"说明包"（本就没有单一产物），不该被硬塞一个文件。
    返回 True 表示确实同步了。
    """
    pkgdir = os.path.join(ROOT, "packages", name)
    if not os.path.isdir(pkgdir):
        return False
    src = os.path.join(REPO, name + ".tncr")
    if not os.path.isfile(src):
        return False
    shutil.copyfile(src, os.path.join(pkgdir, name + ".TNCR"))
    # 该目录自己的 manifest 也要跟上真实 sha256 与产物大小写
    mp = os.path.join(pkgdir, name + ".manifest")
    if os.path.isfile(mp):
        txt = open(mp, encoding="utf-8").read()
        new = re.sub(r'(?m)^sha256:\s*[0-9a-fA-F]{64}\s*$', 'sha256: ' + digest, txt)
        if new == txt:
            new = re.sub(r'(?m)^(min_compiler:.*)$', r'\1\nsha256: ' + digest, txt, count=1)
        # manifest 里写的是 tinysh.tncr，实际产物是大写 .TNCR
        new = new.replace('output: /bin/%s.tncr' % name, 'output: /bin/%s.TNCR' % name)
        open(mp, "w", encoding="utf-8").write(new)
    return True

def main():
    check_only = "--check" in sys.argv
    if not os.path.isdir(OUT):
        print("!! TNCR output dir not found:", OUT)
        return 1
    os.makedirs(REPO, exist_ok=True)

    if check_only:
        ok, bad = verify()
        print("verify: %d ok, %d bad" % (ok, bad))
        return 0 if bad == 0 else 1

    print("syncing from:", OUT)
    npkgdir = 0
    for name in PUBLISH:
        src = os.path.join(OUT, name + ".TNCR")
        if not os.path.isfile(src):
            print("  skip %-12s (no %s.TNCR in build output)" % (name, name))
            continue
        dst = os.path.join(REPO, name + ".tncr")
        shutil.copyfile(src, dst)
        digest = sha256_file(dst)
        size = os.path.getsize(dst)
        patch_manifest(os.path.join(REPO, name + ".manifest"), name, digest, size)
        extra = ""
        if sync_pkg_dir(name, digest, size):
            extra = "  + packages/%s/%s.TNCR" % (name, name)
            npkgdir += 1
        print("  %-12s %8d bytes  sha256=%s...%s" % (name, size, digest[:16], extra))
    if npkgdir:
        print("synced %d package source dir(s) too (no longer empty shells)" % npkgdir)

    # 重生成 INDEX.json
    pkgs = []
    for name in PUBLISH:
        tncr = os.path.join(REPO, name + ".tncr")
        if os.path.isfile(tncr):
            pkgs.append({"name": name,
                         "size": os.path.getsize(tncr),
                         "sha256": sha256_file(tncr)})
    idx = {"generated": time.strftime("%Y-%m-%dT%H:%M:%S"), "packages": pkgs}
    with open(os.path.join(REPO, "INDEX.json"), "w", encoding="utf-8") as f:
        json.dump(idx, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print("wrote INDEX.json (%d packages)" % len(pkgs))

    ok, bad = verify()
    print("verify: %d ok, %d bad" % (ok, bad))
    return 0 if bad == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
