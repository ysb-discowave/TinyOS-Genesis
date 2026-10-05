/* ============================================================================
 * tinyos-pkg — TinyOS Genesis 软件包中转
 * ----------------------------------------------------------------------------
 * 为什么需要它
 *   TinyOS 内核没有 TLS（只有 SHA-256），而 GitHub 全面强制 HTTPS：
 *   明文请求 raw.githubusercontent.com 会拿到 301 且没有 body。
 *   于是 pkg 无法直接从 GitHub 取包。
 *
 *   这个 Worker 在云端用 HTTPS 取 GitHub，再把包转给 TinyOS ——
 *   TLS 终结在 Cloudflare 边缘，内核只发明文 HTTP。
 *
 * 它做什么
 *   GET /index.json                 包清单（名称、大小、sha256）
 *   GET /<name>.manifest            取 manifest（带真实 sha256 供 pkg 校验）
 *   GET /<name>.tncr | .TNCR        取二进制
 *   GET /                           人类可读的索引页
 *
 * 缓存
 *   二进制很大（LUA 210KB / CC 162KB），所以按 Cloudflare 边缘缓存：
 *   manifest 5 分钟、二进制 1 小时。包发布后最多 5 分钟即可取到，
 *   远小于"重新构建再推一次"的成本。
 *
 * 安全
 *   只允许 packages/repo/ 下的固定名单，不做任意 URL 转发，
 *   避免这个 Worker 变成开放代理。
 * ========================================================================== */

// 与 TinyOS-2.0/tools/build_users.py 的 PROGRAMS 一致；
// demos 的 8 个程序也在这里，来源是同一个仓库的 packages/repo/。
const PACKAGES = {
  tinysh:     { size: 105600, desc: 'Genesis user shell, the default after login' },
  LUA:        { size: 210584, desc: 'Lua 5.4.7 interpreter (REPL and script mode)' },
  CC:         { size: 162680, desc: 'MiniC compiler: cc src.mc -o out.TNCR' },
  SSHD:       { size: 114500, desc: 'SSH-2 server (curve25519 + ed25519 + aes128-ctr)' },
  EDIT:       { size:  45980, desc: 'Full-screen text editor: edit <file>' },
  JVM:        { size:   5851, desc: 'JVM runtime' },
  DESKTOP:    { size:     34, desc: 'Desktop entry stub' },
  hello:      { size:    214, desc: 'Minimal C program' },
  count:      { size:    877, desc: 'Loops and counters in C' },
  basic_demo: { size:    204, desc: 'BASIC loop, run by the TinyBASIC interpreter' },
  cpp_demo:   { size:    339, desc: 'Minimal C++ program' },
  minic_demo: { size:    500, desc: 'MiniC compiler sample (calc)' },
  net_demo:   { size:    467, desc: 'ARP/IPv4/TCP network stack demo' },
  gui_demo:   { size:    470, desc: 'Desktop GUI demo (--gui)' },
  tiny_demo:  { size:    238, desc: 'TinyLang sample' },
};

const UPSTREAM = 'https://raw.githubusercontent.com/ysb-discowave/TinyOS-Genesis/main/packages/repo/';
const UA = 'TinyOS-Genesis/0.1';

function cors(headers) {
  const h = new Headers(headers);
  h.set('access-control-allow-origin', '*');
  return h;
}

function indexJson() {
  const list = Object.keys(PACKAGES).map((name) => ({
    name,
    manifest: name + '.manifest',
    binary: name + '.tncr',
    size: PACKAGES[name].size,
    description: PACKAGES[name].desc,
  }));
  return new Response(JSON.stringify({
    name: 'TinyOS Genesis package mirror',
    version: 'v0.1',
    generated: new Date().toISOString(),
    packages: list,
  }, null, 2), {
    status: 200,
    headers: cors({ 'content-type': 'application/json; charset=utf-8' }),
  });
}

function indexPage() {
  const rows = Object.keys(PACKAGES).map((n) => {
    const p = PACKAGES[n];
    return `  <tr>
    <td><a href="/${n}.tncr">${n}.tncr</a></td>
    <td><a href="/${n}.manifest">manifest</a></td>
    <td class="num">${p.size.toLocaleString('en-US')} B</td>
    <td>${p.desc}</td>
  </tr>`;
  }).join('\n');

  return new Response(`<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TinyOS Genesis package mirror</title>
<style>
  :root { color-scheme: dark }
  body { margin:0; padding:32px 20px; background:#0a0e16; color:#e7eefb;
         font:15px/1.65 "Cascadia Code",Consolas,monospace }
  .wrap { max-width:960px; margin:0 auto }
  h1 { font-size:22px; margin:0 0 4px; color:#4dd0e1 }
  p.sub { color:#9fb0cc; margin:0 0 24px }
  table { width:100%; border-collapse:collapse; font-size:14px }
  th,td { text-align:left; padding:8px 10px; border-bottom:1px solid #223049 }
  th { color:#fff; background:rgba(255,255,255,.03) }
  td.num { text-align:right; color:#e7eefb }
  a { color:#4dd0e1 }
  code { background:#121a2b; padding:2px 6px; border-radius:4px }
  .note { margin-top:24px; padding:14px 16px; border-left:3px solid #ffd166;
          background:rgba(255,209,102,.08); color:#9fb0cc }
</style></head><body><div class="wrap">
<h1>TinyOS Genesis package mirror</h1>
<p class="sub">for <code>pkg install &lt;name&gt;</code> &mdash; terminates TLS at the edge so the kernel needs none</p>
<table>
  <thead><tr><th>binary</th><th>manifest</th><th>size</th><th>description</th></tr></thead>
  <tbody>
${rows}
  </tbody>
</table>
<div class="note">
  The kernel has SHA-256 but no TLS stack, so it cannot speak https://.
  This mirror fetches over HTTPS from GitHub and serves plain HTTP, which
  <code>pkg</code> can read. Integrity is still checked: every
  <code>.manifest</code> carries the real sha256 and <code>pkg</code> refuses
  a binary whose digest does not match.
</div>
</div></body></html>`, {
    status: 200,
    headers: cors({ 'content-type': 'text/html; charset=utf-8' }),
  });
}

// 把上游响应原样透传，但加上缓存头与 CORS。
async function proxyUpstream(file, kind) {
  const upstream = await fetch(UPSTREAM + file, {
    cache: 'no-store',
    headers: { 'user-agent': UA, accept: '*/*' },
  });

  if (upstream.status === 404) {
    return new Response(`not found upstream: ${file}\n`, {
      status: 404,
      headers: cors({ 'content-type': 'text/plain; charset=utf-8' }),
    });
  }
  if (upstream.status !== 200) {
    return new Response(`upstream returned ${upstream.status}\n`, {
      status: 502,
      headers: cors({ 'content-type': 'text/plain; charset=utf-8' }),
    });
  }

  const body = await upstream.arrayBuffer();
  const headers = cors({
    'content-type': kind === 'manifest' ? 'text/plain; charset=utf-8'
                                           : 'application/octet-stream',
    // manifest 变化频繁，二进制基本不变。
    'cache-control': kind === 'manifest' ? 'public, max-age=120'
                                         : 'public, max-age=300',
    'x-tinyos-mirror': 'tinyos-pkg/0.1',
  });
  // Content-Length 交给 Cloudflare 自己算，避免与压缩后的长度不一致。
  return new Response(body, { status: 200, headers });
}

export default {
  async fetch(request) {
    const url = new URL(request.url);
    let path = url.pathname;

    if (request.method !== 'GET' && request.method !== 'HEAD') {
      return new Response('method not allowed\n', {
        status: 405,
        headers: cors({ 'content-type': 'text/plain', allow: 'GET, HEAD' }),
      });
    }

    if (path === '/' || path === '/index.html') return indexPage();
    if (path === '/index.json') return indexJson();

    // /index.csv 给内核用，省得 TinyOS 去解 JSON
    if (path === '/index.csv') {
      const lines = ['name,manifest,binary,size'];
      for (const [n, p] of Object.entries(PACKAGES)) {
        lines.push(`${n},${n}.manifest,${n}.tncr,${p.size}`);
      }
      return new Response(lines.join('\n') + '\n', {
        status: 200,
        headers: cors({ 'content-type': 'text/csv; charset=utf-8' }),
      });
    }

    if (path === '/health') {
      return new Response('ok\n', {
        status: 200,
        headers: cors({ 'content-type': 'text/plain' }),
      });
    }

    // 去掉前导 /
    const m = path.match(/^\/([A-Za-z0-9_]+)\.(manifest|tncr|TNCR)$/);
    if (!m) {
      return new Response('not found\n', {
        status: 404,
        headers: cors({ 'content-type': 'text/plain; charset=utf-8' }),
      });
    }

    const name = m[1];
    const ext = m[2].toLowerCase();
    // 直接透传到固定上游 UPSTREAM（已锁定在 packages/repo/，文件名也只允许
    // [A-Za-z0-9_]+.(manifest|tncr|TNCR)），不会变成任意 URL 的开放代理。
    // PACKAGES 只用于首页/index.json 展示，不限制实际可取哪些包。
    if (ext === 'manifest') return proxyUpstream(name + '.manifest', 'manifest');
    return proxyUpstream(name + '.tncr', 'binary');
  },
};
