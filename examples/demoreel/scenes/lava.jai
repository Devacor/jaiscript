// LAVALAMP — a forest of lava lamps, raytraced in 3D, played to the music. The
// lamps stand on a dark floor in an endless grid, each a surface of revolution:
// a steel cone base, a tapered glass vessel and a steel cap. A camera flies them on
// a spline whose marks fall on bar lines: out of one lamp's wax, up into a crane
// over the whole field on the first big hit, low between the rows, then a rolling
// swoop that dives into another lamp's wax on the second. Each ray walks the grid
// cell by cell (a 2D DDA), tests the one lamp a cell holds against its bounding
// cylinder, and solves the lamp's profile (seven cone frustums) exactly only when
// it might hit, so the cost per pixel stays flat however many lamps are in view. Glass shows a fresnel edge and
// a studio glint and bends the ray a little; inside, the wax is a 3D metaball
// field (seven blobs rising off the heater and sinking as they cool, a pool on the
// base, a drip under the cap) marched along the bent ray, lit from the field's
// gradient, the liquid between glowing with it. The floor is black glass: it
// mirrors the lamps (a second walk of the grid along the bounced ray) over a pool of
// each lamp's light, and every lamp blooms into the haze around it, measured as the
// ray passes. Everything fogs with distance. Every lamp takes one of three blob
// simulations, turned to its own quarter, and its own colour family. The wax swells
// with the kick, burns brighter with the section's energy, shifts hue with the
// chord and runs a rainbow wave over the field after the first big hit; each big hit
// sends a ring of white heat racing out across the field, lamp by lamp, and the
// camera sways between the rows on the beat. The close-up holds through a riser, the
// wax swelling; the third big hit blasts the camera back out of the wax in a corkscrew
// up over the field, a lesser ring leaving that lamp on every kick under a rainbow
// wave, then it swoops down into a lane and races along it between the lamps. Rows
// run in parallel.

array<float> LV_BX = [];     // three blob sets of LV_N blobs, in lamp space
array<float> LV_BY = [];
array<float> LV_BZ = [];
array<float> LV_R2 = [];
array<float> LV_ST = [];     // each blob's stretch: its speed pulls it into taffy
array<float> LV_HUE = [];
array<int> LV_ROWS = [];
int LV_N = 9;
int LV_W = 0;
int LV_H = 0;
int LV_GLOW = 0;
int LV_GLASS = 0;
int LV_STEEL = 0;
int LV_ROOM = 0;
float LV_EX = 0.0;           // the camera: eye, basis and focal length
float LV_EY = 0.5;
float LV_EZ = -1.0;
float LV_FX = 0.0;
float LV_FY = 0.0;
float LV_FZ = 1.0;
float LV_RX = 1.0;
float LV_RY = 0.0;
float LV_RZ = 0.0;
float LV_UX = 0.0;
float LV_UY = 1.0;
float LV_UZ = 0.0;
float LV_FOCAL = 1.6;
float LV_GAIN = 1.0;         // how bright the wax burns: energy, kick and hits
float LV_FLASH = 0.0;        // the hits' white
float LV_HUE0 = 0.0;         // the chord's hue shift
float LV_BURN = 0.0;         // the rainbow wave's strength and clock
float LV_T = 0.0;
float LV_RING_X = 0.0;       // the last big hit's ring of heat: its centre, radius and strength
float LV_RING_Z = 0.0;
float LV_RING_R = -9.0;
float LV_RING_A = 0.0;
float LV_PULSE_X = 0.0;      // after the burst, a lesser ring on every kick from the lamp the camera left
float LV_PULSE_Z = 0.0;
float LV_PULSE_R = -9.0;
float LV_PULSE_A = 0.0;

// the vessel's radius at height y (lamp units: 0 the base's foot, 1 the cap's top)
function lamp_width(float y) -> float {
	if (y < 0.2) { return 0.16 - 0.35 * y; }
	float e = y - 0.42;
	if (y < 0.42) { return 0.13 - 0.85 * e * e; }
	if (y < 0.84) { return 0.13 - 0.363 * e * e; }
	return 0.066 - (y - 0.84) * 0.2;
}

// the wax field of the blobs in `mask` (bits of set b0) at a point in the set's own frame
function wax_field(float x, float y, float z, int b0, int mask) -> float {
	float f = 0.0;
	for (int k = 0; k < LV_N; ++k) {
		if (((mask >> k) & 1) == 0) { continue; }
		int b = b0 + k;
		float dx = x - LV_BX[b];
		float dz = z - LV_BZ[b];
		float dy = (y - LV_BY[b]) / LV_ST[b];
		f += LV_R2[b] / (dx * dx + dy * dy + dz * dz + 0.00002);
	}
	return f;
}

// the contribution-weighted hue there, so merging blobs blend their colours
function wax_hue(float x, float y, float z, int b0, int mask) -> float {
	float f = 0.0;
	float hs = 0.0;
	for (int k = 0; k < LV_N; ++k) {
		if (((mask >> k) & 1) == 0) { continue; }
		int b = b0 + k;
		float dx = x - LV_BX[b];
		float dz = z - LV_BZ[b];
		float dy = (y - LV_BY[b]) / LV_ST[b];
		float inv = LV_R2[b] / (dx * dx + dy * dy + dz * dz + 0.00002);
		f += inv;
		hs += inv * LV_HUE[b];
	}
	return f > 0.0 ? hs / f : 3.0;
}

// the blobs of set b0 the ray (from x, y, z along d) passes near enough to matter
function wax_mask(float x, float y, float z, float dx, float dy, float dz, int b0) -> int {
	int result = 0;
	for (int k = 0; k < LV_N; ++k) {
		int b = b0 + k;
		float cx = LV_BX[b] - x;
		float cy = LV_BY[b] - y;
		float cz = LV_BZ[b] - z;
		float along = cx * dx + cy * dy + cz * dz;
		float d2 = cx * cx + cy * cy + cz * cz - along * along;
		float reach = LV_R2[b] * 9.0 * LV_ST[b] * LV_ST[b];
		if (d2 < reach) { result = result | (1 << k); }
	}
	return result;
}

function fog_level(int level, float dist) -> int {
	float k = 1.0 - dist * 0.07;
	k = k < 0.15 ? 0.15 : k;
	return itrunc(level * k);
}

// inside the glass: march the bent ray through the wax field; the wax lit from its gradient, or the liquid's glow
function wax_shade(float x, float y, float z, float dx, float dy, float dz, int b0, int q, float hue_off, float dist, float boost) -> int {
	if (q == 0) { return LV_GLOW + ((itrunc(hue_off + 3.0) % 24 + 24) % 24) * 8 + fog_level(itrunc((0.12 + boost * 0.6) * LV_GAIN * 7.99), dist); }
	float stepl = 0.032;
	float fmax = 0.0;
	float mx = x;
	float my = y;
	float mz = z;
	float px = x;
	float py = y;
	float pz = z;
	for (int s = 0; s < 9; ++s) {
		float nx = px + dx * stepl;
		float ny = py + dy * stepl;
		float nz = pz + dz * stepl;
		if (s > 1 && math::sqrt(nx * nx + nz * nz) > lamp_width(ny) + 0.004) { break; }
		float f = wax_field(nx, ny, nz, b0, q);
		if (f >= 1.0) {
			// home in on the surface, then light it from the field's gradient
			float ax = px; float ay = py; float az = pz;
			float bx = nx; float by = ny; float bz = nz;
			for (int k = 0; k < 4; ++k) {
				float cx = (ax + bx) * 0.5; float cy = (ay + by) * 0.5; float cz = (az + bz) * 0.5;
				if (wax_field(cx, cy, cz, b0, q) >= 1.0) { bx = cx; by = cy; bz = cz; } else { ax = cx; ay = cy; az = cz; }
			}
			return wax_lit(bx, by, bz, dx, dy, dz, b0, q, hue_off, dist, boost);
		}
		if (f > fmax) {
			fmax = f;
			mx = nx; my = ny; mz = nz;
		}
		px = nx; py = ny; pz = nz;
	}
	int hq = (itrunc(wax_hue(mx, my, mz, b0, q) + hue_off + 0.5) % 24 + 24) % 24;
	int gl = itrunc((fmax * fmax * 0.85 + 0.12 + boost * 0.5) * LV_GAIN * 7.99);
	gl = fog_level(gl > 7 ? 7 : gl, dist);
	return LV_GLOW + hq * 8 + gl;
}

function wax_lit(float x, float y, float z, float dx, float dy, float dz, int b0, int q, float hue_off, float dist, float boost) -> int {
	float e = 0.004;
	float f0 = wax_field(x, y, z, b0, q);
	float gx = wax_field(x + e, y, z, b0, q) - f0;
	float gy = wax_field(x, y + e, z, b0, q) - f0;
	float gz = wax_field(x, y, z + e, b0, q) - f0;
	float il = -1.0 / math::sqrt(gx * gx + gy * gy + gz * gz + 0.0000001);
	float nx = gx * il;
	float ny = gy * il;
	float nz = gz * il;
	float d = nx * -0.45 + ny * 0.62 + nz * -0.64;
	d = d < 0.0 ? 0.0 : d;
	float rdot = dx * nx + dy * ny + dz * nz;
	float hx = dx - 2.0 * rdot * nx;
	float hy = dy - 2.0 * rdot * ny;
	float hz = dz - 2.0 * rdot * nz;
	float s = hx * -0.45 + hy * 0.62 + hz * -0.64;
	s = s < 0.0 ? 0.0 : s;
	float s2 = s * s;
	float s4 = s2 * s2;
	float s8 = s4 * s4;
	float rim = 1.0 + rdot;
	rim = rim < 0.0 ? 0.0 : rim;
	int lv = itrunc((d * 0.62 * LV_GAIN + 0.14 + s8 * s8 * 0.5 + rim * rim * 0.25 + LV_FLASH * 0.3 + boost * 0.55) * 15.99);
	lv = fog_level(lv > 15 ? 15 : lv, dist);
	int hq = (itrunc(wax_hue(x, y, z, b0, q) + hue_off + 0.5) % 24 + 24) % 24;
	return hq * 16 + lv;
}

// the lamp's profile as seven cone frustums, solved exactly: [y0, y1] per segment
// (the normal is taken from the true curve, so the shading stays smooth)
array<float> LV_SEG_Y = [0.0, 0.2, 0.3, 0.42, 0.56, 0.7, 0.84, 1.0];

// the lamp standing at cell (ci, cj): its surface's palette index, or -1 when the ray misses it
function lamp_hit(int ci, int cj, float ox, float oy, float oz, float dx, float dy, float dz) -> int {
	float lx = ox - ci;
	float lz = oz - cj;
	float a0 = dx * dx + dz * dz;
	float bo = lx * dx + lz * dz;
	float co = lx * lx + lz * lz;
	float best = 1000.0;
	for (int k = 0; k < 7; ++k) {
		float y0 = LV_SEG_Y[k];
		float y1 = LV_SEG_Y[k + 1];
		float r0 = lamp_width(y0);
		float r1 = lamp_width(y1 - 0.0001);
		float sl = (r1 - r0) / (y1 - y0);
		float ra = r0 + (oy - y0) * sl;
		float rb = dy * sl;
		float qa = a0 - rb * rb;
		float qb = bo - ra * rb;
		float qc = co - ra * ra;
		if (qa > -0.0000001 && qa < 0.0000001) { continue; }
		float disc = qb * qb - qa * qc;
		if (disc < 0.0) { continue; }
		float sq = math::sqrt(disc);
		float t0 = (0.0 - qb - sq) / qa;
		float t1 = (0.0 - qb + sq) / qa;
		float lo = t0 < t1 ? t0 : t1;
		float hi = t0 < t1 ? t1 : t0;
		for (int pass = 0; pass < 2; ++pass) {
			float t = pass == 0 ? lo : hi;
			if (t <= 0.0001 || t >= best) { continue; }
			float y = oy + dy * t;
			if (y < y0 || y > y1) { continue; }
			if (ra + rb * t < 0.0) { continue; }
			best = t;
		}
	}
	if (best >= 1000.0) { return -1; }
	return surface(ci, cj, lx + dx * best, oy + dy * best, lz + dz * best, dx, dy, dz, best);
}

function surface(int ci, int cj, float px, float py, float pz, float dx, float dy, float dz, float dist) -> int {
	float rr = math::sqrt(px * px + pz * pz) + 0.000001;
	float nx = px / rr;
	float nz = pz / rr;
	float slope = (lamp_width(py - 0.004) - lamp_width(py + 0.004)) / 0.008;
	float il = 1.0 / math::sqrt(1.0 + slope * slope);
	float ny = slope * il;
	nx *= il;
	nz *= il;
	float rdot = dx * nx + dy * ny + dz * nz;
	float rx = dx - 2.0 * rdot * nx;
	float ry = dy - 2.0 * rdot * ny;
	float rz = dz - 2.0 * rdot * nz;
	if (py < 0.2 || py > 0.84) {
		// steel: a studio of windows reflected round the cone
		float az = math::atan2(rz, rx);
		float band = 0.5 + 0.5 * math::cos(az * 3.0 + 0.8);
		float up = ry < 0.0 ? 0.0 : ry;
		int sl = itrunc((0.12 + band * band * 0.55 + up * up * 0.4 + LV_FLASH * 0.2 + ring_heat(ci, cj) * 0.4) * 15.99);
		sl = fog_level(sl > 15 ? 15 : sl, dist);
		return LV_STEEL + sl;
	}
	int id = ((ci * 7349 + cj * 1931) % 997 + 997) % 997;
	float boost = ring_heat(ci, cj);
	float cosv = 0.0 - rdot;
	cosv = cosv < 0.0 ? 0.0 : cosv;
	float fres = (1.0 - cosv) * (1.0 - cosv) * (1.0 - cosv);
	float glint = rx * -0.45 + ry * 0.62 + rz * -0.64;
	glint = glint < 0.0 ? 0.0 : glint;
	float g2 = glint * glint;
	float g8 = g2 * g2 * g2 * g2;
	float g16 = g8 * g8;
	// the glint: a hot core with two softer rings, so near glass shows a highlight, not a flat white shape
	float g32 = g16 * g16;
	float hot = 0.42 - LV_FLASH * 0.25;
	if (g32 > hot) { return LV_GLASS + 7; }
	if (g32 > hot * 0.45) { return LV_GLASS + 6; }
	if (g32 > hot * 0.18) { return LV_GLASS + 5; }
	if (fres > 0.55) { return LV_GLASS + 2 + fog_level(itrunc(fres * 5.0), dist); }
	// into the wax, the ray bent a little toward the axis
	float bx = dx - nx * 0.14;
	float by = dy;
	float bz = dz - nz * 0.14;
	float bl = 1.0 / math::sqrt(bx * bx + by * by + bz * bz);
	int set = id % 3;
	int quarter = (id / 3) % 4;
	float hue_off = (id % 6) * 4.0 + LV_HUE0 + LV_BURN * ((ci + cj * 2) * 2.0 + LV_T * 9.0);
	bx *= bl; by *= bl; bz *= bl;
	// into the blob set's own frame: the lamp's quarter turn
	float qx = quarter == 0 ? px : (quarter == 1 ? pz : (quarter == 2 ? 0.0 - px : 0.0 - pz));
	float qz = quarter == 0 ? pz : (quarter == 1 ? 0.0 - px : (quarter == 2 ? 0.0 - pz : px));
	float qdx = quarter == 0 ? bx : (quarter == 1 ? bz : (quarter == 2 ? 0.0 - bx : 0.0 - bz));
	float qdz = quarter == 0 ? bz : (quarter == 1 ? 0.0 - bx : (quarter == 2 ? 0.0 - bz : bx));
	int b0 = set * LV_N;
	int mask = wax_mask(qx, py, qz, qdx, by, qdz, b0);
	return wax_shade(qx, py, qz, qdx, by, qdz, b0, mask, hue_off, dist, boost);
}

// how hard the big hit's ring of heat is passing the lamp at (ci, cj) right now
function ring_heat(int ci, int cj) -> float {
	float result = 0.0;
	if (LV_RING_A > 0.0) {
		float rx = ci - LV_RING_X;
		float rz = cj - LV_RING_Z;
		float off = math::sqrt(rx * rx + rz * rz) - LV_RING_R;
		result = LV_RING_A / (1.0 + off * off * 1.6);
	}
	if (LV_PULSE_A > 0.0) {
		float qx = ci - LV_PULSE_X;
		float qz = cj - LV_PULSE_Z;
		float off2 = math::sqrt(qx * qx + qz * qz) - LV_PULSE_R;
		float pulse = LV_PULSE_A / (1.0 + off2 * off2 * 1.6);
		result = pulse > result ? pulse : result;
	}
	return result;
}

// a palette index at a fraction of its brightness, staying in its own ramp
function dimmed(int idx, float k) -> int {
	if (idx < LV_GLOW) { return idx - idx % 16 + itrunc((idx % 16) * k); }
	if (idx < LV_GLASS) {
		int off = idx - LV_GLOW;
		return LV_GLOW + off - off % 8 + itrunc((off % 8) * k);
	}
	if (idx < LV_STEEL) { return LV_GLASS + itrunc((idx - LV_GLASS) * k); }
	if (idx < LV_ROOM) { return LV_STEEL + itrunc((idx - LV_STEEL) * k); }
	int off2 = idx - LV_ROOM;
	return LV_ROOM + off2 - off2 % 16 + itrunc((off2 % 16) * k);
}

// how bright an index is, 0..15, whatever its ramp
function brightness(int idx) -> int {
	if (idx < LV_GLOW) { return idx % 16; }
	if (idx < LV_GLASS) { return ((idx - LV_GLOW) % 8) * 2; }
	if (idx < LV_STEEL) { return (idx - LV_GLASS) * 2; }
	if (idx < LV_ROOM) { return idx - LV_STEEL; }
	return (idx - LV_ROOM) % 16;
}

// the lamps a ray meets, walking the grid; -1 when it reaches `far` or the cells run out
function walk_lamps(float ox, float oy, float oz, float dx, float dy, float dz, float far, int cells) -> int {
	int ci = math::ifloor(ox + 0.5);
	int cj = math::ifloor(oz + 0.5);
	int step_i = dx > 0.0 ? 1 : -1;
	int step_j = dz > 0.0 ? 1 : -1;
	float adx = dx < 0.0 ? 0.0 - dx : dx;
	float adz = dz < 0.0 ? 0.0 - dz : dz;
	float delta_i = adx > 0.000001 ? 1.0 / adx : 1000000.0;
	float delta_j = adz > 0.000001 ? 1.0 / adz : 1000000.0;
	float next_i = (dx > 0.0 ? ci + 0.5 - ox : ox - (ci - 0.5)) * delta_i;
	float next_j = (dz > 0.0 ? cj + 0.5 - oz : oz - (cj - 0.5)) * delta_j;
	float t_cell = 0.0;
	float a = dx * dx + dz * dz;
	for (int k = 0; k < cells && t_cell < far; ++k) {
		float lx = ox - ci;
		float lz = oz - cj;
		float b = lx * dx + lz * dz;
		if (b * b - a * (lx * lx + lz * lz - 0.027225) >= 0.0) {
			int hit = lamp_hit(ci, cj, ox, oy, oz, dx, dy, dz);
			if (hit >= 0) { return hit; }
		}
		if (next_i < next_j) {
			t_cell = next_i;
			next_i += delta_i;
			ci += step_i;
		} else {
			t_cell = next_j;
			next_j += delta_j;
			cj += step_j;
		}
	}
	return -1;
}

// the floor: a pool of light under each lamp near the point, in the brightest one's colour
function floor_shade(float px, float pz, float dist) -> int {
	int ci = math::ifloor(px + 0.5);
	int cj = math::ifloor(pz + 0.5);
	float best = 0.0;
	float total = 0.0;
	int best_id = 0;
	for (int k = 0; k < 5; ++k) {
		int ii = ci + (k == 1 ? 1 : (k == 2 ? -1 : 0));
		int jj = cj + (k == 3 ? 1 : (k == 4 ? -1 : 0));
		float ddx = px - ii;
		float ddz = pz - jj;
		float pool = 1.0 / (1.0 + (ddx * ddx + ddz * ddz) * 26.0);
		total += pool;
		if (pool > best) {
			best = pool;
			best_id = ((ii * 7349 + jj * 1931) % 997 + 997) % 997;
		}
	}
	float hue_off = (best_id % 6) * 4.0 + LV_HUE0 + 3.0 + LV_BURN * LV_T * 9.0;
	int hq = (itrunc(hue_off) % 24 + 24) % 24;
	// the ring of heat runs across the floor as a line of light
	float ring = 0.0;
	if (LV_RING_A > 0.0) {
		float rx = px - LV_RING_X;
		float rz = pz - LV_RING_Z;
		float off = math::sqrt(rx * rx + rz * rz) - LV_RING_R;
		ring = LV_RING_A / (1.0 + off * off * 40.0);
	}
	if (LV_PULSE_A > 0.0) {
		float qx = px - LV_PULSE_X;
		float qz = pz - LV_PULSE_Z;
		float off2 = math::sqrt(qx * qx + qz * qz) - LV_PULSE_R;
		float pulse = LV_PULSE_A / (1.0 + off2 * off2 * 40.0);
		ring = pulse > ring ? pulse : ring;
	}
	int lv = itrunc((best * best * 1.5 * LV_GAIN + ring_heat(ci, cj) * best * 0.8 + ring * 0.9) * 15.99);
	lv = fog_level(lv > 15 ? 15 : lv, dist);
	if (ring > 0.35) { hq = (hq + 12) % 24; }
	return LV_ROOM + hq * 16 + lv;
}

// one ray: walk the grid cell by cell until a lamp answers, the floor stops it, or the fog swallows it
function trace(float ox, float oy, float oz, float dx, float dy, float dz) -> int {
	float t_floor = dy < -0.0001 ? (0.0 - oy) / dy : 1000.0;
	float far = t_floor < 13.0 ? t_floor : 13.0;
	int ci = math::ifloor(ox + 0.5);
	int cj = math::ifloor(oz + 0.5);
	int step_i = dx > 0.0 ? 1 : -1;
	int step_j = dz > 0.0 ? 1 : -1;
	float adx = dx < 0.0 ? 0.0 - dx : dx;
	float adz = dz < 0.0 ? 0.0 - dz : dz;
	float delta_i = adx > 0.000001 ? 1.0 / adx : 1000000.0;
	float delta_j = adz > 0.000001 ? 1.0 / adz : 1000000.0;
	float edge_i = dx > 0.0 ? ci + 0.5 - ox : ox - (ci - 0.5);
	float edge_j = dz > 0.0 ? cj + 0.5 - oz : oz - (cj - 0.5);
	float next_i = edge_i * delta_i;
	float next_j = edge_j * delta_j;
	float t_cell = 0.0;
	float a = dx * dx + dz * dz;
	// the bloom: how close the ray came to a lamp's glowing body, and which lamp
	float glow = 0.0;
	int glow_i = 0;
	int glow_j = 0;
	for (int k = 0; k < 28 && t_cell < far; ++k) {
		float lx = ox - ci;
		float lz = oz - cj;
		float b = lx * dx + lz * dz;
		float disc = b * b - a * (lx * lx + lz * lz - 0.027225);
		if (disc >= 0.0) {
			int hit = lamp_hit(ci, cj, ox, oy, oz, dx, dy, dz);
			if (hit >= 0) { return hit; }
		}
		float tc = a > 0.000001 ? (0.0 - b) / a : 0.0;
		if (tc > 0.0 && tc < far) {
			float qx = lx + dx * tc;
			float qz = lz + dz * tc;
			float qy = oy + dy * tc;
			float side = qy < 0.25 ? 0.25 - qy : (qy > 0.8 ? qy - 0.8 : 0.0);
			float g = 1.0 / (1.0 + (qx * qx + qz * qz + side * side) * 55.0) / (1.0 + tc * 0.12);
			if (g > glow) {
				glow = g;
				glow_i = ci;
				glow_j = cj;
			}
		}
		if (next_i < next_j) {
			t_cell = next_i;
			next_i += delta_i;
			ci += step_i;
		} else {
			t_cell = next_j;
			next_j += delta_j;
			cj += step_j;
		}
	}
	int halo = itrunc((glow - 0.08) * 14.0 * LV_GAIN);
	int glow_id = ((glow_i * 7349 + glow_j * 1931) % 997 + 997) % 997;
	int glow_ink = LV_ROOM + (itrunc((glow_id % 6) * 4.0 + LV_HUE0 + 3.0 + LV_BURN * LV_T * 9.0) % 24) * 16;
	if (t_floor < 13.0) {
		float px = ox + dx * t_floor;
		float pz = oz + dz * t_floor;
		int result = floor_shade(px, pz, t_floor);
		// the black-glass floor: the lamps it mirrors, darker, over its pool of light
		int seen = t_floor < 5.0 ? walk_lamps(px, 0.0005, pz, dx, 0.0 - dy, dz, 4.0, 6) : -1;
		if (seen >= 0) {
			int mirrored = dimmed(seen, 0.5 - t_floor * 0.08);
			if (brightness(mirrored) > brightness(result)) { result = mirrored; }
		}
		if (halo > brightness(result)) { result = glow_ink + (halo > 15 ? 15 : halo); }
		return result;
	}
	// the haze above the horizon, faintly lit near it, glowing round the lamps
	if (halo > 2) { return glow_ink + (halo > 15 ? 15 : halo); }
	return LV_ROOM + 17 * 16 + (dy > 0.18 ? 0 : (dy > 0.06 ? 1 : 2));
}

function lava_row(int y) -> array<int> {
	array<int> out = [];
	int ww = LV_W;
	float half = LV_H * 0.5;
	float v = (half - y - 0.5) / half;
	for (int x = 0; x < ww; ++x) {
		float u = (x + 0.5 - ww * 0.5) / half;
		float dx = LV_FX * LV_FOCAL + LV_RX * u + LV_UX * v;
		float dy = LV_FY * LV_FOCAL + LV_RY * u + LV_UY * v;
		float dz = LV_FZ * LV_FOCAL + LV_RZ * u + LV_UZ * v;
		float il = 1.0 / math::sqrt(dx * dx + dy * dy + dz * dz);
		out.push(trace(LV_EX, LV_EY, LV_EZ, dx * il, dy * il, dz * il));
	}
	return out;
}

class Lava : Scene {
	array<float> bx = [];
	array<float> by = [];
	array<float> bz = [];
	array<float> br2 = [];
	array<float> st = [];
	array<float> hue = [];
	array<float> period = [];
	array<float> phase = [];
	array<float> sway = [];
	int nb = 7;          // rising blobs per set; each set adds the pool and the cap's drip
	int sets = 3;
	float tt = 0.0;

	// the camera's marks, one on every bar line: eye, the point it looks at, roll; the
	// skim between the rows takes two bars, the dive lands on bar 7 and its close-up
	// holds through the riser, bar 8's hit blasts the camera up out of the wax in a
	// corkscrew over the field, bar 9 swoops it down into a lane and bar 10 races along it
	array<float> key_ex = [0.0, 0.07, 0.62, 2.6, 3.5, 3.5, 3.48, 3.96, 3.98, 3.0, 6.5, 6.5];
	array<float> key_ey = [0.52, 0.56, 0.72, 3.3, 0.36, 0.42, 0.62, 0.5, 0.5, 2.7, 0.42, 0.4];
	array<float> key_ez = [-0.22, -0.34, -1.45, -3.1, 0.45, 1.9, 3.25, 3.81, 3.84, 2.3, 5.6, 9.2];
	array<float> key_lx = [0.0, 0.0, 0.0, 1.2, 3.5, 3.8, 4.0, 4.0, 4.0, 4.5, 6.5, 6.5];
	array<float> key_ly = [0.5, 0.5, 0.45, 0.25, 0.5, 0.5, 0.5, 0.5, 0.5, 0.0, 0.42, 0.4];
	array<float> key_lz = [0.2, 0.0, 0.0, 1.6, 4.0, 4.0, 4.0, 4.2, 4.2, 4.9, 9.0, 12.6];
	array<float> key_roll = [0.0, 0.12, -0.28, 0.42, -0.62, 0.2, 1.2, 2.3, 2.4, 6.28, 6.0, 6.28];

	// the budget is set before setup, which is when the reel reads it
	Lava() { pixel_budget = 17000; }

	// eleven bars: the dive completes on bar 7, the burst out is bar 8, the swoop bar 9, the lane bar 10
	int length_bars() override { return 11; }

	array<float> hit_times() override {
		return [MUSIC_BAR * 2.0, MUSIC_BAR * 3.0, MUSIC_BAR * 4.0, MUSIC_BAR * 6.0, MUSIC_BAR * 8.0, MUSIC_BAR * 9.0, MUSIC_BAR * 10.0];
	}
	array<int> hit_levels() override { return [1, 2, 1, 2, 2, 1, 1]; }

	void build() override {
		name = "LAVALAMP";
		handle = "wax and gradient . by operator overload";
		// 24 hues x 16 shades (wax), then 24 hues x 8 glow levels (the liquid)
		for (int hq = 0; hq < 24; ++hq) {
			var c = gfx.rainbow_rgb(hq * 0.26 + 0.42);
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
		LV_GLOW = pal.size();
		for (int hq2 = 0; hq2 < 24; ++hq2) {
			var c2 = gfx.rainbow_rgb(hq2 * 0.26 + 0.42);
			for (int g2 = 0; g2 < 8; ++g2) {
				float k = g2 / 7.0;
				k = k * k * 0.6;
				pal.add(18.0 + c2[0] * k, 8.0 + c2[1] * k, 36.0 + c2[2] * k);
			}
		}
		LV_GLASS = pal.ramp(8, 60.0, 50.0, 90.0, 245.0, 240.0, 255.0);
		LV_STEEL = pal.gradient(16, [[8.0, 8.0, 12.0], [50.0, 52.0, 62.0], [140.0, 144.0, 160.0], [235.0, 238.0, 250.0]]);
		// the room: each wax hue's light on a dark floor, 16 levels (the violet's lowest are the haze above)
		LV_ROOM = pal.size();
		for (int hq3 = 0; hq3 < 24; ++hq3) {
			var c3 = gfx.rainbow_rgb(hq3 * 0.26 + 0.42);
			for (int r3 = 0; r3 < 16; ++r3) {
				float k3 = r3 / 15.0 * 0.5;
				pal.add(5.0 + c3[0] * k3, 4.0 + c3[1] * k3, 9.0 + c3[2] * k3);
			}
		}
		for (int s2 = 0; s2 < sets; ++s2) {
			for (int i = 0; i < nb; ++i) {
				float r = 0.026 + rng.next(40) * 0.0004;
				br2.push(r * r);
				hue.push(2.0 + i * 0.25);
				period.push(5.5 + rng.next(100) * 0.05);
				phase.push(rng.next(628) * 0.01);
				sway.push(rng.next(628) * 0.01);
				bx.push(0.0);
				by.push(0.0);
				bz.push(0.0);
				st.push(1.0);
			}
			// the pool on the heater and the wax hanging under the cap
			br2.push(0.07 * 0.07);
			hue.push(3.5);
			bx.push(0.0);
			by.push(0.16);
			bz.push(0.0);
			st.push(1.0);
			br2.push(0.024 * 0.024);
			hue.push(4.5);
			bx.push(0.0);
			by.push(0.87);
			bz.push(0.0);
			st.push(1.0);
		}
		LV_N = nb + 2;
	}

	float smooth(float a, float b, float t) {
		float k = (t - a) / (b - a);
		k = k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
		return k * k * (3.0 - 2.0 * k);
	}

	// a Catmull-Rom spline through one track of marks, one mark a bar, so the
	// camera flows through its marks and lands each on a bar line; the close-up eases
	// still into bar 8 and the burst leaves it at full speed, slowing to the next mark
	float track(var& keys, float t) {
		int last = keys.size() - 1;
		float u = t / MUSIC_BAR;
		int k = math::ifloor(u);
		k = k < 0 ? 0 : (k > last - 1 ? last - 1 : k);
		float f = u - k;
		f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
		float p0 = keys[k > 0 ? k - 1 : 0];
		float p1 = keys[k];
		float p2 = keys[k + 1];
		if (k == 7) { return p1 + (p2 - p1) * f * f * (3.0 - 2.0 * f); }
		if (k == 8) {
			float g = 1.0 - f;
			return p1 + (p2 - p1) * (1.0 - g * g * g);
		}
		float p3 = keys[k + 2 <= last ? k + 2 : last];
		float f2 = f * f;
		float f3 = f2 * f;
		return 0.5 * (2.0 * p1 + (p2 - p0) * f + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * f2 + (3.0 * p1 - p0 - 3.0 * p2 + p3) * f3);
	}

	void update(float t, float dt) override {
		tt = t;
		// up off the heater, down as they cool, circling the glass; speed stretches them
		for (int s = 0; s < sets; ++s) {
			for (int i = 0; i < nb; ++i) {
				int k = s * nb + i;
				int slot = s * LV_N + i;
				float a = t * 6.2831853 / period[k] + phase[k];
				float y = 0.27 + 0.5 * (0.5 - 0.5 * math::cos(a));
				float vy = math::sin(a);
				float w = lamp_width(y) * 0.5;
				by[slot] = y;
				bx[slot] = w * math::sin(t * 0.37 + sway[k]);
				bz[slot] = w * math::cos(t * 0.29 + sway[k] * 1.7);
				st[slot] = 1.0 + (vy < 0.0 ? -vy : vy) * 0.9;
			}
		}
	}

	void render(var& grid) override {
		float t = tt;
		float kick = cue == null ? 0.0 : cue.kick_env(t);
		float snare = cue == null ? 0.0 : cue.snare_env(t);
		float riser = cue == null ? 0.0 : cue.riser(t);
		float energy = cue == null ? 0.6 : cue.energy(t);
		float hit = cue == null ? 0.0 : cue.hit(t);
		int chord = cue == null ? 0 : cue.chord(t);
		float ex = track(key_ex, t);
		float ey = track(key_ey, t);
		float ez = track(key_ez, t);
		// between the rows the camera sways on the beat, harder as the energy climbs
		float beat_ph = cue == null ? 0.0 : cue.beat_phase(t);
		float sway = math::sin(beat_ph * 6.2831853) * (0.04 + energy * 0.08) * smooth(MUSIC_BAR, MUSIC_BAR * 1.5, t);
		ex += sway;
		float fx = track(key_lx, t) - ex;
		float fy = track(key_ly, t) - ey;
		float fz = track(key_lz, t) - ez;
		float fl = 1.0 / math::sqrt(fx * fx + fy * fy + fz * fz);
		fx *= fl; fy *= fl; fz *= fl;
		// the right and up vectors from world up, then the roll (with a shudder on each hit)
		float rx = fz;
		float rz = 0.0 - fx;
		float rl = 1.0 / math::sqrt(rx * rx + rz * rz + 0.000001);
		rx *= rl; rz *= rl;
		float ux = fy * rz;
		float uy = fz * rx - fx * rz;
		float uz = 0.0 - fy * rx;
		float roll = track(key_roll, t) + hit * 0.06 * math::sin(t * 41.0) + riser * 0.03 * math::sin(t * 57.0);
		float cr = math::cos(roll);
		float sr = math::sin(roll);
		LV_RX = rx * cr + ux * sr; LV_RY = uy * sr; LV_RZ = rz * cr + uz * sr;
		LV_UX = ux * cr - rx * sr; LV_UY = uy * cr; LV_UZ = uz * cr - rz * sr;
		LV_FX = fx; LV_FY = fy; LV_FZ = fz;
		LV_EX = ex; LV_EY = ey; LV_EZ = ez;
		// the lens breathes on the kick and punches in on the hits, creeping in on the riser;
		// the wax swells with the riser and the snare flashes the glass
		LV_FOCAL = 1.45 * (1.0 + kick * 0.07 + hit * 0.12 + riser * 0.1);
		LV_GAIN = 0.62 + energy * 0.3 + kick * 0.3 + hit * 0.45 + riser * riser * 0.4 + smooth(MUSIC_BAR * 8.0, MUSIC_BAR * 9.0, t) * 0.2;
		LV_FLASH = hit + snare * 0.25;
		LV_HUE0 = chord * 2.0;
		LV_BURN = smooth(MUSIC_BAR * 3.0, MUSIC_BAR * 3.0 + 0.4, t) * (1.0 - smooth(MUSIC_BAR * 4.0 - 0.3, MUSIC_BAR * 4.0 + 0.3, t))
			+ smooth(MUSIC_BAR * 8.0, MUSIC_BAR * 8.0 + 0.4, t);
		LV_T = t;
		// the ring of heat: it leaves the point the camera looked at on each big hit and races outward
		LV_RING_A = 0.0;
		var times = hit_times();
		var levels = hit_levels();
		for (int k = 0; k < times.size(); ++k) {
			float since = t - times[k];
			if (levels[k] < 2 || since < 0.0 || since > 2.6) { continue; }
			LV_RING_X = track(key_lx, times[k]);
			LV_RING_Z = track(key_lz, times[k]);
			LV_RING_R = since * 5.5;
			LV_RING_A = 1.5 * (1.0 - since / 2.6);
		}
		LV_PULSE_A = 0.0;
		float pulse_from = MUSIC_BAR * 8.0 + MUSIC_BEAT;
		if (t >= pulse_from) {
			float beats = (t - pulse_from) / MUSIC_BEAT;
			LV_PULSE_X = track(key_lx, MUSIC_BAR * 8.0);
			LV_PULSE_Z = track(key_lz, MUSIC_BAR * 8.0);
			float frac = beats - math::floor(beats);
			LV_PULSE_R = frac * MUSIC_BEAT * 7.0;
			LV_PULSE_A = 1.1 * (1.0 - frac * 0.85) + kick * 0.3;
		}
		LV_BX = bx;
		LV_BY = by;
		LV_BZ = bz;
		LV_R2 = br2;
		LV_ST = st;
		LV_HUE = hue;
		LV_W = w;
		LV_H = h;
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
