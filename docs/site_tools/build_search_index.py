#!/usr/bin/env python3
"""Regenerate the search indexes from the pages themselves.

    python site_tools/build_search_index.py [--check]

assets/search-index.js (every page loads it): each page's title, section, and every <h2>/<h3> that carries an id.
assets/search-text.js (loaded when the search box is first focused): the text under each of those headings (prose,
table cells, code and output), and the ids inside a section (an error catalogue row, a definition) with the stretch
of text each covers, so a hit links to the nearest one.

The text is dictionary coded. A section's text splits into words ([A-Za-z0-9_]+) and the separators between them; a
lone space between two words is implied, never stored. Every distinct token gets a number, most frequent first, and
a section is the string of its tokens' code points (code_point below; assets/search.js holds the inverse). No code
point is an ASCII letter, digit or underscore, so no run of codes can read as a word.

--check reports whether either file would change and exits 1 if one would, writing nothing.
"""

import argparse
import html
import json
import pathlib
import re
import sys
from html.parser import HTMLParser

SITE = pathlib.Path(__file__).resolve().parent.parent / "site"

PAGES = (
    ["index.html", "tour/index.html", "bridges/index.html"]
    + sorted(p.relative_to(SITE).as_posix() for p in (SITE / "bridges").glob("[0-9]*.html"))
    + ["guide/index.html"]
    + sorted(p.relative_to(SITE).as_posix() for p in (SITE / "guide").glob("[0-9]*.html"))
    + ["embedding/index.html"]
    + sorted(p.relative_to(SITE).as_posix() for p in (SITE / "embedding").glob("[0-9]*.html"))
    + ["examples/index.html", "play/index.html", "reference/index.html", "reference/rules.html", "reference/errors.html", "reference/library.html",
       "reference/host.html", "reference/api.html", "stability.html"]
)

SECTION = {"tour": "Tour", "bridges": "Bridges", "guide": "Guide", "embedding": "Embedding", "examples": "Examples",
           "play": "Play", "reference": "Reference"}

TITLE_RE = re.compile(r"<title>(.*?)</title>", re.S)
H1_RE = re.compile(r"<h1(?:\s[^>]*)?>(.*?)</h1>", re.S)
TAG_RE = re.compile(r"<[^>]+>")
TOKEN_RE = re.compile(r"[A-Za-z0-9_]+|[^A-Za-z0-9_]+")
ZERO_WIDTH = chr(0x200B)
WORD_START = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}
SKIPPED_TAGS = {"script", "style", "nav", "svg", "h1"}
SKIPPED_CLASSES = {"hlink", "cb-label"}
BLOCKS = {"p", "div", "li", "tr", "pre", "h2", "h3", "h4", "h5", "h6", "dt", "dd", "br", "table", "ul", "ol", "section",
          "summary", "details", "figcaption", "blockquote", "article", "hr"}
CELLS = {"td", "th"}
MIDDLE_DOT = chr(0xB7)
CELL_BREAK = " " + MIDDLE_DOT + " "

ASCII_CODES = [c for c in range(0x20, 0x7F) if chr(c) not in WORD_START and chr(c) not in '"\\']
TWO_BYTE_FIRST, TWO_BYTE_END = 0xA1, 0x800
THREE_BYTE_END = 0xD800
THREE_BYTE_HOLES = (0x2028, 0x2029)


def code_point(number):
    """One UTF-16 unit per token: 1 byte of UTF-8 for the commonest, then 2, then 3."""
    if number < len(ASCII_CODES):
        return ASCII_CODES[number]
    number -= len(ASCII_CODES)
    if number < TWO_BYTE_END - TWO_BYTE_FIRST:
        return TWO_BYTE_FIRST + number
    result = TWO_BYTE_END + number - (TWO_BYTE_END - TWO_BYTE_FIRST)
    for hole in THREE_BYTE_HOLES:
        if result >= hole:
            result += 1
    if result >= THREE_BYTE_END:
        raise SystemExit("search index: more distinct tokens than one UTF-16 unit below the surrogates can number")
    return result


def text(fragment):
    plain = html.unescape(TAG_RE.sub("", fragment))
    return " ".join(plain.replace(ZERO_WIDTH, "").split())


class Sections(HTMLParser):
    """The article split at its id-carrying <h2>/<h3>: sections[0] is what precedes the first of them."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.in_main = False
        self.skip_depth = 0
        self.stack = []          # (tag, skipped, anchor) for every open non-void element inside <main>
        self.heading = None      # [id, pieces] while inside an id-carrying h2/h3
        self.sections = [{"id": "", "heading": "", "pieces": [], "length": 0, "anchors": []}]

    def emit(self, piece):
        if self.skip_depth:
            return
        if self.heading is not None:
            self.heading[1].append(piece)
            return
        section = self.sections[-1]
        section["pieces"].append(piece)
        section["length"] += len(piece)

    def handle_starttag(self, tag, attrs):
        if tag == "main":
            self.in_main = True
            return
        if not self.in_main:
            return
        d = dict(attrs)
        if tag in BLOCKS:
            self.emit(" ")
        if tag in VOID:
            return
        classes = set((d.get("class") or "").split())
        skipped = tag in SKIPPED_TAGS or bool(classes & SKIPPED_CLASSES)
        anchor = None
        if tag in ("h2", "h3") and d.get("id") and not self.skip_depth and self.heading is None:
            self.heading = [d["id"], []]
        elif d.get("id") and not self.skip_depth and not skipped and self.heading is None:
            section = self.sections[-1]
            anchor = [section, section["length"], None, d["id"]]
            section["anchors"].append(anchor)
        if skipped:
            self.skip_depth += 1
        self.stack.append((tag, skipped, anchor))

    def handle_startendtag(self, tag, attrs):
        if self.in_main and tag in BLOCKS:
            self.emit(" ")

    def handle_endtag(self, tag):
        if tag == "main":
            self.in_main = False
            return
        if not self.in_main or tag in VOID:
            return
        while self.stack:
            open_tag, skipped, anchor = self.stack.pop()
            if skipped:
                self.skip_depth -= 1
            if anchor is not None:
                # an element a new heading cut short covers its own section only
                anchor[2] = anchor[0]["length"]
            if open_tag == tag:
                break
        if tag in ("h2", "h3") and self.heading is not None:
            hid, pieces = self.heading
            self.heading = None
            self.sections.append({"id": hid, "heading": " ".join("".join(pieces).replace(ZERO_WIDTH, "").split()),
                                  "pieces": [], "length": 0, "anchors": []})
            return
        if tag in CELLS:
            self.emit(CELL_BREAK)
        elif tag in BLOCKS:
            self.emit(" ")

    def handle_data(self, data):
        if self.in_main:
            self.emit(data)


def tokens_of(section):
    """(tokens, anchors as [first token, end token, id]) for one section."""
    raw = "".join(section["pieces"]).replace(ZERO_WIDTH, " ")
    tokens, starts = [], []
    for found in TOKEN_RE.finditer(raw):
        piece = found.group(0)
        if piece[0] not in WORD_START:
            piece = " ".join(piece.split()) if piece.strip() else " "
            if piece != " ":
                edge_left = " " if found.group(0)[0].isspace() else ""
                edge_right = " " if found.group(0)[-1].isspace() else ""
                piece = edge_left + piece + edge_right
            while CELL_BREAK + CELL_BREAK.strip() in piece:
                piece = piece.replace(CELL_BREAK + CELL_BREAK.strip(), CELL_BREAK.rstrip())
            if piece == " ":
                continue
        tokens.append(piece)
        starts.append(found.start())
    while tokens and not tokens[-1].strip(" " + MIDDLE_DOT):
        tokens.pop()
        starts.pop()
    if tokens and tokens[0][0] not in WORD_START:
        tokens[0] = tokens[0].lstrip()
        if not tokens[0].strip(" " + MIDDLE_DOT):
            tokens.pop(0)
            starts.pop(0)

    def token_at(offset):
        low, high = 0, len(starts)
        while low < high:
            mid = (low + high) // 2
            if starts[mid] < offset:
                low = mid + 1
            else:
                high = mid
        return low

    anchors = []
    for _, begin, end, hid in section["anchors"]:
        first, last = token_at(begin), token_at(section["length"] if end is None else end)
        if last > first:
            anchors.append([first, last, hid])
    return tokens, anchors


def page_entry(rel):
    src = (SITE / rel).read_text(encoding="utf-8")
    title = text(TITLE_RE.search(src).group(1))
    for sep in (" — ", " - "):
        if sep in title:
            title = title.split(sep)[0]
    if rel.endswith("index.html") and rel != "index.html":
        h1 = H1_RE.search(src)
        if h1:
            title = text(h1.group(1))
    section = SECTION.get(rel.split("/")[0], "Home")
    parser = Sections()
    parser.feed(src)
    parser.close()
    entry = {"t": title, "u": rel, "s": section,
             "h": [s["heading"] for s in parser.sections[1:]], "i": [s["id"] for s in parser.sections[1:]]}
    return entry, [tokens_of(s) for s in parser.sections]


def build():
    entries, bodies = [], []
    for rel in PAGES:
        entry, sections = page_entry(rel)
        entries.append(entry)
        bodies.append(sections)

    counts = {}
    for sections in bodies:
        for tokens, _ in sections:
            for token in tokens:
                counts[token] = counts.get(token, 0) + 1
    vocabulary = sorted(counts, key=lambda token: (-counts[token], token))
    code = {token: chr(code_point(n)) for n, token in enumerate(vocabulary)}

    pages, anchors = [], []
    for sections in bodies:
        pages.append(["".join(code[token] for token in tokens) for tokens, _ in sections])
        flat = []
        for number, (_, found) in enumerate(sections):
            for first, last, hid in found:
                flat += [number, first, last, hid]
        anchors.append(flat)

    def dump(value):
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"))

    index = "window.SEARCH_INDEX = " + dump(entries) + ";\n"
    body = "window.SEARCH_TEXT = " + dump({"v": vocabulary, "p": pages, "a": anchors}) + ";\n"
    # the public export's leak scan reads U+FFFD anywhere as mojibake; a page that shows the character has it in a token
    body = body.replace(chr(0xFFFD), "\\ufffd")
    stats = "search index: %d pages, %d headings, %d tokens of %d distinct" % (
        len(entries), sum(len(e["h"]) for e in entries), sum(counts.values()), len(vocabulary))
    return index, body, stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    index, body, stats = build()
    stale = []
    for name, out in (("search-index.js", index), ("search-text.js", body)):
        target = SITE / "assets" / name
        current = target.read_text(encoding="utf-8") if target.exists() else ""
        if current == out:
            continue
        stale.append(name)
        if not args.check:
            target.write_text(out, encoding="utf-8", newline="\n")
    if args.check:
        print("search index up to date" if not stale else "search index stale: " + ", ".join(stale))
        return 1 if stale else 0
    print(stats)
    return 0


if __name__ == "__main__":
    sys.exit(main())
