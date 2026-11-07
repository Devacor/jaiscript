// GRUBWELL on the standalone runner: the C++ host's contract (../main.cpp) written in JaiScript.
// Drag this file onto jaiscript.exe; it lives in scripts/ so main.jai's imports resolve, and the
// save goes beside scripts/ whatever the working directory.
//
//   jaiscript crawler.jai                                   play, seed 1337
//   jaiscript crawler.jai -- seed 7 god dev load
//   jaiscript crawler.jai -- colors 256 ascii               the 256-colour cube, ASCII glyphs
//   jaiscript crawler.jai -- smoke turns 1500 seed 6        headless autopilot, prints STATE_HASH
//   jaiscript crawler.jai -- input "n11;wwwww" quiet        scripted keys (';' enter, '^' esc)
//   jaiscript --backend=interp crawler.jai -- smoke backend interp
//
// Flags mirror main.cpp, leading dashes optional: seed N, smoke, turns N, input KEYS, quiet, dev,
// god, load, save PATH, backend LABEL, colors 24|256, ascii. Shim only: view N (forward view
// rows), layout single|multi, screen CxR, dump PATH (every smoke frame and a FRAME_HASH),
// shots N, bench N.
//
// The runner's keyboard: shift+letter gives the uppercase commands, TAB descends and SPACE
// waits (stand-ins for '>' and '.'), Ctrl+C leaves. Its POSIX keyboard has no shift, so esc
// opens the pause page, whose lowercase keys save, quit and reload.

import "host_capture.jai";

var HOST_SEED = 1337;
var HOST_SMOKE = false;
var HOST_TURNS = 250;
var HOST_DEV = false;
var HOST_GOD = false;
var HOST_LOAD = false;
var HOST_SCRIPTED = false;
var HOST_INTERACTIVE = false;
var HOST_TRUECOLOR = true;
var HOST_ASCII = false;
var HOST_BACKEND = "vm";
var SAVE_PATH = parent_dir(root_script_dir()) + "/crawler_save.json";
var SHIM_QUIET = false;
var SHIM_INPUT = "";
var SHIM_COLORS = "";
var SHIM_DUMP = "";         // dump PATH: every smoke turn's frame to PATH.ans, and the FRAME_HASH
var SHIM_BENCH = 0;         // bench N: re-render the final smoke view N times, print ms/frame
var SHIM_SHOT_EVERY = 0;    // shots N (with dump): PATH_tT.bmp and .txt every N turns
var SHIM_VIEW_ROWS = 0;
var SHIM_LAYOUT = 0;        // 1 single, 2 multi, 0 fit the console
var SHIM_SCREEN_COLS = 0;
var SHIM_SCREEN_ROWS = 0;

function shim_flag_name(string a) -> string {
	int i = 0;
	while (i < a.size() && a[i] == "-") { i += 1; }
	return a.substr(i, a.size() - i);
}

{
	int i = 0;
	while (i < ARGS.size()) {
		string flag = shim_flag_name(ARGS[i]);
		string value = i + 1 < ARGS.size() ? ARGS[i + 1] : "";
		int taken = 2;
		if (flag == "seed") { HOST_SEED = value.to_int(HOST_SEED); }
		else if (flag == "turns" || flag == "frames") { HOST_TURNS = value.to_int(HOST_TURNS); }
		else if (flag == "input") { SHIM_INPUT = value; }
		else if (flag == "save") { SAVE_PATH = value; }
		else if (flag == "backend") { HOST_BACKEND = value; }
		else if (flag == "colors") { SHIM_COLORS = value; }
		else if (flag == "dump") { SHIM_DUMP = value; HOST_SMOKE = true; }
		else if (flag == "shots") { SHIM_SHOT_EVERY = value.to_int(0); }
		else if (flag == "view") { SHIM_VIEW_ROWS = value.to_int(20); }
		else if (flag == "layout") { SHIM_LAYOUT = value == "single" ? 1 : 2; }
		else if (flag == "screen") {
			int x = value.find("x");
			if (x > 0) {
				SHIM_SCREEN_COLS = value.substr(0, x).to_int(0);
				SHIM_SCREEN_ROWS = value.substr(x + 1, value.size() - x - 1).to_int(0);
			}
		}
		else if (flag == "bench") { SHIM_BENCH = value.to_int(200); HOST_SMOKE = true; }
		else {
			taken = 1;
			if (flag == "smoke") { HOST_SMOKE = true; }
			else if (flag == "quiet") { SHIM_QUIET = true; }
			else if (flag == "dev") { HOST_DEV = true; }
			else if (flag == "god") { HOST_GOD = true; }
			else if (flag == "load") { HOST_LOAD = true; }
			else if (flag == "ascii") { HOST_ASCII = true; }
			else { print(`crawler shim: unknown arg '${ARGS[i]}'`); }
		}
		i += taken;
	}
}
if (HOST_SMOKE) { SHIM_QUIET = true; }
if (SHIM_INPUT != "") { HOST_SCRIPTED = true; }
HOST_INTERACTIVE = !SHIM_QUIET && !HOST_SCRIPTED;
// COLORTERM=truecolor|24bit says 24-bit, as does any Windows console; elsewhere the 256-colour cube
HOST_TRUECOLOR = SHIM_COLORS != "" ? SHIM_COLORS == "24" : env_var("COLORTERM") == "truecolor" || env_var("COLORTERM") == "24bit" || app_data_dir("x").find(":") == 1;

// ---- the console ----
string SHIM_ENTER = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H";
string SHIM_LEAVE = "\x1b[0m\x1b[2J\x1b[H\x1b[?25h\x1b[?1049l";
array<string> SHIM_HELD_LOG = [];

array<string> SHIM_ENDING = [];   // with dump: the death or victory pages a smoke run ends on

function draw(string frame) -> void {
	if (SHIM_QUIET) {
		if (SHIM_DUMP != "") { SHIM_ENDING.push(frame); }
		return;
	}
	print("\x1b[H" + frame + "\x1b[0m", skip_newline);
}

function frame_mark() -> void { }   // the native host's --time probe

function host_log(string line) -> void {
	if (SHIM_QUIET) { print(line); } else { SHIM_HELD_LOG.push(line); }
}

function shim_leave() -> void {
	if (!SHIM_QUIET) { print(SHIM_LEAVE, skip_newline); }
	for (auto line : SHIM_HELD_LOG) { print(line); }
	SHIM_HELD_LOG = [];
}

// read_key blocks over the runner's non-blocking poll; poll_key waits at most the timeout.
// Scripted input replays main.cpp's stand-ins (';' enter, '^' esc) and quits when it runs out.
array<string> SHIM_KEYS = [];
int SHIM_KEY_INDEX = 0;
{
	for (int i = 0; i < SHIM_INPUT.size(); ++i) { SHIM_KEYS.push(SHIM_INPUT.substr(i, 1)); }
}

function scripted_key() -> string {
	if (SHIM_KEY_INDEX >= SHIM_KEYS.size()) { return "Q"; }
	string queued = SHIM_KEYS[SHIM_KEY_INDEX];
	SHIM_KEY_INDEX += 1;
	if (queued == ";") { return "enter"; }
	if (queued == "^") { return "esc"; }
	return queued;
}

function console_key() -> string {
	string k = HOST_READ_KEY_RAW();
	if (k == "" || k == "shift" || k == "ctrl" || k == "alt") { return ""; }
	if (k.size() == 1) {
		if (k == "c" && key_down("ctrl")) { throw "interrupted"; }
		char c = k[0];
		return key_down("shift") && c >= 'a' && c <= 'z' ? `${to_char(c - 32)}` : k;
	}
	if (k == "tab") { return ">"; }
	if (k == "space") { return "."; }
	return k;
}

function read_key() -> string {
	if (HOST_SCRIPTED) { return scripted_key(); }
	while (true) {
		string k = console_key();
		if (k != "") { return k; }
		sleep(0.01);
	}
	return "";
}

function poll_key(float seconds) -> string {
	if (HOST_SCRIPTED) { return scripted_key(); }
	time::stopwatch waited;
	waited.start();
	while (true) {
		string k = console_key();
		if (k != "" || waited.elapsed() >= seconds) { return k; }
		sleep(0.01);
	}
	return "";
}

// ---- the deterministic Rng: a bit-exact xorshift64* twin of main.cpp's crawler_rng ----
// Checked int64 arithmetic cannot wrap, so the mod-2^64 multiply runs in 16- and 32-bit limbs,
// and since >> is arithmetic a logical shift masks after shifting.
int RNG_GOLDEN = -7046029254386353131;   // 0x9E3779B97F4A7C15 as a signed int64

function u64_shr(int x, int n) -> int { return (x >> n) & ((1 << (64 - n)) - 1); }

function u64_add(int x, int y) -> int {
	int lo = (x & 0xFFFFFFFF) + (y & 0xFFFFFFFF);
	int hi = u64_shr(x, 32) + u64_shr(y, 32) + (lo >> 32);
	return (lo & 0xFFFFFFFF) | (hi << 32);
}

function u32_mul_lo(int x, int y) -> int {
	int xl = x & 0xFFFF;
	int xh = x >> 16;
	int yl = y & 0xFFFF;
	int yh = y >> 16;
	return (xl * yl + ((xl * yh + xh * yl) << 16)) & 0xFFFFFFFF;
}

function u64_mul(int a, int b) -> int {
	int a0 = a & 0xFFFFFFFF;
	int a1 = u64_shr(a, 32);
	int b0 = b & 0xFFFFFFFF;
	int b1 = u64_shr(b, 32);
	int lo = u64_add(a0 * (b0 & 0xFFFF), (a0 * (b0 >> 16)) << 16);
	int mid = (u32_mul_lo(a0, b1) + u32_mul_lo(a1, b0)) & 0xFFFFFFFF;
	return u64_add(lo, mid << 32);
}

function u64_mod_small(int x, int n) -> int {
	int r = (x & 0x7FFFFFFFFFFFFFFF) % n;
	if (x < 0) { r = (r + (((0x4000000000000000 % n) * 2) % n)) % n; }
	return r;
}

class Rng {
	int s = 0;

	Rng(int seed) {
		s = seed != 0 ? seed : RNG_GOLDEN;
		for (int i = 0; i < 4; ++i) { step(); }
	}

	void step() {
		s = s ^ u64_shr(s, 12);
		s = s ^ (s << 25);
		s = s ^ u64_shr(s, 27);
	}

	int next_raw() {
		step();
		return u64_mul(s, 0x2545F4914F6CDD1D);
	}

	int next(int n) { return n <= 0 ? 0 : u64_mod_small(next_raw(), n); }
	int roll(int lo, int hi) { return hi <= lo ? lo : lo + next(hi - lo + 1); }
	bool chance(int percent) { return next(100) < percent; }
	int state() { return s & 0x3FFFFFFFFFFFFFFF; }

	void set_state(int v) { s = v == 0 ? RNG_GOLDEN : v; }
}

// ---- boot: main.cpp's order ----
import "main.jai";

array<string> SHIM_FRAMES = [];
float SHIM_FRAME_MS = 0.0;

function shim_view_probe() -> void {
	time::stopwatch build;
	build.start();
	string frame = frame_join(frame_lines(game_view_lines()));
	SHIM_FRAME_MS += build.elapsed() * 1000;
	SHIM_FRAMES.push(frame);
	if (SHIM_SHOT_EVERY > 0 && G.turn % SHIM_SHOT_EVERY == 0) { shim_dump_view(`${SHIM_DUMP}_t${G.turn}`); }
}

// the last canvas as ground truth: a 24-bit BMP, and a sketch (pane pixels as luminance,
// frames, labels and boxes as their text)
function shim_dump_view(string base) -> void {
	int cw = STRIDE;
	int ch = CANVAS_H * 2;
	int pad = (4 - (cw * 3) % 4) % 4;
	binary::writer bmp;
	bmp.bytes("BM").uint32(54 + (cw * 3 + pad) * ch).uint32(0).uint32(54);
	bmp.uint32(40).int32(cw).int32(ch).uint16(1).uint16(24).uint32(0).uint32((cw * 3 + pad) * ch);
	bmp.int32(2835).int32(2835).uint32(0).uint32(0);
	for (int y = ch - 1; y >= 0; --y) {
		for (int x = 0; x < cw; ++x) {
			int rgb = VPAL_RGB[VPIX[y * cw + x]];
			bmp.uint8(rgb & 255).uint8((rgb >> 8) & 255).uint8((rgb >> 16) & 255);
		}
		for (int p = 0; p < pad; ++p) { bmp.uint8(0); }
	}
	string ramp = " .:-=+*#%@";
	array<string> box_plain = [];
	for (auto row : BOX_ROWS) { box_plain.push(plain_text(row)); }
	array<string> sketch = [];
	for (int y = 0; y < ch; y += 2) {
		string row = "";
		for (int x = 0; x < cw; ++x) {
			int cell = (y / 2) * cw + x;
			int kind = CHROME_KIND[cell];
			if (kind == 3) {
				int ref = CHROME_REF[cell];
				row += utf8_cell(box_plain[ref], x - BOX_ROW_X[ref]);
				continue;
			}
			if (kind >= 0) {
				row += CHROME_CH[cell];
				continue;
			}
			int a = VPAL_RGB[VPIX[y * cw + x]];
			int b = VPAL_RGB[VPIX[(y + 1) * cw + x]];
			int lum = ((a >> 16) & 255) * 2 + ((a >> 8) & 255) * 3 + (a & 255) + ((b >> 16) & 255) * 2 + ((b >> 8) & 255) * 3 + (b & 255);
			row += ramp.substr(imin(9, lum / 250), 1);
		}
		sketch.push(row);
	}
	write_file(base + ".bmp", bmp.to_string());
	write_file(base + ".txt", sketch.join("\n") + "\n");
}

// the cell-th character of UTF-8 text
function utf8_cell(string s, int cell) -> string {
	int n = 0;
	int i = 0;
	while (i < s.size()) {
		int len = 1;
		int b = cast<int>(s[i]);
		if (b >= 0xF0) { len = 4; } else if (b >= 0xE0) { len = 3; } else if (b >= 0xC0) { len = 2; }
		if (n == cell) { return s.substr(i, len); }
		n += 1;
		i += len;
	}
	return " ";
}

function shim_probe_report() -> void {
	if (SHIM_DUMP != "") {
		shim_dump_view(SHIM_DUMP);
		string path = SHIM_DUMP + ".ans";
		write_file(path, SHIM_FRAMES.join(""));
		print(`FRAME_HASH: ${crypto::sha256_file(path)} (${SHIM_FRAMES.size()} frames -> ${path})`);
		print(`frame build: ${SHIM_FRAME_MS:.0f} ms total, ${SHIM_FRAME_MS / imax(1, SHIM_FRAMES.size()):.3f} ms/frame`);
		if (SHIM_ENDING.size() > 0) { write_file(SHIM_DUMP + "_end.ans", SHIM_ENDING.join("")); }
	}
	if (SHIM_BENCH > 0) {
		time::stopwatch bench;
		bench.start();
		int bytes = 0;
		for (int i = 0; i < SHIM_BENCH; ++i) { bytes = frame_join(frame_lines(game_view_lines())).size(); }
		print(`bench: ${SHIM_BENCH} frames, ${bench.elapsed() * 1000 / SHIM_BENCH:.3f} ms/frame, ${bytes} bytes/frame`);
	}
}

if (SHIM_DUMP != "") { VIEW_PROBE = shim_view_probe; }
VIEW_ROWS_FIXED = SHIM_VIEW_ROWS > 0 ? imax(12, SHIM_VIEW_ROWS) : 0;
LAYOUT_FIXED = SHIM_LAYOUT;
SCREEN_COLS = SHIM_SCREEN_COLS;
SCREEN_ROWS = SHIM_SCREEN_ROWS;
if (!SHIM_QUIET) { print(SHIM_ENTER, skip_newline); }
try {
	crawler_main();
	shim_leave();
	shim_probe_report();
} catch (e) {
	shim_leave();
	if (to_string(e) != "interrupted") {
		print(`crawler shim: script error: ${e}`);
		throw e;
	}
}
