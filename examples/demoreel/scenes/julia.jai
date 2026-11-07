// JULIA — morphing Julia set: c orbits just inside the Mandelbrot cardioid so
// the set stays connected while it writhes. Every pixel, full resolution, rows
// iterated across all cores with parallel_transform, with continuous
// escape-time coloring (the log-log fractional count removes the iteration
// bands) into a 256-entry cycling palette, and an orbit trap for the inside:
// the closest the orbit came to the origin shades the set's interior, so the
// prisoner points glow instead of sitting flat black.

float JU_CA = 0.0;
float JU_CB = 0.0;
float JU_SC = 0.01;
float JU_X0 = 0.0;
float JU_Y0 = 0.0;
int JU_W = 0;
int JU_MAXIT = 48;
int JU_PHASE = 0;
int JU_INNER = 0;
array<int> JU_ROWS = [];

function julia_row(int y) -> array<int> {
	array<int> out = [];
	int ww = JU_W;
	int mi = JU_MAXIT;
	float ca = JU_CA;
	float cb = JU_CB;
	float sc = JU_SC;
	float zy0 = JU_Y0 + y * sc;
	for (int x = 0; x < ww; ++x) {
		float zx = JU_X0 + x * sc;
		float zy = zy0;
		float x2 = zx * zx;
		float y2 = zy * zy;
		float trap = 9.0;
		int k = 0;
		while (k < mi && x2 + y2 < 64.0) {
			zy = 2.0 * zx * zy + cb;
			zx = x2 - y2 + ca;
			x2 = zx * zx;
			y2 = zy * zy;
			float m = x2 + y2;
			trap = m < trap ? m : trap;
			++k;
		}
		if (k >= mi) {
			int tl = math::sqrt(trap) * 40.0;
			out.push(JU_INNER + 31 - (tl > 31 ? 31 : tl));
		} else {
			float nu = k + 1.0 - math::log2(math::log2(x2 + y2) * 0.5);
			int ci = math::sqrt(nu) * 52.0;
			out.push((ci + JU_PHASE) & 255);
		}
	}
	return out;
}

class Julia : Scene {
	int inner = 0;
	float tt = 0.0;

	void build() override {
		name = "JULIA";
		handle = "orbit morphing . by the type ladder";
		pal.gradient(256, [[8.0, 4.0, 30.0], [40.0, 20.0, 110.0], [170.0, 40.0, 170.0], [255.0, 110.0, 80.0],
			[255.0, 230.0, 130.0], [255.0, 255.0, 240.0], [100.0, 210.0, 255.0], [30.0, 70.0, 180.0], [8.0, 4.0, 30.0]]);
		inner = pal.gradient(32, [[2.0, 2.0, 10.0], [10.0, 26.0, 60.0], [20.0, 90.0, 120.0], [150.0, 240.0, 230.0]]);
	}

	void update(float t, float dt) override {
		tt = t;
	}

	void render(var& grid) override {
		float zoom = 1.05 + 0.3 * math::sin(tt * 0.4);
		JU_CA = 0.7885 * math::cos(tt * 0.31 + 2.0);
		JU_CB = 0.7885 * math::sin(tt * 0.23 + 2.0);
		JU_SC = 3.3 / (w * zoom);
		JU_X0 = 0.0 - w * 0.5 * JU_SC;
		JU_Y0 = 0.0 - h * 0.5 * JU_SC;
		JU_W = w;
		JU_PHASE = itrunc(tt * 22.0);
		JU_INNER = inner;
		if (JU_ROWS.size() != h) {
			JU_ROWS = [];
			for (int r = 0; r < h; ++r) { JU_ROWS.push(r); }
		}
		var rows = parallel_transform(JU_ROWS, julia_row);
		int idx = 0;
		for (int y = 0; y < h; ++y) {
			var& row = rows[y];
			for (int x = 0; x < w; ++x) {
				grid[idx] = row[x];
				++idx;
			}
		}
	}
}
