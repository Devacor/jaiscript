#!/usr/bin/env bash
# Gator Quest launcher: the runner in the build tree (build/bin), else one beside this file (the shipped demo).
here="$(cd "$(dirname "$0")" && pwd)"
for cand in "$here/../../build/bin/jaiscript" "$here/../../build/bin/jaiscript.exe" "$here/jaiscript" "$here/jaiscript.exe"; do
	if [ -x "$cand" ]; then exec "$cand" --no-pause "$here/scripts/gatorquest.jai" -- "$@"; fi
done
echo "jaiscript runner not found. Build it from the JaiScript source root:" >&2
echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release --target jaiscript_runner" >&2
exit 1
