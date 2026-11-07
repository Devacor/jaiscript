// The episode: hand-authored maps + parser + boot-time validation.
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
// Tile ints (G.tiles): 0 floor, 1-4 walls, 5 door, 6 exit, 7 secret,
//                      8 red door, 9 blue door.

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
		"#.@...D....66....D...9...#",
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

// parse MAPS[mi] into G: tiles, dimensions, spawns. Entities land in G.enemies
// (shared_ptr handles via `new`), items/doors as value records.
function load_map(int mi) -> void {
	var rows = MAPS[mi]["rows"];
	G.map_name = MAPS[mi]["name"];
	G.par = MAPS[mi]["par"];
	G.mh = rows.size();
	G.mw = rows[0].size();
	G.tiles = [];
	G.enemies = [];
	G.items = [];
	G.shots = [];
	G.kills = 0;
	G.kill_total = 0;
	G.secrets = 0;
	G.secret_total = 0;
	G.map_ticks = 0;
	for (int y = 0; y < G.mh; ++y) {
		string row = rows[y];
		for (int x = 0; x < G.mw; ++x) {
			string ch = row.substr(x, 1);
			int wk = wall_kind_for(ch);
			if (wk == 7) { G.secret_total = G.secret_total + 1; }
			if (wk > 0) { G.tiles.push(wk); continue; }
			G.tiles.push(0);
			if (ch == "@") {
				G.px = x + 0.5;
				G.py = y + 0.5;
				G.pang = 0.0;
			} else if (ch == "g") { spawn_enemy(0, x, y); }
			else if (ch == "z") { spawn_enemy(1, x, y); }
			else if (ch == "t") { spawn_enemy(2, x, y); }
			else if (ch == "W") { spawn_enemy(3, x, y); }
			else {
				int ik = item_kind_for(ch);
				if (ik >= 0) { G.items.push(new Item(ik, x + 0.5, y + 0.5)); }
			}
		}
	}
	rebuild_mapstr();
	build_render_tables(mi);
}

// raycast snapshot: render kinds packed 15-per-int64 (4 bits each), rebuilt
// only when a tile changes. Kind 0 floor/open, 1-4 walls, 5 door, 6 exit,
// 7 red, 8 blue; secrets render as wall kind 1 (that is the point of a secret).
array<int> TILE_TO_KIND = [0, 1, 2, 3, 4, 5, 6, 1, 7, 8];

function rebuild_mapstr() -> void {
	G.mappack = mappack_from(G.tiles, G.mw * G.mh);
	G.mapgen = G.mapgen + 1;
}

function mappack_from(var& tiles, int n) -> array<int> {
	array<int> pack = [];
	int acc = 0;
	int slot = 0;
	for (int i = 0; i < n; ++i) {
		acc = acc | (TILE_TO_KIND[tiles[i]] << (slot * 4));
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

// flat ray record for serial gloom_ray calls (hitscan, tracer garnish)
function ray_record(float ax, float ay, float dx, float dy) -> var {
	var rec = [ax, ay, dx, dy, G.mw];
	for (auto wv : G.mappack) { rec.push(wv); }
	return rec;
}

function tile_at(int x, int y) -> int {
	if (x < 0 || y < 0 || x >= G.mw || y >= G.mh) { return 1; }
	return G.tiles[y * G.mw + x];
}

function tile_solid(int t) -> bool { return t != 0; }

// walkable for actors: floor only (doors must be opened first)
function walkable(int x, int y) -> bool { return tile_at(x, y) == 0; }

// ----------------------------------------------------------- validation -----
// Boot-time sanity: consistent widths, sealed border, one '@', doors flanked
// by floor on one axis, and (with keyed doors treated passable) every floor
// tile reachable from the start. Throws on structural errors: a broken map is
// a broken determinism battery.
function validate_map(int mi) -> void {
	var rows = MAPS[mi]["rows"];
	var h = rows.size();
	var w = rows[0].size();
	var start_count = 0;
	for (int y = 0; y < h; ++y) {
		if (rows[y].size() != w) {
			throw `map ${mi} row ${y}: width ${rows[y].size()} != ${w}`;
		}
		for (int x = 0; x < w; ++x) {
			var ch = rows[y].substr(x, 1);
			var border = x == 0 || y == 0 || x == w - 1 || y == h - 1;
			if (border && wall_kind_for(ch) == 0) {
				throw `map ${mi} (${x},${y}): border leak '${ch}'`;
			}
			if (ch == "@") { start_count = start_count + 1; }
			if (ch == "D" || ch == "R" || ch == "B") {
				var open_ns = wall_kind_for(rows[y - 1].substr(x, 1)) == 0 && wall_kind_for(rows[y + 1].substr(x, 1)) == 0;
				var open_ew = wall_kind_for(rows[y].substr(x - 1, 1)) == 0 && wall_kind_for(rows[y].substr(x + 1, 1)) == 0;
				if (!open_ns && !open_ew) {
					throw `map ${mi} (${x},${y}): door not flanked by floor`;
				}
			}
		}
	}
	if (start_count != 1) { throw `map ${mi}: ${start_count} player starts`; }

	// reachability: flood from '@' through floor/doors/secrets; every walkable
	// char and every 'X' neighbor must be reached
	var sx = 0;
	var sy = 0;
	var reach = [];
	for (int i = 0; i < w * h; ++i) { reach.push(0); }
	for (int y2 = 0; y2 < h; ++y2) {
		for (int x2 = 0; x2 < w; ++x2) {
			if (rows[y2].substr(x2, 1) == "@") { sx = x2; sy = y2; }
		}
	}
	var queue = [[sx, sy]];
	reach[sy * w + sx] = 1;
	var head = 0;
	while (head < queue.size()) {
		var cur = queue[head];
		head = head + 1;
		int cx = cur[0];
		int cy = cur[1];
		var dirs = [[1, 0], [-1, 0], [0, 1], [0, -1]];
		for (auto& d : dirs) {
			int nx = cx + d[0];
			int ny = cy + d[1];
			if (nx < 0 || ny < 0 || nx >= w || ny >= h) { continue; }
			var already = reach[ny * w + nx];
			if (already == 1) { continue; }
			var ch2 = rows[ny].substr(nx, 1);
			var wk = wall_kind_for(ch2);
			// doors (any lock) and secrets are traversable for reachability
			if (wk != 0 && wk != 5 && wk != 7 && wk != 8 && wk != 9) { continue; }
			reach[ny * w + nx] = 1;
			queue.push([nx, ny]);
		}
	}
	var exit_reached = false;
	for (int y3 = 0; y3 < h; ++y3) {
		for (int x3 = 0; x3 < w; ++x3) {
			var ch3 = rows[y3].substr(x3, 1);
			var wk3 = wall_kind_for(ch3);
			var r = reach[y3 * w + x3];
			if (wk3 == 0 && r == 0) {
				throw `map ${mi} (${x3},${y3}): unreachable floor '${ch3}'`;
			}
			if (ch3 == "X") {
				var dirs2 = [[1, 0], [-1, 0], [0, 1], [0, -1]];
				for (auto& d2 : dirs2) {
					int ax = x3 + d2[0];
					int ay = y3 + d2[1];
					if (ax < 0 || ay < 0 || ax >= w || ay >= h) { continue; }
					var ar = reach[ay * w + ax];
					if (ar == 1) { exit_reached = true; }
				}
			}
		}
	}
	if (!exit_reached) { throw `map ${mi}: exit switch unreachable`; }
}

function validate_all_maps() -> void {
	for (int mi = 0; mi < MAPS.size(); ++mi) { validate_map(mi); }
}
