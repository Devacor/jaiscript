// The run: the Game object, spawning, facing-locked movement, the turn loop, saves through the
// archive, and the autopilot the smoke test drives. One key is one turn; the monsters act on the
// same grid whichever way the camera points.

class Game {
	int floor = 1;
	int turn = 0;
	int next_id = 1;
	int facing = 1;               // 0 N, 1 E, 2 S, 3 W: the whole genre in one int
	bool show_map = false;        // m: the overhead map in the forward view's place
	bool over = false;
	bool victory = false;
	bool boss_slain = false;
	bool quit = false;
	bool saved = false;
	bool level_pending = false;
	bool autopilot = false;
	bool smoke = false;
	string death_cause = "misadventure";
	int hash = 2166136261;
	int last_dmg = 0;             // damage taken since the last action
	string last_dmg_src = "";
	int stairs_x = -1;
	int stairs_y = -1;
	bool statue_used = false;
	bool mural_used = false;
	Player player;
	array<string> msgs;
	array<int> msg_turns;         // the turn each message was last logged on
	array<int> msg_counts;        // how many times in a row it was logged
	array<Monster> monsters;
	array<Item> ground;
	array<int> tiles;
	array<bool> seen;
	array<bool> visible;
	array<Room> rooms;
	map<string, bool> known_potions;
	map<string, bool> known_scrolls;
	map<string, string> potion_looks;
	map<string, string> scroll_looks;
	var boss_co = null;           // Vexadrach's fight, a live boss_brain coroutine

	// Vexadrach's choreography. A coroutine method: the handle keeps this Game alive, and the
	// phase (enraged, mid-telegraph) lives in the coroutine's own locals between turns.
	coroutine void boss_brain(int boss_id) {
		bool enraged = false;
		while (true) {
			int bi = monster_index_by_id(boss_id);
			if (bi < 0) { return; }

			if (!enraged && monsters[bi].hp * 10 < monsters[bi].max_hp * 3) {
				enraged = true;
				monsters[bi].atk += 3;
				log_ink(INK_BAD, "Vexadrach ROARS. The gray fire in his gullet flickers... almost color.");
			}

			int dist = cheb(monsters[bi].x, monsters[bi].y, player.x, player.y);
			if (dist <= monsters[bi].range && dist > 1 && sees_player(monsters[bi])) {
				// telegraph, then breathe: two turns of committed drama
				log_ink(INK_DIM, "Vexadrach's throat glows the color of a dead television.");
				yield;

				bi = monster_index_by_id(boss_id);
				if (bi < 0) { return; }
				if (cheb(monsters[bi].x, monsters[bi].y, player.x, player.y) <= monsters[bi].range && sees_player(monsters[bi])) {
					log_ink(INK_DIM, "HOLLOW FIRE floods the corridor: heatless light, lightless heat.");
					fx_bolt(4, monsters[bi].x + 0.5, monsters[bi].y + 0.5, player.x, player.y, 0xDCDCE6);
					int dmg = imax(2, monsters[bi].atk - entity_def(player) + RNG.roll(0, 3));
					log_ink(INK_BAD, `The gray torrent sears you for ${dmg} (HP ${player_hp_after(dmg)}/${player.total_max_hp()}).`);
					hurt_entity(player, dmg, "hollow fire");
					if (player.alive) {
						add_effect(player, "burn", 3, 2);
						log_ink(INK_BAD, "You are burning! (-2 HP/turn, 3 turns)");
					}
				} else {
					log_ink(INK_DIM, "The gray torrent scorches empty stone. You were elsewhere.");
				}
				yield;

				if (!enraged) { yield; }   // the recovery turn: his one weakness is cardio
				continue;
			}

			if (adjacent_to_player(monsters[bi])) {
				attack(monsters[bi], player, "savages");
			} else if (sees_player(monsters[bi]) || monsters[bi].aggro) {
				monsters[bi].aggro = true;
				step_toward(monsters[bi], player.x, player.y);
			}
			yield;
		}
	}
}

function ground_index_at(int x, int y) -> int {
	for (int i = 0; i < G.ground.size(); ++i) {
		if (G.ground[i].x == x && G.ground[i].y == y) { return i; }
	}
	return -1;
}

// each run shuffles the potion and scroll looks onto their ids
function assign_looks() -> void {
	G.potion_looks = {};
	G.scroll_looks = {};
	array<string> looks = POTION_LOOKS;
	for (auto id : POTIONS.keys()) {
		int pick_i = RNG.next(looks.size());
		G.potion_looks[id] = looks[pick_i];
		looks.remove(pick_i);
	}
	array<string> scroll_looks = SCROLL_LOOKS;
	for (auto id : SCROLLS.keys()) {
		int pick_i = RNG.next(scroll_looks.size());
		G.scroll_looks[id] = scroll_looks[pick_i];
		scroll_looks.remove(pick_i);
	}
}

// configure before the push: the array holds its own copy, the only real monster from then on
function spawn_monster(string kind, int x, int y) -> void {
	Monster m(kind);
	m.id = G.next_id;
	G.next_id += 1;
	m.x = x;
	m.y = y;
	int depth_bonus = imax(0, G.floor - BESTIARY[kind].min_floor);
	m.hp += depth_bonus;
	m.max_hp += depth_bonus;
	G.monsters.push(m);
}

function random_open_tile(int min_dist_from_player) -> var {
	for (int attempt = 0; attempt < 300; ++attempt) {
		int x = RNG.next(MAP_W);
		int y = RNG.next(MAP_H);
		if (tile_at(x, y) != TILE_FLOOR || monster_index_at(x, y) >= 0) { continue; }
		if (cheb(x, y, G.player.x, G.player.y) < min_dist_from_player) { continue; }
		return Spot{x, y};
	}
	return null;
}

function populate_floor() -> void {
	G.monsters = [];
	G.ground = [];
	int count = 3 + G.floor;
	if (has_welcome()) {
		Welcome w = welcome_layout();
		Item relic = make_lastlight();
		relic.x = w.find.x;
		relic.y = w.find.y;
		G.ground.push(relic);
		if (tile_at(w.greeter.x, w.greeter.y) == TILE_FLOOR) {
			spawn_monster("grublin", w.greeter.x, w.greeter.y);
			count -= 1;
		}
	}
	if (G.floor >= 10) {
		spawn_monster("vexadrach", 50, 10);
		spawn_monster("drakelet", 46, 7);
		spawn_monster("drakelet", 46, 13);
		spawn_monster("drakelet", 52, 10);
		return;
	}
	SpawnTable table = spawn_table(G.floor);
	for (int i = 0; i < count; ++i) {
		var spot = random_open_tile(7);
		if (spot == null) { continue; }
		spawn_monster(table.kinds[pick_weighted(RNG, table.weights)], spot.x, spot.y);
	}
	int item_count = 2 + RNG.next(3);
	for (int i = 0; i < item_count; ++i) {
		var spot = random_open_tile(2);
		if (spot == null) { continue; }
		Item item = make_floor_item(G.floor);
		item.x = spot.x;
		item.y = spot.y;
		G.ground.push(item);
	}
	int gold_count = 1 + RNG.next(2);
	for (int i = 0; i < gold_count; ++i) {
		var spot = random_open_tile(2);
		if (spot == null) { continue; }
		Item pile = make_gold(G.floor);
		pile.x = spot.x;
		pile.y = spot.y;
		G.ground.push(pile);
	}
}

// face the first open cardinal, in a fixed scan order; floor 1 faces its welcome
function face_open_direction() -> void {
	if (has_welcome()) {
		G.facing = welcome_layout().face;
		return;
	}
	for (int f = 0; f < 4; ++f) {
		if (is_walkable(G.player.x + FACE_DX[f], G.player.y + FACE_DY[f])) {
			G.facing = f;
			return;
		}
	}
	G.facing = 1;
}

function enter_floor() -> void {
	FloorPlan plan = generate_floor(G.floor);
	G.player.x = plan.start.x;
	G.player.y = plan.start.y;
	G.stairs_x = plan.stairs.x;
	G.stairs_y = plan.stairs.y;
	G.statue_used = false;
	G.mural_used = false;
	G.player.stubborn_spent = false;
	G.boss_co = null;
	MONSTER_BRAINS = {};
	populate_floor();
	face_open_direction();
	view_cache_invalidate();
	compute_fov();
	FLOOR_START = G.player;
	FLOOR_START_TURN = G.turn;
	if (G.floor == 10) {
		log_ink(INK_BAD, "The air tastes of ash and unspent color. HE is here.");
		fx_banner("FLOOR 10", "HE IS HERE", INK_BAD);
	} else {
		log_ink(INK_TITLE, `Floor ${G.floor}: ${theme_name(G.floor)}.`);
		fx_banner(`FLOOR ${G.floor}`, string::to_upper(theme_name(G.floor)), theme_of(G.floor).glow);
	}
	if (has_welcome()) { log_ink(INK_DIM, "Bones at your feet, and a blade glinting among them. Past JAI's statue, something grunts."); }
}

// You as you came down the stairs: a Player value, so the copy holds still while you change, and
// the stairs down can say what this floor gave and took.
Player FLOOR_START;
int FLOOR_START_TURN;

function floor_summary() -> void {
	Player now = G.player;
	string levels = now.level > FLOOR_START.level ? `, level ${FLOOR_START.level} to ${now.level}` : "";
	log_ink(INK_DIM, `Floor ${G.floor} behind you after ${G.turn - FLOOR_START_TURN} turns: ${now.kills - FLOOR_START.kills} kills, +${now.gold - FLOOR_START.gold} gold${levels}.`);
}

function start_new_game(Species species, Profession calling, string hero_name) -> void {
	NOTICED = {};
	G.player = Player(species, calling);
	if (hero_name != "") { G.player.name = hero_name; }
	G.player.god = HOST_GOD;
	if (G.player.god) {
		G.player.atk = 40;
		G.player.max_hp = 400;
		G.player.hp = 400;
	}
	assign_looks();
	Item ration = make_potion();
	ration.effect = "mend";
	ration.name = POTIONS["mend"].name;
	G.player.inventory.push(ration);
	log_msg(`${G.player.name} the ${species.name} ${calling.name} steps into GRUBWELL.`);
	log_ink(INK_DIM, "w walks, a/d turn, m maps, esc for help.");
	enter_floor();
}

// --------------------------------------------------------------- moving ---
// down the facing: the first live monster within max_range (walls block, mimics stay hidden)
function facing_target_index(int max_range) -> int {
	int cx = G.player.x;
	int cy = G.player.y;
	for (int step = 0; step < max_range; ++step) {
		cx += FACE_DX[G.facing];
		cy += FACE_DY[G.facing];
		if (tile_at(cx, cy) == TILE_WALL) { return -1; }
		int ti = monster_index_at(cx, cy);
		if (ti >= 0 && !G.monsters[ti].disguised) { return ti; }
	}
	return -1;
}

// a step forward (dir_sign 1) or back (-1); walking into a monster is the attack
function player_forward(int dir_sign) -> bool {
	int nx = G.player.x + FACE_DX[G.facing] * dir_sign;
	int ny = G.player.y + FACE_DY[G.facing] * dir_sign;
	int ti = monster_index_at(nx, ny);
	if (ti >= 0) {
		if (dir_sign < 0) {
			log_msg("Something is standing there. You feel its breath.");
			return false;
		}
		if (G.monsters[ti].disguised) {
			G.monsters[ti].disguised = false;
			log_ink(INK_BAD, "That loot has TEETH. Grinnest!");
		}
		attack(G.player, G.monsters[ti], player_attack_verb());
		return true;
	}
	return walk_to(nx, ny);
}

// q and e sidestep; shoulders are not weapons
function player_strafe(int side) -> bool {
	int f = (G.facing + (side > 0 ? 1 : 3)) % 4;
	int nx = G.player.x + FACE_DX[f];
	int ny = G.player.y + FACE_DY[f];
	if (monster_index_at(nx, ny) >= 0) {
		log_msg("You sidle into something with opinions.");
		return false;
	}
	return walk_to(nx, ny);
}

function walk_to(int nx, int ny) -> bool {
	if (!is_walkable(nx, ny)) {
		log_ink(INK_FAINT, "Stone. You bump the wall.");
		return false;
	}
	int from_x = G.player.x;
	int from_y = G.player.y;
	G.player.x = nx;
	G.player.y = ny;
	animate_step(from_x, from_y);
	on_step_tile();
	return true;
}

function player_turn(int rot) -> bool {
	int old_facing = G.facing;
	G.facing = (G.facing + rot + 4) % 4;
	animate_turn(old_facing, rot);
	return true;
}

// on the overhead map the keys walk by compass, and the facing follows the step
function player_move_map(Step d) -> bool {
	if (iabs(d.dx) + iabs(d.dy) != 1) {
		log_msg("Your facing-locked feet refuse the diagonal.");
		return false;
	}
	G.facing = facing_for(d.dx, d.dy);
	return player_forward(1);
}

function on_step_tile() -> void {
	int t = tile_at(G.player.x, G.player.y);
	if (t == TILE_STAIRS) {
		log_ink(INK_INFO, "Stairs spiral down into the dark. Press > to descend.");
	} else if (t == TILE_STATUE) {
		log_ink(INK_BLESS, pick(RNG, STATUE_JAI));
		if (!G.statue_used) {
			G.statue_used = true;
			add_effect(G.player, "blessed", 40, 10);
			log_ink(INK_BLESS, "JAI's grin warms you: +10 evasion for a while.");
		}
	} else if (t == TILE_MURAL) {
		log_ink(INK_MAGIC, pick(RNG, MURAL_DEVACOR));
		if (!G.mural_used) {
			G.mural_used = true;
			int healed = restore_stat(G.player.hp, G.player.total_max_hp(), 999);
			if (healed > 0) {
				log_ink(INK_GOOD, `Painted warmth closes your wounds (+${healed} HP).`);
				fx_screen(`+${healed}`, INK_GOOD);
			}
			else { log_ink(INK_DIM, "The colors hum, content that you are whole."); }
		}
	}
	int gi = ground_index_at(G.player.x, G.player.y);
	if (gi >= 0 && G.ground[gi].name == LASTLIGHT_NAME) { log_ink(INK_DIM, "The bones of the last one down. Their blade still holds a little light."); }
	if (gi >= 0) { log_msg(`You see ${paint(G.ground[gi].ink, item_label(G.ground[gi]))} at your feet. (g to grab)`); }
}

function pickup_here() -> bool {
	int gi = ground_index_at(G.player.x, G.player.y);
	if (gi < 0) {
		log_msg("Nothing here but dungeon.");
		return false;
	}
	if (!give_item(G.player, G.ground[gi])) { return false; }
	G.ground.remove(gi);
	return true;
}

function quaff_first_potion() -> bool {
	for (int i = 0; i < G.player.inventory.size(); ++i) {
		if (G.player.inventory[i].kind == "potion") {
			drink_potion(G.player, i);
			return true;
		}
	}
	log_msg("No potions in your pack.");
	return false;
}

function try_descend() -> bool {
	if (tile_at(G.player.x, G.player.y) != TILE_STAIRS && !G.player.god) {
		log_msg("You need to stand on the stairs to descend.");
		return false;
	}
	if (G.floor >= 10) {
		log_msg("There is nothing below floor 10 but the story's end.");
		return false;
	}
	floor_summary();
	G.floor += 1;
	enter_floor();
	return true;
}

// true when the key spent a turn
function handle_key(string key) -> bool {
	if (G.show_map && MAP_KEYS.has(key)) { return player_move_map(MAP_KEYS[key]); }
	switch (key) {
		case "w": return player_forward(1);
		case "up": return player_forward(1);
		case "s": return player_forward(-1);
		case "down": return player_forward(-1);
		case "a": return player_turn(-1);
		case "left": return player_turn(-1);
		case "d": return player_turn(1);
		case "right": return player_turn(1);
		case "q": return player_strafe(-1);
		case "e": return player_strafe(1);
		case ".": return true;
		case "g": return pickup_here();
		case "i": return inventory_screen();
		case "p": return quaff_first_potion();
		case "z": return use_skill();
		case "E": return smart_equip(G.player);
		case ">": return try_descend();
		case "m": { G.show_map = !G.show_map; return false; }
		case "M": { G.show_map = !G.show_map; return false; }
		case "v": { cycle_layout(); return false; }
		case "c": { character_screen(); return false; }
		case "?": { help_screen(); return false; }
		case "esc": {
			if (G.show_map) {
				G.show_map = false;
				return false;
			}
			return handle_key(pause_screen());
		}
		case "S": {
			save_game();
			G.over = true;
			G.saved = true;
			G.quit = true;
			return false;
		}
		case "Q": {
			if (confirm_quit()) {
				G.over = true;
				G.quit = true;
			}
			return false;
		}
		case "R": {
			live_reload("R");
			return false;
		}
	}
	return false;
}

// ---------------------------------------------------------------- turns ---
function monsters_act() -> void {
	for (auto& m : G.monsters) {
		if (G.over) { return; }
		if (!m.alive) { continue; }
		if (!m.aggro && cheb(m.x, m.y, G.player.x, G.player.y) > 14) { continue; }
		monster_act(m);
		notice_player(m);
	}
	array<Monster> living;
	for (auto m : G.monsters) {
		if (m.alive) { living.push(m); }
	}
	G.monsters = living;
}

function world_tick() -> void {
	tick_effects(G.player);
	if (G.over) { return; }
	for (auto& m : G.monsters) {
		if (m.alive) { tick_effects(m); }
	}
	if (G.turn % 4 == 0) { restore_stat(G.player.focus, G.player.total_max_focus(), 1); }
	if (G.turn % 5 == 0 && G.player.hp < G.player.total_max_hp() && nearest_visible_monster_index(99) < 0) {
		heal_entity(G.player, 1);   // catching your breath, out of sight of trouble
	}
}

// the smoke run's fingerprint: everything the rules decide, folded once per turn
function accumulate_hash() -> void {
	int h = G.hash;
	h = mix32(h, G.turn);
	h = mix32(h, G.floor);
	h = mix32(h, G.facing * 7 + 3);
	h = mix32(h, G.player.x * 256 + G.player.y);
	h = mix32(h, G.player.hp * 1024 + G.player.focus);
	h = mix32(h, G.player.xp * 64 + G.player.level);
	h = mix32(h, G.player.gold);
	h = mix32(h, G.player.inventory.size() * 16 + G.player.effects.size());
	int live = 0;
	int acc = 0;
	for (auto m : G.monsters) {
		if (!m.alive) { continue; }
		live += 1;
		acc = (acc + m.x * 977 + m.y * 331 + m.hp * 7) & 0xFFFFFFFF;
	}
	h = mix32(h, live);
	h = mix32(h, acc);
	h = mix32(h, G.ground.size());
	h = mix32(h, RNG.state());
	G.hash = h;
}

// a level's boon: the pilot draws one from the Rng, a player picks on the level-up screen
function choose_boon() -> void {
	while (G.level_pending) {
		string key = G.autopilot ? to_string(1 + RNG.next(4)) : level_up_screen();
		switch (key) {
			case "1": G.player.max_hp += 4; heal_entity(G.player, 4); G.level_pending = false; break;
			case "2": G.player.atk += 1; G.level_pending = false; break;
			case "3": G.player.def += 1; G.level_pending = false; break;
			case "4": G.player.max_focus += 3; G.player.focus += 3; G.level_pending = false; break;
			case "Q": G.over = true; G.quit = true; G.death_cause = "quit"; return;
		}
	}
}

function game_loop() -> void {
	while (!G.over) {
		compute_fov();
		if (!G.smoke) { render_game(); }
		else if (VIEW_PROBE != null) { VIEW_PROBE(); }
		if (G.level_pending) {
			choose_boon();
			if (G.over) { break; }
			if (!G.smoke) { render_game(); }
		}
		string key = next_key();
		bool acted = handle_key(key);
		if (G.over) { break; }
		if (acted) {
			G.last_dmg = 0;
			G.last_dmg_src = "";
			monsters_act();
			if (!G.over) { world_tick(); }
			flush_spawns();
			G.turn += 1;
			if (G.smoke) { accumulate_hash(); }
			fx_prune();
			fx_animate();
			if (HOST_RELOAD_EVERY > 0 && G.turn % HOST_RELOAD_EVERY == 0) { pilot_reload(); }
			if (G.smoke && G.turn >= HOST_TURNS) {
				G.over = true;
				G.quit = true;
			}
		}
		if (G.boss_slain && !G.victory) {
			G.victory = true;
			G.over = true;
		}
	}

	if (G.victory) {
		compute_fov();
		victory_screen();
		delete_file(SAVE_PATH);
	} else if (!G.quit) {
		compute_fov();
		death_screen();
		delete_file(SAVE_PATH);
	} else if (G.saved) {
		host_log("Saved. Rest well; the Well keeps your place.");
	}
}

// Interactive play waits for a key a quarter second at a time, and between looks reloads the
// scripts when one was saved: edit a file mid-run and the Well changes under you.
function next_key() -> string {
	if (G.autopilot) { return autopilot_key(); }
	if (!HOST_INTERACTIVE) { return read_key(); }
	while (true) {
		string key = poll_key(0.25);
		if (key != "") { return key; }
		if (watch_scripts()) { render_game(); }
	}
	return "";
}

// ------------------------------------------------------------ autopilot ---
// A breadth-first pilot in a body that only steps the way it faces: every wish becomes turn keys
// until the nose lines up.
function turn_key_for(int target_facing) -> string { return (target_facing - G.facing + 4) % 4 == 3 ? "a" : "d"; }

function key_toward(int target_facing) -> string { return target_facing == G.facing ? "w" : turn_key_for(target_facing); }

function pilot_has_upgrade() -> bool {
	for (auto it : G.player.inventory) {
		if (!it.is_gear()) { continue; }
		var current = G.player.worn(it.kind);
		if (current == null || gear_score(it) > gear_score(current)) { return true; }
	}
	return false;
}

function autopilot_key() -> string {
	if (PILOT_CO == null || PILOT_CO.done()) { PILOT_CO = pilot_brain(); }
	var key = PILOT_CO.next();
	return key == null ? "." : key;
}

coroutine string pilot_brain() {
	// the loot it committed to, kept across turns so the goal does not flicker
	int loot_x = -1;
	int loot_y = -1;
	int loot_floor = -1;
	while (true) {
		if (G.player.hp * 3 < G.player.total_max_hp()) {
			bool has_potion = false;
			for (auto it : G.player.inventory) {
				if (it.kind == "potion") { has_potion = true; }
			}
			if (has_potion) { yield "p"; continue; }
		}
		if (ground_index_at(G.player.x, G.player.y) >= 0 && G.player.inventory.size() < PACK_SIZE) { yield "g"; continue; }
		if (tile_at(G.player.x, G.player.y) == TILE_STAIRS) { yield ">"; continue; }
		if (pilot_has_upgrade()) { yield "E"; continue; }
		if (G.player.focus >= G.player.skill_cost && RNG.chance(25)) {
			if (facing_target_index(G.player.skill == "bulwark" ? 1 : 5) >= 0) { yield "z"; continue; }
		}
		// a foe at a cardinal neighbour: face it, then walk into it
		string fight_key = "";
		for (auto m : G.monsters) {
			if (!m.alive) { continue; }
			int mdx = m.x - G.player.x;
			int mdy = m.y - G.player.y;
			if (iabs(mdx) + iabs(mdy) != 1) { continue; }
			fight_key = key_toward(facing_for(mdx, mdy));
			break;
		}
		if (fight_key != "") { yield fight_key; continue; }
		// the committed loot first, then the stairs (the boss on floor 10)
		if (loot_floor != G.floor || ground_index_at(loot_x, loot_y) < 0) { loot_x = -1; }
		if (loot_x < 0 && G.player.inventory.size() < 20) {
			int best_d = 14;
			for (auto it : G.ground) {
				int d = cheb(it.x, it.y, G.player.x, G.player.y);
				if (d < best_d) {
					best_d = d;
					loot_x = it.x;
					loot_y = it.y;
					loot_floor = G.floor;
				}
			}
		}
		int gx = loot_x >= 0 ? loot_x : G.stairs_x;
		int gy = loot_x >= 0 ? loot_y : G.stairs_y;
		if (G.floor >= 10) {
			gx = -1;
			for (auto m : G.monsters) {
				if (m.alive && m.behavior == "boss") {
					gx = m.x;
					gy = m.y;
				}
			}
			if (gx < 0) { yield "."; continue; }
		}
		var step = bfs_step(G.player.x, G.player.y, gx, gy);
		if (step != null) { yield key_toward(facing_for(step.dx, step.dy)); continue; }
		loot_x = -1;   // no path: drop the goal and shuffle
		yield key_toward(RNG.next(4));
	}
}

// ---------------------------------------------------------------- saves ---
// The whole Game goes through the archive: to_json writes every field and from_json rebuilds the
// classes. A live coroutine is not saved state, so the boss restarts his pattern on load.
int SAVE_VERSION = 2;

class SaveFile { int version; int seed; int rng; Game game; }

function save_game() -> void {
	G.boss_co = null;
	write_file(SAVE_PATH, to_json(SaveFile{SAVE_VERSION, HOST_SEED, RNG.state(), G}));
}

function load_game() -> bool {
	if (!file_exists(SAVE_PATH)) { return false; }
	SaveFile save;
	try {
		save = from_json(read_file(SAVE_PATH));
	} catch (e) {
		return false;
	}
	if (save.version != SAVE_VERSION) { return false; }
	RNG.set_state(save.rng);
	G = save.game;
	MONSTER_BRAINS = {};
	G.over = false;
	G.quit = false;
	G.saved = false;
	G.smoke = HOST_SMOKE;
	G.autopilot = HOST_SMOKE;
	view_cache_invalidate();
	FLOOR_START = G.player;
	FLOOR_START_TURN = G.turn;
	log_ink(INK_INFO, `Welcome back to floor ${G.floor}, ${G.player.name}.`);
	return true;
}

// -------------------------------------------------------------- summary ---
function print_run_summary(string label) -> void {
	Player p = G.player;
	host_log("");
	host_log(`=== ${label} ===`);
	host_log(`backend: ${HOST_BACKEND}  seed: ${HOST_SEED}`);
	host_log(`turns: ${G.turn}  floor: ${G.floor}  level: ${p.level}  facing: ${FACE_NAME[G.facing]}`);
	host_log(`hp: ${p.hp}/${p.total_max_hp()}  kills: ${p.kills}  gold: ${p.gold}  items: ${p.inventory.size()}`);
	host_log(`weapon: ${p.weapon == null ? "-" : p.weapon.name}  armor: ${p.armor == null ? "-" : p.armor.name}`);
	host_log(`victory: ${G.victory}  dead: ${!p.alive}  cause: ${G.death_cause}`);
	if (RELOAD_COUNT + RELOAD_REFUSED > 0) { host_log(`hot reloads: ${RELOAD_COUNT} kept the run, ${RELOAD_REFUSED} refused`); }
	host_log(`STATE_HASH: ${G.hash}`);
	for (int i = imax(0, G.msgs.size() - 6); i < G.msgs.size(); ++i) { host_log(`  log| ${plain_text(G.msgs[i])}`); }
}
