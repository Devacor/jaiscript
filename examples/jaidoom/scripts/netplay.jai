// JaiDOOM netplay.jai — up to 16 players over the runner's NetServer/NetChannel,
// organized as classes: a Player per slot (LocalPlayer = me, Puppet = a
// net-driven PLAY-sprite ghost), and a Session owning the wire (HostSession =
// NetServer star hub: assigns slots, relays, runs co-op authority;
// ClientSession = one NetChannel to the host). One action pattern carries
// everything: one JSON map per packet, "t" type + "s" sender slot,
// dispatched by Session.apply. The mobj pool stays flat (admission rules);
// classes hold INDICES into it. Free np_* functions remain the boundary the
// rest of the game calls — they delegate to SESSION.
//
// Relay semantics: clients only ever talk to the host. STATE/FIRE/DIE/RESPAWN
// apply on the host then fan out to the other clients (client-to-client is 2
// hops; the host is the clock). DAMAGE routes to its victim only. Co-op
// streams (MONS/SECT/DROP/BOOM/ITEM/SWITCHED) broadcast from the host's
// authority. Level flow is host-owned in BOTH modes: any exit travels up as
// EXIT, the host answers NEXT (intermission for everyone), clients offer
// READY, and START moves the whole session together. The host quitting ends
// the session for everyone.

import "sim_random.jai";
import "wad.jai";
import "map.jai";
import "vid.jai";
import "sound.jai";
import "things.jai";
import "game.jai";

var NP_PROTO = 3;             // 3: slots + roster + GO lobby (N-player star)
var NP_MAX_PLAYERS = 16;
var NP_NAME = "doomguy";
var SESSION = null;

// flow globals the rest of the game reads (game.jai declares NET_GAME/NP_COOP/
// NP_IS_HOST/NP_FORWARD; these are netplay's own)
var NP_LOBBY = "";            // "" | listening | connecting | joined | ready | failed: why
var NP_LOBBY_T0 = 0.0;
var NET_FRAGS_MINE = 0;       // hud mirror (players[me].frags)
var NP_WI_PEND = false;
var NP_WI_MAP = "";
var NP_WI_NEXT = "";
var NP_WI_K = 0;
var NP_WI_I = 0;
var NP_WI_S = 0;
var NP_WI_T = 0;
var NP_READY_SENT = false;
var NP_ALL_READY = false;     // host: every client pressed through the intermission
var NP_STARTED_PEND = false;
var NP_STARTED_MAP = "";
var NP_SCORE_ON = false;      // 'o' scoreboard overlay

// flat ghost-target mirror for things.jai's monster targeting (admission-
// friendly: refreshed each tic from the live puppets)
array<float> NP_TGT_X = [];
array<float> NP_TGT_Y = [];
array<int> NP_TGT_MOBJ = [];

// ---------------------------------------------------------------- players ---

class Player {
	int slot = 0;
	string name = "";
	int frags = 0;
	int deaths = 0;
	bool alive = true;
	float tx = 0.0;               // last-known pose (lerp target for puppets)
	float ty = 0.0;
	float tz = 0.0;
	float ang = 0.0;
	int weapon = 2;
	int ghost = -1;               // mobj index; -1 = the local player (no ghost)
	int atk_tics = 0;
	int last_hurt_by = -1;        // slot of the last DAMAGE source (frag credit)
	int hurt_tic = -1000;

	Player(int s, string n) {
		slot = s;
		name = n;
	}

	void spawn_ghost(float x, float y) {
		if (ghost >= 0) { return; }
		ghost = mobj_spawn(x, y, 10001 + slot, 0.0, 11, "PLAY", "ABCD", 100, 16.0);
		tx = x;
		ty = y;
		alive = true;
	}

	void despawn() {
		if (ghost >= 0) { MO_STATE[ghost] = 0; ghost = -1; }
	}

	void apply_state(float x, float y, float z, float a, int w, bool now_alive) {
		tx = x;
		ty = y;
		tz = z;
		weapon = w;
		if (ghost >= 0) {
			MO_ANG[ghost] = a;
			if (now_alive && !alive) { revive(x, y, true); }
		}
		ang = a;
		alive = now_alive;
	}

	void die_anim() {
		alive = false;
		if (ghost < 0) { return; }
		MO_ANIM[ghost] = "HIJKLMN";                  // PLAY death run
		MO_FRAME[ghost] = 0;
		MO_TIC[ghost] = 5;
		snd_play_at("DSPLDETH", MO_X[ghost], MO_Y[ghost]);
	}

	void revive(float x, float y, bool snap) {
		alive = true;
		if (ghost < 0) { return; }
		if (snap) {                                  // respawns teleport: no lerp across the map
			MO_X[ghost] = x;
			MO_Y[ghost] = y;
		}
		tx = x;
		ty = y;
		MO_ANIM[ghost] = "ABCD";
		MO_FRAME[ghost] = 0;
		MO_TIC[ghost] = 5;
		MO_Z[ghost] = SE_FLOORH[sector_at(MO_X[ghost], MO_Y[ghost])] * 1.0;
	}

	// quarter-gap glide toward the last STATE; walk/idle/attack poses
	void tick_lerp() {
		if (ghost < 0) { return; }
		if (atk_tics > 0) { atk_tics = atk_tics - 1; }
		if (alive) {
			float dx = tx - MO_X[ghost];
			float dy = ty - MO_Y[ghost];
			if (dx * dx + dy * dy > 160.0 * 160.0) {
				MO_X[ghost] = tx;
				MO_Y[ghost] = ty;
			} else {
				MO_X[ghost] = MO_X[ghost] + dx * 0.25;
				MO_Y[ghost] = MO_Y[ghost] + dy * 0.25;
			}
			MO_Z[ghost] = SE_FLOORH[sector_at(MO_X[ghost], MO_Y[ghost])] * 1.0;
			bool moving = dx * dx + dy * dy > 4.0;
			var want = atk_tics > 0 ? "EF" : (moving ? "ABCD" : "AB");
			if (MO_ANIM[ghost] != want) { MO_ANIM[ghost] = want; MO_FRAME[ghost] = 0; }
			if (MO_TIC[ghost] > 0) { MO_TIC[ghost] = MO_TIC[ghost] - 1; }
			else {
				MO_FRAME[ghost] = (MO_FRAME[ghost] + 1) % MO_ANIM[ghost].size();
				MO_TIC[ghost] = 5;
			}
		} else if (MO_FRAME[ghost] < MO_ANIM[ghost].size() - 1) {
			if (MO_TIC[ghost] > 0) { MO_TIC[ghost] = MO_TIC[ghost] - 1; }
			else { MO_FRAME[ghost] = MO_FRAME[ghost] + 1; MO_TIC[ghost] = 5; }
		}
	}
}

// the local player: pose comes from the live P_* globals, never a ghost
class LocalPlayer : Player {
	LocalPlayer(int s, string n) : super(s, n) {}

	var state_action() {
		return {"t": "STATE", "s": slot, "x": P_X, "y": P_Y, "z": P_Z,
			"ang": P_ANG, "weapon": CUR_WEAPON, "alive": P_HP > 0};
	}
}

// a remote player's ghost; all behavior lives in Player, the class is the
// taxonomy (and the place puppet-only behavior lands later)
class Puppet : Player {
	Puppet(int s, string n) : super(s, n) {}
}

// ---------------------------------------------------------------- session ---

class Session {
	var players = [];             // slot-indexed; null holes
	int me = 0;
	bool coop = false;
	string mapname = "E1M1";
	int skill = 2;
	int stage = 0;                // 0 lobby, 3 playing
	int cadence = 0;
	int last_rx = 0;
	bool stalled = false;
	bool dead_sent = false;
	int close_in = -1;            // deferred close: the wire delivers BYE first

	Session() {
		for (int i = 0; i < NP_MAX_PLAYERS; ++i) { players.push(null); }
	}

	// transport points the subclasses own
	void tx_host(var m) {}                           // client -> host
	void tx_slot(int slot, var m) {}                 // host -> one client
	void tx_others(int except_slot, var m) {}        // host -> everyone but
	void tx_all(var m) {}                            // host -> everyone
	int peer_count() -> int { return 0; }
	void wire_close() {}
	bool wire_connected() -> bool { return false; }

	// a client sends to the host; the host "sends" by broadcasting
	void announce(var m) {
		if (me == 0) { tx_all(m); }
		else { tx_host(m); }
	}

	var my_player() { return players[me]; }

	int living_players() -> int {
		int n = 0;
		for (int i = 0; i < NP_MAX_PLAYERS; ++i) {
			if (players[i] != null) { n = n + 1; }
		}
		return n;
	}

	var player_of_ghost(int mobj) {
		for (int i = 0; i < NP_MAX_PLAYERS; ++i) {
			var p = players[i];
			if (p != null && p.ghost == mobj) { return p; }
		}
		return null;
	}

	void begin() {
		NET_GAME = true;
		NP_COOP = coop;
		NP_IS_HOST = me == 0;
		NP_FORWARD = coop && me != 0;
		SKILL_BIT = skill;
		NIGHTMARE = false;
		FAST_MONSTERS = false;
		np_loadout();
		game_start_map(mapname);
		coop_reset();
		dead_sent = false;
		stage = 3;
		NP_LOBBY = "ready";
		spawn_ghosts();
		announce(my_player().state_action());
		int others = living_players() - 1;
		set_msg(coop ? `co-op x${others + 1} - good hunting` : `deathmatch x${others + 1} - fight!`);
	}

	void spawn_ghosts() {
		for (int i = 0; i < NP_MAX_PLAYERS; ++i) {
			var p = players[i];
			if (p == null || i == me) { continue; }
			p.ghost = -1;                            // pool was rebuilt by game_start_map
			p.spawn_ghost(P_X, P_Y);
		}
	}

	// ---- the dispatch table: every packet lands here with its sender slot ----
	void apply(int from, var m) {
		var t = "" + m["t"];
		last_rx = GAME_TIC;
		stalled = false;
		if (t == "STATE") {
			var p = players[from];
			if (p == null || from == me) { return; }
			p.apply_state(m["x"] * 1.0, m["y"] * 1.0, m["z"] * 1.0, m["ang"] * 1.0,
				math::itrunc(m["weapon"] * 1.0), m["alive"] == true);
		} else if (t == "FIRE") {
			var p2 = players[from];
			if (p2 == null || from == me) { return; }
			int wnum = math::itrunc(m["weapon"] * 1.0);
			float fx = m["x"] * 1.0;
			float fy = m["y"] * 1.0;
			float fz = m["z"] * 1.0;
			float fang = m["ang"] * 1.0;
			var w = WEAPONS[wnum];
			if (w != null && w.snd != "") { snd_play_at(w.snd, fx, fy); }
			p2.atk_tics = 8;
			if (coop && me == 0) {                   // host replays the trigger pull
				replay_fire(p2, wnum, fx, fy, fz, fang);
			}
		} else if (t == "DAMAGE") {
			NP_HURT_HOOK = from;
			my_player().last_hurt_by = from;
			my_player().hurt_tic = GAME_TIC;
			player_damage_from(m["amount"] + 0, m["sx"] * 1.0, m["sy"] * 1.0);
		} else if (t == "DIE") {
			var p3 = players[from];
			if (p3 != null && from != me) {
				p3.die_anim();
				p3.deaths = p3.deaths + 1;
				var kn = p3.name;
				if (coop) { set_msg(`${kn} is down!`); }
			}
			if (me == 0) { host_score_death(from, math::itrunc(m["killer"] * 1.0)); }
		} else if (t == "RESPAWN") {
			var p4 = players[from];
			if (p4 != null && from != me) { p4.revive(m["x"] * 1.0, m["y"] * 1.0, true); }
		} else if (t == "FRAGS") {
			apply_frags(m["k"]);
		} else if (t == "MONS") {
			np_mons_apply(m["k"]);
		} else if (t == "SECT") {
			np_sect_apply(m["k"]);
		} else if (t == "BOOM") {
			np_boom_apply(math::itrunc(m["i"] * 1.0));
		} else if (t == "DROP") {
			np_drop_apply(math::itrunc(m["nid"] * 1.0), math::itrunc(m["ty"] * 1.0), "" + m["spr"], m["x"] * 1.0, m["y"] * 1.0);
		} else if (t == "TAKE") {
			host_take(from, math::itrunc(m["i"] * 1.0), math::itrunc(m["nid"] * 1.0));
		} else if (t == "ITEM") {
			np_item_apply(math::itrunc(m["i"] * 1.0), math::itrunc(m["nid"] * 1.0), math::itrunc(m["by"] * 1.0));
		} else if (t == "USE") {
			host_use(from, m["x"] * 1.0, m["y"] * 1.0, m["ang"] * 1.0, m["keys"] + 0);
		} else if (t == "CROSS") {
			host_cross(from, math::itrunc(m["line"] * 1.0), m["x"] * 1.0, m["y"] * 1.0, math::itrunc(m["side"] * 1.0), m["keys"] + 0);
		} else if (t == "TELE") {
			P_X = m["x"] * 1.0;
			P_Y = m["y"] * 1.0;
			P_ANG = m["ang"] * 1.0;
			P_MOMX = 0.0;
			P_MOMY = 0.0;
			P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
			P_Z = P_FLOORZ * 1.0;
			P_VIEWZ = P_FLOORZ + 41.0;
			announce(my_player().state_action());
		} else if (t == "PROJ") {
			if (from == me) { return; }
			np_proj_apply(from, math::itrunc(m["id"] * 1.0), m["spr"], m["snd"],
				m["x"] * 1.0, m["y"] * 1.0, m["z"] * 1.0,
				m["mx"] * 1.0, m["my"] * 1.0, m["mz"] * 1.0);
		} else if (t == "EXPL") {
			if (from == me) { return; }
			np_expl_apply(from, math::itrunc(m["id"] * 1.0), m["x"] * 1.0, m["y"] * 1.0, m["z"] * 1.0);
		} else if (t == "SWITCHED") {
			switch_use(math::itrunc(m["line"] * 1.0));
		} else if (t == "EXIT") {
			// a client found the exit; NEXT goes out HERE, not via tic(): the
			// flow loop's wi_start clears LEVEL_DONE before the next tic runs
			if (me == 0 && stage == 3 && !NP_NEXT_SENT) {
				SECRET_EXIT = m["sec"] == true;
				LEVEL_DONE = true;
				np_next_announce(next_map_name());
				var pe = players[from];
				if (pe != null) { set_msg(`${pe.name} found the exit`); }
			}
		} else if (t == "NEXT") {
			NP_WI_MAP = "" + m["mapname"];
			NP_WI_NEXT = "" + m["next"];
			NP_WI_K = m["k"] + 0;
			NP_WI_I = m["i"] + 0;
			NP_WI_S = m["s2"] + 0;
			NP_WI_T = m["secs"] + 0;
			NP_WI_PEND = true;
		} else if (t == "READY") {
			host_ready(from);
		} else if (t == "START") {
			NP_STARTED_MAP = "" + m["mapname"];
			if (NP_STARTED_MAP != "") { travel(NP_STARTED_MAP); }
			NP_STARTED_PEND = true;
		} else if (t == "BYE") {
			peer_left(from, "left");
		}
	}

	// ---- frag bookkeeping lives on the host, broadcast as a flat string ----
	void host_score_death(int victim, int killer) {
		if (me != 0 || coop) { return; }
		var vp = players[victim];
		if (vp == null) { return; }
		if (killer >= 0 && killer != victim && players[killer] != null) {
			var kp = players[killer];
			kp.frags = kp.frags + 1;
		} else {
			vp.frags = vp.frags - 1;                 // suicide/environment: your own score pays
		}
		broadcast_frags();
	}

	void broadcast_frags() {
		var k = "";
		for (int i = 0; i < NP_MAX_PLAYERS; ++i) {
			var p = players[i];
			if (p != null) { k += `${i} ${p.frags} `; }
		}
		var m = {"t": "FRAGS", "s": 0, "k": k};
		tx_all(m);
		apply_frags(k);                              // the host's own table too
	}

	void apply_frags(string k) {
		var parts = string::trim(k).split(" ");
		int i = 0;
		while (i + 1 < parts.size()) {
			int slot = parts[i].to_int(-1);
			int f = parts[i + 1].to_int(0);
			if (slot >= 0 && slot < NP_MAX_PLAYERS && players[slot] != null) {
				var p = players[slot];
				p.frags = f;
			}
			i = i + 2;
		}
		NET_FRAGS_MINE = my_player().frags;
	}

	// ---- per-tic: state cadence, death announce, host streams ----
	void tic() {
		if (stage != 3) { return; }
		cadence = cadence + 1;
		if (cadence >= 2) {
			cadence = 0;
			announce(my_player().state_action());
		}
		if (me == 0) {
			if (coop) { host_streams(); }
			// level end is NOT co-op-only: deathmatch travels together too
			if (LEVEL_DONE && !NP_NEXT_SENT) { np_next_announce(next_map_name()); }
		}
		if (P_HP <= 0 && !dead_sent) {
			dead_sent = true;
			var mp = my_player();
			int killer = GAME_TIC - mp.hurt_tic < 35 ? mp.last_hurt_by : -1;
			announce({"t": "DIE", "s": me, "killer": killer});
			if (me == 0) { host_score_death(0, killer); }
			mp.deaths = mp.deaths + 1;
		}
		refresh_targets();
	}

	// the flat mirror monster targeting reads (live ghosts only)
	void refresh_targets() {
		NP_TGT_X = [];
		NP_TGT_Y = [];
		NP_TGT_MOBJ = [];
		for (int i = 0; i < NP_MAX_PLAYERS; ++i) {
			var p = players[i];
			if (p == null || i == me || p.ghost < 0 || !p.alive) { continue; }
			NP_TGT_X.push(MO_X[p.ghost]);
			NP_TGT_Y.push(MO_Y[p.ghost]);
			NP_TGT_MOBJ.push(p.ghost);
		}
	}

	// ---- per-frame: pump (subclass), lerp everyone, respawn key, stall ----
	void frame_common() {
		if (close_in > 0) {
			close_in = close_in - 1;
			if (close_in == 0) { wire_close(); close_in = -1; }
		}
		if (stage != 3) { return; }
		for (int i = 0; i < NP_MAX_PLAYERS; ++i) {
			var p = players[i];
			if (p != null && i != me) { p.tick_lerp(); }
		}
		if (P_HP <= 0 && (key_down("f") || key_down("ctrl") || key_down("enter"))) { respawn(); }
		if (GAME_TIC - last_rx > 105 && !stalled && wire_connected()) {
			stalled = true;
			set_msg("net stalled...");
		}
		if (coop && me != 0) { np_puppet_lerp(); }
	}

	// dm: cycle deathmatch starts by slot+deaths with an occupancy nudge;
	// co-op: your own numbered start, keys kept
	void respawn() {
		var sx = [];
		var sy = [];
		var sa = [];
		int want = coop ? (me == 0 ? 1 : imin_start(me + 1)) : 11;
		for (int t = 0; t < TH_TYPE.size(); ++t) {
			if (TH_TYPE[t] == want) {
				sx.push(TH_X[t] * 1.0);
				sy.push(TH_Y[t] * 1.0);
				sa.push(TH_ANG[t] * math::PI / 180.0);
			}
		}
		np_loadout_coop();
		var mp = my_player();
		int n = sx.size();
		if (n > 0) {
			int pick = coop ? 0 : (me + mp.deaths) % n;
			for (int nudge = 0; nudge < n; ++nudge) {  // skip occupied pads
				int i = (pick + nudge) % n;
				if (!spawn_occupied(sx[i], sy[i])) { pick = i; break; }
			}
			P_X = sx[pick];
			P_Y = sy[pick];
			P_ANG = sa[pick];
		} else {
			for (int t2 = 0; t2 < TH_TYPE.size(); ++t2) {
				if (TH_TYPE[t2] == 1) { P_X = TH_X[t2] * 1.0; P_Y = TH_Y[t2] * 1.0; P_ANG = TH_ANG[t2] * math::PI / 180.0; break; }
			}
		}
		P_MOMX = 0.0;
		P_MOMY = 0.0;
		P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
		P_Z = P_FLOORZ * 1.0;
		P_VIEWZ = P_FLOORZ + 41.0;
		P_AIR = false;
		dead_sent = false;
		announce({"t": "RESPAWN", "s": me, "x": P_X, "y": P_Y, "ang": P_ANG});
		announce(my_player().state_action());
	}

	bool spawn_occupied(float x, float y) -> bool {
		for (int i = 0; i < NP_MAX_PLAYERS; ++i) {
			var p = players[i];
			if (p == null || i == me || p.ghost < 0 || !p.alive) { continue; }
			float dx = MO_X[p.ghost] - x;
			float dy = MO_Y[p.ghost] - y;
			if (dx * dx + dy * dy < 64.0 * 64.0) { return true; }
		}
		return false;
	}

	int imin_start(int want) -> int { return want > 4 ? 4 : want; }   // maps carry starts 1..4

	// NOT named next_map: a bare in-method call to that name resolves to
	// game.jai's zero-arg next_map() global, not the method — the exact crash
	// that stranded co-op clients on the old level at every START
	void travel(string newmap) {
		np_loadout_coop();
		game_start_map(newmap);
		coop_reset();
		dead_sent = false;
		NP_WI_PEND = false;
		NP_ALL_READY = false;
		spawn_ghosts();
		announce(my_player().state_action());
	}

	void peer_left(int slot, string why) {
		var p = players[slot];
		if (p == null) { return; }
		set_msg(`${p.name} ${why}`);
		p.despawn();
		players[slot] = null;
	}

	void shutdown(string why) {
		if (stage == 3) { set_msg(`netgame over: ${why}`); }
		bool was_client = coop && me != 0;
		stage = 0;
		NET_GAME = false;
		NP_COOP = false;
		NP_IS_HOST = false;
		NP_FORWARD = false;
		NP_LOBBY = "";
		NP_WI_PEND = false;
		for (int i = 0; i < NP_MAX_PLAYERS; ++i) {
			var p = players[i];
			if (p != null && i != me) { p.despawn(); }
		}
		if (was_client) {                            // wake frozen puppets into real monsters
			for (int m = 0; m < MO_STATE.size(); ++m) {
				if (MO_STATE[m] == 12) { MO_STATE[m] = 1; }
			}
		}
		SESSION = null;
	}

	// ---- host-only co-op authority (defined here; guarded by me==0) ----
	void host_streams() {
		// SECT streams EVERY tic: the diff is only dirty sectors, so an idle map
		// costs nothing and a moving door lags the client by at most one tic
		// (8Hz batches stepped its collision opening ~8 units at a time)
		var sk = np_sect_batch();
		if (sk != "") { tx_all({"t": "SECT", "s": 0, "k": sk}); }
		if ((GAME_TIC & 3) == 0) {
			NP_MONS_KEY = NP_MONS_KEY - 1;
			var mk = np_mons_batch(NP_MONS_KEY <= 0);
			if (mk != "") { tx_all({"t": "MONS", "s": 0, "k": mk}); }
			if (NP_MONS_KEY <= 0) { NP_MONS_KEY = 17; }
		}
	}

	void host_ready(int from) {
		if (me != 0) { return; }
		NP_READY_SET[from] = 1;
		NP_ALL_READY = np_ready_have() >= np_ready_need();
	}

	void host_take(int from, int idx, int nid) {
		if (me != 0) { return; }
		if (idx < 0 && nid >= 0 && NP_NID_IDX.has(nid)) { idx = NP_NID_IDX[nid]; }
		if (idx < 0 || idx >= MO_X.size() || MO_STATE[idx] != 7) { return; }
		pickup_despawn(idx);
		tx_all({"t": "ITEM", "s": 0, "i": nid >= 0 ? -1 : idx, "nid": nid, "by": from});
	}

	void host_use(int from, float x, float y, float ang, int keys) {
		if (me != 0) { return; }
		var pose = new BorrowedPose(from, x, y, ang, keys);
		p_use();                                     // no use-activated teleports exist: tele_ok stays false
		pose.restore();
	}

	void host_cross(int from, int ld, float x, float y, int side, int keys) {
		if (me != 0) { return; }
		if (ld < 0 || ld >= LD_SPECIAL.size()) { return; }
		var pose = new BorrowedPose(from, x, y, 0.0, keys);
		pose.tele_ok = np_is_tele_special(LD_SPECIAL[ld]);
		CROSS_SIDE = side;
		spec_line(ld, 1);
		pose.restore();
	}

	void replay_fire(var shooter, int wnum, float x, float y, float z, float ang) {
		var w = WEAPONS[wnum];
		if (w == null || shooter.ghost < 0) { return; }
		if (w.missile != 0) {
			NP_PROJ_SKIP = shooter.slot;             // the owner already sees its cosmetic twin
			rocket_spawn(x, y, z, ang, shooter.ghost);
			NP_PROJ_SKIP = -1;
			return;
		}
		int pellets = w.pellets;
		for (int p = 0; p < pellets; ++p) {
			float aim = ang;
			if (w.spread > 0.0) { aim = aim + (SIM_RANDOM.unit() - 0.5) * w.spread; }
			shoot_hitscan(x, y, z, aim, w.dmg_mul * SIM_RANDOM.between(1, w.dmg_roll), shooter.ghost, w.range);
		}
	}

	void coop_reset() {
		np_coop_reset_state();
	}
}

// host: the NetServer star hub — slots assigned on HELLO, everything relayed
class HostSession : Session {
	var srv = null;
	var cid_slot = {};            // NetServer client id -> slot
	var slot_cid = {};

	HostSession() : super() {}

	bool open(int port) {
		srv = new NetServer();
		srv.on_client_connect.connect("np", [](var cid) { SESSION.on_join(cid); });
		srv.on_client_packet.connect("np", [](var cid, var bytes) { SESSION.on_wire(cid, bytes); });
		srv.on_client_disconnect.connect("np", [](var cid, var reason) { SESSION.on_leave(cid, "" + reason); });
		if (!srv.listen(port)) {
			NP_LOBBY = "failed: port taken?";
			return false;
		}
		players[0] = new LocalPlayer(0, NP_NAME);
		NP_LOBBY = "listening";
		NP_LOBBY_T0 = time::now();
		return true;
	}

	void on_join(int cid) {
		if (stage == 3) {                            // no mid-game joins this pass
			srv.send(cid, to_json({"t": "BYE", "s": 0, "why": "game in progress"}));
			srv.kick(cid);
			return;
		}
	}

	// clients stamp their own slot; the host trusts-but-verifies against the wire id
	void on_wire(int cid, string bytes) {
		var m = from_json(bytes);
		if (m == null || !m.has("t")) { return; }
		var t = "" + m["t"];
		if (t == "HELLO") {
			hello(cid, m["proto"] + 0, m["wad"] + 0, "" + m["name"]);
			return;
		}
		if (!cid_slot.has(cid)) { return; }
		int slot = cid_slot[cid];
		int claimed = m.has("s") ? math::itrunc(m["s"] * 1.0) : slot;
		if (claimed != slot) { return; }             // spoofed sender: drop
		apply(slot, m);
		// fan out what the other clients need to see (host-authored actions and
		// routed DAMAGE never come through here); PROJ/EXPL carry dm rockets
		if (t == "STATE" || t == "FIRE" || t == "DIE" || t == "RESPAWN" || t == "PROJ" || t == "EXPL") {
			relay_others(slot, bytes);
		} else if (t == "DAMAGE") {
			int v = math::itrunc(m["v"] * 1.0);
			if (v != 0 && v > 0 && v < NP_MAX_PLAYERS && slot_cid.has(v)) {
				srv.send(slot_cid[v], bytes);        // victim-only routing
			}
			// v == 0 already applied above (the host is the victim)
		}
	}

	void hello(int cid, int proto, int wadsz, string name) {
		if (proto != NP_PROTO) { srv.send(cid, to_json({"t": "BYE", "s": 0, "why": "protocol mismatch"})); srv.kick(cid); return; }
		if (wadsz != WAD.size()) { srv.send(cid, to_json({"t": "BYE", "s": 0, "why": "wad mismatch"})); srv.kick(cid); return; }
		int slot = -1;
		for (int i = 1; i < NP_MAX_PLAYERS; ++i) {
			if (players[i] == null) { slot = i; break; }
		}
		if (slot < 0) { srv.send(cid, to_json({"t": "BYE", "s": 0, "why": "server full"})); srv.kick(cid); return; }
		cid_slot[cid] = slot;
		slot_cid[slot] = cid;
		players[slot] = new Puppet(slot, name);
		srv.send(cid, to_json({"t": "WELCOME", "s": 0, "slot": slot,
			"mode": coop ? "coop" : "dm", "mapname": mapname, "skill": skill, "roster": roster_batch()}));
		tx_others(slot, {"t": "JOINED", "s": 0, "slot": slot, "pname": name});
		NP_LOBBY = "listening";                      // refresh the count line
	}

	var roster_batch() {
		var k = "";
		for (int i = 0; i < NP_MAX_PLAYERS; ++i) {
			var p = players[i];
			if (p != null) { k += `${i}:${p.name} `; }
		}
		return k;
	}

	// host pressed enter in the lobby: everyone in the roster starts together
	void go() {
		if (peer_count() < 1) { return; }
		tx_all({"t": "GO", "s": 0, "mode": coop ? "coop" : "dm", "mapname": mapname, "skill": skill});
		begin();
	}

	void tx_slot(int slot, var m) override {
		if (slot_cid.has(slot)) { srv.send(slot_cid[slot], to_json(m)); }
	}

	void tx_others(int except_slot, var m) override {
		var bytes = to_json(m);
		for (var kv : slot_cid) {
			if (kv.first == except_slot) { continue; }
			srv.send(kv.second, bytes);
		}
	}

	void relay_others(int except_slot, string bytes) {
		for (var kv : slot_cid) {
			if (kv.first == except_slot) { continue; }
			srv.send(kv.second, bytes);
		}
	}

	void tx_all(var m) override {
		srv.broadcast(to_json(m));
	}

	void tx_host(var m) override { apply(0, m); }             // the host talking to itself

	int peer_count() -> int override { return srv == null ? 0 : math::itrunc(srv.client_count() * 1.0); }

	void on_leave(int cid, string reason) {
		if (!cid_slot.has(cid)) { return; }
		int slot = cid_slot[cid];
		cid_slot.remove(cid);
		slot_cid.remove(slot);
		tx_others(slot, {"t": "BYE_SLOT", "s": 0, "slot": slot});
		peer_left(slot, "disconnected (" + reason + ")");
		if (stage == 3 && peer_count() == 0 && !NP_SOLO_OK) {
			// everyone left mid-game: keep playing solo
			shutdown("everyone left");
		}
	}

	void pump() {
		if (srv != null) { srv.pump(); }
	}

	void wire_close() override {
		if (srv != null) { srv.close(); }
	}

	bool wire_connected() -> bool override { return srv != null && srv.client_count() > 0; }
}
var NP_SOLO_OK = false;

// client: one channel to the host; every packet is either host-authored or a
// relay (all slot-stamped, so apply() doesn't care which)
class ClientSession : Session {
	var ch = null;

	ClientSession() : super() {}

	bool open(string host, int port) {
		ch = new NetChannel();
		ch.on_connect.connect("np", []() { SESSION.on_up(); });
		ch.on_packet.connect("np", [](var bytes) { SESSION.on_wire_c(bytes); });
		ch.on_disconnect.connect("np", [](var reason) { SESSION.on_down("" + reason); });
		if (!ch.connect(host, port)) {
			NP_LOBBY = "failed: " + ch.status();
			return false;
		}
		NP_LOBBY = "connecting";
		NP_LOBBY_T0 = time::now();
		return true;
	}

	void on_up() {
		tx_host({"t": "HELLO", "proto": NP_PROTO, "wad": WAD.size(), "name": NP_NAME});
	}

	void on_wire_c(string bytes) {
		var m = from_json(bytes);
		if (m == null || !m.has("t")) { return; }
		var t = "" + m["t"];
		if (t == "WELCOME") {
			me = math::itrunc(m["slot"] * 1.0);
			coop = ("" + m["mode"]) == "coop";
			mapname = "" + m["mapname"];
			skill = m["skill"] + 0;
			players[me] = new LocalPlayer(me, NP_NAME);
			apply_roster("" + m["roster"]);
			NP_LOBBY = "joined";
			return;
		}
		if (t == "JOINED") {
			int slot = math::itrunc(m["slot"] * 1.0);
			if (players[slot] == null) {
				players[slot] = new Puppet(slot, "" + m["pname"]);
				if (stage == 3) {
					var p = players[slot];
					p.spawn_ghost(P_X, P_Y);
				}
			}
			return;
		}
		if (t == "BYE_SLOT") {
			peer_left(math::itrunc(m["slot"] * 1.0), "left");
			return;
		}
		if (t == "GO") {
			coop = ("" + m["mode"]) == "coop";
			mapname = "" + m["mapname"];
			skill = m["skill"] + 0;
			begin();
			return;
		}
		if (t == "BYE" && m.has("why")) {
			NP_LOBBY = "failed: " + m["why"];
			shutdown("" + m["why"]);
			return;
		}
		int from = m.has("s") ? math::itrunc(m["s"] * 1.0) : 0;
		apply(from, m);
	}

	void apply_roster(string k) {
		var parts = string::trim(k).split(" ");
		for (int i = 0; i < parts.size(); ++i) {
			int c = parts[i].find(":");
			if (c <= 0) { continue; }
			int slot = parts[i].substr(0, c).to_int(-1);
			var nm = parts[i].substr(c + 1, parts[i].size() - c - 1);
			if (slot >= 0 && slot < NP_MAX_PLAYERS && slot != me && players[slot] == null) {
				players[slot] = new Puppet(slot, nm);
			}
		}
	}

	void on_down(string reason) {
		if (NP_LOBBY == "connecting" || NP_LOBBY == "joined") { NP_LOBBY = "failed: " + reason; }
		shutdown(reason);
	}

	void tx_host(var m) override {
		if (ch != null && ch.connected()) { ch.send(to_json(m)); }
	}

	void tx_all(var m) override { tx_host(m); }      // clients can't broadcast; the host relays

	int peer_count() -> int override { return living_players() - 1; }

	void pump() {
		if (ch != null) { ch.pump(); }
	}

	void wire_close() override {
		if (ch != null) { ch.close(); }
	}

	bool wire_connected() -> bool override { return ch != null && ch.connected(); }
}

// ------------------------------------------------- co-op stream state (flat) ---
// (kept flat/global: the batch codecs are string-in string-out and the state
// survives session swaps only within one map — reset by coop_reset)

var NP_HURT_HOOK = -1;
var NP_MAP_MOBJS = 0;
var NP_MONS_KEY = 0;
var NP_SECT_F = [];
var NP_SECT_C = [];
var NPM_TX = [];
var NPM_TY = [];
var NP_NID_NEXT = 0;
var NP_NID_IDX = {};
var NP_IDX_NID = {};
var NP_TAKE_PEND = {};
var NP_READY_SET = {};
var NP_NEXT_SENT = false;
var NP_EXIT_SENT = false;
var NP_WI_T0 = null;          // host intermission entry (time::now()): the 10s-grace baseline
var NP_LAST_STATE = [];
var NP_PROJ_NEXT = 0;         // monotonic net ids (never reset: stale packets can't collide)
var NP_PROJ_MINE = {};        // my announced live projectiles: mobj idx -> net id
var NP_PROJ_TWIN = {};        // received twins: sender*100000+id -> local mobj idx
var NP_PROJ_SKIP = -1;        // replay_fire: owner slot whose cosmetic twin already exists

function np_coop_reset_state() -> void {
	NP_MAP_MOBJS = MO_X.size();
	NP_MONS_KEY = 0;
	NP_SECT_F = [];
	NP_SECT_C = [];
	for (int s = 0; s < SE_FLOORH.size(); ++s) {
		NP_SECT_F.push(SE_FLOORH[s]);
		NP_SECT_C.push(SE_CEILH[s]);
	}
	NPM_TX = [];
	NPM_TY = [];
	NP_NID_NEXT = 0;
	NP_NID_IDX = {};
	NP_IDX_NID = {};
	NP_TAKE_PEND = {};
	NP_READY_SET = {};
	NP_NEXT_SENT = false;
	NP_EXIT_SENT = false;
	NP_WI_T0 = null;
	NP_LAST_STATE = [];
	for (int m = 0; m < MO_X.size(); ++m) { NP_LAST_STATE.push(-999999); }
	NP_PROJ_MINE = {};
	NP_PROJ_TWIN = {};
}

// ------------------------------------------------------ projectile streaming ---
// The announcer (co-op: the host for every authoritative projectile; dm: each
// machine for its own rockets) sends PROJ once at spawn; receivers spawn a
// streamed twin that dead-reckons (doom projectiles fly straight — the spawn
// packet's momenta are the whole flight) and pops on the EXPL echo, its own
// wall/floor contact, or a lifetime guard when an EXPL goes missing.

function np_proj_announce(int m, string fsnd) -> void {
	if (SESSION == null || SESSION.stage != 3) { return; }
	if (MO_NODMG[m] != 0) { return; }                // twins and cosmetics never announce
	if (NP_COOP && !NP_IS_HOST) { return; }          // co-op: host authority only
	int id = NP_PROJ_NEXT;
	NP_PROJ_NEXT = NP_PROJ_NEXT + 1;
	NP_PROJ_MINE[m] = id;
	var act = {"t": "PROJ", "s": SESSION.me, "id": id, "spr": MO_SPR[m], "snd": fsnd,
		"x": MO_X[m], "y": MO_Y[m], "z": MO_Z[m],
		"mx": MO_MOMX[m], "my": MO_MOMY[m], "mz": MO_MOMZ[m]};
	if (NP_PROJ_SKIP >= 0 && SESSION.me == 0) { SESSION.tx_others(NP_PROJ_SKIP, act); }
	else { SESSION.announce(act); }
}

function np_expl_announce(int m) -> void {
	if (SESSION == null || SESSION.stage != 3) { return; }
	if (!NP_PROJ_MINE.has(m)) { return; }
	int id = NP_PROJ_MINE[m];
	NP_PROJ_MINE.remove(m);
	SESSION.announce({"t": "EXPL", "s": SESSION.me, "id": id, "x": MO_X[m], "y": MO_Y[m], "z": MO_Z[m]});
}

function np_proj_apply(int from, int id, string spr, string fsnd, float x, float y, float z, float mx, float my, float mz) -> void {
	if (fsnd != "") { snd_play_at(fsnd, x, y); }     // monster fireballs; player rockets ride FIRE
	int tw = mobj_spawn(x, y, 0, math::atan2(my, mx), 8, spr, spr == "MISL" ? "A" : "AB", 1, 6.0);
	MO_Z[tw] = z;
	MO_MOMX[tw] = mx;
	MO_MOMY[tw] = my;
	MO_MOMZ[tw] = mz;
	MO_NODMG[tw] = 2;                                // streamed: EXPL/walls own the pop, no contact checks
	MO_TARGET[tw] = -1;
	MO_COOL[tw] = 350;                               // missed-EXPL lifetime guard (~10s)
	NP_PROJ_TWIN[from * 100000 + id] = tw;
}

function np_expl_apply(int from, int id, float x, float y, float z) -> void {
	int key = from * 100000 + id;
	if (!NP_PROJ_TWIN.has(key)) { return; }          // owner-skip twins were never registered
	int tw = NP_PROJ_TWIN[key];
	NP_PROJ_TWIN.remove(key);
	if (tw < 0 || tw >= MO_X.size() || MO_STATE[tw] != 8) { return; }
	MO_X[tw] = x;
	MO_Y[tw] = y;
	MO_Z[tw] = z;
	projectile_explode(tw);
}

function np_mons_pack(int m) -> int {
	return math::itrunc(MO_X[m]) * 8192 + math::itrunc(MO_Y[m]) + MO_STATE[m] * 31 + MO_FRAME[m] * 7;
}

// "idx x y ang*100 state frame ..." — a compact flat wire format. Kept over
// nested JSON by measurement, not necessity: the byte cost is a wash, and the
// apply side would still decode via hoisted locals (range-for cannot traverse
// a nested payload read directly), so split-parse stays the simpler codec.
function np_mons_batch(bool keyframe) -> string {
	var k = "";
	int n = NP_MAP_MOBJS < MO_X.size() ? NP_MAP_MOBJS : MO_X.size();
	for (int m = 0; m < n; ++m) {
		if (!MINFO.has(MO_TYPE[m]) && MO_TYPE[m] != 2035) { continue; }
		int st = MO_STATE[m];
		if (st == 0 || st == 7 || st == 10) { continue; }
		int packed = np_mons_pack(m);
		if (!keyframe && packed == NP_LAST_STATE[m]) { continue; }
		NP_LAST_STATE[m] = packed;
		k += `${m} ${math::itrunc(MO_X[m])} ${math::itrunc(MO_Y[m])} ${math::itrunc(MO_ANG[m] * 100.0)} ${st} ${MO_FRAME[m]} `;
	}
	return k;
}

function npm_grow(int idx) -> void {
	while (NPM_TX.size() <= idx) {
		NPM_TX.push(-1000000.0);
		NPM_TY.push(0.0);
	}
}

function np_mons_apply(string batch) -> void {
	var k = string::trim(batch).split(" ");
	int i = 0;
	while (i + 5 < k.size()) {
		int idx = k[i].to_int(-1);
		if (idx < 0 || idx >= MO_X.size()) { i = i + 6; continue; }
		npm_grow(idx);
		NPM_TX[idx] = k[i + 1].to_int(0) * 1.0;
		NPM_TY[idx] = k[i + 2].to_int(0) * 1.0;
		MO_ANG[idx] = k[i + 3].to_int(0) / 100.0;
		int st = k[i + 4].to_int(0);
		int fr = k[i + 5].to_int(0);
		int cur = MO_STATE[idx];
		if (st == 5 || st == 6) {
			if (cur == 12) {                         // first sight of the death
				MO_STATE[idx] = 5;
				var mi = MINFO[MO_TYPE[idx]];
				MO_ANIM[idx] = mi[9];
				MO_FRAME[idx] = 0;
				MO_TIC[idx] = 5;
				snd_play_at(death_snd(MO_TYPE[idx]), MO_X[idx], MO_Y[idx]);
			}
		} else if (cur == 12) {
			// mirror the host's ANIM STRING, not just its frame number: an
			// attack/pain frame indexes the attack/pain letters, and landing it
			// in the walk run meant clients never saw a monster wind up
			if (MINFO.has(MO_TYPE[idx]) && st >= 2 && st <= 4) {
				var mi = MINFO[MO_TYPE[idx]];
				var want = "" + mi[6];
				if (st == 3) { want = "" + mi[7]; }
				else if (st == 4) { want = "" + mi[8]; }
				if (MO_ANIM[idx] != want) {
					MO_ANIM[idx] = want;
					if (st == 4) { snd_play_at("DSPOPAIN", MO_X[idx], MO_Y[idx]); }
				}
			}
			MO_FRAME[idx] = fr < MO_ANIM[idx].size() ? fr : 0;
		}
		i = i + 6;
	}
}

function np_sect_batch() -> string {
	var k = "";
	int n = SE_FLOORH.size();
	for (int s = 0; s < n; ++s) {
		if (SE_FLOORH[s] == NP_SECT_F[s] && SE_CEILH[s] == NP_SECT_C[s]) { continue; }
		NP_SECT_F[s] = SE_FLOORH[s];
		NP_SECT_C[s] = SE_CEILH[s];
		k += `${s} ${SE_FLOORH[s]} ${SE_CEILH[s]} `;
	}
	return k;
}

function np_sect_apply(string batch) -> void {
	var k = string::trim(batch).split(" ");
	int i = 0;
	while (i + 2 < k.size()) {
		int s = k[i].to_int(-1);
		if (s >= 0 && s < SE_FLOORH.size()) {
			SE_FLOORH[s] = k[i + 1].to_int(0);
			SE_CEILH[s] = k[i + 2].to_int(0);
		}
		i = i + 3;
	}
}

function np_boom_apply(int idx) -> void {
	if (idx >= 0 && idx < MO_X.size() && (MO_STATE[idx] == 9 || MO_STATE[idx] == 12)) {
		barrel_boom_cosmetic(idx);
	}
}

function np_drop_apply(int nid, int ty, string spr, float x, float y) -> void {
	int idx = mobj_spawn(x, y, ty, 0.0, 7, spr, "A", 0, 20.0);
	NP_NID_IDX[nid] = idx;
	NP_IDX_NID[idx] = nid;
}

function np_item_apply(int idx, int nid, int by) -> void {
	if (idx < 0 && nid >= 0 && NP_NID_IDX.has(nid)) { idx = NP_NID_IDX[nid]; }
	if (idx < 0 || idx >= MO_X.size()) { return; }
	if (SESSION != null && by == SESSION.me) { pickup_apply(idx); }
	else { pickup_despawn(idx); }
}

// puppets glide toward their last MONS positions (client only)
function np_puppet_lerp() -> void {
	int n = NPM_TX.size();
	for (int m = 0; m < n; ++m) {
		if (m >= MO_STATE.size() || MO_STATE[m] != 12) { continue; }
		if (NPM_TX[m] < -900000.0) { continue; }
		float dx = NPM_TX[m] - MO_X[m];
		float dy = NPM_TY[m] - MO_Y[m];
		if (dx * dx + dy * dy > 160.0 * 160.0) {
			MO_X[m] = NPM_TX[m];
			MO_Y[m] = NPM_TY[m];
		} else {
			MO_X[m] = MO_X[m] + dx * 0.25;
			MO_Y[m] = MO_Y[m] + dy * 0.25;
		}
		MO_Z[m] = SE_FLOORH[sector_at(MO_X[m], MO_Y[m])] * 1.0;
	}
}

// host replays a client press/crossing AT THE CLIENT'S POSE; restore() ships
// the destination back to THAT client as a TELE only when a genuine teleport
// special moved the BORROWED pose. Two guards, both load-bearing: `moved`
// must compare against the borrowed coords (comparing against the host's own
// saved pose made EVERY replay look like a teleport — the client got snapped
// to stale coords with a zeroed angle, i.e. the door rubber-band and the
// 180-degree turn at the E1M1 secret), and `tele_ok` pins the echo to actual
// teleport line types so pose drift can never masquerade as one.
class BorrowedPose {
	int who = 0;
	float sx = 0.0;
	float sy = 0.0;
	float sang = 0.0;
	int skeys = 0;
	float bx = 0.0;               // the borrowed pose = the only legal TELE baseline
	float by = 0.0;
	bool tele_ok = false;
	BorrowedPose(int slot, float x, float y, float ang, int keys) {
		who = slot;
		sx = P_X;
		sy = P_Y;
		sang = P_ANG;
		skeys = P_KEYS;
		bx = x;
		by = y;
		P_X = x;
		P_Y = y;
		P_ANG = ang;
		P_KEYS = keys;
	}
	void restore() {
		bool moved = P_X != bx || P_Y != by;
		float tx = P_X;
		float ty = P_Y;
		float ta = P_ANG;
		P_X = sx;
		P_Y = sy;
		P_ANG = sang;
		P_KEYS = skeys;
		if (tele_ok && moved && SESSION != null) { SESSION.tx_slot(who, {"t": "TELE", "s": 0, "x": tx, "y": ty, "ang": ta}); }
	}
}

function np_is_tele_special(int sp) -> bool {
	return sp == 39 || sp == 97;
}

// -------------------------------------------------- the np_* game boundary ---
// (free functions the rest of the game calls; they delegate to SESSION)

function np_lobby_host(int port, bool coop, string mapname, int skill) -> bool {
	try {
		var s = new HostSession();
		s.coop = coop;
		s.mapname = mapname;
		s.skill = skill;
		SESSION = s;
		if (!s.open(port)) { SESSION = null; return false; }
		return true;
	} catch (e) {
		// a copied jaiscript.exe from before the net layer has no NetServer
		set_msg("this jaiscript.exe predates networking - copy a fresh one");
		SESSION = null;
		return false;
	}
}

function np_lobby_join(string host, int port) -> bool {
	try {
		var s = new ClientSession();
		SESSION = s;
		if (!s.open(host, port)) { SESSION = null; return false; }
		return true;
	} catch (e) {
		set_msg("this jaiscript.exe predates networking - copy a fresh one");
		SESSION = null;
		return false;
	}
}

function np_lobby_frame() -> void {
	if (SESSION == null) { return; }
	SESSION.pump();
	if (NP_LOBBY == "connecting" && time::now() - NP_LOBBY_T0 > 10.0) {
		NP_LOBBY = "failed: no answer";
		np_cancel();
	}
}

// the host's lobby enter: launch with the current roster
function np_lobby_go() -> void {
	if (SESSION != null && SESSION.me == 0 && SESSION.stage == 0) { SESSION.go(); }
}

function np_lobby_count() -> int {
	if (SESSION == null) { return 0; }
	return SESSION.peer_count();
}

function np_cancel() -> void {
	if (SESSION == null) { return; }
	var keep = NP_LOBBY;
	SESSION.wire_close();
	SESSION.shutdown("cancelled");
	if (keep.starts_with("failed")) { NP_LOBBY = keep; }
}

function np_bye(string why) -> void {
	if (SESSION == null) { return; }
	SESSION.announce({"t": "BYE", "s": SESSION.me});
	SESSION.close_in = 8;                            // the wire delivers BYE before the close
	SESSION.shutdown(why);
}

function np_tic() -> void {
	if (SESSION != null) { SESSION.tic(); }
}

function np_frame() -> void {
	if (SESSION == null) { return; }
	SESSION.pump();
	if (SESSION == null) { return; }                 // the pump may deliver the session-ending BYE
	SESSION.frame_common();
}

function np_fire_send() -> void {
	if (SESSION == null || SESSION.stage != 3) { return; }
	SESSION.announce({"t": "FIRE", "s": SESSION.me, "weapon": CUR_WEAPON,
		"x": P_X, "y": P_Y, "z": P_Z, "ang": P_ANG});
}

// my shot connected with a ghost (mobj idx): route DAMAGE to that victim
function np_ghost_hit_mobj(int mobj, int dmg, float sx, float sy) -> void {
	if (SESSION == null) { return; }
	var p = SESSION.player_of_ghost(mobj);
	if (p == null) { return; }
	var m = {"t": "DAMAGE", "s": SESSION.me, "v": p.slot, "amount": dmg, "sx": sx, "sy": sy};
	if (SESSION.me == 0) { SESSION.tx_slot(p.slot, m); }
	else { SESSION.tx_host(m); }                     // the host routes it onward
}

function np_take_request(int m) -> void {
	if (SESSION == null) { return; }
	int key = NP_IDX_NID.has(m) ? 1000000 + NP_IDX_NID[m] : m;
	if (NP_TAKE_PEND.has(key) && GAME_TIC - NP_TAKE_PEND[key] < 35) { return; }
	NP_TAKE_PEND[key] = GAME_TIC;
	if (NP_IDX_NID.has(m)) { SESSION.tx_host({"t": "TAKE", "s": SESSION.me, "nid": NP_IDX_NID[m], "i": -1}); }
	else { SESSION.tx_host({"t": "TAKE", "s": SESSION.me, "i": m, "nid": -1}); }
}

// host took an item itself: same ITEM broadcast the clients get
function np_item_announce(int idx, bool mine) -> void {
	if (SESSION == null || SESSION.me != 0) { return; }
	int nid = NP_IDX_NID.has(idx) ? NP_IDX_NID[idx] : -1;
	SESSION.tx_all({"t": "ITEM", "s": 0, "i": nid >= 0 ? -1 : idx, "nid": nid, "by": 0});
}

function np_drop_announce(int idx) -> void {
	if (SESSION == null || SESSION.me != 0) { return; }
	int nid = NP_NID_NEXT;
	NP_NID_NEXT = NP_NID_NEXT + 1;
	NP_NID_IDX[nid] = idx;
	NP_IDX_NID[idx] = nid;
	SESSION.tx_all({"t": "DROP", "s": 0, "nid": nid, "spr": MO_SPR[idx], "ty": MO_TYPE[idx],
		"x": MO_X[idx], "y": MO_Y[idx]});
}

function np_boom_announce(int m) -> void {
	if (SESSION == null || SESSION.me != 0) { return; }
	SESSION.tx_all({"t": "BOOM", "s": 0, "i": m});
}

function np_switch_announce(int line) -> void {
	if (SESSION == null || SESSION.me != 0) { return; }
	SESSION.tx_all({"t": "SWITCHED", "s": 0, "line": line});
}

function np_use_send() -> void {
	if (SESSION == null) { return; }
	SESSION.tx_host({"t": "USE", "s": SESSION.me, "x": P_X, "y": P_Y, "ang": P_ANG, "keys": P_KEYS});
}

function np_cross_send(int ld, int side) -> void {
	if (SESSION == null) { return; }
	SESSION.tx_host({"t": "CROSS", "s": SESSION.me, "line": ld, "x": P_X, "y": P_Y, "side": side, "keys": P_KEYS});
}

function np_pct(int got, int total) -> int {
	if (total <= 0) { return 100; }
	return got * 100 / total;
}

function next_map_name() -> string {
	var nxt = next_map();
	if (nxt != "" && lump_num(nxt) < 0) { nxt = ""; }
	return nxt;
}

function np_next_announce(string nxt) -> void {
	if (SESSION == null || NP_NEXT_SENT) { return; }
	NP_NEXT_SENT = true;
	var m = {"t": "NEXT", "s": 0, "mapname": GAME_MAP, "next": nxt,
		"k": np_pct(KILLS, KILL_TOTAL), "i": np_pct(ITEMS_TAKEN, ITEM_TOTAL),
		"s2": np_pct(SECRETS_FOUND, SECRET_TOTAL), "secs": (GAME_TIC - LEVEL_START_TIC) / 35};
	SESSION.tx_all(m);
}

function np_start_announce(string mapname) -> void {
	if (SESSION == null || SESSION.me != 0) { return; }
	SESSION.tx_all({"t": "START", "s": 0, "mapname": mapname});
	NP_ALL_READY = false;
	NP_READY_SET = {};
	if (mapname != "") { SESSION.travel(mapname); }   // "" = episode over: both sides end
}

function np_ready_send() -> void {
	if (SESSION == null || SESSION.me == 0) { return; }
	SESSION.tx_host({"t": "READY", "s": SESSION.me});
}

// a netgame client's own LEVEL_DONE travels up as EXIT instead of acting
// locally (once per map; the host answers with NEXT for everyone)
function np_exit_send() -> void {
	if (SESSION == null || SESSION.me == 0 || NP_EXIT_SENT) { return; }
	NP_EXIT_SENT = true;
	SESSION.tx_host({"t": "EXIT", "s": SESSION.me, "sec": SECRET_EXIT});
}

// intermission consensus, recomputed fresh at each ask: a mid-intermission
// disconnect must never wedge the count a cached NP_ALL_READY froze
function np_ready_need() -> int {
	if (SESSION == null) { return 0; }
	return SESSION.living_players() - 1;
}

function np_ready_have() -> int {
	if (SESSION == null) { return 0; }
	int have = 0;
	for (var kv : NP_READY_SET) {
		int s = kv.first + 0;
		if (s > 0 && s < NP_MAX_PLAYERS && SESSION.players[s] != null) { have = have + 1; }
	}
	return have;
}

// the host may advance when everyone still present is ready, or once the 10s
// grace runs out (an afk client can't hold the level hostage)
function np_host_can_start() -> bool {
	if (SESSION == null) { return true; }
	int need = np_ready_need();
	if (need <= 0 || np_ready_have() >= need) { return true; }
	return NP_WI_T0 != null && time::now() - NP_WI_T0 > 10.0;
}

function np_wi_begin() -> void {
	NP_WI_T0 = time::now();
}

function np_next_map(string mapname) -> void {
	if (SESSION != null) { SESSION.travel(mapname); }
}

function np_respawn() -> void {
	if (SESSION != null) { SESSION.respawn(); }
}

function np_loadout_coop() -> void {
	int keys = P_KEYS;
	np_loadout();
	if (NP_COOP) { P_KEYS = keys; }
}

function np_loadout() -> void {
	P_HP = 100;
	P_ARMOR = 0;
	P_KEYS = 0;
	AMMO_BULLETS = 50;
	AMMO_SHELLS = 0;
	AMMO_ROCKETS = 0;
	HAVE_SHOTGUN = false;
	HAVE_CHAINGUN = false;
	HAVE_LAUNCHER = false;
	P_BERSERK = false;
	BERSERK_TICS = 0;
	P_INVIS = 0;
	P_SUIT = 0;
	P_GOGGLES = 0;
	CUR_WEAPON = 2;
	W_COOL = 0;
	PAIN_TICS = 0;
	BONUS_TICS = 0;
}

// scoreboard lines for the overlay: "name  frags/deaths" per live player
function np_scoreboard_lines() -> var {
	var lines = [];
	if (SESSION == null) { return lines; }
	for (int i = 0; i < NP_MAX_PLAYERS; ++i) {
		var p = SESSION.players[i];
		if (p == null) { continue; }
		var tag = i == SESSION.me ? " <" : "";
		lines.push(`${p.name}  ${p.frags}${tag}`);
	}
	return lines;
}

// CLI entry ('-- host'/'-- join'): the lobby machine does the rest
function np_start_mode(string mapname, bool host, string johost, int port, bool coop) -> bool {
	if (host) { return np_lobby_host(port, coop, mapname, SKILL_BIT); }
	return np_lobby_join(johost, port);
}

function np_start(string mapname, bool host, string johost, int port) -> bool {
	return np_start_mode(mapname, host, johost, port, false);
}
