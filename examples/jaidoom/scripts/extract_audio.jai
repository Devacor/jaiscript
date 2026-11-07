// JaiDOOM audio extractor: every DS* sound lump to .wav and D_* music lump to .mid under the
// data folder's cache/<wad>/ (boot.jai does this as part of the game's own bootstrap; this is
// the same step on its own).   jaiscript extract_audio.jai -- doom1 freedoom1 [path/to/other.wad]

import "boot.jai";

function main() -> int {
	if (ARGS.size() == 0) { print("usage: jaiscript extract_audio.jai -- <wad> [<wad> ...]"); return 2; }
	int total = 0;
	for (auto p : ARGS) {
		int n = audio_extract_wad(wad_path(p));
		if (n < 0) { print(`   cannot read ${p}`); continue; }
		print(`   ${WAD_BASE}: ${n} sounds and songs -> ${audio_cache_dir()}`);
		total += n;
	}
	return total > 0 ? 0 : 1;
}
