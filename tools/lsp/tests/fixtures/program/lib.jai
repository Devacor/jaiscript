// The library half of the fixture program: a class hierarchy, an enum, a namespace, globals.

enum Color { red, green, blue }

int LIMIT = 100;

class Actor {
	string name = "";
	int hp = 0;

	void heal(int amount) { hp = hp + amount; }
	int power() { return hp; }
}

class Hero : Actor {
	int level = 1;

	Hero(string n, int h) {
		name = n;
		hp = h;
	}

	void heal(int amount) override { hp = hp + amount * 2; }
}

namespace util {
	string shout(string s) { s.to_upper(); return s; }
	// Doubles a value.
	int twice(int v) { return v * 2; }
}

array<Hero> make_party() {
	array<Hero> out;
	out.push(Hero("a", 1));
	return out;
}
