// The renderer. The view is a flat palette-index pixel grid, blitted to the
// terminal through sub-cell block glyphs (the doom-cli approach):
//
//   half  (--pix half): 1x2 px/cell, upper-half-block, fg=top bg=bottom
//   quad  (--pix quad): 2x2 px/cell, quadrant glyphs, 2 colors by luminosity
//   sext  (--pix sext): 2x3 px/cell, sextant glyphs (needs a Unicode-13 font)
//   ascii (--ascii):    1x1 px/cell, a density character over a dimmed background
//
// The tables it draws from (wall tones, texture strips, sprite LUTs, the
// background) come from the reference look here or the showcase look
// (look.jai); the per-pixel work is the same for both.
//
// Wall columns are computed by parallel_for over persistent ray-chunk pair
// workspaces (pure.jai, in-place outputs); everything after the join is
// ordinary serial painting, demoreel discipline throughout: locals for hot
// scalars, var& for every hot array.

int PIXW = 2;        // pixels per cell horizontally (set by gloom_boot)
int PIXH = 2;        // pixels per cell vertically
// The pixel grid + its geometry live at GLOBAL scope (not Game fields) so the
// glyph-row parallel bodies can read them as CAPTURED state: PIX is a flat
// all-primitive int array consumed only by subscript reads, so the region
// borrows it - zero copies, raw element reads. VW/VH capture as scalars;
// ROWIDX is the parallel_transform input (one element per glyph row).
array<int> PIX = [];
int VW = 0;
int VH = 0;
array<int> ROWIDX = [];
// Persistent ray-chunk workspaces (parallel_for v1): typed-field class records
// — pose header + map snapshot + preallocated typed out columns. Rebuilt only
// when the map snapshot changes (WS_MAPGEN vs G.mapgen); the pose header
// rewrites per frame, the region writes outputs in place.
class WallChunk {
public:
	int x0 = 0;
	int x1 = 0;
	int vw = 0;
	float posx = 0.0;
	float posy = 0.0;
	float dirx = 0.0;
	float diry = 0.0;
	float planex = 0.0;
	float planey = 0.0;
	int mw = 0;
	array<int> pack = [];         // tile snapshot, 15 cells x 4 bits per int64
	array<float> odist = [];      // per column: perp distance
	array<int> oside = [];        // per column: 0 x-side, 1 y-side
	array<int> okind = [];        // per column: wall kind
	array<int> otex = [];         // per column: texture column 0..63

	WallChunk(int x0_, int x1_, int vw_) {
		x0 = x0_;
		x1 = x1_;
		vw = vw_;
		mw = G.mw;
		pack = G.mappack;
		for (int k = 0; k < x1_ - x0_; ++k) {
			odist.push(0.0);
			oside.push(0);
			okind.push(0);
			otex.push(0);
		}
	}
}
array<WallChunk> WALL_WS = [];
int WS_MAPGEN = -1;

function build_wall_ws(int ncols) -> void {
	WALL_WS = [];
	int nchunks = 16;
	int per_chunk = (ncols + nchunks - 1) / nchunks;
	int bx0 = 0;
	while (bx0 < ncols) {
		int bx1 = bx0 + per_chunk;
		if (bx1 > ncols) { bx1 = ncols; }
		WALL_WS.push(new WallChunk(bx0, bx1, ncols));
		bx0 = bx1;
	}
}
string RESET = "";
array<string> QUADG = [];      // quadrant glyphs by TL|TR<<1|BL<<2|BR<<3
array<string> SEXTG = [];      // sextant glyphs by TL|TR<<1|ML<<2|MR<<3|BL<<4|BR<<5
string HALFG = "";   // upper half block
array<int> AM_COLS = [];    // automap tile colors (built at boot)
int AM_FLOOR = 0;
int AM_BLINK = 0;
int AM_ITEM = 0;
int AM_ENEMY = 0;
int AM_SELF = 0;
int AM_HEAD = 0;
int HURT_RED = 0;
array<array<int>> STRIPS = []; // lazy 33-tall wall texture strips by (kind,side,shade,tx)
array<int> STRIP_SET = [];

function init_render() -> void {
	RESET = ESC + "[0m";
	rgb_idx(0, 0, 0);                 // palette index 0 is black (grid clear value)
	HALFG = utf8(0x2580);
	QUADG = [" "];
	array<int> quad_cps = [0x2598, 0x259D, 0x2580, 0x2596, 0x258C, 0x259E, 0x259B,
		0x2597, 0x259A, 0x2590, 0x259C, 0x2584, 0x2599, 0x259F, 0x2588];
	for (auto cp : quad_cps) { QUADG.push(utf8(cp)); }
	SEXTG = [];
	for (int b = 0; b < 64; ++b) {
		if (b == 0) { SEXTG.push(" "); }
		else if (b == 21) { SEXTG.push(utf8(0x258C)); }        // left half
		else if (b == 42) { SEXTG.push(utf8(0x2590)); }        // right half
		else if (b == 63) { SEXTG.push(utf8(0x2588)); }        // full
		else {
			int cp = 0x1FB00 + b - 1;
			if (b > 21) { cp = cp - 1; }
			if (b > 42) { cp = cp - 1; }
			SEXTG.push(utf8(cp));
		}
	}
	AM_COLS = [0, rgb_idx(150, 140, 120), rgb_idx(110, 150, 110), rgb_idx(110, 120, 160), rgb_idx(160, 110, 96),
		rgb_idx(200, 170, 110), rgb_idx(90, 230, 110), rgb_idx(150, 140, 120), rgb_idx(230, 70, 60), rgb_idx(90, 120, 240)];
	AM_FLOOR = rgb_idx(40, 40, 46);
	AM_BLINK = rgb_idx(240, 255, 160);
	AM_ITEM = rgb_idx(255, 220, 80);
	AM_ENEMY = rgb_idx(255, 60, 50);
	AM_SELF = rgb_idx(255, 255, 255);
	AM_HEAD = rgb_idx(255, 250, 140);
	HURT_RED = rgb_idx(200, 30, 24);
	build_particle_colors();
}

// wall tone table: [kind 0..8][side 0..1][shade 0..15][tone] flattened. The
// reference look has three tones (0 base, 1 accent, 2 mortar/frame), the
// showcase look eight (look.jai).
array<int> WALLT = [];
array<int> CEILP = [];     // per pixel-row ceiling color
array<int> FLOORP = [];    // per pixel-row floor color

function push_wall_tones(var& base, var& accent) -> void {
	for (int side = 0; side < 2; ++side) {
		float facing = side == 1 ? 0.70 : 1.0;
		for (int s = 0; s < 16; ++s) {
			float lum = (0.13 + 0.87 * s / 15.0) * facing;
			WALLT.push(rgb_idx(math::itrunc(base[0] * lum), math::itrunc(base[1] * lum), math::itrunc(base[2] * lum)));
			float alum = lum * 1.35;
			alum = alum > 1.0 ? 1.0 : alum;
			WALLT.push(rgb_idx(math::itrunc(accent[0] * alum), math::itrunc(accent[1] * alum), math::itrunc(accent[2] * alum)));
			float mlum = lum * 0.42;
			WALLT.push(rgb_idx(math::itrunc(base[0] * mlum), math::itrunc(base[1] * mlum), math::itrunc(base[2] * mlum)));
		}
	}
}

function build_render_tables(int mi) -> void {
	if (SHOWCASE) { build_showcase_tables(mi); }
	else { build_reference_tables(mi); }
	// prebaked background: one deep-copy assignment per frame replaces the
	// per-pixel ceiling/floor loops outright
	int half = G.vh / 2;
	array<int> bg = [];
	for (int y = 0; y < G.vh; ++y) {
		// (a ternary of two element reads mis-types into an int local; if/else works)
		int c = 0;
		if (y < half) { c = CEILP[y]; }
		else { c = FLOORP[y]; }
		for (int x = 0; x < G.vw; ++x) { bg.push(c); }
	}
	G.bg = bg;
	// wall tone strips (32-tall texture columns) build lazily per combination
	STRIPS = [];
	STRIP_SET = [];
	int nstrips = 10 * 2 * 16 * 32;
	for (int i = 0; i < nstrips; ++i) {
		STRIPS.push([]);
		STRIP_SET.push(0);
	}
	G.lutc = {};
}

// the reference tables (reference/README.md section 4.2)
function build_reference_tables(int mi) -> void {
	var theme = MAPS[mi]["theme"];
	WALLT = [];
	// kind 0 (never rendered): flat dark
	array<float> zero = [10.0, 10.0, 12.0];
	push_wall_tones(zero, zero);
	// kinds 1..4 from the theme
	for (int k = 0; k < 4; ++k) {
		var base = theme["walls"][k];
		push_wall_tones(base, base);
	}
	// kind 5 door, 6 exit, 7 red door, 8 blue door (episode constants)
	array<float> door_b = [138.0, 116.0, 94.0];
	array<float> door_a = [188.0, 164.0, 132.0];
	push_wall_tones(door_b, door_a);
	array<float> exit_b = [64.0, 160.0, 84.0];
	array<float> exit_a = [220.0, 235.0, 120.0];
	push_wall_tones(exit_b, exit_a);
	array<float> red_b = [176.0, 50.0, 42.0];
	array<float> red_a = [245.0, 130.0, 96.0];
	push_wall_tones(red_b, red_a);
	array<float> blue_b = [62.0, 94.0, 198.0];
	array<float> blue_a = [136.0, 176.0, 255.0];
	push_wall_tones(blue_b, blue_a);

	// ceiling / floor gradients (dark at the horizon), quantized to CELL rows
	// so flat regions hit the row builder's uniform-cell fast path
	CEILP = [];
	FLOORP = [];
	var crgb = theme["ceil"];
	var frgb = theme["floor"];
	int half = G.vh / 2;
	for (int y = 0; y < G.vh; ++y) {
		int yq = (y / PIXH) * PIXH;
		if (y < half) {
			float t = half > 1 ? 1.0 * yq / (half - 1) : 1.0;
			float lum = 1.0 - 0.72 * t;
			CEILP.push(rgb_idx(math::itrunc(crgb[0] * lum), math::itrunc(crgb[1] * lum), math::itrunc(crgb[2] * lum)));
			FLOORP.push(0);
		} else {
			float t2 = 1.0 * (yq - half) / imax(1, G.vh - 1 - half);
			t2 = t2 > 1.0 ? 1.0 : (t2 < 0.0 ? 0.0 : t2);
			float lum2 = 0.26 + 0.74 * t2;
			CEILP.push(0);
			FLOORP.push(rgb_idx(math::itrunc(frgb[0] * lum2), math::itrunc(frgb[1] * lum2), math::itrunc(frgb[2] * lum2)));
		}
	}
}

// texture tone at (tx, ty) in 0..31 for a wall kind: 0 base, 1 accent, 2 mortar
function tex_tone(int kind, int tx, int ty) -> int {
	if (kind == 1) {
		int tone = ((tx >> 2) + (ty >> 2)) & 1;
		if ((ty & 7) == 7) { tone = 2; }
		return tone;
	}
	if (kind == 2) {
		int tone = ((tx + ((ty >> 3 & 1) << 2)) >> 3) & 1;
		if ((ty & 7) == 0) { tone = 2; }
		return tone;
	}
	if (kind == 3) {
		int tone = (tx >> 3) & 1;
		if ((tx & 7) == 0) { tone = 2; }
		return tone;
	}
	if (kind == 4) {
		int tone = ((tx >> 4) + (ty >> 4)) & 1;
		if ((tx & 15) == 0 || (ty & 15) == 0) { tone = 2; }
		return tone;
	}
	if (kind == 6) {
		int tone = ((tx + ty) >> 2) & 1;               // hazard stripes
		if (ty > 26) { tone = 2; }
		return tone;
	}
	// doors (plain / red / blue): frame, seam, lock studs
	if (tx < 3 || tx > 28 || ty < 2 || ty > 29) { return 2; }
	if (tx == 15 || tx == 16) { return 2; }
	if (ty >= 14 && ty <= 17 && (tx & 3) == 1) { return 1; }
	return 0;
}

function build_strip(int kind, int side, int shade, int tx) -> array<int> {
	array<int> s = [];
	if (SHOWCASE) {
		int sbase = ((kind * 2 + side) * 16 + shade) * TONES;
		for (int ty = 0; ty < 32; ++ty) { s.push(WALLT[sbase + showcase_tone(kind, tx, ty)]); }
	} else {
		int tbase = ((kind * 2 + side) * 16 + shade) * 3;
		for (int ty = 0; ty < 32; ++ty) { s.push(WALLT[tbase + tex_tone(kind, tx, ty)]); }
	}
	s.push(s[31]);        // fixed-point overshoot guard
	return s;
}

// ---------------------------------------------------------- sprite LUTs -----
// per (art, shade band 0..7, flash) palette-code -> palette-index table, cached
array<float> BAND_LUM = [1.0, 0.88, 0.76, 0.64, 0.52, 0.42, 0.32, 0.24];

function sprite_lut(string name, int band, int flash) -> array<int> {
	string key = `${name}:${band}:${flash}`;
	if (G.lutc.has(key)) { return G.lutc[key]; }
	if (SHOWCASE) {
		array<int> shaded = showcase_sprite_lut(name, band, flash);
		G.lutc[key] = shaded;
		return shaded;
	}
	var art = ARTS[name];
	var rgb = art["rgb"];
	float lum = BAND_LUM[band];
	array<int> lut = [0];
	for (int c = 1; c < rgb.size(); ++c) {
		int r = rgb[c][0];
		int g = rgb[c][1];
		int b = rgb[c][2];
		if (flash == 1) {
			r = imin(255, r + 170);
			g = imin(255, g + 150);
			b = imin(255, b + 140);
		}
		lut.push(rgb_idx(math::itrunc(r * lum), math::itrunc(g * lum), math::itrunc(b * lum)));
	}
	G.lutc[key] = lut;
	return lut;
}

// -------------------------------------------------------------- the view ----
// The entry point hands G's buffers to reference parameters once, so every
// stage below works on plain locals.
function render_view() -> var {
	return render_view_impl(PIX, G.zbuf, G.rowparts);
}

function render_view_impl(var& pix, var& zbuf, var& rowparts) -> var {
	int vw = G.vw;
	int vh = G.vh;
	float dirx = math::cos(G.pang);
	float diry = math::sin(G.pang);
	float planex = 0.0 - diry * 0.78;
	float planey = dirx * 0.78;
	float px = G.px;
	float py = G.py;
	int mw = G.mw;

	// --- 1. the rays: ONE ray per cell column (sub-pixels share it; sprites
	// and particles still use the full pixel grid), parallel_for over
	// PERSISTENT column-chunk workspaces (built on map change, pose header
	// rewritten per frame) - zero per-frame array traffic, in-place outputs.
	int pxw = PIXW;
	int ncols = vw / pxw;
	if (WS_MAPGEN != G.mapgen) {
		build_wall_ws(ncols);
		WS_MAPGEN = G.mapgen;
	}
	for (auto& t : WALL_WS) {
		t.posx = px;
		t.posy = py;
		t.dirx = dirx;
		t.diry = diry;
		t.planex = planex;
		t.planey = planey;
	}
	// serial unless workers are requested: at this grain the region barrier
	// costs more than the ray work it distributes
	if (HOST_WORKERS <= 0) {
		for (auto& t : WALL_WS) { gloom_ray_chunk_step(t); }
	} else {
		parallel_for (auto& t : WALL_WS) { gloom_ray_chunk_step(t); }
	}

	// --- 2. columns: prebaked background + textured wall slices ---
	pix = G.bg;                        // ceiling + floor in ONE deep-copy assign
	int boost = G.muzzle > 0 ? 2 : 0;
	int x = 0;
	for (auto& ch : WALL_WS) {
		int cols = ch.x1 - ch.x0;
		for (int ci = 0; ci < cols; ++ci) {
			float dist = ch.odist[ci];
			int side = ch.oside[ci];
			int kind = ch.okind[ci];
			int tx = ch.otex[ci] >> 1;            // 0..31
			int px0 = x * pxw;
			zbuf[px0] = dist;
			if (pxw == 2) { zbuf[px0 + 1] = dist; }
			int lineh = 1.0 * vh / dist;
			lineh = lineh < 1 ? 1 : lineh;
			int y0 = (vh - lineh) / 2;
			int y1 = y0 + lineh;
			int cy0 = y0 < 0 ? 0 : y0;
			int cy1 = y1 > vh ? vh : y1;
			int shade = 15 - math::itrunc(dist * 1.55) + boost;
			shade = shade < 0 ? 0 : (shade > 15 ? 15 : shade);
			int sidx = ((kind * 2 + side) * 16 + shade) * 32 + tx;
			int have = STRIP_SET[sidx];
			if (have == 0) {
				STRIPS[sidx] = build_strip(kind, side, shade, tx);
				STRIP_SET[sidx] = 1;
			}
			var& strip = STRIPS[sidx];
			int tstep = 65536 / lineh;            // (32 << 11) / lineh
			int tacc = (cy0 - y0) * tstep;
			int gi = cy0 * vw + px0;
			int y = cy0;
			if (pxw == 2) {
				while (y < cy1) {
					int c = strip[tacc >> 11];
					pix[gi] = c;
					pix[gi + 1] = c;
					tacc += tstep;
					gi += vw;
					y = y + 1;
				}
			} else {
				while (y < cy1) {
					pix[gi] = strip[tacc >> 11];
					tacc += tstep;
					gi += vw;
					y = y + 1;
				}
			}
			x = x + 1;
		}
	}

	// --- 3. billboards: enemies, pickups, projectiles (far to near) ---
	float det = planex * diry - dirx * planey;
	if (det < 0.0001 && det > -0.0001) { det = 0.0001; }
	float inv = 1.0 / det;
	array<shared_ptr<Billboard>> sp = [];
	for (auto& e : G.enemies) {
		collect_billboard(sp, e.x, e.y, enemy_art(e), ENEMY_DEFS[e.kind]["size"], 0.0,
			e.flash > 0 ? 1 : 0, inv, dirx, diry, planex, planey);
	}
	for (auto& it : G.items) {
		if (it.taken) { continue; }
		int ik = it.kind;
		collect_billboard(sp, it.x, it.y, ITEM_DEFS[ik]["art"],
			ITEM_DEFS[ik]["size"], 0.0, 0, inv, dirx, diry, planex, planey);
	}
	array<string> shot_art = ["", "pr_spit", "pr_fire", "pr_hex"];
	for (auto& s : G.shots) {
		collect_billboard(sp, s.x, s.y, shot_art[s.kind], 0.16, 0.35, 0, inv, dirx, diry, planex, planey);
	}
	// insertion sort, far first
	for (int i3 = 1; i3 < sp.size(); ++i3) {
		shared_ptr<Billboard> rec = sp[i3];
		float di = rec.depth;
		int j = i3 - 1;
		while (j >= 0) {
			shared_ptr<Billboard> cmp = sp[j];
			if (cmp.depth >= di) { break; }
			sp[j + 1] = cmp;
			j = j - 1;
		}
		sp[j + 1] = rec;
	}
	for (auto& rec2 : sp) { draw_billboard(rec2, pix, zbuf); }

	// --- 4. particles: pixel confetti with z-buffer clipping ---
	var& pcols = PART_COLS;
	int half2 = vh / 2;
	int pxh = PIXH;
	for (auto& prt : G.fx.pool) {
		int pk = prt.kind;
		if (pk == 0) { continue; }
		float rx = prt.x - px;
		float ry = prt.y - py;
		float depth = inv * (planex * ry - planey * rx);
		if (depth < 0.15 || depth > 14.0) { continue; }
		float ttx = inv * (diry * rx - dirx * ry);
		int scx = (vw / 2.0) * (1.0 + ttx / depth);
		if (scx < 0 || scx >= vw) { continue; }
		float zd = zbuf[scx];
		if (depth >= zd) { continue; }
		float span = 1.0 * vh / depth;
		int pyx = half2 + span * 0.5 - prt.z * span;
		int life = prt.life;
		int phase = 3 - imin(3, life / 5);
		int color = pcols[pk * 4 + phase];
		int psz = math::itrunc(0.07 * span) + 1;
		psz = psz > 3 ? 3 : psz;
		int psx = (psz * 2 * pxw) / pxh;           // square-ish regardless of mode
		psx = psx < 1 ? 1 : psx;
		for (int oy = 0; oy < psz; ++oy) {
			int yy = pyx + oy;
			if (yy < 0 || yy >= vh) { continue; }
			for (int ox = 0; ox < psx; ++ox) {
				int xx = scx + ox;
				if (xx >= vw) { continue; }
				float zx = zbuf[xx];
				if (depth < zx) { pix[yy * vw + xx] = color; }
			}
		}
	}

	if (SHOWCASE) { draw_viewmodel(pix); }

	// --- 5. overlays: automap, hurt flash ---
	if (G.automap) { draw_automap(pix, G.tiles); }
	if (G.hurt_flash > 0) {
		int red = HURT_RED;
		int thick = pxw * 2;
		for (int x4 = 0; x4 < vw; ++x4) {
			pix[x4] = red;
			pix[vw + x4] = red;
			pix[(vh - 2) * vw + x4] = red;
			pix[(vh - 1) * vw + x4] = red;
		}
		for (int y4 = 0; y4 < vh; ++y4) {
			int rb = y4 * vw;
			for (int t4 = 0; t4 < thick; ++t4) {
				pix[rb + t4] = red;
				pix[rb + vw - 1 - t4] = red;
			}
		}
	}

	// --- 6. pixel grid -> sub-cell glyph rows -------------------------------
	if (HOST_ASCII) { return rows_ascii(pix, rowparts); }
	if (PIXH == 3) { return rows_sext(pix, rowparts); }
	if (PIXW == 2) { return rows_quad(); }
	return rows_half(pix, rowparts);
}

// half mode: fg = top pixel, bg = bottom pixel, upper-half-block glyph
function rows_half(var& pix, var& parts) -> var {
	int vw = G.vw;
	var& fgs = PAL_FG;
	var& bgs = PAL_BG;
	string hb = HALFG;
	array<string> rows = [];
	int vrows = G.vh / 2;
	for (int ry = 0; ry < vrows; ++ry) {
		parts.clear();
		int ptop = -1;
		int pbot = -1;
		int itop = ry * 2 * vw;
		int ibot = itop + vw;
		for (int x = 0; x < vw; ++x) {
			int tc = pix[itop + x];
			int bc = pix[ibot + x];
			if (tc != ptop) { parts.push(fgs[tc]); ptop = tc; }
			if (bc != pbot) { parts.push(bgs[bc]); pbot = bc; }
			parts.push(hb);
		}
		parts.push(RESET);
		rows.push(parts.join(""));
	}
	return rows;
}

// --ascii: one pixel per cell, a density character over a dimmed background
function rows_ascii(var& pix, var& parts) -> var {
	int vw = G.vw;
	var& cells = PAL_ASCII;
	var& chars = PAL_ASCII_CHAR;
	array<string> rows = [];
	for (int ry = 0; ry < G.vh; ++ry) {
		parts.clear();
		int prev = -1;
		int base = ry * vw;
		for (int x = 0; x < vw; ++x) {
			int p = pix[base + x];
			if (p != prev) { parts.push(cells[p]); prev = p; }
			else { parts.push(chars[p]); }
		}
		parts.push(RESET);
		rows.push(parts.join(""));
	}
	return rows;
}

// quad mode: 2x2 pixels per cell, two colors by luminosity partition.
// ONE glyph row, written as a parallel_transform body over CAPTURED state
// (docs/site/guide/09-containers-strings.html, "Parallel loops"): PIX and PAL_LUM are all-primitive and touched
// only by subscript reads, so the region BORROWS them (zero-copy raw element
// reads - the pixel grid never rides inside the elements); PAL_FG/PAL_BG/QUADG
// are string tables (per-worker snapshots at the barrier, dense re-reads then
// free); VW/RESET capture as scalar copies. No var& aliases of captured names
// (aliases are rejected by admission) - subscript the globals directly. The
// SAME function runs in a plain serial loop when HOST_WORKERS <= 0.
function gloom_row_quad(int ry) -> string {
	array<string> parts = [];
	int vw = VW;
	int cells = vw / 2;
	int pfg = -1;
	int pbg = -1;
	int itop = ry * 2 * vw;
	int ibot = itop + vw;
	for (int cx = 0; cx < cells; ++cx) {
		int xx = cx * 2;
		int p0 = PIX[itop + xx];
		int p1 = PIX[itop + xx + 1];
		int p2 = PIX[ibot + xx];
		int p3 = PIX[ibot + xx + 1];
		if (p0 == p1 && p0 == p2 && p0 == p3) {
			if (p0 != pbg) { parts.push(PAL_BG[p0]); pbg = p0; }
			parts.push(" ");
			continue;
		}
		// wall slices split cells into vertical or horizontal halves far
		// more often than not - cheap two-color paths before the full
		// luminosity partition
		if (p0 == p2 && p1 == p3) {
			if (p0 != pfg) { parts.push(PAL_FG[p0]); pfg = p0; }
			if (p1 != pbg) { parts.push(PAL_BG[p1]); pbg = p1; }
			parts.push(QUADG[5]);                 // left half
			continue;
		}
		if (p0 == p1 && p2 == p3) {
			if (p0 != pfg) { parts.push(PAL_FG[p0]); pfg = p0; }
			if (p2 != pbg) { parts.push(PAL_BG[p2]); pbg = p2; }
			parts.push(QUADG[3]);                 // upper half
			continue;
		}
		int l0 = PAL_LUM[p0];
		int l1 = PAL_LUM[p1];
		int l2 = PAL_LUM[p2];
		int l3 = PAL_LUM[p3];
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
		int thr = (lmin + lmax + 1) / 2;
		int bits = 0;
		if (l0 >= thr) { bits = 1; }
		if (l1 >= thr) { bits += 2; }
		if (l2 >= thr) { bits += 4; }
		if (l3 >= thr) { bits += 8; }
		if (fgp != pfg) { parts.push(PAL_FG[fgp]); pfg = fgp; }
		if (bits != 15 && bgp != pbg) { parts.push(PAL_BG[bgp]); pbg = bgp; }
		parts.push(QUADG[bits]);
	}
	parts.push(RESET);
	return parts.join("");
}

function rows_quad() -> var {
	if (HOST_WORKERS <= 0) {
		array<string> rows = [];
		int vrows = VH / 2;
		for (int ry = 0; ry < vrows; ++ry) { rows.push(gloom_row_quad(ry)); }
		return rows;
	}
	return parallel_transform(ROWIDX, gloom_row_quad);
}

// sext mode: 2x3 pixels per cell (Unicode 13 "Symbols for Legacy Computing")
function rows_sext(var& pix, var& parts) -> var {
	int vw = G.vw;
	var& fgs = PAL_FG;
	var& bgs = PAL_BG;
	var& lums = PAL_LUM;
	var& sg = SEXTG;
	array<string> rows = [];
	int vrows = G.vh / 3;
	int cells = vw / 2;
	for (int ry = 0; ry < vrows; ++ry) {
		parts.clear();
		int pfg = -1;
		int pbg = -1;
		int r0 = ry * 3 * vw;
		int r1 = r0 + vw;
		int r2 = r1 + vw;
		for (int cx = 0; cx < cells; ++cx) {
			int xx = cx * 2;
			int p0 = pix[r0 + xx];
			int p1 = pix[r0 + xx + 1];
			int p2 = pix[r1 + xx];
			int p3 = pix[r1 + xx + 1];
			int p4 = pix[r2 + xx];
			int p5 = pix[r2 + xx + 1];
			if (p0 == p1 && p0 == p2 && p0 == p3 && p0 == p4 && p0 == p5) {
				if (p0 != pbg) { parts.push(bgs[p0]); pbg = p0; }
				parts.push(" ");
				continue;
			}
			int l0 = lums[p0];
			int l1 = lums[p1];
			int l2 = lums[p2];
			int l3 = lums[p3];
			int l4 = lums[p4];
			int l5 = lums[p5];
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
			int thr = (lmin + lmax + 1) / 2;
			int bits = 0;
			if (l0 >= thr) { bits = 1; }
			if (l1 >= thr) { bits += 2; }
			if (l2 >= thr) { bits += 4; }
			if (l3 >= thr) { bits += 8; }
			if (l4 >= thr) { bits += 16; }
			if (l5 >= thr) { bits += 32; }
			if (fgp != pfg) { parts.push(fgs[fgp]); pfg = fgp; }
			if (bits != 63 && bgp != pbg) { parts.push(bgs[bgp]); pbg = bgp; }
			parts.push(sg[bits]);
		}
		parts.push(RESET);
		rows.push(parts.join(""));
	}
	return rows;
}

// a sprite slice queued for the far-to-near paint
class Billboard {
public:
	float depth = 0.0;
	int scx = 0;
	string art = "";
	float size = 0.0;
	float zlift = 0.0;
	int flash = 0;

	Billboard(float depth_, int scx_, string art_, float size_, float zlift_, int flash_) {
		depth = depth_;
		scx = scx_;
		art = art_;
		size = size_;
		zlift = zlift_;
		flash = flash_;
	}
}

// world -> camera -> screen; appends a Billboard
function collect_billboard(var& sp, float wx, float wy, string art, float size, float zlift,
		int flash, float inv, float dirx, float diry, float planex, float planey) -> void {
	float rx = wx - G.px;
	float ry = wy - G.py;
	float depth = inv * (planex * ry - planey * rx);
	if (depth < 0.18 || depth > 15.0) { return; }
	float ttx = inv * (diry * rx - dirx * ry);
	int scx = (G.vw / 2.0) * (1.0 + ttx / depth);
	if (scx < -G.vw || scx > G.vw * 2) { return; }
	sp.push(new Billboard(depth, scx, art, size, zlift, flash));
}

function draw_billboard(var& rec, var& pix, var& zbuf) -> void {
	float depth = rec.depth;
	int scx = rec.scx;
	string name = rec.art;
	float size = rec.size;
	float zlift = rec.zlift;
	int flash = rec.flash;
	var& art = ARTS[name];
	int aw = art["w"];
	int ah = art["h"];
	var& apix = art["pix"];
	int vw = G.vw;
	int vh = G.vh;
	float span = 1.0 * vh / depth;
	int ph = span * size;
	if (ph < 2) { ph = 2; }
	// pixel aspect: a cell is ~1:2 (w:h) split into PIXW x PIXH
	int pw = ph * aw * 2 * PIXW / (ah * PIXH);
	if (pw < 2) { pw = 2; }
	int band = math::itrunc(depth * 0.85) - (G.muzzle > 0 ? 1 : 0);
	band = band < 0 ? 0 : (band > 7 ? 7 : band);
	array<int> lut = sprite_lut(name, band, flash);
	int bottom = vh / 2 + span * 0.5 - zlift * span;
	int top = bottom - ph;
	int x0 = scx - pw / 2;
	int x1 = x0 + pw;
	int cx0 = x0 < 0 ? 0 : x0;
	int cx1 = x1 > vw ? vw : x1;
	int cy0 = top < 0 ? 0 : top;
	int cy1 = bottom > vh ? vh : bottom;
	for (int x = cx0; x < cx1; ++x) {
		float zd = zbuf[x];
		if (depth >= zd) { continue; }
		int ax = (x - x0) * aw / pw;
		for (int y = cy0; y < cy1; ++y) {
			int ay = (y - top) * ah / ph;
			int code = apix[ay * aw + ax];
			if (code > 0) { pix[y * vw + x] = lut[code]; }
		}
	}
}

// arcade automap: square tiles, walls in kind colors, live blips
function draw_automap(var& pix, var& tiles) -> void {
	int vw = G.vw;
	int mw = G.mw;
	int mh = G.mh;
	int sx = PIXW * 2;
	int sy = PIXH;
	int ox = sx * 2;
	int oy = sy * 2;
	var& kc = AM_COLS;
	for (int ty = 0; ty < mh; ++ty) {
		for (int tx = 0; tx < mw; ++tx) {
			int t = tiles[ty * mw + tx];
			int c = t == 0 ? AM_FLOOR : kc[t];
			if (t == 6 && (G.tick & 8) == 0) { c = AM_BLINK; }
			int bx = ox + tx * sx;
			int by = oy + ty * sy;
			for (int yy = 0; yy < sy; ++yy) {
				int rb = (by + yy) * vw + bx;
				for (int xx = 0; xx < sx; ++xx) { pix[rb + xx] = c; }
			}
		}
	}
	// blips: items gold, live enemies red, you white (+ heading pixel)
	for (auto& it : G.items) {
		if (it.taken) { continue; }
		blip(pix, ox + math::itrunc(it.x) * sx, oy + math::itrunc(it.y) * sy, AM_ITEM);
	}
	for (auto& e : G.enemies) {
		if (!e.alive) { continue; }
		blip(pix, ox + math::itrunc(e.x) * sx, oy + math::itrunc(e.y) * sy, AM_ENEMY);
	}
	blip(pix, ox + math::itrunc(G.px) * sx, oy + math::itrunc(G.py) * sy, AM_SELF);
	blip(pix, ox + math::itrunc(G.px + math::cos(G.pang) * 1.4) * sx, oy + math::itrunc(G.py + math::sin(G.pang) * 1.4) * sy, AM_HEAD);
}

function blip(var& pix, int bx, int by, int c) -> void {
	int vw = G.vw;
	int vh = G.vh;
	if (bx < 0 || by < 0 || bx + 1 >= vw || by + 1 >= vh) { return; }
	pix[by * vw + bx] = c;
	pix[by * vw + bx + 1] = c;
	pix[(by + 1) * vw + bx] = c;
	pix[(by + 1) * vw + bx + 1] = c;
}
