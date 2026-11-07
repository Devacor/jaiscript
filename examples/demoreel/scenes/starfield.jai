// STARFIELD — a warp run. A periodic value-noise nebula (grown once at build,
// scrolled with wrap masks), 700 stars in four color temperatures, each drawn
// as a motion streak from where it was to where it is, tail dim to head bright.
// The palette is ADDITIVE: every entry is nebula level + star light, so a star
// over a bright cloud glows brighter instead of punching a hole. The ship
// cruises, spools up, jumps to warp (streaks stretch into a tunnel, the view
// rolls), and drops out in a flash. Flat parallel arrays on purpose: the
// hot-loop counterpoint to boids.jai's class-per-particle style.

class Starfield : Scene {
	array<float> sx = [];
	array<float> sy = [];
	array<float> sz = [];
	array<int> temp = [];
	array<int> neb = [];     // 128x128 nebula levels, wrap-tiled
	int nstars = 700;
	int nlev = 20;           // nebula levels
	int per = 33;            // entries per nebula level: 1 bare + 4 temps x 8
	float tt = 0.0;
	float speed = 0.3;
	float roll = 0.0;
	float flash = 0.0;

	void build() override {
		name = "STARFIELD";
		handle = "warp run . greets: parser crew";
		var stops = [[1.0, 1.0, 6.0], [10.0, 6.0, 30.0], [40.0, 12.0, 60.0], [84.0, 22.0, 86.0], [36.0, 70.0, 110.0], [120.0, 150.0, 190.0]];
		var temps = [[150.0, 180.0, 255.0], [255.0, 255.0, 255.0], [255.0, 236.0, 170.0], [255.0, 170.0, 110.0]];
		for (int l = 0; l < nlev; ++l) {
			float f = l * 5.0 / (nlev - 1.0);
			int k = itrunc(f);
			k = k > 4 ? 4 : k;
			float u = f - k;
			var a = stops[k];
			var b = stops[k + 1];
			float ar = a[0]; float ag = a[1]; float ab = a[2];
			float nr = ar + (b[0] - ar) * u;
			float ng = ag + (b[1] - ag) * u;
			float nb = ab + (b[2] - ab) * u;
			pal.add(nr, ng, nb);
			for (int tc = 0; tc < 4; ++tc) {
				var c = temps[tc];
				float cr = c[0]; float cg = c[1]; float cb = c[2];
				for (int lv = 0; lv < 8; ++lv) {
					float s = (lv + 1) / 8.0;
					s *= 0.6 + 0.4 * s;
					pal.add(nr + cr * s, ng + cg * s, nb + cb * s);
				}
			}
		}
		grow_nebula();
		for (int i = 0; i < nstars; ++i) {
			sx.push((rng.next(2000) - 1000) * 0.0016);
			sy.push((rng.next(2000) - 1000) * 0.0016);
			sz.push(0.05 + rng.next(1000) * 0.0011);
			int r = rng.next(100);
			temp.push(r < 30 ? 0 : (r < 70 ? 1 : (r < 90 ? 2 : 3)));
		}
	}

	// three octaves of periodic value noise, squared for wispy contrast
	void grow_nebula() {
		array<int> lat = [];
		int sd = seed & 65535;
		for (int i = 0; i < 32 * 32; ++i) {
			int hs = (i * 73856093 + sd * 19349663) & 1048575;
			lat.push(((hs * 1274126177) >> 12) & 255);
		}
		for (int y = 0; y < 128; ++y) {
			for (int x = 0; x < 128; ++x) {
				int acc = 0;
				int cell = 32;
				int n = 4;
				int wgt = 8;
				for (int o = 0; o < 3; ++o) {
					int ix = x / cell;
					int iy = y / cell;
					int fx = (x % cell) * 256 / cell;
					int fy = (y % cell) * 256 / cell;
					int ix1 = (ix + 1) % n;
					int iy1 = (iy + 1) % n;
					int base = o * 5;
					int a = lat[((iy + base) & 31) * 32 + ((ix + base * 3) & 31)];
					int b = lat[((iy + base) & 31) * 32 + ((ix1 + base * 3) & 31)];
					int c = lat[((iy1 + base) & 31) * 32 + ((ix + base * 3) & 31)];
					int d = lat[((iy1 + base) & 31) * 32 + ((ix1 + base * 3) & 31)];
					int top = a + (b - a) * fx / 256;
					int bot = c + (d - c) * fx / 256;
					acc += (top + (bot - top) * fy / 256) * wgt;
					cell = cell / 2;
					n *= 2;
					wgt = wgt / 2;
				}
				int v = acc / 14;                 // 0..255
				v -= 60;
				v = v < 0 ? 0 : v;
				v = v * v / 110;
				v = v * (nlev - 1) / 255;
				neb.push(v > nlev - 1 ? nlev - 1 : v);
			}
		}
	}

	void update(float t, float dt) override {
		tt = t;
		// cruise -> spool -> warp -> drop out
		float target = 0.3;
		if (t > 2.5) { target = 0.3 + (t - 2.5) * 0.5; }
		if (t > 6.0) { target = 2.1 + (t - 6.0) * 1.1; }
		if (t > 8.5) { target = 0.35; }
		target = target > 3.6 ? 3.6 : target;
		float mixv = dt * (t > 8.5 ? 6.0 : 1.5);
		mixv = mixv > 1.0 ? 1.0 : mixv;
		speed += (target - speed) * mixv;
		flash = t > 8.5 && t < 9.4 ? 1.0 - (t - 8.5) / 0.9 : 0.0;
		roll += dt * (0.05 + speed * 0.07);
		for (int i = 0; i < nstars; ++i) {
			float z = sz[i] - dt * speed * 0.9;
			if (z < 0.03) {
				sx[i] = (rng.next(2000) - 1000) * 0.0016;
				sy[i] = (rng.next(2000) - 1000) * 0.0016;
				z = 1.1 + rng.next(1000) * 0.0003;
			}
			sz[i] = z;
		}
	}

	void render(var& grid) override {
		int ww = w;
		int hh = h;
		int pr = per;
		var& nb = neb;
		int boost = itrunc(flash * 12.0);
		int ox = itrunc(tt * 3.0);
		int oy = itrunc(tt * 1.3);
		int nl1 = nlev - 1;
		int idx = 0;
		for (int y = 0; y < hh; ++y) {
			int row = ((y + oy) & 127) * 128;
			for (int x = 0; x < ww; ++x) {
				int l = nb[row + ((x + ox) & 127)] + boost;
				grid[idx] = (l > nl1 ? nl1 : l) * pr;
				++idx;
			}
		}
		float cx = ww * 0.5;
		float cy = hh * 0.5;
		float f = ww * 0.45;
		float ca = math::cos(roll);
		float sa = math::sin(roll);
		float trail = 0.02 + speed * 0.05;
		var& xs = sx;
		var& ys = sy;
		var& zs = sz;
		var& tp = temp;
		for (int i = 0; i < nstars; ++i) {
			float z = zs[i];
			float rx = xs[i] * ca - ys[i] * sa;
			float ry = xs[i] * sa + ys[i] * ca;
			float hx = cx + rx / z * f;
			float hy = cy + ry / z * f;
			if (hx < 0.0 || hx >= ww || hy < 0.0 || hy >= hh) { continue; }
			float zb = z + trail;
			float tx = cx + rx / zb * f;
			float ty = cy + ry / zb * f;
			int lvl = itrunc(8.8 - z * 7.0);
			lvl = lvl < 0 ? 0 : (lvl > 7 ? 7 : lvl);
			int tbase = 1 + tp[i] * 8;
			float ddx = hx - tx;
			float ddy = hy - ty;
			float adx = ddx < 0.0 ? -ddx : ddx;
			float ady = ddy < 0.0 ? -ddy : ddy;
			int n = itrunc(adx > ady ? adx : ady);
			n = n > 48 ? 48 : n;
			for (int k = 0; k <= n; ++k) {
				float u = n > 0 ? k * 1.0 / n : 1.0;
				int px = itrunc(tx + ddx * u);
				int py = itrunc(ty + ddy * u);
				if (px < 0 || px >= ww || py < 0 || py >= hh) { continue; }
				int sl = itrunc(lvl * (0.25 + 0.75 * u));
				int gi = py * ww + px;
				int cur = grid[gi];
				int bare = cur - cur % pr;
				int had = cur % pr;
				int had_l = had > 0 ? (had - 1) % 8 : -1;
				if (sl > had_l) { grid[gi] = bare + tbase + sl; }
			}
			// near stars bloom: a dim cross around the head
			if (lvl >= 6) {
				int hxi = itrunc(hx);
				int hyi = itrunc(hy);
				for (int q = 0; q < 4; ++q) {
					int gx = hxi + (q == 0 ? 1 : (q == 1 ? -1 : 0));
					int gy = hyi + (q == 2 ? 1 : (q == 3 ? -1 : 0));
					if (gx < 0 || gx >= ww || gy < 0 || gy >= hh) { continue; }
					int gi2 = gy * ww + gx;
					int cur2 = grid[gi2];
					int had2 = cur2 % pr;
					int hl2 = had2 > 0 ? (had2 - 1) % 8 : -1;
					if (lvl - 4 > hl2) { grid[gi2] = cur2 - had2 + tbase + lvl - 4; }
				}
			}
		}
	}
}
