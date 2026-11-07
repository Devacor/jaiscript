// Gator Quest - mobiles: the flat pool, the occupancy grid, movement and
// passability, line of sight, spawners, and the event queue every system
// reports through (the presentation layer and the wire read only events).

import "gq_world.jai";

// log colors (the client maps these to its palette)
int C_WHITE = 0;
int C_GRAY = 1;
int C_RED = 2;
int C_GREEN = 3;
int C_YELLOW = 4;
int C_CYAN = 5;
int C_PURPLE = 6;
int C_ORANGE = 7;
int C_BLUE = 8;

// ---------------------------------------------------------------- events ---
void ev(int kind, int w, int x, int y, int a, int b, int c, int to, string text) {
	EV_KIND.push(kind);
	EV_W.push(w);
	EV_X.push(x);
	EV_Y.push(y);
	EV_A.push(a);
	EV_B.push(b);
	EV_C.push(c);
	EV_TO.push(to);
	EV_TEXT.push(text);
}

void events_clear() {
	EV_KIND = [];
	EV_W = [];
	EV_X = [];
	EV_Y = [];
	EV_A = [];
	EV_B = [];
	EV_C = [];
	EV_TO = [];
	EV_TEXT = [];
}

// a line in one player's journal log
void say_to(int slot, string text, int color) {
	if (slot < 0) { return; }
	ev(Ev.log, -1, 0, 0, color, 0, 0, slot, text);
}

// a line everyone near a spot reads
void say_near(int w, int x, int y, string text, int color) {
	ev(Ev.log, w, x, y, color, 0, 0, -1, text);
}

void say_mob(int m, string text, int color) {
	int slot = MOB_PLAYER[m];
	if (slot >= 0) { say_to(slot, text, color); }
}

void fx_at(int w, int x, int y, int fx, int count) { ev(Ev.fx, w, x, y, fx, count, 0, -1, ""); }
void sound_at(int w, int x, int y, string name) { ev(Ev.sound, w, x, y, 0, 0, 0, -1, name); }
void shake(int slot, int power) { if (slot >= 0) { ev(Ev.shake, -1, 0, 0, power, 0, 0, slot, ""); } }

// ------------------------------------------------------------------ pool ---
int mob_alloc() {
	for (int i = 0; i < MOB_KIND.size(); ++i) {
		if (MOB_KIND[i] < 0) { return i; }
	}
	return mob_push();
}

// a new free slot at the end of the pool
int mob_push() {
	int i = MOB_KIND.size();
	MOB_KIND.push(-1);
	MOB_SER.push(0);
	MOB_WORLD.push(0);
	MOB_X.push(0);
	MOB_Y.push(0);
	MOB_PX.push(0);
	MOB_PY.push(0);
	MOB_MOVE_T.push(0);
	MOB_MOVE_D.push(1);
	MOB_DIR.push(4);
	MOB_HP.push(0);
	MOB_HPMAX.push(1);
	MOB_MP.push(0);
	MOB_MPMAX.push(0);
	MOB_TARGET.push(-1);
	MOB_AI.push(0);
	MOB_NEXT.push(0);
	MOB_ATK.push(0);
	MOB_CAST.push(0);
	MOB_FLAGS.push(0);
	MOB_PLAYER.push(-1);
	MOB_SPAWNER.push(-1);
	MOB_HOMEX.push(0);
	MOB_HOMEY.push(0);
	MOB_POISON.push(0);
	MOB_BURN.push(0);
	MOB_SLOW.push(0);
	MOB_BUFF.push(0);
	MOB_HIT_T.push(-100);
	MOB_OWNER.push(-1);
	MOB_AUX.push(0);
	MOB_ROLE.push(-1);
	MOB_LEVEL.push(0);
	return i;
}

int occ_index(int w, int x, int y) { return W_OFF[w] + y * W_W[w] + x; }

int mob_at(int w, int x, int y) {
	if (!in_world(w, x, y)) { return -1; }
	return OCC[occ_index(w, x, y)] - 1;
}

void occ_set(int m) {
	int w = MOB_WORLD[m];
	if (in_world(w, MOB_X[m], MOB_Y[m])) { OCC[occ_index(w, MOB_X[m], MOB_Y[m])] = m + 1; }
}

void occ_clear(int m) {
	int w = MOB_WORLD[m];
	if (in_world(w, MOB_X[m], MOB_Y[m])) {
		int i = occ_index(w, MOB_X[m], MOB_Y[m]);
		if (OCC[i] == m + 1) { OCC[i] = 0; }
	}
}

bool mob_live(int m) { return m >= 0 && m < MOB_KIND.size() && MOB_KIND[m] >= 0; }
bool mob_ghost(int m) { return (MOB_FLAGS[m] & MF_GHOST) != 0; }
bool mob_alive(int m) { return mob_live(m) && MOB_HP[m] > 0 && !mob_ghost(m); }
bool mob_is_player(int m) { return MOB_PLAYER[m] >= 0; }
int mob_fac(int m) { return MOB_PLAYER[m] >= 0 ? Fac.player : MON_FAC[MOB_KIND[m]]; }

string mob_name(int m) {
	if (!mob_live(m)) { return "something"; }
	int slot = MOB_PLAYER[m];
	if (slot >= 0) { return CHARS[slot].name; }
	int role = MOB_ROLE[m];
	if (role >= 0) { return ROLES[role].name; }
	string n = MONS[MOB_KIND[m]].name;
	if ((MOB_FLAGS[m] & MF_ELITE) != 0) { n = "Elite " + n; }
	return n;
}

// "the Swamp Rat" / "Gus" / "Chomps"
string mob_the(int m) {
	if (!mob_live(m)) { return "something"; }
	if (MOB_PLAYER[m] >= 0 || MOB_ROLE[m] >= 0 || MONS[MOB_KIND[m]].boss) { return mob_name(m); }
	return "the " + mob_name(m);
}

int mob_spawn(int kind, int w, int x, int y) {
	int m = mob_alloc();
	MonDef d = MONS[kind];
	MOB_KIND[m] = kind;
	MOB_SER[m] = NEXT_SER;
	NEXT_SER = NEXT_SER + 1;
	MOB_WORLD[m] = w;
	MOB_X[m] = x;
	MOB_Y[m] = y;
	MOB_PX[m] = x;
	MOB_PY[m] = y;
	MOB_MOVE_T[m] = TICK;
	MOB_MOVE_D[m] = 1;
	MOB_DIR[m] = rnd(8);
	MOB_HP[m] = d.hp;
	MOB_HPMAX[m] = d.hp;
	MOB_MP[m] = 100;
	MOB_MPMAX[m] = 100;
	MOB_TARGET[m] = -1;
	MOB_AI[m] = 0;
	MOB_NEXT[m] = TICK + 1 + rnd(4);
	MOB_ATK[m] = TICK;
	MOB_CAST[m] = TICK + 20 + rnd(20);
	MOB_FLAGS[m] = d.boss ? MF_BOSS : 0;
	if (d.fac == Fac.npc || d.fac == Fac.guard) { MOB_FLAGS[m] = MOB_FLAGS[m] | MF_INVULN; }
	MOB_PLAYER[m] = -1;
	MOB_SPAWNER[m] = -1;
	MOB_HOMEX[m] = x;
	MOB_HOMEY[m] = y;
	MOB_POISON[m] = 0;
	MOB_BURN[m] = 0;
	MOB_SLOW[m] = 0;
	MOB_BUFF[m] = 0;
	MOB_HIT_T[m] = -100;
	MOB_OWNER[m] = -1;
	MOB_AUX[m] = 0;
	MOB_ROLE[m] = -1;
	MOB_LEVEL[m] = d.tier;
	occ_set(m);
	MOB_LIVE = MOB_LIVE + 1;
	STAT_SPAWNS = STAT_SPAWNS + 1;
	return m;
}

void mob_free(int m) {
	if (!mob_live(m)) { return; }
	occ_clear(m);
	int sp = MOB_SPAWNER[m];
	if (sp >= 0 && sp < SP_ALIVE.size() && SP_ALIVE[sp] > 0) { SP_ALIVE[sp] = SP_ALIVE[sp] - 1; }
	MOB_KIND[m] = -1;
	MOB_PLAYER[m] = -1;
	MOB_TARGET[m] = -1;
	MOB_LIVE = MOB_LIVE - 1;
	// forget anyone targeting the freed slot
	for (int i = 0; i < MOB_KIND.size(); ++i) {
		if (MOB_TARGET[i] == m) { MOB_TARGET[i] = -1; }
		if (MOB_OWNER[i] == m) { MOB_OWNER[i] = -1; }
	}
}

// ------------------------------------------------------------- movement ---
int mob_move_class(int m) {
	if (MOB_PLAYER[m] >= 0) { return 3; }
	return MON_MOVE[MOB_KIND[m]];
}

// can this mobile stand on (x, y)? players may step into a sky gap (and fall)
bool mob_can_enter(int m, int x, int y) {
	int w = MOB_WORLD[m];
	if (!in_world(w, x, y)) { return false; }
	int k = walk_at(w, x, y);
	int mv = mob_move_class(m);
	if (k == 0) { return false; }
	if (mv == 0) { return false; }
	if (k == 2) {
		if (mv == 1) { return false; }
	} else if (k == 3) {
		// only flyers cross open sky, and a boss never leaves solid ground (players must be able to reach it)
		if (MOB_PLAYER[m] < 0 && (mv != 4 || (MOB_FLAGS[m] & MF_BOSS) != 0)) { return false; }
	} else {
		if (mv == 2) { return false; }
		if (k == 5 && MOB_PLAYER[m] < 0 && !MONS[MOB_KIND[m]].fireproof) { return false; }
	}
	int o = OCC[occ_index(w, x, y)] - 1;
	if (o >= 0 && o != m && !mob_ghost(o) && !mob_ghost(m)) { return false; }
	return true;
}

// ticks one step takes for this mobile onto (x, y)
int step_ticks(int m, int x, int y) {
	int w = MOB_WORLD[m];
	int k = walk_at(w, x, y);
	int base = 2;
	if (MOB_PLAYER[m] < 0) { base = MON_SPEED[MOB_KIND[m]]; }
	if (MOB_PLAYER[m] >= 0) {
		Character& c = CHARS[MOB_PLAYER[m]];
		if (k == 2) {
			int swim = c.skill[Sk.swimming] + player_bonus(MOB_PLAYER[m], 16) * 10;
			base = swim >= 400 ? 1 : 2;
		} else if (k == 6) { base = 3; }
		if (mob_ghost(m)) { base = 2; }
	} else if (k == 6 && MON_MOVE[MOB_KIND[m]] != 4) { base = base + 1; }
	if (MOB_SLOW[m] > 0) { base = base * 2; }
	return base;
}

void mob_place(int m, int w, int x, int y) {
	occ_clear(m);
	MOB_WORLD[m] = w;
	MOB_X[m] = x;
	MOB_Y[m] = y;
	MOB_PX[m] = x;
	MOB_PY[m] = y;
	MOB_MOVE_T[m] = TICK;
	MOB_MOVE_D[m] = 1;
	occ_set(m);
}

bool mob_step(int m, int dir) {
	int nx = MOB_X[m] + DX[dir];
	int ny = MOB_Y[m] + DY[dir];
	MOB_DIR[m] = dir;
	if (!mob_can_enter(m, nx, ny)) { return false; }
	// no corner cutting through walls on diagonals
	if (DX[dir] != 0 && DY[dir] != 0) {
		int w = MOB_WORLD[m];
		if (walk_at(w, MOB_X[m] + DX[dir], MOB_Y[m]) == 0 && walk_at(w, MOB_X[m], MOB_Y[m] + DY[dir]) == 0) { return false; }
	}
	int d = step_ticks(m, nx, ny);
	occ_clear(m);
	MOB_PX[m] = MOB_X[m];
	MOB_PY[m] = MOB_Y[m];
	MOB_X[m] = nx;
	MOB_Y[m] = ny;
	MOB_MOVE_T[m] = TICK;
	MOB_MOVE_D[m] = d;
	MOB_NEXT[m] = TICK + d;
	occ_set(m);
	int w = MOB_WORLD[m];
	if (walk_at(w, nx, ny) == 2) { MOB_FLAGS[m] = MOB_FLAGS[m] | MF_SWIM; }
	else { MOB_FLAGS[m] = MOB_FLAGS[m] & ~MF_SWIM; }
	return true;
}

// greedy step toward (tx, ty), trying the two neighbouring directions when blocked
bool mob_step_toward(int m, int tx, int ty) {
	int dx = tx - MOB_X[m];
	int dy = ty - MOB_Y[m];
	if (dx == 0 && dy == 0) { return false; }
	int d = dir_of(dx, dy);
	if (mob_step(m, d)) { return true; }
	int alt = (TICK + m) % 2 == 0 ? 1 : 7;
	if (mob_step(m, (d + alt) % 8)) { return true; }
	if (mob_step(m, (d + 8 - alt) % 8)) { return true; }
	if (mob_step(m, (d + 2) % 8)) { return true; }
	return mob_step(m, (d + 6) % 8);
}

bool mob_step_away(int m, int tx, int ty) {
	int dx = MOB_X[m] - tx;
	int dy = MOB_Y[m] - ty;
	if (dx == 0 && dy == 0) { dx = 1; }
	int d = dir_of(dx, dy);
	if (mob_step(m, d)) { return true; }
	if (mob_step(m, (d + 1) % 8)) { return true; }
	if (mob_step(m, (d + 7) % 8)) { return true; }
	if (mob_step(m, (d + 2) % 8)) { return true; }
	return mob_step(m, (d + 6) % 8);
}

// Bresenham sight line; the end tiles do not block
bool los(int w, int x0, int y0, int x1, int y1) {
	int dx = iabs(x1 - x0);
	int dy = iabs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx - dy;
	int x = x0;
	int y = y0;
	int guard = 0;
	while ((x != x1 || y != y1) && guard < 64) {
		guard = guard + 1;
		int e2 = err * 2;
		if (e2 > -dy) { err = err - dy; x = x + sx; }
		if (e2 < dx) { err = err + dx; y = y + sy; }
		if ((x != x1 || y != y1) && T_OPAQUE[tile_now(w, x, y)] != 0) { return false; }
	}
	return true;
}

// is any live player within `r` tiles of (w, x, y)?
int nearest_player(int w, int x, int y, int r) {
	int best = -1;
	int bestd = r + 1;
	for (int s = 0; s < CHARS.size(); ++s) {
		if (!CHARS[s].active) { continue; }
		int m = CHARS[s].mob;
		if (!mob_live(m) || MOB_WORLD[m] != w) { continue; }
		int d = cheb(x, y, MOB_X[m], MOB_Y[m]);
		if (d < bestd) { bestd = d; best = m; }
	}
	return best;
}

// nearest tile of the walk classes in `mask` that nobody stands on
array<int> find_free_spot(int w, int x0, int y0, int maxr, int mask) {
	for (int r = 0; r <= maxr; ++r) {
		for (int dy = -r; dy <= r; ++dy) {
			for (int dx = -r; dx <= r; ++dx) {
				if (iabs(dx) != r && iabs(dy) != r) { continue; }
				int x = x0 + dx;
				int y = y0 + dy;
				if (!in_world(w, x, y) || OCC[occ_index(w, x, y)] != 0) { continue; }
				if (((1 << walk_at(w, x, y)) & mask) != 0) { return [x, y]; }
			}
		}
	}
	return find_spot(w, x0, y0, maxr, mask);
}

bool player_near(int w, int x, int y, int r) {
	if (CAMERA_W == w && cheb(x, y, CAMERA_X, CAMERA_Y) <= r) { return true; }
	return nearest_player(w, x, y, r) >= 0;
}

// ------------------------------------------------------------- spawners ---
void spawners_tick() {
	if (TICK % 10 != 0) { return; }
	for (int s = 0; s < SP_W.size(); ++s) {
		if (SP_ALIVE[s] >= SP_MAX[s] || TICK < SP_NEXT[s]) { continue; }
		int w = SP_W[s];
		int kind = SP_KIND[s];
		bool boss = MONS[kind].boss;
		if (boss && TICK < W_BOSS_T[w]) { continue; }
		int x = SP_X[s] + rnd_range(-SP_RAD[s], SP_RAD[s]);
		int y = SP_Y[s] + rnd_range(-SP_RAD[s], SP_RAD[s]);
		if (boss) { x = SP_X[s]; y = SP_Y[s]; }
		if (!in_world(w, x, y)) { continue; }
		int k = walk_at(w, x, y);
		int mv = MON_MOVE[kind];
		bool ok = mv == 4 ? (k != 0) : (mv == 2 ? k == 2 : (k == 1 || k == 6 || k == 4 || (k == 2 && mv == 3)));
		if (!ok || mob_at(w, x, y) >= 0) {
			SP_NEXT[s] = TICK + 20;
			continue;
		}
		// never pop out of thin air right next to a player (bosses excepted)
		if (!boss && player_near(w, x, y, 5)) {
			SP_NEXT[s] = TICK + 30;
			continue;
		}
		int m = mob_spawn(kind, w, x, y);
		MOB_SPAWNER[m] = s;
		MOB_HOMEX[m] = SP_X[s];
		MOB_HOMEY[m] = SP_Y[s];
		// one in fifteen tier-2+ spawns is an elite: tougher, shinier, better loot
		if (!boss && MONS[kind].tier >= 2 && MONS[kind].fac == Fac.monster && rnd(15) == 0) {
			MOB_FLAGS[m] = MOB_FLAGS[m] | MF_ELITE;
			MOB_HPMAX[m] = MOB_HPMAX[m] * 2;
			MOB_HP[m] = MOB_HPMAX[m];
		}
		SP_ALIVE[s] = SP_ALIVE[s] + 1;
		SP_NEXT[s] = TICK + SP_DELAY[s];
		if (boss) {
			ev(Ev.banner, w, x, y, kind, 0, 0, -1, MONS[kind].name);
		}
	}
}

// npcs from the worldgen plan, one-time
void populate_npcs() {
	for (int i = 0; i < NPC_PLAN_W.size(); ++i) {
		int w = NPC_PLAN_W[i];
		array<int> p = find_spot(w, NPC_PLAN_X[i], NPC_PLAN_Y[i], 3, LAND_MASK);
		int m = mob_spawn(NPC_PLAN_KIND[i], w, p[0], p[1]);
		MOB_ROLE[m] = NPC_PLAN_ROLE[i];
		if (NPC_PLAN_ROLE[i] >= 0) { MOB_AI[m] = 1; }   // shopkeepers stay behind the counter
	}
}
