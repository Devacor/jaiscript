// The showcase look: how GLOOM is lit and colored when it is played. Lighting,
// fog, wall textures, sprite shading and particle colors are all tables built
// at boot and on map load; the per-pixel renderer reads them exactly as it
// reads the reference tables (reference/README.md section 4.2), so both looks
// cost the same per pixel, and --smoke keeps the frames the ports reproduce.
// Colors are math::color values mixed per channel (mix_rgb): plain arithmetic,
// so the tables come out bit-identical on every platform.

bool SHOWCASE = false;   // gloom_boot: HOST_LOOK == "showcase"

function rgb(int r, int g, int b) -> math::color {
	return math::color(r / 255.0, g / 255.0, b / 255.0);
}

function color_idx(math::color c) -> int {
	return rgb_idx(math::itrunc(c.r * 255.0 + 0.5), math::itrunc(c.g * 255.0 + 0.5), math::itrunc(c.b * 255.0 + 0.5));
}

// A floor's lighting: the torchlight that tints what is near, the haze that
// distance fades into (also the horizon of floor and ceiling), the tint a
// wall turned away from the light takes, and the surfaces themselves.
class Mood {
public:
	math::color light;
	math::color fog;
	math::color turned;
	math::color ceiling;
	math::color floor;
	array<math::color> walls = [];   // wall kinds 1-4

	Mood(math::color light_, math::color fog_, math::color turned_, math::color ceiling_, math::color floor_,
			array<math::color> walls_) {
		light = light_;
		fog = fog_;
		turned = turned_;
		ceiling = ceiling_;
		floor = floor_;
		walls = walls_;
	}
}

array<Mood> MOODS = [
	// E1M1: torchlit sandstone, violet shadows
	Mood(rgb(255, 230, 196), rgb(22, 16, 26), rgb(120, 110, 170), rgb(58, 46, 54), rgb(96, 80, 60),
		[rgb(156, 136, 112), rgb(108, 128, 92), rgb(104, 110, 136), rgb(152, 100, 80)]),
	// E1M2: coolant glow, teal haze
	Mood(rgb(190, 236, 255), rgb(6, 20, 26), rgb(70, 120, 150), rgb(26, 46, 56), rgb(44, 70, 72),
		[rgb(100, 140, 152), rgb(84, 112, 164), rgb(104, 152, 116), rgb(136, 114, 152)]),
	// E1M3: furnace heat, ember haze
	Mood(rgb(255, 196, 140), rgb(34, 12, 8), rgb(150, 70, 60), rgb(70, 34, 24), rgb(86, 60, 42),
		[rgb(176, 114, 70), rgb(144, 86, 118), rgb(132, 124, 72), rgb(96, 130, 140)]),
	// E1M4: the throne's red dark
	Mood(rgb(255, 170, 150), rgb(30, 6, 8), rgb(110, 40, 70), rgb(58, 18, 20), rgb(70, 42, 36),
		[rgb(176, 72, 58), rgb(122, 54, 48), rgb(154, 130, 78), rgb(92, 66, 80)])
];

// Wall texels name one of eight tones; a tone is a color per (side, shade).
int TONE_BASE = 0;
int TONE_ACCENT = 1;
int TONE_GAP = 2;
int TONE_LIT_EDGE = 3;
int TONE_DARK_EDGE = 4;
int TONE_GLOW = 5;      // emissive: lamps and lock lights stay bright in the dark
int TONE_GRIT_DARK = 6;
int TONE_GRIT_LIGHT = 7;
int TONES = 8;

// The surfaces every floor shares: doors, the exit switch, the keyed doors.
class Fixture {
public:
	math::color base;
	math::color accent;
	math::color glow;

	Fixture(math::color base_, math::color accent_, math::color glow_) {
		base = base_;
		accent = accent_;
		glow = glow_;
	}
}

array<Fixture> FIXTURES = [
	Fixture(rgb(128, 108, 90), rgb(170, 144, 110), rgb(255, 210, 140)),   // 5 door
	Fixture(rgb(62, 66, 60), rgb(226, 190, 60), rgb(110, 255, 130)),      // 6 exit switch
	Fixture(rgb(118, 62, 56), rgb(176, 92, 72), rgb(255, 70, 50)),        // 7 red door
	Fixture(rgb(62, 76, 124), rgb(94, 114, 176), rgb(80, 156, 255))       // 8 blue door
];

// distance shade 0 (far) .. 15 (near) -> light reaching the wall
function shade_light(int shade) -> float {
	float t = shade / 15.0;
	return 0.05 + 0.8 * t * math::sqrt(t);
}

// WALLT: [kind 0..8][side][shade 0..15][tone 0..7] palette indices
function push_showcase_tones(Mood& mood, math::color base, math::color accent, math::color glow) -> void {
	math::color white = rgb(255, 255, 255);
	array<math::color> albedo = [base, accent, base * 0.36, math::clamp(base * 1.25, 0.0, 1.0), base * 0.62, glow,
		base * 0.86, math::clamp(base * 1.1, 0.0, 1.0)];
	for (int side = 0; side < 2; ++side) {
		math::color light = side == 1 ? math::mix_rgb(mood.light, mood.turned, 0.3) * 0.7 : mood.light;
		for (int shade = 0; shade < 16; ++shade) {
			float lum = shade_light(shade);
			math::color haze = mood.fog * (1.0 - lum);
			for (int tone = 0; tone < TONES; ++tone) {
				if (tone == TONE_GLOW) {
					WALLT.push(color_idx(math::mix_rgb(glow, white, 0.15) * (0.6 + 0.4 * lum) + haze * 0.3));
				} else {
					WALLT.push(color_idx(albedo[tone] * light * lum + haze));
				}
			}
		}
	}
}

function build_showcase_tables(int mi) -> void {
	auto& mood = MOODS[mi];
	WALLT = [];
	math::color unseen = rgb(10, 10, 12);
	push_showcase_tones(mood, unseen, unseen, unseen);
	for (auto& wall : mood.walls) {
		push_showcase_tones(mood, wall, math::mix_rgb(wall, mood.light, 0.18), wall);
	}
	for (auto& fixture : FIXTURES) {
		push_showcase_tones(mood, fixture.base, fixture.accent, fixture.glow);
	}

	// ceiling and floor fade into the fog at the horizon, one color per pixel row
	CEILP = [];
	FLOORP = [];
	int half = G.vh / 2;
	math::color near_floor = mood.floor * math::mix_rgb(mood.light, rgb(255, 255, 255), 0.5);
	for (int y = 0; y < G.vh; ++y) {
		if (y < half) {
			float t = 1.0 * y / imax(1, half - 1);
			float fade = t * t * (3.0 - 2.0 * t);
			CEILP.push(color_idx(math::mix_rgb(mood.ceiling, mood.fog, fade)));
			FLOORP.push(0);
		} else {
			float t = 1.0 * (y - half) / imax(1, G.vh - 1 - half);
			CEILP.push(0);
			FLOORP.push(color_idx(math::mix_rgb(mood.fog, near_floor, t * math::sqrt(t))));
		}
	}
}

// deterministic texel noise: 0..255
function texel_noise(int kind, int tx, int ty) -> int {
	return mix32(mix32(kind * 7919 + 17, tx), ty * 31 + kind) & 255;
}

function grit(int kind, int tx, int ty) -> int {
	int n = texel_noise(kind, tx, ty);
	if (n < 26) { return TONE_GRIT_DARK; }
	if (n > 236) { return TONE_GRIT_LIGHT; }
	return TONE_BASE;
}

// the showcase texture of a wall kind: tone at texel (tx, ty), both 0..31
function showcase_tone(int kind, int tx, int ty) -> int {
	if (kind == 1) { return stone_tone(tx, ty); }
	if (kind == 2) { return panel_tone(tx, ty); }
	if (kind == 3) { return conduit_tone(tx, ty); }
	if (kind == 4) { return block_tone(tx, ty); }
	if (kind == 6) { return switch_tone(tx, ty); }
	if (kind >= 5) { return door_tone(kind, tx, ty); }
	return TONE_BASE;
}

// staggered bricks, eight texels tall
function stone_tone(int tx, int ty) -> int {
	int row = ty >> 3;
	int y = ty & 7;
	int x = (tx + (row & 1) * 8) & 15;
	if (y == 7 || x == 15) { return TONE_GAP; }
	if (y == 0 || x == 0) { return TONE_LIT_EDGE; }
	if (y == 6) { return TONE_DARK_EDGE; }
	if (((row * 5 + ((tx + (row & 1) * 8) >> 4)) % 3) == 0) { return TONE_ACCENT; }
	return grit(1, tx, ty);
}

// riveted plates with a painted band
function panel_tone(int tx, int ty) -> int {
	int x = tx & 15;
	int y = ty & 15;
	if (x == 15 || y == 15) { return TONE_GAP; }
	if (x == 0 || y == 0) { return TONE_LIT_EDGE; }
	if (x == 14 || y == 14) { return TONE_DARK_EDGE; }
	if ((x == 2 || x == 12) && (y == 2 || y == 12)) { return TONE_LIT_EDGE; }
	if ((x == 2 || x == 12) && (y == 3 || y == 13)) { return TONE_DARK_EDGE; }
	if (y >= 6 && y <= 8) { return TONE_ACCENT; }
	return grit(2, tx, ty);
}

// runs of pipe held by brackets
function conduit_tone(int tx, int ty) -> int {
	int y = ty & 7;
	if ((tx & 15) < 2) {
		if (y == 0) { return TONE_LIT_EDGE; }
		return (tx & 15) == 0 ? TONE_ACCENT : TONE_DARK_EDGE;
	}
	if (y == 0) { return TONE_LIT_EDGE; }
	if (y == 5) { return TONE_DARK_EDGE; }
	if (y >= 6) { return TONE_GAP; }
	if (y == 1) { return TONE_ACCENT; }
	return grit(3, tx, ty);
}

// rough cut blocks, cracked and mossy
function block_tone(int tx, int ty) -> int {
	int row = ty >> 4;
	int y = ty & 15;
	int x = (tx + row * 11) & 31;
	if (y == 15 || x == 31) { return TONE_GAP; }
	if (y == 0 || x == 0) { return TONE_LIT_EDGE; }
	if (y == 14 || x == 30) { return TONE_DARK_EDGE; }
	int n = texel_noise(4, tx, ty);
	if ((tx + ty * 2 + n % 3) % 23 == 0) { return TONE_GAP; }
	if (y > 9 && n % 5 == 0) { return TONE_ACCENT; }
	return grit(4, tx, ty);
}

// a framed door of planks, banded, with a handle; keyed doors carry lock lights
function door_tone(int kind, int tx, int ty) -> int {
	if (tx < 2 || tx > 29 || ty < 1) { return TONE_DARK_EDGE; }
	if (ty > 29) { return TONE_GAP; }
	if (tx == 2 || tx == 29 || ty == 1) { return TONE_LIT_EDGE; }
	if (tx == 15 || tx == 16) { return TONE_GAP; }
	if (kind != 5 && (tx == 6 || tx == 7 || tx == 24 || tx == 25) && ty > 4 && ty < 27) { return TONE_GLOW; }
	if (ty == 7 || ty == 22) { return TONE_LIT_EDGE; }
	if (ty == 8 || ty == 9 || ty == 23 || ty == 24) { return TONE_ACCENT; }
	if (ty >= 14 && ty <= 17 && (tx == 13 || tx == 18)) { return TONE_LIT_EDGE; }
	if (tx % 5 == 0) { return TONE_DARK_EDGE; }
	return grit(kind, tx, ty);
}

// the exit: a switch box over hazard stripes, two green lamps
function switch_tone(int tx, int ty) -> int {
	if (tx < 2 || tx > 29 || ty < 2) { return TONE_DARK_EDGE; }
	if (ty >= 24) { return ((tx + ty) >> 2 & 1) == 0 ? TONE_ACCENT : TONE_GAP; }
	if (tx >= 9 && tx <= 22 && ty >= 5 && ty <= 20) {
		if (tx == 9 || ty == 5) { return TONE_LIT_EDGE; }
		if (tx == 22 || ty == 20) { return TONE_GAP; }
		if (tx >= 15 && tx <= 16 && ty >= 7 && ty <= 14) { return TONE_LIT_EDGE; }
		if ((tx == 12 || tx == 19) && ty >= 16 && ty <= 18) { return TONE_GLOW; }
		return TONE_GRIT_DARK;
	}
	return grit(6, tx, ty);
}

// --------------------------------------------------------------- sprites ----
// Sprites keep more light than walls at the same distance so a threat stays
// readable down a dark corridor; uppercase palette keys (eyes, gobs, flame)
// and every projectile glow at full brightness.
function showcase_sprite_lut(string name, int band, int flash) -> array<int> {
	auto& mood = MOODS[G.map_i];
	var& art = ARTS[name];
	var& rgbs = art["rgb"];
	array<string> keys = art["keys"];
	bool glowing = name.substr(0, 3) == "pr_";
	float lum = 1.0 - band * 0.085;
	math::color light = math::mix_rgb(mood.light, rgb(255, 255, 255), 0.55);
	math::color haze = mood.fog * ((1.0 - lum) * 0.8);
	array<int> lut = [0];
	for (int c = 1; c < rgbs.size(); ++c) {
		int r = rgbs[c][0];
		int g = rgbs[c][1];
		int b = rgbs[c][2];
		if (flash == 1) {
			r = imin(255, r + 170);
			g = imin(255, g + 150);
			b = imin(255, b + 140);
		}
		math::color color = rgb(r, g, b);
		string key = keys[c];
		if (glowing || (key >= "A" && key <= "Z")) {
			lut.push(color_idx(color));
		} else {
			lut.push(color_idx(color * light * lum + haze));
		}
	}
	return lut;
}

// per-kind particle ramps, phase 0 = fresh
array<array<math::color>> SHOWCASE_PARTICLE_RAMPS = [
	[rgb(0, 0, 0), rgb(0, 0, 0), rgb(0, 0, 0), rgb(0, 0, 0)],
	[rgb(255, 250, 214), rgb(255, 212, 110), rgb(240, 140, 50), rgb(150, 70, 30)],      // spark
	[rgb(232, 40, 30), rgb(190, 30, 24), rgb(140, 22, 18), rgb(90, 14, 12)],            // blood
	[rgb(214, 72, 54), rgb(172, 50, 38), rgb(128, 36, 28), rgb(86, 24, 20)],            // gib
	[rgb(104, 98, 94), rgb(84, 80, 78), rgb(64, 62, 62), rgb(46, 44, 46)],             // smoke
	[rgb(255, 255, 236), rgb(255, 240, 170), rgb(255, 200, 100), rgb(220, 130, 50)],    // flash
	[rgb(190, 255, 200), rgb(130, 245, 220), rgb(100, 200, 255), rgb(110, 120, 240)]    // hexmote
];
