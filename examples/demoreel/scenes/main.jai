// demoreel entry points. This file executes LAST at boot and is never
// hot-reloaded: it owns the persistent globals (RESET, gfx, the reel instance),
// so pressing 'r' can re-execute every other file without wiping state.

var RESET = ESC + "[0m";
var gfx = Gfx();
var reel = null;

function demo_boot(int w, int h, int seed, string backend, bool smoke, bool hud_on) -> void {
	reel = Reel(w, h, seed, backend, smoke, hud_on);
}

function demo_frame(float t, float dt, string key, float fps, float ms_sim, float ms_draw, float ms_other) -> string {
	return reel.frame(t, dt, key, fps, ms_sim, ms_draw, ms_other);
}

function demo_scene_index() -> int { return reel.cur_idx; }
function demo_scene_time() -> float { return reel.scene_t; }
function demo_jump(int idx, float at_t) -> void { reel.jump(idx, at_t); }
function demo_reload() -> void { reel.reload_current(); }
function demo_scene_count() -> int { return reel.nscenes; }
function demo_scene_name() -> string { return reel.cur.name; }
