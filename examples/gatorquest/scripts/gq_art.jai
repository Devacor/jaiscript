// Gator Quest - the art: named colors, procedural tile textures (4 variants x
// 4 animation frames, baked once per tile size), 8x8 tinted sprites written
// as strings, and tiny ground-item icons. All cached as palette indices.

import "gq_vid.jai";

// ---- named colors
array<string> COLOR_NAMES = ["white", "black", "gray", "darkgray", "red", "crimson", "green", "moss", "olive", "teal", "cyan", "blue",
	"navy", "purple", "pink", "yellow", "gold", "orange", "brown", "tan", "straw", "leather", "wood", "steel", "silver", "iron", "rust",
	"bone", "ice", "lava", "obsidian", "slime", "ghost", "ghoul", "wisp", "char", "paper", "pearl", "copper", "mithril", "sky", "snow",
	"stone", "hue0", "hue1", "hue2", "hue3", "hue4", "hue5", "hue6", "hue7", "hue8", "hue9", "hue10", "hue11", "hue12", "hue13", "hue14", "hue15"];
array<int> COLOR_RGB = [
	255, 255, 255, 24, 22, 28, 128, 128, 136, 70, 70, 78, 214, 48, 40, 150, 24, 44, 70, 178, 64, 96, 124, 52, 124, 128, 44,
	40, 150, 146, 90, 220, 232, 60, 104, 224, 36, 48, 118, 156, 76, 206, 240, 150, 176, 244, 222, 70, 244, 192, 44, 244, 140, 40,
	124, 82, 42, 204, 172, 122, 222, 202, 112, 146, 94, 52, 136, 92, 50, 172, 182, 194, 204, 210, 220, 114, 114, 124, 164, 84, 42,
	232, 222, 190, 172, 222, 255, 255, 104, 30, 56, 34, 70, 124, 224, 64, 204, 232, 244, 124, 164, 112, 184, 255, 204, 64, 44, 34,
	236, 226, 200, 236, 236, 246, 204, 124, 72, 150, 222, 232, 150, 202, 255, 240, 246, 255, 144, 144, 144,
	// player gator hues
	82, 150, 60, 60, 130, 100, 150, 140, 50, 110, 90, 60, 60, 110, 150, 150, 70, 60, 120, 80, 150, 80, 160, 140,
	170, 120, 60, 90, 90, 90, 200, 190, 170, 60, 170, 90, 150, 50, 90, 50, 90, 60, 200, 150, 60, 40, 140, 170
];
map<string, int> COLOR_INDEX;

void colors_init() {
	COLOR_INDEX = {};
	for (int i = 0; i < COLOR_NAMES.size(); ++i) { COLOR_INDEX[COLOR_NAMES[i]] = i; }
}

int crgb(string name) {
	int i = COLOR_INDEX.get(name, 2);
	return rgb(COLOR_RGB[i * 3], COLOR_RGB[i * 3 + 1], COLOR_RGB[i * 3 + 2]);
}

// scale a named color: pct 100 = itself
int cscale(string name, int pct) {
	int i = COLOR_INDEX.get(name, 2);
	return rgb(COLOR_RGB[i * 3] * pct / 100, COLOR_RGB[i * 3 + 1] * pct / 100, COLOR_RGB[i * 3 + 2] * pct / 100);
}

int clight(string name, int pct) {
	int i = COLOR_INDEX.get(name, 2);
	int r = COLOR_RGB[i * 3];
	int g = COLOR_RGB[i * 3 + 1];
	int b = COLOR_RGB[i * 3 + 2];
	return rgb(r + (255 - r) * pct / 100, g + (255 - g) * pct / 100, b + (255 - b) * pct / 100);
}

// ------------------------------------------------------------ textures ---
int TS;                        // tile size in pixels (4, 6 or 8); 0 until art_init
array<int> TEX;                // every tile texture, TS*TS each
array<int> TEX_OFF;            // (tile * 4 + variant) * 4 + frame -> offset into TEX
array<int> TILE_ANIM;          // 1 if the tile animates

// a speckle test on the 8x8 design grid, stable across tile sizes
bool speck(int u, int v, int salt, int permille) { return (hash3(u, v, salt) % 1000) < permille; }

bool disc_hit(int fx, int fy, int cx, int cy, int r) { return (fx - cx) * (fx - cx) + (fy - cy) * (fy - cy) <= r * r; }

int grass_px(int u, int v, int vr) {
	if (speck(u, v, 11 + vr, 180)) { return rgb(44, 112, 40); }
	if (speck(u, v, 23 + vr, 140)) { return rgb(96, 176, 64); }
	return rgb(64, 144, 52);
}

int water_px(int u, int v, int f, int r, int g, int b) {
	int wave = (u + v * 3 + f * 2) % 8;
	if (wave == 0 && speck(u, v / 2, 5, 600)) { return rgb(r + 60, g + 70, b + 60); }
	if (wave == 1) { return rgb(r + 22, g + 26, b + 24); }
	return rgb(r, g, b);
}

int tex_pixel(int t, int vr, int f, int px, int py, int S) {
	int u = px * 8 / S;
	int v = py * 8 / S;
	int fx = (px * 2 + 1) * 500 / S;
	int fy = (py * 2 + 1) * 500 / S;
	if (t == T_VOID) { return rgb(6, 6, 10); }
	if (t == T_GRASS) {
		if (vr == 3 && u == 5 && v == 2) { return rgb(240, 230, 120); }
		return grass_px(u, v, vr);
	}
	if (t == T_TALLGRASS) {
		if ((u + vr) % 3 == 0 && v < 6) { return v < 2 ? rgb(130, 190, 80) : rgb(60, 140, 50); }
		return (u + v) % 4 == 0 ? rgb(40, 104, 38) : rgb(70, 150, 56);
	}
	if (t == T_FLOWERS) {
		if (speck(u, v, 77 + vr, 160)) {
			array<int> petals = [rgb(240, 80, 90), rgb(250, 220, 80), rgb(190, 110, 230), rgb(250, 250, 250)];
			return petals[(u * 3 + v + vr) % 4];
		}
		return grass_px(u, v, vr);
	}
	if (t == T_DIRT) {
		if (speck(u, v, 31 + vr, 150)) { return rgb(100, 72, 44); }
		if (speck(u, v, 37 + vr, 90)) { return rgb(150, 116, 74); }
		return rgb(124, 92, 58);
	}
	if (t == T_ROAD) {
		int bx = (u + (v / 2 % 2) * 2) % 4;
		if (v % 2 == 1 && bx == 3 || v % 2 == 0 && speck(u, v, 41, 120)) { return rgb(96, 92, 86); }
		return speck(u, v, 43 + vr, 300) ? rgb(162, 156, 146) : rgb(146, 140, 130);
	}
	if (t == T_SAND) {
		if (speck(u, v, 51 + vr, 160)) { return rgb(204, 182, 124); }
		if (speck(u, v, 53 + vr, 100)) { return rgb(238, 220, 166); }
		return rgb(224, 204, 146);
	}
	if (t == T_SHALLOW) {
		if (speck(u, v, 61 + vr, 140)) { return rgb(150, 170, 150); }
		return water_px(u, v, f, 70, 150, 196);
	}
	if (t == T_DEEP) { return water_px(u, v, f, 28, 66, 146); }
	if (t == T_SWAMP) {
		if (f == vr && u == 3 && v == 4) { return rgb(120, 150, 110); }
		return water_px(u, v, f, 38, 78, 58);
	}
	if (t == T_SEWAGE) { return water_px(u, v, f, 70, 90, 40); }
	if (t == T_MUD) { return speck(u, v, 71 + vr, 200) ? rgb(70, 50, 30) : rgb(96, 70, 44); }
	if (t == T_BOG) {
		if (speck(u, v, 81 + vr, 120)) { return rgb(50, 70, 50); }
		return speck(u, v, 83, 200) ? rgb(58, 66, 36) : rgb(70, 76, 42);
	}
	if (t == T_OAK || t == T_PINE || t == T_CYPRESS || t == T_DEADTREE || t == T_BUSH) {
		int bg = t == T_CYPRESS ? water_px(u, v, f, 38, 78, 58) : (t == T_DEADTREE ? rgb(70, 76, 42) : grass_px(u, v, vr));
		if (t == T_OAK) {
			if (disc_hit(fx, fy, 500, 380, 360)) { return speck(u, v, 91 + vr, 300) ? rgb(96, 170, 60) : (fy > 480 ? rgb(30, 92, 32) : rgb(44, 120, 40)); }
			if (fx > 400 && fx < 600 && fy >= 700) { return rgb(100, 66, 34); }
			return bg;
		}
		if (t == T_PINE) {
			int half = (fy - 60) * 380 / 820;
			if (fy > 60 && fy < 820 && iabs(fx - 500) < half) { return (fx + fy / 3) % 250 < 60 ? rgb(40, 110, 60) : rgb(24, 80, 44); }
			if (fx > 420 && fx < 580 && fy >= 800) { return rgb(96, 64, 34); }
			return bg;
		}
		if (t == T_CYPRESS) {
			if (iabs(fx - 500) < 260 - iabs(fy - 400) / 4 && fy < 780) { return speck(u, v, 97, 250) ? rgb(130, 150, 110) : rgb(52, 92, 40); }
			if (fx > 380 && fx < 620 && fy >= 700) { return rgb(90, 70, 50); }
			return bg;
		}
		if (t == T_DEADTREE) {
			if ((iabs(fx - 500) < 90 && fy > 200) || (fy > 250 && fy < 350 && fx > 250 && fx < 750) || (fy > 150 && fy < 250 && (fx < 300 || fx > 700) && fx > 150 && fx < 850)) { return rgb(96, 84, 70); }
			return bg;
		}
		if (disc_hit(fx, fy, 500, 560, 330)) {
			if (vr == 1 && speck(u, v, 99, 150)) { return rgb(200, 40, 60); }
			return speck(u, v, 98, 300) ? rgb(70, 150, 60) : rgb(40, 110, 40);
		}
		return bg;
	}
	if (t == T_BOULDER || t == T_ORE || t == T_RUBBLE) {
		int bg = t == T_RUBBLE ? rgb(124, 92, 58) : grass_px(u, v, vr);
		if (t == T_RUBBLE) { return speck(u, v, 101, 300) ? rgb(120, 120, 124) : bg; }
		if (disc_hit(fx, fy, 500, 560, 400)) {
			if (t == T_ORE && speck(u, v, 103 + vr, 260)) { return vr % 2 == 0 ? rgb(214, 110, 50) : rgb(210, 214, 224); }
			if (fy < 400 && fx < 480) { return rgb(176, 176, 180); }
			if (fy > 720) { return rgb(84, 84, 90); }
			return rgb(130, 130, 136);
		}
		return bg;
	}
	if (t == T_MOUNTAIN) {
		if (v < 2 && speck(u, v, 111, 500)) { return rgb(240, 244, 250); }
		return speck(u, v, 113 + vr, 300) ? rgb(96, 84, 72) : rgb(122, 108, 92);
	}
	if (t == T_CAVEWALL || t == T_BASALT) {
		bool crack = speck(u, v, 121 + vr, 120);
		if (t == T_BASALT) { return crack ? rgb(150, 40, 20) : (v < 2 ? rgb(70, 56, 56) : rgb(44, 34, 34)); }
		return crack ? rgb(40, 34, 30) : (v < 2 ? rgb(96, 86, 76) : rgb(70, 62, 56));
	}
	if (t == T_CAVEFLOOR) { return speck(u, v, 131 + vr, 180) ? rgb(76, 68, 60) : rgb(96, 86, 76); }
	if (t == T_SEWERFLOOR) { return speck(u, v, 133 + vr, 160) ? rgb(80, 110, 50) : rgb(72, 76, 66); }
	if (t == T_WOODFLOOR) {
		if (v % 3 == 2) { return rgb(96, 64, 38); }
		return (u + v * 5) % 7 == 0 ? rgb(130, 90, 52) : rgb(156, 110, 64);
	}
	if (t == T_STONEFLOOR || t == T_RUINFLOOR) {
		if (u % 4 == 3 || v % 4 == 3) { return rgb(96, 96, 92); }
		if (t == T_RUINFLOOR && speck(u, v, 141 + vr, 200)) { return rgb(70, 110, 70); }
		return speck(u, v, 143, 200) ? rgb(142, 142, 136) : rgb(128, 128, 122);
	}
	if (t == T_CARPET) {
		if (u == 0 || u == 7 || v == 0 || v == 7) { return rgb(200, 160, 50); }
		return (u + v) % 2 == 0 ? rgb(150, 30, 40) : rgb(130, 24, 34);
	}
	if (t == T_BRICK || t == T_SCONCE) {
		int row = v / 2;
		int off = row % 2 == 0 ? 0 : 2;
		bool mortar = v % 2 == 1 || (u + off) % 4 == 3;
		if (t == T_SCONCE && u >= 3 && u <= 4 && v >= 1 && v <= 4) {
			if (v >= 3) { return rgb(90, 60, 30); }
			array<int> flame = [rgb(255, 220, 90), rgb(255, 150, 40), rgb(255, 240, 160), rgb(240, 110, 30)];
			return flame[(f + u + v) % 4];
		}
		if (mortar) { return rgb(110, 100, 90); }
		return v < 2 ? rgb(170, 84, 60) : rgb(148, 68, 48);
	}
	if (t == T_WOODWALL) {
		if (u % 3 == 2) { return rgb(76, 50, 26); }
		return v < 2 ? rgb(136, 94, 52) : rgb(112, 76, 40);
	}
	if (t == T_RUINWALL || t == T_ICEWALL) {
		bool seam = v % 3 == 2 || (u + (v / 3) * 2) % 4 == 3;
		if (t == T_ICEWALL) { return seam ? rgb(90, 140, 200) : (speck(u, v, 151, 200) ? rgb(220, 240, 255) : rgb(130, 180, 230)); }
		if (speck(u, v, 153 + vr, 180)) { return rgb(76, 116, 70); }
		return seam ? rgb(80, 84, 80) : rgb(116, 120, 114);
	}
	if (t == T_DOOR) {
		if (u == 0 || u == 7 || v == 0) { return rgb(76, 50, 26); }
		if (u == 5 && v == 4) { return rgb(240, 200, 60); }
		return u % 2 == 0 ? rgb(150, 100, 56) : rgb(130, 86, 46);
	}
	if (t == T_COUNTER) {
		if (v < 3) { return v == 0 ? rgb(200, 150, 90) : rgb(176, 124, 72); }
		return u % 4 == 0 ? rgb(90, 60, 34) : rgb(120, 80, 44);
	}
	if (t == T_ANVIL) {
		if ((v == 2 || v == 3) && u >= 1 && u <= 6) { return v == 2 ? rgb(120, 120, 130) : rgb(80, 80, 90); }
		if (v >= 4 && v <= 6 && u >= 3 && u <= 4) { return rgb(60, 60, 70); }
		if (v == 6 && u >= 2 && u <= 5) { return rgb(60, 60, 70); }
		return tex_pixel(T_STONEFLOOR, vr, 0, px, py, S);
	}
	if (t == T_FORGE || t == T_FIRE) {
		bool ring = disc_hit(fx, fy, 500, 560, 420) && !disc_hit(fx, fy, 500, 560, 300);
		if (t == T_FORGE && (u == 0 || u == 7 || v == 0)) { return rgb(148, 68, 48); }
		if (ring && t == T_FIRE) { return speck(u, v, 161, 400) ? rgb(100, 100, 100) : rgb(130, 126, 120); }
		if (disc_hit(fx, fy, 500, 560, 300) || t == T_FORGE && v >= 3) {
			array<int> flame = [rgb(255, 230, 120), rgb(255, 160, 40), rgb(230, 80, 20), rgb(255, 200, 60)];
			int k = (u * 5 + v * 3 + f) % 4;
			if (v < 3 && t == T_FIRE && (u + f) % 2 == 0) { return rgb(255, 120, 30); }
			return flame[k];
		}
		return t == T_FIRE ? grass_px(u, v, vr) : rgb(60, 40, 30);
	}
	if (t == T_ALCHEMY) {
		if (v >= 4) { return u % 4 == 0 ? rgb(90, 60, 34) : rgb(150, 104, 60); }
		if (v >= 1 && (u == 1 || u == 2)) { return f % 2 == 0 ? rgb(120, 240, 90) : rgb(90, 200, 70); }
		if (v >= 2 && (u == 4 || u == 5)) { return f % 2 == 0 ? rgb(200, 100, 240) : rgb(170, 80, 220); }
		if (v == 0 && u == 1) { return rgb(200, 255, 200); }
		return tex_pixel(T_WOODFLOOR, vr, 0, px, py, S);
	}
	if (t == T_BANK || t == T_CHEST) {
		if (v >= 2 && v <= 7 && u >= 1 && u <= 6) {
			if (v == 4 || u == 1 || u == 6) { return rgb(220, 180, 60); }
			if (v == 5 && (u == 3 || u == 4)) { return rgb(250, 230, 120); }
			return v < 4 ? rgb(150, 96, 50) : rgb(120, 76, 40);
		}
		return t == T_BANK ? tex_pixel(T_WOODFLOOR, vr, 0, px, py, S) : rgb(96, 86, 76);
	}
	if (t == T_BED) {
		if (v < 3 && u >= 1 && u <= 6) { return rgb(240, 240, 230); }
		if (u >= 1 && u <= 6) { return (u + v) % 3 == 0 ? rgb(170, 40, 50) : rgb(200, 50, 60); }
		return tex_pixel(T_WOODFLOOR, vr, 0, px, py, S);
	}
	if (t == T_FENCE) {
		if (u == 1 || u == 6 || v == 2 || v == 5) { return u == 1 || u == 6 ? rgb(130, 90, 50) : rgb(160, 116, 70); }
		return grass_px(u, v, vr);
	}
	if (t == T_BRIDGE) {
		if (u == 0 || u == 7) { return rgb(90, 60, 30); }
		return v % 2 == 0 ? rgb(150, 104, 60) : rgb(132, 90, 50);
	}
	if (t == T_LAVA) {
		int k = (vnoise(u + f * 2, v + f, 3, 7 + vr) + (u * 37 + v * 23 + f * 90) % 200) % 1024;
		if (k < 300) { return rgb(255, 210, 60); }
		if (k < 700) { return rgb(250, 110, 20); }
		return rgb(190, 40, 10);
	}
	if (t == T_OBSIDIAN) {
		if ((u + v) % 5 == 0 && speck(u, v, 171, 500)) { return rgb(140, 100, 190); }
		return speck(u, v, 173 + vr, 300) ? rgb(40, 24, 56) : rgb(56, 36, 72);
	}
	if (t == T_ASH) { return speck(u, v, 181 + vr, 200) ? rgb(60, 56, 56) : (speck(u, v, 183, 60) ? rgb(200, 90, 40) : rgb(86, 80, 78)); }
	if (t == T_ICE) {
		if ((u - v + vr + 8) % 6 == 0) { return rgb(240, 250, 255); }
		return speck(u, v, 191, 150) ? rgb(150, 200, 230) : rgb(180, 222, 244);
	}
	if (t == T_SNOW) { return speck(u, v, 193 + vr, 180) ? rgb(200, 214, 236) : rgb(238, 242, 252); }
	if (t == T_CLOUD) {
		if (speck(u, v, 195 + vr, 250)) { return rgb(214, 222, 238); }
		return rgb(244, 246, 255);
	}
	if (t == T_SKY) {
		int drift = (u + f + v * 2 + vr * 3) % 12;
		if (drift < 2 && speck(u / 2, v, 197, 400)) { return rgb(196, 222, 255); }
		return v < 4 ? rgb(126, 176, 240) : rgb(110, 160, 230);
	}
	if (t == T_RAINBOW) {
		array<int> bands = [rgb(240, 60, 60), rgb(250, 150, 40), rgb(250, 230, 60), rgb(80, 210, 80), rgb(70, 140, 250), rgb(160, 80, 230), rgb(250, 150, 40), rgb(240, 60, 60)];
		int k = (v + f / 2) % 8;
		return (u + f) % 7 == 0 ? rgb(255, 255, 255) : bands[k];
	}
	if (t == T_CORAL) {
		if ((u % 3 == 1 && v > 2) || (v == 3 && u > 0 && u < 7) || (v == 1 && u % 3 == 0)) { return vr % 2 == 0 ? rgb(250, 120, 140) : rgb(250, 160, 80); }
		return water_px(u, v, f, 70, 150, 196);
	}
	if (t == T_KELP) {
		int sway = (v + f) % 4 < 2 ? 0 : 1;
		if ((u + sway) % 3 == 1) { return v < 3 ? rgb(90, 170, 70) : rgb(40, 110, 50); }
		return water_px(u, v, f, 28, 66, 146);
	}
	if (t == T_LILY) {
		if (disc_hit(fx, fy, 500, 500, 380) && !(fx > 480 && fy < 500 && fx - 500 < (500 - fy) / 2)) {
			if (vr == 2 && disc_hit(fx, fy, 380, 400, 110)) { return rgb(250, 160, 200); }
			return speck(u, v, 201, 200) ? rgb(110, 190, 70) : rgb(70, 160, 60);
		}
		return water_px(u, v, f, 38, 78, 58);
	}
	if (t == T_REEDS) {
		if ((u + vr) % 2 == 0 && v >= 1) { return v <= 2 ? rgb(150, 110, 60) : rgb(120, 150, 70); }
		return rgb(96, 70, 44);
	}
	if (t == T_GRAVE) {
		if (u >= 2 && u <= 5 && v >= 1 && v <= 6) {
			if ((u == 3 || u == 4) && v == 1) { return rgb(160, 160, 164); }
			if (v == 3 && u >= 3 && u <= 4) { return rgb(80, 80, 84); }
			return rgb(140, 140, 146);
		}
		return rgb(70, 76, 42);
	}
	if (t == T_GATE) {
		if (disc_hit(fx, fy, 500, 500, 460)) {
			int ring = (iabs(fx - 500) + iabs(fy - 500)) / 90 + f;
			array<int> swirl = [rgb(90, 60, 230), rgb(140, 100, 250), rgb(60, 160, 250), rgb(200, 180, 255)];
			return swirl[ring % 4];
		}
		return rgb(128, 128, 122);
	}
	if (t == T_STAIRS_DN || t == T_STAIRS_UP) {
		int step = t == T_STAIRS_DN ? v : 7 - v;
		if (u == 0 || u == 7) { return rgb(96, 96, 92); }
		return rgb(160 - step * 16, 156 - step * 16, 150 - step * 16);
	}
	if (t == T_SHRINE) {
		if ((u == 3 || u == 4) && v >= 1 && v <= 6 || v == 2 && u >= 2 && u <= 5) { return f % 2 == 0 ? rgb(255, 240, 150) : rgb(255, 220, 90); }
		if (v == 7 || v == 0) { return rgb(230, 230, 220); }
		return rgb(190, 190, 180);
	}
	if (t == T_CRYSTAL) {
		if ((iabs(fx - 300) < 120 && fy > 200) || (iabs(fx - 650) < 140 && fy > 350) || (iabs(fx - 480) < 90 && fy > 100 && fy < 600)) {
			return (u + v + f) % 3 == 0 ? rgb(220, 255, 255) : rgb(90, 220, 240);
		}
		return rgb(130, 180, 230);
	}
	if (t == T_STUMP) {
		if (disc_hit(fx, fy, 500, 560, 260)) { return disc_hit(fx, fy, 500, 560, 120) ? rgb(200, 160, 100) : rgb(110, 76, 40); }
		return grass_px(u, v, vr);
	}
	if (t == T_SIGN) {
		if (v >= 1 && v <= 4 && u >= 1 && u <= 6) { return v == 2 || v == 3 ? rgb(90, 60, 30) : rgb(180, 130, 80); }
		if (u >= 3 && u <= 4 && v > 4) { return rgb(100, 66, 34); }
		return rgb(146, 140, 130);
	}
	if (t == T_WELL) {
		if (disc_hit(fx, fy, 500, 500, 420)) { return disc_hit(fx, fy, 500, 500, 260) ? water_px(u, v, f, 20, 40, 80) : rgb(140, 140, 136); }
		return rgb(146, 140, 130);
	}
	if (t == T_PILLAR) {
		if (u >= 2 && u <= 5) { return u == 2 ? rgb(220, 220, 214) : (u == 5 ? rgb(140, 140, 136) : rgb(190, 190, 184)); }
		return tex_pixel(T_RUINFLOOR, vr, 0, px, py, S);
	}
	if (t == T_GOLDPILE) {
		if (disc_hit(fx, fy, 500, 620, 380)) {
			if ((u * 3 + v + f * 5) % 11 == 0) { return rgb(255, 255, 220); }
			return speck(u, v, 211, 400) ? rgb(250, 220, 90) : rgb(220, 170, 40);
		}
		return rgb(96, 86, 76);
	}
	if (t == T_WEB) {
		if (u == v || u + v == 7 || u == 3 || v == 4) { return rgb(220, 220, 230); }
		return rgb(70, 76, 42);
	}
	return rgb(255, 0, 255);
}

bool tile_animates(int t) {
	return t == T_SHALLOW || t == T_DEEP || t == T_SWAMP || t == T_SEWAGE || t == T_LAVA || t == T_GATE || t == T_FIRE
		|| t == T_FORGE || t == T_SCONCE || t == T_SKY || t == T_KELP || t == T_CRYSTAL || t == T_GOLDPILE || t == T_RAINBOW
		|| t == T_CORAL || t == T_LILY || t == T_CYPRESS || t == T_ALCHEMY || t == T_SHRINE || t == T_WELL;
}

void textures_build(int S) {
	TS = S;
	TEX = [];
	TEX_OFF = [];
	TILE_ANIM = [];
	for (int t = 0; t < TILES_DEF.size(); ++t) {
		bool anim = tile_animates(t);
		TILE_ANIM.push(anim ? 1 : 0);
		for (int vr = 0; vr < 4; ++vr) {
			int base_off = -1;
			for (int f = 0; f < 4; ++f) {
				if (!anim && f > 0) { TEX_OFF.push(base_off); continue; }
				int off = TEX.size();
				if (f == 0) { base_off = off; }
				TEX_OFF.push(off);
				for (int py = 0; py < S; ++py) {
					for (int px = 0; px < S; ++px) { TEX.push(tex_pixel(t, vr, f, px, py, S)); }
				}
			}
		}
	}
}

// ------------------------------------------------------------- sprites ---
// 8x8, facing right. Letters: . clear, o outline, b body, s shade, h highlight (all from the tint),
// w white, k black, e eye (yellow), r red, y yellow, g gold, m metal, f flame, n wood, l leaf,
// u blue, v violet, x tan, t teeth, p pink, c cyan, z bone, a armor (worn body color), q hat, d weapon
array<string> SPRITE_SRC = [
	"gator|...qqq..|..bbbkbb|..bbbbbb|...btbt.|.oaaab..|b.aaab..|bb.bb...|...s.s..",
	"gator_npc|........|..bbbkbb|..bbbbbb|...btbt.|.obbbb..|b.bhhb..|bb.bb...|...s.s..",
	"frogman|..h..h..|.bkbbkb.|.bbbbbb.|..brrb..|.bbhhbb.|b.bhhb.b|..b..b..|.ss..ss.",
	"turtle_npc|........|....hhb.|..sssbkb|.ssssbbb|.zsssz..|.bzzzb..|.b...b..|........",
	"guard|..mmmm..|.mbbkbbb|..bbbbbb|..bbtbt.|.mmmmmd.|b.mmmm.d|bb.mm...|...s.s..",
	"frog|........|........|..h.h...|.bkbkb..|.bbbbbb.|bshhhsb.|.b...b..|........",
	"frog_big|..h..h..|.bkbbkb.|bbbbbbbb|bbrrrrbb|.bbhhbb.|bshhhhsb|bb....bb|........",
	"crawdad|........|r.....r.|rr....rr|.rbbbbr.|..bbbb..|.b.bb.b.|..b..b..|........",
	"rabbit|..hh....|..bh....|..bb....|.bbkb...|bbbbbb..|.bbbbbh.|.b..b...|........",
	"heron|....bb..|....bkbr|....bb..|...bb...|.bbbb...|..bbbb..|...s....|...s.s..",
	"sheep|........|.hhhhh..|hhhhhhhk|hhhhhhhb|.hhhhhh.|..s..s..|..s..s..|........",
	"mosquito|........|.h....h.|..h..h..|...bbr..|..bbbbrr|.s.bb...|..s..s..|........",
	"rat|........|........|....bbh.|..bbbbkb|.bbbbbbp|s.s..s..|.ss.....|........",
	"raccoon|........|.b...b..|.bbbbbb.|.kkbkkb.|..bbbbp.|.bbhhbb.|bsb..sb.|s.......",
	"hog|........|........|...bbbbb|.bbbbbkh|bbbbbbbp|.bbbbbb.|.s.s.s.s|........",
	"wolf|........|.....b.b|.....bbb|....bbkh|bbbbbbbb|b.bbbbb.|..s.s.s.|..s...s.",
	"goblin|..l..l..|..llll..|..lelel.|...lll..|.nnnnl..|l.nnn.d.|..n.n.d.|..s.s...",
	"snake|........|........|.....bbk|....bbrb|.s..b...|sbsbs...|.bsb....|........",
	"bandit|...kkk..|..xxkxx.|..xxxx..|...xx...|.rrrrrd.|x.rrr..d|..n.n...|..s.s...",
	"treant|.l.ll.l.|llllllll|.lkllkl.|..nnnn..|.nnnnnn.|n.nnnn.n|..n..n..|.nn..nn.",
	"slime|........|........|...hh...|..hbbb..|.bkbbkb.|.bbbbbb.|bbbbbbbb|ssssssss",
	"slime_small|........|........|........|........|...hb...|..bkkb..|..bbbb..|.ssssss.",
	"roach|........|........|..b..b..|...bb...|.bbbbbb.|sbhhbbsb|.s.s.s..|........",
	"ratking|.g.g.g..|.ggggg..|..bbbbh.|.bbbbbkb|bbbbbbbp|b.rrrr..|s.s..s..|.ss.....",
	"turtle|........|........|..sss.hb|.shshsbk|.sssssbb|.bbbbbb.|.b....b.|........",
	"toad|........|..v..v..|.vkbbkv.|bbbbbbbb|bbvvvvbb|.bbhhbb.|bb....bb|........",
	"crawknight|..mmm...|r.mkm..r|rr.m..rr|.rbbbbr.|.rbbbbm.|.b.bb.bm|..b..b.m|........",
	"zombie|...bbb..|..bkbkb.|..bbbbb.|...bb.b.|.bbbbbb.|b.sbbs..|..b..b..|..s..s..",
	"bigmama|........|..sssshh|.shshsbk|sssssssb|ssshssbb|.bbbbbbt|.bb...bb|........",
	"skeleton|...zzz..|..zkzkz.|..zzzz..|...zz...|.z.zz.z.|z..zz..d|..z..z..|..z..z..",
	"eel|........|........|........|.....bbk|bhb.bbbb|bbbbbb..|.s......|........",
	"merrow|...bb...|..bkbb..|..bbbb.d|...bb..d|.bbbbbbd|b.bhhb..|..bbb...|.bs.sb..",
	"witch|...vv...|..vvvv..|..vxxv..|..xkxk..|.vvvvvc.|v.vvvv.c|..vvvv..|..s..s..",
	"angler|....y...|...b.y..|..bbb...|.bbbbbk.|bbbbtttt|.bbbbbb.|..s..s..|........",
	"drownking|.g.g.g..|.ggggg..|.bzzzzb.|.bzkzkb.|mbbbbbbm|.bbhhbb.|.bbbbbb.|..s..s..",
	"wisp|........|...hh...|..hwwh..|.hwbbwh.|.hwbbwh.|..hwwh..|...hh...|....h...",
	"banshee|...hh...|..hwwh..|..hkkh..|..hwwh..|.hhwwhh.|hhhhhhhh|.h.hh.h.|h..h..h.",
	"ghoulgator|........|..bbbrbb|..bbbbbb|...btbt.|.obbbb..|b.bzzb..|bb.bb...|...s.s..",
	"crocula|.kk.....|kkbbbrbb|kkbbbbbb|kk.btbt.|kkrrrrk.|kkrrrrk.|kk.bb...|...s.s..",
	"bat|........|........|h.....h.|hh.bb.hh|.hbkkbh.|..hbbh..|....h...|........",
	"yeti|..hhhh..|.hhhhhh.|.hkhhkh.|.hhhhhh.|hhhhhhhh|h.hhhh.h|..hh.hh.|..ss.ss.",
	"penguin|...kk...|..kkkk..|..kwkw..|..kkyk..|.kwwwk..|.kwwwk..|..kwk...|..y.y...",
	"troll|..bbb...|.bbbbb..|.bkbkbb.|.btttb..|bbbbbbb.|b.bbbb.b|..bb.bb.|..ss.ss.",
	"glacierqueen|.c.c.c..|.ccccc..|..wbbw..|..bkbk..|.wwwwww.|c.wwww.c|..wwww..|.wwwwww.",
	"imp|.r....r.|..bbbb..|..bebe..|..bbbb..|.bbbbbf.|b.bbb..f|..b.b...|..s.s...",
	"salamander|........|........|.....bbe|b...bbbb|bbbbbbb.|.bfbfb..|.s.s.s..|........",
	"golem|..bbbb..|.bbbbbb.|.bfbbfb.|.bbbbbb.|bbffffbb|bb.bb.bb|..bb.bb.|..ss.ss.",
	"cultist|...rr...|..rrrr..|..rkkr..|..rffr..|.rrrrrr.|r.rrrr.r|..rrrr..|.rr..rr.",
	"emberjaw|......bb|....bbbe|..bbbbbb|f.bbbbtt|ffbbbbb.|.bbbhbb.|bb.bb.bb|........",
	"harpy|..xxx...|..xkx...|h.xxx.h.|hh.bb.hh|.hbbbbh.|..bbbb..|...b.b..|...y.y..",
	"stormel|...cc...|..cwwc..|.cwbbwc.|cwbkkbwc|.cwbbwc.|..cwwc..|..c..c..|.c....c.",
	"griffon|........|.h...bb.|hhh.bbbe|.hhhbbbk|..bbbbb.|.bbbbbb.|.b.b.b.b|........",
	"pirate|..kkkk..|.kkkkkk.|..xxkx..|..xxxx..|.uuuuud.|x.uuu..d|..n.n...|..s.s...",
	"roc|........|h.....h.|hh...hh.|hhh.bhhh|.hhbbbkh|..hbbbyy|...b.b..|...y.y..",
	"familiar|..h..h..|.bkbbkb.|bbbbbbbb|bbrrrrbb|.bbhhbb.|bshhhhsb|bb....bb|........"
];

map<string, int> SPR_ID;
array<string> SPR_ROWS;

void sprites_init() {
	SPR_ID = {};
	SPR_ROWS = [];
	for (int i = 0; i < SPRITE_SRC.size(); ++i) {
		array<string> p = SPRITE_SRC[i].split("|");
		SPR_ID[p[0]] = i;
		string rows = "";
		for (int r = 1; r < p.size(); ++r) { rows += p[r]; }
		SPR_ROWS.push(rows);
	}
}

int sprite_of(string name) { return SPR_ID.get(name, SPR_ID.get("rat", 0)); }

// compiled sprites: key -> offset into SPRPIX (TS*TS palette indices, -1 clear)
map<string, int> SPR_CACHE;
array<int> SPRPIX;

int letter_color(int ch, string tint, int armor_c, int hat_c, int weapon_c) {
	if (ch == 'o') { return cscale(tint, 35); }
	if (ch == 'b') { return crgb(tint); }
	if (ch == 's') { return cscale(tint, 66); }
	if (ch == 'h') { return clight(tint, 40); }
	if (ch == 'w') { return rgb(246, 246, 246); }
	if (ch == 'k') { return rgb(20, 18, 22); }
	if (ch == 'e') { return rgb(250, 230, 60); }
	if (ch == 'r') { return rgb(220, 40, 40); }
	if (ch == 'y') { return rgb(250, 210, 50); }
	if (ch == 'g') { return rgb(250, 200, 40); }
	if (ch == 'm') { return rgb(180, 190, 200); }
	if (ch == 'f') { return rgb(255, 150, 30); }
	if (ch == 'n') { return rgb(130, 88, 46); }
	if (ch == 'l') { return rgb(70, 170, 60); }
	if (ch == 'u') { return rgb(60, 90, 200); }
	if (ch == 'v') { return rgb(150, 70, 200); }
	if (ch == 'x') { return rgb(214, 180, 130); }
	if (ch == 't') { return rgb(250, 250, 240); }
	if (ch == 'p') { return rgb(240, 150, 170); }
	if (ch == 'c') { return rgb(90, 220, 240); }
	if (ch == 'z') { return rgb(232, 222, 190); }
	if (ch == 'a') { return armor_c >= 0 ? armor_c : crgb(tint); }
	if (ch == 'q') { return hat_c; }
	if (ch == 'd') { return weapon_c; }
	return -1;
}

// the sprite at the current tile size (8 -> 6 -> 4 by sampling the 8x8 grid)
int sprite_compiled(int spr, string tint, int armor_c, int hat_c, int weapon_c) {
	string key = `${spr},${tint},${armor_c},${hat_c},${weapon_c}`;
	if (SPR_CACHE.has(key)) { return SPR_CACHE[key]; }
	int off = SPRPIX.size();
	string rows = SPR_ROWS[spr];
	for (int py = 0; py < TS; ++py) {
		for (int px = 0; px < TS; ++px) {
			int u = px * 8 / TS;
			int v = py * 8 / TS;
			int ch = rows[v * 8 + u] + 0;
			SPRPIX.push(letter_color(ch, tint, armor_c, hat_c, weapon_c));
		}
	}
	SPR_CACHE[key] = off;
	return off;
}

// ---- ground item icons: 5x5 on the design grid, colored by the item's color
array<string> ICON_NAMES = ["sword", "greatsword", "dagger", "club", "mace", "hammer", "pan", "anchor", "trident", "bow", "crossbow", "wand", "staff",
	"hat", "cap", "helm", "wizhat", "crown", "tunic", "chain", "plate", "robe", "gloves", "legs", "boots", "shield", "amulet", "ring",
	"potion", "bomb", "scroll", "rune", "book", "fish", "meat", "crawdad", "bowl", "bread", "cheese", "pie", "marsh", "shroom",
	"pearl", "herb", "bulb", "root", "silk", "ash", "pole", "pick", "axe", "mortar", "bandage", "arrow", "ore", "ingot", "log",
	"feather", "hide", "bone", "orb", "shard", "fluff", "tail", "gem", "coins", "can", "bottle", "scale", "frog", "duck", "kazoo", "banjo", "bag"];
array<string> ICON_BITS = ["00001000100010011100", "00011001100110011110", "00000000100010001100", "00000001100110011100", "01110011100010000100", "11111011100010000100", "01110011100010000100", "10101111110010001110", "10101111100010000100", "01100100101001001100", "10001111110010001000", "00001000100010010000", "00001000100010010000",
	"01110111110000000000", "01110111100000000000", "01110111110101000000", "00100011100111011111", "10101111111111100000", "11011111110111001110", "11011111111111001110", "11011111111111101110", "01110111110111011111", "11011110110000000000", "11110110101101100000", "01100111000000000000", "11111111110111000100", "01010001000111000100", "00000011100101001110",
	"00100001000111011111", "00100011100111000000", "11111100011000111111", "01110111111111101110", "11111101111011111111", "00000100111111101000", "01100111101111000000", "10001011100111000000", "11111111110111000000", "01110111111111100000", "00100011101111100000", "01110111111111100000", "01100111101111000000", "01110111110010000100",
	"00000011100111001110", "10101011100010000100", "00100011100111000000", "00100011000110000100", "10101010101010100000", "00000011101111100000", "00001000100010011000", "11110000100010000100", "01100111100010000100", "00000100011111001110", "11111000001111100000", "00001000100010010000", "00000011101111101110", "00000111111111100000", "00000111111111100000",
	"00001000110110010000", "01110111111111001110", "10001011100100001010", "00000011101111101110", "00100011100111000100", "00000011101111101110", "10000100001000011110", "00100011101111100100", "00000011101111111111", "01110101010111000000", "00100010001110011100", "00100011101111100000", "10100111110111001010", "01100111101111000000", "00000111110000000000", "00110001101110011100", "01110111111111101110"];
map<string, int> ICON_ID;

void icons_init() {
	ICON_ID = {};
	for (int i = 0; i < ICON_NAMES.size(); ++i) { ICON_ID[ICON_NAMES[i]] = i; }
}

void art_init(int tile_size) {
	pal_reset();
	colors_init();
	font_init();
	sprites_init();
	icons_init();
	COL_BLACK = rgb(0, 0, 0);
	COL_WHITE = rgb(255, 255, 255);
	textures_build(tile_size);
	SPR_CACHE = {};
	SPRPIX = [];
	// every color a sprite, icon or effect can use, so the shade tables cover them
	for (int i = 0; i < COLOR_NAMES.size(); ++i) {
		crgb(COLOR_NAMES[i]);
		cscale(COLOR_NAMES[i], 35);
		cscale(COLOR_NAMES[i], 66);
		clight(COLOR_NAMES[i], 40);
	}
	array<string> letters = ["w", "k", "e", "r", "y", "g", "m", "f", "n", "l", "u", "v", "x", "t", "p", "c", "z"];
	for (int i = 0; i < letters.size(); ++i) { letter_color(letters[i][0] + 0, "gray", -1, -1, -1); }
	NBASE = PAL_FG.size();
}
