// The Well's looks: the wardrobe each pair of floors wears, the procedural 32x32 wall, floor and
// ceiling textures, and the pixel-art sprites, written as strings of role letters.

// Every two floors the Well changes wardrobe. pattern: 0 mossy brick, 1 bloody brick, 2 fungus
// blocks, 3 riveted panels, 4 ember-cracked rock, 5 veined marble.
class Theme { string name; int pattern; int wall; int mortar; int accent; int glow; int ceil; int floor; int fog; }

array<Theme> DEPTH_THEMES = [
	Theme{"Mossrot Warrens", 0, 0x6E8C4A, 0x3A3428, 0x8CF03C, 0xC8FF6E, 0x2E3A6E, 0x68522E, 0x080C1E},
	Theme{"Bloodbrick Vaults", 1, 0xC44034, 0x4A2A2E, 0x7A0C18, 0xFFAA78, 0x3A1A42, 0x786256, 0x1A060E},
	Theme{"Gilded Fungus Halls", 2, 0xAC3EC4, 0x2A1036, 0x3CD2A0, 0xFFD65A, 0x144650, 0x967A24, 0x08181C},
	Theme{"Screaming Copper Vein", 3, 0x24BAB0, 0x0E2A30, 0xFF963C, 0xFFE08C, 0x46104E, 0xB0601E, 0x1E0622},
	Theme{"The Ashen Throat", 4, 0x8C8474, 0x241E1C, 0x5A4A3C, 0xFF6E1E, 0x36343A, 0x7C7060, 0x16100E},
	Theme{"The Hollow Gallery", 5, 0x8E8E94, 0x3C3C42, 0x5C5A60, 0xDCDCE6, 0x26262A, 0x4A443E, 0x0C0C0E}
];

function theme_index(int floor) -> int {
	if (floor >= 10) { return 5; }
	if (floor >= 9) { return 4; }
	return (floor - 1) / 2;
}

function theme_of(int floor) -> Theme { return DEPTH_THEMES[theme_index(floor)]; }
function theme_name(int floor) -> string { return theme_of(floor).name; }

// ---------------------------------------------------------------- textures ---
// A texture is 1024 material codes while it is generated and 1024 ramp bases once baked.
// Wall codes: 0 mortar 1 base 2 base-dark 3 base-lit 4 accent 5 accent-dark 6 glow 7 void
// 8 bone 9 iron 10 flame 11 flame-core 12 white-hot.
// ------------------------------------------------------------- textures ----
// Textures are 32x32. Texels are material codes while generating, ramp bases
// once baked. Wall codes: 0 mortar 1 base 2 base-dark 3 base-lit 4 accent
// 5 accent-dark 6 glow 7 void 8 bone 9 iron 10 flame 11 flame-core 12 white-hot.
array<int> WTEX = [];       // 4 textures: base, torch A, torch B, skull niche
array<int> FTEX = [];       // 6 textures: flags, puddle, bones, moss, blood, stair runes
array<int> CTEX = [];       // 1 texture: slabs and beams
array<int> WALL_KIND = [];  // per tile: 0 plain, 1 torch sconce, 2 skull niche
array<int> FLOOR_KIND = []; // per tile: FTEX offset

function tex_brick(array<int>& t, int seed) -> void {
	for (int v = 0; v < 32; ++v) {
		int row = v / 8;
		int bv = v % 8;
		for (int u = 0; u < 32; ++u) {
			int uu = (u + (row % 2) * 8) % 32;
			int bu = uu % 16;
			int brick = uu / 16 + row * 2;
			int code = 1;
			if (bv == 7 || bu == 15) { code = 0; }
			else {
				int tone = lat(brick, row, seed) % 5;
				code = tone == 0 ? 2 : (tone == 1 ? 3 : 1);
				if (bv == 0 || bu == 0) { code = code == 2 ? 1 : 3; }
				else if (bv == 6 || bu == 14) { code = 2; }
				else if (lat(u, v, seed + 1) % 13 == 0) { code = code == 3 ? 1 : 2; }
				if (lat(brick, 3, seed) % 4 == 0 && bu > 3 && bu < 7 && bv > 1 && bv < 4) { code = 0; }
			}
			t[v * 32 + u] = code;
		}
	}
}

function tex_block(array<int>& t, int seed) -> void {
	for (int v = 0; v < 32; ++v) {
		int row = v / 16;
		int bv = v % 16;
		for (int u = 0; u < 32; ++u) {
			int uu = (u + row * 8) % 32;
			int bu = uu % 16;
			int code = 1;
			if (bu == 15 || bv == 15) { code = 0; }
			else if (bu == 0 || bv == 0) { code = 3; }
			else if (bu == 14 || bv == 14) { code = 2; }
			else if (lat(u, v, seed) % 9 == 0) { code = 2; }
			else if (lat(u / 4, v / 4, seed + uu / 16 + row * 2) % 5 == 0) { code = 3; }
			t[v * 32 + u] = code;
		}
	}
}

function tex_panel(array<int>& t, int seed) -> void {
	for (int v = 0; v < 32; ++v) {
		for (int u = 0; u < 32; ++u) {
			int bu = u % 16;
			int bv = v % 16;
			bool ru = bu == 3 || bu == 4 || bu == 11 || bu == 12;
			bool rv = bv == 3 || bv == 4 || bv == 11 || bv == 12;
			int code = 1;
			if (bu == 0 || bv == 0) { code = 7; }
			else if (bu == 1 || bv == 1) { code = 3; }
			else if (bu == 15 || bv == 15) { code = 2; }
			else if (ru && rv) { code = (bu == 3 || bu == 11) && (bv == 3 || bv == 11) ? 3 : 4; }
			else if (lat(u / 8, v, seed) % 7 == 0) { code = 2; }
			t[v * 32 + u] = code;
		}
	}
	// orange weeping under the rivets
	for (int u = 0; u < 32; ++u) {
		int bu = u % 16;
		if (bu != 3 && bu != 4 && bu != 11 && bu != 12) { continue; }
		for (int r = 0; r < 4; ++r) {
			int top = (r / 2) * 16 + (r % 2 == 0 ? 5 : 13);
			int len = lat(u, r, seed + 5) % 7;
			for (int k = 0; k < len; ++k) {
				int v = top + k;
				if (v < 32 && v % 16 < 15 && v % 16 != 11 && v % 16 != 12) { t[v * 32 + u] = 5; }
			}
		}
	}
}

function tex_rock(array<int>& t, int seed) -> void {
	for (int v = 0; v < 32; ++v) {
		for (int u = 0; u < 32; ++u) {
			int tone = lat((u + (v / 6) * 3) / 6, v / 6, seed) % 4;
			int code = tone == 0 ? 2 : (tone == 1 ? 3 : 1);
			if (lat(u, v, seed + 2) % 7 == 0) { code = 2; }
			t[v * 32 + u] = code;
		}
	}
	// ember cracks: a vertical meander and a branch that wanders off it
	int cu = 10;
	for (int v = 0; v < 32; ++v) {
		t[v * 32 + cu] = 6;
		t[v * 32 + cu + 1] = 7;
		cu = iclamp(cu + lat(v, 1, seed + 3) % 3 - 1, 3, 27);
	}
	int cv = 18;
	for (int u = 12; u < 32; ++u) {
		t[cv * 32 + u] = 6;
		t[(cv + 1) * 32 + u] = 7;
		cv = iclamp(cv + lat(u, 2, seed + 4) % 3 - 1, 8, 28);
	}
}

function tex_marble(array<int>& t, int seed) -> void {
	for (int v = 0; v < 32; ++v) {
		for (int u = 0; u < 32; ++u) {
			float s = math::sin(u * 0.28 + v * 0.15 + 2.0 * math::sin(v * 0.22 + u * 0.05));
			int code = 1;
			if (s > 0.95) { code = 5; }
			else if (s > 0.84) { code = 2; }
			else if (lat(u, v, seed) % 17 == 0) { code = 3; }
			if (v <= 1 || v == 29) { code = 3; }
			if (v == 2 || v == 30) { code = 2; }
			if (v == 31) { code = 0; }
			t[v * 32 + u] = code;
		}
	}
}

function tex_moss(array<int>& t, int seed) -> void {
	for (int u = 0; u < 32; ++u) {
		int len = lat(u / 2, 0, seed) % 9 + (lat(u / 2, 1, seed) % 4 == 0 ? 9 : 0) + lat(u, 2, seed) % 2;
		for (int v = 0; v < len; ++v) { t[v * 32 + u] = v == len - 1 ? 5 : 4; }
	}
	for (int v = 0; v < 32; ++v) {
		for (int u = 0; u < 32; ++u) {
			if (lat(u / 2, v / 2, seed + 9) % 31 == 0) { t[v * 32 + u] = 4; }
		}
	}
}

function tex_blood(array<int>& t, int seed) -> void {
	for (int u = 0; u < 32; ++u) {
		if (lat(u, 3, seed) % 4 != 0) { continue; }
		int len = 4 + lat(u, 4, seed) % 16;
		for (int v = 0; v < len; ++v) {
			if (t[v * 32 + u] != 0) { t[v * 32 + u] = 5; }
		}
	}
}

function tex_mushrooms(array<int>& t, int seed) -> void {
	for (int b = 0; b < 4; ++b) {
		if (lat(b, 7, seed) % 5 < 2) { continue; }
		int row = b / 2;
		int cu = (b % 2) * 16 - row * 8 + 4 + lat(b, 8, seed) % 8 + 32;
		int v0 = row * 16 + 7;
		for (int du = -1; du <= 1; ++du) { t[v0 * 32 + (cu + du) % 32] = 6; }
		for (int du = -2; du <= 2; ++du) { t[(v0 + 1) * 32 + (cu + du) % 32] = 6; }
		for (int du = -3; du <= 3; ++du) { t[(v0 + 2) * 32 + (cu + du) % 32] = 4; }
		for (int dv = 3; dv <= 6; ++dv) { t[(v0 + dv) * 32 + cu % 32] = 8; }
		t[(v0 + 6) * 32 + (cu - 1) % 32] = 8;
		t[(v0 + 6) * 32 + (cu + 1) % 32] = 8;
	}
}

function stamp(array<int>& t, int u0, int v0, array<string> rows, string key, int code) -> void {
	for (int r = 0; r < rows.size(); ++r) {
		string row = rows[r];
		for (int c = 0; c < row.size(); ++c) {
			if (row[c] == key) { t[(v0 + r) * 32 + u0 + c] = code; }
		}
	}
}

function tex_torch(array<int>& t, int frame) -> void {
	// the torchlight halo first, then bracket and flame over it
	for (int v = 0; v < 32; ++v) {
		for (int u = 0; u < 32; ++u) {
			int du = u - 16;
			int dv = v - 10;
			if (du * du + dv * dv < 110 && t[v * 32 + u] != 0) { t[v * 32 + u] = 3; }
		}
	}
	for (int v = 16; v < 27; ++v) { t[v * 32 + 15] = 9; t[v * 32 + 16] = 9; }
	for (int u = 12; u < 20; ++u) { t[15 * 32 + u] = 9; }
	for (int u = 13; u < 19; ++u) { t[16 * 32 + u] = 9; }
	for (int u = 14; u < 18; ++u) { t[26 * 32 + u] = 9; t[27 * 32 + u] = 9; }
	array<string> flame = frame == 0
		? ["............", ".....a......", ".....aa.....", "....aba.....", "....abaa....", "...aabba....",
		   "...abbba....", "..aabbbaa...", "..abbcbba...", "..abbccba...", "..abcccba...", "...abcba....", "....aaa....."]
		: ["............", "......a.....", ".....aa.....", ".....aba....", "....aaba....", "....abbaa...",
		   "...abbba....", "..aabbbba...", "..abbcbba...", "..abccbba...", "..abcccba...", "...abcba....", "....aaa....."];
	stamp(t, 10, 2, flame, "a", 10);
	stamp(t, 10, 2, flame, "b", 11);
	stamp(t, 10, 2, flame, "c", 12);
}

function tex_skull(array<int>& t) -> void {
	for (int v = 5; v < 30; ++v) {
		for (int u = 7; u < 25; ++u) {
			int du = u * 2 - 31;
			int dv = (13 - v) * 2;
			if (v >= 13 || du * du + dv * dv < 300) { t[v * 32 + u] = 7; }
		}
	}
	for (int u = 6; u < 26; ++u) { t[30 * 32 + u] = 3; t[31 * 32 + u] = 2; }
	array<string> skull = ["..bbbbbb..", ".bbbbbbbb.", "bbbbbbbbbb", "bb..bb..bb", "bb..bb..bb",
		"bbbb..bbbb", ".bbbbbbbb.", "..b.bb.b..", "..bbbbbb..", "...b..b..."];
	stamp(t, 11, 18, skull, "b", 8);
}

// Floor codes: 0 grout 1 base 2 dark 3 lit 4 accent 5 water 6 water-glint 7 bone 8 rune 9 blood.
function tex_flags(array<int>& t, int seed) -> void {
	for (int v = 0; v < 32; ++v) {
		int split = v < 16 ? 19 : 9;
		for (int u = 0; u < 32; ++u) {
			int stone = (v < 16 ? 0 : 2) + (u < split ? 0 : 1);
			int code = 1;
			if (u == 0 || v == 0 || v == 16 || u == split) { code = 0; }
			else {
				code = lat(stone, 5, seed) % 3 == 0 ? 2 : 1;
				if (u == 1 || v == 1 || v == 17 || u == split + 1) { code = 3; }
				else if (u == 31 || v == 15 || v == 31 || u == split - 1) { code = 2; }
				else if (lat(u / 3, v / 3, seed + stone) % 6 == 0) { code = 2; }
				else if (lat(u, v, seed + 6) % 11 == 0) { code = 3; }
			}
			t[v * 32 + u] = code;
		}
	}
}

function tex_floor_decor(array<int>& t, int kind, int seed) -> void {
	for (int v = 0; v < 32; ++v) {
		for (int u = 0; u < 32; ++u) {
			int du = u * 2 - 31;
			int dv = v * 2 - 33;
			int r2 = du * du + dv * dv * 2;
			int code = t[v * 32 + u];
			if (kind == 1) {
				if (r2 < 560) { code = (u + v == 26 || u + v == 27 || u + v == 35) ? 6 : 5; }
				else if (r2 < 700) { code = 2; }
			} else if (kind == 3) {
				if (lat(u / 3, v / 3, seed + 11) % 4 == 0 && lat(u, v, seed) % 3 != 0) { code = 4; }
			} else if (kind == 4) {
				int bu = u - 14;
				int bv = v - 18;
				int b2 = bu * bu + bv * bv;
				if (b2 < 26 || (b2 < 80 && lat(u, v, seed + 12) % 3 == 0) || (b2 < 200 && lat(u, v, seed + 13) % 17 == 0)) { code = 9; }
			} else if (kind == 5) {
				int q = du * du + (v * 2 - 31) * (v * 2 - 31);
				int m = iabs(du) + iabs(v * 2 - 31);
				code = 2;
				if (q > 600 && q < 820) { code = 8; }
				if (m < 16 && m > 10) { code = 8; }
				if ((du == v * 2 - 31 || du == 31 - v * 2) && q > 250 && q < 600) { code = 8; }
			}
			t[v * 32 + u] = code;
		}
	}
	if (kind == 2) {
		array<string> bones = ["..............", ".77.......7...", "..777....777..", "....777..7....", "......777.....",
			"....7...777...", "...777....77..", "....7.........", ".........77...", "........7..7..",
			"........7..7..", ".........77..."];
		for (int r = 0; r < 24; ++r) {
			string row = bones[r / 2];
			for (int c = 0; c < 28; ++c) {
				if (row.substr(c / 2, 1) == "7") { t[(r + 4) * 32 + c + 2] = 7; }
			}
		}
	}
}

// Ceiling codes: 0 joint 1 slab 2 dark 3 lit 4 beam 5 beam-dark 6 root.
function tex_ceiling(array<int>& t, int seed) -> void {
	for (int v = 0; v < 32; ++v) {
		for (int u = 0; u < 32; ++u) {
			int code = 1;
			if (v >= 12 && v <= 19) {
				code = (v == 12 || v == 19 || lat(u / 5, v, seed) % 4 == 0) ? 5 : 4;
			} else if (u % 16 == 0 || v == 0 || v == 31) { code = 0; }
			else if (lat(u, v, seed + 1) % 9 == 0) { code = 2; }
			else if (lat(u / 3, v / 3, seed + 2) % 9 == 0) { code = 3; }
			t[v * 32 + u] = code;
		}
	}
	for (int k = 0; k < 5; ++k) {
		int u = 1 + lat(k, 9, seed) % 30;
		int len = 3 + lat(k, 10, seed) % 6;
		int v0 = lat(k, 11, seed) % 2 == 0 ? 1 : 21;
		for (int d = 0; d < len; ++d) { t[(v0 + d) * 32 + u] = 6; }
	}
}

function bake(array<int>& dst, array<int>& codes, array<int>& mats) -> void {
	for (int i = 0; i < 1024; ++i) { dst.push(mats[codes[i]]); }
}

function fresh_codes() -> array<int> {
	array<int> t = [];
	for (int i = 0; i < 1024; ++i) { t.push(0); }
	return t;
}

// -------------------------------------------------------------- pixel art ---
// A sprite is rows of role letters, '.' transparent. b, d and h take the sprite's own colour
// (body, shade, highlight); the other roles are fixed colours, and the glowing ones stay lit in
// the dark: k outline, e eyes, w teeth, r maw, y gold, g steel, c rune, f F flame, s stone,
// n wood, 1-6 the mural's rainbow, p P q skin, E ember eyes, N dark wood, o white-hot.
string ROLES = "kbdhewrygcfFsn123456pEPqNo";
array<int> ROLE_RGB = [0x120E18, 0, 0, 0, 0xFFEC50, 0xECE6D2, 0xBE1828, 0xFFC428, 0x9698AA, 0x5AFFFA, 0xFF821E,
	0xFFF4AA, 0x807A72, 0x764A28, 0xFF2828, 0xFF9600, 0xFFF028, 0x3CE650, 0x3C78FF, 0xBE46FF, 0xD6A076, 0xFF3C28,
	0xA0663E, 0xF4C79A, 0x4A2C16, 0xFFFCE0];
array<int> ROLE_GLOW = [0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 1];
array<int> ART_W = [];
array<int> ART_H = [];
array<int> ART_OFF = [];
array<int> ART_TEX = [];
map<string, int> ART_ID = {};

function art(string name, array<string> rows) -> void {
	int w = rows[0].size();
	ART_ID[name] = ART_W.size();
	ART_W.push(w);
	ART_H.push(rows.size());
	ART_OFF.push(ART_TEX.size());
	for (int r = 0; r < rows.size(); ++r) {
		string row = rows[r];
		if (row.size() != w) { throw `art '${name}' row ${r} is ${row.size()} wide, expected ${w}`; }
		for (int c = 0; c < w; ++c) { ART_TEX.push(ROLES.find(row.substr(c, 1))); }
	}
}

function art_mural() -> void {
	array<string> rows = [];
	for (int v = 0; v < 16; ++v) {
		string row = "";
		for (int u = 0; u < 16; ++u) {
			string ch = ".";
			if (v < 12) {
				if (u == 0 || u == 15 || v == 0 || v == 11) { ch = "y"; }
				else if (u == 1 || u == 14 || v == 1 || v == 10) { ch = "n"; }
				else { ch = "123456".substr(((u + v * 2) / 3) % 6, 1); }
				if (v >= 3 && v <= 6 && u >= 6 && u <= 9) { ch = (v == 4 && (u == 7 || u == 8)) ? "k" : "y"; }
			} else if (u == 3 || u == 12 || (v == 12 && u == 8)) { ch = "n"; }
			row += ch;
		}
		rows.push(row);
	}
	art("mural", rows);
}

function build_art() -> void {
	art("g", [
		"................", ".k....kkkk....k.", ".bk..kbbbbk..kb.", ".bbkkbbbbbbkkbb.",
		"..bbbbhhbbbbbb..", "..bbeebbbbeebb..", "..bbekbbbbekbb..", "..bbbbbbbbbbbb..",
		"..bkwkwkwkwkwb..", "..bbrrrrrrrrbb..", "...bbbbbbbbbb...", "..dbbbbbbbbbbd..",
		".dd.bbbbbbbb.dd.", "....dbb..bbd....", "....dd....dd....", "...kkk....kkk..."]);
	art("s", [
		"............gg..", "...........n..n.", "....kkkk..n...n.", "...kddddk.n..n..",
		"..kddddddkn.n...", "..kdeeddeedb....", "..bddddddddb....", "..bdkwkwkwdb....",
		"...dddddddd.....", "..bnnnnnnnnb....", "..bddddddddb....", "...dddddddd.....",
		"...dd....dd.....", "...dd....dd.....", "..kkk....kkk....", "................"]);
	art("o", [
		"................", "................", "................", "................",
		"......kkkk......", "....kkbbbbkk....", "...kbhhbbbbbk...", "..kbhhbbwwbbbk..",
		"..kbbbbwkewbbk..", ".kbbbbbbwwbbbbk.", ".kbbhbbbbbbbbbk.", "kbbbbbbbbbhbbbbk",
		"kbbbbbbbbbbbbbbk", "kdbbbbdbbbbdbbdk", ".kddddddddddddk.", "..kkkkkkkkkkkk.."]);
	art("b", [
		"................", "................", "k..............k", "dk....k..k....kd",
		"ddk...kbbk...kdd", "dddk.kbbbbk.kddd", "ddddkbebbebkdddd", "dddddbbbbbbddddd",
		"dkdddbwbbwbdddkd", "k.kddbbrrbbddk.k", "...kd.bbbb.dk...", "....k..bb..k....",
		"................", "................", "................", "................"]);
	art("k", [
		"................", "................", "................", "....kk..........",
		"...kbbk.kk......", "...kbhbkbbk.....", "....kbbbbbbk....", "...kbbebbbbbk...",
		"..kbbbbbbbbbbk..", ".kkbbwbbbbhbbbk.", ".krbbbbbbbbhbbbk", "..kkbbddbbbbbbbk",
		"....kbbddbbbbbk.", "....kdd.kddddk.d", "...kkk..kkkk..dd", "..............d."]);
	art("r", [
		".....kkkkkk.....", "....kbbbbbbk....", "....bkkbbkkb....", "....bkebbkeb....",
		"....bbbkkbbb....", ".....bwbwbwb....", "......kbbk......", "...bbbbbbbbbb...",
		"...b.bdbdbd.b...", "...b.bdbdbd.b...", "...b.bdbdbd.bg..", "...b..bbbb..bg..",
		"......b..b...g..", "......b..b......", "......b..b......", ".....bb..bb....."]);
	art("chest_open", [
		"................", "................", "................", "..kkkkkkkkkkkk..",
		".knnnnnnnnnnnnk.", ".knyyyyyyyyyynk.", ".kwkwkwkwkwkwkk.", ".krreerrrreerrk.",
		".krrrrrrrrrrrrk.", ".krrrrrrrrrrrrk.", ".kwkwkwkwkwkwkk.", ".knnnnnnnnnnnnk.",
		".knnnnyyyynnnnk.", ".knnnnykkynnnnk.", ".knnnnnnnnnnnnk.", "..kkkkkkkkkkkk.."]);
	art("chest", [
		"................", "................", "................", "................",
		"..kkkkkkkkkkkk..", ".knnhhhhhhhhnnk.", ".knnnnnnnnnnnnk.", ".kyyyyyyyyyyyyk.",
		".knnnnnyynnnnnk.", ".knnnnykkynnnnk.", ".knnnnnyynnnnnk.", ".knnnnnnnnnnnnk.",
		".kyyyyyyyyyyyyk.", ".knnnnnnnnnnnnk.", ".knnnnnnnnnnnnk.", "..kkkkkkkkkkkk.."]);
	art("O", [
		".kk..........kk.", ".kwk..kkkk..kwk.", "..kwkkbbbbkkwk..", "...kbbbbbbbbk...",
		"..kbbEEbbEEbbk..", "..kbbbbbbbbbbk..", "..kbwkkkkkkwbk..", "...kbrrrrrrbk...",
		".hhbbbbbbbbbbhh.", "hbbbbbhbbhbbbbbh", "bbd.bbbbbbbb.dbb", "bd..bbbbbbbb..db",
		"kk..nnnnnnnn..kk", "....dddddddd....", "...ddd....ddd...", "..kkkk....kkkk.."]);
	art("m", [
		"................", "....k......k....", ".....k....k.....", "hhh...kbbk...hhh",
		"hbbh..bbbb..hbbh", "bb1bb.beeb.bb1bb", "bb11bbbbbbbb11bb", "bbbbbbbbbbbbbbbb",
		".bdddbbbbbbdddb.", "..bbd.bbbb.dbb..", "..bb1b.bb.b1bb..", "...bbb.bb.bbb...",
		"....b..bb..b....", "......kbbk......", "................", "................"]);
	art("W", [
		"..kk........kk..", ".kbbk......kbbk.", ".kbbbkkkkkkbbbk.", "kbbEbbbbbbbbEbbk",
		"kbbbbbbbbbbbbbbk", "kbwkwkwkwkwkwkbk", "kbrrrrrrrrrrrrbk", "kbrrrrrrrrrrrrbk",
		"kbrrrrkkkkrrrrbk", "kbrrrrrrrrrrrrbk", "kbwkwkwkwkwkwkbk", "kbbbbbbbbbbbbbbk",
		".kbbhbbbbbbhbbk.", "..dddd....dddd..", "..dd........dd..", ".kkk........kkk."]);
	art("w", [
		".......f........", "......ff....f...", "...f..fFf..ff...", "...ff.fFff.f....",
		"....ffFFFffff...", "...fffFFFFff....", "..ffFFFFFFFff...", "..fFFeFFFeFFf...",
		"..fFFFFFFFFFf...", "..ffFFFrrFFff...", "...ffFFFFFFf....", "....fffFFff.....",
		".....ffffff.....", "......ffff......", ".......ff.......", "................"]);
	art("d", [
		"................", ".k...........kk.", "kdk.........kbwk", "kddk.......kbbbk",
		"kdddk.....kbbEbk", "kddddk...kbbbbbk", ".kddddkkkbbbrrwk", "..kdddbbbbbbkkk.",
		"...kbbhhhbbbk...", "...bbhhhhhbbb...", "..bbbbhhhbbbbd..", ".dbb.bbbbb.bbdd.",
		".dd..bb.bb..dddk", "....bb...bb...kk", "...kkk...kkk....", "................"]);
	art("V", [
		"k..................k", "bk....kk....kk....kb", "bbk..kbbk..kbbk..kbb", "bbbk.kbbbkkbbbk.kbbb",
		"dbbbkbbbbbbbbbbkbbbd", "ddbbbbbbbbbbbbbbbbdd", ".ddbbbbccbbbbccbbdd.", "..dbbbbccbbbbccbbd..",
		"...bbbbbbbbbbbbbb...", "...bbwkwkwkwkwkbb...", "...bbkccccccccckb...", "...bbwkwkwkwkwkbb...",
		"..hbbbbbbbbbbbbbbh..", ".hbbbbbhhhhhhbbbbbh.", "hbbbb.bhhhhhhb.bbbbh", "bbbd..bbhhhhbb..dbbb",
		"bdd...bbbbbbbb...ddb", "kk...ddbb..bbdd...kk", ".....ddd....ddd.....", "....kkkk....kkkk...."]);
	art(")", [
		"............", "..........g.", ".........gh.", "........gh..",
		".......gh...", "......gh....", "..y..gh.....", "...ygh......",
		"....y.......", "...n.y......", "..n.........", ".k.........."]);
	art("[", [
		"............", "..kk....kk..", ".kbbkkkkbbk.", ".kbhbbbbhbk.",
		".kbhbbbbbbk.", "..kbbbbbbk..", "..kbbhbbbk..", "..kbbbbbbk..",
		"..kyyyyyyk..", "..kbbbbbbk..", "...kbbbbk...", "....kkkk...."]);
	art("=", [
		"............", "............", "............", "....yyyy....",
		"...y....y...", "..y......y..", "..y......y..", "..y......y..",
		"...y....y...", "....ybby....", ".....hb.....", "......b....."]);
	art("!", [
		"............", "....nnnn....", ".....gg.....", ".....gg.....",
		"....gbbg....", "...gbhbbg...", "..gbhbbbbg..", "..gbbbbbbg..",
		"..gbbbbbbg..", "..gdbbbbdg..", "...gddddg...", "....gggg...."]);
	art("~", [
		"............", "............", "............", "............",
		"............", ".kwwwwwwwwk.", "kwhwwwwwwhwk", "kwwwwbbwwwwk",
		"kwwwwbbwwwwk", "kwhwwwwwwhwk", ".kwwwwwwwwk.", "............"]);
	art("$", [
		"............", "............", "............", "............",
		"............", ".....hh.....", "....hbbh....", "...hbbhbbh..",
		"..hbbbbhbbh.", ".hbbhbbbbbbh", "hbbbbbhbbbbb", "dddddddddddd"]);
	art("stairs", [
		"....ssssssss....", "..sshhhhhhhhss..", ".sskkkkkkkkkkss.", ".skkkkkkkkkkkks.",
		"sskkcccccccckkss", "skkkkkkkkkkkkkks", "skkkkccccccckkks", "skkkkkkkkkkkkkks",
		"skkkkkkcccckkkks", "skkkkkkkkkkkkkks", "sskkkkkkkcckkkss", "ssssssssssssssss"]);
	art("statue", [
		"....ss........", "...ssss.......", "...syssssss...", "...sssswwws...",
		"....ssssss....", "..ssssssssss..", ".ss.ssssss.ss.", ".s..ssssss..s.",
		"....ssssss....", "....ss..ss....", "...sss..sss...", "..yyyyyyyyyy..",
		"..ssssssssss..", "..ssssssssss..", ".ssssssssssss.", ".kkkkkkkkkkkk."]);
	art_mural();
	art("torch_a", [
		"......o.........", ".....fFf........", "....fFoFf.......", "...ffFooFf......",
		"...fFooooFf.....", "..ffFoooFff.....", "...fFFoFFf......", "....ffFff.......",
		".....NnnN.......", ".....NnnN.......", "......Nn........", "......Nn........",
		"....kPNnPk......", "...kqpPNpppk....", "..kqppqpqppPk...", "..kpPpPpPpPPk...",
		"..kppppppppPk...", "...kPppppPPk....", "...dddddddd.....", "..dhhhhhhhhd....",
		"..dbbbbbbbbd....", ".dbbbbbbbbbbd..."]);
	art("torch_b", [
		".......o........", "......fFf.......", ".....fFoFf......", "....fFooFff.....",
		"...fFooooFf.....", "...fFoooFFf.....", "...ffFoFFf......", "....ffFff.......",
		".....NnnN.......", ".....NnnN.......", "......Nn........", "......Nn........",
		"....kPNnPk......", "...kqpPNpppk....", "..kqppqpqppPk...", "..kpPpPpPpPPk...",
		"..kppppppppPk...", "...kPppppPPk....", "...dddddddd.....", "..dhhhhhhhhd....",
		"..dbbbbbbbbd....", ".dbbbbbbbbbbd..."]);
	art("blade", [
		"..............g.", ".............gh.", "............gh..", "...........gh...",
		"..........gh....", ".........gh.....", "........gh......", ".......gh.......",
		"..yy..gh........", "...yygh.........", "....yy..........", "...kPpk.........",
		"..kqpppk........", "..kpPpPk........", ".kpppppPk.......", ".kPpppPPk.......",
		"..dddddd........", ".dhhhhhhd.......", ".dbbbbbbd.......", "dbbbbbbbbd......"]);
}
