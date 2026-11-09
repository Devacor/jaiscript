// Projectiles: Shot objects that step themselves and explode themselves.
// Sight lines live on World, hitscan and damage on Player, the turret's bolt
// on Turret.

// kind 1 spitter gob | 2 warden hollow fire | 3 player hex bolt
class Shot {
public:
	int kind = 0;
	float x = 0.0;
	float y = 0.0;
	float vx = 0.0;
	float vy = 0.0;
	int ttl = 150;

	Shot(int kind_, float x_, float y_, float vx_, float vy_) {
		kind = kind_;
		x = x_;
		y = y_;
		vx = vx_;
		vy = vy_;
	}

	// one sim tick; returns false when the shot is spent.
	// 3 substeps so fast bolts don't tunnel tile corners.
	bool tick() {
		ttl = ttl - 1;
		bool dead = ttl <= 0;
		for (int sub = 0; sub < 3 && !dead; ++sub) {
			x += vx * TICK * 0.333333333;
			y += vy * TICK * 0.333333333;
			if (G.world.solid_at(math::ifloor(x), math::ifloor(y))) {
				if (kind == 3) { explode_hex(x - vx * 0.02, y - vy * 0.02); }
				else if (kind == 2) { explode_fire(x - vx * 0.02, y - vy * 0.02); }
				else { G.fx.wall_hit(x - vx * 0.02, y - vy * 0.02); }
				dead = true;
				break;
			}
			if (kind == 3) {
				// player bolt vs enemies
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
				if (dist2(x, y, G.player.px, G.player.py) < 0.14) {
					if (kind == 2) { explode_fire(x, y); }
					else {
						G.player.damage(RNG.roll(4, 9), "a caustic gob");
						G.fx.blood(x, y, 0.5, 3);
					}
					dead = true;
					break;
				}
			}
		}
		return !dead;
	}

private:
	void explode_hex(float hx, float hy) {
		G.fx.explosion(hx, hy);
		G.noise = 20;
		for (auto& e : G.enemies) {
			if (!e.alive) { continue; }
			float d2 = dist2(hx, hy, e.x, e.y);
			if (d2 < 3.24) {                              // radius 1.8
				int dmg = math::itrunc(42.0 - math::sqrt(d2) * 14.0);
				if (dmg > 0) { e.hurt(dmg); }
			}
		}
		// standing in your own spell is a choice
		float pd2 = dist2(hx, hy, G.player.px, G.player.py);
		if (pd2 < 2.25) {
			int self_dmg = math::itrunc(20.0 - math::sqrt(pd2) * 10.0);
			if (self_dmg > 0) { G.player.damage(self_dmg, "your own hex"); }
		}
	}

	void explode_fire(float hx, float hy) {
		G.fx.explosion(hx, hy);
		float pd2 = dist2(hx, hy, G.player.px, G.player.py);
		if (pd2 < 2.89) {                                 // radius 1.7
			int dmg = math::itrunc(26.0 - math::sqrt(pd2) * 9.0);
			if (dmg > 0) { G.player.damage(dmg, "hollow fire"); }
		}
	}
}
