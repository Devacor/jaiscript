// GRUBWELL entry point. The host executes: import "main.jai"; crawler_main();
// The host supplies: draw, read_key, poll_key, term_cols, term_rows, frame_mark, host_log, the
// file functions, the seeded Rng class and the HOST_* flags (main.cpp, or crawler.jai on the
// standalone runner).

import "state.jai";     // the persistent globals; the dev reload leaves this file out
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
import "game.jai";

// Re-runs every rule and presentation file mid-run ('R' with --dev). An import is program
// scope wherever it stands, so functions and classes replace in place (instances migrate) and
// each file's top-level declarations run again; the group is parsed and checked before any of
// it runs, so a broken edit raises and changes nothing. Live coroutines keep their old bodies
// until they are made again.
function reload_scripts() -> void {
	import("util.jai", "term.jai", "data.jai", "entities.jai", "dungeon.jai", "items.jai", "combat.jai", "ai.jai",
		"art.jai", "view.jai", "ui.jai", "screens.jai", "game.jai");
	setup_screen();
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
