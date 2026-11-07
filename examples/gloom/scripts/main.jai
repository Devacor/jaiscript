// GLOOM entry point. The host executes: import "main.jai"; then calls
// gloom_boot(w, h) once and gloom_frame(dt, key, fps, ms_sim, ms_draw) per
// frame; --smoke additionally calls gloom_state_hash() / gloom_summary().

import "state.jai";
import "util.jai";
import "defs.jai";
import "maps.jai";
import "pure.jai";
import "particles.jai";
import "enemies.jai";
import "combat.jai";
import "sim.jai";
import "look.jai";
import "render.jai";
import "viewmodel.jai";
import "canvas.jai";
import "hud.jai";
import "screens.jai";
import "game.jai";

bool FORCE_PILOT = false;

function gloom_force_autopilot() -> void { FORCE_PILOT = true; }

function gloom_boot(int w, int h) -> void {
	RNG = Rng(HOST_SEED);
	SHOWCASE = HOST_LOOK == "showcase";
	if (HOST_ASCII) { PIXW = 1; PIXH = 1; }
	else if (HOST_PIX == 0) { PIXW = 1; PIXH = 2; }
	else if (HOST_PIX == 2) { PIXW = 2; PIXH = 3; }
	else { PIXW = 2; PIXH = 2; }
	parse_arts();
	parse_viewmodels();
	validate_all_maps();
	G = new Game(w, h);
	init_render();
	if (SHOWCASE) { init_screens(); }
	G.autopilot = HOST_SMOKE || FORCE_PILOT;
}

// dt accumulates into fixed TICKs (smoke feeds exactly TICK: one tick/frame).
// The edge key is consumed by the first tick of the frame.
function gloom_frame(float dt, string key, float fps, float ms_sim, float ms_draw) -> string {
	G.frame_key = key;
	G.accum += dt;
	int steps = 0;
	while (G.accum >= TICK - 0.0000001 && steps < 3) {
		G.accum -= TICK;
		G.run_tick();
		G.frame_key = "";
		steps = steps + 1;
	}
	if (steps == 3) { G.accum = 0.0; }   // stall recovery: never spiral
	return render_frame(fps, ms_sim, ms_draw);
}

function gloom_state_hash() -> int { return G.hash; }

function gloom_wants_quit() -> bool { return G.quit; }

function gloom_summary() -> void {
	host_log("");
	host_log("=== GLOOM RESULT ===");
	host_log(`backend: ${HOST_BACKEND}  seed: ${HOST_SEED}  workers: ${HOST_WORKERS}  ticks: ${G.tick}`);
	host_log(`mode: ${G.mode}  map: ${G.map_name}  deaths: ${G.deaths}`);
	host_log(`hp: ${G.hp}  armor: ${G.armor}  ammo: ${G.ammo[0]}/${G.ammo[1]}/${G.ammo[2]}`);
	host_log(`map kills: ${G.kills}/${G.kill_total}  secrets: ${G.secrets}/${G.secret_total}`);
	host_log(`episode kills: ${G.ep_kills}/${G.ep_kill_total}  secrets: ${G.ep_secrets}/${G.ep_secret_total}`);
	host_log(`warden down: ${G.warden_down}`);
	host_log(`STATE_HASH: ${G.hash}`);
}
