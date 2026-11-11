// DONUT — a product shot that drops in on the beat. The torus homage, grown up: one ray
// per pixel, rows across every core with parallel_transform, in a studio: a glossy walnut
// table that reflects the donut and takes its soft shadow, a cyclorama backdrop and
// drifting bokeh. Every ray is cast in the donut's own frame, where the donut is a torus
// about the y axis, and the ray's quartic with it is solved in closed form: nothing is
// marched. Everything on the donut is painted on that torus by its (phi, theta): the icing
// down to a wavy edge with drips hanging off it (the edge read from a table) and the
// sprinkles, one to a cell of a table laid over the surface, so they ride it as it turns.
// The normal leans with the icing's beaded lip and rounds over each sprinkle like a
// capsule, so both catch the light; a sprinkle's shadow on the icing is the same sprinkle
// looked up from a little toward the key light, the table's reflection one more quartic
// from the camera's mirror image, the shadow on the table a short march past a plain torus
// at every fourth pixel. The shot is scored: the title bar is dark, a
// glint tracing the silhouette over the riser; on the next downbeat the donut drops in from
// above and lands as the key light slams on; then it hops on every kick, the sprinkles
// popping off the icing, the light pumping and the camera swinging round on the bar lines
// while the donut keeps its iced face turned to the lens, pushes in on the sprinkles, and
// spins out after the accent. Then it winds up, tipping its face full to the camera and
// spinning like a wheel as the lights drop to a rim over the riser, is tossed on the big
// hit, tumbling end over end through the donut storm with the camera craning after it, is
// caught flat on the table on the next downbeat, rattling to rest in a splash of sprinkles
// while the camera steps round it on every beat, and the camera cranes up over it for the
// hero shot: the whole iced ring from above, its hole dead centre, held for the cut.

float DN_M00 = 1.0; float DN_M01 = 0.0; float DN_M02 = 0.0;   // world -> donut rotation
float DN_M10 = 0.0; float DN_M11 = 1.0; float DN_M12 = 0.0;
float DN_M20 = 0.0; float DN_M21 = 0.0; float DN_M22 = 1.0;
float DN_BOB = 0.0;                                            // the donut's height
float DN_CX = 0.0; float DN_CY = 0.0; float DN_CZ = 0.0;       // camera position
float DN_FX = 0.0; float DN_FY = 0.0; float DN_FZ = 1.0;       // and basis
float DN_RX = 1.0; float DN_RY = 0.0; float DN_RZ = 0.0;
float DN_UX = 0.0; float DN_UY = 1.0; float DN_UZ = 0.0;
float DN_EX = 0.0; float DN_EY = 0.0; float DN_EZ = 0.0;       // the camera in the donut's frame
float DN_EFX = 0.0; float DN_EFY = 0.0; float DN_EFZ = 1.0;
float DN_ERX = 1.0; float DN_ERY = 0.0; float DN_ERZ = 0.0;
float DN_EUX = 0.0; float DN_EUY = 1.0; float DN_EUZ = 0.0;
float DN_QX = 0.0; float DN_QY = 0.0; float DN_QZ = 0.0;       // and its mirror image in the table
float DN_QFX = 0.0; float DN_QFY = 0.0; float DN_QFZ = 1.0;
float DN_QUX = 0.0; float DN_QUY = 1.0; float DN_QUZ = 0.0;
float DN_ZOOM = 1.0;
float DN_LX = 0.0; float DN_LY = 1.0; float DN_LZ = 0.0;      // key light, donut frame
float DN_SX = 0.0; float DN_SY = 1.0; float DN_SZ = 0.0;      // the sweeping glint's light, donut frame
float DN_KEY = 1.0;          // key light level
float DN_GLINT = 0.0;        // the sweep's level
float DN_RIM = 0.0;          // rim light level
float DN_STAGE = 1.0;        // the set's light level
float DN_DRIFT = 0.0;        // bokeh drift
float DN_FALL = 0.0;         // the donut storm's fall
int DN_STORM = 4;            // how many of every eight backdrop cells hold a falling donut
float DN_FLOOR = -3.4;
float DN_POP = 0.0;          // how far the sprinkles stand off the icing beyond their rest
float DN_TUBE = 1.08;        // the tube's radius, icing and all (the ring's is 2)
float DN_REST = -2.3;        // the donut's height lying flat on the table after the catch
int DN_W = 0;
int DN_H = 0;
int DN_MAT = 0;
int DN_BG = 0;
int DN_TABLE = 0;
int DN_REFL = 0;
int DN_SEED = 0;
array<int> DN_ROWS = [];
array<float> DN_EDGE = [];         // the icing's edge (as sin theta) round the ring, 4096 steps of phi
array<float> DN_EDGE_SLOPE = [];   // and its slope per radian
array<int> DN_CELL_MAT = [];       // the sprinkle in each of 60 x 22 (phi, theta) cells: its material, 0 for none,
array<float> DN_CELL_U = [];       // its centre in cell units
array<float> DN_CELL_V = [];
array<float> DN_CELL_COS = [];     // and the way it lies
array<float> DN_CELL_SIN = [];

// how far along a ray in the donut's frame the donut begins, or -1: the nearest root of the ray's quartic
// with the torus, in closed form; the ray starts outside the donut, so its first crossing is the way in
function donut_entry(float ox, float oy, float oz, float dx, float dy, float dz) -> float {
	float n = ox * dx + oy * dy + oz * dz;
	float k = (ox * ox + oy * oy + oz * oz) * 0.5 + 1.4168;
	float k3 = n;
	float k2 = n * n - 4.0 * (dx * dx + dz * dz) + k;
	float k1 = n * k - 4.0 * (ox * dx + oz * dz);
	float k0 = k * k - 4.0 * (ox * ox + oz * oz);
	float c1 = k3 * (k3 * k3 - k2) + k1;
	// a nearly biquadratic quartic loses its cubic's precision: solve for 2 / t instead
	bool flipped = c1 < 0.01 && c1 > -0.01;
	if (flipped) {
		if (k0 == 0.0) { return -1.0; }
		float held = k1;
		k1 = k3;
		k3 = held;
		k0 = 1.0 / k0;
		k1 *= k0; k2 *= k0; k3 *= k0;
		c1 = k3 * (k3 * k3 - k2) + k1;
	}
	float c2 = k2 * 2.0 - 3.0 * k3 * k3;
	float c0 = k3 * (k3 * (c2 + 2.0 * k2) - 8.0 * k1) + 4.0 * k0;
	c2 = c2 / 3.0; c1 *= 2.0; c0 = c0 / 3.0;
	float q = c2 * c2 + c0;
	float r = c2 * c2 * c2 - 3.0 * c2 * c0 + c1 * c1;
	float h = r * r - q * q * q;
	float root1 = 0.0; float root2 = 0.0; float root3 = 0.0; float root4 = 0.0;
	if (h >= 0.0) {
		h = math::sqrt(h);
		float v = math::cbrt(r + h);
		float u = math::cbrt(r - h);
		float sx = (v + u) + 4.0 * c2;
		float sy = (v - u) * 1.7320508075688772;
		float yy = 0.5 * (math::sqrt(sx * sx + sy * sy) + sx);
		if (yy < 1.0e-18) { return -1.0; }
		float x = 0.5 * sy / math::sqrt(yy);
		float w = 2.0 * c1 / (x * x + yy);
		root1 = x - w - k3;
		root2 = 0.0 - x - w - k3;
	} else {
		float sq = math::sqrt(q);
		float turn = (0.0 - r) / (sq * q);
		turn = turn < -1.0 ? -1.0 : (turn > 1.0 ? 1.0 : turn);
		float w = sq * math::cos(math::acos(turn) / 3.0);
		float d2 = 0.0 - (w + c2);
		if (d2 < 0.0) { return -1.0; }
		float d1 = math::sqrt(d2);
		float lean = d1 > 1.0e-12 ? c1 / d1 : 0.0;
		float h1 = w - 2.0 * c2 + lean;
		float h2 = w - 2.0 * c2 - lean;
		h1 = h1 > 0.0 ? math::sqrt(h1) : 0.0;
		h2 = h2 > 0.0 ? math::sqrt(h2) : 0.0;
		root1 = 0.0 - d1 - h1 - k3;
		root2 = h1 - d1 - k3;
		root3 = d1 - h2 - k3;
		root4 = d1 + h2 - k3;
	}
	if (flipped) {
		root1 = root1 > 0.0 ? 2.0 / root1 : 0.0;
		root2 = root2 > 0.0 ? 2.0 / root2 : 0.0;
		root3 = root3 > 0.0 ? 2.0 / root3 : 0.0;
		root4 = root4 > 0.0 ? 2.0 / root4 : 0.0;
	}
	float result = 1000.0;
	if (root1 > 0.0 && root1 < result) { result = root1; }
	if (root2 > 0.0 && root2 < result) { result = root2; }
	if (root3 > 0.0 && root3 < result) { result = root3; }
	if (root4 > 0.0 && root4 < result) { result = root4; }
	return result < 999.0 ? result : -1.0;
}

// how much of the key light reaches a donut-frame point of the table past the donut (soft)
function donut_shadow(float px, float py, float pz) -> float {
	float lx = DN_LX; float ly = DN_LY; float lz = DN_LZ;
	// the ray to the light against the sphere that holds the shadow's torus
	float toward = px * lx + py * ly + pz * lz;
	float outside = px * px + py * py + pz * pz - 13.0;
	if (outside > 0.0 && (toward >= 0.0 || toward * toward <= outside)) { return 1.0; }
	float span = math::sqrt(toward * toward - outside);
	float t = 0.0 - toward - span;
	t = t > 0.04 ? t : 0.04;
	float tmax = span - toward;
	float result = 1.0;
	for (int s = 0; s < 10; ++s) {
		float x = px + lx * t;
		float y = py + ly * t;
		float z = pz + lz * t;
		float qx = math::sqrt(x * x + z * z) - 2.0;
		float d = math::sqrt(qx * qx + y * y) - 1.09;
		float k = 9.0 * d / t;
		result = k < result ? k : result;
		if (result < 0.02) { return 0.0; }
		t += d < 0.04 ? 0.04 : d;
		if (t > tmax) { break; }
	}
	return result;
}

// the lit donut along a ray in its own frame: a palette index, or -1 where the ray passes it by
// (a_refl: the table's reflection of it)
function donut_seen(float ex, float ey, float ez, float ux, float uy, float uz, bool a_refl) -> int {
	float t = donut_entry(ex, ey, ez, ux, uy, uz);
	if (t < 0.0) { return -1; }
	float px = ex + ux * t;
	float py = ey + uy * t;
	float pz = ez + uz * t;
	float ring = math::sqrt(px * px + pz * pz);
	ring = ring > 0.01 ? ring : 0.01;
	float qx = ring - 2.0;
	float cph = px / ring; float sph = pz / ring;
	float cth = qx * 0.925925926; float sth = py * 0.925925926;
	// the torus's own axes here: out of the tube (o), round the ring (a, level), round the tube (b)
	float ox = cph * cth; float oy = sth; float oz = sph * cth;
	float ax = 0.0 - sph; float az = cph;
	float bx = 0.0 - cph * sth; float by = cth; float bz = 0.0 - sph * sth;
	float lx = DN_LX; float ly = DN_LY; float lz = DN_LZ;
	float nx = ox; float ny = oy; float nz = oz;
	int mat = (sth < 0.2 && sth > -0.22 && qx > 0.2) ? 1 : 0;
	// the hole keeps some of the set's light out
	float ao = cth < 0.0 ? 1.0 + 0.3 * cth : 1.0;
	float sh = 1.0;
	if (sth > -0.7) {
		float phi = math::atan2(pz, px) + 3.14159265;
		int ei = math::ifloor(phi * 651.898646) & 4095;
		float above = (sth - DN_EDGE[ei]) * 11.111111;
		if (above > 0.0) {
			mat = 2;
			if (above > 1.4 && qx > -0.6) {
				float fp = phi * 9.5492966;
				float ft = (math::atan2(py, qx) + 3.14159265) * 3.5014087;
				int cp = math::ifloor(fp);
				int ct = math::ifloor(ft);
				cp = cp < 0 ? 0 : (cp > 59 ? 59 : cp);
				ct = ct < 0 ? 0 : (ct > 21 ? 21 : ct);
				int cell = cp * 22 + ct;
				int sprinkle = DN_CELL_MAT[cell];
				if (sprinkle != 0) {
					float la = (fp - DN_CELL_U[cell]) * 0.104719755 * ring;
					float lb = (ft - DN_CELL_V[cell]) * 0.3055913;
					float ca = DN_CELL_COS[cell];
					float sa = DN_CELL_SIN[cell];
					float su = la * ca + lb * sa;
					float sv = lb * ca - la * sa;
					float du = (su < 0.0 ? 0.0 - su : su) - 0.068;
					du = du > 0.0 ? du : 0.0;
					float reach = du * du + sv * sv;
					if (reach < 0.000961) {
						// on the sprinkle: a capsule's normal, out from its spine
						mat = sprinkle;
						float along = su < 0.0 ? 0.0 - du : du;
						float ka = along * ca - sv * sa;
						float kb = along * sa + sv * ca;
						float up = math::sqrt(0.000961 - reach);
						nx = (up * ox + ka * ax + kb * bx) * 32.2580645;
						ny = (up * oy + kb * by) * 32.2580645;
						nz = (up * oz + ka * az + kb * bz) * 32.2580645;
					} else {
						// the icing at the sprinkle's foot
						if (reach < 0.0036) { ao *= 0.55 + 0.45 * (math::sqrt(reach) - 0.031) * 34.48; }
						float facing_key = lx * ox + ly * oy + lz * oz;
						if (facing_key > 0.0 && !a_refl) {
							// and in its shadow: the same sprinkle seen from as far toward the key light as it stands high
							float thrown = (0.04 + DN_POP) / (facing_key > 0.3 ? facing_key : 0.3);
							float ta = la + (lx * ax + lz * az) * thrown;
							float tb = lb + (lx * bx + ly * by + lz * bz) * thrown;
							float tu = ta * ca + tb * sa;
							float tv = tb * ca - ta * sa;
							float dv = (tu < 0.0 ? 0.0 - tu : tu) - 0.068;
							dv = dv > 0.0 ? dv : 0.0;
							float shadow_reach = dv * dv + tv * tv;
							if (shadow_reach < 0.0025) { sh = shadow_reach < 0.0004 ? 0.0 : (math::sqrt(shadow_reach) - 0.02) * 33.3; }
						}
					}
				}
			}
			if (mat == 2 && above < 3.0) {
				// the icing thins across its edge and its bead: the normal leans down the slope
				float k = above > 1.0 ? 1.0 : above;
				float lip = (above - 0.55) * 2.6;
				float bead = 1.0 + lip * lip;
				float slope = ((above < 1.0 ? 0.42 * k * (1.0 - k) : 0.0) - 0.1664 * lip / (bead * bead)) * 11.111111;
				float round_ring = 0.0 - slope * DN_EDGE_SLOPE[ei] / ring;
				float round_tube = slope * cth * 0.925925926;
				nx = ox - round_ring * ax - round_tube * bx;
				ny = oy - round_tube * by;
				nz = oz - round_ring * az - round_tube * bz;
				float nl = 1.0 / math::sqrt(nx * nx + ny * ny + nz * nz);
				nx *= nl; ny *= nl; nz *= nl;
			}
		} else if (above > -1.3) {
			// the dough tucked under the icing's lip
			ao *= 0.62 - 0.2923 * above;
		}
	}
	float nd = nx * lx + ny * ly + nz * lz;
	float lit = nd;
	if (mat == 2) { lit = (nd + 0.35) / 1.35; }
	lit = lit < 0.0 ? 0.0 : lit;
	float facing = 0.0 - (nx * ux + ny * uy + nz * uz);
	facing = facing < 0.0 ? 0.0 : facing;
	float rim = 1.0 - facing;
	rim = rim * rim * rim * DN_RIM;
	// the light that reaches the side the key misses: a dim ambient, a warm bounce off the
	// table onto whatever faces down, and a fill from the lit backdrop behind, so the shadow
	// side keeps its shape and is never pure black
	float key = DN_KEY;
	float stage = DN_STAGE;
	float wny = DN_M01 * nx + DN_M11 * ny + DN_M21 * nz;
	float bounce = wny < 0.0 ? (0.0 - wny) * 0.26 : 0.0;
	float back = 0.0 - nd;
	back = back > 0.0 ? back * 0.12 : 0.0;
	float fill = (0.13 + bounce + back) * (0.45 + 0.55 * stage);
	float diffuse = (0.1 * stage + fill + key * lit * (0.25 + 0.75 * sh)) * ao + rim;
	if (mat == 2) { diffuse += 0.08 * key * (1.0 - facing); }
	if (a_refl) {
		int lv = math::itrunc(diffuse * 7.99);
		return DN_REFL + mat * 8 + (lv < 0 ? 0 : (lv > 7 ? 7 : lv));
	}
	// Blinn speculars: the key light and the sweeping glint, glossier on icing and sprinkles
	float hx = lx - ux; float hy = ly - uy; float hz = lz - uz;
	float s = (nx * hx + ny * hy + nz * hz) / math::sqrt(hx * hx + hy * hy + hz * hz + 0.0000001);
	s = s < 0.0 ? 0.0 : s;
	float s2 = s * s; float s4 = s2 * s2; float s8 = s4 * s4; float s16 = s8 * s8;
	float spec = (mat <= 1 ? s8 * 0.35 : (mat == 2 ? s16 * s16 * s16 : s16 * s8)) * key * sh;
	float sweep = DN_GLINT;
	if (sweep > 0.0) {
		float gx = DN_SX - ux; float gy = DN_SY - uy; float gz = DN_SZ - uz;
		float g = (nx * gx + ny * gy + nz * gz) / math::sqrt(gx * gx + gy * gy + gz * gz + 0.0000001);
		g = g < 0.0 ? 0.0 : g;
		float g2 = g * g; float g4 = g2 * g2; float g8 = g4 * g4;
		float glint = (mat <= 1 ? g8 * 0.4 : g8 * g8 * g8 * g8) * sweep;
		spec = spec > glint ? spec : glint;
	}
	int di = math::itrunc(diffuse * 15.99);
	int si = math::itrunc(spec * 3.99);
	return DN_MAT + mat * 64 + (di < 0 ? 0 : (di > 15 ? 15 : di)) * 4 + (si > 3 ? 3 : si);
}

// the set behind: the cyclorama graded by the ray's height, lit from behind the
// donut by a soft halo, and bokeh drifting in the open sky above the table
function donut_backdrop(float dx, float dy, float dz) -> int {
	float g = dy * 2.2;
	g = g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);
	float toward = dx * DN_FX + dz * DN_FZ;
	toward = toward < 0.0 ? 0.0 : toward;
	float t2 = toward * toward;
	float t4 = t2 * t2;
	float halo = t4 * t4 * t4 * (1.0 - g * 0.6);
	float bright = ((1.0 - g) * 0.55 + halo * 0.6) * DN_STAGE;
	int level = 23 - math::itrunc(bright * 23.99);
	int b = 0;
	if (dy > 0.05) {
		float az = math::atan2(dx, dz) * 4.0 + DN_DRIFT;
		float el = dy * 4.0 + DN_FALL;
		int ca = math::ifloor(az);
		int ce = math::ifloor(el);
		int hs = ((ca * 92837111 + ce * 689287499 + DN_SEED) & 1048575) * 1274126177;
		if (((hs >> 10) & 7) < DN_STORM) {
			float jx = ((hs >> 4) & 15) / 15.0 * 0.4 + 0.3;
			float jy = ((hs >> 14) & 15) / 15.0 * 0.4 + 0.3;
			float ex = az - ca - jx;
			float ey = el - ce - jy;
			float rr = 0.14 + ((hs >> 18) & 7) * 0.028;
			float q = (ex * ex + ey * ey) / (rr * rr);
			if (q < 1.0 && q > 0.22) {
				// an out-of-focus donut falling past: a soft ring, brighter at its outer rim,
				// feathered at both edges
				float lit = 0.55 + (q > 0.7 ? (q - 0.7) * 1.2 : 0.0) + ((hs >> 21) & 3) * 0.12;
				if (q > 0.88) { lit *= (1.0 - q) / 0.12; }
				if (q < 0.34) { lit *= (q - 0.22) / 0.12; }
				b = 1 + math::itrunc(lit * DN_STAGE * 7.0);
				b = b > 8 ? 8 : b;
				b += ((hs >> 24) % 3) * 8;
			}
		}
	}
	return DN_BG + (level < 0 ? 0 : (level > 23 ? 23 : level)) * 25 + b;
}

// one row of the picture: every ray is the row's base ray plus a step along the camera's right, in the world for
// the set and in the donut's frame for the donut, and a row is all sky or all table
function donut_row(int y) -> array<int> {
	array<int> out = [];
	int ww = DN_W;
	float sc = 1.5 / (ww * DN_ZOOM);
	float v = (DN_H * 0.5 - y - 0.5) * sc;
	float centre = ww * 0.5 - 0.5;
	float vv = 1.0 + v * v;
	float wx = DN_FX + DN_UX * v; float wy = DN_FY + DN_UY * v; float wz = DN_FZ + DN_UZ * v;
	float rx = DN_RX; float rz = DN_RZ;
	float ex = DN_EX; float ey = DN_EY; float ez = DN_EZ;
	float gx = DN_EFX + DN_EUX * v; float gy = DN_EFY + DN_EUY * v; float gz = DN_EFZ + DN_EUZ * v;
	float hx = DN_ERX; float hy = DN_ERY; float hz = DN_ERZ;
	// the ray against the sphere that holds the donut, the ray's length left in
	float bound = (2.0 + DN_TUBE) * (2.0 + DN_TUBE);
	float aim = ex * gx + ey * gy + ez * gz;
	float aim_step = ex * hx + ey * hy + ez * hz;
	float outside = ex * ex + ey * ey + ez * ez - bound;
	bool on_table = wy < -0.0001;
	float reach = 0.0;
	float fx0 = 0.0; float fx_step = 0.0; float fz0 = 0.0; float fz_step = 0.0;
	float px0 = 0.0; float py0 = 0.0; float pz0 = 0.0;
	float px_step = 0.0; float py_step = 0.0; float pz_step = 0.0;
	float mx = 0.0; float my = 0.0; float mz = 0.0;
	float mirror_aim = 0.0; float mirror_step = 0.0; float mirror_outside = 0.0;
	float qx = DN_QX; float qy = DN_QY; float qz = DN_QZ;
	if (on_table) {
		// the table point under each ray, in the world and in the donut's frame, is linear along the row
		reach = (DN_FLOOR - DN_CY) / wy;
		fx0 = DN_CX + wx * reach; fx_step = rx * reach;
		fz0 = DN_CZ + wz * reach; fz_step = rz * reach;
		float fy = DN_FLOOR - DN_BOB;
		px0 = DN_M00 * fx0 + DN_M01 * fy + DN_M02 * fz0; px_step = DN_M00 * fx_step + DN_M02 * fz_step;
		py0 = DN_M10 * fx0 + DN_M11 * fy + DN_M12 * fz0; py_step = DN_M10 * fx_step + DN_M12 * fz_step;
		pz0 = DN_M20 * fx0 + DN_M21 * fy + DN_M22 * fz0; pz_step = DN_M20 * fx_step + DN_M22 * fz_step;
		mx = DN_QFX + DN_QUX * v; my = DN_QFY + DN_QUY * v; mz = DN_QFZ + DN_QUZ * v;
		mirror_aim = qx * mx + qy * my + qz * mz;
		mirror_step = qx * hx + qy * hy + qz * hz;
		mirror_outside = qx * qx + qy * qy + qz * qz - bound;
	}
	float ambient = 0.12 * DN_STAGE;
	float key = 0.85 * DN_KEY;
	int walnut = DN_TABLE;
	// the donut's shadow on the table is soft: taken at every fourth pixel and blended across the three between
	int shadow_x = -8;
	float shadow_left = 1.0;
	float shadow_right = 1.0;
	for (int x = 0; x < ww; ++x) {
		float u = (x - centre) * sc;
		float uu = vv + u * u;
		float il = 1.0 / math::sqrt(uu);
		float toward = aim + aim_step * u;
		if (outside <= 0.0 || (toward < 0.0 && toward * toward > outside * uu)) {
			int seen = donut_seen(ex, ey, ez, (gx + hx * u) * il, (gy + hy * u) * il, (gz + hz * u) * il, false);
			if (seen >= 0) {
				out.push(seen);
				continue;
			}
		}
		if (!on_table) {
			out.push(donut_backdrop((wx + rx * u) * il, wy * il, (wz + rz * u) * il));
			continue;
		}
		float tf = reach * uu * il;
		if (tf > 60.0) {
			out.push(donut_backdrop((wx + rx * u) * il, -0.0001, (wz + rz * u) * il));
			continue;
		}
		// the donut in the table: the same ray from the camera's mirror image
		if (tf < 30.0) {
			float mirrored = mirror_aim + mirror_step * u;
			if (mirror_outside <= 0.0 || (mirrored < 0.0 && mirrored * mirrored > mirror_outside * uu)) {
				int seen = donut_seen(qx, qy, qz, (mx + hx * u) * il, (my + hy * u) * il, (mz + hz * u) * il, true);
				if (seen >= 0) {
					out.push(seen);
					continue;
				}
			}
		}
		// the table: a pool of light under the donut, walnut grain and the donut's shadow
		float fx = fx0 + fx_step * u;
		float fz = fz0 + fz_step * u;
		float pool = 1.0 / (1.0 + (fx * fx + fz * fz) * 0.035);
		float grain = math::sin(fx * 5.1 + math::sin(fz * 0.21) * 1.6) * 0.03 + math::sin(fx * 13.0 + fz * 0.4) * 0.015;
		float plank = fx / 2.6 - math::floor(fx / 2.6);
		if (plank < 0.025) { grain -= 0.1; }
		int fourth = x - (x & 3);
		if (fourth != shadow_x) {
			float left = (fourth - centre) * sc;
			float right = left + 4.0 * sc;
			shadow_left = fourth == shadow_x + 4 ? shadow_right : donut_shadow(px0 + px_step * left, py0 + py_step * left, pz0 + pz_step * left);
			shadow_right = donut_shadow(px0 + px_step * right, py0 + py_step * right, pz0 + pz_step * right);
			shadow_x = fourth;
		}
		float sh = shadow_left + (shadow_right - shadow_left) * (x - fourth) * 0.25;
		float far = 1.0 - (tf - 8.0) / 50.0;
		far = far < 0.5 ? 0.5 : (far > 1.0 ? 1.0 : far);
		int lv = math::itrunc((ambient + key * pool * (0.25 + 0.75 * sh) + grain) * far * 15.99);
		out.push(walnut + (lv < 0 ? 0 : (lv > 15 ? 15 : lv)));
	}
	return out;
}

class Donut : Scene {
	int mat0 = 0;
	float tt = 0.0;
	array<float> burst_at = [];
	array<int> burst_n = [];
	array<float> burst_base = [];
	array<float> burst_up = [];
	array<float> burst_out = [];

	// the reel picks the grid before build runs, so the budget is set at construction
	Donut() { pixel_budget = 26000; }

	// the landing, the spin-out's accent, the toss (a big hit the wind-up rises into) and the catch
	array<float> hit_times() override { return [MUSIC_BAR, MUSIC_BAR * 4.0, MUSIC_BAR * 7.0, MUSIC_BAR * 8.0]; }
	// ten bars: the toss on bar 7, the catch on bar 8, and the last bar holds the hero shot from above
	int length_bars() override { return 10; }
	array<int> hit_levels() override { return [2, 1, 2, 1]; }

	void build() override {
		name = "DONUT";
		handle = "a product shot . by strong_ptr";
		// the cyclorama: 24 levels, dusky near the horizon to deep navy overhead, each with
		// 8 levels of bokeh in three tints
		DN_BG = pal.size();
		for (int l = 0; l < 24; ++l) {
			float f = l / 23.0;
			float br = 58.0 - 52.0 * f;
			float bg = 52.0 - 44.0 * f;
			float bb = 84.0 - 62.0 * f;
			pal.add(br, bg, bb);
			var tints = [[255.0, 176.0, 96.0], [255.0, 120.0, 170.0], [110.0, 210.0, 230.0]];
			for (int tn = 0; tn < 3; ++tn) {
				var c = tints[tn];
				float tr = c[0]; float tg = c[1]; float tb = c[2];
				for (int k = 1; k <= 8; ++k) {
					float glow = k / 8.0 * 0.8;
					pal.add(br + (tr - br) * glow, bg + (tg - bg) * glow, bb + (tb - bb) * glow);
				}
			}
		}
		// the table: dark glossy walnut, 16 levels
		DN_TABLE = pal.ramp(16, 8.0, 5.0, 6.0, 168.0, 112.0, 76.0);
		// materials: dough, the pale band, icing, five sprinkles; 16 diffuse x 4 specular
		mat0 = pal.size();
		var mats = [[178.0, 104.0, 46.0], [232.0, 196.0, 128.0], [255.0, 120.0, 178.0], [255.0, 52.0, 60.0],
			[255.0, 214.0, 40.0], [60.0, 140.0, 255.0], [80.0, 226.0, 110.0], [252.0, 250.0, 246.0]];
		for (int m = 0; m < 8; ++m) {
			var c = mats[m];
			float cr = c[0]; float cg = c[1]; float cb = c[2];
			for (int d = 0; d < 16; ++d) {
				float df = 0.06 + 0.94 * d / 15.0;
				df = df > 1.0 ? 1.0 : df;
				for (int s = 0; s < 4; ++s) {
					float sp = s / 3.0 * (m <= 1 ? 0.4 : 0.92);
					float r = cr * df;
					float g = cg * df;
					float b = cb * df;
					pal.add(r + (255.0 - r) * sp, g + (240.0 - g) * sp, b + (235.0 - b) * sp);
				}
			}
		}
		// the donut in the table: each material at 8 levels, sunk into the walnut
		DN_REFL = pal.size();
		for (int m2 = 0; m2 < 8; ++m2) {
			var c2 = mats[m2];
			for (int l2 = 0; l2 < 8; ++l2) {
				float f2 = 0.08 + l2 / 7.0 * 0.5;
				pal.add(14.0 + c2[0] * f2 * 0.6, 9.0 + c2[1] * f2 * 0.6, 8.0 + c2[2] * f2 * 0.6);
			}
		}
		ice_the_ring();
		scatter_sprinkles();
		plan_bursts();
	}

	// the icing's edge at an angle round the ring, as the sine of the angle round the tube: it waves, and
	// drips hang off it in narrow tongues
	float icing_edge(float phi) {
		float drip = math::cos(phi * 11.0 + 1.7 * math::sin(phi * 2.0));
		drip = drip > 0.0 ? drip : 0.0;
		float d2 = drip * drip;
		float d4 = d2 * d2;
		return -0.02 + 0.1 * math::sin(phi * 5.0 + 2.0 * math::sin(phi * 3.0)) - 0.46 * d4 * d4;
	}

	void ice_the_ring() {
		DN_EDGE = [];
		DN_EDGE_SLOPE = [];
		for (int i = 0; i < 4096; ++i) {
			float phi = (i + 0.5) / 651.898646 - 3.14159265;
			DN_EDGE.push(icing_edge(phi));
			DN_EDGE_SLOPE.push((icing_edge(phi + 0.002) - icing_edge(phi - 0.002)) * 250.0);
		}
	}

	// five cells in eight hold a sprinkle, nudged off its cell's centre, lying along the ring or across it and turned a little
	void scatter_sprinkles() {
		DN_CELL_MAT = [];
		DN_CELL_U = [];
		DN_CELL_V = [];
		DN_CELL_COS = [];
		DN_CELL_SIN = [];
		for (int cp = 0; cp < 60; ++cp) {
			for (int ct = 0; ct < 22; ++ct) {
				int hs = ((cp * 73856093 + ct * 19349663 + (seed & 1023)) & 1048575) * 1274126177;
				int r = (hs >> 12) & 1023;
				float ang = ((r & 1) == 0 ? 0.0 : 1.5707963) + (((hs >> 22) & 31) / 31.0 - 0.5) * 0.7;
				DN_CELL_MAT.push(r < 640 ? 3 + r % 5 : 0);
				DN_CELL_U.push(cp + 0.5 + (((hs >> 3) & 15) / 15.0 - 0.5) * 0.3);
				DN_CELL_V.push(ct + 0.5 + (((hs >> 7) & 15) / 15.0 - 0.5) * 0.3);
				DN_CELL_COS.push(math::cos(ang));
				DN_CELL_SIN.push(math::sin(ang));
			}
		}
	}

	void update(float t, float dt) override {
		tt = t;
	}

	// the sprinkle bursts: each a set of sprinkles flung off the icing at one moment, every
	// one a closed-form arc from its spawn (out, up, then down under gravity), so a jump or a
	// replay lands on exactly the same sprinkles; the landing, the big accent and the toss are
	// bursts, the catch splashes them low and wide across the table, every beat between a puff
	void add_burst(float a_at, int a_count, float a_base, float a_up, float a_out) {
		burst_at.push(a_at);
		burst_n.push(a_count);
		burst_base.push(a_base);
		burst_up.push(a_up);
		burst_out.push(a_out);
	}

	void plan_bursts() {
		burst_at = [];
		burst_n = [];
		burst_base = [];
		burst_up = [];
		burst_out = [];
		add_burst(MUSIC_BAR, 90, 0.7, 5.6, 1.3);
		add_burst(MUSIC_BAR * 4.0, 90, 0.7, 5.6, 1.3);
		for (int b = 0; b < 19; ++b) { add_burst(MUSIC_BAR + MUSIC_BEAT * (b + 1), 12, 0.7, 3.4, 0.9); }
		add_burst(MUSIC_BAR * 7.0, 120, 0.1, 6.4, 1.6);
		add_burst(MUSIC_BAR * 8.0, 110, DN_REST + 0.4, 1.6, 3.2);
		for (int b2 = 1; b2 < 8; ++b2) { add_burst(MUSIC_BAR * 8.0 + MUSIC_BEAT * b2, 14, DN_REST + 0.7, 3.6, 0.9); }
	}

	void draw_sprinkles(var& grid, float t) {
		float sc = 1.5 / (w * DN_ZOOM);
		for (int k = 0; k < burst_at.size(); ++k) {
			float age = t - burst_at[k];
			if (age < 0.0 || age > 1.8) { continue; }
			int n = burst_n[k];
			float base = burst_base[k];
			float up0 = burst_up[k];
			float out0 = burst_out[k];
			for (int i = 0; i < n; ++i) {
				int hs = ((k * 7919 + i * 104729 + DN_SEED * 31) & 1048575) * 1274126177;
				float a = ((hs >> 4) & 1023) / 1023.0 * 6.2831853;
				float out = 2.0 + ((hs >> 14) & 63) / 63.0 * 0.6;
				float sp = out0 + ((hs >> 20) & 31) / 31.0 * 1.6;
				float up = up0 + ((hs >> 9) & 31) / 31.0 * 2.6;
				float px = math::cos(a) * (out + sp * age);
				float pz = math::sin(a) * (out + sp * age);
				float py = base + up * age - 4.9 * age * age;
				float vx = math::cos(a) * sp;
				float vz = math::sin(a) * sp;
				float vy = up - 9.8 * age;
				if (py < DN_FLOOR) { continue; }
				int mat = 3 + (hs >> 26) % 5;
				int ink = DN_MAT + mat * 64 + 15 * 4 + (age < 0.3 ? 2 : 1);
				// the head and a short tail along the velocity, projected through the camera
				for (int q = 0; q < 3; ++q) {
					float back = q * 0.025;
					float wx = px - vx * back - DN_CX;
					float wy = py - vy * back - DN_CY;
					float wz = pz - vz * back - DN_CZ;
					float z = wx * DN_FX + wy * DN_FY + wz * DN_FZ;
					if (z < 0.6) { continue; }
					float u = (wx * DN_RX + wy * DN_RY + wz * DN_RZ) / z;
					float v = (wx * DN_UX + wy * DN_UY + wz * DN_UZ) / z;
					int sx = math::itrunc(u / sc + w * 0.5);
					int sy = math::itrunc(h * 0.5 - v / sc);
					if (sx < 0 || sx >= w - 1 || sy < 0 || sy >= h - 1) { continue; }
					int c = q == 0 ? ink : ink - 8;
					grid[sy * w + sx] = c;
					if (q == 0) {
						grid[sy * w + sx + 1] = c;
						grid[(sy + 1) * w + sx] = c - 4;
					}
				}
			}
		}
	}

	float smooth(float a, float b, float t) {
		float k = (t - a) / (b - a);
		k = k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
		return k * k * (3.0 - 2.0 * k);
	}

	// 0 at the start of a bar's swing, easing to 1 over its first beat: the camera's moves land on the bar lines
	float on_bars(float t, float a, float b) {
		if (t < a) { return 0.0; }
		float bars = (t - a) / MUSIC_BAR;
		int whole = math::ifloor(bars);
		float k = (bars - whole) * 4.0;
		k = k > 1.0 ? 1.0 : k;
		float result = whole + k * k * (3.0 - 2.0 * k);
		float most = (b - a) / MUSIC_BAR;
		return result > most ? most : result;
	}

	// the same a beat at a time, each step eased over its first half beat
	float on_beats(float t, float a, float b) {
		if (t < a) { return 0.0; }
		float beats = (t - a) / MUSIC_BEAT;
		int whole = math::ifloor(beats);
		float k = (beats - whole) * 2.0;
		k = k > 1.0 ? 1.0 : k;
		float result = whole + k * k * (3.0 - 2.0 * k);
		float most = (b - a) / MUSIC_BEAT;
		return result > most ? most : result;
	}

	// 1 at the moment a_at, dying away at a_rate per second
	float decay(float t, float a_at, float a_rate) { return t < a_at ? 0.0 : math::pow(2.718281828, 0.0 - (t - a_at) * a_rate); }

	void render(var& grid) override {
		float t = tt;
		float bar = MUSIC_BAR;
		float land = bar;
		float toss = bar * 7.0;
		float catch_t = bar * 8.0;
		float hero_at = bar * 9.0;
		float kick = cue.kick_env(t);
		float snare = cue.snare_env(t);
		float energy = cue.energy(t);
		float hitv = cue.hit(t);
		bool landed = t >= land;
		bool airborne = t >= toss && t < catch_t;
		bool caught = t >= catch_t;
		// the last acts: the wind-up over bar 6, the flight through bar 7, the crane up to the hero shot in bar 9
		float wind = smooth(bar * 6.0, toss, t);
		float winding = t >= bar * 6.0 && t < toss ? cue.riser(t) : 0.0;
		float air = airborne ? (t - toss) / bar : 0.0;
		float arc = math::sin(air * 3.1415927);
		float crane = smooth(hero_at, hero_at + MUSIC_BEAT * 2.0, t);
		// the camera's way round: a swing on each bar line, a sweep through the toss, a step on every beat
		// after the catch, a slow drift over the hero shot
		float swing = on_bars(t, land, bar * 6.0);
		float az = -0.75 + swing * 0.62 + t * 0.04 + 0.7 * smooth(toss, catch_t, t) + on_beats(t, catch_t, hero_at) * 0.55
			+ (t > hero_at ? (t - hero_at) * 0.22 : 0.0);
		// the donut: tipped toward the camera wherever the camera goes, a little to one side of it and
		// then the other, spinning on its own axis, faster with the energy; it tips its face full to the
		// lens and spins up through the wind-up, flips twice end over end in the air and lands flat,
		// rattling as it settles
		float tilt = 0.82 + 0.2 * math::sin(t * 0.55) + (landed ? 0.0 : (land - t) * 0.6);
		tilt += (1.25 - tilt) * wind;
		if (airborne) { tilt = 1.25 - 13.8163706 * smooth(toss, catch_t - 0.08, t); }
		if (caught) {
			float settle = t - catch_t;
			tilt = 0.3 * math::sin(settle * 15.0) * math::pow(2.718281828, 0.0 - settle * 4.5);
		}
		float spin = t * (0.5 + energy * 0.35) + 0.4 + smooth(bar * 4.0, bar * 5.8, t) * 4.0 + wind * wind * 5.0;
		if (t >= toss) { spin += 5.0 * (1.0 - math::pow(2.718281828, 0.0 - (t - toss) * 1.2)); }
		float lean = az + (0.7 - 0.2 * swing) * (1.0 - wind);
		float ct = math::cos(tilt); float st = math::sin(tilt);
		float cs = math::cos(spin); float ss = math::sin(spin);
		float cl = math::cos(lean); float sl = math::sin(lean);
		// world-from-donut R = Ry(lean) * Rx(tilt) * Ry(spin); the rays want its transpose
		DN_M00 = cs * cl + ss * ct * sl;   DN_M01 = 0.0 - ss * st;   DN_M02 = cs * sl - ss * ct * cl;
		DN_M10 = st * sl;                  DN_M11 = ct;              DN_M12 = 0.0 - st * cl;
		DN_M20 = ss * cl - cs * ct * sl;   DN_M21 = cs * st;         DN_M22 = ss * sl + cs * ct * cl;
		// the drop: falling through the title bar, landing on the downbeat with a bounce,
		// then a hop on every kick; a crouch into the toss, the arc of the flight, and the
		// catch flat on the table bouncing to rest
		float after = t - land;
		float bounce = landed ? math::abs(math::sin(after * 9.0)) * 0.9 * math::pow(2.718281828, 0.0 - after * 3.5) : 0.0;
		float fall = landed ? 0.0 : 9.0 * (1.0 - (t / land) * (t / land));
		DN_BOB = fall + bounce + (landed ? kick * 0.32 * (1.0 - wind) : 0.0) - 0.45 * wind * wind;
		if (airborne) { DN_BOB = -0.45 + (DN_REST + 0.45) * air + 30.0 * air * (1.0 - air); }
		if (caught) {
			float since = t - catch_t;
			DN_BOB = DN_REST + math::abs(math::sin(since * 11.0)) * 0.55 * math::pow(2.718281828, 0.0 - since * 4.0) + kick * (0.34 - 0.2 * crane);
		}
		// never through the table: a tipped donut stands on its rim
		float standing = DN_FLOOR + DN_TUBE + 0.02 + 2.0 * math::abs(st);
		DN_BOB = DN_BOB > standing ? DN_BOB : standing;
		DN_POP = landed ? kick * 0.045 + hitv * 0.05 + arc * 0.05 : 0.0;
		// the camera: close from the start, a push in on the sprinkles, a pull back as it spins out, a
		// shake on the landing; low and close for the wind-up, craning after the donut through the toss,
		// then up over it, framed to hold the whole ring whatever the picture's shape, easing in to the cut
		float dist = 11.0 - 1.4 * smooth(land, land + 0.4, t) - 3.2 * smooth(bar * 3.0, bar * 3.5, t)
			+ 4.0 * smooth(bar * 4.0, bar * 5.6, t);
		dist += -2.4 * wind + 4.5 * arc + 0.5 * smooth(catch_t, catch_t + 0.6, t);
		float hero = 4.7 * w / h;
		hero = hero > 5.2 ? hero : 5.2;
		dist += (hero * (1.18 - 0.18 * smooth(hero_at, bar * 10.5, t)) - dist) * crane;
		dist -= kick * 0.3 * (1.0 - crane);
		float el = 0.2 + 0.06 * math::sin(t * 0.45) + 0.14 * smooth(bar * 3.0, bar * 3.5, t) - 0.1 * smooth(bar * 4.0, bar * 5.6, t);
		el += -0.16 * wind + 0.32 * arc + 0.3 * smooth(catch_t, catch_t + 0.5, t);
		el += (1.12 - el) * crane;
		float pivot = DN_REST * crane;
		DN_CX = dist * math::cos(el) * math::sin(az);
		DN_CY = pivot + dist * math::sin(el);
		DN_CZ = 0.0 - dist * math::cos(el) * math::cos(az);
		float shake = landed ? math::sin(after * 41.0) * 0.18 * math::pow(2.718281828, 0.0 - after * 6.0) : 0.0;
		shake += math::sin(t * 67.0) * 0.08 * winding + math::sin((t - toss) * 43.0) * 0.22 * decay(t, toss, 6.0)
			+ math::sin((t - catch_t) * 47.0) * 0.25 * decay(t, catch_t, 7.0);
		float tx = shake; float ty = -0.25 + DN_BOB * 0.35 + 0.45 * smooth(bar * 3.0, bar * 3.5, t) - 0.45 * smooth(bar * 4.0, bar * 5.6, t); float tz = 0.0;
		float follow = smooth(toss, toss + 0.25, t) * (1.0 - smooth(catch_t - 0.25, catch_t + 0.1, t));
		ty += (DN_BOB * 0.8 - ty) * follow;
		if (caught) { ty += (DN_BOB * 0.75 - ty) * smooth(catch_t - 0.1, catch_t + 0.3, t); }
		// the hero shot aims at the donut's resting place, so its hole sits dead centre and a hop comes at the lens
		ty += (DN_REST - ty) * crane + math::sin((t - catch_t) * 38.0) * 0.2 * decay(t, catch_t, 7.0);
		float fx = tx - DN_CX; float fy = ty - DN_CY; float fz = tz - DN_CZ;
		float fl = 1.0 / math::sqrt(fx * fx + fy * fy + fz * fz);
		fx *= fl; fy *= fl; fz *= fl;
		float rx = fz; float rz = 0.0 - fx;
		float rl = 1.0 / math::sqrt(rx * rx + rz * rz);
		rx *= rl; rz *= rl;
		float ux = rz * fy; float uy = rx * fz - rz * fx; float uz = 0.0 - rx * fy;
		DN_FX = fx; DN_FY = fy; DN_FZ = fz;
		DN_RX = rx; DN_RY = 0.0; DN_RZ = rz;
		DN_UX = ux; DN_UY = uy; DN_UZ = uz;
		// the same camera in the donut's frame, and its mirror image in the table for the reflection
		float ey = DN_CY - DN_BOB;
		DN_EX = DN_M00 * DN_CX + DN_M01 * ey + DN_M02 * DN_CZ;
		DN_EY = DN_M10 * DN_CX + DN_M11 * ey + DN_M12 * DN_CZ;
		DN_EZ = DN_M20 * DN_CX + DN_M21 * ey + DN_M22 * DN_CZ;
		DN_EFX = DN_M00 * fx + DN_M01 * fy + DN_M02 * fz;
		DN_EFY = DN_M10 * fx + DN_M11 * fy + DN_M12 * fz;
		DN_EFZ = DN_M20 * fx + DN_M21 * fy + DN_M22 * fz;
		DN_ERX = DN_M00 * rx + DN_M02 * rz;
		DN_ERY = DN_M10 * rx + DN_M12 * rz;
		DN_ERZ = DN_M20 * rx + DN_M22 * rz;
		DN_EUX = DN_M00 * ux + DN_M01 * uy + DN_M02 * uz;
		DN_EUY = DN_M10 * ux + DN_M11 * uy + DN_M12 * uz;
		DN_EUZ = DN_M20 * ux + DN_M21 * uy + DN_M22 * uz;
		float qy = 2.0 * DN_FLOOR - DN_CY - DN_BOB;
		DN_QX = DN_M00 * DN_CX + DN_M01 * qy + DN_M02 * DN_CZ;
		DN_QY = DN_M10 * DN_CX + DN_M11 * qy + DN_M12 * DN_CZ;
		DN_QZ = DN_M20 * DN_CX + DN_M21 * qy + DN_M22 * DN_CZ;
		DN_QFX = DN_M00 * fx - DN_M01 * fy + DN_M02 * fz;
		DN_QFY = DN_M10 * fx - DN_M11 * fy + DN_M12 * fz;
		DN_QFZ = DN_M20 * fx - DN_M21 * fy + DN_M22 * fz;
		DN_QUX = DN_M00 * ux - DN_M01 * uy + DN_M02 * uz;
		DN_QUY = DN_M10 * ux - DN_M11 * uy + DN_M12 * uz;
		DN_QUZ = DN_M20 * ux - DN_M21 * uy + DN_M22 * uz;
		// the push in is a dolly zoom: the lens widens as the camera closes, so the donut holds its
		// size and the set behind it stretches away; the wind-up draws the lens in and the toss lets it go
		float vertigo = smooth(bar * 3.0, bar * 3.5, t) * (1.0 - smooth(bar * 4.0, bar * 4.6, t));
		DN_ZOOM = (1.0 + kick * 0.06 + hitv * 0.05 + winding * 0.14) * (1.0 - 0.3 * vertigo);
		// the lights: dark through the title bar with a glint tracing the silhouette, the key
		// slamming on at the landing with a flash, then pumping with the kick and the snare;
		// dimming to a rim light over the wind-up, flashing back on the toss and the catch
		float flash = landed ? math::pow(2.718281828, 0.0 - after * 5.0) : 0.0;
		float flash2 = decay(t, toss, 5.0);
		float flash3 = decay(t, catch_t, 6.0);
		float dim = t < toss ? wind : 0.0;
		DN_KEY = landed ? (0.82 + kick * 0.3 + flash * 0.6 + hitv * 0.2 + flash2 * 0.7 + flash3 * 0.5) * (1.0 - 0.45 * dim) : 0.05;
		DN_STAGE = landed ? (0.8 + snare * 0.4 + flash * 0.5 + flash2 * 0.6 + flash3 * 0.4) * (1.0 - 0.6 * dim) : 0.12;
		DN_RIM = landed ? 0.4 + 0.5 * flash + 0.3 * snare + 0.6 * dim + 0.4 * flash2 : 0.95;
		// the glint sweeps the title bar, the spin-out, the wind-up and the hero shot
		float sw = -1.0;
		var sweeps = [(t - 0.15) / (land - 0.2), (t - bar * 4.0) / bar, (t - bar * 6.0) / bar, (t - bar * 9.25) / bar];
		for (int i = 0; i < 4; ++i) {
			float across = sweeps[i];
			if (sw < 0.0 && across >= 0.0 && across <= 1.0) { sw = across; }
		}
		DN_GLINT = sw < 0.0 ? 0.0 : math::sin(sw * 3.1415927) * 1.2;
		float sa = -1.4 + 2.8 * (sw < 0.0 ? 0.0 : sw);
		float wlx = -0.48; float wly = 0.74; float wlz = -0.47;
		DN_LX = DN_M00 * wlx + DN_M01 * wly + DN_M02 * wlz;
		DN_LY = DN_M10 * wlx + DN_M11 * wly + DN_M12 * wlz;
		DN_LZ = DN_M20 * wlx + DN_M21 * wly + DN_M22 * wlz;
		// the glint's light swings across behind the camera
		float gx = math::sin(sa) * 0.8 + fx * -0.6;
		float gy = 0.45;
		float gz = math::cos(sa) * -0.5 + fz * -0.6;
		float gl = 1.0 / math::sqrt(gx * gx + gy * gy + gz * gz);
		gx *= gl; gy *= gl; gz *= gl;
		DN_SX = DN_M00 * gx + DN_M01 * gy + DN_M02 * gz;
		DN_SY = DN_M10 * gx + DN_M11 * gy + DN_M12 * gz;
		DN_SZ = DN_M20 * gx + DN_M21 * gy + DN_M22 * gz;
		DN_DRIFT = t * 0.12;
		DN_FALL = t * 0.55 + (landed ? kick * 0.04 : 0.0);
		DN_STORM = landed ? (airborne ? 7 : (energy > 0.85 ? 6 : 5)) : 2;
		DN_W = w;
		DN_H = h;
		DN_MAT = mat0;
		DN_SEED = seed & 1023;
		if (DN_ROWS.size() != h) {
			DN_ROWS = [];
			for (int r = 0; r < h; ++r) { DN_ROWS.push(r); }
		}
		var rows = parallel_transform(DN_ROWS, donut_row);
		int idx = 0;
		for (int y = 0; y < h; ++y) {
			var& row = rows[y];
			for (int x = 0; x < w; ++x) {
				grid[idx] = row[x];
				++idx;
			}
		}
		draw_sprinkles(grid, t);
	}
}
