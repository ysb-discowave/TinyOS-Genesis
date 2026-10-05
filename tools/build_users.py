#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=============================================================================
 build_users.py -- build TinyOS user programs and pack them into the kernel romfs
=============================================================================
Does three things:
  1) uses tools/compiler/tcc.py to compile the multi-language samples in
     examples/ into .TNCR executables
  2) generates the desktop launcher DESKTOP.TNCR
  3) packs everything into a romfs image and writes kernel/romfs.c
     (the kernel mounts it at boot)

Usage: python tools/build_users.py [--verbose]
=============================================================================
"""
import os
import struct
import sys
import shutil
import hashlib
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EX_SRC = os.path.join(HERE, "compiler", "examples")

# 构建产物（编译好的 .TNCR）一律输出到桌面的 tinyYY\tncr，
# 不往项目目录 / 系统目录里塞二进制。可用 TINYOS_OUT 覆盖。
TINYOS_OUT = os.environ.get(
    "TINYOS_OUT", os.path.join(os.path.expanduser("~"), "Desktop", "tinyYY"))
EX_OUT = os.path.join(TINYOS_OUT, "tncr")
sys.path.insert(0, os.path.join(HERE, "compiler"))

import tcc  # noqa: E402

# (source file, target name, language, extra args)
PROGRAMS = [
    ("hello.c",     "hello",      "c",     []),
    ("count.c",     "count",      "c",     []),
    ("gui_demo.c",  "gui_demo",   "c",     ["--gui"]),
    ("net_demo.c",  "net_demo",   "c",     []),
    ("hello.tiny",  "tiny_demo",  "tiny",  []),
    ("loop.bas",    "basic_demo", "basic", []),
    ("calc.minic",  "minic_demo", "minic", []),
    ("hello.cpp",   "cpp_demo",   "cpp",   []),
    # SSHD.TNCR: a real SSH-2 server (curve25519-sha256 + ed25519 +
    # aes128-ctr + hmac-sha2-256).  The installer offers it as an option;
    # without it, `sshd` tells the user to re-run the installer with --with-ssh.
    ("sshd.c",      "SSHD",       "c",     []),
    # 全屏文本编辑器：`edit <file>`
    ("edit.c",      "EDIT",       "c",     []),
    # 系统自带的 MiniC 编译器：`cc <src.mc> [-o out.TNCR]`
    ("cc.c",        "CC",         "c",     []),
]

# NOTE: these two files are embedded in the romfs and shown on the VGA text
# console via `cat`. The console is 80x25 CP437 text mode, so every line here
# must stay pure ASCII and under 80 columns -- otherwise it garbles/truncates.
README_TXT = """TinyOS in-memory file system
============================
This is a text file stored at /home/readme.txt.

TinyOS is a 32-bit x86 bare-metal operating system:
  * own boot sector OR Multiboot -- both boot paths supported
  * custom GDT/IDT/PIC/PIT, PS/2 keyboard and mouse, VGA text console
  * in-memory virtual file system (VFS) + romfs (romfs loads this file at boot)
  * TNCR native executable format, produced by tcc from
    C / C++ / TinyLang / BASIC / MiniC
  * custom e1000 NIC driver + ARP/IPv4/ICMP/UDP/TCP stack
  * FTP server/client and TinySMB server/client (file up/download)
  * optional desktop engine: run /DESKTOP.TNCR to enter the desktop

You can download this file to another machine over FTP:
  TinyOS> net start ftp
  then on the host: ftp 127.0.0.1 2121   (QEMU port forwarding)
"""

NOTES_TXT = """TinyOS command reference
========================
help                     list commands
ls / cd / pwd / cat      file system
run /bin/hello.TNCR      run a user program
desktop                  enter the graphical desktop
net info                 show NIC state and ARP table
net ping 10.0.2.2        send an ICMP echo
net start ftp            start the local FTP server
net get ftp <host> <port> <user> <pass> <remote> <local>
net put ftp <host> <port> <user> <pass> <local> <remote>
net get smb <host> <port> <share> <user> <pass> <remote> <local>
net put smb <host> <port> <share> <user> <pass> <local> <remote>
ps / mem / uptime        system information
"""


def build_romfs(entries):
    """entries: [(vfs_path, bytes)] -> romfs byte string

    2.0 记录格式（比 1.0 多 5 字节：flag + orig_len，用于 TNCR 压缩）：
        [u32 namelen][name][u8 flag][u32 orig_len][u32 stored_len][data]
    flag = 0：data 即原文件（orig_len == stored_len）
    flag = 1：data 为 zlib raw-deflate 流（compressobj, wbits=-15），
              orig_len = 解压后长度；压缩后反而更大时自动退回 flag=0。
    结束标记 namelen = 0。"""
    import zlib
    out = bytearray()
    saved_total = 0
    for path, data in entries:
        nb = path.encode("utf-8")
        if path.endswith(".TNCR"):
            co = zlib.compressobj(9, zlib.DEFLATED, -15)
            comp = co.compress(data) + co.flush()
            if len(comp) < len(data):
                flag, stored, orig = 1, comp, len(data)
                saved_total += len(data) - len(comp)
            else:
                flag, stored, orig = 0, data, len(data)
        else:
            flag, stored, orig = 0, data, len(data)
        out += struct.pack("<I", len(nb)) + nb
        out += struct.pack("<B I I", flag, orig, len(stored)) + stored
    out += struct.pack("<I", 0)
    print("  compression saved %d bytes on TNCR entries" % saved_total)
    return bytes(out)


# ---------------------------------------------------------------------------
# User database baked into the romfs baseline
# ---------------------------------------------------------------------------
# Password hashing must match kernel/user.c exactly:
#     hash = sha256hex(salt || password)   with salt = 16 hex chars
# Salts are fixed here so the romfs image is byte-reproducible.  The kernel
# treats these files as the *baseline*; once a disk is present, the on-disk
# copies win, so `passwd` changes survive reboots.
ACCOUNTS = [
    # name,    uid,  gid,  home,           password, salt
    ("root",    0,    0,    "/root",        "root",  "5f3a91c2d4e60718"),
    ("guest",   1000, 1000, "/home/guest",  "guest", "9b1e44a70c2d8f36"),
]


def build_user_db():
    pw = ["# TinyOS user database: name:uid:gid:home"]
    sh = ["# TinyOS shadow: name:salt:sha256(salt+password)"]
    for name, uid, gid, home, password, salt in ACCOUNTS:
        pw.append("%s:%d:%d:%s" % (name, uid, gid, home))
        h = hashlib.sha256((salt + password).encode("utf-8")).hexdigest()
        sh.append("%s:%s:%s" % (name, salt, h))
    return ("\n".join(pw) + "\n"), ("\n".join(sh) + "\n")


def emit_c_array(blob, out_path):
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("/* AUTO-GENERATED by tools/build_users.py -- do not edit by hand. */\n")
        f.write("#include \"types.h\"\n\n")
        f.write("const u8 romfs_data[] = {\n")
        for i in range(0, len(blob), 16):
            chunk = blob[i:i + 16]
            f.write("    " + ",".join("0x%02X" % b for b in chunk) + ",\n")
        f.write("    0x00\n};\n")
        f.write("const u32 romfs_len = %du;\n" % len(blob))


def fallback_stub():
    """No toolchain: write an empty romfs placeholder so the kernel still builds."""
    out = os.path.join(ROOT, "src", "kernel", "romfs.c")
    with open(out, "w", encoding="utf-8") as f:
        f.write("/* placeholder romfs (tools/build_users.py was not run). */\n")
        f.write("#include \"types.h\"\n")
        f.write("const u8 romfs_data[] = { 0, 0, 0, 0 };\n")
        f.write("const u32 romfs_len = 4;\n")
    print("  wrote empty romfs placeholder: kernel/romfs.c")


def main():
    verbose = "--verbose" in sys.argv
    os.makedirs(EX_OUT, exist_ok=True)

    kind, tool = tcc.find_toolchain()
    if kind is None:
        print("!! no compiler found (zig / clang); skipping user programs, writing empty romfs.")
        print("   install one, then re-run: python tools/build_users.py")
        fallback_stub()
        return 1

    print("== TinyOS user program build ==")
    print("  toolchain: %s (%s)" % (kind, tool))
    os.makedirs(EX_OUT, exist_ok=True)
    print("  output   : %s" % EX_OUT)

    entries = []
    ok = 0
    for src, name, lang, extra in PROGRAMS:
        sp = os.path.join(EX_SRC, src)
        op = os.path.join(EX_OUT, name + ".TNCR")
        if not os.path.exists(sp):
            print("  skipped (source missing): %s" % src)
            continue
        try:
            tcc.build(sp, op, lang, 0, verbose=verbose)
            entries.append(("/bin/%s.TNCR" % name, open(op, "rb").read()))
            ok += 1
        except SystemExit as e:
            print("  build failed: %s (%s)" % (src, e))

    # desktop launcher
    print("== generating desktop launcher ==")
    dpath = os.path.join(EX_OUT, "DESKTOP.TNCR")
    tcc.build("desktop", dpath, None, tcc.TNCR_F_DESKTOP, verbose=verbose)
    entries.append(("/DESKTOP.TNCR", open(dpath, "rb").read()))

    # ---- Lua 5.4 解释器（tools/lua 交叉编译成 LUA.TNCR）----
    # 裸机 Lua：core + base/coroutine/string/table/os/debug/utf8/io/math 库，
    # 用裸机 libc shim 编译。`lua` 进 REPL；`lua /home/x.lua` 跑脚本。
    lua_build = os.path.join(HERE, "lua", "build_lua.py")
    lua_tncr = os.path.join(EX_OUT, "LUA.TNCR")
    print("== building Lua interpreter ==")
    lua_ok = False
    if os.path.isfile(lua_build):
        try:
            env = dict(os.environ, TINYOS_OUT=TINYOS_OUT)
            r = subprocess.run(
                [sys.executable, lua_build],
                capture_output=True, text=True, env=env)
            if r.returncode == 0 and os.path.isfile(lua_tncr):
                lua_ok = True
                entries.append(("/bin/LUA.TNCR", open(lua_tncr, "rb").read()))
                print("  LUA.TNCR embedded in romfs (%d bytes)"
                      % os.path.getsize(lua_tncr))
            else:
                print("  Lua build failed (rc=%d):" % r.returncode)
                print("  " + (r.stderr.strip().splitlines() or ["<no stderr>"])[-1])
        except Exception as e:
            print("  Lua build error: %s" % e)
    else:
        print("  build_lua.py not found, skipping Lua")
    if not lua_ok:
        print("  Lua NOT embedded (the `lua` command will be absent)")

    # ---- tinysh 用户态 shell（tools/tinysh 交叉编译成 tinysh.TNCR）----
    # Genesis v0.1 的参考 shell：19 条命令，全部经 tinyos_api_t 的
    # Genesis 扩展字段落地（fs_*/proc_*/hw_*/sys_*）。登录后自动启动。
    tsh_build = os.path.join(HERE, "tinysh", "build_tinysh.py")
    tsh_tncr = os.path.join(EX_OUT, "tinysh.TNCR")
    print("== building tinysh ==")
    tsh_ok = False
    if os.path.isfile(tsh_build):
        try:
            env = dict(os.environ, TINYOS_OUT=TINYOS_OUT)
            r = subprocess.run(
                [sys.executable, tsh_build],
                capture_output=True, text=True, env=env)
            if r.returncode == 0 and os.path.isfile(tsh_tncr):
                tsh_ok = True
                entries.append(("/bin/tinysh.TNCR", open(tsh_tncr, "rb").read()))
                print("  tinysh.TNCR embedded in romfs (%d bytes)"
                      % os.path.getsize(tsh_tncr))
            else:
                print("  tinysh build failed (rc=%d):" % r.returncode)
                print("  " + (r.stderr.strip().splitlines() or ["<no stderr>"])[-1])
        except Exception as e:
            print("  tinysh build error: %s" % e)
    else:
        print("  build_tinysh.py not found, skipping tinysh")
    if not tsh_ok:
        print("  tinysh NOT embedded (the `tinysh` command will be absent)")

    # ---- 可选组件：JVM 运行时（TinyOS-JVM 项目）----
    # 与 TinyOS-os 平级的独立项目（零修改 TinyOS-os 的构建逻辑）：
    # 检测到它（且能跑通 build.py）就生成 build/JVM.TNCR 打进 romfs；
    # 安装向导的组件 [7] 据此决定是否把 /bin/JVM.TNCR 写盘。
    # 目录不存在 / 构建失败都不致命：只是向导里这一项显示 "not present"。
    jvm_root = os.environ.get("TINYOS_JVM") or os.path.join(
        os.path.dirname(ROOT), "TinyOS-JVM")
    if os.path.isfile(os.path.join(jvm_root, "tools", "build.py")):
        print("== building JVM runtime (TinyOS-JVM) ==")
        jvm_tncr = os.path.join(jvm_root, "build", "Hello.TNCR")
        ok = False
        try:
            r = subprocess.run(
                [sys.executable, os.path.join(jvm_root, "tools", "build.py")],
                capture_output=True, text=True)
            if r.returncode == 0:
                ok = os.path.isfile(jvm_tncr)
                if not ok:
                    print("  JVM build.py exited 0 but %s missing" % jvm_tncr)
            else:
                print("  JVM build failed (rc=%d):" % r.returncode)
                print("  " + (r.stderr.strip().splitlines() or ["<no stderr>"])[-1])
            if r.returncode != 0 and r.stdout.strip():
                print("  " + r.stdout.strip().splitlines()[-1])
        except Exception as e:
            print("  JVM build error: %s" % e)
        if ok:
            jpath = os.path.join(EX_OUT, "JVM.TNCR")
            shutil.copyfile(jvm_tncr, jpath)
            entries.append(("/bin/JVM.TNCR", open(jpath, "rb").read()))
            print("  JVM.TNCR embedded in romfs (%d bytes)"
                  % os.path.getsize(jpath))
        else:
            print("  JVM runtime NOT embedded (component [7] will show 'not present')")
    else:
        print("== JVM runtime: TinyOS-JVM not found, skipping ==")
        print("   (put it at %s or set TINYOS_JVM)" % jvm_root)

    # text files
    entries.append(("/home/readme.txt", README_TXT.encode("utf-8")))
    entries.append(("/home/notes.txt", NOTES_TXT.encode("utf-8")))

    # MiniC 示例源码：系统自带，可直接 `cc /home/hello.mc -o /bin/hello.TNCR`
    demo_mc = os.path.join(EX_SRC, "hello.mc")
    if os.path.exists(demo_mc):
        entries.append(("/home/hello.mc", open(demo_mc, "rb").read()))

    # user database baseline (/etc/passwd + /etc/shadow, salted SHA-256)
    pw_txt, sh_txt = build_user_db()
    entries.append(("/etc/passwd", pw_txt.encode("utf-8")))
    entries.append(("/etc/shadow", sh_txt.encode("utf-8")))

    # pack
    blob = build_romfs(entries)
    emit_c_array(blob, os.path.join(ROOT, "src", "kernel", "romfs.c"))
    print("== romfs ==")
    print("  %d entries, %d bytes -> kernel/romfs.c" % (len(entries), len(blob)))
    for path, data in entries:
        print("    %-28s %6d bytes" % (path, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
