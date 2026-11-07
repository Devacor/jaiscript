// JaiDOOM demo.jai — the determinism harness: a demo is { seed, map, skill,
// nightmare, tics } where tics is one flat 6-int tuple per game_tic
// (CMD_FWD/SIDE/TURN quantized x10000, USE, FIRE, CUR_WEAPON). Recording
// quantizes the live CMDs IN PLACE so the recorded run consumes exactly what
// playback will feed; SIM_RANDOM.seed pins the sim's generator at both ends and
// GAME_TIC restarts at 0 (floor damage phases on GAME_TIC & 31). All sim
// randomness flows through SIM_RANDOM — the HUD face rolls its own LCG so
// presentation never perturbs the stream. game.jai globals (CMD_*, P_*,
// GAME_TIC) resolve at call time — importing game.jai here would cycle.

import "sim_random.jai";
import "map.jai";
import "things.jai";

bool DEMO_REC = false;
bool DEMO_PLAY = false;
array<int> DEMO_TICS = [];
int DEMO_POS = 0;
int DEMO_SEED = 0;
string DEMO_MAP = "";
int DEMO_SKILL = 2;
bool DEMO_NM = false;

function demo_q(float v) -> int {
	return math::itrunc(v * 10000.0 + (v < 0.0 ? -0.5 : 0.5));
}

// call BEFORE game_start_map: the seed must cover map-start randomness
// (mobj tic stagger, light phases)
function demo_rec_begin(string mapname, int skill, int seed) -> void {
	DEMO_SEED = seed;
	DEMO_MAP = mapname;
	DEMO_SKILL = skill;
	DEMO_NM = NIGHTMARE;
	DEMO_TICS = [];
	DEMO_REC = true;
	GAME_TIC = 0;
	SIM_RANDOM.seed(seed);
}

// one tuple per tic; the quantized values are written back so record == replay
function demo_note() -> void {
	int qf = demo_q(CMD_FWD);
	int qs = demo_q(CMD_SIDE);
	int qt = demo_q(CMD_TURN);
	DEMO_TICS.push(qf);
	DEMO_TICS.push(qs);
	DEMO_TICS.push(qt);
	DEMO_TICS.push(CMD_USE ? 1 : 0);
	DEMO_TICS.push(CMD_FIRE ? 1 : 0);
	DEMO_TICS.push(CUR_WEAPON);
	CMD_FWD = qf / 10000.0;
	CMD_SIDE = qs / 10000.0;
	CMD_TURN = qt / 10000.0;
}

// next tuple -> CMDs; false (and playback off) when the demo runs out
function demo_feed() -> bool {
	if (DEMO_POS + 6 > DEMO_TICS.size()) { DEMO_PLAY = false; return false; }
	CMD_FWD = DEMO_TICS[DEMO_POS] / 10000.0;
	CMD_SIDE = DEMO_TICS[DEMO_POS + 1] / 10000.0;
	CMD_TURN = DEMO_TICS[DEMO_POS + 2] / 10000.0;
	CMD_USE = DEMO_TICS[DEMO_POS + 3] != 0;
	CMD_FIRE = DEMO_TICS[DEMO_POS + 4] != 0;
	CUR_WEAPON = DEMO_TICS[DEMO_POS + 5];
	DEMO_POS = DEMO_POS + 6;
	return true;
}

function demo_write(string path) -> bool {
	path = data_path(path);
	var d = {"seed": DEMO_SEED, "mapname": DEMO_MAP, "skill": DEMO_SKILL,
		"nightmare": DEMO_NM, "tics": DEMO_TICS};
	write_file(path, to_json(d));
	return file_exists(path);
}

function demo_load(string path) -> bool {
	path = data_path(path);
	if (!file_exists(path)) { return false; }
	var txt = read_file(path);
	if (txt.size() < 2) { return false; }
	var d = from_json(txt);
	if (d == null || !d.has("tics") || !d.has("seed") || !d.has("mapname")) { return false; }
	DEMO_SEED = d["seed"] + 0;
	DEMO_MAP = "" + d["mapname"];
	DEMO_SKILL = d["skill"] + 0;
	DEMO_NM = d.has("nightmare") && d["nightmare"] == true;
	DEMO_TICS = [];
	var& ts = d["tics"];
	int n = ts.size();
	for (int i = 0; i < n; ++i) { DEMO_TICS.push(math::itrunc(ts[i] * 1.0)); }
	DEMO_POS = 0;
	return true;
}

// mix32-style fold over the sim fingerprint: tic, player, every mobj, sector
// heights — two runs of the same demo must agree exactly
int HASH_H = 0;

function hfold(int v) -> void {
	HASH_H = ((HASH_H ^ (v & 4294967295)) * 16777619) & 4294967295;
}

function demo_state_hash() -> int {
	HASH_H = 2166136261;
	hfold(GAME_TIC);
	hfold(math::itrunc(P_X * 256.0));
	hfold(math::itrunc(P_Y * 256.0));
	hfold(math::itrunc(P_ANG * 100000.0));
	hfold(P_HP);
	hfold(P_ARMOR);
	hfold(KILLS);
	hfold(AMMO_BULLETS);
	int n = MO_X.size();
	for (int m = 0; m < n; ++m) {
		hfold(math::itrunc(MO_X[m] * 256.0));
		hfold(math::itrunc(MO_Y[m] * 256.0));
		hfold(MO_STATE[m]);
		hfold(MO_HP[m]);
	}
	int ns = SE_FLOORH.size();
	for (int s = 0; s < ns; ++s) {
		hfold(SE_FLOORH[s]);
		hfold(SE_CEILH[s]);
	}
	return HASH_H;
}
