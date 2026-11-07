// The showcase look's first-person weapon: the gun in hand at the bottom of
// the view, swaying with the walk, kicking on every shot, lit by the floor's
// torchlight and by its own muzzle flash. Art uses the sprite format of
// defs.jai; uppercase palette keys glow.

var VIEWMODEL_SRC = {
	"vm_pistol": {"pal": {"k": [22, 20, 26], "m": [84, 88, 98], "l": [150, 156, 168], "s": [214, 160, 120],
			"S": [150, 100, 74], "c": [70, 74, 54], "C": [44, 46, 34]},
		"rows": [
		"....................",
		"....................",
		"....................",
		"....................",
		"....................",
		"....................",
		"........kkkk........",
		"........kllk........",
		".......kmllmk.......",
		".......kmllmk.......",
		"......kmmllmmk......",
		"......kmmllmmk......",
		"......kmmmmmmk......",
		".....kkmmmmmmkk.....",
		".....kmkkmmkkmk.....",
		".....kmmmmmmmmk.....",
		"....kssmmmmmmssk....",
		"...kssskkkkkksssk...",
		"...ksSsssssssSssk...",
		"..kssSssssssssSssk..",
		"..kcsSSssssssSSsck..",
		".kccsSSSSSSSSSSccck.",
		".kcCcccccccccccCcck.",
		"kccCccccccccccccCcck"]},
	"vm_pistol_fire": {"pal": {"k": [22, 20, 26], "m": [84, 88, 98], "l": [150, 156, 168], "s": [214, 160, 120],
			"S": [150, 100, 74], "c": [70, 74, 54], "C": [44, 46, 34], "F": [255, 250, 210], "Y": [255, 190, 80]},
		"rows": [
		".......Y....Y.......",
		".....Y..YFFY..Y.....",
		"......YFFFFFFY......",
		"....YYFFFFFFFFYY....",
		"......YFFFFFFY......",
		"........YFFY........",
		"........kkkk........",
		"........kllk........",
		".......kmllmk.......",
		".......kmllmk.......",
		"......kmmllmmk......",
		"......kmmllmmk......",
		"......kmmmmmmk......",
		".....kkmmmmmmkk.....",
		".....kmkkmmkkmk.....",
		".....kmmmmmmmmk.....",
		"....kssmmmmmmssk....",
		"...kssskkkkkksssk...",
		"...ksSsssssssSssk...",
		"..kssSssssssssSssk..",
		"..kcsSSssssssSSsck..",
		".kccsSSSSSSSSSSccck.",
		".kcCcccccccccccCcck.",
		"kccCccccccccccccCcck"]},
	"vm_scatter": {"pal": {"k": [22, 20, 26], "m": [80, 84, 94], "l": [146, 152, 164], "w": [128, 82, 48],
			"W": [86, 52, 30], "s": [214, 160, 120], "S": [150, 100, 74], "c": [70, 74, 54]},
		"rows": [
		"........................",
		"........................",
		"........................",
		"........................",
		"........................",
		"........................",
		"........kkk..kkk........",
		"........klk..klk........",
		"........klk..klk........",
		"........klk..klk........",
		".......kklkkkklkk.......",
		".......kmlmmmmlmk.......",
		".......kmlmmmmlmk.......",
		"......kkmlmmmmlmkk......",
		"......kmmmmmmmmmmk......",
		"......kwwwwwwwwwwk......",
		".....kwWwwwwwwwwWwk.....",
		".....kwwWwwwwwwWwwk.....",
		"...kssswwwwwwwwwwsssk...",
		"..kssssSwwwwwwwwSssssk..",
		".kcsssssSwwwwwwSsssssck.",
		".kccSSSSSwwwwwwSSSSSSck.",
		"kcccccccWWWWWWWWccccccck",
		"kccccccckkkkkkkkcccccccc"]},
	"vm_scatter_fire": {"pal": {"k": [22, 20, 26], "m": [80, 84, 94], "l": [146, 152, 164], "w": [128, 82, 48],
			"W": [86, 52, 30], "s": [214, 160, 120], "S": [150, 100, 74], "c": [70, 74, 54], "F": [255, 250, 210],
			"Y": [255, 186, 74], "R": [255, 110, 40]},
		"rows": [
		".....R.Y..Y..Y..Y.R.....",
		"...R..YFFY.YYFFY..R.....",
		"....YYFFFFYYFFFFYY......",
		"..RYFFFFFFFFFFFFFFYR....",
		"....YYFFFFYYFFFFYY......",
		"......YFFY..YFFY........",
		"........kFk..kFk........",
		"........klk..klk........",
		"........klk..klk........",
		"........klk..klk........",
		".......kklkkkklkk.......",
		".......kmlmmmmlmk.......",
		".......kmlmmmmlmk.......",
		"......kkmlmmmmlmkk......",
		"......kmmmmmmmmmmk......",
		"......kwwwwwwwwwwk......",
		".....kwWwwwwwwwwWwk.....",
		".....kwwWwwwwwwWwwk.....",
		"...kssswwwwwwwwwwsssk...",
		"..kssssSwwwwwwwwSssssk..",
		".kcsssssSwwwwwwSsssssck.",
		".kccSSSSSwwwwwwSSSSSSck.",
		"kcccccccWWWWWWWWccccccck",
		"kccccccckkkkkkkkcccccccc"]},
	"vm_hex": {"pal": {"k": [20, 14, 26], "p": [110, 56, 150], "P": [170, 100, 230], "d": [62, 30, 86], "G": [140, 255, 160],
			"H": [220, 255, 220], "s": [214, 160, 120], "S": [150, 100, 74], "c": [70, 74, 54]},
		"rows": [
		"......................",
		"......................",
		"......................",
		"......................",
		"..........kk..........",
		".........kGGk.........",
		"........kGHHGk........",
		".......kpGHHGpk.......",
		"......kppkGGkppk......",
		".....kpPpdkkdpPpk.....",
		".....kpPpdddppPpk.....",
		"......kpPppppPpk......",
		".......kpPPPPpk.......",
		"........kpddpk........",
		".........kddk.........",
		"........ksddsk........",
		"......kssddddssk......",
		".....ksssSddSsssk.....",
		"....kcsssSddSssssk....",
		"...kcSSSSSddSSSSSck...",
		"..kccccccSddScccccck..",
		".kcccccccSddSccccccck.",
		"kcccccccckddkcccccccck",
		"kcccccccckkkkcccccccck"]},
	"vm_hex_fire": {"pal": {"k": [20, 14, 26], "p": [110, 56, 150], "P": [170, 100, 230], "d": [62, 30, 86], "G": [140, 255, 160],
			"H": [230, 255, 230], "Q": [190, 120, 255], "s": [214, 160, 120], "S": [150, 100, 74], "c": [70, 74, 54]},
		"rows": [
		"......Q....G....Q.....",
		"....Q...GHHHHG...Q....",
		"......GHHHHHHHHG......",
		"..Q..GHHHHHHHHHHG..Q..",
		"......GHHHHHHHHG......",
		"....Q..GHHHHHHG..Q....",
		"........kGHHGk........",
		".......kpGHHGpk.......",
		"......kppkGGkppk......",
		".....kpPpdkkdpPpk.....",
		".....kpPpdddppPpk.....",
		"......kpPppppPpk......",
		".......kpPPPPpk.......",
		"........kpddpk........",
		".........kddk.........",
		"........ksddsk........",
		"......kssddddssk......",
		".....ksssSddSsssk.....",
		"....kcsssSddSssssk....",
		"...kcSSSSSddSSSSSck...",
		"..kccccccSddScccccck..",
		".kcccccccSddSccccccck.",
		"kcccccccckddkcccccccck",
		"kcccccccckkkkcccccccck"]}
};

var VIEWMODELS = {};
array<string> VIEWMODEL_NAMES = ["vm_pistol", "vm_scatter", "vm_hex"];

function parse_viewmodels() -> void {
	VIEWMODELS = {};
	for (auto kv : VIEWMODEL_SRC) { VIEWMODELS[kv.first] = parse_art(kv.second); }
}

// palette code -> index for a weapon art under this floor's light, brighter in its own flash
function viewmodel_lut(string name, bool flashing) -> array<int> {
	string key = `${name}:${flashing}`;
	if (G.lutc.has(key)) { return G.lutc[key]; }
	auto& mood = MOODS[G.map_i];
	var& art = VIEWMODELS[name];
	var& rgbs = art["rgb"];
	array<string> keys = art["keys"];
	math::color light = math::mix_rgb(mood.light, rgb(255, 255, 255), 0.4) * (flashing ? 1.25 : 0.85);
	array<int> lut = [0];
	for (int c = 1; c < rgbs.size(); ++c) {
		math::color color = rgb(rgbs[c][0], rgbs[c][1], rgbs[c][2]);
		string k = keys[c];
		if (k >= "A" && k <= "Z") { lut.push(color_idx(color)); }
		else { lut.push(color_idx(math::clamp(color * light, 0.0, 1.0))); }
	}
	G.lutc[key] = lut;
	return lut;
}

function draw_viewmodel(var& pix) -> void {
	if (G.automap) { return; }
	bool firing = G.cooldown > WEAPONS[G.weapon]["cd"] - 4 || G.gun_kick > 0;
	string name = VIEWMODEL_NAMES[G.weapon];
	if (firing && G.muzzle > 0) { name = name + "_fire"; }
	var& art = VIEWMODELS[name];
	int aw = art["w"];
	int ah = art["h"];
	var& apix = art["pix"];
	array<int> lut = viewmodel_lut(name, G.muzzle > 0);
	int vw = G.vw;
	int vh = G.vh;
	// art pixels are square: a view pixel is PIXW x PIXH of a 1:2 cell
	float scale = vh * 0.4 / 24.0;
	int ph = math::itrunc(ah * scale);
	int pw = ph * aw * 2 * PIXW / (ah * PIXH);
	int sway = math::itrunc(math::sin(G.bob * 0.5) * vw * 0.025);
	int lift = math::itrunc(math::abs(math::cos(G.bob * 0.5)) * scale * 1.5) + G.gun_kick * math::itrunc(scale * 0.9);
	int x0 = vw / 2 - pw / 2 + sway;
	int y0 = vh - ph + lift;
	int cx0 = imax(0, x0);
	int cx1 = imin(vw, x0 + pw);
	int cy1 = imin(vh, y0 + ph);
	for (int y = imax(0, y0); y < cy1; ++y) {
		int row = ((y - y0) * ah / ph) * aw;
		int base = y * vw;
		for (int x = cx0; x < cx1; ++x) {
			int code = apix[row + (x - x0) * aw / pw];
			if (code > 0) { pix[base + x] = lut[code]; }
		}
	}
}
