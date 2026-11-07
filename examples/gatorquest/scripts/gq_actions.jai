// Gator Quest - what players can do. Every intent a client sends (or the
// local keyboard produces) lands here as a short string, processed by the
// host's tick. The same code path runs single player, host and bots.

import "gq_quests.jai";

int ACT_FISH = 1;
int ACT_MINE = 2;
int ACT_CHOP = 3;
int ACT_BANDAGE = 4;
int ACT_CRAFT = 5;

// ---------------------------------------------------------------- joining ---
int player_create(int slot, string name, int hue) {
	while (CHARS.size() <= slot) { CHARS.push(Character()); }
	while (IN_MOVE.size() <= slot) { IN_MOVE.push(-1); }
	while (IN_QUEUE.size() <= slot) { IN_QUEUE.push([]); }
	Character c = Character();
	c.active = true;
	c.connected = true;
	c.name = name;
	c.hue = hue;
	c.str = 35;
	c.dex = 25;
	c.intel = 25;
	for (int i = 0; i < Sk.count; ++i) { c.skill.push(0); }
	c.skill[Sk.swords] = 300;
	c.skill[Sk.wrestling] = 300;
	c.skill[Sk.tactics] = 250;
	c.skill[Sk.swimming] = 400;
	c.skill[Sk.fishing] = 200;
	c.skill[Sk.magery] = 250;
	c.skill[Sk.healing] = 200;
	c.skill[Sk.mining] = 100;
	c.skill[Sk.lumber] = 100;
	c.skill[Sk.cooking] = 150;
	for (int i = 0; i < Slot.count; ++i) { c.worn.push(Item()); }
	for (int i = 0; i < SPELLS.size(); ++i) { c.spells.push(0); }
	c.spells[0] = 1;
	c.spells[1] = 1;
	for (int i = 0; i < WORLD_COUNT; ++i) { c.visited.push(0); }
	c.visited[0] = 1;
	c.gold = 50;
	c.home_world = 0;
	c.home_x = W_ARRIVE_X[0];
	c.home_y = W_ARRIVE_Y[0];
	CHARS[slot] = c;
	quest_ensure(slot);
	Character& cc = CHARS[slot];
	cc.worn[Slot.weapon] = make_item(I_DAGGER, 1);
	cc.worn[Slot.body] = make_item(I_LEATHERTUNIC, 1);
	cc.worn[Slot.head] = make_item(I_STRAWHAT, 1);
	cc.worn[Slot.feet] = make_item(I_SANDALS, 1);
	pack_add(slot, make_item(I_SPELLBOOK, 1));
	pack_add(slot, make_item(I_MINORHEAL, 3));
	pack_add(slot, make_item(I_BANDAGE, 8));
	pack_add(slot, make_item(I_FISHPOLE, 1));
	pack_add(slot, make_item(I_ONION, 6));
	pack_add(slot, make_item(I_CATTAIL, 6));
	pack_add(slot, make_item(I_FIREFLY, 8));
	pack_add(slot, make_item(I_BREAD, 3));
	player_spawn(slot);
	return slot;
}

// put the character's gator into the world at its home
void player_spawn(int slot) {
	Character& c = CHARS[slot];
	array<int> p = find_free_spot(c.home_world, c.home_x, c.home_y, 8, LAND_MASK);
	int m = mob_spawn(M_GATOR_NPC, c.home_world, p[0], p[1]);
	MOB_PLAYER[m] = slot;
	MOB_FLAGS[m] = 0;
	MOB_ROLE[m] = -1;
	c.mob = m;
	player_refresh(slot);
	MOB_HP[m] = MOB_HPMAX[m];
	MOB_MP[m] = MOB_MPMAX[m];
	if (c.pvp) { MOB_FLAGS[m] = MOB_FLAGS[m] | MF_PVP; }
	fx_at(c.home_world, p[0], p[1], Fx.warp, 16);
}

void player_leave(int slot) {
	if (slot < 0 || slot >= CHARS.size() || !CHARS[slot].active) { return; }
	Character& c = CHARS[slot];
	int m = c.mob;
	for (int i = 0; i < MOB_KIND.size(); ++i) {
		if (MOB_KIND[i] >= 0 && MOB_OWNER[i] == m && (MOB_FLAGS[i] & MF_SUMMON) != 0) { mob_free(i); }
	}
	if (mob_live(m)) {
		fx_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], Fx.smoke, 10);
		MOB_PLAYER[m] = -1;
		mob_free(m);
	}
	// parties forget the leaver (a party of one dissolves)
	for (int s = 0; s < CHARS.size(); ++s) {
		if (CHARS[s].invite == slot) { CHARS[s].invite = -1; }
	}
	party_leave(slot);
	c.mob = -1;
	c.active = false;
	c.connected = false;
	IN_MOVE[slot] = -1;
	IN_QUEUE[slot] = [];
}

// ------------------------------------------------------------- movement ---
int facing_x(int m) { return MOB_X[m] + DX[MOB_DIR[m]]; }
int facing_y(int m) { return MOB_Y[m] + DY[MOB_DIR[m]]; }

void fall_from_sky(int m) {
	int slot = MOB_PLAYER[m];
	say_to(slot, "AAAAAAAAAAAA-", C_RED);
	ev(Ev.banner, -1, 0, 0, -5, 0, 0, slot, "WHEEEEEE!");
	int x = rnd_range(20, W_W[0] - 20);
	int y = rnd_range(15, W_H[0] - 15);
	travel(m, 0, x, y);
	sound_at(0, MOB_X[m], MOB_Y[m], "boom");
	fx_at(0, MOB_X[m], MOB_Y[m], Fx.splash, 20);
	deal(m, MOB_HPMAX[m] / 4, -1, DMG_PHYS, false);
	say_to(slot, "-AAAAAA! You plummet out of the clouds and land in Gatorland with a SPLAT.", C_RED);
}

// stepping onto a tile triggers what the tile does
void on_enter(int m) {
	int w = MOB_WORLD[m];
	int x = MOB_X[m];
	int y = MOB_Y[m];
	int t = tile_now(w, x, y);
	int st = TILES_DEF[t].station;
	int slot = MOB_PLAYER[m];
	if (st == 7 || st == 8) {
		for (int g = 0; g < GATE_W.size(); ++g) {
			if (GATE_W[g] == w && GATE_X[g] == x && GATE_Y[g] == y) {
				int dest = GATE_DEST[g];
				array<int> p = gate_arrival(w, dest);
				travel(m, dest, p[0], p[1]);
				return;
			}
		}
	}
	if (T_WALK[t] == 3 && slot >= 0) { fall_from_sky(m); return; }
	if (T_WALK[t] == 4 && slot >= 0 && !mob_ghost(m)) {
		// ice: keep sliding the way you were going
		MOB_AUX[m] = MOB_DIR[m] + 1;
	} else if (slot >= 0) {
		MOB_AUX[m] = 0;
	}
	if (t == T_GOLDPILE && slot >= 0 && REGROW[W_OFF[w] + y * W_W[w] + x] <= TICK) {
		int amount = 20 + WORLDS[w].tier * 25 + rnd(40);
		CHARS[slot].gold = CHARS[slot].gold + amount;
		CHARS[slot].version = CHARS[slot].version + 1;
		REGROW[W_OFF[w] + y * W_W[w] + x] = TICK + 6000;
		say_to(slot, `You scoop up ${amount} gold from the glittering pile!`, C_YELLOW);
		fx_at(w, x, y, Fx.gold, 18);
		sound_at(w, x, y, "coin");
	}
}

// a player moving into a tile: attack what is there, talk, open, or step
void player_move(int slot, int dir) {
	Character& c = CHARS[slot];
	int m = c.mob;
	if (!mob_live(m) || TICK < MOB_NEXT[m]) { return; }
	int w = MOB_WORLD[m];
	int nx = MOB_X[m] + DX[dir];
	int ny = MOB_Y[m] + DY[dir];
	MOB_DIR[m] = dir;
	c.action = 0;
	int o = mob_at(w, nx, ny);
	if (o >= 0 && o != m) {
		if (mob_ghost(m)) {
			int role = MOB_ROLE[o];
			if (role >= 0 && ROLES[role].heals) {
				say_to(slot, `${ROLES[role].name}: "Back to the land of the living with you, dear."`, C_WHITE);
				player_resurrect(slot, 60);
				MOB_NEXT[m] = TICK + 4;
				return;
			}
		} else if (hostile(m, o)) {
			MOB_TARGET[m] = o;
			if (TICK >= MOB_ATK[m] && attack_range(m) >= 1) { attack(m, o); }
			MOB_NEXT[m] = TICK + 2;
			return;
		} else if (MOB_ROLE[o] >= 0) {
			// bumping a shopkeeper greets them once; leaning on them does not repeat it
			if (c.talk_npc != o || TICK - c.talk_t > 60) {
				c.talk_npc = o;
				npc_talk(slot, o);
			}
			c.talk_t = TICK;
			MOB_NEXT[m] = TICK + 4;
			return;
		} else if (MOB_PLAYER[o] >= 0 || mob_fac(o) == Fac.npc || mob_fac(o) == Fac.guard) {
			// swap places with friendly folk so nobody gets stuck in a doorway
			if (MOB_PLAYER[o] < 0 && mob_can_enter(o, MOB_X[m], MOB_Y[m]) == false) {
				int ox = MOB_X[o];
				int oy = MOB_Y[o];
				occ_clear(o);
				occ_clear(m);
				MOB_X[o] = MOB_X[m];
				MOB_Y[o] = MOB_Y[m];
				MOB_PX[o] = MOB_X[o];
				MOB_PY[o] = MOB_Y[o];
				occ_set(o);
				MOB_PX[m] = MOB_X[m];
				MOB_PY[m] = MOB_Y[m];
				MOB_X[m] = ox;
				MOB_Y[m] = oy;
				MOB_MOVE_T[m] = TICK;
				MOB_MOVE_D[m] = 2;
				MOB_NEXT[m] = TICK + 2;
				occ_set(m);
				return;
			}
			MOB_NEXT[m] = TICK + 1;
			return;
		}
	}
	int t = tile_now(w, nx, ny);
	if (TILES_DEF[t].station == 9 && !mob_ghost(m)) { open_chest(slot, w, nx, ny); MOB_NEXT[m] = TICK + 3; return; }
	if (TILES_DEF[t].station == 6 && mob_ghost(m)) {
		say_to(slot, "The shrine's light washes over your ghost.", C_WHITE);
		player_resurrect(slot, 40);
		MOB_NEXT[m] = TICK + 4;
		return;
	}
	if (mob_step(m, dir)) {
		// stealth: moving while hidden needs a check
		if ((MOB_FLAGS[m] & MF_HIDDEN) != 0) {
			if (!skill_check(slot, Sk.stealth, 300)) {
				MOB_FLAGS[m] = MOB_FLAGS[m] & ~MF_HIDDEN;
				say_to(slot, "You have been revealed!", C_ORANGE);
			}
		}
		if ((MOB_FLAGS[m] & MF_SWIM) != 0 && rnd(12) == 0) { skill_gain(slot, Sk.swimming, true); }
		on_enter(m);
	}
}

// ------------------------------------------------------------------ npcs ---
void npc_talk(int slot, int npc) {
	int role = MOB_ROLE[npc];
	if (role < 0) { return; }
	int m = CHARS[slot].mob;
	MOB_DIR[npc] = dir_of(MOB_X[m] - MOB_X[npc], MOB_Y[m] - MOB_Y[npc]);
	quest_talk(slot, role);
	ev(Ev.dialog, MOB_WORLD[npc], MOB_X[npc], MOB_Y[npc], MOB_SER[npc], role, 0, slot, ROLES[role].name);
}

int find_npc_ser(int ser) {
	for (int i = 0; i < MOB_KIND.size(); ++i) {
		if (MOB_KIND[i] >= 0 && MOB_SER[i] == ser) { return i; }
	}
	return -1;
}

bool npc_in_reach(int slot, int npc) {
	int m = CHARS[slot].mob;
	return mob_live(npc) && MOB_WORLD[npc] == MOB_WORLD[m] && cheb(MOB_X[m], MOB_Y[m], MOB_X[npc], MOB_Y[npc]) <= 3;
}

int buy_price(int item) { return imax(1, ITEMS[item].value * 11 / 10); }
int sell_price(Item it) {
	int v = item_value(it) / 3;
	if (it.base == I_GOLDCARP) { v = 750; }
	return imax(1, v);
}

void shop_buy(int slot, int npc, int index) {
	if (!npc_in_reach(slot, npc)) { return; }
	int role = MOB_ROLE[npc];
	int shop = role >= 0 ? ROLES[role].shop : -1;
	if (shop < 0 || index < 0 || index >= SHOPS[shop].size()) { return; }
	int item = SHOPS[shop][index];
	int price = buy_price(item);
	Character& c = CHARS[slot];
	if (c.gold < price) { say_to(slot, `${ROLES[role].name}: "That is ${price} gold, sugar. You are short."`, C_RED); return; }
	int qty = ITEMS[item].kind == Kind.ammo ? 20 : 1;
	if (qty > 1) { price = price * qty / 2; }
	if (c.gold < price) { qty = 1; price = buy_price(item); }
	Item it = make_item(item, qty);
	if (!pack_add(slot, it)) { say_to(slot, "Your backpack is full.", C_RED); return; }
	c.gold = c.gold - price;
	c.version = c.version + 1;
	say_to(slot, `You buy ${item_name(it)} for ${price} gold.`, C_GREEN);
	sound_at(MOB_WORLD[npc], MOB_X[npc], MOB_Y[npc], "coin");
}

void shop_sell(int slot, int npc, int index) {
	if (!npc_in_reach(slot, npc)) { return; }
	int role = MOB_ROLE[npc];
	if (role < 0 || ROLES[role].shop < 0) { return; }
	Character& c = CHARS[slot];
	if (index < 0 || index >= c.pack.size()) { return; }
	Item one = c.pack[index];
	if (ITEMS[one.base].kind == Kind.quest || one.base == I_SPELLBOOK) { say_to(slot, "You would rather keep that.", C_GRAY); return; }
	Item sold = pack_take_at(slot, index, 1);
	int price = sell_price(sold);
	c.gold = c.gold + price;
	if (sold.base == I_GOLDCARP) {
		say_to(slot, `${ROLES[role].name}: "THE GOLDEN CARP! I am weeping! Take this fortune!"`, C_YELLOW);
		fx_at(MOB_WORLD[npc], MOB_X[npc], MOB_Y[npc], Fx.gold, 30);
	}
	say_to(slot, `You sell ${item_name(sold)} for ${price} gold.`, C_GREEN);
	sound_at(MOB_WORLD[npc], MOB_X[npc], MOB_Y[npc], "coin");
}

void healer_service(int slot, int npc) {
	if (!npc_in_reach(slot, npc)) { return; }
	int role = MOB_ROLE[npc];
	if (role < 0 || !ROLES[role].heals) { return; }
	Character& c = CHARS[slot];
	int m = c.mob;
	if (mob_ghost(m)) { player_resurrect(slot, 60); return; }
	if (MOB_HP[m] >= MOB_HPMAX[m] && MOB_POISON[m] == 0) { say_to(slot, `${ROLES[role].name}: "You look healthy as a hog, dear."`, C_WHITE); return; }
	if (c.gold < 15) { say_to(slot, `${ROLES[role].name}: "Fifteen gold for a healing, dear."`, C_RED); return; }
	c.gold = c.gold - 15;
	MOB_POISON[m] = 0;
	heal_mob(m, MOB_HPMAX[m], npc);
	fx_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], Fx.heal, 14);
	sound_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], "heal");
	say_to(slot, `${ROLES[role].name} heals your wounds for 15 gold.`, C_GREEN);
	c.version = c.version + 1;
}

void bank_deposit(int slot, int npc, int index) {
	if (!npc_in_reach(slot, npc) || MOB_ROLE[npc] < 0 || !ROLES[MOB_ROLE[npc]].banks) { return; }
	Character& c = CHARS[slot];
	if (index == -1) {
		c.bank_gold = c.bank_gold + c.gold;
		say_to(slot, `You deposit ${c.gold} gold. Balance: ${c.bank_gold}.`, C_GREEN);
		c.gold = 0;
	} else if (index >= 0 && index < c.pack.size()) {
		if (c.bank.size() >= 60) { say_to(slot, "Your bank box is full.", C_RED); return; }
		Item it = c.pack[index];
		c.pack.remove(index);
		c.bank.push(it);
		say_to(slot, `You place ${item_name(it)} in your bank box.`, C_GREEN);
	}
	c.version = c.version + 1;
	sound_at(MOB_WORLD[npc], MOB_X[npc], MOB_Y[npc], "coin");
}

void bank_withdraw(int slot, int npc, int index) {
	if (!npc_in_reach(slot, npc) || MOB_ROLE[npc] < 0 || !ROLES[MOB_ROLE[npc]].banks) { return; }
	Character& c = CHARS[slot];
	if (index == -1) {
		c.gold = c.gold + c.bank_gold;
		say_to(slot, `You withdraw ${c.bank_gold} gold.`, C_GREEN);
		c.bank_gold = 0;
	} else if (index >= 0 && index < c.bank.size()) {
		if (c.pack.size() >= PACK_MAX) { say_to(slot, "Your backpack is full.", C_RED); return; }
		Item it = c.bank[index];
		c.bank.remove(index);
		c.pack.push(it);
		say_to(slot, `You take ${item_name(it)} from your bank box.`, C_GREEN);
	}
	c.version = c.version + 1;
}

// ---------------------------------------------------------------- chests ---
void open_chest(int slot, int w, int x, int y) {
	for (int k = 0; k < CHEST_W.size(); ++k) {
		if (CHEST_W[k] != w || CHEST_X[k] != x || CHEST_Y[k] != y) { continue; }
		if (CHEST_T[k] > TICK) { say_to(slot, "The chest is empty. Someone has been here recently.", C_GRAY); return; }
		int tier = CHEST_TIER[k];
		CHEST_T[k] = TICK + 4000;
		int luck = player_luck(slot);
		say_to(slot, "You pry open the chest...", C_YELLOW);
		sound_at(w, x, y, "chest");
		drop_near(make_item(I_GOLD, 15 * tier + rnd(30 * tier)), w, x, y + 1, -1, 3000);
		drop_near(roll_equipment(imin(5, tier + 1), luck, tier >= 3 ? 1 : 0), w, x, y + 1, -1, 3000);
		for (int i = 0; i < 1 + rnd(3); ++i) { drop_near(roll_consumable(tier), w, x, y + 1, -1, 3000); }
		if (rnd(100) < 10 * tier) { drop_near(make_item(iid(["Amethyst", "Ruby", "Sapphire", "Emerald", "Diamond"][imin(4, tier - 1)]), 1), w, x, y + 1, -1, 3000); }
		fx_at(w, x, y, Fx.gold, 14);
		return;
	}
}

// ------------------------------------------------------------ gathering ---
int fish_for(int w, int t, int skill) {
	int r = rnd(1000);
	if (r < 60) {
		array<int> junk = [I_OLDBOOT, I_TINCAN, I_SOGGYHAT, I_BOTTLE];
		return junk[rnd(junk.size())];
	}
	if (t == T_LAVA) { return I_LAVAEEL; }
	if (t == T_SKY) { return I_CLOUDTROUT; }
	if (w == 1 && skill >= 500 && rnd(1000) < 8 + (skill - 500) / 20) { return I_GOLDCARP; }
	if (w == 1 || t == T_SWAMP) { return rnd(2) == 0 ? I_CATFISH : I_RAWFISH; }
	if (w == 3) { return I_GHOSTFISH; }
	if (w == 4) { return I_ICEFISH; }
	if (w == 2) { return rnd(3) == 0 ? I_KELP : I_RAWFISH; }
	return rnd(3) == 0 ? I_CATFISH : I_RAWFISH;
}

// use your facing tile: fish, mine, chop; or talk to who is there; or open a chest
void interact(int slot) {
	Character& c = CHARS[slot];
	int m = c.mob;
	if (mob_ghost(m)) { say_to(slot, "You are a ghost. Seek a healer or a shrine.", C_GRAY); return; }
	int w = MOB_WORLD[m];
	// the facing tile first, then any neighbour that is interesting
	for (int k = 0; k < 9; ++k) {
		int dir = k == 0 ? MOB_DIR[m] : k - 1;
		int x = MOB_X[m] + DX[dir];
		int y = MOB_Y[m] + DY[dir];
		int o = mob_at(w, x, y);
		if (o >= 0 && MOB_ROLE[o] >= 0) { MOB_DIR[m] = dir; npc_talk(slot, o); return; }
		int t = tile_now(w, x, y);
		TileDef td = TILES_DEF[t];
		int res = td.res;
		if (t == T_LAVA && c.skill[Sk.fishing] >= 600) { res = 4; }
		if (t == T_SKY && w == 6) { res = 4; }
		if (td.station == 9) { MOB_DIR[m] = dir; open_chest(slot, w, x, y); return; }
		if (res == 4 && k > 0 && !pack_has(slot, I_FISHPOLE, 1)) { continue; }
		if (res == 4) {
			if (!pack_has(slot, I_FISHPOLE, 1)) { say_to(slot, "You need a fishing pole.", C_GRAY); return; }
			start_action(slot, ACT_FISH, x, y, 25 + rnd(20), 0);
			MOB_DIR[m] = dir;
			say_to(slot, "You cast your line...", C_CYAN);
			sound_at(w, x, y, "splash");
			return;
		}
		if (res == 2 || res == 3 || res == 5) {
			if (!pack_has(slot, I_PICKAXE, 1)) { say_to(slot, "You need a pickaxe to mine that.", C_GRAY); return; }
			start_action(slot, ACT_MINE, x, y, 20, 0);
			MOB_DIR[m] = dir;
			say_to(slot, "You start swinging your pickaxe.", C_CYAN);
			return;
		}
		if (res == 1) {
			if (!pack_has(slot, I_HATCHET, 1)) { say_to(slot, "You need a hatchet to chop that.", C_GRAY); return; }
			start_action(slot, ACT_CHOP, x, y, 20, 0);
			MOB_DIR[m] = dir;
			say_to(slot, "You start chopping.", C_CYAN);
			return;
		}
		if (td.station == 6 && k <= 8) { say_to(slot, `The shrine hums. "${quest_progress(slot, Q_SIX_SCALES)} of 6 Relic Scales."`, C_YELLOW); return; }
	}
	say_to(slot, "There is nothing here to use.", C_GRAY);
}

void start_action(int slot, int kind, int x, int y, int ticks, int arg) {
	Character& c = CHARS[slot];
	c.action = kind;
	c.action_x = x;
	c.action_y = y;
	c.action_t = TICK + ticks;
	c.action_arg = arg;
	c.version = c.version + 1;
}

void finish_action(int slot) {
	Character& c = CHARS[slot];
	int m = c.mob;
	int w = MOB_WORLD[m];
	int kind = c.action;
	c.action = 0;
	c.version = c.version + 1;
	int x = c.action_x;
	int y = c.action_y;
	int t = tile_now(w, x, y);
	if (kind == ACT_FISH) {
		if (!skill_check(slot, Sk.fishing, 150 + WORLDS[w].tier * 80)) {
			say_to(slot, "The fish got away.", C_GRAY);
			return;
		}
		int fish = fish_for(w, t, c.skill[Sk.fishing]);
		Item it = make_item(fish, 1);
		if (!pack_add(slot, it)) { drop_near(it, w, MOB_X[m], MOB_Y[m], slot, 3000); }
		fx_at(w, x, y, Fx.splash, 10);
		sound_at(w, x, y, "splash");
		if (fish == I_GOLDCARP) {
			say_to(slot, "!!! You reel in the legendary GOLDEN CARP !!!", C_YELLOW);
			ev(Ev.banner, -1, 0, 0, -4, 0, 0, slot, "GOLDEN CARP!");
			fx_at(w, x, y, Fx.gold, 30);
		} else if (ITEMS[fish].kind == Kind.junk) {
			say_to(slot, `You fish up ${article(ITEMS[fish].name)}. Hooray?`, C_GRAY);
		} else {
			say_to(slot, `You catch ${article(ITEMS[fish].name)}!`, C_GREEN);
			c.fish_caught = c.fish_caught + 1;
			quest_on_fish(slot);
		}
		return;
	}
	if (kind == ACT_MINE) {
		int res = TILES_DEF[t].res;
		if (res != 2 && res != 3 && res != 5) { return; }
		int tier = WORLDS[w].tier;
		if (!skill_check(slot, Sk.mining, 100 + tier * 90 + (res == 3 ? 100 : 0))) {
			say_to(slot, "You chip away but find nothing useful.", C_GRAY);
			fx_at(w, x, y, Fx.sparks, 4);
			sound_at(w, x, y, "clink");
			return;
		}
		int got = I_IRONORE;
		int sk = c.skill[Sk.mining];
		if (res == 5) {
			got = rnd(3) == 0 ? iid("Frost Shard") : iid(["Amethyst", "Sapphire", "Diamond"][rnd(3)]);
		} else if (res == 3) {
			if (tier >= 4 && sk >= 650 && rnd(3) == 0) { got = I_MITHRILORE; }
			else if (tier >= 2 && sk >= 300 && rnd(2) == 0) { got = I_SILVERORE; }
		}
		int qty = ITEMS[got].kind == Kind.gem ? 1 : 1 + rnd(2);
		Item it = make_item(got, qty);
		if (!pack_add(slot, it)) { drop_near(it, w, MOB_X[m], MOB_Y[m], slot, 3000); }
		say_to(slot, `You dig up ${item_name(it)}.`, C_GREEN);
		fx_at(w, x, y, Fx.sparks, 8);
		sound_at(w, x, y, "clink");
		if (rnd(4) == 0) { REGROW[W_OFF[w] + y * W_W[w] + x] = TICK + 3000; say_to(slot, "The vein is exhausted.", C_GRAY); }
		return;
	}
	if (kind == ACT_CHOP) {
		if (TILES_DEF[t].res != 1) { return; }
		if (!skill_check(slot, Sk.lumber, 150)) {
			say_to(slot, "You hack at the tree but get no usable wood.", C_GRAY);
			sound_at(w, x, y, "chop");
			return;
		}
		Item it = make_item(I_LOG, 2 + rnd(3));
		if (!pack_add(slot, it)) { drop_near(it, w, MOB_X[m], MOB_Y[m], slot, 3000); }
		say_to(slot, `You chop ${item_name(it)}.`, C_GREEN);
		fx_at(w, x, y, Fx.dust, 8);
		sound_at(w, x, y, "chop");
		if (rnd(3) == 0) {
			REGROW[W_OFF[w] + y * W_W[w] + x] = TICK + 3000;
			say_to(slot, "TIMBER! The tree comes crashing down.", C_ORANGE);
			shake(slot, 3);
		}
		return;
	}
	if (kind == ACT_BANDAGE) {
		if (!pack_take(slot, I_BANDAGE, 1)) { return; }
		int amount = 8 + c.skill[Sk.healing] / 25 + rnd(6);
		if (skill_check(slot, Sk.healing, 200)) {
			heal_mob(m, amount, m);
			if (MOB_POISON[m] > 0 && c.skill[Sk.healing] >= 600) { MOB_POISON[m] = 0; say_to(slot, "You cure the poison.", C_GREEN); }
			say_to(slot, "You finish applying the bandage.", C_GREEN);
		} else {
			heal_mob(m, amount / 3, m);
			say_to(slot, "Your fingers slip; the bandage barely helps.", C_GRAY);
		}
		return;
	}
	if (kind == ACT_CRAFT) { craft_finish(slot, c.action_arg); }
}

void actions_tick(int slot) {
	Character& c = CHARS[slot];
	if (c.action != 0 && TICK >= c.action_t) { finish_action(slot); }
}

// ------------------------------------------------------------- crafting ---
bool near_station(int slot, int station) {
	if (station == 0) { return true; }
	int m = CHARS[slot].mob;
	int w = MOB_WORLD[m];
	for (int dy = -2; dy <= 2; ++dy) {
		for (int dx = -2; dx <= 2; ++dx) {
			if (TILES_DEF[tile_now(w, MOB_X[m] + dx, MOB_Y[m] + dy)].station == station) { return true; }
		}
	}
	return false;
}

bool recipe_ok(int slot, int r, bool verbose) {
	RecipeDef d = RECIPES[r];
	Character& c = CHARS[slot];
	if (c.skill[d.skill] < d.min) {
		if (verbose) { say_to(slot, `You need ${tenths(d.min)} ${SKILL_NAME[d.skill]} to make that.`, C_GRAY); }
		return false;
	}
	if (!near_station(slot, d.station)) {
		if (verbose) { say_to(slot, `You need to be near a ${STATION_NAME[d.station]}.`, C_GRAY); }
		return false;
	}
	if (d.tool >= 0 && !pack_has(slot, d.tool, 1)) {
		if (verbose) { say_to(slot, `You need ${article(ITEMS[d.tool].name)}.`, C_GRAY); }
		return false;
	}
	if (d.skill == Sk.smithing && d.station == 2 && !pack_has(slot, I_HAMMER, 1)) {
		if (verbose) { say_to(slot, "You need a smith hammer.", C_GRAY); }
		return false;
	}
	for (int k = 0; k < d.in_item.size(); ++k) {
		if (!pack_has(slot, d.in_item[k], d.in_qty[k])) {
			if (verbose) { say_to(slot, `You need ${d.in_qty[k]} ${ITEMS[d.in_item[k]].name}.`, C_GRAY); }
			return false;
		}
	}
	return true;
}

void craft_start(int slot, int r) {
	if (r < 0 || r >= RECIPES.size() || mob_ghost(CHARS[slot].mob)) { return; }
	if (!recipe_ok(slot, r, true)) { return; }
	start_action(slot, ACT_CRAFT, 0, 0, 12, r);
	int m = CHARS[slot].mob;
	say_to(slot, `You set to work on ${article(ITEMS[RECIPES[r].out].name)}...`, C_CYAN);
	sound_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], RECIPES[r].station == 3 ? "sizzle" : "hammer");
}

void craft_finish(int slot, int r) {
	if (!recipe_ok(slot, r, true)) { return; }
	RecipeDef d = RECIPES[r];
	Character& c = CHARS[slot];
	int m = c.mob;
	int w = MOB_WORLD[m];
	bool ok = skill_check(slot, d.skill, d.min + 150);
	if (!ok) {
		// lose half the materials, rounded up
		for (int k = 0; k < d.in_item.size(); ++k) { pack_take(slot, d.in_item[k], (d.in_qty[k] + 1) / 2); }
		say_to(slot, "You fail, and some materials are ruined.", C_GRAY);
		fx_at(w, MOB_X[m], MOB_Y[m], Fx.smoke, 6);
		return;
	}
	for (int k = 0; k < d.in_item.size(); ++k) { pack_take(slot, d.in_item[k], d.in_qty[k]); }
	Item it = make_item(d.out, d.out_qty);
	string what = "";
	if (ITEMS[d.out].slot >= 0) {
		int margin = c.skill[d.skill] - d.min;
		if (rnd(1000) < 60 + margin / 2) {
			it.bonus = 1 + rnd(3);
			it.rarity = 1;
			bool weapon = ITEMS[d.out].kind == Kind.weapon;
			it.suffix = roll_affix(false, weapon, 2 + margin / 250);
			if (it.suffix >= 0) { it.smag = roll_mag(it.suffix, 1); }
			what = "an exceptional ";
		}
	}
	if (!pack_add(slot, it)) { drop_near(it, w, MOB_X[m], MOB_Y[m], slot, 3000); }
	c.crafted = c.crafted + 1;
	if (what != "") {
		say_to(slot, `You create ${what}${item_name(it)}! A masterpiece!`, C_YELLOW);
		fx_at(w, MOB_X[m], MOB_Y[m], Fx.levelup, 12);
	} else {
		say_to(slot, `You create ${item_name(it)}.`, C_GREEN);
	}
	fx_at(w, MOB_X[m], MOB_Y[m], d.station == 3 ? Fx.fire : Fx.sparks, 8);
	sound_at(w, MOB_X[m], MOB_Y[m], "craft");
	quest_on_craft(slot);
}

// --------------------------------------------------------------- using ---
void use_item(int slot, int index) {
	Character& c = CHARS[slot];
	int m = c.mob;
	if (index < 0 || index >= c.pack.size()) { return; }
	if (mob_ghost(m)) { say_to(slot, "Ghostly paws pass right through it.", C_GRAY); return; }
	Item it = c.pack[index];
	ItemDef d = ITEMS[it.base];
	int w = MOB_WORLD[m];
	if (item_equippable(it)) { equip_from_pack(slot, index); return; }
	string eff = EFF_NAME[d.eff];
	if (d.kind == Kind.potion && eff != "bomb") {
		if (TICK < c.potion_t) {
			say_to(slot, `You must wait ${(c.potion_t - TICK + 9) / 10}s before drinking another potion.`, C_GRAY);
			return;
		}
		c.potion_t = TICK + 30;
	}
	if (eff == "heal") {
		pack_take_at(slot, index, 1);
		heal_mob(m, d.pow, m);
		fx_at(w, MOB_X[m], MOB_Y[m], Fx.heal, 10);
		sound_at(w, MOB_X[m], MOB_Y[m], "drink");
		return;
	}
	if (eff == "mana") {
		pack_take_at(slot, index, 1);
		MOB_MP[m] = imin(MOB_MPMAX[m], MOB_MP[m] + d.pow);
		fx_at(w, MOB_X[m], MOB_Y[m], Fx.magic, 8);
		sound_at(w, MOB_X[m], MOB_Y[m], "drink");
		return;
	}
	if (eff == "cure") {
		pack_take_at(slot, index, 1);
		MOB_POISON[m] = 0;
		say_to(slot, "You feel the poison leave your body.", C_GREEN);
		sound_at(w, MOB_X[m], MOB_Y[m], "drink");
		return;
	}
	if (eff == "str") {
		pack_take_at(slot, index, 1);
		c.buff_str = TICK + 1200;
		say_to(slot, "You feel mighty! (not really, but the potion tasted great)", C_GREEN);
		heal_mob(m, 10, m);
		return;
	}
	if (eff == "eyeshine") { pack_take_at(slot, index, 1); c.buff_night = TICK + d.pow; say_to(slot, "The darkness melts away.", C_YELLOW); return; }
	if (eff == "fireres") { pack_take_at(slot, index, 1); c.buff_fire = TICK + d.pow; say_to(slot, "Your scales feel cool and fireproof.", C_ORANGE); return; }
	if (eff == "regen") { pack_take_at(slot, index, 1); c.buff_regen = TICK + d.pow; say_to(slot, "You feel your wounds knitting.", C_GREEN); return; }
	if (eff == "food") {
		pack_take_at(slot, index, 1);
		heal_mob(m, d.pow, m);
		array<string> yum = ["Delicious!", "Mmm, swampy.", "Chomp chomp chomp.", "That hit the spot.", "Tastes like chicken."];
		say_to(slot, `You eat the ${d.name}. ${yum[rnd(yum.size())]}`, C_GREEN);
		sound_at(w, MOB_X[m], MOB_Y[m], "eat");
		return;
	}
	if (eff == "learn") {
		int spell = d.pow;
		if (c.spells[spell] == 0) {
			pack_take_at(slot, index, 1);
			c.spells[spell] = 1;
			c.version = c.version + 1;
			say_to(slot, `You inscribe ${SPELLS[spell].name} into your spellbook!`, C_PURPLE);
			fx_at(w, MOB_X[m], MOB_Y[m], Fx.magic, 12);
			sound_at(w, MOB_X[m], MOB_Y[m], "chime");
		} else {
			say_to(slot, `You already know ${SPELLS[spell].name}. Sell the scroll to Madame Hoodoo.`, C_GRAY);
		}
		return;
	}
	if (eff == "recall") {
		pack_take_at(slot, index, 1);
		array<int> p = find_spot(c.home_world, c.home_x, c.home_y, 5, LAND_MASK);
		travel(m, c.home_world, p[0], p[1]);
		return;
	}
	if (eff == "bandage") {
		start_action(slot, ACT_BANDAGE, MOB_X[m], MOB_Y[m], 20 - c.skill[Sk.healing] / 100, 0);
		say_to(slot, "You begin applying a bandage.", C_CYAN);
		return;
	}
	if (eff == "random") {
		pack_take_at(slot, index, 1);
		int r = rnd(6);
		if (r == 0) { heal_mob(m, 40, m); say_to(slot, "You feel fantastic! The colors! THE COLORS!", C_PURPLE); }
		else if (r == 1) { MOB_POISON[m] = 60; say_to(slot, "Oh no. Oh no no no. You feel very poisoned.", C_GREEN); }
		else if (r == 2) { c.buff_night = TICK + 900; say_to(slot, "You can see sounds. And also in the dark.", C_YELLOW); }
		else if (r == 3) { MOB_MP[m] = MOB_MPMAX[m]; say_to(slot, "Your mind buzzes with arcane energy!", C_BLUE); }
		else if (r == 4) { c.gold = c.gold + 25; say_to(slot, "You find 25 gold inside the mushroom. How?", C_YELLOW); }
		else { say_to(slot, "You feel... exactly the same. Disappointing.", C_GRAY); }
		fx_at(w, MOB_X[m], MOB_Y[m], Fx.magic, 10);
		return;
	}
	if (eff == "firebreath") {
		pack_take_at(slot, index, 1);
		say_to(slot, "You chug the hot sauce and breathe FIRE!", C_ORANGE);
		int dir = MOB_DIR[m];
		for (int k = 1; k <= 4; ++k) {
			int x = MOB_X[m] + DX[dir] * k;
			int y = MOB_Y[m] + DY[dir] * k;
			fx_at(w, x, y, Fx.fire, 6);
			int o = mob_at(w, x, y);
			if (o >= 0 && hostile(m, o)) { deal(o, d.pow + rnd(10), m, DMG_FIRE, false); if (mob_alive(o)) { MOB_BURN[o] = 20; } }
		}
		sound_at(w, MOB_X[m], MOB_Y[m], "fire");
		shake(slot, 3);
		return;
	}
	if (eff == "bomb") {
		pack_take_at(slot, index, 1);
		int tx = MOB_X[m] + DX[MOB_DIR[m]] * 3;
		int ty = MOB_Y[m] + DY[MOB_DIR[m]] * 3;
		ev(Ev.proj, w, MOB_X[m], MOB_Y[m], tx, ty, 7, -1, "");
		fx_at(w, tx, ty, Fx.poison, 30);
		sound_at(w, tx, ty, "boom");
		for (int dy = -2; dy <= 2; ++dy) {
			for (int dx = -2; dx <= 2; ++dx) {
				int o = mob_at(w, tx + dx, ty + dy);
				if (o >= 0 && hostile(m, o)) { deal(o, d.pow, m, DMG_POISON, false); if (mob_alive(o)) { MOB_POISON[o] = 50; } }
			}
		}
		shake(slot, 4);
		return;
	}
	if (eff == "bottle") {
		pack_take_at(slot, index, 1);
		array<string> msgs = ["HELP. I am trapped in a bottle factory.", "If found, return to Thibodeaux. He owes me 3 gold.",
			"The Golden Carp bites best in the Bayou, when your Fishing is keen.", "Dear diary, today a gator ate my boat. Again.",
			"Six scales. Six tyrants. One shrine. -M", "There is no treasure. Stop reading bottles.", "Nice try, raccoon."];
		say_to(slot, `The message reads: "${msgs[rnd(msgs.size())]}"`, C_CYAN);
		if (rnd(4) == 0) {
			int g = 30 + rnd(60);
			c.gold = c.gold + g;
			say_to(slot, `Tucked inside: ${g} gold!`, C_YELLOW);
		}
		return;
	}
	if (eff == "squeak") { say_to(slot, "Squeak!", C_YELLOW); sound_at(w, MOB_X[m], MOB_Y[m], "squeak"); return; }
	if (eff == "music") {
		say_to(slot, `You play the ${d.name}. Nearby creatures pause to listen. (It is not good.)`, C_PURPLE);
		sound_at(w, MOB_X[m], MOB_Y[m], "kazoo");
		for (int dy = -4; dy <= 4; ++dy) {
			for (int dx = -4; dx <= 4; ++dx) {
				int o = mob_at(w, MOB_X[m] + dx, MOB_Y[m] + dy);
				if (o >= 0 && MOB_PLAYER[o] < 0 && !MONS[MOB_KIND[o]].boss) { MOB_NEXT[o] = TICK + 30; MOB_TARGET[o] = -1; }
			}
		}
		return;
	}
	if (eff == "spellbook") { say_to(slot, "Open your spellbook with B.", C_GRAY); return; }
	say_to(slot, `You can't think of a use for ${article(item_base_name(it))}.`, C_GRAY);
}

void drop_item(int slot, int index) {
	Character& c = CHARS[slot];
	if (index < 0 || index >= c.pack.size()) { return; }
	int m = c.mob;
	Item it = c.pack[index];
	c.pack.remove(index);
	c.version = c.version + 1;
	gi_add(it, MOB_WORLD[m], MOB_X[m], MOB_Y[m], -1, 3000);
	say_to(slot, `You drop ${item_name(it)}.`, C_GRAY);
}

void pick_up(int slot) {
	Character& c = CHARS[slot];
	int m = c.mob;
	if (mob_ghost(m)) {
		// a ghost may take back its own gold pouch
	}
	int w = MOB_WORLD[m];
	int got = 0;
	// what is in reach, most important first: quest items, gold, then by rarity
	array<int> reach;
	array<int> rank;
	for (int i = 0; i < GI_ITEM.size(); ++i) {
		if (GI_ITEM[i].base < 0 || GI_W[i] != w || cheb(GI_X[i], GI_Y[i], MOB_X[m], MOB_Y[m]) > 1) { continue; }
		if (GI_OWNER[i] >= 0 && GI_OWNER[i] != slot) { continue; }
		if (mob_ghost(m) && GI_OWNER[i] != slot) { continue; }
		int kind = ITEMS[GI_ITEM[i].base].kind;
		int r = kind == Kind.quest ? 100 : (kind == Kind.gold ? 90 : GI_ITEM[i].rarity * 10);
		int at = reach.size();
		while (at > 0 && rank[at - 1] < r) { at = at - 1; }
		reach.push(0);
		rank.push(0);
		for (int k = reach.size() - 1; k > at; --k) {
			reach[k] = reach[k - 1];
			rank[k] = rank[k - 1];
		}
		reach[at] = i;
		rank[at] = r;
	}
	for (int ri = 0; ri < reach.size(); ++ri) {
		int i = reach[ri];
		Item it = GI_ITEM[i];
		if (!pack_add(slot, it)) { say_to(slot, "Your backpack is full.", C_RED); break; }
		ev(Ev.pickup, w, GI_X[i], GI_Y[i], GI_SER[i], it.base, 0, -1, "");
		string rare = it.rarity >= 2 ? ` (${RARITY_NAME[it.rarity]})` : "";
		say_to(slot, `You pick up ${item_name(it)}${rare}.`, it.rarity >= 3 ? C_PURPLE : (it.rarity >= 2 ? C_BLUE : C_WHITE));
		sound_at(w, GI_X[i], GI_Y[i], ITEMS[it.base].kind == Kind.gold ? "coin" : "pickup");
		gi_remove(i);
		got = got + 1;
	}
	if (got == 0) { say_to(slot, "There is nothing here to pick up.", C_GRAY); }
}

// nearest hostile in sight, cycling from the current target
void cycle_target(int slot) {
	int m = CHARS[slot].mob;
	int w = MOB_WORLD[m];
	int cur = MOB_TARGET[m];
	int best = -1;
	int bestd = 999;
	int after = -1;
	int afterd = 999;
	int curd = mob_live(cur) ? cheb(MOB_X[m], MOB_Y[m], MOB_X[cur], MOB_Y[cur]) : -1;
	for (int dy = -9; dy <= 9; ++dy) {
		for (int dx = -9; dx <= 9; ++dx) {
			int o = mob_at(w, MOB_X[m] + dx, MOB_Y[m] + dy);
			if (o < 0 || o == m || !hostile(m, o) || (MOB_FLAGS[o] & MF_HIDDEN) != 0) { continue; }
			int d = cheb(0, 0, dx, dy) * 1000 + MOB_SER[o] % 1000;
			if (!los(w, MOB_X[m], MOB_Y[m], MOB_X[o], MOB_Y[o])) { continue; }
			if (d < bestd) { bestd = d; best = o; }
			int cd = curd * 1000 + (mob_live(cur) ? MOB_SER[cur] % 1000 : 0);
			if (o != cur && d > cd && d < afterd) { afterd = d; after = o; }
		}
	}
	int pick = after >= 0 ? after : best;
	MOB_TARGET[m] = pick;
	if (pick >= 0) { say_to(slot, `Target: ${mob_name(pick)}`, C_ORANGE); }
	else { say_to(slot, "No enemies in sight.", C_GRAY); }
}

void toggle_hide(int slot) {
	int m = CHARS[slot].mob;
	if ((MOB_FLAGS[m] & MF_HIDDEN) != 0) {
		MOB_FLAGS[m] = MOB_FLAGS[m] & ~MF_HIDDEN;
		say_to(slot, "You step out of hiding.", C_GRAY);
		return;
	}
	int bonus = TILES_DEF[tile_now(MOB_WORLD[m], MOB_X[m], MOB_Y[m])].hide ? 200 : 0;
	if (skill_check(slot, Sk.stealth, 250 - bonus)) {
		MOB_FLAGS[m] = MOB_FLAGS[m] | MF_HIDDEN;
		MOB_TARGET[m] = -1;
		say_to(slot, "You have hidden yourself well.", C_GREEN);
		fx_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], Fx.smoke, 6);
	} else {
		say_to(slot, "You can't seem to hide here.", C_GRAY);
	}
}

void party_invite(int slot, int other) {
	if (other < 0 || other >= CHARS.size() || !CHARS[other].active || other == slot) { return; }
	CHARS[other].invite = slot;
	say_to(other, `${CHARS[slot].name} invites you to a party. Type /accept to join.`, C_CYAN);
	say_to(slot, `You invite ${CHARS[other].name} to your party.`, C_CYAN);
}

void party_accept(int slot) {
	Character& c = CHARS[slot];
	int leader = c.invite;
	if (leader < 0 || !CHARS[leader].active) { say_to(slot, "No party invitation.", C_GRAY); return; }
	if (CHARS[leader].party < 0) { CHARS[leader].party = leader; }
	c.party = CHARS[leader].party;
	c.invite = -1;
	c.version = c.version + 1;
	CHARS[leader].version = CHARS[leader].version + 1;
	for (int s = 0; s < CHARS.size(); ++s) {
		if (CHARS[s].active && CHARS[s].party == c.party) { say_to(s, `${c.name} joins the party!`, C_CYAN); }
	}
}

void party_leave(int slot) {
	Character& c = CHARS[slot];
	if (c.party < 0) { return; }
	int p = c.party;
	c.party = -1;
	c.version = c.version + 1;
	say_to(slot, "You leave the party.", C_CYAN);
	int left = 0;
	for (int s = 0; s < CHARS.size(); ++s) {
		if (CHARS[s].active && CHARS[s].party == p) { left = left + 1; say_to(s, `${c.name} leaves the party.`, C_CYAN); }
	}
	if (left <= 1) {
		for (int s = 0; s < CHARS.size(); ++s) {
			if (CHARS[s].party == p) { CHARS[s].party = -1; CHARS[s].version = CHARS[s].version + 1; }
		}
	}
}

void chat(int slot, string text) {
	int m = CHARS[slot].mob;
	string clean = text.size() > 120 ? text.substr(0, 120) : text;
	if (clean.starts_with("/")) { chat_command(slot, clean); return; }
	ev(Ev.chat, MOB_WORLD[m], MOB_X[m], MOB_Y[m], slot, CHARS[slot].party, 0, -1, CHARS[slot].name + ": " + clean);
}

void chat_command(int slot, string cmd) {
	array<string> p = cmd.split(" ");
	string verb = p[0];
	if (verb == "/pvp") {
		Character& c = CHARS[slot];
		c.pvp = !c.pvp;
		int m = c.mob;
		if (c.pvp) { MOB_FLAGS[m] = MOB_FLAGS[m] | MF_PVP; } else { MOB_FLAGS[m] = MOB_FLAGS[m] & ~MF_PVP; }
		c.version = c.version + 1;
		say_to(slot, c.pvp ? "PvP ON: other flagged gators may attack you, and you them." : "PvP OFF: you are safe from other players.", c.pvp ? C_RED : C_GREEN);
		return;
	}
	if (verb == "/accept") { party_accept(slot); return; }
	if (verb == "/leave") { party_leave(slot); return; }
	if (verb == "/who") {
		string names = "";
		for (int s = 0; s < CHARS.size(); ++s) {
			if (CHARS[s].active) { names = names + (names == "" ? "" : ", ") + CHARS[s].name; }
		}
		say_to(slot, "Online: " + names, C_CYAN);
		return;
	}
	if (verb == "/invite" && p.size() > 1) {
		for (int s = 0; s < CHARS.size(); ++s) {
			if (CHARS[s].active && string::to_lower(CHARS[s].name) == string::to_lower(p[1])) { party_invite(slot, s); return; }
		}
		say_to(slot, "No gator by that name.", C_GRAY);
		return;
	}
	if (verb == "/p" && p.size() > 1) {
		int m = CHARS[slot].mob;
		string msg = cmd.substr(3, cmd.size() - 3);
		ev(Ev.chat, MOB_WORLD[m], MOB_X[m], MOB_Y[m], slot, CHARS[slot].party, 1, -1, CHARS[slot].name + " (party): " + msg);
		return;
	}
	if (verb == "/home") {
		Character& c = CHARS[slot];
		int m = c.mob;
		int t = tile_now(MOB_WORLD[m], MOB_X[m], MOB_Y[m]);
		if (town_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m]) >= 0 || TILES_DEF[t].station != 0) {
			c.home_world = MOB_WORLD[m];
			c.home_x = MOB_X[m];
			c.home_y = MOB_Y[m];
			say_to(slot, "You bind your soul to this place. Recall brings you here.", C_PURPLE);
		} else {
			say_to(slot, "You can only bind your home in a town.", C_GRAY);
		}
		return;
	}
	say_to(slot, "Commands: /pvp /who /invite name /accept /leave /p message /home", C_GRAY);
}

// ------------------------------------------------------------- dispatch ---
// one queued action string; the wire format of player intents
void do_action(int slot, string a) {
	if (a.size() == 0 || slot >= CHARS.size() || !CHARS[slot].active) { return; }
	int m = CHARS[slot].mob;
	if (!mob_live(m)) { return; }
	string op = a.substr(0, 1);
	string rest = a.substr(1, a.size() - 1);
	array<string> args = rest.split(",");
	int n0 = args.size() > 0 ? args[0].to_int(-1) : -1;
	int n1 = args.size() > 1 ? args[1].to_int(-1) : -1;
	if (op == "A") {
		int t = find_npc_ser(n0);
		if (t >= 0 && hostile(m, t)) { MOB_TARGET[m] = t; }
	}
	else if (op == "T") { cycle_target(slot); }
	else if (op == "C") { player_cast(slot, n0); }
	else if (op == "U") { use_item(slot, n0); }
	else if (op == "R") { unequip(slot, n0); }
	else if (op == "D") { drop_item(slot, n0); }
	else if (op == "G") { pick_up(slot); }
	else if (op == "E") { interact(slot); }
	else if (op == "B") { shop_buy(slot, find_npc_ser(n0), n1); }
	else if (op == "S") { shop_sell(slot, find_npc_ser(n0), n1); }
	else if (op == "K") { craft_start(slot, n0); }
	else if (op == "H") { healer_service(slot, find_npc_ser(n0)); }
	else if (op == "Q") { int npc = find_npc_ser(n0); if (npc >= 0 && npc_in_reach(slot, npc)) { quest_talk(slot, MOB_ROLE[npc]); } }
	else if (op == "W") { bank_deposit(slot, find_npc_ser(n0), n1); }
	else if (op == "X") { bank_withdraw(slot, find_npc_ser(n0), n1); }
	else if (op == "Z") { toggle_hide(slot); }
	else if (op == "N") { use_bandage(slot); }
	else if (op == ":") { chat(slot, rest); }
	else if (op == "!") { MOB_TARGET[m] = -1; }
}

void use_bandage(int slot) {
	Character& c = CHARS[slot];
	for (int i = 0; i < c.pack.size(); ++i) {
		if (c.pack[i].base == I_BANDAGE) { use_item(slot, i); return; }
	}
	say_to(slot, "You have no bandages.", C_GRAY);
}

// per-tick player upkeep: queued actions, held movement, ice slides, auto-attack
void players_tick() {
	for (int slot = 0; slot < CHARS.size(); ++slot) {
		Character& c = CHARS[slot];
		if (!c.active) { continue; }
		int m = c.mob;
		if (!mob_live(m)) { continue; }
		c.played_ticks = c.played_ticks + 1;
		array<string> q = IN_QUEUE[slot];
		IN_QUEUE[slot] = [];
		for (int i = 0; i < q.size() && i < 8; ++i) { do_action(slot, q[i]); }
		if (!mob_live(m)) { continue; }
		actions_tick(slot);
		// ice keeps you going
		if (MOB_AUX[m] > 0 && TICK >= MOB_NEXT[m]) {
			int dir = MOB_AUX[m] - 1;
			if (!mob_step(m, dir)) { MOB_AUX[m] = 0; }
			else { on_enter(m); }
			continue;
		}
		// a predicting client's steps, one per step time, in the order it took them
		if (slot < IN_STEP_SEQ.size() && IN_STEP_SEQ[slot].size() > 0) {
			if (TICK >= MOB_NEXT[m]) {
				int seq = IN_STEP_SEQ[slot][0];
				int sdir = IN_STEP_DIR[slot][0];
				IN_STEP_SEQ[slot].remove(0);
				IN_STEP_DIR[slot].remove(0);
				player_move(slot, sdir);
				IN_ACK[slot] = seq;
				if (IN_STEP_SEQ[slot].size() == 0 && IN_DROPPED[slot] > IN_ACK[slot]) { IN_ACK[slot] = IN_DROPPED[slot]; }
			}
		} else {
			int dir = IN_MOVE[slot];
			if (dir >= 0 && TICK >= MOB_NEXT[m]) { player_move(slot, dir); }
		}
		// war mode: swing at the target whenever it is in reach
		int t = MOB_TARGET[m];
		if (t >= 0 && !mob_ghost(m)) {
			if (!mob_alive(t) || MOB_WORLD[t] != MOB_WORLD[m] || cheb(MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]) > 16) {
				MOB_TARGET[m] = -1;
			} else if (TICK >= MOB_ATK[m] && hostile(m, t)) {
				int d = cheb(MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]);
				if (d <= attack_range(m) && (d <= 1 || los(MOB_WORLD[m], MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]))) { attack(m, t); }
			}
		}
		if (c.buff_str > 0 && TICK >= c.buff_str) { c.buff_str = 0; }
	}
}
