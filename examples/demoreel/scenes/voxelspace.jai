// CANYONRUN — the Comanche (1992) VoxelSpace algorithm, all script, flown like
// a drone shot. A 256x256 wrapping heightmap is grown at build time by
// diamond-square (pure integer hashing, no host rng in the hot loop), a river
// canyon is carved along a wrapping meander and the whole land is terraced into
// mesas and stepped cliffs. The colormap is baked with real light: a low
// golden-hour sun casts shadows marched across the height field, slopes take
// diffuse light from their normals, hollows darken by ambient occlusion (the
// height field against its own blur), cliffs show banded strata, the canyon
// floor greens and the rims carry scrub. Rendering is the classic front-to-back
// per-depth-line march with a per-column y-buffer: for each of 120 depth steps
// the camera frustum line is interpolated across the screen in 16.16 fixed
// point (integer adds only — the inner loop has ZERO host calls and no float
// math), each sample projects to a screen row, and only rows above the y-buffer
// are filled. Distance haze is 12 precomputed palette banks per terrain color.
// The river mirrors the sky about each column's horizon, broken by ripples, with
// the sun's glint running down it; the sky above is a perspective cloud deck
// lit by the setting sun. The camera skims the river between the walls, banks
// hard through the bends, climbs out over the rim through bar three to the reveal
// on bar four's downbeat (the sun flaring with the hit) and dives back in from bar
// six to skim the river home. From bar nine the gorge pinches into a slot canyon
// between towering shadowed walls, its river racing white for a lip; on bar
// eleven's downbeat the drone shoots over the falls into a wide basin and drops to
// skim a lake mirroring the sky, then climbs out of the basin and turns into the
// sun as it blazes over the rim. The kick nods the nose and punches the lens in,
// the snare leans the drone, each hit punches harder. Over the
// terrain: dust streaks rushing past the lens (longer with speed, pumping on the
// kick), a flock of birds beating across the sky, and on each hit a fan of god rays
// bursting from the sun across the sky.

int CR_N = 512;
array<int> CR_H = [];       // heights, shared with the parallel light bake
array<int> CR_BLUR = [];    // the heights blurred: the cavity test for ambient occlusion
array<int> CR_RIV = [];     // per row: the river's centre column
float CR_LX = 0.0;          // toward the sun: horizontal unit vector, elevation sine,
float CR_LY = 1.0;          // and the height gained per texel along it
float CR_LZ = 0.3;
float CR_RISE = 1.4;
int CR_LAKE = 14;           // the lake below the falls: its water level
int CR_LIP = 424;           // the row where the slot canyon ends and the river falls into the lake
int CR_SHORE = 508;         // the row past the lake's far shore where the river runs on

// one row of the colormap: material * 8 + light level 0..7
function canyon_light_row(int y) -> array<int> {
	array<int> out = [];
	int n = CR_N;
	int m = n - 1;
	float lx = CR_LX;
	float ly = CR_LY;
	float lz = CR_LZ;
	float rise = CR_RISE;
	float lh = math::sqrt(1.0 - lz * lz);
	int rc = CR_RIV[y];
	int row = y * n;
	int rup = ((y + 1) & m) * n;
	int rdn = ((y - 1) & m) * n;
	for (int x = 0; x < n; ++x) {
		int hv = CR_H[row + x];
		// a texel is 2.4 height units across
		float gx = (CR_H[row + ((x + 1) & m)] - CR_H[row + ((x - 1) & m)]) * 0.2083;
		float gy = (CR_H[rup + x] - CR_H[rdn + x]) * 0.2083;
		float nl = math::sqrt(gx * gx + gy * gy + 1.0);
		float diff = ((0.0 - gx) * lx * lh + (0.0 - gy) * ly * lh + lz) / nl;
		diff = diff < 0.0 ? 0.0 : diff;
		// the sun's shadow: march toward it, keep the deepest the ground rises above the ray
		float over = 0.0;
		float sxk = x + 0.5;
		float syk = y + 0.5;
		float thr = hv + 2.0;
		for (int k = 0; k < 44; ++k) {
			sxk += lx * 2.5;
			syk += ly * 2.5;
			thr += rise * 2.5;
			if (thr > 262.0) { break; }
			float ex = CR_H[(math::ifloor(syk) & m) * n + (math::ifloor(sxk) & m)] - thr;
			if (ex > over) {
				over = ex;
				if (over > 14.0) { break; }
			}
		}
		float lit = 1.0 - over / 14.0;
		lit = lit < 0.0 ? 0.0 : lit;
		float cav = CR_BLUR[row + x] - hv;
		float ao = 1.0 - cav * 0.011;
		ao = ao < 0.42 ? 0.42 : (ao > 1.08 ? 1.08 : ao);
		float light = (0.26 + 0.12 / nl) * ao + 1.02 * diff * lit;
		int grain = ((((x * 73856093 + y * 19349663) & 1048575) * 1274126177) >> 12) & 3;
		int lv = math::itrunc(light * 6.4 + grain * 0.4 - 0.3);
		lv = lv < 0 ? 0 : (lv > 7 ? 7 : lv);
		int dxr = x - rc;
		dxr = dxr > 256 ? dxr - 512 : dxr;
		dxr = dxr < -256 ? dxr + 512 : dxr;
		int ad = dxr < 0 ? 0 - dxr : dxr;
		float slope = math::sqrt(gx * gx + gy * gy);
		int warp = math::itrunc(14.0 * math::sin(x * 0.036 + y * 0.024) + 7.0 * math::sin(x * 0.095 - y * 0.065));
		int layer = ((hv * 4 + warp + 400) / 38) % 10;
		int mat = 4 + layer;
		if (ad < 10 && y >= CR_LIP - 10 && y < CR_LIP && hv > CR_LAKE) { mat = 15; }   // whitewater racing for the lip
		else if ((ad < 10 && (y < CR_LIP || y >= CR_SHORE)) || hv <= CR_LAKE) { mat = 0; }
		else if (ad < 15 || hv < CR_LAKE + 9) { mat = 1; }
		else if (hv > 236) { mat = 15; }
		else if (slope < 0.32 && hv < 92) { mat = 2; }
		else if (slope < 0.36 && hv >= 172) { mat = 14; }
		else if (slope < 0.28) { mat = 3; }
		out.push(mat * 8 + lv);
	}
	return out;
}

class CanyonRun : Scene {
	var tmap = [];         // packed terrain: height * 256 + colour index (material * 8 + light)
	var clouds = [];       // 128x128 cloud density 0..7
	var ybuf = [];         // per-column y-buffer
	var rolloff = [];      // per-column horizon row with the bank folded in
	var zs = [];           // per-step depth, in texels
	var invz = [];         // per-step projection scale, 16.16 fixed
	var hazes = [];        // per-step haze bank
	var dzs = [];          // per-step height per screen row, 16.16 fixed: the face's height at a row
	var jitter = [];       // per-step, per-column sub-texel offsets: blocks break into grain
	var strata = [];       // by height: the rock layer's palette offset on a cliff face
	int msz = 512;
	int mmask = 511;
	int tbase = 0;         // terrain: (material * 8 + light) * nhaze + haze
	int skyb = 0;          // sky by rows above the horizon
	int cloudb = 0;        // ... and by cloud density
	int wref = 0;          // the river's reflection, by rows below the horizon
	int glintb = 0;
	int sunb = 0;
	int nsteps = 120;
	int nhaze = 12;
	int nsky = 0;
	float posx = 256.0;
	float posy = 0.0;
	float dirx = 0.0;
	float diry = 1.0;
	float planex = 0.0;
	float planey = 0.0;
	float pdx = 0.0;
	float pdy = 1.0;
	float camhf = 90.0;
	float climb = 0.0;
	float roll = 0.0;
	float ph1 = 0.0;
	float ph2 = 0.0;
	int camh = 90;
	int horizon = 12;
	int frame = 0;
	float tt = 0.0;
	array<float> dust_x = [];   // dust motes in camera space: across, up, depth
	array<float> dust_y = [];
	array<float> dust_z = [];
	int dust_ink = 0;
	int bird_ink = 0;
	float speed = 44.0;

	void build() override {
		name = "CANYONRUN";
		handle = "golden hour . strata, shadows, a river, 1992";
		nsky = h;
		// the sky by height above the horizon: a warm band at the horizon deepening to dusk
		var sky_stops = [[255.0, 208.0, 150.0], [250.0, 164.0, 112.0], [214.0, 116.0, 120.0], [120.0, 76.0, 136.0],
			[46.0, 40.0, 104.0], [16.0, 18.0, 60.0]];
		skyb = pal.gradient(nsky, sky_stops);
		// clouds: each sky row mixed toward a sunset-lit cloud colour, 7 densities
		cloudb = pal.size();
		for (int k = 0; k < nsky; ++k) {
			int sc = pal.rgb[skyb + k];
			float sr = sc >> 16;
			float sg = (sc >> 8) & 255;
			float sb = sc & 255;
			float f = k / (nsky - 1.0);
			float cr = 255.0 - f * 70.0;
			float cg = 178.0 - f * 40.0;
			float cb = 150.0 + f * 30.0;
			for (int d = 1; d <= 7; ++d) {
				float a = d / 7.0;
				float shade = 1.0 - (d > 4 ? (d - 4) * 0.12 : 0.0);
				pal.add(sr + (cr * shade - sr) * a, sg + (cg * shade - sg) * a, sb + (cb * shade - sb) * a);
			}
		}
		// the river's mirror: the sky darkened and cooled; the glint: the sun on the water
		wref = pal.size();
		for (int k2 = 0; k2 < nsky; ++k2) {
			int wc = pal.rgb[skyb + (k2 < nsky ? k2 : nsky - 1)];
			float wr = wc >> 16;
			float wg = (wc >> 8) & 255;
			float wb = wc & 255;
			pal.add(12.0 + wr * 0.52, 30.0 + wg * 0.56, 44.0 + wb * 0.66);
		}
		glintb = pal.gradient(4, [[255.0, 196.0, 120.0], [255.0, 236.0, 190.0], [255.0, 255.0, 245.0]]);
		dust_ink = pal.gradient(3, [[150.0, 120.0, 100.0], [220.0, 190.0, 150.0], [255.0, 240.0, 210.0]]);
		bird_ink = pal.add(24.0, 14.0, 28.0);
		for (int m = 0; m < 70; ++m) {
			dust_x.push((rng.next(2000) - 1000) * 0.001);
			dust_y.push((rng.next(2000) - 1000) * 0.0006);
			dust_z.push(0.05 + rng.next(1000) * 0.001);
		}
		// terrain: 16 materials x 8 light levels x 12 haze banks; light warms, shadow cools
		var mats = [[40.0, 70.0, 90.0], [150.0, 124.0, 86.0], [70.0, 104.0, 46.0], [124.0, 112.0, 64.0],
			[186.0, 92.0, 56.0], [214.0, 146.0, 92.0], [150.0, 68.0, 54.0], [228.0, 186.0, 136.0], [176.0, 108.0, 76.0],
			[118.0, 76.0, 74.0], [206.0, 118.0, 66.0], [238.0, 210.0, 164.0], [162.0, 92.0, 62.0], [196.0, 152.0, 112.0],
			[206.0, 160.0, 104.0], [232.0, 216.0, 196.0]];
		int hz0 = pal.rgb[skyb];
		float fogr = (hz0 >> 16) * 0.92;
		float fogg = ((hz0 >> 8) & 255) * 0.88;
		float fogb = (hz0 & 255) * 0.95;
		tbase = pal.size();
		for (int mt = 0; mt < 16; ++mt) {
			var mc = mats[mt];
			float br = mc[0];
			float bg = mc[1];
			float bb = mc[2];
			for (int lv = 0; lv < 8; ++lv) {
				float k3 = 0.16 + lv * 0.15;
				float warm = lv > 4 ? (lv - 4) / 3.0 : 0.0;
				float cool = lv < 3 ? (3 - lv) / 3.0 : 0.0;
				float cr2 = br * k3 + 46.0 * warm + 14.0 * cool;
				float cg2 = bg * k3 + 18.0 * warm + 16.0 * cool;
				float cb2 = bb * k3 + 34.0 * cool;
				for (int hq = 0; hq < nhaze; ++hq) {
					float hf = hq / (nhaze - 1.0) * 0.64;
					pal.add(cr2 + (fogr - cr2) * hf, cg2 + (fogg - cg2) * hf, cb2 + (fogb - cb2) * hf);
				}
			}
		}
		sunb = pal.gradient(6, [[255.0, 150.0, 90.0], [255.0, 200.0, 140.0], [255.0, 248.0, 220.0]]);
		grow_land();
		bake_clouds();
		// depth tables: geometric steps (in old 128-map units, two texels each), projection scale, haze bank
		float z = 1.5;
		float hsc = h * 0.085;
		for (int i = 0; i < nsteps; ++i) {
			zs.push(z * 4.0);
			dzs.push(math::itrunc(z * 65536.0 / hsc));
			invz.push(math::itrunc(hsc * 65536.0 / z));
			float zr = i / (nsteps - 1.0);
			int hzv = math::itrunc(math::pow(zr, 2.0) * (nhaze + 0.5));
			hazes.push(hzv > nhaze - 1 ? nhaze - 1 : hzv);
			z *= 1.0365;
		}
		ybuf = gfx.make_grid(w);
		rolloff = gfx.make_grid(w);
		jitter = gfx.make_grid(nsteps * w);
		for (int j = 0; j < nsteps * w; ++j) {
			jitter[j] = (((((j * 2654435) & 1048575) * 40503) >> 4 & 65535) - 32768) * 3 / 4;
		}
		// the rock ladder: bands 4..12 units thick in shuffled strata colours,
		// some topped by a dark seam
		strata = gfx.make_grid(320);
		int hb = 0;
		int band = 0;
		while (hb < 320) {
			int hsh = ((band * 7919 + (seed & 255) * 131) & 65535) * 40503 >> 8;
			int thick = 4 + hsh % 9;
			int mat = 4 + (hsh / 16) % 10;
			bool seam = (hsh / 256) % 3 == 0;
			for (int k = 0; k < thick && hb < 320; ++k) {
				strata[hb] = ((seam && k == thick - 1 ? 9 : mat) * 8) * nhaze;
				++hb;
			}
			++band;
		}
	}

	// diamond-square with wraparound; displacement comes from an inline integer
	// hash (masked before the multiply so checked overflow never fires); then the
	// river is carved, the land terraced, and the colormap baked with light
	void grow_land() {
		int n = msz;
		int mm = mmask;
		var hm = gfx.make_grid(n * n);
		int sd = seed & 65535;
		hm[0] = 120 + (sd & 63);
		int step = n;
		int amp = 110;
		while (step > 1) {
			int half = step / 2;
			for (int dy = half; dy < n; dy += step) {
				int rw0 = ((dy - half) & mm) * n;
				int rw1 = ((dy + half) & mm) * n;
				int rwc = dy * n;
				for (int dx = half; dx < n; dx += step) {
					int x0 = dx - half;
					int x1 = (dx + half) & mm;
					int hsh = (dx * 73856093 + dy * 19349663 + step * 83492791 + sd * 2971) & 1048575;
					int d = (((hsh * 1274126177) / 4096) & 255) - 128;
					hm[rwc + dx] = (hm[rw0 + x0] + hm[rw0 + x1] + hm[rw1 + x0] + hm[rw1 + x1]) / 4 + d * amp / 128;
				}
			}
			for (int sy = 0; sy < n; sy += half) {
				int xoff = (sy % step) == 0 ? half : 0;
				int rws = sy * n;
				int rwu = ((sy - half) & mm) * n;
				int rwd = ((sy + half) & mm) * n;
				for (int sx = xoff; sx < n; sx += step) {
					int hsh2 = (sx * 19349663 + sy * 73856093 + step * 297121507 + sd * 7919) & 1048575;
					int d2 = (((hsh2 * 1274126177) / 4096) & 255) - 128;
					hm[rws + sx] = (hm[rws + ((sx - half) & mm)] + hm[rws + ((sx + half) & mm)] + hm[rwu + sx] + hm[rwd + sx]) / 4 + d2 * amp / 128;
				}
			}
			step = half;
			amp = amp * 11 / 20;
			amp = amp < 2 ? 2 : amp;
		}
		int hmin = 999999;
		int hmax = -999999;
		int nm = n * n;
		for (int q = 0; q < nm; ++q) {
			int hv0 = hm[q];
			hmin = hv0 < hmin ? hv0 : hmin;
			hmax = hv0 > hmax ? hv0 : hmax;
		}
		int range = hmax - hmin;
		range = range < 1 ? 1 : range;
		// the meander wraps (integer harmonics of 2*pi/256) so the flight loops seamlessly
		ph1 = rng.nextf() * 6.283185307;
		ph2 = rng.nextf() * 6.283185307;
		CR_RIV = [];
		for (int ry = 0; ry < n; ++ry) {
			CR_RIV.push(math::itrunc(256.0 + 104.0 * math::sin(ry * 0.01227184630 + ph1) + 44.0 * math::sin(ry * 0.03681553891 + ph2)));
		}
		// carve a U-shaped gorge 68 texels wide down to the floor, lift the land
		// around it, then terrace: flat benches broken by steep risers. Toward the end
		// of the loop the gorge pinches into a slot canyon between towering walls that
		// ends at a lip where the river falls into a lake in a wide basin
		int lake_x = CR_RIV[462];
		for (int cy = 0; cy < n; ++cy) {
			int rc = CR_RIV[cy];
			int rowc = cy * n;
			float slot = (cy - 250.0) / 50.0;
			slot = cy >= CR_LIP + 4 ? 0.0 : (slot < 0.0 ? 0.0 : (slot > 1.0 ? 1.0 : slot * slot * (3.0 - 2.0 * slot)));
			float ly = (cy - 462.0) / 38.0;
			ly = ly < 0.0 ? -ly : ly;
			for (int cx = 0; cx < n; ++cx) {
				float hv0 = 60.0 + (hm[rowc + cx] - hmin) * 196.0 / range;
				float hv = hv0;
				int dxr = cx - rc;
				dxr = dxr > 256 ? dxr - 512 : dxr;
				dxr = dxr < -256 ? dxr + 512 : dxr;
				float ad = dxr < 0 ? 0 - dxr : dxr;
				if (ad < 68.0) {
					float f = ad / 68.0;
					f = f * f * (3.0 - 2.0 * f);
					f = math::pow(f, 0.7);
					hv = 40.0 + (hv - 40.0) * f;
				}
				if (slot > 0.0) {
					float wall = hv0 + 80.0;
					wall = wall > 250.0 ? 250.0 : wall;
					float sh = wall;
					if (ad < 12.0) { sh = 40.0; }
					else if (ad < 20.0) { sh = 40.0 + (wall - 40.0) * math::pow((ad - 12.0) / 8.0, 0.6); }
					hv += (sh - hv) * slot;
				}
				if (ly < 1.2) {
					// the basin: a rounded lake, its shore climbing back to the land in a cliff
					int dxl = cx - lake_x;
					dxl = dxl > 256 ? dxl - 512 : dxl;
					dxl = dxl < -256 ? dxl + 512 : dxl;
					float lx = (dxl < 0 ? 0 - dxl : dxl) / 130.0;
					float q = lx * lx * lx + ly * ly * ly;
					if (q < 1.7) {
						float sf = (q - 1.0) / 0.7;
						sf = sf < 0.0 ? 0.0 : sf * sf * (3.0 - 2.0 * sf);
						hv = CR_LAKE + (hv - CR_LAKE) * sf;
					}
				}
				float tf = hv / 23.0;
				float fl = math::floor(tf);
				float fr = tf - fl;
				float rs = fr < 0.62 ? 0.0 : (fr - 0.62) / 0.38;
				rs = rs * rs * (3.0 - 2.0 * rs);
				float terr = (fl + rs) * 23.0;
				hv = hv * 0.3 + terr * 0.7;
				int hi = math::itrunc(hv);
				hi = hv <= CR_LAKE + 0.5 ? CR_LAKE : hi;
				hm[rowc + cx] = hi < 0 ? 0 : (hi > 255 ? 255 : hi);
			}
		}
		// the river's water sits flat in the channel
		for (int wy = 0; wy < n; ++wy) {
			if (wy >= CR_LIP && wy < CR_SHORE) { continue; }
			int rcw = CR_RIV[wy];
			for (int wd = -10; wd <= 10; ++wd) {
				int wi = wy * n + ((rcw + wd) & mm);
				if (hm[wi] > CR_LAKE) { hm[wi] = 40; }
			}
		}
		CR_H = hm;
		// the blur for occlusion: a separable 17-wide box by running sums, wrapping
		var tmp = gfx.make_grid(nm);
		for (int by = 0; by < n; ++by) {
			int rb = by * n;
			int sum = 0;
			for (int k = -8; k <= 8; ++k) { sum += hm[rb + (k & mm)]; }
			for (int bx = 0; bx < n; ++bx) {
				tmp[rb + bx] = sum / 17;
				sum += hm[rb + ((bx + 9) & mm)] - hm[rb + ((bx - 8) & mm)];
			}
		}
		var blur = gfx.make_grid(nm);
		for (int bx2 = 0; bx2 < n; ++bx2) {
			int sum2 = 0;
			for (int k2 = -8; k2 <= 8; ++k2) { sum2 += tmp[(k2 & mm) * n + bx2]; }
			for (int by2 = 0; by2 < n; ++by2) {
				blur[by2 * n + bx2] = sum2 / 17;
				sum2 += tmp[((by2 + 9) & mm) * n + bx2] - tmp[((by2 - 8) & mm) * n + bx2];
			}
		}
		CR_BLUR = blur;
		// the sun sits low ahead and to the left of the flight (which runs toward +y)
		float az = -0.62;
		float el = 0.27;
		CR_LX = math::sin(az);
		CR_LY = math::cos(az);
		CR_LZ = math::sin(el);
		CR_RISE = math::sin(el) / math::cos(el) * 2.4;
		CR_N = n;
		array<int> rows = [];
		for (int r = 0; r < n; ++r) { rows.push(r); }
		var lit = parallel_transform(rows, canyon_light_row);
		tmap = gfx.make_grid(nm);
		for (int ly = 0; ly < n; ++ly) {
			var& lr = lit[ly];
			int rl = ly * n;
			for (int lx = 0; lx < n; ++lx) { tmap[rl + lx] = hm[rl + lx] * 256 + lr[lx]; }
		}
	}

	// value-noise fbm, thresholded into a broken cloud deck of 7 densities
	void bake_clouds() {
		int cn = 128;
		array<float> lat = [];
		for (int i = 0; i < 32 * 32; ++i) { lat.push(rng.nextf()); }
		clouds = gfx.make_grid(cn * cn);
		for (int y = 0; y < cn; ++y) {
			for (int x = 0; x < cn; ++x) {
				float v = 0.0;
				float a = 0.55;
				for (int o = 0; o < 4; ++o) {
					int per = 4 << o;   // lattice cells across the tile: the deck wraps
					float fx = x * per / (cn * 1.0);
					float fy = y * per / (cn * 1.0);
					int ix = math::ifloor(fx);
					int iy = math::ifloor(fy);
					float ux = fx - ix;
					float uy = fy - iy;
					ux = ux * ux * (3.0 - 2.0 * ux);
					uy = uy * uy * (3.0 - 2.0 * uy);
					float c00 = lat[(iy % per) * 32 + (ix % per)];
					float c10 = lat[(iy % per) * 32 + ((ix + 1) % per)];
					float c01 = lat[((iy + 1) % per) * 32 + (ix % per)];
					float c11 = lat[((iy + 1) % per) * 32 + ((ix + 1) % per)];
					float top = c00 + (c10 - c00) * ux;
					float bot = c01 + (c11 - c01) * ux;
					v += (top + (bot - top) * uy) * a;
					a *= 0.5;
				}
				int d = math::itrunc((v - 0.42) * 20.0);
				clouds[y * cn + x] = d < 0 ? 0 : (d > 7 ? 7 : d);
			}
		}
	}

	int length_bars() override { return 12; }
	array<float> hit_times() override { return [MUSIC_BAR * 4.0, MUSIC_BAR * 6.0, MUSIC_BAR * 9.0, MUSIC_BAR * 11.0]; }
	array<int> hit_levels() override { return [2, 1, 1, 2]; }

	float ease(float a, float b, float t) {
		float k = (t - a) / (b - a);
		k = k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
		return k * k * (3.0 - 2.0 * k);
	}

	// the drone's altitude plan: skim the river, climb out over the rim through bar
	// three for the reveal on bar four, dive back into the gorge from bar six, skim again,
	// thread the slot canyon from bar nine, drop over the falls on bar eleven's downbeat
	// to skim the lake, and rise into the sunset at the end
	float plan_alt(float t) {
		float lo = 60.0 + 8.0 * math::sin(t * 1.3);
		float hi = 330.0;
		float up = ease(MUSIC_BAR * 3.0, MUSIC_BAR * 4.0, t);
		float down = ease(MUSIC_BAR * 6.0, MUSIC_BAR * 7.0, t);
		float result = lo + (hi - lo) * up * (1.0 - down);
		float lake = ease(MUSIC_BAR * 11.0, MUSIC_BAR * 11.4, t);
		result += (34.0 - result) * lake;
		return result + ease(MUSIC_BAR * 11.45, MUSIC_BAR * 12.0, t) * 270.0;
	}

	// how far the drone has turned from the river into the sun, climbing out of the basin
	float sun_turn(float t) { return ease(MUSIC_BAR * 11.55, MUSIC_BAR * 12.0, t); }

	float path_x(float u, float t) {
		return 256.0 + 104.0 * math::sin(u * 0.01227184630 + ph1) + 44.0 * math::sin(u * 0.03681553891 + ph2) + 4.0 * math::sin(t * 0.6 + 0.8);
	}

	void update(float t, float dt) override {
		tt = t;
		++frame;
		float sp = speed;
		float u = t * sp;
		posy = u;
		posx = path_x(u, t);
		// heading from a look-ahead point on the same path: the turns steer themselves
		float ahead = 26.0;
		float lx = path_x(u + ahead, t + ahead / sp);
		float ddx = lx - posx;
		float dl = math::sqrt(ddx * ddx + ahead * ahead);
		dirx = ddx / dl;
		diry = ahead / dl;
		// out over the lake the drone turns into the sun as it climbs
		float sunward = sun_turn(t);
		if (sunward > 0.0) {
			float hx = dirx + (CR_LX - dirx) * sunward;
			float hy = diry + (CR_LY - diry) * sunward;
			float hl = math::sqrt(hx * hx + hy * hy);
			dirx = hx / hl;
			diry = hy / hl;
		}
		planex = 0.0 - diry * 0.9;
		planey = dirx * 0.9;
		// bank into the turn: roll target from the heading's turn rate
		if (dt > 0.0001) {
			float cr = (pdx * diry - pdy * dirx) / dt;
			float rt = cr * 1.5;
			rt = rt > 0.55 ? 0.55 : rt;
			rt = rt < -0.55 ? -0.55 : rt;
			// a lean on the snare, to the left one bar and the right the next
			if (cue != null) { rt += cue.snare_env(t) * 0.09 * ((math::ifloor(t / MUSIC_BAR) & 1) == 0 ? 1.0 : -1.0); }
			float mixr = dt * 3.6;
			mixr = mixr > 1.0 ? 1.0 : mixr;
			roll += (rt - roll) * mixr;
		}
		pdx = dirx;
		pdy = diry;
		// altitude: the plan, floored by terrain clearance ahead
		int g0 = CR_H[((math::ifloor(posy) & mmask) * msz) + (math::ifloor(posx) & mmask)];
		int g1 = CR_H[((math::ifloor(posy + diry * 16.0) & mmask) * msz) + (math::ifloor(posx + dirx * 16.0) & mmask)];
		int g2 = CR_H[((math::ifloor(posy + diry * 40.0) & mmask) * msz) + (math::ifloor(posx + dirx * 40.0) & mmask)];
		if (t > MUSIC_BAR * 8.5) {
			// in the slot the clearance is read along the river itself, never off a bend into a wall
			g1 = CR_H[((math::ifloor(posy + 16.0) & mmask) * msz) + (math::ifloor(path_x(u + 16.0, t)) & mmask)];
			g2 = CR_H[((math::ifloor(posy + 40.0) & mmask) * msz) + (math::ifloor(path_x(u + 40.0, t)) & mmask)];
		}
		int gm = g0 > g1 ? g0 : g1;
		gm = g2 > gm ? g2 : gm;
		float targ = plan_alt(t);
		float floorh = gm + 22.0;
		targ = floorh > targ ? floorh : targ;
		float was = camhf;
		if (dt > 0.0001) {
			float mixa = dt * 3.0;
			mixa = mixa > 1.0 ? 1.0 : mixa;
			camhf += (targ - camhf) * mixa;
			float vz = (camhf - was) / dt;
			climb += (vz - climb) * (mixa > 0.5 ? 0.5 : mixa);
		}
		camh = math::itrunc(camhf);
		// pitch: the nose follows the climb and the dive
		float pit = climb * 0.0019;
		pit = pit > 0.16 ? 0.16 : pit;
		pit = pit < -0.24 ? -0.24 : pit;
		float kick = cue != null ? cue.kick_env(t) : 0.0;
		float nod = kick * 0.024;
		horizon = math::itrunc(h * (0.36 + pit + nod));
		// the kick punches the lens in
		float lens = 0.9 * (1.0 - kick * 0.07 - (cue != null ? cue.hit(t) * 0.08 : 0.0));
		planex = 0.0 - diry * lens;
		planey = dirx * lens;
		// the dust rushes at the lens; a mote that passes it is born again far ahead
		for (int m = 0; m < dust_z.size(); ++m) {
			dust_z[m] = dust_z[m] - dt * (0.9 + kick * 0.6);
			if (dust_z[m] < 0.04) { dust_z[m] = dust_z[m] + 1.0; }
		}
	}

	// field access resolves through the environment chain per hit — ~2x the cost
	// of a local slot — so every array the inner loop touches is bound to a
	// var& reference declaration (plain assignment would deep-copy the array)
	// and every hot scalar is copied to a local first
	void render(var& grid) override {
		int ww = w;
		int hh = h;
		int mm = mmask;
		int ms = msz;
		int tb = tbase;
		int cam = camh;
		int horiz = horizon;
		int ns = nsteps;
		int nh = nhaze;
		int wr = wref;
		int nsk = nsky;
		int fr = frame;
		float dx_ = dirx;
		float dy_ = diry;
		float px_ = planex;
		float py_ = planey;
		var& tm = tmap;
		var& yb = ybuf;
		var& ro = rolloff;
		var& zt = zs;
		var& izt = invz;
		var& hzt = hazes;
		var& dzt = dzs;
		var& jt = jitter;
		var& so = strata;
		int w2 = ww / 2;
		int rollfp = math::itrunc(roll * 65536.0);
		for (int x = 0; x < ww; ++x) {
			yb[x] = hh;
			ro[x] = horiz + rollfp * (x - w2) / 65536;   // horizon folded in
		}
		// the sun's column: where its bearing falls across the camera plane
		float sdot = CR_LX * dx_ + CR_LY * dy_;
		float scross = CR_LX * dy_ - CR_LY * dx_;
		int scol = sdot > 0.05 ? math::itrunc(w2 - scross / sdot / 0.9 * w2) : -9999;
		// +4096 keeps the fixed-point coords positive so /65536 floors exactly
		float bigx = posx + 4096.0;
		float bigy = posy + 4096.0;
		for (int s = 0; s < ns; ++s) {
			float z = zt[s];
			int iz = izt[s];
			int hz = hzt[s];
			int dz = dzt[s];
			int ji = s * ww;
			float plx = bigx + (dx_ - px_) * z;
			float ply = bigy + (dy_ - py_) * z;
			float prx = bigx + (dx_ + px_) * z;
			float pry = bigy + (dy_ + py_) * z;
			int rxf = math::itrunc(plx * 65536.0);
			int ryf = math::itrunc(ply * 65536.0);
			int dxf = math::itrunc((prx - plx) * 65536.0 / ww);
			int dyf = math::itrunc((pry - ply) * 65536.0 / ww);
			for (int x2 = 0; x2 < ww; ++x2) {
				int ybv = yb[x2];
				if (ybv > 0) {
					// a sub-texel jitter per column and step: magnified texels break into grain
					int jv = jt[ji + x2];
					int mi = (((ryf + jv) >> 16) & mm) * ms + (((rxf - jv) >> 16) & mm);   // coords kept positive: shift == floor
					int tv = tm[mi];
					int sy = (cam - (tv >> 8)) * iz / 65536 + ro[x2];
					if (sy < ybv) {
						sy = sy < 0 ? 0 : sy;
						int ci = tv & 255;
						int gi = sy * ww + x2;
						int yy = sy;
						if (ci < 8) {
							// water: the sky mirrored about this column's horizon, broken
							// by ripples, the sun's glint running down toward the camera
							int rox = ro[x2];
							int gd = x2 - scol;
							gd = gd < 0 ? 0 - gd : gd;
							while (yy < ybv) {
								int k = yy - rox;
								int rip = ((((x2 * 7919 + yy * 104729 + (fr / 2) * 15485863) & 1048575) * 1274126177) >> 12) & 1023;
								k += (rip & 3) - 1;
								k = k < 0 ? 0 : (k >= nsk ? nsk - 1 : k);
								int pix = wr + k;
								int band = 2 + k / 4;
								if (gd < band && (rip >> 2) < 70 - gd * 60 / band) { pix = glintb + ((rip >> 2) < 24 ? 3 : ((rip >> 2) < 48 ? 2 : 1)); }
								grid[gi] = pix;
								gi += ww;
								++yy;
							}
						} else if (ci >= 32 && ci < 112) {
							// a cliff: below its lip each row shows the rock layer at the
							// height that row looks at, lit as the face is
							// height that row looks at, lit as the face is, grained and darkening
							// toward the foot of a tall face
							int th = tv >> 8;
							int top = tb + ci * nh + hz;
							int lv3 = ci & 7;
							int lit = tb + lv3 * nh + hz;
							int rox3 = ro[x2];
							while (yy < ybv) {
								int hw = cam - (yy - rox3) * dz / 65536;
								hw = hw < 0 ? 0 : (hw > 319 ? 319 : hw);
								int pix3 = top;
								if (th - hw >= 3) {
									int gr = ((((x2 * 7919 + hw * 104729) & 1048575) * 1274126177) >> 14) & 3;
									int dim = (gr == 0 ? 1 : 0) + (th - hw > 46 ? 1 : 0);
									dim = dim > lv3 ? lv3 : dim;
									pix3 = lit + so[hw] - dim * nh;
								}
								grid[gi] = pix3;
								gi += ww;
								++yy;
							}
						} else {
							// open ground: one texel, grained per pixel so near ground is not a flat block
							int pi = tb + ci * nh + hz;
							int pd = (ci & 7) > 0 ? pi - nh : pi;
							while (yy < ybv) {
								int gr2 = ((((x2 * 7919 + yy * 104729 + mi) & 1048575) * 1274126177) >> 14) & 3;
								grid[gi] = gr2 == 0 ? pd : pi;
								gi += ww;
								++yy;
							}
						}
						yb[x2] = sy;
					}
				}
				rxf += dxf;
				ryf += dyf;
			}
		}
		// whatever the terrain never reached is sky: a cloud deck in perspective
		// (row k above the horizon looks out to distance ~1/k), then the sun
		var& cl = clouds;
		int cb = cloudb;
		int sb = skyb;
		float cz = hh * 0.085 * 260.0 * 0.25;
		float cu0 = posx * 0.25 + tt * 1.6;
		float cv0 = posy * 0.25 + tt * 0.7;
		// the sun flares with each hit and blazes as the drone turns into it at the end
		float flare = cue != null ? cue.hit(tt) : 0.0;
		float blaze = sun_turn(tt) * 0.75;
		flare = blaze > flare ? blaze : flare;
		float sr = hh * 0.065 * (1.0 + flare * 0.8);
		float halo = 5.5 + flare * 9.0;
		float syf = horiz - hh * 0.10;
		float sxf = scol;
		for (int x3 = 0; x3 < ww; ++x3) {
			int yb2 = yb[x3];
			int rox2 = ro[x3];
			float camx = 2.0 * x3 / (ww - 1.0) - 1.0;
			float rdx = dx_ + px_ * camx;
			float rdy = dy_ + py_ * camx;
			int gi2 = x3;
			for (int yy2 = 0; yy2 < yb2; ++yy2) {
				int k2 = rox2 - yy2;
				int pix2 = sb;
				if (k2 > 0) {
					int ks = k2 >= nsk ? nsk - 1 : k2;
					pix2 = sb + ks;
					float zc = cz / k2;
					int d = cl[(math::ifloor(cv0 + rdy * zc) & 127) * 128 + (math::ifloor(cu0 + rdx * zc) & 127)];
					if (k2 < 7) { d = d * k2 / 7; }
					if (d > 0) {
						pix2 = cb + ks * 7 + d - 1;
					} else {
						float ddx = x3 + 0.5 - sxf;
						float ddy = yy2 + 0.5 - syf;
						float dd = (ddx * ddx + ddy * ddy) / (sr * sr);
						if (dd < 1.0) { pix2 = sunb + 5; }
						else if (dd < halo) {
							int gl = math::itrunc((halo - dd) * 1.1);
							if (gl > 0) { pix2 = sunb + (gl > 4 ? 4 : gl - 1); }
						}
					}
				}
				grid[gi2] = pix2;
				gi2 += ww;
			}
		}
		god_rays(grid, sxf, syf, flare);
		birds(grid);
		dust(grid);
	}

	// on a hit, rays of the sun fan out across the sky (never over the land)
	void god_rays(var& grid, float sx, float sy, float flare) {
		if (flare < 0.05 || sx < -50.0 || sx > w + 50.0) { return; }
		var& yb = ybuf;
		float reach = h * (0.25 + flare * 0.7);
		for (int r = 0; r < 14; ++r) {
			float a = r * 0.4487989 + tt * 0.15;
			float cx = math::cos(a);
			float cy = math::sin(a);
			int steps = math::itrunc(reach);
			for (int k = 2; k < steps; ++k) {
				int x = math::itrunc(sx + cx * k);
				int y = math::itrunc(sy + cy * k * 0.8);
				if (x < 0 || x >= w || y < 0 || y >= h || y >= yb[x]) { continue; }
				int lv = math::itrunc((1.0 - k * 1.0 / steps) * flare * 4.0);
				if (lv > 0) { grid[y * w + x] = sunb + (lv > 3 ? 3 : lv); }
			}
		}
	}

	// a flock beating its wings across the sky, silhouettes against the sunset
	void birds(var& grid) {
		var& yb = ybuf;
		int span = w + 60;
		float size = h / 60.0 + 1.0;
		for (int b = 0; b < 11; ++b) {
			float fx = math::fmod(tt * (9.0 + b * 0.7) + b * 37.0, span * 1.0) - 30.0;
			float fy = horizon - h * 0.2 - (b % 4) * size * 2.2 + math::sin(tt * 1.3 + b) * size * 1.5;
			float flap = math::sin(tt * 11.0 + b * 1.7);
			int wing = math::itrunc(size * 2.2);
			for (int k = 0; k <= wing; ++k) {
				int dy = math::itrunc(k * 0.6 * flap) - (flap < 0.0 ? 0 : 0);
				bird_px(grid, yb, math::itrunc(fx) + k, math::itrunc(fy) - dy);
				bird_px(grid, yb, math::itrunc(fx) - k, math::itrunc(fy) - dy);
			}
		}
	}

	void bird_px(var& grid, var& yb, int x, int y) {
		if (x >= 0 && x < w && y >= 0 && y < h && y < yb[x]) { grid[y * w + x] = bird_ink; }
	}

	// dust streaks: each mote drawn from where it is to where it was, longer with speed
	void dust(var& grid) {
		float f = w * 0.45;
		float kick = cue != null ? cue.kick_env(tt) : 0.0;
		float trail = 0.05 + kick * 0.04;
		for (int m = 0; m < dust_z.size(); ++m) {
			float z = dust_z[m];
			float z2 = z + trail;
			int x0 = math::itrunc(w * 0.5 + dust_x[m] / z * f);
			int y0 = math::itrunc(horizon + dust_y[m] / z * f);
			int x1 = math::itrunc(w * 0.5 + dust_x[m] / z2 * f);
			int y1 = math::itrunc(horizon + dust_y[m] / z2 * f);
			int lv = z < 0.25 ? 2 : (z < 0.55 ? 1 : 0);
			int n = (x0 - x1 < 0 ? x1 - x0 : x0 - x1) + (y0 - y1 < 0 ? y1 - y0 : y0 - y1);
			n = n > 40 ? 40 : n;
			for (int k = 0; k <= n; ++k) {
				int x = n == 0 ? x0 : x1 + (x0 - x1) * k / n;
				int y = n == 0 ? y0 : y1 + (y0 - y1) * k / n;
				if (x >= 0 && x < w && y >= 0 && y < h) { grid[y * w + x] = dust_ink + lv; }
			}
		}
	}
}
