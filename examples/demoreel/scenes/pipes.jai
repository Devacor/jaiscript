// PIPEDREAM — the 3D Pipes screensaver homage, tuned for authenticity: thick
// round pipes, glossy elbow balls at every turn, one segment per fixed tick
// (patient, steady, inevitable), pipes never intersect (occupancy grid; a
// blocked pipe turns or terminates and a fresh pipe respawns), and each
// generation fills to threshold, holds, CRT-wipes and reseeds, like the real thing.
//
// A real camera flies the set. Every frame the whole sculpture is projected from
// the camera: each link between two voxels is a capsule impostor (a sphere swept
// from one voxel's centre to the next, its radius and 1/z interpolated along the
// screen segment), each elbow a fatter ball. Per pixel the nearest axis point
// gives a normal (diffuse, a sharp specular, distance fog) and a depth nudge, so
// crossings intersect against a depth buffer. The impostors are binned into
// bands of rows and the bands drawn across every core with parallel_transform.
//
// The pipes are chrome: each pixel's normal reflects the view ray into the world,
// which reads the set (a sky above, a burning horizon line, a dark floor below,
// and the sun itself as a rim of white wherever a grazing ray points at it). The
// set is 3D too: a striped sun low over the horizon in one direction, a sky graded
// by elevation, a floor plane whose grid lines stay one pixel wide (a line wherever
// one falls between a pixel and its neighbours) rushing in with the energy, and
// glossy, carrying the pipes' mirror images (the impostors mirrored in the floor,
// flat-shaded) and the sun's broken reflection.
//
// Scored to its cue: generation one grows while the camera orbits up from the floor,
// swinging round on every bar line, and fills on bar 3 with a glint and a sweep; the
// wipe carries the camera to a high new angle, generation two grows (round a tunnel
// it is never allowed to enter) as the camera cranes down to the floor and fills on
// bar 7; the camera dives into the tunnel and flies through the sculpture toward the
// sun, lunging on every kick, bursts out, swings round on bar 9 to look back, cranes
// up as the riser builds, and on bar 10 a white-hot surge races along every pipe in
// the order it grew while the camera sweeps round to the sun for the cut. The kick
// punches the lens and lights the floor grid, the snare flares the chrome, every
// chord recolours the sun. The growing tips burn white-hot and shower sparks, and
// canon demands the teapot: ~2% of joints are the jackodile's rainbow eye.

int PD_W = 0;
int PD_H = 0;
int PD_BH = 6;
float PD_F = 100.0;
float PD_EX = 0.0; float PD_EY = 0.0; float PD_EZ = 0.0;
float PD_FX = 0.0; float PD_FY = 0.0; float PD_FZ = -1.0;
float PD_RX = 1.0; float PD_RY = 0.0; float PD_RZ = 0.0;
float PD_UX = 0.0; float PD_UY = 1.0; float PD_UZ = 0.0;
float PD_YF = -7.0;
float PD_GK = 0.4;
float PD_SCROLL = 0.0;
float PD_SY = 0.0; float PD_SZ = -1.0;      // the sun's direction (it lies due -z)
float PD_SCOS = 0.97;
float PD_SELC = 0.08;
float PD_SR = 0.22;
float PD_SUNK = 66.0;
int PD_SKY = 0;
int PD_SUN = 0;
int PD_SUNSHIFT = 0;
int PD_FLOOR = 0;
int PD_GRID = 0;
int PD_GRIDKICK = 0;
float PD_GLINT = -999.0;
int PD_LIFT = 0;
int PD_WIPE = 0;
int PD_BEAM = -9;
array<float> PD_AX = [];   // the impostors: a screen segment A..A+AB, radius RA + t DR, 1/z ZA + t DZ
array<float> PD_AY = [];
array<float> PD_ABX = [];
array<float> PD_ABY = [];
array<float> PD_IL2 = [];
array<float> PD_RA = [];
array<float> PD_DR = [];
array<float> PD_ZA = [];
array<float> PD_DZ = [];
array<float> PD_G = [];
array<float> PD_FOG = [];
array<float> PD_SPEC = [];
array<int> PD_X0 = [];
array<int> PD_X1 = [];
array<int> PD_Y0 = [];
array<int> PD_Y1 = [];
array<int> PD_BASE = [];
array<int> PD_MIR = [];
array<int> PD_LV = [];
array<int> PD_BSTART = [];
array<int> PD_BIDX = [];
array<int> PD_BANDS = [];

// one band of rows: rasterize its impostors into a local depth buffer (and the floor's
// mirror images into another), then lay the set behind whatever no impostor covered
function pd_band(int b) -> array<int> {
	int ww = PD_W;
	int y0 = b * PD_BH;
	int y1 = y0 + PD_BH > PD_H ? PD_H : y0 + PD_BH;
	int n = (y1 - y0) * ww;
	array<float> zb = [];
	zb.resize(n, 0.0);
	array<int> ink = [];
	ink.resize(n, -1);
	array<float> rz = [];
	rz.resize(n, 0.0);
	array<int> rk = [];
	rk.resize(n, -1);
	int wipe = PD_WIPE;
	for (int k = PD_BSTART[b]; k < PD_BSTART[b + 1]; ++k) {
		int p = PD_BIDX[k];
		int py0 = PD_Y0[p] > y0 ? PD_Y0[p] : y0;
		py0 = py0 > wipe ? py0 : wipe;
		int py1 = PD_Y1[p] < y1 - 1 ? PD_Y1[p] : y1 - 1;
		int px0 = PD_X0[p];
		int px1 = PD_X1[p];
		float ax = PD_AX[p]; float ay = PD_AY[p];
		float abx = PD_ABX[p]; float aby = PD_ABY[p];
		float il2 = PD_IL2[p];
		float ra = PD_RA[p]; float dr = PD_DR[p];
		float za = PD_ZA[p]; float dz = PD_DZ[p];
		int base = PD_BASE[p];
		float rm = dr > 0.0 ? ra + dr : ra;
		if (PD_MIR[p] != 0) {
			for (int yy = py0; yy <= py1; ++yy) {
				float ry = yy + 0.5 - ay;
				int rowb = (yy - y0) * ww;
				int xs = px0;
				int xe = px1;
				if (aby > 0.01 || aby < -0.01) {
					float t0 = (ry - rm) / aby;
					float t1 = (ry + rm) / aby;
					t0 = t0 < 0.0 ? 0.0 : (t0 > 1.0 ? 1.0 : t0);
					t1 = t1 < 0.0 ? 0.0 : (t1 > 1.0 ? 1.0 : t1);
					float xa = ax + t0 * abx;
					float xb = ax + t1 * abx;
					int sa = math::ifloor((xa < xb ? xa : xb) - rm);
					int sb = math::ifloor((xa > xb ? xa : xb) + rm);
					xs = sa > xs ? sa : xs;
					xe = sb < xe ? sb : xe;
				}
				for (int xx = xs; xx <= xe; ++xx) {
					float dx = xx + 0.5 - ax;
					float t = (dx * abx + ry * aby) * il2;
					t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
					float ex = dx - t * abx;
					float ey = ry - t * aby;
					float r = ra + t * dr;
					float r2 = r * r;
					float q = r2 - ex * ex - ey * ey;
					if (q < 0.0) { continue; }
					float depth = za + t * dz;
					if (depth <= rz[rowb + xx]) { continue; }
					rz[rowb + xx] = depth;
					rk[rowb + xx] = base + 1 + math::itrunc(q / r2 * 4.0);
				}
			}
			continue;
		}
		float g = PD_G[p];
		float fog = PD_FOG[p];
		float spec = PD_SPEC[p];
		int plift = PD_LV[p] + PD_LIFT;
		float glint = PD_GLINT;
		for (int yy = py0; yy <= py1; ++yy) {
			float ry = yy + 0.5 - ay;
			int rowb = (yy - y0) * ww;
			int xs = px0;
			int xe = px1;
			if (aby > 0.01 || aby < -0.01) {
				float t0 = (ry - rm) / aby;
				float t1 = (ry + rm) / aby;
				t0 = t0 < 0.0 ? 0.0 : (t0 > 1.0 ? 1.0 : t0);
				t1 = t1 < 0.0 ? 0.0 : (t1 > 1.0 ? 1.0 : t1);
				float xa = ax + t0 * abx;
				float xb = ax + t1 * abx;
				int sa = math::ifloor((xa < xb ? xa : xb) - rm);
				int sb = math::ifloor((xa > xb ? xa : xb) + rm);
				xs = sa > xs ? sa : xs;
				xe = sb < xe ? sb : xe;
			}
			for (int xx = xs; xx <= xe; ++xx) {
				float dx = xx + 0.5 - ax;
				float t = (dx * abx + ry * aby) * il2;
				t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
				float ex = dx - t * abx;
				float ey = ry - t * aby;
				float r = ra + t * dr;
				float q = r * r - ex * ex - ey * ey;
				if (q < 0.0) { continue; }
				float ir = 1.0 / r;
				float nz = math::sqrt(q) * ir;
				float depth = za + t * dz + nz * g;
				if (depth <= zb[rowb + xx]) { continue; }
				float nx = ex * ir;
				float ny = ey * ir;
				float d = nx * -0.48 + ny * -0.56 + nz * 0.67;
				d = d < 0.0 ? 0.0 : d;
				float s = nx * -0.25 + ny * -0.29 + nz * 0.92;
				s = s < 0.0 ? 0.0 : s;
				float s2 = s * s;
				float s4 = s2 * s2;
				float s8 = s4 * s4;
				// chrome: the view ray reflected off the normal, in world space; its height
				// reads the sky, the horizon's hard bright line or the floor, and the sun glints
				float k2 = 2.0 * nz;
				float rcx = k2 * nx;
				float rcy = 0.0 - k2 * ny;
				float rcz = 1.0 - k2 * nz;
				float up = rcx * PD_RY + rcy * PD_UY + rcz * PD_FY;
				float env = up > 0.08 ? 0.34 + up * 0.32 : (up > -0.06 ? 1.05 : 0.06 - up * 0.1);
				float sd = up * PD_SY + (rcx * PD_RZ + rcy * PD_UZ + rcz * PD_FZ) * PD_SZ;
				if (sd > 0.94) { env += (sd - 0.94) * 22.0; }
				float lit = (env * 0.72 + d * 0.22) * fog + s8 * s8 * spec;
				int lv = math::itrunc(lit * 15.99) + plift;
				float dg = xx + yy - glint;
				if (dg > -4.0 && dg < 4.0) { lv += 8; } else if (dg > -14.0 && dg < -9.0) { lv += 5; }
				zb[rowb + xx] = depth;
				ink[rowb + xx] = base + (lv > 15 ? 15 : lv);
			}
		}
	}
	array<int> out = [];
	out.resize(n, 0);
	float f = PD_F;
	float iF = 1.0 / f;
	float cx = ww * 0.5;
	float cy = PD_H * 0.5;
	float hy = PD_EY - PD_YF;
	int i = 0;
	for (int y = y0; y < y1; ++y) {
		float v = (cy - y - 0.5) * iF;
		float bx = PD_FX + PD_UX * v;
		float by = PD_FY + PD_UY * v;
		float bz = PD_FZ + PD_UZ * v;
		bool beam = y == PD_BEAM || y == PD_BEAM - 1;
		for (int x = 0; x < ww; ++x) {
			if (beam) { out[i] = PD_GRID + 7; ++i; continue; }
			if (ink[i] >= 0) { out[i] = ink[i]; ++i; continue; }
			float u = (x + 0.5 - cx) * iF;
			float dx = bx + PD_RX * u;
			float dy = by + PD_RY * u;
			float dz = bz + PD_RZ * u;
			float il = 1.0 / math::sqrt(dx * dx + dy * dy + dz * dz);
			if (dy >= 0.0) {
				// the sky by elevation, and the striped sun sinking into the horizon
				float el = dy * il;
				int si = 31 - math::itrunc(el * 68.0);
				int ink2 = PD_SKY + (si < 0 ? 0 : si);
				if ((dy * PD_SY + dz * PD_SZ) * il > PD_SCOS) {
					float down = (PD_SELC + PD_SR - el) / (PD_SR * 1.35);
					float band = (el - PD_SELC + PD_SR) * PD_SUNK;
					float gap = band - math::floor(band);
					bool cut = el < PD_SELC + PD_SR * 0.25 && gap < 0.18 + down * 0.35;
					if (!cut) { ink2 = PD_SUN + PD_SUNSHIFT + math::itrunc((down > 0.999 ? 0.999 : (down < 0.0 ? 0.0 : down)) * 16.0); }
				}
				out[i] = ink2;
				++i;
				continue;
			}
			// the floor: a line wherever a grid line falls between this pixel and its right
			// or lower neighbour, so lines stay one pixel wide; too fine a step is haze
			float tf = hy / (0.0 - dy);
			float gx = (PD_EX + tf * dx) * PD_GK;
			float gz = (PD_EZ + tf * dz) * PD_GK + PD_SCROLL;
			float dyr = dy + PD_RY * iF;
			float dyd = dy - PD_UY * iF;
			float fx = 9.0;
			float fz = 9.0;
			bool xl = false;
			bool zl = false;
			if (dyr < 0.0 && dyd < 0.0) {
				float tr = hy / (0.0 - dyr);
				float td = hy / (0.0 - dyd);
				float gxr = (PD_EX + tr * (dx + PD_RX * iF)) * PD_GK;
				float gzr = (PD_EZ + tr * (dz + PD_RZ * iF)) * PD_GK + PD_SCROLL;
				float gxd = (PD_EX + td * (dx - PD_UX * iF)) * PD_GK;
				float gzd = (PD_EZ + td * (dz - PD_UZ * iF)) * PD_GK + PD_SCROLL;
				float ax1 = math::abs(gxr - gx);
				float ax2 = math::abs(gxd - gx);
				float az1 = math::abs(gzr - gz);
				float az2 = math::abs(gzd - gz);
				fx = ax1 > ax2 ? ax1 : ax2;
				fz = az1 > az2 ? az1 : az2;
				float fgx = math::floor(gx);
				float fgz = math::floor(gz);
				xl = fgx != math::floor(gxr) || fgx != math::floor(gxd);
				zl = fgz != math::floor(gzr) || fgz != math::floor(gzd);
			}
			float near = 1.0 - tf / il * 0.022;
			near = near < 0.0 ? 0.0 : near;
			int glow = math::itrunc(near * 7.99) + PD_GRIDKICK;
			int ink3 = (zl && fz < 0.34) || (xl && fx < 0.34) ? PD_GRID + (glow > 7 ? 7 : glow) : -1;
			if (ink3 < 0) {
				ink3 = PD_FLOOR + math::itrunc((1.0 - near) * 7.99);
				if (rk[i] >= 0) {
					ink3 = rk[i];
				} else {
					// the sun's reflection thins to sparser stripes as the view steepens: the gloss's Fresnel
					float dep = 0.0 - dy * il;
					bool stripe = dep < 0.07 ? y % 2 == 0 : dep < 0.17 && y % 4 == 0;
					if (stripe && (dep * PD_SY + dz * il * PD_SZ) > PD_SCOS) {
						float down2 = (PD_SELC + PD_SR - dep) / (PD_SR * 1.35);
						int sl = math::itrunc((down2 > 0.999 ? 0.999 : (down2 < 0.0 ? 0.0 : down2)) * 16.0) + 4;
						ink3 = PD_SUN + PD_SUNSHIFT + (sl > 15 ? 15 : sl);
					}
				}
			}
			out[i] = ink3;
			++i;
		}
	}
	return out;
}

class Pipes : Scene {
	array<int> cells = [];     // 0 empty, else 1 + pipe (TUNNEL: reserved, never entered)
	array<int> cell_ord = [];  // a filled cell's place in the growth order
	array<float> ox = [];      // the growth order: each voxel's centre, its pipe and its kind
	array<float> oy = [];      // (0 straight, 1 elbow, 2 the eye)
	array<float> oz = [];
	array<int> opipe = [];
	array<int> okind = [];
	array<int> run_a = [];     // the straight runs: first and last voxel, one capsule each
	array<int> run_b = [];
	array<int> cur_run = [];   // each pipe's growing run
	array<float> cvx = [];     // this frame: each voxel in camera space
	array<float> cvy = [];
	array<float> cvz = [];
	array<float> cmx = [];     // and its mirror image under the floor
	array<float> cmy = [];
	array<float> cmz = [];
	array<int> hx = [];        // pipe heads
	array<int> hy = [];
	array<int> hz = [];
	array<int> hd = [];
	array<int> DX = [1, -1, 0, 0, 0, 0];   // dirs paired so (d ^ 1) is the reverse
	array<int> DY = [0, 0, 1, -1, 0, 0];
	array<int> DZ = [0, 0, 0, 0, 1, -1];
	int TUNNEL = 1000;
	int gw = 16;
	int gh = 12;
	int gd = 16;
	int npipes = 6;
	int pipe_base = 0;
	int egg_base = 0;
	int hot_ramp = 0;          // white heat for the tips and the surge, 16 levels
	int sky_base = 0;
	int sun_base = 0;
	int floor_base = 0;
	int grid_base = 0;
	int hot_base = 0;          // the sparks, 8 levels
	float glint_t = -9.0;      // the last fill or surge: the glint and flash
	int gen = 0;
	int filled = 0;
	int cap = 0;
	int phase = 0;             // 0 grow, 1 hold, 2 wipe
	float phase_t = 0.0;
	int last_wipe = 0;
	float tt = 0.0;
	int flick = 0;
	float scroll = 0.0;        // the floor grid's travel, faster as the music builds
	int suns = 4;              // the sun's ramps, one per chord colour
	float a0 = -1.3;           // each generation's starting azimuth
	float c0 = 1.15;
	float foc = 100.0;
	float cam_ex = 0.0;        // the camera this frame
	float cam_ey = 0.0;
	float cam_ez = 0.0;
	float cam_yaw = 0.0;
	float cam_pitch = 0.0;
	float cam_lens = 1.0;
	float cam_dist = 30.0;
	float NEAR = 0.45;

	void build() override {
		name = "PIPEDREAM";
		handle = "screensaver seance . watch for the jackodile";
		replay = 22.0;
		cells = gfx.make_grid(gw * gh * gd);
		cell_ord = gfx.make_grid(gw * gh * gd);
		cap = gw * gh * gd * 34 / 100;
		sky_base = pal.gradient(32, [[4.0, 2.0, 18.0], [22.0, 8.0, 52.0], [92.0, 18.0, 96.0], [230.0, 60.0, 120.0], [255.0, 150.0, 90.0]]);
		sun_base = pal.gradient(16, [[255.0, 236.0, 120.0], [255.0, 150.0, 70.0], [255.0, 60.0, 140.0]]);
		pal.gradient(16, [[255.0, 226.0, 255.0], [214.0, 120.0, 255.0], [120.0, 60.0, 255.0]]);
		pal.gradient(16, [[214.0, 255.0, 244.0], [80.0, 220.0, 255.0], [60.0, 110.0, 255.0]]);
		pal.gradient(16, [[255.0, 250.0, 190.0], [255.0, 120.0, 60.0], [220.0, 30.0, 70.0]]);
		floor_base = pal.gradient(8, [[5.0, 2.0, 12.0], [70.0, 16.0, 78.0]]);
		hot_base = pal.gradient(8, [[255.0, 90.0, 40.0], [255.0, 200.0, 90.0], [255.0, 255.0, 240.0]]);
		grid_base = pal.gradient(8, [[40.0, 20.0, 90.0], [60.0, 220.0, 255.0]]);
		pipe_base = pal.size();
		var hues = [[70.0, 225.0, 195.0], [255.0, 178.0, 58.0], [232.0, 88.0, 212.0], [128.0, 158.0, 255.0],
			[250.0, 90.0, 80.0], [150.0, 240.0, 90.0]];
		for (int p = 0; p < npipes; ++p) {
			var hue = hues[p];
			shade_ramp(hue[0], hue[1], hue[2]);
		}
		egg_base = pal.size();
		for (int e = 0; e < 6; ++e) {
			var c = gfx.rainbow_rgb(e * 1.0471975512);
			shade_ramp(c[0], c[1], c[2]);
		}
		hot_ramp = pal.size();
		pal.gradient(16, [[90.0, 16.0, 8.0], [255.0, 80.0, 30.0], [255.0, 196.0, 110.0], [255.0, 255.0, 246.0], [255.0, 255.0, 255.0]]);
		a0 = -1.3 + rng.next(30) * 0.01;
		c0 = 1.05 + rng.next(30) * 0.01;
		for (int p2 = 0; p2 < npipes; ++p2) {
			hx.push(0);
			hy.push(0);
			hz.push(0);
			hd.push(0);
			cur_run.push(0);
			respawn(p2);
		}
		PD_BANDS = [];
		int nb = (h + PD_BH - 1) / PD_BH;
		for (int b = 0; b < nb; ++b) { PD_BANDS.push(b); }
		PD_BSTART = gfx.make_grid(nb + 1);
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

	int cell_idx(int x, int y, int z) {
		return (y * gd + z) * gw + x;
	}

	bool is_free(int x, int y, int z) {
		if (x < 0 || x >= gw || y < 0 || y >= gh || z < 0 || z >= gd) { return false; }
		return cells[cell_idx(x, y, z)] == 0;
	}

	// a voxel joins the growth order, linked to the one it grew from (-1 for a fresh pipe)
	void occupy(int p, int x, int y, int z, int from) {
		int ci = cell_idx(x, y, z);
		cells[ci] = 1 + p;
		cell_ord[ci] = ox.size();
		ox.push(x - (gw - 1) * 0.5);
		oy.push(y - (gh - 1) * 0.5);
		oz.push(z - (gd - 1) * 0.5);
		opipe.push(p);
		okind.push(0);
		if (from < 0) {
			cur_run[p] = run_a.size();
			run_a.push(ox.size() - 1);
			run_b.push(ox.size() - 1);
		} else {
			run_b[cur_run[p]] = ox.size() - 1;
		}
		hx[p] = x;
		hy[p] = y;
		hz[p] = z;
		++filled;
	}

	void advance(int p, int nx, int ny, int nz) {
		occupy(p, nx, ny, nz, cell_ord[cell_idx(hx[p], hy[p], hz[p])]);
	}

	void respawn(int p) {
		for (int tr = 0; tr < 30; ++tr) {
			int x = rng.next(gw);
			int y = rng.next(gh);
			int z = rng.next(gd);
			if (cells[cell_idx(x, y, z)] == 0) {
				hd[p] = rng.next(6);
				occupy(p, x, y, z, -1);
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
		// a turn: the head becomes an elbow ball, or, rarely, the eye
		int here = cell_ord[cell_idx(x, y, z)];
		okind[here] = rng.chance(2) ? 2 : 1;
		cur_run[p] = run_a.size();
		run_a.push(here);
		run_b.push(here);
		hd[p] = nd;
		advance(p, x + DX[nd], y + DY[nd], z + DZ[nd]);
	}

	// generation one fills by bar 3, holds two beats and wipes by bar 4; generation two
	// fills by bar 7 and stays for the flight
	float fill_end() { return gen == 0 ? MUSIC_BAR * 3.0 : MUSIC_BAR * 7.0; }
	float fill_start() { return gen == 0 ? 0.0 : MUSIC_BAR * 4.0; }

	array<float> hit_times() override { return [MUSIC_BAR * 3.0, MUSIC_BAR * 4.0, MUSIC_BAR * 7.0, MUSIC_BAR * 9.0, MUSIC_BAR * 10.0]; }
	array<int> hit_levels() override { return [2, 1, 2, 1, 2]; }
	// eleven bars: the second generation fills on bar 7, the camera flies through it and swings
	// round on bar 9, the surge lands on bar 10 and the last bar sweeps round to the sun
	int length_bars() override { return 11; }

	void update(float t, float dt) override {
		tt = t;
		float energy = cue == null ? 0.6 : cue.energy(t);
		scroll += dt * 1.6 * (0.55 + 0.9 * energy);
		if (phase == 0) {
			// grow toward the fill the clock calls for: steady, and full on the beat
			float k = (t - fill_start()) / (fill_end() - fill_start());
			k = k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
			int target = math::itrunc(cap * k);
			int guard = 0;
			while (filled < target && guard < 400) {
				for (int p = 0; p < npipes; ++p) { step_one(p); }
				++guard;
			}
			if (t >= fill_end()) {
				phase = 1;
				phase_t = 0.0;
				glint_t = fill_end();
			}
		} else if (phase == 1) {
			phase_t += dt;
			if (gen == 0 && t >= fill_end() + MUSIC_BEAT * 2.0) { phase = 2; phase_t = t - (fill_end() + MUSIC_BEAT * 2.0); }
			if (gen == 1 && t >= MUSIC_BAR * 10.0 && glint_t < MUSIC_BAR * 10.0) { glint_t = MUSIC_BAR * 10.0; }
		} else {
			phase_t += dt;
			if (t >= MUSIC_BAR * 4.0) { reset_volume(); }
		}
	}

	// the second generation grows round a 3x3 tunnel along z that the camera will fly through
	void reset_volume() {
		for (auto& c : cells) { c = 0; }
		for (int z = 0; z < gd; ++z) {
			for (int y = 3; y <= 5; ++y) {
				for (int x = 7; x <= 9; ++x) { cells[cell_idx(x, y, z)] = TUNNEL; }
			}
		}
		++gen;
		ox.clear();
		oy.clear();
		oz.clear();
		opipe.clear();
		okind.clear();
		run_a.clear();
		run_b.clear();
		filled = 0;
		phase = 0;
		phase_t = 0.0;
		last_wipe = 0;
		for (int p = 0; p < npipes; ++p) { respawn(p); }
	}

	// ----------------------------------------------------------------- camera ---

	float ss(float x) {
		float c = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
		return c * c * (3.0 - 2.0 * c);
	}

	// a turn of `amt` on each bar line from bar b0 through b1, eased out over a beat
	float swing(float t, int b0, int b1, float amt) {
		float result = 0.0;
		for (int b = b0; b <= b1; ++b) {
			float s = (t - b * MUSIC_BAR) / MUSIC_BEAT;
			if (s <= 0.0) { continue; }
			float c = s > 1.0 ? 0.0 : 1.0 - s;
			result += amt * (1.0 - c * c * c);
		}
		return result;
	}

	// a hit's sweep: the hit envelope integrated, so the turn it leaves behind stays
	float sweep(float t, float at, float amt, float rate) {
		return t < at ? 0.0 : amt * (1.0 - math::exp(0.0 - (t - at) * rate));
	}

	void orbit(float az, float el, float dist, float ty, float lift) {
		cam_ex = dist * math::cos(el) * math::sin(az);
		cam_ey = ty + dist * math::sin(el);
		cam_ez = dist * math::cos(el) * math::cos(az);
		cam_yaw = az + 3.14159265;
		cam_pitch = lift - el;
		cam_dist = dist;
	}

	// generation one: low over the floor, orbiting up as it grows, swinging on the bar lines
	void cam_a(float t, float kick) {
		float k = ss(t / (MUSIC_BAR * 3.0));
		float az = a0 + 0.11 * t + swing(t, 1, 3, 0.26) + sweep(t, MUSIC_BAR * 3.0, 0.9, 2.2);
		orbit(az, 0.05 + 0.36 * k, (40.0 - 12.0 * k) * (1.0 - 0.05 * kick), 0.0, 0.1 - 0.05 * k);
		cam_lens = 1.0;
	}

	// generation two: from high above, craning down to the floor and the other way round
	void cam_c(float t, float kick) {
		float s = t - MUSIC_BAR * 4.0;
		float k = ss(s / (MUSIC_BAR * 3.0));
		float az = c0 - 0.1 * s - swing(t, 5, 6, 0.24) + sweep(t, MUSIC_BAR * 4.0, -0.3, 3.0);
		orbit(az, 0.66 - 0.6 * k, (31.0 - 9.0 * k) * (1.0 - 0.05 * kick), -1.0 * k, 0.04 + 0.1 * k);
		cam_lens = 1.0;
	}

	// the flight down the tunnel toward the sun, lunging on every kick
	void cam_d(float t, float kick) {
		float s = t - MUSIC_BAR * 7.65;
		cam_ex = 0.5 + 0.35 * math::sin(t * 2.1);
		cam_ey = -1.5 + 0.25 * math::sin(t * 1.7);
		cam_ez = 7.5 - s * 7.2 - 1.1 * kick;
		cam_yaw = 3.14159265 + 0.07 * math::sin(t * 1.3);
		cam_pitch = 0.03 + 0.03 * math::sin(t * 0.9);
		cam_lens = 0.72;
		cam_dist = 30.0;
	}

	// out the far side: swing round to look back, crane up into the riser, and on the surge
	// sweep round to the sun
	void cam_e(float t, float kick) {
		float s = t - MUSIC_BAR * 9.0;
		float hit = cue == null ? 0.0 : cue.hit(t);
		float az = 3.14159265 + 0.12 * s + sweep(t, MUSIC_BAR * 10.0, 2.5, 1.8);
		float el = 0.12 + 0.36 * ss(s / MUSIC_BAR) - 0.38 * ss((s - MUSIC_BAR) / MUSIC_BAR);
		float dist = 12.0 + 12.0 * ss(s / MUSIC_BAR) + 7.0 * ss((s - MUSIC_BAR) / MUSIC_BAR) + 4.0 * hit;
		orbit(az, el, dist * (1.0 - 0.05 * kick), 0.0, 0.08 + 0.06 * ss((s - MUSIC_BAR) / MUSIC_BAR));
		cam_lens = 1.0;
	}

	void cam_section(int which, float t, float kick) {
		if (which == 0) { cam_a(t, kick); }
		else if (which == 1) { cam_c(t, kick); }
		else if (which == 2) { cam_d(t, kick); }
		else { cam_e(t, kick); }
	}

	// the path: each section's camera, blended into the next across its seam (the wipe, the
	// dive into the tunnel, the swing round)
	void camera(float t, float kick) {
		float bar = t / MUSIC_BAR;
		if (bar < 3.5) { cam_a(t, kick); }
		else if (bar < 4.0) { cam_a(t, kick); blend_toward(1, t, kick, ss((bar - 3.5) / 0.5)); }
		else if (bar < 7.0) { cam_c(t, kick); }
		else if (bar < 7.6) { cam_c(t, kick); blend_toward(2, t, kick, ss((bar - 7.0) / 0.6)); }
		else if (bar < 9.0) { cam_d(t, kick); }
		else if (bar < 9.3) { cam_d(t, kick); blend_toward(3, t, kick, ss((bar - 9.0) / 0.3)); }
		else { cam_e(t, kick); }
	}

	// mix the section camera already set with section `other`'s, `k` of the way toward `other`
	void blend_toward(int other, float t, float kick, float k) {
		float ex = cam_ex; float ey = cam_ey; float ez = cam_ez;
		float yw = cam_yaw; float pt = cam_pitch; float ln = cam_lens; float ds = cam_dist;
		cam_section(other, t, kick);
		cam_ex = ex + (cam_ex - ex) * k;
		cam_ey = ey + (cam_ey - ey) * k;
		cam_ez = ez + (cam_ez - ez) * k;
		cam_yaw = yw + (cam_yaw - yw) * k;
		cam_pitch = pt + (cam_pitch - pt) * k;
		cam_lens = ln + (cam_lens - ln) * k;
		cam_dist = ds + (cam_dist - ds) * k;
	}

	// ------------------------------------------------------------- rendering ---

	// every voxel (and its mirror image under the floor) into camera space
	void project_all() {
		int n = ox.size();
		if (cvx.size() < n) {
			cvx.resize(n, 0.0); cvy.resize(n, 0.0); cvz.resize(n, 0.0);
			cmx.resize(n, 0.0); cmy.resize(n, 0.0); cmz.resize(n, 0.0);
		}
		float ex = PD_EX; float ey = PD_EY; float ez = PD_EZ;
		float yf2 = 2.0 * PD_YF;
		for (int i = 0; i < n; ++i) {
			float px = ox[i] - ex;
			float py = oy[i] - ey;
			float pz = oz[i] - ez;
			float xc = px * PD_RX + py * PD_RY + pz * PD_RZ;
			float yc = px * PD_UX + py * PD_UY + pz * PD_UZ;
			float zc = px * PD_FX + py * PD_FY + pz * PD_FZ;
			cvx[i] = xc;
			cvy[i] = yc;
			cvz[i] = zc;
			float dm = yf2 - 2.0 * oy[i];
			cmx[i] = xc + dm * PD_RY;
			cmy[i] = yc + dm * PD_UY;
			cmz[i] = zc + dm * PD_FY;
		}
	}

	// one capsule impostor from voxel ia to voxel ib (a ball when they are one), clipped to
	// the near plane, into the frame's list
	void emit(int ia, int ib, int mir, float rad, int base, float spec, int lift, float fog_off) {
		float xa = mir == 0 ? cvx[ia] : cmx[ia];
		float ya = mir == 0 ? cvy[ia] : cmy[ia];
		float za = mir == 0 ? cvz[ia] : cmz[ia];
		float xb = mir == 0 ? cvx[ib] : cmx[ib];
		float yb = mir == 0 ? cvy[ib] : cmy[ib];
		float zb = mir == 0 ? cvz[ib] : cmz[ib];
		if (za < NEAR && zb < NEAR) { return; }
		if (za < NEAR) {
			float k = (NEAR - za) / (zb - za);
			xa += (xb - xa) * k; ya += (yb - ya) * k; za = NEAR;
		} else if (zb < NEAR) {
			float k2 = (NEAR - zb) / (za - zb);
			xb += (xa - xb) * k2; yb += (ya - yb) * k2; zb = NEAR;
		}
		float aoz = 1.0 / za;
		float boz = 1.0 / zb;
		float cx = w * 0.5;
		float cy = h * 0.5;
		float ax = cx + xa * foc * aoz;
		float ay = cy - ya * foc * aoz;
		float bx = cx + xb * foc * boz;
		float by = cy - yb * foc * boz;
		float ra = rad * foc * aoz;
		float rb = rad * foc * boz;
		ra = ra < 0.7 ? 0.7 : ra;
		rb = rb < 0.7 ? 0.7 : rb;
		float rm = ra > rb ? ra : rb;
		int x0 = math::ifloor((ax < bx ? ax : bx) - rm);
		int x1 = math::ifloor((ax > bx ? ax : bx) + rm);
		int y0 = math::ifloor((ay < by ? ay : by) - rm);
		int y1 = math::ifloor((ay > by ? ay : by) + rm);
		if (x1 < 0 || x0 >= w || y1 < 0 || y0 >= h) { return; }
		float abx = bx - ax;
		float aby = by - ay;
		float l2 = abx * abx + aby * aby;
		float fog = 1.25 - ((za < zb ? za : zb) - fog_off) * 0.018;
		PD_AX.push(ax);
		PD_AY.push(ay);
		PD_ABX.push(abx);
		PD_ABY.push(aby);
		PD_IL2.push(l2 > 0.000001 ? 1.0 / l2 : 0.0);
		PD_RA.push(ra);
		PD_DR.push(rb - ra);
		PD_ZA.push(aoz);
		PD_DZ.push(boz - aoz);
		PD_G.push(rad * aoz * boz);
		PD_FOG.push(fog > 1.0 ? 1.0 : (fog < 0.35 ? 0.35 : fog));
		PD_SPEC.push(spec);
		PD_X0.push(x0 < 0 ? 0 : x0);
		PD_X1.push(x1 >= w ? w - 1 : x1);
		PD_Y0.push(y0 < 0 ? 0 : y0);
		PD_Y1.push(y1 >= h ? h - 1 : y1);
		PD_BASE.push(base);
		PD_MIR.push(mir);
		PD_LV.push(lift);
	}

	void clear_prims() {
		PD_AX.clear(); PD_AY.clear(); PD_ABX.clear(); PD_ABY.clear(); PD_IL2.clear();
		PD_RA.clear(); PD_DR.clear(); PD_ZA.clear(); PD_DZ.clear(); PD_G.clear();
		PD_FOG.clear(); PD_SPEC.clear(); PD_X0.clear(); PD_X1.clear(); PD_Y0.clear();
		PD_Y1.clear(); PD_BASE.clear(); PD_MIR.clear(); PD_LV.clear();
	}

	// the white-hot surge of bar 10: a front racing along every pipe in growth order, embers behind
	float surge_heat(int i, int n) {
		float s = tt - MUSIC_BAR * 10.0;
		if (s < 0.0) { return 0.0; }
		float front = s / (MUSIC_BAR * 0.8) * n;
		float band = n * 0.14 + 6.0;
		float off = front - i;
		if (off < 0.0 - band) { return 0.0; }
		if (off < band) { return 1.0 - math::abs(off) / band; }
		return 0.5 * math::exp(0.0 - (off - band) / (n * 0.2));
	}

	// the sculpture as impostors: a capsule per straight run, a ball per elbow, eye and tip,
	// then each one's mirror image for the glossy floor
	void emit_sculpture(float fog_off) {
		clear_prims();
		bool surge = gen == 1 && tt >= MUSIC_BAR * 10.0;
		int n = ox.size();
		int nr = run_a.size();
		for (int mir = 0; mir < 2; ++mir) {
			for (int r = 0; r < nr; ++r) {
				int ia = run_a[r];
				int ib = run_b[r];
				int base = pipe_base + opipe[ib] * 16;
				int lift = 0;
				if (surge) {
					float heat = surge_heat((ia + ib) / 2, n);
					if (heat > 0.3) { base = hot_ramp; }
					lift = math::itrunc(heat * 8.0);
				}
				emit(ia, ib, mir, 0.4, base, 0.55, lift, fog_off);
			}
			for (int i = 0; i < n; ++i) {
				int kind = okind[i];
				if (kind == 0) { continue; }
				int p = opipe[i];
				int base2 = kind == 2 ? egg_base + ((flick + p + math::itrunc(ox[i] * 3.0 + oz[i] + 40.0)) % 6) * 16 : pipe_base + p * 16;
				int lift2 = 0;
				if (surge) {
					float heat2 = surge_heat(i, n);
					if (heat2 > 0.3) { base2 = hot_ramp; }
					lift2 = math::itrunc(heat2 * 8.0);
				}
				emit(i, i, mir, 0.58, base2, 0.85, lift2, fog_off);
			}
			if (phase == 0) {
				for (int p2 = 0; p2 < npipes; ++p2) {
					int hi = cell_ord[cell_idx(hx[p2], hy[p2], hz[p2])];
					if (hi < n) { emit(hi, hi, mir, 0.52, hot_ramp, 0.9, 6, fog_off); }
				}
			}
		}
	}

	// counting sort of the impostors into bands of rows
	void bin_prims() {
		int nb = PD_BANDS.size();
		var& start = PD_BSTART;
		for (int b = 0; b <= nb; ++b) { start[b] = 0; }
		int np = PD_Y0.size();
		int bh = PD_BH;
		for (int p = 0; p < np; ++p) {
			int b1 = PD_Y1[p] / bh;
			for (int b = PD_Y0[p] / bh; b <= b1; ++b) { start[b + 1] += 1; }
		}
		for (int b2 = 0; b2 < nb; ++b2) { start[b2 + 1] += start[b2]; }
		int total = start[nb];
		if (PD_BIDX.size() < total) { PD_BIDX.resize(total, 0); }
		array<int> fill = [];
		for (int b3 = 0; b3 < nb; ++b3) { fill.push(start[b3]); }
		var& idx = PD_BIDX;
		for (int p2 = 0; p2 < np; ++p2) {
			int b4 = PD_Y1[p2] / bh;
			for (int b = PD_Y0[p2] / bh; b <= b4; ++b) {
				idx[fill[b]] = p2;
				fill[b] += 1;
			}
		}
	}

	// the tips' sparks: from each tip one born on every sixteenth step, each a closed-form
	// arc from its birth (so a replay draws the same sparks)
	void draw_sparks(var& grid) {
		if (phase != 0) { return; }
		int n = ox.size();
		int now = math::itrunc(tt / MUSIC_STEP);
		for (int p = 0; p < npipes; ++p) {
			int hi = cell_ord[cell_idx(hx[p], hy[p], hz[p])];
			if (hi >= n || cvz[hi] <= NEAR) { continue; }
			float iz = 1.0 / cvz[hi];
			float sx = w * 0.5 + cvx[hi] * foc * iz;
			float sy = h * 0.5 - cvy[hi] * foc * iz;
			float reach = 0.5 + iz * 15.0;
			for (int k = 0; k < 5; ++k) {
				int born = now - k;
				float age = tt - born * MUSIC_STEP;
				if (age < 0.0) { continue; }
				int hs = ((p * 7919 + born * 104729) & 1048575) * 1274126177;
				float vx = (((hs >> 4) & 255) / 255.0 - 0.5) * w * 0.5 * reach;
				float vy = (0.0 - ((hs >> 12) & 255) / 255.0 * h * 0.45) * reach;
				int ix = math::ifloor(sx + vx * age);
				int iy = math::ifloor(sy + vy * age + h * 1.2 * age * age * reach);
				if (ix < 0 || ix >= w || iy < 0 || iy >= h) { continue; }
				int heat = 7 - k - ((hs >> 20) & 1);
				grid[iy * w + ix] = hot_base + (heat < 2 ? 2 : heat);
			}
		}
	}

	void render(var& grid) override {
		flick = math::itrunc(tt / MUSIC_STEP);
		if (phase == 2) {
			// generation over: a CRT beam sweeps down and leaves the set behind it
			int target = math::itrunc(phase_t * h / (MUSIC_BEAT * 2.0));
			last_wipe = target > h ? h : target;
		}
		float kick = cue == null ? 0.0 : cue.kick_env(tt);
		float snare = cue == null ? 0.0 : cue.snare_env(tt);
		float rise = cue == null ? 0.0 : cue.riser(tt);
		float hitv = cue == null ? 0.0 : cue.hit(tt);
		camera(tt, kick);
		foc = (w < h * 1.3 ? w : h * 1.3) * 1.02 * cam_lens * (1.0 + kick * 0.05 + rise * 0.06);
		// the roll: a slow sway, banked in the tunnel, punched on each hit
		float bar = tt / MUSIC_BAR;
		float bank = bar > 7.2 && bar < 9.3 ? 0.13 * math::sin(tt * 1.15) : 0.0;
		float roll = 0.045 * math::sin(tt * 0.6 + gen) + bank + hitv * 0.1 * (math::ifloor(bar) % 2 == 0 ? 1.0 : -1.0);
		float cp = math::cos(cam_pitch);
		float fx = cp * math::sin(cam_yaw);
		float fy = math::sin(cam_pitch);
		float fz = cp * math::cos(cam_yaw);
		float rl = 1.0 / math::sqrt(fx * fx + fz * fz);
		float rx = 0.0 - fz * rl;
		float rz = fx * rl;
		float ux = 0.0 - rz * fy;
		float uy = rz * fx - rx * fz;
		float uz = rx * fy;
		float cr = math::cos(roll);
		float sr = math::sin(roll);
		PD_W = w;
		PD_H = h;
		PD_F = foc;
		PD_EX = cam_ex;
		PD_EY = cam_ey < PD_YF + 0.4 ? PD_YF + 0.4 : cam_ey;
		PD_EZ = cam_ez;
		PD_FX = fx; PD_FY = fy; PD_FZ = fz;
		PD_RX = rx * cr + ux * sr; PD_RY = uy * sr; PD_RZ = rz * cr + uz * sr;
		PD_UX = ux * cr - rx * sr; PD_UY = uy * cr; PD_UZ = uz * cr - rz * sr;
		PD_SCROLL = scroll;
		PD_SELC = 0.079;
		PD_SR = 0.226;
		PD_SY = math::sin(PD_SELC);
		PD_SZ = 0.0 - math::cos(PD_SELC);
		PD_SCOS = math::cos(PD_SR);
		PD_SUNK = (w < h * 1.3 ? w : h * 1.3) * 1.02 * 0.42;
		PD_SKY = sky_base;
		PD_SUN = sun_base;
		PD_SUNSHIFT = (cue == null ? 0 : cue.chord(tt) % suns) * 16;
		PD_FLOOR = floor_base;
		PD_GRID = grid_base;
		PD_GRIDKICK = math::itrunc(kick * 2.5);
		// the glint: a bright diagonal band crosses every pipe after a fill or the surge,
		// over a flash that lifts the whole frame and decays
		float since = tt - glint_t;
		PD_GLINT = since >= 0.0 && since < 0.9 ? since / 0.9 * (w + h) * 1.3 - h * 0.3 : -999.0;
		PD_LIFT = (since >= 0.0 && since < 0.35 ? math::itrunc((0.35 - since) * 20.0) : 0) + math::itrunc(snare * 3.5);
		PD_WIPE = phase == 2 ? last_wipe : 0;
		PD_BEAM = phase == 2 ? last_wipe : -9;
		project_all();
		emit_sculpture(cam_dist > 30.0 ? cam_dist - 30.0 : 0.0);
		bin_prims();
		var bands = parallel_transform(PD_BANDS, pd_band);
		int nb = PD_BANDS.size();
		for (int b = 0; b < nb; ++b) {
			var& row = bands[b];
			int o = b * PD_BH * w;
			int m = row.size();
			for (int q = 0; q < m; ++q) { grid[o + q] = row[q]; }
		}
		draw_sparks(grid);
	}
}
