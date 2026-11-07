#!/usr/bin/env python3
"""Grep the site for internal leakage and stale language facts.

Leakage: machine paths, engine-internal names, working-session vocabulary. Stale facts: the
spellings the static-checking redesign retired (opt-in checking, warn as the default, the old
error texts, C.s for a static field, old .jaibite versions).

    python site_tools/check_leakage.py [--notes]

Every hit is printed as file:line: pattern | context. Exit code 1 when a hard rule matches.
--notes also prints the heuristic rules (words like "session" or "opt-in" that are usually
fine); those never fail the check - read the context before acting.
"""

import pathlib
import re
import sys
from html.parser import HTMLParser

SITE = pathlib.Path(__file__).resolve().parent.parent / "site"

LEAKAGE = [
    (r"[A-Za-z]:\\git\\|[A-Za-z]:/git/", "drive path"),
    (r"\\Users\\|/Users/|AppData", "user path"),
    (r"\bBindstone\b", "Bindstone"),
    (r"\bMutedVision\b|\bMV::", "MutedVision"),
    (r"Source[\\/]JaiScript", "repo-relative source path"),
    (r"out[\\/]build|x64-Release|x64-Debug|Release BENCHMARKS", "build dir"),
    (r"\bDev\b(?!\s*(?:Tools|C\+\+))", "Dev"),
    (r"\bDev's\b", "Dev's"),
    (r"\bsubagents?\b|\bagents? (?:own|edit|wrote|left)", "agent"),
    (r"PERF_PASS|perf[ -]pass (?:report|ledger|row)|\bledger row\b|\brow \d+ of the (?:report|ledger)", "perf pass ledger"),
    (r"\bClaude\b|\bAnthropic\b", "Claude"),
    (r"\bDev ruling\b|\bruling\b|\bruled\b", "ruling"),
    (r"CLAUDE\.md|MEMORY\.md", "instruction file"),
    (r"jaidoom\.zip|wads[\/](doom1|doom2|doom)\.wad", "game data file (never in the repository; JaiDOOM fetches its own)"),
    (r"\bthis (?:overnight|window)\b|\bovernight\b", "session talk"),
]

STALE = [
    (r"skip_strict", "skip_strict"),
    (r"warn is the default|warn mode is the default|default(?:s)? to warn|warn by default|defaults? (?:is|to) warn",
     "warn-as-default"),
    (r"Object has no member", "old error text 'Object has no member'"),
    (r"Type mismatch in assignment", "old error text 'Type mismatch in assignment'"),
    (r"No matching overload", "old error text 'No matching overload'"),
    (r"\bC\.s\b", "C.s spelling (statics are C::s)"),
    (r"static_checking\((?:true|false)\)", "static_checking(bool)"),
    (r"enable_static_check|set_static_checking|static_check_mode|static_checking\(|check_mode::|has_strict_errors|strict_region",
     "old checker API (checking is always strict)"),
    (r"--check[= ](?:strict|warn|off)|--check=", "retired runner flag --check"),
    (r"\bwarn mode\b|\bwarn-mode engine|engine mode <code>off|strict by default|checking off\b",
     "a check mode other than strict"),
    (r"\.jaibite\)? version (?:[0-9]|10)\b|jaibite v(?:[0-9]|10)\b|version (?:[0-9]|10) of the \.jaibite|k_jaibite_version = (?:[0-9]|10)\b",
     "old .jaibite version"),
    (r"check(?:ing|er) is off by default", "checker off by default"),
    (r"engine[^.]{0,40}starts on the interpreter|runs the (?:tree-walking )?interpreter until you|interpreter\. The default|alias for the interpreter",
     "interpreter as the engine default"),
    (r"strict(?:_mode| mode)? (?:is|must be) (?:enabled|turned on|switched on)", "strict must be enabled"),
    (r"(?:strict|checker|checking)[^.]{0,40}\bopt[- ]in\b|\bopt[- ]in\b[^.]{0,40}(?:strict|checker|checking)",
     "opt-in near strict/checker"),
]

NEGATED_OPT_IN = re.compile(r"(?:Nothing (?:here )?is|None of this is|not|never|is the opt-out|isn't|is not) opt[- ]in|opt[- ]in;? var is the opt-out",
                            re.I)

NOTES = [
    (r"\bsession\b", "session"),
    (r"\bopt[- ]in\b", "opt-in"),
    (r"\bcampaign\b", "campaign"),
    (r"\bledger\b", "ledger"),
    (r"\bagents?\b", "agent"),
]


class Text(HTMLParser):
    """Visible text with line numbers, code and prose alike (leakage hides in both)."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.lines = {}
        self.skip = 0

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style"):
            self.skip += 1

    def handle_endtag(self, tag):
        if tag in ("script", "style") and self.skip:
            self.skip -= 1

    def handle_data(self, data):
        if self.skip:
            return
        line = self.getpos()[0]
        for i, chunk in enumerate(data.split("\n")):
            self.lines[line + i] = self.lines.get(line + i, "") + chunk


def scan(path, rules, markup_rules=0):
    t = Text()
    src = path.read_text(encoding="utf-8")
    t.feed(src)
    t.close()
    hits = []
    for line in sorted(t.lines):
        text = t.lines[line]
        for pat, label in rules:
            for m in re.finditer(pat, text):
                a, b = max(0, m.start() - 50), min(len(text), m.end() + 50)
                if "opt-in" in label and NEGATED_OPT_IN.search(text[a:b]):
                    continue
                hits.append((line, label, text[a:b].strip()))
    # attributes carry paths too (href/src/title): scan raw markup for the path-ish rules
    seen = {(h[0], h[1]) for h in hits}
    for i, raw in enumerate(src.split("\n"), 1):
        for pat, label in rules[:markup_rules]:
            for m in re.finditer(pat, raw):
                if (i, label) not in seen:
                    a, b = max(0, m.start() - 50), min(len(raw), m.end() + 50)
                    hits.append((i, label + " (markup)", raw[a:b].strip()))
    return hits


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass
    notes = "--notes" in sys.argv
    pages = sorted(SITE.glob("*.html")) + sorted(SITE.glob("*/*.html"))
    total = 0
    groups = [("LEAKAGE", LEAKAGE, 6), ("STALE FACTS", STALE, 0)]
    if notes:
        groups.append(("NOTES (heuristic, never fail)", NOTES, 0))
    for title, rules, markup in groups:
        print("== %s" % title)
        for p in pages:
            rel = p.relative_to(SITE).as_posix()
            for line, label, ctx in scan(p, rules, markup):
                if rules is not NOTES:
                    total += 1
                print("%s:%d: %s | %s" % (rel, line, label, ctx))
    print("\n%d hard hits" % total)
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
