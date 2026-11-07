// DONUT — the torus homage, grown up: sphere-traced per pixel instead of
// splatted, rows across every core with parallel_transform. Each ray is turned
// into the donut's own frame (the tumble is a rotation matrix built once a
// frame), clipped to the bounding sphere, and marched on the torus distance
// field; the hit's exact normal is lit with a diffuse term and a Blinn
// specular (N.H to the 24th by repeated squaring). And it is FROSTED: pink
// icing with a wavy drip edge over the top of the tube, sprinkles scattered
// on a hashed grid in the torus's own (phi, theta) coordinates, so they ride
// the surface as it turns.

float DN_M00 = 1.0; float DN_M01 = 0.0; float DN_M02 = 0.0;
float DN_M10 = 0.0; float DN_M11 = 1.0; float DN_M12 = 0.0;
float DN_M20 = 0.0; float DN_M21 = 0.0; float DN_M22 = 1.0;
float DN_LX = 0.0; float DN_LY = 0.0; float DN_LZ = 0.0;
float DN_HX = 0.0; float DN_HY = 0.0; float DN_HZ = 0.0;
float DN_CAM = 5.6;
int DN_W = 0;
int DN_H = 0;
int DN_MAT = 0;
int DN_SEED = 0;
array<int> DN_ROWS = [];

function donut_row(int y) -> array<int> {
	array<int> out = [];
	int ww = DN_W;
	int hh = DN_H;
	float sc = 1.75 / ww;
	float v = (hh * 0.5 - y - 0.5) * sc;
	// camera at (0, 0, -cam) looking down +z, turned into the donut's frame
	float cam = DN_CAM;
	float ox = 0.0 - DN_M02 * cam;
	float oy = 0.0 - DN_M12 * cam;
	float oz = 0.0 - DN_M22 * cam;
	float oo = ox * ox + oy * oy + oz * oz;
	for (int x = 0; x < ww; ++x) {
		float u = (x + 0.5 - ww * 0.5) * sc;
		float il = 1.0 / math::sqrt(u * u + v * v + 1.0);
		float wx = u * il;
		float wy = v * il;
		float wz = il;
		float dx = DN_M00 * wx + DN_M01 * wy + DN_M02 * wz;
		float dy = DN_M10 * wx + DN_M11 * wy + DN_M12 * wz;
		float dz = DN_M20 * wx + DN_M21 * wy + DN_M22 * wz;
		// bounding sphere, radius 3.05
		float b = ox * dx + oy * dy + oz * dz;
		float disc = b * b - oo + 9.3;
		if (disc <= 0.0) { out.push(y); continue; }
		float sq = math::sqrt(disc);
		float t = 0.0 - b - sq;
		float tfar = 0.0 - b + sq;
		int hit = 0;
		float px = 0.0; float py = 0.0; float pz = 0.0; float ring = 0.0;
		for (int step = 0; step < 40; ++step) {
			px = ox + dx * t;
			py = oy + dy * t;
			pz = oz + dz * t;
			ring = math::sqrt(px * px + pz * pz);
			float qx = ring - 2.0;
			float d = math::sqrt(qx * qx + py * py) - 1.0;
			if (d < 0.004) { hit = 1; break; }
			t += d;
			if (t > tfar) { break; }
		}
		if (hit == 0) { out.push(y); continue; }
		// exact normal: from the tube's center circle out through the hit
		float rs = 2.0 / (ring > 0.0001 ? ring : 0.0001);
		float nx = px - px * rs;
		float ny = py;
		float nz = pz - pz * rs;
		float nl = 1.0 / math::sqrt(nx * nx + ny * ny + nz * nz);
		nx *= nl; ny *= nl; nz *= nl;
		float dl = nx * DN_LX + ny * DN_LY + nz * DN_LZ;
		dl = dl < 0.0 ? 0.0 : dl;
		float s = nx * DN_HX + ny * DN_HY + nz * DN_HZ;
		s = s < 0.0 ? 0.0 : s;
		float s2 = s * s;
		float s4 = s2 * s2;
		float s8 = s4 * s4;
		float s16 = s8 * s8;
		float phi = math::atan2(pz, px);
		int m = 0;
		if (ny > -0.22 + 0.16 * math::sin(phi * 5.0 + 2.0 * math::sin(phi * 3.0))) {
			m = 1;
			float th = math::atan2(ny, nx * px / ring + nz * pz / ring);
			float fp = (phi + 3.14159265) * 9.5;
			float ft = (th + 3.14159265) * 3.5;
			int cp = fp;
			int ct = ft;
			int hs = ((cp * 73856093 + ct * 19349663 + DN_SEED) & 1048575) * 1274126177;
			int r = (hs >> 12) & 1023;
			float ep = fp - cp - 0.5;
			float et = ft - ct - 0.5;
			bool across = (r & 1) == 0;
			float la = across ? ep : et;
			float lb = across ? et : ep;
			if (r < 560 && la * la < 0.13 && lb * lb < 0.024) { m = 2 + r % 5; }
		}
		int di = dl * 15.99;
		int si = s16 * s8 * 3.99;
		out.push(DN_MAT + m * 64 + di * 4 + si);
	}
	return out;
}

class Donut : Scene {
	int mat0 = 0;
	float tt = 0.0;

	void build() override {
		name = "DONUT";
		handle = "torus and effect . by strong_ptr";
		// background: warm bakery dusk, one entry per row
		for (int y = 0; y < h; ++y) {
			float f = y / (h - 1.0);
			pal.add(34.0 + 30.0 * f, 18.0 + 14.0 * f, 30.0 - 8.0 * f);
		}
		// materials: 16 diffuse levels x 4 specular levels each
		mat0 = pal.size();
		var mats = [[196.0, 128.0, 64.0], [255.0, 132.0, 186.0], [255.0, 60.0, 60.0], [255.0, 220.0, 60.0],
			[70.0, 150.0, 255.0], [90.0, 230.0, 110.0], [250.0, 250.0, 250.0]];
		for (int m = 0; m < 7; ++m) {
			var c = mats[m];
			float cr = c[0]; float cg = c[1]; float cb = c[2];
			for (int d = 0; d < 16; ++d) {
				float df = 0.10 + 0.90 * d / 15.0;
				for (int s = 0; s < 4; ++s) {
					float sp = s / 3.0 * (m == 0 ? 0.45 : 0.9);
					pal.add(cr * df + (255.0 - cr * df) * sp, cg * df + (255.0 - cg * df) * sp, cb * df + (255.0 - cb * df) * sp);
				}
			}
		}
	}

	void update(float t, float dt) override {
		tt = t;
	}

	void render(var& grid) override {
		// object-from-world rotation: the transpose of Rz(b) * Rx(a)
		float ca = math::cos(tt * 0.9);
		float sa = math::sin(tt * 0.9);
		float cb = math::cos(tt * 0.53 + 1.2);
		float sb = math::sin(tt * 0.53 + 1.2);
		DN_M00 = cb;        DN_M01 = sb;        DN_M02 = 0.0;
		DN_M10 = 0.0 - sb * ca; DN_M11 = cb * ca; DN_M12 = sa;
		DN_M20 = sb * sa;   DN_M21 = 0.0 - cb * sa; DN_M22 = ca;
		// light and half vector, world space (camera looks down +z), into the donut's frame
		float lx = -0.42; float ly = 0.66; float lz = -0.62;
		float hx = lx; float hy = ly; float hz = lz - 1.0;
		float hl = 1.0 / math::sqrt(hx * hx + hy * hy + hz * hz);
		hx *= hl; hy *= hl; hz *= hl;
		DN_LX = DN_M00 * lx + DN_M01 * ly + DN_M02 * lz;
		DN_LY = DN_M10 * lx + DN_M11 * ly + DN_M12 * lz;
		DN_LZ = DN_M20 * lx + DN_M21 * ly + DN_M22 * lz;
		DN_HX = DN_M00 * hx + DN_M01 * hy + DN_M02 * hz;
		DN_HY = DN_M10 * hx + DN_M11 * hy + DN_M12 * hz;
		DN_HZ = DN_M20 * hx + DN_M21 * hy + DN_M22 * hz;
		DN_W = w;
		DN_H = h;
		DN_MAT = mat0;
		DN_SEED = seed & 1023;
		if (DN_ROWS.size() != h) {
			DN_ROWS = [];
			for (int r = 0; r < h; ++r) { DN_ROWS.push(r); }
		}
		var rows = parallel_transform(DN_ROWS, donut_row);
		int idx = 0;
		for (int y = 0; y < h; ++y) {
			var& row = rows[y];
			for (int x = 0; x < w; ++x) {
				grid[idx] = row[x];
				++idx;
			}
		}
	}
}
