// Gator Quest - the interactive shell: keys to intents, UI modes, the fixed
// 10 Hz sim under a ~30 fps renderer, frame composition, and a headless
// capture mode that renders frames to text files (scripted keys included).

import "gq_hud.jai";

int LOCAL_SLOT;
bool NET_CLIENT;       // true when the sim runs on a remote host
int VIEW_ROWS;
bool QUIT;

// ------------------------------------------------------------- intents ---
void client_act(string a) {
	if (NET_CLIENT) { net_client_send("A|" + a); return; }
	intent_act(LOCAL_SLOT, a);
}

void client_move(int dir) {
	if (dir == LAST_MOVE_SENT) { return; }
	LAST_MOVE_SENT = dir;
	if (NET_CLIENT) { net_client_send("M|" + to_string(dir)); return; }
	intent_move(LOCAL_SLOT, dir);
}

void toast(string s) {
	UI_TOAST = s;
	UI_TOAST_F = 70;
}

// ---------------------------------------------------------------- layout ---
int FORCE_COLS;            // "screen WxH" on the command line pins the layout (tests, captures)
int FORCE_ROWS;

int screen_cols() { return FORCE_COLS > 0 ? FORCE_COLS : term_cols(); }
int screen_rows() { return FORCE_ROWS > 0 ? FORCE_ROWS : term_rows(); }

void layout(int cols, int rows) {
	SCREEN_COLS = imax(60, cols);
	SCREEN_ROWS = imax(20, rows);
	PANE_WIDTH = SCREEN_COLS >= 140 ? 38 : (SCREEN_COLS >= 100 ? 34 : 28);
	VIEW_ROWS = SCREEN_ROWS - 1;
	int vw = SCREEN_COLS - PANE_WIDTH;
	int ts = TILE_ZOOM != 0 ? TILE_ZOOM : (vw >= 150 ? 8 : (vw >= 84 ? 6 : 4));
	if (ts != TS || TEX.size() == 0) {
		art_init(ts);
		minimap_init();
		SHADE_WORLD = -1;
	}
	canvas_init(vw, VIEW_ROWS * 2);
	CAM_SNAP = true;
}

string compose_frame(ClientView& v) {
	array<string> pane = build_pane(v, PANE_WIDTH, VIEW_ROWS);
	string out = "";
	for (int r = 0; r < VIEW_ROWS; ++r) { out += canvas_row(r) + pane[r] + "\n"; }
	out += status_line(v, SCREEN_COLS);
	return out;
}

void render_frame(ClientView& v) {
	if (UI_MODE == "map") { draw_world_map(v); }
	else { render_world(v); }
}

// ------------------------------------------------------------------ keys ---
int held_dir() {
	int dx = 0;
	int dy = 0;
	if (key_down("up") || key_down("w")) { dy = dy - 1; }
	if (key_down("down") || key_down("s")) { dy = dy + 1; }
	if (key_down("left") || key_down("a")) { dx = dx - 1; }
	if (key_down("right") || key_down("d")) { dx = dx + 1; }
	if (dx == 0 && dy == 0) { return -1; }
	return dir_of(dx, dy);
}

void open_mode(string mode) {
	UI_MODE = mode;
	UI_SEL = 0;
	UI_SCROLL = 0;
}

int best_potion(ClientView& v) {
	int best = -1;
	int bestp = 0;
	for (int i = 0; i < v.pack.size(); ++i) {
		ItemDef d = ITEMS[v.pack[i].base];
		if (EFF_NAME[d.eff] == "heal" && d.pow > bestp) { best = i; bestp = d.pow; }
	}
	return best;
}

void quick_cast(ClientView& v, int n) {
	array<int> known = known_spells(v);
	if (n < 0 || n >= known.size()) { toast("No spell in that slot. Open your spellbook with B."); return; }
	client_act("C" + to_string(known[n]));
}

// one key edge in any mode
void on_key(ClientView& v, string k) {
	if (k == "") { return; }
	string mode = UI_MODE;
	if (mode == "chat") {
		if (k == "enter") {
			if (CHAT_BUF != "") { client_act(":" + CHAT_BUF); }
			CHAT_BUF = "";
			UI_MODE = "play";
		} else if (k == "esc") { CHAT_BUF = ""; UI_MODE = "play"; }
		else if (k == "backspace") { if (CHAT_BUF.size() > 0) { CHAT_BUF = CHAT_BUF.substr(0, CHAT_BUF.size() - 1); } }
		else if (k == "space") { CHAT_BUF = CHAT_BUF + " "; }
		else if (k.size() == 1 && CHAT_BUF.size() < 100) { CHAT_BUF = CHAT_BUF + k; }
		return;
	}
	if (mode == "map") {
		if (k == "m" || k == "esc" || k == "enter") { UI_MODE = "play"; }
		return;
	}
	if (mode != "play") {
		on_menu_key(v, k);
		return;
	}
	if (k == "space" || k == "e") { client_act("E"); }
	else if (k == "f" || k == "tab") { client_act("T"); }
	else if (k == "g") { client_act("G"); }
	else if (k == "h") { client_act("N"); }
	else if (k == "q") {
		int p = best_potion(v);
		if (p >= 0) { client_act("U" + to_string(p)); } else { toast("No healing potions!"); }
	}
	else if (k == "x") { client_act("Z"); }
	else if (k == "r") {
		array<int> known = known_spells(v);
		toast("Recast: use 1-9 for spells (see B).");
		if (known.size() > 0) { client_act("C" + to_string(known[0])); }
	}
	else if (k == "i") { open_mode("pack"); }
	else if (k == "p") { open_mode("worn"); }
	else if (k == "k") { open_mode("skills"); }
	else if (k == "j") { open_mode("journal"); }
	else if (k == "b") { open_mode("spells"); }
	else if (k == "c") { open_mode("craft"); }
	else if (k == "m") { UI_MODE = "map"; }
	else if (k == "?") { open_mode("help"); }
	else if (k == "/") { UI_MODE = "chat"; CHAT_BUF = "/"; }
	else if (k == "enter") { UI_MODE = "chat"; CHAT_BUF = ""; }
	else if (k == "esc") {
		if (v.target_ser >= 0) { client_act("!"); }
		else { open_mode("menu"); }
	}
	else if (k == "z") {
		TILE_ZOOM = TS == 8 ? 4 : TS + 2;
		layout(SCREEN_COLS, SCREEN_ROWS);
		toast(`Zoom: ${TS}px tiles`);
	}
	else if (k.size() == 1 && k[0] >= '1' && k[0] <= '9') { quick_cast(v, (k[0] - '1')); }
	// a dialog opened by the host (bumping or E on an npc)
}

void on_menu_key(ClientView& v, string k) {
	string mode = UI_MODE;
	build_menu(v);
	int n = MENU_LABELS.size();
	if (k == "esc" || k == "tab") {
		if (mode == "shop_buy" || mode == "shop_sell" || mode == "bank") { open_mode("dialog"); return; }
		if (mode == "bank_put") { open_mode("bank"); return; }
		UI_MODE = "play";
		return;
	}
	if (k == "up" || k == "w") { UI_SEL = UI_SEL > 0 ? UI_SEL - 1 : imax(0, n - 1); return; }
	if (k == "down" || k == "s") { UI_SEL = UI_SEL + 1 < n ? UI_SEL + 1 : 0; return; }
	int arg = UI_SEL < n ? MENU_ARG[UI_SEL] : -1;
	bool go = k == "enter" || k == "space" || k == "e";
	// panes switch with their own keys from any pane
	if (k == "i" && mode != "pack") { open_mode("pack"); return; }
	if (k == "k") { open_mode("skills"); return; }
	if (k == "j") { open_mode("journal"); return; }
	if (k == "b" && mode != "bank") { open_mode("spells"); return; }
	if (k == "p") { open_mode("worn"); return; }
	if (k == "c") { open_mode("craft"); return; }
	if (k == "i" && mode == "pack") { UI_MODE = "play"; return; }
	if (mode == "pack") {
		if (go && arg >= 0) { client_act("U" + to_string(arg)); }
		else if (k == "d" && arg >= 0) { client_act("D" + to_string(arg)); }
		return;
	}
	if (mode == "worn") {
		if (go && arg >= 0) { client_act("R" + to_string(arg)); }
		return;
	}
	if (mode == "spells") {
		if (go && arg >= 0) { client_act("C" + to_string(arg)); UI_MODE = "play"; }
		else if (k.size() == 1 && k[0] >= '1' && k[0] <= '9') { quick_cast(v, k[0] - '1'); UI_MODE = "play"; }
		return;
	}
	if (mode == "craft") {
		if (go && arg >= 0) { client_act("K" + to_string(arg)); }
		return;
	}
	if (mode == "dialog") {
		if (!go) { return; }
		if (arg == 1) { open_mode("shop_buy"); }
		else if (arg == 2) { open_mode("shop_sell"); }
		else if (arg == 3) { client_act("H" + to_string(v.dlg_ser)); }
		else if (arg == 4) { open_mode("bank"); }
		else if (arg == 5) { client_act("Q" + to_string(v.dlg_ser)); }
		else { UI_MODE = "play"; }
		return;
	}
	if (mode == "shop_buy") {
		if (go && arg >= 0) { client_act(`B${v.dlg_ser},${arg}`); }
		return;
	}
	if (mode == "shop_sell") {
		if (go && arg >= 0) { client_act(`S${v.dlg_ser},${arg}`); }
		return;
	}
	if (mode == "bank") {
		if (!go) { return; }
		if (arg == -1) { client_act(`W${v.dlg_ser},-1`); }
		else if (arg == -2) { client_act(`X${v.dlg_ser},-1`); }
		else if (arg == -3) { open_mode("bank_put"); }
		else if (arg >= 0) { client_act(`X${v.dlg_ser},${arg}`); }
		return;
	}
	if (mode == "bank_put") {
		if (go && arg >= 0) { client_act(`W${v.dlg_ser},${arg}`); }
		return;
	}
	if (mode == "menu") {
		if (!go) { return; }
		if (arg == 0) { UI_MODE = "play"; }
		else if (arg == 1) { toast(game_save_request()); UI_MODE = "play"; }
		else if (arg == 2) { toast(game_load_request()); UI_MODE = "play"; }
		else if (arg == 3) { music_toggle(); }
		else if (arg == 4) { SFX_ON = !SFX_ON; }
		else if (arg == 5) {
			TILE_ZOOM = TILE_ZOOM == 0 ? 4 : (TILE_ZOOM == 8 ? 0 : TILE_ZOOM + 2);
			layout(SCREEN_COLS, SCREEN_ROWS);
		}
		else if (arg == 6) { open_mode("help"); }
		else if (arg == 7) { PLAY_EXIT = "quit"; QUIT = true; }
		else if (arg == 8) {
			int port = TM_PORT.to_int(28777);
			if (net_host_start(port)) {
				string msg = host_welcome();
				cv_log(CV, msg, 5);
				toast(`Hosting on port ${port}.`);
			} else { toast(`Could not open port ${port} for hosting.`); }
			UI_MODE = "play";
		}
		else if (arg == 9) {
			net_host_stop("the host stopped hosting");
			toast("Stopped hosting; the swamp is yours alone again.");
			UI_MODE = "play";
		}
		else if (arg == 10) { PLAY_EXIT = "title"; QUIT = true; }
		return;
	}
	if (mode == "help" || mode == "skills" || mode == "journal") {
		if (go || k == "?") { UI_MODE = "play"; }
		return;
	}
}

// the host opened a dialog for us (an npc was bumped or used)
void check_dialog(ClientView& v) {
	if (!v.dlg_pending) { return; }
	v.dlg_pending = false;
	if (v.dlg_ser >= 0 && v.dlg_role >= 0 && UI_MODE == "play") { open_mode("dialog"); }
}

// ---- save hooks (single player / host); filled in by gq_save.jai
string game_save_request() {
	if (NET_CLIENT) { return "Only the host can save the world."; }
	return save_world_and_chars();
}

string game_load_request() {
	if (NET_CLIENT) { return "Only the host can load the world."; }
	return load_world_and_chars();
}

// ------------------------------------------------------------ the loop ---
// one step of the local host: tick the sim and hand our own view its snapshot
// the host's network is drained after the tick and before the snapshots go out, so what
// joins, leaves and chats between ticks lands in this tick's events (sim_tick clears them)
void local_host_tick() {
	bots_tick();
	sim_tick();
	net_host_pump();
	autosave_tick();
	host_after_tick();
	if (LOCAL_SLOT >= 0 && LOCAL_SLOT < CHARS.size() && CHARS[LOCAL_SLOT].active) {
		CV.rx_ms = NOW_MS;
		cv_apply(CV, snapshot_for(LOCAL_SLOT));
	}
}

int MAX_FRAMES;             // demo/test: stop after this many frames (0 = play until quit)
int STAT_FRAMES;
float STAT_RENDER_MS;
float STAT_COMPOSE_MS;
float STAT_SIM_MS;
string PLAY_EXIT;           // how the last session ended: "title" or "quit"

// ------------------------------------------------------------ sessions ---
// what a host tells its players to type
string host_welcome() {
	array<string> addrs = net_local_addresses();
	string where = addrs.size() > 0 ? addrs.join(" or ") : "this machine's address";
	return `Hosting on port ${NET_PORT}: friends choose Join a game and type ${where}.`;
}

void presentation_reset() {
	P_X = [];
	P_Y = [];
	P_VX = [];
	P_VY = [];
	P_LIFE = [];
	P_MAX = [];
	P_COL = [];
	P_FLAGS = [];
	FT_X = [];
	FT_Y = [];
	FT_S = [];
	FT_COL = [];
	FT_T = [];
	FT_SCALE = [];
	PR_X0 = [];
	PR_Y0 = [];
	PR_X1 = [];
	PR_Y1 = [];
	PR_T = [];
	PR_DUR = [];
	PR_KIND = [];
	FLASH = {};
	VIS_X = {};
	VIS_Y = {};
	BANNER_F = 0;
	UI_TOAST_F = 0;
	SHAKE_F = 0;
	UI_MODE = "play";
	CHAT_BUF = "";
}

// the title's choice becomes a session: the local gator joins this world (alone or hosting),
// or the welcomed join builds the host's world; returns the welcome line for a local gator
string session_start(string choice, bool demo) {
	presentation_reset();
	CAMERA_W = -1;
	pred_reset();
	if (demo) { PRED_ON = false; }
	BOTS = [];
	NET_CLOSE_F = 0;
	NET_CLOSE_WHY = "";
	LAST_MOVE_SENT = -2;
	PLAY_EXIT = "quit";
	if (choice == "join") {
		net_join_finish(TM_SESSION, true);
		cv_log(CV, `Connected to ${TM_JOIN_ADDR}:${TM_JOIN_PORT}. The host keeps your gator; type /who to see who is here.`, 5);
		return "";
	}
	LOCAL_SLOT = 0;
	string welcome = join_character(LOCAL_SLOT, CHOSEN_NAME, CHOSEN_HUE);
	CV = new ClientView();
	CV.slot = LOCAL_SLOT;
	if (demo) { bot_add(LOCAL_SLOT); }
	if (choice == "host") { welcome = welcome + " " + host_welcome(); }
	return welcome;
}

// the session ends: a joined player says goodbye; a host sends everyone home with their
// gators written; the local world and gator are saved (the title reloads the world from disk)
void session_end() {
	music_stop_all();
	if (NET_CLIENT) {
		net_session_bye(NET_SESSION);
		NET_CLIENT = false;
		NET_SESSION = -1;
	} else {
		if (net_hosting()) { net_host_stop("the host has left the swamp"); }
		if (SAVES_ON) { save_world_and_chars(); }
	}
	LOCAL_SLOT = 0;
	BOTS = [];
	pred_reset();
	presentation_reset();
	if (NET_CLOSE_WHY != "") { tm_fail("DISCONNECTED", "The connection to the host ended: " + NET_CLOSE_WHY + "."); }
	else { tm_open("main"); }
}

void play_loop() {
	print(ESC + "[2J", skip_newline);
	layout(screen_cols(), screen_rows());
	int last = time::now() * 1000;
	int acc = 0;
	int size_check = 0;
	time::stopwatch split;
	UI_MODE = "play";
	QUIT = false;
	while (!QUIT) {
		if (MAX_FRAMES > 0 && STAT_FRAMES >= MAX_FRAMES) { QUIT = true; break; }
		NOW_MS = time::now() * 1000;
		int dt = NOW_MS - last;
		last = NOW_MS;
		if (dt > 250) { dt = 250; }
		acc = acc + dt;
		// input
		string k = read_key();
		while (k != "") {
			on_key(CV, k);
			k = read_key();
		}
		// movement: predicted on this frame (the host hears about each step), or held-key intents
		int held = UI_MODE == "play" || UI_MODE == "map" ? held_dir() : -1;
		if (PRED_ON) { pred_input(CV, held, NOW_MS); }
		else { client_move(held); }
		// dev mode: an edited script re-runs on the next frame
		if (DEV_ON) {
			array<string> msgs = dev_poll();
			for (int i = 0; i < msgs.size(); ++i) {
				toast(msgs[i]);
				cv_log(CV, msgs[i], msgs[i].contains("FAILED") ? 2 : 5);
			}
		}
		// the world
		split.start();
		if (NET_CLIENT) {
			net_client_pump();
			CV.status = NET_CLOSE_F > 0 ? "disconnected" : "online";
		}
		else {
			while (acc >= TICK_MS) {
				local_host_tick();
				acc = acc - TICK_MS;
			}
			if (net_hosting()) { CV.status = `hosting :${NET_PORT}, ${net_player_count()} joined`; }
		}
		STAT_SIM_MS = STAT_SIM_MS + split.delta() * 1000;
		pred_reconcile(CV, NOW_MS);
		pred_frame(CV, NOW_MS);
		check_dialog(CV);
		process_events(CV);
		particles_update();
		size_check = size_check + 1;
		if (size_check >= 30) {
			size_check = 0;
			int cols = screen_cols();
			int rows = screen_rows();
			if (cols != SCREEN_COLS || rows != SCREEN_ROWS) {
				layout(cols, rows);
				print(ESC + "[2J", skip_newline);
			}
		}
		split.start();
		render_frame(CV);
		STAT_RENDER_MS = STAT_RENDER_MS + split.delta() * 1000;
		string frame = compose_frame(CV);
		STAT_COMPOSE_MS = STAT_COMPOSE_MS + split.delta() * 1000;
		print(HOME + frame, skip_newline);
		STAT_FRAMES = STAT_FRAMES + 1;
		FRAME = FRAME + 1;
		int spent = time::now() * 1000 - NOW_MS;
		if (spent < 33) { sleep((33 - spent) / 1000.0); }
	}
}

void music_stop_all() {
	if (AUDIO_ON) { music_stop(); }
}

// --------------------------------------------------------- frame capture ---
// strip SGR escapes so a pane row reads as text
string strip_sgr(string s) {
	string out = "";
	int i = 0;
	int n = s.size();
	while (i < n) {
		if (s[i] == '\x1b') {
			int j = i + 1;
			while (j < n && s[j] != 'm') { j = j + 1; }
			i = j + 1;
			continue;
		}
		out += s.substr(i, 1);
		i = i + 1;
	}
	return out;
}

// the view as luminance characters (two pixels per cell averaged), the panes as their text
string sketch_frame(ClientView& v) {
	array<string> pane = build_pane(v, PANE_WIDTH, VIEW_ROWS);
	string ramp = " .:-=+*#%@";
	string out = "";
	for (int r = 0; r < VIEW_ROWS; ++r) {
		string row = "";
		for (int x = 0; x < VW; ++x) {
			int a = PIX[(r * 2) * VW + x];
			int b = PIX[(r * 2 + 1) * VW + x];
			int lum = (PAL_R[a] * 3 + PAL_G[a] * 5 + PAL_B[a] * 2 + PAL_R[b] * 3 + PAL_G[b] * 5 + PAL_B[b] * 2) / 20;
			row += ramp.substr(iclamp(lum * 10 / 256, 0, 9), 1);
		}
		out += row + strip_sgr(pane[r]) + "\n";
	}
	out += strip_sgr(status_line(v, SCREEN_COLS)) + "\n";
	return out;
}

// run the smoke session with slot 0's view and capture frames at the given ticks
int run_shots() {
	int ticks = arg_value("ticks", "600").to_int(600);
	int seed = arg_value("seed", "1").to_int(1);
	int every = arg_value("every", "200").to_int(200);
	string dir = arg_value("out", data_root() + "/shots");
	string size = arg_value("screen", "150x44");
	array<string> wh = size.split("x");
	make_dirs(dir);
	sim_init(seed);
	BOTS = [];
	player_create(0, "Chomps", 0);
	bot_add(0);
	if (arg_flag("friends")) {
		player_create(1, "Snappy", 3);
		bot_add(1);
	}
	CV = new ClientView();
	CV.slot = 0;
	layout(wh[0].to_int(150), wh.size() > 1 ? wh[1].to_int(44) : 44);
	string script = arg_value("keys", "");
	array<string> keys = script == "" ? [] : script.split(" ");
	int key_i = 0;
	int shots = 0;
	for (int t = 1; t <= ticks; ++t) {
		NOW_MS = t * TICK_MS;
		// scripted keys drive the UI instead of the bot for the first slot when given
		if (key_i < keys.size() && t % 3 == 0) {
			string k = keys[key_i];
			key_i = key_i + 1;
			if (k.starts_with("wait")) { }
			else { on_key(CV, k); }
		}
		local_host_tick();
		for (int f = 0; f < 3; ++f) {
			NOW_MS = t * TICK_MS + f * 33;
			check_dialog(CV);
			process_events(CV);
			particles_update();
			FRAME = FRAME + 1;
		}
		if (t % every == 0 || t == ticks) {
			render_frame(CV);
			string name = dir + "/shot_t" + to_string(t);
			write_file(name + ".ans", compose_frame(CV));
			write_file(name + ".txt", sketch_frame(CV));
			png_write_canvas(name + ".png", 3);
			shots = shots + 1;
		}
	}
	print(`wrote ${shots} frame captures to ${dir} (STATE_HASH ${state_hash()})`);
	return 0;
}
