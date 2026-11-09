// JaiDOOM fx.jai — presentation past the 1993 original: the PlayStation port's title fire, burning up over TITLEPIC.
// Its own LCG (the wipe_rnd pattern): nothing here touches SIM_RANDOM, so demos and netplay never see it.

import "vid.jai";
import "tune.jai";

array<int> FIRE = [];         // heat 0..36 per pixel, VW x VH
array<int> FIRE_PAL = [];     // heat -> palette index
int FIRE_W = 0;
int FIRE_H = 0;
int FIRE_SEED = 1;

function fire_rnd() -> int {
	FIRE_SEED = (FIRE_SEED * 1103515245 + 12345) & 2147483647;
	return FIRE_SEED >> 8;
}

// the PSX fire's 37 heats, black through red and yellow to white
function fire_palette() -> void {
	array<int> rgb = [7, 7, 7, 31, 7, 7, 47, 15, 7, 71, 15, 7, 87, 23, 7, 103, 31, 7, 119, 31, 7, 143, 39, 7, 159, 47, 7,
		175, 63, 7, 191, 71, 7, 199, 71, 7, 223, 79, 7, 223, 87, 7, 223, 87, 7, 215, 95, 7, 215, 95, 7, 215, 103, 15,
		207, 111, 15, 207, 119, 15, 207, 127, 15, 207, 135, 23, 199, 135, 23, 199, 143, 23, 199, 151, 31, 191, 159, 31,
		191, 159, 31, 191, 167, 39, 191, 167, 39, 191, 175, 47, 183, 175, 47, 183, 183, 47, 183, 183, 55, 207, 207, 111,
		223, 223, 159, 239, 239, 199, 255, 255, 255];
	FIRE_PAL = [];
	for (int i = 0; i < 37; ++i) { FIRE_PAL.push(rgb_idx(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2])); }
}

// a cold grid with its bottom row lit, again whenever the screen changes size
function fire_start(int seed) -> void {
	if (FIRE_PAL.empty()) { fire_palette(); }
	FIRE_W = VW;
	FIRE_H = VH;
	FIRE_SEED = seed | 1;
	FIRE = [];
	for (int i = 0; i < FIRE_W * FIRE_H; ++i) { FIRE.push(0); }
	int base = (FIRE_H - 1) * FIRE_W;
	for (int x = 0; x < FIRE_W; ++x) { FIRE[base + x] = 36; }
}

// one spread: each pixel's heat rises a row, drifting up to a pixel sideways and cooling by chance. The cooling odds
// scale with the screen so the flames reach the same share of it at any size.
function fire_tic() -> void {
	if (FIRE_W != VW || FIRE_H != VH) { fire_start(FIRE_SEED); }
	int w = FIRE_W;
	int h = FIRE_H;
	// a mean loss of 36 heats over the reach: one step at odds cool, and 3 more at odds 1 in 8 (the ragged tongues)
	int cool = math::itrunc(65536.0 * (36.0 / (h * FIRE_REACH + 1.0) - 0.375));
	if (cool < 0) { cool = 0; }
	if (cool > 65536) { cool = 65536; }
	for (int y = 1; y < h; ++y) {
		int row = y * w;
		for (int x = 0; x < w; ++x) {
			int heat = FIRE[row + x];
			int to = row - w + x;
			if (heat == 0) { FIRE[to] = 0; continue; }
			int r = fire_rnd();
			int drift = (r & 3) - 1;
			int dx = x - drift;
			if (dx < 0) { dx = 0; }
			if (dx >= w) { dx = w - 1; }
			int lose = ((r >> 2) & 65535) < cool ? 1 : 0;
			if (((r >> 18) & 7) == 0) { lose += 3; }
			FIRE[row - w + dx] = heat > lose ? heat - lose : 0;
		}
	}
}

// the pixels at heat show or hotter over what is drawn, the coolest eight through a 4x4 ordered dither: the flames'
// edges thin to smoke and what is under them shows through
array<int> BAYER4 = [0, 4, 1, 5, 6, 2, 7, 3, 1, 5, 0, 4, 7, 3, 6, 2];

function fire_draw(int show) -> void {
	if (FIRE_W * FIRE_H != VW * VH) { return; }
	int w = FIRE_W;
	for (int y = 0; y < FIRE_H; ++y) {
		int row = y * w;
		int brow = (y & 3) * 4;
		for (int x = 0; x < w; ++x) {
			int heat = FIRE[row + x];
			if (heat < show || heat - show < BAYER4[brow + (x & 3)]) { continue; }
			PIX[row + x] = FIRE_PAL[heat];
		}
	}
}

function fire_hash() -> int {
	int result = 17;
	for (int i = 0; i < FIRE.size(); ++i) { result = (result * 31 + FIRE[i]) & 1073741823; }
	return result;
}

// ------------------------------------------------------------- menu backdrop ---
// the menu opened mid-game draws over the paused frame, dimmed; from the title it draws over the fire on black
array<int> BACKDROP = [];
array<int> DIM = [];          // palette index -> its dimmed shade, -1 until asked
float DIM_LEVEL = -1.0;

function dim_of(int c) -> int {
	if (DIM_LEVEL != BACKDROP_DIM) { DIM = []; DIM_LEVEL = BACKDROP_DIM; }
	while (DIM.size() <= c) { DIM.push(-1); }
	if (DIM[c] < 0) {
		float k = BACKDROP_DIM;
		DIM[c] = rgb_idx(math::itrunc(PAL_R[c] * k), math::itrunc(PAL_G[c] * k), math::itrunc(PAL_B[c] * k));
	}
	return DIM[c];
}

function backdrop_take() -> void {
	BACKDROP = PIX;
	for (int i = 0; i < BACKDROP.size(); ++i) { BACKDROP[i] = dim_of(BACKDROP[i]); }
}

function backdrop_draw() -> void {
	if (BACKDROP.size() == VW * VH) { PIX = BACKDROP; return; }
	pix_clear(0);
	fire_draw(1);
}

// ------------------------------------------------------------- JaiScript panel ---
// J shows what the script engine is doing each frame, in the terminal's own font over the top-left of the view:
// the time each stage takes with its recent history, the rows the parallel blit spreads over the workers (P switches
// it off and on live), and tune.jai's reloads.
bool PANEL_ON = false;
int PANEL_SPAN = 24;          // frames of history per stage
array<float> ST_SIM = [];
array<float> ST_VIEW = [];
array<float> ST_BLIT = [];
array<float> ST_TERM = [];
float ST_FRAME = 0.0;
int TUNE_RELOADS = 0;
string TUNE_NOTE = "";        // the last reload's outcome
array<string> SPARK = [];
int PANEL_W = 0;              // the last panel's size in cells
int PANEL_H = 0;

function stat_push(array<float>& ring, float ms) -> void {
	if (ring.size() < PANEL_SPAN) { ring.push(ms); return; }
	for (int i = 0; i + 1 < ring.size(); ++i) { ring[i] = ring[i + 1]; }
	ring[ring.size() - 1] = ms;
}

function stat_note(float sim, float view, float blit, float term, float frame) -> void {
	stat_push(ST_SIM, sim);
	stat_push(ST_VIEW, view);
	stat_push(ST_BLIT, blit);
	stat_push(ST_TERM, term);
	ST_FRAME = ST_FRAME <= 0.0 ? frame : ST_FRAME * 0.9 + frame * 0.1;
}

function stat_mean(array<float>& ring) -> float {
	if (ring.empty()) { return 0.0; }
	float result = 0.0;
	for (float v : ring) { result += v; }
	return result / ring.size();
}

// the history as block heights, full at top_ms
function spark(array<float>& ring, float top_ms) -> string {
	if (SPARK.empty()) { for (int i = 0; i < 8; ++i) { SPARK.push(utf8(0x2581 + i)); } }
	string result = "";
	for (int i = 0; i < PANEL_SPAN - ring.size(); ++i) { result += " "; }
	for (float v : ring) {
		int h = math::itrunc(v / top_ms * 7.99);
		if (h < 0) { h = 0; }
		if (h > 7) { h = 7; }
		result += SPARK[h];
	}
	return result;
}

function ms_text(float ms) -> string {
	string result = `${ms:.2}`;
	while (result.size() < 6) { result = " " + result; }
	return result;
}

// the panel's lines as terminal text, each placed by a cursor move; col and row are the view's top-left cell (1-based)
function panel_text(int row, int col) -> string {
	string bg = sgr_color(48, 14, 14, 24);
	string dim = sgr_color(38, 130, 130, 150);
	string hi = sgr_color(38, 240, 240, 250);
	string gold = sgr_color(38, 255, 190, 60);
	array<string> lines = [];
	lines.push(`${sgr_color(48, 255, 190, 60)}${sgr_color(38, 20, 14, 8)} JAISCRIPT ${bg}${gold} live, every frame ${dim}(J hides)  `);
	float top = 1.0;                                 // one scale for every stage, so their heights compare
	for (float v : ST_SIM) { if (v > top) { top = v; } }
	for (float v : ST_VIEW) { if (v > top) { top = v; } }
	for (float v : ST_BLIT) { if (v > top) { top = v; } }
	for (float v : ST_TERM) { if (v > top) { top = v; } }
	lines.push(`${dim} sim  ${hi}${ms_text(stat_mean(ST_SIM))} ms ${sgr_color(38, 90, 220, 110)}${spark(ST_SIM, top)} `);
	lines.push(`${dim} view ${hi}${ms_text(stat_mean(ST_VIEW))} ms ${sgr_color(38, 255, 140, 40)}${spark(ST_VIEW, top)} `);
	lines.push(`${dim} blit ${hi}${ms_text(stat_mean(ST_BLIT))} ms ${sgr_color(38, 70, 200, 255)}${spark(ST_BLIT, top)} `);
	lines.push(`${dim} term ${hi}${ms_text(stat_mean(ST_TERM))} ms ${sgr_color(38, 220, 90, 220)}${spark(ST_TERM, top)} `);
	int fps = ST_FRAME > 0.0 ? math::itrunc(1000.0 / ST_FRAME + 0.5) : 0;
	string par = BLIT_WORKERS != 0 ? `${hi}parallel_transform ${dim}${ROWIDX.size()} rows on ${thread_count()} threads` :
		`${hi}serial ${dim}${ROWIDX.size()} rows`;
	lines.push(`${dim} blit: ${par} ${dim}(P switches) `);
	lines.push(`${dim} ${hi}${fps} fps ${dim}${VW}x${VH} ${SEXTANTS ? "sextants" : "half blocks"}, ${PAL_FG.size()} colors, gamma ${GAMMA:.0} `);
	lines.push(`${dim} tune.jai ${hi}FOV ${FOV:.0} ${dim}reloaded ${hi}${TUNE_RELOADS}${dim}x ${TUNE_NOTE} `);
	int width = 0;
	array<int> widths = [];
	for (string l : lines) {
		int w = utf8_visible(l);
		widths.push(w);
		if (w > width) { width = w; }
	}
	PANEL_W = width;
	PANEL_H = lines.size();
	string result = "";
	for (int i = 0; i < lines.size(); ++i) {
		result += `${ESC}[${row + i};${col}H${bg}${lines[i]}${bg}`;
		for (int p = widths[i]; p < width; ++p) { result += " "; }
		result += RESET;
	}
	return result;
}

// the columns a line of styled text takes: escapes and utf8 continuation bytes take none
function utf8_visible(string s) -> int {
	int result = 0;
	bool esc = false;
	for (int i = 0; i < s.size(); ++i) {
		int b = s[i] + 0;
		if (esc) { if ((b >= 64 && b <= 126) && b != 91) { esc = false; } continue; }
		if (b == 27) { esc = true; continue; }
		if ((b & 192) != 128) { result += 1; }
	}
	return result;
}
