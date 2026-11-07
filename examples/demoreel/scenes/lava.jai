// LAVALAMP — metaballs, lit. Eight blobs sum an inverse-square field per pixel;
// the same loop accumulates the field's gradient and a contribution-weighted
// hue, so merged blobs blend their colors into a bridge and the surface is lit
// from a normal built out of the gradient (diffuse + a sharp specular), a glossy
// wax bulging out of the screen. Outside the threshold the field is a tinted
// glow that falls off into the lamp's dark glass. Rows run in parallel.

array<float> LV_BX = [];
array<float> LV_BY = [];
array<float> LV_R2 = [];
array<float> LV_HUE = [];
array<int> LV_ROWS = [];
int LV_W = 0;
int LV_GLOW = 0;
float LV_RR = 1.0;

// one row of the field, on a parallel_transform worker: the blob arrays borrow
// zero-copy, the scalars are per-worker copies, only the returned row is written
function lava_row(int y) -> array<int> {
	array<int> out = [];
	int ww = LV_W;
	int n = LV_BX.size();
	float rr = LV_RR;
	float lx = -0.45;
	float ly = -0.55;
	float lz = 0.70;
	float hl = 1.0 / math::sqrt(lx * lx + ly * ly + (lz + 1.0) * (lz + 1.0));
	float hx = lx * hl;
	float hy = ly * hl;
	float hz = (lz + 1.0) * hl;
	float fy = y + 0.5;
	for (int x = 0; x < ww; ++x) {
		float fx = x + 0.5;
		float f = 0.0;
		float gx = 0.0;
		float gy = 0.0;
		float hsum = 0.0;
		for (int b = 0; b < n; ++b) {
			float dx = fx - LV_BX[b];
			float dy = fy - LV_BY[b];
			float d2 = dx * dx + dy * dy + 1.0;
			float inv = LV_R2[b] / d2;
			f += inv;
			float q = inv / d2;
			gx += q * dx;
			gy += q * dy;
			hsum += inv * LV_HUE[b];
		}
		int hq = hsum / f + 0.5;
		if (f >= 1.0) {
			// height of a blob of radius rr under this field: rr*sqrt(1 - 1/f);
			// its gradient is the field gradient over f^2, vertical at the rim
			float rim = 1.0 - 1.0 / f;
			rim = rim < 0.004 ? 0.004 : rim;
			float kz = rr / (f * f * math::sqrt(rim));
			float nx = gx * kz;
			float ny = gy * kz;
			float il = 1.0 / math::sqrt(nx * nx + ny * ny + 1.0);
			nx *= il; ny *= il;
			float d = nx * lx + ny * ly + il * lz;
			d = d < 0.0 ? 0.0 : d;
			float s = nx * hx + ny * hy + il * hz;
			float s2 = s * s;
			float s4 = s2 * s2;
			float s8 = s4 * s4;
			float s16 = s8 * s8;
			int lv = (d * 0.78 + s16 * s16 * 0.42) * 15.99;
			out.push(hq * 16 + (lv > 15 ? 15 : lv));
		} else {
			int gl = f * f * 7.99;
			out.push(LV_GLOW + hq * 8 + gl);
		}
	}
	return out;
}

class Lava : Scene {
	array<float> bx = [];
	array<float> by = [];
	array<float> br2 = [];
	array<float> hue = [];
	array<float> fa = [];
	array<float> fb = [];
	array<float> pa = [];
	array<float> pb = [];
	int nb = 8;
	int glow0 = 0;
	float tt = 0.0;

	void build() override {
		name = "LAVALAMP";
		handle = "wax and gradient . by operator overload";
		// 24 hues x 16 shades (wax), then 24 hues x 8 glow levels
		for (int hq = 0; hq < 24; ++hq) {
			var c = gfx.rainbow_rgb(hq * 0.22 + 4.4);
			float cr = c[0]; float cg = c[1]; float cb = c[2];
			for (int s = 0; s < 16; ++s) {
				float f = s / 15.0;
				float lit = f < 0.75 ? 0.10 + f * 1.2 : 1.0;
				float wh = f < 0.75 ? 0.0 : (f - 0.75) * 3.6;
				wh = wh > 0.9 ? 0.9 : wh;
				float r = cr * lit;
				float g = cg * lit;
				float b = cb * lit;
				pal.add(r + (255.0 - r) * wh, g + (255.0 - g) * wh, b + (255.0 - b) * wh);
			}
		}
		glow0 = pal.size();
		for (int hq2 = 0; hq2 < 24; ++hq2) {
			var c2 = gfx.rainbow_rgb(hq2 * 0.22 + 4.4);
			for (int g2 = 0; g2 < 8; ++g2) {
				float k = g2 / 7.0;
				k = k * k * 0.55;
				pal.add(10.0 + c2[0] * k, 4.0 + c2[1] * k, 18.0 + c2[2] * k);
			}
		}
		for (int i = 0; i < nb; ++i) {
			float r = h * (0.075 + rng.next(50) * 0.001);
			br2.push(r * r);
			hue.push(i * 23.0 / (nb - 1.0));
			fa.push(0.13 + rng.next(100) * 0.003);
			fb.push(0.17 + rng.next(100) * 0.004);
			pa.push(rng.next(628) * 0.01);
			pb.push(rng.next(628) * 0.01);
			bx.push(0.0);
			by.push(0.0);
		}
	}

	void update(float t, float dt) override {
		tt = t;
		for (int i = 0; i < nb; ++i) {
			bx[i] = w * (0.5 + 0.38 * math::sin(t * fa[i] * 2.0 + pa[i]));
			by[i] = h * (0.5 + 0.42 * math::sin(t * fb[i] * 2.0 + pb[i]));
		}
	}

	void render(var& grid) override {
		LV_BX = bx;
		LV_BY = by;
		LV_R2 = br2;
		LV_HUE = hue;
		LV_W = w;
		LV_RR = h * 0.1;
		LV_GLOW = glow0;
		if (LV_ROWS.size() != h) {
			LV_ROWS = [];
			for (int r = 0; r < h; ++r) { LV_ROWS.push(r); }
		}
		var rows = parallel_transform(LV_ROWS, lava_row);
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
