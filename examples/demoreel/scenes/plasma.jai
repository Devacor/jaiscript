// PLASMA — the classic: four summed sine fields over a 256-entry cyclic palette.
// Integer table lookups per pixel; sin() only runs at build time. The radial
// term reads a double-size distance table through a sliding window, so the
// ripple's center orbits the screen at zero per-pixel cost.

class Plasma : Scene {
	array<int> st = [];      // sine table, 1024 entries scaled to +-1024
	array<int> dist = [];    // (2w x 2h) radial distance, read through a moving window
	int dw = 0;
	float tt = 0.0;

	void build() override {
		name = "PLASMA";
		handle = "warmup loop . by the symbolizer";
		for (int i = 0; i < 1024; ++i) {
			st.push(itrunc(math::sin(i * 0.00613592315) * 1024.0));
		}
		dw = w * 2;
		for (int y = 0; y < h * 2; ++y) {
			for (int x = 0; x < dw; ++x) {
				float dx = x - w * 1.0;
				float dy = y - h * 1.0;
				dist.push(itrunc(math::sqrt(dx * dx + dy * dy) * 26.0) & 1023);
			}
		}
		pal.gradient(256, [[12.0, 6.0, 40.0], [68.0, 20.0, 150.0], [214.0, 40.0, 150.0], [255.0, 120.0, 60.0],
			[255.0, 226.0, 110.0], [120.0, 240.0, 200.0], [30.0, 140.0, 230.0], [20.0, 30.0, 110.0], [12.0, 6.0, 40.0]]);
	}

	void update(float t, float dt) override {
		tt = t;
	}

	void render(var& grid) override {
		int ww = w;
		int hh = h;
		int dww = dw;
		var& s = st;
		var& dt_ = dist;
		int t1 = itrunc(tt * 150.0);
		int t2 = itrunc(tt * 97.0);
		int t3 = itrunc(tt * -210.0) + 4096;
		int t4 = itrunc(tt * 61.0);
		int phase = itrunc(tt * 38.0);
		int ox = itrunc(w * 0.5 + w * 0.42 * math::sin(tt * 0.61));
		int oy = itrunc(h * 0.5 + h * 0.40 * math::sin(tt * 0.83 + 1.3));
		int zoom = 17 + itrunc(6.0 * math::sin(tt * 0.35));
		int idx = 0;
		for (int y = 0; y < hh; ++y) {
			int ys = s[(y * 21 + t2) & 1023];
			int drow = (y + oy) * dww + ox;
			int yx = y * 13 + t4;
			for (int x = 0; x < ww; ++x) {
				int v = s[(x * zoom + t1) & 1023] + ys + s[(dt_[drow + x] + t3) & 1023] + s[(x * 11 + yx) & 1023];
				grid[idx] = ((v >> 4) + phase) & 255;
				++idx;
			}
		}
	}
}
