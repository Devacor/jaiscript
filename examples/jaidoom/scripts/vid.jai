// JaiDOOM vid.jai — the terminal video layer (doom-cli method, GLOOM lineage):
// PIX is a flat palette-index grid; colors intern once through rgb_idx; the
// blitter packs each cell's pixels (1x2 through the upper half block, or 2x3
// through the Unicode 13 sextants) into one glyph with fg/bg colors, emitting
// escapes only on change (in 24-bit sextants, each side of a cell in its pixels'
// mean color, through GAMMA). Byte output (utf8, BMP) mints through to_char — no
// dependency on any load-time table.

import "wad.jai";

// 24-bit where the terminal says it takes it (COLORTERM, Windows Terminal, any Windows console), else xterm-256 (macOS
// Terminal.app reads a 24-bit escape as a run of unrelated attributes); --truecolor / --256color decide
bool HOST_TRUECOLOR = env_var("COLORTERM") == "truecolor" || env_var("COLORTERM") == "24bit" || env_var("WT_SESSION") != "" ||
	env_var("OS") == "Windows_NT";

// 2x3 sextant cells instead of 1x2 half blocks; set before vid_init. A sextant pixel is half a 1:2 cell wide and a
// third of it tall, so it stands 4/3 as tall as it is wide: PIXEL_Y scales every vertical size drawn in square units.
bool SEXTANTS = false;
float PIXEL_Y = 1.0;

string ESC = "";
string RESET = "";
string HALFG = "";
array<string> SEXTG = [];     // sextant glyphs by TL|TR<<1|ML<<2|MR<<3|BL<<4|BR<<5
string HOME = "";
string CLEAR = "";

array<int> PIX = [];
int VW = 0;
int VH = 0;
int VIEWH = 0;                // rows the 3D view may touch (hud bar reserves the rest)
// 0 = serial blit; nonzero = parallel_transform rows (GLOOM pattern). 120x39 sextant cells with the mean fit: 3.1 ms
// serial, 0.9 ms on 8 threads; play() turns it on from 80 columns, gates and the bench set it explicitly.
int BLIT_WORKERS = 0;
array<int> ROWIDX = [];       // parallel_transform input, one element per glyph row
string VID_INDENT = "";       // starts every glyph row: the cursor move that centers a view narrower than the screen
int SKIP_ROWS = 0;            // the top glyph rows leave their first SKIP_CELLS cells alone: text drawn over the view
int SKIP_CELLS = 0;           // there stays put instead of being painted over every frame
string SKIP_MOVE = "";

function vid_skip(int rows, int cells) -> void {
	int vcols = VW / cell_w();
	SKIP_CELLS = cells < vcols ? cells : vcols;
	SKIP_ROWS = rows;
	SKIP_MOVE = SKIP_CELLS > 0 ? `${ESC}[${SKIP_CELLS}C` : "";
}

map<int, int> PAL_KEYS = {};
array<string> PAL_FG = [];
array<string> PAL_BG = [];
array<int> PAL_LUM = [];
array<int> PAL_R = [];
array<int> PAL_G = [];
array<int> PAL_B = [];
array<string> PAL_BGR3 = [];  // 3-byte BGR string per index (BMP fast path)
array<int> PAL_CR = [];       // the color as the terminal gets it, through GAMMA
array<int> PAL_CG = [];
array<int> PAL_CB = [];
array<int> PAL_CK = [];       // PAL_CR/CG/CB packed: the mean fit's last-color test
string FG24 = "";
string BG24 = "";
array<string> DEC = [];       // "0".."255", and each after a ';'
array<string> DEC_SEMI = [];
int SEXT_FIT = 1;             // 1 = mean colors where the terminal takes 24-bit, 0 = the two extreme pixels

// DOOM's gamma levels 0 (as drawn) to 4 (brightest), fractions between; the terminal's colors only, never the sim
float GAMMA = 1.0;
array<int> GAMMA_LUT = [];

function utf8(int cp) -> string {
	if (cp < 128) { return "" + to_char(cp); }
	if (cp < 2048) { return "" + to_char(192 + (cp >> 6)) + to_char(128 + (cp & 63)); }
	if (cp < 65536) { return `${to_char(224 + (cp >> 12))}${to_char(128 + ((cp >> 6) & 63))}${to_char(128 + (cp & 63))}`; }
	return "" + to_char(240 + (cp >> 18)) + to_char(128 + ((cp >> 12) & 63)) + to_char(128 + ((cp >> 6) & 63)) + to_char(128 + (cp & 63));
}

// the pixel grid w x h: w a multiple of 2 and h of 3 in sextant mode
function vid_init(int w, int h) -> void {
	ESC = "\x1b";
	RESET = ESC + "[0m";
	HALFG = utf8(0x2580);
	SEXTG = [];
	for (int b = 0; b < 64; ++b) {
		if (b == 0) { SEXTG.push(" "); }
		else if (b == 21) { SEXTG.push(utf8(0x258C)); }        // left half
		else if (b == 42) { SEXTG.push(utf8(0x2590)); }        // right half
		else if (b == 63) { SEXTG.push(utf8(0x2588)); }        // full
		else { SEXTG.push(utf8(0x1FB00 + b - 1 - (b > 21 ? 1 : 0) - (b > 42 ? 1 : 0))); }
	}
	HOME = ESC + "[H";
	CLEAR = ESC + "[2J" + ESC + "[H";
	vid_resize(w, h);
	PAL_KEYS = {};
	PAL_FG = [];
	PAL_BG = [];
	PAL_LUM = [];
	PAL_R = [];
	PAL_G = [];
	PAL_B = [];
	PAL_BGR3 = [];
	PAL_CR = [];
	PAL_CG = [];
	PAL_CB = [];
	PAL_CK = [];
	FG24 = ESC + "[38;2;";
	BG24 = ESC + "[48;2;";
	DEC = [];
	DEC_SEMI = [];
	for (int v = 0; v < 256; ++v) { DEC.push(to_string(v)); DEC_SEMI.push(";" + to_string(v)); }
	gamma_table();
	rgb_idx(0, 0, 0);
}

// the grid for a screen of cols x rows cells
function vid_init_cells(int cols, int rows) -> void { vid_init(cols * cell_w(), rows * cell_h()); }

// a new grid size (and cell shape) keeping the palette intern, whose indices the loaded map's tables hold
function vid_resize(int w, int h) -> void {
	PIXEL_Y = SEXTANTS ? 0.75 : 1.0;
	VW = w;
	VH = h;
	VIEWH = h;
	PIX = [];
	for (int i = 0; i < w * h; ++i) { PIX.push(0); }
	ROWIDX = [];
	int vrows = h / cell_h();
	for (int r = 0; r < vrows; ++r) { ROWIDX.push(r); }
	WIPE_TICS = 0;
}

function cell_w() -> int { return SEXTANTS ? 2 : 1; }
function cell_h() -> int { return SEXTANTS ? 3 : 2; }

// a vertical size given in square (pixel-width) units, in pixel rows
function pixel_rows(float v) -> int { return math::itrunc(v * PIXEL_Y); }

function rgb_idx(int r, int g, int b) -> int {
	int key = r * 65536 + g * 256 + b;
	if (PAL_KEYS.has(key)) { return PAL_KEYS[key]; }
	int idx = PAL_FG.size();
	PAL_KEYS[key] = idx;
	int cr = GAMMA_LUT[r];
	int cg = GAMMA_LUT[g];
	int cb = GAMMA_LUT[b];
	PAL_CR.push(cr);
	PAL_CG.push(cg);
	PAL_CB.push(cb);
	PAL_CK.push((cr << 16) | (cg << 8) | cb);
	PAL_FG.push(sgr_color(38, cr, cg, cb));
	PAL_BG.push(sgr_color(48, cr, cg, cb));
	PAL_LUM.push(r * 2 + g * 3 + b);
	PAL_R.push(r);
	PAL_G.push(g);
	PAL_B.push(b);
	PAL_BGR3.push(`${to_char(b)}${to_char(g)}${to_char(r)}`);
	return idx;
}

function gamma_table() -> void {
	GAMMA_LUT = [];
	float e = 1.0 / (1.0 + 0.25 * GAMMA);
	for (int v = 0; v < 256; ++v) { GAMMA_LUT.push(math::itrunc(255.0 * math::pow(v / 255.0, e) + 0.5)); }
}

// a new gamma for every color interned so far; the indices stay
function vid_set_gamma(float g) -> void {
	GAMMA = g < 0.0 ? 0.0 : (g > 4.0 ? 4.0 : g);
	gamma_table();
	for (int i = 0; i < PAL_FG.size(); ++i) {
		int cr = GAMMA_LUT[PAL_R[i]];
		int cg = GAMMA_LUT[PAL_G[i]];
		int cb = GAMMA_LUT[PAL_B[i]];
		PAL_CR[i] = cr;
		PAL_CG[i] = cg;
		PAL_CB[i] = cb;
		PAL_CK[i] = (cr << 16) | (cg << 8) | cb;
		PAL_FG[i] = sgr_color(38, cr, cg, cb);
		PAL_BG[i] = sgr_color(48, cr, cg, cb);
	}
}

function sgr_color(int layer, int r, int g, int b) -> string {
	if (HOST_TRUECOLOR) { return `${ESC}[${layer};2;${r};${g};${b}m`; }
	return `${ESC}[${layer};5;${xterm256(r, g, b)}m`;
}

// xterm's 240 colors (16-255: the 6x6x6 cube, then the 24-step gray ramp) in OKLab, filled on first use
array<float> XTERM_LAB = [];

// the xterm color nearest in OKLab: plain RGB distance turns DOOM's browns olive and its dark greens gray
function xterm256(int r, int g, int b) -> int {
	if (XTERM_LAB.empty()) {
		array<int> levels = [0, 95, 135, 175, 215, 255];
		for (int c = 0; c < 216; ++c) { oklab_push(XTERM_LAB, levels[c / 36], levels[(c / 6) % 6], levels[c % 6]); }
		for (int k = 0; k < 24; ++k) { oklab_push(XTERM_LAB, 8 + 10 * k, 8 + 10 * k, 8 + 10 * k); }
	}
	array<float> want = [];
	oklab_push(want, r, g, b);
	int result = 0;
	float best = 1.0e30;
	for (int i = 0; i < 240; ++i) {
		float dl = want[0] - XTERM_LAB[i * 3];
		float da = want[1] - XTERM_LAB[i * 3 + 1];
		float db = want[2] - XTERM_LAB[i * 3 + 2];
		float d = dl * dl + da * da + db * db;
		if (d < best) { best = d; result = i; }
	}
	return 16 + result;
}

function srgb_linear(int v) -> float {
	float c = v / 255.0;
	if (c <= 0.04045) { return c / 12.92; }
	return math::pow((c + 0.055) / 1.055, 2.4);
}

function oklab_push(array<float>& out, int r8, int g8, int b8) -> void {
	float r = srgb_linear(r8);
	float g = srgb_linear(g8);
	float b = srgb_linear(b8);
	float l = math::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
	float m = math::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
	float s = math::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
	out.push(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s);
	out.push(1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s);
	out.push(0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s);
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
	string row = cy < SKIP_ROWS ? SKIP_MOVE : "";
	int vw = VW;
	int fg = -1;
	int bg = -1;
	int ti = cy * 2 * vw;
	int bi = ti + vw;
	for (int x = cy < SKIP_ROWS ? SKIP_CELLS : 0; x < vw; ++x) {
		int top = PIX[ti + x];
		int bot = PIX[bi + x];
		if (top != fg) { row += PAL_FG[top]; fg = top; }
		if (bot != bg) { row += PAL_BG[bot]; bg = bot; }
		row += HALFG;
	}
	return row + RESET;
}

// sext mode: 2x3 px per cell, GLOOM's cell fit: the brightest pixel is fg, the darkest bg, and each pixel takes the one
// nearer in luminance. The same capture rules as vid_row_half.
function vid_row_sext(int cy) -> string {
	string row = cy < SKIP_ROWS ? SKIP_MOVE : "";
	int vw = VW;
	int fg = -1;
	int bg = -1;
	int r0 = cy * 3 * vw;
	int r1 = r0 + vw;
	int r2 = r1 + vw;
	for (int x = cy < SKIP_ROWS ? SKIP_CELLS * 2 : 0; x < vw; x += 2) {
		int p0 = PIX[r0 + x];
		int p1 = PIX[r0 + x + 1];
		int p2 = PIX[r1 + x];
		int p3 = PIX[r1 + x + 1];
		int p4 = PIX[r2 + x];
		int p5 = PIX[r2 + x + 1];
		if (p0 == p1 && p0 == p2 && p0 == p3 && p0 == p4 && p0 == p5) {
			if (p0 != bg) { row += PAL_BG[p0]; bg = p0; }
			row += " ";
			continue;
		}
		int l0 = PAL_LUM[p0];
		int l1 = PAL_LUM[p1];
		int l2 = PAL_LUM[p2];
		int l3 = PAL_LUM[p3];
		int l4 = PAL_LUM[p4];
		int l5 = PAL_LUM[p5];
		int lmin = l0;
		int lmax = l0;
		int fgp = p0;
		int bgp = p0;
		if (l1 < lmin) { lmin = l1; bgp = p1; }
		if (l1 > lmax) { lmax = l1; fgp = p1; }
		if (l2 < lmin) { lmin = l2; bgp = p2; }
		if (l2 > lmax) { lmax = l2; fgp = p2; }
		if (l3 < lmin) { lmin = l3; bgp = p3; }
		if (l3 > lmax) { lmax = l3; fgp = p3; }
		if (l4 < lmin) { lmin = l4; bgp = p4; }
		if (l4 > lmax) { lmax = l4; fgp = p4; }
		if (l5 < lmin) { lmin = l5; bgp = p5; }
		if (l5 > lmax) { lmax = l5; fgp = p5; }
		int thr = (lmin + lmax + 1) / 2;
		int bits = 0;
		if (l0 >= thr) { bits = 1; }
		if (l1 >= thr) { bits += 2; }
		if (l2 >= thr) { bits += 4; }
		if (l3 >= thr) { bits += 8; }
		if (l4 >= thr) { bits += 16; }
		if (l5 >= thr) { bits += 32; }
		if (fgp != fg) { row += PAL_FG[fgp]; fg = fgp; }
		if (bits != 63 && bgp != bg) { row += PAL_BG[bgp]; bg = bgp; }
		row += SEXTG[bits];
	}
	return row + RESET;
}

// sext mode, 24-bit: the same luminance split, each side drawn in the mean color of its pixels, the cell's average
// tone instead of its two extremes. A side of one palette color takes that color's interned escape.
function vid_row_sext_mean(int cy) -> string {
	string row = cy < SKIP_ROWS ? SKIP_MOVE : "";
	int vw = VW;
	int fgk = -1;
	int bgk = -1;
	int r0 = cy * 3 * vw;
	int r1 = r0 + vw;
	int r2 = r1 + vw;
	for (int x = cy < SKIP_ROWS ? SKIP_CELLS * 2 : 0; x < vw; x += 2) {
		int p0 = PIX[r0 + x];
		int p1 = PIX[r0 + x + 1];
		int p2 = PIX[r1 + x];
		int p3 = PIX[r1 + x + 1];
		int p4 = PIX[r2 + x];
		int p5 = PIX[r2 + x + 1];
		if (p0 == p1 && p0 == p2 && p0 == p3 && p0 == p4 && p0 == p5) {
			int k = PAL_CK[p0];
			if (k != bgk) { row += PAL_BG[p0]; bgk = k; }
			row += " ";
			continue;
		}
		int l0 = PAL_LUM[p0];
		int l1 = PAL_LUM[p1];
		int l2 = PAL_LUM[p2];
		int l3 = PAL_LUM[p3];
		int l4 = PAL_LUM[p4];
		int l5 = PAL_LUM[p5];
		int lmin = l0;
		int lmax = l0;
		if (l1 < lmin) { lmin = l1; } else if (l1 > lmax) { lmax = l1; }
		if (l2 < lmin) { lmin = l2; } else if (l2 > lmax) { lmax = l2; }
		if (l3 < lmin) { lmin = l3; } else if (l3 > lmax) { lmax = l3; }
		if (l4 < lmin) { lmin = l4; } else if (l4 > lmax) { lmax = l4; }
		if (l5 < lmin) { lmin = l5; } else if (l5 > lmax) { lmax = l5; }
		int thr = lmin == lmax ? lmax + 1 : (lmin + lmax + 1) / 2;
		int bits = 0;
		int fr = 0; int fg = 0; int fb = 0; int fn = 0; int fi = -1; bool fmix = false;
		int br = 0; int bg = 0; int bb = 0; int bi = -1; bool bmix = false;
		if (l0 >= thr) {
			bits += 1;
			fr += PAL_CR[p0]; fg += PAL_CG[p0]; fb += PAL_CB[p0]; fn += 1;
			if (fi < 0) { fi = p0; } else if (fi != p0) { fmix = true; }
		} else {
			br += PAL_CR[p0]; bg += PAL_CG[p0]; bb += PAL_CB[p0];
			if (bi < 0) { bi = p0; } else if (bi != p0) { bmix = true; }
		}
		if (l1 >= thr) {
			bits += 2;
			fr += PAL_CR[p1]; fg += PAL_CG[p1]; fb += PAL_CB[p1]; fn += 1;
			if (fi < 0) { fi = p1; } else if (fi != p1) { fmix = true; }
		} else {
			br += PAL_CR[p1]; bg += PAL_CG[p1]; bb += PAL_CB[p1];
			if (bi < 0) { bi = p1; } else if (bi != p1) { bmix = true; }
		}
		if (l2 >= thr) {
			bits += 4;
			fr += PAL_CR[p2]; fg += PAL_CG[p2]; fb += PAL_CB[p2]; fn += 1;
			if (fi < 0) { fi = p2; } else if (fi != p2) { fmix = true; }
		} else {
			br += PAL_CR[p2]; bg += PAL_CG[p2]; bb += PAL_CB[p2];
			if (bi < 0) { bi = p2; } else if (bi != p2) { bmix = true; }
		}
		if (l3 >= thr) {
			bits += 8;
			fr += PAL_CR[p3]; fg += PAL_CG[p3]; fb += PAL_CB[p3]; fn += 1;
			if (fi < 0) { fi = p3; } else if (fi != p3) { fmix = true; }
		} else {
			br += PAL_CR[p3]; bg += PAL_CG[p3]; bb += PAL_CB[p3];
			if (bi < 0) { bi = p3; } else if (bi != p3) { bmix = true; }
		}
		if (l4 >= thr) {
			bits += 16;
			fr += PAL_CR[p4]; fg += PAL_CG[p4]; fb += PAL_CB[p4]; fn += 1;
			if (fi < 0) { fi = p4; } else if (fi != p4) { fmix = true; }
		} else {
			br += PAL_CR[p4]; bg += PAL_CG[p4]; bb += PAL_CB[p4];
			if (bi < 0) { bi = p4; } else if (bi != p4) { bmix = true; }
		}
		if (l5 >= thr) {
			bits += 32;
			fr += PAL_CR[p5]; fg += PAL_CG[p5]; fb += PAL_CB[p5]; fn += 1;
			if (fi < 0) { fi = p5; } else if (fi != p5) { fmix = true; }
		} else {
			br += PAL_CR[p5]; bg += PAL_CG[p5]; bb += PAL_CB[p5];
			if (bi < 0) { bi = p5; } else if (bi != p5) { bmix = true; }
		}
		if (bi >= 0) {
			if (!bmix) {
				int k = PAL_CK[bi];
				if (k != bgk) { row += PAL_BG[bi]; bgk = k; }
			} else {
				int bn = 6 - fn;
				int mr = br / bn; int mg = bg / bn; int mb = bb / bn;
				int k = (mr << 16) | (mg << 8) | mb;
				if (k != bgk) { row += BG24 + DEC[mr] + DEC_SEMI[mg] + DEC_SEMI[mb] + "m"; bgk = k; }
			}
		}
		if (fi < 0) {
		} else if (!fmix) {
			int k = PAL_CK[fi];
			if (k != fgk) { row += PAL_FG[fi]; fgk = k; }
		} else {
			int mr = fr / fn; int mg = fg / fn; int mb = fb / fn;
			int k = (mr << 16) | (mg << 8) | mb;
			if (k != fgk) { row += FG24 + DEC[mr] + DEC_SEMI[mg] + DEC_SEMI[mb] + "m"; fgk = k; }
		}
		row += SEXTG[bits];
	}
	return row + RESET;
}

// the frame as terminal text, one line per glyph row
function vid_blit() -> string {
	var row_fn = SEXTANTS ? (SEXT_FIT == 1 && HOST_TRUECOLOR ? vid_row_sext_mean : vid_row_sext) : vid_row_half;
	if (BLIT_WORKERS != 0) {
		var rows = parallel_transform(ROWIDX, row_fn);
		return VID_INDENT + rows.join("\n" + VID_INDENT) + "\n";
	}
	string out = "";
	int vrows = ROWIDX.size();
	for (int cy = 0; cy < vrows; ++cy) { out += VID_INDENT + row_fn(cy) + "\n"; }
	return out;
}
