// The creatures and things of the Well: Entity and its Monster and Player, Item, and the timed
// effects both kinds of entity carry. Objects copy on assignment, so every function that changes
// one takes it by var&, and each lives in exactly one place (G.player, G.monsters[i]).

class Effect { string kind; int turns; int power; }

class Entity {
	int id = 0;
	string name = "";
	string glyph = "?";
	int ink = 0xC8C8C8;
	int x = 0;
	int y = 0;
	int hp = 1;
	int max_hp = 1;
	int atk = 1;
	int def = 0;
	int eva = 0;       // dodge %, before traits
	int crit = 5;      // crit %, before traits
	bool alive = true;
	array<Effect> effects;

	function has_effect(string kind) -> bool {
		for (auto fx : effects) {
			if (fx.kind == kind) { return true; }
		}
		return false;
	}
}

class Monster : Entity {
	string kind = "";
	string behavior = "chase";
	int tier = 1;
	int range = 1;          // reach of a ranged attack
	bool aggro = false;
	bool disguised = false; // a mimic until it bites
	int move_clock = 0;     // sluggish things move every other turn
	bool withers = false;
	bool burns = false;

	Monster() : super() {}

	Monster(string monster_kind) : super() {
		kind = monster_kind;
		Breed breed = BESTIARY[monster_kind];
		name = breed.name;
		glyph = breed.glyph;
		ink = breed.ink;
		max_hp = breed.hp;
		hp = max_hp;
		atk = breed.atk;
		def = breed.def;
		eva = breed.eva;
		behavior = breed.behavior;
		tier = breed.tier;
		range = breed.range;
		withers = breed.withers;
		burns = breed.burns;
		disguised = behavior == "mimic";
	}

	function xp_reward() -> int { return tier * 5 + 2; }
	function gold_reward(var& rng) -> int { return rng.roll(0, tier * 4); }
}

class Item {
	string name = "";
	string kind = "weapon";   // weapon | armor | trinket | potion | scroll | gold
	string glyph = ")";
	int ink = 0xC8C8C8;
	string rarity = "common";
	int atk = 0;
	int def = 0;
	map<string, int> bonus;   // crit, eva, hp, focus, burn (a % chance to set the target alight)
	string effect = "";       // a potion's or scroll's id
	int amount = 0;           // a gold pile's coins
	int x = 0;
	int y = 0;

	function bonus_of(string key) -> int { return bonus.has(key) ? bonus[key] : 0; }
	function is_gear() -> bool { return kind == "weapon" || kind == "armor" || kind == "trinket"; }
}

class Player : Entity {
	string species_id = "";
	string profession_id = "";
	string trait = "";
	string skill = "";
	string skill_name = "";
	int skill_cost = 0;
	int level = 1;
	int xp = 0;
	int gold = 0;
	int focus = 8;
	int max_focus = 8;
	int kills = 0;
	int fov_radius = 8;
	bool god = false;
	bool stubborn_spent = false;  // the Brockin trait, refreshed each floor
	var weapon = null;            // an Item, or null for an empty slot
	var armor = null;
	var trinket = null;
	array<Item> inventory;

	Player() : super() {}

	Player(Species species, Profession calling) : super() {
		name = "Wanderer";
		glyph = "@";
		ink = INK_TITLE;
		species_id = species.id;
		profession_id = calling.id;
		trait = species.trait;
		skill = calling.skill;
		skill_name = calling.skill_name;
		skill_cost = calling.skill_cost;
		max_hp = 20 + species.bonus.hp + calling.bonus.hp;
		hp = max_hp;
		atk = 3 + species.bonus.atk + calling.bonus.atk;
		def = species.bonus.def + calling.bonus.def;
		eva = 8 + species.bonus.eva + calling.bonus.eva;
		crit = 5 + species.bonus.crit + calling.bonus.crit;
		max_focus = 8 + species.bonus.focus + calling.bonus.focus;
		focus = max_focus;
	}

	function equip_bonus(string key) -> int {
		int total = 0;
		if (weapon != null) { total += weapon.bonus_of(key); }
		if (armor != null) { total += armor.bonus_of(key); }
		if (trinket != null) { total += trinket.bonus_of(key); }
		return total;
	}

	function total_atk() -> int { return atk + equip_bonus("atk") + (weapon != null ? weapon.atk : 0); }
	function total_def() -> int { return def + equip_bonus("def") + (armor != null ? armor.def : 0); }

	function total_eva() -> int {
		int total = eva + equip_bonus("eva");
		if (trait == "slipjaw" && hp * 4 < total_max_hp()) { total += 20; }
		return total;
	}

	function total_crit() -> int { return crit + equip_bonus("crit"); }
	function total_max_hp() -> int { return max_hp + equip_bonus("hp"); }
	function total_max_focus() -> int { return max_focus + equip_bonus("focus"); }
	function xp_next() -> int { return 8 + level * 7; }

	// the slot an item of this kind goes in, or null
	function worn(string kind) -> var {
		if (kind == "weapon") { return weapon; }
		if (kind == "armor") { return armor; }
		if (kind == "trinket") { return trinket; }
		return null;
	}
}

// ----------------------------------------------------------------- effects ---
// A stat effect applies once when added and reverts when it expires; the change lives in the
// entity's own fields, so a saved run reloads exactly.
map<string, string> EFFECT_FADE = {
	"burn": "The flames on you gutter out.",
	"regen": "The knitting warmth ebbs.",
	"rage": "Your rage cools.",
	"stoneskin": "Your stoneskin crumbles away.",
	"blessed": "JAI's blessing lifts.",
	"curl": "You uncurl.",
	"wither": "The hex unravels."
};

// the stat an effect raises, or ""
function effect_stat(string kind) -> string {
	switch (kind) {
		case "rage": return "atk";
		case "stoneskin": return "def";
		case "curl": return "def";
		case "blessed": return "eva";
	}
	return "";
}

function apply_stat(var& target, string stat, int delta) -> void {
	switch (stat) {
		case "atk": target.atk = imax(0, target.atk + delta); break;
		case "def": target.def = imax(0, target.def + delta); break;
		case "eva": target.eva = target.eva + delta; break;
	}
}

function add_effect(var& target, string kind, int turns, int power) -> void {
	for (auto& fx : target.effects) {
		if (fx.kind == kind) {
			fx.turns = imax(fx.turns, turns);   // refresh, never stack
			return;
		}
	}
	apply_stat(target, effect_stat(kind), power);
	if (kind == "wither") {
		apply_stat(target, "atk", -power);
		apply_stat(target, "def", -power);
	}
	target.effects.push(Effect{kind, turns, power});
}

function expire_effect(var& target, string kind, int power) -> void {
	apply_stat(target, effect_stat(kind), -power);
	if (kind == "wither") {
		apply_stat(target, "atk", power);
		apply_stat(target, "def", power);
	}
}

function tick_effects(var& target) -> void {
	bool is_player = target.glyph == "@";
	array<Effect> kept;
	for (int i = 0; i < target.effects.size(); ++i) {
		string kind = target.effects[i].kind;
		int power = target.effects[i].power;
		if (kind == "burn") {
			int dmg = power;
			if (is_player && target.trait == "scaleblood") { dmg = imax(1, dmg / 2); }
			int left = target.effects[i].turns - 1;
			string left_note = left == 1 ? "1 turn left" : (left > 1 ? `${left} turns left` : "fading");
			if (is_player) {
				log_ink(INK_BAD, `Burning: -${dmg} HP (HP ${player_hp_after(dmg)}/${G.player.total_max_hp()}), ${left_note}.`);
			} else if (is_visible(target.x, target.y)) {
				log_ink(INK_BAD, `${mon_subject(target)} burns for ${dmg}${health_note(imax(0, target.hp - dmg), target.max_hp)}.`);
			}
			hurt_entity(target, dmg, "burning");
		} else if (kind == "regen") {
			if (is_player) {
				int healed = restore_stat(target.hp, target.total_max_hp(), power);
				if (healed > 0) { log_ink(INK_GOOD, `Regen knits you: +${healed} HP (HP ${target.hp}/${target.total_max_hp()}).`); }
			} else {
				heal_entity(target, power);
			}
		}
		if (!target.alive) { return; }
		target.effects[i].turns -= 1;
		if (target.effects[i].turns > 0) {
			kept.push(target.effects[i]);
			continue;
		}
		expire_effect(target, kind, power);
		if (is_player) { log_ink(INK_DIM, EFFECT_FADE.has(kind) ? EFFECT_FADE[kind] : `${kind} fades.`); }
	}
	target.effects = kept;
}
