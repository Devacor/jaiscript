#!/usr/bin/env bash
# JaiDOOM launcher (Linux, macOS, Git Bash): uses a jaiscript next to this file, then one on
# PATH, then the repository's build/bin (the game finds wads/ from its own
# location, so the working directory does not matter). Arguments pass through: ./play.sh -- freedoom2 MAP01
cd "$(dirname "$0")/scripts" || exit 1
for cand in "${JAI_RUNNER:-}" ../jaiscript ../jaiscript.exe "$(command -v jaiscript 2>/dev/null)" \
            ../../../build/bin/jaiscript.exe ../../../build/bin/jaiscript; do
	if [ -n "$cand" ] && [ -x "$cand" ]; then exec "$cand" jaidoom.jai "$@"; fi
done
echo "jaiscript runner not found: set JAI_RUNNER, put jaiscript next to play.sh, or build it (docs/BUILD.md)" >&2
exit 1
