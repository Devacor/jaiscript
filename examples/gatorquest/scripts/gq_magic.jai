// Gator Quest - the spellbook: reagents + mana + a Magery check, sixteen
// spells from Heal to Meteor Swarm. Monsters cast from the same table for free.

import "gq_ai.jai";

int SPELL_RANGE = 8;

int proj_for_fx(int fx) {
	if (fx == Fx.fire) { return 2; }
	if (fx == Fx.frost) { return 3; }
	if (fx == Fx.zap) { return 4; }
	if (fx == Fx.poison) { return 5; }
	return 1;
}

// the player's intended victim: current target if valid, else the nearest visible hostile
int spell_target(int m, int range) {
	int t = MOB_TARGET[m];
	int w = MOB_WORLD[m];
	if (mob_alive(t) && MOB_WORLD[t] == w && hostile(m, t) && cheb(MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]) <= range
		&& los(w, MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t])) { return t; }
	int best = -1;
	int bestd = range + 1;
	for (int dy = -range; dy <= range; ++dy) {
		for (int dx = -range; dx <= range; ++dx) {
			int o = mob_at(w, MOB_X[m] + dx, MOB_Y[m] + dy);
			if (o < 0 || !hostile(m, o) || !mob_alive(o)) { continue; }
			int d = cheb(0, 0, dx, dy);
			if (d < bestd && los(w, MOB_X[m], MOB_Y[m], MOB_X[o], MOB_Y[o])) { best = o; bestd = d; }
		}
	}
	return best;
}

int spell_power(int caster, int lo, int hi) {
	int v = rnd_range(lo, hi);
	int slot = MOB_PLAYER[caster];
	if (slot >= 0) {
		v = v * (100 + CHARS[slot].skill[Sk.magery] / 10 + player_int(slot) / 4 + player_bonus(slot, 20) * 3) / 100;
	} else {
		v = v * (100 + MONS[MOB_KIND[caster]].tier * 15) / 100;
	}
	return imax(1, v);
}

void spell_hit(int caster, int t, SpellDef s) {
	int dmg = spell_power(caster, s.lo, s.hi);
	int w = MOB_WORLD[t];
	fx_at(w, MOB_X[t], MOB_Y[t], s.fx, 10);
	if (s.eff == 5) {
		deal(t, dmg, caster, DMG_FIRE, false);
		if (mob_alive(t)) { MOB_BURN[t] = imax(MOB_BURN[t], 16); }
	} else if (s.eff == 4) {
		deal(t, dmg, caster, DMG_COLD, false);
		if (mob_alive(t)) { MOB_SLOW[t] = imax(MOB_SLOW[t], 40); }
	} else if (s.eff == 7) {
		if (mob_alive(t)) { MOB_POISON[t] = imax(MOB_POISON[t], dmg); }
		deal(t, 2, caster, DMG_POISON, false);
	} else {
		deal(t, dmg, caster, DMG_ENERGY, false);
	}
}

// apply a spell; returns false when there was nothing to aim at (no mana spent)
bool spell_effect(int caster, int spell, int t) {
	SpellDef s = SPELLS[spell];
	int w = MOB_WORLD[caster];
	int cx = MOB_X[caster];
	int cy = MOB_Y[caster];
	int slot = MOB_PLAYER[caster];
	if (s.target == 1) {
		if (!mob_alive(t)) { return false; }
		ev(Ev.proj, w, cx, cy, MOB_X[t], MOB_Y[t], proj_for_fx(s.fx), -1, "");
		spell_hit(caster, t, s);
		return true;
	}
	if (s.target == 2) {
		if (!mob_alive(t)) { return false; }
		int tx = MOB_X[t];
		int ty = MOB_Y[t];
		ev(Ev.proj, w, cx, cy, tx, ty, proj_for_fx(s.fx), -1, "");
		fx_at(w, tx, ty, s.fx, 24);
		for (int dy = -s.radius; dy <= s.radius; ++dy) {
			for (int dx = -s.radius; dx <= s.radius; ++dx) {
				int o = mob_at(w, tx + dx, ty + dy);
				if (o >= 0 && mob_alive(o) && hostile(caster, o)) {
					if (o != t) { ev(Ev.proj, w, tx, ty, MOB_X[o], MOB_Y[o], proj_for_fx(s.fx), -1, ""); }
					spell_hit(caster, o, s);
				}
			}
		}
		if (slot >= 0) { shake(slot, 3); }
		return true;
	}
	if (s.target == 0) {
		int who = caster;
		if (mob_alive(t) && !hostile(caster, t) && cheb(cx, cy, MOB_X[t], MOB_Y[t]) <= SPELL_RANGE) { who = t; }
		if (s.eff == 3) {
			MOB_POISON[who] = 0;
			say_mob(who, "The poison leaves you.", C_GREEN);
		} else {
			heal_mob(who, spell_power(caster, s.lo, s.hi), caster);
		}
		fx_at(w, MOB_X[who], MOB_Y[who], s.fx, 12);
		return true;
	}
	if (s.target == 8 && slot >= 0) {
		CHARS[slot].buff_night = TICK + s.lo;
		say_to(slot, "Your eyes adjust to the dark.", C_YELLOW);
		fx_at(w, cx, cy, s.fx, 8);
		return true;
	}
	if (s.target == 5) {
		int dir = MOB_DIR[caster];
		int bx = cx;
		int by = cy;
		for (int k = 1; k <= s.lo; ++k) {
			int nx = cx + DX[dir] * k;
			int ny = cy + DY[dir] * k;
			int wk = walk_at(w, nx, ny);
			if (wk == 0) { break; }
			if (wk != 3 && mob_at(w, nx, ny) < 0) { bx = nx; by = ny; }
		}
		if (bx == cx && by == cy) { return false; }
		fx_at(w, cx, cy, Fx.warp, 12);
		mob_place(caster, w, bx, by);
		fx_at(w, bx, by, Fx.warp, 12);
		return true;
	}
	if (s.target == 6 && slot >= 0) {
		Character& c = CHARS[slot];
		fx_at(w, cx, cy, Fx.warp, 16);
		array<int> p = find_spot(c.home_world, c.home_x, c.home_y, 5, LAND_MASK);
		travel(caster, c.home_world, p[0], p[1]);
		return true;
	}
	if (s.target == 4) {
		for (int d = 0; d < 8; ++d) {
			int nx = cx + DX[d];
			int ny = cy + DY[d];
			int wk = walk_at(w, nx, ny);
			if ((wk == 1 || wk == 6 || wk == 2 || wk == 4) && mob_at(w, nx, ny) < 0) {
				int f = mob_spawn(M_FAMILIAR, w, nx, ny);
				MOB_OWNER[f] = caster;
				MOB_FLAGS[f] = MOB_FLAGS[f] | MF_SUMMON;
				MOB_AUX[f] = TICK + 1800;
				fx_at(w, nx, ny, Fx.magic, 14);
				say_near(w, nx, ny, "A frog familiar pops into existence! \"Ribbit!\"", C_GREEN);
				return true;
			}
		}
		return false;
	}
	if (s.target == 7) {
		for (int s2 = 0; s2 < CHARS.size(); ++s2) {
			int p = CHARS[s2].mob;
			if (CHARS[s2].active && mob_live(p) && mob_ghost(p) && MOB_WORLD[p] == w && cheb(cx, cy, MOB_X[p], MOB_Y[p]) <= 3) {
				player_resurrect(s2, 50);
				return true;
			}
		}
		return false;
	}
	return false;
}

bool player_cast(int slot, int spell) {
	Character& c = CHARS[slot];
	int m = c.mob;
	if (spell < 0 || spell >= SPELLS.size()) { return false; }
	if (mob_ghost(m)) { say_to(slot, "Ghosts cannot cast. Find a healer.", C_GRAY); return false; }
	if (c.spells.size() <= spell || c.spells[spell] == 0) { say_to(slot, "You do not know that spell.", C_GRAY); return false; }
	if (!pack_has(slot, I_SPELLBOOK, 1)) { say_to(slot, "You need a spellbook.", C_GRAY); return false; }
	SpellDef s = SPELLS[spell];
	if (MOB_MP[m] < s.mana) { say_to(slot, "Insufficient mana.", C_BLUE); return false; }
	for (int r = 0; r < s.reg.size(); ++r) {
		if (!pack_has(slot, s.reg[r], 1)) {
			say_to(slot, `You lack ${ITEMS[s.reg[r]].name} for ${s.name}.`, C_GRAY);
			return false;
		}
	}
	int t = -1;
	if (s.target == 1 || s.target == 2) {
		t = spell_target(m, SPELL_RANGE);
		if (t < 0) { say_to(slot, "No target in sight.", C_GRAY); return false; }
		MOB_TARGET[m] = t;
	} else if (s.target == 0) {
		t = MOB_TARGET[m];
	}
	c.last_spell = spell;
	for (int r = 0; r < s.reg.size(); ++r) { pack_take(slot, s.reg[r], 1); }
	int difficulty = s.circle * 125 - 150;
	if (!skill_check(slot, Sk.magery, difficulty)) {
		MOB_MP[m] = MOB_MP[m] - s.mana / 2;
		say_to(slot, "The spell fizzles.", C_GRAY);
		fx_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], Fx.smoke, 5);
		sound_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], "fizzle");
		return true;
	}
	if (!spell_effect(m, spell, t)) {
		say_to(slot, "The spell finds nothing to hold on to.", C_GRAY);
		return true;
	}
	MOB_MP[m] = MOB_MP[m] - s.mana;
	sound_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], s.snd);
	c.version = c.version + 1;
	return true;
}

void mob_cast(int m, int spell, int t) {
	SpellDef s = SPELLS[spell];
	say_near(MOB_WORLD[m], MOB_X[m], MOB_Y[m], `${cap_first(mob_the(m))} casts ${s.name}!`, C_PURPLE);
	sound_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], s.snd);
	spell_effect(m, spell, t);
}

// moving between places (gates, recall, falling): arrival fx on both ends
void travel(int m, int w, int x, int y) {
	int from = MOB_WORLD[m];
	fx_at(from, MOB_X[m], MOB_Y[m], Fx.warp, 12);
	array<int> p = find_free_spot(w, x, y, 6, LAND_MASK + (MOB_PLAYER[m] >= 0 ? 4 : 0));
	mob_place(m, w, p[0], p[1]);
	fx_at(w, p[0], p[1], Fx.warp, 16);
	sound_at(w, p[0], p[1], "warp");
	int slot = MOB_PLAYER[m];
	if (slot >= 0) {
		Character& c = CHARS[slot];
		step_queue_flush(slot);
		if (from != w) {
			ev(Ev.warp, w, p[0], p[1], w, from, 0, slot, WORLDS[w].name);
			say_to(slot, WORLDS[w].name + ": " + WORLDS[w].banner, C_YELLOW);
			while (c.visited.size() < WORLD_COUNT) { c.visited.push(0); }
			if (c.visited[w] == 0) {
				c.visited[w] = 1;
				quest_on_visit(slot);
			}
		}
		c.action = 0;
		c.version = c.version + 1;
		// summons follow their caster through gates
		for (int i = 0; i < MOB_KIND.size(); ++i) {
			if (MOB_KIND[i] >= 0 && MOB_OWNER[i] == m && (MOB_FLAGS[i] & MF_SUMMON) != 0) {
				array<int> q = find_free_spot(w, p[0] + 1, p[1], 3, LAND_MASK + 4);
				mob_place(i, w, q[0], q[1]);
			}
		}
	}
}
