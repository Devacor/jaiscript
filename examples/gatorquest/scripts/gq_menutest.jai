// Gator Quest - the menu test: the title's host and join pages driven by key
// names (the same title_key/title_update the screen uses), against a real host
// in this process over loopback. Hosting, a busy port, joining, every refusal
// reason, an unreachable port, cancelling, the recent servers list, the ESC
// menu's host/leave entries, and sessions ending back at the title.
//
//   jaiscript gatorquest.jai -- menutest [port N] [tag X]

import "gq_predtest.jai";

void mt_keys(string keys) {
	array<string> ks = keys.split(" ");
	for (int i = 0; i < ks.size(); ++i) {
		if (ks[i] != "") { title_key(ks[i]); }
	}
}

void mt_type(string text) {
	for (int i = 0; i < text.size(); ++i) {
		string ch = text.substr(i, 1);
		title_key(ch == " " ? "space" : string::to_lower(ch));
	}
}

void mt_clear_field(int n) {
	for (int i = 0; i < n; ++i) { title_key("backspace"); }
}

// run a join to its end (joined, a failure page, or the timeout); the host answers in between
string mt_connect(int proto_sent) {
	string choice = "";
	for (int i = 0; i < 1500 && TM_PAGE == "connecting" && choice == ""; ++i) {
		NOW_MS = time::now() * 1000;
		int proto = NET_PROTO;
		NET_PROTO = proto_sent;
		choice = title_update();
		NET_PROTO = proto;
		net_host_pump();
		sleep(0.002);
	}
	return choice != "" ? choice : TM_PAGE;
}

void mt_host_rounds(int n) {
	for (int i = 0; i < n; ++i) {
		net_host_pump();
		sleep(0.002);
	}
}

// the join page with the address, port and name fields typed in fresh
void mt_join_page(string addr, string port, string name) {
	tm_open("join");
	mt_clear_field(60);
	mt_type(addr);
	title_key("down");
	mt_clear_field(6);
	mt_type(port);
	title_key("down");
	TM_NAME = "";
	mt_type(name);
	NOW_MS = time::now() * 1000;
}

int mt_menu_index(int arg) {
	for (int i = 0; i < MENU_ARG.size(); ++i) {
		if (MENU_ARG[i] == arg) { return i; }
	}
	return -1;
}

int active_chars() {
	int n = 0;
	for (int s = 0; s < CHARS.size(); ++s) {
		if (CHARS[s].active) { n = n + 1; }
	}
	return n;
}

int run_menutest(string base_dir, int port) {
	SAVE_DIR = base_dir + "/menutest";
	make_dirs(SAVE_DIR + "/chars");
	array<string> old = list_dir(SAVE_DIR + "/chars");
	for (int i = 0; i < old.size(); ++i) { delete_file(SAVE_DIR + "/chars/" + old[i]); }
	if (file_exists(SAVE_DIR + "/servers.txt")) { delete_file(SAVE_DIR + "/servers.txt"); }
	if (file_exists(SAVE_DIR + "/world.gqw")) { delete_file(SAVE_DIR + "/world.gqw"); }
	SAVES_ON = true;
	TITLE_LAST = "";
	print("menus:");
	sim_init(5);
	BOTS = [];
	NOW_MS = time::now() * 1000;
	string ps = to_string(port);

	// ---- hosting from the title: a busy port is reported on the page, then the real one opens
	title_setup("", false, 28777);
	mt_type("Hostess");
	check(tm_shown_name() == "Hostess", "the name is typed on the first page");
	mt_keys("right right");
	check(TM_HUE == 2, "left/right pick the color");
	mt_keys("down enter");
	check(TM_PAGE == "host" && TM_SEL == 1, "Host a game opens the host page on Start hosting");
	shared_ptr<NetServer> blocker = new NetServer();
	bool blocked = blocker.listen(port + 1);
	title_key("up");
	mt_clear_field(6);
	mt_type(to_string(port + 1));
	string choice = title_key("enter");
	check(!blocked || (choice == "" && TM_HOST_ERR.contains("Could not open port") && !net_hosting()), `a busy port stays on the host page with the reason (${TM_HOST_ERR})`);
	blocker.close();
	mt_clear_field(6);
	mt_type(ps);
	choice = title_key("enter");
	check(choice == "host" && net_hosting() && NET_PORT == port, `Start hosting listens on the typed port (${choice}, ${NET_PORT})`);
	CHOSEN_NAME = tm_shown_name();
	CHOSEN_HUE = TM_HUE;
	string welcome = session_start(choice, false);
	check(CHARS[LOCAL_SLOT].active && CHARS[LOCAL_SLOT].name == "Hostess" && welcome.contains(`Hosting on port ${port}`), "the host's gator joins its own world, told what friends type");

	// ---- joining: a welcome, then each reason a host turns a gator away
	mt_join_page("127.0.0.1", ps, "Visitor");
	check(TM_ADDR == "127.0.0.1" && TM_JPORT == ps && tm_shown_name() == "Visitor", "the join page takes the address, port and name");
	title_key("enter");
	check(TM_PAGE == "connecting", "enter on Connect starts connecting");
	choice = mt_connect(NET_PROTO);
	check(choice == "join" && NCS_STATUS[TM_SESSION] == "joined", `the host lets the visitor in (${choice})`);
	check(RECENT_ADDR.size() == 1 && RECENT_ADDR[0] == "127.0.0.1" && RECENT_PORT[0] == port && RECENT_NAME[0] == "Visitor", "a successful join is remembered with its name");
	int visitor = TM_SESSION;
	net_session_bye(visitor);
	mt_host_rounds(10);
	check(active_chars() == 1, "the visitor's goodbye frees its slot");

	mt_join_page("127.0.0.1", ps, "Hostess");
	title_key("enter");
	mt_connect(NET_PROTO);
	check(TM_PAGE == "failed" && TM_FAIL_TITLE == "NAME TAKEN", `a taken name is refused as NAME TAKEN (${TM_FAIL_TITLE}: ${TM_FAIL})`);
	title_key("enter");
	check(TM_PAGE == "join" && TM_SEL == 2, "back from NAME TAKEN lands on the name row");

	NET_MAX_PLAYERS = 1;
	mt_join_page("127.0.0.1", ps, "Crowd");
	title_key("enter");
	mt_connect(NET_PROTO);
	NET_MAX_PLAYERS = 16;
	check(TM_PAGE == "failed" && TM_FAIL_TITLE == "SERVER FULL", `a full host is refused as SERVER FULL (${TM_FAIL_TITLE})`);

	mt_join_page("127.0.0.1", ps, "Oldtimer");
	title_key("enter");
	mt_connect(NET_PROTO + 1);
	check(TM_PAGE == "failed" && TM_FAIL_TITLE == "VERSION MISMATCH", `another protocol is refused as VERSION MISMATCH (${TM_FAIL_TITLE})`);

	mt_join_page("127.0.0.1", to_string(port + 2), "Nobody");
	title_key("enter");
	time::stopwatch refusal;
	refusal.start();
	mt_connect(NET_PROTO);
	check(TM_PAGE == "failed" && TM_FAIL_TITLE == "UNREACHABLE" && TM_FAIL.contains(`127.0.0.1:${port + 2}`), `a port nobody listens on is UNREACHABLE in ${refusal.elapsed() * 1000:.0f} ms (${TM_FAIL})`);

	mt_join_page("no-such-host.invalid", ps, "Lost");
	title_key("enter");
	mt_connect(NET_PROTO);
	check(TM_PAGE == "failed" && TM_FAIL_TITLE == "UNREACHABLE" && TM_FAIL.contains("host not found"), `an unknown host name is UNREACHABLE: host not found (${TM_FAIL_TITLE})`);

	mt_join_page("127.0.0.1", ps, "Quitter");
	title_key("enter");
	int cancelled = TM_SESSION;
	title_key("esc");
	mt_host_rounds(10);
	check(TM_PAGE == "join" && !net_session_live(cancelled) && active_chars() == 1, "esc while connecting cancels back to the join page");

	// ---- the recent list: picked, it fills the fields and joins under the remembered name
	mt_join_page("", "", "");
	title_key("down");
	check(TM_SEL == 3 && RECENT_ADDR.size() == 1, "the recent server follows the Connect row");
	title_key("enter");
	choice = mt_connect(NET_PROTO);
	check(choice == "join" && TM_JOIN_NAME == "Visitor" && TM_ADDR == "127.0.0.1" && TM_JPORT == ps, `a recent server joins with its address, port and name (${choice} as ${TM_JOIN_NAME})`);
	net_session_bye(TM_SESSION);
	mt_host_rounds(10);
	recent_load();
	check(RECENT_ADDR.size() == 1 && RECENT_NAME[0] == "Visitor" && read_file(SAVE_DIR + "/servers.txt").starts_with("GATORQUEST-SERVERS 1"), "the recent servers live in saves/servers.txt");

	mt_join_page("127.0.0.1:" + ps, "", "Colon");
	title_key("enter");
	choice = mt_connect(NET_PROTO);
	check(choice == "join" && TM_JOIN_PORT == port, "an address typed as host:port connects to that port");
	net_session_bye(TM_SESSION);
	mt_host_rounds(10);
	check(RECENT_ADDR.size() == 1 && RECENT_NAME[0] == "Colon", "joining a remembered server again updates its name, not the list length");
	tm_open("join");
	TM_SEL = 3;
	title_key("backspace");
	check(RECENT_ADDR.size() == 0, "backspace on a recent server forgets it");

	// ---- the ESC menu: a host sees Stop hosting and Save and leave to title
	UI_MODE = "menu";
	build_menu(CV);
	check(mt_menu_index(9) >= 0 && mt_menu_index(8) < 0 && mt_menu_index(10) >= 0, "a host's menu offers Stop hosting and Leave to title");
	UI_SEL = mt_menu_index(10);
	on_menu_key(CV, "enter");
	check(QUIT && PLAY_EXIT == "title", "Leave to title ends the session toward the title");
	QUIT = false;
	session_end();
	check(!net_hosting() && TM_PAGE == "main" && file_exists(SAVE_DIR + "/world.gqw") && file_exists(char_path("Hostess")), "leaving stops hosting and saves the world and the gator");

	// ---- back at the title: the world reloads without players, the gator comes back
	bool resumed = load_world_for_session();
	check(resumed && active_chars() == 0, "the title reloads the saved world with nobody in it");
	CHOSEN_NAME = "Hostess";
	welcome = session_start("single", false);
	check(welcome.starts_with("Welcome back, Hostess") && active_chars() == 1 && !NET_CLIENT, "single player brings the saved gator back");
	UI_MODE = "menu";
	build_menu(CV);
	int host_entry = mt_menu_index(8);
	check(host_entry >= 0 && mt_menu_index(9) < 0, "single player's menu offers Host this game");
	TM_PORT = ps;
	UI_SEL = host_entry;
	on_menu_key(CV, "enter");
	check(net_hosting() && NET_PORT == port, "Host this game opens the port mid-game");
	UI_MODE = "menu";
	build_menu(CV);
	UI_SEL = mt_menu_index(9);
	on_menu_key(CV, "enter");
	check(!net_hosting(), "Stop hosting closes it again");

	// ---- a joined player whose host goes away lands on the title's DISCONNECTED page
	check(net_host_start(port), "the host opens again");
	mt_join_page("127.0.0.1", ps, "Stranded");
	title_key("enter");
	choice = mt_connect(NET_PROTO);
	NET_SESSION = TM_SESSION;
	NET_CLIENT = true;
	CV = NCS_VIEW[TM_SESSION];
	UI_MODE = "menu";
	build_menu(CV);
	check(choice == "join" && mt_menu_index(1) < 0 && mt_menu_index(8) < 0 && mt_menu_index(10) >= 0, "a joined player's menu has no save, load or host entries");
	UI_MODE = "play";
	net_host_stop("the host has left the swamp");
	QUIT = false;
	for (int i = 0; i < 400 && !QUIT; ++i) {
		NOW_MS = time::now() * 1000;
		net_client_pump();
		sleep(0.001);
	}
	check(QUIT && PLAY_EXIT == "title" && NET_CLOSE_WHY.contains("the host has left the swamp"), `the client's session ends toward the title (${NET_CLOSE_WHY})`);
	QUIT = false;
	session_end();
	check(!NET_CLIENT && NET_SESSION == -1 && TM_PAGE == "failed" && TM_FAIL_TITLE == "DISCONNECTED", "the title shows DISCONNECTED with the reason");
	title_key("enter");
	check(TM_PAGE == "join" && TM_SEL == 2, "from there, enter goes back to the join page");

	print(`menutest: ${TEST_PASSES} passed, ${TEST_FAILS} failed`);
	return TEST_FAILS;
}
