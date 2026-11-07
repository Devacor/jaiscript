// Gator Quest - items: names, random affixes and rarities, the backpack and
// paperdoll, equipment bonuses, items on the ground and loot drops.

import "gq_mobs.jai";

int PACK_MAX = 40;

Item make_item(int base, int qty) {
	Item it = Item(base, qty);
	it.rarity = ITEMS[base].rarity;
	it.ser = NEXT_SER;
	NEXT_SER = NEXT_SER + 1;
	return it;
}

string item_base_name(Item it) {
	string n = ITEMS[it.base].name;
	if (it.prefix >= 0) { n = AFFIXES[it.prefix].name + " " + n; }
	if (it.suffix >= 0) { n = n + " " + AFFIXES[it.suffix].name; }
	return n;
}

string item_name(Item it) {
	if (it.base < 0) { return "nothing"; }
	string n = item_base_name(it);
	if (it.bonus > 0 && (ITEMS[it.base].kind == Kind.weapon || ITEMS[it.base].slot >= 0)) { n = n + " +" + to_string(it.bonus); }
	if (it.qty > 1 || ITEMS[it.base].kind == Kind.gold) { n = to_string(it.qty) + " " + n; }
	return n;
}

// sum of one affix code on an item (fixed legendary affixes, prefix, suffix)
int item_stat(Item it, int code) {
	if (it.base < 0) { return 0; }
	int v = 0;
	ItemDef d = ITEMS[it.base];
	for (int k = 0; k < d.aff_code.size(); ++k) {
		if (d.aff_code[k] == code) { v = v + d.aff_val[k]; }
	}
	if (it.prefix >= 0 && AFFIXES[it.prefix].code == code) { v = v + it.pmag; }
	if (it.suffix >= 0 && AFFIXES[it.suffix].code == code) { v = v + it.smag; }
	return v;
}

int item_value(Item it) {
	if (it.base < 0) { return 0; }
	int v = ITEMS[it.base].value;
	if (it.prefix >= 0) { v = v + 20 + iabs(it.pmag) * 6; }
	if (it.suffix >= 0) { v = v + 20 + iabs(it.smag) * 6; }
	v = v * (100 + it.rarity * 60) / 100 + it.bonus * 15;
	if (it.prefix >= 0 && AFFIXES[it.prefix].name == "Soggy") { v = v / 4; }
	return imax(1, v);
}

bool item_equippable(Item it) { return it.base >= 0 && ITEMS[it.base].slot >= 0; }
bool item_stacks(int base) { return ITEMS[base].stack; }

// ---- random magic items
int roll_rarity(int luck, int floor_rarity) {
	int r = rnd(1000) - luck * 2;
	int rar = 0;
	if (r < 4) { rar = 3; }
	else if (r < 30) { rar = 2; }
	else if (r < 180) { rar = 1; }
	return imax(rar, floor_rarity);
}

int roll_affix(bool prefix, bool weapon, int tier) {
	array<int> fits;
	for (int i = 0; i < AFFIXES.size(); ++i) {
		AffixDef a = AFFIXES[i];
		if (a.prefix != prefix || a.tier > tier + 1) { continue; }
		if (a.applies == 1 && !weapon) { continue; }
		if (a.applies == 2 && weapon) { continue; }
		fits.push(i);
	}
	if (fits.size() == 0) { return -1; }
	return fits[rnd(fits.size())];
}

int roll_mag(int affix, int rarity) {
	AffixDef a = AFFIXES[affix];
	int v = rnd_range(a.lo, a.hi);
	if (rarity >= 3) { v = a.hi + (a.hi - a.lo) / 2; }
	return v;
}

// a random weapon/armor/jewelry piece of about this tier
Item roll_equipment(int tier, int luck, int floor_rarity) {
	array<int> pool;
	for (int i = 0; i < ITEMS.size(); ++i) {
		ItemDef d = ITEMS[i];
		if (d.slot < 0 || d.rarity != 0 || d.kind == Kind.trinket) { continue; }
		if (d.tier > tier || d.tier < tier - 2) { continue; }
		pool.push(i);
	}
	if (pool.size() == 0) { return make_item(I_DAGGER, 1); }
	Item it = make_item(pool[rnd(pool.size())], 1);
	int rar = roll_rarity(luck, floor_rarity);
	it.rarity = rar;
	bool weapon = ITEMS[it.base].kind == Kind.weapon;
	if (rar == 1) {
		if (rnd(2) == 0) { it.prefix = roll_affix(true, weapon, tier); }
		else { it.suffix = roll_affix(false, weapon, tier); }
	} else if (rar >= 2) {
		it.prefix = roll_affix(true, weapon, tier);
		it.suffix = roll_affix(false, weapon, tier);
	}
	if (it.prefix >= 0) { it.pmag = roll_mag(it.prefix, rar); }
	if (it.suffix >= 0) { it.smag = roll_mag(it.suffix, rar); }
	if (rar >= 3) { it.bonus = 1 + rnd(3); }
	return it;
}

// consumables and bits for loot tables
Item roll_consumable(int tier) {
	int r = rnd(100);
	if (r < 35) { return make_item(REAGENTS[rnd(REAGENTS.size())], 1 + rnd(3)); }
	if (r < 60) { return make_item(tier >= 3 ? I_HEAL : I_MINORHEAL, 1); }
	if (r < 70) { return make_item(I_MANA, 1); }
	if (r < 78) { return make_item(I_BANDAGE, 2 + rnd(4)); }
	if (r < 86) { return make_item(I_ARROW, 5 + rnd(10)); }
	// a spell scroll of a circle the tier can carry
	array<int> scrolls;
	for (int i = 0; i < ITEMS.size(); ++i) {
		if (ITEMS[i].kind == Kind.scroll && ITEMS[i].eff == 10 && SPELLS[ITEMS[i].pow].circle <= tier + 1) { scrolls.push(i); }
	}
	if (scrolls.size() == 0) { return make_item(I_MINORHEAL, 1); }
	return make_item(scrolls[rnd(scrolls.size())], 1);
}

// ---- backpack
int pack_count(int slot, int base) {
	int n = 0;
	array<Item>& pack = CHARS[slot].pack;
	for (int i = 0; i < pack.size(); ++i) {
		if (pack[i].base == base) { n = n + pack[i].qty; }
	}
	return n;
}

bool pack_has(int slot, int base, int qty) { return pack_count(slot, base) >= qty; }

// add to the backpack; stacks merge. false = no room (the caller drops it at your feet)
bool pack_add(int slot, Item it) {
	Character& c = CHARS[slot];
	if (ITEMS[it.base].kind == Kind.gold) {
		c.gold = c.gold + it.qty;
		c.version = c.version + 1;
		return true;
	}
	if (item_stacks(it.base)) {
		for (int i = 0; i < c.pack.size(); ++i) {
			if (c.pack[i].base == it.base && c.pack[i].prefix < 0 && c.pack[i].suffix < 0) {
				c.pack[i].qty = c.pack[i].qty + it.qty;
				c.version = c.version + 1;
				return true;
			}
		}
	}
	if (c.pack.size() >= PACK_MAX) { return false; }
	c.pack.push(it);
	c.version = c.version + 1;
	return true;
}

bool pack_take(int slot, int base, int qty) {
	if (!pack_has(slot, base, qty)) { return false; }
	Character& c = CHARS[slot];
	int left = qty;
	for (int i = c.pack.size() - 1; i >= 0 && left > 0; --i) {
		if (c.pack[i].base != base) { continue; }
		int take = imin(left, c.pack[i].qty);
		c.pack[i].qty = c.pack[i].qty - take;
		left = left - take;
		if (c.pack[i].qty <= 0) { c.pack.remove(i); }
	}
	c.version = c.version + 1;
	return true;
}

// remove qty from one pack slot; returns what was taken
Item pack_take_at(int slot, int index, int qty) {
	Character& c = CHARS[slot];
	Item out = c.pack[index];
	int take = imin(qty, c.pack[index].qty);
	out.qty = take;
	c.pack[index].qty = c.pack[index].qty - take;
	if (c.pack[index].qty <= 0) { c.pack.remove(index); }
	c.version = c.version + 1;
	return out;
}

// ---- paperdoll
int player_bonus(int slot, int code) {
	int v = 0;
	array<Item>& worn = CHARS[slot].worn;
	for (int i = 0; i < worn.size(); ++i) {
		if (worn[i].base >= 0) { v = v + item_stat(worn[i], code); }
	}
	return v;
}

int player_armor(int slot) {
	int a = 0;
	array<Item>& worn = CHARS[slot].worn;
	for (int i = 0; i < worn.size(); ++i) {
		if (worn[i].base >= 0) {
			a = a + ITEMS[worn[i].base].armor;
			if (ITEMS[worn[i].base].kind != Kind.weapon) { a = a + worn[i].bonus; }
		}
	}
	return a + player_bonus(slot, 9);
}

int player_str(int slot) { return CHARS[slot].str + player_bonus(slot, 5); }
int player_dex(int slot) { return CHARS[slot].dex + player_bonus(slot, 6); }
int player_int(int slot) { return CHARS[slot].intel + player_bonus(slot, 7); }
int player_luck(int slot) { return player_bonus(slot, 12); }

// recompute hp/mana caps after gear or stat changes
void player_refresh(int slot) {
	Character& c = CHARS[slot];
	int m = c.mob;
	if (!mob_live(m)) { return; }
	int hpmax = 40 + player_str(slot) + player_bonus(slot, 10);
	int mpmax = player_int(slot) + player_bonus(slot, 11);
	MOB_HPMAX[m] = imax(10, hpmax);
	MOB_MPMAX[m] = imax(0, mpmax);
	if (MOB_HP[m] > MOB_HPMAX[m]) { MOB_HP[m] = MOB_HPMAX[m]; }
	if (MOB_MP[m] > MOB_MPMAX[m]) { MOB_MP[m] = MOB_MPMAX[m]; }
	c.version = c.version + 1;
}

Item worn_weapon(int slot) { return CHARS[slot].worn[Slot.weapon]; }

bool equip_from_pack(int slot, int index) {
	Character& c = CHARS[slot];
	if (index < 0 || index >= c.pack.size()) { return false; }
	Item it = c.pack[index];
	if (!item_equippable(it)) { return false; }
	int s = ITEMS[it.base].slot;
	Item old = c.worn[s];
	c.worn[s] = it;
	c.pack.remove(index);
	if (old.base >= 0) { c.pack.push(old); }
	player_refresh(slot);
	say_to(slot, "You equip " + item_name(it) + ".", C_WHITE);
	return true;
}

bool unequip(int slot, int s) {
	Character& c = CHARS[slot];
	if (s < 0 || s >= c.worn.size() || c.worn[s].base < 0) { return false; }
	if (c.pack.size() >= PACK_MAX) { say_to(slot, "Your backpack is full.", C_RED); return false; }
	Item it = c.worn[s];
	c.pack.push(it);
	c.worn[s] = Item();
	player_refresh(slot);
	say_to(slot, "You take off " + item_name(it) + ".", C_WHITE);
	return true;
}

// ---- ground items
int gi_add(Item it, int w, int x, int y, int owner, int ttl) {
	int i = -1;
	for (int k = 0; k < GI_ITEM.size(); ++k) {
		if (GI_ITEM[k].base < 0) { i = k; break; }
	}
	if (i < 0) {
		i = GI_ITEM.size();
		GI_ITEM.push(Item());
		GI_W.push(0);
		GI_X.push(0);
		GI_Y.push(0);
		GI_SER.push(0);
		GI_EXPIRE.push(0);
		GI_OWNER.push(-1);
	}
	GI_ITEM[i] = it;
	GI_W[i] = w;
	GI_X[i] = x;
	GI_Y[i] = y;
	GI_SER[i] = NEXT_SER;
	NEXT_SER = NEXT_SER + 1;
	GI_EXPIRE[i] = TICK + ttl;
	GI_OWNER[i] = owner;
	return i;
}

void gi_remove(int i) { GI_ITEM[i] = Item(); }

int gi_count_at(int w, int x, int y) {
	int n = 0;
	for (int i = 0; i < GI_ITEM.size(); ++i) {
		if (GI_ITEM[i].base >= 0 && GI_W[i] == w && GI_X[i] == x && GI_Y[i] == y) { n = n + 1; }
	}
	return n;
}

void ground_tick() {
	if (TICK % 50 != 0) { return; }
	for (int i = 0; i < GI_ITEM.size(); ++i) {
		if (GI_ITEM[i].base >= 0 && TICK >= GI_EXPIRE[i]) { gi_remove(i); }
	}
}

// drop near (x, y): the tile itself, else a free neighbour (loot pops outward)
void drop_near(Item it, int w, int x, int y, int owner, int ttl) {
	// loot never lands where nobody can stand: open sky, walls, lava
	int k0 = walk_at(w, x, y);
	if (k0 == 0 || k0 == 3 || k0 == 5) {
		array<int> safe = find_spot(w, x, y, 8, LAND_MASK + 4);
		x = safe[0];
		y = safe[1];
	}
	int best_x = x;
	int best_y = y;
	int least = gi_count_at(w, x, y);
	for (int d = 0; d < 8 && least > 0; ++d) {
		int nx = x + DX[(d + rnd(8)) % 8];
		int ny = y + DY[(d + rnd(8)) % 8];
		if (!in_world(w, nx, ny)) { continue; }
		int k = walk_at(w, nx, ny);
		if (k == 0 || k == 3 || k == 5) { continue; }
		int n = gi_count_at(w, nx, ny);
		if (n < least) { least = n; best_x = nx; best_y = ny; }
	}
	int gi = gi_add(it, w, best_x, best_y, owner, ttl);
	if (it.rarity >= 2) { ev(Ev.fx, w, best_x, best_y, Fx.loot, it.rarity, 0, -1, ""); }
	ev(Ev.spawn, w, best_x, best_y, GI_SER[gi], x, y, -1, "");
}

// the monster's loot: gold, its drop table, sometimes a magic item
void loot_drop(int m, int killer_slot) {
	int kind = MOB_KIND[m];
	MonDef d = MONS[kind];
	int w = MOB_WORLD[m];
	int x = MOB_X[m];
	int y = MOB_Y[m];
	bool elite = (MOB_FLAGS[m] & MF_ELITE) != 0;
	int luck = killer_slot >= 0 ? player_luck(killer_slot) : 0;
	int goldfind = killer_slot >= 0 ? player_bonus(killer_slot, 18) : 0;
	int gold = rnd_range(d.gold_lo, d.gold_hi);
	if (elite) { gold = gold * 2 + 5; }
	gold = gold * (100 + goldfind) / 100;
	if (gold > 0) { drop_near(make_item(I_GOLD, gold), w, x, y, -1, 3000); }
	if (MON_AI[kind] == Ai.thief && MOB_AUX[m] > 0) {
		drop_near(make_item(I_GOLD, MOB_AUX[m]), w, x, y, -1, 3000);
		say_near(w, x, y, `${cap_first(mob_the(m))} drops the ${MOB_AUX[m]} gold it stole!`, C_YELLOW);
		MOB_AUX[m] = 0;
	}
	for (int k = 0; k < d.drop_item.size(); ++k) {
		if (d.boss || rnd(1000) < d.drop_chance[k] + luck) {
			int qty = item_stacks(d.drop_item[k]) ? 1 + rnd(2) : 1;
			drop_near(make_item(d.drop_item[k], qty), w, x, y, -1, 3000);
		}
	}
	if (d.fac != Fac.monster) { return; }
	int magic = 80 + d.tier * 30 + luck * 2 + (elite ? 300 : 0);
	if (rnd(1000) < magic) { drop_near(roll_equipment(d.tier, luck, elite ? 1 : 0), w, x, y, -1, 3000); }
	if (rnd(1000) < 180 + d.tier * 20) { drop_near(roll_consumable(d.tier), w, x, y, -1, 3000); }
	if (d.boss) {
		for (int k = 0; k < 3; ++k) { drop_near(roll_equipment(imin(5, d.tier + 1), luck, 2), w, x, y, -1, 6000); }
		for (int k = 0; k < 4; ++k) { drop_near(roll_consumable(d.tier), w, x, y, -1, 6000); }
	}
}
