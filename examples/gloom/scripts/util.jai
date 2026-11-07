// Small helpers: math, hashing, the truecolor palette intern, text padding.

float TICK = 1.0 / 30.0; // fixed sim timestep; --smoke feeds exactly this dt

function iabs(int v) -> int { return v < 0 ? -v : v; }
function imax(int a, int b) -> int { return a > b ? a : b; }
function imin(int a, int b) -> int { return a < b ? a : b; }
function iclamp(int v, int lo, int hi) -> int { return v < lo ? lo : (v > hi ? hi : v); }
function fclamp(float v, float lo, float hi) -> float { return v < lo ? lo : (v > hi ? hi : v); }

// FNV-1a, 32-bit lane inside an int64 (checked-overflow build: mask every round)
function mix32(int h, int v) -> int {
	int x = h ^ (v & 0xFFFFFFFF);
	x = x ^ ((v >> 32) & 0xFFFFFFFF);
	return ((x & 0xFFFFFFFF) * 16777619) & 0xFFFFFFFF;
}

// ---------------------------------------------------------- palette intern --
// Every color the game shows is minted at boot / map load through rgb_idx(),
// which interns exact 24-bit colors: pixel grids store small palette indices,
// the row builders emit the prebuilt escapes, and PAL_LUM feeds the
// quadrant/sextant bright/dark cell partition (the doom-cli trick: N pixels
// per cell, two colors chosen by luminosity). The escapes are 24-bit, or the
// nearest xterm-256 color on a terminal without 24-bit color.
map<int, int> PAL_KEYS = {}; // r<<16|g<<8|b -> index
array<string> PAL_FG = [];      // foreground escape per index
array<string> PAL_BG = [];      // background escape per index
array<int> PAL_LUM = [];     // 2r+3g+b per index
array<string> PAL_ASCII = [];   // --ascii: background, foreground and density character per index
array<string> PAL_ASCII_CHAR = [];  // --ascii: the density character alone

function rgb_idx(int r, int g, int b) -> int {
	r = iclamp(r, 0, 255);
	g = iclamp(g, 0, 255);
	b = iclamp(b, 0, 255);
	int key = r * 65536 + g * 256 + b;
	if (PAL_KEYS.has(key)) { return PAL_KEYS[key]; }
	int idx = PAL_FG.size();
	PAL_KEYS[key] = idx;
	PAL_FG.push(sgr_color(38, r, g, b));
	PAL_BG.push(sgr_color(48, r, g, b));
	PAL_LUM.push(r * 2 + g * 3 + b);
	if (HOST_ASCII) {
		string density = " .:-=+*#%@";
		string ch = density.substr((r * 2 + g * 3 + b) * 10 / 1536, 1);
		PAL_ASCII.push(sgr_color(48, r * 2 / 5, g * 2 / 5, b * 2 / 5) + sgr_color(38, r, g, b) + ch);
		PAL_ASCII_CHAR.push(ch);
	}
	return idx;
}

function sgr_color(int layer, int r, int g, int b) -> string {
	if (HOST_TRUECOLOR) { return `${ESC}[${layer};2;${r};${g};${b}m`; }
	return `${ESC}[${layer};5;${xterm256(r, g, b)}m`;
}

// the nearest of the 6x6x6 color cube and the 24-step gray ramp
function xterm256(int r, int g, int b) -> int {
	array<int> levels = [0, 95, 135, 175, 215, 255];
	int cr = cube_step(r);
	int cg = cube_step(g);
	int cb = cube_step(b);
	int dr = r - levels[cr];
	int dg = g - levels[cg];
	int db = b - levels[cb];
	int cube_error = dr * dr + dg * dg + db * db;
	int step = iclamp(((r + g + b) / 3 - 3) / 10, 0, 23);
	int gray = 8 + step * 10;
	int gray_error = (r - gray) * (r - gray) + (g - gray) * (g - gray) + (b - gray) * (b - gray);
	if (gray_error < cube_error) { return 232 + step; }
	return 16 + 36 * cr + 6 * cg + cb;
}

function cube_step(int v) -> int {
	if (v < 48) { return 0; }
	if (v < 115) { return 1; }
	return (v - 35) / 40;
}

function col(string code, string text) -> string {
	return `${ESC}[${code}m${text}${ESC}[0m`;
}

// pad/truncate plain text to n columns
function pad_to(string s, int n) -> string {
	if (s.size() > n) { return s.substr(0, n); }
	return s + string::repeat(" ", n - s.size());
}

// smallest signed angle from a to b, in (-pi, pi]
function ang_diff(float a, float b) -> float {
	float d = b - a;
	while (d > 3.14159265) { d -= 6.2831853; }
	while (d < -3.14159265) { d += 6.2831853; }
	return d;
}

function dist2(float ax, float ay, float bx, float by) -> float {
	float dx = ax - bx;
	float dy = ay - by;
	return dx * dx + dy * dy;
}
