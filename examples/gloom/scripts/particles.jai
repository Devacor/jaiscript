// Particle pool: a fixed-size array of typed-field Particle records, stepped
// IN PLACE by the pure gloom_particle_step body (pure.jai) through
// parallel_for (safe mode: all-primitive flat records prove exclusive at the
// barrier). The pool object owns slot recycling; access labels keep the
// cursor honest.

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

class ParticlePool {
public:
	array<Particle> pool = [];

	ParticlePool() { reset_pool(); }

	// reset_pool, not reset: the builtin shared_ptr reset() shadows same-named
	// class methods, so fx.reset() silently no-ops (stale cursor = frames 3424+
	// diverged from the ports while STATE_HASH stayed blind - its particle fold
	// is additive, slot-order-invariant)
	void reset_pool() {
		pool = [];
		for (int i = 0; i < PART_MAX; ++i) {
			pool.push(new Particle());
		}
		cursor = 0;
	}

	void spawn(int kind, float x, float y, float z, float vx, float vy, float vz, int life) {
		pool[cursor] = new Particle(kind, x, y, z, vx, vy, vz, life);
		cursor = (cursor + 1) % PART_MAX;
	}

	// the whole pool steps through ONE pure in-place function — parallel_for owns
	// each element exclusively when workers are explicitly requested (SAFE mode:
	// elements are all-primitive rows, proven at the barrier), the same function
	// in a serial range-for otherwise. No output array, no pool replacement.
	void update() {
		if (HOST_WORKERS <= 0) {
			for (auto& p : pool) { gloom_particle_step(p); }
		} else {
			parallel_for (auto& p : pool) { gloom_particle_step(p); }
		}
	}

private:
	int cursor = 0;
}

// ------------------------------------------------------------ burst kit -----
// All velocity spreads come from RNG (the seeded host rng): deterministic.
function rnd_spread(float scale) -> float {
	return (RNG.nextf() - 0.5) * 2.0 * scale;
}

function fx_muzzle(float x, float y, float dirx, float diry) -> void {
	float mx = x + dirx * 0.5;
	float my = y + diry * 0.5;
	G.fx.spawn(5, mx, my, 0.52, dirx * 2.0, diry * 2.0, 0.2, 3);
	for (int i = 0; i < 3; ++i) {
		G.fx.spawn(1, mx, my, 0.5, dirx * 3.0 + rnd_spread(1.6), diry * 3.0 + rnd_spread(1.6), 0.6 + RNG.nextf(), 6);
	}
	G.fx.spawn(4, mx, my, 0.55, dirx * 0.6, diry * 0.6, 0.3, 16);
}

function fx_wall_hit(float x, float y) -> void {
	for (int i = 0; i < 5; ++i) {
		G.fx.spawn(1, x, y, 0.45 + RNG.nextf() * 0.2, rnd_spread(2.2), rnd_spread(2.2), 0.8 + RNG.nextf() * 1.4, 8);
	}
	G.fx.spawn(4, x, y, 0.5, rnd_spread(0.3), rnd_spread(0.3), 0.4, 14);
}

function fx_blood(float x, float y, float z, int n) -> void {
	for (int i = 0; i < n; ++i) {
		G.fx.spawn(2, x, y, z + RNG.nextf() * 0.25, rnd_spread(1.8), rnd_spread(1.8), 0.5 + RNG.nextf() * 1.6, 12);
	}
}

// death burst: meat + smoke; big enemies gib harder
function fx_gibs(float x, float y, int n) -> void {
	for (int i = 0; i < n; ++i) {
		G.fx.spawn(3, x, y, 0.35 + RNG.nextf() * 0.3, rnd_spread(2.6), rnd_spread(2.6), 1.2 + RNG.nextf() * 2.4, 26);
	}
	for (int i = 0; i < 3; ++i) {
		G.fx.spawn(4, x, y, 0.4, rnd_spread(0.5), rnd_spread(0.5), 0.5, 20);
	}
}

function fx_explosion(float x, float y) -> void {
	G.fx.spawn(5, x, y, 0.5, 0.0, 0.0, 0.0, 5);
	for (int i = 0; i < 10; ++i) {
		float a = RNG.nextf() * 6.2831853;
		float sp = 1.5 + RNG.nextf() * 3.0;
		G.fx.spawn(1, x, y, 0.3 + RNG.nextf() * 0.5, math::cos(a) * sp, math::sin(a) * sp, 1.0 + RNG.nextf() * 2.0, 11);
	}
	for (int i = 0; i < 5; ++i) {
		G.fx.spawn(4, x, y, 0.4, rnd_spread(0.9), rnd_spread(0.9), 0.6 + RNG.nextf(), 24);
	}
	for (int i = 0; i < 6; ++i) {
		G.fx.spawn(6, x, y, 0.2 + RNG.nextf() * 0.6, rnd_spread(1.4), rnd_spread(1.4), 0.5, 20);
	}
}

function fx_door_puff(float x, float y) -> void {
	for (int i = 0; i < 4; ++i) {
		G.fx.spawn(4, x, y, 0.3 + RNG.nextf() * 0.5, rnd_spread(0.7), rnd_spread(0.7), 0.3, 18);
	}
}

function fx_secret_glitter(float x, float y) -> void {
	for (int i = 0; i < 8; ++i) {
		G.fx.spawn(6, x, y, 0.2 + RNG.nextf() * 0.8, rnd_spread(1.2), rnd_spread(1.2), 0.6, 24);
	}
}

// ------------------------------------------------------------- colors -------
// per-kind life-phase ramps, interned at boot (render reads
// PART_COLS[kind * 4 + phase]; phase 0 = fresh); the showcase look's are in look.jai
array<int> PART_COLS = [];

function build_particle_colors() -> void {
	array<array<array<int>>> ramps = [
		[[0, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0]],                          // 0 dead (unused)
		[[255, 240, 160], [255, 200, 90], [230, 130, 50], [140, 70, 30]],      // 1 spark
		[[210, 40, 30], [180, 34, 26], [140, 26, 20], [95, 18, 14]],           // 2 blood
		[[200, 60, 44], [170, 46, 34], [130, 34, 26], [90, 24, 18]],           // 3 gib
		[[130, 126, 122], [104, 102, 100], [80, 79, 78], [56, 56, 57]],        // 4 smoke
		[[255, 252, 220], [255, 236, 160], [255, 190, 90], [200, 120, 50]],    // 5 flash
		[[150, 255, 170], [110, 235, 200], [90, 190, 250], [80, 120, 220]]     // 6 hexmote
	];
	PART_COLS = [];
	if (SHOWCASE) {
		for (auto& ramp : SHOWCASE_PARTICLE_RAMPS) {
			for (auto& color : ramp) { PART_COLS.push(color_idx(color)); }
		}
		return;
	}
	for (auto& ramp : ramps) {
		for (auto& rgb : ramp) {
			PART_COLS.push(rgb_idx(rgb[0], rgb[1], rgb[2]));
		}
	}
}
