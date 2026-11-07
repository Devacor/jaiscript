// Gator Quest - the multi-view HUD: the world view on the left, a side pane of
// vitals, target, a tabbed panel (paperdoll, backpack, skills, journal,
// spellbook, crafting, npc dialogs), a half-block minimap and the journal log.

import "gq_audio.jai";

// ---- minimap
array<int> MINI;
int MINI_W;
int MINI_H;
array<int> MINI_COL;          // map color per tile type

void minimap_init() {
	MINI_COL = [];
	for (int t = 0; t < TILES_DEF.size(); ++t) {
		int off = TEX_OFF[(t * 4) * 4];
		int r = 0;
		int g = 0;
		int b = 0;
		int n = TS * TS;
		for (int k = 0; k < n; ++k) {
			int c = TEX[off + k];
			r = r + PAL_R[c];
			g = g + PAL_G[c];
			b = b + PAL_B[c];
		}
		MINI_COL.push(rgb(r / n, g / n, b / n));
	}
}

void minimap_draw(ClientView& v, int mw, int mh) {
	MINI_W = mw;
	MINI_H = mh;
	while (MINI.size() < mw * mh) { MINI.push(0); }
	int me = cv_me(v);
	int cx = me >= 0 ? v.m_x[me] : W_ARRIVE_X[0];
	int cy = me >= 0 ? v.m_y[me] : W_ARRIVE_Y[0];
	int scale = 2;
	int x0 = cx - mw * scale / 2;
	int y0 = cy - mh * scale / 2;
	for (int py = 0; py < mh; ++py) {
		for (int px = 0; px < mw; ++px) {
			int t = cv_tile(v, x0 + px * scale, y0 + py * scale);
			MINI[py * mw + px] = MINI_COL[t];
		}
	}
	for (int i = 0; i < v.m_ser.size(); ++i) {
		int px = (v.m_x[i] - x0) / scale;
		int py = (v.m_y[i] - y0) / scale;
		if (px < 0 || py < 0 || px >= mw || py >= mh) { continue; }
		int fac = MONS[v.m_kind[i]].fac;
		int col = rgb(230, 60, 50);
		if (v.m_slot[i] >= 0) { col = rgb(90, 240, 90); }
		else if (fac == Fac.npc || fac == Fac.guard) { col = rgb(250, 220, 90); }
		else if (fac == Fac.animal) { col = rgb(200, 170, 120); }
		if ((v.m_flags[i] & MF_BOSS) != 0) { col = FRAME % 20 < 10 ? rgb(255, 40, 200) : rgb(255, 255, 255); }
		MINI[py * mw + px] = col;
	}
	int mx = (cx - x0) / scale;
	int my = (cy - y0) / scale;
	if (FRAME % 16 < 12 && mx >= 0 && my >= 0 && mx < mw && my < mh) { MINI[my * mw + mx] = rgb(255, 255, 255); }
}

string mini_row(int cy) {
	string row = "";
	int fg = -1;
	int bg = -1;
	int ti = cy * 2 * MINI_W;
	int bi = ti + MINI_W;
	for (int x = 0; x < MINI_W; ++x) {
		int top = MINI[ti + x];
		int bot = MINI[bi + x];
		if (top != fg) { row += PAL_FG[top]; fg = top; }
		if (bot != bg) { row += PAL_BG[bot]; bg = bot; }
		row += HALF;
	}
	return row + RESET;
}

// ---- panel helpers
string SGR_FRAME = "38;2;90;140;70";
string SGR_TITLE = "1;38;2;150;230;110";
string SGR_TEXT = "38;2;220;220;210";
string SGR_DIM = "38;2;130;130;125";
string SGR_GOLD = "38;2;250;210;70";
string SGR_SEL = "1;38;2;20;20;20;48;2;150;220;110";

array<string> LOG_SGR = ["38;2;230;230;230", "38;2;140;140;140", "38;2;240;90;80", "38;2;110;230;110", "38;2;250;220;90",
	"38;2;110;220;240", "38;2;200;130;250", "38;2;250;160;70", "38;2;110;150;250"];

string rule(string label, int width) {
	pane_begin(width);
	pane_add(SGR_FRAME, "\xe2\x94\x80\xe2\x94\x80 ");
	pane_add(SGR_TITLE, label);
	pane_add(SGR_FRAME, " " + string::repeat("\xe2\x94\x80", imax(0, width - label.size() - 4)));
	return pane_end();
}

// word-wrap plain text to a width
array<string> wrap(string text, int width) {
	array<string> out;
	array<string> words = text.split(" ");
	string line = "";
	for (int i = 0; i < words.size(); ++i) {
		string w = words[i];
		if (line == "") { line = w; }
		else if (line.size() + 1 + w.size() <= width) { line = line + " " + w; }
		else { out.push(line); line = "  " + w; }
		while (line.size() > width) {
			out.push(line.substr(0, width));
			line = "  " + line.substr(width, line.size() - width);
		}
	}
	if (line != "") { out.push(line); }
	return out;
}

string item_label(Item it) { return item_name(it); }

// the list a menu mode shows: labels, colors and how many entries
array<string> MENU_LABELS;
array<string> MENU_SGR;
array<int> MENU_ARG;

void menu_clear() {
	MENU_LABELS = [];
	MENU_SGR = [];
	MENU_ARG = [];
}

void menu_add(string label, string sgr, int arg) {
	MENU_LABELS.push(label);
	MENU_SGR.push(sgr);
	MENU_ARG.push(arg);
}

// known spells in book order: index i of this list is quick-cast key i+1
array<int> known_spells(ClientView& v) {
	array<int> out;
	for (int s = 0; s < v.spells.size(); ++s) {
		if (v.spells[s] != 0) { out.push(s); }
	}
	return out;
}

int station_near(ClientView& v, int station) {
	int me = cv_me(v);
	if (me < 0) { return 0; }
	for (int dy = -2; dy <= 2; ++dy) {
		for (int dx = -2; dx <= 2; ++dx) {
			if (TILES_DEF[cv_tile(v, v.m_x[me] + dx, v.m_y[me] + dy)].station == station) { return 1; }
		}
	}
	return 0;
}

int cv_pack_count(ClientView& v, int base) {
	int n = 0;
	for (int i = 0; i < v.pack.size(); ++i) {
		if (v.pack[i].base == base) { n = n + v.pack[i].qty; }
	}
	return n;
}

void build_menu(ClientView& v) {
	menu_clear();
	string mode = UI_MODE;
	if (mode == "pack" || mode == "shop_sell" || mode == "bank_put") {
		for (int i = 0; i < v.pack.size(); ++i) {
			Item it = v.pack[i];
			string extra = "";
			if (mode == "shop_sell") { extra = `  ${sell_price(it)}g`; }
			else if (item_equippable(it)) { extra = ""; }
			menu_add(item_label(it) + extra, rarity_sgr(it.rarity), i);
		}
		if (v.pack.size() == 0) { menu_add("(empty)", SGR_DIM, -1); }
	} else if (mode == "worn") {
		for (int s = 0; s < v.worn.size(); ++s) {
			Item it = v.worn[s];
			string label = SLOT_NAME[s] + ": " + (it.base >= 0 ? item_label(it) : "-");
			menu_add(label, it.base >= 0 ? rarity_sgr(it.rarity) : SGR_DIM, s);
		}
	} else if (mode == "skills") {
		for (int k = 0; k < v.skills.size(); ++k) {
			int s = v.skills[k];
			string rank = s >= 300 ? " " + RANK_NAME[imin(7, s / 100 - 3)] : "";
			menu_add(`${SKILL_NAME[k]:<17}${tenths(s):>5}${rank}`, s >= 700 ? SGR_GOLD : (s > 0 ? SGR_TEXT : SGR_DIM), k);
		}
	} else if (mode == "journal") {
		for (int i = 0; i < v.journal.size(); ++i) { menu_add(v.journal[i], v.journal[i].starts_with("(done)") ? SGR_DIM : (v.journal[i].starts_with("[DONE]") ? SGR_GOLD : SGR_TEXT), i); }
		if (v.journal.size() == 0) { menu_add("No quests yet. Talk to folk in town.", SGR_DIM, -1); }
	} else if (mode == "spells") {
		array<int> known = known_spells(v);
		for (int i = 0; i < known.size(); ++i) {
			SpellDef s = SPELLS[known[i]];
			bool have = true;
			for (int r = 0; r < s.reg.size(); ++r) {
				if (cv_pack_count(v, s.reg[r]) < 1) { have = false; }
			}
			string key = i < 9 ? to_string(i + 1) : " ";
			menu_add(`${key} ${s.name} (${s.mana}mp)`, have && v.mp >= s.mana ? "38;2;200;150;255" : SGR_DIM, known[i]);
		}
	} else if (mode == "craft") {
		for (int r = 0; r < RECIPES.size(); ++r) {
			RecipeDef d = RECIPES[r];
			if (d.station != 0 && station_near(v, d.station) == 0) { continue; }
			if (d.station == 0 && d.tool >= 0 && cv_pack_count(v, d.tool) == 0) { continue; }
			bool have = v.skills.size() > d.skill && v.skills[d.skill] >= d.min;
			for (int k = 0; k < d.in_item.size(); ++k) {
				if (cv_pack_count(v, d.in_item[k]) < d.in_qty[k]) { have = false; }
			}
			string need = "";
			for (int k = 0; k < d.in_item.size(); ++k) { need = need + (k > 0 ? "," : "") + `${d.in_qty[k]} ${ITEMS[d.in_item[k]].name}`; }
			menu_add(`${ITEMS[d.out].name} <- ${need}`, have ? SGR_TEXT : SGR_DIM, r);
		}
		if (MENU_LABELS.size() == 0) { menu_add("Nothing to craft here. Find a forge, anvil,", SGR_DIM, -1); menu_add("fire pit, or carry a mortar and pestle.", SGR_DIM, -1); }
	} else if (mode == "dialog") {
		int role = v.dlg_role;
		if (role >= 0) {
			NpcRole r = ROLES[role];
			if (r.shop >= 0) { menu_add("Buy", SGR_TEXT, 1); menu_add("Sell", SGR_TEXT, 2); }
			if (r.heals) { menu_add(v.ghost != 0 ? "Resurrect me" : "Heal me (15g)", SGR_TEXT, 3); }
			if (r.banks) { menu_add("Open bank box", SGR_TEXT, 4); }
			menu_add("Quests and talk", SGR_TEXT, 5);
			menu_add("Goodbye", SGR_DIM, 0);
		}
	} else if (mode == "shop_buy") {
		int role = v.dlg_role;
		if (role >= 0 && ROLES[role].shop >= 0) {
			array<int> stock = SHOPS[ROLES[role].shop];
			for (int i = 0; i < stock.size(); ++i) {
				int price = buy_price(stock[i]);
				menu_add(`${ITEMS[stock[i]].name:<22}${price:>5}g`, v.gold >= price ? SGR_TEXT : SGR_DIM, i);
			}
		}
	} else if (mode == "bank") {
		menu_add(`[deposit all gold: ${v.gold}]`, SGR_GOLD, -1);
		menu_add(`[withdraw all gold: ${v.bank_gold}]`, SGR_GOLD, -2);
		menu_add("[put an item in the box...]", SGR_TEXT, -3);
		for (int i = 0; i < v.bank.size(); ++i) { menu_add(item_label(v.bank[i]), rarity_sgr(v.bank[i].rarity), i); }
	} else if (mode == "menu") {
		menu_add("Resume", SGR_TEXT, 0);
		if (!NET_CLIENT) {
			menu_add("Save game", SGR_TEXT, 1);
			menu_add("Load game", SGR_TEXT, 2);
		}
		menu_add(MUSIC_ON ? "Music: on" : "Music: off", SGR_TEXT, 3);
		menu_add(SFX_ON ? "Sound: on" : "Sound: off", SGR_TEXT, 4);
		menu_add(`Zoom: ${TILE_ZOOM == 0 ? "auto" : to_string(TILE_ZOOM) + "px"}`, SGR_TEXT, 5);
		menu_add("Help", SGR_TEXT, 6);
		if (net_hosting()) { menu_add(`Stop hosting (${net_player_count()} joined)`, SGR_TEXT, 9); }
		else if (!NET_CLIENT) { menu_add(`Host this game (port ${TM_PORT})`, SGR_TEXT, 8); }
		menu_add(NET_CLIENT ? "Leave to title" : "Save and leave to title", SGR_TEXT, 10);
		menu_add(NET_CLIENT ? "Leave and quit" : "Save and quit", SGR_TEXT, 7);
	} else if (mode == "help") {
		array<string> help = [
			"arrows/WASD  move (bump to attack/talk)", "space or E   use: talk, fish, mine, chop", "F or tab     next target", "G  pick up    H  bandage    Q  potion",
			"1-9  cast spell    R  recast last", "I pack  P paperdoll  K skills", "J journal  B spellbook  C craft", "M world map  X hide  Z zoom",
			"enter  chat (/pvp /who /invite name", "   /accept /leave /p msg /home)", "esc  menu: save, load, music, quit",
			"In lists: up/down, enter, esc.", "Pack: enter use/equip, D drop.", "Die? walk your ghost to a healer."];
		for (int i = 0; i < help.size(); ++i) { menu_add(help[i], SGR_TEXT, i); }
	}
}

string mode_title(ClientView& v) {
	string m = UI_MODE;
	if (m == "pack") { return `BACKPACK ${v.pack.size()}/${PACK_MAX}`; }
	if (m == "worn") { return "PAPERDOLL"; }
	if (m == "skills") { return "SKILLS"; }
	if (m == "journal") { return "JOURNAL"; }
	if (m == "spells") { return "SPELLBOOK"; }
	if (m == "craft") { return "CRAFTING"; }
	if (m == "dialog" || m == "shop_buy" || m == "shop_sell" || m == "bank" || m == "bank_put") {
		string who = v.dlg_role >= 0 ? ROLES[v.dlg_role].name + " " + ROLES[v.dlg_role].title : "";
		if (m == "shop_buy") { return "BUY - " + who; }
		if (m == "shop_sell") { return "SELL - " + who; }
		if (m == "bank" || m == "bank_put") { return "BANK BOX"; }
		return who;
	}
	if (m == "menu") { return "MENU"; }
	if (m == "help") { return "HELP"; }
	return "PAPERDOLL";
}

// the whole side pane, exactly `rows` strings of visible width `w`
array<string> build_pane(ClientView& v, int w, int rows) {
	array<string> out;
	pane_begin(w);
	pane_add("1;38;2;40;30;10;48;2;120;190;70", " \xe2\x98\xbc GATOR QUEST ");
	pane_add("38;2;120;190;70", " " + string::repeat("\xe2\x96\x93", imax(0, w - 16)));
	out.push(pane_end());
	string title = v.name != "" ? v.name + " " + v.title : "connecting...";
	out.push(pane_line(SGR_TITLE, title, w));
	pane_begin(w);
	pane_add(SGR_TEXT, "HP ");
	int bw = w - 14;
	PANE_ROW += bar(v.hp, v.hpmax, bw, v.ghost != 0 ? "38;2;120;140;170" : "38;2;230;60;50", "38;2;80;30;30");
	PANE_W = PANE_W + bw;
	pane_add(SGR_TEXT, ` ${v.hp:>4}/${v.hpmax:<4}`);
	out.push(pane_end());
	pane_begin(w);
	pane_add(SGR_TEXT, "MP ");
	PANE_ROW += bar(v.mp, v.mpmax, bw, "38;2;80;130;250", "38;2;30;40;80");
	PANE_W = PANE_W + bw;
	pane_add(SGR_TEXT, ` ${v.mp:>4}/${v.mpmax:<4}`);
	out.push(pane_end());
	pane_begin(w);
	pane_add(SGR_GOLD, `Gold ${v.gold}`);
	pane_add(SGR_DIM, `  Fame ${v.fame}  S${v.str} D${v.dex} I${v.intel}`);
	out.push(pane_end());
	int me = cv_me(v);
	string where = WORLDS[v.world].name;
	if (me >= 0) {
		int town = town_at(v.world, v.m_x[me], v.m_y[me]);
		if (town >= 0) { where = TOWN_NAME[town]; }
		where = where + ` (${v.m_x[me]},${v.m_y[me]})`;
	}
	out.push(pane_line("38;2;170;210;140", where, w));
	array<string> wx = ["clear", "rain", "snow", "storm", "fog", "ashfall"];
	string buffs = "";
	if (v.buff_night > 0) { buffs = buffs + " eyeshine"; }
	if (v.buff_fire > 0) { buffs = buffs + " fireproof"; }
	if (v.buff_regen > 0) { buffs = buffs + " regen"; }
	if (v.pvp) { buffs = buffs + " PVP"; }
	out.push(pane_line(SGR_DIM, `Day ${v.day} ${time_label(v)}, ${wx[iclamp(v.weather, 0, 5)]}${buffs}`, w));
	// target
	int ti = cv_find(v, v.target_ser);
	if (ti >= 0) {
		string tn = v.m_slot[ti] >= 0 ? v.m_name[ti] : MONS[v.m_kind[ti]].name;
		if ((v.m_flags[ti] & MF_ELITE) != 0) { tn = "Elite " + tn; }
		pane_begin(w);
		pane_add("38;2;250;120;90", "\xe2\x96\xba " + tn + " ");
		int left = w - PANE_W - 1;
		if (left > 3) {
			PANE_ROW += bar(v.m_hp[ti], 100, left, "38;2;230;80;60", "38;2;70;30;30");
			PANE_W = PANE_W + left;
		}
		out.push(pane_end());
	} else if (v.action != 0) {
		array<string> acts = ["", "Fishing", "Mining", "Chopping", "Bandaging", "Crafting"];
		pane_begin(w);
		pane_add("38;2;120;220;240", acts[iclamp(v.action, 0, 5)] + "... ");
		out.push(pane_end());
	} else {
		out.push(pane_line(SGR_DIM, "no target  (F to pick one)", w));
	}
	// the tabbed panel gets what the minimap and log leave
	int mini_rows = rows >= 40 ? 8 : (rows >= 30 ? 6 : 4);
	int log_rows = imax(4, rows / 5);
	int panel_rows = rows - out.size() - 1 - mini_rows - 1 - log_rows - 1;
	if (panel_rows < 3) { panel_rows = 3; }
	string mode = UI_MODE;
	bool list_mode = mode != "play" && mode != "chat" && mode != "map";
	if (!list_mode) {
		out.push(rule("PAPERDOLL", w));
		int shown = 0;
		for (int s = 0; s < v.worn.size() && shown < panel_rows; ++s) {
			if (s == Slot.neck || s == Slot.ring || s == Slot.trinket) {
				if (v.worn[s].base < 0) { continue; }
			}
			Item it = v.worn[s];
			pane_begin(w);
			pane_add(SGR_DIM, `${SLOT_NAME[s]:<8}`);
			pane_add(it.base >= 0 ? rarity_sgr(it.rarity) : SGR_DIM, it.base >= 0 ? item_label(it) : "-");
			out.push(pane_end());
			shown = shown + 1;
		}
		if (shown < panel_rows) { out.push(pane_line(SGR_DIM, `armor ${v.armor}  luck ${v.luck}  pack ${v.pack.size()}/${PACK_MAX}`, w)); shown = shown + 1; }
		for (int p = 0; p < v.party_names.size() && shown < panel_rows; ++p) {
			pane_begin(w);
			pane_add("38;2;120;220;240", `party ${v.party_names[p]:<10} `);
			int left = w - PANE_W - 1;
			PANE_ROW += bar(v.party_hp[p], 100, left, "38;2;90;220;90", "38;2;30;60;30");
			PANE_W = PANE_W + left;
			out.push(pane_end());
			shown = shown + 1;
		}
		while (shown < panel_rows) { out.push(pane_line(SGR_DIM, "", w)); shown = shown + 1; }
	} else {
		out.push(rule(mode_title(v), w));
		build_menu(v);
		int n = MENU_LABELS.size();
		if (UI_SEL >= n) { UI_SEL = imax(0, n - 1); }
		if (UI_SEL < 0) { UI_SEL = 0; }
		if (UI_SEL < UI_SCROLL) { UI_SCROLL = UI_SEL; }
		if (UI_SEL >= UI_SCROLL + panel_rows) { UI_SCROLL = UI_SEL - panel_rows + 1; }
		bool selectable = mode != "skills" && mode != "journal" && mode != "help";
		for (int r = 0; r < panel_rows; ++r) {
			int i = UI_SCROLL + r;
			if (i >= n) { out.push(pane_line(SGR_DIM, "", w)); continue; }
			bool sel = selectable && i == UI_SEL;
			pane_begin(w);
			pane_add(sel ? SGR_SEL : MENU_SGR[i], (sel ? "\xe2\x96\xb6" : " ") + MENU_LABELS[i]);
			out.push(pane_end());
		}
	}
	out.push(rule("MAP", w));
	minimap_draw(v, w, mini_rows * 2);
	for (int r = 0; r < mini_rows; ++r) { out.push(mini_row(r)); }
	out.push(rule("JOURNAL LOG", w));
	// newest lines at the bottom, wrapped
	array<string> lines;
	array<int> cols;
	for (int i = v.log.size() - 1; i >= 0 && lines.size() < log_rows; --i) {
		array<string> wl = wrap(v.log[i], w - 1);
		for (int k = wl.size() - 1; k >= 0 && lines.size() < log_rows; --k) {
			lines.push(wl[k]);
			cols.push(v.log_col[i]);
		}
	}
	for (int r = log_rows - 1; r >= 0; --r) {
		if (r >= lines.size()) { out.push(pane_line(SGR_DIM, "", w)); continue; }
		int age = r;
		string sgr = LOG_SGR[iclamp(cols[r], 0, LOG_SGR.size() - 1)];
		if (age >= 6) { sgr = SGR_DIM; }
		out.push(pane_line(sgr, " " + lines[r], w));
	}
	while (out.size() < rows) { out.push(pane_line(SGR_DIM, "", w)); }
	return out;
}

// the full world map overlay drawn into the canvas (M)
void draw_world_map(ClientView& v) {
	int w = v.world;
	int ww = W_W[w];
	int wh = W_H[w];
	fill_px(0, 0, VW, VH, rgb(10, 12, 16));
	int sc = imax(1, imin(VW / ww, VH / wh));
	float fx = ww * 1.0 / VW;
	float fy = wh * 1.0 / VH;
	float f = fx > fy ? fx : fy;
	for (int py = 0; py < VH; ++py) {
		int ty = math::itrunc(py * f);
		if (ty >= wh) { break; }
		for (int px = 0; px < VW; ++px) {
			int tx = math::itrunc(px * f);
			if (tx >= ww) { break; }
			PIX[py * VW + px] = MINI_COL[cv_tile(v, tx, ty)];
		}
	}
	int me = cv_me(v);
	for (int g = 0; g < GATE_W.size(); ++g) {
		if (GATE_W[g] != w) { continue; }
		int gx = math::itrunc(GATE_X[g] / f);
		int gy = math::itrunc(GATE_Y[g] / f);
		fill_px(gx - 1, gy - 1, 3, 3, rgb(170, 120, 255));
		string label = GATE_DEST[g] == 0 ? "HOME" : WORLDS[GATE_DEST[g]].name;
		if (sc >= 1 && VW >= 100) { draw_text_px(string::to_upper(label.split(" ")[0]), gx + 3, gy - 2, rgb(220, 200, 255), COL_BLACK, 1); }
	}
	for (int t = 0; t < TOWN_W.size(); ++t) {
		if (TOWN_W[t] != w) { continue; }
		int tx = math::itrunc((TOWN_X0[t] + TOWN_X1[t]) / 2 / f);
		int ty = math::itrunc(TOWN_Y0[t] / f);
		draw_text_px(string::to_upper(TOWN_NAME[t]), tx - text_px_width(TOWN_NAME[t], 1) / 2, ty - 6, rgb(255, 230, 120), COL_BLACK, 1);
	}
	if (me >= 0 && FRAME % 20 < 14) {
		int mx = math::itrunc(v.m_x[me] / f);
		int my = math::itrunc(v.m_y[me] / f);
		fill_px(mx - 1, my - 1, 3, 3, rgb(255, 255, 255));
	}
	draw_text_px(WORLDS[w].name, 2, 2, rgb(255, 255, 255), COL_BLACK, 1);
}

string status_line(ClientView& v, int width) {
	pane_begin(width);
	if (UI_MODE == "chat") {
		pane_add("1;38;2;250;250;250", "say: ");
		pane_add("38;2;250;250;200", CHAT_BUF + (FRAME % 20 < 10 ? "_" : " "));
		return pane_end();
	}
	if (UI_TOAST_F > 0) {
		pane_add("1;38;2;250;220;90", " " + UI_TOAST);
		return pane_end();
	}
	pane_add("38;2;120;190;70", " WASD");
	pane_add(SGR_DIM, " move ");
	pane_add("38;2;120;190;70", "SPC");
	pane_add(SGR_DIM, " use ");
	pane_add("38;2;120;190;70", "F");
	pane_add(SGR_DIM, " target ");
	pane_add("38;2;120;190;70", "G");
	pane_add(SGR_DIM, " grab ");
	pane_add("38;2;120;190;70", "1-9");
	pane_add(SGR_DIM, " cast ");
	pane_add("38;2;120;190;70", "I P K J B C M");
	pane_add(SGR_DIM, " panes ");
	pane_add("38;2;120;190;70", "?");
	pane_add(SGR_DIM, " help ");
	pane_add("38;2;120;190;70", "esc");
	pane_add(SGR_DIM, " menu  ");
	pane_add(SGR_DIM, v.status);
	return pane_end();
}
