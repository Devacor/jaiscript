// Enemies: one class per bestiary entry. Each overrides the coroutine method
// make_brain(), whose handle lives in a field: a phase is a loop in the
// coroutine frame, and pain interrupts by dropping the handle. Hooks give the
// death effects and the art. G.enemies holds them as shared_ptr handles.

enum EnemyState { dormant, hunt, attack, dying, dead }

class Enemy {
public:
	int kind = 0;
	string art_base = "grunt";   // sprite family; subclass sets via super()
	float x = 0.0;
	float y = 0.0;
	int hp = 30;
	bool alive = true;
	int state = EnemyState.dormant;
	int anim = 0;          // ticks in current state (drives art frames)
	int walk = 0;          // walk cycle counter (advances only when moving)
	int flash = 0;         // damage flash ticks (renderer whitens the LUT)
	int stun = 0;          // pain stun ticks (brain is dropped + re-minted)
	float radius = 0.34;
	float speed = 3.0;
	int gib_count = 7;
	var brain = null;      // the coroutine handle

	Enemy(int kind_, string art_, float x_, float y_) {
		kind = kind_;
		art_base = art_;
		x = x_;
		y = y_;
		auto& def = ENEMY_DEFS[kind_];
		hp = def.hp;
		radius = def.radius;
		speed = def.speed;
	}

	// one sim tick: corpse cools, stun blocks, otherwise the brain runs.
	// make_brain() dispatches virtually — the base never names a subclass.
	void tick() {
		anim = anim + 1;
		if (flash > 0) { flash = flash - 1; }
		if (!alive) { return; }
		if (stun > 0) {
			stun = stun - 1;
			return;
		}
		if (brain == null || brain.done()) { brain = make_brain(); }
		brain.next();
	}

	// overridden by every subclass; the base brain just stands there
	coroutine void make_brain() {
		while (true) { yield; }
	}

	float player_dist() { return math::sqrt(dist2(x, y, G.player.px, G.player.py)); }

	bool sees_player() {
		if (player_dist() > 13.0) { return false; }
		return G.world.los_clear(x, y, G.player.px, G.player.py);
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
		if (G.world.spot_free(nx, y, radius)) { x = nx; walk = walk + 1; }
		if (G.world.spot_free(x, ny, radius)) { y = ny; walk = walk + 1; }
	}

	void hurt(int dmg) {
		if (!alive) { return; }
		hp = hp - dmg;
		flash = 4;
		G.fx.blood(x, y, 0.5, imin(6, 2 + dmg / 6));
		if (hp <= 0) {
			die();
			return;
		}
		// pain: drop the brain mid-phase; it re-mints next tick = re-telegraph
		if (RNG.chance(ENEMY_DEFS[kind].pain)) {
			stun = 5 + RNG.next(5);
			brain = null;
			state = EnemyState.hunt;
			anim = 0;
		}
	}

	void die() {
		alive = false;
		state = EnemyState.dying;
		anim = 0;
		brain = null;
		G.kills = G.kills + 1;
		G.fx.gibs(x, y, gib_count);
		on_death();
		G.hud.show(`${ENEMY_DEFS[kind].name} destroyed`, "92");
	}

	// template-method hook: subclasses add death effects (Warden explodes)
	void on_death() {}

	// which art this body renders as, given its state machinery
	string art_name() {
		if (alive) {
			if (state == EnemyState.attack) { return art_base + "_at"; }
			return (walk / 6) % 2 == 0 ? art_base + "_w0" : art_base + "_w1";
		}
		if (anim < 6) { return art_base + "_d0"; }
		if (anim < 12) { return art_base + "_d1"; }
		return art_base + "_dead";
	}
}

// --------------------------------------------------------------- GRUNT ------
// doze -> roar -> relentless zigzag chase -> lunge bite
class Grunt : Enemy {
public:
	Grunt(float x_, float y_) : super(0, "grunt", x_, y_) {}

	coroutine void make_brain() override {
		while (state == EnemyState.dormant) {
			if ((sees_player() && player_dist() < 11.0) || hears_player()) { break; }
			yield;
		}
		state = EnemyState.hunt;
		anim = 0;
		for (int i = 0; i < 7; ++i) { yield; }   // the roar (it commits)
		int zig = RNG.next(2) == 0 ? 1 : -1;
		while (true) {
			float d = player_dist();
			if (d < 1.3) {
				state = EnemyState.attack;
				anim = 0;
				for (int i = 0; i < 6; ++i) { yield; }     // lunge windup
				if (player_dist() < 1.6 && G.world.los_clear(x, y, G.player.px, G.player.py)) {
					G.player.damage(RNG.roll(ENEMY_DEFS[0].melee_lo, ENEMY_DEFS[0].melee_hi), "a grunt's claws");
				}
				state = EnemyState.hunt;
				anim = 0;
				for (int i = 0; i < 5; ++i) { yield; }     // recover
				continue;
			}
			// zigzag pursuit: aim past the player's flank, swapping sides
			if (anim % 20 == 19) { zig = 0 - zig; }
			float fx = G.player.py - y;
			float fy = x - G.player.px;
			float fl = math::sqrt(fx * fx + fy * fy);
			if (fl < 0.001) { fl = 1.0; }
			float lean = d > 3.0 ? 0.9 : 0.2;
			move_toward(G.player.px + fx / fl * lean * zig, G.player.py + fy / fl * lean * zig);
			yield;
		}
	}
}

// -------------------------------------------------------------- SPITTER -----
// keeps its range band, strafes, telegraphs, spits
class Spitter : Enemy {
public:
	Spitter(float x_, float y_) : super(1, "spit", x_, y_) {}

	coroutine void make_brain() override {
		while (state == EnemyState.dormant) {
			if ((sees_player() && player_dist() < 12.0) || hears_player()) { break; }
			yield;
		}
		state = EnemyState.hunt;
		anim = 0;
		int orbit = RNG.next(2) == 0 ? 1 : -1;
		while (true) {
			float d = player_dist();
			bool los = G.world.los_clear(x, y, G.player.px, G.player.py);
			if (los && d < 9.5 && d > 2.0) {
				// telegraph glow, then the gob
				state = EnemyState.attack;
				anim = 0;
				for (int i = 0; i < 9; ++i) { yield; }
				if (G.world.los_clear(x, y, G.player.px, G.player.py)) {
					G.spawn_shot(1, x, y, G.player.px, G.player.py, 6.5);
				}
				state = EnemyState.hunt;
				anim = 0;
				// cooldown spent orbiting sideways
				for (int i = 0; i < 16; ++i) {
					float ox = G.player.py - y;
					float oy = x - G.player.px;
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
					G.player.damage(RNG.roll(ENEMY_DEFS[1].melee_lo, ENEMY_DEFS[1].melee_hi), "a spitter's talons");
				}
				move_toward(x + (x - G.player.px), y + (y - G.player.py));
			} else {
				move_toward(G.player.px, G.player.py);
			}
			yield;
		}
	}
}

// --------------------------------------------------------------- TURRET -----
// dormant metal until it has line of sight; then 3-round bursts. Breaking LOS
// resets it to a wary idle (its coroutine never forgets).
class Turret : Enemy {
public:
	Turret(float x_, float y_) : super(2, "tur", x_, y_) {}

	coroutine void make_brain() override {
		while (true) {
			while (!(sees_player() && player_dist() < 11.0)) {
				state = EnemyState.dormant;
				yield;
			}
			state = EnemyState.hunt;     // waking whir
			anim = 0;
			for (int i = 0; i < 8; ++i) { yield; }
			while (sees_player() && player_dist() < 12.0) {
				state = EnemyState.attack;
				anim = 0;
				for (int burst = 0; burst < 3; ++burst) {
					shoot();
					yield;
					yield;
				}
				state = EnemyState.hunt;
				anim = 0;
				for (int i = 0; i < 13; ++i) { yield; }
			}
		}
	}

	// instant bolt: accuracy falls off with range
	void shoot() {
		float dx = G.player.px - x;
		float dy = G.player.py - y;
		float d = math::sqrt(dx * dx + dy * dy);
		if (d < 0.001) { return; }
		G.fx.muzzle(x, y, dx / d, dy / d);
		int hit_pct = imax(12, 66 - math::itrunc(d * 5.0));
		if (RNG.chance(hit_pct) && G.world.los_clear(x, y, G.player.px, G.player.py)) {
			G.player.damage(RNG.roll(2, 6), "turret fire");
		} else {
			// tracer smacks the wall behind you
			float wd = G.world.wall_dist(x, y, dx / d, dy / d) - 0.1;
			float t = d + 1.5;
			if (t > wd) { t = wd; }
			G.fx.wall_hit(x + dx / d * t, y + dy / d * t);
		}
	}

	// the turret has its own frame set (no walk cycle — it is furniture)
	string art_name() override {
		if (alive) {
			if (state == EnemyState.attack) { return "tur_fire"; }
			return "tur_idle";
		}
		if (anim < 6) { return "tur_d0"; }
		if (anim < 12) { return "tur_d1"; }
		return "tur_dead";
	}
}

// --------------------------------------------------------------- WARDEN -----
// the episode's landlord. Stalks and lobs hollow fire; under half health he
// goes double-volley and calls the family, exactly once.
class Warden : Enemy {
public:
	Warden(float x_, float y_) : super(3, "war", x_, y_) {
		gib_count = 14;
	}

	coroutine void make_brain() override {
		bool enraged = false;
		bool summoned = false;
		while (state == EnemyState.dormant) {
			if ((sees_player() && player_dist() < 13.0) || hears_player()) { break; }
			yield;
		}
		state = EnemyState.hunt;
		anim = 0;
		G.hud.show("THE WARDEN HAS SEEN YOU", "91");
		for (int i = 0; i < 10; ++i) { yield; }
		while (true) {
			if (!enraged && hp * 2 < ENEMY_DEFS[3].hp) {
				enraged = true;
				speed = speed * 1.5;
				G.hud.show("The Warden's wounds glow like coals", "91");
			}
			if (enraged && !summoned) {
				summoned = true;
				summon_pack();
				G.hud.show("The Warden howls for his pack", "91");
			}
			float d = player_dist();
			bool los = G.world.los_clear(x, y, G.player.px, G.player.py);
			if (los && d > 2.4) {
				state = EnemyState.attack;
				anim = 0;
				for (int i = 0; i < 8; ++i) { yield; }
				int volleys = enraged ? 2 : 1;
				for (int v = 0; v < volleys; ++v) {
					if (G.world.los_clear(x, y, G.player.px, G.player.py)) {
						G.spawn_shot(2, x, y, G.player.px, G.player.py, 5.5);
					}
					for (int i = 0; i < 4; ++i) { yield; }
				}
				state = EnemyState.hunt;
				anim = 0;
				int rest = enraged ? 6 : 12;
				for (int i = 0; i < rest; ++i) {
					move_toward(G.player.px, G.player.py);
					yield;
				}
				continue;
			}
			if (d < 1.7) {
				state = EnemyState.attack;
				anim = 0;
				for (int i = 0; i < 5; ++i) { yield; }
				if (player_dist() < 2.0) {
					G.player.damage(RNG.roll(ENEMY_DEFS[3].melee_lo, ENEMY_DEFS[3].melee_hi), "the Warden's fist");
				}
				state = EnemyState.hunt;
				anim = 0;
				for (int i = 0; i < 4; ++i) { yield; }
				continue;
			}
			move_toward(G.player.px, G.player.py);
			yield;
		}
	}

	// warden's reinforcements: grunts at the nearest free tiles around him.
	// They count toward the map total (an honest 100% asks for the whole family).
	void summon_pack() {
		array<math::ivec2> offsets = [math::ivec2(2, 0), math::ivec2(-2, 0), math::ivec2(0, 2), math::ivec2(0, -2)];
		int placed = 0;
		for (auto& offset : offsets) {
			if (placed >= 2) { break; }
			int tx = math::ifloor(x) + offset.x;
			int ty = math::ifloor(y) + offset.y;
			if (!G.world.walkable(tx, ty)) { continue; }
			G.spawn_enemy(0, tx, ty);
			G.fx.explosion(tx + 0.5, ty + 0.5);
			placed = placed + 1;
		}
	}

	void on_death() override {
		G.fx.explosion(x, y);
		G.warden_down = true;
	}
}
