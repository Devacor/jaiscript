// MAELSTROM — neon ink in a slab of black water, simulated twice over and seen in
// three dimensions. Under the surface runs Jos Stam's stable fluids: the flow lives
// on a grid a third of the pixel resolution; each frame the forces stir it,
// vorticity confinement hands back the curl the grid smears away (a force along the
// gradient of the curl's magnitude), warm-started SOR projects the velocity
// divergence-free, and the velocity advects itself semi-Lagrangian (trace back,
// sample bilinear). The ink is one number a cell on a grid twice as fine, carried
// by the same flow, and it reads as marbling reads a tray: the amount of ink picks
// a place along a row of eight neon tubes with dark water between them, so wherever
// the ink changes it draws glowing contours the flow folds and no stirring blends
// to mud. The ink also settles, slowly, toward a ramp across the tank, which keeps
// every hue in the water however long it is stirred.
//
// The surface is a height field on the ink's grid obeying the wave equation: what
// lands in it punches a crater with a raised rim (no water added or lost), the
// crater rebounds as a jet and rings run out, reflect off the walls and cross. What
// lands are drops with gravity on them: rain, blobs of ink that fall onto the kick,
// the crowns of spray each landing throws up, and whatever the waves fling off the
// walls. The vortices dish the surface and the drain pulls it into a funnel.
//
// The camera swings about the tank through a shift lens (every column of the screen
// is a vertical plane), so each column casts one ray across the water front to
// back, lifting every sample by its height and keeping the rows no nearer sample
// has covered. The column begins at the cut face of the slab, where the edge's ink
// shines down through the water under the wave's profile. The surface is lit from
// its slope and its height: one key light, a glint where the slope turns the sky to
// the lens, and crests brighter than troughs. Below it all lies a neon grid floor.
//
// The tank opens banded like Jupiter under rain: opposing jets whose shear rolls
// the bands up into billows. A counter-rotating pair of vortices folds them while
// six nozzles orbit the tank and a rod drags a figure-eight through it; the pair
// gives way to one whirlpool whose ramp turns with it, so the bands sweep round as
// arms the faster middle winds ahead of; the whirlpool reverses, and a drain opens
// that swallows the tank into its eye as the camera dives after it.
//
// Scored to its cue: a blob of ink is let go half a second ahead of each kick and
// lands on it, three nozzles pour on the kick and three spout on the snare, the
// kick surges the jets, makes the tubes throb and punches the camera in, the energy
// feeds the curl, each kick sends a wave across the tank (off a side wall, then out
// of the whirlpool's eye), and the hits land as rings that shove the ink outward: two blobs into the pair's
// cores (bar 2), a cannonball into the whirlpool (bar 4, a shock whose wake lays
// the ink back in clean bands, the picture flashing negative), the reversal (bar 6,
// every tube trading its colour while the whole tank sloshes) and the drain's
// plunge (bar 8).

// Globals on purpose: the parallel row and column bodies read them (arrays borrow zero-copy).
array<float> MS_FU = [];     // the flow that carries the ink, flow cells per second
array<float> MS_FV = [];
array<float> MS_INK = [];    // each ink cell's ink, in palette entries
array<float> MS_HOME_X = []; // the ink a cell settles back to: its column's share
array<float> MS_HOME_Y = []; // plus its row's,
array<float> MS_SETTLE = []; // and by whole ink cells of radius from the tank's middle, the share of the way it goes in a frame
array<int> MS_RADIUS = [];   // per ink cell: that radius
array<float> MS_Z = [];      // per ink cell: the surface's height above rest, in ink cells
array<int> MS_LIT = [];      // per ink cell: its brightness level's first palette entry
array<float> MS_GLOW = [];   // by radius: the brightness level of flat water,
array<float> MS_DUSK = [];   // and the share of its light the drain's dark leaves it
array<int> MS_INK_ROWS = [];
array<int> MS_COLS = [];
int MS_W = 0;
int MS_H = 0;
int MS_DW = 0;               // the ink grid
int MS_DH = 0;
int MS_CW = 0;               // the flow grid
int MS_CH = 0;
float MS_RUN = 0.0;          // a frame's step, in ink cells per (flow cell per second)
float MS_CX = 0.0;           // the tank's middle, in ink cells
float MS_CY = 0.0;
float MS_CAM_X = 0.0;        // the camera, in ink cells: where it stands,
float MS_CAM_Y = 0.0;
float MS_CAM_H = 1.0;        // how high,
float MS_FACE_X = 1.0;       // the unit vector it faces along the ground,
float MS_FACE_Y = 0.0;
float MS_FOCAL = 1.0;        // pixels to a cell one cell away,
float MS_HORIZON = 0.0;      // and the row its eye level falls on
float MS_DEPTH = 1.0;        // the slab's depth
float MS_LIGHT_X = 0.0;      // brightness levels a unit of slope toward the key light takes away
float MS_LIGHT_Y = 0.0;
float MS_GLINT_X = 0.0;      // the slope that glints,
float MS_GLINT_Y = 0.0;
float MS_GLINT_NARROW = 1.0; // one over the square of how far from it a slope may stray,
float MS_GLINT = 0.0;        // and the levels it adds
float MS_CREST = 0.0;        // brightness levels a cell of height adds
int MS_TUBES = 0;            // the first palette entry of the tubes as drawn this frame
int MS_PHASE = 0;            // how far along the tubes every cell's ink is shifted
int MS_FLOOR = 0;            // the floor's first palette entry: the dark, then its grid's 8 shades
float MS_GRID = 1.0;         // floor grid lines to an ink cell
float MS_GRID_GAIN = 0.0;    // a line's shade where the camera stands,
float MS_GRID_FADE = 0.0;    // and what a cell of distance takes off it

// one row of ink, on a parallel_transform worker: each cell takes the flow at its
// centre (bilinear over the flow grid, stepped along the row), traces back one
// step, blends the four cells it lands among and settles toward home
function maelstrom_ink_row(int y) -> array<float> {
	int dw = MS_DW;
	array<float> out = [];
	out.resize(dw, 0.0);
	if (y < 1 || y >= MS_DH - 1) { return out; }
	int gw = MS_CW;
	float gymax = MS_CH - 1.001;
	float xmax = dw - 2.001;
	float ymax = MS_DH - 2.001;
	float run = MS_RUN;
	float home_y = MS_HOME_Y[y];
	float qy = y * 0.5 - 0.25;
	qy = qy > gymax ? gymax : qy;
	int gy = cast<int>(qy);
	float ty = qy - gy;
	int g = gy * gw;
	float ua = MS_FU[g] + (MS_FU[g + gw] - MS_FU[g]) * ty;
	float va = MS_FV[g] + (MS_FV[g + gw] - MS_FV[g]) * ty;
	int x = 1;
	int radius_at = y * dw;
	for (int gx = 1; gx < gw; ++gx) {
		++g;
		float ub = MS_FU[g] + (MS_FU[g + gw] - MS_FU[g]) * ty;
		float vb = MS_FV[g] + (MS_FV[g + gw] - MS_FV[g]) * ty;
		float du = (ub - ua) * 0.5;
		float dv = (vb - va) * 0.5;
		float u = ua + du * 0.5;
		float v = va + dv * 0.5;
		int xe = x + 2;
		while (x < xe) {
			float px = x - run * u;
			float py = y - run * v;
			px = px < 1.0 ? 1.0 : (px > xmax ? xmax : px);
			py = py < 1.0 ? 1.0 : (py > ymax ? ymax : py);
			int ix = cast<int>(px);
			int iy = cast<int>(py);
			float sx = px - ix;
			int k = iy * dw + ix;
			int k2 = k + dw;
			float a = MS_INK[k];
			float b = MS_INK[k2];
			float top = a + (MS_INK[k + 1] - a) * sx;
			float bottom = b + (MS_INK[k2 + 1] - b) * sx;
			float ink = top + (bottom - top) * (py - iy);
			out[x] = ink + (home_y + MS_HOME_X[x] - ink) * MS_SETTLE[MS_RADIUS[radius_at + x]];
			u += du;
			v += dv;
			++x;
		}
		ua = ub;
		va = vb;
	}
	return out;
}

// one row of the surface's light, on a parallel_transform worker: each cell's level
// from its slope (the key light's share, and the glint) and its height, at the glow of its radius
function maelstrom_light_row(int y) -> array<int> {
	int dw = MS_DW;
	array<int> out = [];
	out.resize(dw, 0);
	int row = y < 1 ? 1 : (y > MS_DH - 2 ? MS_DH - 2 : y);
	float light_x = MS_LIGHT_X;
	float light_y = MS_LIGHT_Y;
	float glint_x = MS_GLINT_X;
	float glint_y = MS_GLINT_Y;
	float narrow = MS_GLINT_NARROW;
	float glint = MS_GLINT;
	float crest = MS_CREST;
	int k = row * dw + 1;
	for (int x = 1; x < dw - 1; ++x) {
		float sx = MS_Z[k + 1] - MS_Z[k - 1];
		float sy = MS_Z[k + dw] - MS_Z[k - dw];
		int radius = MS_RADIUS[k];
		float lift = MS_Z[k] * crest;
		lift = lift < -2.5 ? -2.5 : (lift > 3.0 ? 3.0 : lift);
		float level = MS_GLOW[radius] + lift - sx * light_x - sy * light_y;
		float ex = sx - glint_x;
		float ey = sy - glint_y;
		float near = 1.0 - (ex * ex + ey * ey) * narrow;
		if (near > 0.0) { level += near * near * glint; }
		if (level < 2.0) { level = 2.0; }
		int whole = cast<int>(level * MS_DUSK[radius]);
		if (whole > 15) { whole = 15; }
		out[x] = whole * 128;
		++k;
	}
	out[0] = out[1];
	out[dw - 1] = out[dw - 2];
	return out;
}

// one column of the picture, on a parallel_transform worker: its ray across the
// ground is clipped to the tank; where it enters, the slab's cut face hangs from
// the waterline; then the ray walks the surface front to back, a step a row, and
// each sample that rises above everything nearer fills the rows it adds. The rows
// nothing covered show the floor and its grid.
function maelstrom_column(int x) -> array<int> {
	int hh = MS_H;
	int dw = MS_DW;
	int tubes = MS_TUBES;
	int phase = MS_PHASE;
	int floor = MS_FLOOR;
	array<int> out = [];
	out.resize(hh, floor);
	float focal = MS_FOCAL;
	float camx = MS_CAM_X;
	float camy = MS_CAM_Y;
	float camh = MS_CAM_H;
	float hz = MS_HORIZON;
	float aside = (x + 0.5 - MS_W * 0.5) / focal;
	float dx = MS_FACE_X - aside * MS_FACE_Y;
	float dy = MS_FACE_Y + aside * MS_FACE_X;
	float xhi = dw - 2.01;
	float yhi = MS_DH - 2.01;
	float t0 = 1.0;
	float t1 = 1000000.0;
	float face = 7.0;
	if (dx > 0.00001) {
		t0 = math::max(t0, (1.0 - camx) / dx);
		t1 = math::min(t1, (xhi - camx) / dx);
	} else if (dx < -0.00001) {
		t0 = math::max(t0, (xhi - camx) / dx);
		t1 = math::min(t1, (1.0 - camx) / dx);
	} else if (camx < 1.0 || camx > xhi) {
		t1 = 0.0;
	}
	float across0 = 1.0;
	if (dy > 0.00001) {
		across0 = (1.0 - camy) / dy;
		t1 = math::min(t1, (yhi - camy) / dy);
	} else if (dy < -0.00001) {
		across0 = (yhi - camy) / dy;
		t1 = math::min(t1, (1.0 - camy) / dy);
	} else if (camy < 1.0 || camy > yhi) {
		t1 = 0.0;
	}
	if (across0 > t0) {
		t0 = across0;
		face = 5.0;
	}
	int ytop = hh;
	int ybase = hh;
	if (t1 > t0) {
		float px0 = camx + t0 * dx;
		float py0 = camy + t0 * dy;
		px0 = px0 < 1.0 ? 1.0 : (px0 > xhi ? xhi : px0);
		py0 = py0 < 1.0 ? 1.0 : (py0 > yhi ? yhi : py0);
		int ix0 = cast<int>(px0);
		int iy0 = cast<int>(py0);
		int k0 = iy0 * dw + ix0;
		float scale0 = focal / t0;
		float waterline = hz + (camh - MS_Z[k0]) * scale0;
		float foot = hz + (camh + MS_DEPTH) * scale0;
		int edge_ink = cast<int>(MS_INK[k0]);
		edge_ink = tubes + ((edge_ink + phase) & 127);
		float dusk = MS_DUSK[MS_RADIUS[k0]];
		float level = face * dusk;
		float fall = (level - dusk) / (foot - waterline + 0.001);
		int ya = 0;
		if (waterline > 0.0) {
			ya = cast<int>(waterline);
		} else {
			level += fall * waterline;
		}
		if (ya > hh) { ya = hh; }
		if (foot < hh) { ybase = foot < 0.0 ? 0 : math::itrunc(foot); }
		if (ybase < ya) { ybase = ya; }
		for (int yf = ya; yf < ybase; ++yf) {
			int whole = cast<int>(level);
			out[yf] = edge_ink + whole * 128;
			level -= fall;
		}
		if (ya < ybase) {
			int meniscus = cast<int>(12.0 * dusk);
			out[ya] = edge_ink + meniscus * 128;
		}
		ytop = ya;
		float t = t0;
		float row_step = 1.0 / (camh * focal);
		while (t < t1 && ytop > 0) {
			float px = camx + t * dx;
			float py = camy + t * dy;
			int ix = cast<int>(px);
			int iy = cast<int>(py);
			float sx = px - ix;
			float sy = py - iy;
			int k = iy * dw + ix;
			int k2 = k + dw;
			float za = MS_Z[k];
			float zb = MS_Z[k2];
			float ztop = za + (MS_Z[k + 1] - za) * sx;
			float zbot = zb + (MS_Z[k2 + 1] - zb) * sx;
			float row = hz + (camh - ztop - (zbot - ztop) * sy) * focal / t;
			if (row < ytop) {
				int ys = row < 0.0 ? 0 : math::itrunc(row);
				if (ys < ytop) {
					float a = MS_INK[k];
					float b = MS_INK[k2];
					float top = a + (MS_INK[k + 1] - a) * sx;
					float bottom = b + (MS_INK[k2 + 1] - b) * sx;
					int ink = cast<int>(top + (bottom - top) * sy);
					int pix = tubes + MS_LIT[k] + ((ink + phase) & 127);
					for (int yy = ys; yy < ytop; ++yy) { out[yy] = pix; }
					ytop = ys;
				}
			}
			float stride = t * t * row_step;
			t += stride < 0.35 ? 0.35 : (stride > 1.0 ? 1.0 : stride);
		}
	}
	float floor_k = (camh + MS_DEPTH) * focal;
	float lines = MS_GRID;
	float line_wide = lines * 1.2 / focal;
	float gain = MS_GRID_GAIN;
	float fade = MS_GRID_FADE;
	int from = 0;
	int to = ytop;
	for (int pass = 0; pass < 2; ++pass) {
		for (int y = from; y < to; ++y) {
			float under = y + 0.5 - hz;
			if (under < 1.0) { continue; }
			float far = floor_k / under;
			float shade = gain - far * fade;
			if (shade < 1.0) { continue; }
			float wide = far * line_wide;
			float gx = (camx + far * dx) * lines + 4096.0;
			float gy = (camy + far * dy) * lines + 4096.0;
			int wx = cast<int>(gx);
			int wy = cast<int>(gy);
			if (gx - wx < wide || gy - wy < wide) {
				int sh = cast<int>(shade);
				out[y] = floor + (sh > 8 ? 8 : sh);
			}
		}
		from = ybase;
		to = hh;
	}
	return out;
}

class Maelstrom : Scene {
	int cw = 0;                  // the flow grid: cw x ch cells, three pixels to a cell
	int ch = 0;
	int dw = 0;                  // the ink grid: two cells to a flow cell
	int dh = 0;
	float unit = 1.0;            // ink cells to one of the reference tank's (80 rows)
	array<float> u = [];         // the flow, flow cells per second,
	array<float> v = [];
	array<float> u1 = [];        // and advected
	array<float> v1 = [];
	array<float> pr = [];        // pressure, kept: each solve starts from the last one
	array<float> dv = [];        // divergence
	array<float> cu = [];        // curl
	array<float> away_x = [];    // per flow cell: the unit vector away from the tank's middle,
	array<float> away_y = [];
	array<int> away_r = [];      // and its radius in whole ink cells
	array<float> radial = [];    // by that radius: the speed outward the rings and the drain give the ink
	array<float> ring_born = [];
	array<float> ring_power = [];
	array<float> ring_lays = [];     // 1 for a shock whose wake lays the ink back in its bands
	int ring_next = 0;
	array<float> hw = [];        // the waves: each ink cell's height above rest,
	array<float> hv = [];        // and how fast it is rising, ink cells a second
	array<float> dish_pair = []; // per ink cell: the share of a vortex's dimple it sits in, the pair's
	array<float> dish_pool = []; // and the whirlpool's
	array<float> drop_x = [];    // what is in the air, ink cells and ink cells a second
	array<float> drop_y = [];
	array<float> drop_z = [];
	array<float> drop_vx = [];
	array<float> drop_vy = [];
	array<float> drop_vz = [];
	array<float> drop_size = []; // a blob's radius; spray and rain have none
	array<int> drop_kind = [];   // -1 none, 0 spray, 1 rain, 2 a blob of ink
	array<int> drop_entry = [];  // its colour, along the tubes
	int drop_next = 0;
	array<float> neon = [        // the tubes: red, green and blue light each
		1.00, 0.08, 0.55,
		0.00, 0.80, 1.00,
		1.00, 0.84, 0.04,
		0.58, 0.20, 1.00,
		0.55, 1.00, 0.00,
		1.00, 0.38, 0.00,
		0.10, 0.35, 1.00,
		0.00, 1.00, 0.50
	];
	int tubes = 0;               // the palette's 128 entries along the tubes, at 16 brightness levels, then all of it negative
	int floor_at = 0;            // then the floor: the dark and 8 shades of grid line, and their negative
	int jets = 5;
	int sor_iters = 8;
	float vorticity = 12.0;
	float jet_speed = 14.0;      // the opening's jets
	float settling = 0.18;       // the share of the way home the ink settles in a second,
	float pool_settling = 0.6;   // and in the whirlpool, where home turns with the water
	float home_rate = 1.4;       // this many radians a second
	float home_turned = 0.0;
	float pair_speed = 16.0;     // how fast each vortex turns at its core's edge, flow cells a second
	float pool_speed = 18.0;
	float drain_speed = 14.0;
	float wave_speed = 1.0;      // ink cells a second: the tank's height in 2 s
	float wave_drag = 1.0;       // ink cells squared a second: what smooths the chop out of the waves
	float gravity = 1.0;         // ink cells a second each second
	float fall_lead = 0.55;      // a blob is let go this long before the kick it lands on,
	float fall_speed = 1.0;      // already falling this fast
	float tt = 0.0;
	float turn = 1.0;            // the way the whirlpool winds
	float orbit = 0.0;           // where the nozzles' ring has turned to
	float cam_yaw = -0.6;        // where the camera's swing has turned to
	int traded = 0;              // the tubes' colours, moved along by the reversal
	float kick_before = 0.0;
	float kick_ahead_before = 0.0;
	float snare_before = 0.0;
	float rain_due = 0.0;
	int blob_tube = 0;
	int blob_turn = 0;
	int spout_turn = 0;
	int paddle_turn = 0;

	float t_pair = MUSIC_BAR * 2.0;
	float t_pool = MUSIC_BAR * 4.0;
	float t_flip = MUSIC_BAR * 6.0;
	float t_drain = MUSIC_BAR * 8.0;

	// the reel picks the grid before build runs, so the budget is set at construction
	Maelstrom() { pixel_budget = 30000; }

	int length_bars() override { return 10; }
	array<float> hit_times() override { return [t_pair, t_pool, t_flip, t_drain]; }
	array<int> hit_levels() override { return [1, 2, 1, 2]; }

	void build() override {
		name = "MAELSTROM";
		handle = "stable fluids under a wave tank . neon ink";
		replay = 20.0;   // the water is its whole history
		cw = (w + 2) / 3;
		ch = (h + 2) / 3;
		dw = cw * 2;
		dh = ch * 2;
		unit = dh / 80.0;
		wave_speed = dh / 2.0;
		wave_drag = 2.0 * unit * unit;
		gravity = 120.0 * unit;
		fall_speed = 30.0 * unit;
		int cn = cw * ch;
		int dn = dw * dh;
		u = gfx.make_fgrid(cn);
		v = gfx.make_fgrid(cn);
		u1 = gfx.make_fgrid(cn);
		v1 = gfx.make_fgrid(cn);
		pr = gfx.make_fgrid(cn);
		dv = gfx.make_fgrid(cn);
		cu = gfx.make_fgrid(cn);
		hw = gfx.make_fgrid(dn);
		hv = gfx.make_fgrid(dn);
		MS_W = w;
		MS_H = h;
		MS_DW = dw;
		MS_DH = dh;
		MS_CW = cw;
		MS_CH = ch;
		MS_CX = dw * 0.5 - 0.5;
		MS_CY = dh * 0.5 - 0.5;
		MS_DEPTH = 10.0 * unit;
		MS_GRID = 4.0 / dh;
		MS_INK = gfx.make_fgrid(dn);
		MS_Z = gfx.make_fgrid(dn);
		MS_LIT = gfx.make_grid(dn);
		MS_HOME_X = gfx.make_fgrid(dw);
		MS_HOME_Y = gfx.make_fgrid(dh);
		MS_INK_ROWS = [];
		for (int r = 0; r < dh; ++r) { MS_INK_ROWS.push(r); }
		MS_COLS = [];
		for (int c = 0; c < w; ++c) { MS_COLS.push(c); }
		mix_tubes();
		int reach = math::itrunc(math::sqrt(dw * dw + dh * dh * 1.0) * 0.5) + 3;
		MS_SETTLE = gfx.make_fgrid(reach);
		MS_GLOW = gfx.make_fgrid(reach);
		MS_DUSK = gfx.make_fgrid(reach);
		radial = gfx.make_fgrid(reach);
		MS_RADIUS = gfx.make_grid(dn);
		dish_pair = gfx.make_fgrid(dn);
		dish_pool = gfx.make_fgrid(dn);
		float pair_core = 5.0 * unit;
		float pool_core = 8.0 * unit;
		for (int y = 0; y < dh; ++y) {
			float ey = y + 0.5 - dh * 0.5;
			for (int x = 0; x < dw; ++x) {
				float ex = x + 0.5 - dw * 0.5;
				float left = ex + dw * 0.23;
				float right = ex - dw * 0.23;
				int at = y * dw + x;
				MS_RADIUS[at] = math::itrunc(math::sqrt(ex * ex + ey * ey));
				dish_pool[at] = 1.0 / (1.0 + (ex * ex + ey * ey) / (pool_core * pool_core));
				dish_pair[at] = 1.0 / (1.0 + (left * left + ey * ey) / (pair_core * pair_core))
					+ 1.0 / (1.0 + (right * right + ey * ey) / (pair_core * pair_core));
			}
		}
		away_x = gfx.make_fgrid(cn);
		away_y = gfx.make_fgrid(cn);
		away_r = gfx.make_grid(cn);
		for (int gy = 0; gy < ch; ++gy) {
			float fy = gy - (ch - 1) * 0.5;
			for (int gx = 0; gx < cw; ++gx) {
				float fx = gx - (cw - 1) * 0.5;
				float far = math::sqrt(fx * fx + fy * fy);
				int i = gy * cw + gx;
				away_x[i] = fx / (far + 0.0001);
				away_y[i] = fy / (far + 0.0001);
				away_r[i] = math::itrunc(far * 2.0);
			}
		}
		for (int k = 0; k < 8; ++k) {
			ring_born.push(-100.0);
			ring_power.push(0.0);
			ring_lays.push(0.0);
		}
		for (int d = 0; d < 192; ++d) {
			drop_x.push(0.0);
			drop_y.push(0.0);
			drop_z.push(0.0);
			drop_vx.push(0.0);
			drop_vy.push(0.0);
			drop_vz.push(0.0);
			drop_size.push(0.0);
			drop_kind.push(-1);
			drop_entry.push(0);
		}
		lay_bands();
		for (int settle = 0; settle < 12; ++settle) { project(); }
		carry_ink(0.0, 0.0);
		shape_surface(0.0);
	}

	// the palette: eight tubes of sixteen entries each, a tube's light swelling from the
	// dark water at its edges to a white-hot core; sixteen levels, from black through
	// full light to the water burning white; then the negative of every entry; then the
	// floor's dark and its grid lines, and their negative
	void mix_tubes() {
		tubes = pal.size();
		array<float> gain = [0.0, 0.08, 0.16, 0.25, 0.35, 0.46, 0.58, 0.72, 0.86, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0];
		array<float> glare = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.12, 0.26, 0.42, 0.6, 0.8, 1.0];
		for (int negative = 0; negative < 2; ++negative) {
			for (int shade = 0; shade < 16; ++shade) {
				for (int e = 0; e < 128; ++e) {
					int hue = (e / 16) * 3;
					float off = ((e % 16) + 0.5) / 16.0 - 0.5;
					float glow = 1.0 - off * off / 0.1156;
					glow = glow < 0.0 ? 0.0 : glow;
					float core = glow > 0.8 ? (glow - 0.8) * 2.2 : 0.0;
					float light = 0.05 + 0.95 * glow;
					float white = core + (1.0 - core) * glare[shade] * (0.3 + 0.7 * glow);
					float r = 3.0 + 252.0 * neon[hue] * light;
					float g = 8.0 + 247.0 * neon[hue + 1] * light;
					float b = 24.0 + 231.0 * neon[hue + 2] * light;
					r = (r + (255.0 - r) * white) * gain[shade];
					g = (g + (255.0 - g) * white) * gain[shade];
					b = (b + (255.0 - b) * white) * gain[shade];
					if (negative == 1) {
						pal.add(255.0 - r, 255.0 - g, 255.0 - b);
					} else {
						pal.add(r, g, b);
					}
				}
			}
		}
		floor_at = pal.size();
		for (int negative2 = 0; negative2 < 2; ++negative2) {
			for (int line = 0; line < 9; ++line) {
				float lit = line / 8.0;
				float r2 = 90.0 * lit;
				float g2 = 14.0 * lit;
				float b2 = 5.0 + 125.0 * lit;
				if (negative2 == 1) {
					pal.add(255.0 - r2, 255.0 - g2, 255.0 - b2);
				} else {
					pal.add(r2, g2, b2);
				}
			}
		}
	}

	// the ramp the ink settles back to: seven tubes to the tank's height, deepening along the unit vector (across, down)
	void lay_home(float across, float down) {
		float tube = 112.0 / dh;
		for (int x = 0; x < dw; ++x) { MS_HOME_X[x] = (x - MS_CX) * across * tube; }
		for (int y = 0; y < dh; ++y) { MS_HOME_Y[y] = 1080.0 + (y - MS_CY) * down * tube; }
	}

	// the opening: the ink in its bands, across jets that run against their neighbours,
	// their seams already rippling so the shear rolls them up
	void lay_bands() {
		lay_home(0.0, 1.0);
		var& ink = MS_INK;
		for (int y = 0; y < dh; ++y) {
			int i = y * dw;
			for (int x = 0; x < dw; ++x) {
				ink[i] = MS_HOME_Y[y];
				++i;
			}
		}
		array<float> phase = [];
		for (int s = 0; s <= jets; ++s) {
			phase.push(rng.nextf() * 6.2831853);
			phase.push(rng.nextf() * 6.2831853);
		}
		for (int gy = 1; gy < ch - 1; ++gy) {
			float across = (gy + 0.5) * jets / ch;
			int seam_at = math::itrunc(across + 0.5);
			float ripple = math::cos(3.14159265 * across);
			ripple = seam_at == 0 || seam_at == jets ? 0.0 : ripple * ripple * ripple * ripple;
			for (int gx = 1; gx < cw - 1; ++gx) {
				int j = gy * cw + gx;
				u[j] = band_jet(gy);
				float along = gx * 6.2831853 / cw;
				v[j] = jet_speed * unit * ripple * (0.3 * math::sin(along * 5.0 + phase[seam_at * 2]) + 0.2 * math::sin(along * 3.0 + phase[seam_at * 2 + 1]));
			}
		}
	}

	// a flow row's place in the opening's jets: alternate ways, shearing at the seams
	float band_jet(int gy) {
		float wave = math::sin(3.14159265 * (gy + 0.5) * jets / ch) * 3.0;
		float steep = math::exp(wave);
		return jet_speed * unit * (steep - 1.0 / steep) / (steep + 1.0 / steep);
	}

	float smooth01(float x) {
		float c = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
		return c * c * (3.0 - 2.0 * c);
	}

	float kick_at(float t) { return cue == null ? 0.0 : cue.kick_env(t); }
	float snare_at(float t) { return cue == null ? 0.0 : cue.snare_env(t); }
	float hit_at(float t) { return cue == null ? 0.0 : cue.hit(t); }

	void update(float t, float dt) override {
		float before = tt;
		tt = t;
		if (dt <= 0.0) { return; }
		vorticity = 9.0 + 9.0 * (cue == null ? 0.6 : cue.energy(t));
		float step = dt > 0.05 ? 0.05 : dt;
		strike(before, t);
		stir(t, step);
		confine(step);
		project();
		advect_flow(step);
		carry_ink(t, step);
		throw_water(before, t, step);
		ripple(step);
		fall(step);
		cam_yaw += turn * (0.35 + 0.5 * smooth01((t - t_drain) / (MUSIC_BAR * 2.0))) * step;
		shape_surface(t);
	}

	// the hits, once each as the clock crosses them: a ring of light for the pair, and a
	// shock for the whirlpool, the reversal (the whole flow turning back on itself, the
	// tank sloshing, every tube trading its colour for the next) and the drain, whose
	// funnel plunges; and each kick, a wave: off a side wall, then out of the whirlpool's eye
	void strike(float before, float t) {
		if (before < t_pair && t >= t_pair) { ring(t_pair, 0.8, 0.0); }
		if (before < t_pool && t >= t_pool) { ring(t_pool, 1.0, 1.0); }
		if (before < t_flip && t >= t_flip) {
			ring(t_flip, 1.0, 1.0);
			turn = 0.0 - turn;
			traded += 16;
			int cn = cw * ch;
			for (int i = 0; i < cn; ++i) {
				u[i] = 0.0 - u[i];
				v[i] = 0.0 - v[i];
				pr[i] = 0.0 - pr[i];
			}
			var& vel = hv;
			float tilt = 12.0 * unit * turn / (dw * 0.5);
			for (int y = 1; y < dh - 1; ++y) {
				int j = y * dw + 1;
				for (int x = 1; x < dw - 1; ++x) {
					vel[j] += (x - MS_CX) * tilt;
					++j;
				}
			}
		}
		if (before < t_drain && t >= t_drain) {
			ring(t_drain, 1.0, 1.0);
			splash(MS_CX, MS_CY, 12.0 * unit, 9.0 * unit);
			crown(MS_CX, MS_CY, 22, 4.0 * unit, 70.0 * unit, 5);
		}
		float kick = kick_at(t);
		if (kick > kick_before + 0.3) {
			if (t > t_pool) {
				ring(t, 0.3 * kick, 0.0);
				bump(MS_CX, MS_CY, 45.0 * unit * kick, 7.0 * unit);
			} else {
				paddle(45.0 * unit * kick);
			}
		}
		kick_before = kick;
	}

	// a side wall shoves the water beside it up and the water beyond that down, the left
	// wall and the right in turn: a wave as long as the tank sets out across it
	void paddle(float speed) {
		var& vel = hv;
		paddle_turn = 1 - paddle_turn;
		for (int y = 1; y < dh - 1; ++y) {
			int row = y * dw;
			for (int k = 0; k < 8; ++k) {
				int x = 1 + k;
				if (paddle_turn == 1) { x = dw - 2 - k; }
				vel[row + x] += speed * math::cos(0.44879895 * k);
			}
		}
	}

	void ring(float born, float power, float lays) {
		ring_born[ring_next] = born;
		ring_power[ring_next] = power;
		ring_lays[ring_next] = lays;
		ring_next = (ring_next + 1) % ring_born.size();
	}

	// the choreography, in ink cells: the opening's jets, then a counter-rotating pair
	// with a rod stirring between them, then one whirlpool (home turning with it) that
	// the drain tightens, six nozzles orbiting from the pair on
	void stir(float t, float step) {
		float cx = MS_CX;
		float cy = MS_CY;
		float kick = kick_at(t);
		float snare = snare_at(t);
		if (t < t_pair) {
			drive_jets((0.5 + 1.6 * kick) * step);
			return;
		}
		float drain = smooth01((t - t_drain) / (MUSIC_BAR * 1.5));
		if (t >= t_pool) {
			float pool = smooth01((t - t_pool) / MUSIC_BEAT);
			home_turned += turn * home_rate * (1.0 + drain) * pool * step;
			lay_home(0.0 - math::sin(home_turned), math::cos(home_turned));
			whirl(cx, cy, turn * unit * (pool_speed + drain_speed * drain) * pool, ch * (0.26 - 0.1 * drain), (4.0 + 2.0 * drain) * step);
		} else {
			float pair = smooth01((t - t_pair) / MUSIC_BEAT);
			whirl(cx - dw * 0.23, cy, 0.0 - pair_speed * unit * pair, ch * 0.2, 3.5 * step);
			whirl(cx + dw * 0.23, cy, pair_speed * unit * pair, ch * 0.2, 3.5 * step);
			float a = t * 1.3;
			float rod_x = cx + dw * 0.3 * math::sin(a);
			float rod_y = cy + dh * 0.27 * math::sin(a * 2.0);
			drag(rod_x, rod_y, dw * 0.195 * math::cos(a), dh * 0.351 * math::cos(a * 2.0), 4.0 * unit);
			bump(rod_x, rod_y, 0.0 - 90.0 * unit * step, 3.5 * unit);
		}
		float rush = cue == null ? 0.0 : cue.riser(t);
		orbit += turn * (0.55 + 1.2 * rush * rush + 1.4 * drain) * step;
		float reach = 1.0 - 0.5 * drain;
		for (int k = 0; k < 6; ++k) {
			float a2 = orbit + k * 1.0471976;
			float ca = math::cos(a2);
			float sa = math::sin(a2);
			bool pours = k % 2 == 0;
			float beat = pours ? kick : snare;
			float aim = pours ? 0.55 : 1.4;
			float speed = (5.0 + 22.0 * beat) * unit / math::sqrt(1.0 + aim * aim);
			float x = cx + ca * dw * 0.37 * reach;
			float y = cy + sa * dh * 0.37 * reach;
			drag(x, y, (0.0 - sa * turn - ca * aim) * speed, (ca * turn - sa * aim) * speed, 3.5 * unit);
			pour(x, y, (pours ? 1.0 : -1.0) * (12.0 + 80.0 * beat) * step, 4.5 * unit);
		}
	}

	// the opening's jets pull the flow back toward their shear
	void drive_jets(float grip) {
		for (int gy = 1; gy < ch - 1; ++gy) {
			float jet = band_jet(gy);
			int i = gy * cw + 1;
			for (int gx = 1; gx < cw - 1; ++gx) {
				u[i] += (jet - u[i]) * grip;
				++i;
			}
		}
	}

	// a disc of `rad` ink cells about ink cell (x, y) pulls the flow toward (vx, vy), hardest at its centre
	void drag(float x, float y, float vx, float vy, float rad) {
		float qx = x * 0.5 - 0.25;
		float qy = y * 0.5 - 0.25;
		float qr = rad * 0.5 + 0.75;
		float qr2 = qr * qr;
		int x0 = math::max(1, math::itrunc(qx - qr));
		int x1 = math::min(cw - 2, math::itrunc(qx + qr) + 1);
		int y0 = math::max(1, math::itrunc(qy - qr));
		int y1 = math::min(ch - 2, math::itrunc(qy + qr) + 1);
		for (int gy = y0; gy <= y1; ++gy) {
			float ey = gy - qy;
			for (int gx = x0; gx <= x1; ++gx) {
				float ex = gx - qx;
				float q = ex * ex + ey * ey;
				if (q >= qr2) { continue; }
				float f = (1.0 - q / qr2) * 0.6;
				int i = gy * cw + gx;
				u[i] += (vx - u[i]) * f;
				v[i] += (vy - v[i]) * f;
			}
		}
	}

	// ink poured into (or, negative, drawn out of) a disc about ink cell (x, y), most at its centre
	void pour(float x, float y, float amount, float rad) {
		var& ink = MS_INK;
		float r2 = rad * rad;
		int x0 = math::max(1, math::itrunc(x - rad));
		int x1 = math::min(dw - 2, math::itrunc(x + rad) + 1);
		int y0 = math::max(1, math::itrunc(y - rad));
		int y1 = math::min(dh - 2, math::itrunc(y + rad) + 1);
		for (int py = y0; py <= y1; ++py) {
			float ey = py - y;
			for (int px = x0; px <= x1; ++px) {
				float ex = px - x;
				float q = ex * ex + ey * ey;
				if (q >= r2) { continue; }
				float fall = 1.0 - q / r2;
				ink[py * dw + px] += amount * fall * fall;
			}
		}
	}

	// a vortex at ink cell (x, y): the flow is pulled toward a swirl about it that turns
	// `speed` flow cells a second at `core` cells out and falls away beyond
	void whirl(float x, float y, float speed, float core, float grip) {
		float qx = x * 0.5 - 0.25;
		float qy = y * 0.5 - 0.25;
		float c2 = core * core;
		float k = 2.0 * speed / core;
		for (int gy = 1; gy < ch - 1; ++gy) {
			float ey = gy - qy;
			int i = gy * cw + 1;
			for (int gx = 1; gx < cw - 1; ++gx) {
				float ex = gx - qx;
				float fall = 1.0 / (1.0 + (ex * ex + ey * ey) / c2);
				float pull = grip * fall;
				u[i] += (0.0 - ey * k * fall - u[i]) * pull;
				v[i] += (ex * k * fall - v[i]) * pull;
				++i;
			}
		}
	}

	// vorticity confinement: push along N x curl, N the unit gradient of |curl|
	void confine(float step) {
		var& uu = u;
		var& vv = v;
		var& c = cu;
		int gw = cw;
		int gh = ch;
		for (int y = 1; y < gh - 1; ++y) {
			int i = y * gw + 1;
			for (int x = 1; x < gw - 1; ++x) {
				c[i] = 0.5 * (vv[i + 1] - vv[i - 1] - uu[i + gw] + uu[i - gw]);
				++i;
			}
		}
		float eps = vorticity * step;
		for (int y2 = 2; y2 < gh - 2; ++y2) {
			int j = y2 * gw + 2;
			for (int x2 = 2; x2 < gw - 2; ++x2) {
				float cr = c[j + 1];
				float cl = c[j - 1];
				float cd = c[j + gw];
				float cn = c[j - gw];
				float gx = (cr < 0.0 ? -cr : cr) - (cl < 0.0 ? -cl : cl);
				float gy = (cd < 0.0 ? -cd : cd) - (cn < 0.0 ? -cn : cn);
				float f = eps * c[j] / (math::sqrt(gx * gx + gy * gy) + 0.00001);
				uu[j] += gy * f;
				vv[j] -= gx * f;
				++j;
			}
		}
	}

	// make the flow divergence-free: solve laplace(p) = div(u) by successive
	// over-relaxation from last frame's pressure (walls are Neumann: a border cell
	// copies its inner neighbour), then subtract grad(p)
	void project() {
		var& uu = u;
		var& vv = v;
		var& p = pr;
		var& dvg = dv;
		int gw = cw;
		int gh = ch;
		for (int y = 1; y < gh - 1; ++y) {
			int i = y * gw + 1;
			for (int x = 1; x < gw - 1; ++x) {
				dvg[i] = 0.5 * (uu[i + 1] - uu[i - 1] + vv[i + gw] - vv[i - gw]);
				++i;
			}
		}
		int last = (gh - 1) * gw;
		for (int it = 0; it < sor_iters; ++it) {
			for (int y2 = 1; y2 < gh - 1; ++y2) {
				int j = y2 * gw + 1;
				for (int x2 = 1; x2 < gw - 1; ++x2) {
					float g = (p[j - 1] + p[j + 1] + p[j - gw] + p[j + gw] - dvg[j]) * 0.25;
					p[j] += 1.7 * (g - p[j]);
					++j;
				}
			}
			for (int x3 = 0; x3 < gw; ++x3) {
				p[x3] = p[x3 + gw];
				p[last + x3] = p[last - gw + x3];
			}
			for (int y3 = 0; y3 < gh; ++y3) {
				int r = y3 * gw;
				p[r] = p[r + 1];
				p[r + gw - 1] = p[r + gw - 2];
			}
		}
		for (int y4 = 1; y4 < gh - 1; ++y4) {
			int k = y4 * gw + 1;
			for (int x4 = 1; x4 < gw - 1; ++x4) {
				uu[k] -= 0.5 * (p[k + 1] - p[k - 1]);
				vv[k] -= 0.5 * (p[k + gw] - p[k - gw]);
				++k;
			}
		}
	}

	// semi-Lagrangian: each cell traces its velocity back one step and takes the
	// bilinear blend of the four cells it lands among; the walls hold still and
	// the water slowly calms
	void advect_flow(float step) {
		var& uu = u;
		var& vv = v;
		var& un = u1;
		var& vn = v1;
		int gw = cw;
		int gh = ch;
		float xmax = gw - 1.001;
		float ymax = gh - 1.001;
		float keep = 1.0 - 0.15 * step;
		for (int y = 1; y < gh - 1; ++y) {
			int i = y * gw + 1;
			for (int x = 1; x < gw - 1; ++x) {
				float px = x - step * uu[i];
				float py = y - step * vv[i];
				px = px < 0.0 ? 0.0 : (px > xmax ? xmax : px);
				py = py < 0.0 ? 0.0 : (py > ymax ? ymax : py);
				int ix = cast<int>(px);
				int iy = cast<int>(py);
				float sx = px - ix;
				float sy = py - iy;
				int k = iy * gw + ix;
				float top = uu[k] + (uu[k + 1] - uu[k]) * sx;
				float bot = uu[k + gw] + (uu[k + gw + 1] - uu[k + gw]) * sx;
				un[i] = (top + (bot - top) * sy) * keep;
				float top2 = vv[k] + (vv[k + 1] - vv[k]) * sx;
				float bot2 = vv[k + gw] + (vv[k + gw + 1] - vv[k + gw]) * sx;
				vn[i] = (top2 + (bot2 - top2) * sy) * keep;
				++i;
			}
		}
		u = u1;
		v = v1;
	}

	// what the tank's middle sends out, by radius: every ring's lit crest, dark wake and
	// shove outward (a shock's wake also lays the ink back in its bands), the drain's
	// pull inward, its white eye and the dark closing in on it
	void radiate(float t, float step) {
		var& settle = MS_SETTLE;
		var& push = radial;
		var& lit = MS_GLOW;
		var& kept = MS_DUSK;
		int n = lit.size();
		float drain = smooth01((t - t_drain) / MUSIC_BAR);
		float closing = smooth01((t - t_drain - MUSIC_BAR * 0.5) / (MUSIC_BAR * 1.5));
		float dark_at = n * (1.0 - closing) + 11.0 * unit * closing;
		float dusk = 0.25 / unit;
		float eye = 3.5 * unit;
		float mouth = 9.0 * unit;
		float pull = 26.0 * unit * drain;
		float calm = (settling + (pool_settling - settling) * smooth01((t - t_pool) / MUSIC_BEAT)) * step;
		float full = 8.0 + 2.5 * hit_at(t);
		for (int r = 0; r < n; ++r) {
			float m = r / mouth;
			float q = r / eye;
			push[r] = 0.0 - pull * m / (1.0 + m * m);
			lit[r] = full + 5.0 * drain / (1.0 + q * q * q * q);
			settle[r] = calm;
		}
		float wide = 3.5 * unit;
		for (int k = 0; k < ring_born.size(); ++k) {
			float age = t - ring_born[k];
			if (age < 0.0 || age > 2.2) { continue; }
			float amp = ring_power[k] * math::exp(0.0 - age * 1.5);
			float at = (3.0 + age * 48.0) * unit;
			int r0 = math::max(0, math::itrunc(at - wide * 2.6));
			int r1 = math::min(n - 1, math::itrunc(at + wide) + 1);
			for (int r2 = r0; r2 <= r1; ++r2) {
				float e = (r2 - at) / wide;
				if (e > -1.0) {
					float crest = 1.0 - e * e;
					if (crest > 0.0) {
						lit[r2] += 3.4 * amp * crest;
						push[r2] += 24.0 * unit * amp * crest;
					}
				} else {
					float wake = 1.0 - (e + 1.8) * (e + 1.8) / 0.64;
					if (wake > 0.0) {
						lit[r2] -= 3.0 * amp * wake;
						settle[r2] += ring_lays[k] * wake;
					}
				}
			}
		}
		for (int r3 = 0; r3 < n; ++r3) {
			if (settle[r3] > 1.0) { settle[r3] = 1.0; }
			float left = (dark_at - r3) * dusk;
			kept[r3] = left < 0.0 ? 0.0 : (left > 1.0 ? 1.0 : left);
		}
	}

	// the ink rides the flow plus what the rings and the drain add (a push the projection
	// would cancel, so the flow itself never takes it); its rows run in parallel and
	// rebuild the ink once every row has read the old one
	void carry_ink(float t, float step) {
		radiate(t, step);
		MS_FU = u;
		MS_FV = v;
		var& fu = MS_FU;
		var& fv = MS_FV;
		int cn = cw * ch;
		for (int i = 0; i < cn; ++i) {
			float out = radial[away_r[i]];
			fu[i] += away_x[i] * out;
			fv[i] += away_y[i] * out;
		}
		MS_RUN = step * 2.0;
		var rows = parallel_transform(MS_INK_ROWS, maelstrom_ink_row);
		var& ink = MS_INK;
		ink.clear();
		for (int r = 0; r < rows.size(); ++r) { ink.append(rows[r]); }
	}

	// the wave equation on the ink's grid, in steps short enough that a wave crosses less
	// than a cell in one: each cell is pulled toward the mean of its four neighbours and
	// its speed toward theirs (which calms the chop and leaves the swell); a wall cell
	// copies its inner neighbour, so waves reflect
	void ripple(float step) {
		var& hgt = hw;
		var& vel = hv;
		int gw = dw;
		int gh = dh;
		int subs = math::itrunc(step * wave_speed / 0.6) + 1;
		float part = step / subs;
		float pull = wave_speed * wave_speed * part;
		float calm = wave_drag * part;
		float keep = 1.0 - 0.8 * part;
		int last = (gh - 1) * gw;
		for (int sub = 0; sub < subs; ++sub) {
			for (int y = 1; y < gh - 1; ++y) {
				int i = y * gw + 1;
				for (int x = 1; x < gw - 1; ++x) {
					float here = hgt[i];
					float speed = vel[i];
					vel[i] = (speed + pull * (hgt[i - 1] + hgt[i + 1] + hgt[i - gw] + hgt[i + gw] - 4.0 * here)
						+ calm * (vel[i - 1] + vel[i + 1] + vel[i - gw] + vel[i + gw] - 4.0 * speed)) * keep;
					++i;
				}
			}
			for (int y2 = 1; y2 < gh - 1; ++y2) {
				int j = y2 * gw + 1;
				for (int x2 = 1; x2 < gw - 1; ++x2) {
					hgt[j] += vel[j] * part;
					++j;
				}
			}
			for (int x3 = 0; x3 < gw; ++x3) {
				hgt[x3] = hgt[x3 + gw];
				hgt[last + x3] = hgt[last - gw + x3];
			}
			for (int y3 = 0; y3 < gh; ++y3) {
				int r = y3 * gw;
				hgt[r] = hgt[r + 1];
				hgt[r + gw - 1] = hgt[r + gw - 2];
			}
		}
	}

	// something lands at ink cell (x, y): a crater `deep` at its middle with a raised rim,
	// which between them hold as much water as the flat they replace
	void splash(float x, float y, float deep, float rad) {
		var& hgt = hw;
		float r2 = rad * rad;
		int x0 = math::max(1, math::itrunc(x - rad));
		int x1 = math::min(dw - 2, math::itrunc(x + rad) + 1);
		int y0 = math::max(1, math::itrunc(y - rad));
		int y1 = math::min(dh - 2, math::itrunc(y + rad) + 1);
		for (int py = y0; py <= y1; ++py) {
			float ey = py - y;
			for (int px = x0; px <= x1; ++px) {
				float ex = px - x;
				float q = (ex * ex + ey * ey) / r2;
				if (q >= 1.0) { continue; }
				hgt[py * dw + px] -= deep * (1.0 - q) * (1.0 - q) * (1.0 - 4.0 * q);
			}
		}
	}

	// a disc of the surface about ink cell (x, y) is thrown upward (or, negative, pressed
	// down), fastest at its middle, its rim going the other way by as much water
	void bump(float x, float y, float speed, float rad) {
		var& vel = hv;
		float r2 = rad * rad;
		int x0 = math::max(1, math::itrunc(x - rad));
		int x1 = math::min(dw - 2, math::itrunc(x + rad) + 1);
		int y0 = math::max(1, math::itrunc(y - rad));
		int y1 = math::min(dh - 2, math::itrunc(y + rad) + 1);
		for (int py = y0; py <= y1; ++py) {
			float ey = py - y;
			for (int px = x0; px <= x1; ++px) {
				float ex = px - x;
				float q = (ex * ex + ey * ey) / r2;
				if (q >= 1.0) { continue; }
				vel[py * dw + px] += speed * (1.0 - q) * (1.0 - q) * (1.0 - 4.0 * q);
			}
		}
	}

	void drop(int kind, float size, int entry, float x, float y, float z, float vx, float vy, float vz) {
		int i = drop_next;
		drop_next = (drop_next + 1) % drop_kind.size();
		drop_kind[i] = kind;
		drop_size[i] = size;
		drop_entry[i] = entry;
		drop_x[i] = x;
		drop_y[i] = y;
		drop_z[i] = z;
		drop_vx[i] = vx;
		drop_vy[i] = vy;
		drop_vz[i] = vz;
	}

	// the bright part of the tube the ink at ink cell (x, y) lies in
	int tube_at(float x, float y) {
		int ink = cast<int>(MS_INK[math::itrunc(y) * dw + math::itrunc(x)]);
		return (((ink + traded) & 127) / 16) * 16 + 5;
	}

	// a blob let go over ink cell (x, y) so that it lands `fall_lead` from now
	void let_fall(float x, float y, float size) {
		blob_tube = (blob_tube + 3) % 8;
		float from = fall_speed * fall_lead + 0.5 * gravity * fall_lead * fall_lead;
		drop(2, size, blob_tube * 16 + 5, x, y, from, 0.0, 0.0, 0.0 - fall_speed);
	}

	// `count` drops of spray thrown up and outward from a ring about ink cell (x, y)
	void crown(float x, float y, int count, float rad, float up, int entry) {
		float from = MS_Z[math::itrunc(y) * dw + math::itrunc(x)] + 0.6;
		for (int j = 0; j < count; ++j) {
			float a = rng.nextf() * 6.2831853;
			float out = (5.0 + 9.0 * rng.nextf()) * unit;
			float ca = math::cos(a);
			float sa = math::sin(a);
			drop(0, 0.0, entry, x + ca * rad, y + sa * rad, from, ca * out, sa * out, up * (0.55 + 0.6 * rng.nextf()));
		}
	}

	// what the cue throws at the water: rain over the opening, a blob let go ahead of
	// every kick (anywhere at first, then over the pouring nozzles in turn), the hits'
	// own blobs, a spout from a drawing nozzle on each snare, and spray wherever a
	// wave climbs a wall
	void throw_water(float before, float t, float step) {
		float cx = MS_CX;
		float cy = MS_CY;
		rain_due += (t < t_pair ? 7.0 : (t < t_drain ? 1.5 : 0.0)) * step;
		while (rain_due >= 1.0) {
			rain_due -= 1.0;
			float rx = 2.0 + rng.nextf() * (dw - 5.0);
			float ry = 2.0 + rng.nextf() * (dh - 5.0);
			drop(1, 0.0, tube_at(rx, ry), rx, ry, 34.0 * unit, 0.0, 0.0, -25.0 * unit);
		}
		float lands = t + fall_lead;
		float ahead = kick_at(lands);
		bool scripted = math::abs(lands - t_pair) < 0.25 || math::abs(lands - t_pool) < 0.25 || math::abs(lands - t_drain) < 0.25;
		if (ahead > kick_ahead_before + 0.3 && !scripted && lands < t_drain + MUSIC_BAR) {
			if (lands < t_pair) {
				let_fall(cx + (rng.nextf() - 0.5) * dw * 0.7, cy + (rng.nextf() - 0.5) * dh * 0.7, 2.2 * unit);
			} else {
				float reach = 1.0 - 0.5 * smooth01((lands - t_drain) / (MUSIC_BAR * 1.5));
				float a = orbit + turn * 0.55 * fall_lead + blob_turn * 2.0943951;
				blob_turn = (blob_turn + 1) % 3;
				let_fall(cx + math::cos(a) * dw * 0.37 * reach, cy + math::sin(a) * dh * 0.37 * reach, 2.2 * unit);
			}
		}
		kick_ahead_before = ahead;
		float pair_falls = t_pair - fall_lead;
		if (before < pair_falls && t >= pair_falls) {
			let_fall(cx - dw * 0.23, cy, 3.0 * unit);
			let_fall(cx + dw * 0.23, cy, 3.0 * unit);
		}
		float pool_falls = t_pool - fall_lead;
		if (before < pool_falls && t >= pool_falls) { let_fall(cx, cy, 4.5 * unit); }
		float snare = snare_at(t);
		if (t >= t_pair && t < t_drain + MUSIC_BAR && snare > snare_before + 0.3) {
			float reach2 = 1.0 - 0.5 * smooth01((t - t_drain) / (MUSIC_BAR * 1.5));
			float a2 = orbit + (1 + 2 * spout_turn) * 1.0471976;
			spout_turn = (spout_turn + 1) % 3;
			float sx = cx + math::cos(a2) * dw * 0.37 * reach2;
			float sy = cy + math::sin(a2) * dh * 0.37 * reach2;
			bump(sx, sy, 50.0 * unit, 5.0 * unit);
			crown(sx, sy, 5, 1.5 * unit, 60.0 * unit, tube_at(sx, sy));
		}
		snare_before = snare;
		for (int s = 0; s < 10; ++s) {
			int side = rng.next(4);
			float along = rng.nextf();
			float wx = 2.0 + along * (dw - 5.0);
			float wy = 2.0 + along * (dh - 5.0);
			float in_x = 0.0;
			float in_y = 0.0;
			if (side == 0) {
				wy = 2.0;
				in_y = 1.0;
			} else if (side == 1) {
				wy = dh - 3.0;
				in_y = -1.0;
			} else if (side == 2) {
				wx = 2.0;
				in_x = 1.0;
			} else {
				wx = dw - 3.0;
				in_x = -1.0;
			}
			int k = math::itrunc(wy) * dw + math::itrunc(wx);
			if (hw[k] > 3.0 * unit && hv[k] > 0.0) {
				drop(0, 0.0, tube_at(wx, wy), wx, wy, MS_Z[k] + 0.6, in_x * 5.0 * unit, in_y * 5.0 * unit, 30.0 * unit + 0.5 * hv[k]);
			}
		}
	}

	// everything in the air falls; what meets the water on its way down lands, and what
	// leaves the tank falls past it to the floor
	void fall(float step) {
		float pull = gravity * step;
		float xhi = dw - 2.0;
		float yhi = dh - 2.0;
		float bed = 0.0 - MS_DEPTH;
		int n = drop_kind.size();
		for (int i = 0; i < n; ++i) {
			int kind = drop_kind[i];
			if (kind < 0) { continue; }
			drop_vz[i] -= pull;
			float x = drop_x[i] + drop_vx[i] * step;
			float y = drop_y[i] + drop_vy[i] * step;
			float z = drop_z[i] + drop_vz[i] * step;
			drop_x[i] = x;
			drop_y[i] = y;
			drop_z[i] = z;
			if (x < 1.0 || x > xhi || y < 1.0 || y > yhi) {
				if (z < bed) { drop_kind[i] = -1; }
				continue;
			}
			if (drop_vz[i] > 0.0 || z > MS_Z[math::itrunc(y) * dw + math::itrunc(x)]) { continue; }
			drop_kind[i] = -1;
			land(kind, drop_size[i], drop_entry[i], x, y);
		}
	}

	// spray makes a ripple, rain a small crater and a couple of drops, a blob a crater
	// to its size, a crown of spray and its ink
	void land(int kind, float size, int entry, float x, float y) {
		if (kind == 0) {
			splash(x, y, 0.4 * unit, math::max(3.0 * unit, 2.5));
			return;
		}
		if (kind == 1) {
			splash(x, y, 2.0 * unit, math::max(4.0 * unit, 3.0));
			crown(x, y, 2, 1.0 * unit, 26.0 * unit, entry);
			return;
		}
		float big = size / (2.2 * unit);
		splash(x, y, 8.0 * unit * big, 5.5 * unit * big);
		pour(x, y, 36.0 * big, 4.5 * unit * big);
		crown(x, y, math::itrunc(8.0 * big * big), 2.4 * unit * big, 46.0 * unit * math::sqrt(big), entry);
	}

	// the camera swings about the tank's long side, framing it whole from wherever it
	// stands: it punches in on the kick and the hits, shakes with a hit, and over the
	// drain climbs and dives at the eye
	void place_camera(float t) {
		float flash = hit_at(t);
		float dive = smooth01((t - t_drain) / (MUSIC_BAR * 2.0));
		float yaw = 1.5707963 + 0.36 * math::sin(cam_yaw);
		float fx = math::cos(yaw);
		float fy = math::sin(yaw);
		float half_w = dw * 0.5;
		float half_h = dh * 0.5;
		float dist = 0.9 * math::sqrt(dw * dw + dh * dh * 1.0);
		float along_a = math::abs(half_w * fx + half_h * fy);
		float aside_a = math::abs(half_h * fx - half_w * fy);
		float along_b = math::abs(half_w * fx - half_h * fy);
		float aside_b = math::abs(half_h * fx + half_w * fy);
		float along = along_a > along_b ? along_a : along_b;
		float near = dist - along;
		float far = dist + along;
		float fit_a = (dist - along_a) / (aside_a + 0.001);
		float fit_b = (dist - along_b) / (aside_b + 0.001);
		float camh = dist * (0.5 + 0.5 * dive);
		float wide = 0.48 * w * (fit_a < fit_b ? fit_a : fit_b);
		float tall = 0.78 * h / ((camh + MS_DEPTH) / near - camh / far);
		float whole = wide < tall ? wide : tall;
		float focal = whole * (1.0 + 1.6 * dive);
		float hz_whole = 0.61 * h - 0.5 * whole * ((camh + MS_DEPTH) / near + camh / far);
		float hz_eye = 0.5 * h - camh * focal / dist;
		float hz = hz_whole + (hz_eye - hz_whole) * dive;
		float punch = 1.0 + 0.035 * kick_at(t) + 0.08 * flash;
		float mid = h * 0.5;
		MS_FOCAL = focal * punch;
		MS_HORIZON = mid - (mid - hz) * punch + math::sin(t * 53.0) * flash * flash * h * 0.02;
		MS_CAM_H = camh;
		MS_FACE_X = fx;
		MS_FACE_Y = fy;
		MS_CAM_X = dw * 0.5 - fx * dist;
		MS_CAM_Y = dh * 0.5 - fy * dist;
		MS_GLINT_X = 0.5 * fx;
		MS_GLINT_Y = 0.5 * fy;
		float closing = smooth01((t - t_drain - MUSIC_BAR * 0.5) / (MUSIC_BAR * 1.5));
		float reach = far * 1.5 - near * 0.8;
		MS_GRID_FADE = 7.0 / reach;
		MS_GRID_GAIN = (7.0 + near * 0.8 * MS_GRID_FADE + 1.5 * kick_at(t)) * (1.0 - closing);
	}

	// what the picture reads: the waves with the vortices' dimples and the drain's funnel
	// pressed into them, the camera, and the surface's light from its slopes
	void shape_surface(float t) {
		place_camera(t);
		float pool = smooth01((t - t_pool) / MUSIC_BEAT);
		float pair = smooth01((t - t_pair) / MUSIC_BEAT) * (1.0 - pool);
		float drain = smooth01((t - t_drain) / (MUSIC_BAR * 1.5));
		float pair_deep = 5.0 * unit * pair;
		float pool_deep = 6.0 * unit * pool;
		float drain_deep = 20.0 * unit * drain;
		var& z = MS_Z;
		var& hgt = hw;
		var& pair_dish = dish_pair;
		var& pool_dish = dish_pool;
		int dn = dw * dh;
		for (int i = 0; i < dn; ++i) {
			float f = pool_dish[i];
			z[i] = hgt[i] - pair_deep * pair_dish[i] - (pool_deep + drain_deep * f) * f;
		}
		MS_LIGHT_X = 1.4;
		MS_LIGHT_Y = 1.9;
		MS_GLINT_NARROW = 1.0 / (0.42 * 0.42);
		MS_GLINT = 7.0;
		MS_CREST = 0.4 / unit;
		var rows = parallel_transform(MS_INK_ROWS, maelstrom_light_row);
		var& lit = MS_LIT;
		lit.clear();
		for (int r = 0; r < rows.size(); ++r) { lit.append(rows[r]); }
	}

	// what is in the air, over the finished picture: spray and rain as short streaks, blobs as discs
	void draw_drops(var& grid) {
		float fx = MS_FACE_X;
		float fy = MS_FACE_Y;
		float camx = MS_CAM_X;
		float camy = MS_CAM_Y;
		float camh = MS_CAM_H;
		float focal = MS_FOCAL;
		float hz = MS_HORIZON;
		int ww = w;
		int hh = h;
		float mid = ww * 0.5;
		int spray = MS_TUBES + 11 * 128;
		int blob = MS_TUBES + 14 * 128;
		int n = drop_kind.size();
		for (int i = 0; i < n; ++i) {
			if (drop_kind[i] < 0) { continue; }
			float ex = drop_x[i] - camx;
			float ey = drop_y[i] - camy;
			float depth = ex * fx + ey * fy;
			if (depth < 4.0) { continue; }
			float scale = focal / depth;
			float px = mid + (ey * fx - ex * fy) * scale;
			float py = hz + (camh - drop_z[i]) * scale;
			float rad = drop_size[i] * scale;
			if (rad < 0.9) {
				float tail = py + drop_vz[i] * 0.03 * scale;
				float top = py < tail ? py : tail;
				float bottom = py < tail ? tail : py;
				if (px < 0.0 || px >= ww || bottom < 0.0 || top >= hh) { continue; }
				int sx = cast<int>(px);
				int ya = top < 0.0 ? 0 : math::itrunc(top);
				int yb = bottom >= hh ? hh - 1 : math::itrunc(bottom);
				if (yb > ya + 3) { yb = ya + 3; }
				int pix = spray + drop_entry[i];
				for (int y = ya; y <= yb; ++y) { grid[y * ww + sx] = pix; }
				continue;
			}
			int reach = cast<int>(rad + 1.0);
			int cx = math::itrunc(px + 1000.0) - 1000;
			int cy = math::itrunc(py + 1000.0) - 1000;
			float r2 = rad * rad + 0.3;
			int core = blob + drop_entry[i];
			int rim = spray + drop_entry[i];
			for (int oy = 0 - reach; oy <= reach; ++oy) {
				int y2 = cy + oy;
				if (y2 < 0 || y2 >= hh) { continue; }
				for (int ox = 0 - reach; ox <= reach; ++ox) {
					int x2 = cx + ox;
					float q = ox * ox + oy * oy;
					if (x2 < 0 || x2 >= ww || q > r2) { continue; }
					if (q * 2.5 < r2) {
						grid[y2 * ww + x2] = core;
					} else {
						grid[y2 * ww + x2] = rim;
					}
				}
			}
		}
	}

	void render(var& grid) override {
		// the shocks flash the picture negative
		float since_pool = tt - t_pool;
		float since_drain = tt - t_drain;
		bool negative = (since_pool >= 0.0 && since_pool < 0.1) || (since_drain >= 0.0 && since_drain < 0.1);
		int ww = w;
		int hh = h;
		MS_W = ww;
		MS_H = hh;
		MS_TUBES = negative ? tubes + 2048 : tubes;
		MS_FLOOR = negative ? floor_at + 9 : floor_at;
		MS_PHASE = traded + math::itrunc(3.5 * kick_at(tt));
		var cols = parallel_transform(MS_COLS, maelstrom_column);
		if (grid.size() != ww * hh) { grid.resize(ww * hh, 0); }
		for (int x = 0; x < ww; ++x) {
			var& col = cols[x];
			int gi = x;
			for (int y = 0; y < hh; ++y) {
				grid[gi] = col[y];
				gi += ww;
			}
		}
		draw_drops(grid);
	}
}
