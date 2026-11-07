// P9 gate: N-player netplay, headless in one process — the REAL HostSession on
// its NetServer vs THREE fake clients speaking raw protocol JSON over real
// loopback sockets. Proves: slot assignment + roster, GO start, relay fan-out
// (A's STATE reaches B and C, never A), DAMAGE victim-routing, frag table +
// scoreboard, JOINED/LEFT puppet lifecycle, suicide scoring, the DM client
// EXIT -> NEXT-for-everyone door; then co-op with two fakes: MONS streams,
// puppet attack/pain anim mapping (apply + wire), client FIRE kills a real
// monster + aggros the shooter's puppet, USE opens a real door + SECT carries
// it, TAKE double-claim refusal, monster attack -> routed DAMAGE, NEXT/READY
// x2/START with the fresh recount, 10s grace, and a mid-intermission leaver;
// finally the INVERTED harness — a REAL ClientSession vs a scripted fake host
// with ~150ms-late deliveries: walk-signature alignment, the EXIT latch, no
// self-advance on NEXT, READY up, START travel, BYE teardown.

import "wad.jai";
import "map.jai";
import "vid.jai";
import "render.jai";
import "things.jai";
import "game.jai";
import "hud.jai";
import "netplay.jai";

var FAILS = 0;
function check(bool ok, string what) -> void {
	if (ok) { print(`  ok    ${what}`); }
	else { FAILS = FAILS + 1; print(`  FAIL  ${what}`); }
}

// three fake peers, flat per-peer packet logs (nested pushes from receivers
// don't land — the net_smoke lesson)
var FA = null;
var FB = null;
var FC = null;
var GA = [];
var GB = [];
var GC = [];

// the client-side section's fake HOST (a raw NetServer speaking protocol JSON)
var FH = null;
var FH_CID = -1;
var HSIG = 0;                 // the host's map-placed walk signature (index alignment)

function pump2(int times) -> void {
	for (int i = 0; i < times; ++i) {
		np_frame();
		if (FH != null) { FH.pump(); }
		sleep(0.003);
	}
}

// fold of the map-placed pool: types in walk order. Host and co-op client
// MUST agree bit-exactly — every MONS/BOOM/TAKE address rides these indices.
// (NP_MAP_MOBJS bounds the shared range: ghosts past it differ per role.)
function map_sig() -> int {
	int sig = 0;
	int n = NP_MAP_MOBJS < MO_TYPE.size() ? NP_MAP_MOBJS : MO_TYPE.size();
	for (int m = 0; m < n; ++m) { sig = sig + MO_TYPE[m] * (m % 89 + 1); }
	return sig * 31 + n;
}

function fake(int which) -> var {
	var c = new NetChannel();
	if (which == 0) { c.on_packet.connect("g", [](var b) { GA.push(b); }); }
	else if (which == 1) { c.on_packet.connect("g", [](var b) { GB.push(b); }); }
	else { c.on_packet.connect("g", [](var b) { GC.push(b); }); }
	return c;
}

function pump_all(int times) -> void {
	for (int i = 0; i < times; ++i) {
		np_frame();                                   // pumps the host session
		if (FA != null) { FA.pump(); }
		if (FB != null) { FB.pump(); }
		if (FC != null) { FC.pump(); }
		sleep(0.003);
	}
}

function log_count(var& log, string t) -> int {
	int n = 0;
	for (int i = 0; i < log.size(); ++i) {
		var m = from_json(log[i]);
		if ("" + m["t"] == t) { n = n + 1; }
	}
	return n;
}

function log_last(var& log, string t) -> var {
	for (int i = log.size() - 1; i >= 0; --i) {
		var m = from_json(log[i]);
		if ("" + m["t"] == t) { return m; }
	}
	return null;
}

function hello_from(var c, string name) -> void {
	c.send(to_json({"t": "HELLO", "proto": NP_PROTO, "wad": WAD.size(), "name": name}));
}

function main() -> int {
	var path = wad_path("doom1");
	if (!wad_load(path)) { print("wad_load failed"); return 1; }
	vid_init(320, 200);
	hud_init();
	SKILL_BIT = 2;
	NP_SOLO_OK = true;                                // gate stages peers leaving; keep the session up

	// ---- lobby: three fakes join, slots 1..3, roster + JOINED relays ----
	check(np_lobby_host(28956, false, "E1M1", 2), "host lobby opens");
	check(NP_LOBBY == "listening" && !NET_GAME, "lobby listens before any game exists");
	FA = fake(0);
	FB = fake(1);
	FC = fake(2);
	check(FA.connect("127.0.0.1", 28956) && FB.connect("127.0.0.1", 28956) && FC.connect("127.0.0.1", 28956), "three fakes connect");
	pump_all(60);
	hello_from(FA, "alpha");
	pump_all(30);
	hello_from(FB, "bravo");
	pump_all(30);
	hello_from(FC, "chuck");
	pump_all(30);
	var wa = log_last(GA, "WELCOME");
	var wb = log_last(GB, "WELCOME");
	var wc = log_last(GC, "WELCOME");
	check(wa != null && math::itrunc(wa["slot"] * 1.0) == 1, "alpha got slot 1");
	check(wb != null && math::itrunc(wb["slot"] * 1.0) == 2, "bravo got slot 2");
	check(wc != null && math::itrunc(wc["slot"] * 1.0) == 3, "chuck got slot 3");
	check(("" + wc["roster"]).contains("1:alpha") && ("" + wc["roster"]).contains("2:bravo"), "late roster carries the early joiners");
	check(log_count(GA, "JOINED") == 2 && log_count(GB, "JOINED") == 1, "JOINED relayed to earlier joiners only");
	check(np_lobby_count() == 3, `lobby counts 3 (${np_lobby_count()})`);

	// ---- GO: the host launches; everyone gets the start order ----
	np_lobby_go();
	pump_all(40);
	check(NET_GAME && !NP_COOP && NP_IS_HOST, "host entered the deathmatch");
	check(log_count(GA, "GO") == 1 && log_count(GB, "GO") == 1 && log_count(GC, "GO") == 1, "GO broadcast to all");
	check(SESSION.players[1] != null && SESSION.players[2] != null && SESSION.players[3] != null, "puppets exist for all slots");
	var p1 = SESSION.players[1];
	check(p1.ghost >= 0 && MO_STATE[p1.ghost] == 11, "slot-1 puppet has a state-11 ghost");

	// ---- relay fan-out: alpha's STATE reaches bravo+chuck, never alpha ----
	int a_states0 = log_count(GA, "STATE");
	int b_states0 = log_count(GB, "STATE");
	FA.send(to_json({"t": "STATE", "s": 1, "x": 1200.0, "y": -3300.0, "z": 0.0, "ang": 1.0, "weapon": 2, "alive": true}));
	pump_all(20);
	check(log_count(GB, "STATE") > b_states0, "alpha's STATE relayed to bravo");
	check(log_count(GC, "STATE") > 0, "alpha's STATE relayed to chuck");
	// (alpha also receives the HOST's own state cadence, so compare kinds: no
	// relayed copy of its OWN packet means no s:1 STATE in its log)
	bool a_saw_self = false;
	for (int i = a_states0; i < GA.size(); ++i) {
		var m = from_json(GA[i]);
		if ("" + m["t"] == "STATE" && math::itrunc(m["s"] * 1.0) == 1) { a_saw_self = true; }
	}
	check(!a_saw_self, "alpha never hears its own STATE back");
	pump_all(10);
	float gd = math::distance(MO_X[p1.ghost], MO_Y[p1.ghost], 1200.0, -3300.0);
	check(gd < 60.0, `slot-1 ghost lerped toward the STATE (${gd:.0} away)`);

	// ---- DAMAGE routing: bravo shoots the host (v:0) and chuck (v:3) ----
	P_HP = 100;
	P_ARMOR = 0;
	FB.send(to_json({"t": "DAMAGE", "s": 2, "v": 0, "amount": 7, "sx": P_X + 50.0, "sy": P_Y}));
	pump_all(20);
	check(P_HP == 93, `host took the v:0 DAMAGE (${P_HP})`);
	int c_dmg0 = log_count(GC, "DAMAGE");
	FB.send(to_json({"t": "DAMAGE", "s": 2, "v": 3, "amount": 5, "sx": 0.0, "sy": 0.0}));
	pump_all(20);
	check(log_count(GC, "DAMAGE") == c_dmg0 + 1, "v:3 DAMAGE routed to chuck");
	check(log_count(GA, "DAMAGE") == 0, "alpha never saw either DAMAGE");

	// ---- dm rockets are visible: a client PROJ spawns a host twin + relays ----
	int mo_before = MO_X.size();
	int b_proj0 = log_count(GB, "PROJ");
	FA.send(to_json({"t": "PROJ", "s": 1, "id": 7, "spr": "MISL", "snd": "",
		"x": 1200.0, "y": -3300.0, "z": 32.0, "mx": 25.0, "my": 0.0, "mz": 0.0}));
	pump_all(20);
	check(log_count(GB, "PROJ") == b_proj0 + 1 && log_count(GC, "PROJ") > 0, "PROJ relayed to the others");
	bool a_proj_echo = false;
	for (int pe = 0; pe < GA.size(); ++pe) {
		var pm = from_json(GA[pe]);
		if ("" + pm["t"] == "PROJ" && math::itrunc(pm["s"] * 1.0) == 1) { a_proj_echo = true; }
	}
	check(!a_proj_echo, "the shooter never hears its own PROJ back");
	check(MO_X.size() == mo_before + 1 && MO_NODMG[mo_before] == 2 && MO_STATE[mo_before] == 8,
		"host spawned a streamed no-damage twin");
	FA.send(to_json({"t": "EXPL", "s": 1, "id": 7, "x": 1300.0, "y": -3300.0, "z": 32.0}));
	pump_all(20);
	check(MO_STATE[mo_before] == 5 && MO_X[mo_before] == 1300.0, "EXPL snapped and popped the twin");

	// ---- frags: chuck dies to bravo; suicide decrements ----
	FC.send(to_json({"t": "DIE", "s": 3, "killer": 2}));
	pump_all(20);
	var fr = log_last(GA, "FRAGS");
	check(fr != null && ("" + fr["k"]).contains("2 1"), `bravo's frag broadcast (${fr == null ? "none" : "" + fr["k"]})`);
	var p2 = SESSION.players[2];
	check(p2.frags == 1, "host table credits bravo");
	FC.send(to_json({"t": "DIE", "s": 3, "killer": -1}));
	pump_all(20);
	var p3 = SESSION.players[3];
	check(p3.frags == -1, `suicide decrements chuck (${p3.frags})`);
	check(p3.deaths == 2, "deaths tracked");

	// ---- scoreboard: lines exist and render pixels ----
	var slines = np_scoreboard_lines();
	check(slines.size() == 4, `scoreboard lists 4 players (${slines.size()})`);
	pix_clear(0);
	scoreboard_draw(slines);
	int lit = 0;
	for (int px = 0; px < VW * VIEWH; ++px) {
		if (PIX[px] != 0) { lit = lit + 1; }
	}
	check(lit > 300, `scoreboard renders (${lit} px)`);

	// ---- the exit gap: a DEATHMATCH client's EXIT ends the level for ALL ----
	check(log_count(GA, "NEXT") == 0, "no NEXT before anyone exits");
	FA.send(to_json({"t": "EXIT", "s": 1, "sec": false}));
	pump_all(20);
	check(LEVEL_DONE, "host level flow armed by the client exit");
	check(log_count(GA, "NEXT") == 1 && log_count(GB, "NEXT") == 1 && log_count(GC, "NEXT") == 1,
		"NEXT broadcast to the whole deathmatch");
	FA.send(to_json({"t": "EXIT", "s": 1, "sec": false}));
	pump_all(15);
	check(log_count(GA, "NEXT") == 1, "duplicate EXIT swallowed (NEXT already out)");
	LEVEL_DONE = false;

	// ---- LEFT: chuck disconnects; his puppet unwinds; others informed ----
	int c_ghost = p3.ghost;
	FC.close();
	pump_all(60);
	check(SESSION.players[3] == null, "slot 3 freed on disconnect");
	check(MO_STATE[c_ghost] == 0, "chuck's ghost despawned");
	check(log_count(GA, "BYE_SLOT") == 1 && log_count(GB, "BYE_SLOT") == 1, "BYE_SLOT relayed to the others");
	check(np_lobby_count() == 2, "count settles at 2");
	np_bye("dm section over");
	pump_all(20);
	FA.close();
	FB.close();
	pump_all(20);

	// ================================================================= co-op ---
	GA = [];
	GB = [];
	check(np_lobby_host(28957, true, "E1M1", 2), "co-op lobby opens");
	FA = fake(0);
	FB = fake(1);
	check(FA.connect("127.0.0.1", 28957) && FB.connect("127.0.0.1", 28957), "two fakes connect");
	pump_all(60);
	hello_from(FA, "alpha");
	pump_all(30);
	hello_from(FB, "bravo");
	pump_all(30);
	np_lobby_go();
	pump_all(40);
	check(NET_GAME && NP_COOP && NP_IS_HOST, "co-op running, host authoritative");
	check(KILL_TOTAL > 0, "monsters spawned host-side");
	HSIG = map_sig();                                    // pristine walk: the client must match it

	// ---- MONS streams reach both fakes ----
	for (int t = 0; t < 40; ++t) { game_tic(); }
	pump_all(30);
	check(log_count(GA, "MONS") > 0 && log_count(GB, "MONS") > 0, "MONS batches reach both clients");

	// ---- a client FIRE kills a real monster and aggro lands on the SHOOTER ----
	int victim = -1;
	for (int m = 0; m < MO_STATE.size(); ++m) {
		if (MO_STATE[m] == 1 && MINFO.has(MO_TYPE[m])) { victim = m; break; }
	}
	check(victim >= 0, "found a sleeping monster");
	var pb = SESSION.players[2];
	// park bravo's ghost at a spot with PROVEN sight to the victim (an
	// arbitrary offset can put map geometry in the way — the p5 probe trick)
	float vex = MO_X[victim];
	float vey = MO_Y[victim];
	float vez = MO_Z[victim] + 32.0;
	bool parked = false;
	for (int pa2 = 0; pa2 < 8 && !parked; ++pa2) {
		float aang2 = pa2 * math::PI / 4.0;
		float gx2 = vex + math::cos(aang2) * 80.0;
		float gy2 = vey + math::sin(aang2) * 80.0;
		if (!sight_clear(gx2, gy2, vex, vey, vez)) { continue; }
		MO_X[pb.ghost] = gx2;
		MO_Y[pb.ghost] = gy2;
		parked = true;
	}
	check(parked, "found a sighted parking spot for the shooter ghost");
	pb.tx = MO_X[pb.ghost];
	pb.ty = MO_Y[pb.ghost];
	pb.alive = true;
	SESSION.refresh_targets();
	int vhp = MO_HP[victim];
	float bang = math::atan2(vey - MO_Y[pb.ghost], vex - MO_X[pb.ghost]);
	FB.send(to_json({"t": "FIRE", "s": 2, "weapon": 2, "x": MO_X[pb.ghost], "y": MO_Y[pb.ghost],
		"z": MO_Z[victim] + 32.0, "ang": bang}));
	pump_all(20);
	check(MO_HP[victim] < vhp, `host replayed the shot (${vhp} -> ${MO_HP[victim]})`);
	check(MO_TARGET[victim] == pb.ghost, "the monster aggroed the SHOOTER's puppet");

	// ---- monsters pick the NEAREST live puppet ----
	var pa = SESSION.players[1];
	MO_X[pa.ghost] = MO_X[victim] + 40.0;             // alpha is closer than bravo now
	MO_Y[pa.ghost] = MO_Y[victim];
	pa.alive = true;
	SESSION.refresh_targets();
	MO_TARGET[victim] = -1;
	P_X = MO_X[victim] + 2000.0;                      // host far away
	P_Y = MO_Y[victim];
	target_info(victim);
	check(MO_TARGET[victim] == pa.ghost, "untargeted monster picked the nearest puppet");

	// ---- puppet attack/pain anims: the apply maps the STATE's anim string ----
	int vic2 = -1;
	for (int v2 = 0; v2 < MO_STATE.size(); ++v2) {
		if (v2 != victim && MO_STATE[v2] == 1 && MINFO.has(MO_TYPE[v2])) { vic2 = v2; break; }
	}
	check(vic2 >= 0, "found a second monster to puppeteer");
	var vmi = MINFO[MO_TYPE[vic2]];
	int sst = MO_STATE[vic2];
	var sanim = "" + MO_ANIM[vic2];
	int sfr = MO_FRAME[vic2];
	float svang = MO_ANG[vic2];
	MO_STATE[vic2] = 12;
	int vwx = math::itrunc(MO_X[vic2]);
	int vwy = math::itrunc(MO_Y[vic2]);
	np_mons_apply(`${vic2} ${vwx} ${vwy} 0 3 1 `);
	check("" + MO_ANIM[vic2] == "" + vmi[7] && MO_FRAME[vic2] == 1, "attack STATE lands the attack letters");
	np_mons_apply(`${vic2} ${vwx} ${vwy} 0 4 0 `);
	check("" + MO_ANIM[vic2] == "" + vmi[8], "pain STATE flinches the puppet");
	np_mons_apply(`${vic2} ${vwx} ${vwy} 0 2 0 `);
	check("" + MO_ANIM[vic2] == "" + vmi[6], "chase STATE returns the walk letters");
	MO_STATE[vic2] = sst;
	MO_ANIM[vic2] = sanim;
	MO_FRAME[vic2] = sfr;
	MO_ANG[vic2] = svang;

	// ...and the wire carries it: force the aggro'd victim into attack
	int gb_base = GB.size();
	MO_STATE[victim] = 3;
	MO_ANIM[victim] = "" + MINFO[MO_TYPE[victim]][7];
	MO_FRAME[victim] = 0;
	MO_TIC[victim] = 5;
	for (int t35 = 0; t35 < 5; ++t35) { game_tic(); }
	pump_all(15);
	bool wire3 = false;
	for (int gi = gb_base; gi < GB.size(); ++gi) {
		var gm = from_json(GB[gi]);
		if ("" + gm["t"] != "MONS") { continue; }
		var wparts = string::trim("" + gm["k"]).split(" ");
		int wi2 = 0;
		while (wi2 + 5 < wparts.size()) {
			if (wparts[wi2].to_int(-1) == victim && wparts[wi2 + 4].to_int(-1) == 3) { wire3 = true; }
			wi2 = wi2 + 6;
		}
	}
	check(wire3, "the MONS stream carried the attack state");

	// ---- USE opens a real door; SECT stream carries it to both ----
	int doorline = -1;
	for (int l = 0; l < LD_SPECIAL.size(); ++l) {
		if (LD_SPECIAL[l] == 1) { doorline = l; break; }
	}
	int dsec = SD_SEC[LD_SIDE1[doorline]];
	int shut = SE_CEILH[dsec];
	float ax = VX_X[LD_V1[doorline]];
	float ay = VX_Y[LD_V1[doorline]];
	float mx = (ax + VX_X[LD_V2[doorline]]) / 2.0;
	float my = (ay + VX_Y[LD_V2[doorline]]) / 2.0;
	FA.send(to_json({"t": "USE", "s": 1, "x": mx, "y": my - 40.0, "ang": 1.5708, "keys": 0}));
	pump_all(15);
	for (int t2 = 0; t2 < 40; ++t2) { game_tic(); }
	pump_all(20);
	check(SE_CEILH[dsec] > shut, "client USE opened the host door");
	var sb = log_last(GB, "SECT");
	check(sb != null && ("" + sb["k"]).contains(`${dsec} `), "SECT stream carried the door to bravo");

	// ---- TAKE double-claim: exactly one ITEM, first claimant wins ----
	int item = -1;
	for (int m2 = 0; m2 < MO_STATE.size(); ++m2) {
		if (MO_STATE[m2] == 7) { item = m2; break; }
	}
	check(item >= 0, "found a map item");
	int items0 = log_count(GA, "ITEM") + log_count(GB, "ITEM");
	FA.send(to_json({"t": "TAKE", "s": 1, "i": item, "nid": -1}));
	FB.send(to_json({"t": "TAKE", "s": 2, "i": item, "nid": -1}));
	pump_all(25);
	var ia = log_last(GA, "ITEM");
	check(ia != null && math::itrunc(ia["by"] * 1.0) == 1, "first claimant won the item");
	check(log_count(GA, "ITEM") == 1 && log_count(GB, "ITEM") == 1, "exactly one ITEM broadcast (double-claim refused)");
	check(MO_STATE[item] == 0, "item despawned host-side");

	// ---- monster attack on a puppet routes DAMAGE to that client only ----
	int a_dmg0 = log_count(GA, "DAMAGE");
	int b_dmg1 = log_count(GB, "DAMAGE");
	MO_TARGET[victim] = pa.ghost;
	MO_X[victim] = MO_X[pa.ghost] + 40.0;             // melee range of a demon? use its own kind
	MO_Y[victim] = MO_Y[pa.ghost];
	monster_attack(victim);
	pump_all(20);
	check(log_count(GA, "DAMAGE") > a_dmg0, "monster attack DAMAGE routed to alpha");
	check(log_count(GB, "DAMAGE") == b_dmg1, "bravo untouched by it");

	// ---- PROJ streaming: a host fireball reaches both fakes, dead-reckons ----
	int a_proj1 = log_count(GA, "PROJ");
	float fbx = 1056.0;
	float fby = -3550.0;
	float fbz = SE_FLOORH[sector_at(fbx, fby)] + 32.0;
	int fb2 = fireball_spawn(fbx, fby, fbz, 1.5708, victim, fbz, 2048.0);
	pump_all(15);
	check(log_count(GA, "PROJ") == a_proj1 + 1 && log_count(GB, "PROJ") > 0, "host fireball PROJ reached both");
	var pj = log_last(GA, "PROJ");
	check(pj != null, "PROJ retrievable");
	var pjspr = "" + pj["spr"];                          // hoist every read before other ops
	var pjsnd = "" + pj["snd"];
	float pjy = pj["y"] * 1.0;
	float pjmy = pj["my"] * 1.0;
	check(pjspr == "BAL1" && pjsnd == "DSFIRSHT", "PROJ carries sprite + launch sound");
	for (int dt = 0; dt < 10; ++dt) { projectile_tic(fb2); }
	check(MO_STATE[fb2] == 8, "fireball still in flight after 10 tics");
	float predict_y = pjy + pjmy * 10.0;
	check(MO_Y[fb2] > predict_y - 0.01 && MO_Y[fb2] < predict_y + 0.01,
		`dead reckoning matches the host flight (${MO_Y[fb2]:.2} vs ${predict_y:.2})`);
	int a_expl0 = log_count(GA, "EXPL");
	projectile_explode(fb2);
	pump_all(15);
	check(log_count(GA, "EXPL") == a_expl0 + 1, "EXPL followed the pop");
	check(!NP_PROJ_MINE.has(fb2), "announcer cleaned its id map");

	// ---- streamed-twin mechanics: no contact damage, exact flight, EXPL snap ----
	// (stage the player back on real floor first: earlier tests parked P off-map)
	P_X = 1056.0;
	P_Y = -3550.0;
	P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
	int hp_before = P_HP;
	np_proj_apply(0, 424242, "BAL1", "", P_X - 40.0, P_Y, P_FLOORZ + 32.0, 10.0, 0.0, 0.0);
	int tw = MO_X.size() - 1;
	check(MO_NODMG[tw] == 2 && MO_COOL[tw] == 350, "twin spawned streamed with a lifetime guard");
	for (int tt = 0; tt < 8; ++tt) { projectile_tic(tw); }
	check(P_HP == hp_before, "twin flew through the player without damage");
	check(MO_X[tw] > P_X - 40.0 + 79.9, "twin integrated its momenta");
	np_expl_apply(0, 424242, P_X + 100.0, P_Y, P_FLOORZ + 32.0);
	check(MO_STATE[tw] == 5 && MO_X[tw] == P_X + 100.0, "EXPL snapped the twin to the blast point");

	// ---- owner-skip: a replayed client rocket never echoes to its shooter ----
	int a_proj2 = log_count(GA, "PROJ");
	int b_proj2 = log_count(GB, "PROJ");
	float rgx = MO_X[pb.ghost];
	float rgy = MO_Y[pb.ghost];
	FB.send(to_json({"t": "FIRE", "s": 2, "weapon": 5, "x": rgx, "y": rgy, "z": 32.0, "ang": 0.0}));
	pump_all(20);
	check(log_count(GA, "PROJ") == a_proj2 + 1, "replayed rocket announced to the other client");
	check(log_count(GB, "PROJ") == b_proj2, "the rocket's owner was skipped (has its own twin)");

	// ---- SECT cadence: a moving door streams EVERY tic, one step max lag ----
	int doorline2 = -1;
	for (int l2 = 0; l2 < LD_SPECIAL.size(); ++l2) {
		if (LD_SPECIAL[l2] == 1 && SD_SEC[LD_SIDE1[l2]] != dsec) { doorline2 = l2; break; }
	}
	check(doorline2 >= 0, "found a second door");
	int dsec2 = SD_SEC[LD_SIDE1[doorline2]];
	door_use(dsec2, false);
	int sect_hits = 0;
	int fresh = 0;
	for (int mt = 0; mt < 20; ++mt) {
		int gb0 = GB.size();
		game_tic();
		pump_all(2);
		var sml = log_last(GB, "SECT");
		if (sml != null && ("" + sml["k"]).contains(`${dsec2} `)) {
			sect_hits = sect_hits + 1;
			// parse the streamed ceil for our sector and compare to live host state
			var parts = string::trim("" + sml["k"]).split(" ");
			int pi = 0;
			while (pi + 2 < parts.size()) {
				if (parts[pi].to_int(-1) == dsec2) {
					int wire_ceil = parts[pi + 2].to_int(-9999);
					int lag = SE_CEILH[dsec2] - wire_ceil;
					if (lag < 0) { lag = -lag; }
					if (lag <= 2) { fresh = fresh + 1; }     // one door step = 2 units
				}
				pi = pi + 3;
			}
		}
	}
	check(sect_hits >= 16, `SECT streamed nearly every moving tic (${sect_hits}/20)`);
	check(fresh >= 16, `streamed heights within one step of the host (${fresh}/${sect_hits})`);

	// ---- TELE gating: door/lift crossings NEVER teleport; real teleports do ----
	int crossline = -1;
	for (int l3 = 0; l3 < LD_SPECIAL.size(); ++l3) {
		int sp3 = LD_SPECIAL[l3];
		if (sp3 == 88 || sp3 == 2 || sp3 == 90) { crossline = l3; break; }   // walkover lift/door
	}
	check(crossline >= 0, "found a walkover special");
	int a_tele0 = log_count(GA, "TELE");
	float hpx = P_X;
	float hpy = P_Y;
	float hpa = P_ANG;
	FA.send(to_json({"t": "CROSS", "s": 1, "line": crossline, "x": 1100.0, "y": -3300.0, "side": 0, "keys": 0}));
	pump_all(20);
	check(log_count(GA, "TELE") == a_tele0, "door/lift crossing produced NO TELE");
	check(P_X == hpx && P_Y == hpy && P_ANG == hpa, "borrowed pose restored the host exactly");

	// synthesize a genuine teleport: spare line -> special 39, fresh tag, dest thing
	int sline = -1;
	for (int l4 = 0; l4 < LD_SPECIAL.size(); ++l4) {
		if (LD_SPECIAL[l4] == 0 && LD_SIDE1[l4] >= 0) { sline = l4; break; }
	}
	check(sline >= 0, "found a spare two-sided line");
	int save_sp = LD_SPECIAL[sline];
	int save_tag = LD_TAG[sline];
	int dest_sec = sector_at(1056.0, -3500.0);
	int save_sectag = SE_TAG[dest_sec];
	LD_SPECIAL[sline] = 39;
	LD_TAG[sline] = 999;
	SE_TAG[dest_sec] = 999;
	TH_TYPE.push(14);
	TH_X.push(1056);
	TH_Y.push(-3500);
	TH_ANG.push(90);
	TH_FLAGS.push(7);
	FA.send(to_json({"t": "CROSS", "s": 1, "line": sline, "x": 1100.0, "y": -3300.0, "side": 0, "keys": 0}));
	pump_all(20);
	var tl = log_last(GA, "TELE");
	check(tl != null, "a real teleport special DID send TELE");
	float tlx = tl == null ? 0.0 : tl["x"] * 1.0;        // hoisted before other ops
	float tly = tl == null ? 0.0 : tl["y"] * 1.0;
	check(tlx == 1056.0 && tly == -3500.0, "TELE carries the dest thing's spot");
	check(P_X == hpx && P_Y == hpy && P_ANG == hpa, "host pose survived the teleport replay too");
	LD_SPECIAL[sline] = save_sp;
	LD_TAG[sline] = save_tag;
	SE_TAG[dest_sec] = save_sectag;
	TH_TYPE.pop();
	TH_X.pop();
	TH_Y.pop();
	TH_ANG.pop();
	TH_FLAGS.pop();

	// ---- NEXT / READY x2 / START ----
	LEVEL_DONE = true;
	for (int t3 = 0; t3 < 8; ++t3) { game_tic(); }
	pump_all(20);
	check(log_count(GA, "NEXT") == 1 && log_count(GB, "NEXT") == 1, "NEXT reached both");
	FA.send(to_json({"t": "READY", "s": 1}));
	pump_all(15);
	check(!NP_ALL_READY, "one READY is not enough");
	check(!np_host_can_start(), "the recount holds the start too");
	FB.send(to_json({"t": "READY", "s": 2}));
	pump_all(15);
	check(NP_ALL_READY, "both READY = all ready");
	check(np_host_can_start(), "fresh recount agrees");
	NP_READY_SET = {};                                   // the 10s grace path, isolated
	NP_ALL_READY = false;
	NP_WI_T0 = time::now();
	check(!np_host_can_start(), "cleared votes hold the start again");
	NP_WI_T0 = time::now() - 11.0;
	check(np_host_can_start(), "the 10s grace unlocks the start");
	NP_WI_T0 = null;
	FA.send(to_json({"t": "READY", "s": 1}));
	FB.send(to_json({"t": "READY", "s": 2}));
	pump_all(15);
	np_start_announce("E1M2");
	pump_all(30);
	check(GAME_MAP == "E1M2", "host moved to E1M2");
	check(log_count(GA, "START") == 1 && log_count(GB, "START") == 1, "START reached both");
	check(SESSION.players[1] != null && SESSION.players[1].ghost >= 0, "puppets respawned on the new map");

	// ---- round 2: a mid-intermission leaver must not wedge the start ----
	LEVEL_DONE = true;
	for (int t4 = 0; t4 < 8; ++t4) { game_tic(); }
	pump_all(20);
	check(log_count(GA, "NEXT") == 2 && log_count(GB, "NEXT") == 2, "second NEXT reached both");
	FA.send(to_json({"t": "READY", "s": 1}));
	pump_all(15);
	check(!np_host_can_start(), "one of two still holds");
	FB.close();
	pump_all(60);
	check(SESSION.players[2] == null, "bravo freed on the mid-intermission drop");
	check(np_host_can_start(), "recount: the leaver no longer holds the exit");
	LEVEL_DONE = false;
	np_start_announce("");                               // episode over: START names no map
	pump_all(20);
	check(log_count(GA, "START") == 2, "episode-end START reached the survivor");
	check(GAME_MAP == "E1M2", "empty START starts nothing: the end screen owns the exit");

	np_bye("done");
	pump_all(20);
	FA.close();
	FA = null;
	FB = null;
	FC = null;

	// ========================================== client-side exit flow (late) ---
	// a REAL ClientSession against a scripted fake host — the latency harness:
	// deliveries land late, the client keeps simulating, and it never travels
	// until the host's START says so.
	GA = [];
	FH = new NetServer();
	FH.on_client_connect.connect("g", [](var cid) { FH_CID = cid + 0; });
	FH.on_client_packet.connect("g", [](var cid, var b) { GA.push(b); });
	check(FH.listen(28958), "fake host listens");
	check(np_lobby_join("127.0.0.1", 28958), "client session dials");
	pump2(80);
	check(log_count(GA, "HELLO") == 1, "client HELLOed on connect");
	FH.send(FH_CID, to_json({"t": "WELCOME", "s": 0, "slot": 1, "mode": "coop",
		"mapname": "E1M1", "skill": 2, "roster": "0:host "}));
	pump2(30);
	check(NP_LOBBY == "joined", "client joined the fake lobby");
	FH.send(FH_CID, to_json({"t": "GO", "s": 0, "mode": "coop", "mapname": "E1M1", "skill": 2}));
	pump2(40);
	check(NET_GAME && NP_COOP && !NP_IS_HOST, "client entered co-op as a non-host");
	int csig = map_sig();
	check(csig == HSIG, `client walk matches the host walk exactly (${csig} vs ${HSIG})`);
	int cpup = 0;
	for (int cm = 0; cm < MO_STATE.size(); ++cm) {
		if (MO_STATE[cm] == 12) { cpup = cpup + 1; }
	}
	check(cpup > 0, `monsters spawned as puppets (${cpup})`);

	// the client's own exit: EXIT travels up ONCE, gameplay does not advance
	check(!NP_EXIT_SENT, "exit latch starts clear");
	np_exit_send();
	np_exit_send();
	pump2(20);
	check(log_count(GA, "EXIT") == 1, "EXIT sent exactly once (latched)");
	check(GAME_MAP == "E1M1" && !NP_WI_PEND, "no self-advance: the host owns NEXT");

	// NEXT arrives ~150ms late; the client keeps ticking E1M1 until it lands
	float due = time::now() + 0.15;
	bool nsent = false;
	int spin = 0;
	while (spin < 400 && !NP_WI_PEND) {
		if (!nsent && time::now() >= due) {
			FH.send(FH_CID, to_json({"t": "NEXT", "s": 0, "mapname": "E1M1", "next": "E1M2",
				"k": 80, "i": 60, "s2": 33, "secs": 90}));
			nsent = true;
		}
		game_tic();
		np_frame();
		FH.pump();
		sleep(0.003);
		spin = spin + 1;
	}
	check(NP_WI_PEND && NP_WI_NEXT == "E1M2" && NP_WI_K == 80, "late NEXT set the intermission pending flags");
	check(GAME_MAP == "E1M1", "still on E1M1: NEXT alone never travels");
	NP_WI_PEND = false;                                  // the flow loop's job; p9 consumes it

	// READY goes up; START travels the client; the exit latch re-arms
	np_ready_send();
	pump2(20);
	check(log_count(GA, "READY") == 1, "READY reached the fake host");
	FH.send(FH_CID, to_json({"t": "START", "s": 0, "mapname": "E1M2"}));
	pump2(30);
	check(GAME_MAP == "E1M2" && NP_STARTED_PEND, "START traveled the client to E1M2");
	NP_STARTED_PEND = false;
	check(!NP_EXIT_SENT, "exit latch re-armed for the new map");
	bool cpup2 = false;
	for (int cm2 = 0; cm2 < MO_STATE.size(); ++cm2) {
		if (MO_STATE[cm2] == 12) { cpup2 = true; }
	}
	check(cpup2, "E1M2 monsters are puppets again");

	// the host leaving ends the session cleanly
	FH.send(FH_CID, to_json({"t": "BYE", "s": 0, "why": "episode complete"}));
	pump2(30);
	check(SESSION == null && !NET_GAME, "host BYE tore the client session down");
	FH.close();

	print("");
	print(FAILS == 0 ? "P9 GATE: ALL GREEN" : `P9 GATE: ${FAILS} FAILURES`);
	return FAILS;
}

main()
