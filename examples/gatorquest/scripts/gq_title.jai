// Gator Quest - the title screen: the live world drifts by behind the logo
// while you name your gator, pick its color and choose how to play: alone,
// hosting (the port and this machine's addresses), or joining (address, port,
// the recent servers with the name last used on each, and a connecting state
// that ends joined, refused or unreachable, with the reason).

import "gq_save.jai";

string CHOSEN_NAME;
int CHOSEN_HUE;

array<string> HUE_NAMES = ["Swamp Green", "Teal", "Olive", "Mud Brown", "Blue Steel", "Brick Red", "Royal Purple", "Mint",
	"Tan", "Charcoal", "Albino", "Emerald", "Crimson", "Moss", "Gold", "Ocean"];

array<string> TM_MAIN = ["Single player", "Host a game", "Join a game", "Quit"];

// ---- the title's pages: main, host, join, connecting, failed
string TM_PAGE;
int TM_SEL;
string TM_NAME;
int TM_HUE;
string TM_PORT;              // host page
string TM_HOST_ERR;
array<string> TM_ADDRS;
string TM_HOSTNAME;
string TM_ADDR;              // join page
string TM_JPORT;
int TM_SESSION;              // the join in progress
int TM_T0;
int TM_F0;                    // the frame the title came up (the logo's entrance and the sky's clock run from it)
bool TM_SAID;
string TM_JOIN_ADDR;
int TM_JOIN_PORT;
string TM_JOIN_NAME;
string TM_FAIL_TITLE;        // failed page: NAME TAKEN, SERVER FULL, VERSION MISMATCH, UNREACHABLE ...
string TM_FAIL;
int TM_CONNECT_MS = 10000;

// ---- recent servers (saves/servers.txt), most recent first
array<string> RECENT_ADDR;
array<int> RECENT_PORT;
array<string> RECENT_NAME;
int RECENT_MAX = 8;

string recent_path() { return save_dir() + "/servers.txt"; }

void recent_load() {
	RECENT_ADDR = [];
	RECENT_PORT = [];
	RECENT_NAME = [];
	if (!file_exists(recent_path())) { return; }
	array<string> ls = read_file(recent_path()).lines();
	for (int i = 0; i < ls.size() && RECENT_ADDR.size() < RECENT_MAX; ++i) {
		array<string> f = string::trim(ls[i]).split(" ");
		if (f.size() < 4 || f[0] != "server") { continue; }
		string name = f[3];
		for (int k = 4; k < f.size(); ++k) { name = name + " " + f[k]; }
		RECENT_ADDR.push(f[1]);
		RECENT_PORT.push(f[2].to_int(28777));
		RECENT_NAME.push(name);
	}
}

void recent_save() {
	string text = "GATORQUEST-SERVERS 1\n";
	for (int i = 0; i < RECENT_ADDR.size(); ++i) { text += `server ${RECENT_ADDR[i]} ${RECENT_PORT[i]} ${RECENT_NAME[i]}\n`; }
	make_dirs(save_dir());
	write_file(recent_path(), text);
}

void recent_remove(int i) {
	RECENT_ADDR.remove(i);
	RECENT_PORT.remove(i);
	RECENT_NAME.remove(i);
}

void recent_add(string addr, int port, string name) {
	array<string> addrs = [addr];
	array<int> ports = [port];
	array<string> names = [name];
	for (int i = 0; i < RECENT_ADDR.size() && addrs.size() < RECENT_MAX; ++i) {
		if (string::to_lower(RECENT_ADDR[i]) == string::to_lower(addr) && RECENT_PORT[i] == port) { continue; }
		addrs.push(RECENT_ADDR[i]);
		ports.push(RECENT_PORT[i]);
		names.push(RECENT_NAME[i]);
	}
	RECENT_ADDR = addrs;
	RECENT_PORT = ports;
	RECENT_NAME = names;
}

// ---- menu logic (no drawing, so the menu test can drive it)
void title_setup(string join_host, bool hosting, int port) {
	TM_PAGE = "main";
	TM_SEL = 0;
	TM_NAME = "";
	TM_HUE = 0;
	TM_PORT = to_string(port);
	TM_JPORT = to_string(port);
	TM_ADDR = "";
	TM_HOST_ERR = "";
	TM_FAIL = "";
	TM_FAIL_TITLE = "";
	TM_SESSION = -1;
	recent_load();
	if (join_host != "") {
		tm_open("join");
		TM_ADDR = join_host;
		TM_SEL = 2;
	} else if (hosting) {
		tm_open("host");
		TM_SEL = 1;
	}
}

string tm_shown_name() { return TM_NAME == "" ? (TITLE_LAST != "" ? TITLE_LAST : "Chomps") : TM_NAME; }

void tm_open(string page) {
	TM_PAGE = page;
	TM_SEL = 0;
	if (page == "host") {
		TM_HOST_ERR = "";
		TM_ADDRS = net_local_addresses();
		TM_HOSTNAME = net_host_name();
	}
}

int tm_rows() {
	if (TM_PAGE == "main") { return TM_MAIN.size(); }
	if (TM_PAGE == "host") { return 3; }
	if (TM_PAGE == "join") { return 4 + RECENT_ADDR.size(); }
	return 1;
}

void tm_edit_name(string k) {
	if (k == "backspace") { if (TM_NAME.size() > 0) { TM_NAME = TM_NAME.substr(0, TM_NAME.size() - 1); } }
	else if (k == "space") { if (TM_NAME.size() > 0 && TM_NAME.size() < 14) { TM_NAME = TM_NAME + " "; } }
	else if (k.size() == 1 && TM_NAME.size() < 14 && (k[0] >= 'a' && k[0] <= 'z' || k[0] >= '0' && k[0] <= '9')) {
		string ch = k;
		if (TM_NAME.size() == 0 || TM_NAME.ends_with(" ")) { ch.to_upper(); }
		TM_NAME = TM_NAME + ch;
	}
}

void tm_edit_digits(string& field, string k) {
	if (k == "backspace") { if (field.size() > 0) { field = field.substr(0, field.size() - 1); } }
	else if (k.size() == 1 && k[0] >= '0' && k[0] <= '9' && field.size() < 5) { field = field + k; }
}

void tm_edit_address(string k) {
	if (k == "backspace") { if (TM_ADDR.size() > 0) { TM_ADDR = TM_ADDR.substr(0, TM_ADDR.size() - 1); } }
	else if (k.size() == 1 && TM_ADDR.size() < 60 && (k[0] >= 'a' && k[0] <= 'z' || k[0] >= '0' && k[0] <= '9' || k == "." || k == "-" || k == ":")) { TM_ADDR = TM_ADDR + k; }
}

void tm_fail(string title, string why) {
	TM_FAIL_TITLE = title;
	TM_FAIL = why;
	TM_PAGE = "failed";
	TM_SEL = 0;
}

bool tm_host() {
	int port = TM_PORT.to_int(0);
	if (port < 1 || port > 65535) {
		TM_HOST_ERR = "Pick a port from 1 to 65535 (28777 is the usual one).";
		return false;
	}
	if (!net_host_start(port)) {
		TM_HOST_ERR = `Could not open port ${port}: something else is using it or the system refused. Try another port.`;
		return false;
	}
	return true;
}

void tm_connect() {
	TM_ADDR.trim();
	string addr = TM_ADDR;
	int port = TM_JPORT.to_int(0);
	int colon = addr.find(":");
	if (colon >= 0) {
		port = addr.substr(colon + 1, addr.size() - colon - 1).to_int(port);
		addr = addr.substr(0, colon);
	}
	if (addr == "") { tm_fail("NO ADDRESS", "Type the host's address first: the host's menu shows it (like 192.168.1.20)."); return; }
	if (port < 1 || port > 65535) { tm_fail("BAD PORT", "The port is a number from 1 to 65535 (28777 unless the host picked another)."); return; }
	TM_JOIN_ADDR = addr;
	TM_JOIN_PORT = port;
	TM_JOIN_NAME = tm_shown_name();
	TM_SESSION = net_client_open(addr, port);
	TM_T0 = NOW_MS;
	TM_SAID = false;
	TM_PAGE = "connecting";
}

void tm_cancel() {
	int s = TM_SESSION;
	if (s < 0 || !net_session_live(s)) { return; }
	NCS_CH[s].close();
	NCS_STATUS[s] = "closed: cancelled";
}

// the host's refusal in the player's terms
void tm_refused(string why) {
	if (why.contains("full")) { tm_fail("SERVER FULL", `The host said: ${why}. Try again when someone leaves.`); }
	else if (why.contains("already here")) { tm_fail("NAME TAKEN", `The host said: ${why}. Type another name on the Connect row and try again.`); }
	else if (why.contains("protocol")) { tm_fail("VERSION MISMATCH", `The host runs a different version of Gator Quest (${why}; this copy speaks protocol ${NET_PROTO}). Both players need the same version.`); }
	else { tm_fail("REFUSED", "The host said: " + why + "."); }
}

void tm_closed(string why) {
	string w = why.starts_with("error: ") ? why.substr(7, why.size() - 7) : why;
	if (TM_SAID) { tm_fail("DISCONNECTED", `The host closed the connection before letting us in (${w}).`); }
	else { tm_fail("UNREACHABLE", `Could not reach ${TM_JOIN_ADDR}:${TM_JOIN_PORT} (${w}). Check the address and port, and that the host has started hosting.`); }
}

// one frame of a join in progress: "join" once the host has welcomed us
string title_update() {
	if (TM_PAGE != "connecting") { return ""; }
	int s = TM_SESSION;
	net_session_pump(s);
	string st = NCS_STATUS[s];
	if (st == "joined") {
		recent_add(TM_JOIN_ADDR, TM_JOIN_PORT, TM_JOIN_NAME);
		recent_save();
		return "join";
	}
	if (st.starts_with("refused: ")) { tm_refused(st.substr(9, st.size() - 9)); return ""; }
	if (st.starts_with("closed: ")) { tm_closed(st.substr(8, st.size() - 8)); return ""; }
	if (!TM_SAID && NCS_CH[s].connected()) {
		net_client_hello(s, TM_JOIN_NAME, TM_HUE);
		TM_SAID = true;
	}
	if (NOW_MS - TM_T0 > TM_CONNECT_MS) {
		tm_cancel();
		tm_fail("UNREACHABLE", `No answer from ${TM_JOIN_ADDR}:${TM_JOIN_PORT} in ${TM_CONNECT_MS / 1000} seconds. Check the address, and that the host's firewall lets Gator Quest through.`);
	}
	return "";
}

// one key: "" to stay, else single, host (already listening), join (welcomed) or quit
string title_key(string k) {
	if (TM_PAGE == "connecting") {
		if (k == "esc") {
			tm_cancel();
			tm_open("join");
			TM_SEL = 2;
		}
		return "";
	}
	if (TM_PAGE == "failed") {
		if (k == "esc" || k == "enter" || k == "space") {
			tm_open("join");
			TM_SEL = TM_FAIL_TITLE == "NAME TAKEN" || TM_FAIL_TITLE == "DISCONNECTED" ? 2 : 0;
		}
		return "";
	}
	int n = tm_rows();
	if (k == "up") { TM_SEL = (TM_SEL + n - 1) % n; return ""; }
	if (k == "down") { TM_SEL = (TM_SEL + 1) % n; return ""; }
	if (TM_PAGE == "main") {
		if (k == "esc") { return "quit"; }
		if (k == "left") { TM_HUE = (TM_HUE + 15) % 16; }
		else if (k == "right") { TM_HUE = (TM_HUE + 1) % 16; }
		else if (k == "enter") {
			if (TM_SEL == 0) { return "single"; }
			if (TM_SEL == 1) { tm_open("host"); TM_SEL = 1; return ""; }
			if (TM_SEL == 2) { tm_open("join"); if (TM_ADDR != "") { TM_SEL = 2; } return ""; }
			return "quit";
		}
		else { tm_edit_name(k); }
		return "";
	}
	if (TM_PAGE == "host") {
		if (k == "esc" || (k == "enter" && TM_SEL == 2)) { tm_open("main"); TM_SEL = 1; return ""; }
		if (k == "enter") { return tm_host() ? "host" : ""; }
		tm_edit_digits(TM_PORT, k);
		return "";
	}
	if (TM_PAGE == "join") {
		int back = 3 + RECENT_ADDR.size();
		if (k == "esc" || (k == "enter" && TM_SEL == back)) { tm_open("main"); TM_SEL = 2; return ""; }
		if (k == "enter") {
			if (TM_SEL >= 3) {
				int r = TM_SEL - 3;
				TM_ADDR = RECENT_ADDR[r];
				TM_JPORT = to_string(RECENT_PORT[r]);
				TM_NAME = RECENT_NAME[r];
			}
			tm_connect();
			return "";
		}
		if (TM_SEL == 0) { tm_edit_address(k); }
		else if (TM_SEL == 1) { tm_edit_digits(TM_JPORT, k); }
		else if (TM_SEL == 2) { tm_edit_name(k); }
		else if (TM_SEL < back && (k == "delete" || k == "backspace")) {
			recent_remove(TM_SEL - 3);
			recent_save();
			TM_SEL = imin(TM_SEL, 2 + RECENT_ADDR.size());
		}
		return "";
	}
	return "";
}

// ---- drawing
string tm_row(string label, bool sel, int w) {
	if (sel) { return pane_line(SGR_SEL, " > " + label + string::repeat(" ", w), w); }
	return pane_line(SGR_TEXT, "   " + label, w);
}

void tm_wrapped(array<string>& out, string sgr, string text, int w) {
	array<string> wl = wrap(text, w - 2);
	for (int k = 0; k < wl.size(); ++k) { out.push(pane_line(sgr, " " + wl[k], w)); }
}

array<string> title_pane(int w, int rows) {
	array<string> out;
	pane_begin(w);
	pane_add("1;38;2;40;30;10;48;2;120;190;70", " \xe2\x98\xbc GATOR QUEST ");
	pane_add("38;2;120;190;70", " " + string::repeat("\xe2\x96\x93", imax(0, w - 16)));
	out.push(pane_end());
	string cursor = FRAME % 20 < 10 ? "_" : " ";
	string red = "38;2;240;100;80";
	if (TM_PAGE == "main") {
		out.push(rule("PLAY", w));
		for (int i = 0; i < TM_MAIN.size(); ++i) { out.push(tm_row(TM_MAIN[i], i == TM_SEL, w)); }
		array<string> story = [
			"", "The swamp is in trouble. Six tyrants have seized the six realms beyond the Lilygates: Big Mama Snapper, the Drowned King, Count Crocula, the Glacier Queen, Emberjaw the Wyrm and the Thunder Roc.",
			"", "Each hoards a Relic Scale. Bring all six to the Shrine of the Six and become the GRAND GATOR.", ""];
		for (int i = 0; i < story.size() && out.size() + 6 < rows; ++i) {
			if (story[i] == "") { out.push(pane_line(SGR_DIM, "", w)); }
			else { tm_wrapped(out, SGR_TEXT, story[i], w); }
		}
		out.push(rule("CONTROLS", w));
		array<string> keys = ["arrows/WASD move, bump to fight", "SPACE use  F target  G grab", "1-9 cast  I pack  ? help"];
		for (int i = 0; i < keys.size(); ++i) { out.push(pane_line(SGR_DIM, " " + keys[i], w)); }
		array<string> saved = saved_gators();
		if (saved.size() > 0 && out.size() + 2 < rows) {
			out.push(rule("SAVED GATORS", w));
			for (int i = 0; i < saved.size() && out.size() < rows; ++i) { out.push(pane_line(SGR_GOLD, " " + saved[i], w)); }
		}
	} else if (TM_PAGE == "host") {
		out.push(rule("HOST A GAME", w));
		out.push(tm_row("Port: " + TM_PORT + (TM_SEL == 0 ? cursor : ""), TM_SEL == 0, w));
		out.push(tm_row("Start hosting", TM_SEL == 1, w));
		out.push(tm_row("Back", TM_SEL == 2, w));
		out.push(pane_line(SGR_DIM, "", w));
		string port = TM_PORT == "" ? "?" : TM_PORT;
		out.push(pane_line(SGR_TEXT, " Friends join with:", w));
		for (int i = 0; i < TM_ADDRS.size() && out.size() + 4 < rows; ++i) { out.push(pane_line(SGR_GOLD, `   ${TM_ADDRS[i]}  port ${port}`, w)); }
		if (TM_HOSTNAME != "") { out.push(pane_line(SGR_GOLD, `   ${TM_HOSTNAME}  port ${port}`, w)); }
		if (TM_ADDRS.size() == 0) { out.push(pane_line(SGR_DIM, "   127.0.0.1 (this machine only)", w)); }
		out.push(pane_line(SGR_DIM, "", w));
		tm_wrapped(out, SGR_DIM, `Over the internet, forward TCP port ${port} on your router to this machine.`, w);
		if (TM_HOST_ERR != "") {
			out.push(pane_line(SGR_DIM, "", w));
			tm_wrapped(out, red, TM_HOST_ERR, w);
		}
	} else if (TM_PAGE == "join") {
		out.push(rule("JOIN A GAME", w));
		out.push(tm_row("Address: " + TM_ADDR + (TM_SEL == 0 ? cursor : ""), TM_SEL == 0, w));
		out.push(tm_row("Port: " + TM_JPORT + (TM_SEL == 1 ? cursor : ""), TM_SEL == 1, w));
		out.push(tm_row("Connect as " + tm_shown_name() + (TM_SEL == 2 ? cursor : ""), TM_SEL == 2, w));
		out.push(rule("RECENT SERVERS", w));
		for (int i = 0; i < RECENT_ADDR.size(); ++i) {
			string port = RECENT_PORT[i] == 28777 ? "" : ":" + to_string(RECENT_PORT[i]);
			out.push(tm_row(`${RECENT_ADDR[i]}${port} as ${RECENT_NAME[i]}`, TM_SEL == 3 + i, w));
		}
		if (RECENT_ADDR.size() == 0) { out.push(pane_line(SGR_DIM, "   (none yet)", w)); }
		out.push(tm_row("Back", TM_SEL == 3 + RECENT_ADDR.size(), w));
		if (TM_SEL >= 3 && TM_SEL < 3 + RECENT_ADDR.size()) {
			out.push(pane_line(SGR_DIM, "", w));
			tm_wrapped(out, SGR_DIM, "enter: join as that name   backspace: forget it", w);
		}
	} else if (TM_PAGE == "connecting") {
		out.push(rule("CONNECTING", w));
		out.push(pane_line(SGR_GOLD, `   ${TM_JOIN_ADDR}:${TM_JOIN_PORT}`, w));
		out.push(pane_line(SGR_TEXT, "   as " + TM_JOIN_NAME, w));
		out.push(pane_line(SGR_DIM, "", w));
		string step = TM_SAID ? "waiting for the host to let us in" : "opening the connection";
		int dots = (FRAME / 8) % 4;
		out.push(pane_line(SGR_TEXT, "   " + step + string::repeat(".", dots), w));
		out.push(pane_line(SGR_DIM, `   ${imax(0, NOW_MS - TM_T0) / 1000} s`, w));
	} else {
		out.push(rule(TM_FAIL_TITLE, w));
		tm_wrapped(out, red, TM_FAIL, w);
	}
	while (out.size() < rows) { out.push(pane_line(SGR_DIM, "", w)); }
	return out;
}

string title_keys_hint() {
	if (TM_PAGE == "main") { return " type your name   left/right: gator color   up/down, enter: choose   esc: quit"; }
	if (TM_PAGE == "host") { return " type the port   up/down, enter: choose   esc: back"; }
	if (TM_PAGE == "join") { return " type on a field   up/down, enter: choose   esc: back"; }
	if (TM_PAGE == "connecting") { return " esc: cancel"; }
	return " enter or esc: back";
}

string title_prompt(bool has_save) {
	if (TM_PAGE == "host") { return "ENTER TO START HOSTING"; }
	if (TM_PAGE == "join") { return "ENTER TO CONNECT"; }
	if (TM_PAGE == "connecting") { return "CONNECTING" + string::repeat(".", (FRAME / 8) % 4); }
	if (TM_PAGE == "failed") { return TM_FAIL_TITLE; }
	if (TM_SEL == 0) { return has_save ? "SAVED WORLD - ENTER TO PLAY" : "TYPE A NAME, ENTER TO BEGIN"; }
	return "ENTER: " + string::to_upper(TM_MAIN[TM_SEL]);
}

// the names of the character files in the save folder (as typed: underscores read as spaces)
array<string> saved_gators() {
	array<string> out;
	if (!SAVES_ON || !dir_exists(save_dir() + "/chars")) { return out; }
	array<string> files = list_dir(save_dir() + "/chars");
	for (int i = 0; i < files.size(); ++i) {
		if (!files[i].ends_with(".gqc")) { continue; }
		string n = files[i].substr(0, files[i].size() - 4);
		n.replace("_", " ");
		out.push(n);
	}
	return out;
}

// the title circles the lake by Gatorland's Lilygate: river, bridge, beach and meadow
int TITLE_LAKE_X = 40;
int TITLE_LAKE_Y = 41;

// runs until a choice: single, host (listening), join (welcomed on TM_SESSION) or quit.
// CHOSEN_NAME / CHOSEN_HUE hold the gator.
string title_screen(bool has_save, int auto_frames) {
	CV = new ClientView();
	CV.slot = -1;
	int last = loop_clock_ms();
	int acc = 0;
	float ang = 0.0;
	int cx = TITLE_LAKE_X;
	int cy = TITLE_LAKE_Y;
	music_play(-1);
	int frames = 0;
	TM_F0 = FRAME;
	while (true) {
		int frame_start = time::now() * 1000;
		NOW_MS = loop_clock_ms();
		int dt = imin(250, NOW_MS - last);
		last = NOW_MS;
		acc = acc + dt;
		frames = frames + 1;
		if (auto_frames > 0 && frames > auto_frames) {
			CHOSEN_NAME = "Demo Gator";
			CHOSEN_HUE = 5;
			return "single";
		}
		string choice = "";
		string k = read_key();
		while (k != "" && choice == "") {
			choice = title_key(k);
			k = read_key();
		}
		if (choice == "") { choice = title_update(); }
		if (choice != "") {
			CHOSEN_NAME = choice == "join" ? TM_JOIN_NAME : tm_shown_name();
			CHOSEN_HUE = TM_HUE;
			return choice;
		}
		while (acc >= TICK_MS) {
			// a slow circle over Swampton keeps the townsfolk and critters awake
			ang = ang + 0.004;
			TITLE_X = cx + math::itrunc(math::cos(ang) * 7.0);
			TITLE_Y = cy + math::itrunc(math::sin(ang) * 4.0);
			CAMERA_W = 0;
			CAMERA_X = TITLE_X;
			CAMERA_Y = TITLE_Y;
			sim_tick();
			CV.rx_ms = NOW_MS;
			cv_apply(CV, snapshot_view(-1, 0, TITLE_X, TITLE_Y));
			acc = acc - TICK_MS;
		}
		title_time_lapse(CV, FRAME - TM_F0);
		process_events(CV);
		particles_update();
		string out = title_frame(has_save);
		print(HOME + out, skip_newline);
		FRAME = FRAME + 1;
		loop_frame_end(frame_start);
	}
	return "";
}

// the world behind, the logo, the gator preview and the prompt; the page in the pane
string title_frame(bool has_save) {
	render_world(CV);
	int t = FRAME - TM_F0;
	int scale = VW >= 145 ? 4 : (VW >= 110 ? 3 : 2);
	string logo = "GATOR QUEST";
	int lw = logo_width(logo, scale);
	int ly = VH / 8 + 2;
	draw_logo(logo, (VW - lw) / 2, ly, scale, t);
	string sub = TM_PAGE == "host" ? "HOST A GAME" : (TM_PAGE == "join" || TM_PAGE == "connecting" || TM_PAGE == "failed" ? "JOIN A GAME" : "A SWAMP IN NEED OF A HERO");
	if (TM_PAGE == "main") { sub = sub.substr(0, iclamp(t - 40, 0, sub.size())); }
	draw_text_px(sub, (VW - text_px_width(sub, 1)) / 2, ly + 5 * scale + 6, rgb(250, 220, 120), COL_BLACK, 1);
	int gs = VH >= 130 ? 4 : (VH >= 100 ? 3 : 2);
	int gy = ly + 5 * scale + 16;
	draw_title_gator(TM_HUE, VW / 2, gy, gs);
	int below = gy + 8 * gs + 3;
	string prompt = "NAME: " + string::to_upper(tm_shown_name()) + (FRAME % 20 < 10 && TM_PAGE == "main" ? "~" : " ");
	draw_text_px(prompt, (VW - text_px_width(prompt, 1)) / 2, below, rgb(255, 255, 255), COL_BLACK, 1);
	string huel = "< " + string::to_upper(HUE_NAMES[TM_HUE]) + " >";
	draw_text_px(huel, (VW - text_px_width(huel, 1)) / 2, below + 7, rgb(200, 230, 180), COL_BLACK, 1);
	string go = title_prompt(has_save);
	int gc = TM_PAGE == "failed" ? rgb(250, 110, 90) : (FRAME % 30 < 20 ? rgb(255, 230, 120) : rgb(200, 170, 80));
	draw_text_px(go, (VW - text_px_width(go, 1)) / 2, VH - 8, gc, COL_BLACK, 1);
	array<string> pane = title_pane(PANE_WIDTH, VIEW_ROWS);
	string out = "";
	for (int r = 0; r < VIEW_ROWS; ++r) { out += canvas_row(r) + pane[r] + "\n"; }
	pane_begin(SCREEN_COLS);
	pane_add(SGR_DIM, title_keys_hint());
	out += pane_end();
	return out;
}

// dev: every title page as a capture (shot title [out DIR])
int run_title_shots() {
	string dir = arg_value("out", data_root() + "/shots");
	make_dirs(dir);
	SAVES_ON = false;
	sim_init(1);
	string size = arg_value("screen", "150x44");
	array<string> wh = size.split("x");
	layout(wh[0].to_int(150), wh.size() > 1 ? wh[1].to_int(44) : 44);
	CV = new ClientView();
	CV.slot = -1;
	TITLE_X = TITLE_LAKE_X + 7;
	TITLE_Y = TITLE_LAKE_Y;
	CAMERA_W = 0;
	CAMERA_X = TITLE_X;
	CAMERA_Y = TITLE_Y;
	for (int t = 0; t < 20; ++t) {
		sim_tick();
		cv_apply(CV, snapshot_view(-1, 0, TITLE_X, TITLE_Y));
	}
	title_setup("", false, 28777);
	// a couple of seconds of the title's sky and its critters, so the pages show what a player sees
	TM_F0 = FRAME;
	for (int f = 0; f < 75; ++f) {
		NOW_MS = NOW_MS + 33;
		title_time_lapse(CV, FRAME - TM_F0);
		process_events(CV);
		particles_update();
		render_world(CV);
		FRAME = FRAME + 1;
	}
	int frame_hash = 0;
	TM_NAME = "Chomps";
	TM_HUE = 3;
	RECENT_ADDR = ["192.168.1.20", "gatorbox.local"];
	RECENT_PORT = [28777, 30000];
	RECENT_NAME = ["Chomps", "Snappy"];
	array<string> pages = ["main", "host", "join", "connecting", "failed"];
	for (int i = 0; i < pages.size(); ++i) {
		tm_open(pages[i]);
		if (pages[i] == "host") { TM_ADDRS = ["192.168.1.20"]; TM_HOSTNAME = "gatorbox"; }
		if (pages[i] == "join") { TM_ADDR = "192.168.1.20"; TM_SEL = 3; }
		if (pages[i] == "connecting") { TM_JOIN_ADDR = "192.168.1.20"; TM_JOIN_PORT = 28777; TM_JOIN_NAME = "Chomps"; TM_SAID = true; TM_T0 = NOW_MS - 2000; }
		if (pages[i] == "failed") { TM_FAIL_TITLE = "NAME TAKEN"; TM_FAIL = "The host said: a gator named Chomps is already here. Type another name on the Connect row and try again."; }
		string frame = title_frame(false);
		frame_hash = mix32(frame_hash, text_checksum(frame));
		string name = dir + "/title_" + pages[i];
		write_file(name + ".ans", frame);
		string sketch = "";
		array<string> pane = title_pane(PANE_WIDTH, VIEW_ROWS);
		string ramp = " .:-=+*#%@";
		for (int r = 0; r < VIEW_ROWS; ++r) {
			string row = "";
			for (int x = 0; x < VW; ++x) {
				int a = PIX[(r * 2) * VW + x];
				int b = PIX[(r * 2 + 1) * VW + x];
				int lum = (PAL_R[a] * 3 + PAL_G[a] * 5 + PAL_B[a] * 2 + PAL_R[b] * 3 + PAL_G[b] * 5 + PAL_B[b] * 2) / 20;
				row += ramp.substr(iclamp(lum * 10 / 256, 0, 9), 1);
			}
			sketch += row + strip_sgr(pane[r]) + "\n";
		}
		write_file(name + ".txt", sketch + title_keys_hint() + "\n");
		png_write_canvas(name + ".png", 3);
	}
	print(`wrote ${pages.size()} title page captures to ${dir} (FRAME_HASH ${frame_hash})`);
	return 0;
}
