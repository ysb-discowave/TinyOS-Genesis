/* ============================================================
   TinyOS Genesis v0.1 — site interactions (app.js)
   Pure native JS. No frameworks, no libraries, no CDN.
   - Bilingual (zh default / en) via data-zh / data-en
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
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
