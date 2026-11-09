#!/usr/bin/env bash
# The targeted tier: the checks a change needs, picked from the paths it touches.
#
#   tools/ci/targeted.sh [--plan] [--build] [--lanes] [--interp] <rev-range>       # e.g. HEAD~1..HEAD, master..my-branch
#                                                                                   # A..B diffs from the merge base of A and B, as A...B
#   tools/ci/targeted.sh [--plan] [--build] [--lanes] [--interp] --files <path>...  # paths relative to the package root
#
# --plan prints the checks and why each was picked, and runs nothing. --build configures and builds what the checks run
# (tools/ci/ci_lib.sh names the build directories and the toolchain). The last line is the verdict: TARGETED GREEN or
# TARGETED RED. The milestone tier is tools/ci/battery.sh, which runs everything below on both tiers.
#
# Two tiers. The VM tier runs for every change to code a program runs (the library, the VM, the interpreter, a kernel, the
# build): the Debug suites on the VM and on the VM reference (--backend=vm, --backend=vm-reference), the VM against
# VM-reference fuzz (gates.sh fuzz-vm-reference) and the example hashes on the VM; a change to the VM or its compiler adds the
# ordering fuzz (gates.sh fuzz-order). The interpreter tier adds the
# interpreter's Debug suites, the interpreter against VM fuzz (gates.sh fuzz) and the example hashes on the interpreter, and
# is picked only when a path touches
#   - the interpreter: source/interpreter/*, include/jaiscript/detail/interpreter.hpp
#   - the VM's general path or its compiler's front end: source/implementation/vm/vm_backend.cpp, vm_compiler.cpp and
#     include/jaiscript/vm/vm_backend.hpp, vm_compiler.hpp, chunk.hpp, opcode.hpp
#   - a kernel both backends share: include/jaiscript/detail/*
#   - the parser, the lexer, the checker or the builtin methods: source/implementation/parser.cpp, lexer.cpp,
#     type_checker.cpp, builtin_methods.cpp
#   - Gator Quest's or JaiDOOM's script (the runner runs them on both backends) or the fuzzer
# The VM's optimized lanes elsewhere (vm_inline.cpp, region_ir.*, the coverage and listing tools) take the VM tier only.
# A test change also takes the android gate: the Debug suites build with MSVC alone, whose template parsing accepts what
# Clang and GCC reject (a dependent `.as<T>()` without `template` in a generic lambda), and the NDK's Clang is the cheapest
# build of the suites with another compiler.
#
# --lanes declares a change confined to the VM's optimized lanes (fused, typed, cell, graft and cache code) inside the
# general files: the general-path reason is dropped and the VM against VM-reference fuzz is the change's oracle. --interp
# picks the interpreter tier whatever the paths.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/ci_lib.sh"

PLAN=0
BUILD=0
LANES=0
FORCE_INTERP=0
RANGE=""
FILES=()
while [ "$#" -gt 0 ]; do
	case "$1" in
		--plan) PLAN=1 ;;
		--build) BUILD=1 ;;
		--lanes) LANES=1 ;;
		--interp) FORCE_INTERP=1 ;;
		--files) shift; FILES=("$@"); break ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) RANGE="$1" ;;
	esac
	shift
done
if [ -n "$RANGE" ]; then
	case "$RANGE" in *...*) ;; *..*) RANGE="${RANGE/../...}" ;; esac   # a branch behind master must not count master's new commits
	mapfile -t FILES < <(git -C "$JAI_ROOT" diff --name-only --relative "$RANGE" -- . | tr -d '\r')
fi
[ -n "$RANGE" ] || [ "${#FILES[@]}" -gt 0 ] || { echo "usage: targeted.sh [--plan] [--build] [--lanes] [--interp] <rev-range> | --files <path>..." >&2; exit 2; }

CHECKS=""
REASONS=""
INTERP_REASONS=""
pick() {
	case " $CHECKS " in *" $1 "*) ;; *) CHECKS="$CHECKS $1" ;; esac
	REASONS="$REASONS"$'\n'"$1|$2"
}

# the interpreter tier's reason (a path's own words)
interp_tier() { INTERP_REASONS="$INTERP_REASONS"$'\n'"$1"; }

# the VM tier: the suites on the VM and the VM reference, the VM against VM-reference fuzz, every example's hash on the VM
vm_tier() {
	local game
	pick debug-suites "$1"
	pick fuzz-vm-reference "$1"
	for game in gloom demoreel crawler gatorquest; do pick "$game" "$1"; done
}

classify() {
	local p="$1"
	case "$p" in
		include/jaiscript/* | tools/ci/host_warnings.cpp | docs/site_tools/check_sample_includes.py | source/tests/integration/doc_samples_tests.cpp | docs/site/embedding/01-embedding-quickstart.html | docs/site/embedding/02-dynamic-binder.html)
			pick headers "the public headers compile alone and warning-free, and the whole-program samples show their includes ($p)" ;;
	esac
	case "$p" in
		docs/site/install.*) pick ci-scripts "the installers ($p)" ;;
	esac
	case "$p" in
		docs/site/* | docs/site_tools/*) pick site "the documentation site ($p)" ;;
		*.md | internal/* | docs/* | LICENSE* | .gitignore | .gitattributes) NOTES="$NOTES $p" ;;
		.github/*) pick ci-scripts "the CI workflow ($p)" ;;
		tools/ci/*) pick ci-scripts "the CI scripts ($p)" ;;
		tools/lsp/*) pick lsp "the language server ($p)" ;;
		tools/vscode-jaiscript/*) NOTES="$NOTES $p" ;;
		tools/install/*) pick ci-scripts "the installers' build script ($p)" ;;
		tools/jaiscript/*) pick gatorquest "the runner ($p)"; pick selftest "the runner's smoke tests ($p)"; pick site "the runner runs the site's samples ($p)"; pick spider "jai_spider shares the runner's file IO, sockets and HTTP transport ($p)" ;;
		tools/jai_spider/*) pick spider "the web host ($p)" ;;
		examples/gloom/*) pick gloom "the GLOOM example ($p)" ;;
		examples/demoreel/*) pick demoreel "the demoreel ($p)" ;;
		examples/crawler/*) pick crawler "the crawler ($p)"; interp_tier "an example's script runs on both backends ($p)" ;;
		examples/gatorquest/*) pick gatorquest "Gator Quest ($p)"; pick lsp "the language server checks Gator Quest ($p)"; interp_tier "an example's script runs on both backends ($p)" ;;
		examples/embed_quickstart/*) pick embed-quickstart "the embedding quickstart ($p)" ;;
		examples/jaidoom/*) pick timedemo "JaiDOOM ($p)"; interp_tier "an example's script runs on both backends ($p)" ;;
		examples/*) NOTES="$NOTES $p" ;;
		source/tests/fuzz/*) pick fuzz-vm-reference "the fuzzer ($p)"; pick debug-suites "the fuzzer's suite ($p)"; interp_tier "the fuzzer's interpreter pair ($p)"; pick android "Clang compiles the suites ($p)" ;;
		source/tests/*) pick debug-suites "a test ($p)"; pick android "Clang compiles the suites ($p)" ;;
		source/implementation/vm/vm_backend.cpp | source/implementation/vm/vm_compiler.cpp | \
		include/jaiscript/vm/vm_backend.hpp | include/jaiscript/vm/vm_compiler.hpp | include/jaiscript/vm/chunk.hpp | include/jaiscript/vm/opcode.hpp)
			vm_tier "the VM or its compiler ($p)"; pick fuzz-order "the VM or its compiler ($p)"; pick android "libc++ and 32-bit compile the library ($p)"
			[ "$LANES" = 1 ] || interp_tier "the VM's general path or its compiler's front end ($p)" ;;
		include/jaiscript/vm/* | source/implementation/vm/*) vm_tier "the VM's optimized lanes or its tools ($p)"; pick fuzz-order "the VM's optimized lanes ($p)"; pick android "libc++ and 32-bit compile the library ($p)" ;;
		source/interpreter/* | include/jaiscript/detail/interpreter.hpp)
			vm_tier "the interpreter ($p)"; interp_tier "the interpreter ($p)"; pick android "libc++ and 32-bit compile the library ($p)" ;;
		include/jaiscript/detail/* | source/implementation/parser.cpp | source/implementation/lexer.cpp | source/implementation/type_checker.cpp | \
		source/implementation/builtin_methods.cpp)
			vm_tier "a kernel both backends share ($p)"; interp_tier "a kernel both backends share ($p)"; pick android "libc++ and 32-bit compile the library ($p)"
			pick selftest "the runner's REPL retries on the parser's missing-';' text ($p)" ;;
		include/jaiscript/properties/* | include/jaiscript/properties.hpp | include/jaiscript/serialization/* | source/implementation/serialization/* | \
		source/implementation/properties.cpp | source/implementation/schema_binding.cpp)
			vm_tier "the property system or the archives ($p)"; HOST_ARCHIVES=1; pick android "libc++ and 32-bit compile the library ($p)" ;;
		source/implementation/stdlib/spider.cpp | include/jaiscript/stdlib/spider.hpp | include/jaiscript/core/http_transport.hpp | include/jaiscript/net/*)
			vm_tier "the spider:: client ($p)"; pick spider "the spider:: client against a live server ($p)"; pick android "libc++ and 32-bit compile the library ($p)" ;;
		include/* | source/implementation/*) vm_tier "the library ($p)"; pick android "libc++ and 32-bit compile the library ($p)" ;;
		CMakeLists.txt | */CMakeLists.txt | cmake/* | source/jaiscript.cpp | CMakePresets.json)
			vm_tier "the build ($p)"; pick amalgamation "the build's file lists ($p)"; pick android "libc++ and 32-bit compile the library ($p)" ;;
		*) vm_tier "a path no rule names ($p)" ;;
	esac
}

NOTES=""
HOST_ARCHIVES=0
for f in "${FILES[@]}"; do classify "$f"; done
[ "$FORCE_INTERP" = 1 ] && interp_tier "--interp"
INTERP=0
if [ -n "$INTERP_REASONS" ]; then
	INTERP=1
	case " $CHECKS " in *" debug-suites "* | *" fuzz-vm-reference "*) pick fuzz "the interpreter tier" ;; esac
fi
SUITE_BACKENDS="vm vm-reference"
export HASH_BACKENDS="vm"
if [ "$INTERP" = 1 ]; then SUITE_BACKENDS="interp vm vm-reference"; HASH_BACKENDS="vm interp"; fi

echo "targeted: ${#FILES[@]} path(s)${RANGE:+ in $RANGE}"
if [ -z "$CHECKS" ]; then
	echo "chose: nothing to run (notes and prose only:${NOTES:- none})"
	echo "TARGETED GREEN (nothing to check)"
	exit 0
fi
RUNS_CODE=0
for check in $CHECKS; do case "$check" in debug-suites | fuzz* | gloom | demoreel | crawler | gatorquest | timedemo) RUNS_CODE=1 ;; esac; done
if [ "$RUNS_CODE" = 0 ]; then
	:
elif [ "$INTERP" = 1 ]; then
	echo "tier: VM and interpreter, because $(printf '%s\n' "$INTERP_REASONS" | sed '/^$/d' | head -n 3 | paste -sd ';' - | sed 's/;/; /g')"
else
	echo "tier: VM (no path touches the interpreter, the VM's general path or front end, a shared kernel or the parser/checker$([ "$LANES" = 1 ] && echo "; --lanes"))"
fi
for check in $CHECKS; do
	echo "chose: $check, because $(printf '%s\n' "$REASONS" | grep "^$check|" | head -n 3 | cut -d'|' -f2- | paste -sd ';' - | sed 's/;/; /g')"
done
[ "$RUNS_CODE" = 1 ] && echo "suites on: $SUITE_BACKENDS; hashes on: $HASH_BACKENDS"
[ -n "$NOTES" ] && echo "no check for:$NOTES"
[ "$HOST_ARCHIVES" = 1 ] && echo "host note: the property system or the archives changed; a host runs its asset round-trip check too"
[ "$PLAN" = 1 ] && exit 0

if [ "$BUILD" = 1 ]; then
	debug_targets=""
	release_targets=""
	for check in $CHECKS; do
		case "$check" in
			debug-suites) debug_targets="$debug_targets jaiscript_tests" ;;
			fuzz | fuzz-vm-reference | fuzz-order) release_targets="$release_targets jaiscript_fuzz" ;;
			gloom) release_targets="$release_targets jai_gloom jaiscript_runner" ;;
			demoreel) release_targets="$release_targets jai_demoreel jaiscript_runner" ;;
			crawler) release_targets="$release_targets jai_crawler jaiscript_runner" ;;
			gatorquest | timedemo) release_targets="$release_targets jaiscript_runner" ;;
			site) release_targets="$release_targets jaiscript_runner jaiscript_tests" ;;
			selftest) release_targets="$release_targets jaiscript_runner" ;;
			lsp) release_targets="$release_targets jaiscript_lsp" ;;
			embed-quickstart) release_targets="$release_targets embed_quickstart" ;;
			spider) release_targets="$release_targets jai_spider jaiscript_runner" ;;
		esac
	done
	# shellcheck disable=SC2086
	[ -z "$debug_targets" ] || jai_build debug $(echo $debug_targets | tr ' ' '\n' | sort -u) || { echo "TARGETED RED: the Debug build failed"; exit 1; }
	# shellcheck disable=SC2086
	[ -z "$release_targets" ] || jai_build release $(echo $release_targets | tr ' ' '\n' | sort -u) || { echo "TARGETED RED: the Release build failed"; exit 1; }
fi

jai_watchdog
FAILED=""
SIDE_GATES=()
TIMING_GATES=()
debug_pid=""
for check in $CHECKS; do
	case "$check" in
		# shellcheck disable=SC2086
		debug-suites)
			{ jai_debug_suites $SUITE_BACKENDS || echo "DEBUG-SUITES-RED"; } > "$JAI_CI_LOG_DIR/targeted_debug.out" 2>&1 &
			debug_pid=$!
			;;
		ci-scripts) ;;
		*) if jai_timing_gate "$check"; then TIMING_GATES+=("$check"); else SIDE_GATES+=("$check"); fi ;;
	esac
done
[ "${#SIDE_GATES[@]}" = 0 ] || { jai_gates_side_by_side release "${SIDE_GATES[@]}"; FAILED="$FAILED$JAI_RED"; }
for check in $CHECKS; do
	case "$check" in
		ci-scripts)
			bad=0
			for script in "$JAI_ROOT"/tools/ci/*.sh; do bash -n "$script" || bad=1; done
			sh -n "$JAI_ROOT/docs/site/install.sh" || bad=1
			install_ps1="$JAI_ROOT/docs/site/install.ps1"
			! command -v cygpath > /dev/null 2>&1 || install_ps1="$(cygpath -m "$install_ps1")"
			for ps in pwsh powershell; do
				command -v "$ps" > /dev/null 2>&1 || continue
				"$ps" -NoProfile -Command "\$e = \$null; [void][System.Management.Automation.Language.Parser]::ParseFile('$install_ps1', [ref]\$null, [ref]\$e); exit [int](\$e.Count -gt 0)" || bad=1
			done
			if [ "$bad" = 0 ]; then echo "green: ci-scripts (every tools/ci script and installer parses)"; else echo "red: ci-scripts"; FAILED="$FAILED ci-scripts"; fi
			;;
	esac
done
if [ -n "$debug_pid" ]; then
	wait "$debug_pid" || true
	cat "$JAI_CI_LOG_DIR/targeted_debug.out"
	grep -q "DEBUG-SUITES-RED" "$JAI_CI_LOG_DIR/targeted_debug.out" && FAILED="$FAILED debug-suites"
fi
for check in "${TIMING_GATES[@]+"${TIMING_GATES[@]}"}"; do jai_gates release "$check" || FAILED="$FAILED $check"; done

if [ -z "$FAILED" ]; then echo "TARGETED GREEN:$CHECKS"; else echo "TARGETED RED:$FAILED"; exit 1; fi
