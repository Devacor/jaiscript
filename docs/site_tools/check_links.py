#!/usr/bin/env python3
"""Check every href/src on the site.

Relative links must point at a file that exists; a #fragment must name an id in the
target page; external URLs are listed, never fetched.

    python site_tools/check_links.py

Exit code 1 when anything is broken.
"""

import html
import pathlib
import re
import sys
from html.parser import HTMLParser
from urllib.parse import unquote, urlsplit

SITE = pathlib.Path(__file__).resolve().parent.parent / "site"


class Collector(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = set()
        self.links = []  # (line, attr, value)

    def handle_starttag(self, tag, attrs):
        d = dict(attrs)
        if "id" in d and d["id"] is not None:
            self.ids.add(d["id"])
        if tag == "a" and d.get("name"):
            self.ids.add(d["name"])
        for key in ("href", "src"):
            v = d.get(key)
            if v is not None:
                self.links.append((self.getpos()[0], tag, key, v))


def scan(path):
    c = Collector()
    c.feed(path.read_text(encoding="utf-8"))
    return c


def main():
    pages = sorted(SITE.glob("*.html")) + sorted(SITE.glob("*/*.html"))
    scanned = {p: scan(p) for p in pages}
    id_cache = {p: c.ids for p, c in scanned.items()}
    external = {}
    failures = []
    for page, c in scanned.items():
        rel = page.relative_to(SITE).as_posix()
        for line, tag, key, raw in c.links:
            v = html.unescape(raw)
            parts = urlsplit(v)
            if parts.scheme in ("http", "https", "mailto", "vscode"):
                external.setdefault(v, []).append("%s:%d" % (rel, line))
                continue
            if parts.scheme:
                failures.append("%s:%d: %s %s=%r unknown scheme" % (rel, line, tag, key, v))
                continue
            target = page
            if parts.path:
                target = (page.parent / unquote(parts.path)).resolve()
                if not target.exists():
                    failures.append("%s:%d: <%s %s=%r> target does not exist" % (rel, line, tag, key, v))
                    continue
                try:
                    target.relative_to(SITE)
                except ValueError:
                    failures.append("%s:%d: <%s %s=%r> leaves the site root" % (rel, line, tag, key, v))
                    continue
            if parts.fragment:
                if target.suffix != ".html":
                    failures.append("%s:%d: fragment on a non-html target %r" % (rel, line, v))
                    continue
                if target not in id_cache:
                    id_cache[target] = scan(target).ids
                if parts.fragment not in id_cache[target]:
                    failures.append("%s:%d: <%s %s=%r> anchor #%s not found in %s"
                                    % (rel, line, tag, key, v, parts.fragment,
                                       target.relative_to(SITE).as_posix()))
    print("pages: %d, links checked: %d, external URLs: %d"
          % (len(pages), sum(len(c.links) for c in scanned.values()), len(external)))
    for url in sorted(external):
        print("  external: %s  (%d use%s, first %s)" % (url, len(external[url]),
                                                        "" if len(external[url]) == 1 else "s",
                                                        external[url][0]))
    if failures:
        print("\nBROKEN (%d):" % len(failures))
        for f in failures:
            print("  " + f)
        return 1
    print("all internal links and anchors resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
