// The Hud: the message ticker, and the frame around the view. In the
// reference look it draws the status strip, the vitals with bars, the ammo
// rack, the face, the bobbing weapon art and the full-screen states (title,
// tally, death, victory): the frames --smoke hashes and the ports reproduce.
// The showcase look hands all of that to Screens (screens.jai).

class Hud {
public:
	string msg = "";
	string msg_color = "97";
	int msg_t = 0;

	map<string, array<string>> faces = {
		"ok": [
			" .-----. ",
			" | o.o | ",
			" |  L  | ",
			" | --- | ",
			" '-----' "],
		"hurt": [
			" .-----. ",
			" | o.o | ",
			" | ,L  | ",
			" | ~~~ | ",
			" '-----' "],
		"bad": [
			" .-----. ",
			" | x.o |,",
			" | ,L. |,",
			" | www | ",
			" '-----' "],
		"crit": [
			" .-----. ",
			" |,x.x,|,",
			" |,,L,,|,",
			" | mmm |,",
			" '-----' "],
		"pain": [
			" .-----. ",
			" | >.< | ",
			" | ,L, | ",
			" | OOO | ",
			" '-----' "],
		"grin": [
			" .-----. ",
			" | ^.^ | ",
			" |  L  | ",
			" | \\_/ | ",
			" '-----' "],
		"dead": [
			" .-----. ",
			" | x.x | ",
			" |  L  | ",
			" | ___ | ",
			" '-----' "]
	};

	array<string> logo = [
		"  ..|'''.|  '||'       ..|''||    ..|''||   '||    ||' ",
		" .|'     '   ||       .|'    ||  .|'    ||   |||  |||  ",
		" ||    ....  ||       ||      || ||      ||  |'|..'||  ",
		" '|.    ||   ||       '|.     || '|.     ||  | '|' ||  ",
		"  ''|...'|  .||.....|  ''|...|'   ''|...|'  .|. | .||. "
	];

	void show(string text, string color) {
		msg = text;
		msg_color = color;
		msg_t = 75;
	}

	void update() {
		if (msg_t > 0) { msg_t = msg_t - 1; }
	}

	// after the Renderer's palette exists
	void init() {
		if (SHOWCASE) { screens = new Screens(G.w, G.h); }
	}

	// the per-frame entry point: full-screen state or the play view + panel
	string frame(float fps, float ms_sim, float ms_draw) {
		if (SHOWCASE) { return screens.frame(fps, ms_sim, ms_draw); }
		if (G.mode == 0) { return title(); }
		if (G.mode == 2) { return tally(); }
		if (G.mode == 3) { return dead(); }
		if (G.mode == 4) { return victory(); }
		return play(fps, ms_sim, ms_draw);
	}

	string face_name() {
		if (G.mode == 3) { return "dead"; }
		if (G.player.face_pain > 0) { return "pain"; }
		if (G.player.face_grin > 0) { return "grin"; }
		if (G.player.hp > 70) { return "ok"; }
		if (G.player.hp > 40) { return "hurt"; }
		if (G.player.hp > 18) { return "bad"; }
		return "crit";
	}

private:
	shared_ptr<Screens> screens = null;

	string meter(int val, int cap, int slots) {
		int fill = iclamp(val * slots / imax(1, cap), 0, slots);
		return string::repeat("#", fill) + string::repeat("-", slots - fill);
	}

	// the status strip above the panel: map, tallies, message
	string status_row() {
		string left = ` ${G.world.map_name}  KILLS ${G.kills}/${G.kill_total}  SCRT ${G.secrets}/${G.secret_total}`;
		string m = msg_t > 0 ? msg : "";
		int room = G.w - left.size() - 2;
		if (m.size() > room) { m = m.substr(0, imax(0, room)); }
		string body = pad_to(left + string::repeat(" ", imax(1, G.w - left.size() - m.size() - 1)) + m, G.w);
		string strip = ESC + "[48;5;235m" + ESC + "[38;5;250m";
		if (msg_t > 0) {
			strip = ESC + "[48;5;235m" + ESC + "[" + msg_color + "m";
		}
		return strip + body + RESET;
	}

	// five panel rows: vitals | ammo | face | weapon art
	array<string> panel_rows(float fps, float ms_sim, float ms_draw) {
		auto& wpn = WEAPONS[G.player.weapon];
		auto& face = faces[face_name()];

		// weapon art with walk-bob shift and fire frame
		array<string> art = G.player.cooldown > wpn.cd - 4 || G.player.gun_kick > 0
			? WGFX_FIRE[G.player.weapon] : WGFX_IDLE[G.player.weapon];
		int bob_shift = 1 + math::itrunc((math::sin(G.player.bob) + 1.0) * 1.4);

		array<string> left = [
			` HP  [${meter(G.player.hp, 100, 12)}] ${G.player.hp}`,
			` ARM [${meter(G.player.armor, 100, 12)}] ${G.player.armor}`,
			` `,
			` ${G.player.key_r ? "[R]" : " . "} ${G.player.key_b ? "[B]" : " . "}  ${wpn.name}`,
			` ${G.autopilot ? "AUTOPILOT" : ""}`
		];
		array<string> mid = [
			`BUL ${G.player.ammo[0]}/${AMMO_MAX[0]}`,
			`SHL ${G.player.ammo[1]}/${AMMO_MAX[1]}`,
			`CEL ${G.player.ammo[2]}/${AMMO_MAX[2]}`,
			`${G.player.have[1] == 1 ? "2:SCAT " : "       "}${G.player.have[2] == 1 ? "3:HEX" : ""}`,
			fps > 0.0 ? `${fps:.0f}fps ${ms_sim + ms_draw:.1f}ms` : ""
		];

		array<string> rows = [];
		int left_w = 34;
		int mid_w = 18;
		int face_w = 11;
		int art_w = 14;
		for (int r = 0; r < 5; ++r) {
			string lseg = pad_to(left[r], left_w);
			string mseg = pad_to(mid[r], mid_w);
			string fseg = pad_to(face[r], face_w);
			int used = left_w + mid_w + face_w + art_w;
			int fill = imax(0, G.w - used);
			string aseg = pad_to(string::repeat(" ", imin(fill > 0 ? bob_shift : 0, 2)) + art[r], art_w + fill);
			string hp_col = G.player.hp > 40 ? "97" : "91";
			rows.push(ESC + "[48;5;236m" + ESC + "[" + hp_col + "m" + lseg +
				ESC + "[38;5;179m" + mseg +
				ESC + "[38;5;114m" + fseg +
				ESC + "[38;5;250m" + aseg.substr(0, imax(0, G.w - left_w - mid_w - face_w)) + RESET);
		}
		return rows;
	}

	// ------------------------------------------------- full-screen modes ----
	// center the payload block on a dark field, pad every row to full width
	array<string> screen_rows(array<string> lines, string tint) {
		array<string> rows = [];
		int total = G.h;
		int top = imax(0, (total - lines.size()) / 2);
		for (int y = 0; y < total; ++y) {
			int li = y - top;
			string body = "";
			if (li >= 0 && li < lines.size()) { body = lines[li]; }
			int padl = imax(0, (G.w - body.size()) / 2);
			rows.push(ESC + "[48;5;233m" + tint + pad_to(string::repeat(" ", padl) + body, G.w) + RESET);
		}
		return rows;
	}

	string title() {
		array<string> lines = [];
		for (auto l : logo) { lines.push(l); }
		lines.push("");
		lines.push("a  t e r m i n a l  r a y c a s t e r ,  a l l  J a i S c r i p t");
		lines.push("");
		lines.push("");
		int blink = (G.tick / 12) % 2;
		lines.push(blink == 0 ? ">>> press FIRE to descend <<<" : "");
		lines.push("");
		lines.push("w/s move  a/d strafe  arrows turn  SPACE fire  E use  1/2/3 weapons  m map");
		string tint = (G.tick / 4) % 3 == 0 ? ESC + "[38;5;203m" : ESC + "[38;5;209m";
		return screen_rows(lines, tint).join("\n");
	}

	string tally() {
		int kp = G.kill_total > 0 ? G.kills * 100 / G.kill_total : 100;
		int sp = G.secret_total > 0 ? G.secrets * 100 / G.secret_total : 100;
		int secs = G.map_ticks / 30;
		int par_secs = G.world.par / 30;
		array<string> lines = [
			G.world.map_name,
			"",
			"=== FLOOR CLEARED ===",
			"",
			`KILLS    ${G.kills} / ${G.kill_total}   (${kp}%)`,
			`SECRETS  ${G.secrets} / ${G.secret_total}   (${sp}%)`,
			`TIME     ${secs}s   par ${par_secs}s`,
			"",
			G.map_i + 1 < MAPS.size() ? "press FIRE for the next floor" : "press FIRE to face the dark"
		];
		return screen_rows(lines, ESC + "[38;5;114m").join("\n");
	}

	string dead() {
		array<string> lines = [
			"Y O U   D I E D",
			"",
			`slain by ${G.player.death_cause}`,
			`on ${G.world.map_name}`,
			"",
			`kills ${G.kills}/${G.kill_total}   deaths ${G.deaths}`,
			"",
			"press FIRE to rise again"
		];
		return screen_rows(lines, ESC + "[38;5;196m").join("\n");
	}

	string victory() {
		int mins = G.ep_ticks / 1800;
		int secs = (G.ep_ticks / 30) % 60;
		array<string> lines = [];
		for (auto l : logo) { lines.push(l); }
		lines.push("");
		lines.push("T H E   W A R D E N   I S   D O W N");
		lines.push("");
		lines.push(`episode kills    ${G.ep_kills} / ${G.ep_kill_total}`);
		lines.push(`episode secrets  ${G.ep_secrets} / ${G.ep_secret_total}`);
		lines.push(`episode time     ${mins}m ${secs}s   deaths ${G.deaths}`);
		lines.push("");
		lines.push("the gloom lifts. for now.        (ESC quits)");
		return screen_rows(lines, ESC + "[38;5;220m").join("\n");
	}

	// the play-mode frame: world rows + status strip + panel
	string play(float fps, float ms_sim, float ms_draw) {
		array<string> rows = G.renderer.draw_world();
		rows.push(status_row());
		for (auto r : panel_rows(fps, ms_sim, ms_draw)) { rows.push(r); }
		return rows.join("\n");
	}
}
