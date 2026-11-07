// Data: weapons, bestiary, pickup tables, and every sprite as pixel art.
// Art format: rows of chars, '.' = transparent, everything else indexes the
// art's own tiny rgb palette. parse_arts() flattens each into code arrays at
// boot; the renderer maps codes -> palette indices via per-(art, shade, flash) LUTs.

// ---------------------------------------------------------------- weapons ---
// kind: 0 hitscan, 1 projectile
var WEAPONS = [
	{"name": "PISTOL",    "ammo": 0, "cd": 11, "pellets": 1, "spread": 0.012, "lo": 6,  "hi": 12, "kind": 0},
	{"name": "SCATTERGUN","ammo": 1, "cd": 28, "pellets": 6, "spread": 0.11,  "lo": 3,  "hi": 7,  "kind": 0},
	{"name": "HEXCASTER", "ammo": 2, "cd": 22, "pellets": 1, "spread": 0.0,   "lo": 24, "hi": 40, "kind": 1}
];
array<string> AMMO_NAMES = ["BULLET", "SHELL", "CELL"];
array<int> AMMO_MAX = [200, 50, 120];

// ---------------------------------------------------------------- bestiary --
// kind indexes: 0 grunt (melee rusher), 1 spitter (ranged), 2 turret, 3 warden (boss)
var ENEMY_DEFS = [
	{"name": "GRUNT",   "hp": 30,  "speed": 3.1, "radius": 0.34, "pain": 55, "size": 0.72, "melee_lo": 4, "melee_hi": 10},
	{"name": "SPITTER", "hp": 45,  "speed": 2.2, "radius": 0.36, "pain": 40, "size": 0.78, "melee_lo": 3, "melee_hi": 7},
	{"name": "TURRET",  "hp": 60,  "speed": 0.0, "radius": 0.38, "pain": 15, "size": 0.60, "melee_lo": 0, "melee_hi": 0},
	{"name": "WARDEN",  "hp": 520, "speed": 2.0, "radius": 0.45, "pain": 8,  "size": 1.05, "melee_lo": 10, "melee_hi": 22}
];

// ---------------------------------------------------------------- pickups ---
// kind: 0 stim 1 medkit 2 armor 3 clip 4 shells 5 cells 6 redkey 7 bluekey
//       8 scattergun 9 hexcaster 10 soul orb
var ITEM_DEFS = [
	{"name": "stimpack",    "art": "it_stim",   "size": 0.22},
	{"name": "medikit",     "art": "it_medkit", "size": 0.28},
	{"name": "flak vest",   "art": "it_armor",  "size": 0.30},
	{"name": "bullet clip", "art": "it_clip",   "size": 0.20},
	{"name": "shell box",   "art": "it_shell",  "size": 0.22},
	{"name": "hex cells",   "art": "it_cell",   "size": 0.24},
	{"name": "RED keycard", "art": "it_rkey",   "size": 0.22},
	{"name": "BLUE keycard","art": "it_bkey",   "size": 0.22},
	{"name": "SCATTERGUN",  "art": "it_sgun",   "size": 0.26},
	{"name": "HEXCASTER",   "art": "it_hgun",   "size": 0.28},
	{"name": "soul orb",    "art": "it_orb",    "size": 0.30}
];

// a placed pickup in the world (per-frame billboard/automap/pickup scans)
class Item {
public:
	int kind = 0;
	float x = 0.0;
	float y = 0.0;
	bool taken = false;

	Item(int kind_, float x_, float y_) {
		kind = kind_;
		x = x_;
		y = y_;
	}
}

// ------------------------------------------------------------- sprite art ---
// Each entry: rows + a tiny palette. Kept chunky on purpose: at half-block
// resolution a 14-wide sprite fills a third of the screen point-blank.
var ART_SRC = {
	// GRUNT - a hunched green rusher, all shoulders and appetite
	"grunt_w0": {"pal": {"h": [86, 158, 62], "d": [52, 104, 40], "E": [255, 60, 40], "m": [240, 226, 190], "c": [220, 210, 170], "l": [46, 70, 36]},
		"rows": [
		"....hhhhh.....",
		"...hhhhhhh....",
		"..hhEhhhEhh...",
		"..hhhhhhhhh...",
		"...hmmmmmh....",
		"...hcmcmch....",
		"dhhhhhhhhhhd..",
		"dhhhhhhhhhhd..",
		"chdhhhhhhdhc..",
		"..dhhhhhhd....",
		"..dhh..hhd....",
		"..lhh..hhl....",
		"..ll....ll....",
		".lll....lll..."]},
	"grunt_w1": {"pal": {"h": [86, 158, 62], "d": [52, 104, 40], "E": [255, 60, 40], "m": [240, 226, 190], "c": [220, 210, 170], "l": [46, 70, 36]},
		"rows": [
		"....hhhhh.....",
		"...hhhhhhh....",
		"..hhEhhhEhh...",
		"..hhhhhhhhh...",
		"...hmmmmmh....",
		"...hcmcmch....",
		".dhhhhhhhhhd..",
		".dhhhhhhhhhd..",
		".chdhhhhhdhc..",
		"...dhhhhhd....",
		"...dhh.hhd....",
		"...lhh..hhl...",
		"..ll......ll..",
		"..lll....lll.."]},
	"grunt_at": {"pal": {"h": [96, 170, 66], "d": [52, 104, 40], "E": [255, 220, 60], "m": [180, 30, 24], "c": [235, 226, 190], "l": [46, 70, 36]},
		"rows": [
		"....hhhhh.....",
		"...hhhhhhh....",
		"..hhEhhhEhh...",
		"..hhhhhhhhh...",
		"...hmmmmmh....",
		"...cmmmmmc....",
		"chhhhhhhhhhhc.",
		".chhhhhhhhhc..",
		".hdhhhhhhdh...",
		"..dhhhhhhd....",
		"..dhh..hhd....",
		"..lhh..hhl....",
		"..ll....ll....",
		".lll....lll..."]},
	"grunt_d0": {"pal": {"h": [86, 148, 62], "d": [52, 96, 40], "E": [90, 30, 24], "r": [190, 40, 30], "l": [46, 70, 36]},
		"rows": [
		"..............",
		"....hhhhh.....",
		"...hhhhhhh....",
		"..hhEhhhEhh...",
		"..rhhhhhhhr...",
		"...rhhhhhr....",
		".dhhhhhhhhhd..",
		".rdhhhhhhhdr..",
		"..dhhhhhhd....",
		"..rhhhhhhr....",
		"...hh..hh.....",
		"...lh..hl.....",
		"..............",
		".............."]},
	"grunt_d1": {"pal": {"h": [74, 120, 56], "d": [48, 82, 38], "r": [180, 38, 28], "b": [120, 26, 20]},
		"rows": [
		"..............",
		"..............",
		"..............",
		"....hhhh......",
		"...hhhhhhr....",
		"..rhhhhhhhr...",
		".rdhhhhhhdr...",
		"..brhhhhrb....",
		"...rbhhbr.....",
		"....rbbr......",
		"..............",
		"..............",
		"..............",
		".............."]},
	"grunt_dead": {"pal": {"h": [64, 100, 50], "r": [150, 34, 26], "b": [96, 22, 18]},
		"rows": [
		"..............",
		"..............",
		"..............",
		"..............",
		"..............",
		"..............",
		"..............",
		"..............",
		"....rbbr......",
		"..rbhhhhbr....",
		".rbhhhhhhbr...",
		"..brrbbrrb....",
		"..............",
		".............."]},

	// SPITTER - stilt-legged violet thing with a glowing green gullet
	"spit_w0": {"pal": {"v": [148, 74, 190], "d": [96, 46, 128], "E": [255, 210, 70], "g": [110, 250, 90], "l": [70, 40, 90]},
		"rows": [
		"....vvvvvv....",
		"...vvvvvvvv...",
		"..vvEvvvvEvv..",
		"..vvvvvvvvvv..",
		"...vvggggvv...",
		"...vvvggvvv...",
		"..dvvvvvvvvd..",
		"..dvvvvvvvvd..",
		"...dvvvvvvd...",
		"....l.vv.l....",
		"...l..vv..l...",
		"...l..ll..l...",
		"..l...ll...l..",
		"..l...ll...l.."]},
	"spit_w1": {"pal": {"v": [148, 74, 190], "d": [96, 46, 128], "E": [255, 210, 70], "g": [110, 250, 90], "l": [70, 40, 90]},
		"rows": [
		"....vvvvvv....",
		"...vvvvvvvv...",
		"..vvEvvvvEvv..",
		"..vvvvvvvvvv..",
		"...vvggggvv...",
		"...vvvggvvv...",
		"..dvvvvvvvvd..",
		"..dvvvvvvvvd..",
		"...dvvvvvvd...",
		"....l.vv.l....",
		"....l.vv.l....",
		"...l..ll...l..",
		"...l..ll...l..",
		"..l...ll....l."]},
	"spit_at": {"pal": {"v": [162, 84, 205], "d": [96, 46, 128], "E": [255, 240, 120], "g": [160, 255, 120], "G": [230, 255, 190], "l": [70, 40, 90]},
		"rows": [
		"....vvvvvv....",
		"...vvvvvvvv...",
		"..vvEvvvvEvv..",
		"..vvggggggvv..",
		"..vggGGGGggv..",
		"..vvgGGGGgvv..",
		".dvvvggggvvvd.",
		"..dvvvvvvvvd..",
		"...dvvvvvvd...",
		"....l.vv.l....",
		"...l..vv..l...",
		"...l..ll..l...",
		"..l...ll...l..",
		"..l...ll...l.."]},
	"spit_d0": {"pal": {"v": [140, 70, 178], "d": [92, 44, 120], "g": [110, 220, 90], "r": [186, 44, 34], "l": [70, 40, 90]},
		"rows": [
		"..............",
		"....vvvvvv....",
		"...vvvvvvvv...",
		"..vvrvvvvrvv..",
		"...vvggggvv...",
		"..rvvvvvvvvr..",
		"..dvvvvvvvvd..",
		"...dvvvvvvd...",
		"....rvvvvr....",
		".....l..l.....",
		"....l....l....",
		"..............",
		"..............",
		".............."]},
	"spit_d1": {"pal": {"v": [118, 60, 150], "g": [96, 190, 80], "r": [176, 40, 30], "b": [110, 26, 22]},
		"rows": [
		"..............",
		"..............",
		"..............",
		"....vvvvv.....",
		"...vvvvvvvr...",
		"..rvvggvvvr...",
		"...rvvvvvr....",
		"....rbvbr.....",
		".....rbr......",
		"..............",
		"..............",
		"..............",
		"..............",
		".............."]},
	"spit_dead": {"pal": {"v": [96, 50, 122], "g": [80, 150, 70], "b": [96, 24, 20], "r": [150, 34, 26]},
		"rows": [
		"..............",
		"..............",
		"..............",
		"..............",
		"..............",
		"..............",
		"..............",
		"..............",
		"....rbbvr.....",
		"..rvvbgbvvr...",
		"..brvvvvvrb...",
		"...bbrrbb.....",
		"..............",
		".............."]},

	// TURRET - a wall-fed gun pod; wakes up mean
	"tur_idle": {"pal": {"s": [130, 134, 142], "d": [78, 82, 92], "k": [52, 54, 62], "r": [120, 30, 30]},
		"rows": [
		"..............",
		"....ssssss....",
		"...sddddddss..",
		"..sdkkkkkkds..",
		"..sdkkrrkkds..",
		"..sdkkkkkkds..",
		"...sddddddss..",
		"....ssssss....",
		".....kddk.....",
		".....kddk.....",
		"....kkddkk....",
		"...kkkkkkkk...",
		"..............",
		".............."]},
	"tur_fire": {"pal": {"s": [150, 154, 162], "d": [88, 92, 102], "k": [52, 54, 62], "r": [255, 80, 50], "y": [255, 230, 120]},
		"rows": [
		"..............",
		"....ssssss....",
		"...sddddddss..",
		"..sdkkkkkkds..",
		"..sdkryyrkds..",
		"..sdkkkkkkds..",
		"...sddddddss..",
		"....ssssss....",
		".....kddk.....",
		".....kddk.....",
		"....kkddkk....",
		"...kkkkkkkk...",
		"..............",
		".............."]},
	"tur_d0": {"pal": {"s": [120, 122, 130], "d": [72, 74, 84], "k": [48, 50, 58], "y": [255, 200, 90], "r": [230, 90, 40]},
		"rows": [
		"..............",
		"..y.ssssss.y..",
		"...sdyddydss..",
		"..sdkkrrkkds..",
		"..sdryyyyrds..",
		"..sdkkrrkkds..",
		"...sdyddydss..",
		"..y.ssssss.y..",
		".....kddk.....",
		".....kddk.....",
		"....kkddkk....",
		"...kkkkkkkk...",
		"..............",
		".............."]},
	"tur_d1": {"pal": {"s": [104, 106, 114], "k": [46, 48, 56], "y": [220, 160, 70], "r": [180, 70, 34]},
		"rows": [
		"..............",
		"..............",
		"..............",
		"...r.ssss.r...",
		"....skkyks....",
		"...skyrrks....",
		"....skkkks....",
		".....ssss.....",
		".....kkkk.....",
		"....kkkkkk....",
		"...kkkkkkkk...",
		"..............",
		"..............",
		".............."]},
	"tur_dead": {"pal": {"s": [88, 90, 98], "k": [42, 44, 52], "r": [120, 50, 30]},
		"rows": [
		"..............",
		"..............",
		"..............",
		"..............",
		"..............",
		"..............",
		"..............",
		"......ss......",
		"....skkrks....",
		"...skkkkkks...",
		"...kkkkkkkk...",
		"..kkkkkkkkkk..",
		"..............",
		".............."]},

	// WARDEN - the episode's landlord. Big, horned, unhappy.
	"war_w0": {"pal": {"r": [190, 44, 36], "d": [126, 30, 26], "k": [64, 20, 18], "E": [255, 230, 90], "h": [230, 220, 200], "f": [255, 140, 40]},
		"rows": [
		"..h..........h..",
		".hh...rrrr...hh.",
		".h..rrrrrrrr..h.",
		".h.rrErrrrErr.h.",
		"...rrrrrrrrrr...",
		"....rffffffr....",
		"....rrffffrr....",
		"..drrrrrrrrrrd..",
		".ddrrrrrrrrrrdd.",
		".kdrrrrrrrrrrdk.",
		".kddrrrrrrrrddk.",
		"..kdrrrrrrrrdk..",
		"...ddrr..rrdd...",
		"...kdrr..rrdk...",
		"...kdd....ddk...",
		"..kkd......dkk..",
		"..kkk......kkk..",
		".kkk........kkk."]},
	"war_w1": {"pal": {"r": [190, 44, 36], "d": [126, 30, 26], "k": [64, 20, 18], "E": [255, 230, 90], "h": [230, 220, 200], "f": [255, 140, 40]},
		"rows": [
		"..h..........h..",
		".hh...rrrr...hh.",
		".h..rrrrrrrr..h.",
		".h.rrErrrrErr.h.",
		"...rrrrrrrrrr...",
		"....rffffffr....",
		"....rrffffrr....",
		"..drrrrrrrrrrd..",
		".ddrrrrrrrrrrdd.",
		".kdrrrrrrrrrrdk.",
		".kddrrrrrrrrddk.",
		"..kdrrrrrrrrdk..",
		"...ddrr..rrdd...",
		"...kdrr..rrdk...",
		"...dd......kk...",
		"..kdd......dkk..",
		"..kk........kk..",
		".kkk........kkkk"]},
	"war_at": {"pal": {"r": [210, 50, 40], "d": [126, 30, 26], "k": [64, 20, 18], "E": [255, 255, 160], "h": [235, 226, 205], "f": [255, 200, 80], "F": [255, 250, 200]},
		"rows": [
		"..h..........h..",
		".hh...rrrr...hh.",
		".h..rrrrrrrr..h.",
		".h.rrErrrrErr.h.",
		"...rrrrrrrrrr...",
		"....rffFFffr....",
		"....rfFFFFfr....",
		"..drrrfFFfrrrd..",
		".ddrrrrffrrrrdd.",
		".kdrrrrrrrrrrdk.",
		".kddrrrrrrrrddk.",
		"..kdrrrrrrrrdk..",
		"...ddrr..rrdd...",
		"...kdrr..rrdk...",
		"...kdd....ddk...",
		"..kkd......dkk..",
		"..kkk......kkk..",
		".kkk........kkk."]},
	"war_d0": {"pal": {"r": [176, 42, 34], "d": [118, 28, 24], "k": [60, 18, 16], "h": [215, 206, 188], "b": [255, 90, 40]},
		"rows": [
		"................",
		"..h..........h..",
		".hh...rrrr...hh.",
		".h..rrbrrbrr..h.",
		"...rrrrrrrrrr...",
		"....rbbbbbbr....",
		"..drrrrrrrrrrd..",
		".bdrrrrrrrrrrdb.",
		".kdrrrbbrrrrrdk.",
		"..kddrrrrrrddk..",
		"...kdrrrrrrdk...",
		"...ddbr..rbdd...",
		"....dd....dd....",
		"....k......k....",
		"................",
		"................",
		"................",
		"................"]},
	"war_d1": {"pal": {"r": [150, 36, 30], "d": [104, 26, 22], "k": [54, 16, 14], "b": [230, 80, 36], "y": [255, 170, 70]},
		"rows": [
		"................",
		"................",
		"................",
		"................",
		"...y..rrrr..y...",
		"..brrrrrrrrrb...",
		"..rbrybbyrbrr...",
		".drrrrbbrrrrrd..",
		".kdrrrrrrrrrdk..",
		"..kdbrrrrrbdk...",
		"...kddrrrddk....",
		"....kbd..dbk....",
		"................",
		"................",
		"................",
		"................",
		"................",
		"................"]},
	"war_dead": {"pal": {"r": [110, 28, 24], "k": [50, 14, 12], "b": [150, 50, 26], "h": [170, 160, 145]},
		"rows": [
		"................",
		"................",
		"................",
		"................",
		"................",
		"................",
		"................",
		"................",
		"................",
		"................",
		"................",
		"..h..........h..",
		"...bkrrrrrrkb...",
		"..brrkkkkkkrrb..",
		".bkrrrrrrrrrrkb.",
		"..bbkkrrrrkkbb..",
		"................",
		"................"]},

	// pickups (small props; feet on the floor line)
	"it_stim": {"pal": {"w": [230, 235, 240], "b": [70, 120, 230], "r": [220, 60, 50]},
		"rows": [
		"..bb..",
		".wwww.",
		".wrrw.",
		".wrrw.",
		".wwww."]},
	"it_medkit": {"pal": {"w": [235, 238, 240], "r": [225, 50, 40], "d": [150, 152, 158]},
		"rows": [
		"wwwwwwww",
		"wwwrrwww",
		"wrrrrrrw",
		"wwwrrwww",
		"dwwwwwwd"]},
	"it_armor": {"pal": {"g": [70, 190, 90], "d": [40, 110, 55], "k": [26, 60, 34]},
		"rows": [
		"gg...gg",
		"ggggggg",
		"gdddddg",
		"gddkddg",
		".ddddd.",
		".dd.dd."]},
	"it_clip": {"pal": {"b": [200, 170, 70], "d": [140, 116, 50], "k": [90, 76, 36]},
		"rows": [
		".bbb.",
		".bbb.",
		"ddddd",
		"dkkkd",
		"ddddd"]},
	"it_shell": {"pal": {"r": [210, 70, 40], "y": [235, 200, 90], "k": [110, 50, 26]},
		"rows": [
		"ryryryr",
		"ryryryr",
		"kkkkkkk",
		"ryryryr",
		"kkkkkkk"]},
	"it_cell": {"pal": {"c": [90, 240, 220], "d": [40, 140, 130], "w": [220, 255, 250]},
		"rows": [
		".cddc.",
		"cwccdc",
		"cccwcc",
		"dccccd",
		".dccd."]},
	"it_rkey": {"pal": {"r": [255, 70, 60], "w": [255, 200, 195]},
		"rows": [
		"rrr..",
		"rwr..",
		"rrrrr",
		"..r.r"]},
	"it_bkey": {"pal": {"b": [80, 130, 255], "w": [200, 215, 255]},
		"rows": [
		"bbb..",
		"bwb..",
		"bbbbb",
		"..b.b"]},
	"it_sgun": {"pal": {"k": [70, 62, 54], "w": [150, 120, 80], "s": [180, 184, 190]},
		"rows": [
		"ssssssssk.",
		"kssssssskk",
		"...wwwkkk.",
		"..www.....",
		".ww......."]},
	"it_hgun": {"pal": {"p": [180, 90, 240], "d": [110, 50, 160], "c": [120, 255, 140]},
		"rows": [
		".pppppp.c.",
		"ppcccppppc",
		"dppppppdd.",
		"..ddd.....",
		".dd......."]},
	"it_orb": {"pal": {"b": [110, 160, 255], "w": [235, 245, 255], "d": [60, 90, 190]},
		"rows": [
		".bbbb.",
		"bwwbbb",
		"bwbbbd",
		"bbbbdd",
		".bddd."]},

	// projectiles
	"pr_spit": {"pal": {"g": [130, 255, 100], "w": [220, 255, 190]},
		"rows": [
		".gg.",
		"gwwg",
		"gwwg",
		".gg."]},
	"pr_hex": {"pal": {"p": [200, 110, 255], "w": [245, 225, 255], "c": [130, 255, 150]},
		"rows": [
		".pp.",
		"pwcp",
		"pcwp",
		".pp."]},
	"pr_fire": {"pal": {"o": [255, 140, 40], "y": [255, 230, 120], "r": [230, 60, 30]},
		"rows": [
		".oo.",
		"oyyo",
		"ryyr",
		".rr."]}
};

// parsed at boot: name -> {w, h, pix (flat codes, 0 transparent), rgb ([[r,g,b]...])}
var ARTS = {};

function parse_arts() -> void {
	ARTS = {};
	for (auto kv : ART_SRC) { ARTS[kv.first] = parse_art(kv.second); }
}

// one art: {"w", "h", "pix": codes row-major, "rgb": color per code, "keys": palette char per code}
function parse_art(var src) -> var {
	var rows = src["rows"];
	var pal = src["pal"];
	map<string, int> codes = {}; // char string -> code
	var rgb = [[0, 0, 0]];
	array<string> keys = ["."];
	int next_code = 1;
	int h = rows.size();
	int w = rows[0].size();
	array<int> pix = [];
	for (int y = 0; y < h; ++y) {
		string row = rows[y];
		for (int x = 0; x < w; ++x) {
			string ch = row.substr(x, 1);
			if (ch == ".") { pix.push(0); continue; }
			if (!codes.has(ch)) {
				codes[ch] = next_code;
				next_code = next_code + 1;
				rgb.push(pal[ch]);
				keys.push(ch);
			}
			pix.push(codes[ch]);
		}
	}
	return {"w": w, "h": h, "pix": pix, "rgb": rgb, "keys": keys};
}

// ------------------------------------------------------- weapon HUD art -----
// 5 rows each, ASCII, drawn at the HUD's right edge with a walk-bob shift.
var WGFX_IDLE = [
	[
	"      ____   ",
	"  ___|__  |  ",
	" |__    | |  ",
	"    |___|_|  ",
	"      |_|    "],
	[
	" ________    ",
	" \\_______\\==.",
	"  |__ __--'| ",
	"    |_|----' ",
	"    |_|      "],
	[
	"   .-<*>-.   ",
	"  / _/|\\_ \\  ",
	" |=( (#) )=| ",
	"  \\__\\|/__/  ",
	"     |=|     "]
];
var WGFX_FIRE = [
	[
	" \\!/  ____   ",
	" -*-_|__  |  ",
	" |__    | |  ",
	"    |___|_|  ",
	"      |_|    "],
	[
	"\\\\!//____    ",
	"=*=______\\==.",
	"//|\\_ __--'| ",
	"    |_|----' ",
	"    |_|      "],
	[
	" *~.-<*>-.~* ",
	"  ~/ _|_ \\~  ",
	" |=( (@) )=| ",
	"  \\__\\|/__/  ",
	"     |=|     "]
];
