// Gator Quest - small pure helpers: the sim RNG, hashing, value noise,
// direction tables. No runner calls (the sim core stays portable).

import "gq_state.jai";

array<int> DX = [0, 1, 1, 1, 0, -1, -1, -1];
array<int> DY = [-1, -1, 0, 1, 1, 1, 0, -1];
array<string> DIR_NAME = ["north", "northeast", "east", "southeast", "south", "southwest", "west", "northwest"];

int iabs(int v) { return v < 0 ? -v : v; }
int imin(int a, int b) { return a < b ? a : b; }
int imax(int a, int b) { return a > b ? a : b; }
int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int isign(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }
int cheb(int x0, int y0, int x1, int y1) { return imax(iabs(x0 - x1), iabs(y0 - y1)); }
int dist2(int x0, int y0, int x1, int y1) { return (x0 - x1) * (x0 - x1) + (y0 - y1) * (y0 - y1); }

// direction index for a step of (dx, dy) in -1..1
int dir_of(int dx, int dy) {
	for (int d = 0; d < 8; ++d) {
		if (DX[d] == isign(dx) && DY[d] == isign(dy)) { return d; }
	}
	return 4;
}

// ---- the sim RNG: xorshift32 kept in an int64 (masked, so checked overflow never trips)
int rng_next() {
	int s = RNG_S;
	s = s ^ ((s << 13) & 0xFFFFFFFF);
	s = s ^ (s >> 17);
	s = s ^ ((s << 5) & 0xFFFFFFFF);
	RNG_S = s;
	return s;
}

int rnd(int n) { return n <= 1 ? 0 : rng_next() % n; }
int rnd_range(int lo, int hi) { return hi <= lo ? lo : lo + rnd(hi - lo + 1); }
bool chance(int permille) { return rnd(1000) < permille; }

void rng_seed(int seed) {
	RNG_S = (seed & 0x7FFFFFFF) ^ 0x2545F491;
	if (RNG_S == 0) { RNG_S = 1; }
	for (int i = 0; i < 8; ++i) { rng_next(); }
}

// ---- stateless hashing (worldgen, visual variants)
int hash3(int x, int y, int z) {
	int h = (x * 374761393 + y * 668265263 + (z & 0x7FFFFFFF) * 1442695041) & 0xFFFFFFFF;
	h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF;
	return h ^ (h >> 16);
}

// FNV-style fold for the state hash, 32-bit lane
int mix32(int h, int v) {
	int x = h ^ (v & 0xFFFFFFFF);
	x = x ^ ((v >> 32) & 0xFFFFFFFF);
	return ((x & 0xFFFFFFFF) * 16777619) & 0xFFFFFFFF;
}

int mix_str(int h, string s) {
	int n = s.size();
	for (int i = 0; i < n; ++i) { h = mix32(h, s[i] + 0); }
	return mix32(h, n);
}

// value noise, fixed point 0..1023, bilinear over a lattice of `cell` tiles
int vnoise(int x, int y, int cell, int salt) {
	int gx = x / cell;
	int gy = y / cell;
	int fx = (x % cell) * 1024 / cell;
	int fy = (y % cell) * 1024 / cell;
	fx = fx * fx * (3072 - 2 * fx) / 1048576;
	fy = fy * fy * (3072 - 2 * fy) / 1048576;
	int a = hash3(gx, gy, salt) & 1023;
	int b = hash3(gx + 1, gy, salt) & 1023;
	int c = hash3(gx, gy + 1, salt) & 1023;
	int d = hash3(gx + 1, gy + 1, salt) & 1023;
	int top = a + (b - a) * fx / 1024;
	int bot = c + (d - c) * fx / 1024;
	return top + (bot - top) * fy / 1024;
}

int fractal(int x, int y, int salt) {
	return (vnoise(x, y, 24, salt) * 4 + vnoise(x, y, 11, salt + 17) * 2 + vnoise(x, y, 5, salt + 31)) / 7;
}

string plural(int n, string one, string many) { return n == 1 ? one : many; }

string cap_first(string s) {
	if (s.size() == 0) { return s; }
	string head = s.substr(0, 1);
	head.to_upper();
	return head + s.substr(1, s.size() - 1);
}

// "a sword" / "an apple" / "3 apples"
string article(string name) {
	if (name.size() == 0) { return name; }
	int c = name[0] + 0;
	if (c == 97 || c == 101 || c == 105 || c == 111 || c == 117 || c == 65 || c == 69 || c == 73 || c == 79 || c == 85) { return "an " + name; }
	return "a " + name;
}

// skill tenths -> "57.3"
string tenths(int v) { return `${v / 10}.${v % 10}`; }
