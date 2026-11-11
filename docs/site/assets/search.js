/* JaiScript docs search: the engine behind the search box, loaded by site.js when the box is first focused. No DOM here,
   so site_tools/test_search.js runs the same code under Node.

   create(SEARCH_INDEX) searches page titles and headings at once; setText(SEARCH_TEXT) adds every section's body, which
   build_search_index.py dictionary-codes (its header describes the format; codePoints below is the inverse of its
   code_point). A query is the AND of its words, the last one matching as a prefix; a word typed inside quotes (the 'x' of
   a pasted error text) is optional when two or more words stand outside them. Order: page title, heading, the exact
   phrase in the body, the words in order with gaps (an error text whose names differ), every word in the body. When no
   section holds every word, the sections holding most of them are returned as closest matches. */
(function () {
  "use strict";

  var WORD = /[A-Za-z0-9_]+/g;
  var MAX_TERMS = 30;
  var GAP = 6;            // words an ordered match may skip between two query words: a quoted name, a type
  var NEAR = 15;
  var PER_PAGE = 3;
  var SNIPPET = 170;

  function wordsOf(text) { return text.toLowerCase().match(WORD) || []; }

  function bitCount(mask) {
    var result = 0;
    while (mask) { mask &= mask - 1; result++; }
    return result;
  }

  // Words in query order; quoted marks the ones typed inside '...', "..." or `...`
  function parseQuery(query) {
    var result = [], open = "", openAt = 0, i = 0, n = query.length;
    function isWordChar(c) { return /[A-Za-z0-9_]/.test(c); }
    while (i < n) {
      var c = query[i];
      if (isWordChar(c)) {
        var j = i;
        while (j < n && isWordChar(query[j])) j++;
        result.push({ word: query.slice(i, j).toLowerCase(), quoted: false, inside: open !== "" });
        i = j; continue;
      }
      if (c === "'" || c === '"' || c === "`") {
        if (open === c) {
          for (var k = openAt; k < result.length; k++) result[k].quoted = true;
          open = "";
        } else if (open === "" && !(i > 0 && isWordChar(query[i - 1]) && c === "'")) {
          open = c; openAt = result.length;
        }
      }
      i++;
    }
    return result;
  }

  function matches(term, word, prefix) {
    if (word === term) return true;
    if (prefix && word.length > term.length && word.lastIndexOf(term, 0) === 0) return true;
    if (term.length > 2 && (word === term + "s" || word === term + "es" || term === word + "s" || term === word + "es")) return true;
    if (word.indexOf("_") < 0) return false;
    var parts = word.split("_");
    for (var i = 0; i < parts.length; i++) {
      if (parts[i] === term || (prefix && parts[i].length > term.length && parts[i].lastIndexOf(term, 0) === 0)) return true;
    }
    return false;
  }

  function codePoints(count) {
    var result = [], c;
    for (c = 0x20; c < 0x7F && result.length < count; c++) {
      var ch = String.fromCharCode(c);
      if (!/[A-Za-z0-9_"\\]/.test(ch)) result.push(c);
    }
    for (c = 0xA1; c < 0x800 && result.length < count; c++) result.push(c);
    for (c = 0x800; result.length < count; c++) {
      if (c !== 0x2028 && c !== 0x2029) result.push(c);
    }
    return result;
  }

  function create(pages) {
    var sectionPage = [], sectionNumber = [], pageFirst = [], titleWords = [], headWords = [];
    for (var p = 0; p < pages.length; p++) {
      pageFirst.push(sectionPage.length);
      titleWords.push(wordsOf(pages[p].t));
      for (var k = 0; k <= pages[p].h.length; k++) {
        sectionPage.push(p);
        sectionNumber.push(k);
        headWords.push(k ? wordsOf(pages[p].h[k - 1]) : []);
      }
    }
    var sectionCount = sectionPage.length;
    var vocabulary = null, lower = null, isWord = null, tokens = null, starts = null, anchors = null;

    function setText(data) {
      var i;
      vocabulary = data.v;
      lower = new Array(vocabulary.length);
      isWord = new Uint8Array(vocabulary.length);
      for (i = 0; i < vocabulary.length; i++) {
        isWord[i] = /^[A-Za-z0-9_]/.test(vocabulary[i]) ? 1 : 0;
        lower[i] = isWord[i] ? vocabulary[i].toLowerCase() : "";
      }
      var points = codePoints(vocabulary.length), numberOf = new Uint16Array(0x10000);
      for (i = 0; i < points.length; i++) numberOf[points[i]] = i;
      var total = 0, g = 0, s;
      for (var p = 0; p < data.p.length; p++) for (s = 0; s < data.p[p].length; s++) total += data.p[p][s].length;
      tokens = new Uint16Array(total);
      starts = new Int32Array(sectionCount + 1);
      anchors = new Array(sectionCount);
      var at = 0;
      for (p = 0; p < pages.length; p++) {
        var bodies = data.p[p] || [];
        for (s = 0; s <= pages[p].h.length; s++, g++) {
          starts[g] = at;
          var body = bodies[s] || "";
          for (i = 0; i < body.length; i++) tokens[at++] = numberOf[body.charCodeAt(i)];
        }
        var flat = data.a[p] || [];
        for (i = 0; i < flat.length; i += 4) {
          var owner = pageFirst[p] + flat[i];
          (anchors[owner] || (anchors[owner] = [])).push([flat[i + 1], flat[i + 2], flat[i + 3]]);
        }
      }
      starts[sectionCount] = at;
    }

    function fieldMask(words, terms, prefixTerm) {
      var result = 0;
      for (var t = 0; t < terms.length; t++) {
        for (var w = 0; w < words.length; w++) {
          if (matches(terms[t], words[w], t === prefixTerm)) { result |= 1 << t; break; }
        }
      }
      return result;
    }

    // Where in a section the query sits: the phrase, else its required words in order, else their tightest window
    function locate(g, mark, order, requiredOrder, required) {
      var from = starts[g], to = starts[g + 1], at = [], ord = [], bits = [], ordinal = 0, i, k;
      for (i = from; i < to && at.length < 4000; i++) {
        var token = tokens[i];
        if (!isWord[token]) continue;
        if (mark[token]) { at.push(i); ord.push(ordinal); bits.push(mark[token]); }
        ordinal++;
      }
      var count = at.length;
      if (!count) return null;
      if (order.length > 1) {
        for (i = 0; i + order.length <= count; i++) {
          for (k = 0; k < order.length; k++) {
            if (ord[i + k] !== ord[i] + k || !(bits[i + k] & (1 << order[k]))) break;
          }
          if (k === order.length) return { kind: "phrase", from: at[i], to: at[i + k - 1] };
        }
      }
      if (requiredOrder.length > 1) {
        for (i = 0; i < count; i++) {
          if (!(bits[i] & (1 << requiredOrder[0]))) continue;
          var j = i, last = i;
          for (k = 1; k < requiredOrder.length; k++) {
            for (j = last + 1; j < count && ord[j] - ord[last] <= GAP; j++) {
              if (bits[j] & (1 << requiredOrder[k])) break;
            }
            if (j >= count || ord[j] - ord[last] > GAP) break;
            last = j;
          }
          if (k === requiredOrder.length) return { kind: "ordered", from: at[i], to: at[last] };
        }
      }
      var present = 0;
      for (i = 0; i < count; i++) present |= bits[i] & required;
      if (bitCount(present) > 1) {
        var seen = {}, have = 0, need = bitCount(present), left = 0, best = null;
        for (i = 0; i < count; i++) {
          for (k = 0; k < MAX_TERMS; k++) {
            if ((bits[i] & present) & (1 << k)) { seen[k] = (seen[k] || 0) + 1; if (seen[k] === 1) have++; }
          }
          while (have === need) {
            if (!best || ord[i] - ord[left] < best.span) best = { span: ord[i] - ord[left], from: at[left], to: at[i] };
            for (k = 0; k < MAX_TERMS; k++) {
              if ((bits[left] & present) & (1 << k)) { seen[k]--; if (!seen[k]) have--; }
            }
            left++;
          }
        }
        if (best) return { kind: best.span <= NEAR ? "near" : "any", from: best.from, to: best.to };
      }
      for (i = 0; i < count; i++) if (bits[i] & required) return { kind: "any", from: at[i], to: at[i] };
      return { kind: "any", from: at[0], to: at[0] };
    }

    function tokenText(g, i) {
      var token = tokens[i];
      return (i > starts[g] && isWord[token] && isWord[tokens[i - 1]] ? " " : "") + vocabulary[token];
    }

    // [text, matched] pieces around the tokens from..to (the section's opening words when from is negative)
    function snippet(g, mark, from, to) {
      var first = starts[g], end = starts[g + 1], i, length = 0;
      if (first === end) return [];
      var begin = from < 0 ? first : from, stop = from < 0 ? first : Math.min(to + 1, end);
      for (i = begin; i < stop && length <= SNIPPET; i++) length += tokenText(g, i).length;
      if (length > SNIPPET) { stop = begin + 1; length = tokenText(g, begin).length; }
      var back = 0;
      while (begin > first && back < 45 && length < SNIPPET) { begin--; back += tokenText(g, begin).length; length += tokenText(g, begin).length; }
      while (stop < end && length < SNIPPET) { length += tokenText(g, stop).length; stop++; }
      while (begin > first && length < SNIPPET) { begin--; length += tokenText(g, begin).length; }
      if (!isWord[tokens[begin]] && begin + 1 < stop) begin++;
      var result = [], plain = begin > first ? "\u2026 " : "";
      for (i = begin; i < stop; i++) {
        var token = tokens[i], text = tokenText(g, i);
        if (i === begin) text = text.replace(/^\s+/, "");
        if (mark && mark[token]) {
          var lead = text.charAt(0) === " " ? " " : "";
          if (plain + lead) result.push([plain + lead, false]);
          result.push([text.slice(lead.length), true]);
          plain = "";
        } else {
          plain += text;
        }
      }
      plain = plain.replace(/\s+$/, "") + (stop < end ? " \u2026" : "");
      if (plain) result.push([plain, false]);
      return result;
    }

    function anchorAt(g, position) {
      var list = anchors && anchors[g], best = null;
      if (!list) return "";
      for (var i = 0; i < list.length; i++) {
        var offset = position - starts[g];
        if (offset >= list[i][0] && offset < list[i][1] && (!best || list[i][1] - list[i][0] <= best[1] - best[0])) best = list[i];
      }
      return best ? best[2] : "";
    }

    function query(text, limit) {
      var parsed = parseQuery(text), terms = [], order = [], t, g, p;
      if (!parsed.length) return null;
      var outside = 0;
      for (t = 0; t < parsed.length; t++) if (!parsed[t].quoted) outside++;
      var required = 0, optional = 0, prefixTerm = -1;
      for (t = 0; t < parsed.length; t++) {
        var slot = terms.indexOf(parsed[t].word);
        if (slot < 0) {
          if (terms.length === MAX_TERMS) continue;
          slot = terms.push(parsed[t].word) - 1;
        }
        order.push(slot);
        if (parsed[t].quoted && outside >= 2) optional |= 1 << slot; else required |= 1 << slot;
      }
      optional &= ~required;
      var lastWord = parsed[parsed.length - 1];
      if (!lastWord.quoted && !lastWord.inside) prefixTerm = terms.indexOf(lastWord.word);
      var requiredOrder = [];
      for (t = 0; t < order.length; t++) if (required & (1 << order[t])) requiredOrder.push(order[t]);
      var requiredCount = bitCount(required);

      var titleMask = new Int32Array(pages.length), headMask = new Int32Array(sectionCount);
      var bodyMask = new Int32Array(sectionCount), bodyCount = new Int32Array(sectionCount), mark = null;
      for (p = 0; p < pages.length; p++) titleMask[p] = fieldMask(titleWords[p], terms, prefixTerm);
      for (g = 0; g < sectionCount; g++) if (headWords[g].length) headMask[g] = fieldMask(headWords[g], terms, prefixTerm);
      if (tokens) {
        mark = new Int32Array(vocabulary.length);
        var whole = new Uint8Array(vocabulary.length);   // the word itself, not a longer word or name it starts or sits in
        for (var v = 0; v < vocabulary.length; v++) {
          if (!isWord[v]) continue;
          for (t = 0; t < terms.length; t++) {
            if (!matches(terms[t], lower[v], t === prefixTerm)) continue;
            mark[v] |= 1 << t;
            if (matches(terms[t], lower[v].replace(/_/g, ""), false)) whole[v] = 1;
          }
        }
        for (g = 0; g < sectionCount; g++) {
          var mask = 0, count = 0;
          for (var i = starts[g], end = starts[g + 1]; i < end; i++) {
            var m = mark[tokens[i]];
            if (m) { mask |= m; if (m & required) count += 1 + 2 * whole[tokens[i]]; }
          }
          bodyMask[g] = mask; bodyCount[g] = count;
        }
      }

      var hits = [], pageHit = {}, partial = false;
      function collect(need) {
        for (p = 0; p < pages.length; p++) {
          if (need === requiredCount && (titleMask[p] & required) === required) {
            pageHit[p] = { page: p, section: pageFirst[p], whole: true,
              score: 1000 + (titleWords[p].length === bitCount(titleMask[p]) ? 100 : 0) - titleWords[p].length };
            hits.push(pageHit[p]);
          }
        }
        for (g = 0; g < sectionCount; g++) {
          p = sectionPage[g];
          var own = (headMask[g] | bodyMask[g]) & required;
          if (!own || bitCount((own | titleMask[p]) & required) < need) continue;
          if (pageHit[p] && sectionNumber[g] === 0) continue;
          var head = headMask[g] & required, body = bodyMask[g] & required, score = 0;
          if (head === required) score += 500 + (headWords[g].length === bitCount(headMask[g]) ? 60 : 0) - Math.min(headWords[g].length, 20);
          else score += bitCount(head) * 15;
          score += body === required ? 60 : bitCount(body) * 10;
          score += Math.min(bodyCount[g], 36);
          score += bitCount((headMask[g] | bodyMask[g]) & optional) * 8 + bitCount(titleMask[p] & required) * 5;
          if (need < requiredCount) score += bitCount((own | titleMask[p]) & required) * 100;
          hits.push({ page: p, section: g, whole: false, score: score });
        }
      }
      collect(requiredCount);
      if (!hits.length && requiredCount >= 3) {
        partial = true;
        collect(requiredCount - Math.max(1, Math.floor(requiredCount / 3)));
      }

      for (var h = 0; h < hits.length; h++) {
        var hit = hits[h];
        hit.anchor = "";
        hit.where = tokens && bodyMask[hit.section] ? locate(hit.section, mark, order, requiredOrder, required) : null;
        if (!hit.where || hit.whole) continue;
        hit.score += hit.where.kind === "phrase" ? 200 : hit.where.kind === "ordered" ? 120 : hit.where.kind === "near" ? 30 : 0;
        if (hit.where.kind === "phrase" || hit.where.kind === "ordered" || requiredOrder.length === 1) {
          hit.anchor = anchorAt(hit.section, hit.where.from);
          if (hit.anchor && hit.where.kind !== "any" && hit.anchor === anchorAt(hit.section, hit.where.to)) hit.score += 100;
        }
      }
      hits.sort(function (a, b) { return b.score - a.score || a.section - b.section; });

      var groups = [], groupOf = {}, shown = 0;
      for (h = 0; h < hits.length; h++) {
        hit = hits[h];
        var group = groupOf[hit.page];
        if (!group) {
          if (shown >= limit) continue;
          group = groupOf[hit.page] = { page: hit.page, hits: [] };
          groups.push(group);
        }
        if (group.hits.length >= PER_PAGE || shown >= limit + PER_PAGE) continue;
        group.hits.push(hit);
        shown++;
      }
      var rows = [];
      for (var gi = 0; gi < groups.length; gi++) {
        var page = pages[groups[gi].page];
        for (h = 0; h < groups[gi].hits.length; h++) {
          hit = groups[gi].hits[h];
          var number = sectionNumber[hit.section];
          var anchor = hit.whole ? "" : hit.anchor || (number ? page.i[number - 1] : "");
          rows.push({
            first: h === 0, group: page.s === page.t ? page.t : page.s + " \u203a " + page.t,
            label: hit.whole || !number ? page.t : page.h[number - 1],
            url: page.u + (anchor ? "#" + anchor : ""),
            snippet: tokens ? (hit.where ? snippet(hit.section, mark, hit.where.from, hit.where.to) : snippet(hit.section, null, -1, -1)) : []
          });
        }
      }
      return { rows: rows, partial: partial, total: hits.length };
    }

    return { setText: setText, query: query, hasText: function () { return tokens !== null; } };
  }

  var api = { create: create };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  if (typeof window !== "undefined") window.JaiSearch = api;
}());
