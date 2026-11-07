// Gator Quest - the endgame run: a well-equipped hero fights every realm's tyrant
// in turn, loots the Relic Scales and turns them in at the Shrine of the Six.
// It is both a test (the whole main quest can be finished) and the balance
// instrument for the bosses (time to kill, potions, deaths per fight).

import "gq_serve.jai";

int boss_of_world(int w) {
	int kind = WORLDS[w].boss;
	for (int m = 0; m < MOB_KIND.size(); ++m) {
		if (MOB_KIND[m] == kind && MOB_WORLD[m] == w && MOB_HP[m] > 0) { return m; }
	}
	return -1;
}

int count_item(int slot, int base) { return pack_count(slot, base); }

void equip_new(int slot, string name) {
	Item it = make_item(iid(name), 1);
	CHARS[slot].worn[ITEMS[it.base].slot] = it;
}

// a hero trained and geared for the given tier (1 = a fresh hero, 5 = endgame)
void outfit_hero(int slot, int tier) {
	Character& c = CHARS[slot];
	int sk = 400 + tier * 110;
	c.skill[Sk.swords] = sk;
	c.skill[Sk.tactics] = sk;
	c.skill[Sk.parrying] = sk - 100;
	c.skill[Sk.healing] = sk - 100;
	c.skill[Sk.resist] = sk - 150;
	c.str = 50 + tier * 9;
	c.dex = 35 + tier * 7;
	c.intel = 30 + tier * 5;
	array<string> blade = ["Short Sword", "Cutlass", "Broadsword", "Katana", "Gator Tooth Blade", "Obsidian Greatsword"];
	equip_new(slot, blade[imin(5, tier)]);
	equip_new(slot, tier >= 4 ? "Plate Cuirass" : (tier >= 2 ? "Chainmail Shirt" : "Leather Tunic"));
	equip_new(slot, tier >= 3 ? "Iron Helm" : "Bucket Helm");
	equip_new(slot, tier >= 4 ? "Plate Gauntlets" : "Chain Gloves");
	equip_new(slot, tier >= 4 ? "Plate Greaves" : "Chain Leggings");
	equip_new(slot, tier >= 4 ? "Plate Boots" : "Leather Boots");
	equip_new(slot, tier >= 4 ? "Tower Shield" : (tier >= 3 ? "Kite Shield" : "Wooden Shield"));
	player_refresh(slot);
	int m = c.mob;
	MOB_HP[m] = MOB_HPMAX[m];
}

// stand next to the boss and fight: target it, drink, bandage; returns ticks to kill, -1 on timeout
int fight(int slot, int boss, int limit, array<int>& stats) {
	int me = CHARS[slot].mob;
	int w = MOB_WORLD[boss];
	int start = TICK;
	int potion = iid("Greater Heal Potion");
	while (TICK - start < limit) {
		if (!mob_live(boss) || MOB_HP[boss] <= 0 || MOB_KIND[boss] != WORLDS[w].boss) { return TICK - start; }
		if (mob_ghost(me)) {
			stats[1] = stats[1] + 1;
			player_resurrect(slot, 100);
			array<int> p = find_free_spot(w, MOB_X[boss] + 3, MOB_Y[boss], 6, LAND_MASK);
			mob_place(me, w, p[0], p[1]);
		}
		intent_move(slot, -1);
		int d = cheb(MOB_X[me], MOB_Y[me], MOB_X[boss], MOB_Y[boss]);
		MOB_TARGET[me] = boss;
		if (d > 1) { intent_move(slot, dir_of(MOB_X[boss] - MOB_X[me], MOB_Y[boss] - MOB_Y[me])); }
		if (d > 8) {
			array<int> p = find_free_spot(w, MOB_X[boss] + 1, MOB_Y[boss], 4, LAND_MASK);
			mob_place(me, w, p[0], p[1]);
		}
		int hp_pct = MOB_HP[me] * 100 / imax(1, MOB_HPMAX[me]);
		if (hp_pct < 45) {
			int idx = pack_index_of(slot, potion);
			if (idx >= 0 && TICK >= CHARS[slot].potion_t) {
				intent_act(slot, "U" + to_string(idx));
				stats[2] = stats[2] + 1;
			}
		}
		if (hp_pct < 75 && CHARS[slot].action == 0 && pack_has(slot, I_BANDAGE, 1)) { intent_act(slot, "N"); }
		sim_tick();
	}
	return -1;
}

void loot_everything(int slot) {
	int me = CHARS[slot].mob;
	for (int pass = 0; pass < 30; ++pass) {
		int gi = -1;
		int best = 99;
		for (int i = 0; i < GI_ITEM.size(); ++i) {
			if (GI_ITEM[i].base < 0 || GI_W[i] != MOB_WORLD[me]) { continue; }
			int d = cheb(GI_X[i], GI_Y[i], MOB_X[me], MOB_Y[me]);
			if (d < best && d <= 12) { best = d; gi = i; }
		}
		if (gi < 0) { return; }
		mob_place(me, GI_W[gi], GI_X[gi], GI_Y[gi]);
		pick_up(slot);
	}
}

// ---------------------------------------------------------------- tour ---
// frame captures of the whole game for the docs: every realm, night, a boss fight, fishing, a HUD pane
void tour_ticks(int n) {
	for (int t = 0; t < n; ++t) {
		NOW_MS = NOW_MS + TICK_MS;
		sim_tick();
		CV.rx_ms = NOW_MS;
		cv_apply(CV, snapshot_for(0));
		for (int f = 0; f < 3; ++f) {
			NOW_MS = NOW_MS + 33;
			check_dialog(CV);
			process_events(CV);
			particles_update();
			FRAME = FRAME + 1;
		}
		NOW_MS = NOW_MS - 99;
	}
}

void tour_shot(string dir, string name) {
	NOW_MS = NOW_MS + 60;
	render_frame(CV);
	png_write_canvas(dir + "/" + name + ".png", 3);
	write_file(dir + "/" + name + ".txt", sketch_frame(CV));
	write_file(dir + "/" + name + ".ans", compose_frame(CV));
	print("  " + name);
}

int run_tour() {
	string dir = arg_value("out", data_root() + "/shots/tour");
	string size = arg_value("screen", "200x56");
	array<string> wh = size.split("x");
	make_dirs(dir);
	sim_init(arg_value("seed", "1").to_int(1));
	BOTS = [];
	player_create(0, "Chomps", 0);
	CV = new ClientView();
	CV.slot = 0;
	NOW_MS = 0;
	layout(wh[0].to_int(200), wh.size() > 1 ? wh[1].to_int(56) : 56);
	int me = CHARS[0].mob;
	outfit_hero(0, 3);
	print("tour: " + dir);
	tour_ticks(20);
	tour_shot(dir, "01_swampton_morning");
	// every realm at its arrival point
	for (int w = 1; w < WORLD_COUNT; ++w) {
		travel(me, w, W_ARRIVE_X[w] + 3, W_ARRIVE_Y[w] + 2);
		tour_ticks(28);
		string n = string::to_lower(WORLDS[w].name);
		n.replace(" ", "_");
		n.replace(".", "");
		tour_shot(dir, `${w + 1 < 10 ? "0" : ""}${w + 1}_${n}`);
	}
	// night in Gatorland: torchlight and the forge glow
	travel(me, 0, W_ARRIVE_X[0], W_ARRIVE_Y[0]);
	TICK = TICK + (DAY_TICKS - time_of_day()) + DAY_TICKS * 15 / 24;
	tour_ticks(10);
	tour_shot(dir, "10_swampton_night");
	// a boss fight: the health bar, damage numbers, adds
	outfit_hero(0, 5);
	int boss = boss_of_world(1);
	if (boss >= 0) {
		array<int> p = find_free_spot(1, MOB_X[boss] + 1, MOB_Y[boss], 4, LAND_MASK);
		travel(me, 1, p[0], p[1]);
		MOB_TARGET[me] = boss;
		for (int t = 0; t < 70; ++t) {
			MOB_TARGET[me] = boss;
			tour_ticks(1);
		}
		tour_shot(dir, "11_boss_big_mama");
	}
	// fishing on the Bayou
	array<int> water = find_tile_near(me, 4, 0, 20);
	if (water[0] >= 0) {
		array<int> p = find_free_spot(MOB_WORLD[me], water[0], water[1] + 1, 4, LAND_MASK);
		mob_place(me, MOB_WORLD[me], p[0], p[1]);
		MOB_DIR[me] = dir_of(water[0] - p[0], water[1] - p[1]);
		MOB_TARGET[me] = -1;
		intent_act(0, "E");
		tour_ticks(12);
		tour_shot(dir, "12_fishing");
	}
	// the journal pane in town
	travel(me, 0, W_ARRIVE_X[0], W_ARRIVE_Y[0]);
	quest_talk(0, R_MAYOR);
	quest_talk(0, R_ORACLE);
	UI_MODE = "journal";
	tour_ticks(6);
	tour_shot(dir, "13_journal");
	UI_MODE = "play";
	print(`tour: done (STATE_HASH ${state_hash()})`);
	return 0;
}

int run_endgame() {
	int seed = arg_value("seed", "2").to_int(2);
	sim_init(seed);
	BOTS = [];
	player_create(0, "Hero", 0);
	int me = CHARS[0].mob;
	pack_add(0, make_item(iid("Greater Heal Potion"), 60));
	pack_add(0, make_item(I_BANDAGE, 80));
	pack_add(0, make_item(iid("Fire Resist Potion"), 5));
	CHARS[0].buff_fire = TICK + 100000;
	int oracle = find_role_npc(0, R_ORACLE);
	// take the Oracle's quests (Tourist, then Big Mama; the Six Scales needs Big Mama done)
	quest_talk(0, R_ORACLE);
	quest_talk(0, R_ORACLE);
	print("endgame: the six tyrants, one hero (VM or interpreter; both must agree)");
	print("  realm                 boss                    tier  ticks  deaths  potions  relic");
	array<int> order = [1, 2, 3, 4, 5, 6];
	int fails = 0;
	int total_deaths = 0;
	for (int k = 0; k < order.size(); ++k) {
		int w = order[k];
		int tier = imin(5, WORLDS[w].tier);
		outfit_hero(0, tier);
		travel(me, w, W_ARRIVE_X[w], W_ARRIVE_Y[w]);
		int boss = boss_of_world(w);
		for (int t = 0; t < 400 && boss < 0; ++t) { sim_tick(); boss = boss_of_world(w); }
		if (boss < 0) {
			print(`  ${WORLDS[w].name:<21} (no boss spawned)`);
			fails = fails + 1;
			continue;
		}
		string name = MONS[MOB_KIND[boss]].name;
		array<int> p = find_free_spot(w, MOB_X[boss] + 2, MOB_Y[boss], 6, LAND_MASK);
		mob_place(me, w, p[0], p[1]);
		array<int> stats = [0, 0, 0];
		int ticks = fight(0, boss, 6000, stats);
		loot_everything(0);
		bool relic = pack_has(0, RELICS[w - 1], 1);
		// keep what a boss hunter needs, sell off the rest (a player would visit the shops)
		Character& hc = CHARS[0];
		for (int i = hc.pack.size() - 1; i >= 0; --i) {
			int kind = ITEMS[hc.pack[i].base].kind;
			if (kind != Kind.quest && kind != Kind.potion && hc.pack[i].base != I_BANDAGE && hc.pack[i].base != I_SPELLBOOK) {
				hc.gold = hc.gold + sell_price(hc.pack[i]) * hc.pack[i].qty;
				hc.pack.remove(i);
			}
		}
		total_deaths = total_deaths + stats[1];
		print(`  ${WORLDS[w].name:<21} ${name:<23} ${tier:>4}  ${ticks:>5}  ${stats[1]:>6}  ${stats[2]:>7}  ${relic ? "yes" : "NO"}`);
		if (ticks < 0 || !relic) { fails = fails + 1; }
		// back to the shrine between realms: quest progress and a top-up
		travel(me, 0, MOB_X[oracle], MOB_Y[oracle] + 1);
		quest_talk(0, R_ORACLE);
		pack_add(0, make_item(iid("Greater Heal Potion"), 10));
	}
	// the Six Scales
	quest_talk(0, R_ORACLE);
	quest_talk(0, R_ORACLE);
	bool crowned = CHARS[0].quest_stage[Q_SIX_SCALES] == 99;
	bool crown = CHARS[0].worn[Slot.head].base == I_CROWN_SWAMP || pack_has(0, I_CROWN_SWAMP, 1);
	print(`  the Six Scales: ${crowned ? "turned in" : "NOT turned in"}; crown ${crown ? "received" : "missing"}; title "${player_title(0)}"`);
	if (!crowned || !crown) { fails = fails + 1; }
	print(`endgame: ${fails == 0 ? "complete" : to_string(fails) + " problems"}, ${total_deaths} deaths in six boss fights, tick ${TICK}, STATE_HASH ${state_hash()}`);
	return fails;
}
