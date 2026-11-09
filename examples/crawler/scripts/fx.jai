// What a turn leaves in the view: damage numbers, slashes, a slain monster melting into sparks,
// spells in flight with their own light, the alarm over a monster that just saw you, and the
// banners for a new floor, a level, a find or a reload. Effects read the game and never its Rng,
// so the smoke hash never sees them. Each lives a turn or two; a dump draws it at rest (FX_REST),
// and an interactive turn plays it through four in-between frames first.

int FX_NUMBER = 0;          // a number or word over a tile
int FX_SCREEN = 1;          // a number or word low in the forward view: what happened to you
int FX_SLASH = 2;
int FX_DEATH = 3;
int FX_BOLT = 4;            // a skill in flight: value 0 ember, 1 thorn, 2 hex, 3 slam, 4 hollow fire
int FX_ALERT = 5;
int FX_BANNER = 6;

class Fx {
	int kind;
	int turn;
	float x;
	float y;
	float tx;
	float ty;
	int rgb;
	int value;
	float size;
	float lift;
	string text;
	string sub;
}

array<Fx> FX = [];
float FX_REST = 0.6;        // where a frame nobody animates catches an effect
float FX_PHASE = 0.6;
int FX_SHAKE = 0;           // pixels the forward view jolts sideways this frame
bool FX_NEXT_CRIT = false;  // attack() marks the hit it is about to deal

function fx_new(int kind, float x, float y, int rgb) -> Fx {
	Fx f;
	f.kind = kind;
	f.turn = G.turn;
	f.x = x;
	f.y = y;
	f.rgb = rgb;
	return f;
}

function fx_age(Fx f) -> int { return G.turn - f.turn; }

function fx_life(int kind) -> int { return kind == FX_BANNER ? 3 : 1; }

// a new turn's key: what the last turn left fades out
function fx_prune() -> void {
	array<Fx> kept;
	for (auto f : FX) {
		if (fx_age(f) <= fx_life(f.kind)) { kept.push(f); }
	}
	FX = kept;
	FX_PHASE = FX_REST;
}

function fx_number(var& m, int amount) -> void {
	Fx f = fx_new(FX_NUMBER, m.x + 0.5, m.y + 0.5, FX_NEXT_CRIT ? INK_TITLE : 0xFFFFFF);
	f.text = FX_NEXT_CRIT ? `${amount}!` : `${amount}`;
	f.value = FX_NEXT_CRIT ? 2 : 1;
	f.size = monster_sizef(m);
	FX_NEXT_CRIT = false;
	FX.push(f);
}

function fx_word(int x, int y, string word, int rgb) -> void {
	Fx f = fx_new(FX_NUMBER, x + 0.5, y + 0.5, rgb);
	f.text = word;
	f.value = 1;
	f.size = 0.6;
	FX.push(f);
}

// what happened to you, low in the view; stacked so a turn's hits don't overprint
function fx_screen(string text, int rgb) -> void {
	int slot = 0;
	for (auto f : FX) {
		if (f.kind == FX_SCREEN && f.turn == G.turn) { slot += 1; }
	}
	Fx f = fx_new(FX_SCREEN, 0.0, 0.0, rgb);
	f.text = text;
	f.value = slot;
	FX.push(f);
}

function fx_slash(var& m) -> void {
	Fx f = fx_new(FX_SLASH, m.x + 0.5, m.y + 0.5, mix_rgb(theme_of(G.floor).glow, 0xFFFFFF, 0.5));
	f.size = monster_sizef(m);
	f.value = lat(G.turn, m.id, 3) % 2;
	FX.push(f);
}

function fx_death(var& m) -> void {
	Fx f = fx_new(FX_DEATH, m.x + 0.5, m.y + 0.5, m.ink);
	f.text = m.glyph;
	f.size = monster_sizef(m);
	f.lift = sprite_lift(m);
	f.value = m.id;
	FX.push(f);
}

function fx_bolt(int style, float x0, float y0, int tx, int ty, int rgb) -> void {
	Fx f = fx_new(FX_BOLT, x0, y0, rgb);
	f.tx = tx + 0.5;
	f.ty = ty + 0.5;
	f.value = style;
	FX.push(f);
}

// a class skill leaves your hands a little ahead of you and flies down the facing
function fx_skill(string skill, int tx, int ty) -> void {
	int style = skill == "emberbolt" ? 0 : (skill == "thornvolley" ? 1 : (skill == "wither" ? 2 : 3));
	array<int> inks = [0xFF8C28, 0x9CD25A, 0xC060FF, 0xF0E6C8];
	float x0 = G.player.x + 0.5 + FACE_DX[G.facing] * 0.35;
	float y0 = G.player.y + 0.5 + FACE_DY[G.facing] * 0.35;
	fx_bolt(style, style == 3 ? tx + 0.5 : x0, style == 3 ? ty + 0.5 : y0, tx, ty, inks[style]);
}

function fx_alert(var& m) -> void {
	Fx f = fx_new(FX_ALERT, m.x + 0.5, m.y + 0.5, INK_TITLE);
	f.text = "!";
	f.size = monster_sizef(m);
	f.lift = sprite_lift(m);
	FX.push(f);
}

// a throw on its way: "!!" in red over the thrower, the turn before it lands
function fx_windup(var& m) -> void {
	Fx f = fx_new(FX_ALERT, m.x + 0.5, m.y + 0.5, INK_BAD);
	f.text = "!!";
	f.size = monster_sizef(m);
	f.lift = sprite_lift(m);
	FX.push(f);
}

// one banner at a time: the newest replaces the last
function fx_banner(string text, string sub, int rgb) -> void {
	array<Fx> kept;
	for (auto f : FX) {
		if (f.kind != FX_BANNER) { kept.push(f); }
	}
	FX = kept;
	Fx f = fx_new(FX_BANNER, 0.0, 0.0, rgb);
	f.text = text;
	f.sub = sub;
	FX.push(f);
}

function fx_fresh() -> bool {
	for (auto f : FX) {
		if (f.turn >= G.turn - 1 && f.kind != FX_BANNER) { return true; }
	}
	return false;
}

// ---------------------------------------------------------------- playing ---
// The turn just resolved: four in-between frames carry its effects from first contact to rest,
// the view jolting under a heavy blow.
function fx_animate() -> void {
	if (!animating() || !fx_fresh()) { return; }
	bool heavy = G.last_dmg * 4 >= G.player.total_max_hp() && G.last_dmg > 0;
	for (auto f : FX) {
		if (f.kind == FX_BOLT && f.value == 3 && fx_age(f) <= 1) { heavy = true; }
	}
	array<int> jolt = [2, -2, 1, 0];
	array<float> phases = [0.2, 0.42, 0.66, 0.86];
	for (int k = 0; k < phases.size(); ++k) {
		FX_PHASE = phases[k];
		FX_SHAKE = heavy ? jolt[k] : 0;
		fx_present();
		anim_pause(0.016);
	}
	FX_PHASE = 1.0;
	FX_SHAKE = 0;
}

function fx_present() -> void {
	fill_boxes();
	present(frame_lines(render_view(camera_x(), camera_y(), FACE_DX[G.facing], FACE_DY[G.facing])));
}

// ----------------------------------------------------------------- lights ---
// Spells in flight, cinderwisps and the better loot carry their own light, added to the torch
// and the sconces wherever it falls.
array<float> DYN_X = [];
array<float> DYN_Y = [];
array<float> DYN_S = [];
int DYN_N = 0;

function dyn_light(float x, float y, float strength) -> void {
	if (DYN_N >= 6) { return; }
	DYN_X.push(x);
	DYN_Y.push(y);
	DYN_S.push(strength);
	DYN_N += 1;
}

function gather_lights() -> void {
	DYN_X = [];
	DYN_Y = [];
	DYN_S = [];
	DYN_N = 0;
	for (auto f : FX) {
		if (f.kind != FX_BOLT || fx_age(f) > 1 || f.value == 1) { continue; }
		FSpot at = bolt_at(f);
		dyn_light(at.fx, at.fy, f.value == 3 ? 6.0 : 11.0);
	}
	for (auto& m : G.monsters) {
		if (m.alive && m.glyph == "w" && cheb(m.x, m.y, G.player.x, G.player.y) <= 9) { dyn_light(m.x + 0.5, m.y + 0.5, 7.0); }
	}
	for (auto& it : G.ground) {
		if (loot_tier(it) >= 2 && cheb(it.x, it.y, G.player.x, G.player.y) <= 9) { dyn_light(it.x + 0.5, it.y + 0.4, 2.0 + loot_tier(it)); }
	}
	for (auto& m : G.monsters) {
		if (m.alive && m.disguised && cheb(m.x, m.y, G.player.x, G.player.y) <= 9) { dyn_light(m.x + 0.5, m.y + 0.4, 6.0); }
	}
}

// 0 plain, 1 uncommon, 2 rare, 3 epic, 4 legendary: how loudly a thing on the floor glows
function loot_tier(var& it) -> int {
	if (!it.is_gear()) { return 0; }
	if (it.rarity == "legendary") { return 4; }
	if (it.rarity == "epic") { return 3; }
	if (it.rarity == "rare") { return 2; }
	return it.rarity == "uncommon" ? 1 : 0;
}

class FSpot { float fx; float fy; }

// a bolt flies through the first three quarters of its animation and bursts in the last
function bolt_progress(Fx f) -> float {
	float p = fx_age(f) >= 1 && FX_PHASE >= 1.0 ? 1.0 : FX_PHASE / 0.75;
	return p > 1.0 ? 1.0 : p;
}

// a bolt still flying at this tile this turn: what it does there waits for it to land
function bolt_pending(Fx f) -> bool {
	for (auto b : FX) {
		if (b.kind == FX_BOLT && b.turn == f.turn && b.tx == f.x && b.ty == f.y && bolt_progress(b) < 1.0) { return true; }
	}
	return false;
}

function bolt_at(Fx f) -> FSpot {
	float p = bolt_progress(f);
	return FSpot{f.x + (f.tx - f.x) * p, f.y + (f.ty - f.y) * p};
}

// ----------------------------------------------------------- monsters notice ---
// The first time a monster has eyes on you it says so: a line in its colour and a "!" over its
// head. Only the view keeps track (NOTICED, by monster id), so the rules never see it; declared
// without an initializer, a reload keeps it.
map<int, bool> NOTICED;

function notice_player(var& m) -> void {
	if (!m.alive || m.disguised || NOTICED.has(m.id)) { return; }
	if (!is_visible(m.x, m.y) || !(m.aggro || sees_player(m))) { return; }
	NOTICED[m.id] = true;
	fx_alert(m);
	if (!BARKS.has(m.kind)) { return; }
	array<string> lines = BARKS[m.kind];
	string line = lines[lat(m.id, G.floor, 17) % lines.size()];
	if (line.substr(0, 1) == "*") { log_msg(paint(m.ink, `${mon_subject(m)} `) + paint(INK_DIM, line)); }
	else { log_msg(paint(m.ink, `${m.name}: `) + paint(INK_TEXT, `"${line}"`)); }
}

// ---------------------------------------------------------------- loot glow ---
// Finer gear on the floor sends up a column of its rarity's light, a spark riding it. A
// Grinnest still pretending to be a chest sends up the brightest beam on the floor: gold.
function draw_loot_beams() -> void {
	for (auto& it : G.ground) {
		int tier = loot_tier(it);
		if (tier > 0) { draw_beam(it.x, it.y, tier, rarity_ink(it.rarity), 0.3); }
	}
	for (auto& m : G.monsters) {
		if (m.alive && m.disguised) { draw_beam(m.x, m.y, 4, INK_COIN, 0.36); }
	}
}

function draw_beam(int tx, int ty, int tier, int rgb, float height) -> void {
	Projected at = project(tx + 0.5, ty + 0.5, height, 0.0);
	if (at.depth <= 0.0) { return; }
	int top = at.bot - at.hgt - at.hgt * (1 + tier);
	int c = pal(rgb);
	int dim = pal(scale_rgb(rgb, 0.55));
	int x = at.sx;
	int spark = top + ((G.turn * 3 + tx) % imax(1, at.bot - at.hgt - top));
	for (int y = imax(0, top); y < at.bot - at.hgt; ++y) {
		if ((y + G.turn) % 2 == 0) { fx_plot(x, y, y == spark ? pal(0xFFFFFF) : dim, at.depth); }
		if (tier >= 3 && y % 3 == 0) { fx_plot(x - 1, y, dim, at.depth); }
	}
	fx_plot(x, spark, pal(0xFFFFFF), at.depth);
	fx_plot(x, at.bot - at.hgt - 1, c, at.depth);
}

// ------------------------------------------------------------ the pixel fonts ---
// 3x5 for numbers and short words; the 5x7 title font (screens.jai) for banners.
map<string, string> TINY = {
	"0": "####.##.##.####", "1": ".#.##..#..#.###", "2": "###..#####..###", "3": "###..#.##..####",
	"4": "#.##.####..#..#", "5": "####..###..####", "6": "####..####.####", "7": "###..#.#..#..#.",
	"8": "####.#####.####", "9": "####.####..####", "A": ".#.#.#####.##.#", "B": "##.#.###.#.###.",
	"C": ".###..#..#...##", "D": "##.#.##.##.###.", "E": "####..##.#..###", "F": "####..##.#..#..",
	"G": ".###..#.##.#.##", "H": "#.##.#####.##.#", "I": "###.#..#..#.###", "J": "..#..#..##.#.#.",
	"K": "#.##.###.#.##.#", "L": "#..#..#..#..###", "M": "#.########.##.#", "N": "##.#.##.##.##.#",
	"O": ".#.#.##.##.#.#.", "P": "##.#.###.#..#..", "Q": ".#.#.##.###..##", "R": "##.#.###.#.##.#",
	"S": ".###...#...###.", "T": "###.#..#..#..#.", "U": "#.##.##.##.####", "V": "#.##.##.##.#.#.",
	"W": "#.##.########.#", "X": "#.##.#.#.#.##.#", "Y": "#.##.#.#..#..#.", "Z": "###..#.#.#..###",
	"+": "....#.###.#....", "-": "......###......", "!": ".#..#..#.....#.", "?": "##...#.#.....#.",
	".": ".............#.", ":": "....#.....#....", "/": "..#..#.#.#..#..", "'": ".#..#..........",
	",": ".........#.#...", " ": "..............."
};

function tiny_width(string text, int scale) -> int { return imax(0, text.size() * 4 * scale - scale); }

function big_width(string text, int scale) -> int { return imax(0, text.size() * 6 * scale - scale); }

// Text into the pane being drawn, clipped to it, with a one-pixel shadow; depth > 0 hides the
// columns a nearer wall covers.
function pane_text(string text, int x, int y, int scale, int c, int shadow, bool tiny, float depth) -> void {
	int gw = tiny ? 3 : 5;
	int gh = tiny ? 5 : 7;
	int advance = (gw + 1) * scale;
	for (int pass = 0; pass < 2; ++pass) {
		int off = pass == 0 ? imax(1, scale / 2) : 0;
		int ink = pass == 0 ? shadow : c;
		if (ink < 0) { continue; }
		for (int ci = 0; ci < text.size(); ++ci) {
			string ch = text.substr(ci, 1);
			string glyph = tiny ? (TINY.has(ch) ? TINY[ch] : "") : (FONT.has(ch) ? FONT[ch] : "");
			if (glyph == "") { continue; }
			int gx0 = x + ci * advance + off;
			for (int gy = 0; gy < gh; ++gy) {
				for (int gx = 0; gx < gw; ++gx) {
					if (glyph[gy * gw + gx] != "#") { continue; }
					for (int py = 0; py < scale; ++py) {
						int Y = y + gy * scale + py + off;
						if (Y < 0 || Y >= PH) { continue; }
						for (int px = 0; px < scale; ++px) {
							int X = gx0 + gx * scale + px;
							if (X < 0 || X >= PW) { continue; }
							if (depth > 0.0 && depth >= ZBUF[X]) { continue; }
							VPIX[PBASE + Y * STRIDE + X] = ink;
						}
					}
				}
			}
		}
	}
}

// darkens or tints a rectangle of the pane; the palette lookups are cached per call
function pane_tint(int x0, int y0, int x1, int y1, int rgb, float amount, float bright) -> void {
	map<int, int> done = {};
	for (int y = imax(0, y0); y < imin(PH, y1); ++y) {
		int row = PBASE + y * STRIDE;
		for (int x = imax(0, x0); x < imin(PW, x1); ++x) {
			int c = VPIX[row + x];
			if (!done.has(c)) { done[c] = pal(scale_rgb(mix_rgb(VPAL_RGB[c], rgb, amount), bright)); }
			VPIX[row + x] = done[c];
		}
	}
}

// ------------------------------------------------------------- in the world ---
// Called from draw_sprites with the pane's camera: melting monsters and bolts join the sprites,
// numbers, slashes and alarms go over them.
function fx_sprites() -> void {
	for (auto f : FX) {
		if (fx_age(f) > 1) { continue; }
		if (f.kind == FX_DEATH) {
			Sprite sp = sprite_at(f.x, f.y, glyph_art(f.text), f.rgb, 1.15 * f.size, f.lift, PH + 12, -1, false);
			if (sp.depth > 0.0) {
				sp.melt = bolt_pending(f) ? 0.0 : FX_PHASE;
				sp.fx = f.value;
				SPRITES.push(sp);
			}
		} else if (f.kind == FX_BOLT) {
			if (bolt_progress(f) >= 1.0 && FX_PHASE >= 1.0) { continue; }
			FSpot at = bolt_at(f);
			string art = f.value == 1 ? "thorn" : (f.value == 3 ? "" : "orb");
			if (art == "") { continue; }
			Sprite sp = sprite_at(at.fx, at.fy, ART_ID[art], f.rgb, f.value == 1 ? 0.22 : 0.3, 0.32, PH, -1, false);
			if (sp.depth > 0.0) {
				sp.lit = 15.0;
				SPRITES.push(sp);
			}
		}
	}
}

function fx_overlays() -> void {
	for (auto f : FX) {
		if (fx_age(f) > 1) { continue; }
		if ((f.kind == FX_NUMBER || f.kind == FX_DEATH) && bolt_pending(f)) { continue; }
		if (f.kind == FX_NUMBER) { fx_draw_number(f); }
		else if (f.kind == FX_SLASH) { fx_draw_slash(f); }
		else if (f.kind == FX_ALERT) { fx_draw_alert(f); }
		else if (f.kind == FX_BOLT) { fx_draw_burst(f); }
		else if (f.kind == FX_DEATH) { fx_draw_sparks(f); }
	}
}

// a tile's place on screen: its centre column, depth, the floor line and the height of a
// sprite of the given size there; depth 0 when it is behind the camera or too far
class Projected { float depth; float sx; int bot; int hgt; }

function project(float x, float y, float height, float lift) -> Projected {
	float rx = x - CAM_POSX;
	float ry = y - CAM_POSY;
	float depth = CAM_INVDET * (0.0 - CAM_PY * rx + CAM_PX * ry);
	if (depth < 0.15 || depth > SPRITE_RANGE) { return Projected{0.0, 0.0, 0, 0}; }
	float ttx = CAM_INVDET * (CAM_DY * rx - CAM_DX * ry);
	int bot = PH * 0.5 + WALL_KPX * (0.5 - lift) / depth;
	return Projected{depth, (PW / 2.0) * (1.0 + ttx / depth), bot, iclamp(WALL_KPX * height / depth, 1, PH + 12)};
}

function fx_draw_number(Fx f) -> void {
	Projected at = project(f.x, f.y, 1.15 * f.size, 0.0);
	if (at.depth <= 0.0) { return; }
	int scale = at.hgt >= 34 || f.value == 2 ? 2 : 1;
	int w = tiny_width(f.text, scale);
	int rise = 2 + FX_PHASE * 7 * scale;
	int y = at.bot - at.hgt - 5 * scale - rise;
	if (y < 1) { y = 1; }
	pane_text(f.text, math::floor(at.sx - w * 0.5), y, scale, pal(f.rgb), pal(0x140A0A), true, at.depth);
}

function fx_draw_slash(Fx f) -> void {
	Projected at = project(f.x, f.y, 1.15 * f.size, 0.0);
	if (at.depth <= 0.0) { return; }
	float reach = FX_PHASE / 0.5;
	if (reach > 1.0) { reach = 1.0; }
	int core = pal(FX_PHASE >= 1.0 ? mix_rgb(f.rgb, 0xC80A14, 0.6) : 0xFFFFFF);
	int edge = pal(f.rgb);
	int half = at.hgt / 2;
	int cx = at.sx;
	int cy = at.bot - half;
	int dir = f.value == 0 ? 1 : -1;
	for (int stroke = -1; stroke <= 1; ++stroke) {
		int len = half * (stroke == 0 ? 1.0 : 0.7) * reach;
		for (int t = -half; t < -half + 2 * len; ++t) {
			int x = cx + dir * t + stroke * 3;
			int y = cy + t;
			fx_plot(x, y, stroke == 0 ? core : edge, at.depth);
			fx_plot(x + 1, y, edge, at.depth);
		}
	}
}

function fx_plot(int x, int y, int c, float depth) -> void {
	if (x < 0 || y < 0 || x >= PW || y >= PH) { return; }
	if (depth > 0.0 && depth >= ZBUF[x]) { return; }
	VPIX[PBASE + y * STRIDE + x] = c;
}

function fx_draw_alert(Fx f) -> void {
	Projected at = project(f.x, f.y, 1.15 * f.size, f.lift);
	if (at.depth <= 0.0) { return; }
	int scale = at.hgt >= 30 ? 2 : 1;
	int bob = (1.0 - FX_PHASE) * 4;
	int w = tiny_width(f.text, scale);
	pane_text(f.text, math::floor(at.sx - w * 0.5), at.bot - at.hgt - 7 * scale - bob, scale, pal(f.rgb), pal(0x3A1000), true, at.depth);
}

// the blast where a bolt lands: a ring of its colour opening out, white at the heart
function fx_draw_burst(Fx f) -> void {
	float p = bolt_progress(f);
	if (p < 1.0 && FX_PHASE < 1.0) { return; }
	float open = FX_PHASE >= 1.0 ? 1.0 : (FX_PHASE - 0.75) / 0.25;
	if (open < 0.0) { open = 0.0; }
	Projected at = project(f.tx, f.ty, 0.9, 0.0);
	if (at.depth <= 0.0) { return; }
	int cy = at.bot - at.hgt / 2;
	int radius = 2 + at.hgt * (0.15 + 0.35 * open);
	int ring = pal(f.rgb);
	int heart = pal(mix_rgb(f.rgb, 0xFFFFFF, 0.7));
	int spokes = 14;
	for (int k = 0; k < spokes; ++k) {
		float a = k * 6.2832 / spokes + f.turn;
		float ca = math::cos(a);
		float sa = math::sin(a);
		for (int r = radius / 2; r <= radius; ++r) {
			fx_plot(at.sx + ca * r, cy + sa * r * 0.8, r > radius * 3 / 4 ? ring : heart, at.depth);
		}
	}
	if (FX_PHASE >= 1.0) { return; }
	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) { fx_plot(at.sx + x, cy + y, pal(0xFFFFFF), at.depth); }
	}
}

// a slain monster's sparks: they burst from its middle and fall, fading through its colour
function fx_draw_sparks(Fx f) -> void {
	Projected at = project(f.x, f.y, 1.15 * f.size, f.lift);
	if (at.depth <= 0.0) { return; }
	int hot = pal(mix_rgb(f.rgb, 0xFFFFFF, 0.6));
	int warm = pal(f.rgb);
	int cool = pal(scale_rgb(f.rgb, 0.45));
	float cy = at.bot - at.hgt * 0.55;
	float t = FX_PHASE;
	for (int k = 0; k < 18; ++k) {
		int h = lat(f.value, k, 41);
		float a = (h % 628) / 100.0;
		float speed = (0.35 + (h / 628 % 50) / 80.0) * at.hgt;
		float x = at.sx + math::cos(a) * speed * t;
		float y = cy - math::sin(a) * speed * t * 0.7 + at.hgt * 0.9 * t * t;
		int c = t < 0.35 ? hot : (t < 0.75 ? warm : cool);
		fx_plot(x, y, c, at.depth);
		if (at.hgt >= 24) { fx_plot(x + 1, y, c, at.depth); }
	}
}

// --------------------------------------------------------- over the forward view ---
// The boss's breath washing the view, the heartbeat rim at low health, the banner, and your own
// numbers low in the view, on top of everything.
function fx_screen_overlays() -> void {
	for (auto f : FX) {
		if (f.kind == FX_BOLT && f.value == 4 && fx_age(f) <= 1) {
			float wash = FX_PHASE >= 1.0 ? 0.18 : 0.65 * (1.0 - FX_PHASE * 0.6);
			pane_tint(0, 0, PW, PH, 0xE6E6F0, wash, 1.0);
		}
	}
	if (G.player.alive && G.player.hp * 10 < G.player.total_max_hp() * 3 && G.last_dmg == 0) { draw_heartbeat(); }
	for (auto f : FX) {
		if (f.kind == FX_BANNER && fx_age(f) <= fx_life(FX_BANNER)) { fx_draw_banner(f); }
	}
	for (auto f : FX) {
		if (f.kind == FX_SCREEN && fx_age(f) <= 1) { fx_draw_screen(f); }
	}
}

function fx_draw_screen(Fx f) -> void {
	int scale = PH >= 56 ? 3 : 2;
	int w = tiny_width(f.text, scale);
	int rise = FX_PHASE * 6;
	int y = PH * 7 / 10 - rise - f.value * 6 * scale;
	pane_text(f.text, (PW - w) / 2, y, scale, pal(f.rgb), pal(0x0A0408), true, 0.0);
}

// below a third of your health the rim of the view beats, wider on every other turn
function draw_heartbeat() -> void {
	int beat = (G.turn % 2 == 0) ? 3 : 2;
	int c = pal(0x5A0810);
	int c2 = pal(0x2E0408);
	for (int y = 0; y < PH; ++y) {
		for (int x = 0; x < PW; ++x) {
			int e = imin(imin(x, PW - 1 - x), imin(y, PH - 1 - y));
			if (e >= beat) { continue; }
			if (e == 0 || (e < beat - 1 && (x + y) % 2 == 0)) { VPIX[PBASE + y * STRIDE + x] = c; }
			else if ((x * 3 + y) % 4 == 0) { VPIX[PBASE + y * STRIDE + x] = c2; }
		}
	}
}

// The banner: big letters on a dark band in the view's upper part, a line or two of small ones
// under them when the view has room; it fades over its last turns.
function fx_draw_banner(Fx f) -> void {
	int age = fx_age(f);
	float fade = age <= 1 ? 1.0 : (age == 2 ? 0.7 : 0.45);
	int big = imax(1, imin((PW - 4) / imax(1, f.text.size() * 6), PH / 22));
	bool tiny_title = big_width(f.text, 1) > PW - 2;   // too wide for the big letters: the small ones, doubled if they fit
	if (tiny_title) { big = tiny_width(f.text, 2) <= PW - 2 ? 2 : 1; }
	int title_h = tiny_title ? 5 * big : 7 * big;
	int tiny_scale = PW >= 150 ? 2 : 1;
	array<string> lines = wrap_words(f.sub, (PW - 2 + tiny_scale) / (4 * tiny_scale));
	if (lines.size() > (PH >= 60 ? 3 : 2) || ASCII_ONLY) { lines = []; }   // ASCII cells blur small letters
	int h = title_h + (lines.size() > 0 ? 2 + lines.size() * 6 * tiny_scale : 0);
	int y0 = imax(2, PH / 5 - h / 3);
	pane_tint(0, y0 - 2, PW, y0 + h + 2, 0x000000, 0.0, 0.38);
	int glow = scale_rgb(f.rgb, fade);
	int tw = tiny_title ? tiny_width(f.text, big) : big_width(f.text, big);
	int shadow = big >= 2 ? pal(0x050306) : -1;
	pane_text(f.text, (PW - tw) / 2, y0, big, pal(glow), shadow, tiny_title, 0.0);
	// the letters' lower rows deepen a little, like the title's gradient
	int lower = pal(mix_rgb(glow, 0x000000, 0.3));
	for (int y = y0 + title_h * 5 / 7; y < y0 + title_h; ++y) {
		for (int x = 0; x < PW; ++x) {
			int i = PBASE + y * STRIDE + x;
			if (VPIX[i] == pal(glow)) { VPIX[i] = lower; }
		}
	}
	int ly = y0 + title_h + 2;
	for (auto line : lines) {
		int lw = tiny_width(line, tiny_scale);
		pane_text(line, (PW - lw) / 2, ly, tiny_scale, pal(scale_rgb(INK_TEXT, fade)), tiny_scale >= 2 ? pal(0x050306) : -1, true, 0.0);
		ly += 6 * tiny_scale;
	}
}

function wrap_words(string text, int width) -> array<string> {
	array<string> lines;
	if (text == "") { return lines; }
	string line = "";
	for (auto word : text.split(" ")) {
		if (line != "" && line.size() + 1 + word.size() > width) {
			lines.push(line);
			line = "";
		}
		line = line == "" ? word : line + " " + word;
	}
	if (line != "") { lines.push(line); }
	return lines;
}

// the forward pane jolts: each row slides sideways, the edge it leaves repeats its last pixel
function fx_apply_shake() -> void {
	if (FX_SHAKE == 0) { return; }
	int s = FX_SHAKE;
	for (int y = 0; y < PH; ++y) {
		int row = PBASE + y * STRIDE;
		if (s > 0) {
			for (int x = PW - 1; x >= s; --x) { VPIX[row + x] = VPIX[row + x - s]; }
		} else {
			for (int x = 0; x < PW + s; ++x) { VPIX[row + x] = VPIX[row + x - s]; }
		}
	}
}

// ------------------------------------------------------------- hot reload ---
// A reload names what it kept, banners itself, and (when animating) sweeps a bright scanline
// down every pane, the old picture rewritten row by row. reload_done runs from the files the
// reload just brought in, so an edit to it shows on the very reload that loads it.
float RELOAD_SWEEP = -1.0;

function reload_done(string why, int files, float ms) -> void {
	string timing = HOST_INTERACTIVE ? ` in ${ms:.1f} ms` : "";
	log_ink(INK_MAGIC, `Reloaded ${files} scripts${timing} (${why}).`);
	log_ink(INK_DIM, `  Kept: floor ${G.floor}, turn ${G.turn}, ${G.monsters.size()} monsters, ${G.player.inventory.size()} items, HP ${G.player.hp}/${G.player.total_max_hp()}.`);
	fx_banner("RELOADED", "YOUR RUN KEPT", INK_MAGIC);
	if (!animating()) { return; }
	for (int k = 0; k <= 6; ++k) {
		RELOAD_SWEEP = k / 6.0;
		fx_present();
		anim_pause(0.02);
	}
	RELOAD_SWEEP = -1.0;
}

// the error's first "file:line:col: error: ..." line with its folders cut, else its first line
function reload_failed(string error) -> void {
	array<string> lines = error.split("\n");
	string first = lines[0];
	for (auto line : lines) {
		if (line.find(": error") >= 0) {
			first = line;
			break;
		}
	}
	int slash = imax(first.rfind("/"), first.rfind("\\"));
	int colon = first.find(": error");
	if (slash >= 0 && (colon < 0 || slash < colon)) { first = first.substr(slash + 1, first.size() - slash - 1); }
	if (first.size() > 160) { first = first.substr(0, 157) + "..."; }
	log_ink(INK_BAD, "Reload refused: the old rules still run.");
	log_ink(INK_DIM, `  ${first}`);
	fx_banner("REFUSED", "FIX IT AND SAVE AGAIN", INK_BAD);
}

function fx_reload_sweep() -> void {
	if (RELOAD_SWEEP < 0.0) { return; }
	int band = imax(3, PH / 8);
	int y = RELOAD_SWEEP * (PH + band) - band;
	int glow = theme_of(G.floor).glow;
	pane_tint(0, y - band * 2, PW, y, glow, 0.18, 1.1);
	pane_tint(0, y, PW, y + band, mix_rgb(glow, 0xFFFFFF, 0.5), 0.55, 1.25);
	int edge = pal(mix_rgb(glow, 0xFFFFFF, 0.8));
	for (int x = 0; x < PW; ++x) { fx_plot(x, y + band, edge, 0.0); }
}
