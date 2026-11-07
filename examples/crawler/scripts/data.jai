// Content: the Kin species, the professions, the bestiary, gear, affixes, potions, scrolls and
// flavour text. Constant data, so a hot reload re-runs this file freely.

// stat bonuses, in this order everywhere: hp, atk, def, eva, crit, focus
class Stats { int hp; int atk; int def; int eva; int crit; int focus; }

// The folk of the Well are anthro, digitigrade "Kin".
class Species { string id; string name; string blurb; Stats bonus; string trait; string trait_name; string trait_blurb; }

array<Species> SPECIES = [
	Species{"jackodile", "Jackodile-kin", "Jackal grin, crocodile patience. JAI's own folk.",
		Stats{0, 2, 0, 2, 0, 0}, "slipjaw", "Slip-Jaw",
		"+20 evasion while below a quarter health. Clever escapes are a birthright."},
	Species{"drake", "Drask-kin", "Wingless dragonfolk. They insist the wings are 'in the mail'.",
		Stats{6, 0, 0, 0, 0, 0}, "scaleblood", "Scaleblood",
		"Burning ticks deal half damage to you. Fire is practically a cousin."},
	Species{"vulpen", "Vulpen", "Foxfolk. Three plans deep before you finish talking.",
		Stats{0, 0, 0, 3, 8, 0}, "keeneye", "Keen Eye",
		"+8% critical chance. The soft spot practically glows."},
	Species{"brockin", "Brockin", "Badgerfolk. Immovable opinions, immovable everything.",
		Stats{3, 0, 2, -1, 0, 0}, "stubborn", "Too Stubborn To Die",
		"Once per floor, a killing blow leaves you at 1 HP instead."},
	Species{"pangolix", "Pangolix", "Pangolinfolk. Walking argument for bringing a can opener.",
		Stats{2, 0, 1, 0, 0, 2}, "curl", "Curl Up",
		"Dropping below a third health hardens your scales: +3 defense for 5 turns."}
];

class Profession { string id; string name; string blurb; Stats bonus; string skill; string skill_name; int skill_cost; string skill_blurb; }

array<Profession> PROFESSIONS = [
	Profession{"warden", "Warden", "Shield-shaped opinions about who gets hurt today. Melee bruiser.",
		Stats{8, 1, 1, 0, 0, 6}, "bulwark", "Bulwark Slam", 5,
		"Slam the foe in front of you for 250% damage."},
	Profession{"emberwright", "Emberwright", "Maker-caste fire-shaper; Devacor's distant apprentice line. Caster.",
		Stats{0, 0, 0, 0, 0, 12}, "emberbolt", "Emberbolt", 4,
		"Hurl fire down your facing (range 6) and set the first foe burning."},
	Profession{"thornscout", "Thornscout", "Hedge-runner with a sling of fired thorns. Skirmisher.",
		Stats{2, 0, 0, 8, 4, 8}, "thornvolley", "Thorn Volley", 3,
		"Skewer the first foe down your facing (range 5). Cheap, mean, repeatable."},
	Profession{"hexweaver", "Hexweaver", "Knits little curses out of spite and spare string. Debuffer.",
		Stats{0, 0, 0, 2, 0, 14}, "wither", "Wither Hex", 5,
		"Hex the first foe down your facing (range 6): -2 attack and defense, and you sip 3 HP."}
];

function species_of(string id) -> Species {
	for (auto s : SPECIES) { if (s.id == id) { return s; } }
	throw `unknown species '${id}'`;
}

function profession_of(string id) -> Profession {
	for (auto p : PROFESSIONS) { if (p.id == id) { return p; } }
	throw `unknown profession '${id}'`;
}

// ---------------------------------------------------------------- bestiary ---
// behavior: chase | ranged | pack | erratic | sluggish | mimic | boss. tier feeds xp and gold;
// a breed spawns on floors min_floor to max_floor; range is the reach of a ranged attack.
class Breed {
	string name; string glyph; int ink; int min_floor; int max_floor;
	int hp; int atk; int def; int eva; int range; string behavior; int tier; bool withers; bool burns;
	string flavor;
}

map<string, Breed> BESTIARY = {
	"grublin": Breed{"Grublin", "g", 0x28AF5A, 1, 3, 6, 3, 0, 5, 1, "chase", 1, false, false,
		"A goblin that never washed. It brandishes something regrettable."},
	"slinger": Breed{"Grublin Slinger", "s", 0x6EF082, 1, 4, 5, 3, 0, 8, 5, "ranged", 1, false, false,
		"Throws pebbles, insults, and the occasional cousin."},
	"gloop": Breed{"Gloop", "o", 0x32AABE, 1, 4, 12, 2, 1, 0, 1, "sluggish", 1, false, false,
		"A slime with commitment issues. It jiggles ominously."},
	"snarlbat": Breed{"Snarlbat", "b", 0xAF50BE, 1, 4, 4, 3, 0, 18, 1, "erratic", 1, false, false,
		"All wing, no plan."},
	"scrappit": Breed{"Scrappit", "k", 0xC8A03C, 2, 6, 7, 4, 1, 8, 1, "pack", 2, false, false,
		"Kobold-adjacent. Braver in triplicate."},
	"rattlekin": Breed{"Rattlekin", "r", 0xC8C8C8, 3, 6, 10, 5, 1, 5, 1, "chase", 2, false, false,
		"A skeleton held together by grudges and good posture."},
	"grinnest": Breed{"Grinnest", "?", 0xC8A03C, 3, 8, 14, 6, 2, 0, 1, "mimic", 2, false, false,
		"It looked like loot. It is mostly teeth arranged like a welcome mat."},
	"orglis": Breed{"Orglis", "O", 0xCD3C32, 4, 8, 18, 7, 2, 3, 1, "chase", 3, false, false,
		"An orc by any other spelling would smell as bad."},
	"hexmoth": Breed{"Hexmoth", "m", 0xFF82E6, 5, 8, 9, 5, 0, 15, 6, "ranged", 3, true, false,
		"Its wing-dust spells 'doom' in cursive."},
	"marrowmaw": Breed{"Marrowmaw", "W", 0xFF5A50, 6, 9, 26, 9, 3, 2, 1, "chase", 4, false, false,
		"A wolf-shaped appetite. The bones go in; nothing comes out."},
	"cinderwisp": Breed{"Cinderwisp", "w", 0xFFE65A, 7, 9, 10, 6, 0, 20, 5, "ranged", 4, false, true,
		"A candle flame that ran away to commit arson."},
	"drakelet": Breed{"Drakelet", "d", 0xCD3C32, 8, 10, 20, 9, 3, 8, 1, "chase", 4, false, true,
		"Vexadrach's brood. The fire is gray and wrong and still very hot."},
	"vexadrach": Breed{"Vexadrach the Hollow-Flame", "V", 0x828287, 10, 10, 90, 11, 4, 8, 4, "boss", 8, false, true,
		"The wyrm who swallowed Devacor's rainbow fire and choked all the color out of it."}
};

// the breeds a floor spawns and their weights, in the bestiary's key order
class SpawnTable { array<string> kinds; array<int> weights; }

function spawn_table(int floor) -> SpawnTable {
	SpawnTable table;
	for (auto kv : BESTIARY) {
		Breed breed = kv.second;
		if (breed.behavior == "boss" || floor < breed.min_floor || floor > breed.max_floor) { continue; }
		int weight = 10;
		if (breed.behavior == "mimic") { weight = 3; }
		if (breed.tier >= 3 && floor < 7) { weight = 6; }
		table.kinds.push(kv.first);
		table.weights.push(weight);
	}
	return table;
}

// -------------------------------------------------------------------- gear ---
// A base is a weapon (atk), an armor (def) or a trinket (one bonus stat).
class GearBase { string name; int atk; int def; string bonus; int power; }

array<GearBase> WEAPON_BASES = [GearBase{"Shiv", 1}, GearBase{"Fang Blade", 2}, GearBase{"Thorn Pike", 3},
	GearBase{"Ember Brand", 4}, GearBase{"Wyrmtooth Saber", 5}];

array<GearBase> ARMOR_BASES = [GearBase{"Patch Vest", 0, 1}, GearBase{"Scale Jerkin", 0, 2},
	GearBase{"Brockhide Coat", 0, 3}, GearBase{"Warden Plate", 0, 4}];

array<GearBase> TRINKET_BASES = [GearBase{"Tooth Charm", 0, 0, "crit", 6}, GearBase{"Slip Anklet", 0, 0, "eva", 6},
	GearBase{"Marrow Locket", 0, 0, "hp", 6}, GearBase{"Wick Ring", 0, 0, "focus", 5}];

// affixes: the count a rarity rolls, and the bump to the base stat
class Rarity { int weight; string id; int affixes; int bump; int ink; }

array<Rarity> RARITIES = [
	Rarity{50, "common", 0, 0, 0xC8C3B4},
	Rarity{30, "uncommon", 1, 0, 0x6EE07A},
	Rarity{14, "rare", 2, 1, 0x7C92FF},
	Rarity{5, "epic", 2, 2, 0xC46CF0},
	Rarity{1, "legendary", 2, 4, 0xFFD45E}
];

function rarity_ink(string id) -> int {
	for (auto r : RARITIES) { if (r.id == id) { return r.ink; } }
	return INK_TEXT;
}

class Affix { string name; int atk; int def; string bonus; int power; }

array<Affix> PREFIXES = [Affix{"Grubby", 0, 0}, Affix{"Polished", 1, 0}, Affix{"Jagged", 1, 0},
	Affix{"Stalwart", 0, 1}, Affix{"Vicious", 2, 0}, Affix{"Bulwark", 0, 2}];

array<Affix> SUFFIXES = [Affix{"of Nipping", 0, 0, "atk", 1}, Affix{"of Warding", 0, 0, "def", 1},
	Affix{"of the Ember", 0, 0, "burn", 10}, Affix{"of Slipping", 0, 0, "eva", 4},
	Affix{"of Vigor", 0, 0, "hp", 4}, Affix{"of the Keen", 0, 0, "crit", 5}];

map<string, string> LEGENDARY_NAMES = {
	"weapon": "Jai's Sworn Tooth",
	"armor": "Devacor's Emberheart Scale",
	"trinket": "The Undimmed Prism"
};

// ---------------------------------------------------- potions and scrolls ---
// Identify by use: each run shuffles the looks onto the ids.
class Consumable { string name; int power; }

map<string, Consumable> POTIONS = {
	"mend": Consumable{"Potion of Mending", 10},
	"greatmend": Consumable{"Potion of Great Mending", 24},
	"focus": Consumable{"Potion of Clear Focus", 10},
	"rage": Consumable{"Potion of Rattling Rage", 3},
	"stoneskin": Consumable{"Potion of Stoneskin", 3},
	"regen": Consumable{"Potion of Knitting Flesh", 8}
};

array<string> POTION_LOOKS = ["murky red", "fizzing gold", "gloopy green", "smoky violet", "pearlescent", "suspiciously chunky"];

map<string, Consumable> SCROLLS = {
	"reveal": Consumable{"Scroll of the Cartographer", 0},
	"teleport": Consumable{"Scroll of Elsewhere", 0},
	"enfeeble": Consumable{"Scroll of Group Discouragement", 4},
	"lore": Consumable{"Scroll of Lore", 0}
};

array<string> SCROLL_LOOKS = ["VEX MORT", "ZELGO MER", "HACKEM MUCHE", "GNIK SISI"];

int INK_POTION = 0xFF82E6;
int INK_SCROLL = 0x8CA0FF;
int INK_COIN = 0xE6B43C;

// ------------------------------------------------------------------ flavour ---
array<string> STATUE_JAI = [
	"A basalt statue of JAI the jackodile: half grin, half undertow.",
	"The plaque reads: 'Swift in, swifter out. Debt is for the slow.'",
	"JAI's stone tail curls around the pedestal like a drawn bracket."
];

array<string> MURAL_DEVACOR = [
	"A mural of DEVACOR, the wingless dragon, breathing a ribbon of seven colors.",
	"Paint-flakes glitter: DEVACOR forging the first lantern out of a sunset.",
	"Someone has chalked under the mural: 'the fire is a gift, not a hoard.'"
];

array<string> DEATH_QUIPS = [
	"The dungeon files you under 'snacks, assorted'.",
	"JAI's statue, somewhere above, grins a little wider.",
	"Your last thought is that the floor is surprisingly comfortable.",
	"Devacor's mural sheds one painted tear. Probably condensation."
];
