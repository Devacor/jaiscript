// JaiDOOM vid.jai — the terminal video layer (doom-cli method, GLOOM lineage):
// PIX is a flat palette-index grid; colors intern once through rgb_idx; the
// blitter packs 1x2 pixels per cell through the upper-half-block glyph with
// truecolor fg/bg, emitting escapes only on change. Byte output (utf8, BMP)
// mints through to_char — no dependency on any load-time table.

import "wad.jai";

string ESC = "";
string RESET = "";
string HALFG = "";
string HOME = "";
string CLEAR = "";

array<int> PIX = [];
int VW = 0;
int VH = 0;
int VIEWH = 0;                // rows the 3D view may touch (hud bar reserves the rest)
// 0 = serial blit; nonzero = parallel_transform rows (GLOOM pattern).
// Measured on the post-region-kill VM: 120x78 still favors serial on the
// whole frame (blit is ~8% of it), 200x140 wins outright (blit 2.6 -> 1.1ms,
// total ~21 -> ~20.7). play() auto-enables at >= 160 cols; gates and the
// bench set this explicitly.
int BLIT_WORKERS = 0;
array<int> ROWIDX = [];       // parallel_transform input, one element per glyph row

map<int, int> PAL_KEYS = {};
array<string> PAL_FG = [];
array<string> PAL_BG = [];
array<int> PAL_LUM = [];
array<int> PAL_R = [];
array<int> PAL_G = [];
array<int> PAL_B = [];
array<string> PAL_BGR3 = [];  // 3-byte BGR string per index (BMP fast path)

function utf8(int cp) -> string {
	if (cp < 128) { return "" + to_char(cp); }
	if (cp < 2048) { return "" + to_char(192 + (cp >> 6)) + to_char(128 + (cp & 63)); }
	if (cp < 65536) { return `${to_char(224 + (cp >> 12))}${to_char(128 + ((cp >> 6) & 63))}${to_char(128 + (cp & 63))}`; }
	return "" + to_char(240 + (cp >> 18)) + to_char(128 + ((cp >> 12) & 63)) + to_char(128 + ((cp >> 6) & 63)) + to_char(128 + (cp & 63));
}

function vid_init(int w, int h) -> void {
	ESC = "\x1b";
	RESET = ESC + "[0m";
	HALFG = utf8(0x2580);
	HOME = ESC + "[H";
	CLEAR = ESC + "[2J" + ESC + "[H";
	VW = w;
	VH = h;
	VIEWH = h;
	PIX = [];
	for (int i = 0; i < w * h; ++i) { PIX.push(0); }
	ROWIDX = [];
	int vrows = h / 2;
	for (int r = 0; r < vrows; ++r) { ROWIDX.push(r); }
	PAL_KEYS = {};
	PAL_FG = [];
	PAL_BG = [];
	PAL_LUM = [];
	PAL_R = [];
	PAL_G = [];
	PAL_B = [];
	PAL_BGR3 = [];
	rgb_idx(0, 0, 0);
}

function rgb_idx(int r, int g, int b) -> int {
	int key = r * 65536 + g * 256 + b;
	if (PAL_KEYS.has(key)) { return PAL_KEYS[key]; }
	int idx = PAL_FG.size();
	PAL_KEYS[key] = idx;
	PAL_FG.push(`${ESC}[38;2;${r};${g};${b}m`);
	PAL_BG.push(`${ESC}[48;2;${r};${g};${b}m`);
	PAL_LUM.push(r * 2 + g * 3 + b);
	PAL_R.push(r);
	PAL_G.push(g);
	PAL_B.push(b);
	PAL_BGR3.push(`${to_char(b)}${to_char(g)}${to_char(r)}`);
	return idx;
}

// ground-truth artifact: the palette-index grid as a 24-bit BMP
function bmp_write(string path) -> void {
	int pad = (4 - (VW * 3) % 4) % 4;
	int stride = VW * 3 + pad;
	binary::writer out;
	out.bytes("BM").uint32(54 + stride * VH).uint32(0).uint32(54);
	out.uint32(40).int32(VW).int32(VH).uint16(1).uint16(24).uint32(0).uint32(stride * VH);
	out.int32(2835).int32(2835).uint32(0).uint32(0);
	var& pix = PIX;
	var& bgr = PAL_BGR3;
	for (int y = VH - 1; y >= 0; --y) {
		int base = y * VW;
		string row = "";
		for (int x = 0; x < VW; ++x) { row += bgr[pix[base + x]]; }
		out.bytes(row);
		for (int p = 0; p < pad; ++p) { out.uint8(0); }
	}
	write_file(path, out.to_string());
}

function pix_clear(int color) -> void {
	int n = VW * VH;
	for (int i = 0; i < n; ++i) { PIX[i] = color; }
}

// ------------------------------------------------------------- screen melt ---
// the classic wipe: the departing frame slides DOWN in ragged columns over
// whatever the draw path just rendered, top rows revealing first. Presentation
// only — its own LCG (the face_rnd pattern) never touches SIM_RANDOM, whose
// stream belongs to the sim (demos and netplay replay it), and nothing
// consults WIPE_TICS except play()'s draw path.
int WIPE_TICS = 0;            // 0 = inactive
array<int> WIPE_COL = [];     // per-column offset: <0 waiting, then falling
array<int> WIPE_OLD = [];     // snapshot of the departing frame
int WIPE_SEED = 1;

function wipe_rnd(int n) -> int {
	WIPE_SEED = (WIPE_SEED * 1103515245 + 12345) & 2147483647;
	return (WIPE_SEED >> 16) % n;
}

function wipe_begin() -> void {
	WIPE_OLD = PIX;
	WIPE_SEED = (time::unix_ms() & 65535) | 1;
	WIPE_COL = [];
	WIPE_COL.push(0 - wipe_rnd(16));
	for (int x = 1; x < VW; ++x) {
		int o = WIPE_COL[x - 1] + wipe_rnd(3) - 1;   // neighbors correlate: the ragged edge
		if (o < -15) { o = -15; }
		if (o > 0) { o = 0; }
		WIPE_COL.push(o);
	}
	WIPE_TICS = 1;
}

// one melt step; true when every column has fallen off the screen. A waiting
// column counts up to 0; a falling one accelerates from 1 toward 8/tic.
function wipe_tic() -> bool {
	bool done = true;
	for (int x = 0; x < VW; ++x) {
		int o = WIPE_COL[x];
		if (o < 0) { WIPE_COL[x] = o + 1; done = false; }
		else if (o < VH) {
			WIPE_COL[x] = o + (o < 16 ? o + 1 : 8);
			done = false;
		}
	}
	return done;
}

// compose: rows at or below each column's offset still show the departing
// frame, shifted down by that offset
function wipe_overlay() -> void {
	for (int x = 0; x < VW; ++x) {
		int o = WIPE_COL[x];
		if (o >= VH) { continue; }
		if (o < 0) { o = 0; }
		for (int y = VH - 1; y >= o; --y) {
			PIX[y * VW + x] = WIPE_OLD[(y - o) * VW + x];
		}
	}
}

function pix_set(int x, int y, int c) -> void {
	if (x >= 0 && x < VW && y >= 0 && y < VH) { PIX[y * VW + x] = c; }
}

function draw_line(int x0, int y0, int x1, int y1, int c) -> void {
	int dx = x1 - x0; if (dx < 0) { dx = -dx; }
	int dy = y1 - y0; if (dy < 0) { dy = -dy; }
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx - dy;
	while (true) {
		pix_set(x0, y0, c);
		if (x0 == x1 && y0 == y1) { break; }
		int e2 = err * 2;
		if (e2 > -dy) { err = err - dy; x0 = x0 + sx; }
		if (e2 < dx) { err = err + dx; y0 = y0 + sy; }
	}
}

// half mode: 1x2 px per cell, fg=top bg=bottom, escapes only on change.
// ONE glyph row, written as a parallel_transform body over CAPTURED state
// (GLOOM's pattern): PIX is all-primitive and touched only by subscript
// reads, so the region borrows it zero-copy; PAL_FG/PAL_BG are string
// tables (per-worker snapshots at the barrier); VW/HALFG/RESET capture as
// scalar copies. No var& aliases of captured names — subscript the globals
// directly. The SAME function runs the serial loop when BLIT_WORKERS == 0.
function vid_row_half(int cy) -> string {
	string row = "";
	int vw = VW;
	int fg = -1;
	int bg = -1;
	int ti = cy * 2 * vw;
	int bi = ti + vw;
	for (int x = 0; x < vw; ++x) {
		int top = PIX[ti + x];
		int bot = PIX[bi + x];
		if (top != fg) { row += PAL_FG[top]; fg = top; }
		if (bot != bg) { row += PAL_BG[bot]; bg = bot; }
		row += HALFG;
	}
	return row + RESET;
}

function blit_half() -> string {
	if (BLIT_WORKERS != 0) {
		var rows = parallel_transform(ROWIDX, vid_row_half);
		return rows.join("\n") + "\n";
	}
	string out = "";
	int vrows = VH / 2;
	for (int cy = 0; cy < vrows; ++cy) { out += vid_row_half(cy) + "\n"; }
	return out;
}
