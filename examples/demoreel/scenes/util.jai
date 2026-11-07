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
		int ir = itrunc(r < 0.0 ? 0.0 : (r > 255.0 ? 255.0 : r));
		int ig = itrunc(g < 0.0 ? 0.0 : (g > 255.0 ? 255.0 : g));
		int ib = itrunc(b < 0.0 ? 0.0 : (b > 255.0 ? 255.0 : b));
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
			int k = itrunc(f);
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

array<int> HB_PIX = [];
array<string> HB_FG = [];
array<string> HB_BG = [];
array<int> HB_RGB = [];
array<int> HB_ROWIDX = [];
int HB_W = 0;
int HB_ROWS = 0;
int HB_WORKERS = 0;
string HB_HALF = "▀";
string HB_FULL = "█";
string HB_RESET = ESC + "[0m";

function hb_ensure(int w, int rows) -> void {
	if (HB_W == w && HB_ROWS == rows && HB_PIX.size() == w * rows * 2) { return; }
	HB_W = w;
	HB_ROWS = rows;
	HB_PIX = [];
	for (int i = 0; i < w * rows * 2; ++i) { HB_PIX.push(0); }
	HB_ROWIDX = [];
	for (int r = 0; r < rows; ++r) { HB_ROWIDX.push(r); }
}

function hb_row(int cy) -> string {
	string row = "";
	int vw = HB_W;
	int fg = -1;
	int bg = -1;
	int ti = cy * 2 * vw;
	int bi = ti + vw;
	for (int x = 0; x < vw; ++x) {
		int top = HB_PIX[ti + x];
		int bot = HB_PIX[bi + x];
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
		int t = itrunc(v * 10.0 + 0.5);
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
	// kind: 0 dissolve, 1 slant wipe, 2 blinds, 3 iris, 4 melt
	void mix_grids(var& ga, var& gb, var& gm, int w, int h, int kind, float prog, int off, int edge, var& melt) {
		int kilo = itrunc(prog * 1000.0);
		int idx = 0;
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
			int e = itrunc(prog * (w + h / 2 + 8.0)) - 4;
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
			int e2 = itrunc(prog * (band + 1.0));
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
			int r = itrunc(prog * prog * (rmax + 400.0));
			int r2 = itrunc(math::sqrt(r * 1.0) + 2.0);
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
			int o = itrunc(fall - melt[x]);
			o = o < 0 ? 0 : o;
			int gi = x;
			for (int y = 0; y < h; ++y) {
				gm[gi] = y < o ? gb[gi] + off : ga[gi - o * w];
				gi += w;
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
