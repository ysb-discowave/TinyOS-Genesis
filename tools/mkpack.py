#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mkpack.py —— 在主机上把一个目录打成 TinyOS .pack 软件包 + manifest。

为什么需要它：TinyOS 里的 `pack` 命令只能在目标机上跑。发布一个多文件
软件包时 We'd rather在主机/CI 里确定性地生成 .pack 与配套 .manifest，
这样 packages/repo/ 里的产物可复现、可校验。

格式与 tinysh 的 cmd_pack 逐字节一致（小端，未压缩）：
    0  "TNPACK" 6B | 6 u16 ver | 8 u32 metalen | 12 u32 nfiles
    16 [元数据 key=value 行, \n 结尾] | .. 每文件: u16 namelen, name, u32 size, data

用法：
    python tools/mkpack.py <srcdir> --name NAME [--desc "..."] [--dir /opt/NAME]
                    [--prog name=/abs/path] [--env K=V] [-o out.pack]
默认输出 NAME.pack 与 NAME.manifest 到当前目录（放在 packages/repo/ 时用 -o 指定）。
"""
import os, sys, struct, hashlib, argparse

MAGIC = b"TNPACK"
VER = 1

def collect(srcdir):
    """递归收集文件，返回 [(相对名, 绝对路径, 字节内容)]，名字用 '/' 分隔。"""
    out = []
    for root, _dirs, files in os.walk(srcdir):
        for fn in files:
            ap = os.path.join(root, fn)
            rel = os.path.relpath(ap, srcdir).replace(os.sep, "/")
            with open(ap, "rb") as f:
                out.append((rel, ap, f.read()))
    out.sort(key=lambda x: x[0])
    return out

def build_pack(files, meta_lines):
    meta = ("\n".join(meta_lines) + "\n").encode("utf-8")
    metalen = len(meta)
    out = bytearray()
    out += MAGIC
    out += struct.pack("<H", VER)
    out += struct.pack("<I", metalen)
    out += struct.pack("<I", len(files))
    out += meta
    for name, _ap, data in files:
        nb = name.encode("utf-8")
        out += struct.pack("<H", len(nb))
        out += nb
        out += struct.pack("<I", len(data))
        out += data
    return bytes(out)

def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("srcdir")
    ap.add_argument("--name", required=True, help="包名")
    ap.add_argument("--desc", default="", help="描述（写进 manifest）")
    ap.add_argument("--dir", default="", help="安装目录（默认 /opt/<name>）")
    ap.add_argument("--prog", action="append", default=[], help="prog.<name>=<abs TNCR path>")
    ap.add_argument("--env", action="append", default=[], help="env.<K>=<V>")
    ap.add_argument("-o", "--out", default="", help="输出 .pack 路径（默认 <name>.pack）")
    ap.add_argument("--repo", default="", help="同时把 .pack + .manifest 写到这个目录")
    args = ap.parse_args()

    files = collect(args.srcdir)
    if not files:
        print("mkpack: no files in %s" % args.srcdir); sys.exit(1)

    installdir = args.dir or ("/opt/%s" % args.name)

    # .pack 元数据用 key=value（与 /bin/path 同风格，pack_install 按此解析）
    meta_lines = ["name=%s" % args.name, "install_dir=%s" % installdir]
    if args.desc:
        meta_lines.append("desc=%s" % args.desc)
    for p in args.prog:
        meta_lines.append("prog.%s" % p)
    for e in args.env:
        meta_lines.append("env.%s" % e)

    blob = build_pack(files, meta_lines)
    sha = hashlib.sha256(blob).hexdigest()

    out_pack = args.out or ("%s.pack" % args.name)
    with open(out_pack, "wb") as f:
        f.write(blob)

    # manifest 用 YAML（与 packages/repo 现有 manifest 同风格；pkg_get_kv 认 ':'）
    man = [
        "name: %s" % args.name,
        "version: v0.1",
        "description: %s" % (args.desc or args.name),
        "dependencies: []",
        "min_compiler: tncr>=0.1",
        "kind: pack",
        "files: %d" % len(files),
        "sha256: %s" % sha,
        "build:",
        "  entry: %s.pack" % args.name,
        "  output: %s" % installdir,
        "  cmd: '# 由 tools/mkpack.py 生成'",
    ]
    man_txt = "\n".join(man) + "\n"
    out_man = os.path.splitext(out_pack)[0] + ".manifest"
    with open(out_man, "w", encoding="utf-8") as f:
        f.write(man_txt)

    print("mkpack: %s  %d file(s), %d bytes" % (out_pack, len(files), len(blob)))
    for name, _ap, data in files:
        print("    + %-28s %d bytes" % (name, len(data)))
    print("mkpack: sha256 %s" % sha)
    print("mkpack: manifest -> %s" % out_man)

    if args.repo:
        os.makedirs(args.repo, exist_ok=True)
        import shutil
        shutil.copy(out_pack, os.path.join(args.repo, "%s.pack" % args.name))
        shutil.copy(out_man, os.path.join(args.repo, "%s.manifest" % args.name))
        print("mkpack: copied to %s" % args.repo)

if __name__ == "__main__":
    main()