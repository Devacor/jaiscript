// FIRE — the house classic: bottom-seeded convection with the Doom fire's
// random sideways jitter and cooling, at full pixel resolution with 73
// heat levels (the ramp is smooth enough that the tongues have gradients, not
// bands) and sparks: embers that break off and drift up, flickering out. Midway through the scene it ignites
// into RAINBOW fire (the palette carries six precomputed hue-rolled banks;
// render just offsets into them), because this codebase is patrolled by a
// wingless rainbow-fire dragon.

class Fire : Scene {
	array<int> heat = [];    // (h+2) rows: two hidden seed rows below the screen
	array<int> cool = [];    // prebuilt 0..3 spread noise, indexed cheaply per pixel
	array<float> spx = [];   // sparks
	array<float> spy = [];
	array<float> svx = [];
	array<float> svy = [];
	array<float> slife = [];
	int nsparks = 48;
	int levels = 73;
	int phases = 6;      // rainbow hue-roll banks
	int fr = 0;
	int rainbow_on = 0;
	float hue_t = 0.0;
	float tt = 0.0;

	void build() override {
		name = "FIRE";
		handle = "ember chapel -> rainbow mode . for devacor";
		build_fire();
	}

	void build_fire() {
		heat = gfx.make_grid(w * (h + 2));
		for (int i = 0; i < 1024; ++i) { cool.push(rng.next(4)); }
		// entries 0..72: classic fire ramp
		for (int i = 0; i < levels; ++i) {
			float f = i / (levels - 1.0);
			float r = 255.0 * f * 2.2;
			float g = f > 0.28 ? 255.0 * (f - 0.28) * 1.7 : 0.0;
			float b = f > 0.75 ? 255.0 * (f - 0.75) * 3.2 : 0.0;
			pal.add(r, g, b);
		}
		// entries 73+p*73 .. : rainbow banks, hue rolled per bank
		for (int p = 0; p < phases; ++p) {
			float base_hue = p * 6.28318530718 / phases;
			for (int i = 0; i < levels; ++i) {
				float f = i / (levels - 1.0);
				var c = gfx.rainbow_rgb(base_hue + f * 1.4);
				float glow = f * 1.8 > 1.0 ? 1.0 : f * 1.8;
				float wh = f > 0.85 ? (f - 0.85) * 4.0 : 0.0;
				float cr = c[0] * glow;
				float cg = c[1] * glow;
				float cb = c[2] * glow;
				pal.add(cr + (255.0 - cr) * wh, cg + (255.0 - cg) * wh, cb + (255.0 - cb) * wh);
			}
		}
		for (int s = 0; s < nsparks; ++s) {
			spx.push(0.0);
			spy.push(0.0);
			svx.push(0.0);
			svy.push(0.0);
			slife.push(0.0);
		}
	}

	void update(float t, float dt) override {
		tt = t;
		++fr;
		rainbow_on = t > 5.5 ? 1 : 0;
		if (rainbow_on == 1) { hue_t += dt * 3.0; }
		seed_rows(40, 33);
		convect();
		move_sparks(dt, true);
	}

	// the two hidden bottom rows: mostly furious, occasionally gapped
	void seed_rows(int lo, int span) {
		int base = h * w;
		for (int x = 0; x < w; ++x) {
			int v = lo + rng.next(span);
			v = rng.next(100) < 13 ? rng.next(lo / 3 + 2) : v;
			heat[base + x] = v;
			heat[base + w + x] = v;
		}
	}

	// convection with the Doom fire's jitter: each pixel averages the three
	// below and one two-below around a column nudged by a random draw, and the
	// same draw cools it; the average keeps gradients, the jitter licks tongues
	void convect() {
		int ww = w;
		int hh = h;
		var& ht = heat;
		var& rn = cool;
		int salt = fr * 131;
		for (int y = 0; y < hh; ++y) {
			int row = y * ww;
			int below = row + ww;
			int below2 = below + ww;
			for (int x = 0; x < ww; ++x) {
				int r = rn[(row + x * 7 + salt) & 1023];
				int xs = x + (r == 3 ? 0 : r - 1);
				xs = xs < 1 ? 1 : (xs > ww - 2 ? ww - 2 : xs);
				int v = ((ht[below + xs - 1] + ht[below + xs] + ht[below + xs + 1] + ht[below2 + xs]) >> 2) - ((r + 1) >> 1);
				ht[row + x] = v > 0 ? v : 0;
			}
		}
	}

	void move_sparks(float dt, bool spawn) {
		for (int s = 0; s < nsparks; ++s) {
			float life = slife[s] - dt;
			if (life <= 0.0) {
				if (!spawn || rng.next(100) >= 12) { slife[s] = 0.0; continue; }
				spx[s] = rng.next(w * 10) * 0.1;
				spy[s] = h * (0.62 + rng.next(30) * 0.01);
				svx[s] = (rng.next(200) - 100) * 0.08;
				svy[s] = 0.0 - (18.0 + rng.next(220) * 0.1);
				life = 0.8 + rng.next(100) * 0.012;
			}
			slife[s] = life;
			svx[s] = svx[s] + math::sin(tt * 3.0 + s) * 6.0 * dt;
			spx[s] = spx[s] + svx[s] * dt;
			spy[s] = spy[s] + svy[s] * dt;
		}
	}

	int bank() {
		return rainbow_on == 1 ? levels + (math::itrunc(hue_t * 4.0) % phases) * levels : 0;
	}

	void render(var& grid) override {
		int b = bank();
		int n = w * h;
		var& ht = heat;
		for (int i = 0; i < n; ++i) {
			int v = ht[i];
			grid[i] = (v > 72 ? 72 : v) + b;
		}
		draw_sparks(grid, b);
	}

	void draw_sparks(var& grid, int b) {
		for (int s = 0; s < nsparks; ++s) {
			float life = slife[s];
			if (life <= 0.0) { continue; }
			int x = math::itrunc(spx[s]);
			int y = math::itrunc(spy[s]);
			if (x < 0 || x >= w || y < 0 || y >= h) { continue; }
			int lv = 40 + math::itrunc(life * 30.0) + ((fr + s) & 3) * 3;
			grid[y * w + x] = (lv > 72 ? 72 : lv) + b;
		}
	}
}
