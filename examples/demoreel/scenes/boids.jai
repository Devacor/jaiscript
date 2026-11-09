// BOIDS — a starling murmuration over a lake at sunset. A thousand birds fly
// in three dimensions: each keeps its distance from its nearest neighbours and
// matches their heading (Reynolds' separation and alignment) while it is drawn
// to its place on a sheet that swings, stretches and folds, the shape that
// makes a murmuration read as veils and dark ribbons. Neighbours come from a
// uniform grid hashed fresh every frame, and the birds steer in parallel
// chunks. A peregrine falcon circles and stoops through the flock: birds near
// it panic, and panic passes from bird to neighbour, so a scare wave rolls
// across the cloud and splits it around the falcon. The camera stands on the
// shore; each bird lands on the screen in perspective and stacks into a
// density buffer, so the cloud darkens where it is thick and pales where the
// sheet faces you, and the lake mirrors it under the sun's glitter path. Sky
// and clouds are baked tones; every palette entry is a sky tone darkened by a
// level, so the birds shade whatever they cross.
//
// The camera flies: it leaves the shore, glides up to the murmuration, rides
// alongside and then inside it as birds stream past as beating chevrons, the
// sky turning with it, and pulls up and away for the veil. Near birds are black
// chevrons, far ones specks fading into the haze, and a bird whose wings turn to
// the sun flashes gold, so a turn of the flock sweeps a wave of light across it.
// Late in the scene the flock gathers into a wheeling vortex, and each kick sends
// a ripple down its fold.
//
// Cut like a film: sun rays fan across the sky and the grade sinks toward dusk;
// the first stoop plays in slow motion as the camera pushes in on the strike
// (the flock squeezes into a ball, a flash and a lens flare land on impact); the
// second comes at full speed with the frame shaking; then the whole murmuration
// sweeps in low and close, a vast veil drawn across the sun.
//
// The last act: the veil lifts off the water and gathers into a black sun, one
// dense sphere of birds wheeling over the real one, turning faster and drawing
// tighter as the lens pushes in through the riser, swelling on every kick. On bar
// ten's downbeat the falcon stoops from high behind it through its heart and past
// the lens: the sphere bursts, every bird thrown straight out from the heart as a
// streak, a shockwave of light rings out across the sky with a flash and a shake,
// and the scattered flock settles into a vast wheeling halo standing on the water
// round the setting sun, mirrored in the lake, slowing its turn as it spreads.
//
// Scored to its cue: the slow-motion strike lands on the downbeat of bar 4 and the
// second stoop on beat 3 of bar 5, both hits, the black sun gathers on bar 8 and
// bursts on bar 10; the kick punches the lens in and swells the sun's rays, the
// snare punches a little less and sets the lake's glitter sparking, the bass
// breathes through the sky, each hit blazes the rays, and the bar before each
// strike draws the light in like a held breath.

// Globals on purpose: the parallel steering reads them (arrays borrow zero-copy).
array<float> BD_X = [];
array<float> BD_Y = [];
array<float> BD_Z = [];
array<float> BD_VX = [];
array<float> BD_VY = [];
array<float> BD_VZ = [];
array<float> BD_PANIC = [];
array<float> BD_U = [];      // per bird: its place along the sheet
array<float> BD_V = [];      // and across it
array<int> BD_HEAD = [];     // per grid cell: its first bird, -1 when empty
array<int> BD_NEXT = [];     // per bird: the next bird in its cell
array<int> BD_CHUNKS = [];
int BD_N = 0;
int BD_CHUNK = 0;
float BD_DT = 0.0;
float BD_TX = 0.0;           // where the flock is drawn to
float BD_TY = 0.0;
float BD_TZ = 0.0;
float BD_AX = 1.0;           // the sheet the flock folds into: its long axis,
float BD_AY = 0.0;
float BD_AZ = 0.0;
float BD_BX = 0.0;           // its short axis,
float BD_BY = 1.0;
float BD_BZ = 0.0;
float BD_NX = 0.0;           // its normal (the fold bends along it),
float BD_NY = 0.0;
float BD_NZ = 1.0;
float BD_LEN = 40.0;         // its half length and half width,
float BD_WID = 20.0;
float BD_FOLD = 10.0;        // the fold's depth and phase
float BD_PHASE = 0.0;
float BD_FLOOR = 18.0;       // how low the birds will fly over the water
float BD_FX = 0.0;           // the falcon
float BD_FY = 0.0;
float BD_FZ = 0.0;
float BD_PULL = 0.32;        // how hard each bird is drawn to its place (harder as the veil sweeps in)
float BD_FR2 = 400.0;        // the reach of its scare, squared (wider at the strike's shockwave)
float BD_VORTEX = 0.0;       // how far the flock has gathered into its wheeling ring,
float BD_VSPIN = 0.0;        // the ring's turn
float BD_VR = 30.0;          // and its radius
array<float> BD_SX = [];     // per bird: its place in the black sun, a point in the unit ball
array<float> BD_SY = [];
array<float> BD_SZ = [];
float BD_BALL = 0.0;         // how far the flock has gathered into the black sun,
float BD_BCX = 0.0;          // its centre,
float BD_BCY = 0.0;
float BD_BCZ = 0.0;
float BD_BR = 12.0;          // its radius (plus how far the burst has flung it),
float BD_BCOS = 1.0;         // and its turn
float BD_BSIN = 0.0;
float BD_BURST = 0.0;        // 1 once the falcon has burst the sphere: the birds fly out alone
array<float> BD_RA = [];     // per bird: its place around the halo the burst leaves
float BD_HALO = 0.0;         // how far the burst has settled into a halo facing the camera,
float BD_HR = 22.0;          // its radius,
float BD_HSPIN = 0.0;        // its turn,
float BD_HRX = 1.0;          // and the camera's right and forward, across the ground
float BD_HRZ = 0.0;
float BD_HFX = 0.0;
float BD_HFZ = 1.0;
float BD_DAMP = 0.0;         // how hard a bird in the black sun or the halo settles onto its place
float BD_OX = 0.0;           // the grid's centre: it follows the black sun
float BD_OZ = 0.0;

// the grid: 12-unit cells over x and z in [-96, 96) about its centre, y in [0, 96)
int BD_GX = 16;
int BD_GY = 8;

function bd_cell(float x, float y, float z) -> int {
	int cx = (x - BD_OX + 96.0) * 0.0833333;
	int cy = y * 0.0833333;
	int cz = (z - BD_OZ + 96.0) * 0.0833333;
	cx = cx < 0 ? 0 : (cx > 15 ? 15 : cx);
	cy = cy < 0 ? 0 : (cy > 7 ? 7 : cy);
	cz = cz < 0 ? 0 : (cz > 15 ? 15 : cz);
	return (cz * 8 + cy) * 16 + cx;
}

// one chunk of birds on a parallel_transform worker: each bird reads its
// neighbours in the 27 cells around it (at most ten: starlings track about
// seven) and returns its new velocity and panic
function boids_steer(int c) -> array<float> {
	int lo = c * BD_CHUNK;
	int hi = lo + BD_CHUNK;
	hi = hi > BD_N ? BD_N : hi;
	float dt = BD_DT;
	float tx = BD_TX;
	float ty = BD_TY;
	float tz = BD_TZ;
	float fx = BD_FX;
	float fy = BD_FY;
	float fz = BD_FZ;
	array<float> out = [];
	for (int i = lo; i < hi; ++i) {
		float x = BD_X[i];
		float y = BD_Y[i];
		float z = BD_Z[i];
		float vx = BD_VX[i];
		float vy = BD_VY[i];
		float vz = BD_VZ[i];
		float panic = BD_PANIC[i] * (1.0 - 1.6 * dt);
		float ax = 0.0;
		float ay = 0.0;
		float az = 0.0;
		float ux = 0.0;
		float uy = 0.0;
		float uz = 0.0;
		int near = 0;
		int cx = (x - BD_OX + 96.0) * 0.0833333;
		int cy = y * 0.0833333;
		int cz = (z - BD_OZ + 96.0) * 0.0833333;
		// a burst bird flies out alone, and a crowded cell is read only so far
		int seen = BD_BURST > 0.0 || BD_HALO > 0.0 ? 99 : 0;
		for (int dz = -1; dz <= 1 && near < 10 && seen < 60; ++dz) {
			int zz = cz + dz;
			if (zz < 0 || zz > 15) { continue; }
			for (int dy = -1; dy <= 1 && near < 10; ++dy) {
				int yy = cy + dy;
				if (yy < 0 || yy > 7) { continue; }
				for (int dx = -1; dx <= 1 && near < 10; ++dx) {
					int xx = cx + dx;
					if (xx < 0 || xx > 15) { continue; }
					int j = BD_HEAD[(zz * 8 + yy) * 16 + xx];
					while (j >= 0 && near < 10 && seen < 60) {
						++seen;
						if (j != i) {
							float ex = BD_X[j] - x;
							float ey = BD_Y[j] - y;
							float ez = BD_Z[j] - z;
							float d2 = ex * ex + ey * ey + ez * ez;
							if (d2 < 121.0) {
								++near;
								ux += BD_VX[j];
								uy += BD_VY[j];
								uz += BD_VZ[j];
								if (d2 < 20.0) {
									float k = 46.0 / (d2 + 0.4);
									ax -= ex * k;
									ay -= ey * k;
									az -= ez * k;
								}
								float pj = BD_PANIC[j] * 0.94;
								panic = pj > panic ? pj : panic;
							}
						}
						j = BD_NEXT[j];
					}
				}
			}
		}
		if (near > 0) {
			float inv = 1.0 / near;
			float al = 2.2 + panic * 3.0;
			ax += (ux * inv - vx) * al;
			ay += (uy * inv - vy) * al;
			az += (uz * inv - vz) * al;
		}
		// each bird is drawn to its place on the folding sheet
		float su = BD_U[i] * BD_LEN;
		float sv = BD_V[i] * BD_WID;
		float fold = BD_FOLD * math::sin(BD_U[i] * 2.4 + BD_PHASE);
		float qx = tx + BD_AX * su + BD_BX * sv + BD_NX * fold;
		float qy = ty + BD_AY * su + BD_BY * sv + BD_NY * fold;
		float qz = tz + BD_AZ * su + BD_BZ * sv + BD_NZ * fold;
		// the vortex: each bird's place on a wheeling ring around the flock's heart
		float vt = BD_VORTEX;
		if (vt > 0.0) {
			float ra = BD_U[i] * 3.1415927 + BD_VSPIN;
			float rr = BD_VR * (0.7 + 0.3 * BD_V[i]);
			float wx = tx + math::cos(ra) * rr;
			float wy = ty + BD_V[i] * 7.0 + math::sin(ra * 2.0 + BD_VSPIN) * 5.0;
			float wz = tz + math::sin(ra) * rr;
			qx += (wx - qx) * vt;
			qy += (wy - qy) * vt;
			qz += (wz - qz) * vt;
		}
		// the black sun: each bird's place in a turning sphere, flung outward by the burst
		float bl = BD_BALL;
		if (bl > 0.0) {
			float sx0 = BD_SX[i];
			float sz0 = BD_SZ[i];
			float br = BD_BR;
			float bx = BD_BCX + (sx0 * BD_BCOS - sz0 * BD_BSIN) * br;
			float by = BD_BCY + BD_SY[i] * br;
			float bz = BD_BCZ + (sx0 * BD_BSIN + sz0 * BD_BCOS) * br;
			qx += (bx - qx) * bl;
			qy += (by - qy) * bl;
			qz += (bz - qz) * bl;
		}
		float hl = BD_HALO;
		if (hl > 0.0) {
			float ra = BD_RA[i] + BD_HSPIN;
			float rr = BD_HR * (0.9 + 0.1 * BD_SY[i]);
			float cr = math::cos(ra) * rr;
			float dj = BD_SZ[i] * 6.0;
			float hx = BD_BCX + cr * BD_HRX + dj * BD_HFX;
			float hy = BD_BCY + math::sin(ra) * rr;
			float hz = BD_BCZ + cr * BD_HRZ + dj * BD_HFZ;
			qx += (hx - qx) * hl;
			qy += (hy - qy) * hl;
			qz += (hz - qz) * hl;
		}
		float gx = qx - x;
		float gy = qy - y;
		float gz = qz - z;
		float pull = BD_PULL * (1.0 - panic * 0.7);
		float damp = BD_DAMP;
		ax += gx * pull - vx * damp;
		ay += gy * pull - vy * damp;
		az += gz * pull - vz * damp;
		if (y < BD_FLOOR) { ay += (BD_FLOOR - y) * 5.0; }
		if (y > 88.0) { ay -= (y - 88.0) * 5.0; }
		// the falcon: birds within its reach flee and panic
		float px = x - fx;
		float py = y - fy;
		float pz = z - fz;
		float pd2 = px * px + py * py + pz * pz;
		float reach2 = BD_FR2;
		if (pd2 < reach2) {
			float reach = math::sqrt(reach2);
			float pd = math::sqrt(pd2) + 0.01;
			float k2 = 160.0 * (1.0 - pd / reach) / pd;
			ax += px * k2;
			ay += py * k2;
			az += pz * k2;
			panic = 1.0;
		}
		vx += ax * dt;
		vy += ay * dt;
		vz += az * dt;
		float sp = math::sqrt(vx * vx + vy * vy + vz * vz) + 0.0001;
		float top = 30.0 + panic * 16.0 + BD_BURST * 40.0;
		float least = 16.0 - BD_HALO * 9.0;
		float k3 = sp > top ? top / sp : (sp < least ? least / sp : 1.0);
		out.push(vx * k3);
		out.push(vy * k3);
		out.push(vz * k3);
		out.push(panic);
	}
	return out;
}

class Boids : Scene {
	int nb = 1500;
	int tones = 32;
	array<int> sky = [];        // per pixel: tone * 8 (darkness 0)
	array<float> glint = [];    // per lake pixel: the sun glitter's phase, < 0 off the path
	array<float> dens = [];     // the birds stacked on the screen, per pixel
	array<float> flap = [];     // per bird: wingbeat phase
	int horizon = 0;            // the first lake row
	float tt = 0.0;
	float cam_x = 0.0;          // the camera on the shore, and its basis
	float cam_y = 6.0;
	float cam_z = 0.0;
	float rx_ = 1.0;
	float rz_ = 0.0;
	float fwx = 0.0;
	float fwy = 0.0;
	float fwz = 1.0;
	float upx = 0.0;
	float upy = 1.0;
	float upz = 0.0;
	float focal = 90.0;
	float look_x = 0.0;         // where the camera turns to: the flock's centre, eased
	float look_z = 0.0;
	float fvx = 0.0;            // the falcon's heading, for its silhouette
	float fvy = 0.0;
	float fvz = 0.0;
	float sim_t = 0.0;          // the flock's own clock: it slows for the first strike
	float strike_t = -1.0;      // when the falcon reached the heart of the flock
	float fsx = 0.0;            // the falcon on screen, where the camera pushes in
	float fsy = 0.0;
	float focus_x = 0.0;
	float focus_y = 0.0;
	array<int> rays = [];       // per sky pixel: how much a sun ray lifts it, in sixteenths
	array<int> frame = [];      // the composed frame, read back through the push-in
	array<int> col_of = [];     // and the column each screen column reads
	float sun_x = 0.0;
	float sun_y = 0.0;
	float cen_x = 0.0;          // the flock's centre: the falcon dives through it
	float cen_y = 40.0;
	float cen_z = 0.0;
	int sky_w = 0;              // the backdrop is baked twice the screen's width
	int sky_x0 = 0;             // and the screen's left edge sits here in it, before the pan
	float pan = 0.0;            // how far the camera's turn pans the sky, in pixels
	float yaw0 = 0.0;           // the camera's first heading, where the pan starts from
	bool yaw_set = false;
	float sun_dx = 0.0;         // the sun's direction in the world: wings facing it flash
	float sun_dy = 0.0;
	float sun_dz = 0.0;
	array<float> lit = [];      // per pixel: the gold of wings caught by the sun
	bool anchored = false;      // the last act's camera: where it stood as the act began
	float anc_x = 0.0;
	float anc_y = 0.0;
	float anc_z = 0.0;
	float anc_yaw = 0.0;
	float burst_t = -1.0;       // when the falcon burst the black sun
	float bspin = 0.0;          // the black sun's turn
	float bsx = 0.0;            // the black sun on screen, where the shockwave rings out from
	float bsy = 0.0;

	void build() override {
		name = "BOIDS";
		handle = "murmuration . a thousand starlings and a falcon";
		replay = 22.0;
		frame = gfx.make_grid(w * h);
		col_of = gfx.make_grid(w);
		var stops = [[10.0, 12.0, 40.0], [48.0, 30.0, 90.0], [150.0, 60.0, 120.0], [240.0, 110.0, 90.0], [255.0, 190.0, 110.0], [255.0, 244.0, 200.0]];
		for (int t = 0; t < tones; ++t) {
			float f = t * 5.0 / (tones - 1.0);
			int k = itrunc(f);
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
		focal = w * 0.9;
		horizon = itrunc(h * 0.74);
		bake_sky();
		dens = gfx.make_fgrid(w * h);
		lit = gfx.make_fgrid(w * h);
		BD_N = nb;
		BD_CHUNK = 64;
		BD_CHUNKS = [];
		for (int c = 0; c * BD_CHUNK < nb; ++c) { BD_CHUNKS.push(c); }
		BD_X = []; BD_Y = []; BD_Z = [];
		BD_VX = []; BD_VY = []; BD_VZ = [];
		BD_PANIC = [];
		BD_NEXT = [];
		BD_U = [];
		BD_V = [];
		flap = [];
		for (int i = 0; i < nb; ++i) {
			// they stream in from the left, low over the water
			BD_X.push(-80.0 + rng.nextf() * 30.0);
			BD_Y.push(30.0 + rng.nextf() * 20.0);
			BD_Z.push(-20.0 + rng.nextf() * 50.0);
			BD_VX.push(24.0 + rng.nextf() * 4.0);
			BD_VY.push(rng.nextf() * 4.0 - 1.0);
			BD_VZ.push(rng.nextf() * 6.0 - 3.0);
			BD_PANIC.push(0.0);
			BD_NEXT.push(-1);
			float ang = rng.nextf() * 6.2831853;
			float rad = math::sqrt(rng.nextf());
			BD_U.push(math::cos(ang) * rad);
			BD_V.push(math::sin(ang) * rad);
			flap.push(rng.nextf() * 6.28);
		}
		// places in the black sun: a dense core under a thick shell
		BD_SX = []; BD_SY = []; BD_SZ = [];
		for (int i2 = 0; i2 < nb; ++i2) {
			float zz = rng.nextf() * 2.0 - 1.0;
			float az = rng.nextf() * 6.2831853;
			float rr = math::sqrt(1.0 - zz * zz);
			float shell = 0.55 + 0.45 * math::sqrt(rng.nextf());
			BD_SX.push(math::cos(az) * rr * shell);
			BD_SY.push(zz * shell);
			BD_SZ.push(math::sin(az) * rr * shell);
		}
		BD_RA = [];
		for (int i3 = 0; i3 < nb; ++i3) { BD_RA.push(i3 * 2.3999632); }
		BD_HALO = 0.0;
		BD_HSPIN = 0.0;
		BD_DAMP = 0.0;
		BD_BALL = 0.0;
		BD_BURST = 0.0;
		BD_OX = 0.0;
		BD_OZ = 0.0;
		BD_HEAD = gfx.make_grid(BD_GX * BD_GY * BD_GX);
		look_x = -65.0;
		look_z = 5.0;
		focus_x = w * 0.5;
		focus_y = h * 0.5;
	}

	// the backdrop: a sunset gradient with the sun low over the water, streaks of
	// cloud lit from below, and the lake mirroring
	// it all a shade darker, with a glitter path under the sun
	void bake_sky() {
		int bw = w * 2;
		sky_w = bw;
		sky_x0 = w / 2;
		sky = gfx.make_grid(bw * h);
		glint = gfx.make_fgrid(bw * h);
		float sunx = sky_x0 + w * 0.62;
		float suny = horizon - h * 0.07;
		float sr = h * 0.075;
		sun_x = sunx;
		sun_y = suny;
		rays = gfx.make_grid(bw * h);
		for (int y = 0; y < horizon; ++y) {
			for (int x2 = 0; x2 < bw; ++x2) {
				float g = y * 1.0 / horizon;
				float tone = 2.0 + g * g * 19.0;
				float dx = x2 - sunx;
				float dy = (y - suny) * 1.6;
				float d = math::sqrt(dx * dx + dy * dy);
				float glow = d < sr ? 31.0 : 27.0 - (d - sr) * 0.42;
				tone = glow > tone ? glow : tone;
				float cover = (cloud(x2, y) - 0.55) * 1.8;
				cover = cover < 0.0 ? 0.0 : (cover > 1.0 ? 1.0 : cover);
				cover *= g < 0.72 ? 1.0 - g * 0.6 : 0.0;
				if (cover > 0.0) {
					float below = cloud(x2, y + 2) - cloud(x2, y);
					float near_sun = 1.0 - d / (w * 0.8);
					near_sun = near_sun < 0.0 ? 0.0 : near_sun;
					float body = 3.0 + g * 10.0 + near_sun * 5.0;
					float lit = below < 0.0 ? -below * (12.0 + near_sun * 16.0) : 0.0;
					tone = tone + (body + lit - tone) * cover;
				}
				int it = itrunc(tone);
				sky[y * bw + x2] = (it > 31 ? 31 : (it < 0 ? 0 : it)) * 8;
				// god rays: a fan of beams from the sun, broken where the clouds are thick
				float ang = math::atan2(dy, dx);
				float beam = math::sin(ang * 23.0) * 0.6 + math::sin(ang * 9.0 + 1.3) * 0.5;
				float reach = 1.0 - d / (w * 0.75);
				rays[y * bw + x2] = beam > 0.35 && reach > 0.0 && d > sr ? itrunc((beam - 0.35) * reach * (1.0 - cover) * 9.0 * 16.0) : 0;
			}
		}
		for (int y2 = horizon; y2 < h; ++y2) {
			int mirror = 2 * horizon - 1 - y2;
			mirror = mirror < 0 ? 0 : mirror;
			float depth = (y2 - horizon) * 1.0 / (h - horizon);
			for (int x4 = 0; x4 < bw; ++x4) {
				int src = sky[mirror * bw + x4];
				int dk = src % 8;
				int tn = src / 8 - itrunc(2.0 + depth * 6.0);
				tn = tn < 0 ? 0 : tn;
				dk = dk > 1 ? dk : 1 + itrunc(depth * 2.0);
				sky[y2 * bw + x4] = tn * 8 + (dk > 7 ? 7 : dk);
				float spread = 2.0 + depth * 14.0;
				float off = x4 - sunx;
				glint[y2 * bw + x4] = off * off < spread * spread ? x4 * 1.7 + y2 * 2.9 + rng.nextf() * 6.28 : -1.0;
			}
		}
	}

	// cloud streaks: two stretched sine octaves under a band that thickens them in the upper sky
	float cloud(int x, int y) {
		float n = math::sin(x * 0.05 + y * 0.17 + math::sin(y * 0.33) * 1.5) + 0.5 * math::sin(x * 0.13 - y * 0.26 + 2.1);
		return n + math::sin(y * 7.5 / horizon + 0.4) * 0.8;
	}

	// the first strike in slow motion: the flock's clock eases down to a third
	// (the flock's clock reaches the strike at 7.015 s just as the scene's reaches bar 4)
	float slowmo(float st) {
		if (st < 6.565 || st > 7.365) { return 1.0; }
		if (st < 6.765) { return 1.0 - (st - 6.565) / 0.2 * 0.7; }
		if (st > 7.165) { return 0.3 + (st - 7.165) / 0.2 * 0.7; }
		return 0.3;
	}

	// the strike, the veil, then the black sun gathers and bursts on bar ten's downbeat
	int length_bars() override { return 11; }

	array<float> hit_times() override { return [MUSIC_BAR * 4.0, MUSIC_BAR * 5.5, MUSIC_BAR * 8.0, MUSIC_BAR * 10.0]; }
	array<int> hit_levels() override { return [2, 1, 1, 2]; }

	void update(float t, float dt) override {
		tt = t;
		place_camera(t);
		if (dt <= 0.0) { return; }
		float sdt = dt * slowmo(sim_t);
		sim_t += sdt;
		if (strike_t < 0.0 && sim_t >= 7.015) { strike_t = MUSIC_BAR * 4.0; }
		float st = sim_t;
		float step = sdt > 0.05 ? 0.05 : sdt;
		BD_TX = 40.0 * math::sin(st * 0.33);
		BD_TY = 42.0 + 10.0 * math::sin(st * 0.53 + 0.8);
		BD_TZ = 30.0 * math::sin(st * 0.47 + 1.3);
		// the finale: the murmuration sweeps in low and close, a vast veil across the sun
		float veil = 0.0;
		if (t > 9.0) {
			veil = (t - 9.0) / 2.0;
			veil = veil > 1.0 ? 1.0 : veil * veil * (3.0 - 2.0 * veil);
			// in front of the camera, across the sun as the camera sees it now
			float side = (sun_x - w * 0.5) / focal * 62.0;
			BD_TX += (cam_x + fwx * 62.0 + rx_ * side - BD_TX) * veil;
			BD_TY += (cam_y - 1.0 - BD_TY) * veil;
			BD_TZ += (cam_z + fwz * 62.0 + rz_ * side - BD_TZ) * veil;
		}
		BD_FLOOR = 18.0 - veil * 10.0;
		BD_PULL = 0.32 + veil * 1.1;
		shape_sheet(st);
		float kick = cue == null ? 0.0 : cue.kick_env(t);
		BD_FOLD += kick * 7.0;
		black_sun(t, dt, kick);
		// the vortex gathers through bar 5 and lets go into the veil
		float gather = (t - MUSIC_BAR * 4.6) / (MUSIC_BAR * 0.8);
		float release = (t - MUSIC_BAR * 6.0) / (MUSIC_BAR * 0.6);
		gather = gather < 0.0 ? 0.0 : (gather > 1.0 ? 1.0 : gather);
		release = release < 0.0 ? 0.0 : (release > 1.0 ? 1.0 : release);
		BD_VORTEX = gather * (1.0 - release) * 0.85;
		BD_VSPIN = st * 1.6;
		BD_VR = 26.0 + 6.0 * math::sin(st * 0.9);
		float since = strike_t < 0.0 ? -1.0 : t - strike_t;
		BD_FR2 = since >= 0.0 && since < 0.45 ? 1800.0 : 400.0;
		BD_LEN *= 1.0 + veil * 0.3;
		if (burst_t < 0.0 && t >= MUSIC_BAR * 10.0) { burst(t); }
		float ox = BD_FX;
		float oy = BD_FY;
		float oz = BD_FZ;
		place_falcon(st);
		fvx = BD_FX - ox;
		fvy = BD_FY - oy;
		fvz = BD_FZ - oz;
		BD_DT = step;
		follow_flock(step);
		hash_birds();
		var vel = parallel_transform(BD_CHUNKS, boids_steer);
		var& x = BD_X;
		var& y = BD_Y;
		var& z = BD_Z;
		var& vx = BD_VX;
		var& vy = BD_VY;
		var& vz = BD_VZ;
		var& pn = BD_PANIC;
		int i = 0;
		for (int c = 0; c < vel.size(); ++c) {
			var& part = vel[c];
			int m = part.size();
			for (int k = 0; k < m; k += 4) {
				vx[i] = part[k];
				vy[i] = part[k + 1];
				vz[i] = part[k + 2];
				pn[i] = part[k + 3];
				x[i] += vx[i] * step;
				y[i] += vy[i] * step;
				z[i] += vz[i] * step;
				++i;
			}
		}
	}

	// the last act: the veil lifts off the water and gathers into one vast dark
	// sphere hanging over the sun, turning faster and drawing tighter through the
	// riser, swelling on every kick, until the falcon bursts it into a vast halo
	// that slows its turn as it spreads
	void black_sun(float t, float dt, float kick) {
		BD_BALL = ease(MUSIC_BAR * 8.0, MUSIC_BAR * 8.9, t);
		if (BD_BALL <= 0.0) { return; }
		float depth = 85.0;
		float side = (sun_x - w * 0.5) / focal * depth;
		float lift = h * 0.33 / focal * depth;
		float cy = math::cos(anc_yaw);
		float sy = math::sin(anc_yaw);
		BD_BCX = anc_x + cy * depth - sy * side;
		BD_BCY = anc_y + lift;
		BD_BCZ = anc_z + sy * depth + cy * side;
		BD_OX = BD_BCX;
		BD_OZ = BD_BCZ;
		BD_TX += (BD_BCX - BD_TX) * BD_BALL;
		BD_TY += (BD_BCY - BD_TY) * BD_BALL;
		BD_TZ += (BD_BCZ - BD_TZ) * BD_BALL;
		BD_FLOOR = 8.0;
		float tighten = ease(MUSIC_BAR * 9.0, MUSIC_BAR * 10.0, t);
		BD_PULL += (9.0 + tighten * 4.0 - BD_PULL) * BD_BALL;
		BD_DAMP = 2.5 * BD_BALL;
		BD_BR = (12.0 - tighten * 3.5) * (1.0 + kick * 0.14);
		float turn = 1.4 + tighten * 2.6;
		BD_HRX = 0.0 - sy;
		BD_HRZ = cy;
		BD_HFX = cy;
		BD_HFZ = sy;
		if (burst_t >= 0.0) {
			// flung outward, each bird's place runs out from the heart, and the shell
			// settles into a wheeling halo standing on the water round the sun
			float since = t - burst_t;
			float r = 8.5 + 16.0 * (1.0 - math::exp(0.0 - since * 4.0));
			turn = 4.0 * (8.5 / r) * (8.5 / r) + 0.25;
			BD_BR = r;
			BD_PULL = 7.0 + BD_HALO * 9.0;
			BD_DAMP = 2.5 + BD_HALO * 5.5;
			BD_FLOOR = 4.0;
			BD_BURST = since < 0.9 ? 1.0 : 0.0;
			BD_HALO = ease(0.15, 0.75, since);
			BD_HR = (8.5 + 10.5 * (1.0 - math::exp(0.0 - since * 4.0))) * (1.0 + kick * 0.07);
			BD_BCY += (cam_y + BD_HR - BD_BCY) * BD_HALO;
			BD_HSPIN += (0.4 + 2.5 * math::exp(0.0 - since * 2.0)) * dt;
		}
		bspin += turn * dt;
		BD_BCOS = math::cos(bspin);
		BD_BSIN = math::sin(bspin);
	}

	// the falcon strikes the heart: every bird is thrown straight out from it
	void burst(float t) {
		burst_t = MUSIC_BAR * 10.0;
		BD_BURST = 1.0;
		var& vx = BD_VX;
		var& vy = BD_VY;
		var& vz = BD_VZ;
		var& pn = BD_PANIC;
		for (int i = 0; i < nb; ++i) {
			float dx = BD_X[i] - BD_BCX;
			float dy = BD_Y[i] - BD_BCY;
			float dz = BD_Z[i] - BD_BCZ;
			float k = 50.0 / (math::sqrt(dx * dx + dy * dy + dz * dz) + 0.5);
			vx[i] = dx * k;
			vy[i] = dy * k;
			vz[i] = dz * k;
			pn[i] = 1.0;
		}
	}

	// the sheet swings and tilts, stretches into a ribbon and gathers in again,
	// and its fold travels along it: seen edge-on it is a dark line, face-on
	// a pale veil
	void shape_sheet(float t) {
		float yaw = 0.3 + 0.95 * math::sin(t * 0.35);
		float tilt = 0.5 * math::sin(t * 0.29);
		float cy = math::cos(yaw);
		float sy = math::sin(yaw);
		float ct = math::cos(tilt);
		float st = math::sin(tilt);
		BD_AX = cy * ct;
		BD_AY = st;
		BD_AZ = sy * ct;
		float roll = 0.9 + 0.7 * math::sin(t * 0.23 + 0.6);
		float cr = math::cos(roll);
		float sr = math::sin(roll);
		float px = -sy;
		float pz = cy;
		float qx = -cy * st;
		float qy = ct;
		float qz = -sy * st;
		BD_BX = px * cr + qx * sr;
		BD_BY = qy * sr;
		BD_BZ = pz * cr + qz * sr;
		BD_NX = BD_AY * BD_BZ - BD_AZ * BD_BY;
		BD_NY = BD_AZ * BD_BX - BD_AX * BD_BZ;
		BD_NZ = BD_AX * BD_BY - BD_AY * BD_BX;
		BD_LEN = 40.0 + 12.0 * math::sin(t * 0.37);
		BD_WID = 18.0 - 5.0 * math::sin(t * 0.37);
		// as the falcon stoops the flock squeezes into a dense ball
		float squeeze = ball(t, 5.815) + ball(t, 8.324);
		BD_LEN *= 1.0 - squeeze * 0.62;
		BD_WID *= 1.0 - squeeze * 0.4;
		BD_FOLD = 6.0 + 4.0 * math::sin(t * 0.61 + 2.0);
		BD_PHASE = t * 1.3;
	}

	float ball(float t, float at) {
		float d = (t - at) / 1.2;
		return d < 0.0 || d > 1.0 ? 0.0 : math::sin(d * 3.1415927);
	}

	void follow_flock(float step) {
		float sx = 0.0;
		float sy = 0.0;
		float sz = 0.0;
		for (int i = 0; i < nb; ++i) {
			sx += BD_X[i];
			sy += BD_Y[i];
			sz += BD_Z[i];
		}
		cen_x = sx / nb;
		cen_y = sy / nb;
		cen_z = sz / nb;
		float k = step * 3.0;
		look_x += (sx / nb - look_x) * k;
		look_z += (sz / nb - look_z) * k;
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

	// the falcon circles high over the flock and stoops through it twice: each
	// dive is a curve from its circle, through the heart of the cloud, and away
	void place_falcon(float t) {
		float ca = t * 0.8;
		float hx = BD_TX + 70.0 * math::cos(ca);
		float hy = BD_TY + 42.0;
		float hz = BD_TZ + 70.0 * math::sin(ca);
		float dive = -1.0;
		if (t > 6.215 && t < 7.815) { dive = (t - 6.215) / 1.6; }
		if (t > 8.724 && t < 10.324) { dive = (t - 8.724) / 1.6; }
		if (BD_BALL > 0.0) {
			// over the black sun it circles high, then stoops from high behind the
			// sphere through its heart on bar ten's downbeat and on past the lens
			hx = BD_BCX + 30.0 * math::cos(ca * 1.4);
			hy = BD_BCY + 26.0;
			hz = BD_BCZ + 30.0 * math::sin(ca * 1.4);
			float last = (tt - (MUSIC_BAR * 10.0 - 0.75)) / 1.5;
			if (last <= 0.0 || last >= 1.0) {
				BD_FX = hx;
				BD_FY = hy;
				BD_FZ = hz;
				return;
			}
			float sx = BD_BCX + (BD_BCX - cam_x) * 0.8;
			float sy = BD_BCY + 40.0;
			float sz = BD_BCZ + (BD_BCZ - cam_z) * 0.8;
			float lx = cam_x + rx_ * 7.0;
			float ly = cam_y + 3.0;
			float lz = cam_z + rz_ * 7.0;
			float a0 = (1.0 - last) * (1.0 - last);
			float b0 = 2.0 * last * (1.0 - last);
			float c0 = last * last;
			float mx0 = 2.0 * BD_BCX - 0.5 * (sx + lx);
			float my0 = 2.0 * BD_BCY - 0.5 * (sy + ly);
			float mz0 = 2.0 * BD_BCZ - 0.5 * (sz + lz);
			BD_FX = sx * a0 + mx0 * b0 + lx * c0;
			BD_FY = sy * a0 + my0 * b0 + ly * c0;
			BD_FZ = sz * a0 + mz0 * b0 + lz * c0;
			return;
		}
		if (dive < 0.0) {
			BD_FX = hx;
			BD_FY = hy;
			BD_FZ = hz;
			return;
		}
		// the curve's control point is set so it passes through the flock's centre halfway
		float ex = cen_x - (hx - cen_x);
		float ey = cen_y - 30.0;
		float ez = cen_z - (hz - cen_z);
		float a = (1.0 - dive) * (1.0 - dive);
		float b = 2.0 * dive * (1.0 - dive);
		float c = dive * dive;
		float mx = 2.0 * cen_x - 0.5 * (hx + ex);
		float my = 2.0 * cen_y - 0.5 * (hy + ey);
		float mz = 2.0 * cen_z - 0.5 * (hz + ez);
		BD_FX = hx * a + mx * b + ex * c;
		BD_FY = hy * a + my * b + ey * c;
		BD_FZ = hz * a + mz * b + ez * c;
	}

	// on the shore, turning slowly to follow the flock; the pitch stays fixed so
	// the horizon stays where the backdrop was baked
	float ease(float a, float b, float t) {
		float k = (t - a) / (b - a);
		k = k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
		return k * k * (3.0 - 2.0 * k);
	}

	void place_camera(float t) {
		if (t >= MUSIC_BAR * 8.0 && anchored) {
			anchored_camera(t);
			return;
		}
		// from the shore, round the world's centre, the camera glides in to circle the
		// flock, closes to ride inside it, then climbs away for the veil
		float follow = ease(MUSIC_BAR * 1.5, MUSIC_BAR * 3.1, t);
		float close = ease(MUSIC_BAR * 4.3, MUSIC_BAR * 5.1, t) * (1.0 - ease(MUSIC_BAR * 5.9, MUSIC_BAR * 6.5, t));
		float dist = 115.0 - follow * 55.0 - close * 22.0 + ease(MUSIC_BAR * 5.9, MUSIC_BAR * 6.7, t) * 8.0;
		float orbit = ease(MUSIC_BAR * 4.3, MUSIC_BAR * 5.1, t);
		float ang = -1.5707963 + 0.25 * math::sin(t * 0.11) + orbit * 0.9 + follow * 0.25 * math::sin(t * 0.3);
		float ox = look_x * follow;
		float oz = look_z * follow;
		cam_x = ox + dist * math::cos(ang);
		cam_z = oz + dist * math::sin(ang);
		cam_y = 6.0 + follow * 14.0 + close * 12.0 - ease(MUSIC_BAR * 5.9, MUSIC_BAR * 6.7, t) * 14.0;
		float yaw = math::atan2(look_z - cam_z, look_x - cam_x);
		if (!yaw_set) {
			yaw0 = yaw;
			yaw_set = true;
		}
		float dyaw = yaw - yaw0;
		while (dyaw > 3.1415927) { dyaw -= 6.2831853; }
		while (dyaw < -3.1415927) { dyaw += 6.2831853; }
		pan = dyaw * focal * 0.7;
		float lim = w * 0.5 - 1.0;
		pan = pan > lim ? lim : (pan < -lim ? -lim : pan);
		sun_x = w * 0.62 - pan;
		float pitch = math::atan2(horizon - h * 0.5, focal);
		float cp = math::cos(pitch);
		float sp = math::sin(pitch);
		float cy = math::cos(yaw);
		float sy = math::sin(yaw);
		fwx = cy * cp;
		fwy = sp;
		fwz = sy * cp;
		rx_ = -sy;
		rz_ = cy;
		upx = -cy * sp;
		upy = cp;
		upz = -sy * sp;
		if (t >= MUSIC_BAR * 8.0 && !anchored) {
			anchored = true;
			anc_x = cam_x;
			anc_y = cam_y;
			anc_z = cam_z;
			anc_yaw = yaw;
		}
	}

	// the last act's camera: it holds its heading on the sun and cranes up a little
	// (the lens pushes in on the sphere through the riser and lets go on the burst)
	void anchored_camera(float t) {
		cam_x = anc_x;
		cam_z = anc_z;
		cam_y = anc_y + ease(MUSIC_BAR * 8.0, MUSIC_BAR * 9.5, t) * 3.0;
	}

	void render(var& grid) override {
		float push = 0.0;
		if (sim_t > 6.415 && sim_t < 7.515) {
			float u = sim_t < 6.665 ? (sim_t - 6.415) / 0.25 : (sim_t > 7.265 ? (7.515 - sim_t) / 0.25 : 1.0);
			push = u * u * (3.0 - 2.0 * u);
		}
		float shake = sim_t > 9.424 && sim_t < 10.024 ? (10.024 - sim_t) * 5.0 : 0.0;
		float since = burst_t < 0.0 ? -1.0 : tt - burst_t;
		if (since >= 0.0 && since < 0.8) { shake += (0.8 - since) * 7.0; }
		// the riser's push-in on the black sun, let go by the burst
		if (anchored && since < 0.0) {
			push = ease(MUSIC_BAR * 8.6, MUSIC_BAR * 10.0, tt) * 0.6;
		}
		// the kick punches the lens in, the snare a touch less
		float punch = cue == null ? 0.0 : cue.kick_env(tt) * 0.04 + cue.snare_env(tt) * 0.02;
		punch = punch < 0.006 ? 0.0 : punch;
		if (push == 0.0 && shake == 0.0 && punch == 0.0 && flash_level() == 0) {
			compose(grid);
			focus_x = fsx;
			focus_y = fsy;
			flare(grid, 1.0);
			return;
		}
		compose(frame);
		finish(grid, push, shake, punch);
	}

	// the push-in toward the strike, the shake of the second stoop, the flash and the flare
	void finish(var& grid, float push, float shake, float punch) {
		var& fr = frame;
		// the falcon's strike draws the eye; in the last act the black sun over the sun does
		bool lens = anchored && burst_t < 0.0;
		focus_x += ((lens ? sun_x : fsx) - focus_x) * 0.12;
		focus_y += ((lens ? (sun_y + h * 0.4) * 0.5 : fsy) - focus_y) * 0.12;
		float zoom = 1.0 + push * 0.3 + punch;
		float ox = math::sin(tt * 61.0) * shake;
		float oy = math::cos(tt * 47.0) * shake * 0.6;
		float fx = focus_x * push + w * 0.5 * (1.0 - push);
		float fy = focus_y * push + h * 0.5 * (1.0 - push);
		// the zoomed window stays inside the frame
		float mxw = w * 0.5 / zoom;
		float myh = h * 0.5 / zoom;
		fx = fx < mxw ? mxw : (fx > w - mxw ? w - mxw : fx);
		fy = fy < myh ? myh : (fy > h - myh ? h - myh : fy);
		int flash = flash_level();
		var& cols = col_of;
		for (int x0 = 0; x0 < w; ++x0) {
			int sx = itrunc(fx + (x0 + 0.5 - w * 0.5) / zoom + ox);
			cols[x0] = sx < 0 ? 0 : (sx >= w ? w - 1 : sx);
		}
		int i = 0;
		for (int y = 0; y < h; ++y) {
			int sy = itrunc(fy + (y + 0.5 - h * 0.5) / zoom + oy);
			sy = sy < 0 ? 0 : (sy >= h ? h - 1 : sy);
			int rb = sy * w;
			for (int x = 0; x < w; ++x) {
				int v = fr[rb + cols[x]];
				if (flash > 0) {
					int tone = v / 8 + flash;
					int dk = v % 8 - flash / 3;
					v = (tone > 31 ? 31 : tone) * 8 + (dk < 0 ? 0 : dk);
				}
				grid[i] = v;
				++i;
			}
		}
		flare(grid, zoom);
	}

	// the strike's flash: the first stoop's impact, in slow motion, a white-hot beat
	int flash_level() {
		float since = strike_t < 0.0 ? -1.0 : tt - strike_t;
		float since2 = burst_t < 0.0 ? -1.0 : tt - burst_t;
		if (since2 >= 0.0 && since2 < 0.7) { return itrunc((0.7 - since2) * 22.0); }
		if (since < 0.0 || since > 0.5) { return 0; }
		return itrunc((0.5 - since) * 24.0);
	}

	// a lens flare: ghosts strung from the sun through the frame's centre, brighter on the strike
	void flare(var& grid, float zoom) {
		if (zoom > 1.05) { return; }
		float cx = w * 0.5;
		float cy = h * 0.5;
		float dx = cx - sun_x;
		float dy = cy - sun_y;
		float gain = 1.0 + flash_level() * 0.15;
		array<float> at = [0.55, 1.1, 1.45, 1.9];
		array<float> size = [2.5, 5.0, 1.6, 8.0];
		for (int g = 0; g < 4; ++g) {
			float gx = sun_x + dx * at[g];
			float gy = sun_y + dy * at[g];
			float r = size[g] * h / 120.0;
			int x0 = itrunc(gx - r);
			int x1 = itrunc(gx + r) + 1;
			int y0 = itrunc(gy - r);
			int y1 = itrunc(gy + r) + 1;
			for (int y = y0; y <= y1; ++y) {
				if (y < 0 || y >= h) { continue; }
				for (int x = x0; x <= x1; ++x) {
					if (x < 0 || x >= w) { continue; }
					float ex = x + 0.5 - gx;
					float ey = y + 0.5 - gy;
					float q = 1.0 - math::sqrt(ex * ex + ey * ey) / r;
					if (q <= 0.0) { continue; }
					int lift = itrunc((g == 3 ? (q < 0.25 ? q * 4.0 : 0.3) : q) * 5.0 * gain);
					if (lift <= 0) { continue; }
					int v = grid[y * w + x];
					int tone = v / 8 + lift;
					grid[y * w + x] = (tone > 31 ? 31 : tone) * 8 + v % 8;
				}
			}
		}
	}

	void compose(var& grid) {
		int n = w * h;
		var& dn = dens;
		var& gold = lit;
		for (int i = 0; i < n; ++i) {
			dn[i] = 0.0;
			gold[i] = 0.0;
		}
		if (sun_dx == 0.0 && sun_dz == 0.0) {
			// the sun's direction: the ray through its pixel from the camera's first pose
			float sxo = w * 0.62 - w * 0.5;
			float syo = h * 0.5 - (horizon - h * 0.07);
			float dxs = fwx * focal + rx_ * sxo + upx * syo;
			float dys = fwy * focal + upy * syo;
			float dzs = fwz * focal + rz_ * sxo + upz * syo;
			float ls = 1.0 / math::sqrt(dxs * dxs + dys * dys + dzs * dzs);
			sun_dx = dxs * ls;
			sun_dy = dys * ls;
			sun_dz = dzs * ls;
		}
		float half_w = w * 0.5;
		float half_h = h * 0.5;
		float px_scale = w / 180.0;   // bird sizes are in a 180-pixel-wide frame's pixels
		float px_inv = 1.0 / px_scale;
		float since = burst_t < 0.0 ? -1.0 : tt - burst_t;
		float streak = since >= 0.0 && since < 1.4 ? 0.07 * (1.4 - since) : 0.0;
		for (int b = 0; b < nb; ++b) {
			float ex = BD_X[b] - cam_x;
			float ey = BD_Y[b] - cam_y;
			float ez = BD_Z[b] - cam_z;
			float zc = ex * fwx + ey * fwy + ez * fwz;
			if (zc < 14.0) { continue; }
			float iz = focal / zc;
			int sx = half_w + (ex * rx_ + ez * rz_) * iz;
			int sy = half_h - (ex * upx + ey * upy + ez * upz) * iz;
			if (sx < 1 || sx >= w - 1 || sy < 1 || sy >= horizon) { continue; }
			float size = 1.6 * iz * px_inv;
			float beat = math::sin(flap[b] + sim_t * (14.0 + BD_PANIC[b] * 10.0));
			// wings turned to the sun on the upstroke flash gold
			float vl = math::sqrt(BD_VX[b] * BD_VX[b] + BD_VY[b] * BD_VY[b] + BD_VZ[b] * BD_VZ[b]) + 0.001;
			float face = (BD_VX[b] * sun_dx + BD_VY[b] * sun_dy + BD_VZ[b] * sun_dz) / vl;
			if (beat > 0.4 && face > -0.3 && face < 0.3 && size > 0.4) {
				gold[sy * w + sx] += (beat - 0.55) * 4.0 * (size > 1.5 ? 1.5 : size);
				continue;
			}
			if (streak > 0.0) {
				// the sunburst: each bird drawn as a streak back along its flight
				float tx = ex - BD_VX[b] * streak;
				float ty = ey - BD_VY[b] * streak;
				float tz = ez - BD_VZ[b] * streak;
				float tzc = tx * fwx + ty * fwy + tz * fwz;
				if (tzc > 14.0) {
					float tiz = focal / tzc;
					streak_line(sx, sy, itrunc(half_w + (tx * rx_ + tz * rz_) * tiz), itrunc(half_h - (tx * upx + ty * upy + tz * upz) * tiz), size);
					continue;
				}
			}
			if (size > 3.0) {
				wings(sx, sy, size * px_scale, beat, BD_PANIC[b]);
				continue;
			}
			// a bird banking in panic shows its wings: the scare wave reads darker
			float wgt = (size < 1.0 ? size * size : 1.0) * (0.75 + 0.25 * beat + BD_PANIC[b] * 0.8);
			int i2 = sy * w + sx;
			dn[i2] += wgt;
			if (size > 1.4) {
				int lift = beat > 0.3 ? -w : 0;
				dn[i2 - 1 + lift] += wgt * 0.8;
				dn[i2 + 1 + lift] += wgt * 0.8;
				if (px_scale > 1.25) {
					dn[i2 + w] += wgt * 0.7;
					dn[i2 - 2 + lift] += wgt * 0.5;
					dn[i2 + 2 + lift] += wgt * 0.5;
				}
			}
		}
		draw_falcon();
		var& sk = sky;
		var& gl = glint;
		var& ry = rays;
		float kick = cue == null ? 0.0 : cue.kick_env(tt);
		float snare = cue == null ? 0.0 : cue.snare_env(tt);
		float bass = cue == null ? 0.0 : cue.bass_env(tt);
		float breath = cue == null ? 0.0 : cue.riser(tt);
		float blaze = cue == null ? 0.0 : cue.hit(tt);
		int glit_on = itrunc(tt * 9.0) + itrunc(snare * 6.0);
		float glit_cut = 0.8 - snare * 0.25;
		float flock_gain = 1.8 * (1.0 + kick * 0.18);
		// the rays swell with the kick and the bass, draw in before the strike, and the
		// light sinks toward dusk as the scene runs
		float swell = 0.45 + 0.55 * math::sin(tt * 0.45) * math::sin(tt * 0.45) + kick * 0.5 + snare * 0.2 + bass * 0.15 + blaze * 1.6;
		int shine = itrunc(swell * (1.0 - breath * 0.7) * 16.0);
		int dusk = itrunc(tt * 0.22);
		int bw = sky_w;
		int shift = sky_x0 + itrunc(pan);
		for (int y = 0; y < h; ++y) {
			int row = y * w;
			int srow = y * bw + shift;
			if (y < horizon) {
				for (int x = 0; x < w; ++x) {
					int s = sk[srow + x];
					int tone = s / 8;
					int lifted = tone + ((ry[srow + x] * shine) >> 8) - (tone < 27 ? dusk : 0);
					int glow = itrunc(gold[row + x]);
					lifted += glow > 6 ? 6 : glow;
					lifted = lifted < 0 ? 0 : (lifted > 31 ? 31 : lifted);
					int lv = dn[row + x] * flock_gain;
					int dk = s % 8 + lv - glow;
					dk = dk < 0 ? 0 : dk;
					grid[row + x] = lifted * 8 + (dk > 7 ? 7 : dk);
				}
				continue;
			}
			int mirror = 2 * horizon - 1 - y;
			int ripple = itrunc(1.6 * math::sin(y * 1.1 + tt * 2.3));
			for (int x2 = 0; x2 < w; ++x2) {
				int s2 = sk[srow + x2];
				int mx = x2 + ripple;
				mx = mx < 0 ? 0 : (mx >= w ? w - 1 : mx);
				int lv2 = mirror >= 0 ? dn[mirror * w + mx] * 2.0 : 0.0;
				int dk2 = s2 % 8 + lv2;
				int tone = s2 - s2 % 8;
				float ph = gl[srow + x2];
				if (ph >= 0.0 && math::sin(ph + glit_on * 0.7) > glit_cut) { tone = 30 * 8; dk2 = lv2; }
				grid[row + x2] = tone + (dk2 > 7 ? 7 : dk2);
			}
		}
		shockwave(grid);
	}

	// the burst's shockwave: a ring of light racing out from the heart across the sky
	void shockwave(var& grid) {
		float since = burst_t < 0.0 ? -1.0 : tt - burst_t;
		if (since < 0.0 || since > 0.9) { return; }
		float ex = BD_BCX - cam_x;
		float ey = BD_BCY - cam_y;
		float ez = BD_BCZ - cam_z;
		float zc = ex * fwx + ey * fwy + ez * fwz;
		if (zc < 10.0) { return; }
		float iz = focal / zc;
		bsx = w * 0.5 + (ex * rx_ + ez * rz_) * iz;
		bsy = h * 0.5 - (ex * upx + ey * upy + ez * upz) * iz;
		float r1 = 4.0 + since * w * 1.1;
		float r0 = r1 - 2.0 - since * 6.0;
		int lift = itrunc((0.9 - since) * 9.0);
		if (lift <= 0) { return; }
		int y0 = itrunc(bsy - r1);
		int y1 = itrunc(bsy + r1) + 1;
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
				int xa = itrunc(bsx + side * ix);
				int xb = itrunc(bsx + side * ox);
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

	// a bird flung out by the burst: a dark streak from where it was to where it is
	void streak_line(int x0, int y0, int x1, int y1, float size) {
		var& dn = dens;
		int dx = x1 - x0;
		int dy = y1 - y0;
		int n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
		n = n > 40 ? 40 : n;
		float wgt = size > 1.0 ? 1.0 : (size < 0.3 ? 0.3 : size);
		for (int k = 0; k <= n; ++k) {
			int x = n == 0 ? x0 : x0 + dx * k / n;
			int y = n == 0 ? y0 : y0 + dy * k / n;
			if (x < 0 || x >= w || y < 0 || y >= horizon) { continue; }
			dn[y * w + x] += wgt * (1.0 - k * 0.6 / (n + 1));
		}
	}

	// a starling close to the camera: a body and two wings swept up or down with the beat
	void wings(int sx, int sy, float size, float beat, float panic) {
		var& dn = dens;
		int span = itrunc(size * 1.1);
		span = span > 26 ? 26 : span;
		for (int k = -span; k <= span; ++k) {
			float a = (k < 0 ? -k : k) * 1.0 / span;
			int x = sx + k;
			int y = sy - itrunc(a * beat * size * 0.55) + itrunc(a * a * size * 0.2);
			if (x < 0 || x >= w || y < 0 || y >= horizon) { continue; }
			dn[y * w + x] += 1.5;
			if (a < 0.15 && y + 1 < horizon) { dn[(y + 1) * w + x] += 1.2; }
		}
	}

	// the falcon: a swept-wing silhouette, bigger than any starling, its wings
	// along its screen heading
	void draw_falcon() {
		float ex = BD_FX - cam_x;
		float ey = BD_FY - cam_y;
		float ez = BD_FZ - cam_z;
		float zc = ex * fwx + ey * fwy + ez * fwz;
		if (zc < 8.0) { return; }
		float iz = focal / zc;
		float sx = w * 0.5 + (ex * rx_ + ez * rz_) * iz;
		float sy = h * 0.5 - (ex * upx + ey * upy + ez * upz) * iz;
		fsx = sx < 0.0 ? 0.0 : (sx > w ? w : sx);
		fsy = sy < 0.0 ? 0.0 : (sy > horizon ? horizon : sy);
		float hx = fvx * rx_ + fvz * rz_;
		float hy = -(fvx * upx + fvy * upy + fvz * upz);
		float hl = math::sqrt(hx * hx + hy * hy) + 0.0001;
		hx /= hl;
		hy /= hl;
		float span = 4.0 + 7.0 * iz;
		float beat = math::sin(sim_t * 9.0) * 0.6;
		var& dn = dens;
		for (int k = -14; k <= 14; ++k) {
			float a = (k < 0 ? -k : k) / 14.0;
			float s = k / 14.0 * span;
			float back = a * span * 0.55;
			float px = sx - hy * s - hx * back;
			float py = sy + hx * s - hy * back + a * beat * span * 0.4;
			int ix = itrunc(px);
			int iy = itrunc(py);
			if (ix >= 0 && ix < w && iy >= 0 && iy + 1 < horizon) {
				dn[iy * w + ix] += 4.0;
				if (a < 0.6) { dn[(iy + 1) * w + ix] += 4.0; }
			}
		}
		for (int k2 = -2; k2 <= 3; ++k2) {
			int bx = itrunc(sx + hx * k2 * span * 0.18);
			int by = itrunc(sy + hy * k2 * span * 0.18);
			if (bx >= 0 && bx < w && by >= 0 && by < horizon) { dn[by * w + bx] += 3.0; }
		}
	}
}
