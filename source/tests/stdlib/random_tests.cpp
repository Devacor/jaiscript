#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <string>

namespace jai::foundry::tests {

namespace random_test_types {
	struct snapshot {
		int64_t tick = 0;
		jai::stdlib::random generator {1};
		template<typename Archive>
		void serialize(Archive& a_archive) {
			a_archive("tick", tick);
			a_archive("generator", generator);
		}
	};
}

class random_tests : public suite {
public:
	random_tests() : suite("Random Type") {}

	void forge_tests() override {
		auto es = [this](const std::string& a_source) { auto engine = make_engine(); stdlib::register_all(*engine); return engine->execute(a_source).as<std::string>(); };
		auto caught = [es](const std::string& a_source) { return es("string m = \"\"; try { " + a_source + " } catch (e) { m = e; } m;"); };

		// Golden values from an independent implementation of the specified algorithms (xoshiro256** seeded by splitmix64,
		// Lemire's unbounded-free ranges, the top 53 bits as a float): any drift on any platform, compiler or backend fails here
		test("a_seed_draws_the_same_values_everywhere", [this, es]() {
			check_eq(std::string("bdd732262feb6e9528efe333b266f10347526757130f9f52581ce1ff0e4ae394|1,3,5,6,6,5,5,6,5,4,5,2|0.8010242975288078 0.3214116333153503 0.7111499449118543|"
			                     "1.8888361481067484 0.5838077129492963|true true true false false false|23,116,129,88,84,162,29,154,240,122,156|0,5,8,7,2,1,9,6,3,4|ef|"
			                     "4556341079545628172|33411ec09134a89a4cb397f1d6c8bf43f5a2aebc4cb4c40e3be15a6cc24ed93b"), es(R"(
				random r(42);
				string out = `${r.state}|`;
				array<int> ints = [];
				for (int i = 0; i < 12; ++i) { ints.push(r.next_int(1, 6)); }
				out += `${ints.join(",")}|`;
				out += `${to_string(r.next_float())} ${r.next_float()} ${r.next_float()}|`;
				out += `${to_string(r.next_float(-2.5, 2.5))} ${r.next_float(-2.5, 2.5)}|`;
				out += `${to_string(r.next_bool())} ${r.next_bool()} ${r.next_bool()} ${r.next_bool()} ${r.next_bool()} ${r.next_bool()}|`;
				out += `${r.next_bytes(11).to_bytes().join(",")}|`;
				array<int> xs = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
				r.shuffle(xs);
				out += `${xs.join(",")}|${r.pick(["ab", "cd", "ef"])}|`;
				out += `${to_string(r.next_int(-9223372036854775807 - 1, 9223372036854775807))}|${r.state}`;
				out;)"));
			check_eq(std::string("e220a8397b1dcdaf6e789e6aa1b965f406c45d188009454ff88bb8a8724c81ec 559892704"), es("random z(0); random n(-1); `${z.state} ${n.next_int(0, 1000000000)}`;"));
		});

		test("the_three_declarations_construct_and_the_default_is_seeded_by_the_system", [this, es]() {
			check_eq(std::string("true true random random 64 true"), es(R"(
				random a = random(9);
				random b(9);
				random c;
				random d;
				`${to_string(a.state == b.state)} ${(a.next_int(0, 1000000) == b.next_int(0, 1000000))} ${type_of(c)} ${type_of(random())} ${c.state.size()} ${(c.state != d.state)}`;)"));
			check_eq(std::string("true"), es("random r(5); r.next_int(0, 9); r.seed(5); random fresh(5); to_string(r.state == fresh.state);"));
		});

		test("a_copy_replays_the_same_draws", [this, es]() {
			check_eq(std::string("true true true true true true"), es(R"(
				random a(77);
				a.next_int(0, 10);
				random b = a;
				var c = a;
				auto d = a;
				function draw(random g) -> int { return g.next_int(0, 1000000); }
				int by_value = draw(a);
				map<string, random> keyed;
				keyed["k"] = a;
				array<random> pushed = [];
				pushed.push(a);
				int x = a.next_int(0, 1000000);
				`${to_string(b.next_int(0, 1000000) == x)} ${(c.next_int(0, 1000000) == x)} ${(d.next_int(0, 1000000) == x)} ${(by_value == x)} ${(keyed["k"].next_int(0, 1000000) == x)} ${(pushed[0].next_int(0, 1000000) == x)}`;)"));
		});

		test("the_state_round_trips_through_every_archive", [this, es]() {
			check_eq(std::string("{\"$type\":\"random\",\"$val\":{\"state\":\"bdd732262feb6e9528efe333b266f10347526757130f9f52581ce1ff0e4ae394\"}}"), es("to_json(random(42));"));
			check_eq(std::string("random true true true"), es(R"(
				random a(1234);
				a.next_float();
				var json = from_json(to_json(a));
				var binary = from_base64(to_base64(a));
				random copied(1);
				copied.state = a.state;
				int x = a.next_int(0, 1000000000);
				`${type_of(json)} ${(json.next_int(0, 1000000000) == x)} ${(binary.next_int(0, 1000000000) == x)} ${(copied.next_int(0, 1000000000) == x)}`;)"));
			check_eq(std::string("random::state expects 64 hex digits, not all zero"), es("string m = \"\"; random r(1); try { r.state = \"zz\"; } catch (e) { m = e; } m;"));
			check_eq(std::string("random::state expects 64 hex digits, not all zero"), es("string m = \"\"; random r(1); try { r.state = string::repeat(\"0\", 64); } catch (e) { m = e; } m;"));
			auto engine = make_engine();
			stdlib::register_all(*engine);
			random_test_types::snapshot saved;
			saved.tick = 9;
			saved.generator.next();
			const std::string json = jai::to_json(*engine, saved, 0);
			random_test_types::snapshot restored = jai::from_json<random_test_types::snapshot>(*engine, json);
			check_eq(saved.generator.state(), restored.generator.state());
			check_eq(saved.generator.next(), restored.generator.next());
			random_test_types::snapshot binary = jai::from_binary<random_test_types::snapshot>(*engine, jai::to_binary(*engine, saved));
			check_eq(saved.generator.next(), binary.generator.next());
		});

		test("refused_draws_raise_their_catalogued_texts", [this, caught, es]() {
			check_eq(std::string("random::next_int range is inverted (max < min)"), caught("random r(1); r.next_int(5, 1);"));
			check_eq(std::string("random::next_float range is inverted (max < min)"), caught("random r(1); r.next_float(2.0, 1.0);"));
			check_eq(std::string("random::next_float bounds must be finite"), caught("random r(1); r.next_float(0.0, math::INF);"));
			check_eq(std::string("true"), caught("random r(1); float x = r.next_float(-1.7e308, 1.7e308); throw to_string(x >= -1.7e308 && x < 1.7e308);"),
			         "finite bounds a float cannot span draw from the whole range");
			check_eq(std::string("Out of memory"), caught("random r(1); r.next_bytes(9223372036854775807);"));
			check_eq(std::string("Out of memory"), caught("crypto::random_bytes(9223372036854775807);"));
			check_eq(std::string("Out of memory"), caught("var s = \"ab\"; s.pad_left(9223372036854775807);"));
			check_eq(std::string("random::pick expects a non-empty array"), caught("random r(1); r.pick([]);"));
			check_eq(std::string("random::next_bytes count cannot be negative"), caught("random r(1); r.next_bytes(-1);"));
			check_eq(std::string("random::shuffle expects an array"), caught("random r(1); var s = \"text\"; r.shuffle(s);"));
			std::string refused;
			try { es("var f = math::random;"); } catch (const jai::static_check_error& e) { refused = e.what(); }
			check_true(refused.find("error: no member 'random' in namespace 'math'") != std::string::npos, refused);
		});

		test("the_checker_holds_constructions_and_draws_to_their_signatures", [this]() {
			auto engine = make_engine();
			stdlib::register_all(*engine);
			for (const char* source : {"random a(\"x\");", "random b(1, 2);", "random c = random(1, 2);", "random d(1); d.next_int(1);", "random e(1); string s = e.next_float(1.0);",
			                           "random f(1); f.nope();", "random g(1); g.seed(\"x\");"}) {
				bool rejected = false;
				try { engine->execute(source); } catch (const std::exception&) { rejected = true; }
				check_true(rejected, source);
			}
			check_eq(std::string("random"), engine->execute("random h(2.5); type_of(h);").as<std::string>());
		});

		test("shuffle_and_pick_work_on_every_array_kind", [this, es]() {
			check_eq(std::string("10 55 | 3 | 4 | 3 | true"), es(R"(
				random r(3);
				array<int> ints = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
				r.shuffle(ints);
				int total = 0;
				for (int v : ints) { total += v; }
				array<string> words = ["a", "b", "c"];
				r.shuffle(words);
				array<math::vec2> points = [math::vec2(1, 2), math::vec2(3, 4), math::vec2(5, 6), math::vec2(7, 8)];
				r.shuffle(points);
				var mixed = [1, "two", 3.0];
				r.shuffle(mixed);
				`${to_string(ints.size())} ${total} | ${words.size()} | ${points.size()} | ${mixed.size()} | ${(words.join("").size() == 3)}`;)"));
		});

		test("each_element_draws_its_own_stream_from_a_captured_random_at_every_worker_count", [this]() {
			// The statement takes one draw from the captured generator; element i draws the stream random seeds from that draw and i
			// (values from the independent implementation): the same at every worker count, the generator moved one draw
			const std::string source = R"(
				random q(7);
				array<int> vs = [];
				for (int i = 0; i < 5000; ++i) { vs.push(0); }
				parallel_for (auto& v : vs) { v = q.next_int(1, 6) * 10 + q.next_int(1, 6); }
				int total = 0;
				int h = 0;
				for (int v : vs) { total += v; h = (h * 31 + v) % 1000000007; }
				array<random> streams = [random(1), random(2), random(3)];
				array<int> got = [0, 0, 0];
				parallel_for (auto& s, auto& g : streams, got) { g = s.next_int(1, 1000000); }
				array<int> made = [0, 0, 0, 0];
				parallel_for (auto& m : made) { random local(5); m = local.next_int(0, 99); }
				`${to_string(total)} ${h} ${q.next_int(0, 1000000000)} ${got.join(",")} ${made.join(",")}`;)";
			for (size_t workers : {size_t(1), size_t(4), size_t(8)}) {
				auto engine = make_engine();
				stdlib::register_all(*engine);
				engine->parallel_thread_count(workers);
				check_eq(std::string("196279 91495803 278751229 702922,102180,690639 28,28,28,28"), engine->execute(source).as<std::string>(), "workers " + std::to_string(workers));
			}
		});

		test("each_element_draws_from_its_own_random_field_like_a_serial_loop_at_every_worker_count", [this]() {
			const std::string source = R"(
				class Agent { int id = 0; random rng; int roll = 0; }
				array<Agent> agents = [];
				array<Agent> serial = [];
				for (int i = 0; i < 300; ++i) { Agent a; a.id = i; a.rng.seed(i * 7 + 1); agents.push(a); serial.push(a); }
				parallel_for (auto& a : agents) { a.roll = a.rng.next_int(1, 1000) * 1000 + a.rng.next_int(1, 1000); }
				for (auto& a : serial) { a.roll = a.rng.next_int(1, 1000) * 1000 + a.rng.next_int(1, 1000); }
				var nested = [[random(1)], [random(2)]];
				array<int> got = [0, 0];
				parallel_for (auto& g, auto& o : nested, got) { o = g[0].next_int(1, 1000000); }
				int same = 0;
				int h = 0;
				for (int i = 0; i < 300; ++i) {
					if (agents[i].roll == serial[i].roll && agents[i].rng.next_int(0, 99) == serial[i].rng.next_int(0, 99)) { ++same; }
					h = (h * 31 + agents[i].roll) % 1000000007;
				}
				`${to_string(same)} ${h} ${got.join(",")}`;)";
			for (size_t workers : {size_t(1), size_t(4), size_t(8)}) {
				auto engine = make_engine();
				stdlib::register_all(*engine);
				engine->parallel_thread_count(workers);
				check_eq(std::string("300 764724181 702922,102180"), engine->execute(source).as<std::string>(), "workers " + std::to_string(workers));
			}
		});

		test("two_elements_sharing_one_random_are_refused_naming_the_field_and_the_elements", [this, caught]() {
			auto engine = make_engine();
			stdlib::register_all(*engine);
			check_true(engine->execute("random r(3); var nest = [[r], [r]]; array<int> outs = [0, 0]; parallel_for (auto& g, auto& o : nest, outs) { o = g[0].next_int(1, 9); } outs[0] == outs[1];").as<bool>());
			auto* raw = engine.get();
			engine->add_function("share_generator", [raw](const script_value& a_from, const script_value& a_to) {
				const uint64_t id = detail::engine_access::symbolize(*raw, "rng");
				auto* from = static_cast<class_instance*>(a_from.get_object_holder()->object());
				auto* to = static_cast<class_instance*>(a_to.get_object_holder()->object());
				*to->find_field_value(id) = *from->find_field_value(id);
			});
			check_eq(std::string("parallel_for: elements 0 and 2 share one 'random' in field 'rng' (an element owns the C++ values it holds: give each element its own, or capture one from outside the loop)"),
			         engine->execute(R"(
				class Agent { random rng; int roll = 0; }
				var agents = [Agent(), Agent(), Agent()];
				share_generator(agents[0], agents[2]);
				string m = "";
				try { parallel_for (auto& a : agents) { a.roll = a.rng.next_int(1, 9); } } catch (e) { m = e; }
				m;)").as<std::string>());
		});

		test("a_captured_random_is_not_reseeded_or_shuffled_into_from_a_parallel_body", [this, caught]() {
			check_true(caught("random g(1); array<int> ys = [1, 2]; parallel_for (auto& y : ys) { g.seed(y); }")
			               .find("parallel_for: random::seed cannot be called on captured 'g' in a parallel body (seed before the loop: each element draws its own stream from it)") != std::string::npos);
			check_true(caught("random g(1); array<int> deck = [1, 2, 3]; array<int> ys = [1, 2]; parallel_for (auto& y : ys) { g.shuffle(deck); }")
			               .find("parallel_for: argument passes enclosing state 'deck' by reference (captured reads are read-only)") != std::string::npos);
			check_true(caught("class Fake { int n = 0; int next_int(int a, int b) { n = a; return a; } } Fake g; array<int> ys = [1, 2]; parallel_for (auto& y : ys) { y = g.next_int(1, 2); }")
			               .find("method 'next_int' of 'Fake' writes its object, and captured 'g' is read-only") != std::string::npos);
		});
	}
};

} // namespace jai::foundry::tests

using random_tests = jai::foundry::tests::random_tests;
FOUNDRY_REGISTER(random_tests)
