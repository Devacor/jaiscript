// PURE parallel bodies. Everything in this file must pass the parallel
// admission walk (docs/site/guide/16-parallel.html): each function touches ONLY
// its one element parameter (its fields included) and locals: no globals, no
// host calls beyond the math whitelist. The same functions run serially when
// HOST_WORKERS <= 0 (the default: at GLOOM's grain the region barrier costs
// more than it distributes), which keeps the 1-vs-N determinism check: explicit
// --workers N runs the identical bodies through the parallel machinery.

// A CHUNK of wall rays over a PERSISTENT workspace (parallel_for v1, SAFE
// mode): a WallChunk class record (render.jai) the body owns exclusively —
// pose header + map snapshot + preallocated typed out columns; per frame only
// the pose header changes (main thread) and the body rewrites the out arrays
// in place. pack = tile snapshot, 15 cells x 4 bits per int64 (bits 60+
// unused: no sign-bit traps under checked overflow).
function gloom_ray_chunk_step(var& t) -> void {
	int x0 = t.x0;
	int x1 = t.x1;
	int vw = t.vw;
	float posx = t.posx;
	float posy = t.posy;
	float dirx = t.dirx;
	float diry = t.diry;
	float planex = t.planex;
	float planey = t.planey;
	int mw = t.mw;
	int oi = 0;
	float xstep = 2.0 / (vw - 1.0);
	for (int x = x0; x < x1; ++x) {
		float camx = x * xstep - 1.0;
		float rdx = dirx + planex * camx;
		float rdy = diry + planey * camx;
		int mx = posx;
		int my = posy;
		float adx = rdx < 0.0 ? -rdx : rdx;
		float ady = rdy < 0.0 ? -rdy : rdy;
		float ddx = adx < 0.00000001 ? 100000000.0 : 1.0 / adx;
		float ddy = ady < 0.00000001 ? 100000000.0 : 1.0 / ady;
		int stepx = rdx < 0.0 ? -1 : 1;
		int stepy = rdy < 0.0 ? -1 : 1;
		float sdx = rdx < 0.0 ? (posx - mx) * ddx : (mx + 1.0 - posx) * ddx;
		float sdy = rdy < 0.0 ? (posy - my) * ddy : (my + 1.0 - posy) * ddy;
		int side = 0;
		int kind = 1;
		int guard = 0;
		bool hit = false;
		while (!hit && guard < 128) {
			if (sdx < sdy) { sdx += ddx; mx += stepx; side = 0; }
			else { sdy += ddy; my += stepy; side = 1; }
			int cell = my * mw + mx;
			int word = t.pack[cell / 15];
			int c = (word >> ((cell % 15) * 4)) & 15;
			if (c != 0) {
				hit = true;
				kind = c;
			}
			guard = guard + 1;
		}
		float pdist = side == 0 ? sdx - ddx : sdy - ddy;
		pdist = pdist < 0.02 ? 0.02 : pdist;
		float wallx = side == 0 ? posy + pdist * rdy : posx + pdist * rdx;
		wallx = wallx - math::floor(wallx);
		int texcol = wallx * 64.0;
		t.odist[oi] = pdist;
		t.oside[oi] = side;
		t.okind[oi] = kind;
		t.otex[oi] = texcol;
		oi = oi + 1;
	}
}

// One wall ray (the serial helper for hitscan/tracers; ray_record builds its
// element). Same DDA as the chunk body. Element: [posx, posy, rdx, rdy, mw,
// pack0, ...]. Returns [perp_dist, side, kind, texcol 0..63].
function gloom_ray(var t) -> var {
	float posx = t[0];
	float posy = t[1];
	float rdx = t[2];
	float rdy = t[3];
	int mw = t[4];
	int mx = posx;                    // typed-local truncation; coords stay positive
	int my = posy;
	float adx = rdx < 0.0 ? -rdx : rdx;
	float ady = rdy < 0.0 ? -rdy : rdy;
	float ddx = adx < 0.00000001 ? 100000000.0 : 1.0 / adx;
	float ddy = ady < 0.00000001 ? 100000000.0 : 1.0 / ady;
	int stepx = rdx < 0.0 ? -1 : 1;
	int stepy = rdy < 0.0 ? -1 : 1;
	float sdx = rdx < 0.0 ? (posx - mx) * ddx : (mx + 1.0 - posx) * ddx;
	float sdy = rdy < 0.0 ? (posy - my) * ddy : (my + 1.0 - posy) * ddy;
	int side = 0;
	int kind = 1;
	int guard = 0;
	bool hit = false;
	while (!hit && guard < 128) {
		if (sdx < sdy) { sdx += ddx; mx += stepx; side = 0; }
		else { sdy += ddy; my += stepy; side = 1; }
		int cell = my * mw + mx;
		int word = t[5 + cell / 15];
		int c = (word >> ((cell % 15) * 4)) & 15;
		if (c != 0) {
			hit = true;
			kind = c;
		}
		guard = guard + 1;
	}
	float pdist = side == 0 ? sdx - ddx : sdy - ddy;
	pdist = pdist < 0.02 ? 0.02 : pdist;
	float wallx = side == 0 ? posy + pdist * rdy : posx + pdist * rdx;
	wallx = wallx - math::floor(wallx);
	int texcol = wallx * 64.0;
	return [pdist, side, kind, texcol];
}

// One particle step at the fixed timestep (TICK baked in as a literal — the
// admission walk forbids reading enclosing state, and the sim is fixed-step
// anyway). Element: a Particle class record (particles.jai).
// z is height in [0..~1.2], 0 = floor. Pure ballistic/drag updates; collision
// with WALLS is intentionally skipped (particles are garnish, walls clip them
// visually via the z-buffer at render time).
// IN PLACE (parallel_for v1, SAFE mode): mutates its owned element through the
// by-ref parameter — no output array, no per-frame pool replacement.
function gloom_particle_step(var& p) -> void {
	int kind = p.kind;
	if (kind == 0) { return; }
	float x = p.x;
	float y = p.y;
	float z = p.z;
	float vx = p.vx;
	float vy = p.vy;
	float vz = p.vz;
	int life = p.life;
	life = life - 1;
	if (life <= 0) {
		p.kind = 0;
		p.x = 0.0;
		p.y = 0.0;
		p.z = 0.0;
		p.vx = 0.0;
		p.vy = 0.0;
		p.vz = 0.0;
		p.life = 0;
		return;
	}
	if (kind == 1) {                       // spark: fast, hard gravity
		vz = vz - 0.30;
		vx = vx * 0.90;
		vy = vy * 0.90;
	} else if (kind == 2) {                // blood: droops
		vz = vz - 0.22;
		vx = vx * 0.94;
		vy = vy * 0.94;
	} else if (kind == 3) {                // gib: ballistic, bounces once-ish
		vz = vz - 0.16;
	} else if (kind == 4) {                // smoke: rises, drifts
		vz = vz * 0.90 + 0.012;
		vx = vx * 0.96;
		vy = vy * 0.96;
	} else if (kind == 5) {                // muzzle/impact flash: hangs still
		vx = vx * 0.5;
		vy = vy * 0.5;
	} else {                               // hexmote: swirly float
		vz = vz * 0.98 + 0.004;
		vx = vx * 0.97;
		vy = vy * 0.97;
	}
	x = x + vx * 0.033333333;
	y = y + vy * 0.033333333;
	z = z + vz * 0.033333333;
	if (z < 0.02) {
		z = 0.02;
		if (kind == 3 && (vz < -0.5 || vz > 0.5)) { vz = 0.0 - vz * 0.45; }
		else { vz = 0.0; }
		if (kind == 1 || kind == 2) { life = life < 4 ? life : 4; }
	}
	p.x = x;
	p.y = y;
	p.z = z;
	p.vx = vx;
	p.vy = vy;
	p.vz = vz;
	p.life = life;
}
