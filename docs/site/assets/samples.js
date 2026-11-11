/* Runnable samples: every block mark_runnable.py marked data-run gets Run and Reset beside Copy, and its code (and every
   support file a page's samples load, data-file) edits in place. Run sends the block and the page's support files, as they
   read now, to sample-worker.js, which runs them on the engine compiled to WebAssembly; the output fills the sample's
   output block until Reset puts the page's text back. Samples run one at a time, in the order they were started. */
(function () {
  "use strict";
  var WORKER = new URL("sample-worker.js", document.currentScript.src).href;
  var BUDGET = 20;
  var LIMIT = 200000;
  var SGR = new RegExp(String.fromCharCode(27) + "\\[([0-9;?]*)([A-Za-z])", "g");
  var worker = null, running = null, waiting = [];

  function codeOf(block) { return block.querySelector("pre > code"); }

  // An edit can add or drop lines: the gutter follows, when the block has one
  function renumber(code) {
    var gutter = code.parentNode.querySelector(".cb-gutter");
    if (!gutter) return;
    var count = code.textContent.replace(/\n$/, "").split("\n").length, nums = "";
    for (var n = 1; n <= count; n++) nums += n + "\n";
    gutter.textContent = nums;
  }

  function typeText(code, text) {
    if (document.execCommand && document.execCommand("insertText", false, text)) return;
    var selection = window.getSelection();
    if (!selection.rangeCount) return;
    var range = selection.getRangeAt(0);
    range.deleteContents();
    range.insertNode(document.createTextNode(text));
    range.collapse(false);
    selection.removeAllRanges();
    selection.addRange(range);
    code.dispatchEvent(new Event("input", { bubbles: true }));
  }

  function makeEditable(block, onRun) {
    var code = codeOf(block);
    if (!code) return null;
    var original = { html: code.innerHTML, text: code.textContent };
    code.setAttribute("contenteditable", "plaintext-only");
    if (code.contentEditable !== "plaintext-only") code.setAttribute("contenteditable", "true");
    code.setAttribute("spellcheck", "false");
    code.setAttribute("autocapitalize", "off");
    code.setAttribute("aria-label", "Sample code, editable");
    var painted = original.text;
    code.addEventListener("input", function () {
      renumber(code);
      block.classList.toggle("cb-edited", code.textContent !== original.text);
    });
    code.addEventListener("blur", function () {
      if (code.textContent === painted || !code.classList.contains("lang-jai") || !window.JaiDocs) return;
      painted = code.textContent;
      code.innerHTML = window.JaiDocs.highlight(painted);
    });
    // Tab types a tab. Escape then Tab, or Shift+Tab, moves focus on, so the keyboard can still leave the sample
    var leaving = false;
    code.addEventListener("keydown", function (event) {
      if (event.key === "Enter" && (event.ctrlKey || event.metaKey) && onRun) {
        event.preventDefault();
        onRun();
      } else if (event.key === "Escape") {
        leaving = true;
        return;
      } else if (event.key === "Tab" && !leaving && !event.shiftKey && !event.ctrlKey && !event.altKey && !event.metaKey) {
        event.preventDefault();
        typeText(code, "\t");
      }
      leaving = false;
    });
    code.addEventListener("blur", function () { leaving = false; });
    return function reset() {
      code.innerHTML = original.html;
      painted = original.text;
      renumber(code);
      block.classList.remove("cb-edited");
    };
  }

  function button(name, text, label) {
    var result = document.createElement("button");
    result.type = "button";
    result.className = "cb-tool cb-" + name;
    result.textContent = text;
    result.setAttribute("aria-label", label);
    return result;
  }

  // The sample's own output block, or one made for it under the sample
  function outputFor(block) {
    var next = block.nextElementSibling;
    if (next && next.classList.contains("cb-output")) {
      var code = codeOf(next), label = next.querySelector(".cb-label");
      return { block: next, code: code, label: label, made: false,
        original: { html: code.innerHTML, label: label ? label.textContent : "" } };
    }
    var made = document.createElement("div");
    made.className = "codeblock cb-output";
    made.innerHTML = '<span class="cb-label">output</span><pre><code class="lang-output"></code></pre>';
    block.parentNode.insertBefore(made, block.nextSibling);
    return { block: made, code: codeOf(made), label: made.querySelector(".cb-label"), made: true };
  }

  // The engine's text: a red SGR (an error) shows as an error, every other escape sequence is dropped
  function paint(target, text) {
    target.textContent = "";
    var red = false, last = 0, found;
    function emit(piece) {
      if (!piece) return;
      if (red) {
        var span = document.createElement("span");
        span.className = "run-error";
        span.textContent = piece;
        target.appendChild(span);
      } else {
        target.appendChild(document.createTextNode(piece));
      }
    }
    SGR.lastIndex = 0;
    while ((found = SGR.exec(text))) {
      emit(text.slice(last, found.index));
      last = SGR.lastIndex;
      if (found[2] !== "m") continue;
      var codes = found[1].split(";");
      for (var i = 0; i < codes.length; i++) {
        if (codes[i] === "31") red = true;
        else if (codes[i] === "" || codes[i] === "0" || codes[i] === "39") red = false;
      }
    }
    emit(text.slice(last));
  }

  function restore(out) {
    if (out.made) {
      if (out.block.parentNode) out.block.parentNode.removeChild(out.block);
      return;
    }
    out.code.innerHTML = out.original.html;
    if (out.label) out.label.textContent = out.original.label;
    out.block.classList.remove("cb-ran");
  }

  function idle(job) {
    job.run.textContent = "▶ run";
    job.run.setAttribute("aria-label", "Run the sample");
  }

  function finish(job, note) {
    idle(job);
    job.text = job.text.replace(/\r/g, "").replace(/\n+$/, "");
    paint(job.out.code, job.text);
    if (job.out.label) job.out.label.textContent = "output · " + note;
  }

  // One sample runs at a time; a sample started meanwhile waits its turn showing what it showed before
  function next() {
    running = waiting.shift() || null;
    if (!running) return;
    var job = running, decoder = new TextDecoder(), started = performance.now();
    if (!worker) worker = new Worker(WORKER);
    job.out.block.classList.add("cb-ran");
    if (job.out.label) job.out.label.textContent = "running in your browser…";
    job.out.code.textContent = "";
    worker.onmessage = function (message) {
      var data = message.data;
      if (data.out && job.text.length < LIMIT) {
        job.text += decoder.decode(data.out, { stream: true });
        if (job.text.length >= LIMIT) job.text = job.text.slice(0, LIMIT) + "\n[output cut at " + LIMIT + " characters]";
        paint(job.out.code, job.text.replace(/\r/g, ""));
      }
      if (data.exit !== undefined) {
        var ms = Math.round(performance.now() - started);
        finish(job, data.exit === 0 ? "ran in your browser in " + ms + " ms" : "exit code " + data.exit);
        next();
      }
      if (data.failed !== undefined) {
        job.text += "\n" + data.failed;
        worker.terminate();
        worker = null;
        finish(job, "the engine did not load");
        next();
      }
    };
    worker.postMessage({ files: job.files, main: job.main, budget: job.budget });
  }

  // Stops a sample that is running or waiting. One stopped before it printed anything shows what it showed before.
  function cancel(job) {
    var queued = waiting.indexOf(job);
    if (queued < 0 && running !== job) return;
    if (queued >= 0) waiting.splice(queued, 1);
    else if (worker) { worker.terminate(); worker = null; }
    if (job.text) {
      finish(job, "stopped");
    } else {
      idle(job);
      if (job.out.made && !job.ranBefore) {
        restore(job.out);
      } else {
        job.out.code.innerHTML = job.htmlBefore;
        if (job.out.label) job.out.label.textContent = job.labelBefore;
        if (!job.ranBefore) job.out.block.classList.remove("cb-ran");
      }
    }
    if (queued < 0) next();
  }

  function start(block, run, out) {
    var files = {};
    var support = document.querySelectorAll(".codeblock[data-file]");
    for (var i = 0; i < support.length; i++) files[support[i].getAttribute("data-file")] = codeOf(support[i]).textContent;
    var main = block.getAttribute("data-run");
    files[main] = codeOf(block).textContent;
    var job = { run: run, out: out, files: files, main: main, text: "",
      budget: Number(block.getAttribute("data-budget")) || BUDGET,
      htmlBefore: out.code.innerHTML, labelBefore: out.label ? out.label.textContent : "",
      ranBefore: out.block.classList.contains("cb-ran") };
    run.textContent = "■ stop";
    run.setAttribute("aria-label", "Stop the sample");
    waiting.push(job);
    if (running) {
      if (out.label) out.label.textContent = "waiting for the sample that is running…";
    } else {
      next();
    }
    return job;
  }

  var runnable = document.querySelectorAll(".codeblock[data-run]");
  for (var r = 0; r < runnable.length; r++) {
    (function (block) {
      var tools = document.createElement("div");
      tools.className = "cb-tools";
      var run = button("run", "▶ run", "Run the sample");
      var resetButton = button("reset", "reset", "Put the sample and its output back");
      var out = null, job = null;
      var active = function () { return job && (running === job || waiting.indexOf(job) >= 0); };
      var trigger = function () {
        if (active()) { cancel(job); return; }
        if (!out || !out.block.parentNode) out = outputFor(block);
        block.classList.add("cb-touched");
        job = start(block, run, out);
      };
      var resetCode = makeEditable(block, trigger);
      resetButton.addEventListener("click", function () {
        if (active()) cancel(job);
        job = null;
        if (resetCode) resetCode();
        if (out) {
          restore(out);
          out = null;
        }
        block.classList.remove("cb-touched");
      });
      run.addEventListener("click", trigger);
      var copy = block.querySelector(".cb-copy");
      tools.appendChild(resetButton);
      tools.appendChild(run);
      if (copy) tools.appendChild(copy);
      block.appendChild(tools);
    }(runnable[r]));
  }
  var files = document.querySelectorAll(".codeblock[data-file]");
  for (var f = 0; f < files.length; f++) makeEditable(files[f], null);
}());
