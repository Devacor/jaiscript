// The showcase look's status bar, panel and full-screen pages (title, floor
// tally, death, victory), painted on half-block canvases. hud.jai keeps the
// reference look's versions, the frames --smoke hashes.

Canvas PANEL = Canvas(1, 1);   // the six rows under the view
Canvas PAGE = Canvas(1, 1);    // a whole-screen page

// the face in the panel: 10x10 pixel art, one per expression (hud.jai:face_name)
var FACE_PAL = {"h": [58, 40, 30], "s": [214, 160, 120], "S": [156, 106, 78], "e": [240, 236, 226], "p": [34, 30, 40],
	"B": [70, 44, 30], "m": [96, 30, 30], "t": [236, 228, 210], "b": [186, 26, 22], "g": [150, 150, 140], "x": [40, 20, 20]};
var FACE_SRC = {
	"ok":   ["..hhhhhh..", ".hhhhhhhh.", ".hsssssshh", ".sBBssBBsh", ".sepssepsS",
	         ".sssSsssss", ".ssSSSssS.", ".ssmmmmsS.", "..ssssss..", "...SSSS..."],
	"hurt": ["..hhhhhh..", ".hhhhhhhh.", ".hsssssshh", ".sBBssBBsh", ".sepssepsS",
	         ".sssSssSSs", ".ssSSSsSS.", ".ssmmmmsS.", "..sSssss..", "...SSSS..."],
	"bad":  ["..hhhhhh..", ".hhbhhhhh.", ".hsbsssshh", ".sBBssBBsh", ".sxxsseps.",
	         ".bssSssSSs", ".bsSSSsSS.", ".ssmmmmsS.", "..sSsSss..", "...SSSS..."],
	"crit": ["..hhbhhh..", ".hhbbhhhh.", ".hbbsbsshh", ".sBBbsBBsh", ".sxxssxxb.",
	         ".bbsSssSbs", ".bsSSSsbS.", ".sbttttbS.", "..sbsSsb..", "...SbSS..."],
	"pain": ["..hhhhhh..", ".hhhhhhhh.", ".hsBBsBBhh", ".sssssssh.", ".sxxssxxs.",
	         ".sssSsssss", ".ssmmmmsS.", ".ssmttmsS.", "..smmmms..", "...SSSS..."],
	"grin": ["..hhhhhh..", ".hhhhhhhh.", ".hsssssshh", ".sBBssBBsh", ".sepssepsS",
	         ".sssSsssss", ".smSSSSms.", ".sttttttS.", "..smmmms..", "...SSSS..."],
	"dead": ["..hhhhhh..", ".hhhbhhhh.", ".hgggggghh", ".gxgggxgg.", ".ggxggxgg.",
	         ".gxgggxgg.", ".gggSgggg.", ".ggxxxxgb.", "..gggggb..", "...gggb..."]
};
var FACES_ART = {};
map<string, array<int>> FACE_LUTS = {};

function init_screens() -> void {
	PANEL = Canvas(G.w, 6);
	PAGE = Canvas(G.w, G.h);
	FACES_ART = {};
	FACE_LUTS = {};
	for (auto kv : FACE_SRC) {
		var art = parse_art({"pal": FACE_PAL, "rows": kv.second});
		FACES_ART[kv.first] = art;
		FACE_LUTS[kv.first] = plain_lut(art);
	}
}

// a code -> palette index table for an art, unshaded
function plain_lut(var& a) -> array<int> {
	array<int> lut = [0];
	var& rgbs = a["rgb"];
	for (int c = 1; c < rgbs.size(); ++c) { lut.push(rgb_idx(rgbs[c][0], rgbs[c][1], rgbs[c][2])); }
	return lut;
}

// five font-row colors from top to bottom
function shades5(math::color top, math::color bottom) -> array<int> {
	array<int> out = [];
	for (int i = 0; i < 5; ++i) { out.push(color_idx(math::mix_rgb(top, bottom, i / 4.0))); }
	return out;
}

// the status-bar color of a message (show_msg takes an SGR color code)
function message_color(string code) -> int {
	if (code == "91") { return rgb_idx(255, 96, 80); }
	if (code == "92") { return rgb_idx(120, 236, 120); }
	if (code == "93") { return rgb_idx(255, 214, 90); }
	if (code == "94") { return rgb_idx(110, 160, 255); }
	if (code == "96") { return rgb_idx(110, 230, 230); }
	if (code == "90") { return rgb_idx(130, 126, 136); }
	return rgb_idx(240, 236, 228);
}

// ------------------------------------------------------------------- HUD ----
// HUD rows under the view: the status bar, then health, armor, the face, the
// weapon's ammo and the arms rack with keycards
function showcase_hud_rows(float fps, float ms_sim, float ms_draw) -> array<string> {
	auto& c = PANEL;
	int w = c.w;
	auto& mood = MOODS[G.map_i];
	c.clear(rgb_idx(14, 12, 18));
	c.gradient(2, 12, rgb(62, 58, 70), rgb(22, 20, 26));
	c.rect(0, 11, w, 1, rgb_idx(10, 8, 12));

	// status bar: where, how it is going, what just happened
	int dim = rgb_idx(132, 124, 136);
	string map_code = G.map_name.substr(0, 4);
	string map_title = G.map_name.substr(6, G.map_name.size() - 6);
	c.text(1, 0, map_code, color_idx(mood.light));
	c.text(6, 0, map_title, rgb_idx(214, 206, 196));
	string tally = `KILLS ${G.kills}/${G.kill_total}   SECRETS ${G.secrets}/${G.secret_total}`;
	int tally_x = 7 + map_title.size() + 3;
	c.text(tally_x, 0, tally, dim);
	if (G.msg_t > 0) {
		int room = w - (tally_x + tally.size()) - 3;
		string m = G.msg.size() > room ? G.msg.substr(0, imax(0, room)) : G.msg;
		c.text_right(w - 1, 0, m, message_color(G.msg_color));
	}

	int stat_w = iclamp((w - 33) / 3, 12, 17);
	int x_hp = 1;
	int x_armor = x_hp + stat_w + 1;
	int x_face = x_armor + stat_w + 1;
	int x_ammo = x_face + 13;
	int x_arms = x_ammo + stat_w + 1;
	int sep = rgb_idx(14, 12, 18);
	for (auto x : [x_armor - 1, x_face - 1, x_ammo - 1, x_arms - 1]) { c.rect(x, 3, 1, 8, sep); }

	array<int> red_digits = shades5(rgb(255, 220, 130), rgb(214, 64, 40));
	array<int> blue_digits = shades5(rgb(200, 232, 255), rgb(64, 120, 220));
	array<int> gold_digits = shades5(rgb(255, 244, 196), rgb(214, 150, 54));
	int hp_bar = G.hp > 100 ? rgb_idx(120, 220, 255) : (G.hp > 60 ? rgb_idx(110, 220, 90) : (G.hp > 30 ? rgb_idx(240, 190, 60) : rgb_idx(240, 60, 40)));
	stat_block(c, x_hp, stat_w, "HEALTH", G.hp, imin(G.hp, 100), 100, red_digits, hp_bar);
	stat_block(c, x_armor, stat_w, "ARMOR", G.armor, G.armor, 100, blue_digits, rgb_idx(90, 150, 240));

	c.rect(x_face, 2, 12, 10, rgb_idx(18, 16, 22));
	if (HOST_ASCII) {
		var& lines = FACES[face_name()];
		for (int r = 0; r < 5; ++r) { c.text(x_face + 1, r + 1, lines[r], rgb_idx(226, 180, 140)); }
	} else {
		c.art(FACES_ART[face_name()], x_face + 1, 2, FACE_LUTS[face_name()]);
	}

	var weapon = WEAPONS[G.weapon];
	int pool = weapon["ammo"];
	array<int> pool_colors = [rgb_idx(226, 186, 84), rgb_idx(226, 96, 64), rgb_idx(90, 230, 220)];
	stat_block(c, x_ammo, stat_w, weapon["name"], G.ammo[pool], G.ammo[pool], AMMO_MAX[pool], gold_digits, pool_colors[pool]);

	int arms_w = w - x_arms - 1;
	if (arms_w >= 11) {
		c.text(x_arms + 1, 1, "ARMS", dim);
		for (int slot = 0; slot < 3; ++slot) {
			int sx = x_arms + 1 + slot * 4;
			bool owned = G.have[slot] == 1;
			if (slot == G.weapon) { c.rect(sx, 4, 3, 2, rgb_idx(132, 100, 40)); }
			c.text(sx + 1, 2, `${slot + 1}`, owned ? rgb_idx(250, 240, 210) : rgb_idx(70, 66, 76));
		}
		if (arms_w >= 26) {
			c.text(x_arms + 14, 2, `B${G.ammo[0]} S${G.ammo[1]} C${G.ammo[2]}`, dim);
		}
		key_chip(c, x_arms + 1, 3, "RED", G.key_r, rgb_idx(200, 40, 34));
		key_chip(c, x_arms + 7, 3, "BLUE", G.key_b, rgb_idx(44, 90, 210));
		if (G.autopilot) { c.text(x_arms + 1, 5, "AUTOPILOT", rgb_idx(110, 230, 230)); }
		else if (fps > 0.0) { c.text(x_arms + 1, 5, `${fps:.0f} fps ${ms_sim + ms_draw:.1f} ms`, dim); }
	}
	return c.rows(0, 6);
}

// a labelled big number with a bar beneath it
function stat_block(Canvas& c, int x, int width, string label, int value, int bar, int bar_max, var& digits,
		int bar_color) -> void {
	c.text(x + 1, 1, label, rgb_idx(150, 140, 132));
	string number = `${value}`;
	if (HOST_ASCII) { c.text_right(x + width - 1, 3, number, digits[1]); }
	else { font5(c, number, x + width - 1 - font5_width(number, 1), 4, 1, digits, rgb_idx(16, 12, 14)); }
	int bar_w = width - 2;
	int filled = iclamp(bar * bar_w / imax(1, bar_max), 0, bar_w);
	c.rect(x + 1, 10, bar_w, 1, rgb_idx(40, 36, 44));
	c.rect(x + 1, 10, filled, 1, bar_color);
}

function key_chip(Canvas& c, int x, int row, string name, bool held, int color) -> void {
	if (held) {
		c.rect(x, row * 2, name.size() + 2, 2, color);
		c.text(x + 1, row, name, rgb_idx(255, 255, 255));
	} else {
		c.text(x + 1, row, name, rgb_idx(76, 70, 82));
	}
}

// ----------------------------------------------------------------- pages ----
function ember(int i, int salt) -> int { return mix32(mix32(i * 2654435 + 7, salt), 99991); }

// sparks rising from the bottom of the page
function draw_embers(Canvas& c, int count, math::color hot, math::color cool) -> void {
	int height = c.h * 2;
	int period = height + 16;
	for (int i = 0; i < count; ++i) {
		int speed = 1 + ember(i, 1) % 3;
		int x = ember(i, 2) % c.w;
		int rise = (G.tick * speed / 2 + ember(i, 3) % period) % period;
		int y = height - rise;
		if ((ember(i, G.tick / 4) & 3) == 0) { continue; }
		float t = 1.0 * rise / period;
		c.dot(x + ((rise / 6 + i) % 3) - 1, y, color_idx(math::mix_rgb(hot, cool, t)));
	}
}

// a heading in the pixel font, centered at pixel row y, at the largest scale that fits
function heading(Canvas& c, string s, int y, int max_scale, var& top_bottom, int shadow) -> int {
	if (HOST_ASCII) {
		c.text_centered(y / 2 + 1, s.split("").join(" "), top_bottom[1]);
		return 4;
	}
	int scale = max_scale;
	while (scale > 1 && font5_width(s, scale) > c.w - 4) { scale = scale - 1; }
	font5(c, s, (c.w - font5_width(s, scale)) / 2, y, scale, top_bottom, shadow);
	return 5 * scale;
}

function blink(int period) -> bool { return (G.tick / period) % 2 == 0; }

// the logo, 57 x 11 pixels: G L O O M
array<string> LOGO_ART = [
	"..######....###...........######......######....###....###",
	".########...###..........########....########...####..####",
	"###....###..###.........###....###..###....###..##.####.##",
	"##..........###.........##......##..##......##..##..##..##",
	"##..........###.........##......##..##......##..##......##",
	"##...#####..###.........##......##..##......##..##......##",
	"##...#####..###.........##......##..##......##..##......##",
	"##......##..###.........##......##..##......##..##......##",
	"###....###..###.........###....###..###....###..##......##",
	".########...#########....########....########...##......##",
	"..######....#########.....######......######....##......##"
];

// outlined, shadowed, lit from above; a bleeding logo gathers drops under its
// letters that fall
function draw_logo(Canvas& c, int x, int y, math::color top, math::color bottom, bool bleeding) -> void {
	if (HOST_ASCII) {
		for (int r = 0; r < LOGO.size(); ++r) {
			c.text_centered(y / 2 + r, LOGO[r], color_idx(math::mix_rgb(top, bottom, r / 4.0)));
		}
		return;
	}
	int lw = LOGO_ART[0].size();
	int lh = LOGO_ART.size();
	int outline = rgb_idx(6, 2, 4);
	int shadow = rgb_idx(40, 6, 8);
	for (int ly = 0; ly < lh; ++ly) {
		for (int lx = 0; lx < lw; ++lx) {
			if (LOGO_ART[ly].substr(lx, 1) != "#") { continue; }
			c.rect(x + lx - 1, y + ly - 1, 3, 3, outline);
		}
	}
	for (int ly = 0; ly < lh; ++ly) {
		for (int lx = 0; lx < lw; ++lx) {
			if (LOGO_ART[ly].substr(lx, 1) == "#") { c.dot(x + lx + 1, y + ly + 2, shadow); }
		}
	}
	for (int ly = 0; ly < lh; ++ly) {
		int fill = color_idx(math::mix_rgb(top, bottom, 1.0 * ly / (lh - 1)));
		for (int lx = 0; lx < lw; ++lx) {
			if (LOGO_ART[ly].substr(lx, 1) == "#") { c.dot(x + lx, y + ly, fill); }
		}
	}
	if (!bleeding) { return; }
	int blood = rgb_idx(150, 14, 12);
	int drop = rgb_idx(210, 28, 22);
	for (int lx = 0; lx < lw; ++lx) {
		if (LOGO_ART[lh - 1].substr(lx, 1) != "#" || ember(lx, 21) % 4 != 0) { continue; }
		int length = 1 + ember(lx, 22) % 4;
		c.rect(x + lx, y + lh, 1, length, blood);
		int fall = (G.tick + ember(lx, 23) % 60) % 60;
		if (fall < 24) { c.dot(x + lx, y + lh + length + fall / 2, drop); }
	}
}

function title_page() -> array<string> {
	auto& c = PAGE;
	c.clear(0);
	int height = c.h * 2;
	int glow_at = height * 3 / 5;
	c.gradient(0, glow_at, rgb(6, 5, 9), rgb(22, 8, 12));
	c.gradient(glow_at, height, rgb(22, 8, 12), rgb(84, 26, 12));
	draw_embers(c, c.w / 2, rgb(255, 214, 120), rgb(110, 30, 16));

	bool flicker = ember(G.tick / 3, 5) % 9 == 0;
	math::color bone = flicker ? rgb(190, 180, 160) : rgb(248, 238, 216);
	int logo_y = imax(2, height / 2 - 24);
	draw_logo(c, (c.w - LOGO_ART[0].size()) / 2, logo_y, bone, rgb(160, 24, 20), true);
	int row = (logo_y + LOGO_ART.size()) / 2 + 4;
	c.text_centered(row, "a terminal raycaster, written in JaiScript", rgb_idx(186, 154, 130));
	if (blink(12)) { c.text_centered(row + 3, "-  press FIRE to descend  -", rgb_idx(255, 226, 160)); }
	int keys = rgb_idx(250, 214, 150);
	int words = rgb_idx(150, 132, 126);
	controls_line(c, row + 6, ["W S", "move", "A D", "strafe", "LEFT RIGHT", "turn", "SHIFT", "run"], keys, words);
	controls_line(c, row + 7, ["SPACE", "fire", "E", "use", "1 2 3", "weapons", "M", "map", "ESC", "quit"], keys, words);
	return c.rows(0, c.h);
}

// "key word   key word ..." centered, keys bright, words dim
function controls_line(Canvas& c, int row, array<string> pairs, int key_color, int word_color) -> void {
	int width = 0;
	for (int i = 0; i < pairs.size(); i += 2) { width += pairs[i].size() + 1 + pairs[i + 1].size() + 4; }
	int x = (c.w - (width - 4)) / 2;
	for (int i = 0; i < pairs.size(); i += 2) {
		c.text(x, row, pairs[i], key_color);
		x += pairs[i].size() + 1;
		c.text(x, row, pairs[i + 1], word_color);
		x += pairs[i + 1].size() + 4;
	}
}

function clock_text(int ticks) -> string {
	int secs = ticks / 30;
	string pad = secs % 60 < 10 ? "0" : "";
	return `${secs / 60}:${pad}${secs % 60}`;
}

// the intermission: the floor's numbers count up, DOOM style
function tally_page() -> array<string> {
	auto& c = PAGE;
	c.clear(0);
	auto& mood = MOODS[G.map_i];
	int height = c.h * 2;
	c.gradient(0, height, math::mix_rgb(mood.fog, rgb(0, 0, 0), 0.3), math::mix_rgb(mood.ceiling, mood.light, 0.12));
	array<int> gold = shades5(rgb(255, 246, 200), rgb(220, 150, 60));
	int top = imax(2, height / 2 - 22);
	c.text_centered(top / 2, G.map_name, color_idx(mood.light));
	int head_h = heading(c, "FLOOR CLEARED", top + 4, 2, gold, rgb_idx(8, 6, 8));
	int row = (top + 4 + head_h) / 2 + 3;
	int counted = G.mode_t * 2;
	int kills = imin(G.kills, counted / 3);
	int secrets = imin(G.secrets, counted / 9);
	int time = imin(G.map_ticks, counted * 40);
	int kp = G.kill_total > 0 ? kills * 100 / G.kill_total : 100;
	int sp = G.secret_total > 0 ? secrets * 100 / G.secret_total : 100;
	int label = rgb_idx(170, 160, 150);
	int value = rgb_idx(255, 236, 190);
	int x = c.w / 2 - 17;
	tally_row(c, x, row, "KILLS", `${kills} / ${G.kill_total}`, kp, label, value);
	tally_row(c, x, row + 2, "SECRETS", `${secrets} / ${G.secret_total}`, sp, label, value);
	c.text(x, row + 4, "TIME", label);
	c.text(x + 10, row + 4, clock_text(time), value);
	c.text(x + 18, row + 4, `PAR ${clock_text(G.par)}`, label);
	bool done = kills == G.kills && secrets == G.secrets && time == G.map_ticks;
	if (done && blink(12)) {
		string next = G.map_i + 1 < MAPS.size() ? "press FIRE for the next floor" : "press FIRE to face the dark";
		c.text_centered(row + 8, next, rgb_idx(255, 226, 160));
	}
	return c.rows(0, c.h);
}

function tally_row(Canvas& c, int x, int row, string name, string count, int percent, int label, int value) -> void {
	c.text(x, row, name, label);
	c.text(x + 10, row, count, value);
	c.text_right(x + 24, row, `${percent}%`, value);
	c.rect(x + 26, row * 2, 10, 2, rgb_idx(40, 34, 40));
	c.rect(x + 26, row * 2, percent / 10, 2, percent == 100 ? rgb_idx(120, 230, 110) : rgb_idx(226, 176, 70));
}

function dead_page() -> array<string> {
	auto& c = PAGE;
	c.clear(0);
	int height = c.h * 2;
	c.gradient(0, height, rgb(36, 4, 6), rgb(8, 2, 3));
	// blood runs down from the top, each run at its own pace and to its own depth
	int trail = rgb_idx(96, 8, 8);
	int bead = rgb_idx(214, 34, 28);
	for (int x = 0; x < c.w; ++x) {
		if (ember(x, 10) % 3 != 0) { continue; }
		int depth = height / 4 + ember(x, 13) % (height / 2);
		int length = imin(depth, G.mode_t * (2 + ember(x, 11) % 5) / 6 - ember(x, 12) % 12);
		for (int y = 0; y < length; ++y) { c.dot(x, y, y == length - 1 ? bead : trail); }
	}
	array<int> red = shades5(rgb(255, 120, 100), rgb(150, 10, 10));
	int top = height / 2 - 14;
	int head_h = heading(c, "YOU DIED", top, 2, red, rgb_idx(6, 0, 0));
	int row = (top + head_h) / 2 + 2;
	c.text_centered(row, `slain by ${G.death_cause} on ${G.map_name}`, rgb_idx(236, 200, 190));
	c.text_centered(row + 2, `kills ${G.kills}/${G.kill_total}    deaths ${G.deaths}`, rgb_idx(170, 130, 126));
	if (G.mode_t > 25 && blink(12)) { c.text_centered(row + 5, "press FIRE to rise again", rgb_idx(255, 226, 160)); }
	return c.rows(0, c.h);
}

function victory_page() -> array<string> {
	auto& c = PAGE;
	c.clear(0);
	int height = c.h * 2;
	c.gradient(0, height, rgb(10, 8, 14), rgb(70, 52, 20));
	draw_embers(c, c.w / 3, rgb(255, 240, 170), rgb(120, 90, 30));
	int top = imax(2, height / 2 - 24);
	draw_logo(c, (c.w - LOGO_ART[0].size()) / 2, top, rgb(255, 250, 220), rgb(214, 140, 40), false);
	int row = (top + LOGO_ART.size()) / 2 + 4;
	c.text_centered(row, "T H E   W A R D E N   I S   D O W N", rgb_idx(255, 230, 160));
	int label = rgb_idx(186, 170, 150);
	int x = c.w / 2 - 14;
	c.text(x, row + 3, "EPISODE KILLS", label);
	c.text(x + 18, row + 3, `${G.ep_kills} / ${G.ep_kill_total}`, rgb_idx(255, 246, 210));
	c.text(x, row + 4, "SECRETS", label);
	c.text(x + 18, row + 4, `${G.ep_secrets} / ${G.ep_secret_total}`, rgb_idx(255, 246, 210));
	c.text(x, row + 5, "TIME", label);
	c.text(x + 18, row + 5, clock_text(G.ep_ticks), rgb_idx(255, 246, 210));
	c.text(x, row + 6, "DEATHS", label);
	c.text(x + 18, row + 6, `${G.deaths}`, rgb_idx(255, 246, 210));
	c.text_centered(row + 9, "the gloom lifts. for now.      ESC quits", rgb_idx(170, 150, 120));
	return c.rows(0, c.h);
}

function showcase_frame(float fps, float ms_sim, float ms_draw) -> string {
	if (G.mode == 0) { return title_page().join("\n"); }
	if (G.mode == 2) { return tally_page().join("\n"); }
	if (G.mode == 3) { return dead_page().join("\n"); }
	if (G.mode == 4) { return victory_page().join("\n"); }
	var rows = render_view();
	for (auto& r : showcase_hud_rows(fps, ms_sim, ms_draw)) { rows.push(r); }
	return rows.join("\n");
}
