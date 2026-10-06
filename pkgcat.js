/* ============================================================
   TinyOS Genesis v0.1 — package catalogue (pkgcat)
   数据来自 packages/ 下真实的 manifest 与 romfs 里的真实字节数。
   交互：点卡片 -> 展开该包的详情面板；详情面板内的 tab 切换
   「概览 / 清单 / 说明」。纯原生 JS，无依赖。
   ============================================================ */
(function () {
  'use strict';

  var root = document.getElementById('pkgcat');
  if (!root) return; // 页面没有这个区块就静默跳过

  var STORAGE_KEY = 'tinyos-lang';

  function lang() {
    try {
      var v = window.localStorage.getItem(STORAGE_KEY);
      return (v === 'en') ? 'en' : 'zh';
    } catch (e) { return 'zh'; }
  }

  /* ------------------------------------------------------------
     包数据。字段全部来自仓库真实文件，不是编的。
     size = 字节数；sha 为 manifest 里的值（__BUILD_FILL__ 表示未填）
     ------------------------------------------------------------ */
  var PKGS = [
    {
      id: 'tinysh',
      state: 'ready',
      dir: 'packages/tinysh/',
      out: '/bin/tinysh.TNCR',
      size: 179104,
      sha: '4c317df355a489aeea5d6f26447f0a0472c67afc7c83b2f4fa4c8229f567f2bc',
      files: [
        { n: 'src/tinysh.c', l: 263, note: '命令分发与 22 条命令的实现（含 pkg / pack / run）' },
        { n: 'src/kernel_api_tinyos.c', l: 256, note: 'TNCR 后端，把 kernel_api 转成 tinyos_api_t 调用' },
        { n: 'src/kernel_api_host.c', l: 264, note: 'POSIX 宿主后端，供离线自测，带 main()' },
        { n: 'src/tinysh.h', l: 54, note: '错误码枚举、命令表结构、历史缓冲' },
        { n: 'src/kernel_api.h', l: 53, note: '后端无关的唯一契约：ksh_/fs_/proc_/hw_/sys_' },
        { n: 'src/build_tinysh.py', l: 77, note: 'zig 交叉编译成 TNCR，打进 romfs' },
        { n: 'src/Makefile', l: 38, note: '宿主侧便捷构建' },
        { n: 'src/README.md', l: 72, note: '包说明' },
        { n: 'src/test_session.txt', l: 14, note: '宿主后端冒烟测试用的输入脚本' }
      ],
      descZh: '系统默认 Shell，登录后自动进入。22 条内置命令，外加一条三级命令查找链：内置 → 内核命令（45 个）→ /bin 下的程序。',
      descEn: 'The default shell, started automatically after login. 22 built-in commands plus a three-level lookup chain: builtins, then the 45 kernel commands, then programs under /bin.',
      detailZh: [
        'tinysh 是真正的用户态 TNCR 程序，不是内核内建。它只能通过 tinyos_api_t 函数指针表访问内核，自己一行内核代码都碰不到。',
        '命令查找链是它最有价值的设计：tinysh 内置 22 个命令，但内核有 45 个。敲 uname / mem / disk / lua / edit 时，内置表里没有，就转发给内核执行；再找不到就去 /bin 找同名 .TNCR，把参数拼好交给它运行。',
        'pkg 子命令也在这里面 —— 它需要 shell、配置解析和大量文件读写，这些都不该在内核里，所以内核只暴露了 sha256_file 这一个原语。'
      ],
      detailEn: [
        'tinysh is a real user-space TNCR program, not a kernel builtin. It reaches the kernel only through the tinyos_api_t function-pointer table.',
        'The lookup chain is its most valuable feature: tinysh has 22 built-ins while the kernel has 45. Type uname, mem, disk, lua or edit, miss the builtin table, and the call is forwarded to the kernel. If that misses too, /bin is searched for a matching .TNCR and the arguments are handed to it.',
        'The pkg subcommand lives here too. It needs a shell, config parsing and heavy file I/O, none of which belong in the kernel, so the kernel exposes exactly one primitive: sha256_file.'
      ]
    },
    {
      id: 'demos',
      state: 'index',
      dir: 'packages/demos/',
      out: '/bin/*.TNCR (8 个)',
      size: null,
      sha: '__BUILD_FILL__',
      files: [
        { n: 'demos.manifest', l: null, note: '含 programs 段，逐个列出 8 个示例及其字节数' },
        { n: 'src/README.md', l: null, note: '逐个说明每个示例演示什么' }
      ],
      descZh: '演示程序索引包。8 个示例由 tools/build_users.py 编译进 romfs，本包只作说明与索引，没有单一可安装产物。',
      descEn: 'An index package for the demo programs. The eight samples are compiled into romfs by tools/build_users.py; this package only documents them and has no single installable artifact.',
      detailZh: [
        '这是一个「说明包」：demos.manifest 里的 sha256 仍是 __BUILD_FILL__，因为没有可供 pkg 落盘的单一产物 —— 8 个 .tncr 是分别编译、分別进 romfs 的。',
        '值得留意的是这些示例的语言分布：hello 与 count 是 C，basic_demo 由 TinyBASIC 解释器运行，cpp_demo 验证 C++ 工具链，minic_demo 是 MiniC 编译器产物，tiny_demo 走 TinyLang，net_demo 演示网络栈，gui_demo 带 --gui。'
      ],
      detailEn: [
        'This is a documentation package: sha256 in demos.manifest is still __BUILD_FILL__ because there is no single artifact for pkg to install. The eight .tncr files are compiled separately into romfs.',
        'Note the language spread: hello and count are C, basic_demo runs under the TinyBASIC interpreter, cpp_demo exercises the C++ toolchain, minic_demo is MiniC compiler output, tiny_demo uses TinyLang, net_demo shows the network stack, and gui_demo takes --gui.'
      ]
    },
    {
      id: 'tncr',
      state: 'planned',
      dir: 'packages/tncr/',
      out: '/bin/tncr.tncr',
      size: null,
      sha: '__BUILD_FILL__',
      files: [
        { n: 'tncr.manifest', l: null, note: 'build.cmd 是注释：规划中的 tncr build 调用' },
        { n: 'src/README.md', l: null, note: '说明缺什么' }
      ],
      descZh: 'TNCR 编译器前端，规划中。运行时（内核侧 tncr.c）已经可用，缺的是把 .tncs 编译成 .TNCR 的编译器。',
      descEn: 'The TNCR compiler front end, planned. The runtime (kernel-side tncr.c) already works; what is missing is the compiler that turns .tncs into .TNCR.',
      detailZh: [
        '这个包常被误解：TNCR 的运行时是已经能跑的 —— 内核里的 tncr.c 负责加载并执行 .TNCR，tinysh、Lua、CC 这些程序每天都靠它运行。',
        '缺的是前端：目前所有 .TNCR 都由 tools/ 下的 Python + zig 脚本直接交叉编译产出，还没有一个在 TinyOS 内部把源码编译成 .TNCR 的工具。',
        'manifest 里的 build.cmd 故意写成注释（# 规划中：tncr build src/main.tncs -o /bin/tncr.tncr），提醒使用者这条命令还不存在。'
      ],
      detailEn: [
        'This package is often misunderstood: the TNCR runtime already works. tncr.c in the kernel loads and executes .TNCR files, and tinysh, Lua and CC rely on it every day.',
        'What is missing is the front end. Today every .TNCR is produced by the Python + zig scripts under tools/; there is no compiler inside TinyOS that turns source into .TNCR.',
        'build.cmd in the manifest is deliberately a comment, to make clear the command does not exist yet.'
      ]
    },
    {
      id: 'web-admin-panel',
      state: 'planned',
      dir: 'packages/web-admin-panel/',
      out: '/bin/web-admin-panel.tncr',
      size: null,
      sha: '__BUILD_FILL__',
      files: [
        { n: 'web-admin-panel.manifest', l: null, note: 'build.cmd 是注释：由 TinyOS 自研 Web 引擎托管' },
        { n: 'src/README.md', l: null, note: '说明缺什么' }
      ],
      descZh: '官方 Web 管理面板示例，规划中。目标是让 TinyOS 在浏览器里可管理，产物由系统自带的 Web 引擎托管。',
      descEn: 'The official web management panel example, planned. The goal is to make TinyOS manageable from a browser, with the artifact hosted by the built-in web engine.',
      detailZh: [
        '这一包还处在最早期：只有 manifest 和一份说明为什么还没写的 README。',
        '它的价值在于把 TinyOS 的网络栈（FTP / TinySMB / TCP socket）包装成一个真正能用的管理界面，而不是只能靠 telnet 敲命令。',
        '需要提醒的是：早期项目资料里曾描述过「浏览器在线体验 TinyOS」，那是**不实**的 —— 内核里没有 Web 服务端，这个包正是为了让那种体验有朝一日成立。'
      ],
      detailEn: [
        'This package is at the earliest stage: a manifest and a README explaining why it is not written yet.',
        'Its value lies in wrapping the network stack (FTP, TinySMB, TCP sockets) into a real management interface instead of nothing but a telnet prompt.',
        'Worth noting: early project material described a "browser experience of TinyOS". That was untrue. The kernel has no web server; this package is what would make such an experience possible one day.'
      ]
    }
  ];

  /* PKGREPO:BEGIN —— 由 tools/sync_site_facts.py 从 packages/repo/ 自动生成，勿手改。
     这份数据是"仓库里真实存在、pkg 真能装的包"：名字、字节数、sha256 全部取自
     真实产物文件。上面 PKGS 里手写的条目提供更详细的逐文件说明与深挖内容。 */
  var PKGS_REPO = [
        { id: 'CC', state: 'ready', dir: 'packages/repo/',
          out: '/bin/CC.TNCR', size: 162680,
          sha: '2cd651fb19122096d1c79a93bd7c131e8b2a143dbac742b8f7f18b8e8ad8f401',
          descZh: 'C 编译器 / 工具链前端（tncr 交叉编译）。把 C 源码编译成 TinyOS 原生可执行文件。', descEn: 'The C compiler / toolchain front end (tncr cross-compiler). Compiles C sources into native TinyOS executables.',
          detailZh: ['C 编译器 / 工具链前端（tncr 交叉编译）。把 C 源码编译成 TinyOS 原生可执行文件。', '安装：pkg install CC —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/CC.TNCR。', '用法：安装后在 tinysh 里敲 cc 使用。', '产物 162680 字节，sha256 前 16 位 2cd651fb19122096…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['The C compiler / toolchain front end (tncr cross-compiler). Compiles C sources into native TinyOS executables.', 'Install: pkg install CC — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/CC.TNCR.', 'Usage: Once installed, invoke it as cc from tinysh.', 'Artifact: 162680 bytes, sha256 starts with 2cd651fb19122096…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'CC.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'CC.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/cc.c', l: 1885, note: '构建时使用的真实源文件' },
            { n: 'tools/compiler/tcc.py', l: 535, note: '构建时使用的真实源文件' }
          ] },
        { id: 'DESKTOP', state: 'ready', dir: 'packages/repo/',
          out: '/bin/DESKTOP.TNCR', size: 34,
          sha: '02b93608bb92b3c652b45a734de43435c726419faa6f7454bd8ed14714f375da',
          descZh: '图形文本桌面启动器。由 tcc 在构建时生成（带 DESKTOP 标志），进入基于 VGA 文本模式与 PS/2 键鼠的桌面。', descEn: 'The graphical text-mode desktop launcher. Generated by tcc at build time (with the DESKTOP flag); enters a desktop built on VGA text mode with PS/2 keyboard and mouse.',
          detailZh: ['图形文本桌面启动器。由 tcc 在构建时生成（带 DESKTOP 标志），进入基于 VGA 文本模式与 PS/2 键鼠的桌面。', '安装：pkg install DESKTOP —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/DESKTOP.TNCR。', '用法：在 tinysh 里敲 run DESKTOP，或直接运行 /DESKTOP.TNCR。', '产物 34 字节，sha256 前 16 位 02b93608bb92b3c6…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['The graphical text-mode desktop launcher. Generated by tcc at build time (with the DESKTOP flag); enters a desktop built on VGA text mode with PS/2 keyboard and mouse.', 'Install: pkg install DESKTOP — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/DESKTOP.TNCR.', 'Usage: Type run DESKTOP in tinysh, or execute /DESKTOP.TNCR directly.', 'Artifact: 34 bytes, sha256 starts with 02b93608bb92b3c6…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'DESKTOP.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'DESKTOP.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' }
          ] },
        { id: 'EDIT', state: 'ready', dir: 'packages/repo/',
          out: '/bin/EDIT.TNCR', size: 45980,
          sha: '0d00374174c799423b56f59e8f6646040a37fe788c96b358ea41677b58ecf37c',
          descZh: '文本编辑器，运行在 VGA 文本模式与 PS/2 键盘之上。', descEn: 'A text editor running on the VGA text mode with a PS/2 keyboard.',
          detailZh: ['文本编辑器，运行在 VGA 文本模式与 PS/2 键盘之上。', '安装：pkg install EDIT —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/EDIT.TNCR。', '用法：安装后敲 edit <文件> 打开。', '产物 45980 字节，sha256 前 16 位 0d00374174c79942…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['A text editor running on the VGA text mode with a PS/2 keyboard.', 'Install: pkg install EDIT — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/EDIT.TNCR.', 'Usage: Once installed, run edit <file>.', 'Artifact: 45980 bytes, sha256 starts with 0d00374174c79942…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'EDIT.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'EDIT.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/edit.c', l: 539, note: '构建时使用的真实源文件' }
          ] },
        { id: 'JVM', state: 'ready', dir: 'packages/repo/',
          out: '/bin/JVM.TNCR', size: 5851,
          sha: '152792856b7d854182b794eac2f2bc8fb667e90c1c0d2b7d806470dd926dcd17',
          descZh: 'JVM 运行时。产物由外部 TinyOS-JVM 项目的 build.py 生成，再打进 romfs。', descEn: 'A JVM runtime. The artifact is produced by the external TinyOS-JVM project\'s build.py and then embedded into romfs.',
          detailZh: ['JVM 运行时。产物由外部 TinyOS-JVM 项目的 build.py 生成，再打进 romfs。', '安装：pkg install JVM —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/JVM.TNCR。', '用法：由安装向导的可选组件 [7] 决定是否写盘。', '产物 5851 字节，sha256 前 16 位 152792856b7d8541…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['A JVM runtime. The artifact is produced by the external TinyOS-JVM project\'s build.py and then embedded into romfs.', 'Install: pkg install JVM — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/JVM.TNCR.', 'Usage: Whether it is written to disk is decided by optional component [7] of the install wizard.', 'Artifact: 5851 bytes, sha256 starts with 152792856b7d8541…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'JVM.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'JVM.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' }
          ] },
        { id: 'LUA', state: 'ready', dir: 'packages/repo/',
          out: '/bin/LUA.TNCR', size: 210584,
          sha: '8294b5a55b4ab7bcd72a7e9aa0198888ff3f9d8eb9ee28bcd026e7a899a0fab1',
          descZh: 'Lua 5.4.7 解释器，编译成 TNCR 跑在用户态。支持 REPL 交互与脚本模式。', descEn: 'The Lua 5.4.7 interpreter, compiled to TNCR and running in user space. Both an interactive REPL and script mode.',
          detailZh: ['Lua 5.4.7 解释器，编译成 TNCR 跑在用户态。支持 REPL 交互与脚本模式。', '安装：pkg install LUA —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/LUA.TNCR。', '用法：敲 lua 进 REPL（:quit 退出）；lua /home/x.lua 直接跑脚本。', '产物 210584 字节，sha256 前 16 位 8294b5a55b4ab7bc…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['The Lua 5.4.7 interpreter, compiled to TNCR and running in user space. Both an interactive REPL and script mode.', 'Install: pkg install LUA — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/LUA.TNCR.', 'Usage: Run lua for the REPL (:quit to leave) or lua /home/x.lua to run a script.', 'Artifact: 210584 bytes, sha256 starts with 8294b5a55b4ab7bc…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'LUA.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'LUA.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/lua/lua_main.c', l: 294, note: '构建时使用的真实源文件' },
            { n: 'tools/lua/lua_package.c', l: 97, note: '构建时使用的真实源文件' },
            { n: 'tools/lua/lua_shim.c', l: 944, note: '构建时使用的真实源文件' }
          ] },
        { id: 'SSHD', state: 'ready', dir: 'packages/repo/',
          out: '/bin/SSHD.TNCR', size: 114500,
          sha: '2507b0deaf22551b65eb85e276ba30aab949974430d6a876d64549e95eac8d02',
          descZh: 'SSH-2 服务端（curve25519-sha256 + ed25519 主机密钥），让外部机器能登进 TinyOS。', descEn: 'An SSH-2 server (curve25519-sha256 with an ed25519 host key) that lets an outside machine log into TinyOS.',
          detailZh: ['SSH-2 服务端（curve25519-sha256 + ed25519 主机密钥），让外部机器能登进 TinyOS。', '安装：pkg install SSHD —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/SSHD.TNCR。', '用法：安装后由 tinysh 启动；与内核的 passwd 账户体系配合。', '产物 114500 字节，sha256 前 16 位 2507b0deaf22551b…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['An SSH-2 server (curve25519-sha256 with an ed25519 host key) that lets an outside machine log into TinyOS.', 'Install: pkg install SSHD — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/SSHD.TNCR.', 'Usage: Started from tinysh once installed; it works with the kernel passwd account system.', 'Artifact: 114500 bytes, sha256 starts with 2507b0deaf22551b…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'SSHD.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'SSHD.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/sshd.c', l: 1010, note: '构建时使用的真实源文件' }
          ] },
        { id: 'SSHCLIENT', state: 'ready', dir: 'packages/repo/',
          out: '/bin/SSHCLIENT.TNCR', size: 57772,
          sha: 'da04c31318bbc970bbeba7844cc8225d8ad6511303fe8957bb04efaaffe625e3',
          descZh: 'SSH-2 客户端：TinyOS 主动连外部 SSH 服务器。传输层与服务端同构（curve25519-sha256 ECDH、AES-128-CTR、HMAC-SHA2-256），走 ssh-userauth 的 password 认证。内核无 pty，因此不申请伪终端，只能用 exec 跑命令或简易逐行 shell。', descEn: 'An SSH-2 client: TinyOS dials out to an external SSH server. The transport mirrors the server side (curve25519-sha256 ECDH, AES-128-CTR, HMAC-SHA2-256) and authenticates via ssh-userauth password. The kernel has no pty, so it never requests a pseudo-terminal and can only run commands via exec or a simple line-based shell.',
          detailZh: ['SSH-2 客户端：TinyOS 主动连外部 SSH 服务器。传输层与服务端同构（curve25519-sha256 ECDH、AES-128-CTR、HMAC-SHA2-256），走 ssh-userauth 的 password 认证。内核无 pty，因此不申请伪终端，只能用 exec 跑命令或简易逐行 shell。', '安装：pkg install SSHCLIENT —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/SSHCLIENT.TNCR。', '用法：SSHCLIENT <ip> [port] [command...] —— 给了 command 就跑完退出，不给就进简易交互 shell。主机只接受点分十进制 IPv4（内核无 DNS）。', '产物 57772 字节，sha256 前 16 位 da04c31318bbc970…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['An SSH-2 client: TinyOS dials out to an external SSH server. The transport mirrors the server side (curve25519-sha256 ECDH, AES-128-CTR, HMAC-SHA2-256) and authenticates via ssh-userauth password. The kernel has no pty, so it never requests a pseudo-terminal and can only run commands via exec or a simple line-based shell.', 'Install: pkg install SSHCLIENT — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/SSHCLIENT.TNCR.', 'Usage: SSHCLIENT <ip> [port] [command...] — with a command it runs and exits; without one it drops into a simple interactive shell. Hosts must be dotted-quad IPv4 (the kernel has no DNS).', 'Artifact: 57772 bytes, sha256 starts with da04c31318bbc970…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'SSHCLIENT.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'SSHCLIENT.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/ssh_client.c', l: 764, note: '构建时使用的真实源文件' },
            { n: 'tools/compiler/sshd_crypto.h', l: 820, note: '构建时使用的真实源文件' }
          ] },
        { id: 'basic_demo', state: 'ready', dir: 'packages/repo/',
          out: '/bin/basic_demo.TNCR', size: 204,
          sha: 'b9e6faf53975ca7b7fad32d0b51471c31c22216e56cc8fff96e2d0601dfa4193',
          descZh: '.bas 语言的演示程序，由 tcc 的 basic 前端编译。', descEn: 'A demo for the .bas language, compiled by tcc\'s basic front end.',
          detailZh: ['.bas 语言的演示程序，由 tcc 的 basic 前端编译。', '安装：pkg install basic_demo —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/basic_demo.TNCR。', '用法：敲 basic_demo 运行。', '产物 204 字节，sha256 前 16 位 b9e6faf53975ca7b…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['A demo for the .bas language, compiled by tcc\'s basic front end.', 'Install: pkg install basic_demo — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/basic_demo.TNCR.', 'Usage: Run basic_demo.', 'Artifact: 204 bytes, sha256 starts with b9e6faf53975ca7b…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'basic_demo.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'basic_demo.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/loop.bas', l: 16, note: '构建时使用的真实源文件' }
          ] },
        { id: 'count', state: 'ready', dir: 'packages/repo/',
          out: '/bin/count.TNCR', size: 877,
          sha: '347c41ce0a4f48c5f28a4bbe3bab3788c87f800076ed19b0d8129c8859e5e8ac',
          descZh: '计数演示程序，展示用户态程序的运行与输出。', descEn: 'A counting demo showing a user-space program running and printing.',
          detailZh: ['计数演示程序，展示用户态程序的运行与输出。', '安装：pkg install count —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/count.TNCR。', '用法：敲 count 运行。', '产物 877 字节，sha256 前 16 位 347c41ce0a4f48c5…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['A counting demo showing a user-space program running and printing.', 'Install: pkg install count — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/count.TNCR.', 'Usage: Run count.', 'Artifact: 877 bytes, sha256 starts with 347c41ce0a4f48c5…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'count.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'count.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/count.c', l: 26, note: '构建时使用的真实源文件' }
          ] },
        { id: 'cpp_demo', state: 'ready', dir: 'packages/repo/',
          out: '/bin/cpp_demo.TNCR', size: 339,
          sha: 'b3c76dd35c1c9abe1178226b24368f2883cf1f5ef97b4889249a0f306a5680d8',
          descZh: 'C++ 演示程序，验证 tcc 的 C++ 前端（带 TNCR_F_GUI 判定）。', descEn: 'A C++ demo exercising tcc\'s C++ front end (with the TNCR_F_GUI check).',
          detailZh: ['C++ 演示程序，验证 tcc 的 C++ 前端（带 TNCR_F_GUI 判定）。', '安装：pkg install cpp_demo —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/cpp_demo.TNCR。', '用法：敲 cpp_demo 运行。', '产物 339 字节，sha256 前 16 位 b3c76dd35c1c9abe…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['A C++ demo exercising tcc\'s C++ front end (with the TNCR_F_GUI check).', 'Install: pkg install cpp_demo — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/cpp_demo.TNCR.', 'Usage: Run cpp_demo.', 'Artifact: 339 bytes, sha256 starts with b3c76dd35c1c9abe…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'cpp_demo.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'cpp_demo.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/hello.cpp', l: 37, note: '构建时使用的真实源文件' }
          ] },
        { id: 'gui_demo', state: 'ready', dir: 'packages/repo/',
          out: '/bin/gui_demo.TNCR', size: 470,
          sha: 'e553e1be79b1da419fe2f0ccd784230dd534157a5ff66aa6c7739372ed48c281',
          descZh: 'GUI 演示：带 --gui 标志构建，演示 TNCR 的 GUI 标志位。', descEn: 'A GUI demo built with the --gui flag, showing the TNCR GUI flag bit.',
          detailZh: ['GUI 演示：带 --gui 标志构建，演示 TNCR 的 GUI 标志位。', '安装：pkg install gui_demo —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/gui_demo.TNCR。', '用法：敲 gui_demo 运行。', '产物 470 字节，sha256 前 16 位 e553e1be79b1da41…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['A GUI demo built with the --gui flag, showing the TNCR GUI flag bit.', 'Install: pkg install gui_demo — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/gui_demo.TNCR.', 'Usage: Run gui_demo.', 'Artifact: 470 bytes, sha256 starts with e553e1be79b1da41…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'gui_demo.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'gui_demo.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/gui_demo.c', l: 24, note: '构建时使用的真实源文件' }
          ] },
        { id: 'hello', state: 'ready', dir: 'packages/repo/',
          out: '/bin/hello.TNCR', size: 214,
          sha: '050668ab67b7743b5e50372f07e95be433bf14234abe52267fbf19170d5ba492',
          descZh: '最小化 C 程序，用来验证 C 工具链与 TNCR 运行时是否正常。', descEn: 'A minimal C program used to verify the C toolchain and the TNCR runtime.',
          detailZh: ['最小化 C 程序，用来验证 C 工具链与 TNCR 运行时是否正常。', '安装：pkg install hello —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/hello.TNCR。', '用法：敲 hello 运行，应打印一行问候。', '产物 214 字节，sha256 前 16 位 050668ab67b7743b…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['A minimal C program used to verify the C toolchain and the TNCR runtime.', 'Install: pkg install hello — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/hello.TNCR.', 'Usage: Run hello; it should print a greeting.', 'Artifact: 214 bytes, sha256 starts with 050668ab67b7743b…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'hello.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'hello.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/hello.c', l: 14, note: '构建时使用的真实源文件' }
          ] },
        { id: 'minic_demo', state: 'ready', dir: 'packages/repo/',
          out: '/bin/minic_demo.TNCR', size: 500,
          sha: 'ca92e2c0b062ee0605499b8e058db8aeaa62e4c1e3e84d6ed21adacf6ecf6aae',
          descZh: '.minic 语言的演示程序，由 tcc 的 minic 前端编译。', descEn: 'A demo for the .minic language, compiled by tcc\'s minic front end.',
          detailZh: ['.minic 语言的演示程序，由 tcc 的 minic 前端编译。', '安装：pkg install minic_demo —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/minic_demo.TNCR。', '用法：敲 minic_demo 运行。', '产物 500 字节，sha256 前 16 位 ca92e2c0b062ee06…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['A demo for the .minic language, compiled by tcc\'s minic front end.', 'Install: pkg install minic_demo — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/minic_demo.TNCR.', 'Usage: Run minic_demo.', 'Artifact: 500 bytes, sha256 starts with ca92e2c0b062ee06…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'minic_demo.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'minic_demo.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/calc.minic', l: 24, note: '构建时使用的真实源文件' }
          ] },
        { id: 'net_demo', state: 'ready', dir: 'packages/repo/',
          out: '/bin/net_demo.TNCR', size: 467,
          sha: '3d41ba3758a1539440b8de1e27a5c22ac91803fabf1898a04521f0090c5f6e16',
          descZh: '网络演示：展示用户态程序如何做网络 I/O。', descEn: 'A networking demo showing how a user-space program does network I/O.',
          detailZh: ['网络演示：展示用户态程序如何做网络 I/O。', '安装：pkg install net_demo —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/net_demo.TNCR。', '用法：敲 net_demo 运行。', '产物 467 字节，sha256 前 16 位 3d41ba3758a15394…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['A networking demo showing how a user-space program does network I/O.', 'Install: pkg install net_demo — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/net_demo.TNCR.', 'Usage: Run net_demo.', 'Artifact: 467 bytes, sha256 starts with 3d41ba3758a15394…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'net_demo.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'net_demo.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/net_demo.c', l: 24, note: '构建时使用的真实源文件' }
          ] },
        { id: 'tiny_demo', state: 'ready', dir: 'packages/repo/',
          out: '/bin/tiny_demo.TNCR', size: 238,
          sha: 'dfdefb943aa31c6d66b415848e7cc1c7b3a4f4c9a069414de9e88130c0cdfdf3',
          descZh: '.tiny 语言的演示程序，由 tcc 的 tiny 前端编译。', descEn: 'A demo for the .tiny language, compiled by tcc\'s tiny front end.',
          detailZh: ['.tiny 语言的演示程序，由 tcc 的 tiny 前端编译。', '安装：pkg install tiny_demo —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/tiny_demo.TNCR。', '用法：敲 tiny_demo 运行。', '产物 238 字节，sha256 前 16 位 dfdefb943aa31c6d…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['A demo for the .tiny language, compiled by tcc\'s tiny front end.', 'Install: pkg install tiny_demo — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/tiny_demo.TNCR.', 'Usage: Run tiny_demo.', 'Artifact: 238 bytes, sha256 starts with dfdefb943aa31c6d…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'tiny_demo.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'tiny_demo.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/compiler/examples/hello.tiny', l: 21, note: '构建时使用的真实源文件' }
          ] },
        { id: 'tinysh', state: 'ready', dir: 'packages/repo/',
          out: '/bin/tinysh.TNCR', size: 179104,
          sha: '4c317df355a489aeea5d6f26447f0a0472c67afc7c83b2f4fa4c8229f567f2bc',
          descZh: '系统默认 Shell，登录后自动进入。22 条内置命令，外加一条三级命令查找链：内置 → 内核命令（45 个）→ /bin 下的程序。', descEn: 'The default shell, started automatically after login. 22 built-in commands plus a three-level lookup chain: builtins, then the 45 kernel commands, then programs under /bin.',
          detailZh: ['系统默认 Shell，登录后自动进入。22 条内置命令，外加一条三级命令查找链：内置 → 内核命令（45 个）→ /bin 下的程序。', '安装：pkg install tinysh —— 按 sourcedir → FTP → HTTP(Worker) 顺序找源，下载后用 sha256 校验，通过则写入 /bin/tinysh.TNCR。', '用法：直接在提示符敲命令；help 列出全部命令，exit 退回内核 shell。', '产物 179104 字节，sha256 前 16 位 4c317df355a489ae…；完整清单见上方表格与 packages/repo/INDEX.json。'],
          detailEn: ['The default shell, started automatically after login. 22 built-in commands plus a three-level lookup chain: builtins, then the 45 kernel commands, then programs under /bin.', 'Install: pkg install tinysh — the source is looked up in order (local sourcedir, FTP, HTTP via the Worker mirror), verified with sha256, and on success written to /bin/tinysh.TNCR.', 'Usage: Just type commands; help lists everything, exit returns to the kernel shell.', 'Artifact: 179104 bytes, sha256 starts with 4c317df355a489ae…; the full table is above and in packages/repo/INDEX.json.'],
          files: [
            { n: 'tinysh.tncr', l: null, note: '已编译产物：pkg install 校验 sha256 后写入 /bin 下的文件' },
            { n: 'tinysh.manifest', l: 10, note: '包描述：版本、依赖、min_compiler 与 sha256' },
            { n: 'tools/tinysh/src/tinysh.c', l: 1266, note: '构建时使用的真实源文件' },
            { n: 'tools/tinysh/src/kernel_api_tinyos.c', l: 343, note: '构建时使用的真实源文件' },
            { n: 'tools/tinysh/src/kernel_api_host.c', l: 454, note: '构建时使用的真实源文件' },
            { n: 'tools/tinysh/src/tinysh.h', l: 55, note: '构建时使用的真实源文件' },
            { n: 'tools/tinysh/src/kernel_api.h', l: 89, note: '构建时使用的真实源文件' }
          ] },
      ];
  /* PKGREPO:END */

  /* 合并成最终列表：仓库里的真实包全部出现；同名则以手写条目为准（信息更全）。
     只有在仓库里不存在、又确实是规划中/说明包的条目才会单独保留。 */
  var ALL_PKGS = (function () {
    var out = PKGS.slice();
    var have = {};
    out.forEach(function (p) { have[p.id] = true; });
    PKGS_REPO.forEach(function (p) { if (!have[p.id]) out.push(p); });
    return out;
  })();

  var STATE_LABEL = {
    ready:  { zh: '✅ 可安装', en: '✅ installable' },
    index:  { zh: '📘 说明包', en: '📘 index only' },
    planned:{ zh: '🚧 规划中', en: '🚧 planned' }
  };

  function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
                      .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }

  function fmtSize(n) {
    if (n == null) return '—';
    if (typeof window.formatBytes === 'function') {
      try { return window.formatBytes(n); } catch (e) { /* fall through */ }
    }
    return n.toLocaleString('en-US') + ' B';
  }

  function shortSha(s) {
    if (!s || s === '__BUILD_FILL__') return s || '—';
    return s.slice(0, 8) + '…' + s.slice(-4);
  }

  function t(zh, en) { return (lang() === 'en') ? en : zh; }

  /* ---------------- 渲染卡片列表 ---------------- */
  function renderList() {
    var L = lang();
    var html = '<ul class="pkg-grid">';
    ALL_PKGS.forEach(function (p) {
      var st = STATE_LABEL[p.state];
      html += '<li class="pkg-item">';
      html += '  <button type="button" class="pkg-card pkg-' + p.state + '"';
      html += '          data-pkg="' + esc(p.id) + '"';
      html += '          aria-expanded="false" aria-controls="pkg-detail">';
      html += '    <span class="pkg-card-top">';
      html += '      <code class="pkg-name">' + esc(p.id) + '</code>';
      html += '      <span class="pkg-state">' + esc(L === 'en' ? st.en : st.zh) + '</span>';
      html += '    </span>';
      html += '    <span class="pkg-desc">' + esc(p.descZh) + '</span>';
      html += '    <span class="pkg-meta">';
      if (p.size != null) {
        html += '<span>' + esc(fmtSize(p.size)) + '</span>';
      } else {
        html += '<span>' + (L === 'en' ? 'no artifact' : '无产物') + '</span>';
      }
      html += '<span class="pkg-dots" aria-hidden="true">·</span>';
      html += '<span>' + esc(L === 'en' ? 'more' : '查看详情') + ' →</span>';
      html += '    </span>';
      html += '  </button>';
      html += '</li>';
    });
    html += '</ul>';
    return html;
  }

  /* ---------------- 渲染详情面板 ---------------- */
  function renderDetail(p) {
    var L = lang();
    var st = STATE_LABEL[p.state] || STATE_LABEL.index;
    /* 任何字段缺失都不该让整块面板变空白：缺就给诚实的占位而不是抛异常 */
    var files = Array.isArray(p.files) ? p.files : [];
    var paras = (L === 'en') ? p.detailEn : p.detailZh;
    if (!Array.isArray(paras) || paras.length === 0) paras = [p.descZh || p.descEn || ''];
    var totalLines = files.reduce(function (a, f) { return a + (f.l || 0); }, 0);

    var h = '';
    h += '<div class="pkg-detail-head">';
    h += '  <div>';
    h += '    <code class="pkg-detail-name">' + esc(p.id) + '</code>';
    h += '    <span class="pkg-state">' + esc(L === 'en' ? st.en : st.zh) + '</span>';
    h += '  </div>';
    h += '  <button type="button" class="pkg-close" data-pkg-close aria-label="' +
         esc(L === 'en' ? 'Close details' : '关闭详情') + '">×</button>';
    h += '</div>';

    /* 关键信息表 */
    h += '<table class="pkg-facts"><tbody>';
    h += '<tr><th scope="row">' + esc(L === 'en' ? 'Version' : '版本') + '</th><td><code>v0.1</code></td></tr>';
    h += '<tr><th scope="row">' + esc(L === 'en' ? 'Directory' : '目录') + '</th><td><code>' + esc(p.dir || '—') + '</code></td></tr>';
    h += '<tr><th scope="row">' + esc(L === 'en' ? 'Installs to' : '安装到') + '</th><td><code>' + esc(p.out || '—') + '</code></td></tr>';
    h += '<tr><th scope="row">' + esc(L === 'en' ? 'Artifact size' : '产物体积') + '</th><td>' +
         esc(p.size != null ? fmtSize(p.size) + ' (' + p.size.toLocaleString('en-US') + ' bytes)' : (L === 'en' ? 'no artifact yet' : '尚无产物')) + '</td></tr>';
    h += '<tr><th scope="row">sha256</th><td><code class="pkg-sha" title="' + esc(p.sha || '') + '">' +
         esc(shortSha(p.sha)) + '</code>' +
         (p.sha === '__BUILD_FILL__'
           ? ' <span class="pkg-flag">' + esc(L === 'en' ? 'placeholder' : '占位符') + '</span>'
           : '') + '</td></tr>';
    h += '<tr><th scope="row">min_compiler</th><td><code>tncr&gt;=0.1</code></td></tr>';
    h += '<tr><th scope="row">dependencies</th><td><code>[]</code></td></tr>';
    h += '</tbody></table>';

    /* tab 切换 */
    h += '<div class="pkg-tabs" role="tablist">';
    h += '  <button type="button" class="pkg-tab is-on" data-tab="about" role="tab" aria-selected="true">' +
         esc(L === 'en' ? 'Overview' : '概览') + '</button>';
    h += '  <button type="button" class="pkg-tab" data-tab="files" role="tab" aria-selected="false">' +
         esc(L === 'en' ? 'Files' : '文件清单') + '</button>';
    h += '</div>';

    /* 概览 */
    h += '<div class="pkg-tabpanel is-on" data-panel="about">';
    paras.forEach(function (s) { h += '<p>' + esc(s) + '</p>'; });
    h += '</div>';

    /* 文件清单 */
    h += '<div class="pkg-tabpanel" data-panel="files">';
    h += '<p class="pkg-files-note">' + esc(
      L === 'en'
        ? (files.length + ' files' + (totalLines ? ', ' + totalLines.toLocaleString('en-US') + ' lines total' : ''))
        : (files.length + ' 个文件' + (totalLines ? '，共 ' + totalLines.toLocaleString('en-US') + ' 行' : ''))
    ) + '</p>';
    h += '<table class="pkg-files"><thead><tr>';
    h += '<th scope="col">' + esc(L === 'en' ? 'File' : '文件') + '</th>';
    h += '<th scope="col">' + esc(L === 'en' ? 'Lines' : '行数') + '</th>';
    h += '<th scope="col">' + esc(L === 'en' ? 'Purpose' : '用途') + '</th>';
    h += '</tr></thead><tbody>';
    files.forEach(function (f) {
      h += '<tr>';
      h += '<th scope="row"><code>' + esc(f.n) + '</code></th>';
      h += '<td class="num">' + (f.l != null ? f.l.toLocaleString('en-US') : '—') + '</td>';
      h += '<td>' + esc(f.note) + '</td>';
      h += '</tr>';
    });
    h += '</tbody></table>';
    h += '</div>';

    return h;
  }

  /* ---------------- 交互 ---------------- */
  var openId = null;

  function select(id, forceOpen) {
    var p = null;
    for (var i = 0; i < ALL_PKGS.length; i++) if (ALL_PKGS[i].id === id) p = ALL_PKGS[i];
    if (!p) return;

    var detail = document.getElementById('pkg-detail');
    if (!detail) return;

    if (openId === id && !forceOpen) {
      close();
      return;
    }

    openId = id;
    detail.innerHTML = renderDetail(p);
    detail.hidden = false;

    var cards = root.querySelectorAll('.pkg-card');
    Array.prototype.forEach.call(cards, function (b) {
      var on = b.getAttribute('data-pkg') === id;
      b.classList.toggle('is-open', on);
      b.setAttribute('aria-expanded', on ? 'true' : 'false');
    });

    /* 详情里的 tab */
    var tabs = detail.querySelectorAll('.pkg-tab');
    Array.prototype.forEach.call(tabs, function (tb) {
      tb.addEventListener('click', function () {
        var name = tb.getAttribute('data-tab');
        Array.prototype.forEach.call(tabs, function (o) {
          var on = o === tb;
          o.classList.toggle('is-on', on);
          o.setAttribute('aria-selected', on ? 'true' : 'false');
        });
        var panels = detail.querySelectorAll('.pkg-tabpanel');
        Array.prototype.forEach.call(panels, function (pp) {
          pp.classList.toggle('is-on', pp.getAttribute('data-panel') === name);
        });
      });
    });

    detail.scrollIntoView({ behavior: prefersReduced() ? 'auto' : 'smooth', block: 'nearest' });
  }

  function close() {
    openId = null;
    var detail = document.getElementById('pkg-detail');
    if (detail) { detail.hidden = true; detail.innerHTML = ''; }
    var cards = root.querySelectorAll('.pkg-card');
    Array.prototype.forEach.call(cards, function (b) {
      b.classList.remove('is-open');
      b.setAttribute('aria-expanded', 'false');
    });
  }

  function prefersReduced() {
    try {
      return !!(window.matchMedia &&
        window.matchMedia('(prefers-reduced-motion: reduce)').matches);
    } catch (e) { return false; }
  }

  function renderAll() {
    var list = document.getElementById('pkgcat-list');
    if (list) list.innerHTML = renderList();
    bindCards();
    if (openId) select(openId, true); // 语言切换后保持展开并重绘
  }

  function bindCards() {
    var cards = document.querySelectorAll('.pkg-card');
    Array.prototype.forEach.call(cards, function (b) {
      b.addEventListener('click', function () {
        select(b.getAttribute('data-pkg'));
      });
    });
    var closeBtn = document.querySelector('[data-pkg-close]');
    if (closeBtn) closeBtn.addEventListener('click', close);
  }

  /* 语言切换后重绘（app.js 用 textContent 覆写，动态内容需自行刷新） */
  var toggle = document.getElementById('lang-toggle');
  if (toggle) {
    toggle.addEventListener('click', function () {
      window.setTimeout(renderAll, 0);
    });
  }

  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape' && openId) close();
  });

  renderAll();
})();
