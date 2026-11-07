// Gator Quest - the simulation core's front door: world creation, the fixed
// 10 Hz tick, day/night and weather, reagent foraging, and the state hash.
// Everything here is deterministic given the seed and the intents per tick.

import "gq_actions.jai";

int TICK_MS = 100;
int DAY_TICKS = 3600;          // six real minutes per swamp day
string WEATHER_LETTERS = "crstfa";

void sim_reset() {
	TICK = 0;
	NEXT_SER = 1;
	MOB_LIVE = 0;
	MOB_KIND = [];
	MOB_SER = [];
	MOB_WORLD = [];
	MOB_X = [];
	MOB_Y = [];
	MOB_PX = [];
	MOB_PY = [];
	MOB_MOVE_T = [];
	MOB_MOVE_D = [];
	MOB_DIR = [];
	MOB_HP = [];
	MOB_HPMAX = [];
	MOB_MP = [];
	MOB_MPMAX = [];
	MOB_TARGET = [];
	MOB_AI = [];
	MOB_NEXT = [];
	MOB_ATK = [];
	MOB_CAST = [];
	MOB_FLAGS = [];
	MOB_PLAYER = [];
	MOB_SPAWNER = [];
	MOB_HOMEX = [];
	MOB_HOMEY = [];
	MOB_POISON = [];
	MOB_BURN = [];
	MOB_SLOW = [];
	MOB_BUFF = [];
	MOB_HIT_T = [];
	MOB_OWNER = [];
	MOB_AUX = [];
	MOB_ROLE = [];
	MOB_LEVEL = [];
	GI_ITEM = [];
	GI_W = [];
	GI_X = [];
	GI_Y = [];
	GI_SER = [];
	GI_EXPIRE = [];
	GI_OWNER = [];
	CHARS = [];
	IN_MOVE = [];
	IN_QUEUE = [];
	IN_STEP_SEQ = [];
	IN_STEP_DIR = [];
	IN_ACK = [];
	IN_DROPPED = [];
	CHAR_ARCHIVE = [];
	CAMERA_W = -1;
	events_clear();
}

void sim_init(int seed) {
	sim_reset();
	world_generate(seed);
	populate_npcs();
	rng_seed(seed * 31 + 7);
	for (int w = 0; w < WORLD_COUNT; ++w) {
		W_WEATHER[w] = 0;
		W_WEATHER_T[w] = 600 + rnd(1200);
	}
	// fill the spawners once so the world starts inhabited
	for (int pass = 0; pass < 3; ++pass) {
		for (int s = 0; s < SP_W.size(); ++s) { SP_NEXT[s] = 0; }
		int saved = TICK;
		TICK = pass * 10;
		spawners_tick_force();
		TICK = saved;
	}
	WORLD_READY = true;
}

// the initial fill ignores the "no spawning next to players" rule (there are none yet)
void spawners_tick_force() {
	for (int s = 0; s < SP_W.size(); ++s) {
		if (SP_ALIVE[s] >= SP_MAX[s]) { continue; }
		int w = SP_W[s];
		int kind = SP_KIND[s];
		for (int attempt = 0; attempt < 6; ++attempt) {
			int x = SP_X[s] + rnd_range(-SP_RAD[s], SP_RAD[s]);
			int y = SP_Y[s] + rnd_range(-SP_RAD[s], SP_RAD[s]);
			if (MONS[kind].boss) { x = SP_X[s]; y = SP_Y[s]; }
			if (!in_world(w, x, y) || mob_at(w, x, y) >= 0) { continue; }
			int k = walk_at(w, x, y);
			int mv = MON_MOVE[kind];
			bool ok = mv == 4 ? (k != 0) : (mv == 2 ? k == 2 : (k == 1 || k == 6 || k == 4 || (k == 2 && mv == 3)));
			if (!ok) { continue; }
			int m = mob_spawn(kind, w, x, y);
			MOB_SPAWNER[m] = s;
			MOB_HOMEX[m] = SP_X[s];
			MOB_HOMEY[m] = SP_Y[s];
			SP_ALIVE[s] = SP_ALIVE[s] + 1;
			SP_NEXT[s] = SP_DELAY[s];
			break;
		}
	}
}

int time_of_day() { return TICK % DAY_TICKS; }
int day_number() { return TICK / DAY_TICKS + 1; }
// 0..23
int hour_of_day() { return (time_of_day() * 24 / DAY_TICKS + 7) % 24; }

void weather_tick() {
	if (TICK % 50 != 0) { return; }
	for (int w = 0; w < WORLD_COUNT; ++w) {
		if (TICK < W_WEATHER_T[w]) { continue; }
		string opts = WORLDS[w].weather;
		int pick = WEATHER_LETTERS.find(opts.substr(rnd(opts.size()), 1));
		if (pick < 0) { pick = 0; }
		if (pick != W_WEATHER[w]) {
			W_WEATHER[w] = pick;
			array<string> msg = ["The sky clears.", "It starts to rain.", "Snow begins to fall.", "Thunder rolls across the sky!", "A thick fog rolls in.", "Ash drifts down from above."];
			for (int s = 0; s < CHARS.size(); ++s) {
				int m = CHARS[s].mob;
				if (CHARS[s].active && mob_live(m) && MOB_WORLD[m] == w) { say_to(s, msg[pick], C_GRAY); }
			}
		}
		W_WEATHER_T[w] = TICK + 1500 + rnd(2000);
	}
}

// wild reagents sprout where players roam (pearls and moss on the grass)
void forage_tick() {
	if (TICK % 120 != 0) { return; }
	for (int s = 0; s < CHARS.size(); ++s) {
		if (!CHARS[s].active) { continue; }
		int m = CHARS[s].mob;
		if (!mob_live(m)) { continue; }
		int w = MOB_WORLD[m];
		int x = MOB_X[m] + rnd_range(-14, 14);
		int y = MOB_Y[m] + rnd_range(-10, 10);
		if (!in_world(w, x, y) || !is_land(w, x, y) || town_at(w, x, y) >= 0) { continue; }
		int near = 0;
		for (int i = 0; i < GI_ITEM.size(); ++i) {
			if (GI_ITEM[i].base >= 0 && GI_W[i] == w && cheb(GI_X[i], GI_Y[i], x, y) < 12) { near = near + 1; }
		}
		if (near >= 10) { continue; }
		int reg = REAGENTS[rnd(REAGENTS.size())];
		gi_add(make_item(reg, 1 + rnd(2)), w, x, y, -1, 2400);
	}
}

// queue an intent for a slot (local keyboard, network, or a scripted bot)
void intent_move(int slot, int dir) {
	if (slot < IN_MOVE.size()) { IN_MOVE[slot] = dir; }
}

void intent_act(int slot, string a) {
	if (slot < IN_QUEUE.size()) { IN_QUEUE[slot].push(a); }
}

void step_queue_ensure(int slot) {
	while (IN_STEP_SEQ.size() <= slot) {
		IN_STEP_SEQ.push([]);
		IN_STEP_DIR.push([]);
		IN_ACK.push(0);
		IN_DROPPED.push(0);
	}
}

// a step a predicting client already took on its side; the host applies it when the player may move
void intent_step(int slot, int seq, int dir) {
	step_queue_ensure(slot);
	if (dir < 0 || dir > 7 || seq <= IN_ACK[slot]) { return; }
	if (IN_STEP_SEQ[slot].size() >= 12) {
		IN_DROPPED[slot] = imax(IN_DROPPED[slot], seq);
		return;
	}
	IN_STEP_SEQ[slot].push(seq);
	IN_STEP_DIR[slot].push(dir);
}

// steps queued for another place are void after a teleport; they count as processed
void step_queue_flush(int slot) {
	step_queue_ensure(slot);
	for (int i = 0; i < IN_STEP_SEQ[slot].size(); ++i) { IN_ACK[slot] = imax(IN_ACK[slot], IN_STEP_SEQ[slot][i]); }
	IN_ACK[slot] = imax(IN_ACK[slot], IN_DROPPED[slot]);
	IN_STEP_SEQ[slot] = [];
	IN_STEP_DIR[slot] = [];
}

void sim_tick() {
	events_clear();
	TICK = TICK + 1;
	players_tick();
	ai_tick();
	status_tick();
	spawners_tick();
	ground_tick();
	weather_tick();
	forage_tick();
}

// ------------------------------------------------------------ the gate ---
int state_hash() {
	int h = 2166136261;
	h = mix32(h, TICK);
	h = mix32(h, RNG_S);
	h = mix32(h, NEXT_SER);
	for (int m = 0; m < MOB_KIND.size(); ++m) {
		if (MOB_KIND[m] < 0) { continue; }
		h = mix32(h, MOB_KIND[m] * 1000003 + MOB_SER[m]);
		h = mix32(h, MOB_WORLD[m] * 65536 + MOB_X[m] * 256 + MOB_Y[m]);
		h = mix32(h, MOB_HP[m] * 64 + MOB_FLAGS[m]);
		h = mix32(h, MOB_TARGET[m] + 1);
	}
	for (int i = 0; i < GI_ITEM.size(); ++i) {
		if (GI_ITEM[i].base < 0) { continue; }
		h = mix32(h, GI_ITEM[i].base * 4096 + GI_ITEM[i].qty);
		h = mix32(h, GI_W[i] * 65536 + GI_X[i] * 256 + GI_Y[i]);
	}
	for (int s = 0; s < CHARS.size(); ++s) {
		Character& c = CHARS[s];
		if (!c.active) { continue; }
		h = mix32(h, c.gold);
		h = mix32(h, c.fame);
		h = mix32(h, c.str * 10000 + c.dex * 100 + c.intel);
		for (int k = 0; k < c.skill.size(); ++k) { h = mix32(h, c.skill[k]); }
		for (int k = 0; k < c.pack.size(); ++k) { h = mix32(h, c.pack[k].base * 4096 + c.pack[k].qty); }
		for (int k = 0; k < c.worn.size(); ++k) { h = mix32(h, c.worn[k].base + 1); }
		for (int k = 0; k < c.quest_stage.size(); ++k) { h = mix32(h, c.quest_stage[k] * 256 + c.quest_count[k]); }
	}
	for (int k = 0; k < SP_ALIVE.size(); ++k) { h = mix32(h, SP_ALIVE[k] * 131 + SP_NEXT[k]); }
	return h;
}
