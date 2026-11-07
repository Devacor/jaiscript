// Gator Quest - definition records and the tiny line DSL the content tables
// are written in: "Name|kind|key=value|key=value|d=flavor text". Parsing is
// pure string work, so the tables hot-reload like any other script.

import "gq_util.jai";

class TileDef {
	string name = "";
	int walk = 0;          // 0 wall, 1 land, 2 water, 3 gap, 4 ice, 5 lava, 6 slow land
	bool opaque = false;
	int light = 0;
	int res = 0;           // 0 none, 1 tree, 2 rock, 3 ore, 4 water(fish), 5 crystal
	int station = 0;       // 0 none, 1 forge, 2 anvil, 3 fire, 4 alchemy, 5 bank, 6 shrine, 7 gate, 8 stairs, 9 chest
	int after = -1;        // tile it turns into while depleted
	bool hide = false;     // stealth bonus standing in it
}

class ItemDef {
	string name = "";
	int kind = 0;
	int slot = -1;
	int lo = 0;
	int hi = 0;
	int armor = 0;
	int speed = 8;
	int skill = 3;
	int range = 1;
	int value = 1;
	int tier = 1;
	int eff = 0;
	int pow = 0;
	int pow2 = 0;
	bool stack = false;
	int rarity = 0;
	string spr = "";
	string col = "";
	string desc = "";
	array<int> aff_code;
	array<int> aff_val;
}

class AffixDef {
	string name = "";
	bool prefix = true;
	int applies = 0;       // 0 any, 1 weapon, 2 armor/jewelry
	int code = 0;
	int lo = 1;
	int hi = 1;
	int tier = 1;
}

class MonDef {
	string name = "";
	int hp = 10;
	int lo = 1;
	int hi = 3;
	int armor = 0;
	int speed = 3;
	int aspeed = 8;
	int skill = 300;
	int ai = 2;
	int fac = 2;
	int range = 1;
	int spell = -1;
	int move = 1;          // 0 static, 1 land, 2 water, 3 amphibious, 4 flying
	int tier = 1;
	int gold_lo = 0;
	int gold_hi = 0;
	int fame = 1;
	int light = 0;
	int summon = -1;
	int split = -1;
	bool undead = false;
	bool fireproof = false;
	bool frostproof = false;
	bool poisonous = false;
	bool boss = false;
	int regen = 0;
	int leech = 0;
	array<int> drop_item;
	array<int> drop_chance;
	string spr = "";
	string col = "";
	string snd = "";
	string quip = "";
	string desc = "";
}

class SpellDef {
	string name = "";
	int circle = 1;
	int mana = 4;
	array<int> reg;
	int target = 0;        // 0 self/ally, 1 bolt at target, 2 area around target, 3 area around self, 4 summon, 5 blink, 6 recall, 7 resurrect, 8 buff
	int eff = 0;
	int lo = 0;
	int hi = 0;
	int range = 7;
	int radius = 0;
	int fx = 0;
	string snd = "";
	string desc = "";
}

class RecipeDef {
	int out = -1;
	int out_qty = 1;
	int station = 0;       // 0 anywhere (needs tool), else TileDef.station
	int skill = 0;
	int min = 0;           // skill tenths to attempt
	int tool = -1;
	array<int> in_item;
	array<int> in_qty;
}

class QuestDef {
	string name = "";
	int giver = 0;         // npc role
	int type = 0;          // 0 kill kind, 1 bring item, 2 slay boss, 3 visit worlds, 4 craft, 5 catch fish
	int target = -1;
	int count = 1;
	int gold = 0;
	int reward = -1;
	int reward_qty = 1;
	int needs = -1;        // quest that must be done first
	string offer = "";
	string done = "";
}

class WorldDef {
	string name = "";
	int w = 64;
	int h = 48;
	int gen = 0;
	int tier = 1;
	int ambient = 0;       // 0 day/night cycle, else fixed light level
	int music = 0;
	int boss = -1;
	string weather = "";   // letters: r rain, s snow, t storm, f fog, a ash, c clear
	string banner = "";
	string col = "";
}

class NpcRole {
	string name = "";
	string title = "";
	int shop = -1;
	bool heals = false;
	bool banks = false;
	string lines = "";
}

// ---- DSL helpers
string dsl_get(array<string>& parts, string key, string def) {
	string k = key + "=";
	for (int i = 1; i < parts.size(); ++i) {
		if (parts[i].starts_with(k)) { return parts[i].substr(k.size(), parts[i].size() - k.size()); }
	}
	return def;
}

int dsl_int(array<string>& parts, string key, int def) {
	string v = dsl_get(parts, key, "");
	if (v == "") { return def; }
	return v.to_int(def);
}

bool dsl_has(array<string>& parts, string flag) {
	for (int i = 1; i < parts.size(); ++i) {
		if (parts[i] == flag) { return true; }
	}
	return false;
}

// "3-9" -> lo, hi into the out pair
array<int> dsl_range(array<string>& parts, string key, int lo, int hi) {
	string v = dsl_get(parts, key, "");
	if (v == "") { return [lo, hi]; }
	array<string> ab = v.split("-");
	if (ab.size() < 2) { int n = v.to_int(lo); return [n, n]; }
	return [ab[0].to_int(lo), ab[1].to_int(hi)];
}

int index_of_name(array<string> names, string n, int def) {
	for (int i = 0; i < names.size(); ++i) {
		if (names[i] == n) { return i; }
	}
	return def;
}
