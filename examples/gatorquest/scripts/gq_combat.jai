// Gator Quest - combat and the skill system: skills that rise by
// use, hit/parry/crit resolution, damage types, death (loot, fame, slime
// splits, boss relics), ghosts and resurrection, poison/burn/slow, regen.

import "gq_items.jai";

int DMG_PHYS = 0;
int DMG_FIRE = 1;
int DMG_COLD = 2;
int DMG_POISON = 3;
int DMG_ENERGY = 4;

int SKILL_CAP = 7000;          // total skill tenths (700.0)
array<int> SKILL_STAT = [0, 0, 1, 0, 0, 1, 2, 2, 2, 1, 1, 0, 0, 0, 2, 2, 1, 1];   // 0 str, 1 dex, 2 int

int skill_total(int slot) {
	int t = 0;
	array<int>& sk = CHARS[slot].skill;
	for (int i = 0; i < sk.size(); ++i) { t = t + sk[i]; }
	return t;
}

// a use of a skill: maybe it rises, and sometimes the stat behind it
void skill_gain(int slot, int sk, bool success) {
	Character& c = CHARS[slot];
	int s = c.skill[sk];
	if (s >= 1000 || skill_total(slot) >= SKILL_CAP) { return; }
	int chance = (1050 - s) / 3 + (success ? 0 : 40);
	if (rnd(1000) >= chance) { return; }
	int amount = s < 300 ? 3 : (s < 600 ? 2 : 1);
	int before_rank = skill_rank(s);
	c.skill[sk] = imin(1000, s + amount);
	c.version = c.version + 1;
	say_to(slot, `Your ${SKILL_NAME[sk]} skill has increased by 0.${amount}. It is now ${tenths(c.skill[sk])}.`, C_CYAN);
	int m = c.mob;
	ev(Ev.gain, MOB_WORLD[m], MOB_X[m], MOB_Y[m], sk, c.skill[sk], 0, slot, "");
	if (skill_rank(c.skill[sk]) > before_rank && skill_rank(c.skill[sk]) >= 0) {
		say_to(slot, `You are now ${article(RANK_NAME[skill_rank(c.skill[sk])])} ${SKILL_TITLE[sk]}!`, C_YELLOW);
		fx_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], Fx.levelup, 16);
		sound_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], "levelup");
	}
	if (rnd(8) == 0) {
		int st = SKILL_STAT[sk];
		int total = c.str + c.dex + c.intel;
		if (total < 250) {
			if (st == 0 && c.str < 100) { c.str = c.str + 1; say_to(slot, "Your strength has increased.", C_CYAN); }
			else if (st == 1 && c.dex < 100) { c.dex = c.dex + 1; say_to(slot, "Your dexterity has increased.", C_CYAN); }
			else if (st == 2 && c.intel < 100) { c.intel = c.intel + 1; say_to(slot, "Your intelligence has increased.", C_CYAN); }
			player_refresh(slot);
		}
	}
}

// -1 below Neophyte; 0 Neophyte (30.0) ... 7 Grandmaster (100.0)
int skill_rank(int tenths_v) {
	if (tenths_v < 300) { return -1; }
	return imin(7, tenths_v / 100 - 3);
}

// succeed against a difficulty (skill tenths where the odds are even); gains either way
bool skill_check(int slot, int sk, int difficulty) {
	int s = CHARS[slot].skill[sk];
	int chance = iclamp(500 + (s - difficulty), 30, 990);
	bool ok = rnd(1000) < chance;
	skill_gain(slot, sk, ok);
	return ok;
}

string player_title(int slot) {
	Character& c = CHARS[slot];
	if (c.quest_stage.size() > Q_SIX_SCALES && c.quest_stage[Q_SIX_SCALES] == 99) { return "the Grand Gator"; }
	int best = 0;
	for (int i = 1; i < c.skill.size(); ++i) {
		if (c.skill[i] > c.skill[best]) { best = i; }
	}
	int r = skill_rank(c.skill[best]);
	if (r < 0) { return c.fame > 200 ? "the Hatchling Hero" : "the Hatchling"; }
	string fame = c.fame >= 2000 ? "Glorious " : (c.fame >= 600 ? "Famed " : "");
	return `the ${fame}${RANK_NAME[r]} ${SKILL_TITLE[best]}`;
}

// ---------------------------------------------------------------- damage ---
int mob_slot_owner(int m) {
	if (!mob_live(m)) { return -1; }
	if (MOB_PLAYER[m] >= 0) { return MOB_PLAYER[m]; }
	int o = MOB_OWNER[m];
	if (o >= 0 && mob_live(o) && MOB_PLAYER[o] >= 0) { return MOB_PLAYER[o]; }
	return -1;
}

// may `a` hurt `d`? players only fight players when both carry the PvP flag
bool hostile(int a, int d) {
	if (!mob_live(a) || !mob_live(d) || a == d) { return false; }
	if ((MOB_FLAGS[d] & MF_INVULN) != 0 || mob_ghost(d)) { return false; }
	int fa = mob_fac(a);
	int fd = mob_fac(d);
	bool ap = fa == Fac.player;
	bool dp = fd == Fac.player;
	if (ap && dp) {
		int sa = mob_slot_owner(a);
		int sd = mob_slot_owner(d);
		if (sa == sd) { return false; }
		if (sa >= 0 && sd >= 0 && CHARS[sa].party >= 0 && CHARS[sa].party == CHARS[sd].party) { return false; }
		return sa >= 0 && sd >= 0 && CHARS[sa].pvp && CHARS[sd].pvp;
	}
	if (fa == Fac.guard) { return fd == Fac.monster || (MOB_FLAGS[d] & MF_CRIMINAL) != 0; }
	if (fd == Fac.guard) { return fa == Fac.monster; }
	if (fa == Fac.npc || fd == Fac.npc) { return false; }
	return fa != fd;
}

void deal(int d, int amount, int a, int type, bool crit) {
	if (!mob_alive(d) || (MOB_FLAGS[d] & MF_INVULN) != 0) { return; }
	int w = MOB_WORLD[d];
	if (MOB_PLAYER[d] < 0) {
		MonDef md = MONS[MOB_KIND[d]];
		if (type == DMG_FIRE && md.fireproof) { amount = amount / 4; }
		if (type == DMG_COLD && md.frostproof) { amount = amount / 4; }
		if (type == DMG_POISON && md.undead) { amount = amount / 2; }
	} else {
		int slot = MOB_PLAYER[d];
		if (type == DMG_FIRE && CHARS[slot].buff_fire > TICK) { amount = amount / 3; }
		if (type != DMG_PHYS) {
			int resist = CHARS[slot].skill[Sk.resist] / 20 + player_bonus(slot, 13);
			amount = amount * 100 / (100 + resist);
			skill_gain(slot, Sk.resist, true);
		}
	}
	if (amount < 1) { amount = 1; }
	MOB_HP[d] = MOB_HP[d] - amount;
	MOB_HIT_T[d] = TICK;
	ev(Ev.dmg, w, MOB_X[d], MOB_Y[d], MOB_SER[d], amount, type + (crit ? 16 : 0), -1, "");
	if (MOB_PLAYER[d] >= 0) {
		shake(MOB_PLAYER[d], imin(6, 1 + amount * 12 / imax(1, MOB_HPMAX[d])));
		CHARS[MOB_PLAYER[d]].action = 0;    // a hit interrupts fishing, mining, crafting
	}
	// wake up and hit back
	if (mob_live(a) && MOB_PLAYER[d] < 0 && a != d) {
		if (MOB_TARGET[d] < 0 || rnd(3) == 0) { MOB_TARGET[d] = a; }
		MOB_FLAGS[d] = MOB_FLAGS[d] | MF_ALERT;
		MOB_FLAGS[d] = MOB_FLAGS[d] & ~MF_HIDDEN;
	}
	if (MOB_HP[d] <= 0) { kill(d, a); }
}

void heal_mob(int m, int amount, int src) {
	if (!mob_alive(m)) { return; }
	int before = MOB_HP[m];
	MOB_HP[m] = imin(MOB_HPMAX[m], MOB_HP[m] + amount);
	int got = MOB_HP[m] - before;
	if (got > 0) { ev(Ev.heal, MOB_WORLD[m], MOB_X[m], MOB_Y[m], MOB_SER[m], got, 0, -1, ""); }
}

// ---------------------------------------------------------------- attack ---
int weapon_skill(int slot) {
	Item wpn = CHARS[slot].worn[Slot.weapon];
	if (wpn.base < 0) { return Sk.wrestling; }
	return ITEMS[wpn.base].skill;
}

int attack_range(int m) {
	if (MOB_PLAYER[m] >= 0) {
		Item wpn = CHARS[MOB_PLAYER[m]].worn[Slot.weapon];
		return wpn.base < 0 ? 1 : ITEMS[wpn.base].range;
	}
	return MONS[MOB_KIND[m]].range;
}

int swing_ticks(int m) {
	if (MOB_PLAYER[m] >= 0) {
		int slot = MOB_PLAYER[m];
		Item wpn = CHARS[slot].worn[Slot.weapon];
		int base = wpn.base < 0 ? 6 : ITEMS[wpn.base].speed;
		base = base - player_bonus(slot, 8) - player_dex(slot) / 40;
		return imax(3, base);
	}
	return MONS[MOB_KIND[m]].aspeed;
}

int defense_skill(int d) {
	if (MOB_PLAYER[d] >= 0) {
		int slot = MOB_PLAYER[d];
		return imax(CHARS[slot].skill[weapon_skill(slot)], CHARS[slot].skill[Sk.wrestling]);
	}
	return MONS[MOB_KIND[d]].skill;
}

int proj_kind(int a) {
	if (MOB_PLAYER[a] >= 0) {
		Item wpn = CHARS[MOB_PLAYER[a]].worn[Slot.weapon];
		if (wpn.base >= 0 && ITEMS[wpn.base].skill == Sk.magery) { return 1; }
		return 0;
	}
	string spr = MONS[MOB_KIND[a]].spr;
	if (spr == "harpy") { return 6; }
	return 0;
}

// one swing (or shot) from a at d; the caller has checked range, cooldown and hostility
void attack(int a, int d) {
	int w = MOB_WORLD[a];
	MOB_ATK[a] = TICK + swing_ticks(a);
	MOB_DIR[a] = dir_of(MOB_X[d] - MOB_X[a], MOB_Y[d] - MOB_Y[a]);
	MOB_FLAGS[a] = MOB_FLAGS[a] & ~MF_HIDDEN;
	int slot = MOB_PLAYER[a];
	int rng = attack_range(a);
	bool ranged = rng > 1 && cheb(MOB_X[a], MOB_Y[a], MOB_X[d], MOB_Y[d]) > 1;
	int lo = 1;
	int hi = 3;
	int askill = 300;
	int sk = Sk.wrestling;
	int dmg_pct = 100;
	int crit = 50;
	int extra_fire = 0;
	int extra_cold = 0;
	int extra_poison = 0;
	int leech = 0;
	if (slot >= 0) {
		Item wpn = CHARS[slot].worn[Slot.weapon];
		sk = weapon_skill(slot);
		askill = CHARS[slot].skill[sk];
		if (wpn.base >= 0) {
			lo = ITEMS[wpn.base].lo + wpn.bonus;
			hi = ITEMS[wpn.base].hi + wpn.bonus;
		} else {
			lo = 2;
			hi = 6 + player_str(slot) / 15;      // the bite
		}
		if (ranged && sk == Sk.archery) {
			if (!pack_take(slot, I_ARROW, 1)) {
				say_to(slot, "You have no arrows!", C_RED);
				return;
			}
		}
		if (ranged && sk == Sk.magery) {
			if (MOB_MP[a] < 2) { say_to(slot, "Your wand needs mana.", C_RED); return; }
			MOB_MP[a] = MOB_MP[a] - 2;
		}
		dmg_pct = 100 + CHARS[slot].skill[Sk.tactics] / 10 + player_str(slot) / 4 + player_bonus(slot, 0);
		crit = 50 + player_bonus(slot, 17) * 10 + player_luck(slot) * 2;
		extra_fire = player_bonus(slot, 1);
		extra_cold = player_bonus(slot, 2);
		extra_poison = player_bonus(slot, 3);
		leech = player_bonus(slot, 4);
	} else {
		MonDef md = MONS[MOB_KIND[a]];
		lo = md.lo;
		hi = md.hi;
		askill = md.skill;
		leech = md.leech;
		if ((MOB_FLAGS[a] & MF_ELITE) != 0) { dmg_pct = 140; }
		if (md.poisonous && rnd(3) == 0) { extra_poison = 3; }
	}
	if (ranged) {
		ev(Ev.proj, w, MOB_X[a], MOB_Y[a], MOB_X[d], MOB_Y[d], proj_kind(a), -1, "");
	}
	int dskill = defense_skill(d);
	int hit = iclamp(500 + (askill - dskill) / 2 + (slot >= 0 ? player_dex(slot) : 0), 100, 950);
	if (rnd(1000) >= hit) {
		ev(Ev.miss, w, MOB_X[d], MOB_Y[d], MOB_SER[d], 0, 0, -1, "");
		sound_at(w, MOB_X[a], MOB_Y[a], ranged ? "bow" : "swing");
		if (slot >= 0) { skill_gain(slot, sk, false); }
		if (MOB_PLAYER[d] >= 0) { skill_gain(MOB_PLAYER[d], weapon_skill(MOB_PLAYER[d]), true); }
		if (MOB_PLAYER[d] < 0 && MOB_TARGET[d] < 0) { MOB_TARGET[d] = a; }
		return;
	}
	// a shield can turn the blow aside
	int ds = MOB_PLAYER[d];
	if (ds >= 0 && CHARS[ds].worn[Slot.offhand].base >= 0 && !ranged) {
		int parry = CHARS[ds].skill[Sk.parrying] / 4;
		if (rnd(1000) < parry) {
			say_to(ds, "You parry the blow!", C_GREEN);
			ev(Ev.miss, w, MOB_X[d], MOB_Y[d], MOB_SER[d], 1, 0, -1, "");
			sound_at(w, MOB_X[d], MOB_Y[d], "clang");
			fx_at(w, MOB_X[d], MOB_Y[d], Fx.sparks, 6);
			skill_gain(ds, Sk.parrying, true);
			return;
		}
		skill_gain(ds, Sk.parrying, false);
	}
	int dmg = rnd_range(lo, hi) * dmg_pct / 100;
	bool is_crit = rnd(1000) < crit;
	if (is_crit) { dmg = dmg * 2; }
	int armor = ds >= 0 ? player_armor(ds) : MONS[MOB_KIND[d]].armor;
	dmg = dmg * 100 / (100 + armor * 5);
	if (dmg < 1) { dmg = 1; }
	sound_at(w, MOB_X[d], MOB_Y[d], is_crit ? "crit" : (ranged ? "arrowhit" : "hit"));
	fx_at(w, MOB_X[d], MOB_Y[d], MOB_PLAYER[d] < 0 && MONS[MOB_KIND[d]].undead ? Fx.bones : Fx.blood, is_crit ? 10 : 4);
	if (slot >= 0) {
		skill_gain(slot, sk, true);
		skill_gain(slot, Sk.tactics, true);
	}
	if (ds >= 0) { skill_gain(ds, weapon_skill(ds), false); }
	// thorns bite back before the blow lands (the attacker may be the one who dies)
	int thorns = ds >= 0 ? player_bonus(ds, 15) : 0;
	int att_hp_before = MOB_HP[a];
	deal(d, dmg, a, DMG_PHYS, is_crit);
	if (leech > 0 && mob_alive(a)) { heal_mob(a, imax(1, dmg * leech / 100), a); }
	if (extra_fire > 0 && mob_alive(d)) { deal(d, extra_fire, a, DMG_FIRE, false); fx_at(w, MOB_X[d], MOB_Y[d], Fx.fire, 4); }
	if (extra_cold > 0 && mob_alive(d)) { deal(d, extra_cold, a, DMG_COLD, false); MOB_SLOW[d] = imax(MOB_SLOW[d], 20); }
	if (extra_poison > 0 && mob_alive(d)) { MOB_POISON[d] = imax(MOB_POISON[d], 40 + extra_poison * 10); }
	if (thorns > 0 && mob_alive(a) && att_hp_before > 0) { deal(a, thorns, d, DMG_PHYS, false); }
}

// ----------------------------------------------------------------- death ---
void kill(int d, int a) {
	int killer = mob_slot_owner(a);
	int w = MOB_WORLD[d];
	int x = MOB_X[d];
	int y = MOB_Y[d];
	if (MOB_PLAYER[d] >= 0) {
		player_die(MOB_PLAYER[d], a);
		return;
	}
	int kind = MOB_KIND[d];
	MonDef md = MONS[kind];
	STAT_KILLS = STAT_KILLS + 1;
	ev(Ev.death, w, x, y, MOB_SER[d], kind, 0, -1, "");
	sound_at(w, x, y, md.boss ? "bossdie" : "die");
	fx_at(w, x, y, md.undead ? Fx.bones : Fx.blood, md.boss ? 30 : 8);
	loot_drop(d, killer);
	if (killer >= 0) {
		Character& c = CHARS[killer];
		int fame = md.fame * ((MOB_FLAGS[d] & MF_ELITE) != 0 ? 2 : 1);
		c.fame = c.fame + fame;
		c.karma = c.karma + (md.fac == Fac.monster ? fame / 2 + 1 : 0);
		c.kills = c.kills + 1;
		c.version = c.version + 1;
		quest_on_kill(killer, kind);
		// party members nearby share the kill credit
		for (int s = 0; s < CHARS.size(); ++s) {
			if (s != killer && CHARS[s].active && c.party >= 0 && CHARS[s].party == c.party) {
				int pm = CHARS[s].mob;
				if (mob_live(pm) && MOB_WORLD[pm] == w && cheb(MOB_X[pm], MOB_Y[pm], x, y) <= 12) { quest_on_kill(s, kind); }
			}
		}
	}
	if (md.boss) {
		W_BOSS_T[w] = TICK + 3000;
		string who = killer >= 0 ? CHARS[killer].name : "the swamp itself";
		for (int s = 0; s < CHARS.size(); ++s) {
			if (CHARS[s].active) { say_to(s, `${md.name} has been slain by ${who}!`, C_YELLOW); }
		}
		if (killer >= 0) {
			CHARS[killer].boss_mask = CHARS[killer].boss_mask | (1 << w);
			shake(killer, 8);
		}
	}
	if (md.split >= 0) {
		for (int k = 0; k < 2; ++k) {
			for (int dir = 0; dir < 8; ++dir) {
				int nx = x + DX[(dir + k * 3) % 8];
				int ny = y + DY[(dir + k * 3) % 8];
				if (mob_at(w, nx, ny) < 0 && is_land(w, nx, ny)) {
					int s2 = mob_spawn(md.split, w, nx, ny);
					MOB_TARGET[s2] = a;
					break;
				}
			}
		}
		say_near(w, x, y, "The slime splits in two!", C_GREEN);
	}
	mob_free(d);
}

void player_die(int slot, int a) {
	Character& c = CHARS[slot];
	int m = c.mob;
	int w = MOB_WORLD[m];
	MOB_HP[m] = 0;
	MOB_FLAGS[m] = (MOB_FLAGS[m] | MF_GHOST) & ~MF_SWIM;
	MOB_TARGET[m] = -1;
	MOB_POISON[m] = 0;
	MOB_BURN[m] = 0;
	c.action = 0;
	c.deaths = c.deaths + 1;
	c.version = c.version + 1;
	if (c.gold > 0) {
		Item pouch = make_item(I_GOLD, c.gold);
		gi_add(pouch, w, MOB_X[m], MOB_Y[m], slot, 6000);
		c.gold = 0;
	}
	string by = mob_live(a) ? mob_the(a) : "misadventure";
	say_to(slot, `You have been slain by ${by}! Your ghost drifts free. Find a healer or a shrine.`, C_RED);
	ev(Ev.banner, -1, 0, 0, -1, 0, 0, slot, "YOU HAVE DIED");
	sound_at(w, MOB_X[m], MOB_Y[m], "death");
	fx_at(w, MOB_X[m], MOB_Y[m], Fx.smoke, 12);
	shake(slot, 8);
	for (int i = 0; i < MOB_KIND.size(); ++i) {
		if (MOB_TARGET[i] == m) { MOB_TARGET[i] = -1; }
	}
	// player killers: murdering an unflagged gator makes you a criminal
	int ks = mob_slot_owner(a);
	if (ks >= 0 && ks != slot && !c.pvp) {
		CHARS[ks].murders = CHARS[ks].murders + 1;
		CHARS[ks].karma = CHARS[ks].karma - 500;
		int km = CHARS[ks].mob;
		MOB_FLAGS[km] = MOB_FLAGS[km] | MF_CRIMINAL;
		say_to(ks, "You are now a murderer! The guards will not forget.", C_RED);
	}
}

void player_resurrect(int slot, int pct) {
	Character& c = CHARS[slot];
	int m = c.mob;
	if (!mob_ghost(m)) { return; }
	MOB_FLAGS[m] = MOB_FLAGS[m] & ~MF_GHOST;
	player_refresh(slot);
	MOB_HP[m] = imax(1, MOB_HPMAX[m] * pct / 100);
	MOB_MP[m] = MOB_MPMAX[m] / 2;
	c.version = c.version + 1;
	say_to(slot, "You feel the warm mud of life again!", C_GREEN);
	ev(Ev.banner, -1, 0, 0, -2, 0, 0, slot, "RESURRECTED");
	fx_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], Fx.holy, 20);
	sound_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], "res");
}

// ------------------------------------------------------ status and regen ---
void status_tick() {
	for (int m = 0; m < MOB_KIND.size(); ++m) {
		if (MOB_KIND[m] < 0 || MOB_HP[m] <= 0) { continue; }
		int slot = MOB_PLAYER[m];
		if (MOB_SLOW[m] > 0) { MOB_SLOW[m] = MOB_SLOW[m] - 1; }
		if (MOB_POISON[m] > 0) {
			MOB_POISON[m] = MOB_POISON[m] - 1;
			if (MOB_POISON[m] % 10 == 0) {
				deal(m, 1 + MOB_POISON[m] / 30, -1, DMG_POISON, false);
				if (!mob_live(m) || MOB_HP[m] <= 0) { continue; }
				if (MOB_POISON[m] % 30 == 0) { fx_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], Fx.poison, 3); }
			}
		}
		int w = MOB_WORLD[m];
		// lava burns whoever stands in it without protection
		if (walk_at(w, MOB_X[m], MOB_Y[m]) == 5 && !mob_ghost(m)) {
			bool proof = slot >= 0 ? CHARS[slot].buff_fire > TICK : MONS[MOB_KIND[m]].fireproof;
			if (!proof) { MOB_BURN[m] = imax(MOB_BURN[m], 12); }
		}
		if (MOB_BURN[m] > 0) {
			MOB_BURN[m] = MOB_BURN[m] - 1;
			if (MOB_BURN[m] % 4 == 0) {
				fx_at(w, MOB_X[m], MOB_Y[m], Fx.fire, 3);
				deal(m, 2, -1, DMG_FIRE, false);
				if (!mob_live(m) || MOB_HP[m] <= 0) { continue; }
			}
		}
		if (slot >= 0) {
			if (mob_ghost(m)) { continue; }
			Character& c = CHARS[slot];
			int regen = player_bonus(slot, 14) + (c.buff_regen > TICK ? 3 : 0);
			int hp_every = imax(4, 28 - regen * 5 - player_str(slot) / 12);
			if ((TICK + slot) % hp_every == 0 && MOB_HP[m] < MOB_HPMAX[m]) { MOB_HP[m] = MOB_HP[m] + 1; }
			int mp_every = imax(3, 22 - c.skill[Sk.meditation] / 70 - player_bonus(slot, 19) * 3);
			if ((TICK + slot) % mp_every == 0 && MOB_MP[m] < MOB_MPMAX[m]) {
				MOB_MP[m] = MOB_MP[m] + 1;
				if (MOB_MP[m] < MOB_MPMAX[m] && rnd(20) == 0) { skill_gain(slot, Sk.meditation, true); }
			}
		} else {
			int rg = MONS[MOB_KIND[m]].regen;
			if (rg > 0 && TICK % 10 == 0 && MOB_HP[m] < MOB_HPMAX[m]) { MOB_HP[m] = imin(MOB_HPMAX[m], MOB_HP[m] + rg); }
			// out-of-combat monsters recover slowly
			if (MOB_TARGET[m] < 0 && TICK % 20 == 0 && MOB_HP[m] < MOB_HPMAX[m]) { MOB_HP[m] = MOB_HP[m] + 1; }
		}
	}
}
