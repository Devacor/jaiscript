// TUNNEL — the table-driven texture tunnel. atan2 and 1/r run once per pixel
// of a double-size table at build; a frame is four integer lookups per pixel:
// angle, depth, fog shade, texel. The camera's aim wanders because the screen
// reads the tables through a sliding window (the vanishing point moves at zero
// cost), the walls twist with depth, and the texture is grown in script:
// staggered bevelled plates with rivets, and neon rings that race toward you.

class Tunnel : Scene {
	array<int> ang = [];     // (2w x 2h) angle, 0..255 around
	array<int> dep = [];     // (2w x 2h) depth coordinate
	array<int> fog = [];     // (2w x 2h) shade bank offset
	array<int> tex = [];     // 64x64 texel levels 0..15
	int tw = 0;
	float tt = 0.0;
	float travel = 0.0;
	float spin = 0.0;

	void build() override {
		name = "TUNNEL";
		handle = "down the wormhole . by lookup table";
		tw = w * 2;
		int th = h * 2;
		float rmax = w * 0.62;
		for (int y = 0; y < th; ++y) {
			for (int x = 0; x < tw; ++x) {
				float dx = x - w + 0.5;
				float dy = y - h + 0.5;
				float r = math::sqrt(dx * dx + dy * dy);
				r = r < 1.0 ? 1.0 : r;
				ang.push(itrunc((math::atan2(dy, dx) + 3.14159265) * 40.7436654) & 255);
				dep.push(itrunc(2600.0 / r));
				int s = itrunc(r * 12.0 / rmax);
				fog.push((s > 11 ? 11 : s) * 16);
			}
		}
		// 16 texel levels: steel plate ramp 0..11, neon 12..15; 12 fog banks
		for (int s2 = 0; s2 < 12; ++s2) {
			float f = s2 / 11.0;
			float k = f * f * (3.0 - 2.0 * f);
			for (int lv = 0; lv < 16; ++lv) {
				float r0 = 0.0; float g0 = 0.0; float b0 = 0.0;
				if (lv < 12) {
					float m = lv / 11.0;
					r0 = 18.0 + 120.0 * m; g0 = 26.0 + 150.0 * m; b0 = 48.0 + 190.0 * m;
				} else {
					float m2 = (lv - 11) / 4.0;
					r0 = 120.0 + 135.0 * m2; g0 = 40.0 + 160.0 * m2 * m2; b0 = 200.0 + 55.0 * m2;
				}
				// far walls dissolve into the glow at the tunnel's end
				float glow = lv >= 12 ? 0.45 + 0.55 * k : k;
				float haze = (1.0 - k) * (1.0 - k);
				pal.add(r0 * glow + 255.0 * haze, g0 * glow + 150.0 * haze, b0 * glow + 120.0 * haze);
			}
		}
		for (int v = 0; v < 64; ++v) {
			for (int u = 0; u < 64; ++u) {
				int row = v / 16;
				int bu = (u + (row % 2) * 8) % 16;
				int bv = v % 16;
				int plate = ((u + (row % 2) * 8) / 16 + row * 5) % 7;
				int lv = 5 + plate % 4;
				if (bu == 0 || bv == 0) { lv = 1; }
				else if (bu == 1 || bv == 1) { lv += 3; }
				else if (bu == 15 || bv == 15) { lv -= 3; }
				if ((bu == 3 || bu == 12) && (bv == 3 || bv == 12)) { lv = 11; }
				if (v >= 28 && v < 36) {
					int d = v < 32 ? v - 28 : 35 - v;
					lv = 12 + d;
				}
				tex.push(lv < 0 ? 0 : lv);
			}
		}
	}

	void update(float t, float dt) override {
		tt = t;
		float sp = 34.0 + 22.0 * math::sin(t * 0.37);
		travel += dt * sp;
		spin += dt * (0.9 * math::sin(t * 0.29));
	}

	void render(var& grid) override {
		int ww = w;
		int hh = h;
		int tww = tw;
		var& an = ang;
		var& de = dep;
		var& fo = fog;
		var& tx = tex;
		int ox = itrunc(w * 0.5 + w * 0.34 * math::sin(tt * 0.53));
		int oy = itrunc(h * 0.5 + h * 0.30 * math::sin(tt * 0.71 + 0.9));
		int mv = itrunc(travel);
		int rot = itrunc(spin * 40.0) + 4096;
		int twist = itrunc(6.0 + 5.0 * math::sin(tt * 0.23));
		int idx = 0;
		for (int y = 0; y < hh; ++y) {
			int trow = (y + oy) * tww + ox;
			for (int x = 0; x < ww; ++x) {
				int ti = trow + x;
				int d = de[ti];
				int u = (an[ti] + rot + ((d * twist) >> 5)) & 63;
				int v = (d + mv) & 63;
				grid[idx] = fo[ti] + tx[v * 64 + u];
				++idx;
			}
		}
	}
}
