// Gator Quest - multiplayer over the runner's NetServer/NetChannel. A star:
// the host owns the world and a NetServer; each client is one NetChannel.
// Clients send only intents; every sim tick the host sends each player a
// snapshot of what that player can know (gq_proto.jai), the same text a
// local player reads. One message per packet, a type then '|' fields:
//
//   client -> host   HELLO|proto|name|hue    M|dir (held)    S|seq|dir (a predicted step)    A|action    BYE
//   host -> client   WELCOME|proto|slot|seed|tick|motd    REFUSE|why
//                    SNAP followed by a newline and the snapshot    BYE|why
//
// Joiners' characters live on the host (saves/chars), restored on rejoin and
// written when they leave. The host quitting says BYE to everyone.

import "gq_game.jai";

int NET_PROTO = 1;
int NET_MAX_PLAYERS = 16;
string NET_CLOSE_WHY;       // why the interactive client's session ended (the title shows it)

// ------------------------------------------------------------------ host ---
bool net_host_start(int port) {
	NET_SRV = new NetServer();
	NET_SRV.on_client_connect.connect("gq", [](int id) { NH_KIND.push(0); NH_ID.push(id); NH_TXT.push(""); });
	NET_SRV.on_client_packet.connect("gq", [](int id, string bytes) { NH_KIND.push(1); NH_ID.push(id); NH_TXT.push(bytes); });
	NET_SRV.on_client_disconnect.connect("gq", [](int id, string reason) { NH_KIND.push(2); NH_ID.push(id); NH_TXT.push(reason); });
	NET_SLOT_OF = {};
	NET_CID_OF = {};
	NET_PORT = port;
	if (!NET_SRV.listen(port)) {
		NET_SRV = null;
		return false;
	}
	return true;
}

bool net_hosting() { return NET_SRV != null; }

int net_player_count() { return NET_CID_OF.size(); }

void net_send(int cid, string msg) {
	NET_SRV.send(cid, msg);
	NET_BYTES_OUT = NET_BYTES_OUT + msg.size();
}

void announce_all(string text, int color) {
	for (int s = 0; s < CHARS.size(); ++s) {
		if (CHARS[s].active) { say_to(s, text, color); }
	}
}

int free_slot() {
	for (int s = 0; s < NET_MAX_PLAYERS; ++s) {
		if (s >= CHARS.size() || !CHARS[s].active) { return s; }
	}
	return -1;
}

bool name_in_use(string name) {
	string low = string::to_lower(name);
	for (int s = 0; s < CHARS.size(); ++s) {
		if (CHARS[s].active && string::to_lower(CHARS[s].name) == low) { return true; }
	}
	return false;
}

string clean_name(string raw) {
	string n = string::trim(clean_text(raw));
	if (n.size() > 14) { n = n.substr(0, 14); }
	return n == "" ? "Gator" : n;
}

void net_host_hello(int cid, array<string>& f) {
	if (NET_SLOT_OF.has(cid)) { return; }
	if (f.size() < 4 || f[1].to_int(0) != NET_PROTO) {
		net_send(cid, `REFUSE|this host speaks protocol ${NET_PROTO}`);
		NET_SRV.kick(cid);
		return;
	}
	string name = clean_name(f[2]);
	int hue = iclamp(f[3].to_int(0), 0, 15);
	if (name_in_use(name)) {
		net_send(cid, `REFUSE|a gator named ${name} is already here`);
		NET_SRV.kick(cid);
		return;
	}
	int slot = free_slot();
	if (slot < 0) {
		net_send(cid, `REFUSE|the swamp is full (${NET_MAX_PLAYERS} gators)`);
		NET_SRV.kick(cid);
		return;
	}
	string welcome = join_character(slot, name, hue);
	ensure_slot(slot);
	NET_SHEET_SENT[slot] = -1;
	snap_reset(slot);
	NET_SLOT_OF[cid] = slot;
	NET_CID_OF[slot] = cid;
	net_send(cid, `WELCOME|${NET_PROTO}|${slot}|${SEED}|${TICK}|${clean_text(welcome)}`);
	say_to(slot, welcome, C_YELLOW);
	for (int s = 0; s < CHARS.size(); ++s) {
		if (s != slot && CHARS[s].active) { say_to(s, `${name} has joined the swamp.`, C_CYAN); }
	}
	int m = CHARS[slot].mob;
	fx_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], Fx.warp, 16);
	sound_at(MOB_WORLD[m], MOB_X[m], MOB_Y[m], "warp");
}

// a player leaves (BYE or a dropped connection): the character is written to its file first
void net_host_leave(int cid, string why) {
	if (!NET_SLOT_OF.has(cid)) { return; }
	int slot = NET_SLOT_OF[cid];
	NET_SLOT_OF.erase(cid);
	NET_CID_OF.erase(slot);
	if (slot >= CHARS.size() || !CHARS[slot].active) { return; }
	string name = CHARS[slot].name;
	make_dirs(save_dir() + "/chars");
	safe_write(char_path(name), char_text(slot));
	player_leave(slot);
	announce_all(`${name} has left the swamp (${why}).`, C_CYAN);
}

void net_host_packet(int cid, string msg) {
	int bar = msg.find("|");
	string kind = bar < 0 ? msg : msg.substr(0, bar);
	if (kind == "HELLO") {
		array<string> f = msg.split("|");
		net_host_hello(cid, f);
		return;
	}
	if (!NET_SLOT_OF.has(cid)) { return; }
	int slot = NET_SLOT_OF[cid];
	string rest = bar < 0 ? "" : msg.substr(bar + 1, msg.size() - bar - 1);
	if (kind == "S") {
		array<string> sf = rest.split("|");
		if (sf.size() >= 2) { intent_step(slot, sf[0].to_int(0), sf[1].to_int(-1)); }
	} else if (kind == "M") {
		int dir = rest.to_int(-1);
		intent_move(slot, dir >= 0 && dir < 8 ? dir : -1);
	} else if (kind == "A") {
		// at most a handful of actions per tick per player
		if (rest.size() > 0 && rest.size() <= 160 && IN_QUEUE[slot].size() < 12) { intent_act(slot, rest); }
	} else if (kind == "BYE") {
		net_host_leave(cid, "logged out");
		NET_SRV.kick(cid);
	}
}

// drain the socket and handle everything that arrived, in order
void net_host_pump() {
	if (NET_SRV == null) { return; }
	NET_SRV.pump();
	int n = NH_KIND.size();
	if (n == 0) { return; }
	array<int> kinds = NH_KIND;
	array<int> ids = NH_ID;
	array<string> txts = NH_TXT;
	NH_KIND = [];
	NH_ID = [];
	NH_TXT = [];
	for (int i = 0; i < n; ++i) {
		if (kinds[i] == 1) { net_host_packet(ids[i], txts[i]); }
		else if (kinds[i] == 2) { net_host_leave(ids[i], txts[i] == "closed by peer" ? "connection closed" : txts[i]); }
	}
}

// after every sim tick: one snapshot per connected player
void host_after_tick() {
	if (NET_SRV == null) { return; }
	array<int> slots = NET_CID_OF.keys();
	for (int k = 0; k < slots.size(); ++k) {
		int slot = slots[k];
		if (slot >= CHARS.size() || !CHARS[slot].active) { continue; }
		net_send(NET_CID_OF[slot], "SNAP\n" + snapshot_for(slot));
		NET_SNAPS_OUT = NET_SNAPS_OUT + 1;
	}
}

// the host is going away: characters to disk, a goodbye to everyone
void net_host_stop(string why) {
	if (NET_SRV == null) { return; }
	array<int> cids = NET_SLOT_OF.keys();
	for (int k = 0; k < cids.size(); ++k) {
		int slot = NET_SLOT_OF[cids[k]];
		if (slot < CHARS.size() && CHARS[slot].active) {
			make_dirs(save_dir() + "/chars");
			safe_write(char_path(CHARS[slot].name), char_text(slot));
		}
		net_send(cids[k], "BYE|" + why);
	}
	NET_SRV.pump();
	NET_SRV.close();
	NET_SRV = null;
	NET_SLOT_OF = {};
	NET_CID_OF = {};
}

// ------------------------------------------------------------- simulator ---
// added latency, jitter and TCP-style loss on a client session, for testing prediction.
// Messages keep their order (a delayed message delays the ones behind it, like TCP).
bool net_sim_on() { return NET_SIM_LAG > 0 || NET_SIM_JITTER > 0 || NET_SIM_LOSS > 0; }

int net_sim_rand(int n) {
	NET_SIM_SEED = (NET_SIM_SEED * 1103515245 + 12345) & 2147483647;
	return n <= 1 ? 0 : (NET_SIM_SEED >> 8) % n;
}

int net_sim_delay() {
	int d = NET_SIM_LAG / 2;
	if (NET_SIM_JITTER > 0) { d = d + net_sim_rand(NET_SIM_JITTER * 2 + 1) - NET_SIM_JITTER; }
	if (NET_SIM_LOSS > 0 && net_sim_rand(1000) < NET_SIM_LOSS * 10) { d = d + 250; }
	return imax(0, d);
}

// ---------------------------------------------------------------- client ---
int net_client_open(string host, int port) {
	int s = NCS_CH.size();
	shared_ptr<NetChannel> ch = new NetChannel();
	NCS_CH.push(ch);
	NCS_INBOX.push([]);
	NCS_OUT_DUE.push([]);
	NCS_OUT_MSG.push([]);
	NCS_IN_DUE.push([]);
	NCS_IN_MSG.push([]);
	NCS_OUT_LAST.push(0);
	NCS_IN_LAST.push(0);
	shared_ptr<ClientView> view = new ClientView();
	NCS_VIEW.push(view);
	NCS_STATUS.push("connecting");
	NCS_SLOT.push(-1);
	NCS_SEED.push(0);
	NCS_DROP.push("");
	NCS_TX.push(0);
	int k = s;
	ch.on_packet.connect("gq", [k](string bytes) { NCS_INBOX[k].push(bytes); });
	ch.on_disconnect.connect("gq", [k](string reason) { NCS_DROP[k] = reason; });
	if (!ch.connect(host, port)) { NCS_STATUS[s] = "closed: " + ch.status(); }
	return s;
}

void ncs_send(int s, string msg) {
	NCS_CH[s].send(msg);
	NCS_TX[s] = NCS_TX[s] + msg.size();
}

void net_client_hello(int s, string name, int hue) {
	ncs_send(s, `HELLO|${NET_PROTO}|${clean_text(name)}|${hue}`);
	if (NCS_STATUS[s] == "connecting") { NCS_STATUS[s] = "hello"; }
}

bool net_session_live(int s) { return s >= 0 && s < NCS_CH.size() && !NCS_STATUS[s].starts_with("closed") && !NCS_STATUS[s].starts_with("refused"); }

void net_session_send(int s, string msg) {
	if (!net_session_live(s)) { return; }
	if (!net_sim_on()) {
		ncs_send(s, msg);
		return;
	}
	int due = imax(NCS_OUT_LAST[s], NOW_MS + net_sim_delay());
	NCS_OUT_LAST[s] = due;
	NCS_OUT_DUE[s].push(due);
	NCS_OUT_MSG[s].push(msg);
}

// pump one session and apply what arrived (after the simulator's delay, when it is on)
void net_session_pump(int s) {
	if (s < 0 || s >= NCS_CH.size()) { return; }
	while (NCS_OUT_DUE[s].size() > 0 && NCS_OUT_DUE[s][0] <= NOW_MS) {
		if (net_session_live(s)) { ncs_send(s, NCS_OUT_MSG[s][0]); }
		NCS_OUT_DUE[s].remove(0);
		NCS_OUT_MSG[s].remove(0);
	}
	if (net_session_live(s)) { NCS_CH[s].pump(); }
	if (net_sim_on() && NCS_INBOX[s].size() > 0) {
		for (int i = 0; i < NCS_INBOX[s].size(); ++i) {
			int due = imax(NCS_IN_LAST[s], NOW_MS + net_sim_delay());
			NCS_IN_LAST[s] = due;
			NCS_IN_DUE[s].push(due);
			NCS_IN_MSG[s].push(NCS_INBOX[s][i]);
		}
		NCS_INBOX[s] = [];
	}
	while (NCS_IN_DUE[s].size() > 0 && NCS_IN_DUE[s][0] <= NOW_MS) {
		NCS_INBOX[s].push(NCS_IN_MSG[s][0]);
		NCS_IN_DUE[s].remove(0);
		NCS_IN_MSG[s].remove(0);
	}
	int n = NCS_INBOX[s].size();
	if (n > 0) {
		array<string> msgs = NCS_INBOX[s];
		NCS_INBOX[s] = [];
		for (int i = 0; i < n; ++i) {
			string msg = msgs[i];
			if (msg.starts_with("SNAP\n")) {
				ClientView& v = NCS_VIEW[s];
				v.rx_ms = NOW_MS;
				cv_apply(v, msg.substr(5, msg.size() - 5));
				continue;
			}
			array<string> f = msg.split("|");
			if (f[0] == "WELCOME" && f.size() >= 5) {
				NCS_SLOT[s] = f[2].to_int(-1);
				NCS_SEED[s] = f[3].to_int(0);
				NCS_STATUS[s] = "joined";
				NCS_VIEW[s].slot = NCS_SLOT[s];
			} else if (f[0] == "REFUSE") {
				NCS_STATUS[s] = "refused: " + (f.size() > 1 ? f[1] : "no reason");
			} else if (f[0] == "BYE") {
				NCS_STATUS[s] = "closed: " + (f.size() > 1 ? f[1] : "bye");
			}
		}
	}
	if (NCS_DROP[s] != "" && net_session_live(s)) { NCS_STATUS[s] = "closed: " + NCS_DROP[s]; }
}

void net_session_bye(int s) {
	if (!net_session_live(s)) { return; }
	ncs_send(s, "BYE");
	NCS_CH[s].pump();
	NCS_CH[s].close();
	NCS_STATUS[s] = "closed: logged out";
}

// ---- the interactive client's hooks (gq_game.jai calls these)
void net_client_send(string msg) { net_session_send(NET_SESSION, msg); }

void net_client_pump() {
	if (NET_SESSION < 0) { return; }
	net_session_pump(NET_SESSION);
	string st = NCS_STATUS[NET_SESSION];
	if (st.starts_with("closed") && NET_CLOSE_F == 0) {
		string why = st.substr(8, st.size() - 8);
		show_banner("DISCONNECTED", rgb(240, 80, 60), why);
		cv_log(CV, "The connection to the host ended: " + why, 2);
		NET_CLOSE_F = 120;
	}
	if (NET_CLOSE_F > 0) {
		NET_CLOSE_F = NET_CLOSE_F - 1;
		if (NET_CLOSE_F == 0) {
			NET_CLOSE_WHY = st.substr(8, st.size() - 8);
			PLAY_EXIT = "title";
			QUIT = true;
		}
	}
}

// connect, say hello and wait for the answer; "" on success, else why not
string net_join(string host, int port, string name, int hue, bool prepare_world) {
	int s = net_client_open(host, port);
	NET_SESSION = s;
	int waited = 0;
	bool said = false;
	while (waited < 800) {
		NOW_MS = cast<int>(time::now() * 1000);
		net_session_pump(s);
		string st = NCS_STATUS[s];
		if (st == "joined") { break; }
		if (st.starts_with("closed") || st.starts_with("refused")) { return st; }
		if (!said && NCS_CH[s].connected()) {
			net_client_hello(s, name, hue);
			said = true;
		}
		sleep(0.01);
		waited = waited + 1;
	}
	if (NCS_STATUS[s] != "joined") { return "no answer from " + host; }
	net_join_finish(s, prepare_world);
	return "";
}

// a welcomed session becomes the interactive client: the host's world (terrain from its seed), our slot, its view
void net_join_finish(int s, bool prepare_world) {
	NET_SESSION = s;
	if (prepare_world) {
		sim_reset();
		world_generate(NCS_SEED[s]);
		WORLD_READY = true;
	}
	NET_CLIENT = true;
	LOCAL_SLOT = NCS_SLOT[s];
	CV = NCS_VIEW[s];
}
