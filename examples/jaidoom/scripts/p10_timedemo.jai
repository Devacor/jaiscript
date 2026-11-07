// JaiDOOM timedemo — replay a recorded demo UNCAPPED and report where the
// time goes. The profiling workhorse: deterministic real gameplay, no sleep,
// no terminal writes (the frame string is built but never printed — stdout
// would dominate). Faithful to play()'s per-frame work: sim tic, full render
// with weapon overlay, hud, blit; a time::stopwatch laps each stage of every
// tic, so the split and the worst tic are exact.
//
//   jaiscript p10_timedemo.jai                       (../demo.json or demo.json)
//   jaiscript p10_timedemo.jai -- mytape.json 200x140
//   jaiscript p10_timedemo.jai -- simonly            (playsim isolated, no render)

import "sim_random.jai";
import "wad.jai";
import "map.jai";
import "vid.jai";
import "render.jai";
import "things.jai";
import "game.jai";
import "hud.jai";
import "save.jai";
import "demo.jai";

// jaidoom.jai owns player_reset; mirrored here (importing the main would run it)
function td_player_reset() -> void {
	P_HP = 100;
	P_ARMOR = 0;
	P_KEYS = 0;
	AMMO_BULLETS = 50;
	AMMO_SHELLS = 0;
	AMMO_ROCKETS = 0;
	HAVE_SHOTGUN = false;
	HAVE_CHAINGUN = false;
	HAVE_LAUNCHER = false;
	CUR_WEAPON = 2;
	PAIN_TICS = 0;
	BONUS_TICS = 0;
	GRIN_TICS = 0;
	FIRE_HELD = 0;
}

function main() -> int {
	var wadpath = wad_path("doom1");
	var tape = "";
	int bw = 120;
	int bh = 78;
	bool simonly = false;
	for (int a = 0; a < ARGS.size(); ++a) {
		var arg = ARGS[a];
		if (arg == "simonly") { simonly = true; }
		else if (arg.ends_with(".json")) { tape = arg; }
		else {
			int xat = arg.find("x");
			if (xat > 0) {
				int aw = arg.substr(0, xat).to_int(0);
				int ah = arg.substr(xat + 1, arg.size() - xat - 1).to_int(0);
				if (aw > 0 && ah > 0) { bw = aw; bh = ah; }
			}
		}
	}
	if (tape == "") { tape = data_path("demo.json"); }
	if (!wad_load(wadpath)) { print("wad_load failed"); return 1; }
	if (!demo_load(tape)) { print(`no demo at ${tape}`); return 1; }
	vid_init(bw, bh);
	hud_init();

	// demo_play_start's launch sequence, minus the menu/terminal machinery
	SKILL_BIT = DEMO_SKILL;
	NIGHTMARE = DEMO_NM;
	FAST_MONSTERS = DEMO_NM;
	td_player_reset();
	GAME_TIC = 0;
	SIM_RANDOM.seed(DEMO_SEED);
	DEMO_POS = 0;
	DEMO_PLAY = true;
	if (!game_start_map(DEMO_MAP)) { print(`no map ${DEMO_MAP}`); return 1; }

	int ntics = DEMO_TICS.size() / 6;
	print(`timedemo: ${tape}  map ${DEMO_MAP} skill ${DEMO_SKILL}${DEMO_NM ? " NIGHTMARE" : ""}  ${ntics} tics  vid ${bw}x${bh}${simonly ? " SIMONLY" : ""}`);

	float sim_s = 0.0;
	float rend_s = 0.0;
	float blit_s = 0.0;
	float worst = 0.0;
	int frame_bytes = 0;
	time::stopwatch run;
	time::stopwatch tic;
	run.start();
	while (demo_feed()) {
		tic.start();
		game_tic();
		sim_s += tic.delta();
		if (!simonly) {
			EYE_Z = P_VIEWZ;
			EXTRA_LIGHT = FLASH_TICS > 0 ? 1 : 0;
			render_view(P_X, P_Y, P_ANG);
			if (P_HP > 0) {
				var w = WEAPONS[CUR_WEAPON];
				draw_weapon(w.spr, w.view_frame());
				if (FLASH_TICS > 0) { draw_weapon_flash(w.flash, "A"); }
			}
			hud_draw();
			rend_s += tic.delta();
			var frame = blit_half();
			frame_bytes = frame.size();
			blit_s += tic.delta();
		}
		float spent = tic.elapsed();
		if (spent > worst) { worst = spent; }
		if (LEVEL_DONE) { break; }
	}
	int wall = run.elapsed() * 1000;
	if (wall < 1) { wall = 1; }
	int played = GAME_TIC;
	float mspt = 1.0 * wall / imax(1, played);
	float realtime = played / 35.0;
	float speedup = realtime * 1000.0 / wall;

	print(`played ${played} tics in ${wall} ms  =  ${mspt:.2} ms/tic  (${speedup:.1}x realtime, ${math::itrunc(played * 1000.0 / wall)} tics/sec)`);
	print(`split: sim ${sim_s * 1000 / imax(1, played):.2}  render ${rend_s * 1000 / imax(1, played):.2}  blit ${blit_s * 1000 / imax(1, played):.2}  (ms/tic, aggregate)  worst tic ${worst * 1000:.1} ms  frame ${frame_bytes} bytes`);
	print(`STATE_HASH: ${demo_state_hash()}  (run twice: identical hash = uncapped replay is deterministic)`);
	return 0;
}

function imax(int a, int b) -> int { return a > b ? a : b; }

main()
