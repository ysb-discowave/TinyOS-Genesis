#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""_packtest.py -- .pack 多文件包 / run 命令端到端测试

走真实启动路径（安装向导 -> 登录 -> tinysh），验证：
  1) pack 命令能把多文件目录打成 .pack（含子目录）
  2) pkg install <name>.pack 能解包到 install_dir
  3) 安装时 env./prog. 被写进 /bin/path
  4) run <name> 能查 /bin/path 找到并执行程序
"""
import os, socket, subprocess, time, sys, random

R = os.path.dirname(os.path.abspath(__file__))
QEMU = r"C:\Program Files\qemu\qemu-system-i386.exe"
ELF  = os.path.join(R, "build", "kernel.elf")
# 用独立的盘：现有 tinyos-disk.img 是旧系统，盘上旧 tinysh 会盖住 romfs 里的新版。
# 每次测试都建新盘走安装向导，保证装的是当前 romfs 里的 tinysh。
DISK = os.path.join(R, "packtest-disk.img")
PORT = random.randint(26100, 26999)   # 随机端口，避免残留 QEMU 占用

# 启动前杀掉残留 QEMU（否则会占端口 + 半写磁盘，导致串口错乱）
subprocess.run(["taskkill", "/F", "/IM", "qemu-system-i386.exe"],
               capture_output=True)
time.sleep(1.0)

checks = []
def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                           ("  -- " + detail) if detail else ""))

if not os.path.isfile(ELF):
    print("kernel.elf not found -- run build.ps1 first"); sys.exit(1)

# 总是重建盘 -> 走完整安装向导（装当前 romfs 的 tinysh）
if os.path.isfile(DISK):
    try: os.remove(DISK)
    except OSError: pass
time.sleep(0.5)
with open(DISK, "wb") as f:
    f.truncate(64 * 1024 * 1024)
fresh = True
print("created fresh disk (wizard will run), serial port %d" % PORT)

cmd = [QEMU, "-kernel", ELF, "-m", "256",
       "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % DISK,
       "-display", "none",
       "-serial", "tcp:127.0.0.1:%d,server=on,wait=off" % PORT,
       "-monitor", "none"]
qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

class Ser:
    def __init__(self, port, timeout=45):
        dl = time.time() + timeout
        while time.time() < dl:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
                self.s.settimeout(0.3); self.buf = b""; return
            except OSError: time.sleep(0.3)
        raise SystemExit("serial connect timeout")
    def pump(self):
        try:
            d = self.s.recv(65536)
            if d: self.buf += d
        except socket.timeout: pass
    def wait(self, needle, t=30):
        nb = needle.encode() if isinstance(needle, str) else needle
        dl = time.time() + t
        while time.time() < dl:
            if nb in self.buf: return True
            self.pump()
        return False
    def send(self, s): self.s.sendall(s.encode() if isinstance(s, str) else s)
    def grab(self, n=12):
        for _ in range(n): self.pump()
        return self.buf.decode("utf-8", "replace")
    def reset(self): self.buf = b""

def run_cmd(ser, cmdline, wait_prompt="tinysh>", t=20):
    """发一条命令，等回到提示符，返回输出文本"""
    ser.reset()
    ser.send(cmdline + "\r")
    ser.wait(wait_prompt, t)
    out = ser.grab(14)
    # 原始输出落盘，便于事后精读（过滤后的 detail 会丢信息）
    try:
        with open(os.path.join(R, "_raw_%d.log" % _rawseq[0]), "a", encoding="utf-8") as f:
            f.write("### %s\n%s\n" % (cmdline, out))
        _rawseq[0] += 1
    except Exception:
        pass
    return out

_rawseq = [0]

try:
    ser = Ser(PORT)

    if fresh and ser.wait("Choice [1] (s = skip the whole wizard)", 90):
        ser.reset(); ser.send("\r"); time.sleep(1.2)
        for step in ["[2/5]", "[3/5]"]:
            ser.reset(); ser.wait(step, 40); ser.send("\r"); time.sleep(1.0)
        ser.reset(); ser.wait("Password for root", 40); ser.send("root\r"); time.sleep(0.9)
        ser.reset(); ser.wait("Retype password", 40); ser.send("root\r"); time.sleep(1.0)
        ser.reset(); ser.wait("Create a normal user", 40); ser.send("\r"); time.sleep(0.9)
        ser.reset(); ser.wait("[5/5]", 40); ser.send("\r"); time.sleep(0.9)
        ser.reset(); ser.wait("Choice [1]", 40); ser.send("\r"); time.sleep(1.0)

    print("\n== 登录 ==")
    ser.reset()
    check("reaches login prompt", ser.wait("login:", 120))
    ser.send("root\r"); time.sleep(1.0)
    ser.send("root\r"); time.sleep(2.5)
    ser.reset()
    check("tinysh starts automatically", ser.wait("tinysh>", 60))

    # ---- 1) 准备多文件源目录 ----
    print("\n== 准备测试源目录 ==")
    # tinysh 的 echo 只打印文本，不支持 '>' 重定向；用 touch 造文件。
    # 用 /home（安装后一定存在），因为内核 mkdir 不建父目录。
    run_cmd(ser, "mkdir /home/src")
    run_cmd(ser, "touch /home/src/main.TNCR")
    run_cmd(ser, "mkdir /home/src/bin")
    run_cmd(ser, "touch /home/src/bin/tool.TNCR")
    o = run_cmd(ser, "ls /home/src")
    check("source dir created with main.TNCR", "main.TNCR" in o, repr([l for l in o.splitlines() if "main" in l][:1]))
    check("source subdir bin/ created", "bin" in o, repr([l for l in o.splitlines() if "bin" in l][:1]))

    # ---- 2) pack 打包 ----
    print("\n== pack 打包 ==")
    o = run_cmd(ser, "pack /home/src -o /home/demo.pack --name demo --dir /opt/demo --prog demo=/opt/demo/main.TNCR --env PATH=/opt/demo/bin", t=25)
    check("pack reports success", "wrote /home/demo.pack" in o,
          repr([l.strip() for l in o.splitlines() if "pack:" in l][:3]))
    check("pack counted 2 files", "2 file(s)" in o,
          repr([l.strip() for l in o.splitlines() if "file(s)" in l][:1]))

    # ---- 3) pkg install .pack ----
    print("\n== pkg install .pack ==")
    # pack 输出默认在 /tmp，先移到 source dir，或直接用路径安装（install 支持直接路径）
    o = run_cmd(ser, "pkg install /home/demo.pack", t=25)
    check("install detects .pack", "pack:" in o, repr([l.strip() for l in o.splitlines() if "pack:" in l][:1]))
    check("install unpacks to /opt/demo", "/opt/demo" in o,
          repr([l.strip() for l in o.splitlines() if "->" in l or "install_dir" in l][:1]))
    check("install releases main.TNCR", "main.TNCR" in o,
          repr([l.strip() for l in o.splitlines() if "main.TNCR" in l][:1]))
    check("install releases subdir bin/tool.TNCR", "bin/tool.TNCR" in o,
          repr([l.strip() for l in o.splitlines() if "tool.TNCR" in l][:1]))
    check("install registered env in /bin/path", "env var" in o,
          repr([l.strip() for l in o.splitlines() if "env" in o and "path" in l][:1]))
    check("install registered prog in /bin/path", "program mapping" in o,
          repr([l.strip() for l in o.splitlines() if "mapping" in l][:1]))

    # ---- 4) 验证释放的文件真实存在 ----
    print("\n== 验证安装产物 ==")
    o = run_cmd(ser, "ls /opt/demo")
    check("/opt/demo/main.TNCR exists", "main.TNCR" in o, repr([l.strip() for l in o.splitlines() if "main" in l][:1]))
    o = run_cmd(ser, "ls /opt/demo/bin")
    check("/opt/demo/bin/tool.TNCR exists", "tool.TNCR" in o, repr([l.strip() for l in o.splitlines() if "tool" in l][:1]))

    # ---- 5) /bin/path 内容 ----
    print("\n== /bin/path 注册表 ==")
    o = run_cmd(ser, "cat /bin/path")
    check("/bin/path has prog.demo mapping", "prog.demo=/opt/demo/main.TNCR" in o,
          repr([l.strip() for l in o.splitlines() if "prog.demo" in l][:1]))
    check("/bin/path has env.PATH", "env.PATH=/opt/demo/bin" in o,
          repr([l.strip() for l in o.splitlines() if "env.PATH" in l][:1]))

    # ---- 6) run 查 path 执行 ----
    print("\n== run 命令 ==")
    o = run_cmd(ser, "run demo", t=20)
    # demo/main.TNCR 内容不是真程序，prog_exec 会尝试加载；无论成功与否，
    # 关键是 run 不能报 "no program"（说明查到了映射）
    check("run finds prog.demo via /bin/path (not 'no program')",
          "no program" not in o, repr([l.strip() for l in o.splitlines() if "run:" in l][:1]))

    o = run_cmd(ser, "run nosuchprog", t=15)
    check("run reports unknown program clearly", "no program" in o,
          repr([l.strip() for l in o.splitlines() if "no program" in l][:1]))

    # ---- 7) help 里能看到新命令 ----
    print("\n== help 收录 ==")
    o = run_cmd(ser, "help")
    check("help lists pack", "pack" in o)
    check("help lists run", " run " in o or o.rstrip().endswith("run"))

finally:
    try: qemu.kill()
    except Exception: pass
    try: qemu.wait(timeout=5)
    except Exception: pass
    # 兜底再杀一次，确保不留残留进程影响下次运行
    subprocess.run(["taskkill", "/F", "/IM", "qemu-system-i386.exe"],
                   capture_output=True)

print("\n== 结果 ==")
p = sum(1 for _, ok, _ in checks if ok)
print("%d/%d passed" % (p, len(checks)))
for name, ok, detail in checks:
    if not ok:
        print("  FAIL: %s  %s" % (name, detail))
sys.exit(0 if p == len(checks) else 1)
