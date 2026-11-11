// JULIA — the whole Julia set, morphing in frame. The constant is steered through its
// fixed point's multiplier lambda = rho * e^(i theta), c = lambda / 2 - lambda^2 / 4:
// rho under 1 is a fat connected set, rho 1 is the main cardioid's boundary (San Marco,
// the Siegel disk), rho over 1 at theta = 2 pi p / q is the q-eared bulb (Douady's
// rabbit at 1/3), and past a bulb's tip the ears starve to a dendrite whose 2q arms
// meet at the origin. The path only ever crosses constants whose set is connected.
// Every pixel iterates z and its derivative on typed float locals: the smooth escape
// count picks the hue, the distance estimate draws the boundary as a white-hot rim a
// pixel wide however thin the filament, and the potential's gradient lights the bands
// as a relief. The inside is banded by where its orbit ends up round the fixed point.
// Every view is centred on the origin, so a frame computes half its rows and mirrors
// them (the set is its own half turn). The camera frames the set by its far fixed
// point beta, so the whole shape fills the frame; kicks punch the zoom, the chord
// turns the palette, and each big hit dives into a dendrite's junction and spins.

float JU_CA = 0.0;          // c
float JU_CB = 0.0;
float JU_FX = 0.0;          // the fixed point alpha = lambda / 2
float JU_FY = 0.0;
float JU_TRAP = 0.0;        // squared radius round alpha inside which an orbit is home; 0 when alpha repels
float JU_DRIFT = 0.0;       // what an iteration adds to the inside's potential, so the trap's early exit leaves no seam
int JU_MAXIT = 40;
int JU_W = 0;
int JU_H = 0;
float JU_UX = 0.0;          // the view, centred on the origin: a pixel's step right and down
float JU_UY = 0.0;
float JU_VX = 0.0;
float JU_VY = 0.0;
float JU_PIXELS = 1.0;      // pixels per plane unit
float JU_UNRESOLVED = 0.0;  // squared stretch past which a pixel that never escaped is boundary, not inside
float JU_LX = 0.0;          // the light's direction in the plane
float JU_LY = 0.0;
float JU_PHASE = 0.0;       // the palette's turn, outside and inside
float JU_INPHASE = 0.0;
float JU_LIFT = 0.0;        // light added on the beat
float JU_RIM = 1.0;         // the rim's width in pixels
int JU_INSIDE = 0;
array<int> JU_ROWS = [];

int JU_HUES = 64;
int JU_LEVELS = 12;
int JU_INHUES = 32;

int JU_RHO = 0;             // the path's tracks
int JU_THETA = 1;
int JU_DIVE = 2;
int JU_SPIN = 3;

// one row of the top half; each entry packs the pixel and the one half a turn away,
// whose orbit is the same and whose gradient points the other way
function julia_row(int y) -> array<int> {
	array<int> out = [];
	int w = JU_W;
	int mi = JU_MAXIT;
	float ca = JU_CA;
	float cb = JU_CB;
	float ux = JU_UX;
	float uy = JU_UY;
	float py = y + 0.5 - JU_H * 0.5;
	float left = 0.5 - w * 0.5;
	float rx = py * JU_VX + left * ux;
	float ry = py * JU_VY + left * uy;
	float lx = JU_LX;
	float ly = JU_LY;
	float fx = JU_FX;
	float fy = JU_FY;
	float trap = JU_TRAP;
	float drift = JU_DRIFT;
	float pixels = JU_PIXELS;
	float phase = JU_PHASE;
	float inphase = JU_INPHASE;
	float lift = JU_LIFT;
	float rim = JU_RIM;
	float rimfall = 0.5 / rim;
	int inside = JU_INSIDE;
	float unresolved = JU_UNRESOLVED;
	int hot = (math::itrunc(mi * 3.5 + phase) & 63) * 12 + 11;
	for (int x = 0; x < w; ++x) {
		float zx = rx + x * ux;
		float zy = ry + x * uy;
		float x2 = zx * zx;
		float y2 = zy * zy;
		float dx = 1.0;
		float dy = 0.0;
		float e2 = 1.0;
		int k = 0;
		float nd = 0.0;
		// four steps to a test: an escaped orbit's count and distance stay exact however far it runs on
		while (k < mi) {
			nd = 2.0 * (zx * dx - zy * dy);
			dy = 2.0 * (zx * dy + zy * dx);
			dx = nd;
			zy = 2.0 * zx * zy + cb;
			zx = x2 - y2 + ca;
			x2 = zx * zx;
			y2 = zy * zy;
			nd = 2.0 * (zx * dx - zy * dy);
			dy = 2.0 * (zx * dy + zy * dx);
			dx = nd;
			zy = 2.0 * zx * zy + cb;
			zx = x2 - y2 + ca;
			x2 = zx * zx;
			y2 = zy * zy;
			nd = 2.0 * (zx * dx - zy * dy);
			dy = 2.0 * (zx * dy + zy * dx);
			dx = nd;
			zy = 2.0 * zx * zy + cb;
			zx = x2 - y2 + ca;
			x2 = zx * zx;
			y2 = zy * zy;
			nd = 2.0 * (zx * dx - zy * dy);
			dy = 2.0 * (zx * dy + zy * dx);
			dx = nd;
			zy = 2.0 * zx * zy + cb;
			zx = x2 - y2 + ca;
			x2 = zx * zx;
			y2 = zy * zy;
			k += 4;
			if (x2 + y2 >= 64.0) { break; }
			float ex = zx - fx;
			float ey = zy - fy;
			e2 = ex * ex + ey * ey;
			if (e2 < trap) { break; }
		}
		float m2 = x2 + y2;
		float d2 = dx * dx + dy * dy;
		if (m2 < 64.0) {
			int c = hot;
			// an orbit still in after stretching its pixel past the whole set sits on the boundary
			if (d2 < unresolved) {
				float band = k * drift - math::log2(e2 + 0.000001) * 1.7 + inphase;
				c = inside + (math::itrunc(band + 1024.0) & 31);
			}
			out.push(c * 1025);
			continue;
		}
		float lg = math::log2(m2);
		float nu = k + 1.0 - math::log2(lg * 0.5);
		float unit = 1.0 / math::sqrt(m2 * d2);
		// the distance to the set, in pixels: |z| ln|z| / |z'|
		float de = m2 * unit * lg * 0.34657 * pixels;
		// the potential's gradient runs along z * conj(z')
		float nl = ((zx * dx + zy * dy) * lx + (zy * dx - zx * dy) * ly) * unit;
		float glow = 1.0 / (1.0 + de * 0.3);
		float lit = 0.4 + 0.3 * glow + lift + (de < rim ? 0.5 - de * rimfall : 0.0);
		float relief = nl * 0.2;
		int hue = (math::itrunc(nu * 3.5 + phase) & 63) * 12;
		int lv = math::itrunc((lit + relief) * 11.99);
		lv = lv < 0 ? 0 : (lv > 11 ? 11 : lv);
		int lm = math::itrunc((lit - relief) * 11.99);
		lm = lm < 0 ? 0 : (lm > 11 ? 11 : lm);
		out.push(hue + lv + (hue + lm) * 1024);
	}
	return out;
}

class Julia : Scene {
	float tt = 0.0;

	// the path, in beats: over key_from..key_to a track glides to key_value. rho and theta are
	// lambda's polar form, dive is octaves of zoom into the origin, spin radians of turn
	array<int> key_track = [];
	array<float> key_from = [];
	array<float> key_to = [];
	array<float> key_value = [];
	array<bool> key_lands = [];

	array<float> hit_times() override { return [MUSIC_BAR * 3.0, MUSIC_BAR * 5.0, MUSIC_BAR * 8.0, MUSIC_BAR * 9.0]; }
	array<int> hit_levels() override { return [2, 1, 2, 1]; }
	int length_bars() override { return 10; }

	void glide(int track, float from, float to, float value) {
		key_track.push(track);
		key_from.push(from);
		key_to.push(to);
		key_value.push(value);
		key_lands.push(false);
	}

	// a glide that leaves at full speed and settles: the move a hit throws
	void land(int track, float from, float to, float value) {
		glide(track, from, to, value);
		key_lands[key_lands.size() - 1] = true;
	}

	void build() override {
		name = "JULIA";
		handle = "one constant, every shape . by the type ladder";
		var night = [24.0, 12.0, 66.0];
		var stops = [[40.0, 30.0, 150.0], [120.0, 50.0, 210.0], [240.0, 50.0, 150.0], [255.0, 130.0, 50.0],
			[255.0, 220.0, 110.0], [80.0, 230.0, 220.0], [30.0, 120.0, 235.0], [40.0, 30.0, 150.0]];
		int ns = stops.size() - 1;
		for (int hq = 0; hq < JU_HUES; ++hq) {
			float f = hq * ns * 1.0 / JU_HUES;
			int k = math::itrunc(f);
			float u = f - k;
			var a = stops[k];
			var b = stops[k + 1];
			// a hue's ramp runs night, the hue in full, white hot
			for (int l = 0; l < JU_LEVELS; ++l) {
				float lit = l / (JU_LEVELS - 1.0);
				float sat = lit * 1.8 > 1.0 ? 1.0 : lit * 1.8;
				float wh = lit > 0.75 ? (lit - 0.75) * 3.0 : 0.0;
				array<float> rgb = [0.0, 0.0, 0.0];
				for (int ch = 0; ch < 3; ++ch) {
					float full = (a[ch] + (b[ch] - a[ch]) * u) * (0.5 + 0.5 * lit);
					float v = night[ch] + (full - night[ch]) * sat;
					rgb[ch] = v + (255.0 - v) * wh;
				}
				pal.add(rgb[0], rgb[1], rgb[2]);
			}
		}
		JU_INSIDE = pal.gradient(JU_INHUES + 1, [[6.0, 4.0, 22.0], [60.0, 18.0, 96.0], [10.0, 8.0, 36.0], [14.0, 62.0, 96.0], [6.0, 4.0, 22.0]]);

		// a fat set swells four ears by bar 1, trades them for the rabbit's three by bar 2,
		// and through the riser the rabbit starves to the six-armed dendrite past its bulb's tip
		glide(JU_THETA, 0.0, 2.5, 1.5708);
		glide(JU_RHO, 2.0, 4.0, 1.0625);
		glide(JU_RHO, 4.5, 5.5, 0.96);
		glide(JU_THETA, 5.0, 7.0, 2.0944);
		glide(JU_RHO, 6.5, 8.0, 1.107);
		glide(JU_RHO, 9.0, 12.0, 1.3283);
		glide(JU_THETA, 9.0, 12.0, 2.0866);
		glide(JU_DIVE, 9.0, 12.0, 0.7);
		// the bar 3 hit dives into the junction at its heart and turns; the pull back fattens the
		// arms and slides along the cardioid to San Marco, which the bar 5 accent twists into a dragon
		land(JU_DIVE, 12.0, 15.5, 3.7);
		land(JU_SPIN, 12.0, 20.0, 3.14159265);
		glide(JU_DIVE, 15.5, 17.5, 0.0);
		glide(JU_RHO, 16.5, 18.2, 1.0);
		glide(JU_THETA, 17.7, 19.7, 3.14159265);
		land(JU_RHO, 20.0, 21.5, 1.0723);
		land(JU_THETA, 20.0, 21.5, 3.0068);
		// the basilica on bar 6, a fat set, the Siegel disk on bar 7
		glide(JU_RHO, 22.5, 24.0, 1.236);
		glide(JU_THETA, 22.5, 24.0, 3.14159265);
		glide(JU_RHO, 24.5, 26.0, 0.97);
		glide(JU_THETA, 25.5, 28.0, 3.8832);
		glide(JU_RHO, 26.5, 28.0, 1.0);
		// the riser carries it up the quarter bulb to an eight-armed star; the bar 8 hit dives in
		glide(JU_THETA, 28.5, 30.3, 4.7124);
		glide(JU_RHO, 30.3, 32.0, 1.2679);
		glide(JU_THETA, 31.0, 32.0, 4.6648);
		glide(JU_DIVE, 29.0, 32.0, 0.6);
		land(JU_DIVE, 32.0, 35.5, 3.6);
		land(JU_SPIN, 32.0, 39.0, 6.2831853);
		// bar 9 pulls back and the star fattens to the four-eared set, which holds the frame to the end
		glide(JU_DIVE, 36.0, 38.0, 0.0);
		glide(JU_THETA, 37.0, 39.0, 4.7124);
		glide(JU_RHO, 37.0, 39.0, 1.0625);
	}

	void update(float t, float dt) override {
		tt = t;
	}

	void render(var& grid) override {
		float t = tt;
		float beats = t / MUSIC_BEAT;
		float kick = cue.kick_env(t);
		float snare = cue.snare_env(t);
		float hitv = cue.hit(t);
		array<float> track = [0.92, 0.9, 0.0, 0.0];
		for (int i = 0; i < key_from.size(); ++i) {
			float from = key_from[i];
			if (beats <= from) { continue; }
			float k = beats >= key_to[i] ? 1.0 : (beats - from) / (key_to[i] - from);
			float back = 1.0 - k;
			k = key_lands[i] ? 1.0 - back * back * back : k * k * (3.0 - 2.0 * k);
			int which = key_track[i];
			track[which] += (key_value[i] - track[which]) * k;
		}
		float rho = track[JU_RHO];
		float theta = track[JU_THETA];
		float dive = track[JU_DIVE];
		float spin = track[JU_SPIN] + 0.07 * math::sin(t * 0.9);
		float lr = rho * math::cos(theta);
		float li = rho * math::sin(theta);
		JU_FX = lr * 0.5;
		JU_FY = li * 0.5;
		JU_CA = lr * 0.5 - (lr * lr - li * li) * 0.25;
		JU_CB = li * 0.5 - lr * li * 0.5;
		// near alpha the map is e -> lambda e + e^2, a contraction while |e| < 1 - rho
		float home = rho < 1.0 ? (1.0 - rho) * 0.6 : 0.0;
		JU_TRAP = home * home;
		JU_DRIFT = rho < 1.0 ? 3.4 * math::log2(rho) : 0.0;
		// beta = 1 - alpha is the set's far tip: the set spans beta to -beta
		float bx = 1.0 - JU_FX;
		float by = 0.0 - JU_FY;
		float reach = math::sqrt(bx * bx + by * by);
		float half_width = reach * 1.2;
		half_width = half_width < 1.4 ? 1.4 : half_width;
		float scale = half_width * 2.0 / w * (1.0 - kick * 0.05) / math::pow(2.0, dive);
		float turn = math::atan2(by, bx) + spin - 0.6 * math::sin(theta);
		float cs = math::cos(turn);
		float sn = math::sin(turn);
		JU_UX = cs * scale;
		JU_UY = sn * scale;
		JU_VX = 0.0 - sn * scale;
		JU_VY = cs * scale;
		JU_PIXELS = 1.0 / scale;
		// the critical orbit's mean stretch tells a set with an inside (it shrinks) from a dendrite (it grows)
		float ox = JU_CA;
		float oy = JU_CB;
		float stretch = 0.0;
		int steps = 0;
		while (steps < 48 && ox * ox + oy * oy < 64.0) {
			stretch += math::log(4.0 * (ox * ox + oy * oy) + 0.000000000001);
			float nx = ox * ox - oy * oy + JU_CA;
			oy = 2.0 * ox * oy + JU_CB;
			ox = nx;
			++steps;
		}
		float thin = steps > 0 ? stretch * 0.5 / steps - 0.06 : 1.0;
		thin = thin < 0.0 ? 0.0 : (thin > 0.25 ? 0.25 : thin);
		JU_UNRESOLVED = JU_PIXELS * JU_PIXELS * math::pow(2.0, 10.0 - 400.0 * thin);
		JU_LX = -0.6 * cs + 0.8 * sn;
		JU_LY = -0.6 * sn - 0.8 * cs;
		JU_PHASE = 4096.0 + t * 9.0 + cue.chord(t) * 5.0;
		JU_INPHASE = t * 4.0;
		JU_LIFT = kick * 0.1 + snare * 0.06 + hitv * 0.14;
		JU_RIM = 1.1 + kick * 0.5 + hitv * 1.5;
		JU_W = w;
		JU_H = h;
		int rows_drawn = (h + 1) / 2;
		if (JU_ROWS.size() != rows_drawn) {
			JU_ROWS = [];
			for (int r = 0; r < rows_drawn; ++r) { JU_ROWS.push(r); }
		}
		var rows = parallel_transform(JU_ROWS, julia_row);
		int last = w * h - 1;
		int idx = 0;
		for (int y = 0; y < rows_drawn; ++y) {
			var& row = rows[y];
			for (int x = 0; x < w; ++x) {
				int v = row[x];
				grid[idx] = v & 1023;
				grid[last - idx] = v >> 10;
				++idx;
			}
		}
	}
}
