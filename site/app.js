/* ============================================================
   TinyOS Genesis v0.1 — site interactions (app.js)
   Pure native JS. No frameworks, no libraries, no CDN.
   - Bilingual (zh default / en) via data-zh / data-en
   - Typewriter terminal demo in #demo
   - Size bar entrance animation via IntersectionObserver
   - window.formatBytes for human-readable byte sizes
   ============================================================ */
(function () {
  'use strict';

  var STORAGE_KEY = 'tinyos-lang';
  var currentLang = 'zh';

  /* ---------- helpers: reduced motion ---------- */
  function prefersReducedMotion() {
    try {
      return !!(window.matchMedia &&
        window.matchMedia('(prefers-reduced-motion: reduce)').matches);
    } catch (e) {
      return false;
    }
  }

  /* ---------- language ---------- */
  function getStoredLang() {
    try {
      var v = window.localStorage.getItem(STORAGE_KEY);
      if (v === 'zh' || v === 'en') return v;
    } catch (e) { /* ignore (private mode etc.) */ }
    return 'zh';
  }

  function setStoredLang(lang) {
    try {
      window.localStorage.setItem(STORAGE_KEY, lang);
    } catch (e) { /* ignore */ }
  }

  function applyLang(lang) {
    currentLang = lang;
    var nodes = document.querySelectorAll('[data-zh]');
    Array.prototype.forEach.call(nodes, function (el) {
      var zh = el.getAttribute('data-zh');
      var en = el.getAttribute('data-en');
      var text;
      if (lang === 'en') {
        text = (en != null) ? en : zh;
      } else {
        text = zh;
      }
      if (text != null) el.textContent = text;
    });
    if (document.documentElement) {
      document.documentElement.lang = (lang === 'zh') ? 'zh-CN' : 'en';
    }
  }

  function bindLangToggle() {
    var btn = document.getElementById('lang-toggle');
    if (!btn) return; // robust: silently skip if missing
    btn.addEventListener('click', function () {
      var next = (currentLang === 'zh') ? 'en' : 'zh';
      setStoredLang(next);
      applyLang(next);
    });
  }

  /* ---------- terminal typewriter ---------- */
  // Real VGA text-mode session output (ASCII only).
  var DEMO_LINES = [
    'C:\\> tinysh',
    '[tinysh] starting the Genesis user shell; type `exit` to come back',
    'tinysh> version',
    'TinyOS Genesis v0.1 | tinysh v0.1 | kernel TinyOS 2.0 (i386)',
    'tinysh> sysinfo',
    'CPU: i386 (TinyOS protected mode)',
    'Mem total: 8192 KB, Free: 7479 KB',
    'Uptime: 11 s',
    'Process count: 1',
    'tinysh> devlist',
    'com1',
    'com2',
    'pic',
    'pit',
    'vga',
    'kbd',
    'ide0',
    'tinysh> ps',
    'PID  STATE    SESS  UID   NAME',
    '1    RUNNING  0     0     /bin/tinysh.TNCR',
    'tinysh> mkdir /home/demo',
    'tinysh> cd /home/demo',
    'tinysh> pwd',
    '/home/demo',
    'tinysh> exit',
    '[tinysh] exited'
  ];

  function typeDemo() {
    var demo = document.getElementById('demo');
    if (!demo) return; // robust: silently skip if missing

    var text = DEMO_LINES.join('\n');

    // Reduced motion: output everything at once, no animation.
    if (prefersReducedMotion()) {
      demo.textContent = text;
      return;
    }

    var tn = document.createTextNode('');
    var cursor = document.createElement('span');
    cursor.className = 'type-cursor';
    cursor.textContent = '█'; // █
    cursor.style.animation = 'blink 1s steps(1) infinite';
    demo.appendChild(tn);
    demo.appendChild(cursor);

    var i = 0;
    function step() {
      if (i < text.length) {
        tn.nodeValue += text.charAt(i);
        var ch = text.charAt(i);
        i++;
        // ~18ms/char, slightly longer on newlines; total < 10s.
        var delay = (ch === '\n') ? 28 : 18;
        demo.scrollTop = demo.scrollHeight;
        setTimeout(step, delay);
      } else {
        // done: stop blinking and remove cursor
        cursor.style.animation = 'none';
        if (cursor.parentNode) cursor.parentNode.removeChild(cursor);
      }
    }
    step();
  }

  /* ---------- replay button (optional) ---------- */
  function addReplayButton() {
    var demo = document.getElementById('demo');
    if (!demo) return;
    var container = demo.parentElement; // .term
    if (!container) return;

    var btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'replay-btn';
    btn.setAttribute('data-zh', '重播');
    btn.setAttribute('data-en', 'Replay');
    btn.textContent = '重播';
    btn.style.marginTop = '12px';
    btn.style.background = 'transparent';
    btn.style.color = 'var(--muted)';
    btn.style.border = '1px solid var(--line)';
    btn.style.borderRadius = '8px';
    btn.style.padding = '6px 14px';
    btn.style.cursor = 'pointer';
    btn.style.fontFamily = 'var(--font)';
    btn.addEventListener('mouseenter', function () {
      btn.style.borderColor = 'var(--accent)';
      btn.style.color = 'var(--accent)';
    });
    btn.addEventListener('mouseleave', function () {
      btn.style.borderColor = 'var(--line)';
      btn.style.color = 'var(--muted)';
    });
    btn.addEventListener('click', function () {
      demo.textContent = ''; // clear (removes any leftover cursor)
      typeDemo();
    });
    container.appendChild(btn);
  }

  /* ---------- size bars entrance animation ---------- */
  function animateBars() {
    var bars = document.querySelectorAll('.bar');
    if (!bars || !bars.length) return; // robust: silently skip if missing

    // Reduced motion OR no IntersectionObserver: show final values (inline width).
    if (prefersReducedMotion() || !('IntersectionObserver' in window)) {
      return;
    }

    var targets = [];
    Array.prototype.forEach.call(bars, function (bar) {
      var t = bar.style.width || '0%';
      targets.push(t);
      bar.style.transition = 'width 1.1s ease';
      bar.style.width = '0%';
    });

    var observer = new IntersectionObserver(function (entries) {
      Array.prototype.forEach.call(entries, function (entry) {
        if (entry.isIntersecting) {
          var idx = Array.prototype.indexOf.call(bars, entry.target);
          if (idx >= 0) entry.target.style.width = targets[idx];
          observer.unobserve(entry.target);
        }
      });
    }, { threshold: 0.3 });

    Array.prototype.forEach.call(bars, function (bar) {
      observer.observe(bar);
    });
  }

  /* ---------- formatBytes (exposed for reuse) ---------- */
  // 12611584 -> "12 MB". Integer + 1 space + unit.
  window.formatBytes = function (n) {
    if (typeof n !== 'number' || isNaN(n) || n < 0) return String(n);
    if (n < 1024) return n + ' B';
    var units = ['KB', 'MB', 'GB', 'TB', 'PB'];
    var val = n;
    var i = -1;
    while (val >= 1024 && i < units.length - 1) {
      val = val / 1024;
      i++;
    }
    return Math.round(val) + ' ' + units[i];
  };

  /* ---------- init ---------- */
  function init() {
    var lang = getStoredLang(); // default 'zh'
    applyLang(lang);
    bindLangToggle();
    animateBars();
    typeDemo();
    addReplayButton();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
