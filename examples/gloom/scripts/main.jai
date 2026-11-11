// GLOOM entry point. The host executes import "main.jai"; then calls
// gloom_boot(w, h) once and gloom_frame(dt, key, fps, ms_sim, ms_draw) per
// frame; --smoke also calls gloom_state_hash() and gloom_summary(). The
// contract and the tick are ../gloom's; only the style differs.

import "state.jai";
import "util.jai";
import "defs.jai";
import "maps.jai";
import "pure.jai";
import "particles.jai";
import "enemies.jai";
import "combat.jai";
import "sim.jai";
import "tune.jai";
import "look.jai";
import "lights.jai";
import "viewmodel.jai";
import "render.jai";
import "canvas.jai";
import "feel.jai";
import "sound.jai";
import "dev.jai";
import "screens.jai";
import "hud.jai";
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
	G.renderer.init();
	G.hud.init();
	G.autopilot = HOST_SMOKE || FORCE_PILOT;
	sound_boot();
	if (HOST_DEV && SHOWCASE) { dev_start(); }
}

// dt accumulates into fixed TICKs (smoke feeds exactly TICK: one tick/frame).
// The edge key is consumed by the first tick of the frame.
function gloom_frame(float dt, string key, float fps, float ms_sim, float ms_draw) -> string {
	if (HOST_DEV && SHOWCASE) { dev_frame(); }
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
	string text = G.hud.frame(fps, ms_sim, ms_draw);
	if (SHOWCASE) { sound_frame(); }
	return text;
}

function gloom_state_hash() -> int { return G.hash; }

// --respawn-test, headless: carry both big guns, low on ammo, onto the throne
// floor, die there, and rise with full health, the guns, the armor and every
// owned pool at its floor; the throne's first room holds a hexcaster
function gloom_respawn_test() -> int {
	for (int i = 0; i < 12; ++i) { gloom_frame(TICK, " ", 0.0, 0.0, 0.0); }
	G.player.have = [1, 1, 1];
	G.player.ammo = [30, 5, 3];
	G.player.armor = 40;
	G.player.weapon = 2;
	G.map_i = MAPS.size() - 2;
	G.finish_map();
	int hexcasters = 0;
	for (auto& it : G.items) {
		if (it.kind == 9 && dist2(it.x, it.y, G.player.px, G.player.py) < 16.0) { hexcasters = hexcasters + 1; }
	}
	G.player.ammo = [2, 0, 0];
	G.player.damage(999, "the test");
	for (int i = 0; i < 40; ++i) { gloom_frame(TICK, i > 30 ? " " : "", 0.0, 0.0, 0.0); }
	auto& p = G.player;
	string got = `${G.world.map_name} deaths ${G.deaths} hp ${p.hp} armor ${p.armor} have ${p.have[0]}${p.have[1]}${p.have[2]} weapon ${p.weapon} ammo ${p.ammo[0]}/${p.ammo[1]}/${p.ammo[2]} hexcasters ${hexcasters}`;
	bool ok = G.mode == 1 && G.map_i == MAPS.size() - 1 && G.deaths == 1 && p.hp == 100 && p.armor == 40 && p.have[1] == 1
		&& p.have[2] == 1 && p.weapon == 2 && p.ammo[0] == 48 && p.ammo[1] == 16 && p.ammo[2] == 40 && hexcasters == 1;
	host_log(`RESPAWN TEST: ${ok ? "PASS" : "FAIL"} (${got})`);
	return ok ? 0 : 1;
}

function gloom_wants_quit() -> bool { return G.quit; }

function gloom_summary() -> void {
	host_log("");
	host_log("=== GLOOM RESULT ===");
	host_log(`backend: ${HOST_BACKEND}  seed: ${HOST_SEED}  workers: ${HOST_WORKERS}  ticks: ${G.tick}`);
	host_log(`mode: ${G.mode}  map: ${G.world.map_name}  deaths: ${G.deaths}`);
	host_log(`hp: ${G.player.hp}  armor: ${G.player.armor}  ammo: ${G.player.ammo[0]}/${G.player.ammo[1]}/${G.player.ammo[2]}`);
	host_log(`map kills: ${G.kills}/${G.kill_total}  secrets: ${G.secrets}/${G.secret_total}`);
	host_log(`episode kills: ${G.ep_kills}/${G.ep_kill_total}  secrets: ${G.ep_secrets}/${G.ep_secret_total}`);
	host_log(`warden down: ${G.warden_down}`);
	host_log(`STATE_HASH: ${G.hash}`);
	if (SHOWCASE && HOST_SMOKE) { host_log(`sound: ${SFX_NAMES.size()} effects, ${SFX_BANK_BYTES} bytes, bank ${SFX_BANK_HASH}, ${CUE_COUNT} cues ${CUE_HASH}`); }
}
