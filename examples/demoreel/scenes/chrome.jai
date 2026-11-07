// CHROME — the Amiga Juggler's grandchild: a ray tracer, one ray per pixel,
// its rows traced across every core with parallel_transform. Three chrome
// spheres bounce on a checkerboard that runs to a hazy horizon; every primary
// ray is intersected against the spheres and the floor, sphere hits spawn a
// reflection ray (which sees the sky, the floor, or another sphere's lit
// surface), floor hits cast a shadow ray at the sun. The trick that makes a
// ray tracer fit a palette: ch_trace() returns what a ray SAW as a class (a
// sky level, a floor tile x fog x shadow, a lit sphere shade), and a chrome
// sphere's pixel is that class looked up in its own tinted copy of the class
// table, plus a white-hot sun glint.
//
// The row function reads its frame through captured globals (scalars copy
// into each worker, the index array borrows), calls nothing but script and
// sqrt, and writes only its own row: parallel_transform's contract. The
// spheres are unrolled into scalars because an array read costs more than
// the arithmetic it feeds.

float CH_X0 = 0.0; float CH_Y0 = 0.0; float CH_Z0 = 0.0; float CH_R0 = 0.9;
float CH_X1 = 0.0; float CH_Y1 = 0.0; float CH_Z1 = 0.0; float CH_R1 = 0.7;
float CH_X2 = 0.0; float CH_Y2 = 0.0; float CH_Z2 = 0.0; float CH_R2 = 0.55;
float CH_EX = 0.0; float CH_EY = 1.6; float CH_EZ = 5.0;
float CH_FX = 0.0; float CH_FY = 0.0; float CH_FZ = -1.0;
float CH_RX = 1.0; float CH_RZ = 0.0;
float CH_UX = 0.0; float CH_UY = 1.0; float CH_UZ = 0.0;
float CH_LX = 0.5; float CH_LY = 0.75; float CH_LZ = 0.43;
int CH_W = 0;
int CH_H = 0;
int CH_FLOOR = 0;
int CH_BALL = 0;
int CH_NBASE = 0;
int CH_GLINT = 0;
array<int> CH_ROWS = [];

// what a ray from (ox,oy,oz) along unit (dx,dy,dz) sees, as a class index;
// `skip` is the sphere it left
function ch_trace(float ox, float oy, float oz, float dx, float dy, float dz, int skip) -> int {
	float best = 1000000.0;
	int hit = -1;
	float hr = 1.0;
	float hx = 0.0; float hy = 0.0; float hz = 0.0;
	if (skip != 0) {
		float qx = ox - CH_X0; float qy = oy - CH_Y0; float qz = oz - CH_Z0;
		float b = qx * dx + qy * dy + qz * dz;
		float disc = b * b - (qx * qx + qy * qy + qz * qz - CH_R0 * CH_R0);
		if (disc > 0.0) {
			float t = 0.0 - b - math::sqrt(disc);
			if (t > 0.001 && t < best) { best = t; hit = 0; hr = CH_R0; hx = CH_X0; hy = CH_Y0; hz = CH_Z0; }
		}
	}
	if (skip != 1) {
		float qx1 = ox - CH_X1; float qy1 = oy - CH_Y1; float qz1 = oz - CH_Z1;
		float b1 = qx1 * dx + qy1 * dy + qz1 * dz;
		float disc1 = b1 * b1 - (qx1 * qx1 + qy1 * qy1 + qz1 * qz1 - CH_R1 * CH_R1);
		if (disc1 > 0.0) {
			float t1 = 0.0 - b1 - math::sqrt(disc1);
			if (t1 > 0.001 && t1 < best) { best = t1; hit = 1; hr = CH_R1; hx = CH_X1; hy = CH_Y1; hz = CH_Z1; }
		}
	}
	if (skip != 2) {
		float qx2 = ox - CH_X2; float qy2 = oy - CH_Y2; float qz2 = oz - CH_Z2;
		float b2 = qx2 * dx + qy2 * dy + qz2 * dz;
		float disc2 = b2 * b2 - (qx2 * qx2 + qy2 * qy2 + qz2 * qz2 - CH_R2 * CH_R2);
		if (disc2 > 0.0) {
			float t2 = 0.0 - b2 - math::sqrt(disc2);
			if (t2 > 0.001 && t2 < best) { best = t2; hit = 2; hr = CH_R2; hx = CH_X2; hy = CH_Y2; hz = CH_Z2; }
		}
	}
	if (hit >= 0) {
		float d = ((ox + dx * best - hx) * CH_LX + (oy + dy * best - hy) * CH_LY + (oz + dz * best - hz) * CH_LZ) / hr;
		d = d < 0.0 ? 0.0 : d;
		int lv = (0.2 + 0.8 * d) * 7.99;
		return CH_BALL + hit * 8 + lv;
	}
	if (dy < -0.0005) {
		float tf = (0.0 - oy) / dy;
		float fx = ox + dx * tf;
		float fz = oz + dz * tf;
		int ix = fx + 4096.0;
		int iz = fz + 4096.0;
		int fog = 11.5 - tf * 0.55;
		fog = fog < 0 ? 0 : (fog > 11 ? 11 : fog);
		// shadow: does the ray toward the sun pass through a sphere?
		int shade = 0;
		float sx = fx - CH_X0; float sz = fz - CH_Z0;
		float sb = sx * CH_LX - CH_Y0 * CH_LY + sz * CH_LZ;
		if (sb < 0.0 && sb * sb - (sx * sx + CH_Y0 * CH_Y0 + sz * sz - CH_R0 * CH_R0) > 0.0) { shade = 1; }
		float sx1 = fx - CH_X1; float sz1 = fz - CH_Z1;
		float sb1 = sx1 * CH_LX - CH_Y1 * CH_LY + sz1 * CH_LZ;
		if (sb1 < 0.0 && sb1 * sb1 - (sx1 * sx1 + CH_Y1 * CH_Y1 + sz1 * sz1 - CH_R1 * CH_R1) > 0.0) { shade = 1; }
		float sx2 = fx - CH_X2; float sz2 = fz - CH_Z2;
		float sb2 = sx2 * CH_LX - CH_Y2 * CH_LY + sz2 * CH_LZ;
		if (sb2 < 0.0 && sb2 * sb2 - (sx2 * sx2 + CH_Y2 * CH_Y2 + sz2 * sz2 - CH_R2 * CH_R2) > 0.0) { shade = 1; }
		return CH_FLOOR + (((ix + iz) & 1) * 2 + shade) * 12 + fog;
	}
	int s = dy * 30.0;
	return s < 0 ? 0 : (s > 23 ? 23 : s);
}

function ch_row(int y) -> array<int> {
	array<int> out = [];
	int ww = CH_W;
	float sc = 2.0 / ww;
	float v = (CH_H * 0.5 - y - 0.5) * sc;
	float ex = CH_EX; float ey = CH_EY; float ez = CH_EZ;
	float bx = CH_FX * 1.35 + CH_UX * v;
	float by = CH_FY * 1.35 + CH_UY * v;
	float bz = CH_FZ * 1.35 + CH_UZ * v;
	for (int x = 0; x < ww; ++x) {
		float u = (x + 0.5 - ww * 0.5) * sc;
		float dx = bx + CH_RX * u;
		float dy = by;
		float dz = bz + CH_RZ * u;
		float il = 1.0 / math::sqrt(dx * dx + dy * dy + dz * dz);
		dx *= il; dy *= il; dz *= il;
		float best = 1000000.0;
		int hit = -1;
		float cx = 0.0; float cy = 0.0; float cz = 0.0; float cr = 1.0;
		float qx = ex - CH_X0; float qy = ey - CH_Y0; float qz = ez - CH_Z0;
		float b = qx * dx + qy * dy + qz * dz;
		float disc = b * b - (qx * qx + qy * qy + qz * qz - CH_R0 * CH_R0);
		if (disc > 0.0) {
			float t = 0.0 - b - math::sqrt(disc);
			if (t > 0.001) { best = t; hit = 0; cx = CH_X0; cy = CH_Y0; cz = CH_Z0; cr = CH_R0; }
		}
		float qx1 = ex - CH_X1; float qy1 = ey - CH_Y1; float qz1 = ez - CH_Z1;
		float b1 = qx1 * dx + qy1 * dy + qz1 * dz;
		float disc1 = b1 * b1 - (qx1 * qx1 + qy1 * qy1 + qz1 * qz1 - CH_R1 * CH_R1);
		if (disc1 > 0.0) {
			float t1 = 0.0 - b1 - math::sqrt(disc1);
			if (t1 > 0.001 && t1 < best) { best = t1; hit = 1; cx = CH_X1; cy = CH_Y1; cz = CH_Z1; cr = CH_R1; }
		}
		float qx2 = ex - CH_X2; float qy2 = ey - CH_Y2; float qz2 = ez - CH_Z2;
		float b2 = qx2 * dx + qy2 * dy + qz2 * dz;
		float disc2 = b2 * b2 - (qx2 * qx2 + qy2 * qy2 + qz2 * qz2 - CH_R2 * CH_R2);
		if (disc2 > 0.0) {
			float t2 = 0.0 - b2 - math::sqrt(disc2);
			if (t2 > 0.001 && t2 < best) { best = t2; hit = 2; cx = CH_X2; cy = CH_Y2; cz = CH_Z2; cr = CH_R2; }
		}
		if (hit < 0) {
			out.push(ch_trace(ex, ey, ez, dx, dy, dz, -1));
			continue;
		}
		float hx = ex + dx * best;
		float hy = ey + dy * best;
		float hz = ez + dz * best;
		float nx = (hx - cx) / cr;
		float ny = (hy - cy) / cr;
		float nz = (hz - cz) / cr;
		float dn = 2.0 * (dx * nx + dy * ny + dz * nz);
		float mx = dx - nx * dn;
		float my = dy - ny * dn;
		float mz = dz - nz * dn;
		float g = mx * CH_LX + my * CH_LY + mz * CH_LZ;
		if (g > 0.975) {
			int gl = (g - 0.975) * 100.0;
			out.push(CH_GLINT + (gl > 2 ? 2 : gl));
		} else {
			out.push(CH_NBASE * (hit + 1) + ch_trace(hx, hy, hz, mx, my, mz, hit));
		}
	}
	return out;
}

class Chrome : Scene {
	array<float> rad = [0.9, 0.7, 0.55];
	array<float> bx = [0.0, 0.0, 0.0];
	array<float> by = [0.0, 0.0, 0.0];
	array<float> bz = [0.0, 0.0, 0.0];
	int nsky = 24;
	int nfog = 12;
	int floor0 = 0;          // 2 tiles x 2 (lit, shadow) x fog levels
	int ball0 = 0;           // 3 spheres x 8 lit shades (as seen in a reflection)
	int nbase = 0;
	int glint = 0;
	float ex = 0.0;
	float ey = 1.6;
	float ez = 5.0;
	float fx = 0.0;
	float fy = 0.0;
	float fz = -1.0;
	float rx = 1.0;
	float rz = 0.0;
	float ux = 0.0;
	float uy = 1.0;
	float uz = 0.0;
	float tt = 0.0;

	void build() override {
		name = "CHROME";
		handle = "juggler's grandchild . one ray per pixel, every core";
		var tints = [[1.0, 0.82, 0.45], [0.86, 0.9, 1.0], [1.0, 0.45, 0.55]];
		// the class table once plain, then once per sphere tint
		for (int copy = 0; copy < 4; ++copy) {
			float tr = 1.0; float tg = 1.0; float tb = 1.0;
			float k = 1.0;
			if (copy > 0) {
				var tc = tints[copy - 1];
				tr = tc[0]; tg = tc[1]; tb = tc[2];
				k = 0.86;
			}
			class_table(tr * k, tg * k, tb * k);
			if (copy == 0) { nbase = pal.size(); }
		}
		glint = pal.ramp(3, 255.0, 236.0, 190.0, 255.0, 255.0, 255.0);
	}

	void class_table(float tr, float tg, float tb) {
		for (int s = 0; s < nsky; ++s) {
			float f = s / (nsky - 1.0);
			pal.add((250.0 - 200.0 * f) * tr, (200.0 - 120.0 * f) * tg, (170.0 + 40.0 * f) * tb);
		}
		if (floor0 == 0) { floor0 = pal.size(); }
		var tiles = [[235.0, 226.0, 210.0], [150.0, 36.0, 60.0]];
		for (int tile = 0; tile < 2; ++tile) {
			var tc = tiles[tile];
			float r0 = tc[0]; float g0 = tc[1]; float b0 = tc[2];
			for (int sh = 0; sh < 2; ++sh) {
				float dk = sh == 1 ? 0.38 : 1.0;
				for (int f2 = 0; f2 < nfog; ++f2) {
					float k = f2 / (nfog - 1.0);
					pal.add((r0 * dk * k + 250.0 * (1.0 - k)) * tr, (g0 * dk * k + 200.0 * (1.0 - k)) * tg, (b0 * dk * k + 170.0 * (1.0 - k)) * tb);
				}
			}
		}
		if (ball0 == 0) { ball0 = pal.size(); }
		var balls = [[255.0, 200.0, 110.0], [210.0, 220.0, 240.0], [255.0, 110.0, 130.0]];
		for (int b = 0; b < 3; ++b) {
			var bc = balls[b];
			for (int lv = 0; lv < 8; ++lv) {
				float k2 = 0.15 + 0.85 * lv / 7.0;
				pal.add(bc[0] * k2 * tr, bc[1] * k2 * tg, bc[2] * k2 * tb);
			}
		}
	}

	void update(float t, float dt) override {
		tt = t;
		for (int i = 0; i < 3; ++i) {
			float bounce = math::sin(t * (1.9 + i * 0.37) + i * 2.1);
			bounce = bounce < 0.0 ? -bounce : bounce;
			float a = t * 0.45 + i * 2.0944;
			bx[i] = math::cos(a) * 1.45;
			bz[i] = math::sin(a) * 1.45;
			by[i] = rad[i] + bounce * (1.1 - i * 0.2);
		}
		float ca = t * 0.18 + 0.4;
		ex = math::sin(ca) * 5.2;
		ez = math::cos(ca) * 5.2;
		ey = 1.5 + 0.35 * math::sin(t * 0.3);
		float tx = 0.0 - ex;
		float ty = 0.8 - ey;
		float tz = 0.0 - ez;
		float il = 1.0 / math::sqrt(tx * tx + ty * ty + tz * tz);
		fx = tx * il; fy = ty * il; fz = tz * il;
		float rl = 1.0 / math::sqrt(fx * fx + fz * fz);
		rx = 0.0 - fz * rl;
		rz = fx * rl;
		ux = 0.0 - rz * fy;
		uy = rz * fx - rx * fz;
		uz = rx * fy;
	}

	void render(var& grid) override {
		CH_X0 = bx[0]; CH_Y0 = by[0]; CH_Z0 = bz[0]; CH_R0 = rad[0];
		CH_X1 = bx[1]; CH_Y1 = by[1]; CH_Z1 = bz[1]; CH_R1 = rad[1];
		CH_X2 = bx[2]; CH_Y2 = by[2]; CH_Z2 = bz[2]; CH_R2 = rad[2];
		CH_EX = ex; CH_EY = ey; CH_EZ = ez;
		CH_FX = fx; CH_FY = fy; CH_FZ = fz;
		CH_RX = rx; CH_RZ = rz;
		CH_UX = ux; CH_UY = uy; CH_UZ = uz;
		float ll = 1.0 / math::sqrt(0.5 * 0.5 + 0.75 * 0.75 + 0.43 * 0.43);
		CH_LX = 0.5 * ll; CH_LY = 0.75 * ll; CH_LZ = 0.43 * ll;
		CH_W = w;
		CH_H = h;
		CH_FLOOR = floor0;
		CH_BALL = ball0;
		CH_NBASE = nbase;
		CH_GLINT = glint;
		if (CH_ROWS.size() != h) {
			CH_ROWS = [];
			for (int r = 0; r < h; ++r) { CH_ROWS.push(r); }
		}
		var rows = parallel_transform(CH_ROWS, ch_row);
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
