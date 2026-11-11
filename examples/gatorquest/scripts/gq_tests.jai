// Gator Quest - headless tests: the save round trip inside the smoke, the save
// robustness suite, and the hot-reload test. Each prints ok/FAIL lines and a
// summary; the exit code is the number of failures.

import "gq_dev.jai";

int TEST_FAILS;
int TEST_PASSES;

void check(bool ok, string what) {
	if (ok) {
		TEST_PASSES = TEST_PASSES + 1;
		print("  ok    " + what);
	} else {
		TEST_FAILS = TEST_FAILS + 1;
		print("  FAIL  " + what);
	}
}

// save everything, wipe the sim, load it back; true when the state hash is unchanged
bool save_roundtrip(string dir) {
	SAVE_DIR = dir;
	int before = state_hash();
	time::stopwatch lap;
	lap.start();
	string msg = save_world_and_chars();
	int save_ms = cast<int>(lap.delta() * 1000);
	string back = load_world_and_chars();
	int load_ms = cast<int>(lap.delta() * 1000);
	int after = state_hash();
	print(`  save: ${msg} (${save_ms} ms, ${read_file(world_path()).size()} bytes)  load: ${back} (${load_ms} ms)  hash ${before} -> ${after}`);
	return before == after;
}

// ------------------------------------------------------------ savetest ---
string corrupt(string text) {
	int at = text.size() / 2;
	string ch = text.substr(at, 1) == "7" ? "8" : "7";
	return text.substr(0, at) + ch + text.substr(at + 1, text.size() - at - 1);
}

int run_savetest(string base_dir) {
	string dir = base_dir + "/savetest";
	SAVE_DIR = dir;
	make_dirs(dir + "/chars");
	array<string> old = list_dir(dir);
	for (int i = 0; i < old.size(); ++i) { delete_file(dir + "/" + old[i]); }
	array<string> oldc = list_dir(dir + "/chars");
	for (int i = 0; i < oldc.size(); ++i) { delete_file(dir + "/chars/" + oldc[i]); }
	print("save robustness:");
	// missing
	check(load_sealed(world_path(), WORLD_MAGIC) == "" && SAVE_NOTE == "missing", "a missing world file is reported as missing");
	check(!load_world_for_session(), "no world file: a fresh world starts");
	// a session to save
	sim_init(3);
	BOTS = [];
	player_create(0, "Tester", 2);
	bot_add(0);
	for (int t = 0; t < 400; ++t) { bots_tick(); sim_tick(); }
	int gold0 = CHARS[0].gold;
	check(save_roundtrip(dir), "round trip keeps the exact state hash");
	string first = read_file(world_path());
	string crlf = first;
	crlf.replace("\n", "\r\n");
	write_file(world_path(), crlf);
	check(load_sealed(world_path(), WORLD_MAGIC) != "" && SAVE_NOTE == "", "a save whose line endings a tool converted to CRLF still verifies");
	write_file(world_path(), first);
	for (int t = 0; t < 200; ++t) { bots_tick(); sim_tick(); }
	int hash2 = state_hash();
	int tick2 = TICK;
	save_world_and_chars();
	check(file_exists(world_path() + ".bak") && read_file(world_path() + ".bak") == first, "the second save keeps the first as .bak");
	check(!file_exists(world_path() + ".tmp"), "no .tmp left behind after a good save");
	// corrupt the main file: the loader falls back to the backup
	write_file(world_path(), corrupt(read_file(world_path())));
	string msg = load_world_and_chars();
	check(TICK == 400 && msg.contains("restored from world.gqw.bak"), `corrupt world file falls back to the backup (${msg})`);
	// truncated file
	string good = seal(unseal(first, WORLD_MAGIC));
	write_file(world_path(), good.substr(0, good.size() / 3));
	check(load_sealed(world_path(), WORLD_MAGIC) != "" && SAVE_NOTE.contains("truncated"), `truncated world file is detected (${SAVE_NOTE})`);
	// a newer format is refused, the backup is used
	string body = unseal(good, WORLD_MAGIC);
	string future = WORLD_MAGIC + " 99" + body.substr(body.find("\n"), body.size() - body.find("\n"));
	write_file(world_path(), seal(future));
	string fb = load_sealed(world_path(), WORLD_MAGIC);
	check(fb != "" && SAVE_NOTE.contains("newer Gator Quest"), `a newer save format is refused (${SAVE_NOTE})`);
	delete_file(world_path() + ".bak");
	check(load_sealed(world_path(), WORLD_MAGIC) == "" && SAVE_NOTE.contains("newer"), "with no backup the newer file is not loaded");
	// an old, sparse character file: every missing field keeps its default
	string sparse = seal(CHAR_MAGIC + " 1\nname Old Timer\ngold 77\nskills 500,400\n");
	write_file(char_path("Old Timer"), sparse);
	sim_init(3);
	string welcome = join_character(0, "Old Timer", 0);
	Character& c = CHARS[0];
	check(c.name == "Old Timer" && c.gold == 77 && c.skill[0] == 500 && c.skill.size() == Sk.count && c.worn.size() == Slot.count, `a sparse character file loads with defaults (${welcome})`);
	check(mob_live(c.mob) && MOB_WORLD[c.mob] == c.home_world && MOB_HP[c.mob] == MOB_HPMAX[c.mob], "it enters the world at home with full health");
	// a character keeps its gear, skills and place across a restart
	sim_init(3);
	BOTS = [];
	player_create(0, "Roundtrip", 4);
	CHARS[0].gold = 1234;
	CHARS[0].skill[Sk.fishing] = 777;
	pack_add(0, make_item(I_RUBBERDUCK, 1));
	mob_place(CHARS[0].mob, 1, W_ARRIVE_X[1], W_ARRIVE_Y[1]);
	MOB_HP[CHARS[0].mob] = 20;
	save_world_and_chars();
	check(load_world_for_session() && MOB_LIVE > 100, "the saved world loads for a new session without its players");
	string back = join_character(0, "Roundtrip", 0);
	Character& r = CHARS[0];
	int rm = r.mob;
	check(r.gold == 1234 && r.skill[Sk.fishing] == 777 && pack_has(0, I_RUBBERDUCK, 1), "gold, skills and backpack survive a restart");
	check(MOB_WORLD[rm] == 1 && MOB_HP[rm] == 20, `position and health survive a restart (${WORLDS[MOB_WORLD[rm]].name}, hp ${MOB_HP[rm]})`);
	// a character file that is corrupt, with no backup: a new gator starts
	write_file(char_path("Broken"), corrupt(char_text(0)));
	sim_init(3);
	string nb = join_character(0, "Broken", 1);
	check(nb.starts_with("Welcome to Swampton") && CHARS[0].gold == 50, "a corrupt character file with no backup starts a new gator");
	print(`savetest: ${TEST_PASSES} passed, ${TEST_FAILS} failed`);
	return TEST_FAILS;
}

// ------------------------------------------------------------ thieftest ---
// the Raccoon Bandits: where they live, how fast they run, what they take and give back

// land tiles reachable on foot from (x, y)
array<int> land_reach(int w, int x, int y) {
	int ww = W_W[w];
	array<int> seen;
	for (int i = 0; i < ww * W_H[w]; ++i) { seen.push(0); }
	array<int> queue = [y * ww + x];
	seen[y * ww + x] = 1;
	for (int head = 0; head < queue.size(); ++head) {
		int cx = queue[head] % ww;
		int cy = queue[head] / ww;
		for (int d = 0; d < 8; d += 2) {
			int nx = cx + DX[d];
			int ny = cy + DY[d];
			if (in_world(w, nx, ny) && seen[ny * ww + nx] == 0 && is_land(w, nx, ny)) {
				seen[ny * ww + nx] = 1;
				queue.push(ny * ww + nx);
			}
		}
	}
	return seen;
}

// tick until slot loses gold to a thief (the victim kept alive); returns the gold taken
int wait_for_theft(int slot, int ticks) {
	int me = CHARS[slot].mob;
	int before = CHARS[slot].gold;
	for (int t = 0; t < ticks; ++t) {
		MOB_HP[me] = MOB_HPMAX[me];
		sim_tick();
		if (CHARS[slot].gold < before) { return before - CHARS[slot].gold; }
	}
	return 0;
}

void thief_reset_beside(int r, int slot) {
	int me = CHARS[slot].mob;
	array<int> p = find_free_spot(MOB_WORLD[me], MOB_X[me] + 1, MOB_Y[me], 3, 2);
	mob_place(r, MOB_WORLD[me], p[0], p[1]);
	MOB_FLAGS[r] = MOB_FLAGS[r] & ~MF_FLEEING;
	MOB_TARGET[r] = me;
	MOB_NEXT[r] = TICK;
	MOB_ATK[r] = TICK;
}

// gold on the ground within r tiles: the total, or with qty >= 0 the number of piles of exactly qty
int ground_gold_near(int w, int x, int y, int r, int qty) {
	int n = 0;
	for (int i = 0; i < GI_ITEM.size(); ++i) {
		if (GI_ITEM[i].base != I_GOLD || GI_W[i] != w || cheb(GI_X[i], GI_Y[i], x, y) > r) { continue; }
		if (qty < 0) { n = n + GI_ITEM[i].qty; }
		else if (GI_ITEM[i].qty == qty) { n = n + 1; }
	}
	return n;
}

int run_thieftest(string base_dir) {
	print("thieves:");
	sim_init(1);
	BOTS = [];
	int kind = mid("Raccoon Bandit");
	int spawners = 0;
	int deep = 0;
	int den = -1;
	for (int s = 0; s < SP_W.size(); ++s) {
		if (SP_KIND[s] != kind) { continue; }
		spawners = spawners + 1;
		if (SP_W[s] == W_WARRENS) { deep = deep + 1; den = s; }
	}
	check(spawners >= 3 && deep == spawners, `raccoons spawn only in the Raccoon Warrens (${deep} of ${spawners} spawners)`);
	int below = 0;
	int elsewhere = 0;
	for (int m = 0; m < MOB_KIND.size(); ++m) {
		if (MOB_KIND[m] != kind || MOB_HP[m] <= 0) { continue; }
		if (MOB_WORLD[m] == W_WARRENS) { below = below + 1; } else { elsewhere = elsewhere + 1; }
	}
	check(below >= 3 && elsewhere == 0, `the world starts with ${below} raccoons in the warrens and ${elsewhere} elsewhere`);
	check(MON_SPEED[kind] > 2 && MONS[kind].poisonous, `a raccoon walks slower than a gator (${MON_SPEED[kind]} ticks a step against 2) and bites can poison`);
	// the way down: Swampton's grate, the sewers' stairs, the warrens' stairs back up
	int down = gate_in(W_SEWERS, W_WARRENS);
	int up = gate_in(W_WARRENS, W_SEWERS);
	check(down >= 0 && up >= 0, "the sewers have stairs down to the warrens, and the warrens stairs back up");
	array<int> sewer = land_reach(W_SEWERS, W_ARRIVE_X[W_SEWERS], W_ARRIVE_Y[W_SEWERS]);
	check(sewer[GATE_Y[down] * W_W[W_SEWERS] + GATE_X[down]] != 0, "the stairs down are a walk from the sewer entrance");
	array<int> warren = land_reach(W_WARRENS, W_ARRIVE_X[W_WARRENS], W_ARRIVE_Y[W_WARRENS]);
	int floor = 0;
	int cut = 0;
	for (int i = 0; i < warren.size(); ++i) {
		if (TILES[W_OFF[W_WARRENS] + i] != T_DIRT) { continue; }
		floor = floor + 1;
		if (warren[i] == 0) { cut = cut + 1; }
	}
	check(floor > 500 && cut == 0, `every burrow is reachable from the stairs (${floor} tiles, ${cut} cut off)`);
	player_create(0, "Tester", 2);
	int me = CHARS[0].mob;
	mob_place(me, W_SEWERS, GATE_X[down], GATE_Y[down]);
	on_enter(me);
	check(MOB_WORLD[me] == W_WARRENS && cheb(MOB_X[me], MOB_Y[me], GATE_X[up], GATE_Y[up]) <= 2, "the stairs down land beside the stairs up");
	mob_place(me, W_WARRENS, GATE_X[up], GATE_Y[up]);
	on_enter(me);
	check(MOB_WORLD[me] == W_SEWERS && cheb(MOB_X[me], MOB_Y[me], GATE_X[down], GATE_Y[down]) <= 2, "the stairs up land beside the sewer stairs down");
	// one raccoon in an emptied den
	for (int m = 0; m < MOB_KIND.size(); ++m) {
		if (MOB_KIND[m] >= 0 && MOB_PLAYER[m] < 0 && MOB_WORLD[m] == W_WARRENS) { mob_free(m); }
	}
	for (int s = 0; s < SP_W.size(); ++s) {
		if (SP_W[s] == W_WARRENS) { SP_NEXT[s] = 1 << 40; }
	}
	array<int> p = find_free_spot(W_WARRENS, SP_X[den], SP_Y[den], 3, 2);
	mob_place(me, W_WARRENS, p[0], p[1]);
	CHARS[0].gold = 200;
	int r = mob_spawn(kind, W_WARRENS, p[0], p[1]);
	thief_reset_beside(r, 0);
	int stolen = wait_for_theft(0, 800);
	check(stolen >= 3 && stolen <= 10 && MOB_AUX[r] == stolen, `a theft takes 3 to 10 gold and the raccoon keeps it (${stolen}, carrying ${MOB_AUX[r]})`);
	int steps = 0;
	int last = MOB_MOVE_T[r];
	for (int t = 0; t < 40; ++t) {
		MOB_HP[me] = MOB_HPMAX[me];
		sim_tick();
		if (MOB_MOVE_T[r] != last) { steps = steps + 1; last = MOB_MOVE_T[r]; }
	}
	check((MOB_FLAGS[r] & MF_FLEEING) != 0 && steps >= 1 && steps <= 10, `a fleeing raccoon takes at most a step every 4 ticks (${steps} steps in 40 ticks; a gator walks 20)`);
	CHARS[0].gold = 0;
	mob_place(me, W_WARRENS, p[0], p[1]);
	thief_reset_beside(r, 0);
	bool poisoned = false;
	for (int t = 0; t < 800 && !poisoned; ++t) {
		MOB_HP[me] = MOB_HPMAX[me];
		sim_tick();
		poisoned = MOB_POISON[me] > 0;
	}
	check(poisoned, "a raccoon's bite poisons");
	MOB_POISON[me] = 0;
	// the purse survives a save and load, and holds every victim's gold
	make_dirs(base_dir + "/thieftest/chars");
	check(save_roundtrip(base_dir + "/thieftest") && MOB_KIND[r] == kind && MOB_AUX[r] == stolen, `the purse survives a save and load (${MOB_AUX[r]})`);
	me = CHARS[0].mob;
	player_create(1, "Second", 3);
	int me2 = CHARS[1].mob;
	mob_place(me, 0, W_ARRIVE_X[0], W_ARRIVE_Y[0]);
	mob_place(me2, W_WARRENS, p[0], p[1]);
	CHARS[1].gold = 100;
	thief_reset_beside(r, 1);
	int stolen2 = wait_for_theft(1, 800);
	int purse = stolen + stolen2;
	check(stolen2 > 0 && MOB_AUX[r] == purse, `a second victim's gold joins the purse (${stolen} + ${stolen2} = ${MOB_AUX[r]})`);
	int w = MOB_WORLD[r];
	int kx = MOB_X[r];
	int ky = MOB_Y[r];
	int gold_before = ground_gold_near(w, kx, ky, 3, -1);
	kill(r, me2);
	int dropped = ground_gold_near(w, kx, ky, 3, -1) - gold_before;
	check(ground_gold_near(w, kx, ky, 3, purse) >= 1 && dropped >= purse + MONS[kind].gold_lo && dropped <= purse + MONS[kind].gold_hi, `killing it drops the ${purse} stolen gold as a pile of its own beside its loot (${dropped} gold on the ground)`);
	mob_place(me2, w, kx, ky);
	int had = CHARS[1].gold;
	pick_up(1);
	int got = CHARS[1].gold - had;
	check(got >= purse && ground_gold_near(w, kx, ky, 3, -1) + got == dropped, `picking it up gives the gold back (+${got}), counted once`);
	// The Masked Menace: the Captain sends you down
	int q = -1;
	for (int i = 0; i < QUESTS.size(); ++i) {
		if (QUESTS[i].name == "The Masked Menace") { q = i; }
	}
	quest_talk(0, R_CAPTAIN);
	check(q >= 0 && quest_active(0, q) && QUESTS[q].offer.contains("warrens below the sewers"), "the Captain's quest points to the warrens");
	mob_place(me, W_WARRENS, p[0], p[1]);
	for (int k = 0; k < QUESTS[q].count; ++k) {
		array<int> at = find_free_spot(W_WARRENS, p[0], p[1] + 2, 4, 2);
		kill(mob_spawn(kind, W_WARRENS, at[0], at[1]), me);
	}
	check(quest_ready(0, q), `three raccoons later the quest is ready (${quest_progress(0, q)}/${quest_goal(q)})`);
	quest_talk(0, R_CAPTAIN);
	check(CHARS[0].quest_stage[q] == 99, "and the Captain takes it");
	print(`thieftest: ${TEST_PASSES} passed, ${TEST_FAILS} failed`);
	return TEST_FAILS;
}

// ------------------------------------------------------------ netplay ---
// the real host session and real client sessions over loopback sockets in one engine
// (p9_netplay style); the clients share the host's terrain instead of regenerating it

array<int> NT_SESS;            // test session indices, by role

void nt_round() {
	NOW_MS = NOW_MS + TICK_MS;
	for (int s = 0; s < NCS_CH.size(); ++s) { net_session_pump(s); }
	sim_tick();
	net_host_pump();
	host_after_tick();
	sleep(0.002);
	for (int s = 0; s < NCS_CH.size(); ++s) { net_session_pump(s); }
}

void nt_rounds(int n) {
	for (int i = 0; i < n; ++i) { nt_round(); }
}

bool log_has(int s, string needle) {
	array<string>& log = NCS_VIEW[s].log;
	for (int i = 0; i < log.size(); ++i) {
		if (log[i].contains(needle)) { return true; }
	}
	return false;
}

bool view_sees_slot(int s, int slot) {
	array<int>& slots = NCS_VIEW[s].m_slot;
	for (int i = 0; i < slots.size(); ++i) {
		if (slots[i] == slot) { return true; }
	}
	return false;
}

bool saw_damage_on_me(int s) {
	ClientView& v = NCS_VIEW[s];
	for (int i = 0; i < v.e_kind.size(); ++i) {
		if (v.e_kind[i] == Ev.dmg && v.e_a[i] == v.my_ser) { return true; }
	}
	return false;
}

// open a session and join; waits (bounded) for the answer
int nt_join(string name, int hue, int port) {
	int s = net_client_open("127.0.0.1", port);
	for (int i = 0; i < 300 && !NCS_CH[s].connected() && net_session_live(s); ++i) { nt_round(); }
	net_client_hello(s, name, hue);
	for (int i = 0; i < 300 && NCS_STATUS[s] == "hello"; ++i) { nt_round(); }
	return s;
}

int nt_mob(int s) { return CHARS[NCS_SLOT[s]].mob; }

int run_nettest(string base_dir, int port) {
	SAVE_DIR = base_dir + "/nettest";
	make_dirs(SAVE_DIR + "/chars");
	array<string> old = list_dir(SAVE_DIR + "/chars");
	for (int i = 0; i < old.size(); ++i) { delete_file(SAVE_DIR + "/chars/" + old[i]); }
	print("netplay:");
	sim_init(11);
	BOTS = [];
	NOW_MS = 0;
	check(net_host_start(port), `host listens on port ${port}`);
	// ---- three gators join
	int a = nt_join("alpha", 1, port);
	int b = nt_join("bravo", 2, port);
	int c = nt_join("chuck", 3, port);
	check(NCS_STATUS[a] == "joined" && NCS_STATUS[b] == "joined" && NCS_STATUS[c] == "joined", `three clients joined (${NCS_STATUS[a]}, ${NCS_STATUS[b]}, ${NCS_STATUS[c]})`);
	check(NCS_SLOT[a] == 0 && NCS_SLOT[b] == 1 && NCS_SLOT[c] == 2, `slots 0, 1, 2 in join order`);
	check(NCS_SEED[a] == SEED && NCS_SEED[c] == SEED, "WELCOME carries the world seed");
	nt_rounds(3);
	check(NCS_VIEW[a].my_ser == MOB_SER[nt_mob(a)] && NCS_VIEW[c].snaps > 0, "each client's snapshots name its own gator");
	if (!(view_sees_slot(a, 1) && view_sees_slot(a, 2) && view_sees_slot(c, 0))) {
		print(`  debug a sees ${NCS_VIEW[a].m_ser.size()} mobiles, slots ${to_json(NCS_VIEW[a].m_slot)}; host mobs a ${MOB_X[nt_mob(a)]},${MOB_Y[nt_mob(a)]} b ${MOB_X[nt_mob(b)]},${MOB_Y[nt_mob(b)]} c ${MOB_X[nt_mob(c)]},${MOB_Y[nt_mob(c)]}`);
	}
	check(view_sees_slot(a, 1) && view_sees_slot(a, 2) && view_sees_slot(c, 0), "the clients see each other in their snapshots");
	check(log_has(a, "bravo has joined the swamp.") && log_has(a, "chuck has joined the swamp.") && !log_has(c, "chuck has joined"), "join announcements reach the others, not the joiner");
	check(NCS_VIEW[b].name == "bravo" && NCS_VIEW[b].skills.size() == Sk.count && NCS_VIEW[b].pack.size() > 0, "each client received its character sheet");
	// ---- movement: intents up, positions down
	int ma = nt_mob(a);
	int x0 = MOB_X[ma];
	net_session_send(a, "M|2");
	nt_rounds(12);
	net_session_send(a, "M|-1");
	nt_rounds(3);
	int moved = MOB_X[ma] - x0;
	int ia = cv_find(NCS_VIEW[b], MOB_SER[ma]);
	check(moved >= 3, `alpha walked east on the host (${moved} tiles)`);
	check(ia >= 0 && NCS_VIEW[b].m_x[ia] == MOB_X[ma], "bravo's view shows alpha where the host has it");
	// ---- chat
	net_session_send(a, "A|:hello swamp");
	nt_rounds(3);
	check(log_has(b, "alpha: hello swamp") && log_has(c, "alpha: hello swamp") && log_has(a, "alpha: hello swamp"), "open chat reaches everyone in the realm");
	// ---- parties
	net_session_send(a, "A|:/invite bravo");
	nt_rounds(3);
	check(log_has(b, "alpha invites you to a party"), "bravo got the party invitation");
	net_session_send(b, "A|:/accept");
	nt_rounds(3);
	int pa = CHARS[NCS_SLOT[a]].party;
	check(pa >= 0 && pa == CHARS[NCS_SLOT[b]].party && CHARS[NCS_SLOT[c]].party < 0, "alpha and bravo share a party on the host");
	check(NCS_VIEW[b].party_names.contains("alpha"), "bravo's sheet lists alpha in the party");
	net_session_send(a, "A|:/p meet at the bayou");
	nt_rounds(3);
	check(log_has(b, "alpha (party): meet at the bayou") && !log_has(c, "meet at the bayou"), "party chat reaches the party only");
	// ---- pvp: nobody can hurt an unflagged gator; flagged gators can fight
	int mc = nt_mob(c);
	mob_place(mc, MOB_WORLD[ma], MOB_X[ma] + 1, MOB_Y[ma]);
	int hp_a = MOB_HP[ma];
	net_session_send(c, "A|A" + to_string(MOB_SER[ma]));
	nt_rounds(12);
	check(MOB_HP[ma] == hp_a && MOB_TARGET[mc] != ma, "chuck cannot target unflagged alpha");
	net_session_send(a, "A|:/pvp");
	net_session_send(c, "A|:/pvp");
	nt_rounds(2);
	check(CHARS[NCS_SLOT[a]].pvp && CHARS[NCS_SLOT[c]].pvp && log_has(a, "PvP ON"), "both flag for PvP over the wire");
	net_session_send(c, "A|A" + to_string(MOB_SER[ma]));
	for (int i = 0; i < 60 && MOB_HP[ma] == hp_a; ++i) { nt_round(); }
	check(MOB_HP[ma] < hp_a, `flagged chuck hurts flagged alpha (${hp_a} -> ${MOB_HP[ma]})`);
	check(saw_damage_on_me(a), "alpha's snapshots carried the damage events on alpha");
	net_session_send(c, "A|!");
	net_session_send(a, "A|:/pvp");
	net_session_send(c, "A|:/pvp");
	nt_rounds(2);
	// ---- a graceful leave writes the character on the host; rejoining restores it
	CHARS[NCS_SLOT[c]].gold = 4321;
	net_session_bye(c);
	nt_rounds(4);
	check(!CHARS[2].active && log_has(a, "chuck has left the swamp (logged out)"), "chuck logged out and the others were told");
	string cfile = read_file(char_path("chuck"));
	check(cfile.contains("gold 4321"), "the host wrote chuck's character file on logout");
	int c2 = nt_join("chuck", 3, port);
	nt_rounds(2);
	check(NCS_STATUS[c2] == "joined" && CHARS[NCS_SLOT[c2]].gold == 4321 && log_has(c2, "Welcome back, chuck"), "chuck rejoined as himself (4321 gold)");
	// ---- an abrupt drop is handled like a leave
	NCS_CH[b].close();
	NCS_STATUS[b] = "closed: test drop";
	nt_rounds(6);
	check(!CHARS[1].active && log_has(a, "bravo has left the swamp (connection closed)") && file_exists(char_path("bravo")), "bravo's dropped connection freed the slot and saved the character");
	check(CHARS[NCS_SLOT[a]].party < 0, "alpha's party dissolved with bravo gone");
	// ---- refusals
	int dup = nt_join("Alpha", 5, port);
	check(NCS_STATUS[dup].starts_with("refused") && NCS_STATUS[dup].contains("already here"), `a second 'alpha' is refused (${NCS_STATUS[dup]})`);
	int old_proto = net_client_open("127.0.0.1", port);
	for (int i = 0; i < 100 && !NCS_CH[old_proto].connected(); ++i) { nt_round(); }
	net_session_send(old_proto, "HELLO|99|zed|0");
	for (int i = 0; i < 100 && net_session_live(old_proto); ++i) { nt_round(); }
	check(NCS_STATUS[old_proto].starts_with("refused") && NCS_STATUS[old_proto].contains("protocol"), `a client speaking another protocol is refused (${NCS_STATUS[old_proto]})`);
	// ---- sixteen gators, and a seventeenth
	array<int> crowd;
	for (int k = 0; k < 14; ++k) { crowd.push(nt_join("gator" + to_string(k), k % 16, port)); }
	int joined = 0;
	for (int k = 0; k < crowd.size(); ++k) {
		if (NCS_STATUS[crowd[k]] == "joined") { joined = joined + 1; }
	}
	check(joined == 14 && net_player_count() == 16, `sixteen gators online (${net_player_count()})`);
	time::stopwatch lap;
	lap.start();
	int bytes0 = NET_BYTES_OUT;
	nt_rounds(20);
	int per_tick_us = cast<int>(lap.delta() * 1000000 / 20);
	int per_tick_bytes = (NET_BYTES_OUT - bytes0) / 20;
	lap.start();
	for (int i = 0; i < 20; ++i) { host_after_tick(); }
	int snap_us = cast<int>(lap.delta() * 1000000 / 20);
	for (int i = 0; i < 20; ++i) {
		for (int s = 0; s < NCS_CH.size(); ++s) { net_session_pump(s); }
	}
	print(`  info  16 players: ${per_tick_us} us per round (sim + 16 snapshots + 17 client pumps); the host's 16 snapshots alone ${snap_us} us; ${per_tick_bytes} bytes of snapshots per tick`);
	check(log_has(crowd[13], "Welcome to Swampton, gator13") && view_sees_slot(crowd[13], NCS_SLOT[a]), "the sixteenth gator plays in the same world");
	int extra = nt_join("latecomer", 0, port);
	check(NCS_STATUS[extra].starts_with("refused") && NCS_STATUS[extra].contains("full"), `a seventeenth is refused (${NCS_STATUS[extra]})`);
	// ---- the host leaves: everyone is told, characters are saved
	net_host_stop("test over");
	for (int i = 0; i < 20; ++i) {
		NOW_MS = NOW_MS + TICK_MS;
		for (int s = 0; s < NCS_CH.size(); ++s) { net_session_pump(s); }
		sleep(0.002);
	}
	int told = 0;
	for (int k = 0; k < crowd.size(); ++k) {
		if (NCS_STATUS[crowd[k]] == "closed: test over") { told = told + 1; }
	}
	check(told == 14 && NCS_STATUS[a] == "closed: test over", `the host's goodbye reached every client (${told + (NCS_STATUS[a] == "closed: test over" ? 1 : 0)} of 15)`);
	check(file_exists(char_path("gator7")) && file_exists(char_path("alpha")), "the host saved every character on the way out");
	print(`netplay: ${TEST_PASSES} passed, ${TEST_FAILS} failed`);
	return TEST_FAILS;
}

// ------------------------------------------------------------ hot reload ---
// an edit in the file's own line endings (a Windows checkout has \r\n)
string replace_once(string text, string from, string to) {
	string f = from;
	string t = to;
	if (text.contains("\r\n")) {
		f.replace("\n", "\r\n");
		t.replace("\n", "\r\n");
	}
	int at = text.find(f);
	if (at < 0) { print("  (edit target not found: " + from + ")"); return text; }
	return text.substr(0, at) + t + text.substr(at + f.size(), text.size() - at - f.size());
}

int count_frog_targets(int player_mob) {
	int n = 0;
	for (int m = 0; m < MOB_KIND.size(); ++m) {
		if (MOB_KIND[m] == mid("Frog") && MOB_TARGET[m] == player_mob) { n = n + 1; }
	}
	return n;
}

int spawn_frogs_around(int player_mob, int count) {
	int w = MOB_WORLD[player_mob];
	int made = 0;
	for (int d = 0; d < 8 && made < count; ++d) {
		int x = MOB_X[player_mob] + DX[d] * 2;
		int y = MOB_Y[player_mob] + DY[d] * 2;
		if (mob_at(w, x, y) < 0 && is_land(w, x, y)) {
			mob_spawn(mid("Frog"), w, x, y);
			made = made + 1;
		}
	}
	return made;
}

// the test edits the scripts it runs from, so it runs only from a scratch copy (hotreload.ps1 makes one)
int run_hotreload() {
	string src = current_script_dir();
	if (!file_exists(src + "/HOTRELOAD_SCRATCH")) {
		print("hotreload edits the scripts it runs from: run hotreload.ps1, which runs it from a scratch copy");
		return 1;
	}
	import_recheck_interval(0);   // every poll checks: the test edits and polls faster than the check window
	array<string> names = ["gq_types.jai", "gq_items.jai", "gq_ai.jai", "gq_actions.jai", "gq_data.jai"];
	array<string> copies;
	for (int i = 0; i < names.size(); ++i) { copies.push(src + "/" + names[i]); }
	print("hot reload:");
	dev_start();
	// whatever the checkout did, two files get \r\n and three get \n: both paths always run
	for (int i = 0; i < copies.size(); ++i) {
		string text = read_file(copies[i]);
		text.replace("\r\n", "\n");
		if (i == 0 || i == 2) { text.replace("\n", "\r\n"); }
		write_file(copies[i], text);
	}
	array<string> first = dev_poll();
	check(first.size() == 5 && !first.join(" ").contains("FAILED"), `the five rewritten files reload without errors (${first.join("; ")})`);
	sim_init(5);
	BOTS = [];
	player_create(0, "Reloady", 6);
	bot_add(0);
	for (int t = 0; t < 300; ++t) { bots_tick(); sim_tick(); }
	int me = CHARS[0].mob;
	intent_move(0, -1);
	MOB_TARGET[me] = -1;
	// before: frogs flee
	int frogs = spawn_frogs_around(me, 3);
	for (int t = 0; t < 30; ++t) { sim_tick(); }
	check(frogs > 0 && count_frog_targets(me) == 0, `before: ${frogs} frogs next to the gator do not attack`);
	check(dev_poll().size() == 0, "nothing reloads while nothing changed");
	int tick0 = TICK;
	int gold0 = CHARS[0].gold;
	int sw0 = CHARS[0].skill[Sk.swords];
	int mobs0 = MOB_LIVE;
	int hash0 = state_hash();
	// edit 1: a class gains a field and a method, a constant changes, a function in one file
	// changes for callers in another, monster behavior and monster data change
	string types = read_file(copies[0]);
	types = replace_once(types, "class Character {\n", "class Character {\n\tint reload_marks = 7;\n\tint power() { return str * 2; }\n");
	types = replace_once(types, "class Item {\n", "class Item {\n\tint shine = 3;\n");
	write_file(copies[0], types);
	write_file(copies[1], replace_once(read_file(copies[1]), "int PACK_MAX = 40;", "int PACK_MAX = 60;"));
	string ai = read_file(copies[2]);
	ai = replace_once(ai, "int AGGRO = 7;", "int AGGRO = 9;");
	ai = replace_once(ai, "void ai_critter(int m) {\n", "void ai_critter(int m) {\n\tai_melee(m);\n\treturn;\n");
	write_file(copies[2], ai);
	write_file(copies[3], replace_once(read_file(copies[3]), "int buy_price(int item) { return imax(1, ITEMS[item].value * 11 / 10); }", "int buy_price(int item) { return imax(1, ITEMS[item].value * 2); }"));
	write_file(copies[4], replace_once(read_file(copies[4]), "\"Swamp Rat|hp=10|", "\"Swamp Rat|hp=77|"));
	array<string> msgs = dev_poll();
	for (int i = 0; i < msgs.size(); ++i) { print("    " + msgs[i]); }
	int failed = 0;
	for (int i = 0; i < msgs.size(); ++i) {
		if (msgs[i].contains("FAILED")) { failed = failed + 1; }
	}
	check(msgs.size() == 5 && failed == 0, `five changed files reloaded (${msgs.size()} messages, ${failed} failed)`);
	// the world carried over
	check(TICK == tick0 && CHARS[0].gold == gold0 && CHARS[0].skill[Sk.swords] == sw0 && CHARS[0].mob == me, "tick, gold, skills and the player's mobile carried over");
	check(MOB_LIVE == mobs0 && state_hash() == hash0, `the whole sim state is untouched by the reload (${MOB_LIVE} mobiles, hash ${state_hash()})`);
	// the class change migrated the live instances
	var& live = CHARS[0];
	check(live.reload_marks == 7, "Character gained reload_marks = 7 on the live instance");
	check(live.power() == CHARS[0].str * 2, `a new method runs on the live instance (power ${live.power()})`);
	var& first_item = CHARS[0].pack[0];
	check(first_item.shine == 3, "items already in the backpack gained the new field");
	Character fresh = Character();
	var fv = fresh;
	check(fv.reload_marks == 7, "new instances get the new field too");
	// the constant, seen from files that were not reloaded
	check(PACK_MAX == 60, "PACK_MAX reads 60 everywhere");
	while (CHARS[0].pack.size() < 45) { CHARS[0].pack.push(make_item(I_OLDBOOT, 1)); }
	check(pack_add(0, make_item(I_TINCAN, 1)), "pack_add uses the new limit (46th item fits)");
	// the function, from its own file and from another
	int price = ITEMS[I_BREAD].value * 2;
	check(buy_price(I_BREAD) == price, `buy_price changed (bread ${buy_price(I_BREAD)})`);
	int npc = find_role_npc(0, R_PROVISIONER);
	mob_place(me, 0, MOB_X[npc], MOB_Y[npc] + 1);
	CHARS[0].gold = 500;
	int gold_before = CHARS[0].gold;
	CHARS[0].pack = [];
	int idx = SHOPS[ROLES[R_PROVISIONER].shop].index_of(I_BREAD);
	shop_buy(0, npc, idx);
	check(CHARS[0].gold == gold_before - price, `shop_buy charges the new price (${gold_before - CHARS[0].gold} gold)`);
	check(AGGRO == 9, "AGGRO changed to 9");
	check(MONS[mid("Swamp Rat")].hp == 77, "the monster table re-ran: Swamp Rats have 77 hp");
	int rat = mob_spawn(mid("Swamp Rat"), 0, MOB_X[me] + 5, MOB_Y[me]);
	check(MOB_HPMAX[rat] == 77, "a Swamp Rat spawned after the reload has 77 hp");
	mob_free(rat);
	// the behavior change: frogs attack now
	mob_place(me, 0, W_ARRIVE_X[0] + 12, W_ARRIVE_Y[0] + 12);
	spawn_frogs_around(me, 3);
	for (int t = 0; t < 40; ++t) { sim_tick(); }
	check(count_frog_targets(me) > 0, `after: frogs attack the gator (${count_frog_targets(me)} targeting)`);
	// edit 2: change the method again; the field keeps the value we give it now
	live.reload_marks = 42;
	write_file(copies[0], replace_once(read_file(copies[0]), "int power() { return str * 2; }", "int power() { return str * 3; }"));
	msgs = dev_poll();
	check(msgs.size() == 1 && !msgs[0].contains("FAILED"), "the second edit reloads");
	var& live2 = CHARS[0];
	check(live2.power() == CHARS[0].str * 3 && live2.reload_marks == 42, "the method changed again and the field kept its live value (42)");
	// edit 3: the state files reload too, and keep the world: all they declare is state
	int hash1 = state_hash();
	string ui0 = UI_MODE;
	write_file(src + "/gq_state.jai", read_file(src + "/gq_state.jai") + "// touched by the hot-reload test\n");
	write_file(src + "/gq_cstate.jai", read_file(src + "/gq_cstate.jai") + "// touched by the hot-reload test\n");
	msgs = dev_poll();
	check(msgs.size() == 2 && !msgs.join(" ").contains("FAILED"), `gq_state.jai and gq_cstate.jai reload (${msgs.join("; ")})`);
	check(state_hash() == hash1 && CHARS[0].mob == me && TICK == tick0 + 40 && UI_MODE == ui0 && PRED_ON, "every mobile, player, tick and client setting survived the state files' reload");
	// edit 4: broken edits are rejected at the import and the running code stays
	// (a) a constant of the wrong type: the checker rejects the module before it runs
	write_file(copies[1], replace_once(read_file(copies[1]), "int PACK_MAX = 60;", "int PACK_MAX = \"lots\";"));
	msgs = dev_poll();
	check(msgs.size() == 1 && msgs[0].contains("FAILED") && PACK_MAX == 60, `a bad constant is rejected, PACK_MAX stays 60 (${msgs.join("; ")})`);
	write_file(copies[1], replace_once(read_file(copies[1]), "int PACK_MAX = \"lots\";", "int PACK_MAX = 60;"));
	msgs = dev_poll();
	check(msgs.size() == 1 && !msgs[0].contains("FAILED"), "fixing the constant reloads cleanly");
	// (b) a type error inside a function: rejected by the checker, the old body keeps running
	write_file(copies[3], replace_once(read_file(copies[3]), "int buy_price(int item) { return imax(1, ITEMS[item].value * 2); }", "int buy_price(int item) { return \"free\"; }"));
	msgs = dev_poll();
	check(msgs.size() == 1 && msgs[0].contains("FAILED") && buy_price(I_BREAD) == price, `a static type error is rejected, buy_price unchanged (${msgs.join("; ")})`);
	write_file(copies[3], replace_once(read_file(copies[3]), "int buy_price(int item) { return \"free\"; }", "int buy_price(int item) { return imax(1, ITEMS[item].value * 2); }"));
	msgs = dev_poll();
	check(msgs.size() == 1 && !msgs[0].contains("FAILED"), "fixing the function reloads cleanly");
	// (c) a run-time error while the module runs: caught at the import, the constant keeps its value
	write_file(copies[1], replace_once(read_file(copies[1]), "int PACK_MAX = 60;", "int PACK_MAX = [60][3];"));
	msgs = dev_poll();
	check(msgs.size() == 1 && msgs[0].contains("FAILED") && PACK_MAX == 60, `a run-time error in a reloading module is caught, PACK_MAX stays 60 (${msgs.join("; ")})`);
	write_file(copies[1], replace_once(read_file(copies[1]), "int PACK_MAX = [60][3];", "int PACK_MAX = 60;"));
	msgs = dev_poll();
	check(msgs.size() == 1 && !msgs[0].contains("FAILED") && PACK_MAX == 60, "and the fixed file reloads");
	// keep playing on the new code
	BOTS[0].phase_t = TICK;
	for (int t = 0; t < 600; ++t) { bots_tick(); sim_tick(); }
	check(mob_live(CHARS[0].mob) && TICK == tick0 + 40 + 600, "600 more ticks on the reloaded code without errors");
	print(`hotreload: ${TEST_PASSES} passed, ${TEST_FAILS} failed, STATE_HASH ${state_hash()}`);
	return TEST_FAILS;
}
