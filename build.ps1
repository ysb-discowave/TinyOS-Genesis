<#
============================================================================
 TinyOS 裸机内核 —— Windows 构建脚本 (PowerShell)
============================================================================
不依赖 GNU make；在本机用 nasm + (zig|clang|gcc) + qemu 构建并可选启动。

用法：
  powershell -ExecutionPolicy Bypass -File build.ps1                # 构建 kernel.elf
  powershell -ExecutionPolicy Bypass -File build.ps1 -Img           # 额外生成 tinyos.img
  powershell -ExecutionPolicy Bypass -File build.ps1 -Img -Installer # 生成安装介质 tinyos-install.img
                                                                    # 从它启动会直接进安装界面（不格式化目标盘）
  powershell -ExecutionPolicy Bypass -File build.ps1 -Users         # 编译用户程序(TNCR)并打包 romfs
  powershell -ExecutionPolicy Bypass -File build.ps1 -Headless      # 无窗口启动并抓串口到 serial.log
  powershell -ExecutionPolicy Bypass -File build.ps1 -Run           # 图形窗口启动
  powershell -ExecutionPolicy Bypass -File build.ps1 -Clean

显式指定工具（自动探测失败时）：
  -Nasm "C:\path\nasm.exe"
  -CC   "C:\path\zig.exe"      # zig 优先（自带 clang + lld）
  -CC   "C:\path\clang.exe"
  -Qemu "C:\Program Files\qemu\qemu-system-i386.exe"
============================================================================
#>
param(
    [string]$Nasm   = "",
    [string]$CC     = "",
    [string]$Qemu   = "",
    [string]$Target = "x86-freestanding",   # zig/clang 交叉目标（i386 32 位）
    [switch]$Img,
    [switch]$Users,
    [switch]$Run,
    [switch]$Headless,
    [switch]$Debug,
    [switch]$Clean,
    [switch]$Disk,
    [switch]$Installer,   # 生成"安装介质"：开机直接进安装界面的 tinyos-install.img
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

# ---- 规范化 Target：允许 i686-elf 写法 ----
if ($Target -ieq "i686-elf") { $Target = "x86-freestanding" }

function Say($m) { Write-Host $m }
function Step($m) { Write-Host "`n>> $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "   $m" -ForegroundColor Green }
function Warn($m) { Write-Host "   $m" -ForegroundColor Yellow }

# --------------------------- 工具探测 ---------------------------
function Find-Exe([string[]]$cands) {
    foreach ($c in $cands) {
        if (-not $c) { continue }
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    return $null
}

function Find-OnPath([string]$name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

# nasm
if (-not $Nasm) {
    $Nasm = Find-OnPath "nasm"
    if (-not $Nasm) {
        $Nasm = Find-Exe @(
            "$env:USERPROFILE\Desktop\tinyYY\toolchain\bin\nasm.exe",
            "$env:USERPROFILE\Desktop\tinyYY\toolchain\nasm_x\nasm.exe",
            "$Root\tools\bin\nasm.exe",
            "C:\Program Files\NASM\nasm.exe",
            "C:\Program Files (x86)\NASM\nasm.exe",
            "$env:LOCALAPPDATA\Programs\NASM\nasm.exe",
            "$env:USERPROFILE\scoop\apps\nasm\current\nasm.exe",
            "C:\msys64\mingw64\bin\nasm.exe"
        )
    }
}

# C 编译器：优先 zig（自带 clang+lld），其次 clang，其次 gcc
$CCKind = $null
if (-not $CC) {
    $zig = Find-OnPath "zig"
    if (-not $zig) {
        $zig = Find-Exe @(
            "$env:USERPROFILE\Desktop\tinyYY\toolchain\zig-windows-x86_64-0.14.0\zig.exe",
            "$env:USERPROFILE\Desktop\tinyYY\toolchain\zig\zig.exe",
            "$Root\tools\bin\zig\zig.exe",
            "$Root\dl\zig\zig.exe",
            "$env:LOCALAPPDATA\Programs\zig\zig.exe",
            "C:\zig\zig.exe"
        )
    }
    if ($zig) { $CC = $zig; $CCKind = "zig" }

    if (-not $CC) {
        $clang = Find-OnPath "clang"
        if (-not $clang) {
            $clang = Find-Exe @(
                "C:\Program Files\LLVM\bin\clang.exe",
                "C:\Program Files (x86)\LLVM\bin\clang.exe",
                "C:\msys64\clang64\bin\clang.exe",
                "C:\msys64\ucrt64\bin\clang.exe"
            )
        }
        if ($clang) { $CC = $clang; $CCKind = "clang" }
    }
    if (-not $CC) {
        $gcc = Find-OnPath "gcc"
        if ($gcc) { $CC = $gcc; $CCKind = "gcc" }
    }
} else {
    $base = [IO.Path]::GetFileNameWithoutExtension($CC).ToLower()
    if ($base -eq "zig") { $CCKind = "zig" }
    elseif ($base -like "clang*") { $CCKind = "clang" }
    else { $CCKind = "gcc" }
}

# objcopy
function Find-Objcopy() {
    $c = Find-OnPath "llvm-objcopy"
    if ($c) { return @{ exe=$c; via="llvm-objcopy" } }
    $c = Find-Exe @(
        "C:\Program Files\LLVM\bin\llvm-objcopy.exe",
        "C:\msys64\clang64\bin\llvm-objcopy.exe",
        "C:\msys64\mingw64\bin\objcopy.exe"
    )
    if ($c) { return @{ exe=$c; via="llvm-objcopy" } }
    if ($CCKind -eq "zig" -and $CC) { return @{ exe=$CC; via="zig-objcopy" } }
    return $null
}

# qemu
if (-not $Qemu) {
    $Qemu = Find-OnPath "qemu-system-i386"
    if (-not $Qemu) {
        $Qemu = Find-Exe @(
            "C:\Program Files\qemu\qemu-system-i386.exe",
            "C:\Program Files (x86)\qemu\qemu-system-i386.exe"
        )
    }
}

# --------------------------- 环境汇报 ---------------------------
Step "工具链"
Say ("   NASM = " + $(if ($Nasm) { $Nasm } else { "!! 未找到" }))
Say ("   CC   = " + $(if ($CC)   { "$CC  ($CCKind)" } else { "!! 未找到" }))
$ob = Find-Objcopy
Say ("   OBJ  = " + $(if ($ob) { $ob.exe + " (" + $ob.via + ")" } else { "!! 未找到" }))
Say ("   QEMU = " + $(if ($Qemu) { $Qemu } else { "!! 未找到" }))
Say ("   TGT  = $Target")

if ($Clean) {
    Step "清理"
    Remove-Item -Recurse -Force "$Root\build" -ErrorAction SilentlyContinue
    Remove-Item -Force "$Root\kernel.elf","$Root\kernel.bin","$Root\tinyos.img","$Root\tinyos-install.img","$Root\serial.log" -ErrorAction SilentlyContinue
    Ok "已清理"
    return
}

if (-not $Nasm) { throw "未找到 nasm。请用 -Nasm <路径> 指定。" }

# 用户程序 / romfs 的实际打包见下面 tools\build_users.py 一段。
if (-not $CC)   { throw "未找到 C 编译器（zig/clang/gcc）。请用 -CC <路径> 指定。" }
if (-not $ob)   { Warn "未找到 objcopy；只有 -Img 需要它。" }

# 确定编译器子命令前缀：zig 需要 `zig cc`
$CCPrefix = @()
if ($CCKind -eq "zig") { $CCPrefix = @("cc") }

# --------------------------- 构建参数 ---------------------------
$Build = Join-Path $Root "build"
$ObjD  = Join-Path $Build "obj"
New-Item -ItemType Directory -Force -Path $ObjD | Out-Null

$CFLAGS = @(
    "-target", $Target,
    # 关键：zig 的 x86-freestanding 默认基线 CPU 带 SSE/SSE2，
    # 仅给 -mno-sse 挡不住自动向量化（编译器仍会生成 movaps → 裸机上 #UD/#GP）。
    # 因此显式把目标 CPU 钉在 i686，并用 -mgeneral-regs-only 只允许通用寄存器。
    "-mcpu=i686", "-mgeneral-regs-only",
    "-std=gnu99", "-ffreestanding", "-O2", "-Wall",
    "-fno-stack-protector", "-fno-pic", "-fno-pie", "-fno-builtin",
    "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-80387", "-mno-avx",
    "-nostdlib", "-nostdinc",
    "-I", (Join-Path $Root "src\kernel\include"),
    "-I", (Join-Path $Root "src\kernel")
)

# 安装介质：编译期打上标记，内核开机无条件进入安装向导，
# 并且启动阶段不会格式化/写入任何目标磁盘。
if ($Installer) { $CFLAGS += "-DTINYOS_INSTALL_MEDIA=1" }
$imgName = "tinyos.img"
if ($Installer) { $imgName = "tinyos-install.img" }
if ($Installer) { Say ">> 目标：安装介质 ($imgName) —— 开机直接进入安装界面" }

function Invoke-Tool([string]$exe, [string[]]$argv) {
    if ($Verbose) { Write-Host ("   $ " + $exe + " " + ($argv -join " ")) -ForegroundColor DarkGray }
    # 注意：PowerShell 5.1 下把原生命令的 stderr 重定向到 2>&1 会生成 ErrorRecord，
    # 在 $ErrorActionPreference='Stop' 时会被当成终止性错误（哪怕退出码为 0）。
    # 因此这里临时放宽，仅以退出码作为成败判据。
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = & $exe @argv 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $eap
    if ($code -ne 0) {
        Write-Host "--- 命令失败 (exit $code) ---" -ForegroundColor Red
        Write-Host ("$exe " + ($argv -join " "))
        $out | ForEach-Object { Write-Host $_ }
        throw "编译失败"
    }
    if ($out) { $out | ForEach-Object { Write-Host "   $_" -ForegroundColor DarkGray } }
    return $out
}

# --------------------------- 用户程序 (romfs) ---------------------------
# 必须排在"编译内核 C 源码"之前：build_users.py 会把 romfs 重新生成为
# kernel/romfs.c，只有先生成它，后面编译+链接出来的 kernel.elf 才会真正
# 带上这些文件（放在链接之后的话，kernel.elf 里装的还是上一次的旧 romfs）。
if ($Users) {
    Step "编译用户程序并打包 romfs"
    $py = Find-OnPath "python"
    if (-not $py) { $py = Find-OnPath "python3" }
    if (-not $py) { Warn "未找到 python；跳过用户程序编译" }
    else {
        # 把已探测到的编译器传给 tcc.py，否则它无法找到不在 PATH 里的 zig
        if ($CCKind -eq "zig") { $env:TCC_ZIG = $CC } else { $env:TCC_CC = $CC }
        $oldEnc = [Console]::OutputEncoding
        try {
            [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
            $env:PYTHONIOENCODING = "utf-8"
            Invoke-Tool $py @((Join-Path $Root "tools\build_users.py")) | Out-Null
        } finally {
            [Console]::OutputEncoding = $oldEnc
        }
    }
}

# --------------------------- 汇编 ---------------------------
Step "汇编"
$asmFiles = @(
    @{ src="src\kernel\start.asm"; obj="start.asm.o" },
    @{ src="src\kernel\idt.asm";   obj="idt.asm.o" }
)
foreach ($a in $asmFiles) {
    $src = Join-Path $Root $a.src
    $dst = Join-Path $ObjD $a.obj
    Invoke-Tool $Nasm @("-f","elf32", $src, "-o", $dst) | Out-Null
    Ok ($a.src + " -> " + $a.obj)
}

# --------------------------- 编译 C ---------------------------
Step "编译内核 C 源码"
$cs = @()
$cs += Get-ChildItem (Join-Path $Root "src\kernel") -Filter *.c -File | Select-Object -ExpandProperty FullName
$cs += Get-ChildItem (Join-Path $Root "src\kernel\net") -Filter *.c -File | Select-Object -ExpandProperty FullName
$cs += Get-ChildItem (Join-Path $Root "src\kernel\disk") -Filter *.c -File | Select-Object -ExpandProperty FullName
$cs += Get-ChildItem (Join-Path $Root "src\kernel\fs") -Filter *.c -File | Select-Object -ExpandProperty FullName
$objs = @()
$onames = @()
foreach ($f in $cs) {
    $rel  = $f.Substring($Root.Length + 1)
    $name = ($rel -replace '[\\/]', '_') + ".o"
    $dst  = Join-Path $ObjD $name
    Invoke-Tool $CC ($CCPrefix + $CFLAGS + @("-c", $f, "-o", $dst)) | Out-Null
    $objs += $dst
    $onames += $name
}
Ok ("编译 " + $cs.Count + " 个 .c 文件")

# --------------------------- 链接（两段式） ---------------------------
# install 命令需要一份"可引导的内核镜像"嵌在内核里。做法：
#   1) 先用「不含 kernel\instimg.c、含 kernel\instimg_stub.c」的目标文件链接出
#      payload.elf —— stub 提供长度 0 的载荷符号，install.c 才能正常链接；
#   2) objcopy 成扁平镜像 kernel.bin，nasm 汇编引导扇区 boot.bin；
#   3) tools/gen_instimg.py 把两者写成真正的 kernel\instimg.c，
#      顺便把内核扇区数补进引导扇区的 'KSCN' 字段；
#   4) 重新编译 instimg.c，用「不含 stub、含 instimg.o」的目标文件链接正式内核。
# 两个内核都带 install 命令，但只有正式内核携带可引导镜像，
# 而且不需要"内核嵌入自己"的鸡生蛋循环，产物是确定的。
$bootObjs = @(
    (Join-Path $ObjD "start.asm.o"),
    (Join-Path $ObjD "idt.asm.o")
)

function Link-Kernel([string]$outElf, [string[]]$objects) {
    # 2.0：--strip-all 剥离 .symtab/.strtab/.debug_*（合计 ~330KB）。
    # 裸机环境不需要调试符号；要调试时临时用 -Debug 去掉该开关。
    $a = @(
        "-target", $Target,
        "-nostdlib", "-fuse-ld=lld",
        "-Wl,-T,$(Join-Path $Root 'src\kernel\link.ld')",
        "-Wl,--entry=start",
        "-Wl,--build-id=none",
        "-o", $outElf
    ) + $objects
    if (-not $Debug) { $a += "-Wl,--strip-all" }
    Invoke-Tool $CC ($CCPrefix + $a) | Out-Null
}

$havePayload = $false
$bin = Join-Path $Build "kernel.bin"

if ($ob) {
    Step "生成安装载荷 (payload.elf -> kernel.bin -> kernel\instimg.c)"
    $payObjs = @()
    for ($i = 0; $i -lt $objs.Count; $i++) {
        # 用后缀匹配而不是全名：对象名由相对路径生成（src\kernel\x.c -> src_kernel_x.c.o），
        # 目录一变全名就跟着变，写死全名会静默失配、导致 instimg.c 和 instimg_stub.c
        # 同时被链进内核，ld 报 duplicate symbol。
        if ($onames[$i] -like "*instimg.c.o") { continue }
        # 安装介质特例：写进目标盘的"载荷内核"必须是普通内核。
        # install.c 是唯一用到 TINYOS_INSTALL_MEDIA 的文件，这里为载荷单独
        # 编译一份不带该宏的版本；否则装完的系统会永远以为自己是安装介质
        # —— 每次开机又弹向导，而且启动阶段不敢挂载自己盘上的文件系统。
        if ($Installer -and $onames[$i] -like "*install.c.o") {
            # 文件名必须以 .o 结尾：zig cc 的链接器按扩展名判断输入类型，
            # 叫 xxx.o.nomedia 会被当成"不认识的文件类型"直接报错。
            $nm = Join-Path $ObjD "kernel_install_nomedia.c.o"
            $noMedia = @()
            foreach ($f in $CFLAGS) { if ($f -ne "-DTINYOS_INSTALL_MEDIA=1") { $noMedia += $f } }
            Invoke-Tool $CC ($CCPrefix + $noMedia + @("-c", (Join-Path $Root "src\kernel\install.c"), "-o", $nm)) | Out-Null
            Ok "载荷内核用的 install.c（不带 TINYOS_INSTALL_MEDIA）"
            $payObjs += $nm
            continue
        }
        $payObjs += $objs[$i]
    }
    $payElf = Join-Path $Build "payload.elf"
    Link-Kernel $payElf ($bootObjs + $payObjs)
    Ok ("载荷内核: " + $payElf)

    if ($ob.via -eq "zig-objcopy") {
        Invoke-Tool $ob.exe @("objcopy", "-O", "binary", $payElf, $bin) | Out-Null
    } else {
        Invoke-Tool $ob.exe @("-O", "binary", $payElf, $bin) | Out-Null
    }
    Ok ("扁平镜像: " + $bin + "  (" + (Get-Item $bin).Length + " 字节)")

    $bootBin = Join-Path $Build "boot.bin"
    Invoke-Tool $Nasm @("-f","bin", (Join-Path $Root "src\boot\boot.asm"), "-o", $bootBin) | Out-Null
    Ok ("引导扇区: " + $bootBin)

    $py = Find-OnPath "python"
    if (-not $py) { $py = Find-OnPath "python3" }
    if ($py) {
        $env:PYTHONIOENCODING = "utf-8"
        Invoke-Tool $py @(
            (Join-Path $Root "tools\gen_instimg.py"),
            $bootBin, $bin, (Join-Path $Root "src\kernel\instimg.c"), "0x100000"
        ) | Out-Null
        Invoke-Tool $CC ($CCPrefix + $CFLAGS + @(
            "-c", (Join-Path $Root "src\kernel\instimg.c"),
            "-o", (Join-Path $ObjD "src_kernel_instimg.c.o")
        )) | Out-Null
        Ok "src\kernel\instimg.c 已生成并编译"
        $havePayload = $true
    } else {
        Warn "未找到 python；跳过安装载荷（install 命令将只报告『没有内嵌镜像』）"
    }
} else {
    Warn "未找到 objcopy；跳过安装载荷（install 命令将只报告『没有内嵌镜像』）"
}

Step "链接 kernel.elf"
$elf = Join-Path $Build "kernel.elf"
$finalObjs = @()
$finalObjs += $bootObjs
for ($i = 0; $i -lt $objs.Count; $i++) {
    $n = $onames[$i]
    # instimg.c 在第 3 步被重新生成并编译；havePayload 时下面会再显式追加
    # 一次它的目标文件，因此这里必须先跳过，否则链接器报 duplicate symbol。
    # 同样用后缀匹配，避免目录改名后静默失配。
    if ($n -like "*instimg.c.o") { continue }
    if ($havePayload -and $n -like "*instimg_stub.c.o") { continue }
    $finalObjs += $objs[$i]
}
if ($havePayload) { $finalObjs += (Join-Path $ObjD "src_kernel_instimg.c.o") }
Link-Kernel $elf $finalObjs
Ok ("内核: " + $elf)
# 复制到根目录，方便 make 风格路径
Copy-Item $elf (Join-Path $Root "src\kernel.elf") -Force

# ============================================================
# 注意：用户程序 / romfs 已经在上面做完（见"用户程序 (romfs)"一段）。
# 这里不要再打包一次——那样只会让 kernel.elf 用到旧数据。
# ============================================================

# --------------------------- 原始磁盘映像 ---------------------------
# tinyos.img = 打了补丁的引导扇区 + 扁平内核。它既是可引导的安装介质，
# 也正是 install 写进硬盘的那份内容，所以两者天然一致。
if ($havePayload) {
    Step "生成 $imgName（可直接引导的安装介质）"
    # 介质必须跑"最终内核"：只有它带着 kernel\instimg.c 里的可写盘镜像。
    # 载荷内核（$bin）里的 boot_img/kernel_img 是空桩，拿它做介质的话向导
    # 一进来就会报 "carries no embedded install image"，根本装不了。
    # 最终内核更大，所以引导扇区要按它的扇区数重新打一次 'KSCN' 补丁。
    $binMedia = Join-Path $Build "kernel_media.bin"
    if ($ob.via -eq "zig-objcopy") {
        Invoke-Tool $ob.exe @("objcopy", "-O", "binary", $elf, $binMedia) | Out-Null
    } else {
        Invoke-Tool $ob.exe @("-O", "binary", $elf, $binMedia) | Out-Null
    }
    $bootBin = Join-Path $Build "boot.bin"
    $bootMedia = Join-Path $Build "boot_media.bin"
    if ($py) {
        Invoke-Tool $py @(
            (Join-Path $Root "tools\gen_instimg.py"),
            "--patch", $bootBin, $binMedia, $bootMedia
        ) | Out-Null
    } else {
        Copy-Item (Join-Path $Build "boot_patched.bin") $bootMedia -Force
    }
    $imgPath = Join-Path $Root $imgName
    $bs = [IO.File]::ReadAllBytes($bootMedia)
    $k  = [IO.File]::ReadAllBytes($binMedia)
    # 内核必须补到整扇区：BIOS 是按扇区读的，映像长度不是 512 的整数倍时
    # 最后一个扇区读不回来，引导扇区会直接报 disk read error。
    if (($k.Length % 512) -ne 0) {
        $pad = 512 - ($k.Length % 512)
        $k = $k + (New-Object byte[] $pad)
    }
    $sector = New-Object byte[] 512
    [Array]::Copy($bs, $sector, 512)
    $stream = [IO.File]::Create($imgPath)
    $stream.Write($sector, 0, 512)
    $stream.Write($k, 0, $k.Length)
    $stream.Close()
    Ok ("映像: $imgPath  (" + (512 + $k.Length) + " 字节 = " + ((512 + $k.Length) / 512) + " 扇区, 可引导)")
} elseif ($Img) {
    Step "生成 $imgName"
    if (-not $ob) { throw "缺少 objcopy，无法生成扁平内核映像。" }
    $bin2 = Join-Path $Build "kernel.bin"
    if ($ob.via -eq "zig-objcopy") {
        Invoke-Tool $ob.exe @("objcopy", "-O", "binary", $elf, $bin2) | Out-Null
    } else {
        Invoke-Tool $ob.exe @("-O", "binary", $elf, $bin2) | Out-Null
    }
    $bootBin = Join-Path $Build "boot.bin"
    Invoke-Tool $Nasm @("-f","bin", (Join-Path $Root "src\boot\boot.asm"), "-o", $bootBin) | Out-Null
    # 同样要打 'KSCN' 扇区数补丁：不打的话引导扇区退回硬编码的 2048 扇区，
    # 小映像上会一直读到盘尾之外，BIOS 报读盘错误、整机停在 "err"。
    $bootPatched = Join-Path $Build "boot_patched.bin"
    if ($py) {
        Invoke-Tool $py @(
            (Join-Path $Root "tools\gen_instimg.py"),
            "--patch", $bootBin, $bin2, $bootPatched
        ) | Out-Null
    } else {
        Copy-Item $bootBin $bootPatched -Force
    }
    $imgPath = Join-Path $Root $imgName
    $b = [IO.File]::ReadAllBytes($bootPatched)
    if ($b.Length -gt 512) { $b = $b[0..511] }
    $sector = New-Object byte[] 512
    [Array]::Copy($b, $sector, [Math]::Min($b.Length, 512))
    $sector[510] = 0x55; $sector[511] = 0xAA
    $k = [IO.File]::ReadAllBytes($bin2)
    if (($k.Length % 512) -ne 0) {      # 同上：按扇区补齐
        $pad = 512 - ($k.Length % 512)
        $k = $k + (New-Object byte[] $pad)
    }
    $stream = [IO.File]::Create($imgPath)
    $stream.Write($sector, 0, 512); $stream.Write($k, 0, $k.Length); $stream.Close()
    Ok ("映像: $imgPath  (" + (512 + $k.Length) + " 字节 = " + ((512 + $k.Length) / 512) + " 扇区)")
}

# --------------------------- 运行 ---------------------------
if ($Run -or $Headless) {
    Step "启动 QEMU"
    if (-not $Qemu) { throw "未找到 qemu-system-i386。请用 -Qemu <路径> 指定。" }
    $net = @("-netdev","user,id=n0,hostfwd=tcp::2222-:22,hostfwd=tcp::21-:21,hostfwd=tcp::21000-:21000","-device","e1000,netdev=n0")
    $dsk = @()
    if ($Disk) {
        $dimg = Join-Path $Root "tinyos-disk.img"
        if (-not (Test-Path $dimg)) {
            $fsx = [IO.File]::Create($dimg)
            $fsx.SetLength(64MB)          # 64 MB 数据盘
            $fsx.Close()
            Ok "已创建数据盘 $dimg (64 MB，首次格式化)"
        }
        $dsk = @("-drive","file=$dimg,format=raw,if=ide,index=0,media=disk")
        Ok "挂载数据盘: $dimg"
    }
    if ($Headless -or $Debug) {
        Remove-Item (Join-Path $Root "serial.log") -ErrorAction SilentlyContinue
        $args = @("-kernel", $elf, "-m", "128", "-display", "none") + $net + $dsk +
                @("-serial","file:$(Join-Path $Root 'serial.log')","-monitor","none")
        Ok "无窗口启动；串口输出 -> serial.log"
        & $Qemu @args
    } else {
        $args = @("-kernel", $elf, "-m", "128") + $net + $dsk + @("-serial","mon:stdio")
        Ok "图形窗口启动（关闭 QEMU 窗口结束）"
        & $Qemu @args
    }
}

# ------------------- 产物归档到桌面 tinyYY -------------------
# 编译好的东西（可启动映像 / ELF）不留在项目目录里，统一放桌面 tinyYY。
# 各个 .TNCR 由 tools/build_users.py 直接写到 tinyYY\tncr。
$outDir = Join-Path $env:USERPROFILE "Desktop\tinyYY"
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
foreach ($f in @($imgName, "kernel.elf")) {
    $src = Join-Path $Root $f
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $outDir $f) -Force
        Ok ("产物 -> " + (Join-Path $outDir $f))
    }
}

Step "完成"
