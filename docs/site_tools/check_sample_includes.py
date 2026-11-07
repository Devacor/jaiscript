#!/usr/bin/env python3
"""Compile each page's checked C++ samples with exactly the includes the page shows.

source/tests/integration/doc_samples_tests.cpp runs every C++ block a page marks data-sample="id", but inside one file
whose own includes would hide a header a page forgets to show. This builds one translation unit per page from the page's
samples alone: the #include lines of its marked C++ blocks, then their code in page order as the harness splits it (a region
whose marker sits at namespace scope at file scope, each harness test's statements as one function that has an engine `eng`
when no block makes one: the pages run their statements inside a host's main). The checked pages are the ones whose samples
build a whole program from its first include (COMPLETE_PAGES); the others show fragments of a host they do not show, with the
one header a feature lives in. A region the harness marks "// [context id]" is code the page names but does not show (a
manager it calls); it goes ahead of that sample.

    python docs/site_tools/check_sample_includes.py --cxx <compiler> [--page SUBSTR]

The compiler takes GCC-style options (g++, clang++). Exit code 1 when a page does not compile.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile
from html.parser import HTMLParser

SITE = pathlib.Path(__file__).resolve().parent.parent / "site"
ROOT = SITE.parents[1]
HARNESS = ROOT / "source" / "tests" / "integration" / "doc_samples_tests.cpp"
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]")
MARKER_RE = re.compile(r"^(\s*)// \[sample ([\w-]+)\]")
END_RE = re.compile(r"^\s*// \[/sample\]")
CONTEXT_RE = re.compile(r"^\s*// \[context ([\w-]+)\]")
END_CONTEXT_RE = re.compile(r"^\s*// \[/context\]")
COMPLETE_PAGES = ("embedding/01-embedding-quickstart.html", "embedding/02-dynamic-binder.html")
TEST_RE = re.compile(r'^\s*test\("([\w-]+)"')
ENGINE_RE = re.compile(r"\b(?:auto|std::shared_ptr<jai::engine>)\s+eng\s*=")


class Blocks(HTMLParser):
    """The ids of the C++ blocks marked data-sample, in page order."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.ids = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "div" and "cb-cpp" in (a.get("class") or "").split() and a.get("data-sample"):
            self.ids.append(a["data-sample"])


def harness_regions(harness_text):
    """Each sample id's regions in the harness, in file order: (session, text), session None for a region at namespace scope
    (its marker unindented) and else the harness test the statements run in."""
    result = {}
    current = None
    session = None
    for line in harness_text.splitlines():
        marker = MARKER_RE.match(line)
        context = CONTEXT_RE.match(line)
        test = TEST_RE.match(line)
        if test:
            session = test.group(1)
        if marker:
            current = (marker.group(2), session if marker.group(1) else None, [])
        elif context:
            current = (context.group(1), None, [])
        elif current and (END_RE.match(line) or END_CONTEXT_RE.match(line)):
            result.setdefault(current[0], []).append((current[1], "\n".join(current[2])))
            current = None
        elif current:
            current[2].append(line)
    return result


def page_unit(ids, regions):
    """The page's translation unit: its includes, its namespace-scope code, then one function per harness session."""
    includes, scope, sessions = [], [], {}
    for sample_id in ids:
        for session, text in regions.get(sample_id, []):
            lines = text.splitlines()
            includes += [line.strip() for line in lines if INCLUDE_RE.match(line) and line.strip() not in includes]
            rest = "// " + sample_id + "\n" + "\n".join(line for line in lines if not INCLUDE_RE.match(line))
            if session is None:
                scope.append(rest)
            else:
                sessions.setdefault(session, []).append(rest)
    unit = "\n".join(includes) + "\n\n" + "\n\n".join(scope) + "\n"
    for number, body in enumerate(sessions.values()):
        statements = "\n\n".join(body)
        prelude = "" if ENGINE_RE.search(statements) else "auto eng = jai::engine::make();\n"
        unit += "\nvoid session_" + str(number) + "() {\n" + prelude + statements + "\n}\n"
    return unit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", required=True)
    ap.add_argument("--page", default="")
    args = ap.parse_args()
    regions = harness_regions(HARNESS.read_text(encoding="utf-8"))
    failed = 0
    with tempfile.TemporaryDirectory() as work:
        for page in sorted(SITE.rglob("*.html")):
            rel = page.relative_to(SITE).as_posix()
            if rel not in COMPLETE_PAGES or args.page not in rel:
                continue
            parser = Blocks()
            parser.feed(page.read_text(encoding="utf-8"))
            unit = page_unit(parser.ids, regions)
            source = pathlib.Path(work) / (rel.replace("/", "_") + ".cpp")
            source.write_text(unit, encoding="utf-8")
            run = subprocess.run([args.cxx, "-std=c++20", "-fsyntax-only", "-I" + str(ROOT / "include"), str(source)],
                                 capture_output=True, text=True, encoding="utf-8", errors="replace")
            if run.returncode != 0:
                failed += 1
                print("FAIL " + rel + ": its C++ samples do not compile with the includes it shows")
                for line in [l for l in run.stderr.splitlines() if "error" in l][:6]:
                    print("  " + line[:240])
            else:
                print("ok " + rel + " (" + str(len(parser.ids)) + " C++ samples)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
