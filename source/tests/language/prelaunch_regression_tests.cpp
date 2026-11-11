#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/ast_serializer.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <cmath>
#include <limits>
#include <optional>

namespace jai::foundry::tests {

namespace prelaunch_detail {

struct first_base { int first{11}; };
struct second_base { int second{22}; };
struct native_child : first_base, second_base {};
struct native_grandchild : native_child {};
struct virtual_child : first_base, virtual second_base {};
struct abstract_base { virtual ~abstract_base() = default; virtual int read() const = 0; };
struct abstract_child : first_base, abstract_base { int read() const override { return 29; } };
struct unrelated { int other{33}; };

inline void bind_native(engine& eng) {
	dynamic_binder<first_base>(eng, "FirstBase").constructor<>().property("first", &first_base::first).build();
	dynamic_binder<second_base>(eng, "SecondBase").constructor<>().property("second", &second_base::second).build();
	dynamic_binder<native_child>(eng, "NativeChild").constructor<>().base_class<first_base>().base_class<second_base>().build();
	dynamic_binder<unrelated>(eng, "Unrelated").constructor<>().build();
}

}

class prelaunch_regression_tests : public suite {
public:
	prelaunch_regression_tests() : suite("Prelaunch Regressions") {}

	void forge_tests() override {
		test("numeric_int64_selection_is_exact", [this]() {
			auto eng = make_engine();
			constexpr script_int low = 9007199254740992LL;
			check_eq(low, eng->execute("math::min(9007199254740993, 9007199254740992);").as_int());
			check_eq(low + 1, eng->execute("math::max(9007199254740992, 9007199254740993);").as_int());
			check_eq(low, eng->execute("math::clamp(9007199254740993, 0, 9007199254740992);").as_int());
			check_eq(-low, eng->execute("math::clamp(-9007199254740993, -9007199254740992, 0);").as_int());
			eng->add_global("lowest", script_value((std::numeric_limits<script_int>::min)(), eng.get()));
			eng->add_global("highest", script_value((std::numeric_limits<script_int>::max)(), eng.get()));
			check_eq((std::numeric_limits<script_int>::max)() - 1, eng->execute("math::min(highest, highest - 1);").as_int());
			check_eq((std::numeric_limits<script_int>::min)() + 1, eng->execute("math::max(lowest, lowest + 1);").as_int());
			script_int bound = low + 1;
			eng->add_global_ref("bound", bound);
			check_eq(low, eng->execute("math::min(bound, 9007199254740992);").as_int());
		});

		test("numeric_mixed_and_float_selection_unchanged", [this]() {
			auto eng = make_engine();
			// Mixed pairs still promote to double, and ties still choose the first operand.
			check_eq(script_int{9007199254740993LL}, eng->execute("math::min(9007199254740993, 9007199254740992.0);").as_int());
			check(eng->execute("math::min(9007199254740992.0, 9007199254740993);").is_float());
			eng->add_global("negative_zero", script_value(-0.0, eng.get()));
			eng->add_global("not_a_number", script_value(std::numeric_limits<double>::quiet_NaN(), eng.get()));
			check(std::signbit(eng->execute("math::min(negative_zero, 0.0);").as_float()));
			check(std::signbit(eng->execute("math::max(negative_zero, 0.0);").as_float()));
			check_eq(script_int{2}, eng->execute("math::min(not_a_number, 2);").as_int());
			check(std::isnan(eng->execute("math::max(2, not_a_number);").as_float()));
			check(std::isnan(eng->execute("math::clamp(not_a_number, 0, 1);").as_float()));
		});

		test("numeric_integer_conversion_valid_boundaries", [this]() {
			auto eng = make_engine();
			const double upper = std::nextafter(0x1p63, 0.0);
			eng->add_global("upper", script_value(upper, eng.get()));
			eng->add_global("lower", script_value(-0x1p63, eng.get()));
			for (const std::string fn : {"math::itrunc", "math::ifloor", "cast<int>"}) {
				check_eq(static_cast<script_int>(upper), eng->execute(fn + "(upper);").as_int());
				check_eq((std::numeric_limits<script_int>::min)(), eng->execute(fn + "(lower);").as_int());
				check_eq(script_int{0}, eng->execute(fn + "(0.0);").as_int());
			}
			check_eq(script_int{-3}, eng->execute("math::itrunc(-3.9);").as_int());
			check_eq(script_int{-4}, eng->execute("math::ifloor(-3.9);").as_int());
			check_eq(script_int{9223372036854775807LL}, eng->execute("math::ifloor(9223372036854775807);").as_int());
			check_eq(script_int{9007199254740993LL}, eng->execute("math::itrunc(9007199254740993);").as_int());
		});

		test("numeric_float_to_int_errors_are_catchable", [this]() {
			// an explicit conversion checks the range; an int slot takes no float at all
			const std::string out_of_range = "Cannot convert float to integer: non-finite or out of range";
			const std::pair<std::string, std::string> actions[] = {
				{"math::itrunc(source);", out_of_range}, {"math::ifloor(source);", out_of_range}, {"cast<int>(source);", out_of_range},
				{"int target = cast<int>(source);", out_of_range},
				{"class Number { var value = 0; int to_int() { return cast<int>(value); } } auto n = Number(); n.value = source; int target = n;", out_of_range},
				{"int target = source;", "Cannot initialize 'target' (declared int) with float"},
				{"int target = 7; target = source;", "Cannot assign float to 'target' (declared int)"},
				{"int accept(int x) { return x; } accept(source);", "Cannot pass float to parameter 'x' of 'accept' (declared int)"},
				{"int convert(var x) { return x; } convert(source);", "Cannot return float from 'convert' (declared -> int)"},
				{"class Box { int value = 7; } auto box = Box(); box.value = source;", "Cannot assign float to field 'value' of Box (declared int)"}};
			for (double input : {0x1p63, std::nextafter(-0x1p63, -INFINITY), 1e100, -1e100,
				std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
				std::numeric_limits<double>::quiet_NaN()}) {
				for (const auto& [action, expected] : actions) {
					auto eng = make_engine();
					eng->add_global("input", script_value(input, eng.get()));
					auto result = eng->execute("var source = input; string message = \"\"; try { " + action +
						" } catch (e) { message = e; } message;");
					check_eq(expected, result.as_string(), action);
					check_eq(script_int{42}, eng->execute("40 + 2;").as_int());
				}
			}
		});

		test("numeric_float_to_int_is_checked_at_container_boundaries", [this]() {
			// Review finding: the scalar slot rejected 1e100 while array<int> / map<string,int>
			// elements took it through an unchecked cast (INT64_MIN). An int element takes no float, in range or out, through any lane.
			const std::string element = "Cannot assign 'float' to element of type 'int'";
			const std::string into_array = "Cannot store float in array<int>";
			const std::string into_map = "Cannot store float in map<string,int>";
			const std::pair<std::string, std::string> actions[] = {
				{"array<int> a = [source];", into_array},
				{"array<int> a = [1, source, 3];", into_array},
				{"map<string, int> m = {\"x\": source};", into_map},
				{"array<int> a = []; a.push(source);", "Cannot push 'float' to array<int>"},
				{"array<int> a = [1]; a[0] = source;", element},
				{"array<int> a = [1]; a[0] += source;", element},
				{"var loose = [source]; array<int> a = loose;", into_array},
				{"var loose = {\"k\": source}; map<string, int> m = loose;", into_map},
				{"auto box = EmptyBox(); box.items = [source];", "Cannot assign array to field 'items' of EmptyBox (declared array<int>)"},
				{"array<int> take(array<int> xs) { return xs; } take([source]);", into_array},
				{"array<int> a = [1]; a[0] *= source;", element},
				{"array<int> a = [1]; var& r = a[0]; r = source;", element},
				{"array<int> a = [1]; var& r = a[0]; r += source;", element},
				{"array<int> a = [1]; for (auto& x : a) { x = source; }", element},
				{"array<array<int>> aa = [[1]]; aa[0][0] = source;", element},
				{"map<string, int> m = {\"x\": 1}; m[\"x\"] = source;", element},
				{"map<string, int> m = {\"x\": 1}; m[\"x\"] += source;", element},
				{"auto box = OneBox(); box.items[0] = source;", element}};
			for (double input : {1.5, 0x1p63, 1e100, -1e100, std::numeric_limits<double>::infinity(),
				std::numeric_limits<double>::quiet_NaN()}) {
				for (const auto& [action, expected] : actions) {
					// once at top level (environment slots) and once inside a function (frame slots,
					// the reference-lvalue kernel): the lanes differ, the verdict may not
					for (const std::string wrap : {"{0}", "void run() { {0} } run();"}) {
						auto eng = make_engine();
						eng->add_global("input", script_value(input, eng.get()));
						std::string body = wrap;
						body.replace(body.find("{0}"), 3, action);
						auto result = eng->execute("class EmptyBox { array<int> items = []; } class OneBox { array<int> items = [1]; } "
							"var source = input; string message = \"\"; try { " + body +
							" } catch (e) { message = e; } message;");
						check_eq(expected, result.as_string(), action + " / " + wrap);
						check_eq(script_int{42}, eng->execute("40 + 2;").as_int());
					}
				}
			}
			// a cast float is an int element, ints still widen into float elements
			auto eng = make_engine();
			check_eq(script_int{-2}, eng->execute("array<int> a = [cast<int>(1.9), cast<int>(-3.9)]; a[0] + a[1];").as_int());
			check_eq(script_int{-29}, eng->execute("array<int> a = [cast<int>(1.9), cast<int>(-3.9)]; a[0] * 10 + a[1] * 13;").as_int());
			check_near(3.0, eng->execute("array<float> f = [1, 2]; f[0] + f[1];").as_float(), 1e-12);
			check_eq(script_int{9223372036854775807LL}, eng->execute("array<int> a = [9223372036854775807]; a[0];").as_int());
		});

		test("nested_container_element_types_are_enforced", [this]() {
			// array<array<int>> used to type only the outer level: the inner arrays stayed
			// untagged, so a string or a float went in silently. The boundary
			// now recurses: inner nodes get their tag (typed buffers), so every store through
			// them runs the same rows as a flat array<int>.
			auto eng = make_engine();
			stdlib::register_all(*eng);
			auto msg = [&](const std::string& body) {
				// Keep the exact runtime boundary diagnostics under the approved static rules.
				return eng->execute("[[warn_mode]] string message = \"\"; try { " + body + " } catch (e) { message = e; } message;").as_string();
			};
			check_eq(std::string("Cannot store string in array<int>"), msg("array<array<int>> aa = [[\"x\"]];"));
			// an element store through the inner node is the flat array's store, text included
			const std::string flat_store = msg("array<int> a = [1]; a[0] = \"x\";");
			check_eq(std::string("Cannot assign 'string' to element of type 'int'"), flat_store);
			check_eq(flat_store, msg("array<array<int>> aa = [[1]]; aa[0][0] = \"x\";"));
			check_eq(std::string("Cannot store string in array<int>"), msg("array<array<int>> aa = [[1]]; aa.push([\"x\"]);"));
			check_eq(std::string("Cannot store string in array<int>"), msg("array<array<int>> aa = [[1]]; aa[0] = [\"x\"];"));
			const std::string flat_float_store = msg("array<int> a = [1]; a[0] = 1e100;");
			check_eq(std::string("Cannot assign 'float' to element of type 'int'"), flat_float_store);
			check_eq(flat_float_store, msg("array<array<int>> aa = [[1]]; aa[0][0] = 1e100;"));
			check_eq(std::string("Cannot store float in array<int>"), msg("array<array<int>> aa = [[1e100]];"));
			check_eq(std::string("Cannot store string in array<int>"), msg("map<string, array<int>> m = {\"k\": [\"x\"]};"));
			check_eq(flat_store, msg("map<string, array<int>> m = {\"k\": [1]}; m[\"k\"][0] = \"x\";"));
			check_eq(std::string("Cannot store string in map<string,int>"), msg("array<map<string, int>> am = [{\"k\": \"x\"}];"));
			const std::string flat_map_store = msg("map<string, int> m = {\"k\": 1}; m[\"k\"] = \"x\";");
			check_true(!flat_map_store.empty());
			check_eq(flat_map_store, msg("map<string, map<string, int>> mm = {\"a\": {\"k\": 1}}; mm[\"a\"][\"k\"] = \"x\";"));
			// the shapes that belong still work, and the inner nodes carry their type
			check_eq(std::string(""), msg("array<array<int>> aa = [[1, 2], [3]]; aa[0][0] = 7; aa.push([4, 5]); aa[1][0] += 1;"));
			check_eq(script_int{22}, eng->execute("array<array<int>> aa = [[1, 2], [3]]; aa[0][0] = 7; aa.push([4, 5]); aa[1][0] += 1; aa[0][0] + aa[0][1] + aa[1][0] + aa[2][0] + aa[2][1];").as_int());
			check_eq(std::string("Cannot assign 'string' to element of type 'int'"), msg("array<array<int>> aa = [[1]]; auto inner = aa[0]; inner[0] = \"x\";"));   // an inferred copy keeps the inner tag (a var slot is the dynamic hatch)
			check_eq(script_int{3}, eng->execute("map<string, array<int>> m = {\"k\": [1, true, 1]}; m[\"k\"][0] + m[\"k\"][1] + m[\"k\"][2];").as_int());
		});

		test("aggregate_copies_share_explicit_handles", [this]() {
			// C++ copy semantics for every aggregate copy: value members deep-copy, shared_ptr<T> members keep sharing their
			// object, on instances, arrays and maps alike; a direct handle copy shares as before.
			auto eng = make_engine();
			eng->execute(R"(
				class N { int v = 1; }
				class H { shared_ptr<N> p; N owned; H() { p = new N(); owned = N(); } }
				H a = H();
				H b = a;
				b.p.v = 5;
				b.owned.v = 50;
				shared_ptr<N> q = a.p;
				q.v = q.v + 2;
				array<shared_ptr<N>> xs = [a.p];
				array<shared_ptr<N>> ys = xs;
				ys[0].v = ys[0].v + 10;
				map<string, shared_ptr<N>> m1 = {"k": a.p};
				map<string, shared_ptr<N>> m2 = m1;
				m2["k"].v = m2["k"].v + 100;
				array<H> hs = [a];
				array<H> hs2 = hs;
				hs2[0].p.v = hs2[0].p.v + 1000;
				hs2[0].owned.v = 7;
			)");
			check_eq(script_int{1117}, eng->execute("a.p.v").as_int());          // 5 + 2 + 10 + 100 + 1000: one object throughout
			check_eq(script_int{1}, eng->execute("a.owned.v").as_int());         // the value member copied
			check_eq(script_int{50}, eng->execute("b.owned.v").as_int());
			check_eq(script_int{1}, eng->execute("hs[0].owned.v").as_int());
			check_true(eng->execute("same_object(a.p, b.p) && same_object(a.p, ys[0]) && same_object(a.p, m2[\"k\"]) && same_object(a.p, hs2[0].p)").as_bool());
			check_false(eng->execute("same_object(a.owned, b.owned)").as_bool());
			// nested value aggregates still copy all the way down
			check_eq(script_int{1}, eng->execute("class G { array<array<int>> grid = [[1]]; } G g1 = G(); G g2 = g1; g2.grid[0][0] = 9; g1.grid[0][0];").as_int());
			// weak handles observe the shared object too
			check_eq(script_int{3}, eng->execute("class W { weak_ptr<N> w; } W w1 = W(); shared_ptr<N> keep = new N(); keep.v = 3; w1.w = keep; W w2 = w1; w2.w.lock().v;").as_int());
		});

		test("string_values_share_their_node_until_written", [this]() {
			// Strings copy as a shared node at every boundary (literal, declaration, element
			// read, argument); every write goes through the copy-on-write gate, so the source
			// keeps its bytes: the literal evaluates to the same text on the next pass, the
			// element and the other variable are untouched, and the mutating builtins on a
			// copy never reach the original.
			auto eng = make_engine();
			eng->execute(R"(
				function tag() -> string { string s = "ab"; s += "c"; return s; }
				function again() -> string { string s = "ab"; return s; }
				var log = [];
				for (int i = 0; i < 3; ++i) { log.push(tag()); log.push(again()); }
				array<string> names = ["left", "right"];
				string n = names[0];
				n += "!";
				var v = names[1];
				v.to_upper();
				var w = names[1];
				w.reverse();
				string lit = "keep";
				var lit2 = lit;
				lit2.erase(0, 1);
				lit2.insert(0, "Z");
				string q = "seed";
				var parts = [];
				for (int i = 0; i < 3; ++i) { string p = "x"; if (i == 0) { p += "0"; } else if (i == 1) { p += "1"; } else { p += "2"; } parts.push(p); parts.push("x"); }
			)");
			check_eq(std::string("abc,ab,abc,ab,abc,ab"), eng->execute("log.join(\",\")").as<std::string>());
			check_eq(std::string("left!"), eng->execute("n").as<std::string>());
			check_eq(std::string("left"), eng->execute("names[0]").as<std::string>());
			check_eq(std::string("RIGHT"), eng->execute("v").as<std::string>());
			check_eq(std::string("thgir"), eng->execute("w").as<std::string>());
			check_eq(std::string("right"), eng->execute("names[1]").as<std::string>());
			check_eq(std::string("keep"), eng->execute("lit").as<std::string>());
			check_eq(std::string("Zeep"), eng->execute("lit2").as<std::string>());
			check_eq(std::string("keep"), eng->execute("\"keep\"").as<std::string>());
			check_eq(std::string("x0,x,x1,x,x2,x"), eng->execute("parts.join(\",\")").as<std::string>());
			check_eq(std::string("x"), eng->execute("\"x\"").as<std::string>());
		});

		test("script_objects_outliving_their_engine_destroy_safely", [this]() {
			// A host that keeps a script value past engine teardown (a UI page: a
			// signal receiver capturing a script object, destroyed after the engine) must be
			// able to drop it. Regression: the instance's destructor reached into its destroyed
			// class definition to unregister itself (a -loginshot 0xC0000005 in
			// class_definition::unregister_instance). The definition orphans its live
			// instances as it dies; dropping them afterwards is a plain destruction.
			std::optional<script_value> instance, closure, nested;
			{
				auto eng = make_engine();
				instance.emplace(eng->execute("class K { int v = 1; string s = \"x\"; } K k = K(); k;"));
				closure.emplace(eng->execute("class L { int w = 2; } L l = L(); var f = function() { return l.w; }; f;"));
				nested.emplace(eng->execute("class M { array<K> ks = [K(), K()]; map<string, L> ls = {\"a\": L()}; } M m = M(); m;"));
				check_true(instance->is_object());
			}
			instance.reset();
			nested.reset();
			closure.reset();
			// Counterexample: an explicit value capture keeps the class definition alive
			// past the engine through the captured environment, so the definition's destructor
			// never ran; the instance must still destroy without consulting the dead engine
			std::optional<script_value> callback, method_capture;
			{
				auto eng = make_engine();
				callback.emplace(eng->execute("class Held { int n = 7; } Held h = Held(); [h]() { return h.n; };"));
				method_capture.emplace(eng->execute("class Bag { array<Held> hs = [Held(), Held()]; var f() { var self = this; return [self]() { return self.hs.size(); }; } } Bag b = Bag(); b.f();"));
				check_true(callback->is_function());
			}
			callback.reset();
			method_capture.reset();
			check_true(true);
		});

		test("numeric_failed_store_preserves_value", [this]() {
			auto eng = make_engine();
			check_eq(script_int{14}, eng->execute(R"(
				var source = 1e100;
				int target = 7;
				class Box { int value = 7; }
				auto box = Box();
				try { target = source; } catch (e) {}
				try { box.value = source; } catch (e) {}
				target + box.value;
			)").as_int());
		});

		test("a_float_seed_converts_like_every_int_parameter", [this]() {
			// toward zero, saturating at the int64 ends, NaN as 0: the same seed on every platform
			auto eng = make_engine();
			jai::stdlib::register_all(*eng);
			const std::pair<double, script_int> inputs[] = {
				{-0.5, 0}, {0.0, 0}, {42.75, 42}, {-1.0, -1}, {0x1p63, INT64_MAX}, {1e100, INT64_MAX}, {-1e19, INT64_MIN},
				{std::numeric_limits<double>::infinity(), INT64_MAX}, {std::numeric_limits<double>::quiet_NaN(), 0},
			};
			for (const auto& [input, integer] : inputs) {
				eng->add_global("seed_input", script_value(input, eng.get()));
				eng->add_global("integer_seed", script_value(integer, eng.get()));
				check(eng->execute(R"(
					random reseeded;
					reseeded.seed(seed_input);
					random(seed_input).state == random(integer_seed).state && reseeded.state == random(integer_seed).state;
				)").as_bool(), std::to_string(input));
			}
		});

		test("saved_destructuring_rejects_missing_slots", [this]() {
			auto eng = make_engine();
			const source_location loc{};
			auto literal = std::make_shared<literal_expr>(loc, script_value(script_int{1}, eng.get()));
			auto array = std::make_shared<array_literal_expr>(loc, std::vector<expression_ptr>{literal});
			auto decl = std::make_shared<destructuring_decl>(loc, array);
			decl->names.emplace_back("item", detail::engine_access::symbolize(*eng, "item"));
			auto bytes = detail::serialize_jaibite({decl}, *detail::engine_access::get_symbolizer(*eng), 0);
			check_throws([&]() { eng->jaibite_load_bytes(bytes); });
			check_eq(script_int{42}, eng->execute("40 + 2;").as_int());
		});

		for (const std::string format : {"json", "binary", "base64"}) {
			test("nested_class_roundtrip_" + format, [this, format]() {
				auto eng = make_engine();
				stdlib::register_all(*eng);
				eng->execute(R"(
					class SavedPoint { int x = 17; }
					class SavedBox { var child = null; }
					auto box = SavedBox();
					box.child = SavedPoint();
					array<var> payload = [box, {"point": SavedPoint()}];
				)");
				eng->execute("var restored = from_" + format + "(to_" + format + "(payload));");
				check_eq(std::string("SavedBox"), eng->execute("type_of(restored[0]);").as_string());
				check_eq(std::string("SavedPoint"), eng->execute("type_of(restored[0].child);").as_string());
				check_eq(script_int{17}, eng->execute("restored[0].child.x;").as_int());
				check_eq(std::string("SavedPoint"), eng->execute("type_of(restored[1][\"point\"]);").as_string());
				check_eq(script_int{17}, eng->execute("restored[1][\"point\"].x;").as_int());
			});
		}

		test("native_second_base_shared_pointer", [this]() {
			auto eng = make_engine();
			prelaunch_detail::bind_native(*eng);
			auto child = std::make_shared<prelaunch_detail::native_child>();
			auto value = eng->make_object(child);
			auto base = value.as<std::shared_ptr<prelaunch_detail::second_base>>();
			check(base.get() == static_cast<prelaunch_detail::second_base*>(child.get()));
			check(!base.owner_before(child) && !child.owner_before(base));
		});

		test("json_maps_retain_string_key_type", [this]() {
			auto eng = make_engine();
			stdlib::register_all(*eng);
			auto result = eng->execute(R"(from_json("{\"nested\": {\"answer\": 42}}");)");
			check(result.get_type_info()->key_type() == type_info_ptr(detail::engine_access::get_type_info_string(*eng)));
			const auto& nested = result.as_map().at(script_value("nested", eng.get()));
			check(nested.get_type_info()->key_type() == type_info_ptr(detail::engine_access::get_type_info_string(*eng)));
			eng->add_global("decoded", result);
			check_throws([&]() { eng->execute("decoded[1] = 5;"); });
			check_throws([&]() { eng->execute("decoded[\"nested\"][1] = 5;"); });
		});

		test("native_checked_errors_do_not_throw", [this]() {
			auto eng = make_engine();
			prelaunch_detail::bind_native(*eng);
			auto wrong = eng->make_object(std::make_shared<prelaunch_detail::unrelated>());
			auto result = detail::checked_native_reference(wrong, typeid(prelaunch_detail::second_base));
			check_false(result.has_value());
			auto holder = wrong.get_object_holder();
			auto shared = detail::checked_native_object(eng.get(), *holder, typeid(prelaunch_detail::second_base));
			check_false(shared.has_value());
			check_eq(std::string("Object type mismatch"), std::string(shared.message()));
		});

		test("retired_native_layer_diagnostic_matches_all_adapters", [this]() {
			using namespace prelaunch_detail;
			auto eng = make_engine();
			bind_native(*eng);
			auto value = eng->make_object(std::make_shared<native_child>());
			value.as<strong_ptr<class_instance>>()->retire_script_layer();
			const std::string expected = "Script layer 'NativeChild' was detached (script_layer_detach, or a move to another engine): this handle no longer reaches its object";
			auto reference = detail::checked_native_reference(value, typeid(second_base));
			check_false(reference.has_value());
			check_eq(expected, eng->format_error(reference));
			auto shared = detail::checked_native_object(eng.get(), *value.get_object_holder(), typeid(second_base));
			check_false(shared.has_value());
			check_eq(expected, eng->format_error(shared));
			auto message = [](auto&& action) -> std::string {
				try { action(); } catch (const runtime_error& error) { return error.what(); }
				return "no error";
			};
			check_eq(expected, message([&]() { value.as<second_base&>(); }));
			check_eq(expected, message([&]() { value.as<const second_base&>(); }));
			check_eq(expected, message([&]() { value.as<std::shared_ptr<second_base>>(); }));
		});

		test("native_borrowed_argument_can_copy_without_ownership", [this]() {
			using namespace prelaunch_detail;
			auto eng = make_engine();
			bind_native(*eng);
			native_child child;
			eng->add_global("borrowed", script_value::make_cpp_reference("NativeChild", detail::engine_access::symbolize(*eng, "NativeChild"), &child, {}, eng.get()));
			eng->add_function("copy_second", [](second_base copy) { copy.second += 3; return copy.second; });
			eng->add_function("read_second", [](const second_base& base) { return base.second; });
			check_eq(script_int{25}, eng->execute("copy_second(borrowed);").as_int());
			check_eq(script_int{22}, eng->execute("read_second(borrowed);").as_int());
			check_eq(11, child.first);
		});

		test("native_second_base_typed_array", [this]() {
			auto eng = make_engine();
			prelaunch_detail::bind_native(*eng);
			auto child = std::make_shared<prelaunch_detail::native_child>();
			eng->add_global("native_child", child);
			auto value = eng->execute("array<shared_ptr<NativeChild>> children = [cast<shared_ptr<NativeChild>>(native_child)]; children;");   // naming the child as a value element copies it
			auto bases = value.as<std::vector<std::shared_ptr<prelaunch_detail::second_base>>>();
			check_eq(size_t{1}, bases.size());
			check(bases[0].get() == static_cast<prelaunch_detail::second_base*>(child.get()), "handle");
			child->second = 23;
			auto copied = eng->execute("array<NativeChild> copies = [native_child]; copies;").as<std::vector<std::shared_ptr<prelaunch_detail::second_base>>>();
			check_eq(size_t{1}, copied.size());
			check(copied[0].get() != static_cast<prelaunch_detail::second_base*>(child.get()), "copy");
			check_eq(23, copied[0]->second);
		});

		test("native_unrelated_pointer_rejected", [this]() {
			auto eng = make_engine();
			prelaunch_detail::bind_native(*eng);
			auto value = eng->make_object(std::make_shared<prelaunch_detail::unrelated>());
			check_throws([&]() { value.as<std::shared_ptr<prelaunch_detail::second_base>>(); });
		});

		test("native_inherited_property_uses_base_subobject", [this]() {
			auto eng = make_engine();
			prelaunch_detail::bind_native(*eng);
			check_eq(script_int{22}, eng->execute("auto child = NativeChild(); child.second;").as_int());
			check_eq(script_int{11}, eng->execute("child.second = 44; child.first;").as_int());
			check_eq(script_int{44}, eng->execute("child.second;").as_int());
		});

		test("native_base_reference_arguments", [this]() {
			auto eng = make_engine();
			prelaunch_detail::bind_native(*eng);
			auto child = std::make_shared<prelaunch_detail::native_child>();
			auto value = eng->make_object(child);
			check(&value.as<prelaunch_detail::second_base&>() == static_cast<prelaunch_detail::second_base*>(child.get()));
			check(&value.as<const prelaunch_detail::second_base&>() == static_cast<prelaunch_detail::second_base*>(child.get()));
			eng->add_global("child", child);
			eng->add_function("set_second", [](prelaunch_detail::second_base& base) { base.second = 47; });
			eng->add_function("read_second", [](const prelaunch_detail::second_base& base) { return base.second; });
			check_eq(script_int{47}, eng->execute("set_second(child); read_second(child);").as_int());
			check_eq(11, child->first);
		});

		test("native_virtual_and_transitive_base_casts", [this]() {
			using namespace prelaunch_detail;
			auto eng = make_engine();
			bind_native(*eng);
			dynamic_binder<native_grandchild>(*eng, "Grandchild").constructor<>().base_class<native_child>().build();
			dynamic_binder<virtual_child>(*eng, "VirtualChild").constructor<>().base_class<first_base>().base_class<second_base>().build();
			auto grandchild = std::make_shared<native_grandchild>();
			auto virtual_object = std::make_shared<virtual_child>();
			check(eng->make_object(grandchild).as<std::shared_ptr<second_base>>().get() == static_cast<second_base*>(grandchild.get()));
			check(eng->make_object(virtual_object).as<std::shared_ptr<second_base>>().get() == static_cast<second_base*>(virtual_object.get()));
			eng->add_global("grandchild", grandchild);
			eng->add_global("virtual_object", virtual_object);
			auto result = eng->execute("array<SecondBase> mixed = [grandchild, virtual_object]; mixed;")
				.as<std::vector<std::shared_ptr<second_base>>>();
			check_eq(size_t{2}, result.size());
			check_eq(22, result[0]->second);
			check_eq(22, result[1]->second);
		});

		test("native_abstract_base_and_unrelated_rejection", [this]() {
			using namespace prelaunch_detail;
			auto eng = make_engine();
			bind_native(*eng);
			dynamic_binder<abstract_base>(*eng, "AbstractBase").method("read", &abstract_base::read).build();
			dynamic_binder<abstract_child>(*eng, "AbstractChild").constructor<>().base_class<first_base>().base_class<abstract_base>().build();
			auto child = std::make_shared<abstract_child>();
			check(eng->make_object(child).as<std::shared_ptr<abstract_base>>().get() == static_cast<abstract_base*>(child.get()));
			auto wrong = eng->make_object(std::make_shared<unrelated>());
			check_throws([&]() { wrong.as<std::shared_ptr<abstract_base>>(); });
		});

		test("native_nonowning_base_reference", [this]() {
			using namespace prelaunch_detail;
			auto eng = make_engine();
			bind_native(*eng);
			auto child = std::make_shared<native_child>();
			auto value = script_value::make_cpp_reference("NativeChild", detail::engine_access::symbolize(*eng, "NativeChild"), child.get(), child, eng.get());
			check(&value.as<second_base&>() == static_cast<second_base*>(child.get()));
			auto shared = value.as<std::shared_ptr<second_base>>();
			check(shared.get() == static_cast<second_base*>(child.get()));
			check(!shared.owner_before(child) && !child.owner_before(shared));
			shared.reset();
			child.reset();
			check_throws([&]() { value.as<std::shared_ptr<second_base>>(); });
		});
	}
};

}

using prelaunch_regression_tests = jai::foundry::tests::prelaunch_regression_tests;
FOUNDRY_REGISTER(prelaunch_regression_tests)
