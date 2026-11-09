// DONUT — a product shot that drops in on the beat. The torus homage, grown up: sphere-traced per pixel,
// rows across every core with parallel_transform, in a studio: a glossy walnut
// table that reflects the donut and takes its soft shadow, a cyclorama backdrop
// and drifting bokeh. The donut is a distance field in its own frame: the dough
// torus, an icing layer with real thickness (a beaded lip, a wavy edge and drips
// hanging off it) and sprinkles raised off the icing as capsules on a hashed grid
// in the torus's (phi, theta) coordinates, so they ride the surface as it turns.
// Normals come from the field, so the sprinkles catch the light; a short march
// toward the key light shadows the icing under them, and a sample along the normal
// darkens their footing. The shot is scored: the title bar is dark, a glint tracing
// the silhouette over the riser; on the next downbeat the donut drops in from above
// and lands on the table as the key light slams on; then it hops on every kick,
// the sprinkles popping off the icing, the light pumping and the camera swinging
// round on the bar lines, pushes in on the sprinkles, and spins out after the
// accent. Then it winds up, rocking back and spinning faster as the lights drop to a
// rim and the lens draws in over the riser, is tossed on the big hit, flipping end
// over end through the donut storm with the camera craning after it, is caught flat
// on the table on the next downbeat, rattling to rest in a splash of sprinkles while
// the camera steps round it on every beat, and the camera cranes over it and down
// into its hole for the cut.

float DN_M00 = 1.0; float DN_M01 = 0.0; float DN_M02 = 0.0;   // world -> donut rotation
float DN_M10 = 0.0; float DN_M11 = 1.0; float DN_M12 = 0.0;
float DN_M20 = 0.0; float DN_M21 = 0.0; float DN_M22 = 1.0;
float DN_BOB = 0.0;                                            // the donut's height
float DN_CX = 0.0; float DN_CY = 0.0; float DN_CZ = 0.0;       // camera position
float DN_FX = 0.0; float DN_FY = 0.0; float DN_FZ = 1.0;       // and basis
float DN_RX = 1.0; float DN_RY = 0.0; float DN_RZ = 0.0;
float DN_UX = 0.0; float DN_UY = 1.0; float DN_UZ = 0.0;
float DN_ZOOM = 1.0;
float DN_LX = 0.0; float DN_LY = 1.0; float DN_LZ = 0.0;      // key light, donut frame
float DN_WLX = 0.0; float DN_WLY = 1.0; float DN_WLZ = 0.0;   // and world frame
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
float DN_REST = -2.0;        // the donut's height lying flat on the table after the catch
int DN_W = 0;
int DN_H = 0;
int DN_MAT = 0;
int DN_BG = 0;
int DN_TABLE = 0;
int DN_REFL = 0;
int DN_SEED = 0;
array<int> DN_ROWS = [];

// the donut's field in its own frame; with a_mat it answers the material instead:
// 0 dough, 1 the pale fried band, 2 icing, 3..7 sprinkles
function donut_field(float px, float py, float pz, bool a_mat) -> float {
	float ring = math::sqrt(px * px + pz * pz);
	float qx = ring - 2.0;
	float rt = math::sqrt(qx * qx + py * py);
	if (rt > 1.32 && !a_mat) { return rt - 1.17; }
	float sy = py / (rt > 0.0001 ? rt : 0.0001);
	// the icing's edge waves round the ring and drips hang off it in narrow tongues
	float phi = math::atan2(pz, px);
	float drip = math::cos(phi * 11.0 + 1.7 * math::sin(phi * 2.0));
	drip = drip > 0.0 ? drip : 0.0;
	float d2 = drip * drip;
	float d4 = d2 * d2;
	float edge = -0.02 + 0.1 * math::sin(phi * 5.0 + 2.0 * math::sin(phi * 3.0)) - 0.46 * d4 * d4;
	float above = (sy - edge) / 0.09;
	float ice = 0.0;
	if (above > 0.0) {
		float k = above > 1.0 ? 1.0 : above;
		ice = 0.07 * k * k * (3.0 - 2.0 * k);
		float lip = (above - 0.55) * 2.6;
		ice += 0.032 / (1.0 + lip * lip);
	}
	float result = rt - 1.0 - ice;
	int mat = above > 0.0 ? 2 : ((sy < 0.2 && sy > -0.22 && qx > 0.2) ? 1 : 0);
	if (above > 1.4 && qx > -0.6) {
		float th = math::atan2(py, qx);
		float fp = (phi + 3.14159265) * 9.5;
		float ft = (th + 3.14159265) * 3.5;
		int cp = math::ifloor(fp);
		int ct = math::ifloor(ft);
		int hs = ((cp * 73856093 + ct * 19349663 + DN_SEED) & 1048575) * 1274126177;
		int r = (hs >> 12) & 1023;
		if (r < 640) {
			float jx = (((hs >> 3) & 15) / 15.0 - 0.5) * 0.3;
			float jy = (((hs >> 7) & 15) / 15.0 - 0.5) * 0.3;
			float ang = ((r & 1) == 0 ? 0.0 : 1.5707963) + (((hs >> 22) & 31) / 31.0 - 0.5) * 0.7;
			float la = (fp - cp - 0.5 - jx) / 9.5 * ring;
			float lb = (ft - ct - 0.5 - jy) / 3.5 * 1.07;
			float ca = math::cos(ang);
			float sa = math::sin(ang);
			float su = la * ca + lb * sa;
			float sv = lb * ca - la * sa;
			float c = rt - 1.0 - ice - 0.022 - DN_POP;
			float du = (su < 0.0 ? -su : su) - 0.068;
			du = du > 0.0 ? du : 0.0;
			float ds = math::sqrt(du * du + sv * sv + c * c) - 0.031;
			if (ds < result) {
				result = ds;
				mat = 3 + r % 5;
			}
		}
	}
	if (a_mat) { return mat; }
	return result < 0.045 ? result : 0.045 + (result - 0.045) * 0.5;
}

// sphere-trace the donut from a world-space ray; the hit's distance, or -1
function donut_trace(float ox, float oy, float oz, float dx, float dy, float dz, int steps) -> float {
	float py0 = oy - DN_BOB;
	float b = ox * dx + py0 * dy + oz * dz;
	float disc = b * b - (ox * ox + py0 * py0 + oz * oz) + 10.6;
	if (disc <= 0.0) { return -1.0; }
	float sq = math::sqrt(disc);
	float t = 0.0 - b - sq;
	t = t > 0.0 ? t : 0.0;
	float tfar = 0.0 - b + sq;
	for (int s = 0; s < steps; ++s) {
		float wx = ox + dx * t;
		float wy = oy + dy * t - DN_BOB;
		float wz = oz + dz * t;
		float px = DN_M00 * wx + DN_M01 * wy + DN_M02 * wz;
		float py = DN_M10 * wx + DN_M11 * wy + DN_M12 * wz;
		float pz = DN_M20 * wx + DN_M21 * wy + DN_M22 * wz;
		float d = donut_field(px, py, pz, false);
		if (d < 0.003) { return t; }
		t += d;
		if (t > tfar) { return -1.0; }
	}
	return -1.0;
}

// how much of the key light reaches a world point past the donut (soft)
function donut_shadow(float wx, float wy, float wz, float lx, float ly, float lz) -> float {
	float oy = wy - DN_BOB;
	float b = wx * lx + oy * ly + wz * lz;
	float c = wx * wx + oy * oy + wz * wz - 10.6;
	if (c > 0.0 && (b > 0.0 || b * b < c)) { return 1.0; }
	float tmax = 0.0 - b + math::sqrt(b * b - c);
	float result = 1.0;
	float t = 0.04;
	for (int s = 0; s < 11; ++s) {
		float qx = wx + lx * t;
		float qy = wy + ly * t - DN_BOB;
		float qz = wz + lz * t;
		float px = DN_M00 * qx + DN_M01 * qy + DN_M02 * qz;
		float py = DN_M10 * qx + DN_M11 * qy + DN_M12 * qz;
		float pz = DN_M20 * qx + DN_M21 * qy + DN_M22 * qz;
		float d = donut_field(px, py, pz, false);
		float k = 9.0 * d / t;
		result = k < result ? k : result;
		if (result < 0.02) { return 0.0; }
		t += d < 0.04 ? 0.04 : (d > 0.8 ? 0.8 : d);
		if (t > tmax) { break; }
	}
	return result < 0.0 ? 0.0 : result;
}

// the lit donut at a world hit: a palette index (a_refl: the table's reflection of it)
function donut_shade(float wx, float wy, float wz, float vx, float vy, float vz, bool a_refl) -> int {
	float qy = wy - DN_BOB;
	float px = DN_M00 * wx + DN_M01 * qy + DN_M02 * wz;
	float py = DN_M10 * wx + DN_M11 * qy + DN_M12 * wz;
	float pz = DN_M20 * wx + DN_M21 * qy + DN_M22 * wz;
	float e = 0.006;
	float a = donut_field(px + e, py - e, pz - e, false);
	float b = donut_field(px - e, py - e, pz + e, false);
	float c = donut_field(px - e, py + e, pz - e, false);
	float d = donut_field(px + e, py + e, pz + e, false);
	float nx = a - b - c + d;
	float ny = 0.0 - a - b + c + d;
	float nz = 0.0 - a + b - c + d;
	float nl = 1.0 / math::sqrt(nx * nx + ny * ny + nz * nz + 0.0000001);
	nx *= nl; ny *= nl; nz *= nl;
	int mat = itrunc(donut_field(px, py, pz, true));
	// the view ray, into the donut's frame
	float ux = DN_M00 * vx + DN_M01 * vy + DN_M02 * vz;
	float uy = DN_M10 * vx + DN_M11 * vy + DN_M12 * vz;
	float uz = DN_M20 * vx + DN_M21 * vy + DN_M22 * vz;
	float lx = DN_LX; float ly = DN_LY; float lz = DN_LZ;
	float nd = nx * lx + ny * ly + nz * lz;
	float lit = nd;
	if (mat == 2) { lit = (nd + 0.35) / 1.35; }
	lit = lit < 0.0 ? 0.0 : lit;
	float sh = 1.0;
	if (!a_refl && nd > 0.0) {
		// what stands close by toward the light (the sprinkles over the icing): four
		// fixed samples along the light from just off the surface, softly
		float bx = px + nx * 0.02;
		float by = py + ny * 0.02;
		float bz = pz + nz * 0.02;
		float reach = 0.05;
		for (int k = 0; k < 4; ++k) {
			float o = donut_field(bx + lx * reach, by + ly * reach, bz + lz * reach, false);
			float f = 8.0 * o / reach;
			sh = f < sh ? f : sh;
			reach *= 2.2;
		}
		sh = sh < 0.0 ? 0.0 : sh;
	}
	float ao = donut_field(px + nx * 0.07, py + ny * 0.07, pz + nz * 0.07, false) / 0.07;
	ao = ao < 0.35 ? 0.35 : (ao > 1.0 ? 1.0 : ao);
	float facing = 0.0 - (nx * ux + ny * uy + nz * uz);
	facing = facing < 0.0 ? 0.0 : facing;
	float rim = 1.0 - facing;
	rim = rim * rim * rim * DN_RIM;
	// the light that reaches the side the key misses: a dim ambient, a warm bounce off the
	// table onto whatever faces down, and a fill from the lit backdrop behind, so the shadow
	// side keeps its shape and is never pure black
	float wny = DN_M01 * nx + DN_M11 * ny + DN_M21 * nz;
	float bounce = wny < 0.0 ? (0.0 - wny) * 0.26 : 0.0;
	float back = 0.0 - nd;
	back = back > 0.0 ? back * 0.12 : 0.0;
	float fill = (0.13 + bounce + back) * (0.45 + 0.55 * DN_STAGE);
	float diffuse = (0.1 * DN_STAGE + fill + DN_KEY * lit * (0.25 + 0.75 * sh)) * ao + rim;
	if (mat == 2) { diffuse += 0.08 * DN_KEY * (1.0 - facing); }
	// Blinn speculars: the key light and the sweeping glint, glossier on icing and sprinkles
	float hx = lx - ux; float hy = ly - uy; float hz = lz - uz;
	float hl = 1.0 / math::sqrt(hx * hx + hy * hy + hz * hz + 0.0000001);
	float s = (nx * hx + ny * hy + nz * hz) * hl;
	s = s < 0.0 ? 0.0 : s;
	float gx = DN_SX - ux; float gy = DN_SY - uy; float gz = DN_SZ - uz;
	float gl = 1.0 / math::sqrt(gx * gx + gy * gy + gz * gz + 0.0000001);
	float g = (nx * gx + ny * gy + nz * gz) * gl;
	g = g < 0.0 ? 0.0 : g;
	float s2 = s * s; float s4 = s2 * s2; float s8 = s4 * s4; float s16 = s8 * s8;
	float g2 = g * g; float g4 = g2 * g2; float g8 = g4 * g4; float g16 = g8 * g8;
	float spec = mat <= 1 ? s8 * 0.35 : (mat == 2 ? s16 * s16 * s16 : s16 * s8);
	spec = spec * DN_KEY * sh;
	float glint = (mat <= 1 ? g8 * 0.4 : g16 * g16) * DN_GLINT;
	spec = spec > glint ? spec : glint;
	if (a_refl) {
		int lv = itrunc(diffuse * 7.99);
		return DN_REFL + mat * 8 + (lv < 0 ? 0 : (lv > 7 ? 7 : lv));
	}
	int di = itrunc(diffuse * 15.99);
	int si = itrunc(spec * 3.99);
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
	int level = 23 - itrunc(bright * 23.99);
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
				b = 1 + itrunc(lit * DN_STAGE * 7.0);
				b = b > 8 ? 8 : b;
				b += ((hs >> 24) % 3) * 8;
			}
		}
	}
	return DN_BG + (level < 0 ? 0 : (level > 23 ? 23 : level)) * 25 + b;
}

function donut_row(int y) -> array<int> {
	array<int> out = [];
	int ww = DN_W;
	int hh = DN_H;
	float sc = 1.5 / (ww * DN_ZOOM);
	float v = (hh * 0.5 - y - 0.5) * sc;
	float cx = DN_CX; float cy = DN_CY; float cz = DN_CZ;
	for (int x = 0; x < ww; ++x) {
		float u = (x + 0.5 - ww * 0.5) * sc;
		float dx = DN_FX + DN_RX * u + DN_UX * v;
		float dy = DN_FY + DN_RY * u + DN_UY * v;
		float dz = DN_FZ + DN_RZ * u + DN_UZ * v;
		float il = 1.0 / math::sqrt(dx * dx + dy * dy + dz * dz);
		dx *= il; dy *= il; dz *= il;
		float t = donut_trace(cx, cy, cz, dx, dy, dz, 48);
		if (t >= 0.0) {
			out.push(donut_shade(cx + dx * t, cy + dy * t, cz + dz * t, dx, dy, dz, false));
			continue;
		}
		if (dy >= -0.0001) {
			out.push(donut_backdrop(dx, dy, dz));
			continue;
		}
		float tf = (DN_FLOOR - cy) / dy;
		float fx = cx + dx * tf;
		float fz = cz + dz * tf;
		if (tf > 60.0) {
			out.push(donut_backdrop(dx, -0.0001, dz));
			continue;
		}
		// the table: a pool of light under the donut, walnut grain, its shadow and its reflection
		float pool = 1.0 / (1.0 + (fx * fx + fz * fz) * 0.035);
		float grain = math::sin(fx * 5.1 + math::sin(fz * 0.21) * 1.6) * 0.03 + math::sin(fx * 13.0 + fz * 0.4) * 0.015;
		float plank = fx / 2.6 - math::floor(fx / 2.6);
		if (plank < 0.025) { grain -= 0.1; }
		float sh = donut_shadow(fx, DN_FLOOR, fz, DN_WLX, DN_WLY, DN_WLZ);
		float far = 1.0 - (tf - 8.0) / 50.0;
		far = far < 0.5 ? 0.5 : (far > 1.0 ? 1.0 : far);
		float level = (0.12 * DN_STAGE + DN_KEY * pool * (0.25 + 0.75 * sh) * 0.85 + grain) * far;
		float rt = donut_trace(fx, DN_FLOOR, fz, dx, 0.0 - dy, dz, 28);
		if (rt >= 0.0 && tf < 30.0) {
			out.push(donut_shade(fx + dx * rt, DN_FLOOR - dy * rt, fz + dz * rt, dx, 0.0 - dy, dz, true));
			continue;
		}
		int lv = itrunc(level * 15.99);
		out.push(DN_TABLE + (lv < 0 ? 0 : (lv > 15 ? 15 : lv)));
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
	// ten bars: the toss on bar 7, the catch on bar 8, and the last bar cranes over the donut into its hole
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
		plan_bursts();
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
					int sx = itrunc(u / sc + w * 0.5);
					int sy = itrunc(h * 0.5 - v / sc);
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
		float kick = cue.kick_env(t);
		float snare = cue.snare_env(t);
		float energy = cue.energy(t);
		float hitv = cue.hit(t);
		bool landed = t >= land;
		bool airborne = t >= toss && t < catch_t;
		bool caught = t >= catch_t;
		// the last acts: the wind-up over bar 6, the flight through bar 7, the crane over the hole in bar 9
		float wind = smooth(bar * 6.0, toss, t);
		float winding = t >= bar * 6.0 && t < toss ? cue.riser(t) : 0.0;
		float air = airborne ? (t - toss) / bar : 0.0;
		float arc = math::sin(air * 3.1415927);
		float crane = smooth(bar * 9.0, bar * 9.0 + MUSIC_BEAT * 2.0, t);
		float push = smooth(bar * 9.0, bar * 9.75, t);
		// the donut: tipped toward the camera, spinning on its own axis, faster with the energy;
		// it rocks back and spins up through the wind-up, flips end over end in the air and
		// lands flat, rattling as it settles
		float tilt = 0.82 + 0.2 * math::sin(t * 0.55) + (landed ? 0.0 : (land - t) * 0.6);
		tilt += (-0.35 - tilt) * wind;
		if (airborne) { tilt = -0.35 - 6.0831853 * smooth(toss, catch_t - 0.08, t); }
		if (caught) {
			float settle = t - catch_t;
			tilt = -0.12 * (1.0 - crane) + 0.3 * math::sin(settle * 15.0) * math::pow(2.718281828, 0.0 - settle * 4.5);
		}
		float spin = t * (0.5 + energy * 0.35) + 0.4 + smooth(bar * 4.0, bar * 5.8, t) * 4.0 + wind * wind * 5.0;
		if (t >= toss) { spin += 5.0 * (1.0 - math::pow(2.718281828, 0.0 - (t - toss) * 1.2)); }
		float ct = math::cos(tilt); float st = math::sin(tilt);
		float cs = math::cos(spin); float ss = math::sin(spin);
		// world-from-donut R = Rx(tilt) * Ry(spin); the field wants its transpose
		DN_M00 = cs;        DN_M01 = 0.0 - ss * st;     DN_M02 = 0.0 - ss * ct;
		DN_M10 = 0.0;       DN_M11 = ct;                DN_M12 = 0.0 - st;
		DN_M20 = ss;        DN_M21 = cs * st;           DN_M22 = cs * ct;
		// the drop: falling through the title bar, landing on the downbeat with a bounce,
		// then a hop on every kick; a crouch into the toss, the arc of the flight, and the
		// catch flat on the table bouncing to rest
		float after = t - land;
		float bounce = landed ? math::abs(math::sin(after * 9.0)) * 0.9 * math::pow(2.718281828, 0.0 - after * 3.5) : 0.0;
		float fall = landed ? 0.0 : 9.0 * (1.0 - (t / land) * (t / land));
		DN_BOB = fall + bounce + (landed ? kick * 0.32 * (1.0 - wind) : 0.0) - 0.6 * wind * wind;
		if (airborne) { DN_BOB = -0.6 + (DN_REST + 0.6) * air + 30.0 * air * (1.0 - air); }
		if (caught) {
			float since = t - catch_t;
			DN_BOB = DN_REST + math::abs(math::sin(since * 11.0)) * 0.55 * math::pow(2.718281828, 0.0 - since * 4.0) + kick * (0.34 - 0.2 * crane);
		}
		DN_POP = landed ? kick * 0.045 + hitv * 0.05 + arc * 0.05 : 0.0;
		// the camera: close from the start, swinging round on each bar line, a push in on the
		// sprinkles, a pull back as it spins out, a shake on the landing; low and close for the
		// wind-up, craning after the donut through the toss, stepping round it on every beat
		// after the catch, then up over it and down into the hole
		float swing = on_bars(t, land, bar * 6.0);
		float dist = 11.0 - 1.4 * smooth(land, land + 0.4, t) - 3.2 * smooth(bar * 3.0, bar * 3.5, t)
			+ 4.0 * smooth(bar * 4.0, bar * 5.6, t);
		dist += -2.4 * wind + 4.5 * arc + 0.5 * smooth(catch_t, catch_t + 0.6, t);
		dist += (2.7 - dist) * push;
		dist -= kick * 0.3 * (1.0 - push);
		float az = -0.75 + swing * 0.62 + t * 0.04 + 0.7 * smooth(toss, catch_t, t) + on_beats(t, catch_t, bar * 9.0) * 0.55 + push * 0.6;
		float el = 0.2 + 0.06 * math::sin(t * 0.45) + 0.14 * smooth(bar * 3.0, bar * 3.5, t) - 0.1 * smooth(bar * 4.0, bar * 5.6, t);
		el += -0.16 * wind + 0.32 * arc + 0.3 * smooth(catch_t, catch_t + 0.5, t);
		el += (1.38 - el) * crane;
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
		ty += (DN_BOB - ty) * crane + math::sin((t - catch_t) * 38.0) * 0.2 * decay(t, catch_t, 7.0);
		float fx = tx - DN_CX; float fy = ty - DN_CY; float fz = tz - DN_CZ;
		float fl = 1.0 / math::sqrt(fx * fx + fy * fy + fz * fz);
		fx *= fl; fy *= fl; fz *= fl;
		float rx = fz; float rz = 0.0 - fx;
		float rl = 1.0 / math::sqrt(rx * rx + rz * rz);
		rx *= rl; rz *= rl;
		DN_FX = fx; DN_FY = fy; DN_FZ = fz;
		DN_RX = rx; DN_RY = 0.0; DN_RZ = rz;
		DN_UX = rz * fy; DN_UY = rx * fz - rz * fx; DN_UZ = 0.0 - rx * fy;
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
		float sweep1 = (t - 0.15) / (land - 0.2);
		float sweep2 = (t - bar * 4.0) / bar;
		float sweep3 = (t - bar * 6.0) / bar;
		float sw = sweep1 >= 0.0 && sweep1 <= 1.0 ? sweep1 : (sweep2 >= 0.0 && sweep2 <= 1.0 ? sweep2 : (sweep3 >= 0.0 && sweep3 <= 1.0 ? sweep3 : -1.0));
		DN_GLINT = sw < 0.0 ? 0.0 : math::sin(sw * 3.1415927) * 1.2;
		float sa = -1.4 + 2.8 * (sw < 0.0 ? 0.0 : sw);
		float wlx = -0.48; float wly = 0.74; float wlz = -0.47;
		DN_WLX = wlx; DN_WLY = wly; DN_WLZ = wlz;
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
