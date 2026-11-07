// KINSTEIN 3D — a Wolfenstein-style grid raycaster: one DDA ray per screen
// column over an authored 24x24 keep, perpendicular-distance slices (no fisheye
// by construction), TEXTURED: the ray's hit coordinate along the wall picks a
// texel column and each pixel of the slice steps down it (16x16 textures grown
// in script: coursed stone, mossy stone, fluted gold, and a banner of animated
// rainbow fire). Floor and ceiling are cast per pixel (flagstones below, oak
// planks above), everything fogs with distance. Billboard Grublins and Orglis
// brutes garrison the route, occluded per column by the wall z-buffer, lit as
// spheres per pixel, with eyes and grins. The scripted camera FIGHTS them: when
// an alive enemy sits in the crosshair cone with clear line of sight, the gun
// auto-fires — recoil, muzzle flash, rainbow hitscan tracer, hit-flash, a gib
// scatter, a kill counter in pixel letters. Enemies respawn each lap. The ray
// fan comes from the camera-plane vector: no per-column trig.

class Kinstein : Scene {
	array<int> wmap = [];
	int mw = 24;
	int mh = 24;
	array<float> wpx = [];
	array<float> wpy = [];
	array<float> seglen = [];
	int nwp = 6;
	float path_len = 0.0;
	float posx = 2.5;
	float posy = 2.5;
	float dirx = 0.0;
	float diry = 1.0;
	float planex = 0.0;
	float planey = 0.0;
	array<int> tex = [];     // 3 wall textures + floor + ceiling, 16x16 texel indices 0..7
	int wall_base = 0;       // ((type*2 + side)*8 + fog)*8 + texel
	int banner_base = 0;     // (side*8 + fog)*16 + hue
	int floor_base = 0;      // fog*8 + texel
	int ceil_base = 0;
	int sprite_base = 0;
	float tt = 0.0;
	// the bestiary (parallel arrays): type 0 grublin, 1 orglis
	array<float> ex = [];
	array<float> ey = [];
	array<int> et = [];
	array<int> es = [];      // 0 alive, 1 hit-flash, 2 gibbing, 3 down (respawns)
	array<float> est = [];
	array<float> stx = [];   // per-frame camera-space transforms
	array<float> sty = [];
	array<int> drawn = [];
	array<float> zcol = [];  // per-column wall depth from the last wall pass
	var gun = [];
	int ne = 11;
	int kills = 0;
	float cool = 0.6;
	float muzzle_t = 0.0;
	float tracer_t = 0.0;
	float hot_t = 0.0;
	int tr_x = 0;
	int tr_y = 0;

	void build() override {
		name = "KINSTEIN 3D";
		handle = "grublin keep . one ray per column, zero mercy";
		// # stone  M moss  G gold pillar  B banner (rainbow-fire, animated)
		var rows = [
			"##################BBBB##",
			"#......##########......#",
			"#......................#",
			"#......MMMMMMMMMM......#",
			"#...G..##########......#",
			"##.##############......#",
			"##.##################.##",
			"#M.M#################.##",
			"##...B###############.##",
			"##.#################M.M#",
			"##.#######MMMMMM#####.##",
			"##.#######......#####.##",
			"##.#######............##",
			"##.#######...G..########",
			"##.#######......########",
			"##.#######MM.MMM########",
			"##.#########.###########",
			"##.#########.###########",
			"#......#####.###########",
			"#..G.G.#####.###########",
			"#......#####.###########",
			"#............###########",
			"#......#################",
			"##BBBB##################"
		];
		for (int y = 0; y < mh; ++y) {
			string row = rows[y];
			for (int x = 0; x < mw; ++x) {
				string ch = row.substr(x, 1);
				int v = 0;
				if (ch == "#") { v = 1; }
				else if (ch == "M") { v = 2; }
				else if (ch == "G") { v = 3; }
				else if (ch == "B") { v = 4; }
				wmap.push(v);
			}
		}
		var fogc = [9.0, 9.0, 16.0];
		var wall_cols = [
			[[40.0, 36.0, 44.0], [88.0, 88.0, 100.0], [104.0, 104.0, 118.0], [120.0, 120.0, 136.0], [138.0, 138.0, 152.0], [156.0, 156.0, 170.0], [186.0, 186.0, 200.0], [70.0, 70.0, 82.0]],
			[[34.0, 36.0, 34.0], [84.0, 90.0, 92.0], [100.0, 106.0, 108.0], [118.0, 124.0, 126.0], [60.0, 118.0, 50.0], [84.0, 150.0, 62.0], [120.0, 190.0, 80.0], [40.0, 86.0, 36.0]],
			[[90.0, 60.0, 20.0], [140.0, 100.0, 36.0], [176.0, 130.0, 50.0], [206.0, 160.0, 64.0], [232.0, 190.0, 90.0], [252.0, 222.0, 130.0], [255.0, 246.0, 200.0], [118.0, 80.0, 28.0]]];
		wall_base = pal.size();
		for (int t = 0; t < 3; ++t) {
			var cols = wall_cols[t];
			for (int side = 0; side < 2; ++side) {
				for (int f = 0; f < 8; ++f) {
					for (int c = 0; c < 8; ++c) {
						var cc = cols[c];
						fogged(cc[0], cc[1], cc[2], f, side, fogc);
					}
				}
			}
		}
		banner_base = pal.size();
		for (int side2 = 0; side2 < 2; ++side2) {
			for (int f2 = 0; f2 < 8; ++f2) {
				for (int hu = 0; hu < 4; ++hu) {
					var rc = gfx.rainbow_rgb(hu * 1.5708);
					float rr = rc[0]; float rg = rc[1]; float rb = rc[2];
					fogged(40.0, 16.0, 60.0, f2, side2, fogc);
					fogged(rr * 0.45, rg * 0.45, rb * 0.45, f2, side2, fogc);
					fogged(rr, rg, rb, f2, side2, fogc);
					fogged(255.0, 196.0, 80.0, f2, side2, fogc);
				}
			}
		}
		var floor_cols = [[46.0, 38.0, 34.0], [92.0, 80.0, 70.0], [104.0, 92.0, 80.0], [116.0, 102.0, 88.0], [128.0, 112.0, 96.0], [140.0, 124.0, 106.0], [76.0, 66.0, 58.0], [160.0, 142.0, 120.0]];
		floor_base = pal.size();
		for (int f3 = 0; f3 < 8; ++f3) {
			for (int c3 = 0; c3 < 8; ++c3) { var fc = floor_cols[c3]; fogged(fc[0], fc[1], fc[2], f3, 0, fogc); }
		}
		var ceil_cols = [[20.0, 12.0, 8.0], [60.0, 38.0, 22.0], [72.0, 46.0, 26.0], [84.0, 54.0, 30.0], [96.0, 62.0, 34.0], [50.0, 32.0, 18.0], [110.0, 72.0, 40.0], [36.0, 24.0, 14.0]];
		ceil_base = pal.size();
		for (int f4 = 0; f4 < 8; ++f4) {
			for (int c4 = 0; c4 < 8; ++c4) { var kc = ceil_cols[c4]; fogged(kc[0] * 0.8, kc[1] * 0.8, kc[2] * 0.8, f4, 0, fogc); }
		}
		// sprites + combat kit
		sprite_base = pal.size();
		pal.ramp(12, 10.0, 34.0, 12.0, 140.0, 240.0, 110.0);      // +0..11 grublin
		pal.ramp(12, 36.0, 16.0, 10.0, 230.0, 140.0, 80.0);       // +12..23 orglis
		pal.add(255, 255, 255);                                   // +24 hit flash
		pal.add(250, 246, 220);                                   // +25 eye white
		pal.add(20, 8, 8);                                        // +26 pupil / mouth
		pal.ramp(4, 36.0, 38.0, 48.0, 150.0, 156.0, 176.0);       // +27..30 gun
		pal.add(255, 250, 200);                                   // +31 muzzle core
		pal.add(255, 196, 70);                                    // +32 muzzle rim
		pal.add(255, 110, 30);                                    // +33 muzzle tip
		for (int tr = 0; tr < 6; ++tr) {                          // +34..39 rainbow tracer
			var ct = gfx.rainbow_rgb(tr * 1.0471975512);
			pal.add(ct[0], ct[1], ct[2]);
		}
		pal.add(130, 255, 150);                                   // +40 crosshair
		pal.add(255, 84, 64);                                     // +41 crosshair, target down
		pal.add(255, 226, 100);                                   // +42 tally ink
		pal.add(255, 140, 50);                                    // +43 tally ink low
		pal.add(8, 6, 10);                                        // +44 tally shadow
		pal.add(170, 30, 30);                                     // +45 grin
		pal.add(226, 170, 128);                                   // +46 skin
		pal.add(150, 100, 72);                                    // +47 skin shade
		grow_textures();
		gun = ["......dlhd......", "......dlmd......", "......dlmd......", ".....ddlmdd.....", ".....dlhmmd.....",
			"....ddlmmmdd....", "....dlhmmmmd....", "...kdlmmmmmdk...", "..kkdmmmmmmdkk..", ".kkkkdmmmmdkkkk.",
			"skkkkkddddkkkkks", "sskkkkkkkkkkkkss"];
		// the flight loop: west corridor, bottom hall, mid room, east corridor
		wpx = [2.5, 2.5, 12.5, 12.5, 21.5, 21.5];
		wpy = [2.5, 21.5, 21.5, 12.5, 12.5, 2.5];
		for (int i = 0; i < nwp; ++i) {
			float ddx = wpx[(i + 1) % nwp] - wpx[i];
			float ddy = wpy[(i + 1) % nwp] - wpy[i];
			seglen.push(math::sqrt(ddx * ddx + ddy * ddy));
			path_len += seglen[i];
		}
		// garrison, posted along the route (a couple wait around corners)
		ex = [2.5, 2.5, 2.5, 4.5, 5.5, 12.5, 12.5, 14.5, 18.5, 21.5, 20.5];
		ey = [6.5, 12.5, 16.5, 19.5, 21.5, 18.5, 13.5, 12.5, 12.5, 8.5, 2.5];
		et = [0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1];
		for (int i2 = 0; i2 < ne; ++i2) {
			es.push(0);
			est.push(0.0);
			stx.push(0.0);
			sty.push(0.0);
			drawn.push(0);
		}
		zcol = gfx.make_fgrid(w);
		for (int i3 = 0; i3 < w; ++i3) { zcol[i3] = 99.0; }
	}

	// one entry: a color fogged toward the keep's gloom at fog level f (7 = near)
	void fogged(float r, float g, float b, int f, int side, var fogc) {
		float k = (0.10 + 0.90 * f / 7.0) * (side == 1 ? 0.72 : 1.0);
		float fr = fogc[0]; float fg = fogc[1]; float fb = fogc[2];
		pal.add(r * k + fr * (1.0 - k), g * k + fg * (1.0 - k), b * k + fb * (1.0 - k));
	}

	int hash2(int a, int b) {
		int hs = (a * 73856093 + b * 19349663 + (seed & 255) * 83492791) & 1048575;
		return ((hs * 1274126177) >> 12) & 1023;
	}

	void grow_textures() {
		// 0 coursed stone, 1 mossy stone, 2 fluted gold, 3 flagstones, 4 oak planks
		for (int t = 0; t < 5; ++t) {
			for (int v = 0; v < 16; ++v) {
				for (int u = 0; u < 16; ++u) {
					int c = 0;
					if (t <= 1) {
						int row = v / 4;
						int bu = (u + (row % 2) * 4) % 8;
						int bv = v % 4;
						int brick = hash2((u + (row % 2) * 4) / 8 + row * 7, t);
						c = 1 + brick % 4;
						if (bv == 1) { ++c; }
						if (bv == 3 && c > 1) { --c; }
						if (bu == 0 || bv == 0) { c = 0; }
						if (t == 1 && c != 0) {
							int m = hash2(u * 3 + v, v * 5 + 11) % 100;
							if (m < v * 5) { c = 4 + m % 3; }
						}
						if (t == 1 && c == 0 && hash2(u, v) % 3 == 0) { c = 7; }
					} else if (t == 2) {
						int fl = u % 4;
						c = fl == 0 ? 7 : (fl == 1 ? 5 : (fl == 2 ? 4 : 2));
						if (v % 8 == 0) { c = 6; }
						if (v % 8 == 1) { c = 1; }
					} else if (t == 3) {
						int su = (u + (v / 8) * 4) % 8;
						int sv = v % 8;
						int stone = hash2((u + (v / 8) * 4) / 8, v / 8 + 40);
						c = 1 + stone % 5;
						if (sv == 1 || su == 1) { c = 7; }
						if (su == 0 || sv == 0) { c = 0; }
						if (hash2(u + 50, v) % 11 == 0 && c != 0) { c = 6; }
					} else {
						int plank = v / 4;
						int pv = v % 4;
						c = 2 + (hash2(plank, 90) % 3);
						if ((u + plank * 5) % 16 == 0) { c = 0; }
						if (pv == 0) { c = 5; }
						if (pv == 3) { c = 7; }
						if (hash2(u, v + 90) % 13 == 0) { c = 6; }
					}
					tex.push(c);
				}
			}
		}
	}

	var path_at(float s) {
		s -= math::floor(s / path_len) * path_len;
		for (int i = 0; i < nwp; ++i) {
			float L = seglen[i];
			if (s <= L) {
				float f = L > 0.0001 ? s / L : 0.0;
				int j = (i + 1) % nwp;
				return [wpx[i] + (wpx[j] - wpx[i]) * f, wpy[i] + (wpy[j] - wpy[i]) * f];
			}
			s -= L;
		}
		return [wpx[0], wpy[0]];
	}

	// DDA toward (dx, dy) in parameter space: true if no wall before frac of the way
	bool los_clear(float dx, float dy, float frac) {
		int mx = itrunc(posx);
		int my = itrunc(posy);
		float adx = dx < 0.0 ? -dx : dx;
		float ady = dy < 0.0 ? -dy : dy;
		float ddx = adx < 0.00000001 ? 100000000.0 : 1.0 / adx;
		float ddy = ady < 0.00000001 ? 100000000.0 : 1.0 / ady;
		int stepx = dx < 0.0 ? -1 : 1;
		int stepy = dy < 0.0 ? -1 : 1;
		float sdx = dx < 0.0 ? (posx - mx) * ddx : (mx + 1.0 - posx) * ddx;
		float sdy = dy < 0.0 ? (posy - my) * ddy : (my + 1.0 - posy) * ddy;
		for (int g = 0; g < 64; ++g) {
			float trav = sdx < sdy ? sdx : sdy;
			if (trav >= frac) { return true; }
			if (sdx < sdy) { sdx += ddx; mx += stepx; }
			else { sdy += ddy; my += stepy; }
			int cv = wmap[my * mw + mx];
			if (cv > 0) { return false; }
		}
		return false;
	}

	void update(float t, float dt) override {
		tt = t;
		float s = t * 2.4;
		var p = path_at(s);
		var q = path_at(s + 1.7);   // look-ahead steers the turns
		posx = p[0];
		posy = p[1];
		float dx = q[0] - posx;
		float dy = q[1] - posy;
		float dl = math::sqrt(dx * dx + dy * dy);
		if (dl > 0.0001) { dirx = dx / dl; diry = dy / dl; }
		planex = 0.0 - diry * 0.66;
		planey = dirx * 0.66;
		// enemy state machine
		cool -= dt;
		muzzle_t -= dt;
		tracer_t -= dt;
		hot_t -= dt;
		for (int i = 0; i < ne; ++i) {
			int st = es[i];
			if (st == 1) { est[i] = est[i] + dt; if (est[i] > 0.14) { es[i] = 2; est[i] = 0.0; } }
			else if (st == 2) { est[i] = est[i] + dt; if (est[i] > 0.55) { es[i] = 3; est[i] = 0.0; } }
			else if (st == 3) { est[i] = est[i] + dt; if (est[i] > 16.0) { es[i] = 0; est[i] = 0.0; } }
		}
		// auto-fire: nearest alive enemy in the crosshair cone with clear LOS
		if (cool <= 0.0) {
			float invdet = 1.0 / (planex * diry - dirx * planey);
			int best = -1;
			float besty = 99.0;
			float bestx = 0.0;
			for (int i2 = 0; i2 < ne; ++i2) {
				int st2 = es[i2];
				if (st2 != 0) { continue; }
				float rx = ex[i2] - posx;
				float ry = ey[i2] - posy;
				float txx = invdet * (diry * rx - dirx * ry);
				float tyy = invdet * (planex * ry - planey * rx);
				if (tyy < 1.0 || tyy > 4.2) { continue; }
				float aim = txx < 0.0 ? -txx : txx;
				if (aim > tyy * 0.14) { continue; }
				if (!los_clear(rx, ry, 0.95)) { continue; }
				if (tyy < besty) { best = i2; besty = tyy; bestx = txx; }
			}
			if (best >= 0) {
				es[best] = 1;
				est[best] = 0.0;
				++kills;
				cool = 0.7;
				muzzle_t = 0.12;
				tracer_t = 0.14;
				hot_t = 0.35;
				tr_x = itrunc(w * 0.5 * (1.0 + bestx / besty));
				tr_y = h / 2 + itrunc(h * 0.10 / besty);
			}
		}
	}

	// floor and ceiling, cast per pixel: each row below the horizon is a fixed
	// distance, stepped across the screen along the camera plane
	void cast_planes(var& grid) {
		int ww = w;
		int hh = h;
		int half = hh / 2;
		var& tx = tex;
		int fb = floor_base;
		int cb = ceil_base;
		float rdx0 = dirx - planex;
		float rdy0 = diry - planey;
		float rdx1 = dirx + planex;
		float rdy1 = diry + planey;
		for (int y = half; y < hh; ++y) {
			float p = y + 0.5 - hh * 0.5;
			float rowd = hh * 0.5 / p;
			float stx_ = rowd * (rdx1 - rdx0) / ww;
			float sty_ = rowd * (rdy1 - rdy0) / ww;
			float fx = posx + rowd * rdx0 + 64.0;
			float fy = posy + rowd * rdy0 + 64.0;
			int fog = itrunc(19.2 / (rowd + 1.4));
			fog = fog > 7 ? 7 : fog;
			int frow = fb + fog * 8;
			int crow = cb + fog * 8;
			int gf = y * ww;
			int gc = (hh - 1 - y) * ww;
			for (int x = 0; x < ww; ++x) {
				int iu = fx * 16.0;
				int iv = fy * 16.0;
				int ti = (iv & 15) * 16 + (iu & 15);
				grid[gf + x] = frow + tx[768 + ti];
				grid[gc + x] = crow + tx[1024 + ti];
				fx += stx_;
				fy += sty_;
			}
		}
	}

	// one billboard: an ellipsoid body lit as a sphere per pixel, eyes with
	// pupils, a grin; walls occlude per column; hit-flash paints it white
	void draw_blob(var& grid, int i) {
		float tyy = sty[i];
		float txx = stx[i];
		int scx = itrunc(w * 0.5 * (1.0 + txx / tyy));
		int typ = et[i];
		float size = typ == 1 ? 1.1 : 0.78;
		int sh = itrunc(h * size / tyy);
		sh = sh < 3 ? 3 : sh;
		sh = sh > 64 ? 64 : sh;
		float hwm = (sh + (typ == 1 ? 6.0 : 2.0)) * 0.5;
		int feet = h / 2 + itrunc(h * 0.5 / tyy);
		int top = feet - sh;
		float wob = math::sin(tt * 5.0 + i * 2.1) * 0.10;
		bool flash = es[i] == 1;
		int body = sprite_base + typ * 12;
		float fogk = 1.1 - tyy * 0.06;
		fogk = fogk < 0.45 ? 0.45 : (fogk > 1.0 ? 1.0 : fogk);
		for (int dy = 0; dy < sh; ++dy) {
			int rr = top + dy;
			if (rr < 0 || rr >= h) { continue; }
			float v = (dy + 0.5) / sh * 2.0 - 1.0;
			float vv = 1.0 - v * v;
			if (vv <= 0.0) { continue; }
			float hwf = math::sqrt(vv) * hwm;
			int hw = itrunc(hwf);
			int cxr = scx + itrunc(wob * v * hwm);
			int rowb = rr * w;
			for (int dx = 0 - hw; dx <= hw; ++dx) {
				int col = cxr + dx;
				if (col < 0 || col >= w) { continue; }
				if (tyy >= zcol[col]) { continue; }
				int pi = sprite_base + 24;
				if (!flash) {
					float nx = dx / (hwm + 0.5);
					float q = 1.0 - nx * nx - v * v;
					float nz = q > 0.0 ? math::sqrt(q) : 0.0;
					float li = -0.45 * nx - 0.55 * v + 0.70 * nz;
					li = li < 0.0 ? 0.0 : li;
					int lv = itrunc((0.12 + 0.88 * li) * fogk * 11.99);
					pi = body + lv;
					float ax = nx < 0.0 ? -nx : nx;
					if (v > -0.5 && v < -0.12 && ax > 0.14 && ax < 0.52) {
						float ey2 = (v + 0.31) * 5.0;
						float ex2 = (ax - 0.33) * 5.0;
						if (ex2 * ex2 + ey2 * ey2 < 1.0) {
							pi = ex2 * ex2 + ey2 * ey2 < 0.3 ? sprite_base + 26 : sprite_base + 25;
						}
					}
					if (v > 0.18 && v < 0.34 && ax < 0.42) {
						pi = (dx & 1) == 0 && v < 0.24 ? sprite_base + 25 : sprite_base + 45;
					}
				}
				grid[rowb + col] = pi;
			}
		}
	}

	void render(var& grid) override {
		cast_planes(grid);
		int ww = w;
		int hh = h;
		var& wm = wmap;
		var& tx = tex;
		var& zc = zcol;
		int wb = wall_base;
		int bb = banner_base;
		int tshift = itrunc(tt * 14.0);
		for (int x = 0; x < ww; ++x) {
			float camx = 2.0 * x / (ww - 1.0) - 1.0;
			float rdx = dirx + planex * camx;
			float rdy = diry + planey * camx;
			int mx = itrunc(posx);
			int my = itrunc(posy);
			float adx = rdx < 0.0 ? -rdx : rdx;
			float ady = rdy < 0.0 ? -rdy : rdy;
			float ddx = adx < 0.00000001 ? 100000000.0 : 1.0 / adx;
			float ddy = ady < 0.00000001 ? 100000000.0 : 1.0 / ady;
			int stepx = rdx < 0.0 ? -1 : 1;
			int stepy = rdy < 0.0 ? -1 : 1;
			float sdx = rdx < 0.0 ? (posx - mx) * ddx : (mx + 1.0 - posx) * ddx;
			float sdy = rdy < 0.0 ? (posy - my) * ddy : (my + 1.0 - posy) * ddy;
			int side = 0;
			int hitv = 0;
			int guard = 0;
			while (hitv == 0 && guard < 64) {
				if (sdx < sdy) { sdx += ddx; mx += stepx; side = 0; }
				else { sdy += ddy; my += stepy; side = 1; }
				int cellv = wm[my * mw + mx];
				hitv = cellv;
				++guard;
			}
			hitv = hitv == 0 ? 1 : hitv;
			float pdist = side == 0 ? sdx - ddx : sdy - ddy;   // perpendicular: no fisheye
			pdist = pdist < 0.05 ? 0.05 : pdist;
			zc[x] = pdist;
			float wallx = side == 0 ? posy + pdist * rdy : posx + pdist * rdx;
			wallx -= math::floor(wallx);
			int tu = itrunc(wallx * 16.0) & 15;
			float lineh = hh / pdist;
			float topf = (hh - lineh) * 0.5;
			int y0 = itrunc(topf);
			int y1 = itrunc(topf + lineh);
			y0 = y0 < 0 ? 0 : y0;
			y1 = y1 > hh ? hh : y1;
			int fog = itrunc(19.2 / (pdist + 1.4));
			fog = fog > 7 ? 7 : fog;
			float tstep = 16.0 / lineh;
			float tpos = (y0 + 0.5 - topf) * tstep;
			int gi = y0 * ww + x;
			if (hitv == 4) {
				int brow = bb + (side * 8 + fog) * 16;
				// rainbow fire: ragged flame tongues per texel column, hue bands scroll up
				int wave = (tu * 3 + tshift) % 12;
				int flen = 6 + (wave < 6 ? wave : 12 - wave);
				int bank = (((tu >> 2) + (tshift >> 3)) & 3) * 4;
				for (int y = y0; y < y1; ++y) {
					int tv = tpos;
					int up = 15 - (tv & 15);
					int heat = up > flen ? 0 : (up * 3 > flen * 2 ? 1 : (up * 3 > flen ? 2 : 3));
					grid[gi] = brow + bank + heat;
					tpos += tstep;
					gi += ww;
				}
			} else {
				int wrow = wb + (((hitv - 1) * 2 + side) * 8 + fog) * 8;
				int tbase = (hitv - 1) * 256 + tu;
				for (int y2 = y0; y2 < y1; ++y2) {
					int tv2 = tpos;
					grid[gi] = wrow + tx[tbase + (tv2 & 15) * 16];
					tpos += tstep;
					gi += ww;
				}
			}
		}
		// sprite pass: camera-space transforms, then paint far to near
		float invdet = 1.0 / (planex * diry - dirx * planey);
		for (int i = 0; i < ne; ++i) {
			float rx = ex[i] - posx;
			float ry = ey[i] - posy;
			stx[i] = invdet * (diry * rx - dirx * ry);
			sty[i] = invdet * (planex * ry - planey * rx);
			drawn[i] = 0;
		}
		for (int pass = 0; pass < ne; ++pass) {
			int pick = -1;
			float pty = -1.0;
			for (int i2 = 0; i2 < ne; ++i2) {
				if (drawn[i2] == 1) { continue; }
				int st = es[i2];
				if (st >= 2) { continue; }
				float sv = sty[i2];
				if (sv <= 0.25) { continue; }
				if (sv > pty) { pty = sv; pick = i2; }
			}
			if (pick < 0) { break; }
			drawn[pick] = 1;
			draw_blob(grid, pick);
		}
		// gib scatter for the dying
		int flick = itrunc(tt * 10.0);
		for (int i3 = 0; i3 < ne; ++i3) {
			int st3 = es[i3];
			if (st3 != 2) { continue; }
			float tyy = sty[i3];
			if (tyy < 0.25) { continue; }
			float prog = est[i3] * 1.9;
			int scx = itrunc(w * 0.5 * (1.0 + stx[i3] / tyy));
			int scy = h / 2 + itrunc(h * 0.22 / tyy);
			float r = prog * (h * 0.45 / tyy);
			int body = sprite_base + et[i3] * 12 + 8;
			for (int k = 0; k < 18; ++k) {
				float a = k * 0.349 + i3;
				int gx = scx + itrunc(math::cos(a) * r * (1.0 + (k % 3) * 0.3));
				int gy = scy + itrunc(math::sin(a) * r + prog * prog * 10.0);
				if (gx < 0 || gx >= w - 1 || gy < 1 || gy >= h - 1) { continue; }
				if (tyy >= zcol[gx]) { continue; }
				int gc = k % 4 == 0 ? sprite_base + 45 : body - (k % 3) * 2;
				grid[gy * w + gx] = gc;
				grid[gy * w + gx + 1] = gc;
			}
		}
		draw_hud(grid, flick);
	}

	void draw_hud(var& grid, int flick) {
		int gx0 = w / 2;
		// rainbow hitscan tracer from the muzzle to the target
		if (tracer_t > 0.0) {
			int y0t = h - 22;
			for (int k2 = 1; k2 < 28; ++k2) {
				int xx = gx0 + (tr_x - gx0) * k2 / 28;
				int yy = y0t + (tr_y - y0t) * k2 / 28;
				if (xx < 0 || xx >= w || yy < 1 || yy >= h) { continue; }
				grid[yy * w + xx] = sprite_base + 34 + (k2 / 2 + flick) % 6;
			}
		}
		// the gun, drawn at 2x: bobs with the walk, kicks on the shot
		int bob = itrunc(math::sin(tt * 7.0) * 2.5);
		int kick = muzzle_t > 0.0 ? 4 : 0;
		int gw = 16;
		int gh = gun.size();
		int gxs = gx0 - gw + bob;
		int gys = h - gh * 2 + 4 + kick + (bob < 0 ? 0 - bob : bob) / 2;
		for (int ry = 0; ry < gh * 2; ++ry) {
			string row = gun[ry / 2];
			int yy2 = gys + ry;
			if (yy2 < 0 || yy2 >= h) { continue; }
			for (int rx = 0; rx < gw * 2; ++rx) {
				string ch = row.substr(rx / 2, 1);
				if (ch == ".") { continue; }
				int c = ch == "d" ? 27 : (ch == "m" ? 28 : (ch == "l" ? 29 : (ch == "h" ? 30 : (ch == "k" ? 46 : 47))));
				int xx2 = gxs + rx;
				if (xx2 >= 0 && xx2 < w) { grid[yy2 * w + xx2] = sprite_base + c; }
			}
		}
		if (muzzle_t > 0.0) {
			int mx = gxs + 15;
			int my = gys - 4;
			for (int q = -5; q <= 5; ++q) {
				int aq = q < 0 ? 0 - q : q;
				int c2 = aq <= 1 ? 31 : (aq <= 3 ? 32 : 33);
				for (int th = 0; th < 2; ++th) {
					if (my + q >= 0 && my + q < h) { grid[(my + q) * w + mx + th] = sprite_base + c2; }
					if (mx + q >= 0 && mx + q < w && my + th >= 0) { grid[(my + th) * w + mx + q] = sprite_base + c2; }
				}
				if (aq <= 3 && mx + q >= 0 && mx + q < w && my + q >= 0 && my + q < h) { grid[(my + q) * w + mx + q] = sprite_base + 32; }
				if (aq <= 3 && mx - q >= 0 && mx - q < w && my + q >= 0 && my + q < h) { grid[(my + q) * w + mx - q] = sprite_base + 32; }
			}
		}
		int chc = hot_t > 0.0 ? sprite_base + 41 : sprite_base + 40;
		int cy = h / 2;
		for (int d = 2; d <= 3; ++d) {
			grid[cy * w + gx0 - d] = chc;
			grid[cy * w + gx0 + d] = chc;
			grid[(cy - d) * w + gx0] = chc;
			grid[(cy + d) * w + gx0] = chc;
		}
		string tally = `KILLS ${(kills < 10 ? "0" : "")}${kills}`;
		gfx.draw_text(grid, w, h, tally, 2, 2, 1, 1, sprite_base + 42, 2, sprite_base + 44, 99);
	}
}
