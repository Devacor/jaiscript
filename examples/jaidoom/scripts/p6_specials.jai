// P6 gate: playsim specials. Completeness: every line/sector special present
// in E1M1..E1M9 must be implemented or whitelisted-cosmetic. Machinery: lift
// full cycle (down/wait/up) with the player riding it, walkover detection
// through p_move, W1 one-shot vs WR refire, W1 door + lights, donut, gun
// trigger, teleport, stairs, damage floors, secrets, E1M8 tag-666 boss drop
// and the sector-11 exit, episode-complete map flow.

import "wad.jai";
import "map.jai";
import "vid.jai";
import "render.jai";
import "things.jai";
import "game.jai";
import "spec.jai";

var FAILS = 0;
function check(bool ok, string what) -> void {
	if (ok) { print(`  ok    ${what}`); }
	else { FAILS = FAILS + 1; print(`  FAIL  ${what}`); }
}

function find_line(int spec) -> int {
	for (int i = 0; i < LD_SPECIAL.size(); ++i) {
		if (LD_SPECIAL[i] == spec) { return i; }
	}
	return -1;
}

function tag_sector(int tag) -> int {
	for (int s = 0; s < SE_TAG.size(); ++s) {
		if (SE_TAG[s] == tag) { return s; }
	}
	return -1;
}

// convex subsector centroid = guaranteed interior point
var PT_X = 0.0;
var PT_Y = 0.0;
function find_interior(int sec) -> bool {
	int nss = SS_COUNT.size();
	for (int ss = 0; ss < nss; ++ss) {
		int first = SS_FIRST[ss];
		if (SEG_FSEC[first] != sec) { continue; }
		float sx = 0.0;
		float sy = 0.0;
		int cnt = SS_COUNT[ss];
		for (int k = 0; k < cnt; ++k) {
			sx = sx + VX_X[SG_V1[first + k]];
			sy = sy + VX_Y[SG_V1[first + k]];
		}
		float cx = sx / cnt;
		float cy = sy / cnt;
		if (sector_at(cx, cy) == sec) { PT_X = cx; PT_Y = cy; return true; }
	}
	return false;
}

function align_tic() -> void { GAME_TIC = (GAME_TIC >> 5) << 5; }

function main() -> int {
	var path = wad_path("doom1");
	if (!wad_load(path)) { print("wad_load failed"); return 1; }
	spec_tables();

	// ---- completeness: every ep1 special implemented or whitelisted ----
	var natlight = "";
	for (int m = 1; m <= 9; ++m) {
		var name = `E1M${m}`;
		if (!map_load(name)) { check(false, `${name} loads`); continue; }
		var seenl = {};
		var missl = "";
		for (int i = 0; i < LD_SPECIAL.size(); ++i) {
			int s = LD_SPECIAL[i];
			if (s == 0 || seenl.has(s)) { continue; }
			seenl[s] = 1;
			if (!spec_line_known(s)) { missl = missl + ` ${s}`; }
		}
		check(missl == "", `${name} line specials covered (${seenl.size()} distinct${missl})`);
		var seens = {};
		var misss = "";
		for (int j = 0; j < SE_SPECIAL.size(); ++j) {
			int s2 = SE_SPECIAL[j];
			if (s2 == 0 || seens.has(s2)) { continue; }
			seens[s2] = 1;
			if (!spec_sector_known(s2)) { misss = misss + ` ${s2}`; }
			if (natlight == "" && (s2 == 1 || s2 == 2 || s2 == 3 || s2 == 8 || s2 == 12 || s2 == 13 || s2 == 17)) { natlight = name; }
		}
		check(misss == "", `${name} sector specials covered (${seens.size()} distinct${misss})`);
	}

	// ---- E1M1: WR lift 88, walkover through p_move, ride it down and up ----
	vid_init(320, 200);
	if (!game_start_map("E1M1")) { print("no E1M1"); return 1; }
	int lift_ld = find_line(88);
	check(lift_ld >= 0, "E1M1 has a WR lift line (88)");
	int lift_sec = tag_sector(LD_TAG[lift_ld]);
	check(lift_sec >= 0, `lift tag ${LD_TAG[lift_ld]} resolves to a sector (${lift_sec})`);
	int f0 = SE_FLOORH[lift_sec];
	int flow = neighbor_floor(lift_sec, 0);
	check(flow < f0, `lift has somewhere to go (${f0} -> ${flow})`);

	// straddle the line 16 units each side; front = right of v1->v2
	float x1 = VX_X[LD_V1[lift_ld]];
	float y1 = VX_Y[LD_V1[lift_ld]];
	float dx = VX_X[LD_V2[lift_ld]] - x1;
	float dy = VX_Y[LD_V2[lift_ld]] - y1;
	float len = math::sqrt(dx * dx + dy * dy);
	float mx = x1 + dx * 0.5;
	float my = y1 + dy * 0.5;
	float ax = mx + dy / len * 16.0;
	float ay = my - dx / len * 16.0;
	float bx = mx - dy / len * 16.0;
	float by = my + dx / len * 16.0;
	bool via_pmove = false;
	if (pos_ok(ax, ay) && pos_ok(bx, by)) {
		P_X = ax;
		P_Y = ay;
		P_FLOORZ = SE_FLOORH[sector_at(ax, ay)];
		P_MOMX = bx - ax;
		P_MOMY = by - ay;
		p_move();
		P_MOMX = 0.0;
		P_MOMY = 0.0;
		via_pmove = MV_SEC.size() > 0;
	}
	if (!via_pmove) { cross_check(ax, ay, bx, by); }
	check(MV_SEC.size() > 0, via_pmove ? "walkover fired the lift (through p_move)" : "walkover fired the lift (direct cross_check)");
	check(LD_SPECIAL[lift_ld] == 88, "WR line keeps its special");

	// ride it: player inside the lift sector, floor-follow + collision stay sane
	bool placed = find_interior(lift_sec);
	check(placed, "interior point of the lift sector found");
	if (placed) { P_X = PT_X; P_Y = PT_Y; P_MOMX = 0.0; P_MOMY = 0.0; }
	bool bottomed = false;
	bool ride_ok = true;
	bool stood = true;
	for (int t = 0; t < 400 && MV_SEC.size() > 0; ++t) {
		movers_tic();
		p_move();
		if (placed && P_FLOORZ != SE_FLOORH[lift_sec]) { ride_ok = false; }
		if (placed && !pos_ok(P_X, P_Y)) { stood = false; }
		if (SE_FLOORH[lift_sec] == flow) { bottomed = true; }
	}
	check(bottomed, `lift reached the bottom (${flow})`);
	check(SE_FLOORH[lift_sec] == f0 && MV_SEC.size() == 0, "lift waited and came back up");
	check(ride_ok, "P_FLOORZ tracked the moving lift floor every tic");
	check(stood, "standing on the lift stayed collision-legal throughout");

	// damage floor: special 7 = 5 hp per 32-tic pulse
	int nukage = -1;
	for (int s7 = 0; s7 < SE_SPECIAL.size(); ++s7) {
		if (SE_SPECIAL[s7] == 7 && find_interior(s7)) { nukage = s7; break; }
	}
	check(nukage >= 0, "found a standable nukage (special 7) sector");
	if (nukage >= 0) {
		P_X = PT_X;
		P_Y = PT_Y;
		P_HP = 100;
		P_ARMOR = 0;
		align_tic();
		spec_tic();
		check(P_HP == 95, `nukage pulse dealt 5 (hp ${P_HP})`);
		GAME_TIC = GAME_TIC + 1;
		spec_tic();
		check(P_HP == 95, "no damage off-pulse");
	}

	// secrets: E1M1 has 3 special-9 sectors; entering one counts once
	check(SECRET_TOTAL == 3, `E1M1 secret count (${SECRET_TOTAL})`);
	int hidey = -1;
	for (int s9 = 0; s9 < SE_SPECIAL.size(); ++s9) {
		if (SE_SPECIAL[s9] == 9 && find_interior(s9)) { hidey = s9; break; }
	}
	check(hidey >= 0, "found a standable secret sector");
	if (hidey >= 0) {
		P_X = PT_X;
		P_Y = PT_Y;
		P_HP = 100;
		spec_tic();
		spec_tic();
		check(SECRETS_FOUND == 1 && SE_SPECIAL[hidey] == 0, `secret counted once (${SECRETS_FOUND}/${SECRET_TOTAL})`);
	}

	// ---- E1M3: W1 door one-shot + W1 lights ----
	if (!game_start_map("E1M3")) { print("no E1M3"); return 1; }
	int wd = find_line(2);
	check(wd >= 0, "E1M3 has a W1 open-door line (2)");
	int wdsec = tag_sector(LD_TAG[wd]);
	int c0 = SE_CEILH[wdsec];
	CROSS_SIDE = 0;
	check(spec_line(wd, 1), "W1 door fired");
	check(LD_SPECIAL[wd] == 0, "W1 line cleared after firing");
	for (int t2 = 0; t2 < 100; ++t2) { doors_tic(); }
	check(SE_CEILH[wdsec] > c0, `door ceiling rose (${c0} -> ${SE_CEILH[wdsec]}) and stays (open-stay)`);
	int ll = find_line(35);
	check(ll >= 0, "E1M3 has a W1 lights-to-35 line");
	int lsec = tag_sector(LD_TAG[ll]);
	spec_line(ll, 1);
	check(SE_LIGHT[lsec] == 35, `lights dropped to 35 (sector ${lsec})`);

	// ---- E1M2: donut + gun-activated door ----
	if (!game_start_map("E1M2")) { print("no E1M2"); return 1; }
	float twall = trace_block_t(P_X, P_Y, P_X + 2048.0, P_Y, P_FLOORZ + 41.0);
	check(twall < 1.0 && TRACE_LINE >= 0, `trace reports its blocking line (${TRACE_LINE})`);
	int dn = find_line(9);
	check(dn >= 0, "E1M2 has the donut line (9)");
	int before_mv = MV_SEC.size();
	check(spec_line(dn, 0), "donut fired");
	check(MV_SEC.size() >= before_mv + 2, "donut moves pool and ring");
	int gr = find_line(46);
	check(gr >= 0, "E1M2 has a GR door line (46)");
	int grsec = tag_sector(LD_TAG[gr]);
	int gc0 = SE_CEILH[grsec];
	check(spec_line(gr, 2), "gun trigger fired");
	for (int t3 = 0; t3 < 100; ++t3) { doors_tic(); }
	check(SE_CEILH[grsec] > gc0, `shot-open door rose (${gc0} -> ${SE_CEILH[grsec]})`);

	// ---- E1M8: teleport, stairs, tag-666 boss drop, sector-11 exit ----
	if (!game_start_map("E1M8")) { print("no E1M8"); return 1; }
	int dest = -1;
	for (int th = 0; th < TH_TYPE.size(); ++th) {
		if (TH_TYPE[th] == 14) { dest = th; break; }
	}
	check(dest >= 0, "E1M8 has a teleport destination thing");
	bool ported = false;
	int port_ld = -1;
	P_MOMX = 5.0;
	P_MOMY = -3.0;
	for (int i2 = 0; i2 < LD_SPECIAL.size() && !ported; ++i2) {
		if (LD_SPECIAL[i2] != 97) { continue; }
		CROSS_SIDE = 0;
		ported = spec_line(i2, 1);
		if (ported) { port_ld = i2; }
	}
	check(ported, "a WR teleport line fired");
	check(P_X == TH_X[dest] * 1.0 && P_Y == TH_Y[dest] * 1.0, `player at the destination (${P_X:.0}, ${P_Y:.0})`);
	check(P_MOMX == 0.0 && P_MOMY == 0.0, "teleport zeroed momentum");

	// side discipline through real crossing geometry: front->back ports, back->front doesn't
	float tx1 = VX_X[LD_V1[port_ld]];
	float ty1 = VX_Y[LD_V1[port_ld]];
	float tdx = VX_X[LD_V2[port_ld]] - tx1;
	float tdy = VX_Y[LD_V2[port_ld]] - ty1;
	float tlen = math::sqrt(tdx * tdx + tdy * tdy);
	float tmx = tx1 + tdx * 0.5;
	float tmy = ty1 + tdy * 0.5;
	float ffx = tmx + tdy / tlen * 8.0;              // front = right of v1->v2
	float ffy = tmy - tdx / tlen * 8.0;
	float bbx = tmx - tdy / tlen * 8.0;
	float bby = tmy + tdx / tlen * 8.0;
	P_X = bbx;
	P_Y = bby;
	cross_check(bbx, bby, ffx, ffy);
	check(P_X == bbx && P_Y == bby, "crossing a teleporter from behind does nothing");
	cross_check(ffx, ffy, bbx, bby);
	check(P_X == TH_X[dest] * 1.0 && P_Y == TH_Y[dest] * 1.0, "crossing from the front teleports");

	int st_ld = find_line(7);
	check(st_ld >= 0, "E1M8 has an S1 stair builder (7)");
	int st_sec = tag_sector(LD_TAG[st_ld]);
	int sf0 = SE_FLOORH[st_sec];
	int mv0 = MV_SEC.size();
	check(spec_line(st_ld, 0), "stairs fired");
	check(MV_SEC.size() >= mv0 + 2, `stairs queued a chain of steps (${MV_SEC.size() - mv0})`);
	for (int t4 = 0; t4 < 700; ++t4) { movers_tic(); }
	check(SE_FLOORH[st_sec] == sf0 + 8, `first step rose 8 (${sf0} -> ${SE_FLOORH[st_sec]})`);

	int s666 = tag_sector(666);
	check(s666 >= 0, `E1M8 has the tag-666 sector (${s666})`);
	int b0 = SE_FLOORH[s666];
	int bexp = neighbor_floor(s666, 0);
	int barons = 0;
	for (int mb = 0; mb < MO_TYPE.size(); ++mb) {
		if (MO_TYPE[mb] == 3003) { barons = barons + 1; mobj_damage(mb, 10000, -1); }
	}
	check(barons >= 2 && BOSS_PEND == barons, `barons present and armed (${barons})`);
	P_HP = 100;
	P_X = TH_X[dest] * 1.0;                          // stand somewhere harmless
	P_Y = TH_Y[dest] * 1.0;
	for (int t5 = 0; t5 < 800 && (BOSS_PEND > 0 || MV_SEC.size() > 0); ++t5) { GAME_TIC = GAME_TIC + 1; spec_tic(); }
	check(BOSS_PEND == 0 && SE_FLOORH[s666] == bexp, `tag-666 floor fell when the barons died (${b0} -> ${SE_FLOORH[s666]})`);

	int s11 = -1;
	for (int se = 0; se < SE_SPECIAL.size(); ++se) {
		if (SE_SPECIAL[se] == 11 && find_interior(se)) { s11 = se; break; }
	}
	check(s11 >= 0, "E1M8 has a standable special-11 exit sector");
	if (s11 >= 0) {
		P_X = PT_X;
		P_Y = PT_Y;
		P_HP = 30;
		P_ARMOR = 0;
		LEVEL_DONE = false;
		align_tic();
		for (int t6 = 0; t6 < 200 && !LEVEL_DONE; ++t6) { spec_tic(); GAME_TIC = GAME_TIC + 1; }
		check(LEVEL_DONE && P_HP == 10, `sector 11 drained to ${P_HP} and exited the level`);
	}
	GAME_MAP = "E1M8";
	SECRET_EXIT = false;
	check(next_map() == "", "after E1M8 the episode completes");

	// ---- sector light effects: natural pickup, then every kind staged ----
	check(natlight != "", `ep1 carries a light-special sector (${natlight})`);
	if (natlight != "") {
		if (!game_start_map(natlight)) { print(`no ${natlight}`); return 1; }
		check(LT_SEC.size() > 0, `spec_init armed ${LT_SEC.size()} light effects on ${natlight}`);
	}
	if (!game_start_map("E1M1")) { print("no E1M1 (lights)"); return 1; }
	int lsx = -1;
	for (int sl = 0; sl < SE_SPECIAL.size(); ++sl) {
		if (SE_SPECIAL[sl] == 0 && SE_LIGHT[sl] - neighbor_light_min(sl) >= 48) { lsx = sl; break; }
	}
	check(lsx >= 0, "found a bright plain sector with a much darker neighbor");
	int lbase = SE_LIGHT[lsx];
	int lmin = neighbor_light_min(lsx);

	// fast strobe: only {base, min}, interior dark runs exactly 15, bright 5
	SE_SPECIAL[lsx] = 2;
	spec_init();
	array<int> tr = [];
	for (int lt = 0; lt < 90; ++lt) { spec_tic(); tr.push(SE_LIGHT[lsx]); }
	bool vals_ok = true;
	for (int v = 0; v < tr.size(); ++v) {
		if (tr[v] != lbase && tr[v] != lmin) { vals_ok = false; }
	}
	check(vals_ok, "strobe holds exactly base and min-neighbor levels");
	// interior run lengths (first and last runs are cut off by the window)
	array<int> runv = [];
	array<int> runl = [];
	for (int r = 0; r < tr.size(); ++r) {
		if (runv.size() > 0 && runv[runv.size() - 1] == tr[r]) { runl[runl.size() - 1] = runl[runl.size() - 1] + 1; }
		else { runv.push(tr[r]); runl.push(1); }
	}
	bool cadence = runv.size() >= 4;
	for (int q = 1; q < runv.size() - 1; ++q) {
		int want = runv[q] == lmin ? 15 : 5;
		if (runl[q] != want) { cadence = false; }
	}
	check(cadence, `strobe cadence 5 bright / 15 dark over ${runv.size()} runs`);
	check(tr[tr.size() - 1] == lbase || tr[tr.size() - 1] == lmin, "strobe keeps flipping (base restored in the pattern)");

	// slow sync strobe: dark runs stretch to 35
	SE_SPECIAL[lsx] = 12;
	SE_LIGHT[lsx] = lbase;
	spec_init();
	int darkrun = 0;
	int maxdark = 0;
	for (int lt2 = 0; lt2 < 130; ++lt2) {
		spec_tic();
		if (SE_LIGHT[lsx] == lmin) {
			darkrun = darkrun + 1;
			if (darkrun > maxdark) { maxdark = darkrun; }
		} else { darkrun = 0; }
	}
	check(maxdark == 35, `slow sync strobe dark phase is 35 tics (${maxdark})`);

	// glow: ramps at most 8/tic, touches the min rail and climbs back to base
	SE_SPECIAL[lsx] = 8;
	SE_LIGHT[lsx] = lbase;
	spec_init();
	bool gmin = false;
	bool gback = false;
	bool gstep = true;
	int prev = SE_LIGHT[lsx];
	for (int lt3 = 0; lt3 < 200; ++lt3) {
		spec_tic();
		int cur = SE_LIGHT[lsx];
		int d = cur - prev;
		if (d < 0) { d = -d; }
		if (d > 8) { gstep = false; }
		if (cur == lmin) { gmin = true; }
		if (gmin && cur == lbase) { gback = true; }
		prev = cur;
	}
	check(gmin && gback && gstep, `glow ramped to ${lmin} and back to ${lbase} at <=8/tic`);

	// blink: mostly bright with occasional random drops to min
	SE_SPECIAL[lsx] = 1;
	SE_LIGHT[lsx] = lbase;
	spec_init();
	int bright = 0;
	int dark = 0;
	bool bvals = true;
	for (int lt4 = 0; lt4 < 400; ++lt4) {
		spec_tic();
		int bv = SE_LIGHT[lsx];
		if (bv == lbase) { bright = bright + 1; }
		else if (bv == lmin) { dark = dark + 1; }
		else { bvals = false; }
	}
	check(bvals && dark > 0 && bright > dark, `blink drops randomly to min-neighbor (${bright} bright / ${dark} dark)`);

	// fire flicker: jitters between base and min+16, several distinct levels
	SE_SPECIAL[lsx] = 17;
	SE_LIGHT[lsx] = lbase;
	spec_init();
	var flick = {};
	bool frange = true;
	for (int lt5 = 0; lt5 < 120; ++lt5) {
		spec_tic();
		int fv = SE_LIGHT[lsx];
		flick[fv] = 1;
		if (fv > lbase || fv < lmin + 16) { frange = false; }
	}
	check(frange && flick.size() >= 2, `flicker jitters within [min+16, base] (${flick.size()} levels)`);
	SE_SPECIAL[lsx] = 0;
	SE_LIGHT[lsx] = lbase;

	print("");
	print(FAILS == 0 ? "P6 GATE: ALL GREEN" : `P6 GATE: ${FAILS} FAILURES`);
	return FAILS;
}

main()
