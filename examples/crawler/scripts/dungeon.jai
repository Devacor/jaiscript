// The floors: rooms joined by corridors, the boss arena, line of sight and the field of view,
// and the breadth-first path the autopilot walks.

int TILE_WALL = 0;
int TILE_FLOOR = 1;
int TILE_STAIRS = 2;
int TILE_STATUE = 3;   // JAI the jackodile, who blesses your evasion
int TILE_MURAL = 4;    // DEVACOR's mural, which mends you

class Room { int x; int y; int w; int h; }

class Spot { int x; int y; }

function tile_at(int x, int y) -> int {
	if (!in_bounds(x, y)) { return TILE_WALL; }
	return G.tiles[idx(x, y)];
}

function is_walkable(int x, int y) -> bool { return tile_at(x, y) != TILE_WALL; }

function carve_room(Room room) -> void {
	for (int ry = room.y; ry < room.y + room.h; ++ry) {
		for (int rx = room.x; rx < room.x + room.w; ++rx) { G.tiles[idx(rx, ry)] = TILE_FLOOR; }
	}
}

function carve_corridor(int x1, int y1, int x2, int y2) -> void {
	int cx = x1;
	int cy = y1;
	if (RNG.chance(50)) {
		while (cx != x2) { G.tiles[idx(cx, cy)] = TILE_FLOOR; cx += sign_of(x2 - cx); }
		while (cy != y2) { G.tiles[idx(cx, cy)] = TILE_FLOOR; cy += sign_of(y2 - cy); }
	} else {
		while (cy != y2) { G.tiles[idx(cx, cy)] = TILE_FLOOR; cy += sign_of(y2 - cy); }
		while (cx != x2) { G.tiles[idx(cx, cy)] = TILE_FLOOR; cx += sign_of(x2 - cx); }
	}
	G.tiles[idx(cx, cy)] = TILE_FLOOR;
}

function room_center(Room room) -> Spot { return Spot{room.x + room.w / 2, room.y + room.h / 2}; }

function rooms_overlap(Room a, Room b) -> bool {
	return a.x - 1 < b.x + b.w && a.x + a.w + 1 > b.x && a.y - 1 < b.y + b.h && a.y + a.h + 1 > b.y;
}

class FloorPlan { Spot start; Spot stairs; }

// fills G.tiles and G.rooms
function generate_floor(int floor) -> FloorPlan {
	G.tiles = [];
	G.seen = [];
	for (int i = 0; i < MAP_W * MAP_H; ++i) {
		G.tiles.push(TILE_WALL);
		G.seen.push(false);
	}
	G.rooms = [];
	if (floor >= 10) { return generate_boss_arena(); }

	int room_target = 7 + RNG.next(3);
	for (int attempt = 0; attempt < 80 && G.rooms.size() < room_target; ++attempt) {
		int rw = 5 + RNG.next(8);
		int rh = 3 + RNG.next(4);
		int rx = 1 + RNG.next(MAP_W - rw - 2);
		int ry = 1 + RNG.next(MAP_H - rh - 2);
		Room candidate{rx, ry, rw, rh};
		bool blocked = false;
		for (auto other : G.rooms) {
			if (rooms_overlap(candidate, other)) {
				blocked = true;
				break;
			}
		}
		if (blocked) { continue; }
		G.rooms.push(candidate);
		carve_room(candidate);
	}
	for (int i = 1; i < G.rooms.size(); ++i) {
		Spot from = room_center(G.rooms[i - 1]);
		Spot to = room_center(G.rooms[i]);
		carve_corridor(from.x, from.y, to.x, to.y);
	}
	FloorPlan plan{room_center(G.rooms[0]), room_center(G.rooms[G.rooms.size() - 1])};
	G.tiles[idx(plan.stairs.x, plan.stairs.y)] = TILE_STAIRS;
	if (has_welcome()) {
		Spot statue = welcome_layout().statue;
		G.tiles[idx(statue.x, statue.y)] = TILE_STATUE;
	}
	if (RNG.chance(40)) { place_landmark(TILE_STATUE, plan); }
	if (RNG.chance(30)) { place_landmark(TILE_MURAL, plan); }
	return plan;
}

function place_landmark(int tile, FloorPlan plan) -> void {
	if (G.rooms.size() < 2) { return; }
	for (int attempt = 0; attempt < 20; ++attempt) {
		Room room = G.rooms[RNG.next(G.rooms.size())];
		int lx = room.x + RNG.next(room.w);
		int ly = room.y + RNG.next(room.h);
		if ((lx == plan.start.x && ly == plan.start.y) || (lx == plan.stairs.x && ly == plan.stairs.y)) { continue; }
		if (G.tiles[idx(lx, ly)] != TILE_FLOOR) { continue; }
		G.tiles[idx(lx, ly)] = tile;
		return;
	}
}

// Floor 1's welcome: in the first room, JAI's statue a few steps ahead of where you wake, the
// bones of the last one down at your feet with their blade, and a Grublin by the statue. It is
// laid out from the room alone, so the view finds the bones again without saving where they are.
class Welcome { int face; Spot find; Spot statue; Spot greeter; }

function has_welcome() -> bool { return G.floor == 1 && G.rooms.size() >= 2; }

function welcome_layout() -> Welcome {
	Room room = G.rooms[0];
	Spot start = room_center(room);
	array<int> reach = [start.y - room.y, room.x + room.w - 1 - start.x, room.y + room.h - 1 - start.y, start.x - room.x];
	int face = 1;
	for (int f = 0; f < 4; ++f) {
		if (reach[f] > reach[face]) { face = f; }
	}
	int dx = FACE_DX[face];
	int dy = FACE_DY[face];
	int far = imin(3, reach[face]);
	Welcome w;
	w.face = face;
	w.find = Spot{start.x + dx, start.y + dy};
	w.statue = Spot{start.x + dx * far, start.y + dy * far};
	if (reach[face] >= far + 1) {
		w.greeter = Spot{start.x + dx * (far + 1), start.y + dy * (far + 1)};
	} else {
		w.greeter = Spot{w.statue.x + FACE_DX[(face + 1) % 4], w.statue.y + FACE_DY[(face + 1) % 4]};
	}
	return w;
}

// floor 10: one great hall, a corridor in from the west, and Devacor's defaced mural
function generate_boss_arena() -> FloorPlan {
	Room arena{8, 3, 48, 14};
	G.rooms.push(arena);
	carve_room(arena);
	carve_corridor(3, 10, 8, 10);
	G.tiles[idx(3, 10)] = TILE_FLOOR;
	G.tiles[idx(32, 4)] = TILE_MURAL;
	return FloorPlan{Spot{3, 10}, Spot{-1, -1}};
}

// ---------------------------------------------------------------------- FOV ---
// every tile strictly between the two must be open
function line_clear(int x1, int y1, int x2, int y2) -> bool {
	int dx = iabs(x2 - x1);
	int dy = iabs(y2 - y1);
	int sx = x2 > x1 ? 1 : -1;
	int sy = y2 > y1 ? 1 : -1;
	int err = dx - dy;
	int cx = x1;
	int cy = y1;
	while (true) {
		if (cx == x2 && cy == y2) { return true; }
		if ((cx != x1 || cy != y1) && tile_at(cx, cy) == TILE_WALL) { return false; }
		int e2 = err * 2;
		if (e2 > -dy) { err -= dy; cx += sx; }
		if (e2 < dx) { err += dx; cy += sy; }
	}
	return false;
}

function compute_fov() -> void {
	G.visible = [];
	for (int i = 0; i < MAP_W * MAP_H; ++i) { G.visible.push(false); }
	int px = G.player.x;
	int py = G.player.y;
	int r = G.player.fov_radius;
	for (int vy = imax(0, py - r); vy <= imin(MAP_H - 1, py + r); ++vy) {
		for (int vx = imax(0, px - r); vx <= imin(MAP_W - 1, px + r); ++vx) {
			int ddx = vx - px;
			int ddy = vy - py;
			if (ddx * ddx + ddy * ddy > r * r + 1 || !line_clear(px, py, vx, vy)) { continue; }
			G.visible[idx(vx, vy)] = true;
			G.seen[idx(vx, vy)] = true;
		}
	}
}

function is_visible(int x, int y) -> bool { return in_bounds(x, y) && G.visible[idx(x, y)]; }

// ---------------------------------------------------------------------- BFS ---
// The first step of a shortest walkable path, four directions only (a facing-locked body cannot
// step diagonally), or null when the goal is unreachable. The autopilot knows the whole map.
function bfs_step(int sx, int sy, int tx, int ty) -> var {
	if (sx == tx && sy == ty) { return null; }
	int start = idx(sx, sy);
	int goal = idx(tx, ty);
	array<int> prev;
	for (int i = 0; i < MAP_W * MAP_H; ++i) { prev.push(-1); }
	prev[start] = start;
	array<int> queue = [start];
	int head = 0;
	while (head < queue.size()) {
		int cur = queue[head];
		head += 1;
		if (cur == goal) { break; }
		int cx = cur % MAP_W;
		int cy = cur / MAP_W;
		for (auto d : FOUR_DIRS) {
			int nx = cx + d.dx;
			int ny = cy + d.dy;
			if (!is_walkable(nx, ny)) { continue; }
			int ni = idx(nx, ny);
			if (prev[ni] != -1) { continue; }
			prev[ni] = cur;
			queue.push(ni);
		}
	}
	if (prev[goal] == -1) { return null; }
	int node = goal;
	while (prev[node] != start) { node = prev[node]; }
	return Step{node % MAP_W - sx, node / MAP_W - sy};
}
