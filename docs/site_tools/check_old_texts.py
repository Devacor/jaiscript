#!/usr/bin/env python3
"""Grep the site for retired error texts (the current ones: reference/index.html "Run-time errors").

    python site_tools/check_old_texts.py

Every hit is printed as file:line: text | context. Exit code 1 when anything is found.
"""

import html
import pathlib
import re
import sys

SITE = pathlib.Path(__file__).resolve().parent.parent / "site"

OLD_TEXTS = [
    r"Member '[^']*' accessed on null",
    r"String has no method",
    r"Array has no method",
    r"Map has no method or key",
    r"weak_ptr has no method",
    r"Object has no member",
    r"Parent class has no method",
    r"Class '[^']*' has no static member",
    r"Cannot assign to non-existent member",
    r"Cannot call non-function value",
    r"Function expected \d+ arguments but got",
    r"No matching overload",
    r"No method overloads found",
    r"No constructor found with matching arguments",
    r"No matching constructor for ancestor",
    r"Failed to evaluate field initializer",
    r"Failed to evaluate super\(\) argument",
    r"Failed to evaluate constructor initializer argument",
    r"No matching constructor found for this\(\)",
    r"Method argument count mismatch",
    r"No method overload matches",
    r"Type mismatch: expected",
    r"Type mismatch in assignment",
    r"Incompatible class types",
    r"Cannot assign shared_ptr<[^>]*> to shared_ptr<",
    r"Cannot assign weak_ptr to shared_ptr - use weak\.lock\(\)",
    r"Cannot store '[^']*' in '[^']*'",
    r"Cannot assign to static member: field",
    r"resume\(\) takes no arguments",
    r"shadows base class method",
    r"overrides virtual method from base class",
    # `Cannot push 'string' to array<int>` and `Cannot assign 'string' to element of type 'int'`
    # are still the live texts (reference/index.html quotes the push one), so they
    # are deliberately not here.
    r"Cannot copy: the value nests too deep",
    r"Maximum JSON nesting depth",
    r"Maximum serialization depth",
    r"Maximum deserialization depth",
    r"cannot be mutated in place",
    r"allow_unsafe_parallel",
    r"--unsafe-parallel",
    r"[Uu]nsafe parallel mode",
    r"Cannot compare: the values nest deeper",
    # a container, an instance and a function print their contents (guide 09 "Printing values"), never a tag
    r"\[array\]",
    r"\[map\]",
    r"\[object\]",
    r"\[function\]",
]


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass
    pages = sorted(SITE.glob("*.html")) + sorted(SITE.glob("*/*.html"))
    hits = 0
    for p in pages:
        rel = p.relative_to(SITE).as_posix()
        for i, raw in enumerate(p.read_text(encoding="utf-8").split("\n"), 1):
            text = html.unescape(re.sub(r"<[^>]+>", "", raw))
            for pat in OLD_TEXTS:
                for m in re.finditer(pat, text):
                    hits += 1
                    a, b = max(0, m.start() - 60), min(len(text), m.end() + 60)
                    print("%s:%d: %s | %s" % (rel, i, m.group(0), text[a:b].strip()))
    print("%d hits" % hits)
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
