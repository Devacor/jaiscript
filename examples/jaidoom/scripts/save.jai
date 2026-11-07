// JaiDOOM save.jai — the whole live sim as one JSON document: the flat MO_*
// pool and sector/line arrays serialize wholesale, doors/movers as compact
// descriptors (a reloaded mover re-runs its lifetime from the restored
// height). Load rebuilds the map through game_start_map FIRST (render caches,
// fresh spawns), then overwrites the sim on top; monster brains are handles
// and cannot cross a save — they reload null and monster_tic re-mints them.
// Loaded JSON is walked by reference (var& / chained reads only).

import "wad.jai";
import "map.jai";
import "tex.jai";
import "render.jai";
import "things.jai";
import "spec.jai";
import "anim.jai";
import "game.jai";

var SAVE_PATH = "jaidoom_save.json";
var SAVE_VERSION = 1;

function game_save(string path) -> bool {
	path = data_path(path);
	if (GAME_MAP == "") { return false; }
	var doors = [];
	for (int i = 0; i < DOORS.size(); ++i) {
		var d = DOORS[i];
		doors.push([d.sec, d.state, d.timer, d.top, d.stay]);
	}
	var movers = [];
	for (int j = 0; j < MOVERS.size(); ++j) {
		var mv = MOVERS[j];
		movers.push([mv.sec, mv.plane, mv.target, mv.speed, mv.wait, mv.ret]);
	}
	var st = {
		"version": SAVE_VERSION,
		"mapname": GAME_MAP,
		"skill": SKILL_BIT,
		"nightmare": NIGHTMARE,
		"fast": FAST_MONSTERS,
		"tic": GAME_TIC,
		"level_tic": LEVEL_START_TIC,
		"spec_tic0": SPEC_TIC0,
		"player": {
			"x": P_X, "y": P_Y, "z": P_Z, "ang": P_ANG,
			"momx": P_MOMX, "momy": P_MOMY, "momz": P_MOMZ,
			"floorz": P_FLOORZ * 1.0, "viewz": P_VIEWZ * 1.0, "air": P_AIR,
			"hp": P_HP, "armor": P_ARMOR, "keys": P_KEYS,
			"bullets": AMMO_BULLETS, "shells": AMMO_SHELLS, "rockets": AMMO_ROCKETS,
			"bmax": AMMO_BULLETS_MAX, "smax": AMMO_SHELLS_MAX, "rmax": AMMO_ROCKETS_MAX,
			"shotgun": HAVE_SHOTGUN, "chaingun": HAVE_CHAINGUN, "launcher": HAVE_LAUNCHER,
			"berserk": P_BERSERK, "invis": P_INVIS, "suit": P_SUIT, "goggles": P_GOGGLES,
			"allmap": P_ALLMAP, "backpack": P_BACKPACK,
			"weapon": CUR_WEAPON, "wcool": W_COOL,
			"god": P_GOD, "noclip": P_NOCLIP
		},
		"mo": {
			"x": MO_X, "y": MO_Y, "z": MO_Z, "ang": MO_ANG,
			"momx": MO_MOMX, "momy": MO_MOMY, "momz": MO_MOMZ, "rad": MO_RAD,
			"type": MO_TYPE, "state": MO_STATE, "hp": MO_HP, "frame": MO_FRAME,
			"tic": MO_TIC, "cool": MO_COOL, "target": MO_TARGET, "charge": MO_CHARGE,
			"cnt": MO_CNT, "shp": MO_SHP, "resp": MO_RESP, "anim": MO_ANIM, "spr": MO_SPR
		},
		"sec": {
			"floorh": SE_FLOORH, "ceilh": SE_CEILH, "light": SE_LIGHT,
			"special": SE_SPECIAL, "floorpic": SE_FLOORPIC, "ffslot": SEC_FFLAT
		},
		"ld_special": LD_SPECIAL,
		"seg": {"mid": SEG_MIDT, "top": SEG_TOPT, "bot": SEG_BOTT},
		"doors": doors,
		"movers": movers,
		"spec": {"secrets": SECRETS_FOUND, "secret_total": SECRET_TOTAL, "boss": BOSS_PEND},
		"anim": {"phase": ANIM_PHASE, "tick": ANIM_TICK, "btn_ld": BTN_LD, "btn_tics": BTN_TICS},
		"stats": {"kills": KILLS, "kill_total": KILL_TOTAL, "items": ITEMS_TAKEN, "item_total": ITEM_TOTAL}
	};
	write_file(path, to_json(st));
	return file_exists(path);
}

function ld_farr(var& src) -> array<float> {
	array<float> out = [];
	int n = src.size();
	for (int i = 0; i < n; ++i) { out.push(src[i] * 1.0); }
	return out;
}

function ld_iarr(var& src) -> array<int> {
	array<int> out = [];
	int n = src.size();
	for (int i = 0; i < n; ++i) { out.push(math::itrunc(src[i] * 1.0)); }
	return out;
}

function ld_sarr(var& src) -> array<string> {
	array<string> out = [];
	int n = src.size();
	for (int i = 0; i < n; ++i) { out.push("" + src[i]); }
	return out;
}

// restore a switch-face texture column, preparing ids the fresh map lacks
function seg_tex_restore(var& saved, var& live) -> void {
	int n = live.size();
	if (saved.size() != n) { return; }
	for (int i = 0; i < n; ++i) {
		int t = math::itrunc(saved[i] * 1.0);
		if (t == live[i]) { continue; }
		live[i] = t;
		if (t >= 0) { tex_prepare(t); }
	}
}

function game_load(string path) -> bool {
	path = data_path(path);
	if (!file_exists(path)) { return false; }
	var txt = read_file(path);
	if (txt.size() < 2) { return false; }
	var st = from_json(txt);
	if (st == null || !st.has("version") || st["version"] + 0 != SAVE_VERSION) { return false; }
	SKILL_BIT = st["skill"] + 0;
	NIGHTMARE = st.has("nightmare") && st["nightmare"] == true;
	FAST_MONSTERS = st.has("fast") && st["fast"] == true;
	if (!game_start_map("" + st["mapname"])) { return false; }

	GAME_TIC = st["tic"] + 0;
	LEVEL_START_TIC = st["level_tic"] + 0;
	SPEC_TIC0 = st["spec_tic0"] + 0;

	var& p = st["player"];
	P_X = p["x"] * 1.0;
	P_Y = p["y"] * 1.0;
	P_Z = p["z"] * 1.0;
	P_ANG = p["ang"] * 1.0;
	P_MOMX = p["momx"] * 1.0;
	P_MOMY = p["momy"] * 1.0;
	P_MOMZ = p["momz"] * 1.0;
	P_FLOORZ = p["floorz"] * 1.0;
	P_VIEWZ = p["viewz"] * 1.0;
	P_AIR = p["air"] == true;
	P_HP = p["hp"] + 0;
	P_ARMOR = p["armor"] + 0;
	P_KEYS = p["keys"] + 0;
	AMMO_BULLETS = p["bullets"] + 0;
	AMMO_SHELLS = p["shells"] + 0;
	AMMO_ROCKETS = p["rockets"] + 0;
	AMMO_BULLETS_MAX = p["bmax"] + 0;
	AMMO_SHELLS_MAX = p["smax"] + 0;
	AMMO_ROCKETS_MAX = p["rmax"] + 0;
	HAVE_SHOTGUN = p["shotgun"] == true;
	HAVE_CHAINGUN = p["chaingun"] == true;
	HAVE_LAUNCHER = p["launcher"] == true;
	P_BERSERK = p["berserk"] == true;
	P_INVIS = p["invis"] + 0;
	P_SUIT = p["suit"] + 0;
	P_GOGGLES = p["goggles"] + 0;
	P_ALLMAP = p["allmap"] == true;
	P_BACKPACK = p["backpack"] == true;
	CUR_WEAPON = p["weapon"] + 0;
	W_COOL = p["wcool"] + 0;
	P_GOD = p["god"] == true;
	P_NOCLIP = p["noclip"] == true;

	var& mo = st["mo"];
	MO_X = ld_farr(mo["x"]);
	MO_Y = ld_farr(mo["y"]);
	MO_Z = ld_farr(mo["z"]);
	MO_ANG = ld_farr(mo["ang"]);
	MO_MOMX = ld_farr(mo["momx"]);
	MO_MOMY = ld_farr(mo["momy"]);
	MO_MOMZ = ld_farr(mo["momz"]);
	MO_RAD = ld_farr(mo["rad"]);
	MO_TYPE = ld_iarr(mo["type"]);
	MO_STATE = ld_iarr(mo["state"]);
	MO_HP = ld_iarr(mo["hp"]);
	MO_FRAME = ld_iarr(mo["frame"]);
	MO_TIC = ld_iarr(mo["tic"]);
	MO_COOL = ld_iarr(mo["cool"]);
	MO_TARGET = ld_iarr(mo["target"]);
	MO_CHARGE = ld_iarr(mo["charge"]);
	MO_CNT = ld_iarr(mo["cnt"]);
	MO_SHP = ld_iarr(mo["shp"]);
	MO_ANIM = ld_sarr(mo["anim"]);
	MO_SPR = ld_sarr(mo["spr"]);
	MO_BRAIN = [];
	int nmo = MO_X.size();
	for (int b = 0; b < nmo; ++b) { MO_BRAIN.push(null); }
	MO_RESP = [];
	if (mo.has("resp")) { MO_RESP = ld_iarr(mo["resp"]); }
	else { for (int r = 0; r < nmo; ++r) { MO_RESP.push(0); } }

	var& sec = st["sec"];
	SE_FLOORH = ld_iarr(sec["floorh"]);
	SE_CEILH = ld_iarr(sec["ceilh"]);
	SE_LIGHT = ld_iarr(sec["light"]);
	SE_SPECIAL = ld_iarr(sec["special"]);
	SE_FLOORPIC = ld_sarr(sec["floorpic"]);
	SEC_FFLAT = ld_iarr(sec["ffslot"]);
	LD_SPECIAL = ld_iarr(st["ld_special"]);

	var& seg = st["seg"];
	seg_tex_restore(seg["mid"], SEG_MIDT);
	seg_tex_restore(seg["top"], SEG_TOPT);
	seg_tex_restore(seg["bot"], SEG_BOTT);

	DOORS = [];
	var& ds = st["doors"];
	for (int di = 0; di < ds.size(); ++di) {
		var& rec = ds[di];
		var d = new Door(rec[0] + 0, rec[1] + 0, rec[4] + 0);
		d.timer = rec[2] + 0;
		d.top = rec[3] + 0;
		DOORS.push(d);
	}
	MV_SEC = [];
	MOVERS = [];
	var& ms = st["movers"];
	for (int mi = 0; mi < ms.size(); ++mi) {
		var& mrec = ms[mi];
		mover_start(mrec[0] + 0, mrec[1] + 0, mrec[2] + 0, mrec[3] + 0, mrec[4] + 0, mrec[5] + 0);
	}

	var& sp = st["spec"];
	SECRETS_FOUND = sp["secrets"] + 0;
	SECRET_TOTAL = sp["secret_total"] + 0;
	BOSS_PEND = sp["boss"] + 0;
	var& an = st["anim"];
	ANIM_PHASE = an["phase"] + 0;
	ANIM_TICK = an["tick"] + 0;
	BTN_LD = ld_iarr(an["btn_ld"]);
	BTN_TICS = ld_iarr(an["btn_tics"]);
	var& stats = st["stats"];
	KILLS = stats["kills"] + 0;
	KILL_TOTAL = stats["kill_total"] + 0;
	ITEMS_TAKEN = stats["items"] + 0;
	ITEM_TOTAL = stats["item_total"] + 0;
	return true;
}
