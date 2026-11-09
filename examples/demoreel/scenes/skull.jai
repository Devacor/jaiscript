// OSSUARY — a story in eight acts over 25 bars. A horned, three-eyed skull on a
// spider's body sleeps on a landscape that will not hold still: one raymarched
// field morphing between three triply periodic surfaces (gyroid, Schwarz P,
// Schwarz D) under a flowing domain warp, its walls dark and quiet, veined with a
// slow glow. It wakes as its crown of flame catches, and explores the sheet on
// eight three-segment IK legs in a tetrapod gait, until the sheet melts away into
// a chamber around it. It climbs its dragline to the hub, casts the spokes one by
// one, lays the capture spiral from the rim inward and waits, head-down, skull
// reared toward the viewer, while dew glints on the silk. A moth with eyespotted
// wings flutters in and strikes on the downbeat; the strike rings out through
// every thread, the spider runs it down, turns and wraps it in a cocoon that spins
// as it closes, and the camera draws back on the web, the cocoon and the resting
// hunter. Then the hunter turns on the viewer: the camera swings round in front of
// its face, its eyes kindle and its crown roars up while the chamber darkens and the
// camera creeps in, and on bar 23's downbeat it lunges at the lens, jaws wide; it
// holds the frame glaring, snapping on every kick, and roars again on bar 24. The
// walls breathe with the music: the veins light on the kick, a ring of light leaves
// the creature on every kick, the wall's hue follows the chord and its light the
// cue's energy. The silk is drawn over the trace, depth-tested; the
// moth and cocoon are distance fields in the same march as the spider. Rows trace
// in parallel.

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
float SK_QFX = 0.0; float SK_QFY = 0.0; float SK_QFZ = 1.0;
float SK_QUX = 0.0; float SK_QUY = 1.0; float SK_QUZ = 0.0;
float SK_LX = 0.0; float SK_LY = 0.0; float SK_LZ = 0.0;
float SK_FLICK = 0.0;
float SK_KAGE = 9.0;         // seconds since the cue's last kick
float SK_KICK = 0.0;         // and its envelope
float SK_ZOOM = 0.5;         // the lens: half the view's height at unit distance, punching in on the hits
float SK_PUNCH = 0.0;        // how far it has punched in
float SK_ENERGY = 0.5;       // the cue's energy
float SK_HUE0 = 0.36;        // the wall's hue, following the chord
float SK_WAKE = 1.0;         // 0 while the creature sleeps
float SK_GLARE = 0.0;        // the eighth act: the hunter's glare at the viewer kindles its eyes and lights its face
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
float SK_CAV = 0.0;
float SK_HBX = 0.0; float SK_HBY = 0.0; float SK_HBZ = 0.0;
float SK_PLX = 0.0; float SK_PLY = 0.0; float SK_PLZ = 0.0;
float SK_KX = 0.0; float SK_KY = 1.0; float SK_KZ = 0.0;
bool SK_MOTH = false;
float SK_MX = 0.0; float SK_MY = 0.0; float SK_MZ = 0.0; float SK_MB = 0.6;
float SK_MFX = 0.0; float SK_MFY = 1.0; float SK_MFZ = 0.0;
float SK_MUX = 0.0; float SK_MUY = 0.0; float SK_MUZ = 1.0;
float SK_MRX = 1.0; float SK_MRY = 0.0; float SK_MRZ = 0.0;
float SK_WSC = 1.0;
float SK_COC = 0.0;
float SK_SPIN = 0.0;
array<float> SK_WOX = [0.0, 0.0, 0.0, 0.0]; array<float> SK_WOY = [0.0, 0.0, 0.0, 0.0]; array<float> SK_WOZ = [0.0, 0.0, 0.0, 0.0];
array<float> SK_WSX = [0.0, 0.0, 0.0, 0.0]; array<float> SK_WSY = [0.0, 0.0, 0.0, 0.0]; array<float> SK_WSZ = [0.0, 0.0, 0.0, 0.0];
array<float> SK_WNX = [0.0, 0.0, 0.0, 0.0]; array<float> SK_WNY = [0.0, 0.0, 0.0, 0.0]; array<float> SK_WNZ = [0.0, 0.0, 0.0, 0.0];
array<float> SK_WCA = [0.22, 0.22, 0.15, 0.15];
array<float> SK_WCB = [0.0, 0.0, -0.075, -0.075];
array<float> SK_WSA = [0.22, 0.22, 0.15, 0.15];
array<float> SK_WSB = [0.105, 0.105, 0.09, 0.09];
array<float> SK_WSW = [-0.25, -0.25, -0.35, -0.35];
int SK_WING = 0;
int SK_FUR = 0;
int SK_COCOON = 0;
int SK_SILK = 0;

// the world's distance: the sheet, with the web's chamber (radius SK_CAV about the hub) cut out of it
function sk_wall(float x, float y, float z) -> float {
	float d = sk_shell(sk_field(x, y, z));
	if (SK_CAV > 0.0) {
		float hx = x - SK_HBX; float hy = y - SK_HBY; float hz = z - SK_HBZ;
		float c = SK_CAV - math::sqrt(hx * hx + hy * hy + hz * hz);
		d = d > c ? d : c;
	}
	return d;
}

// a moth's wing k (0, 1 fore, 2, 3 hind): an ellipse swept back along the body, in a plane through its root
function sk_wing(float x, float y, float z, int k) -> float {
	float px = x - SK_WOX[k]; float py = y - SK_WOY[k]; float pz = z - SK_WOZ[k];
	float a = px * SK_WSX[k] + py * SK_WSY[k] + pz * SK_WSZ[k];
	float b = px * SK_MFX + py * SK_MFY + pz * SK_MFZ;
	float c = px * SK_WNX[k] + py * SK_WNY[k] + pz * SK_WNZ[k];
	float u = (a - SK_WCA[k]) / SK_WSA[k];
	float v = (b - SK_WCB[k] - a * SK_WSW[k]) / SK_WSB[k];
	float d = (math::sqrt(u * u + v * v) - 1.0) * SK_WSB[k];
	float dc = math::abs(c) - 0.007;
	return d > dc ? d : dc;
}

// the wing's pattern: 0 gold, 1 umber vein or band, 2 black ring, 3 iris, 4 pupil, 5 violet margin
function sk_wing_tone(float x, float y, float z, int k) -> int {
	float px = x - SK_WOX[k]; float py = y - SK_WOY[k]; float pz = z - SK_WOZ[k];
	float a = px * SK_WSX[k] + py * SK_WSY[k] + pz * SK_WSZ[k];
	float b = px * SK_MFX + py * SK_MFY + pz * SK_MFZ;
	float u = (a - SK_WCA[k]) / SK_WSA[k];
	float v = (b - SK_WCB[k] - a * SK_WSW[k]) / SK_WSB[k];
	float r = math::sqrt(u * u + v * v);
	if (r > 0.82) { return 5; }
	bool fore = k < 2;
	float eu = u - (fore ? 0.3 : 0.05);
	float ev = v - (fore ? -0.08 : 0.05);
	float e = math::sqrt(eu * eu + ev * ev) / (fore ? 0.3 : 0.45);
	if (e < 0.34) { return 4; }
	if (e < 0.74) { return 3; }
	if (e < 1.0) { return 2; }
	float band = u * 0.8 + v * 0.5 + (fore ? 0.45 : -0.2);
	if (band > -0.1 && band < 0.1) { return 1; }
	float vein = math::sin(math::atan2(v, u + 1.05) * 11.0);
	return vein > 0.9 ? 1 : 0;
}

// the moth: a furred body and head, four wings, and the cocoon the spider wraps around it
function sk_moth(float x, float y, float z) -> float {
	float ox = x - SK_MX; float oy = y - SK_MY; float oz = z - SK_MZ;
	float f = ox * SK_MFX + oy * SK_MFY + oz * SK_MFZ;
	float fc = f < -0.25 ? -0.25 : (f > 0.09 ? 0.09 : f);
	float qx = ox - SK_MFX * fc; float qy = oy - SK_MFY * fc; float qz = oz - SK_MFZ * fc;
	float d = math::sqrt(qx * qx + qy * qy + qz * qz) - (0.066 + (fc < 0.0 ? fc * 0.12 : 0.0));
	float hx = ox - SK_MFX * 0.15; float hy = oy - SK_MFY * 0.15; float hz = oz - SK_MFZ * 0.15;
	float dh = math::sqrt(hx * hx + hy * hy + hz * hz) - 0.058;
	d = d < dh ? d : dh;
	if (SK_WSC > 0.05) {
		for (int k = 0; k < 4; ++k) {
			float dw = sk_wing(x, y, z, k);
			d = d < dw ? d : dw;
		}
	}
	if (SK_COC > 0.0) {
		float cf = f + 0.07;
		float rr = ox * ox + oy * oy + oz * oz - f * f;
		rr = rr < 0.0 ? 0.0 : math::sqrt(rr);
		float ca = 0.18 + 0.22 * SK_COC;
		float cb = 0.05 + 0.14 * SK_COC;
		float dc = (math::sqrt(cf * cf / (ca * ca) + rr * rr / (cb * cb)) - 1.0) * cb;
		d = d < dc ? d : dc;
	}
	return d;
}

// which part of the moth is nearest: -1 body, -2 cocoon, else the wing's index
function sk_moth_part(float x, float y, float z) -> int {
	float ox = x - SK_MX; float oy = y - SK_MY; float oz = z - SK_MZ;
	float f = ox * SK_MFX + oy * SK_MFY + oz * SK_MFZ;
	float fc = f < -0.25 ? -0.25 : (f > 0.09 ? 0.09 : f);
	float qx = ox - SK_MFX * fc; float qy = oy - SK_MFY * fc; float qz = oz - SK_MFZ * fc;
	float best = math::sqrt(qx * qx + qy * qy + qz * qz) - (0.066 + (fc < 0.0 ? fc * 0.12 : 0.0));
	float hx = ox - SK_MFX * 0.15; float hy = oy - SK_MFY * 0.15; float hz = oz - SK_MFZ * 0.15;
	float dh = math::sqrt(hx * hx + hy * hy + hz * hz) - 0.058;
	best = best < dh ? best : dh;
	int result = -1;
	if (SK_WSC > 0.05) {
		for (int k = 0; k < 4; ++k) {
			float dw = sk_wing(x, y, z, k);
			if (dw < best) { best = dw; result = k; }
		}
	}
	if (SK_COC > 0.0) {
		float cf = f + 0.07;
		float rr = ox * ox + oy * oy + oz * oz - f * f;
		rr = rr < 0.0 ? 0.0 : math::sqrt(rr);
		float ca = 0.18 + 0.22 * SK_COC;
		float cb = 0.05 + 0.14 * SK_COC;
		float dc = (math::sqrt(cf * cf / (ca * ca) + rr * rr / (cb * cb)) - 1.0) * cb;
		if (dc < best) { result = -2; }
	}
	return result;
}

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
	float swx = math::sin(qy * 1.3 + SK_T * 0.7);
	float swy = math::sin(qz * 1.3 + SK_T * 0.6);
	float swz = math::sin(qx * 1.3 + SK_T * 0.8);
	float wx = qx + SK_WA * swx;
	float wy = qy + SK_WA * swy;
	float wz = qz + SK_WA * swz;
	float sx = math::sin(wx); float cx = math::cos(wx);
	float sy = math::sin(wy); float cy = math::cos(wy);
	float sz = math::sin(wz); float cz = math::cos(wz);
	float g = sx * cy + sy * cz + sz * cx;
	float p = cx + cy + cz;
	float d = sx * sy * sz + sx * cy * cz + cx * sy * cz + cx * cy * sz;
	return g * SK_MG + p * SK_MP + d * SK_MD * 1.4 - SK_K;
}

// the world's distance from a field value: the shell of half-thickness SK_TH about the level set, scaled to a safe step
function sk_shell(float g) -> float {
	float a = g < 0.0 ? -g : g;
	return (a - SK_TH) / SK_SHELL;
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
	float af = ax * SK_QFX + ay * SK_QFY + az * SK_QFZ;
	float au = ax * SK_QUX + ay * SK_QUY + az * SK_QUZ;
	float ar = ax * SK_PRX + ay * SK_PRY + az * SK_PRZ;
	af = af * 0.74;
	au = au * 1.04;
	float da = (math::sqrt(af * af + au * au + ar * ar) - SK_AR) * 0.74;
	float tx = x - SK_TX; float ty = y - SK_TY; float tz = z - SK_TZ;
	float dt = math::sqrt(tx * tx + ty * ty + tz * tz) - SK_TR;
	float k = 0.09;
	float hh = 0.5 + 0.5 * (dt - da) / k;
	hh = hh < 0.0 ? 0.0 : (hh > 1.0 ? 1.0 : hh);
	return dt + (da - dt) * hh - k * hh * (1.0 - hh);
}

// the crown's ring; its flames are the capsules [SK_CROWN0, SK_NC)
function sk_ring(float x, float y, float z) -> float {
	float ox = x - SK_CRX; float oy = y - SK_CRY; float oz = z - SK_CRZ;
	float cu = ox * SK_PUX + oy * SK_PUY + oz * SK_PUZ;
	float px = ox - SK_PUX * cu; float py = oy - SK_PUY * cu; float pz = oz - SK_PUZ * cu;
	float ring = math::sqrt(px * px + py * py + pz * pz) - 0.2;
	return math::sqrt(ring * ring + cu * cu) - 0.028;
}

function sk_gem(float x, float y, float z) -> float {
	float ox = x - SK_GMX; float oy = y - SK_GMY; float oz = z - SK_GMZ;
	float a = math::abs(ox * SK_GAX + oy * SK_GAY + oz * SK_GAZ);
	float b = math::abs(ox * SK_PUX + oy * SK_PUY + oz * SK_PUZ);
	float c = math::abs(ox * SK_GBX + oy * SK_GBY + oz * SK_GBZ);
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
		if (math::sqrt(ox * ox + oy * oy + oz * oz) - SK_CRB < d) {
			float dr = sk_ring(x, y, z);
			d = d < dr ? d : dr;
			d = sk_capsules(x, y, z, SK_CROWN0, SK_NC, d);
		}
		float dg = sk_gem(x, y, z);
		d = d < dg ? d : dg;
	}
	if ((groups & 4) != 0) {
		float dm = sk_moth(x, y, z);
		d = d < dm ? d : dm;
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
	if (math::sqrt(ox * ox + oy * oy + oz * oz) - SK_CRB < de) {
		float dr = sk_ring(x, y, z);
		de = de < dr ? de : dr;
		de = sk_capsules(x, y, z, SK_CROWN0, SK_NC, de);
	}
	if (de < best) { best = de; m = 3; }
	float dgm = sk_gem(x, y, z);
	if (dgm < best) { best = dgm; m = 5; }
	if (SK_MOTH) {
		float mx = x - SK_MX; float my = y - SK_MY; float mz = z - SK_MZ;
		if (math::sqrt(mx * mx + my * my + mz * mz) - SK_MB < best) {
			float dm = sk_moth(x, y, z);
			if (dm < best) { m = 6; }
		}
	}
	if (m == 2 && chit == db) {
		// chevrons crawl down the abdomen's back, a stripe runs along its spine
		float ax = x - SK_AX; float ay = y - SK_AY; float az = z - SK_AZ;
		float au = ax * SK_QUX + ay * SK_QUY + az * SK_QUZ;
		float ar = ax * SK_PRX + ay * SK_PRY + az * SK_PRZ;
		float af = ax * SK_QFX + ay * SK_QFY + az * SK_QFZ;
		float ar2 = ar < 0.0 ? -ar : ar;
		if (au > 0.07 && af > -0.33 && af < 0.2) {
			float v = af * 7.0 - ar2 * 9.0 + SK_T * 1.6;
			if (v - math::floor(v) < 0.3 || ar2 < 0.018) { m = 4; }
		}
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
	if (sk_ray_near(ox, oy, oz, dx, dy, dz, SK_BGX, SK_BGY, SK_BGZ, 0.7)) { groups = 1; }
	if (sk_ray_near(ox, oy, oz, dx, dy, dz, SK_HGX, SK_HGY, SK_HGZ, 0.74)) { groups = groups + 2; }
	if (SK_MOTH && sk_ray_near(ox, oy, oz, dx, dy, dz, SK_MX, SK_MY, SK_MZ, SK_MB)) { groups = groups + 4; }
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
	float lip = (groups & 6) != 0 ? 1.35 : 1.0;
	float ds_last = 0.0;
	float t_last = 0.0;
	float wall = 0.0;
	bool cut = false;
	bool carved = SK_CAV > 0.0;
	int i = 0;
	while (i < 64) {
		float x = ox + dx * t;
		float y = oy + dy * t;
		float z = oz + dz * t;
		float g = sk_field(x, y, z);
		float dw = sk_shell(g);
		bool in_cut = false;
		if (carved) {
			float hx = x - SK_HBX; float hy = y - SK_HBY; float hz = z - SK_HBZ;
			float c = SK_CAV - math::sqrt(hx * hx + hy * hy + hz * hz);
			if (c > dw) { dw = c; in_cut = true; }
		}
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
			cut = in_cut;
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
		float g = 0.0;
		float nx = 0.0; float ny = 0.0; float nz = 0.0;
		if (cut) {
			// the chamber's wall: the sheet cut through, facing the hub
			g = sk_field(x, y, z);
			nx = SK_HBX - x; ny = SK_HBY - y; nz = SK_HBZ - z;
			float cl = 1.0 / math::sqrt(nx * nx + ny * ny + nz * nz + 0.0000001);
			nx *= cl; ny *= cl; nz *= cl;
		} else {
			float hs = 0.004;
			float n0 = sk_field(x + hs, y - hs, z - hs);
			float n1 = sk_field(x - hs, y - hs, z + hs);
			float n2 = sk_field(x - hs, y + hs, z - hs);
			float n3 = sk_field(x + hs, y + hs, z + hs);
			g = (n0 + n1 + n2 + n3) * 0.25;
			nx = n0 - n1 - n2 + n3;
			ny = -n0 - n1 + n2 + n3;
			nz = -n0 + n1 - n2 + n3;
			float sg = g < 0.0 ? -1.0 : 1.0;
			float nl = sg / math::sqrt(nx * nx + ny * ny + nz * nz + 0.0000001);
			nx *= nl; ny *= nl; nz *= nl;
		}
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
		float pulse = (0.35 + 0.65 * SK_KICK) * (0.5 + 0.5 * math::sin(vein * 5.0 - SK_T * 1.6 + vz * 0.25));
		float ao = 1.0 - i * 0.011;
		ao = ao < 0.3 ? 0.3 : ao;
		float lit = (0.04 + head * 0.5) * fog * ao;
		// the walls glow in slow drifting bands of their own, the chamber's cut faces brightest
		float far = 1.0 - t / 24.0;
		float swirl = math::sin(vx * 0.35 + vy * 0.45 - vz * 0.3 + g * 3.0 + SK_T * 1.4) + math::sin(vy * 0.3 - vx * 0.25 + SK_T * 0.9);
		lit = lit + (0.05 + 0.02 * swirl + (cut ? 0.07 : 0.0)) * far * (0.6 + 0.6 * SK_ENERGY);
		if (va < 0.3) {
			float edge = 1.0 - va / 0.3;
			float glowv = pulse * pulse * edge * 0.8 * (0.35 + fog * 0.65) * (0.4 + 0.6 * SK_WAKE);
			lit = glowv > lit ? glowv : lit * 0.4;
		}
		// oil-slick hue: it turns with the viewing angle, crawls with time and jumps on the veins
		float hue = SK_HUE0 + ((vx * 0.11 + vy * 0.07 - vz * 0.05) * 0.3 + g * 0.35 + t * 0.035 + head * 0.55) * 0.12 + (va < 0.3 ? 0.06 : 0.0);
		// contour lines of the field crawl over the walls; on the chamber's cut faces they are the sheet's strata
		float cv = (cut ? g * 16.0 : g * 2.4) - SK_T * 0.7;
		float cf = cv - math::floor(cv);
		if (cf < 0.06 && cut) {
			float c2 = (0.06 - cf) * 6.0 * far;
			lit = lit > c2 ? lit + c2 * 0.3 : c2;
		}
		// rings of light pulse out through the walls from the creature
		float plx = x - SK_PLX; float ply = y - SK_PLY; float plz = z - SK_PLZ;
		float pd = math::sqrt(plx * plx + ply * ply + plz * plz);
		float rf = pd - SK_KAGE * 6.0;
		rf = rf < 0.0 ? -rf : rf;
		if (rf < 0.22 && pd < 9.0) {
			lit = lit + (1.0 - rf / 0.22) * (1.0 - pd / 9.0) * 0.7 * (0.5 + SK_ENERGY);
		}
		if (ember * fog > lit * 1.15 + (va < 0.3 ? 0.25 : 0.0)) {
			int e = itrunc((ember * fog * 0.85 + lit * 0.3) * 11.0 + dith + 0.5);
			return SK_EMBER + (e < 0 ? 0 : (e > 11 ? 11 : e)) + depth;
		}
		hue = hue - math::floor(hue);
		int hq = itrunc(hue * 12.0);
		hq = hq > 11 ? 11 : hq;
		int l = itrunc(lit * 11.0 + dith + 0.5);
		l = l < 0 ? 0 : (l > 11 ? 11 : l);
		return SK_WORLD + hq * 12 + l + depth;
	}
	if (m == 3) {
		int e = itrunc(2.4 + SK_FLICK + dith);
		for (int k = 0; k < SK_NSOCK; ++k) {
			float ex = x - SK_GX[k]; float ey = y - SK_GY[k]; float ez = z - SK_GZ[k];
			float el = math::sqrt(ex * ex + ey * ey + ez * ez);
			if (el < SK_GR[k] + 0.012) {
				// a wet glint where the eye's sphere mirrors the key light toward the viewer
				float hx = SK_KX - dx; float hy = SK_KY - dy; float hz = SK_KZ - dz;
				float hl = math::sqrt(hx * hx + hy * hy + hz * hz) * el + 0.0000001;
				if ((ex * hx + ey * hy + ez * hz) / hl > 0.8) { e = 4; }
			}
		}
		return SK_EYE + (e < 0 ? 0 : (e > 4 ? 4 : e)) + depth;
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
	float key = nx * SK_KX + ny * SK_KY + nz * SK_KZ;
	if (m == 6) {
		int part = sk_moth_part(x, y, z);
		if (part >= 0) {
			// the wings glow through: lit from either side
			float tl = 0.5 + 0.55 * (key < 0.0 ? -key : key) + eyel * 0.6;
			int tone = sk_wing_tone(x, y, z, part);
			if (tone == 3 && part >= 2) { tone = 6; }
			int l = itrunc(tl * fog * 7.0 + dith + 0.5);
			return SK_WING + tone * 8 + (l < 0 ? 0 : (l > 7 ? 7 : l)) + depth;
		}
		if (part == -2) {
			// the cocoon: silk wound in bands that turn as the spider spins it
			float ox2 = x - SK_MX; float oy2 = y - SK_MY; float oz2 = z - SK_MZ;
			float f = ox2 * SK_MFX + oy2 * SK_MFY + oz2 * SK_MFZ;
			float ang = math::atan2(ox2 * SK_MUX + oy2 * SK_MUY + oz2 * SK_MUZ, ox2 * SK_MRX + oy2 * SK_MRY + oz2 * SK_MRZ);
			float band = math::sin(f * 70.0 + ang * 2.0 + SK_SPIN * 2.0);
			float cl = (0.2 + vd * 0.45 + rim * 0.35 + (key > 0.0 ? key * 0.25 : 0.0)) * (0.72 + 0.28 * band);
			int l = itrunc(cl * fog * 9.0 + dith + 0.5);
			return SK_COCOON + (l < 0 ? 0 : (l > 9 ? 9 : l)) + depth;
		}
		float fl = 0.12 + vd * 0.55 + rim * 0.25 + eyel * 0.5;
		int l = itrunc(fl * fog * 7.0 + dith * 1.6 + 0.5);
		return SK_FUR + (l < 0 ? 0 : (l > 7 ? 7 : l)) + depth;
	}
	if (m == 1) {
		float lit = 0.1 + vd * (0.55 + SK_GLARE * 0.4) + up * 0.22 + rim * 0.25;
		float so = 99.0;
		for (int k = 0; k < SK_NSOCK; ++k) {
			float ox2 = x - SK_GX[k]; float oy2 = y - SK_GY[k]; float oz2 = z - SK_GZ[k];
			float q = math::sqrt(ox2 * ox2 + oy2 * oy2 + oz2 * oz2) - (SK_GR[k] < 0.03 ? SK_GR[k] : 0.03) * 2.5;
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
	// a glossy highlight of the key light
	float hx = SK_KX - dx; float hy = SK_KY - dy; float hz = SK_KZ - dz;
	float hl = 1.0 / math::sqrt(hx * hx + hy * hy + hz * hz + 0.0000001);
	float spec = (nx * hx + ny * hy + nz * hz) * hl;
	spec = spec < 0.0 ? 0.0 : spec;
	spec = spec * spec; spec = spec * spec; spec = spec * spec; spec = spec * spec;
	if (m == 4) {
		int k = itrunc((0.45 + vd * 0.45 + 0.3 * math::sin(SK_T * 4.0) + spec * 0.6) * 5.0 + dith + 0.5);
		return SK_MARK + (k < 0 ? 0 : (k > 5 ? 5 : k)) + depth;
	}
	// beetle iridescence: the shell turns from wine through violet to teal toward its rim
	float lit = 0.1 + vd * 0.42 + rim * 0.45 + eyel * 0.9 + (key > 0.0 ? key * 0.3 : 0.0) + spec * 1.1;
	int band = vd > 0.68 ? 0 : (vd > 0.36 ? 1 : 2);
	int c = itrunc(lit * fog * 11.0 + dith + 0.5);
	return SK_CHIT + band * 12 + (c < 0 ? 0 : (c > 11 ? 11 : c)) + depth;
}

function sk_row(int y) -> array<int> {
	array<int> out = [];
	array<int> cand = [];
	for (int k = 0; k < 64; ++k) { cand.push(0); }
	int ww = SK_W;
	float half_h = SK_H * 0.5;
	float zoom = SK_ZOOM / half_h;
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
		int v = sk_trace(dx * il, dy * il, dz * il, x, y, cand, row, nrow);
		out.push(v);
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
	// the web, the shafts and the dust draw per pixel too: past 16,000 pixels the whole scene renders smaller
	Ossuary() { pixel_budget = 16000; }

	float bx = 0.4; float by = 0.3; float bz = 0.2;
	float fx = 0.0; float fy = 0.0; float fz = 1.0;
	float ux = 0.0; float uy = 1.0; float uz = 0.0;
	float rx = 1.0; float ry = 0.0; float rz = 0.0;
	float cxs = 0.0; float cys = 0.0; float czs = 0.0;
	float ex = 0.0; float ey = 0.0; float ez = 0.0;
	float cux = 0.0; float cuy = 1.0; float cuz = 0.0;
	float lkx = 0.0; float lky = 0.0; float lkz = 0.0;
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
	float l2 = 0.52;
	float tt = 0.0;
	float wander = 0.0;
	float spawn_acc = 0.0;

	// the story, act by act on the cue's bars: it sleeps (bars 0-1), wakes and explores the sheet (2-4), the sheet
	// melts into a chamber and it climbs to the hub (5-6), casts the spokes and lays the spiral (6-12), waits (13-14),
	// the moth strikes on bar 15 and the spider runs it down and wraps it (15-18), the camera draws back (19-20),
	// then the hunter turns on the viewer (21), stalks it (22), lunges on bar 23 and roars on bar 24
	float t_wake = 3.871;
	float t_melt = 9.677;
	float t_lift = 10.9;
	float t_hub = 12.0;
	float t_spokes = 12.2;
	float t_spoke_gap = 0.16;
	float t_out = 14.9;
	float t_spiral0 = 15.484;
	float t_spiral1 = 24.2;
	float t_home = 25.2;
	float t_moth = 27.1;
	float t_impact = 29.032;
	float t_rush = 29.6;
	float t_reach = 30.6;
	float t_turn = 31.2;
	float t_wrap0 = 31.4;
	float t_wrap1 = 36.774;
	float t_face = 40.645;
	float t_stalk = 42.58;
	float t_lunge = 44.516;
	float t_roar = 46.452;
	float hub_up = 2.0;
	float cav_full = 4.6;
	bool planned = false;
	bool on_web = false;
	float hbx = 0.0; float hby = 0.0; float hbz = 0.0;
	float e1x = 1.0; float e1y = 0.0; float e1z = 0.0;
	float e2x = 0.0; float e2y = 1.0; float e2z = 0.0;
	float nnx = 0.0; float nny = 0.0; float nnz = 1.0;
	float g0x = 0.0; float g0y = 0.0; float g0z = 0.0;
	float f0x = 0.0; float f0y = 0.0; float f0z = 1.0;
	int nspoke = 14;
	array<float> spoke_ang = [];
	array<float> spoke_len = [];
	array<float> spa = [];
	array<float> spb = [];
	array<float> spl = [];
	float pa = 0.0; float pb = 0.0; float psi = 1.5707963;
	float spd = 0.0;
	float moth_a = 1.45; float moth_b = 1.0;
	float qa = 0.0; float qb = 0.0;
	float wx = 0.0; float wy = 0.0; float wz = 0.0;
	float qsx = 0.0; float qsy = 0.0; float qzc = 0.0;
	float mox = 0.0; float moy = 0.0; float moz = 0.0;
	float mfx = 0.0; float mfy = 1.0; float mfz = 0.0;
	float mux = 0.0; float muy = 0.0; float muz = 1.0;
	float mrx = 1.0; float mry = 0.0; float mrz = 0.0;
	float flap = 0.0; float wsc = 1.0; float coc = 0.0; float spin = 0.0;

	void build() override {
		name = "OSSUARY";
		handle = "eight legs of inverse kinematics . an orb web . a moth that came too close";
		replay = 49.0;
		SK_WORLD = pal.size();
		for (int hq = 0; hq < 12; ++hq) {
			var c = gfx.rainbow_rgb(hq * 0.5236 + 2.2);
			for (int s = 0; s < 12; ++s) {
				float f = s / 11.0;
				float k = f * f * 0.9 + f * 0.25;
				float wh = f > 0.82 ? (f - 0.82) * 2.2 : 0.0;
				float r = 3.0 + c[0] * k * 0.62;
				float g = 2.0 + c[1] * k * 0.62;
				float b = 7.0 + c[2] * k * 0.62;
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
		array<float> shell = [235.0, 40.0, 90.0, 150.0, 70.0, 255.0, 40.0, 230.0, 210.0];
		for (int band = 0; band < 3; ++band) {
			for (int k = 0; k < 12; ++k) {
				float f = k / 11.0;
				float c = f * f * 0.85 + f * 0.15;
				float wh = f > 0.8 ? (f - 0.8) * 3.5 : 0.0;
				float r = 5.0 + shell[band * 3] * c;
				float g = 2.0 + shell[band * 3 + 1] * c;
				float b = 7.0 + shell[band * 3 + 2] * c;
				pal.add(r + (255.0 - r) * wh, g + (255.0 - g) * wh, b + (255.0 - b) * wh);
			}
		}
		SK_EYE = pal.ramp(4, 220.0, 30.0, 10.0, 255.0, 250.0, 190.0);
		pal.add(255.0, 255.0, 255.0);
		SK_MARK = pal.ramp(6, 90.0, 0.0, 8.0, 255.0, 200.0, 60.0);
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
		// the moth's wing tones: gold, umber, the eyespot's black ring, cyan iris, pupil, violet margin, magenta iris
		SK_WING = pal.size();
		array<float> tones = [255.0, 186.0, 64.0, 120.0, 58.0, 28.0, 34.0, 10.0, 34.0, 60.0, 235.0, 255.0, 255.0, 245.0, 255.0, 160.0, 70.0, 230.0, 255.0, 50.0, 200.0];
		for (int tone = 0; tone < 7; ++tone) {
			for (int l = 0; l < 8; ++l) {
				float f = (l + 1) / 8.0;
				float k = 0.1 + 0.9 * f;
				pal.add(tones[tone * 3] * k, tones[tone * 3 + 1] * k, tones[tone * 3 + 2] * k);
			}
		}
		SK_FUR = pal.ramp(8, 22.0, 12.0, 8.0, 236.0, 196.0, 150.0);
		SK_COCOON = pal.gradient(10, [[16.0, 12.0, 28.0], [110.0, 100.0, 150.0], [230.0, 226.0, 248.0], [255.0, 255.0, 255.0]]);
		// silk: eight iridescent hues, six levels each
		SK_SILK = pal.size();
		for (int hq = 0; hq < 8; ++hq) {
			var c = gfx.rainbow_rgb(hq * 0.7854 + 0.3);
			float cr = c[0] * 0.12 + 196.0;
			float cg = c[1] * 0.12 + 204.0;
			float cb = c[2] * 0.12 + 222.0;
			for (int l = 0; l < 6; ++l) {
				float f = 0.14 + 0.86 * (l + 1) / 6.0;
				pal.add(cr * f, cg * f, cb * f);
			}
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

	int length_bars() override { return 25; }

	array<float> hit_times() override { return [3.871, 9.677, 15.484, 29.032, 36.774, 44.516, 46.452]; }
	array<int> hit_levels() override { return [1, 1, 1, 2, 1, 2, 1]; }

	// the eighth act: 0 until the hunter turns to the viewer, then 1
	float facing(float t) { return smooth((t - t_face) / 1.2); }
	// 0 before the stalk, rising through bar 22 to 1 at the lunge
	float stalk(float t) { return smooth((t - t_stalk) / (t_lunge - t_stalk)); }
	// the lunge itself, 1 from just after bar 23's downbeat on
	float lunge(float t) { return smooth((t - t_lunge + 0.06) / 0.22); }
	// how wide the jaw gapes: the lunge, the roar, and a snap on every kick of the close-up
	float gape(float t) {
		float result = 0.0;
		float s = t - t_lunge;
		if (s > -0.1) { result = math::exp(0.0 - (s > 0.0 ? s : 0.0) * 2.2) + 0.35 * SK_KICK; }
		float r = t - t_roar;
		if (r > -0.08) { result += 1.1 * math::exp(0.0 - (r > 0.0 ? r : 0.0) * 1.6) * smooth((r + 0.08) / 0.1); }
		return result > 1.2 ? 1.2 : result;
	}

	// the music the walls breathe with: the last kick's age and envelope, the energy, the chord's hue
	void hear(float t) {
		if (cue == null) { return; }
		int k = cue.kick.last(t / MUSIC_STEP + 0.0001);
		SK_KAGE = k < 0 ? 9.0 : t - cue.kick.at[k] * MUSIC_STEP;
		SK_KICK = cue.kick_env(t);
		SK_ENERGY = cue.energy(t);
		float want = 0.34 + cue.chord(t) / 12.0 * 0.12 + cue.hit(t) * 0.06;
		SK_HUE0 += (want - SK_HUE0) * 0.08;
		// the lens is long, so the creature fills the frame; it punches in on each hit and breathes on the kick
		float punch = cue.hit(t);
		SK_PUNCH = punch * punch;
		SK_ZOOM = 0.5 * (1.0 - SK_PUNCH * 0.26 - SK_KICK * 0.015 * SK_WAKE);
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
		SK_CAV = cavity(t);
		SK_HBX = hbx; SK_HBY = hby; SK_HBZ = hbz;
	}

	float smooth(float u) {
		float c = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
		return c * c * (3.0 - 2.0 * c);
	}

	float turn_toward(float from, float to, float w) {
		float d = to - from;
		d = d - 6.2831853 * math::floor((d + 3.1415927) / 6.2831853);
		return from + d * w;
	}

	// the chamber the sheet melts away into, about the hub
	float cavity(float t) {
		if (t < t_melt) { return 0.0; }
		if (t < t_lift) { return (t - t_melt) / (t_lift - t_melt) * 1.6; }
		return 1.6 + (cav_full - 1.6) * smooth((t - t_lift) / 1.8);
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

	// a foot's ground: the sheet while it crawls, the web's plane once it hangs there
	void project_foot(float& x, float& y, float& z) {
		if (on_web) {
			float d = (x - hbx) * nnx + (y - hby) * nny + (z - hbz) * nnz;
			x -= nnx * d; y -= nny * d; z -= nnz * d;
		} else {
			sk_project(x, y, z);
		}
	}

	void rest_foot(int i, float lead) {
		float side = i < 4 ? 1.0 : -1.0;
		int j = i % 4;
		float reach = legreach[j] + lead;
		float x = bx + fx * reach + rx * side * 0.78;
		float y = by + fy * reach + ry * side * 0.78;
		float z = bz + fz * reach + rz * side * 0.78;
		project_foot(x, y, z);
		tox[i] = x; toy[i] = y; toz[i] = z;
	}

	float jitter(int n) { return (rng.next(n * 2 + 1) - n) / (n * 1.0); }

	// as the sheet starts to melt the web is planned: its plane stands on the sheet's normal facing the camera, the
	// spokes get their angles and the capture spiral its polygon, one vertex where it crosses each spoke
	void plan_web() {
		planned = true;
		hbx = bx + ux * hub_up; hby = by + uy * hub_up; hbz = bz + uz * hub_up;
		e2x = ux; e2y = uy; e2z = uz;
		float vx = ex - hbx; float vy = ey - hby; float vz = ez - hbz;
		float d = vx * e2x + vy * e2y + vz * e2z;
		vx -= e2x * d; vy -= e2y * d; vz -= e2z * d;
		float vl = math::sqrt(vx * vx + vy * vy + vz * vz);
		if (vl < 0.1) { vx = fx; vy = fy; vz = fz; vl = 1.0; }
		nnx = vx / vl; nny = vy / vl; nnz = vz / vl;
		e1x = e2y * nnz - e2z * nny;
		e1y = e2z * nnx - e2x * nnz;
		e1z = e2x * nny - e2y * nnx;
		spoke_ang = [];
		spoke_len = [];
		for (int k = 0; k < nspoke; ++k) {
			float jit = k == 0 ? 0.0 : jitter(100) * 0.09;
			spoke_ang.push(-1.5707963 + k * 6.2831853 / nspoke + jit);
			spoke_len.push(cav_full);
		}
		spa = [];
		spb = [];
		spl = [];
		int n = 4 * nspoke + nspoke / 2 + 1;
		float len = 0.0;
		for (int i = 0; i < n; ++i) {
			float r = 2.75 - 1.95 * i / (n - 1.0);
			float a = spoke_ang[i % nspoke] + 6.2831853 * (i / nspoke);
			float pa2 = r * math::cos(a);
			float pb2 = r * math::sin(a);
			if (i > 0) {
				float da = pa2 - spa[i - 1];
				float db = pb2 - spb[i - 1];
				len += math::sqrt(da * da + db * db);
			}
			spa.push(pa2);
			spb.push(pb2);
			spl.push(len);
		}
	}

	// it leaves the sheet and climbs its dragline to the hub
	void leave_sheet() {
		on_web = true;
		g0x = bx; g0y = by; g0z = bz;
		f0x = fx; f0y = fy; f0z = fz;
		pa = 0.0;
		pb = -hub_up;
	}

	// the point s along the capture spiral, into qa, qb
	void spiral_at(float s) {
		int n = spl.size();
		if (s <= 0.0) { qa = spa[0]; qb = spb[0]; return; }
		for (int i = 1; i < n; ++i) {
			if (spl[i] >= s) {
				float u = (s - spl[i - 1]) / (spl[i] - spl[i - 1] + 0.000001);
				qa = spa[i - 1] + (spa[i] - spa[i - 1]) * u;
				qb = spb[i - 1] + (spb[i] - spb[i - 1]) * u;
				return;
			}
		}
		qa = spa[n - 1]; qb = spb[n - 1];
	}

	// where it is on the web's plane (pa, pb) and which way it faces (psi), at time t
	void plane_path(float t) {
		if (t < t_out) {
			pa = 0.0; pb = 0.0;
			psi = 1.5707963 + (t - t_hub) * 2.4;
			return;
		}
		if (t < t_spiral0) {
			float e = smooth((t - t_out) / (t_spiral0 - t_out));
			pa = spa[0] * e; pb = spb[0] * e;
			psi = math::atan2(spb[0], spa[0]);
			return;
		}
		int last = spa.size() - 1;
		if (t < t_spiral1) {
			float s = (t - t_spiral0) / (t_spiral1 - t_spiral0) * spl[last];
			spiral_at(s + 0.3);
			float na = qa; float nb = qb;
			spiral_at(s - 0.3);
			psi = math::atan2(nb - qb, na - qa);
			spiral_at(s);
			pa = qa; pb = qb;
			return;
		}
		float ea = spa[last]; float eb = spb[last];
		float home = math::atan2(-eb, -ea);
		if (t < t_home) {
			float e = smooth((t - t_spiral1) / (t_home - t_spiral1));
			pa = ea * (1.0 - e); pb = eb * (1.0 - e);
			psi = home;
			return;
		}
		float ml = math::sqrt(moth_a * moth_a + moth_b * moth_b);
		float da = moth_a / ml; float db = moth_b / ml;
		float toward = math::atan2(db, da);
		if (t < t_rush) {
			pa = 0.0; pb = 0.0;
			psi = turn_toward(home, -1.5707963, smooth((t - t_home) / 0.6));
			if (t > t_impact) { psi = turn_toward(psi, toward, smooth((t - t_impact) / (t_rush - t_impact))); }
			return;
		}
		float reach = ml - 0.85;
		if (t < t_reach) {
			reach = reach * smooth((t - t_rush) / (t_reach - t_rush));
			psi = toward;
		} else if (t < t_turn) {
			float e = smooth((t - t_reach) / (t_turn - t_reach));
			reach = reach - 0.11 * e;
			psi = toward + 3.1415927 * e;
		} else {
			reach = reach - 0.11 + (t < t_wrap1 ? 0.05 * math::sin(t * 8.0) : 0.0);
			psi = toward + 3.1415927 + 0.08 * math::sin(t * 5.0);
		}
		pa = da * reach; pb = db * reach;
	}

	void web_motion(float t, float dt) {
		float opa = pa; float opb = pb;
		if (t < t_hub) {
			float e = smooth((t - t_lift) / (t_hub - t_lift));
			pa = 0.0; pb = -hub_up * (1.0 - e);
			ux = e2x * (1.0 - e) + nnx * e; uy = e2y * (1.0 - e) + nny * e; uz = e2z * (1.0 - e) + nnz * e;
			fx = f0x * (1.0 - e) + e2x * e; fy = f0y * (1.0 - e) + e2y * e; fz = f0z * (1.0 - e) + e2z * e;
			psi = 1.5707963;
		} else {
			plane_path(t);
			float c = math::cos(psi); float s = math::sin(psi);
			fx = e1x * c + e2x * s; fy = e1y * c + e2y * s; fz = e1z * c + e2z * s;
			ux = nnx; uy = nny; uz = nnz;
		}
		float ul = 1.0 / math::sqrt(ux * ux + uy * uy + uz * uz);
		ux *= ul; uy *= ul; uz *= ul;
		float d = fx * ux + fy * uy + fz * uz;
		fx -= ux * d; fy -= uy * d; fz -= uz * d;
		float fl = 1.0 / math::sqrt(fx * fx + fy * fy + fz * fz + 0.0000001);
		fx *= fl; fy *= fl; fz *= fl;
		rx = uy * fz - uz * fy;
		ry = uz * fx - ux * fz;
		rz = ux * fy - uy * fx;
		if (t < t_hub) {
			float e = smooth((t - t_lift) / (t_hub - t_lift));
			bx = g0x + (hbx - g0x) * e; by = g0y + (hby - g0y) * e; bz = g0z + (hbz - g0z) * e;
		} else {
			bx = hbx + e1x * pa + e2x * pb; by = hby + e1y * pa + e2y * pb; bz = hbz + e1z * pa + e2z * pb;
		}
		float ma = pa - opa; float mb = pb - opb;
		spd = dt > 0.0 ? math::sqrt(ma * ma + mb * mb) / dt : 0.0;
		// each spoke runs from the hub to the first wall it meets
		for (int k = 0; k < nspoke; ++k) {
			float ca = math::cos(spoke_ang[k]); float sa = math::sin(spoke_ang[k]);
			float dx = e1x * ca + e2x * sa; float dy = e1y * ca + e2y * sa; float dz = e1z * ca + e2z * sa;
			float s2 = 0.5;
			while (s2 < 7.0) {
				float w = sk_wall(hbx + dx * s2, hby + dy * s2, hbz + dz * s2);
				if (w < 0.02) { break; }
				s2 += w * 0.9;
			}
			spoke_len[k] = s2 > 7.0 ? 7.0 : s2;
		}
	}

	// how far a point of the web (a, b) stands off its plane: the spider's weight, a breeze, the moth's strike
	// ringing out through the threads and its struggle
	float sag(float a, float b) {
		float da = a - pa; float db = b - pb;
		float result = -0.12 / (1.0 + (da * da + db * db) * 3.0);
		result += 0.035 * math::sin(a * 0.8 + tt * 1.1) * math::sin(b * 0.7 - tt * 0.9 + 0.5) * (a * a + b * b) * 0.12;
		if (tt > t_impact) {
			float ma = a - moth_a; float mb = b - moth_b;
			float dist = math::sqrt(ma * ma + mb * mb);
			float s = tt - t_impact;
			float front = s * 3.4 - dist;
			if (front > 0.0) { result += 0.34 / (1.0 + s * s * 1.6) * math::sin(front * 4.5) / (1.0 + dist * 0.6); }
			if (tt < t_wrap1) {
				float fight = 0.07 * (1.0 - coc) * (0.55 + 0.45 * math::sin(tt * 2.3));
				result += fight * math::sin(tt * 21.0 - dist * 4.0) / (1.0 + dist * 2.5);
			}
		}
		return result;
	}

	// a point of the web in the world, into wx, wy, wz
	void web_point(float a, float b) {
		float n = sag(a, b);
		wx = hbx + e1x * a + e2x * b + nnx * n;
		wy = hby + e1y * a + e2y * b + nny * n;
		wz = hbz + e1z * a + e2z * b + nnz * n;
	}

	// the moth at time t: it flutters in along a curve, strikes the web and sticks, beats its wings against the
	// silk, then folds them as the cocoon closes and spins
	void moth_pose(float t) {
		if (t < t_impact) {
			float u = (t - t_moth) / (t_impact - t_moth);
			u = u < 0.0 ? 0.0 : u;
			float w0 = (1.0 - u) * (1.0 - u); float w1 = 2.0 * u * (1.0 - u); float w2 = u * u;
			float a = -3.1 * w0 - 1.0 * w1 + moth_a * w2;
			float b = 1.8 * w0 + 2.4 * w1 + moth_b * w2;
			float n = 1.6 * w0 + 1.0 * w1 + 0.06 * w2;
			float u2 = u + 0.02;
			float va = (-3.1 * (1.0 - u2) * (1.0 - u2) - 1.0 * 2.0 * u2 * (1.0 - u2) + moth_a * u2 * u2) - a;
			float vb = (1.8 * (1.0 - u2) * (1.0 - u2) + 2.4 * 2.0 * u2 * (1.0 - u2) + moth_b * u2 * u2) - b;
			float vn = (1.6 * (1.0 - u2) * (1.0 - u2) + 1.0 * 2.0 * u2 * (1.0 - u2) + 0.06 * u2 * u2) - n;
			float fl = 1.0 - u;
			a += fl * 0.16 * math::sin(t * 6.3);
			b += fl * 0.2 * math::sin(t * 8.9 + 1.0);
			n += fl * 0.14 * math::sin(t * 5.2 + 2.0);
			mox = hbx + e1x * a + e2x * b + nnx * n;
			moy = hby + e1y * a + e2y * b + nny * n;
			moz = hbz + e1z * a + e2z * b + nnz * n;
			float vl = math::sqrt(va * va + vb * vb + vn * vn) + 0.000001;
			mfx = (e1x * va + e2x * vb + nnx * vn) / vl + e2x * 0.5;
			mfy = (e1y * va + e2y * vb + nny * vn) / vl + e2y * 0.5;
			mfz = (e1z * va + e2z * vb + nnz * vn) / vl + e2z * 0.5;
			flap = 0.95 * math::sin(t * 31.4);
			wsc = 1.0; coc = 0.0; spin = 0.0;
		} else {
			web_point(moth_a, moth_b);
			mox = wx + nnx * 0.09; moy = wy + nny * 0.09; moz = wz + nnz * 0.09;
			mfx = e2x * 0.94 + e1x * 0.34; mfy = e2y * 0.94 + e1y * 0.34; mfz = e2z * 0.94 + e1z * 0.34;
			coc = smooth((t - t_wrap0) / (t_wrap1 - 0.6 - t_wrap0));
			spin = t > t_wrap0 ? (t - t_wrap0) * 6.0 : 0.0;
			float beat = math::sin(t * 15.0);
			flap = (1.0 - coc) * 0.55 * (beat > 0.0 ? beat : 0.0) * (0.6 + 0.4 * math::sin(t * 2.3)) + coc * 1.3;
			wsc = 1.0 - 0.7 * coc;
		}
		float ml = 1.0 / math::sqrt(mfx * mfx + mfy * mfy + mfz * mfz);
		mfx *= ml; mfy *= ml; mfz *= ml;
		float d = nnx * mfx + nny * mfy + nnz * mfz;
		mux = nnx - mfx * d; muy = nny - mfy * d; muz = nnz - mfz * d;
		float ul = 1.0 / math::sqrt(mux * mux + muy * muy + muz * muz + 0.0000001);
		mux *= ul; muy *= ul; muz *= ul;
		mrx = muy * mfz - muz * mfy;
		mry = muz * mfx - mux * mfz;
		mrz = mux * mfy - muy * mfx;
		if (t > t_impact) {
			// the struggle jerks it about its spine; the wrapping rolls it
			float roll = spin * 0.12 + 0.3 * (1.0 - coc) * math::sin(t * 9.0);
			float c = math::cos(roll); float s = math::sin(roll);
			float nux = mux * c + mrx * s; float nuy = muy * c + mry * s; float nuz = muz * c + mrz * s;
			mrx = mrx * c - mux * s; mry = mry * c - muy * s; mrz = mrz * c - muz * s;
			mux = nux; muy = nuy; muz = nuz;
		}
	}

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
		// embers rise along the sheet's normal, or the web's up once it hangs there
		float lx = on_web ? e2x : ux; float ly = on_web ? e2y : uy; float lz = on_web ? e2z : uz;
		spawn_acc += dt * (on_web ? 8.0 : 22.0) * SK_WAKE * (0.4 + SK_KICK * 1.6 + SK_ENERGY * 0.5 + gape(tt) * 9.0);
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
				spawn(crx + fx * cxr + rx * sxr, cry + fy * cxr + ry * sxr, crz + fz * cxr + rz * sxr, lx * 0.9 + fx * cxr + rx * sxr, ly * 0.9 + fy * cxr + ry * sxr, lz * 0.9 + fz * cxr + rz * sxr, 0.45 + rng.next(40) * 0.01);
			} else if (kind < 4) {
				float ex2 = hx + fx * 0.52 + ux * 0.14 + rx * jitter(100) * 0.09;
				float ey2 = hy + fy * 0.52 + uy * 0.14 + ry * jitter(100) * 0.09;
				float ez2 = hz + fz * 0.52 + uz * 0.14 + rz * jitter(100) * 0.09;
				spawn(ex2, ey2, ez2, fx * 0.5 + lx * 0.35 + rx * jitter(100) * 0.3, fy * 0.5 + ly * 0.35 + ry * jitter(100) * 0.3, fz * 0.5 + lz * 0.35 + rz * jitter(100) * 0.3, 1.1 + rng.next(80) * 0.01);
			} else if (kind < 6) {
				spawn(hx - fx * 0.55, hy - fy * 0.55, hz - fz * 0.55, -fx * 0.3 + lx * 0.4 + jitter(100) * 0.2, -fy * 0.3 + ly * 0.4 + jitter(100) * 0.2, -fz * 0.3 + lz * 0.4 + jitter(100) * 0.2, 1.4 + rng.next(80) * 0.01);
			} else {
				float r = 0.6 + rng.next(140) * 0.01;
				float a = rng.next(628) * 0.01;
				float ca = math::cos(a) * r;
				float sa = math::sin(a) * r;
				spawn(hx + fx * ca + rx * sa - ux * 0.1, hy + fy * ca + ry * sa - uy * 0.1, hz + fz * ca + rz * sa - uz * 0.1, lx * 0.25, ly * 0.25, lz * 0.25, 2.0 + rng.next(100) * 0.01);
			}
		}
		float drag = 1.0 - dt * 0.9;
		for (int i = 0; i < np; ++i) {
			if (plife[i] <= 0.0) { continue; }
			float ox = ppx[i] - hx;
			float oy = ppy[i] - hy;
			float oz = ppz[i] - hz;
			float sx = ly * oz - lz * oy;
			float sy = lz * ox - lx * oz;
			float sz = lx * oy - ly * ox;
			pvx[i] = (pvx[i] + (lx * 0.9 + sx * 1.3 - ox * 0.25) * dt) * drag;
			pvy[i] = (pvy[i] + (ly * 0.9 + sy * 1.3 - oy * 0.25) * dt) * drag;
			pvz[i] = (pvz[i] + (lz * 0.9 + sz * 1.3 - oz * 0.25) * dt) * drag;
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
		hear(t);
		SK_WAKE = smooth((t - t_wake + 0.4) / 1.0);
		if (!on_web) {
			float turn = 0.75 * math::sin(t * 0.53 + wander) + 0.45 * math::sin(t * 1.37 + wander * 2.0) + 0.2 * math::sin(t * 2.9);
			float ang = turn * dt * 1.2 * smooth((t - t_wake) / 1.2);
			float c = math::cos(ang);
			float s = math::sin(ang);
			float nfx = fx * c + rx * s;
			float nfy = fy * c + ry * s;
			float nfz = fz * c + rz * s;
			fx = nfx; fy = nfy; fz = nfz;
			float speed = (0.38 + 0.12 * math::sin(t * 0.8)) * smooth((t - t_wake) / 1.2);
			bx += fx * speed * dt;
			by += fy * speed * dt;
			bz += fz * speed * dt;
			sk_project(bx, by, bz);
			frame_from_normal();
			if (!planned) {
				hbx = bx + ux * hub_up; hby = by + uy * hub_up; hbz = bz + uz * hub_up;
				if (t >= t_melt && placed) { plan_web(); }
			}
			if (t >= t_lift && planned) { leave_sheet(); }
		}
		if (on_web) { web_motion(t, dt); }
		float spring = lunge(t);
		if (spring > 0.0) {
			float lx = ex - bx; float ly = ey - by; float lz = ez - bz;
			float ll = 0.75 * spring / math::sqrt(lx * lx + ly * ly + lz * lz + 0.000001);
			bx += lx * ll; by += ly * ll; bz += lz * ll;
		}
		SK_HBX = hbx; SK_HBY = hby; SK_HBZ = hbz;
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
		walk(dt);
		update_particles(dt);
		aim_camera(t, dt);
		placed = true;
	}

	// the tetrapod gait: a foot steps when its rest spot has run off, one group at a time; on the web at speed it
	// scuttles, with quicker steps that lead the body and both groups free
	void walk(float dt) {
		float step_time = on_web ? (spd > 1.5 ? 0.08 : 0.13) : 0.17;
		float lead = 0.2;
		if (on_web) {
			lead = spd * 0.1;
			lead = lead > 0.45 ? 0.45 : lead;
		}
		float trigger = on_web ? 0.05 : 0.1;
		bool scuttle = on_web && spd > 1.5;
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
				stept[i] = stept[i] + dt / step_time;
				rest_foot(i, lead);
				if (stept[i] >= 1.0) {
					stept[i] = -1.0;
					footx[i] = tox[i]; footy[i] = toy[i]; footz[i] = toz[i];
				} else {
					float e = stept[i];
					float lift = math::sin(e * 3.14159265) * (on_web ? 0.16 : 0.22);
					footx[i] = fromx[i] + (tox[i] - fromx[i]) * e + ux * lift;
					footy[i] = fromy[i] + (toy[i] - fromy[i]) * e + uy * lift;
					footz[i] = fromz[i] + (toz[i] - fromz[i]) * e + uz * lift;
				}
			} else {
				float px = footx[i];
				float py = footy[i];
				float pz = footz[i];
				project_foot(px, py, pz);
				footx[i] = px; footy[i] = py; footz[i] = pz;
				rest_foot(i, 0.0);
				float ddx = footx[i] - tox[i];
				float ddy = footy[i] - toy[i];
				float ddz = footz[i] - toz[i];
				bool other_moving = group_a ? stepping_b : stepping_a;
				if ((scuttle || !other_moving) && ddx * ddx + ddy * ddy + ddz * ddz > trigger) {
					stept[i] = 0.0;
					fromx[i] = footx[i]; fromy[i] = footy[i]; fromz[i] = footz[i];
					if (group_a) { stepping_a = true; } else { stepping_b = true; }
				}
			}
		}
	}

	// the camera orbits the creature with the sheet's normal as up; on the web it stands off the plane, framing the
	// whole web, then closes in on the moth and the spider; walls it would pass through pull it in
	void aim_camera(float t, float dt) {
		float bob = 0.2 + 0.018 * math::sin(t * 11.0);
		float hx = bx + ux * bob;
		float hy = by + uy * bob;
		float hz = bz + uz * bob;
		float orbit = 2.4 + t * 0.2 + 0.5 * math::sin(t * 0.13);
		float wide = 1.0 + 1.1 * (1.0 - smooth((t - t_wake) / 2.5));
		float oc = math::cos(orbit) * 1.7 * wide;
		float os = math::sin(orbit) * 1.7 * wide;
		float dex = hx + fx * oc + rx * os + ux * 1.0 * wide;
		float dey = hy + fy * oc + ry * os + uy * 1.0 * wide;
		float dez = hz + fz * oc + rz * os + uz * 1.0 * wide;
		float tgx = hx; float tgy = hy; float tgz = hz;
		float lax = hx + fx * 0.3 + ux * 0.25; float lay = hy + fy * 0.3 + uy * 0.25; float laz = hz + fz * 0.3 + uz * 0.25;
		float min_reach = 1.5;
		if (planned) {
			float near = smooth((t - t_impact + 0.4) / (t_reach - t_impact + 0.4)) * (1.0 - smooth((t - t_wrap1 + 0.4) / 2.8));
			float wtx = hbx + (hx - hbx) * 0.55; float wty = hby + (hy - hby) * 0.55; float wtz = hbz + (hz - hbz) * 0.55;
			if (on_web && t > t_moth) {
				moth_pose(t);
				wtx += ((hx + mox) * 0.5 - wtx) * near; wty += ((hy + moy) * 0.5 - wty) * near; wtz += ((hz + moz) * 0.5 - wtz) * near;
			}
			float creep = stalk(t);
			float spring = lunge(t);
			float onto = facing(t) * 0.6 + creep * 0.4;
			wtx += (hx + fx * 0.34 - wtx) * onto; wty += (hy + fy * 0.34 - wty) * onto; wtz += (hz + fz * 0.34 - wtz) * onto;
			float dist = 3.4 + (2.0 - 3.4) * near + 1.6 * smooth((t - t_wrap1 + 0.4) / 2.8) - creep * 2.1 - spring * 1.1;
			float oa = 0.3 + 0.3 * math::sin(t * 0.19);
			float el = 0.45 + 0.15 * math::sin(t * 0.31);
			float dx = nnx * math::cos(oa) + e1x * math::sin(oa) + e2x * el;
			float dy = nny * math::cos(oa) + e1y * math::sin(oa) + e2y * el;
			float dz = nnz * math::cos(oa) + e1z * math::sin(oa) + e2z * el;
			// the eighth act swings round in front of the skull, whose face is reared 0.85 radians off the body
			float dn = math::sqrt(dx * dx + dy * dy + dz * dz);
			dx += ((fx * 0.66 + ux * 0.7513) * dn - dx) * onto;
			dy += ((fy * 0.66 + uy * 0.7513) * dn - dy) * onto;
			dz += ((fz * 0.66 + uz * 0.7513) * dn - dz) * onto;
			float dl = dist / math::sqrt(dx * dx + dy * dy + dz * dz);
			float w = smooth((t - t_melt) / 1.5);
			dex += (wtx + dx * dl - dex) * w; dey += (wty + dy * dl - dey) * w; dez += (wtz + dz * dl - dez) * w;
			tgx += (wtx - tgx) * w; tgy += (wty - tgy) * w; tgz += (wtz - tgz) * w;
			lax += (wtx - lax) * w; lay += (wty - lay) * w; laz += (wtz - laz) * w;
			min_reach = 1.2;
		}
		float cdx = dex - tgx;
		float cdy = dey - tgy;
		float cdz = dez - tgz;
		float want = math::sqrt(cdx * cdx + cdy * cdy + cdz * cdz);
		cdx /= want; cdy /= want; cdz /= want;
		float reach = min_reach;
		while (reach < want) {
			float d = sk_wall(tgx + cdx * reach, tgy + cdy * reach, tgz + cdz * reach);
			if (d < 0.14) { break; }
			reach += d * 0.8;
		}
		reach = reach > want ? want : reach;
		reach = reach - 0.1;
		reach = reach < min_reach ? min_reach : reach;
		dex = tgx + cdx * reach;
		dey = tgy + cdy * reach;
		dez = tgz + cdz * reach;
		float kc = placed ? dt * (2.8 + 9.0 * lunge(t) * (1.0 - smooth((t - t_lunge - 0.5) / 0.5))) : 1.0;
		kc = kc > 1.0 ? 1.0 : kc;
		ex += (dex - ex) * kc; ey += (dey - ey) * kc; ez += (dez - ez) * kc;
		float kl = placed ? dt * 4.0 : 1.0;
		kl = kl > 1.0 ? 1.0 : kl;
		lkx += (lax - lkx) * kl; lky += (lay - lky) * kl; lkz += (laz - lkz) * kl;
		float ku = placed ? dt * 2.2 : 1.0;
		ku = ku > 1.0 ? 1.0 : ku;
		float wux = planned ? e2x : ux; float wuy = planned ? e2y : uy; float wuz = planned ? e2z : uz;
		float onto2 = facing(t) * 0.6 + stalk(t) * 0.4;
		wux += (ux * 0.66 - fx * 0.7513 - wux) * onto2; wuy += (uy * 0.66 - fy * 0.7513 - wuy) * onto2; wuz += (uz * 0.66 - fz * 0.7513 - wuz) * onto2;
		cux += (wux - cux) * ku; cuy += (wuy - cuy) * ku; cuz += (wuz - cuz) * ku;
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

	// a leg from hip to foot: femur and tibia by two-bone IK to an ankle held above the foot, then the tarsus
	int leg(int ci, int i, float hx, float hy, float hz, float side) {
		float px = footx[i]; float py = footy[i]; float pz = footz[i];
		float hdx = px - hx; float hdy = py - hy; float hdz = pz - hz;
		float hu = hdx * ux + hdy * uy + hdz * uz;
		hdx -= ux * hu; hdy -= uy * hu; hdz -= uz * hu;
		float hl = 1.0 / math::sqrt(hdx * hdx + hdy * hdy + hdz * hdz + 0.000001);
		hdx *= hl; hdy *= hl; hdz *= hl;
		float ax = px + ux * 0.13 - hdx * 0.06;
		float ay = py + uy * 0.13 - hdy * 0.06;
		float az = pz + uz * 0.13 - hdz * 0.06;
		float dx = ax - hx;
		float dy = ay - hy;
		float dz = az - hz;
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
		float tx = hx + dx * dc;
		float ty = hy + dy * dc;
		float tz = hz + dz * dc;
		capsule(ci, hx, hy, hz, kx, ky, kz, 0.042);
		capsule(ci + 1, kx, ky, kz, tx, ty, tz, 0.03);
		capsule(ci + 2, tx, ty, tz, px, py, pz, 0.018);
		glow(3 + i, kx, ky, kz, 0.027 + 0.006 * math::sin(tt * 7.0 + i));
		return ci + 3;
	}

	void render(var& grid) override {
		set_world(tt);
		// the rays trace at most about 4,600 pixels (a sextant grid is 2.25 times a half-block
		// one) and the picture scales up to the grid; the web, the moth's dust and the
		// particles draw over it at full size
		// from the strike through the wrap the creature fills the frame and every ray is dear, so fewer are traced
		float close = smooth((tt - t_impact + 0.6) / 0.5) * (1.0 - smooth((tt - t_wrap1 - 0.3) / 1.2));
		float glare = facing(tt) * 0.3 + stalk(tt) * 0.7;
		// from the draw back on the open chamber fills the frame, and the eighth act closes in: fewer rays again
		float late = smooth((tt - t_wrap1) / 2.0) * 0.35 + glare * 0.5;
		close = close > late ? close : late;
		float trace_k = math::sqrt((4600.0 - close * 2100.0) * (1.0 - SK_PUNCH * 0.25) / (w * h));
		trace_k = trace_k > 1.0 ? 1.0 : trace_k;
		int tw = itrunc(w * trace_k);
		int th = itrunc(h * trace_k);
		SK_W = tw;
		SK_H = th;
		SK_FLICK = 0.7 * math::sin(tt * 17.0) * math::sin(tt * 5.3);
		if (tt > t_wrap1) { SK_FLICK = SK_FLICK * 0.4 + 0.9 + glare * 1.4; }
		float bob = 0.2 + 0.018 * math::sin(tt * 11.0);
		float cx = bx + ux * bob;
		float cy = by + uy * bob;
		float cz = bz + uz * bob;
		SK_PLX = cx; SK_PLY = cy; SK_PLZ = cz;
		// on the web the skull rears back to face the viewer
		float pitch = on_web ? 0.85 * smooth((tt - t_lift) / 0.8) : 0.0;
		float cpt = math::cos(pitch); float spt = math::sin(pitch);
		float hfx = fx * cpt + ux * spt; float hfy = fy * cpt + uy * spt; float hfz = fz * cpt + uz * spt;
		float hux = ux * cpt - fx * spt; float huy = uy * cpt - fy * spt; float huz = uz * cpt - fz * spt;
		SK_PFX = hfx; SK_PFY = hfy; SK_PFZ = hfz;
		SK_PUX = hux; SK_PUY = huy; SK_PUZ = huz;
		SK_PRX = rx; SK_PRY = ry; SK_PRZ = rz;
		SK_QFX = fx; SK_QFY = fy; SK_QFZ = fz;
		SK_QUX = ux; SK_QUY = uy; SK_QUZ = uz;
		SK_TX = cx; SK_TY = cy; SK_TZ = cz;
		SK_AR = 0.27;
		SK_AX = cx - fx * 0.45 + ux * 0.12; SK_AY = cy - fy * 0.45 + uy * 0.12; SK_AZ = cz - fz * 0.45 + uz * 0.12;
		float nod = 0.03 * math::sin(tt * 2.1);
		SK_HX = cx + fx * 0.34 + ux * (0.09 + nod); SK_HY = cy + fy * 0.34 + uy * (0.09 + nod); SK_HZ = cz + fz * 0.34 + uz * (0.09 + nod);
		SK_NX = SK_HX + hfx * 0.23 - hux * 0.06; SK_NY = SK_HY + hfy * 0.23 - huy * 0.06; SK_NZ = SK_HZ + hfz * 0.23 - huz * 0.06;
		float jaw = 0.012 * math::sin(tt * 6.0) + 0.1 * gape(tt);
		SK_JX = SK_HX + hfx * 0.1 - hux * (0.14 + jaw); SK_JY = SK_HY + hfy * 0.1 - huy * (0.14 + jaw); SK_JZ = SK_HZ + hfz * 0.1 - huz * (0.14 + jaw);
		SK_BCX = cx; SK_BCY = cy; SK_BCZ = cz;
		SK_BR = 1.6;
		int ncap = 5 + 2 + 24 + 4 + 2 + 7;
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
		float eye = 0.03 * (1.0 + glare * 0.8);
		SK_GLARE = glare;
		glow(0, SK_HX + hfx * 0.175 + hux * 0.03 + rx * 0.088, SK_HY + hfy * 0.175 + huy * 0.03 + ry * 0.088, SK_HZ + hfz * 0.175 + huz * 0.03 + rz * 0.088, eye);
		glow(1, SK_HX + hfx * 0.175 + hux * 0.03 - rx * 0.088, SK_HY + hfy * 0.175 + huy * 0.03 - ry * 0.088, SK_HZ + hfz * 0.175 + huz * 0.03 - rz * 0.088, eye);
		float third = (0.024 + 0.006 * math::sin(tt * 3.0)) * (1.0 + glare * 0.5);
		glow(2, SK_HX + hfx * 0.17 + hux * 0.15, SK_HY + hfy * 0.17 + huy * 0.15, SK_HZ + hfz * 0.17 + huz * 0.15, third);
		SK_LX = SK_GX[2] * 0.4 + (SK_GX[0] + SK_GX[1]) * 0.3 + hfx * 0.1;
		SK_LY = SK_GY[2] * 0.4 + (SK_GY[0] + SK_GY[1]) * 0.3 + hfy * 0.1;
		SK_LZ = SK_GZ[2] * 0.4 + (SK_GZ[0] + SK_GZ[1]) * 0.3 + hfz * 0.1;
		capsule(0, SK_JX - rx * 0.08, SK_JY - ry * 0.08, SK_JZ - rz * 0.08, SK_JX + rx * 0.08, SK_JY + ry * 0.08, SK_JZ + rz * 0.08, 0.05);
		int ci = horn(1, 1.0);
		ci = horn(ci, -1.0);
		float fang = 0.02 * math::sin(tt * 6.0 + 1.0) + 0.05 * gape(tt);
		for (int s = 0; s < 2; ++s) {
			float side = s == 0 ? 1.0 : -1.0;
			float ax = SK_JX + hfx * 0.05 + rx * side * 0.035;
			float ay = SK_JY + hfy * 0.05 + ry * side * 0.035;
			float az = SK_JZ + hfz * 0.05 + rz * side * 0.035;
			capsule(ci, ax, ay, az, ax + hfx * 0.07 - hux * 0.12 - rx * side * (0.02 + fang), ay + hfy * 0.07 - huy * 0.12 - ry * side * (0.02 + fang), az + hfz * 0.07 - huz * 0.12 - rz * side * (0.02 + fang), 0.016);
			ci = ci + 1;
		}
		for (int i = 0; i < 8; ++i) {
			float side = i < 4 ? 1.0 : -1.0;
			float fo = legfo[i % 4];
			float hx = cx + fx * fo * 0.45 + rx * side * 0.1 + ux * 0.03;
			float hy = cy + fy * fo * 0.45 + ry * side * 0.1 + uy * 0.03;
			float hz = cz + fz * fo * 0.45 + rz * side * 0.1 + uz * 0.03;
			ci = leg(ci, i, hx, hy, hz, side);
		}
		// pedipalps feel the air in front of the jaw
		for (int s = 0; s < 2; ++s) {
			float side = s == 0 ? 1.0 : -1.0;
			float twitch = 0.03 * math::sin(tt * 7.3 + s * 2.0);
			float px = SK_HX + hfx * 0.2 - hux * 0.1 + rx * side * 0.08;
			float py = SK_HY + hfy * 0.2 - huy * 0.1 + ry * side * 0.08;
			float pz = SK_HZ + hfz * 0.2 - huz * 0.1 + rz * side * 0.08;
			float mx = px + hfx * 0.11 + hux * (0.05 + twitch) + rx * side * 0.05;
			float my = py + hfy * 0.11 + huy * (0.05 + twitch) + ry * side * 0.05;
			float mz = pz + hfz * 0.11 + huz * (0.05 + twitch) + rz * side * 0.05;
			capsule(ci, px, py, pz, mx, my, mz, 0.02);
			capsule(ci + 1, mx, my, mz, mx + hfx * 0.08 - hux * 0.09, my + hfy * 0.08 - huy * 0.09, mz + hfz * 0.08 - huz * 0.09, 0.017);
			ci = ci + 2;
		}
		// the spinnerets at the abdomen's tip
		for (int s = 0; s < 2; ++s) {
			float side = s == 0 ? 1.0 : -1.0;
			float sx = SK_AX - fx * 0.32 + rx * side * 0.03 - ux * 0.02;
			float sy = SK_AY - fy * 0.32 + ry * side * 0.03 - uy * 0.02;
			float sz = SK_AZ - fz * 0.32 + rz * side * 0.03 - uz * 0.02;
			capsule(ci, sx, sy, sz, sx - fx * 0.08 - ux * 0.03, sy - fy * 0.08 - uy * 0.03, sz - fz * 0.08 - uz * 0.03, 0.02);
			ci = ci + 1;
		}
		float hover = 0.03 * math::sin(tt * 1.7);
		SK_HGX = SK_HX + hux * 0.3; SK_HGY = SK_HY + huy * 0.3; SK_HGZ = SK_HZ + huz * 0.3;
		SK_BGX = (SK_AX + SK_TX) * 0.5; SK_BGY = (SK_AY + SK_TY) * 0.5; SK_BGZ = (SK_AZ + SK_TZ) * 0.5;
		SK_CRX = SK_HX + hux * (0.44 + hover); SK_CRY = SK_HY + huy * (0.44 + hover); SK_CRZ = SK_HZ + huz * (0.44 + hover);
		for (int k = 0; k < 7; ++k) {
			float a = k * 0.8976 + tt * 0.6;
			float ca = math::cos(a) * 0.2;
			float sa = math::sin(a) * 0.2;
			float flame = (0.02 + 0.08 * SK_WAKE) + (0.05 * math::sin(tt * 9.0 + k * 1.9) + 0.03 * math::sin(tt * 23.0 + k)) * SK_WAKE + 0.03 * SK_KICK * SK_WAKE + (0.07 + 0.05 * SK_KICK) * glare;
			float bxk = SK_CRX + hfx * ca + rx * sa;
			float byk = SK_CRY + hfy * ca + ry * sa;
			float bzk = SK_CRZ + hfz * ca + rz * sa;
			capsule(SK_CROWN0 + k, bxk, byk, bzk, bxk + hux * flame + hfx * ca * 0.25 + rx * sa * 0.25, byk + huy * flame + hfy * ca * 0.25 + ry * sa * 0.25, bzk + huz * flame + hfz * ca * 0.25 + rz * sa * 0.25, 0.018);
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
		float spin_gem = tt * 2.2;
		float gl2 = 0.3 + 0.02 * math::sin(tt * 2.5);
		SK_GMX = SK_CRX + hux * gl2; SK_GMY = SK_CRY + huy * gl2; SK_GMZ = SK_CRZ + huz * gl2;
		SK_GAX = rx * math::cos(spin_gem) + hfx * math::sin(spin_gem); SK_GAY = ry * math::cos(spin_gem) + hfy * math::sin(spin_gem); SK_GAZ = rz * math::cos(spin_gem) + hfz * math::sin(spin_gem);
		SK_GBX = hfx * math::cos(spin_gem) - rx * math::sin(spin_gem); SK_GBY = hfy * math::cos(spin_gem) - ry * math::sin(spin_gem); SK_GBZ = hfz * math::cos(spin_gem) - rz * math::sin(spin_gem);
		pose_moth();
		SK_EX = ex; SK_EY = ey; SK_EZ = ez;
		SK_EYE_INSIDE = sk_wall(ex, ey, ez) < 0.0;
		float lx = lkx - ex;
		float ly = lky - ey;
		float lz = lkz - ez;
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
		// the key light: above the camera, a little to its right, toward the scene
		float kx = upx * 0.75 - lx * 0.45 + SK_RX * 0.35;
		float ky = upy * 0.75 - ly * 0.45 + SK_RY * 0.35;
		float kz = upz * 0.75 - lz * 0.45 + SK_RZ * 0.35;
		float kl = 1.0 / math::sqrt(kx * kx + ky * ky + kz * kz);
		SK_KX = kx * kl; SK_KY = ky * kl; SK_KZ = kz * kl;
		if (SK_ROWS.size() != th) {
			SK_ROWS = [];
			for (int r = 0; r < th; ++r) { SK_ROWS.push(r); }
		}
		var rows = parallel_transform(SK_ROWS, sk_row);
		if (depth.size() != w * h) {
			depth = [];
			for (int q = 0; q < w * h; ++q) { depth.push(0); }
		}
		int idx = 0;
		for (int y = 0; y < h; ++y) {
			var& row = rows[y * th / h];
			for (int x = 0; x < w; ++x) {
				int v = row[x * tw / w];
				grid[idx] = v % 1024;
				depth[idx] = v / 1024;
				idx = idx + 1;
			}
		}
		light_shafts(grid);
		draw_web(grid);
		draw_particles(grid);
		strike_shake(grid);
	}

	// shafts of light fall through the chamber from high off the frame's right, swaying slowly and
	// brightening on the kick; dust drifts in them
	void light_shafts(var& grid) {
		if (SK_WAKE <= 0.0) { return; }
		float lsx = w * 0.84;
		float lsy = 0.0 - h * 0.4;
		float reach = 1.0 / (h * 2.2);
		float boost = (0.55 + 0.7 * SK_KICK + (cue != null ? cue.hit(tt) * 0.6 : 0.0)) * SK_WAKE * (1.0 - 0.6 * stalk(tt) * (1.0 - lunge(tt)));
		int idx = 0;
		for (int y = 0; y < h; ++y) {
			float ay = y - lsy;
			for (int x = 0; x < w; ++x) {
				int v = grid[idx];
				bool wall = v >= SK_WORLD && v < SK_WORLD + 144;
				bool fog = v >= SK_FOG && v < SK_FOG + 10;
				if (wall || fog) {
					// a line through the light keeps its slope, so the slope names the shaft
					float ax = x - lsx;
					float ang = ax / ay;
					float beam = math::sin(ang * 17.0 + tt * 0.05 + math::sin(ang * 5.0 + tt * 0.11) * 1.5);
					if (beam > 0.3) {
						float fall = 1.0 - math::sqrt(ax * ax + ay * ay) * reach;
						int add = itrunc((beam - 0.3) * 10.0 * (fall < 0.0 ? 0.0 : fall) * boost);
						if (add > 0) {
							if (wall) {
								int lv = (v - SK_WORLD) % 12;
								grid[idx] = v + (lv + add > 11 ? 11 - lv : add);
							} else {
								int lv2 = v - SK_FOG;
								grid[idx] = v + (lv2 + add > 9 ? 9 - lv2 : add);
							}
							// a mote of dust in the beam
							int mote = (x * 7349 + y * 1931 + itrunc(tt * 9.0) * 31) % 997;
							if (mote < 6 && beam > 0.8) { grid[idx] = SK_SILK + ((x + y) % 8) * 6 + 2 + mote % 3; }
						}
					}
				}
				++idx;
			}
		}
	}

	// the strike and the lunge shake the frame for half a second
	void strike_shake(var& grid) {
		float s = tt - (tt >= t_lunge ? t_lunge : t_impact);
		if (s < 0.0 || s > 0.5) { return; }
		float amp = (0.5 - s) * w * 0.05;
		array<int> row = [];
		for (int x = 0; x < w; ++x) { row.push(0); }
		for (int y = 0; y < h; ++y) {
			int off = itrunc(math::sin(y * 0.7 + tt * 61.0) * amp);
			if (off == 0) { continue; }
			int base = y * w;
			for (int x2 = 0; x2 < w; ++x2) { row[x2] = grid[base + x2]; }
			for (int x3 = 0; x3 < w; ++x3) {
				int from = x3 - off;
				from = from < 0 ? 0 : (from >= w ? w - 1 : from);
				grid[base + x3] = row[from];
			}
		}
	}

	void pose_moth() {
		SK_MOTH = on_web && tt > t_moth;
		if (!SK_MOTH) { return; }
		moth_pose(tt);
		SK_MX = mox; SK_MY = moy; SK_MZ = moz;
		SK_MFX = mfx; SK_MFY = mfy; SK_MFZ = mfz;
		SK_MUX = mux; SK_MUY = muy; SK_MUZ = muz;
		SK_MRX = mrx; SK_MRY = mry; SK_MRZ = mrz;
		SK_WSC = wsc;
		SK_COC = coc;
		SK_SPIN = spin;
		SK_MB = 0.68 * wsc + 0.12 + 0.28 * coc + 0.06;
		SK_MB = SK_MB < 0.4 ? 0.4 : SK_MB;
		float c = math::cos(flap); float s = math::sin(flap);
		for (int k = 0; k < 4; ++k) {
			float side = k % 2 == 0 ? 1.0 : -1.0;
			bool fore = k < 2;
			float off = fore ? 0.045 : -0.07;
			SK_WOX[k] = mox + mfx * off + mrx * side * 0.05;
			SK_WOY[k] = moy + mfy * off + mry * side * 0.05;
			SK_WOZ[k] = moz + mfz * off + mrz * side * 0.05;
			SK_WSX[k] = mrx * side * c + mux * s; SK_WSY[k] = mry * side * c + muy * s; SK_WSZ[k] = mrz * side * c + muz * s;
			SK_WNX[k] = -mrx * side * s + mux * c; SK_WNY[k] = -mry * side * s + muy * c; SK_WNZ[k] = -mrz * side * s + muz * c;
			SK_WCA[k] = (fore ? 0.33 : 0.23) * wsc;
			SK_WSA[k] = (fore ? 0.33 : 0.23) * wsc;
			SK_WCB[k] = (fore ? 0.0 : -0.11) * wsc;
			SK_WSB[k] = (fore ? 0.16 : 0.135) * wsc;
		}
	}

	// a world point onto the screen, into qsx, qsy and its depth qzc; false behind the camera
	bool project(float x, float y, float z) {
		float ox = x - ex; float oy = y - ey; float oz = z - ez;
		float zc = ox * SK_FX + oy * SK_FY + oz * SK_FZ;
		if (zc < 0.08) { return false; }
		float half_h = h * 0.5;
		float scale = half_h / (SK_ZOOM * zc);
		qsx = (ox * SK_RX + oy * SK_RY + oz * SK_RZ) * scale + w * 0.5;
		qsy = half_h - (ox * SK_UX + oy * SK_UY + oz * SK_UZ) * scale;
		qzc = zc;
		return true;
	}

	// a silk thread from (x0, y0, z0) to (x1, y1, z1), depth-tested against the traced scene; its hue turns with its
	// angle on the screen and with time, the way silk breaks light into colors
	void thread(var& grid, float x0, float y0, float z0, float x1, float y1, float z1, float bright) {
		if (!project(x0, y0, z0)) { return; }
		float sx0 = qsx; float sy0 = qsy; float z0c = qzc;
		if (!project(x1, y1, z1)) { return; }
		float ddx = qsx - sx0; float ddy = qsy - sy0;
		float adx = ddx < 0.0 ? -ddx : ddx;
		float ady = ddy < 0.0 ? -ddy : ddy;
		int steps = itrunc((adx > ady ? adx : ady) + 1.0);
		if (steps > 400) { return; }
		float hue = math::atan2(ddy, ddx) * 0.4 + tt * 0.15 + (sx0 + sy0) * 0.006;
		hue = hue - math::floor(hue / 8.0) * 8.0;
		int hq = itrunc(hue);
		hq = hq > 7 ? 7 : hq;
		int ink = SK_SILK + hq * 6;
		float ix = ddx / steps; float iy = ddy / steps; float iz = (qzc - z0c) / steps;
		for (int s = 0; s <= steps; ++s) {
			int px = itrunc(math::floor(sx0 + ix * s));
			int py = itrunc(math::floor(sy0 + iy * s));
			if (px < 0 || py < 0 || px >= w || py >= h) { continue; }
			int at = py * w + px;
			float zc = z0c + iz * s;
			if (depth[at] <= itrunc(zc * 18.0)) { continue; }
			float fog = 1.0 - zc / 11.0;
			int l = itrunc(fog * bright * 5.5 + SK_BAYER[(py % 4) * 4 + px % 4] / 16.0 - 0.3);
			if (l < 0) { continue; }
			grid[at] = ink + (l > 5 ? 5 : l);
		}
	}

	void glint(var& grid, float x, float y, float z) {
		if (!project(x, y, z)) { return; }
		int px = itrunc(math::floor(qsx));
		int py = itrunc(math::floor(qsy));
		if (px < 0 || py < 0 || px >= w || py >= h) { return; }
		int at = py * w + px;
		if (depth[at] <= itrunc(qzc * 18.0)) { return; }
		grid[at] = SK_EYE + 4;
	}

	void draw_web(var& grid) {
		if (!on_web) { return; }
		// the spokes: the dragline first, then the rest cast one after another, a spark at each growing tip
		for (int k = 0; k < nspoke; ++k) {
			float start = k == 0 ? t_lift : t_spokes + (k - 1) * t_spoke_gap;
			if (tt < start) { continue; }
			float grow = k == 0 ? 1.0 : (tt - start) / 0.4;
			grow = grow > 1.0 ? 1.0 : grow;
			float len = spoke_len[k] * grow;
			float ca = math::cos(spoke_ang[k]); float sa = math::sin(spoke_ang[k]);
			web_point(0.0, 0.0);
			float px = wx; float py = wy; float pz = wz;
			for (int s = 1; s <= 12; ++s) {
				float r = len * s / 12.0;
				web_point(ca * r, sa * r);
				thread(grid, px, py, pz, wx, wy, wz, 0.78);
				if (grow >= 1.0 && s % 3 == 0 && math::sin(tt * 1.1 + k * 2.3 + s) > 0.9 - SK_KICK * 0.1) { glint(grid, wx, wy, wz); }
				px = wx; py = wy; pz = wz;
			}
			if (grow < 1.0) { glint(grid, px, py, pz); }
		}
		// the hub's little mesh
		if (tt > t_out) {
			for (int ring = 1; ring <= 2; ++ring) {
				float rr = ring * 0.16;
				web_point(rr, 0.0);
				float px = wx; float py = wy; float pz = wz;
				for (int s = 1; s <= 12; ++s) {
					float a = s * 0.5235988;
					web_point(rr * math::cos(a), rr * math::sin(a));
					thread(grid, px, py, pz, wx, wy, wz, 0.8);
					px = wx; py = wy; pz = wz;
				}
			}
		}
		// the capture spiral as far as it is laid, its last thread running to the spinnerets; dew glints at the crossings
		if (tt > t_spiral0) {
			int last = spa.size() - 1;
			float laid = tt < t_spiral1 ? (tt - t_spiral0) / (t_spiral1 - t_spiral0) * spl[last] : spl[last];
			web_point(spa[0], spb[0]);
			float px = wx; float py = wy; float pz = wz;
			for (int i = 1; i <= last; ++i) {
				if (spl[i] > laid) { break; }
				web_point(spa[i], spb[i]);
				thread(grid, px, py, pz, wx, wy, wz, 0.88);
				if (math::sin(tt * 1.3 + i * 1.7) > 0.84 - SK_KICK * 0.12) { glint(grid, wx, wy, wz); }
				px = wx; py = wy; pz = wz;
			}
			if (tt < t_spiral1) {
				thread(grid, px, py, pz, SK_AX - fx * 0.4, SK_AY - fy * 0.4, SK_AZ - fz * 0.4, 1.2);
			}
		}
		// wrapping: strands from the spinnerets wind onto the turning cocoon
		if (tt > t_wrap0 && tt < t_wrap1 + 0.3) {
			float sx = SK_AX - fx * 0.4; float sy = SK_AY - fy * 0.4; float sz = SK_AZ - fz * 0.4;
			float rad = 0.07 + 0.14 * coc;
			for (int j = 0; j < 4; ++j) {
				float a = spin * 1.6 + j * 1.5708;
				float along = -0.2 + j * 0.1 + 0.07 * math::sin(spin + j);
				float tx = mox + mfx * along + (mrx * math::cos(a) + mux * math::sin(a)) * rad;
				float ty = moy + mfy * along + (mry * math::cos(a) + muy * math::sin(a)) * rad;
				float tz = moz + mfz * along + (mrz * math::cos(a) + muz * math::sin(a)) * rad;
				thread(grid, sx, sy, sz, tx, ty, tz, 1.25);
			}
		}
	}

	void plot(var& grid, int x, int y, int dq, int level) {
		if (x < 0 || y < 0 || x >= w || y >= h) { return; }
		int at = y * w + x;
		if (depth[at] <= dq) { return; }
		int cur = grid[at];
		if (cur >= SK_SPARK && cur < SK_SPARK + 8 && cur - SK_SPARK >= level) { return; }
		grid[at] = SK_SPARK + level;
	}

	void splat(var& grid, float x, float y, float z, int level, int size) {
		if (!project(x, y, z)) { return; }
		int px = itrunc(math::floor(qsx));
		int py = itrunc(math::floor(qsy));
		int dq = itrunc(qzc * 18.0);
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
		int orbiting = SK_WAKE > 0.5 ? 3 : 0;
		for (int g = 0; g < orbiting; ++g) {
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
