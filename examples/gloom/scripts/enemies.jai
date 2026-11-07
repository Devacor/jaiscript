// Enemies. Instances live in G.enemies as shared_ptr handles
// — registry-of-handles per the aliasing guidance — and each enemy's BRAIN is a
// coroutine handle stored in its own field: kind-specific coroutine METHODS
// mint the handle, the handle pins the instance, and per-tick next() drives
// multi-phase behavior with all phase state living in the coroutine frame.

int ENEMY_STATE_DORMANT = 0;
int ENEMY_STATE_HUNT = 1;
int ENEMY_STATE_ATTACK = 2;
int ENEMY_STATE_DYING = 3;
int ENEMY_STATE_DEAD = 4;

class Enemy {
public:
	int kind = 0;
	float x = 0.0;
	float y = 0.0;
	int hp = 30;
	bool alive = true;
	int state = 0;         // ENEMY_STATE_*
	int anim = 0;          // ticks in current state (drives art frames)
	int walk = 0;          // walk cycle counter (advances only when moving)
	int flash = 0;         // damage flash ticks (renderer whitens the LUT)
	int stun = 0;          // pain stun ticks (brain is dropped + re-minted)
	float radius = 0.34;
	float speed = 3.0;
	var brain = null;      // coroutine handle in a field: the whole point

	Enemy(int kind_, float x_, float y_) {
		kind = kind_;
		x = x_;
		y = y_;
		hp = ENEMY_DEFS[kind_]["hp"];
		radius = ENEMY_DEFS[kind_]["radius"];
		speed = ENEMY_DEFS[kind_]["speed"];
	}

	// one sim tick: corpse cools, stun blocks, otherwise the brain runs
	void tick() {
		anim = anim + 1;
		if (flash > 0) { flash = flash - 1; }
		if (!alive) { return; }
		if (stun > 0) {
			stun = stun - 1;
			return;
		}
		if (brain == null || brain.done()) {
			if (kind == 0) { brain = brain_grunt(); }
			else if (kind == 1) { brain = brain_spitter(); }
			else if (kind == 2) { brain = brain_turret(); }
			else { brain = brain_warden(); }
		}
		brain.next();
	}

	float player_dist() { return math::sqrt(dist2(x, y, G.px, G.py)); }

	bool sees_player() {
		if (player_dist() > 13.0) { return false; }
		return los_clear(x, y, G.px, G.py);
	}

	bool hears_player() {
		return G.noise > 0 && player_dist() < 12.0;
	}

	// slide movement against the tile grid (axes resolved separately)
	void move_toward(float tx, float ty) {
		float dx = tx - x;
		float dy = ty - y;
		float len = math::sqrt(dx * dx + dy * dy);
		if (len < 0.0001) { return; }
		float step = speed * TICK;
		float nx = x + dx / len * step;
		float ny = y + dy / len * step;
		if (spot_free(nx, y, radius)) { x = nx; walk = walk + 1; }
		if (spot_free(x, ny, radius)) { y = ny; walk = walk + 1; }
	}

	void hurt(int dmg) {
		if (!alive) { return; }
		hp = hp - dmg;
		flash = 4;
		fx_blood(x, y, 0.5, imin(6, 2 + dmg / 6));
		if (hp <= 0) {
			die();
			return;
		}
		// pain: drop the brain mid-phase; it re-minted next tick = re-telegraph
		if (RNG.chance(ENEMY_DEFS[kind]["pain"])) {
			stun = 5 + RNG.next(5);
			brain = null;
			state = ENEMY_STATE_HUNT;
			anim = 0;
		}
	}

	void die() {
		alive = false;
		state = ENEMY_STATE_DYING;
		anim = 0;
		brain = null;
		G.kills = G.kills + 1;
		fx_gibs(x, y, kind == 3 ? 14 : 7);
		if (kind == 3) {
			fx_explosion(x, y);
			G.warden_down = true;
		}
		show_msg(`${ENEMY_DEFS[kind]["name"]} destroyed`, "92");
	}

	// --------------------------------------------------------- brains -------
	// One yield = one tick. Timers are plain loop counters living in the
	// coroutine frame; pain interrupts by discarding the handle (tick()).

	// GRUNT: doze -> roar -> relentless zigzag chase -> lunge bite
	coroutine void brain_grunt() {
		while (state == ENEMY_STATE_DORMANT) {
			if ((sees_player() && player_dist() < 11.0) || hears_player()) { break; }
			yield;
		}
		state = ENEMY_STATE_HUNT;
		anim = 0;
		for (int i = 0; i < 7; ++i) { yield; }   // the roar (it commits)
		int zig = RNG.next(2) == 0 ? 1 : -1;
		while (true) {
			float d = player_dist();
			if (d < 1.3) {
				state = ENEMY_STATE_ATTACK;
				anim = 0;
				for (int i = 0; i < 6; ++i) { yield; }     // lunge windup
				if (player_dist() < 1.6 && los_clear(x, y, G.px, G.py)) {
					damage_player(RNG.roll(ENEMY_DEFS[0]["melee_lo"], ENEMY_DEFS[0]["melee_hi"]), "a grunt's claws");
				}
				state = ENEMY_STATE_HUNT;
				anim = 0;
				for (int i = 0; i < 5; ++i) { yield; }     // recover
				continue;
			}
			// zigzag pursuit: aim past the player's flank, swapping sides
			if (anim % 20 == 19) { zig = 0 - zig; }
			float fx = G.py - y;
			float fy = x - G.px;
			float fl = math::sqrt(fx * fx + fy * fy);
			if (fl < 0.001) { fl = 1.0; }
			float lean = d > 3.0 ? 0.9 : 0.2;
			move_toward(G.px + fx / fl * lean * zig, G.py + fy / fl * lean * zig);
			yield;
		}
	}

	// SPITTER: keeps its range band, strafes, telegraphs, spits
	coroutine void brain_spitter() {
		while (state == ENEMY_STATE_DORMANT) {
			if ((sees_player() && player_dist() < 12.0) || hears_player()) { break; }
			yield;
		}
		state = ENEMY_STATE_HUNT;
		anim = 0;
		int orbit = RNG.next(2) == 0 ? 1 : -1;
		while (true) {
			float d = player_dist();
			bool los = los_clear(x, y, G.px, G.py);
			if (los && d < 9.5 && d > 2.0) {
				// telegraph glow, then the gob
				state = ENEMY_STATE_ATTACK;
				anim = 0;
				for (int i = 0; i < 9; ++i) { yield; }
				if (los_clear(x, y, G.px, G.py)) {
					spawn_shot(1, x, y, G.px, G.py, 6.5);
				}
				state = ENEMY_STATE_HUNT;
				anim = 0;
				// cooldown spent orbiting sideways
				for (int i = 0; i < 16; ++i) {
					float ox = G.py - y;
					float oy = x - G.px;
					float ol = math::sqrt(ox * ox + oy * oy);
					if (ol < 0.001) { ol = 1.0; }
					move_toward(x + ox / ol * orbit, y + oy / ol * orbit);
					yield;
				}
				if (RNG.chance(40)) { orbit = 0 - orbit; }
				continue;
			}
			if (d <= 2.0) {
				// too close: shove and retreat
				if (d < 1.2 && RNG.chance(30)) {
					damage_player(RNG.roll(ENEMY_DEFS[1]["melee_lo"], ENEMY_DEFS[1]["melee_hi"]), "a spitter's talons");
				}
				move_toward(x + (x - G.px), y + (y - G.py));
			} else {
				move_toward(G.px, G.py);
			}
			yield;
		}
	}

	// TURRET: dormant metal until it has line of sight; then 3-round bursts.
	// Breaking LOS resets it to a wary idle (its coroutine never forgets).
	coroutine void brain_turret() {
		while (true) {
			while (!(sees_player() && player_dist() < 11.0)) {
				state = ENEMY_STATE_DORMANT;
				yield;
			}
			state = ENEMY_STATE_HUNT;     // waking whir
			anim = 0;
			for (int i = 0; i < 8; ++i) { yield; }
			while (sees_player() && player_dist() < 12.0) {
				state = ENEMY_STATE_ATTACK;
				anim = 0;
				for (int burst = 0; burst < 3; ++burst) {
					turret_shot(x, y);
					yield;
					yield;
				}
				state = ENEMY_STATE_HUNT;
				anim = 0;
				for (int i = 0; i < 13; ++i) { yield; }
			}
		}
	}

	// WARDEN: the landlord. Stalks and lobs hollow fire; under half health he
	// goes double-volley and calls the family, exactly once.
	coroutine void brain_warden() {
		bool enraged = false;
		bool summoned = false;
		while (state == ENEMY_STATE_DORMANT) {
			if ((sees_player() && player_dist() < 13.0) || hears_player()) { break; }
			yield;
		}
		state = ENEMY_STATE_HUNT;
		anim = 0;
		show_msg("THE WARDEN HAS SEEN YOU", "91");
		for (int i = 0; i < 10; ++i) { yield; }
		while (true) {
			if (!enraged && hp * 2 < ENEMY_DEFS[3]["hp"]) {
				enraged = true;
				speed = speed * 1.5;
				show_msg("The Warden's wounds glow like coals", "91");
			}
			if (enraged && !summoned) {
				summoned = true;
				summon_adds(x, y);
				show_msg("The Warden howls for his pack", "91");
			}
			float d = player_dist();
			bool los = los_clear(x, y, G.px, G.py);
			if (los && d > 2.4) {
				state = ENEMY_STATE_ATTACK;
				anim = 0;
				for (int i = 0; i < 8; ++i) { yield; }
				int volleys = enraged ? 2 : 1;
				for (int v = 0; v < volleys; ++v) {
					if (los_clear(x, y, G.px, G.py)) {
						spawn_shot(2, x, y, G.px, G.py, 5.5);
					}
					for (int i = 0; i < 4; ++i) { yield; }
				}
				state = ENEMY_STATE_HUNT;
				anim = 0;
				int rest = enraged ? 6 : 12;
				for (int i = 0; i < rest; ++i) {
					move_toward(G.px, G.py);
					yield;
				}
				continue;
			}
			if (d < 1.7) {
				state = ENEMY_STATE_ATTACK;
				anim = 0;
				for (int i = 0; i < 5; ++i) { yield; }
				if (player_dist() < 2.0) {
					damage_player(RNG.roll(ENEMY_DEFS[3]["melee_lo"], ENEMY_DEFS[3]["melee_hi"]), "the Warden's fist");
				}
				state = ENEMY_STATE_HUNT;
				anim = 0;
				for (int i = 0; i < 4; ++i) { yield; }
				continue;
			}
			move_toward(G.px, G.py);
			yield;
		}
	}
}

function spawn_enemy(int kind, int tx, int ty) -> void {
	G.enemies.push(new Enemy(kind, tx + 0.5, ty + 0.5));
	G.kill_total = G.kill_total + 1;
}

// warden's reinforcements: grunts at the nearest free tiles around him.
// They count toward the map total (an honest 100% asks for the whole family).
function summon_adds(float wx, float wy) -> void {
	array<array<int>> offs = [[2, 0], [-2, 0], [0, 2], [0, -2]];
	int placed = 0;
	for (auto& o : offs) {
		if (placed >= 2) { break; }
		int tx = math::ifloor(wx) + o[0];
		int ty = math::ifloor(wy) + o[1];
		if (!walkable(tx, ty)) { continue; }
		spawn_enemy(0, tx, ty);
		fx_explosion(tx + 0.5, ty + 0.5);
		placed = placed + 1;
	}
}

// which art a body renders as, given its state machinery
function enemy_art(var& e) -> string {
	array<string> base = ["grunt", "spit", "tur", "war"];
	string name = base[e.kind];
	if (e.kind == 2) {
		// turret has its own frame set
		if (e.alive) {
			if (e.state == ENEMY_STATE_ATTACK) { return "tur_fire"; }
			return "tur_idle";
		}
		if (e.anim < 6) { return "tur_d0"; }
		if (e.anim < 12) { return "tur_d1"; }
		return "tur_dead";
	}
	if (e.alive) {
		if (e.state == ENEMY_STATE_ATTACK) { return name + "_at"; }
		return (e.walk / 6) % 2 == 0 ? name + "_w0" : name + "_w1";
	}
	if (e.anim < 6) { return name + "_d0"; }
	if (e.anim < 12) { return name + "_d1"; }
	return name + "_dead";
}
