// P3 stage-1 gate: first-person solid-wall view from the E1M1 player start.
// Run: jaiscript p3_view.jai [-- wadpath [mapname [show]]]

import "wad.jai";
import "map.jai";
import "vid.jai";
import "render.jai";

function main() -> int {
	var path = wad_path("doom1");
	var mapname = "E1M1";
	var show = false;
	if (ARGS.size() > 0) { path = ARGS[0]; }
	if (ARGS.size() > 1) { mapname = ARGS[1]; }
	if (ARGS.size() > 2 && ARGS[2] == "show") { show = true; }

	if (!wad_load(path)) { print("wad_load failed"); return 1; }
	if (!map_load(mapname)) { print(`no map ${mapname}`); return 1; }
	vid_init(320, 200);
	prepare_render();

	float px = 0.0;
	float py = 0.0;
	float ang = 0.0;
	int nthings = TH_X.size();
	for (int t = 0; t < nthings; ++t) {
		if (TH_TYPE[t] == 1) {
			px = TH_X[t];
			py = TH_Y[t];
			ang = TH_ANG[t] * math::PI / 180.0;
			break;
		}
	}
	print(`view: (${px}, ${py}) facing ${ang:.3} rad, sector ${sector_at(px, py)}, floor ${SE_FLOORH[sector_at(px, py)]}`);

	render_view(px, py, ang);

	int texready = 0;
	for (int i = 0; i < TEX_READY.size(); ++i) { if (TEX_READY[i] != 0) { texready = texready + 1; } }
	print(`columns unfilled after walk: ${REMAIN} of ${VW}`);
	print(`interned colors: ${PAL_FG.size()}   textures composited: ${texready}   flats: ${FLATS.size()}`);
	bmp_write("p3_view.bmp");
	print("wrote p3_view.bmp");
	if (show) { print(blit_half(), skip_newline); }

	int mid = PIX[(VH / 2) * VW + VW / 2];
	print(`center pixel palette idx ${mid} rgb ${PAL_R[mid]},${PAL_G[mid]},${PAL_B[mid]}`);
	return 0;
}

main()
