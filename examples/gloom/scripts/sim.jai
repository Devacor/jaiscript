// Player simulation: movement with wall sliding, the 'use' action (doors,
// secrets, the exit switch), pickups.

// circle-vs-grid: all four corners of the actor's square must be on floor
function spot_free(float x, float y, float r) -> bool {
	if (tile_solid(tile_at(math::ifloor(x - r), math::ifloor(y - r)))) { return false; }
	if (tile_solid(tile_at(math::ifloor(x + r), math::ifloor(y - r)))) { return false; }
	if (tile_solid(tile_at(math::ifloor(x - r), math::ifloor(y + r)))) { return false; }
	if (tile_solid(tile_at(math::ifloor(x + r), math::ifloor(y + r)))) { return false; }
	return true;
}

function player_move(var& input) -> void {
	float turn = 0.0;
	if (input.tl) { turn -= 1.0; }
	if (input.tr) { turn += 1.0; }
	G.pang = G.pang + turn * 2.7 * TICK;
	while (G.pang > 6.2831853) { G.pang -= 6.2831853; }
	while (G.pang < 0.0) { G.pang += 6.2831853; }

	float dirx = math::cos(G.pang);
	float diry = math::sin(G.pang);
	float wx = 0.0;
	float wy = 0.0;
	if (input.fwd) { wx += dirx; wy += diry; }
	if (input.back) { wx -= dirx; wy -= diry; }
	if (input.sl) { wx += diry; wy -= dirx; }
	if (input.sr) { wx -= diry; wy += dirx; }
	float wl = math::sqrt(wx * wx + wy * wy);
	if (wl > 0.001) {
		bool run = input.run;
		float speed = (run ? 5.0 : 3.4) * TICK;
		wx = wx / wl * speed;
		wy = wy / wl * speed;
		float nx = G.px + wx;
		float ny = G.py + wy;
		bool moved = false;
		if (spot_free(nx, G.py, 0.26)) { G.px = nx; moved = true; }
		if (spot_free(G.px, ny, 0.26)) { G.py = ny; moved = true; }
		if (moved) {
			G.bob = G.bob + (run ? 0.30 : 0.22);
			check_pickups();
		}
	}
}

// ------------------------------------------------------------ 'use' ---------
// probe a few steps down the facing for the first non-floor tile
function player_use() -> void {
	float dirx = math::cos(G.pang);
	float diry = math::sin(G.pang);
	array<float> probes = [0.5, 0.9, 1.3];
	for (auto p : probes) {
		int tx = math::ifloor(G.px + dirx * p);
		int ty = math::ifloor(G.py + diry * p);
		int t = tile_at(tx, ty);
		if (t == 0) { continue; }
		use_tile(tx, ty, t);
		return;
	}
	show_msg("nothing to use", "90");
}

function use_tile(int tx, int ty, int t) -> void {
	if (t == 5) { open_door(tx, ty, "the door grinds open"); return; }
	if (t == 8) {
		if (G.key_r) { open_door(tx, ty, "RED lock released"); }
		else { show_msg("you need the RED keycard", "91"); }
		return;
	}
	if (t == 9) {
		if (G.key_b) { open_door(tx, ty, "BLUE lock released"); }
		else { show_msg("you need the BLUE keycard", "94"); }
		return;
	}
	if (t == 7) {
		G.tiles[ty * G.mw + tx] = 0;
		rebuild_mapstr();
		G.secrets = G.secrets + 1;
		fx_secret_glitter(tx + 0.5, ty + 0.5);
		show_msg("you found a secret!", "96");
		return;
	}
	if (t == 6) {
		G.mode = 2;      // tally
		G.mode_t = 0;
		show_msg("level complete", "92");
		return;
	}
	show_msg("solid rock. very solid.", "90");
}

function open_door(int tx, int ty, string msg) -> void {
	G.tiles[ty * G.mw + tx] = 0;
	rebuild_mapstr();
	fx_door_puff(tx + 0.5, ty + 0.5);
	G.noise = imax(G.noise, 10);
	show_msg(msg, "97");
}

// ------------------------------------------------------------ pickups -------
function give_ammo(int t, int n) -> bool {
	if (G.ammo[t] >= AMMO_MAX[t]) { return false; }
	G.ammo[t] = imin(AMMO_MAX[t], G.ammo[t] + n);
	return true;
}

function check_pickups() -> void {
	for (auto& it : G.items) {
		if (it.taken) { continue; }
		int kind = it.kind;
		if (dist2(G.px, G.py, it.x, it.y) > 0.36) { continue; }
		bool got = false;
		if (kind == 0) { if (G.hp < 100) { G.hp = imin(100, G.hp + 10); got = true; } }
		else if (kind == 1) { if (G.hp < 100) { G.hp = imin(100, G.hp + 25); got = true; } }
		else if (kind == 2) { if (G.armor < 100) { G.armor = imin(100, G.armor + 50); got = true; } }
		else if (kind == 3) { got = give_ammo(0, 20); }
		else if (kind == 4) { got = give_ammo(1, 8); }
		else if (kind == 5) { got = give_ammo(2, 30); }
		else if (kind == 6) { G.key_r = true; got = true; }
		else if (kind == 7) { G.key_b = true; got = true; }
		else if (kind == 8) {
			G.have[1] = 1;
			give_ammo(1, 8);
			G.weapon = 1;
			G.cooldown = 8;
			got = true;
			G.face_grin = 30;
		}
		else if (kind == 9) {
			G.have[2] = 1;
			give_ammo(2, 40);
			G.weapon = 2;
			G.cooldown = 8;
			got = true;
			G.face_grin = 30;
		}
		else if (kind == 10) { G.hp = imin(150, G.hp + 40); got = true; G.face_grin = 20; }
		if (got) {
			it.taken = true;
			show_msg(`picked up ${ITEM_DEFS[kind]["name"]}`, "93");
		}
	}
}
