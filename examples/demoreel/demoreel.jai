// demoreel.jai — drag-drop shim: runs the demoreel on the standalone jaiscript
// runner with zero C++ beyond the runner itself. Recreates the C++ host contract
// (demoreel_host.cpp) in script: the ESC global, the itrunc/ifloor casts, host_log
// and the seeded xorshift64* Rng class, bit-exact under checked int64 (16-bit-limb
// multiplies, masked logical shifts). Then it imports the scene files in the host's
// load order and pumps demo_frame at 30 fps.
//
// Not ported from the C++ host: its escape filter and row-diff redraw (frames
// present whole; the scripts' own run-length escapes still apply), TAB's backend
// swap (one engine here) and 'r' hot reload.
//
//   jaiscript demoreel.jai                        the reel (q or esc quits)
//   jaiscript demoreel.jai -- color 256           the reel quantized to the xterm-256 cube (color 24: 24-bit)
//   jaiscript demoreel.jai -- ascii 1             one density character per cell, no block glyphs
//   jaiscript demoreel.jai -- frames 60           headless self-test: N smoke frames (100x40,
//                                                 seed 20260705, fixed dt 1/30), sizes, a stream
//                                                 hash and timing
//   jaiscript demoreel.jai -- frames 60 dump P    also write frames 0/N-2/N-1 to P_<i>.txt
//                                                 (+ P_<i>.bmp, the pixel grid) for byte diffing
//   jaiscript demoreel.jai -- bench 60            per-scene ms/frame + bytes/frame
//   jaiscript demoreel.jai -- shot I at T dump P  scene I from scene time T-0.2, frame n
//                                                 (default 6) -> P.txt (as presented, so color
//                                                 and ascii apply) + P.bmp
//   jaiscript demoreel.jai -- midi F.mid          write the soundtrack and exit
//   other args: seed N | fps N | w N | h N | scene I (jump after boot) | music 0 (silent)
//   keys: h HUD | f freeze | n/p/space/arrows scene skip | 1-9,0 jump | q/esc quit

// ---------------------------------------------------------------- host API ---

string ESC = to_string(to_char(27));
var RUNNER_BACKEND = "vm";   // HUD label only; the reel never branches on it

function itrunc(float v) -> int { return math::itrunc(v); }
function ifloor(float v) -> int { return math::ifloor(v); }
function host_log(string line) -> void { print(line); }

// xorshift64* — bit-exact port of the host's demo_rng under checked int64:
// shifts wrap mod 2^64 (masked after >> for the logical form), the 64x64 low
// multiply runs in 16-bit limbs so no intermediate exceeds int64.
function u64_mul(int a, int b) -> int {
	int a0 = a & 0xFFFF; int a1 = (a >> 16) & 0xFFFF; int a2 = (a >> 32) & 0xFFFF; int a3 = (a >> 48) & 0xFFFF;
	int b0 = b & 0xFFFF; int b1 = (b >> 16) & 0xFFFF; int b2 = (b >> 32) & 0xFFFF; int b3 = (b >> 48) & 0xFFFF;
	int r0 = a0 * b0;
	int r1 = a0 * b1 + a1 * b0 + (r0 >> 16);
	int r2 = a0 * b2 + a1 * b1 + a2 * b0 + (r1 >> 16);
	int r3 = a0 * b3 + a1 * b2 + a2 * b1 + a3 * b0 + (r2 >> 16);
	return (r0 & 0xFFFF) | ((r1 & 0xFFFF) << 16) | ((r2 & 0xFFFF) << 32) | ((r3 & 0xFFFF) << 48);
}

function u64_shr(int x, int k) -> int {
	return (x >> k) & ((1 << (64 - k)) - 1);
}

class Rng {
	int s = 0;
	Rng(int seed) {
		s = seed != 0 ? seed : -7046029254386353131;   // 0x9E3779B97F4A7C15
		for (int i = 0; i < 4; ++i) { raw(); }
	}
	int raw() {
		int v = s;
		v = v ^ u64_shr(v, 12);
		v = v ^ (v << 25);
		v = v ^ u64_shr(v, 27);
		s = v;
		return u64_mul(v, 2685821657736338717);        // 0x2545F4914F6CDD1D
	}
	int next(int n) {
		if (n <= 0) { return 0; }
		int x = raw();
		if (x >= 0) { return x % n; }
		int m = ((((4611686018427387904 % n) * 2) % n) * 2) % n;   // 2^64 mod n
		int res = (x % n + m) % n;
		return res < 0 ? res + n : res;
	}
	int roll(int lo, int hi) {
		if (hi <= lo) { return lo; }
		return lo + next(hi - lo + 1);
	}
	bool chance(int percent) { return next(100) < percent; }
	float nextf() {
		return u64_shr(raw(), 11) * (1.0 / 9007199254740992.0);
	}
}

// ------------------------------------------------------------------ scenes ---
// the jai adapter's kSceneFiles order: base classes before derived, main.jai last
// (it reads ESC at import time, so the host API above must already exist).

import "scenes/util.jai";
import "scenes/reel.jai";
import "scenes/plasma.jai";
import "scenes/starfield.jai";
import "scenes/tunnel.jai";
import "scenes/donut.jai";
import "scenes/julia.jai";
import "scenes/lava.jai";
import "scenes/pipes.jai";
import "scenes/fire.jai";
import "scenes/sand.jai";
import "scenes/boids.jai";
import "scenes/kinstein.jai";
import "scenes/voxelspace.jai";
import "scenes/marcher.jai";
import "scenes/cloth.jai";
import "scenes/chrome.jai";
import "scenes/skull.jai";
import "scenes/finale.jai";
import "scenes/main.jai";

// -------------------------------------------------------------------- args ---

var OPT_FRAMES = 0;          // 0 = interactive
var OPT_SEED = 20260705;
var OPT_FPS = 30.0;
var OPT_W = 0;               // 0 = autodetect
var OPT_H = 0;
var OPT_SCENE = -1;
var OPT_DUMP = "";
var OPT_BENCH = 0;
var OPT_SHOT = -1;
var OPT_AT = 4.0;
var OPT_SHOTN = 6;
var OPT_MUSIC = true;
var OPT_MIDI = "";
// the xterm-256 cube unless the terminal says it takes 24-bit color (COLORTERM, Windows Terminal); `color 24` / `color 256` decide
var OPT_CUBE = !(env_var("COLORTERM") == "truecolor" || env_var("COLORTERM") == "24bit" || env_var("WT_SESSION") != "");
var OPT_ASCII = false;

for (int i = 0; i + 1 < ARGS.size(); i += 2) {
	string k = ARGS[i];
	string v = ARGS[i + 1];
	if (k == "frames" || k == "turns") { OPT_FRAMES = v.to_int(); }
	else if (k == "seed") { OPT_SEED = v.to_int(); }
	else if (k == "fps") { OPT_FPS = v.to_int() * 1.0; }
	else if (k == "w") { OPT_W = v.to_int(); }
	else if (k == "h") { OPT_H = v.to_int(); }
	else if (k == "scene") { OPT_SCENE = v.to_int(); }
	else if (k == "dump") { OPT_DUMP = v; }
	else if (k == "bench") { OPT_BENCH = v.to_int(); }
	else if (k == "shot") { OPT_SHOT = v.to_int() - 1; }
	else if (k == "at") { OPT_AT = v.to_float(); }
	else if (k == "n") { OPT_SHOTN = v.to_int(); }
	else if (k == "music") { OPT_MUSIC = v != "0"; }
	else if (k == "midi") { OPT_MIDI = v; }
	else if (k == "color") { OPT_CUBE = v == "256"; }
	else if (k == "ascii") { OPT_ASCII = v != "0"; }
	else { print(`demoreel.jai: unknown arg '${k}' (frames/seed/fps/w/h/scene/dump/bench/shot/at/n/music/midi/color/ascii)`); }
}

// ---------------------------------------------------------------- headless ---
// Mirrors the host's run_smoke: 100x40, fixed dt 1/30, smoke=true, hud off,
// key "" and zeroed timing args every frame. The rolling hash makes runs
// comparable across runner backends (vm vs interp) without keeping frames.

function run_headless(int frames) -> int {
	int w = OPT_W > 0 ? OPT_W : 100;
	int h = OPT_H > 0 ? OPT_H : 40;
	float dt = 1.0 / 30.0;
	demo_boot(w, h, OPT_SEED, "vm", true, false);
	if (OPT_SCENE >= 0) { demo_jump(OPT_SCENE, 5.0); }
	print(format("demoreel shim headless: {0} frames @ {1}x{2}, seed {3}, dt {4}", frames, w, h, OPT_SEED, dt));
	int hash = 0;
	int total_bytes = 0;
	int min_len = 0; int max_len = 0;
	time::stopwatch run;
	time::stopwatch render;
	run.start();
	for (int f = 0; f < frames; ++f) {
		render.resume();
		string frame = demo_frame(f * dt, dt, "", 0.0, 0.0, 0.0, 0.0);
		render.pause();
		int len = frame.size();
		total_bytes += len;
		if (f == 0 || len < min_len) { min_len = len; }
		if (len > max_len) { max_len = len; }
		for (int i = 0; i < len; ++i) { hash = (hash * 31 + frame[i]) & 0xFFFFFFFF; }
		if (OPT_DUMP != "" && (f == 0 || f == frames - 2 || f == frames - 1)) {
			write_file(OPT_DUMP + "_" + to_string(f) + ".txt", frame);
			write_bmp(OPT_DUMP + "_" + to_string(f) + ".bmp");
		}
	}
	int elapsed = run.elapsed() * 1000;
	print(format("frames {0} | bytes {1} (len {2}..{3}) | stream hash {4} | {5} ms total, {6} ms/frame incl. hashing, {7} ms/frame render",
		frames, total_bytes, min_len, max_len, hash, elapsed, (elapsed * 1.0) / frames, render.elapsed() * 1000 / frames));
	return 0;
}

function le16(int v) -> string { return "" + to_char(v & 255) + to_char((v >> 8) & 255); }
function le32(int v) -> string {
	return "" + to_char(v & 255) + to_char((v >> 8) & 255) + to_char((v >> 16) & 255) + to_char((v >> 24) & 255);
}

// the presenter's pixel grid as a 24-bit BMP (what the half blocks show)
function write_bmp(string path) -> void {
	int bw = HB_W;
	int bh = HB_ROWS * 2;
	int pad = (4 - (bw * 3) % 4) % 4;
	int stride = bw * 3 + pad;
	string out = "BM" + le32(54 + stride * bh) + le32(0) + le32(54);
	out += le32(40) + le32(bw) + le32(bh) + le16(1) + le16(24) + le32(0) + le32(stride * bh);
	out += le32(2835) + le32(2835) + le32(0) + le32(0);
	string padding = "";
	for (int p = 0; p < pad; ++p) { padding += to_char(0); }
	for (int y = bh - 1; y >= 0; --y) {
		string row = "";
		for (int x = 0; x < bw; ++x) {
			int c = HB_RGB[HB_PIX[y * bw + x]];
			row += "" + to_char(c & 255) + to_char((c >> 8) & 255) + to_char((c >> 16) & 255);
		}
		out += row + padding;
	}
	write_file(path, out);
}

// every scene mid-flight (past its title card), no transitions: script cost
// of update + render + presentation per frame, and the bytes a frame emits
function run_bench(int frames) -> int {
	int w = OPT_W > 0 ? OPT_W : 100;
	int h = OPT_H > 0 ? OPT_H : 40;
	float dt = 1.0 / 30.0;
	demo_boot(w, h, OPT_SEED, "vm", false, false);
	int n = reel.nscenes;
	print(format("demoreel bench: {0} frames per scene @ {1}x{2} cells ({1}x{3} px)", frames, w, h, h * 2));
	float total = 0.0;
	time::stopwatch scene;
	for (int s = 0; s < n; ++s) {
		demo_boot(w, h, OPT_SEED, "vm", false, false);
		demo_jump(s, 3.0);
		int bytes = 0;
		scene.start();
		for (int f = 0; f < frames; ++f) {
			string fr = demo_frame(3.0 + f * dt, dt, "", 0.0, 0.0, 0.0, 0.0);
			bytes += fr.size();
		}
		float el = scene.elapsed() * 1000;
		total += el;
		print(format("{0} {1} | {2} ms/frame | {3} KB/frame", s + 1, reel.cur.name, el / frames, bytes / frames / 1024));
	}
	print(format("mean {0} ms/frame", total / (frames * n)));
	return 0;
}

function run_shot(int scene, float at) -> int {
	int w = OPT_W > 0 ? OPT_W : 100;
	int h = OPT_H > 0 ? OPT_H : 40;
	demo_boot(w, h, OPT_SEED, "vm", false, false);
	demo_jump(scene, at - 0.2);
	string fr = "";
	for (int f = 0; f < OPT_SHOTN; ++f) { fr = demo_frame(at, 1.0 / 30.0, "", 0.0, 0.0, 0.0, 0.0); }
	string p = OPT_DUMP != "" ? OPT_DUMP : "shot";
	var terminal = Terminal();
	terminal.cube = OPT_CUBE;
	terminal.ascii = OPT_ASCII;
	write_file(p + ".txt", terminal.present(fr));
	write_bmp(p + ".bmp");
	print(format("shot: scene {0} {1} at {2}s -> {3}.txt / {3}.bmp ({4} bytes)", scene + 1, reel.cur.name, at, p, fr.size()));
	return 0;
}

// -------------------------------------------------------------- soundtrack ---
// A Standard MIDI File composed and written at boot, played through the
// runner's MIDI sequencer: 64 bars at 124 bpm in A minor over i-VI-III-VII,
// sectioned intro / build / main / breakdown / finale. Bass pumps eighths,
// a warm pad holds the chords, a saw arpeggiates sixteenths, a square lead
// improvises on the pentatonic from the reel's seed, and a four-on-the-floor
// kit drives it. Events are emitted in time order (pending note-offs flushed
// before each step's note-ons), so the track needs no sort.

function vlq(int v) -> string {
	string out = "" + to_char(v & 127);
	int r = v >> 7;
	while (r > 0) {
		out = "" + to_char(128 | (r & 127)) + out;
		r = r >> 7;
	}
	return out;
}

function b3(int a, int b, int c) -> string { return "" + to_char(a) + to_char(b) + to_char(c); }
function be32(int v) -> string {
	return "" + to_char((v >> 24) & 255) + to_char((v >> 16) & 255) + to_char((v >> 8) & 255) + to_char(v & 255);
}

class MidiTrack {
	string trk = "";
	int last = 0;
	array<int> off_t = [];
	array<int> off_c = [];
	array<int> off_n = [];

	void ev(int tick, string bytes) {
		trk += vlq(tick - last) + bytes;
		last = tick;
	}

	void note(int tick, int ch, int n, int vel, int dur) {
		ev(tick, b3(144 | ch, n, vel));
		off_t.push(tick + dur);
		off_c.push(ch);
		off_n.push(n);
	}

	// note-offs due by `tick`, emitted in time order (a delta is never negative)
	void flush(int tick) {
		while (true) {
			int pick = -1;
			for (int i = 0; i < off_t.size(); ++i) {
				if (off_t[i] <= tick && (pick < 0 || off_t[i] < off_t[pick])) { pick = i; }
			}
			if (pick < 0) { return; }
			ev(off_t[pick], b3(128 | off_c[pick], off_n[pick], 0));
			int last_i = off_t.size() - 1;
			off_t[pick] = off_t[last_i];
			off_c[pick] = off_c[last_i];
			off_n[pick] = off_n[last_i];
			off_t.pop();
			off_c.pop();
			off_n.pop();
		}
	}
}

function compose_soundtrack(int seed) -> string {
	var m = MidiTrack();
	int q = 96;
	int s16 = q / 4;
	m.ev(0, "" + to_char(255) + to_char(81) + to_char(3) + to_char(7) + to_char(98) + to_char(31));   // 483871 us per quarter
	var programs = [38, 89, 81, 80];
	var volumes = [104, 78, 64, 96];
	for (int ch = 0; ch < 4; ++ch) {
		m.ev(0, "" + to_char(192 | ch) + to_char(programs[ch]));
		m.ev(0, b3(176 | ch, 7, volumes[ch]));
	}
	var chords = [[57, 60, 64], [53, 57, 60], [52, 55, 60], [55, 59, 62]];
	var roots = [45, 41, 48, 43];
	var penta = [69, 72, 74, 76, 79, 81, 84];
	int lcg = (seed & 65535) | 1;
	int mel = 3;
	for (int bar = 0; bar < 64; ++bar) {
		int ci = (bar / 2) % 4;
		var chord = chords[ci];
		int root = roots[ci];
		bool intro = bar < 8;
		bool build = bar >= 8 && bar < 16;
		bool brk = bar >= 40 && bar < 48;
		bool fin = bar >= 48;
		bool drums = !intro && !brk;
		for (int st = 0; st < 16; ++st) {
			int tick = (bar * 16 + st) * s16;
			m.flush(tick);
			if (st == 0 && (bar % 2 == 0)) {
				for (int k = 0; k < 3; ++k) { m.note(tick, 1, chord[k], intro ? 60 : 74, q * 8 - 4); }
			}
			if (st == 0 && (bar == 16 || bar == 48 || bar == 56)) { m.note(tick, 9, 49, 110, q); }
			if (!intro && st % 2 == 0) {
				int bn = st % 8 == 4 ? root + 12 : root;
				m.note(tick, 0, bn, st % 4 == 0 ? 110 : 84, s16 * 2 - 6);
			}
			if (drums && st % 4 == 0) { m.note(tick, 9, 36, 118, s16); }
			if (drums && !build && (st == 4 || st == 12)) { m.note(tick, 9, 38, 104, s16); }
			if (st % 2 == 0) { m.note(tick, 9, 42, st % 4 == 0 ? 70 : 52, s16); }
			if (!intro && !build && !brk && st % 4 == 2) { m.note(tick, 9, 46, 60, s16); }
			if ((bar >= 16 && !brk) && bar < 64) {
				var pat = [0, 1, 2, 3, 2, 1, 0, 3];
				int pi = pat[st % 8];
				int an = pi == 3 ? chord[0] + 12 : chord[pi];
				m.note(tick, 2, an + 12, 70 + (st % 4 == 0 ? 20 : 0), s16 - 2);
			}
			if ((brk || fin) && st % 2 == 0) {
				lcg = (lcg * 1103515245 + 12345) & 2147483647;
				int r = (lcg >> 16) & 15;
				if (r < 9 || st == 0) {
					mel = mel + (r % 3) - 1;
					mel = mel < 0 ? 1 : (mel > 6 ? 5 : mel);
					int len = r < 3 ? s16 * 4 : s16 * 2;
					m.note(tick, 3, penta[mel], 96, len - 4);
				}
			}
		}
	}
	m.flush(64 * 16 * s16 + q * 8);
	m.ev(m.last, "" + to_char(255) + to_char(47) + to_char(0));
	string head = "MThd" + be32(6) + "" + to_char(0) + to_char(0) + to_char(0) + to_char(1) + to_char(0) + to_char(q);
	return head + "MTrk" + be32(m.trk.size()) + m.trk;
}

// "" when the file can't be written (then the reel just runs silent)
function write_soundtrack() -> string {
	string dir = app_cache_dir("jai_demoreel");
	if (!make_dirs(dir)) { return ""; }
	string path = dir + "/soundtrack.mid";
	if (!write_file(path, compose_soundtrack(OPT_SEED))) { return ""; }
	return path;
}

// ---------------------------------------------------------------- terminal ---
// The color depth comes from the terminal as jai_demoreel reads it (OPT_CUBE): the
// cube quantizes every 24-bit escape to xterm-256 (macOS Terminal.app, the Linux
// console, legacy conhost), `ascii 1` draws one density character per cell for
// terminals without the block glyphs. Only the interactive reel presents through here; the headless modes
// hash the reel's own frames.

string CSI = ESC + "[";

function cube_axis(int v) -> int {
	if (v < 48) { return 0; }
	if (v < 115) { return 1; }
	return (v - 35) / 40;
}

class Terminal {
	bool cube = false;
	bool ascii = false;
	string ramp = " .:-=+*#%@";
	map<string, string> cube_params = {};   // "38;2;r;g;b" -> "38;5;n", one entry per color seen

	string present(string frame) {
		if (!ascii) { return cube ? quantize(frame) : frame; }
		var rows = frame.split("\n");
		for (int cy = 0; cy < rows.size(); ++cy) {
			// rows carrying text (the HUD, a title's handle, a caption) keep their glyphs
			if (rows[cy].find_first_of("abcdefghijklnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") >= 0) {
				if (cube) { rows[cy] = quantize(rows[cy]); }
			} else {
				rows[cy] = density_row(cy);
			}
		}
		return rows.join("\n");
	}

	string quantize(string text) {
		string result = "";
		int pos = 0;
		while (true) {
			int at = text.find(CSI, pos);
			if (at < 0) { break; }
			int end = text.find("m", at);
			string params = text.substr(at + 2, end - at - 2);
			if (!cube_params.has(params)) { cube_params[params] = cube_color(params); }
			result += text.substr(pos, at - pos) + CSI + cube_params[params] + "m";
			pos = end + 1;
		}
		return result + text.substr(pos);
	}

	string cube_color(string params) {
		var parts = params.split(";");
		if (parts.size() != 5 || parts[1] != "2") { return params; }
		int n = 16 + 36 * cube_axis(parts[2].to_int()) + 6 * cube_axis(parts[3].to_int()) + cube_axis(parts[4].to_int());
		return `${parts[0]};5;${n}`;
	}

	// a cell's density carries its two pixels' mean brightness (a square-root ramp, so
	// dark scenes keep their shapes) and its color only their hue, at full value
	string density_row(int cy) {
		string result = cube ? CSI + "48;5;16m" : CSI + "48;2;0;0;0m";
		int last = -1;
		int top_row = cy * 2 * HB_W;
		for (int x = 0; x < HB_W; ++x) {
			int top = HB_RGB[HB_PIX[top_row + x]];
			int bottom = HB_RGB[HB_PIX[top_row + HB_W + x]];
			int r = ((top >> 16) + (bottom >> 16)) / 2;
			int g = (((top >> 8) & 255) + ((bottom >> 8) & 255)) / 2;
			int b = ((top & 255) + (bottom & 255)) / 2;
			int glyph = itrunc(math::sqrt((r * 299 + g * 587 + b * 114) / 255000.0) * 9.99);
			if (glyph > 0 && r * 65536 + g * 256 + b != last) {
				last = r * 65536 + g * 256 + b;
				int peak = math::max(r, math::max(g, b));
				string rgb = `${r * 255 / peak};${g * 255 / peak};${b * 255 / peak}`;
				result += cube ? CSI + cube_color(`38;2;${rgb}`) + "m" : `${CSI}38;2;${rgb}m`;
			}
			result += ramp[glyph];
		}
		return result + RESET;
	}
}

// -------------------------------------------------------------------- pump ---
// the host's interactive loop: clock dt (clamped 0.1s) + fps EMA, poll one key
// edge, run demo_frame, present with a home escape, sleep off the fps budget.

function run_interactive() -> int {
	int w = OPT_W > 0 ? OPT_W : term_cols() - 1;   // one column spare: no wrap
	int h = OPT_H > 0 ? OPT_H : term_rows() - 1;   // one row spare: no scroll
	w = w > 110 ? 110 : (w < 60 ? 60 : w);
	h = h > 44 ? 44 : (h < 24 ? 24 : h);
	demo_boot(w, h, OPT_SEED, RUNNER_BACKEND, false, true);
	if (OPT_SCENE >= 0) { demo_jump(OPT_SCENE, 5.0); }
	HB_WORKERS = w >= 100 ? 1 : 0;
	string music_path = OPT_MUSIC ? write_soundtrack() : "";
	var music = music_path != "" ? play_sound(music_path, true) : null;

	var terminal = Terminal();
	terminal.cube = OPT_CUBE;
	terminal.ascii = OPT_ASCII;
	print(ESC + "[?1049h" + ESC + "[?25l" + ESC + "[2J" + ESC + "[H", skip_newline);
	time::stopwatch show;
	time::stopwatch pace;
	float fps_ema = OPT_FPS;
	float sim_ms = 0.0;
	float draw_ms = 0.0;
	int budget_ms = itrunc(1000.0 / OPT_FPS);
	show.start();
	while (true) {
		float t = show.elapsed();
		float dt = pace.elapsed();
		pace.start();
		if (dt > 0.1) { dt = 0.1; }
		if (dt > 0.0001) { fps_ema = fps_ema * 0.92 + (1.0 / dt) * 0.08; }

		string key = read_key();
		if (key == "q" || key == "esc") { break; }
		if (key == "tab" || key == "r") { key = ""; }   // single engine, no eval: unsupported here

		string frame = demo_frame(t, dt, key, fps_ema, sim_ms, draw_ms, 0.0);
		sim_ms = pace.delta() * 1000;
		print(ESC + "[H" + terminal.present(frame) + ESC + "[0m", skip_newline);
		draw_ms = pace.delta() * 1000;


		float used = pace.elapsed() * 1000;
		if (used < budget_ms) { sleep((budget_ms - used) / 1000.0); }
	}
	if (music != null) { music.stop(); }
	print(ESC + "[0m" + ESC + "[?1049l" + ESC + "[?25h", skip_newline);
	return 0;
}

if (OPT_MIDI != "") { write_file(OPT_MIDI, compose_soundtrack(OPT_SEED)); print("soundtrack -> " + OPT_MIDI); }
else if (OPT_BENCH > 0) { run_bench(OPT_BENCH); }
else if (OPT_SHOT >= 0) { run_shot(OPT_SHOT, OPT_AT); }
else if (OPT_FRAMES > 0) { run_headless(OPT_FRAMES); }
else { run_interactive(); }
0
