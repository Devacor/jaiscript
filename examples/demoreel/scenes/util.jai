// demoreel util: palettes, the half-block presenter, transitions, the pixel font.
// Every scene paints a w x (2*rows) grid of palette indices; the presenter packs
// two pixels into each terminal cell through the upper-half-block glyph, the
// FOREGROUND color painting the top pixel and the BACKGROUND the bottom one, and
// emits an escape only when a color changes along the row (run-length).
// Uniform cells collapse to a space (bg run) or a full block (fg run).
//
// EVERYTHING IS VALUE-SEMANTIC in JaiScript: assignment, params, returns, and
// element reads all deep-copy. Mutation goes through var& reference params /
// ref declarations, or through the owning object's own fields.

class Palette {
	array<string> fg = [];
	array<string> bg = [];
	array<int> rgb = [];

	int add(float r, float g, float b) {
		int ir = math::itrunc(r < 0.0 ? 0.0 : (r > 255.0 ? 255.0 : r));
		int ig = math::itrunc(g < 0.0 ? 0.0 : (g > 255.0 ? 255.0 : g));
		int ib = math::itrunc(b < 0.0 ? 0.0 : (b > 255.0 ? 255.0 : b));
		string c = `${ir};${ig};${ib}m`;
		fg.push(ESC + "[38;2;" + c);
		bg.push(ESC + "[48;2;" + c);
		rgb.push(ir * 65536 + ig * 256 + ib);
		return rgb.size() - 1;
	}

	// n entries lerped from (r0,g0,b0) to (r1,g1,b1); returns the first index
	int ramp(int n, float r0, float g0, float b0, float r1, float g1, float b1) {
		int first = rgb.size();
		for (int i = 0; i < n; ++i) {
			float f = n > 1 ? i / (n - 1.0) : 0.0;
			add(r0 + (r1 - r0) * f, g0 + (g1 - g0) * f, b0 + (b1 - b0) * f);
		}
		return first;
	}

	// n entries through a list of [r,g,b] stops (piecewise linear); returns the first index
	int gradient(int n, var stops) {
		int first = rgb.size();
		int ns = stops.size() - 1;
		for (int i = 0; i < n; ++i) {
			float f = n > 1 ? i * ns / (n - 1.0) : 0.0;
			int k = math::itrunc(f);
			k = k >= ns ? ns - 1 : k;
			float u = f - k;
			var a = stops[k];
			var b = stops[k + 1];
			float ar = a[0]; float ag = a[1]; float ab = a[2];
			float br = b[0]; float bg2 = b[1]; float bb = b[2];
			add(ar + (br - ar) * u, ag + (bg2 - ag) * u, ab + (bb - ab) * u);
		}
		return first;
	}

	int size() { return rgb.size(); }
}

// ------------------------------------------------------------ presenter ---
// Globals on purpose: hb_row is also a parallel_transform body, which reads
// captured state (the pixel grid borrows zero-copy, the string tables snapshot
// per worker). The reel re-binds all of it after a hot reload resets them.

//
// Two cell shapes. Half blocks: a cell is 1x2 pixels. Sextants (Unicode 13): a cell
// is 2x3, fitted as JaiDoom fits them (the brightest pixel is the foreground, the
// darkest the background, each pixel takes the nearer in luminance). Scenes paint
// square pixels either way: a sextant is 2/3 as wide as it is tall, so in sextant
// mode the grid is three pixels a row tall and 3/2 a column wide, and each of a
// cell's two glyph columns shows the grid column nearest it.
//
// Over the pixels lies a text layer, one codepoint per cell (0 for none) with its
// colour: crisp terminal characters drawn on the cell's darkest pixel.

array<int> HB_PIX = [];
array<string> HB_FG = [];
array<string> HB_BG = [];
array<int> HB_RGB = [];
array<int> HB_LUM = [];
array<int> HB_ROWIDX = [];
array<int> HB_SRC = [];       // sextant mode: per glyph pixel column, the grid column it shows
array<string> HB_SEXTG = [];  // sextant glyphs by TL | TR<<1 | ML<<2 | MR<<3 | BL<<4 | BR<<5
array<int> TXT_CP = [];
array<int> TXT_RGB = [];
bool TXT_USED = false;
bool HB_SEXT = false;
int HB_W = 0;                 // grid pixels across
int HB_PH = 0;                // and down
int HB_COLS = 0;              // cells
int HB_ROWS = 0;
int HB_WORKERS = 0;
string HB_HALF = "▀";
string HB_FULL = "█";
string HB_RESET = ESC + "[0m";

function utf8(int cp) -> string {
	if (cp < 128) { return to_string(to_char(cp)); }
	if (cp < 2048) { return `${to_char(192 + (cp >> 6))}${to_char(128 + (cp & 63))}`; }
	if (cp < 65536) { return `${to_char(224 + (cp >> 12))}${to_char(128 + ((cp >> 6) & 63))}${to_char(128 + (cp & 63))}`; }
	return `${to_char(240 + (cp >> 18))}${to_char(128 + ((cp >> 12) & 63))}${to_char(128 + ((cp >> 6) & 63))}${to_char(128 + (cp & 63))}`;
}

// the grid's pixel size for cols x rows cells
function hb_pixel_w(int cols, bool sext) -> int { return sext ? cols * 3 / 2 : cols; }
function hb_pixel_h(int rows, bool sext) -> int { return sext ? rows * 3 : rows * 2; }

function hb_ensure(int cols, int rows, bool sext) -> void {
	HB_SEXT = sext;
	int pw = hb_pixel_w(cols, sext);
	int ph = hb_pixel_h(rows, sext);
	if (HB_COLS == cols && HB_ROWS == rows && HB_W == pw && HB_PH == ph && HB_PIX.size() == pw * ph) { return; }
	HB_COLS = cols;
	HB_ROWS = rows;
	HB_W = pw;
	HB_PH = ph;
	HB_PIX = [];
	for (int i = 0; i < pw * ph; ++i) { HB_PIX.push(0); }
	HB_ROWIDX = [];
	for (int r = 0; r < rows; ++r) { HB_ROWIDX.push(r); }
	HB_SRC = [];
	for (int g = 0; g < cols * 2; ++g) { HB_SRC.push(g * pw / (cols * 2)); }
	TXT_CP = [];
	TXT_RGB = [];
	for (int c = 0; c < cols * rows; ++c) {
		TXT_CP.push(0);
		TXT_RGB.push(0);
	}
	TXT_USED = false;
	HB_SEXTG = [];
	for (int b = 0; b < 64; ++b) {
		if (b == 0) { HB_SEXTG.push(" "); }
		else if (b == 21) { HB_SEXTG.push(utf8(0x258C)); }
		else if (b == 42) { HB_SEXTG.push(utf8(0x2590)); }
		else if (b == 63) { HB_SEXTG.push(HB_FULL); }
		else { HB_SEXTG.push(utf8(0x1FB00 + b - 1 - (b > 21 ? 1 : 0) - (b > 42 ? 1 : 0))); }
	}
}

// the text layer: cells, and the pixels they cover
function txt_clear() -> void {
	if (!TXT_USED) { return; }
	for (int i = 0; i < TXT_CP.size(); ++i) { TXT_CP[i] = 0; }
	TXT_USED = false;
}

function txt_glyph(int col, int row, int cp, int rgb) -> void {
	if (col < 0 || col >= HB_COLS || row < 0 || row >= HB_ROWS) { return; }
	TXT_CP[row * HB_COLS + col] = cp;
	TXT_RGB[row * HB_COLS + col] = rgb;
	TXT_USED = true;
}

// ASCII text from (col, row); spaces are written too, so a line can clear what lies under it
function txt_put(int col, int row, string s, int rgb) -> void {
	for (int i = 0; i < s.size(); ++i) { txt_glyph(col + i, row, s[i] + 0, rgb); }
}

function cell_col(int px) -> int { return px * HB_COLS / HB_W; }
function cell_row(int py) -> int { return py * HB_ROWS / HB_PH; }
function col_px(int col) -> int { return col * HB_W / HB_COLS; }
function row_px(int row) -> int { return row * HB_PH / HB_ROWS; }

// a text cell: its glyph over the darker of the pixels under it
function hb_text(int cell, int dark, var& state) -> string {
	string result = "";
	if (dark != state[1]) {
		result += HB_BG[dark];
		state[1] = dark;
	}
	int c = TXT_RGB[cell];
	state[0] = -1;
	int cp = TXT_CP[cell];
	return result + `${ESC}[38;2;${c >> 16};${(c >> 8) & 255};${c & 255}m` + (cp < 128 ? to_string(to_char(cp)) : utf8(cp));
}

function hb_row(int cy) -> string { return HB_SEXT ? hb_row_sext(cy) : hb_row_half(cy); }

function hb_row_half(int cy) -> string {
	string row = "";
	int vw = HB_W;
	int fg = -1;
	int bg = -1;
	int ti = cy * 2 * vw;
	int bi = ti + vw;
	int cell = cy * vw;
	bool text = TXT_USED;
	for (int x = 0; x < vw; ++x) {
		int top = HB_PIX[ti + x];
		int bot = HB_PIX[bi + x];
		if (text && TXT_CP[cell + x] != 0) {
			var state = [fg, bg];
			row += hb_text(cell + x, HB_LUM[top] < HB_LUM[bot] ? top : bot, state);
			fg = state[0];
			bg = state[1];
			continue;
		}
		if (top == bot) {
			if (bot == bg) { row += " "; }
			else if (top == fg) { row += HB_FULL; }
			else { row += HB_BG[bot]; bg = bot; row += " "; }
		} else {
			if (top != fg) { row += HB_FG[top]; fg = top; }
			if (bot != bg) { row += HB_BG[bot]; bg = bot; }
			row += HB_HALF;
		}
	}
	return row + HB_RESET;
}

function hb_row_sext(int cy) -> string {
	string row = "";
	int vw = HB_W;
	int fg = -1;
	int bg = -1;
	int r0 = cy * 3 * vw;
	int r1 = r0 + vw;
	int r2 = r1 + vw;
	int cols = HB_COLS;
	int cell = cy * cols;
	bool text = TXT_USED;
	for (int cx = 0; cx < cols; ++cx) {
		int xa = HB_SRC[cx * 2];
		int xb = HB_SRC[cx * 2 + 1];
		int p0 = HB_PIX[r0 + xa];
		int p1 = HB_PIX[r0 + xb];
		int p2 = HB_PIX[r1 + xa];
		int p3 = HB_PIX[r1 + xb];
		int p4 = HB_PIX[r2 + xa];
		int p5 = HB_PIX[r2 + xb];
		bool flat = p0 == p1 && p0 == p2 && p0 == p3 && p0 == p4 && p0 == p5;
		if (flat && !(text && TXT_CP[cell] != 0)) {
			if (p0 != bg) { row += HB_BG[p0]; bg = p0; }
			row += " ";
			++cell;
			continue;
		}
		int l0 = HB_LUM[p0];
		int l1 = HB_LUM[p1];
		int l2 = HB_LUM[p2];
		int l3 = HB_LUM[p3];
		int l4 = HB_LUM[p4];
		int l5 = HB_LUM[p5];
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
		if (text && TXT_CP[cell] != 0) {
			var state = [fg, bg];
			row += hb_text(cell, bgp, state);
			fg = state[0];
			bg = state[1];
			++cell;
			continue;
		}
		int thr = (lmin + lmax + 1) / 2;
		int bits = 0;
		if (l0 >= thr) { bits = 1; }
		if (l1 >= thr) { bits += 2; }
		if (l2 >= thr) { bits += 4; }
		if (l3 >= thr) { bits += 8; }
		if (l4 >= thr) { bits += 16; }
		if (l5 >= thr) { bits += 32; }
		if (fgp != fg) { row += HB_FG[fgp]; fg = fgp; }
		if (bits != 63 && bgp != bg) { row += HB_BG[bgp]; bg = bgp; }
		row += HB_SEXTG[bits];
		++cell;
	}
	return row + HB_RESET;
}

function hb_present() -> var {
	if (HB_WORKERS != 0) { return parallel_transform(HB_ROWIDX, hb_row); }
	var rows = [];
	for (int cy = 0; cy < HB_ROWS; ++cy) { rows.push(hb_row(cy)); }
	return rows;
}

// ------------------------------------------------------------ pixel font ---
// 5x7, one int per row, bit 4 = leftmost column

var FONT5 = {
	"A": [14, 17, 17, 31, 17, 17, 17], "B": [30, 17, 17, 30, 17, 17, 30], "C": [15, 16, 16, 16, 16, 16, 15],
	"D": [30, 17, 17, 17, 17, 17, 30], "E": [31, 16, 16, 30, 16, 16, 31], "F": [31, 16, 16, 30, 16, 16, 16],
	"G": [15, 16, 16, 19, 17, 17, 15], "H": [17, 17, 17, 31, 17, 17, 17], "I": [31, 4, 4, 4, 4, 4, 31],
	"J": [31, 2, 2, 2, 18, 18, 12], "K": [17, 18, 20, 24, 20, 18, 17], "L": [16, 16, 16, 16, 16, 16, 31],
	"M": [17, 27, 21, 21, 17, 17, 17], "N": [17, 25, 21, 19, 17, 17, 17], "O": [14, 17, 17, 17, 17, 17, 14],
	"P": [30, 17, 17, 30, 16, 16, 16], "Q": [14, 17, 17, 17, 21, 18, 13], "R": [30, 17, 17, 30, 20, 18, 17],
	"S": [15, 16, 16, 14, 1, 1, 30], "T": [31, 4, 4, 4, 4, 4, 4], "U": [17, 17, 17, 17, 17, 17, 14],
	"V": [17, 17, 17, 17, 17, 10, 4], "W": [17, 17, 17, 21, 21, 21, 10], "X": [17, 17, 10, 4, 10, 17, 17],
	"Y": [17, 17, 10, 4, 4, 4, 4], "Z": [31, 1, 2, 4, 8, 16, 31],
	"0": [14, 17, 19, 21, 25, 17, 14], "1": [4, 12, 4, 4, 4, 4, 14], "2": [14, 17, 1, 2, 4, 8, 31],
	"3": [30, 1, 1, 14, 1, 1, 30], "4": [2, 6, 10, 18, 31, 2, 2], "5": [31, 16, 30, 1, 1, 17, 14],
	"6": [14, 16, 16, 30, 17, 17, 14], "7": [31, 1, 2, 4, 8, 8, 8], "8": [14, 17, 17, 14, 17, 17, 14],
	"9": [14, 17, 17, 15, 1, 1, 14], ".": [0, 0, 0, 0, 0, 12, 12], "!": [4, 4, 4, 4, 4, 0, 4],
	"-": [0, 0, 0, 14, 0, 0, 0], ":": [0, 12, 12, 0, 12, 12, 0], "/": [1, 1, 2, 4, 8, 16, 16],
	"+": [0, 4, 4, 31, 4, 4, 0], "?": [14, 17, 1, 2, 4, 0, 4], "'": [4, 4, 8, 0, 0, 0, 0]
};

class Gfx {
	array<int> make_grid(int n) {
		array<int> g = [];
		for (int i = 0; i < n; ++i) { g.push(0); }
		return g;
	}

	array<float> make_fgrid(int n) {
		array<float> g = [];
		for (int i = 0; i < n; ++i) { g.push(0.0); }
		return g;
	}

	var rainbow_rgb(float phase) {
		return [128.0 + 127.0 * math::sin(phase),
			128.0 + 127.0 * math::sin(phase + 2.094),
			128.0 + 127.0 * math::sin(phase + 4.188)];
	}

	string fmt1(float v) {
		int t = math::itrunc(v * 10.0 + 0.5);
		return `${(t / 10)}.${(t % 10)}`;
	}

	int text_width(string text, int sx) {
		return text.size() * 6 * sx - sx;
	}

	// 5x7 text into a pixel grid; rows [row0, row0 + nramp) of the palette shade
	// the glyphs top to bottom, shadow < 0 draws none; only the first `limit` glyphs
	void draw_text(var& pix, int w, int h, string text, int x0, int y0, int sx, int sy,
		int row0, int nramp, int shadow, int limit) {
		int n = text.size() < limit ? text.size() : limit;
		int th = 7 * sy;
		for (int pass = shadow < 0 ? 1 : 0; pass < 2; ++pass) {
			int ox = pass == 0 ? 1 : 0;
			for (int li = 0; li < n; ++li) {
				string ch = text.substr(li, 1);
				if (!FONT5.has(ch)) { continue; }
				var bits = FONT5[ch];
				int lx = x0 + li * 6 * sx + ox;
				for (int ry = 0; ry < 7; ++ry) {
					int b = bits[ry];
					if (b == 0) { continue; }
					for (int dy = 0; dy < sy; ++dy) {
						int yy = y0 + ry * sy + dy + ox;
						if (yy < 0 || yy >= h) { continue; }
						int c = pass == 0 ? shadow : row0 + ((ry * sy + dy) * nramp) / th;
						for (int rx = 0; rx < 5; ++rx) {
							if (((b >> (4 - rx)) & 1) == 0) { continue; }
							for (int dx = 0; dx < sx; ++dx) {
								int xx = lx + rx * sx + dx;
								if (xx >= 0 && xx < w) { pix[yy * w + xx] = c; }
							}
						}
					}
				}
			}
		}
	}

	// transitions between two pixel grids; gb's indices shift by `off` into the
	// merged palette, `edge` is the palette index of a glowing seam
	// kind: 0 dissolve, 1 slant wipe, 2 blinds, 3 iris, 4 melt, 5 zoom through, 6 shatter, 7 ripple, 8 swirl
	void mix_grids(var& ga, var& gb, var& gm, int w, int h, int kind, float prog, int off, int edge, var& melt) {
		int kilo = math::itrunc(prog * 1000.0);
		int idx = 0;
		if (kind >= 5) {
			mix_motion(ga, gb, gm, w, h, kind, prog, off, edge);
			return;
		}
		if (kind == 0) {
			for (int y = 0; y < h; ++y) {
				for (int x = 0; x < w; ++x) {
					int k = (((x * 73856093 + y * 19349663) & 1048575) * 1274126177 >> 12) % 1000;
					gm[idx] = k < kilo ? gb[idx] + off : ga[idx];
					++idx;
				}
			}
			return;
		}
		if (kind == 1) {
			int e = math::itrunc(prog * (w + h / 2 + 8.0)) - 4;
			for (int y = 0; y < h; ++y) {
				int sh = e - (h - y) / 2;
				for (int x = 0; x < w; ++x) {
					int d = x - sh;
					gm[idx] = d < 0 ? gb[idx] + off : (d < 2 ? edge : ga[idx]);
					++idx;
				}
			}
			return;
		}
		if (kind == 2) {
			int band = 8;
			int e2 = math::itrunc(prog * (band + 1.0));
			for (int y = 0; y < h; ++y) {
				int yb = (y + (y / band) % 2 * 3) % band;
				for (int x = 0; x < w; ++x) {
					gm[idx] = yb < e2 ? gb[idx] + off : (yb == e2 ? edge : ga[idx]);
					++idx;
				}
			}
			return;
		}
		if (kind == 3) {
			int cx = w / 2;
			int cy = h / 2;
			int rmax = cx * cx + cy * cy;
			int r = math::itrunc(prog * prog * (rmax + 400.0));
			int r2 = math::itrunc(math::sqrt(r * 1.0) + 2.0);
			r2 *= r2;
			for (int y = 0; y < h; ++y) {
				int dy = y - cy;
				for (int x = 0; x < w; ++x) {
					int dx = x - cx;
					int d2 = dx * dx + dy * dy;
					gm[idx] = d2 < r ? gb[idx] + off : (d2 < r2 ? edge : ga[idx]);
					++idx;
				}
			}
			return;
		}
		// melt: the departing frame slides down in ragged columns
		float fall = prog * prog * (h + 24.0);
		for (int x = 0; x < w; ++x) {
			int o = math::itrunc(fall - melt[x]);
			o = o < 0 ? 0 : o;
			int gi = x;
			for (int y = 0; y < h; ++y) {
				gm[gi] = y < o ? gb[gi] + off : ga[gi - o * w];
				gi += w;
			}
		}
	}

	// the transitions that move the picture: each pixel of the result reads the departing
	// frame through a moving map (or the arriving one where the map has let go of it)
	// 5 zoom through: the departing frame rushes at the viewer, turning, and the arriving
	//   one opens from the centre behind it
	// 6 shatter: the departing frame breaks into tiles that spin away from the centre
	// 7 ripple: a ring wave runs out from the centre; behind it the arriving frame
	// 8 swirl: the departing frame twists into a vortex and drains into the arriving one
	void mix_motion(var& ga, var& gb, var& gm, int w, int h, int kind, float prog, int off, int edge) {
		float cx = w * 0.5;
		float cy = h * 0.5;
		float e = prog * prog * (3.0 - 2.0 * prog);
		float reach = math::sqrt(cx * cx + cy * cy);
		int idx = 0;
		int tile = h / 9 < 3 ? 3 : h / 9;
		for (int y = 0; y < h; ++y) {
			float dy = y + 0.5 - cy;
			for (int x = 0; x < w; ++x) {
				float dx = x + 0.5 - cx;
				int src = -1;
				if (kind == 5) {
					float zoom = 1.0 + e * e * 7.0;
					float a = e * 1.4;
					float ca = math::cos(a);
					float sa = math::sin(a);
					float sx = cx + (dx * ca + dy * sa) / zoom;
					float sy = cy + (dy * ca - dx * sa) / zoom;
					float r = math::sqrt(dx * dx + dy * dy) / reach;
					bool open = r < e * e * 1.3;
					if (!open && sx >= 0.0 && sx < w && sy >= 0.0 && sy < h) { src = math::itrunc(sy) * w + math::itrunc(sx); }
					gm[idx] = src >= 0 ? ga[src] : gb[idx] + off;
				} else if (kind == 6) {
					int tx = x / tile;
					int ty = y / tile;
					int hsh = ((tx * 73 + ty * 151) % 97 + 97) % 97;
					float delay = hsh / 97.0 * 0.45;
					float k = (prog - delay) / 0.55;
					k = k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
					float tcx = (tx + 0.5) * tile - cx;
					float tcy = (ty + 0.5) * tile - cy;
					float push = k * k * reach * 1.4;
					float tl = math::sqrt(tcx * tcx + tcy * tcy) + 1.0;
					float spin = k * (hsh % 2 == 0 ? 2.0 : -2.0);
					float shrink = 1.0 - k * 0.6;
					float lx = x + 0.5 - ((tx + 0.5) * tile + tcx / tl * push);
					float ly = y + 0.5 - ((ty + 0.5) * tile + tcy / tl * push + k * k * h * 0.5);
					float cs = math::cos(spin);
					float sn = math::sin(spin);
					float ux = (lx * cs + ly * sn) / shrink;
					float uy = (ly * cs - lx * sn) / shrink;
					float half = tile * 0.5;
					if (k < 1.0 && ux > -half && ux < half && uy > -half && uy < half) {
						int sx2 = math::itrunc((tx + 0.5) * tile + ux);
						int sy2 = math::itrunc((ty + 0.5) * tile + uy);
						if (sx2 >= 0 && sx2 < w && sy2 >= 0 && sy2 < h) { src = sy2 * w + sx2; }
					}
					bool seam = k > 0.0 && k < 0.15 && (x % tile == 0 || y % tile == 0);
					gm[idx] = seam ? edge : (src >= 0 ? ga[src] : gb[idx] + off);
				} else if (kind == 7) {
					float r2 = math::sqrt(dx * dx + dy * dy);
					float front = e * (reach + 12.0);
					float d = r2 - front;
					float wave = d > -10.0 && d < 10.0 ? math::sin(d * 0.6) * (10.0 - (d < 0.0 ? -d : d)) * 0.35 : 0.0;
					float sx3 = x + dx / (r2 + 0.01) * wave;
					float sy3 = y + dy / (r2 + 0.01) * wave;
					bool arrived = d < 0.0;
					int ix = math::itrunc(sx3);
					int iy = math::itrunc(sy3);
					ix = ix < 0 ? 0 : (ix >= w ? w - 1 : ix);
					iy = iy < 0 ? 0 : (iy >= h ? h - 1 : iy);
					int si = iy * w + ix;
					gm[idx] = d > -1.2 && d < 0.6 ? edge : (arrived ? gb[si] + off : ga[si]);
				} else {
					float r3 = math::sqrt(dx * dx + dy * dy) / reach;
					float twist = e * 9.0 * (1.0 - r3) * (1.0 - r3);
					float pull = 1.0 + e * e * 3.0 * (1.0 - r3);
					float ca2 = math::cos(twist);
					float sa2 = math::sin(twist);
					float sx4 = cx + (dx * ca2 - dy * sa2) * pull;
					float sy4 = cy + (dx * sa2 + dy * ca2) * pull;
					bool drained = r3 > 1.15 - e * 1.3;
					if (!drained && sx4 >= 0.0 && sx4 < w && sy4 >= 0.0 && sy4 < h) { src = math::itrunc(sy4) * w + math::itrunc(sx4); }
					gm[idx] = src >= 0 ? ga[src] : gb[idx] + off;
				}
				++idx;
			}
		}
	}

	// replace a full row with centered styled text (style = escape prefix)
	void overlay_center(var& rows, int w, int y, string text, string style) {
		if (y < 0 || y >= rows.size()) { return; }
		int n = text.size();
		if (n > w) {
			text = text.substr(0, w);
			n = w;
		}
		int pad = (w - n) / 2;
		rows[y] = style + string::repeat(" ", pad) + text + string::repeat(" ", w - pad - n) + RESET;
	}

}

function sgr_fg(int r, int g, int b) -> string { return `${ESC}[38;2;${r};${g};${b}m`; }
function sgr_bg(int r, int g, int b) -> string { return `${ESC}[48;2;${r};${g};${b}m`; }
