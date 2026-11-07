#!/bin/sh
# JaiScript installer for macOS and Linux: clones JaiScript, builds the jaiscript runner in Release with CMake and
# puts it on your PATH. Running it again updates the clone and rebuilds.
#
#   curl -fsSL https://jaiscript.com/install.sh | sh
#   curl -fsSL https://jaiscript.com/install.sh | sh -s -- --no-modify-path
#
# Options, each with its environment variable:
#   --dir <folder>       JAISCRIPT_HOME              the install folder (default ~/.jaiscript): src/, build/ and bin/
#   --ref <tag|branch>   JAISCRIPT_REF               what to build (default: the newest release tag, else the default branch)
#   --repo <url>         JAISCRIPT_REPO              the repository to clone
#   --generator <name>   CMAKE_GENERATOR             a CMake generator other than Makefiles: Ninja, Xcode
#   --cxx <compiler>     CXX                         a C++ compiler other than the system's c++: clang++, g++-14
#   --no-modify-path     JAISCRIPT_NO_MODIFY_PATH=1  leave your shell profile alone
#
# It needs git, CMake 3.25 or newer and a C++20 compiler. It never uses sudo and writes only to the install folder,
# plus one line in your shell profile that puts the install folder's bin/ on your PATH.
set -eu

say() { printf 'jaiscript-install: %s\n' "$*"; }
die() {
	printf 'jaiscript-install: error: %s\n' "$1" >&2
	shift
	for line in "$@"; do printf '  %s\n' "$line" >&2; done
	exit 1
}

need_value() { [ "$#" -ge 2 ] && [ -n "$2" ] || die "$1 needs a value"; }

profile_line() {
	case "$1" in
		*fish) printf 'set -gx PATH "%s" $PATH  # added by the JaiScript installer\n' "$2" ;;
		*) printf 'export PATH="%s:$PATH"  # added by the JaiScript installer\n' "$2" ;;
	esac
}

main() {
	root="${JAISCRIPT_HOME:-$HOME/.jaiscript}"
	ref="${JAISCRIPT_REF:-}"
	repo="${JAISCRIPT_REPO:-https://github.com/Devacor/jaiscript.git}"
	generator="${CMAKE_GENERATOR:-}"
	cxx="${CXX:-}"
	modify_path=1
	[ "${JAISCRIPT_NO_MODIFY_PATH:-0}" = 0 ] || modify_path=0
	while [ "$#" -gt 0 ]; do
		case "$1" in
			--dir) need_value "$@"; root="$2"; shift ;;
			--ref) need_value "$@"; ref="$2"; shift ;;
			--repo) need_value "$@"; repo="$2"; shift ;;
			--generator) need_value "$@"; generator="$2"; shift ;;
			--cxx) need_value "$@"; cxx="$2"; shift ;;
			--no-modify-path) modify_path=0 ;;
			-h | --help) say "options: --dir <folder> --ref <tag|branch> --repo <url> --generator <name> --cxx <compiler> --no-modify-path"; exit 0 ;;
			*) die "unknown option $1" "options: --dir <folder> --ref <tag|branch> --repo <url> --generator <name> --cxx <compiler> --no-modify-path" ;;
		esac
		shift
	done
	case "$root" in
		/*) ;;
		*) root="$(pwd)/$root" ;;
	esac

	command -v git >/dev/null 2>&1 || die "git is not installed." \
		"macOS: xcode-select --install" "Debian/Ubuntu: sudo apt install git" "Fedora: sudo dnf install git"

	command -v cmake >/dev/null 2>&1 || die "CMake is not installed (3.25 or newer is needed)." \
		"macOS: brew install cmake, or https://cmake.org/download/" "Debian/Ubuntu 24.04: sudo apt install cmake" \
		"any system with Python: python3 -m pip install --user cmake"
	cmake_version="$(cmake --version | sed -n '1s/[^0-9]*\([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')"
	cmake_major="${cmake_version%%.*}"
	cmake_minor="${cmake_version#*.}"
	if [ -z "$cmake_version" ] || [ "$cmake_major" -lt 3 ] || { [ "$cmake_major" -eq 3 ] && [ "$cmake_minor" -lt 25 ]; }; then
		die "CMake ${cmake_version:-of unknown version} is too old: 3.25 or newer is needed." \
			"macOS: brew upgrade cmake" "any system with Python: python3 -m pip install --user --upgrade cmake" \
			"or https://cmake.org/download/"
	fi

	case "$generator" in
		Xcode) command -v xcodebuild >/dev/null 2>&1 || die "the Xcode generator needs Xcode (xcodebuild)." ;;
		Ninja*) command -v ninja >/dev/null 2>&1 || die "the Ninja generator needs ninja." \
			"macOS: brew install ninja" "Debian/Ubuntu: sudo apt install ninja-build" ;;
		"" | *Makefiles) command -v make >/dev/null 2>&1 || die "make is not installed." \
			"macOS: xcode-select --install" "Debian/Ubuntu: sudo apt install build-essential" ;;
	esac

	check_cxx="$cxx"
	if [ -z "$check_cxx" ]; then
		for candidate in c++ g++ clang++; do
			if command -v "$candidate" >/dev/null 2>&1; then check_cxx="$candidate"; break; fi
		done
	fi
	[ -n "$check_cxx" ] || die "no C++ compiler was found (C++20 is needed: GCC 11, Clang 14 or Xcode 15 or newer)." \
		"macOS: xcode-select --install" "Debian/Ubuntu: sudo apt install build-essential" "Fedora: sudo dnf install gcc-c++"
	mkdir -p "$root"
	check_dir="$root/compiler-check"
	mkdir -p "$check_dir"
	cat > "$check_dir/check.cpp" <<'EOF'
#include <charconv>
#include <compare>
int main() {
	char text[32];
	auto converted = std::to_chars(text, text + sizeof text, 0.5);
	return (1 <=> 2) < 0 && converted.ec == std::errc{} ? 0 : 1;
}
EOF
	if ! "$check_cxx" -std=c++20 "$check_dir/check.cpp" -o "$check_dir/check" > "$check_dir/check.log" 2>&1 \
		|| ! "$check_dir/check"; then
		cat "$check_dir/check.log" >&2
		die "$check_cxx cannot build C++20 (GCC 11, Clang 14 or Xcode 15 or newer is needed)." \
			"pick another compiler with --cxx, e.g. --cxx g++-14 or --cxx clang++"
	fi
	rm -rf "$check_dir"

	src="$root/src"
	if [ -z "$ref" ]; then
		tags="$(git ls-remote --tags --refs --sort=-v:refname "$repo" 'refs/tags/v*' </dev/null)" || die "cannot reach $repo"
		ref="$(printf '%s\n' "$tags" | sed -n 's#.*refs/tags/\(v[0-9].*\)#\1#p' | head -n 1)"
	fi
	if [ -d "$src/.git" ]; then
		say "updating $src to ${ref:-the default branch}"
		git -C "$src" remote set-url origin "$repo"
		git -C "$src" fetch --quiet --depth 1 origin "${ref:-HEAD}" </dev/null
		git -C "$src" checkout --quiet --detach FETCH_HEAD
	elif [ -e "$src" ]; then
		die "$src exists and is not a git clone." "remove it, or install elsewhere with --dir <folder>"
	else
		say "cloning $repo${ref:+ at $ref} into $src"
		if [ -n "$ref" ]; then
			git clone --quiet --depth 1 --branch "$ref" "$repo" "$src" </dev/null
		else
			git clone --quiet --depth 1 "$repo" "$src" </dev/null
		fi
	fi

	say "building the runner in $root/build"
	cmake "-DJAI_BUILD_DIR=$root/build" "-DJAI_BIN_DIR=$root/bin" -DJAI_RUNNER_ONLY=ON -DJAI_REPLACE_TREE=ON \
		"-DJAI_GENERATOR=$generator" "-DJAI_CXX=$cxx" -P "$src/tools/install/build_runner.cmake" </dev/null
	bin="$root/bin"
	"$bin/jaiscript" --version

	if [ "$modify_path" = 1 ]; then
		shell_name="${SHELL:-sh}"
		shell_name="${shell_name##*/}"
		case "$shell_name" in
			zsh) profiles="${ZDOTDIR:-$HOME}/.zshrc" ;;
			bash)
				profiles="$HOME/.bashrc"
				[ "$(uname -s)" != Darwin ] || profiles="$profiles $HOME/.bash_profile"
				;;
			fish) profiles="${XDG_CONFIG_HOME:-$HOME/.config}/fish/config.fish" ;;
			*) profiles="$HOME/.profile" ;;
		esac
		line="$(profile_line "$shell_name" "$bin")"
		for profile in $profiles; do
			if [ -f "$profile" ] && grep -Fqx "$line" "$profile"; then
				say "$profile already puts $bin on your PATH"
				continue
			fi
			mkdir -p "$(dirname "$profile")"
			if [ -s "$profile" ] && [ -n "$(tail -c 1 "$profile")" ]; then
				printf '\n' >> "$profile"
			fi
			printf '%s\n' "$line" >> "$profile"
			say "added this line to $profile:"
			printf '  %s\n' "$line"
		done
		say "open a new terminal, then run: jaiscript --version"
	else
		say "your PATH was left alone; to run jaiscript by name, add this line to your shell profile:"
		printf '  %s\n' "$(profile_line "${SHELL:-sh}" "$bin")"
	fi
	say "play Gator Quest: jaiscript \"$src/examples/gatorquest/scripts/gatorquest.jai\""
}

main "$@"
