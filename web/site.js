/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT — documentation site behaviour.
 *
 * Everything here is an enhancement. The site reads correctly with this file
 * blocked: the terminal already contains its final text in the markup, the
 * theme has a working default, and every link is a real link.
 */
(function () {
  "use strict";

  /* ------------------------------------------------------------- theme */

  var root = document.documentElement;
  try {
    var saved = localStorage.getItem("resentment-theme");
    if (saved) root.setAttribute("data-theme", saved);
  } catch (e) { /* private window, or storage blocked */ }

  var toggle = document.getElementById("theme");
  if (toggle) {
    toggle.addEventListener("click", function () {
      var next = root.getAttribute("data-theme") === "light" ? "dark" : "light";
      root.setAttribute("data-theme", next);
      try { localStorage.setItem("resentment-theme", next); } catch (e) {}
    });
  }

  /* -------------------------------------------------------- docs menu */

  var menu = document.getElementById("menu");
  var sidebar = document.querySelector(".sidebar");
  if (menu && sidebar) {
    menu.addEventListener("click", function () { sidebar.classList.toggle("open"); });
    sidebar.addEventListener("click", function (e) {
      if (e.target.tagName === "A") sidebar.classList.remove("open");
    });
  }

  /* -------------------------------------------------------- copy code */

  document.addEventListener("click", function (e) {
    var btn = e.target.closest && e.target.closest(".copy");
    if (!btn) return;
    var code = btn.parentNode.querySelector("code");
    if (!code) return;
    var text = code.innerText;
    var done = function () {
      btn.textContent = "copied";
      btn.classList.add("done");
      setTimeout(function () {
        btn.textContent = "copy";
        btn.classList.remove("done");
      }, 1400);
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(done, function () {});
    } else {
      var ta = document.createElement("textarea");
      ta.value = text;
      document.body.appendChild(ta);
      ta.select();
      try { document.execCommand("copy"); done(); } catch (err) {}
      document.body.removeChild(ta);
    }
  });

  /* ------------------------------------------------ table of contents */

  var tocLinks = [].slice.call(document.querySelectorAll(".toc a"));
  if (tocLinks.length && "IntersectionObserver" in window) {
    var byId = {};
    tocLinks.forEach(function (a) { byId[a.getAttribute("href").slice(1)] = a; });
    var seen = [];
    var obs = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) {
        var id = en.target.id;
        var i = seen.indexOf(id);
        if (en.isIntersecting && i < 0) seen.push(id);
        if (!en.isIntersecting && i >= 0) seen.splice(i, 1);
      });
      tocLinks.forEach(function (a) { a.style.color = ""; a.style.borderLeftColor = ""; });
      if (seen.length) {
        var a = byId[seen[0]];
        if (a) {
          a.style.color = "var(--brass)";
          a.style.borderLeftColor = "var(--brass)";
        }
      }
    }, { rootMargin: "-80px 0px -70% 0px" });
    Object.keys(byId).forEach(function (id) {
      var el = document.getElementById(id);
      if (el) obs.observe(el);
    });
  }

  /* -------------------------------------------------------- the clock */

  /* Kaalka derives keying material from the angles between the hands of a
     clock, so the landing page shows the actual angles of the actual current
     time. It is the one piece of this site that is not decoration. */
  var clock = document.getElementById("clock");
  if (clock) {
    var hh = clock.querySelector(".hand-h"),
        mh = clock.querySelector(".hand-m"),
        sh = clock.querySelector(".hand-s"),
        out = document.getElementById("angles");

    var hand = function (el, deg, len) {
      var r = (deg - 90) * Math.PI / 180;
      el.setAttribute("x2", (100 + Math.cos(r) * len).toFixed(2));
      el.setAttribute("y2", (100 + Math.sin(r) * len).toFixed(2));
    };

    var tick = function () {
      var n = new Date();
      var s = n.getSeconds() + n.getMilliseconds() / 1000;
      var m = n.getMinutes() + s / 60;
      var h = (n.getHours() % 12) + m / 60;

      var as = s * 6, am = m * 6, ah = h * 30;
      hand(sh, as, 70);
      hand(mh, am, 62);
      hand(hh, ah, 44);

      if (out) {
        var sep = function (a, b) {
          var d = Math.abs(a - b) % 360;
          return (d > 180 ? 360 - d : d).toFixed(1);
        };
        out.innerHTML =
          "h–m <b>" + sep(ah, am) + "°</b>" +
          "<span>m–s <b>" + sep(am, as) + "°</b></span>" +
          "<span>h–s <b>" + sep(ah, as) + "°</b></span>";
      }
      requestAnimationFrame(tick);
    };
    tick();
  }

  /* ------------------------------------------------------ the terminal */

  /* The transcript is real: it is what the kernel prints, copied from a boot
     under QEMU. It is retyped rather than faded in because the point is that
     these lines arrive in order, from a machine, in about a tenth of a
     second. */
  var term = document.getElementById("term");
  if (!term) return;

  var script = [
    ["t",  "[    0.000000] ", "pmm      physical memory: 505.7 MiB usable, 131040 frames"],
    ["t",  "[    0.012027] ", "irqchip  APIC timer: 63238 ticks/ms"],
    ["t",  "[    0.021841] ", "kaalka   temporal keying active, epoch 29798350"],
    ["t",  "[    0.025446] ", "graph    runtime graph online, root node 1"],
    ["t",  "[    0.028901] ", "cap      capability system ready: 24 types, sealed"],
    ["t",  "[    0.045804] ", "smp      ACPI reports 4 usable processors"],
    ["t",  "[    0.061432] ", "smp      cpu1 online (apic id 1)"],
    ["t",  "[    0.070330] ", "smp      cpu2 online (apic id 2)"],
    ["t",  "[    0.080912] ", "smp      cpu3 online (apic id 3)"],
    ["t",  "[    0.082106] ", "smp      4 of 4 processors started"],
    ["ok", "[    0.118176] ", "selftest all 7 self-tests passed"],
    ["ok", "[    0.119217] ", "boot     boot complete in 119 ms, 505.4 MiB free"],
    ["gap"],
    ["banner"],
    ["gap"],
    ["cmd", "2 + 2"],
    ["out", "4"],
    ["cmd", "[4, 8, 15, 16, 23, 42] |> filter(fun(n) -> n % 2 is 0) |> sum()"],
    ["out", "104"],
    ["cmd", "now()"],
    ["err", "not allowed to now."],
    ["errd", "  This script was not granted permission to read the clock."],
    ["cmd", ".allow time"],
    ["ok2", "granted permission to read the clock"],
    ["cmd", ".digest"],
    ["dig", "7d4a1f0e83c25b9a6f1e0d4c8b3a7e2f5d9c1b8a4e7f0c3d6a9b2e5f8c1d4a7b"]
  ];

  var esc = function (s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  };

  var out = "", li = 0, ci = 0;
  var cursor = '<span class="cursor"></span>';

  var render = function () { term.innerHTML = out + cursor; };

  var step = function () {
    if (li >= script.length) {
      render();
      return;
    }
    var row = script[li];
    var kind = row[0];

    if (kind === "gap") { out += "\n"; li++; return setTimeout(step, 90); }

    if (kind === "banner") {
      out += '<span class="k">  RESENTMENT 2.0.0 (kaalachakra)  x86_64  4 cpu</span>\n'
           + '<span class="cm">  a capability-secure, AI-native kernel</span>\n';
      li++;
      return setTimeout(step, 260);
    }

    if (kind === "cmd") {
      var text = row[1];
      if (ci === 0) out += '<span class="p">resentment&gt; </span>';
      if (ci < text.length) {
        out += esc(text[ci]);
        ci++;
        render();
        return setTimeout(step, 26);
      }
      out += "\n"; ci = 0; li++;
      return setTimeout(step, 210);
    }

    var cls = { t: "t", ok: "ok", out: "in", err: "k", errd: "cm",
                ok2: "ok", dig: "d" }[kind] || "in";
    if (kind === "t" || kind === "ok") {
      out += '<span class="t">' + esc(row[1]) + '</span>'
           + '<span class="' + cls + '">' + esc(row[2]) + "</span>\n";
      li++;
      render();
      return setTimeout(step, 55);
    }
    out += '<span class="' + cls + '">' + esc(row[1]) + "</span>\n";
    li++;
    render();
    return setTimeout(step, 150);
  };

  var started = false;
  var start = function () {
    if (started) return;
    started = true;
    term.innerHTML = "";
    out = "";
    setTimeout(step, 300);
  };

  if (window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches) {
    return;   /* leave the static transcript that is already in the markup */
  }
  if ("IntersectionObserver" in window) {
    new IntersectionObserver(function (es, o) {
      if (es[0].isIntersecting) { start(); o.disconnect(); }
    }, { threshold: .25 }).observe(term);
  } else {
    start();
  }
})();
