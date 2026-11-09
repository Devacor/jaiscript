// FINALE — JaiScript in eight dimensions: a music video at the soundtrack's 124
// bpm. After the title card it counts the language in, ONE to EIGHT, a slide of
// four bars each (scenes/show.jai): a code card types itself, a live demonstration
// plays beside it, captions run along the lower third, and every slide opens one
// more axis of an 8-cube that turns in a window above the demonstration, the way a
// world turns in Fez: eased quarter-turns snapping on the beat, each a symmetry of
// the cube. The cube sits on Escher's hyperbolic plane: the {8,3} tiling (octagons
// meeting three at a corner) in the Poincare disk, opening ring by ring with the
// dimensions and mirrored through the circle at infinity to fill the screen. With
// all eight shown the cube takes the whole screen, the snaps double up every half
// beat, the scene turns, swirls and ripples as one and the plane accelerates into
// a warp; then it shatters into a vortex of sparks that wind onto JAISCRIPT, which
// slams in with a flash, shockwaves and a shake as the fire wall erupts, pumping
// in rainbow on the beat with the plane behind it, before the neon and the
// credits. The fire is the fire scene's: this class extends it, and the letters
// are heat sources stamped from the pixel font.

// The {8,3} tiling, one row per parallel_transform body. A pixel is first carried
// through the scene's warp (a turn, a swirl tightest at the centre, a ripple
// running outward), which also names the pixel of the cube's light it shows. Its
// disk point is translated by the flow's Mobius map, then folded into the tiling's
// fundamental triangle by its three mirrors (the x axis, the line at pi/8 and the
// central octagon's edge circle). The tiling is three-colourable: the line mirror
// swaps the neighbours' two colours and the edge circle swaps the centre's with
// its neighbour's, so composing those swaps along the fold names the tile's colour.
// Each entry packs (source pixel + 1) * 4096 + (palette index + 1), 0 for none.
float HY_CX = 0.0;
float HY_CY = 0.0;
float HY_INV_R = 1.0;
float HY_AX = 0.0;          // the flow: the Mobius translation's point a
float HY_AY = 0.0;
float HY_TURN = 0.0;        // the warp: the whole scene's turn,
float HY_SWIRL = 0.0;       // the extra twist at its centre,
float HY_RIPPLE = 0.0;      // the ripple's depth and phase
float HY_WAVE = 0.0;
bool HY_SHOW = true;        // whether the plane is drawn, or only the warp is wanted
int HY_H = 0;
float HY_REVEAL = 0.0;      // the hyperbolic radius opened so far
float HY_GAIN = 1.0;
float HY_EDGE_C = 1.5535;   // the central octagon's edge circle: centre on the x axis, radius
float HY_EDGE_R = 1.1889;
float HY_MID = 0.3646;      // the edge's midpoint, the tile's inradius in the disk
float HY_NX = 0.0;          // the pi/8 mirror's normal
float HY_NY = 1.0;
int HY_W = 0;
int HY_TILE = 0;            // palette: 3 colours x 16 levels
int HY_EDGE = 0;            // and 8 levels of edge light
int HY_SPIN = 0;
array<int> HY_ROWS = [];

function hyper_row(int y) -> array<int> {
	array<int> out = [];
	int ww = HY_W;
	float ir = HY_INV_R;
	float cx = HY_CX;
	float cy = HY_CY;
	float v0 = (y + 0.5 - cy) * ir;
	float ax = HY_AX;
	float ay = HY_AY;
	float turn = HY_TURN;
	float swirl = HY_SWIRL;
	float ripple = HY_RIPPLE;
	float wave = HY_WAVE;
	bool show = HY_SHOW;
	int hh = HY_H;
	float ec = HY_EDGE_C;
	float er = HY_EDGE_R;
	float er2 = er * er;
	float nx = HY_NX;
	float ny = HY_NY;
	float reveal = HY_REVEAL;
	float gain = HY_GAIN;
	float mid = HY_MID;
	for (int x = 0; x < ww; ++x) {
		float u0 = (x + 0.5 - cx) * ir;
		float pr = math::sqrt(u0 * u0 + v0 * v0);
		float pa = math::atan2(v0, u0) - turn - swirl / (1.0 + 3.0 * pr * pr);
		pr *= 1.0 + ripple * math::sin(pr * 7.0 - wave);
		float u = pr * math::cos(pa);
		float vv = pr * math::sin(pa);
		int gx = math::ifloor(cx + u / ir);
		int gy = math::ifloor(cy + vv / ir);
		int src = gx >= 0 && gx < ww && gy >= 0 && gy < hh ? gy * ww + gx : -1;
		int packed = (src + 1) * 4096;
		float r2 = u * u + vv * vv;
		// outside the disk the plane continues as its own mirror image through the
		// circle at infinity, dimmer, its tiles growing toward the screen's edges
		float side = 1.0;
		if (r2 > 1.0) {
			u /= r2;
			vv /= r2;
			r2 = 1.0 / r2;
			side = 0.5;
		}
		if (!show || r2 >= 0.994) {
			out.push(packed);
			continue;
		}
		float r = math::sqrt(r2);
		float dh = math::log2((1.0 + r) / (1.0 - r)) * 0.6931472;
		if (dh > reveal) {
			out.push(packed);
			continue;
		}
		float zx = u;
		float zy = vv;
		float qx = zx - ax;
		float qy = zy - ay;
		float dx = 1.0 - ax * zx - ay * zy;
		float dy = ay * zx - ax * zy;
		float dd = dx * dx + dy * dy;
		float fx = (qx * dx + qy * dy) / dd;
		float fy = (qy * dx - qx * dy) / dd;
		int p0 = 0;
		int p1 = 1;
		int p2 = 2;
		int facet = 0;
		float e2 = 9.0;
		for (int it = 0; it < 40; ++it) {
			bool moved = false;
			if (fy < 0.0) {
				fy = -fy;
				facet = 1 - facet;
			}
			float d = fx * nx + fy * ny;
			if (d > 0.0) {
				fx -= 2.0 * d * nx;
				fy -= 2.0 * d * ny;
				int sw = p1; p1 = p2; p2 = sw;
				facet = 1 - facet;
				moved = true;
			}
			float ex = fx - ec;
			e2 = ex * ex + fy * fy;
			if (e2 < er2) {
				float k = er2 / e2;
				fx = ec + ex * k;
				fy *= k;
				int sw2 = p0; p0 = p1; p1 = sw2;
				moved = true;
			}
			if (!moved) { break; }
		}
		float rim = (1.0 - r2) * 2.5;
		float fade = (0.3 + 0.7 * (rim > 1.0 ? 1.0 : rim)) * gain * side;
		float edge = math::sqrt(e2) - er;
		bool front = side == 1.0 && reveal - dh < 0.22;
		if (front || edge < 0.02) {
			int le = itrunc((front ? 7.9 : 6.0 - edge * 150.0) * fade);
			out.push(packed + HY_EDGE + 1 + (le < 0 ? 0 : (le > 7 ? 7 : le)));
			continue;
		}
		// shaded by the distance to the nearest edge: a bevel lit on one side of each
		// edge's midpoint mirror and shadowed on the other, a raised middle, an inlaid ring
		float depth = edge / mid;
		float ring = depth - 0.5;
		ring = ring < 0.0 ? -ring : ring;
		float lit = depth < 0.2 ? (facet == 1 ? 15.0 : 4.0) : 7.0 + depth * 6.0;
		if (ring < 0.05) { lit = 3.0; }
		int l = itrunc(lit * fade);
		out.push(packed + HY_TILE + 1 + ((p0 + HY_SPIN) % 3) * 16 + (l < 0 ? 0 : (l > 15 ? 15 : l)));
	}
	return out;
}

class Finale : Fire {
	array<int> letter_at = [];   // the letters' pixels
	array<int> letter_of = [];   // and which letter each belongs to
	array<int> rim_at = [];      // the pixels around the letters, drawn dark
	array<int> fuel = [];        // per column: the heat the letters feed this frame
	int word_x = 0;
	int word_w = 0;
	int word_top = 0;
	int word_rows = 0;

	array<float> cube = [];      // the 8-cube's vertices, 8 coordinates each, rotated and unfolded
	array<float> sx = [];        // their screen positions and depths
	array<float> sy = [];
	array<float> sz = [];
	array<float> glow = [];      // the hypercube's light, per pixel
	array<int> glow_hue = [];    // and the axis of its brightest edge there
	array<int> shake_row = [];
	int hyper = 0;               // palette: 8 axes x 12 levels
	int white = 0;               // 8 levels of flash, dark to white
	float beat = 0.48387097;     // the soundtrack's 124 bpm

	array<float> pfx = [];       // the shatter: one spark per letter pixel, from a vertex
	array<float> pfy = [];
	array<int> pfaxis = [];

	// the snaps: [start, length, sign, plane a, plane b, second plane a, b (or -1)]
	array<float> snap_at = [];
	array<float> snap_len = [];
	array<float> snap_sign = [];
	array<int> snap_pa = [];
	array<int> snap_pb = [];
	array<int> snap_qa = [];
	array<int> snap_qb = [];
	float landed = -9.0;         // when the last snap clicked home

	var deck = null;             // the eight slides
	float cube_cx = 0.0;         // where the cube and its disk sit, and their size
	float cube_cy = 0.0;
	float cube_scale = 1.0;
	float disk_r = 1.0;
	float view_mix = 0.0;        // 0 in the slides' window, 1 filling the screen

	// the timeline in bars of the 124 bpm soundtrack, set in build
	float snaps_t = 0.0;         // the first quarter-turn: the second slide's square
	float dance_t = 0.0;         // all eight shown: the snaps double up
	float shatter_t = 0.0;       // the cube bursts
	float slam_t = 0.0;          // the word lands and the fire erupts
	float rainbow_t = 0.0;
	float wall_out_t = 0.0;      // the wall starts dying
	float embers_t = 0.0;        // the letters stop feeding the flames
	float neon_t = 0.0;          // ... and have cooled to neon
	float credits_t = 0.0;
	float encore_t = 0.0;        // the word reignites for one last slam
	float encore_out_t = 0.0;    // ... and burns down again
	float end_t = 0.0;           // the frame has dithered to black
	float credit_rows_per_s = 5.0;

	// [kind, left column, right column]: a "role" row splits at the gutter
	var credits = [
		["title", "THE JAISCRIPT DEMOREEL", ""],
		["", "", ""],
		["body", "eleven scenes . six transitions", ""],
		["body", "every pixel computed by the jaiscript vm", ""],
		["", "", ""],
		["role", "ONE", "scripting"],
		["role", "TWO", "serialization"],
		["role", "THREE", "reflection"],
		["role", "FOUR", "hot reload"],
		["role", "FIVE", "parallel"],
		["role", "SIX", "signals"],
		["role", "SEVEN", "debugging"],
		["role", "EIGHT", "embedding"],
		["", "", ""],
		["role", "DONUT", "a product shot"],
		["role", "JULIA", "a dive into the boundary"],
		["role", "LAVALAMP", "a lava lamp, revealed"],
		["role", "PIPEDREAM", "chrome pipes on a synthwave set"],
		["role", "BOIDS", "a murmuration and a falcon"],
		["role", "GLOBHUNTER", "a torchlit raycaster hunt"],
		["role", "CANYONRUN", "a golden-hour voxelspace canyon"],
		["role", "CHROME", "chrome, glass and a polished floor"],
		["role", "OSSUARY", "a skull spider, a web, a moth"],
		["role", "MAELSTROM", "navier-stokes ink"],
		["role", "FINALE", "jaiscript in eight dimensions"],
		["", "", ""],
		["body", "sextant pixels, six to a cell", ""],
		["body", "deterministic to the last byte", ""],
		["", "", ""],
		["", "", ""],
		["title", "thanks for watching", ""],
		["link", "jaiscript.com", ""]
	];

	void build() override {
		name = "FINALE";
		handle = "eight dimensions . jaiscript forever";
		replay = 30.0;
		float bar = beat * 4.0;
		snaps_t = bar * 7.0;
		dance_t = bar * 35.0;
		shatter_t = bar * 37.0;
		slam_t = shatter_t + beat * 3.0;
		rainbow_t = slam_t + bar;
		wall_out_t = slam_t + 6.75;
		embers_t = slam_t + 7.75;
		neon_t = slam_t + 9.75;
		credits_t = slam_t + 8.75;
		encore_t = bar * 49.0;
		encore_out_t = encore_t + bar * 1.5;
		end_t = bar * length_bars();
		build_fire();
		fuel = gfx.make_grid(w);
		stamp_word("JAISCRIPT");
		hyper = pal.size();
		for (int d = 0; d < 8; ++d) {
			var c = gfx.rainbow_rgb(d * 0.7853982 + 0.4);
			float cr = c[0]; float cg = c[1]; float cb = c[2];
			for (int l = 0; l < 12; ++l) {
				float f = (l + 1) / 12.0;
				float wh = f > 0.7 ? (f - 0.7) * 2.4 : 0.0;
				float k = f < 0.7 ? f / 0.7 : 1.0;
				pal.add(cr * k + (255.0 - cr * k) * wh, cg * k + (255.0 - cg * k) * wh, cb * k + (255.0 - cb * k) * wh);
			}
		}
		white = pal.ramp(8, 30.0, 24.0, 60.0, 255.0, 255.0, 255.0);
		glow = gfx.make_fgrid(w * h);
		glow_hue = gfx.make_grid(w * h);
		shake_row = gfx.make_grid(w);
		cube = gfx.make_fgrid(256 * 8);
		sx = gfx.make_fgrid(256);
		sy = gfx.make_fgrid(256);
		sz = gfx.make_fgrid(256);
		for (int k2 = 0; k2 < letter_at.size(); ++k2) {
			pfx.push(0.0);
			pfy.push(0.0);
			pfaxis.push(rng.next(8));
		}
		HY_TILE = pal.size();
		var tile_ink = [[70.0, 90.0, 235.0], [225.0, 60.0, 120.0], [245.0, 185.0, 60.0]];
		for (int c = 0; c < 3; ++c) {
			var ink = tile_ink[c];
			float tr = ink[0]; float tg = ink[1]; float tb = ink[2];
			pal.ramp(16, tr * 0.04, tg * 0.04, tb * 0.05 + 4.0, tr * 0.62, tg * 0.62, tb * 0.62);
		}
		HY_EDGE = pal.ramp(8, 30.0, 30.0, 52.0, 225.0, 240.0, 255.0);
		// the {8,3} geometry: the octagon's inradius from cosh(rho) = cos(pi/3) / sin(pi/8)
		float ch = 0.5 / math::sin(0.3926991);
		HY_MID = math::sqrt((ch - 1.0) / (ch + 1.0));
		HY_EDGE_C = (HY_MID + 1.0 / HY_MID) * 0.5;
		HY_EDGE_R = (1.0 / HY_MID - HY_MID) * 0.5;
		HY_NX = 0.0 - math::sin(0.3926991);
		HY_NY = math::cos(0.3926991);
		deck = Deck();
		deck.hyper = hyper;
		deck.build(pal);
		plan_snaps();
	}

	// how many axes the slides have opened by time t
	int dims_at(float t) {
		int d = deck.section(t);
		return d > 8 ? 8 : d;
	}

	// a quarter-turn every other beat once there is a square, through planes of the
	// axes already open; with all eight shown two disjoint planes snap every half beat
	void plan_snaps() {
		int k = 0;
		float t = snaps_t;
		while (t < shatter_t - 0.1) {
			bool dance = t >= dance_t - 0.01;
			int dims = dims_at(t);
			int a = k % dims;
			int b = (a + 1 + (k / dims) % (dims - 1)) % dims;
			int qa = -1;
			int qb = -1;
			if (dance) {
				b = (a + 1 + 2 * ((k / 8) % 3)) % 8;
				qa = (a + 4) % 8;
				qb = (b + 4) % 8;
			}
			snap_at.push(t);
			snap_len.push(dance ? beat * 0.42 : beat * 0.62);
			snap_sign.push((k & 1) == 0 ? 1.0 : -1.0);
			snap_pa.push(a);
			snap_pb.push(b);
			snap_qa.push(qa);
			snap_qb.push(qb);
			t += dance ? beat * 0.5 : beat * 2.0;
			++k;
		}
	}

	// the 5x7 font stretched to most of the width (columns at a fractional scale,
	// rows at the nearest whole one)
	void stamp_word(string text) {
		float fx = w * 0.86 / (text.size() * 6.0);
		fx = fx < 1.0 ? 1.0 : (fx > 5.0 ? 5.0 : fx);
		int gw = itrunc(fx * 5.0 + 0.5);
		int pitch = itrunc(fx * 6.0 + 0.5);
		int s = itrunc(fx + 0.5);
		word_w = text.size() * pitch - (pitch - gw);
		word_x = (w - word_w) / 2;
		word_rows = 7 * s;
		word_top = h / 2 - word_rows - 2;
		array<int> mask = gfx.make_grid(w * h);
		for (int li = 0; li < text.size(); ++li) {
			var bits = FONT5[text.substr(li, 1)];
			for (int ry = 0; ry < word_rows; ++ry) {
				int b = bits[ry / s];
				for (int rx = 0; rx < gw; ++rx) {
					if (((b >> (4 - rx * 5 / gw)) & 1) == 1) {
						int i = (word_top + ry) * w + word_x + li * pitch + rx;
						mask[i] = 1;
						letter_at.push(i);
						letter_of.push(li);
					}
				}
			}
		}
		for (int i = w; i < w * (h - 1); ++i) {
			if (mask[i] == 0 && (mask[i - 1] == 1 || mask[i + 1] == 1 || mask[i - w] == 1 || mask[i + w] == 1)) { rim_at.push(i); }
		}
	}

	// how hard the current beat hits: the kick in the finale's own score
	float pulse(float t) {
		if (cue != null) { return cue.kick_env(t); }
		float ph = t / beat;
		float f = ph - math::floor(ph);
		return (1.0 - f) * (1.0 - f);
	}

	// the score's marks: an accent on each slide's downbeat, impacts on the dance, the shatter, the slam
	// and the encore
	array<float> hit_times() override {
		array<float> result = [];
		for (int k = 0; k < 8; ++k) { result.push(MUSIC_BAR * (3 + 4 * k)); }
		result.push(MUSIC_BAR * 35.0);
		result.push(MUSIC_BAR * 37.0);
		result.push(MUSIC_BAR * 37.0 + MUSIC_BEAT * 3.0);
		result.push(MUSIC_BAR * 49.0);
		result.push(MUSIC_BAR * 50.0);
		return result;
	}

	array<int> hit_levels() override { return [1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 2, 1]; }

	int length_bars() override { return 52; }

	// the latest of the two slams at or before t
	float slam_of(float t) { return t > encore_t ? encore_t : slam_t; }

	void update(float t, float dt) override {
		tt = t;
		++fr;
		rainbow_on = t > rainbow_t ? 1 : 0;
		if (rainbow_on == 1) { hue_t = math::floor(t / beat) * 0.25; }
		int amp = 0;
		if (t > slam_t) {
			float erupt = (t - slam_t) * 3.0;
			erupt = erupt > 1.0 ? 1.0 : erupt;
			float pump = t < wall_out_t ? pulse(t) * 16.0 : 0.0;
			amp = itrunc((40.0 + pump) * erupt);
			if (t > wall_out_t) { amp = itrunc(42.0 - (t - wall_out_t) * 19.0); }
			if (t > encore_t) {
				float again = (t - encore_t) * 4.0;
				again = again > 1.0 ? 1.0 : again;
				amp = itrunc((44.0 + pulse(t) * 18.0) * again);
				if (t > encore_out_t) { amp = itrunc(44.0 - (t - encore_out_t) * 16.0); }
			}
			amp = amp < 0 ? 0 : amp;
		}
		seed_rows(amp, amp / 2 + 2);
		convect();
		if (t > slam_t) { burn_letters(t); }
		move_sparks(dt, amp > 12);
		place(t);
		turn_cube(t);
		set_warp(t);
		if (t > shatter_t && t < slam_t) { fly_sparks(t); }
	}

	// where the cube and its disk sit: a point at the centre before the slides, the
	// window above the demonstrations during them, the whole screen from the dance on
	void place(float t) {
		int cols = HB_COLS > 0 ? HB_COLS : w;
		int rows = HB_ROWS > 0 ? HB_ROWS : h / 2;
		int split = cols * 56 / 100;
		float vx = col_px((split + 2 + cols - 2) / 2);
		float vy = row_px((4 + rows / 2) / 2) + 1.0;
		float vh = row_px(rows / 2) - row_px(4);
		float into = (t - (deck.first - 0.7)) / 0.7;
		into = into < 0.0 ? 0.0 : (into > 1.0 ? 1.0 : into);
		into = into * into * (3.0 - 2.0 * into);
		float wide = (t - (dance_t - 0.35)) / 0.9;
		wide = wide < 0.0 ? 0.0 : (wide > 1.0 ? 1.0 : wide);
		wide = wide * wide * (3.0 - 2.0 * wide);
		float mx = w * 0.5 + (vx - w * 0.5) * into;
		float my = h * 0.5 + (vy - h * 0.5) * into;
		cube_cx = mx + (w * 0.5 - mx) * wide;
		cube_cy = my + (h * 0.5 - my) * wide;
		float small = vh * 0.15;
		float slide_scale = h * 0.07 + (small - h * 0.07) * into;
		cube_scale = slide_scale + (h * 0.12 - slide_scale) * wide;
		float slide_r = h * 0.3 + (vh * 0.55 - h * 0.3) * into;
		disk_r = slide_r + (h * 0.5 - 0.5 - slide_r) * wide;
		view_mix = wide;
	}

	// from the dance on the whole scene turns, faster through the dance and fastest
	// through the shatter, easing to a drift behind the fire; it swirls hardest at
	// the centre and ripples outward, both throbbing on the beat
	void set_warp(float t) {
		float t1 = t < shatter_t ? t : shatter_t;
		float into = t1 - dance_t;
		float spin = into > 0.0 ? 0.22 * into + 0.35 * into * into : 0.0;
		if (t > shatter_t) { spin += 2.26 * ((t < slam_t ? t : slam_t) - shatter_t); }
		if (t > slam_t) { spin += 0.5 * (t - slam_t); }
		float swirl = 0.0;
		float ripple = t > deck.first ? 0.006 : 0.0;
		if (t > dance_t) {
			swirl = 0.5 + 0.6 * pulse(t) + (t - dance_t) * 0.4;
			ripple = 0.015 + 0.025 * pulse(t);
		}
		if (t > shatter_t) {
			float u = (t - shatter_t) / (slam_t - shatter_t);
			swirl = 1.7 + 1.5 * u;
			ripple = 0.05 + 0.07 * u;
		}
		if (t > slam_t) {
			swirl = 0.7 * math::sin((t - slam_t) * 0.8);
			ripple = 0.02;
		}
		HY_TURN = spin;
		HY_SWIRL = swirl;
		HY_RIPPLE = ripple;
		HY_WAVE = t * 5.0;
		HY_CX = cube_cx;
		HY_CY = cube_cy;
		HY_INV_R = 1.0 / disk_r;
		HY_W = w;
		HY_H = h;
		set_plane(t);
	}

	// where the warp takes the screen point (px, py): the same map hyper_row applies
	array<float> warp_source(float px, float py) {
		float ir = HY_INV_R;
		float u0 = (px - HY_CX) * ir;
		float v0 = (py - HY_CY) * ir;
		float pr = math::sqrt(u0 * u0 + v0 * v0);
		float pa = math::atan2(v0, u0) - HY_TURN - HY_SWIRL / (1.0 + 3.0 * pr * pr);
		pr *= 1.0 + HY_RIPPLE * math::sin(pr * 7.0 - HY_WAVE);
		return [HY_CX + pr * math::cos(pa) / ir, HY_CY + pr * math::sin(pa) / ir];
	}

	// the 8-cube: each axis opens in turn (a line, a square, a cube, ...) and the
	// whole turns by the snaps: every finished one is an exact quarter-turn (a signed
	// swap of two coordinates), the one in flight eases past its mark and settles back
	void turn_cube(float t) {
		var& c = cube;
		array<float> open = [];
		for (int d = 0; d < 8; ++d) {
			float f = (t - deck.start(d + 1)) / 0.45;
			open.push(f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f * f * (3.0 - 2.0 * f)));
		}
		for (int v = 0; v < 256; ++v) {
			int base = v * 8;
			for (int d2 = 0; d2 < 8; ++d2) { c[base + d2] = ((v >> d2) & 1) == 1 ? open[d2] : -open[d2]; }
		}
		for (int k = 0; k < snap_at.size(); ++k) {
			float f = (t - snap_at[k]) / snap_len[k];
			if (f < 0.0) { break; }
			if (f >= 1.0) {
				landed = snap_at[k] + snap_len[k];
				quarter_turn(snap_pa[k], snap_pb[k], snap_sign[k]);
				if (snap_qa[k] >= 0) { quarter_turn(snap_qa[k], snap_qb[k], snap_sign[k]); }
				continue;
			}
			float g = f - 1.0;
			float eased = 1.0 + 2.70158 * g * g * g + 1.70158 * g * g;
			float ang = snap_sign[k] * 1.5707963 * eased;
			turn_plane(snap_pa[k], snap_pb[k], math::cos(ang), math::sin(ang));
			if (snap_qa[k] >= 0) { turn_plane(snap_qa[k], snap_qb[k], math::cos(ang), math::sin(ang)); }
		}
		// down to the screen: the 16-gon Petrie projection, its axes dealt out in
		// bit-reversed order so the first two open as a square and the third as a
		// cube; it breathes on the beat (the warp turns it with the plane)
		float scale = cube_scale * (1.0 + (t > deck.first && t < shatter_t ? pulse(t) * 0.12 : 0.0));
		float cx = cube_cx;
		float cy = cube_cy;
		array<float> dir_x = [];
		array<float> dir_y = [];
		array<float> dir_z = [];
		array<int> deal = [0, 4, 2, 6, 1, 5, 3, 7];
		for (int d3 = 0; d3 < 8; ++d3) {
			float ang2 = deal[d3] * 0.3926991;
			dir_x.push(math::cos(ang2));
			dir_y.push(math::sin(ang2));
			dir_z.push(0.35 * math::cos(ang2 * 3.0 + 0.6));
		}
		for (int v2 = 0; v2 < 256; ++v2) {
			int b2 = v2 * 8;
			float x = 0.0;
			float y = 0.0;
			float z = 0.0;
			for (int d4 = 0; d4 < 8; ++d4) {
				float q = c[b2 + d4];
				x += q * dir_x[d4];
				y += q * dir_y[d4];
				z += q * dir_z[d4];
			}
			float s = scale * 4.0 / (6.0 - z);
			sx[v2] = cx + x * s;
			sy[v2] = cy + y * s;
			sz[v2] = z;
		}
	}

	void quarter_turn(int a, int b, float sign) {
		var& c = cube;
		for (int v = 0; v < 256; ++v) {
			int ia = v * 8 + a;
			int ib = v * 8 + b;
			float xa = c[ia];
			c[ia] = 0.0 - sign * c[ib];
			c[ib] = sign * xa;
		}
	}

	void turn_plane(int a, int b, float ca, float sa) {
		var& c = cube;
		for (int v = 0; v < 256; ++v) {
			int ia = v * 8 + a;
			int ib = v * 8 + b;
			float xa = c[ia];
			float xb = c[ib];
			c[ia] = xa * ca - xb * sa;
			c[ib] = xa * sa + xb * ca;
		}
	}

	// the shatter: each letter pixel's spark leaves a vertex of the cube, curls
	// around the centre and lands where the warp shows it on its pixel on the slam
	void fly_sparks(float t) {
		float u = (t - shatter_t) / (slam_t - shatter_t);
		u = u > 1.0 ? 1.0 : u;
		float e = u * u * (3.0 - 2.0 * u);
		float curl = math::sin(u * 3.1415927) * 1.6;
		float cc = math::cos(curl);
		float ss = math::sin(curl);
		float cx = w * 0.5;
		float cy = h * 0.5;
		for (int k = 0; k < letter_at.size(); ++k) {
			int v = (k * 37) & 255;
			int i = letter_at[k];
			var at = warp_source(i % w + 0.5, i / w + 0.5);
			float tx = at[0];
			float ty = at[1];
			float px = sx[v] + (tx - sx[v]) * e;
			float py = sy[v] + (ty - sy[v]) * e;
			float ox = px - cx;
			float oy = py - cy;
			pfx[k] = cx + ox * cc - oy * ss;
			pfy[k] = cy + ox * ss + oy * cc;
		}
	}

	// each column's fuel swells and ebbs where two waves cross and pumps on the
	// beat, and the air above the word cools fast, so the flames rise as tongues;
	// once the letters have cooled, the last embers anywhere go out the same way
	void burn_letters(float t) {
		float strength = t < embers_t ? 1.0 : (t > neon_t ? 0.0 : (neon_t - t) / (neon_t - embers_t));
		float pump = t < wall_out_t ? 1.0 + pulse(t) * 0.35 : 1.0;
		bool encore = t > encore_t;
		if (encore) {
			strength = t < encore_out_t ? 1.0 : 1.0 - (t - encore_out_t) / 1.2;
			strength = strength < 0.0 ? 0.0 : strength;
			pump = 1.0 + pulse(t) * 0.45;
		}
		for (int x = 0; x < w; ++x) {
			float wave = math::sin(x * 0.61 + t * 6.1) + math::sin(x * 0.23 - t * 3.7 + 1.7);
			fuel[x] = itrunc((46.0 + wave * 15.0) * strength * pump);
		}
		var& ht = heat;
		var& rn = cool;
		var& at = letter_at;
		int n = at.size();
		for (int k = 0; k < n; ++k) {
			int i = at[k];
			int v = fuel[i % w] + (rn[(i * 7 + fr) & 1023] & 1) * 8;
			if (ht[i] < v) { ht[i] = v; }
		}
		int quenched = t > neon_t && (!encore || strength <= 0.0) ? w * h : word_top * w;
		for (int i2 = 0; i2 < quenched; ++i2) {
			int v2 = ht[i2] - 2;
			ht[i2] = v2 > 0 ? v2 : 0;
		}
	}

	// the cube's edges into the glow buffer: each edge joins two vertices one
	// axis apart, its hue that axis', nearer edges brighter; crossings add a
	// little, so the dense middle keeps its lattice instead of burning white
	void draw_cube(float gain) {
		var& g = glow;
		var& gh = glow_hue;
		int n = w * h;
		for (int i = 0; i < n; ++i) { g[i] = 0.0; }
		float cx = cube_cx;
		float cy = cube_cy;
		float outer = 1.0 / (cube_scale * 3.0);
		for (int v = 0; v < 256; ++v) {
			for (int d = 0; d < 8; ++d) {
				if (((v >> d) & 1) == 1) { continue; }
				int u = v | (1 << d);
				float x0 = sx[v];
				float y0 = sy[v];
				float dx = sx[u] - x0;
				float dy = sy[u] - y0;
				float adx = dx < 0.0 ? -dx : dx;
				float ady = dy < 0.0 ? -dy : dy;
				int steps = itrunc((adx > ady ? adx : ady) + 1.0);
				// the rose's outer rings carry its shape; the crowded middle stays dim
				float mx = (x0 + dx * 0.5 - cx) * outer;
				float my = (y0 + dy * 0.5 - cy) * outer;
				float reach = mx * mx + my * my;
				float lum = gain * (0.55 + (sz[v] + sz[u]) * 0.18) * (0.22 + 0.78 * (reach > 1.0 ? 1.0 : reach));
				// in the dance two of the eight axes light on each beat and the rest dim, so the
				// lattice reads as structure strobing with the music instead of a tangle
				if (tt > dance_t && tt < shatter_t) {
					int lit = (itrunc((tt - dance_t) / beat) * 2) % 8;
					lum *= d == lit || d == lit + 1 ? 1.45 : 0.18;
				}
				float ix = dx / steps;
				float iy = dy / steps;
				for (int s = 0; s <= steps; ++s) {
					int px = itrunc(x0 + ix * s);
					int py = itrunc(y0 + iy * s);
					if (px < 0 || px >= w || py < 0 || py >= h) { continue; }
					int i2 = py * w + px;
					float was = g[i2];
					if (lum > was) {
						gh[i2] = d;
						g[i2] = lum + was * 0.25;
					} else {
						g[i2] = was + lum * 0.25;
					}
				}
			}
		}
	}

	void render(var& grid) override {
		int n = w * h;
		int b = bank();
		var& ht = heat;
		var& g = glow;
		var& gh = glow_hue;
		float gain = tt < shatter_t ? 0.8 : (tt < slam_t ? 0.8 * (1.0 - (tt - shatter_t) / (slam_t - shatter_t)) : 0.3);
		if (tt > wall_out_t) { gain = 0.3 * (1.0 - (tt - wall_out_t) * 0.5); }
		float click = tt - landed < 0.16 ? (0.16 - (tt - landed)) * 3.0 : 0.0;
		gain = gain + (tt < shatter_t ? click : 0.0);
		bool cube_on = gain > 0.01;
		bool flight = tt > shatter_t && tt < slam_t;
		if (cube_on) {
			draw_cube(gain);
		} else if (flight) {
			for (int c = 0; c < n; ++c) { g[c] = 0.0; }
		}
		if (flight) { draw_flight(); }
		bool tiles_on = tt > deck.first - 0.7 && (tt < slam_t || (tt > slam_t + 0.5 && tt < wall_out_t + 1.5) || (tt > encore_t + 0.1 && tt < end_t - MUSIC_BAR * 0.5));
		bool warped = tt > deck.first - 0.7 && (cube_on || flight || tiles_on);
		HY_SHOW = tiles_on;
		var rows = [];
		if (warped) { rows = open_plane(); }
		float over = tiles_on && tt < slam_t ? 0.1 : 0.04;
		int i = 0;
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				int v = ht[i];
				int src = i;
				int ink = -1;
				if (warped) {
					int packed = rows[y][x];
					src = (packed >> 12) - 1;
					ink = (packed & 4095) - 1;
				}
				float light = src >= 0 && (cube_on || flight) ? g[src] : 0.0;
				if (v <= 5 && light > over) {
					int lv = light * 9.0;
					grid[i] = hyper + gh[src] * 12 + (lv > 11 ? 11 : lv);
				} else if (v <= 5 && ink >= 0) {
					grid[i] = ink;
				} else {
					grid[i] = (v > 72 ? 72 : v) + b;
				}
				++i;
			}
		}
		if (tt > slam_t) {
			var& rim = rim_at;
			for (int k = 0; k < rim.size(); ++k) {
				int i2 = rim[k];
				grid[i2] = b + ht[i2] / 3;
			}
			draw_letters(grid, b);
		}
		draw_sparks(grid, b);
		if (tt < shatter_t) {
			deck.draw(grid, w, h, tt, 0);
			draw_dimension(grid);
		}
		slam(grid);
		fade_out(grid);
	}

	// over the last bar the frame dithers to black, the word last of all
	void fade_out(var& grid) {
		float k = (tt - (end_t - MUSIC_BAR)) / MUSIC_BAR;
		if (k <= 0.0) { return; }
		int keep = itrunc((k > 1.0 ? 1.0 : k) * 1100.0);
		int b = bank();
		for (int i = 0; i < w * h; ++i) {
			int x = (((i >> 16) ^ i) * 73244475) & 4294967295;
			x = (((x >> 16) ^ x) * 73244475) & 4294967295;
			int d = ((x >> 16) ^ x) & 1023;
			if (d < keep - (heat[i] > 40 ? 200 : 0)) { grid[i] = b; }
		}
	}

	// the warped scene, a row at a time: the plane's colour and the cube light's source
	var open_plane() {
		if (HY_ROWS.size() != h) {
			HY_ROWS = [];
			for (int r = 0; r < h; ++r) { HY_ROWS.push(r); }
		}
		return parallel_transform(HY_ROWS, hyper_row);
	}

	// the hyperbolic plane: it opens ring by ring as the dimensions do, flows under
	// a translation that wraps at four inradii (two tiles across, where the tiling
	// and its colouring both repeat), accelerates at eight dimensions and warps out
	// as the cube shatters; at eight dimensions its colours roll on every snap
	void set_plane(float t) {
		float period = 4.0 * math::log2((1.0 + HY_MID) / (1.0 - HY_MID)) * 0.6931472;
		float s = t * 0.3;
		if (t > dance_t) { s += (t - dance_t) * (t - dance_t) * 0.8; }
		if (t > shatter_t) { s += (t - shatter_t) * (t - shatter_t) * 6.0; }
		if (t > slam_t) { s = (t - slam_t) * 0.8; }
		s -= math::floor(s / period) * period;
		float e = math::pow(2.718281828, s);
		float along = (e - 1.0) / (e + 1.0);
		HY_AX = along;
		HY_AY = 0.0;
		int d = deck.section(t);
		float opening = d >= 1 && d <= 8 ? (t - deck.start(d)) / 0.6 : 1.0;
		opening = opening > 1.0 ? 1.0 : opening;
		HY_REVEAL = d == 0 ? 0.0 : (t < dance_t ? (d - 1 + opening) * 0.6 + 0.25 : 5.05 + (t - dance_t) * 6.0);
		float gain = t < dance_t ? 0.55 + 0.15 * pulse(t) : 0.85 + pulse(t) * 0.3;
		if (t > shatter_t) { gain = 1.0 - (t - shatter_t) / (slam_t - shatter_t); }
		int spun = 0;
		for (int k = 0; k < snap_at.size(); ++k) {
			if (snap_at[k] >= dance_t - 0.01 && snap_at[k] + snap_len[k] <= t) { ++spun; }
		}
		// behind the fire it returns faint, its colours rolling on every beat
		if (t > slam_t) {
			float rise = (t - slam_t - 0.5) * 1.0;
			float fall = (wall_out_t + 1.5 - t) / 1.5;
			gain = 0.45 * (rise < fall ? rise : fall) * (0.8 + pulse(t) * 0.4);
			gain = gain > 0.45 ? 0.45 : gain;
			spun = itrunc(t / beat);
		}
		if (t > encore_t) {
			float rise2 = (t - encore_t) * 2.0;
			float fall2 = (end_t - MUSIC_BAR * 0.5 - t) / MUSIC_BAR;
			gain = 0.55 * (rise2 < fall2 ? rise2 : fall2) * (0.8 + pulse(t) * 0.5);
			gain = gain > 0.55 ? 0.55 : (gain < 0.0 ? 0.0 : gain);
		}
		HY_GAIN = gain;
		HY_SPIN = spun % 3;
	}

	// the sparks in flight, each in the hue of an axis, into the cube's light so the
	// warp carries them with everything else
	void draw_flight() {
		var& g = glow;
		var& gh = glow_hue;
		for (int k = 0; k < letter_at.size(); ++k) {
			int x = math::ifloor(pfx[k]);
			int y = math::ifloor(pfy[k]);
			if (x < 0 || x >= w || y < 0 || y >= h) { continue; }
			int i = y * w + x;
			g[i] = 1.12 + (k & 1) * 0.2;
			gh[i] = pfaxis[k];
		}
	}

	// the dimension counter under the cube: 1D, 2D, ... 8D as each axis opens
	void draw_dimension(var& grid) {
		int dims = dims_at(tt);
		if (dims < 1 || view_mix > 0.5) { return; }
		string label = `${dims}D`;
		int col = cell_col(itrunc(cube_cx)) - 1;
		int row = cell_row(itrunc(cube_cy + cube_scale * 3.6)) + 1;
		row = row > HB_ROWS / 2 - 1 ? HB_ROWS / 2 - 1 : row;
		var c = gfx.rainbow_rgb((dims - 1) * 0.7853982 + 0.4);
		float cr = c[0]; float cg = c[1]; float cb = c[2];
		txt_put(col, row, label, rgb(itrunc(cr), itrunc(cg), itrunc(cb)));
	}

	// the slam: a white flash, two shockwave rings racing out from the word
	// (white leading edges, a rainbow wake) and a shake that settles over half a second
	void slam(var& grid) {
		float s = tt - slam_of(tt);
		if (s < 0.0 || s > 0.9) { return; }
		int n = w * h;
		if (s < 0.1) {
			int lv = 7 - itrunc(s * 40.0);
			for (int i = 0; i < n; ++i) { grid[i] = white + (lv < 5 ? 5 : lv); }
			return;
		}
		float cx = w * 0.5;
		float cy = word_top + word_rows * 0.5;
		for (int ring = 0; ring < 2; ++ring) {
			float r = (s - 0.1 - ring * 0.12) * 150.0;
			if (r < 1.0) { continue; }
			float width = 1.2 + r * 0.03;
			int ink = hyper + ((ring * 3 + fr) % 8) * 12;
			int y0 = itrunc(cy - r - width) - 1;
			int y1 = itrunc(cy + r + width) + 1;
			y0 = y0 < 0 ? 0 : y0;
			y1 = y1 > h - 1 ? h - 1 : y1;
			for (int y = y0; y <= y1; ++y) {
				float ey = y - cy;
				for (int x = 0; x < w; ++x) {
					float ex = x - cx;
					float d = math::sqrt(ex * ex + ey * ey) - r;
					if (d < width && d > -width) { grid[y * w + x] = d > -width * 0.35 ? white + 7 : ink + 10; }
				}
			}
		}
		if (s < 0.6) {
			float amp = (0.6 - s) * 6.0;
			var& row = shake_row;
			for (int y2 = 0; y2 < h; ++y2) {
				int off = itrunc(math::sin(y2 * 0.9 + fr * 2.3) * amp);
				if (off == 0) { continue; }
				int base = y2 * w;
				for (int x2 = 0; x2 < w; ++x2) { row[x2] = grid[base + x2]; }
				for (int x3 = 0; x3 < w; ++x3) {
					int from = x3 - off;
					from = from < 0 ? 0 : (from >= w ? w - 1 : from);
					grid[base + x3] = row[from];
				}
			}
		}
	}

	// the letters burn white-hot over their own flames, ringed by a dark rim;
	// once the fire is out each cools to its own hue of the rainbow, and a
	// glint sweeps across the word every few seconds
	void draw_letters(var& grid, int b) {
		var& at = letter_at;
		int n = at.size();
		if (tt < embers_t || (tt > encore_t && tt < encore_out_t + 0.6)) {
			for (int k = 0; k < n; ++k) {
				int i = at[k];
				grid[i] = b + 64 + ((i * 7 + fr) & 7);
			}
			return;
		}
		float cooled = tt > neon_t ? 1.0 : (tt - embers_t) / (neon_t - embers_t);
		int level = 64 - itrunc(cooled * 12.0);
		int roll = itrunc(tt * 2.0);
		int glint = tt > neon_t ? itrunc((tt - neon_t) * 60.0) % (word_w + 160) - 20 : -20;
		for (int k = 0; k < n; ++k) {
			int i = at[k];
			int d = i % w - word_x + i / w - word_top - glint;
			int lv = d >= 0 && d < 6 ? 72 - d : level + ((i * 7 + fr) & 3);
			grid[i] = levels + ((letter_of[k] + roll) % phases) * levels + lv;
		}
	}

	// the credits roll up from the bottom row, under the word, and stop with the
	// last lines resting midway; rows near the edges of the band dim
	void text_rows(var& rows) override {
		if (tt < credits_t) { return; }
		int top = cell_row(word_top + word_rows) + 2;
		int bottom = HB_ROWS - 2;
		if (bottom - top < 2) { return; }
		int last = credits.size() - 1;
		int rest = bottom + last - (top + bottom) / 2;
		int scroll = itrunc((tt - credits_t) * credit_rows_per_s);
		scroll = scroll > rest ? rest : scroll;
		// from the encore only the farewell stays up
		int first = tt > encore_t ? last - 1 : 0;
		for (int k = first; k <= last; ++k) {
			int row = bottom + 1 + k - scroll;
			if (row < top || row > bottom) { continue; }
			var& line = credits[k];
			if (line[0] == "") { continue; }
			int edge = row - top < bottom - row ? row - top : bottom - row;
			rows[row] = credit_row(line, edge >= 2 ? 0 : 2 - edge);
		}
	}

	string credit_row(var& line, int dim) {
		string kind = line[0];
		string left = line[1];
		string right = line[2];
		string result = sgr_bg(0, 0, 0);
		int cw = HB_COLS;
		if (kind == "role") {
			int gutter = cw / 2;
			left = left.size() > gutter - 2 ? left.substr(0, gutter - 2) : left;
			right = right.size() > cw - gutter - 1 ? right.substr(0, cw - gutter - 1) : right;
			result += string::repeat(" ", gutter - 1 - left.size()) + credit_ink(255, 206, 110, dim) + left
				+ "  " + credit_ink(186, 180, 236, dim) + right + string::repeat(" ", cw - gutter - 1 - right.size());
		} else {
			left = left.size() > cw ? left.substr(0, cw) : left;
			int pad = (cw - left.size()) / 2;
			string ink = credit_ink(214, 208, 255, dim);
			if (kind == "title") { ink = credit_ink(255, 236, 190, dim); }
			if (kind == "link") { ink = credit_ink(120, 200, 255, dim); }
			result += string::repeat(" ", pad) + ink + left + string::repeat(" ", cw - pad - left.size());
		}
		return result + RESET;
	}

	string credit_ink(int r, int g, int b, int dim) {
		return sgr_fg(r * (3 - dim) / 3, g * (3 - dim) / 3, b * (3 - dim) / 3);
	}
}
