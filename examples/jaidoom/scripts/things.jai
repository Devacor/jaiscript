// JaiDOOM things.jai — sprites, the mobj pool, monster AI, combat.
// Behavior data is our own compact table; frame cycles approximate the
// classic state machines (walk/attack/pain/death letter runs). Each live
// monster is driven by a per-mobj brain coroutine (monster_brain) whose
// frame holds the behavior phase; MO_STATE stays the observable state enum.

import "sim_random.jai";
import "wad.jai";
import "map.jai";
import "vid.jai";
import "tex.jai";
import "sound.jai";

// netplay.jai hooks behind NET_GAME: extern-declared so programs that never import netplay.jai still check
extern function np_ghost_hit_mobj;
extern function np_drop_announce;
extern function np_boom_announce;
extern function np_proj_announce;
extern function np_expl_announce;
extern array<float> NP_TGT_X;
extern array<float> NP_TGT_Y;
extern array<int> NP_TGT_MOBJ;

// sprite lump tables: key "XXXXFR" (4-char name + frame letter + rotation 0-8)
map<string, int> SPR_LUMP = {};
map<string, int> SPR_FLIP = {};
map<int, array<int>> SPR_PIX = {}; // lump -> array<int> w*h column-major, -1 transparent
map<int, int> SPR_W = {};
map<int, int> SPR_H = {};
map<int, int> SPR_LO = {};    // left offset
map<int, int> SPR_TO = {};    // top offset

function sprites_init() -> void {
	SPR_LUMP = {};
	SPR_FLIP = {};
	SPR_PIX = {};
	SPR_W = {}; SPR_H = {}; SPR_LO = {}; SPR_TO = {};
	int s0 = lump_num("S_START");
	int s1 = lump_num("S_END");
	if (s0 < 0 || s1 < 0) { return; }
	for (int i = s0 + 1; i < s1; ++i) {
		string nm = LUMP_NAME[i];
		if (nm.size() < 6) { continue; }
		SPR_LUMP[nm.substr(0, 6)] = i;
		SPR_FLIP[nm.substr(0, 6)] = 0;
		if (nm.size() == 8) {
			string mirror = nm.substr(0, 4) + nm.substr(6, 2);
			SPR_LUMP[mirror] = i;
			SPR_FLIP[mirror] = 1;
		}
	}
}

function sprite_prepare(int lump) -> void {
	if (SPR_PIX.has(lump)) { return; }
	int pofs = LUMP_POS[lump];
	binary::reader r = lump_reader(lump);
	int w = r.int16();
	int h = r.int16();
	SPR_W[lump] = w;
	SPR_H[lump] = h;
	SPR_LO[lump] = r.int16();
	SPR_TO[lump] = r.int16();
	array<int> pix = [];
	int n = w * h;
	for (int i = 0; i < n; ++i) { pix.push(-1); }
	var& wd = WAD;
	for (int col = 0; col < w; ++col) {
		int colbase = col * h;
		int post = pofs + r.uint32();
		while (wd[post] != 255) {
			int top = wd[post] + 0;
			int len = wd[post + 1] + 0;
			int src = post + 3;
			for (int k = 0; k < len; ++k) {
				int dy = top + k;
				if (dy >= 0 && dy < h) { pix[colbase + dy] = wd[src + k] + 0; }
			}
			post = post + len + 4;
		}
	}
	SPR_PIX[lump] = pix;
}

// resolve sprite+frame+viewangle to a lump; SPR_LAST_FLIP reports mirroring
int SPR_LAST_FLIP = 0;
function sprite_lookup(string spr4, string frame, int rot) -> int {
	string key = spr4 + frame + to_string(rot + 1);
	if (SPR_LUMP.has(key)) { SPR_LAST_FLIP = SPR_FLIP[key]; return SPR_LUMP[key]; }
	key = spr4 + frame + "0";
	if (SPR_LUMP.has(key)) { SPR_LAST_FLIP = SPR_FLIP[key]; return SPR_LUMP[key]; }
	return -1;
}

// ------------------------------------------------------------ monster data ---
// type -> [spr, hp, per-tic speed, radius, height, painchance, walk, attack,
//          pain, death, attack kind (0 hitscan 1 melee 2 missile 3 melee+missile),
//          damage base, range]
var MINFO = {};
var MISSILE_SPR = {};         // monster type -> probed projectile sprite (BAL1 default)
string TFOG_ANIM = "";        // teleport-fog letters probed from the WAD ("" = no art)

function anim_have(string spr, string anim) -> bool {
	if (anim.size() == 0) { return false; }
	for (int i = 0; i < anim.size(); ++i) {
		if (sprite_lookup(spr, anim.substr(i, 1), 0) < 0) { return false; }
	}
	return true;
}

// doom2 types admit themselves only when the WAD ships every anim letter
function minfo_add(int ty, var mi) -> void {
	if (!anim_have(mi[0], "" + mi[6] + mi[7] + mi[8] + mi[9])) { return; }
	MINFO[ty] = mi;
}

function things_data_init() -> void {
	MINFO = {
		3004: ["POSS", 20, 2.0, 20.0, 56.0, 200, "ABCD", "EF", "G", "HIJKL", 0, 5, 1024.0],
		9:    ["SPOS", 30, 2.0, 20.0, 56.0, 170, "ABCD", "EF", "G", "HIJKL", 0, 12, 1024.0],
		3001: ["TROO", 60, 2.0, 20.0, 56.0, 200, "ABCD", "EFG", "H", "IJKLM", 3, 8, 1024.0],
		3002: ["SARG", 150, 2.5, 30.0, 56.0, 180, "ABCD", "EFG", "H", "IJKLMN", 1, 15, 128.0],
		58:   ["SARG", 150, 2.5, 30.0, 56.0, 180, "ABCD", "EFG", "H", "IJKLMN", 1, 15, 128.0],
		3006: ["SKUL", 100, 3.0, 16.0, 56.0, 256, "AB", "CD", "E", "FGHIJK", 1, 8, 192.0],
		3003: ["BOSS", 1000, 2.0, 24.0, 64.0, 50, "ABCD", "EFG", "H", "IJKLMNO", 3, 25, 1024.0]
	};
	minfo_add(3005, ["HEAD", 400, 2.0, 31.0, 56.0, 128, "A", "BCD", "E", "GHIJKL", 3, 10, 1024.0]);
	minfo_add(69, ["BOS2", 500, 2.0, 24.0, 64.0, 50, "ABCD", "EFG", "H", "IJKLMNO", 3, 25, 1024.0]);
	minfo_add(84, ["SSWV", 50, 2.0, 20.0, 56.0, 170, "ABCD", "EFG", "H", "IJKLM", 0, 12, 1024.0]);
	minfo_add(65, ["CPOS", 70, 2.0, 20.0, 56.0, 170, "ABCD", "EF", "G", "HIJKLMN", 0, 12, 1024.0]);
	MISSILE_SPR = {};
	if (anim_have("BAL2", "ABCDE")) { MISSILE_SPR[3005] = "BAL2"; }
	if (anim_have("BAL7", "ABCDE")) { MISSILE_SPR[69] = "BAL7"; }
	var tf = "";
	int tc = 65;
	while (tc <= 90) {
		var tl = `${to_char(tc)}`;
		if (sprite_lookup("TFOG", tl, 0) < 0) { break; }
		tf += tl;
		tc = tc + 1;
	}
	TFOG_ANIM = tf.size() >= 4 ? tf : "";
	PICKUPS = {
		2011: "STIM", 2012: "MEDI", 2014: "BON1", 2015: "BON2",
		2018: "ARM1", 2019: "ARM2",
		2007: "CLIP", 2048: "AMMO", 2008: "SHEL", 2049: "SBOX",
		2001: "SHOT", 2002: "MGUN", 2003: "LAUN",
		2010: "ROCK", 2046: "BROK",
		5: "BKEY", 13: "RKEY", 6: "YKEY",
		2013: "SOUL",
		2023: "PSTR", 2024: "PINS", 2025: "SUIT", 2026: "PMAP", 2045: "PVIS", 8: "BPAK"
	};
	xdeath_probe();
	decor_data_init();
}
var PICKUPS = {};

// map decorations: type -> [sprite4, anim letters, radius]; radius > 0 =
// obstacle (columns, lamps), 0 = walk-over gore. Solidity rides MO_RAD so it
// survives savegames without a new pool array.
var DECOR = {};

// art-gated like doom2 monsters: absent sprites never spawn. extend probes
// the WAD for letters past the base (animated decor takes its whole run).
function decor_add(int ty, string spr, string base, float rad, bool extend) -> void {
	if (!anim_have(spr, base)) { return; }
	var anim = base;
	if (extend) {
		int c = base[base.size() - 1] + 1;
		while (c <= 90) {
			var letter = `${to_char(c)}`;
			if (sprite_lookup(spr, letter, 0) < 0) { break; }
			anim += letter;
			c = c + 1;
		}
	}
	DECOR[ty] = [spr, anim, rad];
}

function decor_data_init() -> void {
	DECOR = {};
	decor_add(48, "ELEC", "A", 16.0, true);          // tall techno column
	decor_add(2028, "COLU", "A", 16.0, true);        // floor lamp
	decor_add(35, "CBRA", "A", 16.0, true);          // candelabra
	decor_add(30, "COL1", "A", 16.0, true);          // tall green pillar
	decor_add(10, "PLAY", "W", 0.0, false);          // bloody mess (xdeath final)
	decor_add(12, "PLAY", "W", 0.0, false);
	decor_add(15, "PLAY", "N", 0.0, false);          // dead marine
	decor_add(24, "POL5", "A", 0.0, false);          // pool of blood and bones
}

// gib art: letters past the normal death run, probed against the WAD so types
// without xdeath frames (SARG, SKUL, BOSS in ep1) keep their normal death
var XDEATH = {};
function xdeath_probe() -> void {
	XDEATH = {};
	for (var kv : MINFO) {
		var mi = kv.second;
		var spr = mi[0];
		var danim = mi[9];
		int c = danim[danim.size() - 1] + 1;
		var xd = "";
		while (c <= 90) {
			var letter = `${to_char(c)}`;
			if (sprite_lookup(spr, letter, 0) < 0) { break; }
			xd += letter;
			c = c + 1;
		}
		if (xd.size() >= 3) { XDEATH[kv.first] = xd; }
	}
}

// ------------------------------------------------------------------ mobjs ---
// flat pool; STATE: 0 free, 1 sleeping, 2 chasing, 3 attacking, 4 pain,
// 5 dying, 6 dead, 7 pickup, 8 projectile, 9 barrel, 10 decoration,
// 11 net ghost (the remote player), 12 net puppet (co-op client's monsters,
// driven by the host's MONS stream — no AI, shared pool index with the host)
array<float> MO_X = [];
array<float> MO_Y = [];
array<float> MO_Z = [];
array<float> MO_ANG = [];
array<float> MO_MOMX = [];
array<float> MO_MOMY = [];
array<float> MO_MOMZ = [];
array<float> MO_RAD = [];
array<int> MO_TYPE = [];
array<int> MO_STATE = [];
array<int> MO_HP = [];
array<int> MO_FRAME = [];     // index into current anim string
array<int> MO_TIC = [];       // countdown to next frame / next decision
array<int> MO_COOL = [];      // attack cooldown
array<int> MO_TARGET = [];    // monsters: pursued mobj (-1 = player); projectiles: shooter
array<int> MO_CHARGE = [];    // lost soul: 1 while charging
array<int> MO_CNT = [];       // 1 = map-placed pickup, counts toward the items tally
array<int> MO_SHP = [];       // hp at spawn (the gib overkill threshold)
array<int> MO_RESP = [];      // nightmare: corpse respawn countdown (0 = stays down)
array<int> MO_NODMG = [];     // 1 = cosmetic projectile (co-op client rockets: host owns the damage)
array<string> MO_ANIM = [];   // current anim letters (string per mobj)
array<string> MO_SPR = [];    // 4-char sprite name per mobj
var MO_BRAIN = [];            // monster brain coroutine handle per mobj (null when interrupted)
int KILLS = 0;
int KILL_TOTAL = 0;
int ITEMS_TAKEN = 0;
int ITEM_TOTAL = 0;
int SKILL_BIT = 2;            // THINGS spawn filter: 1 easy, 2 medium, 4 hard
bool NIGHTMARE = false;       // skill 5: corpses respawn after ~30s
bool FAST_MONSTERS = false;   // skill 5: speed x1.5 + halved attack cooldowns (use sites; MINFO untouched)

function mobj_spawn(float x, float y, int type, float ang, int state, string spr, string anim, int hp, float rad) -> int {
	MO_X.push(x); MO_Y.push(y);
	MO_Z.push(SE_FLOORH[sector_at(x, y)] * 1.0);
	MO_ANG.push(ang);
	MO_MOMX.push(0.0); MO_MOMY.push(0.0); MO_MOMZ.push(0.0);
	MO_RAD.push(rad);
	MO_TYPE.push(type);
	MO_STATE.push(state);
	MO_HP.push(hp);
	MO_FRAME.push(0);
	MO_TIC.push(4 + (SIM_RANDOM.between(0, 6)));
	MO_COOL.push(0);
	MO_TARGET.push(-1);
	MO_CHARGE.push(0);
	MO_CNT.push(0);
	MO_SHP.push(hp);
	MO_RESP.push(0);
	MO_NODMG.push(0);
	MO_ANIM.push(anim);
	MO_SPR.push(spr);
	MO_BRAIN.push(null);
	return MO_X.size() - 1;
}

function things_spawn_map() -> void {
	MO_X = []; MO_Y = []; MO_Z = []; MO_ANG = []; MO_MOMX = []; MO_MOMY = []; MO_MOMZ = [];
	MO_RAD = []; MO_TYPE = []; MO_STATE = []; MO_HP = []; MO_FRAME = [];
	MO_TIC = []; MO_COOL = []; MO_TARGET = []; MO_CHARGE = []; MO_CNT = []; MO_SHP = []; MO_RESP = []; MO_NODMG = []; MO_ANIM = []; MO_SPR = [];
	MO_BRAIN = [];
	KILLS = 0;
	KILL_TOTAL = 0;
	ITEMS_TAKEN = 0;
	ITEM_TOTAL = 0;
	int n = TH_X.size();
	for (int t = 0; t < n; ++t) {
		int ty = TH_TYPE[t];
		int fl = TH_FLAGS[t];
		if ((fl & 16) != 0 && !NET_GAME) { continue; }   // multiplayer-only items join netgames
		if ((fl & SKILL_BIT) == 0) { continue; }     // skill spawn-filter bit
		if (NET_GAME && !NP_COOP && MINFO.has(ty)) { continue; }   // deathmatch is -nomonsters
		float ang = TH_ANG[t] * math::PI / 180.0;
		if (MINFO.has(ty)) {
			var mi = MINFO[ty];
			// co-op CLIENT spawns monsters as state-12 PUPPETS: the THINGS walk
			// is identical on both machines, so pool INDICES match the host's —
			// that shared index space is the whole MONS/BOOM/TAKE addressing
			// scheme. Puppets have no AI; the host's stream drives them.
			int mst = NET_GAME && NP_COOP && !NP_IS_HOST ? 12 : 1;
			mobj_spawn(TH_X[t] * 1.0, TH_Y[t] * 1.0, ty, ang, mst, mi[0], mi[6], mi[1], mi[3]);
			KILL_TOTAL = KILL_TOTAL + 1;
		} else if (PICKUPS.has(ty)) {
			int pk = mobj_spawn(TH_X[t] * 1.0, TH_Y[t] * 1.0, ty, ang, 7, PICKUPS[ty], "A", 0, 20.0);
			MO_CNT[pk] = 1;
			ITEM_TOTAL = ITEM_TOTAL + 1;
		} else if (DECOR.has(ty)) {
			// decorations join the SAME shared walk as monsters/pickups, so
			// co-op pool indices stay aligned across host and client
			var dd = DECOR[ty];
			mobj_spawn(TH_X[t] * 1.0, TH_Y[t] * 1.0, ty, ang, 10, "" + dd[0], "" + dd[1], 0, dd[2] * 1.0);
		} else if (ty == 2035) {
			mobj_spawn(TH_X[t] * 1.0, TH_Y[t] * 1.0, ty, ang, 9, "BAR1", "AB", 20, 10.0);
		}
	}
}

// --------------------------------------------------------------- sight/shot ---

// 2D ray vs map lines: nearest blocking t along p1->p2 (1e9 if clear).
// A line blocks if one-sided or its vertical opening misses the ray's z
// there; dzfull is the z rise across the whole ray (0 = flat trace).
int TRACE_LINE = -1;          // the line behind the last trace's best t
function trace_block_dz(float x1, float y1, float x2, float y2, float z, float dzfull) -> float {
	TRACE_LINE = -1;
	float best = 1000000000.0;
	int n = LD_V1.size();
	float rdx = x2 - x1;
	float rdy = y2 - y1;
	for (int i = 0; i < n; ++i) {
		float ax = VX_X[LD_V1[i]];
		float ay = VX_Y[LD_V1[i]];
		float bx = VX_X[LD_V2[i]];
		float by = VX_Y[LD_V2[i]];
		float d = (bx - ax) * rdy - (by - ay) * rdx;
		if (d > -0.0001 && d < 0.0001) { continue; }
		float t = ((x1 - ax) * rdy - (y1 - ay) * rdx) / d;
		float s = ((x1 - ax) * (by - ay) - (y1 - ay) * (bx - ax)) / d;
		if (t < 0.0 || t > 1.0 || s < 0.0 || s >= best) { continue; }
		int s1 = LD_SIDE1[i];
		if (s1 < 0) { best = s; TRACE_LINE = i; continue; }
		int f0 = SE_FLOORH[SD_SEC[LD_SIDE0[i]]];
		int f1 = SE_FLOORH[SD_SEC[s1]];
		int c0 = SE_CEILH[SD_SEC[LD_SIDE0[i]]];
		int c1 = SE_CEILH[SD_SEC[s1]];
		float ob = f0 > f1 ? f0 : f1;
		float ot = c0 < c1 ? c0 : c1;
		float zl = z + dzfull * s;
		if (zl <= ob || zl >= ot) { best = s; TRACE_LINE = i; }
	}
	return best;
}

function trace_block_t(float x1, float y1, float x2, float y2, float z) -> float {
	return trace_block_dz(x1, y1, x2, y2, z, 0.0);
}

function sight_clear(float x1, float y1, float x2, float y2, float z) -> bool {
	return trace_block_t(x1, y1, x2, y2, z) >= 1.0;
}

function mobj_height(int m) -> float {
	int ty = MO_TYPE[m];
	if (MINFO.has(ty)) { var mi = MINFO[ty]; float h = mi[4]; return h; }
	if (ty == 2035) { return 42.0; }
	return 16.0;
}

function sight_snd(int ty) -> string {
	if (ty == 3001) { return "DSBGSIT1"; }
	if (ty == 3002 || ty == 58) { return "DSSGTSIT"; }
	if (ty == 3003) { return "DSBRSSIT"; }
	if (ty == 3006) { return ""; }                   // skulls scream when they charge
	if (ty == 3005) { return "DSCACSIT"; }
	if (ty == 69) { return "DSKNTSIT"; }
	if (ty == 84) { return "DSSSSIT"; }
	if (ty == 65) { return "DSPOSIT2"; }
	return "DSPOSIT1";
}

function death_snd(int ty) -> string {
	if (ty == 3001) { return "DSBGDTH1"; }
	if (ty == 3002 || ty == 58) { return "DSSGTDTH"; }
	if (ty == 3003) { return "DSBRSDTH"; }
	if (ty == 3006) { return "DSFIRXPL"; }
	if (ty == 3005) { return "DSCACDTH"; }
	if (ty == 69) { return "DSKNTDTH"; }
	if (ty == 84) { return "DSSSDTH"; }
	if (ty == 65) { return "DSPODTH2"; }
	return "DSPODTH1";
}

// flat-aim vertical overlap: bullet z against a thing's [z, z+height] span
function shot_z_hit(float bz, float mz, float mh) -> bool {
	return bz >= mz && bz <= mz + mh;
}

// hitscan from (x,y,z) along ang with vertical autoaim: pass 1 picks the
// nearest thing reachable within doom's +-0.625 slope window; pass 2 traces
// the walls ALONG that slope (a ledge under a platform monster must not eat
// the shot). attacker -1 = player (excluded from its own shot). Returns the
// hit mobj, -2 when the player was hit, -1 for nothing.
float AIM_SLOPE = 0.625;
function shoot_hitscan(float x, float y, float z, float ang, int dmg, int attacker, float range) -> int {
	float ex = x + math::cos(ang) * range;
	float ey = y + math::sin(ang) * range;
	float rdx = ex - x;
	float rdy = ey - y;
	float best = 1.0;
	int hit = -1;
	float aimz = 0.0;                                // target mid-height for the slope
	int n = MO_X.size();
	for (int m = 0; m < n; ++m) {
		if (m == attacker) { continue; }
		int st = MO_STATE[m];
		if (st != 1 && st != 2 && st != 3 && st != 4 && st != 9 && st != 11) { continue; }
		// dm: only MY shots duel the ghost; co-op host: monster shots hit it too
		if (st == 11 && attacker != -1 && !(NP_COOP && attacker >= 0)) { continue; }
		float px = MO_X[m] - x;
		float py = MO_Y[m] - y;
		float t = (px * rdx + py * rdy) / (rdx * rdx + rdy * rdy);
		if (t <= 0.0 || t >= best) { continue; }
		float d = t * range;
		float mh = mobj_height(m);
		if ((MO_Z[m] - z) / d > AIM_SLOPE || (MO_Z[m] + mh - z) / d < 0.0 - AIM_SLOPE) { continue; }
		float cx = x + rdx * t - MO_X[m];
		float cy = y + rdy * t - MO_Y[m];
		float rr = MO_RAD[m] + 12.0;                 // forgiving hitbox, terminal aim is coarse
		if (cx * cx + cy * cy < rr * rr) {
			best = t;
			hit = m;
			aimz = MO_Z[m] + mh * 0.5;
		}
	}
	if (attacker >= 0 && P_HP > 0) {
		float ppx = P_X - x;
		float ppy = P_Y - y;
		float tp = (ppx * rdx + ppy * rdy) / (rdx * rdx + rdy * rdy);
		if (tp > 0.0 && tp < best) {
			float pd = tp * range;
			if ((P_FLOORZ - z) / pd <= AIM_SLOPE && (P_FLOORZ + P_HEIGHT - z) / pd >= 0.0 - AIM_SLOPE) {
				float pcx = x + rdx * tp - P_X;
				float pcy = y + rdy * tp - P_Y;
				float prr = P_RADIUS + 12.0;
				if (pcx * pcx + pcy * pcy < prr * prr) {
					best = tp;
					hit = -2;
					aimz = P_FLOORZ + 28.0;
				}
			}
		}
	}
	float dzfull = 0.0;
	if (hit != -1 && best > 0.0001) {
		float slope = (aimz - z) / (best * range);
		if (slope > AIM_SLOPE) { slope = AIM_SLOPE; }
		if (slope < 0.0 - AIM_SLOPE) { slope = 0.0 - AIM_SLOPE; }
		dzfull = slope * range;
	}
	float twall = trace_block_dz(x, y, ex, ey, z, dzfull);
	if (twall <= best) { hit = -1; }                 // a wall got there first
	int gl = TRACE_LINE;
	if (NP_FORWARD && attacker == -1) {              // co-op client: cosmetics only, damage is host's
		if (hit >= 0 && MO_STATE[hit] != 11) { decor_spawn(x + rdx * best, y + rdy * best, z + dzfull * best, "BLUD", "CBA"); }
		else if (hit == -1 && twall < 1.0) {
			float pt0 = twall - 6.0 / range;
			if (pt0 < 0.0) { pt0 = 0.0; }
			decor_spawn(x + rdx * pt0, y + rdy * pt0, z + dzfull * pt0 - 8.0, "PUFF", "ABCD");
		}
		return hit;
	}
	if (hit >= 0) {
		// a net ghost takes no local damage: its real player does (DAMAGE action
		// routed to the victim's slot by mobj identity)
		if (MO_STATE[hit] == 11) {
			if (attacker >= 0) { np_ghost_hit_mobj(hit, dmg, MO_X[attacker], MO_Y[attacker]); }
			else { np_ghost_hit_mobj(hit, dmg, x, y); }
		}
		else { mobj_damage(hit, dmg, attacker); }
		decor_spawn(x + rdx * best, y + rdy * best, z + dzfull * best, "BLUD", "CBA");
	} else if (hit == -2) { player_damage_from(dmg, x, y); }
	else if (twall < 1.0) {
		float pt = twall - 6.0 / range;              // puff just in front of the wall
		if (pt < 0.0) { pt = 0.0; }
		decor_spawn(x + rdx * pt, y + rdy * pt, z + dzfull * pt - 8.0, "PUFF", "ABCD");
		if (attacker < 0 && gl >= 0 && LD_SPECIAL[gl] == 46) { spec_line(gl, 2); }   // GR door
	}
	return hit;
}

// decorative dying-state mobjs (bullet puffs, blood): type 0 vanishes when the
// anim runs out, exactly like a spent projectile
function decor_spawn(float x, float y, float z, string spr, string anim) -> int {
	int m = mobj_spawn(x, y, 0, 0.0, 5, spr, anim, 0, 4.0);
	MO_Z[m] = z;
	MO_TIC[m] = 3;
	return m;
}

// ---------------------------------------------------------------- damage ---

// attacker: -1 = player, else the mobj that dealt the damage. A monster hurt
// by another monster turns on it (infighting).
function mobj_damage(int m, int dmg, int attacker) -> void {
	int st = MO_STATE[m];
	if (st != 1 && st != 2 && st != 3 && st != 4 && st != 9) { return; }
	MO_HP[m] = MO_HP[m] - dmg;
	if (MO_STATE[m] == 1) { MO_STATE[m] = 2; }       // wake on pain
	if (attacker >= 0 && attacker != m) {
		int ast = MO_STATE[attacker];
		// infighting retarget — and the net ghost counts as a shooter, so
		// co-op monsters aggro whichever player actually hurt them
		if (MINFO.has(MO_TYPE[m]) && (MINFO.has(MO_TYPE[attacker]) || ast == 11)
				&& (ast == 1 || ast == 2 || ast == 3 || ast == 4 || ast == 11)) { MO_TARGET[m] = attacker; }
	}
	if (MO_HP[m] <= 0) {
		if (MO_STATE[m] == 9) { barrel_explode(m); return; }
		MO_STATE[m] = 5;
		int ty = MO_TYPE[m];
		var mi = MINFO[ty];
		bool gib = XDEATH.has(ty) && MO_HP[m] <= 0 - MO_SHP[m];   // overkill + real xdeath art
		if (gib) { MO_ANIM[m] = XDEATH[ty]; } else { MO_ANIM[m] = mi[9]; }
		MO_FRAME[m] = 0;
		MO_TIC[m] = 5;
		MO_MOMX[m] = 0.0;
		MO_MOMY[m] = 0.0;
		MO_CHARGE[m] = 0;
		MO_BRAIN[m] = null;                          // the brain dies with it
		KILLS = KILLS + 1;
		snd_play_at(gib ? "DSSLOP" : death_snd(ty), MO_X[m], MO_Y[m]);
		mobj_drop(m);
		return;
	}
	if (MINFO.has(MO_TYPE[m]) && MO_STATE[m] != 9 && SIM_RANDOM.between(0, 255) < MINFO[MO_TYPE[m]][5]) {
		MO_STATE[m] = 4;
		MO_ANIM[m] = MINFO[MO_TYPE[m]][8];
		MO_FRAME[m] = 0;
		MO_TIC[m] = 6;
		MO_BRAIN[m] = null;                          // pain interrupt: re-minted when the anim completes
		snd_play_at("DSPOPAIN", MO_X[m], MO_Y[m]);
	}
}

function mobj_drop(int m) -> void {
	int ty = MO_TYPE[m];
	int d = -1;
	if (ty == 3004) { d = mobj_spawn(MO_X[m], MO_Y[m], 2007, 0.0, 7, "CLIP", "A", 0, 20.0); }
	else if (ty == 9) { d = mobj_spawn(MO_X[m], MO_Y[m], 2001, 0.0, 7, "SHOT", "A", 0, 20.0); }
	// drops grow the pool PAST the shared map-placed range, so they travel by
	// net id, not index (the client's pool diverges from here on)
	if (d >= 0 && NET_GAME && NP_COOP && NP_IS_HOST) { np_drop_announce(d); }
}

// visual half of a barrel death: the co-op client mirrors this on BOOM
// (no damage rolls — the host already resolved those authoritatively)
function barrel_boom_cosmetic(int m) -> void {
	MO_STATE[m] = 5;
	MO_SPR[m] = "BEXP";
	MO_ANIM[m] = "ABCDE";
	MO_FRAME[m] = 0;
	MO_TIC[m] = 5;
	snd_play_at("DSBAREXP", MO_X[m], MO_Y[m]);
}

// chain-safe: state flips to 5 before any splash lands, so a barrel caught in
// the blast explodes once and re-entry on it is a no-op
function barrel_explode(int m) -> void {
	barrel_boom_cosmetic(m);
	if (NET_GAME && NP_COOP && NP_IS_HOST) { np_boom_announce(m); }
	float bx = MO_X[m];
	float by = MO_Y[m];
	int n = MO_X.size();
	for (int o = 0; o < n; ++o) {
		if (o == m) { continue; }
		int ost = MO_STATE[o];
		float dx = MO_X[o] - bx;
		float dy = MO_Y[o] - by;
		float d2 = dx * dx + dy * dy;
		if (d2 >= 128.0 * 128.0) { continue; }
		int bdmg = math::itrunc(70.0 - math::sqrt(d2) / 2.0);
		if (ost == 11) { np_ghost_hit_mobj(o, bdmg, bx, by); }
		else { mobj_damage(o, bdmg, m); }
	}
	float pdx = P_X - bx;
	float pdy = P_Y - by;
	float pd2 = pdx * pdx + pdy * pdy;
	if (pd2 < 128.0 * 128.0) { player_damage_from(math::itrunc(70.0 - math::sqrt(pd2) / 2.0), bx, by); }
}

// ------------------------------------------------------------- projectiles ---

// shooter is recorded on the projectile (MO_TARGET) so impacts credit
// infighting; -1 = the player (skips the self-hit check, kills credit the
// player). z steps linearly toward the target's mid-height over `dist`
function projectile_spawn(float x, float y, float z, float ang, int shooter, float tmidz, float dist, string spr, string anim, float speed, string fsnd) -> int {
	if (fsnd != "") { snd_play_at(fsnd, x, y); }
	int m = mobj_spawn(x, y, 0, ang, 8, spr, anim, 1, 6.0);
	MO_Z[m] = z;
	MO_MOMX[m] = math::cos(ang) * speed;
	MO_MOMY[m] = math::sin(ang) * speed;
	if (dist < 10.0) { dist = 10.0; }
	MO_MOMZ[m] = (tmidz - z) / (dist / speed);
	MO_TARGET[m] = shooter;
	if (NET_GAME) { np_proj_announce(m, fsnd); }     // netplay streams authoritative spawns
	return m;
}

function fireball_spawn(float x, float y, float z, float ang, int shooter, float tmidz, float dist) -> int {
	return projectile_spawn(x, y, z, ang, shooter, tmidz, dist, "BAL1", "AB", 10.0, "DSFIRSHT");
}

function rocket_spawn(float x, float y, float z, float ang, int shooter) -> int {
	return projectile_spawn(x, y, z, ang, shooter, z, 2048.0, "MISL", "A", 25.0, "");
}

// splash falls off linearly (doom's damage - distance), hits monsters AND the
// player (rocket jumps: the blast also shoves)
function radius_damage(float bx, float by, float radius, int maxdmg, int attacker) -> void {
	int n = MO_X.size();
	for (int o = 0; o < n; ++o) {
		int st = MO_STATE[o];
		if (st != 1 && st != 2 && st != 3 && st != 4 && st != 9 && st != 11) { continue; }
		float dx = MO_X[o] - bx;
		float dy = MO_Y[o] - by;
		float d2 = dx * dx + dy * dy;
		if (d2 >= radius * radius) { continue; }
		int sdmg = maxdmg - math::itrunc(math::sqrt(d2));
		if (sdmg <= 0) { continue; }
		if (st == 11) { np_ghost_hit_mobj(o, sdmg, bx, by); }   // blasts splash the ghosts too
		else { mobj_damage(o, sdmg, attacker); }
	}
	if (P_HP <= 0) { return; }
	float pdx = P_X - bx;
	float pdy = P_Y - by;
	float pd2 = pdx * pdx + pdy * pdy;
	if (pd2 < radius * radius) {
		float pd = math::sqrt(pd2);
		int pdmg = maxdmg - math::itrunc(pd);
		if (pdmg > 0) {
			player_damage_from(pdmg, bx, by);
			if (pd > 1.0) {
				float shove = pdmg * 0.02;
				P_MOMX = P_MOMX + pdx / pd * shove;
				P_MOMY = P_MOMY + pdy / pd * shove;
			}
		}
	}
}

function projectile_explode(int m) -> void {
	MO_STATE[m] = 5;
	MO_FRAME[m] = 0;
	MO_TIC[m] = 4;
	MO_MOMX[m] = 0.0;
	MO_MOMY[m] = 0.0;
	MO_MOMZ[m] = 0.0;
	if (NET_GAME) { np_expl_announce(m); }           // sync the pop onto streamed twins
	if (MO_SPR[m] == "MISL") {
		MO_ANIM[m] = "BCD";                          // rocket blast frames
		snd_play_at("DSBAREXP", MO_X[m], MO_Y[m]);
		if (MO_NODMG[m] == 0) { radius_damage(MO_X[m], MO_Y[m], 128.0, 128, MO_TARGET[m]); }
	} else {
		MO_ANIM[m] = "CDE";                          // impact puff frames
		snd_play_at("DSFIRXPL", MO_X[m], MO_Y[m]);
	}
}

function projectile_tic(int m) -> void {
	int shooter = MO_TARGET[m];
	float nx = MO_X[m] + MO_MOMX[m];
	float ny = MO_Y[m] + MO_MOMY[m];
	float nz = MO_Z[m] + MO_MOMZ[m];
	bool rocket = MO_SPR[m] == "MISL";
	bool cosmetic = MO_NODMG[m] != 0;                // co-op client rocket: explode-only
	bool streamed = MO_NODMG[m] == 2;                // net twin: EXPL/walls own the pop
	if (streamed) {
		MO_COOL[m] = MO_COOL[m] - 1;                 // lifetime guard for a lost EXPL
		if (MO_COOL[m] <= 0) { projectile_explode(m); return; }
	}
	if (!cosmetic && shooter >= 0 && P_HP > 0) {
		float dx = nx - P_X;
		float dy = ny - P_Y;
		if (dx * dx + dy * dy < (16.0 + MO_RAD[m]) * (16.0 + MO_RAD[m])
				&& shot_z_hit(nz, P_FLOORZ, P_HEIGHT)) {
			player_damage_from(rocket ? 20 * SIM_RANDOM.between(1, 8) : 8 * SIM_RANDOM.between(1, 3), MO_X[m], MO_Y[m]);
			projectile_explode(m);
			return;
		}
	}
	int n = streamed ? 0 : MO_X.size();              // streamed twins fly through everything
	for (int o = 0; o < n; ++o) {
		if (o == m || o == shooter) { continue; }
		int st = MO_STATE[o];
		if (st != 1 && st != 2 && st != 3 && st != 4 && st != 9 && st != 11 && st != 12) { continue; }
		// dm: only MY projectiles duel the ghost; co-op host: monster shots too
		if (st == 11 && shooter != -1 && !(NP_COOP && shooter >= 0)) { continue; }
		if (st == 12 && !cosmetic) { continue; }         // puppets only stop cosmetic rockets
		float odx = nx - MO_X[o];
		float ody = ny - MO_Y[o];
		float orr = MO_RAD[o] + MO_RAD[m];
		if (odx * odx + ody * ody >= orr * orr) { continue; }
		if (!shot_z_hit(nz, MO_Z[o], mobj_height(o))) { continue; }
		if (cosmetic) { projectile_explode(m); return; } // visual pop, host owns the numbers
		int pdmg = rocket ? 20 * SIM_RANDOM.between(1, 8) : 8 * SIM_RANDOM.between(1, 3);
		if (st == 11) { np_ghost_hit_mobj(o, pdmg, MO_X[m], MO_Y[m]); }
		else { mobj_damage(o, pdmg, shooter); }
		projectile_explode(m);
		return;
	}
	if (trace_block_t(MO_X[m], MO_Y[m], nx, ny, MO_Z[m]) < 1.0) {
		projectile_explode(m);
		return;
	}
	int sec = sector_at(nx, ny);
	if (nz <= SE_FLOORH[sec] || nz >= SE_CEILH[sec]) {
		projectile_explode(m);
		return;
	}
	MO_X[m] = nx;
	MO_Y[m] = ny;
	MO_Z[m] = nz;
}

// -------------------------------------------------------------------- AI ---

// resolves m's pursuit target (player or another mobj) into the TGT_ globals;
// a dead mobj target reverts to the player
float TGT_X = 0.0;
float TGT_Y = 0.0;
float TGT_MIDZ = 0.0;
bool TGT_LIVE = false;
function target_info(int m) -> void {
	int t = MO_TARGET[m];
	if (t >= 0) {
		int st = MO_STATE[t];
		bool ghost_live = st == 11 && np_tgt_live(t);
		if (st == 1 || st == 2 || st == 3 || st == 4 || ghost_live) {
			TGT_X = MO_X[t];
			TGT_Y = MO_Y[t];
			TGT_MIDZ = MO_Z[t] + mobj_height(t) * 0.5;
			TGT_LIVE = true;
			return;
		}
		MO_TARGET[m] = -1;
	}
	// co-op host: untargeted monsters pick the NEAREST live player — the host
	// or ANY of the puppet ghosts (netplay mirrors them into NP_TGT_* each tic)
	if (NET_GAME && NP_COOP && NP_IS_HOST && NP_TGT_X.size() > 0) {
		float pdx = P_X - MO_X[m];
		float pdy = P_Y - MO_Y[m];
		float best = P_HP > 0 ? pdx * pdx + pdy * pdy : 1000000000.0;
		int pick = -1;
		int ng = NP_TGT_X.size();
		for (int g = 0; g < ng; ++g) {
			float gdx = NP_TGT_X[g] - MO_X[m];
			float gdy = NP_TGT_Y[g] - MO_Y[m];
			float gd2 = gdx * gdx + gdy * gdy;
			if (gd2 < best) { best = gd2; pick = g; }
		}
		if (pick >= 0) {
			int gm = NP_TGT_MOBJ[pick];
			TGT_X = MO_X[gm];
			TGT_Y = MO_Y[gm];
			TGT_MIDZ = MO_Z[gm] + 28.0;
			TGT_LIVE = true;
			MO_TARGET[m] = gm;                       // commit so attacks route to that ghost
			return;
		}
	}
	TGT_X = P_X;
	TGT_Y = P_Y;
	TGT_MIDZ = P_FLOORZ + 28.0;
	TGT_LIVE = P_HP > 0;
}

// a ghost mobj is a live target while netplay lists it in the target mirror
function np_tgt_live(int mobj) -> bool {
	int n = NP_TGT_MOBJ.size();
	for (int i = 0; i < n; ++i) {
		if (NP_TGT_MOBJ[i] == mobj) { return true; }
	}
	return false;
}

// lost soul commit: high momentum toward the target, decaying; one bite
function charge_start(int m) -> void {
	snd_play_at("DSSKLATK", MO_X[m], MO_Y[m]);
	MO_CHARGE[m] = 1;
	MO_MOMX[m] = math::cos(MO_ANG[m]) * 20.0;
	MO_MOMY[m] = math::sin(MO_ANG[m]) * 20.0;
	var mi = MINFO[MO_TYPE[m]];
	MO_ANIM[m] = mi[7];
	MO_FRAME[m] = 0;
	MO_COOL[m] = 40 + SIM_RANDOM.between(0, 20);
	if (FAST_MONSTERS) { MO_COOL[m] = MO_COOL[m] / 2; }
}

function charge_tic(int m) -> void {
	target_info(m);
	bool bit = false;
	if (TGT_LIVE) {
		int t = MO_TARGET[m];
		float tr = t >= 0 ? MO_RAD[t] : P_RADIUS;
		float dx = TGT_X - MO_X[m];
		float dy = TGT_Y - MO_Y[m];
		float rr = MO_RAD[m] + tr + 8.0;
		if (dx * dx + dy * dy < rr * rr) {
			int dmg = SIM_RANDOM.between(1, 8) + SIM_RANDOM.between(1, 8) + SIM_RANDOM.between(1, 8);
			if (t >= 0) { mobj_damage(t, dmg, m); } else { player_damage_from(dmg, MO_X[m], MO_Y[m]); }
			bit = true;
		}
	}
	float nx = MO_X[m] + MO_MOMX[m];
	float ny = MO_Y[m] + MO_MOMY[m];
	bool moved = false;
	if (mobj_pos_ok(m, nx, ny, true)) { MO_X[m] = nx; MO_Y[m] = ny; moved = true; }
	else if (mobj_pos_ok(m, nx, MO_Y[m], true)) { MO_X[m] = nx; moved = true; }
	else if (mobj_pos_ok(m, MO_X[m], ny, true)) { MO_Y[m] = ny; moved = true; }
	MO_Z[m] = SE_FLOORH[sector_at(MO_X[m], MO_Y[m])] * 1.0;
	MO_MOMX[m] = MO_MOMX[m] * 0.9;
	MO_MOMY[m] = MO_MOMY[m] * 0.9;
	float sp2 = MO_MOMX[m] * MO_MOMX[m] + MO_MOMY[m] * MO_MOMY[m];
	if (bit || !moved || sp2 < 16.0) {
		MO_CHARGE[m] = 0;
		MO_MOMX[m] = 0.0;
		MO_MOMY[m] = 0.0;
		MO_STATE[m] = 2;
		var mi = MINFO[MO_TYPE[m]];
		MO_ANIM[m] = mi[6];
		MO_FRAME[m] = 0;
	}
}

// One brain per monster, one yield per tic: behavior reads top to bottom
// (sleep until sighted, then chase; in range -> wind up, coast the lunge,
// the anim driver lands the strike on the last attack frame). Phase lives in
// the coroutine frame; MO_STATE stays the observable enum everything else
// reads. Interrupts (pain, death, a skull charge) drop the handle and
// monster_tic re-mints it when the interrupt hands control back.
coroutine void monster_brain(int m) {
	var mi = MINFO[MO_TYPE[m]];
	while (MO_STATE[m] == 1) {
		// asleep: wake on sight, generous cone (they hear doors in real doom anyway)
		float wdx = P_X - MO_X[m];
		float wdy = P_Y - MO_Y[m];
		if (P_HP > 0 && wdx * wdx + wdy * wdy < 1440000.0
				&& sight_clear(MO_X[m], MO_Y[m], P_X, P_Y, MO_Z[m] + 32.0)) {
			MO_STATE[m] = 2;
			MO_TIC[m] = 8;
			var ss = sight_snd(MO_TYPE[m]);
			if (ss != "") { snd_play_at(ss, MO_X[m], MO_Y[m]); }
		}
		yield;
	}
	while (true) {                                   // the chase; only an interrupt ends it
		target_info(m);
		float dx = TGT_X - MO_X[m];
		float dy = TGT_Y - MO_Y[m];
		float dist = math::sqrt(dx * dx + dy * dy);
		float eyez = MO_Z[m] + 32.0;
		MO_ANG[m] = math::atan2(dy, dx);
		if (MO_COOL[m] > 0) { MO_COOL[m] = MO_COOL[m] - 1; }
		int kind = mi[10];
		// kind 1: mi[12] IS the melee reach; kind 3: mi[12] is MISSILE range —
		// its melee arm reaches 64 (conflating them made imps claw at any range
		// and never throw)
		float melee_r = kind == 1 ? 1.0 * mi[12] : 64.0;
		bool melee_range = (kind == 1 || kind == 3) && dist < melee_r + 20.0;
		bool can_missile = (kind == 0 || kind == 2 || kind == 3) && dist < mi[12];
		if (TGT_LIVE && MO_COOL[m] == 0 && (melee_range || can_missile)
				&& sight_clear(MO_X[m], MO_Y[m], TGT_X, TGT_Y, eyez)) {
			if (MO_TYPE[m] == 3006) { charge_start(m); return; }   // skull commits; charge_tic owns it now
			MO_STATE[m] = 3;                         // wind up
			MO_ANIM[m] = mi[7];
			MO_FRAME[m] = 0;
			MO_TIC[m] = 6;
			if (melee_range) {                       // bite commit: small forward lunge
				MO_MOMX[m] = math::cos(MO_ANG[m]) * 8.0;
				MO_MOMY[m] = math::sin(MO_ANG[m]) * 8.0;
			}
			yield;
			while (MO_STATE[m] == 3) {               // coast the lunge until the strike lands
				if (MO_MOMX[m] != 0.0 || MO_MOMY[m] != 0.0) {
					float lx = MO_X[m] + MO_MOMX[m];
					float ly = MO_Y[m] + MO_MOMY[m];
					if (mobj_pos_ok(m, lx, ly, false)) { MO_X[m] = lx; MO_Y[m] = ly; }
					MO_MOMX[m] = MO_MOMX[m] * 0.65;
					MO_MOMY[m] = MO_MOMY[m] * 0.65;
				}
				yield;
			}
			continue;
		}
		// walk toward the target, sliding off walls like the player does
		float sp = mi[2];
		if (FAST_MONSTERS) { sp = sp * 1.5; }
		float nx = MO_X[m] + math::cos(MO_ANG[m]) * sp;
		float ny = MO_Y[m] + math::sin(MO_ANG[m]) * sp;
		if (mobj_pos_ok(m, nx, ny, false)) { MO_X[m] = nx; MO_Y[m] = ny; }
		else if (mobj_pos_ok(m, nx, MO_Y[m], false)) { MO_X[m] = nx; }
		else if (mobj_pos_ok(m, MO_X[m], ny, false)) { MO_Y[m] = ny; }
		MO_Z[m] = SE_FLOORH[sector_at(MO_X[m], MO_Y[m])] * 1.0;
		yield;
	}
}

function monster_tic(int m) -> void {
	if (MO_CHARGE[m] > 0) { charge_tic(m); return; }
	if (MO_STATE[m] == 4) { return; }                // pain stun; the anim driver ends it
	var b = MO_BRAIN[m];
	if (b == null || b.done()) { MO_BRAIN[m] = monster_brain(m); }
	MO_BRAIN[m].next();
}

// monster movement: same opening rules as the player, own radius, no big
// drops; fly = charging lost soul (projectile-like, no step/dropoff rules)
function mobj_pos_ok(int m, float x, float y, bool fly) -> bool {
	float r = MO_RAD[m];
	int n = LD_V1.size();
	float fz = MO_Z[m];
	// monsters are few: whole-line scan keeps this simple and correct enough
	for (int i = 0; i < n; ++i) {
		float ax = VX_X[LD_V1[i]];
		float ay = VX_Y[LD_V1[i]];
		float bx = VX_X[LD_V2[i]];
		float by = VX_Y[LD_V2[i]];
		float ddx = bx - ax;
		float ddy = by - ay;
		float len2 = ddx * ddx + ddy * ddy;
		if (len2 < 0.001) { continue; }
		float t = ((x - ax) * ddx + (y - ay) * ddy) / len2;
		if (t < 0.0) { t = 0.0; }
		if (t > 1.0) { t = 1.0; }
		float cx = ax + ddx * t - x;
		float cy = ay + ddy * t - y;
		if (cx * cx + cy * cy >= r * r) { continue; }
		int s1 = LD_SIDE1[i];
		if (s1 < 0) { return false; }
		if ((LD_FLAGS[i] & 1) != 0) { return false; }
		int f0 = SE_FLOORH[SD_SEC[LD_SIDE0[i]]];
		int f1 = SE_FLOORH[SD_SEC[s1]];
		int c0 = SE_CEILH[SD_SEC[LD_SIDE0[i]]];
		int c1 = SE_CEILH[SD_SEC[s1]];
		float ob = f0 > f1 ? f0 : f1;
		float ot = c0 < c1 ? c0 : c1;
		if (ot - ob < 56.0) { return false; }
		if (fly) { continue; }
		if (ob - fz > 24.0) { return false; }
		float lower = f0 < f1 ? f0 : f1;
		if (fz - lower > 24.0) { return false; }     // no dropoffs for monsters
	}
	// avoid stacking into other live monsters (or solid decor: MO_RAD > 0)
	int nm = MO_X.size();
	for (int o = 0; o < nm; ++o) {
		if (o == m) { continue; }
		int st = MO_STATE[o];
		if (st != 1 && st != 2 && st != 3 && st != 4 && st != 9
				&& !(st == 10 && MO_RAD[o] > 0.0)) { continue; }
		float odx = MO_X[o] - x;
		float ody = MO_Y[o] - y;
		float rr = MO_RAD[o] + r;
		if (odx * odx + ody * ody < rr * rr) { return false; }
	}
	return true;
}

// anim driver: advances frames; attack fires on the last attack frame
function mobj_anim_tic(int m) -> void {
	if (MO_TIC[m] > 0) { MO_TIC[m] = MO_TIC[m] - 1; return; }
	int st = MO_STATE[m];
	MO_FRAME[m] = MO_FRAME[m] + 1;
	if (MO_FRAME[m] >= MO_ANIM[m].size()) {
		if (st == 5) {
			// projectiles and barrels vanish after their explosion; monsters become corpses
			if (MO_TYPE[m] == 0 || MO_TYPE[m] == 2035) { MO_STATE[m] = 0; }
			else {
				MO_STATE[m] = 6;
				if (NIGHTMARE && MINFO.has(MO_TYPE[m])) { MO_RESP[m] = 1050; }   // ~30s
			}
			MO_FRAME[m] = MO_ANIM[m].size() - 1;
			return;
		}
		if (st == 4 || st == 3) {
			if (st == 3) { monster_attack(m); }
			var mi = MINFO[MO_TYPE[m]];
			MO_STATE[m] = 2;
			MO_ANIM[m] = mi[6];
		}
		MO_FRAME[m] = 0;
	}
	MO_TIC[m] = st == 2 ? 5 : 5;
}

// melee only lands if the target is still in range when the anim completes
function monster_attack(int m) -> void {
	var mi = MINFO[MO_TYPE[m]];
	int kind = mi[10];
	target_info(m);
	float dx = TGT_X - MO_X[m];
	float dy = TGT_Y - MO_Y[m];
	float dist = math::sqrt(dx * dx + dy * dy);
	MO_COOL[m] = 25 + SIM_RANDOM.between(0, 20);
	if (FAST_MONSTERS) { MO_COOL[m] = MO_COOL[m] / 2; }
	MO_MOMX[m] = 0.0;                                // lunge spent
	MO_MOMY[m] = 0.0;
	if (!TGT_LIVE) { return; }
	int t = MO_TARGET[m];
	float melee_r = kind == 1 ? 1.0 * mi[12] : 64.0;
	if ((kind == 1 || kind == 3) && dist < melee_r + 24.0) {
		snd_play_at(kind == 1 ? "DSSGTATK" : "DSCLAW", MO_X[m], MO_Y[m]);
		int mdmg = mi[11] + SIM_RANDOM.between(0, mi[11]);
		if (t >= 0 && MO_STATE[t] == 11) { np_ghost_hit_mobj(t, mdmg, MO_X[m], MO_Y[m]); }   // bite lands on that ghost's player
		else if (t >= 0) { mobj_damage(t, mdmg, m); }
		else { player_damage_from(mdmg, MO_X[m], MO_Y[m]); }
		return;
	}
	float eyez = MO_Z[m] + 32.0;
	// an invisible player is hard to draw a bead on: big aim wobble
	float wob = t < 0 && P_INVIS > 0 ? 0.9 : 0.12;
	if (kind == 0) {
		snd_play_at(MO_TYPE[m] == 9 ? "DSSHOTGN" : "DSPISTOL", MO_X[m], MO_Y[m]);
		float aim = math::atan2(dy, dx) + (SIM_RANDOM.unit() - 0.5) * wob;
		ATK_AIM = aim;
		shoot_hitscan(MO_X[m], MO_Y[m], eyez, aim, 3 * SIM_RANDOM.between(1, 5), m, 2048.0);
	} else if (kind == 2 || kind == 3) {
		float maim = math::atan2(dy, dx);
		if (t < 0 && P_INVIS > 0) { maim = maim + (SIM_RANDOM.unit() - 0.5) * 0.9; }
		ATK_AIM = maim;
		var bspr = "BAL1";
		if (MISSILE_SPR.has(MO_TYPE[m])) { bspr = "" + MISSILE_SPR[MO_TYPE[m]]; }
		projectile_spawn(MO_X[m] + math::cos(MO_ANG[m]) * 20.0, MO_Y[m] + math::sin(MO_ANG[m]) * 20.0,
			eyez, maim, m, TGT_MIDZ, dist, bspr, "AB", 10.0, "DSFIRSHT");
	}
}
float ATK_AIM = 0.0;          // the last aim a monster attack used (gate hook)

// nightmare corpse comes back where it fell: teleport-fog flash, walk anim,
// full spawn hp; the kill tally can pass 100% exactly like vanilla
function mobj_respawn_tic(int m) -> void {
	MO_RESP[m] = MO_RESP[m] - 1;
	if (MO_RESP[m] > 0) { return; }
	MO_RESP[m] = 0;
	var mi = MINFO[MO_TYPE[m]];
	MO_STATE[m] = 1;
	MO_HP[m] = MO_SHP[m];
	MO_ANIM[m] = mi[6];
	MO_FRAME[m] = 0;
	MO_TIC[m] = 4;
	MO_COOL[m] = 0;
	MO_CHARGE[m] = 0;
	MO_BRAIN[m] = null;
	snd_play_at("DSTELEPT", MO_X[m], MO_Y[m]);
	if (TFOG_ANIM != "") { decor_spawn(MO_X[m], MO_Y[m], MO_Z[m], "TFOG", TFOG_ANIM); }
}

function things_tic() -> void {
	int n = MO_X.size();
	for (int m = 0; m < n; ++m) {
		int st = MO_STATE[m];
		if (st == 6) {
			if (NIGHTMARE && MO_RESP[m] > 0) { mobj_respawn_tic(m); }
			continue;
		}
		if (st == 10) {                              // animated decor cycles; static art idles free
			if (MO_ANIM[m].size() > 1) { mobj_anim_tic(m); }
			continue;
		}
		if (st == 0 || st == 7 || st == 11 || st == 12) { continue; }   // net ghosts/puppets tick in np_frame
		if (st == 8) { projectile_tic(m); mobj_anim_tic(m); continue; }
		if (st == 9) { mobj_anim_tic(m); continue; }
		if (st == 5) { mobj_anim_tic(m); continue; }
		monster_tic(m);
		mobj_anim_tic(m);
	}
}
