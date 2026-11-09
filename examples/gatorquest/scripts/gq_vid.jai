// Gator Quest - terminal video: a palette-index pixel canvas drawn with the
// upper-half-block glyph (fg = top pixel, bg = bottom pixel) in 24-bit or
// xterm-256 color, escapes emitted only on change (the JaiDOOM/GLOOM method).
// Light and tint go through shade tables: SHADE[level * NBASE + color].

import "gq_proto.jai";

string ESC = "\x1b";
string RESET = "\x1b[0m";
string HALF = "\xe2\x96\x80";
string HOME = "\x1b[H";

// 24-bit escapes, or the xterm-256 palette (macOS Terminal.app reads a 24-bit escape as a run of unrelated attributes);
// set by main before the palette is built
bool HOST_TRUECOLOR;

bool terminal_says_truecolor() {
	return env_var("COLORTERM") == "truecolor" || env_var("COLORTERM") == "24bit" || env_var("WT_SESSION") != "" ||
		env_var("OS") == "Windows_NT";
}

string sgr_color(int layer, int r, int g, int b) {
	if (HOST_TRUECOLOR) { return `${ESC}[${layer};2;${r};${g};${b}m`; }
	return `${ESC}[${layer};5;${xterm256(r, g, b)}m`;
}

// xterm's 240 colors (16-255: the 6x6x6 cube, then the 24-step gray ramp) in OKLab, filled on first use
array<float> XTERM_LAB;

// the xterm color nearest in OKLab: plain RGB distance turns the swamp's olives gray and its browns red
int xterm256(int r, int g, int b) {
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

float srgb_linear(int v) {
	float c = v / 255.0;
	if (c <= 0.04045) { return c / 12.92; }
	return math::pow((c + 0.055) / 1.055, 2.4);
}

void oklab_push(array<float>& out, int r8, int g8, int b8) {
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

// a pane's SGR parameters ("1;38;2;r;g;b;48;2;r;g;b") with each 24-bit color as its xterm-256 one when the host lacks 24-bit
map<string, string> SGR_256;

string sgr_params(string sgr) {
	if (HOST_TRUECOLOR) { return sgr; }
	if (SGR_256.has(sgr)) { return SGR_256[sgr]; }
	array<string> p = sgr.split(";");
	string result = "";
	int i = 0;
	while (i < p.size()) {
		string part = p[i];
		if ((part == "38" || part == "48") && i + 4 < p.size() && p[i + 1] == "2") {
			part = `${part};5;${xterm256(p[i + 2].to_int(0), p[i + 3].to_int(0), p[i + 4].to_int(0))}`;
			i = i + 4;
		}
		result += result == "" ? part : ";" + part;
		i = i + 1;
	}
	SGR_256[sgr] = result;
	return result;
}

// the palette: base colors (textures, sprites) are interned first; shaded
// outputs are interned after them and never shaded again
map<int, int> PAL_KEY;
array<string> PAL_FG;
array<string> PAL_BG;
array<int> PAL_R;
array<int> PAL_G;
array<int> PAL_B;
int NBASE;

int rgb(int r, int g, int b) {
	r = iclamp(r, 0, 255);
	g = iclamp(g, 0, 255);
	b = iclamp(b, 0, 255);
	int key = r * 65536 + g * 256 + b;
	if (PAL_KEY.has(key)) { return PAL_KEY[key]; }
	int i = PAL_FG.size();
	PAL_KEY[key] = i;
	PAL_FG.push(sgr_color(38, r, g, b));
	PAL_BG.push(sgr_color(48, r, g, b));
	PAL_R.push(r);
	PAL_G.push(g);
	PAL_B.push(b);
	return i;
}

void pal_reset() {
	PAL_KEY = {};
	PAL_FG = [];
	PAL_BG = [];
	PAL_R = [];
	PAL_G = [];
	PAL_B = [];
	rgb(0, 0, 0);
}

// ---- the canvas
array<int> PIX;
int VW;
int VH;
int COL_BLACK;
int COL_WHITE;

void canvas_init(int w, int h) {
	VW = w;
	VH = h - h % 2;
	PIX = [];
	for (int i = 0; i < VW * VH; ++i) { PIX.push(0); }
}

void pset(int x, int y, int c) {
	if (x >= 0 && y >= 0 && x < VW && y < VH) { PIX[y * VW + x] = c; }
}

void fill_px(int x0, int y0, int w, int h, int c) {
	for (int y = imax(0, y0); y < imin(VH, y0 + h); ++y) {
		for (int x = imax(0, x0); x < imin(VW, x0 + w); ++x) { PIX[y * VW + x] = c; }
	}
}

// ---- shade tables: 16 light levels x NBASE, tinted toward a night color, then the same 16 in a cloud's shadow
int NLEV = 16;
array<int> SHADE;
array<int> GRAYSHADE;        // ghost vision: desaturated, cold
int SHADE_TINT_R;
int SHADE_TINT_G;
int SHADE_TINT_B;

// warm_pct scales the warm middle levels (torchlight, dawn, dusk), day_warm warms full light (outdoor daylight)
void shade_build(int tr, int tg, int tb, bool ghost, int warm_pct, int day_warm) {
	SHADE_TINT_R = tr;
	SHADE_TINT_G = tg;
	SHADE_TINT_B = tb;
	SHADE = [];
	int n = NBASE;
	for (int lev = 0; lev < NLEV; ++lev) {
		// shadows stay cool, the middle levels glow amber, peaking at level 11
		int warm = lev == NLEV - 1 ? day_warm : iclamp(100 - iabs(lev - 11) * 22, 0, 100) * warm_pct / 100;
		if (ghost) { warm = 0; }
		for (int c = 0; c < n; ++c) {
			int r = PAL_R[c];
			int g = PAL_G[c];
			int b = PAL_B[c];
			if (ghost) {
				int lum = (r * 3 + g * 5 + b * 2) / 10;
				r = lum * 3 / 4;
				g = lum * 4 / 5 + 10;
				b = lum + 30;
			}
			if (lev < NLEV - 1) {
				// darker levels fade toward the tint (night blue, cave brown, lava red...)
				r = (r * lev + tr * (NLEV - 1 - lev)) / (NLEV - 1);
				g = (g * lev + tg * (NLEV - 1 - lev)) / (NLEV - 1);
				b = (b * lev + tb * (NLEV - 1 - lev)) / (NLEV - 1);
			}
			if (warm > 0) {
				r = r * (100 + warm * 10 / 100) / 100 + warm / 7;
				g = g * (100 + warm * 4 / 100) / 100 + warm / 30;
				b = b * (100 - warm * 17 / 100) / 100;
			}
			if (lev < NLEV - 1 || warm > 0) {
				// quantize a little so the palette stays small
				r = iclamp(r, 0, 255) & 0xFC;
				g = iclamp(g, 0, 255) & 0xFC;
				b = iclamp(b, 0, 255) & 0xFC;
			}
			SHADE.push(lev == NLEV - 1 && !ghost && warm == 0 ? c : rgb(r, g, b));
		}
	}
	// levels NLEV.. repeat the light under a passing cloud's shadow: darker and cooler
	for (int i = 0; i < NLEV * n; ++i) {
		int s = SHADE[i];
		SHADE.push(ghost ? s : rgb(PAL_R[s] * 80 / 100, PAL_G[s] * 84 / 100, PAL_B[s] * 91 / 100 + 4));
	}
}

// shade any palette color: colors interned after the tables were built pass through unshaded
int shade_px(int lev, int c) { return c < NBASE ? SHADE[lev * NBASE + c] : c; }

// ---- the 4x4 Bayer matrix for dithering between light levels
array<int> BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];

// ---- a 3x5 pixel font (digits, letters, a few marks) for floating numbers and banners
array<string> FONT_CHARS = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N",
	"O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "+", "-", "!", "?", ".", " ", ":", "'", "~", "_", "/", ",", "*"];
array<string> FONT_BITS = [
	"111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
	"111100111001111", "111100111101111", "111001010010010", "111101111101111", "111101111001111",
	"010101111101101", "110101110101110", "011100100100011", "110101101101110", "111100110100111",
	"111100110100100", "011100101101011", "101101111101101", "111010010010111", "001001001101010",
	"101101110101101", "100100100100111", "101111111101101", "110101101101101", "010101101101010",
	"110101110100100", "010101101111011", "110101110101101", "011100010001110", "111010010010010",
	"101101101101111", "101101101101010", "101101111111101", "101101010101101", "101101010010010",
	"111001010100111", "000010111010000", "000000111000000", "010010010000010", "111001010000010",
	"000000000000010", "000000000000000", "000010000010000", "010010000000000", "000011110000000",
	"000000000000111", "001001010100100", "000000000010100", "000101010101000"
];
map<string, int> FONT_INDEX;

void font_init() {
	FONT_INDEX = {};
	for (int i = 0; i < FONT_CHARS.size(); ++i) { FONT_INDEX[FONT_CHARS[i]] = i; }
}

int text_px_width(string s, int scale) { return s.size() * 4 * scale; }

// draw text in the pixel font; `outline` >= 0 draws a 1px dark rim for legibility
void draw_text_px(string s, int x, int y, int col, int outline, int scale) {
	string up = string::to_upper(s);
	for (int i = 0; i < up.size(); ++i) {
		string ch = up.substr(i, 1);
		if (!FONT_INDEX.has(ch)) { continue; }
		string bits = FONT_BITS[FONT_INDEX[ch]];
		int ox = x + i * 4 * scale;
		for (int r = 0; r < 5; ++r) {
			for (int c = 0; c < 3; ++c) {
				if (bits[r * 3 + c] != '1') { continue; }
				for (int sy = 0; sy < scale; ++sy) {
					for (int sx = 0; sx < scale; ++sx) {
						int px = ox + c * scale + sx;
						int py = y + r * scale + sy;
						if (outline >= 0) {
							pset(px - 1, py, outline);
							pset(px + 1, py, outline);
							pset(px, py - 1, outline);
							pset(px, py + 1, outline);
						}
					}
				}
			}
		}
		for (int r = 0; r < 5; ++r) {
			for (int c = 0; c < 3; ++c) {
				if (bits[r * 3 + c] != '1') { continue; }
				for (int sy = 0; sy < scale; ++sy) {
					for (int sx = 0; sx < scale; ++sx) { pset(ox + c * scale + sx, y + r * scale + sy, col); }
				}
			}
		}
	}
}

// ---- one glyph row of the canvas as a string, escapes only on change
string canvas_row(int cy) {
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
		row += HALF;
	}
	return row + RESET;
}

// ---- text panes: a row builder that tracks visible width
string PANE_ROW;
int PANE_W;
int PANE_MAX;
string SGR_PANE_BG = "48;2;20;26;22";

// every pane row starts on the panel's own dark ground
void pane_begin(int width) {
	PANE_ROW = ESC + "[" + sgr_params(SGR_PANE_BG) + "m";
	PANE_W = 0;
	PANE_MAX = width;
}

// add text in an SGR color ("97", "38;2;r;g;b" ...); clipped to the pane width
void pane_add(string sgr, string text) {
	int room = PANE_MAX - PANE_W;
	if (room <= 0) { return; }
	int len = text.utf8_length();
	string t = text;
	if (len > room) {
		utf8_string u = text;
		t = u.substr(0, room);
		len = room;
	}
	PANE_ROW += ESC + "[" + sgr_params(sgr) + "m" + t;
	PANE_W = PANE_W + len;
}

string pane_end() {
	string pad = "";
	if (PANE_W < PANE_MAX) { pad = string::repeat(" ", PANE_MAX - PANE_W); }
	return PANE_ROW + pad + RESET;
}

string pane_line(string sgr, string text, int width) {
	pane_begin(width);
	pane_add(sgr, text);
	return pane_end();
}

// a bar of `width` cells: filled part in one color, the rest a solid dark track
string bar(int value, int maxv, int width, string fill_sgr, string empty_sgr) {
	int n = maxv > 0 ? iclamp(value * width / maxv, 0, width) : 0;
	if (value > 0 && n == 0) { n = 1; }
	string full = string::repeat("\xe2\x96\x88", n);
	string empty = string::repeat("\xe2\x96\x88", width - n);
	return ESC + "[" + sgr_params(fill_sgr) + "m" + full + ESC + "[" + sgr_params(empty_sgr) + "m" + empty;
}
