// Game: owns the subsystems (World, Player, ParticleSystem, Renderer, Hud,
// Pilot), the entity registries and the episode's flow, and runs the
// fixed-timestep tick. G, declared under the class, is a typed shared_ptr:
// every use of it reaches the one world.

// one tick of intent, from the keyboard or the autopilot
class Input {
public:
	bool fwd = false;
	bool back = false;
	bool sl = false;
	bool sr = false;
	bool tl = false;
	bool tr = false;
	bool run = false;
	bool fire = false;
	bool use = false;
	int weapon = -1;
	bool start = false;
}

class Game {
public:
	// console geometry
	int w = 100;
	int h = 40;

	// flow
	int mode = 0;          // 0 title, 1 play, 2 tally, 3 dead, 4 victory
	int mode_t = 0;
	int tick = 0;
	float accum = 0.0;
	bool quit = false;
	bool autopilot = false;
	string frame_key = "";
	int hash = 2166136261;
	int map_i = 0;

	// subsystems
	shared_ptr<World> world = null;
	shared_ptr<Player> player = null;
	shared_ptr<ParticleSystem> fx = null;
	shared_ptr<Renderer> renderer = null;
	shared_ptr<Hud> hud = null;
	shared_ptr<Pilot> pilot = null;

	// what is on the floor
	array<shared_ptr<Enemy>> enemies = [];
	array<shared_ptr<Item>> items = [];
	array<shared_ptr<Shot>> shots = [];
	int noise = 0;         // alertness: gunfire/doors wake nearby enemies
	bool warden_down = false;

	// tallies
	int kills = 0;
	int kill_total = 0;
	int secrets = 0;
	int secret_total = 0;
	int map_ticks = 0;
	int ep_kills = 0;
	int ep_kill_total = 0;
	int ep_secrets = 0;
	int ep_secret_total = 0;
	int ep_ticks = 0;
	int deaths = 0;

	shared_ptr<Input> input = null;   // this tick's

	Game(int w_, int h_) {
		w = w_;
		h = h_;
		world = new World();
		player = new Player();
		fx = new ParticleSystem();
		renderer = new Renderer(w_, h_);
		hud = new Hud();
		pilot = new Pilot();
	}

	// ------------------------------------------------------ episode flow ----
	void start_episode() {
		map_i = 0;
		ep_kills = 0;
		ep_kill_total = 0;
		ep_secrets = 0;
		ep_secret_total = 0;
		ep_ticks = 0;
		deaths = 0;
		player.reset_vitals();
		begin_map();
	}

	void begin_map() {
		player.key_r = false;
		player.key_b = false;
		shots = [];
		fx.reset_pool();
		load_map(map_i);
		mode = 1;
		mode_t = 0;
		hud.show(world.map_name, "97");
	}

	void load_map(int mi) {
		enemies = [];
		items = [];
		shots = [];
		kills = 0;
		kill_total = 0;
		secrets = 0;
		secret_total = 0;
		map_ticks = 0;
		world.load(mi);
		renderer.build_tables(mi);
	}

	void finish_map() {
		ep_kills = ep_kills + kills;
		ep_kill_total = ep_kill_total + kill_total;
		ep_secrets = ep_secrets + secrets;
		ep_secret_total = ep_secret_total + secret_total;
		ep_ticks = ep_ticks + map_ticks;
		map_i = map_i + 1;
		if (map_i >= MAPS.size()) {
			mode = 4;
			mode_t = 0;
		} else {
			begin_map();
		}
	}

	void complete_level() {
		mode = 2;      // tally
		mode_t = 0;
		hud.show("level complete", "92");
	}

	// --------------------------------------------------------- factories ----
	void spawn_enemy(int kind, int tx, int ty) {
		float ex = tx + 0.5;
		float ey = ty + 0.5;
		if (kind == 0) { enemies.push(new Grunt(ex, ey)); }
		else if (kind == 1) { enemies.push(new Spitter(ex, ey)); }
		else if (kind == 2) { enemies.push(new Turret(ex, ey)); }
		else { enemies.push(new Warden(ex, ey)); }
		kill_total = kill_total + 1;
	}

	void spawn_shot(int kind, float ax, float ay, float tx, float ty, float speed) {
		float dx = tx - ax;
		float dy = ty - ay;
		float len = math::sqrt(dx * dx + dy * dy);
		if (len < 0.001) { return; }
		shots.push(new Shot(kind, ax + dx / len * 0.4, ay + dy / len * 0.4, dx / len * speed, dy / len * speed));
	}

	void spawn_shot_dir(int kind, float ax, float ay, float dx, float dy, float speed) {
		shots.push(new Shot(kind, ax + dx * 0.45, ay + dy * 0.45, dx * speed, dy * speed));
	}

	// this tick's intent, from the keyboard or the autopilot
	shared_ptr<Input> gather_input() {
		if (autopilot) { return pilot.next(); }
		shared_ptr<Input> keys = new Input();
		keys.fwd = key_down("w") || key_down("up");
		keys.back = key_down("s") || key_down("down");
		keys.sl = key_down("a");
		keys.sr = key_down("d");
		keys.tl = key_down("left");
		keys.tr = key_down("right");
		keys.run = key_down("shift");
		keys.fire = key_down("space") || key_down("ctrl");
		keys.use = frame_key == "e";
		keys.weapon = frame_key == "1" ? 0 : (frame_key == "2" ? 1 : (frame_key == "3" ? 2 : -1));
		keys.start = frame_key == " " || frame_key == "enter" || keys.fire;
		return keys;
	}

	// ------------------------------------------------------------ the tick --
	void run_tick() {
		tick = tick + 1;
		input = gather_input();
		bool start_pressed = input.start;
		if (mode == 0) {
			mode_t = mode_t + 1;
			if (start_pressed && mode_t > 8) { start_episode(); }
			accumulate_hash();
			return;
		}
		if (mode == 2) {
			mode_t = mode_t + 1;
			if (start_pressed && mode_t > 25) { finish_map(); }
			accumulate_hash();
			return;
		}
		if (mode == 3) {
			mode_t = mode_t + 1;
			if (start_pressed && mode_t > 25) {
				deaths = deaths + 1;
				player.reset_vitals();
				begin_map();       // the floor repopulates; the Well remembers nothing
			}
			accumulate_hash();
			return;
		}
		if (mode == 4) {
			mode_t = mode_t + 1;
			accumulate_hash();
			return;
		}

		// --- play ---
		int wsel = input.weapon;
		if (wsel >= 0 && player.have[wsel] == 1 && wsel != player.weapon) {
			player.weapon = wsel;
			player.cooldown = 8;
			hud.show(`${WEAPONS[wsel].name} ready`, "97");
		}
		if (frame_key == "m") { renderer.automap = !renderer.automap; }
		if (input.use) { player.use_action(); }
		player.move(input);
		if (input.fire) { player.fire(); }

		player.tick_timers();
		if (noise > 0) { noise = noise - 1; }
		hud.update();

		for (auto& e : enemies) { e.tick(); }
		update_shots();
		fx.update();
		map_ticks = map_ticks + 1;
		accumulate_hash();
	}

	void update_shots() {
		array<shared_ptr<Shot>> kept = [];
		for (int i = 0; i < shots.size(); ++i) {
			if (shots[i].tick()) { kept.push(shots[i]); }
		}
		shots = kept;
		// glow trails
		for (int i = 0; i < shots.size(); ++i) {
			auto& s = shots[i];
			if ((tick + i) % 2 == 0) { fx.spawn(s.kind == 2 ? 1 : 6, s.x, s.y, 0.5, 0.0, 0.0, 0.0, 4); }
		}
	}

	void accumulate_hash() {
		int hh = hash;
		hh = mix32(hh, tick);
		hh = mix32(hh, mode * 31 + map_i);
		hh = mix32(hh, math::itrunc(player.px * 256.0) * 4096 + math::itrunc(player.py * 256.0));
		hh = mix32(hh, math::itrunc(player.pang * 1024.0));
		hh = mix32(hh, player.hp * 512 + player.armor);
		hh = mix32(hh, player.ammo[0] * 65536 + player.ammo[1] * 256 + player.ammo[2]);
		hh = mix32(hh, player.weapon * 64 + (player.key_r ? 2 : 0) + (player.key_b ? 1 : 0));
		int live = 0;
		int acc = 0;
		for (auto& e : enemies) {
			if (!e.alive) { continue; }
			live = live + 1;
			acc = (acc + math::itrunc(e.x * 64.0) * 977 + math::itrunc(e.y * 64.0) * 331 + e.hp * 7) & 0xFFFFFFFF;
		}
		hh = mix32(hh, live);
		hh = mix32(hh, acc);
		int pacc = 0;
		for (auto& p : fx.pool) {
			if (p.kind == 0) { continue; }
			pacc = (pacc + p.kind * 131 + (math::itrunc(p.x * 16.0) * 61 + math::itrunc(p.y * 16.0)) * 17 + p.life) & 0xFFFFFFFF;
		}
		hh = mix32(hh, pacc);
		hh = mix32(hh, shots.size() * 8191 + kills * 127 + secrets * 31);
		hh = mix32(hh, RNG.state());
		hash = hh;
	}
}

// ------------------------------------------------------------ autopilot -----
// One coroutine, one yield per tick, every decision from game state and RNG.
// It fights what it sees, loots what remains, hunts the secrets it
// "remembers", opens what its keys allow, and pulls the exit switch.
class Pilot {
public:
	shared_ptr<Input> next() {
		if (brain == null || brain.done()) { brain = think(); }
		return brain.next();
	}

private:
	var brain = null;

	coroutine function think() -> var {
		array<int> dist_field = [];
		int field_age = 999;
		math::ivec2 goal = math::ivec2(-1, -1);
		int strafe_dir = 1;
		int strafe_t = 0;
		while (true) {
			if (G.mode != 1) {
				yield command(false, false, false, false, 0, false, false, -1, true);
				continue;
			}

			// --- combat: nearest visible enemy inside 12 tiles. Below 40 hp the
			// pilot disengages unless cornered — navigation will chase medkits.
			bool desperate = G.player.hp < 40;
			bool found = false;
			float tgt_x = 0.0;
			float tgt_y = 0.0;
			float best_d = 12.0;
			for (auto& e : G.enemies) {
				if (!e.alive) { continue; }
				float d = math::sqrt(dist2(e.x, e.y, G.player.px, G.player.py));
				if (d < best_d && G.world.los_clear(G.player.px, G.player.py, e.x, e.y)) {
					best_d = d;
					tgt_x = e.x;
					tgt_y = e.y;
					found = true;
				}
			}
			if (found && desperate && best_d > 3.0) { found = false; }
			if (found) {
				float want = math::atan2(tgt_y - G.player.py, tgt_x - G.player.px);
				float da = ang_diff(G.player.pang, want);
				int turn = 0;
				if (da > 0.05) { turn = 1; }
				if (da < -0.05) { turn = -1; }
				// weapon sense: scatter close, hex for crowds/far, pistol default
				int wsel = -1;
				if (G.player.have[1] == 1 && G.player.ammo[1] > 0 && best_d < 6.0) { wsel = 1; }
				else if (G.player.have[2] == 1 && G.player.ammo[2] > 0 && best_d > 3.5) { wsel = 2; }
				else if (G.player.ammo[0] > 0) { wsel = 0; }
				else if (G.player.have[1] == 1 && G.player.ammo[1] > 0) { wsel = 1; }
				else if (G.player.have[2] == 1 && G.player.ammo[2] > 0) { wsel = 2; }
				if (wsel == G.player.weapon) { wsel = -1; }
				bool aligned = da < 0.14 && da > -0.14;
				bool fire = aligned && G.player.cooldown == 0 && best_d > 1.2;
				// footwork: back off rushers, strafe under fire
				bool back = best_d < 2.4;
				bool fwd = best_d > 8.5 && aligned;
				strafe_t = strafe_t + 1;
				if (strafe_t > 18) {
					strafe_t = 0;
					if (RNG.chance(60)) { strafe_dir = 0 - strafe_dir; }
				}
				yield command(fwd, back, strafe_dir < 0, strafe_dir > 0, turn, fire, false, wsel, false);
				continue;
			}

			// --- navigate: pick a goal, BFS-from-goal field, walk downhill.
			// COMMIT to the goal until arrival; the field alone refreshes
			// periodically so newly opened doors shorten the route; a stale
			// valve breaks any pathological commitment.
			field_age = field_age + 1;
			bool goal_gone = goal.x < 0 || field_age > 240;
			if (!goal_gone && G.world.tile_at(goal.x, goal.y) == 0
					&& math::ifloor(G.player.px) == goal.x && math::ifloor(G.player.py) == goal.y) {
				goal_gone = true;     // stood on it
			}
			if (goal_gone) {
				goal = pick_goal();
				dist_field = G.world.bfs_field(goal, G.player.key_r, G.player.key_b);
				field_age = 0;
			} else if (field_age % 45 == 0) {
				dist_field = G.world.bfs_field(goal, G.player.key_r, G.player.key_b);
			}
			if (goal.x < 0) {
				// nothing to want: spin slowly (a lost pilot is still deterministic)
				yield command(false, false, false, false, 1, false, false, -1, false);
				continue;
			}
			int mw = G.world.mw;
			int mh = G.world.mh;
			int ptx = math::ifloor(G.player.px);
			int pty = math::ifloor(G.player.py);
			int here = dist_field[pty * mw + ptx];
			if (here < 0) {
				// goal unreachable from here (shouldn't happen): drop it
				goal = math::ivec2(-1, -1);
				yield command(false, false, false, false, 0, false, false, -1, false);
				continue;
			}
			// pick the walkable-or-door neighbor closest to the goal
			int nx = ptx;
			int ny = pty;
			int bestv = here;
			for (auto& step : NEIGHBORS) {
				int cx = ptx + step.x;
				int cy = pty + step.y;
				if (cx < 0 || cy < 0 || cx >= mw || cy >= mh) { continue; }
				int v = dist_field[cy * mw + cx];
				if (v >= 0 && v < bestv) {
					bestv = v;
					nx = cx;
					ny = cy;
				}
			}
			float wx = nx + 0.5;
			float wy = ny + 0.5;
			float want2 = math::atan2(wy - G.player.py, wx - G.player.px);
			float da2 = ang_diff(G.player.pang, want2);
			int turn2 = 0;
			if (da2 > 0.08) { turn2 = 1; }
			if (da2 < -0.08) { turn2 = -1; }
			bool fwd2 = da2 < 0.6 && da2 > -0.6;
			// a door/secret in the way? use it when close and mostly facing
			int front_t = G.world.tile_at(nx, ny);
			bool use2 = false;
			if (front_t != 0 && dist2(G.player.px, G.player.py, wx, wy) < 2.1 && fwd2) { use2 = true; }
			yield command(fwd2, false, false, false, turn2, false, use2, -1, false);
		}
	}

	shared_ptr<Input> command(bool fwd, bool back, bool sl, bool sr, int turn, bool fire, bool use, int wsel, bool start) {
		shared_ptr<Input> intent = new Input();
		intent.fwd = fwd;
		intent.back = back;
		intent.sl = sl;
		intent.sr = sr;
		intent.tl = turn < 0;
		intent.tr = turn > 0;
		intent.run = true;
		intent.fire = fire;
		intent.use = use;
		intent.weapon = wsel;
		intent.start = start;
		return intent;
	}

	// goal priority: medkits when hurting -> live enemies -> loot -> unfound
	// secrets (the pilot "remembers the walls") -> keyed doors -> the exit
	math::ivec2 pick_goal() {
		if (G.player.hp < 55) {
			math::ivec2 med = nearest_item([0, 1, 10]);
			if (med.x >= 0) { return med; }
		}
		math::ivec2 hunt = math::ivec2(-1, -1);
		float hd = 9999.0;
		for (auto& e : G.enemies) {
			if (!e.alive) { continue; }
			float d = dist2(e.x, e.y, G.player.px, G.player.py);
			if (d < hd) {
				hd = d;
				hunt = math::ivec2(math::ifloor(e.x), math::ifloor(e.y));
			}
		}
		if (hunt.x >= 0) { return hunt; }
		math::ivec2 loot = nearest_item([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
		if (loot.x >= 0) { return loot; }
		if (G.secrets < G.secret_total) {
			math::ivec2 secret = nearest_secret();
			if (secret.x >= 0) { return secret; }
		}
		return exit_tile();
	}

	math::ivec2 nearest_item(array<int> kinds) {
		math::ivec2 best = math::ivec2(-1, -1);
		float bd = 9999.0;
		for (auto& it : G.items) {
			if (it.taken) { continue; }
			int k = it.kind;
			if (!kinds.contains(k)) { continue; }
			// skip anything the pickup code would refuse — otherwise the pilot
			// orbits a stimpack it can't absorb until the heat death of the run
			if ((k == 0 || k == 1) && G.player.hp >= 100) { continue; }
			if (k == 10 && G.player.hp >= 150) { continue; }
			if (k == 2 && G.player.armor >= 100) { continue; }
			if (k == 3 && G.player.ammo[0] >= AMMO_MAX[0]) { continue; }
			if (k == 4 && G.player.ammo[1] >= AMMO_MAX[1]) { continue; }
			if (k == 5 && G.player.ammo[2] >= AMMO_MAX[2]) { continue; }
			// keys we already carry are not loot
			if (k == 6 && G.player.key_r) { continue; }
			if (k == 7 && G.player.key_b) { continue; }
			float d = dist2(it.x, it.y, G.player.px, G.player.py);
			if (d < bd) {
				bd = d;
				best = math::ivec2(math::ifloor(it.x), math::ifloor(it.y));
			}
		}
		return best;
	}

	math::ivec2 nearest_secret() {
		math::ivec2 best = math::ivec2(-1, -1);
		float bd = 9999.0;
		for (int ty = 0; ty < G.world.mh; ++ty) {
			for (int tx = 0; tx < G.world.mw; ++tx) {
				if (G.world.tiles[ty * G.world.mw + tx] != 7) { continue; }
				float d = dist2(tx + 0.5, ty + 0.5, G.player.px, G.player.py);
				if (d < bd) {
					bd = d;
					best = math::ivec2(tx, ty);
				}
			}
		}
		return best;
	}

	math::ivec2 exit_tile() {
		for (int ty = 0; ty < G.world.mh; ++ty) {
			for (int tx = 0; tx < G.world.mw; ++tx) {
				if (G.world.tiles[ty * G.world.mw + tx] == 6) { return math::ivec2(tx, ty); }
			}
		}
		return math::ivec2(-1, -1);
	}
}

// the one world handle; typed so nothing can deep-copy it by accident
shared_ptr<Game> G = null;
