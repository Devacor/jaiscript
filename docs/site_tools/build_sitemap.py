#!/usr/bin/env python3
"""Regenerate sitemap.xml and robots.txt at the site root from the page list apply_chrome.py keeps.

    python site_tools/build_sitemap.py [--check]

Every page but the not-found page is listed under the address its <link rel="canonical"> names. --check reports
whether either file would change and exits 1 if one would, writing nothing.
"""

import argparse
import sys

import apply_chrome


def build():
    urls = [apply_chrome.canonical(page[0]) for page in apply_chrome.build_pages() if page[0] != apply_chrome.NOT_FOUND]
    sitemap = "\n".join(
        ['<?xml version="1.0" encoding="UTF-8"?>', '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">']
        + ["<url><loc>%s</loc></url>" % url for url in urls]
        + ["</urlset>", ""])
    robots = "User-agent: *\nAllow: /\n\nSitemap: %ssitemap.xml\n" % apply_chrome.SITE_URL
    return {"sitemap.xml": sitemap, "robots.txt": robots}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    stale = []
    for name, out in build().items():
        target = apply_chrome.SITE / name
        current = target.read_text(encoding="utf-8") if target.exists() else ""
        if current == out:
            continue
        stale.append(name)
        if not args.check:
            target.write_text(out, encoding="utf-8", newline="\n")
    if args.check:
        print("sitemap and robots.txt up to date" if not stale else "stale: " + ", ".join(stale))
        return 1 if stale else 0
    print("sitemap: %d pages" % build()["sitemap.xml"].count("<loc>"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
