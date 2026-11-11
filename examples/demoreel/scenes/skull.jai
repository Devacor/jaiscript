// OSSUARY — a hunt in twelve bars. A horned, three-eyed skull on a spider's body
// hangs asleep from its dragline in a ribbed vault. On bar 2 its crown of flame
// catches and it wakes; it casts the spokes of an orb web one after another and
// runs the capture spiral, every winding growing behind it, home by bar 4. A moth
// with eyespotted wings flutters in and strikes on bar 6's downbeat: the strike
// rings out through every thread, the spider runs it down on eight three-segment
// IK legs in a tetrapod gait and wraps it in a cocoon that closes on bar 8. Then
// the hunter turns on the viewer: the camera swings round in front of its face,
// its eyes kindle and its crown roars up while the vault darkens, it stalks
// through bar 9, lunges at the lens on bar 10's downbeat and holds the frame,
// jaws snapping on every kick. The vault answers the music: a ring of light
// leaves the creature on every kick and the vault's hue follows the chord. The
// creature, the moth and its cocoon are distance fields in one march of about
// 6400 rays whatever the frame's size; the silk is drawn over the trace,
// depth-tested. Rows trace in parallel.

float SK_T = 0.0;
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
float SK_WAKE = 1.0;         // 0 while the creature sleeps
float SK_GLARE = 0.0;        // the hunter's glare at the viewer, which kindles its eyes and lights its face
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
int SK_W = 0;
int SK_H = 0;
int SK_BONE = 0;
int SK_CHIT = 0;
int SK_EYE = 0;
int SK_MARK = 0;
int SK_SPARK = 0;
int SK_GEM = 0;
array<int> SK_ROWS = [];
array<int> SK_BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];
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
int SK_BACK = 0;
int SK_BACK_HUE = 0;
int SK_BACK_COLS = 0;
array<float> SK_BACK_LIGHT = [];
array<float> SK_BACK_WARM = [];

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

// the creature along one ray: only the parts the ray passes near (groups: 1 body, 2 head and its eyes, 4 moth; cand: capsule
// indices, glows as 100 + index).
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
		for (int e = 0; e < SK_NSOCK; ++e) {
			float ex = x - SK_GX[e];
			float ey = y - SK_GY[e];
			float ez = z - SK_GZ[e];
			float de = math::sqrt(ex * ex + ey * ey + ez * ez) - SK_GR[e];
			d = d < de ? d : de;
		}
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

// 1 bone, 2 chitin, 3 glow, 4 marking, 5 ruby, 6 moth at a point the march stopped on: the nearest wins, ties to the earlier.
// A capsule the ray passed no nearer than 0.01 to is farther than the march's reach from every point on it, so only the ray's
// candidates are tried (cand[0, nbone) bone, then chitin up to ncap)
function sk_material(float x, float y, float z, array<int>& cand, int nbone, int ncap) -> int {
	float best = 99.0;
	int m = 0;
	float bone = sk_candidates(x, y, z, cand, 0, nbone, sk_skull(x, y, z, best));
	if (bone < best) { best = bone; m = 1; }
	float db = sk_body(x, y, z);
	float chit = sk_candidates(x, y, z, cand, nbone, ncap, db < best ? db : best);
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

// where the ray (origin o, unit direction d) is inside the sphere (c, r): widens [t_near, t_far] to hold that stretch
function sk_ray_span(float ox, float oy, float oz, float dx, float dy, float dz, float cx, float cy, float cz, float r, float& t_near, float& t_far) -> void {
	float vx = cx - ox;
	float vy = cy - oy;
	float vz = cz - oz;
	float tc = vx * dx + vy * dy + vz * dz;
	float h = r * r - (vx * vx + vy * vy + vz * vz - tc * tc);
	if (h <= 0.0) { return; }
	h = math::sqrt(h);
	t_near = tc - h < t_near ? tc - h : t_near;
	t_far = tc + h > t_far ? tc + h : t_far;
}

// a pixel's palette index plus its depth (0..254) above bit 10, for the silk and the embers drawn over it, or -1 where the ray
// meets neither the creature nor the moth; row: the row's candidate parts (see sk_row)
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
	if (groups == 0 && nc == 0) { return -1; }
	float t = 99.0;
	float t_far = 0.0;
	if ((groups & 3) != 0 || nc != 0) { sk_ray_span(ox, oy, oz, dx, dy, dz, SK_BCX, SK_BCY, SK_BCZ, SK_BR, t, t_far); }
	if ((groups & 4) != 0) { sk_ray_span(ox, oy, oz, dx, dy, dz, SK_MX, SK_MY, SK_MZ, SK_MB, t, t_far); }
	t = t < 0.04 ? 0.04 : t;
	bool hit = false;
	int i = 0;
	while (i < 48 && t < t_far) {
		float d = sk_spider(ox + dx * t, oy + dy * t, oz + dz * t, groups, cand, nc);
		float reach = 0.001 + t * 0.0015;
		if (d < (reach < 0.0085 ? reach : 0.0085)) {
			hit = true;
			break;
		}
		t += d * 0.85;
		i = i + 1;
	}
	if (!hit) { return -1; }
	float dith = SK_BAYER[(py % 4) * 4 + px % 4] / 16.0 - 0.47;
	int depth = itrunc(t * 18.0);
	depth = (depth > 254 ? 254 : depth) * 1024;
	float x = ox + dx * t;
	float y = oy + dy * t;
	float z = oz + dz * t;
	int m = sk_material(x, y, z, cand, nbone, ncap);
	float fog = 1.0 - t / 14.0;
	fog = fog < 0.0 ? 0.0 : fog;
	float lx = SK_LX - x;
	float ly = SK_LY - y;
	float lz = SK_LZ - z;
	float ld2 = lx * lx + ly * ly + lz * lz;
	float lil = 1.0 / math::sqrt(ld2 + 0.0001);
	lx *= lil; ly *= lil; lz *= lil;
	if (m == 3) {
		int e = itrunc((5.4 + SK_FLICK) * SK_WAKE + dith);
		for (int k = 0; k < SK_NSOCK; ++k) {
			float ex = x - SK_GX[k]; float ey = y - SK_GY[k]; float ez = z - SK_GZ[k];
			float el = math::sqrt(ex * ex + ey * ey + ez * ez);
			if (el < SK_GR[k] + 0.012) {
				// an eye burns red at its rim and yellow where it faces the viewer, with a
				// wet glint where its sphere mirrors the key light
				float facing = -(ex * dx + ey * dy + ez * dz) / (el + 0.0000001);
				e = itrunc((2.6 + 3.2 * facing * facing + SK_FLICK) * SK_WAKE + dith);
				float hx = SK_KX - dx; float hy = SK_KY - dy; float hz = SK_KZ - dz;
				float hl = math::sqrt(hx * hx + hy * hy + hz * hz) * el + 0.0000001;
				if ((ex * hx + ey * hy + ez * hz) / hl > 0.93 && SK_WAKE > 0.5) { e = 7; }
			}
		}
		return SK_EYE + (e < 0 ? 0 : (e > 7 ? 7 : e)) + depth;
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

// the vault behind the web at every fourth traced pixel, as light and the creature's warm aura: a ribbed dome about the hub
// with a pale oculus at its crown, a ring of pillars nearer the eye, mist drifting across, a faint ring leaving on each kick
function sk_backdrop(float dim, float flash) -> void {
	int cols = SK_W / 4 + 2;
	int rows = SK_H / 4 + 2;
	SK_BACK_COLS = cols;
	SK_BACK_LIGHT.resize(cols * rows, 0.0);
	SK_BACK_WARM.resize(cols * rows, 0.0);
	float zoom = SK_ZOOM / (SK_H * 0.5);
	float ee = SK_EX * SK_EX + SK_EY * SK_EY + SK_EZ * SK_EZ;
	float exz = SK_EX * SK_EX + SK_EZ * SK_EZ - 21.16;
	float vx = SK_BCX - SK_EX;
	float vy = SK_BCY - SK_EY;
	float vz = SK_BCZ - SK_EZ;
	float vv = vx * vx + vy * vy + vz * vz;
	float ring = SK_KAGE * 1.8;
	float ring_gain = SK_KAGE < 0.7 ? (0.7 - SK_KAGE) * 0.16 * SK_WAKE : 0.0;
	float aura_gain = SK_WAKE * (0.75 + 0.25 * SK_KICK);
	for (int j = 0; j < rows; ++j) {
		float v = (SK_H * 0.5 - j * 4 - 0.5) * zoom;
		for (int i = 0; i < cols; ++i) {
			float u = (i * 4 + 0.5 - SK_W * 0.5) * zoom;
			float dx = SK_FX + SK_RX * u + SK_UX * v;
			float dy = SK_FY + SK_RY * u + SK_UY * v;
			float dz = SK_FZ + SK_RZ * u + SK_UZ * v;
			float il = 1.0 / math::sqrt(dx * dx + dy * dy + dz * dz);
			dx *= il; dy *= il; dz *= il;
			float ed = SK_EX * dx + SK_EY * dy + SK_EZ * dz;
			float t = (math::sqrt(ed * ed - ee + 49.0) - ed) / 7.0;
			float qx = SK_EX / 7.0 + dx * t;
			float qy = SK_EY / 7.0 + dy * t;
			float qz = SK_EZ / 7.0 + dz * t;
			float phi = math::atan2(qx, -qz);
			float rib = math::abs(math::sin(phi * 4.0));
			rib = rib < 0.25 ? 1.0 - rib * 4.0 : 0.0;
			float light = 0.2 + 0.14 * qy + rib * (0.06 + 0.06 * qy);
			float oculus = (qy - 0.8) * 6.0;
			light += (oculus < 0.0 ? 0.0 : (oculus > 1.0 ? 1.0 : oculus)) * 0.3;
			light += 0.05 * math::sin(phi * 3.0 + qy * 4.0 + SK_T * 0.25) * math::sin(qy * 6.0 - SK_T * 0.17 + phi);
			float off = math::abs(math::sqrt(qx * qx + qy * qy) - ring);
			if (off < 0.12) { light += (1.0 - off / 0.12) * ring_gain; }
			float a = dx * dx + dz * dz;
			float b = SK_EX * dx + SK_EZ * dz;
			float disc = b * b - a * exz;
			if (disc > 0.0 && a > 0.000001) {
				float tp = (math::sqrt(disc) - b) / a;
				float gap = math::abs(math::cos(math::atan2(SK_EX + dx * tp, -SK_EZ - dz * tp) * 4.0));
				if (gap < 0.5) { light *= 0.45 + gap * 1.1; }
			}
			light = light * dim + flash;
			float tc = vx * dx + vy * dy + vz * dz;
			float perp = vv - tc * tc;
			float warm = 1.3 - math::sqrt(perp < 0.0 ? 0.0 : perp);
			warm = warm < 0.0 ? 0.0 : (warm > 1.0 ? 1.0 : warm);
			SK_BACK_LIGHT[j * cols + i] = light < 0.0 ? 0.0 : (light > 1.0 ? 1.0 : light);
			SK_BACK_WARM[j * cols + i] = warm * warm * aura_gain;
		}
	}
}

// one traced row of the vault, from the grid sk_backdrop fills
function sk_backdrop_row(int y, array<int>& out) -> void {
	int j = y / 4;
	float fy = (y - j * 4) * 0.25;
	int upper = j * SK_BACK_COLS;
	int lower = upper + SK_BACK_COLS;
	int bayer = (y % 4) * 4;
	int base = SK_BACK + SK_BACK_HUE + 255 * 1024;
	int ww = SK_W;
	int x = 0;
	int i = 0;
	while (x < ww) {
		float l0 = SK_BACK_LIGHT[upper + i];
		float l1 = SK_BACK_LIGHT[upper + i + 1];
		float w0 = SK_BACK_WARM[upper + i];
		float w1 = SK_BACK_WARM[upper + i + 1];
		l0 += (SK_BACK_LIGHT[lower + i] - l0) * fy;
		l1 += (SK_BACK_LIGHT[lower + i + 1] - l1) * fy;
		w0 += (SK_BACK_WARM[lower + i] - w0) * fy;
		w1 += (SK_BACK_WARM[lower + i + 1] - w1) * fy;
		for (int k = 0; k < 4 && x < ww; ++k) {
			float dith = SK_BAYER[bayer + k] / 16.0;
			int level = itrunc((l0 + (l1 - l0) * k * 0.25) * 15.0 + dith);
			int warm = itrunc((w0 + (w1 - w0) * k * 0.25) * 4.0 + dith);
			out[x] = base + (warm > 4 ? 4 : warm) * 16 + (level > 15 ? 15 : level);
			x = x + 1;
		}
		i = i + 1;
	}
}

// the pixels [first, last) of a row whose rays (A + R u, A = F + U v) pass within r of c: widens the span to hold them
function sk_row_span(float ax, float ay, float az, float cx, float cy, float cz, float r, float zoom, int& first, int& last) -> void {
	float vx = cx - SK_EX;
	float vy = cy - SK_EY;
	float vz = cz - SK_EZ;
	float q = vx * vx + vy * vy + vz * vz - r * r;
	float a = vx * ax + vy * ay + vz * az;
	float b = vx * SK_RX + vy * SK_RY + vz * SK_RZ;
	float k = q - b * b;
	if (k < 0.0001) {
		first = 0;
		last = SK_W;
		return;
	}
	float disc = a * a * b * b - k * (q * (ax * ax + ay * ay + az * az) - a * a);
	if (disc <= 0.0 || a <= 0.0) { return; }
	disc = math::sqrt(disc);
	int lo = itrunc(math::floor((a * b - disc) / (k * zoom) + SK_W * 0.5)) - 1;
	int hi = itrunc(math::floor((a * b + disc) / (k * zoom) + SK_W * 0.5)) + 2;
	first = lo < first ? lo : first;
	last = hi > last ? hi : last;
}

function sk_row(int y) -> array<int> {
	array<int> out = [];
	out.resize(SK_W, 0);
	sk_backdrop_row(y, out);
	int ww = SK_W;
	float half_h = SK_H * 0.5;
	float zoom = SK_ZOOM / half_h;
	float v = (half_h - y - 0.5) * zoom;
	float ax = SK_FX + SK_UX * v;
	float ay = SK_FY + SK_UY * v;
	float az = SK_FZ + SK_UZ * v;
	int first = ww;
	int last = 0;
	sk_row_span(ax, ay, az, SK_BCX, SK_BCY, SK_BCZ, SK_BR, zoom, first, last);
	if (SK_MOTH) { sk_row_span(ax, ay, az, SK_MX, SK_MY, SK_MZ, SK_MB, zoom, first, last); }
	first = first < 0 ? 0 : first;
	last = last > ww ? ww : last;
	if (first >= last) { return out; }
	// the row's rays all lie in the plane through the eye spanned by R and F + U v: a capsule (0..SK_CROWN0) or a leg glow
	// (100 + g) farther from that plane than its reach passes near none of them, so the pixels test only the rest
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
	array<int> cand = [];
	cand.resize(64, 0);
	for (int x = first; x < last; ++x) {
		float u = (x + 0.5 - ww * 0.5) * zoom;
		float dx = ax + SK_RX * u;
		float dy = ay + SK_RY * u;
		float dz = az + SK_RZ * u;
		float il = 1.0 / math::sqrt(dx * dx + dy * dy + dz * dz);
		int traced = sk_trace(dx * il, dy * il, dz * il, x, y, cand, row, nrow);
		if (traced >= 0) { out[x] = traced; }
	}
	return out;
}

class Ossuary : Scene {
	Ossuary() { pixel_budget = 16000; }

	float bx = 0.0; float by = 0.0; float bz = 0.0;
	float fx = 0.0; float fy = 1.0; float fz = 0.0;
	float ux = 0.0; float uy = 0.0; float uz = 1.0;
	float rx = 1.0; float ry = 0.0; float rz = 0.0;
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
	array<float> leglift = [1.25, 0.8, 0.25, 0.0];
	array<float> ppx = [];
	array<float> ppy = [];
	array<float> ppz = [];
	array<float> pvx = [];
	array<float> pvy = [];
	array<float> pvz = [];
	array<float> plife = [];
	array<float> prate = [];
	array<int> depth = [];
	int np = 120;
	int pnext = 0;
	float l1 = 0.5;
	float l2 = 0.52;
	float tt = 0.0;
	float spawn_acc = 0.0;
	float stance = 0.5;

	// the story on the cue's bars: asleep at the hub (0-1), awake on bar 2, the spokes thrown and the spiral run by bar 4,
	// the moth strikes on bar 6, the cocoon closes on bar 8, the hunter stalks the viewer through bar 9 and lunges on bar 10
	float t_wake = 3.871;
	float t_spokes = 4.355;
	float t_spoke_gap = 0.0806;
	float t_out = 5.5;
	float t_spiral0 = 5.806;
	float t_spiral1 = 7.742;
	float t_home = 8.25;
	float t_moth = 8.7;
	float t_impact = 11.613;
	float t_rush = 11.85;
	float t_reach = 12.5;
	float t_turn = 12.85;
	float t_wrap0 = 12.95;
	float t_wrap1 = 15.484;
	float t_face = 15.7;
	float t_stalk = 17.419;
	float t_lunge = 19.355;
	int nspoke = 14;
	float spoke_reach = 4.4;
	float web_rim = 1.55;
	float lay_from = 2.24;
	float lay_lead = 0.7;
	float ring_speed = 3.0;
	array<float> spoke_ang = [];
	array<float> spa = [];
	array<float> spb = [];
	array<float> lay_turn = [];
	float pa = 0.0; float pb = 0.0; float psi = 1.5707963;
	float spd = 0.0;
	float moth_a = 1.2; float moth_b = 0.7;
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
		replay = 23.5;
		// the vault: three chord hues, each five steps of the creature's warm aura by sixteen levels of light
		SK_BACK = pal.size();
		array<float> vault = [52.0, 60.0, 112.0, 72.0, 52.0, 110.0, 40.0, 72.0, 104.0];
		for (int hue = 0; hue < 3; ++hue) {
			for (int warm = 0; warm < 5; ++warm) {
				for (int level = 0; level < 16; ++level) {
					float f = level / 15.0;
					pal.add(4.0 + vault[hue * 3] * f + warm * 16.0, 4.0 + vault[hue * 3 + 1] * f + warm * 2.0, 9.0 + vault[hue * 3 + 2] * f + warm * 4.0);
				}
			}
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
		// the eyes and the crown: three steps of dead ember, four of fire, then white
		SK_EYE = pal.size();
		pal.add(34.0, 12.0, 10.0);
		pal.add(96.0, 16.0, 8.0);
		pal.add(160.0, 22.0, 8.0);
		pal.ramp(4, 220.0, 30.0, 10.0, 255.0, 250.0, 190.0);
		pal.add(255.0, 255.0, 255.0);
		SK_MARK = pal.ramp(6, 90.0, 0.0, 8.0, 255.0, 200.0, 60.0);
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
		plan_web();
	}

	int length_bars() override { return 12; }

	array<float> hit_times() override { return [3.871, 7.742, 11.613, 15.484, 19.355]; }
	array<int> hit_levels() override { return [1, 1, 2, 1, 2]; }

	// 0 until the hunter turns from the cocoon to the viewer, then 1
	float facing(float t) { return smooth((t - t_face) / 1.2); }
	// 0 before the stalk, rising through bar 9 to 1 at the lunge
	float stalk(float t) { return smooth((t - t_stalk) / (t_lunge - t_stalk)); }
	// the lunge itself, 1 from just after bar 10's downbeat on
	float lunge(float t) { return smooth((t - t_lunge + 0.06) / 0.22); }
	// the flare of the waking: 1 on bar 2's downbeat, dying away
	float flare(float t) { return t < t_wake ? 0.0 : math::exp((t_wake - t) * 2.5); }
	// how wide the jaw gapes: a yawn on waking, the lunge, and a snap on every kick of the close-up
	float gape(float t) {
		float result = 0.6 * flare(t);
		float s = t - t_lunge;
		if (s > -0.1) { result = math::exp(0.0 - (s > 0.0 ? s : 0.0) * 2.2) + 0.35 * SK_KICK; }
		return result > 1.2 ? 1.2 : result;
	}

	// the music the scene answers: the last kick's age and envelope, the chord's hue in the vault, the hits in the lens
	void hear(float t) {
		if (cue == null) { return; }
		int k = cue.kick.last(t / MUSIC_STEP + 0.0001);
		SK_KAGE = k < 0 ? 9.0 : t - cue.kick.at[k] * MUSIC_STEP;
		SK_KICK = cue.kick_env(t);
		SK_BACK_HUE = cue.chord(t) % 3 * 80;
		float punch = cue.hit(t);
		SK_PUNCH = punch * punch;
		SK_ZOOM = 0.5 * (1.0 - SK_PUNCH * 0.12 - SK_KICK * 0.015 * SK_WAKE);
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

	// where a foot wants to stand: on the web about the body, the front pairs raised off it at the viewer in the stalk and the leap
	void rest_foot(int i, float lead) {
		float side = i < 4 ? 1.0 : -1.0;
		int j = i % 4;
		float reach = (legreach[j] + lead) * stance;
		float span = side * 0.78 * stance;
		tox[i] = bx + fx * reach + rx * span;
		toy[i] = by + fy * reach + ry * span;
		toz[i] = bz * leglift[j] + stalk(tt) * (j == 0 ? 0.3 : 0.0);
	}

	float jitter(int n) { return (rng.next(n * 2 + 1) - n) / (n * 1.0); }

	// the web stands in the plane z = 0 about the origin, facing +z: the spokes get their angles and the capture spiral its
	// polygon, one vertex where it crosses each spoke, winding in from the rim; lay_turn is how far round from lay_from,
	// in turns, the edge into each vertex begins
	void plan_web() {
		for (int k = 0; k < nspoke; ++k) {
			float jit = k == nspoke / 2 ? 0.0 : jitter(100) * 0.09;
			spoke_ang.push(-1.5707963 + k * 6.2831853 / nspoke + jit);
		}
		int n = 4 * nspoke + nspoke / 2 + 1;
		for (int i = 0; i < n; ++i) {
			float r = web_rim - (web_rim - 0.5) * i / (n - 1.0);
			float a = spoke_ang[i % nspoke] + 6.2831853 * (i / nspoke);
			spa.push(r * math::cos(a));
			spb.push(r * math::sin(a));
			float turn = (spoke_ang[(i + nspoke - 1) % nspoke] - lay_from) / 6.2831853;
			lay_turn.push(turn - math::floor(turn));
		}
	}

	// how far round the web the spiral is laid at time t, in turns: every winding grows at once behind the running
	// spider, and the lap closes, a spoke past full circle, on bar 4
	float laid(float t) { return (t - t_spiral0) / (t_spiral1 - t_spiral0) * (1.0 + 1.0 / nspoke); }

	// the spiral's run at time t, into qa, qb: out from the hub, round with the spinnerets trailing over the laid edge, home
	void lap_point(float t) {
		float r = smooth((t - t_out) / 0.5) * (1.0 - smooth((t - t_spiral1) / (t_home - t_spiral1)));
		float a = lay_from + lay_lead + 6.2831853 * laid(t);
		qa = r * math::cos(a);
		qb = r * math::sin(a);
	}

	float lap_heading(float t) {
		lap_point(t - 0.04);
		float a = qa;
		float b = qb;
		lap_point(t + 0.04);
		return math::atan2(qb - b, qa - a);
	}

	// where it is on the web (pa, pb) and which way it faces (psi), at time t
	void plane_path(float t) {
		if (t < t_out) {
			pa = 0.0; pb = 0.0;
			psi = 1.5707963;
			return;
		}
		if (t < t_home) {
			psi = turn_toward(1.5707963, lap_heading(t), smooth((t - t_out) / 0.15));
			lap_point(t);
			pa = qa; pb = qb;
			return;
		}
		float ml = math::sqrt(moth_a * moth_a + moth_b * moth_b);
		float da = moth_a / ml; float db = moth_b / ml;
		float toward = math::atan2(db, da);
		if (t < t_rush) {
			pa = 0.0; pb = 0.0;
			psi = turn_toward(lap_heading(t_home - 0.05), 1.5707963, smooth((t - t_home) / 0.6));
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
			// it works the cocoon, then turns head-down and prowls across the hub at the viewer below
			float busy = 1.0 - smooth((t - t_wrap1 + 0.3) / 0.3);
			float prowl = smooth((t - t_face - 0.3) / (t_lunge - t_face - 0.3));
			reach = reach - 0.11 + 0.05 * math::sin(t * 8.0) * busy;
			psi = turn_toward(toward + 3.1415927 + 0.08 * math::sin(t * 5.0) * busy, -1.5707963, facing(t));
			pa = da * reach * (1.0 - prowl);
			pb = db * reach * (1.0 - prowl) - 0.35 * prowl;
			return;
		}
		pa = da * reach; pb = db * reach;
	}

	void web_motion(float t, float dt) {
		float opa = pa; float opb = pb;
		plane_path(t);
		fx = math::cos(psi); fy = math::sin(psi); fz = 0.0;
		rx = -fy; ry = fx; rz = 0.0;
		bx = pa; by = pb; bz = 0.0;
		float ma = pa - opa; float mb = pb - opb;
		spd = dt > 0.0 ? math::sqrt(ma * ma + mb * mb) / dt : 0.0;
	}

	// how bright the strike's ring makes the silk at (a, b): a front running out from the moth through the threads
	float ring_light(float a, float b) {
		float s = tt - t_impact;
		if (s < 0.0 || s > 1.6) { return 0.0; }
		float ma = a - moth_a; float mb = b - moth_b;
		float behind = s * ring_speed - math::sqrt(ma * ma + mb * mb);
		if (behind < 0.0 || behind > 0.7) { return 0.0; }
		return (1.0 - behind / 0.7) * 1.3 / (1.0 + s * 1.2);
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
			float front = s * ring_speed - dist;
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
		wx = a;
		wy = b;
		wz = sag(a, b);
	}

	float bezier(float from, float via, float to, float u) {
		return from * (1.0 - u) * (1.0 - u) + via * 2.0 * u * (1.0 - u) + to * u * u;
	}

	// the moth at time t: it flutters in along a curve from off the frame's left, strikes the web and sticks, beats its
	// wings against the silk, then folds them as the cocoon closes and spins
	void moth_pose(float t) {
		if (t < t_impact) {
			float u = (t - t_moth) / (t_impact - t_moth);
			u = u < 0.0 ? 0.0 : u;
			float a = bezier(-2.7, -0.9, moth_a, u);
			float b = bezier(0.6, 1.15, moth_b, u);
			float n = bezier(1.4, 0.9, 0.06, u);
			float va = bezier(-2.7, -0.9, moth_a, u + 0.02) - a;
			float vb = bezier(0.6, 1.15, moth_b, u + 0.02) - b;
			float vn = bezier(1.4, 0.9, 0.06, u + 0.02) - n;
			float flutter = 1.0 - u;
			mox = a + flutter * 0.16 * math::sin(t * 6.3);
			moy = b + flutter * 0.18 * math::sin(t * 8.9 + 1.0);
			moz = n + flutter * 0.14 * math::sin(t * 5.2 + 2.0);
			float vl = math::sqrt(va * va + vb * vb + vn * vn) + 0.000001;
			mfx = va / vl; mfy = vb / vl + 0.5; mfz = vn / vl;
			flap = 0.95 * math::sin(t * 31.4);
			wsc = 1.0; coc = 0.0; spin = 0.0;
		} else {
			web_point(moth_a, moth_b);
			mox = wx; moy = wy; moz = wz + 0.09;
			mfx = 0.34; mfy = 0.94; mfz = 0.0;
			coc = smooth((t - t_wrap0) / (t_wrap1 - 0.15 - t_wrap0));
			spin = t > t_wrap0 ? ((t < t_wrap1 ? t : t_wrap1) - t_wrap0) * 6.0 : 0.0;
			float beat = math::sin(t * 15.0);
			flap = (1.0 - coc) * 0.55 * (beat > 0.0 ? beat : 0.0) * (0.6 + 0.4 * math::sin(t * 2.3)) + coc * 1.3;
			wsc = 1.0 - 0.7 * coc;
		}
		float ml = 1.0 / math::sqrt(mfx * mfx + mfy * mfy + mfz * mfz);
		mfx *= ml; mfy *= ml; mfz *= ml;
		mux = 0.0 - mfx * mfz; muy = 0.0 - mfy * mfz; muz = 1.0 - mfz * mfz;
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

	// an ember off the ring of the crown, thrown outward at the given speed
	void crown_ember(float speed) {
		float a = rng.next(628) * 0.01;
		float ca = math::cos(a) * 0.2;
		float sa = math::sin(a) * 0.2;
		float cx = bx + fx * (0.01 + ca * 0.66) + rx * sa;
		float cy = by + fy * (0.01 + ca * 0.66) + ry * sa;
		spawn(cx, cy, bz + 0.6 + ca * 0.75, (fx * ca * 0.66 + rx * sa) * speed, 0.5 + (fy * ca * 0.66 + ry * sa) * speed, 0.3 + ca * 0.75 * speed, 0.45 + rng.next(40) * 0.01);
	}

	bool reached(float mark, float dt) { return tt >= mark && tt - dt < mark; }

	// embers leave the crown once it burns, more on the kick and many when the jaw gapes; the waking and the lunge throw
	// a burst of them, the strike a puff of scales off the moth
	void update_particles(float dt) {
		spawn_acc += dt * 5.0 * SK_WAKE * (0.3 + SK_KICK + gape(tt) * 8.0);
		while (spawn_acc >= 1.0) {
			spawn_acc -= 1.0;
			crown_ember(1.0);
		}
		if (reached(t_wake, dt) || reached(t_lunge, dt)) {
			for (int k = 0; k < 26; ++k) { crown_ember(5.0); }
		}
		if (reached(t_impact, dt)) {
			for (int k = 0; k < 22; ++k) {
				float a = rng.next(628) * 0.01;
				float speed = 0.5 + rng.next(100) * 0.012;
				spawn(moth_a, moth_b, 0.12, math::cos(a) * speed, math::sin(a) * speed, 0.3 + jitter(100) * 0.3, 0.5 + rng.next(50) * 0.01);
			}
		}
		float drag = 1.0 - dt * 1.4;
		for (int i = 0; i < np; ++i) {
			if (plife[i] <= 0.0) { continue; }
			pvx[i] = pvx[i] * drag;
			pvy[i] = (pvy[i] + 0.7 * dt) * drag;
			pvz[i] = pvz[i] * drag;
			ppx[i] = ppx[i] + pvx[i] * dt;
			ppy[i] = ppy[i] + pvy[i] * dt;
			ppz[i] = ppz[i] + pvz[i] * dt;
			plife[i] = plife[i] - dt * prate[i];
		}
	}

	void update(float t, float dt) override {
		tt = t;
		SK_T = t;
		SK_WAKE = smooth((t - t_wake + 0.05) / 0.35);
		hear(t);
		stance = 0.5 + 0.5 * smooth((t - t_wake) / 0.3);
		web_motion(t, dt);
		float spring = lunge(t);
		if (spring > 0.0 && placed) {
			float lx = ex - bx; float ly = ey - by; float lz = ez - bz;
			float ll = 0.75 * spring / math::sqrt(lx * lx + ly * ly + lz * lz + 0.000001);
			bx += lx * ll; by += ly * ll; bz += lz * ll;
		}
		if (!placed) {
			for (int i = 0; i < 8; ++i) {
				rest_foot(i, 0.0);
				footx[i] = tox[i]; footy[i] = toy[i]; footz[i] = toz[i];
			}
		}
		walk(dt);
		update_particles(dt);
		if (t > t_moth) { moth_pose(t); }
		aim_camera(t, dt);
		placed = true;
	}

	// the tetrapod gait: a foot steps when its rest spot has run off, one group at a time; at speed it scuttles, with
	// quicker steps that lead the body and both groups free
	void walk(float dt) {
		bool scuttle = spd > 1.5;
		float step_time = scuttle ? 0.08 : 0.13;
		float lead = spd * 0.1;
		lead = lead > 0.45 ? 0.45 : lead;
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
					footx[i] = fromx[i] + (tox[i] - fromx[i]) * e;
					footy[i] = fromy[i] + (toy[i] - fromy[i]) * e;
					footz[i] = fromz[i] + (toz[i] - fromz[i]) * e + math::sin(e * 3.14159265) * 0.16;
				}
			} else {
				rest_foot(i, 0.0);
				float ddx = footx[i] - tox[i];
				float ddy = footy[i] - toy[i];
				float ddz = footz[i] - toz[i];
				bool other_moving = group_a ? stepping_b : stepping_a;
				if ((scuttle || !other_moving) && ddx * ddx + ddy * ddy + ddz * ddz > 0.05) {
					stept[i] = 0.0;
					fromx[i] = footx[i]; fromy[i] = footy[i]; fromz[i] = footz[i];
					if (group_a) { stepping_a = true; } else { stepping_b = true; }
				}
			}
		}
	}

	// the camera: close on the sleeper, back for the whole web while it is thrown and the moth flies in, in on the hunter
	// and its catch, then round in front of the skull, below the line of its gaze (the face is reared 0.85 radians off
	// the body, the camera stands 0.51 up); the lunge lifts the aim up the face, which keeps the crown in the frame
	void aim_camera(float t, float dt) {
		float hx = bx; float hy = by; float hz = bz + 0.2;
		float back = smooth((t - t_wake - 0.3) / 0.9);
		float near = smooth((t - t_impact - 0.25) / (t_reach - t_impact - 0.25)) * (1.0 - facing(t));
		float creep = stalk(t);
		float onto = facing(t) * 0.6 + creep * 0.4;
		float tgx = hx * (1.0 - 0.75 * back);
		float tgy = hy * (1.0 - 0.75 * back);
		float tgz = hz * (1.0 - back);
		if (t > t_moth) {
			tgx += ((hx + mox) * 0.5 - tgx) * near;
			tgy += ((hy + moy) * 0.5 - tgy) * near;
			tgz += ((hz + moz) * 0.5 - tgz) * near;
		}
		tgx += (hx + fx * 0.34 - tgx) * onto;
		tgy += (hy + fy * 0.34 - tgy) * onto;
		tgz += (hz - tgz) * onto;
		float rise = 0.12 * lunge(t);
		tgx -= fx * 0.7513 * rise;
		tgy -= fy * 0.7513 * rise;
		tgz += 0.66 * rise;
		float dist = 2.3 - 0.3 * (t < t_wake ? t / t_wake : 1.0);
		dist += (3.5 - dist) * back;
		dist += (2.1 - dist) * near;
		dist += (2.75 - dist) * facing(t);
		dist = dist - creep * 0.53 - lunge(t) * 0.53;
		float oa = 0.1 + 0.12 * math::sin(t * 0.19);
		float dx = math::sin(oa);
		float dy = 0.12 + 0.05 * math::sin(t * 0.31);
		float dz = math::cos(oa);
		float dn = math::sqrt(dx * dx + dy * dy + dz * dz);
		dx += (fx * 0.8712 * dn - dx) * onto;
		dy += (fy * 0.8712 * dn - dy) * onto;
		dz += (0.4908 * dn - dz) * onto;
		float dl = dist / math::sqrt(dx * dx + dy * dy + dz * dz);
		float kc = placed ? dt * (2.8 + 9.0 * lunge(t) * (1.0 - smooth((t - t_lunge - 0.5) / 0.5))) : 1.0;
		kc = kc > 1.0 ? 1.0 : kc;
		ex += (tgx + dx * dl - ex) * kc; ey += (tgy + dy * dl - ey) * kc; ez += (tgz + dz * dl - ez) * kc;
		float kl = placed ? dt * 4.0 : 1.0;
		kl = kl > 1.0 ? 1.0 : kl;
		lkx += (tgx - lkx) * kl; lky += (tgy - lky) * kl; lkz += (tgz - lkz) * kl;
		float ku = placed ? dt * 2.2 : 1.0;
		ku = ku > 1.0 ? 1.0 : ku;
		cux += (0.0 - fx * 0.7513 * onto - cux) * ku;
		cuy += (1.0 - onto - fy * 0.7513 * onto - cuy) * ku;
		cuz += (0.66 * onto - cuz) * ku;
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
		// the rays trace on a grid coarser than the picture's and the trace scales up to it, coarser still where the
		// creature fills the frame and every ray is dear; the silk and the embers draw over it at full size
		float glare = facing(tt) * 0.3 + stalk(tt) * 0.7;
		float close = 1.0 - smooth((tt - t_wake - 0.3) / 0.9);
		float caught = smooth((tt - t_impact - 0.25) / 0.5);
		close = close > caught ? close : caught;
		float trace_k = math::sqrt((6400.0 - close * 1800.0) * (1.0 - SK_PUNCH * 0.25) / (w * h));
		trace_k = trace_k > 1.0 ? 1.0 : trace_k;
		int tw = itrunc(w * trace_k);
		int th = itrunc(h * trace_k);
		SK_W = tw;
		SK_H = th;
		SK_FLICK = 0.7 * math::sin(tt * 17.0) * math::sin(tt * 5.3) + 1.5 * flare(tt);
		if (tt > t_wrap1) { SK_FLICK = SK_FLICK * 0.4 + 0.9 + glare * 1.4; }
		float bob = 0.2 + 0.012 * math::sin(tt * 1.6) * (1.0 - SK_WAKE) + 0.018 * math::sin(tt * 11.0) * SK_WAKE;
		float cx = bx + ux * bob;
		float cy = by + uy * bob;
		float cz = bz + uz * bob;
		// the sleeper's skull hangs level with its body; awake it rears back to face the viewer
		float pitch = 0.3 + 0.55 * smooth((tt - t_wake) / 0.4);
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
		float eye = 0.03 * (1.0 + glare * 0.6);
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
		float strike = tt > t_impact ? math::exp((t_impact - tt) * 5.0) : 0.0;
		float leap = tt > t_lunge ? math::exp((t_lunge - tt) * 4.0) : 0.0;
		sk_backdrop((0.55 + 0.45 * SK_WAKE) * (1.0 - 0.5 * glare), 0.1 * flare(tt) + 0.14 * strike + 0.16 * leap);
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
		draw_web(grid);
		draw_particles(grid);
		strike_shake(grid);
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
		SK_MOTH = tt > t_moth;
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
		grid[at] = SK_EYE + 7;
	}

	// the silk, drawn over the trace: the dragline the sleeper hangs from and the spokes cast after it, a spark at each
	// growing tip; the hub's mesh; the capture spiral, every winding growing at once behind the running spider on a fan
	// of fresh thread from its spinnerets; the strands that wind the cocoon. The strike's ring lights the threads it
	// passes, and the whole web flashes with dew as the spiral closes and again as the cocoon is sealed
	void draw_web(var& grid) {
		float shimmer = tt >= t_spiral1 ? math::exp((t_spiral1 - tt) * 4.0) : 0.0;
		float sealed = tt - t_wrap1;
		if (sealed >= 0.0) { shimmer = math::exp(0.0 - sealed * 5.0); }
		for (int k = 0; k < nspoke; ++k) {
			int order = k < nspoke / 2 ? k : k - 1;
			float grow = k == nspoke / 2 ? 1.0 : (tt - t_spokes - order * t_spoke_gap) / 0.25;
			if (grow <= 0.0) { continue; }
			grow = grow > 1.0 ? 1.0 : grow;
			float ca = math::cos(spoke_ang[k]) * spoke_reach * grow / 10.0;
			float sa = math::sin(spoke_ang[k]) * spoke_reach * grow / 10.0;
			web_point(0.0, 0.0);
			float px = wx; float py = wy; float pz = wz;
			for (int s = 1; s <= 10; ++s) {
				web_point(ca * s, sa * s);
				thread(grid, px, py, pz, wx, wy, wz, 0.78 + shimmer * 0.4 + ring_light(ca * s, sa * s));
				px = wx; py = wy; pz = wz;
			}
			if (grow < 1.0) { glint(grid, px, py, pz); }
		}
		if (tt > t_out) {
			for (int ring = 1; ring <= 2; ++ring) {
				float rr = ring * 0.16;
				web_point(rr, 0.0);
				float px = wx; float py = wy; float pz = wz;
				for (int s = 1; s <= 12; ++s) {
					float a = s * 0.5235988;
					web_point(rr * math::cos(a), rr * math::sin(a));
					thread(grid, px, py, pz, wx, wy, wz, 0.8 + shimmer * 0.4);
					px = wx; py = wy; pz = wz;
				}
			}
		}
		float sx = SK_AX - fx * 0.4; float sy = SK_AY - fy * 0.4; float sz = SK_AZ - fz * 0.4;
		if (tt > t_spiral0) {
			float sweep = laid(tt);
			int last = spa.size() - 1;
			for (int i = 1; i <= last; ++i) {
				float u = (sweep - lay_turn[i]) * nspoke;
				if (u <= 0.0) { continue; }
				web_point(spa[i - 1], spb[i - 1]);
				float px = wx; float py = wy; float pz = wz;
				if (u < 1.0) {
					web_point(spa[i - 1] + (spa[i] - spa[i - 1]) * u, spb[i - 1] + (spb[i] - spb[i - 1]) * u);
					thread(grid, px, py, pz, wx, wy, wz, 1.0);
					thread(grid, wx, wy, wz, sx, sy, sz, 1.2);
					glint(grid, wx, wy, wz);
					continue;
				}
				web_point(spa[i], spb[i]);
				thread(grid, px, py, pz, wx, wy, wz, 0.88 + shimmer * 0.4 + ring_light(spa[i], spb[i]));
				if (math::sin(tt * 1.3 + i * 1.7) > 0.93 - shimmer * 0.9) { glint(grid, wx, wy, wz); }
			}
		}
		if (tt > t_wrap0 && tt < t_wrap1) {
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
		if (sealed >= 0.0 && sealed < 0.4) {
			for (int k = 0; k < 12; ++k) {
				float a = k * 0.5235988 + 0.2;
				float r = 0.24 + sealed * 1.3;
				glint(grid, mox + math::cos(a) * r, moy + math::sin(a) * r, moz + 0.05);
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
		if (SK_WAKE < 0.5) { return; }
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
