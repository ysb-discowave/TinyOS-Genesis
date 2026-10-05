# ============================================================================
# TinyOS 工具链准备脚本 (Windows)
# ----------------------------------------------------------------------------
# 自动准备三件套：
#   1) NASM      —— 汇编
#   2) Zig       —— 自带 clang + lld，用于交叉编译 i686 裸机代码
#   3) QEMU      —— 运行/调试
# 优先用 winget 安装；不可用时回退到直接下载（国内可访问的镜像/官网）。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools/setup-toolchain.ps1
# ============================================================================
$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'

$root    = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$toolDir = Join-Path $root ".toolchain"
New-Item -ItemType Directory -Force -Path $toolDir | Out-Null

function Have($name) { return [bool](Get-Command $name -ErrorAction SilentlyContinue) }
function Log($m) { Write-Host "[setup] $m" }

# ---------------------------------------------------------------- NASM
if (Have nasm) {
    Log "NASM 已存在: $((Get-Command nasm).Source)"
} else {
    Log "安装 NASM ..."
    if (Have winget) {
        winget install --id NASM.NASM -e --accept-source-agreements --accept-package-agreements --disable-interactivity
    }
    if (-not (Have nasm)) {
        $zip = Join-Path $toolDir "nasm.zip"
        Log "直接下载 NASM ..."
        Invoke-WebRequest "https://www.nasm.us/pub/nasm/releasebuilds/3.02/win64/nasm-3.02-win64.zip" -OutFile $zip
        Expand-Archive -Path $zip -DestinationPath $toolDir -Force
    }
}

# ---------------------------------------------------------------- Zig
$zigExe = Join-Path $toolDir "zig\zig.exe"
if (Have zig) {
    Log "Zig 已存在: $((Get-Command zig).Source)"
} elseif (Test-Path $zigExe) {
    Log "Zig 已存在于 $zigExe"
} else {
    Log "下载 Zig（自带 clang/lld，用于交叉编译 i686）..."
    $zigZip = Join-Path $toolDir "zig.zip"
    $url = "https://ziglang.org/download/0.14.0/zig-windows-x86_64-0.14.0.zip"
    try {
        Invoke-WebRequest $url -OutFile $zigZip -TimeoutSec 3600
    } catch {
        Log "ziglang.org 下载失败，尝试镜像 ..."
        Invoke-WebRequest "https://mirrors.tuna.tsinghua.edu.cn/zig/zig-windows-x86_64-0.14.0.zip" -OutFile $zigZip -TimeoutSec 3600
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $toolDir "zig") | Out-Null
    Expand-Archive -Path $zigZip -DestinationPath (Join-Path $toolDir "zig_tmp") -Force
    $inner = Get-ChildItem (Join-Path $toolDir "zig_tmp") -Directory | Select-Object -First 1
    Copy-Item (Join-Path $inner.FullName "*") (Join-Path $toolDir "zig") -Recurse -Force
    Remove-Item (Join-Path $toolDir "zig_tmp") -Recurse -Force
    Log "Zig 解压到 $zigExe"
}

# ---------------------------------------------------------------- QEMU
if (Have qemu-system-i386) {
    Log "QEMU 已存在: $((Get-Command qemu-system-i386).Source)"
} else {
    Log "安装 QEMU ..."
    if (Have winget) {
        winget install --id SoftwareFreedomConservancy.QEMU -e --accept-source-agreements --accept-package-agreements --disable-interactivity
    }
    if (-not (Have qemu-system-i386)) {
        Log "请手动安装 QEMU: https://www.qemu.org/download/#windows"
    }
}

Log ""
Log "================ 结果 ================"
Log "nasm  : $((Get-Command nasm -ErrorAction SilentlyContinue).Source)"
Log "zig   : $((Get-Command zig  -ErrorAction SilentlyContinue).Source)"
Log "qemu  : $((Get-Command qemu-system-i386 -ErrorAction SilentlyContinue).Source)"
Log ""
Log "若 zig 不在 PATH，构建时这样指定："
Log "  make CC=`"$zigExe cc --target=x86-freestanding`" NASM=`"$toolDir\nasm\nasm.exe`""
Log ""
Log "接下来:"
Log "  python tools/build_users.py"
Log "  make CC=`"...zig cc --target=x86-freestanding`""
