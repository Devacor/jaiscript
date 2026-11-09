// Loot: rolling gear (rarity and affixes), potions, scrolls and gold; the pack; and what each
// potion and scroll does.

function roll_rarity(int floor) -> Rarity {
	// deeper floors tilt the odds toward the shiny end
	array<int> weights;
	for (auto r : RARITIES) {
		int weight = r.weight;
		if (r.id == "common") { weight = imax(10, weight - floor * 3); }
		if (r.id == "rare" || r.id == "epic") { weight += floor; }
		if (r.id == "legendary" && floor >= 6) { weight += 2; }
		weights.push(weight);
	}
	return RARITIES[pick_weighted(RNG, weights)];
}

function make_equipment(int floor) -> Item {
	Item item;
	int slot_roll = RNG.next(100);
	GearBase base;
	if (slot_roll < 40) {
		item.kind = "weapon";
		item.glyph = ")";
		base = WEAPON_BASES[imin(WEAPON_BASES.size() - 1, RNG.next(1 + imin(floor, 8) / 2))];
		item.atk = base.atk;
	} else if (slot_roll < 75) {
		item.kind = "armor";
		item.glyph = "[";
		base = ARMOR_BASES[imin(ARMOR_BASES.size() - 1, RNG.next(1 + imin(floor, 7) / 2))];
		item.def = base.def;
	} else {
		item.kind = "trinket";
		item.glyph = "=";
		base = TRINKET_BASES[RNG.next(TRINKET_BASES.size())];
		item.bonus[base.bonus] = base.power;
	}
	item.name = base.name;

	Rarity rarity = roll_rarity(floor);
	item.rarity = rarity.id;
	if (item.kind == "weapon") { item.atk += rarity.bump; }
	if (item.kind == "armor") { item.def += rarity.bump; }

	if (rarity.id == "legendary") {
		item.name = LEGENDARY_NAMES[item.kind];
		item.bonus["crit"] = item.bonus_of("crit") + 5;
		item.bonus["hp"] = item.bonus_of("hp") + 5;
	} else if (rarity.affixes > 0) {
		Affix prefix = PREFIXES[RNG.next(PREFIXES.size())];
		item.name = `${prefix.name} ${item.name}`;
		item.atk += prefix.atk;
		item.def += prefix.def;
		if (rarity.affixes > 1) {
			Affix suffix = SUFFIXES[RNG.next(SUFFIXES.size())];
			item.name = `${item.name} ${suffix.name}`;
			if (suffix.bonus == "atk") { item.atk += suffix.power; }
			else if (suffix.bonus == "def") { item.def += suffix.power; }
			else { item.bonus[suffix.bonus] = item.bonus_of(suffix.bonus) + suffix.power; }
		}
	}
	item.ink = rarity.ink;
	return item;
}

// the blade by the bones on floor 1: made by hand, so it costs the Rng nothing
string LASTLIGHT_NAME = "Lastlight Fang";

function make_lastlight() -> Item {
	Item blade;
	blade.kind = "weapon";
	blade.glyph = ")";
	blade.name = LASTLIGHT_NAME;
	blade.rarity = "rare";
	blade.atk = 3;
	blade.bonus["crit"] = 5;
	blade.ink = rarity_ink("rare");
	return blade;
}

function make_potion() -> Item {
	Item item;
	item.kind = "potion";
	item.glyph = "!";
	array<string> ids = POTIONS.keys();
	item.effect = ids[RNG.next(ids.size())];
	item.ink = INK_POTION;
	item.name = POTIONS[item.effect].name;
	return item;
}

function make_scroll() -> Item {
	Item item;
	item.kind = "scroll";
	item.glyph = "~";
	array<string> ids = SCROLLS.keys();
	item.effect = ids[RNG.next(ids.size())];
	item.ink = INK_SCROLL;
	item.name = SCROLLS[item.effect].name;
	return item;
}

function make_gold(int floor) -> Item {
	Item item;
	item.kind = "gold";
	item.glyph = "$";
	item.ink = INK_COIN;
	item.amount = RNG.roll(3, 8 + floor * 3);
	item.name = "gold";
	return item;
}

function make_floor_item(int floor) -> Item {
	int roll = RNG.next(100);
	if (roll < 35) { return make_equipment(floor); }
	if (roll < 60) { return make_potion(); }
	if (roll < 75) { return make_scroll(); }
	return make_gold(floor);
}

// identify by use: an unknown potion or scroll shows only its look
function item_label(Item item) -> string {
	if (item.kind == "potion") {
		if (G.known_potions.has(item.effect)) { return POTIONS[item.effect].name; }
		return `a ${G.potion_looks[item.effect]} potion`;
	}
	if (item.kind == "scroll") {
		if (G.known_scrolls.has(item.effect)) { return SCROLLS[item.effect].name; }
		return `a scroll labeled "${G.scroll_looks[item.effect]}"`;
	}
	if (item.kind == "gold") { return `${item.amount} gold`; }
	return item.name;
}

function item_stats_label(Item item) -> string {
	array<string> parts;
	if (item.atk != 0) { parts.push(`+${item.atk} atk`); }
	if (item.def != 0) { parts.push(`+${item.def} def`); }
	for (auto kv : item.bonus) {
		parts.push(kv.first == "burn" ? `${kv.second}% burn` : `+${kv.second} ${kv.first}`);
	}
	if (parts.size() == 0) { return ""; }
	return ` (${parts.join(", ")})`;
}

// --------------------------------------------------------------- the pack ---
int PACK_SIZE = 22;

function give_item(var& player, Item item) -> bool {
	if (item.kind == "gold") {
		player.gold += item.amount;
		log_ink(INK_GOLD, `You pocket ${item.amount} gold.`);
		fx_screen(`+${item.amount} GOLD`, INK_GOLD);
		return true;
	}
	if (player.inventory.size() >= PACK_SIZE) {
		log_msg("Your pack is stuffed full.");
		return false;
	}
	player.inventory.push(item);
	log_msg(`You pick up ${paint(item.ink, item_label(item))}${item_stats_label(item)}.`);
	if (item.is_gear()) { announce_find(player, item); }
	return true;
}

function equip_item(var& player, int slot_index) -> void {
	Item item = player.inventory[slot_index];
	if (item.kind == "potion") { drink_potion(player, slot_index); return; }
	if (item.kind == "scroll") { read_scroll(player, slot_index); return; }
	var swapped = player.worn(item.kind);
	if (item.kind == "weapon") { player.weapon = item; }
	else if (item.kind == "armor") { player.armor = item; }
	else if (item.kind == "trinket") { player.trinket = item; }
	else { log_msg("You can't wear that."); return; }
	player.inventory.remove(slot_index);
	if (swapped != null) { player.inventory.push(swapped); }
	log_msg(`You equip ${paint(item.ink, item_label(item))}${item_stats_label(item)}.`);
	player.hp = imin(player.hp, player.total_max_hp());
	player.focus = imin(player.focus, player.total_max_focus());
}

function drop_item(var& player, int slot_index) -> void {
	Item item = player.inventory[slot_index];
	player.inventory.remove(slot_index);
	item.x = player.x;
	item.y = player.y;
	G.ground.push(item);
	log_msg(`You drop ${item_label(item)}.`);
}

// raises stat toward cap through the reference; the amount it actually rose
function restore_stat(int& stat, int cap, int amount) -> int {
	int before = stat;
	stat = imin(cap, stat + amount);
	return stat - before;
}

function drink_potion(var& player, int slot_index) -> void {
	Item item = player.inventory[slot_index];
	player.inventory.remove(slot_index);
	bool first_taste = !G.known_potions.has(item.effect);
	G.known_potions[item.effect] = true;
	Consumable potion = POTIONS[item.effect];
	log_msg(first_taste ? `You quaff it... it was a ${potion.name}!` : `You quaff the ${potion.name}.`);
	switch (item.effect) {
		case "mend": {
			int healed = restore_stat(player.hp, player.total_max_hp(), potion.power);
			log_ink(INK_GOOD, `You feel knitted together (+${healed} HP).`);
			fx_screen(`+${healed}`, INK_GOOD);
			break;
		}
		case "greatmend": {
			int healed = restore_stat(player.hp, player.total_max_hp(), potion.power);
			log_ink(INK_GOOD, `Warmth floods you (+${healed} HP).`);
			fx_screen(`+${healed}`, INK_GOOD);
			break;
		}
		case "focus": {
			int gained = restore_stat(player.focus, player.total_max_focus(), potion.power);
			log_ink(INK_INFO, `Your thoughts sharpen (+${gained} focus).`);
			break;
		}
		case "rage": {
			add_effect(player, "rage", 20, potion.power);
			log_ink(INK_BAD, "Your teeth feel three sizes too big. (+atk)");
			break;
		}
		case "stoneskin": {
			add_effect(player, "stoneskin", 20, potion.power);
			log_ink(INK_DIM, "Your hide sets like mortar. (+def)");
			break;
		}
		case "regen": {
			add_effect(player, "regen", potion.power, 2);
			log_ink(INK_GOOD, "Your scratches start closing on their own.");
			break;
		}
	}
}

function read_scroll(var& player, int slot_index) -> void {
	Item item = player.inventory[slot_index];
	player.inventory.remove(slot_index);
	bool first_read = !G.known_scrolls.has(item.effect);
	G.known_scrolls[item.effect] = true;
	Consumable scroll = SCROLLS[item.effect];
	log_msg(first_read ? `You read it aloud... a ${scroll.name}!` : `You read the ${scroll.name}.`);
	switch (item.effect) {
		case "reveal": {
			for (int i = 0; i < MAP_W * MAP_H; ++i) { G.seen[i] = true; }
			log_ink(INK_INFO, "Ink spiders across your mind: the whole floor, mapped.");
			break;
		}
		case "teleport": {
			teleport_player(player);
			break;
		}
		case "enfeeble": {
			int count = 0;
			for (auto& m : G.monsters) {
				if (m.alive && is_visible(m.x, m.y)) {
					add_effect(m, "wither", 12, scroll.power);
					count += 1;
				}
			}
			log_ink(INK_MAGIC, `A groan rolls through ${count} nearby horrors.`);
			break;
		}
		case "lore": {
			for (auto k : POTIONS.keys()) { G.known_potions[k] = true; }
			for (auto k : SCROLLS.keys()) { G.known_scrolls[k] = true; }
			log_ink(INK_INFO, "Labels everywhere! You suddenly know all bottles and scrolls.");
			break;
		}
	}
}

function teleport_player(var& player) -> void {
	for (int attempt = 0; attempt < 200; ++attempt) {
		int tx = RNG.next(MAP_W);
		int ty = RNG.next(MAP_H);
		if (!is_walkable(tx, ty) || monster_index_at(tx, ty) >= 0) { continue; }
		player.x = tx;
		player.y = ty;
		log_ink(INK_MAGIC, "The world hiccups. You are somewhere else.");
		return;
	}
	log_msg("The scroll fizzles; elsewhere was full.");
}

// The first carried piece of gear that fills an empty slot or beats the worn one.
function smart_equip(var& player) -> bool {
	for (int i = 0; i < player.inventory.size(); ++i) {
		if (!player.inventory[i].is_gear()) { continue; }
		var current = player.worn(player.inventory[i].kind);
		int old_score = current == null ? -1 : gear_score(current);
		if (gear_score(player.inventory[i]) > old_score) {
			equip_item(player, i);
			return true;
		}
	}
	log_msg("Nothing in the pack beats what you're wearing.");
	return false;
}

// A find is weighed against what you wear: how much better or worse, and the better ones get a
// banner in the view, louder by rarity.
function announce_find(var& player, Item item) -> void {
	var worn = player.worn(item.kind);
	if (worn == null) {
		log_ink(INK_GOOD, `  Your ${item.kind} slot is empty: E puts it on.`);
	} else {
		int delta = gear_score(item) - gear_score(worn);
		if (delta > 0) { log_ink(INK_GOOD, `  Better than your ${worn.name} (+${delta}): E equips it.`); }
		else if (delta < 0) { log_ink(INK_DIM, `  Worse than your ${worn.name} (${delta}).`); }
		else { log_ink(INK_DIM, `  About as good as your ${worn.name}.`); }
	}
	if (item.rarity == "common" || item.rarity == "uncommon") { return; }
	fx_banner(string::to_upper(item.rarity), string::to_upper(item.name), rarity_ink(item.rarity));
}

function gear_score(Item item) -> int {
	int score = item.atk * 2 + item.def * 2;
	for (auto kv : item.bonus) { score += kv.second; }
	return score;
}
