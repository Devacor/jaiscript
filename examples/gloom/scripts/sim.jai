// The Player: pose, vitals, arsenal, movement with wall sliding, the 'use'
// action (doors, secrets, the exit switch), pickups, hitscan fire, damage.

class Player {
public:
	float px = 2.5;
	float py = 2.5;
	float pang = 0.0;
	int hp = 100;
	int armor = 0;
	array<int> ammo = [48, 0, 0];
	array<int> have = [1, 0, 0];
	int weapon = 0;
	bool key_r = false;
	bool key_b = false;
	int cooldown = 0;
	float bob = 0.0;
	int muzzle = 0;
	int gun_kick = 0;
	string death_cause = "the gloom";

	array<int> entry_have = [1, 0, 0];
	array<int> entry_ammo = [48, 0, 0];
	int entry_armor = 0;
	int entry_weapon = 0;

	// feedback timers (the HUD face + the red screen border)
	int face_pain = 0;
	int face_grin = 0;
	int hurt_flash = 0;

	void place(float x, float y) {
		px = x;
		py = y;
		pang = 0.0;
	}

	// reset_vitals, not reset: a shared_ptr's own reset() would answer player.reset()
	void reset_vitals() {
		hp = 100;
		armor = 0;
		ammo = [48, 0, 0];
		have = [1, 0, 0];
		weapon = 0;
		cooldown = 0;
		bob = 0.0;
	}

	// what you walked onto the floor with, which a death on it gives back
	void remember_loadout() {
		entry_have = have;
		entry_ammo = ammo;
		entry_armor = armor;
		entry_weapon = weapon;
	}

	// a death restarts the floor with full health and the arsenal you brought in, every gun at least half stocked
	void respawn() {
		reset_vitals();
		have = entry_have;
		armor = entry_armor;
		weapon = entry_weapon;
		array<int> least = [48, 16, 40];
		for (int t = 0; t < 3; ++t) {
			ammo[t] = have[t] == 1 ? imax(entry_ammo[t], least[t]) : entry_ammo[t];
		}
	}

	void move(shared_ptr<Input> input) {
		float turn = 0.0;
		if (input.tl) { turn -= 1.0; }
		if (input.tr) { turn += 1.0; }
		pang = pang + turn * 2.7 * TICK;
		while (pang > 6.2831853) { pang -= 6.2831853; }
		while (pang < 0.0) { pang += 6.2831853; }

		float dirx = math::cos(pang);
		float diry = math::sin(pang);
		float wx = 0.0;
		float wy = 0.0;
		if (input.fwd) { wx += dirx; wy += diry; }
		if (input.back) { wx -= dirx; wy -= diry; }
		if (input.sl) { wx += diry; wy -= dirx; }
		if (input.sr) { wx -= diry; wy += dirx; }
		float wl = math::sqrt(wx * wx + wy * wy);
		if (wl > 0.001) {
			float speed = (input.run ? 5.0 : 3.4) * TICK;
			wx = wx / wl * speed;
			wy = wy / wl * speed;
			float nx = px + wx;
			float ny = py + wy;
			bool moved = false;
			if (G.world.spot_free(nx, py, 0.26)) { px = nx; moved = true; }
			if (G.world.spot_free(px, ny, 0.26)) { py = ny; moved = true; }
			if (moved) {
				bob = bob + (input.run ? 0.30 : 0.22);
				check_pickups();
			}
		}
	}

	// ------------------------------------------------------------ 'use' -----
	// probe a few steps down the facing for the first non-floor tile
	void use_action() {
		float dirx = math::cos(pang);
		float diry = math::sin(pang);
		array<float> probes = [0.5, 0.9, 1.3];
		for (auto p : probes) {
			int tx = math::ifloor(px + dirx * p);
			int ty = math::ifloor(py + diry * p);
			int t = G.world.tile_at(tx, ty);
			if (t == 0) { continue; }
			use_tile(tx, ty, t);
			return;
		}
		G.hud.show("nothing to use", "90");
	}

	// ------------------------------------------------------------ combat ----
	void fire() {
		if (cooldown > 0) { return; }
		auto& w = WEAPONS[weapon];
		int ammo_type = w.ammo;
		if (ammo[ammo_type] <= 0) {
			G.hud.show(`out of ${AMMO_NAMES[ammo_type]}S`, "93");
			cooldown = 8;
			return;
		}
		ammo[ammo_type] = ammo[ammo_type] - 1;
		cooldown = w.cd;
		muzzle = 3;
		G.noise = 24;
		gun_kick = 4;
		float dirx = math::cos(pang);
		float diry = math::sin(pang);
		G.fx.muzzle(px, py, dirx, diry);
		if (w.kind == 1) {
			G.spawn_shot_dir(3, px, py, dirx, diry, 9.0);
			return;
		}
		int pellets = w.pellets;
		float spread = w.spread;
		for (int i = 0; i < pellets; ++i) {
			float a = pang + (RNG.nextf() - 0.5) * 2.0 * spread;
			hitscan(math::cos(a), math::sin(a), w.lo, w.hi);
		}
	}

	// the nearest live enemy along a unit ray, short of the wall; hurt it, or chip the wall
	bool hitscan(float dx, float dy, int lo, int hi) {
		float wd = G.world.wall_dist(px, py, dx, dy);
		int best = -1;
		float best_t = 999.0;
		int i = 0;
		for (auto& e : G.enemies) {
			if (e.alive) {
				float rx = e.x - px;
				float ry = e.y - py;
				float t = rx * dx + ry * dy;
				if (t >= 0.2 && t <= wd + e.radius && t < best_t) {
					float perp = rx * dy - ry * dx;
					if (perp < 0.0) { perp = 0.0 - perp; }
					if (perp < e.radius + 0.12) {
						best = i;
						best_t = t;
					}
				}
			}
			i = i + 1;
		}
		if (best >= 0) {
			G.enemies[best].hurt(RNG.roll(lo, hi));
			return true;
		}
		// wall impact garnish just short of the surface
		G.fx.wall_hit(px + dx * (wd - 0.08), py + dy * (wd - 0.08));
		return false;
	}

	void damage(int dmg, string src) {
		if (G.mode != 1 || dmg <= 0) { return; }
		if (HOST_GOD) { return; }
		int absorbed = imin(armor, dmg * 2 / 3);
		armor = armor - absorbed;
		int taken = dmg - absorbed;
		hp = hp - taken;
		face_pain = 12;
		hurt_flash = 4;
		G.hud.show(`-${taken} hp (${src})`, "91");
		if (hp <= 0) {
			hp = 0;
			G.mode = 3;
			G.mode_t = 0;
			death_cause = src;
			G.hud.show("you died", "91");
		}
	}

	void tick_timers() {
		if (cooldown > 0) { cooldown = cooldown - 1; }
		if (muzzle > 0) { muzzle = muzzle - 1; }
		if (gun_kick > 0) { gun_kick = gun_kick - 1; }
		if (face_pain > 0) { face_pain = face_pain - 1; }
		if (face_grin > 0) { face_grin = face_grin - 1; }
		if (hurt_flash > 0) { hurt_flash = hurt_flash - 1; }
	}

private:
	void use_tile(int tx, int ty, int t) {
		if (t == 5) { open_door(tx, ty, "the door grinds open"); return; }
		if (t == 8) {
			if (key_r) { open_door(tx, ty, "RED lock released"); }
			else { G.hud.show("you need the RED keycard", "91"); }
			return;
		}
		if (t == 9) {
			if (key_b) { open_door(tx, ty, "BLUE lock released"); }
			else { G.hud.show("you need the BLUE keycard", "94"); }
			return;
		}
		if (t == 7) {
			G.world.clear_tile(tx, ty);
			G.secrets = G.secrets + 1;
			G.fx.secret_glitter(tx + 0.5, ty + 0.5);
			G.hud.show("you found a secret!", "96");
			return;
		}
		if (t == 6) {
			G.complete_level();
			return;
		}
		G.hud.show("solid rock. very solid.", "90");
	}

	void open_door(int tx, int ty, string msg) {
		G.world.clear_tile(tx, ty);
		G.fx.door_puff(tx + 0.5, ty + 0.5);
		G.noise = imax(G.noise, 10);
		G.hud.show(msg, "97");
	}

	// -------------------------------------------------------- pickups -------
	bool give_ammo(int t, int n) {
		if (ammo[t] >= AMMO_MAX[t]) { return false; }
		ammo[t] = imin(AMMO_MAX[t], ammo[t] + n);
		return true;
	}

	void check_pickups() {
		for (auto& it : G.items) {
			if (it.taken) { continue; }
			int kind = it.kind;
			if (dist2(px, py, it.x, it.y) > 0.36) { continue; }
			bool got = false;
			if (kind == 0) { if (hp < 100) { hp = imin(100, hp + 10); got = true; } }
			else if (kind == 1) { if (hp < 100) { hp = imin(100, hp + 25); got = true; } }
			else if (kind == 2) { if (armor < 100) { armor = imin(100, armor + 50); got = true; } }
			else if (kind == 3) { got = give_ammo(0, 20); }
			else if (kind == 4) { got = give_ammo(1, 8); }
			else if (kind == 5) { got = give_ammo(2, 30); }
			else if (kind == 6) { key_r = true; got = true; }
			else if (kind == 7) { key_b = true; got = true; }
			else if (kind == 8) {
				have[1] = 1;
				give_ammo(1, 8);
				weapon = 1;
				cooldown = 8;
				got = true;
				face_grin = 30;
			}
			else if (kind == 9) {
				have[2] = 1;
				give_ammo(2, 40);
				weapon = 2;
				cooldown = 8;
				got = true;
				face_grin = 30;
			}
			else if (kind == 10) { hp = imin(150, hp + 40); got = true; face_grin = 20; }
			if (got) {
				it.taken = true;
				G.hud.show(`picked up ${ITEM_DEFS[kind].name}`, "93");
			}
		}
	}
}
