// The reference look's HUD (status strip, vitals with bars, ammo rack, the
// face, the bobbing weapon art) and full-screen states (title, tally, death,
// victory): the frames --smoke hashes and the ports reproduce. The showcase
// look draws its own (screens.jai).

var FACES = {
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

function face_name() -> string {
	if (G.mode == 3) { return "dead"; }
	if (G.face_pain > 0) { return "pain"; }
	if (G.face_grin > 0) { return "grin"; }
	if (G.hp > 70) { return "ok"; }
	if (G.hp > 40) { return "hurt"; }
	if (G.hp > 18) { return "bad"; }
	return "crit";
}

function meter(int val, int cap, int slots) -> string {
	int fill = iclamp(val * slots / imax(1, cap), 0, slots);
	return string::repeat("#", fill) + string::repeat("-", slots - fill);
}

function show_msg(string text, string color) -> void {
	G.msg = text;
	G.msg_color = color;
	G.msg_t = 75;
}

// the status strip above the panel: map, tallies, message
function hud_status_row() -> string {
	string left = ` ${G.map_name}  KILLS ${G.kills}/${G.kill_total}  SCRT ${G.secrets}/${G.secret_total}`;
	string m = G.msg_t > 0 ? G.msg : "";
	int room = G.w - left.size() - 2;
	if (m.size() > room) { m = m.substr(0, imax(0, room)); }
	string body = pad_to(left + string::repeat(" ", imax(1, G.w - left.size() - m.size() - 1)) + m, G.w);
	string strip = ESC + "[48;5;235m" + ESC + "[38;5;250m";
	if (G.msg_t > 0) {
		strip = ESC + "[48;5;235m" + ESC + "[" + G.msg_color + "m";
	}
	return strip + body + RESET;
}

// five panel rows: vitals | ammo | face | weapon art
function hud_panel_rows(float fps, float ms_sim, float ms_draw) -> var {
	var w = WEAPONS[G.weapon];
	var face = FACES[face_name()];

	// weapon art with walk-bob shift and fire frame
	var art = G.cooldown > WEAPONS[G.weapon]["cd"] - 4 || G.gun_kick > 0
		? WGFX_FIRE[G.weapon] : WGFX_IDLE[G.weapon];
	int bob_shift = 1 + math::itrunc((math::sin(G.bob) + 1.0) * 1.4);

	array<string> left = [
		` HP  [${meter(G.hp, 100, 12)}] ${G.hp}`,
		` ARM [${meter(G.armor, 100, 12)}] ${G.armor}`,
		` `,
		` ${G.key_r ? "[R]" : " . "} ${G.key_b ? "[B]" : " . "}  ${w["name"]}`,
		` ${G.autopilot ? "AUTOPILOT" : ""}`
	];
	array<string> mid = [
		`BUL ${G.ammo[0]}/${AMMO_MAX[0]}`,
		`SHL ${G.ammo[1]}/${AMMO_MAX[1]}`,
		`CEL ${G.ammo[2]}/${AMMO_MAX[2]}`,
		`${G.have[1] == 1 ? "2:SCAT " : "       "}${G.have[2] == 1 ? "3:HEX" : ""}`,
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
		string hp_col = G.hp > 40 ? "97" : "91";
		rows.push(ESC + "[48;5;236m" + ESC + "[" + hp_col + "m" + lseg +
			ESC + "[38;5;179m" + mseg +
			ESC + "[38;5;114m" + fseg +
			ESC + "[38;5;250m" + aseg.substr(0, imax(0, G.w - left_w - mid_w - face_w)) + RESET);
	}
	return rows;
}

// ------------------------------------------------------- full-screen modes --
array<string> LOGO = [
	"  ..|'''.|  '||'       ..|''||    ..|''||   '||    ||' ",
	" .|'     '   ||       .|'    ||  .|'    ||   |||  |||  ",
	" ||    ....  ||       ||      || ||      ||  |'|..'||  ",
	" '|.    ||   ||       '|.     || '|.     ||  | '|' ||  ",
	"  ''|...'|  .||.....|  ''|...|'   ''|...|'  .|. | .||. "
];

function screen_rows(var& lines, string tint) -> var {
	// center the payload block on a dark field, pad every row to full width
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

function render_title() -> string {
	array<string> lines = [];
	for (auto l : LOGO) { lines.push(l); }
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

function render_tally() -> string {
	int kp = G.kill_total > 0 ? G.kills * 100 / G.kill_total : 100;
	int sp = G.secret_total > 0 ? G.secrets * 100 / G.secret_total : 100;
	int secs = G.map_ticks / 30;
	int par_secs = G.par / 30;
	array<string> lines = [
		G.map_name,
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

function render_dead() -> string {
	array<string> lines = [
		"Y O U   D I E D",
		"",
		`slain by ${G.death_cause}`,
		`on ${G.map_name}`,
		"",
		`kills ${G.kills}/${G.kill_total}   deaths ${G.deaths}`,
		"",
		"press FIRE to rise again"
	];
	return screen_rows(lines, ESC + "[38;5;196m").join("\n");
}

function render_victory() -> string {
	int mins = G.ep_ticks / 1800;
	int secs = (G.ep_ticks / 30) % 60;
	array<string> lines = [];
	for (auto l : LOGO) { lines.push(l); }
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
function render_play(float fps, float ms_sim, float ms_draw) -> string {
	var rows = render_view();
	rows.push(hud_status_row());
	for (auto r : hud_panel_rows(fps, ms_sim, ms_draw)) { rows.push(r); }
	return rows.join("\n");
}

function render_frame(float fps, float ms_sim, float ms_draw) -> string {
	if (SHOWCASE) { return showcase_frame(fps, ms_sim, ms_draw); }
	if (G.mode == 0) { return render_title(); }
	if (G.mode == 2) { return render_tally(); }
	if (G.mode == 3) { return render_dead(); }
	if (G.mode == 4) { return render_victory(); }
	return render_play(fps, ms_sim, ms_draw);
}
