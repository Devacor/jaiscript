// The episode: hand-authored maps, their boot-time validation, and the World:
// the tile grid and every question asked of it (collision, sight, paths, the
// raycast snapshot). Entities live on Game; World.load() spawns them through G
// as it parses.
//
// Map chars:  '#'.'%'.'='.'&'   wall kinds 1-4 (theme-colored)
//             'D' door   'R' red-locked door   'B' blue-locked door
//             'S' secret wall (looks like '#'; 'use' opens it)
//             'X' exit switch wall
//             '.' floor
// Spawns (replaced by floor): '@' player  'g' grunt  'z' spitter  't' turret
//             'W' warden   items: '1' stim '2' medkit '3' armor '4' clip
//             '5' shells '6' cells 'r' red key 'b' blue key '7' scattergun
//             '8' hexcaster '9' soul orb
//
// Tile ints:  0 floor, 1-4 walls, 5 door, 6 exit, 7 secret, 8 red, 9 blue.

var MAPS = [
	{
		"name": "E1M1: DIMLIT ANTECHAMBER",
		"par": 1650,
		"theme": {
			"walls": [[122, 108, 92], [96, 118, 88], [88, 96, 122], [130, 90, 74]],
			"ceil": [38, 34, 46], "floor": [64, 56, 44]
		},
		"rows": [
		"##########################",
		"#........#.....#.....%..4#",
		"#.@......D..g..#..1..%.g.#",
		"#........#.....#.....%...#",
		"#####D####..g..#..%%%%.%%#",
		"#...#....#.....#.........#",
		"#.1.#.g..##D####......g..#",
		"#...#....#.....###D#######",
		"##D###D###..7..#........=#",
		"#..#.....#.....S....2...=#",
		"#..#..g..#.....#........=#",
		"#.4#.....###########D###=#",
		"#..#...........#.....#..=#",
		"#..######D##.g.#..1..#..=#",
		"#......#...#...#.....#..=#",
		"#..2...D.5.#...D.....D..X#",
		"#......#...#...#.....#...#",
		"##########################"]
	},
	{
		"name": "E1M2: COOLANT WARRENS",
		"par": 2100,
		"theme": {
			"walls": [[86, 116, 128], [70, 100, 140], [96, 128, 100], [120, 100, 130]],
			"ceil": [26, 36, 46], "floor": [42, 56, 60]
		},
		"rows": [
		"#############################",
		"#.....#.......#......#......#",
		"#.@...#..g....#..z...#.66...#",
		"#.....D.......D......D......#",
		"#.....#....g..#......#....3.#",
		"#.....#.......#......#......#",
		"###D######D#############D####",
		"#.....#.......#......#......#",
		"#.5...#.......#......#......#",
		"#.....D.......S..9...#..r...#",
		"#..g..#...t...#...6..#......#",
		"#.....#.......#......#....z.#",
		"#.....#.......#......#......#",
		"###D######D#############R####",
		"#.....#.......#......#......#",
		"#.....#.......#..1...#......#",
		"#.2...D..z....D.g....#......X",
		"#..g..#....4..#....g.#....2.#",
		"#.....#.......#......#......#",
		"#############################"]
	},
	{
		"name": "E1M3: THE HEX FOUNDRY",
		"par": 2700,
		"theme": {
			"walls": [[140, 96, 60], [120, 70, 100], [100, 100, 60], [80, 110, 120]],
			"ceil": [44, 28, 24], "floor": [58, 46, 36]
		},
		"rows": [
		"#############################",
		"#.....%.......%......%......#",
		"#.@...%..g....%..z...B...8..#",
		"#.....D...&...D......%......#",
		"#.....%.......%..t...%.66.6.#",
		"#.....%.......%......%......#",
		"###D======D======D==========#",
		"#.....%.......%......%......#",
		"#.....%.&...&.%...3..%......#",
		"#..g..D...t...D..z...D......#",
		"#.....%.&...&.%...1..%....b.#",
		"#.....%.......%......%..z...#",
		"###D======D=============D===#",
		"#.....%.......%......%......#",
		"#.....%.......%...6..%...z..#",
		"#..z..%.......S..9...%......#",
		"#.5...%.2.....%......%..r...#",
		"#.....%.......%......%......#",
		"###D======D=================#",
		"#.....%....4..%......%......#",
		"#.2...%..g....%..1...R......X",
		"#..g..%....g..D....g.%..2...#",
		"#.....%.......%......%......#",
		"#############################"]
	},
	{
		"name": "E1M4: GLOOM'S THRONE",
		"par": 2400,
		"theme": {
			"walls": [[150, 60, 50], [100, 44, 40], [130, 110, 60], [70, 50, 60]],
			"ceil": [40, 18, 18], "floor": [52, 34, 30]
		},
		"rows": [
		"##########################",
		"#.....#..........#.......#",
		"#.@.8.D....66....D...9...#",
		"#.....#....55....#.......#",
		"#..2..#....22....#...3...#",
		"#.....#..........#.......#",
		"###D######....######D#####",
		"#...#....#....#....#.....#",
		"#.6.#.g..D....D..g.#..5..#",
		"#...#....#....#....#.....#",
		"#.t.#....#....#....#..t..#",
		"#...######....######.....#",
		"#........................#",
		"#..&&&...................#",
		"#..&......W..............#",
		"#..&&&...................#",
		"#........................#",
		"#####........###......####",
		"#..2#........#9#......#X.#",
		"#...#........#.#......D..#",
		"#...##########.########..#",
		"#........................#",
		"##########################"]
	}
];

// -------------------------------------------------------------- parsing -----
function wall_kind_for(string ch) -> int {
	if (ch == "#") { return 1; }
	if (ch == "%") { return 2; }
	if (ch == "=") { return 3; }
	if (ch == "&") { return 4; }
	if (ch == "D") { return 5; }
	if (ch == "X") { return 6; }
	if (ch == "S") { return 7; }
	if (ch == "R") { return 8; }
	if (ch == "B") { return 9; }
	return 0;
}

function item_kind_for(string ch) -> int {
	if (ch == "1") { return 0; }
	if (ch == "2") { return 1; }
	if (ch == "3") { return 2; }
	if (ch == "4") { return 3; }
	if (ch == "5") { return 4; }
	if (ch == "6") { return 5; }
	if (ch == "r") { return 6; }
	if (ch == "b") { return 7; }
	if (ch == "7") { return 8; }
	if (ch == "8") { return 9; }
	if (ch == "9") { return 10; }
	return -1;
}

// a pickup on the floor
class Item {
public:
	int kind = 0;
	float x = 0.0;
	float y = 0.0;
	bool taken = false;

	Item(int kind_, float x_, float y_) {
		kind = kind_;
		x = x_;
		y = y_;
	}
}

// raycast snapshot: render kinds packed 15-per-int64 (4 bits each), rebuilt
// only when a tile changes. Kind 0 floor/open, 1-4 walls, 5 door, 6 exit,
// 7 red, 8 blue; secrets render as wall kind 1 (that is the point of a secret).
array<int> TILE_TO_KIND = [0, 1, 2, 3, 4, 5, 6, 1, 7, 8];

// the four steps between neighboring tiles
array<math::ivec2> NEIGHBORS = [math::ivec2(1, 0), math::ivec2(-1, 0), math::ivec2(0, 1), math::ivec2(0, -1)];

// ------------------------------------------------------------ the World -----
class World {
public:
	string map_name = "";
	int par = 0;
	int mw = 0;
	int mh = 0;
	array<int> tiles = [];
	array<int> mappack = [];   // 15 tiles per int64, 4-bit render kinds
	int mapgen = 0;            // bumps with every mappack rebuild

	// parse MAPS[mi]: tiles into self, spawns into G (enemies/items/player pose)
	void load(int mi) {
		array<string> rows = MAPS[mi]["rows"];
		map_name = MAPS[mi]["name"];
		par = MAPS[mi]["par"];
		mh = rows.size();
		mw = rows[0].size();
		tiles = [];
		for (int y = 0; y < mh; ++y) {
			string row = rows[y];
			for (int x = 0; x < mw; ++x) {
				string ch = row.substr(x, 1);
				int wk = wall_kind_for(ch);
				if (wk == 7) { G.secret_total = G.secret_total + 1; }
				if (wk > 0) { tiles.push(wk); continue; }
				tiles.push(0);
				if (ch == "@") { G.player.place(x + 0.5, y + 0.5); }
				else if (ch == "g") { G.spawn_enemy(0, x, y); }
				else if (ch == "z") { G.spawn_enemy(1, x, y); }
				else if (ch == "t") { G.spawn_enemy(2, x, y); }
				else if (ch == "W") { G.spawn_enemy(3, x, y); }
				else {
					int ik = item_kind_for(ch);
					if (ik >= 0) { G.items.push(new Item(ik, x + 0.5, y + 0.5)); }
				}
			}
		}
		rebuild_mappack();
	}

	void rebuild_mappack() {
		mappack = packed_from(tiles, mw * mh);
		mapgen = mapgen + 1;
	}

	int tile_at(int x, int y) {
		if (x < 0 || y < 0 || x >= mw || y >= mh) { return 1; }
		return tiles[y * mw + x];
	}

	bool solid_at(int x, int y) { return tile_at(x, y) != 0; }

	// walkable for actors: floor only (doors must be opened first)
	bool walkable(int x, int y) { return tile_at(x, y) == 0; }

	// circle-vs-grid: all four corners of the actor's square must be on floor
	bool spot_free(float x, float y, float r) {
		if (solid_at(math::ifloor(x - r), math::ifloor(y - r))) { return false; }
		if (solid_at(math::ifloor(x + r), math::ifloor(y - r))) { return false; }
		if (solid_at(math::ifloor(x - r), math::ifloor(y + r))) { return false; }
		if (solid_at(math::ifloor(x + r), math::ifloor(y + r))) { return false; }
		return true;
	}

	// stepped-ray LOS on the tile grid (deterministic float march)
	bool los_clear(float ax, float ay, float bx, float by) {
		float dx = bx - ax;
		float dy = by - ay;
		float len = math::sqrt(dx * dx + dy * dy);
		if (len < 0.001) { return true; }
		float inv = 1.0 / len;
		dx = dx * inv;
		dy = dy * inv;
		float t = 0.15;
		while (t < len) {
			if (solid_at(math::ifloor(ax + dx * t), math::ifloor(ay + dy * t))) { return false; }
			t += 0.15;
		}
		return true;
	}

	// wall distance along a unit ray: the renderer's DDA (pure.jai), called serially
	float wall_dist(float ax, float ay, float dx, float dy) {
		var rec = [ax, ay, dx, dy, mw];
		for (auto word : mappack) { rec.push(word); }
		var hit = gloom_ray(rec);
		float distance = hit[0];
		return distance;
	}

	// doors and secrets open by becoming floor
	void clear_tile(int tx, int ty) {
		tiles[ty * mw + tx] = 0;
		rebuild_mappack();
	}

	// BFS distances FROM the goal, over floor + doors the given keys can open
	// (+ the goal tile itself so walls like secrets/exit are approachable)
	array<int> bfs_field(math::ivec2 goal, bool key_r, bool key_b) {
		array<int> field = [];
		for (int i = 0; i < mw * mh; ++i) { field.push(-1); }
		if (goal.x < 0) { return field; }
		array<math::ivec2> queue = [goal];
		field[goal.y * mw + goal.x] = 0;
		int head = 0;
		while (head < queue.size()) {
			math::ivec2 cur = queue[head];
			head = head + 1;
			int steps = field[cur.y * mw + cur.x];
			for (auto& step : NEIGHBORS) {
				math::ivec2 next = cur + step;
				if (next.x < 0 || next.y < 0 || next.x >= mw || next.y >= mh) { continue; }
				int cell = next.y * mw + next.x;
				if (field[cell] >= 0) { continue; }
				int t = tiles[cell];
				bool pass = t == 0 || t == 5 || (t == 8 && key_r) || (t == 9 && key_b);
				if (!pass) { continue; }
				field[cell] = steps + 1;
				queue.push(next);
			}
		}
		return field;
	}

private:
	array<int> packed_from(array<int>& src, int n) {
		array<int> pack = [];
		int acc = 0;
		int slot = 0;
		for (int i = 0; i < n; ++i) {
			acc = acc | (TILE_TO_KIND[src[i]] << (slot * 4));
			slot = slot + 1;
			if (slot == 15) {
				pack.push(acc);
				acc = 0;
				slot = 0;
			}
		}
		if (slot > 0) { pack.push(acc); }
		return pack;
	}
}

// ----------------------------------------------------------- validation -----
// Boot-time checks of the raw map data: even widths, a sealed border, one '@',
// doors flanked by floor on one axis, every floor tile and the exit switch
// reachable from the start (keyed doors and secrets count as passable). A
// broken map throws: it would break the determinism battery.
function validate_map(int mi) -> void {
	array<string> rows = MAPS[mi]["rows"];
	int h = rows.size();
	int w = rows[0].size();
	int start_count = 0;
	math::ivec2 start;
	for (int y = 0; y < h; ++y) {
		if (rows[y].size() != w) {
			throw `map ${mi} row ${y}: width ${rows[y].size()} != ${w}`;
		}
		for (int x = 0; x < w; ++x) {
			string ch = rows[y].substr(x, 1);
			bool border = x == 0 || y == 0 || x == w - 1 || y == h - 1;
			if (border && wall_kind_for(ch) == 0) {
				throw `map ${mi} (${x},${y}): border leak '${ch}'`;
			}
			if (ch == "@") {
				start_count = start_count + 1;
				start = math::ivec2(x, y);
			}
			if (ch == "D" || ch == "R" || ch == "B") {
				bool open_ns = wall_kind_for(rows[y - 1].substr(x, 1)) == 0 && wall_kind_for(rows[y + 1].substr(x, 1)) == 0;
				bool open_ew = wall_kind_for(rows[y].substr(x - 1, 1)) == 0 && wall_kind_for(rows[y].substr(x + 1, 1)) == 0;
				if (!open_ns && !open_ew) {
					throw `map ${mi} (${x},${y}): door not flanked by floor`;
				}
			}
		}
	}
	if (start_count != 1) { throw `map ${mi}: ${start_count} player starts`; }

	array<bool> reached = [];
	for (int i = 0; i < w * h; ++i) { reached.push(false); }
	array<math::ivec2> queue = [start];
	reached[start.y * w + start.x] = true;
	int head = 0;
	while (head < queue.size()) {
		math::ivec2 cur = queue[head];
		head = head + 1;
		for (auto& step : NEIGHBORS) {
			math::ivec2 next = cur + step;
			if (next.x < 0 || next.y < 0 || next.x >= w || next.y >= h) { continue; }
			if (reached[next.y * w + next.x]) { continue; }
			int wk = wall_kind_for(rows[next.y].substr(next.x, 1));
			if (wk != 0 && wk != 5 && wk != 7 && wk != 8 && wk != 9) { continue; }
			reached[next.y * w + next.x] = true;
			queue.push(next);
		}
	}
	bool exit_reached = false;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			string ch = rows[y].substr(x, 1);
			if (wall_kind_for(ch) == 0 && !reached[y * w + x]) {
				throw `map ${mi} (${x},${y}): unreachable floor '${ch}'`;
			}
			if (ch != "X") { continue; }
			for (auto& step : NEIGHBORS) {
				math::ivec2 next = math::ivec2(x, y) + step;
				if (next.x >= 0 && next.y >= 0 && next.x < w && next.y < h && reached[next.y * w + next.x]) {
					exit_reached = true;
				}
			}
		}
	}
	if (!exit_reached) { throw `map ${mi}: exit switch unreachable`; }
}

function validate_all_maps() -> void {
	for (int mi = 0; mi < MAPS.size(); ++mi) { validate_map(mi); }
}
