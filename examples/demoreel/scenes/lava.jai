// LAVALAMP — a forest of lava lamps, raytraced in 3D, played to the music. The
// lamps stand on a dark floor in an endless grid, each a surface of revolution:
// a steel cone base, a tapered glass vessel and a steel cap. A camera flies them on
// a spline whose marks fall on bar lines: out of one lamp's wax, up into a crane
// over the whole field on the first big hit, low between the rows, then a rolling
// swoop that dives into another lamp's wax on the second. Each ray is clipped to the
// slab of air the lamps stand in and walks the grid cell by cell inside it (a 2D
// DDA), measuring how near it passes each cell's lamp: near lamps solve their
// profile exactly (seven cone frustums, only those the ray's height range reaches),
// far ones take the profile's width where the ray passes the axis. Glass carries a
// rim and a graded glint in the lamp's own colour and bends the ray a little;
// inside, the wax is a 3D metaball field (seven blobs rising off the heater and
// sinking as they cool, a pool on the base, a drip under the cap). Along the bent
// ray each blob near it is one quadratic, so the field is a handful of divisions
// per sample: the surface is bracketed from the blobs' own ellipsoids, bisected and
// lit from the field's gradient, the liquid between glowing with it. The floor is
// black glass: it mirrors the lamps (a second walk of the grid along the bounced
// ray) over a pool of each lamp's light, and every lamp blooms into the haze around
// it, measured as the ray passes. Everything fogs with distance. Every lamp takes
// one of three blob simulations, turned to its own quarter, and its own colour
// family. The wax swells with the kick, burns brighter with the section's energy,
// shifts hue with the chord and runs a rainbow wave over the field after the first
// big hit; each big hit sends a ring of white heat racing out across the field, lamp
// by lamp, and the camera sways between the rows on the beat. The close-up holds
// through a riser, the wax swelling; the third big hit blasts the camera back out
// of the wax in a corkscrew up over the field, a lesser ring leaving that lamp on
// every kick under a rainbow wave, then it swoops down into a lane and races along
// it between the lamps. The picture is traced on a lattice of every second pixel
// each way, rows in parallel; a pixel between lattice samples that agree (one ramp,
// close levels, no silhouette passing near) takes their mean, two samples of the
// floor and haze always do, and every other pixel is traced.

array<float> LV_BX = [];     // three blob sets of LV_N blobs, in lamp space
array<float> LV_BY = [];
array<float> LV_BZ = [];
array<float> LV_R2 = [];
array<float> LV_IST2 = [];   // 1 / stretch squared: a blob's speed pulls it into taffy along y
array<float> LV_BLO = [];    // the heights each blob's field reaches between
array<float> LV_BHI = [];
array<float> LV_HUE = [];
array<int> LV_LEVEL = [];    // each palette index's level within its ramp
array<int> LV_LATTICE = [];  // this frame's lattice samples, LV_CW a row
array<int> LV_LATTICE_ROWS = [];
array<int> LV_BLOCK_ROWS = [];
int LV_N = 9;
int LV_W = 0;
int LV_H = 0;
int LV_CW = 0;
int LV_GLOW = 0;
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
float LV_PIXEL = 0.01;       // a pixel's width at distance 1
float LV_GAIN = 1.0;         // how bright the wax burns: energy, kick and hits
float LV_FLASH = 0.0;        // the hits' white
float LV_HUE0 = 0.0;         // the chord's hue shift
float LV_HUE_ROOM = 0.0;     // the hue a lamp's light takes on the floor and the haze, before its family's
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

float LV_FAR = 13.0;         // the fog swallows everything past this
float LV_TOP = 1.15;         // no lamp or bloom reaches above this height
float LV_EXACT = 5.0;        // lamps nearer than this solve their profile exactly
float LV_NEAR = 6.0;         // lamps nearer than this bloom, their wax is searched for its surface and lit, and their pixels are all drawn

// the vessel's radius at height y (lamp units: 0 the base's foot, 1 the cap's top)
function lamp_width(float y) -> float {
	float e = y - 0.42;
	return y < 0.2 ? 0.16 - 0.35 * y : (y < 0.42 ? 0.13 - 0.85 * e * e : (y < 0.84 ? 0.13 - 0.363 * e * e : 0.066 - (y - 0.84) * 0.2));
}
function hue_index(float hue) -> int {
	return (math::itrunc(hue) % 24 + 24) % 24;
}

function lamp_id(int ci, int cj) -> int {
	return ((ci * 7349 + cj * 1931) % 997 + 997) % 997;
}

// the hue of the lamp at (ci, cj), before the liquid's own: its family's, the chord's and the rainbow wave's
function lamp_hue(int ci, int cj) -> float {
	return (lamp_id(ci, cj) % 6) * 4.0 + LV_HUE0 + LV_BURN * ((ci + cj * 2) * 2.0 + LV_T * 9.0);
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

// the first lamp met by the eye's ray along (dx, eye_dy, dz), or with `mirror` by that ray's bounce off the
// black-glass floor (the lamps it mirrors, sunk a way into the glass): the distance to it, plus 32 when a part of a
// lamp too thin for the lattice passed within a pixel and 64 when the lamp stands past LV_NEAR. Below zero the ray
// met none: -1 - (the bloom's level + 16 * its hue + 1024 for the thin part). The lamps stand on whole numbers, so
// the walk steps along the axis the ray mostly follows and tests the one lamp nearest the ray at each step; the
// eye's ray measures the lamps nearer than LV_NEAR for their bloom as it passes.
function first_lamp(float dx, float eye_dy, float dz, bool mirror) -> float {
	float ox = LV_EX;
	float oy = LV_EY;
	float oz = LV_EZ;
	float dy = eye_dy;
	float t_from = 0.0;
	float t_to = 0.0;
	float t_base = 0.0;
	float t_floor = eye_dy < -0.0001 ? (0.0 - LV_EY) / eye_dy : 1000.0;
	if (mirror) {
		dy = 0.0 - eye_dy;
		ox += dx * t_floor;
		oy = 0.0005;
		oz += dz * t_floor;
		float t_cap = LV_TOP / dy;
		t_to = t_cap < 4.0 ? t_cap : 4.0;
		t_base = t_floor + 3.0;
	} else {
		// the lamps stand in a slab of air between the floor and LV_TOP
		t_to = t_floor < LV_FAR ? t_floor : LV_FAR;
		if (oy > LV_TOP) {
			t_from = dy < -0.0001 ? (LV_TOP - oy) / dy : t_to;
		} else if (dy > 0.0001) {
			float t_top = (LV_TOP - oy) / dy;
			t_to = t_top < t_to ? t_top : t_to;
		}
	}
	if (t_from >= t_to) { return -1.0; }
	float a = dx * dx + dz * dz;
	float inv_a = 1.0 / a;
	bool along_x = dx * dx >= dz * dz;
	float om = along_x ? ox : oz;
	float dm = along_x ? dx : dz;
	float slope = (along_x ? dz : dx) / dm;
	float m_from = om + dm * t_from;
	float m_to = om + dm * t_to;
	float step = dm > 0.0 ? 1.0 : -1.0;
	// the cell along the walk's axis, the cell across it and how far the ray passes from that cell's lamp, across
	float cm = dm > 0.0 ? 0.0 - math::floor(0.35 - m_from) : math::floor(m_from + 0.35);
	int count = math::itrunc(dm > 0.0 ? math::floor(m_to + 0.35) - cm : cm + math::floor(0.35 - m_to)) + 1;
	int near_steps = 0;
	if (!mirror) {
		float m_near = om + dm * (LV_NEAR + 0.5);
		near_steps = math::itrunc(dm > 0.0 ? math::floor(m_near) - cm : cm + math::floor(0.0 - m_near)) + 1;
		near_steps = near_steps > count ? count : near_steps;
	}
	float n = (along_x ? oz : ox) + (cm - om) * slope;
	float cn = math::floor(n + 0.5);
	float f = n - cn;
	float f_step = slope * step;
	float across = dm * dm * inv_a;
	// the bloom: how close the ray came to a lamp's glowing body, and which lamp
	float best = 0.0;
	float bound = 0.1225;
	float glow_x = 0.0;
	float glow_z = 0.0;
	bool thin = false;
	float t_hit = -1.0;
	int k = 0;
	for (; k < near_steps; ++k) {
		float perp2 = f * f * across;
		if (perp2 < bound) {
			float cx = along_x ? cm : cn;
			float cz = along_x ? cn : cm;
			float lx = ox - cx;
			float lz = oz - cz;
			float b = lx * dx + lz * dz;
			float tc = (0.0 - b) * inv_a;
			if (perp2 < 0.027225) {
				float c = lx * lx + lz * lz;
				if (tc > 0.0 || c < 0.027225) {
					float th = -1.0;
					if (t_base + tc < LV_EXACT) {
						// the profile as seven cone frustums, solved exactly: only those between the heights at which
						// the ray enters and leaves the lamp's bounding cylinder, in the order the ray meets them
						float sq = math::sqrt(b * b - a * (c - 0.027225));
						float t_in = (0.0 - b - sq) * inv_a;
						float y_in = oy + dy * (t_in < 0.0 ? 0.0 : t_in);
						float y_out = oy + dy * (sq - b) * inv_a;
						float y_lo = y_in < y_out ? y_in : y_out;
						float y_hi = y_in < y_out ? y_out : y_in;
						if (y_hi > 0.0 && y_lo < 1.0) {
							int seg_lo = y_lo < 0.42 ? (y_lo < 0.2 ? 0 : (y_lo < 0.3 ? 1 : 2)) : (y_lo < 0.7 ? (y_lo < 0.56 ? 3 : 4) : (y_lo < 0.84 ? 5 : 6));
							int seg_hi = y_hi < 0.42 ? (y_hi < 0.2 ? 0 : (y_hi < 0.3 ? 1 : 2)) : (y_hi < 0.7 ? (y_hi < 0.56 ? 3 : 4) : (y_hi < 0.84 ? 5 : 6));
							int seg = dy >= 0.0 ? seg_lo : seg_hi;
							int seg_step = dy >= 0.0 ? 1 : -1;
							for (int left = seg_hi - seg_lo; left >= 0 && th < 0.0; --left) {
								// the frustum's foot and top, its radius at the foot and its slope
								float y0 = 0.0;
								float y1 = 0.2;
								float r0 = 0.16;
								float sl = -0.349825;
								if (seg == 1) {
									y0 = 0.2; y1 = 0.3; r0 = 0.08886; sl = 0.288796;
								} else if (seg == 2) {
									y0 = 0.3; y1 = 0.42; r0 = 0.11776; sl = 0.102;
								} else if (seg == 3) {
									y0 = 0.42; y1 = 0.56; r0 = 0.13; sl = -0.0507474;
								} else if (seg == 4) {
									y0 = 0.56; y1 = 0.7; r0 = 0.1228852; sl = -0.1523148;
								} else if (seg == 5) {
									y0 = 0.7; y1 = 0.84; r0 = 0.1015408; sl = -0.2538822;
								} else if (seg == 6) {
									y0 = 0.84; y1 = 1.0; r0 = 0.066; sl = -0.199875;
								}
								seg += seg_step;
								float ra = r0 + (oy - y0) * sl;
								float rb = dy * sl;
								float qa = a - rb * rb;
								if (qa > -0.0000001 && qa < 0.0000001) { continue; }
								float qb = b - ra * rb;
								float disc = qb * qb - qa * (c - ra * ra);
								if (disc < 0.0) { continue; }
								float root = math::sqrt(disc) / qa;
								float mid = (0.0 - qb) / qa;
								float t0 = root < 0.0 ? mid + root : mid - root;
								float t1 = root < 0.0 ? mid - root : mid + root;
								float ya = oy + dy * t0;
								float yb = oy + dy * t1;
								if (t0 > 0.0001 && ya >= y0 && ya <= y1 && ra + rb * t0 >= 0.0) {
									th = t0;
								} else if (t1 > 0.0001 && yb >= y0 && yb <= y1 && ra + rb * t1 >= 0.0) {
									th = t1;
								}
							}
						}
					} else {
						float qh = oy + dy * tc;
						if (qh > 0.0 && qh < 1.0) {
							float eh = qh - 0.42;
							float wd = qh < 0.2 ? 0.16 - 0.35 * qh : (qh < 0.42 ? 0.13 - 0.85 * eh * eh : (qh < 0.84 ? 0.13 - 0.363 * eh * eh : 0.066 - (qh - 0.84) * 0.2));
							if (perp2 < wd * wd) { th = tc - math::sqrt((wd * wd - perp2) * inv_a); }
						}
					}
					if (th > 0.0) {
						t_hit = th;
						break;
					}
				}
			}
			float tn = tc < t_from ? t_from : (tc > t_to ? t_to : tc);
			float fade = 1.0 - tn / LV_NEAR;
			if (fade > 0.0) {
				float gap = tc - tn;
				float qy = oy + dy * tn;
				float side = qy < 0.25 ? 0.25 - qy : (qy > 0.8 ? qy - 0.8 : 0.0);
				float inside = 1.0 - (perp2 + gap * gap * a + side * side) * 8.1632653;
				if (inside > 0.0 && fade * inside * inside > best) {
					best = fade * inside * inside;
					glow_x = cx;
					glow_z = cz;
					bound = (1.0 - math::sqrt(best)) * 0.1225;
					bound = bound < 0.05 ? 0.05 : bound;
				}
				float pixel = LV_PIXEL * tn;
				if (gap == 0.0 && qy < 1.0 + pixel) {
					float qc = qy < 0.0 ? 0.0 : (qy > 1.0 ? 1.0 : qy);
					float ec = qc - 0.42;
					float wide = qc < 0.2 ? 0.16 - 0.35 * qc : (qc < 0.42 ? 0.13 - 0.85 * ec * ec : (qc < 0.84 ? 0.13 - 0.363 * ec * ec : 0.066 - (qc - 0.84) * 0.2));
					if (wide < pixel * 1.3 && perp2 < (wide + pixel) * (wide + pixel)) { thin = true; }
				}
			}
		}
		f += f_step;
		cm += step;
		if (f > 0.5) {
			f -= 1.0;
			cn += 1.0;
		} else if (f < -0.5) {
			f += 1.0;
			cn -= 1.0;
		}
	}
	bool far = false;
	for (; t_hit < 0.0 && k < count; ++k) {
		float far2 = f * f * across;
		if (far2 < 0.027225) {
			float tf = ((along_x ? cm : cn) - ox) * dx * inv_a + ((along_x ? cn : cm) - oz) * dz * inv_a;
			float qf = oy + dy * tf;
			if (tf > 0.0 && qf > 0.0 && qf < 1.0) {
				float ef = qf - 0.42;
				float wf = qf < 0.2 ? 0.16 - 0.35 * qf : (qf < 0.42 ? 0.13 - 0.85 * ef * ef : (qf < 0.84 ? 0.13 - 0.363 * ef * ef : 0.066 - (qf - 0.84) * 0.2));
				float back = (wf * wf - far2) * inv_a;
				if (back > 0.0 && back < tf * tf) {
					t_hit = tf - math::sqrt(back);
					far = true;
				}
			}
		}
		f += f_step;
		cm += step;
		if (f > 0.5) {
			f -= 1.0;
			cn += 1.0;
		} else if (f < -0.5) {
			f += 1.0;
			cn -= 1.0;
		}
	}
	if (t_hit > 0.0) { return t_hit + (thin ? 32.0 : 0.0) + (far ? 64.0 : 0.0); }
	if (best <= 0.0) { return thin ? -1025.0 : -1.0; }
	// the bloom never clips to a flat patch: it eases toward the ramp's top
	float glow = best * 2.0 * LV_GAIN;
	int glow_id = ((math::itrunc(glow_x) * 7349 + math::itrunc(glow_z) * 1931) % 997 + 997) % 997;
	int hue = (math::itrunc((glow_id % 6) * 4.0 + LV_HUE_ROOM) % 24 + 24) % 24;
	return -1.0 - (math::itrunc(15.99 * glow / (1.0 + glow)) + hue * 16 + (thin ? 1024 : 0));
}

// what the glass holds along a ray that entered it at (x, py, z) heading (wx, wy, wz), both in the blob set's own
// frame: the wax's lit surface in its ramp, or the liquid's glow in LV_GLOW's. `cell` is the lamp (lamp_shade
// packs it), `dist` how far the eye is.
function wax_inside(float x, float py, float z, float wx, float wy, float wz, float dist, int cell) -> int {
	int ci = cell / 32768 - 16384;
	int cj = cell % 32768 - 16384;
	int id = lamp_id(ci, cj);
	float boost = ring_heat(ci, cj);
	float hue_off = lamp_hue(ci, cj);
	float fk = 1.0 - dist * 0.07;
	fk = fk < 0.15 ? 0.15 : fk;
	bool fine = dist < LV_NEAR;
	float axz = wx * wx + wz * wz;
	float t_end = axz > 0.000001 ? (x * wx + z * wz) * -2.0 / axz : 0.288;
	t_end = t_end > 0.288 ? 0.288 : (t_end < 0.02 ? 0.02 : t_end);
	float y_end = py + wy * t_end;
	float y_min = py < y_end ? py : y_end;
	float y_max = py < y_end ? y_end : py;
	float wy2 = wy * wy;
	// the blobs near the ray, six at most: each one's squared distance along the ray as a quadratic (a, 2b, c), its
	// radius squared and its index. An empty place adds nothing to the field.
	float a0 = 0.0; float b0 = 0.0; float c0 = 1.0; float r0 = 0.0; int i0 = 0;
	float a1 = 0.0; float b1 = 0.0; float c1 = 1.0; float r1 = 0.0; int i1 = 0;
	float a2 = 0.0; float b2 = 0.0; float c2 = 1.0; float r2 = 0.0; int i2 = 0;
	float a3 = 0.0; float b3 = 0.0; float c3 = 1.0; float r3 = 0.0; int i3 = 0;
	float a4 = 0.0; float b4 = 0.0; float c4 = 1.0; float r4 = 0.0; int i4 = 0;
	float a5 = 0.0; float b5 = 0.0; float c5 = 1.0; float r5 = 0.0; int i5 = 0;
	int n = 0;
	float peak = 0.0;
	float t_peak = 0.0;
	float t_wax = -1.0;
	int first = (id % 3) * LV_N;
	int last = first + LV_N;
	for (int b = first; b < last && n < 6; ++b) {
		if (y_max < LV_BLO[b] || y_min > LV_BHI[b]) { continue; }
		float ist2 = LV_IST2[b];
		float cx = x - LV_BX[b];
		float cy = py - LV_BY[b];
		float cz = z - LV_BZ[b];
		float qa = 1.0 + wy2 * (ist2 - 1.0);
		float qb = cx * wx + cz * wz + cy * wy * ist2;
		float qc = cx * cx + cz * cz + cy * cy * ist2 + 0.00002;
		float rr = LV_R2[b];
		float tb = (0.0 - qb) / qa;
		tb = tb < 0.0 ? 0.0 : (tb > t_end ? t_end : tb);
		float q = (qa * tb + 2.0 * qb) * tb + qc;
		if (q > rr * 9.0) { continue; }
		if (n == 0) {
			a0 = qa; b0 = 2.0 * qb; c0 = qc; r0 = rr; i0 = b;
		} else if (n == 1) {
			a1 = qa; b1 = 2.0 * qb; c1 = qc; r1 = rr; i1 = b;
		} else if (n == 2) {
			a2 = qa; b2 = 2.0 * qb; c2 = qc; r2 = rr; i2 = b;
		} else if (n == 3) {
			a3 = qa; b3 = 2.0 * qb; c3 = qc; r3 = rr; i3 = b;
		} else if (n == 4) {
			a4 = qa; b4 = 2.0 * qb; c4 = qc; r4 = rr; i4 = b;
		} else {
			a5 = qa; b5 = 2.0 * qb; c5 = qc; r5 = rr; i5 = b;
		}
		++n;
		if (rr > q * peak) {
			peak = rr / q;
			t_peak = tb;
		}
		if (q < rr) {
			// inside this blob's own ellipsoid the field is at least 1: the wax starts at or before its near side
			float te = (0.0 - qb - math::sqrt(qb * qb - qa * (qc - rr))) / qa;
			te = te < 0.0 ? 0.0 : te;
			if (t_wax < 0.0 || te < t_wax) { t_wax = te; }
		}
	}
	int inside = LV_GLOW + hue_index(hue_off + 3.0) * 8 + math::itrunc((0.18 + boost * 0.5) * LV_GAIN * 7.99 * fk);
	if (n > 0) {
		// the field is sampled where the last sample asks: at the hardest glow when no ellipsoid said wax (ask 0),
		// then a little before the wax (1) and at the glass (2) to bracket the surface, then bisecting (3)
		float f_glow = 1.0;
		bool solid = t_wax >= 0.0;
		int ask = solid ? (fine ? (t_wax > 0.06 ? 1 : 2) : 4) : 0;
		float at = solid ? (t_wax > 0.06 ? t_wax - 0.06 : 0.0) : t_peak;
		float lo = 0.0;
		int left = 0;
		while (ask < 4) {
			float field = r0 / ((a0 * at + b0) * at + c0) + r1 / ((a1 * at + b1) * at + c1) + r2 / ((a2 * at + b2) * at + c2);
			if (n > 3) { field = field + r3 / ((a3 * at + b3) * at + c3) + r4 / ((a4 * at + b4) * at + c4) + r5 / ((a5 * at + b5) * at + c5); }
			if (ask == 0) {
				f_glow = field;
				solid = field >= 1.0;
				ask = solid && fine ? (t_peak > 0.06 ? 1 : 2) : 4;
				if (solid) {
					t_wax = t_peak;
					at = t_peak > 0.06 ? t_peak - 0.06 : 0.0;
				}
			} else if (ask == 1 && field >= 1.0) {
				ask = 2;
				at = 0.0;
			} else if (ask == 2 && field >= 1.0) {
				t_wax = 0.0;
				ask = 4;
			} else if (ask < 3) {
				lo = at;
				left = t_wax - lo > 0.07 ? 7 : 5;
				ask = 3;
				at = (lo + t_wax) * 0.5;
			} else {
				if (field >= 1.0) { t_wax = at; } else { lo = at; }
				--left;
				ask = left > 0 ? 3 : 4;
				at = (lo + t_wax) * 0.5;
			}
		}
		// the wax's surface, or where the liquid glows hardest
		float t_at = solid ? t_wax : t_peak;
		float hx = x + wx * t_at;
		float hy = py + wy * t_at;
		float hz = z + wz * t_at;
		if (solid) {
			float wall = lamp_width(hy) + 0.004;
			if (hx * hx + hz * hz > wall * wall) {
				solid = false;
				f_glow = 0.9;
			}
		}
		float gx = 0.0;
		float gy = 0.0;
		float gz = 0.0;
		float fs = 0.0;
		float hs = 0.0;
		for (int near = 0; near < n; ++near) {
			int blob = near < 3 ? (near == 0 ? i0 : (near == 1 ? i1 : i2)) : (near == 3 ? i3 : (near == 4 ? i4 : i5));
			float ex = hx - LV_BX[blob];
			float ey = hy - LV_BY[blob];
			float ez = hz - LV_BZ[blob];
			float eys = ey * LV_IST2[blob];
			float inv = LV_R2[blob] / (ex * ex + ez * ez + eys * ey + 0.00002);
			float pull = inv * inv / LV_R2[blob];
			gx += ex * pull;
			gy += eys * pull;
			gz += ez * pull;
			fs += inv;
			hs += inv * LV_HUE[blob];
		}
		if (!solid) {
			float hue = fs > 0.0 ? hs / fs + 0.5 : 3.0;
			float glow = (f_glow * f_glow * 0.85 + 0.18 + boost * 0.5) * LV_GAIN * 7.99;
			inside = LV_GLOW + hue_index(hue + hue_off) * 8 + math::itrunc((glow > 7.0 ? 7.0 : glow) * fk);
		} else if (!fine) {
			float flat = 0.2 + 0.4 * LV_GAIN + boost * 0.4;
			inside = hue_index(hs / fs + hue_off + 0.5) * 16 + math::itrunc((flat > 0.72 ? 0.72 : flat) * 15.99 * fk);
		} else {
			float ig = 1.0 / math::sqrt(gx * gx + gy * gy + gz * gz + 0.0000001);
			float mx = gx * ig;
			float my = gy * ig;
			float mz = gz * ig;
			float lit = mx * -0.45 + my * 0.62 + mz * -0.64;
			lit = lit < 0.0 ? 0.0 : lit;
			float mdot = wx * mx + wy * my + wz * mz;
			float s = (wx - 2.0 * mdot * mx) * -0.45 + (wy - 2.0 * mdot * my) * 0.62 + (wz - 2.0 * mdot * mz) * -0.64;
			s = s < 0.0 ? 0.0 : s;
			float s2 = s * s;
			float s8 = s2 * s2 * s2 * s2;
			float edge = 1.0 + mdot;
			edge = edge < 0.0 ? 0.0 : edge;
			// the body stops at the ramp's full colour; only the glint climbs into its whites, a soft lobe round a hot core
			float body = lit * 0.6 * LV_GAIN + 0.16 + edge * (0.1 + edge * 0.25) + boost * 0.4;
			body = body > 0.72 ? 0.72 : body;
			float level = (body + s8 * 0.14 + s8 * s8 * s8 * s8 * 0.4 + LV_FLASH * 0.12) * 15.99;
			inside = hue_index(hs / fs + hue_off + 0.5) * 16 + math::itrunc((level > 15.0 ? 15.0 : level) * fk);
		}
	}
	return inside;
}

// the lamp a ray met (`hit` from first_lamp, for the same ray): steel, or glass over the wax, as a palette index
// plus 1024 for first_lamp's thin part and 2048 for a lamp past LV_NEAR
function lamp_shade(float dx, float eye_dy, float dz, bool mirror, float hit) -> int {
	float ox = LV_EX;
	float oy = LV_EY;
	float oz = LV_EZ;
	int flags = math::itrunc(hit * 0.03125);
	float t = hit - flags * 32.0;
	int keep = flags * 1024;
	float dy = eye_dy;
	float dist = t;
	if (mirror) {
		float t_floor = (0.0 - LV_EY) / eye_dy;
		ox += dx * t_floor;
		oy = 0.0005;
		oz += dz * t_floor;
		dy = 0.0 - eye_dy;
		dist = t_floor + 3.0 + t;
	}
	float sx = ox + dx * t;
	float sz = oz + dz * t;
	int ci = math::ifloor(sx + 0.5);
	int cj = math::ifloor(sz + 0.5);
	float px = sx - ci;
	float py = oy + dy * t;
	float pz = sz - cj;
	float e = py - 0.42;
	float slope = py < 0.2 ? 0.35 : (py < 0.42 ? 1.7 * e : (py < 0.84 ? 0.726 * e : 0.2));
	float il = 1.0 / math::sqrt((px * px + pz * pz + 0.000001) * (1.0 + slope * slope));
	float nx = px * il;
	float nz = pz * il;
	float ny = slope / math::sqrt(1.0 + slope * slope);
	float rdot = dx * nx + dy * ny + dz * nz;
	float rx = dx - 2.0 * rdot * nx;
	float ry = dy - 2.0 * rdot * ny;
	float rz = dz - 2.0 * rdot * nz;
	int cell = (ci + 16384) * 32768 + cj + 16384;
	float fk = 1.0 - dist * 0.07;
	fk = fk < 0.15 ? 0.15 : fk;
	// lamp_hue, hue_index and ring_heat are spelled out here: a call folded in would cost this function its raw entry
	int id = ((ci * 7349 + cj * 1931) % 997 + 997) % 997;
	float hue_off = (id % 6) * 4.0 + LV_HUE0 + LV_BURN * ((ci + cj * 2) * 2.0 + LV_T * 9.0);
	int hue_glass = (math::itrunc(hue_off + 3.0) % 24 + 24) % 24;
	if (py < 0.2 || py > 0.84) {
		// steel: a studio of windows reflected round the cone (cos(3 az + 0.8) from the triple angle), the lamp's own light between them
		float h2 = rx * rx + rz * rz;
		float band = 0.5;
		if (h2 > 0.000001) {
			float ih = 1.0 / math::sqrt(h2);
			float ca = rx * ih;
			float sa = rz * ih;
			band = 0.5 + 0.5 * (ca * (4.0 * ca * ca - 3.0) * 0.69671 - sa * (3.0 - 4.0 * sa * sa) * 0.71736);
		}
		float boost = 0.0;
		if (LV_RING_A > 0.0) {
			float ring_off = math::sqrt((ci - LV_RING_X) * (ci - LV_RING_X) + (cj - LV_RING_Z) * (cj - LV_RING_Z)) - LV_RING_R;
			boost = LV_RING_A / (1.0 + ring_off * ring_off * 1.6);
		}
		if (LV_PULSE_A > 0.0) {
			float pulse_off = math::sqrt((ci - LV_PULSE_X) * (ci - LV_PULSE_X) + (cj - LV_PULSE_Z) * (cj - LV_PULSE_Z)) - LV_PULSE_R;
			float pulse = LV_PULSE_A / (1.0 + pulse_off * pulse_off * 1.6);
			boost = pulse > boost ? pulse : boost;
		}
		float window = band * band;
		if (window < 0.3) {
			float tint = (0.1 + (0.3 - window) * 1.5 * LV_GAIN + boost * 0.4) * 15.99;
			return keep + LV_ROOM + hue_glass * 16 + math::itrunc((tint > 15.0 ? 15.0 : tint) * fk);
		}
		float up = ry < 0.0 ? 0.0 : ry;
		float steel = (0.12 + window * 0.55 + up * up * 0.4 + LV_FLASH * 0.2 + boost * 0.4) * 15.99;
		return keep + LV_STEEL + math::itrunc((steel > 15.0 ? 15.0 : steel) * fk);
	}
	// the glass's rim, in the wax ramp of the liquid's hue: the grazing edge is all rim
	float graze = 1.0 + (rdot > 0.0 ? 0.0 : rdot);
	float rim = graze * graze * (8.0 + 3.0 * LV_GAIN + 3.0 * LV_FLASH);
	rim = (rim > 12.0 ? 12.0 : rim) * fk;
	if (graze > 0.8) { return keep + hue_glass * 16 + math::itrunc(rim); }
	float glint = 0.0;
	if (dist < LV_NEAR) {
		float g = rx * -0.45 + ry * 0.62 + rz * -0.64;
		g = g < 0.0 ? 0.0 : g;
		float g2 = g * g;
		float g8 = g2 * g2 * g2 * g2;
		float g32 = g8 * g8 * g8 * g8;
		glint = (g32 * g32 * 9.0 + g8 * 2.5) * (1.0 + LV_FLASH * 0.6) * fk;
	}
	// into the wax, the ray bent a little toward the axis, turned into the blob set's own frame: the lamp's quarter turn
	float bx = dx - nx * 0.14;
	float bz = dz - nz * 0.14;
	float bl = 1.0 / math::sqrt(bx * bx + dy * dy + bz * bz);
	bx *= bl;
	bz *= bl;
	int quarter = (id / 3) % 4;
	float x = quarter == 0 ? px : (quarter == 1 ? pz : (quarter == 2 ? 0.0 - px : 0.0 - pz));
	float z = quarter == 0 ? pz : (quarter == 1 ? 0.0 - px : (quarter == 2 ? 0.0 - pz : px));
	float wx = quarter == 0 ? bx : (quarter == 1 ? bz : (quarter == 2 ? 0.0 - bx : 0.0 - bz));
	float wz = quarter == 0 ? bz : (quarter == 1 ? 0.0 - bx : (quarter == 2 ? 0.0 - bz : bx));
	float wy = dy * bl;
	int inside = wax_inside(x, py, z, wx, wy, wz, dist, cell);
	if (inside >= LV_GLOW) {
		// the liquid under rim or glint moves to the wax ramp of its hue at the level that matches its glow
		int off = inside - LV_GLOW;
		float match = (off % 8) * 1.1 - 1.0;
		float lifted = (rim > match ? rim : match) + glint;
		if (lifted < match + 0.5) { return keep + inside; }
		return keep + off / 8 * 16 + math::itrunc(lifted > 15.0 ? 15.0 : lifted);
	}
	int own = inside % 16;
	float raised = (rim > own ? rim : own) + glint;
	return keep + inside - own + math::itrunc(raised > 15.0 ? 15.0 : raised);
}

// the ray through pixel (x, y): a lamp's surface, the floor with what it mirrors, or the haze, as a palette index
// with first_lamp's 1024 (a thin part near) and 2048 (the far field)
function lava_pixel(int x, int y) -> int {
	float half = LV_H * 0.5;
	float v = (half - y - 0.5) / half;
	float u = (x + 0.5 - LV_W * 0.5) / half;
	float dx = LV_FX * LV_FOCAL + LV_RX * u + LV_UX * v;
	float dy = LV_FY * LV_FOCAL + LV_RY * u + LV_UY * v;
	float dz = LV_FZ * LV_FOCAL + LV_RZ * u + LV_UZ * v;
	// a ray straight down has no heading for the grid walk: lean it a hair
	if (dx * dx + dz * dz < 0.000001) { dx += 0.002; }
	float il = 1.0 / math::sqrt(dx * dx + dy * dy + dz * dz);
	dx *= il;
	dy *= il;
	dz *= il;
	float seen = first_lamp(dx, dy, dz, false);
	if (seen >= 0.0) {
		int lamp = lamp_shade(dx, dy, dz, false, seen);
		return lamp;
	}
	// no lamp: the floor under its lamp's pool of light, or the haze above the horizon, faintly lit near it, either
	// glowing round the lamps as first_lamp counted their bloom
	int bloom = -1 - math::itrunc(seen);
	int edge = bloom >= 1024 ? 1024 : 0;
	int halo = bloom % 16;
	int glow_ink = LV_ROOM + (bloom / 16 % 64) * 16;
	float t_floor = dy < -0.0001 ? (0.0 - LV_EY) / dy : 1000.0;
	if (t_floor >= LV_FAR) {
		if (halo > 2) { return glow_ink + halo + edge; }
		return LV_ROOM + 17 * 16 + (dy > 0.18 ? 0 : (dy > 0.06 ? 1 : 2)) + edge;
	}
	float px = LV_EX + dx * t_floor;
	float pz = LV_EZ + dz * t_floor;
	float cx = math::floor(px + 0.5);
	float cz = math::floor(pz + 0.5);
	float pool = 1.0 / (1.0 + ((px - cx) * (px - cx) + (pz - cz) * (pz - cz)) * 18.0);
	int id = ((math::itrunc(cx) * 7349 + math::itrunc(cz) * 1931) % 997 + 997) % 997;
	int hue = (math::itrunc((id % 6) * 4.0 + LV_HUE_ROOM) % 24 + 24) % 24;
	// a ring of heat lights the pool as it passes the lamp, and runs across the floor as a line of light
	float heat = 0.0;
	float line = 0.0;
	if (LV_RING_A > 0.0) {
		float lamp_off = math::sqrt((cx - LV_RING_X) * (cx - LV_RING_X) + (cz - LV_RING_Z) * (cz - LV_RING_Z)) - LV_RING_R;
		float off = math::sqrt((px - LV_RING_X) * (px - LV_RING_X) + (pz - LV_RING_Z) * (pz - LV_RING_Z)) - LV_RING_R;
		heat = LV_RING_A / (1.0 + lamp_off * lamp_off * 1.6);
		line = LV_RING_A / (1.0 + off * off * 40.0);
	}
	if (LV_PULSE_A > 0.0) {
		float lamp_off2 = math::sqrt((cx - LV_PULSE_X) * (cx - LV_PULSE_X) + (cz - LV_PULSE_Z) * (cz - LV_PULSE_Z)) - LV_PULSE_R;
		float off2 = math::sqrt((px - LV_PULSE_X) * (px - LV_PULSE_X) + (pz - LV_PULSE_Z) * (pz - LV_PULSE_Z)) - LV_PULSE_R;
		float heat2 = LV_PULSE_A / (1.0 + lamp_off2 * lamp_off2 * 1.6);
		float line2 = LV_PULSE_A / (1.0 + off2 * off2 * 40.0);
		heat = heat2 > heat ? heat2 : heat;
		line = line2 > line ? line2 : line;
	}
	float k = 1.0 - t_floor * 0.07;
	float level = (pool * pool * 1.7 * LV_GAIN + heat * pool * 0.8 + line * 0.9) * 15.99;
	if (line > 0.35) { hue = (hue + 12) % 24; }
	int own = math::itrunc((level > 15.0 ? 15.0 : level) * (k < 0.15 ? 0.15 : k));
	if (t_floor >= 5.0) {
		if (halo > own) { return glow_ink + halo + edge + (t_floor < LV_NEAR ? 0 : 2048); }
		return LV_ROOM + hue * 16 + own + edge + (t_floor < LV_NEAR ? 0 : 2048);
	}
	// the near floor is black glass: the lamp it mirrors lies over its pool of light, dimmed, and the bloom over both
	int result = LV_ROOM + hue * 16 + own;
	int shown = own;
	float mirrored = first_lamp(dx, dy, dz, true);
	if (mirrored >= 0.0) {
		int sunk_lamp = lamp_shade(dx, dy, dz, true, mirrored) % 1024;
		// a liquid's ramp has eight levels where the others have sixteen
		bool liquid = sunk_lamp >= LV_GLOW && sunk_lamp < LV_STEEL;
		int lamp_level = sunk_lamp < LV_GLOW ? sunk_lamp % 16 : (liquid ? (sunk_lamp - LV_GLOW) % 8 : (sunk_lamp - LV_STEEL) % 16);
		int sunk = math::itrunc(lamp_level * (0.6 + LV_EY / dy * 0.08));
		int sunk_shown = liquid ? sunk * 2 : sunk;
		if (sunk_shown > shown) {
			result = sunk_lamp - lamp_level + sunk;
			shown = sunk_shown;
		}
	}
	if (halo > shown) { result = glow_ink + halo; }
	return result + edge;
}


// lattice row j: the rays through every second pixel of every second picture row, one past the right edge included
function lava_lattice_row(int j) -> array<int> {
	array<int> out = [];
	for (int i = 0; i < LV_CW; ++i) { out.push(lava_pixel(i * 2, j * 2)); }
	return out;
}

// the colour between two lattice samples: their mean when they sit in one ramp at close levels with no thin part
// passing near either, or -1 for a pixel that must be traced. Two samples of the floor and haze settle it in the
// brighter one's ramp, and two from past LV_NEAR always do (their 2048 stays on): the far field is drawn at the
// lattice's grain.
function between(int s0, int s1, int odd) -> int {
	int i0 = s0 % 1024;
	int i1 = s1 % 1024;
	int l0 = LV_LEVEL[i0];
	int l1 = LV_LEVEL[i1];
	bool one_ramp = i0 - l0 == i1 - l1;
	if (s0 / 1024 == 2 && s1 / 1024 == 2) { return one_ramp ? s0 - l0 + (l0 + l1 + odd) / 2 : s0; }
	if (s0 >= 1024 || s1 >= 1024) { return -1; }
	if (i0 >= LV_ROOM && i1 >= LV_ROOM) { return (l0 >= l1 ? i0 - l0 : i1 - l1) + (l0 + l1 + odd) / 2; }
	int apart = l0 - l1;
	if (!one_ramp || apart > 3 || apart < -3) { return -1; }
	return s0 - l0 + (l0 + l1 + odd) / 2;
}

// picture rows 2j and 2j+1 from the lattice rows above and below them
function lava_block_row(int j) -> array<int> {
	array<int> out = [];
	int ww = LV_W;
	int y = j * 2;
	int top = j * LV_CW;
	for (int x = 0; x < ww; ++x) {
		int at = top + x / 2;
		int here = LV_LATTICE[at];
		if (x % 2 == 0) {
			out.push(here % 1024);
			continue;
		}
		int mean = here < 1024 && here == LV_LATTICE[at + 1] ? here : between(here, LV_LATTICE[at + 1], j % 2);
		out.push((mean >= 0 ? mean : lava_pixel(x, y)) % 1024);
	}
	if (y + 1 >= LV_H) { return out; }
	int bottom = top + LV_CW;
	for (int x2 = 0; x2 < ww; ++x2) {
		int above = LV_LATTICE[top + x2 / 2];
		int below = bottom + x2 / 2;
		int mean2 = -1;
		if (x2 % 2 == 0) {
			mean2 = above < 1024 && above == LV_LATTICE[below] ? above : between(above, LV_LATTICE[below], (x2 / 2) % 2);
		} else {
			int falling = between(above, LV_LATTICE[below + 1], 1);
			int rising = between(LV_LATTICE[top + x2 / 2 + 1], LV_LATTICE[below], 0);
			if (falling >= 0 && rising >= 0) { mean2 = between(falling, rising, (x2 / 2 + j) % 2); }
		}
		out.push((mean2 >= 0 ? mean2 : lava_pixel(x2, y + 1)) % 1024);
	}
	return out;
}

class Lava : Scene {
	array<float> bx = [];
	array<float> by = [];
	array<float> bz = [];
	array<float> br2 = [];
	array<float> ist2 = [];
	array<float> blo = [];
	array<float> bhi = [];
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

	// a blob that neither moves nor stretches: the pool on the heater, the wax under the cap
	void still_blob(float y, float r, float tint) {
		br2.push(r * r);
		hue.push(tint);
		bx.push(0.0);
		by.push(y);
		bz.push(0.0);
		ist2.push(1.0);
		blo.push(y - r * 3.0);
		bhi.push(y + r * 3.0);
	}

	void build() override {
		name = "LAVALAMP";
		handle = "wax and gradient . by operator overload";
		// 24 hues x 16 shades (wax), then 24 hues x 8 glow levels (the liquid)
		array<int> level = [];
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
				level.push(s);
			}
		}
		LV_GLOW = pal.size();
		for (int hq2 = 0; hq2 < 24; ++hq2) {
			var c2 = gfx.rainbow_rgb(hq2 * 0.26 + 0.42);
			for (int g2 = 0; g2 < 8; ++g2) {
				float k = g2 / 7.0;
				k = k * k * 0.35 + k * 0.4;
				pal.add(14.0 + c2[0] * k, 6.0 + c2[1] * k, 30.0 + c2[2] * k);
				level.push(g2);
			}
		}
		LV_STEEL = pal.gradient(16, [[8.0, 8.0, 12.0], [50.0, 52.0, 62.0], [140.0, 144.0, 160.0], [235.0, 238.0, 250.0]]);
		for (int s3 = 0; s3 < 16; ++s3) { level.push(s3); }
		// the room: each wax hue's light on a dark floor, 16 levels (the violet's lowest are the haze above)
		LV_ROOM = pal.size();
		for (int hq3 = 0; hq3 < 24; ++hq3) {
			var c3 = gfx.rainbow_rgb(hq3 * 0.26 + 0.42);
			for (int r3 = 0; r3 < 16; ++r3) {
				float k3 = r3 / 15.0 * 0.5;
				pal.add(5.0 + c3[0] * k3, 4.0 + c3[1] * k3, 9.0 + c3[2] * k3);
				level.push(r3);
			}
		}
		LV_LEVEL = level;
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
				ist2.push(1.0);
				blo.push(0.0);
				bhi.push(0.0);
			}
			still_blob(0.16, 0.07, 3.5);
			still_blob(0.87, 0.024, 4.5);
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
				float stretch = 1.0 + (vy < 0.0 ? -vy : vy) * 0.9;
				float reach = math::sqrt(br2[slot]) * 3.0 * stretch;
				by[slot] = y;
				bx[slot] = w * math::sin(t * 0.37 + sway[k]);
				bz[slot] = w * math::cos(t * 0.29 + sway[k] * 1.7);
				ist2[slot] = 1.0 / (stretch * stretch);
				blo[slot] = y - reach;
				bhi[slot] = y + reach;
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
		LV_PIXEL = 1.0 / (LV_FOCAL * h * 0.5);
		LV_GAIN = 0.62 + energy * 0.3 + kick * 0.3 + hit * 0.45 + riser * riser * 0.4 + smooth(MUSIC_BAR * 8.0, MUSIC_BAR * 9.0, t) * 0.2;
		LV_FLASH = hit + snare * 0.25;
		LV_HUE0 = chord * 2.0;
		LV_BURN = smooth(MUSIC_BAR * 3.0, MUSIC_BAR * 3.0 + 0.4, t) * (1.0 - smooth(MUSIC_BAR * 4.0 - 0.3, MUSIC_BAR * 4.0 + 0.3, t))
			+ smooth(MUSIC_BAR * 8.0, MUSIC_BAR * 8.0 + 0.4, t);
		LV_T = t;
		LV_HUE_ROOM = LV_HUE0 + 3.0 + LV_BURN * t * 9.0;
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
		LV_IST2 = ist2;
		LV_BLO = blo;
		LV_BHI = bhi;
		LV_HUE = hue;
		LV_W = w;
		LV_H = h;
		LV_CW = w / 2 + 1;
		int lattice_rows = h / 2 + 1;
		int block_rows = (h + 1) / 2;
		if (LV_LATTICE_ROWS.size() != lattice_rows) {
			LV_LATTICE_ROWS = [];
			LV_BLOCK_ROWS = [];
			for (int r = 0; r < lattice_rows; ++r) { LV_LATTICE_ROWS.push(r); }
			for (int r2 = 0; r2 < block_rows; ++r2) { LV_BLOCK_ROWS.push(r2); }
		}
		var samples = parallel_transform(LV_LATTICE_ROWS, lava_lattice_row);
		LV_LATTICE.resize(LV_CW * lattice_rows, 0);
		int at = 0;
		for (int j = 0; j < lattice_rows; ++j) {
			var& sampled = samples[j];
			for (int i = 0; i < LV_CW; ++i) {
				LV_LATTICE[at] = sampled[i];
				++at;
			}
		}
		var blocks = parallel_transform(LV_BLOCK_ROWS, lava_block_row);
		int idx = 0;
		for (int band = 0; band < block_rows; ++band) {
			var& pair = blocks[band];
			int count = pair.size();
			for (int x = 0; x < count; ++x) {
				grid[idx] = pair[x];
				++idx;
			}
		}
	}
}
