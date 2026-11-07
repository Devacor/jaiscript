// Scene interface + the Reel manager: timed auto-advance, transitions, title
// cards, HUD. Everything the host calls funnels through Reel.frame().
// Scenes see h as PIXEL rows (two per terminal row) and paint square pixels.

class Scene {
	string name = "SCENE";
	string handle = "";
	int w = 0;
	int h = 0;
	int seed = 0;
	var pal = null;
	var rng = null;

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
	int w = 0;
	int h = 0;           // terminal rows
	int ph = 0;          // pixel rows
	int seed = 0;
	bool smoke = false;
	string backend = "";
	bool hud = true;
	bool paused = false;

	int nscenes = 17;
	var durs = [];
	var cur = null;
	var nxt = null;
	int cur_idx = 0;
	int pending_idx = 0;
	float scene_t = 0.0;
	float dur_cur = 10.0;
	float title_len = 2.4;
	float trans_len = 1.1;
	bool in_trans = false;
	bool finish_pending = false;
	float trans_t = 0.0;
	int trans_kind = 0;
	int trans_count = 0;

	array<int> grid_a = [];
	array<int> grid_b = [];
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

	Reel(int w_, int h_, int seed_, string backend_, bool smoke_, bool hud_) {
		w = w_;
		h = h_;
		ph = h_ * 2;
		seed = seed_;
		backend = backend_;
		smoke = smoke_;
		hud = hud_;
		grid_a = gfx.make_grid(w * ph);
		grid_b = gfx.make_grid(w * ph);
		melt = gfx.make_fgrid(w);
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
		durs = [12.0, 11.0, 12.0, 11.0, 11.0, 12.0, 14.0, 14.0, 12.0, 15.0, 15.0, 16.0, 16.5, 12.0, 11.0, 20.0, 24.0];
		start_scene(0, 0.0);
	}

	var make_scene(int idx) {
		switch (idx) {
			case 0: return Plasma();
			case 1: return Starfield();
			case 2: return Tunnel();
			case 3: return Donut();
			case 4: return Julia();
			case 5: return Lava();
			case 6: return Pipes();
			case 7: return SandBox();
			case 8: return Boids();
			case 9: return Kinstein();
			case 10: return CanyonRun();
			case 11: return SpongeWorks();
			case 12: return BannerFall();
			case 13: return Chrome();
			case 14: return Fire();
			case 15: return Ossuary();
			default: return Finale();
		}
	}

	float dur_for(int idx) {
		return smoke ? 3.0 : durs[idx];
	}

	// the presenter's string tables: current scene, incoming scene (during a
	// transition), then the reel's own UI entries
	void bind_palettes() {
		hb_ensure(w, h);
		HB_FG = [];
		HB_BG = [];
		HB_RGB = [];
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
			HB_RGB.push(p.rgb[i]);
		}
	}

	void start_scene(int idx, float at_t) {
		cur_idx = ((idx % nscenes) + nscenes) % nscenes;
		cur = make_scene(cur_idx);
		cur.setup(w, ph, seed + cur_idx * 7919);
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
		pending_idx = (cur_idx + 1) % nscenes;
		nxt = make_scene(pending_idx);
		nxt.setup(w, ph, seed + pending_idx * 7919);
		in_trans = true;
		trans_t = 0.0;
		trans_kind = (trans_count + seed) % 5;
		++trans_count;
		int m = 0;
		for (int x = 0; x < w; ++x) {
			m += ((x * 7 + trans_count * 13 + (x * x) % 11) % 3) - 1;
			m = m < 0 ? 0 : (m > 14 ? 14 : m);
			melt[x] = m * 1.0;
		}
		bind_palettes();
	}

	// backend swap lands here: same scene, same clock; stateful scenes replay
	// up to 3 sim-seconds so they arrive mid-flight instead of empty.
	void jump(int idx, float at_t) {
		start_scene(idx, at_t);
		int steps = itrunc(at_t * 30.0);
		steps = steps > 90 ? 90 : steps;
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

	void handle_key(string key) {
		if (key == "") { return; }
		if (key == "h") { hud = !hud; }
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

	// the title card: a band opens from the screen's middle, the scene name
	// types itself in big pixel letters (a shine sweeps across them), the
	// handle sits under it as crisp text, and the band folds shut again
	int title_pixels() {
		float open = scene_t / 0.28;
		float close = (title_len - scene_t) / 0.3;
		float k = open < close ? open : close;
		k = k > 1.0 ? 1.0 : k;
		if (k <= 0.0) { return -1; }
		int y0 = ((ph / 2 - 12) / 2) * 2;
		int mid = y0 + 12;
		int reach = itrunc(k * 12.0 + 0.5);
		var& pix = HB_PIX;
		int band = ui_off + ui_band;
		for (int y = mid - reach; y < mid + reach; ++y) {
			if (y < 0 || y >= ph) { continue; }
			int ry = y - y0;
			int c = ry == 0 ? ui_off + ui_rule : (ry == 23 ? ui_off + ui_rule + 1 : band + ry / 2);
			int gi = y * w;
			for (int x = 0; x < w; ++x) { pix[gi + x] = c; }
		}
		if (k < 1.0) { return -1; }
		string name = cur.name;
		int sx = gfx.text_width(name, 2) <= w - 6 ? 2 : 1;
		int tw = gfx.text_width(name, sx);
		int tx = (w - tw) / 2;
		int ty = y0 + 3;
		int shown = itrunc((scene_t - 0.28) * 34.0) + 1;
		gfx.draw_text(pix, w, ph, name, tx, ty, sx, 2, ui_off + ui_ink, 8, ui_off + ui_shadow, shown);
		// the shine: a diagonal stripe of white sweeping over the ink
		int sweep = itrunc((scene_t - 0.6) * 90.0) - 20;
		int ink0 = ui_off + ui_ink;
		for (int y2 = ty; y2 < ty + 14; ++y2) {
			int gi2 = y2 * w;
			for (int x2 = tx; x2 < tx + tw; ++x2) {
				int d = x2 + (y2 - ty) - sweep - tx;
				if (d >= 0 && d < 5) {
					int pv = pix[gi2 + x2];
					if (pv >= ink0 && pv < ink0 + 8) { pix[gi2 + x2] = ui_off + ui_shine; }
				}
			}
		}
		return (y0 + 20) / 2;
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
		top.put(band + sgr_fg(120, 112, 168), `  ${cur_idx + 1 < 10 ? "0" : ""}${cur_idx + 1}/${nscenes}  `);
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
		result.put(label, detail ? ` fps  ${w}x${ph} px ` : " fps ");
		return result;
	}

	string frame(float t, float dt, string key, float fps, float ms_sim, float ms_draw, float ms_other) {
		if (finish_pending) { complete_transition(); }
		handle_key(key);
		hb_ensure(w, h);
		if (HB_RGB.size() == 0) { bind_palettes(); }
		float step = paused ? 0.0 : dt;
		scene_t += step;
		if (!in_trans && scene_t >= dur_cur) { begin_transition(); }
		cur.update(scene_t, step);
		if (in_trans) {
			trans_t += step;
			cur.render(grid_a);
			nxt.update(trans_t, step);
			nxt.render(grid_b);
			float prog = trans_t / trans_len;
			prog = prog > 1.0 ? 1.0 : prog;
			gfx.mix_grids(grid_a, grid_b, HB_PIX, w, ph, trans_kind, prog, off_b, ui_off + ui_edge, melt);
			if (trans_t >= trans_len) { finish_pending = true; }
		} else {
			cur.render(HB_PIX);
		}
		int trow = -1;
		if (!in_trans && !finish_pending && scene_t < title_len) { trow = title_pixels(); }
		var rows = hb_present();
		title_text(rows, trow);
		if (!in_trans) { cur.text_rows(rows); }
		if (hud) { hud_overlay(rows, fps, ms_sim, ms_draw, ms_other); }
		return rows.join("\n");
	}
}
