// Gator Quest - the snapshot protocol. Every tick the host writes each player
// a text snapshot of what that player can know: the mobiles and items around
// them, the events they should see, their own vitals, and their character
// sheet when it changed. The local player reads the same text as a remote
// one; the renderer only ever sees a ClientView filled from it.
//
// One record per line, fields split by '|', text fields last:
//   S|tick|world|myser|weather|tod|day          Y|hp|hpmax|mp|mpmax|target|action|left|ghost
//   M|ser|kind|x|y|px|py|mt|md|dir|hp%|flags|role|slot|hue|name
//   I|ser|base|rarity|x|y|qty                   T|index|tile (after TC: terrain overrides)
//   E|kind|w|x|y|a|b|c|text                     C|... sheet, K skills, P pack, W worn, B bank, L spells, J journal

import "gq_bot.jai";

int VIEW_R = 24;               // snapshot radius in tiles

string clean_text(string s) {
	string out = s;
	out.replace("|", "/");
	out.replace("\n", " ");
	return out;
}

string enc_item(Item it) {
	return `${it.base}.${it.qty}.${it.rarity}.${it.prefix}.${it.suffix}.${it.pmag}.${it.smag}.${it.bonus}`;
}

Item dec_item(string s) {
	array<string> f = s.split(".");
	Item it = Item();
	if (f.size() < 8) { return it; }
	it.base = f[0].to_int(-1);
	it.qty = f[1].to_int(0);
	it.rarity = f[2].to_int(0);
	it.prefix = f[3].to_int(-1);
	it.suffix = f[4].to_int(-1);
	it.pmag = f[5].to_int(0);
	it.smag = f[6].to_int(0);
	it.bonus = f[7].to_int(0);
	if (it.base < 0 || it.base >= ITEMS.size()) { it.base = -1; }
	if (it.prefix >= AFFIXES.size()) { it.prefix = -1; }
	if (it.suffix >= AFFIXES.size()) { it.suffix = -1; }
	return it;
}

string enc_items(array<Item>& items) {
	string out = "";
	for (int i = 0; i < items.size(); ++i) {
		if (i > 0) { out += ","; }
		out += enc_item(items[i]);
	}
	return out;
}

array<Item> dec_items(string s) {
	array<Item> out;
	if (s == "") { return out; }
	array<string> parts = s.split(",");
	for (int i = 0; i < parts.size(); ++i) { out.push(dec_item(parts[i])); }
	return out;
}

string enc_ints(array<int>& v) {
	string out = "";
	for (int i = 0; i < v.size(); ++i) {
		if (i > 0) { out += ","; }
		out += to_string(v[i]);
	}
	return out;
}

array<int> dec_ints(string s) {
	array<int> out;
	if (s == "") { return out; }
	array<string> parts = s.split(",");
	for (int i = 0; i < parts.size(); ++i) { out.push(parts[i].to_int(0)); }
	return out;
}

// ---------------------------------------------------------------- host ---
bool event_for(int e, int slot, int w, int x, int y) {
	int to = EV_TO[e];
	if (to >= 0) { return to == slot; }
	if (EV_KIND[e] == Ev.chat) {
		// party chat reaches the party anywhere; open chat reaches the same realm
		if (EV_C[e] == 1) { return EV_B[e] >= 0 && CHARS[slot].party == EV_B[e]; }
		return EV_W[e] == w;
	}
	if (EV_W[e] < 0) { return true; }
	return EV_W[e] == w && cheb(EV_X[e], EV_Y[e], x, y) <= VIEW_R + 6;
}

string snapshot_for(int slot) {
	while (NET_SHEET_SENT.size() <= slot) { NET_SHEET_SENT.push(-1); }
	Character& c = CHARS[slot];
	int me = c.mob;
	return snapshot_view(slot, MOB_WORLD[me], MOB_X[me], MOB_Y[me]);
}

// a player's delta memory: cleared on join so its first snapshot is full
void snap_reset(int slot) {
	while (SNAP_MOBS.size() <= slot) { SNAP_MOBS.push({}); SNAP_ITEMS.push({}); SNAP_WORLD.push(-1); }
	SNAP_MOBS[slot] = {};
	SNAP_ITEMS[slot] = {};
	SNAP_WORLD[slot] = -1;
}

// the visible records, full or as changes against what this player already has.
// kind "M" mobiles, "I" items; removals are "X|ser" and "Z|ser"
string snap_records(int slot, array<int>& sers, array<string>& recs, bool delta, bool mobs) {
	string out = "";
	if (!delta) {
		for (int i = 0; i < recs.size(); ++i) { out += recs[i]; }
		if (slot >= 0) {
			map<int, string> fresh;
			for (int i = 0; i < recs.size(); ++i) { fresh[sers[i]] = recs[i]; }
			if (mobs) { SNAP_MOBS[slot] = fresh; } else { SNAP_ITEMS[slot] = fresh; }
		}
		return out;
	}
	if (mobs) { return snap_delta(SNAP_MOBS[slot], sers, recs, "X|"); }
	return snap_delta(SNAP_ITEMS[slot], sers, recs, "Z|");
}

string snap_delta(map<int, string>& last, array<int>& sers, array<string>& recs, string removal) {
	string out = "";
	map<int, int> seen;
	for (int i = 0; i < recs.size(); ++i) {
		int ser = sers[i];
		seen[ser] = 1;
		if (last.get(ser, "") != recs[i]) {
			out += recs[i];
			last[ser] = recs[i];
		}
	}
	array<int> known = last.keys();
	for (int k = 0; k < known.size(); ++k) {
		if (!seen.has(known[k])) {
			out += removal + to_string(known[k]) + "\n";
			last.erase(known[k]);
		}
	}
	return out;
}

// what a camera at (w, x, y) sees; slot < 0 = a spectator (title screen) with no sheet or vitals
string snapshot_view(int slot, int w, int x, int y) {
	int me = slot >= 0 ? CHARS[slot].mob : -1;
	bool delta = false;
	if (slot >= 0) {
		while (SNAP_MOBS.size() <= slot) { SNAP_MOBS.push({}); SNAP_ITEMS.push({}); SNAP_WORLD.push(-1); }
		delta = SNAP_DELTAS && SNAP_WORLD[slot] == w;
		SNAP_WORLD[slot] = w;
	}
	string out = `S|${TICK}|${w}|${me >= 0 ? MOB_SER[me] : -1}|${W_WEATHER[w]}|${time_of_day()}|${day_number()}|${delta ? 1 : 0}\n`;
	if (me >= 0) {
		Character& c = CHARS[slot];
		int target = MOB_TARGET[me];
		int tser = mob_live(target) ? MOB_SER[target] : -1;
		int left = c.action != 0 ? imax(0, c.action_t - TICK) : 0;
		step_queue_ensure(slot);
		// the last predicted step this host processed, and where that left the gator (the client replays the rest)
		out += `Y|${MOB_HP[me]}|${MOB_HPMAX[me]}|${MOB_MP[me]}|${MOB_MPMAX[me]}|${tser}|${c.action}|${left}|${mob_ghost(me) ? 1 : 0}`;
		out += `|${IN_ACK[slot]}|${MOB_X[me]}|${MOB_Y[me]}|${MOB_SLOW[me] > 0 ? 1 : 0}|${MOB_AUX[me]}\n`;
	}
	int ww = W_W[w];
	int wh = W_H[w];
	int x0 = imax(0, x - VIEW_R);
	int x1 = imin(ww - 1, x + VIEW_R);
	int y0 = imax(0, y - VIEW_R);
	int y1 = imin(wh - 1, y + VIEW_R);
	array<int> msers;
	array<string> mrecs;
	for (int ty = y0; ty <= y1; ++ty) {
		int row = W_OFF[w] + ty * ww;
		for (int tx = x0; tx <= x1; ++tx) {
			int m = OCC[row + tx] - 1;
			if (m < 0) { continue; }
			string rec = mob_record(m, slot);
			if (rec != "") { msers.push(MOB_SER[m]); mrecs.push(rec); }
		}
	}
	// ghosts do not occupy tiles; players who died still show
	for (int s = 0; s < CHARS.size(); ++s) {
		int pm = CHARS[s].mob;
		if (s == slot || !CHARS[s].active || !mob_live(pm) || !mob_ghost(pm) || MOB_WORLD[pm] != w) { continue; }
		if (cheb(MOB_X[pm], MOB_Y[pm], x, y) <= VIEW_R) { msers.push(MOB_SER[pm]); mrecs.push(mob_record(pm, slot)); }
	}
	if (me >= 0 && mob_ghost(me)) { msers.push(MOB_SER[me]); mrecs.push(mob_record(me, slot)); }
	array<int> isers;
	array<string> irecs;
	for (int i = 0; i < GI_ITEM.size(); ++i) {
		if (GI_ITEM[i].base < 0 || GI_W[i] != w || cheb(GI_X[i], GI_Y[i], x, y) > VIEW_R) { continue; }
		isers.push(GI_SER[i]);
		irecs.push(`I|${GI_SER[i]}|${GI_ITEM[i].base}|${GI_ITEM[i].rarity}|${GI_X[i]}|${GI_Y[i]}|${GI_ITEM[i].qty}\n`);
	}
	out += snap_records(slot, msers, mrecs, delta, true);
	out += snap_records(slot, isers, irecs, delta, false);
	if (slot < 0) { return out; }
	Character& c = CHARS[slot];
	if (TICK % 10 == 0 || c.version != NET_SHEET_SENT[slot]) {
		out += "TC\n";
		for (int ty = y0; ty <= y1; ++ty) {
			for (int tx = x0; tx <= x1; ++tx) {
				int i = W_OFF[w] + ty * ww + tx;
				if (REGROW[i] > TICK) { out += `T|${ty * ww + tx}|${tile_now(w, tx, ty)}\n`; }
			}
		}
	}
	for (int e = 0; e < EV_KIND.size(); ++e) {
		if (!event_for(e, slot, w, x, y)) { continue; }
		out += `E|${EV_KIND[e]}|${EV_W[e]}|${EV_X[e]}|${EV_Y[e]}|${EV_A[e]}|${EV_B[e]}|${EV_C[e]}|${clean_text(EV_TEXT[e])}\n`;
	}
	if (c.version != NET_SHEET_SENT[slot]) {
		NET_SHEET_SENT[slot] = c.version;
		out += sheet_record(slot);
	}
	return out;
}

string mob_record(int m, int viewer) {
	int hpp = MOB_HPMAX[m] > 0 ? MOB_HP[m] * 100 / MOB_HPMAX[m] : 0;
	int slot = MOB_PLAYER[m];
	string name = slot >= 0 ? clean_text(CHARS[slot].name) : "";
	int hue = slot >= 0 ? CHARS[slot].hue : 0;
	int flags = MOB_FLAGS[m];
	// hidden things are hidden (except from themselves)
	if ((flags & MF_HIDDEN) != 0 && slot != viewer) { return ""; }
	return `M|${MOB_SER[m]}|${MOB_KIND[m]}|${MOB_X[m]}|${MOB_Y[m]}|${MOB_PX[m]}|${MOB_PY[m]}|${MOB_MOVE_T[m]}|${MOB_MOVE_D[m]}|${MOB_DIR[m]}|${hpp}|${flags}|${MOB_ROLE[m]}|${slot}|${hue}|${name}\n`;
}

string sheet_record(int slot) {
	Character& c = CHARS[slot];
	string out = `C|${clean_text(c.name)}|${clean_text(player_title(slot))}|${c.hue}|${player_str(slot)}|${player_dex(slot)}|${player_int(slot)}`;
	out += `|${c.gold}|${c.bank_gold}|${c.fame}|${c.karma}|${player_armor(slot)}|${player_luck(slot)}|${c.pvp ? 1 : 0}|${c.party}`;
	out += `|${imax(0, c.buff_night - TICK)}|${imax(0, c.buff_fire - TICK)}|${imax(0, c.buff_regen - TICK)}|${c.version}\n`;
	out += "K|" + enc_ints(c.skill) + "\n";
	out += "P|" + enc_items(c.pack) + "\n";
	out += "W|" + enc_items(c.worn) + "\n";
	out += "B|" + enc_items(c.bank) + "\n";
	out += "L|" + enc_ints(c.spells) + "\n";
	array<string> j = journal_lines(slot);
	for (int i = 0; i < j.size(); ++i) { out += "J|" + clean_text(j[i]) + "\n"; }
	string party = "";
	for (int s = 0; s < CHARS.size(); ++s) {
		if (s != slot && CHARS[s].active && c.party >= 0 && CHARS[s].party == c.party) {
			int pm = CHARS[s].mob;
			int hpp = mob_live(pm) && MOB_HPMAX[pm] > 0 ? MOB_HP[pm] * 100 / MOB_HPMAX[pm] : 0;
			party += `${clean_text(CHARS[s].name)}:${hpp},`;
		}
	}
	out += "Q|" + party + "\n";
	return out;
}

// ---------------------------------------------------------------- client ---
void cv_log(ClientView& v, string text, int col) {
	v.log.push(text);
	v.log_col.push(col);
	if (v.log.size() > 200) {
		v.log.remove(0);
		v.log_col.remove(0);
	}
}

void cv_apply(ClientView& v, string snap) {
	array<string> lines = snap.lines();
	for (int li = 0; li < lines.size(); ++li) {
		string line = lines[li];
		if (line.size() < 1) { continue; }
		array<string> f = line.split("|");
		string k = f[0];
		if (k == "S" && f.size() >= 7) {
			v.tick = f[1].to_int(0);
			int nw = f[2].to_int(0);
			if (nw != v.world) { v.tover = {}; }
			v.world = nw;
			v.my_ser = f[3].to_int(-1);
			v.weather = f[4].to_int(0);
			v.tod = f[5].to_int(0);
			v.day = f[6].to_int(1);
			v.snaps = v.snaps + 1;
			// a delta snapshot changes the picture we have; a full one replaces it
			if (f.size() >= 8 && f[7] == "1") { continue; }
			v.m_ser = [];
			v.m_kind = [];
			v.m_x = [];
			v.m_y = [];
			v.m_px = [];
			v.m_py = [];
			v.m_mt = [];
			v.m_md = [];
			v.m_ppx = [];
			v.m_ppy = [];
			v.m_pmt = [];
			v.m_pmd = [];
			v.m_dir = [];
			v.m_hp = [];
			v.m_flags = [];
			v.m_role = [];
			v.m_slot = [];
			v.m_hue = [];
			v.m_name = [];
			v.i_ser = [];
			v.i_base = [];
			v.i_rar = [];
			v.i_x = [];
			v.i_y = [];
			v.i_qty = [];
		} else if (k == "Y" && f.size() >= 9) {
			v.hp = f[1].to_int(0);
			v.hpmax = imax(1, f[2].to_int(1));
			v.mp = f[3].to_int(0);
			v.mpmax = imax(1, f[4].to_int(1));
			v.target_ser = f[5].to_int(-1);
			v.action = f[6].to_int(0);
			v.action_left = f[7].to_int(0);
			v.ghost = f[8].to_int(0);
			if (f.size() >= 14) {
				v.ack = f[9].to_int(0);
				v.auth_x = f[10].to_int(0);
				v.auth_y = f[11].to_int(0);
				v.slowed = f[12] == "1";
				v.sliding = f[13].to_int(0) != 0;
				v.has_auth = true;
				v.auth_fresh = true;
			}
		} else if (k == "M" && f.size() >= 16) {
			int ser = f[1].to_int(0);
			int i = cv_find(v, ser);
			bool fresh_mob = i < 0;
			if (fresh_mob) {
				i = v.m_ser.size();
				v.m_ser.push(ser);
				v.m_kind.push(0);
				v.m_x.push(0);
				v.m_y.push(0);
				v.m_px.push(0);
				v.m_py.push(0);
				v.m_mt.push(0);
				v.m_md.push(1);
				v.m_ppx.push(0);
				v.m_ppy.push(0);
				v.m_pmt.push(0);
				v.m_pmd.push(1);
				v.m_dir.push(4);
				v.m_hp.push(100);
				v.m_flags.push(0);
				v.m_role.push(-1);
				v.m_slot.push(-1);
				v.m_hue.push(0);
				v.m_name.push("");
			}
			int mt = f[7].to_int(0);
			int md = imax(1, f[8].to_int(1));
			if (fresh_mob || mt != v.m_mt[i]) {
				// a new move: the current one becomes the previous (a mobile seen for the first time has none)
				v.m_ppx[i] = fresh_mob ? f[5].to_int(0) : v.m_px[i];
				v.m_ppy[i] = fresh_mob ? f[6].to_int(0) : v.m_py[i];
				v.m_pmt[i] = fresh_mob ? mt - md : v.m_mt[i];
				v.m_pmd[i] = fresh_mob ? md : v.m_md[i];
			}
			v.m_kind[i] = iclamp(f[2].to_int(0), 0, MONS.size() - 1);
			v.m_x[i] = f[3].to_int(0);
			v.m_y[i] = f[4].to_int(0);
			v.m_px[i] = f[5].to_int(0);
			v.m_py[i] = f[6].to_int(0);
			v.m_mt[i] = mt;
			v.m_md[i] = md;
			v.m_dir[i] = f[9].to_int(4);
			v.m_hp[i] = f[10].to_int(100);
			v.m_flags[i] = f[11].to_int(0);
			v.m_role[i] = f[12].to_int(-1);
			v.m_slot[i] = f[13].to_int(-1);
			v.m_hue[i] = f[14].to_int(0);
			v.m_name[i] = f[15];
		} else if (k == "X" && f.size() >= 2) {
			int i = cv_find(v, f[1].to_int(0));
			if (i >= 0) { cv_remove_mob(v, i); }
		} else if (k == "I" && f.size() >= 7) {
			int base = f[2].to_int(-1);
			if (base < 0 || base >= ITEMS.size()) { continue; }
			int ser = f[1].to_int(0);
			int i = cv_find_item(v, ser);
			if (i < 0) {
				i = v.i_ser.size();
				v.i_ser.push(ser);
				v.i_base.push(0);
				v.i_rar.push(0);
				v.i_x.push(0);
				v.i_y.push(0);
				v.i_qty.push(1);
			}
			v.i_base[i] = base;
			v.i_rar[i] = f[3].to_int(0);
			v.i_x[i] = f[4].to_int(0);
			v.i_y[i] = f[5].to_int(0);
			v.i_qty[i] = f[6].to_int(1);
		} else if (k == "Z" && f.size() >= 2) {
			int i = cv_find_item(v, f[1].to_int(0));
			if (i >= 0) { cv_remove_item(v, i); }
		} else if (k == "TC") {
			v.tover = {};
		} else if (k == "T" && f.size() >= 3) {
			v.tover[f[1].to_int(0)] = f[2].to_int(0);
		} else if (k == "E" && f.size() >= 9) {
			int kind = f[1].to_int(0);
			string text = f[8];
			for (int extra = 9; extra < f.size(); ++extra) { text = text + "/" + f[extra]; }
			if (kind == Ev.log) { cv_log(v, text, f[5].to_int(0)); }
			else if (kind == Ev.chat) { cv_log(v, text, f[7].to_int(0) == 1 ? 5 : 0); }
			v.e_kind.push(kind);
			v.e_w.push(f[2].to_int(0));
			v.e_x.push(f[3].to_int(0));
			v.e_y.push(f[4].to_int(0));
			v.e_a.push(f[5].to_int(0));
			v.e_b.push(f[6].to_int(0));
			v.e_c.push(f[7].to_int(0));
			v.e_text.push(text);
			if (kind == Ev.dialog) { v.dlg_ser = f[5].to_int(-1); v.dlg_role = f[6].to_int(-1); v.dlg_pending = true; }
		} else if (k == "C" && f.size() >= 19) {
			v.name = f[1];
			v.title = f[2];
			v.hue = f[3].to_int(0);
			v.str = f[4].to_int(0);
			v.dex = f[5].to_int(0);
			v.intel = f[6].to_int(0);
			v.gold = f[7].to_int(0);
			v.bank_gold = f[8].to_int(0);
			v.fame = f[9].to_int(0);
			v.karma = f[10].to_int(0);
			v.armor = f[11].to_int(0);
			v.luck = f[12].to_int(0);
			v.pvp = f[13] == "1";
			v.party = f[14].to_int(-1);
			v.buff_night = f[15].to_int(0);
			v.buff_fire = f[16].to_int(0);
			v.buff_regen = f[17].to_int(0);
			v.sheet_version = f[18].to_int(0);
			v.journal = [];
		} else if (k == "K") {
			v.skills = dec_ints(f.size() > 1 ? f[1] : "");
		} else if (k == "P") {
			v.pack = dec_items(f.size() > 1 ? f[1] : "");
		} else if (k == "W") {
			v.worn = dec_items(f.size() > 1 ? f[1] : "");
		} else if (k == "B") {
			v.bank = dec_items(f.size() > 1 ? f[1] : "");
		} else if (k == "L") {
			v.spells = dec_ints(f.size() > 1 ? f[1] : "");
		} else if (k == "J" && f.size() > 1) {
			v.journal.push(f[1]);
		} else if (k == "Q") {
			v.party_names = [];
			v.party_hp = [];
			array<string> ps = f.size() > 1 ? f[1].split(",") : [];
			for (int i = 0; i < ps.size(); ++i) {
				if (ps[i] == "") { continue; }
				array<string> nh = ps[i].split(":");
				v.party_names.push(nh[0]);
				v.party_hp.push(nh.size() > 1 ? nh[1].to_int(0) : 0);
			}
		}
	}
}

// the index of a mobile in the view by serial (-1 = not visible)
int cv_find(ClientView& v, int ser) {
	for (int i = 0; i < v.m_ser.size(); ++i) {
		if (v.m_ser[i] == ser) { return i; }
	}
	return -1;
}

int cv_me(ClientView& v) { return cv_find(v, v.my_ser); }

int cv_find_item(ClientView& v, int ser) {
	for (int i = 0; i < v.i_ser.size(); ++i) {
		if (v.i_ser[i] == ser) { return i; }
	}
	return -1;
}

void cv_remove_mob(ClientView& v, int i) {
	v.m_ser.remove(i);
	v.m_kind.remove(i);
	v.m_x.remove(i);
	v.m_y.remove(i);
	v.m_px.remove(i);
	v.m_py.remove(i);
	v.m_mt.remove(i);
	v.m_md.remove(i);
	v.m_ppx.remove(i);
	v.m_ppy.remove(i);
	v.m_pmt.remove(i);
	v.m_pmd.remove(i);
	v.m_dir.remove(i);
	v.m_hp.remove(i);
	v.m_flags.remove(i);
	v.m_role.remove(i);
	v.m_slot.remove(i);
	v.m_hue.remove(i);
	v.m_name.remove(i);
}

void cv_remove_item(ClientView& v, int i) {
	v.i_ser.remove(i);
	v.i_base.remove(i);
	v.i_rar.remove(i);
	v.i_x.remove(i);
	v.i_y.remove(i);
	v.i_qty.remove(i);
}

// terrain as this client knows it
int cv_tile(ClientView& v, int x, int y) {
	int w = v.world;
	if (x < 0 || y < 0 || x >= W_W[w] || y >= W_H[w]) { return tile_at(w, x, y); }
	int i = y * W_W[w] + x;
	if (v.tover.size() > 0 && v.tover.has(i)) { return v.tover[i]; }
	return TILES[W_OFF[w] + i];
}
