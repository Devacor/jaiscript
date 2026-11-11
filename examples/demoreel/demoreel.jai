// demoreel.jai — drag-drop shim: runs the demoreel on the standalone jaiscript
// runner with zero C++ beyond the runner itself. Recreates the C++ host contract
// (demoreel_host.cpp) in script: the ESC global, the itrunc/ifloor casts, host_log
// and the seeded xorshift64* Rng class, bit-exact under checked int64 (16-bit-limb
// multiplies, masked logical shifts). Then it imports the scene files in the host's
// load order and pumps demo_frame at 30 fps.
//
// Not ported from the C++ host: its escape filter and row-diff redraw (frames
// present whole; the scripts' own run-length escapes still apply) and 'r' hot
// reload.
//
//   jaiscript demoreel.jai                        the reel (q or esc quits)
//   jaiscript demoreel.jai -- color 256           the reel quantized to the xterm-256 cube (color 24: 24-bit)
//   jaiscript demoreel.jai -- ascii 1             one density character per cell, no block glyphs
//   jaiscript demoreel.jai -- frames 60           headless self-test: N smoke frames (100x40,
//                                                 seed 20260705, fixed dt 1/30), sizes, a stream
//                                                 hash and timing
//   jaiscript demoreel.jai -- frames 60 scene I at T   scene I at its real length from scene time T
//   jaiscript demoreel.jai -- frames 60 dump P    also write frames 0/N-2/N-1 to P_<i>.txt
//                                                 (+ P_<i>.bmp, the pixel grid) for byte diffing
//   jaiscript demoreel.jai -- bench 60            per-scene ms/frame + bytes/frame
//   jaiscript demoreel.jai -- shot I at T dump P  scene I from scene time T-0.2, frame n
//                                                 (default 6) -> P.txt (as presented, so color
//                                                 and ascii apply) + P.bmp
//   jaiscript demoreel.jai -- midi DIR            write the soundtrack's cues (one MIDI file a scene) and exit
//   jaiscript demoreel.jai -- video DIR every N   the whole reel's frames (every Nth, 24-bit text) and its cues; frames M stops early
//   other args: seed N | fps N | w N | h N | scene I (jump after boot) | music 0 (silent) | pix half|sext|auto
//               | workers 0|1 (rows in parallel; the reel's default: on at 100 columns or more, off headless)
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

// where the scene files live: EIGHT imports a module from there while it runs
var SCENES_DIR = current_script_dir() + "/scenes";
import "scenes/util.jai";
import "scenes/music.jai";
import "scenes/reel.jai";
import "scenes/donut.jai";
import "scenes/julia.jai";
import "scenes/lava.jai";
import "scenes/pipes.jai";
import "scenes/fire.jai";
import "scenes/boids.jai";
import "scenes/globhunter.jai";
import "scenes/voxelspace.jai";
import "scenes/chrome.jai";
import "scenes/skull.jai";
import "scenes/fluid.jai";
import "scenes/show.jai";
import "scenes/eight.jai";
import "scenes/finale.jai";
import "scenes/intro.jai";
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
var OPT_AT_GIVEN = false;
var OPT_SHOTN = 6;
var OPT_MUSIC = true;
var OPT_MIDI = "";
// the xterm-256 cube unless the terminal says it takes 24-bit color (COLORTERM, Windows Terminal); `color 24` / `color 256` decide
var OPT_CUBE = !(env_var("COLORTERM") == "truecolor" || env_var("COLORTERM") == "24bit" || env_var("WT_SESSION") != "");
var OPT_ASCII = false;
var OPT_PIX = "";
var OPT_VIDEO = "";
var OPT_EVERY = 2;
var OPT_WORKERS = -1;       // -1 = the run's default

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
	else if (k == "at") { OPT_AT = v.to_float(); OPT_AT_GIVEN = true; }
	else if (k == "n") { OPT_SHOTN = v.to_int(); }
	else if (k == "music") { OPT_MUSIC = v != "0"; }
	else if (k == "midi") { OPT_MIDI = v; }
	else if (k == "color") { OPT_CUBE = v == "256"; }
	else if (k == "ascii") { OPT_ASCII = v != "0"; }
	else if (k == "pix") { OPT_PIX = v; }
	else if (k == "video") { OPT_VIDEO = v; }
	else if (k == "every") { OPT_EVERY = v.to_int(); }
	else if (k == "workers") { OPT_WORKERS = v.to_int(); }
	else { print(`demoreel.jai: unknown arg '${k}' (frames/seed/fps/w/h/scene/dump/bench/shot/at/n/music/midi/color/ascii/pix/video/every/workers)`); }
}
// the density map reads two pixels a cell
demo_pixels(OPT_ASCII ? "half" : OPT_PIX);

// ---------------------------------------------------------------- headless ---
// Mirrors the host's run_smoke: 100x40, fixed dt 1/30, smoke=true, hud off,
// key "" and zeroed timing args every frame (`at` plays the scene at its real
// length instead of smoke's three seconds). The rolling hash makes runs
// comparable across runner backends (vm vs interp) without keeping frames.

function run_headless(int frames) -> int {
	int w = OPT_W > 0 ? OPT_W : 100;
	int h = OPT_H > 0 ? OPT_H : 40;
	float dt = 1.0 / 30.0;
	demo_boot(w, h, OPT_SEED, "vm", !OPT_AT_GIVEN, false);
	if (OPT_SCENE >= 0) { demo_jump(OPT_SCENE, OPT_AT_GIVEN ? OPT_AT : 5.0); }
	HB_WORKERS = OPT_WORKERS > 0 ? 1 : 0;
	print(format("demoreel shim headless: {0} frames @ {1}x{2}, seed {3}, dt {4}", frames, w, h, OPT_SEED, dt));
	int hash = 0;
	int total_bytes = 0;
	int min_len = 0; int max_len = 0;
	int text_frames = 0;
	time::stopwatch run;
	time::stopwatch render;
	run.start();
	for (int f = 0; f < frames; ++f) {
		render.resume();
		string frame = demo_frame(f * dt, dt, "", 0.0, 0.0, 0.0, 0.0);
		render.pause();
		if (TXT_USED) { ++text_frames; }
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
	int elapsed = cast<int>(run.elapsed() * 1000);
	print(format("frames {0} | bytes {1} (len {2}..{3}) | stream hash {4} | {5} ms total, {6} ms/frame incl. hashing, {7} ms/frame render",
		frames, total_bytes, min_len, max_len, hash, elapsed, (elapsed * 1.0) / frames, render.elapsed() * 1000 / frames));
	print(`rows ${HB_WORKERS != 0 ? "parallel" : "serial"} | pixels ${HB_SEXT ? "sext" : "half"} | text in ${text_frames} frames | live reloads ${LIVE_RELOADS}`);
	return 0;
}

function le16(int v) -> string { return "" + to_char(v & 255) + to_char((v >> 8) & 255); }
function le32(int v) -> string {
	return "" + to_char(v & 255) + to_char((v >> 8) & 255) + to_char((v >> 16) & 255) + to_char((v >> 24) & 255);
}

// the presenter's pixel grid as a 24-bit BMP (what the half blocks show)
function write_bmp(string path) -> void {
	int bw = HB_W;
	int bh = HB_PH;
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

// the whole reel at its real lengths with a fixed 1/30 s step, every `every`-th frame
// written as it would reach a 24-bit terminal (DIR/frame_00000.txt ...), and the cues
// beside them: the material for a video of the reel (docs/site_tools/render_reel_video.py)
function run_video(string dir) -> int {
	int w = OPT_W > 0 ? OPT_W : 120;
	int h = OPT_H > 0 ? OPT_H : 40;
	float dt = 1.0 / 30.0;
	demo_boot(w, h, OPT_SEED, "vm", false, false);
	make_dirs(dir);
	write_cues(dir);
	float total = 0.0;
	for (int i = 0; i <= reel.intro_idx; ++i) { total += reel.dur_for(i); }
	int frames = itrunc(total / dt);
	var terminal = Terminal();
	int written = 0;
	if (OPT_FRAMES > 0 && OPT_FRAMES < frames) { frames = OPT_FRAMES; }
	for (int f = 0; f < frames; ++f) {
		string frame = demo_frame(f * dt, dt, "", 0.0, 0.0, 0.0, 0.0);
		if (f % OPT_EVERY != 0) { continue; }
		string n = to_string(written);
		write_file(dir + "/frame_" + string::repeat("0", 5 - n.size()) + n + ".txt", terminal.present(frame));
		++written;
	}
	write_file(dir + "/reel.txt", `frames ${written}\nfps ${30.0 / OPT_EVERY}\ncols ${w}\nrows ${h}\nseconds ${total}\n`);
	print(`video: ${written} frames at ${30.0 / OPT_EVERY} fps (${total} s) -> ${dir}`);
	return 0;
}

// every scene mid-flight (past its title card), no transitions: script cost
// of update + render + presentation per frame, and the bytes a frame emits
function run_bench(int frames) -> int {
	int w = OPT_W > 0 ? OPT_W : 100;
	int h = OPT_H > 0 ? OPT_H : 40;
	float dt = 1.0 / 30.0;
	demo_boot(w, h, OPT_SEED, "vm", false, false);
	int n = reel.nscenes;
	print(format("demoreel bench: {0} frames per scene @ {1}x{2} cells ({3}x{4} px)", frames, w, h, reel.pw, reel.ph));
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
// The music is the reel's own (scenes/music.jai): one cue per scene, composed at
// boot and written as Standard MIDI Files for the runner's sequencer. The pump
// preloads them all and switches cue the frame a scene (or its transition) starts,
// which is a bar line, so the music and the pictures cut together.

// the cue files' paths, or [] when they can't be written (then the reel runs silent)
function write_cues(string dir) -> var {
	var result = [];
	if (!make_dirs(dir)) { return result; }
	for (int i = 0; i < reel.cues.size(); ++i) {
		string path = dir + `/cue_${i < 10 ? "0" : ""}${i}.mid`;
		if (!write_file(path, reel.cues[i].midi())) { return []; }
		result.push(path);
	}
	return result;
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
	HB_WORKERS = OPT_WORKERS >= 0 ? (OPT_WORKERS > 0 ? 1 : 0) : (w >= 100 ? 1 : 0);
	var cue_paths = OPT_MUSIC ? write_cues(app_cache_dir("jai_demoreel")) : [];
	var cues = [];
	for (int c = 0; c < cue_paths.size(); ++c) { cues.push(load_sound(cue_paths[c])); }
	int playing = -1;

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
		if (dt > 0.25) { dt = 0.25; }
		if (dt > 0.0001) { fps_ema = fps_ema * 0.92 + (1.0 / dt) * 0.08; }

		string key = read_key();
		if (key == "q" || key == "esc") { break; }
		if (key == "r") { key = ""; }   // hot reload is the C++ host's

		string frame = demo_frame(t, dt, key, fps_ema, sim_ms, draw_ms, 0.0);
		int want = reel.music_cue();
		if (want != playing && want < cues.size()) {
			if (playing >= 0 && cues[playing] != null) { cues[playing].stop(); }
			if (cues[want] != null) { cues[want].play(true); }
			playing = want;
		}
		sim_ms = pace.delta() * 1000;
		print(ESC + "[H" + terminal.present(frame) + ESC + "[0m", skip_newline);
		draw_ms = pace.delta() * 1000;


		float used = pace.elapsed() * 1000;
		if (used < budget_ms) { sleep((budget_ms - used) / 1000.0); }
	}
	stop_sounds();
	print(ESC + "[0m" + ESC + "[?1049l" + ESC + "[?25h", skip_newline);
	return 0;
}

if (OPT_MIDI != "") {
	demo_boot(100, 40, OPT_SEED, "vm", false, false);
	var written = write_cues(OPT_MIDI);
	print(`${written.size()} cues -> ${OPT_MIDI}`);
}
else if (OPT_VIDEO != "") { run_video(OPT_VIDEO); }
else if (OPT_BENCH > 0) { run_bench(OPT_BENCH); }
else if (OPT_SHOT >= 0) { run_shot(OPT_SHOT, OPT_AT); }
else if (OPT_FRAMES > 0) { run_headless(OPT_FRAMES); }
else { run_interactive(); }
0
