// Gator Quest - the world view: an eased camera over the player, per-tile light
// (day/night, torch, lava, fire, glowing monsters) interpolated per pixel with
// a Bayer dither, animated tiles, ground loot, sprites with their gear, and
// all the juice: particles, projectiles, floating numbers, hit flashes,
// screen shake, weather and banners. Presentation randomness never touches
// the sim RNG.

import "gq_art.jai";
import "gq_cstate.jai";

int prand(int n) {
	PRNG = (PRNG * 1103515245 + 12345) & 2147483647;
	return n <= 1 ? 0 : (PRNG >> 8) % n;
}

float prandf() { return prand(10000) / 10000.0; }

// ---- per-world night tints
array<int> WORLD_TINT = [22, 30, 66, 18, 36, 44, 8, 26, 50, 22, 32, 20, 30, 42, 64, 64, 16, 8, 60, 70, 104, 26, 26, 12, 34, 22, 10];
// how warm torchlight, dawn and dusk run in each realm (percent): ice and the deep stay cooler, the sky not at all
array<int> WORLD_WARM = [100, 100, 60, 100, 45, 100, 0, 100, 100];

void ensure_shade(ClientView& v) {
	int ghost = v.ghost;
	if (SHADE_WORLD == v.world && SHADE_GHOST == ghost) { return; }
	SHADE_WORLD = v.world;
	SHADE_GHOST = ghost;
	int w = v.world;
	// the day/night realms get warm sunlight
	shade_build(WORLD_TINT[w * 3], WORLD_TINT[w * 3 + 1], WORLD_TINT[w * 3 + 2], ghost != 0, WORLD_WARM[w], WORLDS[w].ambient == 0 ? 22 : 0);
}

// 0..15 ambient light for the view
int ambient_level(ClientView& v) {
	WorldDef wd = WORLDS[v.world];
	int lvl = 15;
	if (wd.ambient > 0) { lvl = wd.ambient; }
	else {
		int hour = (v.tod * 24 / DAY_TICKS + 7) % 24;
		int minute = (v.tod * 24 * 60 / DAY_TICKS) % 60;
		if (hour >= 8 && hour < 18) { lvl = 15; }
		else if (hour >= 20 || hour < 5) { lvl = 3; }
		else if (hour >= 18) { lvl = 15 - ((hour - 18) * 60 + minute) * 12 / 120; }
		else { lvl = 3 + ((hour - 5) * 60 + minute) * 12 / 180; }
	}
	if (v.weather == 3) { lvl = lvl - 3; }
	else if (v.weather == 4) { lvl = lvl - 2; }
	else if (v.weather == 1) { lvl = lvl - 1; }
	if (v.buff_night > 0) { lvl = lvl + 7; }
	if (v.ghost != 0) { lvl = lvl + 4; }
	if (LIGHTNING_F > 0) { lvl = 15; }
	return iclamp(lvl, 1, 15);
}

string time_label(ClientView& v) {
	int hour = (v.tod * 24 / DAY_TICKS + 7) % 24;
	int minute = (v.tod * 24 * 60 / DAY_TICKS) % 60;
	string ampm = hour < 12 ? "am" : "pm";
	int h12 = hour % 12 == 0 ? 12 : hour % 12;
	return `${h12}:${minute:02}${ampm}`;
}

// ---- the light grid: one level per tile over the view (plus a margin), 16x fixed point
array<int> LG;
int LG_W;
int LG_H;
int LG_X0;
int LG_Y0;

void light_emit(float ex, float ey, float r, int strength) {
	int x0 = imax(0, math::itrunc(ex - r) - LG_X0);
	int x1 = imin(LG_W - 1, math::itrunc(ex + r) + 1 - LG_X0);
	int y0 = imax(0, math::itrunc(ey - r) - LG_Y0);
	int y1 = imin(LG_H - 1, math::itrunc(ey + r) + 1 - LG_Y0);
	float r2 = r * r;
	for (int y = y0; y <= y1; ++y) {
		float dy = (y + LG_Y0) + 0.5 - ey;
		for (int x = x0; x <= x1; ++x) {
			float dx = (x + LG_X0) + 0.5 - ex;
			float d2 = dx * dx + dy * dy;
			if (d2 >= r2) { continue; }
			int lvl = math::itrunc(strength * 16 * (1.0 - d2 / r2));
			int i = y * LG_W + x;
			if (lvl > LG[i]) { LG[i] = lvl; }
		}
	}
}

// interpolated tile position of a mobile at a (fractional) server tick: its current move, or the
// one before when the tick is earlier (others are drawn a little behind the clock); the local
// gator is drawn where the prediction has it
float move_lerp(int from, int to, float vtick, int mt, int md) {
	if (iabs(to - from) > 2) { return to * 1.0; }
	float t = (vtick - mt) / md;
	if (t < 0.0) { t = 0.0; }
	if (t > 1.0) { t = 1.0; }
	return from + (to - from) * t;
}

float mob_vx(ClientView& v, int i, float vtick) {
	if (v.m_ser[i] == v.my_ser && pred_active(v)) { return pred_draw_x(NOW_MS); }
	if (vtick >= v.m_mt[i] || v.m_ppx.size() <= i) { return move_lerp(v.m_px[i], v.m_x[i], vtick, v.m_mt[i], v.m_md[i]); }
	return move_lerp(v.m_ppx[i], v.m_px[i], vtick, v.m_pmt[i], v.m_pmd[i]);
}

float mob_vy(ClientView& v, int i, float vtick) {
	if (v.m_ser[i] == v.my_ser && pred_active(v)) { return pred_draw_y(NOW_MS); }
	if (vtick >= v.m_mt[i] || v.m_ppy.size() <= i) { return move_lerp(v.m_py[i], v.m_y[i], vtick, v.m_mt[i], v.m_md[i]); }
	return move_lerp(v.m_ppy[i], v.m_py[i], vtick, v.m_pmt[i], v.m_pmd[i]);
}

float view_tick(ClientView& v) {
	float f = (NOW_MS - v.rx_ms) / (TICK_MS * 1.0);
	if (f < 0.0) { f = 0.0; }
	if (f > 1.0) { f = 1.0; }
	return v.tick + f;
}

// ---- drawing
void draw_tiles(ClientView& v, int camx, int camy, int amb) {
	int S = TS;
	int w = v.world;
	int tx0 = (camx - VW / 2) / S - 1;
	int ty0 = (camy - VH / 2) / S - 1;
	if (camx - VW / 2 < 0) { tx0 = tx0 - 1; }
	if (camy - VH / 2 < 0) { ty0 = ty0 - 1; }
	int tw = VW / S + 3;
	int th = VH / S + 3;
	VIEW_TX0 = tx0;
	VIEW_TY0 = ty0;
	// light grid covers the view plus one tile each side
	LG_X0 = tx0 - 1;
	LG_Y0 = ty0 - 1;
	LG_W = tw + 2;
	LG_H = th + 2;
	int n = LG_W * LG_H;
	while (LG.size() < n) { LG.push(0); }
	int base = amb * 16;
	for (int i = 0; i < n; ++i) { LG[i] = base; }
	int frame = (FRAME / 6) % 4;
	if (amb < 15) {
		for (int ty = LG_Y0 - 5; ty < LG_Y0 + LG_H + 5; ++ty) {
			for (int tx = LG_X0 - 5; tx < LG_X0 + LG_W + 5; ++tx) {
				int t = cv_tile(v, tx, ty);
				int l = T_LIGHT[t];
				if (l > 0) {
					float flick = (t == T_FIRE || t == T_SCONCE || t == T_FORGE) ? (prand(10) - 5) * 0.05 : 0.0;
					light_emit(tx + 0.5, ty + 0.5, l + 0.5 + flick, t == T_LAVA ? 11 : 14);
				}
				else if (t == T_DOOR && amb <= 9) { light_emit(tx + 0.5, ty + 0.9, 2.3, 10); }
			}
		}
		float vt = render_tick(v);
		for (int i = 0; i < v.m_ser.size(); ++i) {
			int kind = v.m_kind[i];
			int l = MONS[kind].light;
			bool me = v.m_ser[i] == v.my_ser;
			if (me) {
				// your torch, with a lively flicker
				float r = v.buff_night > 0 ? 8.5 : LIVE_TORCH + 0.2;
				light_emit(mob_vx(v, i, vt) + 0.5, mob_vy(v, i, vt) + 0.5, r + (prand(7) - 3) * 0.06, 15);
			} else if (v.m_slot[i] >= 0) {
				light_emit(mob_vx(v, i, vt) + 0.5, mob_vy(v, i, vt) + 0.5, 3.5, 12);
			} else if (l > 0) {
				light_emit(mob_vx(v, i, vt) + 0.5, mob_vy(v, i, vt) + 0.5, l + 0.5, 13);
			}
		}
		for (int p = 0; p < P_X.size(); ++p) {
			if ((P_FLAGS[p] & 1) != 0 && P_LIFE[p] > 0 && p % 3 == 0) { light_emit(P_X[p], P_Y[p], 1.6, 10); }
		}
		fx_lights();
	}
	fx_vignette(amb);
	// past a walled realm's edge the rock fades into the dark over four tiles
	if (w != 0 && w != 6) {
		for (int ly = 0; ly < LG_H; ++ly) {
			int wy = LG_Y0 + ly;
			int oy = wy < 0 ? -wy : (wy >= W_H[w] ? wy - W_H[w] + 1 : 0);
			for (int lx = 0; lx < LG_W; ++lx) {
				int wx = LG_X0 + lx;
				int ox = wx < 0 ? -wx : (wx >= W_W[w] ? wx - W_W[w] + 1 : 0);
				int out = imax(ox, oy);
				if (out > 0) { LG[ly * LG_W + lx] = LG[ly * LG_W + lx] * imax(0, 4 - out) / 5; }
			}
		}
	}
	int nb = NBASE;
	bool clouds = fx_cloud_grid(v, tx0, ty0, tw, th, amb);
	int cw = tw + 1;
	int cloud_rows = NLEV * nb;
	for (int ty = ty0; ty < ty0 + th; ++ty) {
		int sy0 = ty * S - camy + VH / 2;
		if (sy0 >= VH || sy0 + S <= 0) { continue; }
		for (int tx = tx0; tx < tx0 + tw; ++tx) {
			int sx0 = tx * S - camx + VW / 2;
			if (sx0 >= VW || sx0 + S <= 0) { continue; }
			int t = cv_tile(v, tx, ty);
			int vr = in_world(w, tx, ty) ? TVAR[W_OFF[w] + ty * W_W[w] + tx] & 3 : hash3(tx, ty, 7) & 3;
			int anim = TILE_ANIM[t];
			int f = 0;
			if (anim == ANIM_WATER) { f = frame; }
			else if (anim == ANIM_FLICKER) { f = (frame + vr) % 4; }
			else if (anim == ANIM_BREEZE) { f = (FRAME / 10 - ((tx + ty) >> 2)) & 3; }
			int src = TEX_OFF[(t * 4 + vr) * 4 + f];
			// the four corner light levels: averages of the tile centers that meet there
			int lx = tx - LG_X0;
			int ly = ty - LG_Y0;
			int c00 = (LG[(ly - 1) * LG_W + lx - 1] + LG[(ly - 1) * LG_W + lx] + LG[ly * LG_W + lx - 1] + LG[ly * LG_W + lx]) / 4;
			int c10 = (LG[(ly - 1) * LG_W + lx] + LG[(ly - 1) * LG_W + lx + 1] + LG[ly * LG_W + lx] + LG[ly * LG_W + lx + 1]) / 4;
			int c01 = (LG[ly * LG_W + lx - 1] + LG[ly * LG_W + lx] + LG[(ly + 1) * LG_W + lx - 1] + LG[(ly + 1) * LG_W + lx]) / 4;
			int c11 = (LG[ly * LG_W + lx] + LG[ly * LG_W + lx + 1] + LG[(ly + 1) * LG_W + lx] + LG[(ly + 1) * LG_W + lx + 1]) / 4;
			int ya = imax(0, -sy0);
			int yb = imin(S, VH - sy0);
			int xa = imax(0, -sx0);
			int xb = imin(S, VW - sx0);
			// a cloud's shadow over the tile: none, whole (the shadowed rows), or its dithered edge
			int k00 = 0;
			int k10 = 0;
			int k01 = 0;
			int k11 = 0;
			if (clouds) {
				int ci = (ty - ty0) * cw + tx - tx0;
				k00 = CLOUD_COV[ci];
				k10 = CLOUD_COV[ci + 1];
				k01 = CLOUD_COV[ci + cw];
				k11 = CLOUD_COV[ci + cw + 1];
			}
			int ksum = k00 + k10 + k01 + k11;
			int shadow = ksum == 64 ? cloud_rows : 0;
			bool edge = ksum != 0 && ksum != 64;
			if (!edge && c00 == c10 && c00 == c01 && c00 == c11 && c00 % 16 == 0) {
				int sh = (c00 >= 240 ? NLEV - 1 : c00 / 16) * nb + shadow;
				for (int py = ya; py < yb; ++py) {
					int dst = (sy0 + py) * VW + sx0;
					int s2 = src + py * S;
					for (int px = xa; px < xb; ++px) { PIX[dst + px] = SHADE[sh + TEX[s2 + px]]; }
				}
				continue;
			}
			if (c00 == c10 && c00 == c01 && c00 == c11 && c00 % 16 == 0) {
				// even light, a cloud's edge: only the shadow's dither varies
				int even = (c00 >= 240 ? NLEV - 1 : c00 / 16) * nb;
				for (int py = ya; py < yb; ++py) {
					int dst = (sy0 + py) * VW + sx0;
					int s2 = src + py * S;
					int krow = ((sy0 + py + 2) & 3) * 4;
					int kl = k00 * S + (k01 - k00) * py;
					int kr = k10 * S + (k11 - k10) * py;
					for (int px = xa; px < xb; ++px) {
						int sh = (kl * S + (kr - kl) * px) / (S * S) > BAYER[krow + ((sx0 + px + 1) & 3)] ? even + cloud_rows : even;
						PIX[dst + px] = SHADE[sh + TEX[s2 + px]];
					}
				}
				continue;
			}
			for (int py = ya; py < yb; ++py) {
				int left = c00 + (c01 - c00) * py / S;
				int right = c10 + (c11 - c10) * py / S;
				int dst = (sy0 + py) * VW + sx0;
				int s2 = src + py * S;
				int brow = ((sy0 + py) & 3) * 4;
				int krow = ((sy0 + py + 2) & 3) * 4;
				int kl = k00 * S + (k01 - k00) * py;
				int kr = k10 * S + (k11 - k10) * py;
				for (int px = xa; px < xb; ++px) {
					int l16 = left + (right - left) * px / S;
					int lev = (l16 + BAYER[brow + ((sx0 + px) & 3)]) / 16;
					if (lev > 15) { lev = 15; }
					int sh = shadow;
					if (edge && (kl * S + (kr - kl) * px) / (S * S) > BAYER[krow + ((sx0 + px + 1) & 3)]) { sh = cloud_rows; }
					PIX[dst + px] = SHADE[lev * nb + sh + TEX[s2 + px]];
				}
			}
		}
	}
	draw_shore_foam(v, tx0, ty0, tw, th, camx, camy);
}

// foam along every water edge that meets land, its gaps crawling along the shore
void draw_shore_foam(ClientView& v, int tx0, int ty0, int tw, int th, int camx, int camy) {
	int S = TS;
	int nb = NBASE;
	int ph = FRAME / 8;
	for (int ty = ty0; ty < ty0 + th; ++ty) {
		int sy0 = ty * S - camy + VH / 2;
		if (sy0 >= VH || sy0 + S <= 0) { continue; }
		for (int tx = tx0; tx < tx0 + tw; ++tx) {
			int sx0 = tx * S - camx + VW / 2;
			if (sx0 >= VW || sx0 + S <= 0) { continue; }
			if (TILE_FOAMS[cv_tile(v, tx, ty)] == 0) { continue; }
			bool n = TILE_WET[cv_tile(v, tx, ty - 1)] == 0;
			bool s = TILE_WET[cv_tile(v, tx, ty + 1)] == 0;
			bool we = TILE_WET[cv_tile(v, tx - 1, ty)] == 0;
			bool ea = TILE_WET[cv_tile(v, tx + 1, ty)] == 0;
			if (!n && !s && !we && !ea) { continue; }
			int lev = light_at(tx, ty);
			int fa = SHADE[lev * nb + FOAM_A];
			int fb = SHADE[lev * nb + FOAM_B];
			for (int k = 0; k < S; ++k) {
				int along = (tx + ty) * S + k;
				bool lit = (along + ph) % 5 != 0;
				bool lap = (along - ph) % 4 == 0;
				if (n) { pset(sx0 + k, sy0, lit ? fa : fb); if (lap) { pset(sx0 + k, sy0 + 1, fb); } }
				if (s) { pset(sx0 + k, sy0 + S - 1, lit ? fa : fb); if (lap) { pset(sx0 + k, sy0 + S - 2, fb); } }
				if (we) { pset(sx0, sy0 + k, lit ? fa : fb); if (lap) { pset(sx0 + 1, sy0 + k, fb); } }
				if (ea) { pset(sx0 + S - 1, sy0 + k, lit ? fa : fb); if (lap) { pset(sx0 + S - 2, sy0 + k, fb); } }
			}
		}
	}
}

// light level (0..15) at a world tile, from the grid
int light_at(int tx, int ty) {
	int lx = tx - LG_X0;
	int ly = ty - LG_Y0;
	if (lx < 0 || ly < 0 || lx >= LG_W || ly >= LG_H) { return 15; }
	return imin(15, LG[ly * LG_W + lx] / 16);
}

int rarity_color(int r) {
	if (r == 1) { return rgb(90, 230, 90); }
	if (r == 2) { return rgb(80, 150, 255); }
	if (r == 3) { return rgb(200, 90, 255); }
	if (r == 4) { return rgb(255, 160, 40); }
	return rgb(230, 230, 230);
}

string rarity_sgr(int r) {
	if (r == 1) { return "38;2;110;230;110"; }
	if (r == 2) { return "38;2;100;160;255"; }
	if (r == 3) { return "38;2;210;110;255"; }
	if (r == 4) { return "38;2;255;170;50"; }
	return "38;2;220;220;220";
}

void draw_items(ClientView& v, int camx, int camy) {
	int S = TS;
	for (int i = 0; i < v.i_ser.size(); ++i) {
		int x = v.i_x[i];
		int y = v.i_y[i];
		int sx = x * S - camx + VW / 2;
		int sy = y * S - camy + VH / 2;
		if (sx < -S || sy < -S || sx >= VW || sy >= VH) { continue; }
		ItemDef d = ITEMS[v.i_base[i]];
		int icon = ICON_ID.get(d.spr, ICON_ID.get("bag", 0));
		string bits = ICON_BITS[icon];
		int lev = light_at(x, y);
		int col = shade_px(lev, crgb(d.col));
		int dark = shade_px(lev, cscale(d.col, 66));
		int ox = sx + (S - 5 * S / 8) / 2;
		int oy = sy + S - 4 * S / 8 - 1;
		for (int r = 0; r < 4; ++r) {
			for (int c = 0; c < 5; ++c) {
				if (bits.size() < 20 || bits[r * 5 + c] != '1') { continue; }
				int px = ox + c * S / 8;
				int py = oy + r * S / 8;
				pset(px, py, r == 3 ? dark : col);
			}
		}
		int rar = v.i_rar[i];
		if (rar >= 2 && (FRAME + i * 7) % 20 < 10) {
			pset(sx + S - 2, sy + 1 + (FRAME / 3 + i) % 3, rarity_color(rar));
		}
		if (rar >= 3) {
			// a beam of light for the good stuff
			int beam = rarity_color(rar);
			int h = 6 + (FRAME / 2 + i) % 4;
			for (int k = 1; k <= h; ++k) {
				if ((k + FRAME / 2) % 3 != 0) { pset(sx + S / 2, sy - k, beam); }
			}
		}
	}
}

// what a player gator looks like wearing its gear
int worn_color(Item it, int fallback) {
	if (it.base < 0) { return fallback; }
	return crgb(ITEMS[it.base].col);
}

void draw_sprite(int off, int sx, int sy, bool flip, int lev, int mode, int cut) {
	int S = TS;
	int nb = NBASE;
	int sh = lev * nb;
	for (int py = 0; py < S; ++py) {
		int y = sy + py;
		if (y < 0 || y >= VH) { continue; }
		if (cut > 0 && py >= cut) { break; }
		int row = off + py * S;
		for (int px = 0; px < S; ++px) {
			int c = SPRPIX[row + (flip ? S - 1 - px : px)];
			if (c < 0) { continue; }
			int x = sx + px;
			if (x < 0 || x >= VW) { continue; }
			if (mode == 1) { PIX[y * VW + x] = COL_WHITE; }
			else if (mode == 2) {
				if ((x + y + FRAME / 4) % 2 == 0) { PIX[y * VW + x] = SHADE_GHOST != 0 ? SHADE[15 * nb + c] : c; }
			} else { PIX[y * VW + x] = SHADE[sh + c]; }
		}
	}
}

// 2x nearest-neighbour for bosses: they fill four tiles and cast a shadow
void draw_sprite2x(int off, int sx, int sy, bool flip, int lev, int mode) {
	int S = TS;
	int sh = lev * NBASE;
	for (int py = 0; py < S * 2; ++py) {
		int y = sy + py;
		if (y < 0 || y >= VH) { continue; }
		int row = off + (py / 2) * S;
		for (int px = 0; px < S * 2; ++px) {
			int c = SPRPIX[row + (flip ? S - 1 - px / 2 : px / 2)];
			if (c < 0) { continue; }
			int x = sx + px;
			if (x < 0 || x >= VW) { continue; }
			PIX[y * VW + x] = mode == 1 ? COL_WHITE : SHADE[sh + c];
		}
	}
}

void draw_mobiles(ClientView& v, int camx, int camy) {
	int S = TS;
	float vt = render_tick(v);
	int n = v.m_ser.size();
	// painter's order: by interpolated y
	array<int> order;
	array<float> keys;
	for (int i = 0; i < n; ++i) {
		order.push(i);
		keys.push(mob_vy(v, i, vt));
	}
	for (int i = 1; i < n; ++i) {
		int oi = order[i];
		float k = keys[i];
		int j = i - 1;
		while (j >= 0 && keys[j] > k) {
			keys[j + 1] = keys[j];
			order[j + 1] = order[j];
			j = j - 1;
		}
		keys[j + 1] = k;
		order[j + 1] = oi;
	}
	int me_i = cv_me(v);
	float me_x = me_i >= 0 ? mob_vx(v, me_i, vt) : -100.0;
	float me_y = me_i >= 0 ? mob_vy(v, me_i, vt) : -100.0;
	for (int oi = 0; oi < n; ++oi) {
		int i = order[oi];
		float fx = mob_vx(v, i, vt);
		float fy = mob_vy(v, i, vt);
		int sx = math::itrunc(fx * S) - camx + VW / 2;
		int sy = math::itrunc(fy * S) - camy + VH / 2;
		int ser = v.m_ser[i];
		if (KB_F.has(ser) && KB_F[ser] > FRAME) {
			int push = KB_F[ser] - FRAME >= 3 ? 2 : 1;
			sx = sx + KB_DX[ser] * push;
			sy = sy + KB_DY[ser] * push;
		}
		if (sx < -S * 2 || sy < -S * 2 || sx >= VW + S || sy >= VH + S) { continue; }
		int kind = v.m_kind[i];
		MonDef md = MONS[kind];
		int flags = v.m_flags[i];
		bool predicted = v.m_ser[i] == v.my_ser && pred_active(v);
		int dir = predicted ? PRED_DIR : v.m_dir[i];
		bool flip = dir == 5 || dir == 6 || dir == 7;
		if ((md.fac == Fac.npc || md.fac == Fac.guard) && math::abs(me_x - fx) + math::abs(me_y - fy) < 5.0 && me_x != fx) { flip = me_x < fx; }
		bool player = v.m_slot[i] >= 0;
		bool me = v.m_ser[i] == v.my_ser;
		int spr = sprite_of(md.spr);
		string tint = md.col;
		int armor_c = -1;
		int hat_c = -1;
		int weapon_c = -1;
		if (player) {
			spr = sprite_of("gator");
			tint = "hue" + to_string(v.m_hue[i] % 16);
			if (me && v.worn.size() > 8) {
				armor_c = worn_color(v.worn[Slot.body], -1);
				hat_c = worn_color(v.worn[Slot.head], -1);
				weapon_c = worn_color(v.worn[Slot.weapon], -1);
			} else {
				armor_c = crgb("leather");
				hat_c = crgb("straw");
				weapon_c = crgb("steel");
			}
		}
		if ((flags & MF_ELITE) != 0 && FRAME % 30 < 15) { tint = "gold"; }
		int off = sprite_compiled(spr, tint, armor_c, hat_c, weapon_c);
		// walking bob and the swim cut
		bool moving = predicted ? NOW_MS < PRED_T0 + PRED_DUR && (PRED_X != PRED_FX || PRED_Y != PRED_FY) : vt - v.m_mt[i] < v.m_md[i] && (v.m_x[i] != v.m_px[i] || v.m_y[i] != v.m_py[i]);
		int stride = predicted ? PRED_STEPS : v.m_mt[i] / imax(1, v.m_md[i]);
		int bob = moving && stride % 2 == 0 ? -1 : 0;
		if (!moving && (FRAME + ser * 7) % 48 < 16) { bob = -1; }
		int tile_x = math::itrunc(fx + 0.5);
		int tile_y = math::itrunc(fy + 0.5);
		int lev = light_at(tile_x, tile_y) + (cloud_at(tile_x, tile_y) ? NLEV : 0);
		int mode = 0;
		if (FLASH.has(v.m_ser[i]) && FLASH[v.m_ser[i]] > FRAME) { mode = 1; }
		if ((flags & MF_GHOST) != 0) { mode = 2; }
		bool swim = (flags & MF_SWIM) != 0 && md.move != 4;
		int cut = swim ? S * 5 / 8 : 0;
		if ((flags & MF_BOSS) != 0) {
			// shadow, then the boss at double size
			for (int k = 0; k < S * 2 - 2; ++k) { pset(sx - S / 2 + 1 + k, sy + S, shade_px(imax(0, lev - 6), crgb("black"))); }
			draw_sprite2x(off, sx - S / 2, sy - S, flip, lev, mode);
		} else {
			draw_sprite(off, sx, sy + bob + (swim ? S * 3 / 8 : 0), flip, lev, mode, cut);
		}
		if (swim) {
			// ripples around a swimmer
			int rc = shade_px(lev, rgb(200, 230, 255));
			int ry = sy + S - 1;
			int ph = (FRAME / 5) % 3;
			pset(sx + ph, ry, rc);
			pset(sx + S - 1 - ph, ry, rc);
		}
		// health bars for the wounded (not for townsfolk)
		if (v.m_hp[i] < 100 && md.fac != Fac.npc && (flags & MF_GHOST) == 0) {
			int bw = (flags & MF_BOSS) != 0 ? S * 2 : S;
			int bx = (flags & MF_BOSS) != 0 ? sx - S / 2 : sx;
			int by = (flags & MF_BOSS) != 0 ? sy - S - 2 : sy - 2;
			int filled = bw * v.m_hp[i] / 100;
			int hc = v.m_hp[i] > 50 ? rgb(70, 220, 70) : (v.m_hp[i] > 25 ? rgb(240, 200, 40) : rgb(240, 50, 40));
			for (int k = 0; k < bw; ++k) { pset(bx + k, by, k < filled ? hc : rgb(60, 20, 20)); }
		}
		// the target marker: pulsing corners
		if (v.m_ser[i] == v.target_ser) {
			int mc = FRAME % 16 < 8 ? rgb(255, 90, 60) : rgb(255, 200, 60);
			int size = (flags & MF_BOSS) != 0 ? S * 2 : S;
			int mx = (flags & MF_BOSS) != 0 ? sx - S / 2 : sx;
			int my = (flags & MF_BOSS) != 0 ? sy - S : sy;
			pset(mx - 1, my - 1, mc);
			pset(mx, my - 1, mc);
			pset(mx - 1, my, mc);
			pset(mx + size, my - 1, mc);
			pset(mx + size - 1, my - 1, mc);
			pset(mx + size, my, mc);
			pset(mx - 1, my + size, mc);
			pset(mx - 1, my + size - 1, mc);
			pset(mx + size, my + size, mc);
			pset(mx + size, my + size - 1, mc);
		}
		if ((flags & MF_PVP) != 0 && FRAME % 20 < 10) { pset(sx + S / 2, sy - 3, rgb(255, 40, 40)); }
		if ((flags & MF_CRIMINAL) != 0) { pset(sx + S / 2 - 1, sy - 3, rgb(255, 0, 0)); pset(sx + S / 2 + 1, sy - 3, rgb(255, 0, 0)); }
	}
}

// ------------------------------------------------------------ particles ---
void particle(float x, float y, float vx, float vy, int life, int col, int flags) {
	if (P_X.size() > 700) { return; }
	int slot = -1;
	for (int i = 0; i < P_X.size(); ++i) {
		if (P_LIFE[i] <= 0) { slot = i; break; }
	}
	if (slot < 0) {
		slot = P_X.size();
		P_X.push(0.0);
		P_Y.push(0.0);
		P_VX.push(0.0);
		P_VY.push(0.0);
		P_LIFE.push(0);
		P_MAX.push(1);
		P_COL.push(0);
		P_FLAGS.push(0);
	}
	P_X[slot] = x;
	P_Y[slot] = y;
	P_VX[slot] = vx;
	P_VY[slot] = vy;
	P_LIFE[slot] = life;
	P_MAX[slot] = life;
	P_COL[slot] = col;
	P_FLAGS[slot] = flags;
}

// flags: 1 glow (ignores darkness), 2 gravity, 4 drag
void burst(int fx, float x, float y, int count) {
	for (int k = 0; k < count; ++k) {
		float a = prandf() * 6.2832;
		float sp = 0.03 + prandf() * 0.08;
		float vx = math::cos(a) * sp;
		float vy = math::sin(a) * sp;
		int life = 12 + prand(16);
		if (fx == Fx.sparks) { particle(x, y, vx * 1.6, vy * 1.6 - 0.03, 6 + prand(8), prand(2) == 0 ? rgb(255, 240, 150) : rgb(255, 255, 255), 1 + 2); }
		else if (fx == Fx.blood) { particle(x, y, vx, vy - 0.06, life, prand(3) == 0 ? rgb(150, 10, 20) : rgb(210, 30, 30), 2); }
		else if (fx == Fx.splash || fx == Fx.bubbles) { particle(x, y, vx * 0.8, -0.08 - prandf() * 0.08, life, prand(2) == 0 ? rgb(200, 230, 255) : rgb(90, 160, 230), 2); }
		else if (fx == Fx.fire) { particle(x + (prandf() - 0.5) * 0.6, y, vx * 0.3, -0.04 - prandf() * 0.05, life, prand(3) == 0 ? rgb(255, 230, 90) : (prand(2) == 0 ? rgb(255, 140, 30) : rgb(230, 60, 20)), 1 + 4); }
		else if (fx == Fx.frost) { particle(x, y, vx * 0.6, vy * 0.6, life + 10, prand(2) == 0 ? rgb(200, 240, 255) : rgb(120, 200, 255), 1 + 4); }
		else if (fx == Fx.zap) { particle(x + (prandf() - 0.5), y + (prandf() - 0.5), vx * 0.2, vy * 0.2, 4 + prand(6), prand(2) == 0 ? rgb(255, 255, 255) : rgb(120, 230, 255), 1); }
		else if (fx == Fx.heal || fx == Fx.holy) { particle(x + (prandf() - 0.5) * 0.8, y + 0.3, 0.0, -0.03 - prandf() * 0.03, life + 8, fx == Fx.holy ? (prand(2) == 0 ? rgb(255, 250, 200) : rgb(255, 220, 90)) : (prand(2) == 0 ? rgb(120, 255, 140) : rgb(220, 255, 220)), 1); }
		else if (fx == Fx.smoke || fx == Fx.dust) { particle(x + (prandf() - 0.5) * 0.5, y, vx * 0.4, -0.02 - prandf() * 0.02, life + 14, fx == Fx.dust ? rgb(150, 120, 90) : (prand(2) == 0 ? rgb(110, 110, 116) : rgb(80, 80, 86)), 4); }
		else if (fx == Fx.poison) { particle(x + (prandf() - 0.5) * 0.7, y, 0.0, -0.02 - prandf() * 0.03, life + 6, prand(2) == 0 ? rgb(120, 230, 60) : rgb(70, 170, 40), 1); }
		else if (fx == Fx.magic || fx == Fx.warp) { particle(x, y, vx * 1.2, vy * 1.2, life, prand(3) == 0 ? rgb(255, 255, 255) : (prand(2) == 0 ? rgb(170, 110, 255) : rgb(90, 140, 255)), 1 + 4); }
		else if (fx == Fx.gold || fx == Fx.levelup) { particle(x, y, vx * 0.8, -0.09 - prandf() * 0.08, life + 10, prand(3) == 0 ? rgb(255, 255, 200) : rgb(255, 200, 50), 1 + 2); }
		else if (fx == Fx.bones) { particle(x, y, vx, vy - 0.07, life, rgb(232, 222, 190), 2); }
		else { particle(x, y, vx, vy, life, rgb(255, 255, 255), 1); }
	}
}

void loot_burst(float x, float y, int rarity) {
	int col = rarity_color(rarity);
	for (int k = 0; k < 10 + rarity * 6; ++k) {
		particle(x + (prandf() - 0.5) * 0.4, y, (prandf() - 0.5) * 0.04, -0.05 - prandf() * 0.1, 18 + prand(14), prand(3) == 0 ? rgb(255, 255, 255) : col, 1 + 2);
	}
}

void particles_update() {
	critters_step();
	if (HURT_F > 0) { HURT_F = HURT_F - 1; }
	if (BOLT_F > 0) { BOLT_F = BOLT_F - 1; }
	if (RELOAD_F > 0) { RELOAD_F = RELOAD_F - 1; }
	if (ARRIVE_F > 0) { ARRIVE_F = ARRIVE_F - 1; }
	for (int i = 0; i < FT_X.size(); ++i) {
		if (FT_T[i] > 0) {
			FT_T[i] = FT_T[i] - 1;
			FT_Y[i] = FT_Y[i] + FT_VY[i];
			FT_VY[i] = FT_VY[i] * 0.84;
		}
	}
	if (SHAKE_F > 0) { SHAKE_F = SHAKE_F - 1; }
	if (LIGHTNING_F > 0) { LIGHTNING_F = LIGHTNING_F - 1; }
	if (BANNER_F > 0) { BANNER_F = BANNER_F - 1; }
	if (UI_TOAST_F > 0) { UI_TOAST_F = UI_TOAST_F - 1; }
	if (HITSTOP_F > 0) {
		HITSTOP_F = HITSTOP_F - 1;
		return;
	}
	for (int i = 0; i < P_X.size(); ++i) {
		if (P_LIFE[i] <= 0) { continue; }
		P_LIFE[i] = P_LIFE[i] - 1;
		P_X[i] = P_X[i] + P_VX[i];
		P_Y[i] = P_Y[i] + P_VY[i];
		int fl = P_FLAGS[i];
		if ((fl & 2) != 0) { P_VY[i] = P_VY[i] + 0.012; }
		if ((fl & 4) != 0) { P_VX[i] = P_VX[i] * 0.92; P_VY[i] = P_VY[i] * 0.94; }
	}
}

void draw_particles(int camx, int camy) {
	int S = TS;
	for (int i = 0; i < P_X.size(); ++i) {
		if (P_LIFE[i] <= 0) { continue; }
		int sx = math::itrunc(P_X[i] * S) - camx + VW / 2;
		int sy = math::itrunc(P_Y[i] * S) - camy + VH / 2;
		if (sx < 0 || sy < 0 || sx >= VW || sy >= VH) { continue; }
		int c = P_COL[i];
		if ((P_FLAGS[i] & 1) == 0) {
			int lev = light_at(math::itrunc(P_X[i]), math::itrunc(P_Y[i]));
			if (c < NBASE) { c = SHADE[lev * NBASE + c]; }
		}
		// fading: the last third blinks out
		if (P_LIFE[i] * 3 < P_MAX[i] && (P_LIFE[i] + i) % 2 == 0) { continue; }
		PIX[sy * VW + sx] = c;
	}
}

void float_text(float x, float y, string s, int col, int frames, int scale) {
	int slot = -1;
	for (int i = 0; i < FT_X.size(); ++i) {
		if (FT_T[i] <= 0) { slot = i; break; }
	}
	if (slot < 0) {
		slot = FT_X.size();
		FT_X.push(0.0);
		FT_Y.push(0.0);
		FT_S.push("");
		FT_COL.push(0);
		FT_T.push(0);
		FT_SCALE.push(1);
		FT_VY.push(0.0);
	}
	// a fresh number stacks above the ones still rising from the same spot
	float lift = 0.0;
	for (int i = 0; i < FT_X.size(); ++i) {
		if (i == slot || FT_T[i] <= 0) { continue; }
		if (math::abs(FT_X[i] - x) < 1.5 && FT_Y[i] < y - lift + 1.0 && FT_Y[i] > y - lift - 1.25) { lift = lift + 1.25; }
	}
	FT_X[slot] = x + (prandf() - 0.5) * 0.4;
	FT_Y[slot] = y - lift;
	FT_S[slot] = s;
	FT_COL[slot] = col;
	FT_T[slot] = frames;
	FT_SCALE[slot] = scale;
	FT_VY[slot] = -0.13;
}

void draw_float_text(int camx, int camy) {
	int S = TS;
	for (int i = 0; i < FT_X.size(); ++i) {
		if (FT_T[i] <= 0) { continue; }
		int sc = FT_SCALE[i];
		if (sc > 1 && FT_T[i] > 30) { sc = sc + 1; }
		if (FT_T[i] <= 5 && FT_T[i] % 2 == 0) { continue; }
		int w = text_px_width(FT_S[i], sc);
		int sx = math::itrunc(FT_X[i] * S) - camx + VW / 2 + S / 2 - w / 2;
		int sy = math::itrunc(FT_Y[i] * S) - camy + VH / 2 - 4;
		draw_text_px(FT_S[i], sx, sy, FT_COL[i], COL_BLACK, sc);
	}
}

void projectile(float x0, float y0, float x1, float y1, int kind) {
	int slot = -1;
	for (int i = 0; i < PR_X0.size(); ++i) {
		if (PR_T[i] >= PR_DUR[i]) { slot = i; break; }
	}
	if (slot < 0) {
		slot = PR_X0.size();
		PR_X0.push(0.0);
		PR_Y0.push(0.0);
		PR_X1.push(0.0);
		PR_Y1.push(0.0);
		PR_T.push(0);
		PR_DUR.push(1);
		PR_KIND.push(0);
	}
	float d = math::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
	PR_X0[slot] = x0;
	PR_Y0[slot] = y0;
	PR_X1[slot] = x1;
	PR_Y1[slot] = y1;
	PR_T[slot] = 0;
	PR_DUR[slot] = imax(2, math::itrunc(d * 1.3));
	PR_KIND[slot] = kind;
}

// kinds: 0 arrow, 1 magic, 2 fire, 3 frost, 4 lightning, 5 poison, 6 feather, 7 bomb
void draw_projectiles(int camx, int camy) {
	int S = TS;
	for (int i = 0; i < PR_X0.size(); ++i) {
		if (PR_T[i] >= PR_DUR[i]) { continue; }
		PR_T[i] = PR_T[i] + 1;
		float t = PR_T[i] / (PR_DUR[i] * 1.0);
		float x = PR_X0[i] + (PR_X1[i] - PR_X0[i]) * t + 0.5;
		float y = PR_Y0[i] + (PR_Y1[i] - PR_Y0[i]) * t + 0.5;
		int kind = PR_KIND[i];
		if (kind == 7) { y = y - math::sin(t * 3.14159) * 1.5; }
		int sx = math::itrunc(x * S) - camx + VW / 2;
		int sy = math::itrunc(y * S) - camy + VH / 2;
		if (kind == 4) {
			// lightning: a jagged line drawn whole for a few frames
			float px = PR_X0[i] + 0.5;
			float py = PR_Y0[i] + 0.5;
			for (int k = 1; k <= 8; ++k) {
				float qx = PR_X0[i] + 0.5 + (PR_X1[i] - PR_X0[i]) * k / 8.0 + (k < 8 ? (prandf() - 0.5) * 0.6 : 0.0);
				float qy = PR_Y0[i] + 0.5 + (PR_Y1[i] - PR_Y0[i]) * k / 8.0 + (k < 8 ? (prandf() - 0.5) * 0.6 : 0.0);
				draw_line_px(math::itrunc(px * S) - camx + VW / 2, math::itrunc(py * S) - camy + VH / 2,
					math::itrunc(qx * S) - camx + VW / 2, math::itrunc(qy * S) - camy + VH / 2, k % 2 == 0 ? rgb(255, 255, 255) : rgb(150, 230, 255));
				px = qx;
				py = qy;
			}
			if (PR_T[i] == 1) { LIGHTNING_F = imax(LIGHTNING_F, 2); }
			continue;
		}
		int col = rgb(230, 200, 150);
		if (kind == 1) { col = rgb(200, 150, 255); }
		else if (kind == 2) { col = rgb(255, 170, 40); }
		else if (kind == 3) { col = rgb(170, 230, 255); }
		else if (kind == 5) { col = rgb(130, 240, 70); }
		else if (kind == 6) { col = rgb(240, 230, 210); }
		else if (kind == 7) { col = rgb(140, 220, 60); }
		pset(sx, sy, col);
		int tx = math::itrunc((PR_X1[i] - PR_X0[i]) * 2.0);
		int ty = math::itrunc((PR_Y1[i] - PR_Y0[i]) * 2.0);
		pset(sx - isign(tx), sy - isign(ty), kind == 0 ? rgb(130, 90, 50) : col);
		if (kind >= 1 && kind <= 3 || kind == 5) {
			pset(sx + 1, sy, col);
			pset(sx, sy + 1, col);
			if (FRAME % 2 == 0) {
				int fxk = kind == 2 ? Fx.fire : (kind == 3 ? Fx.frost : (kind == 5 ? Fx.poison : Fx.magic));
				burst(fxk, x - 0.5 + 0.5, y - 0.5 + 0.5, 1);
			}
		}
	}
}

void draw_line_px(int x0, int y0, int x1, int y1, int c) {
	int dx = iabs(x1 - x0);
	int dy = iabs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx - dy;
	int guard = 0;
	while (guard < 400) {
		guard = guard + 1;
		pset(x0, y0, c);
		if (x0 == x1 && y0 == y1) { break; }
		int e2 = err * 2;
		if (e2 > -dy) { err = err - dy; x0 = x0 + sx; }
		if (e2 < dx) { err = err + dx; y0 = y0 + sy; }
	}
}

// ---------------------------------------------------------------- weather ---
void draw_weather(ClientView& v) {
	int wx = v.weather;
	if (wx == 1 || wx == 3) {
		int drops = VW * VH / (wx == 3 ? 90 : 160);
		int c = rgb(150, 170, 220);
		int c2 = rgb(96, 112, 160);
		for (int k = 0; k < drops; ++k) {
			int x = prand(VW);
			int y = prand(VH);
			pset(x, y, c);
			pset(x - 1, y - 1, c);
			pset(x - 2, y - 2, c2);
			if (wx == 3) { pset(x - 3, y - 3, c2); }
		}
		int splash = rgb(200, 215, 240);
		for (int k = 0; k < drops / 10; ++k) {
			int x = prand(VW);
			int y = prand(VH);
			pset(x - 1, y, splash);
			pset(x + 1, y, splash);
			pset(x, y - 1, c);
		}
		if (wx == 3 && prand(160) == 0) {
			LIGHTNING_F = 3;
			BOLT_F = 4;
			BOLT_X = VW / 8 + prand(VW * 3 / 4);
			BOLT_SEED = prand(100000);
			sfx("thunder");
		}
	}
	if (wx == 2 || wx == 5) {
		while (WX_X.size() < 70) { WX_X.push(prandf() * VW); WX_Y.push(prandf() * VH); }
		int c = wx == 2 ? rgb(245, 248, 255) : rgb(90, 84, 84);
		int c2 = wx == 2 ? rgb(200, 210, 230) : rgb(200, 80, 40);
		for (int k = 0; k < WX_X.size(); ++k) {
			WX_Y[k] = WX_Y[k] + 0.35 + (k % 3) * 0.15;
			WX_X[k] = WX_X[k] + math::sin((FRAME + k * 13) * 0.05) * 0.3 - (wx == 5 ? 0.2 : 0.0);
			if (WX_Y[k] >= VH) { WX_Y[k] = 0.0; WX_X[k] = prandf() * VW; }
			if (WX_X[k] < 0.0) { WX_X[k] = WX_X[k] + VW; }
			if (WX_X[k] >= VW) { WX_X[k] = WX_X[k] - VW; }
			pset(math::itrunc(WX_X[k]), math::itrunc(WX_Y[k]), k % 7 == 0 ? c2 : c);
		}
	}
	if (wx == 4) {
		// drifting fog: a sparse moving dither of pale pixels
		int c = rgb(170, 176, 184);
		int ph = FRAME / 3;
		for (int y = 0; y < VH; y += 2) {
			for (int x = (y / 2 + ph) % 5; x < VW; x += 5) {
				if (((x / 7 + y / 5 + ph / 4) % 3) != 0) { PIX[y * VW + x] = c; }
			}
		}
	}
}

void draw_banner() {
	if (BANNER_F <= 0 || BANNER == "") { return; }
	int scale = VW >= 150 ? 2 : 1;
	int w = text_px_width(BANNER, scale);
	int x = (VW - w) / 2;
	int y = VH / 3;
	// slide in from the left during the first frames
	int age = 90 - BANNER_F;
	if (age < 10) { x = x - (10 - age) * VW / 20; }
	draw_text_px(BANNER, x, y, BANNER_COL, COL_BLACK, scale);
	if (BANNER_SUB != "") {
		int w2 = text_px_width(BANNER_SUB, 1);
		draw_text_px(BANNER_SUB, (VW - w2) / 2, y + 7 * scale + 2, rgb(230, 230, 230), COL_BLACK, 1);
	}
}

void show_banner(string text, int col, string sub) {
	BANNER = text;
	BANNER_COL = col;
	BANNER_SUB = sub;
	BANNER_F = 90;
}

// --------------------------------------------------------------- events ---
void process_events(ClientView& v) {
	for (int e = 0; e < v.e_kind.size(); ++e) {
		int kind = v.e_kind[e];
		int x = v.e_x[e];
		int y = v.e_y[e];
		int a = v.e_a[e];
		int b = v.e_b[e];
		int c = v.e_c[e];
		fx_event(v, kind, x, y, a, b, c);
		if (kind == Ev.dmg) {
			int who = cv_find(v, a);
			bool crit = (c & 16) != 0;
			int type = c & 15;
			int col = rgb(255, 255, 255);
			if (a == v.my_ser) { col = rgb(255, 70, 60); }
			else if (type == 1) { col = rgb(255, 150, 40); }
			else if (type == 2) { col = rgb(150, 220, 255); }
			else if (type == 3) { col = rgb(130, 240, 70); }
			else if (type == 4) { col = rgb(220, 160, 255); }
			if (crit) { col = rgb(255, 230, 60); }
			float_text(x * 1.0, y * 1.0, crit ? to_string(b) + "!" : to_string(b), col, crit ? 34 : 26, crit && VW >= 120 ? 2 : 1);
			FLASH[a] = FRAME + 3;
			if (crit) { SHAKE_F = imax(SHAKE_F, 5); SHAKE_P = imax(SHAKE_P, 2); }
			if (who >= 0 && a != v.my_ser) { sfx(crit ? "crit" : "hit"); }
		} else if (kind == Ev.heal) {
			float_text(x * 1.0, y * 1.0, "+" + to_string(b), rgb(90, 255, 110), 26, 1);
		} else if (kind == Ev.miss) {
			float_text(x * 1.0, y * 1.0, b == 1 ? "PARRY" : "MISS", b == 1 ? rgb(170, 200, 255) : rgb(150, 150, 160), 14, 1);
		} else if (kind == Ev.death) {
			burst(Fx.smoke, x + 0.5, y + 0.5, 5);
			if (MONS[b].boss) {
				show_banner("VICTORY!", rgb(255, 210, 60), MONS[b].name + " is no more");
				SHAKE_F = 20;
				SHAKE_P = 4;
				burst(Fx.gold, x + 0.5, y + 0.5, 40);
				sfx("victory");
			}
		} else if (kind == Ev.proj) {
			projectile(x * 1.0, y * 1.0, a * 1.0, b * 1.0, c);
			sfx(c == 0 ? "bow" : (c == 2 ? "fire" : (c == 4 ? "thunder" : "zap")));
		} else if (kind == Ev.fx) {
			if (a == Fx.loot) { loot_burst(x + 0.5, y + 0.5, b); }
			else { burst(a, x + 0.5, y + 0.5, imin(40, b)); }
		} else if (kind == Ev.sound) {
			sfx_at(v, v.e_text[e], x, y);
		} else if (kind == Ev.shake) {
			SHAKE_F = imax(SHAKE_F, 4 + a * 2);
			SHAKE_P = imax(SHAKE_P, imin(4, 1 + a / 2));
		} else if (kind == Ev.banner) {
			if (a == -1) { show_banner(v.e_text[e], rgb(230, 40, 40), "walk to a healer or a shrine"); music_play(-2); }
			else if (a == -2) { show_banner(v.e_text[e], rgb(120, 255, 140), ""); music_play(v.world); }
			else if (a == -4) { show_banner(v.e_text[e], rgb(255, 220, 80), ""); }
			else if (a == -5) { show_banner(v.e_text[e], rgb(120, 200, 255), ""); }
			else if (a >= 0 && a < MONS.size()) {
				show_banner(MONS[a].name, rgb(255, 80, 60), "\"" + MONS[a].quip + "\"");
				SHAKE_F = 14;
				SHAKE_P = 3;
				sfx("roar");
			}
		} else if (kind == Ev.gain) {
			burst(Fx.magic, x + 0.5, y + 0.2, 5);
		} else if (kind == Ev.warp) {
			show_banner(WORLDS[a].name, rgb(255, 230, 120), WORLDS[a].banner);
			CAM_SNAP = true;
			music_play(a);
			WX_X = [];
			WX_Y = [];
		} else if (kind == Ev.spawn) {
			// loot pops out of the corpse toward where it lands
			float dx = (x - b) * 1.0;
			float dy = (y - c) * 1.0;
			particle(b + 0.5, c + 0.5, dx * 0.06, dy * 0.06 - 0.12, 14, rgb(255, 230, 150), 1 + 2);
		} else if (kind == Ev.pickup) {
			burst(Fx.gold, x + 0.5, y + 0.5, 6);
		} else if (kind == Ev.chat) {
			float_text(x * 1.0, y - 0.6, "...", rgb(255, 255, 255), 30, 1);
		}
	}
	v.e_kind = [];
	v.e_w = [];
	v.e_x = [];
	v.e_y = [];
	v.e_a = [];
	v.e_b = [];
	v.e_c = [];
	v.e_text = [];
}

// ----------------------------------------------------------------- juice ---
// footsteps: dust on dry ground, splashes in water, for every player that moved since the last frame
void step_particles(ClientView& v) {
	for (int i = 0; i < v.m_ser.size(); ++i) {
		if (v.m_slot[i] < 0 || (v.m_flags[i] & MF_GHOST) != 0) { continue; }
		// the predicted gator kicks up its own dust on the key press (gq_predict.jai)
		if (v.m_ser[i] == v.my_ser && pred_active(v)) { continue; }
		int ser = v.m_ser[i];
		float was_x = VIS_X.get(ser, -1.0);
		float was_y = VIS_Y.get(ser, -1.0);
		float now_x = v.m_x[i] * 1.0;
		float now_y = v.m_y[i] * 1.0;
		VIS_X[ser] = now_x;
		VIS_Y[ser] = now_y;
		if (was_x < 0.0 || (was_x == now_x && was_y == now_y)) { continue; }
		int t = cv_tile(v, math::itrunc(was_x), math::itrunc(was_y));
		int k = T_WALK[t];
		float fx = was_x + 0.5;
		float fy = was_y + 0.9;
		if (k == 2 || t == T_SHALLOW || t == T_SWAMP || t == T_BOG || t == T_MUD) {
			for (int n = 0; n < 4; ++n) { particle(fx + (prandf() - 0.5) * 0.6, fy - 0.2, (prandf() - 0.5) * 0.05, -0.05 - prandf() * 0.05, 10 + prand(8), prand(2) == 0 ? rgb(200, 230, 255) : rgb(110, 170, 220), 2); }
		} else if (t == T_SNOW || t == T_ICE) {
			for (int n = 0; n < 3; ++n) { particle(fx + (prandf() - 0.5) * 0.6, fy, (prandf() - 0.5) * 0.03, -0.02, 14, rgb(240, 245, 255), 4); }
		} else if (t == T_DIRT || t == T_SAND || t == T_ROAD || t == T_ASH || t == T_CAVEFLOOR) {
			int c = t == T_SAND ? rgb(230, 210, 160) : (t == T_ASH ? rgb(120, 110, 110) : rgb(160, 130, 95));
			for (int n = 0; n < 2; ++n) { particle(fx + (prandf() - 0.5) * 0.5, fy, (prandf() - 0.5) * 0.03, -0.015, 12 + prand(6), c, 4); }
		}
	}
}

// what the local player is busy with, animated: a bobbing float, pick sparks, wood chips, anvil sparks
void action_juice(ClientView& v, int camx, int camy) {
	int me = cv_me(v);
	if (me < 0 || v.action == 0) { return; }
	int S = TS;
	float vt = render_tick(v);
	int dir = v.m_dir[me];
	float tx = mob_vx(v, me, vt) + DX[dir];
	float ty = mob_vy(v, me, vt) + DY[dir];
	if (v.action == 1) {
		int sx = math::itrunc((tx + 0.5) * S) - camx + VW / 2;
		int sy = math::itrunc((ty + 0.5) * S) - camy + VH / 2 + ((FRAME / 6) % 2);
		bool bite = v.action_left <= 4 && v.action_left > 0;
		if (bite) { sy = sy + 1; }
		pset(sx, sy - 1, rgb(250, 250, 250));
		pset(sx, sy, rgb(230, 40, 40));
		if (FRAME % 20 == 0 || (bite && FRAME % 4 == 0)) { particle(tx + 0.5, ty + 0.5, 0.0, 0.0, 12, rgb(200, 230, 255), 4); }
		if (bite && FRAME % 3 == 0) { particle(tx + 0.5, ty + 0.5, (prandf() - 0.5) * 0.08, -0.08, 10, rgb(220, 240, 255), 2); }
		// the line, from the gator's snout to the float
		int gx = math::itrunc((mob_vx(v, me, vt) + 0.5 + DX[dir] * 0.4) * S) - camx + VW / 2;
		int gy = math::itrunc((mob_vy(v, me, vt) + 0.3) * S) - camy + VH / 2;
		if (FRAME % 2 == 0) { draw_line_px(gx, gy, sx, sy - 1, rgb(200, 200, 190)); }
	} else if ((v.action == 2 || v.action == 3) && FRAME % 5 == 0) {
		int c = v.action == 2 ? (prand(2) == 0 ? rgb(255, 240, 160) : rgb(200, 200, 210)) : (prand(2) == 0 ? rgb(170, 120, 70) : rgb(120, 170, 70));
		for (int n = 0; n < 3; ++n) { particle(tx + 0.5, ty + 0.5, (prandf() - 0.5) * 0.1, -0.06 - prandf() * 0.05, 8 + prand(6), c, v.action == 2 ? 3 : 2); }
		if (FRAME % 10 == 0) { sfx(v.action == 2 ? "clink" : "chop"); }
	} else if (v.action == 5 && FRAME % 6 == 0) {
		burst(Fx.sparks, mob_vx(v, me, vt) + 0.5, mob_vy(v, me, vt) + 0.4, 3);
		if (FRAME % 12 == 0) { sfx("hammer"); }
	} else if (v.action == 4 && FRAME % 8 == 0) {
		burst(Fx.heal, mob_vx(v, me, vt) + 0.5, mob_vy(v, me, vt) + 0.5, 2);
	}
}

// a boss in view gets a health bar across the top
void boss_bar(ClientView& v) {
	for (int i = 0; i < v.m_ser.size(); ++i) {
		if ((v.m_flags[i] & MF_BOSS) == 0) { continue; }
		string name = MONS[v.m_kind[i]].name;
		int bw = imax(40, VW * 3 / 5);
		int bx = (VW - bw) / 2;
		int by = 9;
		int filled = bw * v.m_hp[i] / 100;
		bool hurt = FLASH.has(v.m_ser[i]) && FLASH[v.m_ser[i]] > FRAME;
		for (int x = -1; x <= bw; ++x) {
			pset(bx + x, by - 1, rgb(30, 10, 10));
			pset(bx + x, by + 3, rgb(30, 10, 10));
		}
		for (int y = 0; y < 3; ++y) {
			pset(bx - 1, by + y, rgb(30, 10, 10));
			pset(bx + bw, by + y, rgb(30, 10, 10));
			for (int x = 0; x < bw; ++x) {
				int c = x < filled ? (hurt ? rgb(255, 255, 255) : (y == 0 ? rgb(255, 110, 90) : rgb(200, 30, 30))) : rgb(70, 20, 20);
				pset(bx + x, by + y, c);
			}
		}
		draw_text_px(string::to_upper(name), (VW - text_px_width(name, 1)) / 2, 2, rgb(255, 220, 120), COL_BLACK, 1);
		return;
	}
}

// low health: the edges of the view pulse red, with a heartbeat
void low_health(ClientView& v) {
	if (v.ghost != 0 || v.hpmax <= 0 || v.hp * 100 / v.hpmax >= 30 || v.hp <= 0) { return; }
	int phase = FRAME % 24;
	int strength = phase < 4 ? 3 : (phase < 8 ? 2 : 1);
	if (phase == 0) { sfx("heartbeat"); }
	int c = rgb(200, 20, 30);
	int c2 = rgb(120, 10, 20);
	for (int d = 0; d < strength * 2; ++d) {
		int col = d < strength ? c : c2;
		for (int x = d; x < VW - d; ++x) {
			if ((x + d + FRAME) % (d + 1) == 0) {
				pset(x, d, col);
				pset(x, VH - 1 - d, col);
			}
		}
		for (int y = d; y < VH - d; ++y) {
			if ((y + d + FRAME) % (d + 1) == 0) {
				pset(d, y, col);
				pset(VW - 1 - d, y, col);
			}
		}
	}
}

// ------------------------------------------------------------- the frame ---
void render_world(ClientView& v) {
	ensure_shade(v);
	int me = cv_me(v);
	float vt = render_tick(v);
	float tx = TITLE_X * 1.0;
	float ty = TITLE_Y * 1.0;
	if (me >= 0) {
		tx = mob_vx(v, me, vt);
		ty = mob_vy(v, me, vt);
	}
	float want_x = tx * TS + TS / 2.0;
	float want_y = ty * TS + TS / 2.0;
	float gap_x = want_x - CAM_X;
	float gap_y = want_y - CAM_Y;
	if (gap_x * gap_x + gap_y * gap_y > (VW * VW * 1.0)) { CAM_SNAP = true; }
	if (CAM_SNAP) {
		CAM_X = want_x;
		CAM_Y = want_y;
		CAM_SNAP = false;
	} else {
		CAM_X = CAM_X + (want_x - CAM_X) * 0.35;
		CAM_Y = CAM_Y + (want_y - CAM_Y) * 0.35;
	}
	int camx = math::itrunc(CAM_X);
	int camy = math::itrunc(CAM_Y);
	if (ARRIVE_F > 0) {
		// a new session: the camera swoops down onto the gator
		float k = ARRIVE_F / 50.0;
		camy = camy - math::itrunc(k * k * VH * 1.2);
	}
	if (SHAKE_F > 0) {
		camx = camx + prand(SHAKE_P * 2 + 1) - SHAKE_P;
		camy = camy + prand(SHAKE_P * 2 + 1) - SHAKE_P;
	}
	int amb = ambient_level(v);
	step_particles(v);
	fx_world_life(v, camx, camy, amb);
	fx_alerts(v);
	fx_barks(v);
	draw_tiles(v, camx, camy, amb);
	draw_items(v, camx, camy);
	draw_mobiles(v, camx, camy);
	draw_critters(camx, camy);
	draw_slashes(camx, camy);
	action_juice(v, camx, camy);
	draw_projectiles(camx, camy);
	draw_particles(camx, camy);
	draw_weather(v);
	draw_season_sky(v);
	draw_bolt();
	draw_alerts(v, camx, camy);
	draw_barks(v, camx, camy);
	draw_float_text(camx, camy);
	boss_bar(v);
	low_health(v);
	draw_hurt();
	draw_reload(v);
	draw_banner();
	if (v.ghost != 0 && FRAME % 60 < 30) { draw_text_px("GHOST", 2, 2, rgb(200, 220, 255), COL_BLACK, 1); }
}
