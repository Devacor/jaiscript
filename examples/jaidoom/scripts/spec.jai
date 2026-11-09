// JaiDOOM spec.jai — the playsim specials: tag->sector triggers, Mover class
// instances each owning a lifetime coroutine (floors/plats/timed ceilings;
// doors stay on game.jai's Door machinery), walkover crossing detection,
// teleports, stairs, donut, lights, damage floors, secrets, and the E1M8
// tag-666 boss drop. game.jai globals (P_X, door_use, sector_at, ...)
// resolve at call time — importing game.jai here would cycle.

import "sim_random.jai";
import "wad.jai";
import "map.jai";

// netplay.jai hook behind NET_GAME: extern-declared so programs that never import netplay.jai still check
extern function np_cross_send;

int SECRETS_FOUND = 0;
int SECRET_TOTAL = 0;
int BOSS_PEND = 0;            // living tag-666 barons on E1M8, 0 = fired/none
int SPEC_TIC0 = 0;
int CROSS_SIDE = 0;           // side the last crossing came from (0 = front)
array<int> CLOSE30_SECS = [];
array<int> OPEN300_SECS = [];

// active sector movers: each Mover instance owns its lifetime coroutine
// (mover_run); MV_SEC mirrors the busy sectors flat because the busy scans
// and the P6 gate read it
array<int> MV_SEC = [];
var MOVERS = [];

class Mover {
	int sec = 0;
	int plane = 0;                // descriptor kept save-visible; a reloaded mover
	int target = 0;               // re-runs its lifetime from the restored height
	int speed = 0;
	int wait = 0;
	int ret = 0;
	var co = null;
	Mover(int s, int pl, int t, int sp, int w, int r) {
		sec = s;
		plane = pl; target = t; speed = sp; wait = w; ret = r;
		co = mover_run(s, pl, t, sp, w, r);
	}
	bool tic() {
		co.next();
		return co.done();
	}
}

// implemented / whitelisted-cosmetic tables (single source for the P6 gate)
var SPEC_IMPL = {};
var SPEC_COSMETIC = {};
var SECSPEC_IMPL = {};
var SECSPEC_COSMETIC = {};
bool TABLES_BUILT = false;

function spec_tables() -> void {
	if (TABLES_BUILT) { return; }
	TABLES_BUILT = true;
	array<int> li = [1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 16, 18, 20, 21, 22, 23,
		26, 27, 28, 29, 31, 32, 33, 34, 35, 36, 38, 39, 42, 46, 51, 52, 61, 62, 63,
		70, 76, 79, 80, 81, 82, 86, 88, 90, 91, 97, 98, 102, 103, 119, 121];
	for (int i = 0; i < li.size(); ++i) { SPEC_IMPL[li[i]] = 1; }
	SPEC_COSMETIC[48] = 1;                           // scrolling wall
	array<int> si = [1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17];
	for (int j = 0; j < si.size(); ++j) { SECSPEC_IMPL[si[j]] = 1; }
}

function spec_line_known(int s) -> bool { return SPEC_IMPL.has(s) || SPEC_COSMETIC.has(s); }
function spec_sector_known(int s) -> bool { return SECSPEC_IMPL.has(s) || SECSPEC_COSMETIC.has(s); }

function spec_init() -> void {
	spec_tables();
	MV_SEC = []; MOVERS = [];
	SECRETS_FOUND = 0;
	SECRET_TOTAL = 0;
	CLOSE30_SECS = [];
	OPEN300_SECS = [];
	LT_SEC = []; LT_KIND = []; LT_MAX = []; LT_MIN = []; LT_CNT = []; LT_AUX = [];
	SPEC_TIC0 = GAME_TIC;
	int nsec = SE_SPECIAL.size();
	for (int s = 0; s < nsec; ++s) {
		int sp = SE_SPECIAL[s];
		if (sp == 9) { SECRET_TOTAL = SECRET_TOTAL + 1; }
		else if (sp == 10) { CLOSE30_SECS.push(s); }
		else if (sp == 14) { OPEN300_SECS.push(s); }
		else if (sp == 1 || sp == 2 || sp == 3 || sp == 8 || sp == 12 || sp == 13 || sp == 17) { light_spawn(s, sp); }
	}
	BOSS_PEND = 0;
	if (GAME_MAP == "E1M8") {
		int nm = MO_TYPE.size();
		for (int m = 0; m < nm; ++m) {
			if (MO_TYPE[m] == 3003) { BOSS_PEND = BOSS_PEND + 1; }
		}
	}
}

// -------------------------------------------------------- neighbor queries ---

// mode 0: lowest incl own; 1: highest neighbor (-500 if none); 2: next higher
function neighbor_floor(int sec, int mode) -> int {
	int own = SE_FLOORH[sec];
	int best = own;
	if (mode == 1) { best = -500; }
	if (mode == 2) { best = 32767; }
	int nlines = LD_V1.size();
	for (int i = 0; i < nlines; ++i) {
		int s0 = LD_SIDE0[i];
		int s1 = LD_SIDE1[i];
		if (s0 < 0 || s1 < 0) { continue; }
		int a = SD_SEC[s0];
		int b = SD_SEC[s1];
		int other = -1;
		if (a == sec) { other = b; }
		else if (b == sec) { other = a; }
		if (other < 0 || other == sec) { continue; }
		int f = SE_FLOORH[other];
		if (mode == 0) { if (f < best) { best = f; } }
		else if (mode == 1) { if (f > best) { best = f; } }
		else { if (f > own && f < best) { best = f; } }
	}
	if (mode == 2 && best == 32767) { best = own; }
	return best;
}

function neighbor_light(int sec) -> int {
	int best = SE_LIGHT[sec];
	int nlines = LD_V1.size();
	for (int i = 0; i < nlines; ++i) {
		int s0 = LD_SIDE0[i];
		int s1 = LD_SIDE1[i];
		if (s0 < 0 || s1 < 0) { continue; }
		int a = SD_SEC[s0];
		int b = SD_SEC[s1];
		int other = -1;
		if (a == sec) { other = b; }
		else if (b == sec) { other = a; }
		if (other < 0 || other == sec) { continue; }
		if (SE_LIGHT[other] > best) { best = SE_LIGHT[other]; }
	}
	return best;
}

function neighbor_light_min(int sec) -> int {
	int best = SE_LIGHT[sec];
	int nlines = LD_V1.size();
	for (int i = 0; i < nlines; ++i) {
		int s0 = LD_SIDE0[i];
		int s1 = LD_SIDE1[i];
		if (s0 < 0 || s1 < 0) { continue; }
		int a = SD_SEC[s0];
		int b = SD_SEC[s1];
		int other = -1;
		if (a == sec) { other = b; }
		else if (b == sec) { other = a; }
		if (other < 0 || other == sec) { continue; }
		if (SE_LIGHT[other] < best) { best = SE_LIGHT[other]; }
	}
	return best;
}

// ------------------------------------------------------------ light effects ---
// sector specials 1 blink / 2 fast strobe / 3 slow strobe / 8 glow /
// 12 slow sync strobe / 13 fast sync strobe / 17 fire flicker, precomputed
// flat at spec_init (base + min-neighbor light); lights_tic writes SE_LIGHT
// and the renderer reads it live

array<int> LT_SEC = [];
array<int> LT_KIND = [];      // 1 blink, 2 strobe, 3 glow, 4 flicker
array<int> LT_MAX = [];
array<int> LT_MIN = [];
array<int> LT_CNT = [];
array<int> LT_AUX = [];       // strobe: dark tics; glow: ramp direction

function light_spawn(int s, int sp) -> void {
	int base = SE_LIGHT[s];
	int mn = neighbor_light_min(s);
	if (sp == 1) {
		LT_SEC.push(s); LT_KIND.push(1); LT_MAX.push(base); LT_MIN.push(mn);
		LT_CNT.push(SIM_RANDOM.between(1, 64)); LT_AUX.push(0);
	} else if (sp == 8) {
		LT_SEC.push(s); LT_KIND.push(3); LT_MAX.push(base); LT_MIN.push(mn);
		LT_CNT.push(0); LT_AUX.push(-1);
	} else if (sp == 17) {
		LT_SEC.push(s); LT_KIND.push(4); LT_MAX.push(base); LT_MIN.push(mn + 16);
		LT_CNT.push(4); LT_AUX.push(0);
	} else {
		if (mn == base) { mn = 0; }                  // an isolated strobe still strobes
		LT_SEC.push(s); LT_KIND.push(2); LT_MAX.push(base); LT_MIN.push(mn);
		LT_CNT.push(sp == 12 || sp == 13 ? 1 : SIM_RANDOM.between(1, 8));
		LT_AUX.push(sp == 2 || sp == 13 ? 15 : 35);  // fast/slow dark time
	}
}

function lights_tic() -> void {
	int n = LT_SEC.size();
	for (int i = 0; i < n; ++i) {
		int s = LT_SEC[i];
		int kind = LT_KIND[i];
		if (kind == 3) {                             // glow: 8/tic ramp, bounce at the rails
			int l = SE_LIGHT[s];
			if (LT_AUX[i] < 0) {
				l = l - 8;
				if (l <= LT_MIN[i]) { l = LT_MIN[i]; LT_AUX[i] = 1; }
			} else {
				l = l + 8;
				if (l >= LT_MAX[i]) { l = LT_MAX[i]; LT_AUX[i] = -1; }
			}
			SE_LIGHT[s] = l;
			continue;
		}
		LT_CNT[i] = LT_CNT[i] - 1;
		if (LT_CNT[i] > 0) { continue; }
		if (kind == 1) {                             // blink: long bright, short random drops
			if (SE_LIGHT[s] == LT_MAX[i]) { SE_LIGHT[s] = LT_MIN[i]; LT_CNT[i] = SIM_RANDOM.between(1, 8); }
			else { SE_LIGHT[s] = LT_MAX[i]; LT_CNT[i] = SIM_RANDOM.between(1, 64); }
		} else if (kind == 2) {                      // strobe: 5 bright / darktime dark
			if (SE_LIGHT[s] == LT_MIN[i]) { SE_LIGHT[s] = LT_MAX[i]; LT_CNT[i] = 5; }
			else { SE_LIGHT[s] = LT_MIN[i]; LT_CNT[i] = LT_AUX[i]; }
		} else {                                     // flicker: fire jitter every 4 tics
			int l2 = LT_MAX[i] - SIM_RANDOM.between(0, 3) * 16;
			if (l2 < LT_MIN[i]) { l2 = LT_MIN[i]; }
			SE_LIGHT[s] = l2;
			LT_CNT[i] = 4;
		}
	}
}

// ----------------------------------------------------------------- movers ---

function mover_busy(int sec) -> bool {
	int n = MV_SEC.size();
	for (int i = 0; i < n; ++i) {
		if (MV_SEC[i] == sec) { return true; }
	}
	return false;
}

// one machine per sector, doom's sector->specialdata rule
function sector_busy(int sec) -> bool {
	return mover_busy(sec) || door_busy(sec);
}

function mover_start(int sec, int plane, int target, int speed, int wait, int ret) -> bool {
	if (sector_busy(sec)) { return false; }
	MV_SEC.push(sec);
	MOVERS.push(new Mover(sec, plane, target, speed, wait, ret));
	return true;
}

function step_toward(int cur, int target, int speed) -> int {
	if (cur < target) {
		cur = cur + speed;
		if (cur > target) { cur = target; }
	} else if (cur > target) {
		cur = cur - speed;
		if (cur < target) { cur = target; }
	}
	return cur;
}

// a whole mover lifetime, one yield per tic; returning (not yielding) on the
// landing step keeps the old same-tic list removal
coroutine void mover_run(int sec, int plane, int target, int speed, int wait, int ret) {
	int cur = plane == 0 ? SE_FLOORH[sec] : SE_CEILH[sec];
	while (cur != target) {
		cur = step_toward(cur, target, speed);
		if (plane == 0) { SE_FLOORH[sec] = cur; } else { SE_CEILH[sec] = cur; }
		if (cur == target && wait < 0) { return; }   // one-way: done the tic it lands
		yield;
	}
	if (wait < 0) { return; }
	for (int t = 0; t < wait; ++t) { yield; }
	while (cur != ret) {
		cur = step_toward(cur, ret, speed);
		if (plane == 0) { SE_FLOORH[sec] = cur; } else { SE_CEILH[sec] = cur; }
		if (cur == ret) { return; }
		yield;
	}
}

function movers_tic() -> void {
	int i = 0;
	while (i < MOVERS.size()) {
		var mv = MOVERS[i];
		if (mv.tic()) { MV_SEC.remove(i); MOVERS.remove(i); }
		else { i = i + 1; }
	}
}

// ---------------------------------------------------------------- actions ---

// kind: 0 open-wait-close, 1 open-stay, 2 close-stay, 3 close-wait-30s-open
function tag_doors(int tag, int kind) -> bool {
	bool any = false;
	int nsec = SE_TAG.size();
	for (int s = 0; s < nsec; ++s) {
		if (SE_TAG[s] != tag) { continue; }
		if (kind == 0) { door_use(s, false); }
		else if (kind == 1) { door_use(s, true); }
		else if (kind == 2) { door_close(s); }
		else { mover_start(s, 1, SE_FLOORH[s], 2, 1050, SE_CEILH[s]); }
		any = true;
	}
	return any;
}

function tag_plats(int tag) -> bool {
	bool any = false;
	int nsec = SE_TAG.size();
	for (int s = 0; s < nsec; ++s) {
		if (SE_TAG[s] != tag) { continue; }
		int low = neighbor_floor(s, 0);
		if (mover_start(s, 0, low, 4, 105, SE_FLOORH[s])) { any = true; }
	}
	return any;
}

function adopt_floorpic(int s, int from) -> void {
	SE_FLOORPIC[s] = SE_FLOORPIC[from];
	if (s < SEC_FFLAT.size() && from < SEC_FFLAT.size()) { SEC_FFLAT[s] = SEC_FFLAT[from]; }
}

// mode: 0 lower-to-lowest, 1 lower-to-highest, 2 raise-to-next-higher,
// 3 turbo-lower (8 above highest), 4 raise-to-lowest-ceiling
function tag_floor(int tag, int mode, int speed, int texline) -> bool {
	bool any = false;
	int nsec = SE_TAG.size();
	for (int s = 0; s < nsec; ++s) {
		if (SE_TAG[s] != tag) { continue; }
		if (sector_busy(s)) { continue; }
		int cur = SE_FLOORH[s];
		int t = cur;
		if (mode == 0) { t = neighbor_floor(s, 0); }
		else if (mode == 1) {
			t = neighbor_floor(s, 1);
			if (t == -500 || t > cur) { t = cur; }
		} else if (mode == 2) { t = neighbor_floor(s, 2); }
		else if (mode == 3) {
			t = neighbor_floor(s, 1);
			if (t == -500) { t = cur; }
			if (t != cur) { t = t + 8; }
			if (t > cur) { t = cur; }
		} else {
			t = lowest_neighbor_ceiling(s);
			if (t > SE_CEILH[s]) { t = SE_CEILH[s]; }
			if (t < cur) { t = cur; }
		}
		if (texline >= 0) {                          // raise-and-change: doom's new floor drops the old one's damage too
			int fs = LD_SIDE0[texline];
			if (fs >= 0) { adopt_floorpic(s, SD_SEC[fs]); }
			SE_SPECIAL[s] = 0;
		}
		mover_start(s, 0, t, speed, -1, 0);
		any = true;
	}
	return any;
}

function tag_lights(int tag, int level) -> bool {
	bool any = false;
	int nsec = SE_TAG.size();
	for (int s = 0; s < nsec; ++s) {
		if (SE_TAG[s] != tag) { continue; }
		if (level < 0) { SE_LIGHT[s] = neighbor_light(s); }
		else { SE_LIGHT[s] = level; }
		any = true;
	}
	return any;
}

function teleport_player(int tag) -> bool {
	if (CROSS_SIDE != 0) { return false; }           // only crossing from the front
	int nth = TH_TYPE.size();
	for (int t = 0; t < nth; ++t) {
		if (TH_TYPE[t] != 14) { continue; }
		int ds = sector_at(TH_X[t] * 1.0, TH_Y[t] * 1.0);
		if (SE_TAG[ds] != tag) { continue; }
		P_X = TH_X[t] * 1.0;
		P_Y = TH_Y[t] * 1.0;
		P_ANG = TH_ANG[t] * math::PI / 180.0;
		P_MOMX = 0.0;
		P_MOMY = 0.0;
		P_FLOORZ = SE_FLOORH[ds];
		P_Z = P_FLOORZ * 1.0;                        // teleports snap, never arc
		P_MOMZ = 0.0;
		P_AIR = false;
		P_VIEWZ = P_FLOORZ + 41.0;                   // no easing across a teleport
		return true;
	}
	return false;
}

// each queued step becomes busy, so the texture-chain walk cannot cycle
function build_stairs(int tag) -> bool {
	bool any = false;
	int nsec = SE_TAG.size();
	int nlines = LD_V1.size();
	for (int s = 0; s < nsec; ++s) {
		if (SE_TAG[s] != tag) { continue; }
		if (sector_busy(s)) { continue; }
		var tex = SE_FLOORPIC[s];
		int h = SE_FLOORH[s] + 8;
		mover_start(s, 0, h, 1, -1, 0);
		any = true;
		int cur = s;
		bool found = true;
		while (found) {
			found = false;
			for (int i = 0; i < nlines; ++i) {
				if (LD_SIDE1[i] < 0) { continue; }
				int front = SD_SEC[LD_SIDE0[i]];
				if (front != cur) { continue; }
				int back = SD_SEC[LD_SIDE1[i]];
				if (SE_FLOORPIC[back] != tex) { continue; }
				if (sector_busy(back)) { continue; }
				h = h + 8;
				mover_start(back, 0, h, 1, -1, 0);
				cur = back;
				found = true;
				break;
			}
		}
	}
	return any;
}

// tagged pool lowers, its ring rises to the far side's height and turns safe
function donut(int tag) -> bool {
	bool any = false;
	int nsec = SE_TAG.size();
	int nlines = LD_V1.size();
	for (int s = 0; s < nsec; ++s) {
		if (SE_TAG[s] != tag) { continue; }
		if (sector_busy(s)) { continue; }
		int ring = -1;
		for (int i = 0; i < nlines; ++i) {
			if (LD_SIDE1[i] < 0) { continue; }
			int a = SD_SEC[LD_SIDE0[i]];
			int b = SD_SEC[LD_SIDE1[i]];
			if (a == s && b != s) { ring = b; break; }
			if (b == s && a != s) { ring = a; break; }
		}
		if (ring < 0 || sector_busy(ring)) { continue; }
		int outer = -1;
		for (int j = 0; j < nlines; ++j) {
			if (LD_SIDE1[j] < 0) { continue; }
			int a2 = SD_SEC[LD_SIDE0[j]];
			int b2 = SD_SEC[LD_SIDE1[j]];
			int other = -1;
			if (a2 == ring && b2 != ring) { other = b2; }
			else if (b2 == ring && a2 != ring) { other = a2; }
			if (other < 0 || other == s) { continue; }
			outer = other;
			break;
		}
		if (outer < 0) { continue; }
		int h = SE_FLOORH[outer];
		adopt_floorpic(ring, outer);
		SE_SPECIAL[ring] = 0;
		mover_start(ring, 0, h, 1, -1, 0);
		mover_start(s, 0, h, 1, -1, 0);
		any = true;
	}
	return any;
}

// ------------------------------------------------------------- dispatch ---

// act: 0 use, 1 cross, 2 gun. Returns true when the special fired; W1/S1
// clear the line only then (doom keeps a busy-sector trigger armed).
function spec_line(int ld, int act) -> bool {
	int sp = LD_SPECIAL[ld];
	int tag = LD_TAG[ld];
	bool ok = false;
	bool once = false;
	if (act == 1) {
		if (sp == 2) { ok = tag_doors(tag, 1); once = true; }
		else if (sp == 3) { ok = tag_doors(tag, 2); once = true; }
		else if (sp == 4) { ok = tag_doors(tag, 0); once = true; }
		else if (sp == 5) { ok = tag_floor(tag, 4, 1, -1); once = true; }
		else if (sp == 8) { ok = build_stairs(tag); once = true; }
		else if (sp == 10 || sp == 121) { ok = tag_plats(tag); once = true; }
		else if (sp == 12) { ok = tag_lights(tag, -1); once = true; }
		else if (sp == 13) { ok = tag_lights(tag, 255); once = true; }
		else if (sp == 16) { ok = tag_doors(tag, 3); once = true; }
		else if (sp == 22) { ok = tag_floor(tag, 2, 1, ld); once = true; }
		else if (sp == 35) { ok = tag_lights(tag, 35); once = true; }
		else if (sp == 36) { ok = tag_floor(tag, 3, 4, -1); once = true; }
		else if (sp == 38) { ok = tag_floor(tag, 0, 1, -1); once = true; }
		else if (sp == 39) { ok = teleport_player(tag); once = true; }
		else if (sp == 52) { LEVEL_DONE = true; ok = true; once = true; }
		else if (sp == 119) { ok = tag_floor(tag, 2, 1, -1); once = true; }
		else if (sp == 76) { ok = tag_doors(tag, 3); }
		else if (sp == 79) { ok = tag_lights(tag, 35); }
		else if (sp == 80) { ok = tag_lights(tag, -1); }
		else if (sp == 81) { ok = tag_lights(tag, 255); }
		else if (sp == 82) { ok = tag_floor(tag, 0, 1, -1); }
		else if (sp == 86) { ok = tag_doors(tag, 1); }
		else if (sp == 88) { ok = tag_plats(tag); }
		else if (sp == 90) { ok = tag_doors(tag, 0); }
		else if (sp == 91) { ok = tag_floor(tag, 4, 1, -1); }
		else if (sp == 97) { ok = teleport_player(tag); }
		else if (sp == 98) { ok = tag_floor(tag, 3, 4, -1); }
	} else if (act == 0) {
		if (sp == 7) { ok = build_stairs(tag); once = true; }
		else if (sp == 9) { ok = donut(tag); once = true; }
		else if (sp == 18) { ok = tag_floor(tag, 2, 1, -1); once = true; }
		else if (sp == 20) { ok = tag_floor(tag, 2, 1, ld); once = true; }
		else if (sp == 21) { ok = tag_plats(tag); once = true; }
		else if (sp == 23) { ok = tag_floor(tag, 0, 1, -1); once = true; }
		else if (sp == 29) { ok = tag_doors(tag, 0); once = true; }
		else if (sp == 102) { ok = tag_floor(tag, 1, 1, -1); once = true; }
		else if (sp == 103) { ok = tag_doors(tag, 1); once = true; }
		else if (sp == 42) { ok = tag_doors(tag, 2); }
		else if (sp == 61) { ok = tag_doors(tag, 1); }
		else if (sp == 62) { ok = tag_plats(tag); }
		else if (sp == 63) { ok = tag_doors(tag, 0); }
		else if (sp == 70) { ok = tag_floor(tag, 3, 4, -1); }
	} else {
		if (sp == 46) { ok = tag_doors(tag, 1); }
	}
	if (ok && once) { LD_SPECIAL[ld] = 0; }
	return ok;
}

// ------------------------------------------------------ walkover crossing ---

function line_side_pt(int ld, float x, float y) -> int {
	float x1 = VX_X[LD_V1[ld]];
	float y1 = VX_Y[LD_V1[ld]];
	float ldx = VX_X[LD_V2[ld]] - x1;
	float ldy = VX_Y[LD_V2[ld]] - y1;
	float cross = ldx * (y - y1) - ldy * (x - x1);
	return cross < 0.0 ? 0 : 1;                      // 0 = front (right of v1->v2)
}

// fire specials on lines the move segment crossed (blockmap cells = candidates)
function cross_check(float ox, float oy, float nx, float ny) -> void {
	float lox = ox < nx ? ox : nx;
	float hix = ox > nx ? ox : nx;
	float loy = oy < ny ? oy : ny;
	float hiy = oy > ny ? oy : ny;
	int bx0 = math::ifloor((lox - BMAP_ORGX) / 128.0);
	int bx1 = math::ifloor((hix - BMAP_ORGX) / 128.0);
	int by0 = math::ifloor((loy - BMAP_ORGY) / 128.0);
	int by1 = math::ifloor((hiy - BMAP_ORGY) / 128.0);
	if (bx0 < 0) { bx0 = 0; }
	if (by0 < 0) { by0 = 0; }
	if (bx1 >= BMAP_W) { bx1 = BMAP_W - 1; }
	if (by1 >= BMAP_H) { by1 = BMAP_H - 1; }
	array<int> seen = [];
	var& bm = BMAP;
	for (int cy = by0; cy <= by1; ++cy) {
		for (int cx = bx0; cx <= bx1; ++cx) {
			int i = bm[4 + cy * BMAP_W + cx] & 65535;
			if (bm[i] == 0) { i = i + 1; }           // leading zero convention
			while (bm[i] != -1) {
				int ld = bm[i] & 65535;
				i = i + 1;
				if (LD_SPECIAL[ld] == 0) { continue; }
				bool dup = false;
				for (int k = 0; k < seen.size(); ++k) {
					if (seen[k] == ld) { dup = true; break; }
				}
				if (dup) { continue; }
				seen.push(ld);
				int oside = line_side_pt(ld, ox, oy);
				if (line_side_pt(ld, nx, ny) == oside) { continue; }
				float x1 = VX_X[LD_V1[ld]];
				float y1 = VX_Y[LD_V1[ld]];
				float x2 = VX_X[LD_V2[ld]];
				float y2 = VX_Y[LD_V2[ld]];
				float d = (x2 - x1) * (ny - oy) - (y2 - y1) * (nx - ox);
				if (d > -0.0001 && d < 0.0001) { continue; }
				float u = ((ox - x1) * (ny - oy) - (oy - y1) * (nx - ox)) / d;
				if (u < 0.0 || u > 1.0) { continue; }
				CROSS_SIDE = oside;
				// co-op client: crossings are host authority (teleport dests
				// come back as a TELE snap; doors/lifts via the SECT stream)
				if (NET_GAME && NP_COOP && !NP_IS_HOST) { np_cross_send(ld, oside); }
				else { spec_line(ld, 1); }
			}
		}
	}
}

// -------------------------------------------------------------- per tic ---

function boss_check() -> void {
	int nm = MO_TYPE.size();
	for (int m = 0; m < nm; ++m) {
		if (MO_TYPE[m] != 3003) { continue; }
		int st = MO_STATE[m];
		if (st == 1 || st == 2 || st == 3 || st == 4) { return; }
	}
	BOSS_PEND = 0;
	int nsec = SE_TAG.size();
	for (int s = 0; s < nsec; ++s) {
		if (SE_TAG[s] != 666) { continue; }
		int t = neighbor_floor(s, 0);
		mover_start(s, 0, t, 1, -1, 0);
	}
	set_msg("the walls fall");
}

function spec_tic() -> void {
	// co-op client: movers/timers/boss are host authority (SECT stream applies
	// the heights); secrets, own floor damage, and light shows stay local
	bool authority = !(NET_GAME && NP_COOP && !NP_IS_HOST);
	if (authority) {
		movers_tic();
		int dt = GAME_TIC - SPEC_TIC0;
		if (dt == 1050) {
			for (int c = 0; c < CLOSE30_SECS.size(); ++c) { door_close(CLOSE30_SECS[c]); }
		}
		if (dt == 10500) {
			for (int o = 0; o < OPEN300_SECS.size(); ++o) { door_use(OPEN300_SECS[o], false); }
		}
	}
	if (P_HP > 0) {
		int ps = sector_at(P_X, P_Y);
		int sp = SE_SPECIAL[ps];
		if (sp == 9) {
			SECRETS_FOUND = SECRETS_FOUND + 1;
			SE_SPECIAL[ps] = 0;
			set_msg("a secret is revealed!");
		} else if (sp != 0 && (GAME_TIC & 31) == 0 && P_SUIT <= 0) {   // radsuit blocks floor damage
			if (sp == 7) { player_damage(5); }
			else if (sp == 5) { player_damage(10); }
			else if (sp == 4 || sp == 16) { player_damage(20); }
			else if (sp == 11) {
				player_damage(20);
				if (P_HP <= 10) { LEVEL_DONE = true; }   // E1M8's exit
			}
		}
	}
	if (authority && BOSS_PEND > 0) { boss_check(); }
	lights_tic();
}
