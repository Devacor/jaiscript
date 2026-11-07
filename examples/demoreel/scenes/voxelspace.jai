// CANYONRUN — the Comanche (1992) VoxelSpace algorithm, all script. A 128x128
// wrapping heightmap is grown at build time by diamond-square (pure integer
// hashing, no host rng in the hot loop), a river canyon is carved along a
// wrapping sine meander, and a colormap bakes height bands + north-light slope
// shading into 16 color indices. Rendering is the classic front-to-back
// per-depth-line march with a per-column y-buffer: for each of ~58 depth steps
// the camera frustum line is interpolated across the screen in 16.16 fixed
// point (integer adds only — the inner loop has ZERO host calls and no float
// math), each sample projects to a screen row, and only rows above the y-buffer
// are filled. Total writes are bounded by w*h. Distance haze is 6 precomputed
// palette banks per terrain color; the camera flies a scripted path that
// follows the river's own meander formula, banking into turns (per-column
// horizon roll offsets) and diving on a slow altitude wave with terrain
// clearance lookahead. Column-marching terrain in a terminal, like it's 1992.

class CanyonRun : Scene {
	var hmap = [];         // 128x128 heights 0..255
	var cmap = [];         // 128x128 color indices 0..15 (band*2 + slope shade)
	var ybuf = [];         // per-column y-buffer
	var rolloff = [];      // per-column banking offset, rebuilt each frame
	var zs = [];           // per-step depth
	var invz = [];         // per-step projection scale, 16.16 fixed
	var hazes = [];        // per-step haze bank 0..5
	int msz = 128;
	int mmask = 127;
	int tbase = 0;         // terrain palette base (sky rows sit at 0..h-1)
	int nsteps = 72;
	int nhaze = 10;
	int sunb = 0;
	float posx = 64.0;
	float posy = 0.0;
	float dirx = 0.0;
	float diry = 1.0;
	float planex = 0.0;
	float planey = 0.0;
	float pdx = 0.0;
	float pdy = 1.0;
	float camhf = 210.0;
	float roll = 0.0;
	float ph1 = 0.0;
	float ph2 = 0.0;
	int camh = 210;
	int horizon = 12;
	float tt = 0.0;

	void build() override {
		name = "CANYONRUN";
		handle = "river meander . a heightmap, a y-buffer, 1992";
		// sky: entries 0..h-1, deep dusk blue up top warming toward the horizon
		for (int py = 0; py < h; ++py) {
			float f = py / (h - 1.0);
			float f2 = f * f;
			pal.add(16.0 + 150.0 * f2 + 30.0 * f, 24.0 + 138.0 * f2 + 18.0 * f, 64.0 + 130.0 * f);
		}
		tbase = h;
		// 8 height bands x 4 light shades x 10 haze banks (haze lerps toward the sky)
		var bands = [[16.0, 48.0, 112.0], [30.0, 94.0, 158.0], [152.0, 134.0, 94.0], [98.0, 128.0, 58.0],
			[154.0, 88.0, 54.0], [190.0, 124.0, 78.0], [146.0, 142.0, 136.0], [238.0, 242.0, 248.0]];
		for (int b = 0; b < 8; ++b) {
			var bc = bands[b];
			float br = bc[0];
			float bg = bc[1];
			float bb = bc[2];
			for (int sh = 0; sh < 4; ++sh) {
				float k = 0.52 + sh * 0.20;
				float cr = br * k;
				float cg = bg * k;
				float cb = bb * k;
				for (int hq = 0; hq < nhaze; ++hq) {
					float hf = hq * 0.093;
					pal.add(cr + (176.0 - cr) * hf, cg + (176.0 - cg) * hf, cb + (206.0 - cb) * hf);
				}
			}
		}
		// diamond-square with wraparound; displacement comes from an inline
		// integer hash (masked before the multiply so checked overflow never fires)
		hmap = gfx.make_grid(msz * msz);
		int sd = seed & 65535;
		hmap[0] = 110 + (sd & 63);
		int step = msz;
		int amp = 92;
		while (step > 1) {
			int half = step / 2;
			for (int dy = half; dy < msz; dy += step) {
				int rw0 = ((dy - half) & mmask) * msz;
				int rw1 = ((dy + half) & mmask) * msz;
				int rwc = dy * msz;
				for (int dx = half; dx < msz; dx += step) {
					int x0 = dx - half;
					int x1 = (dx + half) & mmask;
					int a1 = hmap[rw0 + x0];
					int a2 = hmap[rw0 + x1];
					int a3 = hmap[rw1 + x0];
					int a4 = hmap[rw1 + x1];
					int hsh = (dx * 73856093 + dy * 19349663 + step * 83492791 + sd * 2971) & 1048575;
					int d = (((hsh * 1274126177) / 4096) & 255) - 128;
					hmap[rwc + dx] = (a1 + a2 + a3 + a4) / 4 + d * amp / 128;
				}
			}
			for (int sy = 0; sy < msz; sy += half) {
				int xoff = (sy % step) == 0 ? half : 0;
				int rws = sy * msz;
				int rwu = ((sy - half) & mmask) * msz;
				int rwd = ((sy + half) & mmask) * msz;
				for (int sx = xoff; sx < msz; sx += step) {
					int xl = (sx - half) & mmask;
					int xr = (sx + half) & mmask;
					int b1 = hmap[rws + xl];
					int b2 = hmap[rws + xr];
					int b3 = hmap[rwu + sx];
					int b4 = hmap[rwd + sx];
					int hsh2 = (sx * 19349663 + sy * 73856093 + step * 297121507 + sd * 7919) & 1048575;
					int d2 = (((hsh2 * 1274126177) / 4096) & 255) - 128;
					hmap[rws + sx] = (b1 + b2 + b3 + b4) / 4 + d2 * amp / 128;
				}
			}
			step = half;
			amp = amp * 11 / 20;
			amp = amp < 3 ? 3 : amp;
		}
		// range scan, then normalize + carve the river canyon in one integer pass.
		// the meander wraps (integer harmonics of 2*pi/128) so the flight loops seamlessly
		int hmin = 999999;
		int hmax = -999999;
		int nm = msz * msz;
		for (int q = 0; q < nm; ++q) {
			int hv0 = hmap[q];
			hmin = hv0 < hmin ? hv0 : hmin;
			hmax = hv0 > hmax ? hv0 : hmax;
		}
		int range = hmax - hmin;
		range = range < 1 ? 1 : range;
		ph1 = rng.nextf() * 6.283185307;
		ph2 = rng.nextf() * 6.283185307;
		var rivc = [];
		for (int ry = 0; ry < msz; ++ry) {
			rivc.push(itrunc(64.0 + 26.0 * math::sin(ry * 0.04908738521 + ph1) + 11.0 * math::sin(ry * 0.1472621556 + ph2)));
		}
		var falltab = [];   // smoothstep carve profile in 1/256ths, by distance from the river axis
		for (int fd = 0; fd < 16; ++fd) {
			float ff = fd / 15.0;
			falltab.push(itrunc(ff * ff * (3.0 - 2.0 * ff) * 256.0));
		}
		for (int cy = 0; cy < msz; ++cy) {
			int rc = rivc[cy];
			int rowc = cy * msz;
			for (int cx = 0; cx < msz; ++cx) {
				int hraw = hmap[rowc + cx];
				int hv = (hraw - hmin) * 255 / range;
				int dxr = cx - rc;
				dxr = dxr > 64 ? dxr - 128 : dxr;
				dxr = dxr < -64 ? dxr + 128 : dxr;
				int ad = dxr < 0 ? 0 - dxr : dxr;
				if (ad < 16) {
					int f256 = falltab[ad > 15 ? 15 : ad];
					hv = 50 + (hv - 50) * f256 / 256;
				}
				hv = hv < 0 ? 0 : hv;
				hv = hv > 255 ? 255 : hv;
				hmap[rowc + cx] = hv;
			}
		}
		// colormap: water lives ONLY in the carved channel (banding low terrain
		// as water flooded half the map); heights band into sand/scrub/mesa/rock/
		// snow with four levels of slope light, then the color index is PACKED
		// into the height cell (hv*32 + ci) so the render inner loop pays one
		// array read per sample instead of two
		cmap = gfx.make_grid(nm);
		for (int my = 0; my < msz; ++my) {
			int rowm = my * msz;
			int rown = ((my + 1) & mmask) * msz;
			int rc2 = rivc[my];
			for (int mx = 0; mx < msz; ++mx) {
				int hv2 = hmap[rowm + mx];
				int hn = hmap[rown + mx];
				int dxr2 = mx - rc2;
				dxr2 = dxr2 > 64 ? dxr2 - 128 : dxr2;
				dxr2 = dxr2 < -64 ? dxr2 + 128 : dxr2;
				int ad2 = dxr2 < 0 ? 0 - dxr2 : dxr2;
				int band = 7;
				if (ad2 < 3) { band = 0; }
				else if (ad2 < 6) { band = 1; }
				else if (hv2 <= 80) { band = 2; }
				else if (hv2 <= 118) { band = 3; }
				else if (hv2 <= 165) { band = 4; }
				else if (hv2 <= 205) { band = 5; }
				else if (hv2 <= 235) { band = 6; }
				// sun from the south-west: lit when the ground falls away toward it,
				// plus a little hashed grain so broad faces are not flat blocks
				int he = hmap[rowm + ((mx + 1) & mmask)];
				int hs = hmap[((my - 1) & mmask) * msz + mx];
				int hw = hmap[rowm + ((mx - 1) & mmask)];
				int grain = (((mx * 73856093 + my * 19349663) & 1048575) * 1274126177 >> 12) & 3;
				int lit = 6 + (hw - he) + (hs - hn) + grain - 1;
				int shade = band < 2 ? 1 + grain / 2 : (lit < 3 ? 0 : (lit < 7 ? 1 : (lit < 11 ? 2 : 3)));
				cmap[rowm + mx] = band * 4 + shade;
			}
		}
		for (int q2 = 0; q2 < nm; ++q2) { cmap[q2] = hmap[q2] * 32 + cmap[q2]; }
		// depth tables: geometric steps, projection scale and haze bank per step
		float z = 2.4;
		float hsc = h * 0.085;
		for (int i = 0; i < nsteps; ++i) {
			zs.push(z);
			invz.push(itrunc(hsc * 65536.0 / z));
			float zr = i / (nsteps - 1.0);
			int hzv = itrunc(zr * zr * (nhaze + 0.6));
			hazes.push(hzv > nhaze - 1 ? nhaze - 1 : hzv);
			z *= 1.058;
		}
		// the sun: a glow ramp toward white, drawn only where sky shows through
		sunb = pal.gradient(6, [[255.0, 170.0, 110.0], [255.0, 210.0, 150.0], [255.0, 244.0, 214.0]]);
		ybuf = gfx.make_grid(w);
		rolloff = gfx.make_grid(w);
	}

	void update(float t, float dt) override {
		tt = t;
		float sp = 8.2;
		float u = t * sp;
		posy = u;
		float wv = 13.0 * math::sin(t * 0.5 + 0.8);
		posx = 64.0 + 26.0 * math::sin(u * 0.04908738521 + ph1) + 11.0 * math::sin(u * 0.1472621556 + ph2) + wv;
		// heading from a look-ahead point on the same path: the turns steer themselves
		float u2 = u + 7.5;
		float wv2 = 13.0 * math::sin((t + 7.5 / sp) * 0.5 + 0.8);
		float lx = 64.0 + 26.0 * math::sin(u2 * 0.04908738521 + ph1) + 11.0 * math::sin(u2 * 0.1472621556 + ph2) + wv2;
		float ddx = lx - posx;
		float dl = math::sqrt(ddx * ddx + 56.25);
		dirx = ddx / dl;
		diry = 7.5 / dl;
		planex = 0.0 - diry * 0.92;
		planey = dirx * 0.92;
		// bank into the turn: roll target from the heading's turn rate
		if (dt > 0.0001) {
			float cr = (pdx * diry - pdy * dirx) / dt;
			float rt = cr * 1.05;
			rt = rt > 0.55 ? 0.55 : rt;
			rt = rt < -0.55 ? -0.55 : rt;
			float mixr = dt * 3.2;
			mixr = mixr > 1.0 ? 1.0 : mixr;
			roll += (rt - roll) * mixr;
		}
		pdx = dirx;
		pdy = diry;
		// altitude: slow dive/climb wave, floored by terrain clearance ahead
		int g0 = hmap[((ifloor(posy) & mmask) * msz) + (ifloor(posx) & mmask)];
		int g1 = hmap[((ifloor(posy + diry * 6.0) & mmask) * msz) + (ifloor(posx + dirx * 6.0) & mmask)];
		int g2 = hmap[((ifloor(posy + diry * 14.0) & mmask) * msz) + (ifloor(posx + dirx * 14.0) & mmask)];
		int gm = g0 > g1 ? g0 : g1;
		gm = g2 > gm ? g2 : gm;
		float base = 118.0 + 74.0 * math::sin(t * 0.31 + 4.4);
		float floorh = gm + 30.0;
		float targ = floorh > base ? floorh : base;
		if (dt > 0.0001) {
			float mixa = dt * 1.9;
			mixa = mixa > 1.0 ? 1.0 : mixa;
			camhf += (targ - camhf) * mixa;
		}
		camh = itrunc(camhf);
		// pitch: nose down when high over the terrain ahead, plus a gentle bob
		float pit = (camhf - (gm + 70.0)) * 0.0020 + 0.06 * math::sin(t * 0.23);
		pit = pit > 0.20 ? 0.20 : pit;
		pit = pit < -0.14 ? -0.14 : pit;
		horizon = itrunc(h * (0.30 + pit));
	}

	// field access resolves through the environment chain per hit — ~2x the cost
	// of a local slot — so every array the inner loop touches is bound to a
	// var& reference declaration (plain assignment would deep-copy the array)
	// and every hot scalar is copied to a local first
	void render(var& grid) override {
		int ww = w;
		int hh = h;
		int mm = mmask;
		int ms = msz;
		int tb = tbase;
		int cam = camh;
		int horiz = horizon;
		int ns = nsteps;
		int nh = nhaze;
		float dx_ = dirx;
		float dy_ = diry;
		float px_ = planex;
		float py_ = planey;
		var& tm = cmap;   // packed terrain: hv*32 + color index (ref decl: no copy)
		var& yb = ybuf;
		var& ro = rolloff;
		var& zt = zs;
		var& izt = invz;
		var& hzt = hazes;
		int w2 = ww / 2;
		int rollfp = itrunc(roll * 65536.0);
		for (int x = 0; x < ww; ++x) {
			yb[x] = hh;
			ro[x] = horiz + rollfp * (x - w2) / 65536;   // horizon folded in
		}
		// +4096 keeps the fixed-point coords positive so /65536 floors exactly
		float bigx = posx + 4096.0;
		float bigy = posy + 4096.0;
		for (int s = 0; s < ns; ++s) {
			float z = zt[s];
			int iz = izt[s];
			int hz = hzt[s];
			float plx = bigx + (dx_ - px_) * z;
			float ply = bigy + (dy_ - py_) * z;
			float prx = bigx + (dx_ + px_) * z;
			float pry = bigy + (dy_ + py_) * z;
			int rxf = itrunc(plx * 65536.0);
			int ryf = itrunc(ply * 65536.0);
			int dxf = itrunc((prx - plx) * 65536.0 / ww);
			int dyf = itrunc((pry - ply) * 65536.0 / ww);
			for (int x2 = 0; x2 < ww; ++x2) {
				int ybv = yb[x2];
				if (ybv > 0) {
					int mi = ((ryf >> 16) & mm) * ms + ((rxf >> 16) & mm);   // coords kept positive: shift == floor
					int tv = tm[mi];
					int sy = (cam - (tv >> 5)) * iz / 65536 + ro[x2];
					if (sy < ybv) {
						sy = sy < 0 ? 0 : sy;
						int pi = tb + (tv & 31) * nh + hz;
						int gi = sy * ww + x2;
						int yy = sy;
						while (yy < ybv) { grid[gi] = pi; gi += ww; ++yy; }
						yb[x2] = sy;
					}
				}
				rxf += dxf;
				ryf += dyf;
			}
		}
		// whatever the terrain never reached is sky (row-gradient entries 0..h-1)
		for (int x3 = 0; x3 < ww; ++x3) {
			int yb2 = yb[x3];
			int gi2 = x3;
			int yy2 = 0;
			while (yy2 < yb2) { grid[gi2] = yy2; gi2 += ww; ++yy2; }
		}
		// the sun sits at a fixed bearing: its screen column follows the heading
		float rel = math::atan2(dx_, dy_) - 0.55;
		float sxf = ww * 0.5 - rel * ww * 0.55;
		float syf = horiz - hh * 0.16;
		float sr = hh * 0.07;
		int sx0 = itrunc(sxf - sr * 2.2);
		int sx1 = itrunc(sxf + sr * 2.2);
		int sy0 = itrunc(syf - sr * 2.2);
		int sy1 = itrunc(syf + sr * 2.2);
		for (int sy = sy0; sy <= sy1; ++sy) {
			if (sy < 0 || sy >= hh) { continue; }
			for (int sx = sx0; sx <= sx1; ++sx) {
				if (sx < 0 || sx >= ww || sy >= yb[sx]) { continue; }
				float ddx = sx + 0.5 - sxf;
				float ddy = sy + 0.5 - syf;
				float d = math::sqrt(ddx * ddx + ddy * ddy) / sr;
				if (d < 1.0) { grid[sy * ww + sx] = sunb + 5; }
				else if (d < 2.2) {
					int gl = itrunc((2.2 - d) * 4.2);
					if (gl > 0) { grid[sy * ww + sx] = sunb + (gl > 4 ? 4 : gl - 1); }
				}
			}
		}
	}
}
