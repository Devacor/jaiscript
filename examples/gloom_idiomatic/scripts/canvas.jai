// A cell canvas for the showcase HUD and pages. Each cell is two stacked
// pixels (drawn as an upper half block, square on a 1:2 cell) or one character
// over the cell's upper pixel. Colors are palette indices, so the canvas
// follows the terminal's color depth and --ascii like the view does.

// ------------------------------------------------------------ pixel font ----
// 3x5 capitals, digits and a little punctuation; '#' is ink
map<string, array<string>> FONT5 = {
	"A": ["###", "#.#", "###", "#.#", "#.#"], "B": ["##.", "#.#", "##.", "#.#", "##."],
	"C": ["###", "#..", "#..", "#..", "###"], "D": ["##.", "#.#", "#.#", "#.#", "##."],
	"E": ["###", "#..", "##.", "#..", "###"], "F": ["###", "#..", "##.", "#..", "#.."],
	"G": ["###", "#..", "#.#", "#.#", "###"], "H": ["#.#", "#.#", "###", "#.#", "#.#"],
	"I": ["###", ".#.", ".#.", ".#.", "###"], "J": ["..#", "..#", "..#", "#.#", "###"],
	"K": ["#.#", "#.#", "##.", "#.#", "#.#"], "L": ["#..", "#..", "#..", "#..", "###"],
	"M": ["#.#", "###", "###", "#.#", "#.#"], "N": ["##.", "#.#", "#.#", "#.#", "#.#"],
	"O": ["###", "#.#", "#.#", "#.#", "###"], "P": ["###", "#.#", "###", "#..", "#.."],
	"Q": ["###", "#.#", "#.#", "###", "..#"], "R": ["##.", "#.#", "##.", "#.#", "#.#"],
	"S": ["###", "#..", "###", "..#", "###"], "T": ["###", ".#.", ".#.", ".#.", ".#."],
	"U": ["#.#", "#.#", "#.#", "#.#", "###"], "V": ["#.#", "#.#", "#.#", "#.#", ".#."],
	"W": ["#.#", "#.#", "###", "###", "#.#"], "X": ["#.#", "#.#", ".#.", "#.#", "#.#"],
	"Y": ["#.#", "#.#", ".#.", ".#.", ".#."], "Z": ["###", "..#", ".#.", "#..", "###"],
	"0": ["###", "#.#", "#.#", "#.#", "###"], "1": [".#.", "##.", ".#.", ".#.", "###"],
	"2": ["###", "..#", "###", "#..", "###"], "3": ["###", "..#", ".##", "..#", "###"],
	"4": ["#.#", "#.#", "###", "..#", "..#"], "5": ["###", "#..", "###", "..#", "###"],
	"6": ["###", "#..", "###", "#.#", "###"], "7": ["###", "..#", ".#.", ".#.", ".#."],
	"8": ["###", "#.#", "###", "#.#", "###"], "9": ["###", "#.#", "###", "..#", "###"],
	"%": ["#.#", "..#", ".#.", "#..", "#.#"], ":": ["...", ".#.", "...", ".#.", "..."],
	"/": ["..#", "..#", ".#.", "#..", "#.."], "-": ["...", "...", "###", "...", "..."],
	"!": [".#.", ".#.", ".#.", "...", ".#."], "'": [".#.", ".#.", "...", "...", "..."],
	".": ["...", "...", "...", "...", ".#."], " ": ["...", "...", "...", "...", "..."]
};

class Canvas {
public:
	int w = 0;
	int h = 0;                   // cells; pixel rows are 2 * h
	array<int> px = [];          // palette index per pixel, row-major, w x 2h
	array<string> glyph = [];    // "" or the character drawn in the cell
	array<int> ink = [];         // its color

	Canvas(int w_, int h_) {
		w = w_;
		h = h_;
		for (int i = 0; i < w_ * h_ * 2; ++i) { px.push(0); }
		for (int i = 0; i < w_ * h_; ++i) {
			glyph.push("");
			ink.push(0);
		}
	}

	void clear(int color) {
		for (int i = 0; i < px.size(); ++i) { px[i] = color; }
		for (int i = 0; i < glyph.size(); ++i) { glyph[i] = ""; }
	}

	void dot(int x, int y, int color) {
		if (x >= 0 && x < w && y >= 0 && y < h * 2) { px[y * w + x] = color; }
	}

	void rect(int x, int y, int rw, int rh, int color) {
		for (int yy = imax(0, y); yy < imin(h * 2, y + rh); ++yy) {
			for (int xx = imax(0, x); xx < imin(w, x + rw); ++xx) { px[yy * w + xx] = color; }
		}
	}

	// one color per pixel row from top to bottom, from rows y0 to y1 (exclusive)
	void gradient(int y0, int y1, math::color top, math::color bottom) {
		for (int y = imax(0, y0); y < imin(h * 2, y1); ++y) {
			int c = color_idx(math::mix_rgb(top, bottom, 1.0 * (y - y0) / imax(1, y1 - y0 - 1)));
			for (int x = 0; x < w; ++x) { px[y * w + x] = c; }
		}
	}

	// text at cell (x, row); characters past the edge are dropped
	void text(int x, int row, string s, int color) {
		if (row < 0 || row >= h) { return; }
		for (int i = 0; i < s.size(); ++i) {
			int cx = x + i;
			if (cx < 0 || cx >= w) { continue; }
			glyph[row * w + cx] = s.substr(i, 1);
			ink[row * w + cx] = color;
		}
	}

	void text_centered(int row, string s, int color) { text((w - s.size()) / 2, row, s, color); }

	void text_right(int x_end, int row, string s, int color) { text(x_end - s.size(), row, s, color); }

	// an Art (defs.jai) at pixel (x, y), through a code -> color table
	void art(shared_ptr<Art> a, int x, int y, array<int> lut) {
		for (int ay = 0; ay < a.h; ++ay) {
			for (int ax = 0; ax < a.w; ++ax) {
				int code = a.pix[ay * a.w + ax];
				if (code > 0) { dot(x + ax, y + ay, lut[code]); }
			}
		}
	}

	// s in the 3x5 font with its top-left at pixel (x, y), a color per font row, a shadow below right
	void lettering(string s, int x, int y, int scale, array<int> shades, int shadow) {
		for (int pass = 0; pass < 2; ++pass) {
			for (int i = 0; i < s.size(); ++i) {
				string ch = s.substr(i, 1);
				if (!FONT5.has(ch)) { continue; }
				auto& glyph_rows = FONT5[ch];
				int gx = x + i * 4 * scale;
				for (int ry = 0; ry < 5; ++ry) {
					for (int rx = 0; rx < 3; ++rx) {
						if (glyph_rows[ry].substr(rx, 1) != "#") { continue; }
						if (pass == 0 && shadow >= 0) { rect(gx + (rx + 1) * scale, y + (ry + 1) * scale, scale, scale, shadow); }
						if (pass == 1) { rect(gx + rx * scale, y + ry * scale, scale, scale, shades[ry]); }
					}
				}
			}
		}
	}

	// rows first .. last (cells) as terminal lines
	array<string> rows(int first, int last) {
		array<string> out = [];
		array<string> parts = [];
		for (int row = first; row < last; ++row) {
			parts.clear();
			int fg = -1;
			int bg = -1;
			for (int x = 0; x < w; ++x) {
				int cell = row * w + x;
				int upper = px[row * 2 * w + x];
				int lower = px[(row * 2 + 1) * w + x];
				string ch = glyph[cell];
				if (ch != "") {
					if (ink[cell] != fg) { fg = ink[cell]; parts.push(PAL_FG[fg]); }
					if (upper != bg) { bg = upper; parts.push(PAL_BG[bg]); }
					parts.push(ch);
				} else if (upper == lower || HOST_ASCII) {
					if (upper != bg) { bg = upper; parts.push(PAL_BG[bg]); }
					parts.push(" ");
				} else {
					if (upper != fg) { fg = upper; parts.push(PAL_FG[fg]); }
					if (lower != bg) { bg = lower; parts.push(PAL_BG[bg]); }
					parts.push(HALFG);
				}
			}
			parts.push(RESET);
			out.push(parts.join(""));
		}
		return out;
	}
}

// width in pixels of s in the 3x5 font at a scale, one pixel of spacing per letter
function lettering_width(string s, int scale) -> int { return s.size() * 4 * scale - scale; }
