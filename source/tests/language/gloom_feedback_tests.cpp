// Regressions surfaced by the GLOOM dogfooding project (examples/gloom,
// reference/README.md "language-feel notes"). Each test runs on BOTH backends and
// asserts the intended behavior.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"

#include <string>
#include <tuple>
#include <variant>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {

std::shared_ptr<jai::engine> gloom_engine(bool use_vm) {
	auto e = jai::foundry::make_default_engine();
	e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
	jai::stdlib::register_all(e);
	return e;
}

std::string backend_tag(bool use_vm) { return use_vm ? "[vm] " : "[interp] "; }

using expected_value = std::variant<int64_t, std::string>;

// {name, source, expected}: the source's value on each backend, read as the expected alternative's type
template<typename Cases>
void check_both_backends(const Cases& a_cases) {
	check_each(a_cases, [](const auto& c) {
		for (bool use_vm : {false, true}) {
			auto e = gloom_engine(use_vm);
			auto r = e->execute(std::get<1>(c));
			if (const auto* expected = std::get_if<int64_t>(&std::get<2>(c))) {
				check_eq(*expected, r.as_int(), backend_tag(use_vm));
			} else {
				check_eq(std::get<std::string>(std::get<2>(c)), r.template as<std::string>(), backend_tag(use_vm));
			}
		}
	});
}

} // namespace

class gloom_feedback_tests : public suite {
public:
	gloom_feedback_tests() : suite("GLOOM Feedback") {}

	void forge_tests() override {

		// ============================================================
		// Item D: ternary over element reads into a typed local.
		// `int x = c ? a[i] : b[j];` errored "Type mismatch in assignment"
		// (element reads arrive as reference wrappers; ternaries hid them from
		// the decl path's lvalue detection) while if/else worked.
		// ============================================================

		test("decls_from_element_reads_and_ref_returns_copy_values_and_share_handles", [this]() {
			const std::tuple<const char*, const char*, expected_value> cases[] = {
				{"ternary_element_reads_into_typed_local", R"(
					var a = [1, 2, 3];
					var b = [10, 20, 30];
					bool c = true;
					int x = c ? a[1] : b[2];
					int y = !c ? a[1] : b[2];
					x * 100 + y;
				)", int64_t{230}},
				{"ternary_element_reads_typed_variants", R"(
					var a = [1, 2, 3];
					var b = [10, 20, 30];
					var m = {"k": 7};
					bool c = true;
					float f = c ? a[1] : b[2];        // float target converts
					int mixed = c ? a[1] : 99;         // TAKEN branch is the element read
					int mk = c ? m["k"] : a[0];        // map-entry read
					int nested = c ? (c ? a[0] : a[1]) : b[0];
					to_string(f) + " " + to_string(mixed) + " " + to_string(mk) + " " + to_string(nested);
				)", std::string("2.0 2 7 1")},
				{"ternary_element_reads_in_function_slots", R"(
					var a = [1, 2, 3];
					var b = [10, 20, 30];
					function pick(bool c) -> int {
						int x = c ? a[1] : b[2];
						return x;
					}
					pick(true) * 100 + pick(false);
				)", int64_t{230}},
				{"ternary_element_decl_never_aliases", R"(
					var a = [1, 2, 3];
					var b = [10, 20, 30];
					bool c = true;
					var t = c ? a[1] : b[2];
					auto u = c ? a[1] : b[2];
					a[1] = 777;                       // must not change t or u
					var w = c ? a[0] : b[0];
					w = 42;                            // must not write into a[0]
					to_string(t) + " " + to_string(u) + " " + to_string(a[0]);
				)", std::string("2 2 1")},
				{"ternary_element_decl_deep_copies_containers", R"(
					var rows = [[1, 2], [3, 4]];
					bool c = true;
					var row = c ? rows[0] : rows[1];
					row.push(99);                      // must not grow rows[0]
					rows[0].size() * 10 + row.size();
				)", int64_t{23}},
				{"typed_shared_ptr_decl_from_element_read", R"(
					class Mob { int hp = 10; void hurt(int d) { hp = hp - d; } }
					var arr = [];
					arr.push(new Mob());
					shared_ptr<Mob> t = arr[0];        // used to error: "Cannot initialize shared_ptr with this type"
					t.hurt(3);
					arr[0].hp;
				)", int64_t{7}},
				{"weak_ptr_decl_from_element_read", R"(
					class Mob { int hp = 10; }
					var arr = [];
					arr.push(new Mob());
					weak_ptr<Mob> w = arr[0];
					auto locked = w.lock();
					locked.hp;
				)", int64_t{10}},
				{"value_decl_from_ref_return_copies", R"(
					int g = 5;
					int& get() { return g; }
					var x = get();
					x = 9;                             // must not write through into g
					g * 10 + x;
				)", int64_t{59}},
			};
			check_both_backends(cases);
		});

		// ============================================================
		// Item C: implicit copies of shared_ptr values must SHARE the handle
		// (guide ch03: "assignment shares instead of deep-copying"; clone() stays
		// the explicit deep copy). GLOOM: `var e = arr[i]; e.hurt()` silently
		// mutated a deep copy. The full store-boundary matrix is pinned here;
		// value-semantic elements must keep deep-copying.
		// ============================================================

		test("shared_ptr_stores_share_the_handle_and_values_deep_copy", [this]() {
			const std::tuple<const char*, const char*, expected_value> cases[] = {
				{"shared_ptr_element_reads_share", R"(
					class Mob { int hp = 10; void hurt(int d) { hp = hp - d; } }
					var arr = [];
					arr.push(new Mob());
					var m = {};
					m["k"] = new Mob();
					var e1 = arr[0];   e1.hurt(1);      // element decl
					auto e2 = arr[0];  e2.hurt(1);      // auto element decl
					var e3 = null;     e3 = arr[0];  e3.hurt(1);   // element assign
					var mv = m["k"];   mv.hurt(1);      // map-value decl
					var mv2 = null;    mv2 = m["k"]; mv2.hurt(1);  // map-value assign
					arr[0].hp * 100 + m["k"].hp;
				)", int64_t{708}},
				{"shared_ptr_ident_assign_shares", R"(
					class Mob { int hp = 10; void hurt(int d) { hp = hp - d; } }
					auto p = new Mob();
					var q = null;
					q = p;                    // assign into an any/null-typed var keeps the handle marker
					q.hurt(1);
					function f() {
						auto p2 = new Mob();
						var q2 = null;
						q2 = p2;              // slot path twin
						q2.hurt(1);
						return p2.hp;
					}
					p.hp * 100 + f();
				)", int64_t{909}},
				{"shared_ptr_field_and_element_stores_share", R"(
					class Mob { int hp = 10; void hurt(int d) { hp = hp - d; } }
					class Holder { var fx = null; }
					auto h = new Holder();
					var arr = [];
					arr.push(new Mob());
					h.fx = arr[0];            // field assign from element
					h.fx.hurt(1);
					var arr2 = [null];
					auto p = new Mob();
					arr2[0] = p;              // element store of a handle
					arr2[0].hurt(1);
					arr[0].hp * 100 + p.hp;
				)", int64_t{909}},
				{"shared_ptr_loop_destructure_args_share", R"(
					class Mob { int hp = 10; void hurt(int d) { hp = hp - d; } }
					var arr = [];
					arr.push(new Mob());
					arr.push(new Mob());
					for (auto en : arr) { en.hurt(1); }          // copy range-for binding shares handles
					var [x, y] = arr;                             // destructuring shares handles
					x.hurt(1);
					function take(en) { en.hurt(1); }
					take(arr[1]);                                 // element read as by-value arg
					var m = {};
					m["a"] = new Mob();
					for (auto kv : m) { kv.second.hurt(1); }      // map iteration value shares
					arr[0].hp * 1000 + arr[1].hp * 100 + m["a"].hp;
				)", int64_t{8809}},
				{"value_semantics_still_deep_copy_at_stores", R"(
					class Pt { int v = 1; }
					var arr = [];
					arr.push(Pt());                               // VALUE instance element
					var c = arr[0];  c.v = 9;                     // element decl copies
					var c2 = null;   c2 = arr[0]; c2.v = 8;       // element assign copies
					for (auto p : arr) { p.v = 7; }               // copy loop binding copies
					var rows = [[1], [2]];
					var [r0, r1] = rows;                          // destructuring copies containers
					r0.push(99);
					arr[0].v * 10 + rows[0].size();
				)", int64_t{11}},
				{"assigned_shared_ptr_keeps_reference_semantics_downstream", R"(
					class Mob { int hp = 10; void hurt(int d) { hp = hp - d; } }
					auto p = new Mob();
					var q = null;
					q = p;
					var q2 = q;               // handle copied on through a second decl
					q2.hurt(1);
					weak_ptr<Mob> w = q;      // and stays weak_ptr-compatible
					auto locked = w.lock();
					locked.hurt(1);
					p.hp;
				)", int64_t{8}},
			};
			check_both_backends(cases);
		});

		// ============================================================
		// Item B: string subscript. `s[i]` is a read-only CHAR read (C++-familiar);
		// writes are a clear error (strings share storage under copy - subscript
		// writes would need copy-on-write, noted as future work). Bounds/type error
		// text matches the array style, with the numbers actually printed (the
		// placeholder machinery resolves symbol ids, so raw counts printed garbage
		// like "Array index bool out of bounds for array of size class_definition").
		// Char-char ordering comparisons ride in (the demoreel key[0] path needs
		// c0 >= '1' && c0 <= '9').
		// ============================================================

		test("string_subscript_reads_a_char", [this]() {
			const std::tuple<const char*, const char*, expected_value> cases[] = {
				{"string_subscript_reads_char", R"(
					var s = "hello";
					char c = s[1];
					var parts = [];
					parts.push(type_of(c));
					parts.push(to_string(c == 'e'));
					parts.push(type_of(s.at(1)));       // at() keeps returning a 1-char string
					parts.push(to_string(s[s.size() - 1] == 'o'));
					int count = 0;
					for (int i = 0; i < s.size(); ++i) { if (s[i] == 'l') { count = count + 1; } }
					parts.push(to_string(count));
					parts.push(s);                       // source string untouched
					parts[0] + " " + parts[1] + " " + parts[2] + " " + parts[3] + " " + parts[4] + " " + parts[5];
				)", std::string("char true char true 2 hello")},
				{"demoreel_handle_key_shape_is_live", R"(
					int nscenes = 9;
					int started = -1;
					function start_scene(int i) { started = i; }
					function handle_key(string key) {
						if (key == "") { return; }
						if (key.size() == 1) {
							char c0 = key[0];
							if (c0 >= '1' && c0 <= '9') {
								int d = key.to_int();
								if (d <= nscenes) { start_scene(d - 1); }
							}
							else if (c0 == '0') { start_scene(nscenes - 1); }
						}
					}
					handle_key("7");
					int seven = started;
					handle_key("0");
					seven * 100 + started;
				)", int64_t{608}},
			};
			check_both_backends(cases);
		});

		test("string_subscript_errors_byte_identical", [this]() {
			auto run_catch = [](bool use_vm, const char* src) -> std::string {
				auto e = gloom_engine(use_vm);
				try { e->execute(src); } catch (const std::exception& ex) { return ex.what(); }
				return "<no throw>";
			};
			// Each case: interp and vm messages must be byte-IDENTICAL, and carry the
			// expected core text (the engine boundary appends the error-code category)
			const std::pair<const char*, const char*> cases[] = {
				{ R"(var s = "hello"; s[99];)", "String index 99 out of bounds for string of size 5" },
				{ R"(var s = "hello"; s[-1];)", "String index -1 out of bounds for string of size 5" },
				{ R"(var s = "hello"; s["k"];)", "String index must be an integer" },
				{ R"(var s = "hello"; s[0] = 5;)", "Cannot assign int to a string character (it takes a char or a string)" },
				{ R"(var a = [1]; a[5];)", "Array index 5 out of bounds for array of size 1" },
			};
			for (const auto& [src, core] : cases) {
				const std::string interp_msg = run_catch(false, src);
				const std::string vm_msg = run_catch(true, src);
				check_eq(interp_msg, vm_msg, std::string("backend parity: ") + core);
				check_true(interp_msg.find(core) != std::string::npos, std::string("core text present: ") + interp_msg);
			}
		});

		test("char_ordering_comparisons", [this]() {
			for (bool use_vm : {false, true}) {
				auto e = gloom_engine(use_vm);
				auto r = e->execute(R"(
					char a = 'a';
					char m = 'm';
					var ok = (a < m) && (a <= 'a') && (m > a) && (m >= 'm') && !('z' < a);
					ok;
				)");
				check_true(r.as<bool>(), backend_tag(use_vm) + "char-char ordering comparisons");
				// Comparison inting: mixed char/int ordering promotes to int64 0..255
				// like arithmetic.
				auto e2 = gloom_engine(use_vm);
				check_true(!e2->execute("var x = 'a' < 1; x").as<bool>(), backend_tag(use_vm) + "char/int ordering promotes (97 < 1 is false)");
				check_true(e2->execute("var y = 'a' < 98; y").as<bool>(), backend_tag(use_vm) + "char/int ordering promotes (97 < 98 is true)");
			}
		});

		// ============================================================
		// Item E: an uncaught script throw crossing the host boundary through a
		// stored callable (get_variable + as_function) must propagate as the same
		// script_exception engine::execute throws - with the ORIGINAL message. It
		// used to be swallowed: the thunk returned success(null) with the backend's
		// unwinding flag latched, so hosts saw nonsense downstream errors
		// ("script_value is not a boolean. Actual type: 0") and a poisoned engine.
		// ============================================================

		test("host_callable_uncaught_throw_carries_message", [this]() {
			for (bool use_vm : {false, true}) {
				auto e = gloom_engine(use_vm);
				e->execute(R"(
					function boom() { throw "the actual message"; }
					function fine() { return 7; }
				)");
				auto boom = e->get_variable("boom");
				std::string caught = "<no throw>";
				try {
					auto r = boom.as_function()(std::vector<jai::script_value>{});
					if (!r.has_value()) { caught = std::string("<checked error> ") + e->format_error(r); }   // the checked flavor carries it
				} catch (const jai::script_exception& ex) {
					caught = std::string("<threw> ") + ex.what();
				}
				check_true(caught.rfind("<checked error> ", 0) == 0, backend_tag(use_vm) + "the result carries it, no throw: " + caught);
				check_true(caught.find("the actual message") != std::string::npos,
					backend_tag(use_vm) + "host sees the thrown message, got: " + caught);

				// ...and matches execute()'s uncaught-throw report for the same script
				std::string exec_msg = "<no throw>";
				try { e->execute("boom();"); } catch (const jai::script_exception& ex) { exec_msg = ex.what(); }
				check_true(exec_msg.find("the actual message") != std::string::npos,
					backend_tag(use_vm) + "execute() baseline carries the message");

				// the engine stays usable after the boundary throw
				auto fine = e->get_variable("fine");
				auto r2 = fine.as_function()(std::vector<jai::script_value>{});
				check_true(r2.has_value(), backend_tag(use_vm) + "engine usable after boundary throw");
				check_eq((int64_t)7, r2.value().as_int(), backend_tag(use_vm) + "next host call returns real value");
			}
		});

		test("host_callable_throw_mid_script_stays_catchable", [this]() {
			// A host function that reinvokes a script callable MID-SCRIPT must not
			// steal the throw: the calling script's try/catch still sees the
			// original thrown VALUE (the boundary conversion is host-level only).
			for (bool use_vm : {false, true}) {
				auto e = gloom_engine(use_vm);
				auto stored = std::make_shared<jai::script_value>(std::monostate{}, e.get());
				e->add_function("call_stored", [stored]() -> jai::script_value {
					auto r = stored->as_function()(std::vector<jai::script_value>{});
					if (!r.has_value()) { throw std::runtime_error(std::string(r.message())); }
					return r.value();
				});
				e->execute(R"(function inner() { throw "inner payload"; })");
				*stored = e->get_variable("inner");
				auto r = e->execute(R"(
					var got = "";
					try { call_stored(); } catch (err) { got = err; }
					got;
				)");
				check_eq(std::string("inner payload"), r.as<std::string>(),
					backend_tag(use_vm) + "script catch still sees the original thrown value");
			}
		});

		test("host_callable_after_nested_execute_stays_mid_script", [this]() {
			// The executing state is a COUNTER (semaphore), not a bool.
			// A host callback that runs a NESTED execute() and then invokes a
			// throwing stored callable is still MID-SCRIPT after the nested execute
			// returns - the boundary conversion must not fire, and the outer
			// script's catch still sees the original thrown value.
			for (bool use_vm : {false, true}) {
				auto e = gloom_engine(use_vm);
				auto stored = std::make_shared<jai::script_value>(std::monostate{}, e.get());
				jai::engine* raw = e.get();
				e->add_function("nested_then_call", [stored, raw]() -> jai::script_value {
					raw->execute("var nested_side = 1;");   // nested execute enters and EXITS
					auto r = stored->as_function()(std::vector<jai::script_value>{});
					if (!r.has_value()) { throw std::runtime_error(std::string(r.message())); }
					return r.value();
				});
				e->execute(R"(function inner() { throw "counted payload"; })");
				*stored = e->get_variable("inner");
				auto r = e->execute(R"(
					var got = "";
					try { nested_then_call(); } catch (err) { got = err; }
					got;
				)");
				check_eq(std::string("counted payload"), r.as<std::string>(),
					backend_tag(use_vm) + "post-nested-execute callable throw stays script-catchable");
			}
		});

		// ============================================================
		// Item A: `var name() { ... }` methods. GLOOM's feel notes claimed the
		// spelling silently vanished (no parse error, "no member" at call time) and
		// required `function name() -> var`. The reported failure does NOT
		// reproduce at HEAD in any shape (incl. the real GLOOM Game class, jaibite
		// cache reload and include-hot-reload) - the declaration-form catalog
		// (docs/site/guide/06-functions.html "Declaration forms: the full catalog") says
		// class methods accept every free-function spelling, and they do. These
		// pins keep the whole var/auto method family from EVER vanishing silently.
		// ============================================================

		test("var_and_auto_function_and_method_forms_are_callable", [this]() {
			const std::tuple<const char*, const char*, expected_value> cases[] = {
				{"var_auto_method_forms", R"(
					class G {
						int hp = 3;
						var pilot = null;
						var gather() { return {"fwd": true, "n": hp}; }   // map-literal return (the GLOOM shape)
						auto pick() { return hp * 2; }
						static var mk() { return 42; }
						coroutine var brain() {
							for (int i = 0; i < 2; ++i) { yield i; }
							yield 99;
						}
						var take(var kinds) { return kinds[0]; }
					}
					class H : G {
						override var gather() { return {"fwd": false, "n": 0}; }
					}
					auto g = G();
					auto h = H();
					var inp = g.gather();
					auto co = g.brain();
					co.next();
					var second = co.next();
					var parts = [];
					parts.push(to_string(inp["n"]));
					parts.push(to_string(g.pick()));
					parts.push(to_string(G::mk()));
					parts.push(to_string(g.take([7, 8])));
					parts.push(to_string(second));
					parts.push(to_string(h.gather()["fwd"]));
					parts[0] + " " + parts[1] + " " + parts[2] + " " + parts[3] + " " + parts[4] + " " + parts[5];
				)", std::string("3 6 42 7 1 false")},
				{"var_function_forms_top_level_and_namespace", R"(
					var f() { return 41; }
					namespace ns {
						var helper() { return 8; }
						class Inner { var val() { return 9; } }
					}
					auto i = ns::Inner();
					f() * 100 + ns::helper() * 10 + i.val();
				)", int64_t{4189}},
			};
			check_both_backends(cases);
		});

		test("var_method_survives_class_redefinition", [this]() {
			// Re-executing the class (hot-reload path) must keep var methods callable
			for (bool use_vm : {false, true}) {
				auto e = gloom_engine(use_vm);
				const char* cls = R"(class R { int n = 1; var getn() { return n; } })";
				e->execute(cls);
				auto r1 = e->execute("auto r = R(); r.getn();");
				check_eq((int64_t)1, r1.as_int(), backend_tag(use_vm) + "var method before reload");
				e->execute(cls);   // same parse unit again = reload
				auto r2 = e->execute("auto r2 = R(); r2.getn();");
				check_eq((int64_t)1, r2.as_int(), backend_tag(use_vm) + "var method after reload");
			}
		});

	}
};

} // namespace jai::foundry::tests

using gloom_feedback_tests = jai::foundry::tests::gloom_feedback_tests;
FOUNDRY_REGISTER(gloom_feedback_tests)
