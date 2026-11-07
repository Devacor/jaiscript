// Monster minds. Each behavior is one switch case over state the monster carries in its fields;
// the boss alone runs a coroutine, Game.boss_brain, whose handle lives in G.boss_co.

function sees_player(var& m) -> bool {
	if (cheb(m.x, m.y, G.player.x, G.player.y) > 9) { return false; }
	return line_clear(m.x, m.y, G.player.x, G.player.y);
}

function monster_step(var& m, int dx, int dy) -> bool {
	int nx = m.x + dx;
	int ny = m.y + dy;
	if (!is_walkable(nx, ny) || (nx == G.player.x && ny == G.player.y) || monster_index_at(nx, ny) >= 0) { return false; }
	m.x = nx;
	m.y = ny;
	return true;
}

// greedy, sliding around corners: good enough to feel hungry
function step_toward(var& m, int tx, int ty) -> void {
	int dx = sign_of(tx - m.x);
	int dy = sign_of(ty - m.y);
	if (monster_step(m, dx, dy)) { return; }
	if (dx != 0 && monster_step(m, dx, 0)) { return; }
	if (dy != 0 && monster_step(m, 0, dy)) { return; }
	if (dx == 0) {
		if (monster_step(m, 1, dy)) { return; }
		if (monster_step(m, -1, dy)) { return; }
	}
	if (dy == 0) {
		if (monster_step(m, dx, 1)) { return; }
		monster_step(m, dx, -1);
	}
}

function step_away(var& m, int tx, int ty) -> void { step_toward(m, m.x + (m.x - tx) * 2, m.y + (m.y - ty) * 2); }

function wander(var& m) -> void {
	Step d = EIGHT_DIRS[RNG.next(8)];
	monster_step(m, d.dx, d.dy);
}

function adjacent_to_player(var& m) -> bool { return cheb(m.x, m.y, G.player.x, G.player.y) <= 1; }

// one action for one monster; m is a reference into G.monsters
function monster_act(var& m) -> void {
	switch (m.behavior) {
		case "chase": {
			if (adjacent_to_player(m)) { attack(m, G.player, "claws"); }
			else if (m.aggro || sees_player(m)) {
				m.aggro = true;
				step_toward(m, G.player.x, G.player.y);
			}
			break;
		}
		case "ranged": {
			int dist = cheb(m.x, m.y, G.player.x, G.player.y);
			bool in_sight = sees_player(m);
			if (in_sight) { m.aggro = true; }
			if (adjacent_to_player(m)) {
				if (RNG.chance(50)) { attack(m, G.player, "swats"); }
				else { step_away(m, G.player.x, G.player.y); }
			} else if (in_sight && dist <= m.range) {
				attack(m, G.player, "pelts");
			} else if (m.aggro) {
				step_toward(m, G.player.x, G.player.y);
			}
			break;
		}
		case "pack": {
			if (adjacent_to_player(m)) {
				attack(m, G.player, "bites");
			} else if (sees_player(m)) {
				// braver when a packmate also has eyes on the prize
				int packmates = 0;
				for (auto& other : G.monsters) {
					if (other.alive && other.behavior == "pack" && other.id != m.id && sees_player(other)) { packmates += 1; }
				}
				if (packmates >= 1 || m.aggro || m.hp < m.max_hp) {
					m.aggro = true;
					step_toward(m, G.player.x, G.player.y);
				} else {
					wander(m);
				}
			}
			break;
		}
		case "erratic": {
			if (adjacent_to_player(m) && RNG.chance(70)) { attack(m, G.player, "rakes"); }
			else if (RNG.chance(40)) { wander(m); }
			else if (sees_player(m)) { step_toward(m, G.player.x, G.player.y); }
			break;
		}
		case "sluggish": {
			m.move_clock += 1;
			if (adjacent_to_player(m)) { attack(m, G.player, "engulfs"); }
			else if (m.move_clock % 2 == 0 && sees_player(m)) { step_toward(m, G.player.x, G.player.y); }
			break;
		}
		case "mimic": {
			if (m.disguised) {
				if (adjacent_to_player(m)) {
					m.disguised = false;
					log_ink(INK_BAD, "The loot grows teeth. It was a Grinnest all along!");
					attack(m, G.player, "chomps");
				}
			} else if (adjacent_to_player(m)) {
				attack(m, G.player, "chomps");
			} else if (sees_player(m)) {
				step_toward(m, G.player.x, G.player.y);
			}
			break;
		}
		case "boss": {
			if (G.boss_co == null || G.boss_co.done()) { G.boss_co = G.boss_brain(m.id); }
			G.boss_co.next();
			break;
		}
	}
}

function monster_index_by_id(int id) -> int {
	for (int i = 0; i < G.monsters.size(); ++i) {
		if (G.monsters[i].alive && G.monsters[i].id == id) { return i; }
	}
	return -1;
}

function monster_index_at(int x, int y) -> int {
	for (int i = 0; i < G.monsters.size(); ++i) {
		if (G.monsters[i].alive && G.monsters[i].x == x && G.monsters[i].y == y) { return i; }
	}
	return -1;
}
