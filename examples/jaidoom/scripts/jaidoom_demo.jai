// JaiDOOM fly-through demo — drag this file onto jaiscript.exe.
// Renders a scripted camera path through E1M1 straight to the terminal
// (100% pure JaiScript: stock runner, no host additions needed).
// Optional: jaiscript jaidoom_demo.jai -- <wad> <map> <cells_wide> <frames>

import "wad.jai";
import "map.jai";
import "vid.jai";
import "render.jai";

function main() -> int {
	var path = wad_path("doom1");
	var mapname = "E1M1";
	int cw = 110;
	int frames = 420;
	if (ARGS.size() > 0) { path = ARGS[0]; }
	if (ARGS.size() > 1) { mapname = ARGS[1]; }
	if (ARGS.size() > 2) { cw = ARGS[2].to_int(110); }
	if (ARGS.size() > 3) { frames = ARGS[3].to_int(420); }

	if (!wad_load(path)) { print("wad_load failed"); return 1; }
	if (!map_load(mapname)) { print(`no map ${mapname}`); return 1; }
	vid_init(cw, (cw * 10) / 11);   // ~square-ish view, 2 px per cell row
	prepare_render();

	// waypoints through E1M1: start room -> main hall -> zigzag window wall ->
	// back across the hall to the eastern door. (x, y, look-at-turn in rad)
	var wx = [1056.0, 1056.0, 1130.0, 928.0, 704.0, 640.0, 896.0, 1270.0, 1400.0];
	var wy = [-3616.0, -3392.0, -3264.0, -3104.0, -3104.0, -3360.0, -3390.0, -3360.0, -3360.0];
	var wa = [1.5708, 1.5708, 1.2, 2.6, 3.14159, 4.4, 5.6, 6.2832, 6.2832];

	var out_prefix = ESC + "[H";
	print(ESC + "[2J" + ESC + "[?25l", skip_newline);   // clear + hide cursor

	int legs = wx.size() - 1;
	for (int f = 0; f < frames; ++f) {
		float t = 1.0 * f / frames * legs;
		int leg = math::itrunc(t);
		if (leg >= legs) { leg = legs - 1; }
		float ft = t - leg;
		float s = ft * ft * (3.0 - 2.0 * ft);   // smoothstep per leg
		float px = wx[leg] + (wx[leg + 1] - wx[leg]) * s;
		float py = wy[leg] + (wy[leg + 1] - wy[leg]) * s;
		float ang = wa[leg] + (wa[leg + 1] - wa[leg]) * s;
		render_view(px, py, ang);
		print(out_prefix + blit_half(), skip_newline);
	}

	print(ESC + "[0m" + ESC + "[?25h");   // restore
	print(`jaidoom demo: ${frames} frames at ${VW}x${VH} px (${VW}x${VH / 2} cells), map ${mapname}`);
	return 0;
}

main()
