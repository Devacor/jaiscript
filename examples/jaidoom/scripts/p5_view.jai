// P5 gate: monsters exist, render as sprites, take damage, die; hitscan works.
// Writes p5_view.bmp facing the nearest monster.

import "wad.jai";
import "map.jai";
import "vid.jai";
import "render.jai";
import "things.jai";
import "game.jai";
import "spec.jai";

var FAILS = 0;
function check(bool ok, string what) -> void {
	if (ok) { print(`  ok    ${what}`); }
	else { FAILS = FAILS + 1; print(`  FAIL  ${what}`); }
}

// probe positions around monster m until one is standable with clear sight;
// puts the player there facing the monster
function place_player_facing(int m) -> bool {
	for (int a = 0; a < 8; ++a) {
		float ang = a * math::PI / 4.0;
		for (int di = 0; di < 3; ++di) {
			float d = 140.0 + di * 70.0;
			float px = MO_X[m] + math::cos(ang) * d;
			float py = MO_Y[m] + math::sin(ang) * d;
			P_FLOORZ = SE_FLOORH[sector_at(px, py)];
			if (!pos_ok(px, py)) { continue; }
			if (!sight_clear(px, py, MO_X[m], MO_Y[m], P_FLOORZ + 41.0)) { continue; }
			P_X = px;
			P_Y = py;
			P_ANG = math::atan2(MO_Y[m] - py, MO_X[m] - px);
			return true;
		}
	}
	return false;
}

// convex subsector centroid = guaranteed interior point (p6's trick)
var PT_X = 0.0;
var PT_Y = 0.0;
function find_interior(int sec) -> bool {
	int nss = SS_COUNT.size();
	for (int ss = 0; ss < nss; ++ss) {
		int first = SS_FIRST[ss];
		if (SEG_FSEC[first] != sec) { continue; }
		float sx = 0.0;
		float sy = 0.0;
		int cnt = SS_COUNT[ss];
		for (int k = 0; k < cnt; ++k) {
			sx = sx + VX_X[SG_V1[first + k]];
			sy = sy + VX_Y[SG_V1[first + k]];
		}
		float cx = sx / cnt;
		float cy = sy / cnt;
		if (sector_at(cx, cy) == sec) { PT_X = cx; PT_Y = cy; return true; }
	}
	return false;
}

function corridor_stand(float x, float y) -> void {
	P_X = x;
	P_Y = y;
	P_FLOORZ = SE_FLOORH[sector_at(x, y)];
}

// every live thing becomes a corpse so later staging has clean lanes
function sweep_corridor() -> void {
	for (int cm = 0; cm < MO_X.size(); ++cm) {
		int cst = MO_STATE[cm];
		if (cst == 1 || cst == 2 || cst == 3 || cst == 4 || cst == 9) {
			MO_STATE[cm] = 6;
			MO_BRAIN[cm] = null;
			MO_CHARGE[cm] = 0;
		}
	}
}

function main() -> int {
	var path = wad_path("doom1");
	if (!wad_load(path)) { print("wad_load failed"); return 1; }
	vid_init(320, 200);
	if (!game_start_map("E1M1")) { print("no E1M1"); return 1; }

	check(KILL_TOTAL >= 4, `monsters spawned (${KILL_TOTAL} on skill 2, ${MO_X.size()} things total)`);

	// find a monster with a legal, sight-clear firing position near it
	int target = -1;
	for (int m = 0; m < MO_X.size() && target < 0; ++m) {
		if (MO_STATE[m] != 1) { continue; }
		if (place_player_facing(m)) { target = m; }
	}
	check(target >= 0, "found a monster with a clear firing position");
	if (target < 0) { return 1; }
	print(`  target: type ${MO_TYPE[target]} (${MO_SPR[target]}) at (${MO_X[target]:.0}, ${MO_Y[target]:.0}), player at (${P_X:.0}, ${P_Y:.0})`);

	render_view(P_X, P_Y, P_ANG);
	draw_weapon("PISG", "A");
	check(SPRITES_DRAWN >= 1, `sprites drawn (${SPRITES_DRAWN})`);
	bmp_write("p5_view.bmp");
	print("  wrote p5_view.bmp");

	// combat: pistol shot must hit the monster ahead
	int before = MO_HP[target];
	int hit = shoot_hitscan(P_X, P_Y, P_FLOORZ + 41.0, P_ANG, 10, -1, 2048.0);
	check(hit == target, `hitscan hit the monster ahead (hit=${hit})`);
	check(MO_HP[target] == before - 10, "damage applied");
	check(MO_STATE[target] == 2 || MO_STATE[target] == 4, "monster woke (chase or pain)");

	// kill it: death anim then corpse, kill counted
	mobj_damage(target, 1000, -1);
	check(MO_STATE[target] == 5, "death animation started");
	for (int i = 0; i < 60; ++i) { mobj_anim_tic(target); }
	check(MO_STATE[target] == 6, "became a corpse");
	check(KILLS == 1, `kill counted (${KILLS}/${KILL_TOTAL})`);

	// projectile: fireball flies then explodes on a wall eventually
	int fb = fireball_spawn(P_X, P_Y, P_FLOORZ + 32.0, P_ANG, -1, P_FLOORZ + 32.0, 2048.0);
	float fx0 = MO_X[fb];
	float fy0 = MO_Y[fb];
	for (int t = 0; t < 4; ++t) { projectile_tic(fb); }
	check(MO_X[fb] != fx0 || MO_Y[fb] != fy0 || MO_STATE[fb] == 5, "fireball moved (or exploded)");
	for (int t2 = 0; t2 < 300 && MO_STATE[fb] == 8; ++t2) { projectile_tic(fb); }
	check(MO_STATE[fb] != 8, "fireball exploded on world geometry");

	// AI: a fresh monster near the player starts chasing and closes distance
	int chaser = -1;
	for (int m2 = 0; m2 < MO_X.size(); ++m2) {
		if (MO_STATE[m2] == 1) { chaser = m2; break; }
	}
	while (chaser >= 0 && !place_player_facing(chaser)) {
		int next = -1;
		for (int m3 = chaser + 1; m3 < MO_X.size(); ++m3) {
			if (MO_STATE[m3] == 1) { next = m3; break; }
		}
		chaser = next;
	}
	if (chaser >= 0) {
		P_HP = 100;
		float dx0 = P_X - MO_X[chaser];
		float dy0 = P_Y - MO_Y[chaser];
		float d0 = math::sqrt(dx0 * dx0 + dy0 * dy0);
		for (int t3 = 0; t3 < 40; ++t3) { monster_tic(chaser); mobj_anim_tic(chaser); }
		bool woke = MO_STATE[chaser] != 1;
		float dx1 = P_X - MO_X[chaser];
		float dy1 = P_Y - MO_Y[chaser];
		float d1 = math::sqrt(dx1 * dx1 + dy1 * dy1);
		check(woke, "second monster woke on sight");
		if (woke) { check(d1 < d0, `chaser closed distance (${d0:.0} -> ${d1:.0})`); }
	}

	// ------------------------------------------- track 2: combat completeness ---
	// staged along the spawn corridor (x=1056, y -3616..-2896, proven walkable
	// by the p4 selftest walk)

	// monster drops: corpses leave ammo/weapon pickups
	int spos = mobj_spawn(1056.0, -3050.0, 9, 0.0, 1, "SPOS", "ABCD", 30, 20.0);
	int nb4 = MO_X.size();
	mobj_damage(spos, 1000, -1);
	int drop = -1;
	for (int d = nb4; d < MO_X.size(); ++d) {
		if (MO_STATE[d] == 7 && MO_SPR[d] == "SHOT") { drop = d; }
	}
	check(drop >= 0, "dead SPOS left a shotgun pickup");
	check(drop >= 0 && MO_X[drop] == MO_X[spos] && MO_Y[drop] == MO_Y[spos], "pickup sits at the corpse");
	int poss = mobj_spawn(1056.0, -3100.0, 3004, 0.0, 1, "POSS", "ABCD", 20, 20.0);
	int nb5 = MO_X.size();
	mobj_damage(poss, 1000, -1);
	bool clipdrop = false;
	for (int d2 = nb5; d2 < MO_X.size(); ++d2) {
		if (MO_STATE[d2] == 7 && MO_SPR[d2] == "CLIP") { clipdrop = true; }
	}
	check(clipdrop, "dead POSS left a clip");

	// infighting: friendly fire retargets, then they trade shots
	int ia = mobj_spawn(1056.0, -3450.0, 3004, 0.0, 1, "POSS", "ABCD", 200, 20.0);
	int ib = mobj_spawn(1056.0, -3300.0, 3004, 0.0, 1, "POSS", "ABCD", 200, 20.0);
	P_X = 1056.0;
	P_Y = -3616.0;
	P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
	P_HP = 100;
	mobj_damage(ia, 5, ib);
	check(MO_TARGET[ia] == ib, "friendly fire retargeted the victim");
	for (int it = 0; it < 300; ++it) {
		monster_tic(ia); mobj_anim_tic(ia);
		monster_tic(ib); mobj_anim_tic(ib);
	}
	check(MO_HP[ib] < 200, `infight: victim shot its attacker (${MO_HP[ib]})`);
	check(MO_TARGET[ib] == ia, "return fire retargeted the attacker");
	check(MO_HP[ia] < 195, `infight: they exchanged attacks (${MO_HP[ia]})`);

	// z-aware shots: flat-aim bullets need vertical overlap
	check(shot_z_hit(41.0, 0.0, 56.0) && !shot_z_hit(100.0, 0.0, 56.0) && !shot_z_hit(-8.0, 0.0, 56.0), "z overlap predicate");
	float aang = math::atan2(MO_Y[ia] - P_Y, MO_X[ia] - P_X);
	int zm = shoot_hitscan(P_X, P_Y, MO_Z[ia] + 500.0, aang, 1, -1, 2048.0);
	check(zm != ia, `shot far above the monster missed (${zm})`);
	int zh = shoot_hitscan(P_X, P_Y, MO_Z[ia] + 32.0, aang, 1, -1, 2048.0);
	check(zh == ia, `same shot at body height hits (${zh})`);

	// barrel chain: one blast sets off the neighbor, both burn out
	int b1 = mobj_spawn(1000.0, -3500.0, 2035, 0.0, 9, "BAR1", "AB", 20, 10.0);
	int b2 = mobj_spawn(1040.0, -3500.0, 2035, 0.0, 9, "BAR1", "AB", 20, 10.0);
	mobj_damage(b1, 100, -1);
	check(MO_STATE[b1] == 5 && MO_SPR[b1] == "BEXP", "barrel exploded");
	check(MO_STATE[b2] == 5 && MO_SPR[b2] == "BEXP", "chain: blast set off the second barrel");
	for (int bt = 0; bt < 80; ++bt) { mobj_anim_tic(b1); mobj_anim_tic(b2); }
	check(MO_STATE[b1] == 0 && MO_STATE[b2] == 0, "exploded barrels vanish");

	// fist: melee range, zero ammo, and the PUNG overlay sprite resolves
	check(sprite_lookup("PUNG", "A", 8) >= 0, "PUNG fist sprite present");
	P_X = 1056.0;
	P_Y = -3590.0;
	P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
	P_HP = 100;
	int fd = mobj_spawn(1056.0, -3550.0, 3004, 0.0, 2, "POSS", "ABCD", 20, 20.0);
	P_ANG = math::atan2(MO_Y[fd] - P_Y, MO_X[fd] - P_X);
	CUR_WEAPON = 1;
	W_COOL = 0;
	AMMO_BULLETS = 0;
	player_fire();
	check(MO_HP[fd] < 20, `fist landed with zero ammo (${MO_HP[fd]})`);
	while (MO_STATE[fd] == 2 || MO_STATE[fd] == 4) { W_COOL = 0; player_fire(); }
	check(MO_STATE[fd] == 5, "punched it out");
	CUR_WEAPON = 2;

	// lost soul: charge toward the target, bite on contact, resume chase
	P_X = 1056.0;
	P_Y = -3050.0;
	P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
	P_HP = 100;
	int sk = mobj_spawn(1056.0, -3200.0, 3006, 0.0, 2, "SKUL", "AB", 100, 16.0);
	monster_tic(sk);
	check(MO_CHARGE[sk] == 1, "lost soul committed to a charge");
	float smom = math::sqrt(MO_MOMX[sk] * MO_MOMX[sk] + MO_MOMY[sk] * MO_MOMY[sk]);
	check(smom > 15.0, `charge momentum high (${smom:.1})`);
	int hp0 = P_HP;
	for (int st = 0; st < 40 && MO_CHARGE[sk] == 1; ++st) { monster_tic(sk); }
	check(P_HP < hp0, `charge bit on contact (${hp0} -> ${P_HP})`);
	check(MO_CHARGE[sk] == 0 && MO_STATE[sk] == 2, "skull resumed chase after the charge");

	// demon bite commit lunges forward
	P_X = 1056.0;
	P_Y = -3616.0;
	P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
	P_HP = 100;
	int sg = mobj_spawn(1056.0, -3526.0, 3002, 0.0, 2, "SARG", "ABCD", 150, 30.0);
	monster_tic(sg);
	check(MO_STATE[sg] == 3, "demon committed to a bite");
	check(MO_MOMX[sg] != 0.0 || MO_MOMY[sg] != 0.0, "bite commit lunges forward");
	float sy0 = MO_Y[sg];
	monster_tic(sg);
	check(MO_Y[sg] < sy0, `lunge moved the demon (${sy0:.0} -> ${MO_Y[sg]:.0})`);

	// ------------------------------------------------ playtest regression pins ---
	// clear the corridor: everything alive becomes a corpse so lanes are open
	for (int cm = 0; cm < MO_X.size(); ++cm) {
		int cst = MO_STATE[cm];
		if (cst == 1 || cst == 2 || cst == 3 || cst == 4 || cst == 9) {
			MO_STATE[cm] = 6;
			MO_BRAIN[cm] = null;
			MO_CHARGE[cm] = 0;
		}
	}

	// imp at range throws a fireball (kind-3 melee arm is 64, NOT its missile range)
	P_X = 1056.0;
	P_Y = -3550.0;
	P_FLOORZ = SE_FLOORH[sector_at(P_X, P_Y)];
	P_HP = 100;
	int imp = mobj_spawn(1056.0, -3250.0, 3001, 0.0, 2, "TROO", "ABCD", 60, 20.0);
	MO_COOL[imp] = 0;
	MO_TIC[imp] = 0;
	bool threw = false;
	for (int it2 = 0; it2 < 120 && !threw; ++it2) {
		monster_tic(imp);
		mobj_anim_tic(imp);
		for (int fm = 0; fm < MO_X.size(); ++fm) {
			if (MO_SPR[fm] == "BAL1" && (MO_STATE[fm] == 8 || MO_STATE[fm] == 5)) { threw = true; }
		}
	}
	check(threw, "imp at 300 units threw a fireball instead of clawing");

	// walking off a ledge leaves its line inside our radius; escape must stay legal
	int ledge = -1;
	float lnx = 0.0;
	float lny = 0.0;
	int nlines2 = LD_V1.size();
	for (int li = 0; li < nlines2 && ledge < 0; ++li) {
		int ls1 = LD_SIDE1[li];
		if (LD_SIDE0[li] < 0 || ls1 < 0 || LD_SPECIAL[li] != 0) { continue; }
		if ((LD_FLAGS[li] & 1) != 0) { continue; }
		int lf0 = SE_FLOORH[SD_SEC[LD_SIDE0[li]]];
		int lf1 = SE_FLOORH[SD_SEC[ls1]];
		int diff = lf0 > lf1 ? lf0 - lf1 : lf1 - lf0;
		if (diff <= 24) { continue; }
		float ax2 = VX_X[LD_V1[li]];
		float ay2 = VX_Y[LD_V1[li]];
		float bx2 = VX_X[LD_V2[li]];
		float by2 = VX_Y[LD_V2[li]];
		float ddx = bx2 - ax2;
		float ddy = by2 - ay2;
		float llen = math::sqrt(ddx * ddx + ddy * ddy);
		if (llen < 64.0) { continue; }
		float mx2 = (ax2 + bx2) / 2.0;
		float my2 = (ay2 + by2) / 2.0;
		float nnx = 0.0 - ddy / llen;
		float nny = ddx / llen;
		int lowf = lf0 < lf1 ? lf0 : lf1;
		float sgn = 0.0;
		if (SE_FLOORH[sector_at(mx2 + nnx * 8.0, my2 + nny * 8.0)] == lowf) { sgn = 1.0; }
		else if (SE_FLOORH[sector_at(mx2 - nnx * 8.0, my2 - nny * 8.0)] == lowf) { sgn = -1.0; }
		else { continue; }
		P_X = mx2 + nnx * sgn * 8.0;
		P_Y = my2 + nny * sgn * 8.0;
		P_FLOORZ = lowf;
		if (pos_ok(P_X, P_Y)) { continue; }          // want a genuinely violated spot
		P_MOMX = nnx * sgn * 8.0;
		P_MOMY = nny * sgn * 8.0;
		float d2a = seg_dist2(li, P_X, P_Y);
		p_move();
		if (seg_dist2(li, P_X, P_Y) > d2a) {
			ledge = li;
			lnx = nnx * sgn;
			lny = nny * sgn;
		}
	}
	check(ledge >= 0, "overlapping a ledge line: walking away escapes");
	if (ledge >= 0) {
		int lowsec = sector_at(P_X, P_Y);
		for (int pt = 0; pt < 10; ++pt) {
			P_MOMX = 0.0 - lnx * 12.0;
			P_MOMY = 0.0 - lny * 12.0;
			p_move();
		}
		check(sector_at(P_X, P_Y) == lowsec || SE_FLOORH[sector_at(P_X, P_Y)] == SE_FLOORH[lowsec],
			"the violated wall still cannot be crossed");

		// sprite beyond that step clips to the final window (armor-through-stairs mechanism)
		float ax3 = VX_X[LD_V1[ledge]];
		float ay3 = VX_Y[LD_V1[ledge]];
		float mx3 = (ax3 + VX_X[LD_V2[ledge]]) / 2.0;
		float my3 = (ay3 + VX_Y[LD_V2[ledge]]) / 2.0;
		float camx = mx3 + lnx * 120.0;
		float camy = my3 + lny * 120.0;
		int bar = mobj_spawn(mx3 - lnx * 80.0, my3 - lny * 80.0, 2035, 0.0, 9, "BAR1", "AB", 20, 10.0);
		SPR_DBG_M = bar;
		SPR_DBG_YMAX = -1;
		render_view(camx, camy, math::atan2(my3 - camy, mx3 - camx));
		SPR_DBG_M = -1;
		if (SPR_DBG_YMAX >= 0) {
			// bound: nothing below the window across the barrel's column span
			float bdx = MO_X[bar] - camx;
			float bdy = MO_Y[bar] - camy;
			float bdep = bdx * VCOS + bdy * VSIN;
			float blat = bdx * VSIN - bdy * VCOS;
			int blump = sprite_lookup("BAR1", "A", 0);
			sprite_prepare(blump);
			float bxs = FOCAL / bdep;
			float bx0 = CENTERX + blat / bdep * FOCAL - SPR_LO[blump] * bxs;
			int bxa = math::itrunc(bx0 < 0.0 ? 0.0 : bx0);
			int bxb = math::itrunc(bx0 + SPR_W[blump] * bxs);
			if (bxb >= VW) { bxb = VW - 1; }
			int maxcb = 0;
			for (int bc = bxa; bc <= bxb; ++bc) {
				if (CLIPBOT[bc] > maxcb) { maxcb = CLIPBOT[bc]; }
			}
			check(SPR_DBG_YMAX <= maxcb - 1, `sprite behind the step respects the clip window (${SPR_DBG_YMAX} vs ${maxcb - 1})`);
		} else {
			check(true, "barrel fully occluded behind the step (clip held)");
		}
	}

	// vertical autoaim: a monster on a platform >41 above eye level is hittable
	// (flat aim missed the z-span AND the flat wall trace ate the shot)
	bool plat_done = false;
	int nl3 = LD_V1.size();
	for (int pl = 0; pl < nl3 && !plat_done; ++pl) {
		int ps1 = LD_SIDE1[pl];
		if (LD_SIDE0[pl] < 0 || ps1 < 0 || LD_SPECIAL[pl] != 0) { continue; }
		int pf0 = SE_FLOORH[SD_SEC[LD_SIDE0[pl]]];
		int pf1 = SE_FLOORH[SD_SEC[ps1]];
		int pdiff = pf0 > pf1 ? pf0 - pf1 : pf1 - pf0;
		if (pdiff <= 45 || pdiff >= 120) { continue; }
		int pc0 = SE_CEILH[SD_SEC[LD_SIDE0[pl]]];
		int pc1 = SE_CEILH[SD_SEC[ps1]];
		int hif = pf0 > pf1 ? pf0 : pf1;
		int hic = pf0 > pf1 ? pc0 : pc1;
		if (hic - hif < 64) { continue; }            // target must fit up there
		float pax = VX_X[LD_V1[pl]];
		float pay = VX_Y[LD_V1[pl]];
		float pbx = VX_X[LD_V2[pl]];
		float pby = VX_Y[LD_V2[pl]];
		float pdx2 = pbx - pax;
		float pdy2 = pby - pay;
		float plen = math::sqrt(pdx2 * pdx2 + pdy2 * pdy2);
		if (plen < 64.0) { continue; }
		float pmx = (pax + pbx) / 2.0;
		float pmy = (pay + pby) / 2.0;
		float pnx = 0.0 - pdy2 / plen;
		float pny = pdx2 / plen;
		int lof = pf0 < pf1 ? pf0 : pf1;
		float psgn = 0.0;
		if (SE_FLOORH[sector_at(pmx + pnx * 24.0, pmy + pny * 24.0)] == lof) { psgn = 1.0; }
		else if (SE_FLOORH[sector_at(pmx - pnx * 24.0, pmy - pny * 24.0)] == lof) { psgn = -1.0; }
		else { continue; }
		float shx = pmx + pnx * psgn * 120.0;
		float shy = pmy + pny * psgn * 120.0;
		float tgx = pmx - pnx * psgn * 40.0;
		float tgy = pmy - pny * psgn * 40.0;
		if (SE_FLOORH[sector_at(shx, shy)] != lof) { continue; }
		if (SE_FLOORH[sector_at(tgx, tgy)] != hif) { continue; }
		int plat = mobj_spawn(tgx, tgy, 3004, 0.0, 1, "POSS", "ABCD", 60, 20.0);
		float peye = lof + 41.0;
		float paim = math::atan2(tgy - shy, tgx - shx);
		int phit = shoot_hitscan(shx, shy, peye, paim, 1, -1, 2048.0);
		check(phit == plat, `autoaim hit the platform monster ${pdiff} up (hit=${phit})`);
		MO_STATE[plat] = 6;
		plat_done = true;
	}
	check(plat_done, "found a platform scenario for the autoaim pin");

	// switches press in (SW1->SW2) and armed lines pop back out after 35 tics
	int swl = -1;
	for (int li2 = 0; li2 < LD_V1.size() && swl < 0; ++li2) {
		int s0b = LD_SIDE0[li2];
		if (s0b < 0 || LD_SPECIAL[li2] == 0) { continue; }
		if (SD_MID[s0b].starts_with("SW1") || SD_TOP[s0b].starts_with("SW1") || SD_BOT[s0b].starts_with("SW1")) { swl = li2; }
	}
	check(swl >= 0, "found a SW1 switch line");
	if (swl >= 0) {
		int seg0 = -1;
		for (int sgi = 0; sgi < SG_LINE.size(); ++sgi) {
			if (SG_LINE[sgi] == swl && SG_DIR[sgi] == 0) { seg0 = sgi; break; }
		}
		check(seg0 >= 0, "switch line has a front seg");
		int m0 = SEG_MIDT[seg0];
		int t0 = SEG_TOPT[seg0];
		int b0 = SEG_BOTT[seg0];
		switch_use(swl);
		check(SEG_MIDT[seg0] != m0 || SEG_TOPT[seg0] != t0 || SEG_BOTT[seg0] != b0, "switch face pressed in");
		for (int at = 0; at < 40; ++at) { anim_tic(); }
		check(SEG_MIDT[seg0] == m0 && SEG_TOPT[seg0] == t0 && SEG_BOTT[seg0] == b0, "armed switch popped back out");
	}

	// ------------------------------------------------- track 1: the arsenal ---
	sweep_corridor();

	// rocket launcher exists as weapon 5 with real art
	check(WEAPONS.size() == 6, `weapon 5 registered (${WEAPONS.size() - 1} weapons)`);
	check(sprite_lookup("MISG", "A", 8) >= 0 && sprite_lookup("MISL", "A", 0) >= 0
		&& sprite_lookup("MISL", "D", 0) >= 0, "MISG view + MISL flight/blast art present");

	// LAUN pickup arms weapon 5 with 2 rockets
	corridor_stand(1056.0, -3616.0);
	P_HP = 100;
	P_ARMOR = 0;
	HAVE_LAUNCHER = false;
	AMMO_ROCKETS = 0;
	CUR_WEAPON = 2;
	mobj_spawn(P_X, P_Y, 2003, 0.0, 7, "LAUN", "A", 0, 20.0);
	pickup_touch();
	check(HAVE_LAUNCHER && CUR_WEAPON == 5 && AMMO_ROCKETS == 2, `launcher pickup arms weapon 5 (${AMMO_ROCKETS} rockets)`);

	// rocket flies up the corridor, direct-hits the target, splash reaches the bystander
	int va = mobj_spawn(1056.0, -3300.0, 3004, 0.0, 1, "POSS", "ABCD", 20000, 20.0);
	int vb = mobj_spawn(996.0, -3300.0, 3004, 0.0, 1, "POSS", "ABCD", 20000, 20.0);
	P_ANG = math::PI / 2.0;
	W_COOL = 0;
	int nbr = MO_X.size();
	player_fire();
	check(AMMO_ROCKETS == 1, "firing consumed a rocket");
	int rk = -1;
	for (int r = nbr; r < MO_X.size(); ++r) {
		if (MO_SPR[r] == "MISL" && MO_STATE[r] == 8) { rk = r; }
	}
	check(rk >= 0, "player rocket spawned (shooter -1)");
	if (rk >= 0) {
		float rx0 = MO_X[rk];
		float ry0 = MO_Y[rk];
		for (int rt = 0; rt < 60 && MO_STATE[rk] == 8; ++rt) { projectile_tic(rk); }
		check(MO_Y[rk] > ry0 + 100.0 || MO_STATE[rk] != 8, `rocket flew (${ry0:.0} -> ${MO_Y[rk]:.0})`);
		check(MO_STATE[rk] == 5 && MO_ANIM[rk] == "BCD", "rocket exploded with MISL blast frames");
		check(MO_HP[va] < 20000, `direct hit damaged the target (${20000 - MO_HP[va]})`);
		check(MO_HP[vb] < 20000, `blast radius-damaged the bystander 60 to the side (${20000 - MO_HP[vb]})`);
	}
	MO_STATE[va] = 6; MO_BRAIN[va] = null;
	MO_STATE[vb] = 6; MO_BRAIN[vb] = null;

	// rocket jump: point-blank wall shot splashes and shoves the shooter
	corridor_stand(1056.0, -3500.0);
	float wt = trace_block_t(P_X, P_Y, P_X - 512.0, P_Y, P_FLOORZ + 41.0);
	check(wt < 1.0, "found the corridor's west wall");
	corridor_stand(1056.0 - wt * 512.0 + 40.0, -3500.0);
	P_ANG = math::PI;
	P_HP = 200;
	P_ARMOR = 0;
	P_MOMX = 0.0;
	P_MOMY = 0.0;
	W_COOL = 0;
	int nbj = MO_X.size();
	player_fire();
	int rj = -1;
	for (int r2 = nbj; r2 < MO_X.size(); ++r2) {
		if (MO_SPR[r2] == "MISL") { rj = r2; }
	}
	for (int rt2 = 0; rt2 < 20 && rj >= 0 && MO_STATE[rj] == 8; ++rt2) { projectile_tic(rj); }
	check(rj >= 0 && MO_STATE[rj] == 5, "wall rocket exploded");
	check(P_HP < 200, `rocket jump: own blast hurts (${200 - P_HP})`);
	check(P_MOMX > 0.0, `blast shoves the player away from the wall (momx ${P_MOMX:.1})`);
	AMMO_ROCKETS = 0;

	// berserk: fists multiply x10 for the level
	corridor_stand(1056.0, -3616.0);
	P_HP = 100;
	P_ANG = math::PI / 2.0;
	int sponge = mobj_spawn(1056.0, -3560.0, 3004, 0.0, 1, "POSS", "ABCD", 100000, 20.0);
	CUR_WEAPON = 1;
	P_BERSERK = false;
	int maxn = 0;
	for (int pn = 0; pn < 12; ++pn) {
		int h0 = MO_HP[sponge];
		W_COOL = 0;
		player_fire();
		if (h0 - MO_HP[sponge] > maxn) { maxn = h0 - MO_HP[sponge]; }
	}
	check(maxn > 0 && maxn <= 20, `normal fist tops out at 20 (max ${maxn})`);
	P_BERSERK = true;
	int maxb = 0;
	for (int pb = 0; pb < 12; ++pb) {
		int h1 = MO_HP[sponge];
		W_COOL = 0;
		player_fire();
		if (h1 - MO_HP[sponge] > maxb) { maxb = h1 - MO_HP[sponge]; }
	}
	check(maxb > 20, `berserk fist multiplies damage (max ${maxb})`);
	P_BERSERK = false;
	CUR_WEAPON = 2;
	MO_STATE[sponge] = 6; MO_BRAIN[sponge] = null;

	// invisibility: monster aim wobble widens (ATK_AIM vs the true bearing)
	int gunner = mobj_spawn(1056.0, -3200.0, 3004, 0.0, 2, "POSS", "ABCD", 100000, 20.0);
	corridor_stand(1056.0, -3616.0);
	P_HP = 30000;
	float bearing = math::atan2(P_Y - MO_Y[gunner], P_X - MO_X[gunner]);
	P_INVIS = 0;
	float devn = 0.0;
	for (int an = 0; an < 30; ++an) {
		monster_attack(gunner);
		float d = ATK_AIM - bearing;
		if (d < 0.0) { d = 0.0 - d; }
		if (d > devn) { devn = d; }
	}
	check(devn <= 0.061, `baseline aim wobble tight (${devn:.3})`);
	P_INVIS = 2100;
	float devi = 0.0;
	for (int ai = 0; ai < 30; ++ai) {
		monster_attack(gunner);
		float d2 = ATK_AIM - bearing;
		if (d2 < 0.0) { d2 = 0.0 - d2; }
		if (d2 > devi) { devi = d2; }
	}
	check(devi > 0.15, `invisibility widens the spread (${devi:.3})`);
	P_INVIS = 0;
	P_HP = 100;
	MO_STATE[gunner] = 6; MO_BRAIN[gunner] = null;

	// radiation suit: sector damage suppressed while it lasts
	int nuke = -1;
	for (int ns = 0; ns < SE_SPECIAL.size() && nuke < 0; ++ns) {
		if (SE_SPECIAL[ns] == 7 && find_interior(ns)) { nuke = ns; }
	}
	check(nuke >= 0, "found a standable nukage sector on E1M1");
	if (nuke >= 0) {
		corridor_stand(PT_X, PT_Y);
		P_HP = 100;
		P_ARMOR = 0;
		GAME_TIC = (GAME_TIC >> 5) << 5;
		P_SUIT = 2100;
		spec_tic();
		check(P_HP == 100, "suited: nukage pulse did nothing");
		P_SUIT = 0;
		spec_tic();
		check(P_HP == 95, `unsuited: same pulse dealt 5 (hp ${P_HP})`);
	}

	// backpack: doubles every cap once, hands out a spread of ammo
	corridor_stand(1056.0, -3616.0);
	P_BACKPACK = false;
	AMMO_BULLETS_MAX = 200; AMMO_SHELLS_MAX = 50; AMMO_ROCKETS_MAX = 50;
	AMMO_BULLETS = 0; AMMO_SHELLS = 0; AMMO_ROCKETS = 0;
	mobj_spawn(P_X, P_Y, 8, 0.0, 7, "BPAK", "A", 0, 20.0);
	pickup_touch();
	check(P_BACKPACK && AMMO_BULLETS_MAX == 400 && AMMO_SHELLS_MAX == 100 && AMMO_ROCKETS_MAX == 100, "backpack doubled all ammo caps");
	check(AMMO_BULLETS == 10 && AMMO_SHELLS == 4 && AMMO_ROCKETS == 1, "backpack gave bullets + shells + a rocket");
	mobj_spawn(P_X, P_Y, 8, 0.0, 7, "BPAK", "A", 0, 20.0);
	pickup_touch();
	check(AMMO_BULLETS_MAX == 400, "second backpack doesn't re-double the caps");

	// puffs: a wall shot leaves a PUFF mobj that animates out
	P_ANG = math::PI;
	CUR_WEAPON = 2;
	AMMO_BULLETS = 10;
	W_COOL = 0;
	int nbp = MO_X.size();
	player_fire();
	int puff = -1;
	for (int pp = nbp; pp < MO_X.size(); ++pp) {
		if (MO_SPR[pp] == "PUFF" && MO_STATE[pp] == 5) { puff = pp; }
	}
	check(puff >= 0, "wall hit spawned a bullet puff");
	if (puff >= 0) {
		for (int pt = 0; pt < 40; ++pt) { mobj_anim_tic(puff); }
		check(MO_STATE[puff] == 0, "puff animated out and vanished");
	}

	// blood: a monster hit bleeds BLUD instead
	int bleeder = mobj_spawn(1056.0, -3560.0, 3004, 0.0, 1, "POSS", "ABCD", 100000, 20.0);
	P_ANG = math::PI / 2.0;
	W_COOL = 0;
	int nbb = MO_X.size();
	player_fire();
	int blood = -1;
	for (int bb = nbb; bb < MO_X.size(); ++bb) {
		if (MO_SPR[bb] == "BLUD" && MO_STATE[bb] == 5) { blood = bb; }
	}
	check(blood >= 0, "monster hit spawned blood");
	if (blood >= 0) {
		for (int bt2 = 0; bt2 < 40; ++bt2) { mobj_anim_tic(blood); }
		check(MO_STATE[blood] == 0, "blood animated out and vanished");
	}
	MO_STATE[bleeder] = 6; MO_BRAIN[bleeder] = null;

	// gibs: overkill + xdeath art gibs, no-art types keep their normal death
	int gib1 = mobj_spawn(1056.0, -3400.0, 3004, 0.0, 1, "POSS", "ABCD", 20, 20.0);
	mobj_damage(gib1, 1000, -1);
	check(MO_STATE[gib1] == 5 && MO_ANIM[gib1].starts_with("M") && MO_ANIM[gib1].size() >= 8,
		`overkilled POSS gibs through xdeath letters (${MO_ANIM[gib1]})`);
	for (int gt = 0; gt < 80; ++gt) { mobj_anim_tic(gib1); }
	check(MO_STATE[gib1] == 6, "gib run ends in a corpse");
	int gib2 = mobj_spawn(1056.0, -3430.0, 3002, 0.0, 1, "SARG", "ABCD", 150, 30.0);
	mobj_damage(gib2, 10000, -1);
	check(MO_ANIM[gib2] == "IJKLMN", "SARG has no xdeath art: overkill keeps the normal death");
	int ng = mobj_spawn(1056.0, -3460.0, 3004, 0.0, 1, "POSS", "ABCD", 20, 20.0);
	mobj_damage(ng, 21, -1);
	check(MO_ANIM[ng] == "HIJKL", "non-overkill death keeps the normal anim");

	// spectre fuzz: same art, different pixels than a plain demon (staged on
	// the first scene's proven sightline, clear of the corridor's corpse pile)
	int spd = mobj_spawn(2912.0, -2766.0, 3002, 0.0, 1, "SARG", "ABCD", 150, 30.0);
	float spang = math::atan2(MO_Y[spd] - (0.0 - 2676.0), MO_X[spd] - 2912.0);
	render_view(2912.0, -2676.0, spang);
	int sigd = 0;
	for (int px3 = 0; px3 < VW * VIEWH; ++px3) { sigd = sigd + PIX[px3] * (px3 % 97); }
	MO_TYPE[spd] = 58;
	render_view(2912.0, -2676.0, spang);
	int sigs = 0;
	for (int px4 = 0; px4 < VW * VIEWH; ++px4) { sigs = sigs + PIX[px4] * (px4 % 97); }
	check(sigd != sigs, "spectre renders differently than the demon");
	bmp_write("p5_spectre.bmp");
	print("  wrote p5_spectre.bmp");
	MO_STATE[spd] = 6; MO_BRAIN[spd] = null;

	// animated flats cycle (E1M3 is nukage-rich)
	game_start_map("E1M3");
	check(ANIM_SLOTS.size() > 0, `animated flats registered (${ANIM_SLOTS.size()})`);
	if (ANIM_SLOTS.size() > 0) {
		int aslot = ANIM_SLOTS[0];
		var f0 = FLATS[aslot];
		for (int at2 = 0; at2 < 8; ++at2) { anim_tic(); }
		bool cycled = false;
		var& f1 = FLATS[aslot];
		for (int px2 = 0; px2 < 4096 && !cycled; ++px2) {
			if (f1[px2] != f0[px2]) { cycled = true; }
		}
		check(cycled, "flat animation cycles the texels");
	}

	// ---- decorations: the E1M1 census spawns, solids block, gore doesn't ----
	game_start_map("E1M1");
	int d48 = 0;
	int d2028 = 0;
	int d35 = 0;
	int dgore = 0;
	int dcol = -1;
	int dgori = -1;
	bool dnear = false;
	for (int dm = 0; dm < MO_STATE.size(); ++dm) {
		if (MO_STATE[dm] != 10) { continue; }
		int dty = MO_TYPE[dm];
		if (dty == 48) {
			d48 = d48 + 1;
			dcol = dm;
			// the west room with the armor staircase (both stand at x=288)
			if (math::distance(MO_X[dm], MO_Y[dm], 288.0, -3232.0) < 300.0) { dnear = true; }
		}
		else if (dty == 2028) { d2028 = d2028 + 1; }
		else if (dty == 35) { d35 = d35 + 1; }
		else if (dty == 10 || dty == 12 || dty == 15 || dty == 24) {
			dgore = dgore + 1;
			if (dgori < 0) { dgori = dm; }
		}
	}
	check(d48 == 2, `two ELEC techno columns spawned (${d48})`);
	check(d2028 == 8, `eight COLU floor lamps spawned (${d2028})`);
	check(d35 == 2, `two CBRA candelabras spawned (${d35})`);
	check(dgore >= 10, `gore decorations spawned (${dgore})`);
	check(dnear, "a techno column stands in the armor room");
	check(dcol >= 0 && MO_RAD[dcol] > 0.0 && "" + MO_SPR[dcol] == "ELEC", "columns are solid ELEC sprites");
	check(dgori >= 0 && MO_RAD[dgori] == 0.0, "gore carries no collision radius");
	check(!pos_ok(MO_X[dcol] + 4.0, MO_Y[dcol]), "player blocked inside a column");
	check(pos_ok_scan(MO_X[dcol] + 4.0, MO_Y[dcol], MO_X[dcol] + 8.0, MO_Y[dcol], true),
		"an overlapped player may still step (escape stays legal)");
	int dmon = -1;
	for (int dm2 = 0; dm2 < MO_STATE.size(); ++dm2) {
		if (MO_STATE[dm2] == 1 && MINFO.has(MO_TYPE[dm2])) { dmon = dm2; break; }
	}
	check(dmon >= 0 && !mobj_pos_ok(dmon, MO_X[dcol] + 4.0, MO_Y[dcol], false), "monsters blocked by solid decor");
	// the column must actually land pixels: render the room with and without it
	if (place_player_facing(dcol)) {
		render_view(P_X, P_Y, P_ANG);
		int sigc = 0;
		for (int dpx = 0; dpx < VW * VIEWH; ++dpx) { sigc = sigc + PIX[dpx] * (dpx % 97); }
		bmp_write("p5_decor.bmp");
		print("  wrote p5_decor.bmp (the armor-room techno column)");
		int dst = MO_STATE[dcol];
		MO_STATE[dcol] = 0;
		render_view(P_X, P_Y, P_ANG);
		int sign = 0;
		for (int dpx2 = 0; dpx2 < VW * VIEWH; ++dpx2) { sign = sign + PIX[dpx2] * (dpx2 % 97); }
		MO_STATE[dcol] = dst;
		check(sigc != sign, "the column renders");
	} else {
		check(false, "no vantage on the techno column");
	}

	print("");
	print(FAILS == 0 ? "P5 GATE: ALL GREEN" : `P5 GATE: ${FAILS} FAILURES`);
	return FAILS;
}

main()
