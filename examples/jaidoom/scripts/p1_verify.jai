// P1 gate: WAD loader proves itself against doom1.wad's known structure.
// Run: jaiscript p1_verify.jai   (wads/ sits next to scripts/)

import "wad.jai";
import "vid.jai";
import "sound.jai";
import "mus.jai";

var FAILS = 0;

function check(bool ok, string what) -> void {
	if (ok) { print(`  ok    ${what}`); }
	else { FAILS = FAILS + 1; print(`  FAIL  ${what}`); }
}

function main() -> int {
	var path = wad_path("doom1");
	print(`loading ${path}`);
	check(wad_load(path), "wad_load succeeds");
	print(`  id=${WAD_ID} lumps=${NUM_LUMPS} bytes=${WAD.size()}`);
	check(WAD_ID == "IWAD", "IWAD header");
	check(NUM_LUMPS > 1000 && NUM_LUMPS < 3000, "plausible lump count");

	// language byte-contract pins (docs/site/guide/04-operators.html: promotion, to_char, \xNN)
	check(("A"[0] + 0) == 65 && "I"[0] == 73 && " "[0] == 32, "char promotion in arithmetic AND comparison");
	check(to_char(73) == 'I' && ("" + to_char(0)).size() == 1, "to_char minting round-trip (incl. NUL)");
	check("\xC8"[0] == 200 && ("\xFF"[0] + 0) == 255, "\\xNN literals; high bytes unsigned both ways");
	check("\xFF"[0] > "a"[0], "byte ordering is unsigned");
	var nest = {"a": {"b": 1, "c": 2}};
	int nsum = 0;
	for (var kv : nest["a"]) { nsum = nsum + kv.second; }
	var jrows = from_json("{\"r\": [[1, 5], [2, 9]]}");
	for (var row : jrows["r"]) { nsum = nsum + row[1]; }
	check(nsum == 17, `range-for over nested container reads (${nsum})`);

	int pp = lump_num("PLAYPAL");
	check(pp >= 0, "PLAYPAL exists");
	check(LUMP_SIZE[pp] == 10752, `PLAYPAL is 14 palettes x 768 bytes (got ${LUMP_SIZE[pp]})`);
	int cm = lump_num("COLORMAP");
	check(cm >= 0 && LUMP_SIZE[cm] == 8704, "COLORMAP is 34 maps x 256 bytes");

	// palette 0, color 0 should be near-black; color 4 is the doom white-ish
	array<int> pal = lump_bytes(pp);
	print(`  pal[0] rgb = ${pal[0]},${pal[1]},${pal[2]}   pal[4] rgb = ${pal[12]},${pal[13]},${pal[14]}`);
	check(pal.size() == 10752, "PLAYPAL fully decoded");

	// shareware episode 1 maps
	for (int m = 1; m <= 9; ++m) {
		check(lump_num(`E1M${m}`) >= 0, `E1M${m} marker present`);
	}
	check(lump_num("E2M1") == -1, "no E2M1 (shareware)");

	// E1M1 sub-lumps with exact record granularity
	int e1m1 = lump_num("E1M1");
	int things = lump_num_from("THINGS", e1m1);
	int lines = lump_num_from("LINEDEFS", e1m1);
	int sides = lump_num_from("SIDEDEFS", e1m1);
	int verts = lump_num_from("VERTEXES", e1m1);
	int segs = lump_num_from("SEGS", e1m1);
	int ssecs = lump_num_from("SSECTORS", e1m1);
	int nodes = lump_num_from("NODES", e1m1);
	int sects = lump_num_from("SECTORS", e1m1);
	int reject = lump_num_from("REJECT", e1m1);
	int bmap = lump_num_from("BLOCKMAP", e1m1);
	check(things > e1m1 && lines > e1m1 && sides > e1m1 && verts > e1m1, "E1M1 geometry lumps found");
	check(segs > e1m1 && ssecs > e1m1 && nodes > e1m1 && sects > e1m1 && reject > e1m1 && bmap > e1m1, "E1M1 BSP lumps found");
	check(LUMP_SIZE[things] % 10 == 0, `THINGS % 10 (${LUMP_SIZE[things] / 10} things)`);
	check(LUMP_SIZE[lines] % 14 == 0, `LINEDEFS % 14 (${LUMP_SIZE[lines] / 14} linedefs)`);
	check(LUMP_SIZE[sides] % 30 == 0, `SIDEDEFS % 30 (${LUMP_SIZE[sides] / 30} sidedefs)`);
	check(LUMP_SIZE[verts] % 4 == 0, `VERTEXES % 4 (${LUMP_SIZE[verts] / 4} vertexes)`);
	check(LUMP_SIZE[segs] % 12 == 0, `SEGS % 12 (${LUMP_SIZE[segs] / 12} segs)`);
	check(LUMP_SIZE[ssecs] % 4 == 0, `SSECTORS % 4 (${LUMP_SIZE[ssecs] / 4} subsectors)`);
	check(LUMP_SIZE[nodes] % 28 == 0, `NODES % 28 (${LUMP_SIZE[nodes] / 28} nodes)`);
	check(LUMP_SIZE[sects] % 26 == 0, `SECTORS % 26 (${LUMP_SIZE[sects] / 26} sectors)`);

	// first THINGS record: x,y are signed i16 map coords; type of thing 1 = player 1 start
	var th = lump_reader(things).unpack("x", binary::int16, "y", binary::int16, "angle", binary::int16, "type", binary::int16, "flags", binary::int16);
	print(`  first thing: x=${th["x"]} y=${th["y"]} angle=${th["angle"]} type=${th["type"]} flags=${th["flags"]}`);

	// DS* sound lumps decode to valid WAVs in pure script (playback is host-side)
	var wav = snd_wav_bytes("DSPISTOL");
	check(wav.size() > 44 && wav.substr(0, 4) == "RIFF" && wav.substr(8, 8) == "WAVEfmt ", "DSPISTOL decodes to RIFF/WAVE");
	int wrate = wav.reader().seek(24).uint32();
	check(wrate == 11025 || wrate == 22050, `sample rate sane (${wrate})`);
	int dlen = wav.reader().seek(40).uint32();
	check(wav.size() == 44 + dlen, "data chunk length matches file size");
	check(snd_wav_bytes("DSNOSUCH") == "", "missing sound lump decodes to nothing");

	// positional-audio variants: stereo header shape, interleave, attenuation, pan
	var st0 = snd_wav_stereo("DSPISTOL", 16, 16, 16);
	check(st0.substr(0, 4) == "RIFF" && st0.substr(8, 8) == "WAVEfmt ", "stereo variant is RIFF/WAVE");
	var fmt = st0.reader().seek(22).unpack("channels", binary::uint16, "rate", binary::uint32, "byterate", binary::uint32, "align", binary::uint16);
	check(fmt["channels"] == 2, "stereo: nchannels == 2");
	check(fmt["align"] == 2, "stereo: blockalign == 2");
	int srate = fmt["rate"];
	int sbyte = fmt["byterate"];
	check(srate == wrate && sbyte == srate * 2, "stereo: byterate is rate x 2");
	int sdl = st0.reader().seek(40).uint32();
	check(sdl == dlen * 2 && st0.size() == 44 + sdl, `stereo data is 2x the mono samples (${sdl})`);
	var st1 = snd_wav_stereo("DSPISTOL", 8, 8, 16);
	int amp0 = 0;
	for (int a0 = 44; a0 < st0.size(); ++a0) {
		int v0 = (st0[a0] + 0) - 128;
		if (v0 < 0) { v0 = -v0; }
		if (v0 > amp0) { amp0 = v0; }
	}
	int amp1 = 0;
	for (int a1 = 44; a1 < st1.size(); ++a1) {
		int v1 = (st1[a1] + 0) - 128;
		if (v1 < 0) { v1 = -v1; }
		if (v1 > amp1) { amp1 = v1; }
	}
	check(amp1 < amp0, `tier-1 gain is quieter than tier-0 (max amp ${amp0} -> ${amp1})`);
	var stl = snd_wav_stereo("DSPISTOL", 16, 8, 16);
	int ampl = 0;
	int ampr = 0;
	for (int a2 = 44; a2 < stl.size(); a2 = a2 + 2) {
		int vl = (stl[a2] + 0) - 128;
		if (vl < 0) { vl = -vl; }
		if (vl > ampl) { ampl = vl; }
		int vr = (stl[a2 + 1] + 0) - 128;
		if (vr < 0) { vr = -vr; }
		if (vr > ampr) { ampr = vr; }
	}
	check(ampr < ampl, `left pan: right ear quieter (L ${ampl} vs R ${ampr})`);
	check(snd_pos_pick(100.0, 0.0) && SPA_TIER == 0 && SPA_PAN == 0, "pick: close ahead = tier 0 center");
	check(snd_pos_pick(400.0, 0.7) && SPA_TIER == 1 && SPA_PAN == 1, "pick: mid left = tier 1 pan L");
	check(snd_pos_pick(800.0, -0.7) && SPA_TIER == 2 && SPA_PAN == 2, "pick: far right = tier 2 pan R");
	check(snd_pos_pick(319.0, 0.4) && SPA_TIER == 0 && SPA_PAN == 0, "pick: tier/pan thresholds hold inside");
	check(!snd_pos_pick(1200.0, 0.0), "pick: beyond 1200 is silent");
	check(lump_num("DSOOF") >= 0, "DSOOF landing thud exists");

	// D_* music lumps transcode to structurally valid MIDI in pure script
	var mid = mus_midi_bytes("D_E1M1");
	check(mid.size() > 200 && mid.substr(0, 4) == "MThd" && mid.substr(14, 4) == "MTrk", "D_E1M1 transcodes to MIDI");
	int tlen = mid.reader().big_endian().seek(18).uint32();
	check(mid.size() == 22 + tlen, `MIDI track length consistent (${tlen} bytes)`);
	check(mid[mid.size() - 3] == 255 && mid[mid.size() - 2] == 47 && mid[mid.size() - 1] == 0, "end-of-track marker present");
	check(mus_midi_bytes("D_NOSUCH") == "", "missing music lump transcodes to nothing");

	// texture + sprite infrastructure lumps
	check(lump_num("TEXTURE1") >= 0, "TEXTURE1 exists");
	check(lump_num("PNAMES") >= 0, "PNAMES exists");
	check(lump_num("TITLEPIC") >= 0, "TITLEPIC exists");
	int pn = lump_num("PNAMES");
	print(`  PNAMES count = ${lump_reader(pn).int32()}`);
	int t1 = lump_num("TEXTURE1");
	print(`  TEXTURE1 count = ${lump_reader(t1).int32()}`);

	print("");
	if (FAILS == 0) { print("P1 GATE: ALL GREEN"); }
	else { print(`P1 GATE: ${FAILS} FAILURES`); }
	return FAILS;
}

main()
