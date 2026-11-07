#!/usr/bin/env python3
"""Extract every jaiscript sample on the site and run it on both backends under strict.

Each <div class="codeblock cb-jai"> is a sample. Two conventions turn a block into a support
file instead (written next to the samples of its page, never run on its own); a template file (a
<div class="codeblock cb-spider"> labeled "template &mdash; name.spider" that a sample renders) is always one:

  * its introducing paragraph reads "with <code>name.jai</code>:" / "and <code>name.jai</code>:"
  * its label reads "jaiscript &mdash; name.jai" AND another block on the page imports or
    includes that name

A labeled block nobody imports runs on its own under its label's file name (so the listing
matches the page). A block that carries data-host is skipped (it needs the embedding host);
data-budget="N" runs with --budget=N. A label that says the block needs the host ("with the
host above", "needs the host", "sketch", "under the ... cap") is reported as host-dependent
but unmarked.

    python site_tools/run_samples.py [--runner PATH] [--out DIR] [--keep] [--page SUBSTR]

Reports, per sample: load or runtime failures the page's own output block does not show,
backend divergence (vm and interp print different text), and expected-output drift (a line of
the page's output block the runner did not print). Exit code 1 on failures, divergence or drift;
--allow-drift reports drift without failing, --quiet-expected skips the comparison.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
from html.parser import HTMLParser

SITE = pathlib.Path(__file__).resolve().parent.parent / "site"
REPO = SITE.parents[1]
RUNNER_DIRS = [REPO / "build" / "bin", REPO / "out" / "build" / "x64-Release" / "bin",
               REPO / "out" / "build" / "x64-Release BENCHMARKS" / "bin"]
DEFAULT_RUNNER = next((d / "jaiscript" for d in RUNNER_DIRS if any((d / n).exists() for n in ("jaiscript", "jaiscript.exe"))),
                      RUNNER_DIRS[0] / "jaiscript")

INTRO_SUPPORT_RE = re.compile(r"^\s*(?:with|and|plus|then)\s+`([A-Za-z0-9_./-]+\.(?:jai|spider))`\s*:?\s*$", re.I)
INTRO_SUPPORT_RE2 = re.compile(r"`([A-Za-z0-9_./-]+\.(?:jai|spider))`\s*:\s*$")
LABEL_FILE_RE = re.compile(r"(?:jaiscript|template)\s*[—–·-]+\s*([A-Za-z0-9_./-]+\.(?:jai|spider))\s*$")
HOST_LABEL_RE = re.compile(r"host|sketch|cap above|registration|needs the", re.I)
REF_RE = re.compile(r'\b(?:import|include|template|scoped_template)\s*\(?\s*"([^"]+)"')


class Extractor(HTMLParser):
    """Collects codeblocks in document order with their label and the paragraph before each."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.blocks = []
        self._div = None
        self._depth = 0
        self._in_code = False
        self._in_label = False
        self._code = []
        self._p = None
        self._last_p = ""
        self._p_line = 0

    def handle_starttag(self, tag, attrs):
        d = dict(attrs)
        if tag == "div" and self._div is not None:
            self._depth += 1
        if tag == "div" and "codeblock" in (d.get("class") or ""):
            kind = [c for c in d["class"].split() if c.startswith("cb-")]
            self._div = {"kind": kind[0] if kind else "cb-?", "line": self.getpos()[0],
                         "attrs": d, "code": "", "label": "", "intro": self._last_p}
            self._depth = 0
        if self._div is not None and tag == "span" and "cb-label" in (d.get("class") or ""):
            self._in_label = True
        if tag == "code" and self._div is not None:
            self._in_code = True
            self._code = []
        if tag == "p":
            self._p = []
            self._p_line = self.getpos()[0]
        if tag == "code" and self._p is not None and self._div is None:
            self._p.append("`")

    def handle_endtag(self, tag):
        if tag == "span" and self._in_label:
            self._in_label = False
        if tag == "code" and self._in_code:
            self._in_code = False
            self._div["code"] = "".join(self._code)
        elif tag == "code" and self._p is not None and self._div is None:
            self._p.append("`")
        if tag == "div" and self._div is not None:
            if self._depth == 0:
                self.blocks.append(self._div)
                self._div = None
            else:
                self._depth -= 1
        if tag == "p" and self._p is not None:
            self._last_p = "".join(self._p)
            self._p = None
        if tag in ("h1", "h2", "h3", "h4", "ul", "ol", "table"):
            self._last_p = ""

    def handle_data(self, data):
        if self._in_label and self._div is not None:
            self._div["label"] += data
        elif self._in_code:
            self._code.append(data)
        elif self._p is not None:
            self._p.append(data)


def plan_page(page):
    """Return (samples, pool, notes). samples: dicts to run; pool: name -> support code."""
    ex = Extractor()
    ex.feed(page.read_text(encoding="utf-8"))
    ex.close()
    blocks = ex.blocks
    jai = [b for b in blocks if b["kind"] in ("cb-jai", "cb-spider")]   # a cb-spider block is a template file, never a sample
    referenced = set()
    for b in jai:
        referenced.update(REF_RE.findall(b["code"]))

    for b in jai:
        b["name"] = None
        b["support"] = False
        m = INTRO_SUPPORT_RE.match(b["intro"]) or INTRO_SUPPORT_RE2.search(b["intro"])
        if m:
            b["name"], b["support"] = m.group(1), True
            continue
        m = LABEL_FILE_RE.search(b["label"])
        if m:
            b["name"] = m.group(1)
            b["support"] = m.group(1) in referenced

    pool, notes = {}, []
    for b in jai:
        if b["support"]:
            if b["name"] in pool and pool[b["name"]] != b["code"]:
                notes.append("line %d: support file %r defined twice with different text; later wins"
                             % (b["line"], b["name"]))
            pool[b["name"]] = b["code"]

    samples = []
    for i, b in enumerate(blocks):
        if b["kind"] != "cb-jai" or b["support"]:
            continue
        expected, expected_warnings = None, False
        for nb in blocks[i + 1:]:
            if nb["kind"] == "cb-jai" and nb["support"]:
                continue
            if nb["kind"] in ("cb-output", "cb-plain"):
                expected = nb["code"]
                expected_warnings = "--warnings" in nb["label"]
            break
        samples.append({"line": b["line"], "code": b["code"], "attrs": b["attrs"], "label": b["label"],
                        "file": b["name"] or "main.jai", "expected": expected,
                        "expected_warnings": expected_warnings, "index": len(samples) + 1})
    return samples, pool, notes


def norm(text, sample_dir):
    text = text.replace("\r\n", "\n")
    for form in (str(sample_dir), str(sample_dir).replace("\\", "/")):
        text = text.replace(form, "<sample>")
    return text.strip()


def run(runner, backend, main_path, budget, timeout, warnings=False):
    cmd = [str(runner), "--no-pause", "--backend=" + backend] + (["--warnings"] if warnings else [])
    if budget:
        cmd.append("--budget=" + str(budget))
    cmd.append(main_path.name)
    try:
        p = subprocess.run(cmd, cwd=str(main_path.parent), capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "", "<<timeout after %ss>>" % timeout
    dec = lambda b: b.decode("utf-8", errors="replace")
    return p.returncode, dec(p.stdout), dec(p.stderr)


def looks_like_load_failure(stderr):
    return ("static check failed" in stderr) or ("parse failed" in stderr)


def expected_shows_error(expected):
    if not expected:
        return False
    e = expected.lower()
    return ("error" in e) or ("static check failed" in e) or ("jaiscript:" in e) or ("caught" in e) \
        or ("at <script>" in e) or e.startswith("  at ")


LISTING_PREFIX_RE = re.compile(r"^(?:jaiscript: )?(?:[\w./<>\\-]+\.(?:jai|spider)|script|<script>|<sample>[\\/][\w.]+):(\d+:\d+)")
FRAME_RE = re.compile(r"\((?:[^()]*\.jai|<script>):(\d+)\)")


def canon(line):
    """Fold the file-name spellings that differ between the page and the runner."""
    l = line.strip()
    l = re.sub(r"^jaiscript: ", "", l)
    l = LISTING_PREFIX_RE.sub(r"FILE:\1", l)
    l = FRAME_RE.sub(r"(FILE:\1)", l)
    return l


def compare_expected(expected, stdout, stderr):
    """Every non-empty expected line (minus '== result'/'== error' harness lines and caret
    lines) must appear, in order, in the real output, after folding file-name spellings."""
    if expected is None:
        return None
    combined = (stdout + "\n" + stderr).replace("\r\n", "\n")
    got_lines = [canon(l) for l in combined.split("\n")]
    missing = []
    pos = 0
    for line in expected.replace("\r\n", "\n").split("\n"):
        l = line.strip()
        if not l or l.startswith("==") or set(l) <= {"^", " "}:
            continue
        l = canon(l)
        found = False
        for j in range(pos, len(got_lines)):
            if got_lines[j] == l:
                pos = j + 1
                found = True
                break
        if not found and l not in got_lines:   # stdout/stderr interleave differently per terminal
            missing.append(l)
    return missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", default=str(DEFAULT_RUNNER))
    ap.add_argument("--out", default=None, help="scratch dir for extracted samples")
    ap.add_argument("--keep", action="store_true", help="keep the extracted files")
    ap.add_argument("--page", default=None, help="only pages whose path contains this")
    ap.add_argument("--timeout", type=float, default=600.0, help="seconds a sample may run before it counts as hung (a hang rail, never a speed bound)")
    ap.add_argument("--quiet-expected", action="store_true", help="skip the expected-output comparison")
    ap.add_argument("--allow-drift", action="store_true", help="report expected-output drift without failing")
    ap.add_argument("--show-errors", action="store_true",
                    help="list the samples whose page shows an error, with the runner's text beside it")
    args = ap.parse_args()

    runner = pathlib.Path(args.runner)
    if not runner.exists() and runner.with_name(runner.name + ".exe").exists():
        runner = runner.with_name(runner.name + ".exe")
    if not runner.exists():
        print("runner not found: %s" % runner)
        return 2
    out = (pathlib.Path(args.out) if args.out else pathlib.Path(__file__).resolve().parent / "_samples").resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    pages = sorted(SITE.glob("*.html")) + sorted(SITE.glob("*/*.html"))
    if args.page:
        pages = [p for p in pages if args.page in p.relative_to(SITE).as_posix()]

    total = skipped = 0
    load_fail, diverge, drift, error_ok, host_unmarked, notes_all = [], [], [], [], [], []
    for page in pages:
        rel = page.relative_to(SITE).as_posix()
        samples, pool, notes = plan_page(page)
        notes_all.extend("%s: %s" % (rel, n) for n in notes)
        print("%s: %d samples" % (rel, len(samples)), flush=True)
        for s in samples:
            total += 1
            tag = "%s:%d (sample %d, %s)" % (rel, s["line"], s["index"], s["file"])
            if "data-host" in s["attrs"]:
                skipped += 1
                print("SKIP  %s  data-host=%r" % (tag, s["attrs"].get("data-host")))
                continue
            sdir = out / (rel.replace("/", "__").replace(".html", "") + "__%03d" % s["index"])
            sdir.mkdir()
            main_path = sdir / s["file"]
            main_path.write_text(s["code"], encoding="utf-8", newline="\n")
            for name, code in pool.items():
                if name == s["file"]:
                    continue
                target = sdir / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(code, encoding="utf-8", newline="\n")
            budget = s["attrs"].get("data-budget")
            results = {}
            for be in ("vm", "interp"):
                rc, so, se = run(runner, be, main_path, budget, args.timeout)
                results[be] = (rc, norm(so, sdir), norm(se, sdir))
            (rc_v, so_v, se_v), (rc_i, so_i, se_i) = results["vm"], results["interp"]
            if (so_v, se_v) != (so_i, se_i) or rc_v != rc_i:
                diverge.append((tag, results))
            host_label = bool(HOST_LABEL_RE.search(s["label"]))
            if rc_v is None or rc_i is None:
                load_fail.append((tag, "vm" if rc_v is None else "interp", "TIMEOUT", "", "", host_label))
            else:
                fails = looks_like_load_failure(se_v) or looks_like_load_failure(se_i)
                runtime_err = (rc_v == 1 and se_v.startswith("jaiscript:")) or \
                              (rc_i == 1 and se_i.startswith("jaiscript:"))
                if fails or runtime_err:
                    if expected_shows_error(s["expected"]):
                        error_ok.append((tag, (se_v or se_i).split("\n")[:2],
                                         [l for l in s["expected"].split("\n") if l.strip()][:2]))
                    else:
                        load_fail.append((tag, "both" if (se_v == se_i) else "vm/interp",
                                          "static-check/parse failure" if fails else "runtime error",
                                          so_v, se_v if se_v else se_i, host_label))
                elif host_label:
                    host_unmarked.append(tag)
            if not args.quiet_expected and rc_v is not None:
                so_c, se_c = so_v, se_v
                if s["expected_warnings"]:
                    _, so_c, se_c = run(runner, "vm", main_path, budget, args.timeout, True)
                    so_c, se_c = norm(so_c, sdir), norm(se_c, sdir)
                missing = compare_expected(s["expected"], so_c, se_c)
                if missing:
                    drift.append((tag + (" [--warnings]" if s["expected_warnings"] else ""), missing, so_c, se_c))

    print("\n%d samples on %d pages: %d skipped (data-host), %d intentional-error samples matched"
          % (total, len(pages), skipped, len(error_ok)))
    for n in notes_all:
        print("  note: " + n)
    if args.show_errors:
        print("\nINTENTIONAL-ERROR SAMPLES (runner text vs page text, first lines):")
        for tag, got, exp in error_ok:
            print("  %s" % tag)
            for l in got:
                print("      runner | " + l)
            for l in exp:
                print("      page   | " + l)

    hard = [f for f in load_fail if not f[5]]
    soft = [f for f in load_fail if f[5]]
    if hard:
        print("\nLOAD / RUNTIME FAILURES not shown by the page (%d):" % len(hard))
        for tag, be, why, so, se, _ in hard:
            print("  %s  [%s] %s" % (tag, be, why))
            for l in (se or so).split("\n")[:6]:
                print("      | " + l)
    if soft:
        print("\nHOST-DEPENDENT BY LABEL, NOT MARKED data-host, and failing standalone (%d):" % len(soft))
        for tag, be, why, so, se, _ in soft:
            print("  %s  [%s] %s" % (tag, be, why))
            for l in (se or so).split("\n")[:3]:
                print("      | " + l)
    if host_unmarked:
        print("\nHOST-DEPENDENT BY LABEL but runs standalone anyway (%d):" % len(host_unmarked))
        for tag in host_unmarked:
            print("  " + tag)
    if diverge:
        print("\nBACKEND DIVERGENCE (%d):" % len(diverge))
        for tag, res in diverge:
            print("  %s" % tag)
            for be in ("vm", "interp"):
                rc, so, se = res[be]
                print("    %s exit=%s" % (be, rc))
                for l in (so + ("\n" + se if se else "")).split("\n")[:12]:
                    print("      | " + l)
    if drift and not args.quiet_expected:
        print("\nEXPECTED-OUTPUT DRIFT (%s, %d): lines in the page's output block "
              "that the runner did not print" % ("informational" if args.allow_drift else "failure", len(drift)))
        for tag, missing, so, se in drift:
            print("  %s" % tag)
            for l in missing[:6]:
                print("      expected | " + l)
            for l in (so + ("\n" + se if se else "")).split("\n")[:8]:
                print("      got      | " + l)

    if not args.keep:
        shutil.rmtree(out, ignore_errors=True)
    return 1 if (hard or diverge or (drift and not args.allow_drift)) else 0


if __name__ == "__main__":
    sys.exit(main())
