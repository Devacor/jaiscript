#!/usr/bin/env bash
# Shared by targeted.sh and battery.sh: the package root, the build directories, the build step and the gate stamps.
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
		[ "$#" = 1 ] && [ -z "${JAI_TEST_SHARD:-}" ] && jai_gate_stamp "$1" "$cfg"
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

# jai_suites <debug|release> [backend...]: the suites on that build on the named backends (interp, vm, vm-reference; default
# all three), side by side, each backend split into JAI_SUITE_SHARDS processes (jaiscript_tests --shard; default from the core
# count), each shard with its own temp directory; one line per green backend, and each red shard's lines
jai_suite_shards() {
	local cores
	[ -n "${JAI_SUITE_SHARDS:-}" ] && { echo "$JAI_SUITE_SHARDS"; return; }
	cores="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
	# a quarter of the cores per backend: other work shares the machine
	cores=$((cores / 4))
	[ "$cores" -lt 1 ] && cores=1
	[ "$cores" -gt 8 ] && cores=8
	echo "$cores"
}

jai_suites() {
	local cfg="$1" be shard dir pids=() rc=0 red backends shards
	shift
	backends=("$@")
	[ "${#backends[@]}" -gt 0 ] || backends=(interp vm vm-reference)
	shards="$(jai_suite_shards)"
	for be in "${backends[@]}"; do
		for ((shard = 0; shard < shards; shard++)); do
			dir="$JAI_CI_LOG_DIR/tmp_${cfg}_${be}_$shard"
			mkdir -p "$dir"
			TEMP="$dir" TMP="$dir" TMPDIR="$dir" JAI_TEST_SHARD="$shard/$shards" jai_gates "$cfg" "tests-$be" > "$JAI_CI_LOG_DIR/suite_${cfg}_${be}_$shard.line" &
			pids+=($!)
		done
	done
	for be in "${pids[@]}"; do wait "$be" || true; done
	for be in "${backends[@]}"; do
		red=""
		for ((shard = 0; shard < shards; shard++)); do
			grep -q "^green" "$JAI_CI_LOG_DIR/suite_${cfg}_${be}_$shard.line" || red="$red $shard"
		done
		if [ -z "$red" ]; then
			echo "green: tests-$be ($cfg; $shards shards)"
			jai_gate_stamp "tests-$be" "$cfg"
			continue
		fi
		for shard in $red; do
			cat "$JAI_CI_LOG_DIR/suite_${cfg}_${be}_$shard.line"
		done
		rc=1
	done
	return $rc
}

# Gate stamps. A gate that passes in a run begun with jai_stamp_begin writes a stamp: the tree (git rev-parse HEAD^{tree}),
# the gate, its key (the configuration that changes what a green run proves, and the toolchain it ran with), the commit, the
# checkout, the log folder and the time. A landing (targeted.sh --landing) skips a gate when jai_stamp_trusted finds a stamp
# whose tree, gate and key all equal the landing's and that is younger than JAI_STAMP_DAYS; anything else, a missing or
# unreadable stamp included, runs the gate. A stamp decides whether a gate runs, never what its run reports.
#   JAI_STAMP_DIR     the stamp folder every checkout shares (default jai_gate_stamps in the system temp directory)
#   JAI_STAMP_DAYS    how many days a stamp is trusted (default 7)
#   LAND_NO_STAMPS=1  trusts no stamp, so every gate runs; green runs still write theirs
JAI_STAMP_DIR="${JAI_STAMP_DIR:-$(lockq_temp_dir)/jai_gate_stamps}"
JAI_STAMP_TREE=""
JAI_STAMP_SCOPE=()

jai_digest() {
	if command -v sha1sum >/dev/null 2>&1; then sha1sum | cut -c1-16
	elif command -v shasum >/dev/null 2>&1; then shasum | cut -c1-16
	else cksum | tr ' ' '-'; fi
}

jai_stamp_days() { case "${JAI_STAMP_DAYS:-}" in '' | *[!0-9]*) echo 7 ;; *) echo "$JAI_STAMP_DAYS" ;; esac; }

# jai_stamp_clean <pathspec>...: no tracked change anywhere and no untracked file (outside .gitignore) under the pathspecs,
# since a file the build reads but the commit lacks (a header never added) would stamp a tree that does not build
jai_stamp_clean() {
	local out
	out="$(git -C "$JAI_ROOT" status --porcelain --untracked-files=no 2>/dev/null)" && [ -z "$out" ] || return 1
	out="$(git -C "$JAI_ROOT" status --porcelain --untracked-files=all -- "$@" 2>/dev/null)" && [ -z "$out" ]
}

# jai_stamp_begin <pathspec>...: this run writes stamps only if it starts clean (jai_stamp_clean over the pathspecs, relative
# to the package root) and each gate still finds that tree clean when it passes
jai_stamp_begin() {
	JAI_STAMP_SCOPE=("$@")
	JAI_STAMP_TREE="$(git -C "$JAI_ROOT" rev-parse 'HEAD^{tree}' 2>/dev/null)" || JAI_STAMP_TREE=""
	if [ -z "$JAI_STAMP_TREE" ] || ! jai_stamp_clean "$@"; then
		JAI_STAMP_TREE=""
		echo "stamps: none written by this run (the checkout has a change or an untracked file its commit lacks)"
	fi
}

jai_stamp_file() { echo "$JAI_STAMP_DIR/$1.$2.$(printf '%s\n' "$3" | jai_digest).stamp"; }

# jai_stamp_write <gate> <key>: a green gate's stamp, when the run began clean and the checkout is still clean at that tree
jai_stamp_write() {
	local gate="$1" key="$2" file
	[ -n "$JAI_STAMP_TREE" ] && [ -n "$key" ] || return 0
	if [ "$(git -C "$JAI_ROOT" rev-parse 'HEAD^{tree}' 2>/dev/null)" != "$JAI_STAMP_TREE" ] || ! jai_stamp_clean "${JAI_STAMP_SCOPE[@]+"${JAI_STAMP_SCOPE[@]}"}"; then
		echo "stamps: no stamp for $gate (the checkout changed during the run)"
		return 0
	fi
	mkdir -p "$JAI_STAMP_DIR" || return 0
	file="$(jai_stamp_file "$JAI_STAMP_TREE" "$gate" "$key")"
	{
		echo "stamp 1"
		echo "tree $JAI_STAMP_TREE"
		echo "gate $gate"
		printf '%s\n' "$key"
		echo "commit $(git -C "$JAI_ROOT" rev-parse HEAD)"
		echo "checkout $(git -C "$JAI_ROOT" rev-parse --show-toplevel)"
		echo "log $(cygpath -m "$JAI_CI_LOG_DIR" 2>/dev/null || echo "$JAI_CI_LOG_DIR")"
		echo "time $(date +%s) $(date '+%Y-%m-%d %H:%M')"
	} > "$file.$$.tmp" && mv -f "$file.$$.tmp" "$file"
	find "$JAI_STAMP_DIR" -maxdepth 1 -name '*.stamp' -mtime +"$(jai_stamp_days)" -delete 2>/dev/null
	return 0
}

# jai_stamp_trusted <tree> <gate> <key>: succeeds, printing the run it trusts, only for a stamp of that tree and gate whose key
# is the same text and whose time is under JAI_STAMP_DAYS old (and not ahead of the clock)
jai_stamp_trusted() {
	local tree="$1" gate="$2" key="$3" file line first="" s_tree="" s_gate="" s_key="" s_time="" s_commit="" s_checkout="" s_when="" now
	[ "${LAND_NO_STAMPS:-0}" != 1 ] && [ -n "$tree" ] && [ -n "$key" ] || return 1
	file="$(jai_stamp_file "$tree" "$gate" "$key")"
	[ -f "$file" ] || return 1
	while IFS= read -r line || [ -n "$line" ]; do
		line="${line%$'\r'}"
		[ -n "$first" ] || first="$line"
		case "$line" in
			"tree "*) s_tree="${line#tree }" ;;
			"gate "*) s_gate="${line#gate }" ;;
			"config "* | "toolchain "*) s_key="$s_key${s_key:+$'\n'}$line" ;;
			"commit "*) s_commit="${line#commit }" ;;
			"checkout "*) s_checkout="${line#checkout }" ;;
			"time "*) s_time="${line#time }"; s_when="${s_time#* }"; s_time="${s_time%% *}" ;;
		esac
	done < "$file"
	[ "$first" = "stamp 1" ] && [ "$s_tree" = "$tree" ] && [ "$s_gate" = "$gate" ] && [ "$s_key" = "$key" ] || return 1
	case "$s_time" in '' | *[!0-9]*) return 1 ;; esac
	now="$(date +%s)"
	[ "$s_time" -le $((now + 300)) ] && [ $((now - s_time)) -lt $(($(jai_stamp_days) * 86400)) ] || return 1
	echo "trusted: $gate, the green run at ${s_commit:0:10} in $s_checkout ($s_when)"
}

jai_stamp_python() {
	[ -n "${JAI_STAMP_PYTHON:-}" ] || JAI_STAMP_PYTHON="$("$(jai_python 2>/dev/null)" --version 2>&1 | tr -d '\r')"
	echo "$JAI_STAMP_PYTHON"
}

# jai_stamp_build <gate> <debug|release>: the build a gate's stamp is keyed on, "-" for a gate that runs none of its binaries
jai_stamp_build() { case "$1" in headers | wasm | android | amalgamation | ci-scripts) echo - ;; *) echo "$2" ;; esac; }

# jai_gate_key <gate> <debug|release|->: the key of a gate's stamp, two lines. The build's part comes from its CMakeCache.txt
# (type, benchmarks, a digest of the compiler, flags and JAISCRIPT_ options; the CMake and compiler versions); a build that
# was never configured has no key, so its gates always run.
jai_gate_key() {
	local gate="$1" cfg="$2" dir cache config="" toolchain="" cmake_version cxx_version shells="" ps
	if [ "$cfg" != - ]; then
		dir="$(jai_dir_of "$cfg")"
		cache="$dir/CMakeCache.txt"
		[ -f "$cache" ] || return 1
		cmake_version="$(sed -n 's/^CMAKE_CACHE_\(MAJOR\|MINOR\|PATCH\)_VERSION:INTERNAL=//p' "$cache" | tr -d '\r' | paste -sd. -)"
		cxx_version="$(sed -n 's/^set(CMAKE_CXX_COMPILER_\(ID\|VERSION\) "\(.*\)")/\2/p' "$dir/CMakeFiles/$cmake_version/CMakeCXXCompiler.cmake" 2>/dev/null | tr -d '\r' | paste -sd' ' -)"
		[ -n "$cmake_version" ] && [ -n "$cxx_version" ] || return 1
		config="build=$cfg type=$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$cache" | tr -d '\r') benchmarks=$(sed -n 's/^JAISCRIPT_ENABLE_BENCHMARKS:[A-Z]*=//p' "$cache" | tr -d '\r')"
		config="$config options=$(grep -E '^(CMAKE_BUILD_TYPE|CMAKE_(C|CXX)_COMPILER|CMAKE_(C|CXX)_FLAGS(_[A-Z]+)?|CMAKE_(EXE|SHARED|STATIC|MODULE)_LINKER_FLAGS(_[A-Z]+)?|CMAKE_MSVC_RUNTIME_LIBRARY|CMAKE_GENERATOR|JAISCRIPT_[A-Z0-9_]+):' "$cache" | tr -d '\r' | LC_ALL=C sort | jai_digest)"
		# cl and link also read their flags from these
		config="$config$(env | grep -E '^(CL|_CL_|LINK|_LINK_)=' | LC_ALL=C sort | sed 's/^/ env:/' | tr -d '\n')"
		toolchain="cmake=$cmake_version cxx=$cxx_version"
	fi
	case "$gate" in
		gloom | gloom-workers | gloom-long | demoreel | crawler | gatorquest | timedemo | embed-quickstart) config="$config hashes=${HASH_BACKENDS:-vm interp}" ;;
		fuzz | fuzz-vm-reference | fuzz-order) config="$config iterations=${FUZZ_ITERATIONS:-300}" ;;
		ci-scripts)
			for ps in pwsh powershell; do command -v "$ps" >/dev/null 2>&1 && shells="$shells,$ps"; done
			toolchain="$toolchain bash=$BASH_VERSION shells=sh$shells" ;;
		headers | wasm) toolchain="$toolchain $(bash "$JAI_ROOT/tools/ci/gates.sh" --toolchain "$gate")" ;;
	esac
	config="${config# }"
	toolchain="${toolchain# }"
	printf 'config gate=%s%s\ntoolchain python=%s%s\n' "$gate" "${config:+ $config}" "$(jai_stamp_python)" "${toolchain:+ ${toolchain% }}"
}

# jai_gate_stamp <gate> <debug|release>: jai_stamp_write for a gate of this package's build
jai_gate_stamp() {
	[ -n "$JAI_STAMP_TREE" ] || return 0
	local key
	key="$(jai_gate_key "$1" "$(jai_stamp_build "$1" "$2")")" || return 0
	jai_stamp_write "$1" "$key"
}

# jai_gate_trusted <tree> <gate> <debug|release>: jai_stamp_trusted for a gate of this package's build
jai_gate_trusted() {
	local key
	key="$(jai_gate_key "$2" "$(jai_stamp_build "$2" "$3")")" || return 1
	jai_stamp_trusted "$1" "$2" "$key"
}
