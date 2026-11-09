// JaiDOOM hud.jai — presentation from real WAD graphics: STBAR status bar
// with STTNUM/STYSNUM digits, STKEYS and the STF face, automap overlay,
// TITLEPIC title, STCFN-font menu, WIMAP0 intermission tally, episode end.
// Patches ride things.jai's sprite cache (same picture format). game.jai /
// spec.jai globals (P_HP, KILLS, SECRETS_FOUND, ...) resolve at call time.

import "wad.jai";
import "map.jai";
import "vid.jai";
import "tex.jai";
import "things.jai";
import "fx.jai";

// netplay.jai mirror behind NET_GAME: extern-declared so programs that never import netplay.jai still check
extern NET_FRAGS_MINE;

int BAR_H = 0;
float BSX = 1.0;              // bar space (320x32) -> screen scale
float BSY = 1.0;
int HUD_AMMO_DRAWN = -1;      // what the bar last rendered (gate hooks)
int HUD_HEALTH_DRAWN = -1;
int HUD_ARMOR_DRAWN = -1;
string HUD_FACE = "";
bool AM_ON = false;
float AM_ZOOM = 1.0;

function hud_init() -> void {
	gfx_init();                                      // title/menu draw before any map loads PALCM
	hud_layout();
}

// the status bar's share of the screen, again after a resize
function hud_layout() -> void {
	BAR_H = pixel_rows(32.0 * VW / 320.0);
	if (BAR_H < 4) { BAR_H = 4; }
	if (BAR_H > VH / 3) { BAR_H = VH / 3; }
	VIEWH = VH - BAR_H;
	BSX = VW / 320.0;
	BSY = BAR_H / 32.0;
}

// nearest-sampled patch blit into an arbitrary rect; -1 stays transparent
function draw_patch(int lump, int x0, int y0, int w, int h) -> void {
	sprite_prepare(lump);
	int pw = SPR_W[lump];
	int ph = SPR_H[lump];
	if (w < 1 || h < 1 || pw < 1 || ph < 1) { return; }
	var& tp = SPR_PIX[lump];
	var& pix = PIX;
	var& palcm = PALCM;
	for (int sx = 0; sx < w; ++sx) {
		int px = x0 + sx;
		if (px < 0 || px >= VW) { continue; }
		int base = (sx * pw / w) * ph;
		for (int sy = 0; sy < h; ++sy) {
			int py = y0 + sy;
			if (py < 0 || py >= VH) { continue; }
			int texel = tp[base + sy * ph / h];
			if (texel >= 0) { pix[py * VW + px] = palcm[texel]; }
		}
	}
}

// the patch's opaque texels in one color: a text shadow
function draw_patch_solid(int lump, int x0, int y0, int w, int h, int color) -> void {
	sprite_prepare(lump);
	int pw = SPR_W[lump];
	int ph = SPR_H[lump];
	if (w < 1 || h < 1 || pw < 1 || ph < 1) { return; }
	var& tp = SPR_PIX[lump];
	for (int sx = 0; sx < w; ++sx) {
		int px = x0 + sx;
		if (px < 0 || px >= VW) { continue; }
		int base = (sx * pw / w) * ph;
		for (int sy = 0; sy < h; ++sy) {
			int py = y0 + sy;
			if (py >= 0 && py < VH && tp[base + sy * ph / h] >= 0) { PIX[py * VW + px] = color; }
		}
	}
}

function draw_patch_name(string name, int x, int y, int w, int h) -> bool {
	int lu = lump_num(name);
	if (lu < 0) { return false; }
	draw_patch(lu, x, y, w, h);
	return true;
}

// ------------------------------------------------------------- status bar ---

// honors patch offsets like the original blitter (x - leftofs, y - topofs):
// the STF faces carry (-5,-2), so ignoring them drew doomguy up-left
function bar_patch(string name, int bx, int by) -> void {
	int lu = lump_num(name);
	if (lu < 0) { return; }
	sprite_prepare(lu);
	int w = math::itrunc(SPR_W[lu] * BSX);
	int h = math::itrunc(SPR_H[lu] * BSY);
	if (w < 1) { w = 1; }
	if (h < 1) { h = 1; }
	draw_patch(lu, math::itrunc((bx - SPR_LO[lu]) * BSX), VIEWH + math::itrunc((by - SPR_TO[lu]) * BSY), w, h);
}

// digits right-aligned ending at bar-space x = rx
function bar_num(string pfx, int val, int rx, int by) -> void {
	if (val < 0) { val = 0; }
	while (true) {
		string nm = `${pfx}${val % 10}`;
		int lu = lump_num(nm);
		if (lu < 0) { return; }
		sprite_prepare(lu);
		int dw = SPR_W[lu];
		rx = rx - dw;
		bar_patch(nm, rx, by);
		val = val / 10;
		if (val == 0) { break; }
	}
}

// the face performs: dead > evil grin (weapon pickup) > pain (big hits gape,
// small hits turn TOWARD the attacker, frontal hits grimace) > rampage
// (trigger held 2s) > idle glances left/center/right
int FACE_LOOK = 0;
int FACE_NEXT = -1;           // GAME_TIC of the next glance re-roll
int FACE_SEED = 4321;

// hud-only LCG: the face glance must never touch SIM_RANDOM — that stream
// is the sim's, and demos replay it (demo.jai pins the seed)
function face_rnd(int n) -> int {
	FACE_SEED = (FACE_SEED * 1103515245 + 12345) & 2147483647;
	return (FACE_SEED >> 16) % n;
}

function face_name() -> string {
	if (P_HP <= 0) { return "STFDEAD0"; }
	if (P_GOD) { return "STFGOD0"; }
	int idx = 4;
	if (P_HP >= 80) { idx = 0; }
	else if (P_HP >= 60) { idx = 1; }
	else if (P_HP >= 40) { idx = 2; }
	else if (P_HP >= 20) { idx = 3; }
	if (GRIN_TICS > 0) { return `STFEVL${idx}`; }
	if (PAIN_TICS > 0) {
		if (FACE_PAIN_BIG) { return `STFOUCH${idx}`; }
		if (FACE_SIDE < 0) { return `STFTL${idx}0`; }
		if (FACE_SIDE > 0) { return `STFTR${idx}0`; }
		return `STFKILL${idx}`;                      // frontal grimace shares the rampage art
	}
	if (FIRE_HELD >= 70) { return `STFKILL${idx}`; }
	if (GAME_TIC >= FACE_NEXT) {
		FACE_LOOK = face_rnd(3);
		FACE_NEXT = GAME_TIC + 25 + face_rnd(46);
	}
	return `STFST${idx}${FACE_LOOK}`;
}

// the face gets its own blit: centered on the slot (bar-space 159.5), scaled
// to fill the bar height with aspect kept, rounded not truncated — bar_patch
// at raw patch size read small and drifted up-left at terminal scales
function face_draw(string name) -> void {
	int lu = lump_num(name);
	if (lu < 0) { return; }
	sprite_prepare(lu);
	float fill = 30.0 / SPR_H[lu];
	int w = math::itrunc(SPR_W[lu] * fill * BSX + 0.5);
	int h = math::itrunc(30.0 * BSY + 0.5);
	if (w < 1) { w = 1; }
	if (h < 1) { h = 1; }
	int x = math::itrunc(159.5 * BSX - w / 2.0 + 0.5);
	int y = VIEWH + math::itrunc(31.0 * BSY + 0.5) - h;
	draw_patch(lu, x, y, w, h);
}

int HUD_ARMS_DRAWN = 0;       // owned-slot bitmask the arms panel last showed (gate hook)

// arms panel: STARMS backdrop, then slots 2-7 — yellow digit owned, gray
// unowned. Deathmatch swaps the panel for the FRAG count (the classic bar).
int HUD_FRAGS_DRAWN = -1;

function arms_draw() -> void {
	if (NET_GAME && !NP_COOP) {                      // frags are a deathmatch thing
		bar_num("STTNUM", NET_FRAGS_MINE, 138, 3);
		HUD_FRAGS_DRAWN = NET_FRAGS_MINE;
		HUD_ARMS_DRAWN = -1;
		return;
	}
	HUD_FRAGS_DRAWN = -1;
	bar_patch("STARMS", 104, 0);
	int arms = 0;
	for (int i = 0; i < 6; ++i) {
		int slot = i + 2;
		bool owned = slot == 2 || (slot == 3 && HAVE_SHOTGUN)
			|| (slot == 4 && HAVE_CHAINGUN) || (slot == 5 && HAVE_LAUNCHER);
		bar_patch(owned ? `STYSNUM${slot}` : `STGNUM${slot}`, 111 + (i % 3) * 12, 4 + (i / 3) * 10);
		if (owned) { arms = arms | (1 << slot); }
	}
	HUD_ARMS_DRAWN = arms;
}

function hud_draw() -> void {
	if (BAR_H <= 0) { return; }
	bar_patch("STBAR", 0, 0);
	arms_draw();
	if (CUR_WEAPON != 1) {                           // fist shows no ammo count
		int ammo = AMMO_BULLETS;
		if (CUR_WEAPON == 3) { ammo = AMMO_SHELLS; }
		else if (CUR_WEAPON == 5) { ammo = AMMO_ROCKETS; }
		bar_num("STTNUM", ammo, 44, 3);
		HUD_AMMO_DRAWN = ammo;
	} else { HUD_AMMO_DRAWN = -1; }
	bar_patch("STTPRCNT", 90, 3);
	bar_num("STTNUM", P_HP, 90, 3);
	HUD_HEALTH_DRAWN = P_HP;
	bar_patch("STTPRCNT", 221, 3);
	bar_num("STTNUM", P_ARMOR, 221, 3);
	HUD_ARMOR_DRAWN = P_ARMOR;
	HUD_FACE = face_name();
	face_draw(HUD_FACE);
	if ((P_KEYS & 1) != 0) { bar_patch("STKEYS0", 239, 3); }
	if ((P_KEYS & 4) != 0) { bar_patch("STKEYS1", 239, 13); }
	if ((P_KEYS & 2) != 0) { bar_patch("STKEYS2", 239, 23); }
	bar_num("STYSNUM", AMMO_BULLETS, 288, 5);        // per-weapon ammo / max pairs
	bar_num("STYSNUM", AMMO_BULLETS_MAX, 314, 5);
	bar_num("STYSNUM", AMMO_SHELLS, 288, 11);
	bar_num("STYSNUM", AMMO_SHELLS_MAX, 314, 11);
	bar_num("STYSNUM", AMMO_ROCKETS, 288, 17);
	bar_num("STYSNUM", AMMO_ROCKETS_MAX, 314, 17);
}

// ------------------------------------------------------------- STCFN text ---

function stcfn_lump(int code) -> int {
	if (code >= 97 && code <= 122) { code = code - 32; }
	if (code < 33 || code > 95) { return -1; }
	return lump_num(`STCFN0${code}`);
}

// whole multiples of a 320-column screen's console font, the same size on screen in either cell shape
function text_scale() -> int {
	int sc = VW / cell_w() / 320;
	if (sc < 1) { sc = 1; }
	return sc * cell_w();
}

// fractional scales render menus at 320-space proportions on any terminal
function text_width(string s, float fs) -> int {
	float w = 0.0;
	for (int i = 0; i < s.size(); ++i) {
		int lu = stcfn_lump(s[i] + 0);
		if (lu < 0) { w = w + 4.0 * fs; continue; }
		sprite_prepare(lu);
		int cw = SPR_W[lu];
		w = w + (cw + 1) * fs;
	}
	return math::itrunc(w);
}

int TEXT_SHADOW = -1;          // a palette color: draw_text lays each glyph over its shadow, a scaled pixel down-right

function draw_text(string s, int x, int y, float fs) -> void {
	float fx = x;
	int sdx = math::itrunc(fs + 0.5);
	int sdy = pixel_rows(fs + 0.5);
	if (sdx < 1) { sdx = 1; }
	if (sdy < 1) { sdy = 1; }
	for (int i = 0; i < s.size(); ++i) {
		int lu = stcfn_lump(s[i] + 0);
		if (lu < 0) { fx = fx + 4.0 * fs; continue; }
		sprite_prepare(lu);
		int cw = SPR_W[lu];
		int chh = SPR_H[lu];
		int pw = math::itrunc(cw * fs);
		int ph = math::itrunc(chh * fs * PIXEL_Y);
		if (pw < 1) { pw = 1; }
		if (ph < 1) { ph = 1; }
		if (TEXT_SHADOW >= 0) { draw_patch_solid(lu, math::itrunc(fx) + sdx, y + sdy, pw, ph, TEXT_SHADOW); }
		draw_patch(lu, math::itrunc(fx), y, pw, ph);
		fx = fx + (cw + 1) * fs;
	}
}

function draw_text_center(string s, int y, float fs) -> void {
	draw_text(s, (VW - text_width(s, fs)) / 2, y, fs);
}

// menus/screens live in 320-space: scale down as well as up
function menu_scale() -> float {
	float ms = VW / 320.0;
	if (ms < 0.5) { ms = 0.5; }
	return ms;
}

function msg_draw() -> void {
	if (MSG_TIMER <= 0) { return; }
	TEXT_SHADOW = rgb_idx(0, 0, 0);
	draw_text(GAME_MSG, 2, 2, text_scale());
	TEXT_SHADOW = -1;
}

// frame-time EMA -> FPS in the top-right corner
float FPS_EMA = 0.0;

function fps_note(float dtms) -> void {
	if (dtms < 1) { dtms = 1; }
	if (FPS_EMA <= 0.0) { FPS_EMA = dtms; }
	else { FPS_EMA = FPS_EMA * 0.9 + dtms * 0.1; }
}

function fps_draw() -> void {
	if (FPS_EMA <= 0.0) { return; }
	int fps = math::itrunc(1000.0 / FPS_EMA + 0.5);
	int sc = text_scale();
	var s = `${fps} FPS`;
	// bottom-right of the view, clear of top-left messages
	draw_text(s, VW - text_width(s, sc) - 2, VIEWH - pixel_rows(9.0 * sc), sc);
}

// big red numbers in screen space, right-aligned ending at rx
function big_num(int val, int rx, int y, float fs) -> void {
	if (val < 0) { val = 0; }
	while (true) {
		int lu = lump_num(`STTNUM${val % 10}`);
		if (lu < 0) { return; }
		sprite_prepare(lu);
		int w = math::itrunc(SPR_W[lu] * fs);
		int h = math::itrunc(SPR_H[lu] * fs * PIXEL_Y);
		if (w < 1) { w = 1; }
		if (h < 1) { h = 1; }
		rx = rx - w;
		draw_patch(lu, rx, y, w, h);
		val = val / 10;
		if (val == 0) { break; }
	}
}

function big_pct(int val, int rx, int y, float fs) -> void {
	int lu = lump_num("STTPRCNT");
	if (lu >= 0) {
		sprite_prepare(lu);
		int w = math::itrunc(SPR_W[lu] * fs);
		int h = math::itrunc(SPR_H[lu] * fs * PIXEL_Y);
		if (w < 1) { w = 1; }
		if (h < 1) { h = 1; }
		draw_patch(lu, rx, y, w, h);
	}
	big_num(val, rx, y, fs);
}

// ----------------------------------------------------------------- screens ---

// TITLEPIC with the fire climbing over it, and PRESS ANY KEY blinking on its shadow
int TITLE_BLINK = 0;          // frames the title has shown

function title_draw() -> void {
	if (!draw_patch_name("TITLEPIC", 0, 0, VW, VH)) { pix_clear(0); }
	fire_draw(FIRE_SHOW);
	TITLE_BLINK += 1;
	if ((TITLE_BLINK / 20) % 4 != 3) {
		float fs = fit_scale("PRESS ANY KEY", text_scale());
		TEXT_SHADOW = rgb_idx(0, 0, 0);
		draw_text_center("PRESS ANY KEY", VH - pixel_rows(12.0 * fs), fs);
		TEXT_SHADOW = -1;
	}
}

// the real menu art where the WAD has it (M_JKILL and friends are ~2x the
// console font — the legible size the game intends); STCFN doubles as the
// fallback for entries without a lump (RESUME)
function menu_lump(string label) -> string {
	if (label == "NEW GAME") { return "M_NGAME"; }
	if (label == "SAVE GAME") { return "M_SAVEG"; }
	if (label == "LOAD GAME") { return "M_LOADG"; }
	if (label == "QUIT") { return "M_QUITG"; }
	if (label == "I'M TOO YOUNG TO DIE") { return "M_JKILL"; }
	if (label == "HEY, NOT TOO ROUGH") { return "M_ROUGH"; }
	if (label == "HURT ME PLENTY") { return "M_HURT"; }
	if (label == "ULTRA-VIOLENCE") { return "M_ULTRA"; }
	if (label == "NIGHTMARE!") { return "M_NMARE"; }
	if (label == "CHOOSE A SKILL") { return "M_SKILL"; }
	return "";
}

// full-screen net lobby / address entry (STCFN text — no M_ art exists here).
// Every row is width-FITTED: the scale shrinks until the line fits the screen
// (floor 0.45 keeps glyphs legible), so narrow terminals wrap nothing and
// clip nothing.
function fit_scale(string s, float want) -> float {
	float ms = want;
	while (ms > 0.45 && text_width(s, ms) > VW - 8) { ms = ms * 0.85; }
	return ms;
}

// centered row at a fitted scale; returns the next row's y
function screen_row(string s, int y, float want) -> int {
	if (s == "") { return y; }
	float ms = fit_scale(s, want);
	draw_text_center(s, y, ms);
	return y + pixel_rows(10.0 * ms) + 4;
}

function lobby_draw(string title, string line1, string line2) -> void {
	pix_clear(0);
	float ms = menu_scale();
	int y = VH / 5;
	y = screen_row(title, y, ms * 2.0) + pixel_rows(6.0 * ms);
	y = screen_row(line1, y, ms * 1.25) + pixel_rows(6.0 * ms);
	screen_row(line2, y, ms);
}

function ip_draw(string addr) -> void {
	pix_clear(0);
	float ms = menu_scale();
	int y = VH / 6;
	y = screen_row("ENTER HOST ADDRESS", y, ms * 2.0) + pixel_rows(8.0 * ms);
	y = screen_row(addr + "_", y, ms * 2.0) + pixel_rows(10.0 * ms);
	y = screen_row("LETTERS DIGITS . : -", y, ms);
	y = screen_row("ENTER JOINS", y, ms);
	screen_row("ESC BACKS OUT", y, ms);
}

function demo_name_draw(string name) -> void {
	pix_clear(0);
	float ms = menu_scale();
	int y = VH / 6;
	y = screen_row("PLAY WHICH DEMO", y, ms * 2.0) + pixel_rows(8.0 * ms);
	y = screen_row(name + "_", y, ms * 2.0) + pixel_rows(10.0 * ms);
	y = screen_row("LETTERS DIGITS . - _", y, ms);
	y = screen_row("ENTER PLAYS", y, ms);
	y = screen_row("ESC BACKS OUT", y, ms);
	if (MSG_TIMER > 0) { screen_row(GAME_MSG, y + pixel_rows(6.0 * ms), ms); }   // "no demo x" lands here
}

// REC / DEMO tape badge, top-right clear of the top-left message row
function tape_draw(string tag) -> void {
	int sc = text_scale();
	draw_text(tag, VW - text_width(tag, sc) - 2, 2, sc);
}

// the 'o' frag overlay: one fitted row per player over the view
function scoreboard_draw(var& lines) -> void {
	float ms = menu_scale();
	int y = 6;
	y = screen_row("FRAGS", y, ms * 1.5) + 2;
	for (int i = 0; i < lines.size(); ++i) {
		var line = lines[i];
		y = screen_row(line, y, ms);
		if (y > VIEWH - 12) { break; }               // 16 players at a tiny size: clip honestly
	}
}
// draws the entry's menu patch (or fallback text) left-aligned at x; -1 x = centered

function menu_entry_draw(string label, int x, int y, float ms) -> int {
	var lname = menu_lump(label);
	int lu = lname == "" ? -1 : lump_num(lname);
	if (lu >= 0) {
		sprite_prepare(lu);
		int w = math::itrunc(SPR_W[lu] * ms);
		int h = math::itrunc(SPR_H[lu] * ms * PIXEL_Y);
		if (w < 1) { w = 1; }
		if (h < 1) { h = 1; }
		if (x < 0) { x = (VW - w) / 2; }
		draw_patch(lu, x, y, w, h);
		return x;
	}
	float fs = fit_scale(label, ms * 2.0);
	if (x < 0) { x = (VW - text_width(label, fs)) / 2; }
	draw_text(label, x, y, fs);
	return x;
}

function menu_draw(var& entries, int sel, string title) -> void {
	backdrop_draw();
	int mlu = title == "" ? lump_num("M_DOOM") : -1;
	float head = 20.0;                               // the heading's height in 320-wide units
	if (mlu >= 0) {
		sprite_prepare(mlu);
		head = SPR_H[mlu] + 10.0;
	}
	// sized by the width, but never taller than the screen below its top tenth: a wide console lost the last entries
	float ms = menu_scale();
	float fit = (VH - VH / 10) / ((head + 19.0 * entries.size()) * PIXEL_Y);
	if (fit < ms) { ms = fit; }
	int y = VH / 10;
	if (title == "") {
		if (mlu >= 0) {
			int w = math::itrunc(SPR_W[mlu] * ms);
			int h = pixel_rows(SPR_H[mlu] * ms);
			if (w > 0 && h > 0) {
				draw_patch(mlu, (VW - w) / 2, y, w, h);
				y = y + h + pixel_rows(10.0 * ms);
			}
		} else {
			draw_text_center("JAIDOOM", y, ms * 2.0);
			y = y + pixel_rows(20.0 * ms);
		}
	} else {
		menu_entry_draw(title, -1, y, ms);
		y = y + pixel_rows(20.0 * ms);
	}
	// entries left-align to the widest so the skull cursor has a lane
	int step = pixel_rows(19.0 * ms);
	for (int i = 0; i < entries.size(); ++i) {
		var line = entries[i];
		int x = menu_entry_draw(line, i == 0 ? -1 : MENU_X, y, ms);
		if (i == 0) { MENU_X = x; }
		if (i == sel) {
			int sk = lump_num("M_SKULL1");
			if (sk >= 0) {
				sprite_prepare(sk);
				int sw = math::itrunc(SPR_W[sk] * ms);
				int sh = math::itrunc(SPR_H[sk] * ms * PIXEL_Y);
				if (sw < 1) { sw = 1; }
				if (sh < 1) { sh = 1; }
				draw_patch(sk, MENU_X - sw - math::itrunc(8.0 * ms), y - pixel_rows(2.0 * ms), sw, sh);
			} else {
				draw_text("*", MENU_X - math::itrunc(12.0 * ms), y, ms * 2.0);
			}
		}
		y = y + step;
	}
}
int MENU_X = 0;

map<string, int> PARS = {"E1M1": 30, "E1M2": 75, "E1M3": 120, "E1M4": 90, "E1M5": 165,
	"E1M6": 180, "E1M7": 180, "E1M8": 30, "E1M9": 165};

function par_secs(string mapname) -> int {
	if (PARS.has(mapname)) { return PARS[mapname]; }
	return 0;
}

function mmss(string label, int secs) -> string {
	int mm = secs / 60;
	int ss = secs % 60;
	if (ss < 10) { return `${label} ${mm}:0${ss}`; }
	return `${label} ${mm}:${ss}`;
}

function wi_draw(string mapname, string nextmap, int kp, int ip, int sp, int tsecs) -> void {
	// doom2 wads ship INTERPIC instead of WIMAP0; neither -> dark background
	if (!draw_patch_name("WIMAP0", 0, 0, VW, VH) && !draw_patch_name("INTERPIC", 0, 0, VW, VH)) { pix_clear(0); }
	int sc = text_scale();
	float fs = VW / 320.0;
	draw_text_center(`${mapname} FINISHED`, VH / 16, sc * 2);
	int lx = VW / 5;
	int rx = VW * 72 / 100;
	int y1 = VH * 30 / 100;
	int dy = VH * 13 / 100;
	draw_text("KILLS", lx, y1, sc);
	big_pct(kp, rx, y1, fs);
	draw_text("ITEMS", lx, y1 + dy, sc);
	big_pct(ip, rx, y1 + dy, fs);
	draw_text("SECRET", lx, y1 + dy * 2, sc);
	big_pct(sp, rx, y1 + dy * 2, fs);
	draw_text(mmss("TIME", tsecs), lx, y1 + dy * 3, sc);
	int par = par_secs(mapname);
	if (par > 0) { draw_text(mmss("PAR", par), VW * 55 / 100, y1 + dy * 3, sc); }
	if (nextmap != "") { draw_text_center(`ENTERING ${nextmap}`, y1 + dy * 4, sc); }
	draw_text_center(WI_FOOT != "" ? WI_FOOT : "PRESS ANY KEY", VH - pixel_rows(10.0 * sc), sc);
}
string WI_FOOT = "";          // netgame footer (WAITING FOR HOST / N/M READY); "" = solo default

function end_draw() -> void {
	pix_clear(0);
	int sc = text_scale();
	int y = VH / 6;
	var title = "EPISODE ONE COMPLETE";
	var lines = ["THE ANOMALY IS SILENT.",
		"WHATEVER OPENED THE GATE",
		"WAITS PAST THE DEIMOS RELAY.",
		"YOUR TOUR OF PHOBOS IS OVER.",
		"",
		"THANKS FOR PLAYING JAIDOOM"];
	if (GAME_MAP.starts_with("MAP")) {
		title = "THE INVASION IS OVER";
		lines = ["THIRTY MAPS DOWN.",
			"THE ICON SPEAKS NO MORE,",
			"AND EARTH IS QUIET AGAIN.",
			"",
			"THANKS FOR PLAYING JAIDOOM"];
	}
	draw_text_center(title, y, sc * 2);
	y = y + pixel_rows(24.0 * sc);
	for (int i = 0; i < lines.size(); ++i) {
		var line = lines[i];
		if (line != "") { draw_text_center(line, y, sc); }
		y = y + pixel_rows(12.0 * sc);
	}
	draw_text_center("PRESS ANY KEY", VH - pixel_rows(10.0 * sc), sc);
}

// ----------------------------------------------------------------- automap ---

int AM_CHEAT = 0;             // iddt: 0 normal, 1 all lines bright, 2 + things
array<array<float>> AM_MARKS = []; // [x, y] map-space pairs, numbered from 1

function am_mark_add() -> void {
	if (AM_MARKS.size() >= 10) { set_msg("no more marks"); return; }
	AM_MARKS.push([P_X, P_Y]);
	set_msg(`mark ${AM_MARKS.size()} dropped`);
}

function am_mark_clear() -> void {
	AM_MARKS = [];
	set_msg("marks cleared");
}

function am_marks_reset() -> void { AM_MARKS = []; }

function am_zoom(float f) -> void {
	AM_ZOOM = AM_ZOOM * f;
	if (AM_ZOOM < 0.5) { AM_ZOOM = 0.5; }
	if (AM_ZOOM > 8.0) { AM_ZOOM = 8.0; }
}

function am_px(int x, int y, int c, int hlim) -> void {
	if (x >= 0 && x < VW && y >= 0 && y < hlim) { PIX[y * VW + x] = c; }
}

// Liang-Barsky clip state for am_line
float AM_T0 = 0.0;
float AM_T1 = 0.0;
function am_clip(float p, float q) -> bool {
	if (p == 0.0) { return q >= 0.0; }
	float r = q / p;
	if (p < 0.0) {
		if (r > AM_T1) { return false; }
		if (r > AM_T0) { AM_T0 = r; }
	} else {
		if (r < AM_T0) { return false; }
		if (r < AM_T1) { AM_T1 = r; }
	}
	return true;
}

function am_line(float x0, float y0, float x1, float y1, int c, int hlim) -> void {
	AM_T0 = 0.0;
	AM_T1 = 1.0;
	float dx = x1 - x0;
	float dy = y1 - y0;
	if (!am_clip(-dx, x0)) { return; }
	if (!am_clip(dx, VW - 1.0 - x0)) { return; }
	if (!am_clip(-dy, y0)) { return; }
	if (!am_clip(dy, hlim - 1.0 - y0)) { return; }
	int ix0 = math::itrunc(x0 + dx * AM_T0);
	int iy0 = math::itrunc(y0 + dy * AM_T0);
	int ix1 = math::itrunc(x0 + dx * AM_T1);
	int iy1 = math::itrunc(y0 + dy * AM_T1);
	int adx = ix1 - ix0; if (adx < 0) { adx = -adx; }
	int ady = iy1 - iy0; if (ady < 0) { ady = -ady; }
	int sx = ix0 < ix1 ? 1 : -1;
	int sy = iy0 < iy1 ? 1 : -1;
	int err = adx - ady;
	while (true) {
		am_px(ix0, iy0, c, hlim);
		if (ix0 == ix1 && iy0 == iy1) { break; }
		int e2 = err * 2;
		if (e2 > -ady) { err = err - ady; ix0 = ix0 + sx; }
		if (e2 < adx) { err = err + adx; iy0 = iy0 + sy; }
	}
}

// full-map overlay in the view region, centered on the player
function automap_draw() -> void {
	int amh = VIEWH;
	var& pix = PIX;
	int n = VW * amh;
	for (int i = 0; i < n; ++i) { pix[i] = 0; }
	float spanx = (MAP_MAXX - MAP_MINX) * 1.0;
	float spany = (MAP_MAXY - MAP_MINY) * 1.0;
	if (spanx < 1.0) { spanx = 1.0; }
	if (spany < 1.0) { spany = 1.0; }
	float sx = (VW - 8.0) / spanx;
	float sy = (amh - 8.0) / (spany * PIXEL_Y);
	float sc = (sx < sy ? sx : sy) * AM_ZOOM;
	float scy = sc * PIXEL_Y;
	float cx = VW / 2.0;
	float cy = amh / 2.0;
	int c1 = rgb_idx(255, 96, 64);                   // one-sided: bright
	int c2 = rgb_idx(120, 120, 130);                 // two-sided: dim
	if (P_ALLMAP || AM_CHEAT > 0) { c2 = rgb_idx(200, 200, 215); }   // computer map / iddt reveal
	int cp = rgb_idx(255, 255, 255);
	int nl = LD_V1.size();
	for (int l = 0; l < nl; ++l) {
		float x0 = cx + (VX_X[LD_V1[l]] - P_X) * sc;
		float y0 = cy - (VX_Y[LD_V1[l]] - P_Y) * scy;
		float x1 = cx + (VX_X[LD_V2[l]] - P_X) * sc;
		float y1 = cy - (VX_Y[LD_V2[l]] - P_Y) * scy;
		int cline = c2;
		if (LD_SIDE1[l] < 0) { cline = c1; }
		am_line(x0, y0, x1, y1, cline, amh);
	}
	if (AM_CHEAT >= 2) {                             // iddt twice: live things as crosses
		int ct = rgb_idx(96, 255, 96);
		int nmo = MO_X.size();
		for (int mo = 0; mo < nmo; ++mo) {
			int st = MO_STATE[mo];
			if (st == 0 || st == 6) { continue; }
			float tx = cx + (MO_X[mo] - P_X) * sc;
			float ty = cy - (MO_Y[mo] - P_Y) * scy;
			am_line(tx - 2.0, ty, tx + 2.0, ty, ct, amh);
			am_line(tx, ty - 2.0 * PIXEL_Y, tx, ty + 2.0 * PIXEL_Y, ct, amh);
		}
	}
	for (int mk = 0; mk < AM_MARKS.size(); ++mk) {
		var pt = AM_MARKS[mk];
		draw_text(to_string(mk + 1), math::itrunc(cx + (pt[0] - P_X) * sc),
			math::itrunc(cy - (pt[1] - P_Y) * scy), 1.0);
	}
	float hx = cx + math::cos(P_ANG) * 7.0;
	float hy = cy - math::sin(P_ANG) * 7.0 * PIXEL_Y;
	am_line(cx - math::cos(P_ANG) * 4.0, cy + math::sin(P_ANG) * 4.0 * PIXEL_Y, hx, hy, cp, amh);
	am_line(hx, hy, hx + math::cos(P_ANG + 2.6) * 4.0, hy - math::sin(P_ANG + 2.6) * 4.0 * PIXEL_Y, cp, amh);
	am_line(hx, hy, hx + math::cos(P_ANG - 2.6) * 4.0, hy - math::sin(P_ANG - 2.6) * 4.0 * PIXEL_Y, cp, amh);
}
