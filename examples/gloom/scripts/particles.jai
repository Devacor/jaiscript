// The particle system: a fixed pool of Particle records stepped in place by
// the pure gloom_particle_step body (pure.jai), through parallel_for when the
// host grants workers, and the burst kit every effect spawns from.

int PART_MAX = 288;

// kind 0 dead | 1 spark | 2 blood | 3 gib | 4 smoke | 5 flash | 6 hexmote
class Particle {
public:
	int kind = 0;
	float x = 0.0;
	float y = 0.0;
	float z = 0.0;
	float vx = 0.0;
	float vy = 0.0;
	float vz = 0.0;
	int life = 0;

	Particle() {}

	Particle(int kind_, float x_, float y_, float z_, float vx_, float vy_, float vz_, int life_) {
		kind = kind_;
		x = x_;
		y = y_;
		z = z_;
		vx = vx_;
		vy = vy_;
		vz = vz_;
		life = life_;
	}
}

class ParticleSystem {
public:
	array<Particle> pool = [];
	array<int> colors = [];    // palette index per kind and life phase: colors[kind * 4 + phase]

	ParticleSystem() { reset_pool(); }

	// reset_pool, not reset: a shared_ptr's own reset() would answer fx.reset()
	void reset_pool() {
		pool = [];
		for (int i = 0; i < PART_MAX; ++i) { pool.push(Particle()); }
		cursor = 0;
	}

	void spawn(int kind, float x, float y, float z, float vx, float vy, float vz, int life) {
		pool[cursor] = Particle(kind, x, y, z, vx, vy, vz, life);
		cursor = (cursor + 1) % PART_MAX;
	}

	// one pure in-place function steps the whole pool: parallel_for owns each
	// element exclusively when the host grants workers
	void update() {
		if (HOST_WORKERS <= 0) {
			for (auto& p : pool) { gloom_particle_step(p); }
		} else {
			parallel_for (auto& p : pool) { gloom_particle_step(p); }
		}
	}

	// ---------------------------------------------------------- burst kit ---
	// All velocity spreads come from RNG (the seeded host rng): deterministic.

	void muzzle(float x, float y, float dirx, float diry) {
		float mx = x + dirx * 0.5;
		float my = y + diry * 0.5;
		spawn(5, mx, my, 0.52, dirx * 2.0, diry * 2.0, 0.2, 3);
		for (int i = 0; i < 3; ++i) {
			spawn(1, mx, my, 0.5, dirx * 3.0 + spread(1.6), diry * 3.0 + spread(1.6), 0.6 + RNG.nextf(), 6);
		}
		spawn(4, mx, my, 0.55, dirx * 0.6, diry * 0.6, 0.3, 16);
	}

	void wall_hit(float x, float y) {
		for (int i = 0; i < 5; ++i) {
			spawn(1, x, y, 0.45 + RNG.nextf() * 0.2, spread(2.2), spread(2.2), 0.8 + RNG.nextf() * 1.4, 8);
		}
		spawn(4, x, y, 0.5, spread(0.3), spread(0.3), 0.4, 14);
	}

	void blood(float x, float y, float z, int n) {
		for (int i = 0; i < n; ++i) {
			spawn(2, x, y, z + RNG.nextf() * 0.25, spread(1.8), spread(1.8), 0.5 + RNG.nextf() * 1.6, 12);
		}
	}

	// death burst: meat + smoke; big enemies gib harder
	void gibs(float x, float y, int n) {
		for (int i = 0; i < n; ++i) {
			spawn(3, x, y, 0.35 + RNG.nextf() * 0.3, spread(2.6), spread(2.6), 1.2 + RNG.nextf() * 2.4, 26);
		}
		for (int i = 0; i < 3; ++i) {
			spawn(4, x, y, 0.4, spread(0.5), spread(0.5), 0.5, 20);
		}
	}

	void explosion(float x, float y) {
		spawn(5, x, y, 0.5, 0.0, 0.0, 0.0, 5);
		for (int i = 0; i < 10; ++i) {
			float a = RNG.nextf() * 6.2831853;
			float sp = 1.5 + RNG.nextf() * 3.0;
			spawn(1, x, y, 0.3 + RNG.nextf() * 0.5, math::cos(a) * sp, math::sin(a) * sp, 1.0 + RNG.nextf() * 2.0, 11);
		}
		for (int i = 0; i < 5; ++i) {
			spawn(4, x, y, 0.4, spread(0.9), spread(0.9), 0.6 + RNG.nextf(), 24);
		}
		for (int i = 0; i < 6; ++i) {
			spawn(6, x, y, 0.2 + RNG.nextf() * 0.6, spread(1.4), spread(1.4), 0.5, 20);
		}
	}

	void door_puff(float x, float y) {
		for (int i = 0; i < 4; ++i) {
			spawn(4, x, y, 0.3 + RNG.nextf() * 0.5, spread(0.7), spread(0.7), 0.3, 18);
		}
	}

	void secret_glitter(float x, float y) {
		for (int i = 0; i < 8; ++i) {
			spawn(6, x, y, 0.2 + RNG.nextf() * 0.8, spread(1.2), spread(1.2), 0.6, 24);
		}
	}

	// per-kind life-phase color ramps (phase 0 = fresh); the showcase look's come from look.jai
	void build_colors() {
		colors = [];
		if (SHOWCASE) {
			for (auto& ramp : SHOWCASE_PARTICLE_RAMPS) {
				for (auto& color : ramp) { colors.push(color_idx(color)); }
			}
			return;
		}
		array<array<array<int>>> ramps = [
			[[0, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0]],                          // 0 dead (unused)
			[[255, 240, 160], [255, 200, 90], [230, 130, 50], [140, 70, 30]],      // 1 spark
			[[210, 40, 30], [180, 34, 26], [140, 26, 20], [95, 18, 14]],           // 2 blood
			[[200, 60, 44], [170, 46, 34], [130, 34, 26], [90, 24, 18]],           // 3 gib
			[[130, 126, 122], [104, 102, 100], [80, 79, 78], [56, 56, 57]],        // 4 smoke
			[[255, 252, 220], [255, 236, 160], [255, 190, 90], [200, 120, 50]],    // 5 flash
			[[150, 255, 170], [110, 235, 200], [90, 190, 250], [80, 120, 220]]     // 6 hexmote
		];
		for (auto& ramp : ramps) {
			for (auto& rgb : ramp) { colors.push(rgb_idx(rgb[0], rgb[1], rgb[2])); }
		}
	}

private:
	int cursor = 0;

	float spread(float scale) {
		return (RNG.nextf() - 0.5) * 2.0 * scale;
	}
}
