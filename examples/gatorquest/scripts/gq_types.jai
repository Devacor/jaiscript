// Gator Quest - the value classes the world is made of. Reloadable: a class
// redefinition migrates live instances (new fields take their initializers,
// existing fields keep their values).

import "gq_enums.jai";

class Item {
	int base = -1;
	int qty = 0;
	int rarity = 0;
	int prefix = -1;
	int suffix = -1;
	int pmag = 0;
	int smag = 0;
	int bonus = 0;
	int ser = 0;

	Item() {}
	Item(int b, int q) { base = b; qty = q; }

	bool empty() { return base < 0 || qty <= 0; }
}

// what one player's client knows: filled only from snapshots, read by the renderer and HUD
class ClientView {
	int slot = -1;
	int my_ser = -1;
	int tick = 0;
	int world = 0;
	int weather = 0;
	int tod = 0;
	int day = 1;
	int rx_ms = 0;
	int snaps = 0;
	array<int> m_ser;
	array<int> m_kind;
	array<int> m_x;
	array<int> m_y;
	array<int> m_px;
	array<int> m_py;
	array<int> m_mt;
	array<int> m_md;
	array<int> m_ppx;          // the move before the current one (drawing others a tick behind needs both)
	array<int> m_ppy;
	array<int> m_pmt;
	array<int> m_pmd;
	array<int> m_dir;
	array<int> m_hp;
	array<int> m_flags;
	array<int> m_role;
	array<int> m_slot;
	array<int> m_hue;
	array<string> m_name;
	array<int> i_ser;
	array<int> i_base;
	array<int> i_rar;
	array<int> i_x;
	array<int> i_y;
	array<int> i_qty;
	map<int, int> tover;
	int hp = 0;
	int hpmax = 1;
	int mp = 0;
	int mpmax = 1;
	int target_ser = -1;
	int action = 0;
	int action_left = 0;
	int gold = 0;
	int bank_gold = 0;
	int fame = 0;
	int karma = 0;
	int str = 0;
	int dex = 0;
	int intel = 0;
	int armor = 0;
	int luck = 0;
	int ghost = 0;
	string name = "";
	string title = "";
	int hue = 0;
	bool pvp = false;
	int party = -1;
	array<int> skills;
	array<Item> pack;
	array<Item> worn;
	array<Item> bank;
	array<int> spells;
	array<string> journal;
	int buff_night = 0;
	int buff_fire = 0;
	int buff_regen = 0;
	int sheet_version = -1;
	array<int> e_kind;
	array<int> e_w;
	array<int> e_x;
	array<int> e_y;
	array<int> e_a;
	array<int> e_b;
	array<int> e_c;
	array<string> e_text;
	array<string> log;
	array<int> log_col;
	int ack = 0;               // the last predicted step the host processed
	int auth_x = 0;            // where the host has this player's gator
	int auth_y = 0;
	bool slowed = false;
	bool sliding = false;
	bool has_auth = false;
	bool auth_fresh = false;   // a snapshot with a new authoritative position arrived (reconcile)
	int dlg_ser = -1;
	int dlg_role = -1;
	bool dlg_pending = false;
	array<string> party_names;
	array<int> party_hp;
	array<string> who;
	string status = "";
}

// an autopilot driving one player slot (smoke tests, net tests, idle demo)
class Bot {
	int slot = -1;
	int phase = 0;
	int phase_t = 0;
	int goal_w = -1;
	int goal_x = 0;
	int goal_y = 0;
	int stuck = 0;
	int last_x = -1;
	int last_y = -1;
	int done = 0;
	int kills_at = 0;
	int step = 0;
	string log = "";
	array<int> path;
	int path_i = 0;
	int path_w = -1;
	int path_tx = -1;
	int path_ty = -1;
}

class Character {
	bool active = false;
	bool connected = false;
	string name = "";
	int mob = -1;
	int hue = 0;
	int str = 30;
	int dex = 20;
	int intel = 20;
	array<int> skill;
	array<Item> pack;
	array<Item> worn;
	array<Item> bank;
	int gold = 0;
	int bank_gold = 0;
	int fame = 0;
	int karma = 0;
	int kills = 0;
	int deaths = 0;
	int murders = 0;
	array<int> spells;
	array<int> quest_stage;
	array<int> quest_count;
	array<int> visited;
	int boss_mask = 0;
	bool pvp = false;
	int party = -1;
	int invite = -1;
	int home_world = 0;
	int home_x = 0;
	int home_y = 0;
	int last_spell = -1;
	int talk_npc = -1;
	int talk_t = -1000;
	int action = 0;
	int action_t = 0;
	int action_x = 0;
	int action_y = 0;
	int action_arg = 0;
	int potion_t = 0;          // tick the next potion may be drunk
	int buff_night = 0;
	int buff_fire = 0;
	int buff_str = 0;
	int buff_regen = 0;
	int fish_caught = 0;
	int crafted = 0;
	int stat_t = 0;
	int version = 1;
	int played_ticks = 0;

	Character() {}
}
