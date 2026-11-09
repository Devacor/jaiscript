// The showcase look's feel: what the game does on screen when something
// happens. Feel watches the game from outside, frame by frame: it compares what
// it sees with what it saw (a shot spent, an enemy flinching, ammo gone, a door
// open) and answers with light sources, screen shake, view bob, decals on the
// floor, hit markers, screen tints, the iris and title card that open a floor,
// and sound cues. It reads the simulation and never writes to it, so the
// reference frames and STATE_HASH never notice it.

array<string> GLOOM_CUES;   // this frame's sound cues; the host plays them, if it has sound

// a light that outlives the particle that made it: a blast's afterglow
class Blast {
public:
	float x = 0.0;
	float y = 0.0;
	int channel = 0;
	int age = 0;

	Blast(float x_, float y_, int channel_) {
		x = x_;
		y = y_;
		channel = channel_;
	}
}

class Feel {
public:
	float yaw = 0.0;                     // shake: a turn of the view, radians
	int pitch = 0;                       // bob and shake: pixel rows the horizon moves
	array<LightSource> sources = [];     // this frame's lights over the bake

	// once per frame, before the view draws
	void frame() {
		GLOOM_CUES.clear();
		int ticks = G.tick - last_tick;
		last_tick = G.tick;
		if (ticks < 0) { ticks = 0; }
		watch_flow();
		if (G.mode != 1) { return; }
		if (G.map_ticks < last_map_ticks || G.enemies.size() != e_alive.size()) { new_floor(); }
		last_map_ticks = G.map_ticks;
		for (int i = 0; i < ticks; ++i) { age_one_tick(); }
		watch_player(ticks);
		watch_enemies();
		watch_shots();
		watch_world();
		stain_landed();
		G.renderer.dry_stains(G.tick);
		gather_lights();
		camera();
	}

	// a --dev reload changed the look: tints and the title card rebuild on their next use
	void forget_colors() {
		remaps = [];
		card_shades = [];
		mask_w = 0;
	}

	// after the view, the gun and the particles: tints, the vignette, the hit marker, the floor's opening
	void overlay(int vw, int vh) {
		if (mask_w != vw || mask_h != vh) { build_mask(vw, vh); }
		int base = 0;
		int tint = -1;
		if (blast_t > 0) { tint = 4; base = imin(3, blast_t); }
		else if (hurt_t > 0) { tint = 1; base = imin(3, hurt_t / 2 + 1); }
		else if (pickup_t > 0) { tint = 2; base = imin(2, pickup_t / 2); }
		else if (secret_t > 0) { tint = 3; base = imin(2, secret_t / 3); }
		else if (G.player.hp <= 25 && (G.tick % 24) < 5) { tint = 1; base = 1; }
		shade_pass(0, 0);
		if (tint >= 0) { shade_pass(tint, base); }
		draw_hit_marker(vw);
		opening(vw, vh);
	}

private:
	int last_tick = 0;
	int last_mode = -1;
	int last_map_ticks = 0;
	int last_hp = 100;
	int last_armor = 0;
	array<int> last_ammo = [48, 0, 0];
	array<int> last_have = [1, 0, 0];
	bool last_key_r = false;
	bool last_key_b = false;
	int last_secrets = 0;
	int last_taken = 0;
	int last_mapgen = 0;
	float last_bob = 0.0;
	float bob_amp = 0.0;
	array<int> e_flash = [];
	array<bool> e_alive = [];
	array<int> e_state = [];
	array<string> e_art = [];
	array<int> shot_kind = [];
	array<float> shot_x = [];
	array<float> shot_y = [];
	array<Blast> blasts = [];
	float trauma = 0.0;
	int hurt_t = 0;
	int pickup_t = 0;
	int secret_t = 0;
	int blast_t = 0;
	int hit_t = 0;
	bool hit_kill = false;
	int mask_w = 0;
	int mask_h = 0;
	array<int> mask_px = [];          // vignette: the pixels it darkens, and how much (0..4)
	array<int> mask_level = [];
	array<int> mask_all = [];         // every pixel's level, for the full-screen tints
	array<array<int>> remaps = [];    // [tint * 5 + level]: palette index -> tinted index, -1 not yet made
	array<int> card_shades = [];

	void cue(string name) {
		if (GLOOM_CUES.size() < 8 && !GLOOM_CUES.contains(name)) { GLOOM_CUES.push(name); }
	}

	void kick(float amount) { trauma = fclamp(trauma + amount, 0.0, 1.0); }

	// --------------------------------------------------------------- watching
	void watch_flow() {
		if (G.mode != last_mode) {
			if (G.mode == 1 && last_mode >= 0) { cue("start"); }
			if (G.mode == 2) { cue("clear"); }
			if (G.mode == 3) { cue("death"); }
			if (G.mode == 4) { cue("clear"); }
			last_mode = G.mode;
		}
	}

	void new_floor() {
		e_flash = [];
		e_alive = [];
		e_state = [];
		e_art = [];
		for (auto& e : G.enemies) {
			e_flash.push(e.flash);
			e_alive.push(e.alive);
			e_state.push(e.state);
			e_art.push(e.art_name());
		}
		shot_kind = [];
		shot_x = [];
		shot_y = [];
		blasts = [];
		last_taken = taken_count();
		last_secrets = G.secrets;
		last_mapgen = G.world.mapgen;
		last_hp = G.player.hp;
		last_armor = G.player.armor;
		last_ammo = G.player.ammo;
		last_have = G.player.have;
		card_shades = [];
		trauma = 0.0;
		hurt_t = 0;
		pickup_t = 0;
		secret_t = 0;
		blast_t = 0;
		hit_t = 0;
	}

	void age_one_tick() {
		trauma = trauma * 0.86;
		if (trauma < 0.01) { trauma = 0.0; }
		if (hurt_t > 0) { hurt_t = hurt_t - 1; }
		if (pickup_t > 0) { pickup_t = pickup_t - 1; }
		if (secret_t > 0) { secret_t = secret_t - 1; }
		if (blast_t > 0) { blast_t = blast_t - 1; }
		if (hit_t > 0) { hit_t = hit_t - 1; }
		array<Blast> kept = [];
		for (auto& b : blasts) {
			b.age = b.age + 1;
			if (b.age < 14) { kept.push(b); }
		}
		blasts = kept;
	}

	// everything that fades does so per tick, so the feel is the same at any frame rate
	void watch_player(int ticks) {
		auto& player = G.player;
		int lost = (last_hp - player.hp) + (last_armor - player.armor);
		if (player.hp < last_hp) {
			hurt_t = 8;
			kick(0.25 + lost / 30.0);
			cue(lost >= 15 ? "pain_big" : "pain");
		}
		for (int pool = 0; pool < 3; ++pool) {
			if (player.ammo[pool] < last_ammo[pool]) {
				int weapon = player.weapon;
				if (weapon == 0) { kick(0.10); cue("pistol"); }
				else if (weapon == 1) { kick(0.42); cue("scatter"); }
				else { kick(0.18); cue("hexcast"); }
			} else if (player.ammo[pool] > last_ammo[pool]) {
				pickup_t = 5;
				cue("pickup");
			}
		}
		for (int slot = 0; slot < 3; ++slot) {
			if (player.have[slot] > last_have[slot]) { cue("weapon"); }
		}
		if ((player.key_r && !last_key_r) || (player.key_b && !last_key_b)) {
			pickup_t = 6;
			cue("key");
		}
		if (player.hp > last_hp || player.armor > last_armor) {
			pickup_t = 5;
			cue("pickup");
		}
		last_hp = player.hp;
		last_armor = player.armor;
		last_ammo = player.ammo;
		last_have = player.have;
		last_key_r = player.key_r;
		last_key_b = player.key_b;
		float moved = player.bob - last_bob;
		last_bob = player.bob;
		for (int i = 0; i < ticks; ++i) { bob_amp = moved > 0.0 ? fclamp(bob_amp + 0.25, 0.0, 1.0) : bob_amp * 0.8; }
	}

	void watch_enemies() {
		for (int i = 0; i < G.enemies.size(); ++i) {
			auto& e = G.enemies[i];
			if (e.flash > e_flash[i] && e_alive[i]) {
				hit_t = 6;
				hit_kill = false;
				cue("thud");
			}
			if (e_alive[i] && !e.alive) {
				hit_t = 9;
				hit_kill = true;
				kick(e.kind == 3 ? 0.6 : 0.12);
				G.renderer.stain_floor(e.x, e.y, e.kind == 2 ? 0.0 : 0.34, TONE_BLOOD, 170);
				cue(e.kind == 2 ? "scrap" : "splat");
			}
			if (e_state[i] == EnemyState.dormant && e.state != EnemyState.dormant && e.alive) {
				array<string> alerts = ["roar", "hiss", "whir", "bellow"];
				cue(alerts[e.kind]);
			}
			string art = e.art_name();
			if (art == "tur_fire" && e_art[i] != "tur_fire") { cue("burst"); }
			e_flash[i] = e.flash;
			e_alive[i] = e.alive;
			e_state[i] = e.state;
			e_art[i] = art;
		}
	}

	// a shot gone since the last frame burst somewhere near where it was; a new one was just fired
	void watch_shots() {
		array<bool> matched = [];
		for (int j = 0; j < G.shots.size(); ++j) { matched.push(false); }
		for (int i = 0; i < shot_kind.size(); ++i) {
			bool found = false;
			for (int j = 0; j < G.shots.size(); ++j) {
				auto& s = G.shots[j];
				if (!matched[j] && s.kind == shot_kind[i] && dist2(s.x, s.y, shot_x[i], shot_y[i]) < 1.2) {
					matched[j] = true;
					found = true;
					break;
				}
			}
			if (found) { continue; }
			int kind = shot_kind[i];
			if (kind == 1) {
				cue("splat");
				continue;
			}
			blasts.push(Blast(shot_x[i], shot_y[i], kind == 3 ? LIGHT_HEX : LIGHT_FIRE));
			G.renderer.stain_floor(shot_x[i], shot_y[i], 0.55, TONE_SCORCH, 190);
			G.renderer.stain_floor(shot_x[i], shot_y[i], 0.8, TONE_ASH, 40);
			float d = math::sqrt(dist2(shot_x[i], shot_y[i], G.player.px, G.player.py));
			if (d < 6.0) {
				kick(0.75 * (1.0 - d / 6.0) + 0.1);
				blast_t = imax(blast_t, d < 3.0 ? 3 : 2);
			}
			cue("boom");
		}
		for (int j = 0; j < G.shots.size(); ++j) {
			if (!matched[j] && G.shots[j].kind != 3) { cue(G.shots[j].kind == 1 ? "spit" : "fireball"); }
		}
		shot_kind = [];
		shot_x = [];
		shot_y = [];
		for (auto& s : G.shots) {
			shot_kind.push(s.kind);
			shot_x.push(s.x);
			shot_y.push(s.y);
		}
	}

	void watch_world() {
		if (G.secrets > last_secrets) {
			secret_t = 9;
			cue("secret");
		} else if (G.world.mapgen != last_mapgen) {
			cue("door");
		}
		last_secrets = G.secrets;
		last_mapgen = G.world.mapgen;
		int taken = taken_count();
		last_taken = taken;
	}

	int taken_count() {
		int n = 0;
		for (auto& it : G.items) {
			if (it.taken) { n = n + 1; }
		}
		return n;
	}

	// blood and meat that reached the floor leave their mark
	void stain_landed() {
		for (auto& p : G.fx.pool) {
			if ((p.kind == 2 || p.kind == 3) && p.z <= 0.021) {
				G.renderer.stain_floor(p.x, p.y, p.kind == 3 ? 0.09 : 0.06, TONE_BLOOD, 230);
			}
		}
	}

	// ----------------------------------------------------------------- light
	void gather_lights() {
		sources = [];
		auto& player = G.player;
		if (player.muzzle > 0) {
			float dx = math::cos(player.pang);
			float dy = math::sin(player.pang);
			int channel = player.weapon == 2 ? LIGHT_HEX : LIGHT_FIRE;
			sources.push(LightSource(player.px + dx * 0.6, player.py + dy * 0.6, channel,
				MUZZLE_RADIUS * (0.55 + 0.15 * player.muzzle), 1.0));
		}
		array<int> shot_light = [0, LIGHT_ACID, LIGHT_FIRE, LIGHT_HEX];
		for (auto& s : G.shots) { sources.push(LightSource(s.x, s.y, shot_light[s.kind], SHOT_RADIUS, 0.9)); }
		for (auto& b : blasts) {
			float t = 1.0 - b.age / 14.0;
			sources.push(LightSource(b.x, b.y, b.channel, BLAST_RADIUS * (0.4 + 0.6 * t), 0.35 + 0.65 * t));
		}
	}

	// ---------------------------------------------------------------- camera
	// shake is trauma squared, so small knocks barely move the view and big ones rattle it
	void camera() {
		float shake = trauma * trauma * SHAKE;
		float n1 = (mix32(G.tick, 101) % 2001) / 1000.0 - 1.0;
		float n2 = (mix32(G.tick, 202) % 2001) / 1000.0 - 1.0;
		yaw = shake * 0.045 * n1;
		float bob = (math::abs(math::cos(G.player.bob * 0.5)) - 0.5) * 2.0 * BOB_PIXELS * bob_amp * PIXH / 2.0;
		pitch = math::itrunc(bob + shake * 5.0 * PIXH / 2.0 * n2);
	}

	// ------------------------------------------------------------- overlays
	void build_mask(int vw, int vh) {
		mask_w = vw;
		mask_h = vh;
		mask_px = [];
		mask_level = [];
		mask_all = [];
		float cx = vw / 2.0;
		float cy = vh / 2.0;
		for (int y = 0; y < vh; ++y) {
			for (int x = 0; x < vw; ++x) {
				float dx = (x + 0.5 - cx) / cx;
				float dy = (y + 0.5 - cy) / cy;
				float e = dx * dx * 0.8 + dy * dy;
				int level = iclamp(math::itrunc((e - 0.5) * 3.2 + BAYER4[(y & 3) * 4 + (x & 3)]), 0, 4);
				mask_all.push(level);
				if (level > 0) {
					mask_px.push(y * vw + x);
					mask_level.push(level);
				}
			}
		}
	}

	// tint 0 darkens the corners; the others wash the screen from the edges in: 1 hurt, 2 pickup, 3 secret, 4 blast
	void shade_pass(int tint, int base) {
		while (remaps.size() < 25) { remaps.push([]); }
		int pal = PAL_RGB.size();
		for (int level = 1; level <= 4; ++level) {
			auto& table = remaps[tint * 5 + level];
			while (table.size() < pal) { table.push(-1); }
		}
		auto& pix = PIX;
		if (tint == 0) {
			if (VIGNETTE <= 0.0) { return; }
			for (int k = 0; k < mask_px.size(); ++k) {
				int i = mask_px[k];
				pix[i] = remapped(0, mask_level[k], pix[i]);
			}
			return;
		}
		for (int i = 0; i < pix.size(); ++i) {
			int level = imin(4, mask_all[i] + base);
			if (level > 0) { pix[i] = remapped(tint, level, pix[i]); }
		}
	}

	int remapped(int tint, int level, int p) {
		auto& table = remaps[tint * 5 + level];
		int m = table[p];
		if (m >= 0) { return m; }
		int key = PAL_RGB[p];
		float r = (key >> 16) & 255;
		float g = (key >> 8) & 255;
		float b = key & 255;
		float a = level / 4.0;
		if (tint == 0) {
			float f = 1.0 - VIGNETTE * a;
			m = rgb_idx(math::itrunc(r * f), math::itrunc(g * f), math::itrunc(b * f));
		} else if (tint == 1) {
			m = rgb_idx(math::itrunc(r + (255.0 - r) * a * 0.55), math::itrunc(g * (1.0 - a * 0.65)), math::itrunc(b * (1.0 - a * 0.65)));
		} else {
			array<array<float>> toward = [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0], [255.0, 222.0, 120.0], [120.0, 255.0, 230.0],
				[255.0, 236.0, 200.0]];
			float k = tint == 4 ? a * 0.55 : a * 0.32;
			m = rgb_idx(math::itrunc(r + (toward[tint][0] - r) * k), math::itrunc(g + (toward[tint][1] - g) * k),
				math::itrunc(b + (toward[tint][2] - b) * k));
		}
		table[p] = m;
		return m;
	}

	// four ticks around the aim point: white on a hit, red and wider on a kill
	void draw_hit_marker(int vw) {
		if (hit_t <= 0 || G.renderer.automap) { return; }
		int color = hit_kill ? rgb_idx(255, 64, 48) : rgb_idx(255, 250, 236);
		int cx = vw / 2;
		int cy = G.renderer.horizon;
		int gap = (hit_kill ? 3 : 2) * PIXW;
		int len = 2 * PIXW;
		auto& pix = PIX;
		int vh = G.renderer.view_h;
		for (int k = 0; k < len; ++k) {
			int ox = (gap + k);
			int oy = (gap + k) * PIXH / PIXW / 2 + 1;
			for (int s = 0; s < 4; ++s) {
				int x = cx + ((s & 1) == 0 ? ox : -ox);
				int y = cy + ((s & 2) == 0 ? oy : -oy);
				if (x >= 0 && x < vw && y >= 0 && y < vh) { pix[y * vw + x] = color; }
			}
		}
	}

	// a floor opens with an iris from the center, then its name, which fades into the dark
	void opening(int vw, int vh) {
		int t = G.map_ticks;
		if (t >= 80) { return; }
		auto& pix = PIX;
		if (t < 16) {
			// in cell widths: a pixel is 1 / PIXW of a cell across and 2 / PIXH down
			float cx = vw / 2.0;
			float cy = vh / 2.0;
			float sx = 1.0 / PIXW;
			float sy = 2.0 / PIXH;
			float r = (t / 16.0) * math::sqrt(cx * sx * cx * sx + cy * sy * cy * sy) * 1.02;
			float r2 = r * r;
			for (int y = 0; y < vh; ++y) {
				float dy = (y + 0.5 - cy) * sy;
				int row = y * vw;
				for (int x = 0; x < vw; ++x) {
					float dx = (x + 0.5 - cx) * sx;
					if (dx * dx + dy * dy > r2) { pix[row + x] = 0; }
				}
			}
		}
		if (t < 6 || G.renderer.automap) { return; }
		if (card_shades.size() == 0) { build_card_shades(); }
		int fade = t < 60 ? 0 : (t - 60) / 4;
		if (fade >= 5) { return; }
		string name = G.world.map_name;
		string code = name.substr(0, 4);
		string title = name.substr(6, name.size() - 6);
		// a font pixel spans sx view pixels across and sy down, so letters stay square on 1:2 cells
		int sx = imax(1, PIXW * 2 / PIXH);
		int scale = 3;
		while (scale > 1 && lettering_width(title, scale) * sx > vw - 8) { scale = scale - 1; }
		int top = vh / 5;
		pix_lettering(code, (vw - lettering_width(code, 1) * sx) / 2, top, 1, sx, card_shades[fade + 5], card_shades[10]);
		pix_lettering(title, (vw - lettering_width(title, scale) * sx) / 2, top + 8, scale, sx, card_shades[fade],
			card_shades[10]);
	}

	void build_card_shades() {
		auto& mood = MOODS[G.map_i];
		math::color bone = rgb(250, 240, 220);
		card_shades = [];
		for (int i = 0; i < 5; ++i) { card_shades.push(color_idx(math::mix_rgb(bone, mood.fog, i / 5.0))); }
		for (int i = 0; i < 5; ++i) { card_shades.push(color_idx(math::mix_rgb(mood.light, mood.fog, i / 5.0))); }
		card_shades.push(rgb_idx(4, 2, 4));
	}

	// the 3x5 font straight into the view's pixels, a font pixel scale rows tall and scale * sx columns wide
	void pix_lettering(string s, int x, int y, int scale, int sx, int color, int shadow) {
		auto& pix = PIX;
		int vw = G.renderer.view_w;
		int vh = G.renderer.view_h;
		int sy = scale;
		int cw = sx * scale;
		for (int pass = 0; pass < 2; ++pass) {
			int ink = pass == 0 ? shadow : color;
			int off = pass == 0 ? 1 : 0;
			for (int i = 0; i < s.size(); ++i) {
				string ch = s.substr(i, 1);
				if (!FONT5.has(ch)) { continue; }
				auto& glyph_rows = FONT5[ch];
				int gx = x + i * 4 * cw;
				for (int ry = 0; ry < 5; ++ry) {
					for (int rx = 0; rx < 3; ++rx) {
						if (glyph_rows[ry].substr(rx, 1) != "#") { continue; }
						for (int yy = 0; yy < sy; ++yy) {
							int py = y + (ry + off) * sy + yy;
							if (py < 0 || py >= vh) { continue; }
							for (int xx = 0; xx < cw; ++xx) {
								int qx = gx + (rx + off) * cw + xx;
								if (qx >= 0 && qx < vw) { pix[py * vw + qx] = ink; }
							}
						}
					}
				}
			}
		}
	}
}
