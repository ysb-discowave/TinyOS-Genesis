#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=============================================================================
 tcc —— TinyOS 编译器驱动
=============================================================================
把多种主流/教学语言编译成 TinyOS 原生可执行文件 (.TNCR)。

支持的语言（--lang，缺省按扩展名推断）
-----------------------------------------------------------------------------
  c        C    （真正的机器码，由 LLVM/clang 交叉编译，能力最强）
  cpp     C++   （无标准库的 freestanding C++，-fno-exceptions -fno-rtti）
  minic   MiniC （C 简化子集，print() 直接输出）
  tiny    TinyLang（教学语言：let/print/gui/if/while）
  basic   BASIC（行号 + PRINT/LET/IF..THEN/GOTO/INPUT/END）

用法
-----------------------------------------------------------------------------
  tcc <源文件> -o <输出.TNCR> [--lang c|cpp|minic|tiny|basic] [--gui] [--desktop]
  tcc desktop   -o DESKTOP.TNCR --desktop        # 生成桌面启动器

产物 .TNCR 由 TinyOS 的 `run` 命令或桌面终端执行。

TNCR 结构（小端）
-----------------------------------------------------------------------------
  0   4  "TNCR"
  4   4  flags   bit0=需要图形窗口(GUI)  bit1=桌面启动器(DESKTOP)
  8   4  load_addr   代码链接/装载地址
  12  4  entry_off   入口 = load_addr + entry_off
  16  4  code_size   映像字节数
  20  4  bss_size    映像之后需清零的字节数
  24  N  payload     扁平映像（.text/.rodata/.data 按地址顺序拼接）
=============================================================================
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
STARTUP_LINK_ADDR = 0x01000000

TNCR_F_GUI = 1 << 0
TNCR_F_DESKTOP = 1 << 1

# --------------------------------------------------------------------------
# 工具链定位：优先 zig（自带 clang+lld，单文件、跨平台），其次 clang
# --------------------------------------------------------------------------
def _search_dirs():
    """在项目内常见的工具目录中查找编译器（zig 常被解压到 dl/ 下而不在 PATH 中）。"""
    root = os.path.dirname(os.path.dirname(HERE))     # TinyOS-os/
    home = os.path.expanduser("~")
    # 工具链包（zig / nasm）统一放在桌面的 tinyYY\toolchain 下，
    # 不往项目目录里塞几百 MB 的压缩包。
    yy = os.path.join(home, "Desktop", "tinyYY", "toolchain")
    dirs = [
        os.path.join(yy, "bin"),
        yy,
        os.path.join(HERE, "bin"),
        os.path.join(root, "tools", "bin"),
        os.path.join(root, "dl"),
        os.path.join(os.path.dirname(root), "dl"),
        os.path.join(home, "dl"),
    ]
    return [d for d in dirs if os.path.isdir(d)]


def _find_in_dirs(names):
    for d in _search_dirs():
        try:
            for entry in os.listdir(d):
                p = os.path.join(d, entry)
                if os.path.isdir(p):
                    for n in names:
                        c = os.path.join(p, n)
                        if os.path.exists(c):
                            return c
                for n in names:
                    if entry.lower() == n:
                        return p
        except OSError:
            continue
    return None


def find_toolchain():
    zig = os.environ.get("TCC_ZIG")
    if zig and os.path.exists(zig):
        return ("zig", zig)
    zig = shutil.which("zig")
    if zig:
        return ("zig", zig)
    zig = _find_in_dirs(["zig.exe", "zig"])
    if zig:
        return ("zig", zig)
    clang = os.environ.get("TCC_CC")
    if clang and os.path.exists(clang):
        return ("clang", clang)
    clang = shutil.which("clang")
    if clang:
        return ("clang", clang)
    clang = _find_in_dirs(["clang.exe", "clang"])
    if clang:
        return ("clang", clang)
    return (None, None)


def compile_elf(kind, tool, src_path, out_elf, verbose=False):
    """把源文件编译并链接为固定地址的 ELF32 可执行文件。"""
    is_cpp = src_path.lower().endswith((".cpp", ".cc", ".cxx"))
    if kind == "zig":
        base = [tool, "c++" if is_cpp else "cc", "-target", "x86-freestanding"]
    else:
        base = [tool + ("++" if is_cpp else ""), "--target=i686-elf"]

    cmd = base + [
        # 用户程序同样跑在裸机上（无 SSE 上下文），必须钉住 CPU 基线，
        # 否则 O2 自动向量化会生成 movaps/xmm 指令导致 #UD。
        "-mcpu=i686", "-mgeneral-regs-only",
        "-ffreestanding", "-nostdlib", "-fno-builtin",
        "-fno-pic", "-fno-pie", "-fno-stack-protector",
        "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-80387", "-mno-avx",
        "-O2", "-Wall",
        "-I", HERE,
    ]
    if not is_cpp:
        cmd += ["-nostdinc"]
    if is_cpp:
        cmd += ["-fno-exceptions", "-fno-rtti"]
    cmd += [
        "-Wl,-T," + os.path.join(HERE, "user.ld"),
        # 关键：必须显式指定入口。ld.lld 不采信链接脚本里的 ENTRY()，
        # 而它默认开启 --gc-sections；找不到入口符号(_start)时会把
        # .text/.rodata 全部当作不可达回收，产出一个没有任何可装载段的空 ELF，
        # 上层 elf32_flatten 就会报 "ELF 中没有可装载段"。
        "-Wl,--entry=user_main",
        "-Wl,--build-id=none",
        "-o", out_elf, src_path,
    ]
    if verbose:
        print("  $ " + " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("编译失败（%s）" % " ".join(base[:2]))
    if r.stderr.strip() and verbose:
        sys.stderr.write(r.stderr)


# --------------------------------------------------------------------------
# ELF32 -> 扁平映像 + 入口偏移 + BSS 大小
# --------------------------------------------------------------------------
SHT_NOBITS = 8
SHF_ALLOC = 0x2

def elf32_flatten(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or struct.unpack_from("<H", d, 18)[0] != 3:
        raise SystemExit("不是 32 位小端 ELF: %s" % path)
    e_shoff, = struct.unpack_from("<I", d, 0x20)
    e_shentsize, = struct.unpack_from("<H", d, 0x2E)
    e_shnum, = struct.unpack_from("<H", d, 0x30)
    e_shstrndx, = struct.unpack_from("<H", d, 0x32)

    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset,
         sh_size, sh_link, sh_info, sh_align, sh_entsize) = struct.unpack_from("<IIIIIIIIII", d, o)
        secs.append(dict(name=sh_name, type=sh_type, flags=sh_flags, addr=sh_addr,
                         off=sh_offset, size=sh_size, link=sh_link, entsize=sh_entsize))

    # 字符串表
    strtab_off = secs[e_shstrndx]["off"]
    def secname(i):
        o = strtab_off + secs[i]["name"]
        e = d.index(b"\x00", o)
        return d[o:e].decode("utf-8", "replace")

    alloc = [s for s in secs if (s["flags"] & SHF_ALLOC)]
    if not alloc:
        raise SystemExit("ELF 中没有可装载段")
    base = min(s["addr"] for s in alloc)
    end = max(s["addr"] + s["size"] for s in alloc)

    image = bytearray(end - base)
    for s in alloc:
        if s["type"] == SHT_NOBITS or s["size"] == 0:
            continue
        pos = s["addr"] - base
        image[pos:pos + s["size"]] = d[s["off"]:s["off"] + s["size"]]

    # 找 user_main 入口
    entry = None
    for i in range(e_shnum):
        if secs[i]["type"] != 2:      # SHT_SYMTAB
            continue
        sym_strtab = secs[secs[i]["link"]]["off"]
        n = secs[i]["size"] // 16
        for k in range(n):
            o = secs[i]["off"] + k * 16
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", d, o)
            so = sym_strtab + st_name
            se = d.index(b"\x00", so)
            if d[so:se] == b"user_main":
                entry = st_value
                break
        if entry is not None:
            break
    if entry is None:
        raise SystemExit("在 ELF 符号表中找不到 user_main")

    return base, entry - base, bytes(image)


def pack_tncr(load_addr, entry_off, code, bss_size, flags):
    hdr = (b"TNCR" +
           struct.pack("<IIIII", flags, load_addr, entry_off, len(code), bss_size))
    return hdr + code


# ==========================================================================
# 语言前端（-> C）
# ==========================================================================
PROLOGUE = '#include "api_user.h"\n'


def _tok(src):
    """通用分词：字符串 / 注释 / 数字 / 标识符 / 运算符。"""
    pat = re.compile(r'''
        "(?:\\.|[^"\\])*"      # 字符串
      | //[^\n]*               # 行注释
      | /\*.*?\*/              # 块注释
      | [A-Za-z_]\w*
      | \d+
      | ==|!=|<=|>=|<>|&&|\|\|
      | [-+*/%=<>!(){}\[\],;:]
      | \s+
    ''', re.X | re.S)
    return [t for t in pat.findall(src)]


def tiny_to_c(src):
    """TinyLang -> C"""
    toks = _tok(src)
    out = []
    vars_seen = set()
    i = 0
    n = len(toks)

    def rest_of_stmt():
        nonlocal i
        parts = []
        depth = 0
        while i < n:
            t = toks[i]
            if t in "({[":
                depth += 1
            elif t in ")}]":
                depth -= 1
            if t == ";" and depth <= 0:
                i += 1
                break
            if t.strip():
                parts.append(t)
            i += 1
        return " ".join(parts).strip()

    while i < n:
        t = toks[i]
        s = t.strip()
        if not s or s.startswith("//") or s.startswith("/*"):
            i += 1
            continue
        if s in "{}":
            out.append(s)
            i += 1
            continue
        if s == "print":
            i += 1
            out.append("api->println(%s);" % (rest_of_stmt() or '""'))
            continue
        if s == "printn":
            i += 1
            out.append("{ char _t[16]; api->itoa(%s, _t); api->println(_t); }"
                       % (rest_of_stmt() or "0"))
            continue
        if s == "gui":
            i += 1
            out.append("api->gui_open(%s);" % (rest_of_stmt() or '"窗口",""'))
            continue
        # 块语句 if <cond> { ... } / while <cond> { ... }
        # 分词器把 { } 各自成一个 token，这里只把条件补上括号，
        # 花括号本身交给下面的 "{}" 分支原样输出。
        if s == "if" or s == "while":
            kind = s
            i += 1
            parts = []
            while i < n:
                t = toks[i].strip()
                i += 1
                if t == "{":
                    break
                if t:
                    parts.append(t)
            out.append("%s (%s) {" % (kind, " ".join(parts)))
            continue
        if s == "let":
            i += 1
            # 分词结果里混着空白 token，必须跳过才拿得到变量名
            while i < n and not toks[i].strip():
                i += 1
            name = toks[i].strip() if i < n else "v"
            vars_seen.add(name)
            r = rest_of_stmt()
            if r:
                out.append("%s;" % r)
            continue
        # 赋值 name = expr  或 表达式语句
        r = rest_of_stmt()
        m = re.match(r"^([A-Za-z_]\w*)\s*=", r)
        if m:
            vars_seen.add(m.group(1))
        if r:
            out.append("%s;" % r)

    decls = "".join("    int %s;\n" % v for v in sorted(vars_seen))
    body = "\n    ".join(x for x in out if x)
    return (PROLOGUE +
            "void user_main(tinyos_api_t *api) {\n" +
            decls +
            "    " + body + "\n}\n")


BASIC_VARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def _basic_expr_to_c(e):
    """BASIC 表达式 -> C：= 变 ==，<> 变 !=，^ 变 ** 不支持则忽略。"""
    e = e.replace("<>", "!=")
    # 把单独的 = 替换为 ==（仅用于条件）
    e = re.sub(r"(?<![<>=!])=(?!=)", "==", e)
    e = re.sub(r"\bMOD\b", "%", e, flags=re.I)
    return e.strip()


def basic_to_c(src):
    """BASIC（行号子集）-> C"""
    lines = []
    for raw in src.splitlines():
        line = raw.strip()
        if not line:
            continue
        up = line.upper()
        if up.startswith("REM"):
            continue
        m = re.match(r"^(\d+)\s+(.*)$", line)
        if m:
            lines.append((m.group(1), m.group(2)))
        else:
            lines.append((None, line))

    body = []
    for label, stmt in lines:
        if label:
            body.append("L%s: ;" % label)
        up = stmt.upper()

        # 行号后面跟的 REM 同样是注释（前面的循环只挡掉了"行首 REM"）
        if up.startswith("REM"):
            continue

        if up.startswith("PRINT"):
            arg = stmt[5:].strip()
            if arg.startswith('"'):
                body.append("api->println(%s);" % arg)
            else:
                body.append("{ char _t[16]; api->itoa(%s, _t); api->println(_t); }"
                            % _basic_expr_to_c(arg))
        elif up.startswith("LET"):
            body.append("%s;" % stmt[3:].strip())
        elif up.startswith("INPUT"):
            v = stmt[5:].strip()
            body.append('{ char _b[32]; int _n = api->readline(_b, 32); '
                        '_b[_n<0?0:_n]=0; %s = mc_atoi(_b); }' % v)
        elif up.startswith("IF"):
            rest = stmt[2:].strip()
            k = rest.upper().find("THEN")
            if k < 0:
                continue
            cond = _basic_expr_to_c(rest[:k])
            thenp = rest[k + 4:].strip()
            if thenp.upper().startswith("GOTO"):
                body.append("if (%s) goto L%s;" % (cond, thenp[4:].strip()))
            elif thenp.upper().startswith("PRINT"):
                a = thenp[5:].strip()
                if a.startswith('"'):
                    body.append('if (%s) api->println(%s);' % (cond, a))
                else:
                    body.append('if (%s) { char _t[16]; api->itoa(%s,_t); api->println(_t); }'
                                % (cond, a))
            elif thenp.upper().startswith("LET"):
                body.append("if (%s) { %s; }" % (cond, thenp[3:].strip()))
            else:
                body.append("if (%s) { %s; }" % (cond, thenp))
        elif up.startswith("GOTO"):
            body.append("goto L%s;" % stmt[4:].strip())
        elif up.startswith("END"):
            body.append("return;")
        elif up.startswith("CLS"):
            body.append('api->println("");')
        else:
            body.append("%s;" % stmt)

    decls = "    int " + ",".join("%s=0" % v for v in BASIC_VARS) + ";\n"
    return (PROLOGUE +
            'static int mc_atoi(const char *s){int v=0,neg=0;while(*s==\' \')s++;'
            'if(*s==\'-\'){neg=1;s++;}while(*s>=\'0\'&&*s<=\'9\'){v=v*10+(*s-\'0\');s++;}'
            'return neg?-v:v;}\n'
            "void user_main(tinyos_api_t *api) {\n" + decls +
            "    " + "\n    ".join(body) + "\n}\n")


def minic_to_c(src):
    """MiniC -> C。
    MiniC 是 C 的简化子集，提供两个输出原语：
      print("文本")   -> api->println(文本)
      printn(表达式)  -> 整数转字符串后输出
    其余语法与 C 相同。"""
    s = src
    s = re.sub(r"\bprintn\s*\(", "MC_PRINTN(api, ", s)
    s = re.sub(r"\bprint\s*\(", "api->println(", s)
    helper = ("#define MC_PRINTN(A, v) do { char _t[16]; (A)->itoa((int)(v), _t); "
              "(A)->println(_t); } while (0)\n")
    return PROLOGUE + helper + s + "\n"


# ==========================================================================
# 主流程
# ==========================================================================
EXT_LANG = {".c": "c", ".cpp": "cpp", ".cc": "cpp", ".cxx": "cpp",
            ".tiny": "tiny", ".bas": "basic", ".basic": "basic", ".minic": "minic"}


def build(src_arg, out, lang, flags, verbose=False):
    kind, tool = find_toolchain()
    if kind is None:
        raise SystemExit("找不到编译器：请安装 zig（推荐）或 clang，"
                         "或用 TCC_ZIG / TCC_CC 指定路径。")

    tmpdir = tempfile.mkdtemp(prefix="tcc_")
    try:
        if src_arg == "desktop":
            csrc = (PROLOGUE +
                    "void user_main(tinyos_api_t *api) { api->desktop(); }\n")
            flags |= TNCR_F_DESKTOP
            src_path = os.path.join(tmpdir, "desktop.c")
            open(src_path, "w", encoding="utf-8").write(csrc)
        else:
            ext = os.path.splitext(src_arg)[1].lower()
            if lang is None:
                lang = EXT_LANG.get(ext, "c")
            text = open(src_arg, "r", encoding="utf-8").read()
            if lang == "c" or lang == "cpp":
                src_path = src_arg
            elif lang == "tiny":
                src_path = os.path.join(tmpdir, "u.c")
                open(src_path, "w", encoding="utf-8").write(tiny_to_c(text))
            elif lang == "basic":
                src_path = os.path.join(tmpdir, "u.c")
                open(src_path, "w", encoding="utf-8").write(basic_to_c(text))
            elif lang == "minic":
                src_path = os.path.join(tmpdir, "u.c")
                open(src_path, "w", encoding="utf-8").write(minic_to_c(text))
            else:
                raise SystemExit("未知语言: %s" % lang)

            low = text.lower()
            # 必须看起来像"调用"而不是"提到"：CC.TNCR 内置了一张 api 成员名
            # 表，里面有 "gui_open" 这个字符串，光看名字会误判成 GUI 程序。
            if "gui_open(" in low or "gui(" in low:
                flags |= TNCR_F_GUI

        elf = os.path.join(tmpdir, "u.elf")
        compile_elf(kind, tool, src_path, elf, verbose=verbose)
        base, entry_off, image = elf32_flatten(elf)
        blob = pack_tncr(base, entry_off, image, 0, flags)
        open(out, "wb").write(blob)
        gname = os.path.basename(out)
        print("  %-22s %6d 字节  load=0x%08X entry=+0x%X flags=%s"
              % (gname, len(blob), base, entry_off,
                 ("GUI " if flags & TNCR_F_GUI else "") +
                 ("DESKTOP" if flags & TNCR_F_DESKTOP else "") or "-"))
        return True
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    src = args[0]
    out = None
    lang = None
    flags = 0
    verbose = False
    i = 1
    while i < len(args):
        a = args[i]
        if a == "-o":
            out = args[i + 1]; i += 2
        elif a == "--lang":
            lang = args[i + 1]; i += 2
        elif a == "--gui":
            flags |= TNCR_F_GUI; i += 1
        elif a == "--desktop":
            flags |= TNCR_F_DESKTOP; i += 1
        elif a in ("-v", "--verbose"):
            verbose = True; i += 1
        else:
            i += 1
    if out is None:
        base = "DESKTOP" if src == "desktop" else os.path.splitext(os.path.basename(src))[0]
        out = base + ".TNCR"
    build(src, out, lang, flags, verbose)
    return 0


if __name__ == "__main__":
    sys.exit(main())
