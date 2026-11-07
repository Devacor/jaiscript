// P8 gate: DOOM II support proven against freedoom2.wad — MAPxx loading and
// progression (15->31->32->16, 30 ends the game), the doom2 monsters that
// probed in (cacodemon/hell knight/SS/chaingunner + their projectiles), the
// MAPxx music table, per-third skies, the INTERPIC intermission fallback.
// Eyeball artifact: p8_doom2.bmp (MAP01 spawn view with the status bar).

import "wad.jai";
import "map.jai";
import "vid.jai";
import "tex.jai";
import "render.jai";
import "things.jai";
import "game.jai";
import "hud.jai";
import "mus.jai";

var FAILS = 0;

function check(bool ok, string what) -> void {
	if (ok) { print(`  ok    ${what}`); }
	else { FAILS = FAILS + 1; print(`  FAIL  ${what}`); }
}

function first_px() -> int {
	int n = VW * VH;
	for (int i = 0; i < n; ++i) {
		if (PIX[i] != 0) { return i; }
	}
	return -1;
}

function main() -> int {
	var path = wad_path("freedoom2");
	if (!wad_load(path)) { print("wad_load failed"); return 1; }
	check(lump_num("MAP01") >= 0 && lump_num("E1M1") < 0, "doom2 iwad: MAPxx markers, no ExMy");

	vid_init(320, 200);
	hud_init();
	SKILL_BIT = 2;
	check(game_start_map("MAP01"), "MAP01 loads through the generic map path");

	// spawns: known types in, unknown doom2-only types skipped silently
	int monsters = 0;
	int newtypes = 0;
	for (int m = 0; m < MO_TYPE.size(); ++m) {
		if (MO_STATE[m] != 1 || !MINFO.has(MO_TYPE[m])) { continue; }
		monsters = monsters + 1;
		int ty = MO_TYPE[m];
		if (ty == 3005 || ty == 69 || ty == 84 || ty == 65) { newtypes = newtypes + 1; }
	}
	check(monsters > 0, `MAP01 spawns monsters (${monsters})`);

	// the probe-gated doom2 additions: freedoom2 ships full art for all four
	check(MINFO.has(3005), "cacodemon (HEAD) admitted by the art probe");
	check(MINFO.has(69), "hell knight (BOS2) admitted");
	check(MINFO.has(84), "SS trooper (SSWV) admitted");
	check(MINFO.has(65), "chaingunner (CPOS) admitted");
	check(MISSILE_SPR.has(3005) && MISSILE_SPR[3005] == "BAL2", "cacodemon fireball uses BAL2");
	check(MISSILE_SPR.has(69) && MISSILE_SPR[69] == "BAL7", "knight fireball uses BAL7");
	check(TFOG_ANIM.size() >= 4, `teleport fog art probed ("${TFOG_ANIM}")`);

	// a cacodemon dropped next to the player chases and works the attack machinery
	float cacox = P_X + 40.0;
	int caco = mobj_spawn(cacox, P_Y, 3005, 0.0, 2, "HEAD", "A", 400, 31.0);
	for (int ct = 0; ct < 60; ++ct) { monster_tic(caco); mobj_anim_tic(caco); }
	check(MO_X[caco] != cacox || MO_COOL[caco] > 0 || MO_STATE[caco] == 3,
		"spawned cacodemon acts (moved or attacked)");

	// progression: the doom2 chain incl. the secret pair
	GAME_MAP = "MAP01"; SECRET_EXIT = false;
	check(next_map() == "MAP02", "MAP01 -> MAP02");
	GAME_MAP = "MAP09";
	check(next_map() == "MAP10", "MAP09 -> MAP10 (zero-padded rollover)");
	GAME_MAP = "MAP15"; SECRET_EXIT = true;
	check(next_map() == "MAP31", "MAP15 secret -> MAP31");
	GAME_MAP = "MAP31"; SECRET_EXIT = true;
	check(next_map() == "MAP32", "MAP31 secret -> MAP32");
	GAME_MAP = "MAP31"; SECRET_EXIT = false;
	check(next_map() == "MAP16", "MAP31 -> MAP16");
	GAME_MAP = "MAP32";
	check(next_map() == "MAP16", "MAP32 -> MAP16");
	GAME_MAP = "MAP30";
	check(next_map() == "", "MAP30 ends the game");
	SECRET_EXIT = false;
	GAME_MAP = "MAP01";

	// music: table names resolve to real lumps in this wad
	check(map_music("MAP01") == "D_RUNNIN" && lump_num("D_RUNNIN") >= 0, "MAP01 music D_RUNNIN resolves");
	check(map_music("MAP10") == "D_DEAD" && lump_num("D_DEAD") >= 0, "MAP10 music D_DEAD resolves");
	check(map_music("MAP11") == "D_RUNNIN", "MAP11 reuses the table cyclically");
	bool musall = true;
	for (int mi = 1; mi <= 10; ++mi) {
		if (lump_num(map_music(mi < 10 ? `MAP0${mi}` : `MAP${mi}`)) < 0) { musall = false; }
	}
	check(musall, "all ten table lumps ship in freedoom2");
	check(map_music("E1M1") == "D_E1M1", "episodic naming unchanged");

	// skies rotate by map third
	check(sky_name("MAP01") == "SKY1" && sky_name("MAP11") == "SKY1", "first third SKY1");
	check(sky_name("MAP12") == "SKY2" && sky_name("MAP20") == "SKY2", "second third SKY2");
	check(sky_name("MAP21") == "SKY3" && sky_name("MAP32") == "SKY3", "final third SKY3");
	check(sky_name("E1M1") == "SKY1" && sky_name("E3M5") == "SKY3", "episodes keep their sky");
	check(SKY_TEX == tex_id("SKY1") && SKY_TEX >= 0, "MAP01 render uses SKY1");

	// the eyeball artifact: spawn view + status bar
	EYE_Z = P_VIEWZ;
	render_view(P_X, P_Y, P_ANG);
	hud_draw();
	bmp_write("p8_doom2.bmp");
	check(first_px() >= 0, "MAP01 frame renders (p8_doom2.bmp)");

	// intermission: no WIMAP0 in doom2 wads -> INTERPIC carries the tally screen
	check(lump_num("WIMAP0") < 0 && lump_num("INTERPIC") >= 0, "WIMAP0 absent, INTERPIC present");
	pix_clear(0);
	wi_draw("MAP01", "MAP02", 100, 50, 0, 90);
	check(first_px() >= 0, "intermission draws via the INTERPIC fallback");

	// skipped doom2 bestiary (no MINFO row yet, spawns skip them silently):
	// 66 revenant, 67 mancubus, 68 arachnotron, 64 archvile, 71 pain elemental,
	// 7 spiderdemon, 16 cyberdemon, 88/89/87 icon fixtures, 82 SSG + doom2 pickups
	print("");
	print(FAILS == 0 ? "P8 GATE: ALL GREEN" : `P8 GATE: ${FAILS} FAILURES`);
	return FAILS;
}

main()
