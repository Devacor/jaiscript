#!/usr/bin/env bash
# The milestone tier: every gate, at milestone depth, on both tiers of tools/ci/targeted.sh. The Debug suites on the
# interpreter, the VM and the VM reference, then on the Release build the same suites, a 3000-program run of each
# differential (BATTERY_FUZZ_ITERATIONS: the interpreter against the VM, the VM against the VM reference), every example's
# hashes (GLOOM with and without workers and at 600 ticks, the demoreel and the crawler on the VM, their hosts' one backend,
# and the two through the runner on both; Gator Quest and JaiDOOM's timedemo, where the checkout has it, on both through the
# runner), the embedding quickstart, the language server, the one-file build's include list, the runner's selftest and the
# documentation site.
#
#   tools/ci/battery.sh [--build]
#
# --build configures and builds every target first (tools/ci/ci_lib.sh names the build directories and the toolchain);
# BATTERY_SKIP_DEBUG=1 skips the Debug suites when they already passed at this commit.
# The last line is the verdict: BATTERY GREEN or BATTERY RED. The per-change tier is tools/ci/targeted.sh.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/ci_lib.sh"

if [ "${1:-}" = "--build" ]; then
	jai_build debug jaiscript_tests || { echo "BATTERY RED: the Debug build failed"; exit 1; }
	jai_build release jaiscript_tests jaiscript_runner jai_gloom jai_crawler jai_demoreel jaiscript_fuzz jaiscript_lsp embed_quickstart ||
		{ echo "BATTERY RED: the Release build failed"; exit 1; }
fi

jai_watchdog
FAILED=""
export HASH_BACKENDS="vm interp"
export FUZZ_ITERATIONS="${BATTERY_FUZZ_ITERATIONS:-3000}"
if [ "${BATTERY_SKIP_DEBUG:-0}" = 1 ]; then
	echo "skipped: the Debug suites (already green at this commit)"
	: > "$JAI_CI_LOG_DIR/battery_debug.out"
else
	{ jai_debug_suites interp vm vm-reference || echo "DEBUG-SUITES-RED"; } > "$JAI_CI_LOG_DIR/battery_debug.out" 2>&1 &
	debug_pid=$!
fi
jai_gates_side_by_side release tests-interp tests-vm tests-vm-reference fuzz fuzz-vm-reference fuzz-order gloom gloom-workers gloom-long demoreel crawler gatorquest timedemo embed-quickstart lsp amalgamation selftest site
FAILED="$FAILED$JAI_RED"
if [ -n "${debug_pid:-}" ]; then
	wait "$debug_pid" || true
	cat "$JAI_CI_LOG_DIR/battery_debug.out"
	grep -q "DEBUG-SUITES-RED" "$JAI_CI_LOG_DIR/battery_debug.out" && FAILED="$FAILED debug-suites"
fi
jai_gates release lsp-latency || FAILED="$FAILED lsp-latency"

if [ -z "$FAILED" ]; then echo "BATTERY GREEN"; else echo "BATTERY RED:$FAILED"; exit 1; fi
