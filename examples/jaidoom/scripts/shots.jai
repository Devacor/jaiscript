// JaiDOOM frames for the docs page: the title screen and a fight, drawn through
// the blit the terminal gets and written as one .ans file each. A fixed seed and
// scripted tics, no clock and no keyboard, so the same WAD gives the same frames.
//
//   jaiscript shots.jai -- [<wad>] [<map>] [<cols>x<rows * 2>] [pix half|sext] [out <dir>]
//                          [fight <mobj>] [tics <n>] [every <n>]
//
// The screen is <cols> by <rows> cells, drawn in half blocks (1x2 pixels per cell) unless pix sext
// draws it in sextants (2x3).
//
// The fight stands the player in sight of one monster (by mobj index; the first one with a
// clear line by default), wakes it and fires for <tics> tics; every <n> adds numbered frames
// of it, to choose one.

import "sim_random.jai";
import "wad.jai";
import "map.jai";
import "vid.jai";
import "render.jai";
import "things.jai";
import "game.jai";
import "spec.jai";
import "hud.jai";
import "fx.jai";

string OUT_DIR = ".";

function shoot(string name) -> void {
	write_file(`${OUT_DIR}/${name}.ans`, vid_blit());
	print(`  ${OUT_DIR}/${name}.ans`);
}

// play()'s draw path: the pain palette, the view, the weapon and its flash, the status bar
function draw_play() -> void {
	int red = PAIN_TICS;
	set_palette_bank(red > 0 ? 1 + (red > 42 ? 7 : red / 6) : 0);
	EXTRA_LIGHT = FLASH_TICS > 0 ? 1 : 0;
	EYE_Z = P_VIEWZ;
	render_view(P_X, P_Y, P_ANG);
	if (P_HP > 0) {
		var w = WEAPONS[CUR_WEAPON];
		draw_weapon(w.spr, w.view_frame());
		if (FLASH_TICS > 0) { draw_weapon_flash(w.flash, "A"); }
	}
	hud_draw();
	msg_draw();
}

// a standable spot with a clear line to monster m, facing it
function face_monster(int m) -> bool {
	for (int a = 0; a < 8; ++a) {
		float ang = a * math::PI / 4.0;
		for (int di = 0; di < 3; ++di) {
			float d = 200.0 + di * 80.0;
			float px = MO_X[m] + math::cos(ang) * d;
			float py = MO_Y[m] + math::sin(ang) * d;
			P_FLOORZ = SE_FLOORH[sector_at(px, py)];
			if (!pos_ok(px, py) || !sight_clear(px, py, MO_X[m], MO_Y[m], P_FLOORZ + 41.0)) { continue; }
			P_X = px;
			P_Y = py;
			P_Z = P_FLOORZ * 1.0;
			P_VIEWZ = P_FLOORZ + 41.0;
			P_ANG = math::atan2(MO_Y[m] - py, MO_X[m] - px);
			return true;
		}
	}
	return false;
}

function main() -> int {
	string wad = "freedoom1";
	string mapname = "E1M1";
	int cols = 160;
	int prows = 98;
	int target = -1;
	int tics = 30;
	int every = 0;
	bool panel = false;
	for (int i = 0; i < ARGS.size(); ++i) {
		string a = ARGS[i];
		string next = i + 1 < ARGS.size() ? ARGS[i + 1] : "";
		if (a == "out") { OUT_DIR = next; i += 1; }
		else if (a == "fight") { target = next.to_int(-1); i += 1; }
		else if (a == "tics") { tics = next.to_int(tics); i += 1; }
		else if (a == "every") { every = next.to_int(0); i += 1; }
		else if (a == "pix") { SEXTANTS = next == "sext"; i += 1; }
		else if (a == "panel") { panel = true; }
		else if (a == "fit") { SEXT_FIT = next.to_int(1); i += 1; }
		else if (a == "gamma") { GAMMA = next.to_float(0.0); i += 1; }
		else if (a.starts_with("E") || a.starts_with("MAP")) { mapname = a; }
		else if (a.find("x") > 0 && a.substr(0, a.find("x")).to_int(0) > 0) {
			int x = a.find("x");
			cols = a.substr(0, x).to_int(cols);
			prows = a.substr(x + 1, a.size() - x - 1).to_int(prows);
		}
		else { wad = a; }
	}
	if (!wad_load(wad_path(wad))) { print(`no WAD ${wad}: run jaidoom.jai once to fetch it`); return 1; }
	HOST_TRUECOLOR = true;   // the page's frames are 24-bit wherever they are captured
	vid_init_cells(cols, prows / 2);
	hud_init();

	fire_start(7);
	for (int f = 0; f < 70; ++f) { fire_tic(); }
	title_draw();
	shoot("title");

	SKILL_BIT = 2;
	SIM_RANDOM.seed(1);
	if (!game_start_map(mapname)) { print(`no map ${mapname}`); return 1; }
	if (target < 0) {
		for (int m = 0; m < MO_X.size() && target < 0; ++m) {
			if (MO_STATE[m] == 1 && face_monster(m)) { target = m; }
		}
	}
	if (target < 0 || target >= MO_X.size() || !face_monster(target)) { print("no monster with a clear line"); return 1; }
	MO_STATE[target] = 2;
	CMD_FIRE = true;
	for (int t = 1; t <= tics; ++t) {
		P_ANG = math::atan2(MO_Y[target] - P_Y, MO_X[target] - P_X);
		game_tic();
		if (every > 0 && t % every == 0) {
			draw_play();
			shoot(`fight_${t}`);
		}
	}
	draw_play();
	shoot("fight");
	if (panel) {                                     // the J panel over the fight, with made-up but typical numbers
		BLIT_WORKERS = 1;
		for (int k = 0; k < PANEL_SPAN; ++k) {
			stat_note(0.1 + (k % 5) * 0.03, 2.6 + (k % 7) * 0.2, 0.7 + (k % 3) * 0.1, 1.8 + (k % 4) * 0.3, 16.7);
		}
		write_file(`${OUT_DIR}/fight_panel.ans`, `${ESC}[1;1H` + vid_blit() + panel_text(2, 2));
		print(`  ${OUT_DIR}/fight_panel.ans`);
	}
	return 0;
}

main()
