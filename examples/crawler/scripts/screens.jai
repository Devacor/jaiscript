// The full-screen pages: the title over a torchlit shaft that falls toward a green glow, the
// kin, profession and name pickers, the death page in the run's last view washed in blood, and
// the victory page in rainbow fire. A page is a pixel canvas the size of the console with
// framed text laid over it.

int PAGE_W = 0;             // cells; the page is PAGE_W x PAGE_H*2 pixels
int PAGE_H = 0;
array<int> PAGE_PIX = [];
class Overlay { int x; int y; string text; int cells; }
array<Overlay> OVERLAYS = [];

function page_open(int w, int h) -> void {
	PAGE_W = w;
	PAGE_H = h;
	PAGE_PIX = [];
	int black = pal(INK_BACK);
	for (int i = 0; i < w * h * 2; ++i) { PAGE_PIX.push(black); }
	OVERLAYS = [];
}

// the console size, or 100x30 for scripted runs
function page_size_cols() -> int { return SCREEN_COLS > 0 ? SCREEN_COLS : 100; }
function page_size_rows() -> int { return SCREEN_ROWS > 0 ? SCREEN_ROWS : 30; }

function page_text(int x, int y, Line l) -> void {
	if (y < 0 || y >= PAGE_H || x < 0) { return; }
	OVERLAYS.push(Overlay{x, y, l.text, l.cells});
}

// a framed text box in the frame and panel colours; the lines sit one cell in from each side
function page_box(int x, int y, int w, string label, array<Line> lines, int frame_rgb, int label_rgb) -> void {
	string bg = sgr_bg(INK_PANEL);
	Line top;
	top.text = bg;
	top.glyphs(frame_rgb, G_TL, 1);
	top.glyphs(frame_rgb, G_HLINE, 1);
	if (label != "") {
		top.put(label_rgb, ` ${label} `);
	}
	top.glyphs(frame_rgb, G_HLINE, w - 1 - top.cells);
	top.glyphs(frame_rgb, G_TR, 1);
	page_text(x, y, top);
	for (int i = 0; i < lines.size(); ++i) {
		Line row;
		row.text = bg;
		row.glyphs(frame_rgb, G_VLINE, 1);
		row.gap(1);
		Line l = lines[i];
		row.text += l.cells > w - 4 ? clip_text(l.text, w - 4) : l.text;
		row.cells += imin(l.cells, w - 4);
		row.text += bg;
		row.gap(w - 1 - row.cells);
		row.glyphs(frame_rgb, G_VLINE, 1);
		page_text(x, y + 1 + i, row);
	}
	Line bottom;
	bottom.text = bg;
	bottom.glyphs(frame_rgb, G_BL, 1);
	bottom.glyphs(frame_rgb, G_HLINE, w - 2);
	bottom.glyphs(frame_rgb, G_BR, 1);
	page_text(x, y + 1 + lines.size(), bottom);
}

function page_present() -> void {
	array<string> rows;
	for (int cy = 0; cy < PAGE_H; ++cy) { rows.push(page_row(cy)); }
	present(rows);
}

function page_row(int cy) -> string {
	array<Overlay> on_row;
	for (auto o : OVERLAYS) {
		if (o.y != cy) { continue; }
		int j = on_row.size();
		on_row.push(o);
		while (j > 0 && on_row[j - 1].x > o.x) {
			on_row[j] = on_row[j - 1];
			j -= 1;
		}
		on_row[j] = o;
	}
	string row = "";
	int x = 0;
	for (auto o : on_row) {
		if (o.x < x) { continue; }
		row += page_pixels(cy, x, imin(o.x, PAGE_W));
		row += o.text + RESET;
		x = o.x + o.cells;
	}
	return row + page_pixels(cy, x, PAGE_W);
}

function page_pixels(int cy, int x0, int x1) -> string {
	if (x0 >= x1) { return ""; }
	string row = "";
	int ti = cy * 2 * PAGE_W;
	int bi = ti + PAGE_W;
	int fg = -1;
	int bg = -1;
	for (int x = x0; x < x1; ++x) {
		int top = PAGE_PIX[ti + x];
		int bot = PAGE_PIX[bi + x];
		if (ASCII_ONLY) {
			int c = pal_mean(top, bot);
			if (c != bg) { row += VPAL_BG[c]; bg = c; }
			row += " ";
			continue;
		}
		if (top != fg) { row += VPAL_FG[top]; fg = top; }
		if (bot != bg) { row += VPAL_BG[bot]; bg = bot; }
		row += G_HALF;
	}
	return row + RESET;
}

// darkens the pixels of a rectangle, the plate a logo sits on
function page_dim(int x0, int y0, int x1, int y1, float k) -> void {
	map<int, int> dimmed = {};
	for (int y = imax(0, y0); y < imin(PAGE_H * 2, y1); ++y) {
		for (int x = imax(0, x0); x < imin(PAGE_W, x1); ++x) {
			int c = PAGE_PIX[y * PAGE_W + x];
			if (!dimmed.has(c)) { dimmed[c] = pal(scale_rgb(VPAL_RGB[c], k)); }
			PAGE_PIX[y * PAGE_W + x] = dimmed[c];
		}
	}
}

// ----------------------------------------------------------- the pixel font ---
// 5x7 capitals and digits, one string of 35 cells per glyph.
map<string, string> FONT = {
	"A": ".###.#...##...#######...##...##...#", "B": "####.#...##...#####.#...##...#####.",
	"C": ".###.#...##....#....#....#...#.###.", "D": "####.#...##...##...##...##...#####.",
	"E": "######....#....####.#....#....#####", "F": "######....#....####.#....#....#....",
	"G": ".###.#...##....#.####...##...#.####", "H": "#...##...##...#######...##...##...#",
	"I": "#####..#....#....#....#....#..#####", "J": "..###...#....#....#.#..#.#..#..##..",
	"K": "#...##..#.#.#..##...#.#..#..#.#...#", "L": "#....#....#....#....#....#....#####",
	"M": "#...###.###.#.##.#.##...##...##...#", "N": "#...###..##.#.##..###...##...##...#",
	"O": ".###.#...##...##...##...##...#.###.", "P": "####.#...##...#####.#....#....#....",
	"Q": ".###.#...##...##...##.#.##..#..##.#", "R": "####.#...##...#####.#.#..#..#.#...#",
	"S": ".#####....#.....###.....#....#####.", "T": "#####..#....#....#....#....#....#..",
	"U": "#...##...##...##...##...##...#.###.", "V": "#...##...##...##...##...#.#.#...#..",
	"W": "#...##...##...##.#.##.#.###.###...#", "X": "#...##...#.#.#...#...#.#.#...##...#",
	"Y": "#...##...#.#.#...#....#....#....#..", "Z": "#####....#...#...#...#...#....#####",
	"0": ".###.#...##..###.#.###..##...#.###.", "1": "..#...##....#....#....#....#...###.",
	"2": ".###.#...#....#...#...#...#...#####", "3": "####.....#....#.###.....#....#####.",
	"4": "...#...##..#.#.#..#.#####...#....#.", "5": "######....####.....#....##...#.###.",
	"6": ".###.#....#....####.#...##...#.###.", "7": "#####....#...#...#...#....#....#...",
	"8": ".###.#...##...#.###.#...##...#.###.", "9": ".###.#...##...#.####....#....#.###."
};

// Draws text in the pixel font, each font pixel sx pixels wide and sy tall, its strokes
// shaded from top_rgb to bottom_rgb with a dark drop shadow; centred on cx when cx >= 0.
// Returns the width in pixels.
function font_text(string text, int x, int y, int sx, int sy, int top_rgb, int bottom_rgb, bool shadow) -> int {
	int width = text.size() * 6 * sx - sx;
	if (x < 0) { x = (PAGE_W - width) / 2; }
	array<int> shade;
	for (int r = 0; r < 7 * sy; ++r) { shade.push(pal(mix_rgb(top_rgb, bottom_rgb, r / (7.0 * sy - 1.0)))); }
	int dark = pal(0x050306);
	for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
		int off = pass == 0 ? imax(1, sy / 2) : 0;
		for (int c = 0; c < text.size(); ++c) {
			string ch = text.substr(c, 1);
			if (!FONT.has(ch)) { continue; }
			string glyph = FONT[ch];
			for (int gy = 0; gy < 7; ++gy) {
				for (int gx = 0; gx < 5; ++gx) {
					if (glyph[gy * 5 + gx] != "#") { continue; }
					for (int py = 0; py < sy; ++py) {
						for (int px = 0; px < sx; ++px) {
							int X = x + (c * 6 + gx) * sx + px;
							int Y = y + gy * sy + py + off;
							if (X < 0 || Y < 0 || X >= PAGE_W || Y >= PAGE_H * 2) { continue; }
							PAGE_PIX[Y * PAGE_W + X] = pass == 0 ? dark : shade[gy * sy + py];
						}
					}
				}
			}
		}
	}
	return width;
}

// --------------------------------------------------------------- the shaft ---
// Looking down the Well: brick rings that fall away toward a sickly green glow, lit by your
// torch at the lip. Each pixel's depth and angle are fixed, so a frame only slides the bricks.
array<float> SHAFT_DEPTH = [];
array<float> SHAFT_ANGLE = [];
array<int> SHAFT_RAMP = [];     // 24 light levels each of brick, lit brick, mortar and the lip, then the glow
int SHAFT_LEVELS = 24;

function shaft_build() -> void {
	SHAFT_DEPTH = [];
	SHAFT_ANGLE = [];
	float cx = PAGE_W / 2.0;
	float cy = PAGE_H * 1.2;
	float rim = imin(PAGE_W, PAGE_H * 2) * 0.66;
	for (int y = 0; y < PAGE_H * 2; ++y) {
		for (int x = 0; x < PAGE_W; ++x) {
			float dx = x + 0.5 - cx;
			float dy = (y + 0.5 - cy) * 1.1;
			float r = math::sqrt(dx * dx + dy * dy) + 0.01;
			SHAFT_DEPTH.push(rim / r);
			SHAFT_ANGLE.push(math::atan2(dy, dx) / (2.0 * math::PI) + 0.5);
		}
	}
	SHAFT_RAMP = [];
	for (int tone : [0x45603A, 0x5C7A46, 0x1C1814, 0x6E6A5C]) {
		for (int l = 0; l < SHAFT_LEVELS; ++l) {
			float t = l / (SHAFT_LEVELS - 1.0);
			SHAFT_RAMP.push(pal(mix_rgb(0x030204, mix_rgb(tone, 0xFFB45A, t * t * 0.3), t)));
		}
	}
	for (int l = 0; l < SHAFT_LEVELS; ++l) {
		float t = l / (SHAFT_LEVELS - 1.0);
		SHAFT_RAMP.push(pal(mix_rgb(0x030204, mix_rgb(0x58C83A, 0xF4FFB4, t * t), t)));
	}
}

function shaft_frame(float descent) -> void {
	int n = PAGE_W * PAGE_H * 2;
	int levels = SHAFT_LEVELS;
	int glow = 4 * levels;
	var& depth = SHAFT_DEPTH;
	var& angle = SHAFT_ANGLE;
	var& ramps = SHAFT_RAMP;
	var& pix = PAGE_PIX;
	for (int i = 0; i < n; ++i) {
		float z = depth[i];
		if (z < 0.86) {
			pix[i] = ramps[0];
			continue;
		}
		if (z < 1.0) {
			pix[i] = ramps[3 * levels + 12 + lat(i % PAGE_W / 3, i / PAGE_W / 2, 9) % 4];   // the stone lip
			continue;
		}
		float v = z * 2.6 + descent;
		int row = cast<int>(v);
		float u = angle[i] * 30.0 + (row % 2) * 0.5;
		int brick = cast<int>(u);
		int tone = v - row < 0.16 || u - brick < 0.08 ? 2 : (lat(brick, row, 3) % 4 == 0 ? 1 : 0);
		float torch = 15.0 / (1.0 + 0.3 * (z - 1.0) * (z - 1.0));
		float deep = z > 6.0 ? (z - 6.0) * 1.1 : 0.0;
		int level = cast<int>(torch + (lat(i, row, 5) % 3) * 0.4);
		if (deep > torch) { pix[i] = ramps[glow + iclamp(cast<int>(deep), 0, levels - 1)]; }
		else { pix[i] = ramps[tone * levels + iclamp(level, 0, levels - 1)]; }
	}
}

// ---------------------------------------------------------------- the title ---
// N, C, ? or Q; animates until a key
function title_screen(bool can_continue) -> string {
	page_open(page_size_cols(), page_size_rows());
	shaft_build();
	float descent = 0.0;
	int frame = 0;
	while (true) {
		shaft_frame(descent);
		shaft_life(frame);
		title_overlay(can_continue, frame);
		page_present();
		string key = HOST_INTERACTIVE ? poll_key(0.05) : read_key();
		descent += 0.06;
		frame += 1;
		if (key == "") { continue; }
		if (key == "n" || key == "enter") { return "n"; }
		if (key == "c" && can_continue) { return "c"; }
		if (key == "?" || key == "h") { how_to_play(); continue; }
		if (key == "q" || key == "Q" || key == "esc") { return "q"; }
	}
	return "q";
}

// What lives in the Well: embers rising out of the glow toward you, and pairs of eyes in the
// brickwork that open, watch and blink. Placed by the presentation hash, never the game's Rng.
function shaft_life(int frame) -> void {
	float cx = PAGE_W / 2.0;
	float cy = PAGE_H * 1.2;
	float rim = imin(PAGE_W, PAGE_H * 2) * 0.66;
	array<int> ember = [pal(0xF4FFB4), pal(0xC8F05A), pal(0xFFC850), pal(0xFF7A28), pal(0x8A3014)];
	for (int k = 0; k < 40; ++k) {
		int h = lat(k, 7, 23);
		float a = (h % 628) / 100.0;
		int life = 40 + h % 30;
		float t = ((frame + h % life) % life) / (life * 1.0);
		float r = rim * (0.06 + 1.1 * t * t);
		float wobble = math::sin(t * 9.0 + k) * 0.18;
		int x = cast<int>(cx + math::cos(a + wobble) * r);
		int y = cast<int>(cy + math::sin(a + wobble) * r / 1.1);
		page_plot(x, y, ember[iclamp(cast<int>(t * 5), 0, 4)]);
		if (t > 0.55) { page_plot(x + 1, y, ember[iclamp(cast<int>(t * 5), 0, 4)]); }
	}
	array<int> eye = [pal(0xFF3020), pal(0xFFD040), pal(0x7CFF50)];
	for (int k = 0; k < 7; ++k) {
		int h = lat(k, 3, 61);
		if ((frame / 12 + h) % 9 < 2 || (frame + h) % 37 == 0) { continue; }   // shut, or mid-blink
		float a = (h % 628) / 100.0;
		float z = 1.6 + (h / 628 % 30) / 10.0;
		int x = cast<int>(cx + math::cos(a) * rim / z);
		int y = cast<int>(cy + math::sin(a) * rim / z / 1.1);
		if (y < 0 || y >= PAGE_H * 2 - 1) { continue; }
		int c = eye[k % 3];
		int gap = z < 2.6 ? 3 : 2;
		page_plot(x, y, c);
		page_plot(x + gap, y, c);
	}
}

function page_plot(int x, int y, int c) -> void {
	if (x < 0 || y < 0 || x >= PAGE_W || y >= PAGE_H * 2) { return; }
	PAGE_PIX[y * PAGE_W + x] = c;
}

// a highlight that crosses the logo every few seconds, leaning like light off wet stone
function logo_shine(int x0, int y0, int w, int h, int frame) -> void {
	int sweep = (frame % 90) * (w + h + 40) / 50 - h - 20;
	if (sweep > w + h) { return; }
	map<int, int> lit = {};
	int back = PAGE_PIX[imax(0, y0 - 1) * PAGE_W + imax(0, x0 - 1)];
	for (int y = imax(0, y0); y < imin(PAGE_H * 2, y0 + h); ++y) {
		for (int x = imax(0, x0); x < imin(PAGE_W, x0 + w); ++x) {
			int d = x - x0 - sweep - (y0 + h - y) / 2;
			if (d < 0 || d > 3) { continue; }
			int c = PAGE_PIX[y * PAGE_W + x];
			int rgb = VPAL_RGB[c];
			if (((rgb >> 16) & 255) + ((rgb >> 8) & 255) < 220) { continue; }   // the letters only, not the plate
			if (!lit.has(c)) { lit[c] = pal(mix_rgb(rgb, 0xFFFFFF, d == 1 || d == 2 ? 0.75 : 0.4)); }
			PAGE_PIX[y * PAGE_W + x] = lit[c];
		}
	}
}

function title_overlay(bool can_continue, int frame) -> void {
	OVERLAYS = [];
	bool big = PAGE_W >= 110 && PAGE_H >= 34;
	int sx = big ? 2 : 1;
	int sy = big ? 3 : 2;
	int logo_y = imax(2, PAGE_H / 5);
	int logo_w = 8 * 6 * sx - sx;
	page_dim((PAGE_W - logo_w) / 2 - 3 * sx, logo_y - sy * 2, (PAGE_W + logo_w) / 2 + 3 * sx, logo_y + 9 * sy, 0.35);
	font_text("GRUBWELL", -1, logo_y, sx, sy, 0xFFE68C, 0x7AAE3C, true);
	logo_shine((PAGE_W - logo_w) / 2, logo_y, logo_w, 7 * sy, frame);
	int under = (logo_y + 7 * sy + 3) / 2;
	page_text((PAGE_W - 26) / 2, under, back_line(INK_DIM, "dungeons of the unwashed"));
	array<Line> menu = [key_row("N", "new descent", 5)];
	if (can_continue) { menu.push(key_row("C", "continue the saved run", 5)); }
	menu.push(key_row("?", "how to play", 5));
	menu.push(key_row("Q", "leave", 5));
	int w = 34;
	page_box((PAGE_W - w) / 2, PAGE_H - menu.size() - 5, w, "", menu, 0x4A5A36, INK_TITLE);
	string live = "R reloads the scripts mid-run, and so does saving one: edit them while you play";
	if (live.size() + 4 <= PAGE_W) { page_text((PAGE_W - live.size()) / 2, PAGE_H - 2, back_line(INK_MAGIC, live)); }
	page_text(1, PAGE_H - 1, back_line(INK_FAINT, `seed ${HOST_SEED}`));
	string credit = "written in JaiScript";
	page_text(PAGE_W - credit.size() - 1, PAGE_H - 1, back_line(INK_FAINT, credit));
}

function back_line(int ink, string text) -> Line {
	Line l;
	l.text = sgr_bg(INK_BACK);
	l.put(ink, text);
	return l;
}

function how_to_play() -> void {
	array<Line> lines = [
		line_of(INK_TEXT, "Ten floors down the Well, Vexadrach the Hollow-Flame"),
		line_of(INK_TEXT, "hoards the color he stole. Go and get it back."),
		Line(),
		key_row("w s", "step forward and back", 8),
		key_row("a d", "turn; q e sidestep", 8),
		key_row("w", "into a foe: attack", 8),
		key_row("z", "your class skill, ahead", 8),
		key_row("g i", "pick up; the pack", 8),
		key_row("m v", "the map; one view or four", 8),
		key_row(">", "descend the stairs", 8),
		key_row("esc", "every other key", 8),
		key_row("R", "reload the scripts; saving one does too", 8),
		Line(),
		line_of(INK_FAINT, "One key is one turn. Death is permanent.")];
	int w = 60;
	page_box((PAGE_W - w) / 2, imax(0, (PAGE_H - lines.size() - 2) / 2), w, "HOW TO PLAY", lines, 0x4A5A36, INK_TITLE);
	page_present();
	read_key();
}

// ---------------------------------------------------------- making a hero ---
class Hero { Species species; Profession calling; string name; bool chosen; }

function choose_hero() -> Hero {
	Hero hero;
	int step = 0;
	int cursor = 0;
	while (step < 2) {
		int count = step == 0 ? SPECIES.size() : PROFESSIONS.size();
		creation_page(step, cursor);
		string key = read_key();
		if (key == "Q") { return hero; }
		if (key == "up" || key == "k" || key == "w") { cursor = (cursor + count - 1) % count; continue; }
		if (key == "down" || key == "j" || key == "s") { cursor = (cursor + 1) % count; continue; }
		if (key == "esc") {
			if (step == 0) { return hero; }
			step = 0;
			cursor = 0;
			continue;
		}
		int picked = key == "enter" ? cursor : -1;
		for (int i = 0; i < count; ++i) {
			if (key == to_string(i + 1)) { picked = i; }
		}
		if (picked < 0) { continue; }
		if (step == 0) { hero.species = SPECIES[picked]; } else { hero.calling = PROFESSIONS[picked]; }
		step += 1;
		cursor = 0;
	}
	hero.name = name_page(hero);
	hero.chosen = true;
	return hero;
}

function creation_backdrop() -> void {
	page_open(page_size_cols(), page_size_rows());
	shaft_build();
	shaft_frame(1.5);
	for (int i = 0; i < PAGE_PIX.size(); ++i) {
		int rgb = VPAL_RGB[PAGE_PIX[i]];
		PAGE_PIX[i] = pal(scale_rgb(rgb, 0.4));
	}
}

function stat_bonus_line(Stats bonus) -> Line {
	Line l;
	array<string> names = ["hp", "atk", "def", "eva", "crit", "focus"];
	array<int> values = [bonus.hp, bonus.atk, bonus.def, bonus.eva, bonus.crit, bonus.focus];
	for (int i = 0; i < names.size(); ++i) {
		if (values[i] == 0) { continue; }
		if (l.cells > 0) { l.gap(2); }
		l.put(values[i] > 0 ? INK_GOOD : INK_BAD, values[i] > 0 ? `+${values[i]}` : `${values[i]}`);
		l.put(INK_DIM, ` ${names[i]}`);
	}
	return l;
}

function creation_page(int step, int cursor) -> void {
	creation_backdrop();
	int w = imin(PAGE_W - 4, 72);
	int x = (PAGE_W - w) / 2;
	array<Line> lines;
	lines.push(line_of(INK_TITLE, step == 0 ? "Who goes down the Well?" : "And what did you train as?"));
	lines.push(line_of(INK_FAINT, "number or arrows and enter; esc goes back"));
	lines.push(Line());
	int count = step == 0 ? SPECIES.size() : PROFESSIONS.size();
	for (int i = 0; i < count; ++i) {
		string name = step == 0 ? SPECIES[i].name : PROFESSIONS[i].name;
		Line l;
		if (i == cursor) { l.text = sgr_bg(INK_SELECT); }
		l.put(i == cursor ? INK_TITLE : INK_DIM, `${i + 1} `);
		l.put(i == cursor ? INK_BRIGHT : INK_TEXT, name);
		l.gap(w - 4 - l.cells);
		l.text += sgr_bg(INK_PANEL);
		lines.push(l);
	}
	lines.push(Line());
	int inner = w - 4;
	if (step == 0) {
		Species s = SPECIES[cursor];
		for (auto row : wrap_text(paint(INK_DIM, s.blurb), inner)) { lines.push(painted_line(row)); }
		lines.push(stat_bonus_line(s.bonus));
		for (auto row : wrap_text(paint(INK_TEAL, `${s.trait_name}: `) + paint(INK_TEXT, s.trait_blurb), inner)) { lines.push(painted_line(row)); }
	} else {
		Profession p = PROFESSIONS[cursor];
		for (auto row : wrap_text(paint(INK_DIM, p.blurb), inner)) { lines.push(painted_line(row)); }
		lines.push(stat_bonus_line(p.bonus));
		for (auto row : wrap_text(paint(INK_TEAL, `${p.skill_name} (${p.skill_cost} focus): `) + paint(INK_TEXT, p.skill_blurb), inner)) { lines.push(painted_line(row)); }
	}
	page_box(x, imax(0, (PAGE_H - lines.size() - 2) / 2), w, step == 0 ? "KIN" : "TRAINING", lines, 0x4A5A36, INK_TITLE);
	page_present();
}

// letters, enter to finish, esc for a nameless run
function name_page(Hero hero) -> string {
	string name = "";
	while (true) {
		creation_backdrop();
		int w = 50;
		Line field;
		field.put(INK_DIM, "> ");
		field.put(INK_BRIGHT, name);
		field.put(INK_TITLE, "_");
		array<Line> lines = [line_of(INK_TITLE, "Your name, wanderer?"),
			line_of(INK_DIM, `${hero.species.name} ${hero.calling.name}`), Line(), field, Line(),
			line_of(INK_FAINT, "enter to begin; esc for a nameless run")];
		page_box((PAGE_W - w) / 2, imax(0, (PAGE_H - lines.size() - 2) / 2), w, "NAME", lines, 0x4A5A36, INK_TITLE);
		page_present();
		string key = read_key();
		if (key == "enter") { break; }
		if (key == "esc") {
			name = "";
			break;
		}
		if (key == "backspace") {
			if (name.size() > 0) { name = name.substr(0, name.size() - 1); }
			continue;
		}
		if (key.size() == 1 && name.size() < 12) { name += key; }
	}
	return name == "" ? "Wanderer" : name;
}

// ------------------------------------------------------------- the endings ---
// the frame's canvas as a page: the title bar row, then the cockpit's pixels
function page_from_view() -> void {
	if (LAYOUT_KEY != layout_key()) { build_layout(); }
	fill_boxes();
	render_view(camera_x(), camera_y(), FACE_DX[G.facing], FACE_DY[G.facing]);
	page_open(CANVAS_W + (VIEW_LAYOUT == LAYOUT_SINGLE ? PANEL_W : 0), CANVAS_H + 1);
	for (int y = 0; y < CANVAS_H * 2; ++y) {
		for (int x = 0; x < CANVAS_W; ++x) { PAGE_PIX[(y + 2) * PAGE_W + x] = VPIX[y * STRIDE + x]; }
	}
}

function page_wash(int tint, float amount, float brightness) -> void {
	map<int, int> washed = {};
	for (int i = 0; i < PAGE_PIX.size(); ++i) {
		int c = PAGE_PIX[i];
		if (!washed.has(c)) {
			int rgb = VPAL_RGB[c];
			int lum = (((rgb >> 16) & 255) * 2 + ((rgb >> 8) & 255) * 3 + (rgb & 255)) / 6;
			washed[c] = pal(scale_rgb(mix_rgb(rgb, scale_rgb(tint, lum / 140.0), amount), brightness));
		}
		PAGE_PIX[i] = washed[c];
	}
}

function run_summary_lines() -> array<Line> {
	Player p = G.player;
	array<Line> lines;
	Line who;
	who.put(INK_BRIGHT, p.name);
	who.put(INK_DIM, `, level ${p.level} ${species_of(p.species_id).name} ${profession_of(p.profession_id).name}`);
	lines.push(who);
	lines.push(stat_line(["kills", "gold", "turns", "floor"], [p.kills, p.gold, G.turn, G.floor], 52));
	return lines;
}

function death_screen() -> void {
	if (G.smoke && VIEW_PROBE == null && !HOST_ANIM) { return; }   // a dump or an --anim run draws the ending too
	page_from_view();
	array<int> base = PAGE_PIX;
	for (int frame = 0; frame <= 6; ++frame) {
		PAGE_PIX = base;
		page_wash(0xB0141A, frame / 6.0 * 0.8, 1.0 - frame * 0.06);
		OVERLAYS = [];
		if (frame == 6) { death_overlay(); }
		page_present();
		if (HOST_INTERACTIVE) { sleep(0.06); }
	}
	if (HOST_INTERACTIVE) { read_key(); }
}

// The one that killed you, large, in full colour over the washed-out view, when it was a monster.
function killer_portrait() -> void {
	string glyph = "";
	int rgb = 0;
	for (auto kv : BESTIARY) {
		if (kv.second.name == G.death_cause) {
			glyph = kv.second.glyph;
			rgb = kv.second.ink;
		}
	}
	if (glyph == "") { return; }
	int a = glyph_art(glyph);
	int h = PAGE_H * 2 * 11 / 20;
	int w = h * ART_W[a] / ART_H[a];
	page_art(a, rgb, PAGE_W - w - imax(2, PAGE_W / 14), PAGE_H * 2 - h - 2, w, h);
}

// art a at w x h pixels on the page, lit full: the body roles take rgb and its shade and highlight
function page_art(int a, int rgb, int x0, int y0, int w, int h) -> void {
	array<int> colors;
	for (int i = 0; i < ROLE_RGB.size(); ++i) { colors.push(pal(ROLE_RGB[i])); }
	colors[1] = pal(rgb);
	colors[2] = pal(scale_rgb(rgb, 0.55));
	colors[3] = pal(mix_rgb(rgb, 0xFFFFFF, 0.45));
	int aw = ART_W[a];
	int ah = ART_H[a];
	int off = ART_OFF[a];
	for (int y = imax(0, y0); y < imin(PAGE_H * 2, y0 + h); ++y) {
		for (int x = imax(0, x0); x < imin(PAGE_W, x0 + w); ++x) {
			int code = ART_TEX[off + ((x - x0) * aw) / w + (((y - y0) * ah) / h) * aw];
			if (code >= 0) { PAGE_PIX[y * PAGE_W + x] = colors[code]; }
		}
	}
}

function death_overlay() -> void {
	killer_portrait();
	bool big = PAGE_W >= 110;
	int sx = big ? 2 : 1;
	int sy = big ? 3 : 2;
	int y = imax(2, PAGE_H / 4);
	font_text("FALLEN", -1, y, sx, sy, 0xFF7A5A, 0x8A0A12, true);
	Line cause;
	cause.put(INK_DIM, "slain by ");
	cause.put(INK_BAD, G.death_cause);
	cause.put(INK_DIM, ` on floor ${G.floor}, ${theme_name(G.floor)}`);
	array<Line> lines = [cause];
	for (auto l : run_summary_lines()) { lines.push(l); }
	lines.push(Line());
	int w = imin(PAGE_W - 4, 64);
	for (auto row : wrap_text(paint(INK_DIM, DEATH_QUIPS[lat(G.turn, G.floor, HOST_SEED) % DEATH_QUIPS.size()]), w - 4)) { lines.push(painted_line(row)); }
	lines.push(Line());
	lines.push(line_of(INK_FAINT, "any key to leave the Well"));
	page_box((PAGE_W - w) / 2, imin(PAGE_H - lines.size() - 3, (y + 7 * sy) / 2 + 3), w, "HERE LIES", lines, 0x6A2020, INK_BAD);
}

array<int> RAINBOW = [0xFF3030, 0xFF9020, 0xFFE040, 0x50E060, 0x40C8FF, 0x6060FF, 0xD050FF];

// The stolen fire comes home: flames climb the screen in rainbow bands rising like the colour
// climbing the Well. The flames run on their own little LCG, never the game's Rng.
function victory_screen() -> void {
	if (G.smoke && VIEW_PROBE == null && !HOST_ANIM) { return; }   // a dump or an --anim run draws the ending too
	page_open(page_size_cols(), page_size_rows());
	int w = PAGE_W;
	int h = PAGE_H * 2;
	array<int> heat;
	for (int i = 0; i < w * (h + 1); ++i) { heat.push(0); }
	array<int> fire;
	for (int hue : RAINBOW) {
		for (int l = 0; l < 16; ++l) { fire.push(pal(mix_rgb(0x050306, mix_rgb(hue, 0xFFFFFF, l > 12 ? (l - 12) / 5.0 : 0.0), l / 15.0))); }
	}
	int seed = 1;
	for (int warm = 0; warm < h; ++warm) { seed = fire_step(heat, w, h, seed); }
	bool big = PAGE_W >= 110;
	int frames = HOST_INTERACTIVE ? 80 : 1;
	for (int frame = 0; frame < frames; ++frame) {
		seed = fire_step(heat, w, h, seed);
		for (int y = 0; y < h; ++y) {
			int band = ((h - y) / 5 + frame / 2) % RAINBOW.size();
			for (int x = 0; x < w; ++x) { PAGE_PIX[y * w + x] = fire[band * 16 + heat[y * w + x]]; }
		}
		OVERLAYS = [];
		font_text("VICTORY", -1, imax(2, PAGE_H / 5), big ? 2 : 1, big ? 3 : 2, 0xFFFFFF, 0xFFD45E, true);
		if (frame >= frames / 3) { victory_overlay(big ? 3 : 2); }
		page_present();
		if (HOST_INTERACTIVE) { sleep(0.05); }
	}
	if (HOST_INTERACTIVE) { read_key(); }
}

// one step of the flames: a white-hot bottom row, and each pixel takes the one below it,
// drifted a pixel sideways and now and then a step cooler
function fire_step(array<int>& heat, int w, int h, int seed) -> int {
	for (int x = 0; x < w; ++x) {
		seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF;
		heat[h * w + x] = (seed >> 16) % 5 == 0 ? 6 : 15;
	}
	for (int y = h - 1; y >= 0; --y) {
		for (int x = 0; x < w; ++x) {
			seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF;
			int from = iclamp(x + (seed >> 16) % 3 - 1, 0, w - 1);
			heat[y * w + x] = imax(0, heat[(y + 1) * w + from] - ((seed >> 20) % 7 < 3 ? 1 : 0));
		}
	}
	return seed;
}

function victory_overlay(int sy) -> void {
	array<Line> lines = [line_of(INK_TEXT, "Vexadrach unravels. The swallowed fire pours out of him"),
		line_of(INK_TEXT, "in seven colors and climbs the Well like sunrise practicing."), Line()];
	for (auto l : run_summary_lines()) { lines.push(l); }
	lines.push(Line());
	lines.push(line_of(INK_TITLE, "Somewhere above, a stone jackodile grins approvingly."));
	lines.push(line_of(INK_FAINT, "any key to climb out"));
	int w = imin(PAGE_W - 4, 66);
	int y = imax(2, PAGE_H / 5) + 7 * sy / 2 + 3;
	page_box((PAGE_W - w) / 2, imin(PAGE_H - lines.size() - 2, y), w, "THE COLOR RETURNS", lines, 0x8A7A3A, INK_TITLE);
}
