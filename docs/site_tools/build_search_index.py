#!/usr/bin/env python3
"""Regenerate assets/search-index.js from the pages themselves.

Every page contributes its title, its section, and every <h2>/<h3> that carries an id, so the
index can never drift from the headings a reader actually sees. Run it after editing any page:

    python site_tools/build_search_index.py [--check]

--check reports whether the index would change and exits 1 if it would, writing nothing.
"""

import argparse
import html
import json
import pathlib
import re
import sys

SITE = pathlib.Path(__file__).resolve().parent.parent / "site"

PAGES = (
    ["index.html", "tour/index.html", "bridges/index.html"]
    + sorted(p.relative_to(SITE).as_posix() for p in (SITE / "bridges").glob("[0-9]*.html"))
    + ["guide/index.html"]
    + sorted(p.relative_to(SITE).as_posix() for p in (SITE / "guide").glob("[0-9]*.html"))
    + ["embedding/index.html"]
    + sorted(p.relative_to(SITE).as_posix() for p in (SITE / "embedding").glob("[0-9]*.html"))
    + ["examples/index.html", "reference/index.html", "reference/api.html", "stability.html"]
)

SECTION = {"bridges": "Bridges", "guide": "Guide", "embedding": "Embedding", "examples": "Examples", "reference": "Reference"}

TITLE_RE = re.compile(r"<title>(.*?)</title>", re.S)
H1_RE = re.compile(r"<h1(?:\s[^>]*)?>(.*?)</h1>", re.S)
HEAD_RE = re.compile(r'<(h[23])\b[^>]*\bid="([^"]*)"[^>]*>(.*?)</\1>', re.S)
TAG_RE = re.compile(r"<[^>]+>")


def text(fragment):
    plain = html.unescape(TAG_RE.sub("", fragment))
    return " ".join(plain.replace("​", "").split())


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
    heads, ids = [], []
    for _, hid, body in HEAD_RE.findall(src):
        heads.append(text(body))
        ids.append(hid)
    return {"t": title, "u": rel, "s": section, "h": heads, "i": ids}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    entries = [page_entry(rel) for rel in PAGES]
    out = "window.SEARCH_INDEX = " + json.dumps(entries, ensure_ascii=False, separators=(",", ":")) + ";\n"
    target = SITE / "assets" / "search-index.js"
    current = target.read_text(encoding="utf-8") if target.exists() else ""
    if args.check:
        print("search index up to date" if current == out else "search index stale")
        return 0 if current == out else 1
    target.write_text(out, encoding="utf-8", newline="\n")
    print("search index: %d pages, %d headings" % (len(entries), sum(len(e["h"]) for e in entries)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
