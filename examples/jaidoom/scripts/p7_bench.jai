// P7 gate: performance + feel. Headless autopilot frame-time measurement at
// realistic play size (term 120x40 -> vid 120x78, hud bar on): 300 frames of
// scripted movement through the E1M1 start room with the nearest monsters
// force-woken, budget 28.5 ms/frame (35fps), sim / render / blit split.
// Then the feel features: weapon bob, viewz easing, muzzle-flash light
// boost, explicit eye height, FPS counter.

import "wad.jai";
import "map.jai";
import "vid.jai";
import "render.jai";
import "things.jai";
import "game.jai";
import "hud.jai";

var FRAMES = 300;
var FAILS = 0;

function check(bool ok, string what) -> void {
	if (ok) { print(`  ok    ${what}`); }
	else { FAILS = FAILS + 1; print(`  FAIL  ${what}`); }
}

function wake_nearest(int count) -> void {
	int woken = 0;
	while (woken < count) {
		int best = -1;
		float bestd = 1000000000.0;
		for (int m = 0; m < MO_X.size(); ++m) {
			if (MO_STATE[m] != 1) { continue; }
			float dx = MO_X[m] - P_X;
			float dy = MO_Y[m] - P_Y;
			float d2 = dx * dx + dy * dy;
			if (d2 < bestd) { bestd = d2; best = m; }
		}
		if (best < 0) { return; }
		MO_STATE[best] = 2;
		woken = woken + 1;
	}
}

function first_px() -> int {
	int n = VW * VH;
	for (int i = 0; i < n; ++i) {
		if (PIX[i] != 0) { return i; }
	}
	return -1;
}

function main() -> int {
	var path = wad_path("doom1");
	if (!wad_load(path)) { print("wad_load failed"); return 1; }
	// knobs: `-- 200x140` sets the vid size, `-- serial` / `-- parallel` pins the blit path
	int bw = 120;
	int bh = 78;
	for (int a = 0; a < ARGS.size(); ++a) {
		var arg = ARGS[a];
		if (arg == "serial") { BLIT_WORKERS = 0; }
		else if (arg == "parallel") { BLIT_WORKERS = 1; }
		else {
			int xat = arg.find("x");
			if (xat > 0) {
				int aw = arg.substr(0, xat).to_int(0);
				int ah = arg.substr(xat + 1, arg.size() - xat - 1).to_int(0);
				if (aw > 0 && ah > 0) { bw = aw; bh = ah; }
			}
		}
	}
	vid_init(bw, bh);
	hud_init();
	if (!game_start_map("E1M1")) { print("no E1M1"); return 1; }
	wake_nearest(6);

	// scripted run through the start room: forward with a slow weave, firing bursts
	float simms = 0.0;
	float renderms = 0.0;
	float blitms = 0.0;
	float worst = 0.0;
	time::stopwatch lap;
	for (int f = 0; f < FRAMES; ++f) {
		CMD_FWD = 50.0;
		CMD_SIDE = 0.0;
		CMD_TURN = f < 100 ? 0.0 : (((f / 50) & 1) == 0 ? 0.04 : -0.04);
		CMD_FIRE = (f % 40) < 6;
		CMD_USE = false;
		lap.start();
		game_tic();
		simms += lap.delta() * 1000;
		EYE_Z = P_VIEWZ;
		EXTRA_LIGHT = FLASH_TICS > 0 ? 1 : 0;
		render_view(P_X, P_Y, P_ANG);
		draw_weapon("PISG", "A");
		if (FLASH_TICS > 0) { draw_weapon("PISF", "A"); }
		hud_draw();
		msg_draw();
		renderms += lap.delta() * 1000;
		blit_half();
		blitms += lap.delta() * 1000;
		float spent = lap.elapsed() * 1000;
		if (spent > worst) { worst = spent; }
	}
	float total = simms + renderms + blitms;
	float perframe = total / FRAMES;
	fps_note(perframe);
	fps_draw();
	bmp_write("p7_view.bmp");
	print(`P7 BENCH: ${perframe:.2} ms/frame over ${FRAMES} frames (budget 28.5)`);
	print(`  sim ${simms / FRAMES:.2}  render ${renderms / FRAMES:.2}  blit ${blitms / FRAMES:.2}  worst ${worst:.1}`);
	print(`  view ${VW}x${VIEWH}+${BAR_H} bar, sprites last frame ${SPRITES_DRAWN}, pos (${P_X:.0}, ${P_Y:.0})`);
	check(perframe <= 28.5, "under the 35fps budget");

	// ------------------------------------------------------------------ feel ---

	// weapon bob follows momentum: strong while running, dies with it
	// (staged in the spawn corridor: the autopilot run ends wall-stuck, momentum dead)
	P_X = 1056.0;
	P_Y = -3550.0;
	P_ANG = math::PI / 2.0;
	P_MOMX = 0.0;
	P_MOMY = 0.0;
	P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
	CMD_FIRE = false;
	CMD_TURN = 0.0;
	CMD_FWD = 50.0;
	float bobpeak = 0.0;
	for (int b = 0; b < 20; ++b) {
		game_tic();
		float mag = WBOB_X * WBOB_X + WBOB_Y * WBOB_Y;
		if (mag > bobpeak) { bobpeak = mag; }
	}
	check(bobpeak > 4.0, `bob swings while running (peak mag2 ${bobpeak:.1})`);
	CMD_FWD = 0.0;
	for (int b2 = 0; b2 < 80; ++b2) { game_tic(); }
	check(WBOB_X * WBOB_X + WBOB_Y * WBOB_Y < 0.1, "bob settles at rest");

	// the bob offset really moves the drawn weapon
	WBOB_X = 0.0; WBOB_Y = 0.0;
	pix_clear(0);
	draw_weapon("PISG", "A");
	int p0 = first_px();
	WBOB_X = 20.0;
	pix_clear(0);
	draw_weapon("PISG", "A");
	int p1 = first_px();
	check(p0 >= 0 && p1 >= 0 && p0 != p1, `bob shifts the weapon overlay (${p0} -> ${p1})`);
	WBOB_X = 0.0;

	// viewz eases toward floor + 41 at 8u/tic instead of snapping
	P_MOMX = 0.0; P_MOMY = 0.0;
	game_tic();
	P_VIEWZ = P_FLOORZ + 41.0 - 24.0;
	game_tic();
	check(P_VIEWZ == P_FLOORZ + 41.0 - 16.0, `viewz eased up 8 (${P_VIEWZ - P_FLOORZ - 41.0:.0} to go)`);
	game_tic();
	game_tic();
	check(P_VIEWZ == P_FLOORZ + 41.0, "viewz settled at floor + 41");
	P_VIEWZ = P_FLOORZ + 41.0 + 20.0;
	game_tic();
	check(P_VIEWZ == P_FLOORZ + 41.0 + 12.0, "viewz eases down too");
	for (int v = 0; v < 5; ++v) { game_tic(); }

	// muzzle flash boosts the light band by one, clamped at full bright
	EXTRA_LIGHT = 0;
	int b0 = light_band(160, 0.0);
	EXTRA_LIGHT = 1;
	check(light_band(160, 0.0) == b0 - 1, `flash brightens one band (${b0} -> ${b0 - 1})`);
	check(light_band(255, 0.0) == 0, "full bright stays clamped");
	EXTRA_LIGHT = 0;

	// explicit eye height drives the renderer; sentinel derives from the sector
	EYE_Z = P_FLOORZ + 10.0;
	render_view(P_X, P_Y, P_ANG);
	check(VIEW_Z == P_FLOORZ + 10.0, "render_view takes the explicit eye height");
	EYE_Z = -100000.0;
	render_view(P_X, P_Y, P_ANG);
	check(VIEW_Z == SE_FLOORH[sector_at(P_X, P_Y)] + 41.0, "sentinel derives eye height from the sector");

	// FPS counter: EMA math and hud digits
	FPS_EMA = 0.0;
	fps_note(20);
	check(FPS_EMA == 20.0, "first frame seeds the EMA");
	fps_note(30);
	check(FPS_EMA == 21.0, `EMA blends (${FPS_EMA:.1})`);
	pix_clear(0);
	fps_draw();
	check(first_px() >= 0, "fps counter draws on the hud");

	print("");
	print(FAILS == 0 ? "P7 GATE: ALL GREEN" : `P7 GATE: ${FAILS} FAILURES`);
	return FAILS;
}

main()
