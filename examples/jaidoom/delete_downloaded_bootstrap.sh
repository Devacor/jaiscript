#!/usr/bin/env bash
# Removes everything bootstrap.sh fetched or generated: the WADs, the download folder, the
# audio cache and any stray .wav/.mid the game wrote next to its scripts, plus the per-user
# data folder the game uses when this folder is read-only. Keeps COPYING.txt.
set -u
cd "$(dirname "$0")"
echo "JaiDOOM: removing downloaded game data and generated caches"
removed=0
gone() { echo "   removed  $1"; removed=$((removed + 1)); }
for w in wads/doom1.wad wads/freedoom1.wad wads/freedoom2.wad wads/SOURCES.txt; do
	[ -e "$w" ] && rm -f "$w" && gone "$w"
done
[ -d wads/.download ] && rm -rf wads/.download && gone "wads/.download/"
[ -d cache ] && rm -rf cache && gone "cache/"
for d in "${XDG_DATA_HOME:-$HOME/.local/share}/JaiDOOM" "$HOME/Library/Application Support/JaiDOOM"; do
	[ -d "$d" ] && rm -rf "$d" && gone "$d/"
done
for f in ./*.wav ./*.mid scripts/*.wav scripts/*.mid; do
	[ -e "$f" ] && rm -f "$f" && gone "$f"
done
[ "$removed" = 0 ] && echo "   nothing to remove" || echo "   done ($removed items). Run ./bootstrap.sh to fetch everything again."
