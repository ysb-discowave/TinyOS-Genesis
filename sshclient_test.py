#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""_sshclient_test.py -- SSHCLIENT.TNCR 端到端实测（两 VM 自测）

SERVER VM 跑 SSHD.TNCR（hostfwd 2222->22），CLIENT VM 跑 SSHCLIENT.TNCR
连 10.0.2.2:2222（网关=宿主，hostfwd 把 2222 指到 server guest:22），
执行 `echo sshclient_connected_ok`，验证完整握手 + password 认证 + exec 回显。
串口日志落盘到 build/sshclient_qemu.log。
"""
import os, socket, subprocess, time, random

R = os.path.dirname(os.path.abspath(__file__))
QEMU = r"C:\Program Files\qemu\qemu-system-i386.exe"
ELF  = os.path.join(R, "build", "kernel.elf")
LOG  = os.path.join(R, "build", "sshclient_qemu.log")

SERVER_DISK = os.path.join(R, "sshtest-server.img")
CLIENT_DISK = os.path.join(R, "sshtest-client.img")
P1 = random.randint(27100, 27499)
P2 = random.randint(27500, 27999)

def log(m):
    with open(LOG, "a", encoding="utf-8") as f: f.write(m + "\n")
    print(m)

subprocess.run(["taskkill", "/F", "/IM", "qemu-system-i386.exe"], capture_output=True)
time.sleep(1.0)
for d in (SERVER_DISK, CLIENT_DISK):
    if os.path.isfile(d):
        try: os.remove(d)
        except OSError: pass
    time.sleep(0.3)
    with open(d, "wb") as f: f.truncate(64 * 1024 * 1024)
open(LOG, "w", encoding="utf-8").write("")
log("=== SSHCLIENT end-to-end test ===")
log("server serial %d (hostfwd 2222->22), client serial %d" % (P1, P2))

def launch(tag, disk, port, hostfwd):
    net = ["-netdev", "user,id=n0" + (",hostfwd=tcp::2222-:22" if hostfwd else ""),
           "-device", "e1000,netdev=n0"]
    cmd = [QEMU, "-kernel", ELF, "-m", "256",
           "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % disk,
           "-display", "none",
           "-serial", "tcp:127.0.0.1:%d,server=on,wait=off" % port,
           "-monitor", "none"] + net
    log("[%s] %s" % (tag, " ".join(cmd[:7])))
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

class Ser:
    def __init__(self, port, t=180):
        dl = time.time() + t
        while time.time() < dl:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
                self.s.settimeout(0.3); self.buf = b""; return
            except OSError: time.sleep(0.3)
        raise SystemExit("serial timeout")
    def pump(self):
        try:
            d = self.s.recv(65536)
            if d: self.buf += d
        except socket.timeout: pass
    def wait(self, needle, t=90):
        nb = needle.encode() if isinstance(needle, str) else needle
        dl = time.time() + t
        while time.time() < dl:
            if nb in self.buf: return True
            self.pump()
        return False
    def send(self, s): self.s.sendall(s.encode())
    def reset(self): self.buf = b""
    def grab(self, n=30):
        for _ in range(n): self.pump()
        return self.buf.decode("utf-8", "replace")

def run_wizard(ser, tag):
    """响应式走安装向导，设 root/root，返回是否见到 login:"""
    if not ser.wait("Choice [1] (s = skip the whole wizard)", 150):
        log("[%s] WARN: no wizard first prompt" % tag)
    root1 = False
    dl = time.time() + 90
    while time.time() < dl:
        ser.pump()
        b = ser.buf.decode("utf-8", "replace")
        if "login:" in b:
            return True
        if "Password for root" in b and not root1:
            ser.send("root\r"); root1 = True; time.sleep(1.0); ser.reset(); continue
        if "Retype password" in b:
            ser.send("root\r"); time.sleep(1.0); ser.reset(); continue
        if "Create a normal user" in b:
            ser.send("\r"); time.sleep(1.0); ser.reset(); continue
        if "[5/5]" in b or (("Choice [1]" in b) and ("install" in b.lower() or "confirm" in b.lower())):
            ser.send("\r"); time.sleep(1.0); ser.reset(); continue
        # 默认：Accept 当前提示（跳过/下一步）
        ser.send("\r"); time.sleep(1.2); ser.reset()
    return ser.wait("login:", 30)

def boot_login(ser, tag):
    ok = run_wizard(ser, tag)
    log("[%s] wizard->login: %s" % (tag, ok))
    ser.send("root\r"); time.sleep(1.0)
    ser.send("root\r"); time.sleep(3.0)
    ok2 = ser.wait("tinysh>", 60)
    log("[%s] tinysh reached: %s" % (tag, ok2))
    return ok2

srv = launch("SERVER", SERVER_DISK, P1, hostfwd=True)
cl  = launch("CLIENT", CLIENT_DISK, P2, hostfwd=False)
time.sleep(2.0)

try:
    ser = Ser(P1); cli = Ser(P2)
    boot_login(ser, "SERVER")
    ser.send("SSHD\r"); time.sleep(2.0)
    srv_up = ser.wait("listening on guest TCP port 22", 25)
    log("[SERVER] sshd listening: %s" % srv_up)
    log("[SERVER] snippet:\n" + ser.grab(10)[-1200:])

    boot_login(cli, "CLIENT")
    cli.send("SSHCLIENT 10.0.2.2 2222 echo sshclient_connected_ok\r")
    log("[CLIENT] sent SSHCLIENT command")
    if cli.wait("user:", 45):
        log("[CLIENT] user prompt -> root"); cli.send("root\r")
    else:
        log("[CLIENT] WARN no user prompt")
    if cli.wait("password:", 45):
        log("[CLIENT] password prompt -> root"); cli.send("root\r")
    else:
        log("[CLIENT] WARN no password prompt")
    got_conn = (cli.wait("sshclient_connected_ok", 60) or cli.wait("ssh: session closed", 60)
               or cli.wait("authentication failed", 60) or cli.wait("connection failed", 60))
    out = cli.grab(60)
    log("[CLIENT] result seen: %s" % got_conn)
    log("[CLIENT] exec-output detected: %s" % ("sshclient_connected_ok" in out))
    log("[CLIENT] tail (last 3500):\n" + out[-3500:])
    log("=== FULL SERVER ===\n" + ser.grab(60)[-2500:])
    log("=== FULL CLIENT ===\n" + out[-3500:])
finally:
    for p in (srv, cl):
        try: p.kill()
        except Exception: pass
        try: p.wait(timeout=5)
        except Exception: pass
    subprocess.run(["taskkill", "/F", "/IM", "qemu-system-i386.exe"], capture_output=True)
log("=== done ===")
