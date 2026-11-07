#!/usr/bin/env bash
# The CI gates, one per argument, so the workflow can give each its own step and a
# local run can reproduce any of them:
#
#   BIN=build/bin tools/ci/gates.sh tests-interp tests-vm tests-vm-reference fuzz fuzz-vm-reference fuzz-order gloom demoreel crawler gatorquest embed-quickstart lsp lsp-latency spider selftest site
#   tools/ci/gates.sh all
#
# BIN is the directory holding the built executables (default: build/bin under the
# repository root; a relative BIN is taken from the repository root too). FUZZ_ITERATIONS
# sets a fuzz run's program count (default 300) and FUZZ_CORPUS a directory it keeps divergent programs in. The milestone gates outside `all`
# (gloom-workers, gloom-long, timedemo, amalgamation) are the battery's (tools/ci/battery.sh).
#
# The android gate cross-compiles the library and the suites with the Android NDK (libc++, the standard library Apple
# platforms use too; the 32-bit ABI catches pointer-size assumptions). It builds and runs nothing else, and skips with a
# message when no NDK is found. ANDROID_NDK_ROOT names the NDK (else ANDROID_NDK_HOME, ANDROID_NDK, or the newest ndk/
# under ANDROID_HOME, ANDROID_SDK_ROOT or the SDK's default home); ANDROID_ABIS the ABIs (default arm64-v8a armeabi-v7a);
# ANDROID_BUILD_DIR the build tree (default build-android, one directory per ABI; relative to the repository root).
#
# The two differentials: `fuzz` runs each program on the interpreter and the optimized VM, `fuzz-vm-reference` on the
# optimized VM and the VM in reference mode (engine::vm_reference_mode: every optimization off), so a divergence there is a
# bug of an optimized lane. `fuzz-order` runs the ordering band with call arguments. tests-vm-reference runs the suite with
# --backend=vm-reference.
#
# The asan gate builds its own Debug tree under AddressSanitizer (with UndefinedBehaviorSanitizer on GCC and Clang, at -O1;
# MSVC has no UBSan) and runs the suites on the interpreter and the VM, then a short differential fuzz run that must also
# report no crash. ASAN_BUILD_DIR names the tree (default build-asan, relative to the repository root) and FUZZ_ITERATIONS
# the fuzz count (default 100). With cl on PATH and no CXX set it builds with MSVC and copies the ASan runtime DLL beside the
# binaries. The interpreter's Debug suite runs for hours under it, so neither `all` nor the battery includes it.
#
# HASH_BACKENDS names the backends the runner's hash gates (gatorquest, timedemo) run (default "vm interp"; the per-change
# tier passes "vm" unless its interpreter tier is picked, tools/ci/targeted.sh). GLOOM, the demoreel, the crawler and the
# quickstart link the library without the interpreter: their gates run the VM, and skip when HASH_BACKENDS leaves it out.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${BIN:-build/bin}"
case "$BIN" in
	/* | [A-Za-z]:*) ;;
	*) BIN="$ROOT/$BIN" ;;
esac

# The reference hashes each example prints for its deterministic headless run.
GLOOM_FRAME=7fecc09815bb64e0
GLOOM_STATE=3580805725
GLOOM_SHOWCASE_FRAME=6559224cca20d1f0
DEMOREEL_STREAM=76322bdbe2343c29
CRAWLER_STATE=1743191104
GATORQUEST_STATE=3218840367
GLOOM_LONG_FRAME=dfd969ed5a336dca
GLOOM_LONG_STATE=1503537018
TIMEDEMO_STATE=1746772898

# Deep-recursion tests outgrow Linux's 8 MB main-thread stack in Debug builds; the
# Windows binaries link a 128 MB stack, so match it where the shell allows.
ulimit -s 131072 2>/dev/null || true

. "$ROOT/tools/ci/python.sh"
# Python buffers a pipe's output, and the watchdog (jai_gate_run --idle) reads silence as a hang
export PYTHONUNBUFFERED=1

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

run() {
	echo "+ $*"
	"$@"
}

# Runs the command with its output shown and a CR-stripped copy kept in $LOG.
run_logged() {
	echo "+ $*"
	"$@" 2>&1 | tr -d '\r' | tee "$LOG"
}

want() {
	grep -Eq -- "$2" "$LOG" || { echo "::error::$1"; exit 1; }
}

HASH_BACKENDS="${HASH_BACKENDS:-vm interp}"
hash_on() { case " $HASH_BACKENDS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

# JAI_TEST_SHARD=i/n runs one shard of the suites (jaiscript_tests --shard; tools/ci/ci_lib.sh jai_debug_suites); --progress prints a
# dot per test, the output the watchdog reads as progress. JAI_TEST_SHARDS=n runs a suite as n shards at once, each its own process
# and log, printed in shard order when all have ended (the Debug suites are single-threaded: a 4-vCPU runner takes them in a third
# of the time).
suite_run() {
	local backend="$1" shards="${JAI_TEST_SHARDS:-1}" dir i failed=0 pids=()
	if [ -n "${JAI_TEST_SHARD:-}" ] || [ "$shards" -le 1 ]; then
		run "$BIN/jaiscript_tests" --progress --backend="$backend" ${JAI_TEST_SHARD:+"--shard=$JAI_TEST_SHARD"}
		return
	fi
	dir="$(mktemp -d)"
	echo "+ $BIN/jaiscript_tests --progress --backend=$backend, shards 0..$((shards - 1)) of $shards at once"
	for ((i = 0; i < shards; i++)); do
		"$BIN/jaiscript_tests" --progress --backend="$backend" "--shard=$i/$shards" > "$dir/$i.log" 2>&1 &
		pids+=($!)
	done
	for ((i = 0; i < shards; i++)); do
		wait "${pids[$i]}" || failed=1
		echo "-- shard $i/$shards"
		tr -d '\r' < "$dir/$i.log"
	done
	rm -rf "$dir"
	[ "$failed" = 0 ] || { echo "::error::jaiscript_tests --backend=$backend: a shard failed"; return 1; }
}

gate_tests_interp() { suite_run interpreter; }
gate_tests_vm() { suite_run vm; }
gate_tests_vm_reference() { suite_run vm-reference; }

fuzz_run() {
	local corpus=()
	[ -n "${FUZZ_CORPUS:-}" ] && corpus=(--corpus "$FUZZ_CORPUS")
	run_logged "$BIN/jaiscript_fuzz" --seed-start 1 --iterations "${FUZZ_ITERATIONS:-300}" --max-depth 3 --timeout-ms 250 --isolate "${corpus[@]+"${corpus[@]}"}" "$@"
	want "fuzz: expected no new divergence" "new divergences: ?0\\b"
}

gate_fuzz() { fuzz_run; }
gate_fuzz_vm_reference() { fuzz_run --vm-reference; }

# The ordering band with call arguments (writing calls beside reads of what they write, three oracles: the backends, the
# metamorphic variant and the spec model), then the seeds past the band that once diverged
ORDER_SEEDS=(710115 710329 710548 710591 710665 710791 710960 760275 760410 760428 760626)
gate_fuzz_order() {
	fuzz_run --order-args
	local seed
	for seed in "${ORDER_SEEDS[@]}"; do
		run_logged "$BIN/jaiscript_fuzz" --seed "$seed" --order-args --max-depth 3 --timeout-ms 250 --isolate
		want "fuzz-order: expected no divergence at seed $seed" "new divergences: ?0\\b"
	done
}

vm_only() { hash_on vm || { echo "$1: its one backend is the VM, which HASH_BACKENDS leaves out; skipped"; return 1; }; }

# gloom_hashes <ticks> <workers> <frame hash> <STATE_HASH> [look] [example]: the reference look draws the frames the
# ports reproduce, the showcase look the game as played; both run the same simulation, so STATE_HASH is one number.
# gloom_idiomatic is the same game in another style and must draw the same frames.
gloom_hashes() {
	vm_only gloom || return 0
	cd "$ROOT/examples/gloom"
	local look="${5:-reference}" example="${6:-gloom}"
	run_logged "$BIN/jai_gloom" --smoke --ticks "$1" --seed 666 --workers "$2" --look "$look" --scripts "$ROOT/examples/$example/scripts"
	want "$example vm ($1 ticks, $2 workers, $look look): expected frame hash $3 and STATE_HASH $4" "^vm +\\|.*\\| $3 +\\| $4\$"
}

gate_gloom() {
	gloom_hashes 300 0 "$GLOOM_FRAME" "$GLOOM_STATE"
	gloom_hashes 300 0 "$GLOOM_SHOWCASE_FRAME" "$GLOOM_STATE" showcase
	gloom_hashes 300 0 "$GLOOM_FRAME" "$GLOOM_STATE" reference gloom_idiomatic
	gloom_hashes 300 0 "$GLOOM_SHOWCASE_FRAME" "$GLOOM_STATE" showcase gloom_idiomatic
}
gate_gloom_workers() {
	gloom_hashes 300 8 "$GLOOM_FRAME" "$GLOOM_STATE"
	gloom_hashes 300 8 "$GLOOM_SHOWCASE_FRAME" "$GLOOM_STATE" showcase
}
gate_gloom_long() { gloom_hashes 600 0 "$GLOOM_LONG_FRAME" "$GLOOM_LONG_STATE"; }

# JaiDOOM's timedemo and the game's own headless selftest (which loads jaidoom.jai, the entry point), where the checkout carries the example (it is optional)
gate_timedemo() {
	local dir="$ROOT/examples/jaidoom/scripts"
	[ -f "$dir/p10_timedemo.jai" ] || { echo "timedemo: examples/jaidoom is not in this checkout, skipped"; return 0; }
	cd "$dir"
	for backend in vm interp; do
		hash_on "$backend" || continue
		rm -rf ./.cache
		run_logged "$BIN/jaiscript" --no-pause --backend="$backend" p10_timedemo.jai -- 120x78
		want "timedemo $backend: expected STATE_HASH $TIMEDEMO_STATE" "STATE_HASH: $TIMEDEMO_STATE\\b"
		rm -rf ./.cache
		run_logged "$BIN/jaiscript" --no-pause --backend="$backend" jaidoom.jai -- selftest
		want "jaidoom selftest $backend: expected ALL GREEN" "SELFTEST: ALL GREEN"
	done
	rm -rf ./.cache
}

# source/jaiscript.cpp (the one-file build) includes every implementation file the library's CMake list names
gate_amalgamation() {
	local py
	py="$(jai_python)"
	run "$py" - "$ROOT" <<'PYEOF'
import re, sys, pathlib
root = pathlib.Path(sys.argv[1])
listed = set(re.findall(r"(?:implementation|interpreter)/[A-Za-z0-9_/]+\.cpp", (root / "source/CMakeLists.txt").read_text(encoding="utf-8", errors="replace")))
included = set(re.findall(r'#include\s+"((?:implementation|interpreter)/[A-Za-z0-9_/]+\.cpp)"', (root / "source/jaiscript.cpp").read_text(encoding="utf-8", errors="replace")))
separate = {"implementation/debug/connector.cpp", "implementation/debug/controller.cpp", "implementation/parallel_transform.cpp"}
missing = sorted(listed - included - separate)
if missing:
    print("::error::source/jaiscript.cpp must include every implementation file on the CMake list; missing: " + " ".join(missing))
    sys.exit(1)
print("amalgamation: every listed implementation file is included")
PYEOF
}

gate_demoreel() {
	vm_only demoreel || return 0
	run "$BIN/jai_demoreel" --smoke --expect "$DEMOREEL_STREAM"
}

gate_crawler() {
	vm_only crawler || return 0
	run_logged "$BIN/jai_crawler" --smoke --seed 6 --turns 1500
	want "crawler vm: expected STATE_HASH $CRAWLER_STATE" "STATE_HASH: $CRAWLER_STATE\$"
}

lsp_test() {
	local exe="$BIN/jaiscript_lsp" py
	[ -f "$exe.exe" ] && exe="$exe.exe"
	py="$(jai_python)"
	run "$py" "$ROOT/tools/lsp/tests/lsp_test.py" --exe "$exe" "$@"
}

gate_lsp() {
	lsp_test --only fixtures
	lsp_test --only gatorquest
	lsp_test --only robustness
}

gate_lsp_latency() { lsp_test --only latency; }

gate_gatorquest() {
	cd "$ROOT/examples/gatorquest"
	for backend in vm interp; do
		hash_on "$backend" || continue
		run_logged "$BIN/jaiscript" --backend="$backend" scripts/gatorquest.jai -- smoke ticks 2000 bots 2
		want "Gator Quest $backend: expected STATE_HASH $GATORQUEST_STATE" "STATE_HASH: $GATORQUEST_STATE\$"
	done
}

android_ndk() {
	local candidate sdk
	for candidate in "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK:-}"; do
		[ -n "$candidate" ] && [ -f "$candidate/build/cmake/android.toolchain.cmake" ] && { echo "$candidate"; return 0; }
	done
	for sdk in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "${LOCALAPPDATA:-}/Android/Sdk" "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do
		[ -n "$sdk" ] || continue
		command -v cygpath > /dev/null 2>&1 && sdk="$(cygpath -u "$sdk")"
		[ -d "$sdk/ndk" ] || continue
		candidate="$(ls -d "$sdk"/ndk/*/ 2> /dev/null | sort -V | tail -n 1)"
		candidate="${candidate%/}"
		[ -n "$candidate" ] && [ -f "$candidate/build/cmake/android.toolchain.cmake" ] && { echo "$candidate"; return 0; }
	done
	return 1
}

gate_android() {
	local ndk build abi exe extra=()
	ndk="$(android_ndk)" || { echo "android: no Android NDK found (set ANDROID_NDK_ROOT), skipped"; return 0; }
	if ! command -v ninja > /dev/null 2>&1; then
		for exe in "$ndk"/../../cmake/*/bin/ninja "$ndk"/../../cmake/*/bin/ninja.exe; do
			[ -f "$exe" ] && extra+=("-DCMAKE_MAKE_PROGRAM=$exe") && break
		done
	fi
	command -v cygpath > /dev/null 2>&1 && ndk="$(cygpath -m "$ndk")"
	# CMake hands clang++ a short 8.3 name for a path with spaces, which no longer selects its C++ driver (libc++ goes unlinked)
	case "$ndk" in *" "*) extra+=("-DCMAKE_CXX_FLAGS=--driver-mode=g++") ;; esac
	build="${ANDROID_BUILD_DIR:-build-android}"
	case "$build" in /* | [A-Za-z]:*) ;; *) build="$ROOT/$build" ;; esac
	echo "android: NDK $ndk"
	for abi in ${ANDROID_ABIS:-arm64-v8a armeabi-v7a}; do
		run cmake -Wno-deprecated -S "$ROOT" -B "$build/$abi" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake" \
			"-DANDROID_ABI=$abi" -DANDROID_PLATFORM=android-21 -DJAISCRIPT_BUILD_EXAMPLES=OFF -DJAISCRIPT_BUILD_RUNNER=OFF \
			-DJAISCRIPT_BUILD_LSP=OFF -DJAISCRIPT_INSTALL=OFF "${extra[@]+"${extra[@]}"}" > /dev/null
		run cmake --build "$build/$abi" --target jaiscript jaiscript_tests
		echo "android $abi: the library and the suites build"
	done
}

# The public headers (include/jaiscript less detail/, vm/, testing/ and net/openssl_tls.hpp, which needs OpenSSL's) each
# compile alone, the pages whose samples build a whole program compile with the includes they show
# (docs/site_tools/check_sample_includes.py), and tools/ci/host_warnings.cpp, a host file over the headers the embedding pages name, builds under
# -Wall -Wextra -Werror: with HEADER_CXX, else with each of g++ and clang++ found (on Windows, Visual Studio's clang++ when
# none is on PATH). JAI_BUILD_JOBS sets the parallel compiles (default 4).
header_compilers() {
	local c vswhere install
	if [ -n "${HEADER_CXX:-}" ]; then echo "$HEADER_CXX"; return; fi
	for c in g++ clang++; do command -v "$c" > /dev/null 2>&1 && echo "$c"; done
	command -v clang++ > /dev/null 2>&1 && return
	vswhere="$(printenv 'ProgramFiles(x86)' 2>/dev/null)/Microsoft Visual Studio/Installer/vswhere.exe"
	[ -f "$vswhere" ] || return 0
	install="$("$vswhere" -latest -products '*' -property installationPath 2>/dev/null | tr -d '\r' | head -n 1)"
	[ -n "$install" ] && [ -f "$install/VC/Tools/Llvm/x64/bin/clang++.exe" ] && echo "$install/VC/Tools/Llvm/x64/bin/clang++.exe"
	return 0
}

gate_headers() {
	local work compilers=() headers=() cxx failed=0 bad
	mapfile -t compilers < <(header_compilers)
	[ "${#compilers[@]}" -gt 0 ] || { echo "::error::headers: no g++ or clang++ found (set HEADER_CXX)"; exit 1; }
	mapfile -t headers < <(cd "$ROOT/include" && find jaiscript -name '*.hpp' ! -path 'jaiscript/detail/*' ! -path 'jaiscript/vm/*' \
		! -path 'jaiscript/testing/*' ! -name openssl_tls.hpp | sort)
	work="$(mktemp -d)"
	for cxx in "${compilers[@]}"; do
		echo "headers: $cxx, ${#headers[@]} headers alone"
		bad="$(printf '%s\n' "${headers[@]}" | xargs -P "${JAI_BUILD_JOBS:-4}" -I{} "$BASH" -c \
			'printf "#include <%s>\n" "$2" | "$1" -std=c++20 -I"$3" -fsyntax-only -x c++ - > "$4/$(printf %s "$2" | tr / _).log" 2>&1 || echo "$2"' \
			_ "$cxx" {} "$ROOT/include" "$work")"
		if [ -n "$bad" ]; then
			failed=1
			for h in $bad; do
				echo "::error::$h does not compile alone with $cxx"
				grep -m 3 -E "error" "$work/$(printf %s "$h" | tr / _).log" | cut -c1-240
			done
		fi
		echo "headers: $cxx, the complete pages' C++ samples with the includes they show"
		if ! "$(jai_python)" "$ROOT/docs/site_tools/check_sample_includes.py" --cxx "$cxx" > "$work/samples.log" 2>&1; then
			failed=1
			echo "::error::a page's C++ samples need an include the page does not show ($cxx)"
			grep -A 6 "^FAIL" "$work/samples.log" | head -14 | cut -c1-240
		fi
		echo "headers: $cxx, the host file under -Wall -Wextra -Werror"
		if ! "$cxx" -std=c++20 -I"$ROOT/include" -Wall -Wextra -Werror -O2 -c "$ROOT/tools/ci/host_warnings.cpp" -o "$work/host_warnings.o" > "$work/host.log" 2>&1; then
			failed=1
			echo "::error::tools/ci/host_warnings.cpp warns or fails with $cxx"
			grep -m 12 -E "warning|error" "$work/host.log" | cut -c1-240
		fi
	done
	rm -rf "$work"
	[ "$failed" = 0 ] || exit 1
	echo "headers: every public header compiles alone, the samples need no include their page hides, and the host file builds warning-free"
}

gate_embed_quickstart() {
	local work
	vm_only embed_quickstart || return 0
	work="$(mktemp -d)"
	cd "$work"
	run_logged "$BIN/embed_quickstart"
	want "embed_quickstart: expected every check to hold" "^all checks passed\$"
	cd "$ROOT"
	rm -rf "$work"
}

# jai_spider on a free port over a copy of its test app and of the guestbook example, and the runner's spider:: client
# against it, both backends
gate_spider() {
	local exe="$BIN/jai_spider" runner="$BIN/jaiscript" py
	[ -f "$exe.exe" ] && exe="$exe.exe"
	[ -f "$runner.exe" ] && runner="$runner.exe"
	py="$(jai_python)"
	run_logged "$py" "$ROOT/tools/jai_spider/tests/spider_test.py" --exe "$exe" --runner "$runner"
	want "spider: expected every check to hold" "^jai_spider: every check holds\$"
}

# The runner's built-in smoke tests (flags, file and -e modes, a scripted REPL session, the host file API)
gate_selftest() {
	cd "$ROOT"
	run "$BIN/jaiscript" --selftest
}

# The documentation site: links, structure, leakage and every sample on both backends (docs/site_tools/verify_site.py; it
# runs the host samples through jaiscript_tests beside the runner)
gate_site() {
	cd "$ROOT"
	run "$(jai_python)" docs/site_tools/verify_site.py --runner "$BIN/jaiscript"
}

gate_asan() {
	local build msvc=0 dll
	build="${ASAN_BUILD_DIR:-build-asan}"
	case "$build" in /* | [A-Za-z]:*) ;; *) build="$ROOT/$build" ;; esac
	if [ -z "${CXX:-}" ] && command -v cl > /dev/null 2>&1; then
		msvc=1
		# CMake's MSVC Debug flags carry /RTC1, which /fsanitize=address refuses
		run cmake -S "$ROOT" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl -DCMAKE_C_COMPILER=cl \
			"-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /fsanitize=address" "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /fsanitize=address" \
			"-DCMAKE_CXX_FLAGS_DEBUG=/Zi /Ob0 /Od" "-DCMAKE_C_FLAGS_DEBUG=/Zi /Ob0 /Od" \
			-DJAISCRIPT_BUILD_EXAMPLES=OFF -DJAISCRIPT_BUILD_RUNNER=OFF -DJAISCRIPT_BUILD_LSP=OFF -DJAISCRIPT_BUILD_SPIDER=OFF -DJAISCRIPT_INSTALL=OFF
	else
		run cmake -S "$ROOT" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
			"-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
			"-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
			"-DCMAKE_CXX_FLAGS_DEBUG=-g -O1" "-DCMAKE_C_FLAGS_DEBUG=-g -O1" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined" \
			-DJAISCRIPT_BUILD_EXAMPLES=OFF -DJAISCRIPT_BUILD_RUNNER=OFF -DJAISCRIPT_BUILD_LSP=OFF -DJAISCRIPT_BUILD_SPIDER=OFF -DJAISCRIPT_INSTALL=OFF
		# scripts can build strong cycles on purpose (a coroutine handle in its own receiver's field), so leaks are not errors here
		export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
		export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
	fi
	run cmake --build "$build" --target jaiscript_tests jaiscript_fuzz
	if [ "$msvc" = 1 ]; then
		dll="$(dirname "$(command -v cl)")/clang_rt.asan_dynamic-x86_64.dll"
		[ -f "$dll" ] || { echo "::error::asan: no $dll beside cl"; exit 1; }
		run cp -f "$dll" "$build/bin/"
	fi
	BIN="$build/bin"
	# ASan clears the stack an exception unwinds only on a stack of at most 64 MiB (__asan_handle_no_return); past that the
	# unwound frames stay poisoned and a later test reads them as a stack-use-after-scope
	ulimit -s 61440 2>/dev/null || true
	suite_run interpreter
	suite_run vm
	FUZZ_ITERATIONS="${FUZZ_ITERATIONS:-100}"
	fuzz_run --timeout-ms 2000
	want "asan fuzz: expected no crash" "^crashes: +0\b"
}

ALL=(tests-interp tests-vm tests-vm-reference fuzz fuzz-vm-reference fuzz-order gloom demoreel crawler gatorquest embed-quickstart lsp lsp-latency spider selftest site)
MILESTONE=(gloom-workers gloom-long timedemo amalgamation)
CROSS=(android headers)
SANITIZER=(asan)

[ "$#" -gt 0 ] || { echo "usage: $0 <gate>... (${ALL[*]} all; milestone: ${MILESTONE[*]}; cross: ${CROSS[*]}; sanitizer: ${SANITIZER[*]})" >&2; exit 2; }
[ "$1" = all ] && set -- "${ALL[@]}"

for gate in "$@"; do
	case "$gate" in
		tests-interp | tests-vm | tests-vm-reference | fuzz | fuzz-vm-reference | fuzz-order | gloom | demoreel | crawler | gatorquest | embed-quickstart | lsp | lsp-latency | spider | selftest | site) ;;
		gloom-workers | gloom-long | timedemo | amalgamation | android | asan | headers) ;;
		*) echo "unknown gate: $gate (${ALL[*]} all; milestone: ${MILESTONE[*]}; cross: ${CROSS[*]}; sanitizer: ${SANITIZER[*]})" >&2; exit 2 ;;
	esac
	echo "== gate: $gate"
	( "gate_${gate//-/_}" )
done
