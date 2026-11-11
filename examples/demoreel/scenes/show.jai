// SHOW — EIGHT's presentation: eight slides on the beat, one for each axis
// the 8-cube opens, each a dimension of JaiScript. A slide counts itself in with a
// big word, a code card slides in and types itself in syntax colours, a live
// demonstration plays beside it and three captions run along the lower third;
// at the end of its three bars it all flies out and the feature lights its pip
// in the checklist. The code is the language's own: every script card runs.

function rgb(int r, int g, int b) -> int { return r * 65536 + g * 256 + b; }

// the FOUR slide's live code: the slide imports scenes/live/spin_tweaked.jai over this
// definition while it runs (the reel's own hot reload, on screen); 'r' re-executes this
// file and puts it back
function live_spin() -> float { return 1.0; }
int LIVE_RELOADS = 0;

int INK_TEXT = rgb(232, 232, 240);
int INK_KEY = rgb(255, 121, 198);
int INK_TYPE = rgb(139, 233, 253);
int INK_NUM = rgb(255, 184, 108);
int INK_STR = rgb(241, 250, 140);
int INK_COMMENT = rgb(110, 124, 170);
int INK_FUNC = rgb(80, 250, 123);
int INK_PUNCT = rgb(176, 180, 210);

var SYN_KEYWORDS = {"class": 1, "function": 1, "for": 1, "if": 1, "else": 1, "return": 1, "while": 1, "import": 1,
	"true": 1, "false": 1, "null": 1, "new": 1, "override": 1, "const": 1, "extern": 1, "this": 1, "yield": 1,
	"struct": 1, "public": 1, "private": 1, "auto": 1, "var": 1, "switch": 1, "case": 1, "break": 1, "include": 1};
var SYN_TYPES = {"int": 1, "float": 1, "bool": 1, "string": 1, "void": 1, "char": 1, "array": 1, "map": 1,
	"double": 1, "int64_t": 1, "std": 1, "jai": 1};

function syn_alpha(char c) -> bool { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
function syn_digit(char c) -> bool { return c >= '0' && c <= '9'; }

// one colour per character of a line of JaiScript or C++
function syntax_ink(string s) -> array<int> {
	array<int> out = [];
	int n = s.size();
	int i = 0;
	while (i < n) {
		char c = s[i];
		if (c == '/' && i + 1 < n && s[i + 1] == '/') {
			while (i < n) {
				out.push(INK_COMMENT);
				++i;
			}
			break;
		}
		if (c == '"' || c == '`') {
			char q = c;
			out.push(INK_STR);
			++i;
			while (i < n) {
				char d = s[i];
				out.push(INK_STR);
				++i;
				if (d == q) { break; }
			}
			continue;
		}
		if (syn_digit(c)) {
			while (i < n && (syn_digit(s[i]) || s[i] == '.' || s[i] == 'f')) {
				out.push(INK_NUM);
				++i;
			}
			continue;
		}
		if (syn_alpha(c) || c == '#') {
			int j = i + 1;
			while (j < n && (syn_alpha(s[j]) || syn_digit(s[j]) || (s[j] == ':' && j + 1 < n && s[j + 1] == ':') ||
				(s[j] == ':' && j > 0 && s[j - 1] == ':'))) { ++j; }
			string word = s.substr(i, j - i);
			int ink = INK_TEXT;
			if (c == '#' || SYN_KEYWORDS.has(word)) { ink = INK_KEY; }
			else if (SYN_TYPES.has(word)) { ink = INK_TYPE; }
			else if (j < n && s[j] == '(') { ink = INK_FUNC; }
			else if (c >= 'A' && c <= 'Z') { ink = INK_TYPE; }
			for (int k = i; k < j; ++k) { out.push(ink); }
			i = j;
			continue;
		}
		out.push(INK_PUNCT);
		++i;
	}
	return out;
}

function ease_out(float k) -> float {
	float c = k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
	float g = c - 1.0;
	return 1.0 + 2.70158 * g * g * g + 1.70158 * g * g;
}

function ease_in(float k) -> float {
	float c = k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
	return c * c * c;
}

function shade(int c, float k) -> int {
	float f = k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
	return rgb(math::itrunc((c >> 16) * f), math::itrunc(((c >> 8) & 255) * f), math::itrunc((c & 255) * f));
}

class Slide {
	string word = "";
	string title = "";
	string file = "";
	int axis = 0;
	var code = [];
	var ink = [];
	var bullets = [];

	Slide(string word_, string title_, string file_, int axis_, var code_, var bullets_) {
		word = word_;
		title = title_;
		file = file_;
		axis = axis_;
		code = code_;
		bullets = bullets_;
		for (int i = 0; i < code.size(); ++i) { ink.push(syntax_ink(code[i])); }
	}
}

class Deck {
	var slides = [];
	int ui = 0;          // palette: 10 panel shades, the title bar, a line highlight, three window dots, white, black, reload green
	int hyper = 0;       // EIGHT's 8 axes x 12 levels
	float beat = 0.48387097;
	float bar = 1.93548388;
	float first = 1.93548388;   // the first slide's downbeat: bar 1
	float span = 5.80645164;    // three bars a slide

	// the layout, in cells and pixels, refreshed every frame
	int cols = 0;
	int rows = 0;
	int pw = 0;
	int ph = 0;
	int split = 0;       // the left column's right edge, in cells

	void build(var& pal) {
		ui = pal.size();
		pal.ramp(10, 10.0, 11.0, 24.0, 24.0, 26.0, 52.0);
		pal.add(38.0, 34.0, 70.0);
		pal.add(46.0, 52.0, 104.0);
		pal.add(255.0, 95.0, 86.0);
		pal.add(255.0, 189.0, 46.0);
		pal.add(39.0, 201.0, 63.0);
		pal.add(250.0, 250.0, 255.0);
		pal.add(0.0, 0.0, 0.0);
		pal.add(96.0, 224.0, 168.0);
		slides = [
			Slide("ONE", "SCRIPTING", "spark.jai", 0, [
				"class Spark {",
				"  int hp = 3;",
				"  string say() { return `hp=${hp}`; }",
				"}",
				"auto s = Spark();",
				"var up = [=](int k) { return k * 2; };",
				"s.hp = up(s.hp);"
			], ["C++-shaped: classes, lambdas", "int / auto / var: you pick strict", "fails at load, not at 3 a.m."]),
			Slide("TWO", "SERIALIZATION", "save.jai", 1, [
				"class Spark { int hp = 3; string name; }",
				"auto s = Spark();",
				"s.name = \"ember\";",
				"var j = to_json(s);",
				"var t = from_json(j);",
				"var b = to_base64(s);"
			], ["to_json / from_json round trip", "binary too: to_binary, to_base64", "shared handles keep sharing"]),
			Slide("THREE", "REFLECTION", "peek.jai", 2, [
				"class Spark { int hp = 3; }",
				"auto s = Spark();",
				"reflect::set(s, \"hp\", 9);",
				"for (auto f : reflect::fields(s)) {",
				"  print(f[\"name\"], f[\"type\"]);",
				"}",
				"print(reflect::type_name(s));"
			], ["fields, get, set, invoke by name", "field order is a contract", "every query is live"]),
			Slide("FOUR", "HOT RELOAD", "main.jai", 3, [
				"for (int i = 0; i < 12; i++) {",
				"  import(\"rules.jai\");",
				"  tick(i);",
				"  sleep(0.25);",
				"}"
			], ["edit a file, save, it applies live", "live objects migrate in place", "a broken edit never bricks it"]),
			Slide("FIVE", "PARALLEL", "swarm.jai", 4, [
				"var xs = [1, 2, 3, 4, 5, 6, 7, 8];",
				"parallel_for (auto& x : xs) {",
				"  x = x * x;",
				"}",
				"int inc(int v) { return v + 1; }",
				"var ys = parallel_transform(xs, inc);"
			], ["same result at any thread count", "byte-identical to the serial loop", "unsafe bodies refused, not run"]),
			Slide("SIX", "SIGNALS", "door.jai", 5, [
				"var d = Door();",
				"d.onOpen.connect(\"watch\",",
				"  [=](int w) -> void {",
				"    print(\"opened to {}\", w);",
				"  });",
				"d.open(42);"
			], ["private emitter, public signal", "receivers: C++ lambdas or script", "drop a receiver to disconnect"]),
			Slide("SEVEN", "DEBUGGING", "hero.jai", 6, [
				"function step(int n) {",
				"  var hp = n * 3;",
				"  return hp + 1;  // breakpoint here",
				"}",
				"for (int i = 0; i < 5; i++) {",
				"  print(step(i));",
				"}"
			], ["DAP debugger for VS Code", "step, call stack, locals, globals", "language server: hover, rename"]),
			Slide("EIGHT", "EMBEDDING", "main.cpp", 7, [
				"auto eng = jai::engine::make();",
				"jai::stdlib::register_all(eng);",
				"eng->add_function(\"roll\",",
				"  [](int64_t n) { return n / 2 + 1; });",
				"eng->execute(\"print(roll(20));\");"
			], ["no macros: bind any C++ class", "add_function takes any callable", "script writes land in C++ objects"])
		];
	}

	float start(int d) { return first + (d - 1) * span; }

	// 0 before the first slide, 1..8 during one, 9 after the last
	int section(float t) {
		if (t < first) { return 0; }
		int d = math::itrunc((t - first) / span) + 1;
		return d > 9 ? 9 : d;
	}

	float pulse(float t) {
		float ph_ = t / beat;
		float f = ph_ - math::floor(ph_);
		return (1.0 - f) * (1.0 - f);
	}

	void layout(int pw_, int ph_) {
		pw = pw_;
		ph = ph_;
		cols = HB_COLS;
		rows = HB_ROWS;
		split = cols * 56 / 100;
	}

	// ------------------------------------------------------------- pixels ---

	void fill(var& grid, int x0, int y0, int x1, int y1, int c) {
		x0 = x0 < 0 ? 0 : x0;
		y0 = y0 < 0 ? 0 : y0;
		x1 = x1 > pw ? pw : x1;
		y1 = y1 > ph ? ph : y1;
		for (int y = y0; y < y1; ++y) {
			int base = y * pw;
			for (int x = x0; x < x1; ++x) { grid[base + x] = c; }
		}
	}

	void dot(var& grid, float cx, float cy, float r, int c) {
		int x0 = math::itrunc(cx - r - 1.0);
		int x1 = math::itrunc(cx + r + 1.0);
		int y0 = math::itrunc(cy - r - 1.0);
		int y1 = math::itrunc(cy + r + 1.0);
		for (int y = y0; y <= y1; ++y) {
			if (y < 0 || y >= ph) { continue; }
			for (int x = x0; x <= x1; ++x) {
				if (x < 0 || x >= pw) { continue; }
				float dx = x + 0.5 - cx;
				float dy = y + 0.5 - cy;
				if (dx * dx + dy * dy <= r * r) { grid[y * pw + x] = c; }
			}
		}
	}

	void wire(var& grid, float x0, float y0, float x1, float y1, int c) {
		float dx = x1 - x0;
		float dy = y1 - y0;
		float adx = dx < 0.0 ? -dx : dx;
		float ady = dy < 0.0 ? -dy : dy;
		int steps = math::itrunc((adx > ady ? adx : ady) + 1.0);
		for (int s = 0; s <= steps; ++s) {
			int x = math::itrunc(x0 + dx * s / steps);
			int y = math::itrunc(y0 + dy * s / steps);
			if (x >= 0 && x < pw && y >= 0 && y < ph) { grid[y * pw + x] = c; }
		}
	}

	// a window: rounded body in the panel shades, a glowing border, a title bar a
	// cell tall with the three dots and a name; the cell rectangle is [c0, c1) x [r0, r1)
	void window(var& grid, int c0, int r0, int c1, int r1, int border, string name, int name_ink) {
		int x0 = col_px(c0);
		int x1 = col_px(c1);
		int y0 = row_px(r0);
		int y1 = row_px(r1);
		int bar_y = row_px(r0 + 1);
		for (int y = y0; y < y1; ++y) {
			if (y < 0 || y >= ph) { continue; }
			int shade_ = y < bar_y ? ui + 10 : ui + 9 - (y - y0) * 8 / (y1 - y0 + 1);
			int base = y * pw;
			for (int x = x0; x < x1; ++x) {
				if (x < 0 || x >= pw) { continue; }
				bool corner = (x == x0 || x == x1 - 1) && (y == y0 || y == y1 - 1);
				if (corner) { continue; }
				bool edge = x == x0 || x == x1 - 1 || y == y0 || y == y1 - 1;
				grid[base + x] = edge ? border : shade_;
			}
		}
		if (c1 - c0 < 8) { return; }
		txt_glyph(c0 + 1, r0, 0x25CF, rgb(255, 95, 86));
		txt_glyph(c0 + 2, r0, 0x25CF, rgb(255, 189, 46));
		txt_glyph(c0 + 3, r0, 0x25CF, rgb(39, 201, 63));
		int room = c1 - c0 - 7;
		string shown = name.size() > room ? name.substr(0, room) : name;
		txt_put(c0 + (c1 - c0 - shown.size()) / 2, r0, shown, name_ink);
	}

	// ------------------------------------------------------------- slides ---

	void draw(var& grid, int pw_, int ph_, float t, int done_through) {
		layout(pw_, ph_);
		if (cols < 60 || rows < 24) { return; }
		int d = section(t);
		if (d == 0) {
			draw_intro(grid, t);
			return;
		}
		draw_pips(grid, t, d);
		if (d <= 8) {
			draw_slide(grid, d, t - start(d), t);
			draw_sweep(grid, d, t - start(d));
		}
	}

	// the downbeat of a slide: a band of light in its axis' colour sweeps the screen
	void draw_sweep(var& grid, int d, float lt) {
		if (lt > 0.32) { return; }
		int axis_ink = hyper + slides[d - 1].axis * 12;
		int edge = math::itrunc(lt / 0.32 * (pw + ph + 40)) - 20;
		for (int y = 0; y < ph; ++y) {
			int base = y * pw;
			int x0 = edge - y / 2;
			for (int x = x0 - 10; x < x0 + 3; ++x) {
				if (x < 0 || x >= pw) { continue; }
				grid[base + x] = x >= x0 ? ui + 15 : axis_ink + 11 - (x0 - x) / 2;
			}
		}
	}

	// before the first slide, one bar: the number's name in big letters with a shine
	// across them, what the eight are typed under it, and a count-in on the four beats
	void draw_intro(var& grid, float t) {
		float lt = t - 0.08;
		if (lt < 0.0) { return; }
		string name = "EIGHT";
		int sx = 1;
		while (gfx.text_width(name, sx + 1) <= pw * 7 / 10 && sx < 6) { ++sx; }
		int tw = gfx.text_width(name, sx);
		int tx = (pw - tw) / 2;
		int ty = ph / 2 - 7 * sx - 2;
		int shown = math::itrunc(lt * 14.0) + 1;
		float leave = (first - t) / 0.35;
		if (leave > 0.0) {
			int ramp = leave < 1.0 ? hyper + 7 * 12 + 2 : hyper + 6 * 12 + 4;
			gfx.draw_text(grid, pw, ph, name, tx, ty, sx, sx, ramp, 8, ui + 16, shown);
			int sweep = math::itrunc((lt - 0.45) * pw * 0.9);
			for (int y = ty; y < ty + 7 * sx; ++y) {
				int base = y * pw;
				for (int x = tx; x < tx + tw; ++x) {
					int dd = x - tx + (y - ty) - sweep;
					if (dd >= 0 && dd < 2 * sx && grid[base + x] >= hyper && grid[base + x] < hyper + 96) { grid[base + x] = ui + 15; }
				}
			}
		}
		float count_at = first - bar;
		if (t > count_at) {
			int beats = math::itrunc((t - count_at) / beat);
			for (int k = 0; k < 4; ++k) {
				float cx = pw * 0.5 + (k - 1.5) * 4.0 * sx;
				float cy = ty + 7 * sx + 5.0 * sx;
				int axis_ink = hyper + (k * 2) * 12;
				dot(grid, cx, cy, k <= beats ? 1.2 * sx : 0.5 * sx, k < beats ? axis_ink + 6 : (k == beats ? axis_ink + 11 : ui + 4));
			}
		}
		string about = "jaiscript in eight dimensions";
		int shown_about = math::itrunc((lt - 0.15) * 60.0);
		float fade = (first - t) / 0.3;
		fade = fade > 1.0 ? 1.0 : fade;
		if (shown_about > 0) { txt_put((cols - about.size()) / 2, rows / 2 + rows / 4 + 1, about.substr(0, shown_about > about.size() ? about.size() : shown_about), shade(rgb(255, 206, 110), fade)); }
	}

	// the checklist: eight pips across the header's right, lit as each slide ends
	void draw_pips(var& grid, float t, int d) {
		int c0 = cols - 2 - 8 * 3;
		for (int k = 0; k < 8; ++k) {
			int col = c0 + k * 3;
			bool lit = d > k + 1 || (d == k + 1 && t - start(d) > span - 0.6);
			bool now = d == k + 1;
			int axis_ink = hyper + k * 12;
			int x0 = col_px(col);
			int x1 = col_px(col + 2);
			int y0 = row_px(1);
			int y1 = row_px(2);
			int c = lit ? axis_ink + 10 : (now ? axis_ink + 4 + math::itrunc(pulse(t) * 6.0) : ui + 3);
			fill(grid, x0, y0, x1, y1, c);
			txt_put(col, 2, `${k + 1}`, lit || now ? rgb(238, 236, 255) : rgb(90, 92, 130));
		}
	}

	void draw_slide(var& grid, int d, float lt, float t) {
		var& s = slides[d - 1];
		float in_k = ease_out((lt - 0.12) / 0.5);
		float out_k = ease_in((lt - (span - 0.5)) / 0.42);
		int axis_ink = hyper + s.axis * 12;
		int accent = HB_RGB.size() > 0 ? 0 : 0;
		// the counted word slams in white on the downbeat and settles to its axis' colours
		int scale = rows >= 34 ? 3 : 2;
		if (pw >= 300) { scale = 4; }
		int wx = col_px(2) - math::itrunc(out_k * pw * 0.6);
		int wy = row_px(1) + 1 - math::itrunc(pulse(t) * pulse(t) * 2.0);
		int ramp = lt < 0.12 ? ui + 15 : axis_ink + 4;
		gfx.draw_text(grid, pw, ph, s.word, wx, wy, scale, scale, ramp, lt < 0.12 ? 1 : 8, ui + 16, s.word.size());
		int word_cells = cell_col(gfx.text_width(s.word, scale)) + 4;
		int title_row = cell_row(wy + 7 * scale / 2);
		int typed = math::itrunc((lt - 0.2) * 28.0);
		if (typed > 0 && out_k < 0.5) {
			string spaced = "";
			for (int i = 0; i < s.title.size() && i < typed; ++i) { spaced += s.title.substr(i, 1) + " "; }
			txt_put(word_cells, title_row, spaced, shade(rgb(238, 236, 255), 1.0 - out_k * 2.0));
		}
		int top = cell_row(wy + 7 * scale) + 2;
		// the code card, sliding in from the left
		int card_w = split - 3;
		int lines = s.code.size();
		int card_h = lines + 4;
		int shift = math::itrunc((in_k - 1.0 - out_k) * (card_w + 4));
		int c0 = 2 + shift;
		int r0 = top;
		window(grid, c0, r0, c0 + card_w, r0 + card_h, axis_ink + 7, s.file, rgb(200, 196, 240));
		draw_code(grid, s, c0, r0, card_w, lt, d);
		// the captions under it
		int br = r0 + card_h + 2;
		for (int k = 0; k < s.bullets.size(); ++k) {
			float bk = ease_out((lt - 1.3 - k * 0.85) / 0.4);
			if (bk <= 0.0) { continue; }
			int bshift = math::itrunc((bk - 1.0 - out_k) * (split + 4));
			int row = br + k * 2;
			if (row >= rows - 2) { break; }
			fill(grid, col_px(2 + bshift), row_px(row), col_px(3 + bshift), row_px(row + 1), axis_ink + 9);
			string line = s.bullets[k];
			int shown = math::itrunc((lt - 1.3 - k * 0.85) * 48.0);
			txt_put(4 + bshift, row, line.substr(0, shown > line.size() ? line.size() : (shown < 0 ? 0 : shown)), rgb(238, 236, 255));
		}
		// the demonstration, sliding in from the right under the cube's window
		int v0 = split + 2;
		int v1 = cols - 2;
		int vr0 = rows / 2;
		int vr1 = rows - 3;
		int vshift = math::itrunc((1.0 - in_k + out_k) * (v1 - v0 + 4));
		window(grid, v0 + vshift, vr0, v1 + vshift, vr1, axis_ink + 7, "live", rgb(160, 156, 210));
		demo(grid, d, v0 + vshift, vr0 + 1, v1 + vshift, vr1, lt, t);
	}

	// the code, typed in at a steady clip with a block cursor; the debugging slide
	// keeps a breakpoint in the gutter and, once paused, highlights the line it holds
	void draw_code(var& grid, var& s, int c0, int r0, int card_w, float lt, int d) {
		int typed = math::itrunc((lt - 0.55) * 60.0);
		int lines = s.code.size();
		int left = c0 + 4;
		int room = card_w - 6;
		bool debugging = d == 7;
		int step_line = debugging && lt > 2.9 ? 2 : -1;
		for (int li = 0; li < lines; ++li) {
			int row = r0 + 2 + li;
			txt_put(c0 + 1, row, li + 1 < 10 ? ` ${li + 1}` : `${li + 1}`, rgb(84, 90, 140));
			if (debugging && li == 2) { txt_glyph(c0 + 3, row, 0x25CF, rgb(255, 85, 85)); }
			if (li == step_line) {
				fill(grid, col_px(c0 + 4), row_px(row), col_px(c0 + card_w - 1), row_px(row + 1), ui + 11);
				txt_glyph(c0 + 3, row, 0x25B6, rgb(255, 206, 110));
			}
			string line = s.code[li];
			var& ink = s.ink[li];
			int col = left;
			for (int ci = 0; ci < line.size(); ++ci) {
				if (typed <= 0) { break; }
				--typed;
				char ch = line[ci];
				if (ch == '\t') {
					col += 2;
					continue;
				}
				if (col - left < room) { txt_glyph(col, row, ch + 0, ink[ci]); }
				++col;
			}
			if (typed <= 0) {
				if (math::itrunc(lt * 4.0) % 2 == 0 && col - left < room) { txt_glyph(col, row, 0x2588, rgb(238, 236, 255)); }
				break;
			}
			--typed;
		}
	}

	// ---------------------------------------------------- demonstrations ---

	void demo(var& grid, int d, int c0, int r0, int c1, int r1, float lt, float t) {
		if (c1 - c0 < 10 || r1 - r0 < 5) { return; }
		int x0 = col_px(c0 + 1);
		int x1 = col_px(c1 - 1);
		int y0 = row_px(r0) + 1;
		int y1 = row_px(r1) - 2;
		switch (d) {
			case 1: demo_sparks(grid, x0, y0, x1, y1, lt); break;
			case 2: demo_archive(grid, c0, r0, c1, r1, lt); break;
			case 3: demo_inspector(grid, c0, r0, c1, r1, lt); break;
			case 4: demo_reload(grid, x0, y0, x1, y1, c0, r0, c1, lt); break;
			case 5: demo_cores(grid, x0, y0, x1, y1, c0, r0, lt); break;
			case 6: demo_signals(grid, x0, y0, x1, y1, c0, r0, c1, r1, lt, t); break;
			case 7: demo_debugger(grid, c0, r0, c1, r1, lt); break;
			default: demo_embed(grid, x0, y0, x1, y1, c0, r0, c1, r1, lt, t); break;
		}
	}

	// ONE: sparks are Spark objects, born at the centre, each ticking its own clock
	void demo_sparks(var& grid, int x0, int y0, int x1, int y1, float lt) {
		float cx = (x0 + x1) * 0.5;
		float cy = (y0 + y1) * 0.5;
		float reach = (x1 - x0 < y1 - y0 ? x1 - x0 : y1 - y0) * 0.48;
		int born = math::itrunc(lt * 22.0);
		born = born > 160 ? 160 : born;
		for (int k = 0; k < born; ++k) {
			float age = lt - k / 22.0;
			float life = 1.0 - age / 2.6;
			if (life <= 0.0) { continue; }
			float a = k * 2.39996 + age * 0.6;
			float r = reach * (age / 2.6) * (0.6 + 0.4 * math::sin(k * 1.7));
			int axis_ink = hyper + (k % 8) * 12;
			dot(grid, cx + math::cos(a) * r * 1.0, cy + math::sin(a) * r, 0.6 + life * 1.4, axis_ink + 4 + math::itrunc(life * 7.0));
		}
	}

	// TWO: the Spark from the card, its fields flowing out into the archive's own text
	// and back into an object
	void demo_archive(var& grid, int c0, int r0, int c1, int r1, float lt) {
		var json = ["{\"$type\":\"Spark\",", " \"$val\":{\"hp\":3,", "         \"name\":\"ember\"}}"];
		float out_k = lt < 3.1 ? ease_out((lt - 0.6) / 1.1) : 1.0 - ease_out((lt - 3.4) / 1.0);
		float k = 1.0 - out_k * 0.6;
		int box_w = 12;
		int bx = c0 + 2;
		int by = r0 + 1;
		fill(grid, col_px(bx), row_px(by), col_px(bx + box_w), row_px(by + 4), ui + 5);
		fill(grid, col_px(bx), row_px(by), col_px(bx + box_w), row_px(by + 1), hyper + 1 * 12 + 8);
		txt_put(bx + 1, by, "Spark", shade(rgb(255, 255, 255), k));
		txt_put(bx + 1, by + 1, "hp   3", shade(INK_NUM, k));
		txt_put(bx + 1, by + 2, "name ember", shade(INK_STR, k));
		int jc = bx + box_w + 3;
		int room = c1 - jc - 1;
		// the bytes in flight between them
		float ax = col_px(bx + box_w) + 1.0;
		float bx2 = col_px(jc) - 1.0;
		float fy = row_px(by + 2) * 1.0;
		for (int q = 0; q < 5; ++q) {
			float f = lt * 1.3 + q * 0.2;
			f -= math::floor(f);
			if (lt >= 3.1) { f = 1.0 - f; }
			dot(grid, ax + (bx2 - ax) * f, fy, 0.9, hyper + 1 * 12 + 10);
		}
		int chars = math::itrunc(out_k * 80.0);
		for (int li = 0; li < json.size(); ++li) {
			string line = json[li];
			var ink = syntax_ink(line);
			for (int ci = 0; ci < line.size() && ci < room; ++ci) {
				if (chars <= 0) { break; }
				--chars;
				txt_glyph(jc + ci, by + li, line[ci] + 0, ink[ci]);
			}
		}
		txt_put(c0 + 2, r1 - 2, lt < 3.1 ? "to_json(s)    >>" : "<<    from_json(j)", INK_FUNC);
		if (lt > 4.2) { txt_put(c0 + 2, r1 - 3, "t.name == \"ember\"", INK_STR); }
	}

	// THREE: an inspector built from reflect::fields, its values live
	void demo_inspector(var& grid, int c0, int r0, int c1, int r1, float lt) {
		txt_put(c0 + 2, r0 + 1, "reflect::fields(boss)", INK_FUNC);
		var names = ["name", "hp", "speed", "angry", "loot"];
		var types = ["string", "int", "float", "bool", "array"];
		int hp = 900 - math::itrunc(lt * 61.0) % 900;
		float speed = 2.5 + math::sin(lt * 2.1) * 1.5;
		var vals = ["\"lich\"", `${hp}`, gfx.fmt1(speed), lt > 2.4 ? "true" : "false", "[3 items]"];
		int pick = math::itrunc(lt / (beat * 2.0)) % 5;
		for (int k = 0; k < 5; ++k) {
			int row = r0 + 3 + k;
			if (row >= r1 - 1) { break; }
			float rk = ease_out((lt - 0.5 - k * 0.18) / 0.35);
			if (rk <= 0.0) { continue; }
			if (k == pick) { fill(grid, col_px(c0 + 1), row_px(row), col_px(c1 - 1), row_px(row + 1), ui + 11); }
			txt_put(c0 + 2, row, names[k], INK_TEXT);
			txt_put(c0 + 10, row, types[k], INK_TYPE);
			txt_put(c0 + 18, row, vals[k], k == 1 || k == 2 ? INK_NUM : (k == 0 ? INK_STR : INK_KEY));
		}
		// the speed field as a slider
		int sr = r0 + 9;
		if (sr < r1 - 1) {
			int sx0 = col_px(c0 + 2);
			int sx1 = col_px(c1 - 3);
			int y = row_px(sr) + 1;
			fill(grid, sx0, y, sx1, y + 1, ui + 4);
			float f = (speed - 1.0) / 3.0;
			dot(grid, sx0 + (sx1 - sx0) * f, y + 0.5, 1.8, hyper + 2 * 12 + 10);
		}
	}

	// FOUR, for real: a star turned by live_spin(), whose code this reel reloads while it
	// runs. On the slide's beat the reel imports scenes/live/spin_tweaked.jai; the engine
	// redefines live_spin() under the running scene, and the star keeps the angle it has
	// and turns at the new speed from the next frame. A second pass through the reel
	// finds the tweak already live (a module runs once a process) and says so.
	float live_angle = 0.0;
	float live_last = -1.0;
	float live_at = -1.0;
	bool live_was_live = false;

	void demo_reload(var& grid, int x0, int y0, int x1, int y1, int c0, int r0, int c1, float lt) {
		if (lt < live_last || live_last < 0.0) {
			live_angle = 0.0;
			live_at = -1.0;
			live_was_live = live_spin() != 1.0;
		}
		float step = live_last < 0.0 || lt < live_last ? 0.0 : lt - live_last;
		live_last = lt;
		if (live_at < 0.0 && lt >= 2.88) {
			live_at = lt;
			if (!live_was_live) {
				import(SCENES_DIR + "/live/spin_tweaked.jai");
				++LIVE_RELOADS;
			}
		}
		float spin = live_spin();
		live_angle += spin * step;
		float cx = (x0 + x1) * 0.5;
		float cy = (y0 + y1) * 0.5 + 3.0;
		float r = (x1 - x0 < y1 - y0 ? x1 - x0 : y1 - y0) * 0.3;
		for (int k = 0; k < 5; ++k) {
			float a = live_angle + k * 1.2566371;
			float b = a + 2.5132742;
			wire(grid, cx + math::cos(a) * r, cy + math::sin(a) * r, cx + math::cos(b) * r, cy + math::sin(b) * r, hyper + 3 * 12 + 9);
		}
		dot(grid, cx, cy, 1.5, ui + 15);
		txt_put(c0 + 2, r0, "scenes/live/spin_tweaked.jai", rgb(200, 196, 240));
		txt_put(c0 + 2, r0 + 1, "function live_spin() -> float {", INK_KEY);
		txt_put(c0 + 11, r0 + 1, "live_spin", INK_FUNC);
		txt_put(c0 + 26, r0 + 1, "float", INK_TYPE);
		txt_put(c0 + 2, r0 + 2, "  return 6.0;", INK_NUM);
		txt_put(c0 + 4, r0 + 2, "return", INK_KEY);
		txt_put(c0 + 2, r0 + 3, "}", INK_PUNCT);
		int vr = row_of(y1) - 1;
		txt_put(c0 + 2, vr, `live_spin() = ${gfx.fmt1(spin)}   angle ${gfx.fmt1(live_angle)}`, spin > 1.0 ? rgb(120, 255, 170) : rgb(200, 200, 220));
		if (live_at >= 0.0 && lt - live_at < 2.6) {
			int br = r0 + 5;
			fill(grid, col_px(c0 + 1), row_px(br), col_px(c1 - 1), row_px(br + 1), ui + 17);
			txt_put(c0 + 1, br, live_was_live ? " ALREADY LIVE " : " RELOADED ", rgb(10, 34, 28));
			txt_put(c0 + (live_was_live ? 16 : 12), br, live_was_live ? "since the reel's last pass" : "imported mid-scene, state kept", rgb(226, 255, 240));
		}
	}

	int row_of(int y) { return cell_row(y); }

	// FIVE: eight cores render their own bands of the same frame at once
	void demo_cores(var& grid, int x0, int y0, int x1, int y1, int c0, int r0, float lt) {
		int bands = 8;
		int bh = (y1 - y0) / bands;
		if (bh < 1) { return; }
		float sweep = ease_out(lt / 2.2) * 1.0;
		int reach = x0 + math::itrunc((x1 - x0) * (sweep > 1.0 ? 1.0 : sweep));
		for (int b = 0; b < bands; ++b) {
			int by = y0 + b * bh;
			int axis_ink = hyper + b * 12;
			for (int y = by; y < by + bh - 1; ++y) {
				int base = y * pw;
				for (int x = x0; x < reach; ++x) {
					float u = (x - x0) * 1.0 / (x1 - x0);
					float v = (y - y0) * 1.0 / (y1 - y0);
					float f = math::sin(u * 9.0 + lt * 2.0) + math::sin(v * 7.0 - lt) + math::sin((u + v) * 5.0);
					int lv = math::itrunc((f + 3.0) * 1.6);
					grid[base + x] = axis_ink + (lv < 0 ? 0 : (lv > 11 ? 11 : lv));
				}
				if (reach < x1 && reach > x0) { grid[base + reach] = ui + 15; }
			}
		}
		if (lt > 2.4) { txt_put(c0 + 2, cell_row(y1) - 1, " 8 cores . 1 frame . same bytes ", rgb(238, 236, 255)); }
	}

	// SIX: one emitter, four receivers; a pulse runs down every wire on the beat
	void demo_signals(var& grid, int x0, int y0, int x1, int y1, int c0, int r0, int c1, int r1, float lt, float t) {
		float ex = x0 + (x1 - x0) * 0.16;
		float ey = (y0 + y1) * 0.5;
		var names = ["watch", "audio", "ui", "fx"];
		float ph_ = t / beat;
		float f = ph_ - math::floor(ph_);
		for (int k = 0; k < 4; ++k) {
			float rx = x0 + (x1 - x0) * 0.78;
			float ry = y0 + (y1 - y0) * (k + 0.5) / 4.0;
			int axis_ink = hyper + (k * 2 + 1) * 12;
			wire(grid, ex, ey, rx, ry, ui + 4);
			if (lt > 0.8) {
				float px = ex + (rx - ex) * f;
				float py = ey + (ry - ey) * f;
				dot(grid, px, py, 1.3, axis_ink + 10);
			}
			dot(grid, rx, ry, 2.2 + (1.0 - f) * 1.0, axis_ink + (f < 0.25 ? 11 : 6));
			txt_put(cell_col(math::itrunc(rx)) + 2, cell_row(math::itrunc(ry)), names[k], rgb(220, 216, 250));
		}
		dot(grid, ex, ey, 3.0, hyper + 5 * 12 + 6 + math::itrunc((1.0 - f) * 5.0));
		txt_put(cell_col(math::itrunc(ex)) - 3, cell_row(math::itrunc(ey)) + 2, "onOpen", INK_FUNC);
		int hits = lt > 0.8 ? math::itrunc((lt - 0.8) / beat) : 0;
		txt_put(c0 + 2, r1 - 2, `open(42) x${hits}  received ${hits * 4}`, INK_NUM);
	}

	// SEVEN: paused on the breakpoint, once per call: the locals and the call stack
	void demo_debugger(var& grid, int c0, int r0, int c1, int r1, float lt) {
		bool paused = lt > 2.9;
		int n = paused ? math::itrunc((lt - 2.9) / (beat * 2.0)) % 5 : 0;
		fill(grid, col_px(c0 + 1), row_px(r0), col_px(c1 - 1), row_px(r0 + 1), paused ? ui + 13 : ui + 11);
		txt_put(c0 + 2, r0, paused ? " PAUSED  breakpoint  hero.jai:3 " : " running ", paused ? rgb(255, 250, 230) : rgb(170, 170, 210));
		txt_put(c0 + 2, r0 + 2, "LOCALS", rgb(150, 146, 210));
		txt_put(c0 + 3, r0 + 3, "n", INK_TEXT);
		txt_put(c0 + 9, r0 + 3, `${n}`, INK_NUM);
		txt_put(c0 + 3, r0 + 4, "hp", INK_TEXT);
		txt_put(c0 + 9, r0 + 4, `${n * 3}`, INK_NUM);
		if (r0 + 8 < r1) {
			txt_put(c0 + 2, r0 + 6, "CALL STACK", rgb(150, 146, 210));
			txt_put(c0 + 3, r0 + 7, "step    hero.jai:3", INK_FUNC);
			txt_put(c0 + 3, r0 + 8, "<main>  hero.jai:6", rgb(170, 170, 210));
		}
		if (paused && r0 + 10 < r1) { txt_put(c0 + 2, r0 + 10, `printed ${n * 3 + 1}`, INK_NUM); }
	}

	// EIGHT: C++ and script, calls crossing the bridge both ways
	void demo_embed(var& grid, int x0, int y0, int x1, int y1, int c0, int r0, int c1, int r1, float lt, float t) {
		int bw = (x1 - x0) / 3;
		int ay = y0 + 4;
		int by = y1 - 4;
		fill(grid, x0 + 2, ay, x0 + 2 + bw, by, ui + 6);
		fill(grid, x1 - 2 - bw, ay, x1 - 2, by, ui + 6);
		int cpp = hyper + 7 * 12;
		int jai = hyper + 4 * 12;
		fill(grid, x0 + 2, ay, x0 + 2 + bw, ay + 2, cpp + 8);
		fill(grid, x1 - 2 - bw, ay, x1 - 2, ay + 2, jai + 8);
		txt_put(cell_col(x0 + 2 + bw / 2) - 1, cell_row(ay + 5), "C++", rgb(255, 255, 255));
		txt_put(cell_col(x1 - 2 - bw / 2) - 4, cell_row(ay + 5), "script", rgb(255, 255, 255));
		float lane_a = ay + (by - ay) * 0.4;
		float lane_b = ay + (by - ay) * 0.65;
		wire(grid, x0 + 2 + bw, lane_a, x1 - 2 - bw, lane_a, ui + 4);
		wire(grid, x0 + 2 + bw, lane_b, x1 - 2 - bw, lane_b, ui + 4);
		float gap = (x1 - 2 - bw) - (x0 + 2 + bw);
		for (int k = 0; k < 4; ++k) {
			float f = (t * 1.6 + k * 0.25) - math::floor(t * 1.6 + k * 0.25);
			dot(grid, x0 + 2 + bw + gap * f, lane_a, 1.2, jai + 10);
			dot(grid, x1 - 2 - bw - gap * f, lane_b, 1.2, cpp + 10);
		}
		txt_put(c0 + 2, r1 - 2, lt > 2.4 ? "print(roll(20));  ->  11" : "print(roll(20));", INK_FUNC);
	}
}
