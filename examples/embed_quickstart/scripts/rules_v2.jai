// The edited rules module: new numbers, a new Tuning field. Live Tuning instances migrate in
// place (crit keeps its runtime value, bonus takes its initializer), hits_dealt keeps counting.

int hits_dealt;

class Tuning {
	int crit = 25;
	int bonus = 3;
}

int rules_version() { return 2; }

int damage_for(int frame) {
	hits_dealt += 1;
	return 5 + frame % 2;
}
