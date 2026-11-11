// FINALE — fireworks over a skyline that reads the soundtrack: the reel's last number
// and its credits. A night city stands on black water, every tower a level meter for
// one voice of the score (kick, snare, hats, bass, arpeggio, lead), its windows lit
// from the street up with a peak light riding above. Shells climb from behind the
// towers and burst on the beat: chrysanthemums, tilted rings, willows that weep gold,
// palms, double shells and five-point stars. A shell is a table of directions thrown
// from one point, slowed by drag and pulled down by gravity, so its whole flight is a
// function of its age and the scene replays to any moment. The water mirrors all of
// it. The show builds for eight bars, the credits climb the sky between shells kept to
// the flanks, a barrage crosses the sky, and after a beat of dark one last shell bursts
// into sparks that settle into JAISCRIPT; then the picture dithers to black, the word
// last.

array<int> FINALE_DITHER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];

class Finale : Scene {
	array<var> stills = [];         // the skyline at three sky brightnesses: a fresh burst lights the sky
	array<float> shell_t = [];      // the moment each shell bursts
	array<float> shell_x = [];
	array<float> shell_y = [];
	array<float> shell_size = [];
	array<int> shell_kind = [];
	array<int> shell_hue = [];
	array<float> spark_x = [];      // every kind's unit directions, one kind after another
	array<float> spark_y = [];
	array<int> spark_shift = [];    // a spark's hue as a step from its shell's
	array<int> kind_first = [];
	array<int> kind_count = [];
	array<int> kind_trail = [];
	array<float> kind_gravity = [];
	array<float> kind_life = [];
	array<int> tower_x = [];
	array<int> tower_w = [];
	array<int> tower_segments = [];
	array<int> tower_lane = [];
	array<float> tower_gain = [];
	array<int> tower_beacon = [];   // the pixel of the tower's warning light, or -1
	array<float> meter = [];
	array<float> peak = [];
	array<float> lane_now = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0];
	array<int> word_at = [];
	array<float> word_x = [];
	array<float> word_y = [];
	array<float> word_dx = [];
	array<float> word_dy = [];
	array<int> word_ink = [];
	array<int> credit_kind = [];    // 0 a gap, 1 a title, 2 a line, 3 a name and its role, 4 the link
	array<string> credit_a = [];
	array<string> credit_b = [];
	int sky = 0;         // 3 banks of 24 levels, zenith to horizon
	int star = 0;        // 4 levels
	int sky_end = 0;     // inks below this are open sky: a spark may land on them
	int spark = 0;       // 8 hues x 12 levels
	int spark_end = 0;
	int body = 0;
	int unlit = 0;
	int lane_ink = 0;    // 6 voices x 6 levels
	int peak_ink = 0;
	int beacon = 0;      // off, on
	int neon = 0;        // 4 levels of the waterline
	int black = 0;
	int inks = 0;        // the inks above; the water's two dimmed copies of them follow
	int horizon = 0;
	int period = 2;      // a meter segment and the gap above it, in rows
	int word_top = 0;
	int centre_roofs = 0; // the highest roof among the towers under the credits
	int word_h = 0;
	int fr = 0;
	float tt = 0.0;
	float rise = 0.9;    // a rocket's climb before its burst
	float credits_t = 0.0;
	float credits_len = 0.0;
	float word_t = 0.0;
	float fade_t = 0.0;
	float end_t = 0.0;

	Finale() { pixel_budget = 24000; }

	void build() override {
		name = "FINALE";
		handle = "fireworks over a neon skyline";
		replay = 3.0;
		horizon = h * 80 / 100;
		period = h >= 100 ? 3 : 2;
		credits_t = MUSIC_BAR * 8.0;
		credits_len = MUSIC_BAR * 10.0;
		word_t = MUSIC_BAR * 20.0;
		fade_t = MUSIC_BAR * 22.5;
		end_t = MUSIC_BAR * 24.0;
		build_palette();
		build_kinds();
		build_skyline();
		build_show();
		build_word();
		build_credits();
	}

	array<float> hit_times() override { return [MUSIC_BAR, MUSIC_BAR * 4.0, MUSIC_BAR * 8.0, MUSIC_BAR * 18.0, MUSIC_BAR * 20.0]; }
	array<int> hit_levels() override { return [1, 2, 1, 1, 2]; }
	int length_bars() override { return 24; }

	void build_palette() {
		array<float> lift = [0.0, 0.3, 0.75];
		for (int k = 0; k < 3; ++k) {
			float f = lift[k];
			int first = pal.gradient(24, [[3.0 + f * 14.0, 2.0 + f * 9.0, 14.0 + f * 26.0], [10.0 + f * 24.0, 5.0 + f * 14.0, 36.0 + f * 40.0], [34.0 + f * 36.0, 10.0 + f * 20.0, 64.0 + f * 46.0], [84.0 + f * 46.0, 22.0 + f * 28.0, 96.0 + f * 46.0]]);
			if (k == 0) { sky = first; }
		}
		star = pal.ramp(4, 70.0, 70.0, 110.0, 235.0, 235.0, 255.0);
		sky_end = pal.size();
		array<var> hues = [[255.0, 60.0, 110.0], [255.0, 140.0, 40.0], [255.0, 208.0, 90.0], [90.0, 255.0, 150.0], [60.0, 230.0, 255.0], [90.0, 130.0, 255.0], [190.0, 100.0, 255.0], [255.0, 70.0, 220.0]];
		for (int hu = 0; hu < 8; ++hu) {
			var c = hues[hu];
			float r = c[0];
			float g = c[1];
			float b = c[2];
			int first = pal.gradient(12, [[r * 0.14, g * 0.14, b * 0.14], [r * 0.62, g * 0.62, b * 0.62], [r, g, b], [255.0, 250.0, 240.0]]);
			if (hu == 0) { spark = first; }
		}
		spark_end = pal.size();
		body = pal.add(9.0, 7.0, 24.0);
		unlit = pal.add(25.0, 19.0, 54.0);
		// the voices in the towers' order: kick, snare, hats, bass, arpeggio, lead
		array<var> voices = [[255.0, 56.0, 130.0], [70.0, 230.0, 255.0], [200.0, 220.0, 255.0], [170.0, 90.0, 255.0], [255.0, 200.0, 80.0], [90.0, 255.0, 170.0]];
		for (int v = 0; v < 6; ++v) {
			var c = voices[v];
			float r = c[0];
			float g = c[1];
			float b = c[2];
			int first = pal.ramp(6, r * 0.34, g * 0.34, b * 0.34, r, g, b);
			if (v == 0) { lane_ink = first; }
		}
		peak_ink = pal.add(255.0, 250.0, 240.0);
		beacon = pal.ramp(2, 84.0, 8.0, 26.0, 255.0, 44.0, 64.0);
		neon = pal.ramp(4, 150.0, 20.0, 112.0, 255.0, 204.0, 250.0);
		black = pal.add(0.0, 0.0, 0.0);
		inks = pal.size();
		array<float> keep = [0.64, 0.36];
		for (int bank = 0; bank < 2; ++bank) {
			float f = keep[bank];
			for (int i = 0; i < inks; ++i) {
				int c = pal.rgb[i];
				pal.add(3.0 + (c >> 16) * f, 5.0 + ((c >> 8) & 255) * f, 16.0 + (c & 255) * f);
			}
		}
	}

	void begin_kind(int trail, float gravity, float life) {
		kind_first.push(spark_x.size());
		kind_count.push(0);
		kind_trail.push(trail);
		kind_gravity.push(gravity);
		kind_life.push(life);
	}

	void end_kind() {
		int kind = kind_first.size() - 1;
		kind_count[kind] = spark_x.size() - kind_first[kind];
	}

	void add_spark(float x, float y, int shift) {
		spark_x.push(x);
		spark_y.push(y);
		spark_shift.push(shift);
	}

	// n directions spread evenly over a sphere and seen from the front, so the rim is densest
	void add_sphere(int n, float radius, int shift) {
		for (int i = 0; i < n; ++i) {
			float z = 1.0 - 2.0 * (i + 0.5) / n;
			float r = math::sqrt(1.0 - z * z) * radius;
			float a = i * 2.39996323;
			add_spark(math::cos(a) * r, math::sin(a) * r, shift);
		}
	}

	void build_kinds() {
		// 0 chrysanthemum
		begin_kind(4, 0.018, 1.8);
		add_sphere(108, 1.0, 0);
		add_sphere(44, 0.6, 0);
		end_kind();
		// 1 a tilted ring around a core of another hue
		begin_kind(3, 0.014, 1.6);
		for (int i = 0; i < 40; ++i) {
			float a = i * 6.2831853 / 40.0;
			float x = math::cos(a);
			float y = math::sin(a) * 0.4;
			add_spark(x * 0.9 - y * 0.435, x * 0.435 + y * 0.9, 0);
		}
		add_sphere(18, 0.3, 4);
		end_kind();
		// 2 willow: heavy sparks with long tails
		begin_kind(7, 0.07, 2.7);
		add_sphere(64, 0.85, 0);
		end_kind();
		// 3 palm: nine fronds
		begin_kind(4, 0.04, 1.9);
		for (int arm = 0; arm < 9; ++arm) {
			for (int s = 0; s < 6; ++s) {
				float a = arm * 6.2831853 / 9.0 + 0.2 + (s % 2 == 0 ? 0.03 : -0.03);
				float reach = 0.5 + s * 0.1;
				add_spark(math::cos(a) * reach, math::sin(a) * reach, 0);
			}
		}
		end_kind();
		// 4 a shell inside a shell
		begin_kind(3, 0.018, 1.8);
		add_sphere(84, 1.0, 0);
		add_sphere(36, 0.5, 3);
		end_kind();
		// 5 a five-point star, one point up
		begin_kind(2, 0.01, 1.6);
		for (int v = 0; v < 10; ++v) {
			float a0 = v * 0.62831853 - 1.57079633;
			float a1 = (v + 1) * 0.62831853 - 1.57079633;
			float r0 = v % 2 == 0 ? 1.0 : 0.42;
			float r1 = v % 2 == 0 ? 0.42 : 1.0;
			for (int s = 0; s < 7; ++s) {
				float f = s / 7.0;
				add_spark(math::cos(a0) * r0 * (1.0 - f) + math::cos(a1) * r1 * f, math::sin(a0) * r0 * (1.0 - f) + math::sin(a1) * r1 * f, 0);
			}
		}
		end_kind();
	}

	void build_skyline() {
		int n = w * h;
		array<int> still = gfx.make_grid(n);
		for (int y = 0; y < horizon; ++y) {
			float f = y * 1.0 / horizon;
			float v = f * f * 23.0;
			for (int x = 0; x < w; ++x) {
				int lv = math::itrunc(v + FINALE_DITHER[(y & 3) * 4 + (x & 3)] / 16.0);
				still[y * w + x] = sky + (lv > 23 ? 23 : lv);
			}
		}
		for (int s = 0; s < n / 240; ++s) {
			int x = rng.next(w);
			int y = rng.next(horizon * 3 / 4);
			still[y * w + x] = star + rng.next(4);
		}
		for (int x = 0; x < w; ++x) { still[horizon * w + x] = neon; }
		int count = w / 9;
		count = count < 8 ? 8 : (count > 22 ? 22 : count);
		float slot = w * 1.0 / count;
		// the voices dealt so neighbours differ: bass, kick, arpeggio, snare, lead, hats
		array<int> deal = [3, 0, 4, 1, 5, 2];
		for (int j = 0; j < count; ++j) {
			float u = (j + 0.5) / count * 2.0 - 1.0;
			float tall = 0.13 + u * u * 0.2 + rng.next(100) * 0.0009;
			int x0 = math::itrunc(j * slot) + 1;
			int tw = math::itrunc((j + 1) * slot) - x0 - 1;
			tw = tw < 3 ? 3 : tw;
			int segments = math::itrunc(tall * h) / period;
			segments = segments < 3 ? 3 : segments;
			int top = horizon - segments * period - 1;
			for (int y = top; y < horizon; ++y) {
				for (int x = x0; x < x0 + tw; ++x) { still[y * w + x] = body; }
			}
			if (u * u < 0.36 && (centre_roofs == 0 || top < centre_roofs)) { centre_roofs = top; }
			tower_x.push(x0);
			tower_w.push(tw);
			tower_segments.push(segments);
			tower_lane.push(deal[j % 6]);
			tower_gain.push(0.8 + rng.next(45) * 0.01);
			int light = -1;
			if (u * u > 0.2 && rng.next(3) > 0 && top > 6) {
				int cx = x0 + tw / 2;
				for (int m = 1; m <= 3; ++m) { still[(top - m) * w + cx] = body; }
				light = (top - 4) * w + cx;
			}
			tower_beacon.push(light);
			meter.push(0.0);
			peak.push(0.0);
		}
		stills.push(still);
		for (int bank = 1; bank < 3; ++bank) {
			array<int> lit = still;
			for (int i = 0; i < n; ++i) {
				if (lit[i] < sky + 24) { lit[i] = lit[i] + bank * 24; }
			}
			stills.push(lit);
		}
		for (int k = 0; k < 3; ++k) {
			var& into = stills[k];
			for (int j = 0; j < count; ++j) { paint_windows(into, j, 0, tower_segments[j], unlit, unlit); }
		}
	}

	// a tower's window rows from segment `from` up to (not including) `to`, shaded street to roof
	void paint_windows(var& grid, int j, int from, int to, int low, int high) {
		int segments = tower_segments[j];
		int x0 = tower_x[j] + 1;
		int x1 = tower_x[j] + tower_w[j] - 1;
		for (int s = from; s < to; ++s) {
			int ink = low + (high - low) * s / (segments - 1);
			for (int r = 0; r < period - 1; ++r) {
				int row = (horizon - 1 - s * period - r) * w;
				for (int x = x0; x < x1; ++x) {
					if ((x - x0) % 3 != 2) { grid[row + x] = ink; }
				}
			}
		}
	}

	void shell(float t, float x, float y, float size, int kind, int hue) {
		shell_t.push(t);
		shell_x.push(x * w);
		shell_y.push(y * h);
		shell_size.push(size * h);
		shell_kind.push(kind);
		shell_hue.push(kind == 2 ? 2 : hue);
	}

	// a shell somewhere between x0 and x1 of the width; its kind and colour take turns
	void drawn_shell(float t, float x0, float x1, float size, int serial) {
		array<int> kinds = [0, 4, 1, 3, 5, 2];
		array<int> hues = [7, 4, 6, 2, 0, 5, 1, 3];
		float x = x0 + rng.next(1000) * 0.001 * (x1 - x0);
		float y = 0.17 + rng.next(1000) * 0.00015;
		shell(t, x, y, size + rng.next(100) * 0.0006, kinds[(serial + rng.next(2)) % 6], hues[(serial * 3 + rng.next(2)) & 7]);
	}

	void build_show() {
		float bar = MUSIC_BAR;
		float beat = MUSIC_BEAT;
		int serial = 0;
		// the opener, then two shells a bar
		shell(bar, 0.5, 0.27, 0.27, 0, 7);
		for (int k = 1; k < 6; ++k) {
			drawn_shell(bar + k * beat * 2.0, 0.16, 0.84, 0.18, serial);
			++serial;
		}
		// the drop: three at once, then a shell on every beat
		shell(bar * 4.0, 0.2, 0.27, 0.2, 4, 4);
		shell(bar * 4.0, 0.5, 0.25, 0.25, 5, 2);
		shell(bar * 4.0, 0.8, 0.27, 0.2, 4, 7);
		for (int k = 1; k < 16; ++k) {
			drawn_shell(bar * 4.0 + k * beat, 0.14, 0.86, 0.17, serial);
			++serial;
		}
		// under the credits the shells keep to the flanks, both sides on the bar line
		for (int k = 0; k < 40; ++k) {
			float t = credits_t + k * beat;
			bool left = k % 2 == 0;
			drawn_shell(t, left ? 0.06 : 0.86, left ? 0.14 : 0.94, 0.12, serial);
			++serial;
			if (k % 4 == 0) {
				drawn_shell(t, left ? 0.86 : 0.06, left ? 0.94 : 0.14, 0.12, serial);
				++serial;
			}
		}
		// the barrage: a shell every half beat leaping across the sky, dark for the last beat
		shell(bar * 18.0, 0.14, 0.3, 0.18, 2, 2);
		shell(bar * 18.0, 0.86, 0.3, 0.18, 2, 2);
		array<int> hues = [7, 4, 6, 2, 0, 5, 1, 3];
		for (int k = 0; k < 14; ++k) {
			float x = 0.1 + 0.8 * ((k * 5) % 14) / 13.0;
			shell(bar * 18.0 + k * beat * 0.5, x, 0.17 + (k % 3) * 0.07, 0.18 + (k % 4) * 0.02, k % 6, hues[k & 7]);
		}
		// a crown of small shells over the word
		for (int k = 4; k < 11; ++k) {
			shell(word_t + k * beat, 0.14 + 0.72 * ((k * 3) % 7) / 6.0, 0.115, 0.095, k % 2 == 0 ? 5 : 1, hues[k & 7]);
		}
	}

	void build_word() {
		int scale = 1;
		while (gfx.text_width("JAISCRIPT", scale + 1) <= w * 84 / 100 && scale < 8) { ++scale; }
		int word_w = gfx.text_width("JAISCRIPT", scale);
		word_h = 7 * scale;
		word_top = horizon * 42 / 100 - word_h / 2;
		array<int> mask = gfx.make_grid(w * h);
		gfx.draw_text(mask, w, h, "JAISCRIPT", (w - word_w) / 2, word_top, scale, scale, 1, 1, -1, 9);
		// gold at the top of the letters down to magenta at the foot
		array<int> rows = [2, 2, 1, 0, 7, 7];
		for (int i = 0; i < w * h; ++i) {
			if (mask[i] != 1) { continue; }
			int k = word_at.size();
			float a = k * 2.39996323;
			float r = 0.3 + 0.7 * ((k * 7919) % 97) / 96.0;
			word_at.push(i);
			word_x.push(i % w);
			word_y.push(i / w);
			word_dx.push(math::cos(a) * r);
			word_dy.push(math::sin(a) * r);
			word_ink.push(spark + rows[(i / w - word_top) * 6 / word_h] * 12);
		}
	}

	void credit(int kind, string a, string b) {
		credit_kind.push(kind);
		credit_a.push(a);
		credit_b.push(b);
	}

	void build_credits() {
		credit(1, "THE JAISCRIPT DEMOREEL", "");
		credit(0, "", "");
		credit(2, "twelve scenes . six transitions", "");
		credit(2, "every pixel computed by the jaiscript vm", "");
		credit(0, "", "");
		credit(3, "EIGHT", "jaiscript in eight dimensions");
		credit(3, "DONUT", "a product shot");
		credit(3, "JULIA", "the whole set, morphing in frame");
		credit(3, "LAVALAMP", "a raytraced forest of lava lamps");
		credit(3, "PIPEDREAM", "chrome pipes on a synthwave set");
		credit(3, "BOIDS", "a murmuration that flies geometry");
		credit(3, "GLOBHUNTER", "a torchlit raycaster hunt");
		credit(3, "CANYONRUN", "a golden-hour voxelspace canyon");
		credit(3, "CHROME", "chrome, glass and a polished floor");
		credit(3, "OSSUARY", "a skull spider, a web, a moth");
		credit(3, "MAELSTROM", "neon ink in a wave tank");
		credit(3, "FINALE", "fireworks over a neon skyline");
		credit(0, "", "");
		credit(3, "ONE", "scripting");
		credit(3, "TWO", "serialization");
		credit(3, "THREE", "reflection");
		credit(3, "FOUR", "hot reload");
		credit(3, "FIVE", "parallel");
		credit(3, "SIX", "signals");
		credit(3, "SEVEN", "debugging");
		credit(3, "EIGHT", "embedding");
		credit(0, "", "");
		credit(2, "sextant pixels, six to a cell", "");
		credit(2, "deterministic to the last byte", "");
	}

	void update(float t, float dt) override {
		tt = t;
		fr = math::itrunc(t * 30.0);
		if (cue != null) {
			lane_now[0] = cue.kick_env(t);
			lane_now[1] = cue.snare_env(t);
			lane_now[2] = cue.hat_env(t);
			lane_now[3] = cue.bass_env(t);
			lane_now[4] = cue.arp_env(t);
			lane_now[5] = cue.lead_env(t);
		} else {
			float f = math::fmod(t / MUSIC_BEAT, 1.0);
			for (int v = 0; v < 6; ++v) { lane_now[v] = (1.0 - f) * (1.0 - f); }
		}
		float wake = t / MUSIC_BAR;
		wake = wake > 1.0 ? 1.0 : wake;
		for (int j = 0; j < meter.size(); ++j) {
			float level = (0.12 + lane_now[tower_lane[j]] * tower_gain[j]) * wake;
			level = level > 1.0 ? 1.0 : level;
			float held = meter[j] - dt * 2.4;
			meter[j] = level > held ? level : held;
			float riding = peak[j] - dt * 0.4;
			peak[j] = meter[j] > riding ? meter[j] : riding;
		}
	}

	void render(var& grid) override {
		float t = tt;
		grid = stills[sky_bank(t)];
		draw_meters(grid, t);
		draw_shells(grid, t);
		draw_word(grid, t);
		draw_water(grid, t);
		roll_credits(t);
		sign_off(t);
		fade_out(grid, t);
	}

	// how bright the freshest burst leaves the sky: 0, 1 or 2
	int sky_bank(float t) {
		float flash = 0.0;
		for (int b = 0; b < shell_t.size(); ++b) {
			float a = t - shell_t[b];
			if (a < 0.0 || a > 0.28) { continue; }
			float f = (1.0 - a / 0.28) * shell_size[b] / (h * 0.2);
			flash = f > flash ? f : flash;
		}
		float since = t - word_t;
		if (since >= 0.0 && since < 0.5) { flash = 1.0 - since; }
		return flash > 0.6 ? 2 : (flash > 0.22 ? 1 : 0);
	}

	void draw_meters(var& grid, float t) {
		int kick = math::itrunc(lane_now[0] * 3.9);
		for (int x = 0; x < w; ++x) { grid[horizon * w + x] = neon + (kick > 3 ? 3 : kick); }
		bool blink = math::itrunc(t / MUSIC_BEAT) % 2 == 0;
		for (int j = 0; j < meter.size(); ++j) {
			int segments = tower_segments[j];
			int lit = math::itrunc(meter[j] * segments + 0.5);
			lit = lit > segments ? segments : lit;
			int ramp = lane_ink + tower_lane[j] * 6;
			paint_windows(grid, j, 0, lit, ramp, ramp + 5);
			int top = math::itrunc(peak[j] * segments + 0.5);
			top = top >= segments ? segments - 1 : top;
			if (top >= lit) { paint_windows(grid, j, top, top + 1, peak_ink, peak_ink); }
			if (tower_beacon[j] >= 0) { grid[tower_beacon[j]] = beacon + (blink ? 1 : 0); }
		}
	}

	void draw_shells(var& grid, float t) {
		for (int b = 0; b < shell_t.size(); ++b) {
			float a = t - shell_t[b];
			int kind = shell_kind[b];
			float life = kind_life[kind];
			if (a < 0.0 - rise || a > life) { continue; }
			if (a < 0.0) {
				rocket(grid, shell_x[b], shell_y[b], a, b);
				continue;
			}
			float cx = shell_x[b];
			float cy = shell_y[b];
			float size = shell_size[b];
			int hue = shell_hue[b];
			if (a < 0.07) { flash_disc(grid, cx, cy, size * (0.07 + a * 1.4), spark + hue * 12 + 11); }
			int first = kind_first[kind];
			int count = kind_count[kind];
			int trail = kind_trail[kind];
			float gap = trail > 4 ? 0.075 : 0.05;
			bool guttering = a > life * 0.55;
			for (int p = 0; p < trail; ++p) {
				float at = a - p * gap;
				float glow = 1.0 - at / life;
				if (at <= 0.0 || glow <= 0.0) { continue; }
				int lv = math::itrunc(11.9 * math::pow(glow, 0.6) * (1.0 - p / (trail + 0.5)));
				if (lv < 1) { continue; }
				float k = size * (1.0 - math::exp(0.0 - 3.2 * at));
				float fall = kind_gravity[kind] * h * at * at;
				for (int j = 0; j < count; ++j) {
					if (guttering && ((j * 37 + fr * 11 + b * 5) & 3) == 0) { continue; }
					int s = first + j;
					float fx = cx + spark_x[s] * k;
					float fy = cy + spark_y[s] * k + fall;
					if (fx < 0.0 || fx >= w || fy < 0.0 || fy >= horizon) { continue; }
					int i = math::itrunc(fy) * w + math::itrunc(fx);
					int under = grid[i];
					if (under < sky_end || (under < spark_end && (under - spark) % 12 < lv)) {
						grid[i] = spark + ((hue + spark_shift[s]) & 7) * 12 + lv;
					}
				}
			}
		}
	}

	// a shell on its way up: a gold head and a tail, `a` seconds before the burst
	void rocket(var& grid, float x, float y, float a, int b) {
		float p = 1.0 + a / rise;
		float fy = horizon - (horizon - y) * (1.0 - (1.0 - p) * (1.0 - p));
		float sway = math::sin(p * 9.0 + b) * (1.0 - p) * 1.5;
		for (int k = 0; k < 5; ++k) {
			int py = math::itrunc(fy) + k;
			float fx = x + sway * (1.0 + k * 0.3);
			if (py < 0 || py >= horizon || fx < 0.0 || fx >= w) { continue; }
			int i = py * w + math::itrunc(fx);
			if (grid[i] < sky_end) { grid[i] = spark + 2 * 12 + 10 - k * 2; }
		}
	}

	void flash_disc(var& grid, float cx, float cy, float r, int ink) {
		int y0 = math::itrunc(cy - r);
		int y1 = math::itrunc(cy + r) + 1;
		y0 = y0 < 0 ? 0 : y0;
		y1 = y1 > horizon ? horizon : y1;
		for (int y = y0; y < y1; ++y) {
			float ey = y - cy;
			float span = r * r - ey * ey;
			if (span <= 0.0) { continue; }
			float half = math::sqrt(span);
			int x0 = math::itrunc(cx - half);
			int x1 = math::itrunc(cx + half) + 1;
			x0 = x0 < 0 ? 0 : x0;
			x1 = x1 > w ? w : x1;
			for (int x = x0; x < x1; ++x) {
				if (grid[y * w + x] < sky_end) { grid[y * w + x] = ink; }
			}
		}
	}

	// the last shell: a spark for every pixel of the word, thrown out white-hot, then
	// drawn home to its place in the letters
	void draw_word(var& grid, float t) {
		float a = t - word_t;
		float ox = w * 0.5;
		float oy = word_top + word_h * 0.5;
		if (a < 0.0) {
			if (a > 0.0 - rise) { rocket(grid, ox, oy, a, 0); }
			return;
		}
		int n = word_at.size();
		if (a > 1.7) {
			int lift = lane_now[0] > 0.5 ? 1 : 0;
			for (int j = 0; j < n; ++j) { grid[word_at[j]] = word_ink[j] + 7 + lift + ((j * 13 + fr) & 3); }
			return;
		}
		if (a < 0.12) { flash_disc(grid, ox, oy, h * (0.06 + a * 1.2), spark + 2 * 12 + 11); }
		for (int p = 2; p >= 0; --p) {
			float at = a - p * 0.045;
			if (at <= 0.0) { continue; }
			float k = (1.0 - math::exp(0.0 - 4.5 * at)) * h * 0.42;
			float s = (at - 0.4) / 1.1;
			s = s < 0.0 ? 0.0 : (s > 1.0 ? 1.0 : s);
			s = s * s * (3.0 - 2.0 * s);
			float fall = at * at * h * 0.03;
			int lv = (s < 0.7 ? 11 : 10) - p * 3;
			for (int j = 0; j < n; ++j) {
				float fx = (ox + word_dx[j] * k * 1.4) * (1.0 - s) + word_x[j] * s;
				float fy = (oy + word_dy[j] * k + fall) * (1.0 - s) + word_y[j] * s;
				if (fx < 0.0 || fx >= w || fy < 0.0 || fy >= horizon) { continue; }
				int i = math::itrunc(fy + 0.5) * w + math::itrunc(fx + 0.5);
				if (i < horizon * w && grid[i] < spark_end) { grid[i] = word_ink[j] + lv; }
			}
		}
	}

	// the water: the picture above it folded down, rippling, in the dimmed inks
	void draw_water(var& grid, float t) {
		int drift = 5 - math::itrunc(t * 6.0) % 5;
		for (int d = 1; horizon + d < h; ++d) {
			int from = horizon - 1 - math::itrunc(d * 2.6);
			from = (from < 0 ? 0 : from) * w;
			int row = (horizon + d) * w;
			int sway = math::itrunc(math::sin(d * 0.8 + t * 2.4) * (0.6 + d * 0.1) + 100.5) - 100;
			int bank = (d * 3 + drift) % 5 < 2 ? inks * 2 : inks;
			for (int x = 0; x < w; ++x) {
				int sx = x + sway;
				sx = sx < 0 ? 0 : (sx >= w ? w - 1 : sx);
				grid[row + x] = grid[from + sx] + bank;
			}
		}
	}

	// the credits climb the sky from the rooftops, dimming into its edges
	void roll_credits(float t) {
		float p = (t - credits_t) / credits_len;
		if (p <= 0.0 || p >= 1.0) { return; }
		int top = 1;
		int bottom = centre_roofs * HB_ROWS / h - 1;
		int n = credit_kind.size();
		int climbed = math::itrunc(p * (n + bottom - top + 1));
		int mid = HB_COLS / 2;
		for (int k = 0; k < n; ++k) {
			int row = bottom + 1 + k - climbed;
			if (row < top || row > bottom || credit_kind[k] == 0) { continue; }
			int edge = row - top < bottom - row ? row - top : bottom - row;
			float keep = edge >= 3 ? 1.0 : (edge + 1) * 0.25;
			string a = credit_a[k];
			switch (credit_kind[k]) {
				case 1:
					txt_put(mid - a.size() / 2, row, a, shade(rgb(255, 236, 190), keep));
				break;
				case 3:
					txt_put(mid - 2 - a.size(), row, a, shade(rgb(255, 206, 110), keep));
					txt_put(mid + 1, row, credit_b[k], shade(rgb(186, 180, 236), keep));
				break;
				default:
					txt_put(mid - a.size() / 2, row, a, shade(rgb(214, 208, 255), keep));
			}
		}
	}

	// two lines typed under the word once it has settled
	void sign_off(float t) {
		float a = t - word_t - 1.9;
		if (a <= 0.0 || t >= end_t - 0.3) { return; }
		float keep = t < fade_t ? 1.0 : 1.0 - (t - fade_t) / (end_t - 0.3 - fade_t);
		string thanks = "thanks for watching";
		string link = "jaiscript.com";
		int row = (word_top + word_h) * HB_ROWS / h + 2;
		int shown = math::itrunc(a * 26.0);
		txt_put((HB_COLS - thanks.size()) / 2, row, thanks.substr(0, shown > thanks.size() ? thanks.size() : shown), shade(rgb(255, 236, 190), keep));
		int linked = math::itrunc((a - 1.0) * 26.0);
		if (linked > 0) { txt_put((HB_COLS - link.size()) / 2, row + 2, link.substr(0, linked > link.size() ? link.size() : linked), shade(rgb(120, 200, 255), keep)); }
	}

	// the picture dithers to black, the word last of all
	void fade_out(var& grid, float t) {
		if (t <= fade_t) { return; }
		float k = (t - fade_t) / (end_t - 0.3 - fade_t);
		int gone = math::itrunc(k * 290.0);
		int n = w * h;
		for (int i = 0; i < n; ++i) {
			if (threshold(i) < gone) { grid[i] = black; }
		}
		int word_gone = math::itrunc((k - 0.45) * 520.0);
		for (int j = 0; j < word_at.size(); ++j) {
			if (threshold(word_at[j] + 977) >= word_gone) { grid[word_at[j]] = word_ink[j] + 7 + ((j * 13 + fr) & 3); }
		}
	}

	// a fixed 0..255 for each pixel, kept under 2^62 so the checked multiplies never overflow
	int threshold(int i) {
		int v = (i * 73856093) & 2147483647;
		v = ((v ^ (v >> 13)) * 1274126177) & 2147483647;
		return (v ^ (v >> 16)) & 255;
	}
}
