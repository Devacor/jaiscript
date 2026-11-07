// The cockpit around the view: the title bar, the six text boxes of the multi-view (log, map,
// compass, character, monsters and loot in sight), the panel beside the forward view alone,
// the screen fit, the turn and step animations, the message log, and the pages the forward
// pane shows in place of the view (the map, help, pack, character sheet, level-up, quit).

// ------------------------------------------------------------------ lines ---
// A line of box text knows its width in cells, so a glyph of three UTF-8 bytes counts as one.
class Line {
	string text = "";
	int cells = 0;

	void put(int ink, string ascii) {
		text += sgr_fg(ink) + ascii;
		cells += ascii.size();
	}

	void glyphs(int ink, string glyph, int count) {
		if (count <= 0) { return; }
		text += sgr_fg(ink) + string::repeat(glyph, count);
		cells += count;
	}

	void painted(string s) {
		text += s;
		cells += vis_len(s);
	}

	void gap(int count) {
		if (count <= 0) { return; }
		text += string::repeat(" ", count);
		cells += count;
	}
}

function line_of(int ink, string ascii) -> Line {
	Line l;
	l.put(ink, ascii);
	return l;
}

function painted_line(string s) -> Line {
	Line l;
	l.painted(s);
	return l;
}

// the visible cells of ASCII text carrying SGR escapes
function vis_len(string s) -> int {
	int n = s.size();
	int e = s.find(ESC);
	while (e >= 0) {
		int m = s.find("m", e);
		if (m < 0) { m = s.size() - 1; }
		n -= m - e + 1;
		e = s.find(ESC, m + 1);
	}
	return n;
}

function plain_text(string s) -> string {
	string out = "";
	int i = 0;
	while (i < s.size()) {
		int e = s.find(ESC, i);
		if (e < 0) { return out + s.substr(i, s.size() - i); }
		out += s.substr(i, e - i);
		int m = s.find("m", e);
		if (m < 0) { return out; }
		i = m + 1;
	}
	return out;
}

// ASCII text with escapes cut to w cells
function clip_text(string s, int w) -> string {
	string out = "";
	int n = 0;
	int i = 0;
	while (i < s.size() && n < w) {
		if (s[i] == ESC) {
			int m = s.find("m", i);
			if (m < 0) { break; }
			out += s.substr(i, m - i + 1);
			i = m + 1;
			continue;
		}
		int e = s.find(ESC, i);
		int stop = e < 0 ? s.size() : e;
		int take = imin(stop - i, w - n);
		out += s.substr(i, take);
		n += take;
		i += take;
	}
	return out;
}

// Word-wraps ASCII text to w cells, continuation lines indented one; the colour in force at a
// break carries on to the next line.
function wrap_text(string s, int w) -> array<string> {
	if (vis_len(s) <= w) { return [s]; }
	array<string> chars;     // the visible characters
	array<string> before;    // the escapes just before each one
	array<string> tint;      // the colour each one is drawn in
	string pending = "";
	string current = "";
	int i = 0;
	while (i < s.size()) {
		int e = s.find(ESC, i);
		int stop = e < 0 ? s.size() : e;
		for (int k = i; k < stop; ++k) {
			chars.push(s.substr(k, 1));
			before.push(pending);
			tint.push(current);
			pending = "";
		}
		if (e < 0) { break; }
		int m = s.find("m", e);
		if (m < 0) { break; }
		string esc = s.substr(e, m - e + 1);
		pending += esc;
		current = esc;
		i = m + 1;
	}
	array<string> lines;
	int n = chars.size();
	int a = 0;
	while (a < n) {
		while (a < n && chars[a] == " ") { a += 1; }
		if (a >= n) { break; }
		int b = a + (lines.size() == 0 ? w : w - 1);
		if (b >= n) {
			b = n;
		} else {
			int cut = b;
			while (cut > a && chars[cut] != " ") { cut -= 1; }
			if (cut > a) { b = cut; }
		}
		string line = lines.size() == 0 ? "" : " ";
		line += tint[a] + chars[a];
		for (int k = a + 1; k < b; ++k) { line += before[k] + chars[k]; }
		lines.push(line);
		a = b;
	}
	return lines;
}

// a meter: filled cells in ink with an eighth-cell edge, the rest a dark groove
function meter(int cur, int cap, int width, int ink) -> Line {
	Line l;
	int eighths = iclamp((imax(0, cur) * width * 8) / imax(1, cap), 0, width * 8);
	int full = eighths / 8;
	l.glyphs(ink, G_FULL, full);
	if (full < width) {
		int part = eighths % 8;
		int groove = mix_rgb(ink, INK_PANEL, 0.78);
		if (part > 0) {
			l.text += sgr_bg(groove);
			l.glyphs(ink, G_EIGHTHS[part - 1], 1);
			l.text += sgr_bg(INK_PANEL);
		}
		l.glyphs(groove, ASCII_ONLY ? "-" : G_FULL, width - full - (part > 0 ? 1 : 0));
	}
	return l;
}

function health_ink(int hp, int cap) -> int {
	if (hp * 10 >= cap * 6) { return INK_GOOD; }
	if (hp * 10 >= cap * 3) { return INK_GOLD; }
	return INK_BAD;
}

// -------------------------------------------------------------- the log ---
int LOG_KEEP = 60;

function log_msg(string text) -> void {
	if (G.msgs.size() > 0 && G.msgs[G.msgs.size() - 1] == text) {
		G.msg_counts[G.msgs.size() - 1] += 1;
		G.msg_turns[G.msgs.size() - 1] = G.turn;
		return;
	}
	G.msgs.push(text);
	G.msg_turns.push(G.turn);
	G.msg_counts.push(1);
	if (G.msgs.size() > LOG_KEEP) {
		G.msgs.remove(0);
		G.msg_turns.remove(0);
		G.msg_counts.remove(0);
	}
}

function log_ink(int ink, string text) -> void { log_msg(paint(ink, text)); }

// a message as shown: since your last move in its colours, older ones dimmed, repeats counted
function shown_message(int i) -> string {
	string text = G.msgs[i];
	if (G.msg_turns[i] < G.turn - 1) { text = paint(INK_DIM, plain_text(text)); }
	if (G.msg_counts[i] > 1) { text += paint(INK_FAINT, ` x${G.msg_counts[i]}`); }
	return text;
}

// ------------------------------------------------------------ the frame ---
// the bar over the canvas: the game, the floor's name, the depth and the turn
function title_bar(int width) -> string {
	Theme theme = theme_of(G.floor);
	Line l;
	l.text = sgr_bg(INK_BACK);
	l.put(INK_TITLE, " GRUBWELL ");
	l.glyphs(INK_FAINT, G_DOT, 1);
	l.put(theme.glow, ` ${theme.name}`);
	if (G.player.god) { l.put(INK_MAGIC, "  [tourist]"); }
	Line right;
	right.put(INK_DIM, "floor ");
	right.put(INK_BRIGHT, `${G.floor}`);
	right.put(INK_DIM, "/10   turn ");
	right.put(INK_BRIGHT, `${G.turn} `);
	l.gap(width - l.cells - right.cells);
	return l.text + right.text + RESET;
}

// no newline after the last row, so a frame as tall as the console never scrolls it
function frame_join(array<string> lines) -> string { return lines.join(`${EOL}\n`) + EOL + ESC + "[J"; }

function present(array<string> lines) -> void { draw(frame_join(lines)); }

// The multi-view carries everything in its boxes; the forward view alone has the panel beside
// it and the newest log lines under it.
int PANEL_W = 20;
int SINGLE_LOG_LINES = 5;

function frame_lines(array<string> view_lines) -> array<string> {
	if (VIEW_LAYOUT != LAYOUT_SINGLE) {
		array<string> lines = [title_bar(CANVAS_W)];
		for (auto row : view_lines) { lines.push(row); }
		return lines;
	}
	array<string> lines = [title_bar(CANVAS_W + PANEL_W)];
	array<Line> panel = build_panel(PANEL_W - 2, view_lines.size());
	string bg = sgr_bg(INK_PANEL) + TEXT_FG;
	for (int y = 0; y < view_lines.size(); ++y) {
		Line side = y < panel.size() ? panel[y] : Line();
		lines.push(view_lines[y] + bg + "  " + side.text + bg + string::repeat(" ", imax(0, PANEL_W - 2 - side.cells)) + RESET);
	}
	int width = CANVAS_W + PANEL_W - 2;
	array<string> log;
	for (int i = G.msgs.size() - 1; i >= 0 && log.size() < SINGLE_LOG_LINES; --i) {
		array<string> wrapped = wrap_text(shown_message(i), width);
		for (int k = wrapped.size() - 1; k >= 0 && log.size() < SINGLE_LOG_LINES; --k) { log.push(wrapped[k]); }
	}
	for (int k = log.size(); k < SINGLE_LOG_LINES; ++k) { log.push(""); }
	for (int k = log.size() - 1; k >= 0; --k) {
		lines.push(bg + " " + log[k] + bg + string::repeat(" ", imax(0, width + 1 - vis_len(log[k]))) + RESET);
	}
	return lines;
}

// ----------------------------------------------------------- screen fit ---
// The console's size when the host can tell; 0 keeps the default 64x20 forward view (headless
// runs and dumps). Declared without initializers, so a hot reload keeps them; a host may set
// them before crawler_main (the runner shim's view, layout and screen flags).
int SCREEN_COLS;
int SCREEN_ROWS;
int VIEW_ROWS_FIXED;        // the forward view's rows when the host asked for a size
int LAYOUT_FIXED;           // 1 the forward view alone, 2 the multi-view, 0 fit
int VIEW_ROWS_MAX = 48;

function set_view_rows(int rows) -> void {
	VIEW_H = rows;
	VIEW_W = rows * 16 / 5;
}

function layout_fits(int mode, int rows) -> bool {
	layout_panes(mode, rows * 16 / 5, rows);
	LAYOUT_KEY = -1;
	if (mode == LAYOUT_SINGLE) { return CANVAS_W + PANEL_W <= SCREEN_COLS && CANVAS_H + 1 + SINGLE_LOG_LINES <= SCREEN_ROWS; }
	return CANVAS_W <= SCREEN_COLS && CANVAS_H + 1 <= SCREEN_ROWS;
}

// the tallest forward view, from VIEW_ROWS_MAX down to min_rows, whose frame fits; 0 if none
function fit_rows(int mode, int min_rows) -> int {
	for (int rows = VIEW_ROWS_MAX; rows >= min_rows; --rows) {
		if (layout_fits(mode, rows)) { return rows; }
	}
	return 0;
}

// the multi-view whenever it fits a forward view of 20 rows, else the forward view alone as
// large as fits (12 rows at the least)
function fit_screen() -> void {
	if (SCREEN_COLS <= 0) {
		if (VIEW_ROWS_FIXED > 0) { set_view_rows(VIEW_ROWS_FIXED); }
		if (LAYOUT_FIXED > 0) { VIEW_LAYOUT = LAYOUT_FIXED == 1 ? LAYOUT_SINGLE : LAYOUT_MULTI; }
		return;
	}
	if (VIEW_ROWS_FIXED > 0) {
		set_view_rows(VIEW_ROWS_FIXED);
		VIEW_LAYOUT = LAYOUT_FIXED == 1 || !layout_fits(LAYOUT_MULTI, VIEW_ROWS_FIXED) ? LAYOUT_SINGLE : LAYOUT_MULTI;
		return;
	}
	int rows = LAYOUT_FIXED == 1 ? 0 : fit_rows(LAYOUT_MULTI, 20);
	VIEW_LAYOUT = rows > 0 ? LAYOUT_MULTI : LAYOUT_SINGLE;
	if (rows == 0) { rows = imax(12, fit_rows(LAYOUT_SINGLE, 12)); }
	set_view_rows(rows);
}

// the colour depth and glyphs, and a view sized to the console
function setup_screen() -> void {
	term_setup(HOST_TRUECOLOR, HOST_ASCII);
	VPAL_KEYS = {};
	VPAL_FG = [];
	VPAL_BG = [];
	VPAL_RGB = [];
	VPAL_MEAN = {};
	view_cache_invalidate();
	CHROME_FLOOR = -1;
	if (SCREEN_COLS == 0 && HOST_INTERACTIVE) {
		SCREEN_COLS = term_cols();
		SCREEN_ROWS = term_rows();
	}
	fit_screen();
}

// a console resized mid-run re-fits the view
function follow_console_size() -> void {
	if (!HOST_INTERACTIVE) { return; }
	int cols = term_cols();
	int rows = term_rows();
	if (cols == SCREEN_COLS && rows == SCREEN_ROWS) { return; }
	SCREEN_COLS = cols;
	SCREEN_ROWS = rows;
	fit_screen();
	draw(`${ESC}[2J`);
}

// v: the forward view alone <-> the multi-view
function cycle_layout() -> void {
	VIEW_LAYOUT = VIEW_LAYOUT == LAYOUT_SINGLE ? LAYOUT_MULTI : LAYOUT_SINGLE;
	LAYOUT_FIXED = VIEW_LAYOUT == LAYOUT_SINGLE ? 1 : 2;
	if (SCREEN_COLS > 0 && VIEW_ROWS_FIXED == 0) {
		set_view_rows(imax(12, fit_rows(VIEW_LAYOUT, 12)));
		draw(`${ESC}[2J`);
	}
}

// --------------------------------------------------------- drawing turns ---
function camera_x() -> float { return G.player.x + 0.5; }
function camera_y() -> float { return G.player.y + 0.5; }

// m shows the overhead map in the forward pane's place; the other panes and boxes stay live
function game_view_lines() -> array<string> {
	if (LAYOUT_KEY != layout_key()) { build_layout(); }
	if (G.show_map) {
		OVERLAY_ON = true;
		OVERLAY_LABEL = "MAP | NORTH UP";
		OVERLAY_ROWS = map_page(PANES[0].w, PANES[0].h);
	}
	fill_boxes();
	return render_view(camera_x(), camera_y(), FACE_DX[G.facing], FACE_DY[G.facing]);
}

function render_game() -> void {
	frame_mark();
	follow_console_size();
	present(frame_lines(game_view_lines()));
	OVERLAY_ON = false;
}

function animating() -> bool { return !G.smoke && !HOST_SCRIPTED && !G.show_map; }

// a 90-degree snap turn swept through 30 and 60 degrees, so it reads as motion
function animate_turn(int old_facing, int rot) -> void {
	if (!animating()) { return; }
	fill_boxes();
	float dx0 = FACE_DX[old_facing];
	float dy0 = FACE_DY[old_facing];
	for (float a : [0.5236, 1.0472]) {
		float ca = math::cos(rot * a);
		float sa = math::sin(rot * a);
		present(frame_lines(render_view(camera_x(), camera_y(), dx0 * ca - dy0 * sa, dx0 * sa + dy0 * ca)));
		sleep(0.022);
	}
}

// a step glides the camera a third and two thirds of the way, the hands dipping as you go
function animate_step(int from_x, int from_y) -> void {
	if (!animating()) { return; }
	fill_boxes();
	for (int k = 1; k <= 2; ++k) {
		float t = k / 3.0;
		HAND_BOB = k;
		float x = from_x + 0.5 + (G.player.x - from_x) * t;
		float y = from_y + 0.5 + (G.player.y - from_y) * t;
		present(frame_lines(render_view(x, y, FACE_DX[G.facing], FACE_DY[G.facing])));
		sleep(0.018);
	}
	HAND_BOB = 0;
}

// ------------------------------------------------------------- the maps ---
// The explored floor: remembered stone dim, what you see lit, monsters and loot where you can
// see them, and your arrow. The m page and the MAP box are windows onto it.
class MapMark { string glyph; int ink; }

function map_marks() -> map<int, MapMark> {
	map<int, MapMark> marks;
	for (auto it : G.ground) { marks[idx(it.x, it.y)] = MapMark{it.glyph, it.ink}; }
	for (auto m : G.monsters) {
		if (!m.alive) { continue; }
		marks[idx(m.x, m.y)] = m.disguised ? MapMark{"?", INK_COIN} : MapMark{m.glyph, m.ink};
	}
	return marks;
}

// w x h cells of the map from tile (x0, y0); off the map is blank
function map_window(int x0, int y0, int w, int h) -> array<Line> {
	map<int, MapMark> marks = map_marks();
	Theme theme = theme_of(G.floor);
	int wall_lit = mix_rgb(theme.wall, 0xFFFFFF, 0.15);
	int wall_seen = mix_rgb(theme.wall, INK_PANEL, 0.68);
	int floor_lit = mix_rgb(theme.floor, 0xFFFFFF, 0.2);
	int floor_seen = mix_rgb(theme.floor, INK_PANEL, 0.72);
	string wall = ASCII_ONLY ? "#" : G_FULL;
	array<Line> rows;
	for (int y = y0; y < y0 + h; ++y) {
		Line row;
		for (int x = x0; x < x0 + w; ++x) {
			if (!in_bounds(x, y)) {
				row.gap(1);
				continue;
			}
			int i = idx(x, y);
			int t = G.tiles[i];
			bool lit = G.visible[i];
			if (x == G.player.x && y == G.player.y) {
				row.glyphs(INK_TITLE, G_ARROWS[G.facing], 1);
			} else if (lit && marks.has(i)) {
				row.put(marks[i].ink, marks[i].glyph);
			} else if (!lit && !G.seen[i]) {
				row.gap(1);
			} else if (t == TILE_WALL) {
				row.glyphs(lit ? wall_lit : wall_seen, wall, 1);
			} else if (t == TILE_STAIRS) {
				row.put(lit ? INK_INFO : INK_TEAL, ">");
			} else if (t == TILE_STATUE) {
				row.put(lit ? INK_BLESS : INK_DIM, "&");
			} else if (t == TILE_MURAL) {
				row.put(lit ? INK_MAGIC : INK_DIM, "%");
			} else {
				row.glyphs(lit ? floor_lit : floor_seen, G_DOT, 1);
			}
		}
		rows.push(row);
	}
	return rows;
}

// the m page: the whole floor centred in the forward pane
function map_page(int w, int h) -> array<string> {
	array<Line> rows = map_window(0 - imax(0, w - MAP_W) / 2, 0 - imax(0, h - MAP_H) / 2, w, h);
	return page_rows(rows, w, h, false);
}

// pane-sized rows on the panel background, each exactly w cells
// A page's block of lines is centred both ways; the map runs from the corner.
function page_rows(array<Line> lines, int w, int h, bool centered_block) -> array<string> {
	string bg = sgr_bg(INK_PANEL) + TEXT_FG;
	int top = centered_block ? imax(0, (h - lines.size()) / 2) : 0;
	int widest = 0;
	for (auto l : lines) { widest = imax(widest, l.cells); }
	int left = centered_block ? imax(0, (w - widest) / 2) : 0;
	array<string> rows;
	for (int r = 0; r < h; ++r) {
		int k = r - top;
		if (k < 0 || k >= lines.size()) {
			rows.push(bg + string::repeat(" ", w) + RESET);
			continue;
		}
		Line l = lines[k];
		int room = w - left;
		string text = l.cells > room ? clip_text(l.text, room) : l.text;
		rows.push(bg + string::repeat(" ", left) + text + bg + string::repeat(" ", imax(0, room - l.cells)) + RESET);
	}
	return rows;
}

// ------------------------------------------------------------- the boxes ---
function log_box(int w, int h) -> array<Line> {
	array<string> rows;
	for (int i = G.msgs.size() - 1; i >= 0 && rows.size() < h; --i) {
		array<string> wrapped = wrap_text(shown_message(i), w);
		for (int k = wrapped.size() - 1; k >= 0 && rows.size() < h; --k) { rows.push(wrapped[k]); }
	}
	array<Line> out;
	for (int k = rows.size(); k < h; ++k) { out.push(Line()); }
	for (int k = rows.size() - 1; k >= 0; --k) { out.push(painted_line(rows[k])); }
	return out;
}

// the box draws the map in half blocks, two tiles to a cell, so tiles come out square
function map_box(int w, int h) -> array<Line> {
	if (ASCII_ONLY) { return map_window(G.player.x - w / 2, G.player.y - h / 2, w, h); }
	return map_pixels(G.player.x - w / 2, G.player.y - h, w, h);
}

// a tile's colour on the pixel map: you, then what you can see on it, then the stone
function map_pixel_rgb(int x, int y, map<int, MapMark>& marks, Theme theme) -> int {
	if (!in_bounds(x, y)) { return INK_PANEL; }
	int i = idx(x, y);
	if (x == G.player.x && y == G.player.y) { return INK_TITLE; }
	bool lit = G.visible[i];
	if (!lit && !G.seen[i]) { return INK_PANEL; }
	if (lit && marks.has(i)) { return marks[i].ink; }
	int t = G.tiles[i];
	if (t == TILE_STAIRS) { return lit ? INK_INFO : INK_TEAL; }
	if (t == TILE_STATUE) { return INK_BLESS; }
	if (t == TILE_MURAL) { return INK_MAGIC; }
	int base = t == TILE_WALL ? theme.wall : theme.floor;
	return mix_rgb(base, INK_PANEL, t == TILE_WALL ? (lit ? 0.2 : 0.62) : (lit ? 0.45 : 0.78));
}

function map_pixels(int x0, int y0, int w, int h) -> array<Line> {
	map<int, MapMark> marks = map_marks();
	Theme theme = theme_of(G.floor);
	array<Line> rows;
	for (int r = 0; r < h; ++r) {
		Line row;
		int fg = -1;
		int bg = -1;
		for (int x = x0; x < x0 + w; ++x) {
			int top = map_pixel_rgb(x, y0 + r * 2, marks, theme);
			int bottom = map_pixel_rgb(x, y0 + r * 2 + 1, marks, theme);
			if (top != fg) { row.text += sgr_fg(top); fg = top; }
			if (bottom != bg) { row.text += sgr_bg(bottom); bg = bottom; }
			row.text += G_HALF;
			row.cells += 1;
		}
		row.text += sgr_bg(INK_PANEL);
		rows.push(row);
	}
	return rows;
}

function centered(Line l, int w) -> Line {
	Line out;
	out.gap((w - l.cells) / 2);
	out.text += l.text;
	out.cells += l.cells;
	return out;
}

// the rose with your heading lit, then the heading and the depth gauge
function compass_box(int w, int h) -> array<Line> {
	int glow_ink = theme_of(G.floor).glow;
	array<Line> block;
	block.push(compass_line(" ", 0, " "));
	block.push(compass_arm(0));
	Line middle;
	middle.put(G.facing == 3 ? glow_ink : INK_DIM, "W ");
	middle.glyphs(G.facing == 3 ? glow_ink : INK_FAINT, G_HLINE, 2);
	middle.glyphs(INK_TITLE, G_CROSS, 1);
	middle.glyphs(G.facing == 1 ? glow_ink : INK_FAINT, G_HLINE, 2);
	middle.put(G.facing == 1 ? glow_ink : INK_DIM, " E");
	block.push(middle);
	block.push(compass_arm(2));
	block.push(compass_line(" ", 2, " "));
	block.push(Line());
	block.push(line_of(glow_ink, FACE_NAME[G.facing]));
	Line pips;
	for (int f = 1; f <= 10; ++f) { pips.glyphs(f < G.floor ? INK_DIM : (f == G.floor ? glow_ink : INK_FAINT), f <= G.floor ? G_FULL : G_DOT, 1); }
	block.push(pips);
	block.push(line_of(INK_DIM, `floor ${G.floor}/10`));
	array<Line> out;
	for (int k = 0; k < (h - block.size()) / 2; ++k) { out.push(Line()); }
	for (auto l : block) { out.push(centered(l, w)); }
	return out;
}

function compass_line(string left, int face, string right) -> Line {
	Line l;
	l.put(INK_FAINT, left);
	l.put(G.facing == face ? theme_of(G.floor).glow : INK_DIM, FACE_LETTER[face]);
	l.put(INK_FAINT, right);
	return l;
}

function compass_arm(int face) -> Line {
	Line l;
	l.glyphs(G.facing == face ? theme_of(G.floor).glow : INK_FAINT, G_VLINE, 1);
	return l;
}

// label and value pairs on one line: the labels faint, the values bright
function stat_line(array<string> labels, array<int> values, int w) -> Line {
	Line l;
	int column = w / labels.size();
	for (int i = 0; i < labels.size(); ++i) {
		int start = l.cells;
		l.put(INK_DIM, labels[i]);
		l.put(INK_BRIGHT, ` ${values[i]}`);
		if (i + 1 < labels.size()) { l.gap(column - (l.cells - start)); }
	}
	return l;
}

function meter_line(string label, int cur, int cap, int ink, int w) -> Line {
	Line l;
	l.put(INK_DIM, label);
	l.gap(3 - label.size());
	string figures = `${cur}/${cap}`;
	Line bar = meter(cur, cap, imax(4, w - 4 - figures.size() - 1), ink);
	l.text += bar.text;
	l.cells += bar.cells;
	l.gap(w - l.cells - figures.size());
	l.put(INK_BRIGHT, figures);
	return l;
}

function gear_line(string slot, var item, int w) -> Line {
	Line l;
	l.put(INK_FAINT, slot);
	l.gap(4 - slot.size());
	if (item == null) {
		l.put(INK_FAINT, "-");
		return l;
	}
	string name = item.name;
	string stats = item_stats_label(item);
	if (l.cells + name.size() > w) { name = name.substr(0, imax(1, w - l.cells)); }
	l.put(item.ink, name);
	if (l.cells + stats.size() <= w) { l.put(INK_DIM, stats); }
	return l;
}

function effects_line(int w) -> Line {
	Line l;
	for (auto fx : G.player.effects) {
		string chip = `${fx.kind} ${fx.turns}`;
		if (l.cells + chip.size() + 2 > w) { break; }
		if (l.cells > 0) { l.glyphs(INK_FAINT, G_DOT, 1); l.gap(1); }
		l.put(fx.kind == "burn" || fx.kind == "wither" ? INK_BAD : INK_MAGIC, chip);
		l.gap(1);
	}
	return l;
}

function hero_box(int w, int h) -> array<Line> {
	Player p = G.player;
	Profession calling = profession_of(p.profession_id);
	Species species = species_of(p.species_id);
	array<Line> out;
	Line top = line_of(INK_BRIGHT, p.name);
	string level = `${p.level}`;
	top.gap(w - top.cells - 6 - level.size());
	top.put(INK_FAINT, "level ");
	top.put(INK_TITLE, level);
	out.push(top);
	out.push(line_of(INK_DIM, `${species.name} ${calling.name}`));
	out.push(meter_line("HP", p.hp, p.total_max_hp(), health_ink(p.hp, p.total_max_hp()), w));
	out.push(meter_line("FO", p.focus, p.total_max_focus(), INK_INFO, w));
	out.push(meter_line("XP", p.xp, p.xp_next(), INK_GOLD, w));
	if (G.last_dmg > 0) {
		Line hurt;
		hurt.put(INK_BAD, `-${G.last_dmg} HP`);
		hurt.put(INK_DIM, `  ${G.last_dmg_src}`);
		out.push(hurt);
	} else {
		out.push(effects_line(w));
	}
	out.push(stat_line(["ATK", "DEF", "EVA", "CRT"], [p.total_atk(), p.total_def(), p.total_eva(), p.total_crit()], w));
	Line purse;
	purse.put(INK_COIN, `${p.gold}`);
	purse.put(INK_DIM, " gold   ");
	purse.put(INK_BRIGHT, `${p.kills}`);
	purse.put(INK_DIM, p.kills == 1 ? " kill" : " kills");
	out.push(purse);
	Line skill;
	skill.put(INK_TEAL, p.skill_name);
	skill.put(INK_FAINT, `  z, ${p.skill_cost} focus`);
	out.push(skill);
	if (G.last_dmg > 0 && G.player.effects.size() > 0) { out.push(effects_line(w)); }
	out.push(Line());
	out.push(gear_line("WPN", p.weapon, w));
	out.push(gear_line("ARM", p.armor, w));
	out.push(gear_line("TRK", p.trinket, w));
	// a tall box also spells out the kin's trait and the class skill, each only if it fits whole
	array<string> notes = [paint(INK_TEAL, `${species.trait_name}: `) + paint(INK_DIM, species.trait_blurb),
		paint(INK_TEAL, `${calling.skill_name}: `) + paint(INK_DIM, calling.skill_blurb)];
	for (auto note : notes) {
		array<string> wrapped = wrap_text(note, w);
		if (out.size() + 1 + wrapped.size() > h) { break; }
		out.push(Line());
		for (auto row : wrapped) { out.push(painted_line(row)); }
	}
	return out;
}

// "LEFT 3": the pane a tile shows up in, and how many steps away
function where_label(int x, int y) -> string {
	int dx = x - G.player.x;
	int dy = y - G.player.y;
	if (dx == 0 && dy == 0) { return "HERE"; }
	int ahead = dx * FACE_DX[G.facing] + dy * FACE_DY[G.facing];
	int right = dx * FACE_DX[(G.facing + 1) % 4] + dy * FACE_DY[(G.facing + 1) % 4];
	int pane = iabs(ahead) >= iabs(right) ? (ahead > 0 ? 0 : 2) : (right > 0 ? 1 : 3);
	return `${PANE_NAMES[pane]} ${cheb(x, y, G.player.x, G.player.y)}`;
}

class Sighting { string glyph; string name; string note; int ink; int x; int y; }

// nearest first: the glyph and name in their ink, a note, and where on the right
function sighting_rows(array<Sighting> seen, int w, int h, string none) -> array<Line> {
	array<int> order;
	array<int> dist;
	for (int i = 0; i < seen.size(); ++i) {
		int d = cheb(seen[i].x, seen[i].y, G.player.x, G.player.y);
		int j = order.size();
		order.push(i);
		dist.push(d);
		while (j > 0 && dist[j - 1] > d) {
			order[j] = order[j - 1];
			dist[j] = dist[j - 1];
			j -= 1;
		}
		order[j] = i;
		dist[j] = d;
	}
	array<Line> out;
	if (order.size() == 0) { out.push(line_of(INK_FAINT, none)); }
	for (int k = 0; k < order.size() && k < h; ++k) {
		Sighting s = seen[order[k]];
		string where = where_label(s.x, s.y);
		Line l;
		l.put(s.ink, `${s.glyph} `);
		int room = w - 3 - where.size();
		string name = s.name.size() > room ? s.name.substr(0, imax(0, room)) : s.name;
		l.put(INK_TEXT, name);
		if (s.note != "" && l.cells + s.note.size() + 2 <= room + 2) { l.put(INK_DIM, `  ${s.note}`); }
		l.gap(w - l.cells - where.size());
		l.put(INK_INFO, where);
		out.push(l);
	}
	return out;
}

function sight_box(int w, int h) -> array<Line> {
	array<Sighting> seen;
	for (auto m : G.monsters) {
		if (!m.alive || m.disguised || !is_visible(m.x, m.y)) { continue; }
		seen.push(Sighting{m.glyph, m.name, m.hp < m.max_hp ? health_word(m.hp, m.max_hp) : "", m.ink, m.x, m.y});
	}
	return sighting_rows(seen, w, h, "nothing but the dark");
}

// a mimic is loot until it very much is not
function loot_box(int w, int h) -> array<Line> {
	array<Sighting> seen;
	for (auto it : G.ground) {
		if (is_visible(it.x, it.y)) { seen.push(Sighting{it.glyph, item_label(it), "", it.ink, it.x, it.y}); }
	}
	for (auto m : G.monsters) {
		if (m.alive && m.disguised && is_visible(m.x, m.y)) { seen.push(Sighting{"?", "a chest", "", INK_COIN, m.x, m.y}); }
	}
	return sighting_rows(seen, w, h, "no loot in sight");
}

function box_lines(int id, int w, int h) -> array<Line> {
	if (id == BOX_LOG) { return log_box(w, h); }
	if (id == BOX_MAP) { return map_box(w, h); }
	if (id == BOX_COMPASS) { return compass_box(w, h); }
	if (id == BOX_HERO) { return hero_box(w, h); }
	if (id == BOX_SIGHT) { return sight_box(w, h); }
	return loot_box(w, h);
}

// text boxes keep a blank cell inside each side; the map runs edge to edge
function fill_boxes() -> void {
	string bg = sgr_bg(INK_PANEL) + TEXT_FG;
	for (auto box : BOXES) {
		bool text = box.id != BOX_MAP;
		int w = text ? box.w - 2 : box.w;
		array<Line> lines = box_lines(box.id, w, box.h);
		string margin = text ? " " : "";
		for (int r = 0; r < box.h; ++r) {
			Line l = r < lines.size() ? lines[r] : Line();
			string body = l.cells > w ? clip_text(l.text, w) : l.text;
			BOX_ROWS[box.first_row + r] = bg + margin + body + bg + string::repeat(" ", imax(0, w - l.cells)) + margin + RESET;
		}
	}
}

// --------------------------------------------- the forward view alone's panel ---
function build_panel(int w, int h) -> array<Line> {
	Player p = G.player;
	array<Line> out;
	out.push(line_of(INK_BRIGHT, p.name));
	out.push(line_of(INK_DIM, `${species_of(p.species_id).name} lv ${p.level}`));
	out.push(meter_line("HP", p.hp, p.total_max_hp(), health_ink(p.hp, p.total_max_hp()), w));
	out.push(meter_line("FO", p.focus, p.total_max_focus(), INK_INFO, w));
	out.push(meter_line("XP", p.xp, p.xp_next(), INK_GOLD, w));
	out.push(G.last_dmg > 0 ? line_of(INK_BAD, `-${G.last_dmg} ${G.last_dmg_src}`) : effects_line(w));
	out.push(stat_line(["ATK", "DEF"], [p.total_atk(), p.total_def()], w));
	out.push(stat_line(["EVA", "CRT"], [p.total_eva(), p.total_crit()], w));
	Line purse;
	purse.put(INK_COIN, `${p.gold}`);
	purse.put(INK_DIM, " gold");
	out.push(purse);
	out.push(Line());
	for (auto l : compass_box(w, 9)) {
		if (out.size() < h - 4) { out.push(l); }
	}
	while (out.size() < h - 3) { out.push(Line()); }
	out.push(gear_line("WPN", p.weapon, w));
	out.push(gear_line("ARM", p.armor, w));
	out.push(gear_line("TRK", p.trinket, w));
	return out;
}

// ------------------------------------------------------ the forward pane's pages ---
// A page replaces the forward view with text until a key; the rest of the cockpit stays live.
function show_page(string label, array<Line> lines) -> void {
	if (LAYOUT_KEY != layout_key()) { build_layout(); }
	OVERLAY_ON = true;
	OVERLAY_LABEL = label;
	OVERLAY_ROWS = page_rows(lines, PANES[0].w, PANES[0].h, true);
	fill_boxes();
	present(frame_lines(render_view(camera_x(), camera_y(), FACE_DX[G.facing], FACE_DY[G.facing])));
	OVERLAY_ON = false;
}

function heading(string text) -> Line {
	Line l;
	l.gap(2);
	l.put(theme_of(G.floor).glow, text);
	return l;
}

function key_row(string keys, string what, int key_width) -> Line {
	Line l;
	l.gap(2);
	l.put(INK_TITLE, keys);
	l.gap(key_width - keys.size());
	l.put(INK_TEXT, what);
	return l;
}

// two key columns side by side when the pane is wide enough
function key_columns(array<Line> left, array<Line> right, int w) -> array<Line> {
	array<Line> out;
	int half = w / 2;
	if (half < 30) {
		for (auto l : left) { out.push(l); }
		for (auto l : right) { out.push(l); }
		return out;
	}
	for (int i = 0; i < imax(left.size(), right.size()); ++i) {
		Line l = i < left.size() ? left[i] : Line();
		l.gap(half - l.cells);
		if (i < right.size()) {
			l.text += right[i].text;
			l.cells += right[i].cells;
		}
		out.push(l);
	}
	return out;
}

function help_screen() -> void {
	int w = PANES.size() > 0 ? PANES[0].w : VIEW_W;
	array<Line> moving = [heading("MOVING"),
		key_row("w  up", "step forward", 9), key_row("s  down", "step back", 9),
		key_row("a  d", "turn left, right", 9), key_row("q  e", "sidestep", 9),
		key_row(".", "wait a turn", 9), key_row("m", "map: keys walk", 9),
		key_row("", "by compass", 9), key_row("v", "one view or four", 9)];
	array<Line> acting = [heading("ACTING"),
		key_row("w", "into a foe: attack", 9), key_row("z", "class skill, ahead", 9),
		key_row("g", "pick up", 9), key_row("i", "pack", 9),
		key_row("p  E", "quaff, equip best", 9), key_row(">", "descend the stairs", 9),
		key_row("c", "character", 9), key_row("esc", "pause: save, quit", 9)];
	array<Line> lines = key_columns(moving, acting, w);
	lines.push(Line());
	if (HOST_DEV) { lines.push(key_row("R", "hot-reload the scripts", 9)); }
	Line lore;
	lore.gap(2);
	lore.put(INK_DIM, "Ten floors down, Vexadrach hoards the stolen color.");
	lines.push(lore);
	Line lore2;
	lore2.gap(2);
	lore2.put(INK_DIM, "JAI's statues (&) bless you; DEVACOR's murals (%) mend.");
	lines.push(lore2);
	lines.push(Line());
	lines.push(line_of(INK_FAINT, "  any key to return"));
	show_page("HELP", lines);
	read_key();
}

function character_screen() -> void {
	Player p = G.player;
	Species species = species_of(p.species_id);
	Profession calling = profession_of(p.profession_id);
	int w = PANES[0].w - 4;
	array<Line> lines;
	Line top;
	top.gap(2);
	top.put(INK_BRIGHT, p.name);
	top.put(INK_DIM, `  level ${p.level} ${species.name} ${calling.name}`);
	lines.push(top);
	lines.push(Line());
	for (auto row : wrap_text(paint(INK_DIM, species.blurb), w)) { lines.push(painted_line(`  ${row}`)); }
	for (auto row : wrap_text(paint(INK_TEAL, `${species.trait_name}: `) + paint(INK_TEXT, species.trait_blurb), w)) { lines.push(painted_line(`  ${row}`)); }
	lines.push(Line());
	for (auto row : wrap_text(paint(INK_DIM, calling.blurb), w)) { lines.push(painted_line(`  ${row}`)); }
	for (auto row : wrap_text(paint(INK_TEAL, `${calling.skill_name} (${calling.skill_cost} focus): `) + paint(INK_TEXT, calling.skill_blurb), w)) { lines.push(painted_line(`  ${row}`)); }
	lines.push(Line());
	Line stats;
	stats.gap(2);
	Line inner = stat_line(["HP", "FOCUS", "ATK", "DEF", "EVA", "CRIT"], [p.total_max_hp(), p.total_max_focus(), p.total_atk(), p.total_def(), p.total_eva(), p.total_crit()], w);
	stats.text += inner.text;
	stats.cells += inner.cells;
	lines.push(stats);
	Line tally;
	tally.gap(2);
	tally.put(INK_DIM, `${p.kills} kills, ${p.gold} gold, ${G.turn} turns, floor ${G.floor}, facing ${FACE_NAME[G.facing]}`);
	lines.push(tally);
	lines.push(Line());
	lines.push(line_of(INK_FAINT, "  any key to return"));
	show_page("CHARACTER", lines);
	read_key();
}

string PACK_LETTERS = "abcdefghijklmnopqrstuv";

// true when using, equipping or dropping spent a turn
function inventory_screen() -> bool {
	while (true) {
		Player p = G.player;
		int w = PANES[0].w;
		array<Line> lines;
		Line top;
		top.gap(2);
		top.put(INK_BRIGHT, `${p.inventory.size()}`);
		top.put(INK_DIM, ` of ${PACK_SIZE} carried    a letter to look closer, esc to close`);
		lines.push(top);
		lines.push(Line());
		array<Line> items;
		for (int i = 0; i < p.inventory.size(); ++i) {
			Item it = p.inventory[i];
			Line l;
			l.gap(2);
			l.put(INK_TITLE, PACK_LETTERS.substr(i, 1));
			l.put(INK_FAINT, " ");
			l.put(it.ink, `${it.glyph} ${item_label(it)}`);
			items.push(l);
		}
		if (items.size() == 0) { items.push(line_of(INK_FAINT, "  (lint, mostly)")); }
		int rows = imax(1, PANES[0].h - 4);
		if (items.size() > rows) {
			array<Line> left;
			array<Line> right;
			for (int i = 0; i < items.size(); ++i) {
				if (i < (items.size() + 1) / 2) { left.push(items[i]); } else { right.push(items[i]); }
			}
			items = key_columns(left, right, w);
		}
		for (auto l : items) { lines.push(l); }
		show_page("PACK", lines);
		string key = read_key();
		if (key == "esc" || key == "i" || key == "Q") { return false; }
		int pick_index = PACK_LETTERS.find(key);
		if (key.size() != 1 || pick_index < 0 || pick_index >= p.inventory.size()) { continue; }
		Item chosen = p.inventory[pick_index];
		string verb = chosen.kind == "potion" ? "quaff" : (chosen.kind == "scroll" ? "read" : "equip");
		array<Line> detail;
		Line name;
		name.gap(2);
		name.put(chosen.ink, `${chosen.glyph} ${item_label(chosen)}`);
		name.put(INK_DIM, item_stats_label(chosen));
		detail.push(name);
		if (chosen.is_gear()) { detail.push(line_of(rarity_ink(chosen.rarity), `  ${chosen.rarity}`)); }
		detail.push(Line());
		detail.push(key_row("e", verb, 4));
		detail.push(key_row("d", "drop", 4));
		detail.push(key_row("esc", "back", 4));
		show_page("PACK", detail);
		string act = read_key();
		if (act == "e") { equip_item(G.player, pick_index); return true; }
		if (act == "d") { drop_item(G.player, pick_index); return true; }
	}
	return false;
}

// the boon a new level brings; returns the key pressed
function level_up_screen() -> string {
	array<Line> lines;
	Line top;
	top.gap(2);
	top.put(INK_TITLE, `LEVEL ${G.player.level}!`);
	top.put(INK_DIM, "  pick a boon");
	lines.push(top);
	lines.push(Line());
	lines.push(key_row("1", "Vigor   +4 max HP, and heal 4", 4));
	lines.push(key_row("2", "Might   +1 attack", 4));
	lines.push(key_row("3", "Guard   +1 defense", 4));
	lines.push(key_row("4", "Wits    +3 max focus", 4));
	show_page("LEVEL UP", lines);
	return read_key();
}

// esc pauses; its keys are lowercase, so they work on a keyboard that reports no shift.
// Returns the command key to run ("" for none).
function pause_screen() -> string {
	array<Line> lines = [key_row("esc", "back to the Well", 6), key_row("?  h", "the keys", 6),
		key_row("s", "save and quit", 6), key_row("q", "quit without saving", 6)];
	if (HOST_DEV) { lines.push(key_row("r", "reload the scripts", 6)); }
	show_page("PAUSED", lines);
	string key = read_key();
	if (key == "s" || key == "S") { return "S"; }
	if (key == "r" || key == "R") { return "R"; }
	if (key == "?" || key == "h") { return "?"; }
	if (key == "q" || key == "Q") {
		G.over = true;
		G.quit = true;
	}
	return "";
}

// Q asks once more; a second Q (or a scripted run out of keys) quits without saving
function confirm_quit() -> bool {
	array<Line> lines = [line_of(INK_BAD, "  Leave the Well without saving?"), Line(),
		key_row("Q", "quit, and the run is lost", 4), key_row("S", "save and quit instead", 4),
		key_row("esc", "keep going", 4)];
	show_page("QUIT", lines);
	string key = read_key();
	if (key == "S") {
		save_game();
		G.saved = true;
		return true;
	}
	return key == "Q";
}
