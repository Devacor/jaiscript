// Gator Quest - the dedicated host (no screen, no local player) and a scripted
// headless client. Together they make a real two-process netplay test
// (nettest2.bat), and the dedicated host is how a group runs a persistent swamp.

import "gq_tests.jai";

// jaiscript gatorquest.jai -- serve [port N] [ticks N] [bots K] [seed S]
int run_serve() {
	int port = arg_value("port", "28777").to_int(28777);
	int ticks = arg_value("ticks", "0").to_int(0);
	int bots = arg_value("bots", "0").to_int(0);
	SAVES_ON = !arg_flag("nosave");
	if (arg_value("savedir", "") != "") { SAVE_DIR = arg_value("savedir", ""); }
	bool resumed = SAVES_ON && load_world_for_session();
	if (!resumed) { sim_init(arg_value("seed", "1").to_int(1)); }
	if (!net_host_start(port)) {
		print(`serve: could not listen on port ${port}`);
		return 1;
	}
	print(`serve: ${resumed ? "saved world" : "new world"} (seed ${SEED}, day ${day_number()}) on port ${port}${ticks > 0 ? ", " + to_string(ticks) + " ticks" : ""}`);
	BOTS = [];
	for (int b = 0; b < bots; ++b) {
		player_create(b, "Bot " + to_string(b + 1), b);
		bot_add(b);
	}
	int next = cast<int>(time::now() * 1000);
	int players = net_player_count();
	int peak = players;
	int joins = 0;
	int leaves = 0;
	time::stopwatch wall;
	time::stopwatch work;
	wall.start();
	while (ticks == 0 || TICK < ticks) {
		int now = cast<int>(time::now() * 1000);
		if (now < next) {
			NET_SRV.pump();
			sleep(imin(5, next - now) / 1000.0);
			continue;
		}
		next = next + TICK_MS;
		if (now - next > 1000) { next = now; }
		work.resume();
		bots_tick();
		sim_tick();
		net_host_pump();
		autosave_tick();
		host_after_tick();
		work.pause();
		int n = net_player_count();
		if (n > players) { joins = joins + n - players; }
		if (n < players) { leaves = leaves + players - n; }
		if (n != players) {
			string names = "";
			array<int> slots = NET_CID_OF.keys();
			for (int k = 0; k < slots.size(); ++k) { names = names + (k > 0 ? ", " : "") + CHARS[slots[k]].name; }
			print(`serve: tick ${TICK}: ${n} ${plural(n, "player", "players")} online${n > 0 ? " (" + names + ")" : ""}`);
			players = n;
			peak = imax(peak, n);
		}
	}
	net_host_stop("the host is shutting down");
	if (SAVES_ON) { print("serve: " + save_world_and_chars()); }
	int work_pct = cast<int>(work.elapsed() * 100 / math::max(wall.elapsed(), 0.001));
	print(`serve: done after ${TICK} ticks: peak ${peak} players, ${joins} joins, ${leaves} leaves, ${NET_SNAPS_OUT} snapshots, ${NET_BYTES_OUT / 1024} KB sent, host work ${work_pct}% of wall time`);
	return 0;
}

// jaiscript gatorquest.jai -- client HOST [port N] [name X] [hue H] [frames N] [say TEXT] [walk DIR]
// a scripted, screenless player: walks, chats, reads its snapshots, prints what it saw
int run_client() {
	string host = arg_value("client", "127.0.0.1");
	int port = arg_value("port", "28777").to_int(28777);
	string name = arg_value("name", "Tester");
	int frames = arg_value("frames", "150").to_int(150);
	string say = arg_value("say", "");
	int walk = arg_value("walk", "2").to_int(2);
	string err = net_join(host, port, name, arg_value("hue", "3").to_int(3), true);
	if (err != "") {
		print(`client ${name}: could not join: ${err}`);
		return 1;
	}
	int s = NET_SESSION;
	print(`client ${name}: joined as slot ${NCS_SLOT[s]} (world seed ${NCS_SEED[s]})`);
	int start_x = -1;
	int end_x = -1;
	int max_players = 0;
	array<string> others;
	for (int f = 0; f < frames && net_session_live(s); ++f) {
		NOW_MS = cast<int>(time::now() * 1000);
		net_session_pump(s);
		ClientView& v = NCS_VIEW[s];
		int me = cv_me(v);
		if (me >= 0) {
			if (start_x < 0) { start_x = v.m_x[me]; }
			end_x = v.m_x[me];
		}
		int seen = 0;
		for (int i = 0; i < v.m_ser.size(); ++i) {
			if (v.m_slot[i] >= 0 && v.m_ser[i] != v.my_ser) {
				seen = seen + 1;
				if (!others.contains(v.m_name[i])) { others.push(v.m_name[i]); }
			}
		}
		max_players = imax(max_players, seen);
		if (f == 20) { net_session_send(s, "M|" + to_string(walk)); }
		if (f == 35) { net_session_send(s, "M|-1"); }
		if (f == 60 && say != "") { net_session_send(s, "A|:" + say); }
		sleep(0.033);
	}
	ClientView& v = NCS_VIEW[s];
	// chat from the other players: "Name: text" lines whose name is a player we saw
	int chats = 0;
	for (int i = 0; i < v.log.size(); ++i) {
		for (int k = 0; k < others.size(); ++k) {
			if (v.log[i].starts_with(others[k] + ": ")) {
				print(`client ${name}: heard "${v.log[i]}"`);
				chats = chats + 1;
			}
		}
	}
	print(`client ${name}: ${v.snaps} snapshots, walked ${end_x - start_x} tiles, saw other players: ${others.size() > 0 ? others.join(", ") : "none"}, ${chats} chat ${plural(chats, "line", "lines")} heard, status ${NCS_STATUS[s]}`);
	net_session_bye(s);
	return 0;
}
