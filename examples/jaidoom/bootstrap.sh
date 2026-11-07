#!/usr/bin/env bash
# JaiDOOM bootstrap, headless: the game fetches Freedoom and the DOOM shareware episode itself
# (boot.jai), verifies them by SHA-256 and decodes the audio. Same as the first-run screen.
#   ./bootstrap.sh [--no-shareware] [--no-audio]
# Needs a jaiscript runner: JAI_RUNNER, one next to this file, one on PATH, or the repo build.
cd "$(dirname "$0")/scripts" || exit 1
for cand in "${JAI_RUNNER:-}" ../jaiscript ../jaiscript.exe "$(command -v jaiscript 2>/dev/null)" \
            ../../../build/bin/jaiscript.exe ../../../build/bin/jaiscript; do
	if [ -n "$cand" ] && [ -x "$cand" ]; then exec "$cand" --no-pause jaidoom.jai -- bootstrap "$@"; fi
done
echo "jaiscript runner not found: set JAI_RUNNER, put jaiscript next to this script, or build it (docs/BUILD.md)" >&2
exit 1
