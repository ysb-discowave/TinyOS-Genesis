#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_tinysh.py -- 把 tinysh 交叉编译成 tinysh.TNCR

镜像 tools/lua/build_lua.py 的构建方式：
  * zig 交叉编译 freestanding ELF（绝不带 -mno-80387，否则 x87 死机）
  * 裸机 libc 由 tools/lua/lua_shim.c 提供（printf/fwrite/malloc/字符串…）
  * 链接 user.ld 到 0x01000000，入口 user_main
  * elf32_flatten + pack_tncr 打成 TNCR

产物：<TINYOS_OUT>/tncr/tinysh.TNCR
"""
import os, sys, subprocess, tempfile, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ZIG  = os.path.join(os.path.expanduser("~"), "Desktop", "tinyYY",
                    "toolchain", "zig-windows-x86_64-0.14.0", "zig.exe")
SRC  = os.path.join(ROOT, "tools", "tinysh", "src")
SHIMINC = os.path.join(ROOT, "tools", "lua", "shiminc")
LUASHIM = os.path.join(ROOT, "tools", "lua", "lua_shim.c")
OUT  = os.path.join(os.environ.get("TINYOS_OUT",
             os.path.join(os.path.expanduser("~"), "Desktop", "tinyYY")), "tncr", "tinysh.TNCR")

sys.path.insert(0, os.path.join(ROOT, "tools", "compiler"))
import tcc

LOCAL_C = ["tinysh.c", "kernel_api_tinyos.c"]

def build():
    if not os.path.isfile(ZIG):
        print("!! zig not found at", ZIG); return 1
    tmp = tempfile.mkdtemp(prefix="tinysht_")
    try:
        sources = []
        for f in LOCAL_C:
            p = os.path.join(SRC, f)
            if not os.path.isfile(p):
                print("!! missing:", f); return 1
            sources.append(p)
        sources.append(LUASHIM)

        elf = os.path.join(tmp, "tinysh.elf")
        cc = [ZIG, "cc",
            "-target", "x86-freestanding",
            "-mcpu=i686", "-mgeneral-regs-only",
            "-ffreestanding", "-nostdlib", "-fno-builtin",
            "-fno-pic", "-fno-pie", "-fno-stack-protector",
            "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-avx",
            "-O2", "-std=gnu99",
            "-fno-omit-frame-pointer",
            "-nostdinc",
            "-I", SHIMINC,
            "-I", os.path.join(ROOT, "tools", "compiler"),
            "-I", SRC,
            "-Wl,-T," + os.path.join(ROOT, "tools", "compiler", "user.ld"),
            "-Wl,--entry=user_main",
            "-Wl,--build-id=none",
            "-o", elf,
        ] + sources
        print("CC tinysh: %d sources" % len(sources))
        r = subprocess.run(cc, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[-4000:] if r.stderr else r.stdout[-4000:])
            return 1
        print("  ELF: %d bytes" % os.path.getsize(elf))

        os.makedirs(os.path.dirname(OUT), exist_ok=True)
        base, entry_off, image = tcc.elf32_flatten(elf)
        blob = tcc.pack_tncr(base, entry_off, image, 0, 0)
        open(OUT, "wb").write(blob)
        print("  TNCR: %s (%d bytes) load=0x%08X" % (OUT, len(blob), base))
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

if __name__ == "__main__":
    sys.exit(build())
