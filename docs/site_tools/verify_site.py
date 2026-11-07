#!/usr/bin/env python3
"""Run every site check in one go.

    python site_tools/verify_site.py [--no-samples] [--runner PATH]

Order: script syntax (node --check), links and anchors, HTML structure, internal leakage and stale facts, retired error
texts, search index freshness, chrome freshness, then (unless --no-samples) every jaiscript
sample on both backends under strict, with its expected output, and the host samples: the C++ samples and the
host-dependent script samples a page marks data-sample, which the "Doc Samples" suite of jaiscript_tests (beside the
runner) compiles from its own copy of their code, checks against the page and runs, on both backends. Exit code 1 if any
check fails.
"""

import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent

STEPS = [
    ("links", ["check_links.py"]),
    ("html", ["check_html.py"]),
    ("leakage / stale facts", ["check_leakage.py"]),
    ("retired error texts", ["check_old_texts.py"]),
    ("search index", ["build_search_index.py", "--check"]),
    ("chrome", ["apply_chrome.py", "--check"]),
    ("api catalog", ["check_api_catalog.py"]),
    ("samples", ["run_samples.py"]),
]


def main():
    skip_samples = "--no-samples" in sys.argv
    runner = []
    if "--runner" in sys.argv:
        runner = ["--runner", str(pathlib.Path(sys.argv[sys.argv.index("--runner") + 1]).resolve())]
    failed = []
    print("=" * 72)
    print("== scripts")
    node = shutil.which("node")
    if node:
        bad = [js.name for js in sorted((HERE.parent / "site" / "assets").glob("*.js"))
               if subprocess.call([node, "--check", str(js)]) != 0]
        print("-- scripts: %s" % ("FAILED (%s)" % ", ".join(bad) if bad else "ok"))
        if bad:
            failed.append("scripts")
    else:
        print("-- scripts: skipped (node not on PATH)")
    for name, cmd in STEPS:
        if name == "samples" and skip_samples:
            continue
        print("=" * 72)
        print("== " + name)
        extra = runner if name == "samples" else []
        rc = subprocess.call([sys.executable, str(HERE / cmd[0])] + cmd[1:] + extra, cwd=str(HERE.parent / "site"))
        print("-- %s: %s" % (name, "ok" if rc == 0 else "FAILED (exit %d)" % rc))
        if rc:
            failed.append(name)
    if not skip_samples:
        print("=" * 72)
        print("== host samples")
        runner_path = pathlib.Path(runner[1]) if runner else None
        bin_dir = runner_path.parent if runner_path else HERE.parents[1] / "build" / "bin"
        tests = next((bin_dir / n for n in ("jaiscript_tests.exe", "jaiscript_tests") if (bin_dir / n).exists()), None)
        if tests is None:
            print("-- host samples: FAILED (no jaiscript_tests in %s: build it beside the runner)" % bin_dir)
            failed.append("host samples")
        else:
            rc = 0
            for backend in ([], ["--backend=vm"]):
                rc |= subprocess.call([str(tests), "Doc Samples"] + backend, cwd=str(bin_dir))
            print("-- host samples: %s" % ("ok" if rc == 0 else "FAILED"))
            if rc:
                failed.append("host samples")
    print("=" * 72)
    print("failed: %s" % (", ".join(failed) if failed else "none"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
