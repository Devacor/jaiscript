// JaiDOOM render.jai — stage 3: textured walls (pegging-correct, perspective-
// correct u), per-pixel textured flats, COLORMAP light diminishing through the
// prefolded PALCM table, angle-mapped sky. Front-to-back BSP with per-column
// clip tracking exactly as stage 2.

import "wad.jai";
import "map.jai";
import "vid.jai";
import "tex.jai";
import "things.jai";

float VIEW_X = 0.0;
float VIEW_Y = 0.0;
float VIEW_Z = 0.0;
float VIEW_ANG = 0.0;         // radians, 0 = east, CCW
float VSIN = 0.0;
float VCOS = 0.0;
float FOCAL = 0.0;
float FOCALY = 0.0;
int CENTERX = 0;
int CENTERY = 0;
float PXASPECT = 1.2;         // doom's 320x200-on-4:3 vertical stretch
float EYE_Z = -100000.0;      // explicit view height; sentinel = derive from sector
int EXTRA_LIGHT = 0;          // brighter bands while the muzzle flashes
float WBOB_X = 0.0;           // weapon bob, 320-space pixels (game_tic drives it)
float WBOB_Y = 0.0;

array<int> SEG_FSEC = [];
array<int> SEG_BSEC = [];
array<int> SEG_MIDT = [];     // texture ids, -1 = none
array<int> SEG_TOPT = [];
array<int> SEG_BOTT = [];
array<int> SEG_XOFS = [];     // sidedef xofs + seg offset
array<int> SEG_YOFS = [];
array<int> SEG_PEGT = [];     // ML_DONTPEGTOP (upper unpegged)
array<int> SEG_PEGB = [];     // ML_DONTPEGBOTTOM (lower unpegged)
array<float> SEG_LEN = [];

map<string, int> FLAT_SLOT = {}; // flat name -> FLATS index
array<array<int>> FLATS = []; // arrays of 4096 palette bytes
array<int> SEC_FFLAT = [];    // per sector: FLATS index of floor
array<int> SEC_CFLAT = [];
array<int> SEC_SKY = [];      // per sector: 1 = F_SKY1 ceiling

array<int> CLIPTOP = [];
array<int> CLIPBOT = [];
array<int> COLDONE = [];
array<float> WALLZ = [];      // depth of the solid wall that closed each column
// depth-aware clip snapshots (the drawseg idea, flattened): every portal that
// narrows a column records (depth, window-so-far); a sprite clips to the
// window AS OF ITS OWN DEPTH — nearer portals clip it, farther ones must not
// (clipping to the final window hid platform items behind their own back
// steps until the viewer got close). 4 slots/column; overflow tightens the
// deepest slot, which can only ever over-occlude in 5-portal columns.
array<int> CLIPS_N = [];      // per column: snapshots recorded
array<float> CLIPS_D = [];    // [x*4+i] narrowing seg depth
array<int> CLIPS_T = [];      // window top after it
array<int> CLIPS_B = [];      // window bottom after it
int REMAIN = 0;

array<int> MSEG = [];         // two-sided segs with a mid texture (grates/meshes):
array<float> MSEG_D = [];     // deferred past the solid pass, drawn with the sprites
int MASKED_DRAWN = 0;

function flat_slot(string name) -> int {
	if (FLAT_SLOT.has(name)) { return FLAT_SLOT[name]; }
	int idx = FLATS.size();
	FLAT_SLOT[name] = idx;
	FLATS.push(flat_pix(name));
	return idx;
}

// doom2 rotates skies by map third; episodic maps use their episode's sky
function sky_name(string mapname) -> string {
	if (mapname.starts_with("MAP")) {
		int n = mapname.substr(3, 2).to_int(1);
		if (n <= 11) { return "SKY1"; }
		if (n <= 20) { return "SKY2"; }
		return "SKY3";
	}
	if (mapname.starts_with("E") && mapname.size() >= 2) { return "SKY" + mapname.substr(1, 1); }
	return "SKY1";
}

function prepare_render() -> void {
	gfx_init();
	int sk = tex_id(sky_name(MAP_NAME));
	if (sk >= 0) { SKY_TEX = sk; }                   // missing per-map sky keeps gfx_init's SKY1
	DARK_MAP = {};                                   // interned ids reset with the palette
	SEG_FSEC = []; SEG_BSEC = [];
	SEG_MIDT = []; SEG_TOPT = []; SEG_BOTT = [];
	SEG_XOFS = []; SEG_YOFS = []; SEG_PEGT = []; SEG_PEGB = [];
	SEG_LEN = [];
	int nsegs = SG_V1.size();
	for (int i = 0; i < nsegs; ++i) {
		int ld = SG_LINE[i];
		int dir = SG_DIR[i];
		int fside = 0;
		int bside = 0;
		if (dir == 0) { fside = LD_SIDE0[ld]; bside = LD_SIDE1[ld]; }
		else { fside = LD_SIDE1[ld]; bside = LD_SIDE0[ld]; }
		SEG_FSEC.push(fside >= 0 ? SD_SEC[fside] : 0);
		SEG_BSEC.push(bside >= 0 ? SD_SEC[bside] : -1);
		SEG_MIDT.push(fside >= 0 ? tex_id(SD_MID[fside]) : -1);
		SEG_TOPT.push(fside >= 0 ? tex_id(SD_TOP[fside]) : -1);
		SEG_BOTT.push(fside >= 0 ? tex_id(SD_BOT[fside]) : -1);
		SEG_XOFS.push((fside >= 0 ? SD_XOFS[fside] : 0) + SG_OFS[i]);
		SEG_YOFS.push(fside >= 0 ? SD_YOFS[fside] : 0);
		int flags = LD_FLAGS[ld];
		SEG_PEGT.push((flags & 8) != 0 ? 1 : 0);
		SEG_PEGB.push((flags & 16) != 0 ? 1 : 0);
		float dxw = VX_X[SG_V2[i]] - VX_X[SG_V1[i]];
		float dyw = VX_Y[SG_V2[i]] - VX_Y[SG_V1[i]];
		SEG_LEN.push(math::sqrt(dxw * dxw + dyw * dyw));
	}
	FLAT_SLOT = {};
	FLATS = [];
	SEC_FFLAT = []; SEC_CFLAT = []; SEC_SKY = [];
	int nsecs = SE_FLOORH.size();
	for (int s = 0; s < nsecs; ++s) {
		SEC_FFLAT.push(flat_slot(SE_FLOORPIC[s]));
		SEC_CFLAT.push(flat_slot(SE_CEILPIC[s]));
		SEC_SKY.push(SE_CEILPIC[s] == "F_SKY1" ? 1 : 0);
	}
	tex_warm_map();
}

function tex_warm_note(int tid, var& tids, var& seen) -> void {
	if (tid < 0 || TEX_READY[tid] != 0) { return; }
	if (seen.has(tid)) { return; }
	seen[tid] = 1;
	tids.push(tid);
}

// map-start warm: composite every wall texture the map's sidedefs reference
// (plus the sky) in one parallel_transform pass — the lazy first-sight
// composite spikes move into load time
function tex_warm_map() -> void {
	array<int> tids = [];
	map<int, int> seen = {};
	tex_warm_note(SKY_TEX, tids, seen);
	int nsegs = SEG_MIDT.size();
	for (int i = 0; i < nsegs; ++i) {
		int mt = SEG_MIDT[i];
		int tt = SEG_TOPT[i];
		int bt = SEG_BOTT[i];
		tex_warm_note(mt, tids, seen);
		tex_warm_note(tt, tids, seen);
		tex_warm_note(bt, tids, seen);
	}
	int nt = tids.size();
	if (nt == 0) { return; }
	for (int s0 = 0; s0 < nt; ++s0) { tex_patches_prepare(tids[s0]); }
	if (TEX_WORKERS == 0) {
		for (int s = 0; s < nt; ++s) { int sid = tids[s]; tex_prepare(sid); }
		return;
	}
	var grids = parallel_transform(tids, tex_composite);
	for (int k = 0; k < nt; ++k) {
		int id = tids[k];
		TEX_PIX[id] = grids[k];
		TEX_READY[id] = 1;
	}
}

function light_band(int light, float dist) -> int {
	int band = ((255 - light) >> 3) + math::itrunc(dist / 512.0) - EXTRA_LIGHT;
	if (band < 0) { band = 0; }
	if (band > 31) { band = 31; }
	return band;
}

function point_side(float x, float y, int n) -> int {
	float dx = x - ND_X[n];
	float dy = y - ND_Y[n];
	float cross = ND_DX[n] * dy - ND_DY[n] * dx;
	return cross > 0.0 ? 1 : 0;
}

function sector_at(float x, float y) -> int {
	int n = ND_X.size() - 1;
	while ((n & 0x8000) == 0) {
		if (point_side(x, y, n) == 0) { n = ND_RIGHT[n]; }
		else { n = ND_LEFT[n]; }
	}
	return SEG_FSEC[SS_FIRST[n & 0x7FFF]];
}

function draw_tex_col(int x, int y0, int y1, array<int>& tp, int tw, int th, int ucol, int vfx, int dvfx, int band, array<int>& pix, array<int>& palcm) -> void {
	if (y0 < 0) { int skip = -y0; vfx = vfx + dvfx * skip; y0 = 0; }
	if (y1 >= VIEWH) { y1 = VIEWH - 1; }
	int tc = ucol % tw;
	if (tc < 0) { tc = tc + tw; }
	int base = tc * th;
	int pcm = band * 256;
	for (int y = y0; y <= y1; ++y) {
		int iv = (vfx >> 16) % th;
		if (iv < 0) { iv = iv + th; }
		pix[y * VW + x] = palcm[pcm + tp[base + iv]];
		vfx = vfx + dvfx;
	}
}

function draw_flat_col(int x, int y0, int y1, array<int>& flat, float planeh, float raydx, float raydy, int light, array<int>& pix, array<int>& palcm) -> void {
	if (y0 < 0) { y0 = 0; }
	if (y1 >= VIEWH) { y1 = VIEWH - 1; }
	float dz = (VIEW_Z - planeh) * FOCALY;
	int bandbase = ((255 - light) >> 3) - EXTRA_LIGHT;
	for (int y = y0; y <= y1; ++y) {
		int dy = y - CENTERY;
		if (dy == 0) { continue; }
		float z = dz / dy;
		float wx = VIEW_X + raydx * z;
		float wy = VIEW_Y + raydy * z;
		int band = bandbase + math::itrunc(z * 0.001953125);
		if (band < 0) { band = 0; }
		if (band > 31) { band = 31; }
		pix[y * VW + x] = palcm[band * 256 + flat[((math::itrunc(wy) & 63) << 6) + (math::itrunc(wx) & 63)]];
	}
}

function draw_sky_col(int x, int y0, int y1) -> void {
	if (y0 < 0) { y0 = 0; }
	if (y1 >= VIEWH) { y1 = VIEWH - 1; }
	if (SKY_TEX < 0) { return; }
	float xfrac = (x - CENTERX) / FOCAL;
	float adeg = (VIEW_ANG - math::atan2(xfrac, 1.0)) * 180.0 / math::PI;
	int ucol = math::itrunc(adeg * (256.0 / 90.0));
	int tc = ucol % 256;
	if (tc < 0) { tc = tc + 256; }
	var& tp = TEX_PIX[SKY_TEX];
	int th = TEX_H[SKY_TEX];
	int base = tc * th;
	var& pix = PIX;
	var& palcm = PALCM;
	for (int y = y0; y <= y1; ++y) {
		int iv = (y * 128) / VIEWH;
		if (iv >= th) { iv = th - 1; }
		pix[y * VW + x] = palcm[tp[base + iv]];
	}
}

function draw_seg(int sg) -> void {
	float x1w = VX_X[SG_V1[sg]];
	float y1w = VX_Y[SG_V1[sg]];
	float x2w = VX_X[SG_V2[sg]];
	float y2w = VX_Y[SG_V2[sg]];

	float dx1 = x1w - VIEW_X; float dy1 = y1w - VIEW_Y;
	float dx2 = x2w - VIEW_X; float dy2 = y2w - VIEW_Y;
	float dep1 = dx1 * VCOS + dy1 * VSIN;
	float lat1 = dx1 * VSIN - dy1 * VCOS;
	float dep2 = dx2 * VCOS + dy2 * VSIN;
	float lat2 = dx2 * VSIN - dy2 * VCOS;

	float seglen = SEG_LEN[sg];
	float u1 = 0.0;
	float u2 = seglen;
	float neareps = 0.05;
	if (dep1 < neareps && dep2 < neareps) { return; }
	if (dep1 < neareps) {
		float t = (neareps - dep1) / (dep2 - dep1);
		dep1 = neareps;
		lat1 = lat1 + (lat2 - lat1) * t;
		u1 = t * seglen;
	} else if (dep2 < neareps) {
		float t2 = (neareps - dep2) / (dep1 - dep2);
		dep2 = neareps;
		lat2 = lat2 + (lat1 - lat2) * t2;
		u2 = seglen * (1.0 - t2);
	}

	float x1f = CENTERX + lat1 / dep1 * FOCAL;
	float x2f = CENTERX + lat2 / dep2 * FOCAL;
	if (x2f <= x1f) { return; }
	if (x1f >= VW || x2f < 0.0) { return; }

	int fsec = SEG_FSEC[sg];
	int bsec = SEG_BSEC[sg];
	float fceil = SE_CEILH[fsec];
	float ffloor = SE_FLOORH[fsec];
	int light = SE_LIGHT[fsec];
	bool fsky = SEC_SKY[fsec] != 0;

	bool solid = bsec < 0;
	float bceil = 0.0;
	float bfloor = 0.0;
	bool bsky = false;
	if (!solid) {
		bceil = SE_CEILH[bsec];
		bfloor = SE_FLOORH[bsec];
		bsky = SEC_SKY[bsec] != 0;
		if (bceil <= bfloor || bceil <= ffloor || bfloor >= fceil) { solid = true; }
	}

	int midt = SEG_MIDT[sg];
	int topt = SEG_TOPT[sg];
	int bott = SEG_BOTT[sg];
	if (!solid && topt >= 0 && bceil < fceil) { tex_prepare(topt); }
	if (!solid && bott >= 0 && bfloor > ffloor) { tex_prepare(bott); }
	if (!solid && midt >= 0) {                       // masked mid: defer past the solid pass
		MSEG.push(sg);
		MSEG_D.push((dep1 + dep2) * 0.5);
	}

	int xofs = SEG_XOFS[sg];
	int yofs = SEG_YOFS[sg];
	// world-height anchors where texel v = 0 sits (pegging rules)
	float amid = SEG_PEGB[sg] != 0 && midt >= 0 ? ffloor + TEX_H[midt] : fceil;
	float atop = fceil;
	if (!solid && topt >= 0 && SEG_PEGT[sg] == 0) { atop = bceil + TEX_H[topt]; }
	float abot = SEG_PEGB[sg] != 0 ? fceil : bfloor;

	// the face a solid column shows: one-sided walls use MID, but a CLOSED
	// portal (a shut door: back ceiling met back floor) has its art on the
	// UPPER texture — mid is usually empty there, and the old flat fallback
	// swam like rock as you moved. Pegged uppers anchor to the shut ceiling
	// so the art rises with the door.
	int facet = midt;
	float aface = amid;
	if (solid && bsec >= 0) {
		if (topt >= 0) {
			facet = topt;
			aface = SEG_PEGT[sg] == 0 ? bceil + TEX_H[topt] : fceil;
		} else if (bott >= 0) {
			facet = bott;
			aface = SEG_PEGB[sg] != 0 ? fceil : bfloor;
		}
	}
	if (solid && facet >= 0) { tex_prepare(facet); }

	float invz1 = 1.0 / dep1;
	float invz2 = 1.0 / dep2;
	float uz1 = u1 * invz1;
	float uz2 = u2 * invz2;
	float spanx = x2f - x1f;
	float ctop = fceil - VIEW_Z;
	float cbot = ffloor - VIEW_Z;

	int xa = math::itrunc(x1f < 0.0 ? 0.0 : x1f + 0.999);
	int xb = math::itrunc(x2f >= VW ? VW - 1 : x2f);
	var& cliptop = CLIPTOP;
	var& clipbot = CLIPBOT;
	var& done = COLDONE;
	var& clipsn = CLIPS_N;
	var& clipsd = CLIPS_D;
	var& clipst = CLIPS_T;
	var& clipsb = CLIPS_B;
	array<int>& pix = PIX;
	array<int>& palcm = PALCM;
	// seg-invariant handles: the column loop re-fetched these per drawn part
	int cslot = SEC_CFLAT[fsec];
	int fslot = SEC_FFLAT[fsec];
	array<int>& cflat = FLATS[cslot];
	array<int>& fflat = FLATS[fslot];
	array<int>& flat0 = FLATS[0];
	array<int>& ftp = TEX_PIX[facet >= 0 ? facet : 0];
	int ftw = facet >= 0 ? TEX_W[facet] : 64;
	int fth = facet >= 0 ? TEX_H[facet] : 64;
	array<int>& ttp = TEX_PIX[topt >= 0 ? topt : 0];
	int ttw = topt >= 0 ? TEX_W[topt] : 64;
	int tth = topt >= 0 ? TEX_H[topt] : 64;
	array<int>& btp = TEX_PIX[bott >= 0 ? bott : 0];
	int btw = bott >= 0 ? TEX_W[bott] : 64;
	int bth = bott >= 0 ? TEX_H[bott] : 64;
	for (int x = xa; x <= xb; ++x) {
		if (done[x] != 0) { continue; }
		float t3 = (x - x1f) / spanx;
		float invz = invz1 + (invz2 - invz1) * t3;
		float sc = invz * FOCALY;
		float dist = 1.0 / invz;
		float u = (uz1 + (uz2 - uz1) * t3) * dist;
		int ucol = math::itrunc(u) + xofs;
		int band = light_band(light, dist);
		int dvfx = math::itrunc(65536.0 / sc);
		int yc = math::itrunc(CENTERY - ctop * sc);
		int yf = math::itrunc(CENTERY - cbot * sc);
		int ct = cliptop[x];
		int cb = clipbot[x];
		float xfrac = (x - CENTERX) / FOCAL;
		float raydx = VCOS + VSIN * xfrac;
		float raydy = VSIN - VCOS * xfrac;

		// ceiling plane (or sky) above the wall top
		int c1 = yc - 1;
		if (c1 > cb - 1) { c1 = cb - 1; }
		if (ct + 1 <= c1) {
			if (fsky) { draw_sky_col(x, ct + 1, c1); }
			else {
				draw_flat_col(x, ct + 1, c1, cflat, fceil, raydx, raydy, light, pix, palcm);
			}
		}
		// floor plane below the wall bottom
		int f0 = yf + 1;
		if (f0 < ct + 1) { f0 = ct + 1; }
		if (f0 <= cb - 1) {
			draw_flat_col(x, f0, cb - 1, fflat, ffloor, raydx, raydy, light, pix, palcm);
		}

		if (solid) {
			int w0 = yc; if (w0 < ct + 1) { w0 = ct + 1; }
			int w1 = yf; if (w1 > cb - 1) { w1 = cb - 1; }
			if (w0 <= w1) {
				if (facet >= 0) {
					int vfx = math::itrunc(((aface - VIEW_Z + yofs) - (CENTERY - w0) / sc) * 65536.0);
					draw_tex_col(x, w0, w1, ftp, ftw, fth, ucol, vfx, dvfx, band, pix, palcm);
				} else {
					draw_tex_col(x, w0, w1, flat0, 64, 64, ucol, 0, dvfx, band, pix, palcm);
				}
			}
			done[x] = 1;
			REMAIN = REMAIN - 1;
			WALLZ[x] = dist;
			continue;
		}

		int newct = ct;
		if (bceil < fceil) {
			int ybc = math::itrunc(CENTERY - (bceil - VIEW_Z) * sc);
			int u0 = yc; if (u0 < ct + 1) { u0 = ct + 1; }
			int u1i = ybc - 1; if (u1i > cb - 1) { u1i = cb - 1; }
			if (u0 <= u1i) {
				if (fsky && bsky) { draw_sky_col(x, u0, u1i); }
				else if (topt >= 0) {
					int vfxu = math::itrunc(((atop - VIEW_Z + yofs) - (CENTERY - u0) / sc) * 65536.0);
					draw_tex_col(x, u0, u1i, ttp, ttw, tth, ucol, vfxu, dvfx, band, pix, palcm);
				}
			}
			if (ybc - 1 > newct) { newct = ybc - 1; }
		} else {
			if (yc - 1 > newct) { newct = yc - 1; }
		}
		int newcb = cb;
		if (bfloor > ffloor) {
			int ybf = math::itrunc(CENTERY - (bfloor - VIEW_Z) * sc);
			int l0 = ybf; if (l0 < newct + 1) { l0 = newct + 1; }
			int l1 = yf; if (l1 > cb - 1) { l1 = cb - 1; }
			if (l0 <= l1 && bott >= 0) {
				int vfxl = math::itrunc(((abot - VIEW_Z + yofs) - (CENTERY - l0) / sc) * 65536.0);
				draw_tex_col(x, l0, l1, btp, btw, bth, ucol, vfxl, dvfx, band, pix, palcm);
			}
			if (ybf < newcb) { newcb = ybf; }
		} else {
			if (yf + 1 < newcb) { newcb = yf + 1; }
		}
		if (newct != ct || newcb != cb) {
			int cn = clipsn[x];
			if (cn < 4) {
				int ci = x * 4 + cn;
				clipsd[ci] = dist;
				clipst[ci] = newct;
				clipsb[ci] = newcb;
				clipsn[x] = cn + 1;
			} else {
				int ci2 = x * 4 + 3;
				clipst[ci2] = newct;
				clipsb[ci2] = newcb;
			}
		}
		cliptop[x] = newct;
		clipbot[x] = newcb;
		if (newct + 1 >= newcb) {
			done[x] = 1;
			REMAIN = REMAIN - 1;
			WALLZ[x] = dist;
		}
	}
}

function bsp_subsector(int ss) -> void {
	int first = SS_FIRST[ss];
	int stop = first + SS_COUNT[ss];
	for (int i = first; i < stop; ++i) { draw_seg(i); }
}

function bsp_node(int n) -> void {
	if (REMAIN <= 0) { return; }
	if ((n & 0x8000) != 0) {
		bsp_subsector(n & 0x7FFF);
		return;
	}
	int side = point_side(VIEW_X, VIEW_Y, n);
	if (side == 0) {
		bsp_node(ND_RIGHT[n]);
		if (REMAIN > 0) { bsp_node(ND_LEFT[n]); }
	} else {
		bsp_node(ND_LEFT[n]);
		if (REMAIN > 0) { bsp_node(ND_RIGHT[n]); }
	}
}

function render_view(float px, float py, float ang) -> void {
	VIEW_X = px;
	VIEW_Y = py;
	VIEW_ANG = ang;
	if (EYE_Z > -99999.0) { VIEW_Z = EYE_Z; }
	else { VIEW_Z = SE_FLOORH[sector_at(px, py)] + 41.0; }
	VSIN = math::sin(ang);
	VCOS = math::cos(ang);
	CENTERX = VW / 2;
	CENTERY = VIEWH / 2;
	FOCAL = 1.0 * CENTERX;
	FOCALY = FOCAL * PXASPECT;

	pix_clear(0);
	if (CLIPTOP.size() != VW) {
		CLIPTOP = [];
		CLIPBOT = [];
		COLDONE = [];
		WALLZ = [];
		for (int i = 0; i < VW; ++i) {
			CLIPTOP.push(-1);
			CLIPBOT.push(VIEWH);
			COLDONE.push(0);
			WALLZ.push(1000000000.0);
		}
	} else {
		var& rct = CLIPTOP;
		var& rcb = CLIPBOT;
		var& rcd = COLDONE;
		var& rwz = WALLZ;
		for (int i = 0; i < VW; ++i) {
			rct[i] = -1;
			rcb[i] = VIEWH;
			rcd[i] = 0;
			rwz[i] = 1000000000.0;
		}
	}
	if (CLIPS_N.size() != VW) {
		CLIPS_N = []; CLIPS_D = []; CLIPS_T = []; CLIPS_B = [];
		int total = VW * 4;
		for (int i2 = 0; i2 < VW; ++i2) { CLIPS_N.push(0); }
		for (int i3 = 0; i3 < total; ++i3) {
			CLIPS_D.push(0.0);
			CLIPS_T.push(0);
			CLIPS_B.push(0);
		}
	} else {
		for (int i4 = 0; i4 < VW; ++i4) { CLIPS_N[i4] = 0; }
	}
	MSEG = [];
	MSEG_D = [];
	MASKED_DRAWN = 0;
	FUZZ_T = 1 - FUZZ_T;
	REMAIN = VW;
	bsp_node(ND_X.size() - 1);
	draw_things();
}

// ---------------------------------------------------------------- sprites ---

int SPRITES_DRAWN = 0;
int SPR_DBG_M = -1;           // gate hook: track the lowest row drawn for this mobj
int SPR_DBG_YMAX = -1;

// far-to-near painter's order over sprites AND deferred masked walls
// (mobj entries are the index, masked entries are -(k+1) into MSEG)
function draw_things() -> void {
	SPRITES_DRAWN = 0;
	array<int> order = [];
	array<float> depth = [];
	int n = MO_X.size();
	for (int m = 0; m < n; ++m) {
		if (MO_STATE[m] == 0) { continue; }
		float dx = MO_X[m] - VIEW_X;
		float dy = MO_Y[m] - VIEW_Y;
		float dep = dx * VCOS + dy * VSIN;
		if (dep < 4.0) { continue; }
		int j = order.size() - 1;
		order.push(0);
		depth.push(0.0);
		while (j >= 0 && depth[j] < dep) {
			order[j + 1] = order[j];
			depth[j + 1] = depth[j];
			j = j - 1;
		}
		order[j + 1] = m;
		depth[j + 1] = dep;
	}
	int nm = MSEG.size();
	for (int k = 0; k < nm; ++k) {
		float mdep = MSEG_D[k];
		int j2 = order.size() - 1;
		order.push(0);
		depth.push(0.0);
		while (j2 >= 0 && depth[j2] < mdep) {
			order[j2 + 1] = order[j2];
			depth[j2 + 1] = depth[j2];
			j2 = j2 - 1;
		}
		order[j2 + 1] = 0 - (k + 1);
		depth[j2 + 1] = mdep;
	}
	for (int k2 = 0; k2 < order.size(); ++k2) {
		int e = order[k2];
		if (e >= 0) { draw_sprite(e); }
		else { draw_masked(MSEG[0 - e - 1]); }
	}
}

// a two-sided seg's mid texture: clipped to the opening, never tiled
// vertically, holes transparent — drawn at its depth among the sprites
function draw_masked(int sg) -> void {
	float x1w = VX_X[SG_V1[sg]];
	float y1w = VX_Y[SG_V1[sg]];
	float x2w = VX_X[SG_V2[sg]];
	float y2w = VX_Y[SG_V2[sg]];
	float dx1 = x1w - VIEW_X; float dy1 = y1w - VIEW_Y;
	float dx2 = x2w - VIEW_X; float dy2 = y2w - VIEW_Y;
	float dep1 = dx1 * VCOS + dy1 * VSIN;
	float lat1 = dx1 * VSIN - dy1 * VCOS;
	float dep2 = dx2 * VCOS + dy2 * VSIN;
	float lat2 = dx2 * VSIN - dy2 * VCOS;
	float seglen = SEG_LEN[sg];
	float u1 = 0.0;
	float u2 = seglen;
	float neareps = 0.05;
	if (dep1 < neareps && dep2 < neareps) { return; }
	if (dep1 < neareps) {
		float t = (neareps - dep1) / (dep2 - dep1);
		dep1 = neareps;
		lat1 = lat1 + (lat2 - lat1) * t;
		u1 = t * seglen;
	} else if (dep2 < neareps) {
		float t2 = (neareps - dep2) / (dep1 - dep2);
		dep2 = neareps;
		lat2 = lat2 + (lat1 - lat2) * t2;
		u2 = seglen * (1.0 - t2);
	}
	float x1f = CENTERX + lat1 / dep1 * FOCAL;
	float x2f = CENTERX + lat2 / dep2 * FOCAL;
	if (x2f <= x1f) { return; }
	if (x1f >= VW || x2f < 0.0) { return; }

	int fsec = SEG_FSEC[sg];
	int bsec = SEG_BSEC[sg];
	int midt = SEG_MIDT[sg];
	tex_masked(midt);
	var& mp = MTEX_PIX[midt];
	int tw = TEX_W[midt];
	int th = TEX_H[midt];
	float opentop = SE_CEILH[fsec] < SE_CEILH[bsec] ? SE_CEILH[fsec] : SE_CEILH[bsec];
	float openbot = SE_FLOORH[fsec] > SE_FLOORH[bsec] ? SE_FLOORH[fsec] : SE_FLOORH[bsec];
	if (opentop <= openbot) { return; }
	int light = SE_LIGHT[fsec];
	float aface = SEG_PEGB[sg] != 0 ? openbot + th : opentop;
	int xofs = SEG_XOFS[sg];
	int yofs = SEG_YOFS[sg];

	float invz1 = 1.0 / dep1;
	float invz2 = 1.0 / dep2;
	float uz1 = u1 * invz1;
	float uz2 = u2 * invz2;
	float spanx = x2f - x1f;
	int xa = math::itrunc(x1f < 0.0 ? 0.0 : x1f + 0.999);
	int xb = math::itrunc(x2f >= VW ? VW - 1 : x2f);
	if (xb >= VW) { xb = VW - 1; }
	var& pix = PIX;
	var& palcm = PALCM;
	var& wallz = WALLZ;
	var& clipsn = CLIPS_N;
	var& clipsd = CLIPS_D;
	var& clipst = CLIPS_T;
	var& clipsb = CLIPS_B;
	for (int x = xa; x <= xb; ++x) {
		float t3 = (x - x1f) / spanx;
		float invz = invz1 + (invz2 - invz1) * t3;
		float dist = 1.0 / invz;
		if (dist >= wallz[x]) { continue; }
		float sc = invz * FOCALY;
		int y0 = math::itrunc(CENTERY - (opentop - VIEW_Z) * sc);
		int y1 = math::itrunc(CENTERY - (openbot - VIEW_Z) * sc);
		int cn = clipsn[x];
		if (cn > 0) {
			int cbase = x * 4;
			int wi = -1;
			for (int ci = 0; ci < cn; ++ci) {
				if (clipsd[cbase + ci] >= dist - 0.01) { break; }   // own snapshot excluded
				wi = cbase + ci;
			}
			if (wi >= 0) {
				if (y0 < clipst[wi] + 1) { y0 = clipst[wi] + 1; }
				if (y1 > clipsb[wi] - 1) { y1 = clipsb[wi] - 1; }
			}
		}
		if (y0 < 0) { y0 = 0; }
		if (y1 >= VIEWH) { y1 = VIEWH - 1; }
		if (y0 > y1) { continue; }
		float u = (uz1 + (uz2 - uz1) * t3) * dist;
		int tc = (math::itrunc(u) + xofs) % tw;
		if (tc < 0) { tc = tc + tw; }
		int base = tc * th;
		int band = light_band(light, dist);
		int pcm = band * 256;
		int vfx = math::itrunc(((aface - VIEW_Z + yofs) - (CENTERY - y0) / sc) * 65536.0);
		int dvfx = math::itrunc(65536.0 / sc);
		bool painted = false;
		for (int y = y0; y <= y1; ++y) {
			int iv = vfx >> 16;
			vfx = vfx + dvfx;
			if (iv < 0 || iv >= th) { continue; }    // no vertical tiling: past the art = hole
			int texel = mp[base + iv];
			if (texel >= 0) { pix[y * VW + x] = palcm[pcm + texel]; painted = true; }
		}
		if (painted) { MASKED_DRAWN = MASKED_DRAWN + 1; }
	}
}

function draw_sprite(int m) -> void {
	float dx = MO_X[m] - VIEW_X;
	float dy = MO_Y[m] - VIEW_Y;
	float dep = dx * VCOS + dy * VSIN;
	float lat = dx * VSIN - dy * VCOS;
	float xc = CENTERX + lat / dep * FOCAL;

	int fi = MO_FRAME[m];
	if (fi >= MO_ANIM[m].size()) { fi = MO_ANIM[m].size() - 1; }
	string frame = MO_ANIM[m].substr(fi, 1);
	float rel = math::atan2(VIEW_Y - MO_Y[m], VIEW_X - MO_X[m]) - MO_ANG[m];
	while (rel < 0.0) { rel = rel + math::TAU; }
	int rot = math::itrunc((rel + math::PI / 8.0) / (math::PI / 4.0)) & 7;
	int lump = sprite_lookup(MO_SPR[m], frame, rot);
	if (lump < 0) { return; }
	SPRITES_DRAWN = SPRITES_DRAWN + 1;
	int flip = SPR_LAST_FLIP;
	sprite_prepare(lump);
	int w = SPR_W[lump];
	int h = SPR_H[lump];

	float xscale = FOCAL / dep;
	float yscale = FOCALY / dep;
	float x0f = xc - SPR_LO[lump] * xscale;
	float topz = MO_Z[m] + SPR_TO[lump];
	float y0f = CENTERY - (topz - VIEW_Z) * yscale;
	float dv = dep / FOCALY;                          // world units per screen row

	int xa = math::itrunc(x0f < 0.0 ? 0.0 : x0f + 0.999);
	int xb = math::itrunc(x0f + w * xscale);
	if (xb >= VW) { xb = VW - 1; }
	if (xa > xb || xa >= VW || xb < 0) { return; }

	int st = MO_STATE[m];
	int band = 0;
	if (st != 8 && MO_SPR[m] != "BEXP" && MO_SPR[m] != "BAL1" && MO_SPR[m] != "MISL") {
		band = light_band(SE_LIGHT[sector_at(MO_X[m], MO_Y[m])], dep);
	}
	int pcm = band * 256;
	bool fuzz = MO_TYPE[m] == 58;                    // spectre: warp the pixels behind it

	var& tp = SPR_PIX[lump];
	var& pix = PIX;
	var& palcm = PALCM;
	var& wallz = WALLZ;
	var& clipsn = CLIPS_N;
	var& clipsd = CLIPS_D;
	var& clipst = CLIPS_T;
	var& clipsb = CLIPS_B;
	for (int sx = xa; sx <= xb; ++sx) {
		if (dep >= wallz[sx]) { continue; }
		int tc = math::itrunc((sx - x0f) / xscale);
		if (tc < 0) { tc = 0; }
		if (tc >= w) { tc = w - 1; }
		if (flip != 0) { tc = w - 1 - tc; }
		int base = tc * h;
		int y0 = math::itrunc(y0f < 0.0 ? 0.0 : y0f + 0.999);
		int y1 = math::itrunc(y0f + h * yscale);
		// clip to the window AS OF our depth: the tightest snapshot from a
		// portal NEARER than us (windows only narrow, so the last one wins)
		int cn = clipsn[sx];
		if (cn > 0) {
			int cbase = sx * 4;
			int wi = -1;
			for (int ci = 0; ci < cn; ++ci) {
				if (clipsd[cbase + ci] >= dep) { break; }
				wi = cbase + ci;
			}
			if (wi >= 0) {
				if (y0 < clipst[wi] + 1) { y0 = clipst[wi] + 1; }
				if (y1 > clipsb[wi] - 1) { y1 = clipsb[wi] - 1; }
			}
		}
		if (y1 >= VIEWH) { y1 = VIEWH - 1; }
		if (m == SPR_DBG_M && y1 > SPR_DBG_YMAX) { SPR_DBG_YMAX = y1; }
		if (fuzz) {
			// alternating checkerboard: darken what's already behind the
			// silhouette, skip the other half — the classic shimmer
			for (int fy = y0; fy <= y1; ++fy) {
				int fiv = math::itrunc((fy - y0f) * dv);
				if (fiv < 0 || fiv >= h) { continue; }
				if (tp[base + fiv] < 0) { continue; }
				if (((sx + fy + FUZZ_T) & 1) != 0) { continue; }
				int fpi = fy * VW + sx;
				pix[fpi] = fuzz_dark(pix[fpi]);
			}
			continue;
		}
		for (int y = y0; y <= y1; ++y) {
			int iv = math::itrunc((y - y0f) * dv);
			if (iv < 0 || iv >= h) { continue; }
			int texel = tp[base + iv];
			if (texel >= 0) { pix[y * VW + sx] = palcm[pcm + texel]; }
		}
	}
}

// interned color -> its half-bright twin, built lazily (a couple light bands
// down reads as the spectre's smoke)
map<int, int> DARK_MAP = {};
int FUZZ_T = 0;               // parity flip per frame so the checkerboard crawls

function fuzz_dark(int c) -> int {
	if (DARK_MAP.has(c)) { return DARK_MAP[c]; }
	int d = rgb_idx(PAL_R[c] / 2, PAL_G[c] / 2, PAL_B[c] / 2);
	DARK_MAP[c] = d;
	return d;
}

// first-person weapon overlay: the base gun is bottom-anchored and centered;
// overlays (muzzle flash) place themselves via the lumps' own offset deltas
// relative to the base — the WAD's flash art is authored to line up that way
float PSP_X0 = 0.0;
float PSP_Y0 = 0.0;

function draw_psprite(int lump, float x0f, float y0f, float scale) -> void {
	int w = SPR_W[lump];
	int h = SPR_H[lump];
	var& tp = SPR_PIX[lump];
	var& pix = PIX;
	var& palcm = PALCM;
	int xa = math::itrunc(x0f < 0.0 ? 0.0 : x0f);
	int xb = math::itrunc(x0f + w * scale);
	if (xb >= VW) { xb = VW - 1; }
	for (int sx = xa; sx <= xb; ++sx) {
		int tc = math::itrunc((sx - x0f) / scale);
		if (tc < 0 || tc >= w) { continue; }
		int base = tc * h;
		int ya = math::itrunc(y0f < 0.0 ? 0.0 : y0f);
		for (int y = ya; y < VIEWH; ++y) {
			int iv = math::itrunc((y - y0f) / scale);
			if (iv < 0 || iv >= h) { continue; }
			int texel = tp[base + iv];
			if (texel >= 0) { pix[y * VW + sx] = palcm[texel]; }
		}
	}
}

// doom's absolute psprite placement: weapon lumps carry their own position in
// the 320x200 weapon space (WEAPONTOP 32 raised, BASEYCENTER 100; texturemid =
// 100.5 - (32 - topofs)). Bottom-anchoring only LOOKED right because the
// pistol's offsets happen to land there — tall guns (the launcher, 79 rows)
// are authored to overhang the screen bottom, so anchoring drew them ~30 rows
// high. Flashes carry their own offsets, so gun and flash place identically.
function psprite_place(int lump, float scale) -> void {
	float texmid = 68.5 + SPR_TO[lump];
	PSP_X0 = VW / 2.0 + (1.0 - 160.0 - SPR_LO[lump]) * scale + WBOB_X * scale;
	PSP_Y0 = VIEWH / 2.0 - texmid * scale + WBOB_Y * scale;
}

function draw_weapon(string spr4, string frame) -> void {
	int lump = sprite_lookup(spr4, frame, 8);         // rot-0 fallback path
	if (lump < 0) { return; }
	sprite_prepare(lump);
	float scale = VW / 320.0;
	psprite_place(lump, scale);
	draw_psprite(lump, PSP_X0, PSP_Y0, scale);
}

function draw_weapon_flash(string spr4, string frame) -> void {
	int lump = sprite_lookup(spr4, frame, 8);
	if (lump < 0) { return; }
	sprite_prepare(lump);
	float scale = VW / 320.0;
	psprite_place(lump, scale);
	draw_psprite(lump, PSP_X0, PSP_Y0, scale);
}
