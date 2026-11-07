// PIPEDREAM — the 3D Pipes screensaver homage, tuned for authenticity: thick
// round pipes, glossy elbow balls at every turn, one segment per fixed tick
// (patient, steady, inevitable), pipes never intersect (occupancy grid; a
// blocked pipe turns or terminates and a fresh pipe respawns), and a camera
// that holds STILL for a whole generation — fill to threshold, brief hold, a
// CRT-style wipe, then reseed with a NEW viewing angle, like the real thing.
// Every tube is a chain of lit sphere impostors a quarter voxel apart: each
// pixel of a splat derives its normal from the disc's bulge (diffuse, a sharp
// specular, distance fog) and nudges its depth by it, so crossings intersect.
//
// The static camera is also the perf trick: the screen image is persistent
// (pgrid/pz fields), and each frame only the voxels grown since the last frame
// are projected and splatted (~a dozen), not the whole volume. Canon demands
// the teapot: ~2% of joints instead spawn the jackodile's eye, a
// rainbow-flickering ball (re-splatted per frame so it shimmers).

class Pipes : Scene {
	array<int> cells = [];   // 0 empty, else 1 + pipe*3 + kind (0 seg, 1 joint, 2 eye)
	array<int> pgrid = [];   // persistent projected frame (palette indices)
	array<float> pz = [];    // persistent depth buffer
	array<int> pending = []; // voxels grown since the last render
	array<int> eggs = [];    // jackodile eyes, re-splatted every frame
	array<int> hx = [];      // pipe heads
	array<int> hy = [];
	array<int> hz = [];
	array<int> hd = [];
	array<int> DX = [1, -1, 0, 0, 0, 0];   // dirs paired so (d ^ 1) is the reverse
	array<int> DY = [0, 0, 1, -1, 0, 0];
	array<int> DZ = [0, 0, 0, 0, 1, -1];
	int gw = 16;
	int gh = 12;
	int gd = 16;
	int npipes = 4;
	int pipe_base = 0;
	int egg_base = 0;
	int filled = 0;
	int cap = 0;
	int phase = 0;       // 0 grow, 1 hold, 2 wipe
	float phase_t = 0.0;
	int last_wipe = 0;
	float step_acc = 0.0;
	float tt = 0.0;
	float gen_ang = 0.0; // camera angle for this generation (new one each reset)
	float gen_tilt = 0.34;
	float f_ca = 1.0;
	float f_sa = 0.0;
	float f_ct = 1.0;
	float f_st = 0.0;
	int flick = 0;

	void build() override {
		name = "PIPEDREAM";
		handle = "screensaver seance . watch for the jackodile";
		cells = gfx.make_grid(gw * gh * gd);
		pgrid = gfx.make_grid(w * h);
		pz = gfx.make_fgrid(w * h);
		cap = gw * gh * gd * 34 / 100;
		// the office monitor at 2am: one entry per row
		for (int y = 0; y < h; ++y) {
			float f = y / (h - 1.0);
			pal.add(3.0 + 6.0 * f, 4.0 + 5.0 * f, 12.0 + 14.0 * f);
		}
		for (int q = 0; q < w * h; ++q) { pgrid[q] = q / w; }
		pipe_base = h;
		var hues = [[70.0, 225.0, 195.0], [255.0, 178.0, 58.0], [232.0, 88.0, 212.0], [128.0, 158.0, 255.0]];
		for (int p = 0; p < npipes; ++p) {
			var hue = hues[p];
			shade_ramp(hue[0], hue[1], hue[2]);
		}
		egg_base = pal.size();
		for (int e = 0; e < 6; ++e) {
			var c = gfx.rainbow_rgb(e * 1.0471975512);
			shade_ramp(c[0], c[1], c[2]);
		}
		set_view(rng.next(628) * 0.01, 0.26 + rng.next(20) * 0.01);
		for (int p2 = 0; p2 < npipes; ++p2) {
			hx.push(0);
			hy.push(0);
			hz.push(0);
			hd.push(0);
			respawn(p2);
		}
	}

	// 16 levels: shadow -> full color -> white-hot highlight
	void shade_ramp(float r, float g, float b) {
		for (int s = 0; s < 16; ++s) {
			float f = s / 15.0;
			float k = f < 0.8 ? 0.06 + f * 1.18 : 1.0;
			float wh = f < 0.8 ? 0.0 : (f - 0.8) * 4.0;
			pal.add(r * k + (255.0 - r * k) * wh, g * k + (255.0 - g * k) * wh, b * k + (255.0 - b * k) * wh);
		}
	}

	void set_view(float ang, float tilt) {
		gen_ang = ang;
		gen_tilt = tilt;
		f_ca = math::cos(ang);
		f_sa = math::sin(ang);
		f_ct = math::cos(tilt);
		f_st = math::sin(tilt);
	}

	int cell_idx(int x, int y, int z) {
		return (y * gd + z) * gw + x;
	}

	bool is_free(int x, int y, int z) {
		if (x < 0 || x >= gw || y < 0 || y >= gh || z < 0 || z >= gd) { return false; }
		return cells[cell_idx(x, y, z)] == 0;
	}

	void advance(int p, int nx, int ny, int nz) {
		hx[p] = nx;
		hy[p] = ny;
		hz[p] = nz;
		int ci = cell_idx(nx, ny, nz);
		cells[ci] = 1 + p * 3;
		pending.push(ci);
		++filled;
	}

	void respawn(int p) {
		for (int tr = 0; tr < 30; ++tr) {
			int x = rng.next(gw);
			int y = rng.next(gh);
			int z = rng.next(gd);
			int ci = cell_idx(x, y, z);
			if (cells[ci] == 0) {
				hx[p] = x;
				hy[p] = y;
				hz[p] = z;
				hd[p] = rng.next(6);
				cells[ci] = 1 + p * 3;
				pending.push(ci);
				++filled;
				return;
			}
		}
	}

	void step_one(int p) {
		int x = hx[p];
		int y = hy[p];
		int z = hz[p];
		int d = hd[p];
		bool straight_ok = is_free(x + DX[d], y + DY[d], z + DZ[d]);
		if (straight_ok && rng.next(100) < 72) {
			advance(p, x + DX[d], y + DY[d], z + DZ[d]);
			return;
		}
		int nd = -1;
		for (int tr = 0; tr < 8; ++tr) {
			int cand = rng.next(6);
			if (cand == d || cand == (d ^ 1)) { continue; }
			if (is_free(x + DX[cand], y + DY[cand], z + DZ[cand])) { nd = cand; break; }
		}
		if (nd < 0 && straight_ok) {
			advance(p, x + DX[d], y + DY[d], z + DZ[d]);
			return;
		}
		if (nd < 0) {
			for (int cand2 = 0; cand2 < 6; ++cand2) {
				if (cand2 == (d ^ 1)) { continue; }
				if (is_free(x + DX[cand2], y + DY[cand2], z + DZ[cand2])) { nd = cand2; break; }
			}
		}
		if (nd < 0) {
			respawn(p);   // terminated: a fresh pipe takes over elsewhere
			return;
		}
		// a turn: the head cell becomes an elbow ball — or, rarely, the eye
		int here = cell_idx(x, y, z);
		if (rng.chance(2)) {
			cells[here] = 3 + p * 3;
			eggs.push(here);
		} else {
			cells[here] = 2 + p * 3;
		}
		pending.push(here);
		hd[p] = nd;
		advance(p, x + DX[nd], y + DY[nd], z + DZ[nd]);
	}

	void update(float t, float dt) override {
		tt = t;
		if (phase == 0) {
			step_acc += dt;
			while (phase == 0 && step_acc >= 0.03) {
				step_acc -= 0.03;
				for (int p = 0; p < npipes; ++p) { step_one(p); }
				if (filled >= cap) { phase = 1; phase_t = 0.0; step_acc = 0.0; }
			}
		} else if (phase == 1) {
			phase_t += dt;
			if (phase_t > 1.8) { phase = 2; phase_t = 0.0; }
		} else {
			phase_t += dt;
			if (phase_t > 1.5) { reset_volume(); }
		}
	}

	void reset_volume() {
		for (auto& c : cells) { c = 0; }
		int nn = w * h;
		for (int q = 0; q < nn; ++q) { pgrid[q] = q / w; pz[q] = 0.0; }
		pending.clear();
		eggs.clear();
		filled = 0;
		phase = 0;
		phase_t = 0.0;
		last_wipe = 0;
		step_acc = 0.0;
		set_view(rng.next(628) * 0.01, 0.26 + rng.next(20) * 0.01);   // the canon beat: a new angle
		for (int p = 0; p < npipes; ++p) { respawn(p); }
	}

	// one lit sphere impostor into the persistent frame: per pixel the disc's
	// bulge gives a normal (diffuse + specular) and a depth nudge, so tubes read
	// round and crossing pipes intersect cleanly; joints are fatter balls
	void splat_at(float fx, float fy, float fz, int c) {
		float rx = fx * f_ca + fz * f_sa;
		float rz = fz * f_ca - fx * f_sa;
		float ry = fy * f_ct - rz * f_st;
		float zcam = fy * f_st + rz * f_ct + 30.0;
		float ooz = 1.0 / zcam;
		int kind = (c - 1) % 3;
		int p = (c - 1) / 3;
		float scale = (w < h * 1.3 ? w : h * 1.3) * 1.25 * ooz;
		float rad = scale * (kind == 0 ? 0.40 : 0.58);
		rad = rad < 0.7 ? 0.7 : rad;
		float sx = w * 0.5 + rx * scale;
		float sy = h * 0.52 - ry * scale;
		float fog = 1.25 - zcam * 0.018;
		fog = fog > 1.0 ? 1.0 : (fog < 0.35 ? 0.35 : fog);
		int base = kind == 2 ? egg_base + ((flick + p + itrunc(fx * 3.0 + fz + 40.0)) % 6) * 16 : pipe_base + p * 16;
		int x0 = itrunc(sx - rad);
		int x1 = itrunc(sx + rad) + 1;
		int y0 = itrunc(sy - rad);
		int y1 = itrunc(sy + rad) + 1;
		float ir = 1.0 / rad;
		float r2 = rad * rad;
		float dz_gain = ooz * 0.02;
		var& pg = pgrid;
		var& zz = pz;
		for (int yy = y0; yy <= y1; ++yy) {
			if (yy < 0 || yy >= h) { continue; }
			float dy = yy + 0.5 - sy;
			int rowb = yy * w;
			for (int xx = x0; xx <= x1; ++xx) {
				if (xx < 0 || xx >= w) { continue; }
				float dx = xx + 0.5 - sx;
				float q = r2 - dx * dx - dy * dy;
				if (q < 0.0) { continue; }
				float nz = math::sqrt(q) * ir;
				float depth = ooz + nz * dz_gain;
				if (depth < zz[rowb + xx]) { continue; }
				float nx = dx * ir;
				float ny = dy * ir;
				float d = nx * -0.48 + ny * -0.56 + nz * 0.67;
				d = d < 0.0 ? 0.0 : d;
				float s = nx * -0.25 + ny * -0.29 + nz * 0.92;
				s = s < 0.0 ? 0.0 : s;
				float s2 = s * s;
				float s4 = s2 * s2;
				float s8 = s4 * s4;
				float lit = (0.10 + d * 0.78) * fog + s8 * s8 * (kind == 0 ? 0.5 : 0.8);
				int lv = itrunc(lit * 15.99);
				zz[rowb + xx] = depth;
				pg[rowb + xx] = base + (lv > 15 ? 15 : lv);
			}
		}
	}

	// splat one voxel plus midpoints toward every same-pipe neighbor
	// (midpoints are what keep the tube reading as one solid run)
	void splat_voxel(int vidx) {
		int c = cells[vidx];
		if (c == 0) { return; }
		int ix = vidx % gw;
		int rest = vidx / gw;
		int iz = rest % gd;
		int iy = rest / gd;
		float fx = ix - (gw - 1) * 0.5;
		float fy = iy - (gh - 1) * 0.5;
		float fz = iz - (gd - 1) * 0.5;
		splat_at(fx, fy, fz, c);
		int pp = (c - 1) / 3;
		int seg = 1 + pp * 3;
		if (ix > 0) {
			int n0 = cells[vidx - 1];
			if (n0 != 0 && (n0 - 1) / 3 == pp) { splat_at(fx - 0.25, fy, fz, seg); splat_at(fx - 0.5, fy, fz, seg); }
		}
		if (ix < gw - 1) {
			int n1 = cells[vidx + 1];
			if (n1 != 0 && (n1 - 1) / 3 == pp) { splat_at(fx + 0.25, fy, fz, seg); splat_at(fx + 0.5, fy, fz, seg); }
		}
		if (iz > 0) {
			int n2 = cells[vidx - gw];
			if (n2 != 0 && (n2 - 1) / 3 == pp) { splat_at(fx, fy, fz - 0.25, seg); splat_at(fx, fy, fz - 0.5, seg); }
		}
		if (iz < gd - 1) {
			int n3 = cells[vidx + gw];
			if (n3 != 0 && (n3 - 1) / 3 == pp) { splat_at(fx, fy, fz + 0.25, seg); splat_at(fx, fy, fz + 0.5, seg); }
		}
		if (iy > 0) {
			int n4 = cells[vidx - gw * gd];
			if (n4 != 0 && (n4 - 1) / 3 == pp) { splat_at(fx, fy - 0.25, fz, seg); splat_at(fx, fy - 0.5, fz, seg); }
		}
		if (iy < gh - 1) {
			int n5 = cells[vidx + gw * gd];
			if (n5 != 0 && (n5 - 1) / 3 == pp) { splat_at(fx, fy + 0.25, fz, seg); splat_at(fx, fy + 0.5, fz, seg); }
		}
	}

	void render(var& grid) override {
		int nn = w * h;
		if (phase == 2) {
			// generation over: the monitor wipes to black, top down
			int target = itrunc(phase_t * h / 1.3);
			target = target > h ? h : target;
			int stop = target * w;
			for (int q = last_wipe * w; q < stop; ++q) { pgrid[q] = q / w; pz[q] = 0.0; }
			last_wipe = target;
		} else {
			flick = itrunc(tt * 8.0);
			for (auto v : pending) { splat_voxel(v); }
			pending.clear();
			for (auto e : eggs) { splat_voxel(e); }   // the eyes shimmer every frame
		}
		for (int i = 0; i < nn; ++i) { grid[i] = pgrid[i]; }
	}
}
