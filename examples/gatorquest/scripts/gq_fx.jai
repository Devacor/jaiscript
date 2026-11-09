// Gator Quest - the world's life and the feedback on top of it: drifting cloud
// shadows, fireflies, birds that scatter, butterflies, leaping fish, embers,
// townsfolk who chat, monsters that spot you, knockback, slashes, hit-stop,
// monsters that shatter, the hot-reload sweep, the title's logo and the live
// tweak file (seasons). Presentation only: none of it touches the sim or its RNG.

import "gq_render.jai";

// the noise lattice: 64x64 values, tiled, made once
array<float> noise_lattice() {
	array<float> result;
	for (int i = 0; i < 64 * 64; ++i) { result.push((hash3(i % 64, i / 64, 9173) % 1000) / 1000.0); }
	return result;
}

array<float> NOISE_LAT = noise_lattice();

float lattice(int ix, int iy) { return NOISE_LAT[(iy & 63) * 64 + (ix & 63)]; }

// smooth value noise in 0..1
float vnoise(float x, float y) {
	int ix = math::ifloor(x);
	int iy = math::ifloor(y);
	float fx = x - ix;
	float fy = y - iy;
	fx = fx * fx * (3.0 - 2.0 * fx);
	fy = fy * fy * (3.0 - 2.0 * fy);
	float a = lattice(ix, iy);
	float b = lattice(ix + 1, iy);
	float c = lattice(ix, iy + 1);
	float d = lattice(ix + 1, iy + 1);
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

bool outdoor_sky(ClientView& v) { return WORLDS[v.world].ambient == 0; }

void fx_reset() {
	CR_X = [];
	CR_Y = [];
	CR_VX = [];
	CR_VY = [];
	CR_KIND = [];
	CR_T = [];
	CR_COL = [];
	CR_WORLD = -1;
	ALERT_F = {};
	BARK_F = {};
	BARK_SER = -1;
	BARK_TEXT = "";
	BARK_T = 0;
	BARK_NEXT = 0;
	KB_F = {};
	KB_DX = {};
	KB_DY = {};
	SL_AX = [];
	SL_AY = [];
	SL_VX = [];
	SL_VY = [];
	SL_T = [];
	SL_HIT = [];
	HITSTOP_F = 0;
	HURT_F = 0;
	RELOAD_F = 0;
	ARRIVE_F = 0;
	BOLT_F = 0;
	SW_X = [];
	SW_Y = [];
}

// a new palette: whatever holds a color index from the old one lets go of it
void palette_users_reset() {
	for (int i = 0; i < P_LIFE.size(); ++i) { P_LIFE[i] = 0; }
	for (int i = 0; i < FT_T.size(); ++i) { FT_T[i] = 0; }
	for (int i = 0; i < CR_KIND.size(); ++i) { CR_KIND[i] = 0; }
	BANNER_F = 0;
}

// ------------------------------------------------------------ cloud shadows ---
// coverage 0..16 at a tile corner: noise drifting with the wind
int cloud_cover(int tx, int ty, float drift) {
	float n = vnoise(tx / 9.0 + drift, ty / 6.5 + drift * 0.35);
	return iclamp(math::itrunc((n - 0.7) * 200.0), 0, 16);
}

// shadows of clouds sweep over the land in daylight: coverage at every tile corner of the view, for the tiles and sprites
bool fx_cloud_grid(ClientView& v, int tx0, int ty0, int tw, int th, int amb) {
	CLOUD_TX0 = tx0;
	CLOUD_TY0 = ty0;
	CLOUD_CW = tw + 1;
	CLOUD_ON = outdoor_sky(v) && amb >= 10 && v.weather != 1 && v.weather != 3 && SHADE_GHOST == 0;
	if (!CLOUD_ON) { return false; }
	float drift = FRAME * 0.012;
	int n = (tw + 1) * (th + 1);
	while (CLOUD_COV.size() < n) { CLOUD_COV.push(0); }
	int k = 0;
	for (int j = 0; j <= th; ++j) {
		for (int i = 0; i <= tw; ++i) {
			CLOUD_COV[k] = cloud_cover(tx0 + i, ty0 + j, drift);
			k = k + 1;
		}
	}
	return true;
}

// is the tile under a cloud (for sprites, which shade by their tile)
bool cloud_at(int tx, int ty) {
	if (!CLOUD_ON) { return false; }
	int i = tx - CLOUD_TX0;
	int j = ty - CLOUD_TY0;
	if (i < 0 || j < 0 || i >= CLOUD_CW - 1 || (j + 1) * CLOUD_CW + i + 1 >= CLOUD_COV.size()) { return false; }
	int c = j * CLOUD_CW + i;
	return CLOUD_COV[c] + CLOUD_COV[c + 1] + CLOUD_COV[c + CLOUD_CW] + CLOUD_COV[c + CLOUD_CW + 1] >= 32;
}

// night: the edges of the view sink a little deeper into the dark
void fx_vignette(int amb) {
	if (amb > 8) { return; }
	for (int ly = 0; ly < LG_H; ++ly) {
		int ey = imin(ly, LG_H - 1 - ly);
		for (int lx = 0; lx < LG_W; ++lx) {
			int e = imin(imin(lx, LG_W - 1 - lx), ey);
			if (e < 3) {
				int i = ly * LG_W + lx;
				LG[i] = LG[i] * (5 + e) / 8;
			}
		}
	}
}

// --------------------------------------------------------------- critters ---
// kinds: 1 bird (sitting, then flying), 2 butterfly, 3 firefly, 4 leaping fish
int CRK_BIRD = 1;
int CRK_BUTTERFLY = 2;
int CRK_FIREFLY = 3;
int CRK_FISH = 4;

int critter_add(int kind, float x, float y, float vx, float vy, int life, int col) {
	int slot = -1;
	for (int i = 0; i < CR_X.size(); ++i) {
		if (CR_KIND[i] == 0) { slot = i; break; }
	}
	if (slot < 0) {
		slot = CR_X.size();
		CR_X.push(0.0);
		CR_Y.push(0.0);
		CR_VX.push(0.0);
		CR_VY.push(0.0);
		CR_KIND.push(0);
		CR_T.push(0);
		CR_COL.push(0);
	}
	CR_X[slot] = x;
	CR_Y[slot] = y;
	CR_VX[slot] = vx;
	CR_VY[slot] = vy;
	CR_KIND[slot] = kind;
	CR_T[slot] = life;
	CR_COL[slot] = col;
	return slot;
}

int critter_count(int kind) {
	int n = 0;
	for (int i = 0; i < CR_KIND.size(); ++i) {
		if (CR_KIND[i] == kind) { n = n + 1; }
	}
	return n;
}

bool perch_tile(int t) { return t == T_GRASS || t == T_DIRT || t == T_ROAD || t == T_SAND || t == T_FLOWERS || t == T_SNOW; }
bool glow_tile(int t) { return t == T_GRASS || t == T_TALLGRASS || t == T_FLOWERS || t == T_REEDS || t == T_SWAMP || t == T_SHALLOW || t == T_LILY || t == T_BUSH || t == T_BOG || t == T_GRAVE; }

// where the view looks: the local gator, or the title's drifting camera
float focus_x(ClientView& v) {
	int me = cv_me(v);
	return me >= 0 ? mob_vx(v, me, render_tick(v)) : TITLE_X * 1.0;
}

float focus_y(ClientView& v) {
	int me = cv_me(v);
	return me >= 0 ? mob_vy(v, me, render_tick(v)) : TITLE_Y * 1.0;
}

// each frame: critters move, flee, flutter and fall back into the water
void critters_step() {
	for (int i = 0; i < CR_KIND.size(); ++i) {
		int k = CR_KIND[i];
		if (k == 0) { continue; }
		CR_T[i] = CR_T[i] - 1;
		if (CR_T[i] <= 0) { CR_KIND[i] = 0; continue; }
		if (k == CRK_BIRD && CR_VY[i] != 0.0) {
			CR_X[i] = CR_X[i] + CR_VX[i];
			CR_Y[i] = CR_Y[i] + CR_VY[i];
			CR_VY[i] = CR_VY[i] * 1.01;
		} else if (k == CRK_FIREFLY || k == CRK_BUTTERFLY) {
			CR_VX[i] = CR_VX[i] * 0.96 + (prandf() - 0.5) * 0.006;
			CR_VY[i] = CR_VY[i] * 0.96 + (prandf() - 0.5) * 0.006;
			CR_X[i] = CR_X[i] + CR_VX[i];
			CR_Y[i] = CR_Y[i] + CR_VY[i];
		} else if (k == CRK_FISH) {
			CR_X[i] = CR_X[i] + CR_VX[i];
			CR_Y[i] = CR_Y[i] + CR_VY[i];
			CR_VY[i] = CR_VY[i] + 0.014;
			if (CR_VY[i] > 0.15) {
				burst(Fx.splash, CR_X[i], CR_Y[i], 6);
				CR_KIND[i] = 0;
			}
		}
	}
}

// a random tile of the view (plus a margin), as world coordinates
array<int> random_view_tile(int camx, int camy) {
	int S = TS;
	int tx = (camx - VW / 2) / S + prand(VW / S + 1);
	int ty = (camy - VH / 2) / S + prand(VH / S + 1);
	return [tx, ty];
}

// spawn what the place and the hour call for, and let birds take off when someone comes near
void fx_world_life(ClientView& v, int camx, int camy, int amb) {
	if (CR_WORLD != v.world) {
		CR_KIND = [];
		CR_X = [];
		CR_Y = [];
		CR_VX = [];
		CR_VY = [];
		CR_T = [];
		CR_COL = [];
		CR_WORLD = v.world;
	}
	float fx = focus_x(v);
	float fy = focus_y(v);
	bool sky = outdoor_sky(v);
	int w = v.world;
	bool day = sky && amb >= 10 && v.weather != 1 && v.weather != 3;
	bool night = (sky && amb <= 8) || w == 3;
	int S = TS;
	float vx0 = (camx - VW / 2) * 1.0 / S - 1.0;
	float vy0 = (camy - VH / 2) * 1.0 / S - 1.0;
	float vx1 = (camx + VW / 2) * 1.0 / S + 1.0;
	float vy1 = (camy + VH / 2) * 1.0 / S + 1.0;
	// anything that wandered out of view goes away
	for (int i = 0; i < CR_KIND.size(); ++i) {
		if (CR_KIND[i] == 0) { continue; }
		if (CR_X[i] < vx0 - 2.0 || CR_X[i] > vx1 + 2.0 || CR_Y[i] < vy0 - 3.0 || CR_Y[i] > vy1 + 2.0) { CR_KIND[i] = 0; continue; }
		if (CR_KIND[i] == CRK_FIREFLY && !night) { CR_T[i] = imin(CR_T[i], 8); }
		if (CR_KIND[i] == CRK_BUTTERFLY && !day) { CR_T[i] = imin(CR_T[i], 8); }
	}
	// birds sit in the open by day and scatter from anyone who walks up
	if (day || w == 6) {
		for (int tries = 0; tries < 3 && critter_count(CRK_BIRD) < 6; ++tries) {
			array<int> p = random_view_tile(camx, camy);
			int t = cv_tile(v, p[0], p[1]);
			float d = math::abs(p[0] - fx) + math::abs(p[1] - fy);
			if (w == 6) {
				if (d > 6.0) { critter_add(CRK_BIRD, p[0] + 0.5, vy1 + 1.0, (prandf() - 0.5) * 0.04, -0.035 - prandf() * 0.02, 400, rgb(240, 240, 250)); }
			} else if (perch_tile(t) && d > 4.0) {
				critter_add(CRK_BIRD, p[0] + prandf(), p[1] + 0.6, 0.0, 0.0, 1800, prand(3) == 0 ? rgb(70, 60, 54) : rgb(110, 84, 60));
			}
		}
		for (int i = 0; i < CR_KIND.size(); ++i) {
			if (CR_KIND[i] != CRK_BIRD || CR_VY[i] != 0.0) { continue; }
			float dx = CR_X[i] - fx - 0.5;
			float dy = CR_Y[i] - fy - 0.5;
			if (dx * dx + dy * dy < 7.5 || ARRIVE_F > 0) {
				CR_VX[i] = (dx < 0.0 ? -1.0 : 1.0) * (0.05 + prandf() * 0.04);
				CR_VY[i] = -0.05 - prandf() * 0.03;
				CR_T[i] = 160;
			}
		}
	}
	// butterflies over flowers by day
	for (int tries = 0; tries < 3 && day && critter_count(CRK_BUTTERFLY) < 5; ++tries) {
		array<int> p = random_view_tile(camx, camy);
		if (cv_tile(v, p[0], p[1]) == T_FLOWERS) {
			array<int> cols = [rgb(250, 160, 200), rgb(255, 230, 90), rgb(250, 250, 250), rgb(140, 200, 255), rgb(255, 150, 60)];
			critter_add(CRK_BUTTERFLY, p[0] + 0.5, p[1] + 0.3, 0.0, 0.0, 300 + prand(300), cols[prand(cols.size())]);
		}
	}
	// fireflies (wisps in Gloomfen) after dark
	int ff_max = w == 3 ? 10 : LIVE_FIREFLIES;
	for (int tries = 0; tries < 4 && night && critter_count(CRK_FIREFLY) < ff_max; ++tries) {
		array<int> p = random_view_tile(camx, camy);
		if (glow_tile(cv_tile(v, p[0], p[1]))) {
			int col = w == 3 ? rgb(150, 230, 255) : (prand(3) == 0 ? rgb(255, 240, 140) : rgb(200, 255, 110));
			critter_add(CRK_FIREFLY, p[0] + prandf(), p[1] + prandf(), 0.0, 0.0, 200 + prand(300), col);
		}
	}
	// a fish leaps now and then where the water runs deep
	if (amb >= 5 && prand(40) == 0) {
		array<int> p = random_view_tile(camx, camy);
		int t = cv_tile(v, p[0], p[1]);
		if (t == T_DEEP || t == T_SHALLOW || t == T_SWAMP) {
			float x = p[0] + 0.3 + prandf() * 0.4;
			float y = p[1] + 0.6;
			critter_add(CRK_FISH, x, y, (prand(2) == 0 ? -1.0 : 1.0) * 0.035, -0.16, 40, rgb(214, 226, 236));
			burst(Fx.splash, x, y, 4);
		}
	}
	// embers rise from lava
	if (prand(2) == 0) {
		array<int> p = random_view_tile(camx, camy);
		if (cv_tile(v, p[0], p[1]) == T_LAVA) {
			particle(p[0] + prandf(), p[1] + prandf(), (prandf() - 0.5) * 0.02, -0.03 - prandf() * 0.03, 30 + prand(30), prand(3) == 0 ? rgb(255, 230, 120) : rgb(255, 120, 30), 1);
		}
	}
}

// fireflies light the grass around them
void fx_lights() {
	for (int i = 0; i < CR_KIND.size(); ++i) {
		if (CR_KIND[i] == CRK_FIREFLY && firefly_lit(i)) { light_emit(CR_X[i], CR_Y[i], 1.4, 8); }
	}
}

bool firefly_lit(int i) { return math::sin((FRAME + i * 37) * 0.13) > -0.3; }

void draw_critters(int camx, int camy) {
	int S = TS;
	for (int i = 0; i < CR_KIND.size(); ++i) {
		int k = CR_KIND[i];
		if (k == 0) { continue; }
		int sx = math::itrunc(CR_X[i] * S) - camx + VW / 2;
		int sy = math::itrunc(CR_Y[i] * S) - camy + VH / 2;
		if (sx < -3 || sy < -3 || sx >= VW + 3 || sy >= VH + 3) { continue; }
		int c = CR_COL[i];
		if (k == CRK_BIRD) {
			int lev = light_at(math::itrunc(CR_X[i]), math::itrunc(CR_Y[i]));
			int sc = shade_px(lev, c);
			if (CR_VY[i] == 0.0) {
				// perched: body, head and beak, a peck now and then
				bool peck = (FRAME + i * 29) % 70 < 8;
				bool left = (i % 2) == 0;
				int hx = left ? sx - 1 : sx + 1;
				pset(sx, sy, sc);
				pset(hx, peck ? sy : sy - 1, sc);
				pset(left ? hx - 1 : hx + 1, peck ? sy + 1 : sy - 1, shade_px(lev, rgb(240, 160, 50)));
			} else {
				bool up = (FRAME / 3 + i) % 2 == 0;
				pset(sx, sy, sc);
				pset(sx - 1, up ? sy - 1 : sy, sc);
				pset(sx + 1, up ? sy - 1 : sy, sc);
				if (up) {
					pset(sx - 2, sy - 2, sc);
					pset(sx + 2, sy - 2, sc);
				}
			}
		} else if (k == CRK_BUTTERFLY) {
			int bx = sx + math::itrunc(math::sin((FRAME + i * 11) * 0.09) * S * 0.6);
			int by = sy + math::itrunc(math::sin((FRAME + i * 7) * 0.13) * S * 0.4);
			int lev = light_at(math::itrunc(CR_X[i]), math::itrunc(CR_Y[i]));
			int sc = shade_px(lev, c);
			pset(bx, by, shade_px(lev, rgb(40, 30, 30)));
			if ((FRAME / 2 + i) % 3 != 0) {
				pset(bx - 1, by, sc);
				pset(bx + 1, by, sc);
			} else {
				pset(bx, by - 1, sc);
			}
		} else if (k == CRK_FIREFLY) {
			if (!firefly_lit(i)) { continue; }
			pset(sx, sy, c);
			if (math::sin((FRAME + i * 37) * 0.13) > 0.6) {
				int halo = rgb(PAL_R[c] / 3, PAL_G[c] / 3, PAL_B[c] / 4);
				pset(sx - 1, sy, halo);
				pset(sx + 1, sy, halo);
			}
		} else if (k == CRK_FISH) {
			int lev = light_at(math::itrunc(CR_X[i]), math::itrunc(CR_Y[i]));
			int dir = CR_VX[i] < 0.0 ? -1 : 1;
			int tilt = CR_VY[i] < -0.04 ? -1 : (CR_VY[i] > 0.04 ? 1 : 0);
			pset(sx, sy, shade_px(lev, c));
			pset(sx - dir, sy - tilt, shade_px(lev, rgb(150, 170, 190)));
			pset(sx + dir, sy + tilt, shade_px(lev, rgb(250, 250, 255)));
		}
	}
}

// ---------------------------------------------------- the season's sky ---
// autumn leaves, winter snow and spring petals drift over the open-air realms
void draw_season_sky(ClientView& v) {
	if (SEASON == 0 || !outdoor_sky(v)) { return; }
	if (SEASON == 2 && (v.weather == 2 || v.weather == 5)) { return; }
	int want = SEASON == 2 ? 80 : 26;
	while (SW_X.size() < want) {
		SW_X.push(prandf() * VW);
		SW_Y.push(prandf() * VH);
	}
	while (SW_X.size() > want) {
		SW_X.remove(SW_X.size() - 1);
		SW_Y.remove(SW_Y.size() - 1);
	}
	array<int> cols;
	if (SEASON == 1) { cols = [rgb(230, 120, 30), rgb(200, 60, 30), rgb(240, 190, 60), rgb(150, 90, 40)]; }
	else if (SEASON == 2) { cols = [rgb(250, 252, 255), rgb(220, 230, 245), rgb(250, 252, 255)]; }
	else { cols = [rgb(250, 180, 210), rgb(255, 220, 235), rgb(240, 150, 190)]; }
	for (int k = 0; k < SW_X.size(); ++k) {
		float fall = SEASON == 2 ? 0.3 + (k % 3) * 0.12 : 0.22 + (k % 4) * 0.05;
		SW_Y[k] = SW_Y[k] + fall;
		SW_X[k] = SW_X[k] + math::sin((FRAME + k * 17) * 0.06) * (SEASON == 2 ? 0.25 : 0.6) + (SEASON == 2 ? 0.0 : 0.15);
		if (SW_Y[k] >= VH) { SW_Y[k] = 0.0; SW_X[k] = prandf() * VW; }
		if (SW_X[k] < 0.0) { SW_X[k] = SW_X[k] + VW; }
		if (SW_X[k] >= VW) { SW_X[k] = SW_X[k] - VW; }
		int x = math::itrunc(SW_X[k]);
		int y = math::itrunc(SW_Y[k]);
		int c = cols[k % cols.size()];
		pset(x, y, c);
		if (SEASON != 2 && (FRAME / 4 + k) % 3 == 0) { pset(x + 1, y, c); }
	}
}

// ---------------------------------------------------------- townsfolk talk ---
array<string> ROLE_BARKS = [
	"SHARP THINGS!|GET A BLADE!|OR A FRYING PAN?",
	"PLATE IS NICER.|SHINY ARMOR!|NO FROG HELMS.",
	"POTIONS HERE!|NO REFUNDS.|GOT GNARLROOT?",
	"FOOD! TOOLS!|YOU LOOK HUNGRY.|ARROWS?",
	"FRESH-ISH FISH!|GOLDEN CARP?|FISH!",
	"YOUR GOLD IS SAFE.|MOSTLY SAFE.|SAVINGS!",
	"BLESS YOU!|NEED A HEAL?|GHOSTS FIRST.",
	"WELCOME!|FINEST SWAMP!|VOTE BEAUREGARD!",
	"CLAWS TO YOURSELF.|RACCOONS. UGH.|STAY SAFE.",
	"SIX SCALES...|THE GATES HUM.|A HERO?"
];
array<string> FOLK_BARKS = ["NICE DAY!", "HI THERE!", "MIND THE MUD.", "HEARD THAT?", "RACCOONS AGAIN!", "WHAT A SWAMP!", "LOVELY MUD.", "HOWDY!", "OOH A HERO!", "GATOR QUEST!"];
array<string> GUARD_BARKS = ["HALT!", "MOVE ALONG.", "ALL QUIET.", "STAY SAFE.", "NO SPITTING."];
array<string> NIGHT_BARKS = ["ZZZ...", "LATE OUT?", "*YAWN*", "SPOOKY NIGHT."];

string bark_for(ClientView& v, int i) {
	MonDef md = MONS[v.m_kind[i]];
	bool late = outdoor_sky(v) && ambient_level(v) <= 5;
	if (late && prand(3) == 0) { return NIGHT_BARKS[prand(NIGHT_BARKS.size())]; }
	int role = v.m_role.size() > i ? v.m_role[i] : -1;
	if (role >= 0 && role < ROLE_BARKS.size()) {
		array<string> lines = ROLE_BARKS[role].split("|");
		return lines[prand(lines.size())];
	}
	if (md.fac == Fac.guard || md.name.contains("Guard")) { return GUARD_BARKS[prand(GUARD_BARKS.size())]; }
	return FOLK_BARKS[prand(FOLK_BARKS.size())];
}

// now and then the nearest townsfolk says something
void fx_barks(ClientView& v) {
	if (BARK_SER >= 0 && FRAME - BARK_T > 84) { BARK_SER = -1; }
	if (cv_me(v) < 0) { return; }
	if (FRAME < BARK_NEXT || BARK_SER >= 0) { return; }
	float fx = focus_x(v);
	float fy = focus_y(v);
	int best = -1;
	float bestd = 40.0;
	for (int i = 0; i < v.m_ser.size(); ++i) {
		if (v.m_slot[i] >= 0) { continue; }
		MonDef md = MONS[v.m_kind[i]];
		if (md.fac != Fac.npc && md.fac != Fac.guard) { continue; }
		if (FRAME - BARK_F.get(v.m_ser[i], -10000) < 420) { continue; }
		float dx = v.m_x[i] - fx;
		float dy = v.m_y[i] - fy;
		float d = dx * dx + dy * dy;
		if (d < bestd) { bestd = d; best = i; }
	}
	BARK_NEXT = FRAME + 45 + prand(90);
	if (best < 0) { return; }
	BARK_SER = v.m_ser[best];
	BARK_TEXT = bark_for(v, best);
	BARK_T = FRAME;
	BARK_F[BARK_SER] = FRAME;
}

void draw_bubble(string text, int cx, int top) {
	int w = text_px_width(text, 1) - 1;
	int bw = w + 4;
	int bh = 9;
	int x0 = iclamp(cx - bw / 2, 1, VW - bw - 1);
	int y0 = top - bh - 2;
	if (y0 < 1) { y0 = 1; }
	int ink = rgb(30, 28, 40);
	int paper = rgb(250, 248, 238);
	int rim = rgb(90, 86, 100);
	for (int y = 0; y < bh; ++y) {
		for (int x = 0; x < bw; ++x) {
			bool corner = (x == 0 || x == bw - 1) && (y == 0 || y == bh - 1);
			if (corner) { continue; }
			bool edge = x == 0 || y == 0 || x == bw - 1 || y == bh - 1;
			pset(x0 + x, y0 + y, edge ? rim : paper);
		}
	}
	int tail = iclamp(cx, x0 + 2, x0 + bw - 3);
	pset(tail, y0 + bh, rim);
	pset(tail + 1, y0 + bh, paper);
	pset(tail + 1, y0 + bh - 1, paper);
	pset(tail + 2, y0 + bh, rim);
	pset(tail + 1, y0 + bh + 1, rim);
	draw_text_px(text, x0 + 2, y0 + 2, ink, -1, 1);
}

// --------------------------------------------------------- alerts and hits ---
// a hostile that steps toward you gets a "!" over its head
void fx_alerts(ClientView& v) {
	int me = cv_me(v);
	if (me < 0 || v.ghost != 0) { return; }
	int mx = v.m_x[me];
	int my = v.m_y[me];
	for (int i = 0; i < v.m_ser.size(); ++i) {
		if (v.m_slot[i] >= 0) { continue; }
		MonDef md = MONS[v.m_kind[i]];
		if (md.fac != Fac.monster) { continue; }
		int d = cheb(v.m_x[i], v.m_y[i], mx, my);
		if (d > 8 || d <= 1) { continue; }
		if (v.m_x[i] == v.m_px[i] && v.m_y[i] == v.m_py[i]) { continue; }
		if (cheb(v.m_px[i], v.m_py[i], mx, my) <= d) { continue; }
		int ser = v.m_ser[i];
		if (FRAME - ALERT_F.get(ser, -10000) < 600) { continue; }
		ALERT_F[ser] = FRAME;
		sfx("squeak");
	}
}

void slash_add(int ax, int ay, int vx, int vy, bool hit) {
	SL_AX.push(ax);
	SL_AY.push(ay);
	SL_VX.push(vx);
	SL_VY.push(vy);
	SL_T.push(FRAME);
	SL_HIT.push(hit ? 1 : 0);
	while (SL_T.size() > 8) {
		SL_AX.remove(0);
		SL_AY.remove(0);
		SL_VX.remove(0);
		SL_VY.remove(0);
		SL_T.remove(0);
		SL_HIT.remove(0);
	}
}

void hitstop(ClientView& v, int frames) {
	if (HITSTOP_F <= 0) { HITSTOP_VT = render_tick(v); }
	HITSTOP_F = imax(HITSTOP_F, frames);
}

// a monster bursts into the pixels it was drawn with
void shatter(int kind, int x, int y, bool boss) {
	MonDef md = MONS[kind];
	int off = sprite_compiled(sprite_of(md.spr), md.col, -1, -1, -1);
	int S = TS;
	float size = boss ? 2.0 : 1.0;
	float ox = boss ? x - 0.5 : x * 1.0;
	float oy = boss ? y - 1.0 : y * 1.0;
	for (int py = 0; py < S; ++py) {
		for (int px = 0; px < S; ++px) {
			int c = SPRPIX[off + py * S + px];
			if (c < 0) { continue; }
			float rx = (px + 0.5) / S - 0.5;
			float ry = (py + 0.5) / S - 0.5;
			int copies = boss ? 2 : 1;
			for (int k = 0; k < copies; ++k) {
				particle(ox + (px + 0.5) * size / S, oy + (py + 0.5) * size / S, rx * 0.16 + (prandf() - 0.5) * 0.05, ry * 0.08 - 0.06 - prandf() * 0.07, 16 + prand(18), c, 2);
			}
		}
	}
	// the spirit leaves
	particle(x + 0.5, y + 0.3, 0.0, -0.03, 34, rgb(230, 240, 255), 1);
	particle(x + 0.4, y + 0.4, 0.0, -0.025, 30, rgb(170, 200, 255), 1);
}

// one event of the frame, for the feedback the old handlers do not give
void fx_event(ClientView& v, int kind, int x, int y, int a, int b, int c) {
	int me = cv_me(v);
	int mx = me >= 0 ? v.m_x[me] : -100;
	int my = me >= 0 ? v.m_y[me] : -100;
	if (kind == Ev.dmg) {
		bool crit = (c & 16) != 0;
		if (a == v.my_ser) {
			HURT_F = imax(HURT_F, crit ? 12 : 8);
			SHAKE_F = imax(SHAKE_F, crit ? 8 : 5);
			SHAKE_P = imax(SHAKE_P, crit ? 3 : 1);
			return;
		}
		int dx = isign(x - mx);
		int dy = isign(y - my);
		if (me >= 0 && cheb(x, y, mx, my) <= 1) {
			slash_add(mx, my, x, y, true);
			if (crit) { hitstop(v, 3); }
		}
		if (me >= 0 && cheb(x, y, mx, my) <= 10) {
			KB_F[a] = FRAME + 5;
			KB_DX[a] = dx;
			KB_DY[a] = dy;
		}
		if (crit) { burst(Fx.sparks, x + 0.5, y + 0.4, 8); }
		else { burst(Fx.blood, x + 0.5, y + 0.5, 3); }
	} else if (kind == Ev.miss) {
		if (a != v.my_ser && me >= 0 && cheb(x, y, mx, my) <= 1) { slash_add(mx, my, x, y, false); }
	} else if (kind == Ev.death) {
		bool boss = MONS[b].boss;
		shatter(b, x, y, boss);
		if (me >= 0 && cheb(x, y, mx, my) <= 2) { hitstop(v, boss ? 8 : 4); }
		SHAKE_F = imax(SHAKE_F, 4);
		SHAKE_P = imax(SHAKE_P, 1);
	}
}

void draw_slashes(int camx, int camy) {
	int S = TS;
	for (int s = 0; s < SL_T.size(); ++s) {
		int age = FRAME - SL_T[s];
		if (age < 0 || age > 4) { continue; }
		float ax = SL_AX[s] + 0.5;
		float ay = SL_AY[s] + 0.5;
		float bx = SL_VX[s] + 0.5;
		float by = SL_VY[s] + 0.5;
		float dx = bx - ax;
		float dy = by - ay;
		float len = math::sqrt(dx * dx + dy * dy);
		if (len < 0.01) { continue; }
		dx = dx / len;
		dy = dy / len;
		float px = -dy;
		float py = dx;
		int head = rgb(255, 255, 255);
		int trail = SL_HIT[s] == 1 ? rgb(255, 230, 150) : rgb(170, 180, 200);
		for (int k = -4; k <= 4; ++k) {
			int lead = -4 + age * 3;
			if (k > lead || k < lead - 4) { continue; }
			float t = k / 4.0;
			float bend = (1.0 - t * t) * 0.3;
			float wx = ax + dx * (0.45 + bend) + px * t * 0.55;
			float wy = ay + dy * (0.45 + bend) + py * t * 0.55;
			pset(math::itrunc(wx * S) - camx + VW / 2, math::itrunc(wy * S) - camy + VH / 2, k >= lead - 1 ? head : trail);
		}
	}
}

// "!" over every monster that just spotted you
void draw_alerts(ClientView& v, int camx, int camy) {
	int S = TS;
	float vt = render_tick(v);
	for (int i = 0; i < v.m_ser.size(); ++i) {
		int age = FRAME - ALERT_F.get(v.m_ser[i], -10000);
		if (age < 0 || age > 34) { continue; }
		int sx = math::itrunc(mob_vx(v, i, vt) * S) - camx + VW / 2 + S / 2 - 1;
		int sy = math::itrunc(mob_vy(v, i, vt) * S) - camy + VH / 2 - 7;
		int hop = age < 6 ? 6 - age : (age < 10 ? (age - 6) / 2 : 0);
		draw_text_px("!", sx, sy - hop, (FRAME / 3) % 2 == 0 ? rgb(255, 70, 50) : rgb(255, 220, 70), COL_BLACK, 1);
	}
}

// the townsfolk's line, over whoever said it
void draw_barks(ClientView& v, int camx, int camy) {
	if (BARK_SER < 0) { return; }
	int i = cv_find(v, BARK_SER);
	if (i < 0) { BARK_SER = -1; return; }
	int S = TS;
	float vt = render_tick(v);
	int sx = math::itrunc(mob_vx(v, i, vt) * S) - camx + VW / 2 + S / 2;
	int sy = math::itrunc(mob_vy(v, i, vt) * S) - camy + VH / 2;
	if (sx < 0 || sx >= VW || sy < 0 || sy >= VH) { return; }
	int age = FRAME - BARK_T;
	string text = age < 12 ? BARK_TEXT.substr(0, imin(BARK_TEXT.size(), 1 + age * 2)) : BARK_TEXT;
	draw_bubble(text, sx, sy);
}

// hurt: the screen's rim flashes red, strongest on the hit
void draw_hurt() {
	if (HURT_F <= 0) { return; }
	int depth = imin(4, 1 + HURT_F / 3);
	int c = rgb(230, 30, 30);
	int c2 = rgb(130, 10, 16);
	for (int d = 0; d < depth; ++d) {
		int col = d < depth / 2 ? c : c2;
		for (int x = 0; x < VW; ++x) {
			if ((x + d) % (d + 1) != 0) { continue; }
			pset(x, d, col);
			pset(x, VH - 1 - d, col);
		}
		for (int y = 0; y < VH; ++y) {
			if ((y + d) % (d + 1) != 0) { continue; }
			pset(d, y, col);
			pset(VW - 1 - d, y, col);
		}
	}
}

// the storm's bolt: the same jagged path on every frame it lasts
void draw_bolt() {
	if (BOLT_F <= 0) { return; }
	int seed = BOLT_SEED;
	int x = BOLT_X;
	int y = 0;
	int ground = VH * 2 / 3 + BOLT_SEED % (VH / 4 + 1);
	int core = rgb(255, 255, 255);
	int glow = rgb(170, 210, 255);
	while (y < ground) {
		seed = (seed * 1103515245 + 12345) & 2147483647;
		int nx = x + ((seed >> 8) % 7) - 3;
		int ny = y + 3 + ((seed >> 12) % 4);
		draw_line_px(x + 1, y, nx + 1, ny, glow);
		draw_line_px(x, y, nx, ny, core);
		if (((seed >> 16) % 5) == 0) {
			int bx = nx + (((seed >> 4) % 2) == 0 ? -6 : 6);
			draw_line_px(nx, ny, bx, ny + 6, glow);
		}
		x = nx;
		y = ny;
	}
	for (int k = -2; k <= 2; ++k) { pset(x + k, y, glow); }
}

// ------------------------------------------------------------- hot reload ---
// what a reload carried over, in a line
string reload_kept_line(ClientView& v) {
	int me = cv_me(v);
	string where = me >= 0 ? `, YOU AT ${v.m_x[me]} ${v.m_y[me]}` : "";
	return `WORLD KEPT: TICK ${v.tick}|${v.m_ser.size()} IN VIEW${where}`;
}

void fx_reload_notice(ClientView& v, array<string> msgs) {
	if (msgs.size() == 0) { return; }
	bool ok = true;
	array<string> names;
	for (int i = 0; i < msgs.size(); ++i) {
		if (msgs[i].contains("FAILED")) { ok = false; }
		string n = msgs[i];
		n.replace("Reloaded ", "");
		names.push(n);
	}
	RELOAD_F = 90;
	RELOAD_OK = ok;
	RELOAD_WHAT = ok ? names.join(" ") : msgs[0];
	RELOAD_KEPT = reload_kept_line(v);
	sfx(ok ? "warp" : "fizzle");
}

// the sweep down the view, then a card naming the files and what the world kept
void draw_reload(ClientView& v) {
	if (RELOAD_F <= 0) { return; }
	int age = 90 - RELOAD_F;
	int line = rgb(120, 255, 240);
	int dim = rgb(40, 120, 130);
	if (!RELOAD_OK) {
		line = rgb(255, 90, 80);
		dim = rgb(130, 30, 30);
	}
	if (age < 14) {
		int y = age * VH / 13;
		for (int x = 0; x < VW; ++x) {
			pset(x, y, line);
			if ((x + age) % 2 == 0) { pset(x, y - 1, dim); }
			if ((x + age) % 4 == 0) { pset(x, y - 3, dim); }
		}
	}
	int fit = (VW - 10) / 4;
	string head = (RELOAD_OK ? "HOT RELOAD: " : "RELOAD FAILED: ") + string::to_upper(RELOAD_WHAT);
	array<string> kept = (RELOAD_OK ? RELOAD_KEPT : "THE RUNNING CODE STAYS|UNTIL THE FILE IS FIXED").split("|");
	if (head.size() > fit) { head = head.substr(0, fit); }
	int w = text_px_width(head, 1);
	for (int i = 0; i < kept.size(); ++i) {
		if (kept[i].size() > fit) { kept[i] = kept[i].substr(0, fit); }
		w = imax(w, text_px_width(kept[i], 1));
	}
	w = w + 5;
	int h = 10 + kept.size() * 7;
	int x0 = 2;
	int y0 = VH - h - 3;
	if (age < 6) { x0 = x0 - (6 - age) * (w + 2) / 6; }
	int ground = rgb(10, 22, 26);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			bool edge = x == 0 || y == 0 || x == w - 1 || y == h - 1;
			pset(x0 + x, y0 + y, edge ? line : ground);
		}
	}
	bool lit = age < 30 && (age / 4) % 2 == 0;
	draw_text_px(head, x0 + 3, y0 + 3, lit ? rgb(255, 255, 255) : line, -1, 1);
	for (int i = 0; i < kept.size(); ++i) { draw_text_px(kept[i], x0 + 3, y0 + 10 + i * 7, RELOAD_OK ? rgb(150, 230, 160) : rgb(255, 170, 160), -1, 1); }
}

// a new session opens on the camera dropping out of the sky onto the gator, the realm's name and a scatter of birds
void fx_arrival(int w) {
	ARRIVE_F = 50;
	CAM_SNAP = true;
	show_banner(WORLDS[w].name, rgb(255, 230, 120), WORLDS[w].banner);
}

// ------------------------------------------------- the live tweak file ---
// saves/live/gq_live.jai: plain calls the game runs again whenever the file changes (hot reload)
array<string> SEASON_NAMES = ["summer", "autumn", "winter", "spring"];

string live_text(string season, int torch, int fireflies) {
	return "// Gator Quest live tweaks. Save this file while the game runs: it re-runs on the next frame\n"
		+ "// (JaiScript hot reload) and the world, your gator and every monster carry on.\n"
		+ "// season: summer, autumn, winter or spring. torch: your light's radius in tiles. fireflies: how many at night.\n"
		+ `live_set("season", "${season}");\n`
		+ `live_set("torch", "${torch}");\n`
		+ `live_set("fireflies", "${fireflies}");\n`;
}

void live_set(string key, string value) {
	if (key == "season") {
		int s = SEASON_NAMES.index_of(string::to_lower(string::trim(value)));
		if (s >= 0) { LIVE_SEASON = s; }
	} else if (key == "torch") {
		LIVE_TORCH = iclamp(value.to_int(5), 1, 12);
	} else if (key == "fireflies") {
		LIVE_FIREFLIES = iclamp(value.to_int(16), 0, 60);
	}
}

// a season change rebakes the tile textures
void live_apply() {
	if (LIVE_SEASON == SEASON) { return; }
	SEASON = LIVE_SEASON;
	SW_X = [];
	SW_Y = [];
	TS = 0;
	SHADE_WORLD = -1;
	if (VW > 0) { layout(SCREEN_COLS, SCREEN_ROWS); }
}

// the file the menu edits and anyone can open, in `dir`: made with the defaults when missing
void live_start(string dir) {
	LIVE_PATH = dir + "/gq_live.jai";
	LIVE_SEASON = SEASON;
	if (!file_exists(LIVE_PATH)) {
		make_dirs(dir);
		write_file(LIVE_PATH, live_text(SEASON_NAMES[SEASON], LIVE_TORCH, LIVE_FIREFLIES));
	}
	// the file's tweaks hold from the start; a broken file waits for its fix like any other reload
	try {
		import(LIVE_PATH);
	} catch (e) {
		toast("gq_live.jai: " + to_string(e).lines()[0]);
	}
	live_apply();
}

// the menu's "next season": one line of the file changes, the reload does the rest
void live_next_season() {
	if (LIVE_PATH == "") { return; }
	string text = read_file(LIVE_PATH);
	string next = SEASON_NAMES[(SEASON + 1) % 4];
	string from = `live_set("season", "${SEASON_NAMES[SEASON]}");`;
	if (text.contains(from)) { text.replace(from, `live_set("season", "${next}");`); }
	else { text = live_text(next, LIVE_TORCH, LIVE_FIREFLIES); }
	write_file(LIVE_PATH, text);
}

// ------------------------------------------------------------------ title ---
array<int> LOGO_GRAD_RGB = [236, 255, 150, 180, 245, 96, 128, 222, 74, 86, 188, 58, 56, 150, 46, 38, 112, 40];

// the logo's letters sit closer than the text font's, so it stays big on narrow views
int logo_advance(int scale) { return 3 * scale + 1; }
int logo_width(string text, int scale) { return text.size() * logo_advance(scale) - 1; }

// the logo: letters drop in and bounce, a gradient fill, a dark rim, a drop shadow and a passing glint
void draw_logo(string text, int x, int y, int scale, int t) {
	string up = string::to_upper(text);
	int h = 5 * scale;
	array<int> grad;
	for (int k = 0; k < 6; ++k) { grad.push(rgb(LOGO_GRAD_RGB[k * 3], LOGO_GRAD_RGB[k * 3 + 1], LOGO_GRAD_RGB[k * 3 + 2])); }
	int rim = rgb(14, 34, 12);
	int shadow = rgb(4, 10, 6);
	int glint = rgb(255, 255, 228);
	int adv = logo_advance(scale);
	int w = up.size() * adv;
	int shine = (t * 3) % (w + 160) - 30;
	for (int pass = 0; pass < 3; ++pass) {
		for (int i = 0; i < up.size(); ++i) {
			string ch = up.substr(i, 1);
			if (!FONT_INDEX.has(ch)) { continue; }
			int k = t - i * 3;
			if (k < 0) { continue; }
			int dy = 0;
			if (k < 9) { dy = -(9 - k) * (9 - k) * 2 / 3; }
			else if (k < 13) { dy = -(2 - iabs(k - 11)); }
			else { dy = math::itrunc(math::sin((t + i * 5) * 0.08) * 1.5); }
			string bits = FONT_BITS[FONT_INDEX[ch]];
			int ox = x + i * adv;
			for (int r = 0; r < 5; ++r) {
				for (int c = 0; c < 3; ++c) {
					if (bits[r * 3 + c] != '1') { continue; }
					for (int sy = 0; sy < scale; ++sy) {
						for (int sx = 0; sx < scale; ++sx) {
							int px = ox + c * scale + sx;
							int py = y + dy + r * scale + sy;
							if (pass == 0) {
								for (int d = 1; d <= 2; ++d) { pset(px + d, py + d + 1, shadow); }
							} else if (pass == 1) {
								pset(px - 1, py, rim);
								pset(px + 1, py, rim);
								pset(px, py - 1, rim);
								pset(px, py + 1, rim);
								pset(px - 1, py - 1, rim);
								pset(px + 1, py + 1, rim);
							} else {
								int row = r * scale + sy;
								int col = grad[row * 6 / h];
								int diag = px - x + (py - y);
								if (diag >= shine && diag < shine + 4) { col = glint; }
								pset(px, py, col);
							}
						}
					}
				}
			}
		}
	}
}

// the hero in the title, big: the 8x8 design at `scale`, breathing and blinking
void draw_title_gator(int hue, int cx, int top, int scale) {
	string rows = SPR_ROWS[sprite_of("gator")];
	string tint = "hue" + to_string(hue);
	bool blink = FRAME % 110 < 5;
	int breath = (FRAME / 14) % 2;
	int shadow = rgb(10, 16, 10);
	for (int x = -3 * scale; x < 3 * scale; ++x) { pset(cx + x, top + 8 * scale, shadow); }
	int rim = rgb(8, 14, 8);
	for (int v = 0; v < 8; ++v) {
		for (int u = 0; u < 8; ++u) {
			if (rows[v * 8 + u] == '.') { continue; }
			int lift = v < 4 ? breath : 0;
			fill_px(cx - 4 * scale + u * scale - 1, top + v * scale - lift - 1, scale + 2, scale + 2, rim);
		}
	}
	for (int v = 0; v < 8; ++v) {
		for (int u = 0; u < 8; ++u) {
			int ch = rows[v * 8 + u] + 0;
			if (blink && ch == 'k') { ch = 'b' + 0; }
			int c = letter_color(ch, tint, crgb("leather"), crgb("straw"), crgb("steel"));
			if (c < 0) { continue; }
			int lift = v < 4 ? breath : 0;
			for (int sy = 0; sy < scale; ++sy) {
				for (int sx = 0; sx < scale; ++sx) { pset(cx - 4 * scale + u * scale + sx, top + v * scale + sy - lift, c); }
			}
		}
	}
}

// the title's sky turns: a whole day in two minutes, from late afternoon into golden hour, night and dawn
void title_time_lapse(ClientView& v, int t) {
	v.tod = (DAY_TICKS * 11 / 24 + t * DAY_TICKS / 3600) % DAY_TICKS;
	v.weather = 0;
}
