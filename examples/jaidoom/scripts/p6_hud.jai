// P6 hud gate: presentation layer. The status bar reserves its rows (view +
// bar == VH, renderer never writes below VIEWH), draws from real WAD patches
// with digits/face/keys matching player state; automap, title, menu,
// intermission and episode-end screens render; skill filter + item tally
// parameterization. Writes p6_ingame/p6_title/p6_menu/p6_inter BMPs.

import "wad.jai";
import "map.jai";
import "vid.jai";
import "render.jai";
import "tex.jai";
import "things.jai";
import "game.jai";
import "spec.jai";
import "hud.jai";

var FAILS = 0;
function check(bool ok, string what) -> void {
	if (ok) { print(`  ok    ${what}`); }
	else { FAILS = FAILS + 1; print(`  FAIL  ${what}`); }
}

function count_lit(int x0, int y0, int x1, int y1) -> int {
	int n = 0;
	for (int y = y0; y < y1; ++y) {
		int base = y * VW;
		for (int x = x0; x < x1; ++x) {
			if (PIX[base + x] != 0) { n = n + 1; }
		}
	}
	return n;
}

// sum of interned color ids over a rect: cheap content fingerprint
function region_sig(int x0, int y0, int x1, int y1) -> int {
	int s = 0;
	for (int y = y0; y < y1; ++y) {
		int base = y * VW;
		for (int x = x0; x < x1; ++x) { s = s + PIX[base + x] * (x + y * 7); }
	}
	return s;
}

function main() -> int {
	var path = wad_path("doom1");
	if (!wad_load(path)) { print("wad_load failed"); return 1; }

	// ---- required graphics lumps ----
	for (int d = 0; d <= 9; ++d) {
		check(lump_num(`STTNUM${d}`) >= 0 && lump_num(`STYSNUM${d}`) >= 0, `digit lumps ${d}`);
	}
	check(lump_num("STBAR") >= 0 && lump_num("STTPRCNT") >= 0, "STBAR + percent lumps");
	check(lump_num("STKEYS0") >= 0 && lump_num("STKEYS1") >= 0 && lump_num("STKEYS2") >= 0, "key lumps");
	bool faces = true;
	for (int f = 0; f <= 4; ++f) {
		if (lump_num(`STFST${f}0`) < 0 || lump_num(`STFOUCH${f}`) < 0) { faces = false; }
	}
	check(faces && lump_num("STFDEAD0") >= 0, "face lumps (straight, ouch, dead)");
	check(lump_num("TITLEPIC") >= 0 && lump_num("WIMAP0") >= 0, "TITLEPIC + WIMAP0");
	check(lump_num("STCFN065") >= 0 && lump_num("STCFN090") >= 0, "STCFN font lumps");

	// ---- bar reservation ----
	vid_init(320, 200);
	check(VIEWH == VH, "no bar reserved before hud_init");
	hud_init();
	check(BAR_H == 32, `BAR_H scaled from STBAR (${BAR_H})`);
	check(VIEWH + BAR_H == VH, `view rows + bar rows == VH (${VIEWH}+${BAR_H})`);
	if (!game_start_map("E1M1")) { print("no E1M1"); return 1; }
	check(LEVEL_START_TIC == GAME_TIC, "level time anchored at map start");

	// ---- in-game frame with the bar ----
	P_HP = 74;
	P_ARMOR = 12;
	P_KEYS = 5;                                      // blue + yellow
	AMMO_BULLETS = 37;
	AMMO_SHELLS = 3;
	PAIN_TICS = 0;
	render_view(P_X, P_Y, P_ANG);
	draw_weapon("PISG", "A");
	check(count_lit(0, VIEWH, VW, VH) == 0, "renderer + weapon stay out of the bar rows");
	check(count_lit(0, 0, VW, VIEWH) > VW * VIEWH / 2, "view region rendered");
	FACE_LOOK = 0;                                   // pin the idle glance for exact-name checks
	FACE_NEXT = GAME_TIC + 100000;
	hud_draw();
	check(HUD_AMMO_DRAWN == 37 && HUD_AMMO_DRAWN == AMMO_BULLETS, `ammo digits match state (${HUD_AMMO_DRAWN})`);
	check(HUD_HEALTH_DRAWN == 74 && HUD_ARMOR_DRAWN == 12, "health/armor digits match state");
	check(HUD_FACE == "STFST10", `face row picked by health (${HUD_FACE})`);
	check(count_lit(0, VIEWH, VW, VH) > BAR_H * VW / 2, "bar covers its rows");
	int cols_lit = 0;
	for (int x = 0; x < VW; ++x) {
		bool any = false;
		for (int y = VIEWH; y < VH; ++y) {
			if (PIX[y * VW + x] != 0) { any = true; break; }
		}
		if (any) { cols_lit = cols_lit + 1; }
	}
	check(cols_lit == VW, `bar spans the full width (${cols_lit}/${VW})`);
	int facex0 = 143 * VW / 320;
	int facex1 = 167 * VW / 320;
	check(count_lit(facex0, VIEWH, facex1, VH) > 40, "face pixels present");
	msg_draw();
	bmp_write("p6_ingame.bmp");
	print("  wrote p6_ingame.bmp");

	// ammo digits change the framebuffer where the count lives
	int ax0 = 8 * VW / 320;
	int ax1 = 46 * VW / 320;
	AMMO_BULLETS = 111;
	hud_draw();
	int sig1 = region_sig(ax0, VIEWH, ax1, VH);
	AMMO_BULLETS = 888;
	hud_draw();
	int sig8 = region_sig(ax0, VIEWH, ax1, VH);
	check(sig1 != sig8, "ammo count changes the drawn digits");

	// ---- arms panel: yellow owned digits vs gray unowned ----
	check(HUD_ARMS_DRAWN == 4, `arms mask: pistol only at start (${HUD_ARMS_DRAWN})`);
	int armx0 = 104 * VW / 320;
	int armx1 = 140 * VW / 320;
	int asig0 = region_sig(armx0, VIEWH, armx1, VH);
	check(count_lit(armx0, VIEWH, armx1, VH) > 40, "arms panel pixels present");
	HAVE_SHOTGUN = true;
	HAVE_CHAINGUN = true;
	hud_draw();
	check(HUD_ARMS_DRAWN == 28, `arms mask follows ownership (${HUD_ARMS_DRAWN})`);
	int asig1 = region_sig(armx0, VIEWH, armx1, VH);
	check(asig0 != asig1, "owning weapons changes the arms digits");
	HAVE_SHOTGUN = false;
	HAVE_CHAINGUN = false;
	hud_draw();
	check(region_sig(armx0, VIEWH, armx1, VH) == asig0, "dropping ownership restores the gray digits");

	// ---- face states (glance pinned to 0 above) ----
	P_HP = 100; PAIN_TICS = 0; GRIN_TICS = 0; FIRE_HELD = 0;
	hud_draw();
	check(HUD_FACE == "STFST00", `healthy face (${HUD_FACE})`);
	P_HP = 10;
	hud_draw();
	check(HUD_FACE == "STFST40", `near-death face (${HUD_FACE})`);
	PAIN_TICS = 8; FACE_PAIN_BIG = true; FACE_SIDE = 0;
	hud_draw();
	check(HUD_FACE == "STFOUCH4", `big pain gapes (${HUD_FACE})`);
	FACE_PAIN_BIG = false;
	FACE_SIDE = -1;
	hud_draw();
	check(HUD_FACE == "STFTL40", `hit from the left turns left (${HUD_FACE})`);
	FACE_SIDE = 1;
	hud_draw();
	check(HUD_FACE == "STFTR40", `hit from the right turns right (${HUD_FACE})`);
	FACE_SIDE = 0;
	hud_draw();
	check(HUD_FACE == "STFKILL4", `frontal pain grimaces (${HUD_FACE})`);
	GRIN_TICS = 70;
	hud_draw();
	check(HUD_FACE == "STFEVL4", `evil grin outranks pain (${HUD_FACE})`);
	GRIN_TICS = 0; PAIN_TICS = 0;
	FIRE_HELD = 71;
	hud_draw();
	check(HUD_FACE == "STFKILL4", `rampage face while trigger held (${HUD_FACE})`);
	FIRE_HELD = 0;
	// damage direction: an attacker 90° left of facing sets the turn
	P_HP = 50; P_ANG = 0.0;
	player_damage_from(5, P_X, P_Y + 100.0);
	check(FACE_SIDE == -1 && !FACE_PAIN_BIG, `left attacker -> FACE_SIDE -1 (${FACE_SIDE})`);
	player_damage_from(25, P_X + 100.0, P_Y);
	check(FACE_PAIN_BIG && FACE_SIDE == 0, "big frontal hit gapes instead");
	PAIN_TICS = 0;
	// idle glances animate: forcing a re-roll changes only the look digit
	P_HP = 100;
	FACE_NEXT = -1;
	hud_draw();
	check(HUD_FACE.starts_with("STFST0"), `glance stays on the health row (${HUD_FACE})`);
	bool saw_turn = false;
	for (int g = 0; g < 40 && !saw_turn; ++g) {
		FACE_NEXT = -1;
		GAME_TIC = GAME_TIC + 1;
		hud_draw();
		if (HUD_FACE == "STFST01" || HUD_FACE == "STFST02") { saw_turn = true; }
	}
	check(saw_turn, "the face glances around");
	P_HP = 0;
	hud_draw();
	check(HUD_FACE == "STFDEAD0", `dead face at 0 hp (${HUD_FACE})`);
	P_HP = 100;
	P_GOD = true;
	hud_draw();
	check(HUD_FACE == "STFGOD0", `god face under iddqd (${HUD_FACE})`);
	P_GOD = false;
	PAIN_TICS = 0;
	FACE_LOOK = 0;
	FACE_NEXT = GAME_TIC + 100000;

	// ---- automap ----
	P_HP = 74;
	automap_draw();
	hud_draw();
	bmp_write("p6_automap.bmp");
	print("  wrote p6_automap.bmp");
	check(count_lit(0, 0, VW, VIEWH) > 800, "automap lines drawn in the view region");
	int cp = rgb_idx(255, 255, 255);
	int arrow = 0;
	for (int y = VIEWH / 2 - 10; y < VIEWH / 2 + 10; ++y) {
		for (int x = VW / 2 - 10; x < VW / 2 + 10; ++x) {
			if (PIX[y * VW + x] == cp) { arrow = arrow + 1; }
		}
	}
	check(arrow >= 5, `player arrow at the center (${arrow} px)`);
	float z0 = AM_ZOOM;
	am_zoom(1.25);
	check(AM_ZOOM > z0, "zoom in steps");
	for (int zi = 0; zi < 40; ++zi) { am_zoom(0.5); }
	check(AM_ZOOM >= 0.5, "zoom clamped");
	AM_ZOOM = 1.0;

	// marks: numbered digit lands where the player stands, clear removes it
	am_marks_reset();
	automap_draw();
	int mx0 = VW / 2 - 12;
	int my0 = VIEWH / 2 - 12;
	int msig0 = region_sig(mx0, my0, mx0 + 24, my0 + 24);
	am_mark_add();
	automap_draw();
	check(AM_MARKS.size() == 1 && region_sig(mx0, my0, mx0 + 24, my0 + 24) != msig0, "mark 1 drawn at the player spot");
	am_mark_clear();
	automap_draw();
	check(AM_MARKS.size() == 0 && region_sig(mx0, my0, mx0 + 24, my0 + 24) == msig0, "marks cleared");
	// iddt stage 2: live things appear as crosses
	AM_CHEAT = 0;
	automap_draw();
	int lit0 = count_lit(0, 0, VW, VIEWH);
	AM_CHEAT = 2;
	automap_draw();
	int lit2 = count_lit(0, 0, VW, VIEWH);
	check(lit2 > lit0, `iddt reveal adds thing markers (${lit0} -> ${lit2})`);
	AM_CHEAT = 0;

	// ---- item tally: map pickups count, dropped ones don't ----
	check(ITEM_TOTAL > 10, `E1M1 pickups counted (${ITEM_TOTAL})`);
	check(ITEMS_TAKEN == 0, "no items taken yet");
	P_HP = 50; P_ARMOR = 0;
	AMMO_BULLETS = 10; AMMO_SHELLS = 0;
	int pick = -1;
	for (int m = 0; m < MO_X.size(); ++m) {
		if (MO_STATE[m] != 7 || MO_CNT[m] == 0) { continue; }
		var k = MO_SPR[m];
		if (k == "STIM" || k == "MEDI" || k == "BON1" || k == "BON2" || k == "CLIP" || k == "SHEL") { pick = m; break; }
	}
	check(pick >= 0, "found a countable pickup");
	if (pick >= 0) {
		P_X = MO_X[pick];
		P_Y = MO_Y[pick];
		pickup_touch();
		check(ITEMS_TAKEN >= 1, `taking a map pickup tallies (${ITEMS_TAKEN}/${ITEM_TOTAL})`);
	}
	int taken0 = ITEMS_TAKEN;
	int dropped = mobj_spawn(P_X, P_Y, 2007, 0.0, 7, "CLIP", "A", 0, 20.0);
	pickup_touch();
	check(MO_STATE[dropped] == 0 && ITEMS_TAKEN == taken0, "dropped items don't count in the tally");

	// ---- skill filter parameterized ----
	SKILL_BIT = 1;
	things_spawn_map();
	int easy = KILL_TOTAL;
	SKILL_BIT = 4;
	things_spawn_map();
	int hard = KILL_TOTAL;
	SKILL_BIT = 2;
	things_spawn_map();
	int med = KILL_TOTAL;
	check(easy > 0 && hard > easy && med >= easy && hard >= med, `skill spawn counts scale (${easy}/${med}/${hard})`);

	// ---- title / menu / intermission / episode end ----
	title_draw();
	check(count_lit(0, 0, VW, VH) > VW * VH / 2, "title screen mostly opaque");
	bmp_write("p6_title.bmp");
	print("  wrote p6_title.bmp");

	var entries = ["NEW GAME", "QUIT"];
	menu_draw(entries, 0, "");
	check(count_lit(0, 0, VW, VH) > 300, "menu drew logo + entries");
	bmp_write("p6_menu.bmp");
	print("  wrote p6_menu.bmp");

	wi_draw("E1M1", "E1M2", 85, 66, 33, 143);
	check(count_lit(0, 0, VW, VH) > VW * VH / 2, "intermission background + tally drawn");
	bmp_write("p6_inter.bmp");
	print("  wrote p6_inter.bmp");

	// ---- par times ----
	check(par_secs("E1M1") == 30 && par_secs("E1M5") == 165 && par_secs("E1M8") == 30, "ep1 par table");
	check(par_secs("NOPAR") == 0, "unknown maps carry no par");
	int py0 = VH * 30 / 100 + (VH * 13 / 100) * 3;
	int psig1 = region_sig(VW * 55 / 100, py0, VW * 92 / 100, py0 + 10);
	wi_draw("NOPAR", "E1M2", 85, 66, 33, 143);
	int psig0 = region_sig(VW * 55 / 100, py0, VW * 92 / 100, py0 + 10);
	check(psig1 != psig0, "PAR drawn beside TIME only when the map has one");

	end_draw();
	check(count_lit(0, 0, VW, VH) > 300, "episode end text drawn");

	// ---- the netgame footer: WI_FOOT replaces PRESS ANY KEY when set ----
	int fsc = text_scale();
	WI_FOOT = "";
	wi_draw("E1M1", "E1M2", 85, 66, 33, 143);
	int fsig0 = region_sig(0, VH - 12 * fsc, VW, VH);
	WI_FOOT = "WAITING FOR HOST";
	wi_draw("E1M1", "E1M2", 85, 66, 33, 143);
	int fsig1 = region_sig(0, VH - 12 * fsc, VW, VH);
	check(fsig0 != fsig1, "WAITING FOR HOST lands in the footer row");
	WI_FOOT = "";

	// ---- screen melt composite: the game view half-fallen over the tally ----
	render_view(P_X, P_Y, P_ANG);
	hud_draw();
	wipe_begin();
	wi_draw("E1M1", "E1M2", 85, 66, 33, 143);
	array<int> MINTER = PIX;
	for (int wt = 0; wt < 20; ++wt) { wipe_tic(); }
	wipe_overlay();
	int dold = 0;
	int dint = 0;
	for (int wp = 0; wp < VW * VH; ++wp) {
		if (PIX[wp] != WIPE_OLD[wp]) { dold = dold + 1; }
		if (PIX[wp] != MINTER[wp]) { dint = dint + 1; }
	}
	check(dold > 1000 && dint > 1000, `melt composite mixes both frames (old-diff ${dold}, new-diff ${dint})`);
	bmp_write("p6_melt.bmp");
	print("  wrote p6_melt.bmp (mid-melt composite)");
	WIPE_TICS = 0;

	print("");
	print(FAILS == 0 ? "P6 HUD GATE: ALL GREEN" : `P6 HUD GATE: ${FAILS} FAILURES`);
	return FAILS;
}

main()
