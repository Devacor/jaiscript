// JaiDOOM wad.jai — WAD file access in pure JaiScript. The file's bytes are one string, read
// through binary::reader (little-endian, the format's own order); the directory entry is a
// binary::layout, and whole lumps come out of to_bytes (an array<uint8>) into the array<int> tables the renderer indexes.

string WAD = "";              // raw file bytes
string WAD_ID = "";           // "IWAD" | "PWAD"
string WAD_BASE = "";         // file name without folder or extension: "doom1", "freedoom2"
array<string> WAD_FILES = []; // every file merged into the directory, base first
int NUM_LUMPS = 0;
array<int> LUMP_POS = [];
array<int> LUMP_SIZE = [];
array<string> LUMP_NAME = [];
map<string, int> LUMP_INDEX = {}; // name -> index, later lumps win (W_GetNumForName scans backward)

// a directory entry: the lump's offset and size, its name NUL-padded to 8 bytes (all 8 when the name fills them)
binary::layout LUMP_ENTRY = binary::layout("pos", binary::int32, "size", binary::int32, "name", binary::text(8));

// a reader over the WAD at a lump's first byte
function lump_reader(int idx) -> binary::reader { return WAD.reader().seek(LUMP_POS[idx]); }

// whole lump as a flat byte array (decode once, then hot code reads ints only)
function lump_bytes(int idx) -> array<int> { return WAD.to_bytes(LUMP_POS[idx], LUMP_SIZE[idx]); }

// count directory entries at r's cursor join the lump tables, their offsets shifted by base
function wad_directory(binary::reader& r, int base, int count) -> void {
	for (int i = 0; i < count; ++i) {
		var e = r.unpack(LUMP_ENTRY);
		string nm = e["name"];
		LUMP_POS.push(base + e["pos"]);
		LUMP_SIZE.push(e["size"]);
		LUMP_NAME.push(nm);
		LUMP_INDEX[nm] = NUM_LUMPS + i;
	}
}

// The game folder is the one holding wads/ and cache/: the folder above scripts/ in the
// repository layout, the executable's own folder or its parent when jaiscript sits inside a
// copied game folder. Every path the game reads or writes starts from it (or from the
// per-user data folder when it is not writable), never from the working directory, so a
// script dropped onto jaiscript.exe runs the same as one started from a shell.
string GAME_ROOT = "";
string DATA_ROOT = "";
function game_root() -> string {
	if (GAME_ROOT == "") {
		array<string> roots = [parent_dir(root_script_dir()), root_script_dir(), bin_dir(), parent_dir(bin_dir())];
		GAME_ROOT = roots[0];
		for (auto r : roots) { if (dir_exists(r + "/wads")) { GAME_ROOT = r; break; } }
	}
	return GAME_ROOT;
}

// Where downloads, the audio cache, saves and demos go: the game folder when a file can be
// created there, the per-user data folder (app_data_dir) for a read-only install
function data_root() -> string {
	if (DATA_ROOT == "") { DATA_ROOT = writable_dir(game_root() + "/wads") ? game_root() : app_data_dir("JaiDOOM"); }
	return DATA_ROOT;
}
function wad_dir() -> string { return data_root() + "/wads/"; }

// A WAD by name ("doom1", "freedoom2.wad") or path: as given when that file exists, else
// the game folder's wads/, else the data folder's (where a download lands)
function wad_path(string name) -> string {
	string file = name.ends_with(".wad") ? name : name + ".wad";
	if (file_exists(file)) { return file; }
	string base = file_name(file);
	if (file_exists(game_root() + "/wads/" + base)) { return game_root() + "/wads/" + base; }
	return wad_dir() + base;
}

// A save, demo or config file: a bare name lives in the data folder, a path stays as given
function data_path(string name) -> string {
	return file_name(name) == name ? data_root() + "/" + name : name;
}

// cache/<wad>/ under the data folder: the decoded sound and music files (sound.jai, mus.jai;
// bootstrap fills it up front through extract_audio.jai, the game fills it lazily otherwise)
function audio_cache_dir() -> string { return data_root() + "/cache/" + WAD_BASE + "/"; }

// Merge a further WAD (a PWAD of maps, or another IWAD's resources) over what is loaded:
// its bytes append to the buffer, its lumps join the directory with shifted offsets, and
// a name it repeats wins (doom's W_GetNumForName scans backward), so a PWAD's MAP01 or its
// replaced textures take effect while everything it does not mention stays from the base.
function wad_add(string path) -> bool {
	if (!file_exists(path)) { return false; }
	string more = read_file(path);
	if (more.size() < 12) { return false; }
	binary::reader r = more.reader();
	string id = r.text(4);
	if (id != "IWAD" && id != "PWAD") { return false; }
	int count = r.int32();
	r.seek(r.int32());
	int base = WAD.size();
	WAD = WAD + more;
	wad_directory(r, base, count);
	NUM_LUMPS = NUM_LUMPS + count;
	WAD_FILES.push(path);
	return true;
}

// Every map the merged directory can start: an ExMy or MAPxx marker whose THINGS lump
// follows, later files winning, in name order (episode maps before MAPxx)
function map_list() -> array<string> {
	array<string> names = [];
	var seen = {};
	for (int i = NUM_LUMPS - 1; i >= 0; --i) {
		string nm = LUMP_NAME[i];
		bool episode = nm.size() == 4 && nm[0] == 'E' && nm[2] == 'M' && nm[1] >= '1' && nm[1] <= '9' && nm[3] >= '1' && nm[3] <= '9';
		bool level = nm.size() == 5 && nm.starts_with("MAP") && nm[3] >= '0' && nm[3] <= '9' && nm[4] >= '0' && nm[4] <= '9';
		if (!(episode || level) || seen.has(nm)) { continue; }
		if (i + 1 >= NUM_LUMPS || LUMP_NAME[i + 1] != "THINGS") { continue; }
		seen[nm] = true;
		names.push(nm);
	}
	names.sort();
	return names;
}

function wad_load(string path) -> bool {
	if (!file_exists(path)) { return false; }
	WAD = read_file(path);
	if (WAD.size() < 12) { return false; }
	WAD_FILES = [path];
	int slash = path.find_last_of("/\\");
	string file = slash >= 0 ? path.substr(slash + 1) : path;
	int dot = file.rfind(".");
	WAD_BASE = dot > 0 ? file.substr(0, dot) : file;
	binary::reader r = WAD.reader();
	WAD_ID = r.text(4);
	if (WAD_ID != "IWAD" && WAD_ID != "PWAD") { return false; }
	int count = r.int32();
	r.seek(r.int32());
	LUMP_POS = [];
	LUMP_SIZE = [];
	LUMP_NAME = [];
	LUMP_INDEX = {};
	NUM_LUMPS = 0;
	wad_directory(r, 0, count);
	NUM_LUMPS = count;
	return true;
}

function lump_num(string name) -> int {
	if (LUMP_INDEX.has(name)) { return LUMP_INDEX[name]; }
	return -1;
}

// first lump named `name` at or after `start` (map sub-lumps live after their marker)
function lump_num_from(string name, int start) -> int {
	for (int i = start; i < NUM_LUMPS && i < start + 12; ++i) {
		if (LUMP_NAME[i] == name) { return i; }
	}
	return -1;
}

