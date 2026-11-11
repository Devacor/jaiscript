#!/usr/bin/env node
/* Runs the site's search engine (assets/search.js) over the built indexes and checks where known queries land.

       node site_tools/test_search.js [--show]

   Each case is [query, N, url prefix]: a result with that url must sit in one of the first N page groups. --show prints every
   query's top results. Exit code 1 when a case fails. */
"use strict";

var path = require("path");
var assets = path.join(__dirname, "..", "site", "assets");
global.window = {};
require(path.join(assets, "search-index.js"));
require(path.join(assets, "search-text.js"));
var search = require(path.join(assets, "search.js"));

var CASES = [
  ["exception", 3, "guide/05-control-flow.html#"],
  ["try catch", 2, "guide/05-control-flow.html#"],
  ["hot reload", 1, "embedding/07-hot-reload.html"],
  ["enum", 2, "guide/11-kitchen-sink.html"],
  ["fallthrough", 2, "guide/05-control-flow.html#"],
  ["lost write", 2, "guide/03-value-semantics.html#lost-writes"],
  ["parallel_for", 2, "guide/16-parallel.html"],
  ["parallel_f", 2, "guide/16-parallel.html"],
  ["Cannot access 'x' on null", 1, "reference/errors.html#err-cannot-access-member-on-null"],
  ["Cannot access 'player' on null ('hero' is null)", 1, "reference/errors.html#err-cannot-access-member-on-null-variable"],
  ["cannot assign 'string' to 'x' declared 'int'", 1, "reference/errors.html#err-"],
  ["Cannot access private member 'hp' of class 'Monster'", 1, "reference/errors.html#err-cannot-access-private-member"],
  ["coroutines", 1, "guide/08-coroutines.html"],
  ["Control Flow", 1, "guide/05-control-flow.html"],
  ["dynamic_binder", 1, "embedding/02-dynamic-binder.html"],
  ["shared_ptr", 3, "guide/03-value-semantics.html"],
  ["sandbox", 3, "play/index.html"]
];

function main() {
  var show = process.argv.indexOf("--show") >= 0;
  var titles = search.create(window.SEARCH_INDEX);
  var early = titles.query("hot reload", 8);
  var failed = 0;
  if (!early || !early.rows.length || early.rows[0].url.indexOf("embedding/07-hot-reload.html") !== 0) {
    console.log("FAILED before the text loads: \"hot reload\" -> " + (early && early.rows.length ? early.rows[0].url : "nothing"));
    failed++;
  }
  var engine = search.create(window.SEARCH_INDEX);
  engine.setText(window.SEARCH_TEXT);
  var slowest = 0;
  CASES.forEach(function (test) {
    var started = process.hrtime.bigint();
    var found = engine.query(test[0], 8);
    slowest = Math.max(slowest, Number(process.hrtime.bigint() - started) / 1e6);
    var rows = found ? found.rows : [], rank = -1, group = -1;
    for (var i = 0; i < rows.length && rank < 0; i++) {
      if (rows[i].first) group++;
      if (rows[i].url.indexOf(test[2]) === 0) rank = group;
    }
    var ok = rank >= 0 && rank < test[1] && !found.partial;
    if (!ok) failed++;
    if (!ok || show) {
      console.log((ok ? "ok      " : "FAILED  ") + JSON.stringify(test[0]) + " expects " + test[2] + " in the top " + test[1]
        + (found && found.partial ? " (closest matches only)" : ""));
      rows.slice(0, show ? 8 : 5).forEach(function (row, n) {
        if (row.first) console.log("  " + row.group);
        console.log("    " + (n + 1) + ". " + row.url + "  [" + row.label + "]  "
          + row.snippet.map(function (piece) { return piece[1] ? "<" + piece[0] + ">" : piece[0]; }).join("").slice(0, 150));
      });
    }
  });
  if (engine.query("   ", 8) !== null) { console.log("FAILED  a blank query returns results"); failed++; }
  var none = engine.query("zzqx", 8);
  if (!none || none.rows.length) { console.log("FAILED  a word the site lacks returns results"); failed++; }
  console.log("search: %d of %d queries land, slowest %s ms", CASES.length + 3 - failed, CASES.length + 3, slowest.toFixed(1));
  return failed ? 1 : 0;
}

process.exit(main());
