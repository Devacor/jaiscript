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
// The middle sphere is glass: its ray refracts in, crosses the sphere, refracts
// out (Snell at both faces, index 1.5) and reads the world upside down through a
// pale copy of the table, while its grazing rim turns to mirror. The dark tiles
// are polished: a floor ray that lands on one bounces and shows any sphere above
// in a dim copy of the table. It plays to its cue: every bounce is a parabola that
// lands on a beat (one sphere on each beat, one every other, one on the offbeats),
// the camera cranes up from the floor, rides the riser in and whips around on the
// downbeat of bar three, and in bar four the spheres fall into step until they land
// as one on the downbeat of bar five, then pull back high; star sparkles bloom on
// the brightest glints, flaring on the hits and twinkling on the snare.
//
// A ring of six small chrome satellites orbits the big sphere, spinning up with the
// music and flung wide on each hit (one bounding sphere skips the six for rays that
// miss the ring), a sun hangs low in the sky so every mirror carries it, and each
// hit sends a glowing shockwave racing across the checkerboard from under the spheres.
// Through bar seven the three whirl inward, faster and faster, and crash together on
// bar eight's downbeat (a big hit, the frame whited out) into one giant mirror ball:
// its normal snaps to a facet, so each tile mirrors one piece of the world and whole
// tiles flash on the kick, and it throws wheeling rings of light spots on the floor
// that swell on the kick. The camera dives in around it and whips back on bar nine.
// The kick punches the lens in, the hits shake its roll.
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
float CH_RX = 1.0; float CH_RY = 0.0; float CH_RZ = 0.0;
float CH_UX = 0.0; float CH_UY = 1.0; float CH_UZ = 0.0;
float CH_LX = 0.5; float CH_LY = 0.75; float CH_LZ = 0.43;
int CH_W = 0;
int CH_H = 0;
int CH_FLOOR = 0;
int CH_BALL = 0;
int CH_NBASE = 0;
int CH_GLINT = 0;
int CH_MIRROR = 0;           // the polished tiles' copy of the class table
int CH_RING = 0;             // classes: the shockwave's glow by fog level, the satellites' shades, the sun
int CH_SAT = 0;
int CH_SUN = 0;
array<float> CH_SX = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0];   // the satellites
array<float> CH_SY = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0];
array<float> CH_SZ = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0];
float CH_SR = 0.2;
float CH_BX = 0.0; float CH_BY = 0.0; float CH_BZ = 0.0; float CH_BR = 0.0;   // the ring's bounding sphere
float CH_WX = 0.0; float CH_WZ = 0.0; float CH_WR = -1.0; float CH_WW = 0.3;  // the shockwave: centre, radius, width
float CH_FOC = 1.35;         // the lens, punching in on the kick
bool CH_DISCO = false;       // the three spheres fused into one faceted mirror ball
float CH_SPIN = 0.0;         // its turn about the vertical
float CH_GT = 0.975;         // the big sphere's glint threshold (a facet flashes whole)
float CH_GS = 100.0;
float CH_SPOT = 0.0;         // the light spots it throws on the floor, their radius in spot spacings
array<int> CH_ROWS = [];

// the distance to the nearest satellite a ray hits, or `best` when it hits none nearer
function ch_sat_t(float ox, float oy, float oz, float dx, float dy, float dz, float best, int skip) -> float {
	float qx = ox - CH_BX; float qy = oy - CH_BY; float qz = oz - CH_BZ;
	float b = qx * dx + qy * dy + qz * dz;
	float c = qx * qx + qy * qy + qz * qz - CH_BR * CH_BR;
	if (c > 0.0 && (b > 0.0 || b * b - c < 0.0)) { return best; }
	float r2 = CH_SR * CH_SR;
	for (int k = 0; k < 6; ++k) {
		if (k + 3 == skip) { continue; }
		float sx = ox - CH_SX[k]; float sy = oy - CH_SY[k]; float sz = oz - CH_SZ[k];
		float sb = sx * dx + sy * dy + sz * dz;
		float disc = sb * sb - (sx * sx + sy * sy + sz * sz - r2);
		if (disc <= 0.0) { continue; }
		float t = 0.0 - sb - math::sqrt(disc);
		if (t > 0.001 && t < best) { best = t; }
	}
	return best;
}

// which satellite a ray met at distance t (only asked when one was)
function ch_sat_at(float ox, float oy, float oz, float dx, float dy, float dz, float t) -> int {
	float px = ox + dx * t; float py = oy + dy * t; float pz = oz + dz * t;
	int result = 0;
	float bestd = 1000000.0;
	for (int k = 0; k < 6; ++k) {
		float ex = px - CH_SX[k]; float ey = py - CH_SY[k]; float ez = pz - CH_SZ[k];
		float d = ex * ex + ey * ey + ez * ez;
		if (d < bestd) { bestd = d; result = k; }
	}
	return result;
}

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
	float st = ch_sat_t(ox, oy, oz, dx, dy, dz, best, skip);
	if (st < best) {
		best = st;
		int sat = ch_sat_at(ox, oy, oz, dx, dy, dz, st);
		hit = 3;
		hr = CH_SR;
		hx = CH_SX[sat]; hy = CH_SY[sat]; hz = CH_SZ[sat];
	}
	if (hit >= 0) {
		float d = ((ox + dx * best - hx) * CH_LX + (oy + dy * best - hy) * CH_LY + (oz + dz * best - hz) * CH_LZ) / hr;
		d = d < 0.0 ? 0.0 : d;
		int lv = cast<int>((0.2 + 0.8 * d) * 7.99);
		return hit == 3 ? CH_SAT + lv : CH_BALL + hit * 8 + lv;
	}
	if (dy < -0.0005) {
		float tf = (0.0 - oy) / dy;
		float fx = ox + dx * tf;
		float fz = oz + dz * tf;
		int ix = cast<int>(fx + 4096.0);
		int iz = cast<int>(fz + 4096.0);
		int fog = cast<int>(11.5 - tf * 0.55);
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
		if (CH_DISCO) {
			// the mirror ball throws rings of light spots that wheel with it
			float ox2 = fx - CH_X0; float oz2 = fz - CH_Z0;
			float r = math::sqrt(ox2 * ox2 + oz2 * oz2);
			if (r > 1.0 && r < 11.0) {
				float ring = r * 0.9;
				float ri = math::floor(ring);
				float a = (math::atan2(oz2, ox2) - CH_SPIN) * (3.0 + ri * 5.5) * 0.1591549 + ri * 0.37;
				float fa = a - math::floor(a) - 0.5;
				float fr = ring - ri - 0.5;
				if (fa * fa + fr * fr < CH_SPOT * CH_SPOT) { return CH_RING + fog; }
			}
		}
		if (CH_WR > 0.0) {
			float wx = fx - CH_WX;
			float wz = fz - CH_WZ;
			float off = math::sqrt(wx * wx + wz * wz) - CH_WR;
			if (off < CH_WW && off > 0.0 - CH_WW) { return CH_RING + fog; }
		}
		return CH_FLOOR + (((ix + iz) & 1) * 2 + shade) * 12 + fog;
	}
	if (dx * CH_LX + dy * CH_LY + dz * CH_LZ > 0.993) { return CH_SUN + (dx * CH_LX + dy * CH_LY + dz * CH_LZ > 0.998 ? 1 : 0); }
	int s = cast<int>(dy * 30.0);
	return s < 0 ? 0 : (s > 23 ? 23 : s);
}

function ch_row(int y) -> array<int> {
	array<int> out = [];
	int ww = CH_W;
	float sc = 2.0 / ww;
	float v = (CH_H * 0.5 - y - 0.5) * sc;
	float ex = CH_EX; float ey = CH_EY; float ez = CH_EZ;
	float bx = CH_FX * CH_FOC + CH_UX * v;
	float by = CH_FY * CH_FOC + CH_UY * v;
	float bz = CH_FZ * CH_FOC + CH_UZ * v;
	for (int x = 0; x < ww; ++x) {
		float u = (x + 0.5 - ww * 0.5) * sc;
		float dx = bx + CH_RX * u;
		float dy = by + CH_RY * u;
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
		float st = ch_sat_t(ex, ey, ez, dx, dy, dz, best, -1);
		int sat = 0;
		if (st < best) {
			best = st;
			sat = ch_sat_at(ex, ey, ez, dx, dy, dz, st) + 1;
			hit = 3;
			cx = CH_SX[sat - 1]; cy = CH_SY[sat - 1]; cz = CH_SZ[sat - 1]; cr = CH_SR;
		}
		if (hit < 0) {
			int seen = ch_trace(ex, ey, ez, dx, dy, dz, -1);
			// a lit dark tile is polished: its bounce shows any sphere standing over it
			int tile = seen - CH_FLOOR;
			if (tile >= 24 && tile < 36) {
				float tf = (0.0 - ey) / dy;
				int above = ch_trace(ex + dx * tf, 0.0, ez + dz * tf, dx, 0.0 - dy, dz, -1);
				if (above >= CH_BALL && above < CH_BALL + 24) { seen = CH_MIRROR + above; }
			}
			out.push(seen);
			continue;
		}
		float hx = ex + dx * best;
		float hy = ey + dy * best;
		float hz = ez + dz * best;
		float nx = (hx - cx) / cr;
		float ny = (hy - cy) / cr;
		float nz = (hz - cz) / cr;
		if (hit == 0 && CH_DISCO) {
			// the mirror ball: snap the normal to its facet, tiles fewer toward the poles, dark grout between
			float lat = math::atan2(ny, math::sqrt(nx * nx + nz * nz)) * 5.7295780;
			float li = math::floor(lat + 0.5);
			float qlat = li * 0.1745329;
			float cl = math::cos(qlat);
			float nl = math::floor(36.0 * cl + 0.5);
			nl = nl < 4.0 ? 4.0 : nl;
			float lon = (math::atan2(nz, nx) + CH_SPIN) * nl * 0.1591549;
			float lo = math::floor(lon + 0.5);
			float el = lat - li;
			float eo = lon - lo;
			if (el > 0.42 || el < -0.42 || eo > 0.42 || eo < -0.42) {
				out.push(CH_BALL + 9);
				continue;
			}
			float qlon = lo / nl * 6.2831853 - CH_SPIN;
			nx = cl * math::cos(qlon);
			ny = math::sin(qlat);
			nz = cl * math::sin(qlon);
		}
		float cosi = 0.0 - (dx * nx + dy * ny + dz * nz);
		float dn = -2.0 * cosi;
		float mx = dx - nx * dn;
		float my = dy - ny * dn;
		float mz = dz - nz * dn;
		float g = mx * CH_LX + my * CH_LY + mz * CH_LZ;
		float gt = hit == 0 ? CH_GT : 0.975;
		if (g > gt) {
			int gl = cast<int>((g - gt) * (hit == 0 ? CH_GS : 100.0));
			out.push(CH_GLINT + (gl > 2 ? 2 : gl));
		} else if (hit == 1 && cosi > 0.3) {
			// glass: refract in, cross to the far face, refract out
			float k = 1.0 - 0.4444444 * (1.0 - cosi * cosi);
			float f = 0.6666667 * cosi - math::sqrt(k);
			float tx = dx * 0.6666667 + nx * f;
			float ty = dy * 0.6666667 + ny * f;
			float tz = dz * 0.6666667 + nz * f;
			float across = -2.0 * ((hx - cx) * tx + (hy - cy) * ty + (hz - cz) * tz);
			float px = hx + tx * across;
			float py = hy + ty * across;
			float pz = hz + tz * across;
			float ox = (px - cx) / cr;
			float oy = (py - cy) / cr;
			float oz = (pz - cz) / cr;
			float cosx = tx * ox + ty * oy + tz * oz;
			float k2 = 1.0 - 2.25 * (1.0 - cosx * cosx);
			if (k2 > 0.0) {
				float f2 = 1.5 * cosx - math::sqrt(k2);
				float rx = tx * 1.5 - ox * f2;
				float ry = ty * 1.5 - oy * f2;
				float rz = tz * 1.5 - oz * f2;
				out.push(CH_NBASE * 2 + ch_trace(px, py, pz, rx, ry, rz, 1));
			} else {
				out.push(CH_NBASE * 2 + ch_trace(hx, hy, hz, mx, my, mz, 1));
			}
		} else if (hit == 3 || (hit == 0 && CH_DISCO)) {
			out.push(ch_trace(hx, hy, hz, mx, my, mz, hit == 3 ? 2 + sat : 0));
		} else {
			out.push(CH_NBASE * (hit + 1) + ch_trace(hx, hy, hz, mx, my, mz, hit));
		}
	}
	return out;
}

class Chrome : Scene {
	array<float> base_rad = [0.9, 0.7, 0.55];
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
	int ring0 = 0;
	int sat0 = 0;
	int sun0 = 0;
	array<float> sat_x = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0];
	array<float> sat_y = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0];
	array<float> sat_z = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0];
	float ring_spin = 0.0;
	bool disco = false;
	float wave_r = -1.0;
	float wave_w = 0.3;
	float ex = 0.0;
	float ey = 1.6;
	float ez = 5.0;
	float fx = 0.0;
	float fy = 0.0;
	float fz = -1.0;
	float rx = 1.0;
	float ry = 0.0;
	float rz = 0.0;
	float ux = 0.0;
	float uy = 1.0;
	float uz = 0.0;
	float tt = 0.0;
	float foc = 1.35;
	float spin = 0.0;
	float t_merge = MUSIC_BAR * 8.0;

	void build() override {
		name = "CHROME";
		handle = "juggler's grandchild . one ray per pixel, every core";
		replay = 12.0;
		var tints = [[1.0, 0.82, 0.45], [0.93, 1.0, 1.04], [1.0, 0.45, 0.55], [0.8, 0.5, 0.56]];
		// the class table once plain, then once per sphere tint (the glass one pale
		// and bright), then once dim and dark red for the polished tiles
		for (int copy = 0; copy < 5; ++copy) {
			float tr = 1.0; float tg = 1.0; float tb = 1.0;
			float k = 1.0;
			if (copy > 0) {
				var tc = tints[copy - 1];
				tr = tc[0]; tg = tc[1]; tb = tc[2];
				k = copy == 2 ? 1.0 : 0.86;
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
		// the shockwave's glow, cyan to white through the fog; the satellites' cool chrome; the sun
		if (ring0 == 0) { ring0 = pal.size(); }
		for (int f3 = 0; f3 < nfog; ++f3) {
			float k3 = f3 / (nfog - 1.0);
			pal.add((250.0 * (1.0 - k3) + 150.0 * k3) * tr, (200.0 * (1.0 - k3) + 255.0 * k3) * tg, (170.0 * (1.0 - k3) + 255.0 * k3) * tb);
		}
		if (sat0 == 0) { sat0 = pal.size(); }
		for (int lv2 = 0; lv2 < 8; ++lv2) {
			float k4 = 0.2 + 0.8 * lv2 / 7.0;
			pal.add(225.0 * k4 * tr, 235.0 * k4 * tg, 255.0 * k4 * tb);
		}
		if (sun0 == 0) { sun0 = pal.size(); }
		pal.add(255.0 * tr, 214.0 * tg, 150.0 * tb);
		pal.add(255.0 * tr, 250.0 * tg, 225.0 * tb);
	}

	int length_bars() override { return 10; }
	array<float> hit_times() override { return [MUSIC_BAR * 3.0, MUSIC_BAR * 5.0, MUSIC_BAR * 8.0, MUSIC_BAR * 9.0]; }
	array<int> hit_levels() override { return [2, 1, 2, 1]; }

	float smooth01(float u) {
		float c = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
		return c * c * (3.0 - 2.0 * c);
	}

	// a bounce that lands every `period` beats, `offset` beats late: 0 on the floor, 1 at the top
	float hop(float t, float period, float offset) {
		float b = (t / MUSIC_BEAT - offset) / period;
		float f = b - math::floor(b);
		return 4.0 * f * (1.0 - f);
	}

	void update(float t, float dt) override {
		tt = t;
		// each sphere lands on its own beats until bar four, when they fall into step
		// and land together from bar five on
		float step = (t - MUSIC_BAR * 4.0) / MUSIC_BAR;
		step = step < 0.0 ? 0.0 : (step > 1.0 ? 1.0 : step);
		step = step * step * (3.0 - 2.0 * step);
		array<float> periods = [1.0, 2.0, 1.0];
		array<float> offsets = [0.0, 0.0, 0.5];
		float energy = cue != null ? cue.energy(t) : 0.6;
		float kick = cue != null ? cue.kick_env(t) : 0.0;
		float fling = cue != null ? cue.hit(t) : 0.0;
		// through bar seven they whirl inward, faster and faster, and crash together on bar eight's
		// downbeat into one giant mirror ball that rises and turns, jolted round by every kick
		float gather = (t - MUSIC_BAR * 7.0) / MUSIC_BAR;
		gather = gather < 0.0 ? 0.0 : (gather > 1.0 ? 1.0 : gather);
		float orbit_r = 1.45 - 0.5 * gather * gather;
		for (int i = 0; i < 3; ++i) {
			float own = hop(t, periods[i], offsets[i]);
			float shared = hop(t, 1.0, 0.0);
			float bounce = own + (shared - own) * step;
			float a = t * 0.45 + i * 2.0944 + gather * gather * gather * 7.0;
			rad[i] = base_rad[i];
			bx[i] = math::cos(a) * orbit_r;
			bz[i] = math::sin(a) * orbit_r;
			by[i] = rad[i] + bounce * (1.15 - i * 0.2) * (1.0 - gather * 0.5);
		}
		disco = t >= t_merge;
		if (disco) {
			float s = t - t_merge;
			float again = t - MUSIC_BAR * 9.0;
			float pop = again >= 0.0 ? 0.22 * math::exp(0.0 - again * 6.0) : 0.0;
			float big = 1.5 + (0.9 - 1.5) * math::exp(0.0 - s * 6.0) * math::cos(s * 15.0);
			rad[0] = big * (1.0 + kick * 0.04 + pop);
			bx[0] = 0.0;
			bz[0] = 0.0;
			by[0] = rad[0] + 0.45 * smooth01(s / 1.2) + 0.08 * math::sin(t * 1.9) - kick * 0.07;
			for (int j = 1; j < 3; ++j) {
				rad[j] = 0.3;
				bx[j] = 0.0; by[j] = by[0]; bz[j] = 0.0;
			}
			spin += dt * (0.5 + kick * 4.0 + fling * 3.0);
		}
		ring_spin += dt * (1.2 + energy * 2.6 + kick * 2.0);
		float orbit = rad[0] + 0.55 + fling * 0.9 + kick * 0.12;
		for (int k = 0; k < 6; ++k) {
			float a2 = ring_spin + k * 1.0471976;
			float tilt = math::sin(a2 * 2.0 + t) * 0.25;
			sat_x[k] = bx[0] + math::cos(a2) * orbit;
			sat_z[k] = bz[0] + math::sin(a2) * orbit;
			sat_y[k] = by[0] + tilt + math::sin(a2) * 0.3;
		}
		// the shockwave: out from under the spheres from each hit, fading as it widens
		wave_r = -1.0;
		var hits = hit_times();
		for (int h2 = 0; h2 < hits.size(); ++h2) {
			float since = t - hits[h2];
			if (since >= 0.0 && since < 2.2) {
				wave_r = since * 7.5 + (disco ? rad[0] : 0.0);
				wave_w = 0.18 + since * 0.35;
			}
		}
		// the camera: a low dolly that cranes up, rides the riser in, whips around on the
		// bar-three hit and pulls back high through the last bars
		float span = MUSIC_BAR * 7.0;
		float rise = t / span;
		rise = rise > 1.0 ? 1.0 : rise;
		float hit_at = MUSIC_BAR * 3.0;
		float whip = (t - hit_at) / (MUSIC_BEAT * 3.0);
		whip = whip < 0.0 ? 0.0 : (whip > 1.0 ? 1.0 : whip);
		whip = whip * whip * (3.0 - 2.0 * whip);
		float push = cue != null ? cue.riser(t) : 0.0;
		// from the crash it dives down and in around the mirror ball, whipping round again on the bar-nine accent
		float dive = smooth01((t - t_merge) / (MUSIC_BAR * 1.6));
		float whip2 = smooth01((t - t_merge) / (MUSIC_BEAT * 3.0));
		float whip3 = smooth01((t - MUSIC_BAR * 9.0) / (MUSIC_BEAT * 2.0));
		float ca = t * 0.18 + 0.4 + whip * 2.4 + whip2 * 1.9 - whip3 * 1.3;
		float radius = 3.4 + rise * rise * 2.2 - push * 0.9 - dive * 1.4;
		ex = math::sin(ca) * radius;
		ez = math::cos(ca) * radius;
		ey = 0.45 + rise * 1.55 - dive * 0.75;
		float tx = 0.0 - ex;
		float ty = 0.7 + dive * 1.25 - ey;
		float tz = 0.0 - ez;
		float il = 1.0 / math::sqrt(tx * tx + ty * ty + tz * tz);
		fx = tx * il; fy = ty * il; fz = tz * il;
		float rl = 1.0 / math::sqrt(fx * fx + fz * fz);
		rx = 0.0 - fz * rl;
		ry = 0.0;
		rz = fx * rl;
		ux = 0.0 - rz * fy;
		uy = rz * fx - rx * fz;
		uz = rx * fy;
		// the hits shake the roll, the kick punches the lens in
		float roll = fling * fling * 0.16 * math::sin(t * 37.0);
		float cr = math::cos(roll);
		float sr = math::sin(roll);
		float nrx = rx * cr + ux * sr; float nry = ry * cr + uy * sr; float nrz = rz * cr + uz * sr;
		ux = ux * cr - rx * sr; uy = uy * cr - ry * sr; uz = uz * cr - rz * sr;
		rx = nrx; ry = nry; rz = nrz;
		foc = 1.35 * (1.0 + kick * 0.07 + fling * 0.12);
	}

	void render(var& grid) override {
		CH_X0 = bx[0]; CH_Y0 = by[0]; CH_Z0 = bz[0]; CH_R0 = rad[0];
		CH_X1 = bx[1]; CH_Y1 = by[1]; CH_Z1 = bz[1]; CH_R1 = rad[1];
		CH_X2 = bx[2]; CH_Y2 = by[2]; CH_Z2 = bz[2]; CH_R2 = rad[2];
		CH_EX = ex; CH_EY = ey; CH_EZ = ez;
		CH_FX = fx; CH_FY = fy; CH_FZ = fz;
		CH_RX = rx; CH_RY = ry; CH_RZ = rz;
		CH_FOC = foc;
		CH_DISCO = disco;
		CH_SPIN = spin;
		float kick = cue != null ? cue.kick_env(tt) : 0.0;
		CH_GT = 0.93 - kick * 0.07;
		CH_GS = 3.0 / (1.0 - CH_GT);
		CH_SPOT = disco ? (0.1 + kick * 0.16) * smooth01((tt - t_merge) / 0.4) : 0.0;
		CH_UX = ux; CH_UY = uy; CH_UZ = uz;
		float ll = 1.0 / math::sqrt(0.5 * 0.5 + 0.75 * 0.75 + 0.43 * 0.43);
		CH_LX = 0.5 * ll; CH_LY = 0.75 * ll; CH_LZ = 0.43 * ll;
		CH_W = w;
		CH_H = h;
		CH_FLOOR = floor0;
		CH_BALL = ball0;
		CH_NBASE = nbase;
		CH_GLINT = glint;
		CH_MIRROR = nbase * 4;
		CH_RING = ring0;
		CH_SAT = sat0;
		CH_SUN = sun0;
		float bcx = 0.0; float bcy = 0.0; float bcz = 0.0;
		for (int k = 0; k < 6; ++k) {
			CH_SX[k] = sat_x[k]; CH_SY[k] = sat_y[k]; CH_SZ[k] = sat_z[k];
			bcx += sat_x[k] / 6.0; bcy += sat_y[k] / 6.0; bcz += sat_z[k] / 6.0;
		}
		float br = 0.0;
		for (int k2 = 0; k2 < 6; ++k2) {
			float ddx = sat_x[k2] - bcx; float ddy = sat_y[k2] - bcy; float ddz = sat_z[k2] - bcz;
			float dd = math::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
			br = dd > br ? dd : br;
		}
		CH_BX = bcx; CH_BY = bcy; CH_BZ = bcz; CH_BR = br + CH_SR + 0.01;
		CH_WX = (bx[0] + bx[1] + bx[2]) / 3.0;
		CH_WZ = (bz[0] + bz[1] + bz[2]) / 3.0;
		CH_WR = wave_r;
		CH_WW = wave_w;
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
		sparkle(grid);
		flash(grid);
	}

	// the big hits white the frame out and let it fall back through a dither
	void flash(var& grid) {
		array<int> bayer = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];
		var hits = hit_times();
		var levels = hit_levels();
		float since = 9.0;
		for (int k = 0; k < hits.size(); ++k) {
			float s = tt - hits[k];
			if (levels[k] >= 2 && s >= 0.0 && s < since) { since = s; }
		}
		if (since >= 0.35) { return; }
		float a = 1.0 - since / 0.35;
		int edge = math::itrunc(a * a * 17.0);
		for (int y = 0; y < h; ++y) {
			int row = y * w;
			int by4 = (y & 3) * 4;
			for (int x = 0; x < w; ++x) {
				int b = bayer[by4 + (x & 3)];
				if (b < edge) { grid[row + x] = b * 2 < edge ? glint + 2 : glint + 1; }
			}
		}
	}

	// star sparkles: on up to a dozen of the brightest glints, spaced apart, four
	// rays that twinkle in length
	void sparkle(var& grid) {
		int hot = glint + 2;
		int gap = w / 10 + 3;
		array<int> sx = [];
		array<int> sy = [];
		for (int y = 1; y < h - 1 && sx.size() < 12; ++y) {
			int row = y * w;
			for (int x = 1; x < w - 1; ++x) {
				if (grid[row + x] != hot) { continue; }
				bool near = false;
				for (int k = 0; k < sx.size(); ++k) {
					int ax = sx[k] - x;
					int ay = sy[k] - y;
					if (ax * ax + ay * ay < gap * gap) { near = true; break; }
				}
				if (!near) {
					sx.push(x);
					sy.push(y);
				}
			}
		}
		for (int s2 = 0; s2 < sx.size(); ++s2) {
			float flare = cue != null ? 1.0 + cue.hit(tt) * 2.2 + cue.snare_env(tt) * 0.8 : 1.0;
			int len = math::itrunc((3.0 + 4.0 * (0.5 + 0.5 * math::sin(tt * 7.0 + s2 * 1.7))) * flare * h / 120.0) + 2;
			for (int r = 1; r <= len; ++r) {
				int ink = r * 3 < len ? glint + 2 : (r * 3 < len * 2 ? glint + 1 : glint);
				star_px(grid, sx[s2] + r, sy[s2], ink);
				star_px(grid, sx[s2] - r, sy[s2], ink);
				star_px(grid, sx[s2], sy[s2] + r / 2, ink);
				star_px(grid, sx[s2], sy[s2] - r / 2, ink);
			}
		}
	}

	void star_px(var& grid, int x, int y, int ink) {
		if (x >= 0 && x < w && y >= 0 && y < h) { grid[y * w + x] = ink; }
	}
}
