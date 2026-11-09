// The showcase look's light: a grid of light texels, LIGHT_SUB to a tile edge,
// each holding a light variant (a channel and a level, 0 unlit). Ceiling lamps
// and the glowing fixtures (the exit switch, the lock lights) are baked when a
// floor loads or a door opens; the muzzle flash, projectiles and blasts are
// stamped over the bake every frame. Light flows through open tiles only, so it
// rounds corners and stops at walls. Surfaces read their texel and look the lit
// color up in a table per variant (look.jai builds them), so a lit pixel costs
// what an unlit one does.

int LIGHT_SUB = 4;
int LIGHT_LEVELS = 4;
int LIGHT_LAMP = 0;
int LIGHT_FIRE = 1;
int LIGHT_HEX = 2;
int LIGHT_ACID = 3;
int LIGHT_RED = 4;
int LIGHT_BLUE = 5;
int LIGHT_CHANNELS = 6;
int LIGHT_VARIANTS = 25;     // unlit + channels x levels

// ordered dither over the light texels: light falls off in a fine stipple, not in rings
array<float> BAYER4 = [0.0, 0.5, 0.125, 0.625, 0.75, 0.25, 0.875, 0.375, 0.1875, 0.6875, 0.0625, 0.5625, 0.9375,
	0.4375, 0.8125, 0.3125];

function light_level(int variant) -> int { return variant == 0 ? 0 : (variant - 1) % LIGHT_LEVELS + 1; }

class LightGrid {
public:
	int gw = 0;                   // texels across
	int gh = 0;
	array<int> grid = [];         // this frame's light
	array<bool> lamps = [];       // per tile: a lamp hangs over it

	// a floor's lamps: open tiles, LAMP_SPACING apart, picked in a fixed order from a hash
	void load() {
		auto& world = G.world;
		gw = world.mw * LIGHT_SUB;
		gh = world.mh * LIGHT_SUB;
		lamps = [];
		seen = [];
		for (int i = 0; i < world.mw * world.mh; ++i) {
			lamps.push(false);
			seen.push(0);
		}
		for (int ty = 1; ty < world.mh - 1; ++ty) {
			for (int tx = 1; tx < world.mw - 1; ++tx) {
				if (world.tiles[ty * world.mw + tx] != 0 || texel_noise(11, tx, ty) % 3 != 0) { continue; }
				if (!lamp_near(tx, ty)) { lamps[ty * world.mw + tx] = true; }
			}
		}
		baked_gen = -1;
	}

	// the bake, redone when a door or a secret opens
	void frame(array<LightSource>& sources) {
		if (baked_gen != G.world.mapgen) {
			bake();
			baked_gen = G.world.mapgen;
		}
		grid = baked;
		for (auto& s : sources) { stamp(grid, s.x, s.y, s.channel, s.radius, s.power); }
	}

	int at(float x, float y) {
		int gx = x * LIGHT_SUB;
		int gy = y * LIGHT_SUB;
		if (gx < 0 || gy < 0 || gx >= gw || gy >= gh) { return 0; }
		return grid[gy * gw + gx];
	}

private:
	array<int> baked = [];
	int baked_gen = -1;
	array<int> seen = [];         // per tile: the stamp that last reached it
	int stamp_id = 0;
	array<int> queue = [];

	bool lamp_near(int tx, int ty) {
		auto& world = G.world;
		for (int y = imax(0, ty - LAMP_SPACING + 1); y < imin(world.mh, ty + LAMP_SPACING); ++y) {
			for (int x = imax(0, tx - LAMP_SPACING + 1); x < imin(world.mw, tx + LAMP_SPACING); ++x) {
				if (lamps[y * world.mw + x]) { return true; }
			}
		}
		return false;
	}

	void bake() {
		auto& world = G.world;
		baked = [];
		for (int i = 0; i < gw * gh; ++i) { baked.push(0); }
		for (int ty = 0; ty < world.mh; ++ty) {
			for (int tx = 0; tx < world.mw; ++tx) {
				int t = world.tiles[ty * world.mw + tx];
				if (lamps[ty * world.mw + tx]) { stamp(baked, tx + 0.5, ty + 0.5, LIGHT_LAMP, LAMP_RADIUS, LAMP_POWER); }
				int channel = t == 6 ? LIGHT_ACID : (t == 8 ? LIGHT_RED : (t == 9 ? LIGHT_BLUE : -1));
				if (channel < 0) { continue; }
				for (auto& step : NEIGHBORS) {
					if (world.tile_at(tx + step.x, ty + step.y) != 0) { continue; }
					stamp(baked, tx + 0.5 + step.x * 0.55, ty + 0.5 + step.y * 0.55, channel, 2.4, 0.75);
				}
			}
		}
	}

	// light from (lx, ly) over the open tiles a walk of the radius reaches; a
	// texel keeps the brighter of what it had and what this light gives it
	void stamp(array<int>& into, float lx, float ly, int channel, float radius, float power) {
		auto& world = G.world;
		int mw = world.mw;
		int sx = lx;
		int sy = ly;
		if (world.tile_at(sx, sy) != 0) { return; }
		stamp_id = stamp_id + 1;
		int steps = math::itrunc(radius) + 1;
		queue.clear();
		queue.push(sy * mw + sx);
		seen[sy * mw + sx] = stamp_id;
		int head = 0;
		float inv_r = 1.0 / radius;
		int base = 1 + channel * LIGHT_LEVELS;
		while (head < queue.size()) {
			int tile = queue[head];
			head = head + 1;
			int tx = tile % mw;
			int ty = tile / mw;
			for (int sv = 0; sv < LIGHT_SUB; ++sv) {
				float wy = ty + (sv + 0.5) / LIGHT_SUB - ly;
				int row = (ty * LIGHT_SUB + sv) * gw + tx * LIGHT_SUB;
				for (int su = 0; su < LIGHT_SUB; ++su) {
					float wx = tx + (su + 0.5) / LIGHT_SUB - lx;
					float d = math::sqrt(wx * wx + wy * wy) * inv_r;
					if (d >= 1.0) { continue; }
					float f = power * (1.0 - d) * (1.0 - d * 0.5);
					int level = math::itrunc(f * LIGHT_LEVELS + BAYER4[((ty * LIGHT_SUB + sv) & 3) * 4 + ((tx * LIGHT_SUB + su) & 3)]);
					if (level <= 0) { continue; }
					if (level > LIGHT_LEVELS) { level = LIGHT_LEVELS; }
					int i = row + su;
					if (level >= light_level(into[i])) { into[i] = base + level - 1; }
				}
			}
			if (iabs(tx - sx) + iabs(ty - sy) >= steps) { continue; }
			for (auto& step : NEIGHBORS) {
				int nx = tx + step.x;
				int ny = ty + step.y;
				if (world.tile_at(nx, ny) != 0) { continue; }
				int next = ny * mw + nx;
				if (seen[next] == stamp_id) { continue; }
				seen[next] = stamp_id;
				queue.push(next);
			}
		}
	}
}

// a light for one frame
class LightSource {
public:
	float x = 0.0;
	float y = 0.0;
	int channel = 0;
	float radius = 1.0;
	float power = 1.0;

	LightSource(float x_, float y_, int channel_, float radius_, float power_) {
		x = x_;
		y = y_;
		channel = channel_;
		radius = radius_;
		power = power_;
	}
}
