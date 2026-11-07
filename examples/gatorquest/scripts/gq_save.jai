// Gator Quest - saves as plain text files the script owns. The host keeps
// one world file (everything that changes after worldgen; the terrain itself
// regenerates from the seed) and one file per character. Every file is
// "MAGIC version" on the first line, one "key values" record per line, and a
// last line "end <checksum>" over everything above it. Writes go to .tmp,
// are read back and verified, the previous good file becomes .bak, then the
// real file is written; loads fall back to .bak (then .tmp) when a file is
// missing, truncated, corrupt or from a newer version. Unknown keys are
// skipped and missing keys keep their defaults, so older saves load.

import "gq_net.jai";

int SAVE_VERSION = 1;
string WORLD_MAGIC = "GATORQUEST-WORLD";
string CHAR_MAGIC = "GATORQUEST-CHAR";
int AUTOSAVE_TICKS = 3000;

string DATA_ROOT;
string SAVE_DIR;
string SAVE_NOTE;              // what the last load had to do (fallbacks, refusals), for the log
bool SAVES_ON;                 // interactive play autosaves; tests and smoke do not

// where the game keeps its files: beside the scripts when writable, else the per-user data folder
string data_root() {
	if (DATA_ROOT != "") { return DATA_ROOT; }
	string beside = parent_dir(current_script_dir());
	DATA_ROOT = writable_dir(beside + "/saves") ? beside : app_data_dir("GatorQuest");
	return DATA_ROOT;
}

string save_dir() {
	if (SAVE_DIR == "") { SAVE_DIR = data_root() + "/saves"; }
	return SAVE_DIR;
}

string world_path() { return save_dir() + "/world.gqw"; }

string safe_name(string name) {
	string low = string::to_lower(name);
	string out = "";
	for (int i = 0; i < low.size(); ++i) {
		int c = low[i] + 0;
		if (c >= 97 && c <= 122 || c >= 48 && c <= 57) { out += low.substr(i, 1); }
		else if (c == 32 || c == 45 || c == 95) { out += "_"; }
	}
	return out == "" ? "gator" : out;
}

string char_path(string name) { return save_dir() + "/chars/" + safe_name(name) + ".gqc"; }

// ------------------------------------------------------------ sealing ---
int text_checksum(string s) {
	int h = 2166136261;
	int n = s.size();
	for (int i = 0; i < n; ++i) { h = ((h ^ (s[i] + 0)) * 16777619) & 0xFFFFFFFF; }
	return h;
}

string seal(string body) { return body + "end " + to_string(text_checksum(body)) + "\n"; }

// the body of a sealed file, or "" with SAVE_NOTE saying why
string unseal(string text, string magic) {
	if (text == "") { SAVE_NOTE = "missing or empty"; return ""; }
	string t = text;
	t.replace("\r\n", "\n");              // saves are written with \n; a tool that converted them is forgiven
	while (t.size() > 0 && (t.ends_with("\n") || t.ends_with("\r") || t.ends_with(" "))) { t = t.substr(0, t.size() - 1); }
	int at = t.rfind("\n");
	if (at < 0) { SAVE_NOTE = "truncated"; return ""; }
	string last = t.substr(at + 1, t.size() - at - 1);
	if (!last.starts_with("end ")) { SAVE_NOTE = "truncated (no end line)"; return ""; }
	string body = t.substr(0, at + 1);
	int stored = last.substr(4, last.size() - 4).to_int(-1);
	if (stored != text_checksum(body)) { SAVE_NOTE = "corrupt (checksum mismatch)"; return ""; }
	if (!body.starts_with(magic + " ")) { SAVE_NOTE = "not a " + magic + " file"; return ""; }
	int nl = body.find("\n");
	int version = body.substr(magic.size() + 1, nl - magic.size() - 1).to_int(0);
	if (version < 1) { SAVE_NOTE = "unreadable version"; return ""; }
	if (version > SAVE_VERSION) { SAVE_NOTE = `saved by a newer Gator Quest (format ${version}, this build reads up to ${SAVE_VERSION})`; return ""; }
	return body;
}

// the file's text, "" when it is missing or cannot be read (read_file raises for those)
string read_file_or_empty(string path) {
	try { return read_file(path); } catch (e) { return ""; }
}

// write, read back, keep the previous good copy as .bak, then replace
bool safe_write(string path, string text) {
	string tmp = path + ".tmp";
	if (!write_file(tmp, text)) { return false; }
	if (read_file_or_empty(tmp) != text) { return false; }
	if (file_exists(path)) {
		string old = read_file_or_empty(path);
		if (unseal(old, text.substr(0, text.find(" "))) != "") { write_file(path + ".bak", old); }
	}
	if (!write_file(path, text)) { return false; }
	if (read_file_or_empty(path) != text) { return false; }
	delete_file(tmp);
	return true;
}

// the first good body among the file, its backup and a leftover temp; "" if none
string load_sealed(string path, string magic) {
	array<string> tries = [path, path + ".bak", path + ".tmp"];
	string first_problem = "";
	for (int i = 0; i < tries.size(); ++i) {
		if (!file_exists(tries[i])) {
			if (i == 0) { first_problem = "missing"; }
			continue;
		}
		string body = unseal(read_file_or_empty(tries[i]), magic);
		if (body != "") {
			SAVE_NOTE = i == 0 ? "" : `${file_name(path)} was ${first_problem}; restored from ${file_name(tries[i])}`;
			return body;
		}
		if (i == 0) { first_problem = SAVE_NOTE; }
	}
	SAVE_NOTE = first_problem == "" ? "missing" : first_problem;
	return "";
}

// "key values" lookup over a body; "" when absent
map<string, string> parse_records(string body) {
	map<string, string> out;
	array<string> lines = body.lines();
	for (int i = 1; i < lines.size(); ++i) {
		string line = lines[i];
		int sp = line.find(" ");
		if (sp <= 0) { continue; }
		string key = line.substr(0, sp);
		if (key == "m" || key == "g") { continue; }
		out[key] = line.substr(sp + 1, line.size() - sp - 1);
	}
	return out;
}

int rec_int(map<string, string>& r, string key, int def) { return r.has(key) ? r[key].to_int(def) : def; }
string rec_str(map<string, string>& r, string key, string def) { return r.has(key) ? r[key] : def; }
// a list of ints, separated by commas or spaces
array<int> rec_ints(map<string, string>& r, string key) {
	if (!r.has(key)) { return []; }
	string v = r[key];
	v.replace(" ", ",");
	return dec_ints(v);
}

string save_enc_item(Item it) { return enc_item(it) + "." + to_string(it.ser); }

Item save_dec_item(string s) {
	Item it = dec_item(s);
	array<string> f = s.split(".");
	if (f.size() >= 9) { it.ser = f[8].to_int(0); }
	return it;
}

string save_enc_items(array<Item>& items) {
	string out = "";
	for (int i = 0; i < items.size(); ++i) {
		if (i > 0) { out += ","; }
		out += save_enc_item(items[i]);
	}
	return out;
}

array<Item> save_dec_items(string s) {
	array<Item> out;
	if (s == "") { return out; }
	array<string> parts = s.split(",");
	for (int i = 0; i < parts.size(); ++i) {
		Item it = save_dec_item(parts[i]);
		if (it.base >= 0 || parts[i].starts_with("-1.")) { out.push(it); }
	}
	return out;
}

// ---------------------------------------------------------- characters ---
string char_text(int slot) {
	Character& c = CHARS[slot];
	int m = c.mob;
	string out = CHAR_MAGIC + " " + to_string(SAVE_VERSION) + "\n";
	out += "name " + clean_text(c.name) + "\n";
	out += `hue ${c.hue}\n`;
	out += `stats ${c.str} ${c.dex} ${c.intel}\n`;
	out += "skills " + enc_ints(c.skill) + "\n";
	out += `gold ${c.gold}\nbank_gold ${c.bank_gold}\n`;
	out += `record ${c.fame} ${c.karma} ${c.kills} ${c.deaths} ${c.murders}\n`;
	out += "spells " + enc_ints(c.spells) + "\n";
	out += "quest_stage " + enc_ints(c.quest_stage) + "\n";
	out += "quest_count " + enc_ints(c.quest_count) + "\n";
	out += "visited " + enc_ints(c.visited) + "\n";
	out += `boss_mask ${c.boss_mask}\npvp ${c.pvp ? 1 : 0}\nparty ${c.party} ${c.invite}\n`;
	out += `home ${c.home_world} ${c.home_x} ${c.home_y}\n`;
	if (mob_live(m)) {
		out += `pos ${MOB_WORLD[m]} ${MOB_X[m]} ${MOB_Y[m]}\n`;
		out += `vitals ${MOB_HP[m]} ${MOB_MP[m]} ${mob_ghost(m) ? 1 : 0} ${MOB_POISON[m]} ${MOB_BURN[m]} ${MOB_SLOW[m]}\n`;
	}
	out += `buffs ${c.buff_night - TICK} ${c.buff_fire - TICK} ${c.buff_str - TICK} ${c.buff_regen - TICK}\n`;
	out += `action ${c.action} ${c.action_t - TICK} ${c.action_x} ${c.action_y} ${c.action_arg}\n`;
	out += `talk ${c.talk_npc} ${c.talk_t - TICK}\n`;
	out += `misc ${c.last_spell} ${c.fish_caught} ${c.crafted} ${c.played_ticks} ${c.version} ${c.stat_t}\n`;
	out += "pack " + save_enc_items(c.pack) + "\n";
	out += "worn " + save_enc_items(c.worn) + "\n";
	out += "bank " + save_enc_items(c.bank) + "\n";
	return seal(out);
}

// a Character from a sealed body; every field it lacks keeps a new character's default
Character char_from_body(string body) {
	map<string, string> r = parse_records(body);
	Character c = Character();
	c.active = true;
	c.connected = true;
	c.name = rec_str(r, "name", "Gator");
	c.hue = rec_int(r, "hue", 0);
	array<int> st = rec_ints(r, "stats");
	if (st.size() >= 3) { c.str = st[0]; c.dex = st[1]; c.intel = st[2]; }
	c.skill = rec_ints(r, "skills");
	while (c.skill.size() < Sk.count) { c.skill.push(0); }
	c.gold = rec_int(r, "gold", 0);
	c.bank_gold = rec_int(r, "bank_gold", 0);
	array<int> rec = rec_ints(r, "record");
	if (rec.size() >= 5) { c.fame = rec[0]; c.karma = rec[1]; c.kills = rec[2]; c.deaths = rec[3]; c.murders = rec[4]; }
	c.spells = rec_ints(r, "spells");
	while (c.spells.size() < SPELLS.size()) { c.spells.push(0); }
	c.quest_stage = rec_ints(r, "quest_stage");
	c.quest_count = rec_ints(r, "quest_count");
	c.visited = rec_ints(r, "visited");
	while (c.visited.size() < WORLD_COUNT) { c.visited.push(0); }
	c.boss_mask = rec_int(r, "boss_mask", 0);
	c.pvp = rec_int(r, "pvp", 0) != 0;
	array<int> party = rec_ints(r, "party");
	if (party.size() >= 2) { c.party = party[0]; c.invite = party[1]; }
	array<int> home = rec_ints(r, "home");
	if (home.size() >= 3 && home[0] >= 0 && home[0] < WORLD_COUNT) { c.home_world = home[0]; c.home_x = home[1]; c.home_y = home[2]; }
	else { c.home_world = 0; c.home_x = W_ARRIVE_X[0]; c.home_y = W_ARRIVE_Y[0]; }
	array<int> buffs = rec_ints(r, "buffs");
	if (buffs.size() >= 4) {
		c.buff_night = TICK + buffs[0];
		c.buff_fire = TICK + buffs[1];
		c.buff_str = TICK + buffs[2];
		c.buff_regen = TICK + buffs[3];
	}
	array<int> act = rec_ints(r, "action");
	if (act.size() >= 5) { c.action = act[0]; c.action_t = TICK + act[1]; c.action_x = act[2]; c.action_y = act[3]; c.action_arg = act[4]; }
	array<int> talk = rec_ints(r, "talk");
	if (talk.size() >= 2) { c.talk_npc = talk[0]; c.talk_t = TICK + talk[1]; }
	array<int> misc = rec_ints(r, "misc");
	if (misc.size() >= 6) { c.last_spell = misc[0]; c.fish_caught = misc[1]; c.crafted = misc[2]; c.played_ticks = misc[3]; c.version = misc[4]; c.stat_t = misc[5]; }
	c.pack = save_dec_items(rec_str(r, "pack", ""));
	c.worn = save_dec_items(rec_str(r, "worn", ""));
	while (c.worn.size() < Slot.count) { c.worn.push(Item()); }
	c.bank = save_dec_items(rec_str(r, "bank", ""));
	return c;
}

void ensure_slot(int slot) {
	while (CHARS.size() <= slot) { CHARS.push(Character()); }
	while (IN_MOVE.size() <= slot) { IN_MOVE.push(-1); }
	while (IN_QUEUE.size() <= slot) { IN_QUEUE.push([]); }
	while (NET_SHEET_SENT.size() <= slot) { NET_SHEET_SENT.push(-1); }
}

// a returning character enters the live world at its saved spot and condition
bool player_restore(int slot, string body) {
	ensure_slot(slot);
	Character c = char_from_body(body);
	c.party = -1;              // parties and invitations belong to the session that made them
	c.invite = -1;
	CHARS[slot] = c;
	quest_ensure(slot);
	map<string, string> r = parse_records(body);
	array<int> pos = rec_ints(r, "pos");
	Character& cc = CHARS[slot];
	int w = cc.home_world;
	int x = cc.home_x;
	int y = cc.home_y;
	if (pos.size() >= 3 && pos[0] >= 0 && pos[0] < WORLD_COUNT && in_world(pos[0], pos[1], pos[2])) { w = pos[0]; x = pos[1]; y = pos[2]; }
	array<int> p = find_free_spot(w, x, y, 8, LAND_MASK + 4);
	int m = mob_spawn(M_GATOR_NPC, w, p[0], p[1]);
	MOB_PLAYER[m] = slot;
	MOB_FLAGS[m] = cc.pvp ? MF_PVP : 0;
	cc.mob = m;
	player_refresh(slot);
	array<int> vit = rec_ints(r, "vitals");
	MOB_HP[m] = MOB_HPMAX[m];
	MOB_MP[m] = MOB_MPMAX[m];
	if (vit.size() >= 6) {
		MOB_HP[m] = iclamp(vit[0], 0, MOB_HPMAX[m]);
		MOB_MP[m] = iclamp(vit[1], 0, MOB_MPMAX[m]);
		if (vit[2] != 0) { MOB_FLAGS[m] = MOB_FLAGS[m] | MF_GHOST; MOB_HP[m] = 0; }
		MOB_POISON[m] = vit[3];
	}
	if (MOB_HP[m] <= 0 && !mob_ghost(m)) { MOB_HP[m] = 1; }
	cc.version = cc.version + 1;
	return true;
}

// ---------------------------------------------------------------- world ---
string ints_line(string key, array<int>& v) { return key + " " + enc_ints(v) + "\n"; }

string world_text() {
	string out = WORLD_MAGIC + " " + to_string(SAVE_VERSION) + "\n";
	out += `seed ${SEED}\ntick ${TICK}\nrng ${RNG_S}\nser ${NEXT_SER}\n`;
	string regrow = "";
	string occ = "";
	int n = TILES.size();
	for (int i = 0; i < n; ++i) {
		if (REGROW[i] > TICK) { regrow += (regrow == "" ? "" : ",") + `${i}:${REGROW[i]}`; }
		if (OCC[i] != 0) { occ += (occ == "" ? "" : ",") + `${i}:${OCC[i]}`; }
	}
	out += "regrow " + regrow + "\n";
	out += "occ " + occ + "\n";
	out += ints_line("chest_t", CHEST_T);
	out += ints_line("weather", W_WEATHER);
	out += ints_line("weather_t", W_WEATHER_T);
	out += ints_line("boss_t", W_BOSS_T);
	out += ints_line("sp_alive", SP_ALIVE);
	out += ints_line("sp_next", SP_NEXT);
	out += `mobs ${MOB_KIND.size()}\n`;
	for (int m = 0; m < MOB_KIND.size(); ++m) {
		if (MOB_KIND[m] < 0) { out += "m -\n"; continue; }
		out += `m ${MOB_KIND[m]} ${MOB_SER[m]} ${MOB_WORLD[m]} ${MOB_X[m]} ${MOB_Y[m]} ${MOB_PX[m]} ${MOB_PY[m]} ${MOB_MOVE_T[m]} ${MOB_MOVE_D[m]} ${MOB_DIR[m]}`;
		out += ` ${MOB_HP[m]} ${MOB_HPMAX[m]} ${MOB_MP[m]} ${MOB_MPMAX[m]} ${MOB_TARGET[m]} ${MOB_AI[m]} ${MOB_NEXT[m]} ${MOB_ATK[m]} ${MOB_CAST[m]} ${MOB_FLAGS[m]}`;
		out += ` ${MOB_PLAYER[m]} ${MOB_SPAWNER[m]} ${MOB_HOMEX[m]} ${MOB_HOMEY[m]} ${MOB_POISON[m]} ${MOB_BURN[m]} ${MOB_SLOW[m]} ${MOB_BUFF[m]} ${MOB_HIT_T[m]}`;
		out += ` ${MOB_OWNER[m]} ${MOB_AUX[m]} ${MOB_ROLE[m]} ${MOB_LEVEL[m]}\n`;
	}
	out += `items ${GI_ITEM.size()}\n`;
	for (int i = 0; i < GI_ITEM.size(); ++i) {
		if (GI_ITEM[i].base < 0) { out += "g -\n"; continue; }
		out += `g ${save_enc_item(GI_ITEM[i])} ${GI_W[i]} ${GI_X[i]} ${GI_Y[i]} ${GI_SER[i]} ${GI_EXPIRE[i]} ${GI_OWNER[i]}\n`;
	}
	string players = "";
	for (int s = 0; s < CHARS.size(); ++s) {
		if (CHARS[s].active) { players += (players == "" ? "" : ",") + `${s}:${safe_name(CHARS[s].name)}`; }
	}
	out += "players " + players + "\n";
	out += "last " + (CHARS.size() > 0 && CHARS[0].active ? clean_text(CHARS[0].name) : "") + "\n";
	return seal(out);
}

// rebuild the world from a sealed body. keep_players = a full restore (the same session
// continues: player mobiles stay in their slots); otherwise their mobiles are removed and
// characters rejoin through player_restore.
bool world_from_body(string body, bool keep_players) {
	map<string, string> r = parse_records(body);
	int seed = rec_int(r, "seed", 1);
	sim_reset();
	world_generate(seed);
	TICK = rec_int(r, "tick", 0);
	RNG_S = rec_int(r, "rng", 1);
	NEXT_SER = rec_int(r, "ser", 1);
	array<string> reg = rec_str(r, "regrow", "").split(",");
	for (int k = 0; k < reg.size(); ++k) {
		array<string> iv = reg[k].split(":");
		int i = iv[0].to_int(-1);
		if (iv.size() == 2 && i >= 0 && i < REGROW.size()) { REGROW[i] = iv[1].to_int(0); }
	}
	array<string> occ = rec_str(r, "occ", "").split(",");
	array<int> chest = rec_ints(r, "chest_t");
	for (int k = 0; k < chest.size() && k < CHEST_T.size(); ++k) { CHEST_T[k] = chest[k]; }
	array<int> wx = rec_ints(r, "weather");
	array<int> wxt = rec_ints(r, "weather_t");
	array<int> boss = rec_ints(r, "boss_t");
	for (int k = 0; k < WORLD_COUNT; ++k) {
		if (k < wx.size()) { W_WEATHER[k] = wx[k]; }
		if (k < wxt.size()) { W_WEATHER_T[k] = wxt[k]; }
		if (k < boss.size()) { W_BOSS_T[k] = boss[k]; }
	}
	array<int> spa = rec_ints(r, "sp_alive");
	array<int> spn = rec_ints(r, "sp_next");
	for (int k = 0; k < SP_ALIVE.size(); ++k) {
		if (k < spa.size()) { SP_ALIVE[k] = spa[k]; }
		if (k < spn.size()) { SP_NEXT[k] = spn[k]; }
	}
	// the pools, slot for slot (free slots included: allocation reuses the first free one)
	array<string> lines = body.lines();
	for (int li = 0; li < lines.size(); ++li) {
		string line = lines[li];
		if (line.starts_with("m ")) {
			int m = mob_push();
			if (line == "m -") { continue; }
			array<int> f = dec_ints(string::replace(line.substr(2, line.size() - 2), " ", ","));
			if (f.size() < 33 || f[0] < 0 || f[0] >= MONS.size() || f[2] < 0 || f[2] >= WORLD_COUNT) { continue; }
			MOB_KIND[m] = f[0];
			MOB_SER[m] = f[1];
			MOB_WORLD[m] = f[2];
			MOB_X[m] = f[3];
			MOB_Y[m] = f[4];
			MOB_PX[m] = f[5];
			MOB_PY[m] = f[6];
			MOB_MOVE_T[m] = f[7];
			MOB_MOVE_D[m] = f[8];
			MOB_DIR[m] = f[9];
			MOB_HP[m] = f[10];
			MOB_HPMAX[m] = f[11];
			MOB_MP[m] = f[12];
			MOB_MPMAX[m] = f[13];
			MOB_TARGET[m] = f[14];
			MOB_AI[m] = f[15];
			MOB_NEXT[m] = f[16];
			MOB_ATK[m] = f[17];
			MOB_CAST[m] = f[18];
			MOB_FLAGS[m] = f[19];
			MOB_PLAYER[m] = f[20];
			MOB_SPAWNER[m] = f[21];
			MOB_HOMEX[m] = f[22];
			MOB_HOMEY[m] = f[23];
			MOB_POISON[m] = f[24];
			MOB_BURN[m] = f[25];
			MOB_SLOW[m] = f[26];
			MOB_BUFF[m] = f[27];
			MOB_HIT_T[m] = f[28];
			MOB_OWNER[m] = f[29];
			MOB_AUX[m] = f[30];
			MOB_ROLE[m] = f[31];
			MOB_LEVEL[m] = f[32];
			MOB_LIVE = MOB_LIVE + 1;
		} else if (line.starts_with("g ")) {
			int i = GI_ITEM.size();
			GI_ITEM.push(Item());
			GI_W.push(0);
			GI_X.push(0);
			GI_Y.push(0);
			GI_SER.push(0);
			GI_EXPIRE.push(0);
			GI_OWNER.push(-1);
			if (line == "g -") { continue; }
			array<string> f = line.split(" ");
			if (f.size() < 8) { continue; }
			GI_ITEM[i] = save_dec_item(f[1]);
			GI_W[i] = iclamp(f[2].to_int(0), 0, WORLD_COUNT - 1);
			GI_X[i] = f[3].to_int(0);
			GI_Y[i] = f[4].to_int(0);
			GI_SER[i] = f[5].to_int(0);
			GI_EXPIRE[i] = f[6].to_int(0);
			GI_OWNER[i] = f[7].to_int(-1);
		}
	}
	// occupancy exactly as saved (two mobiles can share a tile with a ghost; the order matters)
	for (int k = 0; k < occ.size(); ++k) {
		array<string> iv = occ[k].split(":");
		int i = iv[0].to_int(-1);
		int v = iv.size() == 2 ? iv[1].to_int(0) : 0;
		if (i >= 0 && i < OCC.size() && v > 0 && v <= MOB_KIND.size()) { OCC[i] = v; }
	}
	if (!keep_players) {
		for (int m = 0; m < MOB_KIND.size(); ++m) {
			if (MOB_KIND[m] >= 0 && MOB_PLAYER[m] >= 0) {
				MOB_PLAYER[m] = -1;
				mob_free(m);
			}
		}
	}
	WORLD_READY = true;
	return true;
}

// the names in a world file's player list, by slot ("slot:name,...")
map<int, string> world_players(string body) {
	map<int, string> out;
	map<string, string> r = parse_records(body);
	array<string> ps = rec_str(r, "players", "").split(",");
	for (int k = 0; k < ps.size(); ++k) {
		array<string> sn = ps[k].split(":");
		if (sn.size() == 2) { out[sn[0].to_int(-1)] = sn[1]; }
	}
	return out;
}

// ---------------------------------------------------------- front door ---
string save_world_and_chars() {
	if (!WORLD_READY) { return "Nothing to save yet."; }
	make_dirs(save_dir() + "/chars");
	int chars = 0;
	for (int s = 0; s < CHARS.size(); ++s) {
		if (!CHARS[s].active) { continue; }
		if (!safe_write(char_path(CHARS[s].name), char_text(s))) { return "Save FAILED writing " + char_path(CHARS[s].name); }
		chars = chars + 1;
	}
	if (!safe_write(world_path(), world_text())) { return "Save FAILED writing " + world_path(); }
	return `Saved the world and ${chars} ${plural(chars, "gator", "gators")} (day ${day_number()}).`;
}

// full restore of the saved session (world + every saved player in its slot)
string load_world_and_chars() {
	string body = load_sealed(world_path(), WORLD_MAGIC);
	if (body == "") { return "No saved world could be loaded: " + SAVE_NOTE + "."; }
	string note = SAVE_NOTE;
	map<int, string> players = world_players(body);
	world_from_body(body, true);
	int loaded = 0;
	array<int> slots = players.keys();
	for (int k = 0; k < slots.size(); ++k) {
		int slot = slots[k];
		string cbody = load_sealed(save_dir() + "/chars/" + players[slot] + ".gqc", CHAR_MAGIC);
		if (cbody == "" || slot < 0 || slot >= 16) { continue; }
		ensure_slot(slot);
		CHARS[slot] = char_from_body(cbody);
		quest_ensure(slot);
		for (int m = 0; m < MOB_KIND.size(); ++m) {
			if (MOB_KIND[m] >= 0 && MOB_PLAYER[m] == slot) { CHARS[slot].mob = m; }
		}
		if (!mob_live(CHARS[slot].mob)) { CHARS[slot].active = false; continue; }
		loaded = loaded + 1;
	}
	// a player whose character file is gone leaves its gator behind: remove it
	for (int m = 0; m < MOB_KIND.size(); ++m) {
		if (MOB_KIND[m] >= 0 && MOB_PLAYER[m] >= 0) {
			int s = MOB_PLAYER[m];
			if (s >= CHARS.size() || !CHARS[s].active || CHARS[s].mob != m) {
				MOB_PLAYER[m] = -1;
				mob_free(m);
			}
		}
	}
	string extra = note == "" ? "" : " (" + note + ")";
	return `Loaded day ${day_number()} with ${loaded} ${plural(loaded, "gator", "gators")}${extra}.`;
}

// start of an interactive session: the saved world without its players; true if one loaded
bool load_world_for_session() {
	string body = load_sealed(world_path(), WORLD_MAGIC);
	if (body == "") { return false; }
	world_from_body(body, false);
	return true;
}

string last_player_name() {
	string body = load_sealed(world_path(), WORLD_MAGIC);
	if (body == "") { return ""; }
	map<string, string> r = parse_records(body);
	return rec_str(r, "last", "");
}

// a character joining: restored from its file when it has one, else a new gator
string join_character(int slot, string name, int hue) {
	string body = load_sealed(char_path(name), CHAR_MAGIC);
	if (body != "") {
		player_restore(slot, body);
		string extra = SAVE_NOTE == "" ? "" : " (" + SAVE_NOTE + ")";
		return `Welcome back, ${CHARS[slot].name}!${extra}`;
	}
	player_create(slot, name, hue);
	return `Welcome to Swampton, ${name}! Talk to the townsfolk (bump them) for work. Press ? for help.`;
}

void autosave_tick() {
	if (!SAVES_ON || TICK % AUTOSAVE_TICKS != 0 || TICK == 0) { return; }
	string msg = save_world_and_chars();
	for (int s = 0; s < CHARS.size(); ++s) {
		if (CHARS[s].active) { say_to(s, "Autosave: " + msg, C_GRAY); }
	}
}
