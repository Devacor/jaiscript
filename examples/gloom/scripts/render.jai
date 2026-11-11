// The Renderer: one class owning the view geometry, the z-buffer, the wall
// tone tables and texture strips, the sprite shading cache, the ray
// workspaces and the automap. The view is a palette-index pixel grid blitted
// through sub-cell block glyphs:
//
//   half  (--pix half): 1x2 px/cell, upper-half-block, fg=top bg=bottom
//   quad  (--pix quad): 2x2 px/cell, quadrant glyphs, 2 colors by luminosity
//   sext  (--pix sext): 2x3 px/cell, sextant glyphs (needs a Unicode-13 font)
//   ascii (--ascii):    1x1 px/cell, a density character over a dimmed background
//
// The reference look casts a ray per cell column over a prebaked floor and
// ceiling (reference_view); the showcase look casts one per pixel column and
// textures and lights every pixel (showcase_view, tables from look.jai). The
// parallel row body reads its inputs as captured globals, and captured reads
// admit only globals, so the pixel grid and the glyph tables live outside the
// class.

int PIXW = 2;           // pixels per cell horizontally (set by gloom_boot)
int PIXH = 2;           // pixels per cell vertically
array<int> PIX = [];    // the pixel grid; the parallel row body borrows it
int VW = 0;
int VH = 0;
array<int> ROWIDX = []; // parallel_transform input: one element per glyph row
array<string> QUADG = [];   // quadrant glyphs by TL|TR<<1|BL<<2|BR<<3
string HALFG = "";      // upper half block
string RESET = "";

// quad mode: one glyph row, 2x2 pixels per cell, two colors by luminosity
// partition. A parallel_transform body over captured globals: PIX and PAL_LUM
// are borrowed zero-copy, PAL_FG, PAL_BG and QUADG snapshot per worker, VW and
// RESET copy as scalars. The same function runs in a serial loop without workers.
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
		// wall slices split cells into vertical or horizontal halves far more
		// often than not: two-color paths before the full partition
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

// A column range of wall rays, owned by one parallel_for iteration: the pose
// (rewritten each frame), the map snapshot, and the per-column results.
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
	array<float> odist = [];      // per column: perpendicular distance
	array<int> oside = [];        // per column: 0 x-side, 1 y-side
	array<int> okind = [];        // per column: wall kind
	array<int> otex = [];         // per column: texture column 0..63

	WallChunk(int x0_, int x1_, int vw_, int mw_, array<int> pack_) {
		x0 = x0_;
		x1 = x1_;
		vw = vw_;
		mw = mw_;
		pack = pack_;
		for (int k = 0; k < x1_ - x0_; ++k) {
			odist.push(0.0);
			oside.push(0);
			okind.push(0);
			otex.push(0);
		}
	}
}

// a sprite queued for the far-to-near paint
class Billboard {
public:
	float depth = 0.0;
	int scx = 0;
	string art = "";
	float size = 0.0;
	float zlift = 0.0;
	int flash = 0;
	float wx = 0.0;
	float wy = 0.0;

	Billboard(float depth_, int scx_, string art_, float size_, float zlift_, int flash_, float wx_, float wy_) {
		depth = depth_;
		scx = scx_;
		art = art_;
		size = size_;
		zlift = zlift_;
		flash = flash_;
		wx = wx_;
		wy = wy_;
	}
}

class Renderer {
public:
	int view_w = 0;
	int view_h = 0;
	bool automap = false;
	array<float> zbuf = [];
	array<string> rowparts = [];
	int horizon = 0;              // the pixel row the eye looks along: view_h / 2, moved by bob and shake
	shared_ptr<LightGrid> lights = null;

	Renderer(int w, int h) {
		view_w = w * PIXW;
		view_h = (h - 6) * PIXH;
		VW = view_w;
		VH = view_h;
		PIX = [];
		for (int i = 0; i < view_w * view_h; ++i) { PIX.push(0); }
		ROWIDX = [];
		for (int r = 0; r < view_h / PIXH; ++r) { ROWIDX.push(r); }
		for (int i = 0; i < view_w; ++i) { zbuf.push(99.0); }
		horizon = view_h / 2;
		lights = new LightGrid();
	}

	// glyph tables and fixed colors; palette index 0 is black, the grid's clear value
	void init() {
		RESET = `${ESC}[0m`;
		rgb_idx(0, 0, 0);
		HALFG = utf8(0x2580);
		QUADG = [" "];
		array<int> quad_cps = [0x2598, 0x259D, 0x2580, 0x2596, 0x258C, 0x259E, 0x259B,
			0x2597, 0x259A, 0x2590, 0x259C, 0x2584, 0x2599, 0x259F, 0x2588];
		for (auto cp : quad_cps) { QUADG.push(utf8(cp)); }
		sextg = [];
		for (int b = 0; b < 64; ++b) {
			if (b == 0) { sextg.push(" "); }
			else if (b == 21) { sextg.push(utf8(0x258C)); }        // left half
			else if (b == 42) { sextg.push(utf8(0x2590)); }        // right half
			else if (b == 63) { sextg.push(utf8(0x2588)); }        // full
			else {
				int cp = 0x1FB00 + b - 1;
				if (b > 21) { cp = cp - 1; }
				if (b > 42) { cp = cp - 1; }
				sextg.push(utf8(cp));
			}
		}
		am_cols = [0, rgb_idx(150, 140, 120), rgb_idx(110, 150, 110), rgb_idx(110, 120, 160), rgb_idx(160, 110, 96),
			rgb_idx(200, 170, 110), rgb_idx(90, 230, 110), rgb_idx(150, 140, 120), rgb_idx(230, 70, 60), rgb_idx(90, 120, 240)];
		am_floor = rgb_idx(40, 40, 46);
		am_blink = rgb_idx(240, 255, 160);
		am_item = rgb_idx(255, 220, 80);
		am_enemy = rgb_idx(255, 60, 50);
		am_self = rgb_idx(255, 255, 255);
		am_head = rgb_idx(255, 250, 140);
		hurt_red = rgb_idx(200, 30, 24);
		G.fx.build_colors();
	}

	// per-map tone tables, the prebaked ceiling/floor background, fresh caches
	void build_tables(int mi) {
		lutc = {};
		if (SHOWCASE) {
			build_showcase_tables(mi);
			return;
		}
		build_reference_tables(mi);
		// one deep-copy assignment per frame replaces the per-pixel ceiling/floor loops
		int half = view_h / 2;
		bg = [];
		for (int y = 0; y < view_h; ++y) {
			int c = 0;
			if (y < half) { c = ceilp[y]; }
			else { c = floorp[y]; }
			for (int x = 0; x < view_w; ++x) { bg.push(c); }
		}
		// 32-tall texture strips build lazily per (kind, side, shade, column)
		strips = [];
		strip_set = [];
		for (int i = 0; i < 10 * 2 * 16 * 32; ++i) {
			strips.push([]);
			strip_set.push(false);
		}
	}

	// ---------------------------------------------------------- the view ----
	array<string> draw_world() {
		int vw = view_w;
		int vh = view_h;
		float px = G.player.px;
		float py = G.player.py;
		float pang = SHOWCASE ? G.player.pang + G.feel.yaw : G.player.pang;
		float dirx = math::cos(pang);
		float diry = math::sin(pang);
		float planex = 0.0 - diry * 0.78;
		float planey = dirx * 0.78;
		if (SHOWCASE) {
			showcase_view(px, py, dirx, diry, planex, planey);
		} else {
			reference_view(px, py, dirx, diry, planex, planey);
		}

		// --- 3. billboards: enemies, pickups, projectiles, far to near
		float det = planex * diry - dirx * planey;
		if (det < 0.0001 && det > -0.0001) { det = 0.0001; }
		float inv = 1.0 / det;
		array<shared_ptr<Billboard>> sprites = [];
		for (auto& e : G.enemies) {
			collect(sprites, e.x, e.y, e.art_name(), ENEMY_DEFS[e.kind].size, 0.0,
				e.flash > 0 ? 1 : 0, inv, dirx, diry, planex, planey);
		}
		for (auto& it : G.items) {
			if (it.taken) { continue; }
			auto& def = ITEM_DEFS[it.kind];
			collect(sprites, it.x, it.y, def.art, def.size, 0.0, 0, inv, dirx, diry, planex, planey);
		}
		array<string> shot_art = ["", "pr_spit", "pr_fire", "pr_hex"];
		for (auto& s : G.shots) {
			collect(sprites, s.x, s.y, shot_art[s.kind], 0.16, 0.35, 0, inv, dirx, diry, planex, planey);
		}
		for (int i = 1; i < sprites.size(); ++i) {   // insertion sort, stable, far first
			shared_ptr<Billboard> rec = sprites[i];
			int j = i - 1;
			while (j >= 0 && sprites[j].depth < rec.depth) {
				sprites[j + 1] = sprites[j];
				j = j - 1;
			}
			sprites[j + 1] = rec;
		}
		for (auto& rec : sprites) { draw_billboard(rec); }

		// --- 4. particles, the gun in hand, overlays
		draw_particles(inv, dirx, diry, planex, planey, px, py);
		if (SHOWCASE) {
			draw_viewmodel();
			G.feel.overlay(view_w, view_h);
		}
		if (automap) { draw_automap(); }
		if (!SHOWCASE && G.player.hurt_flash > 0) { draw_hurt_border(); }

		// --- 5. pixel grid -> glyph rows
		if (HOST_ASCII) { return rows_ascii(); }
		if (PIXH == 3) { return rows_sext(); }
		if (PIXW == 2) { return rows_quad(); }
		return rows_half();
	}

	// the showcase look's view: a ray per pixel column, then each column top to
	// bottom: textured ceiling, the lit wall slice, textured floor with its
	// stains. Every surface reads its light texel and a lit table.
	void showcase_view(float px, float py, float dirx, float diry, float planex, float planey) {
		int vw = view_w;
		int vh = view_h;
		horizon = vh / 2 + G.feel.pitch;
		if (ws_mapgen != G.world.mapgen) {
			build_wall_ws(vw);
			ws_mapgen = G.world.mapgen;
		}
		for (auto& t : wall_ws) {
			t.posx = px;
			t.posy = py;
			t.dirx = dirx;
			t.diry = diry;
			t.planex = planex;
			t.planey = planey;
		}
		if (HOST_WORKERS <= 0) {
			for (auto& t : wall_ws) { gloom_ray_chunk_step(t); }
		} else {
			parallel_for (auto& t : wall_ws) { gloom_ray_chunk_step(t); }
		}
		lights.frame(G.feel.sources);

		// the floor or ceiling distance and shade of every pixel row
		float hz = horizon;
		for (int y = 0; y < vh; ++y) {
			float k = y + 0.5 - hz;
			if (k < 0.0) { k = 0.0 - k; }
			float d = k > 0.3 ? vh / (2.0 * k) : 60.0;
			rowd[y] = d;
			int shade = 15 - math::itrunc(d * 1.55);
			rows_shade[y] = shade < 0 ? 0 : shade;
			rows_far[y] = d > FAR_TEXTURE ? 1 : 0;
		}

		auto& pix = PIX;
		auto& zb = zbuf;
		auto& lg = lights.grid;
		int gw = lights.gw;
		int mw = G.world.mw;
		auto& walls = lwall;
		auto& floors = lfloor;
		auto& ceilings = lceil;
		auto& wtex = walltex;
		auto& ftex = floortex;
		auto& ctex = ceiltex;
		auto& fvar = floor_var;
		auto& cvar = ceil_var;
		auto& stain = stains;
		float xstep = 2.0 / (vw - 1.0);
		int x = 0;
		for (auto& ch : wall_ws) {
			int cols = ch.x1 - ch.x0;
			for (int ci = 0; ci < cols; ++ci) {
				float camx = x * xstep - 1.0;
				float rdx = dirx + planex * camx;
				float rdy = diry + planey * camx;
				float dist = ch.odist[ci];
				int side = ch.oside[ci];
				int kind = ch.okind[ci];
				int tx = ch.otex[ci] >> 1;
				zb[x] = dist;
				// the wall's rows are those whose centers lie within half of the horizon,
				// so every ceiling and floor pixel is strictly nearer than the wall
				float half = 0.5 * vh / dist;
				float ytop = hz - half;
				int y0 = 0 - math::itrunc(math::floor(0.5 - ytop));
				int y1 = math::itrunc(math::floor(hz - 0.5 + half)) + 1;
				int cy0 = y0 < 0 ? 0 : (y0 > vh ? vh : y0);
				int cy1 = y1 > vh ? vh : (y1 < cy0 ? cy0 : y1);

				// ceiling
				int gi = x;
				for (int y = 0; y < cy0; ++y) {
					float d = rowd[y];
					float fx = px + rdx * d;
					float fy = py + rdy * d;
					int ix = cast<int>(fx);
					int iy = cast<int>(fy);
					int u = cast<int>((fx - ix) * 16.0);
					int v = cast<int>((fy - iy) * 16.0);
					int lx = cast<int>(fx * 4.0);
					int ly = cast<int>(fy * 4.0);
					int tone = ctex[rows_far[y] * 512 + cvar[iy * mw + ix] + v * 16 + u];
					pix[gi] = ceilings[(lg[ly * gw + lx] * 16 + rows_shade[y]) * 8 + tone];
					gi += vw;
				}

				// wall
				int shade = 15 - math::itrunc(dist * 1.55);
				shade = shade < 0 ? 0 : (shade > 15 ? 15 : shade);
				float back = dist - 0.03;
				int wlx = cast<int>((px + rdx * back) * 4.0);
				int wly = cast<int>((py + rdy * back) * 4.0);
				int light = lg[wly * gw + wlx];
				int wbase = kind * WALL_TABLE_STRIDE + ((light * 2 + side) * 16 + shade) * 8;
				int tbase = (dist > FAR_TEXTURE ? 9216 : 0) + kind * 1024 + tx;
				float tscale = 65536.0 / (2.0 * half);
				int tstep = cast<int>(tscale);
				int tacc = cast<int>((cy0 + 0.5 - ytop) * tscale);
				gi = cy0 * vw + x;
				for (int y = cy0; y < cy1; ++y) {
					pix[gi] = walls[wbase + wtex[tbase + ((tacc >> 11) & 31) * 32]];
					tacc += tstep;
					gi += vw;
				}

				// floor
				for (int y = cy1; y < vh; ++y) {
					float d = rowd[y];
					float fx = px + rdx * d;
					float fy = py + rdy * d;
					int ix = cast<int>(fx);
					int iy = cast<int>(fy);
					int u = cast<int>((fx - ix) * 16.0);
					int v = cast<int>((fy - iy) * 16.0);
					int lx = cast<int>(fx * 4.0);
					int ly = cast<int>(fy * 4.0);
					int ti = iy * mw + ix;
					int tone = stain[ti * 64 + (v >> 1) * 8 + (u >> 1)];
					if (tone == 0) { tone = ftex[rows_far[y] * 1024 + fvar[ti] + v * 16 + u]; }
					pix[gi] = floors[(lg[ly * gw + lx] * 16 + rows_shade[y]) * FLOOR_TONES + tone];
					gi += vw;
				}
				x = x + 1;
			}
		}
	}

	// a --dev reload: the look's tables and lamps again, the floor's stains kept
	void rebuild_look(int mi) {
		array<int> kept = stains;
		build_showcase_tables(mi);
		stains = kept;
		lutc = {};
		G.fx.build_colors();
	}

	// the showcase look's lit tables, textures and lamps for floor mi
	void build_showcase_tables(int mi) {
		lwall = showcase_lit_wall_tones(mi);
		lfloor = showcase_floor_tones(mi);
		lceil = showcase_ceiling_tones(mi);
		walltex = showcase_wall_texture();
		floortex = showcase_floor_texture(mi);
		ceiltex = showcase_ceiling_texture();
		lights.load();
		auto& world = G.world;
		floor_var = [];
		ceil_var = [];
		stains = [];
		for (int ty = 0; ty < world.mh; ++ty) {
			for (int tx = 0; tx < world.mw; ++tx) {
				floor_var.push((texel_noise(31, tx, ty) & 3) * 256);
				ceil_var.push(lights.lamps[ty * world.mw + tx] ? 256 : 0);
			}
		}
		for (int i = 0; i < world.mw * world.mh * 64; ++i) { stains.push(0); }
		rowd = [];
		rows_shade = [];
		rows_far = [];
		for (int y = 0; y < view_h; ++y) {
			rowd.push(0.0);
			rows_shade.push(0);
			rows_far.push(0);
		}
	}

	// a decal on the floor: tone over the texels within radius of (x, y), each kept with chance keep/256
	void stain_floor(float x, float y, float radius, int tone, int keep) {
		auto& world = G.world;
		int mw = world.mw;
		int x0 = math::itrunc((x - radius) * 8.0);
		int x1 = math::itrunc((x + radius) * 8.0);
		int y0 = math::itrunc((y - radius) * 8.0);
		int y1 = math::itrunc((y + radius) * 8.0);
		float r2 = radius * radius * 64.0;
		for (int sy = imax(0, y0); sy <= imin(world.mh * 8 - 1, y1); ++sy) {
			for (int sx = imax(0, x0); sx <= imin(mw * 8 - 1, x1); ++sx) {
				float dx = sx + 0.5 - x * 8.0;
				float dy = sy + 0.5 - y * 8.0;
				if (dx * dx + dy * dy > r2) { continue; }
				if ((texel_noise(41, sx, sy) & 255) >= keep) { continue; }
				int tile = (sy >> 3) * mw + (sx >> 3);
				if (world.tiles[tile] != 0) { continue; }
				int i = tile * 64 + (sy & 7) * 8 + (sx & 7);
				int old = stains[i];
				if (tone == TONE_BLOOD && old == TONE_SCORCH) { continue; }
				stains[i] = tone;
			}
		}
	}

	// old blood dries: one pass a second darkens a slice of the stains
	void dry_stains(int tick) {
		int n = stains.size();
		int slice = n / 30 + 1;
		int start = (tick % 30) * slice;
		for (int i = start; i < imin(n, start + slice); ++i) {
			if (stains[i] == TONE_BLOOD && (texel_noise(43, i, tick / 30) & 7) == 0) { stains[i] = TONE_BLOOD_DRY; }
		}
	}

	// the reference look's rays and columns: one ray per cell column, the prebaked background, cached texture strips
	void reference_view(float px, float py, float dirx, float diry, float planex, float planey) {
		int vw = view_w;
		int vh = view_h;
		// --- 1. the rays: one per cell column, over persistent column chunks
		int pxw = PIXW;
		int ncols = vw / pxw;
		if (ws_mapgen != G.world.mapgen) {
			build_wall_ws(ncols);
			ws_mapgen = G.world.mapgen;
		}
		for (auto& t : wall_ws) {
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
			for (auto& t : wall_ws) { gloom_ray_chunk_step(t); }
		} else {
			parallel_for (auto& t : wall_ws) { gloom_ray_chunk_step(t); }
		}

		// --- 2. columns: prebaked background + textured wall slices
		PIX = bg;
		auto& pix = PIX;
		auto& zb = zbuf;
		int boost = G.player.muzzle > 0 ? 2 : 0;
		int x = 0;
		for (auto& ch : wall_ws) {
			int cols = ch.x1 - ch.x0;
			for (int ci = 0; ci < cols; ++ci) {
				float dist = ch.odist[ci];
				int side = ch.oside[ci];
				int kind = ch.okind[ci];
				int tx = ch.otex[ci] >> 1;            // 0..31
				int px0 = x * pxw;
				zb[px0] = dist;
				if (pxw == 2) { zb[px0 + 1] = dist; }
				int lineh = cast<int>(1.0 * vh / dist);
				lineh = lineh < 1 ? 1 : lineh;
				int y0 = (vh - lineh) / 2;
				int y1 = y0 + lineh;
				int cy0 = y0 < 0 ? 0 : y0;
				int cy1 = y1 > vh ? vh : y1;
				int shade = 15 - math::itrunc(dist * 1.55) + boost;
				shade = shade < 0 ? 0 : (shade > 15 ? 15 : shade);
				int sidx = ((kind * 2 + side) * 16 + shade) * 32 + tx;
				if (!strip_set[sidx]) {
					strips[sidx] = build_strip(kind, side, shade, tx);
					strip_set[sidx] = true;
				}
				auto& strip = strips[sidx];
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
	}

private:
	array<int> bg = [];
	map<string, array<int>> lutc = {};
	array<int> wallt = [];        // [kind 0..8][side 0..1][shade 0..15][tone] flattened
	array<int> ceilp = [];        // per pixel-row ceiling color
	array<int> floorp = [];       // per pixel-row floor color
	array<array<int>> strips = [];
	array<bool> strip_set = [];
	array<WallChunk> wall_ws = [];
	int ws_mapgen = -1;
	array<string> sextg = [];     // sextant glyphs by TL|TR<<1|ML<<2|MR<<3|BL<<4|BR<<5
	array<int> am_cols = [];      // automap tile colors
	int am_floor = 0;
	int am_blink = 0;
	int am_item = 0;
	int am_enemy = 0;
	int am_self = 0;
	int am_head = 0;
	int hurt_red = 0;
	array<float> band_lum = [1.0, 0.88, 0.76, 0.64, 0.52, 0.42, 0.32, 0.24];
	array<int> lwall = [];        // showcase: [kind][variant][side][shade][tone]
	array<int> lfloor = [];       // showcase: [variant][shade][tone]
	array<int> lceil = [];
	array<int> walltex = [];      // showcase: wall tones [kind][ty][tx]
	array<int> floortex = [];     // showcase: floor tones [variant][v][u]
	array<int> ceiltex = [];
	array<int> floor_var = [];    // per tile: which floor variant, times 256
	array<int> ceil_var = [];     // per tile: 256 under a lamp
	array<int> stains = [];       // per tile 8x8 decal tones, 0 none
	array<float> rowd = [];       // per pixel row: floor or ceiling distance
	array<int> rows_shade = [];
	array<int> rows_far = [];     // per pixel row: 1 past FAR_TEXTURE

	void build_wall_ws(int ncols) {
		wall_ws = [];
		int per_chunk = (ncols + 15) / 16;
		int bx0 = 0;
		while (bx0 < ncols) {
			int bx1 = imin(bx0 + per_chunk, ncols);
			wall_ws.push(WallChunk(bx0, bx1, ncols, G.world.mw, G.world.mappack));
			bx0 = bx1;
		}
	}

	// ------------------------------------------------- the reference look ----
	// the tables of ../gloom/reference/README.md section 4.2: three tones per shade
	void build_reference_tables(int mi) {
		var theme = MAPS[mi]["theme"];
		wallt = [];
		array<float> unseen = [10.0, 10.0, 12.0];
		push_wall_tones(unseen, unseen);
		for (int k = 0; k < 4; ++k) {
			array<float> base = theme["walls"][k];
			push_wall_tones(base, base);
		}
		// kind 5 door, 6 exit, 7 red door, 8 blue door
		push_wall_tones([138.0, 116.0, 94.0], [188.0, 164.0, 132.0]);
		push_wall_tones([64.0, 160.0, 84.0], [220.0, 235.0, 120.0]);
		push_wall_tones([176.0, 50.0, 42.0], [245.0, 130.0, 96.0]);
		push_wall_tones([62.0, 94.0, 198.0], [136.0, 176.0, 255.0]);

		// ceiling / floor gradients, dark at the horizon, quantized to cell rows
		// so flat regions hit the row builders' uniform-cell fast path
		ceilp = [];
		floorp = [];
		array<float> crgb = theme["ceil"];
		array<float> frgb = theme["floor"];
		int half = view_h / 2;
		for (int y = 0; y < view_h; ++y) {
			int yq = (y / PIXH) * PIXH;
			if (y < half) {
				float t = half > 1 ? 1.0 * yq / (half - 1) : 1.0;
				float lum = 1.0 - 0.72 * t;
				ceilp.push(rgb_idx(math::itrunc(crgb[0] * lum), math::itrunc(crgb[1] * lum), math::itrunc(crgb[2] * lum)));
				floorp.push(0);
			} else {
				float t = math::clamp(1.0 * (yq - half) / imax(1, view_h - 1 - half), 0.0, 1.0);
				float lum = 0.26 + 0.74 * t;
				ceilp.push(0);
				floorp.push(rgb_idx(math::itrunc(frgb[0] * lum), math::itrunc(frgb[1] * lum), math::itrunc(frgb[2] * lum)));
			}
		}
	}

	void push_wall_tones(array<float> base, array<float> accent) {
		for (int side = 0; side < 2; ++side) {
			float facing = side == 1 ? 0.70 : 1.0;
			for (int s = 0; s < 16; ++s) {
				float lum = (0.13 + 0.87 * s / 15.0) * facing;
				wallt.push(rgb_idx(math::itrunc(base[0] * lum), math::itrunc(base[1] * lum), math::itrunc(base[2] * lum)));
				float alum = math::min(lum * 1.35, 1.0);
				wallt.push(rgb_idx(math::itrunc(accent[0] * alum), math::itrunc(accent[1] * alum), math::itrunc(accent[2] * alum)));
				float mlum = lum * 0.42;
				wallt.push(rgb_idx(math::itrunc(base[0] * mlum), math::itrunc(base[1] * mlum), math::itrunc(base[2] * mlum)));
			}
		}
	}

	// texture tone at (tx, ty) in 0..31 for a wall kind: 0 base, 1 accent, 2 mortar
	int tex_tone(int kind, int tx, int ty) {
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

	array<int> build_strip(int kind, int side, int shade, int tx) {
		array<int> strip = [];
		int base = ((kind * 2 + side) * 16 + shade) * 3;
		for (int ty = 0; ty < 32; ++ty) { strip.push(wallt[base + tex_tone(kind, tx, ty)]); }
		strip.push(strip[31]);        // fixed-point overshoot guard
		return strip;
	}

	// palette code -> palette index for an art at a shade band, flashing or not; cached
	array<int> sprite_lut(string name, int band, int flash, int variant) {
		string key = `${name}:${band}:${flash}:${variant}`;
		if (lutc.has(key)) { return lutc[key]; }
		array<int> lut = [0];
		if (SHOWCASE) {
			lut = showcase_sprite_lut(name, band, flash, variant);
		} else {
			shared_ptr<Art> art = ARTS[name];
			float lum = band_lum[band];
			for (int c = 1; c < art.rgb.size(); ++c) {
				int r = art.rgb[c][0];
				int g = art.rgb[c][1];
				int b = art.rgb[c][2];
				if (flash == 1) {
					r = imin(255, r + 170);
					g = imin(255, g + 150);
					b = imin(255, b + 140);
				}
				lut.push(rgb_idx(math::itrunc(r * lum), math::itrunc(g * lum), math::itrunc(b * lum)));
			}
		}
		lutc[key] = lut;
		return lut;
	}

	// ----------------------------------------------------------- sprites ----
	// world -> camera -> screen
	void collect(array<shared_ptr<Billboard>>& sprites, float wx, float wy, string art, float size, float zlift,
			int flash, float inv, float dirx, float diry, float planex, float planey) {
		float rx = wx - G.player.px;
		float ry = wy - G.player.py;
		float depth = inv * (planex * ry - planey * rx);
		if (depth < 0.18 || depth > 15.0) { return; }
		float ttx = inv * (diry * rx - dirx * ry);
		int scx = cast<int>((view_w / 2.0) * (1.0 + ttx / depth));
		if (scx < -view_w || scx > view_w * 2) { return; }
		sprites.push(new Billboard(depth, scx, art, size, zlift, flash, wx, wy));
	}

	void draw_billboard(shared_ptr<Billboard> rec) {
		shared_ptr<Art> art = ARTS[rec.art];
		int aw = art.w;
		int ah = art.h;
		int vh = view_h;
		float depth = rec.depth;
		float span = 1.0 * vh / depth;
		int ph = imax(2, math::itrunc(span * rec.size));
		// pixel aspect: a cell is ~1:2 (w:h) split into PIXW x PIXH
		int pw = imax(2, ph * aw * 2 * PIXW / (ah * PIXH));
		int band = iclamp(math::itrunc(depth * 0.85) - (G.player.muzzle > 0 ? 1 : 0), 0, 7);
		array<int> lut = sprite_lut(rec.art, band, rec.flash, SHOWCASE ? lights.at(rec.wx, rec.wy) : 0);
		int bottom = cast<int>(horizon + span * 0.5 - rec.zlift * span);
		int top = bottom - ph;
		int x0 = rec.scx - pw / 2;
		int cx0 = imax(0, x0);
		int cx1 = imin(view_w, x0 + pw);
		int cy0 = imax(0, top);
		int cy1 = imin(vh, bottom);
		auto& pix = PIX;
		auto& zb = zbuf;
		int vw = view_w;
		for (int x = cx0; x < cx1; ++x) {
			if (depth >= zb[x]) { continue; }
			int ax = (x - x0) * aw / pw;
			for (int y = cy0; y < cy1; ++y) {
				int code = art.pix[((y - top) * ah / ph) * aw + ax];
				if (code > 0) { pix[y * vw + x] = lut[code]; }
			}
		}
	}

	// pixel confetti, clipped by the z-buffer
	void draw_particles(float inv, float dirx, float diry, float planex, float planey, float px, float py) {
		auto& pix = PIX;
		auto& zb = zbuf;
		auto& colors = G.fx.colors;
		int vw = view_w;
		int vh = view_h;
		int half = horizon;
		int pxw = PIXW;
		int pxh = PIXH;
		for (auto& p : G.fx.pool) {
			int kind = p.kind;
			if (kind == 0) { continue; }
			float rx = p.x - px;
			float ry = p.y - py;
			float depth = inv * (planex * ry - planey * rx);
			if (depth < 0.15 || depth > 14.0) { continue; }
			float ttx = inv * (diry * rx - dirx * ry);
			int scx = cast<int>((vw / 2.0) * (1.0 + ttx / depth));
			if (scx < 0 || scx >= vw) { continue; }
			if (depth >= zb[scx]) { continue; }
			float span = 1.0 * vh / depth;
			int top = cast<int>(half + span * 0.5 - p.z * span);
			int phase = 3 - imin(3, p.life / 5);
			int color = colors[kind * 4 + phase];
			int psz = imin(3, math::itrunc(0.07 * span) + 1);
			int psx = imax(1, (psz * 2 * pxw) / pxh);    // square-ish in every mode
			for (int oy = 0; oy < psz; ++oy) {
				int yy = top + oy;
				if (yy < 0 || yy >= vh) { continue; }
				for (int ox = 0; ox < psx; ++ox) {
					int xx = scx + ox;
					if (xx < vw && depth < zb[xx]) { pix[yy * vw + xx] = color; }
				}
			}
		}
	}

	// the showcase look's gun in hand: sways with the walk, kicks on each shot
	void draw_viewmodel() {
		if (automap) { return; }
		auto& player = G.player;
		bool firing = player.cooldown > WEAPONS[player.weapon].cd - 4 || player.gun_kick > 0;
		string name = VIEWMODEL_NAMES[player.weapon];
		if (firing && player.muzzle > 0) { name = `${name}_fire`; }
		shared_ptr<Art> art = VIEWMODELS[name];
		array<int> lut = viewmodel_lut(name, player.muzzle > 0, lights.at(player.px, player.py));
		int vw = view_w;
		int vh = view_h;
		// art pixels are square: a view pixel is PIXW x PIXH of a 1:2 cell
		float scale = vh * 0.4 / 24.0;
		int ph = math::itrunc(art.h * scale);
		int pw = ph * art.w * 2 * PIXW / (art.h * PIXH);
		int sway = math::itrunc(math::sin(player.bob * 0.5) * vw * 0.025);
		int lift = math::itrunc(math::abs(math::cos(player.bob * 0.5)) * scale * 1.5) + player.gun_kick * math::itrunc(scale * 0.9);
		int x0 = vw / 2 - pw / 2 + sway;
		int y0 = vh - ph + lift + (horizon - vh / 2) / 2;
		int cx0 = imax(0, x0);
		int cx1 = imin(vw, x0 + pw);
		int cy1 = imin(vh, y0 + ph);
		auto& pix = PIX;
		for (int y = imax(0, y0); y < cy1; ++y) {
			int row = ((y - y0) * art.h / ph) * art.w;
			int base = y * vw;
			for (int x = cx0; x < cx1; ++x) {
				int code = art.pix[row + (x - x0) * art.w / pw];
				if (code > 0) { pix[base + x] = lut[code]; }
			}
		}
	}

	// palette code -> index for a gun under this floor's light, brighter in its own flash
	array<int> viewmodel_lut(string name, bool flashing, int variant) {
		string key = `${name}:${flashing}:${variant}`;
		if (lutc.has(key)) { return lutc[key]; }
		auto& mood = MOODS[G.map_i];
		shared_ptr<Art> art = VIEWMODELS[name];
		math::color light = math::mix_rgb(mood.light, rgb(255, 255, 255), 0.4) * (flashing ? 1.25 : 0.7) + sprite_light(mood, variant) * 0.8;
		array<int> lut = [0];
		for (int c = 1; c < art.rgb.size(); ++c) {
			math::color color = rgb(art.rgb[c][0], art.rgb[c][1], art.rgb[c][2]);
			string key_char = art.keys[c];
			if (key_char >= "A" && key_char <= "Z") { lut.push(color_idx(color)); }
			else { lut.push(color_idx(math::clamp(color * light, 0.0, 1.0))); }
		}
		lutc[key] = lut;
		return lut;
	}

	// ---------------------------------------------------------- overlays ----
	// arcade automap: square tiles, walls in kind colors, live blips
	void draw_automap() {
		auto& pix = PIX;
		auto& tiles = G.world.tiles;
		int vw = view_w;
		int mw = G.world.mw;
		int mh = G.world.mh;
		int sx = PIXW * 2;
		int sy = PIXH;
		int ox = sx * 2;
		int oy = sy * 2;
		for (int ty = 0; ty < mh; ++ty) {
			for (int tx = 0; tx < mw; ++tx) {
				int t = tiles[ty * mw + tx];
				int c = t == 0 ? am_floor : am_cols[t];
				if (t == 6 && (G.tick & 8) == 0) { c = am_blink; }
				int bx = ox + tx * sx;
				int by = oy + ty * sy;
				for (int yy = 0; yy < sy; ++yy) {
					int rb = (by + yy) * vw + bx;
					for (int xx = 0; xx < sx; ++xx) { pix[rb + xx] = c; }
				}
			}
		}
		// blips: items gold, live enemies red, you white, and your heading
		for (auto& it : G.items) {
			if (it.taken) { continue; }
			blip(ox + math::itrunc(it.x) * sx, oy + math::itrunc(it.y) * sy, am_item);
		}
		for (auto& e : G.enemies) {
			if (!e.alive) { continue; }
			blip(ox + math::itrunc(e.x) * sx, oy + math::itrunc(e.y) * sy, am_enemy);
		}
		auto& player = G.player;
		blip(ox + math::itrunc(player.px) * sx, oy + math::itrunc(player.py) * sy, am_self);
		blip(ox + math::itrunc(player.px + math::cos(player.pang) * 1.4) * sx,
			oy + math::itrunc(player.py + math::sin(player.pang) * 1.4) * sy, am_head);
	}

	void blip(int bx, int by, int c) {
		auto& pix = PIX;
		int vw = view_w;
		if (bx < 0 || by < 0 || bx + 1 >= vw || by + 1 >= view_h) { return; }
		pix[by * vw + bx] = c;
		pix[by * vw + bx + 1] = c;
		pix[(by + 1) * vw + bx] = c;
		pix[(by + 1) * vw + bx + 1] = c;
	}

	// the red border of a hit
	void draw_hurt_border() {
		auto& pix = PIX;
		int vw = view_w;
		int vh = view_h;
		int red = hurt_red;
		int thick = PIXW * 2;
		for (int x = 0; x < vw; ++x) {
			pix[x] = red;
			pix[vw + x] = red;
			pix[(vh - 2) * vw + x] = red;
			pix[(vh - 1) * vw + x] = red;
		}
		for (int y = 0; y < vh; ++y) {
			int rb = y * vw;
			for (int t = 0; t < thick; ++t) {
				pix[rb + t] = red;
				pix[rb + vw - 1 - t] = red;
			}
		}
	}

	// ------------------------------------------- pixel grid -> glyph rows ---
	// half mode: fg = top pixel, bg = bottom pixel, upper-half-block glyph
	array<string> rows_half() {
		int vw = view_w;
		auto& pix = PIX;
		auto& parts = rowparts;
		array<string> rows = [];
		for (int ry = 0; ry < view_h / 2; ++ry) {
			parts.clear();
			int ptop = -1;
			int pbot = -1;
			int itop = ry * 2 * vw;
			int ibot = itop + vw;
			for (int x = 0; x < vw; ++x) {
				int tc = pix[itop + x];
				int bc = pix[ibot + x];
				if (tc != ptop) { parts.push(PAL_FG[tc]); ptop = tc; }
				if (bc != pbot) { parts.push(PAL_BG[bc]); pbot = bc; }
				parts.push(HALFG);
			}
			parts.push(RESET);
			rows.push(parts.join(""));
		}
		return rows;
	}

	// quad mode: gloom_row_quad per row, through parallel_transform when the host grants workers
	array<string> rows_quad() {
		if (HOST_WORKERS <= 0) {
			array<string> rows = [];
			for (int ry = 0; ry < VH / 2; ++ry) { rows.push(gloom_row_quad(ry)); }
			return rows;
		}
		return parallel_transform(ROWIDX, gloom_row_quad);
	}

	// sext mode: 2x3 pixels per cell (Unicode 13 "Symbols for Legacy Computing")
	array<string> rows_sext() {
		int vw = view_w;
		auto& pix = PIX;
		auto& lums = PAL_LUM;
		auto& parts = rowparts;
		array<string> rows = [];
		int cells = vw / 2;
		for (int ry = 0; ry < view_h / 3; ++ry) {
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
					if (p0 != pbg) { parts.push(PAL_BG[p0]); pbg = p0; }
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
				if (fgp != pfg) { parts.push(PAL_FG[fgp]); pfg = fgp; }
				if (bits != 63 && bgp != pbg) { parts.push(PAL_BG[bgp]); pbg = bgp; }
				parts.push(sextg[bits]);
			}
			parts.push(RESET);
			rows.push(parts.join(""));
		}
		return rows;
	}

	// --ascii: one pixel per cell, a density character over a dimmed background
	array<string> rows_ascii() {
		int vw = view_w;
		auto& pix = PIX;
		auto& parts = rowparts;
		array<string> rows = [];
		for (int ry = 0; ry < view_h; ++ry) {
			parts.clear();
			int prev = -1;
			int base = ry * vw;
			for (int x = 0; x < vw; ++x) {
				int p = pix[base + x];
				if (p != prev) { parts.push(PAL_ASCII[p]); prev = p; }
				else { parts.push(PAL_ASCII_CHAR[p]); }
			}
			parts.push(RESET);
			rows.push(parts.join(""));
		}
		return rows;
	}
}
