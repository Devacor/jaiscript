/* Landing-page hot-reload demo: the sliders and the say toggle edit gator.jai, the shown file reloads, and its
   hot_reload_migrate hook copies the edited field onto the live gator. Once the engine (WebAssembly) loads, the shown text
   is the file it runs: every frame calls stage.jai's frame(), and an edit re-runs gator.jai by its path, a hot reload.
   Until then, and where the engine cannot load, the same scene runs in JavaScript. */
(function () {
  "use strict";
  var demo = document.getElementById("hero-demo");
  if (!demo) return;
  var spdIn = document.getElementById("demo-spd");
  var slicesIn = document.getElementById("demo-slices");
  var sayIns = demo.querySelectorAll('input[name="demo-say"]');
  var spdVal = document.getElementById("demo-spd-val");
  var slicesVal = document.getElementById("demo-slices-val");
  var sayVal = document.getElementById("demo-say-val");
  var hookEl = document.getElementById("demo-hook");
  var status = document.getElementById("demo-status");
  var label = document.getElementById("demo-label");
  var live = document.getElementById("demo-live");
  var code = demo.querySelector("pre");
  var engineNote = document.getElementById("demo-engine");
  var cv = document.getElementById("demo-scene");
  var g = cv && cv.getContext && cv.getContext("2d");
  if (!g || !spdIn || !slicesIn || !sayIns.length) return;

  var reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)");
  var still = !!(reduce && reduce.matches);
  var TICK = 1000 / 60, START = 30, DEBOUNCE = 250, CAKE_SLICES = 6, SLICE_W = 10, CHEW = [0, 5, 10, 5];
  var gator = { spd: Number(spdIn.value), slices: Number(slicesIn.value), x: START, says: checkedSay() };
  var frameNo = 0, nom = 0, lastTs = 0, acc = 0;
  var visible = true, running = false, statusTimer = 0, editTimer = 0;
  var colors = {};
  var engine = null, IDLE = "running";
  var GATOR = "#3fae5a", GATOR_DARK = "#2a7a3d", BELLY = "#c9e7a1", MOUTH = "#7a2430";
  var HOOKS = {
    spd: 'gator.spd = <span class="tk-fn">Gator</span>().spd;',
    slices: 'gator.slices = <span class="tk-fn">Gator</span>().slices;'
  };

  function checkedSay() {
    for (var i = 0; i < sayIns.length; i++) {
      if (sayIns[i].checked) return sayIns[i].value;
    }
    return sayIns[0].value;
  }

  function readColors() {
    var cs = getComputedStyle(document.documentElement);
    function token(name, fallback) { return cs.getPropertyValue(name).trim() || fallback; }
    colors.ground = token("--border", "#252b3d");
    colors.bubble = token("--panel", "#141826");
    colors.bubbleEdge = token("--border-strong", "#3d4560");
    colors.words = token("--head", "#f1f3f8");
  }

  function cakeX() { return cv.width - 110; }
  // The gator stops at the cake's left edge, which moves right as slices go.
  function goal() { return cakeX() + (CAKE_SLICES - Math.max(gator.slices, 1)) * SLICE_W - 50; }

  // The upper jaw hinges at the back of the snout: open is how far its tip rises, so the mouth opens as a "<".
  function drawGator(px, py, open) {
    var leg = (((px - START) / 20 | 0) % 2) * 3, jaw = px + 18, JAW_W = 30, STRIP = 3;
    function lift(x) { return Math.round(open * (x - jaw + STRIP) / JAW_W); }
    g.fillStyle = GATOR_DARK;
    g.fillRect(px - 34, py + 10, 14, 6); g.fillRect(px - 44, py + 12, 12, 4); g.fillRect(px - 52, py + 13, 10, 3);
    g.fillStyle = GATOR; g.fillRect(px - 22, py, 44, 18);
    g.fillStyle = BELLY; g.fillRect(px - 18, py + 13, 36, 5);
    g.fillStyle = GATOR_DARK; for (var b = 0; b < 4; b++) g.fillRect(px - 16 + b * 10, py - 4, 6, 4);
    for (var s = jaw; s < jaw + JAW_W; s += STRIP) {
      var up = lift(s);
      if (up > 0) { g.fillStyle = MOUTH; g.fillRect(s, py + 10 - up, STRIP, up); }
      g.fillStyle = GATOR; g.fillRect(s, py + 2 - up, STRIP, 8);
    }
    g.fillStyle = GATOR_DARK; g.fillRect(jaw, py + 10, JAW_W, 3);
    g.fillStyle = "#ffffff";
    for (var t = 0; t < 4; t++) {
      var tooth = px + 22 + t * 7;
      g.fillRect(tooth, py + 9 - lift(tooth), 3, 3);
      if (lift(tooth + 3) >= 3) g.fillRect(tooth + 3, py + 8, 3, 2);
    }
    g.fillStyle = GATOR; g.fillRect(px + 14, py - 6, 10, 8);
    g.fillStyle = "#f5f2e8"; g.fillRect(px + 18, py - 4, 4, 4);
    g.fillStyle = "#111111"; g.fillRect(px + 20, py - 3, 2, 2);
    g.fillStyle = GATOR_DARK;
    g.fillRect(px - 16, py + 18, 6, 6 - leg); g.fillRect(px + 8, py + 18, 6, 3 + leg);
  }

  // The cake has room for CAKE_SLICES slices; the gator eats from the left, and the candle sits on the last slice.
  function cake(cx, cy, left) {
    if (left <= 0) return;
    var flick = (frameNo >> 3) % 2, full = CAKE_SLICES * SLICE_W, from = cx + full - left * SLICE_W;
    g.fillStyle = "#8b5a2b"; g.fillRect(from, cy + 14, left * SLICE_W, 16);
    g.fillStyle = "#f6d3e0"; g.fillRect(from, cy + 6, left * SLICE_W, 10);
    g.fillStyle = "#ffffff"; for (var d = from; d < cx + full; d += SLICE_W) g.fillRect(d + 3, cy + 15, 5, 4);
    g.fillStyle = "#e94b6a"; g.fillRect(cx + full - 7, cy - 8, 4, 14);
    g.fillStyle = flick ? "#ffd84a" : "#ff9f1a"; g.fillRect(cx + full - 8, cy - 15 - flick, 6, 7);
  }

  function bubble(headX, headY, words) {
    g.font = '600 15px "JetBrains Mono", Consolas, monospace';
    var w = Math.ceil(g.measureText(words).width) + 16, h = 24;
    var x = Math.max(4, Math.min(cv.width - w - 4, headX - 12)), y = headY - h - 12;
    var tail = Math.max(x + 6, Math.min(x + w - 14, headX - 4));
    g.fillStyle = colors.bubble; g.strokeStyle = colors.bubbleEdge; g.lineWidth = 2;
    g.beginPath(); g.moveTo(tail, y + h); g.lineTo(tail + 8, y + h); g.lineTo(tail + 2, y + h + 8); g.closePath();
    g.fill(); g.stroke();
    g.fillRect(x, y, w, h); g.strokeRect(x, y, w, h);
    g.fillRect(tail + 1, y + h - 2, 6, 3);
    g.fillStyle = colors.words; g.textBaseline = "middle";
    g.fillText(words, x + 8, y + h / 2 + 1);
  }

  function setLabel() {
    var mood = gator.spd <= 0 ? " (standing still)" : gator.spd > 1 ? " (cake rush)" : "";
    label.textContent = "gator at spd " + gator.spd + " · slices " + gator.slices + mood;
  }

  // A still frame (reduced motion) shows the gator at the cake, saying its line.
  function draw() {
    var w = cv.width, x = still ? goal() : gator.x;
    g.clearRect(0, 0, w, cv.height);
    g.fillStyle = colors.ground;
    for (var i = 0; i < w; i += 32) g.fillRect(i, 118, 28, 6);
    cake(cakeX(), 88, gator.slices);
    drawGator(x, 92, nom > 0 ? CHEW[(frameNo >> 2) % CHEW.length] : 0);
    if (nom > 0 || still) bubble(x + 18, 86, gator.says);
  }

  // stage.jai's state(): "x spd slices chewing words"
  function adopt(state) {
    var parts = state.split(" ");
    gator.x = Number(parts[0]);
    gator.spd = Number(parts[1]);
    gator.slices = Number(parts[2]);
    nom = Number(parts[3]);
    gator.says = parts.slice(4).join(" ") || gator.says;
  }

  function engineCall(name, arg) {
    var reply = engine.ccall(name, "string", ["string"], [arg]);
    return engine.ccall("jaiweb_live_failed", "number", [], []) ? { error: reply } : { value: reply };
  }

  function tick() {
    frameNo++;
    if (engine) {
      var shown = nom;
      var step = engineCall("jaiweb_live_eval", "frame()");
      if (step.error) { engine = null; return; }
      adopt(step.value);
      if ((nom === 0) !== (shown === 0)) setLabel();
      return;
    }
    if (nom > 0) {
      if (--nom === 0) {
        gator.x = START;
        if (gator.slices <= 0) gator.slices = Number(slicesIn.value);
        setLabel();
      }
      return;
    }
    gator.x = Math.max(START, Math.min(goal(), gator.x + gator.spd));
    if (gator.x >= goal()) { gator.slices--; nom = 50; setLabel(); }
  }

  function frame(ts) {
    if (!running) { lastTs = 0; return; }
    acc += lastTs ? Math.min(ts - lastTs, 250) : TICK;
    lastTs = ts;
    for (; acc >= TICK; acc -= TICK) tick();
    draw();
    requestAnimationFrame(frame);
  }

  function sync() {
    var want = !still && visible && !document.hidden;
    if (want && !running) { running = true; requestAnimationFrame(frame); }
    else if (!want) running = false;
  }

  function flash(el) {
    el.classList.remove("flash");
    void el.offsetWidth;
    el.classList.add("flash");
    setTimeout(function () { el.classList.remove("flash"); }, 900);
  }

  function setStatus(text) {
    clearTimeout(statusTimer);
    status.textContent = text;
    status.classList.remove("idle");
    statusTimer = setTimeout(function () { status.textContent = IDLE; status.classList.add("idle"); }, 1800);
  }

  // The engine re-runs gator.jai as the page shows it; a field edit reaches the live gator only through the hook
  function reloadInEngine(field) {
    engine.FS.writeFile("/gator/gator.jai", code.textContent);
    var began = performance.now();
    var result = engineCall("jaiweb_live_file", "/gator/gator.jai");
    var ms = (performance.now() - began).toFixed(1);
    if (result.error) return "reload failed: the gator keeps the code it had";
    adopt(engineCall("jaiweb_live_eval", "state()").value);
    if (field === "say") return "reloaded in " + ms + " ms · tick() updated, slices kept";
    return "reloaded in " + ms + " ms · " + (field === "spd" ? "slices kept" : "live gator took slices " + gator.slices);
  }

  // The file as the controls show it, reloaded: a field edit goes through the hook, a say edit changes tick() itself.
  function reload(field, fromVisitor) {
    var message;
    showSpd();
    showSlices();
    showSay();
    if (field !== "say") {
      hookEl.innerHTML = HOOKS[field];
      flash(hookEl);
    }
    if (engine) {
      setStatus(reloadInEngine(field));
      setLabel();
      if (fromVisitor) live.textContent = "Reloaded: spd " + gator.spd + ", slices " + gator.slices + ", says " + gator.says;
      if (!running) draw();
      return;
    }
    if (field === "say") {
      gator.says = checkedSay();
      message = "reloaded · tick() updated, slices kept";
    } else {
      if (field === "spd") {
        gator.spd = Number(spdIn.value);
        message = "reloaded · slices kept";
      } else {
        gator.slices = Number(slicesIn.value);
        message = "reloaded · live gator took slices " + gator.slices;
      }
    }
    setStatus(message);
    setLabel();
    if (fromVisitor) live.textContent = "Reloaded: spd " + gator.spd + ", slices " + gator.slices + ", says " + gator.says;
    if (!running) draw();
  }

  function showSpd() { spdVal.textContent = spdIn.value; }
  function showSlices() { slicesVal.textContent = slicesIn.value; }
  function showSay() { sayVal.textContent = '"' + checkedSay() + '"'; }

  function onSlider(input, field, show, valEl) {
    input.addEventListener("input", function () {
      show();
      flash(valEl);
      clearTimeout(editTimer);
      editTimer = setTimeout(function () { reload(field, true); }, DEBOUNCE);
    });
    input.addEventListener("change", function () {
      clearTimeout(editTimer);
      reload(field, true);
    });
  }

  onSlider(spdIn, "spd", showSpd, spdVal);
  onSlider(slicesIn, "slices", showSlices, slicesVal);
  for (var i = 0; i < sayIns.length; i++) {
    sayIns[i].addEventListener("change", function () {
      showSay();
      flash(sayVal);
      reload("say", true);
    });
  }

  // The engine is 6 MB of WebAssembly: it loads after the page, and not at all where the visitor asks to save data
  function loadEngine() {
    var src = demo.getAttribute("data-engine"), stage = demo.getAttribute("data-stage");
    var saving = navigator.connection && navigator.connection.saveData;
    if (!src || !stage || saving || typeof WebAssembly !== "object") return;
    var script = document.createElement("script");
    script.src = src;
    script.onload = function () {
      var files = ["world.jai", "stage.jai"].map(function (name) {
        return fetch(stage + name).then(function (response) {
          if (!response.ok) throw new Error(name + ": " + response.status);
          return response.text();
        });
      });
      Promise.all([window.createJaiScript({ print: function () {}, printErr: function () {} })].concat(files)).then(function (loaded) {
        var jai = loaded[0];
        jai.FS.mkdir("/gator");
        jai.FS.writeFile("/gator/world.jai", loaded[1]);
        jai.FS.writeFile("/gator/stage.jai", loaded[2]);
        jai.FS.writeFile("/gator/gator.jai", code.textContent);
        engine = jai;
        var steps = ["/gator/world.jai", "/gator/gator.jai", "/gator/stage.jai"];
        for (var s = 0; s < steps.length; s++) {
          var result = engineCall("jaiweb_live_file", steps[s]);
          if (result.error) throw new Error(result.error);
        }
        adopt(engineCall("jaiweb_live_eval", "state()").value);
        IDLE = "live in WebAssembly";
        status.textContent = IDLE;
        if (engineNote) engineNote.hidden = false;
        setLabel();
        if (!running) draw();
      }).catch(function (error) {
        engine = null;
        if (window.console) console.warn("gator.jai stays in JavaScript:", error);
      });
    };
    document.head.appendChild(script);
  }

  function whenIdle() {
    if (window.requestIdleCallback) requestIdleCallback(loadEngine, { timeout: 3000 });
    else setTimeout(loadEngine, 500);
  }

  showSpd();
  showSlices();
  showSay();
  readColors();
  setLabel();
  draw();
  new MutationObserver(function () { readColors(); if (!running) draw(); })
    .observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
  if (reduce && reduce.addEventListener) {
    reduce.addEventListener("change", function (e) { still = e.matches; sync(); if (!running) draw(); });
  }
  document.addEventListener("visibilitychange", sync);
  if ("IntersectionObserver" in window) {
    new IntersectionObserver(function (entries) { visible = entries[0].isIntersecting; sync(); }).observe(demo);
  }
  sync();
  if (document.readyState === "complete") whenIdle();
  else window.addEventListener("load", whenIdle);
}());
