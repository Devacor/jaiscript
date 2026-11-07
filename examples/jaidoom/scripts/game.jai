// JaiDOOM game.jai — the playsim: 35Hz tics, doom movement constants,
// blockmap collision with axis-split wall slide, DR/D1 doors, use lines,
// exit switches. All heights live in the SE_ arrays, so the renderer sees
// moving doors for free.

import "sim_random.jai";
import "wad.jai";
import "map.jai";
import "vid.jai";
import "sound.jai";
import "mus.jai";
import "things.jai";
import "render.jai";
import "spec.jai";
import "anim.jai";
import "demo.jai";

// netplay.jai hooks behind NET_GAME: extern-declared so programs that never import netplay.jai still check
extern function np_tic;
extern function np_fire_send;
extern function np_take_request;
extern function np_item_announce;
extern function np_use_send;
extern function np_switch_announce;

float P_X = 0.0;
float P_Y = 0.0;
float P_ANG = 0.0;
float P_MOMX = 0.0;
float P_MOMY = 0.0;
float P_FLOORZ = 0.0;
float P_VIEWZ = 41.0;         // eases toward P_FLOORZ + 41 on stairs/lifts
float P_RADIUS = 16.0;
float P_HEIGHT = 56.0;
float P_STEP = 24.0;
float P_Z = 0.0;              // feet height; above P_FLOORZ = falling
float P_MOMZ = 0.0;
bool P_AIR = false;           // in a gravity arc: no thrust, no friction, no step-up
float P_AIRZ0 = 0.0;          // z where the ground was left (landing-thud height)

float CMD_FWD = 0.0;          // forwardmove units (walk 25 / run 50)
float CMD_SIDE = 0.0;
float CMD_TURN = 0.0;         // radians per tic
bool CMD_USE = false;
bool CMD_FIRE = false;
bool USE_LATCH = false;

int P_HP = 100;
int P_ARMOR = 0;
int P_KEYS = 0;               // 1 blue, 2 red, 4 yellow
int AMMO_BULLETS = 50;
int AMMO_SHELLS = 0;
int AMMO_ROCKETS = 0;
int AMMO_BULLETS_MAX = 200;   // a backpack doubles all three
int AMMO_SHELLS_MAX = 50;
int AMMO_ROCKETS_MAX = 50;
bool HAVE_SHOTGUN = false;
bool HAVE_CHAINGUN = false;
bool HAVE_LAUNCHER = false;
bool P_BERSERK = false;       // fists x10 for the rest of the level
int BERSERK_TICS = 0;         // the pickup's red burst (palette only)
int P_INVIS = 0;              // tics left; monsters aim with a wide wobble
int P_SUIT = 0;               // tics left; sector damage suppressed, green tint
int P_GOGGLES = 0;            // tics left; render fullbright
bool P_ALLMAP = false;        // automap shows all lines brighter
bool P_BACKPACK = false;
int CUR_WEAPON = 2;           // 1 fist, 2 pistol, 3 shotgun, 4 chaingun, 5 rocket launcher
bool P_GOD = false;           // iddqd: damage no-ops, STFGOD0 face
bool P_NOCLIP = false;        // idclip: relative moves ignore walls (sectors still track)
int W_COOL = 0;
int FLASH_TICS = 0;
int PAIN_TICS = 0;
int BONUS_TICS = 0;
int GRIN_TICS = 0;            // evil-grin window after a weapon pickup
int FIRE_HELD = 0;            // consecutive tics with the trigger down (rampage face)
int FACE_SIDE = 0;            // -1 hit from the left, 1 right, 0 frontal/unknown
bool FACE_PAIN_BIG = false;   // >=20 damage gapes (STFOUCH) instead of turning

bool NET_GAME = false;        // netplay.jai flips it; spawn rules + per-tic net hooks key on it
bool NP_COOP = false;         // netgame mode: false = deathmatch, true = co-op (netplay.jai sets)
bool NP_IS_HOST = false;      // co-op authority: host runs monsters/movers, client mirrors
bool NP_FORWARD = false;      // co-op client: own shots are cosmetic, the FIRE action carries the damage
// declared here (not netplay.jai) so gates that import game.jai without the
// net layer still parse the guards; netplay is the only writer
string GAME_MAP = "";
int GAME_TIC = 0;
int LEVEL_START_TIC = 0;
bool LEVEL_DONE = false;
bool SECRET_EXIT = false;
string GAME_MSG = "";
int MSG_TIMER = 0;

// active doors: each Door drives its sector's ceiling in SE_CEILH
var DOORS = [];

// doors announce from their sector's first-seg midpoint (a remote tag door
// should sound remote); silent mode bails before the seg scan
function snd_sector(int sec, string name) -> void {
	if (!SND_ON) { return; }
	int n = SEG_FSEC.size();
	for (int i = 0; i < n; ++i) {
		if (SEG_FSEC[i] != sec) { continue; }
		snd_play_at(name, (VX_X[SG_V1[i]] + VX_X[SG_V2[i]]) * 0.5, (VX_Y[SG_V1[i]] + VX_Y[SG_V2[i]]) * 0.5);
		return;
	}
	snd_play(name);
}

class Door {
	int sec = 0;
	int state = 0;                // 0 opening, 1 waiting, 2 closing
	int timer = 0;
	int top = 0;
	int stay = 0;
	Door(int s, int st, int stayopen) {
		sec = s;
		state = st;
		top = lowest_neighbor_ceiling(s) - 4;
		stay = stayopen;
	}
	// true when the door finished and leaves the live list
	bool tic() {
		if (state == 0) {
			SE_CEILH[sec] = SE_CEILH[sec] + 2;
			if (SE_CEILH[sec] >= top) {
				SE_CEILH[sec] = top;
				if (stay != 0) { return true; }
				state = 1;
				timer = 150;
			}
		} else if (state == 1) {
			timer = timer - 1;
			if (timer <= 0) { state = 2; snd_sector(sec, "DSDORCLS"); }
		} else if (sector_at(P_X, P_Y) == sec) {
			state = 0;            // reopen instead of crushing whoever stands in the doorway
			snd_sector(sec, "DSDOROPN");
		} else {
			SE_CEILH[sec] = SE_CEILH[sec] - 2;
			if (SE_CEILH[sec] <= SE_FLOORH[sec]) {
				SE_CEILH[sec] = SE_FLOORH[sec];
				return true;
			}
		}
		return false;
	}
}

function set_msg(string m) -> void {
	GAME_MSG = m;
	MSG_TIMER = 70;
}

function game_start_map(string name) -> bool {
	if (!map_load(name)) { return false; }
	prepare_render();
	GAME_MAP = name;
	LEVEL_START_TIC = GAME_TIC;
	LEVEL_DONE = false;
	SECRET_EXIT = false;
	DOORS = [];
	weapons_init();
	P_BERSERK = false;                               // powers end with the level (backpack persists)
	BERSERK_TICS = 0;
	P_INVIS = 0;
	P_SUIT = 0;
	P_GOGGLES = 0;
	P_ALLMAP = false;
	P_MOMX = 0.0;
	P_MOMY = 0.0;
	// the co-op joiner takes the player-2 start; everyone else takes player 1
	int want_start = NET_GAME && NP_COOP && !NP_IS_HOST ? 2 : 1;
	int nthings = TH_X.size();
	bool placed = false;
	for (int t = 0; t < nthings; ++t) {
		if (TH_TYPE[t] == want_start) {
			P_X = TH_X[t];
			P_Y = TH_Y[t];
			P_ANG = TH_ANG[t] * math::PI / 180.0;
			placed = true;
			break;
		}
	}
	if (!placed) {
		for (int t2 = 0; t2 < nthings; ++t2) {
			if (TH_TYPE[t2] == 1) {
				P_X = TH_X[t2] + 32.0;                   // no P2 start: offset off P1
				P_Y = TH_Y[t2];
				P_ANG = TH_ANG[t2] * math::PI / 180.0;
				break;
			}
		}
	}
	P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
	P_Z = P_FLOORZ * 1.0;
	P_MOMZ = 0.0;
	P_AIR = false;
	P_VIEWZ = P_FLOORZ + 41.0;
	sprites_init();
	things_data_init();
	things_spawn_map();
	spec_init();
	anim_init();
	music_want(map_music(name));
	W_COOL = 0;
	FLASH_TICS = 0;
	PAIN_TICS = 0;
	BONUS_TICS = 0;
	set_msg(name);
	return true;
}

// ------------------------------------------------------------- the player ---

// damage with a known source: the face turns toward the attacker
function player_damage_from(int dmg, float sx, float sy) -> void {
	if (P_HP <= 0 || P_GOD) { return; }
	float rel = math::atan2(sy - P_Y, sx - P_X) - P_ANG;
	while (rel > math::PI) { rel = rel - math::TAU; }
	while (rel < 0.0 - math::PI) { rel = rel + math::TAU; }
	player_damage(dmg);
	if (rel > 0.39) { FACE_SIDE = -1; }              // attacker to the left
	else if (rel < -0.39) { FACE_SIDE = 1; }
}

function player_damage(int dmg) -> void {
	if (P_HP <= 0 || P_GOD) { return; }
	FACE_SIDE = 0;
	FACE_PAIN_BIG = dmg >= 20;
	int soaked = P_ARMOR > 0 ? dmg / 3 : 0;
	if (soaked > P_ARMOR) { soaked = P_ARMOR; }
	P_ARMOR = P_ARMOR - soaked;
	P_HP = P_HP - (dmg - soaked);
	PAIN_TICS = PAIN_TICS + dmg;
	if (PAIN_TICS > 60) { PAIN_TICS = 60; }
	if (P_HP <= 0) {
		P_HP = 0;
		snd_play("DSPLDETH");
		set_msg("you died. enter restarts the map");
	} else {
		snd_play("DSPLPAIN");
	}
}

// the arsenal: keys 1-4 index WEAPONS (slot 0 pads the 1-based keys); the
// observables the HUD and gates read (CUR_WEAPON, AMMO_*, W_COOL, FLASH_TICS)
// stay globals the class writes
var WEAPONS = [];

class Weapon {
	string name = "";
	string spr = "";              // view overlay sprite
	string flash = "";            // muzzle flash overlay
	int ammo = 0;                 // 0 none, 1 bullets, 2 shells, 3 rockets
	int dmg_mul = 0;              // per pellet: dmg_mul * random(1, dmg_roll)
	int dmg_roll = 1;
	int pellets = 1;
	float spread = 0.0;
	float range = 2048.0;
	int cooldown = 0;
	int flash_tics = 0;
	int missile = 0;              // 1 = fires a rocket instead of hitscan pellets
	string anim = "";             // fire-sequence frames, spread across the cooldown
	string snd = "";              // DS* lump played on fire
	Weapon(string n, string sp, string fl, int am, int dm, int dr, int pe, float sd, float rg, int cd, int ft) {
		name = n; spr = sp; flash = fl; ammo = am; dmg_mul = dm; dmg_roll = dr;
		pellets = pe; spread = sd; range = rg; cooldown = cd; flash_tics = ft;
	}
	string view_frame() {
		if (W_COOL <= 0 || anim.size() == 0 || cooldown <= 0) { return "A"; }
		int idx = (cooldown - W_COOL) * anim.size() / cooldown;
		if (idx >= anim.size()) { idx = anim.size() - 1; }
		return anim.substr(idx, 1);
	}
	void fire() {
		if (ammo == 1) {
			if (AMMO_BULLETS <= 0) { return; }
			AMMO_BULLETS = AMMO_BULLETS - 1;
		} else if (ammo == 2) {
			if (AMMO_SHELLS <= 0) { return; }
			AMMO_SHELLS = AMMO_SHELLS - 1;
		} else if (ammo == 3) {
			if (AMMO_ROCKETS <= 0) { return; }
			AMMO_ROCKETS = AMMO_ROCKETS - 1;
		}
		if (snd != "") { snd_play(snd); }
		float eyez = P_FLOORZ + 41.0;
		if (missile != 0) {
			int rk = rocket_spawn(P_X, P_Y, P_FLOORZ + 32.0, P_ANG, -1);   // doom's missile height (feet + 32)
			if (NP_FORWARD) { MO_NODMG[rk] = 1; }        // co-op client: my rocket is the visual, the host's is the law
		} else {
			int mul = dmg_mul;
			if (ammo == 0 && P_BERSERK) { mul = mul * 10; }   // berserk fists
			for (int p = 0; p < pellets; ++p) {
				float aim = P_ANG;
				if (spread > 0.0) { aim = aim + (SIM_RANDOM.unit() - 0.5) * spread; }
				shoot_hitscan(P_X, P_Y, eyez, aim, mul * SIM_RANDOM.between(1, dmg_roll), -1, range);
			}
		}
		W_COOL = cooldown;
		if (flash_tics > 0) { FLASH_TICS = flash_tics; }
	}
}

function weapons_init() -> void {
	if (WEAPONS.size() > 0) { return; }
	WEAPONS.push(null);
	var fist = new Weapon("fist", "PUNG", "PISF", 0, 2, 10, 1, 0.0, 64.0, 16, 0);   // fist never sets FLASH_TICS
	fist.anim = "BCDCB";
	fist.snd = "DSPUNCH";
	WEAPONS.push(fist);
	var pistol = new Weapon("pistol", "PISG", "PISF", 1, 5, 3, 1, 0.04, 2048.0, 12, 3);
	pistol.anim = "BCB";
	pistol.snd = "DSPISTOL";
	WEAPONS.push(pistol);
	var shotgun = new Weapon("shotgun", "SHTG", "SHTF", 2, 5, 3, 7, 0.20, 2048.0, 32, 4);
	shotgun.anim = "AABCDCBA";
	shotgun.snd = "DSSHOTGN";
	WEAPONS.push(shotgun);
	var chaingun = new Weapon("chaingun", "CHGG", "CHGF", 1, 5, 3, 1, 0.06, 2048.0, 4, 2);
	chaingun.anim = "AB";
	chaingun.snd = "DSPISTOL";                       // the chaingun shares the pistol's bark
	WEAPONS.push(chaingun);
	var launcher = new Weapon("rocket launcher", "MISG", "MISF", 3, 20, 8, 1, 0.0, 2048.0, 32, 4);
	launcher.anim = "BBAAAAAA";
	launcher.snd = "DSRLAUNC";
	launcher.missile = 1;
	WEAPONS.push(launcher);
}

function weapon_select(int i) -> void {
	CUR_WEAPON = i;
	var w = WEAPONS[i];
	set_msg(w.name);
}

function player_fire() -> void {
	if (W_COOL > 0 || P_HP <= 0) { return; }
	var w = WEAPONS[CUR_WEAPON];
	w.fire();
	if (NET_GAME && W_COOL > 0) { np_fire_send(); }  // cooldown set = the shot really happened
}

function pickup_touch() -> void {
	int n = MO_X.size();
	for (int m = 0; m < n; ++m) {
		if (MO_STATE[m] != 7) { continue; }
		float dx = MO_X[m] - P_X;
		float dy = MO_Y[m] - P_Y;
		float rr = P_RADIUS + MO_RAD[m];
		if (dx * dx + dy * dy >= rr * rr) { continue; }
		// co-op: the host owns every item — a client touch asks and waits
		// for the ITEM echo instead of applying (double-pickup proof)
		if (NET_GAME && NP_COOP && !NP_IS_HOST) { np_take_request(m); continue; }
		bool applied = pickup_apply(m);
		if (applied && NET_GAME && NP_COOP && NP_IS_HOST) { np_item_announce(m, true); }
	}
}

// stripped despawn for items claimed by the OTHER player (effects are theirs)
function pickup_despawn(int m) -> void {
	if (MO_STATE[m] != 7) { return; }
	MO_STATE[m] = 0;
	if (MO_CNT[m] != 0) { ITEMS_TAKEN = ITEMS_TAKEN + 1; }
}

// the whole effect switch: applies to THE LOCAL PLAYER and despawns on take
function pickup_apply(int m) -> bool {
	if (MO_STATE[m] != 7) { return false; }
	var kind = MO_SPR[m];
	bool taken = true;
		if (kind == "STIM") { if (P_HP >= 100) { taken = false; } else { P_HP = imin100(P_HP + 10); set_msg("picked up a stimpack"); } }
		else if (kind == "MEDI") { if (P_HP >= 100) { taken = false; } else { P_HP = imin100(P_HP + 25); set_msg("picked up a medikit"); } }
		else if (kind == "BON1") { if (P_HP < 200) { P_HP = P_HP + 1; } set_msg("health bonus"); }
		else if (kind == "BON2") { if (P_ARMOR < 200) { P_ARMOR = P_ARMOR + 1; } set_msg("armor bonus"); }
		else if (kind == "ARM1") { if (P_ARMOR >= 100) { taken = false; } else { P_ARMOR = 100; set_msg("picked up armor"); } }
		else if (kind == "ARM2") { if (P_ARMOR >= 200) { taken = false; } else { P_ARMOR = 200; set_msg("picked up megaarmor"); } }
		else if (kind == "CLIP") { AMMO_BULLETS = imin_ammo(AMMO_BULLETS + 10, AMMO_BULLETS_MAX); set_msg("picked up a clip"); }
		else if (kind == "AMMO") { AMMO_BULLETS = imin_ammo(AMMO_BULLETS + 50, AMMO_BULLETS_MAX); set_msg("picked up a box of bullets"); }
		else if (kind == "SHEL") { AMMO_SHELLS = imin_ammo(AMMO_SHELLS + 4, AMMO_SHELLS_MAX); set_msg("picked up shells"); }
		else if (kind == "SBOX") { AMMO_SHELLS = imin_ammo(AMMO_SHELLS + 20, AMMO_SHELLS_MAX); set_msg("picked up a box of shells"); }
		else if (kind == "ROCK") { AMMO_ROCKETS = imin_ammo(AMMO_ROCKETS + 1, AMMO_ROCKETS_MAX); set_msg("picked up a rocket"); }
		else if (kind == "BROK") { AMMO_ROCKETS = imin_ammo(AMMO_ROCKETS + 5, AMMO_ROCKETS_MAX); set_msg("picked up a box of rockets"); }
		else if (kind == "SHOT") { HAVE_SHOTGUN = true; AMMO_SHELLS = imin_ammo(AMMO_SHELLS + 8, AMMO_SHELLS_MAX); CUR_WEAPON = 3; set_msg("you got the shotgun!"); }
		else if (kind == "MGUN") { HAVE_CHAINGUN = true; AMMO_BULLETS = imin_ammo(AMMO_BULLETS + 20, AMMO_BULLETS_MAX); CUR_WEAPON = 4; set_msg("you got the chaingun!"); }
		else if (kind == "LAUN") { HAVE_LAUNCHER = true; AMMO_ROCKETS = imin_ammo(AMMO_ROCKETS + 2, AMMO_ROCKETS_MAX); CUR_WEAPON = 5; set_msg("you got the rocket launcher!"); }
		else if (kind == "BKEY") { P_KEYS = P_KEYS | 1; set_msg("picked up the blue keycard"); }
		else if (kind == "RKEY") { P_KEYS = P_KEYS | 2; set_msg("picked up the red keycard"); }
		else if (kind == "YKEY") { P_KEYS = P_KEYS | 4; set_msg("picked up the yellow keycard"); }
		else if (kind == "SOUL") { P_HP = imin_ammo(P_HP + 100, 200); set_msg("supercharge!"); }
		else if (kind == "PSTR") {
			P_BERSERK = true;
			BERSERK_TICS = 64;                           // the red surge
			P_HP = imin100(P_HP + 100);
			CUR_WEAPON = 1;
			set_msg("berserk!");
		}
		else if (kind == "PINS") { P_INVIS = 2100; set_msg("invisibility!"); }
		else if (kind == "SUIT") { P_SUIT = 2100; set_msg("radiation shielding suit"); }
		else if (kind == "PMAP") { P_ALLMAP = true; set_msg("computer area map"); }
		else if (kind == "PVIS") { P_GOGGLES = 2100; set_msg("light amplification visor"); }
		else if (kind == "BPAK") {
			if (!P_BACKPACK) {
				P_BACKPACK = true;
				AMMO_BULLETS_MAX = AMMO_BULLETS_MAX * 2;
				AMMO_SHELLS_MAX = AMMO_SHELLS_MAX * 2;
				AMMO_ROCKETS_MAX = AMMO_ROCKETS_MAX * 2;
			}
			AMMO_BULLETS = imin_ammo(AMMO_BULLETS + 10, AMMO_BULLETS_MAX);
			AMMO_SHELLS = imin_ammo(AMMO_SHELLS + 4, AMMO_SHELLS_MAX);
			AMMO_ROCKETS = imin_ammo(AMMO_ROCKETS + 1, AMMO_ROCKETS_MAX);
			set_msg("picked up a backpack full of ammo!");
		}
	else { taken = false; }
	if (taken) {
		MO_STATE[m] = 0;
		BONUS_TICS = 12;
		if (MO_CNT[m] != 0) { ITEMS_TAKEN = ITEMS_TAKEN + 1; }
		bool weapon = kind == "SHOT" || kind == "MGUN" || kind == "LAUN";
		bool power = kind == "PSTR" || kind == "PINS" || kind == "SUIT" || kind == "PMAP" || kind == "PVIS";
		if (weapon) { GRIN_TICS = 70; }              // the evil grin
		snd_play(weapon ? "DSWPNUP" : (power ? "DSGETPOW" : "DSITEMUP"));
	}
	return taken;
}

function imin100(int v) -> int { return v > 100 ? 100 : v; }
function imin_ammo(int v, int cap) -> int { return v > cap ? cap : v; }

// ------------------------------------------------------------- collision ---

function line_blocks(int ld, float x, float y) -> bool {
	int v1 = LD_V1[ld];
	int v2 = LD_V2[ld];
	float x1 = VX_X[v1];
	float y1 = VX_Y[v1];
	float dx = VX_X[v2] - x1;
	float dy = VX_Y[v2] - y1;
	float len2 = dx * dx + dy * dy;
	if (len2 < 0.001) { return false; }
	float t = ((x - x1) * dx + (y - y1) * dy) / len2;
	if (t < 0.0) { t = 0.0; }
	if (t > 1.0) { t = 1.0; }
	float cx = x1 + dx * t - x;
	float cy = y1 + dy * t - y;
	if (cx * cx + cy * cy >= P_RADIUS * P_RADIUS) { return false; }

	int s1 = LD_SIDE1[ld];
	if (s1 < 0) { return true; }                     // one-sided wall
	if ((LD_FLAGS[ld] & 1) != 0) { return true; }    // ML_BLOCKING
	int s0 = LD_SIDE0[ld];
	int f0 = SE_FLOORH[SD_SEC[s0]];
	int f1 = SE_FLOORH[SD_SEC[s1]];
	int c0 = SE_CEILH[SD_SEC[s0]];
	int c1 = SE_CEILH[SD_SEC[s1]];
	float openbottom = f0 > f1 ? f0 : f1;
	float opentop = c0 < c1 ? c0 : c1;
	if (opentop - openbottom < P_HEIGHT) { return true; }
	float feet = P_AIR ? P_Z : P_FLOORZ;             // airborne: steps measure from the arc, not the floor
	if (openbottom - feet > P_STEP) { return true; }
	return false;
}

// distance² from (x,y) to linedef ld's segment
function seg_dist2(int ld, float x, float y) -> float {
	float x1 = VX_X[LD_V1[ld]];
	float y1 = VX_Y[LD_V1[ld]];
	float dx = VX_X[LD_V2[ld]] - x1;
	float dy = VX_Y[LD_V2[ld]] - y1;
	float len2 = dx * dx + dy * dy;
	if (len2 < 0.001) { return 1000000000.0; }
	float t = ((x - x1) * dx + (y - y1) * dy) / len2;
	if (t < 0.0) { t = 0.0; }
	if (t > 1.0) { t = 1.0; }
	float cx = x1 + dx * t - x;
	float cy = y1 + dy * t - y;
	return cx * cx + cy * cy;
}

// a line the player ALREADY violates (walking off a ledge leaves its line
// inside our radius, and the step-up rule turns it blocking) must not freeze
// us: it only blocks moves that close in on it further
function line_blocks_from(int ld, float ox, float oy, float x, float y) -> bool {
	if (!line_blocks(ld, x, y)) { return false; }
	if (!line_blocks(ld, ox, oy)) { return true; }
	return seg_dist2(ld, x, y) <= seg_dist2(ld, ox, oy);
}

int BLOCK_LINE = -1;          // the line that rejected the last relative scan

function pos_ok_scan(float ox, float oy, float x, float y, bool relative) -> bool {
	BLOCK_LINE = -1;
	int bx0 = math::ifloor((x - P_RADIUS - BMAP_ORGX) / 128.0);
	int bx1 = math::ifloor((x + P_RADIUS - BMAP_ORGX) / 128.0);
	int by0 = math::ifloor((y - P_RADIUS - BMAP_ORGY) / 128.0);
	int by1 = math::ifloor((y + P_RADIUS - BMAP_ORGY) / 128.0);
	if (bx0 < 0) { bx0 = 0; }
	if (by0 < 0) { by0 = 0; }
	if (bx1 >= BMAP_W) { bx1 = BMAP_W - 1; }
	if (by1 >= BMAP_H) { by1 = BMAP_H - 1; }
	var& bm = BMAP;
	for (int cy = by0; cy <= by1; ++cy) {
		for (int cx = bx0; cx <= bx1; ++cx) {
			int i = bm[4 + cy * BMAP_W + cx] & 65535;
			if (bm[i] == 0) { i = i + 1; }           // leading zero convention
			while (bm[i] != -1) {
				int ld = bm[i] & 65535;
				if (relative) {
					if (line_blocks_from(ld, ox, oy, x, y)) { BLOCK_LINE = ld; return false; }
				} else {
					if (line_blocks(ld, x, y)) { BLOCK_LINE = ld; return false; }
				}
				i = i + 1;
			}
		}
	}
	// solid decorations (MO_RAD > 0) are obstacles; one we already overlap
	// never wedges the move — stepping OUT stays legal
	int nd = MO_X.size();
	for (int o = 0; o < nd; ++o) {
		if (MO_STATE[o] != 10 || MO_RAD[o] <= 0.0) { continue; }
		float dr = MO_RAD[o] + P_RADIUS;
		float ddx = MO_X[o] - x;
		float ddy = MO_Y[o] - y;
		if (ddx * ddx + ddy * ddy >= dr * dr) { continue; }
		if (relative) {
			float odx = MO_X[o] - ox;
			float ody = MO_Y[o] - oy;
			if (odx * odx + ody * ody < dr * dr) { continue; }
		}
		return false;
	}
	return true;
}

function pos_ok(float x, float y) -> bool { return pos_ok_scan(x, y, x, y, false); }
function pos_ok_from(float ox, float oy, float x, float y) -> bool {
	if (P_NOCLIP) { return true; }
	return pos_ok_scan(ox, oy, x, y, true);
}

function p_move() -> void {
	float ox = P_X;
	float oy = P_Y;
	float nx = P_X + P_MOMX;
	float ny = P_Y + P_MOMY;
	if (pos_ok_from(ox, oy, nx, ny)) { P_X = nx; P_Y = ny; }
	else {
		// project the FULL momentum onto the blocking wall (P_SlideMove's
		// idea): hugging a wall glides at speed instead of grinding to the
		// blocked-axis component — the secret-hunting wall-rub
		bool slid = false;
		int bl = BLOCK_LINE;
		if (bl >= 0) {
			float wdx = VX_X[LD_V2[bl]] - VX_X[LD_V1[bl]];
			float wdy = VX_Y[LD_V2[bl]] - VX_Y[LD_V1[bl]];
			float wl2 = wdx * wdx + wdy * wdy;
			if (wl2 > 0.001) {
				float dot = (P_MOMX * wdx + P_MOMY * wdy) / wl2;
				float sx2 = wdx * dot;
				float sy2 = wdy * dot;
				if ((sx2 * sx2 + sy2 * sy2) > 0.01 && pos_ok_from(ox, oy, P_X + sx2, P_Y + sy2)) {
					P_X = P_X + sx2;
					P_Y = P_Y + sy2;
					P_MOMX = sx2;
					P_MOMY = sy2;
					slid = true;
				}
			}
		}
		if (!slid) {
			if (pos_ok_from(ox, oy, nx, P_Y)) { P_X = nx; P_MOMY = 0.0; }
			else if (pos_ok_from(ox, oy, P_X, ny)) { P_Y = ny; P_MOMX = 0.0; }
			else { P_MOMX = 0.0; P_MOMY = 0.0; }
		}
	}
	P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
	if (ox != P_X || oy != P_Y) { cross_check(ox, oy, P_X, P_Y); }
}

// ------------------------------------------------------------------ doors ---

function lowest_neighbor_ceiling(int sec) -> int {
	int best = SE_CEILH[sec];
	bool found = false;
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
		if (other < 0) { continue; }
		if (!found || SE_CEILH[other] < best) { best = SE_CEILH[other]; found = true; }
	}
	return best;
}

function door_use(int sec, bool stay) -> void {
	if (mover_busy(sec)) { return; }
	int n = DOORS.size();
	for (int i = 0; i < n; ++i) {
		var d = DOORS[i];
		if (d.sec == sec) {
			if (d.state == 2) { d.state = 0; snd_sector(sec, "DSDOROPN"); }   // closing -> reopen
			return;
		}
	}
	DOORS.push(new Door(sec, 0, stay ? 1 : 0));
	snd_sector(sec, "DSDOROPN");
}

function door_close(int sec) -> void {
	if (mover_busy(sec)) { return; }
	int n = DOORS.size();
	for (int i = 0; i < n; ++i) {
		var d = DOORS[i];
		if (d.sec == sec) { d.state = 2; return; }
	}
	DOORS.push(new Door(sec, 2, 0));
	snd_sector(sec, "DSDORCLS");
}

function door_busy(int sec) -> bool {
	int n = DOORS.size();
	for (int i = 0; i < n; ++i) {
		var d = DOORS[i];
		if (d.sec == sec) { return true; }
	}
	return false;
}

function doors_tic() -> void {
	int i = 0;
	while (i < DOORS.size()) {
		var d = DOORS[i];
		if (d.tic()) { DOORS.remove(i); }
		else { i = i + 1; }
	}
}

// -------------------------------------------------------------------- use ---

// nearest special line the use ray (64 units) crosses
function p_use() -> void {
	// co-op: the host owns doors/movers/exits — a client press travels as a
	// USE action and the host replays it at the client's pose
	if (NET_GAME && NP_COOP && !NP_IS_HOST) { np_use_send(); return; }
	float ux = math::cos(P_ANG);
	float uy = math::sin(P_ANG);
	float ex = P_X + ux * 64.0;
	float ey = P_Y + uy * 64.0;
	int nlines = LD_V1.size();
	float best_t = 2.0;
	int best = -1;
	for (int i = 0; i < nlines; ++i) {
		float x1 = VX_X[LD_V1[i]];
		float y1 = VX_Y[LD_V1[i]];
		float x2 = VX_X[LD_V2[i]];
		float y2 = VX_Y[LD_V2[i]];
		float d = (x2 - x1) * (ey - P_Y) - (y2 - y1) * (ex - P_X);
		if (d > -0.0001 && d < 0.0001) { continue; }
		float t = ((P_X - x1) * (ey - P_Y) - (P_Y - y1) * (ex - P_X)) / d;
		float s = ((P_X - x1) * (y2 - y1) - (P_Y - y1) * (x2 - x1)) / d;
		if (t < 0.0 || t > 1.0 || s < 0.0 || s > 1.0) { continue; }
		if (s < best_t) {
			// nearest crossing that either does something or stops the ray
			if (LD_SPECIAL[i] != 0 || LD_SIDE1[i] < 0) {
				best_t = s;
				best = i;
			}
		}
	}
	if (best < 0) { return; }
	int special = LD_SPECIAL[best];
	int s1 = LD_SIDE1[best];
	bool used = false;
	if (special == 1) {
		if (s1 >= 0) { door_use(SD_SEC[s1], false); used = true; }
	} else if (special == 26 || special == 27 || special == 28) {
		int need = special == 26 ? 1 : (special == 28 ? 2 : 4);
		var keyname = special == 26 ? "blue" : (special == 28 ? "red" : "yellow");
		if ((P_KEYS & need) == 0) { set_msg(`you need a ${keyname} key`); }
		else if (s1 >= 0) { door_use(SD_SEC[s1], false); used = true; }
	} else if (special == 31) {
		if (s1 >= 0) { door_use(SD_SEC[s1], true); used = true; }
	} else if (special == 32 || special == 33 || special == 34) {
		int need2 = special == 32 ? 1 : (special == 33 ? 2 : 4);
		var keyname2 = special == 32 ? "blue" : (special == 33 ? "red" : "yellow");
		if ((P_KEYS & need2) == 0) { set_msg(`you need a ${keyname2} key`); }
		else if (s1 >= 0) { door_use(SD_SEC[s1], true); used = true; }
	} else if (special == 11) {
		snd_play("DSSWTCHN");
		LEVEL_DONE = true;
		used = true;
	} else if (special == 51) {
		snd_play("DSSWTCHN");
		LEVEL_DONE = true;
		SECRET_EXIT = true;
		used = true;
	} else if (special != 0) {
		if (spec_line(best, 0)) { used = true; }
		else if (!spec_line_known(special)) {
			set_msg(`special ${special} not wired up yet`);
		}
	}
	if (used) {
		switch_use(best);                            // press the switch face in
		if (NET_GAME && NP_COOP && NP_IS_HOST) { np_switch_announce(best); }
	}
}

function mapxx(int n) -> string {
	if (n < 10) { return `MAP0${n}`; }
	return `MAP${n}`;
}

// After the standard successor: the next map the loaded WADs actually contain (a PWAD
// with MAP01 and MAP03 goes 01 -> 03), or the end when nothing follows
function next_existing_map(string after) -> string {
	var maps = map_list();
	for (auto m : maps) { if (m > after) { return m; } }
	return "";
}

function next_map() -> string {
	string n = next_map_standard();
	if (n != "" && lump_num(n) < 0) { n = next_existing_map(GAME_MAP); }
	return n;
}

function next_map_standard() -> string {
	if (GAME_MAP.starts_with("MAP")) {                       // doom2 chain: 15->31->32->16, 30 ends it
		int mn = GAME_MAP.substr(3, 2).to_int(1);
		if (SECRET_EXIT) { return mn == 31 ? "MAP32" : "MAP31"; }
		if (mn == 30) { return ""; }
		if (mn == 31 || mn == 32) { return "MAP16"; }
		return mapxx(mn + 1);
	}
	if (SECRET_EXIT) { return `E${GAME_MAP.substr(1, 1)}M9`; }
	int e = GAME_MAP.substr(1, 1).to_int(1);
	int m = GAME_MAP.substr(3, 1).to_int(1);
	if (m == 9) { return `E${e}M4`; }                        // secret level returns
	if (m == 8) { return ""; }                               // episode end
	return `E${e}M${m + 1}`;
}

// -------------------------------------------------------------------- tic ---

function game_tic() -> void {
	if (DEMO_REC) { demo_note(); }
	GAME_TIC = GAME_TIC + 1;
	P_ANG = P_ANG + CMD_TURN;
	if (P_ANG > math::TAU) { P_ANG = P_ANG - math::TAU; }
	if (P_ANG < 0.0) { P_ANG = P_ANG + math::TAU; }

	// P_Thrust: move * 2048/65536 per tic, then friction 0.90625 — ground
	// only, doom gives no air control and no air friction
	if (!P_AIR) {
		float fc = math::cos(P_ANG);
		float fs = math::sin(P_ANG);
		P_MOMX = P_MOMX + (CMD_FWD * fc + CMD_SIDE * fs) * 0.03125;
		P_MOMY = P_MOMY + (CMD_FWD * fs - CMD_SIDE * fc) * 0.03125;
	}
	p_move();
	if (!P_AIR) {
		P_MOMX = P_MOMX * 0.90625;
		P_MOMY = P_MOMY * 0.90625;
	}

	// real z: rising floors and step-ups snap the feet; walking off a ledge
	// arcs down under gravity (1/tic²), hard landings dip the view and thud
	if (P_Z < P_FLOORZ) {
		P_Z = P_FLOORZ * 1.0;
		if (P_MOMZ < 0.0) { P_MOMZ = 0.0; }
	} else if (P_Z > P_FLOORZ) {
		if (!P_AIR && P_Z - P_FLOORZ <= 4.5) { P_Z = P_FLOORZ * 1.0; }   // a descending mover carries its rider
		else {
			if (!P_AIR) { P_AIR = true; P_AIRZ0 = P_Z; P_MOMZ = 0.0; }
			P_MOMZ = P_MOMZ - 1.0;
			P_Z = P_Z + P_MOMZ;
			if (P_Z <= P_FLOORZ) {
				P_Z = P_FLOORZ * 1.0;
				P_AIR = false;
				P_MOMZ = 0.0;
				if (P_AIRZ0 - P_Z > 8.0) {
					P_VIEWZ = P_Z + 41.0 - 6.0;
					snd_play("DSOOF");
				}
			}
		}
	} else {
		P_AIR = false;
		P_MOMZ = 0.0;
	}

	float vt = P_Z + 41.0;
	if (P_VIEWZ < vt) { P_VIEWZ = P_VIEWZ + 8.0; if (P_VIEWZ > vt) { P_VIEWZ = vt; } }
	else if (P_VIEWZ > vt) { P_VIEWZ = P_VIEWZ - 8.0; if (P_VIEWZ < vt) { P_VIEWZ = vt; } }

	// weapon bob: momentum^2 / 4 capped at 16 (doom's MAXBOB), ~64-tic cycle
	float bob = (P_MOMX * P_MOMX + P_MOMY * P_MOMY) * 0.25;
	if (bob > 16.0) { bob = 16.0; }
	float ba = GAME_TIC * 0.098;
	WBOB_X = bob * 0.5 * math::cos(ba);
	float bs = math::sin(ba);
	if (bs < 0.0) { bs = -bs; }
	WBOB_Y = bob * 0.5 * bs;

	if (CMD_USE) {
		if (!USE_LATCH) { p_use(); USE_LATCH = true; }
	} else { USE_LATCH = false; }
	if (CMD_FIRE) { player_fire(); FIRE_HELD = FIRE_HELD + 1; }
	else { FIRE_HELD = 0; }
	if (W_COOL > 0) { W_COOL = W_COOL - 1; }
	if (FLASH_TICS > 0) { FLASH_TICS = FLASH_TICS - 1; }
	if (PAIN_TICS > 0) { PAIN_TICS = PAIN_TICS - 1; }
	if (BONUS_TICS > 0) { BONUS_TICS = BONUS_TICS - 1; }
	if (GRIN_TICS > 0) { GRIN_TICS = GRIN_TICS - 1; }
	if (BERSERK_TICS > 0) { BERSERK_TICS = BERSERK_TICS - 1; }
	if (P_INVIS > 0) {
		P_INVIS = P_INVIS - 1;
		if (P_INVIS == 0) { set_msg("you feel visible again"); }
		else if (P_INVIS < 140 && P_INVIS % 35 == 0) { set_msg("invisibility is wearing off"); }
	}
	if (P_SUIT > 0) {
		P_SUIT = P_SUIT - 1;
		if (P_SUIT == 0) { set_msg("the suit dissolves"); }
		else if (P_SUIT < 140 && P_SUIT % 35 == 0) { set_msg("the suit is wearing off"); }
	}
	if (P_GOGGLES > 0) { P_GOGGLES = P_GOGGLES - 1; }
	pickup_touch();
	things_tic();

	if (NET_GAME && NP_COOP && !NP_IS_HOST) {
		spec_tic();                                  // client: lights + own floor damage only
	} else {                                         // (doors/movers/timers are host authority,
		doors_tic();                                 // heights arrive via the SECT stream)
		spec_tic();
	}
	anim_tic();
	if (NET_GAME) { np_tic(); }
	if (MSG_TIMER > 0) { MSG_TIMER = MSG_TIMER - 1; }
}
