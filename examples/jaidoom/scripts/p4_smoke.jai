// P4 smoke: the interactive boot order (vid_init -> game_start_map -> render
// -> vid_blit), then a mid-game level transition — the paths play() takes.

import "wad.jai";
import "map.jai";
import "vid.jai";
import "render.jai";
import "game.jai";

function main() -> int {
	var path = wad_path("doom1");
	if (!wad_load(path)) { print("wad_load failed"); return 1; }

	vid_init(120, 56);                               // play() order: video first
	if (!game_start_map("E1M1")) { print("no E1M1"); return 1; }
	render_view(P_X, P_Y, P_ANG);
	var f1 = vid_blit();
	print(`E1M1 frame: ${f1.size()} bytes, palette ${PAL_FG.size()} colors`);

	for (int i = 0; i < 35; ++i) { CMD_FWD = 25.0; game_tic(); }   // walk a second
	render_view(P_X, P_Y, P_ANG);
	vid_blit();

	if (!game_start_map("E1M2")) { print("no E1M2"); return 1; }   // exit-switch path
	render_view(P_X, P_Y, P_ANG);
	var f2 = vid_blit();
	print(`E1M2 frame: ${f2.size()} bytes, palette ${PAL_FG.size()} colors`);

	print("P4 SMOKE: ALL GREEN");
	return 0;
}

main()
