// Gator Quest entry point.
//
//   jaiscript gatorquest.jai                         play: the title offers single player, host and join
//   jaiscript gatorquest.jai -- host [port N]        ... opening on the host page
//   jaiscript gatorquest.jai -- join HOST [port N]   ... opening on the join page (lag/jitter/loss N simulate a link)
//   jaiscript gatorquest.jai -- smoke [ticks N] [seed S] [bots K]
//                                                    headless autopilot session, prints STATE_HASH
//   jaiscript gatorquest.jai -- shot [ticks N] [every N] [screen 150x44] [out DIR] [keys "i down enter"]
//                                                    headless frame captures (.ans + .txt sketch)
//   jaiscript gatorquest.jai -- savetest             save robustness suite
//   jaiscript gatorquest.jai -- thieftest            the Raccoon Bandits: warrens, pace, thefts, the purse, the quest
//   powershell -File hotreload.ps1 [-Backend interp]  hot-reload test (it edits its scripts: runs a scratch copy)
//   jaiscript gatorquest.jai -- dev                  play with live script reload
//   jaiscript gatorquest.jai -- predtest | menutest | nettest | endgame | tour | serve | client   see README.md

import "gq_enums.jai";
import "gq_types.jai";
import "gq_state.jai";
import "gq_cstate.jai";
import "gq_util.jai";
import "gq_defs.jai";
import "gq_data.jai";
import "gq_world.jai";
import "gq_mobs.jai";
import "gq_items.jai";
import "gq_combat.jai";
import "gq_ai.jai";
import "gq_magic.jai";
import "gq_quests.jai";
import "gq_actions.jai";
import "gq_sim.jai";
import "gq_bot.jai";
import "gq_proto.jai";
import "gq_vid.jai";
import "gq_art.jai";
import "gq_render.jai";
import "gq_audio.jai";
import "gq_predict.jai";
import "gq_hud.jai";
import "gq_game.jai";
import "gq_net.jai";
import "gq_save.jai";
import "gq_title.jai";
import "gq_png.jai";
import "gq_dev.jai";
import "gq_tests.jai";
import "gq_serve.jai";
import "gq_endgame.jai";
import "gq_predtest.jai";
import "gq_menutest.jai";

string arg_value(string key, string def) {
	for (int i = 0; i + 1 < ARGS.size(); ++i) {
		if (ARGS[i] == key) { return ARGS[i + 1]; }
	}
	return def;
}

bool arg_flag(string key) {
	for (int i = 0; i < ARGS.size(); ++i) {
		if (ARGS[i] == key) { return true; }
	}
	return false;
}

void print_char_summary(int slot) {
	Character& c = CHARS[slot];
	int m = c.mob;
	string where = mob_live(m) ? `${WORLDS[MOB_WORLD[m]].name} (${MOB_X[m]},${MOB_Y[m]})` : "nowhere";
	print(`  ${c.name} ${player_title(slot)} | hp ${MOB_HP[m]}/${MOB_HPMAX[m]} gold ${c.gold} fame ${c.fame} kills ${c.kills} deaths ${c.deaths} | ${where}`);
	string skills = "";
	for (int k = 0; k < c.skill.size(); ++k) {
		if (c.skill[k] > 0) { skills = skills + `${SKILL_KEY[k]} ${tenths(c.skill[k])}  `; }
	}
	print("  skills: " + skills);
	string pack = "";
	for (int k = 0; k < c.pack.size(); ++k) { pack = pack + item_name(c.pack[k]) + ", "; }
	print("  pack: " + pack);
	string worn = "";
	for (int k = 0; k < c.worn.size(); ++k) {
		if (c.worn[k].base >= 0) { worn = worn + SLOT_NAME[k] + "=" + item_name(c.worn[k]) + ", "; }
	}
	print("  worn: " + worn);
	array<string> j = journal_lines(slot);
	for (int k = 0; k < j.size(); ++k) { print("  journal: " + j[k]); }
}

int run_smoke() {
	int ticks = arg_value("ticks", "3000").to_int(3000);
	int seed = arg_value("seed", "1").to_int(1);
	int bots = arg_value("bots", "2").to_int(2);
	bool verbose = arg_flag("verbose");
	time::stopwatch lap;
	lap.start();
	sim_init(seed);
	int gen_ms = lap.delta() * 1000;
	print(`world: seed ${seed}, ${TILES.size()} tiles in ${WORLD_COUNT} realms, ${MOB_LIVE} mobiles, ${SP_W.size()} spawners, ${GATE_W.size()} gates (gen ${gen_ms} ms)`);
	BOTS = [];
	array<string> names = ["Chomps", "Snappy", "Gumbo", "Bayou Bill"];
	for (int b = 0; b < bots; ++b) {
		player_create(b, names[b % names.size()], b);
		bot_add(b);
	}
	int logs = 0;
	// a save/load round trip mid-session (the rest of the run continues from the loaded state)
	int roundtrip = arg_flag("noroundtrip") ? -1 : arg_value("roundtrip", to_string(ticks / 2)).to_int(ticks / 2);
	int fails = 0;
	for (int t = 0; t < ticks; ++t) {
		if (t == roundtrip && t > 0) {
			bool same = save_roundtrip(data_root() + "/saves/smoke");
			print(same ? `ROUNDTRIP: ok at tick ${TICK}` : `ROUNDTRIP: FAIL at tick ${TICK}`);
			if (!same) { fails = fails + 1; }
		}
		bots_tick();
		sim_tick();
		for (int e = 0; e < EV_KIND.size(); ++e) {
			if (EV_KIND[e] == Ev.log && verbose && logs < 4000) {
				logs = logs + 1;
				print(`[${TICK}] ${EV_TO[e] >= 0 ? CHARS[EV_TO[e]].name : "near"}: ${EV_TEXT[e]}`);
			}
		}
	}
	int run_us = lap.delta() * 1000000;
	print(`ran ${ticks} ticks in ${run_us / 1000} ms (${run_us / imax(1, ticks)} us/tick), ${MOB_LIVE} mobiles alive, kills ${STAT_KILLS}, spawns ${STAT_SPAWNS}`);
	for (int b = 0; b < bots; ++b) {
		print_char_summary(b);
		print("  plan: " + BOTS[b].log);
	}
	print(`STATE_HASH: ${state_hash()}`);
	return fails;
}

// the interactive game: title (single player, host, join) -> session -> back to the title or quit
int run_play() {
	int seed = arg_value("seed", "1").to_int(1);
	string screen = arg_value("screen", "");
	if (screen != "") {
		array<string> wh = screen.split("x");
		FORCE_COLS = wh[0].to_int(0);
		FORCE_ROWS = wh.size() > 1 ? wh[1].to_int(0) : 0;
	}
	bool demo = arg_flag("demo");
	int port = arg_value("port", "28777").to_int(28777);
	MAX_FRAMES = arg_value("frames", "0").to_int(0);
	// demos and timed runs never touch this machine's saves (a joined game's gators live on its host)
	SAVES_ON = !demo && MAX_FRAMES == 0;
	print(ESC + "[?1049h" + ESC + "[?25l" + ESC + "[2J", skip_newline);
	layout(screen_cols(), screen_rows());
	AUDIO_ON = !arg_flag("mute") && audio_prepare(data_root() + "/cache");
	title_setup(arg_value("join", ""), arg_flag("host"), port);
	if (arg_flag("join") && TM_PAGE == "main") { tm_open("join"); }
	bool dev = arg_flag("dev");
	if (dev) {
		dev_start();
		DEV_ON = true;
	}
	while (true) {
		string last = SAVES_ON ? last_player_name() : "";
		TITLE_LAST = last != "" ? last : arg_value("name", "");
		bool resumed = SAVES_ON && load_world_for_session();
		if (!resumed) { sim_init(seed); }
		string choice = title_screen(resumed, demo ? 45 : 0);
		if (choice == "quit") { break; }
		string welcome = session_start(choice, demo);
		if (dev) { welcome = welcome + ` Dev mode: ${DEV_MODULES.size()} scripts reload when saved.`; }
		if (!NET_CLIENT) {
			say_to(LOCAL_SLOT, welcome, C_YELLOW);
			music_play(MOB_WORLD[CHARS[LOCAL_SLOT].mob]);
		} else {
			music_play(0);
		}
		play_loop();
		if (arg_value("png", "") != "") { png_write_canvas(arg_value("png", ""), 3); }
		session_end();
		if (PLAY_EXIT != "title" || MAX_FRAMES > 0) { break; }
	}
	print(ESC + "[0m" + ESC + "[?25h" + ESC + "[?1049l", skip_newline);
	print("Thanks for playing Gator Quest! Your swamp awaits.");
	if (MAX_FRAMES > 0) {
		int n = imax(1, STAT_FRAMES);
		print(`frames ${STAT_FRAMES}: sim ${STAT_SIM_MS * 1000 / n:.0f} us, render ${STAT_RENDER_MS * 1000 / n:.0f} us, compose ${STAT_COMPOSE_MS * 1000 / n:.0f} us per frame; tick ${TICK}; STATE_HASH ${state_hash()}`);
	}
	return 0;
}

// dev: every sprite at 4x with its tint, and every tile texture, into one PNG
int run_sheet() {
	sim_init(1);
	art_init(8);
	shade_build(18, 22, 48, false);
	int cols = 12;
	int cell = 40;
	int nspr = SPRITE_SRC.size();
	int rows = (nspr + cols - 1) / cols;
	int trows = (TILES_DEF.size() + 23) / 24;
	canvas_init(cols * cell, rows * cell + trows * 20 + 8);
	fill_px(0, 0, VW, VH, rgb(40, 44, 52));
	for (int i = 0; i < nspr; ++i) {
		string name = SPRITE_SRC[i].split("|")[0];
		string tint = "green";
		for (int k = 0; k < MONS.size(); ++k) {
			if (MONS[k].spr == name) { tint = MONS[k].col; break; }
		}
		if (name == "gator") { tint = "hue0"; }
		int off = sprite_compiled(i, tint, crgb("leather"), crgb("straw"), crgb("steel"));
		int ox = (i % cols) * cell + 4;
		int oy = (i / cols) * cell + 2;
		for (int py = 0; py < 32; ++py) {
			for (int px = 0; px < 32; ++px) {
				int c = SPRPIX[off + (py / 4) * TS + px / 4];
				if (c >= 0) { pset(ox + px, oy + py, c); }
			}
		}
		draw_text_px(name.substr(0, 9), ox - 2, oy + 33, rgb(220, 220, 220), -1, 1);
	}
	int ty0 = rows * cell + 4;
	for (int t = 0; t < TILES_DEF.size(); ++t) {
		int ox = (t % 24) * 20 + 2;
		int oy = ty0 + (t / 24) * 20;
		for (int vr = 0; vr < 2; ++vr) {
			int src = TEX_OFF[(t * 4 + vr) * 4];
			for (int py = 0; py < 8; ++py) {
				for (int px = 0; px < 8; ++px) { pset(ox + vr * 8 + px, oy + py, TEX[src + py * 8 + px]); }
			}
		}
	}
	string out = arg_value("out", data_root() + "/shots/sheet.png");
	png_write_canvas(out, 2);
	print("wrote " + out);
	return 0;
}

// build the audio cache (every effect and every theme) so a fresh install starts without the wait
int run_prepare() {
	time::stopwatch took;
	took.start();
	string dir = data_root() + "/cache";
	if (!audio_prepare(dir)) {
		print("prepare: " + dir + " is not writable");
		return 1;
	}
	int themes = 0;
	for (int theme = 0; theme < THEME_TEMPO.size(); ++theme) {
		string p = dir + "/theme" + to_string(theme) + ".mid";
		if (!file_exists(p)) { write_file(p, midi_theme(theme)); }
		if (file_exists(p)) { themes = themes + 1; }
	}
	print(`prepare: ${SFX_PATH.size()} sound effects and ${themes} music themes in ${dir} (${took.elapsed() * 1000:.0f} ms)`);
	return 0;
}

int main() {
	client_state_init();
	if (arg_flag("nodelta")) { SNAP_DELTAS = false; }
	if (arg_flag("noprediction")) { PRED_ON = false; }
	// the network simulator: lag N (round trip ms), jitter N (ms), loss N (percent)
	NET_SIM_LAG = arg_value("lag", "0").to_int(0);
	NET_SIM_JITTER = arg_value("jitter", "0").to_int(0);
	NET_SIM_LOSS = arg_value("loss", "0").to_int(0);
	if (arg_flag("sheet")) { return run_sheet(); }
	if (arg_flag("smoke")) { return run_smoke(); }
	if (arg_flag("shot")) { return arg_flag("title") ? run_title_shots() : run_shots(); }
	// "tag X" gives a test run its own folders, so runs in parallel never share files
	string tests = data_root() + "/saves" + (arg_value("tag", "") == "" ? "" : "/" + arg_value("tag", ""));
	if (arg_flag("savetest")) { return run_savetest(tests); }
	if (arg_flag("thieftest")) { return run_thieftest(tests); }
	if (arg_flag("hotreload")) { return run_hotreload(); }
	if (arg_flag("nettest")) { return run_nettest(tests, arg_value("port", "28911").to_int(28911)); }
	if (arg_flag("prepare")) { return run_prepare(); }
	if (arg_flag("endgame")) { return run_endgame(); }
	if (arg_flag("tour")) { return run_tour(); }
	if (arg_flag("predtest")) { return run_predtest(tests); }
	if (arg_flag("menutest")) { return run_menutest(tests, arg_value("port", "28960").to_int(28960)); }
	if (arg_flag("serve")) { return run_serve(); }
	if (arg_flag("client")) { return run_client(); }
	return run_play();
}
main();
