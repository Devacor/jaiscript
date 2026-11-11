// BOIDS — a starling murmuration over a lake at sunset that flies geometry.
// Fifteen hundred birds fly in three dimensions: each keeps its distance from
// its nearest neighbours and matches their heading (Reynolds' separation and
// alignment, the neighbours read from a uniform grid hashed fresh every frame,
// the steering done in parallel chunks) while it chases its own place in the
// figure the flock is flying. The first figure is the murmuration itself, a
// loose sheet that swings, stretches and folds; then, two bars apart, the
// flock pulls into a torus whose coils roll like a smoke ring, a double helix
// with its rungs, a figure-eight ribbon the birds stream round, a spiral
// galaxy, and a banded sphere hung in front of the sun.
//
// A figure is only a set of moving places. A bird flies to its place at a
// bird's speed, swirling in and holding to its neighbours on the way, and the
// places hand over in a wave along the figure, so one shape unravels into the
// next as a stream. No place lies near the water, and the lake has a floor of
// air over it that no bird crosses.
//
// The camera stands on the shore and drifts round until the sun sits behind
// the flock. A bird on the far side of a figure is a speck of haze that only
// darkens the sky a little, and specks stack, so density reads as tone; a bird
// on the near side is a black chevron whose wings beat, larger as it comes
// nearer. The lake mirrors the flock faintly under the sun's glitter path.
//
// The last act: the sphere is a black sun over the real one, turning faster
// and drawing tighter through the riser, swelling on every kick. On the last
// hit a falcon stoops through its heart and past the lens, the sphere bursts
// with a flash and a ring of light, and the flock settles into three rings
// that tumble through one another like a gyroscope and come to rest as one
// halo round the sun.
//
// Scored to its cue: each figure stands on its hit, the kick swells whatever
// the flock is flying and the sun's rays, the snare sets the lake's glitter
// sparking, each hit blazes the rays, and the riser draws the light in.

// Globals on purpose: the parallel steering reads them (arrays borrow zero-copy).
array<float> BD_X = [];
array<float> BD_Y = [];
array<float> BD_Z = [];
array<float> BD_VX = [];
array<float> BD_VY = [];
array<float> BD_VZ = [];
array<float> BD_ALONG = [];  // per bird: how far along its figure's curve it flies, 0..1
array<float> BD_U = [];      // its place along the murmuration's sheet
array<float> BD_V = [];      // and across it
array<float> BD_OFF = [];    // how far off its curve it holds, -1..1
array<float> BD_GR = [];     // its radius in the galaxy,
array<float> BD_GA = [];     // its angle there,
array<float> BD_GY = [];     // and its height off the disc
array<float> BD_LATC = [];   // per band of the sphere: its latitude's cosine
array<float> BD_LATS = [];   // and sine
array<float> BD_FRAME = [];  // per figure: its rotation (nine) and its centre (three)
array<int> BD_HEAD = [];     // per grid cell: its first bird, -1 when empty
array<int> BD_NEXT = [];     // per bird: the next bird in its cell
array<int> BD_CHUNKS = [];
int BD_N = 0;
int BD_CHUNK = 0;
float BD_DT = 0.0;
float BD_T = 0.0;
int BD_FIG = 0;              // the figure the flock is flying to,
int BD_PREV = 0;             // the one it is leaving,
float BD_SINCE = 0.0;        // how long ago the hand-over began
float BD_PEEL = 0.0;         // and how long it takes to run along the figure
float BD_ARRIVE = 1.0;       // how eagerly a bird closes on its place,
float BD_RESPONSE = 1.0;     // how fast it turns to the heading it wants,
float BD_VMAX = 30.0;        // its top speed
float BD_VMIN = 15.0;        // and its stalling speed
float BD_ALIGN = 2.0;        // how hard it matches its neighbours' heading
float BD_SWIRL = 0.0;        // how much a bird far from its place circles in round the lens's axis
float BD_SWX = 0.0;
float BD_SWY = 0.0;
float BD_SWZ = 1.0;
float BD_BREATH = 1.0;       // the kick's swell
float BD_FLOOR = 15.0;       // the floor of air over the lake
float BD_LEN = 30.0;         // the sheet's half length and half width,
float BD_WID = 15.0;
float BD_FOLD = 6.0;         // its fold's depth and phase
float BD_PHASE = 0.0;
float BD_BALL = 14.0;        // the sphere's radius
float BD_SPIN = 0.0;         // and its turn

// the figures
int BD_VEIL = 0;
int BD_TORUS = 1;
int BD_HELIX = 2;
int BD_RIBBON = 3;
int BD_GALAXY = 4;
int BD_SPHERE = 5;
int BD_RINGS = 6;            // three frames: one a ring

// the grid: 12-unit cells over x and z in [-72, 72), y in [0, 72)
function bd_cell(float x, float y, float z) -> int {
	int cx = cast<int>((x + 72.0) * 0.0833333);
	int cy = cast<int>(y * 0.0833333);
	int cz = cast<int>((z + 72.0) * 0.0833333);
	cx = cx < 0 ? 0 : (cx > 11 ? 11 : cx);
	cy = cy < 0 ? 0 : (cy > 5 ? 5 : cy);
	cz = cz < 0 ? 0 : (cz > 11 ? 11 : cz);
	return (cz * 6 + cy) * 12 + cx;
}

// one chunk of birds on a parallel_transform worker: each bird finds its place
// in its figure, reads its neighbours in the eight cells nearest it (at most
// seven: starlings track about seven) and returns its new velocity
function boids_steer(int c) -> array<float> {
	int lo = c * BD_CHUNK;
	int hi = lo + BD_CHUNK;
	hi = hi > BD_N ? BD_N : hi;
	float dt = BD_DT;
	float t = BD_T;
	int fig_now = BD_FIG;
	int fig_was = BD_PREV;
	float since = BD_SINCE;
	float peel = BD_PEEL;
	float arrive = BD_ARRIVE;
	float response = BD_RESPONSE;
	float top = BD_VMAX;
	float least = BD_VMIN;
	float align = BD_ALIGN;
	float swirl = BD_SWIRL * top;
	float swx = BD_SWX;
	float swy = BD_SWY;
	float swz = BD_SWZ;
	float breath = BD_BREATH;
	float floor_y = BD_FLOOR;
	bool loose = fig_now == 0;
	array<float> out = [];
	for (int i = lo; i < hi; ++i) {
		float x = BD_X[i];
		float y = BD_Y[i];
		float z = BD_Z[i];
		float vx = BD_VX[i];
		float vy = BD_VY[i];
		float vz = BD_VZ[i];
		float along = BD_ALONG[i];
		int fig = since >= along * peel ? fig_now : fig_was;
		float px = 0.0;
		float py = 0.0;
		float pz = 0.0;
		if (fig == 0) {
			float su = BD_U[i];
			px = su * BD_LEN;
			py = BD_V[i] * BD_WID;
			pz = BD_FOLD * math::sin(su * 2.4 + BD_PHASE);
		} else if (fig == 1) {
			// one line coiled thirty times round the torus: the coils roll and creep round
			float coil = along * 188.49556 + t * 2.2;
			float tube = (6.3 + BD_OFF[i] * 0.5) * breath;
			float ring = 17.5 + tube * math::cos(coil);
			float around = along * 6.2831853 + t * 0.3;
			px = ring * math::cos(around);
			py = tube * math::sin(coil);
			pz = ring * math::sin(around);
		} else if (fig == 2) {
			// two strands, and every fifth bird on one of the rungs between them
			float turn = along;
			float reach = 8.5 * breath;
			if (i % 5 == 4) {
				turn = (math::floor(along * 20.0) + 0.5) * 0.05;
				reach *= BD_OFF[i];
			} else if (i % 2 == 1) {
				reach = 0.0 - reach;
			}
			float twist = turn * 15.707963 + t * 2.0;
			px = (turn - 0.5) * 64.0;
			py = reach * math::cos(twist);
			pz = reach * math::sin(twist);
		} else if (fig == 3) {
			// a figure eight in four lanes: its two passes cross one over the other
			float lap = along * 6.2831853 + t * 0.85;
			float lobe = math::sin(lap);
			px = 30.0 * lobe;
			py = (i % 4 - 1.5) * 2.2 + 5.0 * math::cos(lap);
			pz = 26.0 * lobe * math::cos(lap);
		} else if (fig == 4) {
			float arm = BD_GA[i] + t * 0.75;
			float reach = BD_GR[i] * (0.94 + 0.06 * breath);
			px = reach * math::cos(arm);
			py = BD_GY[i];
			pz = reach * math::sin(arm);
		} else if (fig == 5) {
			int band = i % 7;
			float lon = along * 6.2831853 + BD_SPIN;
			float reach = BD_BALL * BD_LATC[band];
			px = reach * math::cos(lon);
			py = BD_BALL * BD_LATS[band];
			pz = reach * math::sin(lon);
		} else {
			int hoop = i % 3;
			fig = 6 + hoop;
			float lap = along * 6.2831853 + t * (hoop == 1 ? -0.5 : 0.5);
			float reach = (19.0 - hoop * 3.2) * (1.5 - 0.5 * breath);
			px = reach * math::cos(lap);
			py = reach * math::sin(lap);
			pz = (i / 3) % 2 * 1.6 - 0.8;
		}
		int m = fig * 12;
		float gx = BD_FRAME[m + 9] + BD_FRAME[m] * px + BD_FRAME[m + 1] * py + BD_FRAME[m + 2] * pz - x;
		float gy = BD_FRAME[m + 10] + BD_FRAME[m + 3] * px + BD_FRAME[m + 4] * py + BD_FRAME[m + 5] * pz - y;
		float gz = BD_FRAME[m + 11] + BD_FRAME[m + 6] * px + BD_FRAME[m + 7] * py + BD_FRAME[m + 8] * pz - z;
		float gap = math::sqrt(gx * gx + gy * gy + gz * gz) + 0.001;
		// a bird far from its place flocks and swirls; one in its place holds it
		float astray = loose || gap > 12.0 ? 1.0 : gap * 0.0833333;
		float ax = 0.0;
		float ay = 0.0;
		float az = 0.0;
		if (astray > 0.12) {
			float ux = 0.0;
			float uy = 0.0;
			float uz = 0.0;
			int near = 0;
			int seen = 0;
			float fx = (x + 72.0) * 0.0833333;
			float fy = y * 0.0833333;
			float fz = (z + 72.0) * 0.0833333;
			int cx = cast<int>(fx);
			int cy = cast<int>(fy);
			int cz = cast<int>(fz);
			int ox = fx - cx > 0.5 ? 1 : -1;
			int oy = fy - cy > 0.5 ? 1 : -1;
			int oz = fz - cz > 0.5 ? 1 : -1;
			for (int k = 0; k < 8 && near < 7 && seen < 24; ++k) {
				int xx = (k & 1) != 0 ? cx + ox : cx;
				int yy = (k & 2) != 0 ? cy + oy : cy;
				int zz = (k & 4) != 0 ? cz + oz : cz;
				if (xx < 0 || xx > 11 || yy < 0 || yy > 5 || zz < 0 || zz > 11) { continue; }
				int j = BD_HEAD[(zz * 6 + yy) * 12 + xx];
				while (j >= 0 && near < 7 && seen < 24) {
					++seen;
					float ex = BD_X[j] - x;
					float ey = BD_Y[j] - y;
					float ez = BD_Z[j] - z;
					float d2 = ex * ex + ey * ey + ez * ez;
					if (d2 < 36.0 && j != i) {
						++near;
						ux += BD_VX[j];
						uy += BD_VY[j];
						uz += BD_VZ[j];
						if (d2 < 16.0) {
							float push = 40.0 / (d2 + 0.4);
							ax -= ex * push;
							ay -= ey * push;
							az -= ez * push;
						}
					}
					j = BD_NEXT[j];
				}
			}
			if (near > 0) {
				float inv = 1.0 / near;
				ax += (ux * inv - vx) * align;
				ay += (uy * inv - vy) * align;
				az += (uz * inv - vz) * align;
			}
			ax *= astray;
			ay *= astray;
			az *= astray;
		}
		float pace = gap * arrive > top ? top / gap : arrive;
		float curl = loose ? 0.0 : swirl * astray / gap;
		ax += (gx * pace + (gy * swz - gz * swy) * curl - vx) * response;
		ay += (gy * pace + (gz * swx - gx * swz) * curl - vy) * response;
		az += (gz * pace + (gx * swy - gy * swx) * curl - vz) * response;
		if (y < floor_y) { ay += (floor_y - y) * 12.0; }
		if (y > 66.0) { ay -= (y - 66.0) * 6.0; }
		vx += ax * dt;
		vy += ay * dt;
		vz += az * dt;
		float sp = math::sqrt(vx * vx + vy * vy + vz * vz) + 0.0001;
		float k3 = sp > top ? top / sp : (sp < least ? least / sp : 1.0);
		out.push(vx * k3);
		out.push(vy * k3);
		out.push(vz * k3);
	}
	return out;
}

class Boids : Scene {
	int nb = 1500;
	int tones = 32;
	array<int> sky = [];        // per pixel: tone * 8 + darkness
	array<float> glint = [];    // per lake pixel: the sun glitter's phase, < 0 off the path
	array<int> rays = [];       // per sky pixel: how much a sun ray lifts it, in sixteenths
	array<float> dens = [];     // the birds stacked on the screen, per pixel, in levels of darkness
	array<float> flap = [];     // per bird: where its wingbeat stands, in quarter beats
	array<float> stands = [];   // per figure after the murmuration: when it stands, on its hit
	array<float> leaves = [];   // and when the flock leaves for it
	int horizon = 0;            // the first lake row
	int sky_w = 0;              // the backdrop is baked twice the screen's width, the sun at its middle
	float tt = 0.0;
	float cam_x = 0.0;          // the camera on the shore, and its basis
	float cam_y = 9.0;
	float cam_z = 0.0;
	float cam_far = 92.0;       // how far it stands from the stage
	float rx_ = 1.0;
	float rz_ = 0.0;
	float fwx = 0.0;
	float fwy = 0.0;
	float fwz = 1.0;
	float upx = 0.0;
	float upy = 1.0;
	float upz = 0.0;
	float focal = 90.0;
	float sun_x = 0.0;          // the sun on the screen
	float stage_y = 40.0;       // the figures' centre, over the middle of the lake
	float ball_y = 31.0;        // the sun's height there as the lens sees it: the sphere's centre and the rings'
	float burst_at = 0.0;
	float fal_x = 0.0;          // the falcon,
	float fal_y = 0.0;
	float fal_z = 0.0;
	float fal_vx = 0.0;         // its heading, for its silhouette,
	float fal_vy = 0.0;
	float fal_vz = 0.0;
	bool fal_out = false;       // and whether it is in the sky at all
	bool burst_done = false;
	int row_lo = 0;             // the rows the flock's birds touched this frame
	int row_hi = -1;

	void build() override {
		name = "BOIDS";
		handle = "murmuration . fifteen hundred starlings fly geometry";
		replay = 30.0;
		var stops = [[10.0, 12.0, 40.0], [48.0, 30.0, 90.0], [150.0, 60.0, 120.0], [240.0, 110.0, 90.0], [255.0, 190.0, 110.0], [255.0, 244.0, 200.0]];
		for (int t = 0; t < tones; ++t) {
			float f = t * 5.0 / (tones - 1.0);
			int k = math::itrunc(f);
			k = k > 4 ? 4 : k;
			float u = f - k;
			var a = stops[k];
			var b = stops[k + 1];
			float ar = a[0]; float ag = a[1]; float ab = a[2];
			float cr = ar + (b[0] - ar) * u;
			float cg = ag + (b[1] - ag) * u;
			float cb = ab + (b[2] - ab) * u;
			for (int d = 0; d < 8; ++d) {
				float k2 = 1.0 - d / 7.0 * 0.95;
				pal.add(cr * k2 + 5.0 * (1.0 - k2), cg * k2 + 3.0 * (1.0 - k2), cb * k2 + 9.0 * (1.0 - k2));
			}
		}
		// the lens frames the stage by the screen's height, so a wide screen only sees more sky
		focal = w * 0.9 < h * 1.125 ? w * 0.9 : h * 1.125;
		horizon = math::itrunc(h * 0.78);
		bake_sky();
		dens = gfx.make_fgrid(w * h);
		stands = [];
		leaves = [];
		for (int f2 = 0; f2 < 6; ++f2) {
			stands.push(MUSIC_BAR * (3.0 + f2 * 2.0));
			leaves.push(stands[f2] - 1.7);
		}
		burst_at = stands[5];
		leaves[5] = burst_at;
		burst_done = false;
		fal_out = false;
		BD_N = nb;
		BD_CHUNK = 64;
		BD_CHUNKS = [];
		for (int c = 0; c * BD_CHUNK < nb; ++c) { BD_CHUNKS.push(c); }
		BD_X = []; BD_Y = []; BD_Z = [];
		BD_VX = []; BD_VY = []; BD_VZ = [];
		BD_NEXT = [];
		BD_ALONG = [];
		BD_U = [];
		BD_V = [];
		BD_OFF = [];
		BD_GR = [];
		BD_GA = [];
		BD_GY = [];
		flap = [];
		for (int i = 0; i < nb; ++i) {
			// they stream in from the left, high over the water
			BD_X.push(-84.0 + rng.nextf() * 30.0);
			BD_Y.push(30.0 + rng.nextf() * 18.0);
			BD_Z.push(-14.0 + rng.nextf() * 40.0);
			BD_VX.push(24.0 + rng.nextf() * 4.0);
			BD_VY.push(rng.nextf() * 4.0 - 2.0);
			BD_VZ.push(rng.nextf() * 6.0 - 3.0);
			BD_NEXT.push(-1);
			float along = (i + 0.5) / nb;
			BD_ALONG.push(along);
			float ang = rng.nextf() * 6.2831853;
			float rad = math::sqrt(rng.nextf());
			BD_U.push(math::cos(ang) * rad);
			BD_V.push(math::sin(ang) * rad);
			float off = rng.nextf() * 2.0 - 1.0;
			BD_OFF.push(off);
			// the galaxy: two arms wound out from a thick core, every fifth bird adrift between them
			float reach = 2.5 + 24.5 * along;
			float spread = i % 5 == 0 ? off * 1.5707963 : off * (off < 0.0 ? 0.0 - off : off) * 0.42;
			BD_GR.push(reach);
			BD_GA.push((i % 2) * 3.1415927 + 2.3 * math::log2(1.0 + reach * 0.25) + spread);
			BD_GY.push((rng.nextf() * 2.0 - 1.0) * (0.6 + 4.0 * math::exp(0.0 - reach * 0.2)));
			flap.push(rng.nextf() * 4.0);
		}
		BD_LATC = [];
		BD_LATS = [];
		for (int band = 0; band < 7; ++band) {
			float lat = (band - 3) * 0.38;
			BD_LATC.push(math::cos(lat));
			BD_LATS.push(math::sin(lat));
		}
		BD_FRAME = gfx.make_fgrid(9 * 12);
		BD_HEAD = gfx.make_grid(12 * 6 * 12);
		BD_FIG = 0;
		BD_PREV = 0;
	}

	// the backdrop: a sunset gradient with the sun low over the water, streaks of
	// cloud lit from below, and the lake mirroring it all a shade darker, with a
	// glitter path under the sun. The sky stays light enough at the top for a
	// black bird to read anywhere in it.
	void bake_sky() {
		int bw = w * 2;
		sky_w = bw;
		sky = gfx.make_grid(bw * h);
		glint = gfx.make_fgrid(bw * h);
		rays = gfx.make_grid(bw * h);
		float sunx = w;
		float suny = sun_row();
		float sr = h * 0.075;
		for (int y = 0; y < horizon; ++y) {
			for (int x2 = 0; x2 < bw; ++x2) {
				float g = y * 1.0 / horizon;
				float tone = 7.0 + g * math::sqrt(g) * 14.5;
				float dx = x2 - sunx;
				float dy = (y - suny) * 1.15;
				float d = math::sqrt(dx * dx + dy * dy);
				float glow = d < sr ? 31.0 : 27.0 - (d - sr) * 0.42 * 120.0 / h;
				tone = glow > tone ? glow : tone;
				float cover = (cloud(x2, y) - 0.55) * 1.8;
				cover = cover < 0.0 ? 0.0 : (cover > 1.0 ? 1.0 : cover);
				cover *= g < 0.72 ? 1.0 - g * 0.6 : 0.0;
				if (cover > 0.0) {
					float below = cloud(x2, y + 2) - cloud(x2, y);
					float near_sun = 1.0 - d / (w * 0.8);
					near_sun = near_sun < 0.0 ? 0.0 : near_sun;
					float body = 9.0 + g * 8.0 + near_sun * 4.0;
					float lit = below < 0.0 ? (0.0 - below) * (10.0 + near_sun * 14.0) : 0.0;
					tone = tone + (body + lit - tone) * cover;
				}
				int it = math::itrunc(tone);
				sky[y * bw + x2] = (it > 31 ? 31 : (it < 0 ? 0 : it)) * 8;
				// god rays: a fan of beams from the sun, broken where the clouds are thick
				float ang = math::atan2(dy, dx);
				float beam = math::sin(ang * 23.0) * 0.6 + math::sin(ang * 9.0 + 1.3) * 0.5;
				float reach = 1.0 - d / (h * 0.95);
				rays[y * bw + x2] = beam > 0.35 && reach > 0.0 && d > sr ? math::itrunc((beam - 0.35) * reach * (1.0 - cover) * 9.0 * 16.0) : 0;
			}
		}
		for (int y2 = horizon; y2 < h; ++y2) {
			int mirror = mirrored_row(y2);
			float depth = (y2 - horizon) * 1.0 / (h - horizon);
			for (int x4 = 0; x4 < bw; ++x4) {
				int src = sky[mirror * bw + x4];
				int tn = src / 8 - math::itrunc(2.0 + depth * 6.0);
				tn = tn < 0 ? 0 : tn;
				sky[y2 * bw + x4] = tn * 8 + 1 + math::itrunc(depth * 2.0);
				float spread = (2.0 + depth * 14.0) * h / 120.0;
				float off = x4 - sunx;
				glint[y2 * bw + x4] = off * off < spread * spread ? x4 * 1.7 + y2 * 2.9 + rng.nextf() * 6.28 : -1.0;
			}
		}
	}

	// the sun stands well clear of the water, so a figure can ring it without nearing the lake
	float sun_row() { return horizon - h * 0.31; }

	// the lake foreshortens what it mirrors
	int mirrored_row(int y) {
		int result = horizon - 1 - (y - horizon) * 11 / 5;
		return result < 1 ? 1 : result;
	}

	// cloud streaks: two stretched sine octaves under a band that thickens them in the upper sky
	float cloud(int x, int y) {
		float n = math::sin(x * 0.05 + y * 0.17 + math::sin(y * 0.33) * 1.5) + 0.5 * math::sin(x * 0.13 - y * 0.26 + 2.1);
		return n + math::sin(y * 7.5 / horizon + 0.4) * 0.8;
	}

	// the murmuration alone for three bars, a figure every two, the burst, and two bars for the halo
	int length_bars() override { return 15; }

	array<float> hit_times() override { return [MUSIC_BAR * 3.0, MUSIC_BAR * 5.0, MUSIC_BAR * 7.0, MUSIC_BAR * 9.0, MUSIC_BAR * 11.0, MUSIC_BAR * 13.0]; }
	array<int> hit_levels() override { return [2, 1, 1, 1, 1, 2]; }

	float ease(float a, float b, float t) {
		float k = (t - a) / (b - a);
		k = k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
		return k * k * (3.0 - 2.0 * k);
	}

	void update(float t, float dt) override {
		tt = t;
		place_camera(t);
		if (dt <= 0.0) { return; }
		float step = dt > 0.05 ? 0.05 : dt;
		float kick = cue == null ? 0.0 : cue.kick_env(t);
		choreograph(t, kick);
		place_falcon(t);
		if (!burst_done && t >= burst_at) { burst(); }
		BD_DT = step;
		BD_T = t;
		hash_birds();
		var vel = parallel_transform(BD_CHUNKS, boids_steer);
		var& x = BD_X;
		var& y = BD_Y;
		var& z = BD_Z;
		var& vx = BD_VX;
		var& vy = BD_VY;
		var& vz = BD_VZ;
		float lowest = BD_FLOOR - 2.0;
		int i = 0;
		for (int c = 0; c < vel.size(); ++c) {
			var& part = vel[c];
			int m = part.size();
			for (int k = 0; k < m; k += 3) {
				vx[i] = part[k];
				vy[i] = part[k + 1];
				vz[i] = part[k + 2];
				x[i] += vx[i] * step;
				z[i] += vz[i] * step;
				float up = y[i] + vy[i] * step;
				if (up < lowest) {
					up = lowest;
					vy[i] = 0.0;
				}
				y[i] = up;
				++i;
			}
		}
	}

	// which figure the flock flies, how it steers there, and where every figure's frame stands
	void choreograph(float t, float kick) {
		int fig = 0;
		while (fig < 6 && t >= leaves[fig]) { ++fig; }
		BD_FIG = fig;
		BD_PREV = fig > 0 ? fig - 1 : 0;
		BD_SINCE = fig > 0 ? t - leaves[fig - 1] : 0.0;
		// the hand-over runs along the figure; the burst takes every bird at once
		BD_PEEL = fig == BD_RINGS ? 0.0 : 0.25;
		bool loose = fig == 0;
		float flung = fig == BD_RINGS && BD_SINCE < 0.45 ? 1.0 : 0.0;
		BD_ARRIVE = loose ? 1.3 : 7.0;
		BD_RESPONSE = loose ? 1.4 : 7.0 - flung * 6.0;
		BD_VMAX = loose ? 30.0 : 50.0 + flung * 20.0;
		BD_VMIN = loose ? 15.0 : 3.0;
		BD_ALIGN = loose ? 2.2 : 1.4;
		BD_SWIRL = fig == BD_RINGS ? 0.25 : 0.4;
		BD_SWX = fwx;
		BD_SWY = fwy;
		BD_SWZ = fwz;
		BD_BREATH = 1.0 + kick * 0.14;
		BD_FLOOR = 15.0;
		shape_sheet(t, kick);
		rotated_frame(BD_TORUS, 0.55 * math::sin(t * 0.8), 1.0 + 0.4 * math::sin(t * 1.1 + 1.0), 0.0, 0.0, stage_y, 0.0);
		rotated_frame(BD_HELIX, 0.3 * math::sin(t * 0.8 + 1.2), 0.0, 0.28, 0.0, stage_y, 0.0);
		rotated_frame(BD_RIBBON, 0.55 * math::sin(t * 0.7), 0.6, 0.12 * math::sin(t), 0.0, stage_y, 0.0);
		rotated_frame(BD_GALAXY, 0.0, 0.85 + 0.15 * math::sin(t * 0.8), 0.3, 0.0, stage_y, 0.0);
		rotated_frame(BD_SPHERE, 0.0, 0.45, 0.3, 0.0, ball_y, 0.0);
		// the black sun turns faster and draws tighter through the riser
		float held = t - stands[4];
		held = held < 0.0 ? 0.0 : held;
		float tighten = ease(stands[4], burst_at, t);
		BD_SPIN = held * 1.1 + held * held * 0.45;
		BD_BALL = (14.0 - tighten * 3.0) * (1.0 + kick * 0.2);
		// the gyroscope: each ring turns half over about its own axis across the screen and comes to rest facing the lens
		float gone = t - burst_at;
		tumbled_frame(6, 0.0, 3.1415927 * ease(0.25, 3.1, gone));
		tumbled_frame(7, 1.0471976, -3.1415927 * ease(0.4, 3.3, gone));
		tumbled_frame(8, 2.0943951, 3.1415927 * ease(0.15, 3.4, gone));
	}

	// a figure's frame: rolled, tilted back from the lens, then turned about the vertical
	void rotated_frame(int fig, float yaw, float tilt, float roll, float cx, float cy, float cz) {
		float c = math::cos(yaw);
		float s = math::sin(yaw);
		float ct = math::cos(tilt);
		float st = math::sin(tilt);
		float cr = math::cos(roll);
		float sr = math::sin(roll);
		var& fr = BD_FRAME;
		int m = fig * 12;
		fr[m] = c * cr + s * st * sr;
		fr[m + 1] = s * st * cr - c * sr;
		fr[m + 2] = s * ct;
		fr[m + 3] = ct * sr;
		fr[m + 4] = ct * cr;
		fr[m + 5] = 0.0 - st;
		fr[m + 6] = c * st * sr - s * cr;
		fr[m + 7] = s * sr + c * st * cr;
		fr[m + 8] = c * ct;
		fr[m + 9] = cx;
		fr[m + 10] = cy;
		fr[m + 11] = cz;
	}

	// a ring's frame: facing the lens, turned by an angle about an axis lying across the screen
	void tumbled_frame(int fig, float axis, float turn) {
		float c = math::cos(axis);
		float s = math::sin(axis);
		float ca = math::cos(turn);
		float sa = math::sin(turn);
		var& fr = BD_FRAME;
		int m = fig * 12;
		fr[m] = ca + (1.0 - ca) * c * c;
		fr[m + 1] = (1.0 - ca) * c * s;
		fr[m + 2] = sa * s;
		fr[m + 3] = (1.0 - ca) * c * s;
		fr[m + 4] = ca + (1.0 - ca) * s * s;
		fr[m + 5] = 0.0 - sa * c;
		fr[m + 6] = 0.0 - sa * s;
		fr[m + 7] = sa * c;
		fr[m + 8] = ca;
		fr[m + 9] = 0.0;
		fr[m + 10] = ball_y;
		fr[m + 11] = 0.0;
	}

	// the murmuration's sheet swings and tilts, stretches into a ribbon and gathers
	// in again, and its fold travels along it: seen edge-on it is a dark line,
	// face-on a pale veil
	void shape_sheet(float t, float kick) {
		float yaw = 0.3 + 0.95 * math::sin(t * 0.35);
		float tilt = 0.5 * math::sin(t * 0.29);
		float cy = math::cos(yaw);
		float sy = math::sin(yaw);
		float ct = math::cos(tilt);
		float st = math::sin(tilt);
		float roll = 0.9 + 0.7 * math::sin(t * 0.23 + 0.6);
		float cr = math::cos(roll);
		float sr = math::sin(roll);
		float ax = cy * ct;
		float ay = st;
		float az = sy * ct;
		float bx = 0.0 - sy * cr - cy * st * sr;
		float by = ct * sr;
		float bz = cy * cr - sy * st * sr;
		var& fr = BD_FRAME;
		fr[0] = ax;
		fr[1] = bx;
		fr[2] = ay * bz - az * by;
		fr[3] = ay;
		fr[4] = by;
		fr[5] = az * bx - ax * bz;
		fr[6] = az;
		fr[7] = bz;
		fr[8] = ax * by - ay * bx;
		fr[9] = 22.0 * math::sin(t * 0.33 - 0.5);
		fr[10] = stage_y + 2.0 + 7.0 * math::sin(t * 0.53 + 0.8);
		fr[11] = 8.0 + 12.0 * math::sin(t * 0.47 + 1.3);
		BD_LEN = 31.0 + 8.0 * math::sin(t * 0.37);
		BD_WID = 15.0 - 4.0 * math::sin(t * 0.37);
		BD_FOLD = 6.0 + 4.0 * math::sin(t * 0.61 + 2.0) + kick * 6.0;
		BD_PHASE = t * 1.3;
	}

	// the falcon strikes the black sun's heart: every bird is thrown straight out from it
	void burst() {
		burst_done = true;
		var& vx = BD_VX;
		var& vy = BD_VY;
		var& vz = BD_VZ;
		for (int i = 0; i < nb; ++i) {
			float dx = BD_X[i];
			float dy = BD_Y[i] - ball_y;
			float dz = BD_Z[i];
			float k = 60.0 / (math::sqrt(dx * dx + dy * dy + dz * dz) + 0.5);
			vx[i] = dx * k;
			vy[i] = dy * k;
			vz[i] = dz * k;
		}
	}

	// birds into their cells: each cell's list threads through BD_NEXT
	void hash_birds() {
		var& head = BD_HEAD;
		var& next = BD_NEXT;
		int cells = head.size();
		for (int c = 0; c < cells; ++c) { head[c] = -1; }
		for (int i = 0; i < nb; ++i) {
			int cell = bd_cell(BD_X[i], BD_Y[i], BD_Z[i]);
			next[i] = head[cell];
			head[cell] = i;
		}
	}

	// the falcon wheels high behind the black sun, then stoops through its heart on
	// the burst and out past the lens's right: a curve whose halfway point is the heart
	void place_falcon(float t) {
		float dive = (t - (burst_at - 0.75)) / 1.5;
		fal_out = t > stands[4] && dive < 1.0;
		if (!fal_out) { return; }
		float ox = fal_x;
		float oy = fal_y;
		float oz = fal_z;
		float wheel = t * 1.1;
		float sx = 0.0 - fwx * 46.0 + 12.0 * math::cos(wheel);
		float sy = ball_y + 34.0 + 3.0 * math::sin(wheel * 2.0);
		float sz = 0.0 - fwz * 46.0 + 12.0 * math::sin(wheel);
		// it comes in over the lens as the sphere stands
		float entry = 1.0 - ease(stands[4], stands[4] + 1.6, t);
		sx += (cam_x - sx) * entry;
		sy += 20.0 * entry;
		sz += (cam_z - sz) * entry;
		if (dive <= 0.0) {
			fal_x = sx;
			fal_y = sy;
			fal_z = sz;
		} else {
			float lx = cam_x + rx_ * 30.0;
			float ly = cam_y + 3.0;
			float lz = cam_z + rz_ * 30.0;
			float a = (1.0 - dive) * (1.0 - dive);
			float b = 2.0 * dive * (1.0 - dive);
			float c = dive * dive;
			fal_x = sx * a + (0.0 - 0.5 * (sx + lx)) * b + lx * c;
			fal_y = sy * a + (2.0 * ball_y - 0.5 * (sy + ly)) * b + ly * c;
			fal_z = sz * a + (0.0 - 0.5 * (sz + lz)) * b + lz * c;
		}
		fal_vx = fal_x - ox;
		fal_vy = fal_y - oy;
		fal_vz = fal_z - oz;
	}

	// on the shore, circling a little way round the stage until the sun stands behind
	// it; the pitch stays fixed so the horizon stays where the backdrop was baked
	void place_camera(float t) {
		float orbit = (0.36 + 0.04 * math::sin(t * 0.4)) * (1.0 - ease(0.0, stands[4] - 1.0, t));
		float ang = orbit - 1.5707963;
		cam_far = 96.0 - 8.0 * ease(0.0, burst_at, t);
		cam_x = cam_far * math::cos(ang);
		cam_z = cam_far * math::sin(ang);
		float pitch = math::atan2(horizon - h * 0.5, focal);
		float cp = math::cos(pitch);
		float sp = math::sin(pitch);
		float cy = 0.0 - math::cos(ang);
		float sy = 0.0 - math::sin(ang);
		fwx = cy * cp;
		fwy = sp;
		fwz = sy * cp;
		rx_ = sy;
		rz_ = 0.0 - cy;
		upx = 0.0 - cy * sp;
		upy = cp;
		upz = 0.0 - sy * sp;
		sun_x = w * 0.5 + focal * math::sin(orbit) / math::cos(orbit);
		float rise = pitch + math::atan2(h * 0.5 - sun_row(), focal);
		ball_y = cam_y + cam_far * math::sin(rise) / math::cos(rise);
	}

	void render(var& grid) override {
		var& dn = dens;
		int ww = w;
		float half_w = w * 0.5;
		float half_h = h * 0.5;
		float cx = cam_x;
		float cy = cam_y;
		float cz = cam_z;
		float fx = fwx;
		float fy = fwy;
		float fz = fwz;
		float rx = rx_;
		float rz = rz_;
		float ux = upx;
		float uy = upy;
		float uz = upz;
		float fl = focal;
		float far = cam_far;
		int right = w - 2;
		int bottom = horizon - 2;
		float wing = tt * 7.0;
		int lo = h;
		int hi = -1;
		for (int b = 0; b < nb; ++b) {
			float ex = BD_X[b] - cx;
			float ey = BD_Y[b] - cy;
			float ez = BD_Z[b] - cz;
			float zc = ex * fx + ey * fy + ez * fz;
			if (zc < 12.0) { continue; }
			float iz = fl / zc;
			int sx = cast<int>(half_w + (ex * rx + ez * rz) * iz);
			int sy = cast<int>(half_h - (ex * ux + ey * uy + ez * uz) * iz);
			if (sx < 2 || sx >= right || sy < 1 || sy >= bottom) { continue; }
			lo = sy < lo ? sy : lo;
			hi = sy > hi ? sy : hi;
			int at = sy * ww + sx;
			// the far side of a figure is haze, the near side black wings
			float near = (far - zc) * 0.045;
			if (near < 0.0) {
				dn[at] += near < -1.1 ? 1.6 : 3.4 + near * 1.6;
				continue;
			}
			int pose = math::itrunc(wing + flap[b]) & 3;
			if (near < 0.6) {
				float wgt = 3.0 + near * 5.0;
				int tip = pose < 2 ? at - ww : at + ww;
				dn[at] += wgt + 2.0;
				dn[tip - 1] += wgt;
				dn[tip + 1] += wgt;
				continue;
			}
			dn[at] += 7.0;
			dn[at - 1] += 7.0;
			dn[at + 1] += 7.0;
			if (pose == 1) {
				dn[at - ww - 1] += 7.0;
				dn[at - ww + 1] += 7.0;
				dn[at - 2] += 7.0;
				dn[at + 2] += 7.0;
			} else {
				int tip2 = pose == 0 ? at - ww : at + ww;
				dn[tip2 - 2] += 7.0;
				dn[tip2 + 2] += 7.0;
			}
		}
		row_lo = lo > 0 ? lo - 1 : 0;
		row_hi = hi + 1;
		draw_falcon();
		compose(grid);
		shockwave(grid);
	}

	// sky, rays, flock and lake into the frame; the flock's rows are wiped as they are read
	void compose(var& grid) {
		var& dn = dens;
		var& sk = sky;
		var& gl = glint;
		var& ry = rays;
		int ww = w;
		float kick = cue == null ? 0.0 : cue.kick_env(tt);
		float snare = cue == null ? 0.0 : cue.snare_env(tt);
		float bass = cue == null ? 0.0 : cue.bass_env(tt);
		float breath = cue == null ? 0.0 : cue.riser(tt);
		float blaze = cue == null ? 0.0 : cue.hit(tt);
		int glit_on = math::itrunc(tt * 9.0) + math::itrunc(snare * 6.0);
		float glit_cut = 0.8 - snare * 0.25;
		// the rays swell with the kick and the bass, blaze on a hit and draw in before a big one
		float swell = 0.45 + 0.55 * math::sin(tt * 0.45) * math::sin(tt * 0.45) + kick * 0.5 + snare * 0.2 + bass * 0.15 + blaze * 1.6;
		int shine = math::itrunc(swell * (1.0 - breath * 0.7) * 16.0);
		float since = tt - burst_at;
		int flash = since >= 0.0 && since < 0.5 ? math::itrunc((0.5 - since) * 16.0) : 0;
		int bw = sky_w;
		int shift = math::itrunc(w - sun_x);
		shift = shift < 0 ? 0 : (shift > w ? w : shift);
		int lo = row_lo;
		int hi = row_hi;
		int hz = horizon;
		for (int y2 = hz; y2 < h; ++y2) {
			int row = y2 * ww;
			int srow = y2 * bw + shift;
			int mirror = mirrored_row(y2);
			bool flock = mirror >= lo && mirror - 1 <= hi;
			int mrow = mirror * ww;
			int ripple = math::itrunc(1.6 * math::sin(y2 * 1.1 + tt * 2.3));
			int last = ww - 1;
			for (int x2 = 0; x2 < ww; ++x2) {
				int s2 = sk[srow + x2];
				int dk2 = s2 & 7;
				if (flock) {
					int mx = x2 + ripple;
					mx = mx < 0 ? 0 : (mx > last ? last : mx);
					int lv2 = cast<int>((dn[mrow + mx] + dn[mrow - ww + mx]) * 0.2);
					dk2 += lv2 > 2 ? 2 : lv2;
				}
				int tone = s2 - (s2 & 7);
				float ph = gl[srow + x2];
				if (ph >= 0.0 && math::sin(ph + glit_on * 0.7) > glit_cut) {
					tone = 30 * 8;
					dk2 = 0;
				}
				grid[row + x2] = tone + (dk2 > 7 ? 7 : dk2);
			}
		}
		for (int y = 0; y < hz; ++y) {
			int row = y * ww;
			int srow = y * bw + shift;
			if (y < lo || y > hi) {
				for (int x = 0; x < ww; ++x) {
					int tone = (sk[srow + x] >> 3) + ((ry[srow + x] * shine) >> 8) + flash;
					grid[row + x] = (tone > 31 ? 31 : tone) * 8;
				}
				continue;
			}
			for (int x = 0; x < ww; ++x) {
				int tone = (sk[srow + x] >> 3) + ((ry[srow + x] * shine) >> 8) + flash;
				int dk = cast<int>(dn[row + x]);
				dn[row + x] = 0.0;
				grid[row + x] = (tone > 31 ? 31 : tone) * 8 + (dk > 7 ? 7 : dk);
			}
		}
	}

	// the burst's shockwave: a ring of light racing out from the black sun's heart across the sky
	void shockwave(var& grid) {
		float since = tt - burst_at;
		if (since < 0.0 || since > 0.9) { return; }
		float ex = 0.0 - cam_x;
		float ey = ball_y - cam_y;
		float ez = 0.0 - cam_z;
		float iz = focal / (ex * fwx + ey * fwy + ez * fwz);
		float bsx = w * 0.5 + (ex * rx_ + ez * rz_) * iz;
		float bsy = h * 0.5 - (ex * upx + ey * upy + ez * upz) * iz;
		float r1 = 4.0 + since * w * 1.1;
		float r0 = r1 - 2.0 - since * 6.0;
		int lift = math::itrunc((0.9 - since) * 9.0);
		if (lift <= 0) { return; }
		int y0 = math::itrunc(bsy - r1);
		int y1 = math::itrunc(bsy + r1) + 1;
		y0 = y0 < 0 ? 0 : y0;
		y1 = y1 > horizon ? horizon : y1;
		for (int y = y0; y < y1; ++y) {
			float dy = y + 0.5 - bsy;
			float oo = r1 * r1 - dy * dy;
			if (oo <= 0.0) { continue; }
			float ox = math::sqrt(oo);
			float ii = r0 * r0 - dy * dy;
			float ix = ii > 0.0 ? math::sqrt(ii) : 0.0;
			for (int side = -1; side <= 1; side += 2) {
				int xa = math::itrunc(bsx + side * ix);
				int xb = math::itrunc(bsx + side * ox);
				int lo = xa < xb ? xa : xb;
				int hi = xa < xb ? xb : xa;
				lo = lo < 0 ? 0 : lo;
				hi = hi >= w ? w - 1 : hi;
				for (int x = lo; x <= hi; ++x) {
					int v = grid[y * w + x];
					int tone = v / 8 + lift;
					int dk = v % 8 - lift / 2;
					grid[y * w + x] = (tone > 31 ? 31 : tone) * 8 + (dk < 0 ? 0 : dk);
				}
			}
		}
	}

	// the falcon: a swept-wing silhouette, bigger than any starling, its wings
	// across its screen heading
	void draw_falcon() {
		if (!fal_out) { return; }
		float ex = fal_x - cam_x;
		float ey = fal_y - cam_y;
		float ez = fal_z - cam_z;
		float zc = ex * fwx + ey * fwy + ez * fwz;
		if (zc < 24.0) { return; }
		float iz = focal / zc;
		float sx = w * 0.5 + (ex * rx_ + ez * rz_) * iz;
		float sy = h * 0.5 - (ex * upx + ey * upy + ez * upz) * iz;
		float hx = fal_vx * rx_ + fal_vz * rz_;
		float hy = 0.0 - (fal_vx * upx + fal_vy * upy + fal_vz * upz);
		float hl = math::sqrt(hx * hx + hy * hy) + 0.0001;
		hx /= hl;
		hy /= hl;
		float span = (2.5 + 5.0 * iz) * h / 120.0;
		float widest = h * 0.13;
		span = span > widest ? widest : span;
		float beat = math::sin(tt * 9.0) * 0.6;
		var& dn = dens;
		int steps = math::itrunc(span * 1.5) + 4;
		for (int k = 0 - steps; k <= steps; ++k) {
			float a = (k < 0 ? 0 - k : k) * 1.0 / steps;
			float s = k * span / steps;
			float back = a * span * 0.55;
			int ix = math::itrunc(sx - hy * s - hx * back);
			int iy = math::itrunc(sy + hx * s - hy * back + a * beat * span * 0.4);
			if (ix >= 0 && ix < w && iy >= 0 && iy + 1 < horizon) {
				dn[iy * w + ix] += 7.0;
				if (a < 0.6) { dn[(iy + 1) * w + ix] += 7.0; }
				row_lo = iy < row_lo ? iy : row_lo;
				row_hi = iy + 1 > row_hi ? iy + 1 : row_hi;
			}
		}
		for (int k2 = -2; k2 <= 3; ++k2) {
			int bx = math::itrunc(sx + hx * k2 * span * 0.18);
			int by = math::itrunc(sy + hy * k2 * span * 0.18);
			if (bx >= 0 && bx < w && by >= 0 && by < horizon) {
				dn[by * w + bx] += 7.0;
				row_lo = by < row_lo ? by : row_lo;
				row_hi = by > row_hi ? by : row_hi;
			}
		}
	}
}
