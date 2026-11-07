// Gator Quest - monster and npc brains. One think per mobile when its
// timer comes due; monsters far from every player sleep (the realm keeps its
// population, it just stops burning ticks nobody can see).

import "gq_combat.jai";

int AGGRO = 7;
int LEASH = 22;

// best hostile target for m within r tiles it can see
int find_target(int m, int r) {
	int w = MOB_WORLD[m];
	int x = MOB_X[m];
	int y = MOB_Y[m];
	int best = -1;
	int bestd = 999;
	// players (and their summons) are the usual prey; scan the pool only near the mobile
	for (int s = 0; s < CHARS.size(); ++s) {
		if (!CHARS[s].active) { continue; }
		int p = CHARS[s].mob;
		if (!mob_alive(p) || MOB_WORLD[p] != w) { continue; }
		int reach = (MOB_FLAGS[p] & MF_HIDDEN) != 0 ? 2 : r;
		int d = cheb(x, y, MOB_X[p], MOB_Y[p]);
		if (d > reach || d >= bestd || !hostile(m, p)) { continue; }
		if (!los(w, x, y, MOB_X[p], MOB_Y[p])) { continue; }
		best = p;
		bestd = d;
	}
	// guards and summons also hunt monsters, monsters also fight summons
	int fac = mob_fac(m);
	if (fac == Fac.guard || (MOB_FLAGS[m] & MF_SUMMON) != 0 || fac == Fac.monster) {
		for (int dy = -r; dy <= r; ++dy) {
			for (int dx = -r; dx <= r; ++dx) {
				int o = mob_at(w, x + dx, y + dy);
				if (o < 0 || o == m || MOB_PLAYER[o] >= 0) { continue; }
				int d = cheb(0, 0, dx, dy);
				if (d >= bestd || !hostile(m, o)) { continue; }
				if (fac == Fac.monster && (MOB_FLAGS[o] & MF_SUMMON) == 0 && mob_fac(o) != Fac.guard) { continue; }
				best = o;
				bestd = d;
			}
		}
	}
	return best;
}

bool target_ok(int m) {
	int t = MOB_TARGET[m];
	if (!mob_alive(t) || MOB_WORLD[t] != MOB_WORLD[m]) { return false; }
	if (!hostile(m, t)) { return false; }
	return cheb(MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]) <= LEASH;
}

void think_later(int m, int ticks) { MOB_NEXT[m] = TICK + imax(1, ticks); }

// the melee core: close in, swing when adjacent and ready
void chase_and_swing(int m, int t) {
	int d = cheb(MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]);
	if (d <= 1) {
		if (TICK >= MOB_ATK[m]) { attack(m, t); }
		think_later(m, 2);
		return;
	}
	if (!mob_step_toward(m, MOB_X[t], MOB_Y[t])) { think_later(m, 3); }
}

void wander(int m, int radius) {
	if (rnd(3) != 0) { think_later(m, 6 + rnd(10)); return; }
	int hx = MOB_HOMEX[m];
	int hy = MOB_HOMEY[m];
	if (cheb(MOB_X[m], MOB_Y[m], hx, hy) > radius) {
		if (!mob_step_toward(m, hx, hy)) { think_later(m, 5); }
		return;
	}
	if (!mob_step(m, rnd(8))) { think_later(m, 4); }
}

void go_home_or_wander(int m) {
	if (cheb(MOB_X[m], MOB_Y[m], MOB_HOMEX[m], MOB_HOMEY[m]) > 8) {
		if (!mob_step_toward(m, MOB_HOMEX[m], MOB_HOMEY[m])) { think_later(m, 4); }
		return;
	}
	wander(m, 6);
}

void alert_pack(int m, int t) {
	int w = MOB_WORLD[m];
	int kind = MOB_KIND[m];
	for (int dy = -6; dy <= 6; ++dy) {
		for (int dx = -6; dx <= 6; ++dx) {
			int o = mob_at(w, MOB_X[m] + dx, MOB_Y[m] + dy);
			if (o >= 0 && o != m && MOB_KIND[o] == kind && MOB_TARGET[o] < 0) {
				MOB_TARGET[o] = t;
				MOB_FLAGS[o] = MOB_FLAGS[o] | MF_ALERT;
			}
		}
	}
}

// acquire or keep a target; -1 = nothing to fight
int engage(int m, int r) {
	if (target_ok(m)) { return MOB_TARGET[m]; }
	MOB_TARGET[m] = -1;
	int t = find_target(m, r);
	if (t >= 0) {
		MOB_TARGET[m] = t;
		MOB_FLAGS[m] = MOB_FLAGS[m] | MF_ALERT;
		string quip = MONS[MOB_KIND[m]].quip;
		if (quip != "" && rnd(3) == 0) { say_near(MOB_WORLD[m], MOB_X[m], MOB_Y[m], `${cap_first(mob_the(m))}: "${quip}"`, C_ORANGE); }
		string snd = MONS[MOB_KIND[m]].snd;
		if (snd != "") { sound_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], snd); }
		if (MON_AI[MOB_KIND[m]] == Ai.pack) { alert_pack(m, t); }
	}
	return t;
}

void ai_melee(int m) {
	int t = engage(m, AGGRO);
	if (t < 0) { go_home_or_wander(m); return; }
	chase_and_swing(m, t);
}

// archers keep their distance: 3..range
void ai_ranged(int m) {
	int t = engage(m, AGGRO + 1);
	if (t < 0) { go_home_or_wander(m); return; }
	int d = cheb(MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]);
	int rng = MONS[MOB_KIND[m]].range;
	bool sight = los(MOB_WORLD[m], MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]);
	if (d < 3 && rnd(3) != 0) {
		if (mob_step_away(m, MOB_X[t], MOB_Y[t])) { return; }
	}
	if (d <= rng && sight) {
		if (TICK >= MOB_ATK[m]) { attack(m, t); }
		think_later(m, 3);
		return;
	}
	if (!mob_step_toward(m, MOB_X[t], MOB_Y[t])) { think_later(m, 3); }
}

// casters open with spells from range, then keep a respectful distance
void ai_caster(int m, bool coward) {
	int t = engage(m, AGGRO + 1);
	if (t < 0) { go_home_or_wander(m); return; }
	int d = cheb(MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]);
	if (coward && MOB_HP[m] * 10 < MOB_HPMAX[m] * 4 && rnd(4) != 0) {
		MOB_FLAGS[m] = MOB_FLAGS[m] | MF_FLEEING;
		if (mob_step_away(m, MOB_X[t], MOB_Y[t])) { return; }
	}
	int spell = MONS[MOB_KIND[m]].spell;
	bool sight = los(MOB_WORLD[m], MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]);
	if (spell >= 0 && TICK >= MOB_CAST[m] && d <= 7 && sight) {
		mob_cast(m, spell, t);
		MOB_CAST[m] = TICK + 30 + rnd(30);
		think_later(m, 6);
		return;
	}
	if (d < 3) {
		if (d <= 1 && TICK >= MOB_ATK[m] && !coward) { attack(m, t); think_later(m, 3); return; }
		if (mob_step_away(m, MOB_X[t], MOB_Y[t])) { return; }
	}
	if (d > 5 || !sight) {
		if (!mob_step_toward(m, MOB_X[t], MOB_Y[t])) { think_later(m, 3); }
		return;
	}
	// strafe a little so they are not sitting ducks
	if (rnd(2) == 0 && mob_step(m, rnd(8))) { return; }
	think_later(m, 3);
}

// raccoons: grab gold (MOB_AUX keeps the purse), run off laughing, slowed by the loot
void ai_thief(int m) {
	if ((MOB_FLAGS[m] & MF_FLEEING) != 0) {
		int p = nearest_player(MOB_WORLD[m], MOB_X[m], MOB_Y[m], 12);
		if (p < 0 || MOB_AUX[m] == 0 && rnd(20) == 0) { MOB_FLAGS[m] = MOB_FLAGS[m] & ~MF_FLEEING; think_later(m, 5); return; }
		if (mob_step_away(m, MOB_X[p], MOB_Y[p])) { MOB_NEXT[m] = MOB_NEXT[m] + 1; }
		else { think_later(m, 2); }
		return;
	}
	int t = engage(m, AGGRO);
	if (t < 0) { go_home_or_wander(m); return; }
	int d = cheb(MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]);
	if (d <= 1 && TICK >= MOB_ATK[m]) {
		int slot = MOB_PLAYER[t];
		if (slot >= 0 && CHARS[slot].gold > 0 && rnd(4) == 0) {
			int grab = imin(CHARS[slot].gold, 3 + rnd(8));
			CHARS[slot].gold = CHARS[slot].gold - grab;
			CHARS[slot].version = CHARS[slot].version + 1;
			MOB_AUX[m] = MOB_AUX[m] + grab;
			say_to(slot, `${cap_first(mob_the(m))} swipes ${grab} gold and scampers off! "Yoink!"`, C_ORANGE);
			sound_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], "coin");
			MOB_FLAGS[m] = MOB_FLAGS[m] | MF_FLEEING;
			MOB_ATK[m] = TICK + 12;
			think_later(m, 1);
			return;
		}
		attack(m, t);
		think_later(m, 2);
		return;
	}
	if (!mob_step_toward(m, MOB_X[t], MOB_Y[t])) { think_later(m, 3); }
}

void ai_ambush(int m) {
	if (MOB_TARGET[m] < 0 && (MOB_FLAGS[m] & MF_ALERT) == 0) {
		MOB_FLAGS[m] = MOB_FLAGS[m] | MF_HIDDEN;
		int p = nearest_player(MOB_WORLD[m], MOB_X[m], MOB_Y[m], 2);
		if (p >= 0 && hostile(m, p)) {
			MOB_FLAGS[m] = (MOB_FLAGS[m] | MF_ALERT) & ~MF_HIDDEN;
			MOB_TARGET[m] = p;
			say_near(MOB_WORLD[m], MOB_X[m], MOB_Y[m], `${cap_first(mob_the(m))} erupts from the murk!`, C_RED);
			fx_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], Fx.splash, 14);
			sound_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], "splash");
			shake(MOB_PLAYER[p], 4);
		}
		think_later(m, 4);
		return;
	}
	ai_melee(m);
}

void ai_critter(int m) {
	int p = nearest_player(MOB_WORLD[m], MOB_X[m], MOB_Y[m], 3);
	if (p >= 0 || MOB_HP[m] < MOB_HPMAX[m]) {
		if (p >= 0 && mob_step_away(m, MOB_X[p], MOB_Y[p])) { return; }
	}
	wander(m, 6);
}

void ai_wander_npc(int m) {
	if (MOB_AI[m] == 1) {
		// shopkeepers face whoever comes near and otherwise mind the counter
		int p = nearest_player(MOB_WORLD[m], MOB_X[m], MOB_Y[m], 4);
		if (p >= 0) { MOB_DIR[m] = dir_of(MOB_X[p] - MOB_X[m], MOB_Y[p] - MOB_Y[m]); }
		think_later(m, 10);
		return;
	}
	wander(m, 5);
}

void ai_guard(int m) {
	int t = engage(m, 10);
	if (t < 0) {
		if (cheb(MOB_X[m], MOB_Y[m], MOB_HOMEX[m], MOB_HOMEY[m]) > 2) {
			if (!mob_step_toward(m, MOB_HOMEX[m], MOB_HOMEY[m])) { think_later(m, 5); }
			return;
		}
		think_later(m, 8);
		return;
	}
	chase_and_swing(m, t);
}

void ai_summon(int m) {
	int owner = MOB_OWNER[m];
	if (!mob_alive(owner) || TICK >= MOB_AUX[m]) {
		fx_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], Fx.magic, 8);
		say_near(MOB_WORLD[m], MOB_X[m], MOB_Y[m], "The frog familiar croaks farewell and vanishes.", C_GREEN);
		mob_free(m);
		return;
	}
	int t = MOB_TARGET[m];
	int ot = MOB_TARGET[owner];
	if (mob_alive(ot) && hostile(m, ot)) { t = ot; MOB_TARGET[m] = ot; }
	if (!target_ok(m)) { t = find_target(m, 6); MOB_TARGET[m] = t; }
	if (t >= 0) { chase_and_swing(m, t); return; }
	if (cheb(MOB_X[m], MOB_Y[m], MOB_X[owner], MOB_Y[owner]) > 2) {
		if (MOB_WORLD[m] != MOB_WORLD[owner] || cheb(MOB_X[m], MOB_Y[m], MOB_X[owner], MOB_Y[owner]) > 14) {
			array<int> p = find_free_spot(MOB_WORLD[owner], MOB_X[owner], MOB_Y[owner] + 1, 3, LAND_MASK + 4);
			mob_place(m, MOB_WORLD[owner], p[0], p[1]);
			return;
		}
		if (!mob_step_toward(m, MOB_X[owner], MOB_Y[owner])) { think_later(m, 3); }
		return;
	}
	think_later(m, 4);
}

// bosses: a caster-brawler with an enrage, summoned adds and a telegraphed stomp
void ai_boss(int m) {
	int t = engage(m, AGGRO + 3);
	int kind = MOB_KIND[m];
	MonDef md = MONS[kind];
	int w = MOB_WORLD[m];
	if (t < 0) {
		if (MOB_HP[m] < MOB_HPMAX[m] && TICK % 5 == 0) { MOB_HP[m] = imin(MOB_HPMAX[m], MOB_HP[m] + MOB_HPMAX[m] / 50); }
		go_home_or_wander(m);
		return;
	}
	if (MOB_AUX[m] == 0 && MOB_HP[m] * 2 < MOB_HPMAX[m]) {
		MOB_AUX[m] = 1;
		say_near(w, MOB_X[m], MOB_Y[m], `${md.name} flies into a RAGE!`, C_RED);
		sound_at(w, MOB_X[m], MOB_Y[m], "roar");
		fx_at(w, MOB_X[m], MOB_Y[m], Fx.fire, 20);
		for (int s = 0; s < CHARS.size(); ++s) {
			int p = CHARS[s].mob;
			if (CHARS[s].active && mob_live(p) && MOB_WORLD[p] == w && cheb(MOB_X[p], MOB_Y[p], MOB_X[m], MOB_Y[m]) < 12) { shake(s, 6); }
		}
	}
	bool enraged = MOB_AUX[m] != 0;
	// the stomp: telegraphed one think ahead with a log line
	if (MOB_ROLE[m] == -2) {
		MOB_ROLE[m] = -1;
		fx_at(w, MOB_X[m], MOB_Y[m], Fx.dust, 26);
		sound_at(w, MOB_X[m], MOB_Y[m], "boom");
		for (int s = 0; s < CHARS.size(); ++s) {
			int p = CHARS[s].mob;
			if (CHARS[s].active && mob_alive(p) && MOB_WORLD[p] == w && cheb(MOB_X[p], MOB_Y[p], MOB_X[m], MOB_Y[m]) <= 2) {
				deal(p, md.lo + rnd(md.hi - md.lo + 1), m, DMG_PHYS, false);
				shake(s, 8);
			}
		}
		think_later(m, 4);
		return;
	}
	if (TICK % 90 < 3 && cheb(MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]) <= 2) {
		MOB_ROLE[m] = -2;
		say_near(w, MOB_X[m], MOB_Y[m], `${md.name} rears up for a crushing stomp!`, C_YELLOW);
		think_later(m, 6);
		return;
	}
	// adds
	if (md.summon >= 0 && TICK >= MOB_CAST[m] - 10 && TICK % 150 < 4) {
		int adds = 0;
		for (int i = 0; i < MOB_KIND.size(); ++i) {
			if (MOB_KIND[i] == md.summon && MOB_OWNER[i] == m) { adds = adds + 1; }
		}
		if (adds < 5) {
			for (int k = 0; k < 2; ++k) {
				int dir = rnd(8);
				int nx = MOB_X[m] + DX[dir] * 2;
				int ny = MOB_Y[m] + DY[dir] * 2;
				int kk = walk_at(w, nx, ny);
				if (mob_at(w, nx, ny) < 0 && kk != 0 && kk != 3) {
					int a = mob_spawn(md.summon, w, nx, ny);
					MOB_OWNER[a] = m;
					MOB_TARGET[a] = t;
					fx_at(w, nx, ny, Fx.smoke, 8);
				}
			}
			say_near(w, MOB_X[m], MOB_Y[m], `${md.name} calls for help!`, C_ORANGE);
		}
	}
	int d = cheb(MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]);
	if (md.spell >= 0 && TICK >= MOB_CAST[m] && d >= 2 && d <= 8 && los(w, MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t])) {
		mob_cast(m, md.spell, t);
		MOB_CAST[m] = TICK + (enraged ? 25 : 45);
		think_later(m, 5);
		return;
	}
	if (d <= 1) {
		if (TICK >= MOB_ATK[m]) {
			attack(m, t);
			if (enraged) { MOB_ATK[m] = MOB_ATK[m] - 2; }
		}
		think_later(m, 2);
		return;
	}
	if (!mob_step_toward(m, MOB_X[t], MOB_Y[t])) { think_later(m, 2); }
	else if (enraged) { MOB_NEXT[m] = MOB_NEXT[m] - 1; }
}

void ai_tick() {
	int n = MOB_KIND.size();
	for (int m = 0; m < n; ++m) {
		int kind = MOB_KIND[m];
		if (kind < 0 || MOB_PLAYER[m] >= 0 || MOB_HP[m] <= 0 || TICK < MOB_NEXT[m]) { continue; }
		int ai = MON_AI[kind];
		if (ai != Ai.summon && !player_near(MOB_WORLD[m], MOB_X[m], MOB_Y[m], 24)) {
			MOB_NEXT[m] = TICK + 15;
			continue;
		}
		MOB_NEXT[m] = TICK + 1;
		if (ai == Ai.melee || ai == Ai.pack) { ai_melee(m); }
		else if (ai == Ai.ranged) { ai_ranged(m); }
		else if (ai == Ai.caster) { ai_caster(m, false); }
		else if (ai == Ai.coward) { ai_caster(m, true); }
		else if (ai == Ai.thief) { ai_thief(m); }
		else if (ai == Ai.ambush) { ai_ambush(m); }
		else if (ai == Ai.boss) { ai_boss(m); }
		else if (ai == Ai.critter) { ai_critter(m); }
		else if (ai == Ai.wander) { ai_wander_npc(m); }
		else if (ai == Ai.guard) { ai_guard(m); }
		else if (ai == Ai.summon) { ai_summon(m); }
		else { think_later(m, 10); }
	}
}
