// OSSUARY — a horned, three-eyed skull on a spider's body crawls a landscape
// that will not hold still. The world is one raymarched field that morphs
// between three triply periodic surfaces (gyroid, Schwarz P, Schwarz D) under
// a flowing domain warp; the sheet breathes, its two labyrinths trade volume
// and it twists about the creature's axis, while a finer gyroid veins the
// walls with pulses of light. The creature walks the sheet: its body
// re-projects onto the moving level set every frame, eight two-bone IK legs
// step in a tetrapod gait on glowing joints, three tendrils writhe behind it
// and the camera orbits with the surface normal as up, so the labyrinth rolls
// around the viewer. A crown of flame floats above the skull under a spinning
// ruby; embers pour off the eyes and the crown and spiral up around it, blood
// gems orbit it, all depth-tested against the traced scene. Rows trace in
// parallel.

float SK_T = 0.0;
float SK_S = 0.8;
float SK_K = 0.0;
float SK_TW = 0.0;
float SK_TH = 0.16;
float SK_MG = 1.0;
float SK_MP = 0.0;
float SK_MD = 0.0;
float SK_WA = 0.0;
float SK_SHELL = 1.68;
float SK_CX = 0.0;
float SK_CY = 0.0;
float SK_CZ = 0.0;
float SK_EX = 0.0; float SK_EY = 0.0; float SK_EZ = 0.0;
float SK_FX = 0.0; float SK_FY = 0.0; float SK_FZ = 1.0;
float SK_RX = 1.0; float SK_RY = 0.0; float SK_RZ = 0.0;
float SK_UX = 0.0; float SK_UY = 1.0; float SK_UZ = 0.0;
float SK_BCX = 0.0; float SK_BCY = 0.0; float SK_BCZ = 0.0; float SK_BR = 1.0;
float SK_AX = 0.0; float SK_AY = 0.0; float SK_AZ = 0.0; float SK_AR = 0.24;
float SK_TX = 0.0; float SK_TY = 0.0; float SK_TZ = 0.0; float SK_TR = 0.14;
float SK_HX = 0.0; float SK_HY = 0.0; float SK_HZ = 0.0; float SK_HR = 0.24;
float SK_NX = 0.0; float SK_NY = 0.0; float SK_NZ = 0.0;
float SK_JX = 0.0; float SK_JY = 0.0; float SK_JZ = 0.0;
float SK_PFX = 0.0; float SK_PFY = 0.0; float SK_PFZ = 1.0;
float SK_PUX = 0.0; float SK_PUY = 1.0; float SK_PUZ = 0.0;
float SK_PRX = 1.0; float SK_PRY = 0.0; float SK_PRZ = 0.0;
float SK_LX = 0.0; float SK_LY = 0.0; float SK_LZ = 0.0;
float SK_FLICK = 0.0;
array<float> SK_CAX = [];
array<float> SK_CAY = [];
array<float> SK_CAZ = [];
array<float> SK_CDX = [];
array<float> SK_CDY = [];
array<float> SK_CDZ = [];
array<float> SK_CID = [];
array<float> SK_CR = [];
array<float> SK_CMX = [];
array<float> SK_CMY = [];
array<float> SK_CMZ = [];
array<float> SK_CB = [];
float SK_HGX = 0.0; float SK_HGY = 0.0; float SK_HGZ = 0.0;
float SK_BGX = 0.0; float SK_BGY = 0.0; float SK_BGZ = 0.0;
int SK_NC = 0;
int SK_NBONE = 5;
int SK_CROWN0 = 0;
float SK_CRX = 0.0; float SK_CRY = 0.0; float SK_CRZ = 0.0;
float SK_SKB = 0.4;
float SK_CRB = 0.4;
float SK_GMX = 0.0; float SK_GMY = 0.0; float SK_GMZ = 0.0;
float SK_GAX = 1.0; float SK_GAY = 0.0; float SK_GAZ = 0.0;
float SK_GBX = 0.0; float SK_GBY = 0.0; float SK_GBZ = 1.0;
array<float> SK_GX = [];
array<float> SK_GY = [];
array<float> SK_GZ = [];
array<float> SK_GR = [];
int SK_NG = 0;
int SK_NSOCK = 3;
bool SK_EYE_INSIDE = false;
int SK_W = 0;
int SK_H = 0;
int SK_WORLD = 0;
int SK_EMBER = 0;
int SK_BONE = 0;
int SK_CHIT = 0;
int SK_EYE = 0;
int SK_MARK = 0;
int SK_FOG = 0;
int SK_SPARK = 0;
int SK_GEM = 0;
array<int> SK_ROWS = [];
array<int> SK_BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];

// the landscape's level function: three surfaces blended under a flowing warp, twisted about the creature's axis
function sk_field(float x, float y, float z) -> float {
	float a = (z - SK_CZ) * SK_TW;
	float ca = math::cos(a);
	float sa = math::sin(a);
	float px = x - SK_CX;
	float py = y - SK_CY;
	float qx = (px * ca - py * sa + SK_CX) * SK_S;
	float qy = (px * sa + py * ca + SK_CY) * SK_S;
	float qz = z * SK_S;
	float wx = qx + SK_WA * math::sin(qy * 1.3 + SK_T * 0.7);
	float wy = qy + SK_WA * math::sin(qz * 1.3 + SK_T * 0.6);
	float wz = qz + SK_WA * math::sin(qx * 1.3 + SK_T * 0.8);
	float sx = math::sin(wx); float cx = math::cos(wx);
	float sy = math::sin(wy); float cy = math::cos(wy);
	float sz = math::sin(wz); float cz = math::cos(wz);
	float g = sx * cy + sy * cz + sz * cx;
	float p = cx + cy + cz;
	float d = sx * sy * sz + sx * cy * cz + cx * sy * cz + cx * cy * sz;
	return g * SK_MG + p * SK_MP + d * SK_MD * 1.4 - SK_K;
}

// the world's distance: the shell of half-thickness SK_TH about the landscape's level set, scaled to a safe step
function sk_world(float x, float y, float z) -> float {
	float g = sk_field(x, y, z);
	return ((g < 0.0 ? -g : g) - SK_TH) / SK_SHELL;
}

// capsules [first, last) against best: a capsule whose bounding sphere cannot beat best is skipped without a root
function sk_capsules(float x, float y, float z, int first, int last, float best) -> float {
	for (int i = first; i < last; ++i) {
		float mx = x - SK_CMX[i];
		float my = y - SK_CMY[i];
		float mz = z - SK_CMZ[i];
		float reach = best + SK_CB[i];
		if (mx * mx + my * my + mz * mz > reach * reach) { continue; }
		float pax = x - SK_CAX[i];
		float pay = y - SK_CAY[i];
		float paz = z - SK_CAZ[i];
		float dx = SK_CDX[i];
		float dy = SK_CDY[i];
		float dz = SK_CDZ[i];
		float hh = (pax * dx + pay * dy + paz * dz) * SK_CID[i];
		hh = hh < 0.0 ? 0.0 : (hh > 1.0 ? 1.0 : hh);
		float ex = pax - dx * hh;
		float ey = pay - dy * hh;
		float ez = paz - dz * hh;
		float d = math::sqrt(ex * ex + ey * ey + ez * ez) - SK_CR[i];
		best = d < best ? d : best;
	}
	return best;
}

function sk_glows(float x, float y, float z) -> float {
	float best = 99.0;
	for (int i = 0; i < SK_NG; ++i) {
		float dx = x - SK_GX[i];
		float dy = y - SK_GY[i];
		float dz = z - SK_GZ[i];
		float d = math::sqrt(dx * dx + dy * dy + dz * dz) - SK_GR[i];
		best = d < best ? d : best;
	}
	return best;
}

// the skull against best: an ellipsoid (up 1.18, forward sqrt(0.92)) with its jaw, eye sockets and nose cut in. It is no
// nearer than sqrt(0.92) of the distance to its center less SK_SKB (the jaw's reach), so it is skipped when that cannot beat best
function sk_skull(float x, float y, float z, float best) -> float {
	float hx = x - SK_HX;
	float hy = y - SK_HY;
	float hz = z - SK_HZ;
	if (math::sqrt(hx * hx + hy * hy + hz * hz) * 0.959 - SK_SKB >= best) { return best; }
	float cu = hx * SK_PUX + hy * SK_PUY + hz * SK_PUZ;
	float cf = hx * SK_PFX + hy * SK_PFY + hz * SK_PFZ;
	float cr = hx * SK_PRX + hy * SK_PRY + hz * SK_PRZ;
	cu = cu * 1.18;
	float d = math::sqrt(cu * cu + cf * cf * 0.92 + cr * cr) - SK_HR;
	float jx = x - SK_JX;
	float jy = y - SK_JY;
	float jz = z - SK_JZ;
	float jr = jx * SK_PRX + jy * SK_PRY + jz * SK_PRZ;
	jr = jr < -0.085 ? -0.085 : (jr > 0.085 ? 0.085 : jr);
	float qx = jx - SK_PRX * jr;
	float qy = jy - SK_PRY * jr;
	float qz = jz - SK_PRZ * jr;
	float dj = math::sqrt(qx * qx + qy * qy + qz * qz) - 0.09;
	d = d < dj ? d : dj;
	for (int i = 0; i < SK_NSOCK; ++i) {
		float sx = x - SK_GX[i];
		float sy = y - SK_GY[i];
		float sz = z - SK_GZ[i];
		float c = SK_GR[i] * 2.5 - math::sqrt(sx * sx + sy * sy + sz * sz);
		d = d > c ? d : c;
	}
	float nx = x - SK_NX; float ny = y - SK_NY; float nz = z - SK_NZ;
	float cn = 0.042 - math::sqrt(nx * nx + ny * ny + nz * nz);
	d = d > cn ? d : cn;
	return d < best ? d : best;
}

function sk_body(float x, float y, float z) -> float {
	float ax = x - SK_AX; float ay = y - SK_AY; float az = z - SK_AZ;
	float af = ax * SK_PFX + ay * SK_PFY + az * SK_PFZ;
	float au = ax * SK_PUX + ay * SK_PUY + az * SK_PUZ;
	float ar = ax * SK_PRX + ay * SK_PRY + az * SK_PRZ;
	af = af * 0.74;
	au = au * 1.12;
	float da = (math::sqrt(af * af + au * au + ar * ar) - SK_AR) * 0.74;
	float tx = x - SK_TX; float ty = y - SK_TY; float tz = z - SK_TZ;
	float dt = math::sqrt(tx * tx + ty * ty + tz * tz) - SK_TR;
	float k = 0.09;
	float hh = 0.5 + 0.5 * (dt - da) / k;
	hh = hh < 0.0 ? 0.0 : (hh > 1.0 ? 1.0 : hh);
	return dt + (da - dt) * hh - k * hh * (1.0 - hh);
}

// the crown against best: its ring, then its flames (the capsules [SK_CROWN0, SK_NC))
function sk_crown(float x, float y, float z, float best) -> float {
	float ox = x - SK_CRX; float oy = y - SK_CRY; float oz = z - SK_CRZ;
	float cu = ox * SK_PUX + oy * SK_PUY + oz * SK_PUZ;
	float px = ox - SK_PUX * cu; float py = oy - SK_PUY * cu; float pz = oz - SK_PUZ * cu;
	float ring = math::sqrt(px * px + py * py + pz * pz) - 0.2;
	float d = math::sqrt(ring * ring + cu * cu) - 0.028;
	d = best < d ? best : d;
	return sk_capsules(x, y, z, SK_CROWN0, SK_NC, d);
}

function sk_gem(float x, float y, float z) -> float {
	float ox = x - SK_GMX; float oy = y - SK_GMY; float oz = z - SK_GMZ;
	float a = ox * SK_GAX + oy * SK_GAY + oz * SK_GAZ;
	float b = ox * SK_PUX + oy * SK_PUY + oz * SK_PUZ;
	float c = ox * SK_GBX + oy * SK_GBY + oz * SK_GBZ;
	a = a < 0.0 ? -a : a;
	b = b < 0.0 ? -b : b;
	c = c < 0.0 ? -c : c;
	return (a + b * 0.62 + c - 0.075) * 0.54;
}

// the creature along one ray: only the parts the ray passes near (groups: 1 body, 2 head; cand: capsule indices, glows as 100 + index).
// A part the ray never enters changes the step lengths, never a hit.
function sk_spider(float x, float y, float z, int groups, array<int>& cand, int nc) -> float {
	float d = 99.0;
	if ((groups & 1) != 0) { d = sk_body(x, y, z); }
	if ((groups & 2) != 0) {
		d = sk_skull(x, y, z, d);
		// the crown's ring and flames lie within SK_CRB of its center
		float ox = x - SK_CRX; float oy = y - SK_CRY; float oz = z - SK_CRZ;
		if (math::sqrt(ox * ox + oy * oy + oz * oz) - SK_CRB < d) { d = sk_crown(x, y, z, d); }
		float dg = sk_gem(x, y, z);
		d = d < dg ? d : dg;
	}
	for (int k = 0; k < nc; ++k) {
		int c = cand[k];
		if (c >= 100) {
			int g = c - 100;
			float gx = x - SK_GX[g];
			float gy = y - SK_GY[g];
			float gz = z - SK_GZ[g];
			float dg2 = math::sqrt(gx * gx + gy * gy + gz * gz) - SK_GR[g];
			d = d < dg2 ? d : dg2;
		} else {
			float mx = x - SK_CMX[c];
			float my = y - SK_CMY[c];
			float mz = z - SK_CMZ[c];
			float reach = d + SK_CB[c];
			if (mx * mx + my * my + mz * mz > reach * reach) { continue; }
			float pax = x - SK_CAX[c];
			float pay = y - SK_CAY[c];
			float paz = z - SK_CAZ[c];
			float cdx = SK_CDX[c];
			float cdy = SK_CDY[c];
			float cdz = SK_CDZ[c];
			float hh = (pax * cdx + pay * cdy + paz * cdz) * SK_CID[c];
			hh = hh < 0.0 ? 0.0 : (hh > 1.0 ? 1.0 : hh);
			float ex = pax - cdx * hh;
			float ey = pay - cdy * hh;
			float ez = paz - cdz * hh;
			float dc = math::sqrt(ex * ex + ey * ey + ez * ez) - SK_CR[c];
			d = d < dc ? d : dc;
		}
	}
	return d;
}

// the ray's candidate parts cand[first, last) against best: capsule indices in ascending order, then glows as 100 + index
function sk_candidates(float x, float y, float z, array<int>& cand, int first, int last, float d) -> float {
	for (int k = first; k < last; ++k) {
		int c = cand[k];
		if (c >= 100) {
			int g = c - 100;
			float gx = x - SK_GX[g];
			float gy = y - SK_GY[g];
			float gz = z - SK_GZ[g];
			float dg2 = math::sqrt(gx * gx + gy * gy + gz * gz) - SK_GR[g];
			d = d < dg2 ? d : dg2;
		} else {
			float mx = x - SK_CMX[c];
			float my = y - SK_CMY[c];
			float mz = z - SK_CMZ[c];
			float reach = d + SK_CB[c];
			if (mx * mx + my * my + mz * mz > reach * reach) { continue; }
			float pax = x - SK_CAX[c];
			float pay = y - SK_CAY[c];
			float paz = z - SK_CAZ[c];
			float cdx = SK_CDX[c];
			float cdy = SK_CDY[c];
			float cdz = SK_CDZ[c];
			float hh = (pax * cdx + pay * cdy + paz * cdz) * SK_CID[c];
			hh = hh < 0.0 ? 0.0 : (hh > 1.0 ? 1.0 : hh);
			float ex = pax - cdx * hh;
			float ey = pay - cdy * hh;
			float ez = paz - cdz * hh;
			float dc = math::sqrt(ex * ex + ey * ey + ez * ez) - SK_CR[c];
			d = d < dc ? d : dc;
		}
	}
	return d;
}

// does the ray (origin o, unit direction d) pass within r of c ahead of the origin
function sk_ray_near(float ox, float oy, float oz, float dx, float dy, float dz, float cx, float cy, float cz, float r) -> bool {
	float vx = cx - ox;
	float vy = cy - oy;
	float vz = cz - oz;
	float tc = vx * dx + vy * dy + vz * dz;
	if (tc < -r) { return false; }
	return vx * vx + vy * vy + vz * vz - tc * tc < r * r;
}

// 0 world, 1 bone, 2 chitin, 3 glow, 4 marking, 5 ruby at a point on the ray the wall is dw from: the nearest wins, ties to the
// earlier. Each capsule run starts from the best so far, so a capsule that cannot win costs only its bounding test; and a capsule
// the ray passed no nearer than 0.01 to (not among its candidates: cand[0, nbone) bone, then chitin up to ncap) is that far from
// every point on it, so where the wall is nearer only the candidates can win
function sk_material(float x, float y, float z, float dw, array<int>& cand, int nbone, int ncap) -> int {
	float best = dw;
	int m = 0;
	bool candidates_only = dw < 0.009;
	float bone = sk_skull(x, y, z, best);
	if (candidates_only) { bone = sk_candidates(x, y, z, cand, 0, nbone, bone); } else { bone = sk_capsules(x, y, z, 0, SK_NBONE, bone); }
	if (bone < best) { best = bone; m = 1; }
	float db = sk_body(x, y, z);
	float chit = db < best ? db : best;
	if (candidates_only) { chit = sk_candidates(x, y, z, cand, nbone, ncap, chit); } else { chit = sk_capsules(x, y, z, SK_NBONE, SK_CROWN0, chit); }
	if (chit < best) { best = chit; m = 2; }
	float de = sk_glows(x, y, z);
	de = de < best ? de : best;
	float ox = x - SK_CRX; float oy = y - SK_CRY; float oz = z - SK_CRZ;
	if (math::sqrt(ox * ox + oy * oy + oz * oz) - SK_CRB < de) { de = sk_crown(x, y, z, de); }
	if (de < best) { best = de; m = 3; }
	float dgm = sk_gem(x, y, z);
	if (dgm < best) { m = 5; }
	if (m == 2 && chit == db) {
		float ax = x - SK_AX; float ay = y - SK_AY; float az = z - SK_AZ;
		float au = ax * SK_PUX + ay * SK_PUY + az * SK_PUZ;
		float ar = ax * SK_PRX + ay * SK_PRY + az * SK_PRZ;
		float af = ax * SK_PFX + ay * SK_PFY + az * SK_PFZ;
		float ar2 = ar < 0.0 ? -ar : ar;
		float af2 = af < 0.0 ? -af : af;
		if (au > 0.09 && ar2 < 0.03 + af2 * 0.2 && af > -0.28 && af < 0.18) { m = 4; }
	}
	return m;
}

// a pixel's palette index plus its depth (0..255) above bit 10, for the particle pass; row: the row's candidate parts (see sk_row)
function sk_trace(float dx, float dy, float dz, int px, int py, array<int>& cand, array<int>& row, int nrow) -> int {
	float ox = SK_EX;
	float oy = SK_EY;
	float oz = SK_EZ;
	int groups = 0;
	int nc = 0;
	int nbone = 0;
	int ncap = 0;
	if (sk_ray_near(ox, oy, oz, dx, dy, dz, SK_BGX, SK_BGY, SK_BGZ, 0.62)) { groups = 1; }
	if (sk_ray_near(ox, oy, oz, dx, dy, dz, SK_HGX, SK_HGY, SK_HGZ, 0.74)) { groups = groups + 2; }
	for (int k = 0; k < nrow; ++k) {
		int c = row[k];
		if (c < 100) {
			if (sk_ray_near(ox, oy, oz, dx, dy, dz, SK_CMX[c], SK_CMY[c], SK_CMZ[c], SK_CB[c] + 0.01)) {
				cand[nc] = c;
				nc = nc + 1;
				ncap = nc;
				if (c < SK_NBONE) { nbone = nc; }
			}
		} else {
			int g = c - 100;
			if (sk_ray_near(ox, oy, oz, dx, dy, dz, SK_GX[g], SK_GY[g], SK_GZ[g], SK_GR[g] + 0.01)) {
				cand[nc] = c;
				nc = nc + 1;
			}
		}
	}
	bool near = groups != 0 || nc != 0;
	float t = 0.04;
	float glow = 9.0;
	if (!near) {
		float vx = SK_BCX - ox;
		float vy = SK_BCY - oy;
		float vz = SK_BCZ - oz;
		float tc = vx * dx + vy * dy + vz * dz;
		float perp = vx * vx + vy * vy + vz * vz - tc * tc;
		glow = tc < 0.0 ? 9.0 : math::sqrt(perp < 0.0 ? 0.0 : perp) - 0.55;
	}
	bool hit = false;
	bool inside = SK_EYE_INSIDE;
	// the creature's distance is Lipschitz: 1.18 where the skull's long axis is among the ray's parts, else 1
	float lip = (groups & 2) != 0 ? 1.2 : 1.0;
	float ds_last = 0.0;
	float t_last = 0.0;
	float wall = 0.0;
	int i = 0;
	while (i < 64) {
		float x = ox + dx * t;
		float y = oy + dy * t;
		float z = oz + dz * t;
		float dw = sk_world(x, y, z);
		if (inside) {
			// the camera sits inside a wall: the first slab along the ray is cut away
			if (dw < 0.0) {
				t += 0.01 - dw;
				i = i + 1;
				continue;
			}
			inside = false;
		}
		float d = dw;
		if (near) {
			// while the creature's last distance, less what the ray has closed since, stays above the wall's distance and
			// above the glow so far or the aura's reach (0.5), it can change neither the step nor the aura
			float bound = ds_last - (t - t_last) * lip - 0.001;
			if (bound < dw || (bound < glow && bound < 0.5)) {
				float ds = sk_spider(x, y, z, groups, cand, nc);
				glow = ds < glow ? ds : glow;
				d = dw < ds ? dw : ds;
				ds_last = ds;
				t_last = t;
			}
		}
		if (d < 0.001 + t * 0.0015) {
			hit = true;
			wall = dw;
			break;
		}
		t += d * 0.85;
		if (t > 14.0) { break; }
		i = i + 1;
	}
	float dith = SK_BAYER[(py % 4) * 4 + px % 4] / 16.0 - 0.47;
	if (!hit) {
		float aura = 1.0 - glow * 2.0;
		aura = aura < 0.0 ? 0.0 : aura;
		int f = itrunc(aura * aura * 9.0 + dith + 0.5 + i * 0.015);
		return SK_FOG + (f < 0 ? 0 : (f > 9 ? 9 : f)) + 255 * 1024;
	}
	int depth = itrunc(t * 18.0);
	depth = (depth > 254 ? 254 : depth) * 1024;
	float x = ox + dx * t;
	float y = oy + dy * t;
	float z = oz + dz * t;
	int m = sk_material(x, y, z, wall, cand, nbone, ncap);
	float fog = 1.0 - t / 14.0;
	fog = fog < 0.0 ? 0.0 : fog;
	float lx = SK_LX - x;
	float ly = SK_LY - y;
	float lz = SK_LZ - z;
	float ld2 = lx * lx + ly * ly + lz * lz;
	float lil = 1.0 / math::sqrt(ld2 + 0.0001);
	lx *= lil; ly *= lil; lz *= lil;
	if (m == 0) {
		float hs = 0.004;
		float n0 = sk_field(x + hs, y - hs, z - hs);
		float n1 = sk_field(x - hs, y - hs, z + hs);
		float n2 = sk_field(x - hs, y + hs, z - hs);
		float n3 = sk_field(x + hs, y + hs, z + hs);
		float g = (n0 + n1 + n2 + n3) * 0.25;
		float nx = n0 - n1 - n2 + n3;
		float ny = -n0 - n1 + n2 + n3;
		float nz = -n0 + n1 - n2 + n3;
		float sg = g < 0.0 ? -1.0 : 1.0;
		float nl = sg / math::sqrt(nx * nx + ny * ny + nz * nz + 0.0000001);
		nx *= nl; ny *= nl; nz *= nl;
		float head = -(nx * dx + ny * dy + nz * dz);
		head = head < 0.0 ? 0.0 : head;
		float ember = nx * lx + ny * ly + nz * lz;
		ember = ember < 0.0 ? 0.0 : ember * 1.6 / (1.0 + ld2 * 3.0);
		float s = SK_S * 3.1;
		float vx = x * s;
		float vy = y * s;
		float vz = z * s;
		float vein = math::sin(vx) * math::cos(vy) + math::sin(vy) * math::cos(vz) + math::sin(vz) * math::cos(vx);
		float va = vein < 0.0 ? -vein : vein;
		float pulse = 0.5 + 0.5 * math::sin(vein * 5.0 - SK_T * 3.4 + vz * 0.25);
		float ao = 1.0 - i * 0.011;
		ao = ao < 0.3 ? 0.3 : ao;
		float lit = (0.06 + head * 0.7) * fog * ao;
		if (va < 0.3) {
			float edge = 1.0 - va / 0.3;
			float glowv = pulse * pulse * edge * 1.15 * (0.35 + fog * 0.65);
			lit = glowv > lit ? glowv : lit * 0.4;
		}
		if (ember * fog > lit * 1.15 + (va < 0.3 ? 0.25 : 0.0)) {
			int e = itrunc((ember * fog * 0.85 + lit * 0.3) * 11.0 + dith + 0.5);
			return SK_EMBER + (e < 0 ? 0 : (e > 11 ? 11 : e)) + depth;
		}
		float hue = (vx * 0.11 + vy * 0.07 - vz * 0.05) * 0.3 + g * 0.35 + SK_T * 0.05 + t * 0.035 + (va < 0.3 ? 0.33 : 0.0);
		hue = hue - math::floor(hue);
		int hq = itrunc(hue * 12.0);
		hq = hq > 11 ? 11 : hq;
		int l = itrunc(lit * 11.0 + dith + 0.5);
		l = l < 0 ? 0 : (l > 11 ? 11 : l);
		return SK_WORLD + hq * 12 + l + depth;
	}
	if (m == 3) {
		int e = itrunc(2.4 + SK_FLICK + dith);
		return SK_EYE + (e < 0 ? 0 : (e > 3 ? 3 : e)) + depth;
	}
	if (m == 5) {
		float ox = x - SK_GMX; float oy = y - SK_GMY; float oz = z - SK_GMZ;
		float a = ox * SK_GAX + oy * SK_GAY + oz * SK_GAZ;
		float b = ox * SK_PUX + oy * SK_PUY + oz * SK_PUZ;
		float c = ox * SK_GBX + oy * SK_GBY + oz * SK_GBZ;
		float sa = a < 0.0 ? -1.0 : 1.0;
		float sb = b < 0.0 ? -1.0 : 1.0;
		float sc = c < 0.0 ? -1.0 : 1.0;
		float gnx = SK_GAX * sa + SK_PUX * sb * 0.62 + SK_GBX * sc;
		float gny = SK_GAY * sa + SK_PUY * sb * 0.62 + SK_GBY * sc;
		float gnz = SK_GAZ * sa + SK_PUZ * sb * 0.62 + SK_GBZ * sc;
		float gl = 1.0 / math::sqrt(gnx * gnx + gny * gny + gnz * gnz);
		float face = -(gnx * dx + gny * dy + gnz * dz) * gl;
		face = face < 0.0 ? 0.0 : face;
		int facet = (sa > 0.0 ? 1 : 0) + (sb > 0.0 ? 2 : 0) + (sc > 0.0 ? 4 : 0);
		float twinkle = math::sin(SK_T * 9.0 + facet * 2.3);
		float lit = 0.25 + face * face * 0.6 + (twinkle > 0.85 ? 0.6 : 0.0);
		int r = itrunc(lit * 7.0 + dith + 0.5);
		return SK_GEM + (r < 0 ? 0 : (r > 7 ? 7 : r)) + depth;
	}
	float hstep = 0.0015;
	float n0 = sk_spider(x + hstep, y - hstep, z - hstep, groups, cand, nc);
	float n1 = sk_spider(x - hstep, y - hstep, z + hstep, groups, cand, nc);
	float n2 = sk_spider(x - hstep, y + hstep, z - hstep, groups, cand, nc);
	float n3 = sk_spider(x + hstep, y + hstep, z + hstep, groups, cand, nc);
	float nx = n0 - n1 - n2 + n3;
	float ny = -n0 - n1 + n2 + n3;
	float nz = -n0 + n1 - n2 + n3;
	float nl = 1.0 / math::sqrt(nx * nx + ny * ny + nz * nz + 0.0000001);
	nx *= nl; ny *= nl; nz *= nl;
	float vd = -(nx * dx + ny * dy + nz * dz);
	vd = vd < 0.0 ? 0.0 : vd;
	float up = nx * SK_PUX + ny * SK_PUY + nz * SK_PUZ;
	up = up < 0.0 ? 0.0 : up;
	float rim = 1.0 - vd;
	rim = rim * rim * rim;
	float eyel = nx * lx + ny * ly + nz * lz;
	eyel = eyel < 0.0 ? 0.0 : eyel / (1.0 + ld2 * 6.0);
	if (m == 1) {
		float lit = 0.1 + vd * 0.55 + up * 0.22 + rim * 0.25;
		float so = 99.0;
		for (int k = 0; k < SK_NSOCK; ++k) {
			float ox2 = x - SK_GX[k]; float oy2 = y - SK_GY[k]; float oz2 = z - SK_GZ[k];
			float q = math::sqrt(ox2 * ox2 + oy2 * oy2 + oz2 * oz2) - SK_GR[k] * 2.5;
			so = q < so ? q : so;
		}
		if (so < 0.012) { lit = lit * 0.15 + 0.07 * (1.0 + SK_FLICK); }
		float nsx = x - SK_NX; float nsy = y - SK_NY; float nsz = z - SK_NZ;
		if (nsx * nsx + nsy * nsy + nsz * nsz < 0.0025) { lit = lit * 0.2; }
		float jx = x - SK_JX; float jy = y - SK_JY; float jz = z - SK_JZ;
		float jf = jx * SK_PFX + jy * SK_PFY + jz * SK_PFZ;
		float ju = jx * SK_PUX + jy * SK_PUY + jz * SK_PUZ;
		if (jf > 0.045 && ju > -0.05 && ju < 0.042) {
			float tr = (jx * SK_PRX + jy * SK_PRY + jz * SK_PRZ) * 44.0;
			if (tr - math::floor(tr) < 0.24 || (ju > -0.008 && ju < 0.006)) { lit = lit * 0.22; }
		}
		int b = itrunc(lit * fog * 11.0 + dith + 0.5);
		return SK_BONE + (b < 0 ? 0 : (b > 11 ? 11 : b)) + depth;
	}
	if (m == 4) {
		int k = itrunc((0.45 + vd * 0.45 + 0.3 * math::sin(SK_T * 4.0)) * 5.0 + dith + 0.5);
		return SK_MARK + (k < 0 ? 0 : (k > 5 ? 5 : k)) + depth;
	}
	float lit = 0.03 + vd * 0.16 + rim * 0.62 + eyel * 0.9;
	int c = itrunc(lit * fog * 11.0 + dith + 0.5);
	return SK_CHIT + (c < 0 ? 0 : (c > 11 ? 11 : c)) + depth;
}

function sk_row(int y) -> array<int> {
	array<int> out = [];
	array<int> cand = [];
	for (int k = 0; k < 64; ++k) { cand.push(0); }
	int ww = SK_W;
	float half_h = SK_H * 0.5;
	float zoom = 0.6 / half_h;
	float v = (half_h - y - 0.5) * zoom;
	// the row's rays all lie in the plane through the eye spanned by R and F + U v: a capsule (0..SK_CROWN0) or a leg glow
	// (100 + g) farther from that plane than its reach passes near none of them, so the pixels test only the rest
	float ax = SK_FX + SK_UX * v;
	float ay = SK_FY + SK_UY * v;
	float az = SK_FZ + SK_UZ * v;
	float nx = SK_RY * az - SK_RZ * ay;
	float ny = SK_RZ * ax - SK_RX * az;
	float nz = SK_RX * ay - SK_RY * ax;
	float nl = 1.0 / math::sqrt(nx * nx + ny * ny + nz * nz);
	array<int> row = [];
	for (int c = 0; c < SK_CROWN0; ++c) {
		float off = ((SK_CMX[c] - SK_EX) * nx + (SK_CMY[c] - SK_EY) * ny + (SK_CMZ[c] - SK_EZ) * nz) * nl;
		if (math::abs(off) < SK_CB[c] + 0.02) { row.push(c); }
	}
	for (int g = 3; g < SK_NG; ++g) {
		float off = ((SK_GX[g] - SK_EX) * nx + (SK_GY[g] - SK_EY) * ny + (SK_GZ[g] - SK_EZ) * nz) * nl;
		if (math::abs(off) < SK_GR[g] + 0.02) { row.push(100 + g); }
	}
	int nrow = row.size();
	for (int x = 0; x < ww; ++x) {
		float u = (x + 0.5 - ww * 0.5) * zoom;
		float dx = SK_FX + SK_RX * u + SK_UX * v;
		float dy = SK_FY + SK_RY * u + SK_UY * v;
		float dz = SK_FZ + SK_RZ * u + SK_UZ * v;
		float il = 1.0 / math::sqrt(dx * dx + dy * dy + dz * dz);
		out.push(sk_trace(dx * il, dy * il, dz * il, x, y, cand, row, nrow));
	}
	return out;
}

// the sheet's surface on the + side: Newton steps onto sk_field == SK_TH
function sk_project(float& x, float& y, float& z) -> void {
	for (int k = 0; k < 5; ++k) {
		float e = 0.004;
		float g = sk_field(x, y, z) - SK_TH;
		float gx = (sk_field(x + e, y, z) - sk_field(x - e, y, z)) / (2.0 * e);
		float gy = (sk_field(x, y + e, z) - sk_field(x, y - e, z)) / (2.0 * e);
		float gz = (sk_field(x, y, z + e) - sk_field(x, y, z - e)) / (2.0 * e);
		float q = g / (gx * gx + gy * gy + gz * gz + 0.000001);
		x -= gx * q;
		y -= gy * q;
		z -= gz * q;
	}
}

class Ossuary : Scene {
	float bx = 0.4; float by = 0.3; float bz = 0.2;
	float fx = 0.0; float fy = 0.0; float fz = 1.0;
	float ux = 0.0; float uy = 1.0; float uz = 0.0;
	float rx = 1.0; float ry = 0.0; float rz = 0.0;
	float cxs = 0.0; float cys = 0.0; float czs = 0.0;
	float ex = 0.0; float ey = 0.0; float ez = 0.0;
	float cux = 0.0; float cuy = 1.0; float cuz = 0.0;
	bool placed = false;
	array<float> footx = [];
	array<float> footy = [];
	array<float> footz = [];
	array<float> fromx = [];
	array<float> fromy = [];
	array<float> fromz = [];
	array<float> tox = [];
	array<float> toy = [];
	array<float> toz = [];
	array<float> stept = [];
	array<float> legfo = [0.3, 0.1, -0.1, -0.3];
	array<float> legreach = [0.42, 0.14, -0.08, -0.36];
	array<float> ppx = [];
	array<float> ppy = [];
	array<float> ppz = [];
	array<float> pvx = [];
	array<float> pvy = [];
	array<float> pvz = [];
	array<float> plife = [];
	array<float> prate = [];
	array<int> depth = [];
	int np = 240;
	int pnext = 0;
	float l1 = 0.5;
	float l2 = 0.66;
	float tt = 0.0;
	float wander = 0.0;
	float spawn_acc = 0.0;

	void build() override {
		name = "OSSUARY";
		handle = "eight legs of inverse kinematics . a landscape that will not hold still";
		SK_WORLD = pal.size();
		for (int hq = 0; hq < 12; ++hq) {
			var c = gfx.rainbow_rgb(hq * 0.5236 + 2.2);
			for (int s = 0; s < 12; ++s) {
				float f = s / 11.0;
				float k = f * f * 0.9 + f * 0.25;
				float wh = f > 0.82 ? (f - 0.82) * 2.2 : 0.0;
				float r = 5.0 + c[0] * k * 0.9;
				float g = 2.0 + c[1] * k * 0.9;
				float b = 10.0 + c[2] * k * 0.9;
				pal.add(r + (255.0 - r) * wh, g + (255.0 - g) * wh, b + (255.0 - b) * wh);
			}
		}
		SK_EMBER = pal.size();
		for (int e = 0; e < 12; ++e) {
			float f = e / 11.0;
			pal.add(18.0 + 237.0 * f, 2.0 + 150.0 * f * f * f, 6.0 + 60.0 * f * f * f * f);
		}
		SK_BONE = pal.ramp(12, 26.0, 20.0, 16.0, 250.0, 238.0, 206.0);
		SK_CHIT = pal.size();
		for (int k = 0; k < 12; ++k) {
			float f = k / 11.0;
			pal.add(6.0 + 200.0 * f * f + 49.0 * f * f * f * f, 3.0 + 20.0 * f * f * f + 150.0 * f * f * f * f * f * f, 8.0 + 30.0 * f * f + 60.0 * f * f * f * f * f * f);
		}
		SK_EYE = pal.ramp(4, 220.0, 30.0, 10.0, 255.0, 250.0, 190.0);
		SK_MARK = pal.ramp(6, 90.0, 0.0, 8.0, 255.0, 60.0, 30.0);
		SK_FOG = pal.size();
		for (int g = 0; g < 10; ++g) {
			float f = g / 9.0;
			pal.add(2.0 + 110.0 * f * f, 1.0 + 12.0 * f * f * f, 5.0 + 22.0 * f);
		}
		SK_SPARK = pal.size();
		for (int k = 0; k < 8; ++k) {
			float f = k / 7.0;
			pal.add(90.0 + 165.0 * f, 8.0 + 120.0 * f * f + 120.0 * f * f * f * f, 4.0 + 40.0 * f * f * f + 180.0 * f * f * f * f * f * f);
		}
		SK_GEM = pal.size();
		for (int k = 0; k < 8; ++k) {
			float f = k / 7.0;
			float wh = f > 0.75 ? (f - 0.75) * 3.2 : 0.0;
			float r = 40.0 + 215.0 * f;
			float g = 0.0 + 30.0 * f * f;
			float b = 12.0 + 50.0 * f * f;
			pal.add(r + (255.0 - r) * wh, g + (255.0 - g) * wh, b + (255.0 - b) * wh);
		}
		for (int i = 0; i < 8; ++i) {
			footx.push(0.0); footy.push(0.0); footz.push(0.0);
			fromx.push(0.0); fromy.push(0.0); fromz.push(0.0);
			tox.push(0.0); toy.push(0.0); toz.push(0.0);
			stept.push(-1.0);
		}
		for (int i = 0; i < np; ++i) {
			ppx.push(0.0); ppy.push(0.0); ppz.push(0.0);
			pvx.push(0.0); pvy.push(0.0); pvz.push(0.0);
			plife.push(0.0); prate.push(1.0);
		}
		wander = rng.next(628) * 0.01;
	}

	void set_world(float t) {
		SK_T = t;
		SK_S = 0.8 + 0.07 * math::sin(t * 0.37);
		SK_K = 0.45 * math::sin(t * 0.21 + 0.6);
		SK_TW = 0.11 * math::sin(t * 0.29 + 1.3);
		SK_WA = 0.18 + 0.14 * math::sin(t * 0.33 + 2.0);
		SK_SHELL = SK_S * (2.1 + SK_WA * 2.4);
		float ph = t * 0.19;
		float a = 0.5 + 0.5 * math::cos(ph);
		float b = 0.5 + 0.5 * math::cos(ph - 2.0944);
		float c = 0.5 + 0.5 * math::cos(ph + 2.0944);
		a = a * a * a; b = b * b * b; c = c * c * c;
		float sum = a + b + c;
		SK_MG = a / sum;
		SK_MP = b / sum;
		SK_MD = c / sum;
		SK_CX = cxs; SK_CY = cys; SK_CZ = czs;
	}

	void frame_from_normal() {
		float e = 0.004;
		float gx = sk_field(bx + e, by, bz) - sk_field(bx - e, by, bz);
		float gy = sk_field(bx, by + e, bz) - sk_field(bx, by - e, bz);
		float gz = sk_field(bx, by, bz + e) - sk_field(bx, by, bz - e);
		float gl = 1.0 / math::sqrt(gx * gx + gy * gy + gz * gz + 0.0000001);
		ux = gx * gl; uy = gy * gl; uz = gz * gl;
		float d = fx * ux + fy * uy + fz * uz;
		fx -= ux * d; fy -= uy * d; fz -= uz * d;
		float fl = math::sqrt(fx * fx + fy * fy + fz * fz);
		if (fl < 0.001) {
			fx = uy; fy = uz; fz = ux;
			d = fx * ux + fy * uy + fz * uz;
			fx -= ux * d; fy -= uy * d; fz -= uz * d;
			fl = math::sqrt(fx * fx + fy * fy + fz * fz);
		}
		fx /= fl; fy /= fl; fz /= fl;
		rx = uy * fz - uz * fy;
		ry = uz * fx - ux * fz;
		rz = ux * fy - uy * fx;
	}

	void rest_foot(int i, float lead) {
		float side = i < 4 ? 1.0 : -1.0;
		int j = i % 4;
		float reach = legreach[j] + lead;
		float x = bx + fx * reach + rx * side * 0.78;
		float y = by + fy * reach + ry * side * 0.78;
		float z = bz + fz * reach + rz * side * 0.78;
		sk_project(x, y, z);
		tox[i] = x; toy[i] = y; toz[i] = z;
	}

	float jitter(int n) { return (rng.next(n * 2 + 1) - n) / (n * 1.0); }

	void spawn(float x, float y, float z, float vx, float vy, float vz, float life) {
		int i = pnext;
		pnext = (pnext + 1) % np;
		ppx[i] = x; ppy[i] = y; ppz[i] = z;
		pvx[i] = vx; pvy[i] = vy; pvz[i] = vz;
		plife[i] = 1.0;
		prate[i] = 1.0 / life;
	}

	void update_particles(float dt) {
		float hx = bx + ux * 0.2;
		float hy = by + uy * 0.2;
		float hz = bz + uz * 0.2;
		spawn_acc += dt * 110.0;
		while (spawn_acc >= 1.0) {
			spawn_acc -= 1.0;
			int kind = rng.next(14);
			if (kind >= 10) {
				float ca2 = rng.next(628) * 0.01;
				float cxr = math::cos(ca2) * 0.2;
				float sxr = math::sin(ca2) * 0.2;
				float crx = hx + fx * 0.34 + ux * 0.55;
				float cry = hy + fy * 0.34 + uy * 0.55;
				float crz = hz + fz * 0.34 + uz * 0.55;
				spawn(crx + fx * cxr + rx * sxr, cry + fy * cxr + ry * sxr, crz + fz * cxr + rz * sxr, ux * 0.9 + fx * cxr + rx * sxr, uy * 0.9 + fy * cxr + ry * sxr, uz * 0.9 + fz * cxr + rz * sxr, 0.45 + rng.next(40) * 0.01);
			} else if (kind < 4) {
				float ex2 = hx + fx * 0.52 + ux * 0.14 + rx * jitter(100) * 0.09;
				float ey2 = hy + fy * 0.52 + uy * 0.14 + ry * jitter(100) * 0.09;
				float ez2 = hz + fz * 0.52 + uz * 0.14 + rz * jitter(100) * 0.09;
				spawn(ex2, ey2, ez2, fx * 0.5 + ux * 0.35 + rx * jitter(100) * 0.3, fy * 0.5 + uy * 0.35 + ry * jitter(100) * 0.3, fz * 0.5 + uz * 0.35 + rz * jitter(100) * 0.3, 1.1 + rng.next(80) * 0.01);
			} else if (kind < 6) {
				spawn(hx - fx * 0.55, hy - fy * 0.55, hz - fz * 0.55, -fx * 0.3 + ux * 0.4 + jitter(100) * 0.2, -fy * 0.3 + uy * 0.4 + jitter(100) * 0.2, -fz * 0.3 + uz * 0.4 + jitter(100) * 0.2, 1.4 + rng.next(80) * 0.01);
			} else {
				float r = 0.6 + rng.next(140) * 0.01;
				float a = rng.next(628) * 0.01;
				float ca = math::cos(a) * r;
				float sa = math::sin(a) * r;
				spawn(hx + fx * ca + rx * sa - ux * 0.1, hy + fy * ca + ry * sa - uy * 0.1, hz + fz * ca + rz * sa - uz * 0.1, ux * 0.25, uy * 0.25, uz * 0.25, 2.0 + rng.next(100) * 0.01);
			}
		}
		float drag = 1.0 - dt * 0.9;
		for (int i = 0; i < np; ++i) {
			if (plife[i] <= 0.0) { continue; }
			float ox = ppx[i] - hx;
			float oy = ppy[i] - hy;
			float oz = ppz[i] - hz;
			float sx = uy * oz - uz * oy;
			float sy = uz * ox - ux * oz;
			float sz = ux * oy - uy * ox;
			pvx[i] = (pvx[i] + (ux * 0.9 + sx * 1.3 - ox * 0.25) * dt) * drag;
			pvy[i] = (pvy[i] + (uy * 0.9 + sy * 1.3 - oy * 0.25) * dt) * drag;
			pvz[i] = (pvz[i] + (uz * 0.9 + sz * 1.3 - oz * 0.25) * dt) * drag;
			ppx[i] = ppx[i] + pvx[i] * dt;
			ppy[i] = ppy[i] + pvy[i] * dt;
			ppz[i] = ppz[i] + pvz[i] * dt;
			plife[i] = plife[i] - dt * prate[i];
		}
	}

	void update(float t, float dt) override {
		tt = t;
		if (!placed) {
			cxs = bx; cys = by; czs = bz;
		}
		set_world(t);
		float turn = 0.75 * math::sin(t * 0.53 + wander) + 0.45 * math::sin(t * 1.37 + wander * 2.0) + 0.2 * math::sin(t * 2.9);
		float ang = turn * dt * 1.6;
		float c = math::cos(ang);
		float s = math::sin(ang);
		float nfx = fx * c + rx * s;
		float nfy = fy * c + ry * s;
		float nfz = fz * c + rz * s;
		fx = nfx; fy = nfy; fz = nfz;
		float speed = 0.55 + 0.2 * math::sin(t * 0.8);
		bx += fx * speed * dt;
		by += fy * speed * dt;
		bz += fz * speed * dt;
		sk_project(bx, by, bz);
		frame_from_normal();
		float k = dt * 0.6;
		k = k > 1.0 ? 1.0 : k;
		cxs += (bx - cxs) * k; cys += (by - cys) * k; czs += (bz - czs) * k;
		SK_CX = cxs; SK_CY = cys; SK_CZ = czs;
		if (!placed) {
			for (int i = 0; i < 8; ++i) {
				rest_foot(i, 0.0);
				footx[i] = tox[i]; footy[i] = toy[i]; footz[i] = toz[i];
			}
		}
		bool stepping_a = false;
		bool stepping_b = false;
		for (int i = 0; i < 8; ++i) {
			if (stept[i] >= 0.0) {
				if ((i + i / 4) % 2 == 0) { stepping_a = true; } else { stepping_b = true; }
			}
		}
		for (int i = 0; i < 8; ++i) {
			bool group_a = (i + i / 4) % 2 == 0;
			if (stept[i] >= 0.0) {
				stept[i] = stept[i] + dt / 0.17;
				rest_foot(i, 0.2);
				if (stept[i] >= 1.0) {
					stept[i] = -1.0;
					footx[i] = tox[i]; footy[i] = toy[i]; footz[i] = toz[i];
				} else {
					float e = stept[i];
					float lift = math::sin(e * 3.14159265) * 0.22;
					footx[i] = fromx[i] + (tox[i] - fromx[i]) * e + ux * lift;
					footy[i] = fromy[i] + (toy[i] - fromy[i]) * e + uy * lift;
					footz[i] = fromz[i] + (toz[i] - fromz[i]) * e + uz * lift;
				}
			} else {
				float px = footx[i];
				float py = footy[i];
				float pz = footz[i];
				sk_project(px, py, pz);
				footx[i] = px; footy[i] = py; footz[i] = pz;
				rest_foot(i, 0.0);
				float ddx = footx[i] - tox[i];
				float ddy = footy[i] - toy[i];
				float ddz = footz[i] - toz[i];
				bool other_moving = group_a ? stepping_b : stepping_a;
				if (!other_moving && ddx * ddx + ddy * ddy + ddz * ddz > 0.1) {
					stept[i] = 0.0;
					fromx[i] = footx[i]; fromy[i] = footy[i]; fromz[i] = footz[i];
					if (group_a) { stepping_a = true; } else { stepping_b = true; }
				}
			}
		}
		update_particles(dt);
		float bob = 0.2 + 0.018 * math::sin(t * 11.0);
		float hx = bx + ux * bob;
		float hy = by + uy * bob;
		float hz = bz + uz * bob;
		float orbit = 2.4 + t * 0.33 + 0.5 * math::sin(t * 0.17);
		float oc = math::cos(orbit) * 2.1;
		float os = math::sin(orbit) * 2.1;
		float dex = hx + fx * oc + rx * os + ux * 1.0;
		float dey = hy + fy * oc + ry * os + uy * 1.0;
		float dez = hz + fz * oc + rz * os + uz * 1.0;
		float cdx = dex - hx;
		float cdy = dey - hy;
		float cdz = dez - hz;
		float want = math::sqrt(cdx * cdx + cdy * cdy + cdz * cdz);
		cdx /= want; cdy /= want; cdz /= want;
		float reach = 1.5;
		while (reach < want) {
			float d = sk_world(hx + cdx * reach, hy + cdy * reach, hz + cdz * reach);
			if (d < 0.14) { break; }
			reach += d * 0.8;
		}
		reach = reach > want ? want : reach;
		reach = reach - 0.1;
		reach = reach < 1.5 ? 1.5 : reach;
		dex = hx + cdx * reach;
		dey = hy + cdy * reach;
		dez = hz + cdz * reach;
		float kc = placed ? dt * 2.8 : 1.0;
		kc = kc > 1.0 ? 1.0 : kc;
		ex += (dex - ex) * kc; ey += (dey - ey) * kc; ez += (dez - ez) * kc;
		float ku = placed ? dt * 2.2 : 1.0;
		ku = ku > 1.0 ? 1.0 : ku;
		cux += (ux - cux) * ku; cuy += (uy - cuy) * ku; cuz += (uz - cuz) * ku;
		placed = true;
	}

	void capsule(int i, float ax, float ay, float az, float bx2, float by2, float bz2, float r) {
		float dx = bx2 - ax;
		float dy = by2 - ay;
		float dz = bz2 - az;
		SK_CAX[i] = ax; SK_CAY[i] = ay; SK_CAZ[i] = az;
		SK_CDX[i] = dx; SK_CDY[i] = dy; SK_CDZ[i] = dz;
		SK_CID[i] = 1.0 / (dx * dx + dy * dy + dz * dz + 0.000001);
		SK_CR[i] = r;
		SK_CMX[i] = ax + dx * 0.5; SK_CMY[i] = ay + dy * 0.5; SK_CMZ[i] = az + dz * 0.5;
		SK_CB[i] = math::sqrt(dx * dx + dy * dy + dz * dz) * 0.5 + r;
	}

	void glow(int i, float x, float y, float z, float r) {
		SK_GX[i] = x; SK_GY[i] = y; SK_GZ[i] = z; SK_GR[i] = r;
	}

	// a curved horn: base, a bend out and back, a tip up and back
	int horn(int i, float side) {
		float bx0 = SK_HX + SK_PUX * 0.16 + SK_PRX * side * 0.12 - SK_PFX * 0.04;
		float by0 = SK_HY + SK_PUY * 0.16 + SK_PRY * side * 0.12 - SK_PFY * 0.04;
		float bz0 = SK_HZ + SK_PUZ * 0.16 + SK_PRZ * side * 0.12 - SK_PFZ * 0.04;
		float mx = bx0 + SK_PRX * side * 0.2 + SK_PUX * 0.12 - SK_PFX * 0.12;
		float my = by0 + SK_PRY * side * 0.2 + SK_PUY * 0.12 - SK_PFY * 0.12;
		float mz = bz0 + SK_PRZ * side * 0.2 + SK_PUZ * 0.12 - SK_PFZ * 0.12;
		float tx = mx + SK_PRX * side * 0.04 + SK_PUX * 0.26 - SK_PFX * 0.2;
		float ty = my + SK_PRY * side * 0.04 + SK_PUY * 0.26 - SK_PFY * 0.2;
		float tz = mz + SK_PRZ * side * 0.04 + SK_PUZ * 0.26 - SK_PFZ * 0.2;
		capsule(i, bx0, by0, bz0, mx, my, mz, 0.05);
		capsule(i + 1, mx, my, mz, tx, ty, tz, 0.028);
		return i + 2;
	}

	void render(var& grid) override {
		set_world(tt);
		SK_W = w;
		SK_H = h;
		SK_FLICK = 0.7 * math::sin(tt * 17.0) * math::sin(tt * 5.3);
		float bob = 0.2 + 0.018 * math::sin(tt * 11.0);
		float cx = bx + ux * bob;
		float cy = by + uy * bob;
		float cz = bz + uz * bob;
		SK_PFX = fx; SK_PFY = fy; SK_PFZ = fz;
		SK_PUX = ux; SK_PUY = uy; SK_PUZ = uz;
		SK_PRX = rx; SK_PRY = ry; SK_PRZ = rz;
		SK_TX = cx; SK_TY = cy; SK_TZ = cz;
		SK_AX = cx - fx * 0.4 + ux * 0.1; SK_AY = cy - fy * 0.4 + uy * 0.1; SK_AZ = cz - fz * 0.4 + uz * 0.1;
		float nod = 0.03 * math::sin(tt * 2.1);
		SK_HX = cx + fx * 0.34 + ux * (0.09 + nod); SK_HY = cy + fy * 0.34 + uy * (0.09 + nod); SK_HZ = cz + fz * 0.34 + uz * (0.09 + nod);
		SK_NX = SK_HX + fx * 0.23 - ux * 0.06; SK_NY = SK_HY + fy * 0.23 - uy * 0.06; SK_NZ = SK_HZ + fz * 0.23 - uz * 0.06;
		float jaw = 0.012 * math::sin(tt * 6.0);
		SK_JX = SK_HX + fx * 0.1 - ux * (0.14 + jaw); SK_JY = SK_HY + fy * 0.1 - uy * (0.14 + jaw); SK_JZ = SK_HZ + fz * 0.1 - uz * (0.14 + jaw);
		SK_BCX = cx; SK_BCY = cy; SK_BCZ = cz;
		SK_BR = 1.6;
		int ncap = 5 + 2 + 16 + 12 + 7;
		if (SK_CAX.size() != ncap) {
			SK_CAX = []; SK_CAY = []; SK_CAZ = []; SK_CDX = []; SK_CDY = []; SK_CDZ = []; SK_CID = []; SK_CR = []; SK_CMX = []; SK_CMY = []; SK_CMZ = []; SK_CB = [];
			for (int i = 0; i < ncap; ++i) {
				SK_CAX.push(0.0); SK_CAY.push(0.0); SK_CAZ.push(0.0); SK_CDX.push(0.0); SK_CDY.push(0.0); SK_CDZ.push(0.0); SK_CID.push(1.0); SK_CR.push(0.1); SK_CMX.push(0.0); SK_CMY.push(0.0); SK_CMZ.push(0.0); SK_CB.push(1.0);
			}
		}
		int nglow = 3 + 8;
		if (SK_GX.size() != nglow) {
			SK_GX = []; SK_GY = []; SK_GZ = []; SK_GR = [];
			for (int i = 0; i < nglow; ++i) { SK_GX.push(0.0); SK_GY.push(0.0); SK_GZ.push(0.0); SK_GR.push(0.02); }
		}
		SK_NC = ncap;
		SK_CROWN0 = ncap - 7;
		SK_NG = nglow;
		SK_NBONE = 5;
		glow(0, SK_HX + fx * 0.175 + ux * 0.03 + rx * 0.088, SK_HY + fy * 0.175 + uy * 0.03 + ry * 0.088, SK_HZ + fz * 0.175 + uz * 0.03 + rz * 0.088, 0.03);
		glow(1, SK_HX + fx * 0.175 + ux * 0.03 - rx * 0.088, SK_HY + fy * 0.175 + uy * 0.03 - ry * 0.088, SK_HZ + fz * 0.175 + uz * 0.03 - rz * 0.088, 0.03);
		float third = 0.024 + 0.006 * math::sin(tt * 3.0);
		glow(2, SK_HX + fx * 0.17 + ux * 0.15, SK_HY + fy * 0.17 + uy * 0.15, SK_HZ + fz * 0.17 + uz * 0.15, third);
		SK_LX = SK_GX[2] * 0.4 + (SK_GX[0] + SK_GX[1]) * 0.3 + fx * 0.1;
		SK_LY = SK_GY[2] * 0.4 + (SK_GY[0] + SK_GY[1]) * 0.3 + fy * 0.1;
		SK_LZ = SK_GZ[2] * 0.4 + (SK_GZ[0] + SK_GZ[1]) * 0.3 + fz * 0.1;
		capsule(0, SK_JX - rx * 0.08, SK_JY - ry * 0.08, SK_JZ - rz * 0.08, SK_JX + rx * 0.08, SK_JY + ry * 0.08, SK_JZ + rz * 0.08, 0.05);
		int ci = horn(1, 1.0);
		ci = horn(ci, -1.0);
		float fang = 0.02 * math::sin(tt * 6.0 + 1.0);
		for (int s = 0; s < 2; ++s) {
			float side = s == 0 ? 1.0 : -1.0;
			float ax = SK_JX + fx * 0.05 + rx * side * 0.035;
			float ay = SK_JY + fy * 0.05 + ry * side * 0.035;
			float az = SK_JZ + fz * 0.05 + rz * side * 0.035;
			capsule(ci, ax, ay, az, ax + fx * 0.07 - ux * 0.12 - rx * side * (0.02 + fang), ay + fy * 0.07 - uy * 0.12 - ry * side * (0.02 + fang), az + fz * 0.07 - uz * 0.12 - rz * side * (0.02 + fang), 0.016);
			ci = ci + 1;
		}
		for (int i = 0; i < 8; ++i) {
			float side = i < 4 ? 1.0 : -1.0;
			int j = i % 4;
			float fo = legfo[j];
			float hx = cx + fx * fo * 0.45 + rx * side * 0.1 + ux * 0.03;
			float hy = cy + fy * fo * 0.45 + ry * side * 0.1 + uy * 0.03;
			float hz = cz + fz * fo * 0.45 + rz * side * 0.1 + uz * 0.03;
			float dx = footx[i] - hx;
			float dy = footy[i] - hy;
			float dz = footz[i] - hz;
			float d = math::sqrt(dx * dx + dy * dy + dz * dz);
			float dn = d < 0.0001 ? 0.0001 : d;
			dx /= dn; dy /= dn; dz /= dn;
			float dmax = (l1 + l2) * 0.995;
			float dc = d > dmax ? dmax : d;
			dc = dc < 0.08 ? 0.08 : dc;
			float a = (l1 * l1 - l2 * l2 + dc * dc) / (2.0 * dc);
			float hh = l1 * l1 - a * a;
			hh = hh < 0.0 ? 0.0 : math::sqrt(hh);
			float bux = ux + rx * side * 0.35;
			float buy = uy + ry * side * 0.35;
			float buz = uz + rz * side * 0.35;
			float bd = bux * dx + buy * dy + buz * dz;
			bux -= dx * bd; buy -= dy * bd; buz -= dz * bd;
			float bl = 1.0 / math::sqrt(bux * bux + buy * buy + buz * buz + 0.0000001);
			bux *= bl; buy *= bl; buz *= bl;
			float kx = hx + dx * a + bux * hh;
			float ky = hy + dy * a + buy * hh;
			float kz = hz + dz * a + buz * hh;
			float tipx = hx + dx * dc;
			float tipy = hy + dy * dc;
			float tipz = hz + dz * dc;
			capsule(ci, hx, hy, hz, kx, ky, kz, 0.034);
			capsule(ci + 1, kx, ky, kz, tipx, tipy, tipz, 0.02);
			ci = ci + 2;
			glow(3 + i, kx, ky, kz, 0.036 + 0.008 * math::sin(tt * 7.0 + i));
		}
		for (int k = 0; k < 3; ++k) {
			float side = k - 1.0;
			float px = SK_AX - fx * 0.3 + rx * side * 0.07;
			float py = SK_AY - fy * 0.3 + ry * side * 0.07;
			float pz = SK_AZ - fz * 0.3 + rz * side * 0.07;
			for (int seg = 0; seg < 4; ++seg) {
				float ph = tt * 3.1 + seg * 0.9 + k * 2.1;
				float sw = math::sin(ph) * (0.1 + seg * 0.05);
				float lift = math::cos(ph * 0.7 + k) * 0.08 + 0.05;
				float len = 0.2 - seg * 0.025;
				float qx = px - fx * len + rx * (sw + side * 0.05) + ux * lift;
				float qy = py - fy * len + ry * (sw + side * 0.05) + uy * lift;
				float qz = pz - fz * len + rz * (sw + side * 0.05) + uz * lift;
				capsule(ci, px, py, pz, qx, qy, qz, 0.03 - seg * 0.006);
				ci = ci + 1;
				px = qx; py = qy; pz = qz;
			}
		}
		float hover = 0.03 * math::sin(tt * 1.7);
		SK_HGX = SK_HX + ux * 0.3; SK_HGY = SK_HY + uy * 0.3; SK_HGZ = SK_HZ + uz * 0.3;
		SK_BGX = (SK_AX + SK_TX) * 0.5; SK_BGY = (SK_AY + SK_TY) * 0.5; SK_BGZ = (SK_AZ + SK_TZ) * 0.5;
		SK_CRX = SK_HX + ux * (0.44 + hover); SK_CRY = SK_HY + uy * (0.44 + hover); SK_CRZ = SK_HZ + uz * (0.44 + hover);
		for (int k = 0; k < 7; ++k) {
			float a = k * 0.8976 + tt * 0.6;
			float ca = math::cos(a) * 0.2;
			float sa = math::sin(a) * 0.2;
			float flame = 0.1 + 0.05 * math::sin(tt * 9.0 + k * 1.9) + 0.03 * math::sin(tt * 23.0 + k);
			float bxk = SK_CRX + fx * ca + rx * sa;
			float byk = SK_CRY + fy * ca + ry * sa;
			float bzk = SK_CRZ + fz * ca + rz * sa;
			capsule(SK_CROWN0 + k, bxk, byk, bzk, bxk + ux * flame + fx * ca * 0.25 + rx * sa * 0.25, byk + uy * flame + fy * ca * 0.25 + ry * sa * 0.25, bzk + uz * flame + fz * ca * 0.25 + rz * sa * 0.25, 0.018);
		}
		// the bounding radii the distance skips on: the skull's jaw about its center, the ring and flames about the crown's
		float jhx = SK_JX - SK_HX; float jhy = SK_JY - SK_HY; float jhz = SK_JZ - SK_HZ;
		float jaw_reach = math::sqrt(jhx * jhx + jhy * jhy + jhz * jhz) + 0.085 + 0.09;
		SK_SKB = (jaw_reach > SK_HR ? jaw_reach : SK_HR) + 0.001;
		float crown_reach = 0.2 + 0.028;
		for (int k = SK_CROWN0; k < SK_NC; ++k) {
			float mx = SK_CMX[k] - SK_CRX; float my = SK_CMY[k] - SK_CRY; float mz = SK_CMZ[k] - SK_CRZ;
			float r = math::sqrt(mx * mx + my * my + mz * mz) + SK_CB[k];
			crown_reach = r > crown_reach ? r : crown_reach;
		}
		SK_CRB = crown_reach + 0.001;
		float spin = tt * 2.2;
		float gl2 = 0.3 + 0.02 * math::sin(tt * 2.5);
		SK_GMX = SK_CRX + ux * gl2; SK_GMY = SK_CRY + uy * gl2; SK_GMZ = SK_CRZ + uz * gl2;
		SK_GAX = rx * math::cos(spin) + fx * math::sin(spin); SK_GAY = ry * math::cos(spin) + fy * math::sin(spin); SK_GAZ = rz * math::cos(spin) + fz * math::sin(spin);
		SK_GBX = fx * math::cos(spin) - rx * math::sin(spin); SK_GBY = fy * math::cos(spin) - ry * math::sin(spin); SK_GBZ = fz * math::cos(spin) - rz * math::sin(spin);
		SK_EX = ex; SK_EY = ey; SK_EZ = ez;
		SK_EYE_INSIDE = sk_world(ex, ey, ez) < 0.0;
		float lx = cx + fx * 0.3 + ux * 0.25 - ex;
		float ly = cy + fy * 0.3 + uy * 0.25 - ey;
		float lz = cz + fz * 0.3 + uz * 0.25 - ez;
		float ll = 1.0 / math::sqrt(lx * lx + ly * ly + lz * lz);
		lx *= ll; ly *= ll; lz *= ll;
		float d = cux * lx + cuy * ly + cuz * lz;
		float upx = cux - lx * d;
		float upy = cuy - ly * d;
		float upz = cuz - lz * d;
		float ul = 1.0 / math::sqrt(upx * upx + upy * upy + upz * upz + 0.0000001);
		upx *= ul; upy *= ul; upz *= ul;
		SK_FX = lx; SK_FY = ly; SK_FZ = lz;
		SK_UX = upx; SK_UY = upy; SK_UZ = upz;
		SK_RX = ly * upz - lz * upy;
		SK_RY = lz * upx - lx * upz;
		SK_RZ = lx * upy - ly * upx;
		if (SK_ROWS.size() != h) {
			SK_ROWS = [];
			for (int r = 0; r < h; ++r) { SK_ROWS.push(r); }
		}
		var rows = parallel_transform(SK_ROWS, sk_row);
		if (depth.size() != w * h) {
			depth = [];
			for (int q = 0; q < w * h; ++q) { depth.push(0); }
		}
		int idx = 0;
		for (int y = 0; y < h; ++y) {
			var& row = rows[y];
			for (int x = 0; x < w; ++x) {
				int v = row[x];
				grid[idx] = v % 1024;
				depth[idx] = v / 1024;
				idx = idx + 1;
			}
		}
		draw_particles(grid);
	}

	void plot(var& grid, int x, int y, int dq, int level) {
		if (x < 0 || y < 0 || x >= w || y >= h) { return; }
		int at = y * w + x;
		if (depth[at] <= dq) { return; }
		int cur = grid[at];
		if (cur >= SK_SPARK && cur - SK_SPARK >= level) { return; }
		grid[at] = SK_SPARK + level;
	}

	void splat(var& grid, float x, float y, float z, int level, int size) {
		float ox = x - ex;
		float oy = y - ey;
		float oz = z - ez;
		float zc = ox * SK_FX + oy * SK_FY + oz * SK_FZ;
		if (zc < 0.08) { return; }
		float half_h = h * 0.5;
		float scale = half_h / (0.6 * zc);
		float sx = (ox * SK_RX + oy * SK_RY + oz * SK_RZ) * scale + w * 0.5;
		float sy = half_h - (ox * SK_UX + oy * SK_UY + oz * SK_UZ) * scale;
		int px = itrunc(math::floor(sx));
		int py = itrunc(math::floor(sy));
		int dq = itrunc(zc * 18.0);
		plot(grid, px, py, dq, level);
		if (size > 0) {
			int halo = level > 2 ? level - 3 : 0;
			plot(grid, px + 1, py, dq, halo);
			plot(grid, px - 1, py, dq, halo);
			plot(grid, px, py + 1, dq, halo);
			plot(grid, px, py - 1, dq, halo);
		}
		if (size > 1) {
			plot(grid, px + 1, py + 1, dq, level > 1 ? level - 1 : 0);
			plot(grid, px + 1, py, dq, level);
			plot(grid, px, py + 1, dq, level);
		}
	}

	void draw_particles(var& grid) {
		for (int i = 0; i < np; ++i) {
			float life = plife[i];
			if (life <= 0.0) { continue; }
			int level = itrunc(life * 7.99);
			splat(grid, ppx[i], ppy[i], ppz[i], level, life > 0.55 ? 1 : 0);
		}
		float hx = bx + ux * 0.25;
		float hy = by + uy * 0.25;
		float hz = bz + uz * 0.25;
		for (int g = 0; g < 7; ++g) {
			float a = tt * (0.9 + g * 0.07) + g * 0.8976;
			float r = 1.05 + 0.2 * math::sin(tt * 1.3 + g);
			float ca = math::cos(a) * r;
			float sa = math::sin(a) * r;
			float lift = 0.35 + 0.25 * math::sin(tt * 2.0 + g * 1.7);
			int pulse = itrunc(5.5 + 2.0 * math::sin(tt * 6.0 + g));
			splat(grid, hx + fx * ca + rx * sa + ux * lift, hy + fy * ca + ry * sa + uy * lift, hz + fz * ca + rz * sa + uz * lift, pulse > 7 ? 7 : pulse, 2);
		}
		for (int s = 0; s < 6; ++s) {
			float ph = tt * 4.0 + s * 1.047;
			float tw = math::sin(tt * 11.0 + s * 2.7);
			if (tw < 0.2) { continue; }
			float rr = 0.1 + 0.05 * math::sin(tt * 3.0 + s);
			float ca = math::cos(ph) * rr;
			float sa = math::sin(ph) * rr;
			float lift = 0.06 * math::sin(tt * 5.0 + s * 1.3);
			splat(grid, SK_GMX + SK_GAX * ca + SK_GBX * sa + ux * lift, SK_GMY + SK_GAY * ca + SK_GBY * sa + uy * lift, SK_GMZ + SK_GAZ * ca + SK_GBZ * sa + uz * lift, tw > 0.7 ? 7 : 5, 1);
		}
	}
}
