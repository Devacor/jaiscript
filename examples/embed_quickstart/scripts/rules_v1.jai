// The live rules module. The host copies this file to live/rules.jai, then overwrites it with
// rules_v2.jai while the game runs; the next frame that reaches `import` re-runs it.

int hits_dealt;                 // no initializer: a re-run keeps the value it holds

class Tuning {
	int crit = 10;
}

int rules_version() { return 1; }

int damage_for(int frame) {
	hits_dealt += 1;
	return 2;
}
