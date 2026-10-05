#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_lua.py -- 把 Lua 5.4 核心 + shim + main 交叉编译成 LUA.TNCR"""
import os, sys, subprocess, tempfile, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ZIG  = os.path.join(os.path.expanduser("~"), "Desktop", "tinyYY",
                    "toolchain", "zig-windows-x86_64-0.14.0", "zig.exe")
LUA_SRC = os.path.join(ROOT, "thirdparty", "lua", "src")
SHIM    = os.path.join(ROOT, "tools", "lua")
OUT     = os.path.join(os.environ.get("TINYOS_OUT",
             os.path.join(os.path.expanduser("~"), "Desktop", "tinyYY")), "tncr", "LUA.TNCR")

sys.path.insert(0, os.path.join(ROOT, "tools", "compiler"))
import tcc

# 要编译的 Lua 源（不含 liolib.c / loadlib.c / lua.c / luac.c）
LUA_C = [
    "lapi.c","lauxlib.c","lbaselib.c","lcode.c","lcorolib.c",
    "lctype.c","ldebug.c","ldo.c","lfunc.c","lgc.c",
    "llex.c","lmem.c","lobject.c","lopcodes.c","lparser.c",
    "lstate.c","lstring.c","ltable.c","ltm.c","lundump.c","ldump.c",
    "lvm.c","lzio.c",
    "lstrlib.c","ltablib.c","lmathlib.c","loslib.c","lutf8lib.c",
    "liolib.c","ldblib.c","linit.c",
]
LOCAL_C = ["lua_shim.c", "lua_main.c", "lua_package.c"]

def build():
    if not os.path.isfile(ZIG):
        print("!! zig not found at", ZIG); return 1
    tmp = tempfile.mkdtemp(prefix="luatncr_")
    try:
        sources = []
        for f in LUA_C:
            p = os.path.join(LUA_SRC, f)
            if not os.path.isfile(p):
                print("!! missing:", f); return 1
            sources.append(p)
        for f in LOCAL_C:
            sources.append(os.path.join(SHIM, f))

        elf = os.path.join(tmp, "lua.elf")
        cc = [ZIG, "cc",
            "-target", "x86-freestanding",
            "-mcpu=i686", "-mgeneral-regs-only",
            "-ffreestanding", "-nostdlib", "-fno-builtin",
            "-fno-pic", "-fno-pie", "-fno-stack-protector",
            "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-avx",
            "-O2", "-std=gnu99",
            "-fno-omit-frame-pointer",
            "-nostdinc",
            "-I", os.path.join(SHIM, "shiminc"),
            "-I", LUA_SRC,
            "-I", SHIM,
            "-I", os.path.join(ROOT, "tools", "compiler"),
            "-I", os.path.join(ROOT, "src", "kernel", "include"),
            "-Wl,-T," + os.path.join(ROOT, "tools", "compiler", "user.ld"),
            "-Wl,--entry=user_main",
            "-Wl,--build-id=none",
            "-o", elf,
        ] + sources
        print("CC: %d sources" % len(sources))
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
