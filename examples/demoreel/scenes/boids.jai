// BOIDS — a starling murmuration at sunset, and one hungry hawk. Each boid is
// a script class instance (the object-y counterpoint to starfield's flat
// arrays), steering by the three classic rules plus wander and fear. They fly
// as silhouettes: the sky is a baked per-pixel tone (gradient + sun glow + the
// skyline), and every entry of the palette is a sky tone darkened by a level,
// so the birds and their fading wakes shade whatever sky they cross.

class Boid {
	float x = 0.0;
	float y = 0.0;
	float vx = 0.0;
	float vy = 0.0;
	float px = 0.0;
	float py = 0.0;
}

class Boids : Scene {
	var flock = [];
	array<int> trail = [];
	array<int> sky = [];
	int nb = 80;
	int tones = 32;
	float hx = 10.0;    // the hawk
	float hy = 10.0;
	float hvx = 8.0;
	float hvy = 0.0;
	float tt = 0.0;

	void build() override {
		name = "BOIDS";
		handle = "murmuration . by the vm crew";
		trail = gfx.make_grid(w * h);
		sky = gfx.make_grid(w * h);
		var stops = [[10.0, 12.0, 40.0], [48.0, 30.0, 90.0], [150.0, 60.0, 120.0], [240.0, 110.0, 90.0], [255.0, 190.0, 110.0], [255.0, 244.0, 200.0]];
		for (int t = 0; t < tones; ++t) {
			float f = t * 5.0 / (tones - 1.0);
			int k = itrunc(f);
			k = k > 4 ? 4 : k;
			float u = f - k;
			var a = stops[k];
			var b = stops[k + 1];
			float ar = a[0]; float ag = a[1]; float ab = a[2];
			float cr = ar + (b[0] - ar) * u;
			float cg = ag + (b[1] - ag) * u;
			float cb = ab + (b[2] - ab) * u;
			for (int d = 0; d < 8; ++d) {
				float k2 = 1.0 - d / 7.0 * 0.93;
				pal.add(cr * k2 + 6.0 * (1.0 - k2), cg * k2 + 4.0 * (1.0 - k2), cb * k2 + 10.0 * (1.0 - k2));
			}
		}
		// sky tones: height gradient + sun glow; the skyline is solid silhouette
		float sunx = w * 0.68;
		float suny = h * 0.80;
		float sr = h * 0.13;
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				float g = (y * 1.0 / h) * 9.0;
				float dx = x - sunx;
				float dy = (y - suny) * 1.3;
				float d = math::sqrt(dx * dx + dy * dy);
				float glow = d < sr ? 15.0 : 13.0 - (d - sr) * 0.22;
				float tone = glow > g ? glow : g;
				int it = itrunc(tone * 2.0);
				sky[y * w + x] = (it > 31 ? 31 : it) * 8;
			}
		}
		int hx0 = 0;
		while (hx0 < w) {
			int bw = 4 + rng.next(9);
			int bh = h / 14 + rng.next(h / 7);
			for (int x2 = hx0; x2 < hx0 + bw && x2 < w; ++x2) {
				for (int y2 = h - bh; y2 < h; ++y2) {
					bool lit = (x2 - hx0) % 3 == 1 && (y2 - (h - bh)) % 3 == 1 && rng.next(100) < 30;
					sky[y2 * w + x2] = lit ? 31 * 8 + 1 : 7;
				}
			}
			hx0 += bw + rng.next(3);
		}
		for (int i = 0; i < nb; ++i) {
			var b2 = Boid();
			b2.x = w * 0.3 + rng.next(w * 4) * 0.1;
			b2.y = h * 0.2 + rng.next(h * 4) * 0.1;
			float a2 = rng.next(628) * 0.01;
			b2.vx = math::cos(a2) * 12.0;
			b2.vy = math::sin(a2) * 12.0;
			b2.px = b2.x;
			b2.py = b2.y;
			flock.push(b2);
		}
	}

	void update(float t, float dt) override {
		tt = t;
		if (dt <= 0.0) { return; }
		// wander target keeps the flock touring the sky, above the rooftops
		float wx = w * (0.5 + 0.36 * math::sin(t * 0.37));
		float wy = h * (0.38 + 0.2 * math::sin(t * 0.53 + 1.7));
		hawk_update(dt);
		for (int i = 0; i < nb; ++i) {
			steer_one(i, wx, wy, dt);
		}
	}

	void hawk_update(float dt) {
		float cx = 0.0;
		float cy = 0.0;
		for (int i = 0; i < nb; ++i) {
			var& b = flock[i];
			cx += b.x;
			cy += b.y;
		}
		cx /= nb;
		cy /= nb;
		hvx += (cx - hx) * 0.6 * dt;
		hvy += (cy - hy) * 0.6 * dt;
		float psp = math::sqrt(hvx * hvx + hvy * hvy);
		if (psp > 15.0) { hvx *= 15.0 / psp; hvy *= 15.0 / psp; }
		hx += hvx * dt;
		hy += hvy * dt;
		if (hx < 3.0) { hx = 3.0; hvx = 0.0 - hvx; }
		if (hx > w - 4.0) { hx = w - 4.0; hvx = 0.0 - hvx; }
		if (hy < 3.0) { hy = 3.0; hvy = 0.0 - hvy; }
		if (hy > h * 0.72) { hy = h * 0.72; hvy = 0.0 - hvy; }
	}

	// classic three rules + wander + hawk fear, one boid per call
	void steer_one(int i, float wx, float wy, float dt) {
		var& b = flock[i];
		float bx = b.x;
		float by = b.y;
		float ax = 0.0;
		float ay = 0.0;
		float mx = 0.0;
		float my = 0.0;
		float vx_sum = 0.0;
		float vy_sum = 0.0;
		int near = 0;
		for (int j = 0; j < nb; ++j) {
			if (j == i) { continue; }
			var& o = flock[j];
			float dx = o.x - bx;
			float dy = o.y - by;
			float d2 = dx * dx + dy * dy;
			if (d2 > 220.0) { continue; }
			++near;
			mx += o.x;
			my += o.y;
			vx_sum += o.vx;
			vy_sum += o.vy;
			if (d2 < 10.0 && d2 > 0.0001) {
				ax -= dx / d2 * 26.0;
				ay -= dy / d2 * 26.0;
			}
		}
		if (near > 0) {
			ax += (mx / near - bx) * 1.1;             // cohesion
			ay += (my / near - by) * 1.1;
			ax += (vx_sum / near - b.vx) * 1.7;       // alignment
			ay += (vy_sum / near - b.vy) * 1.7;
		}
		ax += (wx - bx) * 0.45;                       // wander
		ay += (wy - by) * 0.45;
		if (bx < 10.0) { ax += (10.0 - bx) * 6.0; }   // soft walls
		if (bx > w - 10.0) { ax -= (bx - w + 10.0) * 6.0; }
		if (by < 8.0) { ay += (8.0 - by) * 6.0; }
		if (by > h * 0.66) { ay -= (by - h * 0.66) * 6.0; }
		float fx = bx - hx;                           // flee the hawk
		float fy = by - hy;
		float fd2 = fx * fx + fy * fy;
		if (fd2 < 260.0 && fd2 > 0.001) {
			ax += fx / fd2 * 240.0;
			ay += fy / fd2 * 240.0;
		}
		b.vx += ax * dt;
		b.vy += ay * dt;
		float sp = math::sqrt(b.vx * b.vx + b.vy * b.vy);
		if (sp > 24.0) { b.vx *= 24.0 / sp; b.vy *= 24.0 / sp; }
		if (sp < 7.0 && sp > 0.001) { b.vx *= 7.0 / sp; b.vy *= 7.0 / sp; }
		b.px = bx;
		b.py = by;
		b.x += b.vx * dt;
		b.y += b.vy * dt;
		if (b.x < 0.0) { b.x = 0.0; b.vx = 0.0 - b.vx; }
		if (b.x > w - 1.0) { b.x = w - 1.0; b.vx = 0.0 - b.vx; }
		if (b.y < 0.0) { b.y = 0.0; b.vy = 0.0 - b.vy; }
		if (b.y > h * 0.82) { b.y = h * 0.82; b.vy = 0.0 - b.vy * 0.5; }
	}

	void mark(int x, int y, int lv) {
		if (x < 0 || x >= w || y < 0 || y >= h) { return; }
		int i = y * w + x;
		if (trail[i] < lv) { trail[i] = lv; }
	}

	void render(var& grid) override {
		int n = w * h;
		var& tr = trail;
		var& sk = sky;
		bool fade = (itrunc(tt * 30.0) & 1) == 0;
		for (int i = 0; i < n; ++i) {
			int tv = tr[i];
			if (fade && tv > 0) { --tv; tr[i] = tv; }
			int s = sk[i];
			int dk = s % 8;
			grid[i] = s + (tv > dk ? tv - dk : 0);
		}
		// wakes: each bird draws the segment it flew this frame into the trail
		for (int k = 0; k < nb; ++k) {
			var& b = flock[k];
			float ex = b.x;
			float ey = b.y;
			float sx = b.px;
			float sy = b.py;
			mark(itrunc(sx), itrunc(sy), 3);
			mark(itrunc((sx + ex) * 0.5), itrunc((sy + ey) * 0.5), 4);
			mark(itrunc(ex), itrunc(ey), 5);
		}
		// the birds themselves, darkest, one pixel ahead along the heading
		for (int k2 = 0; k2 < nb; ++k2) {
			var& b2 = flock[k2];
			int bx = itrunc(b2.x);
			int by = itrunc(b2.y);
			float sp = math::sqrt(b2.vx * b2.vx + b2.vy * b2.vy) + 0.001;
			int nx = itrunc(b2.x + b2.vx / sp * 1.2);
			int ny = itrunc(b2.y + b2.vy / sp * 1.2);
			if (bx >= 0 && bx < w && by >= 0 && by < h) { grid[by * w + bx] = sk[by * w + bx] - sk[by * w + bx] % 8 + 7; }
			if (nx >= 0 && nx < w && ny >= 0 && ny < h) { grid[ny * w + nx] = sk[ny * w + nx] - sk[ny * w + nx] % 8 + 6; }
		}
		// the hawk: a wide V with flapping wingtips, and a red eye
		int hxi = itrunc(hx);
		int hyi = itrunc(hy);
		int flap = (itrunc(tt * 7.0) % 4);
		int tip = flap == 0 ? -2 : (flap == 2 ? 1 : 0);
		for (int s2 = -4; s2 <= 4; ++s2) {
			int as = s2 < 0 ? 0 - s2 : s2;
			int yy = hyi + (as <= 1 ? 0 : (as <= 3 ? (tip < 0 ? -1 : 0) : tip));
			int xx = hxi + s2;
			if (xx >= 0 && xx < w && yy >= 0 && yy < h) {
				int gi = yy * w + xx;
				grid[gi] = sk[gi] - sk[gi] % 8 + 7;
			}
		}
		if (hyi + 1 < h && hxi >= 0 && hxi < w) {
			int gb = (hyi + 1) * w + hxi;
			grid[gb] = sk[gb] - sk[gb] % 8 + 7;
		}
	}
}
