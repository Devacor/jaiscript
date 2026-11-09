// GLOBHUNTER — a Wolfenstein-style grid raycaster: one DDA ray per screen
// column over an authored 24x24 keep, perpendicular-distance slices (no fisheye
// by construction), TEXTURED: the ray's hit coordinate along the wall picks a
// texel column and each pixel of the slice steps down it (16x16 textures grown
// in script: coursed stone, mossy stone, fluted gold, and a banner of animated
// rainbow fire). Floor and ceiling are cast per pixel (flagstones below, oak
// planks above). LIT: torches flicker along the route and a light map rebuilt
// every frame from them (and from the muzzle on each shot) lights walls, floor,
// ceiling and monsters, warm where the fire reaches and cold blue in the gloom
// between, under a vignette. Billboard Grublins and Orglis brutes garrison the
// route, occluded per column by the wall z-buffer, lit as spheres per pixel,
// with eyes and grins. The scripted camera HUNTS them: it swings onto a mark in
// clear sight, bobs as it walks, and when the mark sits in the crosshair the gun
// fires — recoil kicking the view, muzzle flash lighting the hall, a rainbow
// hitscan tracer, a hit-flash, a hit-stop, goo that bursts in 3D and bounces off
// the flagstones, a callout (DOUBLE KILL on a quick second), a kill counter in
// pixel letters. Enemies respawn each lap. It plays to its cue: the gun fires
// only on a beat (on the eighths through the bar after the big hit), footfalls
// land on the beat, the torches flare with the kick, the big hit on bar five's
// downbeat floods the hall with light, and the bar after the boss slows the walk and holds
// fire on the corridor ahead. Every burst lights the hall around it, the view quakes
// with each kill, the big hit drops into bullet time for two beats (the walk crawls,
// the goo hangs in the air), and on bar seven a boss glob twice a brute's size rises
// in the corridor ahead, eyes burning, swelling on every kick: two bars of fight, a
// hit on every beat flashing it, until it bursts on bar nine's downbeat in bullet
// time, flooding the hall with goo, and the camera walks on through the aftermath.
// Then the keep falls: on bar ten it calls RUN, the torches gutter low and strobe
// back up on every kick, the view trembles, stone shakes loose from the vault on
// every snare, an ambush rises in the mid hall and the gun fires on the eighths as
// the camera sprints (just fast enough to arrive on time) for the east wall, whose
// cracks glow hotter with daylight the closer it gets. On bar twelve's downbeat
// the wall blows in, stone flying at the lens, the frame whites out, KEEP CLEARED,
// and the last bar walks out through the breach as daylight blooms over the frame.
// The ray fan comes from the camera-plane vector: no per-column trig.

array<int> BAYER4 = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];

class GlobHunter : Scene {
	array<int> wmap = [];
	int mw = 24;
	int mh = 24;
	array<float> wpx = [];
	array<float> wpy = [];
	array<float> seglen = [];
	int nwp = 6;
	float path_len = 0.0;
	float travel = 0.0;      // distance along the route: it crawls through each hit-stop
	float posx = 2.5;
	float posy = 2.5;
	float dirx = 0.0;
	float diry = 1.0;
	float planex = 0.0;
	float planey = 0.0;
	float aim = 0.0;         // the eased swing of the view toward the current mark
	float pitch = 0.0;       // the horizon's offset in pixels: walk bob and recoil
	array<int> tex = [];     // 3 wall textures + floor + ceiling, 16x16 texel indices 0..7
	int wall_base = 0;       // ((type*2 + side)*8 + light)*8 + texel
	int banner_base = 0;     // (side*8 + light)*16 + hue
	int floor_base = 0;      // light*8 + texel
	int ceil_base = 0;
	int sprite_base = 0;
	float tt = 0.0;
	int fr = 0;
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
	int ne = 18;             // seventeen of the garrison (three wait in ambush for the escape), and the boss in the last slot
	float boss_flash = 0.0;  // the boss shrugs off each hit with a flash until bar nine
	int kills = 0;
	float cool = 0.6;
	int last_step = -1;      // the last beat (or eighth) the gun was allowed to fire on
	float muzzle_t = 0.0;
	float tracer_t = 0.0;
	float hot_t = 0.0;
	float kick_t = 0.0;
	float stop_t = 0.0;      // the hit-stop
	float last_kill = -9.0;
	int combo = 0;
	float call_t = 0.0;      // the callout's life
	string callout = "";
	int tr_x = 0;
	int tr_y = 0;
	// torches: positions; the light map's steady sum of them, and which one rules each cell
	array<float> tox = [];
	array<float> toy = [];
	array<float> still = [];
	array<int> owner = [];
	array<int> lmap = [];    // 48x48: two cells per map cell, light levels 0..5
	int lres = 48;
	// goo: a pool of particles in world space (z up, 0 = floor, 1 = ceiling)
	array<float> gx = [];
	array<float> gy = [];
	array<float> gz = [];
	array<float> gvx = [];
	array<float> gvy = [];
	array<float> gvz = [];
	array<float> glife = [];
	array<int> gtype = [];
	int ngoo = 220;
	int goo_next = 0;
	array<int> vigx = [];    // the vignette: light levels lost toward the edges
	array<int> vigy = [];
	int rubble_base = 0;     // falling stone, 12 levels
	int breach_x = 22;       // the wall the escape blows open, east at the end of the mid hall
	int breach_y = 12;
	float last_snare = 0.0;

	void build() override {
		name = "GLOBHUNTER";
		handle = "grublin keep . torchlight, one ray per column, zero mercy";
		replay = 26.0;
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
		wmap[breach_y * mw + breach_x] = 5;
		var fogc = [5.0, 7.0, 18.0];
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
						lit(cc[0], cc[1], cc[2], f, side, fogc);
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
					int f3 = f2 < 3 ? 3 : f2;   // the banner's fire glows in the dark
					lit(40.0, 16.0, 60.0, f3, side2, fogc);
					lit(rr * 0.45, rg * 0.45, rb * 0.45, f3, side2, fogc);
					lit(rr, rg, rb, f3, side2, fogc);
					lit(255.0, 196.0, 80.0, f3, side2, fogc);
				}
			}
		}
		var floor_cols = [[46.0, 38.0, 34.0], [92.0, 80.0, 70.0], [104.0, 92.0, 80.0], [116.0, 102.0, 88.0], [128.0, 112.0, 96.0], [140.0, 124.0, 106.0], [76.0, 66.0, 58.0], [160.0, 142.0, 120.0]];
		floor_base = pal.size();
		for (int f4 = 0; f4 < 8; ++f4) {
			for (int c3 = 0; c3 < 8; ++c3) { var fc = floor_cols[c3]; lit(fc[0], fc[1], fc[2], f4, 0, fogc); }
		}
		var ceil_cols = [[20.0, 12.0, 8.0], [60.0, 38.0, 22.0], [72.0, 46.0, 26.0], [84.0, 54.0, 30.0], [96.0, 62.0, 34.0], [50.0, 32.0, 18.0], [110.0, 72.0, 40.0], [36.0, 24.0, 14.0]];
		ceil_base = pal.size();
		for (int f5 = 0; f5 < 8; ++f5) {
			for (int c4 = 0; c4 < 8; ++c4) { var kc = ceil_cols[c4]; lit(kc[0] * 0.8, kc[1] * 0.8, kc[2] * 0.8, f5, 0, fogc); }
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
		pal.add(255, 60, 40);                                     // +48 glowing eye
		pal.ramp(4, 120.0, 255.0, 120.0, 230.0, 255.0, 200.0);    // +49..52 callout ink, grublin goo glint
		pal.ramp(4, 255.0, 120.0, 220.0, 255.0, 240.0, 255.0);    // +53..56 combo ink
		rubble_base = pal.size();
		pal.ramp(12, 22.0, 20.0, 28.0, 196.0, 186.0, 172.0);
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
		// (three more hold the bottom hall for the frenzy, three wait in the mid hall for the escape)
		ex = [2.5, 2.5, 2.5, 4.5, 5.5, 12.5, 12.5, 14.5, 18.5, 21.5, 20.5, 9.0, 11.0, 12.5, 11.5, 16.5, 19.5, 0.0];
		ey = [6.5, 12.5, 16.5, 19.5, 21.5, 18.5, 13.5, 12.5, 12.5, 8.5, 2.5, 21.5, 21.3, 19.0, 11.5, 12.5, 12.3, 0.0];
		et = [0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1];
		for (int i2 = 0; i2 < ne; ++i2) {
			es.push(0);
			est.push(0.0);
			stx.push(0.0);
			sty.push(0.0);
			drawn.push(0);
		}
		es[ne - 1] = 3;   // the boss waits for bar seven
		for (int a = ne - 4; a < ne - 1; ++a) {
			// the ambush rises (a downed mark stands again after 16 s) as the escape begins
			es[a] = 3;
			est[a] = 16.0 - MUSIC_BAR * 10.0;
		}
		zcol = gfx.make_fgrid(w);
		for (int i3 = 0; i3 < w; ++i3) { zcol[i3] = 99.0; }
		// torches hang beside the walls along the route; each keeps its falloff over the light map
		var spots = [[1.2, 2.5], [6.8, 1.6], [2.2, 8.0], [2.8, 14.5], [1.2, 19.5], [9.5, 20.2], [12.2, 17.5],
			[10.2, 12.5], [15.8, 13.5], [21.8, 7.5], [22.8, 2.5], [17.2, 1.4]];
		for (int k = 0; k < spots.size(); ++k) {
			var sp = spots[k];
			float px = sp[0];
			float py = sp[1];
			if (wmap[itrunc(py) * mw + itrunc(px)] != 0) { continue; }
			tox.push(px);
			toy.push(py);
		}
		array<float> best = [];
		for (int c5 = 0; c5 < lres * lres; ++c5) {
			still.push(0.0);
			owner.push(0);
			best.push(0.0);
		}
		for (int k3 = 0; k3 < tox.size(); ++k3) {
			for (int c6 = 0; c6 < lres * lres; ++c6) {
				float lx = (c6 % lres + 0.5) * 0.5 - tox[k3];
				float ly = (c6 / lres + 0.5) * 0.5 - toy[k3];
				float f = 1.0 / (1.0 + (lx * lx + ly * ly) * 1.7);
				still[c6] = still[c6] + f * 5.6 * 0.82;
				if (f > best[c6]) {
					best[c6] = f;
					owner[c6] = k3;
				}
			}
		}
		lmap = gfx.make_grid(lres * lres);
		for (int g = 0; g < ngoo; ++g) {
			gx.push(0.0);
			gy.push(0.0);
			gz.push(0.0);
			gvx.push(0.0);
			gvy.push(0.0);
			gvz.push(0.0);
			glife.push(0.0);
			gtype.push(0);
		}
		vigx = gfx.make_grid(w);
		vigy = gfx.make_grid(h);
		for (int vx = 0; vx < w; ++vx) {
			float e = 2.0 * vx / (w - 1.0) - 1.0;
			vigx[vx] = itrunc(e * e * e * e * 2.4);
		}
		for (int vy = 0; vy < h; ++vy) {
			float e2 = 2.0 * vy / (h - 1.0) - 1.0;
			vigy[vy] = itrunc(e2 * e2 * e2 * e2 * 1.6);
		}
	}

	// one entry: a colour at light level f (0 black gloom .. 7 full torchlight):
	// the dark sinks into a cold blue, the lit end warms toward the fire
	void lit(float r, float g, float b, int f, int side, var fogc) {
		float k = (0.07 + 0.93 * f / 7.0) * (side == 1 ? 0.72 : 1.0);
		float warm = f >= 5 ? (f - 4) / 3.0 : 0.0;
		float fr2 = fogc[0]; float fg = fogc[1]; float fb = fogc[2];
		float cr = r * (1.0 + 0.32 * warm);
		float cg = g * (1.0 + 0.06 * warm);
		float cb = b * (1.0 - 0.28 * warm);
		pal.add(cr * k + fr2 * (1.0 - k), cg * k + fg * (1.0 - k), cb * k + fb * (1.0 - k));
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

	int length_bars() override { return 13; }
	array<float> hit_times() override { return [MUSIC_BAR * 3.0, MUSIC_BAR * 5.0, MUSIC_BAR * 7.0, MUSIC_BAR * 9.0, MUSIC_BAR * 10.0, MUSIC_BAR * 12.0]; }
	array<int> hit_levels() override { return [1, 2, 1, 2, 1, 2]; }

	// the route's distance at which the escape reaches the breach, facing it
	float breach_stop() { return 19.0 + 10.0 + 9.0 + 8.3; }

	void update(float t, float dt) override {
		tt = t;
		++fr;
		bool aftermath = t > MUSIC_BAR * 9.0 && t < MUSIC_BAR * 10.0;
		bool escape = t >= MUSIC_BAR * 10.0 && t < MUSIC_BAR * 12.0;
		bool out = t >= MUSIC_BAR * 12.0;
		bool fight = t > MUSIC_BAR * 7.0 && t < MUSIC_BAR * 9.0;
		float bullet = slow_mo(t);
		float lap = travel - math::floor(travel / path_len) * path_len;
		float pace = stop_t > 0.0 ? 0.18 : (aftermath ? 0.4 : (fight ? 0.16 : 1.0));
		if (escape) {
			// the run for the breach: always just fast enough to reach it on bar twelve
			float want = (breach_stop() - lap) / (MUSIC_BAR * 12.0 - t + 0.05) / 2.4;
			want = want < 0.0 ? 0.0 : (want > 3.0 ? 3.0 : want);
			pace = stop_t > 0.0 ? want * 0.5 : want;
		}
		if (out) { pace = 0.0; }
		pace *= bullet;
		travel += dt * 2.4 * pace;
		lap = travel - math::floor(travel / path_len) * path_len;
		float look = 1.7;
		if (escape || out) {
			float to_corner = 19.0 + 10.0 + 9.0 + 9.0 - lap;
			look = to_corner < look && to_corner > 0.0 ? to_corner : look;
		}
		var p = path_at(travel);
		var q = path_at(travel + look);   // look-ahead steers the turns
		posx = p[0];
		posy = p[1];
		if (out) {
			// into the light: the last bar walks through the breach
			float walk = (t - MUSIC_BAR * 12.0) / MUSIC_BAR;
			walk = walk > 1.0 ? 1.0 : walk;
			posx += walk * (2.0 - walk) * 0.92;
		}
		float bx = q[0] - posx;
		float by = q[1] - posy;
		float bl = math::sqrt(bx * bx + by * by);
		if (out) { bx = 1.0; by = 0.0; bl = 1.0; }
		if (bl > 0.0001) { bx /= bl; by /= bl; } else { bx = dirx; by = diry; }
		// the hunt: swing toward the nearest live mark in clear sight ahead
		float want = 0.0;
		float near = 5.5;
		for (int m = 0; m < ne; ++m) {
			if (es[m] != 0) { continue; }
			float mx = ex[m] - posx;
			float my = ey[m] - posy;
			float md = math::sqrt(mx * mx + my * my);
			if (md > near || md < 0.6) { continue; }
			float ang = math::atan2(bx * my - by * mx, bx * mx + by * my);
			if (ang > 0.75 || ang < -0.75) { continue; }
			if (!los_clear(mx, my, 0.95)) { continue; }
			near = md;
			want = ang;
		}
		if (out) { want = 0.0; }
		float ease = dt * 7.0;
		ease = ease > 1.0 ? 1.0 : ease;
		aim += (want - aim) * ease;
		// the quake: each kill shakes the view, the big hit hardest, and the keep rumbles as it falls
		float kick_now = cue != null ? cue.kick_env(t) : 0.0;
		float quake = (hot_t > 0.0 ? hot_t * 0.16 : 0.0) + (cue != null ? cue.hit(t) * 0.05 : 0.0);
		if (escape) { quake += 0.012 + kick_now * 0.03; }
		float view = aim + quake * math::sin(fr * 2.7);
		float ca = math::cos(view);
		float sa = math::sin(view);
		dirx = bx * ca - by * sa;
		diry = bx * sa + by * ca;
		planex = 0.0 - diry * 0.66;
		planey = dirx * 0.66;
		// timers and the enemy state machine
		cool -= dt;
		muzzle_t -= dt;
		tracer_t -= dt;
		hot_t -= dt;
		kick_t -= dt;
		stop_t -= dt;
		call_t -= dt;
		boss_flash -= dt;
		for (int i = 0; i < ne; ++i) {
			int st = es[i];
			if (st == 1) { est[i] = est[i] + dt; if (est[i] > 0.14) { es[i] = 2; est[i] = 0.0; } }
			else if (st == 2) { est[i] = est[i] + dt; if (est[i] > 0.55) { es[i] = 3; est[i] = 0.0; } }
			else if (st == 3) { est[i] = est[i] + dt; if (est[i] > 16.0) { es[i] = 0; est[i] = 0.0; } }
		}
		// auto-fire on the beat: nearest alive enemy in the crosshair cone with clear LOS
		float e = cue != null ? cue.energy(t) : 0.6;
		bool eighths = e > 0.92 || (t > MUSIC_BAR * 5.0 && t < MUSIC_BAR * 6.0) || escape;
		// the big hit: the hall floods with light and the frenzy is called
		if (t >= MUSIC_BAR * 5.0 && t - dt < MUSIC_BAR * 5.0) {
			callout = "FRENZY";
			call_t = 1.2;
		}
		int step = math::ifloor(t / (eighths ? MUSIC_BEAT * 0.5 : MUSIC_BEAT));
		bool on_step = step != last_step;
		last_step = step;
		if (t >= MUSIC_BAR * 10.0 && t - dt < MUSIC_BAR * 10.0) {
			callout = "RUN";
			call_t = 1.3;
			hot_t = 0.5;
		}
		if (t >= MUSIC_BAR * 12.0 && t - dt < MUSIC_BAR * 12.0) { blow_breach(); }
		if (escape) { falling_stone(t); }
		if (cool <= 0.0 && on_step && !aftermath && !out && t > MUSIC_BAR) {
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
				if (tyy < 1.0 || tyy > 4.6) { continue; }
				float off = txx < 0.0 ? -txx : txx;
				if (off > tyy * 0.12) { continue; }
				if (!los_clear(rx, ry, 0.95)) { continue; }
				if (tyy < besty) { best = i2; besty = tyy; bestx = txx; }
			}
			if (best >= 0) { fire(best, bestx, besty, t); }
		}
		// the boss rises in the corridor ahead as the last bar begins
		int boss = ne - 1;
		if (t >= MUSIC_BAR * 7.0 && t - dt < MUSIC_BAR * 7.0) {
			var ahead = path_at(travel + 4.4);
			ex[boss] = ahead[0];
			ey[boss] = ahead[1];
			es[boss] = 0;
			est[boss] = 0.0;
			callout = "BOSS GLOB";
			call_t = 1.6;
			// the garrison scatters before it
			for (int m2 = 0; m2 < boss; ++m2) {
				float ddx2 = ex[m2] - posx;
				float ddy2 = ey[m2] - posy;
				if (ddx2 * ddx2 + ddy2 * ddy2 < 64.0) {
					es[m2] = 3;
					est[m2] = 0.0;
				}
			}
		}
		// bar nine's downbeat: the boss bursts
		if (t >= MUSIC_BAR * 9.0 && t - dt < MUSIC_BAR * 9.0 && es[boss] == 0) {
			float invdet2 = 1.0 / (planex * diry - dirx * planey);
			float rbx = ex[boss] - posx;
			float rby = ey[boss] - posy;
			float bsx = invdet2 * (diry * rbx - dirx * rby);
			float bsy = invdet2 * (planex * rby - planey * rbx);
			fire(boss, bsx, bsy > 0.3 ? bsy : 0.3, t);
		}
		move_goo(dt * bullet);
		// the flicker is redrawn at 15 Hz, and every frame while the muzzle burns or the kick lands
		float beat_light = cue != null ? cue.kick_env(t) + cue.hit(t) : 0.0;
		if ((fr & 1) == 0 || muzzle_t > -0.05 || beat_light > 0.2 || escape || out) { light_up(); }
		// the view: a footfall on each beat (each eighth on the run), the recoil's kick, a shudder
		// while the muzzle burns, and the falling keep's tremor
		float stride = cue != null ? cue.beat_phase(t) : travel * 0.83;
		if (escape) { stride = stride * 2.0 - math::floor(stride * 2.0); }
		float bob = math::sin(stride * 3.1415927) * h * (escape ? 0.022 : 0.014) * (aftermath || out ? 0.4 : 1.0);
		float kick = kick_t > 0.0 ? h * 0.05 * kick_t / 0.2 : 0.0;
		float shud = muzzle_t > 0.0 ? ((fr * 7) % 3 - 1) * h * 0.012 : 0.0;
		float tremor = escape ? math::sin(fr * 5.3) * h * (0.006 + kick_now * 0.02) : 0.0;
		pitch = bob + kick + shud + tremor;
	}

	// the escape: on each snare stones shake loose from the vault ahead and fall
	void falling_stone(float t) {
		float sn = cue != null ? cue.snare_env(t) : 0.0;
		bool onset = sn > last_snare + 0.25;
		last_snare = sn;
		if (!onset) { return; }
		for (int k = 0; k < 14; ++k) {
			int g = goo_next;
			goo_next = (goo_next + 1) % ngoo;
			float ahead = 1.2 + rng.nextf() * 3.2;
			float across = (rng.nextf() - 0.5) * 1.6;
			float sx = posx + dirx * ahead - diry * across;
			float sy = posy + diry * ahead + dirx * across;
			if (wmap[itrunc(sy) * mw + itrunc(sx)] != 0) { continue; }
			gx[g] = sx;
			gy[g] = sy;
			gz[g] = 0.95;
			gvx[g] = (rng.nextf() - 0.5) * 0.6;
			gvy[g] = (rng.nextf() - 0.5) * 0.6;
			gvz[g] = 0.0 - rng.nextf() * 1.5;
			glife[g] = 1.4 + rng.nextf() * 0.8;
			gtype[g] = 2;
		}
	}

	// bar twelve: the east wall bursts, stone flies at the camera and daylight floods in
	void blow_breach() {
		callout = "KEEP CLEARED";
		call_t = 2.6;
		hot_t = 0.8;
		for (int k = 0; k < 70; ++k) {
			int g = goo_next;
			goo_next = (goo_next + 1) % ngoo;
			gx[g] = breach_x - 0.05;
			gy[g] = breach_y + 0.1 + rng.nextf() * 0.8;
			gz[g] = 0.1 + rng.nextf() * 0.8;
			gvx[g] = 0.0 - (1.5 + rng.nextf() * 4.5);
			gvy[g] = (rng.nextf() - 0.5) * 2.4;
			gvz[g] = 0.5 + rng.nextf() * 2.5;
			glife[g] = 1.8 + rng.nextf() * 1.2;
			gtype[g] = 2;
		}
	}

	// 0 a wall, rising to 1 as the cracks of the breach open through the escape, 2 blown open
	float breach_state() {
		if (tt >= MUSIC_BAR * 12.0) { return 2.0; }
		float k = (tt - MUSIC_BAR * 10.0) / (MUSIC_BAR * 2.0);
		return k < 0.0 ? 0.0 : k;
	}

	// bullet time: two beats after each big hit play at a fifth of the speed
	float slow_mo(float t) {
		float since = t - MUSIC_BAR * 5.0;
		if (t >= MUSIC_BAR * 9.0) { since = t - MUSIC_BAR * 9.0; }
		if (t >= MUSIC_BAR * 12.0) { since = t - MUSIC_BAR * 12.0; }
		if (since < 0.0 || since > MUSIC_BEAT * 2.5) { return 1.0; }
		float back = (since - MUSIC_BEAT * 2.0) / (MUSIC_BEAT * 0.5);
		back = back < 0.0 ? 0.0 : (back > 1.0 ? 1.0 : back);
		return 0.2 + 0.8 * back;
	}

	void fire(int i, float bx, float by, float t) {
		bool boss = i == ne - 1;
		bool boss_down = boss && t >= MUSIC_BAR * 9.0 - 0.01;
		if (boss && !boss_down) {
			// a hit the boss takes: the muzzle, the tracer and a flash, but it stands
			boss_flash = 0.1;
			cool = 0.2;
			muzzle_t = 0.12;
			tracer_t = 0.14;
			kick_t = 0.2;
			tr_x = itrunc(w * 0.5 * (1.0 + bx / by));
			tr_y = h / 2 + itrunc(h * 0.10 / by);
			callout = "";
			for (int k0 = 0; k0 < 8; ++k0) {
				int g0 = goo_next;
				goo_next = (goo_next + 1) % ngoo;
				float a0 = rng.nextf() * 6.2831853;
				gx[g0] = ex[i];
				gy[g0] = ey[i];
				gz[g0] = 0.5 + rng.nextf() * 0.4;
				gvx[g0] = math::cos(a0) * 1.2 - dirx * 0.8;
				gvy[g0] = math::sin(a0) * 1.2 - diry * 0.8;
				gvz[g0] = 1.0 + rng.nextf() * 2.0;
				glife[g0] = 1.6;
				gtype[g0] = 1;
			}
			return;
		}
		es[i] = 1;
		est[i] = 0.0;
		++kills;
		cool = 0.2;
		muzzle_t = 0.12;
		tracer_t = 0.14;
		hot_t = 0.35;
		kick_t = 0.2;
		stop_t = 0.16;
		tr_x = itrunc(w * 0.5 * (1.0 + bx / by));
		tr_y = h / 2 + itrunc(h * 0.10 / by);
		combo = t - last_kill < 1.8 ? combo + 1 : 1;
		last_kill = t;
		var words = ["SPLAT", "GLOBBED", "SQUISH", "POP"];
		callout = boss_down ? "BOSS DOWN" : (combo >= 3 ? "MULTI KILL" : (combo == 2 ? "DOUBLE KILL" : words[kills % 4]));
		call_t = boss_down ? 2.2 : 0.9;
		if (boss_down) { hot_t = 0.8; }
		// the burst: goo flung out of the body, mostly up and back from the shot
		for (int k = 0; k < (boss_down ? 120 : 34); ++k) {
			int g = goo_next;
			goo_next = (goo_next + 1) % ngoo;
			float a = rng.nextf() * 6.2831853;
			float sp = 0.8 + rng.nextf() * 2.2;
			gx[g] = ex[i];
			gy[g] = ey[i];
			gz[g] = 0.35 + rng.nextf() * 0.3;
			gvx[g] = math::cos(a) * sp + dirx * 1.2;
			gvy[g] = math::sin(a) * sp + diry * 1.2;
			gvz[g] = 1.2 + rng.nextf() * 2.6;
			glife[g] = 2.2 + rng.nextf() * 1.5;
			gtype[g] = et[i];
		}
	}

	// goo falls, bounces off the flagstones and the walls, then lies there fading
	void move_goo(float dt) {
		for (int g = 0; g < ngoo; ++g) {
			float life = glife[g];
			if (life <= 0.0) { continue; }
			glife[g] = life - dt;
			float vz = gvz[g] - 7.5 * dt;
			float nz = gz[g] + vz * dt;
			if (nz < 0.0) {
				nz = 0.0;
				vz = vz < -0.6 ? 0.0 - vz * 0.3 : 0.0;
				gvx[g] = gvx[g] * 0.55;
				gvy[g] = gvy[g] * 0.55;
			}
			gz[g] = nz;
			gvz[g] = vz;
			float nx = gx[g] + gvx[g] * dt;
			float ny = gy[g] + gvy[g] * dt;
			if (wmap[itrunc(gy[g]) * mw + itrunc(nx)] != 0) { gvx[g] = 0.0 - gvx[g] * 0.4; nx = gx[g]; }
			if (wmap[itrunc(ny) * mw + itrunc(gx[g])] != 0) { gvy[g] = 0.0 - gvy[g] * 0.4; ny = gy[g]; }
			gx[g] = nx;
			gy[g] = ny;
		}
	}

	// the light map: the torches' steady sum, each cell flickering with the torch
	// that rules it, plus the muzzle near the camera while it burns
	void light_up() {
		int n = lres * lres;
		int nt = tox.size();
		array<float> flick = [];
		float kick = cue != null ? cue.kick_env(tt) : 0.0;
		// on the run the torches gutter low and the kick strobes them back up
		bool run = tt >= MUSIC_BAR * 10.0 && tt < MUSIC_BAR * 12.0;
		float gutter = run ? 0.4 + kick * 1.1 : 1.0;
		for (int k = 0; k < nt; ++k) {
			float beat_flare = cue != null ? 0.45 * kick + 0.7 * cue.hit(tt) : 0.0;
			flick.push((1.0 + 0.12 * math::sin(tt * 13.0 + k * 2.3) + 0.10 * math::sin(tt * 29.0 + k * 5.1)) * gutter + beat_flare);
		}
		var& st = still;
		var& ow = owner;
		var& lm = lmap;
		int flood = cue != null ? itrunc(cue.hit(tt) * 4.0) : 0;
		for (int c = 0; c < n; ++c) {
			int lv = itrunc(st[c] * flick[ow[c]]) + flood;
			lm[c] = lv > 5 ? 5 : lv;
		}
		// every burst lights the hall around it while the goo flies
		for (int b = 0; b < ne; ++b) {
			int sb = es[b];
			if (sb != 1 && sb != 2) { continue; }
			float age = sb == 1 ? est[b] : 0.14 + est[b];
			float power = 5.0 * (1.0 - age / 0.7);
			if (power <= 0.0) { continue; }
			int bx0 = itrunc(ex[b] * 2.0);
			int by0 = itrunc(ey[b] * 2.0);
			for (int dy2 = -9; dy2 <= 9; ++dy2) {
				int cy2 = by0 + dy2;
				if (cy2 < 0 || cy2 >= lres) { continue; }
				for (int dx2 = -9; dx2 <= 9; ++dx2) {
					int cx2 = bx0 + dx2;
					if (cx2 < 0 || cx2 >= lres) { continue; }
					float dd = (dx2 * dx2 + dy2 * dy2) * 0.25;
					int c3 = cy2 * lres + cx2;
					int lv3 = lm[c3] + itrunc(power / (1.0 + dd * 0.5));
					lm[c3] = lv3 > 5 ? 5 : lv3;
				}
			}
		}
		// daylight through the breach: a glow through its cracks, then a flood once it is open
		float bs = breach_state();
		if (bs > 0.0) {
			float power = bs >= 2.0 ? 6.0 : bs * (2.0 + kick * 2.0);
			int bx1 = breach_x * 2;
			int by1 = breach_y * 2 + 1;
			for (int dy3 = -12; dy3 <= 12; ++dy3) {
				int cy3 = by1 + dy3;
				if (cy3 < 0 || cy3 >= lres) { continue; }
				for (int dx3 = -24; dx3 <= 0; ++dx3) {
					int cx3 = bx1 + dx3;
					if (cx3 < 0) { continue; }
					float dd3 = (dx3 * dx3 + dy3 * dy3 * 2.0) * 0.25;
					int c4 = cy3 * lres + cx3;
					int lv4 = lm[c4] + itrunc(power / (1.0 + dd3 * 0.12));
					lm[c4] = lv4 > 5 ? 5 : lv4;
				}
			}
		}
		if (muzzle_t > 0.0) {
			int cx0 = itrunc(posx * 2.0);
			int cy0 = itrunc(posy * 2.0);
			for (int dy = -10; dy <= 10; ++dy) {
				int cy = cy0 + dy;
				if (cy < 0 || cy >= lres) { continue; }
				for (int dx = -10; dx <= 10; ++dx) {
					int cx = cx0 + dx;
					if (cx < 0 || cx >= lres) { continue; }
					float d2 = (dx * dx + dy * dy) * 0.25;
					int c2 = cy * lres + cx;
					int lv2 = itrunc(st[c2] * flick[ow[c2]] + 4.5 / (1.0 + d2 * 0.35));
					lm[c2] = lv2 > 5 ? 5 : lv2;
				}
			}
		}
	}

	int light_at(float x, float y) {
		int cx = itrunc(x * 2.0);
		int cy = itrunc(y * 2.0);
		cx = cx < 0 ? 0 : (cx >= lres ? lres - 1 : cx);
		cy = cy < 0 ? 0 : (cy >= lres ? lres - 1 : cy);
		return lmap[cy * lres + cx];
	}

	// floor and ceiling, cast per pixel: each row off the horizon is a fixed
	// distance, stepped across the screen along the camera plane and lit from the
	// map; a floor row and the ceiling row as far above the horizon share one walk
	void cast_planes(var& grid) {
		int ww = w;
		int hh = h;
		int h0 = hh / 2 + itrunc(pitch);
		var& tx = tex;
		var& lm = lmap;
		var& vx = vigx;
		var& vy = vigy;
		int lr = lres;
		int fb = floor_base;
		int cb = ceil_base;
		float rdx0 = dirx - planex;
		float rdy0 = diry - planey;
		float rdx1 = dirx + planex;
		float rdy1 = diry + planey;
		int reach = h0 > hh - h0 ? h0 : hh - h0;
		for (int k = 0; k < reach; ++k) {
			int yf = h0 + k;
			int yc = h0 - 1 - k;
			bool dof = yf < hh;
			bool doc = yc >= 0;
			float rowd = hh * 0.5 / (k + 0.5);
			float stx_ = rowd * (rdx1 - rdx0) / ww;
			float sty_ = rowd * (rdy1 - rdy0) / ww;
			float fx = posx + rowd * rdx0;
			float fy = posy + rowd * rdy0;
			int amb = itrunc(8.0 / (rowd + 1.8));
			amb = amb > 3 ? 3 : amb;
			int ambf = amb - (dof ? vy[yf] : 0);
			int ambc = amb - (doc ? vy[yc] : 0);
			int gf = yf * ww;
			int gc = yc * ww;
			int cell = -1;
			int lit = 0;
			for (int x = 0; x < ww; ++x) {
				int iu = fx * 16.0;
				int iv = fy * 16.0;
				int ti = (iv & 15) * 16 + (iu & 15);
				// the light changes only from one light-map cell to the next
				int lc = (iv >> 3) * 4096 + (iu >> 3);
				if (lc != cell) {
					cell = lc;
					int lcx = iu >> 3;
					int lcy = iv >> 3;
					lcx = lcx < 0 ? 0 : (lcx >= lr ? lr - 1 : lcx);
					lcy = lcy < 0 ? 0 : (lcy >= lr ? lr - 1 : lcy);
					lit = lm[lcy * lr + lcx];
				}
				int vxx = vx[x];
				if (dof) {
					int lv = ambf + lit - vxx;
					lv = lv < 0 ? 0 : (lv > 7 ? 7 : lv);
					grid[gf + x] = fb + lv * 8 + tx[768 + ti];
				}
				if (doc) {
					int lv2 = ambc + lit - vxx;
					lv2 = lv2 < 0 ? 0 : (lv2 > 7 ? 7 : lv2);
					grid[gc + x] = cb + lv2 * 8 + tx[1024 + ti];
				}
				fx += stx_;
				fy += sty_;
			}
		}
	}

	// one billboard: an ellipsoid body lit as a sphere per pixel, eyes that glow
	// in the gloom, a grin; walls occlude per column; hit-flash paints it white
	void draw_blob(var& grid, int i) {
		float tyy = sty[i];
		float txx = stx[i];
		int scx = itrunc(w * 0.5 * (1.0 + txx / tyy));
		int typ = et[i];
		bool boss = i == ne - 1;
		float size = boss ? 1.7 + (cue != null ? cue.kick_env(tt) * 0.22 : 0.0) : (typ == 1 ? 1.1 : 0.78);
		int sh = itrunc(h * size / tyy);
		sh = sh < 3 ? 3 : sh;
		sh = sh > h ? h : sh;
		float hwm = (sh + (typ == 1 ? 6.0 : 2.0) * h / 80.0) * 0.5;
		int feet = h / 2 + itrunc(pitch) + itrunc(h * 0.5 / tyy);
		int top = feet - sh;
		float wob = math::sin(tt * 5.0 + i * 2.1) * 0.10;
		bool flash = es[i] == 1 || (boss && boss_flash > 0.0);
		int body = sprite_base + typ * 12;
		float fogk = 0.18 + light_at(ex[i], ey[i]) * 0.15 + 0.35 / (tyy + 0.6);
		fogk = fogk < 0.2 ? 0.2 : (fogk > 1.0 ? 1.0 : fogk);
		bool glow = fogk < 0.55 || boss;
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
					// a rim of torchlight on the silhouette's edge
					float rim = q < 0.12 && li < 0.3 ? 0.35 : 0.0;
					int lv = itrunc((0.10 + 0.9 * li + rim) * fogk * 11.99);
					pi = body + (lv > 11 ? 11 : lv);
					float ax = nx < 0.0 ? -nx : nx;
					if (v > -0.5 && v < -0.12 && ax > 0.14 && ax < 0.52) {
						float ey2 = (v + 0.31) * 5.0;
						float ex2 = (ax - 0.33) * 5.0;
						if (ex2 * ex2 + ey2 * ey2 < 1.0) {
							bool core = ex2 * ex2 + ey2 * ey2 < 0.3;
							pi = core ? (glow ? sprite_base + 48 : sprite_base + 26) : (glow ? sprite_base + 26 : sprite_base + 25);
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

	// a torch's flame: a flickering teardrop, occluded by nearer walls
	void draw_flame(var& grid, int k) {
		float invdet = 1.0 / (planex * diry - dirx * planey);
		float rx = tox[k] - posx;
		float ry = toy[k] - posy;
		float tyy = invdet * (planex * ry - planey * rx);
		if (tyy < 0.3) { return; }
		float txx = invdet * (diry * rx - dirx * ry);
		int scx = itrunc(w * 0.5 * (1.0 + txx / tyy));
		float hc = h / 2 + itrunc(pitch);
		int fh = itrunc(h * 0.24 / tyy);
		fh = fh < 3 ? 3 : fh;
		int fy0 = itrunc(hc - h * 0.14 / tyy) - fh;
		float flick = math::sin(tt * 17.0 + k * 3.0);
		for (int dy = 0; dy < fh; ++dy) {
			float v = (dy + 0.5) / fh;
			float half = (v < 0.65 ? v / 0.65 : 1.0 - (v - 0.65) / 0.35 * 0.4) * fh * 0.28;
			int sway = itrunc(flick * (1.0 - v) * fh * 0.12);
			int hw = itrunc(half);
			int row = fy0 + dy;
			if (row < 0 || row >= h) { continue; }
			for (int dx = 0 - hw; dx <= hw; ++dx) {
				int col = scx + dx + sway;
				if (col < 0 || col >= w || tyy >= zcol[col]) { continue; }
				float ax = dx < 0 ? 0 - dx : dx;
				float e = hw > 0 ? ax / hw : 0.0;
				int c = e < 0.4 && v > 0.45 ? 31 : (e < 0.75 ? 32 : 33);
				grid[row * w + col] = sprite_base + c;
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
		var& vx = vigx;
		int wb = wall_base;
		int bb = banner_base;
		int tshift = itrunc(tt * 14.0);
		float hc = hh / 2 + itrunc(pitch);
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
			float topf = hc - lineh * 0.5;
			int y0 = itrunc(topf);
			int y1 = itrunc(topf + lineh);
			y0 = y0 < 0 ? 0 : y0;
			y1 = y1 > hh ? hh : y1;
			// light: the gloom by distance, the torches at the face, the vignette
			int amb = itrunc(8.0 / (pdist + 1.8));
			amb = amb > 3 ? 3 : amb;
			int lv = amb + light_at(posx + rdx * (pdist - 0.05), posy + rdy * (pdist - 0.05)) - vx[x];
			lv = lv < 0 ? 0 : (lv > 7 ? 7 : lv);
			float tstep = 16.0 / lineh;
			float tpos = (y0 + 0.5 - topf) * tstep;
			int gi = y0 * ww + x;
			if (hitv == 5) {
				breach_slice(grid, x, y0, y1, tpos, tstep, tu, lv, side);
			} else if (hitv == 4) {
				int brow = bb + (side * 8 + lv) * 16;
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
				int wrow = wb + (((hitv - 1) * 2 + side) * 8 + lv) * 8;
				int tbase = (hitv - 1) * 256 + tu;
				for (int y2 = y0; y2 < y1; ++y2) {
					int tv2 = tpos;
					grid[gi] = wrow + tx[tbase + (tv2 & 15) * 16];
					tpos += tstep;
					gi += ww;
				}
			}
		}
		for (int k = 0; k < tox.size(); ++k) { draw_flame(grid, k); }
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
		draw_goo(grid, invdet);
		daylight(grid);
		draw_hud(grid);
	}

	// out through the breach: daylight blooms from the heart of the frame, its edge dithered gold
	void daylight(var& grid) {
		if (tt < MUSIC_BAR * 12.0) { return; }
		float k = (tt - MUSIC_BAR * 12.0) / MUSIC_BAR;
		k = k > 1.0 ? 1.0 : k;
		// the blast whites out the frame for a moment, then the opening blooms
		float r = k < 0.06 ? w * 1.2 : (0.12 + k * k * 0.6) * w;
		float band = 7.0;
		float cx = w * 0.5;
		float cy = h * 0.5 + pitch;
		int sb = sprite_base;
		for (int y = 0; y < h; ++y) {
			float dy = (y + 0.5 - cy) * 1.5;
			float oo = r * r - dy * dy;
			if (oo <= 0.0) { continue; }
			// per row: the white heart, the warm white ring, the dithered gold edge
			float xo = math::sqrt(oo);
			float ri = r - band;
			float ii = ri * ri - dy * dy;
			float xi = ii > 0.0 ? math::sqrt(ii) : 0.0;
			float hh = r * r * 0.36 - dy * dy;
			float xh = hh > 0.0 ? math::sqrt(hh) : 0.0;
			int row = y * w;
			int x0 = itrunc(cx - xo);
			int x1 = itrunc(cx + xo) + 1;
			x0 = x0 < 0 ? 0 : x0;
			x1 = x1 > w ? w : x1;
			for (int x = x0; x < x1; ++x) {
				float dx = x + 0.5 - cx;
				float ax = dx < 0.0 ? -dx : dx;
				if (ax < xh) {
					grid[row + x] = sb + 24;
				} else if (ax < xi) {
					grid[row + x] = sb + 31;
				} else {
					float over = (r - math::sqrt(dx * dx + dy * dy)) / band;
					if (over * 16.0 > BAYER4[(y & 3) * 4 + (x & 3)]) { grid[row + x] = sb + 32; }
				}
			}
		}
	}

	// the breach: stone whose cracks glow hotter through the escape, then open daylight
	void breach_slice(var& grid, int x, int y0, int y1, float tpos, float tstep, int tu, int lv, int side) {
		float bs = breach_state();
		int sb = sprite_base;
		int gi = y0 * w + x;
		if (bs >= 2.0) {
			// open: white at the heart of the opening, gold toward its edges, rays rippling up it
			int shimmer = itrunc(tt * 9.0);
			for (int y = y0; y < y1; ++y) {
				int tv = tpos;
				int ev = tv < 8 ? tv : 15 - tv;
				int eu = tu < 8 ? tu : 15 - tu;
				int edge = ev < eu ? ev : eu;
				int ray = ((tu * 5 + (tv >> 1) + shimmer) & 7) == 0 ? 1 : 0;
				grid[gi] = edge > 3 ? sb + 24 : (edge > 1 - ray ? sb + 31 : sb + 32);
				tpos += tstep;
				gi += w;
			}
			return;
		}
		float kick = cue != null ? cue.kick_env(tt) : 0.0;
		int heat = itrunc((bs + kick * 0.35) * 14.0);
		int wrow = wall_base + (side * 8 + lv) * 8;
		for (int y2 = y0; y2 < y1; ++y2) {
			int tv2 = tpos;
			int ti = (tv2 & 15) * 16 + tu;
			int crack = (hash2(ti, 77) & 15);
			int c = tex[ti];
			// mortar lines and a hashed web of cracks catch fire from the light behind
			int pix = wrow + c;
			if ((c == 0 && crack < heat) || crack + 6 < heat - 8) { pix = crack < heat - 6 ? sb + 31 : (crack < heat - 3 ? sb + 32 : sb + 33); }
			grid[gi] = pix;
			tpos += tstep;
			gi += w;
		}
	}

	// the goo in flight and on the floor: blobs sized by distance, lit by the map
	void draw_goo(var& grid, float invdet) {
		float hc = h / 2 + itrunc(pitch);
		for (int g = 0; g < ngoo; ++g) {
			float life = glife[g];
			if (life <= 0.0) { continue; }
			float rx = gx[g] - posx;
			float ry = gy[g] - posy;
			float tyy = invdet * (planex * ry - planey * rx);
			if (tyy < 0.3) { continue; }
			float txx = invdet * (diry * rx - dirx * ry);
			int scx = itrunc(w * 0.5 * (1.0 + txx / tyy));
			if (scx < 0 || scx >= w || tyy >= zcol[scx]) { continue; }
			int scy = itrunc(hc + (0.5 - gz[g]) * h / tyy);
			int rad = itrunc(h * (gtype[g] == 2 ? 0.024 : 0.04) / tyy * (life < 0.6 ? life / 0.6 : 1.0));
			int flat = gz[g] <= 0.0 ? 1 : 0;
			float lk = 0.3 + light_at(gx[g], gy[g]) * 0.14;
			int base = gtype[g] == 2 ? rubble_base : sprite_base + gtype[g] * 12;
			int body = base + itrunc(lk * 10.0);
			body = body > base + 11 ? base + 11 : body;
			for (int dy = 0 - rad; dy <= rad; ++dy) {
				int row = scy + (flat == 1 ? dy / 2 : dy);
				if (row < 0 || row >= h) { continue; }
				for (int dx = 0 - rad - flat * rad; dx <= rad + flat * rad; ++dx) {
					int col = scx + dx;
					if (col < 0 || col >= w || tyy >= zcol[col]) { continue; }
					if (dx * dx + dy * dy * (flat == 1 ? 4 : 1) > (rad + 1) * (rad + 1) * (flat == 1 ? 4 : 1)) { continue; }
					grid[row * w + col] = dy < 0 && dx < 0 && flat == 0 && gtype[g] != 2 ? sprite_base + 52 : body;
				}
			}
		}
	}

	void draw_hud(var& grid) {
		int gx0 = w / 2;
		int gs = h / 40;
		gs = gs < 2 ? 2 : gs;
		int flick = itrunc(tt * 10.0);
		// rainbow hitscan tracer from the muzzle to the target
		if (tracer_t > 0.0) {
			int y0t = h - 11 * gs;
			int steps = 14 * gs;
			for (int k2 = 1; k2 < steps; ++k2) {
				int xx = gx0 + (tr_x - gx0) * k2 / steps;
				int yy = y0t + (tr_y - y0t) * k2 / steps;
				for (int th = 0; th < gs / 2; ++th) {
					if (xx + th < 0 || xx + th >= w || yy < 1 || yy >= h) { continue; }
					grid[yy * w + xx + th] = sprite_base + 34 + (k2 / gs + flick) % 6;
				}
			}
		}
		// the gun, drawn at gs x: bobs with the walk, kicks on the shot
		int bob = itrunc(math::sin(travel * 5.2) * 1.25 * gs);
		int kick = muzzle_t > 0.0 ? 2 * gs : 0;
		int gw = 16;
		int gh = gun.size();
		int gxs = gx0 - gw * gs / 2 + bob;
		int gys = h - gh * gs + 2 * gs + kick + (bob < 0 ? 0 - bob : bob) / 2;
		for (int ry = 0; ry < gh * gs; ++ry) {
			string row = gun[ry / gs];
			int yy2 = gys + ry;
			if (yy2 < 0 || yy2 >= h) { continue; }
			for (int rx = 0; rx < gw * gs; ++rx) {
				string ch = row.substr(rx / gs, 1);
				if (ch == ".") { continue; }
				int c = ch == "d" ? 27 : (ch == "m" ? 28 : (ch == "l" ? 29 : (ch == "h" ? 30 : (ch == "k" ? 46 : 47))));
				int xx2 = gxs + rx;
				if (xx2 >= 0 && xx2 < w) { grid[yy2 * w + xx2] = sprite_base + c; }
			}
		}
		if (muzzle_t > 0.0) {
			int mx = gxs + gw * gs / 2 - 1;
			int my = gys - 2 * gs;
			int ray = 3 * gs;
			for (int q = 0 - ray; q <= ray; ++q) {
				int aq = q < 0 ? 0 - q : q;
				int c2 = aq * 5 <= ray ? 31 : (aq * 5 <= ray * 3 ? 32 : 33);
				for (int th2 = 0; th2 < gs; ++th2) {
					if (my + q >= 0 && my + q < h && mx + th2 < w) { grid[(my + q) * w + mx + th2] = sprite_base + c2; }
					if (mx + q >= 0 && mx + q < w && my + th2 >= 0 && my + th2 < h) { grid[(my + th2) * w + mx + q] = sprite_base + c2; }
				}
				if (aq * 5 <= ray * 3 && mx + q >= 0 && mx + q < w && my + q >= 0 && my + q < h) { grid[(my + q) * w + mx + q] = sprite_base + 32; }
				if (aq * 5 <= ray * 3 && mx - q >= 0 && mx - q < w && my + q >= 0 && my + q < h) { grid[(my + q) * w + mx - q] = sprite_base + 32; }
			}
		}
		int chc = hot_t > 0.0 ? sprite_base + 41 : sprite_base + 40;
		int cy = itrunc(h * 0.5);
		int cr = gs / 2;
		for (int d = cr + 1; d <= cr * 2 + 2; ++d) {
			grid[cy * w + gx0 - d] = chc;
			grid[cy * w + gx0 + d] = chc;
			grid[(cy - d) * w + gx0] = chc;
			grid[(cy + d) * w + gx0] = chc;
		}
		int ts = h / 80;
		ts = ts < 1 ? 1 : ts;
		string tally = `KILLS ${(kills < 10 ? "0" : "")}${kills}`;
		gfx.draw_text(grid, w, h, tally, 2, 2, ts, ts, sprite_base + 42, 2, sprite_base + 44, 99);
		// the callout: it punches in large and settles, rising as it fades
		if (call_t > 0.0 && callout.size() > 0) {
			float age = 0.9 - call_t;
			int cs = w * 3 / 5 / (callout.size() * 6);
			cs = cs < 1 ? 1 : (cs > 3 * ts ? 3 * ts : cs);
			cs += age < 0.08 ? 1 : 0;
			int tw = callout.size() * 6 * cs;
			int cx = (w - tw) / 2;
			int cyy = itrunc(h * 0.22 - age * h * 0.08);
			int ink = combo >= 2 ? sprite_base + 53 + (flick & 3) : sprite_base + 49 + (flick & 1) * 2;
			if (cx > 0) { gfx.draw_text(grid, w, h, callout, cx, cyy, cs, cs, ink, 1, sprite_base + 44, 99); }
		}
	}
}
