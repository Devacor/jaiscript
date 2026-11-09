// Scene interface + the Reel manager: timed auto-advance, transitions, title
// cards, HUD. Everything the host calls funnels through Reel.frame().
// Scenes see w and h in PIXELS (half blocks: one column and two rows a cell;
// sextants: two columns and 9/4 rows a cell) and paint square pixels.

class Scene {
	string name = "SCENE";
	string handle = "";
	int w = 0;
	int h = 0;
	int seed = 0;
	var pal = null;
	var rng = null;
	float replay = 3.0;   // the sim-seconds a jump replays to arrive mid-flight
	var cue = null;       // the scene's music (scenes/music.jai): ask it for the kick, the chord, the energy, the hits
	int pixel_budget = 0; // set in the scene's constructor: past this many pixels the scene renders smaller and scales up

	void setup(int w_, int h_, int seed_) {
		w = w_;
		h = h_;
		seed = seed_;
		rng = Rng(seed_);
		pal = Palette();
		build();
	}

	void build() {}
	void update(float t, float dt) {}
	void render(var& grid) {}
	void text_rows(var& rows) {}    // glyph rows laid over the picture (the finale's credits)
	// the scene's big moments in its own seconds (on beats or bar lines), and how big:
	// 2 an impact with a riser into it, 1 an accent; the composer scores them
	array<float> hit_times() { return []; }
	array<int> hit_levels() { return []; }
	// how many bars the scene needs for its showpiece to land in full, its last image held;
	// 0 takes the reel's default. The reel and the scene's music both run this long.
	int length_bars() { return 0; }
}

// one HUD row: styled segments clipped to the row's width, with a second line's
// segments right-aligned after them when both fit
class HudLine {
	int width = 0;
	int used = 0;
	string text = "";

	HudLine(int w) { width = w; }

	void put(string style, string s) {
		if (used >= width) { return; }
		string shown = s.size() > width - used ? s.substr(0, width - used) : s;
		text += style + shown;
		used += shown.size();
	}

	string finish(string fill, var& right) {
		int gap = width - used - right.used;
		if (gap < 0) { return text + fill + string::repeat(" ", width - used) + RESET; }
		return text + fill + string::repeat(" ", gap) + right.text + RESET;
	}
}

class Reel {
	int w = 0;           // terminal columns
	int h = 0;           // terminal rows
	bool sext = false;   // sextant cells, else half blocks
	int pw = 0;          // pixel columns
	int ph = 0;          // pixel rows
	int seed = 0;
	bool smoke = false;
	string backend = "";
	bool hud = true;
	bool hud_full = false;   // 'h' steps the HUD: the performance chip, the full status and key bars, off
	bool paused = false;

	int nscenes = 11;
	int intro_idx = 11;    // the cold open: one more entry past the scenes, played once at the start
	var bars = [];         // each scene's length in bars of the 124 bpm soundtrack
	var cues = [];
	var seed_slots = [];   // each scene seeds from its own slot, so the running order never changes a scene's frames
	var cur = null;
	var nxt = null;
	int cur_idx = 0;
	int pending_idx = 0;
	float scene_t = 0.0;
	float dur_cur = 10.0;
	float title_len = MUSIC_BAR;
	float trans_len = MUSIC_BEAT * 2.0;
	bool in_trans = false;
	bool finish_pending = false;
	float trans_t = 0.0;
	int trans_kind = 0;
	// how each scene hands over to the next: DONUT dives through its hole into JULIA, JULIA swirls
	// into the lamps, the wax ripples into chrome pipes, the pipes iris onto the flock, the flock
	// melts like Doom into GLOBHUNTER, the hunt zooms out over the canyon, the canyon ripples
	// into CHROME, the glass shatters into OSSUARY, the web swirls into the ink, the ink dives
	// into FINALE, and FINALE irises back to the start
	array<int> out_transition = [5, 8, 7, 3, 4, 5, 7, 6, 8, 5, 3, 5];
	int trans_count = 0;
	int trans_frame = 0;

	array<int> grid_a = [];
	array<int> grid_b = [];
	array<int> flash_noise = [];   // a threshold per pixel for the dithered flashes and the vignette
	array<int> ring = [];          // per pixel: its distance from the centre, 0 there to 1000 at the corners
	array<int> small_a = [];   // the scaled-down frames of scenes past their pixel budget
	array<int> small_b = [];
	array<float> melt = [];
	var ui = null;
	int ui_off = 0;
	int off_b = 0;
	int ui_band = 0;     // 12 band shades
	int ui_rule = 0;     // 2 rule lines
	int ui_ink = 0;      // 8-step letter ramp
	int ui_shine = 0;
	int ui_shadow = 0;
	int ui_edge = 0;

	string reload_files = "";   // the scene files the last 'r' changed
	string reload_error = "";   // or its first failure: the diagnostic, its source line and caret
	float reload_left = 0.0;    // seconds its rows stay up

	Reel(int w_, int h_, int seed_, string backend_, bool smoke_, bool hud_) {
		w = w_;
		h = h_;
		sext = PIX_MODE == "sext" || (PIX_MODE == "" && hb_pixel_w(w_, true) * hb_pixel_h(h_, true) <= PIX_BUDGET);
		pw = hb_pixel_w(w_, sext);
		ph = hb_pixel_h(h_, sext);
		seed = seed_;
		backend = backend_;
		smoke = smoke_;
		hud = hud_;
		grid_a = gfx.make_grid(pw * ph);
		grid_b = gfx.make_grid(pw * ph);
		melt = gfx.make_fgrid(pw);
		ui = Palette();
		ui_band = ui.size();
		for (int b = 0; b < 12; ++b) {
			float f = b < 6 ? b / 5.0 : (11 - b) / 5.0;
			ui.add(10.0 + 14.0 * f, 8.0 + 8.0 * f, 30.0 + 26.0 * f);
		}
		ui_rule = ui.add(255, 206, 110);
		ui.add(110, 200, 255);
		ui_ink = ui.gradient(8, [[255.0, 250.0, 214.0], [255.0, 214.0, 110.0], [255.0, 128.0, 60.0], [214.0, 52.0, 92.0]]);
		ui_shine = ui.add(255, 255, 255);
		ui_shadow = ui.add(4, 2, 12);
		ui_edge = ui.add(255, 244, 200);
		bars = [6, 6, 6, 7, 7, 8, 8, 6, 21, 8, 49, 4];
		seed_slots = [3, 4, 5, 6, 8, 9, 10, 13, 15, 17, 16, 21];
		for (int i = 0; i <= intro_idx; ++i) {
			var probe = make_scene(i);
			if (probe.length_bars() > 0) { bars[i] = probe.length_bars(); }
			cues.push(Cue(bars[i], music_mood(i), seed + i * 101, probe.hit_times(), probe.hit_levels()));
		}
		start_scene(intro_idx, 0.0);
	}

	var make_scene(int idx) {
		switch (idx) {
			case 0: return Donut();
			case 1: return Julia();
			case 2: return Lava();
			case 3: return Pipes();
			case 4: return Boids();
			case 5: return GlobHunter();
			case 6: return CanyonRun();
			case 7: return Chrome();
			case 8: return Ossuary();
			case 9: return Maelstrom();
			case 10: return Finale();
			default: return Intro();
		}
	}

	float dur_for(int idx) {
		return smoke ? 3.0 : bars[idx] * MUSIC_BAR;
	}

	// the cue that should be sounding: the incoming scene's from the moment its transition starts
	int music_cue() { return in_trans || finish_pending ? pending_idx : cur_idx; }

	// a scene at its grid, or at the largest grid within its pixel budget
	void setup_scene(var& sc, int idx) {
		sc.cue = cues[idx];
		int sw = pw;
		int sh = ph;
		if (sc.pixel_budget > 0 && pw * ph > sc.pixel_budget) {
			float k = math::sqrt(sc.pixel_budget * 1.0 / (pw * ph));
			sw = itrunc(pw * k);
			sh = itrunc(ph * k);
		}
		sc.setup(sw, sh, seed + seed_slots[idx] * 7919);
	}

	// render into a grid of the reel's size, scaling up a scene that renders smaller
	void render_scene(var& sc, var& target, var& scratch) {
		if (sc.w == pw && sc.h == ph) {
			sc.render(target);
			return;
		}
		if (scratch.size() != sc.w * sc.h) {
			scratch = [];
			for (int i = 0; i < sc.w * sc.h; ++i) { scratch.push(0); }
		}
		sc.render(scratch);
		int sw = sc.w;
		int sh = sc.h;
		int idx = 0;
		for (int y = 0; y < ph; ++y) {
			int row = (y * sh / ph) * sw;
			for (int x = 0; x < pw; ++x) {
				target[idx] = scratch[row + x * sw / pw];
				++idx;
			}
		}
	}

	// the presenter's string tables: current scene, incoming scene (during a
	// transition), then the reel's own UI entries
	void bind_palettes() {
		hb_ensure(w, h, sext);
		HB_FG = [];
		HB_BG = [];
		HB_RGB = [];
		HB_LUM = [];
		absorb(cur.pal);
		off_b = HB_RGB.size();
		if (in_trans) { absorb(nxt.pal); }
		ui_off = HB_RGB.size();
		absorb(ui);
	}

	void absorb(var& p) {
		int n = p.rgb.size();
		for (int i = 0; i < n; ++i) {
			HB_FG.push(p.fg[i]);
			HB_BG.push(p.bg[i]);
			int c = p.rgb[i];
			HB_RGB.push(c);
			HB_LUM.push((c >> 16) * 2 + ((c >> 8) & 255) * 3 + (c & 255));
		}
	}

	void start_scene(int idx, float at_t) {
		cur_idx = idx == intro_idx ? idx : ((idx % nscenes) + nscenes) % nscenes;
		cur = make_scene(cur_idx);
		setup_scene(cur, cur_idx);
		dur_cur = dur_for(cur_idx);
		scene_t = at_t;
		in_trans = false;
		finish_pending = false;
		nxt = null;
		trans_t = 0.0;
		bind_palettes();
	}

	// runs at the top of the frame AFTER the one that finished the transition
	void complete_transition() {
		finish_pending = false;
		in_trans = false;
		cur = nxt;
		nxt = null;
		cur_idx = pending_idx;
		dur_cur = dur_for(cur_idx);
		scene_t = trans_t;
		trans_t = 0.0;
		bind_palettes();
	}

	void begin_transition() {
		pending_idx = cur_idx == intro_idx ? 0 : (cur_idx + 1) % nscenes;
		nxt = make_scene(pending_idx);
		setup_scene(nxt, pending_idx);
		in_trans = true;
		trans_t = 0.0;
		trans_kind = out_transition[cur_idx];
		++trans_count;
		trans_frame = 0;
		int m = 0;
		for (int x = 0; x < pw; ++x) {
			m += ((x * 7 + trans_count * 13 + (x * x) % 11) % 3) - 1;
			m = m < 0 ? 0 : (m > 14 ? 14 : m);
			melt[x] = m * 1.0;
		}
		bind_palettes();
	}

	// a jump to a scene mid-flight: same scene, same clock; stateful scenes
	// replay up to their replay seconds so they arrive mid-flight instead of empty.
	void jump(int idx, float at_t) {
		start_scene(idx, at_t);
		int steps = itrunc(at_t * 30.0);
		int most = itrunc(cur.replay * 30.0);
		steps = steps > most ? most : steps;
		float ft = at_t - steps * 0.0333333333;
		for (int i = 0; i < steps; ++i) {
			ft += 0.0333333333;
			cur.update(ft, 0.0333333333);
		}
	}

	// after the host re-executes scene files, rebuild the live scene so
	// build()-time changes (palettes, tables, layouts) show immediately
	void reload_current() {
		float keep_t = scene_t;
		start_scene(cur_idx, keep_t);
	}

	void reloaded(string changed, string failed) {
		reload_files = changed.split(" ").join(", ");
		reload_error = failed;
		reload_left = failed == "" ? 3.0 : 8.0;
	}

	// what the last reload did, under the HUD: the files it changed and the scene it
	// rebuilt, or the file that failed (its previous code still running) with the error
	void reload_overlay(var& rows) {
		int row = hud && hud_full ? 1 : 0;
		var none = HudLine(0);
		var head = HudLine(w);
		if (reload_error == "") {
			string band = sgr_bg(14, 44, 40);
			head.put(sgr_bg(96, 224, 168) + sgr_fg(10, 34, 28), " RELOADED ");
			head.put(band + sgr_fg(226, 255, 240), reload_files == "" ? "  no scene file changed" : `  ${reload_files}`);
			head.put(band + sgr_fg(130, 190, 170), `  ${cur.name} rebuilt at ${gfx.fmt1(scene_t)} s `);
			rows[row] = head.finish(band, none);
			return;
		}
		string band = sgr_bg(48, 10, 24);
		string file = reload_error.substr(0, reload_error.find(":"));
		head.put(sgr_bg(214, 52, 92) + sgr_fg(255, 250, 230), " RELOAD FAILED ");
		head.put(band + sgr_fg(255, 214, 220), `  ${file} keeps running its previous code`);
		if (reload_files != "") { head.put(band + sgr_fg(170, 130, 150), `  (reloaded ${reload_files})`); }
		rows[row] = head.finish(band, none);
		var lines = reload_error.split("\n");
		var inks = [sgr_fg(255, 170, 180), sgr_fg(238, 236, 255), sgr_fg(255, 206, 110)];
		for (int k = 0; k < lines.size() && k < 3 && row + 1 + k < h; ++k) {
			var body = HudLine(w);
			body.put(band + inks[k], "  " + lines[k]);
			rows[row + 1 + k] = body.finish(band, none);
		}
	}

	void handle_key(string key) {
		if (key == "") { return; }
		if (key == "h") {
			if (!hud) {
				hud = true;
				hud_full = false;
			} else if (!hud_full) {
				hud_full = true;
			} else {
				hud = false;
			}
		}
		else if (key == "f") { paused = !paused; }
		else if (key == "n" || key == " " || key == "right") { start_scene(cur_idx + 1, 0.0); }
		else if (key == "p" || key == "left") { start_scene(cur_idx - 1, 0.0); }
		else if (key.size() == 1) {
			char c0 = key[0];
			if (c0 >= '1' && c0 <= '9') {
				int d = key.to_int();
				if (d <= nscenes) { start_scene(d - 1, 0.0); }
			}
			else if (c0 == '0') { start_scene(nscenes - 1, 0.0); }
		}
	}

	void ensure_noise() {
		int n = pw * ph;
		if (flash_noise.size() == n) { return; }
		var noise_rng = Rng(seed + 977);
		flash_noise = [];
		ring = [];
		for (int i = 0; i < n; ++i) {
			flash_noise.push(noise_rng.next(1000));
			float dx = (i % pw - pw * 0.5) / (pw * 0.5);
			float dy = (i / pw - ph * 0.5) / (ph * 0.5);
			ring.push(itrunc((dx * dx + dy * dy) * 500.0));
		}
	}

	// the cut lands on the bar line: a white flash dithered across the frame, gone within a few frames
	void cut_flash(float prog) {
		float k = 1.0 - prog / 0.14;
		if (k <= 0.0) { return; }
		dither_flash(itrunc(k * k * 1000.0));
	}

	void dither_flash(int keep) {
		ensure_noise();
		var& pix = HB_PIX;
		int shine = ui_off + ui_shine;
		var& noise = flash_noise;
		for (int i = 0; i < pw * ph; ++i) {
			if (noise[i] < keep) { pix[i] = shine; }
		}
	}

	// every big hit lands on the whole frame: over the bar before it an iris closes in,
	// tightest through the score's beat of silence, then the hit throws it open and
	// shakes the picture
	void drop_fx(var& sc, float t) {
		if (sc.cue == null) { return; }
		var& cue = sc.cue;
		float close = 0.0;
		float since = 9.0;
		for (int k = 0; k < cue.hit_t.size(); ++k) {
			if (cue.hit_level[k] < 2) { continue; }
			float to = cue.hit_t[k] - t;
			if (to > 0.0 && to <= MUSIC_BAR) {
				float c = 1.0 - to / MUSIC_BAR;
				c = to <= MUSIC_STEP * 4.0 ? 0.62 : c * c * 0.45;
				close = c > close ? c : close;
			}
			if (to <= 0.0 && 0.0 - to < since) { since = 0.0 - to; }
		}
		if (close > 0.0) {
			ensure_noise();
			var& pix = HB_PIX;
			var& noise = flash_noise;
			var& rg = ring;
			int dark = ui_off + ui_shadow;
			// past the iris everything is dark, across a thin rim it dithers in
			int edge = itrunc((1.04 - close) * 1000.0);
			int rim = 70;
			for (int i = 0; i < pw * ph; ++i) {
				int over = rg[i] - edge + rim;
				if (over > 0 && (over >= rim || noise[i] * rim < over * 1000)) { pix[i] = dark; }
			}
		}
		if (since < 0.32) {
			float amp = (0.32 - since) / 0.32 * (sext ? 4.0 : 2.5);
			int dx = itrunc(math::sin(t * 97.0) * amp);
			int dy = itrunc(math::cos(t * 71.0) * amp * 0.6);
			if (dx != 0 || dy != 0) { shift_frame(dx, dy); }
		}
	}

	// the frame moved by (dx, dy) pixels, its edges smeared in
	void shift_frame(int dx, int dy) {
		var& pix = HB_PIX;
		var& tmp = grid_b;
		int n = pw * ph;
		for (int i = 0; i < n; ++i) { tmp[i] = pix[i]; }
		int i2 = 0;
		for (int y = 0; y < ph; ++y) {
			int sy = y - dy;
			sy = sy < 0 ? 0 : (sy >= ph ? ph - 1 : sy);
			int row = sy * pw;
			for (int x = 0; x < pw; ++x) {
				int sx = x - dx;
				sx = sx < 0 ? 0 : (sx >= pw ? pw - 1 : sx);
				pix[i2] = tmp[row + sx];
				++i2;
			}
		}
	}

	// the title card: a band opens from the screen's middle, the scene name
	// types itself in big pixel letters (a shine sweeps across them), the
	// handle sits under it as crisp text, and the band folds shut again; the
	// letters take the largest scale that fits, so sextants draw them finer
	int title_pixels() {
		float open = scene_t / 0.28;
		float close = (title_len - scene_t) / 0.3;
		float k = open < close ? open : close;
		k = k > 1.0 ? 1.0 : k;
		if (k <= 0.0) { return -1; }
		string name = cur.name;
		int sx = sext ? 4 : 2;
		while (sx > 1 && gfx.text_width(name, sx) > pw - 6) { --sx; }
		int sy = sext ? sx : 2;
		int th = 7 * sy;
		int band_h = th + th * 5 / 7 + 2;
		int y0 = (ph - band_h) / 2 - ph / 24;
		if (!sext) { y0 = (y0 / 2) * 2; }
		int mid = y0 + band_h / 2;
		// three styles take turns: the band opening from the middle, a shutter closing from
		// both sides, and a glitch whose rows slip sideways on the hats
		int style = cur_idx % 3;
		int reach = style == 1 ? band_h / 2 : itrunc(k * band_h * 0.5 + 0.5);
		var& pix = HB_PIX;
		int band = ui_off + ui_band;
		int kick = cur.cue != null ? itrunc(cur.cue.kick_env(scene_t) * 2.5) : 0;
		for (int y = mid - reach; y < mid + reach; ++y) {
			if (y < 0 || y >= ph) { continue; }
			int ry = y - y0;
			int shade = ry * 12 / band_h + kick;
			int c = ry == 0 ? ui_off + ui_rule : (ry == band_h - 1 ? ui_off + ui_rule + 1 : band + (shade > 11 ? 11 : shade));
			int gi = y * pw;
			int edge_l = style == 1 ? itrunc(pw * 0.5 * k) : pw;
			for (int x = 0; x < pw; ++x) {
				if (style == 1 && x >= edge_l && x < pw - edge_l) { continue; }
				pix[gi + x] = c;
			}
		}
		if (k < 1.0) { return -1; }
		int tw = gfx.text_width(name, sx);
		int tx = (pw - tw) / 2;
		int ty = y0 + 3;
		int shown = itrunc((scene_t - 0.28) * 34.0) + 1;
		gfx.draw_text(pix, pw, ph, name, tx, ty, sx, sy, ui_off + ui_ink, 8, ui_off + ui_shadow, style == 1 ? name.size() : shown);
		if (style == 2 && cur.cue != null) { title_glitch(ty, ty + 7 * sy, cur.cue.hat_env(scene_t)); }
		// the shine: a diagonal stripe of white sweeping over the ink
		int sweep = itrunc((scene_t - 0.6) * 90.0 * sx / 2) - 20;
		int ink0 = ui_off + ui_ink;
		for (int y2 = ty; y2 < ty + th; ++y2) {
			int gi2 = y2 * pw;
			for (int x2 = tx; x2 < tx + tw; ++x2) {
				int d = x2 + (y2 - ty) - sweep - tx;
				if (d >= 0 && d < 5 * sx / 2) {
					int pv = pix[gi2 + x2];
					if (pv >= ink0 && pv < ink0 + 8) { pix[gi2 + x2] = ui_off + ui_shine; }
				}
			}
		}
		return (ty + th + 2) * h / ph + 1;
	}

	// the glitch: rows of the lettering slip sideways in slices, harder on each hat
	void title_glitch(int y0, int y1, float hit) {
		var& pix = HB_PIX;
		int step = itrunc(scene_t / MUSIC_STEP);
		int amount = itrunc(hit * pw * 0.05) + (scene_t < 0.6 ? 6 : 0);
		if (amount <= 0) { return; }
		array<int> row = [];
		for (int x = 0; x < pw; ++x) { row.push(0); }
		for (int y = y0; y < y1; ++y) {
			if (y < 0 || y >= ph) { continue; }
			int slice = (y / 3 + step * 7) % 5;
			if (slice > 1) { continue; }
			int off = slice == 0 ? amount : -amount;
			int gi = y * pw;
			for (int x2 = 0; x2 < pw; ++x2) { row[x2] = pix[gi + x2]; }
			for (int x3 = 0; x3 < pw; ++x3) {
				int from = x3 - off;
				from = from < 0 ? 0 : (from >= pw ? pw - 1 : from);
				pix[gi + x3] = row[from];
			}
		}
	}

	void title_text(var& rows, int row) {
		if (row < 0) { return; }
		int c = HB_RGB[ui_off + ui_band + 10];
		gfx.overlay_center(rows, w, row, cur.handle, sgr_bg(c / 65536, (c / 256) % 256, c % 256) + sgr_fg(170, 184, 240));
	}

	// The HUD: a status bar on top (the scene, then the frame's cost: ms_sim is the
	// script's update and frame string, ms_draw the host's console write, and the
	// headline is their sum) and a key bar below that fills as the scene plays.
	// The headless runs boot with the HUD off, so it never reaches a hash.
	void hud_overlay(var& rows, float fps, float ms_sim, float ms_draw, float ms_other) {
		string band = sgr_bg(16, 11, 34);
		var top = HudLine(w);
		top.put(sgr_bg(255, 206, 110) + sgr_fg(30, 16, 44), " JAISCRIPT DEMOREEL ");
		top.put(band + sgr_fg(120, 112, 168), cur_idx == intro_idx ? "  intro  " : `  ${cur_idx + 1 < 10 ? "0" : ""}${cur_idx + 1}/${nscenes}  `);
		top.put(band + sgr_fg(255, 226, 160), cur.name);
		var stats = hud_stats(band, fps, ms_sim, ms_draw, ms_other, true);
		if (top.used + stats.used > w) { stats = hud_stats(band, fps, ms_sim, ms_draw, ms_other, false); }
		rows[0] = top.finish(band, stats);

		string keys = " n p  next/prev    1-9 0  jump    f  freeze    h  hud    r  reload    q  quit";
		keys = keys.size() > w ? keys.substr(0, w) : keys + string::repeat(" ", w - keys.size());
		float played = scene_t / dur_cur;
		int filled = itrunc((played > 1.0 ? 1.0 : played) * w);
		rows[h - 1] = sgr_bg(52, 38, 96) + sgr_fg(214, 206, 255) + keys.substr(0, filled)
			+ band + sgr_fg(120, 112, 168) + keys.substr(filled) + RESET;
	}

	// the default HUD: two quiet chips on the bottom row, the scene on the left and the
	// frame's cost on the right (the VM's time per frame, the cores it can use, the rate)
	void perf_chip(var& rows, float fps, float ms_sim, float ms_draw) {
		string scene = cur_idx == intro_idx ? " JAISCRIPT DEMOREEL " : ` ${cur_idx + 1 < 10 ? "0" : ""}${cur_idx + 1}/${nscenes}  ${cur.name} `;
		string cost = ` JaiScript ${backend} ${gfx.fmt1(ms_sim + ms_draw)} ms . ${thread_count()} cores . ${gfx.fmt1(fps)} fps `;
		int gap = w - scene.size() - cost.size();
		if (gap < 1) { return; }
		string dim = sgr_bg(10, 8, 22);
		rows[h - 1] = dim + sgr_fg(255, 206, 110) + scene + sgr_bg(0, 0, 0) + string::repeat(" ", gap) + dim + sgr_fg(186, 180, 236) + cost + RESET;
	}

	// the frame's cost, with its split and the grid size when the row has room for them
	HudLine hud_stats(string band, float fps, float ms_sim, float ms_draw, float ms_other, bool detail) {
		var result = HudLine(w);
		string label = band + sgr_fg(120, 112, 168);
		string value = band + sgr_fg(238, 236, 255);
		if (paused) { result.put(sgr_bg(214, 52, 92) + sgr_fg(255, 250, 230), " FROZEN "); }
		result.put(value, `  ${backend} ${gfx.fmt1(ms_sim + ms_draw)} ms`);
		if (detail) { result.put(label, ` (sim ${gfx.fmt1(ms_sim)} + draw ${gfx.fmt1(ms_draw)})`); }
		if (detail && ms_other > 0.0) { result.put(label, ` other ${gfx.fmt1(ms_other)} ms`); }
		result.put(value, `  ${gfx.fmt1(fps)}`);
		result.put(label, detail ? ` fps  ${pw}x${ph} px ${sext ? "sextants " : ""}` : " fps ");
		return result;
	}

	string frame(float t, float dt, string key, float fps, float ms_sim, float ms_draw, float ms_other) {
		if (finish_pending) { complete_transition(); }
		handle_key(key);
		hb_ensure(w, h, sext);
		txt_clear();
		if (HB_RGB.size() == 0) { bind_palettes(); }
		float step = paused ? 0.0 : dt;
		scene_t += step;
		if (!in_trans && scene_t >= dur_cur) { begin_transition(); }
		cur.update(scene_t, step);
		if (in_trans) {
			trans_t += step;
			// two scenes draw through a cut: the departing one draws every other frame (the
			// transition's own motion carries it), so the heaviest pairs hold the frame rate
			if (trans_frame % 2 == 0) { render_scene(cur, grid_a, small_a); }
			++trans_frame;
			nxt.update(trans_t, step);
			render_scene(nxt, grid_b, small_b);
			float prog = trans_t / trans_len;
			prog = prog > 1.0 ? 1.0 : prog;
			gfx.mix_grids(grid_a, grid_b, HB_PIX, pw, ph, trans_kind, prog, off_b, ui_off + ui_edge, melt);
			cut_flash(prog);
			if (trans_t >= trans_len) { finish_pending = true; }
		} else {
			render_scene(cur, HB_PIX, small_a);
			drop_fx(cur, scene_t);
		}
		int trow = -1;
		if (!in_trans && !finish_pending && scene_t < title_len && cur_idx != intro_idx) { trow = title_pixels(); }
		var rows = hb_present();
		title_text(rows, trow);
		if (!in_trans) { cur.text_rows(rows); }
		if (hud) {
			if (hud_full) { hud_overlay(rows, fps, ms_sim, ms_draw, ms_other); }
			else { perf_chip(rows, fps, ms_sim, ms_draw); }
		}
		if (reload_left > 0.0) {
			reload_left -= dt;
			reload_overlay(rows);
		}
		return rows.join("\n");
	}
}
