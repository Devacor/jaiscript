// --dev: the showcase look's scripts reload while the game runs. Each frame
// imports the look's modules as one group; the engine re-runs only what changed
// on disk, after checking every changed file, so a mistake anywhere is
// rejected whole and the running look stays as it was. A reload rebuilds the
// look's tables; the floor, the monsters, your health and the decals carry on.
// The simulation is never part of the group: STATE_HASH cannot notice a reload.

array<string> DEV_MODULES = ["tune.jai", "look.jai", "lights.jai", "feel.jai", "sound.jai"];
map<string, string> DEV_STAMP;
int DEV_RELOADS;

function dev_stamp(string module) -> string {
	var fd = file_data(resolve_path(module));
	return fd == null ? "" : to_string(fd["modified"]) + ":" + to_string(fd["size"]);
}

function dev_start() -> void {
	DEV_STAMP = {};
	for (auto& module : DEV_MODULES) { DEV_STAMP[module] = dev_stamp(module); }
}

// one group import; answers "" when nothing changed, else what the HUD says
function dev_poll() -> string {
	bool ran = false;
	try {
		ran = import(DEV_MODULES);
	} catch (e) {
		array<string> lines = to_string(e).lines();
		string why = lines.size() > 1 ? string::trim(lines[1]) : (lines.size() > 0 ? lines[0] : "?");
		return "reload rejected: " + why;
	}
	if (!ran) { return ""; }
	array<string> changed = [];
	for (auto& module : DEV_MODULES) {
		string stamp = dev_stamp(module);
		if (stamp == DEV_STAMP.get(module, "")) { continue; }
		DEV_STAMP[module] = stamp;
		changed.push(module);
	}
	DEV_RELOADS = DEV_RELOADS + 1;
	time::stopwatch rebuild;
	rebuild.start();
	G.renderer.rebuild_look(G.map_i);
	G.feel.forget_colors();
	if (changed.contains("sound.jai") || changed.contains("tune.jai")) { sound_boot(); }
	int ms = cast<int>(rebuild.elapsed() * 1000);
	return `reloaded ${changed.size() > 0 ? changed.join(" ") : "the look"} (tables ${ms} ms)`;
}

// the frame's reload, shown on the status bar
function dev_frame() -> void {
	string said = dev_poll();
	if (said != "") { G.hud.show(said, said.substr(0, 8) == "reloaded" ? "96" : "91"); }
}

// --reload-test, headless, in a scratch copy of scripts/: an edit to tune.jai
// changes the frame and keeps STATE_HASH; a broken edit is rejected and changes
// nothing; restoring the file brings the first frame back byte for byte
function dev_reload_test() -> int {
	import_recheck_interval(0.0);
	dev_start();
	string path = resolve_path("tune.jai");
	string original = read_file(path);
	for (int i = 0; i < 90; ++i) { gloom_frame(TICK, "", 0.0, 0.0, 0.0); }
	string before = gloom_frame(0.0, "", 0.0, 0.0, 0.0);
	int hash_before = G.hash;
	int fails = 0;

	write_file(path, string::replace(original, "float AMBIENT = 0.34;", "float AMBIENT = 0.9;"));
	string said = dev_wait();
	string brighter = gloom_frame(0.0, "", 0.0, 0.0, 0.0);
	fails += dev_check(said.substr(0, 8) == "reloaded", `an edit reloads (said: ${said})`);
	fails += dev_check(brighter != before, "the edited light changes the frame");
	fails += dev_check(AMBIENT > 0.8, "the new value is live");

	write_file(path, string::replace(original, "float AMBIENT = 0.34;", "float AMBIENT = ;"));
	said = dev_wait();
	string kept = gloom_frame(0.0, "", 0.0, 0.0, 0.0);
	fails += dev_check(said.substr(0, 15) == "reload rejected", `a broken edit is rejected (said: ${said})`);
	fails += dev_check(kept == brighter, "a rejected edit leaves the look as it was");

	write_file(path, original);
	said = dev_wait();
	string restored = gloom_frame(0.0, "", 0.0, 0.0, 0.0);
	fails += dev_check(said.substr(0, 8) == "reloaded", `restoring reloads (said: ${said})`);
	fails += dev_check(restored == before, "the restored file draws the first frame again");
	fails += dev_check(G.hash == hash_before, "no reload touches STATE_HASH");

	for (int i = 0; i < 30; ++i) { gloom_frame(TICK, "", 0.0, 0.0, 0.0); }
	host_log(fails == 0 ? `RELOAD TEST: PASS (${DEV_RELOADS} reloads, STATE_HASH ${G.hash})` : `RELOAD TEST: FAIL (${fails})`);
	return fails == 0 ? 0 : 1;
}

// poll until the engine notices the write (file times can be coarse)
function dev_wait() -> string {
	time::stopwatch waited;
	waited.start();
	while (waited.elapsed() < 5.0) {
		string said = dev_poll();
		if (said != "") { return said; }
	}
	return "nothing noticed";
}

function dev_check(bool ok, string what) -> int {
	host_log(`${ok ? "ok  " : "FAIL"} ${what}`);
	return ok ? 0 : 1;
}
