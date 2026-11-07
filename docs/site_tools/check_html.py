#!/usr/bin/env python3
"""Structural sanity for every docs page.

Balanced structural tags, exactly one <h1> and one <main>, the shared chrome present,
no leftover template placeholders, no duplicate ids, every heading id unique.

    python site_tools/check_html.py

Exit code 1 when anything is off.
"""

import pathlib
import re
import sys
from html.parser import HTMLParser

SITE = pathlib.Path(__file__).resolve().parent.parent / "site"

STRUCTURAL = {"html", "head", "body", "header", "main", "aside", "article", "section", "nav",
              "footer", "div", "table", "thead", "tbody", "tr", "td", "th", "ul", "ol", "li",
              "pre", "code", "p", "h1", "h2", "h3", "h4", "a", "span", "strong", "em",
              "details", "summary", "blockquote", "dl", "dt", "dd", "button", "script", "style",
              "title", "label", "small", "sup", "sub", "kbd", "figure", "figcaption"}
VOID = {"meta", "link", "br", "hr", "img", "input", "source", "wbr", "col"}

CHROME = [
    ('<a class="skip" href="#main">', "skip link"),
    ('<header class="site-header">', "site header"),
    ('<nav class="site-nav" aria-label="Sections">', "section nav"),
    ('id="search-box"', "search box"),
    ('id="search-results"', "search results"),
    ('id="theme-toggle"', "theme toggle"),
    ('<div class="layout">', "layout wrapper"),
    ('<main class="content" id="main" tabindex="-1">', "main"),
    ('<footer class="site-footer">', "site footer"),
    ("assets/search-index.js", "search index script"),
    ("assets/site.js", "site script"),
    ("assets/style.css", "stylesheet"),
    ("assets/favicon.svg", "favicon"),
    ('<meta name="viewport"', "viewport meta"),
    ('<meta name="description"', "description meta"),
    ('<meta property="og:title"', "og:title"),
    ('data-theme', "theme bootstrap"),
    ('<html lang="en">', "html lang"),
    ("<!DOCTYPE html>", "doctype"),
]

PLACEHOLDER_RE = re.compile(
    r"\{\{[^}]*\}\}|\{%[^%]*%\}|\bTODO\b|\bTBD\b|\bFIXME\b|\bXXX\b|Lorem ipsum|\bPLACEHOLDER\b|\[\[TK\]\]|\bTK\b|<<<|>>>")


class Checker(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=False)
        self.stack = []
        self.errors = []
        self.counts = {}
        self.ids = {}
        self.in_code = 0
        self.code_text = []
        self.prose_text = []
        self.title = None
        self._in_title = False
        self.heading_ids = []
        self.headings = []
        self.in_article = 0

    def handle_starttag(self, tag, attrs):
        d = dict(attrs)
        if tag in VOID:
            return
        self.counts[tag] = self.counts.get(tag, 0) + 1
        if tag in STRUCTURAL:
            self.stack.append((tag, self.getpos()[0]))
        if tag in ("pre", "code"):
            self.in_code += 1
        if tag == "title":
            self._in_title = True
        hid = d.get("id")
        if hid is not None:
            if hid in self.ids:
                self.errors.append("line %d: duplicate id %r (first at line %d)"
                                   % (self.getpos()[0], hid, self.ids[hid]))
            else:
                self.ids[hid] = self.getpos()[0]
        if tag == "article":
            self.in_article += 1
        if tag in ("h1", "h2", "h3", "h4"):
            self.headings.append((tag, hid, self.getpos()[0]))
            if tag in ("h2", "h3") and not hid and self.in_article:
                self.errors.append("line %d: <%s> without an id (search index / toc cannot reach it)"
                                   % (self.getpos()[0], tag))

    def handle_startendtag(self, tag, attrs):
        if tag not in VOID:
            self.errors.append("line %d: self-closing <%s/> on a non-void element" % (self.getpos()[0], tag))
        self.handle_starttag(tag, attrs)

    def handle_endtag(self, tag):
        if tag in VOID:
            self.errors.append("line %d: stray </%s> on a void element" % (self.getpos()[0], tag))
            return
        if tag in ("pre", "code"):
            self.in_code = max(0, self.in_code - 1)
        if tag == "title":
            self._in_title = False
        if tag == "article":
            self.in_article = max(0, self.in_article - 1)
        if tag not in STRUCTURAL:
            return
        if not self.stack:
            self.errors.append("line %d: </%s> with nothing open" % (self.getpos()[0], tag))
            return
        open_tag, open_line = self.stack[-1]
        if open_tag == tag:
            self.stack.pop()
            return
        # mismatch: see whether tag is open further up (implies an unclosed element)
        names = [t for t, _ in self.stack]
        if tag in names:
            idx = len(names) - 1 - names[::-1].index(tag)
            for t, l in self.stack[idx + 1:]:
                self.errors.append("line %d: <%s> opened at line %d never closed (closed by </%s>)"
                                   % (self.getpos()[0], t, l, tag))
            del self.stack[idx:]
        else:
            self.errors.append("line %d: </%s> does not match any open tag (innermost <%s> from line %d)"
                               % (self.getpos()[0], tag, open_tag, open_line))

    def handle_data(self, data):
        if self._in_title:
            self.title = (self.title or "") + data
        (self.code_text if self.in_code else self.prose_text).append((self.getpos()[0], data))

    def handle_entityref(self, name):
        pass

    def handle_charref(self, name):
        pass


def check(path):
    src = path.read_text(encoding="utf-8")
    c = Checker()
    c.feed(src)
    c.close()
    errs = list(c.errors)
    for t, l in c.stack:
        errs.append("EOF: <%s> opened at line %d never closed" % (t, l))
    for tag, want in (("h1", 1), ("main", 1), ("html", 1), ("head", 1), ("body", 1), ("title", 1)):
        n = c.counts.get(tag, 0)
        if n != want:
            errs.append("expected %d <%s>, found %d" % (want, tag, n))
    for marker, label in CHROME:
        if marker not in src:
            errs.append("chrome missing: %s (%r)" % (label, marker))
    if not c.title or not c.title.strip():
        errs.append("empty <title>")
    for line, data in c.prose_text:
        for m in PLACEHOLDER_RE.finditer(data):
            errs.append("line %d: placeholder-looking text %r" % (line, m.group(0)))
    # h1 must come before any h2
    seen_h1 = False
    for tag, hid, line in c.headings:
        if tag == "h1":
            seen_h1 = True
        elif not seen_h1:
            errs.append("line %d: <%s> before the page's <h1>" % (line, tag))
            break
    # unescaped raw '<' inside code is a parse hazard: flag literal '< ' in code data
    body = src[src.index("<main"):src.index("</main>")] if "<main" in src and "</main>" in src else ""
    for m in re.finditer(r"&(?!(?:[a-zA-Z][a-zA-Z0-9]*|#[0-9]+|#x[0-9a-fA-F]+);)", body):
        line = src.count("\n", 0, src.index("<main") + m.start()) + 1
        errs.append("line %d: bare '&' not followed by an entity" % line)
    return errs


def main():
    pages = sorted(SITE.glob("*.html")) + sorted(SITE.glob("*/*.html"))
    bad = 0
    for p in pages:
        errs = check(p)
        rel = p.relative_to(SITE).as_posix()
        if errs:
            bad += 1
            print("%s:" % rel)
            for e in errs:
                print("  " + e)
    print("%d pages checked, %d with problems" % (len(pages), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
