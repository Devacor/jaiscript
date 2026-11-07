// Gator Quest - the content. Every table is a list of DSL lines parsed at load
// (see gq_defs.jai). Ids are positions; code names the few it needs through
// tid()/iid()/mid()/sid(), which throw on a typo so a bad name fails at load.

import "gq_defs.jai";

array<string> SKILL_NAME = ["Swordsmanship", "Mace Fighting", "Archery", "Wrestling", "Tactics", "Parrying",
	"Magery", "Meditation", "Resisting Spells", "Healing", "Fishing", "Mining", "Lumberjacking",
	"Blacksmithing", "Cooking", "Alchemy", "Stealth", "Swimming"];
array<string> SKILL_KEY = ["swords", "maces", "archery", "wrestling", "tactics", "parrying", "magery", "meditation",
	"resist", "healing", "fishing", "mining", "lumber", "smithing", "cooking", "alchemy", "stealth", "swimming"];
// titles: the highest skill names you
array<string> SKILL_TITLE = ["Swordsgator", "Brawler", "Archer", "Wrestler", "Tactician", "Shieldbearer",
	"Mage", "Mystic", "Warder", "Healer", "Angler", "Miner", "Lumberjack", "Blacksmith", "Chef", "Alchemist",
	"Sneak", "Swimmer"];
array<string> RANK_NAME = ["Neophyte", "Novice", "Apprentice", "Journeyman", "Expert", "Adept", "Master", "Grandmaster"];
array<string> SLOT_NAME = ["head", "neck", "body", "hands", "legs", "feet", "weapon", "offhand", "ring", "trinket"];
array<string> KIND_NAME = ["weapon", "armor", "shield", "potion", "scroll", "food", "reagent", "tool", "resource", "ammo", "trinket", "junk", "gem", "quest", "gold"];
array<string> RARITY_NAME = ["Common", "Uncommon", "Rare", "Epic", "Legendary"];
array<string> EFF_NAME = ["none", "heal", "mana", "cure", "str", "dex", "int", "eyeshine", "fireres", "regen", "learn",
	"recall", "food", "random", "firebreath", "bandage", "bottle", "squeak", "music", "bomb", "spellbook"];
array<string> AF_NAME = ["dmg", "fire", "cold", "poison", "leech", "str", "dex", "int", "speed", "armor", "hp", "mana",
	"luck", "resist", "regen", "thorns", "swim", "crit", "gold", "mregen", "magery"];
array<string> AI_NAME = ["none", "wander", "melee", "pack", "ranged", "caster", "coward", "thief", "ambush", "boss", "guard", "critter", "summon"];
array<string> FAC_NAME = ["player", "npc", "monster", "animal", "guard"];
array<string> STATION_NAME = ["", "forge", "anvil", "fire", "alchemy", "bank", "shrine", "gate", "stairs", "chest"];

// ------------------------------------------------------------------ tiles ---
// walk: 0 wall, 1 land, 2 water, 3 gap, 4 ice, 5 lava, 6 slow land
array<string> TILE_SRC = [
	"void|walk=3",
	"grass|walk=1",
	"tall grass|walk=1|hide",
	"flowers|walk=1",
	"dirt|walk=1",
	"road|walk=1",
	"sand|walk=1",
	"shallows|walk=6|res=4",
	"deep water|walk=2|res=4",
	"swamp water|walk=2|res=4",
	"mud|walk=6",
	"oak|walk=0|opaque|res=1|after=stump",
	"pine|walk=0|opaque|res=1|after=stump",
	"cypress|walk=0|opaque|res=1|after=stump",
	"dead tree|walk=0|res=1|after=stump",
	"bush|walk=6|hide",
	"boulder|walk=0|res=2|after=rubble",
	"ore vein|walk=0|res=3|after=rubble",
	"mountain|walk=0|opaque",
	"cave wall|walk=0|opaque",
	"cave floor|walk=1",
	"wood floor|walk=1",
	"stone floor|walk=1",
	"brick wall|walk=0|opaque",
	"wood wall|walk=0|opaque",
	"door|walk=1",
	"counter|walk=0",
	"anvil|walk=0|st=2",
	"forge|walk=0|st=1|light=4",
	"fire pit|walk=0|st=3|light=5",
	"alchemy table|walk=0|st=4|light=1",
	"bank chest|walk=0|st=5",
	"bed|walk=0",
	"fence|walk=0",
	"bridge|walk=1",
	"lava|walk=5|light=4",
	"obsidian|walk=1",
	"ash|walk=1",
	"ice|walk=4",
	"snow|walk=1",
	"ice wall|walk=0|opaque",
	"cloud|walk=1",
	"sky|walk=3",
	"rainbow|walk=1|light=2",
	"ruin floor|walk=1",
	"ruin wall|walk=0|opaque",
	"coral|walk=0",
	"kelp|walk=6|hide",
	"lily pad|walk=1",
	"reeds|walk=6|hide",
	"grave|walk=0",
	"bog|walk=6",
	"lilygate|walk=1|st=7|light=5",
	"stairs down|walk=1|st=8|light=2",
	"stairs up|walk=1|st=8|light=2",
	"shrine|walk=0|st=6|light=5",
	"crystal|walk=0|res=5|light=3|after=rubble",
	"stump|walk=1",
	"rubble|walk=6",
	"chest|walk=0|st=9",
	"sign|walk=0",
	"sconce|walk=0|opaque|light=5",
	"sewage|walk=2",
	"sewer floor|walk=1",
	"carpet|walk=1",
	"well|walk=0",
	"basalt|walk=0|opaque",
	"pillar|walk=0",
	"gold pile|walk=1|light=1",
	"web|walk=6|hide"
];

array<TileDef> build_tiles() {
	array<TileDef> out;
	array<string> names;
	for (int i = 0; i < TILE_SRC.size(); ++i) {
		array<string> p = TILE_SRC[i].split("|");
		names.push(p[0]);
	}
	for (int i = 0; i < TILE_SRC.size(); ++i) {
		array<string> p = TILE_SRC[i].split("|");
		TileDef t = TileDef();
		t.name = p[0];
		t.walk = dsl_int(p, "walk", 1);
		t.opaque = dsl_has(p, "opaque");
		t.hide = dsl_has(p, "hide");
		t.light = dsl_int(p, "light", 0);
		t.res = dsl_int(p, "res", 0);
		t.station = dsl_int(p, "st", 0);
		string after = dsl_get(p, "after", "");
		t.after = after == "" ? -1 : index_of_name(names, after, -1);
		out.push(t);
	}
	return out;
}

array<TileDef> TILES_DEF = build_tiles();

int tid(string name) {
	for (int i = 0; i < TILES_DEF.size(); ++i) {
		if (TILES_DEF[i].name == name) { return i; }
	}
	throw "unknown tile: " + name;
}

int T_VOID = tid("void");
int T_GRASS = tid("grass");
int T_TALLGRASS = tid("tall grass");
int T_FLOWERS = tid("flowers");
int T_DIRT = tid("dirt");
int T_ROAD = tid("road");
int T_SAND = tid("sand");
int T_SHALLOW = tid("shallows");
int T_DEEP = tid("deep water");
int T_SWAMP = tid("swamp water");
int T_MUD = tid("mud");
int T_OAK = tid("oak");
int T_PINE = tid("pine");
int T_CYPRESS = tid("cypress");
int T_DEADTREE = tid("dead tree");
int T_BUSH = tid("bush");
int T_BOULDER = tid("boulder");
int T_ORE = tid("ore vein");
int T_MOUNTAIN = tid("mountain");
int T_CAVEWALL = tid("cave wall");
int T_CAVEFLOOR = tid("cave floor");
int T_WOODFLOOR = tid("wood floor");
int T_STONEFLOOR = tid("stone floor");
int T_BRICK = tid("brick wall");
int T_WOODWALL = tid("wood wall");
int T_DOOR = tid("door");
int T_COUNTER = tid("counter");
int T_ANVIL = tid("anvil");
int T_FORGE = tid("forge");
int T_FIRE = tid("fire pit");
int T_ALCHEMY = tid("alchemy table");
int T_BANK = tid("bank chest");
int T_BED = tid("bed");
int T_FENCE = tid("fence");
int T_BRIDGE = tid("bridge");
int T_LAVA = tid("lava");
int T_OBSIDIAN = tid("obsidian");
int T_ASH = tid("ash");
int T_ICE = tid("ice");
int T_SNOW = tid("snow");
int T_ICEWALL = tid("ice wall");
int T_CLOUD = tid("cloud");
int T_SKY = tid("sky");
int T_RAINBOW = tid("rainbow");
int T_RUINFLOOR = tid("ruin floor");
int T_RUINWALL = tid("ruin wall");
int T_CORAL = tid("coral");
int T_KELP = tid("kelp");
int T_LILY = tid("lily pad");
int T_REEDS = tid("reeds");
int T_GRAVE = tid("grave");
int T_BOG = tid("bog");
int T_GATE = tid("lilygate");
int T_STAIRS_DN = tid("stairs down");
int T_STAIRS_UP = tid("stairs up");
int T_SHRINE = tid("shrine");
int T_CRYSTAL = tid("crystal");
int T_STUMP = tid("stump");
int T_RUBBLE = tid("rubble");
int T_CHEST = tid("chest");
int T_SIGN = tid("sign");
int T_SCONCE = tid("sconce");
int T_SEWAGE = tid("sewage");
int T_SEWERFLOOR = tid("sewer floor");
int T_CARPET = tid("carpet");
int T_WELL = tid("well");
int T_BASALT = tid("basalt");
int T_PILLAR = tid("pillar");
int T_GOLDPILE = tid("gold pile");
int T_WEB = tid("web");

// flat mirrors for the hot paths (movement, sight, light)
array<int> tile_walk_table() {
	array<int> out;
	for (int i = 0; i < TILES_DEF.size(); ++i) { out.push(TILES_DEF[i].walk); }
	return out;
}
array<int> tile_opaque_table() {
	array<int> out;
	for (int i = 0; i < TILES_DEF.size(); ++i) { out.push(TILES_DEF[i].opaque ? 1 : 0); }
	return out;
}
array<int> tile_light_table() {
	array<int> out;
	for (int i = 0; i < TILES_DEF.size(); ++i) { out.push(TILES_DEF[i].light); }
	return out;
}
array<int> T_WALK = tile_walk_table();
array<int> T_OPAQUE = tile_opaque_table();
array<int> T_LIGHT = tile_light_table();

// ------------------------------------------------------------------ items ---
array<string> ITEM_SRC = [
	// -- weapons: dmg, swing speed in ticks, skill, range
	"Rusty Dagger|weapon|dmg=2-5|spd=5|sk=swords|val=8|tier=1|spr=dagger|col=rust|d=Rust counts as poison, right?",
	"Short Sword|weapon|dmg=3-8|spd=7|sk=swords|val=25|tier=1|spr=sword|col=steel|d=Short, like your temper.",
	"Cutlass|weapon|dmg=5-11|spd=8|sk=swords|val=60|tier=2|spr=sword|col=steel|d=Arr.",
	"Broadsword|weapon|dmg=6-14|spd=9|sk=swords|val=110|tier=2|spr=sword|col=silver|d=Broad in every sense.",
	"Katana|weapon|dmg=7-15|spd=7|sk=swords|val=240|tier=3|spr=sword|col=silver|d=Folded one thousand times. Mostly by accident.",
	"Gator Tooth Blade|weapon|dmg=9-18|spd=8|sk=swords|val=420|tier=4|spr=sword|col=bone|d=Carved from a molar you lost as a hatchling.",
	"Obsidian Greatsword|weapon|dmg=14-26|spd=12|sk=swords|val=900|tier=5|spr=greatsword|col=obsidian|d=Heavy, sharp, and deeply emo.",
	"Club|weapon|dmg=3-7|spd=7|sk=maces|val=12|tier=1|spr=club|col=wood|d=The original user interface.",
	"Frying Pan|weapon|dmg=4-9|spd=8|sk=maces|val=30|tier=1|spr=pan|col=iron|d=BONK. Also cooks eggs.",
	"Mace|weapon|dmg=6-12|spd=9|sk=maces|val=90|tier=2|spr=mace|col=steel|d=Spiky persuasion.",
	"Fish Slapper|weapon|dmg=5-12|spd=6|sk=maces|val=150|tier=3|spr=fish|col=cyan|d=A frozen mackerel on a stick. Humiliating for all involved.",
	"War Hammer|weapon|dmg=10-20|spd=12|sk=maces|val=250|tier=3|spr=hammer|col=steel|d=Stop. Hammer time.",
	"Anchor Flail|weapon|dmg=12-24|spd=12|sk=maces|val=600|tier=5|spr=anchor|col=iron|d=Found at the bottom of Sogg harbor, still attached to a very surprised ship.",
	"Short Bow|weapon|dmg=3-9|spd=8|sk=archery|rng=7|val=40|tier=1|spr=bow|col=wood|d=Needs arrows. Gators forget this constantly.",
	"Long Bow|weapon|dmg=5-13|spd=10|sk=archery|rng=9|val=140|tier=2|spr=bow|col=wood|d=For hitting things you would rather not smell.",
	"Crossbow|weapon|dmg=8-16|spd=12|sk=archery|rng=8|val=300|tier=3|spr=crossbow|col=wood|d=Point the pointy end away from face.",
	"Harpoon Gun|weapon|dmg=10-20|spd=11|sk=archery|rng=7|val=700|tier=5|spr=crossbow|col=steel|d=For whales. And rude eels.",
	"Apprentice Wand|weapon|dmg=4-9|spd=9|sk=magery|rng=6|val=120|tier=2|spr=wand|col=purple|d=Shoots sparkles. Sparkles hurt.",
	"Staff of Sparks|weapon|dmg=6-14|spd=10|sk=magery|rng=7|val=480|tier=4|spr=staff|col=cyan|d=Zappity zap.",
	// -- armor
	"Straw Hat|armor|slot=head|arm=1|val=5|tier=1|spr=hat|col=straw|d=Keeps the sun off your scales.",
	"Leather Cap|armor|slot=head|arm=2|val=15|tier=1|spr=cap|col=leather",
	"Bucket Helm|armor|slot=head|arm=3|val=20|tier=1|spr=helm|col=iron|d=Clang.",
	"Iron Helm|armor|slot=head|arm=5|val=80|tier=2|spr=helm|col=steel",
	"Wizard Hat|armor|slot=head|arm=1|val=120|tier=2|spr=wizhat|col=purple|aff=int:5,mana:10|d=Pointy. Mysterious. A little itchy.",
	"Leather Tunic|armor|slot=body|arm=4|val=30|tier=1|spr=tunic|col=leather",
	"Chainmail Shirt|armor|slot=body|arm=8|val=120|tier=2|spr=chain|col=steel",
	"Gator-Scale Vest|armor|slot=body|arm=10|val=220|tier=3|spr=tunic|col=green|d=Made from a distant cousin. Awkward at reunions.",
	"Plate Cuirass|armor|slot=body|arm=13|val=400|tier=4|spr=plate|col=silver",
	"Robe of the Mire|armor|slot=body|arm=3|val=260|tier=3|spr=robe|col=moss|aff=magery:8,mregen:2",
	"Leather Gloves|armor|slot=hands|arm=1|val=10|tier=1|spr=gloves|col=leather",
	"Chain Gloves|armor|slot=hands|arm=3|val=60|tier=2|spr=gloves|col=steel",
	"Plate Gauntlets|armor|slot=hands|arm=5|val=160|tier=4|spr=gloves|col=silver",
	"Leather Leggings|armor|slot=legs|arm=2|val=20|tier=1|spr=legs|col=leather",
	"Chain Leggings|armor|slot=legs|arm=5|val=90|tier=2|spr=legs|col=steel",
	"Plate Greaves|armor|slot=legs|arm=8|val=240|tier=4|spr=legs|col=silver",
	"Sandals|armor|slot=feet|arm=1|val=6|tier=1|spr=boots|col=straw",
	"Leather Boots|armor|slot=feet|arm=2|val=18|tier=1|spr=boots|col=leather",
	"Swamp Waders|armor|slot=feet|arm=2|val=90|tier=2|spr=boots|col=moss|aff=swim:20|d=Waterproof to the knee. Gators have no knees.",
	"Plate Boots|armor|slot=feet|arm=4|val=150|tier=4|spr=boots|col=silver",
	"Pot Lid|shield|slot=offhand|arm=2|val=8|tier=1|spr=shield|col=iron|d=Still smells like gumbo.",
	"Wooden Shield|shield|slot=offhand|arm=4|val=30|tier=1|spr=shield|col=wood",
	"Kite Shield|shield|slot=offhand|arm=7|val=140|tier=3|spr=shield|col=steel",
	"Turtle Shell Shield|shield|slot=offhand|arm=8|val=200|tier=3|spr=shield|col=moss|d=It is fine. The turtle was done with it.",
	"Tower Shield|shield|slot=offhand|arm=10|val=320|tier=4|spr=shield|col=silver",
	"Copper Amulet|armor|slot=neck|arm=0|val=40|tier=1|spr=amulet|col=copper",
	"Bone Charm|armor|slot=neck|arm=0|val=70|tier=2|spr=amulet|col=bone",
	"Pearl Necklace|armor|slot=neck|arm=0|val=160|tier=3|spr=amulet|col=pearl",
	"Copper Ring|armor|slot=ring|arm=0|val=35|tier=1|spr=ring|col=copper",
	"Silver Ring|armor|slot=ring|arm=0|val=90|tier=2|spr=ring|col=silver",
	"Gold Ring|armor|slot=ring|arm=0|val=200|tier=3|spr=ring|col=gold",
	// -- trinkets
	"Lucky Frog Charm|trinket|slot=trinket|val=60|tier=1|spr=frog|col=green|aff=luck:10|d=Ribbit for luck.",
	"Rubber Duck|trinket|slot=trinket|val=500|tier=5|rar=legendary|eff=squeak|spr=duck|col=yellow|aff=luck:30,swim:30,hp:15|d=The single most powerful artifact in Gatorland. Squeaks.",
	"Kazoo|trinket|slot=trinket|val=25|tier=1|eff=music|spr=kazoo|col=red|d=Bzzzt bzzt bzzzzzt.",
	"Banjo|trinket|slot=trinket|val=180|tier=2|eff=music|spr=banjo|col=wood|aff=dex:3|d=Dueling optional.",
	// -- potions
	"Minor Heal Potion|potion|eff=heal|pow=18|val=15|tier=1|stack|spr=potion|col=red",
	"Heal Potion|potion|eff=heal|pow=40|val=40|tier=2|stack|spr=potion|col=red",
	"Greater Heal Potion|potion|eff=heal|pow=80|val=110|tier=3|stack|spr=potion|col=crimson",
	"Mana Potion|potion|eff=mana|pow=35|val=45|tier=2|stack|spr=potion|col=blue",
	"Cure Potion|potion|eff=cure|val=30|tier=1|stack|spr=potion|col=orange",
	"Strength Potion|potion|eff=str|pow=10|val=60|tier=2|stack|spr=potion|col=white",
	"Eyeshine Potion|potion|eff=eyeshine|pow=1500|val=35|tier=1|stack|spr=potion|col=yellow",
	"Fire Resist Potion|potion|eff=fireres|pow=900|val=70|tier=3|stack|spr=potion|col=lava",
	"Swamp Tonic|potion|eff=regen|pow=300|val=55|tier=2|stack|spr=potion|col=moss|d=Tastes like the bottom of a pond. In a good way.",
	"Swamp Gas Bomb|potion|eff=bomb|pow=25|val=80|tier=3|stack|spr=bomb|col=slime|d=Do not light indoors.",
	// -- scrolls: pow = spell id
	"Scroll of Heal|scroll|eff=learn|pow=0|val=30|tier=1|stack|spr=scroll|col=paper",
	"Scroll of Snap Bolt|scroll|eff=learn|pow=1|val=30|tier=1|stack|spr=scroll|col=paper",
	"Scroll of Eyeshine|scroll|eff=learn|pow=2|val=35|tier=1|stack|spr=scroll|col=paper",
	"Scroll of Cure|scroll|eff=learn|pow=3|val=40|tier=1|stack|spr=scroll|col=paper",
	"Scroll of Frost Bolt|scroll|eff=learn|pow=4|val=90|tier=2|stack|spr=scroll|col=paper",
	"Scroll of Fireball|scroll|eff=learn|pow=5|val=110|tier=2|stack|spr=scroll|col=paper",
	"Scroll of Teleport|scroll|eff=learn|pow=6|val=140|tier=2|stack|spr=scroll|col=paper",
	"Scroll of Poison|scroll|eff=learn|pow=7|val=120|tier=2|stack|spr=scroll|col=paper",
	"Scroll of Recall|scroll|eff=learn|pow=8|val=160|tier=2|stack|spr=scroll|col=paper",
	"Scroll of Lightning|scroll|eff=learn|pow=9|val=240|tier=3|stack|spr=scroll|col=paper",
	"Scroll of Greater Heal|scroll|eff=learn|pow=10|val=260|tier=3|stack|spr=scroll|col=paper",
	"Scroll of Summon Frog|scroll|eff=learn|pow=11|val=300|tier=3|stack|spr=scroll|col=paper",
	"Scroll of Resurrection|scroll|eff=learn|pow=12|val=500|tier=4|stack|spr=scroll|col=paper",
	"Scroll of Flame Geyser|scroll|eff=learn|pow=13|val=600|tier=4|stack|spr=scroll|col=paper",
	"Scroll of Chain Lightning|scroll|eff=learn|pow=14|val=800|tier=5|stack|spr=scroll|col=paper",
	"Scroll of Meteor Swarm|scroll|eff=learn|pow=15|val=1200|tier=5|stack|spr=scroll|col=paper",
	"Homeward Rune|scroll|eff=recall|val=50|tier=1|stack|spr=rune|col=blue|d=Snap it to go home. Works 60% of the time, every time.",
	"Spellbook|tool|eff=spellbook|val=40|tier=1|spr=book|col=purple|d=A slim volume. Your spells live here.",
	// -- food: pow = heal
	"Raw Fish|food|eff=food|pow=3|val=3|tier=1|stack|spr=fish|col=silver",
	"Cooked Fish|food|eff=food|pow=14|val=10|tier=1|stack|spr=fish|col=tan",
	"Catfish|food|eff=food|pow=4|val=6|tier=1|stack|spr=fish|col=brown",
	"Blackened Catfish|food|eff=food|pow=24|val=22|tier=2|stack|spr=fish|col=char",
	"Frog Legs|food|eff=food|pow=3|val=4|tier=1|stack|spr=meat|col=green",
	"Fried Frog Legs|food|eff=food|pow=18|val=16|tier=1|stack|spr=meat|col=tan|d=Tastes like chicken. Everything does.",
	"Crawdad|food|eff=food|pow=3|val=5|tier=1|stack|spr=crawdad|col=red",
	"Crawdad Etouffee|food|eff=food|pow=35|val=40|tier=2|stack|spr=bowl|col=orange|d=Mama's recipe. Do not tell Mama.",
	"Bread|food|eff=food|pow=10|val=5|tier=1|stack|spr=bread|col=tan",
	"Cheese Wheel|food|eff=food|pow=12|val=8|tier=1|stack|spr=cheese|col=yellow",
	"Moon Pie|food|eff=food|pow=25|val=15|tier=1|stack|spr=pie|col=tan|d=Once in a blue moon.",
	"Marshmallow|food|eff=food|pow=2|val=2|tier=1|stack|spr=marsh|col=white",
	"Toasted Marshmallow|food|eff=food|pow=16|val=9|tier=1|stack|spr=marsh|col=tan",
	"Hot Sauce|food|eff=firebreath|pow=20|val=30|tier=2|stack|spr=potion|col=red|d=Breathe fire at whatever is in front of you. Regret it later.",
	"Suspicious Mushroom|food|eff=random|val=12|tier=1|stack|spr=shroom|col=purple|d=What could possibly go wrong?",
	"Ice Fish|food|eff=food|pow=6|val=12|tier=2|stack|spr=fish|col=ice",
	"Cloud Trout|food|eff=food|pow=8|val=18|tier=3|stack|spr=fish|col=white",
	"Ghost Fish|food|eff=food|pow=6|val=20|tier=3|stack|spr=fish|col=ghost|d=You can see its lunch.",
	"Lava Eel|food|eff=food|pow=6|val=20|tier=3|stack|spr=fish|col=lava",
	"Smoked Eel|food|eff=food|pow=45|val=50|tier=3|stack|spr=fish|col=char",
	"Golden Carp|junk|val=750|tier=4|spr=fish|col=gold|d=A legend of the Bayou. Worth a fortune to the Fishmonger.",
	// -- reagents
	"Swamp Pearl|reagent|val=5|tier=1|stack|spr=pearl|col=black",
	"Gatormoss|reagent|val=5|tier=1|stack|spr=herb|col=red",
	"Wild Onion|reagent|val=3|tier=1|stack|spr=bulb|col=white",
	"Cattail Root|reagent|val=3|tier=1|stack|spr=root|col=tan",
	"Gnarlroot|reagent|val=4|tier=1|stack|spr=root|col=brown",
	"Voodoo Lily|reagent|val=4|tier=1|stack|spr=herb|col=purple",
	"Bog Silk|reagent|val=4|tier=1|stack|spr=silk|col=white",
	"Firefly Dust|reagent|val=4|tier=1|stack|spr=ash|col=yellow",
	// -- tools and ammo
	"Fishing Pole|tool|val=12|tier=1|spr=pole|col=wood",
	"Pickaxe|tool|val=20|tier=1|spr=pick|col=iron",
	"Hatchet|tool|val=18|tier=1|spr=axe|col=iron",
	"Smith Hammer|tool|val=25|tier=1|spr=hammer|col=iron",
	"Mortar and Pestle|tool|val=25|tier=1|spr=mortar|col=stone",
	"Bandage|tool|eff=bandage|pow=12|val=2|tier=1|stack|spr=bandage|col=white",
	"Arrow|ammo|val=1|tier=1|stack|spr=arrow|col=wood",
	// -- resources
	"Iron Ore|resource|val=4|tier=1|stack|spr=ore|col=rust",
	"Silver Ore|resource|val=10|tier=2|stack|spr=ore|col=silver",
	"Mithril Ore|resource|val=30|tier=4|stack|spr=ore|col=mithril",
	"Iron Ingot|resource|val=10|tier=1|stack|spr=ingot|col=iron",
	"Silver Ingot|resource|val=25|tier=2|stack|spr=ingot|col=silver",
	"Mithril Ingot|resource|val=70|tier=4|stack|spr=ingot|col=mithril",
	"Log|resource|val=3|tier=1|stack|spr=log|col=wood",
	"Feather|resource|val=2|tier=1|stack|spr=feather|col=white",
	"Hide|resource|val=6|tier=1|stack|spr=hide|col=leather",
	"Bone|resource|val=3|tier=1|stack|spr=bone|col=bone",
	"Magma Core|resource|val=60|tier=5|stack|spr=orb|col=lava",
	"Frost Shard|resource|val=50|tier=4|stack|spr=shard|col=ice",
	"Storm Essence|resource|val=70|tier=5|stack|spr=orb|col=cyan",
	"Ectoplasm|resource|val=40|tier=3|stack|spr=orb|col=ghost",
	"Kelp|resource|val=3|tier=1|stack|spr=herb|col=moss",
	"Cloud Fluff|resource|val=20|tier=4|stack|spr=fluff|col=white",
	"Rat Tail|resource|val=1|tier=1|stack|spr=tail|col=pink",
	// -- gems
	"Amethyst|gem|val=60|tier=2|stack|spr=gem|col=purple",
	"Ruby|gem|val=100|tier=3|stack|spr=gem|col=red",
	"Sapphire|gem|val=100|tier=3|stack|spr=gem|col=blue",
	"Emerald|gem|val=120|tier=3|stack|spr=gem|col=green",
	"Diamond|gem|val=250|tier=4|stack|spr=gem|col=white",
	// -- money
	"Gold Coins|gold|val=1|tier=1|stack|spr=coins|col=gold",
	// -- junk
	"Old Boot|junk|val=1|tier=1|spr=boots|col=leather|d=Somebody lost a boot. Somebody is still looking.",
	"Tin Can|junk|val=1|tier=1|spr=can|col=silver",
	"Soggy Hat|junk|val=1|tier=1|spr=hat|col=moss",
	"Message in a Bottle|junk|eff=bottle|val=5|tier=1|spr=bottle|col=cyan",
	// -- quest relics
	"Mossy Relic Scale|quest|val=0|tier=5|spr=scale|col=moss|d=Taken from Big Mama Snapper. It hums.",
	"Pearl Relic Scale|quest|val=0|tier=5|spr=scale|col=pearl|d=Taken from the Drowned King. It is damp.",
	"Shadow Relic Scale|quest|val=0|tier=5|spr=scale|col=ghost|d=Taken from Count Crocula. It whispers about blood types.",
	"Frost Relic Scale|quest|val=0|tier=5|spr=scale|col=ice|d=Taken from the Glacier Queen. Cold to the touch.",
	"Ember Relic Scale|quest|val=0|tier=5|spr=scale|col=lava|d=Taken from Emberjaw. Do not pocket.",
	"Storm Relic Scale|quest|val=0|tier=5|spr=scale|col=cyan|d=Taken from the Thunder Roc. Crackles.",
	"Rat King's Crown|armor|slot=head|arm=4|val=300|tier=3|rar=legendary|spr=crown|col=gold|aff=luck:20,gold:25|d=Slightly chewed.",
	// -- legendaries (boss drops)
	"Big Mama's Shell|shield|slot=offhand|arm=14|val=1500|tier=5|rar=legendary|spr=shield|col=moss|aff=hp:30,thorns:5",
	"Trident of the Drowned King|weapon|dmg=12-24|spd=9|sk=maces|val=2000|tier=5|rar=legendary|spr=trident|col=pearl|aff=cold:8,mana:15",
	"Crocula's Cape|armor|slot=body|arm=9|val=2200|tier=5|rar=legendary|spr=robe|col=crimson|aff=leech:12,stealth:0",
	"Glacier Queen's Tiara|armor|slot=head|arm=4|val=2400|tier=5|rar=legendary|spr=crown|col=ice|aff=int:12,mregen:4,frost:0",
	"Emberjaw's Fang|weapon|dmg=15-28|spd=8|sk=swords|val=3000|tier=5|rar=legendary|spr=sword|col=lava|aff=fire:12,crit:8",
	"Stormcaller|weapon|dmg=11-22|spd=9|sk=archery|rng=10|val=3200|tier=5|rar=legendary|spr=bow|col=cyan|aff=speed:2,dex:10",
	"Crown of the Swamp|armor|slot=head|arm=8|val=9999|tier=5|rar=legendary|spr=crown|col=gold|aff=str:15,dex:15,int:15,hp:40,luck:25|d=Worn by the Grand Gator. Fits perfectly."
];

array<ItemDef> build_items() {
	array<ItemDef> out;
	for (int i = 0; i < ITEM_SRC.size(); ++i) {
		array<string> p = ITEM_SRC[i].split("|");
		ItemDef d = ItemDef();
		d.name = p[0];
		d.kind = index_of_name(KIND_NAME, p[1], 11);
		d.slot = index_of_name(SLOT_NAME, dsl_get(p, "slot", ""), -1);
		if (d.kind == Kind.weapon) { d.slot = Slot.weapon; }
		array<int> dmg = dsl_range(p, "dmg", 0, 0);
		d.lo = dmg[0];
		d.hi = dmg[1];
		d.armor = dsl_int(p, "arm", 0);
		d.speed = dsl_int(p, "spd", 8);
		d.skill = index_of_name(SKILL_KEY, dsl_get(p, "sk", "wrestling"), 3);
		d.range = dsl_int(p, "rng", 1);
		d.value = dsl_int(p, "val", 1);
		d.tier = dsl_int(p, "tier", 1);
		d.eff = index_of_name(EFF_NAME, dsl_get(p, "eff", "none"), 0);
		d.pow = dsl_int(p, "pow", 0);
		d.stack = dsl_has(p, "stack");
		d.rarity = index_of_name(["common", "uncommon", "rare", "epic", "legendary"], dsl_get(p, "rar", "common"), 0);
		d.spr = dsl_get(p, "spr", "bag");
		d.col = dsl_get(p, "col", "white");
		d.desc = dsl_get(p, "d", "");
		string aff = dsl_get(p, "aff", "");
		if (aff != "") {
			array<string> pairs = aff.split(",");
			for (int k = 0; k < pairs.size(); ++k) {
				array<string> cv = pairs[k].split(":");
				int code = index_of_name(AF_NAME, cv[0], -1);
				if (code >= 0 && cv.size() > 1) {
					d.aff_code.push(code);
					d.aff_val.push(cv[1].to_int(0));
				}
			}
		}
		out.push(d);
	}
	return out;
}

array<ItemDef> ITEMS = build_items();

int iid(string name) {
	for (int i = 0; i < ITEMS.size(); ++i) {
		if (ITEMS[i].name == name) { return i; }
	}
	throw "unknown item: " + name;
}

int I_DAGGER = iid("Rusty Dagger");
int I_SHORTSWORD = iid("Short Sword");
int I_SHORTBOW = iid("Short Bow");
int I_ARROW = iid("Arrow");
int I_BANDAGE = iid("Bandage");
int I_SPELLBOOK = iid("Spellbook");
int I_FISHPOLE = iid("Fishing Pole");
int I_PICKAXE = iid("Pickaxe");
int I_HATCHET = iid("Hatchet");
int I_HAMMER = iid("Smith Hammer");
int I_MORTAR = iid("Mortar and Pestle");
int I_MINORHEAL = iid("Minor Heal Potion");
int I_HEAL = iid("Heal Potion");
int I_MANA = iid("Mana Potion");
int I_CURE = iid("Cure Potion");
int I_SWAMPPEARL = iid("Swamp Pearl");
int I_GATORMOSS = iid("Gatormoss");
int I_ONION = iid("Wild Onion");
int I_CATTAIL = iid("Cattail Root");
int I_GNARLROOT = iid("Gnarlroot");
int I_VOODOOLILY = iid("Voodoo Lily");
int I_SILK = iid("Bog Silk");
int I_FIREFLY = iid("Firefly Dust");
int I_IRONORE = iid("Iron Ore");
int I_SILVERORE = iid("Silver Ore");
int I_MITHRILORE = iid("Mithril Ore");
int I_IRONINGOT = iid("Iron Ingot");
int I_LOG = iid("Log");
int I_FEATHER = iid("Feather");
int I_RAWFISH = iid("Raw Fish");
int I_CATFISH = iid("Catfish");
int I_ICEFISH = iid("Ice Fish");
int I_CLOUDTROUT = iid("Cloud Trout");
int I_GHOSTFISH = iid("Ghost Fish");
int I_LAVAEEL = iid("Lava Eel");
int I_GOLDCARP = iid("Golden Carp");
int I_FROGLEGS = iid("Frog Legs");
int I_CRAWDAD = iid("Crawdad");
int I_OLDBOOT = iid("Old Boot");
int I_TINCAN = iid("Tin Can");
int I_SOGGYHAT = iid("Soggy Hat");
int I_BOTTLE = iid("Message in a Bottle");
int I_RATTAIL = iid("Rat Tail");
int I_LEATHERTUNIC = iid("Leather Tunic");
int I_STRAWHAT = iid("Straw Hat");
int I_SANDALS = iid("Sandals");
int I_POTLID = iid("Pot Lid");
int I_BREAD = iid("Bread");
int I_KELP = iid("Kelp");
int I_RUBBERDUCK = iid("Rubber Duck");
int I_GOLD = iid("Gold Coins");
int I_CROWN_SWAMP = iid("Crown of the Swamp");
array<int> RELICS = [iid("Mossy Relic Scale"), iid("Pearl Relic Scale"), iid("Shadow Relic Scale"),
	iid("Frost Relic Scale"), iid("Ember Relic Scale"), iid("Storm Relic Scale")];
array<int> REAGENTS = [I_SWAMPPEARL, I_GATORMOSS, I_ONION, I_CATTAIL, I_GNARLROOT, I_VOODOOLILY, I_SILK, I_FIREFLY];

// ---------------------------------------------------------------- affixes ---
// name|pre or suf|applies (any, weapon, armor)|code|lo-hi|tier
array<string> AFFIX_SRC = [
	"Sharp|pre|weapon|dmg|10-25|1", "Ancient|pre|weapon|dmg|25-45|3", "Flaming|pre|weapon|fire|3-8|2",
	"Frosty|pre|weapon|cold|3-8|2", "Venomous|pre|weapon|poison|2-6|2", "Vampiric|pre|weapon|leech|5-12|3",
	"Swift|pre|weapon|speed|1-2|2", "Chompy|pre|weapon|crit|4-10|1", "Soggy|pre|any|dmg|-15--5|1",
	"Sturdy|pre|armor|armor|1-3|1", "Reinforced|pre|armor|armor|3-6|3", "Blessed|pre|armor|hp|8-20|2",
	"Lucky|pre|any|luck|5-15|1", "Nimble|pre|armor|dex|2-6|2", "Mighty|pre|any|str|2-6|2",
	"of the Bear|suf|any|str|3-8|1", "of the Fox|suf|any|dex|3-8|1", "of the Owl|suf|any|int|3-8|1",
	"of the Gator|suf|any|hp|10-25|2", "of Slaying|suf|weapon|dmg|15-30|3", "of Warding|suf|armor|resist|5-15|2",
	"of the Magi|suf|any|mana|8-20|2", "of Fortune|suf|any|gold|10-30|1", "of Regeneration|suf|armor|regen|1-3|3",
	"of Thorns|suf|armor|thorns|2-6|2", "of the Bayou|suf|any|swim|10-30|1", "of Sorcery|suf|any|magery|3-10|3",
	"of Clarity|suf|armor|mregen|1-3|3"
];

array<AffixDef> build_affixes() {
	array<AffixDef> out;
	for (int i = 0; i < AFFIX_SRC.size(); ++i) {
		array<string> p = AFFIX_SRC[i].split("|");
		AffixDef a = AffixDef();
		a.name = p[0];
		a.prefix = p[1] == "pre";
		a.applies = p[2] == "weapon" ? 1 : (p[2] == "armor" ? 2 : 0);
		a.code = index_of_name(AF_NAME, p[3], 0);
		string r = p[4];
		int dash = r.find("-", 1);
		a.lo = r.substr(0, dash).to_int(1);
		a.hi = r.substr(dash + 1, r.size() - dash - 1).to_int(1);
		a.tier = p[5].to_int(1);
		out.push(a);
	}
	return out;
}

array<AffixDef> AFFIXES = build_affixes();

// ---------------------------------------------------------------- spells ---
array<string> SPELL_SRC = [
	"Heal|circle=1|mana=4|reg=Wild Onion,Cattail Root|tgt=0|eff=heal|pow=8-14|fx=heal|snd=heal|d=Knit flesh. Or scales.",
	"Snap Bolt|circle=1|mana=4|reg=Firefly Dust|tgt=1|eff=dmg|pow=5-10|fx=magic|snd=zap|d=A bolt of pure spite.",
	"Eyeshine|circle=1|mana=5|reg=Bog Silk,Firefly Dust|tgt=8|eff=eyeshine|pow=1500|fx=magic|snd=chime|d=See in the dark like a proper swamp predator.",
	"Cure|circle=2|mana=6|reg=Wild Onion,Cattail Root|tgt=0|eff=cure|fx=heal|snd=heal|d=Purge poison.",
	"Frost Bolt|circle=2|mana=8|reg=Voodoo Lily,Bog Silk|tgt=1|eff=frost|pow=8-15|fx=frost|snd=zap|d=Damage and slow a foe.",
	"Fireball|circle=3|mana=9|reg=Swamp Pearl|tgt=1|eff=fire|pow=10-18|fx=fire|snd=fire|d=The classic.",
	"Teleport|circle=3|mana=10|reg=Gatormoss,Gnarlroot|tgt=5|eff=blink|pow=6|fx=warp|snd=warp|d=Blink forward up to six tiles.",
	"Poison|circle=3|mana=10|reg=Voodoo Lily|tgt=1|eff=poison|pow=40-60|fx=poison|snd=hiss|d=Slow, green, and personal.",
	"Recall|circle=4|mana=12|reg=Swamp Pearl,Gatormoss,Gnarlroot|tgt=6|eff=recall|fx=warp|snd=warp|d=Return home to your bound shrine.",
	"Lightning|circle=4|mana=14|reg=Gnarlroot,Firefly Dust|tgt=1|eff=dmg|pow=16-26|fx=zap|snd=thunder|d=Smite.",
	"Greater Heal|circle=5|mana=14|reg=Wild Onion,Cattail Root,Gnarlroot,Bog Silk|tgt=0|eff=heal|pow=30-45|fx=heal|snd=heal|d=Mend almost anything.",
	"Summon Frog|circle=5|mana=16|reg=Gatormoss,Gnarlroot,Bog Silk|tgt=4|eff=summon|fx=magic|snd=ribbit|d=Summon a loyal frog familiar. It is very brave.",
	"Resurrection|circle=6|mana=25|reg=Gatormoss,Wild Onion,Cattail Root|tgt=7|eff=res|fx=holy|snd=chime|d=Bring a ghostly friend back to the land of the scaly.",
	"Flame Geyser|circle=6|mana=25|reg=Bog Silk,Firefly Dust|tgt=1|eff=fire|pow=30-45|fx=fire|snd=fire|d=A pillar of fire. Stand back.",
	"Chain Lightning|circle=7|mana=30|reg=Swamp Pearl,Gatormoss,Gnarlroot,Firefly Dust|tgt=2|eff=dmg|pow=18-30|rad=3|fx=zap|snd=thunder|d=Arcs between every foe near your target.",
	"Meteor Swarm|circle=8|mana=40|reg=Gatormoss,Gnarlroot,Bog Silk,Firefly Dust|tgt=2|eff=fire|pow=25-40|rad=4|fx=fire|snd=boom|d=Rocks. From. SPACE."
];

array<string> SPELL_EFF = ["heal", "dmg", "eyeshine", "cure", "frost", "fire", "blink", "poison", "recall", "summon", "res"];
array<string> FX_NAME = ["sparks", "blood", "splash", "fire", "frost", "zap", "heal", "smoke", "poison", "magic", "gold", "loot", "levelup", "warp", "bubbles", "bones", "dust", "holy"];

array<SpellDef> build_spells() {
	array<SpellDef> out;
	for (int i = 0; i < SPELL_SRC.size(); ++i) {
		array<string> p = SPELL_SRC[i].split("|");
		SpellDef s = SpellDef();
		s.name = p[0];
		s.circle = dsl_int(p, "circle", 1);
		s.mana = dsl_int(p, "mana", 4);
		array<string> regs = dsl_get(p, "reg", "").split(",");
		for (int r = 0; r < regs.size(); ++r) {
			if (regs[r] != "") { s.reg.push(iid(regs[r])); }
		}
		s.target = dsl_int(p, "tgt", 0);
		s.eff = index_of_name(SPELL_EFF, dsl_get(p, "eff", "dmg"), 1);
		array<int> pw = dsl_range(p, "pow", 0, 0);
		s.lo = pw[0];
		s.hi = pw[1];
		s.radius = dsl_int(p, "rad", 0);
		s.range = dsl_int(p, "rng", 8);
		s.fx = index_of_name(FX_NAME, dsl_get(p, "fx", "magic"), 9);
		s.snd = dsl_get(p, "snd", "zap");
		s.desc = dsl_get(p, "d", "");
		out.push(s);
	}
	return out;
}

array<SpellDef> SPELLS = build_spells();

int sid(string name) {
	for (int i = 0; i < SPELLS.size(); ++i) {
		if (SPELLS[i].name == name) { return i; }
	}
	throw "unknown spell: " + name;
}

// -------------------------------------------------------------- monsters ---
// ai: none wander melee pack ranged caster coward thief ambush boss guard critter summon
// move: 0 static 1 land 2 water 3 amphibious 4 flying
array<string> MON_SRC = [
	// -- town folk and guards (npc faction, invulnerable)
	"Townsgator|hp=100|ai=wander|fac=npc|move=1|spr=gator_npc|col=olive|quip=Mornin'!",
	"Frogfolk|hp=100|ai=wander|fac=npc|move=3|spr=frogman|col=green|quip=Ribbit. I mean, hello.",
	"Old Turtle|hp=100|ai=wander|fac=npc|move=1|spr=turtle_npc|col=moss|quip=Back in my day the swamp was wetter.",
	"Swampton Guard|hp=999|dmg=40-60|ai=guard|fac=guard|move=3|spd=2|aspd=6|skill=1000|spr=guard|col=steel|quip=Halt, scum!",
	// -- critters (animal faction: harmless, flee)
	"Frog|hp=6|dmg=1-1|ai=critter|fac=animal|move=3|spd=4|tier=1|drop=Frog Legs:800|spr=frog|col=green|snd=ribbit",
	"Crawdad|hp=6|dmg=1-2|ai=critter|fac=animal|move=3|spd=5|tier=1|drop=Crawdad:800|spr=crawdad|col=red",
	"Marsh Rabbit|hp=5|ai=critter|fac=animal|move=1|spd=2|tier=1|drop=Hide:500|spr=rabbit|col=tan",
	"Heron|hp=10|ai=critter|fac=animal|move=4|spd=3|tier=1|drop=Feather:900|spr=heron|col=white",
	"Cloud Sheep|hp=40|ai=critter|fac=animal|move=1|spd=4|tier=4|drop=Cloud Fluff:900|spr=sheep|col=white|quip=Baaaa.",
	// -- Gatorland overworld (tier 1)
	"Mosquito Swarm|hp=6|dmg=1-2|spd=2|aspd=5|ai=pack|move=4|tier=1|fame=2|spr=mosquito|col=gray|snd=buzz|quip=BZZZZZ",
	"Swamp Rat|hp=10|dmg=1-4|spd=3|aspd=7|ai=melee|tier=1|fame=3|gold=0-3|drop=Rat Tail:500|spr=rat|col=brown|snd=squeak",
	"Raccoon Bandit|hp=14|dmg=1-3|spd=3|aspd=6|ai=thief|tier=1|fame=6|gold=2-10|poisonous|spr=raccoon|col=gray|quip=Yoink!",
	"Feral Hog|hp=22|dmg=3-6|spd=3|aspd=8|ai=melee|tier=1|fame=6|drop=Hide:700|spr=hog|col=brown|snd=oink",
	"Timber Wolf|hp=20|dmg=3-7|spd=2|aspd=7|ai=pack|tier=1|fame=7|drop=Hide:600|spr=wolf|col=gray|snd=howl",
	"Goblin Poacher|hp=24|dmg=3-8|spd=3|aspd=9|ai=ranged|rng=6|tier=1|fame=9|gold=4-15|drop=Arrow:700,Short Bow:60|spr=goblin|col=green|quip=Gator boots! Gator boots!",
	"Giant Frog|hp=18|dmg=2-6|spd=3|aspd=8|ai=melee|move=3|tier=1|fame=5|drop=Frog Legs:900|spr=frog_big|col=green|snd=ribbit",
	"Cottonmouth|hp=16|dmg=2-5|spd=3|aspd=7|ai=melee|move=3|tier=1|fame=6|poisonous|spr=snake|col=olive|snd=hiss",
	"Bandit|hp=30|dmg=4-9|spd=3|aspd=8|ai=melee|tier=2|fame=10|gold=10-30|drop=Minor Heal Potion:200|spr=bandit|col=red|quip=Your gold or your scales!",
	"Treant|hp=60|dmg=6-12|armor=4|spd=5|aspd=11|ai=melee|tier=2|fame=15|drop=Log:1000|spr=treant|col=wood|quip=Hrrrmm. You stepped on my cousin.",
	// -- Swampton Sewers (tier 1-2)
	"Sewer Rat|hp=12|dmg=2-4|spd=2|aspd=7|ai=pack|tier=1|fame=3|gold=0-4|drop=Rat Tail:600|spr=rat|col=gray|snd=squeak",
	"Green Slime|hp=30|dmg=3-6|spd=4|aspd=9|ai=melee|move=3|tier=1|fame=6|split=Slimelet|spr=slime|col=slime|snd=squish",
	"Slimelet|hp=9|dmg=1-3|spd=3|aspd=8|ai=melee|move=3|tier=1|fame=1|spr=slime_small|col=slime|snd=squish",
	"Giant Cockroach|hp=20|dmg=2-5|spd=1|aspd=6|ai=melee|tier=1|fame=4|spr=roach|col=brown|quip=It survived the apocalypse. It will survive you.",
	"The Rat King|hp=260|dmg=8-14|armor=3|spd=3|aspd=7|ai=boss|tier=2|fame=120|gold=80-160|boss|summon=Sewer Rat|drop=Rat King's Crown:1000|spr=ratking|col=gold|quip=MY KINGDOM FOR A CHEESE!",
	// -- The Bayou (tier 2)
	"Snapping Turtle|hp=40|dmg=5-10|armor=6|spd=4|aspd=9|ai=melee|move=3|tier=2|fame=12|drop=Turtle Shell Shield:30|spr=turtle|col=moss|snd=snap",
	"Voodoo Toad|hp=30|dmg=2-5|spd=3|aspd=10|ai=caster|move=3|spell=Poison|tier=2|fame=14|gold=5-20|drop=Voodoo Lily:700,Gatormoss:400|spr=toad|col=purple|quip=Hex-ribbit!",
	"Crawdad Knight|hp=45|dmg=6-11|armor=5|spd=3|aspd=8|ai=melee|move=3|tier=2|fame=15|gold=5-25|drop=Crawdad:900|spr=crawknight|col=red|quip=En garde, mudling!",
	"Bog Zombie|hp=35|dmg=5-9|spd=5|aspd=9|ai=melee|move=3|tier=2|fame=10|undead|drop=Bone:600|spr=zombie|col=moss|snd=groan",
	"Big Mama Snapper|hp=420|dmg=12-22|armor=8|spd=3|aspd=8|ai=boss|move=3|tier=3|fame=300|gold=200-400|boss|summon=Snapping Turtle|drop=Mossy Relic Scale:1000,Big Mama's Shell:1000|spr=bigmama|col=moss|light=2|quip=WHO'S BEEN SWIMMING IN MY BAYOU?",
	// -- Sunken City of Sogg (tier 3)
	"Drowned Sailor|hp=50|dmg=7-13|spd=3|aspd=8|ai=melee|move=3|tier=3|fame=18|undead|gold=10-30|drop=Bone:500|spr=skeleton|col=bone|quip=Yo ho... ho...",
	"Electric Eel|hp=35|dmg=5-12|spd=1|aspd=6|ai=melee|move=2|tier=3|fame=16|spr=eel|col=cyan|light=2|snd=zap",
	"Merrow Spearman|hp=60|dmg=8-14|armor=3|spd=2|aspd=8|ai=pack|move=3|tier=3|fame=20|gold=10-30|drop=Kelp:600,Swamp Pearl:300|spr=merrow|col=teal",
	"Sea Witch|hp=55|dmg=3-6|spd=3|aspd=10|ai=caster|move=3|spell=Frost Bolt|tier=3|fame=24|gold=20-50|drop=Swamp Pearl:800,Scroll of Frost Bolt:80|spr=witch|col=teal|quip=Such pretty scales. MINE.",
	"Angler Lurker|hp=70|dmg=10-18|spd=3|aspd=9|ai=ambush|move=2|tier=3|fame=26|light=3|drop=Pearl Necklace:40|spr=angler|col=navy",
	"The Drowned King|hp=720|dmg=14-24|armor=8|spd=3|aspd=8|ai=boss|move=3|spell=Frost Bolt|tier=4|fame=500|gold=300-600|boss|summon=Drowned Sailor|drop=Pearl Relic Scale:1000,Trident of the Drowned King:1000|spr=drownking|col=pearl|light=3|quip=KNEEL BEFORE THE TIDE!",
	// -- Gloomfen, the haunted swamp (tier 3)
	"Will-o'-Wisp|hp=30|dmg=3-6|spd=2|aspd=10|ai=coward|move=4|spell=Snap Bolt|tier=3|fame=16|light=4|drop=Ectoplasm:400|spr=wisp|col=wisp|quip=This way... this way...",
	"Swamp Zombie|hp=55|dmg=8-13|spd=5|aspd=10|ai=pack|move=3|tier=3|fame=14|undead|drop=Bone:600|spr=zombie|col=gray|snd=groan",
	"Banshee|hp=60|dmg=4-8|spd=2|aspd=11|ai=caster|move=4|spell=Lightning|tier=3|fame=25|undead|drop=Ectoplasm:700|spr=banshee|col=ghost|quip=EEEEEEEEE!",
	"Ghoul Gator|hp=80|dmg=10-16|spd=3|aspd=8|ai=melee|move=3|tier=3|fame=28|undead|leech=30|drop=Bone:500|spr=ghoulgator|col=ghoul|quip=We could have been friends.",
	"Bone Heron|hp=45|dmg=8-12|spd=2|aspd=8|ai=melee|move=4|tier=3|fame=18|undead|drop=Feather:800,Bone:500|spr=heron|col=bone",
	"Count Crocula|hp=900|dmg=18-30|armor=6|spd=2|aspd=8|ai=boss|move=3|spell=Poison|tier=4|fame=650|gold=400-800|boss|leech=40|summon=Will-o'-Wisp|drop=Shadow Relic Scale:1000,Crocula's Cape:1000|spr=crocula|col=crimson|quip=I vant to bite your... tail.",
	// -- Frostfang Caverns (tier 4)
	"Ice Bat|hp=30|dmg=5-10|spd=1|aspd=6|ai=pack|move=4|tier=4|fame=12|spr=bat|col=ice|snd=screech",
	"Yeti|hp=110|dmg=12-22|armor=4|spd=3|aspd=9|ai=melee|tier=4|fame=40|frostproof|drop=Hide:900,Frost Shard:200|spr=yeti|col=white|quip=RAAARGH (politely)",
	"Frost Wisp|hp=45|dmg=4-8|spd=2|aspd=10|ai=caster|move=4|spell=Frost Bolt|tier=4|fame=26|frostproof|light=2|drop=Frost Shard:500|spr=wisp|col=ice",
	"Penguin Mercenary|hp=50|dmg=9-14|armor=3|spd=2|aspd=7|ai=pack|move=3|tier=4|fame=22|frostproof|gold=15-40|drop=Raw Fish:800|spr=penguin|col=black|quip=Noot noot!",
	"Snow Troll|hp=140|dmg=14-24|armor=4|spd=4|aspd=10|ai=melee|tier=4|fame=45|frostproof|regen=2|drop=Hide:800|spr=troll|col=snow",
	"The Glacier Queen|hp=1100|dmg=15-26|armor=10|spd=3|aspd=8|ai=boss|move=1|spell=Frost Bolt|tier=5|fame=800|gold=500-900|boss|frostproof|summon=Frost Wisp|drop=Frost Relic Scale:1000,Glacier Queen's Tiara:1000|spr=glacierqueen|col=ice|light=4|quip=Your blood will make a lovely sorbet.",
	// -- Mt. Scorchmaw (tier 5)
	"Fire Imp|hp=55|dmg=4-8|spd=2|aspd=10|ai=coward|spell=Fireball|tier=5|fame=26|fireproof|light=2|drop=Firefly Dust:900|spr=imp|col=lava|quip=Hehehehe, crispy!",
	"Salamander|hp=80|dmg=12-20|spd=2|aspd=8|ai=melee|tier=5|fame=34|fireproof|light=2|drop=Magma Core:120|spr=salamander|col=orange",
	"Magma Golem|hp=200|dmg=18-30|armor=12|spd=6|aspd=12|ai=melee|tier=5|fame=60|fireproof|light=3|drop=Magma Core:500,Iron Ore:900|spr=golem|col=lava|snd=rumble",
	"Ash Bat|hp=40|dmg=8-14|spd=1|aspd=6|ai=pack|move=4|tier=5|fame=16|fireproof|spr=bat|col=char|snd=screech",
	"Cinder Cultist|hp=90|dmg=6-10|spd=3|aspd=10|ai=caster|spell=Flame Geyser|tier=5|fame=40|fireproof|gold=30-80|drop=Scroll of Flame Geyser:60,Firefly Dust:900|spr=cultist|col=crimson|quip=The mountain hungers!",
	"Emberjaw the Wyrm|hp=1500|dmg=18-30|armor=12|spd=3|aspd=8|ai=boss|spell=Fireball|tier=6|fame=1200|gold=800-1500|boss|fireproof|summon=Fire Imp|drop=Ember Relic Scale:1000,Emberjaw's Fang:1000|spr=emberjaw|col=lava|light=5|quip=A GATOR? I EAT GATORS FOR BRUNCH.",
	// -- Cloudreach (tier 5)
	"Harpy|hp=70|dmg=10-16|spd=2|aspd=9|ai=ranged|rng=6|move=4|tier=5|fame=28|drop=Feather:900|spr=harpy|col=tan|quip=SKREEEE!",
	"Storm Elemental|hp=120|dmg=8-14|spd=2|aspd=11|ai=caster|move=4|spell=Lightning|tier=5|fame=44|light=3|drop=Storm Essence:400|spr=stormel|col=cyan|snd=thunder",
	"Griffon|hp=160|dmg=18-28|armor=4|spd=2|aspd=8|ai=melee|move=4|tier=5|fame=50|drop=Feather:1000|spr=griffon|col=gold",
	"Sky Pirate|hp=110|dmg=14-22|spd=3|aspd=10|ai=ranged|rng=7|tier=5|fame=40|gold=40-100|drop=Arrow:800,Crossbow:60|spr=pirate|col=navy|quip=Yarr, fall off my island!",
	"The Thunder Roc|hp=1800|dmg=19-32|armor=10|spd=2|aspd=8|ai=boss|move=4|spell=Chain Lightning|tier=6|fame=1500|gold=900-1800|boss|summon=Harpy|drop=Storm Relic Scale:1000,Stormcaller:1000|spr=roc|col=gold|light=3|quip=SKRAAAAAAWK!",
	// -- summons
	"Frog Familiar|hp=60|dmg=6-12|spd=2|aspd=7|ai=summon|fac=player|move=3|tier=1|spr=frog_big|col=gold|quip=Ribbit! (determined)"
];

array<MonDef> build_monsters() {
	array<MonDef> out;
	array<string> names;
	for (int i = 0; i < MON_SRC.size(); ++i) {
		array<string> p = MON_SRC[i].split("|");
		names.push(p[0]);
	}
	for (int i = 0; i < MON_SRC.size(); ++i) {
		array<string> p = MON_SRC[i].split("|");
		MonDef m = MonDef();
		m.name = p[0];
		m.hp = dsl_int(p, "hp", 10);
		array<int> dmg = dsl_range(p, "dmg", 1, 3);
		m.lo = dmg[0];
		m.hi = dmg[1];
		m.armor = dsl_int(p, "armor", 0);
		m.speed = dsl_int(p, "spd", 3);
		m.aspeed = dsl_int(p, "aspd", 8);
		m.ai = index_of_name(AI_NAME, dsl_get(p, "ai", "melee"), 2);
		m.fac = index_of_name(FAC_NAME, dsl_get(p, "fac", "monster"), 2);
		m.range = dsl_int(p, "rng", 1);
		string sp = dsl_get(p, "spell", "");
		m.spell = sp == "" ? -1 : sid(sp);
		m.move = dsl_int(p, "move", 1);
		m.tier = dsl_int(p, "tier", 1);
		m.skill = dsl_int(p, "skill", 250 + m.tier * 120);
		array<int> g = dsl_range(p, "gold", 0, 0);
		m.gold_lo = g[0];
		m.gold_hi = g[1];
		m.fame = dsl_int(p, "fame", 1);
		m.light = dsl_int(p, "light", 0);
		string su = dsl_get(p, "summon", "");
		m.summon = su == "" ? -1 : index_of_name(names, su, -1);
		string spl = dsl_get(p, "split", "");
		m.split = spl == "" ? -1 : index_of_name(names, spl, -1);
		m.undead = dsl_has(p, "undead");
		m.fireproof = dsl_has(p, "fireproof");
		m.frostproof = dsl_has(p, "frostproof");
		m.poisonous = dsl_has(p, "poisonous");
		m.boss = dsl_has(p, "boss");
		m.regen = dsl_int(p, "regen", 0);
		m.leech = dsl_int(p, "leech", 0);
		string drops = dsl_get(p, "drop", "");
		if (drops != "") {
			array<string> dl = drops.split(",");
			for (int k = 0; k < dl.size(); ++k) {
				array<string> ic = dl[k].split(":");
				m.drop_item.push(iid(ic[0]));
				m.drop_chance.push(ic.size() > 1 ? ic[1].to_int(500) : 500);
			}
		}
		m.spr = dsl_get(p, "spr", "rat");
		m.col = dsl_get(p, "col", "gray");
		m.snd = dsl_get(p, "snd", "");
		m.quip = dsl_get(p, "quip", "");
		out.push(m);
	}
	return out;
}

array<MonDef> MONS = build_monsters();

int mid(string name) {
	for (int i = 0; i < MONS.size(); ++i) {
		if (MONS[i].name == name) { return i; }
	}
	throw "unknown monster: " + name;
}

int M_GATOR_NPC = mid("Townsgator");
int M_FROGFOLK = mid("Frogfolk");
int M_TURTLE_NPC = mid("Old Turtle");
int M_GUARD = mid("Swampton Guard");
int M_FAMILIAR = mid("Frog Familiar");

// flat mirrors for the AI hot path
array<int> mon_int_table(int field) {
	array<int> out;
	for (int i = 0; i < MONS.size(); ++i) {
		MonDef m = MONS[i];
		if (field == 0) { out.push(m.speed); }
		else if (field == 1) { out.push(m.move); }
		else if (field == 2) { out.push(m.ai); }
		else if (field == 3) { out.push(m.fac); }
		else { out.push(m.aspeed); }
	}
	return out;
}
array<int> MON_SPEED = mon_int_table(0);
array<int> MON_MOVE = mon_int_table(1);
array<int> MON_AI = mon_int_table(2);
array<int> MON_FAC = mon_int_table(3);
array<int> MON_ASPEED = mon_int_table(4);

// ---------------------------------------------------------------- recipes ---
// output|station|skill|min skill|inputs Name:qty,Name:qty|out qty|tool
array<string> RECIPE_SRC = [
	"Iron Ingot|forge|smithing|0|Iron Ore:1|1",
	"Silver Ingot|forge|smithing|300|Silver Ore:1|1",
	"Mithril Ingot|forge|smithing|650|Mithril Ore:1|1",
	"Short Sword|anvil|smithing|0|Iron Ingot:3|1",
	"Frying Pan|anvil|smithing|50|Iron Ingot:2|1",
	"Cutlass|anvil|smithing|200|Iron Ingot:5|1",
	"Mace|anvil|smithing|250|Iron Ingot:6|1",
	"Broadsword|anvil|smithing|350|Iron Ingot:8|1",
	"War Hammer|anvil|smithing|450|Iron Ingot:10|1",
	"Katana|anvil|smithing|550|Silver Ingot:6,Iron Ingot:2|1",
	"Gator Tooth Blade|anvil|smithing|750|Mithril Ingot:5,Bone:4|1",
	"Obsidian Greatsword|anvil|smithing|900|Mithril Ingot:8,Magma Core:2|1",
	"Bucket Helm|anvil|smithing|50|Iron Ingot:3|1",
	"Iron Helm|anvil|smithing|300|Iron Ingot:5|1",
	"Chainmail Shirt|anvil|smithing|350|Iron Ingot:10|1",
	"Chain Gloves|anvil|smithing|300|Iron Ingot:4|1",
	"Chain Leggings|anvil|smithing|350|Iron Ingot:7|1",
	"Plate Cuirass|anvil|smithing|700|Iron Ingot:16,Silver Ingot:2|1",
	"Plate Gauntlets|anvil|smithing|650|Iron Ingot:6|1",
	"Plate Greaves|anvil|smithing|700|Iron Ingot:12|1",
	"Plate Boots|anvil|smithing|650|Iron Ingot:6|1",
	"Kite Shield|anvil|smithing|450|Iron Ingot:8|1",
	"Tower Shield|anvil|smithing|750|Iron Ingot:14|1",
	"Wooden Shield|anvil|lumber|100|Log:6|1",
	"Short Bow|anvil|lumber|150|Log:5|1",
	"Long Bow|anvil|lumber|450|Log:9,Feather:2|1",
	"Crossbow|anvil|lumber|650|Log:10,Iron Ingot:3|1",
	"Arrow|anvil|lumber|0|Log:1,Feather:1|10",
	"Club|anvil|lumber|0|Log:3|1",
	"Cooked Fish|fire|cooking|0|Raw Fish:1|1",
	"Blackened Catfish|fire|cooking|250|Catfish:1,Wild Onion:1|1",
	"Fried Frog Legs|fire|cooking|100|Frog Legs:1|1",
	"Crawdad Etouffee|fire|cooking|400|Crawdad:2,Wild Onion:1,Bread:1|1",
	"Toasted Marshmallow|fire|cooking|0|Marshmallow:1|1",
	"Smoked Eel|fire|cooking|600|Lava Eel:1|1",
	"Bread|fire|cooking|50|Cattail Root:1|2",
	"Minor Heal Potion|anywhere|alchemy|0|Cattail Root:1|1|Mortar and Pestle",
	"Cure Potion|anywhere|alchemy|100|Wild Onion:2|1|Mortar and Pestle",
	"Eyeshine Potion|anywhere|alchemy|150|Bog Silk:1,Firefly Dust:1|1|Mortar and Pestle",
	"Heal Potion|anywhere|alchemy|300|Cattail Root:2,Wild Onion:1|1|Mortar and Pestle",
	"Mana Potion|anywhere|alchemy|350|Swamp Pearl:1,Bog Silk:1|1|Mortar and Pestle",
	"Swamp Tonic|anywhere|alchemy|400|Kelp:2,Cattail Root:1|1|Mortar and Pestle",
	"Strength Potion|anywhere|alchemy|500|Gnarlroot:2,Gatormoss:1|1|Mortar and Pestle",
	"Greater Heal Potion|anywhere|alchemy|650|Cattail Root:3,Wild Onion:1,Gnarlroot:1|1|Mortar and Pestle",
	"Fire Resist Potion|anywhere|alchemy|700|Magma Core:1,Wild Onion:2|1|Mortar and Pestle",
	"Swamp Gas Bomb|anywhere|alchemy|550|Firefly Dust:2,Kelp:1|1|Mortar and Pestle",
	"Bandage|anywhere|healing|0|Hide:1|5"
];

array<RecipeDef> build_recipes() {
	array<RecipeDef> out;
	for (int i = 0; i < RECIPE_SRC.size(); ++i) {
		array<string> p = RECIPE_SRC[i].split("|");
		RecipeDef r = RecipeDef();
		r.out = iid(p[0]);
		r.station = p[1] == "anywhere" ? 0 : index_of_name(STATION_NAME, p[1], 0);
		r.skill = index_of_name(SKILL_KEY, p[2], 0);
		r.min = p[3].to_int(0);
		array<string> ins = p[4].split(",");
		for (int k = 0; k < ins.size(); ++k) {
			array<string> iq = ins[k].split(":");
			r.in_item.push(iid(iq[0]));
			r.in_qty.push(iq[1].to_int(1));
		}
		r.out_qty = p[5].to_int(1);
		r.tool = p.size() > 6 ? iid(p[6]) : -1;
		out.push(r);
	}
	return out;
}

array<RecipeDef> RECIPES = build_recipes();

// ---------------------------------------------------------------- worlds ---
// gen: 0 overworld, 1 bayou, 2 sunken city, 3 haunted, 4 ice caves, 5 volcano, 6 sky, 7 sewers, 8 warrens (a maze below the sewers)
array<string> WORLD_SRC = [
	"Gatorland|w=128|h=96|gen=0|tier=1|amb=0|mus=0|wx=ccccrrft|col=green|b=The green heart of the swamp. Home.",
	"The Bayou|w=96|h=72|gen=1|tier=2|amb=0|mus=1|wx=crrftt|col=moss|boss=Big Mama Snapper|b=Cypress knees and bad intentions.",
	"Sunken City of Sogg|w=88|h=72|gen=2|tier=3|amb=7|mus=2|wx=cccf|col=teal|boss=The Drowned King|b=A drowned city. Hold your breath. Gators can.",
	"Gloomfen|w=96|h=72|gen=3|tier=3|amb=4|mus=3|wx=fffr|col=ghoul|boss=Count Crocula|b=The fog here has opinions.",
	"Frostfang Caverns|w=88|h=64|gen=4|tier=4|amb=5|mus=4|wx=sscs|col=ice|boss=The Glacier Queen|b=Cold-blooded is not a compliment down here.",
	"Mt. Scorchmaw|w=88|h=72|gen=5|tier=5|amb=9|mus=5|wx=aaca|col=lava|boss=Emberjaw the Wyrm|b=The mountain breathes. Try not to be what it exhales.",
	"Cloudreach|w=100|h=72|gen=6|tier=5|amb=14|mus=6|wx=cctt|col=sky|boss=The Thunder Roc|b=Islands in the sky. Mind the gap.",
	"Swampton Sewers|w=64|h=48|gen=7|tier=1|amb=3|mus=7|wx=c|col=slime|boss=The Rat King|b=It smells exactly how you think.",
	"Raccoon Warrens|w=57|h=41|gen=8|tier=2|amb=2|mus=7|wx=c|col=brown|b=Somebody down here has your coin purse."
];

array<WorldDef> build_worlds() {
	array<WorldDef> out;
	for (int i = 0; i < WORLD_SRC.size(); ++i) {
		array<string> p = WORLD_SRC[i].split("|");
		WorldDef w = WorldDef();
		w.name = p[0];
		w.w = dsl_int(p, "w", 64);
		w.h = dsl_int(p, "h", 48);
		w.gen = dsl_int(p, "gen", 0);
		w.tier = dsl_int(p, "tier", 1);
		w.ambient = dsl_int(p, "amb", 0);
		w.music = dsl_int(p, "mus", 0);
		string b = dsl_get(p, "boss", "");
		w.boss = b == "" ? -1 : mid(b);
		w.weather = dsl_get(p, "wx", "c");
		w.banner = dsl_get(p, "b", "");
		w.col = dsl_get(p, "col", "green");
		out.push(w);
	}
	return out;
}

array<WorldDef> WORLDS = build_worlds();
int WORLD_COUNT = WORLDS.size();
int W_GATORLAND = 0;
int W_SEWERS = 7;
int W_WARRENS = 8;

// ------------------------------------------------------------- npc roles ---
array<string> ROLE_SRC = [
	"Gus|the Weaponsmith|shop=0|lines=Sharp things! Get your sharp things!;Every gator needs a good blade. Or a frying pan.",
	"Marguerite|the Armorer|shop=1|lines=Scales are nice, but plate is nicer.;I once made a helm for a frog. Never again.",
	"Madame Hoodoo|the Alchemist|shop=2|lines=Potions, powders, reagents. No refunds on curses.;Bring me gnarlroot, cher.",
	"Boudreaux|the Provisioner|shop=3|lines=Tools, food, arrows. All you need for the swamp life.;You look hungry. You always look hungry.",
	"Thibodeaux|the Fishmonger|shop=4|lines=Fish! Fresh-ish fish!;I would give a fortune for a Golden Carp.",
	"Penny|the Banker|banks|lines=Your gold is safe with Swampton Savings and Loan.;We have not been robbed in almost a week.",
	"Sister Clementine|the Healer|heals|lines=Bless your scaly heart.;Ghosts to the front, please. No cutting.",
	"Mayor Beauregard|the Mayor|lines=Welcome to Swampton, finest swamp in the swamp!;We have a rat problem. And a raccoon problem. And a problem problem.",
	"Captain Fontenot|of the Guard|lines=Keep your claws to yourself in town.;Raccoons. I hate raccoons.",
	"Oracle Mireweather|the Shrine Keeper|heals|lines=Six realms, six scales, one gator.;The Lilygates hum when a hero is near."
];

array<NpcRole> build_roles() {
	array<NpcRole> out;
	for (int i = 0; i < ROLE_SRC.size(); ++i) {
		array<string> p = ROLE_SRC[i].split("|");
		NpcRole r = NpcRole();
		r.name = p[0];
		r.title = p[1];
		r.shop = dsl_int(p, "shop", -1);
		r.heals = dsl_has(p, "heals");
		r.banks = dsl_has(p, "banks");
		r.lines = dsl_get(p, "lines", "");
		out.push(r);
	}
	return out;
}

array<NpcRole> ROLES = build_roles();
int R_WEAPONS = 0;
int R_ARMOR = 1;
int R_ALCHEMIST = 2;
int R_PROVISIONER = 3;
int R_FISHMONGER = 4;
int R_BANKER = 5;
int R_HEALER = 6;
int R_MAYOR = 7;
int R_CAPTAIN = 8;
int R_ORACLE = 9;

// shop stock by shop id: item names
array<string> SHOP_SRC = [
	"Rusty Dagger,Short Sword,Cutlass,Broadsword,Club,Frying Pan,Mace,War Hammer,Short Bow,Long Bow,Arrow,Apprentice Wand",
	"Straw Hat,Leather Cap,Bucket Helm,Iron Helm,Leather Tunic,Chainmail Shirt,Leather Gloves,Leather Leggings,Chain Leggings,Sandals,Leather Boots,Swamp Waders,Pot Lid,Wooden Shield,Kite Shield",
	"Minor Heal Potion,Heal Potion,Mana Potion,Cure Potion,Eyeshine Potion,Swamp Pearl,Gatormoss,Wild Onion,Cattail Root,Gnarlroot,Voodoo Lily,Bog Silk,Firefly Dust,Spellbook,Scroll of Heal,Scroll of Snap Bolt,Scroll of Eyeshine,Scroll of Cure,Scroll of Frost Bolt,Scroll of Fireball,Scroll of Recall,Mortar and Pestle,Homeward Rune",
	"Bread,Cheese Wheel,Moon Pie,Marshmallow,Hot Sauce,Fishing Pole,Pickaxe,Hatchet,Smith Hammer,Bandage,Arrow,Kazoo",
	"Raw Fish,Catfish,Cooked Fish,Fishing Pole,Kelp"
];

array<array<int>> build_shops() {
	array<array<int>> out;
	for (int i = 0; i < SHOP_SRC.size(); ++i) {
		array<int> stock;
		array<string> names = SHOP_SRC[i].split(",");
		for (int k = 0; k < names.size(); ++k) { stock.push(iid(names[k])); }
		out.push(stock);
	}
	return out;
}

array<array<int>> SHOPS = build_shops();

// ---------------------------------------------------------------- quests ---
// type: 0 kill kind, 1 bring item, 2 slay boss, 3 visit worlds, 4 craft, 5 catch fish
array<string> QUEST_SRC = [
	"Rats in the Cellar|giver=Mayor Beauregard|type=0|target=Sewer Rat|count=8|gold=60|reward=Leather Cap|o=The sewers under town are crawling. Squash 8 Sewer Rats for me, would you?|d=Splendid! The cellar smells almost tolerable now.",
	"The Masked Menace|giver=Captain Fontenot|type=0|target=Raccoon Bandit|count=3|gold=80|reward=Lucky Frog Charm|o=Raccoon Bandits lift purses and scurry off to their warrens below the sewers. Teach 3 of them some manners; they drop what they stole.|d=Ha! That will show the little burglars.",
	"Frog Legs Friday|giver=Boudreaux|type=1|target=Frog Legs|count=5|gold=50|reward=Fried Frog Legs|rq=3|o=Friday is frog leg night and I am plumb out. Bring me 5 Frog Legs?|d=Now THAT is a Friday. Here, have some on the house.",
	"Gone Fishin'|giver=Thibodeaux|type=5|count=10|gold=100|reward=Fish Slapper|o=Catch 10 fish, any fish, and I will teach you the ancient art of the slap.|d=A natural! Take this. Treat it with respect.",
	"Iron Will|giver=Gus|type=4|count=1|gold=40|reward=Iron Ingot|rq=10|o=Smelt some ore at the forge and make anything at the anvil. Show me what you have got.|d=Not bad for a gator with no thumbs.",
	"Reagent Run|giver=Madame Hoodoo|type=1|target=Gnarlroot|count=3|gold=70|reward=Scroll of Fireball|o=Bring me 3 Gnarlroot, cher. Reagents grow wild all over Gatorland.|d=Mmm, fresh. Here is a little something that goes whoosh.",
	"The Rat King|giver=Mayor Beauregard|type=2|target=The Rat King|count=1|gold=150|reward=Heal Potion|rq=3|need=0|o=The rats have a KING. Down in the deepest sewer. End his reign!|d=Long live... nobody! Swampton owes you.",
	"Tourist|giver=Oracle Mireweather|type=3|count=8|gold=200|reward=Rubber Duck|o=Walk every realm the Lilygates touch. All eight. The swamp wants to be seen.|d=You have seen it all. Take this. It is more powerful than it looks.",
	"Big Mama|giver=Oracle Mireweather|type=2|target=Big Mama Snapper|count=1|gold=250|reward=Greater Heal Potion|rq=3|o=Big Mama Snapper rules the Bayou and she bites. Take her Relic Scale.|d=The first scale. Five remain.",
	"The Six Scales|giver=Oracle Mireweather|type=1|target=Mossy Relic Scale|count=1|gold=1000|reward=Crown of the Swamp|need=8|o=Gather all six Relic Scales from the realm tyrants and return to the Shrine. Become the Grand Gator.|d=The swamp crowns you. Rise, Grand Gator!"
];

array<QuestDef> build_quests() {
	array<QuestDef> out;
	for (int i = 0; i < QUEST_SRC.size(); ++i) {
		array<string> p = QUEST_SRC[i].split("|");
		QuestDef q = QuestDef();
		q.name = p[0];
		string giver = dsl_get(p, "giver", "");
		for (int r = 0; r < ROLES.size(); ++r) {
			if (ROLES[r].name == giver) { q.giver = r; }
		}
		q.type = dsl_int(p, "type", 0);
		string tgt = dsl_get(p, "target", "");
		if (tgt != "") {
			q.target = q.type == 1 ? iid(tgt) : mid(tgt);
		}
		q.count = dsl_int(p, "count", 1);
		q.gold = dsl_int(p, "gold", 0);
		string rw = dsl_get(p, "reward", "");
		q.reward = rw == "" ? -1 : iid(rw);
		q.reward_qty = dsl_int(p, "rq", 1);
		q.needs = dsl_int(p, "need", -1);
		q.offer = dsl_get(p, "o", "");
		q.done = dsl_get(p, "d", "");
		out.push(q);
	}
	return out;
}

array<QuestDef> QUESTS = build_quests();
int Q_SIX_SCALES = 9;
