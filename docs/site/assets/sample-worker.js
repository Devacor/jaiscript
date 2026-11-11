/* Runs the site's samples (samples.js) on the engine as WebAssembly: the plain build, which needs no shared memory, so any
   page can start it. Each run starts from an empty /sample folder holding the sample and the page's support files, on a
   fresh engine; {out} carries output as it is printed and {exit} the exit code. A sleep() spins this worker, never the page. */
"use strict";

var ENGINE = new URL("../play/engine/plain/", self.location.href).href;
var FOLDER = "/sample";
importScripts(ENGINE + "jaiscript_web_worker.js");

var ready = createJaiScript({
  locateFile: function (path) { return ENGINE + path; },
  jaiwebWrite: function (bytes) { self.postMessage({ out: bytes }, [bytes.buffer]); }
});

// What the last run left (its files and whatever it wrote) goes before the next one starts
function empty(jai, folder) {
  jai.FS.readdir(folder).forEach(function (name) {
    if (name === "." || name === "..") return;
    var path = folder + "/" + name;
    if (jai.FS.isDir(jai.FS.stat(path).mode)) {
      empty(jai, path);
      jai.FS.rmdir(path);
    } else {
      jai.FS.unlink(path);
    }
  });
}

self.onmessage = function (message) {
  ready.then(function (jai) {
    var data = message.data;
    jai.FS.chdir("/");
    jai.FS.mkdirTree(FOLDER);
    empty(jai, FOLDER);
    Object.keys(data.files).forEach(function (name) {
      var full = FOLDER + "/" + name;
      jai.FS.mkdirTree(full.slice(0, full.lastIndexOf("/")));
      jai.FS.writeFile(full, data.files[name]);
    });
    jai.FS.chdir(FOLDER);
    self.postMessage({ exit: jai.ccall("jaiweb_run_file", "number", ["string", "string", "number", "number"], [data.main, "", data.budget, 1]) });
  }).catch(function (error) {
    self.postMessage({ failed: String(error && error.message || error) });
  });
};
