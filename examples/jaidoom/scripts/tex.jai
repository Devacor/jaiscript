// JaiDOOM tex.jai — graphics lumps: PLAYPAL/COLORMAP, flats, patches,
// TEXTURE1 composites. Everything decodes into flat arrays (the texture
// directory at map start, patch column tables and composites lazily); PALCM
// prefolds colormap band x palette index -> interned terminal color.

import "wad.jai";
import "vid.jai";

array<int> DPAL = [];         // doom palette idx -> interned vid idx (bank 0)
array<int> CMAP = [];         // 34 x 256 colormap bytes
array<int> PALCM = [];        // band*256+palidx -> interned vid idx (prefolded)

map<string, int> TEX_NAMES = {}; // texture name -> id
array<int> TEX_W = [];        // per id
array<int> TEX_H = [];
array<int> TEX_READY = [];    // 0 = not composited yet
array<array<int>> TEX_PIX = []; // per id: array<int> w*h column-major (col*h+row)
array<int> TEX_PFIRST = [];   // per id: its first patch in TP_OX / TP_OY / TP_PIDX
array<int> TEX_NPATCH = [];   // per id: how many patches it draws
array<int> TP_OX = [];        // per texture patch: origin x, origin y, PNAMES index
array<int> TP_OY = [];
array<int> TP_PIDX = [];
array<int> PNAME_LUMP = [];   // patch index -> lump index
array<int> PATCH_W = [];      // per PNAMES index: width in columns, -1 until decoded
array<int> PATCH_COL0 = [];   // per PNAMES index: its first column in PATCH_POST
array<int> PATCH_POST = [];   // per decoded column: WAD offset of its first post

map<string, array<int>> FLAT_CACHE = {}; // flat name -> array<int> 4096
int SKY_TEX = -1;

int CUR_BANK = -1;

int TEX_WORKERS = 1;          // 0 = serial map-start texture warm; nonzero = parallel_transform

// PLAYPAL bank switch (pain reds 1-8, pickup golds 9-12, radsuit 13):
// re-intern the 256 colors and refold PALCM — only on actual change
function set_palette_bank(int b) -> void {
	if (b == CUR_BANK) { return; }
	CUR_BANK = b;
	array<int> rgb = WAD.to_bytes(LUMP_POS[lump_num("PLAYPAL")] + b * 768, 768);
	DPAL = [];
	for (int i = 0; i < 256; ++i) {
		DPAL.push(rgb_idx(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]));
	}
	int total = 34 * 256;
	PALCM = [];
	for (int k = 0; k < total; ++k) { PALCM.push(DPAL[CMAP[k]]); }
}

function gfx_init() -> void {
	CMAP = WAD.to_bytes(LUMP_POS[lump_num("COLORMAP")], 34 * 256);
	CUR_BANK = -1;
	set_palette_bank(0);

	// PNAMES: count, then 8-byte names -> lump numbers
	binary::reader names = lump_reader(lump_num("PNAMES"));
	int npatch = names.int32();
	PNAME_LUMP = []; PATCH_W = []; PATCH_COL0 = []; PATCH_POST = [];
	for (int p = 0; p < npatch; ++p) {
		PNAME_LUMP.push(lump_num(string::to_upper(names.text(8))));
		PATCH_W.push(-1);
		PATCH_COL0.push(0);
	}

	// TEXTURE1: count and offsets, then each def: name8, masked, w, h, coldir, npatches, and per
	// patch origin x, origin y, PNAMES index, stepdir, colormap
	int t1 = LUMP_POS[lump_num("TEXTURE1")];
	binary::reader dir = WAD.reader().seek(t1);
	binary::reader def = WAD.reader();
	int ntex = dir.int32();
	TEX_NAMES = {};
	TEX_W = []; TEX_H = []; TEX_READY = []; TEX_PIX = []; TEX_PFIRST = []; TEX_NPATCH = [];
	TP_OX = []; TP_OY = []; TP_PIDX = [];
	for (int t = 0; t < ntex; ++t) {
		def.seek(t1 + dir.int32());
		TEX_NAMES[def.text(8)] = t;
		def.skip(4);
		TEX_W.push(def.int16());
		TEX_H.push(def.int16());
		def.skip(4);
		int np = def.int16();
		TEX_PFIRST.push(TP_OX.size());
		TEX_NPATCH.push(np);
		for (int q = 0; q < np; ++q) {
			TP_OX.push(def.int16());
			TP_OY.push(def.int16());
			TP_PIDX.push(def.int16());
			def.skip(4);
		}
		TEX_READY.push(0);
		TEX_PIX.push([]);
	}
	SKY_TEX = tex_id("SKY1");
	FLAT_CACHE = {};
}

function tex_id(string name) -> int {
	if (TEX_NAMES.has(name)) { return TEX_NAMES[name]; }
	return -1;
}

// a patch's column table, decoded once per map start: its width and each column's first post
function patch_prepare(int pidx) -> void {
	if (PATCH_W[pidx] >= 0) { return; }
	int lumpidx = PNAME_LUMP[pidx];
	binary::reader r = lump_reader(lumpidx);
	int pw = r.int16();
	r.skip(6);
	PATCH_COL0[pidx] = PATCH_POST.size();
	for (int col = 0; col < pw; ++col) { PATCH_POST.push(LUMP_POS[lumpidx] + r.uint32()); }
	PATCH_W[pidx] = pw < 0 ? 0 : pw;
}

// every patch texture id draws, decoded ahead of tex_composite / tex_masked (serial code)
function tex_patches_prepare(int id) -> void {
	int first = TEX_PFIRST[id];
	int np = TEX_NPATCH[id];
	int known = PNAME_LUMP.size();
	for (int p = 0; p < np; ++p) {
		int pidx = TP_PIDX[first + p];
		if (pidx >= 0 && pidx < known && PNAME_LUMP[pidx] >= 0) { patch_prepare(pidx); }
	}
}

// composite ONE texture def -> w*h column-major grid. Written as a pure
// parallel_transform body (the map-start warm pass runs it over every wall
// texture the map references, its patches prepared first): captured globals
// touched by subscript reads only, all state local, no var& aliases of
// captured names. The SAME function serves the lazy serial tex_prepare.
function tex_composite(int id) -> array<int> {
	int texw = TEX_W[id];
	int texh = TEX_H[id];
	array<int> pix = [];
	int n = texw * texh;
	for (int i = 0; i < n; ++i) { pix.push(0); }
	int first = TEX_PFIRST[id];
	int npatch = TEX_NPATCH[id];
	int known = PNAME_LUMP.size();
	for (int p = 0; p < npatch; ++p) {
		int ox = TP_OX[first + p];
		int oy = TP_OY[first + p];
		int pidx = TP_PIDX[first + p];
		if (pidx < 0 || pidx >= known) { continue; }
		int lumpidx = PNAME_LUMP[pidx];
		if (lumpidx < 0) { continue; }
		int pw = PATCH_W[pidx];
		int col0 = PATCH_COL0[pidx];
		for (int col = 0; col < pw; ++col) {
			int dx = ox + col;
			if (dx < 0 || dx >= texw) { continue; }
			int colbase = dx * texh;
			int post = PATCH_POST[col0 + col];
			while (WAD[post] != 255) {
				int topdelta = WAD[post] + 0;
				int len = WAD[post + 1] + 0;
				int src = post + 3;
				for (int i2 = 0; i2 < len; ++i2) {
					int dy = oy + topdelta + i2;
					if (dy >= 0 && dy < texh) { pix[colbase + dy] = WAD[src + i2] + 0; }
				}
				post = post + len + 4;
			}
		}
	}
	return pix;
}

function tex_prepare(int id) -> void {
	if (id < 0 || TEX_READY[id] != 0) { return; }
	tex_patches_prepare(id);
	TEX_PIX[id] = tex_composite(id);
	TEX_READY[id] = 1;
}

// masked variant for two-sided mid textures (grates/meshes): -1 where no
// post covered a texel, so holes stay transparent like sprites
map<int, array<int>> MTEX_PIX = {};

function tex_masked(int id) -> void {
	if (MTEX_PIX.has(id)) { return; }
	tex_patches_prepare(id);
	int texw = TEX_W[id];
	int texh = TEX_H[id];
	array<int> pix = [];
	int n = texw * texh;
	for (int i = 0; i < n; ++i) { pix.push(-1); }
	int first = TEX_PFIRST[id];
	int npatch = TEX_NPATCH[id];
	int known = PNAME_LUMP.size();
	for (int p = 0; p < npatch; ++p) {
		int ox = TP_OX[first + p];
		int oy = TP_OY[first + p];
		int pidx = TP_PIDX[first + p];
		if (pidx < 0 || pidx >= known) { continue; }
		int lumpidx = PNAME_LUMP[pidx];
		if (lumpidx < 0) { continue; }
		int pw = PATCH_W[pidx];
		int col0 = PATCH_COL0[pidx];
		for (int col = 0; col < pw; ++col) {
			int dx = ox + col;
			if (dx < 0 || dx >= texw) { continue; }
			int colbase = dx * texh;
			int post = PATCH_POST[col0 + col];
			while (WAD[post] != 255) {
				int topdelta = WAD[post] + 0;
				int len = WAD[post + 1] + 0;
				int src = post + 3;
				for (int i2 = 0; i2 < len; ++i2) {
					int dy = oy + topdelta + i2;
					if (dy >= 0 && dy < texh) { pix[colbase + dy] = WAD[src + i2] + 0; }
				}
				post = post + len + 4;
			}
		}
	}
	MTEX_PIX[id] = pix;
}

function flat_pix(string name) -> array<int> {
	if (FLAT_CACHE.has(name)) { return FLAT_CACHE[name]; }
	int lu = lump_num(name);
	array<int> out = [];
	if (lu < 0 || LUMP_SIZE[lu] < 4096) {
		for (int i = 0; i < 4096; ++i) { out.push(96); }
	} else {
		out = WAD.to_bytes(LUMP_POS[lu], 4096);
	}
	FLAT_CACHE[name] = out;
	return out;
}
