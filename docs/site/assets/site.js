/* JaiScript docs: theme toggle, hand-rolled highlighter, code/output pairs, client-side search. No dependencies. */
(function () {
  "use strict";

  /* ---------------- theme toggle ---------------- */
  var root = document.documentElement;
  var toggle = document.getElementById("theme-toggle");
  function paintToggle() {
    if (!toggle) return;
    var dark = root.getAttribute("data-theme") === "dark";
    var label = dark ? "Switch to light theme" : "Switch to dark theme";
    toggle.setAttribute("aria-label", label);
    toggle.title = label;
  }
  if (toggle) {
    paintToggle();
    toggle.addEventListener("click", function () {
      var next = root.getAttribute("data-theme") === "dark" ? "light" : "dark";
      root.setAttribute("data-theme", next);
      try { localStorage.setItem("jai-theme", next); } catch (e) {}
      paintToggle();
    });
  }

  /* ---------------- heading anchors ---------------- */
  // The markup carries a bare "link" label; say what the link is for, once, here.
  (function () {
    var anchors = document.querySelectorAll(".hlink");
    for (var i = 0; i < anchors.length; i++) {
      anchors[i].setAttribute("aria-label", "Link to this section");
    }
  }());

  /* ---------------- tiny tokenizing highlighter ---------------- */
  var JAI_KW = {};
  ("var auto int double float string bool char class function coroutine yield if else while for " +
   "switch case default fallthrough break continue try catch throw return include import namespace " +
   "enum new null true false void this super override operator static private public array map " +
   "shared_ptr weak_ptr cast extern skip_newline skip_flush").split(" ").forEach(function (k) { JAI_KW[k] = 1; });
  var CPP_KW = {};
  ("auto int double float bool char class struct const constexpr void return if else while for switch " +
   "case break continue try catch throw new delete nullptr true false namespace using template typename " +
   "static inline virtual override public private protected friend operator enum union sizeof this " +
   "std jai uint8_t int8_t int16_t int32_t int64_t uint32_t uint64_t size_t include define").split(" ")
    .forEach(function (k) { CPP_KW[k] = 1; });
  var JAI_TYPES = {};
  "int double float string bool char void array map shared_ptr weak_ptr auto var".split(" ")
    .forEach(function (k) { JAI_TYPES[k] = 1; });

  function isDigit(c) { return c >= "0" && c <= "9"; }
  function isIdent(c) {
    return (c >= "a" && c <= "z") || (c >= "A" && c <= "Z") || isDigit(c) || c === "_";
  }
  function esc(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }

  function highlight(src, kw, types) {
    var out = [], i = 0, n = src.length;
    function push(cls, text) {
      out.push(cls ? '<span class="' + cls + '">' + esc(text) + "</span>" : esc(text));
    }
    while (i < n) {
      var c = src[i];
      // line comment
      if (c === "/" && src[i + 1] === "/") {
        var j = src.indexOf("\n", i); if (j < 0) j = n;
        push("tk-com", src.slice(i, j)); i = j; continue;
      }
      // block comment
      if (c === "/" && src[i + 1] === "*") {
        var j2 = src.indexOf("*/", i + 2); j2 = j2 < 0 ? n : j2 + 2;
        push("tk-com", src.slice(i, j2)); i = j2; continue;
      }
      // raw string: R"delim( ... )delim"
      if (c === "R" && src[i + 1] === '"' && kw === JAI_KW && !(i > 0 && isIdent(src[i - 1]))) {
        var open = src.indexOf("(", i + 2);
        if (open >= 0 && open - (i + 2) <= 16) {
          var close = src.indexOf(")" + src.slice(i + 2, open) + '"', open + 1);
          var stop = close < 0 ? n : close + (open - i);
          push("tk-str", src.slice(i, stop)); i = stop; continue;
        }
      }
      // string
      if (c === '"' || (c === "`" && kw === JAI_KW)) {
        var k = i + 1;
        while (k < n && src[k] !== c) { if (src[k] === "\\") k++; k++; }
        k = Math.min(k + 1, n);
        push("tk-str", src.slice(i, k)); i = k; continue;
      }
      // char literal
      if (c === "'") {
        var k2 = i + 1;
        while (k2 < n && src[k2] !== "'") { if (src[k2] === "\\") k2++; k2++; }
        k2 = Math.min(k2 + 1, n);
        push("tk-str", src.slice(i, k2)); i = k2; continue;
      }
      // number (hex, binary, decimal w/ exponent)
      if (isDigit(c)) {
        var k3 = i;
        if (c === "0" && (src[i + 1] === "x" || src[i + 1] === "X" || src[i + 1] === "b" || src[i + 1] === "B")) {
          k3 = i + 2;
          while (k3 < n && isIdent(src[k3])) k3++;
        } else {
          while (k3 < n && (isDigit(src[k3]) || src[k3] === ".")) k3++;
          if (src[k3] === "e" || src[k3] === "E") {
            k3++;
            if (src[k3] === "+" || src[k3] === "-") k3++;
            while (k3 < n && isDigit(src[k3])) k3++;
          }
        }
        push("tk-num", src.slice(i, k3)); i = k3; continue;
      }
      // identifier / keyword
      if (isIdent(c) && !isDigit(c)) {
        var k4 = i;
        while (k4 < n && isIdent(src[k4])) k4++;
        var word = src.slice(i, k4);
        if (kw[word]) push(types[word] ? "tk-ty" : "tk-kw", word);
        else if (src[k4] === "(") push("tk-fn", word);
        else push("", word);
        i = k4; continue;
      }
      // everything else: pass through one char
      push("", c); i++;
    }
    return out.join("");
  }

  var blocks = document.querySelectorAll("code.lang-jai, code.lang-cpp");
  for (var b = 0; b < blocks.length; b++) {
    var el = blocks[b];
    var isJai = el.className.indexOf("lang-jai") >= 0;
    el.innerHTML = highlight(el.textContent, isJai ? JAI_KW : CPP_KW, isJai ? JAI_TYPES : {});
  }

  /* ---------------- line-number gutters ---------------- */
  // Every code snippet gets a line gutter; the numbers live outside the selection
  // (user-select: none) so copied code stays clean. Output/capture frames are
  // program results, not snippets — no numbers there. One-liners stay bare.
  var numbered = document.querySelectorAll(
    ".codeblock:not(.cb-output):not(.cb-capture) pre > code");
  for (var n = 0; n < numbered.length; n++) {
    var codeEl = numbered[n];
    var text = codeEl.textContent;
    if (text.charAt(text.length - 1) === "\n") { text = text.slice(0, -1); }
    var count = text.split("\n").length;
    if (count < 2) { continue; }
    var gutter = document.createElement("span");
    gutter.className = "cb-gutter";
    gutter.setAttribute("aria-hidden", "true");
    var nums = "";
    for (var ln = 1; ln <= count; ln++) { nums += ln + "\n"; }
    gutter.textContent = nums;
    var pre = codeEl.parentNode;
    pre.classList.add("cb-numbered");
    pre.insertBefore(gutter, codeEl);
  }

  /* ---------------- code block labels ---------------- */
  // Language labels read well in small caps; a file path does not.
  (function () {
    var labels = document.querySelectorAll(".cb-label");
    for (var i = 0; i < labels.length; i++) {
      if (/[\/\\]/.test(labels[i].textContent)) { labels[i].classList.add("cb-file"); }
    }
  }());

  /* ---------------- copy buttons ---------------- */
  // The gutter is a sibling of <code>, so code.textContent is already clean.
  (function () {
    var cbs = document.querySelectorAll(".codeblock:not(.cb-output):not(.cb-capture)");
    for (var i = 0; i < cbs.length; i++) {
      (function (block) {
        var code = block.querySelector("pre > code");
        if (!code) return;
        var btn = document.createElement("button");
        btn.type = "button";
        btn.className = "cb-copy";
        btn.textContent = "copy";
        btn.setAttribute("aria-label", "Copy code");
        btn.addEventListener("click", function () {
          var body = code.textContent.replace(/\n$/, "");
          var done = function () {
            btn.textContent = "Copied"; btn.classList.add("done");
            setTimeout(function () { btn.textContent = "Copy"; btn.classList.remove("done"); }, 1500);
          };
          var fallback = function () {
            var range = document.createRange();
            range.selectNodeContents(code);
            var selection = window.getSelection();
            selection.removeAllRanges();
            selection.addRange(range);
            btn.textContent = "Ctrl+C";
          };
          if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(body).then(done, fallback);
          } else { fallback(); }
        });
        block.appendChild(btn);
      }(cbs[i]));
    }
  }());

  /* ---------------- tabs ---------------- */
  // Without script every panel shows, stacked, and the tab strip stays hidden.
  (function () {
    var groups = document.querySelectorAll(".tabs");
    for (var g = 0; g < groups.length; g++) {
      (function (group) {
        var list = group.querySelector('[role="tablist"]');
        if (!list) return;
        var tabs = list.querySelectorAll('[role="tab"]');
        var select = function (index, focus) {
          for (var i = 0; i < tabs.length; i++) {
            var on = i === index;
            tabs[i].setAttribute("aria-selected", on ? "true" : "false");
            tabs[i].tabIndex = on ? 0 : -1;
            var panel = document.getElementById(tabs[i].getAttribute("aria-controls"));
            if (panel) { panel.hidden = !on; }
          }
          if (focus) { tabs[index].focus(); }
        };
        var showTarget = function () {
          var id = decodeURIComponent(location.hash.slice(1));
          var target = id && document.getElementById(id);
          if (!target || !group.contains(target)) return;
          for (var i = 0; i < tabs.length; i++) {
            var panel = document.getElementById(tabs[i].getAttribute("aria-controls"));
            if (panel && panel.contains(target)) { select(i, false); target.scrollIntoView(); return; }
          }
        };
        for (var i = 0; i < tabs.length; i++) {
          (function (index) {
            tabs[index].addEventListener("click", function () { select(index, false); });
            tabs[index].addEventListener("keydown", function (e) {
              var next = -1;
              if (e.key === "ArrowRight") next = (index + 1) % tabs.length;
              else if (e.key === "ArrowLeft") next = (index + tabs.length - 1) % tabs.length;
              else if (e.key === "Home") next = 0;
              else if (e.key === "End") next = tabs.length - 1;
              if (next < 0) return;
              e.preventDefault();
              select(next, true);
            });
          }(i));
        }
        list.hidden = false;
        select(0, false);
        showTarget();
        window.addEventListener("hashchange", showTarget);
      }(groups[g]));
    }
  }());

  /* ---------------- code + output pairs ---------------- */
  // A sample and the output verified under it become one .pair; the stylesheet puts them
  // side by side when both longest lines fit the column and stacks them otherwise.
  (function () {
    function widest(el) {
      var code = el.querySelector("pre > code");
      if (!code) return 0;
      var lines = code.textContent.replace(/\n$/, "").split("\n"), w = 0;
      for (var i = 0; i < lines.length; i++) {
        var n = lines[i].replace(/\t/g, "    ").length;
        if (n > w) w = n;
      }
      return w;
    }
    var outs = document.querySelectorAll(".codeblock.cb-output");
    for (var i = 0; i < outs.length; i++) {
      var out = outs[i], code = out.previousElementSibling;
      if (!code || !code.classList.contains("codeblock") ||
          !(code.classList.contains("cb-jai") || code.classList.contains("cb-cpp"))) continue;
      var pairEl = document.createElement("div");
      pairEl.className = "pair";
      pairEl.style.setProperty("--code-ch", Math.max(widest(code), 20));
      pairEl.style.setProperty("--out-ch", Math.max(widest(out), 12));
      code.parentNode.insertBefore(pairEl, code);
      pairEl.appendChild(code);
      pairEl.appendChild(out);
    }
  }());

  /* ---------------- in-page table of contents ---------------- */
  (function () {
    var narrow = window.matchMedia && window.matchMedia("(max-width: 900px)").matches;
    if (narrow) {
      var boxes = document.querySelectorAll(".sidebar details");
      for (var d = 0; d < boxes.length; d++) { boxes[d].open = false; }
    }
    var host = document.querySelector("[data-page-toc]");
    var article = document.querySelector("article.chapter");
    if (!host || !article) return;
    var sel = host.hasAttribute("data-toc-h3") ? "h2[id], h3[id]" : "h2[id]";
    var heads = article.querySelectorAll(sel);
    if (heads.length < 3) return;
    var links = [];
    for (var i = 0; i < heads.length; i++) {
      var h = heads[i];
      var clone = h.cloneNode(true);
      var anchor = clone.querySelector(".hlink");
      if (anchor) anchor.parentNode.removeChild(anchor);
      var li = document.createElement("li");
      if (h.tagName === "H3") li.className = "lvl3";
      var a = document.createElement("a");
      a.href = "#" + h.id;
      a.textContent = clone.textContent.trim();
      li.appendChild(a);
      host.appendChild(li);
      links.push(a);
    }
    if (!("IntersectionObserver" in window)) return;
    var io = new IntersectionObserver(function (entries) {
      for (var e = 0; e < entries.length; e++) {
        if (!entries[e].isIntersecting) continue;
        var want = "#" + entries[e].target.id;
        for (var j = 0; j < links.length; j++) {
          links[j].classList.toggle("active", links[j].getAttribute("href") === want);
        }
      }
    }, { rootMargin: "-15% 0px -75% 0px" });
    for (var k = 0; k < heads.length; k++) io.observe(heads[k]);
  }());

  /* ---------------- search ---------------- */
  var box = document.getElementById("search-box");
  var results = document.getElementById("search-results");
  var index = window.SEARCH_INDEX || [];
  // Compute path prefix back to site root from the stylesheet href.
  var cssHref = (document.querySelector('link[rel="stylesheet"]') || {}).getAttribute
    ? document.querySelector('link[rel="stylesheet"]').getAttribute("href") : "assets/style.css";
  var rootPrefix = cssHref.replace(/assets\/style\.css$/, "");
  var sel = -1;

  function expand(on) {
    results.hidden = !on;
    if (box) box.setAttribute("aria-expanded", on ? "true" : "false");
    if (!on && box) box.removeAttribute("aria-activedescendant");
  }

  function render(items) {
    if (!items.length) {
      results.innerHTML = '<div class="sr-none">no matches</div>';
      expand(true); sel = -1; return;
    }
    var htmlOut = "";
    for (var i = 0; i < items.length; i++) {
      var it = items[i];
      htmlOut += '<a role="option" id="sr-' + i + '" href="' + rootPrefix + it.u + (it.a ? "#" + it.a : "") + '">' +
        '<div class="sr-title">' + esc(it.label) + "</div>" +
        '<div class="sr-ctx">' + esc(it.ctx) + "</div></a>";
    }
    results.innerHTML = htmlOut; expand(true); sel = -1;
  }

  function search(q) {
    q = q.toLowerCase().trim();
    if (!q) { expand(false); return; }
    var hits = [];
    for (var i = 0; i < index.length && hits.length < 12; i++) {
      var e = index[i];
      if (e.t.toLowerCase().indexOf(q) >= 0) {
        hits.push({ u: e.u, label: e.t, ctx: e.s });
        continue;
      }
      for (var h = 0; h < e.h.length; h++) {
        if (e.h[h].toLowerCase().indexOf(q) >= 0) {
          hits.push({ u: e.u, label: e.h[h], ctx: e.s + " › " + e.t,
                      a: (e.i && e.i[h]) ||
                         e.h[h].toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "") });
          break;
        }
      }
    }
    render(hits);
  }

  if (box && results) {
    box.addEventListener("input", function () { search(box.value); });
    box.addEventListener("keydown", function (ev) {
      var links = results.querySelectorAll("a");
      if (ev.key === "ArrowDown" || ev.key === "ArrowUp") {
        ev.preventDefault();
        if (!links.length) return;
        sel = ev.key === "ArrowDown" ? Math.min(sel + 1, links.length - 1) : Math.max(sel - 1, 0);
        for (var i = 0; i < links.length; i++) links[i].className = i === sel ? "sel" : "";
        links[sel].scrollIntoView({ block: "nearest" });
        box.setAttribute("aria-activedescendant", "sr-" + sel);
      } else if (ev.key === "Enter") {
        if (sel >= 0 && links[sel]) { window.location.href = links[sel].getAttribute("href"); }
        else if (links.length === 1) { window.location.href = links[0].getAttribute("href"); }
      } else if (ev.key === "Escape") {
        expand(false); box.blur();
      }
    });
    document.addEventListener("click", function (ev) {
      if (!results.contains(ev.target) && ev.target !== box) expand(false);
    });
    // "/" focuses search from anywhere that is not itself a text field
    document.addEventListener("keydown", function (ev) {
      if (ev.key !== "/" || ev.ctrlKey || ev.metaKey || ev.altKey) return;
      var t = ev.target, tag = t && t.tagName;
      if (tag === "INPUT" || tag === "TEXTAREA" || (t && t.isContentEditable)) return;
      ev.preventDefault(); box.focus(); box.select();
    });
  }
}());
