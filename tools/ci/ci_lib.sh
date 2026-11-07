#!/usr/bin/env bash
# Shared by targeted.sh and battery.sh: the package root, the build directories and the build step.
#
#   JAI_DEBUG_DIR, JAI_RELEASE_DIR   the Debug and Release build directories (default build-debug and build; relative to the
#                                    package root unless absolute). Each binary lives in <dir>/bin.
#   JAI_CMAKE_ARGS_DEBUG/_RELEASE    extra arguments for a first configure
#   JAI_BUILD_JOBS                   parallel build jobs (default 4)
#   JAI_CI_LOG_DIR                   where build and check logs go (default jaiscript-ci-<checkout id> in the system temp
#                                    directory: one folder per checkout, so worktrees running checks at once keep their logs apart)
#   JAI_GATE_IDLE                    seconds a gate may print nothing before its watchdog (jai_gate_run,
#                                    tools/ci/jai_gate_run.cpp) writes minidumps of its processes to <log dir>/dumps and kills
#                                    them (default 900; 0: none): a hang ends a gate, slowness under load never does
#   JAI_GATE_TIMEOUT                 seconds a gate may run in all before the same (default 0: none); JAI_WATCHDOG names the
#                                    watchdog (default: from either build; empty: none)
#                                    Values passed through the watchdog get MSYS path conversion (/W4 arrives as C:/.../W4)
#   VSDEVCMD                         on Windows, the VsDevCmd.bat a build runs under when cl is not on PATH (default: the
#                                    latest Visual Studio vswhere reports)

JAI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$JAI_ROOT/tools/ci/timing_lock.sh"

jai_abs() {
	case "$1" in
		/* | [A-Za-z]:*) echo "$1" ;;
		*) echo "$JAI_ROOT/$1" ;;
	esac
}

JAI_DEBUG_DIR="$(jai_abs "${JAI_DEBUG_DIR:-build-debug}")"
JAI_RELEASE_DIR="$(jai_abs "${JAI_RELEASE_DIR:-build}")"
JAI_CI_LOG_DIR="${JAI_CI_LOG_DIR:-$(lockq_temp_dir)/jaiscript-ci-$(printf '%s' "$JAI_ROOT" | cksum | cut -d' ' -f1)}"
mkdir -p "$JAI_CI_LOG_DIR"

. "$JAI_ROOT/tools/ci/python.sh"

jai_dir_of() { [ "$1" = debug ] && echo "$JAI_DEBUG_DIR" || echo "$JAI_RELEASE_DIR"; }

jai_on_windows() { case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) return 0 ;; *) return 1 ;; esac; }

jai_vsdevcmd() {
	[ -n "${VSDEVCMD:-}" ] && { echo "$VSDEVCMD"; return; }
	local vswhere install
	vswhere="$(printenv 'ProgramFiles(x86)' 2>/dev/null)/Microsoft Visual Studio/Installer/vswhere.exe"
	[ -f "$vswhere" ] || return 1
	install="$("$vswhere" -latest -products '*' -property installationPath 2>/dev/null | tr -d '\r' | head -n 1)"
	[ -n "$install" ] && [ -f "$install/Common7/Tools/VsDevCmd.bat" ] && echo "$install/Common7/Tools/VsDevCmd.bat"
}

# cmake with the compiler's environment: as is where cl (or any other toolchain) is already on PATH, else on Windows under
# VsDevCmd. Arguments are passed through; output goes to the log named first.
jai_cmake() {
	local log="$1"
	shift
	if ! jai_on_windows || command -v cl >/dev/null 2>&1; then
		cmake "$@" >> "$log" 2>&1
		return
	fi
	local vsdev bat arg line=""
	vsdev="$(jai_vsdevcmd)" || { echo "no Visual Studio found: set VSDEVCMD to its VsDevCmd.bat" >> "$log"; return 1; }
	for arg in "$@"; do
		case "$arg" in
			/*) arg="$(cygpath -w "$arg")" ;;
		esac
		line="$line \"$arg\""
	done
	bat="$(mktemp --suffix=.bat)"
	printf '@call "%s" -arch=x64 >nul 2>&1\r\n@cmake%s >> "%s" 2>&1\r\n@exit /b %%errorlevel%%\r\n' "$(cygpath -w "$vsdev")" "$line" "$(cygpath -w "$log")" > "$bat"
	cmd //c "$(cygpath -w "$bat")"
	local rc=$?
	rm -f "$bat"
	return $rc
}

# jai_build <debug|release> <target>...: configure (new test files are found at configure time), then build the targets
jai_build() {
	local cfg="$1" dir log type
	shift
	dir="$(jai_dir_of "$cfg")"
	log="$JAI_CI_LOG_DIR/build_${cfg}.log"
	: > "$log"
	if [ -f "$dir/CMakeCache.txt" ]; then
		jai_cmake "$log" -S "$JAI_ROOT" -B "$dir" || { jai_build_failed "$cfg" "$log"; return 1; }
	else
		type=$([ "$cfg" = debug ] && echo Debug || echo Release)
		local extra=()
		if [ "$cfg" = debug ]; then read -r -a extra <<< "${JAI_CMAKE_ARGS_DEBUG:-}"; else read -r -a extra <<< "${JAI_CMAKE_ARGS_RELEASE:-}"; fi
		jai_cmake "$log" -S "$JAI_ROOT" -B "$dir" -G Ninja "-DCMAKE_BUILD_TYPE=$type" "${extra[@]+"${extra[@]}"}" || { jai_build_failed "$cfg" "$log"; return 1; }
	fi
	local targets=() t
	for t in "$@" jai_gate_run; do targets+=(--target "$t"); done
	jai_cmake "$log" --build "$dir" "${targets[@]}" -j "${JAI_BUILD_JOBS:-4}" || { jai_build_failed "$cfg" "$log"; return 1; }
	echo "built $cfg: $*"
}

jai_build_failed() {
	echo "BUILD FAILED: $1 (log: $2)"
	grep -E "error C[0-9]+|error LNK|error:|ninja: error|CMake Error" "$2" | head -6 | cut -c1-220
}

# jai_watchdog: finds the watchdog once (the shells a run starts inherit it); without one the gates run unwrapped
jai_watchdog() {
	[ -n "${JAI_WATCHDOG+set}" ] && return 0
	local exe
	JAI_WATCHDOG=""
	for exe in "$JAI_RELEASE_DIR/bin/jai_gate_run" "$JAI_DEBUG_DIR/bin/jai_gate_run"; do
		[ -f "$exe" ] || [ -f "$exe.exe" ] || continue
		# a build from before --idle refuses it, which would turn every gate red
		"$exe" --idle=1 -- "$BASH" -c : > /dev/null 2>&1 && { JAI_WATCHDOG="$exe"; break; }
		echo "$exe predates --idle: rebuild it (jai_build builds it with any target)"
	done
	export JAI_WATCHDOG
	[ -n "$JAI_WATCHDOG" ] || echo "no jai_gate_run in either build: the gates run without a watchdog"
}

# jai_watched <program> <args...>: the program under the watchdog when jai_watchdog found one, else as is
jai_watched() {
	if [ -n "$JAI_WATCHDOG" ]; then
		"$JAI_WATCHDOG" "--idle=${JAI_GATE_IDLE:-900}" "--timeout=${JAI_GATE_TIMEOUT:-0}" "--dump-dir=$JAI_CI_LOG_DIR/dumps" -- "$@"
	else
		"$@"
	fi
}

# jai_gates <debug|release> <gate>...: tools/ci/gates.sh against that build under the watchdog, its output in a log; prints
# one line per gate
jai_gates() {
	local cfg="$1" log
	shift
	jai_watchdog
	log="$JAI_CI_LOG_DIR/gates_${cfg}_$(echo "$*" | tr ' ' '_')${JAI_TEST_SHARD:+_shard${JAI_TEST_SHARD%%/*}}.log"
	if BIN="$(jai_dir_of "$cfg")/bin" jai_watched "$BASH" "$JAI_ROOT/tools/ci/gates.sh" "$@" > "$log" 2>&1; then
		echo "green: $* ($cfg)"
		return 0
	fi
	echo "red: $* ($cfg; log: $log)"
	grep -aE "::error::|^  x |Failed tests|FAIL|watchdog: .* (exceeded|printed nothing for) [0-9]+ s" "$log" | head -8 | cut -c1-220
	return 1
}

# jai_timing_gate <gate>: gates that measure time run alone, after everything else
jai_timing_gate() { case "$1" in lsp-latency) return 0 ;; *) return 1 ;; esac; }

# jai_gates_side_by_side <cfg> <gate>...: the gates at once, each with its own temp directory; the gates are deterministic, so
# a red side by side is the verdict; prints one line per gate, returns the red ones in JAI_RED
jai_gates_side_by_side() {
	local cfg="$1" gate pids=() tmp
	shift
	JAI_RED=""
	for gate in "$@"; do
		tmp="$JAI_CI_LOG_DIR/tmp_${cfg}_$gate"
		mkdir -p "$tmp"
		TEMP="$tmp" TMP="$tmp" TMPDIR="$tmp" jai_gates "$cfg" "$gate" > "$JAI_CI_LOG_DIR/gate_${cfg}_$gate.line" 2>&1 &
		pids+=($!)
	done
	for gate in "${pids[@]}"; do wait "$gate" || true; done
	for gate in "$@"; do
		cat "$JAI_CI_LOG_DIR/gate_${cfg}_$gate.line"
		grep -q "^green" "$JAI_CI_LOG_DIR/gate_${cfg}_$gate.line" || JAI_RED="$JAI_RED $gate"
	done
}

# jai_debug_suites [backend...]: the Debug suites on the named backends (interp, vm, vm-reference; default all three), side by
# side, each backend split into JAI_DEBUG_SHARDS processes (jaiscript_tests --shard; default from the core count), each
# shard with its own temp directory; one line per green backend, and each red shard's lines
jai_debug_shards() {
	local cores
	[ -n "${JAI_DEBUG_SHARDS:-}" ] && { echo "$JAI_DEBUG_SHARDS"; return; }
	cores="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
	# a quarter of the cores per backend: other work shares the machine
	cores=$((cores / 4))
	[ "$cores" -lt 1 ] && cores=1
	[ "$cores" -gt 8 ] && cores=8
	echo "$cores"
}

jai_debug_suites() {
	local be shard dir pids=() rc=0 red backends=("$@") shards
	[ "${#backends[@]}" -gt 0 ] || backends=(interp vm vm-reference)
	shards="$(jai_debug_shards)"
	for be in "${backends[@]}"; do
		for ((shard = 0; shard < shards; shard++)); do
			dir="$JAI_CI_LOG_DIR/tmp_${be}_$shard"
			mkdir -p "$dir"
			TEMP="$dir" TMP="$dir" TMPDIR="$dir" JAI_TEST_SHARD="$shard/$shards" jai_gates debug "tests-$be" > "$JAI_CI_LOG_DIR/suite_${be}_$shard.line" &
			pids+=($!)
		done
	done
	for be in "${pids[@]}"; do wait "$be" || true; done
	for be in "${backends[@]}"; do
		red=""
		for ((shard = 0; shard < shards; shard++)); do
			grep -q "^green" "$JAI_CI_LOG_DIR/suite_${be}_$shard.line" || red="$red $shard"
		done
		if [ -z "$red" ]; then
			echo "green: tests-$be (debug; $shards shards)"
			continue
		fi
		for shard in $red; do
			cat "$JAI_CI_LOG_DIR/suite_${be}_$shard.line"
		done
		rc=1
	done
	return $rc
}
