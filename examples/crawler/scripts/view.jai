// The first-person view: a DDA raycaster on the four cardinal facings, drawn into a pixel canvas
// at twice the text rows (each cell is an upper half block, foreground the top pixel and
// background the bottom one, an escape only where a colour changes). Walls, floors and
// ceilings take the floor's textures and its torchlight: your own torch, and the sconces on the
// walls, which light the stone around them. Sprites stand behind a per-column z-buffer. The
// multi-view runs the same caster once per pane: ahead, and smaller left, right and behind.

int VIEW_W = 64;            // the forward view in cells (VIEW_W x VIEW_H*2 pixels)
int VIEW_H = 20;
int LAYOUT_SINGLE = 0;      // the forward view alone
int LAYOUT_MULTI = 1;       // the forward view with LEFT, RIGHT and BEHIND around it
int VIEW_LAYOUT = 1;
float PLANE_LEN = 0.9;      // half-FOV tangent: about 84 degrees
float WALL_K = 1.3;         // wall height at distance 1, in pane heights
float SPRITE_RANGE = 14.0;  // past this the dark swallows sprites
float SPRITE_ASPECT = 0.85;
int VIEW_WORKERS = 0;       // nonzero: parallel_transform the blit rows (pays off on wide canvases)
var VIEW_PROBE = null;      // headless frame tap: called once per smoke turn when set
int HAND_BOB = 0;           // pixels the hands dip mid-step
float TORCH_LEVEL = 13.0;   // your torch's light at arm's length, in ramp levels (15 is the brightest)
float SIDE_LIGHT = 0.74;    // the side and rear panes are dimmer, so the view ahead leads
float PANE_LIGHT = 1.0;

int INK_BACK = 0x09080C;    // behind the frames and between the panes

// ----------------------------------------------------------------- palette ---
// Every colour the canvas holds is interned once, with its two escapes built in the terminal's
// colour depth. ASCII mode draws one colour per cell, the mean of its two pixels.
map<int, int> VPAL_KEYS = {};
array<string> VPAL_FG = [];
array<string> VPAL_BG = [];
array<int> VPAL_RGB = [];
map<int, int> VPAL_MEAN = {};

function pal(int rgb) -> int {
	if (VPAL_KEYS.has(rgb)) { return VPAL_KEYS[rgb]; }
	int i = VPAL_FG.size();
	VPAL_KEYS[rgb] = i;
	VPAL_FG.push(sgr_fg(rgb));
	VPAL_BG.push(sgr_bg(rgb));
	VPAL_RGB.push(rgb);
	return i;
}

function pal_mean(int top, int bottom) -> int {
	int key = top * 1048576 + bottom;
	if (VPAL_MEAN.has(key)) { return VPAL_MEAN[key]; }
	int mean = pal(mix_rgb(VPAL_RGB[top], VPAL_RGB[bottom], 0.5));
	VPAL_MEAN[key] = mean;
	return mean;
}

// ------------------------------------------------------------------- ramps ---
// A ramp is 16 palette indices from the fog (level 0) to torch-lit (level 15), warming as it
// brightens. A glow ramp ignores the dark.
array<int> RAMP = [];
map<int, int> RAMP_CACHE = {};
int FOG_RGB = 0;

function ramp(int rgb) -> int {
	if (RAMP_CACHE.has(rgb)) { return RAMP_CACHE[rgb]; }
	int base = RAMP.size();
	RAMP_CACHE[rgb] = base;
	int r = (rgb >> 16) & 255;
	int g = (rgb >> 8) & 255;
	int b = rgb & 255;
	int fr = (FOG_RGB >> 16) & 255;
	int fg = (FOG_RGB >> 8) & 255;
	int fb = FOG_RGB & 255;
	for (int l = 0; l < 16; ++l) {
		float t = l / 15.0;
		float lit = 0.3 + 0.85 * t;
		int lr = iclamp(lerp_f(fr, r * lit * (1.0 + 0.12 * t), t), 0, 255);
		int lg = iclamp(lerp_f(fg, g * lit, t), 0, 255);
		int lb = iclamp(lerp_f(fb, b * lit * (1.0 - 0.12 * t), t), 0, 255);
		RAMP.push(pal((lr << 16) | (lg << 8) | lb));
	}
	return base;
}

function lerp_f(int a, float b, float t) -> float { return a + (b - a) * t; }

function glow(int rgb) -> int {
	int key = rgb | 0x1000000;
	if (RAMP_CACHE.has(key)) { return RAMP_CACHE[key]; }
	int base = RAMP.size();
	RAMP_CACHE[key] = base;
	int c = pal(rgb);
	for (int l = 0; l < 16; ++l) { RAMP.push(c); }
	return base;
}

// your torch: full light within a step, a tenth of it at fourteen
function lightf(float d) -> float {
	float l = 1.7 / (1.0 + 0.4 * d + 0.05 * d * d);
	return l > 1.0 ? 1.0 : l;
}

// ---------------------------------------------------------------- the cache ---
// Rebuilt when the floor changes (or after a reload or a load wipes it): the theme's ramps,
// the baked textures, each tile's wall and floor variant, the landmarks, and the sconce light.
int VC_FLOOR = -1;
array<int> WTEX = [];       // 4 wall textures: plain, sconce (two flame frames), skull niche
array<int> FTEX = [];       // 6 floor textures: flags, puddle, bones, moss, blood, stair runes
array<int> CTEX = [];       // the ceiling: slabs and beams
array<int> WALL_KIND = [];  // per tile: 0 plain, 1 sconce, 2 skull niche
array<int> FLOOR_KIND = []; // per tile: its FTEX offset
array<int> ROLE_RAMP = [];
array<int> FLASH_TAB = [];
int HURT_RAMP = 0;
int BAR_GOOD = 0;
int BAR_BAD = 0;
array<int> LM_X = [];
array<int> LM_Y = [];
array<int> LM_ART = [];

// the sconces' light, LIGHT_RES cells to a tile edge, in ramp levels
int LIGHT_RES = 4;
int LIGHT_W = 0;
array<float> LIGHT = [];

function view_cache_invalidate() -> void { VC_FLOOR = -1; }

// visual-only combat marks: the turn each monster was last hit, and the blood each death leaves
map<int, int> VIEW_HITS = {};
array<int> DECALS = [];
int DECAL_FLOOR = -1;

function view_note_hit(int id) -> void { VIEW_HITS[id] = G.turn; }

function view_note_kill(int x, int y) -> void {
	if (DECAL_FLOOR != G.floor) {
		DECALS = [];
		DECAL_FLOOR = G.floor;
	}
	DECALS.push(idx(x, y));
	if (VC_FLOOR == G.floor) { apply_decals(); }
}

function apply_decals() -> void {
	if (DECAL_FLOOR != G.floor) { return; }
	for (int t : DECALS) {
		if (FLOOR_KIND[t] != 5 * 1024) { FLOOR_KIND[t] = 4 * 1024; }
	}
}

function build_view_cache() -> void {
	if (ART_W.size() == 0) { build_art(); }
	VC_FLOOR = G.floor;
	int ti = theme_index(G.floor);
	Theme theme = DEPTH_THEMES[ti];
	FOG_RGB = theme.fog;
	RAMP = [];
	RAMP_CACHE = {};
	SPRITE_RAMPS = {};

	array<int> wmat = [ramp(theme.mortar), ramp(theme.wall), ramp(scale_rgb(theme.wall, 0.62)), ramp(mix_rgb(theme.wall, 0xFFFFFF, 0.3)),
		ramp(theme.accent), ramp(scale_rgb(theme.accent, 0.6)), glow(theme.glow), ramp(0x0A080C), ramp(0xE6DCC0), ramp(0x5A5A68),
		glow(0xFF8C1E), glow(0xFFD050), glow(0xFFFCE8)];
	int seed = ti * 101 + 7;
	array<int> base = fresh_codes();
	switch (theme.pattern) {
		case 0: tex_brick(base, seed); tex_moss(base, seed); break;
		case 1: tex_brick(base, seed); tex_blood(base, seed); break;
		case 2: tex_block(base, seed); tex_mushrooms(base, seed); break;
		case 3: tex_panel(base, seed); break;
		case 4: tex_rock(base, seed); break;
		default: tex_marble(base, seed); break;
	}
	WTEX = [];
	bake(WTEX, base, wmat);
	for (int frame = 0; frame < 2; ++frame) {
		array<int> torch = base;
		tex_torch(torch, frame);
		bake(WTEX, torch, wmat);
	}
	array<int> niche = base;
	tex_skull(niche);
	bake(WTEX, niche, wmat);

	array<int> fmat = [ramp(scale_rgb(theme.floor, 0.45)), ramp(theme.floor), ramp(scale_rgb(theme.floor, 0.75)),
		ramp(mix_rgb(theme.floor, 0xFFFFFF, 0.22)), ramp(theme.accent), ramp(mix_rgb(FOG_RGB, 0x3C64B4, 0.6)),
		ramp(0xB4DCFF), ramp(0xDCD2B4), glow(0x5AFFF0), ramp(0x8C1020)];
	array<int> flags = fresh_codes();
	tex_flags(flags, seed + 3);
	FTEX = [];
	bake(FTEX, flags, fmat);
	for (int kind = 1; kind <= 5; ++kind) {
		array<int> decor = flags;
		tex_floor_decor(decor, kind, seed + kind);
		bake(FTEX, decor, fmat);
	}

	array<int> cmat = [ramp(scale_rgb(theme.ceil, 0.45)), ramp(theme.ceil), ramp(scale_rgb(theme.ceil, 0.7)),
		ramp(mix_rgb(theme.ceil, 0xFFFFFF, 0.2)), ramp(0x5A3A22), ramp(0x32200F), ramp(mix_rgb(theme.accent, theme.ceil, 0.5))];
	array<int> slabs = fresh_codes();
	tex_ceiling(slabs, seed + 8);
	CTEX = [];
	bake(CTEX, slabs, cmat);

	WALL_KIND = [];
	FLOOR_KIND = [];
	LM_X = [];
	LM_Y = [];
	LM_ART = [];
	for (int ty = 0; ty < MAP_H; ++ty) {
		for (int tx = 0; tx < MAP_W; ++tx) {
			int t = G.tiles[idx(tx, ty)];
			int h = lat(tx, ty, G.floor);
			WALL_KIND.push(h % 13 == 0 ? 1 : (h % 13 == 6 ? 2 : 0));
			int fk = h % 19;
			fk = fk == 3 ? 1 : (fk == 7 ? 2 : (fk == 11 ? 3 : (fk == 15 ? 4 : 0)));
			if (t == TILE_STAIRS) { fk = 5; }
			FLOOR_KIND.push(fk * 1024);
			string landmark = t == TILE_STAIRS ? "stairs" : (t == TILE_STATUE ? "statue" : (t == TILE_MURAL ? "mural" : ""));
			if (landmark != "") {
				LM_X.push(tx);
				LM_Y.push(ty);
				LM_ART.push(ART_ID[landmark]);
			}
		}
	}
	apply_decals();
	build_light();

	ROLE_RAMP = [];
	FLASH_TAB = [];
	for (int i = 0; i < ROLE_RGB.size(); ++i) {
		ROLE_RAMP.push(ROLE_GLOW[i] != 0 ? glow(ROLE_RGB[i]) : ramp(ROLE_RGB[i]));
		FLASH_TAB.push(glow(mix_rgb(ROLE_RGB[i], 0xFFFFFF, 0.6)));
	}
	HURT_RAMP = glow(0xC80A14);
	BAR_GOOD = RAMP[glow(0x3CF05A)];
	BAR_BAD = RAMP[glow(0x781414)];
}

// Each sconce is a light a little out from its wall face, falling off with the square of the
// distance, and only onto tiles that can see the stone in front of it.
function build_light() -> void {
	LIGHT_W = MAP_W * LIGHT_RES;
	LIGHT = [];
	for (int i = 0; i < LIGHT_W * MAP_H * LIGHT_RES; ++i) { LIGHT.push(0.0); }
	int reach = 5;
	for (int wy = 0; wy < MAP_H; ++wy) {
		for (int wx = 0; wx < MAP_W; ++wx) {
			if (G.tiles[idx(wx, wy)] != TILE_WALL || WALL_KIND[idx(wx, wy)] != 1) { continue; }
			for (Step d : FOUR_DIRS) {
				int fx = wx + d.dx;
				int fy = wy + d.dy;
				if (!is_walkable(fx, fy)) { continue; }
				float lx = wx + 0.5 + d.dx * 0.62;
				float ly = wy + 0.5 + d.dy * 0.62;
				for (int ty = imax(0, fy - reach); ty <= imin(MAP_H - 1, fy + reach); ++ty) {
					for (int tx = imax(0, fx - reach); tx <= imin(MAP_W - 1, fx + reach); ++tx) {
						if (!is_walkable(tx, ty) || !line_clear(fx, fy, tx, ty)) { continue; }
						light_tile(tx, ty, lx, ly);
					}
				}
				break;
			}
		}
	}
}

function light_tile(int tx, int ty, float lx, float ly) -> void {
	for (int sy = 0; sy < LIGHT_RES; ++sy) {
		for (int sx = 0; sx < LIGHT_RES; ++sx) {
			float px = tx + (sx + 0.5) / LIGHT_RES - lx;
			float py = ty + (sy + 0.5) / LIGHT_RES - ly;
			float level = 14.0 / (1.0 + 0.55 * (px * px + py * py));
			int i = (ty * LIGHT_RES + sy) * LIGHT_W + tx * LIGHT_RES + sx;
			if (level > LIGHT[i]) { LIGHT[i] = level; }
		}
	}
}

function light_at(float x, float y) -> float {
	int lx = iclamp(x * LIGHT_RES, 0, LIGHT_W - 1);
	int ly = iclamp(y * LIGHT_RES, 0, MAP_H * LIGHT_RES - 1);
	return LIGHT[ly * LIGHT_W + lx];
}

// ------------------------------------------------------------------ layout ---
// Panes are inner rectangles in cells; pane 0 is always the forward view. A framed pane has a
// one-cell border with its label in the top edge. Boxes are framed text rectangles in the
// multi-view's corners; ui.jai writes their rows into BOX_ROWS every frame.
class Pane { int x; int y; int w; int h; int rot; bool framed; int row_offset; }   // rot: quarter turns clockwise from the facing
class Box { int x; int y; int w; int h; int id; int first_row; }

array<Pane> PANES = [];
array<Box> BOXES = [];
int BOX_LOG = 0;
int BOX_MAP = 1;
int BOX_COMPASS = 2;
int BOX_HERO = 3;
int BOX_SIGHT = 4;
int BOX_LOOT = 5;
array<string> BOX_NAMES = ["LOG", "MAP", "COMPASS", "CHARACTER", "IN SIGHT", "LOOT"];
array<string> BOX_ROWS = [];    // every box row, exactly its box's width in cells
array<int> BOX_ROW_X = [];      // the canvas column each BOX_ROWS entry starts at
array<string> PANE_NAMES = ["AHEAD", "RIGHT", "BEHIND", "LEFT"];

// The forward pane can hold text instead of the view: the overhead map, the pack, the help.
bool OVERLAY_ON = false;
string OVERLAY_LABEL = "";
array<string> OVERLAY_ROWS = [];

int CANVAS_W = 0;           // cells; the canvas is CANVAS_W x CANVAS_H*2 pixels
int CANVAS_H = 0;
int STRIDE = 0;
array<int> VPIX = [];
array<float> ZBUF = [];
array<int> VROWS = [];
array<float> ROW_DIST = [];
array<float> ROW_LIGHT = [];
array<float> BAYER = [];
int LAYOUT_KEY = -1;

function layout_key() -> int { return (VIEW_LAYOUT * 1000 + VIEW_W) * 1000 + VIEW_H; }

// The multi-view is a cockpit: LEFT and RIGHT at half scale beside the forward view on its
// horizon (the three read as one panorama), BEHIND above it like a rear-view mirror, and a
// text box in every corner.
function layout_panes(int mode, int fw, int fh) -> void {
	PANES = [];
	BOXES = [];
	if (mode == LAYOUT_SINGLE) {
		PANES.push(Pane{0, 0, fw, fh, 0, false});
		CANVAS_W = fw;
		CANVAS_H = fh;
		return;
	}
	int sh = fh / 2;
	int sw = sh * 16 / 5;
	int fx = sw + 3;
	int fy = sh + 3;
	int side_y = fy + (fh - sh) / 2;
	int mid = (fw - sw) / 2;
	CANVAS_W = 2 * (sw + 2) + fw + 2;
	CANVAS_H = fh + sh + 4;
	PANES.push(Pane{fx, fy, fw, fh, 0, true});
	PANES.push(Pane{1, side_y, sw, sh, 3, true});
	PANES.push(Pane{fx + fw + 2, side_y, sw, sh, 1, true});
	PANES.push(Pane{fx + mid, 1, sw, sh, 2, true});
	int low_y = side_y + sh + 2;
	BOXES.push(Box{1, 1, sw, side_y - 3, BOX_LOG});
	BOXES.push(Box{sw + 3, 1, mid - 2, sh, BOX_MAP});
	BOXES.push(Box{fx + mid + sw + 2, 1, fw - sw - mid - 2, sh, BOX_COMPASS});
	BOXES.push(Box{fx + fw + 2, 1, sw, side_y - 3, BOX_HERO});
	BOXES.push(Box{1, low_y, sw, CANVAS_H - 1 - low_y, BOX_SIGHT});
	BOXES.push(Box{fx + fw + 2, low_y, sw, CANVAS_H - 1 - low_y, BOX_LOOT});
}

function build_layout() -> void {
	layout_panes(VIEW_LAYOUT, VIEW_W, VIEW_H);
	LAYOUT_KEY = layout_key();
	STRIDE = CANVAS_W;
	VPIX = [];
	for (int i = 0; i < CANVAS_W * CANVAS_H * 2; ++i) { VPIX.push(0); }
	ZBUF = [];
	for (int i = 0; i < VIEW_W; ++i) { ZBUF.push(0.0); }
	VROWS = [];
	for (int r = 0; r < CANVAS_H; ++r) { VROWS.push(r); }
	ROW_DIST = [];
	ROW_LIGHT = [];
	for (auto& pane : PANES) {
		pane.row_offset = ROW_DIST.size();
		int ph = pane.h * 2;
		float kpx = WALL_K * ph;
		for (int y = 0; y < ph; ++y) {
			float off = y + 0.5 - ph * 0.5;
			if (off < 0.0) { off = -off; }
			float d = kpx * 0.5 / off;
			ROW_DIST.push(d);
			ROW_LIGHT.push(lightf(d) * TORCH_LEVEL);
		}
	}
	BOX_ROWS = [];
	BOX_ROW_X = [];
	for (auto& box : BOXES) {
		box.first_row = BOX_ROWS.size();
		for (int r = 0; r < box.h; ++r) {
			BOX_ROWS.push(string::repeat(" ", box.w));
			BOX_ROW_X.push(box.x);
		}
	}
	BAYER = [];
	array<int> order = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];
	for (int o : order) { BAYER.push((o + 0.5) / 16.0); }
	VIEW_WORKERS = CANVAS_W >= 160 ? 1 : 0;
	CHROME_FLOOR = -1;
}

// ------------------------------------------------------------------ chrome ---
// Per canvas cell, CHROME_KIND: -1 pane pixel, 0 gap, 1 frame, 2 label, 3 box text (CHROME_REF:
// the pane, or the BOX_ROWS entry). Each canvas row is a list of segments: a prebuilt text run,
// a pixel run, or a box row.
array<int> CHROME_KIND = [];
array<int> CHROME_REF = [];
array<string> CHROME_CH = [];
array<int> SEG_START = [];  // per canvas row, its first segment; one extra entry closes the last row
array<int> SEG_KIND = [];   // 0 text, 1 pixel run, 2 box row
array<int> SEG_REF = [];    // a pixel run's pane; a box row's BOX_ROWS entry
array<int> SEG_X0 = [];
array<int> SEG_X1 = [];
array<string> SEG_TEXT = [];
int CHROME_FLOOR = -1;
string CHROME_LABELS = "";
string FRAME_SGR = "";
string LABEL_SGR = "";
string GAP_SGR = "";
int FRAME_RGB = 0;
int LABEL_RGB = 0;

function chrome_put(int x, int y, string ch, int kind, int ref) -> void {
	int i = y * CANVAS_W + x;
	CHROME_CH[i] = ch;
	CHROME_KIND[i] = kind;
	CHROME_REF[i] = ref;
}

function chrome_text(int cy, int x0, int x1) -> string {
	string s = GAP_SGR;
	int cur = 0;
	for (int x = x0; x < x1; ++x) {
		int i = cy * CANVAS_W + x;
		int k = CHROME_KIND[i];
		if (k != cur) {
			s += k == 1 ? FRAME_SGR : (k == 2 ? LABEL_SGR : GAP_SGR);
			cur = k;
		}
		s += CHROME_CH[i];
	}
	return s + RESET;
}

function frame_rect(int x0, int y0, int x1, int y1) -> void {
	for (int x = x0; x < x1; ++x) {
		chrome_put(x, y0 - 1, G_HLINE, 1, 0);
		chrome_put(x, y1, G_HLINE, 1, 0);
	}
	for (int y = y0; y < y1; ++y) {
		chrome_put(x0 - 1, y, G_VLINE, 1, 0);
		chrome_put(x1, y, G_VLINE, 1, 0);
	}
	chrome_put(x0 - 1, y0 - 1, G_TL, 1, 0);
	chrome_put(x1, y0 - 1, G_TR, 1, 0);
	chrome_put(x0 - 1, y1, G_BL, 1, 0);
	chrome_put(x1, y1, G_BR, 1, 0);
}

// a label in a top edge: "- NAME . W ---"
function put_label(int x0, int x1, int y, string text) -> void {
	for (int x = x0; x < x1; ++x) { chrome_put(x, y, G_HLINE, 1, 0); }
	array<string> cells = [" "];
	for (int c = 0; c < text.size(); ++c) { cells.push(text[c] == "|" ? G_DOT : text.substr(c, 1)); }
	cells.push(" ");
	for (int c = 0; c < cells.size() && x0 + 1 + c < x1 - 1; ++c) {
		chrome_put(x0 + 1 + c, y, cells[c], cells[c] == " " ? 1 : 2, 0);
	}
}

// a pane names itself and the heading it shows; the forward pane names what it holds
function pane_label(int p) -> string {
	if (p == 0 && OVERLAY_ON) { return OVERLAY_LABEL; }
	return `${PANE_NAMES[PANES[p].rot]} | ${FACE_LETTER[(G.facing + PANES[p].rot) % 4]}`;
}

function seg_kind(int chrome_kind) -> int { return chrome_kind < 0 ? 1 : (chrome_kind == 3 ? 2 : 0); }

function build_chrome_segments() -> void {
	SEG_START = [];
	SEG_KIND = [];
	SEG_REF = [];
	SEG_X0 = [];
	SEG_X1 = [];
	SEG_TEXT = [];
	for (int cy = 0; cy < CANVAS_H; ++cy) {
		SEG_START.push(SEG_KIND.size());
		int x = 0;
		while (x < CANVAS_W) {
			int i = cy * CANVAS_W + x;
			int kind = seg_kind(CHROME_KIND[i]);
			int ref = kind == 0 ? 0 : CHROME_REF[i];
			int x1 = x + 1;
			while (x1 < CANVAS_W && seg_kind(CHROME_KIND[i + x1 - x]) == kind && (kind == 0 || CHROME_REF[i + x1 - x] == ref)) { x1 += 1; }
			SEG_KIND.push(kind);
			SEG_REF.push(ref);
			SEG_X0.push(x);
			SEG_X1.push(x1);
			SEG_TEXT.push(kind == 0 ? chrome_text(cy, x, x1) : "");
			x = x1;
		}
	}
	SEG_START.push(SEG_KIND.size());
}

function labels_key() -> string { return `${G.facing}${OVERLAY_ON ? OVERLAY_LABEL : ""}`; }

function refresh_labels() -> void {
	CHROME_LABELS = labels_key();
	for (int p = 0; p < PANES.size(); ++p) {
		if (!PANES[p].framed) { continue; }
		put_label(PANES[p].x, PANES[p].x + PANES[p].w, PANES[p].y - 1, pane_label(p));
		int cy = PANES[p].y - 1;
		for (int s = SEG_START[cy]; s < SEG_START[cy + 1]; ++s) {
			if (SEG_KIND[s] == 0) { SEG_TEXT[s] = chrome_text(cy, SEG_X0[s], SEG_X1[s]); }
		}
	}
}

// the frames take the floor's colours: its wall tone dimmed, the labels in its glow
function build_chrome() -> void {
	CHROME_FLOOR = G.floor;
	Theme theme = theme_of(G.floor);
	FRAME_RGB = mix_rgb(scale_rgb(theme.wall, 0.55), INK_BACK, 0.25);
	LABEL_RGB = theme.glow;
	FRAME_SGR = sgr_fg(FRAME_RGB);
	LABEL_SGR = sgr_fg(LABEL_RGB);
	GAP_SGR = sgr_bg(INK_BACK);
	CHROME_KIND = [];
	CHROME_REF = [];
	CHROME_CH = [];
	for (int i = 0; i < CANVAS_W * CANVAS_H; ++i) {
		CHROME_KIND.push(0);
		CHROME_REF.push(0);
		CHROME_CH.push(" ");
	}
	for (int p = 0; p < PANES.size(); ++p) {
		Pane pane = PANES[p];
		for (int y = pane.y; y < pane.y + pane.h; ++y) {
			for (int x = pane.x; x < pane.x + pane.w; ++x) { chrome_put(x, y, "", -1, p); }
		}
		if (!pane.framed) { continue; }
		frame_rect(pane.x, pane.y, pane.x + pane.w, pane.y + pane.h);
		put_label(pane.x, pane.x + pane.w, pane.y - 1, pane_label(p));
	}
	for (auto box : BOXES) {
		for (int y = box.y; y < box.y + box.h; ++y) {
			for (int x = box.x; x < box.x + box.w; ++x) { chrome_put(x, y, " ", 3, box.first_row + y - box.y); }
		}
		frame_rect(box.x, box.y, box.x + box.w, box.y + box.h);
		put_label(box.x, box.x + box.w, box.y - 1, BOX_NAMES[box.id]);
	}
	CHROME_LABELS = labels_key();
	// the canvas under the text, for the dumps: frames and labels in their colours
	int frame_pal = pal(FRAME_RGB);
	int gap_pal = pal(INK_BACK);
	int panel_pal = pal(INK_PANEL);
	for (int i = 0; i < CANVAS_W * CANVAS_H; ++i) {
		int k = CHROME_KIND[i];
		if (k < 0) { continue; }
		int c = k == 1 || k == 2 ? frame_pal : (k == 3 ? panel_pal : gap_pal);
		int top = (i / CANVAS_W) * 2 * STRIDE + i % CANVAS_W;
		VPIX[top] = c;
		VPIX[top + STRIDE] = c;
	}
	build_chrome_segments();
}

// ----------------------------------------------------------------- camera ---
float CAM_DX = 0.0;
float CAM_DY = 0.0;
float CAM_PX = 0.0;
float CAM_PY = 0.0;
float CAM_POSX = 0.0;
float CAM_POSY = 0.0;
float CAM_INVDET = 0.0;
int PW = 0;                 // the pane being drawn: its size in pixels and first pixel
int PH = 0;
int PBASE = 0;
float WALL_KPX = 0.0;

// ------------------------------------------------------------- the caster ---
// Renders every pane from (posx, posy) looking along (dirx, diry), each pane turned its quarter
// turns from it; the step and turn animations pass the in-between cameras. The forward pane
// holding text is not cast. Returns CANVAS_H finished rows.
function render_view(float posx, float posy, float dirx, float diry) -> array<string> {
	if (LAYOUT_KEY != layout_key()) { build_layout(); }
	if (VC_FLOOR != G.floor) { build_view_cache(); }
	if (CHROME_FLOOR != G.floor) { build_chrome(); }
	if (CHROME_LABELS != labels_key()) { refresh_labels(); }
	for (int p = 0; p < PANES.size(); ++p) {
		int rot = PANES[p].rot;
		if (rot == 0) { if (!OVERLAY_ON) { render_pane(p, posx, posy, dirx, diry); } }
		else if (rot == 1) { render_pane(p, posx, posy, 0.0 - diry, dirx); }
		else if (rot == 2) { render_pane(p, posx, posy, 0.0 - dirx, 0.0 - diry); }
		else { render_pane(p, posx, posy, diry, 0.0 - dirx); }
	}
	if (VIEW_WORKERS != 0) { return parallel_transform(VROWS, view_row_half); }
	array<string> lines = [];
	for (int cy = 0; cy < CANVAS_H; ++cy) { lines.push(view_row_half(cy)); }
	return lines;
}

function render_pane(int p, float posx, float posy, float dirx, float diry) -> void {
	PW = PANES[p].w;
	PH = PANES[p].h * 2;
	PBASE = PANES[p].y * 2 * STRIDE + PANES[p].x;
	WALL_KPX = WALL_K * PH;
	float planex = 0.0 - diry * PLANE_LEN;
	float planey = dirx * PLANE_LEN;
	CAM_DX = dirx;
	CAM_DY = diry;
	CAM_PX = planex;
	CAM_PY = planey;
	CAM_POSX = posx;
	CAM_POSY = posy;
	int cellx = posx;
	int celly = posy;
	int pw = PW;
	int ph = PH;
	int stride = STRIDE;
	int base = PBASE;
	int roff = PANES[p].row_offset;
	float kpx = WALL_KPX;
	int turn = G.turn;
	int mapw = MAP_W;
	int maph = MAP_H;
	int wall_tile = TILE_WALL;
	int lres = LIGHT_RES;
	int lw = LIGHT_W;
	int lh = MAP_H * LIGHT_RES;
	PANE_LIGHT = PANES[p].rot == 0 ? 1.0 : SIDE_LIGHT;
	float pane_light = PANE_LIGHT;
	float flicker = 1.0 + 0.035 * ((turn * 7) % 3 - 1);
	var& pix = VPIX;
	var& ramps = RAMP;
	var& wtex = WTEX;
	var& ftex = FTEX;
	var& ctex = CTEX;
	var& bayer = BAYER;
	var& rowd = ROW_DIST;
	var& rowl = ROW_LIGHT;
	var& zbuf = ZBUF;
	var& wkind = WALL_KIND;
	var& fkind = FLOOR_KIND;
	var& light = LIGHT;
	var& tiles = G.tiles;

	for (int x = 0; x < pw; ++x) {
		float camx = 2.0 * x / (pw - 1.0) - 1.0;
		float vignette = flicker * pane_light * (1.0 - 0.2 * camx * camx);
		float rdx = dirx + planex * camx;
		float rdy = diry + planey * camx;
		int mx = cellx;
		int my = celly;
		float adx = rdx < 0.0 ? 0.0 - rdx : rdx;
		float ady = rdy < 0.0 ? 0.0 - rdy : rdy;
		float ddx = adx < 0.00000001 ? 100000000.0 : 1.0 / adx;
		float ddy = ady < 0.00000001 ? 100000000.0 : 1.0 / ady;
		int stepx = rdx < 0.0 ? -1 : 1;
		int stepy = rdy < 0.0 ? -1 : 1;
		float sdx = rdx < 0.0 ? (posx - mx) * ddx : (mx + 1.0 - posx) * ddx;
		float sdy = rdy < 0.0 ? (posy - my) * ddy : (my + 1.0 - posy) * ddy;
		int side = 0;
		bool hit = false;
		int guard = 0;
		while (!hit && guard < 96) {
			if (sdx < sdy) { sdx += ddx; mx += stepx; side = 0; }
			else { sdy += ddy; my += stepy; side = 1; }
			if (mx < 0 || my < 0 || mx >= mapw || my >= maph) { hit = true; }
			else if (tiles[my * mapw + mx] == wall_tile) { hit = true; }
			guard += 1;
		}
		float pdist = side == 0 ? sdx - ddx : sdy - ddy;
		pdist = pdist < 0.05 ? 0.05 : pdist;
		zbuf[x] = pdist;

		float wallx = side == 0 ? posy + pdist * rdy : posx + pdist * rdx;
		wallx = wallx - math::floor(wallx);
		int u = wallx * 32.0;
		if (u > 31) { u = 31; }
		if ((side == 0 && rdx > 0.0) || (side == 1 && rdy < 0.0)) { u = 31 - u; }
		int kind = 0;
		if (mx >= 0 && my >= 0 && mx < mapw && my < maph) { kind = wkind[my * mapw + mx]; }
		int tb = kind == 1 ? 1024 * (1 + (turn + mx + my) % 2) : (kind == 2 ? 3072 : 0);
		tb += u;

		// the face takes the brighter of your torch and the sconce light just in front of it
		int lineh = kpx / pdist;
		if (lineh < 1) { lineh = 1; }
		int ytop = ph / 2 - lineh / 2;
		int y0 = ytop < 0 ? 0 : ytop;
		int y1 = ytop + lineh > ph ? ph : ytop + lineh;
		float lf = lightf(pdist) * TORCH_LEVEL * vignette;
		int hlx = (posx + (pdist - 0.1) * rdx) * lres;
		int hly = (posy + (pdist - 0.1) * rdy) * lres;
		if (hlx >= 0 && hly >= 0 && hlx < lw && hly < lh) {
			float sconce = light[hly * lw + hlx] * pane_light;
			if (sconce > lf) { lf = sconce; }
		}
		lf *= side == 1 ? 0.72 : 1.0;
		if (lf > 15.0) { lf = 15.0; }
		int vstep = 2097152 / lineh;
		int v = (y0 - ytop) * vstep;
		int bx = x & 3;
		int cb = base + x;
		for (int y = y0; y < y1; ++y) {
			int l = lf + bayer[((y & 3) << 2) + bx];
			pix[y * stride + cb] = ramps[wtex[tb + ((v >> 16) & 31) * 32] + l];
			v += vstep;
		}

		// floor below and ceiling above, cast from the same ray, one world point per row pair
		int ystart = y1 < ph - y0 ? y1 : ph - y0;
		for (int y = ystart; y < ph; ++y) {
			float d = rowd[roff + y];
			float fx = posx + d * rdx;
			float fy = posy + d * rdy;
			int ix = fx;
			int iy = fy;
			int tu = (fx - ix) * 32.0;
			int tv = (fy - iy) * 32.0;
			int texel = (tv & 31) * 32 + (tu & 31);
			if (ix < 0) { ix = 0; } else if (ix >= mapw) { ix = mapw - 1; }
			if (iy < 0) { iy = 0; } else if (iy >= maph) { iy = maph - 1; }
			float fl = rowl[roff + y] * vignette;
			int flx = fx * lres;
			int fly = fy * lres;
			if (flx >= 0 && fly >= 0 && flx < lw && fly < lh) {
				float sconce = light[fly * lw + flx] * pane_light;
				if (sconce > fl) { fl = sconce; }
			}
			if (fl > 15.0) { fl = 15.0; }
			if (y >= y1) {
				int l = fl + bayer[((y & 3) << 2) + bx];
				pix[y * stride + cb] = ramps[ftex[fkind[iy * mapw + ix] + texel] + l];
			}
			int cy = ph - 1 - y;
			if (cy < y0) {
				int l = fl * 0.8 + bayer[((cy & 3) << 2) + bx];
				pix[cy * stride + cb] = ramps[ctex[texel] + l];
			}
		}
	}

	draw_sprites();
	if (p == 0) {
		draw_hands();
		if (G.last_dmg > 0) { draw_hurt(); }
	}
}

// One canvas row: pixel runs as half blocks between the chrome's text runs and the box rows.
// Also the parallel_transform body, so it reads the globals directly.
function view_row_half(int cy) -> string {
	string row = "";
	int stride = STRIDE;
	int ti = cy * 2 * stride;
	int bi = ti + stride;
	int s1 = SEG_START[cy + 1];
	for (int s = SEG_START[cy]; s < s1; ++s) {
		int kind = SEG_KIND[s];
		if (kind == 0) {
			row += SEG_TEXT[s];
			continue;
		}
		if (kind == 2) {
			row += BOX_ROWS[SEG_REF[s]];
			continue;
		}
		if (OVERLAY_ON && SEG_REF[s] == 0) {
			row += OVERLAY_ROWS[cy - PANES[0].y];
			continue;
		}
		int x1 = SEG_X1[s];
		if (ASCII_ONLY) {
			int bg = -1;
			for (int x = SEG_X0[s]; x < x1; ++x) {
				int c = pal_mean(VPIX[ti + x], VPIX[bi + x]);
				if (c != bg) { row += VPAL_BG[c]; bg = c; }
				row += " ";
			}
			row += RESET;
			continue;
		}
		int fg = -1;
		int bg = -1;
		for (int x = SEG_X0[s]; x < x1; ++x) {
			int top = VPIX[ti + x];
			int bot = VPIX[bi + x];
			if (top != fg) { row += VPAL_FG[top]; fg = top; }
			if (bot != bg) { row += VPAL_BG[bot]; bg = bot; }
			row += G_HALF;
		}
		row += RESET;
	}
	return row;
}

// ---------------------------------------------------------------- sprites ---
class Sprite { float depth; float sx; int art; int rgb; float height; float lift; int cap; int hp; bool flash; float lit; }

array<Sprite> SPRITES = [];
map<int, int> SPRITE_RAMPS = {};

function monster_sizef(var& m) -> float { return m.behavior == "boss" ? 1.4 : 0.5 + m.tier * 0.08; }

function body_ramps(int rgb) -> int {
	if (SPRITE_RAMPS.has(rgb)) { return SPRITE_RAMPS[rgb]; }
	int packed = ramp(rgb) | (ramp(scale_rgb(rgb, 0.55)) << 20) | (ramp(mix_rgb(rgb, 0xFFFFFF, 0.45)) << 40);
	SPRITE_RAMPS[rgb] = packed;
	return packed;
}

function sprite_add(int wx, int wy, int art_id, int rgb, float height, float lift, int cap, int hp, bool flash) -> void {
	float rx = wx + 0.5 - CAM_POSX;
	float ry = wy + 0.5 - CAM_POSY;
	float depth = CAM_INVDET * (0.0 - CAM_PY * rx + CAM_PX * ry);
	if (depth < 0.12 || depth > SPRITE_RANGE) { return; }
	float ttx = CAM_INVDET * (CAM_DY * rx - CAM_DX * ry);
	float sx = (PW / 2.0) * (1.0 + ttx / depth);
	if (sx < -PW || sx > PW * 2) { return; }
	float lit = lightf(depth) * TORCH_LEVEL * PANE_LIGHT;
	float sconce = light_at(wx + 0.5, wy + 0.5) * PANE_LIGHT;
	SPRITES.push(Sprite{depth, sx, art_id, rgb, height, lift, cap, hp, flash, sconce > lit ? sconce : lit});
}

function monster_art(var& m) -> int {
	if (m.glyph == "?") { return ART_ID["chest_open"]; }
	return ART_ID.has(m.glyph) ? ART_ID[m.glyph] : ART_ID["g"];
}

function draw_sprites() -> void {
	float det = CAM_PX * CAM_DY - CAM_DX * CAM_PY;
	if (det < 0.0001 && det > -0.0001) { return; }
	CAM_INVDET = 1.0 / det;
	SPRITES = [];
	int prop_cap = PH * 2 / 5;
	int stairs = ART_ID["stairs"];
	int statue = ART_ID["statue"];
	for (int i = 0; i < LM_X.size(); ++i) {
		int a = LM_ART[i];
		float height = a == stairs ? 0.42 : (a == statue ? 0.98 : 0.9);
		int rgb = a == stairs ? 0x78FFFF : (a == statue ? 0xFFDC60 : 0xFF82E1);
		sprite_add(LM_X[i], LM_Y[i], a, rgb, height, 0.0, a == stairs ? prop_cap : PH, -1, false);
	}
	for (auto& it : G.ground) {
		int a = ART_ID.has(it.glyph) ? ART_ID[it.glyph] : ART_ID["~"];
		sprite_add(it.x, it.y, a, it.ink, 0.3, 0.0, prop_cap, -1, false);
	}
	for (auto& m : G.monsters) {
		if (!m.alive) { continue; }
		if (m.disguised) {
			sprite_add(m.x, m.y, ART_ID["chest"], 0xC8A03C, 0.36, 0.0, prop_cap, -1, false);   // loot, until it is not
			continue;
		}
		float lift = 0.0;
		if (m.glyph == "b" || m.glyph == "m" || m.glyph == "w") { lift = 0.32 + 0.05 * ((G.turn + m.id) % 2); }
		int hp = m.hp < m.max_hp ? (m.hp * 1000) / imax(1, m.max_hp) : -1;
		bool flash = VIEW_HITS.has(m.id) && VIEW_HITS[m.id] + 1 >= G.turn;
		sprite_add(m.x, m.y, monster_art(m), m.ink, 1.15 * monster_sizef(m), lift, PH + 12, hp, flash);
	}

	// painter's order, far to near: an insertion sort of a handful
	int n = SPRITES.size();
	array<int> order = [];
	for (int i = 0; i < n; ++i) {
		int j = order.size();
		order.push(i);
		while (j > 0 && SPRITES[order[j - 1]].depth < SPRITES[i].depth) {
			order[j] = order[j - 1];
			j -= 1;
		}
		order[j] = i;
	}
	for (int i : order) {
		Sprite sp = SPRITES[i];
		int hgt = iclamp(WALL_KPX * sp.height / sp.depth, 1, sp.cap);
		int wdt = imax(1, hgt * ART_W[sp.art] * SPRITE_ASPECT / ART_H[sp.art]);
		int bot = PH * 0.5 + WALL_KPX * (0.5 - sp.lift) / sp.depth;
		int x0 = math::floor(sp.sx - wdt * 0.5);
		draw_art(sp.art, sp.rgb, x0, bot - hgt, wdt, hgt, sp.lit + 0.5, sp.depth, sp.flash);
		if (sp.hp >= 0) { draw_hp_bar(x0, bot - hgt - 2, wdt, sp.hp, sp.depth); }
	}
}

function draw_hp_bar(int x0, int y, int w, int per_mille, float depth) -> void {
	if (y < 0 || y >= PH) { return; }
	int bw = imax(3, imin(w, PW / 4));
	int bx = x0 + (w - bw) / 2;
	int full = (bw * per_mille + 999) / 1000;
	for (int x = imax(0, bx); x < imin(PW, bx + bw); ++x) {
		if (depth >= ZBUF[x]) { continue; }
		VPIX[PBASE + y * STRIDE + x] = x - bx < full ? BAR_GOOD : BAR_BAD;
	}
}

// Scales art a into the w x h rect at (x0, y0) at light level lvl; depth > 0 tests the
// z-buffer, and flash blanches every opaque texel toward white (a monster that just took a hit).
function draw_art(int a, int rgb, int x0, int y0, int w, int h, int lvl, float depth, bool flash) -> void {
	array<int> tab = flash ? FLASH_TAB : ROLE_RAMP;
	int packed = flash ? body_ramps(mix_rgb(rgb, 0xFFFFFF, 0.6)) : body_ramps(rgb);
	tab[1] = packed & 0xFFFFF;
	tab[2] = (packed >> 20) & 0xFFFFF;
	tab[3] = (packed >> 40) & 0xFFFFF;
	if (flash) { lvl = 15; }
	lvl = iclamp(lvl, 0, 15);
	int aw = ART_W[a];
	int ah = ART_H[a];
	int off = ART_OFF[a];
	int sx0 = imax(0, x0);
	int sx1 = imin(PW, x0 + w);
	int sy0 = imax(0, y0);
	int sy1 = imin(PH, y0 + h);
	int stride = STRIDE;
	int base = PBASE;
	var& pix = VPIX;
	var& ramps = RAMP;
	var& texels = ART_TEX;
	for (int sx = sx0; sx < sx1; ++sx) {
		if (depth > 0.0 && depth >= ZBUF[sx]) { continue; }
		int col = off + ((sx - x0) * aw) / w;
		int cb = base + sx;
		for (int sy = sy0; sy < sy1; ++sy) {
			int code = texels[col + (((sy - y0) * ah) / h) * aw];
			if (code >= 0) { pix[sy * stride + cb] = ramps[tab[code] + lvl]; }
		}
	}
}

// the torch in your off hand (its flame flickers turn to turn) and, once you wield one, your blade
function draw_hands() -> void {
	int torch = ART_ID[G.turn % 2 == 0 ? "torch_a" : "torch_b"];
	int th = PH * 11 / 20;
	int tw = th * ART_W[torch] / ART_H[torch];
	draw_art(torch, 0x5A3C8C, PW / 20, PH - th + HAND_BOB, tw, th, 14, 0.0, false);
	if (G.player.weapon != null) {
		int blade = ART_ID["blade"];
		int bh = PH * 3 / 5;
		int bw = bh * ART_W[blade] / ART_H[blade];
		draw_art(blade, 0x5A3C8C, PW - bw - PW / 14, PH - bh + HAND_BOB, bw, bh, 13, 0.0, false);
	}
}

// fresh damage: a ragged red rim around the view
function draw_hurt() -> void {
	int c = RAMP[HURT_RAMP];
	for (int y = 0; y < PH; ++y) {
		for (int x = 0; x < PW; ++x) {
			int e = imin(imin(x, PW - 1 - x), imin(y, PH - 1 - y));
			if (e == 0 || (e == 1 && (x + y) % 2 == 0) || (e == 2 && (x * 3 + y) % 5 == 0)) { VPIX[PBASE + y * STRIDE + x] = c; }
		}
	}
}
