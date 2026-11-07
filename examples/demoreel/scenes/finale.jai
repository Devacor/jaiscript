// FINALE — the word JAISCRIPT ignites out of a wall of fire and burns in
// rolling rainbow flame; then the fire dies, the letters cool to a neon
// rainbow and the credits roll up beneath them. It IS the fire scene (a
// subclass): the letters are heat sources stamped from the pixel font, fed to
// the same convection pass, with sparks thrown while the wall roars.

class Finale : Fire {
	array<int> letter_at = [];   // the letters' pixels
	array<int> letter_of = [];   // and which letter each belongs to
	array<int> rim_at = [];      // the pixels around the letters, drawn dark
	array<int> fuel = [];        // per column: the heat the letters feed this frame
	int word_x = 0;
	int word_w = 0;
	int word_top = 0;
	int word_rows = 0;
	float ignite_t = 2.2;
	float rainbow_t = 4.5;
	float wall_out_t = 10.5;     // the wall starts dying
	float embers_t = 11.5;       // the letters stop feeding the flames
	float neon_t = 14.0;         // ... and have cooled to neon
	float credits_t = 13.0;
	float credit_rows_per_s = 5.0;

	// [kind, left column, right column]: a "role" row splits at the gutter
	var credits = [
		["title", "THE JAISCRIPT DEMOREEL", ""],
		["", "", ""],
		["body", "seventeen scenes . five transitions", ""],
		["body", "every pixel computed by the jaiscript vm", ""],
		["", "", ""],
		["role", "PLASMA", "four sine fields"],
		["role", "STARFIELD", "a warp run through a nebula"],
		["role", "TUNNEL", "a table-driven texture tunnel"],
		["role", "DONUT", "a sphere-traced torus"],
		["role", "JULIA", "a morphing julia set"],
		["role", "LAVALAMP", "lit metaballs"],
		["role", "PIPEDREAM", "the 3d pipes screensaver"],
		["role", "POWDER", "sand, water, ember and plant"],
		["role", "BOIDS", "a murmuration and a hawk"],
		["role", "KINSTEIN 3D", "a textured raycaster"],
		["role", "CANYONRUN", "voxelspace terrain"],
		["role", "SPONGEWORKS", "a raymarched menger sponge"],
		["role", "BANNERFALL", "verlet cloth in a gale"],
		["role", "CHROME", "a ray tracer"],
		["role", "FIRE", "the doom fire, in rainbow"],
		["role", "OSSUARY", "a skull spider on a morphing gyroid"],
		["role", "FINALE", "you are here"],
		["", "", ""],
		["body", "half-block pixels, two to a cell", ""],
		["body", "deterministic to the last byte", ""],
		["", "", ""],
		["", "", ""],
		["title", "thanks for watching", ""],
		["link", "jaiscript.com", ""]
	];

	void build() override {
		name = "FINALE";
		handle = "jaiscript forever . rainbow-fire salute";
		build_fire();
		fuel = gfx.make_grid(w);
		stamp_word("JAISCRIPT");
	}

	// the 5x7 font at double size, its second column kept single so a glyph is
	// 9 px wide on a 10 px pitch; narrow screens get it at single size
	void stamp_word(string text) {
		bool big = w >= 95;
		array<int> font_col = [0, 1, 2, 3, 4];
		if (big) { font_col = [0, 0, 1, 2, 2, 3, 3, 4, 4]; }
		int sy = big ? 2 : 1;
		int pitch = font_col.size() + 1;
		word_w = text.size() * pitch - 1;
		word_x = (w - word_w) / 2;
		word_rows = 7 * sy;
		word_top = h / 2 - word_rows - 2;
		array<int> mask = gfx.make_grid(w * h);
		for (int li = 0; li < text.size(); ++li) {
			var bits = FONT5[text.substr(li, 1)];
			for (int ry = 0; ry < word_rows; ++ry) {
				int b = bits[ry / sy];
				for (int rx = 0; rx < font_col.size(); ++rx) {
					if (((b >> (4 - font_col[rx])) & 1) == 1) {
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

	void update(float t, float dt) override {
		tt = t;
		++fr;
		rainbow_on = t > rainbow_t ? 1 : 0;
		if (rainbow_on == 1) { hue_t += dt * 3.0; }
		int amp = t > wall_out_t ? itrunc(42.0 - (t - wall_out_t) * 19.0) : 42;
		amp = amp < 0 ? 0 : amp;
		seed_rows(amp, amp / 2 + 2);
		convect();
		if (t > ignite_t) { burn_letters(t); }
		move_sparks(dt, amp > 12);
	}

	// each column's fuel swells and ebbs where two waves cross, and the air
	// above the word cools fast, so the flames rise as separate tongues; once
	// the letters have cooled, the last embers anywhere go out the same way
	void burn_letters(float t) {
		float strength = t < embers_t ? 1.0 : (t > neon_t ? 0.0 : (neon_t - t) / (neon_t - embers_t));
		for (int x = 0; x < w; ++x) {
			float wave = math::sin(x * 0.61 + t * 6.1) + math::sin(x * 0.23 - t * 3.7 + 1.7);
			fuel[x] = itrunc((46.0 + wave * 15.0) * strength);
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
		int quenched = t > neon_t ? w * h : word_top * w;
		for (int i = 0; i < quenched; ++i) {
			int v = ht[i] - 2;
			ht[i] = v > 0 ? v : 0;
		}
	}

	// the letters burn white-hot over their own flames, ringed by a dark rim;
	// once the fire is out each cools to its own hue of the rainbow, and a
	// glint sweeps across the word every few seconds
	void render(var& grid) override {
		int b = bank();
		int n = w * h;
		var& ht = heat;
		for (int i = 0; i < n; ++i) {
			int v = ht[i];
			grid[i] = (v > 72 ? 72 : v) + b;
		}
		if (tt > ignite_t) {
			var& rim = rim_at;
			for (int k = 0; k < rim.size(); ++k) {
				int i = rim[k];
				grid[i] = b + ht[i] / 3;
			}
			draw_letters(grid, b);
		}
		draw_sparks(grid, b);
	}

	void draw_letters(var& grid, int b) {
		var& at = letter_at;
		int n = at.size();
		if (tt < embers_t) {
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
		int top = (word_top + word_rows) / 2 + 2;
		int bottom = h / 2 - 2;
		if (bottom - top < 2) { return; }
		int last = credits.size() - 1;
		int rest = bottom + last - (top + bottom) / 2;
		int scroll = itrunc((tt - credits_t) * credit_rows_per_s);
		scroll = scroll > rest ? rest : scroll;
		for (int k = 0; k <= last; ++k) {
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
		if (kind == "role") {
			int gutter = w / 2;
			left = left.size() > gutter - 2 ? left.substr(0, gutter - 2) : left;
			right = right.size() > w - gutter - 1 ? right.substr(0, w - gutter - 1) : right;
			result += string::repeat(" ", gutter - 1 - left.size()) + credit_ink(255, 206, 110, dim) + left
				+ "  " + credit_ink(186, 180, 236, dim) + right + string::repeat(" ", w - gutter - 1 - right.size());
		} else {
			left = left.size() > w ? left.substr(0, w) : left;
			int pad = (w - left.size()) / 2;
			string ink = credit_ink(214, 208, 255, dim);
			if (kind == "title") { ink = credit_ink(255, 236, 190, dim); }
			if (kind == "link") { ink = credit_ink(120, 200, 255, dim); }
			result += string::repeat(" ", pad) + ink + left + string::repeat(" ", w - pad - left.size());
		}
		return result + RESET;
	}

	string credit_ink(int r, int g, int b, int dim) {
		return sgr_fg(r * (3 - dim) / 3, g * (3 - dim) / 3, b * (3 - dim) / 3);
	}
}
