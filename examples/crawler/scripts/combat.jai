// Combat: to-hit, damage and criticals, the one funnel every lost hit point passes through,
// death and its spoils, experience, and the class skills (each fires down your facing).

function is_player(var& e) -> bool { return e.glyph == "@"; }

function entity_atk(var& e) -> int { return is_player(e) ? e.total_atk() : e.atk; }
function entity_def(var& e) -> int { return is_player(e) ? e.total_def() : e.def; }
function entity_eva(var& e) -> int { return is_player(e) ? e.total_eva() : e.eva; }
function entity_crit(var& e) -> int { return is_player(e) ? e.total_crit() : e.crit; }

// a foe's health reads as a word; exact numbers are the player's privilege
function health_word(int hp, int cap) -> string {
	if (hp >= cap) { return "unscathed"; }
	if (hp * 10 >= cap * 7) { return "scuffed"; }
	if (hp * 10 >= cap * 4) { return "bloodied"; }
	if (hp * 10 >= cap * 2) { return "wavering"; }
	return "near death";
}

// " -- bloodied" after a hit that leaves the foe standing; a kill announces itself
function health_note(int hp_after, int cap) -> string {
	if (hp_after <= 0) { return ""; }
	return ` -- ${health_word(hp_after, cap)}`;
}

// the player's HP once a hit lands, for lines logged before hurt_entity applies it
function player_hp_after(int dmg) -> int {
	if (G.player.god) { return G.player.hp; }
	int after = G.player.hp - dmg;
	if (after <= 0 && G.player.trait == "stubborn" && !G.player.stubborn_spent) { return 1; }
	return imax(0, after);
}

function player_attack_verb() -> string {
	var w = G.player.weapon;
	if (w == null) { return "pummel"; }
	if (w.name.find("Pike") >= 0) { return "skewer"; }
	if (w.name.find("Shiv") >= 0) { return "shank"; }
	if (w.name.find("Brand") >= 0) { return "sear"; }
	return "slash";
}

// "The Grublin", but Vexadrach is a name
function mon_subject(var& m) -> string { return m.behavior == "boss" ? m.name : `The ${m.name}`; }
function mon_object(var& m) -> string { return m.behavior == "boss" ? m.name : `the ${m.name}`; }

// the noun of each monster verb, for "you slip the Gloop's lunge"
map<string, string> VERB_NOUNS = {
	"bites": "bite", "claws": "claws", "rakes": "talons", "engulfs": "lunge",
	"chomps": "chomp", "swats": "swat", "pelts": "volley", "savages": "fury"
};

function verb_noun(string verb) -> string { return VERB_NOUNS.has(verb) ? VERB_NOUNS[verb] : "attack"; }

// Every hit point lost in the game passes through here.
function hurt_entity(var& target, int amount, string source) -> void {
	if (!target.alive) { return; }
	if (!is_player(target)) {
		target.hp -= amount;
		view_note_hit(target.id);
		fx_number(target, amount);
		if (target.hp <= 0) {
			target.hp = 0;
			target.alive = false;
			view_note_kill(target.x, target.y);
			fx_death(target);
			on_monster_death(target);
		}
		return;
	}
	if (target.god) { return; }
	fx_screen(`-${amount}`, INK_BAD);
	target.hp -= amount;
	G.last_dmg += amount;
	G.last_dmg_src = source;
	if (target.trait == "curl" && target.hp * 3 < target.total_max_hp() && !target.has_effect("curl")) {
		add_effect(target, "curl", 5, 3);
		log_ink(INK_DIM, "You curl: scales lock into a living shield. (+3 def)");
	}
	if (target.hp > 0) { return; }
	if (target.trait == "stubborn" && !target.stubborn_spent) {
		target.stubborn_spent = true;
		target.hp = 1;
		log_ink(INK_TITLE, "Too stubborn to die! You refuse, loudly, at 1 HP.");
		return;
	}
	target.hp = 0;
	target.alive = false;
	G.over = true;
	G.death_cause = source;
}

function heal_entity(var& target, int amount) -> void {
	if (!target.alive) { return; }
	int cap = is_player(target) ? target.total_max_hp() : target.max_hp;
	target.hp = imin(cap, target.hp + amount);
}

function on_monster_death(var& monster) -> void {
	log_ink(INK_GOOD, DEATH_LINES.has(monster.kind) ? DEATH_LINES[monster.kind] : `${mon_subject(monster)} is destroyed!`);
	if (monster.behavior == "boss") {
		G.boss_slain = true;   // the game loop runs the victory
		return;
	}
	G.player.kills += 1;
	gain_xp(monster.xp_reward());
	if (monster.kind == "gloop") { split_gloop(monster.x, monster.y); }
	int coins = monster.gold_reward(RNG);
	if (coins > 0) {
		Item pile = make_gold(G.floor);
		pile.amount = coins;
		pile.x = monster.x;
		pile.y = monster.y;
		G.ground.push(pile);
	}
	if (RNG.chance(18 + monster.tier * 3)) {
		Item drop = make_floor_item(G.floor);
		drop.x = monster.x;
		drop.y = monster.y;
		G.ground.push(drop);
		log_msg(`${mon_subject(monster)} drops something.`);
	}
}

// Two Glooplings well up on the free tiles around a slain Gloop, in a fixed scan order. They
// wait in SPAWNS until the turn ends, since a death can land while G.monsters is being walked.
array<Spot> SPAWNS = [];

function split_gloop(int x, int y) -> void {
	int made = 0;
	for (Step d : EIGHT_DIRS) {
		if (made >= 2) { break; }
		int nx = x + d.dx;
		int ny = y + d.dy;
		if (!is_walkable(nx, ny) || (nx == G.player.x && ny == G.player.y) || monster_index_at(nx, ny) >= 0) { continue; }
		bool taken = false;
		for (auto s : SPAWNS) {
			if (s.x == nx && s.y == ny) { taken = true; }
		}
		if (taken) { continue; }
		SPAWNS.push(Spot{nx, ny});
		made += 1;
	}
}

function flush_spawns() -> void {
	for (auto s : SPAWNS) {
		if (monster_index_at(s.x, s.y) < 0 && !(s.x == G.player.x && s.y == G.player.y)) { spawn_monster("gloopling", s.x, s.y); }
	}
	SPAWNS = [];
}

function gain_xp(int amount) -> void {
	G.player.xp += amount;
	while (G.player.xp >= G.player.xp_next()) {
		G.player.xp -= G.player.xp_next();
		G.player.level += 1;
		G.player.max_hp += 2;
		G.player.hp = imin(G.player.hp + 2, G.player.total_max_hp());
		G.level_pending = true;
		log_ink(INK_TITLE, `Welcome to level ${G.player.level}! Choose a boon.`);
		fx_banner("LEVEL UP", `LEVEL ${G.player.level}`, INK_TITLE);
	}
}

// one swing, melee or ranged: dodge, damage, critical, then the attacker's riders
function attack(var& attacker, var& defender, string verb) -> void {
	if (RNG.chance(iclamp(entity_eva(defender), 0, 60))) {
		if (is_player(defender)) {
			log_ink(INK_INFO, `JAI's blessing! You slip ${mon_object(attacker)}'s ${verb_noun(verb)}.`);
			fx_screen("DODGE", INK_INFO);
		} else {
			log_ink(INK_DIM, `${mon_subject(defender)} twists aside -- your ${verb} misses.`);
			fx_word(defender.x, defender.y, "MISS", INK_DIM);
		}
		return;
	}
	int dmg = imax(1, entity_atk(attacker) + RNG.roll(-1, 2) - entity_def(defender));
	bool crit = RNG.chance(iclamp(entity_crit(attacker), 0, 60));
	if (crit) { dmg *= 2; }
	if (is_player(attacker)) {
		FX_NEXT_CRIT = crit;
		fx_slash(defender);
		string crit_note = crit ? paint(INK_TITLE, "Critical! ") : "";
		log_msg(`${crit_note}You ${verb} ${mon_object(defender)} for ${paint(INK_BRIGHT, `${dmg}`)}${health_note(imax(0, defender.hp - dmg), defender.max_hp)}.`);
		if (attacker.equip_bonus("burn") > 0 && RNG.chance(attacker.equip_bonus("burn"))) {
			add_effect(defender, "burn", 3, 2);
			log_ink(INK_BAD, `Your weapon's ember bites deep -- ${mon_object(defender)} is burning!`);
		}
	} else {
		string crit_note = crit ? paint(INK_BAD, " A crushing hit!") : "";
		log_msg(`${mon_subject(attacker)} ${verb} you for ${paint(INK_BAD, `${dmg}`)} (HP ${player_hp_after(dmg)}/${G.player.total_max_hp()}).${crit_note}`);
	}
	hurt_entity(defender, dmg, attacker.name);
	if (is_player(attacker) || !defender.alive) { return; }
	if (attacker.burns && RNG.chance(25)) {
		add_effect(defender, "burn", 3, 2);
		log_ink(INK_BAD, "Gray fire clings to you! (Burning: -2 HP/turn, 3 turns)");
	}
	if (attacker.withers && defender.alive && RNG.chance(35)) {
		add_effect(defender, "wither", 8, 2);
		log_ink(INK_MAGIC, "Curse-dust settles into your joints. (-2 atk/def, 8 turns)");
	}
}

// ------------------------------------------------------------------ skills ---
// the nearest visible monster's index in G.monsters within max_range, or -1
function nearest_visible_monster_index(int max_range) -> int {
	int best = -1;
	int best_dist = max_range + 1;
	for (int i = 0; i < G.monsters.size(); ++i) {
		var& m = G.monsters[i];
		if (!m.alive || m.disguised || !is_visible(m.x, m.y)) { continue; }
		int d = cheb(G.player.x, G.player.y, m.x, m.y);
		if (d < best_dist) {
			best_dist = d;
			best = i;
		}
	}
	return best;
}

function spend_focus(int& pool, int cost) -> bool {
	if (pool < cost) { return false; }
	pool -= cost;
	return true;
}

// the class skill, down your facing: the first live foe within its range, walls block
function use_skill() -> bool {
	int range = G.player.skill == "bulwark" ? 1 : (G.player.skill == "thornvolley" ? 5 : 6);
	int ti = facing_target_index(range);
	if (ti < 0) {
		log_msg(range == 1 ? "Nothing ahead to slam." : "Nothing down that corridor.");
		return false;
	}
	if (!spend_focus(G.player.focus, G.player.skill_cost)) {
		log_msg("Not enough focus.");
		return false;
	}
	var& target = G.monsters[ti];
	fx_skill(G.player.skill, target.x, target.y);
	switch (G.player.skill) {
		case "bulwark": {
			int dmg = imax(1, (G.player.total_atk() * 5) / 2 - entity_def(target));
			log_ink(INK_INFO, `BULWARK SLAM! ${mon_subject(target)} takes ${dmg}${health_note(imax(0, target.hp - dmg), target.max_hp)}.`);
			hurt_entity(target, dmg, "a bulwark slam");
			break;
		}
		case "emberbolt": {
			int dmg = imax(1, G.player.total_atk() + 3 - entity_def(target));
			log_ink(INK_BAD, `Emberbolt sears ${mon_object(target)} for ${dmg}${health_note(imax(0, target.hp - dmg), target.max_hp)}!`);
			hurt_entity(target, dmg, "an emberbolt");
			if (target.alive) {
				add_effect(target, "burn", 3, 2);
				log_ink(INK_BAD, `${mon_subject(target)} is burning. (-2 HP/turn, 3 turns)`);
			}
			break;
		}
		case "thornvolley": {
			int dmg = imax(1, G.player.total_atk() - entity_def(target));
			log_ink(INK_GOOD, `Thorns skewer ${mon_object(target)} for ${dmg}${health_note(imax(0, target.hp - dmg), target.max_hp)}!`);
			hurt_entity(target, dmg, "a thorn volley");
			break;
		}
		case "wither": {
			add_effect(target, "wither", 8, 2);
			heal_entity(G.player, 3);
			fx_screen("+3", INK_GOOD);
			log_ink(INK_MAGIC, `${mon_subject(target)} wilts (-2 atk/def, 8 turns); you sip its vigor (+3 HP, ${G.player.hp}/${G.player.total_max_hp()}).`);
			break;
		}
	}
	return true;
}
