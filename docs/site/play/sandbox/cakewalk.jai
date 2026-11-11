// CAKEWALK: a gator in a dark cave. Eat every cake to go deeper; a bat costs a heart.
// Arrows or WASD walk, R digs a new cave, esc quits. Until a key is pressed the gator walks itself.
// The game runs live: change a setting while it plays and the running game takes the new value.
using math::ivec2;

// ---- settings that apply at once
float FRAMES_PER_SECOND = 30.0;
float GATOR_TILES_PER_SECOND = 7.5;
float BAT_TILES_PER_SECOND = 3.75;
int BAT_SMELL_RANGE = 10;                 // steps of walking within which a bat hunts the gator
float TORCH_REACH = 8.0;                  // tiles
array<int> ROCK_COLOR = [70, 80, 120];
array<int> FLOOR_COLOR = [200, 130, 60];
array<int> GATOR_COLOR = [70, 200, 90];
array<int> CAKE_COLOR = [255, 150, 200];
array<int> BAT_COLOR = [230, 120, 255];

// ---- settings that shape the next cave (R digs one now)
int CAVE_WIDTH = 48;
int CAVE_HEIGHT = 16;
int OPEN_PERCENT = 45;                    // how much of the rock a new cave digs out
int STARTING_HEARTS = 3;
int FIRST_CAVE_CAKES = 6;
int FIRST_CAVE_BATS = 2;

string ESC = "\x1b";
array<ivec2> NEIGHBORS = [ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1)];
map<string, ivec2> KEY_STEPS = {
	"left": ivec2(-1, 0), "a": ivec2(-1, 0), "right": ivec2(1, 0), "d": ivec2(1, 0),
	"up": ivec2(0, -1), "w": ivec2(0, -1), "down": ivec2(0, 1), "s": ivec2(0, 1)
};
enum Tile { Rock, Floor, Cake }

// How many steps of walking separate each tile from the nearest goal; -1 where no walk leads
class Trail {
	Trail(array<int> a_steps = [], int a_width = 0) {
		steps = a_steps;
		width = a_width;
	}

	int stepsFrom(ivec2 a_at) { return steps[a_at.x + a_at.y * width]; }
	bool reaches(ivec2 a_at) { return stepsFrom(a_at) >= 0; }

	array<ivec2> tilesBeyond(int a_steps, array<ivec2> a_tiles) {
		return array::filter(a_tiles, at => stepsFrom(at) > a_steps);
	}

	ivec2 nextStep(ivec2 a_from) {
		for (ivec2 direction : NEIGHBORS) {
			if (reaches(a_from + direction) && stepsFrom(a_from + direction) < stepsFrom(a_from)) {
				return a_from + direction;
			}
		}
		return a_from;
	}

private:
	array<int> steps;
	int width = 0;
}

class Cave {
	int width = 0;
	int height = 0;
	ivec2 entrance;

	bool isRock(ivec2 a_at) { return tiles[indexOf(a_at)] == Tile::Rock; }
	bool hasCake(ivec2 a_at) { return tiles[indexOf(a_at)] == Tile::Cake; }
	void placeCake(ivec2 a_at) { tiles[indexOf(a_at)] = Tile::Cake; }
	void eatCake(ivec2 a_at) { tiles[indexOf(a_at)] = Tile::Floor; }

	array<ivec2> tilesOf(Tile a_kind) {
		array<ivec2> result;
		for (int i = 0; i < tiles.size(); ++i) {
			if (tiles[i] == a_kind) {
				result.push(ivec2(i % width, i / width));
			}
		}
		return result;
	}

	// Breadth-first search outward from the goals
	Trail trailTo(array<ivec2> a_goals) {
		array<int> steps;
		steps.assign(width * height, -1);
		for (ivec2 goal : a_goals) {
			steps[indexOf(goal)] = 0;
		}
		for (int next = 0; next < a_goals.size(); ++next) {
			for (ivec2 direction : NEIGHBORS) {
				ivec2 beside = a_goals[next] + direction;   // the rim is rock, so no neighbour leaves the cave
				if (!isRock(beside) && steps[indexOf(beside)] < 0) {
					steps[indexOf(beside)] = steps[indexOf(a_goals[next])] + 1;
					a_goals.push(beside);
				}
			}
		}
		return Trail(steps, width);
	}

	// A digger wanders from the middle of solid rock, carving floor, until enough of the cave is open
	void dig(random& a_dice) {
		width = math::min(CAVE_WIDTH, term_cols() / 2);   // a tile is two characters wide
		height = math::min(CAVE_HEIGHT, term_rows() - 2);
		tiles.assign(width * height, Tile::Rock);
		entrance = ivec2(width / 2, height / 2);
		ivec2 digger = entrance;
		for (int toCarve = (width - 2) * (height - 2) * OPEN_PERCENT / 100; toCarve > 0; ) {
			if (isRock(digger)) {
				tiles[indexOf(digger)] = Tile::Floor;
				toCarve -= 1;
			}
			ivec2 next = digger + a_dice.pick(NEIGHBORS);
			bool insideTheRim = next.x > 0 && next.y > 0 && next.x < width - 1 && next.y < height - 1;
			digger = insideTheRim ? next : digger;
		}
	}

private:
	array<Tile> tiles;

	int indexOf(ivec2 a_at) { return a_at.x + a_at.y * width; }
}

class Gator {
	ivec2 position;
	bool facingLeft = false;
	int hearts = STARTING_HEARTS;
}

class Bat {
	ivec2 position;
	// A bat that smells the gator closes in; any other flutters about
	void fly(Cave& a_cave, Trail& a_toGator, random& a_dice) {
		bool smellsTheGator = a_toGator.reaches(position) && a_toGator.stepsFrom(position) <= BAT_SMELL_RANGE;
		ivec2 next = smellsTheGator ? a_toGator.nextStep(position) : position + a_dice.pick(NEIGHBORS);
		if (!a_cave.isRock(next)) {
			position = next;
		}
	}
}

class Painter {
	// The whole frame as one string: a tile's background is written only where it changes
	string paint(Cave& a_cave, Gator& a_gator, array<Bat>& a_bats) {
		string result = ESC + "[H";
		for (int y = 0; y < a_cave.height; ++y) {
			string lastBackground = "";
			for (int x = 0; x < a_cave.width; ++x) {
				ivec2 at = ivec2(x, y);
				float light = math::max(0.0, 1.0 - math::length(math::vec2(at - a_gator.position)) / TORCH_REACH);
				float checker = (x + y) % 2 == 0 ? 1.0 : 0.88;
				string background = color(48, a_cave.isRock(at) ? ROCK_COLOR : FLOOR_COLOR, light * checker);
				string tile = "  ";
				if (at == a_gator.position) {
					background = color(48, GATOR_COLOR, 1.0);
					tile = color(38, GATOR_COLOR, 0.2) + (a_gator.facingLeft ? "<:" : ":>");
				} else if (light > 0.0 && a_bats.some(bat => bat.position == at)) {
					tile = color(38, BAT_COLOR, 1.0) + "^^";
				} else if (a_cave.hasCake(at)) {
					tile = color(38, CAKE_COLOR, 1.0) + "◢◣";   // a cake glows, so it shows in the dark
				}
				if (background != lastBackground) {
					lastBackground = background;
					result += background;
				}
				result += tile;
			}
			result += ESC + "[0m\n";
		}
		return result;
	}

private:
	// The escape code that sets the text color (layer 38) or the background (layer 48)
	string color(int a_layer, array<int> a_rgb, float a_brightness) {
		array<int> lit = array::map(a_rgb, channel => math::itrunc(channel * a_brightness));
		return `${ESC}[${a_layer};2;${lit[0]};${lit[1]};${lit[2]}m`;
	}
}

class Game {
	Game() { digCave(); }

	bool play() {
		readKeys();
		gatorSteps += GATOR_TILES_PER_SECOND / FRAMES_PER_SECOND;
		batSteps += BAT_TILES_PER_SECOND / FRAMES_PER_SECOND;
		for (; gatorSteps >= 1.0; gatorSteps -= 1.0) {
			moveGator();
		}
		for (; batSteps >= 1.0; batSteps -= 1.0) {
			for (auto& bat : bats) {
				bat.fly(cave, toGator, dice);
			}
		}
		sufferBites();
		if (gator.hearts <= 0) {
			gator = Gator();
			level = 1;
			digCave();
		} else if (cakesLeft == 0) {
			level += 1;
			digCave();
		}
		print(painter.paint(cave, gator, bats) + statusLine() + ESC + "[K", skip_newline);
		sleep(1.0 / FRAMES_PER_SECOND);
		if (!running) {
			print(`${ESC}[0m${ESC}[?25h\nThe gator reached cave ${level}.`);
		}
		return running;
	}

private:
	Cave cave;
	Gator gator;
	array<Bat> bats;
	Painter painter;
	Trail toGator;
	Trail toCake;
	float gatorSteps = 0.0;        // the steps each is owed: its speed adds a fraction of one every frame
	float batSteps = 0.0;
	random dice = random(2026);
	ivec2 heading;                 // the step the keys ask for; (0, 0) stands still
	int level = 1;
	int cakesLeft = 0;
	bool selfPlaying = true;
	bool running = true;

	void digCave() {
		cave.dig(dice);
		gator.position = cave.entrance;
		toGator = cave.trailTo([gator.position]);
		array<ivec2> farFromTheGator = toGator.tilesBeyond(8, cave.tilesOf(Tile::Floor));
		dice.shuffle(farFromTheGator);
		cakesLeft = math::min(FIRST_CAVE_CAKES + level - 1, farFromTheGator.size());
		for (int i = 0; i < cakesLeft; ++i) {
			cave.placeCake(farFromTheGator[i]);
		}
		bats.clear();
		for (int i = cakesLeft; i < cakesLeft + FIRST_CAVE_BATS + level - 1 && i < farFromTheGator.size(); ++i) {
			bats.push(Bat{farFromTheGator[i]});
		}
		toCake = cave.trailTo(cave.tilesOf(Tile::Cake));
		print(ESC + "[?25l" + ESC + "[2J", skip_newline);
	}

	void readKeys() {
		for (string key = read_key(); key != ""; key = read_key()) {
			selfPlaying = false;
			if (key == "esc") {
				running = false;
			} else if (key == "r") {
				digCave();
			} else if (KEY_STEPS.has(key)) {
				heading = KEY_STEPS[key];   // a tap between two steps still walks a tile
			}
		}
	}

	void moveGator() {
		for (auto [key, step] : KEY_STEPS) {
			if (key_down(key) && !cave.isRock(gator.position + step)) {
				heading = step;
			}
		}
		ivec2 to = selfPlaying ? toCake.nextStep(gator.position) : gator.position + heading;
		heading = ivec2(0, 0);
		if (to == gator.position || cave.isRock(to)) {
			return;
		}
		gator.facingLeft = to.x < gator.position.x || (gator.facingLeft && to.x == gator.position.x);
		gator.position = to;
		toGator = cave.trailTo([to]);
		if (cave.hasCake(to)) {
			cave.eatCake(to);
			cakesLeft -= 1;
			toCake = cave.trailTo(cave.tilesOf(Tile::Cake));
		}
	}

	void sufferBites() {
		for (auto& bat : bats) {
			if (bat.position == gator.position) {
				gator.hearts -= 1;
				bat.position = dice.pick(cave.tilesOf(Tile::Floor));
			}
		}
	}

	string statusLine() {
		string result = `CAKEWALK  cave ${level}  cakes left ${cakesLeft}  ${string::repeat("♥", gator.hearts)}`;
		return result + (selfPlaying ? "   press a key to take over from the gator" : "   arrows or WASD walk, R digs, esc quits");
	}
}

// Declared without an initializer, so a reload keeps the running game while everything above it takes its new text
Game game;
bool frame() { return game.play(); }
