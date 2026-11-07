// Gator Quest - an autopilot that plays a slot through the same intent path a
// keyboard or a network client uses: shopping, quests, hunting and looting,
// fishing/mining/chopping, smelting and cooking, the sewers and the Bayou.
// Deterministic: it reads the sim state and rolls its dice from hashes, never
// from the sim RNG, so bots do not perturb the world they test.

import "gq_sim.jai";

int BP_SHOP = 0;
int BP_QUEST = 1;
int BP_HUNT = 2;
int BP_FISH = 3;
int BP_MINE = 4;
int BP_CHOP = 5;
int BP_SMELT = 6;
int BP_COOK = 7;
int BP_SEWER = 8;
int BP_BAYOU = 9;
int BP_TURNIN = 10;
int BP_FORGE = 11;
array<string> BP_NAME = ["shop", "quest", "hunt", "fish", "mine", "chop", "smelt", "cook", "sewer", "bayou", "turnin", "forge"];
array<int> BOT_PLAN = [0, 1, 2, 3, 4, 5, 6, 11, 7, 10, 8, 10, 2, 9, 3, 10, 2];

// ------------------------------------------------------------------- BFS ---
array<int> BFS_SEEN;
array<int> BFS_FIRST;
array<int> BFS_Q;
int BFS_STAMP;
int BFS_LAST_LEN;

bool bot_walkable(int w, int x, int y, int tx, int ty) {
	if (!in_world(w, x, y)) { return false; }
	int t = tile_now(w, x, y);
	int k = T_WALK[t];
	if (k == 0 || k == 3 || k == 5) { return x == tx && y == ty && k != 0; }
	int st = TILES_DEF[t].station;
	if ((st == 7 || st == 8) && (x != tx || y != ty)) { return false; }
	// shopkeepers never step aside: route around them (and around everyone once stuck)
	int o = OCC[W_OFF[w] + y * W_W[w] + x] - 1;
	if (o >= 0 && (MOB_ROLE[o] >= 0 || BFS_AVOID_MOBS) && (x != tx || y != ty)) { return false; }
	return true;
}

bool BFS_AVOID_MOBS;

// a route from the mobile toward (tx, ty) as a list of directions; `near` = stop when adjacent.
// empty = already there; [-1] = no route
array<int> bfs_path(int m, int tx, int ty, bool near, int maxn) {
	int w = MOB_WORLD[m];
	int ww = W_W[w];
	int n = ww * W_H[w];
	while (BFS_SEEN.size() < n) { BFS_SEEN.push(0); BFS_FIRST.push(0); }
	BFS_STAMP = BFS_STAMP + 1;
	int sx = MOB_X[m];
	int sy = MOB_Y[m];
	array<int> out;
	if ((near && cheb(sx, sy, tx, ty) <= 1) || (sx == tx && sy == ty)) { return out; }
	BFS_Q = [];
	BFS_Q.push(sy * ww + sx);
	BFS_SEEN[sy * ww + sx] = BFS_STAMP;
	BFS_FIRST[sy * ww + sx] = -1;
	int head = 0;
	int found = -1;
	while (head < BFS_Q.size() && head < maxn && found < 0) {
		int cur = BFS_Q[head];
		head = head + 1;
		int cx = cur % ww;
		int cy = cur / ww;
		for (int d = 0; d < 8; ++d) {
			int nx = cx + DX[d];
			int ny = cy + DY[d];
			if (nx < 0 || ny < 0 || nx >= ww || ny >= W_H[w]) { continue; }
			int ni = ny * ww + nx;
			if (BFS_SEEN[ni] == BFS_STAMP) { continue; }
			if (!bot_walkable(w, nx, ny, tx, ty)) { continue; }
			if (DX[d] != 0 && DY[d] != 0 && walk_at(w, cx + DX[d], cy) == 0 && walk_at(w, cx, cy + DY[d]) == 0) { continue; }
			BFS_SEEN[ni] = BFS_STAMP;
			BFS_FIRST[ni] = cur * 8 + d;           // parent tile and the step that reached us
			if ((nx == tx && ny == ty) || (near && cheb(nx, ny, tx, ty) <= 1)) { found = ni; break; }
			BFS_Q.push(ni);
		}
	}
	if (found < 0) { out.push(-1); return out; }
	int at = found;
	while (at != sy * ww + sx) {
		int link = BFS_FIRST[at];
		out.push(link % 8);
		at = link / 8;
	}
	out.reverse();
	return out;
}

// ------------------------------------------------------------ bot helpers ---
int bot_roll(Bot& b, int n) { return hash3(TICK, b.slot * 97 + b.step, 12345) % imax(1, n); }

int find_role_npc(int w, int role) {
	for (int i = 0; i < MOB_KIND.size(); ++i) {
		if (MOB_KIND[i] >= 0 && MOB_ROLE[i] == role && MOB_WORLD[i] == w) { return i; }
	}
	return -1;
}

// nearest tile in the mobile's world with this resource class (or station); -1s when none
array<int> find_tile_near(int m, int res, int station, int maxr) {
	int w = MOB_WORLD[m];
	for (int r = 1; r <= maxr; ++r) {
		for (int dy = -r; dy <= r; ++dy) {
			for (int dx = -r; dx <= r; ++dx) {
				if (iabs(dx) != r && iabs(dy) != r) { continue; }
				int x = MOB_X[m] + dx;
				int y = MOB_Y[m] + dy;
				if (!in_world(w, x, y)) { continue; }
				TileDef td = TILES_DEF[tile_now(w, x, y)];
				if ((res > 0 && td.res == res) || (station > 0 && td.station == station)) { return [x, y]; }
			}
		}
	}
	return [-1, -1];
}

// walk toward a spot along a cached route; true once there (or adjacent when near)
bool bot_go(Bot& b, int m, int x, int y, bool near) {
	if ((near && cheb(MOB_X[m], MOB_Y[m], x, y) <= 1) || (MOB_X[m] == x && MOB_Y[m] == y)) {
		intent_move(b.slot, -1);
		return true;
	}
	if (TICK + 1 < MOB_NEXT[m]) { return false; }
	// keep following the route while we are where it expects us to be
	bool replan = b.path_w != MOB_WORLD[m] || b.path_tx != x || b.path_ty != y || b.path_i >= b.path.size();
	if (!replan && (MOB_X[m] != b.last_x || MOB_Y[m] != b.last_y)) { replan = true; }
	if (!replan && b.stuck > 3) { replan = true; }
	if (b.stuck > 8) {
		// still stuck after routing around everyone: shuffle sideways and try again
		intent_move(b.slot, bot_roll(b, 8));
		b.stuck = 0;
		b.path = [];
		return false;
	}
	if (replan) {
		BFS_AVOID_MOBS = b.stuck > 3;
		b.path = bfs_path(m, x, y, near, 6000);
		BFS_AVOID_MOBS = false;
		b.path_i = 0;
		b.path_w = MOB_WORLD[m];
		b.path_tx = x;
		b.path_ty = y;
		if (b.path.size() == 0) { intent_move(b.slot, -1); return true; }
		if (b.path[0] < 0) {
			intent_move(b.slot, bot_roll(b, 8));
			b.path = [];
			b.stuck = b.stuck + 1;
			return false;
		}
	}
	int d = b.path[b.path_i];
	int nx = MOB_X[m] + DX[d];
	int ny = MOB_Y[m] + DY[d];
	intent_move(b.slot, d);
	// where we will be after this step; if the step fails we notice next time
	if (mob_can_enter(m, nx, ny)) {
		b.last_x = nx;
		b.last_y = ny;
		b.path_i = b.path_i + 1;
		b.stuck = 0;
	} else {
		b.last_x = MOB_X[m];
		b.last_y = MOB_Y[m];
		b.stuck = b.stuck + 1;
	}
	return false;
}

int bot_nearest_hostile(int m, int r, bool bosses) {
	int w = MOB_WORLD[m];
	int best = -1;
	int bestd = r + 1;
	for (int dy = -r; dy <= r; ++dy) {
		for (int dx = -r; dx <= r; ++dx) {
			int o = mob_at(w, MOB_X[m] + dx, MOB_Y[m] + dy);
			if (o < 0 || !hostile(m, o) || !mob_alive(o)) { continue; }
			if (!bosses && (MOB_FLAGS[o] & MF_BOSS) != 0) { continue; }
			if (mob_fac(o) == Fac.animal && bot_roll_static(o) != 0) { continue; }
			int d = cheb(0, 0, dx, dy);
			if (d < bestd) { bestd = d; best = o; }
		}
	}
	return best;
}

int bot_roll_static(int o) { return MOB_SER[o] % 3; }

int nearest_ground_item(int m, int slot, int r) {
	int w = MOB_WORLD[m];
	int best = -1;
	int bestd = r + 1;
	for (int i = 0; i < GI_ITEM.size(); ++i) {
		if (GI_ITEM[i].base < 0 || GI_W[i] != w) { continue; }
		if (GI_OWNER[i] >= 0 && GI_OWNER[i] != slot) { continue; }
		int d = cheb(GI_X[i], GI_Y[i], MOB_X[m], MOB_Y[m]);
		if (d < bestd && is_land(w, GI_X[i], GI_Y[i]) || d < bestd && walk_at(w, GI_X[i], GI_Y[i]) == 2) { bestd = d; best = i; }
	}
	return best;
}

int item_score(Item it) {
	if (it.base < 0) { return -1; }
	ItemDef d = ITEMS[it.base];
	int s = d.armor * 10 + (d.lo + d.hi) * 5 + it.bonus * 12 + it.rarity * 15;
	for (int c = 0; c < AF_NAME.size(); ++c) { s = s + item_stat(it, c); }
	return s;
}

// equip anything strictly better (bots are shameless about it)
bool bot_gear_up(Bot& b) {
	Character& c = CHARS[b.slot];
	for (int i = 0; i < c.pack.size(); ++i) {
		Item it = c.pack[i];
		if (!item_equippable(it)) { continue; }
		int s = ITEMS[it.base].slot;
		if (ITEMS[it.base].kind == Kind.weapon && ITEMS[it.base].range > 1 && !pack_has(b.slot, I_ARROW, 5) && ITEMS[it.base].skill == Sk.archery) { continue; }
		if (item_score(it) > item_score(c.worn[s])) {
			intent_act(b.slot, "U" + to_string(i));
			return true;
		}
	}
	return false;
}

int pack_index_of(int slot, int base) {
	Character& c = CHARS[slot];
	for (int i = 0; i < c.pack.size(); ++i) {
		if (c.pack[i].base == base) { return i; }
	}
	return -1;
}

// reflexes that beat any plan: ghosts go to the healer, hurt gators drink
bool bot_survive(Bot& b, int m) {
	Character& c = CHARS[b.slot];
	if (mob_ghost(m)) {
		intent_act(b.slot, "!");
		int healer = find_role_npc(MOB_WORLD[m], R_HEALER);
		if (healer < 0) { healer = find_role_npc(MOB_WORLD[m], R_ORACLE); }
		if (healer < 0) {
			// no healer here: walk to the nearest gate home
			int g = gate_in(MOB_WORLD[m], 0);
			if (g >= 0) { bot_go(b, m, GATE_X[g], GATE_Y[g], false); }
			return true;
		}
		if (bot_go(b, m, MOB_X[healer], MOB_Y[healer], true)) {
			intent_move(b.slot, dir_of(MOB_X[healer] - MOB_X[m], MOB_Y[healer] - MOB_Y[m]));
		}
		return true;
	}
	if (MOB_HP[m] * 100 < MOB_HPMAX[m] * 40) {
		int p = pack_index_of(b.slot, I_HEAL);
		if (p < 0) { p = pack_index_of(b.slot, I_MINORHEAL); }
		if (p >= 0 && TICK % 5 == 0) { intent_act(b.slot, "U" + to_string(p)); return false; }
		if (c.action == 0 && pack_has(b.slot, I_BANDAGE, 1) && TICK % 7 == 0) { intent_act(b.slot, "N"); }
		if (c.spells[0] == 1 && MOB_MP[m] >= 4 && pack_has(b.slot, I_ONION, 1) && pack_has(b.slot, I_CATTAIL, 1) && TICK % 9 == 0) {
			intent_act(b.slot, "C0");
		}
	}
	return false;
}

void bot_next_phase(Bot& b) {
	b.step = b.step + 1;
	b.phase = BOT_PLAN[b.step % BOT_PLAN.size()];
	b.phase_t = TICK;
	b.done = 0;
	b.goal_w = -1;
	b.kills_at = CHARS[b.slot].kills;
	b.log = b.log + BP_NAME[b.phase] + " ";
}

// fight what is in front of us, pick up what is lying around; true = busy
bool bot_fight_and_loot(Bot& b, int m, int r) {
	int t = bot_nearest_hostile(m, r, false);
	if (t >= 0) {
		intent_act(b.slot, "A" + to_string(MOB_SER[t]));
		int rng = attack_range(m);
		int d = cheb(MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]);
		if (d > rng || (d > 1 && !los(MOB_WORLD[m], MOB_X[m], MOB_Y[m], MOB_X[t], MOB_Y[t]))) { bot_go(b, m, MOB_X[t], MOB_Y[t], true); }
		else if (d <= 1) { intent_move(b.slot, dir_of(MOB_X[t] - MOB_X[m], MOB_Y[t] - MOB_Y[m])); }
		else { intent_move(b.slot, -1); }
		// a spell now and then
		if (CHARS[b.slot].spells[1] == 1 && MOB_MP[m] >= 8 && pack_has(b.slot, I_FIREFLY, 2) && bot_roll(b, 6) == 0) { intent_act(b.slot, "C1"); }
		return true;
	}
	int gi = nearest_ground_item(m, b.slot, 7);
	if (gi >= 0) {
		if (cheb(GI_X[gi], GI_Y[gi], MOB_X[m], MOB_Y[m]) <= 1) {
			intent_move(b.slot, -1);
			intent_act(b.slot, "G");
		} else {
			bot_go(b, m, GI_X[gi], GI_Y[gi], false);
		}
		return true;
	}
	return false;
}

void bot_wander(Bot& b, int m, int cx, int cy, int radius) {
	if (b.goal_w != MOB_WORLD[m] || cheb(MOB_X[m], MOB_Y[m], b.goal_x, b.goal_y) <= 1 || b.stuck > 20) {
		b.goal_w = MOB_WORLD[m];
		array<int> p = find_spot(MOB_WORLD[m], cx + bot_roll(b, radius * 2 + 1) - radius, cy + bot_roll(b, radius * 2 + 1) - radius, 5, LAND_MASK);
		b.goal_x = p[0];
		b.goal_y = p[1];
		b.stuck = 0;
	}
	bot_go(b, m, b.goal_x, b.goal_y, false);
}

// gather at the nearest tile of a resource class: walk up, face it, E, wait
void bot_gather(Bot& b, int m, int res, int times, int tool) {
	Character& c = CHARS[b.slot];
	if (tool >= 0 && !pack_has(b.slot, tool, 1)) { bot_next_phase(b); return; }
	if (b.done >= times || TICK - b.phase_t > 1200) { bot_next_phase(b); return; }
	if (c.action != 0) { intent_move(b.slot, -1); return; }
	array<int> p = find_tile_near(m, res, 0, 30);
	if (p[0] < 0) { bot_wander(b, m, MOB_X[m], MOB_Y[m], 12); return; }
	if (bot_go(b, m, p[0], p[1], true)) {
		MOB_DIR[m] = dir_of(p[0] - MOB_X[m], p[1] - MOB_Y[m]);
		intent_act(b.slot, "E");
		b.done = b.done + 1;
	}
}

void bot_craft_at(Bot& b, int m, int station, string output, int times) {
	if (b.done >= times || TICK - b.phase_t > 900) { bot_next_phase(b); return; }
	Character& c = CHARS[b.slot];
	if (c.action != 0) { intent_move(b.slot, -1); return; }
	int recipe = -1;
	for (int r = 0; r < RECIPES.size(); ++r) {
		if (ITEMS[RECIPES[r].out].name == output) { recipe = r; }
	}
	if (recipe < 0 || !recipe_has_inputs(b.slot, recipe)) { bot_next_phase(b); return; }
	if (MOB_WORLD[m] != 0) { bot_go_world(b, m, 0); return; }
	array<int> p = find_tile_near(m, 0, station, 40);
	if (p[0] < 0) { bot_go(b, m, W_ARRIVE_X[0], W_ARRIVE_Y[0], false); return; }
	if (bot_go(b, m, p[0], p[1], true)) {
		intent_act(b.slot, "K" + to_string(recipe));
		b.done = b.done + 1;
	}
}

bool recipe_has_inputs(int slot, int r) {
	RecipeDef d = RECIPES[r];
	for (int k = 0; k < d.in_item.size(); ++k) {
		if (!pack_has(slot, d.in_item[k], d.in_qty[k])) { return false; }
	}
	return true;
}

// head for another world through the right gate (or the sewer stairs)
void bot_go_world(Bot& b, int m, int dest) {
	int w = MOB_WORLD[m];
	int g = gate_in(w, dest);
	if (g < 0) { g = gate_in(w, 0); }
	if (g < 0) { return; }
	bot_go(b, m, GATE_X[g], GATE_Y[g], false);
}

void bot_talk_role(Bot& b, int m, int role) {
	int npc = find_role_npc(0, role);
	if (npc < 0) { b.done = b.done + 1; return; }
	if (MOB_WORLD[m] != 0) { bot_go_world(b, m, 0); return; }
	if (bot_go(b, m, MOB_X[npc], MOB_Y[npc], true)) {
		intent_act(b.slot, "Q" + to_string(MOB_SER[npc]));
		b.done = b.done + 1;
	}
}

void bot_tick(Bot& b) {
	if (b.slot < 0 || b.slot >= CHARS.size() || !CHARS[b.slot].active) { return; }
	Character& c = CHARS[b.slot];
	int m = c.mob;
	if (!mob_live(m)) { return; }
	if (b.phase_t == 0 && b.step == 0) { b.phase = BOT_PLAN[0]; b.phase_t = TICK; b.log = BP_NAME[b.phase] + " "; }
	intent_move(b.slot, -1);
	if (bot_survive(b, m)) { return; }
	if (TICK % 25 == 0 && bot_gear_up(b)) { return; }
	int phase = b.phase;
	if (phase == BP_SHOP) {
		int npc = find_role_npc(0, R_PROVISIONER);
		if (npc < 0 || MOB_WORLD[m] != 0) { bot_next_phase(b); return; }
		if (bot_go(b, m, MOB_X[npc], MOB_Y[npc], true)) {
			array<int> stock = SHOPS[ROLES[R_PROVISIONER].shop];
			array<int> want = [I_PICKAXE, I_HATCHET, I_HAMMER];
			for (int k = 0; k < want.size(); ++k) {
				int idx = stock.index_of(want[k]);
				if (idx >= 0 && !pack_has(b.slot, want[k], 1)) { intent_act(b.slot, `B${MOB_SER[npc]},${idx}`); }
			}
			bot_next_phase(b);
		}
		return;
	}
	if (phase == BP_QUEST || phase == BP_TURNIN) {
		array<int> roles = [R_MAYOR, R_CAPTAIN, R_PROVISIONER, R_FISHMONGER, R_WEAPONS, R_ORACLE];
		if (b.done >= roles.size() || TICK - b.phase_t > 1500) { bot_next_phase(b); return; }
		bot_talk_role(b, m, roles[b.done]);
		return;
	}
	if (phase == BP_HUNT) {
		if (TICK - b.phase_t > 700) { bot_next_phase(b); return; }
		if (MOB_WORLD[m] != 0) { bot_go_world(b, m, 0); return; }
		if (bot_fight_and_loot(b, m, 9)) { return; }
		bot_wander(b, m, W_ARRIVE_X[0] + 24, W_ARRIVE_Y[0] + 4, 14);
		return;
	}
	if (phase == BP_FISH) { bot_gather(b, m, 4, 6, I_FISHPOLE); return; }
	if (phase == BP_MINE) { bot_gather(b, m, 2, 4, I_PICKAXE); return; }
	if (phase == BP_CHOP) { bot_gather(b, m, 1, 4, I_HATCHET); return; }
	if (phase == BP_SMELT) { bot_craft_at(b, m, 1, "Iron Ingot", 5); return; }
	if (phase == BP_FORGE) { bot_craft_at(b, m, 2, "Short Sword", 1); return; }
	if (phase == BP_COOK) { bot_craft_at(b, m, 3, "Cooked Fish", 4); return; }
	if (phase == BP_SEWER || phase == BP_BAYOU) {
		int dest = phase == BP_SEWER ? W_SEWERS : 1;
		if (b.done == 0) {
			if (MOB_WORLD[m] == dest) { b.done = 1; b.phase_t = TICK; return; }
			if (TICK - b.phase_t > 1500) { bot_next_phase(b); return; }
			bot_go_world(b, m, dest);
			return;
		}
		if (b.done == 1) {
			if (TICK - b.phase_t > 800 || MOB_WORLD[m] != dest) { b.done = 2; return; }
			if (bot_fight_and_loot(b, m, 9)) { return; }
			bot_wander(b, m, W_ARRIVE_X[dest] + 10, W_ARRIVE_Y[dest] + 6, 12);
			return;
		}
		if (MOB_WORLD[m] == 0 || TICK - b.phase_t > 2000) { bot_next_phase(b); return; }
		bot_go_world(b, m, 0);
		return;
	}
	bot_next_phase(b);
}

int bot_add(int slot) {
	Bot b = Bot();
	b.slot = slot;
	BOTS.push(b);
	return BOTS.size() - 1;
}

void bots_tick() {
	for (int i = 0; i < BOTS.size(); ++i) { bot_tick(BOTS[i]); }
}
