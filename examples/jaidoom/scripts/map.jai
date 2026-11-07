// JaiDOOM map.jai — level lumps decoded into flat parallel typed arrays
// (all-primitive array<int> is the VM fast path; GLOOM discipline throughout).

import "wad.jai";

string MAP_NAME = "";

array<int> VX_X = [];
array<int> VX_Y = [];

array<int> LD_V1 = [];
array<int> LD_V2 = [];
array<int> LD_FLAGS = [];
array<int> LD_SPECIAL = [];
array<int> LD_TAG = [];
array<int> LD_SIDE0 = [];     // -1 = none
array<int> LD_SIDE1 = [];

array<int> SD_XOFS = [];
array<int> SD_YOFS = [];
array<int> SD_SEC = [];
array<string> SD_TOP = [];    // texture names (strings; "-" = none)
array<string> SD_BOT = [];
array<string> SD_MID = [];

array<int> SE_FLOORH = [];
array<int> SE_CEILH = [];
array<int> SE_LIGHT = [];
array<int> SE_SPECIAL = [];
array<int> SE_TAG = [];
array<string> SE_FLOORPIC = [];
array<string> SE_CEILPIC = [];

array<int> SG_V1 = [];
array<int> SG_V2 = [];
array<int> SG_ANG = [];
array<int> SG_LINE = [];
array<int> SG_DIR = [];
array<int> SG_OFS = [];

array<int> SS_COUNT = [];
array<int> SS_FIRST = [];

array<int> ND_X = [];
array<int> ND_Y = [];
array<int> ND_DX = [];
array<int> ND_DY = [];
array<int> ND_BBOX = [];      // 8 per node: right(top,bottom,left,right), left(...)
array<int> ND_RIGHT = [];     // bit 15 set = subsector index
array<int> ND_LEFT = [];

array<int> TH_X = [];
array<int> TH_Y = [];
array<int> TH_ANG = [];
array<int> TH_TYPE = [];
array<int> TH_FLAGS = [];

array<int> BMAP = [];         // raw blockmap words (i16)
int BMAP_ORGX = 0;
int BMAP_ORGY = 0;
int BMAP_W = 0;
int BMAP_H = 0;

int MAP_MINX = 0;
int MAP_MINY = 0;
int MAP_MAXX = 0;
int MAP_MAXY = 0;

function side_or_none(int v) -> int { return v == 65535 ? -1 : v; }

function map_load(string mapname) -> bool {
	int marker = lump_num(mapname);
	if (marker < 0) { return false; }
	MAP_NAME = mapname;

	int lu = lump_num_from("VERTEXES", marker);
	binary::reader r = lump_reader(lu);
	int count = LUMP_SIZE[lu] / 4;
	VX_X = []; VX_Y = [];
	MAP_MINX = 100000; MAP_MINY = 100000; MAP_MAXX = -100000; MAP_MAXY = -100000;
	for (int i = 0; i < count; ++i) {
		int x = r.int16(); int y = r.int16();
		VX_X.push(x); VX_Y.push(y);
		if (x < MAP_MINX) { MAP_MINX = x; }
		if (x > MAP_MAXX) { MAP_MAXX = x; }
		if (y < MAP_MINY) { MAP_MINY = y; }
		if (y > MAP_MAXY) { MAP_MAXY = y; }
	}

	lu = lump_num_from("LINEDEFS", marker);
	r = lump_reader(lu);
	count = LUMP_SIZE[lu] / 14;
	LD_V1 = []; LD_V2 = []; LD_FLAGS = []; LD_SPECIAL = []; LD_TAG = []; LD_SIDE0 = []; LD_SIDE1 = [];
	for (int i2 = 0; i2 < count; ++i2) {
		LD_V1.push(r.uint16());
		LD_V2.push(r.uint16());
		LD_FLAGS.push(r.uint16());
		LD_SPECIAL.push(r.uint16());
		LD_TAG.push(r.uint16());
		LD_SIDE0.push(side_or_none(r.uint16()));
		LD_SIDE1.push(side_or_none(r.uint16()));
	}

	lu = lump_num_from("SIDEDEFS", marker);
	r = lump_reader(lu);
	count = LUMP_SIZE[lu] / 30;
	SD_XOFS = []; SD_YOFS = []; SD_SEC = []; SD_TOP = []; SD_BOT = []; SD_MID = [];
	for (int i3 = 0; i3 < count; ++i3) {
		SD_XOFS.push(r.int16());
		SD_YOFS.push(r.int16());
		SD_TOP.push(r.text(8));
		SD_BOT.push(r.text(8));
		SD_MID.push(r.text(8));
		SD_SEC.push(r.uint16());
	}

	lu = lump_num_from("SECTORS", marker);
	r = lump_reader(lu);
	count = LUMP_SIZE[lu] / 26;
	SE_FLOORH = []; SE_CEILH = []; SE_LIGHT = []; SE_SPECIAL = []; SE_TAG = []; SE_FLOORPIC = []; SE_CEILPIC = [];
	for (int i4 = 0; i4 < count; ++i4) {
		SE_FLOORH.push(r.int16());
		SE_CEILH.push(r.int16());
		SE_FLOORPIC.push(r.text(8));
		SE_CEILPIC.push(r.text(8));
		SE_LIGHT.push(r.int16());
		SE_SPECIAL.push(r.uint16());
		SE_TAG.push(r.uint16());
	}

	lu = lump_num_from("SEGS", marker);
	r = lump_reader(lu);
	count = LUMP_SIZE[lu] / 12;
	SG_V1 = []; SG_V2 = []; SG_ANG = []; SG_LINE = []; SG_DIR = []; SG_OFS = [];
	for (int i5 = 0; i5 < count; ++i5) {
		SG_V1.push(r.uint16());
		SG_V2.push(r.uint16());
		SG_ANG.push(r.uint16());
		SG_LINE.push(r.uint16());
		SG_DIR.push(r.uint16());
		SG_OFS.push(r.int16());
	}

	lu = lump_num_from("SSECTORS", marker);
	r = lump_reader(lu);
	count = LUMP_SIZE[lu] / 4;
	SS_COUNT = []; SS_FIRST = [];
	for (int i6 = 0; i6 < count; ++i6) {
		SS_COUNT.push(r.uint16());
		SS_FIRST.push(r.uint16());
	}

	lu = lump_num_from("NODES", marker);
	r = lump_reader(lu);
	count = LUMP_SIZE[lu] / 28;
	ND_X = []; ND_Y = []; ND_DX = []; ND_DY = []; ND_BBOX = []; ND_RIGHT = []; ND_LEFT = [];
	for (int i7 = 0; i7 < count; ++i7) {
		ND_X.push(r.int16());
		ND_Y.push(r.int16());
		ND_DX.push(r.int16());
		ND_DY.push(r.int16());
		for (int b = 0; b < 8; ++b) { ND_BBOX.push(r.int16()); }
		ND_RIGHT.push(r.uint16());
		ND_LEFT.push(r.uint16());
	}

	lu = lump_num_from("THINGS", marker);
	r = lump_reader(lu);
	count = LUMP_SIZE[lu] / 10;
	TH_X = []; TH_Y = []; TH_ANG = []; TH_TYPE = []; TH_FLAGS = [];
	for (int i8 = 0; i8 < count; ++i8) {
		TH_X.push(r.int16());
		TH_Y.push(r.int16());
		TH_ANG.push(r.int16());
		TH_TYPE.push(r.int16());
		TH_FLAGS.push(r.int16());
	}

	lu = lump_num_from("BLOCKMAP", marker);
	r = lump_reader(lu);
	count = LUMP_SIZE[lu] / 2;
	BMAP = [];
	for (int i9 = 0; i9 < count; ++i9) { BMAP.push(r.int16()); }
	BMAP_ORGX = BMAP[0];
	BMAP_ORGY = BMAP[1];
	BMAP_W = BMAP[2] & 65535;
	BMAP_H = BMAP[3] & 65535;

	return true;
}
