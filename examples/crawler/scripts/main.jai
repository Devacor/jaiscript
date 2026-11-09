// GRUBWELL entry point. The host executes: import "main.jai"; crawler_main();
// The host supplies: draw, read_key, poll_key, term_cols, term_rows, frame_mark, host_log, the
// file functions (file_stamp fingerprints a script for the reload watcher), the seeded Rng class
// and the HOST_* flags (main.cpp, or crawler.jai on the standalone runner).

import "state.jai";     // the persistent globals; the live reload leaves this file out
import "util.jai";
import "term.jai";
import "data.jai";
import "entities.jai";
import "dungeon.jai";
import "items.jai";
import "combat.jai";
import "ai.jai";
import "art.jai";
import "view.jai";
import "ui.jai";
import "screens.jai";
import "fx.jai";
import "game.jai";

// ------------------------------------------------------------- hot reload ---
// Re-runs every rule and presentation file mid-run: R, a script saved to disk, or the smoke
// pilot's --reload-every. An import is program scope wherever it stands, so functions and
// classes replace in place (instances migrate) and each file's top-level declarations run
// again; the group is parsed and checked before any of it runs, so a broken edit raises, changes
// nothing, and the run goes on under the old rules. Live coroutines keep their old bodies until
// they are made again.
array<string> RELOAD_GROUP = ["util.jai", "term.jai", "data.jai", "entities.jai", "dungeon.jai", "items.jai",
	"combat.jai", "ai.jai", "art.jai", "view.jai", "ui.jai", "screens.jai", "fx.jai", "game.jai"];
int RELOAD_COUNT;           // declared without initializers, like everything a reload keeps
int RELOAD_REFUSED;
string WATCH_STAMP;
string WATCH_PENDING;

function reload_scripts() -> void {
	import("util.jai", "term.jai", "data.jai", "entities.jai", "dungeon.jai", "items.jai", "combat.jai", "ai.jai",
		"art.jai", "view.jai", "ui.jai", "screens.jai", "fx.jai", "game.jai");
	setup_screen();
}

function live_reload(string why) -> bool { return attempt_reload(why, reload_scripts); }

function attempt_reload(string why, var reload) -> bool {
	time::stopwatch clock;
	clock.start();
	bool done = true;
	string failure = "";
	try {
		reload();
	} catch (e) {
		done = false;
		failure = to_string(e);
	}
	float ms = clock.elapsed() * 1000.0;
	WATCH_STAMP = scripts_stamp();
	WATCH_PENDING = WATCH_STAMP;
	if (!done) {
		RELOAD_REFUSED += 1;
		reload_failed(failure);
		return false;
	}
	RELOAD_COUNT += 1;
	reload_done(why, RELOAD_GROUP.size(), ms);
	return true;
}

// --reload-every: the smoke pilot reloads mid-run, and its first try asks for a file that is not
// there, so one run shows a refused reload changing nothing and every other one keeping the run.
function pilot_reload() -> void {
	if (RELOAD_REFUSED == 0) { attempt_reload("a file that is not there", import_missing_script); }
	live_reload("the pilot's reload-every");
}

function import_missing_script() -> void { import("no_such_script.jai"); }

function scripts_stamp() -> string {
	string stamp = "";
	for (auto name : RELOAD_GROUP) { stamp += file_stamp(name) + ";"; }
	return stamp;
}

// Called while the game waits for a key: a file that changed, and then held still for one more
// look (an editor may write it in two steps), reloads the group. True when it reloaded.
function watch_scripts() -> bool {
	string stamp = scripts_stamp();
	if (WATCH_STAMP == "") {
		WATCH_STAMP = stamp;
		WATCH_PENDING = stamp;
		return false;
	}
	if (stamp == WATCH_STAMP) { return false; }
	if (stamp != WATCH_PENDING) {
		WATCH_PENDING = stamp;
		return false;
	}
	live_reload("a script changed on disk");
	return true;
}

function crawler_main() -> void {
	setup_screen();
	RNG = Rng(HOST_SEED);
	G = Game();
	PILOT_CO = null;   // a second run in the same engine (the REPL) starts a fresh pilot
	G.smoke = HOST_SMOKE;
	G.autopilot = HOST_SMOKE;

	if (G.autopilot) {
		// the pilot's kin and profession come from the seed; from there it presses keys like you
		Species species = SPECIES[RNG.next(SPECIES.size())];
		Profession calling = PROFESSIONS[RNG.next(PROFESSIONS.size())];
		start_new_game(species, calling, "Testpilot");
	} else if (!(HOST_LOAD && load_game())) {
		bool saved = file_exists(SAVE_PATH);
		while (true) {
			string choice = title_screen(saved);
			if (choice == "q") { return; }
			if (choice == "c") {
				if (load_game()) { break; }
				saved = false;
				continue;
			}
			Hero hero = choose_hero();
			if (!hero.chosen) { continue; }
			start_new_game(hero.species, hero.calling, hero.name);
			break;
		}
	}

	game_loop();

	if (G.smoke) { print_run_summary("SMOKE RESULT"); }
	else if (HOST_SCRIPTED) { print_run_summary("SCRIPTED RESULT"); }
}
