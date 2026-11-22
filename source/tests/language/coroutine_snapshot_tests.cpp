#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/core/coroutine.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class coroutine_snapshot_tests : public suite {
public:
	coroutine_snapshot_tests() : suite("Coroutine Snapshot Tests") {}

	// 0 the interpreter, 1 the VM, 2 the VM in its reference mode
	static std::shared_ptr<engine> backend_engine(int a_backend) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(a_backend == 0 ? jai::backend_type::interpreter : jai::backend_type::vm);
		if (a_backend == 2) { e->vm_reference_mode(true); }
		e->execution_budget(0);
		jai::stdlib::register_all(e);
		return e;
	}

	static std::string run(engine& a_engine, const std::string& a_source) {
		try { return a_engine.execute(a_source).to_string(); }
		catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
	}

	// The program's last value as text on each backend (interpreter, vm), an exception as "ERROR: what"
	static std::pair<std::string, std::string> both_backends(const std::string& a_source) {
		auto interpreted = backend_engine(0);
		auto compiled = backend_engine(1);
		return {run(*interpreted, a_source), run(*compiled, a_source)};
	}

	void check_both(const std::string& a_source, const std::string& a_expected, const std::string& a_what) {
		auto [interpreted, compiled] = both_backends(a_source);
		check_eq(a_expected, interpreted, "interp: " + a_what);
		check_eq(a_expected, compiled, "vm: " + a_what);
	}

	// A program that declares make(), a function returning a fresh coroutine: steps_of drains one into "v,v,=result,"
	static constexpr const char* k_driver = R"(
		function steps_of(h) {
			string out = "";
			while (!h.done()) {
				var v = h.next();
				out += `${h.done() ? "=" : ""}${v},`;
			}
			return out;
		}
		function at_step(int k) { var h = make(); for (int i = 0; i < k; ++i) { h.next(); } return h; }
		function saved_binary(int k) { return base64_encode(to_binary(at_step(k))); }
		function saved_json(int k) { return base64_encode(to_json(at_step(k))); }
		function rest_binary(string s) { return steps_of(from_binary(base64_decode(s))); }
		function rest_json(string s) { return steps_of(from_json(base64_decode(s))); }
	)";

	static std::vector<std::string> steps(const std::string& a_drained) {
		std::vector<std::string> result;
		size_t from = 0;
		for (size_t at = a_drained.find(','); at != std::string::npos; at = a_drained.find(',', from)) {
			result.push_back(a_drained.substr(from, at - from));
			from = at + 1;
		}
		return result;
	}

	// A save after every number of steps, loaded on each backend whichever backend saved it, leaves what an uninterrupted run
	// leaves, and the save does not disturb the coroutine it was taken of; the backends write the same bytes and the same text
	void check_round_trips(const std::string& a_name, const std::string& a_program, const std::string& a_expected = {}) {
		constexpr int k_backends = 3;
		std::shared_ptr<engine> engines[k_backends] = {backend_engine(0), backend_engine(1), backend_engine(2)};
		const char* names[k_backends] = {"interp", "vm", "vm-reference"};
		std::string full[k_backends];
		for (int b = 0; b < k_backends; ++b) {
			const std::string loaded = run(*engines[b], a_program + k_driver + "\n0");
			check_eq(std::string("0"), loaded, a_name + ": " + names[b] + " loads");
			full[b] = run(*engines[b], "steps_of(make())");
		}
		check_eq(full[0], full[1], a_name + ": the backends agree on the uninterrupted run");
		check_eq(full[0], full[2], a_name + ": the reference VM agrees on the uninterrupted run");
		if (!a_expected.empty()) { check_eq(a_expected, full[1], a_name + ": the uninterrupted run"); }
		const std::vector<std::string> all = steps(full[1]);
		for (size_t k = 0; k <= all.size(); ++k) {
			std::string rest;
			for (size_t i = k; i < all.size(); ++i) { rest += all[i] + ","; }
			const std::string at = a_name + " after " + std::to_string(k) + " steps: ";
			std::string binary[k_backends];
			std::string json[k_backends];
			for (int b = 0; b < k_backends; ++b) {
				binary[b] = run(*engines[b], "saved_binary(" + std::to_string(k) + ")");
				json[b] = run(*engines[b], "saved_json(" + std::to_string(k) + ")");
			}
			for (int b = 1; b < k_backends; ++b) {
				check_eq(binary[0], binary[b], at + names[b] + " writes the interpreter's bytes");
				check_eq(json[0], json[b], at + names[b] + " writes the interpreter's text");
			}
			for (int saver = 0; saver < k_backends; ++saver) {
				for (int loader = 0; loader < k_backends; ++loader) {
					const std::string route = std::string(names[saver]) + " -> " + names[loader];
					check_eq(rest, run(*engines[loader], "rest_binary(\"" + binary[saver] + "\")"), at + route + " binary");
					check_eq(rest, run(*engines[loader], "rest_json(\"" + json[saver] + "\")"), at + route + " json");
				}
			}
		}
	}

	void forge_tests() override {
		test("a_paused_coroutine_saves_and_loads_at_every_yield_statement", [this]() {
			const std::pair<const char*, const char*> cases[] = {
				{"straight line and typed locals", R"(
					enum Mood { calm, angry }
					class Vec { int x = 0; int y = 0; Vec() {} Vec(int a, int b) { x = a; y = b; } }
					coroutine var body(int from, string label) {
						yield from;
						int x = from * 2;
						float f = 1.5;
						string s = label + "!";
						bool b = true;
						Mood mood = Mood::angry;
						Vec v(3, 4);
						var any = null;
						yield x;
						x += 1; f *= 2.0; any = [1, "two"];
						yield `${x} ${f} ${s} ${b} ${mood} ${v.x + v.y} ${any.size()}`;
						array<int> typed = [1, 2];
						map<string, int> counts = {"a": 1};
						auto inferred = typed;
						typed.push(3); counts["b"] = 2;
						yield `${typed.size()} ${counts.size()} ${inferred.size()} ${reflect::type_of(typed)} ${reflect::type_of(inferred)}`;
						return x;
					}
					function make() { return body(5, "hi"); }
				)"},
				{"branches and loops of every kind", R"(
					coroutine var body(int n) {
						for (int i = 0; i < n; ++i) {
							if (i % 2 == 0) { yield `e${i}`; } else { yield `o${i}`; }
							int j = 0;
							while (j < 2) { yield i * 10 + j; ++j; }
						}
						int k = 0;
						do { yield `d${k}`; ++k; } while (k < 2);
						for (int a = 5; a > 0; a -= 2) { for (int b = 0; b < 2; b++) { yield a * 100 + b; } }
						for (int i = 0; i < n; ++i) yield i;
						int w = 0;
						while (w < 2) yield w++;
						if (n > 1) yield "big";
						for (auto x : [7, 8]) yield x;
					}
					function make() { return body(3); }
				)"},
				{"range-for over arrays, strings, maps and coroutines", R"(
					coroutine var inner(int n) { for (int i = 0; i < n; ++i) { yield i; } return 7; }
					coroutine var body() {
						var xs = [10, 20, 30];
						for (auto x : xs) { yield x; x += 5; yield x; }
						for (auto& x : xs) { x += 1; yield x; }
						yield xs[0] + xs[1] + xs[2];
						for (int x : xs) { if (x == 21) { xs.push(1); } yield x; }
						for (auto c : "hey") { yield c; }
						var m = {"a": 1, "b": 2, "c": 3};
						for (auto [k, v] : m) { yield `${k}${v}`; }
						for (auto p : m) { yield `${p.first}${p.second}`; p.second = 9; }
						for (auto x : [[1, 2], [3]]) { for (auto y : x) { yield y; } }
						for (auto v : inner(3)) { yield `a${v}`; }
						auto h = inner(2);
						h.next();
						yield "mid";
						for (auto v : h) { yield `b${v}`; }
						yield h.result();
					}
					function make() { return body(); }
				)"},
				{"switch and try", R"(
					coroutine var body(int n) {
						for (int i = 0; i < n; ++i) {
							switch (i) {
								case 0: yield "zero"; break;
								case 1: yield "one"; fallthrough;
								case 2: yield "two"; break;
								default: yield "many";
							}
							switch (i * 2 + 1) {
								case 1: { int t = i; yield `t${t}`; } break;
								case 3: yield "three"; fallthrough;
								default: yield "rest";
							}
						}
						try { yield 1; throw "boom"; } catch (e) { }
						for (int i = 0; i < 2; ++i) { try { yield 10 + i; if (i == 1) { throw "x"; } yield 20 + i; } catch (e) { } }
						try { try { yield "deep"; throw "in"; } catch (e) { throw e + "out"; } } catch (e2) { }
						yield 3;
					}
					function make() { return body(4); }
				)"},
				{"typed arithmetic, counted loops and loops the VM runs as cell blocks", R"(
					coroutine var body(int n) {
						float acc = 0.0;
						int count = 0;
						for (int i = 0; i < n; ++i) {
							for (int j = 0; j < 50; ++j) { acc += j * 0.5; count += j % 3; }
							if (i % 2 == 0) { yield `${acc} ${count}`; }
							acc = acc * 0.5 + i;
							int limit = i + 2;
							for (int k = 0; k < limit; k += 2) { yield k * count; }
						}
						array<float> samples = [1.0, 2.0, 3.0];
						for (int k = 0; k < 3; ++k) { samples[k] = samples[k] * acc; yield samples[k]; }
						int down = 3;
						while (down > 0) { count -= down; --down; yield count; }
						return count;
					}
					function make() { return body(3); }
				)"},
				{"loops over program variables", R"(
					var targets = [3, 4, 5];
					var scores = {"a": 1, "b": 2};
					coroutine var body() {
						for (auto t : targets) { yield t; }
						for (auto [name, score] : scores) { yield `${name}${score}`; }
						for (auto p : scores) { for (auto t : targets) { yield p.second * t; } }
					}
					function make() { return body(); }
				)"},
				{"references the frame owns and handles it shares", R"(
					class Node { int value = 0; Node() {} Node(int v) { value = v; } }
					coroutine var body() {
						int total = 0;
						auto& alias = total;
						alias += 4;
						yield total;
						{ auto bump = [&total]() { total += 1; }; bump(); }
						yield alias;
						shared_ptr<Node> node = make_shared<Node>(7);
						auto same = node;
						yield node.value;
						same.value = 9;
						yield node.value;
						weak_ptr<Node> watcher = node;
						yield watcher.expired();
						const Node& kept = Node(5);
						yield kept.value;
					}
					function make() { return body(); }
				)"},
			};
			for (const auto& [name, program] : cases) { check_round_trips(name, program); }
		});

		test("generated_bodies_save_at_every_step", [this]() {
			// a body of nested loops, branches, switches and tries over a few locals, a yield wherever one fits
			struct generator {
				uint32_t state;
				int names = 0;
				uint32_t pick(uint32_t a_bound) { state = state * 1664525u + 1013904223u; return (state >> 16) % a_bound; }
				std::string block(int a_depth) {
					std::string out;
					const uint32_t count = 1 + pick(3);
					for (uint32_t i = 0; i < count; ++i) { out += statement(a_depth); }
					return out;
				}
				std::string statement(int a_depth) {
					const int n = names++;
					const std::string id = std::to_string(n);
					const uint32_t kinds = a_depth > 0 ? 10 : 3;
					switch (pick(kinds)) {
						case 0: return "yield acc;\n";
						case 1: return "acc = (acc * 3 + " + id + ") % 97;\nyield `s${acc}`;\n";
						case 2: return "var v" + id + " = acc + " + id + ";\nyield v" + id + ";\nacc += v" + id + " % 5;\n";
						case 3: return "for (int i" + id + " = 0; i" + id + " < 2; ++i" + id + ") {\n" + block(a_depth - 1) + "acc += i" + id + ";\n}\n";
						case 4: return "int w" + id + " = 0;\nwhile (w" + id + " < 2) {\n" + block(a_depth - 1) + "++w" + id + ";\n}\n";
						case 5: return "if (acc % 2 == 0) {\n" + block(a_depth - 1) + "} else {\n" + block(a_depth - 1) + "}\n";
						case 6: return "switch (acc % 3) {\ncase 0:\n" + block(a_depth - 1) + "break;\ncase 1:\n" + block(a_depth - 1) + "fallthrough;\ndefault:\n" + block(a_depth - 1) + "}\n";
						case 7: return "try {\n" + block(a_depth - 1) + "if (acc % 3 == 0) { throw \"t\"; }\n" + block(a_depth - 1) + "} catch (e" + id + ") { acc += 7; }\n";
						case 8: return "for (auto x" + id + " : [acc, 2, 3]) {\n" + block(a_depth - 1) + "acc += x" + id + ";\n}\n";
						default: return "var a" + id + " = [1, 2];\nfor (auto& r" + id + " : a" + id + ") {\nr" + id + " += acc;\n" + block(a_depth - 1) + "}\nacc += a" + id + "[0] + a" + id + "[1];\n";
					}
				}
			};
			size_t tried = 0;
			for (uint32_t seed = 1; tried < 16 && seed < 200; ++seed) {
				generator made{seed * 2654435761u};
				const std::string body = made.block(2);
				const std::string program = "coroutine var body(int acc) {\n" + body + "return acc;\n}\nfunction make() { return body(1); }\n";
				auto probe = backend_engine(1);
				run(*probe, program + k_driver);
				const size_t length = steps(run(*probe, "steps_of(make())")).size();
				if (length < 4 || length > 28) { continue; }
				++tried;
				check_round_trips("seed " + std::to_string(seed) + "\n" + body, program);
			}
			check_eq(static_cast<size_t>(16), tried, "bodies generated");
		});

		test("a_coroutine_saved_with_its_receiver_keeps_this", [this]() {
			const std::string prelude = R"(
				class Npc {
					int hp = 10;
					string name = "";
					array<int> path = [1, 2, 3];
					coroutine var think() {
						for (auto& step : path) { step *= 2; yield `${name} walks ${step}`; }
						while (hp > 0) { hp -= 4; yield `${name} hp ${hp}`; }
					}
					auto brain = think();
				}
				class Squad { var brains = []; }
				class World {
					Npc first;
					Npc second;
					shared_ptr<Npc> boss = null;
					var boss_brain = null;
					Squad squad;
					int ticks = 0;
					coroutine var clock() { while (ticks < 9) { ++ticks; yield `tick ${ticks}`; } }
					World() {
						first.name = "a";
						second.name = "b";
						second.hp = 5;
						boss = new Npc();
						boss.name = "boss";
						boss_brain = boss.think();
						squad.brains.push(clock());
					}
				}
				function tick(var& w) {
					string out = "";
					if (!w.first.brain.done()) { out += `${w.first.brain.next()};`; }
					if (!w.second.brain.done()) { out += `${w.second.brain.next()};`; }
					if (!w.boss_brain.done()) { out += `${w.boss_brain.next()};`; }
					if (!w.squad.brains[0].done()) { out += `${w.squad.brains[0].next()};`; }
					return out + `[${w.first.hp} ${w.first.path[2]} ${w.second.hp} ${w.boss.hp} ${w.boss.path[2]} ${w.ticks}]`;
				}
				function trace(var& w, int n) { string out = ""; for (int i = 0; i < n; ++i) { out += tick(w) + "\n"; } return out; }
				function attempt(f) { try { f(); return "saved"; } catch (e) { return `${e}`; } }
			)";
			// each object's coroutines act on that object in the loaded world: an owner by value (owner 1), a shared_ptr's
			// object (this by id), an owner two instances out (owner 2); a loop over a field walks the loaded field
			const std::string expected =
				"a walks 6;b walks 6;boss walks 6;tick 3;[10 6 5 10 6 3]\n"
				"a hp 6;b hp 1;boss hp 6;tick 4;[6 6 1 6 6 4]\n"
				"a hp 2;b hp -3;boss hp 2;tick 5;[2 6 -3 2 6 5]\n"
				"a hp -2;null;boss hp -2;tick 6;[-2 6 -3 -2 6 6]\n";
			const std::pair<const char*, const char*> routes[] = {
				{"World w; tick(w); tick(w); trace(w, 4)", "an uninterrupted world"},
				{"World w; tick(w); tick(w); World loaded = from_json(to_json(w)); trace(w, 4); trace(loaded, 4)", "a world loaded from text"},
				{"World w; tick(w); tick(w); World loaded = from_binary(base64_decode(base64_encode(to_binary(w)))); trace(w, 4); trace(loaded, 4)", "a world loaded from bytes"},
			};
			for (const auto& [source, what] : routes) { check_both(prelude + source, expected, what); }
			check_both(prelude + R"(
				World w;
				tick(w);
				string first = to_json(w);
				string owners = `${first.contains("\"owner\":1")} ${first.contains("\"owner\":2")} ${first.contains("\"this\":{\"$id\":")} ${first.contains("\"field\":\"path\"")}`;
				`${owners} ${first == to_json(from_json(first))} ${base64_encode(to_binary(w)) == base64_encode(to_binary(from_binary(base64_decode(base64_encode(to_binary(w))))))}`
			)", "true true true true true true", "a loaded world saves to the same text and bytes");
			// a coroutine saved on its own brings its receiver; one whose receiver the save also holds by value elsewhere is refused
			const std::string refused = "Cannot save coroutine 'Npc::think': its receiver is saved by value elsewhere in the same save; hold the receiver in a "
			                            "shared_ptr, or keep the coroutine inside the receiver";
			const std::pair<const char*, std::string> cases[] = {
				{"Npc n; n.name = \"n\"; auto h = n.think(); h.next(); auto loaded = from_binary(to_binary(h)); loaded.next() + \"|\" + h.next()", "n walks 4|n walks 4"},
				{"class Before { Npc npc; var brain = null; } Before b; b.brain = b.npc.think(); attempt([&]() { to_json(b); })", refused},
				{"class After { var brain = null; Npc npc; } After a; a.brain = a.npc.think(); attempt([&]() { to_binary(a); })", refused},
				{"class Before { Npc npc; var brain = null; } Before b; b.brain = b.npc.think(); attempt([&]() { to_binary(b); })", refused},
				{"class After { var brain = null; Npc npc; } After a; a.brain = a.npc.think(); attempt([&]() { to_json(a); })", refused},
			};
			for (const auto& [source, wanted] : cases) { check_both(prelude + source, wanted, source); }
		});

		test("a_save_that_cannot_be_exact_is_refused_by_name", [this]() {
			const std::string prelude = R"(
				function why(f) { try { f(); return "saved"; } catch (e) { return `${e}`; } }
				var sink = null;
				var me = null;
				class Grid { var cells = [[1, 2], [3]]; coroutine var walk() { for (auto x : cells[0]) { yield x; } } }
				coroutine var in_expr() { var x = yield 1; yield 2; }
				coroutine var in_call() { print(yield 1); }
				coroutine var in_catch() { try { throw "x"; } catch (e) { yield 1; } }
				coroutine var holds_fn() { auto f = []() { return 1; }; yield f(); }
				coroutine var nests_fn() { var fs = [[]() { return 1; }]; yield 1; }
				coroutine var shared_cell() { int n = 0; sink = [&n]() { n += 1; }; yield n; }
				coroutine var elem_ref() { var xs = [1, 2]; auto& e = xs[0]; yield e; }
				coroutine var map_ref() { var m = {"a": 1}; for (auto& p : m) { yield p.first; } }
				coroutine var self_save() { yield why([&]() { to_json(me); }); }
				coroutine var declares() { function helper() { return 1; } yield helper(); }
				function outer_fn() { coroutine var local_co() { yield 1; } return local_co(); }
				function paused(h) { h.next(); return h; }
			)";
			const std::pair<const char*, const char*> cases[] = {
				{"auto h = paused(in_expr()); why([&]() { to_json(h); })", "Cannot save coroutine 'in_expr': it is paused inside an expression, and only a yield that is its own statement is saved"},
				{"auto h = paused(in_call()); why([&]() { to_binary(h); })", "Cannot save coroutine 'in_call': it is paused inside an expression, and only a yield that is its own statement is saved"},
				{"auto h = paused(in_catch()); why([&]() { to_json(h); })", "Cannot save coroutine 'in_catch': it is paused inside a catch block"},
				{"auto h = paused(holds_fn()); why([&]() { to_json(h); })", "Cannot save coroutine 'holds_fn': its local 'f' holds a function value"},
				{"auto h = paused(nests_fn()); why([&]() { to_json(h); })", "Cannot save coroutine 'nests_fn': it holds a function value"},
				{"auto h = paused(nests_fn()); why([&]() { to_binary(h); })", "Cannot save coroutine 'nests_fn': it holds a function value"},
				{"auto h = paused(shared_cell()); why([&]() { to_json(h); })", "Cannot save coroutine 'shared_cell': its local 'n' is shared with a function value"},
				{"auto h = paused(elem_ref()); why([&]() { to_json(h); })", "Cannot save coroutine 'elem_ref': its local 'e' is a reference the save cannot name"},
				{"auto h = paused(map_ref()); why([&]() { to_json(h); })", "Cannot save coroutine 'map_ref': its local 'p' is a by-reference loop variable over a map"},
				{"Grid g; auto h = paused(g.walk()); why([&]() { to_json(h); })", "Cannot save coroutine 'Grid::walk': its range-for over 'x' walks storage the save cannot name"},
				{"me = self_save(); me.next()", "Cannot save coroutine 'self_save': it is running"},
				{"auto h = paused(declares()); why([&]() { to_json(h); })", "Cannot save coroutine 'declares': its body declares a function, a class, an enum or a namespace ahead of the yield it is paused at"},
				{"auto h = paused(outer_fn()); why([&]() { to_json(h); })", "Cannot save coroutine 'local_co': its function is declared inside another function"},
			};
			for (const auto& [source, wanted] : cases) { check_both(prelude + source, wanted, source); }
			// a coroutine paused across a reload that changed its function runs its old body, which no load could find
			const std::string before = "coroutine var gen() { yield 1; yield 2; }\nfunction why(f) { try { f(); return \"saved\"; } catch (e) { return `${e}`; } }\nvar h = gen(); h.next();";
			for (int use_vm : {0, 1, 2}) {
				auto e = backend_engine(use_vm);
				run(*e, before);
				check_eq(std::string("saved"), run(*e, "why([&]() { to_json(h); })"), "before a reload");
				run(*e, "coroutine var gen() { yield 1; yield 2; }");
				check_eq(std::string("saved"), run(*e, "why([&]() { to_json(h); })"), "a reload with the same code");
				run(*e, "coroutine var gen() { yield 1; yield 3; }");
				check_eq(std::string("Cannot save coroutine 'gen': its function was redefined after it started"), run(*e, "why([&]() { to_json(h); })"), "a reload that changed the code");
				check_eq(std::string("2"), run(*e, "h.next()"), "the paused coroutine still runs its old body");
			}
		});

		test("a_load_checks_what_it_is_handed", [this]() {
			const std::string program = R"(
				coroutine int count(int from, int to) { int seen = 0; for (int i = from; i < to; ++i) { ++seen; yield i; } return seen; }
				function why(f) { try { return `${f()}`; } catch (e) { return `${e}`; } }
			)";
			for (int use_vm : {0, 1, 2}) {
				auto saver = backend_engine(use_vm);
				run(*saver, program);
				const std::string good = run(*saver, "var h = count(0, 9); h.next(); h.next(); to_json(h)");
				const std::string head = "{\"$id\":1,\"$type\":\"coroutine\",\"$val\":{\"function\":\"count\",\"kind\":\"function\",\"code\":\"";
				check_eq(head, good.substr(0, head.size()), "the saved text opens with the function");
				check_eq(std::string("\",\"state\":\"suspended\",\"at\":0,\"locals\":[0,9,2,1],\"loops\":[],\"switches\":[]}}"), good.substr(head.size() + 16), "and holds the frame");
				const auto edited = [&](const std::string& a_from, const std::string& a_to) {
					std::string text = good;
					const size_t at = text.find(a_from);
					check(at != std::string::npos, "the save holds " + a_from);
					if (at != std::string::npos) { text.replace(at, a_from.size(), a_to); }
					auto loader = backend_engine(use_vm);
					run(*loader, program);
					loader->add_global("text", text);
					return run(*loader, "why([&]() { var loaded = from_json(text); return `${loaded.next()} ${loaded.next()}`; })");
				};
				const std::pair<std::pair<const char*, const char*>, const char*> cases[] = {
					{{"\"at\":0", "\"at\":0"}, "2 3"},
					{{"\"locals\":[0,9,2,1]", "\"locals\":[0,9,7,4]"}, "5 6"},
					{{"\"at\":0", "\"at\":7"}, "Cannot load coroutine 'count': its save names no yield of the function"},
					{{"\"at\":0", "\"at\":-1"}, "Cannot load coroutine 'count': its save names no yield of the function"},
					{{"\"at\":0", "\"at\":\"0\""}, "Cannot load coroutine 'count': its save names no yield of the function"},
					{{"\"locals\":[0,9,2,1]", "\"locals\":[0,9,2]"}, "Cannot load coroutine 'count': its save does not fit the function's locals"},
					{{"\"locals\":[0,9,2,1]", "\"locals\":[0,9,2,1,5]"}, "Cannot load coroutine 'count': its save does not fit the function's locals"},
					{{"\"locals\":[0,9,2,1]", "\"locals\":[0,\"nine\",2,1]"}, "Cannot load coroutine 'count': its save holds a value its local 'to' cannot take"},
					{{"\"locals\":[0,9,2,1]", "\"locals\":[0,9,[2],1]"}, "Cannot load coroutine 'count': its save holds a value its local 'seen' cannot take"},
					{{"\"locals\":[0,9,2,1]", "\"locals\":7"}, "Cannot load coroutine 'count': its save is malformed"},
					{{"\"loops\":[]", "\"loops\":[{\"at\":0}]"}, "Cannot load coroutine 'count': its save does not fit the function's locals"},
					{{"\"switches\":[]", "\"switches\":[1]"}, "Cannot load coroutine 'count': its save does not fit the function's locals"},
					{{"\"function\":\"count\"", "\"function\":\"missing\""}, "Cannot load coroutine 'missing': the program has no such coroutine function"},
					{{"\"function\":\"count\"", "\"function\":\"why\""}, "Cannot load coroutine 'why': the program has no such coroutine function"},
					{{"\"function\":\"count\"", "\"function\":7"}, "Cannot load coroutine '?': its save is malformed"},
					{{"\"kind\":\"function\"", "\"kind\":\"method\""}, "Cannot load coroutine 'count': the program has no such coroutine function"},
					{{"\"state\":\"suspended\"", "\"state\":\"dancing\""}, "Cannot load coroutine 'count': its save is malformed"},
					{{"\"code\":\"", "\"code\":\"0"}, "Cannot load coroutine 'count': the program's function differs from the one that was saved"},
				};
				for (const auto& [edit, wanted] : cases) { check_eq(std::string(wanted), edited(edit.first, edit.second), std::string(use_vm == 0 ? "interp: " : use_vm == 1 ? "vm: " : "vm-reference: ") + edit.second); }

				// the same save against a program whose function changed, and against one without it
				auto changed = backend_engine(use_vm);
				run(*changed, "coroutine int count(int from, int to) { for (int i = from; i < to; ++i) { yield i * 2; } return 0; }\nfunction why(f) { try { return `${f()}`; } catch (e) { return `${e}`; } }");
				changed->add_global("text", good);
				check_eq(std::string("Cannot load coroutine 'count': the program's function differs from the one that was saved"), run(*changed, "why([&]() { return from_json(text); })"), "a changed function");
				auto empty = backend_engine(use_vm);
				run(*empty, "function why(f) { try { return `${f()}`; } catch (e) { return `${e}`; } }");
				empty->add_global("text", good);
				check_eq(std::string("Cannot load coroutine 'count': the program has no such coroutine function"), run(*empty, "why([&]() { return from_json(text); })"), "a missing function");
				// cut short, the archive fails in its reader
				check_eq(std::string("Binary archive read past end of data"),
				         run(*saver, "why([&]() { string bytes = to_binary(h); return from_binary(bytes.substr(0, bytes.size() - 9)); })"), "a truncated save");

				// an auto local holds to the type its initializer gave it
				const std::string tally = "coroutine int tally() { auto seen = 0; auto label = \"n\"; while (true) { ++seen; yield seen; } }\n"
				                          "function why(f) { try { return `${f()}`; } catch (e) { return `${e}`; } }";
				auto inferred = backend_engine(use_vm);
				run(*inferred, tally);
				const std::string kept = run(*inferred, "var t = tally(); t.next(); to_json(t)");
				const auto doctored = [&](const std::string& a_from, const std::string& a_to) {
					std::string text = kept;
					const size_t at = text.find(a_from);
					check(at != std::string::npos, "the save holds " + a_from);
					if (at != std::string::npos) { text.replace(at, a_from.size(), a_to); }
					auto loader = backend_engine(use_vm);
					run(*loader, tally);
					loader->add_global("text", text);
					return run(*loader, "why([&]() { return to_string(from_json(text).next()); })");
				};
				check_eq(std::string("2"), doctored("\"locals\":[1,\"n\"]", "\"locals\":[1,\"n\"]"), "an auto local loads");
				check_eq(std::string("Cannot load coroutine 'tally': its save holds a value its local 'seen' cannot take"),
				         doctored("\"locals\":[1,\"n\"]", "\"locals\":[\"one\",\"n\"]"), "an auto int local refuses a string");
				check_eq(std::string("Cannot load coroutine 'tally': its save holds a value its local 'label' cannot take"),
				         doctored("\"locals\":[1,\"n\"]", "\"locals\":[1,[1]]"), "an auto string local refuses an array");
			}
		});

		test("created_finished_and_failed_coroutines_save_as_they_stand", [this]() {
			const std::string prelude = R"(
				function why(f) { try { return `${f()}`; } catch (e) { return `${e}`; } }
				coroutine int count(int from, int to) { for (int i = from; i < to; ++i) { yield i; } return 99; }
				coroutine int fails() { yield 1; throw "bad"; }
				class Box { int n = 4; coroutine int drain() { while (n > 0) { --n; yield n; } return -1; } static coroutine int ticks() { yield 1; yield 2; } }
				namespace tools { coroutine int twice(int v) { yield v; yield v * 2; } }
			)";
			const std::pair<const char*, const char*> cases[] = {
				{"auto h = from_json(to_json(count(1, 3))); `${h.done()} ${h.next()} ${h.next()} ${h.next()} ${h.done()} ${h.result()}`", "false 1 2 99 true 99"},
				{"var targets = [1, 2]; coroutine var walk() { for (auto t : targets) { yield t; } } auto h = walk(); h.next(); string text = to_json(h); targets.push(7); "
				 "auto l = from_json(text); `${text.contains(\"\\\"global\\\":\\\"targets\\\"\")} ${l.next()} ${l.next()} ${h.next()}`", "true 2 7 2"},
				{"auto h = count(1, 2); h.next(); h.next(); auto l = from_binary(to_binary(h)); `${l.done()} ${l.result()} ` + why([&]() { return l.next(); })",
				 "true 99 Cannot resume a completed coroutine"},
				{"auto h = fails(); h.next(); try { h.next(); } catch (e) { } to_json(h).contains(\"\\\"state\\\":\\\"failed\\\"\")", "true"},
				{"auto h = fails(); h.next(); try { h.next(); } catch (e) { } auto l = from_json(to_json(h)); `${l.done()} ${l.result()} ` + why([&]() { return l.next(); })",
				 "true null Cannot resume a completed coroutine"},
				{"auto h = count(0, 9); h.next(); var l = from_binary(to_binary([h, h, {\"again\": h}])); `${l[0].next()} ${l[1].next()} ${l[2][\"again\"].next()} ${h.next()}`", "1 2 3 1"},
				{"auto h = Box::ticks(); h.next(); auto l = from_json(to_json(h)); `${l.next()} ${h.next()}`", "2 2"},
				{"auto h = tools::twice(4); h.next(); auto l = from_binary(base64_decode(base64_encode(to_binary(h)))); `${l.next()} ${h.next()} ${to_json(h).contains(\"tools::twice\")}`", "8 8 true"},
				{"shared_ptr<Box> b = make_shared<Box>(); auto h = b.drain(); h.next(); var l = from_json(to_json([b, h])); `${l[1].next()} ${l[0].n} ${b.n}`", "2 2 3"},
				{"auto h = count(0, 3); h.next(); auto c = from_binary(to_binary(h)); auto d = h; `${c.next()} ${h.next()} ${d.next()} ${c.next()} ${c.next()} ${c.done()} ${h.done()}`", "1 1 2 2 99 true false"},
				{"Box b; auto h = b.drain(); h.next(); auto c = from_binary(to_binary(h)); `${c.next()} ${h.next()} ${b.n}`", "2 2 2"},
			};
			for (const auto& [source, wanted] : cases) { check_both(prelude + source, wanted, source); }
			// a throw that leaves the body fails the handle on either backend
			for (int use_vm : {0, 1, 2}) {
				auto e = backend_engine(use_vm);
				run(*e, prelude + "var h = fails(); h.next(); try { h.next(); } catch (e) { }");
				auto holder = e->execute("h").get_object_holder();
				auto handle = std::static_pointer_cast<jai::coroutine_handle>(holder->host_object());
				check(handle->get_status() == jai::coroutine_handle::status::failed, use_vm == 0 ? "interp: failed" : "vm: failed");
			}
		});

		test("a_copy_gives_the_coroutines_its_object_holds_their_own_frames", [this]() {
			// After k steps, the copy's coroutine runs on from the same yield over the copy's state, and the original's is untouched
			const std::pair<const char*, const char*> shapes[] = {
				{"locals and a while loop", R"(
					class H { int ticks = 0; auto co = body(3); coroutine int body(int a_step) { int local = 100; auto inferred = 1; while (local < 112) { local += a_step; inferred += 1; ticks += 1; yield local + inferred; } return ticks; } }
				)"},
				{"range-for over a field and a local", R"(
					class H { int ticks = 0; array<int> plan = [3, 5, 8]; auto co = body();
						coroutine int body() { array<int> mine = [1, 2]; for (auto p : plan) { for (auto& m : mine) { m += p; ticks += 1; yield m; } } return ticks; } }
				)"},
				{"a map loop, a switch and a reference local", R"(
					class H { int ticks = 0; map<string, int> bag = {"a": 1, "b": 2}; auto co = body();
						coroutine int body() { int base = 10; int& alias = base; for (auto [k, v] : bag) { switch (v) { case 1: alias += 1; yield alias; break; default: ticks += v; yield ticks; } } return base; } }
				)"},
				{"a lambda local bound to the object", R"(
					class H { int ticks = 0; auto co = body(); coroutine int body() { auto bump = [this]() { ticks += 1; return ticks; }; while (ticks < 4) { yield bump(); } return -ticks; } }
				)"},
				{"a nested coroutine of the object", R"(
					class H { int ticks = 0; auto co = body(); coroutine int inner() { for (int i = 0; i < 3; ++i) { ticks += 1; yield i; } }
						coroutine int body() { for (auto v : inner()) { yield v * 10 + ticks; } return ticks; } }
				)"},
			};
			const std::string driver = R"(
				function drain(var h) { string out = ""; while (!h.done()) { var v = h.next(); out += `${h.done() ? "=" : ""}${v},`; } return out; }
				function copied(int k) { H a; for (int i = 0; i < k; ++i) { a.co.next(); } H b = a; string rb = drain(b.co); string after_b = `${a.ticks}`; string ra = drain(a.co); return `${rb}|${ra}|${after_b}|${a.ticks}|${b.ticks}`; }
				function whole() { H a; return drain(a.co); }
			)";
			for (const auto& [name, program] : shapes) {
				std::shared_ptr<engine> engines[3] = {backend_engine(0), backend_engine(1), backend_engine(2)};
				std::string full[3];
				for (int b = 0; b < 3; ++b) {
					check_eq(std::string("0"), run(*engines[b], std::string(program) + driver + "\n0"), std::string(name) + ": loads");
					full[b] = run(*engines[b], "whole()");
				}
				check_eq(full[0], full[1], std::string(name) + ": the backends agree");
				check_eq(full[0], full[2], std::string(name) + ": the reference VM agrees");
				const std::vector<std::string> all = steps(full[1]);
				for (size_t k = 0; k < all.size(); ++k) {
					const std::string result[3] = {run(*engines[0], "copied(" + std::to_string(k) + ")"), run(*engines[1], "copied(" + std::to_string(k) + ")"),
					                               run(*engines[2], "copied(" + std::to_string(k) + ")")};
					std::string rest;
					for (size_t i = k; i < all.size(); ++i) { rest += all[i] + ","; }
					const std::string at = std::string(name) + " after " + std::to_string(k) + " steps";
					check_eq(rest, result[1].substr(0, result[1].find('|')), at + ": the copy runs on from the same yield");
					check_eq(rest, result[1].substr(result[1].find('|') + 1, rest.size()), at + ": the original runs on from it too");
					check_eq(result[0], result[1], at + ": the backends agree");
					check_eq(result[1], result[2], at + ": the reference VM agrees");
				}
			}
			check_both(R"(
				class H { int ticks = 0; auto co = body(); coroutine int body() { while (true) { ticks += 1; yield ticks; } } }
				H a; a.co.next(); H b = a; a.co.next(); auto bare = b.co; bare.next(); b.co.next();
				`${a.ticks}|${b.ticks}`
			)", "2|3", "a handle copied by itself shares its coroutine; the object's copy has its own");
			check_both(R"(
				coroutine int free_body() { yield 1; yield 2; }
				class H { int ticks = 0; var co; }
				H a; a.co = free_body(); H b = a; a.co.next(); b.co.next(); `${b.co.done()}`
			)", "false", "a coroutine bound to no object is shared by the copy");
			check_both(R"(
				class Npc { int steps = 0; var partner; coroutine int walk() { while (true) { steps += 1; yield steps; } } }
				class Camp { array<Npc> pair; }
				array<Npc> duo; duo.push(Npc()); duo.push(Npc()); duo[0].partner = duo[1].walk(); duo[0].partner.next();
				var copy = duo; copy[0].partner.next(); copy[0].partner.next();
				Camp camp; camp.pair = duo; Camp other = camp; other.pair[0].partner.next();
				map<string, Npc> named; named["a"] = Npc(); named["b"] = Npc(); named["a"].partner = named["b"].walk();
				auto named_copy = named; named_copy["a"].partner.next();
				`${duo[1].steps}|${copy[1].steps}|${camp.pair[1].steps}|${other.pair[1].steps}|${named["b"].steps}|${named_copy["b"].steps}`
			)", "1|3|1|2|0|1", "a coroutine in one element whose receiver is another element runs on that element's copy: an array, the array in an object, a map");
			check_both(R"(
				class H { int got = 0; auto co = body(); coroutine int body() { while (true) { var v = yield got; got += 1; } } }
				H a; a.co.next(); H b = a; a.co.next();
				string r = `${b.co.done()}`;
				try { b.co.next(); } catch (e) { r += "|" + e; }
				try { to_json(b); } catch (e) { r += "|" + e; }
				r + "|" + to_string(a.got)
			)", "false|Cannot resume coroutine 'H::body': it was not copied with its object: it was paused inside an expression, and only a yield that is its own statement is copied"
			    "|Cannot save coroutine 'H::body': it was not copied with its object: it was paused inside an expression, and only a yield that is its own statement is copied|1",
			    "a coroutine whose state has no copy leaves a handle that raises when it is resumed");
			check_both(R"(
				class H { int n = 0; auto co = body(); coroutine int body() { try { throw "x"; } catch (e) { yield 1; } yield 2; } }
				H a; a.co.next(); H b = a; string r = ""; try { b.co.next(); } catch (e) { r = e; } r + "|" + to_string(a.co.next())
			)", "Cannot resume coroutine 'H::body': it was not copied with its object: it was paused inside a catch block|2", "a pause in a catch block");
			check_both(R"(
				class H { int n = 0; auto co = body(); coroutine int body() { while (true) { n += 1; H snap = this; yield n; } } }
				H a; string r = to_string(a.co.next()); r
			)", "1", "a copy taken while the coroutine runs leaves the copy's handle unresumable, and the original runs on");
		});

		test("a_dropped_loaded_coroutine_destroys_what_the_original_would", [this]() {
			const std::string prelude = R"(
				var log = "";
				class Noisy { string tag = ""; Noisy() {} Noisy(string t) { tag = t; } ~Noisy() { log += `~${tag} `; } }
				coroutine var body(Noisy first) {
					Noisy second("second");
					for (auto item : [Noisy("a"), Noisy("b")]) {
						Noisy inner("inner");
						for (int i = 0; i < 2; ++i) {
							Noisy deep(`deep${i}`);
							yield item.tag;
						}
					}
				}
				function paused(int steps) { auto h = body(Noisy("first")); for (int i = 0; i < steps; ++i) { h.next(); } return h; }
			)";
			for (int k = 0; k <= 4; ++k) {
				const std::string count = std::to_string(k);
				auto [interpreted, compiled] = both_backends(prelude + "var original = paused(" + count + "); log = ``; original = null; log");
				check_eq(interpreted, compiled, "the backends agree after " + count + " steps");
				for (const char* route : {"from_binary(to_binary(original))", "from_json(to_json(original))"}) {
					check_both(prelude + "var original = paused(" + count + "); var copy = " + route + "; log = ``; copy = null; string first = log; log = ``; original = null; first == log ? `same` : `${first}|${log}`",
					           "same", std::string(route) + " after " + count + " steps");
				}
			}
		});

		test("a_try_entered_under_one_resume_catches_under_a_deeper_one", [this]() {
			check_both(R"(
				coroutine var tries() { try { yield 1; throw "boom"; } catch (e) { yield "caught"; } yield 3; }
				function deep(h, int n) { if (n == 0) { return h.next(); } return deep(h, n - 1); }
				auto h = tries();
				`${h.next()} ${deep(h, 5)} ${deep(h, 2)} ${h.next()} ${h.done()}`
			)", "1 caught 3 null true", "the handler restores the depth of the resume that reached it");
		});

		test("an_archive_never_drops_a_coroutine_silently", [this]() {
			const std::string prelude =
				"coroutine int count() { yield 1; yield 2; }\n"
				"class Boss { int hp = 3; coroutine int fight() { yield hp; } auto brain = fight(); }\n"
				"function attempt(f) { try { f(); return \"saved\"; } catch (e) { return \"\" + e; } }\n";
			const std::pair<const char*, const char*> cases[] = {
				{"auto h = count(); to_json(h) != \"null\"", "true"},
				{"auto h = count(); to_json([1, h]).contains(\"coroutine\")", "true"},
				{"auto h = count(); from_binary(to_binary({\"k\": h}))[\"k\"].next()", "1"},
				{"Boss b; b.brain.next(); from_binary(base64_decode(base64_encode(to_binary(b)))).brain.done()", "false"},
			};
			for (const auto& [source, expected] : cases) { check_both(prelude + source, expected, source); }
		});
	}
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::coroutine_snapshot_tests)
