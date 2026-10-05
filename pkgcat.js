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
        { id: 'CC', state: 'ready', size: 162680,
          sha: '2cd651fb19122096d1c79a93bd7c131e8b2a143dbac742b8f7f18b8e8ad8f401',
          descZh: 'C 编译器/工具链前端（tncr 交叉编译）',
          descEn: 'C 编译器/工具链前端（tncr 交叉编译） (162680 bytes, sha256 2cd651fb1912...)' },
        { id: 'DESKTOP', state: 'ready', size: 34,
          sha: '02b93608bb92b3c652b45a734de43435c726419faa6f7454bd8ed14714f375da',
          descZh: '图形文本桌面启动器',
          descEn: '图形文本桌面启动器 (34 bytes, sha256 02b93608bb92...)' },
        { id: 'EDIT', state: 'ready', size: 45980,
          sha: '0d00374174c799423b56f59e8f6646040a37fe788c96b358ea41677b58ecf37c',
          descZh: '文本编辑器',
          descEn: '文本编辑器 (45980 bytes, sha256 0d00374174c7...)' },
        { id: 'JVM', state: 'ready', size: 5851,
          sha: '152792856b7d854182b794eac2f2bc8fb667e90c1c0d2b7d806470dd926dcd17',
          descZh: 'JVM 运行时',
          descEn: 'JVM 运行时 (5851 bytes, sha256 152792856b7d...)' },
        { id: 'LUA', state: 'ready', size: 210584,
          sha: '8294b5a55b4ab7bcd72a7e9aa0198888ff3f9d8eb9ee28bcd026e7a899a0fab1',
          descZh: 'Lua 5.4.7 解释器（REPL 交互 + lua /home/x.lua 脚本模式）',
          descEn: 'Lua 5.4.7 解释器（REPL 交互 + lua /home/x.lua 脚本模式） (210584 bytes, sha256 8294b5a55b4a...)' },
        { id: 'SSHD', state: 'ready', size: 114500,
          sha: '2507b0deaf22551b65eb85e276ba30aab949974430d6a876d64549e95eac8d02',
          descZh: 'SSH 服务端',
          descEn: 'SSH 服务端 (114500 bytes, sha256 2507b0deaf22...)' },
        { id: 'basic_demo', state: 'ready', size: 204,
          sha: 'b9e6faf53975ca7b7fad32d0b51471c31c22216e56cc8fff96e2d0601dfa4193',
          descZh: 'TinyBASIC 解释器运行的循环示例',
          descEn: 'TinyBASIC 解释器运行的循环示例 (204 bytes, sha256 b9e6faf53975...)' },
        { id: 'count', state: 'ready', size: 877,
          sha: '347c41ce0a4f48c5f28a4bbe3bab3788c87f800076ed19b0d8129c8859e5e8ac',
          descZh: '循环与计数器，演示 C 的基本控制流与终端输出',
          descEn: '循环与计数器，演示 C 的基本控制流与终端输出 (877 bytes, sha256 347c41ce0a4f...)' },
        { id: 'cpp_demo', state: 'ready', size: 339,
          sha: 'b3c76dd35c1c9abe1178226b24368f2883cf1f5ef97b4889249a0f306a5680d8',
          descZh: '最小化 C++ 程序，验证 C++ 工具链可用性',
          descEn: '最小化 C++ 程序，验证 C++ 工具链可用性 (339 bytes, sha256 b3c76dd35c1c...)' },
        { id: 'gui_demo', state: 'ready', size: 470,
          sha: 'e553e1be79b1da419fe2f0ccd784230dd534157a5ff66aa6c7739372ed48c281',
          descZh: '桌面 GUI 演示（--gui），展示窗口与图形输出',
          descEn: '桌面 GUI 演示（--gui），展示窗口与图形输出 (470 bytes, sha256 e553e1be79b1...)' },
        { id: 'hello', state: 'ready', size: 214,
          sha: '050668ab67b7743b5e50372f07e95be433bf14234abe52267fbf19170d5ba492',
          descZh: '最小化 C 程序，验证 C 工具链与 TNCR 运行时',
          descEn: '最小化 C 程序，验证 C 工具链与 TNCR 运行时 (214 bytes, sha256 050668ab67b7...)' },
        { id: 'minic_demo', state: 'ready', size: 500,
          sha: 'ca92e2c0b062ee0605499b8e058db8aeaa62e4c1e3e84d6ed21adacf6ecf6aae',
          descZh: 'MiniC 编译器示例（calc）',
          descEn: 'MiniC 编译器示例（calc） (500 bytes, sha256 ca92e2c0b062...)' },
        { id: 'net_demo', state: 'ready', size: 467,
          sha: '3d41ba3758a1539440b8de1e27a5c22ac91803fabf1898a04521f0090c5f6e16',
          descZh: '网络栈演示，使用 ARP/IPv4/TCP',
          descEn: '网络栈演示，使用 ARP/IPv4/TCP (467 bytes, sha256 3d41ba3758a1...)' },
        { id: 'tiny_demo', state: 'ready', size: 238,
          sha: 'dfdefb943aa31c6d66b415848e7cc1c7b3a4f4c9a069414de9e88130c0cdfdf3',
          descZh: 'TinyLang 源程序示例，验证 TinyLang 工具链',
          descEn: 'TinyLang 源程序示例，验证 TinyLang 工具链 (238 bytes, sha256 dfdefb943aa3...)' },
        { id: 'tinysh', state: 'ready', size: 179104,
          sha: '4c317df355a489aeea5d6f26447f0a0472c67afc7c83b2f4fa4c8229f567f2bc',
          descZh: 'TinyOS Genesis 默认 Shell（登录后自动进入，22 条命令 + pack/run + 三级命令查找链）',
          descEn: 'TinyOS Genesis 默认 Shell（登录后自动进入，22 条命令 + pack/run + 三级命令查找链） (179104 bytes, sha256 4c317df355a4...)' },
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
    var st = STATE_LABEL[p.state];
    var totalLines = p.files.reduce(function (a, f) { return a + (f.l || 0); }, 0);

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
    h += '<tr><th scope="row">' + esc(L === 'en' ? 'Directory' : '目录') + '</th><td><code>' + esc(p.dir) + '</code></td></tr>';
    h += '<tr><th scope="row">' + esc(L === 'en' ? 'Installs to' : '安装到') + '</th><td><code>' + esc(p.out) + '</code></td></tr>';
    h += '<tr><th scope="row">' + esc(L === 'en' ? 'Artifact size' : '产物体积') + '</th><td>' +
         esc(p.size != null ? fmtSize(p.size) + ' (' + p.size.toLocaleString('en-US') + ' bytes)' : (L === 'en' ? 'no artifact yet' : '尚无产物')) + '</td></tr>';
    h += '<tr><th scope="row">sha256</th><td><code class="pkg-sha" title="' + esc(p.sha) + '">' +
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
    var paras = (L === 'en') ? p.detailEn : p.detailZh;
    h += '<div class="pkg-tabpanel is-on" data-panel="about">';
    paras.forEach(function (s) { h += '<p>' + esc(s) + '</p>'; });
    h += '</div>';

    /* 文件清单 */
    h += '<div class="pkg-tabpanel" data-panel="files">';
    h += '<p class="pkg-files-note">' + esc(
      L === 'en'
        ? (p.files.length + ' files' + (totalLines ? ', ' + totalLines.toLocaleString('en-US') + ' lines total' : ''))
        : (p.files.length + ' 个文件' + (totalLines ? '，共 ' + totalLines.toLocaleString('en-US') + ' 行' : ''))
    ) + '</p>';
    h += '<table class="pkg-files"><thead><tr>';
    h += '<th scope="col">' + esc(L === 'en' ? 'File' : '文件') + '</th>';
    h += '<th scope="col">' + esc(L === 'en' ? 'Lines' : '行数') + '</th>';
    h += '<th scope="col">' + esc(L === 'en' ? 'Purpose' : '用途') + '</th>';
    h += '</tr></thead><tbody>';
    p.files.forEach(function (f) {
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
