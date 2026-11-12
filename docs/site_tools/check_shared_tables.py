#!/usr/bin/env python3
"""Check that every copy of a shared table matches its source.

    python site_tools/check_shared_tables.py

A table marked <div class="tablewrap" data-shared="NAME" data-source> is the source; every other div marked
data-shared="NAME" must hold the same table, compared with link targets reduced to their #fragment (a copy links
back to the source page). Exit code 1 on a mismatch, a copy with no source, or a source with no copy.
"""

import pathlib
import re
import sys

SITE = pathlib.Path(__file__).resolve().parent.parent / "site"
SHARED = re.compile(r'<div class="tablewrap" data-shared="([^"]+)"( data-source)?>(.*?)</div>', re.S)


def normalized(a_table):
    result = re.sub(r'href="[^"#]*#', 'href="#', a_table)
    return re.sub(r"\s+", " ", result).strip()


def main():
    sources = {}
    copies = {}
    for page in sorted(SITE.rglob("*.html")):
        text = page.read_text(encoding="utf-8")
        for match in SHARED.finditer(text):
            name, is_source, table = match.group(1), match.group(2), match.group(3)
            where = page.relative_to(SITE).as_posix()
            if is_source:
                if name in sources:
                    print("%s: a second source for shared table '%s' (first in %s)" % (where, name, sources[name][0]))
                    return 1
                sources[name] = (where, normalized(table))
            else:
                copies.setdefault(name, []).append((where, normalized(table)))
    failed = False
    for name, entries in copies.items():
        if name not in sources:
            print("shared table '%s' has copies (%s) and no source" % (name, ", ".join(where for where, _ in entries)))
            failed = True
            continue
        for where, table in entries:
            if table != sources[name][1]:
                print("%s: shared table '%s' differs from its source in %s" % (where, name, sources[name][0]))
                failed = True
    for name, (where, _) in sources.items():
        if name not in copies:
            print("%s: shared table '%s' has no copy" % (where, name))
            failed = True
    print("shared tables: %d checked" % sum(len(entries) for entries in copies.values()))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
