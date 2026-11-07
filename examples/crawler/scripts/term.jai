// The terminal: colour depth (24-bit, or the xterm 256-colour cube), the glyph set (Unicode
// blocks and box lines, or plain ASCII), and the named inks every screen writes in.

string ESC = "\x1b";
string RESET = "\x1b[0m";
string EOL = "\x1b[K";

// Declared without initializers, so a hot reload of this file keeps what term_setup chose.
bool TRUECOLOR;
bool ASCII_ONLY;
string TEXT_FG;                 // the escape back to INK_TEXT after a painted span

// glyphs: the half block every view pixel pair is drawn with, box lines, bars, dots and arrows
string G_HALF;
string G_FULL;
string G_HLINE;
string G_VLINE;
string G_TL;
string G_TR;
string G_BL;
string G_BR;
string G_DOT;
string G_CROSS;
array<string> G_EIGHTHS;        // a bar's partial cell, 1/8 to 7/8 filled
array<string> G_ARROWS;         // facing N E S W on the maps

// inks: one palette for every word on screen
int INK_TEXT = 0xE8E2D0;
int INK_BRIGHT = 0xFFFFFF;
int INK_DIM = 0x8C8676;
int INK_FAINT = 0x58534A;
int INK_TITLE = 0xFFD45E;
int INK_GOLD = 0xF2B544;
int INK_GOOD = 0x7EE08A;
int INK_BAD = 0xFF5C4E;
int INK_MAGIC = 0xD08CFF;
int INK_INFO = 0x7CDCEB;
int INK_TEAL = 0x3FB8A8;
int INK_BLESS = 0xFFE89A;
int INK_PANEL = 0x0E0C12;       // the background of text boxes and modals
int INK_SELECT = 0x2A2433;      // a highlighted row

function term_setup(bool truecolor, bool ascii) -> void {
	TRUECOLOR = truecolor;
	ASCII_ONLY = ascii;
	TEXT_FG = sgr_fg(INK_TEXT);
	if (ascii) {
		G_HALF = " ";
		G_FULL = "#";
		G_HLINE = "-";
		G_VLINE = "|";
		G_TL = "+";
		G_TR = "+";
		G_BL = "+";
		G_BR = "+";
		G_DOT = ".";
		G_CROSS = "+";
		G_EIGHTHS = ["#", "#", "#", "#", "#", "#", "#"];
		G_ARROWS = ["^", ">", "v", "<"];
		return;
	}
	G_HALF = utf8_char(0x2580);
	G_FULL = utf8_char(0x2588);
	G_HLINE = utf8_char(0x2500);
	G_VLINE = utf8_char(0x2502);
	G_TL = utf8_char(0x256D);
	G_TR = utf8_char(0x256E);
	G_BL = utf8_char(0x2570);
	G_BR = utf8_char(0x256F);
	G_DOT = utf8_char(0x00B7);
	G_CROSS = utf8_char(0x253C);
	G_EIGHTHS = [];
	for (int i = 0; i < 7; ++i) { G_EIGHTHS.push(utf8_char(0x258F - i)); }
	G_ARROWS = [utf8_char(0x25B2), utf8_char(0x25BA), utf8_char(0x25BC), utf8_char(0x25C4)];
}

function utf8_char(int cp) -> string {
	if (cp < 0x80) { return `${to_char(cp)}`; }
	if (cp < 0x800) { return `${to_char(0xC0 | (cp >> 6))}${to_char(0x80 | (cp & 63))}`; }
	return `${to_char(0xE0 | (cp >> 12))}${to_char(0x80 | ((cp >> 6) & 63))}${to_char(0x80 | (cp & 63))}`;
}

// the nearest of the xterm palette's 6x6x6 cube and its 24 grays
function xterm256(int rgb) -> int {
	int r = (rgb >> 16) & 255;
	int g = (rgb >> 8) & 255;
	int b = rgb & 255;
	int ri = cube_step(r);
	int gi = cube_step(g);
	int bi = cube_step(b);
	int cube = 16 + ri * 36 + gi * 6 + bi;
	int cube_error = sq(r - cube_level(ri)) + sq(g - cube_level(gi)) + sq(b - cube_level(bi));
	int gray_step = iclamp(((r + g + b) / 3 - 3) / 10, 0, 23);
	int gray = 8 + gray_step * 10;
	int gray_error = sq(r - gray) + sq(g - gray) + sq(b - gray);
	return gray_error < cube_error ? 232 + gray_step : cube;
}

function cube_step(int v) -> int { return v < 48 ? 0 : (v < 115 ? 1 : (v - 35) / 40); }
function cube_level(int step) -> int { return step == 0 ? 0 : 55 + step * 40; }
function sq(int v) -> int { return v * v; }

function sgr_fg(int rgb) -> string {
	if (TRUECOLOR) { return `${ESC}[38;2;${(rgb >> 16) & 255};${(rgb >> 8) & 255};${rgb & 255}m`; }
	return `${ESC}[38;5;${xterm256(rgb)}m`;
}

function sgr_bg(int rgb) -> string {
	if (TRUECOLOR) { return `${ESC}[48;2;${(rgb >> 16) & 255};${(rgb >> 8) & 255};${rgb & 255}m`; }
	return `${ESC}[48;5;${xterm256(rgb)}m`;
}

// text in one ink, then back to INK_TEXT; a background already set carries through
function paint(int rgb, string text) -> string { return `${sgr_fg(rgb)}${text}${TEXT_FG}`; }

function mix_rgb(int a, int b, float t) -> int {
	int r = lerp_i((a >> 16) & 255, (b >> 16) & 255, t);
	int g = lerp_i((a >> 8) & 255, (b >> 8) & 255, t);
	int bl = lerp_i(a & 255, b & 255, t);
	return (r << 16) | (g << 8) | bl;
}

function scale_rgb(int rgb, float k) -> int {
	int r = iclamp(((rgb >> 16) & 255) * k, 0, 255);
	int g = iclamp(((rgb >> 8) & 255) * k, 0, 255);
	int b = iclamp((rgb & 255) * k, 0, 255);
	return (r << 16) | (g << 8) | b;
}

function lerp_i(int a, int b, float t) -> int { return a + (b - a) * t; }
