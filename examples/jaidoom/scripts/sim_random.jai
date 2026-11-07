// JaiDOOM sim_random.jai — the sim's one random stream, owned here: MT19937-64
// (the generator C++ names std::mt19937_64) under the mappings every recorded
// demo was made with. An int range is lo + word mod span (unsigned), a unit
// float is the word's top 53 bits over 2^53 - 1, both ends inclusive. demo.jai
// seeds it at both ends of a recording, so a demo replays the draws it was
// recorded with; the HUD face rolls its own LCG and never touches it.
//
// The words are unsigned 64-bit values in int slots: products and sums wrap
// through math::wrap_*, and a right shift masks off what sign extension adds.

int MT_WORDS = 312;
int MT_SHIFT = 156;
int MT_UPPER = -2147483648;             // 0xFFFFFFFF80000000
int MT_LOWER = 2147483647;              // 0x000000007FFFFFFF
int MT_MATRIX = -5403634167711393303;   // 0xB5026F5AA96619E9
int MT_SIGN = -9223372036854775807 - 1;

class MersenneTwister64 {
	array<int> words;
	int index = 312;

	MersenneTwister64(int seed_value) { seed(seed_value); }

	void seed(int seed_value) {
		words = [seed_value];
		for (int i = 1; i < MT_WORDS; ++i) {
			int prev = words[i - 1];
			words.push(math::wrap_add(math::wrap_mul(6364136223846793005, prev ^ ((prev >> 62) & 3)), i));
		}
		index = MT_WORDS;
	}

	int mixed(int upper_from, int lower_from, int far) {
		int joined = (upper_from & MT_UPPER) | (lower_from & MT_LOWER);
		return far ^ ((joined >> 1) & 9223372036854775807) ^ ((joined & 1) != 0 ? MT_MATRIX : 0);
	}

	void twist() {
		int i = 0;
		for (; i < MT_WORDS - MT_SHIFT; ++i) { words[i] = mixed(words[i], words[i + 1], words[i + MT_SHIFT]); }
		for (; i < MT_WORDS - 1; ++i) { words[i] = mixed(words[i], words[i + 1], words[i + MT_SHIFT - MT_WORDS]); }
		words[MT_WORDS - 1] = mixed(words[MT_WORDS - 1], words[0], words[MT_SHIFT - 1]);
		index = 0;
	}

	int next_word() {
		if (index >= MT_WORDS) { twist(); }
		int x = words[index];
		++index;
		x = x ^ ((x >> 29) & 22906492245);              // 0x5555555555555555 over the 35 bits a logical >> 29 keeps
		x = x ^ ((x << 17) & 8202884508482404352);      // 0x71D67FFFEDA60000
		x = x ^ ((x << 37) & -2270628950310912);        // 0xFFF7EEE000000000
		return x ^ ((x >> 43) & 2097151);
	}

	// a mod b for unsigned a and b != 0
	int unsigned_mod(int a, int b) {
		if (b < 0) { return (a ^ MT_SIGN) >= (b ^ MT_SIGN) ? math::wrap_sub(a, b) : a; }
		if (a >= 0) { return a % b; }
		int rest = ((a >> 1) & 9223372036854775807) % b;
		int doubled = rest >= b - rest ? rest - (b - rest) : rest + rest;
		if ((a & 1) == 0) { return doubled; }
		return doubled == b - 1 ? 0 : doubled + 1;
	}

	// [lo, hi], both inclusive
	int between(int lo, int hi) {
		if (hi < lo) { throw "MersenneTwister64.between: range is inverted (max < min)"; }
		int span = math::wrap_add(math::wrap_sub(hi, lo), 1);
		int word = next_word();
		return math::wrap_add(lo, span == 0 ? word : unsigned_mod(word, span));
	}

	// [0, 1], both inclusive
	float unit() { return ((next_word() >> 11) & 9007199254740991) * (1.0 / 9007199254740991.0); }
}

MersenneTwister64 SIM_RANDOM = MersenneTwister64(-7046029254386353131);   // 0x9E3779B97F4A7C15, the stream's first seed
