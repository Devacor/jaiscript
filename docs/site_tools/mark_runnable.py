#!/usr/bin/env python3
"""Marks the site's runnable samples, which assets/samples.js gives a Run button: the blocks run_samples.py runs.

    python site_tools/mark_runnable.py [--check]

A sample gets data-run="<file>" (main.jai, or the file its label names); a file samples load (run_samples.py's support
files) gets data-file="<name>", and a page's samples run beside every such file on it. A sample marked data-host, or
labeled as needing its host, stays unmarked. --check reports pages whose marks are stale and writes nothing.
"""

import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from run_samples import HOST_LABEL_RE, SITE, page_blocks  # noqa: E402

OPEN_RE = re.compile(r'<div class="codeblock[^"]*"[^>]*>')
MARK_RE = re.compile(r' data-(?:run|file)="[^"]*"')


def mark(block):
    if block["kind"] not in ("cb-jai", "cb-spider"):
        return ""
    if block["support"]:
        return ' data-file="%s"' % block["name"]
    if block["kind"] != "cb-jai" or "data-host" in block["attrs"] or HOST_LABEL_RE.search(block["label"]):
        return ""
    return ' data-run="%s"' % (block["name"] or "main.jai")


def marked(text):
    blocks = page_blocks(text)
    opens = list(OPEN_RE.finditer(text))
    if len(opens) != len(blocks):
        raise SystemExit("mark_runnable: %d codeblock tags but %d codeblocks parsed" % (len(opens), len(blocks)))
    result, last = [], 0
    for found, block in zip(opens, blocks):
        tag = MARK_RE.sub("", found.group(0))
        result.append(text[last:found.start()])
        result.append(tag[:-1] + mark(block) + ">")
        last = found.end()
    result.append(text[last:])
    return "".join(result)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="report stale pages, write nothing")
    args = ap.parse_args()
    stale, runnable = [], 0
    for page in sorted(SITE.glob("*.html")) + sorted(SITE.glob("*/*.html")):
        with page.open(encoding="utf-8", newline="") as handle:
            text = handle.read()
        out = marked(text)
        runnable += out.count(" data-run=")
        if out != text:
            stale.append(page.relative_to(SITE).as_posix())
            if not args.check:
                with page.open("w", encoding="utf-8", newline="") as handle:
                    handle.write(out)
    if args.check:
        print("\n".join("stale runnable marks: " + p for p in stale) if stale else "runnable marks up to date")
        return 1 if stale else 0
    print("runnable samples: %d; pages marked: %d" % (runnable, len(stale)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
