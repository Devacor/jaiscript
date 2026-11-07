// Combat: line of sight, hitscan weapons, projectiles, explosions, damage.

// stepped-ray LOS on the tile grid (deterministic float march)
function los_clear(float ax, float ay, float bx, float by) -> bool {
	float dx = bx - ax;
	float dy = by - ay;
	float len = math::sqrt(dx * dx + dy * dy);
	if (len < 0.001) { return true; }
	float inv = 1.0 / len;
	dx = dx * inv;
	dy = dy * inv;
	float t = 0.15;
	while (t < len) {
		if (tile_solid(tile_at(math::ifloor(ax + dx * t), math::ifloor(ay + dy * t)))) { return false; }
		t += 0.15;
	}
	return true;
}

// wall distance along an arbitrary ray — the SAME pure DDA the renderer uses,
// called serially (pure functions are just functions outside a region)
function wall_dist(float ax, float ay, float dx, float dy) -> float {
	var hit = gloom_ray(ray_record(ax, ay, dx, dy));
	float pd = hit[0];
	// gloom_ray returns PERPENDICULAR distance for the render plane; for a
	// bare ray the euclidean distance is what we want. dx/dy is unit here, so
	// perp == euclid (camera-plane rays are the non-unit case).
	return pd;
}

// nearest live enemy along a unit ray, cross-checked against the wall.
// NOTE: `var e = G.enemies[i]` would DEEP-COPY the enemy (element reads copy
// even for shared_ptr elements) — mutations must go through auto& bindings or
// chained calls on the registry itself.
function hitscan(float ax, float ay, float dx, float dy, int lo, int hi, bool from_player) -> bool {
	float wd = wall_dist(ax, ay, dx, dy);
	int best = -1;
	float best_t = 999.0;
	int i = 0;
	for (auto& e : G.enemies) {
		if (e.alive) {
			float rx = e.x - ax;
			float ry = e.y - ay;
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
	fx_wall_hit(ax + dx * (wd - 0.08), ay + dy * (wd - 0.08));
	return false;
}

// turret bolt: instant, accuracy falls off with range
function turret_shot(float tx, float ty) -> void {
	float dx = G.px - tx;
	float dy = G.py - ty;
	float d = math::sqrt(dx * dx + dy * dy);
	if (d < 0.001) { return; }
	fx_muzzle(tx, ty, dx / d, dy / d);
	int hit_pct = imax(12, 66 - math::itrunc(d * 5.0));
	if (RNG.chance(hit_pct) && los_clear(tx, ty, G.px, G.py)) {
		damage_player(RNG.roll(2, 6), "turret fire");
	} else {
		// tracer smacks the wall behind you
		float wd = wall_dist(tx, ty, dx / d, dy / d) - 0.1;
		float t = d + 1.5;
		if (t > wd) { t = wd; }
		fx_wall_hit(tx + dx / d * t, ty + dy / d * t);
	}
}

// ------------------------------------------------------------ projectiles ---
// kind 1 spitter gob | 2 warden hollow fire | 3 player hex bolt
class Shot {
public:
	int kind = 0;
	float x = 0.0;
	float y = 0.0;
	float vx = 0.0;
	float vy = 0.0;
	int ttl = 0;

	Shot(int kind_, float x_, float y_, float vx_, float vy_, int ttl_) {
		kind = kind_;
		x = x_;
		y = y_;
		vx = vx_;
		vy = vy_;
		ttl = ttl_;
	}
}

function spawn_shot(int kind, float ax, float ay, float tx, float ty, float speed) -> void {
	float dx = tx - ax;
	float dy = ty - ay;
	float len = math::sqrt(dx * dx + dy * dy);
	if (len < 0.001) { return; }
	G.shots.push(new Shot(kind, ax + dx / len * 0.4, ay + dy / len * 0.4, dx / len * speed, dy / len * speed, 150));
}

function spawn_shot_dir(int kind, float ax, float ay, float dx, float dy, float speed) -> void {
	G.shots.push(new Shot(kind, ax + dx * 0.45, ay + dy * 0.45, dx * speed, dy * speed, 150));
}

function update_shots() -> void {
	array<shared_ptr<Shot>> kept = [];
	for (auto& s : G.shots) {
		int kind = s.kind;
		float x = s.x;
		float y = s.y;
		float vx = s.vx;
		float vy = s.vy;
		int ttl = s.ttl - 1;
		bool dead = ttl <= 0;
		// 3 substeps so fast bolts don't tunnel tile corners
		for (int sub = 0; sub < 3 && !dead; ++sub) {
			x += vx * TICK * 0.333333333;
			y += vy * TICK * 0.333333333;
			if (tile_solid(tile_at(math::ifloor(x), math::ifloor(y)))) {
				if (kind == 3) { explode_hex(x - vx * 0.02, y - vy * 0.02); }
				else if (kind == 2) { explode_fire(x - vx * 0.02, y - vy * 0.02); }
				else { fx_wall_hit(x - vx * 0.02, y - vy * 0.02); }
				dead = true;
				break;
			}
			if (kind == 3) {
				// player bolt vs enemies (auto&: element reads deep-copy)
				for (auto& e : G.enemies) {
					if (!e.alive) { continue; }
					if (dist2(x, y, e.x, e.y) < (e.radius + 0.2) * (e.radius + 0.2)) {
						explode_hex(x, y);
						dead = true;
						break;
					}
				}
			} else {
				// enemy shot vs player
				if (dist2(x, y, G.px, G.py) < 0.14) {
					if (kind == 2) { explode_fire(x, y); }
					else { damage_player(RNG.roll(4, 9), "a caustic gob"); fx_blood(x, y, 0.5, 3); }
					dead = true;
					break;
				}
			}
		}
		if (!dead) {
			s.x = x;
			s.y = y;
			s.ttl = ttl;
			kept.push(s);
		}
	}
	G.shots = kept;
	// glow trail
	int i2 = 0;
	for (auto& s2 : G.shots) {
		int k2 = s2.kind;
		if ((G.tick + i2) % 2 == 0) {
			G.fx.spawn(k2 == 1 ? 6 : (k2 == 2 ? 1 : 6), s2.x, s2.y, 0.5, 0.0, 0.0, 0.0, 4);
		}
		i2 = i2 + 1;
	}
}

function explode_hex(float x, float y) -> void {
	fx_explosion(x, y);
	G.noise = 20;
	for (auto& e : G.enemies) {
		if (!e.alive) { continue; }
		float d2 = dist2(x, y, e.x, e.y);
		if (d2 < 3.24) {                              // radius 1.8
			int dmg = math::itrunc(42.0 - math::sqrt(d2) * 14.0);
			if (dmg > 0) { e.hurt(dmg); }
		}
	}
	// standing in your own spell is a choice
	float pd2 = dist2(x, y, G.px, G.py);
	if (pd2 < 2.25) {
		int self = math::itrunc(20.0 - math::sqrt(pd2) * 10.0);
		if (self > 0) { damage_player(self, "your own hex"); }
	}
}

function explode_fire(float x, float y) -> void {
	fx_explosion(x, y);
	float pd2 = dist2(x, y, G.px, G.py);
	if (pd2 < 2.89) {                                 // radius 1.7
		int dmg = math::itrunc(26.0 - math::sqrt(pd2) * 9.0);
		if (dmg > 0) { damage_player(dmg, "hollow fire"); }
	}
}

// ------------------------------------------------------------ the player ----
function damage_player(int dmg, string src) -> void {
	if (G.mode != 1 || dmg <= 0) { return; }
	if (HOST_GOD) { return; }
	int absorbed = imin(G.armor, dmg * 2 / 3);
	G.armor = G.armor - absorbed;
	int taken = dmg - absorbed;
	G.hp = G.hp - taken;
	G.face_pain = 12;
	G.hurt_flash = 4;
	show_msg(`-${taken} hp (${src})`, "91");
	if (G.hp <= 0) {
		G.hp = 0;
		G.mode = 3;
		G.mode_t = 0;
		G.death_cause = src;
		show_msg("you died", "91");
	}
}

function player_fire() -> void {
	if (G.cooldown > 0) { return; }
	var w = WEAPONS[G.weapon];
	int ammo_type = w["ammo"];
	if (G.ammo[ammo_type] <= 0) {
		show_msg(`out of ${AMMO_NAMES[ammo_type]}S`, "93");
		G.cooldown = 8;
		return;
	}
	G.ammo[ammo_type] = G.ammo[ammo_type] - 1;
	G.cooldown = w["cd"];
	G.muzzle = 3;
	G.noise = 24;
	G.gun_kick = 4;
	float dirx = math::cos(G.pang);
	float diry = math::sin(G.pang);
	fx_muzzle(G.px, G.py, dirx, diry);
	if (w["kind"] == 1) {
		spawn_shot_dir(3, G.px, G.py, dirx, diry, 9.0);
		return;
	}
	int pellets = w["pellets"];
	float spread = w["spread"];
	for (int i = 0; i < pellets; ++i) {
		float a = G.pang + (RNG.nextf() - 0.5) * 2.0 * spread;
		hitscan(G.px, G.py, math::cos(a), math::sin(a), w["lo"], w["hi"], true);
	}
}
