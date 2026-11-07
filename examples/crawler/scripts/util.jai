// Small helpers shared by every file: integer math, the map grid, directions, weighted picks
// and the run's fingerprint hash.

int MAP_W = 64;
int MAP_H = 20;

// facing: 0 north, 1 east, 2 south, 3 west; y grows downward, so north is -y
array<int> FACE_DX = [0, 1, 0, -1];
array<int> FACE_DY = [-1, 0, 1, 0];
array<string> FACE_LETTER = ["N", "E", "S", "W"];
array<string> FACE_NAME = ["NORTH", "EAST", "SOUTH", "WEST"];

class Step { int dx; int dy; }

array<Step> EIGHT_DIRS = [Step{-1, 0}, Step{1, 0}, Step{0, -1}, Step{0, 1}, Step{-1, -1}, Step{1, -1}, Step{-1, 1}, Step{1, 1}];
array<Step> FOUR_DIRS = [Step{-1, 0}, Step{1, 0}, Step{0, -1}, Step{0, 1}];

// the overhead map's movement keys: vi keys, wasd and the arrows (diagonals are refused)
map<string, Step> MAP_KEYS = {
	"h": Step{-1, 0}, "l": Step{1, 0}, "k": Step{0, -1}, "j": Step{0, 1},
	"y": Step{-1, -1}, "u": Step{1, -1}, "b": Step{-1, 1}, "n": Step{1, 1},
	"w": Step{0, -1}, "a": Step{-1, 0}, "s": Step{0, 1}, "d": Step{1, 0},
	"left": Step{-1, 0}, "right": Step{1, 0}, "up": Step{0, -1}, "down": Step{0, 1}
};

function facing_for(int dx, int dy) -> int {
	if (dy < 0) { return 0; }
	if (dx > 0) { return 1; }
	if (dy > 0) { return 2; }
	return 3;
}

function iabs(int v) -> int { return v < 0 ? -v : v; }
function imax(int a, int b) -> int { return a > b ? a : b; }
function imin(int a, int b) -> int { return a < b ? a : b; }
function iclamp(int v, int lo, int hi) -> int { return v < lo ? lo : (v > hi ? hi : v); }
function sign_of(int v) -> int { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

// Chebyshev distance: adjacency in eight directions
function cheb(int x1, int y1, int x2, int y2) -> int { return imax(iabs(x1 - x2), iabs(y1 - y2)); }

function idx(int x, int y) -> int { return y * MAP_W + x; }
function in_bounds(int x, int y) -> bool { return x >= 0 && y >= 0 && x < MAP_W && y < MAP_H; }

// The generator comes by reference: a copy would draw the number the real one then draws again.
function pick(var& rng, array<string> lines) -> string { return lines[rng.next(lines.size())]; }

// an index drawn by weight: one draw from rng, whatever the weights
function pick_weighted(var& rng, array<int> weights) -> int {
	int total = 0;
	for (int w : weights) { total += w; }
	int roll = rng.next(total);
	for (int i = 0; i < weights.size(); ++i) {
		if (roll < weights[i]) { return i; }
		roll -= weights[i];
	}
	return weights.size() - 1;
}

// FNV-1a in a 32-bit lane of an int64: every round masks, so checked arithmetic never overflows
function mix32(int h, int v) -> int {
	int x = h ^ (v & 0xFFFFFFFF);
	x = x ^ ((v >> 32) & 0xFFFFFFFF);
	return ((x & 0xFFFFFFFF) * 16777619) & 0xFFFFFFFF;
}

// a presentation-only hash (texture variants, flavour picks): never the game's Rng
function lat(int x, int y, int s) -> int {
	int h = (x * 374761393 + y * 668265263 + s * 1442695041) & 0xFFFFFF;
	h = (h ^ (h >> 7)) * 1274126177;
	h = h & 0x7FFFFFFF;
	return (h ^ (h >> 15)) & 0xFFFF;
}
