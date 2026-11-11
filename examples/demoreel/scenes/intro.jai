// INTRO — the cold open, four bars before the first scene. A dead signal rolls and
// flickers over the riser and a CRT line powers on; on the first downbeat JAISCRIPT
// slams in split into red and cyan ghosts that snap together, a chromatic shockwave
// racing out over a starfield of blue and rose stars that starts to warp on the kick,
// a violet glow breathing behind the letters, and the tagline types itself; the logo
// glitches on the hats, then on the second impact bursts into sparks of its own
// colours that stream into the warp until the screen whites out into EIGHT. It has no
// title card and plays once, when the reel starts.

array<int> INTRO_DITHER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];

class Intro : Scene {
	array<float> star_x = [];
	array<float> star_y = [];
	array<float> star_z = [];
	array<float> spark_x = [];
	array<float> spark_y = [];
	array<float> spark_vx = [];
	array<float> spark_vy = [];
	array<int> spark_ink = [];
	array<int> logo_at = [];
	array<int> glow = [];      // the halo behind the logo, 0..255 a pixel
	array<int> lum = [];       // each palette entry's brightness, 0..15, for the white-out
	int nstars = 260;
	int gray = 0;        // 16 levels black to white
	int star_cold = 0;   // 16 levels black to blue-white
	int star_warm = 0;   // 16 levels black to rose-white
	int neb = 0;         // 8 levels of the violet halo
	int red = 0;         // 8 levels
	int cyan = 0;
	int hot = 0;         // 8 levels of the burst's sparks
	int logo_ink = 0;    // 8 levels, gold at the top of the letters to magenta at the foot
	int logo_x = 0;
	int logo_y = 0;
	int logo_w = 0;
	int logo_h = 0;
	int scale = 1;
	float tt = 0.0;
	float slam_t = 1.93548388;      // bar 1
	float burst_t = 5.80645164;     // bar 3
	bool burst_done = false;

	void build() override {
		name = "JAISCRIPT";
		handle = "";
		titled = false;
		replay = 8.0;
		gray = pal.ramp(16, 0.0, 0.0, 0.0, 255.0, 255.0, 255.0);
		red = pal.ramp(8, 30.0, 0.0, 10.0, 255.0, 40.0, 70.0);
		cyan = pal.ramp(8, 0.0, 20.0, 30.0, 60.0, 230.0, 255.0);
		hot = pal.gradient(8, [[60.0, 10.0, 40.0], [255.0, 80.0, 60.0], [255.0, 210.0, 90.0], [255.0, 255.0, 240.0]]);
		logo_ink = pal.gradient(8, [[255.0, 250.0, 220.0], [255.0, 214.0, 110.0], [255.0, 128.0, 70.0], [230.0, 60.0, 140.0]]);
		star_cold = pal.gradient(16, [[0.0, 0.0, 0.0], [44.0, 64.0, 160.0], [140.0, 190.0, 255.0], [248.0, 252.0, 255.0]]);
		star_warm = pal.gradient(16, [[0.0, 0.0, 0.0], [110.0, 24.0, 96.0], [255.0, 128.0, 196.0], [255.0, 244.0, 250.0]]);
		neb = pal.gradient(8, [[0.0, 0.0, 0.0], [16.0, 6.0, 38.0], [44.0, 16.0, 92.0], [92.0, 34.0, 150.0]]);
		for (int i = 0; i < nstars; ++i) {
			star_x.push((rng.next(2000) - 1000) / 1000.0);
			star_y.push((rng.next(2000) - 1000) / 1000.0);
			star_z.push(0.05 + rng.next(1000) / 1000.0);
		}
		scale = 1;
		while (gfx.text_width("JAISCRIPT", scale + 1) <= w * 9 / 10 && scale < 8) { ++scale; }
		logo_w = gfx.text_width("JAISCRIPT", scale);
		logo_h = 7 * scale;
		logo_x = (w - logo_w) / 2;
		logo_y = h / 2 - logo_h / 2 - h / 10;
		array<int> mask = gfx.make_grid(w * h);
		gfx.draw_text(mask, w, h, "JAISCRIPT", logo_x, logo_y, scale, scale, 1, 1, -1, 9);
		for (int i2 = 0; i2 < w * h; ++i2) {
			if (mask[i2] == 1) { logo_at.push(i2); }
		}
		float gcx = w * 0.5;
		float gcy = logo_y + logo_h * 0.5;
		float grx = logo_w * 0.72 + 4.0;
		float gry = logo_h * 2.4 + 4.0;
		for (int y = 0; y < h; ++y) {
			float ey = (y - gcy) / gry;
			for (int x = 0; x < w; ++x) {
				float ex = (x - gcx) / grx;
				float f = 1.0 - ex * ex - ey * ey;
				glow.push(f > 0.0 ? math::itrunc(f * f * 255.0) : 0);
			}
		}
		for (int c = 0; c < pal.size(); ++c) {
			int v = pal.rgb[c];
			int l = ((v >> 16) * 3 + ((v >> 8) & 255) * 6 + (v & 255)) / 160;
			lum.push(l > 15 ? 15 : l);
		}
	}

	array<float> hit_times() override { return [slam_t, burst_t]; }
	array<int> hit_levels() override { return [2, 2]; }

	void update(float t, float dt) override {
		tt = t;
		float warp = t < slam_t ? 0.0 : 0.25 + (cue != null ? cue.kick_env(t) * 0.5 : 0.0) + (t > burst_t ? (t - burst_t) * 1.8 : 0.0);
		for (int i = 0; i < nstars; ++i) {
			star_z[i] = star_z[i] - dt * warp;
			if (star_z[i] <= 0.03) {
				star_z[i] = 1.0;
				star_x[i] = (rng.next(2000) - 1000) / 1000.0;
				star_y[i] = (rng.next(2000) - 1000) / 1000.0;
			}
		}
		if (t >= burst_t && !burst_done) {
			burst_done = true;
			for (int k = 0; k < logo_at.size(); k += 3) {
				int i3 = logo_at[k];
				float px = i3 % w;
				float py = i3 / w;
				float dx = px - w * 0.5;
				float dy = py - h * 0.5;
				float l = math::sqrt(dx * dx + dy * dy) + 1.0;
				float speed = 40.0 + rng.next(100) * 1.2;
				int lv = math::itrunc((py - logo_y) * 8 / logo_h);
				spark_ink.push(logo_ink + (lv < 0 ? 0 : (lv > 7 ? 7 : lv)));
				spark_x.push(px);
				spark_y.push(py);
				spark_vx.push(dx / l * speed + (rng.next(40) - 20));
				spark_vy.push(dy / l * speed + (rng.next(40) - 20));
			}
		}
		for (int s = 0; s < spark_x.size(); ++s) {
			spark_x[s] = spark_x[s] + spark_vx[s] * dt;
			spark_y[s] = spark_y[s] + spark_vy[s] * dt;
			spark_vx[s] = spark_vx[s] * 1.03;
			spark_vy[s] = spark_vy[s] * 1.03;
		}
	}

	void render(var& grid) override {
		int n = w * h;
		float t = tt;
		float kick = cue != null ? cue.kick_env(t) : 0.0;
		float hat = cue != null ? cue.hat_env(t) : 0.0;
		// before the slam: static that thickens over the riser, then the CRT line
		if (t < slam_t) {
			float rise = t / slam_t;
			int seed_frame = math::itrunc(t * 30.0);
			int density = math::itrunc(rise * rise * 90.0);
			int band = math::itrunc(math::fmod(t * 0.9, 1.0) * (h + h / 3)) - h / 6;
			for (int y = 0; y < h; ++y) {
				int dy = y - band;
				int roll = dy >= 0 && dy < h / 6 ? 3 : 0;
				for (int x = 0; x < w; ++x) {
					int i = y * w + x;
					int hsh = noise(i, seed_frame);
					int lv = hsh < density + roll * 12 ? 1 + (hsh % 6) + roll : (roll > 0 && hsh % 9 == 0 ? 1 : 0);
					grid[i] = gray + (lv > 15 ? 15 : lv);
				}
			}
			if (t > slam_t * 0.72) {
				float open = (t - slam_t * 0.72) / (slam_t * 0.28);
				int half = math::itrunc(open * open * w * 0.5);
				int y = h / 2;
				for (int x = w / 2 - half; x < w / 2 + half; ++x) {
					if (x >= 0 && x < w) {
						grid[y * w + x] = gray + 15;
						if (y + 1 < h) { grid[(y + 1) * w + x] = gray + 9; }
					}
				}
			}
			return;
		}
		float since = t - slam_t;
		float halo = (since < 0.33 ? since * 3.0 : 1.0) * (0.6 + 0.4 * kick) * (t > burst_t ? math::max(0.0, 1.0 - (t - burst_t) * 1.4) : 1.0);
		int halo_k = math::itrunc(halo * 8.0 * 256.0);
		for (int i = 0; i < n; ++i) {
			int lv = (glow[i] * halo_k + INTRO_DITHER[(i / w & 3) * 4 + (i % w & 3)] * 4096) >> 16;
			grid[i] = lv > 0 ? neb + (lv > 7 ? 7 : lv) : gray;
		}
		// the warp: stars streak from the centre, longer as the speed climbs
		float speed = 0.25 + kick * 0.5 + (t > burst_t ? (t - burst_t) * 1.8 : 0.0);
		for (int s = 0; s < nstars; ++s) {
			float z = star_z[s];
			float sx = w * 0.5 + star_x[s] / z * w * 0.35;
			float sy = h * 0.5 + star_y[s] / z * h * 0.35;
			float z2 = z + speed * 0.06 + 0.01;
			float tx = w * 0.5 + star_x[s] / z2 * w * 0.35;
			float ty = h * 0.5 + star_y[s] / z2 * h * 0.35;
			int lv = math::itrunc((1.0 - z) * 15.0);
			lv = lv < 2 ? 2 : (lv > 15 ? 15 : lv);
			streak(grid, tx, ty, sx, sy, s % 3 == 0 ? star_warm : star_cold, lv);
		}
		// the shockwaves: the slam's, then the burst's
		if (since < 1.0) {
			ring(grid, w * 0.5, logo_y + logo_h * 0.5, since * w * 0.9, 1.5 + since * 4.0, red + 7, gray + 15, cyan + 7);
		}
		float since_burst = t - burst_t;
		if (since_burst > 0.0 && since_burst < 0.8) {
			ring(grid, w * 0.5, logo_y + logo_h * 0.5, since_burst * w * 1.2, 2.0 + since_burst * 6.0, hot + 3, hot + 7, star_cold + 13);
		}
		if (t < burst_t) {
			// the logo: red and cyan ghosts split apart on the slam and on every hat, white on top
			float split = (since < 0.5 ? (0.5 - since) * 2.0 : 0.0) * scale * 3.0 + hat * scale * 0.8;
			int off = math::itrunc(split);
			draw_logo(grid, -off, 0, red + 7);
			draw_logo(grid, off, 0, cyan + 7);
			if (since < 0.12) { draw_logo(grid, 0, 0, gray + 15); }
			else { draw_logo_gradient(grid, kick); }
			if (cue != null && hat > 0.6 && t > slam_t + 1.0) { glitch(grid, math::itrunc(t / MUSIC_STEP)); }
		}
		tagline();
		// the burst
		for (int s2 = 0; s2 < spark_x.size(); ++s2) {
			float x = spark_x[s2];
			float y = spark_y[s2];
			if (x < 0.0 || x >= w || y < 0.0 || y >= h) { continue; }
			paint_line(grid, x - spark_vx[s2] * 0.04, y - spark_vy[s2] * 0.04, x, y, s2 % 5 == 0 ? hot + 7 : spark_ink[s2]);
		}
		// the white-out into the first scene
		float white = (t - (burst_t + MUSIC_BAR * 0.75)) / (MUSIC_BAR * 0.25);
		if (white > 0.0) {
			int lv2 = math::itrunc((white > 1.0 ? 1.0 : white) * 15.0);
			for (int i4 = 0; i4 < n; ++i4) {
				if (lum[grid[i4]] < lv2) { grid[i4] = gray + lv2; }
			}
		}
	}

	void tagline() {
		if (tt < slam_t + 0.35 || tt > burst_t) { return; }
		string a = "a scripting language for c++";
		string b = "every pixel . every note . script";
		int shown_a = math::itrunc((tt - slam_t - 0.35) * 34.0);
		int shown_b = math::itrunc((tt - slam_t - 1.4) * 30.0);
		int row = cell_row(logo_y + logo_h) + 2;
		if (shown_a > 0) { txt_put((HB_COLS - a.size()) / 2, row, a.substr(0, shown_a > a.size() ? a.size() : shown_a), rgb(214, 208, 255)); }
		if (shown_b > 0 && row + 2 < HB_ROWS) { txt_put((HB_COLS - b.size()) / 2, row + 2, b.substr(0, shown_b > b.size() ? b.size() : shown_b), rgb(255, 206, 110)); }
	}

	// the letters shaded top to bottom, the kick lifting them toward white
	void draw_logo_gradient(var& grid, float kick) {
		int lift = math::itrunc(kick * 3.0);
		for (int k = 0; k < logo_at.size(); ++k) {
			int i = logo_at[k];
			int lv = (i / w - logo_y) * 8 / logo_h - lift;
			grid[i] = logo_ink + (lv < 0 ? 0 : (lv > 7 ? 7 : lv));
		}
	}

	void draw_logo(var& grid, int dx, int dy, int ink) {
		for (int k = 0; k < logo_at.size(); ++k) {
			int i = logo_at[k];
			int x = i % w + dx;
			int y = i / w + dy;
			if (x >= 0 && x < w && y >= 0 && y < h) { grid[y * w + x] = ink; }
		}
	}

	void glitch(var& grid, int step) {
		array<int> row = [];
		for (int x = 0; x < w; ++x) { row.push(0); }
		for (int y = logo_y - 2; y < logo_y + logo_h + 2; ++y) {
			if (y < 0 || y >= h) { continue; }
			int slice = (y / 3 + step * 5) % 6;
			if (slice > 1) { continue; }
			int off = slice == 0 ? scale * 2 : -scale * 2;
			int base = y * w;
			for (int x2 = 0; x2 < w; ++x2) { row[x2] = grid[base + x2]; }
			for (int x3 = 0; x3 < w; ++x3) {
				int from = x3 - off;
				from = from < 0 ? 0 : (from >= w ? w - 1 : from);
				grid[base + x3] = row[from];
			}
		}
	}

	// an integer hash of a pixel and a frame, kept under 2^62 so the checked multiplies never overflow
	int noise(int i, int frame) {
		int v = (i * 73856093 + frame * 19349663) & 2147483647;
		v = ((v ^ (v >> 13)) * 1274126177) & 2147483647;
		return (v ^ (v >> 16)) & 255;
	}

	// a star's streak, dim at the tail (x0, y0) and brightest at its head
	void streak(var& grid, float x0, float y0, float x1, float y1, int ramp, int lv) {
		float dx = x1 - x0;
		float dy = y1 - y0;
		float adx = dx < 0.0 ? -dx : dx;
		float ady = dy < 0.0 ? -dy : dy;
		int steps = math::itrunc((adx > ady ? adx : ady) + 1.0);
		steps = steps > 60 ? 60 : steps;
		for (int s = 0; s <= steps; ++s) {
			int x = math::itrunc(x0 + dx * s / steps);
			int y = math::itrunc(y0 + dy * s / steps);
			if (x >= 0 && x < w && y >= 0 && y < h) { grid[y * w + x] = ramp + lv / 3 + (lv - lv / 3) * s / steps; }
		}
	}

	void paint_line(var& grid, float x0, float y0, float x1, float y1, int ink) {
		float dx = x1 - x0;
		float dy = y1 - y0;
		float adx = dx < 0.0 ? -dx : dx;
		float ady = dy < 0.0 ? -dy : dy;
		int steps = math::itrunc((adx > ady ? adx : ady) + 1.0);
		steps = steps > 30 ? 30 : steps;
		for (int s = 0; s <= steps; ++s) {
			int x = math::itrunc(x0 + dx * s / steps);
			int y = math::itrunc(y0 + dy * s / steps);
			if (x >= 0 && x < w && y >= 0 && y < h) { grid[y * w + x] = ink; }
		}
	}

	// a ring split like a lens: the outer third in one ink, the core in another, the inner third in a third
	void ring(var& grid, float cx, float cy, float r, float width, int outer, int core, int inner) {
		int y0 = math::itrunc(cy - r - width) - 1;
		int y1 = math::itrunc(cy + r + width) + 1;
		y0 = y0 < 0 ? 0 : y0;
		y1 = y1 > h - 1 ? h - 1 : y1;
		for (int y = y0; y <= y1; ++y) {
			float ey = y - cy;
			for (int x = 0; x < w; ++x) {
				float ex = x - cx;
				float d = math::sqrt(ex * ex + ey * ey) - r;
				if (d < width && d > -width) { grid[y * w + x] = d > width * 0.34 ? outer : (d < -width * 0.34 ? inner : core); }
			}
		}
	}
}
