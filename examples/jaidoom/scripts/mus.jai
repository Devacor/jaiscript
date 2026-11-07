// JaiDOOM mus.jai — music from the WAD's own D_* lumps, zero audio libraries.
// MUS is a compact MIDI dialect: same note/controller model, 7-bit varlen
// delays at 140Hz. Transcoding is pure byte work (MIDI division 70 at the
// default 120bpm = 140 ticks/sec, so delays copy through unchanged); the
// runner's Sound plays the track, looped. Tracks convert lazily to cached
// .mid files.

import "wad.jai";
import "sound.jai";

function midi_varlen(int v) -> string {
	if (v < 0) { v = 0; }
	var out = "" + to_char(v & 127);
	v = v >> 7;
	while (v > 0) {
		out = to_char(0x80 | (v & 127)) + out;
		v = v >> 7;
	}
	return out;
}

// MUS controller number -> MIDI CC (controller 0 is a program change instead)
function mus_cc(int cn) -> int {
	if (cn == 1) { return 0; }
	if (cn == 2) { return 1; }
	if (cn == 3) { return 7; }
	if (cn == 4) { return 10; }
	if (cn == 5) { return 11; }
	if (cn == 6) { return 91; }
	if (cn == 7) { return 93; }
	if (cn == 8) { return 64; }
	if (cn == 9) { return 67; }
	return 7;
}

function mus_midi_bytes(string name) -> string {
	int lu = lump_num(name);
	if (lu < 0) { return ""; }
	int pos = LUMP_POS[lu];
	if (LUMP_SIZE[lu] < 16 || WAD.substr(pos, 3) != "MUS" || WAD[pos + 3] != 0x1A) { return ""; }
	binary::reader r = lump_reader(lu);
	r.skip(4);
	int slen = r.uint16();
	r.seek(pos + r.uint16());
	int stop = r.pos() + slen;
	int lend = pos + LUMP_SIZE[lu];
	if (stop > lend) { stop = lend; }

	var trk = "";
	array<int> chvol = [];
	for (int c = 0; c < 16; ++c) { chvol.push(100); }
	int delay = 0;
	bool done = false;
	while (r.pos() < stop && !done) {
		int ev = r.uint8();
		int last = ev & 0x80;
		int etype = (ev >> 4) & 7;
		int mch = ev & 15;
		int ch = mch == 15 ? 9 : (mch >= 9 ? mch + 1 : mch);   // MUS 15 = percussion = MIDI 9
		var out = "";
		if (etype == 0) {
			int note = r.uint8() & 127;
			out = "" + to_char(0x80 | ch) + to_char(note) + to_char(64);
		} else if (etype == 1) {
			int nb = r.uint8();
			if ((nb & 0x80) != 0) { chvol[ch] = r.uint8() & 127; }
			out = "" + to_char(0x90 | ch) + to_char(nb & 127) + to_char(chvol[ch]);
		} else if (etype == 2) {
			int w = r.uint8() * 64;
			out = "" + to_char(0xE0 | ch) + to_char(w & 127) + to_char((w >> 7) & 127);
		} else if (etype == 3) {
			int sy = r.uint8() & 127;
			int cc = 120;
			if (sy == 11) { cc = 123; }
			else if (sy == 12) { cc = 126; }
			else if (sy == 13) { cc = 127; }
			else if (sy == 14) { cc = 121; }
			out = "" + to_char(0xB0 | ch) + to_char(cc) + to_char(0);
		} else if (etype == 4) {
			int cn = r.uint8() & 127;
			int cv = r.uint8() & 127;
			if (cn == 0) { out = "" + to_char(0xC0 | ch) + to_char(cv); }
			else { out = "" + to_char(0xB0 | ch) + to_char(mus_cc(cn)) + to_char(cv); }
		} else if (etype == 6) { done = true; }
		else if (etype == 7) { r.skip(1); }
		if (out != "") {
			trk += midi_varlen(delay) + out;
			delay = 0;
		}
		if (last != 0) {
			int d = 0;
			while (r.pos() < stop) {
				int b = r.uint8();
				d = (d << 7) | (b & 127);
				if ((b & 0x80) == 0) { break; }
			}
			delay = delay + d;
		}
	}
	trk += midi_varlen(delay) + to_char(0xFF) + to_char(0x2F) + to_char(0);
	binary::writer mid;
	mid.big_endian().bytes("MThd").uint32(6).uint16(0).uint16(1).uint16(70).bytes("MTrk").uint32(trk.size()).bytes(trk);
	return mid.to_string();
}

// ------------------------------------------------------------------ player ---

// doom2 track table (doom2 has no D_MAPxx lumps; 11+ reuse the ten cyclically);
// episodic maps keep the D_ExMy convention
var MAPMUS = ["D_RUNNIN", "D_STALKS", "D_COUNTD", "D_BETWEE", "D_DOOM",
	"D_THE_DA", "D_SHAWN", "D_DDTBLU", "D_IN_CIT", "D_DEAD"];

function map_music(string mapname) -> string {
	if (!mapname.starts_with("MAP")) { return "D_" + mapname; }
	int n = mapname.substr(3, 2).to_int(1);
	return MAPMUS[(n - 1) % 10];
}

var MUSIC_CUR = "";
var MUSIC_READY = {};         // track name -> its Sound (null = missing/failed)
var MUSIC_SOUND = null;       // the track playing, looped

function music_want(string name) -> void {
	if (!SND_ON || MUSIC_CUR == name) { return; }
	if (!MUSIC_READY.has(name)) {
		var path = audio_cache_dir() + string::to_lower(name) + ".mid";
		if (!file_exists(path)) {                    // bootstrap (or an earlier run) already transcoded it
			var mid = mus_midi_bytes(name);
			if (mid == "" || !write_file(path, mid)) { path = ""; }
		}
		MUSIC_READY[name] = path != "" ? load_sound(path) : null;
	}
	MUSIC_CUR = name;                                // even when missing: stop retrying per frame
	if (MUSIC_SOUND != null) { MUSIC_SOUND.stop(); }
	MUSIC_SOUND = MUSIC_READY[name];
	if (MUSIC_SOUND != null) { MUSIC_SOUND.play(true); }
}

function music_stop() -> void {
	MUSIC_CUR = "";
	if (MUSIC_SOUND != null) { MUSIC_SOUND.stop(); }
	MUSIC_SOUND = null;
}
