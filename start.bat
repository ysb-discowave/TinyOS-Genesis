@echo off
REM ============================================================================
REM  TinyOS Genesis v0.1  --  QEMU launcher
REM  Double-click this file. Boots the OS in a QEMU window.
REM
REM  First run: creates a 64 MB data disk, then the OS runs its setup
REM  wizard (5 steps: disk / network / software / accounts / confirm).
REM  Press Enter for steps 1,2,3,5; at step 4 type a root password
REM  (min 3 chars) twice, then press Enter to skip the extra user.
REM  That password is what you log in with.
REM
REM  Later runs: the disk already exists, so you go straight to login.
REM
REM  Optional: pass your own disk image as the first argument.
REM ============================================================================
setlocal enabledelayedexpansion

cd /d "%~dp0"
REM %CD% 没有尾部反斜杠，所以后面每次拼接都必须自己带 "\"
set "ROOT=%CD%"
set "ELF=%ROOT%\build\kernel.elf"
set "QEMU=C:\Program Files\qemu\qemu-system-i386.exe"

set "DISK=%~1"
if "%DISK%"=="" set "DISK=%ROOT%\tinyos-disk.img"

set "FIRST=0"

echo.
echo ==========================================================
echo    TinyOS Genesis v0.1   --   QEMU launcher
echo ==========================================================
echo.

if not exist "%QEMU%" call :find_qemu
if not exist "%QEMU%" goto no_qemu
if not exist "%ELF%" call :find_kernel
if not exist "%ELF%" goto no_kernel

REM 端口被上一次没退干净的 QEMU 占着时，QEMU 会**直接退出**且窗口一闪就没，
REM 看起来像"启动不了"。这里主动检测并清理。
call :free_ports

if exist "%DISK%" goto boot

set "FIRST=1"
echo  [*] First run: creating a 64 MB disk at
echo        %DISK%
powershell -NoProfile -Command "$f=[IO.File]::Create('%DISK%'); $f.SetLength(64*1048576); $f.Close()"
if not exist "%DISK%" goto nodisk
echo  [+] Disk created.
echo.

:boot
echo  [*] kernel : %ELF%
echo  [*] disk   : %DISK%
echo  [*] memory : 256 MB
if defined NET goto ports_ok
echo  [*] ports  : none ^(forwarding disabled^)
goto ports_done
:ports_ok
echo  [*] ports  : host 2222 -^> OS 22 (SSH),  host 21000 -^> OS 21000 (FTP)
:ports_done
echo.

if "%FIRST%"=="1" goto firstrun
goto launch

:firstrun
echo  ==========================================================
echo    FIRST BOAT -- the setup wizard runs now (5 steps).
echo.
echo      [1/5] Disk       press Enter
echo      [2/5] Network    press Enter
echo      [3/5] Software   press Enter
echo      [4/5] Accounts   type a root password (min 3 chars),
echo                           then type it again, then press
echo                           Enter at "Create a normal user?"
echo      [5/5] Confirm    press Enter
echo.
echo    It takes about a minute. Remember the password you typed --
echo    that is what you log in with.
echo  ==========================================================
echo.

:launch
if defined NET goto launch_net
start "TinyOS Genesis v0.1" "%QEMU%" -kernel "%ELF%" -m 256 -drive "file=%DISK%,format=raw,if=ide,index=0,media=disk" -serial "mon:stdio" -monitor none
goto launched
:launch_net
start "TinyOS Genesis v0.1" "%QEMU%" -kernel "%ELF%" -m 256 -drive "file=%DISK%,format=raw,if=ide,index=0,media=disk" -netdev "user,id=n0,hostfwd=tcp::2222-:22,hostfwd=tcp::21000-:21000" -device e1000,netdev=n0 -serial "mon:stdio" -monitor none
:launched

echo.
echo  [i] The QEMU window was closed -- TinyOS has stopped.
echo.
pause
goto done

:no_qemu
echo  [X] Could not find QEMU (qemu-system-i386.exe).
echo      Looked for: %QEMU%
echo      Also searched PATH and the usual install folders.
echo.
echo      Install it from https://www.qemu.org/download/
echo      or edit the QEMU= line near the top of this file.
echo.
pause
goto done

REM ---------------------------------------------------------------------------
REM  在 PATH 与常见安装位置搜索 QEMU
REM ---------------------------------------------------------------------------
:find_qemu
for %%Q in (
    "C:\Program Files\qemu\qemu-system-i386.exe"
    "C:\Program Files (x86)\qemu\qemu-system-i386.exe"
    "%LOCALAPPDATA%\Programs\qemu\qemu-system-i386.exe"
) do if exist "%%~Q" set "QEMU=%%~Q"
REM 再从 PATH 里找
for /f "delims=" %%Q in ('where qemu-system-i386.exe 2^>nul') do if exist "%%Q" set "QEMU=%%Q"
if exist "%QEMU%" echo  [+] Found QEMU: %QEMU%
goto :eof

:no_kernel
echo  [X] Could not find the kernel image.
echo.
echo      Looked for: %ELF%
echo.
echo      Searched these places:
echo        - this script's own folder
echo        - %USERPROFILE%\Desktop\tinyYY
echo        - the project's build\ folder
echo.
echo      If you have not built it yet:
echo          cd /d "%ROOT%"
echo          python tools\build_users.py
echo          powershell -ExecutionPolicy Bypass -File build.ps1
echo.
pause
goto done

REM ---------------------------------------------------------------------------
REM  在常见位置搜索 kernel.elf
REM ---------------------------------------------------------------------------
:find_kernel
echo  [*] Kernel not next to this script; searching...
for %%P in (
    "%ROOT%\build"
    "%USERPROFILE%\Desktop\tinyYY"
    "%ROOT%"
    "%USERPROFILE%\WorkBuddy"
) do if exist "%%~P\kernel.elf" set "ELF=%%~P\kernel.elf"
if exist "%ELF%" echo  [+] Found kernel: %ELF%
goto :eof

REM ---------------------------------------------------------------------------
REM  端口体检
REM  上一次 QEMU 没退干净时，2222/21000 会被它占着；此时再启动一个 QEMU
REM  会报 "Could not set up host forwarding rule" 然后**立刻退出**，
REM  表现就是窗口一闪就没、看起来"根本启动不了"。
REM  这里主动找出占用者并结束掉。
REM ---------------------------------------------------------------------------
:free_ports
set "BUSY="
for %%P in (2222 21000) do call :check_port %%P
if not defined BUSY goto ports_clear

echo  [*] Ports in use:!BUSY!
echo      A previous QEMU is probably still running. Stopping it...
taskkill /F /IM qemu-system-i386.exe >nul 2>&1
timeout /t 2 /nobreak >nul
set "BUSY="
for %%P in (2222 21000) do call :check_port %%P
if not defined BUSY goto ports_clear

echo  [!] Ports!BUSY! are still busy -- something else is using them.
echo      Starting WITHOUT SSH/FTP port forwarding.
echo      If TinyOS fails to boot, close that program and try again.
set "NET="
goto :eof

:ports_clear
set "NET=-netdev user,id=n0,hostfwd=tcp::2222-:22,hostfwd=tcp::21000-:21000 -device e1000,netdev=n0"
goto :eof

:check_port
netstat -ano 2>nul | find "LISTENING" | find ":%1 " >nul 2>&1
if not errorlevel 1 set "BUSY=!BUSY! %1"
goto :eof

:nodisk
echo  [X] Could not create the disk file: %DISK%
echo.
pause
goto done

:done
endlocal
