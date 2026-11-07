// P2 gate: decode E1M1 and draw it top-down through the terminal blitter.
// Run: jaiscript p2_automap.jai [-- wadpath [mapname [width]]]

import "wad.jai";
import "map.jai";
import "vid.jai";

function main() -> int {
	var path = wad_path("doom1");
	var mapname = "E1M1";
	int cells_w = 188;
	if (ARGS.size() > 0) { path = ARGS[0]; }
	if (ARGS.size() > 1) { mapname = ARGS[1]; }

	if (!wad_load(path)) { print("wad_load failed"); return 1; }
	if (!map_load(mapname)) { print(`no map ${mapname}`); return 1; }

	print(`${mapname}: ${VX_X.size()} verts, ${LD_V1.size()} lines, ${SE_FLOORH.size()} sectors, ` +
		`${TH_X.size()} things, bounds x[${MAP_MINX},${MAP_MAXX}] y[${MAP_MINY},${MAP_MAXY}]`);

	int span_x = MAP_MAXX - MAP_MINX;
	int span_y = MAP_MAXY - MAP_MINY;
	int pw = cells_w;
	int ph = (span_y * pw) / span_x;
	vid_init(pw, ph + 2);

	int col_wall = rgb_idx(255, 96, 64);      // one-sided: solid wall
	int col_step = rgb_idx(190, 170, 120);    // two-sided, floor height differs
	int col_flat = rgb_idx(90, 90, 110);      // two-sided, same floor
	int col_thing = rgb_idx(80, 200, 90);
	int col_player = rgb_idx(255, 255, 255);

	// map -> pixel: uniform scale, y flipped (doom is y-up)
	float scale = 1.0 * (pw - 1) / span_x;
	int nlines = LD_V1.size();
	for (int i = 0; i < nlines; ++i) {
		int v1 = LD_V1[i];
		int v2 = LD_V2[i];
		int x0 = math::itrunc((VX_X[v1] - MAP_MINX) * scale);
		int y0 = math::itrunc((MAP_MAXY - VX_Y[v1]) * scale);
		int x1 = math::itrunc((VX_X[v2] - MAP_MINX) * scale);
		int y1 = math::itrunc((MAP_MAXY - VX_Y[v2]) * scale);
		int c = col_wall;
		int s1 = LD_SIDE1[i];
		if (s1 >= 0) {
			int s0 = LD_SIDE0[i];
			int f0 = SE_FLOORH[SD_SEC[s0]];
			int f1 = SE_FLOORH[SD_SEC[s1]];
			c = f0 == f1 ? col_flat : col_step;
		}
		draw_line(x0, y0, x1, y1, c);
	}

	int nthings = TH_X.size();
	for (int t = 0; t < nthings; ++t) {
		int tx = math::itrunc((TH_X[t] - MAP_MINX) * scale);
		int ty = math::itrunc((MAP_MAXY - TH_Y[t]) * scale);
		int tc = TH_TYPE[t] == 1 ? col_player : col_thing;
		pix_set(tx, ty, tc);
		if (TH_TYPE[t] == 1) {
			pix_set(tx - 1, ty, tc); pix_set(tx + 1, ty, tc);
			pix_set(tx, ty - 1, tc); pix_set(tx, ty + 1, tc);
		}
	}

	print(blit_half());

	// side artifact for headless verification: pixel-grid silhouette at cell resolution
	var preview = "";
	for (int cy = 0; cy < VH / 2; ++cy) {
		var row = "";
		for (int x = 0; x < VW; ++x) {
			int top = PIX[cy * 2 * VW + x];
			int bot = PIX[(cy * 2 + 1) * VW + x];
			if (top == col_wall || bot == col_wall) { row += "#"; }
			else if (top == col_player || bot == col_player) { row += "@"; }
			else if (top == col_thing || bot == col_thing) { row += "o"; }
			else if (top != 0 || bot != 0) { row += "."; }
			else { row += " "; }
		}
		preview += row + "\n";
	}
	write_file("p2_preview.txt", preview);
	return 0;
}

main()
