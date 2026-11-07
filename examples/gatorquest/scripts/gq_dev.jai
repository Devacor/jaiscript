// Gator Quest - dev-mode hot reload. One group import per frame reaches every
// module: the changed ones are all parsed and checked before any of them runs,
// so a broken edit anywhere raises once and nothing applies (the running code
// stays as it was until the file is fixed); otherwise they re-run in load
// order: functions and classes are replaced in place (live instances migrate),
// a declaration with an initializer takes its new value, one without keeps the
// live value. Between the engine's check passes the import costs nanoseconds.
// The two state files reload like the rest: all they declare is state.

import "gq_save.jai";

bool DEV_ON;
int DEV_RELOADS;
map<string, string> DEV_STAMP;   // module -> "modified:size" when it last ran

// every module the running game can reload, in load order (this file and the entry file are not)
array<string> DEV_MODULES = ["gq_enums.jai", "gq_types.jai", "gq_state.jai", "gq_cstate.jai", "gq_util.jai", "gq_defs.jai",
	"gq_data.jai", "gq_world.jai", "gq_mobs.jai", "gq_items.jai", "gq_combat.jai", "gq_ai.jai", "gq_magic.jai", "gq_quests.jai",
	"gq_actions.jai", "gq_sim.jai", "gq_bot.jai", "gq_proto.jai", "gq_vid.jai", "gq_art.jai", "gq_render.jai", "gq_audio.jai",
	"gq_predict.jai", "gq_hud.jai", "gq_game.jai", "gq_net.jai", "gq_save.jai", "gq_title.jai", "gq_png.jai", "gq_tests.jai",
	"gq_serve.jai", "gq_endgame.jai", "gq_predtest.jai", "gq_menutest.jai"];

string dev_stamp(string module) {
	var fd = file_data(module);
	return fd == null ? "" : to_string(fd["modified"]) + ":" + to_string(fd["size"]);
}

void dev_start() {
	DEV_STAMP = {};
	for (int i = 0; i < DEV_MODULES.size(); ++i) { DEV_STAMP[DEV_MODULES[i]] = dev_stamp(DEV_MODULES[i]); }
}

// the frame's reload: one import of the whole list; the stamps name what ran only on a frame where something did
array<string> dev_poll() {
	array<string> msgs;
	bool ran = false;
	try {
		ran = import(DEV_MODULES);
	} catch (e) {
		string text = to_string(e);
		string module = "?";
		for (int i = 0; i < DEV_MODULES.size(); ++i) {   // a run-time error names its file, a rejected load its listing
			if (e.file.contains(DEV_MODULES[i]) || text.contains(DEV_MODULES[i])) { module = DEV_MODULES[i]; break; }
		}
		array<string> el = text.lines();
		msgs.push(`Reload FAILED (${module}): ` + (el.size() > 0 ? el[0] + (el.size() > 1 ? " " + string::trim(el[1]) : "") : "?"));
		return msgs;
	}
	if (!ran) { return msgs; }
	for (int i = 0; i < DEV_MODULES.size(); ++i) {
		string module = DEV_MODULES[i];
		string stamp = dev_stamp(module);
		if (stamp == DEV_STAMP.get(module, "")) { continue; }
		DEV_STAMP[module] = stamp;
		DEV_RELOADS = DEV_RELOADS + 1;
		msgs.push("Reloaded " + module);
		dev_after_reload(module);
	}
	return msgs;
}

// derived client state that a reloaded file feeds
void dev_after_reload(string name) {
	if (name == "gq_art.jai" || name == "gq_vid.jai" || name == "gq_data.jai") {
		TS = 0;
		SHADE_WORLD = -1;
		if (VW > 0) { layout(SCREEN_COLS, SCREEN_ROWS); }
	}
}
