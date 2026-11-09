// JaiDOOM — playable. Drag this file onto jaiscript.exe (or run from a
// terminal on Windows/Linux/macOS — POSIX terminals emulate held keys from
// auto-repeat, so movement is chunkier there and shift is invisible: use R).
//
//   move: W/S or up/down     strafe: A/D      turn: left/right arrows
//   run: hold shift (Windows) or R to toggle (all platforms)
//   use/open: E or space     fire: ctrl or F
//   weapons: 1 fist, 2 pistol, 3 shotgun, 4 chaingun, 5 rocket launcher
//   automap: tab (or M), zoom -/=, B drops a numbered mark, C clears marks
//   quicksave: Z   quickload: X   menu: esc (resume/new/multiplayer/demos/options/save/load/quit)
//   gamma: G (DOOM's five levels)   smooth motion: U (60 fps eased between the 35 Hz tics, or DOOM's own 35)
//   J: the JaiScript panel (each stage's time and history, the parallel blit; P runs it serially to compare)
//   tune.jai: field of view and the title fire, reloaded while the game runs: edit and save it mid-game
//   skills: I'M TOO YOUNG..ULTRA-VIOLENCE + NIGHTMARE! (fast monsters, corpses respawn)
//   demos: menu DEMOS -> RECORD NEW GAME (tape writes at level exit or quit; REC shows
//          top-right) / PLAY DEMO (filename entry, esc stops playback back to the title);
//          left at the title, the game plays the shipped demo.json between title showings, as DOOM
//          does (on the IWAD it was recorded on), and any key opens the menu
//   cheats (type them): iddqd idkfa idfa idclip idclev## (E#M#)
//                       idbehold + s/i/r/a/l, iddt (map reveal cycle)
//
// First run: with no WAD in wads/ the game fetches Freedoom and (if you say yes) the DOOM
//            shareware episode itself, verified by SHA-256, and decodes the audio (boot.jai).
//            Headless: jaiscript jaidoom.jai -- bootstrap [--no-shareware] [--no-audio]
//           jaiscript jaidoom.jai -- uncapped    (smooth motion with no frame cap: every frame the machine allows)
//           jaiscript jaidoom.jai -- --mute      (no music or sound)
//           jaiscript jaidoom.jai -- --pix half  (1x2 half blocks every terminal font draws, for a font without the
//              default 2x3 sextants; the menu's OPTIONS -> HIGH DETAIL sets either for later runs, and a --pix
//              on the command line wins for its run)
//
// Optional: jaiscript jaidoom.jai -- <wad> <map>       (MAPxx names work: -- freedoom2 MAP01)
//           jaiscript jaidoom.jai -- doom2.wad mypack.wad   (further WADs merge over the first; a
//              map pack starts on its first map, LEVEL SELECT lists every map loaded, and the
//              level chain follows doom's rules with a fall-through to the next map that exists)
//           jaiscript jaidoom.jai -- record demo.json  (demo writes at level exit or quit)
//           jaiscript jaidoom.jai -- playdemo demo.json
//           jaiscript jaidoom.jai -- selftest      (headless playsim check)
// Multiplayer (up to 16 players; menu: MULTIPLAYER -> host/join + address entry;
//              the host's lobby counts joiners and ENTER launches everyone;
//              'o' toggles the frag scoreboard in-game; ANY player's exit ends
//              the level for the whole session — clients hold at the tally
//              ("WAITING FOR HOST", a key = READY, esc reaches only quit),
//              the host shows N/M READY and starts everyone together):
//           jaiscript jaidoom.jai -- host [port]        (deathmatch, default 28666)
//           jaiscript jaidoom.jai -- host [port] coop   (co-op: host runs the monsters,
//           jaiscript jaidoom.jai -- join <host> [port]  joiners adopt map/skill/mode)

import "sim_random.jai";
import "wad.jai";
import "map.jai";
import "vid.jai";
import "render.jai";
import "game.jai";
import "hud.jai";
import "save.jai";
import "demo.jai";
import "netplay.jai";
import "boot.jai";

float TICK_MS = 1000.0 / 35.0;
bool SMOOTH = true;          // U / OPTIONS: frames at up to 60 Hz, the camera and every mobj eased between the 35 Hz tics
bool UNCAPPED = false;       // -- uncapped: smooth with no frame cap, every frame the machine allows
float SMOOTH_MS = 1000.0 / 60.0;
bool MUTE = false;           // -- --mute: no music or sound
string PIX_MODE = "";        // --pix half|sext, else the OPTIONS choice; "" = sextants within the pixel budget
string PIX_SETTING = "";     // the OPTIONS render mode, saved with the config; "" until it is changed
float IP_X = 0.0; float IP_Y = 0.0; float IP_ANG = 0.0; float IP_Z = 41.0;   // the view at the previous tic
float IP_BOBX = 0.0; float IP_BOBY = 0.0;                                     // and the weapon's bob
bool RUN_TOGGLE = false;

int GSTATE = 0;               // 0 title, 1 menu, 2 game, 3 intermission, 4 episode end
int MENU_MODE = 0;            // 0 main, 1 skill pick
int MENU_SEL = 0;
array<string> MENU_ITEMS = [];
bool MID_GAME = false;
string START_MAP = "E1M1";
// intermission tally, one record per finished level
class Tally {
	string mapname = "";              // "map" is the type keyword
	string next = "";
	int kills = 0;
	int items = 0;
	int secrets = 0;
	int secs = 0;
	Tally(string m, string nx, int k, int i, int s, int t) {
		mapname = m; next = nx; kills = k; items = i; secrets = s; secs = t;
	}
}
var WI = null;

string DEMO_ARM = "";         // -- record target path; new_game starts the tape
bool DEMO_QUEUE = false;      // -- playdemo armed; play() starts it instead of the title
string NP_QUEUE_MODE = "";    // -- host/join armed; play() starts the netgame instead
string NP_QUEUE_HOST = "";
int NP_QUEUE_PORT = 28666;
bool NP_QUEUE_COOP = false;   // -- coop flag rides the host/join queue
string MUS_TITLE = "D_INTRO"; // doom2 wads lack D_INTRO/D_INTER; main() picks the fallbacks
string MUS_INTER = "D_INTER";

function demo_finish() -> void {
	if (!DEMO_REC) { return; }
	DEMO_REC = false;
	set_msg(demo_write(DEMO_ARM) ? `demo saved: ${DEMO_ARM}` : "demo write failed");
	DEMO_ARM = "";                                   // one tape per arming: later games don't re-record
}

// esc during playback: drop the tape and return to the title
function demo_stop() -> void {
	DEMO_PLAY = false;
	MID_GAME = false;
	GSTATE = 0;
}

function demo_play_start() -> void {
	SKILL_BIT = DEMO_SKILL;
	NIGHTMARE = DEMO_NM;
	FAST_MONSTERS = DEMO_NM;
	player_reset();
	GAME_TIC = 0;                                    // recording started at 0 too
	SIM_RANDOM.seed(DEMO_SEED);
	DEMO_POS = 0;
	DEMO_PLAY = true;
	game_start_map(DEMO_MAP);
	am_marks_reset();
	MID_GAME = true;
	GSTATE = 2;
}

// the title's attract loop, as DOOM runs it: the title for a while, then the shipped demo, then the title again; any
// key opens the menu. Only on the IWAD the tape was recorded on, with nothing merged over it: elsewhere it desyncs.
bool ATTRACT = false;
bool ATTRACT_OFF = false;     // no tape this WAD can play: the title stays
float TITLE_MS = 0.0;         // time on the title since it last came up
float ATTRACT_WAIT_MS = 170 * 1000.0 / 35.0;   // DOOM's 170 tics of TITLEPIC

function attract_start() -> void {
	if (NET_GAME) { TITLE_MS = 0.0; return; }
	if (WAD_FILES.size() != 1 || !demo_load(game_root() + "/demo.json") || DEMO_WAD != WAD_BASE || lump_num(DEMO_MAP) < 0) {
		ATTRACT_OFF = true;
		return;
	}
	wipe_begin();
	demo_play_start();
	MID_GAME = false;
	ATTRACT = true;
}

function attract_end(bool to_menu) -> void {
	ATTRACT = false;
	DEMO_PLAY = false;
	LEVEL_DONE = false;
	TITLE_MS = 0.0;
	GSTATE = 0;
	if (to_menu) { menu_open(); }
	else { wipe_begin(); FIRE_W = 0; }               // back on the title: the fire lights again from cold
}

function read_input() -> void {
	if (DEMO_PLAY) {
		if (!demo_feed()) {                          // tape ran out: hand the controls over
			CMD_FWD = 0.0; CMD_SIDE = 0.0; CMD_TURN = 0.0;
			CMD_USE = false; CMD_FIRE = false;
			if (!ATTRACT) { set_msg("demo end"); }
		}
		return;
	}
	if (P_HP <= 0) {
		CMD_FWD = 0.0; CMD_SIDE = 0.0; CMD_TURN = 0.0;
		CMD_USE = false; CMD_FIRE = false;
		return;
	}
	bool run = key_down("shift") || RUN_TOGGLE;
	float fwd = 0.0;
	if (key_down("w") || key_down("up")) { fwd = fwd + 1.0; }
	if (key_down("s") || key_down("down")) { fwd = fwd - 1.0; }
	float side = 0.0;
	if (key_down("d")) { side = side + 1.0; }
	if (key_down("a")) { side = side - 1.0; }
	CMD_FWD = fwd * (run ? 50.0 : 25.0);
	CMD_SIDE = side * (run ? 40.0 : 24.0);
	float turn = 0.0;
	if (key_down("left")) { turn = turn + 1.0; }
	if (key_down("right")) { turn = turn - 1.0; }
	CMD_TURN = turn * (run ? 0.1227 : 0.0614);
	CMD_USE = key_down("e") || key_down("space");
	CMD_FIRE = key_down("ctrl") || key_down("f");
}

function player_reset() -> void {
	P_HP = 100;
	P_ARMOR = 0;
	P_KEYS = 0;
	AMMO_BULLETS = 50;
	AMMO_SHELLS = 0;
	AMMO_ROCKETS = 0;
	AMMO_BULLETS_MAX = 200;
	AMMO_SHELLS_MAX = 50;
	AMMO_ROCKETS_MAX = 50;
	HAVE_SHOTGUN = false;
	HAVE_CHAINGUN = false;
	HAVE_LAUNCHER = false;
	P_BERSERK = false;
	BERSERK_TICS = 0;
	P_INVIS = 0;
	P_SUIT = 0;
	P_GOGGLES = 0;
	P_ALLMAP = false;
	P_BACKPACK = false;
	CUR_WEAPON = 2;
	PAIN_TICS = 0;
	BONUS_TICS = 0;
	P_GOD = false;
	P_NOCLIP = false;
}

function weapon_spr() -> string {
	var w = WEAPONS[CUR_WEAPON];
	return w.spr;
}

function weapon_flash_spr() -> string {
	var w = WEAPONS[CUR_WEAPON];
	return w.flash;
}

// ---------------------------------------------------------------- the flow ---

function menu_build() -> void {
	MENU_ITEMS = [];
	if (MENU_MODE == 1) {
		MENU_ITEMS.push("I'M TOO YOUNG TO DIE");
		MENU_ITEMS.push("HEY, NOT TOO ROUGH");
		MENU_ITEMS.push("HURT ME PLENTY");
		MENU_ITEMS.push("ULTRA-VIOLENCE");
		MENU_ITEMS.push("NIGHTMARE!");
		return;
	}
	if (MENU_MODE == 2) {
		MENU_ITEMS.push("HOST DEATHMATCH");
		MENU_ITEMS.push("HOST CO-OP");
		MENU_ITEMS.push("JOIN GAME");
		MENU_ITEMS.push("BACK");
		return;
	}
	if (MENU_MODE == 4) {
		MENU_ITEMS.push("RECORD NEW GAME");
		MENU_ITEMS.push("PLAY DEMO");
		MENU_ITEMS.push("BACK");
		return;
	}
	if (NP_WI_HOLD) {                                // held at a net intermission:
		MENU_ITEMS.push("RESUME");                   // back to the tally, and quit —
		MENU_ITEMS.push("QUIT");                     // never into stale gameplay
		return;
	}
	if (MENU_MODE == 7) {
		MENU_ITEMS.push(SEXTANTS ? "HIGH DETAIL: ON" : "HIGH DETAIL: OFF");
		MENU_ITEMS.push(SMOOTH ? "SMOOTH MOTION: ON" : "SMOOTH MOTION: OFF");
		MENU_ITEMS.push(`BRIGHTNESS: ${math::itrunc(GAMMA)}`);
		MENU_ITEMS.push("BACK");
		return;
	}
	if (MENU_MODE == 6) {
		for (auto m : map_list()) { MENU_ITEMS.push(m); }
		MENU_ITEMS.push("BACK");
		return;
	}
	if (MID_GAME) { MENU_ITEMS.push("RESUME"); }
	MENU_ITEMS.push("NEW GAME");
	MENU_ITEMS.push("LEVEL SELECT");
	MENU_ITEMS.push("MULTIPLAYER");
	MENU_ITEMS.push("DEMOS");
	MENU_ITEMS.push("OPTIONS");
	if (MID_GAME) { MENU_ITEMS.push("SAVE GAME"); }
	if (file_exists(data_path(SAVE_PATH))) { MENU_ITEMS.push("LOAD GAME"); }
	MENU_ITEMS.push("QUIT");
}

// A long list scrolls: `rows` entries around the selection, arrows marking more above/below
function menu_draw_window(var& entries, int sel, int rows, string title) -> void {
	int n = entries.size();
	if (n <= rows) { menu_draw(entries, sel, title); return; }
	int first = sel - rows / 2;
	if (first < 0) { first = 0; }
	if (first > n - rows) { first = n - rows; }
	var view = [];
	for (int i = first; i < first + rows; ++i) { view.push(entries[i]); }
	string t = title + "  " + (first > 0 ? "^ " : "  ") + to_string(sel + 1) + "/" + to_string(n) + (first + rows < n ? " v" : "");
	menu_draw(view, sel - first, t);
}

function menu_open() -> void {
	if (GSTATE == 2 && MID_GAME && VW > 0) { backdrop_take(); } else { BACKDROP = []; }
	GSTATE = 1;
	MENU_MODE = 0;
	MENU_SEL = 0;
	menu_build();
}

function new_game(int mask, bool nightmare) -> void {
	SKILL_BIT = mask;
	NIGHTMARE = nightmare;
	FAST_MONSTERS = nightmare;
	player_reset();
	if (DEMO_ARM != "") { demo_rec_begin(START_MAP, mask, time::unix_ms() & 16777215); }   // seed before spawns
	game_start_map(START_MAP);
	MID_GAME = true;
	AM_ON = false;
	AM_CHEAT = 0;
	am_marks_reset();
	DEMO_MENU_ARMED = false;                         // the tape is rolling; esc no longer disarms
	GSTATE = 2;
}

string NP_ADDR_EDIT = "";     // the JOIN GAME text field
string NP_LAST_ADDR = "";     // prefill, persisted to jaidoom_net.json
string NET_CFG = "jaidoom_net.json";
string DEMO_NAME_EDIT = "";   // the PLAY DEMO text field
string DEMO_LAST_NAME = "demo.json"; // record target + play prefill: zero-typing round trip
bool DEMO_MENU_ARMED = false; // skill-pick esc disarms a DEMOS-armed tape

function cfg_save() -> void {
	write_file(data_path(NET_CFG), to_json({"addr": NP_LAST_ADDR, "demo": DEMO_LAST_NAME, "pix": PIX_SETTING, "gamma": GAMMA,
		"smooth": SMOOTH}));
}

function addr_save() -> void {
	NP_LAST_ADDR = NP_ADDR_EDIT;
	cfg_save();
}

function addr_load() -> void {
	if (!file_exists(data_path(NET_CFG))) { return; }
	var cfg = from_json(read_file(data_path(NET_CFG)));
	if (cfg == null) { return; }
	if (cfg.has("addr")) { NP_LAST_ADDR = "" + cfg["addr"]; }
	if (cfg.has("demo")) { DEMO_LAST_NAME = "" + cfg["demo"]; }
	if (cfg.has("pix")) { PIX_SETTING = "" + cfg["pix"]; }
	if (cfg.has("gamma")) { GAMMA = cfg["gamma"] + 0.0; }
	if (cfg.has("smooth")) { SMOOTH = cfg["smooth"] == true; }
	if (PIX_MODE == "") { PIX_MODE = PIX_SETTING; }   // --pix on the command line wins for its run
}

// OPTIONS: flip between sextants and half blocks, kept for later runs and applied at once
function render_mode_toggle() -> void {
	PIX_SETTING = SEXTANTS ? "half" : "sext";
	PIX_MODE = PIX_SETTING;
	cfg_save();
	if (SCREEN_COLS > 0) {
		screen_fit();
		print(ESC + "[2J", skip_newline);
	}
	menu_build();
}

// G / OPTIONS: DOOM's five gamma levels, kept for later runs
function gamma_cycle() -> void {
	vid_set_gamma(GAMMA >= 4.0 ? 0.0 : math::itrunc(GAMMA) + 1.0);
	set_msg(`gamma correction level ${math::itrunc(GAMMA)}`);
	cfg_save();
}

function smooth_toggle() -> void {
	SMOOTH = !SMOOTH;
	set_msg(SMOOTH ? "smooth motion: on (60 fps)" : "smooth motion: off (35 fps, as doom drew it)");
	cfg_save();
}

// the frame's reach of tune.jai's import: an edited file re-runs, a broken one raises here and the old values stay
function tune_poll() -> void {
	try {
		if (import("tune.jai")) {
			TUNE_RELOADS += 1;
			TUNE_NOTE = "";
			set_msg(`tune.jai reloaded: fov ${FOV:.0}`);
		}
	} catch (e) {
		array<string> el = to_string(e).lines();
		TUNE_NOTE = el.size() > 0 ? "FAILED: " + el[0] : "FAILED";
		set_msg("tune.jai: reload failed, values kept");
	}
}

// "host", "host:port" or bare port after ':' -> starts the join lobby
function join_go() -> bool {
	var addr = NP_ADDR_EDIT;
	int port = 28666;
	int colon = addr.find(":");
	if (colon >= 0) {
		port = addr.substr(colon + 1, addr.size() - colon - 1).to_int(28666);
		addr = addr.substr(0, colon);
	}
	if (addr == "") { set_msg("no address"); return false; }
	addr_save();
	return np_lobby_join(addr, port);
}

// the address field: printable chars in, backspace out, enter joins
function ip_key(string k) -> bool {
	if (k == "esc") { MENU_MODE = 2; MENU_SEL = 2; menu_build(); return false; }
	if (k == "backspace") {
		if (NP_ADDR_EDIT.size() > 0) { NP_ADDR_EDIT = NP_ADDR_EDIT.substr(0, NP_ADDR_EDIT.size() - 1); }
		return false;
	}
	if (k == "enter") {
		if (join_go()) { GSTATE = 5; }
		return false;
	}
	if (k.size() != 1 || NP_ADDR_EDIT.size() >= 40) { return false; }
	int c = k[0] + 0;
	bool ok = (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 46 || c == 58 || c == 45;
	if (ok) { NP_ADDR_EDIT = NP_ADDR_EDIT + k; }
	return false;
}

// the demo filename field: enter validates + plays, esc backs out
function demo_name_key(string k) -> bool {
	if (k == "esc") { MENU_MODE = 4; MENU_SEL = 1; menu_build(); return false; }
	if (k == "backspace") {
		if (DEMO_NAME_EDIT.size() > 0) { DEMO_NAME_EDIT = DEMO_NAME_EDIT.substr(0, DEMO_NAME_EDIT.size() - 1); }
		return false;
	}
	if (k == "enter") {
		if (!file_exists(data_path(DEMO_NAME_EDIT))) { set_msg(`no demo ${DEMO_NAME_EDIT}`); return false; }
		if (!demo_load(DEMO_NAME_EDIT)) { set_msg("not a demo file"); return false; }
		DEMO_LAST_NAME = DEMO_NAME_EDIT;
		cfg_save();
		demo_play_start();
		return false;
	}
	if (k.size() != 1 || DEMO_NAME_EDIT.size() >= 40) { return false; }
	int c = k[0] + 0;
	bool ok = (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 46 || c == 45 || c == 95;
	if (ok) { DEMO_NAME_EDIT = DEMO_NAME_EDIT + k; }
	return false;
}

// returns true when the program should quit
function menu_key(string k) -> bool {
	if (MENU_MODE == 3) { return ip_key(k); }
	if (MENU_MODE == 5) { return demo_name_key(k); }
	int n = MENU_ITEMS.size();
	if (k == "up") { MENU_SEL = (MENU_SEL + n - 1) % n; }
	else if (k == "down") { MENU_SEL = (MENU_SEL + 1) % n; }
	else if (k == "esc") {
		if (MENU_MODE == 1 && DEMO_MENU_ARMED) { DEMO_ARM = ""; DEMO_MENU_ARMED = false; }
		if (MENU_MODE == 1 || MENU_MODE == 2 || MENU_MODE == 4 || MENU_MODE == 6 || MENU_MODE == 7) { MENU_MODE = 0; MENU_SEL = 0; menu_build(); }
		else if (NP_WI_HOLD) { GSTATE = 3; }
		else if (MID_GAME) { GSTATE = 2; }
		else { GSTATE = 0; }
	} else if (k == "enter" || k == "space" || k == "e") {
		if (MENU_MODE == 1) {
			int mask = 1;                            // skills 1+2 share the easy bit
			if (MENU_SEL == 2) { mask = 2; }
			if (MENU_SEL >= 3) { mask = 4; }         // UV and nightmare share the hard bit
			new_game(mask, MENU_SEL == 4);
		} else if (MENU_MODE == 4) {
			var dsel = MENU_ITEMS[MENU_SEL];
			if (dsel == "BACK") { MENU_MODE = 0; MENU_SEL = 0; menu_build(); }
			else if (dsel == "RECORD NEW GAME") {
				DEMO_ARM = DEMO_LAST_NAME;
				DEMO_MENU_ARMED = true;
				MENU_MODE = 1; MENU_SEL = 2; menu_build();
			} else {
				MENU_MODE = 5;
				DEMO_NAME_EDIT = DEMO_LAST_NAME;
			}
		} else if (MENU_MODE == 7) {
			var osel = MENU_ITEMS[MENU_SEL];
			if (osel == "BACK") { MENU_MODE = 0; MENU_SEL = 0; menu_build(); }
			else if (osel.starts_with("SMOOTH")) { smooth_toggle(); menu_build(); }
			else if (osel.starts_with("BRIGHTNESS")) { gamma_cycle(); menu_build(); }
			else { render_mode_toggle(); }
		} else if (MENU_MODE == 6) {
			var lsel = MENU_ITEMS[MENU_SEL];
			if (lsel == "BACK") { MENU_MODE = 0; MENU_SEL = 0; menu_build(); }
			else { START_MAP = lsel; MENU_MODE = 1; MENU_SEL = 2; menu_build(); }   // the skill pick starts it
		} else if (MENU_MODE == 2) {
			var msel = MENU_ITEMS[MENU_SEL];
			if (msel == "BACK") { MENU_MODE = 0; MENU_SEL = 0; menu_build(); }
			else if (msel == "JOIN GAME") {
				MENU_MODE = 3;
				NP_ADDR_EDIT = NP_LAST_ADDR;
			}
			else if (np_lobby_host(28666, msel == "HOST CO-OP", START_MAP, SKILL_BIT)) { GSTATE = 5; }
		} else {
			var sel = MENU_ITEMS[MENU_SEL];
			if (sel == "RESUME") { GSTATE = NP_WI_HOLD ? 3 : 2; }
			else if (sel == "NEW GAME") { MENU_MODE = 1; MENU_SEL = 2; menu_build(); }
			else if (sel == "LEVEL SELECT") { MENU_MODE = 6; MENU_SEL = 0; menu_build(); }
			else if (sel == "MULTIPLAYER") { MENU_MODE = 2; MENU_SEL = 0; menu_build(); }
			else if (sel == "DEMOS") { MENU_MODE = 4; MENU_SEL = 0; menu_build(); }
			else if (sel == "OPTIONS") { MENU_MODE = 7; MENU_SEL = 0; menu_build(); }
			else if (sel == "SAVE GAME") {
				if (game_save(SAVE_PATH)) { GSTATE = 2; set_msg("game saved"); }
				else { set_msg("save failed"); }
			}
			else if (sel == "LOAD GAME") {
				if (game_load(SAVE_PATH)) { MID_GAME = true; GSTATE = 2; set_msg("game loaded"); }
				else { set_msg("load failed"); }
			}
			else { return true; }
		}
	}
	return false;
}

// ------------------------------------------------------------------ cheats ---
// classic typed sequences: every letter/digit edge rolls into CHEAT_BUF and
// the suffix decides (vanilla-faithful: idclev's digits also switch weapons)

string CHEAT_BUF = "";

function cheat_ack(string m) -> void {
	set_msg(m);
	CHEAT_BUF = "";
}

function cheat_arm(bool keys) -> void {
	HAVE_SHOTGUN = true;
	HAVE_CHAINGUN = true;
	HAVE_LAUNCHER = true;
	AMMO_BULLETS = AMMO_BULLETS_MAX;
	AMMO_SHELLS = AMMO_SHELLS_MAX;
	AMMO_ROCKETS = AMMO_ROCKETS_MAX;
	P_ARMOR = 200;
	if (keys) { P_KEYS = 7; }
}

function cheat_check() -> void {
	var b = CHEAT_BUF;
	if (b.ends_with("iddqd")) {
		P_GOD = !P_GOD;
		if (P_GOD && P_HP < 100) { P_HP = 100; }
		cheat_ack(P_GOD ? "degreelessness mode on" : "degreelessness mode off");
	} else if (b.ends_with("idkfa")) {
		cheat_arm(true);
		cheat_ack("very happy ammo added");
	} else if (b.ends_with("idfa")) {
		cheat_arm(false);
		cheat_ack("ammo (no keys) added");
	} else if (b.ends_with("idclip")) {
		P_NOCLIP = !P_NOCLIP;
		cheat_ack(P_NOCLIP ? "no clipping mode on" : "no clipping mode off");
	} else if (b.ends_with("iddt")) {
		AM_CHEAT = (AM_CHEAT + 1) % 3;
		cheat_ack(AM_CHEAT == 0 ? "map reveal off" : (AM_CHEAT == 1 ? "map reveal: lines" : "map reveal: things"));
	} else if (b.size() >= 9 && b.substr(b.size() - 9, 8) == "idbehold") {
		var pw = b.substr(b.size() - 1, 1);
		if (pw == "s") { P_BERSERK = !P_BERSERK; if (P_BERSERK) { CUR_WEAPON = 1; } cheat_ack("power-up toggled"); }
		else if (pw == "i") { P_INVIS = P_INVIS > 0 ? 0 : 2100; cheat_ack("power-up toggled"); }
		else if (pw == "r") { P_SUIT = P_SUIT > 0 ? 0 : 2100; cheat_ack("power-up toggled"); }
		else if (pw == "a") { P_ALLMAP = !P_ALLMAP; cheat_ack("power-up toggled"); }
		else if (pw == "l") { P_GOGGLES = P_GOGGLES > 0 ? 0 : 2100; cheat_ack("power-up toggled"); }
	} else if (b.ends_with("idbehold")) {
		set_msg("Str, Inviso, Rad, Allmap, or Lite-amp");
	} else if (b.size() >= 8 && b.substr(b.size() - 8, 6) == "idclev") {
		int e = b[b.size() - 2] - 48;
		int mn = b[b.size() - 1] - 48;
		if (e >= 1 && e <= 9 && mn >= 1 && mn <= 9) {
			var wname = `E${e}M${mn}`;
			CHEAT_BUF = "";
			if (lump_num(wname) >= 0 && game_start_map(wname)) {
				MID_GAME = true;
				GSTATE = 2;
				am_marks_reset();
				set_msg(`warped to ${wname}`);
			} else { set_msg(`no map ${wname}`); }
		}
	}
}

function cheat_key(string k) -> void {
	if (k.size() != 1) { return; }
	int c = k[0] + 0;
	if (!((c >= 97 && c <= 122) || (c >= 48 && c <= 57))) { return; }
	CHEAT_BUF = CHEAT_BUF + k;
	if (CHEAT_BUF.size() > 16) { CHEAT_BUF = CHEAT_BUF.substr(CHEAT_BUF.size() - 16, 16); }
	cheat_check();
}

function cheat_feed(string s) -> void {
	for (int i = 0; i < s.size(); ++i) { cheat_key(s.substr(i, 1)); }
}

function game_key(string k) -> void {
	cheat_key(k);
	if (k == "esc") { menu_open(); }
	else if (k == "tab" || k == "m") { AM_ON = !AM_ON; }
	else if (k == "-" || k == "_") { am_zoom(0.8); }
	else if (k == "=" || k == "+") { am_zoom(1.25); }
	else if (k == "b" && AM_ON) { am_mark_add(); }
	else if (k == "c" && AM_ON) { am_mark_clear(); }
	else if (k == "o" && NET_GAME) { NP_SCORE_ON = !NP_SCORE_ON; }
	else if (k == "z") {
		if (game_save(SAVE_PATH)) { set_msg("quicksave"); } else { set_msg("save failed"); }
	}
	else if (k == "x") {
		if (game_load(SAVE_PATH)) { set_msg("quickload"); } else { set_msg("no quicksave yet"); }
	}
	else if (k == "enter") {
		if (P_HP <= 0) { player_reset(); game_start_map(GAME_MAP); am_marks_reset(); }
	}
	else if (k == "r") {
		RUN_TOGGLE = !RUN_TOGGLE;
		set_msg(RUN_TOGGLE ? "run: on" : "run: off");
	}
	else if (k == "u") { smooth_toggle(); }
	else if (k == "g") { gamma_cycle(); }
	else if (k == "1") { weapon_select(1); }
	else if (k == "2") { weapon_select(2); }
	else if (k == "3" && HAVE_SHOTGUN) { weapon_select(3); }
	else if (k == "4" && HAVE_CHAINGUN) { weapon_select(4); }
	else if (k == "5" && HAVE_LAUNCHER) { weapon_select(5); }
}

function pct100(int got, int total) -> int {
	if (total <= 0) { return 100; }
	return got * 100 / total;
}

function wi_start() -> void {
	demo_finish();                                   // a demo ends with its level
	LEVEL_DONE = false;
	var nxt = next_map();
	if (nxt != "" && lump_num(nxt) < 0) { nxt = ""; }
	WI = new Tally(GAME_MAP, nxt, pct100(KILLS, KILL_TOTAL), pct100(ITEMS_TAKEN, ITEM_TOTAL),
		pct100(SECRETS_FOUND, SECRET_TOTAL), (GAME_TIC - LEVEL_START_TIC) / 35);
	GSTATE = 3;
}

function wi_screen() -> void {
	WI_FOOT = "";
	if (NET_GAME) {
		if (!NP_IS_HOST) { WI_FOOT = NP_READY_SENT ? "WAITING FOR HOST" : "PRESS A KEY WHEN READY"; }
		else if (np_ready_need() > 0) {
			WI_FOOT = `${np_ready_have()}/${np_ready_need()} READY`;
			if (np_host_can_start()) { WI_FOOT += " - ANY KEY STARTS"; }
		}
	}
	wi_draw(WI.mapname, WI.next, WI.kills, WI.items, WI.secrets, WI.secs);
}

// the host's synced advance: START travels everyone together ("" at episode
// end sends both sides to the end screen; the session stays up until then)
function np_wi_advance() -> void {
	var nxt = WI.next;
	np_start_announce(nxt);
	if (nxt == "") { GSTATE = 4; wipe_begin(); }
	else { am_marks_reset(); GSTATE = 2; }
}
bool NP_WI_HOLD = false;      // client intermission menu: quit yes, gameplay no

// render and blit grow with the pixel count: 640x240 (320x80 sextant cells) takes ~13 ms of DOOM's 28.5 ms tic
int PIXEL_BUDGET = 160000;
bool BLIT_SERIAL_PICK = false; // P in the J panel: the blit runs serially until P again
int SCREEN_COLS = 0;
int SCREEN_ROWS = 0;
string VIEW_HOME = "";
int VIEW_TOP = 1;             // the view's top-left cell, 1-based
int VIEW_LEFT = 1;
float FIRE_ACC = 0.0;

// the view fills the console: sextants where they fit the budget, half blocks otherwise (or by choice), and a
// screen too large for even those gets the largest view inside the budget, centered
function screen_fit() -> void {
	SCREEN_COLS = term_cols();
	SCREEN_ROWS = term_rows();
	int cols = SCREEN_COLS > 8 ? SCREEN_COLS : 8;
	int rows = SCREEN_ROWS > 5 ? SCREEN_ROWS - 1 : 4;   // a newline on the last row would scroll the screen
	SEXTANTS = PIX_MODE == "sext" || (PIX_MODE == "" && cols * rows * 6 <= PIXEL_BUDGET);
	int pixels = cols * cell_w() * rows * cell_h();
	if (pixels > PIXEL_BUDGET) {
		float k = math::sqrt(1.0 * PIXEL_BUDGET / pixels);
		cols = math::itrunc(cols * k);
		rows = math::itrunc(rows * k);
	}
	if (VW == 0) { vid_init_cells(cols, rows); }     // BEFORE map start: vid_init resets the palette intern
	else { vid_resize(cols * cell_w(), rows * cell_h()); }
	BLIT_WORKERS = cols >= 80 && BLIT_SERIAL_PICK == false ? 1 : 0;   // parallel rows pay off once the barrier amortizes
	hud_layout();
	int left = (SCREEN_COLS - cols) / 2;
	int top = (SCREEN_ROWS - 1 - rows) / 2;
	VID_INDENT = left > 0 ? `${ESC}[${left}C` : "";
	VIEW_TOP = top > 0 ? top + 1 : 1;
	VIEW_LEFT = left > 0 ? left + 1 : 1;
	VIEW_HOME = `${ESC}[${VIEW_TOP};1H`;
}

function play(string mapname) -> int {
	START_MAP = mapname;
	screen_fit();
	hud_init();                                      // after the palette reset vid_init makes

	SND_ON = !MUTE;                                  // only real play makes noise; gates stay silent
	print(ESC + "[?1049h" + ESC + "[?25l" + ESC + "[2J", skip_newline);   // alt screen, no cursor
	float accum = 0.0;
	time::stopwatch frame;
	frame.start();
	bool quit = false;
	GSTATE = 0;
	if (DEMO_QUEUE) { DEMO_QUEUE = false; demo_play_start(); }
	if (NP_QUEUE_MODE != "") {                       // netgame skips the title: straight to the lobby
		if (np_start_mode(mapname, NP_QUEUE_MODE == "host", NP_QUEUE_HOST, NP_QUEUE_PORT, NP_QUEUE_COOP)) {
			GSTATE = 5;
		}
		NP_QUEUE_MODE = "";
	}
	time::stopwatch stage;
	while (!quit) {
		float dt = frame.elapsed() * 1000;
		frame.start();
		stage.start();
		if (term_cols() != SCREEN_COLS || term_rows() != SCREEN_ROWS) {
			screen_fit();
			print(ESC + "[2J", skip_newline);
		}
		if (GSTATE == 2) {
			fps_note(dt);
			accum = accum + dt;
			if (accum > 120.0) { accum = 120.0; }    // stall recovery, GLOOM-style
			while (accum >= TICK_MS) {
				IP_X = P_X; IP_Y = P_Y; IP_ANG = P_ANG; IP_Z = P_VIEWZ;
				IP_BOBX = WBOB_X; IP_BOBY = WBOB_Y;
				lerp_snapshot();
				read_input();
				game_tic();
				accum = accum - TICK_MS;
				if (ATTRACT && (!DEMO_PLAY || LEVEL_DONE)) { break; }
			}
			if (ATTRACT && (!DEMO_PLAY || LEVEL_DONE)) { attract_end(false); }
		} else { accum = 0.0; }
		if (GSTATE == 0) {
			TITLE_MS = TITLE_MS + dt;
			if (TITLE_MS >= ATTRACT_WAIT_MS && !ATTRACT_OFF) { attract_start(); }
		} else if (!ATTRACT) { TITLE_MS = 0.0; }
		if (GSTATE == 0 || (GSTATE == 1 && BACKDROP.empty())) {   // the fire burns behind the title and its menu
			FIRE_ACC = FIRE_ACC + dt;
			if (FIRE_ACC > 120.0) { FIRE_ACC = 120.0; }
			while (FIRE_ACC >= TICK_MS) { fire_tic(); FIRE_ACC = FIRE_ACC - TICK_MS; }
		}
		float sim_ms = stage.elapsed() * 1000;

		var edge = read_key();                       // drain ALL pending edges each frame
		while (edge != "") {
			bool typing = GSTATE == 1 && (MENU_MODE == 3 || MENU_MODE == 5);
			if (!typing && edge == "j") { PANEL_ON = !PANEL_ON; print(ESC + "[2J", skip_newline); }
			else if (!typing && edge == "p" && PANEL_ON) {
				BLIT_SERIAL_PICK = !BLIT_SERIAL_PICK;
				BLIT_WORKERS = BLIT_SERIAL_PICK ? 0 : 1;
			}
			else if (ATTRACT) { attract_end(true); }
			else if (GSTATE == 0) { menu_open(); }
			else if (GSTATE == 1) { if (menu_key(edge)) { quit = true; } }
			else if (GSTATE == 5) {                  // net lobby: esc backs out,
				if (edge == "esc") { np_cancel(); menu_open(); }
				else if (edge == "enter") { np_lobby_go(); }   // host launches with the roster
			}
			else if (GSTATE == 3) {
				if (NET_GAME) {                      // synced advance: clients offer READY,
					if (!NP_IS_HOST) {               // the host STARTs everyone together
						if (!NP_READY_SENT) { NP_READY_SENT = true; np_ready_send(); }
						// esc reaches a held menu (quit is legal, gameplay is not)
						if (edge == "esc") { NP_WI_HOLD = true; menu_open(); }
					} else if (np_host_can_start()) {
						np_wi_advance();
					}
				} else {
					var nxt = WI.next;
					if (nxt == "") { GSTATE = 4; wipe_begin(); }
					else { game_start_map(nxt); am_marks_reset(); GSTATE = 2; }
				}
			}
			else if (GSTATE == 4) {
				if (NET_GAME) { np_bye("episode complete"); }
				GSTATE = 0;
				MID_GAME = false;
			}
			else if (DEMO_PLAY && edge == "esc") { demo_stop(); }
			else { game_key(edge); }
			edge = read_key();
		}

		if (GSTATE == 5) {                           // lobby: pump the handshake
			np_lobby_frame();
			if (NP_LOBBY == "ready") {
				MID_GAME = true;
				AM_ON = false;
				am_marks_reset();
				GSTATE = 2;
			} else if (NP_LOBBY.starts_with("failed")) {
				set_msg(NP_LOBBY);
				NP_LOBBY = "";
				menu_open();
			}
		}
		if (NET_GAME) { np_frame(); }                // pump the wire once per FRAME, not per tic
		if (NP_WI_PEND) {                            // netgame client: host says the level ended
			NP_WI_PEND = false;
			LEVEL_DONE = false;
			WI = new Tally(NP_WI_MAP, NP_WI_NEXT, NP_WI_K, NP_WI_I, NP_WI_S, NP_WI_T);
			NP_READY_SENT = false;
			NP_WI_HOLD = false;
			GSTATE = 3;
			wipe_begin();                            // melt into WAITING FOR HOST
		}
		if (NP_STARTED_PEND) {                       // host pressed on: back into the game
			NP_STARTED_PEND = false;
			NP_WI_HOLD = false;
			if (NP_STARTED_MAP == "") { GSTATE = 4; wipe_begin(); }
			else { am_marks_reset(); GSTATE = 2; }
		}
		// everyone ready = travel NOW; the host reading its tally can't strand
		// clients on WAITING FOR HOST (keys only matter for the grace path)
		if (GSTATE == 3 && NET_GAME && NP_IS_HOST && np_ready_need() > 0
				&& np_ready_have() >= np_ready_need()) {
			np_wi_advance();
		}

		if (GSTATE == 2 && LEVEL_DONE) {
			if (NET_GAME && !NP_IS_HOST) { np_exit_send(); LEVEL_DONE = false; }   // the host's NEXT is authoritative
			else {
				wi_start();
				wipe_begin();
				if (NET_GAME) { np_wi_begin(); }     // arms the host's 10s READY grace
			}
		}

		if (GSTATE == 0 || GSTATE == 1) { music_want(MUS_TITLE); }
		else if (GSTATE == 3 || GSTATE == 4) { music_want(MUS_INTER); }

		tune_poll();
		stage.start();
		if (GSTATE == 2) {
			// palette flash: pain/berserk reds beat pickup golds beat the radsuit green
			int bank = 0;
			int red = PAIN_TICS > BERSERK_TICS ? PAIN_TICS : BERSERK_TICS;
			if (red > 0) { bank = 1 + (red > 42 ? 7 : red / 6); }
			else if (BONUS_TICS > 0) { bank = 9 + (BONUS_TICS > 9 ? 3 : BONUS_TICS / 3); }
			else if (P_SUIT > 0) { bank = 13; }
			set_palette_bank(bank);
			if (AM_ON) { automap_draw(); }
			else {
				EXTRA_LIGHT = P_GOGGLES > 0 ? 32 : (FLASH_TICS > 0 ? 1 : 0);
				float vx = P_X; float vy = P_Y; float va = P_ANG; EYE_Z = P_VIEWZ;
				LERP_F = SMOOTH ? accum / TICK_MS : 1.0;
				if (SMOOTH) {
					// the camera between the last two tics (the playsim stays at 35 Hz); a teleport snaps
					float f = accum / TICK_MS;
					float jx = P_X - IP_X; float jy = P_Y - IP_Y;
					if (jx * jx + jy * jy < 64.0 * 64.0) {
						vx = IP_X + jx * f; vy = IP_Y + jy * f; EYE_Z = IP_Z + (P_VIEWZ - IP_Z) * f;
						float da = P_ANG - IP_ANG;
						while (da > 3.14159265358979) { da = da - 6.28318530717959; }
						while (da < -3.14159265358979) { da = da + 6.28318530717959; }
						va = IP_ANG + da * f;
					}
				}
				render_view(vx, vy, va);
				if (P_HP > 0) {
					float bx = WBOB_X; float by = WBOB_Y;
					WBOB_X = IP_BOBX + (bx - IP_BOBX) * LERP_F;
					WBOB_Y = IP_BOBY + (by - IP_BOBY) * LERP_F;
					var w = WEAPONS[CUR_WEAPON];
					draw_weapon(w.spr, w.view_frame());
					if (FLASH_TICS > 0) { draw_weapon_flash(w.flash, "A"); }
					WBOB_X = bx; WBOB_Y = by;
				}
			}
			hud_draw();
			if (NET_GAME && NP_SCORE_ON) {
				var slines = np_scoreboard_lines();
				scoreboard_draw(slines);
			}
			msg_draw();
			if (!ATTRACT) { fps_draw(); }
			if (DEMO_REC) { tape_draw("REC"); }
			else if (DEMO_PLAY && !ATTRACT) { tape_draw("DEMO"); }
		} else {
			set_palette_bank(0);
			if (GSTATE == 0) { title_draw(); }
			else if (GSTATE == 1 && MENU_MODE == 3) { ip_draw(NP_ADDR_EDIT); }
			else if (GSTATE == 1 && MENU_MODE == 5) { demo_name_draw(DEMO_NAME_EDIT); }
			else if (GSTATE == 1 && MENU_MODE == 6) { menu_draw_window(MENU_ITEMS, MENU_SEL, 8, "SELECT LEVEL"); }
			else if (GSTATE == 1) { menu_draw(MENU_ITEMS, MENU_SEL, MENU_MODE == 1 ? "CHOOSE A SKILL" : ""); }
			else if (GSTATE == 5) {
				bool hosting = SESSION != null && SESSION.me == 0;
				int joined = np_lobby_count();
				var lt = hosting ? `WAITING - ${joined} OF ${NP_MAX_PLAYERS - 1} JOINED` : "CONNECTING...";
				var lm = hosting ? (NP_QUEUE_COOP || (SESSION != null && SESSION.coop) ? "CO-OP ON " + START_MAP : "DEATHMATCH ON " + START_MAP) : NP_LAST_ADDR;
				var lb = hosting && joined > 0 ? "ENTER STARTS - ESC CANCELS" : "ESC CANCELS";
				lobby_draw(lt, lm, lb);
			}
			else if (GSTATE == 3) { wi_screen(); }
			else { end_draw(); }
		}
		// the melt: departing frame over the fresh draw, one step per frame
		if (WIPE_TICS > 0 && (GSTATE == 0 || GSTATE == 3 || GSTATE == 4 || ATTRACT)) {
			wipe_overlay();
			if (wipe_tic()) { WIPE_TICS = 0; }
		}
		float view_ms = stage.elapsed() * 1000;
		stage.start();
		string panel = PANEL_ON && SCREEN_COLS >= 60 ? panel_text(VIEW_TOP, VIEW_LEFT) : "";
		if (panel == "") { vid_skip(0, 0); } else { vid_skip(PANEL_H, PANEL_W); }   // the frame leaves the panel's cells be
		string shown = VIEW_HOME + vid_blit() + panel;
		float blit_ms = stage.elapsed() * 1000;
		stage.start();
		print(shown, skip_newline);
		stat_note(sim_ms, view_ms, blit_ms, stage.elapsed() * 1000, dt);

		float cap = !SMOOTH ? math::itrunc(TICK_MS) : (UNCAPPED ? 0.0 : SMOOTH_MS);
		float rest = cap - frame.elapsed() * 1000;
		if (rest > 0) { sleep(rest / 1000.0); }
	}
	demo_finish();                                   // quitting mid-level still writes the tape
	music_stop();
	print(ESC + "[0m" + ESC + "[?25h" + ESC + "[?1049l");                 // restore terminal
	print("thanks for playing jaidoom");
	return 0;
}

function selftest() -> int {
	int fails = 0;
	// spawn
	if (sector_at(P_X, P_Y) != 38) { fails = fails + 1; print("FAIL spawn sector"); }
	// walk forward (north) 70 tics: must travel
	CMD_FWD = 50.0;
	float y0 = P_Y;
	for (int i = 0; i < 70; ++i) { game_tic(); }
	if (P_Y - y0 < 400.0) { fails = fails + 1; print(`FAIL forward travel (${P_Y - y0})`); }
	print(`walked to (${P_X:.0}, ${P_Y:.0}) sector ${sector_at(P_X, P_Y)} floor ${P_FLOORZ}`);
	// keep pushing north into the far wall: collision must hold us inside the map
	for (int i2 = 0; i2 < 300; ++i2) { game_tic(); }
	if (P_Y > -2000.0) { fails = fails + 1; print(`FAIL north wall didn't stop us (${P_Y:.0})`); }
	if (!pos_ok(P_X, P_Y)) { fails = fails + 1; print("FAIL resting position invalid"); }
	print(`pushed against wall at (${P_X:.0}, ${P_Y:.0})`);
	CMD_FWD = 0.0;
	// door: E1M1 line special 1 exists?
	int doorline = -1;
	for (int l = 0; l < LD_SPECIAL.size(); ++l) {
		if (LD_SPECIAL[l] == 1) { doorline = l; break; }
	}
	if (doorline < 0) { fails = fails + 1; print("FAIL no DR door on E1M1"); }
	else {
		int dsec = SD_SEC[LD_SIDE1[doorline]];
		int shut = SE_CEILH[dsec];
		door_use(dsec, false);
		for (int i3 = 0; i3 < 60; ++i3) { doors_tic(); }
		if (SE_CEILH[dsec] <= shut) { fails = fails + 1; print("FAIL door did not rise"); }
		else { print(`door sector ${dsec} rose ${SE_CEILH[dsec] - shut} units`); }
		for (int i4 = 0; i4 < 400; ++i4) { doors_tic(); }
		if (SE_CEILH[dsec] != shut) { fails = fails + 1; print(`FAIL door did not close (${SE_CEILH[dsec]} vs ${shut})`); }
		else { print("door waited and closed"); }
	}
	// gravity: walk off the armor ledge — several airborne tics, momentum
	// carried through the arc, feet settle on the lower floor
	int armor = -1;
	for (int a0 = 0; a0 < TH_TYPE.size(); ++a0) {
		if (TH_TYPE[a0] == 2018) { armor = a0; break; }
	}
	if (armor < 0) { fails = fails + 1; print("FAIL no armor thing on E1M1"); }
	else {
		P_X = TH_X[armor] * 1.0;
		P_Y = TH_Y[armor] * 1.0;
		P_MOMX = 0.0;
		P_MOMY = 0.0;
		P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
		P_Z = P_FLOORZ * 1.0;
		P_AIR = false;
		P_VIEWZ = P_FLOORZ + 41.0;
		float offdir = -100.0;
		for (int a1 = 0; a1 < 16 && offdir < -99.0; ++a1) {
			float ang = a1 * math::TAU / 16.0;
			int pf = SE_FLOORH[sector_at(P_X + math::cos(ang) * 48.0, P_Y + math::sin(ang) * 48.0)];
			if (P_FLOORZ - pf > 8) { offdir = ang; }
		}
		if (offdir < -99.0) { fails = fails + 1; print("FAIL no drop next to the armor ledge"); }
		else {
			P_ANG = offdir;
			CMD_FWD = 50.0;
			int airtics = 0;
			bool landed = false;
			float carry0 = 0.0;
			float carry1 = 0.0;
			for (int g = 0; g < 60 && !landed; ++g) {
				game_tic();
				if (P_AIR) {
					if (airtics == 0) { carry0 = math::sqrt(P_MOMX * P_MOMX + P_MOMY * P_MOMY); }
					airtics = airtics + 1;
					carry1 = math::sqrt(P_MOMX * P_MOMX + P_MOMY * P_MOMY);
				} else if (airtics > 0) { landed = true; }
			}
			CMD_FWD = 0.0;
			if (!landed || airtics < 2) { fails = fails + 1; print(`FAIL ledge walk-off did not arc (${airtics} air tics)`); }
			else { print(`fell off the armor ledge: ${airtics} tics airborne, landed on floor ${P_FLOORZ}`); }
			if (landed && (carry1 < carry0 * 0.9 || carry1 <= 0.0)) {
				fails = fails + 1;
				print(`FAIL momentum did not carry through the air (${carry0:.1} -> ${carry1:.1})`);
			}
			if (landed && P_Z != P_FLOORZ * 1.0) { fails = fails + 1; print("FAIL landing did not settle on the floor"); }
		}
	}

	// exit progression math
	GAME_MAP = "E1M1";
	if (next_map() != "E1M2") { fails = fails + 1; print("FAIL next_map E1M1"); }
	GAME_MAP = "E1M9";
	if (next_map() != "E1M4") { fails = fails + 1; print("FAIL next_map E1M9"); }
	GAME_MAP = "E1M1";
	print(fails == 0 ? "P4 SELFTEST: ALL GREEN" : `P4 SELFTEST: ${fails} FAILURES`);
	return fails;
}

// headless walk of the whole presentation flow: title -> menu -> skill ->
// game -> intermission -> next map -> episode end -> title, every screen drawn
int FIRE_PIN = 120978818;     // the title fire's hash after flowtest's 90 tics at 320x200, seed 4242: both backends

function flowtest() -> int {
	int fails = 0;
	int mload = file_exists(data_path(SAVE_PATH)) ? 1 : 0;      // a real save adds LOAD GAME
	GSTATE = 0;
	title_draw();
	menu_open();                                     // the any-key-at-title path
	if (GSTATE != 1 || MENU_ITEMS.size() != 6 + mload) { fails = fails + 1; print("FAIL title -> menu"); }
	menu_draw(MENU_ITEMS, MENU_SEL, "");
	menu_key("down");
	menu_key("up");
	menu_key("enter");                               // NEW GAME -> skill pick (4 classics + NIGHTMARE!)
	if (MENU_MODE != 1 || MENU_ITEMS.size() != 5 || MENU_SEL != 2) { fails = fails + 1; print("FAIL skill pick"); }
	menu_draw(MENU_ITEMS, MENU_SEL, "CHOOSE A SKILL");
	menu_key("enter");                               // hurt me plenty
	if (GSTATE != 2 || GAME_MAP != "E1M1" || SKILL_BIT != 2 || !MID_GAME) { fails = fails + 1; print("FAIL new game start"); }
	for (int t = 0; t < 5; ++t) { game_tic(); }
	render_view(P_X, P_Y, P_ANG);
	hud_draw();
	msg_draw();
	game_key("tab");
	if (!AM_ON) { fails = fails + 1; print("FAIL automap toggle"); }
	automap_draw();
	game_key("tab");
	game_key("esc");                                 // mid-game menu gains RESUME + SAVE GAME
	if (GSTATE != 1 || MENU_ITEMS.size() != 8 + mload) { fails = fails + 1; print("FAIL mid-game menu"); }
	menu_key("esc");                                 // esc resumes
	if (GSTATE != 2) { fails = fails + 1; print("FAIL menu resume"); }
	LEVEL_DONE = true;                               // exit -> intermission
	wi_start();
	if (GSTATE != 3 || WI.mapname != "E1M1" || WI.next != "E1M2" || LEVEL_DONE) { fails = fails + 1; print("FAIL intermission stats"); }
	wi_screen();
	var adv = WI.next;
	game_start_map(adv);                             // the any-key advance
	GSTATE = 2;
	if (GAME_MAP != "E1M2") { fails = fails + 1; print("FAIL next map"); }
	game_start_map("E1M8");                          // episode end path
	LEVEL_DONE = true;
	wi_start();
	if (WI.next != "") { fails = fails + 1; print(`FAIL E1M8 should end the episode (${WI.next})`); }
	GSTATE = 4;
	end_draw();
	GSTATE = 0;
	MID_GAME = false;
	title_draw();
	menu_open();
	if (MENU_ITEMS.size() != 6 + mload) { fails = fails + 1; print("FAIL RESUME lingered past episode end"); }

	// ---- multiplayer menu + address entry (no sockets: p9 owns the wire) ----
	menu_open();
	MENU_SEL = 0;
	for (int mi = 0; mi < MENU_ITEMS.size(); ++mi) { if (MENU_ITEMS[mi] == "MULTIPLAYER") { MENU_SEL = mi; } }
	menu_key("enter");
	if (MENU_MODE != 2 || MENU_ITEMS.size() != 4 || MENU_ITEMS[1] != "HOST CO-OP") {
		fails = fails + 1; print("FAIL multiplayer submenu");
	}
	menu_draw(MENU_ITEMS, MENU_SEL, "");
	MENU_SEL = 2;                                    // JOIN GAME -> the address field
	NP_LAST_ADDR = "10.0.0.9";
	menu_key("enter");
	if (MENU_MODE != 3 || NP_ADDR_EDIT != "10.0.0.9") { fails = fails + 1; print("FAIL join prefills the last address"); }
	menu_key("backspace");
	menu_key("backspace");
	menu_key("2");
	menu_key(".");
	menu_key("5");
	menu_key(":");
	menu_key("2");
	menu_key("9");
	if (NP_ADDR_EDIT != "10.0.02.5:29") { fails = fails + 1; print(`FAIL address editing (${NP_ADDR_EDIT})`); }
	menu_key("-");                                   // hostname chars land too
	menu_key("a");
	if (NP_ADDR_EDIT != "10.0.02.5:29-a") { fails = fails + 1; print(`FAIL hostname chars (${NP_ADDR_EDIT})`); }
	menu_key("!");                                   // rejected characters bounce
	if (NP_ADDR_EDIT != "10.0.02.5:29-a") { fails = fails + 1; print("FAIL address filter"); }
	ip_draw(NP_ADDR_EDIT);
	menu_key("esc");                                 // esc backs out to the submenu
	if (MENU_MODE != 2) { fails = fails + 1; print("FAIL address esc"); }
	menu_key("esc");
	if (MENU_MODE != 0) { fails = fails + 1; print("FAIL submenu esc"); }
	NP_ADDR_EDIT = "192.168.7.3:28777";              // persistence round-trip
	addr_save();
	NP_LAST_ADDR = "";
	addr_load();
	delete_file(data_path(NET_CFG));
	if (NP_LAST_ADDR != "192.168.7.3:28777") { fails = fails + 1; print(`FAIL net config round-trip (${NP_LAST_ADDR})`); }
	lobby_draw("WAITING FOR PLAYER", "CO-OP ON E1M1", "ESC CANCELS");

	// ---- demos menu: record arming, filename entry, menu-launched playback ----
	MID_GAME = false;
	menu_open();
	int drow = -1;
	for (int di = 0; di < MENU_ITEMS.size(); ++di) {
		if (MENU_ITEMS[di] == "DEMOS") { drow = di; }
	}
	if (drow < 0) { fails = fails + 1; print("FAIL no DEMOS menu entry"); }
	MENU_SEL = drow;
	menu_key("enter");
	if (MENU_MODE != 4 || MENU_ITEMS.size() != 3) { fails = fails + 1; print("FAIL demos submenu"); }
	menu_draw(MENU_ITEMS, MENU_SEL, "");
	DEMO_LAST_NAME = "jaidoom_menudemo.json";
	MENU_SEL = 0;                                    // RECORD NEW GAME -> skill pick, tape armed
	menu_key("enter");
	if (MENU_MODE != 1 || DEMO_ARM != "jaidoom_menudemo.json" || !DEMO_MENU_ARMED) { fails = fails + 1; print("FAIL record arms the tape"); }
	menu_key("esc");                                 // skill esc disarms a menu-armed tape
	if (MENU_MODE != 0 || DEMO_ARM != "") { fails = fails + 1; print("FAIL skill esc disarms"); }
	MENU_SEL = drow;
	menu_key("enter");
	MENU_SEL = 1;                                    // PLAY DEMO -> filename entry, prefilled
	menu_key("enter");
	if (MENU_MODE != 5 || DEMO_NAME_EDIT != "jaidoom_menudemo.json") { fails = fails + 1; print("FAIL play-demo prefill"); }
	menu_key("backspace");
	menu_key("x");
	menu_key("_");
	menu_key("!");                                   // rejected chars bounce
	if (DEMO_NAME_EDIT != "jaidoom_menudemo.jsox_") { fails = fails + 1; print(`FAIL demo name editing (${DEMO_NAME_EDIT})`); }
	demo_name_draw(DEMO_NAME_EDIT);
	DEMO_NAME_EDIT = "jaidoom_nope.json";            // missing file: friendly message, stays put
	menu_key("enter");
	if (MENU_MODE != 5 || GSTATE != 1) { fails = fails + 1; print("FAIL missing demo must not launch"); }
	menu_key("esc");
	if (MENU_MODE != 4) { fails = fails + 1; print("FAIL demo entry esc"); }
	DEMO_LAST_NAME = "mytape.json";                  // persistence rides the net config
	NP_ADDR_EDIT = NP_LAST_ADDR;
	addr_save();
	DEMO_LAST_NAME = "";
	addr_load();
	delete_file(data_path(NET_CFG));
	if (DEMO_LAST_NAME != "mytape.json") { fails = fails + 1; print(`FAIL demo name round-trip (${DEMO_LAST_NAME})`); }
	menu_open();                                     // OPTIONS: the render mode flips and persists
	for (int oi = 0; oi < MENU_ITEMS.size(); ++oi) { if (MENU_ITEMS[oi] == "OPTIONS") { MENU_SEL = oi; } }
	menu_key("enter");
	if (MENU_MODE != 7 || MENU_ITEMS[0] != "HIGH DETAIL: OFF") { fails = fails + 1; print("FAIL options submenu"); }
	menu_draw(MENU_ITEMS, MENU_SEL, "");
	menu_key("enter");
	PIX_SETTING = "";
	PIX_MODE = "";
	addr_load();
	delete_file(data_path(NET_CFG));
	if (PIX_SETTING != "sext" || PIX_MODE != "sext") { fails = fails + 1; print(`FAIL render mode round-trip (${PIX_SETTING})`); }
	menu_key("esc");
	if (MENU_MODE != 0) { fails = fails + 1; print("FAIL options esc"); }
	PIX_SETTING = "";
	PIX_MODE = "";
	// menu-armed record -> tape writes at level exit -> menu replay matches the hash
	DEMO_LAST_NAME = "jaidoom_menudemo.json";
	menu_open();
	MENU_SEL = drow;
	menu_key("enter");
	MENU_SEL = 0;
	menu_key("enter");
	menu_key("enter");                               // hurt me plenty: the tape rolls
	if (GSTATE != 2 || !DEMO_REC || DEMO_MENU_ARMED) { fails = fails + 1; print("FAIL menu record start"); }
	for (int mt = 0; mt < 80; ++mt) {
		CMD_FWD = mt < 50 ? 50.0 : 0.0;
		CMD_TURN = mt >= 30 ? 0.0614 : 0.0;
		CMD_FIRE = (mt % 25) < 3;
		game_tic();
	}
	int mh1 = demo_state_hash();
	int mfed = DEMO_TICS.size() / 6;
	wi_start();                                      // level exit: the tape writes + disarms
	if (DEMO_REC || DEMO_ARM != "" || !file_exists(data_path("jaidoom_menudemo.json"))) { fails = fails + 1; print("FAIL tape write at exit"); }
	player_reset();                                  // scramble, then replay from the menu
	game_start_map("E1M3");
	for (int ms0 = 0; ms0 < 8; ++ms0) { CMD_FWD = 50.0; game_tic(); }
	CMD_FWD = 0.0; CMD_TURN = 0.0; CMD_FIRE = false;
	menu_open();
	int drow2 = -1;
	for (int di2 = 0; di2 < MENU_ITEMS.size(); ++di2) {
		if (MENU_ITEMS[di2] == "DEMOS") { drow2 = di2; }
	}
	MENU_SEL = drow2;
	menu_key("enter");
	MENU_SEL = 1;
	menu_key("enter");                               // PLAY DEMO, prefilled with the fresh tape
	menu_key("enter");                               // launch it
	if (GSTATE != 2 || !DEMO_PLAY || GAME_MAP != "E1M1") { fails = fails + 1; print("FAIL menu playback launch"); }
	for (int mp0 = 0; mp0 < mfed; ++mp0) { read_input(); game_tic(); }
	int mh2 = demo_state_hash();
	if (mh2 != mh1) { fails = fails + 1; print(`FAIL menu replay hash ${mh2} != recorded ${mh1}`); }
	else { print(`menu demo round-trip pinned: ${mfed} tics replay to hash ${mh1}`); }
	demo_stop();                                     // the esc path
	if (DEMO_PLAY || GSTATE != 0 || MID_GAME) { fails = fails + 1; print("FAIL demo esc stop"); }
	attract_start();                                 // the title's attract loop plays the shipped tape
	if (!ATTRACT || !DEMO_PLAY || GSTATE != 2 || MID_GAME || GAME_MAP != DEMO_MAP) { fails = fails + 1; print("FAIL attract start"); }
	attract_end(true);                               // a key: the menu, never RESUME into the tape
	if (ATTRACT || DEMO_PLAY || GSTATE != 1 || MENU_ITEMS.size() != 6 + mload) { fails = fails + 1; print("FAIL attract key to menu"); }
	WIPE_TICS = 0;
	delete_file(data_path("jaidoom_menudemo.json"));
	DEMO_LAST_NAME = "demo.json";

	// ---- cheats: the classic typed sequences through the rolling buffer ----
	player_reset();
	game_start_map("E1M1");
	GSTATE = 2;
	cheat_feed("iddqd");
	if (!P_GOD || P_HP != 100) { fails = fails + 1; print("FAIL iddqd god mode on"); }
	player_damage(999);
	player_damage_from(999, P_X + 50.0, P_Y);
	if (P_HP != 100) { fails = fails + 1; print(`FAIL god did not block damage (${P_HP})`); }
	cheat_feed("iddqd");
	if (P_GOD) { fails = fails + 1; print("FAIL iddqd did not toggle off"); }
	cheat_feed("idkfa");
	if (!HAVE_SHOTGUN || !HAVE_CHAINGUN || !HAVE_LAUNCHER || P_KEYS != 7
			|| AMMO_BULLETS != AMMO_BULLETS_MAX || AMMO_SHELLS != AMMO_SHELLS_MAX
			|| AMMO_ROCKETS != AMMO_ROCKETS_MAX || P_ARMOR != 200) {
		fails = fails + 1; print("FAIL idkfa arsenal");
	}
	P_KEYS = 0;
	HAVE_SHOTGUN = false;
	AMMO_SHELLS = 0;
	cheat_feed("idfa");
	if (P_KEYS != 0 || !HAVE_SHOTGUN || AMMO_SHELLS != AMMO_SHELLS_MAX) { fails = fails + 1; print("FAIL idfa (keys must stay)"); }
	// noclip: find a distance the wall rejects, then ghost through it
	float bd = -1.0;
	for (float d = 32.0; d < 2000.0 && bd < 0.0; d = d + 16.0) {
		if (!pos_ok_from(P_X, P_Y, P_X + d, P_Y)) { bd = d; }
	}
	if (bd < 0.0) { fails = fails + 1; print("FAIL no blocking wall found east of spawn"); }
	cheat_feed("idclip");
	if (!P_NOCLIP || !pos_ok_from(P_X, P_Y, P_X + bd, P_Y)) { fails = fails + 1; print("FAIL idclip does not ghost the wall"); }
	cheat_feed("idclip");
	if (P_NOCLIP || pos_ok_from(P_X, P_Y, P_X + bd, P_Y)) { fails = fails + 1; print("FAIL idclip did not toggle back solid"); }
	cheat_feed("idbeholdr");
	if (P_SUIT != 2100) { fails = fails + 1; print("FAIL idbeholdr radsuit"); }
	cheat_feed("idbeholdr");
	if (P_SUIT != 0) { fails = fails + 1; print("FAIL idbeholdr toggle off"); }
	cheat_feed("idbeholda");
	if (!P_ALLMAP) { fails = fails + 1; print("FAIL idbeholda allmap"); }
	AM_CHEAT = 0;
	cheat_feed("iddt");
	if (AM_CHEAT != 1) { fails = fails + 1; print("FAIL iddt first stage"); }
	cheat_feed("iddt");
	cheat_feed("iddt");
	if (AM_CHEAT != 0) { fails = fails + 1; print("FAIL iddt cycle back to off"); }
	cheat_feed("idclev12");
	if (GAME_MAP != "E1M2" || GSTATE != 2) { fails = fails + 1; print(`FAIL idclev12 warp (${GAME_MAP})`); }
	cheat_feed("idclev99");
	if (GAME_MAP != "E1M2") { fails = fails + 1; print("FAIL idclev99 must not warp (no E9M9)"); }

	// ---- raise-and-change (types 20/22) takes the new floor's texture and drops the old floor's damage ----
	player_reset();
	game_start_map("E1M3");
	int rline = -1;
	int rsec = -1;
	for (int rl = 0; rl < LD_SPECIAL.size() && rline < 0; ++rl) {
		if (LD_SPECIAL[rl] != 20) { continue; }
		for (int rs = 0; rs < SE_TAG.size(); ++rs) {
			if (SE_TAG[rs] == LD_TAG[rl] && SE_SPECIAL[rs] == 7) { rline = rl; rsec = rs; }
		}
	}
	if (rline < 0) { fails = fails + 1; print("FAIL no type 20 line raising nukage on E1M3"); }
	else {
		spec_line(rline, 0);
		for (int rt = 0; rt < 200; ++rt) { spec_tic(); }
		P_X = 0.0; P_Y = 0.0;
		for (int rv = 0; rv < LD_V1.size() && sector_at(P_X, P_Y) != rsec; ++rv) {
			if (LD_SIDE0[rv] >= 0 && SD_SEC[LD_SIDE0[rv]] == rsec) {
				P_X = (VX_X[LD_V1[rv]] + VX_X[LD_V2[rv]]) / 2.0 + (VX_Y[LD_V2[rv]] - VX_Y[LD_V1[rv]]) * 0.1;
				P_Y = (VX_Y[LD_V1[rv]] + VX_Y[LD_V2[rv]]) / 2.0 - (VX_X[LD_V2[rv]] - VX_X[LD_V1[rv]]) * 0.1;
			}
		}
		P_HP = 100;
		for (int rh = 0; rh < 64; ++rh) { GAME_TIC = GAME_TIC + 1; spec_tic(); }
		if (SE_SPECIAL[rsec] != 0 || SE_FLOORPIC[rsec] == "NUKAGE3" || P_HP != 100) {
			fails = fails + 1; print(`FAIL raised nukage still hurts (sector ${rsec} special ${SE_SPECIAL[rsec]}, ${SE_FLOORPIC[rsec]}, hp ${P_HP}, standing in ${sector_at(P_X, P_Y)})`);
		}
	}

	// ---- save/load: the whole sim round-trips through JSON ----
	player_reset();
	SKILL_BIT = 2;
	game_start_map("E1M1");
	CMD_FWD = 50.0;
	for (int st1 = 0; st1 < 40; ++st1) { game_tic(); }
	CMD_FWD = 0.0;
	int doorline = -1;
	for (int sl = 0; sl < LD_SPECIAL.size(); ++sl) {
		if (LD_SPECIAL[sl] == 1) { doorline = sl; break; }
	}
	int dsec = SD_SEC[LD_SIDE1[doorline]];
	int shut = SE_CEILH[dsec];
	door_use(dsec, false);
	for (int st2 = 0; st2 < 25; ++st2) { doors_tic(); }
	int midceil = SE_CEILH[dsec];
	if (midceil <= shut || DOORS.size() != 1) { fails = fails + 1; print("FAIL door staging"); }
	int mon = -1;
	for (int sm = 0; sm < MO_TYPE.size(); ++sm) {
		if (MINFO.has(MO_TYPE[sm]) && MO_STATE[sm] == 1) { mon = sm; break; }
	}
	MO_X[mon] = MO_X[mon] + 17.0;
	MO_HP[mon] = MO_HP[mon] - 5;
	MO_STATE[mon] = 2;
	float monx = MO_X[mon];
	int monhp = MO_HP[mon];
	int wline = -1;                                  // stage a fired W1: special zeroed
	for (int sw = 0; sw < LD_SPECIAL.size(); ++sw) {
		if (LD_SPECIAL[sw] != 0 && LD_SPECIAL[sw] != 1) { wline = sw; break; }
	}
	LD_SPECIAL[wline] = 0;
	P_HP = 61; P_ARMOR = 33; AMMO_SHELLS = 7; HAVE_SHOTGUN = true; P_KEYS = 3; CUR_WEAPON = 3;
	SECRETS_FOUND = 2;
	KILLS = 4;
	float sx = P_X;
	float sy = P_Y;
	float sang = P_ANG;
	int stic = GAME_TIC;
	int nmo0 = MO_X.size();
	if (!game_save("jaidoom_savetest.json")) { fails = fails + 1; print("FAIL save write"); }
	// mutate wildly, then load must put every bit of it back
	player_reset();
	game_start_map("E1M4");
	for (int st3 = 0; st3 < 10; ++st3) { game_tic(); }
	if (!game_load("jaidoom_savetest.json")) { fails = fails + 1; print("FAIL load"); }
	delete_file(data_path("jaidoom_savetest.json"));
	if (GAME_MAP != "E1M1" || GAME_TIC != stic) { fails = fails + 1; print("FAIL map/tic restore"); }
	if (P_X != sx || P_Y != sy || P_ANG != sang) { fails = fails + 1; print("FAIL player position exact restore"); }
	if (P_HP != 61 || P_ARMOR != 33 || AMMO_SHELLS != 7 || !HAVE_SHOTGUN || P_KEYS != 3 || CUR_WEAPON != 3) {
		fails = fails + 1; print("FAIL player block restore");
	}
	if (MO_X.size() != nmo0 || MO_X[mon] != monx || MO_HP[mon] != monhp || MO_STATE[mon] != 2) {
		fails = fails + 1; print("FAIL monster mid-hurt restore");
	}
	if (SE_CEILH[dsec] != midceil || DOORS.size() != 1) { fails = fails + 1; print(`FAIL door mid-open restore (${SE_CEILH[dsec]} vs ${midceil})`); }
	if (SECRETS_FOUND != 2 || KILLS != 4) { fails = fails + 1; print("FAIL tally restore"); }
	if (LD_SPECIAL[wline] != 0) { fails = fails + 1; print("FAIL fired W1 line came back armed"); }
	if (MO_BRAIN[mon] != null) { fails = fails + 1; print("FAIL brains must reload null"); }
	for (int st4 = 0; st4 < 6; ++st4) { game_tic(); }
	if (MO_BRAIN[mon] == null) { fails = fails + 1; print("FAIL brain not re-minted after load"); }
	for (int st5 = 0; st5 < 600; ++st5) { doors_tic(); }
	if (SE_CEILH[dsec] != shut || DOORS.size() != 0) { fails = fails + 1; print("FAIL restored door did not finish its cycle"); }

	// ---- nightmare: skill 5 wiring, fast masks at use sites, corpse respawn ----
	GSTATE = 1;
	MENU_MODE = 1;
	menu_build();
	if (MENU_ITEMS.size() != 5 || MENU_ITEMS[4] != "NIGHTMARE!") { fails = fails + 1; print("FAIL skill menu gains NIGHTMARE!"); }
	MENU_SEL = 4;
	menu_key("enter");
	if (GSTATE != 2 || SKILL_BIT != 4 || !NIGHTMARE || !FAST_MONSTERS) { fails = fails + 1; print("FAIL nightmare new game flags"); }
	int nmon = -1;
	for (int nm0 = 0; nm0 < MO_TYPE.size(); ++nm0) {
		if (MINFO.has(MO_TYPE[nm0]) && MO_STATE[nm0] == 1) { nmon = nm0; break; }
	}
	monster_attack(nmon);
	if (MO_COOL[nmon] > 22) { fails = fails + 1; print(`FAIL fast attack cooldown not halved (${MO_COOL[nmon]})`); }
	FAST_MONSTERS = false;
	monster_attack(nmon);
	if (MO_COOL[nmon] < 25) { fails = fails + 1; print(`FAIL normal cooldown regressed (${MO_COOL[nmon]})`); }
	FAST_MONSTERS = true;
	mobj_damage(nmon, 9999, -1);
	if (MO_STATE[nmon] != 5) { fails = fails + 1; print("FAIL nightmare kill staging"); }
	for (int nt = 0; nt < 120 && MO_STATE[nmon] != 6; ++nt) { mobj_anim_tic(nmon); }
	if (MO_STATE[nmon] != 6 || MO_RESP[nmon] != 1050) { fails = fails + 1; print(`FAIL corpse respawn armed (${MO_RESP[nmon]})`); }
	MO_RESP[nmon] = 2;
	things_tic();
	things_tic();
	if (MO_STATE[nmon] != 1 || MO_HP[nmon] != MO_SHP[nmon]) { fails = fails + 1; print("FAIL corpse respawn to full hp"); }
	NIGHTMARE = false;
	FAST_MONSTERS = false;

	// ---- demo record/replay: the determinism pin ----
	SKILL_BIT = 2;
	player_reset();
	demo_rec_begin("E1M1", 2, 987654);               // seeds the RNG BEFORE the spawns
	game_start_map("E1M1");
	for (int dt = 0; dt < 200; ++dt) {
		CMD_FWD = dt < 120 ? 50.0 : 0.0;
		CMD_SIDE = dt >= 60 && dt < 90 ? 24.0 : 0.0;
		CMD_TURN = dt >= 100 ? 0.0614 : 0.0;
		CMD_USE = dt == 40;
		CMD_FIRE = (dt % 30) < 4;
		game_tic();
	}
	DEMO_REC = false;
	int h1 = demo_state_hash();
	if (DEMO_TICS.size() != 1200) { fails = fails + 1; print(`FAIL demo tuple count (${DEMO_TICS.size()})`); }
	if (AMMO_BULLETS >= 50) { fails = fails + 1; print("FAIL demo run never fired (RNG unexercised)"); }
	if (!demo_write("jaidoom_demotest.json")) { fails = fails + 1; print("FAIL demo write"); }
	DEMO_TICS = [];                                  // prove the replay runs off the file
	DEMO_SEED = 0;
	DEMO_MAP = "";
	player_reset();
	game_start_map("E1M4");                          // scramble everything between the runs
	for (int dx0 = 0; dx0 < 10; ++dx0) { CMD_FWD = 50.0; game_tic(); }
	CMD_FWD = 0.0; CMD_SIDE = 0.0; CMD_TURN = 0.0; CMD_USE = false; CMD_FIRE = false;
	if (!demo_load("jaidoom_demotest.json")) { fails = fails + 1; print("FAIL demo load"); }
	delete_file(data_path("jaidoom_demotest.json"));
	SKILL_BIT = DEMO_SKILL;
	NIGHTMARE = DEMO_NM;
	FAST_MONSTERS = DEMO_NM;
	player_reset();
	GAME_TIC = 0;
	SIM_RANDOM.seed(DEMO_SEED);
	DEMO_POS = 0;
	game_start_map(DEMO_MAP);
	int fed = 0;
	while (demo_feed()) { game_tic(); fed = fed + 1; }
	int h2 = demo_state_hash();
	if (fed != 200) { fails = fails + 1; print(`FAIL demo replay fed ${fed} tics`); }
	if (h1 != h2) { fails = fails + 1; print(`FAIL demo replay hash ${h2} != recorded ${h1}`); }
	else { print(`demo determinism pinned: 200 tics replay to hash ${h1}`); }

	// ---- screen melt: bounded, monotonic, and sim-RNG-isolated ----
	pix_clear(rgb_idx(255, 0, 0));
	SIM_RANDOM.seed(777);
	int wr1 = SIM_RANDOM.between(0, 1000000);
	int wr2 = SIM_RANDOM.between(0, 1000000);
	SIM_RANDOM.seed(777);
	wipe_begin();
	if (WIPE_TICS != 1 || WIPE_COL.size() != VW) { fails = fails + 1; print("FAIL wipe_begin staging"); }
	int wprev = WIPE_COL[VW / 2];
	int wticks = 0;
	bool wdone = false;
	while (wticks < VH + 40 && !wdone) {
		wdone = wipe_tic();
		int wnow = WIPE_COL[VW / 2];
		if (wprev >= 0 && wnow < wprev) { fails = fails + 1; print("FAIL wipe offset regressed"); wdone = true; }
		wprev = wnow;
		wticks = wticks + 1;
	}
	if (!wdone) { fails = fails + 1; print(`FAIL wipe never finished (${wticks} tics)`); }
	else { print(`wipe completed in ${wticks} tics at ${VW}x${VH}`); }
	int wr1b = SIM_RANDOM.between(0, 1000000);
	int wr2b = SIM_RANDOM.between(0, 1000000);
	if (wr1b != wr1 || wr2b != wr2) { fails = fails + 1; print("FAIL wipe leaked into the sim RNG stream"); }
	WIPE_TICS = 0;

	// ---- presentation: the mean sextant fit, gamma, the title fire, the menu backdrop, the J panel, tune.jai, easing ----
	bool was_tc = HOST_TRUECOLOR;
	HOST_TRUECOLOR = true;
	SEXTANTS = true;
	vid_resize(2, 3);
	int cw = rgb_idx(255, 255, 255);
	int ca = rgb_idx(100, 0, 0);
	int cb = rgb_idx(60, 0, 0);
	PIX = [cw, cw, ca, cb, ca, cb];                  // white over two reds: the white glyph on the reds' mean
	int mr = (GAMMA_LUT[100] * 2 + GAMMA_LUT[60] * 2) / 4;
	string want_cell = BG24 + to_string(mr) + ";0;0m" + PAL_FG[cw] + SEXTG[3] + RESET;
	if (vid_row_sext_mean(0) != want_cell) { fails = fails + 1; print("FAIL mean sextant fit"); }
	SEXTANTS = false;
	vid_resize(4, 2);
	PIX = [cw, cw, cw, cw, cb, cb, cb, cb];
	vid_skip(1, 3);                                  // the J panel's cells: the row starts past them
	if (vid_row_half(0) != SKIP_MOVE + PAL_FG[cw] + PAL_BG[cb] + HALFG + RESET || SKIP_MOVE != ESC + "[3C") {
		fails = fails + 1;
		print("FAIL view rows skip the panel's cells");
	}
	vid_skip(0, 0);
	vid_resize(320, 200);
	float g0 = GAMMA;
	vid_set_gamma(0.0);
	if (PAL_FG[ca] != sgr_color(38, 100, 0, 0)) { fails = fails + 1; print("FAIL gamma 0 leaves colors as drawn"); }
	vid_set_gamma(2.0);
	if (PAL_CR[ca] <= GAMMA_LUT[60] || PAL_CR[ca] <= 100) { fails = fails + 1; print("FAIL gamma 2 brightens"); }
	vid_set_gamma(g0);
	HOST_TRUECOLOR = was_tc;

	fire_start(4242);
	for (int ft = 0; ft < 90; ++ft) { fire_tic(); }
	int flame_top = VH;
	for (int fy = VH - 1; fy >= 0; --fy) {
		for (int fx2 = 0; fx2 < VW; ++fx2) { if (FIRE[fy * VW + fx2] > 0) { flame_top = fy; } }
	}
	if (flame_top > VH * 3 / 4 || flame_top < VH / 4) { fails = fails + 1; print(`FAIL title fire reach (top row ${flame_top} of ${VH})`); }
	if (fire_hash() != FIRE_PIN) { fails = fails + 1; print(`FAIL title fire stream (hash ${fire_hash()}, pinned ${FIRE_PIN})`); }
	else { print(`title fire pinned: 90 tics to hash ${FIRE_PIN}, flames up to row ${flame_top} of ${VH}`); }
	title_draw();
	if (PIX[(VH - 1) * VW + VW / 2] != FIRE_PAL[36]) { fails = fails + 1; print("FAIL title fire not drawn over TITLEPIC"); }

	GSTATE = 2;
	MID_GAME = true;
	pix_clear(cw);
	menu_open();
	if (BACKDROP.size() != VW * VH || PAL_R[BACKDROP[0]] != math::itrunc(255 * BACKDROP_DIM)) { fails = fails + 1; print("FAIL menu backdrop"); }
	menu_draw(MENU_ITEMS, MENU_SEL, "");
	if (PIX[0] != BACKDROP[0]) { fails = fails + 1; print("FAIL menu over the dimmed game"); }
	MID_GAME = false;
	GSTATE = 0;
	menu_open();
	if (!BACKDROP.empty()) { fails = fails + 1; print("FAIL title menu kept a game backdrop"); }
	GSTATE = 0;

	for (int sn = 0; sn < 30; ++sn) { stat_note(0.1, 2.5 + sn % 3, 0.8, 1.5, 16.7); }
	string ptext = panel_text(1, 1);
	if (!ptext.contains("JAISCRIPT") || !ptext.contains(" rows") || !ptext.contains("tune.jai") || ST_VIEW.size() != PANEL_SPAN) {
		fails = fails + 1;
		print("FAIL J panel text");
	}

	float fov_live = FOV;
	FOV = 123.0;
	string tune_src = read_file(game_root() + "/scripts/tune.jai");
	write_file(game_root() + "/scripts/tune.jai", tune_src);   // the same text, saved again: the next frame reloads it
	float recheck = import_recheck_interval();
	import_recheck_interval(0.0);                    // imports look at their files every 200 ms; now, for the test
	tune_poll();
	import_recheck_interval(recheck);
	if (FOV != fov_live || TUNE_RELOADS != 1) { fails = fails + 1; print(`FAIL tune.jai reload (fov ${FOV}, ${TUNE_RELOADS} reloads)`); }
	else { print("tune.jai reloaded in place"); }

	if (MO_X.size() > 0) {
		lerp_snapshot();
		LERP_MOX[0] = MO_X[0] - 10.0;
		LERP_F = 0.5;
		lerp_mobjs();
		bool eased = DRAW_X[0] == MO_X[0] - 5.0;
		LERP_MOX[0] = MO_X[0] - 100.0;
		lerp_mobjs();
		bool snapped = DRAW_X[0] == MO_X[0];
		LERP_F = 1.0;
		if (!eased || !snapped) { fails = fails + 1; print("FAIL mobj easing between tics"); }
	}

	print(fails == 0 ? "FLOWTEST: ALL GREEN" : `FLOWTEST: ${fails} FAILURES`);
	return fails;
}

function main() -> int {
	var path = "doom1";
	var mapname = "";
	bool test = false;
	bool flow = false;
	bool bootstrap = false;
	bool path_given = false;
	array<string> extra_wads = [];   // PWADs merged over the base, in order
	bool want_shareware = true;
	bool want_audio = true;
	var demopath = "";
	for (int i = 0; i < ARGS.size(); ++i) {
		if (ARGS[i] == "selftest") { test = true; }
		else if (ARGS[i] == "bootstrap") { bootstrap = true; }
		else if (ARGS[i] == "--no-shareware") { want_shareware = false; }
		else if (ARGS[i] == "--no-audio") { want_audio = false; }
		else if (ARGS[i] == "--mute") { MUTE = true; }
		else if (ARGS[i] == "uncapped") { UNCAPPED = true; SMOOTH = true; }
		else if (ARGS[i] == "--truecolor") { HOST_TRUECOLOR = true; }
		else if (ARGS[i] == "--256color") { HOST_TRUECOLOR = false; }
		else if (ARGS[i] == "--pix" && i + 1 < ARGS.size()) {
			PIX_MODE = ARGS[i + 1];
			i = i + 1;
			if (PIX_MODE != "half" && PIX_MODE != "sext") { print(`unknown --pix mode: ${PIX_MODE} (half|sext)`); return 2; }
		}
		else if (ARGS[i] == "flowtest") { flow = true; }
		else if (ARGS[i] == "freedoom2") { path = "freedoom2"; }
		else if (ARGS[i] == "record" && i + 1 < ARGS.size()) { DEMO_ARM = ARGS[i + 1]; i = i + 1; }
		else if (ARGS[i] == "playdemo" && i + 1 < ARGS.size()) { demopath = ARGS[i + 1]; i = i + 1; }
		else if (ARGS[i] == "host") {
			NP_QUEUE_MODE = "host";
			if (i + 1 < ARGS.size() && ARGS[i + 1].to_int(0) > 0) { NP_QUEUE_PORT = ARGS[i + 1].to_int(28666); i = i + 1; }
		}
		else if (ARGS[i] == "join" && i + 1 < ARGS.size()) {
			NP_QUEUE_MODE = "join";
			NP_QUEUE_HOST = ARGS[i + 1];
			i = i + 1;
			if (i + 1 < ARGS.size() && ARGS[i + 1].to_int(0) > 0) { NP_QUEUE_PORT = ARGS[i + 1].to_int(28666); i = i + 1; }
		}
		else if (ARGS[i] == "coop") { NP_QUEUE_COOP = true; }
		else if (ARGS[i].ends_with(".wad")) { if (path_given) { extra_wads.push(ARGS[i]); } else { path = ARGS[i]; path_given = true; } }
		else if (ARGS[i].starts_with("E") || ARGS[i].starts_with("MAP")) { mapname = ARGS[i]; }
	}
	path = wad_path(path);
	if (bootstrap) { return boot_run(false, want_shareware, want_audio) ? 0 : 1; }   // headless: play.bat/bootstrap.sh, CI
	if (!file_exists(path) && !test && !flow) {                                    // first run: the game fetches its own data
		if (!boot_run(true, want_shareware, want_audio)) { return 1; }
		if (!file_exists(path)) { print(`no WAD at ${path} (declined or failed); run again with -- bootstrap to retry`); return 1; }
	}
	if (!file_exists(path)) { print(`no WAD at ${path}: run jaidoom.jai -- bootstrap first`); return 1; }
	if (!wad_load(path)) { print("wad_load failed"); return 1; }
	for (auto extra : extra_wads) {
		string ep = wad_path(extra);
		if (!wad_add(ep)) { print(`cannot merge ${extra}`); return 1; }
	}
	if (mapname == "" && !extra_wads.empty()) { var first = map_list(); if (first.size() > 0) { mapname = first[0]; } }   // a map pack starts on its first map
	if (mapname == "") { mapname = lump_num("E1M1") >= 0 ? "E1M1" : "MAP01"; }
	MUS_TITLE = lump_num("D_INTRO") >= 0 ? "D_INTRO" : "D_DM2TTL";
	MUS_INTER = lump_num("D_INTER") >= 0 ? "D_INTER" : "D_DM2INT";
	addr_load();                                     // prefill JOIN GAME with the last address
	if (demopath != "") {
		if (!demo_load(demopath)) { print(`no demo ${demopath}`); return 1; }
		DEMO_QUEUE = true;
	}
	if (test) {
		vid_init(320, 200);
		if (!game_start_map(mapname)) { print(`no map ${mapname}`); return 1; }
		return selftest();
	}
	if (flow) {
		vid_init(320, 200);
		hud_init();
		START_MAP = mapname;
		return flowtest();
	}
	return play(mapname);
}

main()
