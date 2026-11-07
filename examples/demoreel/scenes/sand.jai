// POWDER — falling-sand mini toy: sand, water, ember, plant, smoke, each a
// script class owning its behavior. Spawners keep it evolving unattended:
// sand and water pour in, plants drink and grow, embers burn them back down.

class Element {
	void step(var& c, var& a, int w, int h, int i, int x, int y, var& r, int fr) {}
}

class SandEl : Element {
	void step(var& c, var& a, int w, int h, int i, int x, int y, var& r, int fr) override {
		if (y + 1 >= h) { return; }
		int b = i + w;
		int t = c[b];
		if (t == 0) { c[b] = 2; c[i] = 0; return; }
		if (t == 3) { c[b] = 2; c[i] = 3; return; }   // heavier than water: swap
		int dir = ((x + y + fr) & 1) * 2 - 1;
		int nx = x + dir;
		if (nx >= 0 && nx < w && c[b + dir] == 0 && c[i + dir] == 0) { c[b + dir] = 2; c[i] = 0; return; }
		nx = x - dir;
		if (nx >= 0 && nx < w && c[b - dir] == 0 && c[i - dir] == 0) { c[b - dir] = 2; c[i] = 0; }
	}
}

class WaterEl : Element {
	void step(var& c, var& a, int w, int h, int i, int x, int y, var& r, int fr) override {
		if (y + 1 < h) {
			int b = i + w;
			if (c[b] == 0) { c[b] = 3; c[i] = 0; return; }
			int dir = ((x * 3 + y + fr * 5) & 1) * 2 - 1;
			int nx = x + dir;
			if (nx >= 0 && nx < w && c[b + dir] == 0 && c[i + dir] == 0) { c[b + dir] = 3; c[i] = 0; return; }
		}
		// flow up to four cells sideways per step, so pools level out
		int dir = ((x + fr * 3 + y * 7) & 1) * 2 - 1;
		for (int side = 0; side < 2; ++side) {
			int far = 0;
			for (int k = 1; k <= 4; ++k) {
				int nx = x + dir * k;
				if (nx < 0 || nx >= w || c[i + dir * k] != 0) { break; }
				far = k;
			}
			if (far > 0) { c[i + dir * far] = 3; c[i] = 0; return; }
			dir = 0 - dir;
		}
	}
}

class EmberEl : Element {
	void step(var& c, var& a, int w, int h, int i, int x, int y, var& r, int fr) override {
		a[i] = a[i] + 1;
		// ignite any neighboring plant
		if (x > 0 && c[i - 1] == 5) { c[i - 1] = 4; a[i - 1] = 0; }
		if (x < w - 1 && c[i + 1] == 5) { c[i + 1] = 4; a[i + 1] = 0; }
		if (y > 0 && c[i - w] == 5) { c[i - w] = 4; a[i - w] = 0; }
		if (y < h - 1 && c[i + w] == 5) { c[i + w] = 4; a[i + w] = 0; }
		if (a[i] > 22 + r.next(26)) { c[i] = 6; a[i] = 0; return; }   // burn out to smoke
		if (y + 1 >= h) { return; }
		int b = i + w;
		if (c[b] == 3) { c[i] = 6; a[i] = 0; return; }   // doused
		if ((fr & 1) == 0 && c[b] == 0) { c[b] = 4; a[b] = a[i]; c[i] = 0; a[i] = 0; }
	}
}

class PlantEl : Element {
	void step(var& c, var& a, int w, int h, int i, int x, int y, var& r, int fr) override {
		if (a[i] > 30) { return; }             // generation cap: stop creeping forever
		if (r.next(100) >= 6) { return; }
		// drink an adjacent water cell, then sprout into an empty neighbor
		int drink = -1;
		if (x > 0 && c[i - 1] == 3) { drink = i - 1; }
		else if (x < w - 1 && c[i + 1] == 3) { drink = i + 1; }
		else if (y > 0 && c[i - w] == 3) { drink = i - w; }
		else if (y < h - 1 && c[i + w] == 3) { drink = i + w; }
		if (drink < 0) { return; }
		c[drink] = 0;
		int sprout = -1;
		if (y > 0 && c[i - w] == 0) { sprout = i - w; }
		else if (x > 0 && y > 0 && c[i - w - 1] == 0) { sprout = i - w - 1; }
		else if (x < w - 1 && y > 0 && c[i - w + 1] == 0) { sprout = i - w + 1; }
		else if (x > 0 && c[i - 1] == 0) { sprout = i - 1; }
		else if (x < w - 1 && c[i + 1] == 0) { sprout = i + 1; }
		if (sprout >= 0) {
			c[sprout] = 5;
			a[sprout] = a[i] + 1;
		}
	}
}

class SmokeEl : Element {
	void step(var& c, var& a, int w, int h, int i, int x, int y, var& r, int fr) override {
		a[i] = a[i] + 1;
		if (a[i] > 26) { c[i] = 0; a[i] = 0; return; }
		if (y == 0) { c[i] = 0; a[i] = 0; return; }
		int wob = ((x + fr + a[i] * 3) % 3) - 1;
		int nx = x + wob;
		if (nx >= 0 && nx < w && c[i - w + wob] == 0) { c[i - w + wob] = 6; a[i - w + wob] = a[i]; c[i] = 0; a[i] = 0; return; }
		if (c[i - w] == 0) { c[i - w] = 6; a[i - w] = a[i]; c[i] = 0; a[i] = 0; }
	}
}

class SandBox : Scene {
	var cells = null;
	var aux = null;
	var el_sand = null;      // element singletons live in fields, dispatch by if-chain
	var el_water = null;
	var el_ember = null;
	var el_plant = null;
	var el_smoke = null;
	array<int> depth = [];   // per column: water cells stacked above the current row
	int fr = 0;
	int p_sand = 0;
	int p_water = 0;
	int p_ember = 0;
	int p_plant = 0;
	int p_smoke = 0;
	int p_rock = 0;
	int p_glow = 0;

	void build() override {
		name = "POWDER";
		handle = "sand.water.ember.plant . by ref_lvalue";
		cells = gfx.make_grid(w * h);
		aux = gfx.make_grid(w * h);
		depth = gfx.make_grid(w);
		el_sand = SandEl();
		el_water = WaterEl();
		el_ember = EmberEl();
		el_plant = PlantEl();
		el_smoke = SmokeEl();

		// 0..h-1: the empty air, a dusk gradient by row
		for (int y = 0; y < h; ++y) {
			float f = y / (h - 1.0);
			pal.add(8.0 + 18.0 * f, 6.0 + 10.0 * f, 18.0 + 16.0 * f);
		}
		p_rock = pal.ramp(4, 50.0, 46.0, 58.0, 118.0, 112.0, 126.0);
		p_sand = pal.ramp(6, 170.0, 130.0, 66.0, 250.0, 222.0, 150.0);
		p_water = pal.ramp(8, 90.0, 170.0, 255.0, 14.0, 40.0, 130.0);
		p_ember = pal.gradient(6, [[150.0, 30.0, 10.0], [255.0, 90.0, 20.0], [255.0, 190.0, 60.0], [255.0, 255.0, 200.0]]);
		p_plant = pal.ramp(5, 28.0, 96.0, 40.0, 120.0, 220.0, 100.0);
		p_smoke = pal.ramp(4, 96.0, 94.0, 104.0, 34.0, 32.0, 42.0);
		p_glow = pal.ramp(3, 60.0, 26.0, 16.0, 140.0, 60.0, 24.0);

		// rock floor + two shelves making basins
		int ground = (h - 1) * w;
		for (int x = 0; x < w; ++x) { cells[ground + x] = 1; cells[ground - w + x] = 1; }
		int shelf1 = (h - h * 9 / 40) * w;
		for (int x = w / 8; x < w / 8 + w / 4; ++x) { cells[shelf1 + x] = 1; cells[shelf1 + w + x] = 1; }
		int shelf2 = (h - h * 15 / 40) * w;
		for (int x = w - w / 3 - w / 8; x < w - w / 8; ++x) { cells[shelf2 + x] = 1; cells[shelf2 + w + x] = 1; }
		for (int k = 0; k < 14; ++k) {
			int x = 4 + rng.next(w - 8);
			cells[ground - w - w + x] = 5;
		}
	}

	void update(float t, float dt) override {
		for (int sub = 0; sub < 2; ++sub) {
			++fr;
			do_spawn();
			do_fall();
			do_smoke();
		}
	}

	void do_spawn() {
		for (int k = 0; k < 2; ++k) {
			int sx = w / 5 + rng.next(7) - 3;
			if (cells[sx] == 0) { cells[sx] = 2; }
			int wx = (w * 4) / 5 + rng.next(7) - 3;
			if (cells[wx] == 0) { cells[wx] = 3; }
		}
		if (fr % 90 == 0) {
			for (int k2 = 0; k2 < 6; ++k2) {
				int ex = w / 2 - 5 + rng.next(11);
				if (cells[ex] == 0) { cells[ex] = 4; aux[ex] = 0; }
			}
		}
	}

	// bottom-up: falling and static matter (alternate x sweep per frame)
	void do_fall() {
		for (int y = h - 2; y >= 0; --y) {
			int row = y * w;
			if ((fr & 1) == 0) {
				for (int x = 0; x < w; ++x) {
					int i = row + x;
					int t2 = cells[i];
					if (t2 == 2) { el_sand.step(cells, aux, w, h, i, x, y, rng, fr); }
					else if (t2 == 3) { el_water.step(cells, aux, w, h, i, x, y, rng, fr); }
					else if (t2 == 4) { el_ember.step(cells, aux, w, h, i, x, y, rng, fr); }
					else if (t2 == 5) { el_plant.step(cells, aux, w, h, i, x, y, rng, fr); }
				}
			} else {
				for (int x = w - 1; x >= 0; --x) {
					int i = row + x;
					int t2 = cells[i];
					if (t2 == 2) { el_sand.step(cells, aux, w, h, i, x, y, rng, fr); }
					else if (t2 == 3) { el_water.step(cells, aux, w, h, i, x, y, rng, fr); }
					else if (t2 == 4) { el_ember.step(cells, aux, w, h, i, x, y, rng, fr); }
					else if (t2 == 5) { el_plant.step(cells, aux, w, h, i, x, y, rng, fr); }
				}
			}
		}
	}

	// top-down: rising smoke
	void do_smoke() {
		int n = w * h;
		for (int i = 0; i < n; ++i) {
			if (cells[i] == 6) {
				el_smoke.step(cells, aux, w, h, i, i % w, i / w, rng, fr);
			}
		}
	}

	// surfaces catch the light (a cell with air above is its material's top
	// shade), water darkens with the depth of the column above it, and embers
	// throw a warm glow onto the air around them
	void render(var& grid) override {
		int ww = w;
		int hh = h;
		var& c = cells;
		var& a = aux;
		var& dp = depth;
		for (int x0 = 0; x0 < ww; ++x0) { dp[x0] = 0; }
		int idx = 0;
		for (int y = 0; y < hh; ++y) {
			for (int x = 0; x < ww; ++x) {
				int t = c[idx];
				int g = y;
				bool open = y == 0 || c[idx - ww] == 0;
				if (t == 3) {
					int d = dp[x] + 1;
					dp[x] = d;
					int wl = open ? 0 : 1 + d / 3 + (x * 3 + y + fr / 5) % 2;
					g = p_water + (wl > 7 ? 7 : wl);
				} else {
					dp[x] = 0;
					if (t == 2) { g = p_sand + (open ? 5 : ((x * 7 + y * 13) * 2971 / 7) % 4 + (y % 3 == 0 ? 0 : 1)); }
					else if (t == 1) { g = p_rock + (open ? 3 : (x * 5 + y * 3) % 3); }
					else if (t == 5) { g = p_plant + (open ? 4 : (x * 7 + y * 3) % 4); }
					else if (t == 4) { g = p_ember + (a[idx] * 3 + x + fr) % 6; }
					else if (t == 6) { int sa = a[idx] / 7; g = p_smoke + (sa > 3 ? 3 : sa); }
				}
				grid[idx] = g;
				++idx;
			}
		}
		for (int i = ww; i < ww * hh - ww; ++i) {
			if (c[i] != 4) { continue; }
			int gl = p_glow + (fr + i) % 3;
			if (c[i - 1] == 0) { grid[i - 1] = gl; }
			if (c[i + 1] == 0) { grid[i + 1] = gl; }
			if (c[i - ww] == 0) { grid[i - ww] = gl; }
			if (c[i + ww] == 0) { grid[i + ww] = gl; }
		}
	}
}
