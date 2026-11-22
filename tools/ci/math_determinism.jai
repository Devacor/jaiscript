// The portable math check (docs/site/stability.html): every math:: function IEEE does not round exactly, and the
// rotations, angles, eased mixes and color blends built on them, over a fixed sweep; each result's 64 bits are written in
// order and hashed. Every platform, compiler and backend prints the same hash.
const string EXPECTED = "6b90815dec6934f660c04ec3abaabf4828411d9bdad26d62b5db7adefaf0354f";

int state = 20261119;
int next_bits() {
	state = math::wrap_add(math::wrap_mul(state, 6364136223846793005), 1442695040888963407);
	return state;
}
// uniform in [lo, hi) from the state's top 53 bits
float uniform(float lo, float hi) {
	float u = cast<float>((next_bits() >> 11) & 9007199254740991) / 9007199254740992.0;
	return lo + (hi - lo) * u;
}
// any 64 bits as a float: infinities, NaNs and subnormals included
float any_float() {
	binary::writer w;
	w.int64(next_bits());
	return w.to_string().reader().float64();
}
// a random sign times 2^e, e uniform in [lo, hi)
float scattered(float lo, float hi) {
	float m = math::exp2(uniform(lo, hi));
	return next_bits() < 0 ? -m : m;
}

binary::writer out;
void put(float v) { out.float64(v); }

for (int i = 0; i < 300; ++i) {
	float a = any_float();
	float b = any_float();
	float t = uniform(-10.0, 10.0);
	float w = scattered(-40.0, 1023.0);
	float u = uniform(-1.0, 1.0);
	float p = uniform(0.0, 4.0);
	float q = uniform(-60.0, 60.0);
	put(math::sin(a)); put(math::sin(t)); put(math::sin(w));
	put(math::cos(a)); put(math::cos(t)); put(math::cos(w));
	put(math::tan(a)); put(math::tan(t)); put(math::tan(w));
	put(math::asin(a)); put(math::asin(u));
	put(math::acos(a)); put(math::acos(u));
	put(math::atan(a)); put(math::atan(t)); put(math::atan(w));
	put(math::atan2(a, b)); put(math::atan2(t, q)); put(math::atan2(w, t));
	put(math::sinh(a)); put(math::sinh(t)); put(math::sinh(q));
	put(math::cosh(a)); put(math::cosh(t)); put(math::cosh(q));
	put(math::tanh(a)); put(math::tanh(t)); put(math::tanh(u));
	put(math::exp(a)); put(math::exp(q)); put(math::exp(uniform(-745.0, 709.0)));
	put(math::exp2(a)); put(math::exp2(q)); put(math::exp2(uniform(-1075.0, 1024.0)));
	put(math::log(a)); put(math::log(math::abs(w))); put(math::log(p));
	put(math::log2(a)); put(math::log2(math::abs(w))); put(math::log2(p));
	put(math::log10(a)); put(math::log10(math::abs(w))); put(math::log10(p));
	put(math::pow(a, b)); put(math::pow(p, q)); put(math::pow(-p, math::round(q))); put(math::pow(math::abs(w), u));
	put(math::cbrt(a)); put(math::cbrt(w)); put(math::cbrt(q));
	put(math::hypot(a, b)); put(math::hypot(t, q)); put(math::hypot(w, t));
}

for (int i = 0; i < 100; ++i) {
	float t = uniform(-10.0, 10.0);
	math::vec2 f = math::from_angle(t, 2.5);
	math::vec2 r = math::rotate(math::vec2(uniform(-5.0, 5.0), uniform(-5.0, 5.0)), t);
	math::vec2 around = math::rotate_around(math::vec2(uniform(-5.0, 5.0), 1), math::vec2(0.5, uniform(-1.0, 1.0)), t);
	math::vec3 turned = math::rotate(math::vec3(1, 2, 3), math::vec3(uniform(-1.0, 1.0), uniform(-1.0, 1.0), 1), t);
	put(f.x); put(f.y); put(r.x); put(r.y); put(around.x); put(around.y); put(turned.x); put(turned.y); put(turned.z);
	put(math::angle(math::vec2(uniform(-3.0, 3.0), uniform(-3.0, 3.0))));
	put(math::angle_between(math::vec3(uniform(-3.0, 3.0), 1, 2), math::vec3(1, uniform(-3.0, 3.0), -2)));
	float s = uniform(0.0, 1.0);
	float k = uniform(0.5, 4.0);
	put(math::mix_in(0.0, 10.0, s, k)); put(math::mix_out(0.0, 10.0, s, k));
	put(math::mix_in_out(-2.0, 3.0, s, k)); put(math::mix_out_in(-2.0, 3.0, s, k));
	put(math::unmix_in(0.0, 10.0, s * 10.0, k)); put(math::unmix_out(0.0, 10.0, s * 10.0, k));
	put(math::unmix_in_out(-2.0, 3.0, s * 5.0 - 2.0, k)); put(math::unmix_out_in(-2.0, 3.0, s * 5.0 - 2.0, k));
	math::color c = math::mix(math::color(uniform(0.0, 1.0), uniform(0.0, 1.0), uniform(0.0, 1.0)),
	                          math::color(uniform(0.0, 1.0), uniform(0.0, 1.0), uniform(0.0, 1.0), 0.5), s);
	math::color e = math::mix_in(math::color(0.1, 0.8, 0.3), math::color(0.9, 0.2, 0.6), s, k);
	put(c.r); put(c.g); put(c.b); put(c.a); put(e.r); put(e.g); put(e.b);
}

string hash = crypto::sha256(out.to_string());
print(hash == EXPECTED ? "portable math: PASS" : "portable math: FAIL " + hash);
