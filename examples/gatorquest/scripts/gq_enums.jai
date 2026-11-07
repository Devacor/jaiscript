// Gator Quest - enums and flag bits.

enum Dir { n, ne, e, se, s, sw, w, nw }

enum Sk {
	swords, maces, archery, wrestling, tactics, parrying, magery, meditation,
	resist, healing, fishing, mining, lumber, smithing, cooking, alchemy,
	stealth, swimming, count
}

enum Slot { head, neck, body, hands, legs, feet, weapon, offhand, ring, trinket, count }

enum Kind { weapon, armor, shield, potion, scroll, food, reagent, tool, resource, ammo, trinket, junk, gem, quest, gold }

enum Rar { common, uncommon, rare, epic, legendary }

enum Fac { player, npc, monster, animal, guard }

enum Ai { none, wander, melee, pack, ranged, caster, coward, thief, ambush, boss, guard, critter, summon }

enum Ev { dmg, heal, miss, death, proj, fx, sound, log, shake, banner, chat, gain, pickup, spawn, warp, dialog }

enum Fx { sparks, blood, splash, fire, frost, zap, heal, smoke, poison, magic, gold, loot, levelup, warp, bubbles, bones, dust, holy }

// mobile flag bits
int MF_GHOST = 1;
int MF_PVP = 2;
int MF_BOSS = 4;
int MF_HIDDEN = 8;
int MF_INVULN = 16;
int MF_SUMMON = 32;
int MF_CRIMINAL = 64;
int MF_ALERT = 128;
int MF_FLEEING = 256;
int MF_SWIM = 512;
int MF_ELITE = 1024;
