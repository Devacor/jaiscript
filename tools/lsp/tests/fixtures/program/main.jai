import "lib.jai";

int total(array<int> xs) {
	int sum = 0;
	for (auto x : xs) {
		sum += x;
	}
	return sum;
}

void run() {
	Hero h = Hero("gator", 10);
	h.heal(5);
	int hp = h.hp;
	int shade = Color.green;
	string s = util::shout(h.name);
	auto party = make_party();
	party[0].heal(util::twice(1));
	Actor& a = party[0];
	print(s + to_string(total([1, 2, 3]) + hp + shade + LIMIT + a.power()));
	var dyn = h;
	dyn.level = 3;
}

run();
