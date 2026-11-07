// Gator Quest - terrain access and world generation. Every realm is carved
// from the seed alone (value noise + the sim RNG), so a client regenerates
// the same maps from the WELCOME seed and terrain never rides the wire.

import "gq_data.jai";

// ---------------------------------------------------------------- access ---
bool in_world(int w, int x, int y) { return x >= 0 && y >= 0 && x < W_W[w] && y < W_H[w]; }

int tile_at(int w, int x, int y) {
	if (x < 0 || y < 0 || x >= W_W[w] || y >= W_H[w]) { return w == 0 ? T_DEEP : (w == 6 ? T_SKY : T_CAVEWALL); }
	return TILES[W_OFF[w] + y * W_W[w] + x];
}

// terrain as it stands now: a felled tree shows its stump until it regrows
int tile_now(int w, int x, int y) {
	if (x < 0 || y < 0 || x >= W_W[w] || y >= W_H[w]) { return tile_at(w, x, y); }
	int i = W_OFF[w] + y * W_W[w] + x;
	int t = TILES[i];
	if (REGROW[i] > TICK) {
		int after = TILES_DEF[t].after;
		if (after >= 0) { return after; }
	}
	return t;
}

void set_tile(int w, int x, int y, int t) {
	if (in_world(w, x, y)) { TILES[W_OFF[w] + y * W_W[w] + x] = t; }
}

int walk_at(int w, int x, int y) { return T_WALK[tile_now(w, x, y)]; }
bool is_land(int w, int x, int y) { int k = walk_at(w, x, y); return k == 1 || k == 6 || k == 4; }
bool is_water(int w, int x, int y) { return walk_at(w, x, y) == 2; }

void fill_rect(int w, int x0, int y0, int x1, int y1, int t) {
	for (int y = y0; y <= y1; ++y) {
		for (int x = x0; x <= x1; ++x) { set_tile(w, x, y, t); }
	}
}

void fill_disc(int w, int cx, int cy, int r, int t) {
	for (int y = cy - r; y <= cy + r; ++y) {
		for (int x = cx - r; x <= cx + r; ++x) {
			if (dist2(x, y, cx, cy) <= r * r + r) { set_tile(w, x, y, t); }
		}
	}
}

// nearest tile (spiral out) that satisfies the walk classes in `mask` (bit per class)
array<int> find_spot(int w, int x0, int y0, int maxr, int mask) {
	for (int r = 0; r <= maxr; ++r) {
		for (int dy = -r; dy <= r; ++dy) {
			for (int dx = -r; dx <= r; ++dx) {
				if (iabs(dx) != r && iabs(dy) != r) { continue; }
				int x = x0 + dx;
				int y = y0 + dy;
				if (!in_world(w, x, y)) { continue; }
				if (((1 << walk_at(w, x, y)) & mask) != 0) { return [x, y]; }
			}
		}
	}
	return [x0, y0];
}

int LAND_MASK = 2 + 64 + 16;   // land, slow land, ice

// a road from a to b: greedy steps with a wobble; water becomes bridge, obstacles are cleared
void carve_road(int w, int ax, int ay, int bx, int by, int road) {
	int x = ax;
	int y = ay;
	int guard = 0;
	while ((x != bx || y != by) && guard < 600) {
		guard = guard + 1;
		int dx = isign(bx - x);
		int dy = isign(by - y);
		if (dx != 0 && dy != 0) {
			if (rnd(2) == 0) { dx = 0; } else { dy = 0; }
		}
		x = x + dx;
		y = y + dy;
		int t = tile_at(w, x, y);
		int k = T_WALK[t];
		if (t == T_GATE || t == T_STAIRS_DN || TILES_DEF[t].station != 0 || t == T_DOOR || t == T_WOODFLOOR || t == T_CARPET) { continue; }
		if (t == T_BRICK || t == T_WOODWALL || t == T_COUNTER || t == T_FENCE) { continue; }
		if (k == 2 || t == T_SHALLOW) { set_tile(w, x, y, T_BRIDGE); }
		else { set_tile(w, x, y, road); }
	}
}

// ------------------------------------------------------------- recording ---
void add_gate(int w, int x, int y, int dest) {
	set_tile(w, x, y, T_GATE);
	GATE_W.push(w);
	GATE_X.push(x);
	GATE_Y.push(y);
	GATE_DEST.push(dest);
}

int gate_in(int w, int dest) {
	for (int i = 0; i < GATE_W.size(); ++i) {
		if (GATE_W[i] == w && GATE_DEST[i] == dest) { return i; }
	}
	return -1;
}

void add_town(int w, int x0, int y0, int x1, int y1, string name) {
	TOWN_W.push(w);
	TOWN_X0.push(x0);
	TOWN_Y0.push(y0);
	TOWN_X1.push(x1);
	TOWN_Y1.push(y1);
	TOWN_NAME.push(name);
}

int town_at(int w, int x, int y) {
	for (int i = 0; i < TOWN_W.size(); ++i) {
		if (TOWN_W[i] == w && x >= TOWN_X0[i] && x <= TOWN_X1[i] && y >= TOWN_Y0[i] && y <= TOWN_Y1[i]) { return i; }
	}
	return -1;
}

void add_chest(int w, int x, int y, int tier) {
	set_tile(w, x, y, T_CHEST);
	CHEST_W.push(w);
	CHEST_X.push(x);
	CHEST_Y.push(y);
	CHEST_TIER.push(tier);
	CHEST_T.push(0);
}

void add_spawner(int w, int x, int y, int kind, int maxn, int rad, int delay) {
	SP_W.push(w);
	SP_X.push(x);
	SP_Y.push(y);
	SP_KIND.push(kind);
	SP_MAX.push(maxn);
	SP_RAD.push(rad);
	SP_DELAY.push(delay);
	SP_NEXT.push(0);
	SP_ALIVE.push(0);
}

// NPC placements the populate pass turns into mobiles
array<int> NPC_PLAN_W;
array<int> NPC_PLAN_X;
array<int> NPC_PLAN_Y;
array<int> NPC_PLAN_KIND;
array<int> NPC_PLAN_ROLE;

void plan_npc(int w, int x, int y, int kind, int role) {
	NPC_PLAN_W.push(w);
	NPC_PLAN_X.push(x);
	NPC_PLAN_Y.push(y);
	NPC_PLAN_KIND.push(kind);
	NPC_PLAN_ROLE.push(role);
}

// a walled building with a door on the south wall; returns [doorx, doory]
array<int> building(int w, int x0, int y0, int bw, int bh, int wall, int floor) {
	fill_rect(w, x0, y0, x0 + bw - 1, y0 + bh - 1, wall);
	fill_rect(w, x0 + 1, y0 + 1, x0 + bw - 2, y0 + bh - 2, floor);
	int dx = x0 + bw / 2;
	int dy = y0 + bh - 1;
	set_tile(w, dx, dy, T_DOOR);
	set_tile(w, dx, dy + 1, T_ROAD);
	return [dx, dy];
}

// a shop: building + counter + keeper behind it (+ an optional station)
void shop(int w, int x0, int y0, int role, int kind, int station) {
	array<int> door = building(w, x0, y0, 7, 5, T_WOODWALL, T_WOODFLOOR);
	fill_rect(w, x0 + 1, y0 + 2, x0 + 5, y0 + 2, T_COUNTER);
	set_tile(w, x0 + 3, y0 + 2, T_WOODFLOOR);
	set_tile(w, x0 + 1, y0 + 1, T_CARPET);
	if (station >= 0) { set_tile(w, x0 + 5, y0 + 1, station); }
	plan_npc(w, x0 + 3, y0 + 1, kind, role);
}

// ------------------------------------------------------------ generation ---
void world_alloc() {
	W_OFF = [];
	W_W = [];
	W_H = [];
	TILES = [];
	TVAR = [];
	REGROW = [];
	OCC = [];
	for (int w = 0; w < WORLD_COUNT; ++w) {
		W_OFF.push(TILES.size());
		W_W.push(WORLDS[w].w);
		W_H.push(WORLDS[w].h);
		int n = WORLDS[w].w * WORLDS[w].h;
		for (int i = 0; i < n; ++i) {
			TILES.push(0);
			TVAR.push(hash3(i, w, SEED) & 255);
			REGROW.push(0);
			OCC.push(0);
		}
		W_WEATHER.push(0);
		W_WEATHER_T.push(0);
		W_ARRIVE_X.push(1);
		W_ARRIVE_Y.push(1);
		W_BOSS_T.push(0);
	}
}

void gen_overworld() {
	int w = 0;
	int ww = W_W[w];
	int hh = W_H[w];
	int salt = SEED * 7 + 11;
	for (int y = 0; y < hh; ++y) {
		for (int x = 0; x < ww; ++x) {
			int e = fractal(x, y, salt);
			int m = fractal(x, y, salt + 1000);
			int cx = (x - ww / 2) * 1000 / (ww / 2);
			int cy = (y - hh / 2) * 1000 / (hh / 2);
			int falloff = (cx * cx + cy * cy) / 2600;
			e = e + 180 - falloff;
			int h = hash3(x, y, salt + 5) & 1023;
			int t = T_GRASS;
			if (e < 330) { t = T_DEEP; }
			else if (e < 380) { t = T_SHALLOW; }
			else if (e < 410) { t = T_SAND; }
			else if (e > 830) { t = T_MOUNTAIN; }
			else if (e > 760) { t = h < 90 ? T_BOULDER : (h < 120 ? T_ORE : (h < 400 ? T_DIRT : T_GRASS)); }
			else if (m > 640) {
				if (h < 260) { t = T_SWAMP; }
				else if (h < 380) { t = T_CYPRESS; }
				else if (h < 470) { t = T_REEDS; }
				else if (h < 560) { t = T_MUD; }
				else if (h < 600) { t = T_LILY; }
				else { t = T_BOG; }
			}
			else if (m > 540) { t = h < 520 ? T_OAK : (h < 600 ? T_BUSH : (h < 640 ? T_PINE : T_GRASS)); }
			else if (m < 280) { t = h < 300 ? T_DIRT : (h < 500 ? T_TALLGRASS : T_GRASS); }
			else {
				if (h < 50) { t = T_OAK; }
				else if (h < 90) { t = T_BUSH; }
				else if (h < 170) { t = T_TALLGRASS; }
				else if (h < 210) { t = T_FLOWERS; }
				else if (h < 214) { t = T_BOULDER; }
			}
			TILES[W_OFF[w] + y * ww + x] = t;
		}
	}
	// rivers: walk downhill from high ground to the sea
	for (int r = 0; r < 4; ++r) {
		int x = rnd_range(20, ww - 20);
		int y = rnd_range(15, hh - 15);
		int dir = rnd(8);
		for (int step = 0; step < 90; ++step) {
			int t = tile_at(w, x, y);
			if (t == T_DEEP) { break; }
			set_tile(w, x, y, step % 7 == 0 ? T_DEEP : T_SHALLOW);
			if (rnd(3) == 0) { dir = (dir + rnd(3) + 7) % 8; }
			x = iclamp(x + DX[dir], 1, ww - 2);
			y = iclamp(y + DY[dir], 1, hh - 2);
		}
	}
	// Swampton, near the middle
	int sx = ww / 2 - 13;
	int sy = hh / 2 - 9;
	fill_rect(w, sx - 2, sy - 2, sx + 27, sy + 20, T_GRASS);
	fill_rect(w, sx, sy, sx + 25, sy + 18, T_ROAD);
	for (int i = 0; i < 60; ++i) {
		int fx = sx + rnd(26);
		int fy = sy + rnd(19);
		set_tile(w, fx, fy, rnd(3) == 0 ? T_FLOWERS : T_GRASS);
	}
	shop(w, sx + 1, sy + 1, R_WEAPONS, M_GATOR_NPC, T_ANVIL);
	shop(w, sx + 9, sy + 1, R_ARMOR, M_TURTLE_NPC, -1);
	shop(w, sx + 17, sy + 1, R_ALCHEMIST, M_FROGFOLK, T_ALCHEMY);
	shop(w, sx + 1, sy + 12, R_PROVISIONER, M_GATOR_NPC, -1);
	shop(w, sx + 9, sy + 12, R_BANKER, M_TURTLE_NPC, T_BANK);
	shop(w, sx + 17, sy + 12, R_HEALER, M_FROGFOLK, -1);
	// plaza: well, fire pit, forge + anvil yard, the sewer grate, a sign
	int px = sx + 12;
	int py = sy + 9;
	set_tile(w, px, py, T_WELL);
	set_tile(w, px - 4, py, T_FIRE);
	set_tile(w, px + 5, py - 1, T_FORGE);
	set_tile(w, px + 6, py - 1, T_ANVIL);
	set_tile(w, px + 3, py + 1, T_STAIRS_DN);
	set_tile(w, px - 1, py - 2, T_SIGN);
	plan_npc(w, px - 2, py + 1, M_GATOR_NPC, R_MAYOR);
	plan_npc(w, px + 1, py - 2, M_GUARD, R_CAPTAIN);
	plan_npc(w, sx + 25, sy + 8, M_FROGFOLK, R_FISHMONGER);
	plan_npc(w, sx + 3, sy + 8, M_GATOR_NPC, -1);
	plan_npc(w, sx + 20, sy + 10, M_FROGFOLK, -1);
	plan_npc(w, sx + 14, sy + 16, M_TURTLE_NPC, -1);
	plan_npc(w, sx - 1, sy + 9, M_GUARD, -1);
	plan_npc(w, sx + 26, sy + 9, M_GUARD, -1);
	plan_npc(w, sx + 12, sy - 1, M_GUARD, -1);
	add_town(w, sx - 3, sy - 3, sx + 28, sy + 21, "Swampton");
	W_ARRIVE_X[w] = px;
	W_ARRIVE_Y[w] = py + 2;

	// the Shrine of the Six on a knoll north-west of town
	int shx = sx - 10;
	int shy = sy - 8;
	fill_disc(w, shx, shy, 3, T_STONEFLOOR);
	set_tile(w, shx, shy, T_SHRINE);
	plan_npc(w, shx, shy + 2, M_TURTLE_NPC, R_ORACLE);
	carve_road(w, shx, shy + 3, sx, sy + 9, T_DIRT);

	// Mudport on the east coast: scan west from the east edge for the coast line
	int mx = ww - 22;
	int my = hh / 2 + 10;
	fill_rect(w, mx - 1, my - 1, mx + 14, my + 10, T_SAND);
	fill_rect(w, mx, my, mx + 13, my + 9, T_ROAD);
	fill_rect(w, mx + 14, my - 1, mx + 17, my + 10, T_SHALLOW);
	fill_rect(w, mx + 14, my + 4, mx + 17, my + 5, T_BRIDGE);
	shop(w, mx + 1, my + 1, R_FISHMONGER, M_FROGFOLK, -1);
	shop(w, mx + 1, my + 5, R_PROVISIONER, M_GATOR_NPC, T_FORGE);
	set_tile(w, mx + 10, my + 3, T_FIRE);
	set_tile(w, mx + 11, my + 3, T_ANVIL);
	plan_npc(w, mx + 10, my + 6, M_FROGFOLK, R_HEALER);
	plan_npc(w, mx + 12, my + 8, M_GUARD, -1);
	add_town(w, mx - 2, my - 2, mx + 15, my + 11, "Mudport");
	carve_road(w, sx + 25, sy + 9, mx, my + 5, T_ROAD);

	// six lilygates in a ring around Swampton
	for (int g = 1; g <= 6; ++g) {
		int ang = (g - 1) * 60 + 30;
		int gx = px + math::itrunc(math::cos(ang * 0.0174533) * 26.0);
		int gy = py + math::itrunc(math::sin(ang * 0.0174533) * 19.0);
		gx = iclamp(gx, 4, ww - 5);
		gy = iclamp(gy, 4, hh - 5);
		fill_disc(w, gx, gy, 2, T_STONEFLOOR);
		add_gate(w, gx, gy, g);
		carve_road(w, px, py + 2, gx, gy + 3, T_DIRT);
	}

	// wild chests and spawners
	for (int c = 0; c < 6; ++c) {
		array<int> p = find_spot(w, rnd_range(8, ww - 8), rnd_range(8, hh - 8), 12, LAND_MASK);
		if (town_at(w, p[0], p[1]) < 0) { add_chest(w, p[0], p[1], 1 + rnd(2)); }
	}
	for (int s = 0; s < 42; ++s) {
		int x = rnd_range(4, ww - 5);
		int y = rnd_range(4, hh - 5);
		int t = tile_at(w, x, y);
		if (town_at(w, x, y) >= 0 || cheb(x, y, px, py) < 18) { continue; }
		int kind = -1;
		if (t == T_SWAMP || t == T_REEDS || t == T_BOG || t == T_MUD || t == T_LILY) {
			array<string> pool = ["Frog", "Mosquito Swarm", "Cottonmouth", "Giant Frog", "Crawdad"];
			kind = mid(pool[rnd(pool.size())]);
		} else if (t == T_OAK || t == T_BUSH || t == T_PINE) {
			array<string> pool = ["Timber Wolf", "Treant", "Feral Hog", "Marsh Rabbit"];
			kind = mid(pool[rnd(pool.size())]);
		} else if (t == T_DIRT || t == T_BOULDER || t == T_ORE) {
			array<string> pool = ["Goblin Poacher", "Bandit", "Feral Hog"];
			kind = mid(pool[rnd(pool.size())]);
		} else if (t == T_SHALLOW || t == T_SAND) {
			array<string> pool = ["Heron", "Cottonmouth", "Crawdad"];
			kind = mid(pool[rnd(pool.size())]);
		} else if (T_WALK[t] == 1) {
			array<string> pool = ["Swamp Rat", "Marsh Rabbit", "Feral Hog", "Mosquito Swarm"];
			kind = mid(pool[rnd(pool.size())]);
		}
		if (kind >= 0) { add_spawner(w, x, y, kind, 2 + rnd(2), 6, 300 + rnd(300)); }
	}
	// rats loiter just outside town
	add_spawner(w, sx + 32, sy - 4, mid("Swamp Rat"), 3, 5, 300);
}

// a world's way home: a lilygate at its arrival point leading back to Gatorland
void arrival_gate(int w, int x, int y) {
	fill_disc(w, x, y, 2, w == 6 ? T_CLOUD : (w == 2 ? T_RUINFLOOR : (w == 4 ? T_SNOW : (w == 5 ? T_OBSIDIAN : T_MUD))));
	add_gate(w, x, y, 0);
	W_ARRIVE_X[w] = x;
	W_ARRIVE_Y[w] = y + 2;
}

void boss_lair(int w, int x, int y) {
	int boss = WORLDS[w].boss;
	if (boss >= 0) { add_spawner(w, x, y, boss, 1, 3, 3000); }
}

void spawners_by_pool(int w, array<string> pool, int count, int mask, int maxn, int delay) {
	int ww = W_W[w];
	int hh = W_H[w];
	for (int s = 0; s < count; ++s) {
		array<int> p = find_spot(w, rnd_range(3, ww - 4), rnd_range(3, hh - 4), 6, mask);
		if (cheb(p[0], p[1], W_ARRIVE_X[w], W_ARRIVE_Y[w]) < 8) { continue; }
		add_spawner(w, p[0], p[1], mid(pool[rnd(pool.size())]), maxn, 6, delay);
	}
}

void gen_bayou() {
	int w = 1;
	int ww = W_W[w];
	int hh = W_H[w];
	int salt = SEED * 13 + 101;
	for (int y = 0; y < hh; ++y) {
		for (int x = 0; x < ww; ++x) {
			int e = fractal(x, y, salt);
			int h = hash3(x, y, salt + 3) & 1023;
			int t = T_SWAMP;
			if (e > 560) {
				t = h < 220 ? T_CYPRESS : (h < 340 ? T_REEDS : (h < 560 ? T_MUD : (h < 640 ? T_BUSH : T_BOG)));
			} else if (e > 500) {
				t = h < 300 ? T_REEDS : T_MUD;
			} else {
				t = h < 70 ? T_LILY : (h < 90 ? T_CYPRESS : T_SWAMP);
			}
			TILES[W_OFF[w] + y * ww + x] = t;
		}
	}
	arrival_gate(w, 8, 8);
	fill_disc(w, 8, 10, 4, T_MUD);
	set_tile(w, 8, 8, T_GATE);
	// shacks on stilts
	for (int s = 0; s < 4; ++s) {
		int x = rnd_range(20, ww - 20);
		int y = rnd_range(12, hh - 12);
		fill_rect(w, x - 1, y - 1, x + 5, y + 4, T_BRIDGE);
		building(w, x, y, 5, 4, T_WOODWALL, T_WOODFLOOR);
		add_chest(w, x + 2, y + 1, 2);
	}
	// Big Mama's nest: an island ringed with reeds in the far corner
	int bx = ww - 14;
	int by = hh - 12;
	fill_disc(w, bx, by, 7, T_SWAMP);
	fill_disc(w, bx, by, 5, T_MUD);
	fill_disc(w, bx, by, 2, T_BOG);
	set_tile(w, bx + 3, by - 3, T_GOLDPILE);
	add_chest(w, bx - 3, by + 3, 3);
	boss_lair(w, bx, by);
	spawners_by_pool(w, ["Snapping Turtle", "Voodoo Toad", "Crawdad Knight", "Bog Zombie", "Mosquito Swarm", "Frog", "Cottonmouth"], 26, LAND_MASK + 4, 3, 400);
}

void gen_sunken() {
	int w = 2;
	int ww = W_W[w];
	int hh = W_H[w];
	fill_rect(w, 0, 0, ww - 1, hh - 1, T_DEEP);
	// city blocks on a grid of flooded streets
	for (int by = 6; by < hh - 8; by += 11) {
		for (int bx = 6; bx < ww - 8; bx += 12) {
			fill_rect(w, bx - 2, by - 2, bx + 9, by + 8, T_SHALLOW);
			int kind = hash3(bx, by, SEED) % 4;
			if (kind == 0) {
				building(w, bx, by, 8, 6, T_RUINWALL, T_RUINFLOOR);
				add_chest(w, bx + 2, by + 2, 3);
			} else if (kind == 1) {
				fill_rect(w, bx, by, bx + 7, by + 5, T_RUINFLOOR);
				set_tile(w, bx + 1, by + 1, T_PILLAR);
				set_tile(w, bx + 6, by + 1, T_PILLAR);
				set_tile(w, bx + 1, by + 4, T_PILLAR);
				set_tile(w, bx + 6, by + 4, T_PILLAR);
			} else if (kind == 2) {
				fill_rect(w, bx, by, bx + 7, by + 5, T_KELP);
				fill_disc(w, bx + 3, by + 3, 1, T_CORAL);
			} else {
				fill_rect(w, bx, by, bx + 7, by + 5, T_RUINFLOOR);
				for (int k = 0; k < 6; ++k) { set_tile(w, bx + rnd(8), by + rnd(6), T_CORAL); }
			}
		}
	}
	// scatter ruin rubble and coral
	for (int k = 0; k < 140; ++k) {
		int x = rnd(ww);
		int y = rnd(hh);
		if (tile_at(w, x, y) == T_SHALLOW) { set_tile(w, x, y, rnd(3) == 0 ? T_CORAL : T_RUINFLOOR); }
	}
	arrival_gate(w, 3, hh - 5);
	fill_rect(w, 1, hh - 8, 8, hh - 2, T_RUINFLOOR);
	set_tile(w, 3, hh - 5, T_GATE);
	// the drowned palace up north
	int pxl = ww / 2 - 8;
	fill_rect(w, pxl - 2, 0, pxl + 17, 9, T_SHALLOW);
	building(w, pxl, 1, 16, 8, T_RUINWALL, T_RUINFLOOR);
	set_tile(w, pxl + 8, 2, T_GOLDPILE);
	set_tile(w, pxl + 2, 2, T_SCONCE);
	set_tile(w, pxl + 13, 2, T_SCONCE);
	add_chest(w, pxl + 3, 5, 4);
	boss_lair(w, pxl + 8, 4);
	spawners_by_pool(w, ["Drowned Sailor", "Electric Eel", "Merrow Spearman", "Sea Witch", "Angler Lurker"], 26, LAND_MASK + 4, 3, 450);
}

void gen_gloomfen() {
	int w = 3;
	int ww = W_W[w];
	int hh = W_H[w];
	int salt = SEED * 17 + 303;
	for (int y = 0; y < hh; ++y) {
		for (int x = 0; x < ww; ++x) {
			int e = fractal(x, y, salt);
			int h = hash3(x, y, salt + 9) & 1023;
			int t = T_BOG;
			if (e < 420) { t = h < 60 ? T_DEADTREE : T_SWAMP; }
			else if (e < 470) { t = T_MUD; }
			else { t = h < 140 ? T_DEADTREE : (h < 190 ? T_GRAVE : (h < 300 ? T_TALLGRASS : (h < 330 ? T_WEB : T_BOG))); }
			TILES[W_OFF[w] + y * ww + x] = t;
		}
	}
	arrival_gate(w, ww / 2, hh - 6);
	// graveyards with fences
	for (int g = 0; g < 4; ++g) {
		int x = rnd_range(8, ww - 16);
		int y = rnd_range(8, hh - 20);
		fill_rect(w, x, y, x + 8, y + 6, T_FENCE);
		fill_rect(w, x + 1, y + 1, x + 7, y + 5, T_BOG);
		for (int k = 0; k < 8; ++k) { set_tile(w, x + 1 + rnd(7), y + 1 + rnd(5), T_GRAVE); }
		set_tile(w, x + 4, y + 6, T_DIRT);
	}
	// Crocula's chapel
	int cx = ww / 2 - 6;
	building(w, cx, 3, 13, 9, T_RUINWALL, T_STONEFLOOR);
	fill_rect(w, cx + 2, 5, cx + 10, 5, T_CARPET);
	set_tile(w, cx + 2, 4, T_SCONCE);
	set_tile(w, cx + 10, 4, T_SCONCE);
	add_chest(w, cx + 6, 4, 4);
	boss_lair(w, cx + 6, 6);
	spawners_by_pool(w, ["Will-o'-Wisp", "Swamp Zombie", "Banshee", "Ghoul Gator", "Bone Heron"], 28, LAND_MASK + 4, 3, 450);
}

// cellular-automaton caves (ice caverns, sewers)
void cave_automaton(int w, int wall, int floor, int fillpct, int salt) {
	int ww = W_W[w];
	int hh = W_H[w];
	int base = W_OFF[w];
	for (int y = 0; y < hh; ++y) {
		for (int x = 0; x < ww; ++x) {
			bool edge = x == 0 || y == 0 || x == ww - 1 || y == hh - 1;
			TILES[base + y * ww + x] = edge || (hash3(x, y, salt) % 100) < fillpct ? wall : floor;
		}
	}
	array<int> next;
	for (int i = 0; i < ww * hh; ++i) { next.push(0); }
	for (int pass = 0; pass < 4; ++pass) {
		for (int y = 0; y < hh; ++y) {
			for (int x = 0; x < ww; ++x) {
				int walls = 0;
				for (int dy = -1; dy <= 1; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						int nx = x + dx;
						int ny = y + dy;
						if (nx < 0 || ny < 0 || nx >= ww || ny >= hh || TILES[base + ny * ww + nx] == wall) { walls = walls + 1; }
					}
				}
				next[y * ww + x] = walls >= 5 ? wall : floor;
			}
		}
		for (int i = 0; i < ww * hh; ++i) { TILES[base + i] = next[i]; }
	}
}

// guarantee connectivity with a winding corridor through a list of points
void cave_corridor(int w, int ax, int ay, int bx, int by, int floor) {
	int x = ax;
	int y = ay;
	int guard = 0;
	while ((x != bx || y != by) && guard < 800) {
		guard = guard + 1;
		if (rnd(3) == 0) { x = x + rnd_range(-1, 1); }
		else if (rnd(2) == 0) { x = x + isign(bx - x); }
		else { y = y + isign(by - y); }
		x = iclamp(x, 1, W_W[w] - 2);
		y = iclamp(y, 1, W_H[w] - 2);
		set_tile(w, x, y, floor);
		set_tile(w, x + 1, y, floor);
	}
}

void gen_frostfang() {
	int w = 4;
	int ww = W_W[w];
	int hh = W_H[w];
	cave_automaton(w, T_ICEWALL, T_SNOW, 44, SEED * 19 + 404);
	int salt = SEED * 23 + 17;
	for (int y = 1; y < hh - 1; ++y) {
		for (int x = 1; x < ww - 1; ++x) {
			int t = tile_at(w, x, y);
			int h = hash3(x, y, salt) & 1023;
			if (t == T_SNOW && vnoise(x, y, 9, salt) > 640) { set_tile(w, x, y, T_ICE); }
			else if (t == T_ICEWALL && h < 40) { set_tile(w, x, y, T_CRYSTAL); }
			else if (t == T_ICEWALL && h < 70) { set_tile(w, x, y, T_ORE); }
		}
	}
	arrival_gate(w, 6, 6);
	fill_disc(w, 6, 7, 3, T_SNOW);
	set_tile(w, 6, 6, T_GATE);
	int qx = ww - 12;
	int qy = hh - 10;
	fill_disc(w, qx, qy, 6, T_ICE);
	fill_disc(w, qx, qy, 1, T_SNOW);
	set_tile(w, qx - 4, qy - 4, T_CRYSTAL);
	set_tile(w, qx + 4, qy - 4, T_CRYSTAL);
	add_chest(w, qx + 2, qy + 4, 4);
	cave_corridor(w, 6, 8, ww / 2, hh / 2, T_SNOW);
	cave_corridor(w, ww / 2, hh / 2, qx, qy, T_SNOW);
	cave_corridor(w, 6, 8, ww - 10, 8, T_SNOW);
	cave_corridor(w, ww / 3, hh - 8, qx, qy, T_SNOW);
	boss_lair(w, qx, qy);
	spawners_by_pool(w, ["Ice Bat", "Yeti", "Frost Wisp", "Penguin Mercenary", "Snow Troll"], 24, LAND_MASK, 3, 450);
	for (int c = 0; c < 3; ++c) {
		array<int> p = find_spot(w, rnd_range(10, ww - 10), rnd_range(10, hh - 10), 10, LAND_MASK);
		add_chest(w, p[0], p[1], 3);
	}
}

void gen_scorchmaw() {
	int w = 5;
	int ww = W_W[w];
	int hh = W_H[w];
	int salt = SEED * 29 + 505;
	int cxm = ww / 2;
	int cym = hh / 2;
	for (int y = 0; y < hh; ++y) {
		for (int x = 0; x < ww; ++x) {
			int e = fractal(x, y, salt);
			int h = hash3(x, y, salt + 1) & 1023;
			int band = vnoise(x, y, 13, salt + 7);
			int t = T_ASH;
			bool edge = x < 2 || y < 2 || x >= ww - 2 || y >= hh - 2;
			if (edge || e > 780) { t = T_BASALT; }
			else if (band > 470 && band < 520) { t = T_LAVA; }
			else if (h < 30) { t = T_ORE; }
			else if (h < 70) { t = T_BOULDER; }
			else if (e < 360) { t = T_OBSIDIAN; }
			TILES[W_OFF[w] + y * ww + x] = t;
		}
	}
	// the crater: a lava ring around the wyrm's island, one obsidian causeway
	fill_disc(w, cxm, cym, 10, T_LAVA);
	fill_disc(w, cxm, cym, 6, T_OBSIDIAN);
	fill_rect(w, cxm - 1, cym + 5, cxm, cym + 11, T_OBSIDIAN);
	set_tile(w, cxm + 2, cym - 2, T_GOLDPILE);
	add_chest(w, cxm - 3, cym - 2, 5);
	boss_lair(w, cxm, cym);
	arrival_gate(w, 6, hh - 7);
	fill_disc(w, 6, hh - 6, 3, T_OBSIDIAN);
	set_tile(w, 6, hh - 7, T_GATE);
	carve_road(w, 6, hh - 5, cxm, cym + 12, T_OBSIDIAN);
	spawners_by_pool(w, ["Fire Imp", "Salamander", "Magma Golem", "Ash Bat", "Cinder Cultist"], 24, LAND_MASK, 3, 450);
}

void gen_cloudreach() {
	int w = 6;
	int ww = W_W[w];
	int hh = W_H[w];
	fill_rect(w, 0, 0, ww - 1, hh - 1, T_SKY);
	array<int> ix;
	array<int> iy;
	ix.push(8);
	iy.push(hh - 9);
	for (int k = 0; k < 11; ++k) {
		ix.push(rnd_range(12, ww - 12));
		iy.push(rnd_range(10, hh - 10));
	}
	ix.push(ww - 12);
	iy.push(9);
	for (int k = 0; k < ix.size(); ++k) {
		int r = k == ix.size() - 1 ? 7 : 3 + rnd(3);
		fill_disc(w, ix[k], iy[k], r, T_CLOUD);
		fill_disc(w, ix[k], iy[k], r - 2, rnd(2) == 0 ? T_GRASS : T_FLOWERS);
		if (k > 0) {
			// rainbow bridge from the previous island
			int x = ix[k - 1];
			int y = iy[k - 1];
			int guard = 0;
			while ((x != ix[k] || y != iy[k]) && guard < 300) {
				guard = guard + 1;
				if (x != ix[k] && (y == iy[k] || rnd(2) == 0)) { x = x + isign(ix[k] - x); }
				else { y = y + isign(iy[k] - y); }
				if (tile_at(w, x, y) == T_SKY) { set_tile(w, x, y, T_RAINBOW); }
			}
		}
		if (k > 0 && k < ix.size() - 1 && rnd(3) == 0) { add_chest(w, ix[k], iy[k], 4); }
	}
	arrival_gate(w, ix[0], iy[0]);
	int bx = ix[ix.size() - 1];
	int by = iy[iy.size() - 1];
	set_tile(w, bx + 3, by - 3, T_GOLDPILE);
	add_chest(w, bx - 3, by + 2, 5);
	boss_lair(w, bx, by);
	for (int k = 1; k < ix.size() - 1; ++k) {
		array<string> pool = ["Harpy", "Storm Elemental", "Cloud Sheep", "Griffon", "Sky Pirate"];
		add_spawner(w, ix[k], iy[k], mid(pool[rnd(pool.size())]), 2 + rnd(2), 3, 450);
	}
}

void gen_sewers() {
	int w = 7;
	int ww = W_W[w];
	int hh = W_H[w];
	fill_rect(w, 0, 0, ww - 1, hh - 1, T_BRICK);
	// a grid of tunnels with a channel of sewage down the middle of each
	for (int y = 4; y < hh - 3; y += 8) {
		fill_rect(w, 2, y - 1, ww - 3, y + 1, T_SEWERFLOOR);
		fill_rect(w, 2, y, ww - 3, y, T_SEWAGE);
	}
	for (int x = 5; x < ww - 3; x += 10) {
		fill_rect(w, x - 1, 3, x + 1, hh - 4, T_SEWERFLOOR);
		fill_rect(w, x, 3, x, hh - 4, T_SEWAGE);
	}
	// grates to cross the sewage
	for (int k = 0; k < 40; ++k) {
		int x = rnd_range(2, ww - 3);
		int y = rnd_range(2, hh - 3);
		if (tile_at(w, x, y) == T_SEWAGE) { set_tile(w, x, y, T_BRIDGE); }
	}
	// the stairs further down get a room of their own, west of where the side rooms put chests
	fill_rect(w, 2, hh - 8, 7, hh - 3, T_SEWERFLOOR);
	// side rooms
	for (int k = 0; k < 7; ++k) {
		int x = rnd_range(3, ww - 12);
		int y = rnd_range(3, hh - 10);
		fill_rect(w, x, y, x + 6, y + 4, T_SEWERFLOOR);
		if (rnd(2) == 0) { add_chest(w, x + 3, y + 2, 1); }
	}
	// the entrance and the Rat King's throne
	fill_rect(w, 2, 2, 7, 7, T_SEWERFLOOR);
	set_tile(w, 3, 3, T_STAIRS_UP);
	W_ARRIVE_X[w] = 4;
	W_ARRIVE_Y[w] = 4;
	int kx = ww - 9;
	int ky = hh - 8;
	fill_rect(w, kx - 5, ky - 4, kx + 5, ky + 4, T_SEWERFLOOR);
	set_tile(w, kx, ky - 3, T_GOLDPILE);
	set_tile(w, kx - 4, ky - 3, T_SCONCE);
	set_tile(w, kx + 4, ky - 3, T_SCONCE);
	add_chest(w, kx + 3, ky + 2, 2);
	boss_lair(w, kx, ky);
	spawners_by_pool(w, ["Sewer Rat", "Sewer Rat", "Green Slime", "Giant Cockroach"], 16, 2, 3, 300);
}

// the Raccoon Warrens below the sewers: a burrow maze with a few loops, and dens where the bandits stash their loot
void gen_warrens() {
	int w = W_WARRENS;
	int ww = W_W[w];
	int hh = W_H[w];
	fill_rect(w, 0, 0, ww - 1, hh - 1, T_CAVEWALL);
	int cw = (ww - 1) / 2;
	int ch = (hh - 1) / 2;
	array<int> seen;
	for (int i = 0; i < cw * ch; ++i) { seen.push(0); }
	array<int> stack = [0];
	seen[0] = 1;
	set_tile(w, 1, 1, T_DIRT);
	while (stack.size() > 0) {
		int c = stack[stack.size() - 1];
		int cx = c % cw;
		int cy = c / cw;
		int start = rnd(4);
		int next = -1;
		for (int k = 0; k < 4 && next < 0; ++k) {
			int d = (start + k) % 4 * 2;
			int nx = cx + DX[d];
			int ny = cy + DY[d];
			if (nx >= 0 && ny >= 0 && nx < cw && ny < ch && seen[ny * cw + nx] == 0) {
				next = ny * cw + nx;
				set_tile(w, 1 + cx * 2 + DX[d], 1 + cy * 2 + DY[d], T_DIRT);
			}
		}
		if (next < 0) { stack.pop(); continue; }
		seen[next] = 1;
		set_tile(w, 1 + (next % cw) * 2, 1 + (next / cw) * 2, T_DIRT);
		stack.push(next);
	}
	for (int k = 0; k < 40; ++k) {
		int x = rnd_range(1, ww - 2);
		int y = rnd_range(1, hh - 2);
		if ((x + y) % 2 == 1) { set_tile(w, x, y, T_DIRT); }
	}
	fill_rect(w, 1, 1, 5, 3, T_DIRT);
	W_ARRIVE_X[w] = 3;
	W_ARRIVE_Y[w] = 3;
	// dens first, then their chests: a later den never paves over an earlier chest
	array<int> den_x;
	array<int> den_y;
	for (int k = 0; k < 6; ++k) {
		int x = 1 + rnd(cw - 2) * 2;
		int y = 1 + rnd(ch - 1) * 2;
		if (cheb(x + 2, y + 1, W_ARRIVE_X[w], W_ARRIVE_Y[w]) < 10) { continue; }
		fill_rect(w, x, y, x + 4, y + 2, T_DIRT);
		den_x.push(x + 2);
		den_y.push(y + 1);
	}
	for (int k = 0; k < den_x.size(); ++k) {
		if (rnd(2) == 0 && tile_at(w, den_x[k], den_y[k]) != T_CHEST) { add_chest(w, den_x[k], den_y[k], 2); }
		add_spawner(w, den_x[k], den_y[k], mid("Raccoon Bandit"), 2, 3, 400);
	}
	spawners_by_pool(w, ["Sewer Rat", "Giant Cockroach"], 4, 2, 2, 400);
}

// every realm, then the flat mirrors the renderer and movement code read
void world_generate(int seed) {
	SEED = seed;
	rng_seed(seed);
	GATE_W = [];
	GATE_X = [];
	GATE_Y = [];
	GATE_DEST = [];
	TOWN_W = [];
	TOWN_X0 = [];
	TOWN_Y0 = [];
	TOWN_X1 = [];
	TOWN_Y1 = [];
	TOWN_NAME = [];
	CHEST_W = [];
	CHEST_X = [];
	CHEST_Y = [];
	CHEST_TIER = [];
	CHEST_T = [];
	SP_W = [];
	SP_X = [];
	SP_Y = [];
	SP_KIND = [];
	SP_MAX = [];
	SP_RAD = [];
	SP_DELAY = [];
	SP_NEXT = [];
	SP_ALIVE = [];
	NPC_PLAN_W = [];
	NPC_PLAN_X = [];
	NPC_PLAN_Y = [];
	NPC_PLAN_KIND = [];
	NPC_PLAN_ROLE = [];
	W_WEATHER = [];
	W_WEATHER_T = [];
	W_ARRIVE_X = [];
	W_ARRIVE_Y = [];
	W_BOSS_T = [];
	world_alloc();
	gen_overworld();
	gen_bayou();
	gen_sunken();
	gen_gloomfen();
	gen_frostfang();
	gen_scorchmaw();
	gen_cloudreach();
	gen_sewers();
	gen_warrens();
	// the sewer grate in Swampton leads down; the stairs up lead back
	add_gate(0, gate_x_of_stairs(), gate_y_of_stairs(), W_SEWERS);
	set_tile(0, gate_x_of_stairs(), gate_y_of_stairs(), T_STAIRS_DN);
	add_gate(W_SEWERS, 3, 3, 0);
	set_tile(W_SEWERS, 3, 3, T_STAIRS_UP);
	// and further down, the Raccoon Warrens
	add_gate(W_SEWERS, 3, W_H[W_SEWERS] - 5, W_WARRENS);
	set_tile(W_SEWERS, 3, W_H[W_SEWERS] - 5, T_STAIRS_DN);
	add_gate(W_WARRENS, 3, 1, W_SEWERS);
	set_tile(W_WARRENS, 3, 1, T_STAIRS_UP);
}

int STAIRS_X;
int STAIRS_Y;
int gate_x_of_stairs() { find_stairs(); return STAIRS_X; }
int gate_y_of_stairs() { find_stairs(); return STAIRS_Y; }
void find_stairs() {
	for (int y = 0; y < W_H[0]; ++y) {
		for (int x = 0; x < W_W[0]; ++x) {
			if (tile_at(0, x, y) == T_STAIRS_DN) { STAIRS_X = x; STAIRS_Y = y; return; }
		}
	}
}

// where a traveller lands arriving in `dest` through a gate
array<int> gate_arrival(int from_w, int dest) {
	// home, or between two dungeon levels: beside the gate leading back
	if (dest == 0 || from_w != 0) {
		int g = gate_in(dest, from_w);
		if (g >= 0) {
			array<int> p = find_spot(dest, GATE_X[g], GATE_Y[g] + 1, 4, LAND_MASK);
			return p;
		}
		if (dest == 0) { return [W_ARRIVE_X[0], W_ARRIVE_Y[0]]; }
	}
	return find_spot(dest, W_ARRIVE_X[dest], W_ARRIVE_Y[dest], 6, LAND_MASK);
}
