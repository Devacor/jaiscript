// Gator Quest - terminal video: a palette-index pixel canvas drawn with the
// upper-half-block glyph (fg = top pixel, bg = bottom pixel) in 24-bit
// color, escapes emitted only on change (the JaiDOOM/GLOOM method). Light
// and tint go through shade tables: SHADE[level * NBASE + color].

import "gq_proto.jai";

string ESC = "\x1b";
string RESET = "\x1b[0m";
string HALF = "\xe2\x96\x80";
string HOME = "\x1b[H";

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
	PAL_FG.push(`${ESC}[38;2;${r};${g};${b}m`);
	PAL_BG.push(`${ESC}[48;2;${r};${g};${b}m`);
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

// ---- shade tables: 16 light levels x NBASE, tinted toward a night color
int NLEV = 16;
array<int> SHADE;
array<int> GRAYSHADE;        // ghost vision: desaturated, cold
int SHADE_TINT_R;
int SHADE_TINT_G;
int SHADE_TINT_B;

void shade_build(int tr, int tg, int tb, bool ghost) {
	SHADE_TINT_R = tr;
	SHADE_TINT_G = tg;
	SHADE_TINT_B = tb;
	SHADE = [];
	int n = NBASE;
	for (int lev = 0; lev < NLEV; ++lev) {
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
				// quantize a little so the palette stays small
				r = r & 0xFC;
				g = g & 0xFC;
				b = b & 0xFC;
			}
			SHADE.push(lev == NLEV - 1 && !ghost ? c : rgb(r, g, b));
		}
	}
}

// shade any palette color: colors interned after the tables were built pass through unshaded
int shade_px(int lev, int c) { return c < NBASE ? SHADE[lev * NBASE + c] : c; }

// ---- the 4x4 Bayer matrix for dithering between light levels
array<int> BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];

// ---- a 3x5 pixel font (digits, letters, a few marks) for floating numbers and banners
array<string> FONT_CHARS = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N",
	"O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "+", "-", "!", "?", ".", " ", ":", "'", "~"];
array<string> FONT_BITS = [
	"111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
	"111100111001111", "111100111101111", "111001010010010", "111101111101111", "111101111001111",
	"010101111101101", "110101110101110", "011100100100011", "110101101101110", "111100110100111",
	"111100110100100", "011100101101011", "101101111101101", "111010010010111", "001001001101010",
	"101101110101101", "100100100100111", "101111111101101", "110101101101101", "010101101101010",
	"110101110100100", "010101101111011", "110101110101101", "011100010001110", "111010010010010",
	"101101101101111", "101101101101010", "101101111111101", "101101010101101", "101101010010010",
	"111001010100111", "000010111010000", "000000111000000", "010010010000010", "111001010000010",
	"000000000000010", "000000000000000", "000010000010000", "010010000000000", "000011110000000"
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

void pane_begin(int width) {
	PANE_ROW = "";
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
	PANE_ROW += ESC + "[" + sgr + "m" + t;
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

// a bar of `width` cells: filled part in one color, rest dim
string bar(int value, int maxv, int width, string fill_sgr, string empty_sgr) {
	int n = maxv > 0 ? iclamp(value * width / maxv, 0, width) : 0;
	if (value > 0 && n == 0) { n = 1; }
	string full = string::repeat("\xe2\x96\x88", n);
	string empty = string::repeat("\xe2\x96\x91", width - n);
	return ESC + "[" + fill_sgr + "m" + full + ESC + "[" + empty_sgr + "m" + empty;
}
