// Gator Quest - client-side prediction. The local gator moves on the key press:
// the client knows the terrain (same seed) and what its snapshots show, so it
// takes the step itself, sends it with a sequence number, and keeps it pending.
// Each snapshot says which step the host processed last and where the gator is;
// the client rewinds to that, replays the pending steps, and if the result
// differs from what it drew, glides there (small) or snaps (large). Everyone
// else is drawn a little behind a smoothed server clock, from their last two
// moves, so they glide instead of stepping at the 10 Hz snapshot rate.

import "gq_audio.jai";

// ---------------------------------------------------------------- rules ---
// the host's player movement rules, as the client can know them
// 0 free, 1 wall (turn only), 2 somebody is there (the host bumps: attack or talk)
int pred_blocked(ClientView& v, int x0, int y0, int dir) {
	int x = x0 + DX[dir];
	int y = y0 + DY[dir];
	int w = v.world;
	if (!in_world(w, x, y)) { return 1; }
	int t = cv_tile(v, x, y);
	if (T_WALK[t] == 0) { return 1; }
	if (DX[dir] != 0 && DY[dir] != 0) {
		if (T_WALK[cv_tile(v, x0 + DX[dir], y0)] == 0 && T_WALK[cv_tile(v, x0, y0 + DY[dir])] == 0) { return 1; }
	}
	if (v.ghost != 0) { return 0; }
	PRED_BUMP = -1;
	for (int i = 0; i < v.m_ser.size(); ++i) {
		if (v.m_ser[i] == v.my_ser || v.m_x[i] != x || v.m_y[i] != y || (v.m_flags[i] & MF_GHOST) != 0) { continue; }
		PRED_BUMP = i;
		return 2;
	}
	return 0;
}

int pred_swim(ClientView& v) {
	int s = v.skills.size() > Sk.swimming ? v.skills[Sk.swimming] : 0;
	for (int i = 0; i < v.worn.size(); ++i) {
		if (v.worn[i].base >= 0) { s = s + item_stat(v.worn[i], 16) * 10; }
	}
	return s;
}

// ticks one step onto (x, y) takes, as the host's step_ticks computes it for a player
int pred_step_ticks(ClientView& v, int x, int y) {
	int k = T_WALK[cv_tile(v, x, y)];
	int base = 2;
	if (k == 2) { base = pred_swim(v) >= 400 ? 1 : 2; }
	else if (k == 6) { base = 3; }
	if (v.ghost != 0) { base = 2; }
	if (v.slowed) { base = base * 2; }
	return base;
}

// ---------------------------------------------------------------- state ---
void pred_reset() {
	PRED_SEQ = 0;
	PEND_SEQ = [];
	PEND_DIR = [];
	PRED_W = -1;
	PRED_READY = 0;
	PRED_LUNGE_T = -1000;
	PRED_WALL_DIR = -1;
	CORR_X = 0.0;
	CORR_Y = 0.0;
	PRED_CORRECTIONS = 0;
	PRED_SNAPS = 0;
	PRED_CORR_MAX = 0.0;
	PRED_STEPS = 0;
	CLOCK_SET = false;
	CLOCK_SEEN = -1;
}

bool pred_active(ClientView& v) { return PRED_ON && PRED_W >= 0 && PRED_W == v.world && v.has_auth; }

void pred_snap(ClientView& v, int now) {
	PRED_W = v.world;
	PRED_X = v.auth_x;
	PRED_Y = v.auth_y;
	PRED_FX = PRED_X;
	PRED_FY = PRED_Y;
	PRED_T0 = now - 1000;
	PRED_DUR = 1;
	PRED_READY = now;
	PEND_SEQ = [];
	PEND_DIR = [];
	CORR_X = 0.0;
	CORR_Y = 0.0;
	int me = cv_me(v);
	if (me >= 0) { PRED_DIR = v.m_dir[me]; }
}

float pred_lerp_t(int now) {
	float t = (now - PRED_T0) / (PRED_DUR * 1.0);
	return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

float pred_lunge(int now, bool x) {
	int age = now - PRED_LUNGE_T;
	if (age < 0 || age > 160) { return 0.0; }
	float s = math::sin(3.14159 * age / 160.0) * 0.3;
	return x ? DX[PRED_DIR] * s : DY[PRED_DIR] * s;
}

// where the local gator is drawn (tiles): the predicted step, the correction still gliding out, a lunge
float pred_draw_x(int now) { return PRED_FX + (PRED_X - PRED_FX) * pred_lerp_t(now) + CORR_X + pred_lunge(now, true); }
float pred_draw_y(int now) { return PRED_FY + (PRED_Y - PRED_FY) * pred_lerp_t(now) + CORR_Y + pred_lunge(now, false); }

// ----------------------------------------------------------------- input ---
void pred_send(int seq, int dir) {
	if (NET_CLIENT) { net_client_send(`S|${seq}|${dir}`); }
	else { intent_step(LOCAL_SLOT, seq, dir); }
}

void pred_footstep(ClientView& v, int x, int y) {
	int t = cv_tile(v, x, y);
	int k = T_WALK[t];
	float fx = x + 0.5;
	float fy = y + 0.9;
	if (k == 2 || t == T_SHALLOW || t == T_SWAMP || t == T_BOG || t == T_MUD) {
		for (int n = 0; n < 4; ++n) { particle(fx + (prandf() - 0.5) * 0.6, fy - 0.2, (prandf() - 0.5) * 0.05, -0.05 - prandf() * 0.05, 10 + prand(8), prand(2) == 0 ? rgb(200, 230, 255) : rgb(110, 170, 220), 2); }
	} else if (t == T_SNOW || t == T_ICE) {
		for (int n = 0; n < 3; ++n) { particle(fx + (prandf() - 0.5) * 0.6, fy, (prandf() - 0.5) * 0.03, -0.02, 14, rgb(240, 245, 255), 4); }
	} else if (t == T_DIRT || t == T_SAND || t == T_ROAD || t == T_ASH || t == T_CAVEFLOOR) {
		int c = t == T_SAND ? rgb(230, 210, 160) : (t == T_ASH ? rgb(120, 110, 110) : rgb(160, 130, 95));
		for (int n = 0; n < 2; ++n) { particle(fx + (prandf() - 0.5) * 0.5, fy, (prandf() - 0.5) * 0.03, -0.015, 12 + prand(6), c, 4); }
	}
}

// called every frame with the held direction (-1 none); a step happens on this frame when it can
void pred_input(ClientView& v, int dir, int now) {
	if (!pred_active(v) || dir < 0 || v.sliding) {
		if (dir < 0) { PRED_WALL_DIR = -1; }
		return;
	}
	PRED_DIR = dir;
	if (now < PRED_READY) { return; }
	int r = pred_blocked(v, PRED_X, PRED_Y, dir);
	if (r == 1) {
		// walking into a wall only turns; the host hears about the new facing once
		if (dir != PRED_WALL_DIR) {
			PRED_WALL_DIR = dir;
			PRED_SEQ = PRED_SEQ + 1;
			pred_send(PRED_SEQ, dir);
			PEND_SEQ.push(PRED_SEQ);
			PEND_DIR.push(dir);
		}
		PRED_READY = now + 100;
		return;
	}
	PRED_WALL_DIR = -1;
	PRED_SEQ = PRED_SEQ + 1;
	pred_send(PRED_SEQ, dir);
	PEND_SEQ.push(PRED_SEQ);
	PEND_DIR.push(dir);
	if (r == 0) {
		int nx = PRED_X + DX[dir];
		int ny = PRED_Y + DY[dir];
		pred_footstep(v, PRED_X, PRED_Y);
		PRED_FX = PRED_X;
		PRED_FY = PRED_Y;
		PRED_X = nx;
		PRED_Y = ny;
		PRED_T0 = now;
		PRED_DUR = pred_step_ticks(v, nx, ny) * TICK_MS;
		PRED_READY = now + PRED_DUR;
		PRED_STEPS = PRED_STEPS + 1;
		return;
	}
	// a bump: the host attacks or talks; the swing is shown now, the hit when the host says so
	PRED_LUNGE_T = now;
	PRED_READY = now + 2 * TICK_MS;
	int fac = MONS[v.m_kind[PRED_BUMP]].fac;
	if (v.m_slot[PRED_BUMP] < 0 && (fac == Fac.monster || fac == Fac.animal)) { sfx("swing"); }
}

// ------------------------------------------------------------ reconcile ---
// a snapshot arrived: rewind to the host's position, replay what it has not processed yet
void pred_reconcile(ClientView& v, int now) {
	if (!v.auth_fresh) { return; }
	v.auth_fresh = false;
	if (!PRED_ON || !v.has_auth) { return; }
	if (PRED_W != v.world || PRED_W < 0) {
		pred_snap(v, now);
		return;
	}
	while (PEND_SEQ.size() > 0 && PEND_SEQ[0] <= v.ack) {
		PEND_SEQ.remove(0);
		PEND_DIR.remove(0);
	}
	int x = v.auth_x;
	int y = v.auth_y;
	for (int k = 0; k < PEND_DIR.size(); ++k) {
		if (pred_blocked(v, x, y, PEND_DIR[k]) == 0) {
			x = x + DX[PEND_DIR[k]];
			y = y + DY[PEND_DIR[k]];
		}
	}
	if (x == PRED_X && y == PRED_Y) { return; }
	int d = cheb(x, y, PRED_X, PRED_Y);
	float drawn_x = pred_draw_x(now) - pred_lunge(now, true);
	float drawn_y = pred_draw_y(now) - pred_lunge(now, false);
	PRED_CORRECTIONS = PRED_CORRECTIONS + 1;
	if (d > PRED_CORR_MAX) { PRED_CORR_MAX = d * 1.0; }
	PRED_X = x;
	PRED_Y = y;
	PRED_FX = x;
	PRED_FY = y;
	PRED_T0 = now - PRED_DUR;
	if (d > 3) {
		// a teleport or a long disagreement: no glide across the map
		PRED_SNAPS = PRED_SNAPS + 1;
		CORR_X = 0.0;
		CORR_Y = 0.0;
		PEND_SEQ = [];
		PEND_DIR = [];
	} else {
		// keep drawing where we were and glide the difference out over a few frames
		CORR_X = drawn_x - x;
		CORR_Y = drawn_y - y;
	}
}

// once per frame: the correction glides out, the server clock advances
void pred_frame(ClientView& v, int now) {
	CORR_X = CORR_X * 0.72;
	CORR_Y = CORR_Y * 0.72;
	if (CORR_X < 0.02 && CORR_X > -0.02) { CORR_X = 0.0; }
	if (CORR_Y < 0.02 && CORR_Y > -0.02) { CORR_Y = 0.0; }
	clock_frame(v, now);
}

// ---------------------------------------------------------- server clock ---
// a clock that runs at real time and is nudged toward each snapshot's tick, so late or bunched
// snapshots do not make the world jump; others are drawn INTERP_DELAY ticks behind it
void clock_frame(ClientView& v, int now) {
	if (v.snaps == 0) { return; }
	if (!CLOCK_SET) {
		CLOCK_TICK = v.tick * 1.0;
		CLOCK_MS = now;
		CLOCK_SET = true;
		CLOCK_SEEN = v.snaps;
		return;
	}
	CLOCK_TICK = CLOCK_TICK + (now - CLOCK_MS) / (TICK_MS * 1.0);
	CLOCK_MS = now;
	if (v.snaps != CLOCK_SEEN) {
		CLOCK_SEEN = v.snaps;
		float err = v.tick - CLOCK_TICK;
		if (err > 3.0 || err < -3.0) { CLOCK_TICK = v.tick * 1.0; }
		else { CLOCK_TICK = CLOCK_TICK + err * 0.15; }
	}
	if (CLOCK_TICK > v.tick + 1.5) { CLOCK_TICK = v.tick + 1.5; }
}

float render_tick(ClientView& v) {
	if (CLOCK_SET) { return CLOCK_TICK - INTERP_DELAY; }
	return view_tick(v);
}
