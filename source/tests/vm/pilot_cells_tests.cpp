// A closed typed loop's scalar slots run as raw cells inside one block. The contract controls: every
// shape lands the same on the interpreter and the vm: a miss right after the body-defined cell is
// written and after the first pixel store, late overflow with prior stores retained, a budget stop
// inside the cell loop, proofs off, zero and one iteration, a loop that is not closed, chained index
// stores, a float slot fed by int arithmetic, interleaved coroutines, the hoisted containers.
#include <filesystem>
#include <fstream>
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/detail/type_checker.hpp>
#include <jaiscript/vm/vm_compiler.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/vm/disassembler.hpp>
#include <jaiscript/vm/chunk_manifest.hpp>
#include <jaiscript/vm/region_ir.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <chrono>
#include <cstring>
#include <cmath>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class pilot_cells_tests : public suite {
public:
	pilot_cells_tests() : suite("Cell Loops") {}

	// The render column loop's shape over a var pixel array (so a string element can sit where a
	// store lands) and a var strip table. Returns "<gi*1000+y>:<weighted pixel sum>" or "throw:<text>".
	static constexpr const char* k_prelude = R"(
		var PIX = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
		var STRIPS = [[7, 8, 9, 10]];
		function paint(var& pix, var& strip, int cy0, int cy1, int vw, int px0, int tstep, int tacc0, int k) -> int {
			int gi = cy0 * vw + px0;
			int tacc = tacc0;
			int y = cy0;
			int acc = 0;
			while (y < cy1) {
				int c = strip[tacc >> 11];
				pix[gi] = c;
				pix[gi + 1] = c;
				tacc += tstep;
				gi += vw;
				acc += k;
				y = y + 1;
			}
			return gi * 1000 + y;
		}
		function weighted() -> int { int s = 0; for (int i = 0; i < PIX.size(); ++i) { if (type_of(PIX[i]) == "int") { s += PIX[i] * (i + 1); } } return s; }
		function run(int cy0, int cy1, int tstep, int k) -> string {
			var& s = STRIPS[0];
			string out = "";
			try { out = to_string(paint(PIX, s, cy0, cy1, 2, 0, tstep, 0, k)); }
			catch (err) { out = "throw:" + err; }
			return out + ":" + to_string(weighted());
		}
	)";

	static void register_host_math_fixture(engine& a_engine) {
		// Exercise the host alias optimization explicitly; stdlib no longer adds globals.
		a_engine.add_function("host_hypot", [](script_float a_x, script_float a_y) { return std::hypot(a_x,a_y); });
		a_engine.add_function("host_sqrt", [](script_float a_value) { return std::sqrt(a_value); });
		detail::engine_access::register_math_alias(a_engine, "host_sqrt", "sqrt");
		a_engine.add_function("host_floor", [](script_float a_value) { return std::floor(a_value); });
		detail::engine_access::register_math_alias(a_engine, "host_floor", "floor");
		a_engine.add_function("host_ceil", [](script_float a_value) { return std::ceil(a_value); });
		detail::engine_access::register_math_alias(a_engine, "host_ceil", "ceil");
		a_engine.add_function("host_abs", [](script_float a_value) { return std::abs(a_value); });
		detail::engine_access::register_math_alias(a_engine, "host_abs", "abs");
		a_engine.add_function("host_sin", [](script_float a_value) { return std::sin(a_value); });
		detail::engine_access::register_math_alias(a_engine, "host_sin", "sin");
		a_engine.add_function("host_cos", [](script_float a_value) { return std::cos(a_value); });
		detail::engine_access::register_math_alias(a_engine, "host_cos", "cos");
		a_engine.add_function("host_tan", [](script_float a_value) { return std::tan(a_value); });
		detail::engine_access::register_math_alias(a_engine, "host_tan", "tan");
		a_engine.add_function("host_degrees", [](script_float a_value) { return a_value * (180.0 / 3.14159265358979323846); });
		detail::engine_access::register_math_alias(a_engine, "host_degrees", "radians_to_degrees");
		a_engine.add_function("host_radians", [](script_float a_value) { return a_value * (3.14159265358979323846 / 180.0); });
		detail::engine_access::register_math_alias(a_engine, "host_radians", "degrees_to_radians");
	}

	std::string run_on(bool vm, const std::string& script, bool proofs = true) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
		if (vm) { e->max_call_depth(backend_type::vm, 10000); }   // a runaway recursion raises the cap text, as the interpreter does, inside the budget
		stdlib::register_all(*e); register_host_math_fixture(*e);
		if (!proofs) { e->type_proofs(false); }
		try { return e->execute(script).as_string(); }
		catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
	}

	// the VM's listing of a snippet (the closed loops and the refusals appear under each chunk)
	std::string listing_of(const std::string& source) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(backend_type::vm);
		stdlib::register_all(*e); register_host_math_fixture(*e);
		auto* symbolizer = detail::engine_access::get_symbolizer(*e);
		auto templates = detail::engine_access::get_registered_template_types(*e);
		jai::lexer lex(source, symbolizer);
		auto tokens = lex.tokenize();
		jai::parser p(tokens, symbolizer, e.get(), templates);
		auto parse_result = p.parse();
		if (!parse_result.has_value()) { return "<parse failed>"; }
		(void)jai::detail::run_static_check(*e, parse_result.value());
		jai::builtin_method_registries builtins; jai::init_builtin_method_registries(symbolizer, builtins);   // the call sites' builtin indexes, as the engine's compiler has them (a push site needs its array index)
		jai::vm::vm_compiler compiler(symbolizer, &builtins);
		compiler.ir_listing(true);
		return jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
	}
	// the listing of every chunk the engine's own compiler makes for a script (the checker's proofs stamped, so the grafts of the
	// inliner appear), through the disassembly sink; the script runs on the vm
	std::string listing_run(const std::string& source) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(backend_type::vm);
		stdlib::register_all(*e); register_host_math_fixture(*e);
		std::string out;
		e->disassembly_sink([&out](const std::string& listing, const std::string&) { out += listing; out += "\n"; });
		try { e->execute(source); } catch (const std::exception& ex) { out += std::string("throw:") + ex.what(); }
		return out;
	}
	// the compiled program of a snippet and its symbolizer (the manifest controls mutate a copy)
	struct compiled_snippet { std::shared_ptr<engine> eng; std::shared_ptr<jai::vm::chunk> program; std::shared_ptr<jai::vm::chunk> function; };   // function: the first function's body (its loops close; program-scope loops run over environment values)
	compiled_snippet compile_snippet(const std::string& source) {
		compiled_snippet c; c.eng = jai::foundry::make_default_engine();
		c.eng->set_backend(backend_type::vm);
		stdlib::register_all(*c.eng);
		auto* symbolizer = detail::engine_access::get_symbolizer(*c.eng);
		auto templates = detail::engine_access::get_registered_template_types(*c.eng);
		jai::lexer lex(source, symbolizer);
		auto tokens = lex.tokenize();
		jai::parser p(tokens, symbolizer, c.eng.get(), templates);
		auto parse_result = p.parse();
		if (!parse_result.has_value()) { return c; }
		(void)jai::detail::run_static_check(*c.eng, parse_result.value());
		jai::vm::vm_compiler compiler(symbolizer);
		compiler.ir_listing(true);
		c.program = compiler.compile_program(parse_result.value());
		for (const auto& d : parse_result.value()) {
			if (d && d->get_type() == node_type::function_decl) {
				auto* fd = static_cast<function_decl*>(d.get());
				c.function = compiler.compile_callable(fd->name, fd->parameters, fd->body, fd->local_count);
				break;
			}
		}
		return c;
	}
	void both(const std::string& script, const std::string& expected, const char* label, bool proofs = true) {
		const std::string interp = run_on(false, script, proofs);
		const std::string vm = run_on(true, script, proofs);
		check_eq(interp, vm, std::string(label) + ": both backends");
		if (!expected.empty()) { check_eq(expected, vm, std::string(label) + ": expected"); }
	}

	void forge_tests() override {
		test("nested_object_fields_close_scalar_update_loops", [this]() {
			const std::string listing = listing_run(R"(
				class Position { float x = 1.0; }
				class Item { Position pos = Position(); Position vel = Position(); }
				void update(array<Item>& items) {
					for (auto& item : items) { item.pos.x = item.pos.x + item.vel.x; }
				}
				array<Item> items = [Item(),Item()]; update(items);
			)");
			check_true(listing.find("C_MLOAD") != std::string::npos, "nested scalar reads use cell member sites");
			check_true(listing.find("C_MSTORE") != std::string::npos, "nested scalar writes use cell member sites");
			check_true(listing.find("-- open loop") == std::string::npos, "the update loop closes");
		});
		test("typed_class_references_close_with_guarded_rebinding", [this]() {
			const std::string code = R"(
				class Item { int n = 1; }
				int collect(array<Item>& input) {
					int total = 0;
					for (int i=0; i<2; ++i) { Item& item = input[i]; total += item.n; }
					return total;
				}
				array<Item> items = [Item(), Item()]; collect(items);
			)";
			const std::string listing = listing_run(code);
			check(listing.find("C_REBIND") != std::string::npos, "typed class reference stays in the cell loop");
			check(listing.find("throw:") == std::string::npos, "guarded loop executes successfully");
		});
		test("manifest_is_the_same_across_engines_and_names_no_runtime_state", [this]() {
			// two engines intern different ids for the same names; the manifest names symbols by spelling
			const std::string src = "float f = 1.0000001; function run() { int s = 0; int i = 0; while (i < 10) { s = s + i * 2; i = i + 1; } return s; } print(to_string(run()) + to_string(f));";
			auto a = compile_snippet(src); auto b = compile_snippet(src);
			check_true(a.program && b.program && a.function && b.function, "compiled");
			const std::string ma = jai::vm::chunk_manifest(*a.program, detail::engine_access::get_symbolizer(*a.eng)) + jai::vm::chunk_manifest(*a.function, detail::engine_access::get_symbolizer(*a.eng));
			const std::string mb = jai::vm::chunk_manifest(*b.program, detail::engine_access::get_symbolizer(*b.eng)) + jai::vm::chunk_manifest(*b.function, detail::engine_access::get_symbolizer(*b.eng));
			check_eq(ma, mb, "the same program and function manifest the same in two engines");
			check_true(ma.find("cell_loops 2") != std::string::npos, "the function's loop closed, and the frame program over its body");
		});
		test("manifest_changes_with_a_constants_low_bit", [this]() {
			// 1.0000001 and 1.0000002 never list the same; the manifest carries the constant's 64 bits
			auto a = compile_snippet("float f = 1.0000001; print(to_string(f));");
			check_true(static_cast<bool>(a.program), "compiled");
			const std::string before = jai::vm::chunk_manifest(*a.program, detail::engine_access::get_symbolizer(*a.eng));
			bool flipped = false;
			for (auto& c : a.program->constants) {
				if (c.raw_storage_index() != script_value::TYPEID_FLOAT) continue;
				double d = c.unchecked_as_float(); uint64_t bits = 0; std::memcpy(&bits, &d, sizeof bits); bits ^= 1u; std::memcpy(&d, &bits, sizeof d);
				c = script_value(d, static_cast<engine*>(nullptr)); flipped = true; break;
			}
			check_true(flipped, "a float constant was found");
			check_ne(before, jai::vm::chunk_manifest(*a.program, detail::engine_access::get_symbolizer(*a.eng)), "one low bit of a constant changes the manifest");
		});
		test("manifest_changes_with_a_live_mask_bit_and_an_origin_entry", [this]() {
			auto a = compile_snippet("function run() { int s = 0; int i = 0; while (i < 10) { s = s + i * 2; i = i + 1; } return s; } print(to_string(run()));");
			check_true(a.function && !a.function->cell_loops.empty(), "a closed loop in the function's body (a compound with a binary right side is a translation refusal; the plain store closes)");
			const std::string before = jai::vm::chunk_manifest(*a.function, detail::engine_access::get_symbolizer(*a.eng));
			auto& loop = a.function->cell_loops[0];
			check_true(!loop.live_temps.empty() && !loop.origin.empty(), "live masks and origins exist");
			loop.live_temps[0].w[0] ^= 1u;
			const std::string after_mask = jai::vm::chunk_manifest(*a.function, detail::engine_access::get_symbolizer(*a.eng));
			check_ne(before, after_mask, "one live mask bit changes the manifest");
			loop.live_temps[0].w[0] ^= 1u;
			loop.origin[0] += 1;
			check_ne(before, jai::vm::chunk_manifest(*a.function, detail::engine_access::get_symbolizer(*a.eng)), "one origin entry changes the manifest");
			loop.origin[0] -= 1;
			check_eq(before, jai::vm::chunk_manifest(*a.function, detail::engine_access::get_symbolizer(*a.eng)), "restored, the manifest is the same again");
		});
		test("column_loop_lands_the_same_on_both_backends", [this]() {
			both(std::string(k_prelude) + "run(0, 4, 2048, 1);", "8004:326", "four iterations");
		});
		test("zero_and_one_iteration", [this]() {
			both(std::string(k_prelude) + "run(4, 4, 2048, 1);", "8004:0", "zero iterations");
			both(std::string(k_prelude) + "run(0, 1, 2048, 1);", "2001:21", "one iteration");
		});
		test("late_overflow_keeps_prior_stores_and_the_text", [this]() {
			// acc += k with k = 2^62: the second addition overflows after the second iteration's stores
			both(std::string(k_prelude) + "run(0, 4, 2048, 4611686018427387904);", "", "late overflow");
			const std::string vm = run_on(true, std::string(k_prelude) + "run(0, 4, 2048, 4611686018427387904);");
			check_true(vm.rfind("throw:", 0) == 0, "the overflow raises: " + vm);
			check_true(vm.find(":77") != std::string::npos, "two iterations of stores retained: " + vm);
		});
		test("miss_right_after_the_body_defined_cell_is_written", [this]() {
			// the third strip element is a string: the cell load misses at the third iteration after two
			// complete iterations; the flushed state must be what the general declaration then rejects
			const std::string script = std::string(k_prelude) + "STRIPS[0][2] = \"x\"; run(0, 4, 2048, 1);";
			both(script, "", "string element on the third iteration");
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("throw:", 0) == 0, "the declaration rejects the string: " + vm);
			check_true(vm.find(":77") != std::string::npos, "two iterations of stores retained: " + vm);
		});
		test("miss_at_the_first_pixel_store_after_c_is_defined", [this]() {
			// a string sits where the third iteration's first store lands (a hetero target of another kind):
			// the cell store misses AFTER c was defined in its cell; the flush must materialize c before the
			// general store runs, and the general store then writes the int over the string
			const std::string script = std::string(k_prelude) + "PIX[4] = \"s\"; run(0, 4, 2048, 1);";
			both(script, "8004:326", "store miss after the cell definition of c");
		});
		test("miss_at_the_second_pixel_store", [this]() {
			const std::string script = std::string(k_prelude) + "PIX[5] = \"s\"; run(0, 4, 2048, 1);";
			both(script, "8004:326", "second store misses, first already landed raw");
		});
		test("getter_backed_reference_container_runs_the_getter_identically", [this]() {
			struct probe_box { script_int reads = 0; std::vector<script_int> values{0, 0, 0, 0, 0, 0, 0, 0}; };
			for (bool proofs : {true, false}) {
				std::string seen[2];
				for (int backend = 0; backend < 2; ++backend) {
					probe_box box;
					auto e = jai::foundry::make_default_engine();
					e->set_backend((backend == 1) ? backend_type::vm : backend_type::interpreter);
					e->type_proofs(proofs);
					stdlib::register_all(*e); register_host_math_fixture(*e);
					dynamic_binder<probe_box>(*e, "probe_box")
						.property("values", [](probe_box& b) { ++b.reads; return b.values; }, nullptr)
						.build();
					e->add_global_ref("box", box);
					const char* src = R"(
						var STRIPS = [[7, 8, 9, 10]];
						function paint(var& pix, var& strip, int cy0, int cy1, int vw, int px0, int tstep) -> int {
							int gi = cy0 * vw + px0; int tacc = 0; int y = cy0;
							while (y < cy1) { int c = strip[tacc >> 11]; pix[gi] = c; pix[gi + 1] = c; tacc += tstep; gi += vw; y = y + 1; }
							return gi * 1000 + y;
						}
						var& s = STRIPS[0];
						paint(box.values, s, 0, 3, 2, 0, 2048);
					)";
					std::string got;
					try { got = std::to_string(e->execute(src).as_int()); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
					seen[backend] = got + "|reads=" + std::to_string(box.reads);
				}
				check_eq(seen[0], seen[1], std::string("getter-backed container, proofs ") + (proofs ? "on" : "off"));
			}
		});
		test("proofs_off_lands_the_same", [this]() {
			both(std::string(k_prelude) + "run(0, 4, 2048, 1);", "8004:326", "proofs off", /*proofs*/ false);
		});
		test("budget_exhaustion_inside_the_cell_loop_raises_the_same_text", [this]() {
			// warmed by a short direct call under the default budget; only the long direct call runs
			// under the small budget; the loop is a closed arithmetic loop with no bounded writes
			const char* spin = "function spin(int n, int k) -> int { int y = 0; int acc = 0; while (y < n) { acc += k; y = y + 1; } return acc; }";
			std::string seen[2];
			for (int backend = 0; backend < 2; ++backend) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend((backend == 1) ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				e->execute(spin);
				script_value fn = e->get_variable("spin");
				std::vector<script_value> warm{ script_value(static_cast<script_int>(10), e.get()), script_value(static_cast<script_int>(1), e.get()) };
				auto warmed = fn.as_function()(warm);
				check_true(warmed.has_value() && warmed.value().as_int() == 10, "the warm call runs the loop ten times");
				std::vector<script_value> big{ script_value(static_cast<script_int>(200000000), e.get()), script_value(static_cast<script_int>(1), e.get()) };
				e->execution_budget(0.00001);
				auto result = fn.as_function()(big);
				seen[backend] = result.has_value() ? std::to_string(result.value().as_int()) : std::string("throw:") + std::string(result.message());
			}
			check_true(seen[0].rfind("throw:", 0) == 0, "interpreter raises the budget: " + seen[0]);
			check_true(seen[1].rfind("throw:", 0) == 0, "vm raises the budget: " + seen[1]);
			check_eq(seen[0], seen[1], "the same budget text");
		});
		test("values_survive_the_loop_exit_after_a_budget_free_run", [this]() {
			// the flushed live-outs are the general form's: y, tacc and gi after the loop
			const std::string script = R"(
				function walk(int n) -> string { int y = 0; int tacc = 5; int gi = 1; while (y < n) { tacc += 3; gi += 2; y = y + 1; } return to_string(y) + "," + to_string(tacc) + "," + to_string(gi); }
				walk(0) + "|" + walk(1) + "|" + walk(7);
			)";
			both(script, "0,5,1|1,8,3|7,26,15", "live-outs after exit");
		});
		test("float_cells_land_bit_exactly", [this]() {
			const std::string script = R"(
				function acc(int n) -> string { float x = 0.1; float s = 0.0; int i = 0; while (i < n) { s += x; x = x * 1.5; i = i + 1; } return to_string(s) + "," + to_string(x); }
				acc(0) + "|" + acc(3) + "|" + acc(12);
			)";
			both(script, "", "float payloads");
		});
		test("chained_index_store_folds_every_step", [this]() {
			// Control: pix[gi + 1 + offset] carries a chain step past the
			// store's first binary; the cell store folds every step exactly as the general lane does
			const std::string script = R"(
				array<int> pixels = [0, 0, 0, 0, 0, 0, 0, 0];
				array<int> strip = [7, 8, 9];
				function paint(var& pix, var& src, int offset) -> int {
					int y = 0; int gi = 0; int tacc = 0;
					while (y < 3) { int c = src[tacc >> 11]; pix[gi + 1 + offset] = c; tacc += 2048; gi += 2; y = y + 1; }
					return 0;
				}
				paint(pixels, strip, 1);
				to_string(pixels[2] * 3 + pixels[4] * 5 + pixels[6] * 7);
			)";
			both(script, "124", "chained index store");
		});
		test("late_chain_overflow_keeps_prior_stores_and_the_text", [this]() {
			// Control (the plain program gives "Integer overflow in '+'" and 49):
			// the first chained index lands in bounds through the final %8, the second iteration's addition
			// overflows in checked mode; the text and the prior store match the interpreter
			const std::string script = R"(
				array<int> pixels = [0, 0, 0, 0, 0, 0, 0, 0];
				array<int> strip = [7, 8, 9];
				function paint(var& pix, var& src, int offset) -> int {
					int y = 0; int gi = 0; int tacc = 0;
					while (y < 3) { int c = src[tacc >> 11]; pix[(gi + 1 + offset) % 8] = c; tacc += 2048; gi += 2; y = y + 1; }
					return 0;
				}
				string out = "";
				try { paint(pixels, strip, 9223372036854775805); out = "ok"; } catch (err) { out = "throw:" + err; }
				out + "|" + to_string(pixels[2] * 3 + pixels[4] * 5 + pixels[6] * 7);
			)";
			const std::string interp = run_on(false, script), vm = run_on(true, script);
			check_eq(interp, vm, "late chain overflow: both backends");
			check(vm.find("Integer overflow in '+'") != std::string::npos, "late chain overflow: the text");
			check(vm.size() >= 3 && vm.compare(vm.size() - 3, 3, "|49") == 0, "late chain overflow: the prior store survives (49)");
		});
		test("store_landing_into_a_slot_of_another_declared_kind_takes_the_general_form", [this]() {
			// f is a float slot fed by int arithmetic: the general landing converts through the slot's declared
			// type; a cell of int kind over that slot is refused at entry (the slot holds a float), so the loop
			// runs the general form and lands the same
			const std::string script = R"(
				function run(int n) -> string {
					float f = 0.0; int y = 0; int acc = 0;
					while (y < n) { f = y + 3; acc += y; y = y + 1; }
					return to_string(f) + ":" + type_of(f) + ":" + to_string(acc);
				}
				run(5);
			)";
			both(script, "", "float slot fed by int arithmetic");
		});
		test("interleaved_coroutines_each_running_a_closed_loop", [this]() {
			// two coroutines yield between closed loops: a frame copied into fiber state carries no raw area and a
			// record's area is its own, so the interleaving lands the same everywhere
			const std::string script = R"(
				coroutine walker(int start, int rounds) -> int {
					int total = start;
					for (int r = 0; r < rounds; ++r) {
						int y = 0; int acc = 0;
						while (y < 50) { acc += y * (r + 1); y = y + 1; }
						total += acc;
						yield total;
					}
					return total;
				}
				var a = walker(1, 4);
				var b = walker(1000, 4);
				string out = "";
				for (int k = 0; k < 4; ++k) { out = out + to_string(a.next()) + "/" + to_string(b.next()) + ";"; }
				out;
			)";
			both(script, "", "interleaved coroutines");
		});
		test("stores_through_the_hoisted_container_are_read_back_by_the_same_loop", [this]() {
			// pix is resolved once per entry; the loop reads what it wrote through the same node
			const std::string script = R"(
				function walk(var& pix, int n, int k) -> int {
					int gi = 0; int y = 0;
					while (y < n) { int c = pix[gi]; int d = c + k; pix[gi + 1] = d; gi += 1; y = y + 1; }
					return gi;
				}
				var PIX = [1, 0, 0, 0, 0, 0];
				int r = walk(PIX, 4, 3);
				string out = to_string(r); for (int i = 0; i < PIX.size(); ++i) { out = out + (i == 0 ? "|" : ",") + to_string(PIX[i]); }
				out;
			)";
			both(script, "4|1,4,7,10,13,0", "read back through the hoisted node");
		});
		test("a_second_call_with_other_containers_resolves_them_anew", [this]() {
			const std::string script = std::string(k_prelude) + R"(
				var PIX2 = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
				STRIPS.push([1, 2, 3, 4]);
				var& s0 = STRIPS[0]; var& s1 = STRIPS[1];
				int a = paint(PIX, s0, 0, 4, 2, 0, 2048, 0, 1);
				int b = paint(PIX2, s1, 0, 4, 2, 0, 2048, 0, 1);
				string out = to_string(a) + "/" + to_string(b) + "|" + to_string(PIX[6]) + "," + to_string(PIX2[6]);
				out;
			)";
			both(script, "8004/8004|10,4", "second call, other containers");
		});
		test("a_callee_rebinding_a_container_is_seen_by_the_landing_before_its_next_read", [this]() {
			// two calls per iteration: the container read after the first call is checked at that landing, whichever call rebinds it
			const char* shape = R"(
				var A = [1, 2, 3, 4, 5, 6, 7, 8];
				int calls = 0;
				function pick(int i) -> int { int r = i; if (r < 0) { r = 0; print("never"); } return r; }
				function churn(int i) -> int { calls += 1; if (calls == WHEN) { A = [100, 200, 300, 400, 500, 600, 700, 800]; } int r = 0; if (i < 0) { print("never"); } return r; }
				function run() -> int {
					int s = 0;
					for (int i = 0; i < 8; ++i) {
						FIRST
						s += A[a];
						SECOND
						s += b;
					}
					return s;
				}
				to_string(run());
			)";
			auto make = [&](const char* when, const char* first, const char* second) {
				std::string src = shape;
				src.replace(src.find("WHEN"), 4, when); src.replace(src.find("FIRST"), 5, first); src.replace(src.find("SECOND"), 6, second);
				return src;
			};
			both(make("3", "int a = pick(i);", "int b = churn(i);"), "3006", "the second call rebinds, the read follows the first landing");
			both(make("3", "int a = churn(i) + i;", "int b = pick(i) - i;"), "3303", "the first call rebinds, the read follows its own landing");
		});
		test("element_reads_inside_typed_expressions_land_the_same", [this]() {
			// acc = acc + data[idx]: the read lands in a compiler temp (T_LOAD), the add is typed, the loop closes
			const std::string script = R"(
				function run(int n) -> int {
					array<int> data = [3, 1, 4, 1, 5, 9, 2, 6];
					int acc = 0; int i = 0;
					while (i < n) { int idx = i % 8; acc = acc + data[idx]; i = i + 1; }
					return acc;
				}
				to_string(run(20));
			)";
			both(script, "71", "expression-form element read");
		});
		test("element_reads_from_a_var_array_land_and_a_string_element_falls_back", [this]() {
			const std::string script = R"(
				function run(var& data, int n) -> string {
					int acc = 0; int i = 0;
					string out = "";
					try { while (i < n) { int idx = i % 8; acc = acc + data[idx]; i = i + 1; } out = to_string(acc); }
					catch (err) { out = "throw:" + err + "|" + to_string(acc); }
					return out;
				}
				var D = [3, 1, 4, 1, 5, 9, 2, 6];
				string a = run(D, 20);
				D[5] = "s";
				string b = run(D, 20);
				a + " / " + b;
			)";
			both(script, "", "var container, then a string element on the sixth read");
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("71 / throw:", 0) == 0, "first pass 71, second raises: " + vm);
			check_true(vm.find("|14") != std::string::npos, "the sum before the string element is retained (3+1+4+1+5 = 14): " + vm);
		});
		test("an_out_of_range_element_read_inside_an_expression_raises_the_same", [this]() {
			const std::string script = R"(
				function run(int n) -> string {
					array<int> data = [10, 20, 30, 40];
					int acc = 0; int i = 0; string out = "";
					try { while (i < n) { int idx = i + 2; acc = acc + data[idx]; i = i + 1; } out = to_string(acc); }
					catch (err) { out = "throw:" + err + "|" + to_string(acc); }
					return out;
				}
				run(5);
			)";
			both(script, "", "out of range on the third read");
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("throw:", 0) == 0 && vm.find("|70") != std::string::npos, "30 + 40 retained, then the range error: " + vm);
		});
		test("two_element_reads_and_a_comparison_land_the_same", [this]() {
			const std::string script = R"(
				function run(int n, int limit) -> string {
					array<int> a = [1, 2, 3, 4, 5, 6]; array<int> b = [10, 20, 30, 40, 50, 60];
					int sum = 0; int count = 0; int i = 0;
					while (i < n) { int s = a[i] + b[i]; sum = sum + s; if (a[i] < limit) { count = count + 1; } i = i + 1; }
					return to_string(sum) + ":" + to_string(count);
				}
				run(6, 4);
			)";
			both(script, "231:3", "two reads, a comparison read");
		});
		test("float_element_reads_inside_expressions_land_bit_exactly", [this]() {
			const std::string script = R"(
				function run(int n) -> float {
					array<float> fd = [0.5, 1.25, -2.75, 3.125, 1e-3];
					float acc = 0.0; int i = 0;
					while (i < n) { int idx = i % 5; acc = acc + fd[idx]; i = i + 1; }
					return acc;
				}
				to_string(run(23));
			)";
			both(script, "", "float reads");
		});
		test("a_global_container_read_inside_an_expression_lands_the_same", [this]() {
			const std::string script = R"(
				array<int> G = [7, 8, 9, 10, 11];
				function run(int n) -> int { int acc = 0; int i = 0; while (i < n) { int idx = i % 5; acc = acc + G[idx]; i = i + 1; } return acc; }
				to_string(run(12));
			)";
			both(script, "105", "env-resident container");   // 45 + 45 + 7 + 8
		});
		test("the_expression_form_lowers_to_a_typed_load_and_the_loop_closes", [this]() {
			auto vm = jai::foundry::make_default_engine();
			vm->set_backend(backend_type::vm);
			stdlib::register_all(*vm);
			auto* symbolizer = detail::engine_access::get_symbolizer(*vm);
			const char* script = "var f = function(int n) { array<int> data = [3, 1, 4, 1, 5, 9, 2, 6]; int acc = 0; int i = 0;"
			                     " while (i < n) { int idx = i % 8; acc = acc + data[idx]; i = i + 1; } return acc; };";
			auto templates = detail::engine_access::get_registered_template_types(*vm);
			jai::lexer lex(script, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, vm.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*vm, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			check_true(listing.find("T_LOAD") != std::string::npos, "the element read is a typed load: " + listing);
			check_true(listing.find("CELLS_BACK") != std::string::npos, "the loop closes around it: " + listing);
		});
		test("a_computed_index_read_inside_an_expression_lands_the_same", [this]() {
			const std::string script = R"(
				function run(int n) -> int {
					array<int> data = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3];
					int acc = 0; int i = 0;
					while (i < n) { acc = acc + data[i + 2]; i = i + 1; }
					return acc;
				}
				to_string(run(8));
			)";
			both(script, "35", "computed index operand");   // 4+1+5+9+2+6+5+3
		});
		test("an_unknown_container_read_inside_an_expression_lands_and_falls_back_the_same", [this]() {
			// tp is a var& parameter: no proof, the read lands as any scalar; a string element mid-loop raises the same
			const std::string script = R"(
				function run(var& tp, int n) -> string {
					int acc = 0; int i = 0; string out = "";
					try { while (i < n) { acc = acc + tp[i]; i = i + 1; } out = to_string(acc); }
					catch (err) { out = "throw:" + err + "|" + to_string(acc); }
					return out;
				}
				var T = [[10, 20, 30, 40, 50]];
				var& t0 = T[0];
				string a = run(t0, 5);
				t0[3] = "x";
				string b = run(t0, 5);
				a + " / " + b;
			)";
			both(script, "", "unknown container operand");
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("150 / throw:", 0) == 0 && vm.find("|60") != std::string::npos, "150, then the string element after 60: " + vm);
		});
		test("an_element_to_element_copy_lands_the_same", [this]() {
			const std::string script = R"(
				function copy(var& dst, var& src, int n) -> int { int i = 0; while (i < n) { dst[i] = src[i]; i = i + 1; } return i; }
				array<int> A = [0, 0, 0, 0]; array<int> B = [7, 8, 9, 10];
				var V = [0, 0, 0, 0]; var W = [1, "two", 3.5, 4];
				int a = copy(A, B, 4); int b = copy(V, W, 4);
				to_string(a) + to_string(b) + "|" + to_string(A[3]) + "," + to_string(V[1]) + "," + to_string(V[2]);
			)";
			both(script, "44|10,two,3.5", "element-to-element copies, typed and var");
		});
		test("a_nested_index_read_lands_the_same", [this]() {
			const std::string script = R"(
				function run(int n) -> int {
					array<int> lut = [5, 6, 7, 8]; array<int> idx = [3, 1, 0, 2, 1];
					int acc = 0; int i = 0;
					while (i < n) { acc = acc + lut[idx[i]]; i = i + 1; }
					return acc;
				}
				to_string(run(5));
			)";
			both(script, "32", "nested index");   // 8+6+5+7+6
		});
		test("an_operand_that_could_raise_first_keeps_the_interpreter_order", [this]() {
			// `missing + data[9]`: the undefined global raises before the out-of-range read on both backends
			const std::string script = R"(
				function run() -> string {
					array<int> data = [1, 2, 3];
					string out = "";
					try { int v = missing + data[9]; out = to_string(v); } catch (err) { out = "throw:" + err; }
					return out;
				}
				run();
			)";
			both(script, "", "error order with an env partner");
		});
		test("a_reference_alias_of_a_typed_global_proves_its_element_reads", [this]() {
			// var& g = G with G an array<int>: the alias's reads are proven int, the loop closes as a cell block
			auto vm = jai::foundry::make_default_engine();
			vm->set_backend(backend_type::vm);
			stdlib::register_all(*vm);
			auto* symbolizer = detail::engine_access::get_symbolizer(*vm);
			const char* scripts[2] = {
				"array<int> G = [3, 1, 4, 1, 5, 9, 2, 6]; var f = function(int n) { var& g = G; int acc = 0; int i = 0;"
				" while (i < n) { int idx = i % 8; acc = acc + g[idx]; i = i + 1; } return acc; };",
				"array<int> G = [3, 1, 4, 1, 5, 9, 2, 6]; var f = function(int n) { int acc = 0; int i = 0;"
				" while (i < n) { int idx = i % 8; acc = acc + G[idx]; i = i + 1; } return acc; };" };
			for (const char* script : scripts) {
			auto templates = detail::engine_access::get_registered_template_types(*vm);
			jai::lexer lex(script, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, vm.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*vm, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			check_true(listing.find("T_LOAD") != std::string::npos && listing.find("CELLS_BACK") != std::string::npos, "alias read typed and the loop closed: " + listing);
			}
			const std::string run = R"(
				array<int> G = [3, 1, 4, 1, 5, 9, 2, 6];
				function run(int n) -> int { var& g = G; int acc = 0; int i = 0; while (i < n) { int idx = i % 8; acc = acc + g[idx]; i = i + 1; } return acc; }
				to_string(run(20));
			)";
			both(run, "71", "alias read lands the same");
		});
		test("an_element_copy_between_aliases_lands_the_same_and_a_reassigned_global_converts", [this]() {
			// pix and pal alias typed globals; PIX is reassigned between calls (converted into array<int> by its declaration)
			const std::string script = R"(
				array<int> PIX = [0, 0, 0, 0]; array<int> PAL = [9, 8, 7, 6];
				function blit(int n) -> int { var& pix = PIX; var& pal = PAL; int i = 0; while (i < n) { pix[i] = pal[i]; i = i + 1; } return pix[0] * 1000 + pix[n - 1]; }
				int a = blit(4);
				PIX = [1, 1, 1, 1, 1, 1];
				int b = blit(3);
				to_string(a) + "|" + to_string(b) + "|" + to_string(PIX[3]) + "," + to_string(PIX.size());
			)";
			both(script, "9006|9007|1,6", "alias to alias copy, reassigned global");
		});
		test("a_store_to_a_typed_parameter_takes_the_typed_row_and_the_loop_closes", [this]() {
			// v = v + dv with both declared int parameters: a parse-proven typed store, so the loop is a cell block
			auto vm = jai::foundry::make_default_engine();
			vm->set_backend(backend_type::vm);
			stdlib::register_all(*vm);
			auto* symbolizer = detail::engine_access::get_symbolizer(*vm);
			const char* script = "var g = function(int v, int dv, int n) { int i = 0; while (i < n) { v = v + dv; i = i + 1; } return v; };";
			auto templates = detail::engine_access::get_registered_template_types(*vm);
			jai::lexer lex(script, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, vm.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*vm, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			check_true(listing.find("BINARY_FUSED_STORE") == std::string::npos && listing.find("CELLS_BACK") != std::string::npos,
			           "the parameter store is typed and the loop closed: " + listing);
			both("function g(int v, int dv, int n) -> int { int i = 0; while (i < n) { v = v + dv; i = i + 1; } return v; } to_string(g(1, 3, 20));",
			     "61", "parameter store lands the same");
			// the column loop's shape: the parameter advances by a parameter each row, a negative texel row wraps
			const std::string column = R"(
				function col(int y0, int y1, int vfx, int dvfx, int th) -> int {
					int acc = 0;
					for (int y = y0; y <= y1; ++y) { int iv = (vfx >> 16) % th; if (iv < 0) { iv = iv + th; } acc = acc + iv; vfx = vfx + dvfx; }
					return acc;
				}
				to_string(col(0, 9, 0, 196608, 7)) + "," + to_string(col(0, 3, -131072, 65536, 7));
			)";
			both(column, "30,12", "column loop over parameters");
			// a parameter bound by reference downstream (its storage is a cell): the alias sees every store
			both("function k(int v, int dv, int n) -> int { var& a = v; int i = 0; while (i < n) { v = v + dv; i = i + 1; } return a; } to_string(k(1, 3, 20));",
			     "61", "reference-escaping parameter");
			// a bool argument converts on entry: the parameter slot holds the declared int before the first store; a var's float is refused there
			both("function g(int v, int dv, int n) -> int { int i = 0; while (i < n) { v = v + dv; i = i + 1; } return v; } var f = 1.0; string r = to_string(g(true, 3, 2)); try { r = r + \",\" + to_string(g(f, 3, 20)); } catch (err) { r = r + \",\" + err; } r;",
			     "7,Cannot pass float to parameter 'v' of 'g' (declared int)", "converted arguments");
			// array<int> parameters: the element class classifies a read, the store into an int parameter proves
			both("function s(array<int> t, int acc, int n) -> int { int i = 0; while (i < n) { acc = acc + t[i % 4]; i = i + 1; } return acc; } to_string(s([1, 2, 3, 4], 0, 10));",
			     "23", "typed array parameter");
		});
		test("a_hoisted_read_keeps_the_interpreter_order_past_a_property_reference", [this]() {
			// a var& bound to a C++ property dereferences through its getter; the interpreter evaluates that operand
			// before an element read that follows it, so the getter's error must come first on both backends
			struct order_box { bool armed = false; std::vector<script_int> values{5, 6, 7, 8}; script_int n = 3; };
			struct shape_case { const char* text; bool getter_first; };
			const shape_case shapes[5] = {
				{"function probe(int k, int i) -> int { var& p = box.values; box.arm(); return p[k + Q[i % 16]]; }", true},   // index-read arm
				{"function probe(int k, int i) -> int { var& p = box.values; box.arm(); return p[Q[i % 16]]; }", true},       // nested-index arm
				{"function probe(int k, int i) -> int { var& s = box.n; box.arm(); return s + Q[i % 16]; }", true},           // flat pre-step
				{"function probe(int k, int i) -> int { var& s = box.n; box.arm(); return s + Q[i % 16] * 2; }", true},       // the lift
				{"function probe(int k, int i) -> int { var& p = box.values; box.arm(); p[k] = Q[i % 16]; return 1; }", false} // a store: its right-hand side comes first
			};
			for (const shape_case& sc : shapes) {
				const char* shape = sc.text;
				std::string seen[2];
				for (int backend = 0; backend < 2; ++backend) {
					order_box box;
					auto e = jai::foundry::make_default_engine();
					e->set_backend((backend == 1) ? backend_type::vm : backend_type::interpreter);
					stdlib::register_all(*e); register_host_math_fixture(*e);
					dynamic_binder<order_box>(*e, "order_box")
						.property("values", [](order_box& b) { if (b.armed) { throw std::runtime_error("GETTER_FIRST"); } return b.values; }, nullptr)
						.property("n", [](order_box& b) { if (b.armed) { throw std::runtime_error("GETTER_FIRST"); } return b.n; }, nullptr)
						.method("arm", [](order_box& b) { b.armed = true; })
						.build();
					e->add_global_ref("box", box);
					const std::string src = std::string("array<int> Q = [1, 2];\n") + shape +
						"\nstring out = \"\"; try { out = to_string(probe(0, 9)); } catch (err) { out = \"throw:\" + err; } out;";
					try { seen[backend] = e->execute(src).as_string(); } catch (const std::exception& ex) { seen[backend] = std::string("throw:") + ex.what(); }
				}
				check_eq(seen[0], seen[1], std::string("evaluation order: ") + shape);
				check_eq(sc.getter_first, seen[1].find("GETTER_FIRST") != std::string::npos, std::string("which error comes first: ") + seen[1]);
			}
		});
		test("a_reference_declaration_keeps_a_function_on_the_lazy_env_path", [this]() {
			// var& g = G lands in a frame slot: the body needs no per-call scope environment
			auto vm = jai::foundry::make_default_engine();
			vm->set_backend(backend_type::vm);
			stdlib::register_all(*vm);
			auto* symbolizer = detail::engine_access::get_symbolizer(*vm);
			const char* scripts[2] = {
				"array<int> G = [1, 2, 3, 4]; int H = 7; var f = function(int n) { var& g = G; int acc = 0;"
				" for (int i = 0; i < n; ++i) { acc = acc + g[i % 4] + H; } return acc; };",
				"array<int> G = [1, 2, 3, 4]; var f = function(int n) { int acc = 0;"
				" for (int i = 0; i < n; ++i) { auto& e = G[i % 4]; acc = acc + e; } return acc; };" };
			for (const char* script : scripts) {
				auto templates = detail::engine_access::get_registered_template_types(*vm);
				jai::lexer lex(script, symbolizer);
				auto tokens = lex.tokenize();
				jai::parser p(tokens, symbolizer, vm.get(), templates);
				auto parse_result = p.parse();
				check_true(parse_result.has_value(), "snippet parses");
				(void)jai::detail::run_static_check(*vm, parse_result.value());
				jai::vm::vm_compiler compiler(symbolizer);
				const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
				check_true(listing.find("(env: lazy)") != std::string::npos, "the body is lazy: " + listing);
			}
			// the aliases read and write through on both backends: a global, a parameter, a boxed local, an element
			const std::string run = R"(
				array<int> G = [1, 2, 3, 4]; int H = 7;
				function f(int n) -> int { var& g = G; int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + g[i % 4] + H; } return acc; }
				function g(int v) -> int { var& a = v; a = a + 1; int w = 3; var& b = w; b = b * 2; return v * 100 + w; }
				function r(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { var& e = G[i % 4]; s = s + e; e = e + 1; } return s; }
				function fact(int n) -> int { var& g = G; if (n <= 1) { return g[0]; } return n * fact(n - 1); }
				function outer() -> int { auto k = [](int x) -> int { return x; }; return f(4) + k(1); }
				string out = to_string(f(10)) + "," + to_string(g(4)) + "," + to_string(r(8)) + "," + to_string(G[0]) + "," + to_string(fact(5)) + "," + to_string(outer());
				G = [9, 9];
				out + "," + to_string(f(2));
			)";
			both(run, "93,506,24,3,360,47,32", "aliases through lazy frames");
			// a method's alias of its own field (the sticky method scope keeps the same rule)
			const std::string method = R"(
				class C { int hp = 10; int hit(int d) { var& h = hp; h = h - d; return hp; } }
				C c = C();
				to_string(c.hit(3)) + "," + to_string(c.hit(2));
			)";
			both(method, "7,5", "field alias in a method");
			// an alias of an undefined name raises the same text on both backends
			both("[[warn_mode]] function u() -> int { var& z = nowhere; return 1; } string out = \"\"; try { out = to_string(u()); } catch (e) { out = \"throw:\" + e; } out;", "", "undefined referent");
		});
		test("a_member_shadowing_a_same_named_global_keeps_its_getter_ahead_of_a_hoisted_read", [this]() {
			// same-unit `array<int> values`, a script class over a native class whose property is also `values`:
			// inside the method the bare name is the native property, so its getter still runs before the read
			struct shadow_box { bool armed = false; std::vector<script_int> values{5, 6, 7, 8}; };
			std::string seen[2];
			for (int backend = 0; backend < 2; ++backend) {
				shadow_box box;
				auto e = jai::foundry::make_default_engine();
				e->set_backend((backend == 1) ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				dynamic_binder<shadow_box>(*e, "shadow_box")
					.constructor<>()
					.property("values", [](shadow_box& b) { if (b.armed) { throw std::runtime_error("GETTER_FIRST"); } return b.values; }, nullptr)
					.method("arm", [](shadow_box& b) { b.armed = true; })
					.build();
				e->add_global_ref("box", box);
				const char* src = R"(
					array<int> values = [1, 2, 3, 4];
					array<int> Q = [1, 2];
					class Probe : shadow_box {
						int probe(int k, int i) { var& p = values; arm(); return p[Q[i % 16]]; }
					}
					var pr = shadow_box();
					extend(pr, Probe);
					string out = "";
					try { out = to_string(pr.probe(0, 9)); } catch (err) { out = "throw:" + err; }
					out;
				)";
				try { seen[backend] = e->execute(src).as_string(); } catch (const std::exception& ex) { seen[backend] = std::string("throw:") + ex.what(); }
			}
			check_eq(seen[0], seen[1], "member shadowing a global: both backends");
			check_true(seen[1].find("GETTER_FIRST") != std::string::npos || seen[1].find("throw:") != 0,
			           "the getter's error comes first when the member is the referent: " + seen[1]);
		});
		test("a_read_inside_an_index_expression_lands_in_a_temp_and_the_outer_read_fuses", [this]() {
			// p[k + Q[i % 8]]: the inner read lands in a temp first, the outer read takes the T_LOAD row with the temp
			// as an index operand, and the loop closes as a cell block
			auto vm = jai::foundry::make_default_engine();
			vm->set_backend(backend_type::vm);
			stdlib::register_all(*vm);
			auto* symbolizer = detail::engine_access::get_symbolizer(*vm);
			const char* script =
				"array<int> P = [10, 20, 30, 40, 50, 60, 70, 80]; array<int> Q = [1, 2, 3, 0, 1, 2, 3, 0];"
				" var f = function(int n) { var& p = P; int acc = 0; int i = 0;"
				" while (i < n) { int k = i % 4; acc = acc + p[k + Q[i % 8]]; i = i + 1; } return acc; };";
			auto templates = detail::engine_access::get_registered_template_types(*vm);
			jai::lexer lex(script, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, vm.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*vm, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			check_true(listing.find("T_LOAD") != std::string::npos && listing.find("CELLS_BACK") != std::string::npos,
			           "both reads typed and the loop closed: " + listing);
			// the read on either side, or on both sides (left before right); values land the same on both backends
			const std::string run = R"(
				array<int> P = [10, 20, 30, 40, 50, 60, 70, 80]; array<int> Q = [1, 2, 3, 0, 1, 2, 3, 0];
				function r1(int n) -> int { var& p = P; int acc = 0; int i = 0; while (i < n) { int k = i % 4; acc = acc + p[k + Q[i % 8]]; i = i + 1; } return acc; }
				function r2(int n) -> int { var& p = P; int acc = 0; int i = 0; while (i < n) { int k = i % 4; acc = acc + p[Q[i % 8] + k]; i = i + 1; } return acc; }
				function r3(int n) -> int { var& p = P; int acc = 0; int i = 0; while (i < n) { acc = acc + p[Q[i % 8] + Q[(i + 1) % 8]]; i = i + 1; } return acc; }
				to_string(r1(20)) + "," + to_string(r2(21)) + "," + to_string(r3(22));
			)";
			both(run, "800,820,900", "nested reads land the same");
			// error order: the inner read's failure comes first, then the outer's; the same texts on both backends
			const std::string probe = R"(
				array<int> P = [1, 2, 3, 4]; array<int> Q = [0, 1];
				function f(int a, int b) -> int { var& p = P; return p[a + Q[b]]; }
				string out = "";
				try { out = to_string(f(1, 9)); } catch (e) { out = "throw:" + e; }
				try { out = out + "|" + to_string(f(9, 1)); } catch (e) { out = out + "|throw:" + e; }
				out;
			)";
			both(probe, "", "inner then outer raise");
			const std::string vm_probe = run_on(true, probe);
			check_true(vm_probe.rfind("throw:", 0) == 0 && vm_probe.find("|throw:") != std::string::npos, "both reads raise in turn: " + vm_probe);
			// an undefined outer container raises before the inner read (the container must be a frame slot for the hoist)
			const std::string outer_missing = R"(
				[[warn_mode]] array<int> Q = [1, 2];
				function probe(int k, int i) -> int { return missing_outer[k + Q[i % 16]]; }
				string out = "";
				try { out = to_string(probe(0, 9)); } catch (e) { out = "throw:" + e; }
				out;
			)";
			both(outer_missing, "", "undefined outer container raises first");
			const std::string vm_missing = run_on(true, outer_missing);
			check_true(vm_missing.find("missing_outer") != std::string::npos, "the outer name is the error: " + vm_missing);
		});
		test("a_loop_with_a_break_is_not_closed_and_still_lands_the_same", [this]() {
			const std::string script = R"(
				array<int> PIX = [0, 0, 0, 0, 0, 0, 0, 0];
				var STRIPS = [[7, 8, 9, 10]];
				function paint(var& pix, var& strip, int cy1) -> int {
					int gi = 0; int tacc = 0; int y = 0;
					while (y < cy1) { int c = strip[tacc >> 11]; if (c == 9) { break; } pix[gi] = c; tacc += 2048; gi += 2; y = y + 1; }
					return gi * 1000 + y;
				}
				var& s = STRIPS[0];
				to_string(paint(PIX, s, 4)) + ":" + to_string(PIX[0]) + to_string(PIX[2]) + to_string(PIX[4]);
			)";
			both(script, "4002:780", "break inside the loop");
		});
		test("a_statement_increment_lowers_to_the_typed_row_and_the_loop_closes", [this]() {
			// `++y` / `y--` in statement position on a frame slot: the int compound row (`y += 1`'s landing);
			// a var slot holding a float, a shared cell, a reference and the overflow text take the general op
			const char* values =
				"function up(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s += i; } int j = n; while (j > 0) { j--; s += j; } return s; }"
				"function post(int n) -> int { int i = 0; int t = 0; while (i < n) { t = i++; } return t * 100 + i; }"
				"function fl() -> string { var f = 1.5; ++f; f--; ++f; return to_string(f); }"
				"function viaref() -> int { int x = 1; int& r = x; ++r; r++; --r; return x * 10 + r; }"
				"function ovf() -> string { int m = 9223372036854775807; try { ++m; } catch (err) { return \"throw:\" + err; } return to_string(m); }"
				"function unf() -> string { int m = -9223372036854775807 - 1; try { m--; } catch (err) { return \"throw:\" + err; } return to_string(m); }"
				"function boxed() -> int { int c = 0; var bump = [&]() { ++c; }; bump(); bump(); return c; }"
				"function dyn() -> int { var v = 5; ++v; v++; return v; }"
				"to_string(up(10)) + \",\" + to_string(post(3)) + \",\" + fl() + \",\" + to_string(viaref()) + \",\" + ovf() + \",\" + unf() + \",\" + to_string(boxed()) + \",\" + to_string(dyn());";
			both(values, "", "increments");
			both(values, "", "increments, proofs off", false);
			const std::string vm = run_on(true, values);
			check_true(vm.rfind("90,203,2.5,22,throw:", 0) == 0, "typed, postfix, float and reference shapes: " + vm);
			check_true(vm.find("Integer overflow in '++'") != std::string::npos && vm.find("Integer overflow in '--'") != std::string::npos, "the general op's overflow texts: " + vm);
			check_true(vm.size() >= 4 && vm.compare(vm.size() - 4, 4, ",2,7") == 0, "the shared cell and the var slot: " + vm);
			auto e = jai::foundry::make_default_engine();
			e->set_backend(backend_type::vm);
			stdlib::register_all(*e); register_host_math_fixture(*e);
			auto* symbolizer = detail::engine_access::get_symbolizer(*e);
			const char* script = "array<int> PIX = [0, 0, 0, 0]; var S = [[1, 2, 3]];"
			                     " var f = function(var& pix, var& strip, int cy0, int cy1, int vw, int px0, int tstep) {"
			                     " int gi = cy0 * vw + px0; int tacc = 0; int y = cy0;"
			                     " while (y < cy1) { int c = strip[tacc >> 11]; pix[gi] = c; pix[gi + 1] = c; tacc += tstep; gi += vw; ++y; }"
			                     " int z = cy1; while (z > cy0) { pix[gi] = z; z--; }"
			                     " return gi; };";
			auto templates = detail::engine_access::get_registered_template_types(*e);
			jai::lexer lex(script, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, e.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*e, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			check_true(listing.find("INCDEC") == std::string::npos, "no general increment remains: " + listing);
			check_true(listing.find("T_ADD_II") != std::string::npos && listing.find("T_SUB_II") != std::string::npos, "++ and -- take the int rows: " + listing);
			check_true(listing.find("CELLS_BACK") != std::string::npos, "the column loop still closes with ++y: " + listing);
		});
		test("a_read_through_an_unproven_container_takes_the_typed_row_and_falls_back_the_same", [this]() {
			// pal[base + tp[i + j]] with both containers var& parameters (a computed inner index, JaiDOOM's texture shape): the checker cannot class the index binary, the
			// outer read takes the T_LOAD row on the element-read temp anyway; a map container, a float element and a
			// string element miss into the general op with its own results and texts
			const std::string script = R"(
				function f(var& tp, var& pal, int n, int j) -> string {
					int s = 0;
					try { for (int i = 0; i < n; ++i) { int base = i * 2; s = s + pal[base + tp[i + j]]; } }
					catch (err) { return "throw:" + err; }
					return to_string(s);
				}
				var T = [0, 1, 1]; var P = [10, 20, 30, 40, 50, 60]; var M = {0: 10, 3: 40, 5: 60};
				var F = [0, 1.5, 1]; var S = [0, "x", 1];
				f(T, P, 3, 0) + "," + f(T, M, 3, 0) + "," + f(F, P, 3, 0) + "," + f(S, P, 3, 0);
			)";
			both(script, "", "unproven index binary");
			both(script, "", "unproven index binary, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("110,110,", 0) == 0, "the array and the map land the same sum: " + vm);
			auto e = jai::foundry::make_default_engine();
			e->set_backend(backend_type::vm);
			stdlib::register_all(*e); register_host_math_fixture(*e);
			auto* symbolizer = detail::engine_access::get_symbolizer(*e);
			const char* fn = "var f = function(var& tp, var& pal, int n, int j) { int s = 0; for (int i = 0; i < n; ++i) { int base = i * 2; s = s + pal[base + tp[i + j]]; } return s; };";
			auto templates = detail::engine_access::get_registered_template_types(*e);
			jai::lexer lex(fn, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, e.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*e, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			size_t loads = 0; for (size_t at = listing.find("T_LOAD"); at != std::string::npos; at = listing.find("T_LOAD", at + 6)) { ++loads; }
			check_true(loads >= 2 && listing.find("INDEX_FUSED_DECL") == std::string::npos, "inner and outer reads both take the typed row: " + listing);
		});
		test("a_math_intrinsic_result_lands_in_a_temp_and_the_binary_takes_the_typed_row", [this]() {
			// JaiDOOM's flat-column shapes: an intrinsic inside a typed binary, inside an index expression, in a float
			// expression; the kernel's errors keep their place ahead of and behind a hoisted element read
			const std::string script = R"(
				function band(int bb, float z) -> int { int b = bb + math::itrunc(z * 0.001953125); if (b < 0) { b = 0; } if (b > 31) { b = 31; } return b; }
				function texel(var& flat, float wx, float wy) -> int { return flat[((math::itrunc(wy) & 63) << 6) + (math::itrunc(wx) & 63)]; }
				function fl(float x) -> float { return math::sqrt(x) * 2.0 + math::floor(x); }
				function ints(int i) -> int { return math::itrunc(i) + math::abs(i - 10); }
				function bad(float x) -> string { try { return to_string(math::itrunc(x) + 1); } catch (err) { return "throw:" + err; } }
				function read_first(var& arr, int i) -> string { try { return to_string(arr[i] + math::itrunc(1e300)); } catch (err) { return "throw:" + err; } }
				function call_first(var& arr, int i) -> string { try { return to_string(math::itrunc(1e300) + arr[i]); } catch (err) { return "throw:" + err; } }
				var F = []; for (int i = 0; i < 4096; ++i) { F.push(i); }
				to_string(band(5, 1000.0)) + "," + to_string(band(0, 40000.0)) + "," + to_string(texel(F, 3.7, 2.2)) + "," + to_string(fl(4.0)) + "," + to_string(ints(3)) + "," + bad(1e300) + "," + read_first(F, 9999) + "," + call_first(F, 9999);
			)";
			both(script, "", "math temps");
			both(script, "", "math temps, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("6,31,131,8", 0) == 0, "band, clamp, texel and the float expression: " + vm);
			check_true(vm.find(",10,throw:") != std::string::npos, "the int forms and the conversion error: " + vm);
			const std::string interp = run_on(false, script);
			const size_t tail = vm.rfind(",throw:");
			check_true(tail != std::string::npos && vm.substr(tail) == interp.substr(interp.rfind(",throw:")), "the read-first and call-first texts agree: " + vm);
			auto e = jai::foundry::make_default_engine();
			e->set_backend(backend_type::vm);
			stdlib::register_all(*e); register_host_math_fixture(*e);
			auto* symbolizer = detail::engine_access::get_symbolizer(*e);
			const char* fn = "var f = function(var& flat, int bb, float z, float wx, float wy) { int b = bb + math::itrunc(z * 0.001953125); return b + flat[((math::itrunc(wy) & 63) << 6) + (math::itrunc(wx) & 63)]; };";
			auto templates = detail::engine_access::get_registered_template_types(*e);
			jai::lexer lex(fn, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, e.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*e, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			size_t maths = 0, temps = 0;
			for (size_t at = listing.find("MATH"); at != std::string::npos; at = listing.find("MATH", at + 4)) {
				if (at >= 2 && listing.compare(at - 2, 2, "C_") == 0) continue;   // a region's copy
				++maths; if (listing.compare(at, 9, "MATH_TEMP") == 0) { ++temps; }
			}
			check_true(maths == 3 && temps == 3, "every intrinsic lands in a temp: " + listing);
			check_true(listing.find("T_AND_II") != std::string::npos && listing.find("T_SHL_II") != std::string::npos && listing.find("T_ADD_II") != std::string::npos, "the binaries around them take the int rows: " + listing);
		});
		test("an_index_tree_lands_in_a_temp_and_the_read_takes_the_typed_row", [this]() {
			// flat[((itrunc(wy) & 63) << 6) + (itrunc(wx) & 63)] (draw_flat_col's texel), a plain arithmetic tree index, a
			// tree that raises inside the index, and the bounds error of a tree index: the same on both backends
			const std::string script = R"(
				function texel(var& flat, float wx, float wy) -> int { int v = flat[((math::itrunc(wy) & 63) << 6) + (math::itrunc(wx) & 63)]; return v; }
				function tree(var& arr, int i, int j) -> int { int s = 0; s = s + arr[(i + 1) * 2 - j]; return s; }
				function raises(var& arr, int i) -> string { try { return to_string(arr[math::itrunc(1e300) + i]); } catch (err) { return "throw:" + err; } }
				function oob(var& arr, int i) -> string { try { return to_string(arr[(i + 1) * 2]); } catch (err) { return "throw:" + err; } }
				var F = []; for (int i = 0; i < 4096; ++i) { F.push(i); }
				var A = [10, 20, 30, 40, 50, 60];
				to_string(texel(F, 3.7, 2.2)) + "," + to_string(tree(A, 1, 1)) + "," + raises(A, 1) + "," + oob(A, 4);
			)";
			both(script, "", "index trees");
			both(script, "", "index trees, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("131,40,throw:", 0) == 0, "the texel, the tree and the conversion error: " + vm);
			auto e = jai::foundry::make_default_engine();
			e->set_backend(backend_type::vm);
			stdlib::register_all(*e); register_host_math_fixture(*e);
			auto* symbolizer = detail::engine_access::get_symbolizer(*e);
			const char* fn = "var f = function(var& flat, float wx, float wy, int k) { int v = flat[((math::itrunc(wy) & 63) << 6) + (math::itrunc(wx) & 63)]; int w = flat[(k + 1) * 2 - v]; return v + w; };";
			auto templates = detail::engine_access::get_registered_template_types(*e);
			jai::lexer lex(fn, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, e.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*e, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			size_t loads = 0; for (size_t at = listing.find("T_LOAD"); at != std::string::npos; at = listing.find("T_LOAD", at + 6)) { ++loads; }
			check_true(loads == 2 && listing.find("INDEX") == std::string::npos, "both tree-indexed reads take the typed row: " + listing);
		});
		test("a_plain_alias_container_reads_through_the_guard_and_a_shrunken_element_reference_does_not", [this]() {
			// the guard leaves a plain alias (a var& parameter, a var& of a same-unit or other-unit value) to the reader;
			// an element reference whose array shrank is not plain: its dereference raises before the index, both backends
			const std::string script = R"(
				var PAL = []; for (int i = 0; i < 512; ++i) { PAL.push(i * 3); }
				var TEX = [[0, 1, 2, 3], [4, 5, 6, 7]];
				function texel(var& tp, var& pal, int base, int iv) -> int { var& p = pal; int v = p[base + tp[iv + 0]]; return v + pal[(base + 1) * 2 + tp[iv]]; }
				function shrunk(int i) -> string { var rows = [[1, 2, 3], [4, 5, 6]]; var& r = rows[1]; rows.pop(); rows.pop(); try { return to_string(r[i + 0 + rows.size()]); } catch (err) { return "throw:" + err; } }
				var& t0 = TEX[0];
				to_string(texel(t0, PAL, 10, 2)) + "," + to_string(texel(TEX[1], PAL, 0, 3)) + "," + shrunk(9) + "," + shrunk(0);
			)";
			both(script, "", "guarded containers");
			both(script, "", "guarded containers, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("108,48,", 0) == 0, "the plain aliases land: " + vm);
		});
		test("a_closed_loop_may_branch_inside_itself", [this]() {
			// Diamond fixtures in while form, the texture loop's guarded arm, a continue, a break (an exit of the
			// block) and an overflow inside the taken arm: the same on both backends, proofs on and off
			const std::string script = R"(
				function diamond(int n) -> int { int total = 0; int i = 0; while (i < n) { if ((i % 2) == 0) { total += i; } else { total -= i; } i = i + 1; } return total; }
				function guarded(var& pix, var& pal, var& tp, int n, int th) -> int { int acc = 0; int y = 0; while (y < n) { int iv = (y * 7) % th - 3; if (iv < 0) { iv = iv + th; } pix[y] = pal[tp[iv]]; acc = acc + pix[y]; y = y + 1; } return acc; }
				function cont(int n) -> int { int total = 0; for (int i = 0; i < n; ++i) { if (i == 1) { continue; } total += i; } return total; }
				function brk(int n) -> int { int total = 0; int i = 0; while (i < n) { if (i == 5) { break; } total += i; i = i + 1; } return total; }
				function ovf(int n) -> string { int total = 9223372036854775800; int seen = 0; int i = 0; try { while (i < n) { seen = i; if ((i % 2) == 0) { total += i; } else { total -= 1; } i = i + 1; } } catch (err) { return "throw:" + err + "|" + to_string(seen); } return to_string(total); }
				var PIX = [0, 0, 0, 0, 0, 0, 0, 0]; var PAL = []; for (int k = 0; k < 64; ++k) { PAL.push(k * 2); } var TP = [3, 1, 4, 1, 5, 9, 2, 6];
				to_string(diamond(6)) + "," + to_string(diamond(0)) + "," + to_string(diamond(1)) + "," + to_string(guarded(PIX, PAL, TP, 8, 8)) + "," + to_string(cont(10)) + "," + to_string(brk(10)) + "," + ovf(12);
			)";
			both(script, "", "branching cells");
			both(script, "", "branching cells, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("-3,0,0,62,44,10,throw:", 0) == 0 && vm.find("|6") != std::string::npos, "diamonds, the guarded arm, continue, break and the overflow in the arm: " + vm);
			auto e = jai::foundry::make_default_engine();
			e->set_backend(backend_type::vm);
			stdlib::register_all(*e); register_host_math_fixture(*e);
			auto* symbolizer = detail::engine_access::get_symbolizer(*e);
			auto listing_of = [&](const char* src) {
				auto templates = detail::engine_access::get_registered_template_types(*e);
				jai::lexer lex(src, symbolizer);
				auto tokens = lex.tokenize();
				jai::parser p(tokens, symbolizer, e.get(), templates);
				auto parse_result = p.parse();
				check_true(parse_result.has_value(), "snippet parses");
				(void)jai::detail::run_static_check(*e, parse_result.value());
				jai::vm::vm_compiler compiler(symbolizer);
				return jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			};
			const std::string d = listing_of("var f = function(int n) { int total = 0; int i = 0; while (i < n) { if ((i % 2) == 0) { total += i; } else { total -= i; } i = i + 1; } return total; };");
			check_true(d.find("CELLS_BACK") != std::string::npos && d.find("C_JCMP") != std::string::npos && d.find("C_JUMP") != std::string::npos, "the two-arm diamond closes with cell branches: " + d);
			const std::string g = listing_of("var f = function(var& pix, var& pal, var& tp, int n, int th) { int y = 0; while (y < n) { int iv = (y * 7) % th - 3; if (iv < 0) { iv = iv + th; } pix[y] = pal[tp[iv]]; y = y + 1; } return y; };");
			check_true(g.find("CELLS_BACK") != std::string::npos && (g.find("C_JCMP") != std::string::npos || g.find("C_JL") != std::string::npos || g.find("C_JG") != std::string::npos) && g.find("C_GUARD") != std::string::npos, "the texture shape closes over its guarded arm and its container guard: " + g);
			const std::string b = listing_of("var f = function(int n) { int total = 0; int i = 0; while (i < n) { if (i == 5) { break; } total += i; i = i + 1; } return total; };");
			check_true(b.find("CELLS_BACK") != std::string::npos && b.find("C_EXIT") != std::string::npos, "a break is an exit of the block: " + b);
		});
		test("an_environment_scalar_in_a_store_index_is_a_read_only_cell", [this]() {
			// JaiDOOM's pix[y * VW + x] with VW a global: the loop closes with VW resolved once at entry; a float VW and a
			// reference VW keep the general form; a VW changed between calls is read anew at each entry
			const std::string script = R"(
				var VW = 4;
				var PIX = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
				function col(var& pix, var& pal, var& tp, int x, int n) -> int { int acc = 0; int y = 0; while (y < n) { int iv = (y * 5) % 8 - 2; if (iv < 0) { iv = iv + 8; } pix[y * VW + x] = pal[tp[iv]]; acc = acc + pix[y * VW + x]; y = y + 1; } return acc; }
				var PAL = []; for (int k = 0; k < 64; ++k) { PAL.push(k * 3); } var TP = [3, 1, 4, 1, 5, 9, 2, 6];
				int a = col(PIX, PAL, TP, 1, 4);
				VW = 2; int b = col(PIX, PAL, TP, 0, 4);
				VW = 1.5; string c = ""; try { c = to_string(col(PIX, PAL, TP, 0, 4)); } catch (err) { c = "throw:" + err; }
				to_string(a) + "," + to_string(b) + "," + c + "," + to_string(PIX[1]) + "," + to_string(PIX[5]);
			)";
			both(script, "", "env cell");
			both(script, "", "env cell, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("", 0) == 0 && vm.find(",") != std::string::npos, "ran: " + vm);
			auto e = jai::foundry::make_default_engine();
			e->set_backend(backend_type::vm);
			stdlib::register_all(*e); register_host_math_fixture(*e);
			auto* symbolizer = detail::engine_access::get_symbolizer(*e);
			const char* fn = "var VW = 4; var f = function(var& pix, var& pal, var& tp, int x, int n) { int y = 0; while (y < n) { int iv = (y * 5) % 8 - 2; if (iv < 0) { iv = iv + 8; } pix[y * VW + x] = pal[tp[iv]]; y = y + 1; } return y; };";
			auto templates = detail::engine_access::get_registered_template_types(*e);
			jai::lexer lex(fn, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, e.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*e, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			check_true(listing.find("CELLS_BACK") != std::string::npos && listing.find("C_INDEX_STORE") != std::string::npos, "the texture shape with a global view width closes: " + listing);
		});
		test("a_flat_column_loop_closes_over_var_globals_math_and_clamps", [this]() {
			// JaiDOOM's draw_flat_col: var globals in the arithmetic (CENTERY, VIEW_X, VIEW_Y, VW), a float / int division,
			// itrunc into a band, clamps, a continue, the texture as an element reference into a table; a float CENTERY
			// (a conversion at the int decl) and a pixel table aliasing the texture table keep the general form's results;
			// the same pixels on both backends, proofs on and off
			const std::string script = R"(
				var VW = 4; var VIEWH = 16; var CENTERY = 3; var VIEW_X = 0.5; var VIEW_Y = 0.25; var VIEW_Z = 41.0; var FOCALY = 60.0;
				var PIX = []; for (int k = 0; k < 64; ++k) { PIX.push(0); }
				var PALCM = []; for (int k = 0; k < 8192; ++k) { PALCM.push(k % 251); }
				var FLATS = []; for (int t = 0; t < 2; ++t) { var f = []; for (int k = 0; k < 4096; ++k) { f.push((k * (7 + t)) % 64); } FLATS.push(f); }
				function flat_col(int x, int y0, int y1, var& flat, float planeh, float raydx, float raydy, int light) -> int {
					if (y0 < 0) { y0 = 0; }
					if (y1 >= VIEWH) { y1 = VIEWH - 1; }
					float dz = (VIEW_Z - planeh) * FOCALY;
					int bandbase = ((255 - light) >> 3) - 1;
					var& pix = PIX; var& palcm = PALCM;
					for (int y = y0; y <= y1; ++y) {
						int dy = y - CENTERY;
						if (dy == 0) { continue; }
						float z = dz / dy;
						float wx = VIEW_X + raydx * z;
						float wy = VIEW_Y + raydy * z;
						int band = bandbase + math::itrunc(z * 0.001953125);
						if (band < 0) { band = 0; }
						if (band > 31) { band = 31; }
						pix[y * VW + x] = palcm[band * 256 + flat[((math::itrunc(wy) & 63) << 6) + (math::itrunc(wx) & 63)]];
					}
					int acc = 0; for (int k = 0; k < 64; ++k) { acc = acc + pix[k] * (k + 1); }
					return acc;
				}
				var& cflat = FLATS[1];
				int a = flat_col(1, -2, 40, cflat, 20.0, 0.7, -0.3, 96);
				int b = flat_col(2, 0, 15, FLATS[0], -8.0, 1.9, 0.4, 200);
				CENTERY = 2.5; string c = ""; try { c = to_string(flat_col(3, 0, 15, cflat, 20.0, 0.7, -0.3, 96)); } catch (err) { c = "throw:" + err; }
				CENTERY = 3; PIX = FLATS; string d = ""; try { d = to_string(flat_col(1, 0, 3, cflat, 20.0, 0.7, -0.3, 96)); } catch (err) { d = "throw:" + err; }
				to_string(a) + "," + to_string(b) + "," + c + "," + d + "," + to_string(FLATS[1].size()) + "," + type_of(FLATS[1]);
			)";
			both(script, "", "flat column");
			both(script, "", "flat column, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("throw:", 0) != 0 && vm.find(",") != std::string::npos, "ran: " + vm);
			auto e = jai::foundry::make_default_engine();
			e->set_backend(backend_type::vm);
			stdlib::register_all(*e); register_host_math_fixture(*e);
			auto* symbolizer = detail::engine_access::get_symbolizer(*e);
			const char* fn = "var VW = 4; var CENTERY = 3; var VIEW_X = 0.5; var VIEW_Y = 0.25;"
			                 " var f = function(var& pix, var& palcm, var& flat, int x, int y0, int y1, float dz, float raydx, float raydy, int bandbase) {"
			                 " for (int y = y0; y <= y1; ++y) { int dy = y - CENTERY; if (dy == 0) { continue; } float z = dz / dy; float wx = VIEW_X + raydx * z; float wy = VIEW_Y + raydy * z;"
			                 " int band = bandbase + math::itrunc(z * 0.001953125); if (band < 0) { band = 0; } if (band > 31) { band = 31; }"
			                 " pix[y * VW + x] = palcm[band * 256 + flat[((math::itrunc(wy) & 63) << 6) + (math::itrunc(wx) & 63)]]; } return y0; };";
			auto templates = detail::engine_access::get_registered_template_types(*e);
			jai::lexer lex(fn, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, e.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*e, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			check_true(listing.find("CELLS_BACK") != std::string::npos && listing.find("C_ARITH_ANY") != std::string::npos && listing.find("C_MATH") != std::string::npos && listing.find("C_MOV") != std::string::npos, "the flat column loop closes: " + listing);
		});
		test("two_pinned_tables_sharing_a_guard_slot_keep_their_own_snapshots", [this]() {
			// Probes (guard_slot_reuse, guard_slot_reuse_kind): two guarded element-reference reads in
			// different statements share one guard-temp slot; a bounds miss and a kind miss at the first read must resume
			// the general form on the first table's handle, not the second's
			const char* body = R"(
				function probe(var& a, var& b, int n, int zero, int one) -> int {
					int total = 0;
					int i = 0;
					while (i < n) {
						int x = a[(i + zero) * (one + zero)];
						int y = b[(zero + zero) * (one + zero)];
						total = total + x + y;
						i = i + 1;
					}
					return total;
				}
				var& a = rows[0];
				var& b = rows[1];
				string r = ""; try { r = to_string(probe(a, b, 4, 0, 1)); } catch (e) { r = "caught:" + e; }
				r + "|" + to_string(probe(a, b, 2, 0, 1));
			)";
			const std::string bounds = std::string("var rows = [[10,20], [100,101,102,103,104,105]];") + body;
			const std::string kind = std::string("var rows = [[10,20,\"bad\",40], [100,101,102,103,104,105]];") + body;
			both(bounds, "caught:Array index 2 out of bounds for array of size 2|230", "bounds miss on the first table");
			both(bounds, "", "bounds miss on the first table, proofs off", false);
			both(kind, "", "kind miss on the first table");
			both(kind, "", "kind miss on the first table, proofs off", false);
			const std::string vm = run_on(true, kind);
			check_true(vm.rfind("caught:", 0) == 0 && vm.find("declared int") != std::string::npos && vm.find("|230") != std::string::npos, "the kind miss raises the declaration's text: " + vm);
		});
		test("a_counted_loop_runs_as_a_block_with_its_back_edge_stepping_in_the_cells", [this]() {
			// for (int i = 0; i < n; ++i) (the counted form: pix_clear, the row loops): a clear into a typed global, a stride
			// with a continue, an end changed inside the body (read fresh at every back edge), a step that overflows (the
			// general back edge's text), zero iterations, a float and a string value into array<int> (the general store's
			// conversion and rejection); the same on both backends, proofs on and off
			const std::string script = R"(
				array<int> PIX = []; for (int k = 0; k < 64; ++k) { PIX.push(0); }
				var VW = 8; var VH = 8;
				function clear_to(int color) -> int { int n = VW * VH; for (int i = 0; i < n; ++i) { PIX[i] = color; } return PIX[0] + PIX[63]; }
				function fill_any(var v) -> int { int n = VW * VH; for (int i = 0; i < n; ++i) { PIX[i] = v; } return PIX[1]; }
				function sum_stride(var& pix, int n, int step) -> int { int acc = 0; for (int i = 0; i < n; i += step) { acc = acc + pix[i]; if (i == 42) { continue; } acc = acc + 1; } return acc; }
				function shrink(var& pix, int n) -> int { int hits = 0; for (int i = 0; i < n; ++i) { pix[i] = i; n = n - 2; hits = hits + 1; } return hits * 100 + pix[3]; }
				function stepped(int n) -> string { int acc = 0; for (int i = 9223372036854775800; i < n; i += 4) { acc = acc + 1; } return to_string(acc); }
				int a = clear_to(7);
				int b = sum_stride(PIX, 64, 3) + sum_stride(PIX, 0, 3);
				int c = shrink(PIX, 64);
				string d = ""; try { d = stepped(9223372036854775807); } catch (err) { d = "throw:" + err; }
				string e = ""; try { e = to_string(fill_any(2.5)); } catch (err) { e = "throw:" + err; }
				string f = ""; try { f = to_string(fill_any("x")); } catch (err) { f = "throw:" + err; }
				to_string(a) + "," + to_string(b) + "," + to_string(c) + "," + d + "," + e + "," + f + "," + to_string(PIX[2]);
			)";
			both(script, "", "counted loops");
			both(script, "", "counted loops, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("14,", 0) == 0 && vm.find("throw:Integer overflow in '+='") != std::string::npos, "the clear and the overflow text: " + vm);
			auto e = jai::foundry::make_default_engine();
			e->set_backend(backend_type::vm);
			stdlib::register_all(*e); register_host_math_fixture(*e);
			auto* symbolizer = detail::engine_access::get_symbolizer(*e);
			const char* fn = "array<int> PIX = []; var VW = 8; var VH = 8; var f = function(int color) { int n = VW * VH; for (int i = 0; i < n; ++i) { PIX[i] = color; } return n; };";
			auto templates = detail::engine_access::get_registered_template_types(*e);
			jai::lexer lex(fn, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, e.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*e, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			check_true(listing.find("CELLS_CFOR_BACK") != std::string::npos && listing.find("C_CFOR_BACK") != std::string::npos && listing.find("C_INDEX_STORE") != std::string::npos, "the counted clear loop closes: " + listing);
		});
		test("compare_jumps_with_run_time_kinds_close_a_patch_loop_and_an_env_headed_loop", [this]() {
			// JaiDOOM's draw_patch inner loop (py >= VH with VH a var global, a continue, a pinned sprite table, VW in the
			// store) and a while whose head compares against a var global; a float head operand takes the promotion row, a
			// string global keeps the general form; the same on both backends, proofs on and off
			const std::string script = R"(
				var VW = 8; var VH = 8; var LIMIT = 5; var FLIMIT = 4.5; var SLIMIT = "x";
				var PIX = []; for (int k = 0; k < 64; ++k) { PIX.push(0); }
				var PALCM = []; for (int k = 0; k < 256; ++k) { PALCM.push(k * 3 % 251); }
				var SPR_PIX = []; for (int t = 0; t < 2; ++t) { var s = []; for (int k = 0; k < 64; ++k) { s.push((k * (5 + t)) % 40 - 3); } SPR_PIX.push(s); }
				function patch_col(int lump, int x0, int y0, int w, int h, int pw, int ph) -> int {
					var& tp = SPR_PIX[lump]; var& pix = PIX; var& palcm = PALCM;
					int acc = 0;
					for (int sx = 0; sx < w; ++sx) {
						int px = x0 + sx;
						if (px < 0 || px >= VW) { continue; }
						int base = (sx * pw / w) * ph;
						for (int sy = 0; sy < h; ++sy) {
							int py = y0 + sy;
							if (py < 0 || py >= VH) { continue; }
							int texel = tp[base + sy * ph / h];
							if (texel >= 0) { pix[py * VW + px] = palcm[texel]; }
						}
					}
					for (int k = 0; k < 64; ++k) { acc = acc + pix[k] * (k + 1); }
					return acc;
				}
				function count_up(int start) -> int { int x = start; int n = 0; while (x < LIMIT) { n = n + x; x = x + 1; } return n; }
				function count_f(float start) -> int { float x = start; int n = 0; while (x < LIMIT) { n = n + 1; x = x + 0.5; } return n; }
				function count_fl(int start) -> int { int x = start; int n = 0; while (x < FLIMIT) { n = n + 1; x = x + 1; } return n; }
				int a = patch_col(1, -2, -3, 12, 12, 8, 8);
				int b = patch_col(0, 3, 2, 4, 9, 8, 8);
				int c = count_up(-3) * 1000 + count_f(1.25) * 10 + count_fl(0);
				LIMIT = 7.5; int d = count_up(0);
				string e = ""; try { LIMIT = SLIMIT; e = to_string(count_up(0)); } catch (err) { e = "throw:" + err; }
				to_string(a) + "," + to_string(b) + "," + to_string(c) + "," + to_string(d) + "," + e;
			)";
			both(script, "", "run-time-kind compares");
			both(script, "", "run-time-kind compares, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.find(",4085,28,") != std::string::npos, "the env-headed loops: " + vm);
			const std::string listing = listing_of("var VW = 8; var VH = 8; var LIMIT = 5;"
			                 " var f = function(var& pix, var& palcm, var& tp, int px, int y0, int h, int base, int ph) {"
			                 " for (int sy = 0; sy < h; ++sy) { int py = y0 + sy; if (py < 0) { continue; } if (py >= VH) { continue; } int texel = tp[base + sy * ph / h]; if (texel >= 0) { pix[py * VW + px] = palcm[texel]; } } return h; };"
			                 " var g = function(int start) { int x = start; int n = 0; while (x < LIMIT) { n = n + x; x = x + 1; } return n; };"
			                 " var f_or = function(int h, int y0) { int acc = 0; for (int sy = 0; sy < h; ++sy) { int py = y0 + sy; if (py < 0 || py >= VH) { continue; } acc = acc + py; } return acc; };");
			check_true(listing.find("C_JCMP_ANY") != std::string::npos && listing.find("CELLS_CFOR_BACK") != std::string::npos, "the patch loop closes over its env compare: " + listing);
			check_true(listing.find("C_CMP_ANY") != std::string::npos, "the env-headed while closes: " + listing);
			// the || chain (a comparison value and JUMP_IF_TRUE) is one cmp with a jump sense: f_or closes too
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(3), closed, "f, g and f_or close: " + listing);
		});
		test("translation_equivalence_the_same_loop_in_every_spelling_closes_the_same_way", [this]() {
			// one meaning, five spellings: the step as ++i, i += 1 and i = i + 1; the loop as a while, an identifier-start for
			// and a literal-start for; every one closes (the listing) and lands the same sums on both backends
			const char* defs =
				"array<int> A = []; for (int k = 0; k < 32; ++k) { A.push(k * 3 % 17); }"
				" var s1 = function(var& a, int n) { int acc = 0; int i = 0; while (i < n) { acc = acc + a[i]; ++i; } return acc; };"
				" var s2 = function(var& a, int n) { int acc = 0; int i = 0; while (i < n) { acc = acc + a[i]; i += 1; } return acc; };"
				" var s3 = function(var& a, int n) { int acc = 0; int i = 0; while (i < n) { acc = acc + a[i]; i = i + 1; } return acc; };"
				" var s4 = function(var& a, int n, int from) { int acc = 0; for (int i = from; i < n; ++i) { acc = acc + a[i]; } return acc; };"
				" var s5 = function(var& a, int n) { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + a[i]; } return acc; };";
			const std::string script = std::string(defs) + " to_string(s1(A, 32)) + \",\" + to_string(s2(A, 32)) + \",\" + to_string(s3(A, 32)) + \",\" + to_string(s4(A, 32, 0)) + \",\" + to_string(s5(A, 32));";
			both(script, "", "five spellings");
			both(script, "", "five spellings, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.find(",") != std::string::npos && vm.find("throw") == std::string::npos, "ran: " + vm);
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(5), closed, "every spelling closes (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 400) + ")");
		});
		test("open_loops_are_named_by_class_and_first_instruction", [this]() {
			// the translation's refusals are recorded per loop: a call is an effect (a break and a string append are not: both close),
			// a member read of a frame-slot instance is a load site, a tree deeper than
			// the lift's temp cap (a right-deep tree 20 levels deep, its left sides counted held: the lifted part pushes and the general BINARYs consume it) is a translation gap
			const std::string listing = listing_of(
				"var g = function(int x) { return x; }; class P { int v = 1; }"
				" var f1 = function(int n) { int acc = 0; int i = 0; while (i < n) { acc = acc + g(i); i = i + 1; } return acc; };"
				" var f2 = function(int n) { int acc = 0; int i = 0; while (i < n) { if (i == 3) { break; } acc = acc + i; i = i + 1; } return acc; };"
				" var f3 = function(int n) { string s = \"\"; int i = 0; while (i < n) { s += \"x\"; i = i + 1; } return s; };"
				" var f4 = function(int n, P p) { int acc = 0; int i = 0; while (i < n) { acc = acc + p.v; i = i + 1; } return acc; };"
				" var f5 = function(int n) { int acc = 0; int i = 0; while (i < n) { int v = i + (i + (i + (i + (i + (i + (i + (i + (i + (i + (i + (i + (i + (i + (i + (i + (i + (i + (i + (i + (i)))))))))))))))))))); acc = acc + v; i = i + 1; } return acc; };"
				" var f6 = function(int n) { int acc = 0; int i = 0; while (i < n) { acc = acc + to_string(i).size(); i = i + 1; } return acc; };");
			check_true(listing.find("-- open loop") != std::string::npos, "refusals listed: " + listing);
			check_true(listing.find(": effect at") != std::string::npos, "the method call on a call's result is an effect, its loop too thin for a boundary: " + listing);
			check_true(listing.find("CALL_TEMP") != std::string::npos && listing.find("C_PROBE") != std::string::npos, "the call the sum consumes lands in a temp and its loop closes through the door: " + listing);
			check_true(listing.find(": translation at") != std::string::npos && listing.find("BINARY op=") != std::string::npos, "the tree past the lift's cap is a translation gap (the lifted part pushes, the stack binaries consume it; as a boundary it would be most of the body): " + listing);
			check_true(listing.find("C_MLOAD") != std::string::npos && listing.find("(MEMBER_TEMP)") == std::string::npos, "the member read of a frame-slot instance is a load site and its loop closes: " + listing);
		});
		test("head_spans_the_math_family_and_untyped_compounds_close", [this]() {
			// a while whose condition computes before its compare (a temp, an env value, an element read), the float family of
			// math on int and float values (host_abs of INT64_MIN takes the kernel's overflow text), x += y and ++x on var slots
			// (kinds at run time: a kind change on the slot lands through the general conversion); the same on both backends
			const char* defs =
				"var LIMIT = 6; array<int> B = [3, 1, 4, 1, 5, 9, 2, 6];"
				" var h1 = function(int n) { int acc = 0; int i = 0; while (i < n * 2) { acc = acc + i; i = i + 1; } return acc; };"
				" var h2 = function(int n) { int acc = 0; int i = 0; while (i < LIMIT + n) { acc = acc + i; i = i + 1; } return acc; };"
				" var h3 = function(var& b, int n) { int acc = 0; int i = 0; while (i < b[n]) { acc = acc + b[i]; i = i + 1; } return acc; };"
				" var m1 = function(float x0, int n) { float acc = 0.0; float x = x0; int i = 0; while (i < n) { acc = acc + math::floor(x) + math::ceil(x) + math::sqrt(x + 2.0) + math::abs(x - 3.0) + math::sin(x) + math::cos(x) + math::tan(x) + math::radians_to_degrees(x) + math::degrees_to_radians(x); x = x + 0.75; i = i + 1; } return acc; };"
				" var m2 = function(int n) { int acc = 0; int i = -n; while (i < n) { acc = acc + math::abs(i) + math::itrunc(math::sqrt(i * i)); i = i + 1; } return acc; };"
				" var m3 = function(int n) { int acc = 0; int i = 0; int v = -9223372036854775807 - 1; while (i < n) { if (i == 2) { acc = acc + math::abs(v); } i = i + 1; } return acc; };"
				" var c1 = function(var v, int n) { int i = 0; while (i < n) { v += 2; ++i; } return v; };"
				" var c2 = function(var v, int n) { int i = 0; while (i < n) { v += 0.5; i++; } return v; };"
				" var c3 = function(int n) { int i = 0; var k = 0; while (i < n) { k -= 3; k *= 2; k %= 1000; i = i + 1; } return k; };";
			const std::string script = std::string(defs) + " string r = to_string(h1(5)) + \",\" + to_string(h2(3)) + \",\" + to_string(h3(B, 4)) + \",\" + to_string(m1(0.5, 6)) + \",\" + to_string(m2(4)) + \",\" + to_string(c1(1, 5)) + \",\" + to_string(c1(1.5, 5)) + \",\" + to_string(c2(1, 4)) + \",\" + to_string(c3(6));"
			                           " try { r = r + \",\" + to_string(m3(4)); } catch (err) { r = r + \",throw:\" + err; } r;";
			both(script, "", "head spans, math, compounds");
			both(script, "", "head spans, math, compounds, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("45,36,", 0) == 0 && vm.find("throw:Integer overflow in math::abs") != std::string::npos, "the head spans and the host_abs overflow: " + vm);
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(9), closed, "every loop closes (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 600) + ")");
		});
		test("var_and_auto_locals_land_by_their_declared_type", [this]() {
			// the declared-type rule: a var accumulator takes ints then floats (the kind change lands, the any stamp stays), an
			// auto local infers then enforces, an int local fed a float takes the general form's checked conversion, a float
			// local fed an int widens through a store as through a declaration; var element reads land the element's kind
			const char* defs =
				"array<int> A = [3, 1, 4, 1, 5, 9, 2, 6]; var H = [1, 2.5, 3, 4.5];"
				" var v1 = function(var& a, int n) { var acc = 0; int i = 0; while (i < n) { acc = acc + a[i]; i = i + 1; } return acc; };"
				" var v2 = function(var& h, int n) { var acc = 0; int i = 0; while (i < n) { var e = h[i]; acc = acc + e; i = i + 1; } return acc; };"
				" var v3 = function(var& a, int n) { auto acc = 0; int i = 0; while (i < n) { acc = acc + a[i] * 2; i = i + 1; } return acc; };"
				" var v4 = function(var& h, int n) { int acc = 0; int i = 0; string r = \"\"; try { while (i < n) { acc = acc + h[i]; i = i + 1; } r = to_string(acc); } catch (err) { r = \"throw:\" + err; } return r; };"
				" var v5 = function(var& a, int n) { float acc = 0.0; int i = 0; while (i < n) { int t = a[i]; acc = t; acc = acc + i; i = i + 1; } return acc; };"
				" var v6 = function(var& h, int n) { var last = 0; int i = 0; while (i < n) { last = h[i]; i = i + 1; } return last; };";
			const std::string script = std::string(defs) + " to_string(v1(A, 8)) + \",\" + to_string(v2(H, 4)) + \",\" + to_string(v3(A, 8)) + \",\" + v4(H, 4) + \",\" + to_string(v5(A, 8)) + \",\" + to_string(v6(H, 4)) + \",\" + type_of(v6(H, 4)) + \",\" + type_of(v1(A, 8));";
			both(script, "", "declared-type landings");
			both(script, "", "declared-type landings, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("31,11.0,62,", 0) == 0, "the var, the float sum and the auto: " + vm);
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			// v5's widening store from a local (`acc = t`) and v6's assignment from an element read (`last = h[i]`) are move
			// rows (a LOAD and STORE pair, a pushing read and STORE before it), so all six close
			check_eq(size_t(6), closed, "the six value-model loops close (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 600) + ")");
		});
		test("temps_are_values_per_definition", [this]() {
			// a compiler temp reused by consecutive statements with different kinds (draw_seg's column loop: an int element
			// read in the test, a float subtraction in the next statement) is two values, not one slot of two kinds, so the
			// loop closes; a miss at an int overflow in the first statement, after the float definition of the same temp in the
			// previous iteration, still reports the overflow (the dead float is never flushed over the live int); both
			// backends, proofs on and off
			const char* defs =
				"array<int> done = [0, 1, 0, 0, 1, 0]; array<float> F = [0.5, 1.5, 2.5, 3.5, 4.5, 5.5]; array<int> B = [1, 2, 3, 4, 5, 6]; array<int> C = [1, 2, 9223372036854775807, 4, 5, 6];"
				" var s1 = function(int xa, int xb, float x1f, float spanx) { float acc = 0.0; for (int x = xa; x <= xb; ++x) { if (done[x] != 0) { continue; } float t3 = (x - x1f) / spanx; acc = acc + t3 * F[x]; } return acc; };"
				" var s2 = function(var& a, int n) { int p = 0; float q = 0.0; int i = 0; while (i < n) { p = p + (a[i] + 1); q = q + (F[i] - 0.5) * 2.0; i = i + 1; } return to_string(p) + \",\" + to_string(q); };";
			const std::string script = std::string(defs) + " string r = to_string(s1(0, 5, 1.5, 0.75)) + \",\" + s2(B, 6);"
			                           " try { r = r + \",\" + s2(C, 6); } catch (err) { r = r + \",throw:\" + err; } r;";
			both(script, "", "values per definition");
			both(script, "", "values per definition, proofs off", false);
			// a graft's converting declaration (a load and a declaration, not one row) on a temp slot an earlier graft defined with
			// another kind writes its own definition's cell
			const std::string converting = "float g = 1.0; float half() { return 0.5; } int three() { int t = true; return t; }"
				" int run() { int s = 0; for (int i = 0; i < 3; ++i) { bool b = (g + half()) != g; s = s + three(); } return s; } to_string(run());";
			both(converting, "3", "a converting declaration on a reused temp slot");
			const std::string vm = run_on(true, script);
			check_true(vm.find(",throw:") != std::string::npos && vm.find("overflow") != std::string::npos, "the overflow after a float definition of the same temp: " + vm);
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(2), closed, "both loops close (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 400) + ")");
		});
		test("moves_are_one_row", [this]() {
			// a statement-position move from a slot, a literal or an element read is one destination-addressed row whatever the
			// checker proved: the widening store, the var store, the unproven same-kind store, the literal into a float, the
			// element read into a var and into a typed local; the row's guard is total (the same-type payload test, else the
			// exact general pair), so proofs on and off land the same, the listing has no general STORE left in these bodies,
			// and every loop closes
			const char* defs =
				"array<int> A = [3, 1, 4, 1, 5]; var H = [1, 2.5, 3];"
				" var m1 = function(int n) { float acc = 0.0; int t = 0; int i = 0; while (i < n) { t = A[i]; acc = t; i = i + 1; } return acc; };"
				" var m2 = function(var p, int n) { var q = 0; int i = 0; while (i < n) { q = p; p = i; i = i + 1; } return q; };"
				" var m3 = function(int n) { float f = 0.0; int k = 0; int i = 0; while (i < n) { f = 2; k = 7; i = i + 1; } return f + k; };"
				" var m4 = function(int n) { var last = 0; int e = 0; int i = 0; while (i < n) { last = H[i]; e = A[i]; i = i + 1; } return to_string(last) + \",\" + to_string(e); };"
				" var m5 = function(int n) { float w = 0.0; int i = 0; while (i < n) { w = A[i]; i = i + 1; } return w; };";
			const std::string script = std::string(defs) + " to_string(m1(5)) + \",\" + to_string(m2(9, 4)) + \",\" + to_string(m3(3)) + \",\" + m4(3) + \",\" + to_string(m5(5)) + \",\" + type_of(m2(2.5, 0));";
			both(script, "", "moves");
			both(script, "", "moves, proofs off", false);
			const std::string listing = listing_of(defs);
			size_t stores = 0; for (size_t at = listing.find("\tSTORE "); at != std::string::npos; at = listing.find("\tSTORE ", at + 1)) { ++stores; }
			size_t moves = 0; for (size_t at = listing.find("T_STORE_MOV"); at != std::string::npos; at = listing.find("T_STORE_MOV", at + 1)) { ++moves; }
			check_eq(size_t(0), stores, "no general STORE left in the move bodies: " + listing);
			check_true(moves >= 7, "the moves are rows: " + std::to_string(moves));
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(5), closed, "every move loop closes (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 400) + ")");
		});
		test("a_removed_map_entry_refuses_the_block", [this]() {
			// Regression: the entry dereferenced a guarded map-entry reference before the loop's own code ran,
			// so a key erased since the reference was taken raised the removed-entry text at admission on a path the general
			// form never takes; admission is non-observing, the stale entry refuses the block, and the text appears only where
			// the loop itself reads the entry (the taken companion), on both backends, proofs on and off
			const char* defs =
				"var untaken = function(int n, int zero, int one) { var table = {\"key\": [10, 20]}; var& pixels = table[\"key\"]; table.erase(\"key\"); int total = 0; int i = 0;"
				" while (i < n) { if (i < zero) { total = total + pixels[(i + zero) * (one + zero)]; } total = total + i; i = i + 1; } return total; };"
				" var taken = function(int n, int zero, int one) { var table = {\"key\": [10, 20]}; var& pixels = table[\"key\"]; table.erase(\"key\"); int total = 0; int i = 0;"
				" while (i < n) { if (i >= one) { total = total + pixels[(i + zero) * (one + zero)]; } i = i + 1; } return total; };";
			const std::string untaken_script = std::string(defs) + " to_string(untaken(1, 0, 1)) + \",\" + to_string(untaken(3, 0, 1));";
			const std::string taken_script = std::string(defs) + " string r = \"\"; try { r = to_string(taken(3, 0, 1)); } catch (err) { r = \"caught:\" + err; } r;";
			both(untaken_script, "0,3", "the untaken branch never touches the removed entry");
			both(untaken_script, "0,3", "the untaken branch, proofs off", false);
			both(taken_script, "", "the taken branch raises the removed-entry text");
			both(taken_script, "", "the taken branch, proofs off", false);
			const std::string vm = run_on(true, taken_script);
			check_true(vm.find("removed map entry") != std::string::npos, "the text where the loop reads it: " + vm);
			const std::string listing = listing_of(defs);
			check_true(listing.find("-- cell loop ") != std::string::npos, "the probe loops close, so the entry is what refuses: " + listing);
		});
		test("unaries_are_values", [this]() {
			// a unary minus, complement or not over an operand is a definition into a compiler value: in a declaration, in a
			// store, as a binary's operand, as a math argument and as an element store's value; the general unary runs
			// underneath (the overflow text at the minimum int, the numeric-operand text on a string), the loops over int and
			// float values close (neg and bnot in the copy), `!` keeps its bool on the general path; both backends, proofs on
			// and off; the listing has no general UNARY left in these bodies
			const char* defs =
				"array<int> A = [3, 1, 4, 1, 5]; array<float> F = [0.5, 1.5, 2.5]; array<int> M = [1, 1 << 63, 3]; array<int> O = [0, 0, 0];"
				" var u1 = function(int n) { int acc = 0; int i = 0; while (i < n) { int v = -A[i]; int w = ~A[i]; acc = acc + v + w; i = i + 1; } return acc; };"
				" var u2 = function(int n) { float acc = 0.0; int i = 0; while (i < n) { float g = -F[i] * 2.0; acc = acc + g; i = i + 1; } return acc; };"
				" var u3 = function(int n) { int acc = 0; int y = 0; int i = 0; while (i < n) { y = -i; acc = acc + math::abs(-A[i]) + y; O[i] = -y; i = i + 1; } return acc + O[0] + O[1] + O[2]; };"
				" var u4 = function(int n) { int acc = 0; int i = 0; while (i < n) { int v = -M[i]; acc = acc + v; i = i + 1; } return acc; };"
				" var u5 = function(int n) { int c = 0; int i = 0; while (i < n) { bool z = !A[i]; if (z) { c = c + 1; } i = i + 1; } return c; };";
			const std::string script = std::string(defs) + " string r = to_string(u1(5)) + \",\" + to_string(u2(3)) + \",\" + to_string(u3(3)) + \",\" + to_string(u5(5));"
			                           " try { r = r + \",\" + to_string(u4(3)); } catch (err) { r = r + \",throw:\" + err; } r;";
			both(script, "", "unaries");
			both(script, "", "unaries, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.find(",throw:") != std::string::npos && vm.find("unary '-'") != std::string::npos, "the negation overflow text: " + vm);
			const std::string listing = listing_of(defs);
			size_t unaries = 0; for (size_t at = listing.find("\tUNARY "); at != std::string::npos; at = listing.find("\tUNARY ", at + 1)) { ++unaries; }
			check_eq(size_t(0), unaries, "no general UNARY left in the bodies: " + listing);
			check_true(listing.find("UNARY_TEMP") != std::string::npos && listing.find("C_NEG") != std::string::npos && listing.find("C_BNOT") != std::string::npos, "the rows and their copies: " + listing);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(5), closed, "the four numeric loops and the ! loop close (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 400) + ")");
		});
		test("value_doors_close", [this]() {
			// draw_sprite's `int fiv = math::itrunc((fy - y0f) * dv)` (a math call as a declaration's value), a math call as a store's
			// and an element store's value, trace_block_dz's `VX_X[LD_V1[i]]` (an element read as the index of an environment
			// array), and truth tests on int and float locals as a loop head and inside a body, all landing the same on both
			// backends with proofs on and off, every one of those loops closing, no general MATH or INDEX left in them
			const char* defs =
				"array<float> VX_X = [0.5, 1.5, 2.5, 3.5]; array<int> LD_V1 = [2, 0, 3, 1]; array<int> O = [0, 0, 0, 0];"
				" var d1 = function(int y0, int y1, float y0f, float dv, int h) { int acc = 0; for (int fy = y0; fy <= y1; ++fy) { int fiv = math::itrunc((fy - y0f) * dv); if (fiv < 0 || fiv >= h) { continue; } acc = acc + fiv; } return acc; };"
				" var d2 = function(int n) { float acc = 0.0; float r = 0.0; int i = 0; while (i < n) { r = math::sqrt(acc + 1.0); O[i] = math::itrunc(r * 2.0); acc = acc + r; i = i + 1; } return to_string(acc) + \",\" + to_string(O[0] + O[1] + O[2] + O[3]); };"
				" var d3 = function(int n) { float acc = 0.0; int i = 0; while (i < n) { float ax = VX_X[LD_V1[i]]; acc = acc + ax; i = i + 1; } return acc; };"
				" var d4 = function(int n) { int acc = 0; while (n) { acc = acc + n; n = n - 1; } return acc; };"
				" var d5 = function(int n) { int acc = 0; int i = 0; while (i < n) { int m = i % 3; if (m) { acc = acc + 1; } float f = 0.5 * i; if (f) { acc = acc + 10; } i = i + 1; } return acc; };";
			const std::string script = std::string(defs) + " to_string(d1(2, 7, 1.5, 0.75, 6)) + \",\" + d2(4) + \",\" + to_string(d3(4)) + \",\" + to_string(d4(5)) + \",\" + to_string(d5(6));";
			both(script, "", "value doors");
			both(script, "", "value doors, proofs off", false);
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(5), closed, "every value-door loop closes (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 600) + ")");
			size_t general = 0; for (const char* op : {"\tMATH ", "\tINDEX "}) { for (size_t at = listing.find(op); at != std::string::npos; at = listing.find(op, at + 1)) { ++general; } }
			check_eq(size_t(0), general, "no general MATH or INDEX left in the bodies (the declarations at the top level are the script's own): " + listing);
		});
		test("negative_literals_and_environment_bounds", [this]() {
			// -1 is a literal: `int other = -1` takes the constant row, `while (bm[i] != -1)` takes the fused compare and its loop
			// closes (cross_check's head), -1.5 and -(-1) and INT64_MIN spelled as -9223372036854775807 - 1 land the same on both
			// backends, -x on a variable keeps the unary; a counted loop whose bound is a global (render_view's `i < VW`) closes
			const char* defs =
				"int VW = 6; array<int> CL = [1, 1, 1, 1, 1, 1]; array<int> BM = [3, 7, 9, -1, 5];"
				" var e1 = function() { int acc = 0; for (int i = 0; i < VW; ++i) { CL[i] = 0; acc = acc + i; } return acc + CL[0] + CL[5]; };"
				" var e2 = function(var& bm) { int acc = 0; int i = 0; while (bm[i] != -1) { int other = -1; acc = acc + bm[i] + other; i = i + 1; } return acc; };"
				" var e3 = function(int x) { float f = -1.5; int m = -(-1); int lo = -9223372036854775807 - 1; return to_string(f) + \",\" + to_string(m) + \",\" + to_string(lo) + \",\" + to_string(-x) + \",\" + type_of(-1) + \",\" + to_string(-0.0); };";
			const std::string script = std::string(defs) + " to_string(e1()) + \",\" + to_string(e2(BM)) + \",\" + e3(7);";
			both(script, "15,16,-1.5,1,-9223372036854775808,-7,int,-0.0", "negative literals and environment bounds");
			both(script, "", "proofs off", false);
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(2), closed, "the environment-bound loop and the -1 head close (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 600) + ")");
			size_t unaries = 0; for (const char* op : {"\tUNARY ", "\tUNARY_TEMP "}) { for (size_t at = listing.find(op); at != std::string::npos; at = listing.find(op, at + 1)) { ++unaries; } }
			check_eq(size_t(1), unaries, "the one unary left is -x on the variable: " + listing);
		});
		test("calls_inside_a_block", [this]() {
			// draw_seg's shape: a call landing in a declaration, a statement call with a computed and a reference argument, float
			// math around them, an element read of a var& container guarding a continue; and every effect a callee can have on the
			// block, re-read after the call: a global the loop reads, an array the loop reads (grown by the callee), an int& taken
			// on a loop local (the slot is the value from then on), an int result into a float declaration (the widening) and a
			// float result into an int declaration (its rejection, the earlier iterations' sum kept), a throw out of the callee into the caller's catch (materialized locals), a recursive callee, a
			// zero-argument callee, a string result into a var, and the argument values: a negation, a math call, an element read,
			// an environment name
			const char* defs = R"JAI(
int K = 1; array<int> G = [1, 2, 3, 4, 5, 6]; var FLATS = [[10, 20], [30, 40]]; array<int> DONE = [0, 1, 0, 0, 1, 0];
var light_band = function(int light, float dist) -> int { return light + math::itrunc(dist); };
var col = function(int x, int y0, int y1, var& flat, float ph, float rdx, float rdy, int light) -> int { return x + y0 + y1 + flat[0] + math::itrunc(ph + rdx + rdy) + light; };
var seg = function(int n, var& done) { int acc = 0; float dist = 1.5; int light = 3; var& cflat = FLATS[1]; float fceil = 2.0;
  for (int x = 0; x < n; ++x) { if (done[x] != 0) { continue; } float rdx = x * 0.5; float rdy = rdx + 1.0; int band = light_band(light, dist); acc = col(x, band + 1, acc, cflat, fceil, rdx, rdy, light); }
  return acc; };
var bump = function() -> int { K = K + 1; return K; };
var globals = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + K; int r = bump(); acc = acc + r; } return acc; };
var grow = function(var& a) -> int { a.push(9); return a.size(); };
var arrays = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + G[i]; int s = grow(G); acc = acc + s; } return acc; };
var inc = function(int& v) -> void { v = v + 100; };
var escape = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { int t = i * 2; inc(t); acc = acc + t; } return acc; };
var twice = function(int a) -> int { return a * 2; };
var widen = function(int n) { float acc = 0.25; for (int i = 0; i < n; ++i) { float q = twice(i * 2); acc = acc + q; } return acc; };
var half = function(int a) { if (a < 2) { return a * 2; } return a * 0.5; };
var rejects = function(int n) { int acc = 0; try { for (int i = 0; i < n; ++i) { acc = acc + i; int q = half(i); acc = acc + q; } } catch (e) { return to_string(acc) + " " + e; } return to_string(acc); };
var boom = function(int i) -> int { if (i == 2) { throw "boom at " + to_string(i); } return i; };
var thrown = function(int n) { int acc = 0; float f = 0.5; try { for (int i = 0; i < n; ++i) { int b = boom(i); acc = acc + b; f = f * 2.0; } } catch (e) { return to_string(acc) + " " + to_string(f) + " " + e; } return to_string(acc); };
var fact = function(int k) -> int { if (k <= 1) { return 1; } return k * fact(k - 1); };
var recursive = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { int fk = fact(i); acc = acc + fk; } return acc; };
var tick = function() -> int { return 7; };
var zero = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { int t = tick(); acc = acc + t; } return acc; };
var name = function(int i) -> string { return "n" + to_string(i); };
var strings = function(int n) { var s = ""; for (int i = 0; i < n; ++i) { var w = name(i); s = s + w; } return s; };
var values = function(int n) { int acc = 0; float r = 2.5; for (int i = 0; i < n; ++i) { int a1 = light_band(-i, r); int a2 = light_band(math::itrunc(r), 1.0); int a3 = light_band(G[i], 2.0); int a4 = light_band(K, 0.5); acc = acc + a1 + a2 + a3 + a4; } return acc; };
)JAI";
			struct probe { const char* call; const char* expected; };
			const probe probes[] = {
				{ "to_string(seg(6, DONE));", "184" }, { "to_string(globals(3));", "15" }, { "to_string(arrays(3));", "30" }, { "to_string(escape(3));", "306" },
				{ "to_string(widen(3));", "12.25" }, { "rejects(3);", "5 Cannot initialize 'q' (declared int) with float" }, { "thrown(4);", "1 2.0 boom at 2" }, { "to_string(recursive(5));", "34" },
				{ "to_string(zero(3));", "21" }, { "strings(3);", "n0n1n2" }, { "to_string(values(3));", "27" },
			};
			for (const probe& p : probes) {
				both(std::string(defs) + " " + p.call, p.expected, p.call);
				both(std::string(defs) + " " + p.call, "", (std::string(p.call) + " proofs off").c_str(), false);
			}
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(11), closed, "every loop with a call closes (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 800) + ")");
			size_t calls = 0; for (size_t at = listing.find("\tC_CALL "); at != std::string::npos; at = listing.find("\tC_CALL ", at + 1)) { ++calls; }
			size_t program_calls = 0;   // a frame program's copy holds its own C_CALL per call
			for (size_t at = listing.find("-- frame program "); at != std::string::npos; at = listing.find("-- frame program ", at + 1)) {
				const size_t c = listing.find("(calls ", at), eol = listing.find('\n', at);
				if (c != std::string::npos && c < eol) { program_calls += std::stoul(listing.substr(c + 7)); }
			}
			check_eq(size_t(15) + program_calls, calls, "one C_CALL per call in a closed loop: " + listing);
		});
		test("globals_written_inside_a_block", [this]() {
			// draw_seg's REMAIN = REMAIN - 1: an environment scalar written by a fused store, a compound, an increment, a move and a
			// call result; the head reading the written value; the value seen by a callee (written back ahead of the call) and
			// rewritten by one (re-read after it); a var global changing kind; an int global fed a var's float (the general store's text, the earlier store kept);
			// a float global fed ints; a throw after the write (the catch sees the written value)
			const char* defs = R"JAI(
int REMAIN = 10; var V = 1; int T = 1; float F = 0.0; int SEEN = 0; var STEPS = [3, 1.5];
var peek = function() -> int { return REMAIN; };
var poke = function(int v) -> void { REMAIN = v; };
var boom = function() -> void { throw "boom"; };
var count_down = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { REMAIN = REMAIN - 1; acc = acc + REMAIN; } return acc; };
var compound = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { REMAIN -= 2; REMAIN--; acc = acc + REMAIN; } return acc; };
var head = function() { int acc = 0; while (REMAIN > 0) { REMAIN = REMAIN - 1; acc = acc + 1; } return acc; };
var seen_by_callee = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { REMAIN = REMAIN + 1; int p = peek(); acc = acc + p; } return acc; };
var written_by_callee = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { REMAIN = REMAIN + 1; poke(100); acc = acc + REMAIN; } return acc; };
var var_kind = function(int n) { for (int i = 0; i < n; ++i) { V = V * 1.5; } return to_string(V) + " " + type_of(V); };
var narrow = function(int n) { try { for (int i = 0; i < n; ++i) { T = T * STEPS[i]; } } catch (e) { return e + " " + to_string(T); } return to_string(T); };
var widen = function(int n) { for (int i = 0; i < n; ++i) { F = F + i; } return to_string(F); };
var thrown = function(int n) { try { for (int i = 0; i < n; ++i) { REMAIN = REMAIN + 10; if (i == 1) { boom(); } } } catch (e) { return to_string(REMAIN) + " " + e; } return to_string(REMAIN); };
var mov = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { REMAIN = i; SEEN = 7; acc = acc + REMAIN + SEEN; } return acc; };
var from_call = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { REMAIN = peek(); acc = acc + REMAIN; } return acc; };
)JAI";
			struct probe { const char* call; const char* expected; };
			const probe probes[] = {
				{ "to_string(count_down(3));", "24" }, { "to_string(compound(2));", "11" }, { "to_string(head());", "10" }, { "to_string(seen_by_callee(3));", "36" },
				{ "to_string(written_by_callee(3));", "300" }, { "var_kind(2);", "2.25 float" }, { "narrow(2);", "Cannot assign float to 'T' (declared int) 3" }, { "widen(3);", "3.0" },
				{ "thrown(3);", "30 boom" }, { "to_string(mov(3));", "24" }, { "to_string(from_call(2));", "20" },
				{ "count_down(2); to_string(REMAIN);", "8" }, { "head(); to_string(REMAIN);", "0" },
			};
			for (const probe& p : probes) {
				both(std::string(defs) + " " + p.call, p.expected, p.call);
				both(std::string(defs) + " " + p.call, "", (std::string(p.call) + " proofs off").c_str(), false);
			}
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(11), closed, "every loop writing a global closes (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 800) + ")");
		});
		test("element_store_values_are_values", [this]() {
			// draw_seg's `clipsn[x] = cn + 1`: an element store whose value is a value binary lands the binary in a compiler temp and
			// stores the temp; the store's typed lane and the block hold; a lifted tree, a float value, a value that raises (the
			// kernel's text first), an int array fed a var's float value (the general store's text, the earlier stores kept), a map container and a var& container; a global
			// stored through a computed index, in an arm the first call never takes (the general store parks its container in the value's
			// lookup role: the value reads through a role the index leaves free, or the store refuses when the index takes both)
			const char* defs = R"JAI(
array<int> G = [0, 0, 0, 0, 0, 0]; array<float> F = [0.0, 0.0, 0.0, 0.0]; var M = {1: 0, 2: 0}; var V = [0, 0, 0, 0]; var S = [3, 4, 2.5];
var fill = function(int n, var& a) { int acc = 0; for (int x = 0; x < n; ++x) { int v = a[x]; a[x] = v + x * 2 + 1; acc = acc + a[x]; } return acc; };
var tree = function(int n) { for (int i = 0; i < n; ++i) { G[i] = i * 3 + (i - 1) * 2; } return to_string(G); };
var floats = function(int n) { for (int i = 0; i < n; ++i) { F[i] = i * 0.5 + 1.0; } return to_string(F); };
var raises = function(int n) { int d = 0; try { for (int i = 0; i < n; ++i) { G[i] = i / d; } } catch (e) { return e; } return "no"; };
var narrow = function(int n) { try { for (int i = 0; i < n; ++i) { G[i] = S[i] * 2; } } catch (e) { return e + " " + to_string(G); } return to_string(G); };
var keyed = function(int n) { for (int i = 1; i <= n; ++i) { M[i] = i * 10 + 1; } return to_string(M); };
var mixed = function(int n) { for (int i = 0; i < n; ++i) { V[i] = i + 0.5; } return to_string(V); };
int W = 99; int K = 1;
var flash = function(int s, int mode) { for (int x = 0; x < 4; ++x) { if (mode == 1) { G[x + s] = W; } else { G[x + s] = 5; } } return to_string(G); };
var flashadd = function(int s, int mode) { for (int x = 0; x < 4; ++x) { if (mode == 1) { G[x + s] += W; } else { G[x + s] += 1; } } return to_string(G); };
var flashk = function(int mode) { for (int x = 0; x < 4; ++x) { if (mode == 1) { G[x + K] = W; } else { G[K + x] = 5; } } return to_string(G); };
var flashkk = function(int mode) { for (int x = 0; x < 4; ++x) { if (mode == 1) { G[K + K] = W; } else { G[x] = 5; } } return to_string(G); };
)JAI";
			struct probe { const char* call; const char* expected; };
			const probe probes[] = {
				{ "to_string(fill(4, G));", "16" }, { "tree(4);", "" }, { "floats(4);", "" }, { "raises(3);", "" }, { "narrow(3);", "Cannot assign 'float' to element of type 'int' [6, 8, 0, 0, 0, 0]" }, { "keyed(2);", "" }, { "mixed(3);", "" },
				{ "flash(1, 0) + flash(1, 1);", "" }, { "flashadd(2, 0) + flashadd(2, 1);", "" }, { "flashk(0) + flashk(1);", "" }, { "flashkk(0) + flashkk(1);", "" },
			};
			for (const probe& p : probes) {
				both(std::string(defs) + " " + p.call, p.expected, p.call);
				both(std::string(defs) + " " + p.call, "", (std::string(p.call) + " proofs off").c_str(), false);
			}
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_ge(closed, size_t(5), "the stores with value binaries close (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 800) + ")");
			size_t pushing = 0; for (size_t at = listing.find("\tBINARY_FUSED op="); at != std::string::npos; at = listing.find("\tBINARY_FUSED op=", at + 1)) { ++pushing; }
			check_eq(size_t(0), pushing, "no pushing fused binary feeds an element store: " + listing);
		});
		test("bools_are_cells", [this]() {
			// draw_seg's `if (solid)`, `if (fsky && bsky)`, `if (!solid && ...)`, `bool painted = false; painted = true;`: bool locals
			// and globals are cells; a truth test is the language's truthiness of an int, a float or a bool (a local or an
			// environment value, as a head or in a body), `!x` and a comparison land bools, a bool argument boxes, a bool result
			// lands, a bool into an int declaration takes the general conversion
			const char* defs = R"JAI(
bool NIGHTMARE = true; bool RUNNING = true; int TICKS = 0; var FLAG = false;
var is_odd = function(int x) -> bool { return (x & 1) == 1; };
var take = function(bool b, int x) -> int { if (b) { return x; } return -x; };
var seg = function(int n, bool solid, bool fsky, bool bsky) { int acc = 0; for (int x = 0; x < n; ++x) { bool painted = false; if (solid) { acc = acc + 1; painted = true; } if (fsky && bsky) { acc = acc + 10; } if (!solid && x > 1) { acc = acc + 100; } if (painted) { acc = acc + 1000; } } return acc; };
var values = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { bool lt = i < 2; bool odd = is_odd(i); var neg = !lt; int t = take(lt, i); acc = acc + t; if (odd) { acc = acc + 1; } if (neg) { acc = acc + 5; } } return acc; };
var env_tests = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { if (NIGHTMARE) { acc = acc + 1; } if (!RUNNING) { acc = acc + 100; } if (FLAG) { acc = acc + 1000; } FLAG = i == 1; } return acc; };
var head = function() { int acc = 0; while (RUNNING) { TICKS = TICKS + 1; acc = acc + 1; if (TICKS >= 4) { RUNNING = false; } } return acc; };
var narrow = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { bool b = i < 2; int q = b; acc = acc + q; } return acc; };
var floats = function(int n) { int acc = 0; float f = -1.0; for (int i = 0; i < n; ++i) { f = f + 0.5; if (f) { acc = acc + 1; } var nz = !f; if (nz) { acc = acc + 10; } } return acc; };
)JAI";
			struct probe { const char* call; const char* expected; };
			const probe probes[] = {
				{ "to_string(seg(4, true, true, false));", "4004" }, { "to_string(seg(4, false, true, true));", "240" }, { "to_string(values(4));", "8" },
				{ "to_string(env_tests(3));", "1003" }, { "to_string(head()) + \" \" + to_string(RUNNING);", "4 false" }, { "to_string(narrow(3));", "2" }, { "to_string(floats(4));", "13" },
			};
			for (const probe& p : probes) {
				both(std::string(defs) + " " + p.call, p.expected, p.call);
				both(std::string(defs) + " " + p.call, "", (std::string(p.call) + " proofs off").c_str(), false);
			}
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(6), closed, "every loop over bools closes (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 800) + ")");
		});
		test("compound_binary_operands_land_in_temps", [this]() {
			// x op= a op b inside a loop: the binary lands in a temp and the compound reads it; the results, the string rows, the errors
			// of the binary and of the compound are the general form's
			const std::string script = R"(
				function run() -> string {
					float fs = 0.0; int n = 0; string s = "";
					for (int i = 0; i < 6; ++i) { float v = i * 0.5; fs += math::sqrt(v) * 1.5 - v / 3.0; n += i * i - 1; s += to_string(i + 1); }
					string e1 = ""; int z = 0;
					try { for (int i = 0; i < 3; ++i) { n += i / z; } } catch (ex) { e1 = ex; }
					string e2 = ""; int big = 9223372036854775807;
					try { for (int i = 0; i < 3; ++i) { big += i * 2 + 1; } } catch (ex) { e2 = ex; }
					return `${fs}|${n}|${s}|${e1}|${e2}|${big}`;
				}
				run();
			)";
			const std::string interp = run_on(false, script);
			check_eq(interp, run_on(true, script), "compound temps: interpreter and vm agree");
			check_true(interp.find("|49|123456|") != std::string::npos, interp.c_str());
		});

		test("member_reads_of_slot_instances_close_loops", [this]() {
			// a member read in a truth test, a compound operand and a declaration lands in a member temp: the loop closes, and every
			// kernel fact holds (a user accessor runs per read, a call in the body that changes a field is seen, a null receiver raises)
			const std::string script = R"(
				class En { bool alive = true; float x = 0.0; En(float a) { x = a; alive = a > 3.0; } }
				class Acc { int n = 0; int reads = 0; function _get_n() -> int { reads += 1; return n; } }
				function bump(En& e) -> void { e.x = e.x + 100.0; }
				function run() -> string {
					array<En> ens = []; for (int i = 0; i < 8; ++i) { ens.push(En(i * 1.0)); }
					float s = 0.0; float t = 0.0;
					for (int r = 0; r < 3; ++r) {
						for (auto& e : ens) { if (!e.alive) { continue; } s += e.x; float v = e.x; t = t + v; }
					}
					En p = En(5.0); float u = 0.0;
					for (int i = 0; i < 4; ++i) { if (p.alive) { u += p.x; } if (i == 1) { bump(p); } }
					Acc a = Acc(); a.n = 2; int w = 0;
					for (int i = 0; i < 5; ++i) { w += a.n; }
					string err = "";
					shared_ptr<En> q = null;
					try { for (int i = 0; i < 2; ++i) { u += q.x; } } catch (ex) { err = ex; }
					return `${s},${t},${u},${w},${a.reads}|${err}`;
				}
				run();
			)";
			both(script, "66.0,66.0,220.0,10,5|Cannot access 'x' on null ('q' is null)", "member temps");
			const std::string listing = listing_run(R"(
				class En { bool alive = true; float x = 0.0; }
				function f(array<En> ens) -> float { float s = 0.0; for (auto& e : ens) { if (!e.alive) { continue; } s += e.x; } return s; }
				array<En> xs = [En(), En()]; f(xs);
			)");
			check_true(listing.find("-- cell loop") != std::string::npos, "the range-for over instances closes");
		});

		test("size_heads_land_in_temps", [this]() {
			// k < xs.size() as a loop head: the size lands in a temp and the loop closes; the size is read live (a push inside the body
			// grows it), a string and a map answer size and length, a null receiver raises the call's text, a class with its own size()
			// keeps its method
			const std::string script = R"(
				class Box { int n = 3; function size() -> int { return n; } }
				function run() -> string {
					array<int> xs = [1, 2, 3]; int s = 0;
					for (int k = 0; k < xs.size(); ++k) { s += xs[k]; if (xs.size() < 6) { xs.push(k + 10); } }
					string t = "abcd"; int c = 0;
					for (int k = 0; k < t.length(); ++k) { c += k; }
					map<string, int> m = {"a": 1, "b": 2}; int d = 0;
					for (int k = 0; k < m.size(); ++k) { d += 10; }
					Box b = Box(); int e = 0;
					for (int k = 0; k < b.size(); ++k) { e += 1; }
					string err = "";
					var z = null;
					try { for (int k = 0; k < z.size(); ++k) { s += 1; } } catch (ex) { err = ex; }
					return `${s},${xs.size()},${c},${d},${e}|${err}`;
				}
				run();
			)";
			const std::string interp = run_on(false, script);
			check_eq(interp, run_on(true, script), "size temps: interpreter and vm agree");
			check_true(interp.find("39,6,6,20,3|") == 0, interp.c_str());
			const std::string listing = listing_run(R"(
				function f(array<int> xs) -> int { int s = 0; for (int k = 0; k < xs.size(); ++k) { s += xs[k]; } return s; }
				f([1, 2, 3]);
			)");
			check_true(listing.find("-- cell loop") != std::string::npos, "the size head closes");
			// a global a reloaded module redeclares as an instance whose script size() runs in its own frame: the compiled size site calls it
			const auto dir = jai::foundry::test_temp_directory();
			int tick = 0;
			for (const bool vm : {false, true}) {
				const std::string module = vm ? "size_reload_vm.jai" : "size_reload_interp.jai";
				auto write_module = [&](const std::string& body) {
					{ std::ofstream(dir / module, std::ios::binary | std::ios::trunc) << body; }
					std::error_code ec;
					std::filesystem::last_write_time(dir / module, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
				};
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e);
				e->add_include_path(dir.string());
				e->import_recheck_interval(0);
				std::string got;
				try {
					write_module("array<int> G = [1, 2, 3];\n");
					e->execute("import(\"" + module + "\"); int n() { int s = 0; for (int i = 0; i < 2; ++i) { s += G.size(); } return s + G.size(); } n();");
					write_module("class S { int size() { return 42; } }\nS G;\n");
					got = std::to_string(e->execute("import(\"" + module + "\"); n();").as_int());
				} catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(std::string("126"), got, vm ? "a script size() behind a compiled size site, vm" : "a script size() behind a compiled size site, interpreter");
			}
		});

		test("compound_element_operands_land_in_temps", [this]() {
			// x op= a[i] (and a size, a unary or a math operand) inside a loop: the operand lands in a temp and the compound reads it; the
			// sums, the float promotion, an out-of-range read, an overflow and a string element are the general form's
			const std::string script = R"(
				function run() -> string {
					array<int> xs = [1, 2, 3, 4]; array<float> fs = [0.5, 1.5];
					int s = 0; float g = 0.0; int n = 0; float h = 1.0;
					for (int k = 0; k < xs.size(); ++k) { s += xs[k]; g += xs[k]; n -= -xs[k]; h *= fs[k % 2]; n += xs.size(); }
					string e1 = "";
					try { for (int k = 0; k < 9; ++k) { s += xs[k]; } } catch (ex) { e1 = ex; }
					array<int> big = [9223372036854775807, 1]; int b = 0; string e2 = "";
					try { for (int k = 0; k < 2; ++k) { b += big[k]; } } catch (ex) { e2 = ex; }
					array<string> ws = ["a", "b"]; string w = "";
					for (int k = 0; k < 2; ++k) { w += ws[k]; }
					return `${s},${g},${n},${h},${w}|${e1}|${e2}`;
				}
				run();
			)";
			const std::string interp = run_on(false, script);
			check_eq(interp, run_on(true, script), "compound element temps: interpreter and vm agree");
			check_true(interp.find("20,10.0,26,0.5625,ab|") == 0, interp.c_str());
			const std::string listing = listing_run(R"(
				function f(array<int> xs) -> int { int s = 0; for (int k = 0; k < xs.size(); ++k) { s += xs[k]; } return s; }
				f([1, 2, 3]);
			)");
			check_true(listing.find("C_APPEND") == std::string::npos, listing.c_str());
		});

		test("every_back_edge_a_block_runs_polls_the_budget", [this]() {
			// a loop whose body is a closed inner loop (counted, while, nested), a function whose loops one region covers, an endless loop under a
			// try: every back edge the block runs takes part in the budget's tick, and a spent budget raises the same terminal text the general
			// form raises, under the clock-free step budget and the wall-clock budget, both backends, with an injected miss at every position
			const char* shapes[] = {
				"function f() { int i = 0; while (i < 2) { for (int j = 0; j < 8; ++j) {} } } f();",
				"function f() { int i = 0; while (i < 2) { int j = 0; while (j < 8) { j = j + 1; } } } f();",
				"function f() { int s = 0; for (int i = 0; i < 2; i = i) { for (int j = 0; j < 4; ++j) { for (int k = 0; k < 3; ++k) { s = (s + k) % 7; } } } } f();",
				"array<int> A = [1, 2, 3, 4];\nint g(int k, array<int>& a) { int s = k; while (true) { for (int q = 0; q < 4; ++q) { s = (s + a[q]) % 1000; } if (s < 0) { break; } } return s; }\ng(3, A);",
				"function f() { int i = 0; while (i < 2) { for (int j = 0; j < 8; ++j) {} } } var r = 0; try { f(); } catch (e) { r = 7; } r;",
			};
			auto run = [](bool vm, const char* src, uint64_t steps, double seconds, uint32_t miss_every) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e);
				if (steps) { e->execution_step_budget(steps); }
				e->execution_budget(seconds);
				if (miss_every) { e->cells_miss_every(miss_every); }
				try { return e->execute(src).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (const char* s : shapes) {
				for (bool vm : { false, true }) {
					const std::string by_steps = run(vm, s, 64, 30.0, 0);
					check_true(by_steps.find("Script execution budget exceeded") != std::string::npos, std::string(vm ? "vm" : "interpreter") + " step budget: " + by_steps + "\n" + s);
					const std::string by_clock = run(vm, s, 0, 0.2, 0);
					check_true(by_clock.find("Script execution budget exceeded") != std::string::npos, std::string(vm ? "vm" : "interpreter") + " wall budget: " + by_clock + "\n" + s);
				}
				if (!engine::miss_injection_available()) continue;
				for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
					const std::string got = run(true, s, 64, 30.0, every);
					check_true(got.find("Script execution budget exceeded") != std::string::npos, "vm step budget under an injected miss at every " + std::to_string(every) + ": " + got);
				}
			}
		});
		test("an_endless_loop_around_a_closed_loop_and_a_host_call_ends_at_every_step_budget", [this]() {
			// jaiscript_fuzz seeds 5792 and 9516 (--max-depth 3) on the vm ran past the budget in most runs: the loop counter is reset inside the
			// body (i /= 94; a loop storing 0 into it), the body holds a closed counted loop, a host call and a continue; whether the deadline
			// fell on the block's back edge or on the general form's decided it, so the vm tries every step budget up to 24 polls (the interpreter,
			// which has no blocks, a few)
			const char* shapes[] = {
				"function f() { for (int i = 0; i < 6; ++i) { array<int> a = [41, 35, 36, 31, 27, 14, 60, 19]; int s = 7;\n"
				"for (int k = 0; k < 8; k += 1) { s = s / 5 + a[k]; s = (s + i % 97) % 100003; }\n"
				"sink(s, a[7]); i /= (91 + 3); auto b = (!false); continue; } } f();",
				"function f() { var m = {\"k0\": 1}; for (int i = 0; i < 5; ++i) { sink(m); array<int> a = [-2, 13, 35, 7, -3]; int s = 8;\n"
				"for (int k = 0; k < 5; k++) { int t = a[k] + k; s = (s + t) % 100003; for (int j = 0; j < 2; j++) { a[j] += 4; } int u = a[k] + k; s = (s + u) % 100003; }\n"
				"sink(s, a[1]); for (int r = 0; r < 6; r++) { i = (-((r * 0))); } } } f();",
			};
			auto run = [](bool vm, const char* src, uint64_t steps) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				e->add_variadic_function("sink", [raw = e.get()](const std::vector<script_value>&) { return script_value(std::monostate{}, raw); });
				e->execution_step_budget(steps);
				e->execution_budget(30.0);
				try { return e->execute(src).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (const char* s : shapes) {
				for (bool vm : { false, true }) {
					for (uint64_t steps = 1; steps <= 24; steps += vm ? 1 : 7) {
						const std::string got = run(vm, s, steps);
						check_true(got.find("Script execution budget exceeded") != std::string::npos, std::string(vm ? "vm" : "interpreter") + " step budget " + std::to_string(steps) + ": " + got + "\n" + s);
					}
				}
			}
		});
		test("a_math_pairs_first_argument_takes_its_value_before_the_second_calls", [this]() {
			// math::min(G, bump() ? 4 : 8) in a loop: the first argument takes its value before the second argument's call writes it (C++17
			// argument order), for a global, a bare field, this.x, another object's field, a global element, a reference local and a captured
			// local, in nested loops and straight-line code, both backends, proofs on and off, and under an injected miss
			const std::string script = R"(
				int G = 5; array<int> A = [5, 0];
				bool bump() { G = G - 2; A[0] = A[0] - 2; return false; }
				class C {
					int x = 5;
					bool dec() { x = x - 2; return false; }
					function bare(int n) -> int { int s = 0; for (int i = 0; i < n; i++) { for (int j = 0; j < 2; j++) { s += math::min(x, dec() ? 4 : 8); } } return s; }
					function own(int n) -> int { int s = 0; for (int i = 0; i < n; i++) { s += math::max(this.x, dec() ? 0 : -100); } return s; }
				}
				function globals(int n) -> int { int s = 0; for (int i = 0; i < n; i++) { int k = 0; while (k < 2) { s += math::min(G, bump() ? 4 : 8); k++; } } return s; }
				function elements(int n) -> int { int s = 0; for (int i = 0; i < n; i++) { s += math::max(A[0], bump() ? 0 : -100); } return s; }
				function fields(int n) -> int { C c = C(); int s = 0; for (int i = 0; i < n; i++) { s += math::min(c.x, c.dec() ? 4 : 8); } return s; }
				function refs(int n) -> int { int& r = G; int s = 0; for (int i = 0; i < n; i++) { s += math::min(r, bump() ? 4 : 8); } return s; }
				function captured(int n) -> int { int L = 5; var dl = [&]() { L = L - 2; return false; }; int s = 0; for (int i = 0; i < n; i++) { s += math::min(L, dl() ? 4 : 8); } return s; }
				function pows(int n) -> int { float s = 0.0; for (int i = 0; i < n; i++) { s += math::pow(G, bump() ? 1 : 2); } return math::itrunc(s); }
				function straight() -> int { int s = math::min(G, bump() ? 4 : 8); s += math::min(G, bump() ? 4 : 8); return s; }
				function run() -> string {
					string out = `${globals(2)}`;
					G = 5; A[0] = 5; out += `,${elements(3)}`;
					G = 5; out += `,${fields(3)},${C().bare(2)},${C().own(3)}`;
					G = 5; out += `,${refs(3)}`;
					out += `,${captured(3)}`;
					G = 5; out += `,${pows(3)}`;
					G = 5; out += `,${straight()}`;
					return out;
				}
				run();
			)";
			const std::string expected = "8,9,9,8,9,9,9,35,8";
			both(script, expected, "a pair's first argument before the second's call");
			both(script, expected, "proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(script).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(expected, got, "under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_block_holds_up_to_256_cells", [this]() {
			// draw_seg's column loop with its column bodies grafted holds more than 128 cells (its locals, the outer locals it reads, its
			// compiler values, its argument values, its environment values and the grafts' locals); loops over 80 and 140 locals close
			// and land the same, the boundary sits at 256 cells
			auto wide = [](int n) {
				std::string s = "var f = function(int n) { ";
				for (int i = 0; i < n; ++i) { s += "int a" + std::to_string(i) + " = " + std::to_string(i) + "; "; }
				s += "for (int k = 0; k < 3; ++k) { ";
				for (int i = 0; i < n; ++i) { s += "a" + std::to_string(i) + " = a" + std::to_string(i) + " + k; "; }
				s += "} int acc = 0; ";
				for (int i = 0; i < n; ++i) { s += "acc = acc + a" + std::to_string(i) + "; "; }
				s += "return acc; };";
				return s;
			};
			// the limit's boundary: n locals and the counter are n + 1 cells; 255 locals close, 256 stay open
			for (int n : {63, 64, 65, 80, 127, 128, 140, 255}) { both(wide(n) + " to_string(f(3));", std::to_string(n * (n - 1) / 2 + n * 3), (std::to_string(n) + " locals").c_str()); const std::string l = listing_of(wide(n)); check(l.find("-- cell loop ") != std::string::npos, std::to_string(n) + " locals close: " + l.substr(l.find("-- ") == std::string::npos ? l.size() : l.find("-- "), 200)); }
			both(wide(256) + " to_string(f(3));", std::to_string(256 * 255 / 2 + 256 * 3), "256 locals"); check(listing_of(wide(256)).find("too many cells") != std::string::npos, "256 locals stay open");
		});
		test("body_declared_locals_enter_empty", [this]() {
			// draw_seg's column loop declares locals inside branches the first iterations never take, so their slots held
			// placeholders at every entry and the block was refused; a local the body declares enters empty, its declaration is
			// the first write (an int takes ints, a float widens, a var takes any kind), and the loop lands the same on both backends
			const char* defs = R"JAI(
var branches = function(int n) { int acc = 0; for (int x = 0; x < n; ++x) { if (x > 2) { int v = x * 2; float w = v; acc = acc + v + math::itrunc(w); } if (x == 4) { var any = x; acc = acc + any; } float f = x * 0.5; acc = acc + math::itrunc(f); } return acc; };
var late = function(int n) { int acc = 0; for (int x = 0; x < n; ++x) { if (x >= 1) { int q = x; acc = acc + q; } } return acc; };
var clamped = function(int n) { int acc = 0; for (int x = 0; x < n; ++x) { int c1 = x - 1; if (c1 > 2) { c1 = 2; } var w = x; if (x > 3) { w = 3; } acc = acc + c1 + w; } return acc + w_kind(n); };
var w_kind = function(int n) { var w = 1; for (int x = 0; x < n; ++x) { var v = x; if (x > 1) { v = x * 0.5; } w = v; } return math::itrunc(w * 10); };
)JAI";
			both(std::string(defs) + " to_string(branches(6));", "58", "locals declared in branches");
			both(std::string(defs) + " to_string(branches(6));", "", "proofs off", false);
			both(std::string(defs) + " to_string(late(5));", "10", "a local declared past the first iteration");
			both(std::string(defs) + " to_string(clamped(6));", "43", "body-declared locals re-assigned in a branch");
			both(std::string(defs) + " type_of(w_kind(4)) + to_string(w_kind(4));", "int15", "a var declared in the body changes kind");
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(4), closed, "the four loops close (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 400) + ")");
		});
		test("a_landing_sees_what_the_call_changed", [this]() {
			// the landing after a call re-checks the containers by identity and re-reads the environment cells through their
			// bindings: a callee that rebinds the container a var& local aliases, that rebinds a container the loop names, that
			// grows the container, that writes a scalar global the loop reads, or that needs a frame environment (the epoch
			// moves) lands the same on both backends
			const char* defs = R"JAI(
array<int> D = [1, 2, 3, 4, 5, 6, 7, 8];
array<int> E = [10, 20, 30, 40, 50, 60, 70, 80];
var D2 = [1, 2, 3, 4, 5, 6, 7, 8];
array<int> W = [5, 6, 7, 8];
int G = 100;
function swap_d(int i) -> void { if (i == 3) { D = E; } }
function swap_d2(int i) -> void { if (i == 3) { D2 = [10, 20, 30, 40, 50, 60, 70, 80]; } }
function bump_g(int i) -> void { G = G + i; }
function grow_w(int i) -> void { if (i == 2) { W.push(1000); W.push(2000); } }
function env_callee(int i) -> int { var f = [=]() { return i * 2; }; return f(); }
var through_ref = function(int n) { var& d = D; int acc = 0; for (int i = 0; i < n; ++i) { swap_d(i); acc = acc + d[i]; } return acc; };
var by_name = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { swap_d(i); acc = acc + D[i]; } return acc; };
var through_ref2 = function(int n) { var& d = D2; int acc = 0; for (int i = 0; i < n; ++i) { swap_d2(i); acc = acc + d[i]; } return acc; };
var reads_g = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { bump_g(i); acc = acc + G; } return acc; };
var grows = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { grow_w(i); acc = acc + W[i]; } return acc; };
var via_env = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { int v = env_callee(i); acc = acc + v + G; } return acc; };
)JAI";
			struct probe { const char* call; const char* expected; };
			const probe probes[] = {
				{ "to_string(through_ref(8));", "306" }, { "to_string(by_name(8));", "306" }, { "to_string(through_ref2(8));", "306" },
				{ "to_string(reads_g(5));", "520" }, { "to_string(grows(6));", "3026" }, { "to_string(via_env(4));", "412" },
			};
			for (const probe& p : probes) {
				both(std::string(defs) + " " + p.call, p.expected, p.call);
				both(std::string(defs) + " " + p.call, "", (std::string(p.call) + " proofs off").c_str(), false);
			}
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(6), closed, "the six loops close (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 800) + ")");
		});
		test("exits_leave_the_block", [this]() {
			// a break, a compare whose edge leaves the loop, and a return inside the loop (bare, with a value, with an element read
			// pushed for it) end the block: the cells flush and the general form runs the statement or continues at the target
			const char* defs = R"JAI(
array<int> A = [3, 1, 4, 1, 5, 9, 2, 6];
var first_over = function(int n, int lim) { int acc = 0; int found = -1; for (int i = 0; i < n; ++i) { acc = acc + A[i]; if (A[i] > lim) { found = i; break; } } return acc * 100 + found; };
var while_break = function(float lim) { int i = 0; int acc = 0; while (i < 100) { float f = i * 1.5; if (f > lim) { break; } acc = acc + i; i = i + 1; } return acc * 10 + i; };
var and_head = function(int n) { int i = 0; int acc = 0; while (i < n && acc < 20) { acc = acc + i; i = i + 1; } return acc * 100 + i; };
var ret_bare = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + i; if (acc > 6) { return acc; } } return -acc; };
var ret_elem = function(int n) { for (int i = 0; i < n; ++i) { int v = A[i]; if (v == 4) { return A[i + 1]; } } return -1; };
var ret_expr = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + i * 2; if (i == 2) { return acc + 1000; } } return acc; };
var nested_break = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { int t = i * 3; if (t > 6) { if (t > 9) { break; } acc = acc + 100; } acc = acc + t; } return acc; };
)JAI";
			struct probe { const char* call; const char* expected; };
			const probe probes[] = {
				{ "to_string(first_over(8, 4));", "1404" }, { "to_string(while_break(4.0));", "33" }, { "to_string(and_head(10));", "2107" },
				{ "to_string(ret_bare(10));", "10" }, { "to_string(ret_elem(8));", "1" }, { "to_string(ret_expr(6));", "1006" }, { "to_string(nested_break(6));", "118" },
			};
			for (const probe& p : probes) {
				both(std::string(defs) + " " + p.call, p.expected, p.call);
				both(std::string(defs) + " " + p.call, "", (std::string(p.call) + " proofs off").c_str(), false);
			}
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(7), closed, "every loop with an exit closes (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 800) + ")");
			check(listing.find("C_EXIT") != std::string::npos, "the exits are in the copies: " + listing);
		});
		test("a_callee_may_reload_arm_operators_or_unwind_through_the_block", [this]() {
			// the call door's boundaries through a native callee: one that rebinds the function the loop calls (the probe resolves
			// the callee at every call), one that rebinds a container the loop reads by name and one that rebinds the container a
			// var& local aliases (the landing checks the sites by identity), one that arms custom numeric operators (the landing
			// leaves the block for the general form from then on), and a throw out of a callee that unwinds through the looping
			// frame into its caller's catch (the record's cell state resets for the next call); the interpreter is the oracle
			const char* defs = R"JAI(
var f = function(int i) -> int { return i; };
var f2 = function(int i) -> int { return i * 100; };
array<int> G = [1, 2, 3, 4, 5, 6, 7, 8];
array<int> G2 = [10, 20, 30, 40, 50, 60, 70, 80];
var swap_f_at = function(int i, int at) -> void { if (i == at) { host_swap_f(); } };
var rebind_g_at = function(int i, int at) -> void { if (i == at) { host_rebind_g(); } };
var arm_at = function(int i, int at) -> void { if (i == at) { host_arm_ops(); } };
var calls_f = function(int n, int at) { int acc = 0; for (int i = 0; i < n; ++i) { swap_f_at(i, at); int r = f(i); acc = acc + r; } return acc; };
var reads_g = function(int n, int at) { int acc = 0; for (int i = 0; i < n; ++i) { rebind_g_at(i, at); acc = acc + G[i]; } return acc; };
var reads_alias = function(int n, int at) { var& g = G; int acc = 0; for (int i = 0; i < n; ++i) { rebind_g_at(i, at); acc = acc + g[i]; } return acc; };
var armed = function(int n, int at) { int acc = 0; float w = 0.5; for (int i = 0; i < n; ++i) { arm_at(i, at); int r = f(i); acc = acc + r * 2; w = w + r; } return to_string(acc) + " " + to_string(w); };
var boom = function(int i) -> int { if (i == 3) { throw "boom " + to_string(i); } return i; };
var looping = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { int b = boom(i); acc = acc + b * 10; } return acc; };
var across = function(int n) { string s = ""; try { s = to_string(looping(n)); } catch (e) { s = "caught " + e; } return s + "|" + to_string(looping(2)); };
)JAI";
			auto run_with = [&](bool vm, const std::string& script, bool proofs) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (!proofs) { e->type_proofs(false); }
				engine* raw = e.get();
				e->add_function("host_swap_f", [raw]() { raw->execute("f = f2;"); });
				e->add_function("host_rebind_g", [raw]() { raw->execute("G = G2;"); });
				e->add_function("host_arm_ops", [raw]() { detail::engine_access::set_has_custom_numeric_operators(*raw, true); });
				try { return e->execute(script).as_string(); }
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			struct probe { const char* call; const char* expected; };
			const probe probes[] = {
				{ "to_string(calls_f(6, 3));", "1203" }, { "to_string(calls_f(6, 0));", "1500" }, { "to_string(calls_f(6, 9));", "15" },
				{ "to_string(reads_g(6, 2));", "183" }, { "to_string(reads_alias(6, 2));", "183" }, { "to_string(reads_g(6, 0));", "210" },
				{ "armed(6, 3);", "30 15.5" }, { "armed(6, 0);", "30 15.5" }, { "across(6);", "caught boom 3|10" }, { "across(2);", "10|10" },
			};
			for (const probe& p : probes) {
				for (bool proofs : { true, false }) {
					const std::string script = std::string(defs) + " " + p.call;
					const std::string interp = run_with(false, script, proofs), vm = run_with(true, script, proofs);
					check_eq(interp, vm, std::string(p.call) + (proofs ? "" : " proofs off") + ": both backends");
					check_eq(std::string(p.expected), vm, std::string(p.call) + (proofs ? "" : " proofs off") + ": expected");
				}
			}
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(5), closed, "the five loops with calls close (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 800) + ")");
		});
		test("the_region_lists_its_operations_and_control_flow", [this]() {
			// the listing prints each closed region's canonical operations with their control flow: a while loop's head compare
			// exits, a diamond's compare targets an operation and its else arm jumps, a break is an exit, a counted loop's continue
			// jumps to the back edge; the region verifier (must-define over that graph) accepts every loop the suite closes
			const char* defs = R"JAI(
var diamond = function(int n) { int total = 0; int i = 0; while (i < n) { if ((i % 2) == 0) { total += i; } else { total -= i; } i = i + 1; } return total; };
var brk = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { if (i == 3) { break; } acc = acc + i; } return acc; };
var cont = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { if (i == 1) { continue; } acc = acc + i; } return acc; };
)JAI";
			both(std::string(defs) + " to_string(diamond(6)) + \",\" + to_string(brk(10)) + \",\" + to_string(cont(10));", "-3,3,44", "the three loops");
			const std::string listing = listing_of(defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(3), closed, "the three loops close (open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 800) + ")");
			size_t ir_lines = 0; for (size_t at = listing.find("-- ir "); at != std::string::npos; at = listing.find("-- ir ", at + 1)) { ++ir_lines; }
			size_t programs = 0; for (size_t at = listing.find("-- frame program "); at != std::string::npos; at = listing.find("-- frame program ", at + 1)) { ++programs; }
			check_eq(size_t(3) + programs, ir_lines, "one IR line per closed region: " + listing);
			check(listing.find("-- ir 0:cmp->exit") != std::string::npos, "the while loop's head compare exits: " + listing);
			check(listing.find(":jump->") != std::string::npos, "the diamond's else arm is a jump to an operation: " + listing);
			check(listing.find(":exit->exit") != std::string::npos, "the break is an exit: " + listing);
			check(listing.find(":cmp->back") != std::string::npos, "the continue's compare jumps to the back edge: " + listing);
		});
		test("pinned_operations_skip_their_kind_checks", [this]() {
			// a loop over declared ints and floats runs its arithmetic and compares in the pinned forms; a var local (an adopted kind)
			// keeps the checked forms and a kind change inside the loop still lands the same; an int declared from a cast float
			// converts as the cast does on both backends
			const char* defs = R"JAI(
var ints = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { int t = i * 3 - 1; if (t > 4) { acc = acc + t; } } return acc; };
var flts = function(int n) { float acc = 0.0; float k = 0.5; int i = 0; while (i < n) { acc = acc + k * 1.5; if (acc > 3.0) { acc = acc - 1.0; } i = i + 1; } return math::itrunc(acc * 100.0); };
var adopted = function(int n) { var v = 1; int acc = 0; for (int i = 0; i < n; ++i) { v = v + i; if (i == 2) { v = v * 1.5; } acc = acc + math::itrunc(v); } return to_string(acc) + " " + type_of(v); };
var narrows = function(int n) { int acc = 0; float f = 0.5; for (int i = 0; i < n; ++i) { f = f + 1.25; int q = cast<int>(f); acc = acc + q; } return acc; };
)JAI";
			struct probe { const char* call; const char* expected; };
			const probe probes[] = { { "to_string(ints(6));", "38" }, { "to_string(flts(8));", "300" }, { "adopted(5);", "31 float" }, { "to_string(narrows(4));", "13" } };
			for (const probe& p : probes) {
				both(std::string(defs) + " " + p.call, p.expected, p.call);
				both(std::string(defs) + " " + p.call, "", (std::string(p.call) + " proofs off").c_str(), false);
			}
			const std::string listing = listing_of(defs);
			check(listing.find("C_ARITH_P") != std::string::npos && listing.find("C_JCMP_P") != std::string::npos && listing.find("C_CMP_P") != std::string::npos, "the pinned forms are emitted: " + listing);
			const std::string adopted_listing = listing_of("var adopted = function(int n) { var v = 1; int acc = 0; for (int i = 0; i < n; ++i) { v = v + i; acc = acc + math::itrunc(v); } return acc; };");
			check(adopted_listing.find("C_ARITH_ANY") != std::string::npos, "an adopted kind keeps the run-time kinds: " + adopted_listing);
			// the switch off: the same pinned forms and the same results (the differential the fuzz's --no-passes chain runs)
			auto run_passes_off = [&](bool vm, const std::string& script) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				e->ir_passes(false);
				try { return e->execute(script).as_string(); }
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (const probe& p : probes) { check_eq(run_passes_off(false, std::string(defs) + " " + p.call), run_passes_off(true, std::string(defs) + " " + p.call), std::string(p.call) + " passes off: both backends"); }
			{
				auto e = jai::foundry::make_default_engine();
				e->set_backend(backend_type::vm);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				auto* symbolizer = detail::engine_access::get_symbolizer(*e);
				auto templates = detail::engine_access::get_registered_template_types(*e);
				jai::lexer lex(defs, symbolizer);
				auto tokens = lex.tokenize();
				jai::parser p(tokens, symbolizer, e.get(), templates);
				auto parse_result = p.parse();
				check_true(parse_result.has_value(), "the pinned defs parse");
				(void)jai::detail::run_static_check(*e, parse_result.value());
				jai::vm::vm_compiler compiler(symbolizer);
				compiler.ir_passes(false);
				const std::string off = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
				check(off.find("C_ARITH_P") != std::string::npos && off.find("C_JCMP_P") != std::string::npos && off.find("C_CMP_P") != std::string::npos, "the switch off keeps the pinned forms: " + off);
			}
		});
		test("a_redeclared_global_after_a_call_lands_by_its_new_type", [this]() {
			// Regression: a native callee redeclares the global the loop holds as an environment cell (`var G` becomes
			// `int G = 5`, the raw kind unchanged); the next iteration's float store must take the int boundary (the general form's
			// conversion), so the landing misses on the stamp change and both backends land the same
			const char* defs = R"JAI(
var G = 1;
var retag_at = function(int i, int at) -> void { if (i == at) { host_retag(); } };
var probe = function(int n, int at) { int i = 0; while (i < n) { retag_at(i, at); if (i == 2) { G = 1.5; } else { G = G + 1; } i = i + 1; } return to_string(G) + " " + type_of(G); };
)JAI";
			// same_unit: the host redeclares through the program's own unit (a reload of its file writes in place, the retag this row
			// is about); otherwise through a literal, another unit, which shadows and leaves the loop on its own cell
			auto run_with = [&](bool vm, const std::string& script, bool proofs, bool same_unit) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (!proofs) { e->type_proofs(false); }
				e->disable(jai::filesystem);
				engine* raw = e.get();
				const std::string path = "D:/virtual/pilot/retag_unit.jai";
				e->add_function("host_retag", [raw, same_unit, path]() { if (same_unit) { raw->execute_file_source(path, "int G = 5;"); } else { raw->execute("int G = 5;"); } });
				try { return (same_unit ? e->execute_file_source(path, script) : e->execute(script)).as_string(); }
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			struct probe { const char* call; };
			const probe probes[] = { { "probe(4, 2);" }, { "probe(4, 0);" }, { "probe(4, 9);" }, { "probe(6, 3);" } };
			for (const probe& p : probes) {
				for (bool same_unit : { true, false }) {
					for (bool proofs : { true, false }) {
						const std::string script = std::string(defs) + " " + p.call;
						const std::string interp = run_with(false, script, proofs, same_unit), vm = run_with(true, script, proofs, same_unit);
						check_eq(interp, vm, std::string(p.call) + (same_unit ? " same unit" : " other unit") + (proofs ? "" : " proofs off") + ": both backends");
					}
				}
			}
			check(run_with(true, std::string(defs) + " probe(4, 2);", true, true).find("int") != std::string::npos, "the redeclared int boundary holds after the call when the same unit redeclares: " + run_with(true, std::string(defs) + " probe(4, 2);", true, true));
			check(run_with(true, std::string(defs) + " probe(4, 2);", true, false).rfind("2.5", 0) == 0, "another unit's redeclaration shadows and the loop keeps its own cell: " + run_with(true, std::string(defs) + " probe(4, 2);", true, false));
		});
		test("a_pinned_global_keeps_its_kind_or_leaves_the_block", [this]() {
			// a comparison the checker classed int_int or float_float pins its environment values to that kind (checked at the entry and
			// at every landing) and a truth test on a cell runs inline; every way a typed global can change while the block runs is a
			// call, so the landing after it rereads a same-kind value and misses on another kind or a binding it cannot hold: a host
			// define of the same kind or another, a host reference, a script callback, a reload of the unit with the same type or
			// another, a write to the float, and a parallel region (a write there is an error)
			const char* defs = R"JAI(
int G = 3;
float F = 0.5;
array<int> A = [5, 1, 3, 3, 9, 2, 8, 4];
function hit(int i, int at, int how) -> void { if (i == at) { host_change(how); } }
function probe(int n, int at, int how) -> string {
	int acc = 0;
	bool odd = false;
	for (int i = 0; i < n; ++i) {
		hit(i, at, how);
		if (A[i] == G) { acc = acc + 100; }
		if (i < G) { acc = acc + 1; }
		if (F > 0.25) { acc = acc + 10; }
		odd = !odd;
		if (odd) { acc = acc + 1000; }
	}
	return to_string(acc) + " " + type_of(G) + " " + to_string(G) + " " + type_of(F);
}
)JAI";
			struct outcome { std::string value; unsigned long long misses = 0; };
			auto run_with = [&](bool vm, const std::string& call, bool proofs) -> outcome {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (!proofs) { e->type_proofs(false); }
				e->disable(jai::filesystem);
				engine* raw = e.get();
				script_int host_g = 11;
				const std::string path = "D:/virtual/pilot/pinned_unit.jai";
				e->add_function("host_change", [raw, &host_g, path](int how) {
					switch (how) {
					case 1: raw->add_global("G", script_value(script_int{7}, raw)); break;
					case 2: raw->add_global("G", script_value(script_float{2.5}, raw)); break;
					case 3: raw->add_global_ref("G", host_g); break;
					case 4: raw->execute("G = G + 2;"); break;
					case 5: raw->execute_file_source(path, "int G = 1;"); break;
					case 6: raw->execute_file_source(path, "float G = 4.5;"); break;
					case 7: raw->add_global("F", script_value(script_float{0.125}, raw)); break;
					case 8: raw->execute("array<int> xs = [6]; try { parallel_for (auto& x : xs) { G = x; } } catch (err) { }"); break;
					default: break;
					}
				});
				outcome result;
				try { result.value = e->execute_file_source(path, std::string(defs) + " " + call).as_string(); }
				catch (const std::exception& ex) { result.value = std::string("throw:") + ex.what(); }
				if (vm) { result.misses = detail::engine_access::cells_counters(*e).misses; }
				return result;
			};
			struct probe { int how; const char* expected; bool misses; };
			const probe probes[] = {
				{ 0, "4283 int 3 float", false }, { 1, "", false }, { 2, "", true }, { 3, "", true }, { 4, "", false },
				{ 5, "", false }, { 6, "", true }, { 7, "", false }, { 8, "4283 int 3 float", false },
			};
			std::string seen, observed;
			for (const probe& p : probes) {
				const std::string call = "probe(8, 2, " + std::to_string(p.how) + ");";
				for (bool proofs : { true, false }) {
					const outcome interp = run_with(false, call, proofs), vm = run_with(true, call, proofs);
					check_eq(interp.value, vm.value, call + (proofs ? "" : " proofs off") + ": both backends");
					if (p.expected[0] != 0) { check_eq(std::string(p.expected), vm.value, call + (proofs ? "" : " proofs off") + ": the value"); }
					if (proofs) { seen += std::to_string(p.how) + (vm.misses != 0 ? "m " : "r ") ; observed += call + " " + vm.value + " misses " + std::to_string(vm.misses) + "; "; }
				}
			}
			std::string expected_seen; for (const probe& p : probes) { expected_seen += std::to_string(p.how) + (p.misses ? "m " : "r "); }
			check_eq(expected_seen, seen, "each change rereads or misses at the landing: " + observed);
			std::string listing;
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); stdlib::register_all(*e);
				e->add_function("host_change", [](int) {});
				e->disassembly_sink([&listing](const std::string& l, const std::string&) { listing += l; listing += "\n"; });
				try { e->execute(std::string(defs) + " probe(8, 2, 0);"); } catch (const std::exception& ex) { listing += std::string("throw:") + ex.what(); }
			}
			check(listing.find("-- cell loop ") != std::string::npos, "the loop closes: " + listing.substr(0, 4000));
			check(listing.find("C_JCMP_ANY") == std::string::npos, "the classed compares on globals and the truth test are pinned: " + listing.substr(0, 6000));
			check(listing.find("C_JTRUTH") != std::string::npos, "the truth test runs inline: " + listing.substr(0, 6000));
		});
		test("a_lambda_enters_its_block_over_a_program_global", [this]() {
			// a lambda names a program global through a plain cell: the block reads the cell's value, writes it back through the cell at
			// its flush and rereads it after a call, as a named function's block does with the global itself; both backends, proofs off
			const char* defs = R"JAI(
int G = 3;
float F = 0.5;
var bump = function(int i, int at) -> void { if (i == at) { G = G + 10; F = F * 2.0; } };
var reads = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { if (i < G) { acc = acc + 1; } } return acc; };
var writes = function(int n) { for (int i = 0; i < n; ++i) { G = G + i; F = F + 0.25; } return to_string(G) + " " + to_string(F); };
var called = function(int n, int at) { int acc = 0; for (int i = 0; i < n; ++i) { bump(i, at); acc = acc + G; } return to_string(acc) + " " + to_string(F); };
var retyped = function(int n, int at) { int acc = 0; for (int i = 0; i < n; ++i) { if (i == at) { host_retype(); } acc = acc + 1; if (G > 2) { acc = acc + 100; } } return to_string(acc) + " " + type_of(G); };
function nreads(int n) -> int { int acc = 0; for (int i = 0; i < n; ++i) { if (i < G) { acc = acc + 1; } } return acc; }
function nwrites(int n) -> string { for (int i = 0; i < n; ++i) { G = G + i; F = F + 0.25; } return to_string(G) + " " + to_string(F); }
)JAI";
			auto run_with = [&](bool vm, const std::string& call, bool proofs, unsigned long long* entries) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (!proofs) { e->type_proofs(false); }
				engine* raw = e.get();
				e->add_function("host_retype", [raw]() { raw->add_global("G", script_value(script_float{7.5}, raw)); });
				std::string out;
				try { out = e->execute(std::string(defs) + " " + call).as_string(); }
				catch (const std::exception& ex) { out = std::string("throw:") + ex.what(); }
				if (entries) { *entries = detail::engine_access::cells_counters(*e).entries; }
				return out;
			};
			struct probe { const char* call; const char* expected; const char* twin; };
			const probe probes[] = {
				{ "to_string(reads(8));", "3", "to_string(nreads(8));" }, { "writes(5);", "13 1.75", "nwrites(5);" }, { "called(4, 1);", "", nullptr },
				{ "called(4, 9);", "", nullptr }, { "retyped(6, 2);", "", nullptr },
			};
			for (const probe& p : probes) {
				for (bool proofs : { true, false }) {
					unsigned long long entries = 0;
					const std::string interp = run_with(false, p.call, proofs, nullptr), vm = run_with(true, p.call, proofs, &entries);
					check_eq(interp, vm, std::string(p.call) + (proofs ? "" : " proofs off") + ": both backends");
					if (p.expected[0] != 0) { check_eq(std::string(p.expected), vm, std::string(p.call) + ": the value"); }
					if (proofs) { check(entries > 0, std::string(p.call) + ": the lambda's loop enters its block"); }
					if (p.twin) {
						unsigned long long twin_entries = 0;
						check_eq(vm, run_with(true, p.twin, proofs, &twin_entries), std::string(p.twin) + (proofs ? "" : " proofs off") + ": the named twin's value");
						check_eq(twin_entries, entries, std::string(p.call) + (proofs ? "" : " proofs off") + ": the lambda enters as often as its named twin");
					}
				}
			}
		});
		test("a_graft_keeps_the_calls_its_body_makes", [this]() {
			// a callee within the graft budget grafts even when its body calls: the call stays a call made from the grafted body (a door call
			// in a block), its frame one level below the grafted one; values, errors with their traces, the call-depth cap and the block's
			// entries agree with the interpreter and with the middle end off, the grafts a trace counts past its cap included (a rethrow at
			// every level of a deep stack, a coroutine resumed over other frames)
			const char* defs = R"JAI(
array<int> T = [1, 2, 3, 4, 5, 6, 7, 8];
int LOG = 0;
int far(int i) { if (i > 100) { throw "far " + to_string(i); } return i * 2; }
void note(int v) { LOG = LOG + v; }
int near(int i) { if (i < 0 || i >= 8) { return far(i); } note(i); return T[i]; }
int outer(int i) { return near(i) + 1; }
int run(int n, int bad) { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + outer(i == bad ? 200 : i % 10); } return acc; }
int straight(int i) { return outer(i) * 3; }
int rec(int d) { if (d <= 0) { throw "bottom"; } return hop(d) + 1; }
int hop(int d) { if (d < 0) { return 0; } return rec(d - 1); }
int deep(int d) { if (d <= 0) { return 0; } return step(d) + 1; }
int step(int d) { if (d < 0) { return 0; } return deep(d - 1); }
string trace(int n, int bad) { string out = ""; try { out = to_string(run(n, bad)); } catch (e) { out = to_string(e) + " @"; for (auto& fr : e.stack) { out = out + " " + fr; } } return out + " log " + to_string(LOG); }
string traced_rec(int d) { string out = ""; try { out = to_string(rec(d)); } catch (e) { out = to_string(e) + " @"; for (auto& fr : e.stack) { out = out + " " + fr; } } return out; }
int rr(int d) { if (d <= 0) { throw "bottom"; } try { return rh(d) + 1; } catch (e) { if (d == 30) { throw to_string(e.stack.size()) + " " + e.stack[64]; } throw e; } }
int rh(int d) { if (d < 0) { return 0; } return rr(d - 1); }
string traced_rethrow(int d) { string out = ""; try { out = to_string(rr(d)); } catch (e) { out = to_string(e); } return out; }
int down(int d) { if (d <= 0) { throw "down"; } return down(d - 1) + 1; }
array<string> SEEN = [];
coroutine tracer() -> int { try { for (int i = 0; i < 2; i++) { try { down(70); } catch (e) { SEEN.push(e.stack[64]); } yield i; } } catch (e) { } }
var H = null;
int rz(int d) { if (d <= 0) { H.next(); return 0; } return rw(d) + 1; }
int rw(int d) { if (d < 0) { return 0; } return rz(d - 1); }
string fiber_traces() { H = tracer(); rz(5); rz(0); return to_json(SEEN); }
)JAI";
			enum class mode { interp, vm, vm_off };
			auto run_with = [&](mode m, const std::string& call, unsigned long long* entries, size_t depth_cap = 0) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(m == mode::interp ? backend_type::interpreter : backend_type::vm);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (m == mode::vm_off) { e->ir_passes(false); }
				if (depth_cap) { e->max_call_depth(backend_type::interpreter, depth_cap); e->max_call_depth(backend_type::vm, depth_cap); }
				std::string out;
				try { out = e->execute_file_source("D:/virtual/pilot/graft_calls.jai", std::string(defs) + " " + call).as_string(); }
				catch (const std::exception& ex) { out = std::string("throw:") + ex.what(); }
				if (entries) { *entries = detail::engine_access::cells_counters(*e).entries; }
				return out;
			};
			struct probe { const char* call; size_t cap; bool block; };
			const probe probes[] = {
				{ "to_string(run(40, -1));", 0, true }, { "trace(40, -1);", 0, false }, { "trace(20, 7);", 0, false }, { "to_string(straight(3)) + \" \" + to_string(straight(9));", 0, false },
				{ "straight(300);", 0, false }, { "traced_rec(3);", 0, false }, { "traced_rec(90);", 0, false }, { "traced_rethrow(100);", 0, false },
				{ "fiber_traces();", 0, false },
				{ "to_string(deep(15));", 40, false }, { "to_string(deep(19));", 40, false }, { "to_string(deep(20));", 40, false }, { "to_string(deep(60));", 40, false },
			};
			for (const probe& p : probes) {
				unsigned long long entries = 0;
				const std::string interp = run_with(mode::interp, p.call, nullptr, p.cap), vm = run_with(mode::vm, p.call, &entries, p.cap), off = run_with(mode::vm_off, p.call, nullptr, p.cap);
				check_eq(interp, vm, std::string(p.call) + ": both backends");
				check_eq(off, vm, std::string(p.call) + ": the middle end off");
				if (p.block) { check(entries > 0, std::string(p.call) + ": the loop enters its block"); }
			}
			check(run_with(mode::vm, "traced_rec(3);", nullptr).find("graft_calls.jai:11 ") != std::string::npos, "the grafted hop's frame names its line: " + run_with(mode::vm, "traced_rec(3);", nullptr));
			std::string listing;
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); stdlib::register_all(*e);
				e->disassembly_sink([&listing](const std::string& l, const std::string&) { listing += l; listing += "\n"; });
				try { e->execute(std::string(defs) + " run(4, -1); straight(3);"); } catch (const std::exception& ex) { listing += std::string("throw:") + ex.what(); }
			}
			check(listing.find("GRAFT_DEPTH") != std::string::npos && listing.find("C_GDEPTH") != std::string::npos, "near grafts with its calls kept, in the loop's block and outside it: " + listing.substr(0, 6000));
		});
		test("a_graft_that_calls_follows_reloads_budgets_coroutines_and_workers", [this]() {
			// the guard of a graft whose body calls takes the fallback when the callee is rebound, and the kept call resolves its own callee at
			// every call; a coroutine never grafts; an execution budget expiring inside a kept call raises as the call would; a parallel region's
			// workers take the fallback and its admission is the general form's
			const char* defs = R"JAI(
int far(int i) { return i * 2; }
int near(int i) { if (i > 3) { return far(i); } return i + 1; }
int sum(int n) { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + near(i); } return acc; }
coroutine gen(int n) -> int { for (int i = 0; i < n; i++) { yield i; } }
int drain(int n) { int s = 0; for (auto v : gen(n)) { s = s + v; } return s; }
int first(int n) { auto h = gen(n); return h.next(); }
int via(int n) { int a = first(n); return drain(n) + a + near(n); }
int spin(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + (i % 7); } return s; }
int slow(int n) { if (n > 0) { return spin(n); } return 0; }
int burn(int n) { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + slow(4000000); } return acc; }
)JAI";
			auto run_with = [&](bool vm, const std::string& call, double budget = 0.0) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (budget > 0.0) { e->execution_budget(budget); }
				std::string out;
				try {
					e->execute(std::string(defs));
					out = e->execute("to_string(sum(8));").as_string();
					if (std::string(call) == "rebind far") { e->execute("int far(int i) { return i * 100; }"); out += " " + e->execute("to_string(sum(8));").as_string(); }
					else if (std::string(call) == "rebind near") { e->execute("int near(int i) { return 7; }"); out += " " + e->execute("to_string(sum(8));").as_string(); }
					else { out += " " + e->execute(call).as_string(); }
				} catch (const std::exception& ex) { out += std::string(" throw:") + ex.what(); }
				return out;
			};
			struct probe { const char* call; double budget; };
			const probe probes[] = {
				{ "rebind far", 0.0 }, { "rebind near", 0.0 }, { "to_string(via(5));", 0.0 },
				{ "to_string(burn(200));", 0.2 },
				{ "array<int> xs = [1, 2, 3, 4, 5, 6]; parallel_for (auto& x : xs) { x = near(x) + far(x); } to_json(xs);", 0.0 },
				{ "int G = 0; int bump(int v) { G = G + v; return G; } int w(int v) { if (v > 9) { return bump(v); } return v; } array<int> ys = [1, 20]; string r = \"\"; try { parallel_for (auto& y : ys) { y = w(y); } r = to_json(ys); } catch (err) { r = \"refused \" + err; } r;", 0.0 },
			};
			for (const probe& p : probes) {
				const std::string interp = run_with(false, p.call, p.budget), vm = run_with(true, p.call, p.budget);
				if (p.budget > 0.0) {
					check(interp.find("budget") != std::string::npos && vm.find("budget") != std::string::npos, std::string(p.call) + ": the budget raises on both backends: " + interp + " | " + vm);
					continue;
				}
				check_eq(interp, vm, std::string(p.call) + ": both backends");
			}
			std::string listing;
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); stdlib::register_all(*e);
				e->disassembly_sink([&listing](const std::string& l, const std::string&) { listing += l; listing += "\n"; });
				try { e->execute(std::string(defs) + " via(3);"); } catch (const std::exception& ex) { listing += std::string("throw:") + ex.what(); }
			}
			check(listing.find("call kept gen ") != std::string::npos, "the coroutine stays a call: " + listing.substr(0, 6000));
		});
		test("call_arguments_and_the_callee_keep_their_source_order_across_a_getter", [this]() {
			// Regression, with a callee-getter variant: `take(V, G + 1)` where V is a global reference to a native
			// property whose getter steps G, the same through a local reference slot, and `F(G + 1)` where F is a reference to a
			// property whose getter returns the callee and steps G. Argument order is the source order on both backends: a computed
			// argument after a verbatim load keeps the loop general (an effect refusal); the callee's resolution runs first, at its
			// own place, and a getter observed there re-lands the block, so the loop with the getter-backed callee still closes.
			// The fixture arms the getter after the references are bound, so no read during binding moves G; the getter sets G = 20 (idempotent, so how often it runs does not enter: a
			// property argument converted by value can run its getter more than once on a first call, both backends the same, an
			// open finding) and each iteration resets G = 10, so the order shows: 21 per call when the getter precedes G + 1, 11
			// otherwise; before(3) computes a = 11 ahead of the getter (take_ab: 1100 per call, 2100 if the getter came first)
			struct stepper { bool armed = false; };
			const char* defs = R"JAI(
var G = 10;
function target(int a) -> int { return a; }
var box = Stepper();
var& V = box.number;
var& F = box.callable;
function take_b(int a, int b) -> int { return b; }
function take_ab(int a, int b) -> int { return a * 100 + b; }
function after(int n) { box.arm(); int acc = 0; for (int i = 0; i < n; ++i) { G = 10; int z = take_b(V, G + 1); acc = acc + z; } return to_string(acc); }
function before(int n) { box.arm(); int acc = 0; for (int i = 0; i < n; ++i) { G = 10; int z = take_ab(G + 1, V); acc = acc + z; } return to_string(acc); }
function via_local(int n) { var& p = box.number; box.arm(); int acc = 0; for (int i = 0; i < n; ++i) { G = 10; int z = take_b(p, G + 1); acc = acc + z; } return to_string(acc); }
function via_callee(int n) { box.arm(); int acc = 0; for (int i = 0; i < n; ++i) { G = 10; int z = F(G + 1); acc = acc + z; } return to_string(acc); }
)JAI";
			// the listing compiles the same four loops as lambdas held in vars: a named function body compiles on its first call
			const char* listing_defs = R"JAI(
var G = 10;
function target(int a) -> int { return a; }
var box = Stepper();
var& V = box.number;
var& F = box.callable;
function take_b(int a, int b) -> int { return b; }
function take_ab(int a, int b) -> int { return a * 100 + b; }
var after = function(int n) { box.arm(); int acc = 0; for (int i = 0; i < n; ++i) { G = 10; int z = take_b(V, G + 1); acc = acc + z; } return to_string(acc); };
var before = function(int n) { box.arm(); int acc = 0; for (int i = 0; i < n; ++i) { G = 10; int z = take_ab(G + 1, V); acc = acc + z; } return to_string(acc); };
var via_local = function(int n) { var& p = box.number; box.arm(); int acc = 0; for (int i = 0; i < n; ++i) { G = 10; int z = take_b(p, G + 1); acc = acc + z; } return to_string(acc); };
var via_callee = function(int n) { box.arm(); int acc = 0; for (int i = 0; i < n; ++i) { G = 10; int z = F(G + 1); acc = acc + z; } return to_string(acc); };
)JAI";
			auto run_with = [&](bool vm, const std::string& script, bool proofs) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (!proofs) { e->type_proofs(false); }
				engine* raw = e.get();
				dynamic_binder<stepper>(*e, "Stepper").constructor<>()
					.method("arm", [](stepper& s) { s.armed = true; })
					.property("number", [raw](stepper& s) -> int64_t { if (s.armed) { raw->execute("G = 20;"); } return 0; }, nullptr)
					.property("callable", [raw](stepper& s) -> script_value { if (s.armed) { raw->execute("G = 20;"); } return raw->get_variable("target"); }, nullptr)
					.build();
				try { return e->execute(script).as_string(); }
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			struct probe { const char* call; const char* expected; };
			const probe probes[] = { { "after(3);", "63" }, { "before(3);", "3300" }, { "via_local(3);", "63" }, { "via_callee(3);", "63" } };
			for (const probe& p : probes) {
				for (bool proofs : { true, false }) {
					const std::string script = std::string(defs) + " " + p.call;
					const std::string interp = run_with(false, script, proofs), vm = run_with(true, script, proofs);
					check_eq(interp, vm, std::string(p.call) + (proofs ? "" : " proofs off") + ": both backends");
					check_eq(std::string(p.expected), vm, std::string(p.call) + (proofs ? "" : " proofs off") + ": expected");
				}
			}
			const std::string listing = listing_of(listing_defs);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			size_t open = 0; for (size_t at = listing.find("-- open loop "); at != std::string::npos; at = listing.find("-- open loop ", at + 1)) { ++open; }
			check_eq(size_t(2), open, "the computed argument after a verbatim load (an environment name, a reference slot) keeps its loop general (nothing beside the statement would stay in a block): " + listing);
			check_eq(size_t(2), closed, "the computed argument before the load, and the getter-backed callee, close: " + listing);
			check(listing.find("effect at") != std::string::npos, "the refusal is an effect refusal: " + listing);
			check(listing.find(":probe") != std::string::npos, "the probe is an operation of the region: " + listing);
		});
		test("a_read_that_may_run_user_code_never_runs_ahead_of_an_earlier_operand", [this]() {
			// Regression and its variants: the outer container of a nested read, or the left operand of a fused
			// binary, is read before the inner read in the language's order; when that read may run user code (an operator[] on a
			// var container, a getter-backed reference as the index or as the receiver, a class operator or a custom numeric
			// operator inside the index) and the operand is a binding user code could rebind (a global, a local captured by
			// reference, a reference parameter to such a binding), the operand's value is taken ahead of the read on both
			// backends; a private local keeps the fused shape without a snapshot. The fixture is a C++ box bound as a global by reference
			// with a free arm() (a method call on the box would touch its properties), armed after the references are bound
			struct stepper { bool armed = false; };
			const char* defs = R"JAI(
var P = [10];
var X = 5;
var R = [10, 20];
var D = [1, 2, 3];
array<int> grid_source = [5, 6, 7];
var& PR = box.grid;
array<int> P2 = [10, 20];
array<int> Other = [30, 40, 50, 60];
array<int> Q2 = [0, 1, 2];
class Index { var target = null; Index(var f) { target = f; } int operator[](int i) { target(); return 0; } }
class Vec { int v = 0; Vec(int a) { v = a; } function operator+(int o) -> int { X = 100; return v + o; } }
var global_rebind = function() { var Q = Index([]() { P = [90]; }); return to_string(P[Q[0]]) + "|" + to_string(P[0]); };
var local_rebind = function() { var L = [10]; var Q = Index([&L]() { L = [90]; }); return to_string(L[Q[0]]) + "|" + to_string(L[0]); };
var lift_rebind = function() { var Q = Index([]() { X = 100; }); int r = X + Q[0]; return to_string(r) + "|" + to_string(X); };
var typed_getter = function() { array<int> Q = [0]; var& idx = box.number; arm(); return to_string(P[Q[idx]]) + "|" + to_string(P[0]); };
var private_operator = function() { Vec q = Vec(1); int r = X + R[q + 0]; return to_string(r) + "|" + to_string(X); };
var by_ref = function(var& p) { var& s = box.source; arm(); return to_string(p[s[0]]) + "|" + to_string(D[0]); };
var ref_param_rebind = function() { return by_ref(D); };
var typed_receiver = function() { array<int>& t = box.grid; arm(); int r = X + t[0]; return to_string(r) + "|" + to_string(X); };
var numeric_operator = function() { array<int> Q = [0, 0]; int i = 0; install_plus(); int r = P[Q[i + 0]]; int p0 = P[0]; return to_string(r * 1000 + p0); };
var outer_ref = function() { array<int> Q = [0]; arm(); string m = ""; try { int r = PR[Q[2]]; m = to_string(r); } catch (e) { m = "caught"; } return m + "|" + to_string(X); };
var guard_reuse = function(int n) { int s = 0; int i = 0; string m = ""; try { while (i < n) { s = s + P2[Q2[i + 0]]; s = s + Other[Q2[i + 0]]; i = i + 1; } } catch (e) { m = "caught"; } return m + "|" + to_string(s); };
var private_stays = function(int n) { array<int> A = [1, 2, 3, 4]; int p = 100; int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + (p + A[i]); } return acc; };
)JAI";
			auto run_with = [&](bool vm, const std::string& script, bool proofs) -> std::string {
				stepper source;   // outlives the engine below
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (!proofs) { e->type_proofs(false); }
				engine* raw = e.get();
				dynamic_binder<stepper>(*e, "Stepper")
					.property("number", [raw](stepper& s) -> int64_t { if (s.armed) { raw->execute("P = [90];"); } return 0; }, nullptr)
					.property("source", [raw](stepper& s) -> script_value { if (s.armed) { raw->execute("D = [7, 8, 9];"); } return raw->execute("[0]"); }, nullptr)
					.property("grid", [raw](stepper& s) -> script_value { if (s.armed) { raw->execute("X = 100;"); } return raw->get_variable("grid_source"); }, nullptr)
					.build();
				e->add_global_ref("box", source);
				e->add_function("arm", [&source]() { source.armed = true; });
				e->add_function("install_plus", [raw]() {
					raw->add_function("+", [raw](script_int a, script_int b) { raw->execute("P = [90];"); return a + b; });
					detail::engine_access::set_has_custom_numeric_operators(*raw, true);
				});
				try { return e->execute(script).as_string(); }
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			struct probe { const char* call; const char* expected; };
			const probe probes[] = {
				{ "global_rebind();", "10|90" }, { "local_rebind();", "10|90" }, { "lift_rebind();", "5|100" }, { "typed_getter();", "10|90" },
				{ "private_operator();", "25|100" }, { "ref_param_rebind();", "1|7" }, { "typed_receiver();", "10|100" }, { "numeric_operator();", "10090" }, { "outer_ref();", "caught|100" }, { "guard_reuse(3);", "caught|100" },
				{ "to_string(private_stays(4));", "410" },
			};
			for (const probe& p : probes) {
				for (bool proofs : { true, false }) {
					const std::string script = std::string(defs) + " " + p.call;
					const std::string interp = run_with(false, script, proofs), vm = run_with(true, script, proofs);
					check_eq(interp, vm, std::string(p.call) + (proofs ? "" : " proofs off") + ": both backends");
					check_eq(std::string(p.expected), vm, std::string(p.call) + (proofs ? "" : " proofs off") + ": expected");
				}
			}
			const std::string priv = listing_of("var f = function(int n) { array<int> A = [1, 2, 3, 4]; int p = 100; int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + (p + A[i]); } return acc; };");
			check(priv.find("OPERAND_TEMP") == std::string::npos && priv.find("-- cell loop") != std::string::npos, "a private local ahead of a proven read keeps the fused shape and the loop closes: " + priv);
			const std::string reuse = listing_of("array<int> P2 = [10, 20]; array<int> Other = [30, 40, 50, 60]; array<int> Q2 = [0, 1, 2]; var f = function(int n) { int s = 0; int i = 0; while (i < n) { s = s + P2[Q2[i + 0]]; s = s + Other[Q2[i + 0]]; i = i + 1; } return s; };");
			check(reuse.find("-- cell loop") != std::string::npos, "two guarded global containers in one body still close as a block (the miss path restores each snapshot): " + reuse);
			const std::string snap = listing_of("var f = function(var& t, var& pal, int n) { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + pal[t[i + 1] + 1]; } return acc; };");
			check(snap.find("-- cell loop") != std::string::npos, "a guarded container ahead of an index that may run user code still closes as a block: " + snap);
		});
		test("a_global_redeclared_as_a_property_reference_after_compilation_reads_first", [this]() {
			// Reload case: a function compiled while P and X2 were values of this unit; the host then redeclares them as
			// references to properties (var& P = box.grid; var& X2 = box.number); the compiled reads must still read P (its getter,
			// or its error) before the inner read that raises, and a getter-backed binding inside a loop runs its getter on every
			// read (the block admits neither the site nor the cell). The fixture is a C++ box bound as a global by reference with
			// a free arm(); the getters count in H
			struct stepper { bool armed = false; };
			const char* defs = R"JAI(
var X = 5;
var H = 0;
var P = [10];
var X2 = 7;
array<int> grid_source = [5, 6, 7];
function probe() { array<int> Q = [0]; string m = ""; try { int r = P[Q[2]]; m = to_string(r); } catch (err) { m = "caught"; } return m + "|" + to_string(X); }
function probe2() { array<int> Q = [0]; string m = ""; try { int r = X2 + Q[2]; m = to_string(r); } catch (err) { m = "caught"; } return m + "|" + to_string(X); }
function loop_container(int n) { array<int> Q = [0, 0, 0, 0]; int s = 0; for (int i = 0; i < n; ++i) { s = s + P[Q[i]]; } return to_string(s) + "|" + to_string(H); }
function loop_scalar(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + X2; } return to_string(s) + "|" + to_string(H); }
)JAI";
			auto run_with = [&](bool vm, const char* first, const char* second, bool proofs) -> std::string {
				stepper source;   // outlives the engine below
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (!proofs) { e->type_proofs(false); }
				engine* raw = e.get();
				dynamic_binder<stepper>(*e, "Stepper")
					.property("number", [raw](stepper& s) -> int64_t { if (s.armed) { raw->execute("X = 100; H = H + 1;"); } return 0; }, nullptr)
					.property("grid", [raw](stepper& s) -> script_value { if (s.armed) { raw->execute("X = 100; H = H + 1;"); } return raw->get_variable("grid_source"); }, nullptr)
					.build();
				e->add_global_ref("box", source);
				e->add_function("arm", [&source]() { source.armed = true; });
				try {
					e->disable(jai::filesystem);
					const std::string path = "D:/virtual/pilot/reload_unit.jai";
					const std::string before = e->execute_file_source(path, std::string(defs) + " " + first).as_string();   // compiled and run while the names are values
					e->execute_file_source(path, "var& P = box.grid; var& X2 = box.number;");   // the same unit (a reload of the file) redeclares them in place as references to properties
					return before + " / " + e->execute(std::string("arm(); ") + second).as_string();
				}
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			struct probe { const char* first; const char* second; const char* expected; };
			const probe probes[] = {
				{ "probe();", "probe();", "caught|5 / caught|100" },
				{ "probe2();", "probe2();", "caught|5 / caught|100" },
				{ "loop_container(4);", "loop_container(4);", "40|0 / 20|4" },
				{ "loop_scalar(4);", "loop_scalar(4);", "28|0 / 0|4" },
			};
			for (const probe& p : probes) {
				for (bool proofs : { true, false }) {
					const std::string interp = run_with(false, p.first, p.second, proofs), vm = run_with(true, p.first, p.second, proofs);
					check_eq(interp, vm, std::string(p.second) + (proofs ? "" : " proofs off") + ": both backends");
					check_eq(std::string(p.expected), vm, std::string(p.second) + (proofs ? "" : " proofs off") + ": expected");
				}
			}
			const std::string listing = listing_of("var P = [10]; var f = function(int n) { array<int> Q = [0, 0, 0, 0]; int s = 0; for (int i = 0; i < n; ++i) { s = s + P[Q[i]]; } return s; };");
			size_t guards = 0; for (size_t at = listing.find("C_GUARD"); at != std::string::npos; at = listing.find("C_GUARD", at + 1)) { ++guards; }
			check(listing.find("-- cell loop") != std::string::npos && listing.find("OPERAND_TEMP") != std::string::npos && guards == 2, "an environment container ahead of a quiet inner read keeps its value snapshot at its own point in the block too (its guard and the counted loop's placeholder): " + listing);
		});
		test("a_guard_temp_is_a_value_at_its_own_point", [this]() {
			// Two regressions: a getter's captured null is a value, never a sign that no snapshot was taken (the
			// getter runs once whether it returns null or a table), and a guard temp's slot belongs to later definitions once the guard's
			// read is past (a live multiply operand of the next statement is never erased: the overflow text and the float-to-int
			// cast both need it on the general form after a miss)
			struct nulls { bool armed = false; bool flips = false; int calls = 0; };
			auto run_null = [&](bool vm, bool flips, bool proofs) -> std::string {
				nulls source; source.flips = flips;
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (!proofs) { e->type_proofs(false); }
				engine* raw = e.get();
				dynamic_binder<nulls>(*e, "Nulls")
					.property("values", [raw](nulls& s) -> script_value { if (s.armed) { ++s.calls; if (!s.flips || s.calls == 1) { return script_value(std::monostate{}, raw); } } return raw->get_variable("VALUES"); }, nullptr)
					.build();
				e->add_global_ref("box", source);
				try {
					e->disable(jai::filesystem);
					const std::string path = "D:/virtual/pilot/null_unit.jai";
					e->execute_file_source(path, "var VALUES = [10]; var P = [10]; function probe() { array<int> Q = [0]; return P[Q[0]]; } probe();");
					e->execute_file_source(path, "var& P = box.values;");   // the same unit (a reload) redeclares P in place
					source.armed = true; source.calls = 0;
					const std::string r = e->execute("var result = \"\"; try { probe(); result = \"value\"; } catch (err) { result = \"caught\"; } result;").as_string();
					return r + "|" + std::to_string(source.calls);
				}
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (bool flips : { false, true }) {
				for (bool proofs : { true, false }) {
					const std::string interp = run_null(false, flips, proofs), vm = run_null(true, flips, proofs);
					const std::string label = std::string(flips ? "null once then a table" : "null") + (proofs ? "" : " proofs off");
					check_eq(interp, vm, label + ": both backends");
					check_eq(std::string("caught|1"), vm, label + ": the getter runs once and its null is the container");
				}
			}
			const char* liveness = R"JAI(
array<int> P = [1, 2, 3, 4];
var overflow = function(int n, int limit) { array<int> Q = [0, 1, 2, 3]; int total = 0; int value = 0; int i = 0; string m = ""; try { while (i < n) { total = total + P[Q[i]]; value = (i + 1) * (limit - i); i = i + 1; } } catch (err) { m = err; } return m + "|" + to_string(total) + "|" + to_string(value); };
var conversion = function(int n) { array<int> Q = [0, 1, 2, 3]; int total = 0; int value = 0; int i = 0; while (i < n) { total = total + P[Q[i]]; value = cast<int>((i + 0.5) * 2.0); i = i + 1; } return to_string(total) + "|" + to_string(value); };
)JAI";
			auto run_with = [&](bool vm, const std::string& script, bool proofs) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (!proofs) { e->type_proofs(false); }
				try { return e->execute(script).as_string(); }
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			struct probe { const char* call; const char* expected; };
			const probe probes[] = { { "overflow(3, 4611686018427387903);", "Integer overflow in '*'|6|9223372036854775804" }, { "conversion(3);", "6|5" } };
			for (const probe& p : probes) {
				for (bool proofs : { true, false }) {
					const std::string script = std::string(liveness) + " " + p.call;
					const std::string interp = run_with(false, script, proofs), vm = run_with(true, script, proofs);
					check_eq(interp, vm, std::string(p.call) + (proofs ? "" : " proofs off") + ": both backends");
					check_eq(std::string(p.expected), vm, std::string(p.call) + (proofs ? "" : " proofs off") + ": expected");
				}
			}
		});
		test("strings_append_in_a_block", [this]() {
			// vid_row_half's shape (a var string built from string elements of var arrays and an environment string), an alias taken
			// before the loop (the first append misses and the general form unshares), the interned literal, a char appended as text,
			// an int appended (the mismatch text), a callee keeping a copy (the append after it misses), a typed array<string>, a
			// declared string local, a var that stops being a string mid-loop; all the same on both backends, proofs on and off
			const char* defs = R"JAI(
var PAL_FG = ["a", "b"]; var PAL_BG = ["c", "d"]; var HALFG = "h"; array<int> PIX = [0, 1, 1, 0, 1, 0, 0, 1]; array<string> WORDS = ["x", "y", "z"]; var KEEP = "";
var row_half = function(int cy, int vw) { var row = ""; int fg = -1; int bg = -1; int ti = cy * 2 * vw; int bi = ti + vw; for (int x = 0; x < vw; ++x) { int top = PIX[ti + x]; int bot = PIX[bi + x]; if (top != fg) { row += PAL_FG[top]; fg = top; } if (bot != bg) { row += PAL_BG[bot]; bg = bot; } row += HALFG; } return row; };
var aliased = function(int n) { var s = ""; var s2 = s; for (int i = 0; i < n; ++i) { s += HALFG; } return s + "|" + s2; };
var chars = function(int n) { var s = ""; for (int i = 0; i < n; ++i) { s += 'x'; s += HALFG; } return s; };
var ints = function(int n) { var s = ""; try { for (int i = 0; i < n; ++i) { s += HALFG; s += i; } } catch (e) { return s + "|" + e; } return s; };
var keep = function(string v) -> void { KEEP = v; };
var kept = function(int n) { var s = ""; for (int i = 0; i < n; ++i) { s += HALFG; keep(s); } return s + "|" + KEEP; };
var typed = function(int n) { var s = ""; for (int i = 0; i < n; ++i) { s += WORDS[i % 3]; } return s; };
var declared = function(int n) { string s = "s:"; for (int i = 0; i < n; ++i) { s += WORDS[i % 3]; s += "-"; } return s; };
var flips = function(int n) { var s = ""; try { for (int i = 0; i < n; ++i) { s += HALFG; if (i == 1) { s = 5; } } } catch (e) { return "caught " + e; } return to_string(s); };
var selfs = function(int n) { var s = "ab"; for (int i = 0; i < n; ++i) { s += s; } return s; };
var aliased_src = function(int n) { var s = "a"; var t = s; for (int i = 0; i < n; ++i) { s += t; } return s + "|" + t; };
var summed = function(var& data, int n) { int acc = 0; try { for (int i = 0; i < n; ++i) { acc = acc + data[i % 8]; } } catch (e) { return "throw|" + to_string(acc); } return to_string(acc); };
var MIXED = [3, 1, 4, 1, 5, "s", 2, 6];
)JAI";
			struct probe { const char* call; const char* expected; };
			const probe probes[] = {
				{ "row_half(0, 4);", "adhbchhadh" }, { "aliased(3);", "hhh|" }, { "chars(2);", "xhxh" }, { "ints(2);", "" }, { "kept(3);", "hhh|hhh" },
				{ "typed(5);", "xyzxy" }, { "declared(3);", "s:x-y-z-" }, { "flips(3);", "" }, { "selfs(3);", "abababababababab" }, { "aliased_src(3);", "aaaa|a" }, { "summed(MIXED, 8);", "throw|14" },
			};
			for (const probe& p : probes) {
				both(std::string(defs) + " " + p.call, p.expected, p.call);
				both(std::string(defs) + " " + p.call, "", (std::string(p.call) + " proofs off").c_str(), false);
			}
			// the memory cap reached inside the loop: the general form's text and post-state on both backends
			auto capped = [&](bool vm) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				e->memory_cap(65536);
				try { return e->execute(std::string(defs) + " var big = function(int n) { var s = \"\"; try { for (int i = 0; i < n; ++i) { s += WORDS[i % 3]; s += HALFG; } } catch (e) { return \"caught|\" + e; } return \"no cap\"; }; big(100000);").as_string(); }
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			const std::string ci = capped(false), cv = capped(true);
			check_eq(ci, cv, "the memory cap inside the loop: both backends");
			check(ci.find("|") != std::string::npos, "the cap raised inside the loop and was caught: " + ci);
			const std::string listing = listing_of(defs);
			size_t appends = 0; for (size_t at = listing.find("\tC_APPEND "); at != std::string::npos; at = listing.find("\tC_APPEND ", at + 1)) { ++appends; }
			check(appends >= 10, "the appends are cell operations (" + std::to_string(appends) + "): " + listing);
			size_t closed = 0; for (size_t at = listing.find("-- cell loop "); at != std::string::npos; at = listing.find("-- cell loop ", at + 1)) { ++closed; }
			check_eq(size_t(10), closed, "ten loops close, the char append's stays general (a boundary would leave its loop only the counter; open loops: " + listing.substr(listing.find("-- open loop ") == std::string::npos ? listing.size() : listing.find("-- open loop "), 800) + ")");
			check(listing.find(":append") != std::string::npos, "the IR names the append: " + listing);
		});
		test("a_string_environment_cell_is_re_borrowed_after_every_effect_point", [this]() {
			// The string cells' nodes are re-read in cells_reland, so a callee
			// that rebinds the environment string (flip(i): TEXT = "b" at i == 2, a call taking only an int, so the loop stays admitted)
			// and a getter observed by the callee probe that rebinds it (var& F = box.callable, armed from i == 2) both show in the
			// next append; the fixture is a C++ box bound as a global by reference with a free arm()
			struct stepper { bool armed = false; };
			const char* defs = R"JAI(
var TEXT = "a";
function target(int a) -> int { return a; }
var& F = box.callable;
function flip(int i) -> void { if (i == 2) { TEXT = "b"; } }
function by_call() { var s = ""; int i = 0; while (i < 5) { flip(i); s += TEXT; i = i + 1; } return s + "|" + TEXT; }
function by_getter() { var s = ""; int i = 0; while (i < 5) { if (i == 2) { arm(); } int z = F(i); s += TEXT; i = i + 1; } return s + "|" + TEXT; }
)JAI";
			auto run_with = [&](bool vm, const std::string& script, bool proofs) -> std::string {
				stepper source;   // outlives the engine below
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (!proofs) { e->type_proofs(false); }
				engine* raw = e.get();
				dynamic_binder<stepper>(*e, "Stepper")
					.property("callable", [raw](stepper& s) -> script_value { if (s.armed) { raw->execute("TEXT = \"b\";"); } return raw->get_variable("target"); }, nullptr)
					.build();
				e->add_global_ref("box", source);
				e->add_function("arm", [&source]() { source.armed = true; });
				try { return e->execute(script).as_string(); }
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			struct probe { const char* call; const char* expected; };
			const probe probes[] = { { "by_call();", "aabbb|b" }, { "by_getter();", "aabbb|b" } };
			for (const probe& p : probes) {
				for (bool proofs : { true, false }) {
					const std::string script = std::string(defs) + " " + p.call;
					const std::string interp = run_with(false, script, proofs), vm = run_with(true, script, proofs);
					check_eq(interp, vm, std::string(p.call) + (proofs ? "" : " proofs off") + ": both backends");
					check_eq(std::string(p.expected), vm, std::string(p.call) + (proofs ? "" : " proofs off") + ": expected");
				}
			}
			const std::string listing = listing_of("var TEXT = \"a\"; function flip(int i) -> void { if (i == 2) { TEXT = \"b\"; } } var f = function() { var s = \"\"; int i = 0; while (i < 5) { flip(i); s += TEXT; i = i + 1; } return s; };");
			check(listing.find("-- cell loop") != std::string::npos && listing.find("C_APPEND") != std::string::npos, "the loop with the int-only call closes and appends in place: " + listing);
		});
		test("a_nested_or_chain_and_a_ternary_declaration_close_inside_a_block", [this]() {
			// trace_block_dz's shape (JaiDOOM things.jai 324): `t < 0.0 || t > 1.0 || t >= best` is jumps with no bool materialized,
			// `float ob = f0 > f1 ? f0 : f1` lands each arm in the declaration (the block widens the int arm), and the loop closes
			const char* fn = "var f = function(var& VX, var& L1, var& L2, int n, float rdx, float rdy, float x1) -> float {"
			                 " float best = 1000.0; int hits = 0;"
			                 " for (int i = 0; i < n; ++i) {"
			                 "  float ax = VX[L1[i]]; float bx = VX[L2[i]];"
			                 "  float d = (bx - ax) * rdy - (bx + ax) * rdx;"
			                 "  if (d > -0.0001 && d < 0.0001) { continue; }"
			                 "  float t = (x1 - ax) / d;"
			                 "  if (t < 0.0 || t > 1.0 || t >= best) { continue; }"
			                 "  int f0 = L1[i]; int f1 = L2[i];"
			                 "  float ob = f0 > f1 ? f0 : f1;"
			                 "  float ot = f0 < f1 ? f0 : f1;"
			                 "  if (t * 10.0 <= ob || ot < 0.5) { best = t; hits = hits + 1; }"
			                 " }"
			                 " return best + hits; };";
			const std::string listing = listing_of(fn);
			check_true(listing.find("CELLS_") != std::string::npos, "the loop closes: " + listing);
			check_true(listing.find("TO_BOOL") == std::string::npos, "no bool is materialized for the condition chain: " + listing);
			const std::string call = std::string(fn) + " array<float> VX = [0.0, 1.0, 2.5, 4.0, 8.0, 0.5]; array<int> L1 = [0, 1, 2, 3, 4, 5, 1]; array<int> L2 = [1, 2, 3, 4, 5, 0, 1];"
			                         " to_string(f(VX, L1, L2, 7, 0.25, 0.75, 3.0));";
			for (bool proofs : { true, false }) {
				const std::string vm = run_on(true, call, proofs), in = run_on(false, call, proofs);
				check_eq(in, vm, std::string("both backends agree") + (proofs ? "" : " (proofs off)"));
				check_true(vm.find("throw") == std::string::npos, "the call completes: " + vm);
			}
		});
		test("a_member_of_a_frame_slot_instance_is_a_block_container", [this]() {
			// render_view_impl's shape (GLOOM render.jai 336): a chunk record's columns are read and stored through members of the
			// reference parameter; the entry resolves each field once (the member IC's plain-field probe), a user accessor on the
			// field keeps its site on the general form, and a callee that rebinds the field is seen at the landing
			const char* fn = "class Chunk { array<float> odist = []; array<int> okind = []; array<float> out = []; }"
			                 " var f = function(var& ch, int cols, float vh) -> float {"
			                 " float acc = 0.0;"
			                 " for (int ci = 0; ci < cols; ++ci) {"
			                 "  float dist = ch.odist[ci]; int kind = ch.okind[ci];"
			                 "  float lineh = vh / dist; if (kind == 2) { lineh = lineh * 0.5; }"
			                 "  ch.out[ci] = lineh; acc = acc + lineh;"
			                 " }"
			                 " return acc; };";
			const std::string listing = listing_of(fn);
			check_true(listing.find("CELLS_") != std::string::npos && listing.find("C_LOAD") != std::string::npos && listing.find("C_INDEX_STORE") != std::string::npos, "the member containers close the loop: " + listing);
			const std::string call = std::string(fn) + " var c = Chunk(); c.odist = [2.0, 4.0, 8.0, 0.5]; c.okind = [1, 2, 1, 2]; c.out = [0.0, 0.0, 0.0, 0.0];"
			                         " float r = f(c, 4, 16.0); to_string(r) + \" \" + to_string(c.out[1]) + \" \" + to_string(c.out[3]);";
			for (bool proofs : { true, false }) {
				const std::string vm = run_on(true, call, proofs), in = run_on(false, call, proofs);
				check_eq(in, vm, std::string("both backends agree") + (proofs ? "" : " (proofs off)"));
				check_true(vm.find("throw") == std::string::npos, "the call completes: " + vm);
			}
			const std::string acc = "class Chunk { array<float> odist = []; int reads = 0; function _get_odist() { reads = reads + 1; return odist; } }"
			                        " var g = function(var& ch, int cols) -> float { float acc = 0.0; for (int ci = 0; ci < cols; ++ci) { acc = acc + ch.odist[ci]; } return acc; };"
			                        " var c = Chunk(); c.odist = [1.0, 2.0, 3.0]; float r = g(c, 3); to_string(r) + \" \" + to_string(c.reads);";
			for (bool proofs : { true, false }) {
				check_eq(run_on(false, acc, proofs), run_on(true, acc, proofs), std::string("an accessor-backed member agrees (the general form runs the getter per read)") + (proofs ? "" : " (proofs off)"));
			}
			const std::string rebind = "class Chunk { array<float> odist = []; }"
			                           " function swap(var& ch) { ch.odist = [10.0, 20.0, 30.0, 40.0]; }"
			                           " var h = function(var& ch, int cols) -> float { float acc = 0.0; for (int ci = 0; ci < cols; ++ci) { acc = acc + ch.odist[ci]; if (ci == 1) { swap(ch); } } return acc; };"
			                           " var c = Chunk(); c.odist = [1.0, 2.0, 3.0, 4.0]; to_string(h(c, 4));";
			for (bool proofs : { true, false }) {
				check_eq(run_on(false, rebind, proofs), run_on(true, rebind, proofs), std::string("a field rebound by a callee agrees (the landing resolves it again)") + (proofs ? "" : " (proofs off)"));
			}
			// a member of a program global (`canvas.px[i] = ...` in a frame loop): the entry resolves the global through its lookup cache, then the field
			const char* gfn = "class Chunk { array<float> odist = []; array<float> out = []; } Chunk G;"
			                  " var gf = function(int cols, float vh) -> float {"
			                  " float acc = 0.0;"
			                  " for (int ci = 0; ci < cols; ++ci) { float lineh = vh / G.odist[ci]; G.out[ci] = lineh; acc = acc + lineh; }"
			                  " return acc; };";
			const std::string glisting = listing_of(gfn);
			check_true(glisting.find("CELLS_") != std::string::npos && glisting.find("C_LOAD") != std::string::npos && glisting.find("C_INDEX_STORE") != std::string::npos, "a global's member containers close the loop: " + glisting);
			const std::string gcall = std::string(gfn) + " G.odist = [2.0, 4.0, 8.0, 0.5]; G.out = [0.0, 0.0, 0.0, 0.0];"
			                          " float r = gf(4, 16.0); to_string(r) + \" \" + to_string(G.out[1]) + \" \" + to_string(G.out[3]);";
			const std::string gswap = "class Chunk { array<float> odist = []; array<float> out = []; } Chunk G;"
			                          " function swap_global() { Chunk fresh; fresh.odist = [10.0, 20.0, 30.0, 40.0]; fresh.out = [0.0, 0.0, 0.0, 0.0]; G = fresh; }"
			                          " function swap_field() { G.odist = [100.0, 200.0, 300.0, 400.0]; }"
			                          " var gh = function(int cols) -> float { float acc = 0.0;"
			                          " for (int ci = 0; ci < cols; ++ci) { acc = acc + G.odist[ci]; G.out[ci] = acc; if (ci == 0) { swap_global(); } if (ci == 2) { swap_field(); } } return acc; };"
			                          " G.odist = [1.0, 2.0, 3.0, 4.0]; G.out = [0.0, 0.0, 0.0, 0.0]; float r = gh(4); to_string(r) + \" \" + to_string(G.out);";
			for (bool proofs : { true, false }) {
				const std::string vm = run_on(true, gcall, proofs), in = run_on(false, gcall, proofs);
				check_eq(in, vm, std::string("a global's members: both backends agree") + (proofs ? "" : " (proofs off)"));
				check_true(vm.find("throw") == std::string::npos, "the call completes: " + vm);
				check_eq(run_on(false, gswap, proofs), run_on(true, gswap, proofs), std::string("a global or its field replaced by a callee agrees (the landing resolves it again)") + (proofs ? "" : " (proofs off)"));
			}
		});
		test("a_float_compound_store_takes_the_float_row", [this]() {
			// `x += y` on floats took the int row (a fallback at every execution; inside a loop a representation refusal): the
			// class the checker stamps picks the float row, the loop closes, and the mixed and unknown classes keep the general
			// compound with its conversions
			const char* fn = "var f = function(int n, float d, float e) -> float {"
			                 " float x = 1.5; float y = 100.0; float z = 1.0; float w = 81.0; int i = 0;"
			                 " while (i < n) { x += d; y -= e; z *= 2.0; w /= 3.0; i += 1; }"
			                 " return x + y + z + w + i; };";
			const std::string listing = listing_of(fn);
			check_true(listing.find("T_ADD_FF") != std::string::npos && listing.find("T_SUB_FF") != std::string::npos && listing.find("T_MUL_FF") != std::string::npos && listing.find("T_DIV_FF") != std::string::npos,
			           "the float compounds take the float rows: " + listing);
			check_true(listing.find("CELLS_") != std::string::npos, "the loop closes: " + listing);
			const std::string call = std::string(fn) + " to_string(f(4, 0.25, 2.5)) + \" \" + to_string(f(0, 0.25, 2.5));";
			for (bool proofs : { true, false }) {
				const std::string vm = run_on(true, call, proofs), in = run_on(false, call, proofs);
				check_eq(in, vm, std::string("both backends agree") + (proofs ? "" : " (proofs off)"));
				check_true(vm.find("throw") == std::string::npos, "the call completes: " + vm);
			}
			const std::string mixed = "var g = function(int n) -> string { float x = 1.5; var v = 2; int i = 3; for (int k = 0; k < n; ++k) { x += 1; v += 2.5; i += 2; x *= 2.0; x /= 4.0; }"
			                          " return to_string(x) + \" \" + to_string(v) + \" \" + to_string(i); }; g(3);";
			for (bool proofs : { true, false }) {
				check_eq(run_on(false, mixed, proofs), run_on(true, mixed, proofs), std::string("mixed and unknown classes agree") + (proofs ? "" : " (proofs off)"));
			}
		});
		test("a_computed_index_on_a_member_container_fuses_and_the_dda_loop_closes", [this]() {
			// gloom_ray_chunk_step's DDA while (GLOOM pure.jai 46): `t.pack[cell / 15]` reads a member container at a computed
			// index; fused, the loop closes over the member container with its float compounds
			const char* fn = "class Ws { int mw = 8; array<int> pack = []; }"
			                 " var f = function(var& t, float posx, float posy, float rdx, float rdy) -> float {"
			                 " int mw = t.mw; int mx = math::itrunc(posx); int my = math::itrunc(posy);"
			                 " float adx = rdx < 0.0 ? -rdx : rdx; float ady = rdy < 0.0 ? -rdy : rdy;"
			                 " float ddx = adx < 0.00000001 ? 100000000.0 : 1.0 / adx; float ddy = ady < 0.00000001 ? 100000000.0 : 1.0 / ady;"
			                 " int stepx = rdx < 0.0 ? -1 : 1; int stepy = rdy < 0.0 ? -1 : 1;"
			                 " float sdx = rdx < 0.0 ? (posx - mx) * ddx : (mx + 1.0 - posx) * ddx;"
			                 " float sdy = rdy < 0.0 ? (posy - my) * ddy : (my + 1.0 - posy) * ddy;"
			                 " int side = 0; int kind = 1; int guard = 0; bool hit = false;"
			                 " while (!hit && guard < 128) {"
			                 "  if (sdx < sdy) { sdx += ddx; mx += stepx; side = 0; } else { sdy += ddy; my += stepy; side = 1; }"
			                 "  int cell = my * mw + mx; int word = t.pack[cell / 15]; int c = (word >> ((cell % 15) * 4)) & 15;"
			                 "  if (c != 0) { hit = true; kind = c; }"
			                 "  guard = guard + 1;"
			                 " }"
			                 " return (side == 0 ? sdx - ddx : sdy - ddy) + kind * 1000.0 + guard; };";
			const std::string listing = listing_of(fn);
			check_true(listing.find("INDEX_FUSED_DECL") != std::string::npos, "the member container read is one fused instruction: " + listing);
			check_true(listing.find("CELLS_") != std::string::npos, "the DDA loop closes: " + listing);
			const std::string call = std::string(fn) + " var w = Ws(); array<int> pack = []; int word = 0; int fill = 0;"
			                         " for (int cell = 0; cell < 64; ++cell) { int cx = cell % 8; int cy = cell / 8; int k = (cx == 0 || cy == 0 || cx == 7 || cy == 7) ? 1 : 0;"
			                         "  word = word | (k << ((cell % 15) * 4)); fill = fill + 1; if (fill == 15) { pack.push(word); word = 0; fill = 0; } }"
			                         " if (fill > 0) { pack.push(word); } w.pack = pack;"
			                         " to_string(f(w, 3.5, 3.5, 1.0, 0.66)) + \" \" + to_string(f(w, 3.5, 3.5, -0.7, -0.3)) + \" \" + to_string(f(w, 1.2, 6.1, 0.1, 1.0));";
			for (bool proofs : { true, false }) {
				const std::string vm = run_on(true, call, proofs), in = run_on(false, call, proofs);
				check_eq(in, vm, std::string("both backends agree") + (proofs ? "" : " (proofs off)"));
				check_true(vm.find("throw") == std::string::npos, "the call completes: " + vm);
			}
			// the member read raises like the unfused shape: an index past the end and a missing member, the same text on both backends
			const std::string oob = "class Ws { array<int> pack = [1, 2, 3]; } var r = function(var& t, int i) -> int { return t.pack[i * 2]; };"
			                        " var w = Ws(); string out = \"\"; try { out = to_string(r(w, 1)); } catch (e) { out = \"caught \" + e; } try { out = out + \"|\" + to_string(r(w, 5)); } catch (e) { out = out + \"|caught \" + e; } out;";
			for (bool proofs : { true, false }) {
				check_eq(run_on(false, oob, proofs), run_on(true, oob, proofs), std::string("the fused member read raises the general text") + (proofs ? "" : " (proofs off)"));
			}
		});
		test("an_element_reference_is_a_member_containers_base", [this]() {
			// GLOOM's ray step takes its record as `var& t` bound to an element of `array<WallChunk>` (`for (auto& t : WALL_WS)`), a
			// reference in the container+index mode: the member resolution dereferences it and pins the table; a body that stores into
			// that table keeps the loop on the general form; a var array of records and a typed array behave alike on both backends
			const char* src = "class Chunk { array<float> odist = [1.0, 2.0, 4.0, 8.0]; array<float> out = [0.0, 0.0, 0.0, 0.0]; float k = 1.0; }"
			                  " function f(var& ch, int cols, float vh) -> float { float acc = 0.0; for (int ci = 0; ci < cols; ++ci) { float d = ch.odist[ci]; float v = vh / d * ch.k; ch.out[ci] = v; acc = acc + v; } return acc; }"
			                  " var loose = [Chunk(), Chunk(), Chunk()]; array<Chunk> typed = [Chunk(), Chunk()]; loose[1].k = 2.0; typed[1].k = 3.0;"
			                  " float total = 0.0; for (auto& ch : loose) { total = total + f(ch, 4, 16.0); } for (auto& ch : typed) { total = total + f(ch, 4, 16.0); }"
			                  " function g(var& ch, var& table, int cols) -> float { float acc = 0.0; for (int ci = 0; ci < cols; ++ci) { acc = acc + ch.odist[ci]; if (ci == 1) { table[0] = Chunk(); } } return acc; }"
			                  " var& first = loose[0]; float swapped = g(first, loose, 4);"
			                  " to_string(total) + \" \" + to_string(loose[2].out[3]) + \" \" + to_string(typed[1].out[0]) + \" \" + to_string(swapped) + \" \" + to_string(loose[0].k);";
			for (bool proofs : { true, false }) {
				const std::string vm = run_on(true, src, proofs), in = run_on(false, src, proofs);
				check_eq(in, vm, std::string("both backends agree") + (proofs ? "" : " (proofs off)"));
				check_true(vm.find("throw") == std::string::npos, "the program completes: " + vm);
			}
		});
		test("pushes_onto_a_frame_slot_array_run_inside_the_block", [this]() {
			// gloom_row_quad's shape (GLOOM render.jai 526): string elements of environment tables and string constants pushed onto a
			// local var array inside the cells loop; the loop closes and the joined row agrees with the interpreter; a typed
			// array<string> receiver, an int buffer fed a float (the general text), the memory cap and an index past the end agree too
			const char* src = "array<int> PIX = [3, 3, 3, 3, 5, 3, 5, 3, 1, 2, 3, 4, 7, 7, 7, 7]; array<int> PAL_LUM = [0, 40, 80, 120, 160, 200, 240, 255];"
			                  " array<string> PAL_FG = [\"f0\", \"f1\", \"f2\", \"f3\", \"f4\", \"f5\", \"f6\", \"f7\"]; array<string> PAL_BG = [\"b0\", \"b1\", \"b2\", \"b3\", \"b4\", \"b5\", \"b6\", \"b7\"];"
			                  " array<string> QUADG = [\"q0\", \"q1\", \"q2\", \"q3\", \"q4\", \"q5\", \"q6\", \"q7\", \"q8\", \"q9\", \"qa\", \"qb\", \"qc\", \"qd\", \"qe\", \"qf\"];"
			                  " var row = function(int vw) -> string { var parts = []; int cells = vw / 2; int pfg = -1; int pbg = -1; int itop = 0; int ibot = vw;"
			                  "  for (int cx = 0; cx < cells; ++cx) { int xx = cx * 2; int p0 = PIX[itop + xx]; int p1 = PIX[itop + xx + 1]; int p2 = PIX[ibot + xx]; int p3 = PIX[ibot + xx + 1];"
			                  "   if (p0 == p1 && p0 == p2 && p0 == p3) { if (p0 != pbg) { parts.push(PAL_BG[p0]); pbg = p0; } parts.push(\" \"); continue; }"
			                  "   if (p0 == p2 && p1 == p3) { if (p0 != pfg) { parts.push(PAL_FG[p0]); pfg = p0; } if (p1 != pbg) { parts.push(PAL_BG[p1]); pbg = p1; } parts.push(QUADG[5]); continue; }"
			                  "   int l0 = PAL_LUM[p0]; int l1 = PAL_LUM[p1]; int l2 = PAL_LUM[p2]; int l3 = PAL_LUM[p3]; int lmin = l0; int lmax = l0; int fgp = p0; int bgp = p0;"
			                  "   if (l1 < lmin) { lmin = l1; bgp = p1; } if (l1 > lmax) { lmax = l1; fgp = p1; } if (l2 < lmin) { lmin = l2; bgp = p2; } if (l2 > lmax) { lmax = l2; fgp = p2; }"
			                  "   int thr = (lmin + lmax + 1) / 2; int bits = 0; if (l0 >= thr) { bits = 1; } if (l1 >= thr) { bits += 2; } if (l2 >= thr) { bits += 4; } if (l3 >= thr) { bits += 8; }"
			                  "   if (fgp != pfg) { parts.push(PAL_FG[fgp]); pfg = fgp; } if (bits != 15 && bgp != pbg) { parts.push(PAL_BG[bgp]); pbg = bgp; } parts.push(QUADG[bits]); }"
			                  "  return parts.join(\",\"); };";
			const std::string listing = listing_of(src);
			check_true(listing.find("C_PUSH") != std::string::npos && listing.find("CELLS_") != std::string::npos, "the pushes run inside the block: " + listing);
			const std::string call = std::string(src) + " row(8) + \"|\" + row(8);";
			for (bool proofs : { true, false }) {
				const std::string vm = run_on(true, call, proofs), in = run_on(false, call, proofs);
				check_eq(in, vm, std::string("the joined rows agree") + (proofs ? "" : " (proofs off)"));
				check_true(vm.find("throw") == std::string::npos && vm.find("q") != std::string::npos, "the row is built: " + vm);
			}
			const std::string kinds = "var f = function(int n) -> string { array<string> names = []; array<int> ints = []; array<float> floats = []; var mixed = []; string out = \"\";"
			                          " for (int i = 0; i < n; ++i) { names.push(\"n\"); ints.push(i); floats.push(i); floats.push(0.5); mixed.push(i); mixed.push(true); mixed.push(\"s\"); }"
			                          " out = out + names.join(\"\") + to_string(ints.size()) + to_string(floats[3]) + to_string(mixed.size());"
			                          " try { for (int i = 0; i < n; ++i) { ints.push(1.5); } } catch (e) { out = out + \"|\" + e; }"
			                          " array<string> tbl = [\"a\", \"b\"]; try { for (int i = 0; i < n; ++i) { names.push(tbl[i]); } } catch (e) { out = out + \"|\" + e; }"
			                          " return out; }; f(3);";
			for (bool proofs : { true, false }) {
				check_eq(run_on(false, kinds, proofs), run_on(true, kinds, proofs), std::string("typed receivers, a declined kind and an index past the end agree") + (proofs ? "" : " (proofs off)"));
			}
		});
		test("an_element_push_from_an_element_alias_reads_the_element_of_each_iteration", [this]() {
			// fuzz seed 181 (element band): the push's source container is a range variable or a `var&` alias the block rebinds per iteration
			const char* src = "var T = [[1, \"bb\", 1], [false, \"a\", 2], [false, \"q r\", 1], [3, \"z\", 4]]; var G = [];"
			                  " function local_range() -> string { var out = []; for (auto& r : T) { out.push(r[1]); } return to_json(out); }"
			                  " function global_range() -> string { G = []; for (auto& r : T) { G.push(r[1]); } return to_json(G); }"
			                  " function local_alias() -> string { var out = []; for (int i = 0; i < T.size(); ++i) { var& r = T[i]; out.push(r[2]); } return to_json(out); }"
			                  " local_range() + global_range() + local_alias();";
			for (bool proofs : { true, false }) {
				const std::string vm = run_on(true, src, proofs), in = run_on(false, src, proofs);
				check_eq(in, vm, std::string("each element pushed") + (proofs ? "" : " (proofs off)"));
				check_true(vm.find("\"a\"") != std::string::npos, "the second element is read: " + vm);
			}
		});
		test("ternary_arms_take_every_landing_and_an_int_cell_takes_a_cast_float", [this]() {
			// the ray step's column loop shapes (GLOOM pure.jai 28..62) without its inner while: `float adx = rdx < 0.0 ? -rdx : rdx;`
			// (a unary arm), `int mx = math::itrunc(posx);` and `int texcol = math::itrunc(wallx * 64.0);` (a float truncated into an int), `pdist = pdist < 0.02 ? 0.02 : pdist;` (a ternary assignment); the loop closes and agrees with the interpreter,
			// and a cast of a float out of the int range inside the loop raises the cast's text on both backends
			const char* fn = "var f = function(int n, float posx, float posy, float dirx, float planex, var& out) -> float {"
			                 " float acc = 0.0; float xstep = 2.0 / (n - 1.0);"
			                 " for (int x = 0; x < n; ++x) {"
			                 "  float camx = x * xstep - 1.0; float rdx = dirx + planex * camx;"
			                 "  int mx = math::itrunc(posx); int my = math::itrunc(posy);"
			                 "  float adx = rdx < 0.0 ? -rdx : rdx; float ddx = adx < 0.00000001 ? 100000000.0 : 1.0 / adx;"
			                 "  int stepx = rdx < 0.0 ? -1 : 1;"
			                 "  float sdx = rdx < 0.0 ? (posx - mx) * ddx : (mx + 1.0 - posx) * ddx;"
			                 "  float pdist = sdx - ddx; pdist = pdist < 0.02 ? 0.02 : pdist;"
			                 "  float wallx = posy + pdist * rdx; wallx = wallx - math::floor(wallx);"
			                 "  int texcol = math::itrunc(wallx * 64.0); int shade = 15 - math::itrunc(pdist * 1.55);"
			                 "  shade = shade < 0 ? 0 : (shade > 15 ? 15 : shade);"
			                 "  out[x] = texcol * 16 + shade + stepx + my; acc = acc + pdist;"
			                 " }"
			                 " return acc; };";
			const std::string listing = listing_of(fn);
			check_true(listing.find("CELLS_") != std::string::npos, "the column loop closes: " + listing);
			check_true(listing.find("UNARY op=") == std::string::npos, "the unary arm lands through the move row: " + listing);
			const std::string call = std::string(fn) + " array<int> out = [0, 0, 0, 0, 0, 0]; float r = f(6, 3.5, 2.25, 1.0, 0.66, out);"
			                         " to_string(r) + \" \" + to_string(out[0]) + \" \" + to_string(out[2]) + \" \" + to_string(out[5]);";
			for (bool proofs : { true, false }) {
				const std::string vm = run_on(true, call, proofs), in = run_on(false, call, proofs);
				check_eq(in, vm, std::string("both backends agree") + (proofs ? "" : " (proofs off)"));
				check_true(vm.find("throw") == std::string::npos, "the call completes: " + vm);
			}
			const std::string narrow = "var g = function(int n, float big) -> string { string out = \"\"; int keep = 0;"
			                           " try { for (int i = 0; i < n; ++i) { float f = i * 2.5 + (i == 3 ? big : 0.0); int t = cast<int>(f); keep = keep + t; int s = 7; s = i > 1 ? cast<int>(f) : s; keep = keep + s; } }"
			                           " catch (e) { out = \"caught \" + e; }"
			                           " return out + \"|\" + to_string(keep); }; g(3, 1.0e30) + \" \" + g(6, 1.0e30) + \" \" + g(4, -0.0);";
			for (bool proofs : { true, false }) {
				check_eq(run_on(false, narrow, proofs), run_on(true, narrow, proofs), std::string("a float cast into an int cell truncates in range and raises the cast's text out of range") + (proofs ? "" : " (proofs off)"));
			}
		});
		test("an_outer_loop_closes_over_an_inner_while", [this]() {
			// the ray step's column loop with its DDA while inside (GLOOM pure.jai 28) and render_view_impl's column loop with its
			// two pixel whiles (render.jai 336): the outer counted loop closes as one block whose inner back edges are backward cell
			// jumps; the results agree with the interpreter; an inner while that never exits raises the budget's text on both backends
			const char* ray = "class Ws { int mw = 8; array<int> pack = []; array<float> odist = [0.0, 0.0, 0.0, 0.0]; array<int> okind = [0, 0, 0, 0]; }"
			                  " var f = function(var& t, int n, float posx, float posy, float dirx, float planex) -> float {"
			                  " int mw = t.mw; float acc = 0.0; float xstep = 2.0 / (n - 1.0);"
			                  " for (int x = 0; x < n; ++x) {"
			                  "  float camx = x * xstep - 1.0; float rdx = dirx + planex * camx; float rdy = 0.3 + 0.1 * camx;"
			                  "  int mx = math::itrunc(posx); int my = math::itrunc(posy);"
			                  "  float adx = rdx < 0.0 ? -rdx : rdx; float ady = rdy < 0.0 ? -rdy : rdy;"
			                  "  float ddx = adx < 0.00000001 ? 100000000.0 : 1.0 / adx; float ddy = ady < 0.00000001 ? 100000000.0 : 1.0 / ady;"
			                  "  int stepx = rdx < 0.0 ? -1 : 1; int stepy = rdy < 0.0 ? -1 : 1;"
			                  "  float sdx = rdx < 0.0 ? (posx - mx) * ddx : (mx + 1.0 - posx) * ddx; float sdy = rdy < 0.0 ? (posy - my) * ddy : (my + 1.0 - posy) * ddy;"
			                  "  int side = 0; int kind = 1; int guard = 0; bool hit = false;"
			                  "  while (!hit && guard < 128) {"
			                  "   if (sdx < sdy) { sdx += ddx; mx += stepx; side = 0; } else { sdy += ddy; my += stepy; side = 1; }"
			                  "   int cell = my * mw + mx; int word = t.pack[cell / 15]; int c = (word >> ((cell % 15) * 4)) & 15;"
			                  "   if (c != 0) { hit = true; kind = c; }"
			                  "   guard = guard + 1;"
			                  "  }"
			                  "  float pdist = side == 0 ? sdx - ddx : sdy - ddy; pdist = pdist < 0.02 ? 0.02 : pdist;"
			                  "  t.odist[x] = pdist; t.okind[x] = kind; acc = acc + pdist + guard;"
			                  " }"
			                  " return acc; };";
			const std::string ray_listing = listing_of(ray);
			check_true(ray_listing.find("-- open loop") == std::string::npos && ray_listing.find("C_JUMP") != std::string::npos, "the column loop closes over its DDA while: " + ray_listing);
			const std::string ray_call = std::string(ray) + " var w = Ws(); array<int> pack = []; int word = 0; int fill = 0;"
			                             " for (int cell = 0; cell < 64; ++cell) { int cx = cell % 8; int cy = cell / 8; int k = (cx == 0 || cy == 0 || cx == 7 || cy == 7) ? 1 : 0;"
			                             "  word = word | (k << ((cell % 15) * 4)); fill = fill + 1; if (fill == 15) { pack.push(word); word = 0; fill = 0; } }"
			                             " if (fill > 0) { pack.push(word); } w.pack = pack;"
			                             " to_string(f(w, 4, 3.5, 3.5, 1.0, 0.66)) + \" \" + to_string(w.odist[1]) + \" \" + to_string(w.okind[3]) + \" \" + to_string(f(w, 4, 1.2, 6.1, -0.7, 0.2));";
			for (bool proofs : { true, false }) {
				const std::string vm = run_on(true, ray_call, proofs), in = run_on(false, ray_call, proofs);
				check_eq(in, vm, std::string("the ray columns agree") + (proofs ? "" : " (proofs off)"));
				check_true(vm.find("throw") == std::string::npos, "the call completes: " + vm);
			}
			const char* cols = "var g = function(var& pix, var& strip, int cols, int vh, int vw, int pxw) -> int {"
			                   " int touched = 0;"
			                   " for (int ci = 0; ci < cols; ++ci) {"
			                   "  int lineh = 1 + (ci * 7) % vh; lineh = lineh < 1 ? 1 : lineh; int y0 = (vh - lineh) / 2; int y1 = y0 + lineh;"
			                   "  int cy0 = y0 < 0 ? 0 : y0; int cy1 = y1 > vh ? vh : y1; int tstep = 65536 / lineh; int tacc = (cy0 - y0) * tstep;"
			                   "  int px0 = ci * pxw; int gi = cy0 * vw + px0; int y = cy0;"
			                   "  if (pxw == 2) { while (y < cy1) { int c = strip[tacc >> 11]; pix[gi] = c; pix[gi + 1] = c; tacc += tstep; gi += vw; y = y + 1; } }"
			                   "  else { while (y < cy1) { pix[gi] = strip[tacc >> 11]; tacc += tstep; gi += vw; y = y + 1; } }"
			                   "  touched = touched + 1;"
			                   " }"
			                   " return touched; };";
			const std::string cols_listing = listing_of(cols);
			check_true(cols_listing.find("-- open loop") == std::string::npos, "the column loop closes over its two pixel whiles: " + cols_listing);
			const std::string cols_call = std::string(cols) + " array<int> pix = []; for (int i = 0; i < 8 * 6; ++i) { pix.push(0); } array<int> strip = []; for (int i = 0; i < 32; ++i) { strip.push(i * 3 % 16); }"
			                              " int t2 = g(pix, strip, 4, 6, 8, 2); int t1 = g(pix, strip, 4, 6, 8, 1); to_string(t2) + \" \" + to_string(t1) + \" \" + to_string(pix[8 * 2 + 1]) + \" \" + to_string(pix[8 * 3 + 4]);";
			for (bool proofs : { true, false }) {
				const std::string vm = run_on(true, cols_call, proofs), in = run_on(false, cols_call, proofs);
				check_eq(in, vm, std::string("the pixel columns agree") + (proofs ? "" : " (proofs off)"));
			}
			// an inner while that never exits under a tiny budget: the same text on both backends, raised from the inner back edge
			const std::string spin = "var h = function(int n) -> int { int total = 0; for (int i = 0; i < n; ++i) { int k = 0; while (k >= 0) { k = k + 1; total = total + (k & 1); } } return total; };"
			                         " string out = \"\"; try { out = to_string(h(3)); } catch (e) { out = \"caught \" + e; } out;";
			for (bool vm : { false, true }) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				e->execution_budget(0.00001);
				std::string got;
				try { got = e->execute(spin).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_true(got.find("caught") != std::string::npos || got.find("throw") != std::string::npos, std::string(vm ? "vm" : "interp") + " raises the budget inside the inner while: " + got);
				check_true(got.find("budget") != std::string::npos || got.find("limit") != std::string::npos || got.find("exceeded") != std::string::npos, std::string(vm ? "vm" : "interp") + " names the budget: " + got);
			}
		});
		test("a_table_reached_by_several_member_sites_is_pinned_once", [this]() {
			// the ray step's shape: one member load and four member stores through one element reference into a typed record
			// buffer (five sites, one table); a body that also stores into that table keeps the general form; the same on both
			// backends, proofs on and off
			const std::string script = R"(
				class Rec { int x0 = 0; int x1 = 0; array<int> pack = []; array<float> odist = []; array<int> oside = []; array<int> okind = []; array<int> otex = [];
					Rec(int a, int b) { x0 = a; x1 = b; for (int i = 0; i < 64; ++i) { pack.push(i * 7 + 3); odist.push(0.0); oside.push(0); okind.push(0); otex.push(0); } } }
				array<Rec> WS = []; for (int k = 0; k < 4; ++k) { WS.push(new Rec(k * 16, k * 16 + 16)); }
				function step(var& t) -> void { int x0 = t.x0; int x1 = t.x1; int oi = 0; for (int x = x0; x < x1; ++x) { int cell = x * 3; int word = t.pack[cell / 15]; int c = (word >> ((cell % 15) * 4)) & 15; float pdist = c * 0.5 + 0.25; t.odist[oi] = pdist; t.oside[oi] = c & 1; t.okind[oi] = c; t.otex[oi] = word & 63; oi = oi + 1; } }
				function step_writes_table(var& t, var& ws, int k) -> int { int x0 = t.x0; int x1 = t.x1; int oi = 0; int acc = 0; for (int x = x0; x < x1; ++x) { int word = t.pack[x % 64]; t.okind[oi] = word & 15; acc = acc + t.okind[oi]; if (x == x0 + 2) { ws[k] = t; } oi = oi + 1; } return acc; }
				for (int r = 0; r < 3; ++r) { for (auto& t : WS) { step(t); } }
				int sum = 0; for (auto& t : WS) { for (int i = 0; i < 16; ++i) { sum = sum + t.oside[i] + t.okind[i] + t.otex[i] + math::itrunc(t.odist[i] * 4.0); } }
				int w = step_writes_table(WS[1], WS, 1) + step_writes_table(WS[2], WS, 3);
				to_string(sum) + "," + to_string(w) + "," + to_string(WS[3].x0) + "," + to_string(WS[3].okind[2]);
			)";
			both(script, "", "five member sites on one pinned table");
			both(script, "", "five member sites on one pinned table, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("throw:", 0) != 0 && vm.find(",32,") != std::string::npos, "the record loops ran and the table store copied the record: " + vm);
			const std::string l = listing_of("class Rec { int x0 = 0; int x1 = 0; array<int> pack = []; array<float> odist = []; array<int> oside = []; array<int> okind = []; array<int> otex = []; } var f = function(var& t) { int x0 = t.x0; int x1 = t.x1; int oi = 0; for (int x = x0; x < x1; ++x) { int word = t.pack[x / 15]; int c = (word >> ((x % 15) * 4)) & 15; t.odist[oi] = c * 0.5 + 0.25; t.oside[oi] = c & 1; t.okind[oi] = c; t.otex[oi] = word & 63; oi = oi + 1; } return oi; };");
			check_true(l.find("CELLS_BACK") != std::string::npos && l.find("C_INDEX_STORE") != std::string::npos && l.find("-- open loop") == std::string::npos, "the five-site record loop closes: " + l);
		});
		test("a_local_the_body_declares_lands_by_its_declaration_on_every_iteration", [this]() {
			// the ray step's `int mx = cast<int>(posx);`: a narrowing declaration inside the body lands the cast float on every
			// iteration of a block run (not only the first, when the cell is empty), a cast float assigned to a body-declared int
			// local truncates alike, a var re-kinds freely, an out-of-range float raises the cast's text and a var's float the
			// declaration's text, each with the stores before it kept; the same on both backends, proofs on and off
			const std::string script = R"(
				function narrow(var& xs, int n) -> int { int acc = 0; float f = 0.5; int i = 0; while (i < n) { f = f + 1.25; int m = cast<int>(f); acc = acc + m; xs[i] = m; i = i + 1; } return acc; }
				function assign(var& xs, int n) -> int { int acc = 0; float f = 0.5; int i = 0; while (i < n) { f = f + 1.25; int m = 0; m = cast<int>(f); acc = acc + m; xs[i] = m; i = i + 1; } return acc; }
				function rekind(int n) -> string { string s = ""; float f = 0.5; int i = 0; while (i < n) { var v = i; if ((i % 2) == 1) { v = f; } s = s + to_string(v) + ","; f = f + 1.0; i = i + 1; } return s; }
				function far(var& xs, int n) -> string { float f = 1.0; int i = 0; try { while (i < n) { f = f * 10000000000.0; int m = cast<int>(f); xs[i] = m; i = i + 1; } } catch (err) { return "caught:" + err + "|" + to_string(i) + "|" + to_string(xs[0]); } return "ok"; }
				function refuse(var& xs, var& src, int n) -> string { int i = 0; try { while (i < n) { int m = src[i]; xs[i] = m; i = i + 1; } } catch (err) { return "caught:" + err + "|" + to_string(i) + "|" + to_string(xs[1]); } return "ok"; }
				var XS = [0, 0, 0, 0, 0, 0, 0, 0];
				string a = to_string(narrow(XS, 6)) + "|" + to_string(assign(XS, 6)) + "|" + rekind(5) + "|" + to_string(XS[3]);
				var SRC = [7, 8, 2.5, 9];
				a + "|" + far(XS, 6) + "|" + refuse(XS, SRC, 4);
			)";
			both(script, "", "body-declared landings");
			both(script, "", "body-declared landings, proofs off", false);
			const std::string vm = run_on(true, script);
			check_eq(std::string("27|27|0,1.5,2,3.5,4,|5|caught:Cannot convert float to integer: non-finite or out of range|1|10000000000|caught:Cannot initialize 'm' (declared int) with float|2|8"), vm, "the cast on every iteration, the var re-kinded, each text after its kept stores");
			const std::string l = listing_of("var f = function(var& xs, int n) { int acc = 0; float f = 0.5; int i = 0; while (i < n) { f = f + 1.25; int m = cast<int>(f); acc = acc + m; xs[i] = m; i = i + 1; } return acc; };");
			check_true(l.find("CELLS_BACK") != std::string::npos && l.find("-- open loop") == std::string::npos, "the narrowing loop closes: " + l);
		});
		test("a_push_of_a_getter_backed_element_misses_before_the_getter_runs", [this]() {
			// A host fixture: a property reference stored in an array<var> element through the native array view; its
			// getter adds 10 to a global (or rewrites the element); the block's push must not observe it ahead of the general
			// form, so the sums and the rewrite agree with the interpreter, proofs on and off (a getter that throws is a separate,
			// older boundary gap on both backends: the native exception is not the script's to catch through an element read)
			struct source_t { engine* eng = nullptr; int mode = 0; bool armed = false; int calls = 0; };
			auto run = [&](bool vm, bool proofs, int mode, const char* body) -> std::string {
				source_t source{ nullptr, mode };   // declared first: the engine refers to it until it dies
				auto e = jai::foundry::make_default_engine();
				source.eng = e.get();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				if (!proofs) { e->type_proofs(false); }
				stdlib::register_all(*e); register_host_math_fixture(*e);
				dynamic_binder<source_t>(*e, "Source").property("number", [](source_t& s) -> script_int {
					if (!s.armed) { return 1; }
					++s.calls;
					if (s.mode == 0) { (void)s.eng->execute("G = G + 10;"); }
					else if (s.mode == 2 && s.calls == 2) { (void)s.eng->execute("SRC[0] = 7;"); }
					return 1;
				}, nullptr).build();
				e->add_global_ref("box", source);
				e->add_global("G", script_value(script_int(0), e.get()));
				(void)e->execute("var& r = box.number; var SRC = [0];");
				auto reference = detail::engine_access::get_global_environment(*e)->get_ref("r");
				if (!reference) { return "no reference"; }
				auto src = e->get_variable("SRC");
				src.as_array()[0] = reference.value().get();
				source.armed = true; source.calls = 0;
				std::string out;
				try { out = e->execute(body).to_string(); }
				catch (const std::exception& ex) { out = std::string("throw:") + ex.what(); }
				return out + "|calls=" + std::to_string(source.calls) + "|G=" + e->get_variable("G").to_string();
			};
			const char* sum_body = "int probe() { array<int> dst = []; int sum = 0; int i = 0; while (i < 4) { dst.push(SRC[0]); sum = sum + G; i++; } return sum; } probe();";
			const char* rewrite_body = "string probe() { array<int> dst = []; int i = 0; while (i < 4) { dst.push(SRC[0]); i++; } return to_string(dst[0]) + \",\" + to_string(dst[1]) + \",\" + to_string(dst[2]) + \",\" + to_string(dst[3]); } probe();";
			for (bool proofs : { true, false }) {
				const std::string i0 = run(false, proofs, 0, sum_body), v0 = run(true, proofs, 0, sum_body);
				check_eq(i0, v0, "the getter's effect seen by the sum: both backends");
				check_eq(std::string("100|calls=4|G=40"), v0, "sum 100, four getter calls");
				const std::string i2 = run(false, proofs, 2, rewrite_body), v2 = run(true, proofs, 2, rewrite_body);
				check_eq(i2, v2, "the getter rewriting the element: both backends");
				check_true(v2.rfind("1,1,7,7|", 0) == 0, "the rewritten element from the third push: " + v2);
			}
		});
		test("a_block_closes_over_an_inner_counted_loop", [this]() {
			// part 2 of nested loops: draw_patch's shape (a counted column loop over a counted row loop with continues in both, a texel
			// table and a palette, the view width an environment scalar), += 2 and -- steps, a break out of the inner loop, an overflow
			// of the inner step (the general text, the count before it kept), an environment-scalar end, a three-deep nest, and a miss
			// inside the inner body (a bool element landing on an int declaration at the third column) that hands the general form the
			// rest of the iteration with the counted state in place; the same on both backends, proofs on and off
			const std::string script = R"(
				var VW = 8; var VH = 4;
				function patch(var& pix, var& tp, var& pal, int x0, int y0, int w, int h) -> int {
					int acc = 0;
					for (int sx = 0; sx < w; ++sx) { int px = x0 + sx; if (px < 0 || px >= VW) { continue; } int base = sx * h;
						for (int sy = 0; sy < h; ++sy) { int py = y0 + sy; if (py < 0 || py >= VH) { continue; } int texel = tp[base + sy]; if (texel >= 0) { pix[py * VW + px] = pal[texel]; acc = acc + pal[texel]; } } }
					return acc; }
				function steps(int n) -> int { int acc = 0; int i = 0; while (i < n) { for (int j = 0; j < 10; j += 2) { acc = acc + j; } for (int k = 5; k > 0; --k) { acc = acc + k; } i = i + 1; } return acc; }
				function brk(int n) -> int { int acc = 0; int i = 0; while (i < n) { for (int j = 0; j < 100; ++j) { if (j == 3) { break; } acc = acc + 1; } i = i + 1; } return acc; }
				function ovf() -> string { int acc = 0; try { int i = 0; while (i < 2) { for (int j = 9223372036854775800; j <= 9223372036854775807; j += 4) { acc = acc + 1; } i = i + 1; } } catch (err) { return "caught:" + err + "|" + to_string(acc); } return "ok:" + to_string(acc); }
				function envend(int n) -> int { int acc = 0; int i = 0; while (i < n) { for (int j = 0; j < VW; ++j) { acc = acc + j; } i = i + 1; } return acc; }
				function deep(int n) -> int { int acc = 0; int i = 0; while (i < n) { for (int a = 0; a < 3; ++a) { for (int b = 0; b < 2; ++b) { acc = acc + a * b + 1; } } i = i + 1; } return acc; }
				function missy(var& xs, int n) -> int { int acc = 0; int i = 0; while (i < n) { for (int j = 0; j < 4; ++j) { int v = xs[j]; acc = acc + v; } i = i + 1; } return acc; }
				var PIX = [0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0];
				var TP = [1, -1, 2, 3, 0, 1, 2, -1, 3, 3, 1, 0]; var PAL = [10, 20, 30, 40];
				var XS = [1, 2, true, 4];
				to_string(patch(PIX, TP, PAL, -1, 1, 4, 3)) + "|" + to_string(steps(3)) + "|" + to_string(brk(3)) + "|" + ovf() + "|" + to_string(envend(2)) + "|" + to_string(deep(2)) + "|" + to_string(missy(XS, 3)) + "|" + to_string(PIX[9]) + "," + to_string(PIX[17]);
			)";
			both(script, "", "inner counted loops");
			both(script, "", "inner counted loops, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("throw:", 0) != 0 && vm.find("|caught:") != std::string::npos && vm.find("|2|") != std::string::npos && vm.find("|24|") != std::string::npos, "the shapes ran, the overflow caught after two steps, the bool element summed as one: " + vm);
			const std::string l = listing_of("var f = function(var& tp, var& pal, var& pix, int w, int h) { int acc = 0; for (int sx = 0; sx < w; ++sx) { for (int sy = 0; sy < h; ++sy) { int t = tp[sx * h + sy]; if (t >= 0) { pix[sy * 8 + sx] = pal[t]; acc = acc + t; } } } return acc; };");
			check_true(l.find("CELLS_CFOR_BACK") != std::string::npos && (l.find("C_JCMP") != std::string::npos || l.find("C_JL") != std::string::npos || l.find("C_JG") != std::string::npos) && l.find("-- open loop") == std::string::npos, "the counted column loop closes over its counted row loop: " + l);
		});
		test("an_untranslatable_instruction_inside_a_branch_arm_is_a_bail", [this]() {
			// GLOOM's column loop (a cache-fill arm storing a call result into a global table the first time a key is seen, the pixel
			// loop after it), JaiDOOM's fuzz loop (a call result stored into the pixel table under a parity test), a bail whose arm is
			// taken on every iteration, a bail inside an inner while, and a bail with a body-defined value live across it; the same on
			// both backends, proofs on and off; the listing shows the bail and no open loop
			const std::string script = R"(
				var STRIPS = [0, 0, 0, 0]; var STRIP_SET = [0, 0, 0, 0]; var PIX = [0, 0, 0, 0, 0, 0, 0, 0];
				function build_strip(int k) -> var { return [k * 10, k * 10 + 1, k * 10 + 2, k * 10 + 3]; }
				function columns(var& pix, int cols, int vh) -> int {
					int acc = 0; int x = 0;
					while (x < cols) {
						int sidx = x % 4; int have = STRIP_SET[sidx];
						if (have == 0) { STRIPS[sidx] = build_strip(sidx); STRIP_SET[sidx] = 1; }
						var& strip = STRIPS[sidx]; int y = 0; int gi = x;
						while (y < vh) { int c = strip[y]; pix[gi % 8] = c; acc = acc + c; gi = gi + 2; y = y + 1; }
						x = x + 1; }
					return acc; }
				function dark(int v) -> int { return v - 1; }
				function fuzz(var& pix, int n) -> int { int acc = 0; int i = 0; while (i < n) { int fpi = i % 8; if ((i & 1) != 0) { pix[fpi] = dark(pix[fpi]); } acc = acc + pix[fpi]; i = i + 1; } return acc; }
				function always(var& pix, int n) -> int { int acc = 0; int i = 0; while (i < n) { int fpi = i % 8; if (i >= 0) { pix[fpi] = dark(pix[fpi]); } acc = acc + pix[fpi]; i = i + 1; } return acc; }
				function inner(var& pix, int n) -> int { int acc = 0; int i = 0; while (i < n) { int j = 0; while (j < 4) { if (j == 3) { pix[j] = dark(pix[j]); } acc = acc + pix[j] + j; j = j + 1; } i = i + 1; } return acc; }
				function live(var& pix, int n) -> int { int acc = 0; int i = 0; while (i < n) { int t = i * 3 + 1; if (i == 2) { pix[i] = dark(pix[i]); } acc = acc + t * 2; i = i + 1; } return acc; }
				to_string(columns(PIX, 6, 4)) + "|" + to_string(fuzz(PIX, 10)) + "|" + to_string(always(PIX, 5)) + "|" + to_string(inner(PIX, 3)) + "|" + to_string(live(PIX, 5)) + "|" + to_string(PIX[0]) + "," + to_string(PIX[3]) + "," + to_string(STRIP_SET[2]);
			)";
			both(script, "", "bails");
			both(script, "", "bails, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("throw:", 0) != 0 && vm.find("|") != std::string::npos, "the shapes ran: " + vm);
			const std::string l = listing_of("var STRIPS = [0, 0, 0, 0]; var STRIP_SET = [0, 0, 0, 0]; function build_strip(int k) -> var { return [k, k, k, k]; } var f = function(var& pix, int cols, int vh) { int acc = 0; int x = 0; while (x < cols) { int sidx = x % 4; int have = STRIP_SET[sidx]; if (have == 0) { STRIPS[sidx] = build_strip(sidx); STRIP_SET[sidx] = 1; } int y = 0; int gi = x; while (y < vh) { int c = STRIP_SET[sidx] + y; pix[gi % 8] = c; acc = acc + c; gi = gi + 2; y = y + 1; } x = x + 1; } return acc; };");
			check_true(l.find("C_BOUNDARY") != std::string::npos && l.find("CELLS_BACK") != std::string::npos && l.find("-- open loop") == std::string::npos, "the column loop closes with a boundary in its cache-fill arm (without the reference alias, a main-path gap of its own): " + l);
			const std::string m = listing_of("function dark(int v) -> int { return v - 1; } var f = function(var& pix, int n) { int acc = 0; int i = 0; while (i < n) { int fpi = i % 8; pix[fpi] = dark(pix[fpi]); acc = acc + pix[fpi]; i = i + 1; } return acc; };");
			check_true(m.find("-- open loop") == std::string::npos && m.find("C_BOUNDARY") != std::string::npos && m.find("C_BAIL") == std::string::npos, "a translation gap on the main path is a boundary: " + m);
			// lights_tic's shape (JaiDOOM spec.jai): a declaration the translation does not take inside an arm, read by the arm's later
			// statements, under a continue-guarded body; a bail stands for the rest of its arm, so the arm's local leaves no cell to guard
			// at entry (otherwise lights_tic slot 5 is want=1 have=0, refused at every entry)
			const std::string lights = R"(
				var LT_MAX = [200, 180, 160, 140]; var LT_MIN = [100, 90, 80, 70]; var SE_LIGHT = [0, 0, 0, 0]; var LT_CNT = [1, 2, 1, 3]; var LT_SEC = [0, 1, 2, 3]; var LT_KIND = [0, 1, 2, 0];
				function jitter(int k) -> int { return k % 3 + 1; }
				function wobble(int k) -> int { for (int q = 0; q < 1; ++q) { if (q > 5) { return 0; } } return k % 3 + 1; }
				var tic = function() {
					int n = LT_SEC.size();
					for (int i = 0; i < n; ++i) {
						int s = LT_SEC[i]; int kind = LT_KIND[i];
						LT_CNT[i] = LT_CNT[i] - 1;
						if (LT_CNT[i] > 0) { continue; }
						if (kind == 1) {
							if (SE_LIGHT[s] == LT_MAX[i]) { SE_LIGHT[s] = LT_MIN[i]; LT_CNT[i] = jitter(i); }
							else { SE_LIGHT[s] = LT_MAX[i]; LT_CNT[i] = jitter(i + 1); }
						} else {
							int l2 = LT_MAX[i] - wobble(i) * 16;
							if (l2 < LT_MIN[i]) { l2 = LT_MIN[i]; }
							SE_LIGHT[s] = l2; LT_CNT[i] = 4;
						}
					}
				};
				tic(); tic(); tic();
				to_string(SE_LIGHT[0]) + "," + to_string(SE_LIGHT[1]) + "," + to_string(SE_LIGHT[2]) + "," + to_string(SE_LIGHT[3]) + "|" + to_string(LT_CNT[0]) + "," + to_string(LT_CNT[1]) + "," + to_string(LT_CNT[2]) + "," + to_string(LT_CNT[3]);
			)";
			both(lights, "", "a declaration bailed inside an arm");
			both(lights, "", "a declaration bailed inside an arm, proofs off", false);
			check_eq(std::string("184,180,112,124|2,2,2,4"), run_on(true, lights));
			const std::string k = listing_of(lights);
			size_t bails = 0, last_bail = std::string::npos;
			for (size_t at = k.find("C_BOUNDARY"); at != std::string::npos; at = k.find("C_BOUNDARY", at + 1)) { ++bails; last_bail = at; }
			// the two element stores of jitter's result graft (their arguments are locals), while `int l2 = LT_MAX[i] - wobble(i) * 16`
			// keeps its call (wobble returns inside a loop) and runs as a boundary standing for the declaration and the statements after it in
			// its arm (the skips after it in the region's listing)
			(void)last_bail;
			size_t jitter_grafts = 0; for (size_t at = k.find("-- inlined jitter"); at != std::string::npos; at = k.find("-- inlined jitter", at + 1)) { ++jitter_grafts; }
			check_true(bails == 1 && jitter_grafts == 2 && k.find("-- open loop") == std::string::npos && k.find("boundary->") != std::string::npos && k.find("boundary->") < k.find("skip;", k.find("boundary->")),
			           "the light loop closes with one boundary standing for the declaration and the statements after it in its arm, and two jitter grafts: " + k);
			// the same declaration over a leaf: `LT_MAX[i]`, an element read of a var array with no proven kind, reads into a temp ahead of
			// the graft, so jitter grafts there too and the arm needs no bail
			std::string graft_lights = lights;
			const std::string wob = "wobble(i) * 16";
			graft_lights.replace(graft_lights.find(wob), wob.size(), "jitter(i) * 16");
			both(graft_lights, "", "an element read ahead of a graft in an arm");
			both(graft_lights, "", "an element read ahead of a graft in an arm, proofs off", false);
			check_eq(std::string("184,180,112,124|2,2,2,4"), run_on(true, graft_lights));
			const std::string g = listing_of(graft_lights);
			size_t g_grafts = 0; for (size_t at = g.find("-- inlined jitter"); at != std::string::npos; at = g.find("-- inlined jitter", at + 1)) { ++g_grafts; }
			check_true(g_grafts == 3 && g.find("C_BAIL") == std::string::npos && g.find("C_BOUNDARY") == std::string::npos && g.find("-- open loop") == std::string::npos, "all three jitter calls graft and the loop closes without a bail or a boundary: " + g);
			const size_t cells_at = k.find("-- cell loop");
			const std::string cells_line = cells_at == std::string::npos ? std::string() : k.substr(cells_at, k.find('\n', cells_at) - cells_at);
			bool adopted_slot = false;
			for (size_t at = cells_line.find(" s"); at != std::string::npos; at = cells_line.find(" s", at + 1)) {
				size_t d = at + 2;
				while (d < cells_line.size() && cells_line[d] >= '0' && cells_line[d] <= '9') { ++d; }
				if (d > at + 2 && cells_line.compare(d, 2, ":0") == 0) { adopted_slot = true; }
			}
			check_true(!adopted_slot, "no slot cell of an adopted kind remains from the bailed arm: " + cells_line);
		});
		test("an_element_alias_declared_in_the_body_is_a_rebind", [this]() {
			// GLOOM's column loop as written (the cache-fill arm, then `var& strip = STRIPS[sidx]` and the pixel loop through it): the alias's
			// declaration rebinds its sites' container once per iteration and the loop closes; a store through the alias; a callee between the
			// alias and its reads replacing the element; a store into the source table (the pinned table refuses the entry); an element that is
			// not an array; a read through the alias missing mid-iteration (the general form resumes with the alias bound); both backends, proofs on and off
			const std::string script = R"(
				var STRIPS = [0, 0, 0, 0]; var STRIP_SET = [0, 0, 0, 0]; var PIX = [0, 0, 0, 0, 0, 0, 0, 0];
				function build_strip(int k) -> var { return [k * 10, k * 10 + 1, k * 10 + 2, k * 10 + 3]; }
				function columns(var& pix, int cols, int vh) -> int {
					int acc = 0; int x = 0;
					while (x < cols) {
						int sidx = x % 4; int have = STRIP_SET[sidx];
						if (have == 0) { STRIPS[sidx] = build_strip(sidx); STRIP_SET[sidx] = 1; }
						var& strip = STRIPS[sidx]; int y = 0; int gi = x;
						while (y < vh) { int c = strip[y]; pix[gi % 8] = c; acc = acc + c; gi = gi + 2; y = y + 1; }
						x = x + 1; }
					return acc; }
				function writes(int n) -> int {
					int acc = 0; int i = 0;
					while (i < n) { var& row = STRIPS[i % 4]; row[i % 4] = row[i % 4] + 1; acc = acc + row[(i + 1) % 4]; i = i + 1; }
					return acc; }
				function swap_strip(int k) -> int { STRIPS[k] = [k + 100, 1, 2, 3]; return 1; }
				function replaced(int n) -> int {
					int acc = 0; int i = 0;
					while (i < n) { var& s = STRIPS[i % 4]; int k = swap_strip(i % 4); acc = acc + s[0] + k; i = i + 1; }
					return acc; }
				function stale(int n) -> string {
					int acc = 0; int i = 0;
					try { while (i < n) { var& s = STRIPS[i % 4]; STRIPS[(i + 1) % 4] = 7; acc = acc + s[1]; i = i + 1; } } catch (err) { return "caught:" + to_string(acc); }
					return "ok:" + to_string(acc); }
				function scalar_element(int n) -> string {
					var T = [1, [2, 3], 4]; int acc = 0; int i = 0;
					try { while (i < n) { var& e = T[i]; acc = acc + e[0]; i = i + 1; } } catch (err) { return "caught:" + to_string(acc); }
					return "ok:" + to_string(acc); }
				function mid_loop_miss(int n) -> string {
					var T = [[1, 2], [3, "x"], [5, 6]]; int acc = 0; int i = 0;
					try { while (i < n) { var& r = T[i]; int a = r[0]; int b = r[1]; acc = acc + a + b; i = i + 1; } } catch (err) { return "caught:" + to_string(acc); }
					return "ok:" + to_string(acc); }
				to_string(columns(PIX, 6, 4)) + "|" + to_string(writes(6)) + "|" + to_string(replaced(5)) + "|" + stale(5) + "|" + scalar_element(3) + "|" + mid_loop_miss(3) + "|" + to_string(PIX[0]) + "," + to_string(PIX[3]) + "," + to_string(STRIPS[3][1]);
			)";
			both(script, "", "element alias rebinds");
			both(script, "", "element alias rebinds, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("throw:", 0) != 0 && vm.find("caught:") != std::string::npos, "the shapes ran: " + vm);
			const std::string l = listing_of("var STRIPS = [0, 0, 0, 0]; var STRIP_SET = [0, 0, 0, 0]; function build_strip(int k) -> var { return [k, k, k, k]; } var f = function(var& pix, int cols, int vh) { int acc = 0; int x = 0; while (x < cols) { int sidx = x % 4; int have = STRIP_SET[sidx]; if (have == 0) { STRIPS[sidx] = build_strip(sidx); STRIP_SET[sidx] = 1; } var& strip = STRIPS[sidx]; int y = 0; int gi = x; while (y < vh) { int c = strip[y]; pix[gi % 8] = c; acc = acc + c; gi = gi + 2; y = y + 1; } x = x + 1; } return acc; };");
			check_true(l.find("C_REBIND") != std::string::npos && l.find("C_BOUNDARY") != std::string::npos && l.find("CELLS_BACK") != std::string::npos && l.find("-- open loop") == std::string::npos, "the column loop closes with its alias as a rebind and its cache-fill arm as a boundary: " + l);
		});
		test("a_range_for_over_an_array_is_a_block", [this]() {
			// a range-for over an array as a block head: by value (a typed buffer, a var array, an int-declared variable over a var array, which binds
			// each element as the general form does: a bool converts, a float raises the declaration's text), by reference over arrays of arrays (a rebind), a continue and a break, the body growing the
			// array (the live size), an element the cells do not take (a string: one miss, then the call's iterations run general), a call inside,
			// an instance element by reference (a miss at the landing until member cells), a scalar element by reference, a mid-body miss that
			// resumes at the right element, and a nested range-for; both backends, proofs on and off
			const std::string script = R"(
				class P { float x = 0.0; float vx = 1.0; }
				function twice(int v) -> int { return v * 2; }
				function sum_typed(int n) -> int { array<int> a = [1, 2, 3, 4]; int s = 0; int k = 0; while (k < n) { for (int v : a) { s = s + v; } k = k + 1; } return s; }
				function sum_var() -> int { var a = [5, 6, 7]; int s = 0; for (auto v : a) { s = s + v; } return s; }
				function int_over_floats() -> string { var a = [true, 2, 2.5]; string t = ""; try { for (int v : a) { t = t + type_of(v) + to_string(v); } } catch (err) { return t + ":" + err; } return t; }
				function rows() -> int { var g = [[1, 2], [3, 4], [5, 6]]; int s = 0; for (auto& r : g) { r[0] = r[0] + 10; s = s + r[0] + r[1]; } return s + g[2][0]; }
				function skip_break() -> int { var a = [1, 2, 3, 4, 5, 6]; int s = 0; for (auto v : a) { if (v == 2) { continue; } if (v == 5) { break; } s = s + v; } return s; }
				function grows() -> int { var a = [1, 2]; int s = 0; for (auto v : a) { if (v == 2) { a.push(9); } s = s + v; } return s; }
				function strings() -> string { var a = ["a", "bb", "ccc"]; int n = 0; for (auto v : a) { n = n + 1; } return to_string(n); }
				function calls() -> int { var a = [1, 2, 3]; int s = 0; for (auto v : a) { s = s + twice(v); } return s; }
				function objs() -> float { array<P> pool = [P(), P(), P()]; for (auto& p : pool) { p.x = p.x + p.vx; } float s = 0.0; for (auto& p : pool) { s = s + p.x; } return s; }
				function scalar_ref() -> int { var a = [1, 2, 3]; for (auto& v : a) { v = v * 2; } return a[0] + a[1] + a[2]; }
				function mid_miss() -> string { var a = [1, 2, true, 4]; int s = 0; try { for (auto v : a) { int w = v; s = s + w; } } catch (err) { return "caught:" + to_string(s); } return "ok:" + to_string(s); }
				function nested() -> int { var g = [[1, 2], [3]]; int s = 0; for (auto& r : g) { for (auto v : r) { s = s + v; } } return s; }
				to_string(sum_typed(3)) + "|" + to_string(sum_var()) + "|" + int_over_floats() + "|" + to_string(rows()) + "|" + to_string(skip_break()) + "|" + to_string(grows()) + "|" + strings() + "|" + to_string(calls()) + "|" + to_string(objs()) + "|" + to_string(scalar_ref()) + "|" + mid_miss() + "|" + to_string(nested());
			)";
			both(script, "", "range-for blocks");
			both(script, "", "range-for blocks, proofs off", false);
			const std::string vm = run_on(true, script);
			check_eq(std::string("30|18|int1int2:Cannot initialize 'v' (declared int) with float|66|8|12|3|12|3.0|12|ok:8|6"), vm, "the shapes");
			const std::string l = listing_of("var f = function(int n) { array<int> a = [n, 2]; int s = 0; for (int v : a) { s = s + v; } return s; };");
			check_true(l.find("C_RANGE") != std::string::npos && l.find("C_BACK_STEP") != std::string::npos && l.find("C_LOAD") != std::string::npos && l.find("-- open loop") == std::string::npos, "a by-value range closes with the range head and the stepping back edge: " + l);
			const std::string m = listing_of("var f = function(var& g) { int s = 0; for (auto& r : g) { r[0] = r[0] + 1; s = s + r[1]; } return s; };");
			check_true(m.find("C_RANGE") != std::string::npos && m.find("C_REBIND") != std::string::npos && m.find("-- open loop") == std::string::npos, "a by-reference range closes with a rebind at the head: " + m);
		});
		test("member_reads_land_in_temps_and_member_stores_read_their_base_from_the_slot", [this]() {
			// the general form: a member read of a frame-slot base is a compiler temp (op_get_member's kernel, its place among the operands
			// kept), so it feeds the fused binaries and the typed lowering; a member store on a frame-slot base reads the base from the slot.
			// Both members of a binary, a member with a local, a member with a global read first, a user getter in a binary, a getter
			// that changes the partner global (the order), a null base (the text), a store through a reference parameter, a var's float stored into
			// an int field (the text, the earlier store kept), a store whose value is used, a user setter; both backends, proofs on and off
			const std::string script = R"(
				class P { float x = 1.5; float vx = 0.25; int n = 3; string s = "a"; }
				class G { int hits = 0; int _get_v() { hits = hits + 1; TOTAL = TOTAL + 100; return 7; } void _set_w(int v) { hits = hits + 10; } }
				var TOTAL = 1;
				function both_members(P p) -> float { return p.x + p.vx; }
				function member_local(P p, float y) -> float { float a = p.x * y; float b = y - p.vx; return a + b; }
				function global_first(G g) -> int { return TOTAL + g.v; }
				function getter_binary(G g) -> int { int r = g.v * 2 + g.v; return r + g.hits; }
				function null_base(int k) -> string { P q = P(); var z = null; try { if (k == 1) { return to_string(z.x + 1.0); } } catch (err) { return "caught:" + err; } return to_string(q.n + 1); }
				function store_ref(P& p, float d) -> float { p.x = p.x + d; p.vx = d; return p.x + p.vx; }
				function store_int(P p) -> string { var h = 2.5; try { p.n = p.n + 1; p.n = h; } catch (err) { return "caught:" + err + to_string(p.n); } return "ok:" + to_string(p.n); }
				function store_used(P p) -> float { float r = (p.x = 4.5) + 1.0; return r + p.x; }
				function setter(G g) -> int { g.w = 5; g.w = g.hits + 1; return g.hits; }
				function decls(P p) -> string { float x = p.x; int k = p.n; var s = p.s; auto v = p.vx; p.x = 2.5; try { var q = p.x; int bad = q; return "ok:" + to_string(x + k + v) + s + to_string(bad); } catch (err) { return "caught:" + to_string(x + k + v) + s; } }
				function cond(P p, int t) -> int { int c = 0; if (p.n > t) { c = c + 1; } if (p.x < 2.0 && p.vx > 0.0) { c = c + 10; } return c; }
				class Leaf { int v = 0; string tag = "leaf"; } class Node { Leaf a = Leaf(); }
				function copies(Node n) -> string { auto l = n.a; l.tag = l.tag + "!"; var m = n.a; m.v = 5; string t = n.a.tag; t = t + "?"; return n.a.tag + to_string(n.a.v) + l.tag + to_string(m.v) + t; }
				P p = P(); G g = G(); Node nd = Node();
				to_string(both_members(p)) + "|" + to_string(member_local(p, 2.0)) + "|" + to_string(global_first(g)) + "|" + to_string(getter_binary(g)) + "|" + null_base(1) + "|" + null_base(0) + "|" + to_string(store_ref(p, 0.5)) + "|" + store_int(p) + "|" + to_string(store_used(p)) + "|" + to_string(setter(g)) + "|" + decls(p) + "|" + to_string(cond(p, 2)) + "|" + copies(nd) + "|" + to_string(TOTAL) + "|" + to_string(p.x);
			)";
			both(script, "", "member temps");
			both(script, "", "member temps, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("throw:", 0) != 0 && vm.find("|caught:Cannot assign float to field 'n' of P (declared int)4|") != std::string::npos, "the shapes ran: " + vm);
			const std::string l = listing_of("class P { float x = 1.5; float vx = 0.25; } var f = function(P p, float y) { float w = p.x; p.x = w + y; return p.x + p.vx; };");
			check_true(l.find("MEMBER_TEMP") != std::string::npos && (l.find("SET_MEMBER_FUSED") != std::string::npos || l.find("SET_MEMBER_VALUE") != std::string::npos) && l.find("T_DECL_MOV") != std::string::npos && l.find("GET_MEMBER") == std::string::npos && l.find("SET_MEMBER ") == std::string::npos, "member reads are temps (the declaration a move row) and the store reads its base from the slot: " + l);
		});
		test("an_alias_lives_to_the_end_of_the_body_and_an_inner_counted_step_polls_the_budget", [this]() {
			// Regressions: a return inside the body reading the alias after its last read
			// inside the block (the flush left the first iteration's reference), and an inner counted loop whose step never polled the budget
			// (a zero step, a huge bound); and the coverage limit beside them: a while holding a counted for was skipped as the counted
			// loop's own generic duplicate. Both backends, proofs on and off
			const std::string alias = "int probe() { var strips = [[1], [2, 20], [3, 30, 300]]; int sum = 0; int i = 0; while (i < 3) { var& strip = strips[i]; sum += strip[0]; if (i == 2) { return sum + strip.size(); } i++; } return sum; } to_string(probe());";
			both(alias, "", "an alias live at a return");
			both(alias, "", "an alias live at a return, proofs off", false);
			check_eq(std::string("9"), run_on(true, alias));
			const std::string alias_break = "int probe() { var g = [[1], [2, 20], [3, 30, 300]]; int sum = 0; int i = 0; int last = 0; while (i < 3) { var& row = g[i]; sum += row[0]; if (sum > 2) { last = row.size(); break; } i++; } return sum * 10 + last; } to_string(probe());";
			both(alias_break, "", "an alias live at a break");
			check_eq(std::string("32"), run_on(true, alias_break));
			auto budget_run = [&](bool vm, bool proofs, const std::string& src) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				if (!proofs) { e->type_proofs(false); }
				e->execution_budget(0.05);
				try { return "ok:" + e->execute(src).as_string(); }
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			const std::string late = "int probe() { int sum = 0; for (int i = 0; i < 3; i++) { int step = 1; if (i == 1) { step = 0; } for (int j = 0; j < 3; j += step) { sum += 0; } } return sum; } to_string(probe());";
			const std::string bound = "int probe() { int sum = 0; for (int i = 0; i < 3; i++) { int bound = 3; if (i == 1) { bound = 1000000000; } for (int j = 0; j < bound; j++) { sum += 0; } } return sum; } to_string(probe());";
			for (const std::string* src : { &late, &bound }) {
				const std::string interp = budget_run(false, true, *src);
				check_true(interp.rfind("throw:", 0) == 0, "the interpreter raises the budget: " + interp);
				check_eq(interp, budget_run(true, true, *src));
				check_eq(interp, budget_run(true, false, *src));
			}
			const std::string l = listing_of("var f = function(int n) { int s = 0; int i = 0; while (i < n) { for (int j = 0; j < 3; j++) { s = s + j; } i = i + 1; } return s; };");
			size_t closed = 0;
			for (size_t at = l.find("-- cell loop"); at != std::string::npos; at = l.find("-- cell loop", at + 1)) { ++closed; }
			check_true(closed >= 2 && l.find("-- open loop") == std::string::npos, "a while holding a counted for closes over it (the duplicate test names the counted loop's own fallback): " + l);
			check_true(l.find("C_JUMP") != std::string::npos, "the inner step is preceded by the budget poll: " + l);
		});
		test("a_typed_push_moves_the_payload_without_an_intermediate_value", [this]() {
			// a cell or a typed element pushed into a typed buffer moves its payload; a hetero
			// destination still takes a value with the cell's declared stamp; the conversion subset (a var's float into ints raises with the earlier pushes kept), the memory charge, self pushes and the
			// fallbacks are the general push's. Both backends, proofs on and off
			const std::string script = R"(
				array<int> INTS = [1, 2, 3]; array<float> FLOATS = [0.5, 1.5]; array<string> STRS = ["a", "bb"]; var PAL = ["x", "yy", "zzz"];
				function ints(int n) -> int { array<int> out = []; int i = 0; while (i < n) { int v = i * 3; out.push(v); int m = i % 3; out.push(INTS[m]); i = i + 1; } int s = 0; int k = 0; while (k < out.size()) { s = s + out[k]; k = k + 1; } return s * 100 + out.size(); }
				function floats(int n) -> float { array<float> out = []; int i = 0; while (i < n) { float v = i * 0.25; out.push(v); out.push(i); int m = i % 2; out.push(FLOATS[m]); i = i + 1; } float s = 0.0; int k = 0; while (k < out.size()) { s = s + out[k]; k = k + 1; } return s * 10.0 + out.size(); }
				function strings(int n) -> string { array<string> out = []; int i = 0; while (i < n) { int m = i % 2; out.push(STRS[m]); i = i + 1; } string s = ""; int k = 0; while (k < out.size()) { s = s + out[k]; k = k + 1; } return s + to_string(out.size()); }
				function hetero(int n) -> string { var out = []; int i = 0; while (i < n) { int v = i + 1; float w = i * 0.5; bool b = i % 2 == 0; out.push(v); out.push(w); out.push(b); int m = i % 3; out.push(PAL[m]); out.push(INTS[m]); i = i + 1; } return to_string(out.size()) + type_of(out[0]) + type_of(out[1]) + type_of(out[2]) + type_of(out[3]) + type_of(out[4]) + to_string(out[3]); }
				function float_into_ints(int n) -> string { array<int> out = []; int i = 0; try { while (i < n) { var v = i < 2 ? i : i * 1.5; out.push(v); i = i + 1; } } catch (err) { return "caught:" + err + to_string(out.size()); } return "ok:" + to_string(out.size()); }
				function string_into_ints(int n) -> string { array<int> out = []; int i = 0; try { while (i < n) { int m = i % 2; var s = STRS[m]; out.push(s); i = i + 1; } } catch (err) { return "caught:" + to_string(out.size()); } return "ok:" + to_string(out.size()); }
				function self_push(int n) -> int { array<int> a = [1, 2]; int i = 0; while (i < n) { a.push(a[i]); i = i + 1; } int s = 0; int k = 0; while (k < a.size()) { s = s + a[k]; k = k + 1; } return s * 100 + a.size(); }
				function self_push_strings(int n) -> string { array<string> a = ["p", "q"]; int i = 0; while (i < n) { a.push(a[i]); i = i + 1; } string s = ""; int k = 0; while (k < a.size()) { s = s + a[k]; k = k + 1; } return s; }
				to_string(ints(6)) + "|" + to_string(floats(5)) + "|" + strings(5) + "|" + hetero(4) + "|" + float_into_ints(3) + "|" + string_into_ints(3) + "|" + to_string(self_push(6)) + "|" + self_push_strings(5);
			)";
			both(script, "", "typed pushes");
			both(script, "", "typed pushes, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("throw:", 0) != 0 && vm.find("|caught:Cannot push 'float' to array<int>2|caught:") != std::string::npos, "the shapes ran: " + vm);
			const std::string l = listing_of("array<int> INTS = [1, 2, 3]; var f = function(int n) { array<int> out = []; int i = 0; while (i < n) { int v = i * 3; out.push(v); int m = i % 3; out.push(INTS[m]); i = i + 1; } return out.size(); };");
			check_true(l.find("C_PUSH") != std::string::npos && l.find("-- open loop") == std::string::npos, "the typed pushes close the loop: " + l);
		});
		test("the_recognizer_marks_the_column_loop", [this]() {
			auto vm = jai::foundry::make_default_engine();
			vm->set_backend(backend_type::vm);
			stdlib::register_all(*vm);
			auto* symbolizer = detail::engine_access::get_symbolizer(*vm);
			const char* script = "array<int> PIX = [0, 0, 0, 0]; var S = [[1, 2, 3]];"
			                     " var f = function(var& pix, var& strip, int cy0, int cy1, int vw, int px0, int tstep) {"
			                     " int gi = cy0 * vw + px0; int tacc = 0; int y = cy0;"
			                     " while (y < cy1) { int c = strip[tacc >> 11]; pix[gi] = c; pix[gi + 1] = c; tacc += tstep; gi += vw; y = y + 1; }"
			                     " return gi; };";
			auto templates = detail::engine_access::get_registered_template_types(*vm);
			jai::lexer lex(script, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, vm.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			(void)jai::detail::run_static_check(*vm, parse_result.value());
			jai::vm::vm_compiler compiler(symbolizer);
			const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
			check_true(listing.find("CELLS_BACK") != std::string::npos, "the closed typed loop's back edge is the cell entry: " + listing);
			check_true(listing.find("C_LOAD") != std::string::npos && listing.find("C_INDEX_STORE") != std::string::npos, "the cell copy is present: " + listing);
		});
		test("an_injected_miss_at_every_plain_position_keeps_the_interpreter_s_answer", [this]() {
			// miss injection (a build with JAISCRIPT_MISS_INJECTION defined, else the shapes run as plain parity):
			// every nth plain cell instruction misses before it runs, so the flush, the alias and temp liveness, the inner counted
			// loop's state, the call door's landing and the range head's index sync continue on the general form from every position
			const char* body = R"(
				array<int> G = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8];
				array<float> F = [0.5, 1.5, 2.5, 3.5];
				int twice(int v) { return v * 2; }
				string probe() {
					int acc = 0; float facc = 0.0; array<int> out = [];
					int n = G.size();
					for (int i = 0; i < n; i++) {
						var& e = G[i];
						int t = e * 3 + i;
						if (t % 2 == 0) { acc = acc + twice(t); } else { acc = acc - t; }
						for (int j = 0; j < 3; j++) { acc = acc + j * e; }
						e = e + 1;
						out.push(t);
					}
					for (auto v : G) { acc = acc + v; }
					int k = 0; int m = F.size();
					while (k < m) { facc = facc + F[k] * 2.0; F[k] = F[k] + 1.0; k = k + 1; }
					return to_string(acc) + "|" + to_string(facc) + "|" + to_string(out.size()) + "|" + to_string(G[0]) + "|" + to_string(F[3]);
				}
				probe();
			)";
			auto run = [&](bool vm, uint32_t every) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				if (every) { e->cells_miss_every(every); }
				stdlib::register_all(*e); register_host_math_fixture(*e);
				try { return e->execute(body).to_string(); }
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			const std::string oracle = run(false, 0);
			check_true(oracle.rfind("throw:", 0) != 0, "the shapes run: " + oracle);
			check_eq(oracle, run(true, 0), "the vm without the lever");
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u, 13u }) {
				check_eq(oracle, run(true, every), "an injected miss at every " + std::to_string(every) + " plain cell instructions");
			}
		});
		test("a_var_local_fixed_to_int_by_a_typed_use_keeps_its_float", [this]() {
			// Regression: a var local declared in the body from a float constant, incremented by the speculative typed row: the cell's
			// kind was fixed to int by the increment and the landing took that for the declaration's kind (a span truncated the
			// constant, a loop's pinned arithmetic read the float's bits). The declaration's own kind judges the first write, the
			// cell's contract refuses any other kind at the landing, and a constant seeds the cell's kind at admission
			const char* script =
				"function span_shape() -> string { var f = 1.5; ++f; f--; ++f; return to_string(f); }\n"
				"function loop_shape() -> float { float acc = 0.0; for (int i = 0; i < 3; i++) { var f = 1.5; ++f; f--; ++f; acc = acc + f; } return acc; }\n"
				"function any_shape() -> float { float acc = 0.0; for (int i = 0; i < 3; i++) { var f = 1.5; f = f * 1.5; ++f; acc = acc + f; } return acc; }\n"
				"function from_slot(var g) -> float { float acc = 0.0; for (int i = 0; i < 3; i++) { var f = g; ++f; acc = acc + f; } return acc; }\n"
				"function int_var() -> int { int acc = 0; for (int i = 0; i < 40; i++) { var n = 0; n++; ++n; acc = acc + n; } return acc; }\n"
				"span_shape() + \"|\" + to_string(loop_shape()) + \"|\" + to_string(any_shape()) + \"|\" + to_string(from_slot(2.5)) + \"|\" + to_string(from_slot(2)) + \"|\" + to_string(int_var());";
			both(script, "2.5|7.5|9.75|10.5|9.0|80", "a var local under a typed use");
			both(script, "", "a var local under a typed use, proofs off", false);
			const std::string l = listing_of("var f = function() -> int { int acc = 0; for (int i = 0; i < 40; i++) { var n = 0; n++; ++n; acc = acc + n; } return acc; };");
			check_true(l.find("-- cell loop") != std::string::npos, "an int var under the typed rows still closes: " + l);
		});
		test("two_environment_arguments_of_one_call_push_their_own_values", [this]() {
			// Regression: a call with two global arguments inside a block ran both general loads at the copy's ip, so they shared one
			// lookup cache slot and the second argument received the first's value (JaiDOOM's draw_weapon once its body was a span);
			// the loop shape and the span shape, both backends, and the values through a third callee that writes the globals first
			const char* script =
				"var PX = 0.0; var PY = 0.0; var W = 320; int HITS = 0;\n"
				"function place(int k, float s) -> void { PX = W / 2.0 + k * s; PY = W / 4.0 - k * s; }\n"
				"function take(int k, float x, float y, float s) -> void { HITS = HITS + math::itrunc(x * 10.0) + math::itrunc(y * 1000.0) + k; }\n"
				"function span_shape(int k) -> int { int lump = k + 1; if (lump < 0) { return -1; } float scale = W / 320.0; place(lump, scale); take(lump, PX, PY, scale); return HITS; }\n"
				"function loop_shape(int n) -> int { int acc = 0; for (int i = 0; i < n; i++) { place(i, 0.5); take(i, PX, PY, 0.5); acc = acc + HITS; } return acc; }\n"
				"to_string(span_shape(3)) + \"|\" + to_string(span_shape(7)) + \"|\" + to_string(loop_shape(4));";
			both(script, "", "two environment arguments");
			both(script, "", "two environment arguments, proofs off", false);
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
		});
		test("a_call_the_expression_consumes_lands_in_a_temp_and_the_loop_closes", [this]() {
			// `if (side(x, y, n) == 0)`, `acc + twice(i) * 2`, `while (dec(k) > 0)`: the call's result lands in a compiler temp
			// (op_call_temp) when its partner is a literal or a private local, the consumer reads a slot, the door lands it in a temp
			// cell; an environment partner keeps the stack form (the general read must follow the call); a string result and a
			// reference result land on the general form; an error inside the callee keeps its text
			const std::string prelude =
				"array<float> NX = [0.0, 1.0, 2.0]; array<float> NY = [0.0, 1.0, 0.0]; array<int> RIGHT = [1, 2, -1]; array<int> LEFT = [2, -1, -1]; int G = 5;\n"
				"int side(float x, float y, int n) { float dx = x - NX[n]; float dy = y - NY[n]; if (dx * 2.0 > dy) { return 1; } return 0; }\n"
				"int twice(int v) { return v * 2; }\n"
				"int dec(int v) { return v - 1; }\n"
				"string name(int i) { if (i % 2 == 0) { return \"a\"; } return \"b\"; }\n"
				"int ovf(int v) { return v * 3; }\n"
				"var walk = function(float x, float y) { int n = 0; int steps = 0; while (n >= 0 && steps < 8) { if (side(x, y, n) == 0) { n = RIGHT[n]; } else { n = LEFT[n]; } steps = steps + 1; } return steps * 10 + n; };\n"
				"var sum = function(int k) { int acc = 0; for (int i = 0; i < k; i++) { acc = acc + twice(i); } return acc; };\n"
				"var down = function(int k) { int c = 0; while (dec(k) > 0) { k = k - 1; c = c + 1; } return c; };\n"
				"var envpartner = function(int k) { int s = 0; for (int i = 0; i < k; i++) { s = s + (G + twice(i)); } return s; };\n"
				"var names = function(int k) { int c = 0; for (int i = 0; i < k; i++) { if (name(i) == \"b\") { c = c + 1; } } return c; };\n"
				"var blows = function(int k) { int c = 0; string m = \"\"; try { for (int i = 0; i < k; i++) { if (ovf(4611686018427387904) > 0) { c = c + 1; } } } catch (err) { m = err; } return m; };\n";
			const std::string body =
				"string run() { return to_string(walk(0.5, 0.2)) + \"|\" + to_string(sum(6)) + \"|\" + to_string(down(5)) + \"|\" + to_string(envpartner(4)) + \"|\" + to_string(names(7)) + \"|\" + blows(2); }\n"
				"run();";
			both(prelude + body, "", "calls as operands");
			both(prelude + body, "", "calls as operands, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0 && vm.find("Integer overflow") != std::string::npos, "the shapes ran: " + vm);
			const std::string l = listing_of(prelude);
			auto count = [&](const char* needle) { size_t n = 0; for (size_t at = l.find(needle); at != std::string::npos; at = l.find(needle, at + 1)) { ++n; } return n; };
			// side, twice and dec are leaves and graft; a call the graft does not take lands in a temp
			check_true(count("CALL_TEMP") + count("-- inlined ") >= 4, "the calls the expressions consume land in temps or graft: " + std::to_string(count("CALL_TEMP")) + " + " + std::to_string(count("-- inlined ")) + "\n" + l);
			check_true(count("C_PROBE") + count("-- inlined ") >= 3, "the loops around them close with the door or the graft: " + std::to_string(count("C_PROBE")) + "\n" + l);
			const std::string envl = listing_of("int G = 5; int twice(int v) { return v * 2; } var envpartner = function(int k) { int s = 0; for (int i = 0; i < k; i++) { s = s + (G + twice(i)); } return s; };");
			check_true(envl.find("CALL_TEMP") == std::string::npos, "an environment partner keeps the general order: " + envl);
			const std::string refl = listing_of("function mutate(int& v) -> int { v = 20; return 2; } var g = function(int n) { int acc = 0; for (int i = 0; i < n; i++) { int x = 3; acc = acc + (x + mutate(x)); } return acc; };");
			check_true(refl.find("CALL_TEMP") == std::string::npos, "a partner the call names as an argument keeps the stack form (the binary reads x before the call writes it): " + refl);
			if (!engine::miss_injection_available()) { return; }
			auto injected = [&](uint32_t every) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				try { return e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) { check_eq(vm, injected(every), "calls as operands under an injected miss at every " + std::to_string(every)); }
		});
		test("a_string_call_result_lands_in_its_temp_and_concatenates", [this]() {
			// blit_half's shape (`out += vid_row_half(cy) + "\n"`): a string result lands in its compiler temp, whose slot holds it as
			// op_call_temp leaves it, and string + string lands there too, a to_string partner included; the landing still misses for a
			// reference result and under the memory cap (the general text); a later miss reads
			// the temp's slot; a no-result store lands the string by the store's rule; both backends, proofs on and off, and under an
			// injected miss
			const std::string prelude =
				"array<int> PIX = [0, 1, 2, 3, 3, 2, 1, 0]; array<string> PAL = [\"a\", \"b\", \"c\", \"d\"]; string G = \"g\";\n"
				"function row(int cy) -> string { string r = \"\"; for (int x = 0; x < 4; ++x) { r += PAL[PIX[cy * 4 + x]]; } return r + \"|\"; }\n"
				"function sep(int i) -> string { if (i % 2 == 0) { return \",\"; } return \";\"; }\n"
				"function pick(int i) -> string& { return G; }\n"
				"function big(int i) -> string { return \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"; }\n"
				"var blit = function(int n) { string out = \"\"; for (int cy = 0; cy < n; ++cy) { out += row(cy % 2) + \"\\n\"; } return out; };\n"
				"var lead = function(int n) { string out = \"\"; for (int i = 0; i < n; ++i) { out += \"[\" + sep(i); } return out; };\n"
				"var counted = function(int n) { int acc = 0; string out = \"\"; for (int i = 0; i < n; ++i) { acc = acc + i; out += sep(i) + \".\"; } return to_string(acc) + out; };\n"
				"var intpartner = function(int n) { string out = \"\"; for (int i = 0; i < n; ++i) { out += sep(i) + to_string(5); } return out; };\n"
				"var refs = function(int n) { string out = \"\"; for (int i = 0; i < n; ++i) { out += pick(i) + \".\"; if (i == 1) { G = \"h\"; } } return out; };\n"
				"var capped = function(int n) { string out = \"\"; try { for (int i = 0; i < n; ++i) { out += big(i) + \"\\n\"; } } catch (e) { return \"caught|\" + e; } return \"no cap\"; };\n";
			const std::string body =
				"string run() { return blit(4) + \"#\" + lead(3) + \"#\" + counted(4) + \"#\" + intpartner(3) + \"#\" + refs(4) + \"#\" + G; }\n"
				"run();";
			both(prelude + body, "abcd|\ndcba|\nabcd|\ndcba|\n#[,[;[,#6,.;.,.;.#,5;5,5#g.g.h.h.#h", "string results");
			both(prelude + body, "", "string results, proofs off", false);
			// a no-result store takes a string where it converts nothing: a string local as it is, a var local (one holding an int before, one
			// the body declares) under the var's stamp; an int local misses onto the general store, which raises its text
			const std::string stores = "[[warn_mode]]\n" + prelude +
				"function anyv(int i) { if (i == 2) { return \"s\"; } return i; }\n"
				"var stored = function(int n) { string s = \"\"; var v = 0; int acc = 0; for (int i = 0; i < n; ++i) { s = sep(i); v = anyv(i); acc = acc + i; } return s + \"|\" + to_string(v) + \"|\" + type_of(v) + \"|\" + to_string(acc); };\n"
				"var last = function(int n) { var v = 0; for (int i = 0; i < n; ++i) { v = anyv(i); } return to_string(v) + \"|\" + type_of(v); };\n"
				"var body_decl = function(int n) { string out = \"\"; for (int i = 0; i < n; ++i) { var t = 0; t = sep(i); out += t; } return out; };\n"
				"var typed_int = function(int n) { int k = 0; try { for (int i = 0; i < n; ++i) { k = anyv(i); } } catch (e) { return to_string(k) + \"|\" + e; } return to_string(k); };\n"
				"string runs() { return stored(4) + \"#\" + last(3) + \"#\" + body_decl(3) + \"#\" + typed_int(4); }\n"
				"runs();";
			both(stores, "", "string stores");
			both(stores, "", "string stores, proofs off", false);
			const std::string sv = run_on(true, stores);
			check_true(sv.rfind(";|3|int|6#s|string#,;,#1|", 0) == 0 && sv.size() > 25, "the stores ran and the int local raised: " + sv);
			auto capped = [&](bool vm) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				e->memory_cap(65536);
				try { return e->execute(prelude + "capped(100000);").as_string(); }
				catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			const std::string ci = capped(false), cv = capped(true);
			check_eq(ci, cv, "the memory cap inside the loop: both backends");
			check(ci.rfind("caught|", 0) == 0, "the cap raised inside the loop and was caught: " + ci);
			const std::string l = listing_of(prelude);
			const size_t fb = l.find("C_LAND");
			check_true(fb != std::string::npos && l.find("C_ARITH_ANY", fb) != std::string::npos && l.find("C_APPEND", fb) != std::string::npos, "the loops close with the landing, the concatenation and the append: " + l);
			if (!engine::miss_injection_available()) { return; }
			const std::string vm = run_on(true, prelude + body);
			auto injected = [&](const std::string& src, uint32_t every) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				try { return e->execute(src).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				check_eq(vm, injected(prelude + body, every), "string results under an injected miss at every " + std::to_string(every));
				check_eq(sv, injected(stores, every), "string stores under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_member_read_of_a_frame_slot_instance_is_a_block_site", [this]() {
			// `p.x` inside a loop and inside a span is a load site: the field resolved once at entry, C_MLOAD lands its kind; a
			// string field, a user accessor and a null base keep the general form (the entry refuses); a callee that rewrites the field
			// between reads is seen (the hold check after the call re-resolves); both backends, proofs on and off
			const std::string prelude =
				"class P { int x = 3; float y = 1.5; bool on = true; string name = \"p\"; int _get_z() { return x * 100; } }\n"
				"int bump(P& p) { p.x = p.x + 1; return p.x; }\n"
				"var sum = function(P p, int n) { int acc = 0; for (int i = 0; i < n; i++) { acc = acc + p.x * i; if (p.on) { acc = acc + 1; } } return acc; };\n"
				"var mix = function(P p) { float a = p.y * 2.0; int b = p.x + 4; float c = a + b; int d = b * 3; float e = c - d; int f = d + 1; float g = e + f; return g; };\n"
				"var mix2 = function(P p, int n) { float acc = 0.0; for (int i = 0; i < n; i++) { acc = acc + p.y * 2.0 + p.x * 3; } return acc; };\n"
				"var strs = function(P p, int n) { int acc = 0; for (int i = 0; i < n; i++) { acc = acc + p.name.size(); } return acc; };\n"
				"var acc = function(P p, int n) { int t = 0; for (int i = 0; i < n; i++) { t = t + p.z; } return t; };\n"
				"var rewritten = function(P p, int n) { int t = 0; for (int i = 0; i < n; i++) { t = t + p.x; bump(p); } return t; };\n";
			const std::string body =
				"string run() { P p = P(); P q = P(); q.x = 10;\n"
				"  return to_string(sum(p, 5)) + \"|\" + to_string(mix(q)) + \"|\" + to_string(mix2(q, 4)) + \"|\" + to_string(strs(p, 3)) + \"|\" + to_string(acc(p, 2)) + \"|\" + to_string(rewritten(q, 4)) + \"|\" + to_string(q.x); }\n"
				"run();";
			both(prelude + body, "", "member read sites");
			both(prelude + body, "", "member read sites, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
			const std::string l = listing_of(prelude);
			auto count = [&](const char* needle) { size_t n = 0; for (size_t at = l.find(needle); at != std::string::npos; at = l.find(needle, at + 1)) { ++n; } return n; };
			check_true(count("C_MLOAD") >= 2, "the member reads are sites: " + std::to_string(count("C_MLOAD")) + "\n" + l);
			check_true(l.find("-- cell loop") != std::string::npos, "the loop over p.x closes: " + l);
			if (!engine::miss_injection_available()) { return; }
			auto injected = [&](uint32_t every) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				try { return e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) { check_eq(vm, injected(every), "member read sites under an injected miss at every " + std::to_string(every)); }
		});
		test("a_member_read_through_an_element_alias_is_resolved_per_rebind", [this]() {
			// `for (auto& p : pool) { p.x }` over a hetero list of instances and over a class-typed buffer, `var& e = list[i]; e.x`,
			// a subclass element with the same field (the per-site cache re-probes on a class change), a callee that replaces the element
			// between reads (the hold check re-applies the rebind), a string field (the general form); both backends, proofs on and off
			const std::string prelude =
				"class P { int x = 3; float y = 1.5; bool on = true; string name = \"p\"; }\n"
				"class D : P { int extra = 7; D(int v) { x = v; on = false; } }\n"
				"var LIST = [P(), P(), D(10), P()];\n"
				"array<P> BUF = [P(), P(), P()];\n"
				"int swap(int i) { LIST[i] = D(100); return 1; }\n"
				"var sum = function(int n) { int acc = 0; for (int r = 0; r < n; r++) { for (auto& p : LIST) { acc = acc + p.x * 2; if (p.x > 5) { acc = acc + 1; } } } return acc; };\n"
				"var fsum = function() { float acc = 0.0; for (auto& p : BUF) { acc = acc + p.y * 2.0; } return acc; };\n"
				"var byidx = function() { int acc = 0; int n = LIST.size(); for (int i = 0; i < n; i++) { var& e = LIST[i]; acc = acc + e.x * 3; } return acc; };\n"
				"var replaced = function() { int acc = 0; int n = LIST.size(); for (int i = 0; i < n; i++) { var& e = LIST[i]; int before = e.x; int s = swap(i); acc = acc + before + s + e.x; } return acc; };\n"
				"var names = function() { int acc = 0; for (auto& p : LIST) { acc = acc + p.name.size(); } return acc; };\n";
			const std::string body =
				"string run() { string a = to_string(sum(3)) + \"|\" + to_string(fsum()) + \"|\" + to_string(byidx()) + \"|\" + to_string(names());\n"
				"  return a + \"|\" + to_string(replaced()) + \"|\" + to_string(byidx()); }\n"
				"run();";
			both(prelude + body, "", "alias member sites");
			both(prelude + body, "", "alias member sites, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
			const std::string l = listing_of(prelude);
			auto count = [&](const char* needle) { size_t n = 0; for (size_t at = l.find(needle); at != std::string::npos; at = l.find(needle, at + 1)) { ++n; } return n; };
			check_true(count("C_MLOAD") >= 5, "the member reads through the aliases are sites: " + std::to_string(count("C_MLOAD")) + "\n" + l);
			check_true(count("-- cell loop") >= 4, "the loops over the aliases close: " + std::to_string(count("-- cell loop")) + "\n" + l);
			if (!engine::miss_injection_available()) { return; }
			auto injected = [&](uint32_t every) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				try { return e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) { check_eq(vm, injected(every), "alias member sites under an injected miss at every " + std::to_string(every)); }
		});
		test("a_member_store_in_statement_position_is_one_op_and_a_block_site", [this]() {
			// `p.x = v;` takes its value as an operand (SET_MEMBER_VALUE, no push, no POP) and a block writes the field in place
			// (C_MSTORE) when the value's kind is the field's kind; an int into a float field, a float into a var field holding an int, a
			// string field and a user setter run the general store (its conversions and texts); the value read back after the loop, both
			// backends, proofs on and off, and under an injected miss
			const std::string prelude =
				"class P { int x = 0; float y = 0.0; bool on = false; var w = 1; string name = \"p\"; int _set_z(int v) { x = v * 100; return v; } int z = 0; }\n"
				"class T { float posx = 0.0; float posy = 0.0; int hits = 0; }\n"
				"var WS = [T(), T(), T()];\n"
				"var fill = function(P p, int n) { for (int i = 0; i < n; i++) { p.x = i; p.y = i * 0.5; p.on = i > 2; p.w = i; } return p.x * 1000 + p.w * 10 + (p.on ? 1 : 0); };\n"
				"var conv = function(P p, int n) { for (int i = 0; i < n; i++) { p.y = i; p.w = i * 0.5; p.x = p.x + 2; } return to_string(p.y) + \"|\" + to_string(p.w) + \"|\" + to_string(p.x); };\n"
				"var sweep = function(float px, float py) { int t = 0; for (auto& w : WS) { w.posx = px; w.posy = py + w.hits; w.hits = w.hits + 1; t = t + w.hits; } return t; };\n"
				"var strs = function(P p, int n) { for (int i = 0; i < n; i++) { p.name = \"n\" + to_string(i); } return p.name; };\n"
				"var setter = function(P p, int n) { for (int i = 0; i < n; i++) { p.z = i; } return p.x; };\n";
			const std::string body =
				"string run() { P a = P(); P b = P(); P c = P(); P d = P(); string out = to_string(fill(a, 5)) + \"|\" + conv(b, 3) + \"|\" + to_string(sweep(1.5, 2.0)) + \"|\" + to_string(sweep(0.5, 0.0));\n"
				"  return out + \"|\" + strs(c, 3) + \"|\" + to_string(setter(d, 4)) + \"|\" + to_string(WS[1].posy) + \"|\" + to_string(WS[2].hits); }\n"
				"run();";
			both(prelude + body, "", "member stores");
			both(prelude + body, "", "member stores, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
			const std::string l = listing_of(prelude);
			auto count = [&](const char* needle) { size_t n = 0; for (size_t at = l.find(needle); at != std::string::npos; at = l.find(needle, at + 1)) { ++n; } return n; };
			check_true(count("SET_MEMBER_VALUE") >= 8, "the statement stores take the value as an operand: " + std::to_string(count("SET_MEMBER_VALUE")) + "\n" + l);
			check_true(count("C_MSTORE") >= 6, "the stores are block sites: " + std::to_string(count("C_MSTORE")) + "\n" + l);
			if (!engine::miss_injection_available()) { return; }
			auto injected = [&](uint32_t every) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				try { return e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) { check_eq(vm, injected(every), "member stores under an injected miss at every " + std::to_string(every)); }
		});
		test("an_element_alias_passed_to_a_callee_passes_its_reference_and_the_loop_stays_closed", [this]() {
			// `for (auto& p : pool) { step(p); }` with `step(var& p)` writing the element's fields: the alias is a call argument, the door
			// pushes the element reference the general LOAD pushes, the callee's writes land in the element, the loop closes and runs without a
			// miss (the injected-miss controls cover the door); a by-value callee copies as on the general form; a body-declared alias too
			const std::string prelude =
				"class P { int kind = 1; float x = 0.0; int life = 5; }\n"
				"function step(var& p) -> void { if (p.kind == 0) { return; } int life = p.life - 1; if (life <= 0) { p.kind = 0; p.life = 0; return; } p.x = p.x + 1.5; p.life = life; }\n"
				"function peek(P p) -> int { p.life = 100; return p.life + p.kind; }\n"
				"array<P> POOL = [new P(), new P(), new P(), new P()];\n"
				"var LIST = [P(), P(), P()];\n"
				"var run_steps = function(int n) { for (int t = 0; t < n; t++) { for (auto& p : POOL) { step(p); } } float s = 0.0; for (auto& p : POOL) { s = s + p.x + p.life + p.kind; } return s; };\n"
				"var peeks = function() { int s = 0; for (auto& p : LIST) { s = s + peek(p); } int u = 0; for (auto& p : LIST) { u = u + p.life; } return s * 1000 + u; };\n"
				"var byidx = function() { float s = 0.0; int n = LIST.size(); for (int i = 0; i < n; i++) { var& e = LIST[i]; step(e); s = s + e.x; } return s; };\n";
			const std::string body = "string run() { return to_string(run_steps(7)) + \"|\" + to_string(peeks()) + \"|\" + to_string(byidx()); }\nrun();";
			both(prelude + body, "", "alias arguments");
			both(prelude + body, "", "alias arguments, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
			const std::string l = listing_of(prelude);
			auto count = [&](const char* needle) { size_t n = 0; for (size_t at = l.find(needle); at != std::string::npos; at = l.find(needle, at + 1)) { ++n; } return n; };
			check_true(count("C_PROBE") + count("-- inlined ") >= 3, "the loops with the calls close through the door or take the leaf inline: " + std::to_string(count("C_PROBE")) + "\n" + l);
			if (!engine::miss_injection_available()) { return; }
			auto injected = [&](uint32_t every) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				try { return e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) { check_eq(vm, injected(every), "alias arguments under an injected miss at every " + std::to_string(every)); }
		});
		test("adjacent_plain_cell_pairs_run_as_one_dispatch", [this]() {
			// arithmetic chains, compare chains and element load pairs in a block are fused pairs in the listing; the values match the
			// interpreter with the middle end's switch on and off, under the injected miss at every plain position (both halves count), with
			// an overflow inside a pair's second half (the general text), and with a jump landing on a pair's second instruction
			const std::string prelude =
				"array<int> A = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3]; array<int> B = [2, 7, 1, 8, 2, 8, 1, 8, 2, 8, 4, 5, 9, 0, 4, 5];\n"
				"var chains = function(int n) { int acc = 0; for (int i = 0; i < n; i++) { int a = i * 3 * 5; int b = a * 2 ^ i; int c = (b & 1023) * 7; acc = acc ^ c * 3 * a; } return acc; };\n"
				"var compares = function(int n) { int hits = 0; for (int i = 0; i < n; i++) { int p0 = A[i]; int p1 = B[i]; int p2 = A[15 - i]; int p3 = B[15 - i]; if (p0 == p1 && p0 == p2 && p0 == p3) { hits = hits + 100; continue; } if (p0 == p2 && p1 == p3) { hits = hits + 10; continue; } if (p0 > p1 && p2 > p3) { hits = hits + 1; } } return hits; };\n"
				"var loads = function(int n) { int s = 0; for (int i = 0; i < n; i++) { int x = A[i]; int y = B[i]; int z = A[(i + 1) & 15]; int w = B[(i + 2) & 15]; s = s + x * y - z + w; } return s; };\n"
				"var landing = function(int n) { int s = 0; for (int i = 0; i < n; i++) { if (i > 7) { s = s + 1; } s = s + i * 2; s = s - 1; } return s; };\n"
				"var blows = function(int n) { int s = 0; string m = \"\"; try { for (int i = 0; i < n; i++) { int a = 4611686018427387900 + i; int b = a * 2 + a; s = s + b; } } catch (err) { m = err; } return m; };\n";
			const std::string body = "string run() { return to_string(chains(16)) + \"|\" + to_string(compares(16)) + \"|\" + to_string(loads(16)) + \"|\" + to_string(landing(16)) + \"|\" + blows(3); }\nrun();";
			both(prelude + body, "", "fused pairs");
			both(prelude + body, "", "fused pairs, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0 && vm.find("Integer overflow") != std::string::npos, "the shapes ran: " + vm);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			const std::string l = listing_of(prelude);
			auto count = [&](const char* needle) { size_t n = 0; for (size_t at = l.find(needle); at != std::string::npos; at = l.find(needle, at + 1)) { ++n; } return n; };
			check_true(count("C_ARITH_P2") >= 2, "the arithmetic chains pair: " + std::to_string(count("C_ARITH_P2")) + "\n" + l);
			check_true(count("C_JCMP_P2") >= 2, "the compare chains pair: " + std::to_string(count("C_JCMP_P2")) + "\n" + l);
			check_true(count("C_LOAD2") >= 1, "the element loads pair: " + std::to_string(count("C_LOAD2")) + "\n" + l);
			if (!engine::miss_injection_available()) { return; }
			auto injected = [&](uint32_t every) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				try { return e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) { check_eq(vm, injected(every), "fused pairs under an injected miss at every " + std::to_string(every)); }
		});
		test("pinned_arithmetic_and_moves_run_by_kind", [this]() {
			// float and int add, subtract and multiply over pinned cells and constants, and moves between cells of one pinned kind or from a
			// constant of it, run as their own cell instructions; an int overflow inside one misses onto the general form's text; both backends,
			// proofs off (the checked forms), the middle end's switch off and the injected miss at every plain position
			const std::string prelude =
				"var floats = function(int n) { float x = 0.5; float y = 1.25; float acc = 0.0; for (int i = 0; i < n; i++) { float a = x * y + 0.75; float b = a - x * 2.0; float c = b * b - a; x = y; y = c + 0.125; acc = acc + a * 0.5 - b; } return acc; };\n"
				"var ints = function(int n) { int s = 3; int t = 5; int acc = 0; for (int i = 0; i < n; i++) { int a = s + t; int b = a - i; int c = b + 11; s = t; t = c - 7; acc = acc + a - b + c; } return acc; };\n"
				"var moves = function(int n) { int k = 0; float f = 0.0; bool on = false; int acc = 0; for (int i = 0; i < n; i++) { int j = i; float g = f; k = 7; f = 2.5; on = true; if (on) { acc = acc + j + k; } f = g + f; } return to_string(acc) + \"/\" + to_string(f); };\n"
				"var adds = function(int n) { int s = 0; string m = \"\"; try { for (int i = 0; i < n; i++) { int a = 9223372036854775800 + i; [[warn_mode]] s = a + 3; } } catch (err) { m = err; } return m; };\n"
				"var subs = function(int n) { int s = 0; string m = \"\"; try { for (int i = 0; i < n; i++) { int a = -9223372036854775800 - i; [[warn_mode]] s = a - 3; } } catch (err) { m = err; } return m; };\n"
				"var reads = function(int n) { array<float> xs = [0.5, 1.5, -2.0, 3.25, 0.0, 7.5]; array<int> ys = [4, -1, 9]; float acc = 0.0; int hits = 0; for (int i = 0; i < n; i++) { int j = i % 6; float v = xs[j]; int q = j % 3; float w = ys[q]; if (v < 1.0) { acc = acc + v; } if (v >= acc) { hits = hits + 1; } if (i > 3) { acc = acc * 0.5 + w; } } return to_string(acc) + \"/\" + to_string(hits); };\n";
			const std::string body = "string run() { return to_string(floats(24)) + \"|\" + to_string(ints(24)) + \"|\" + moves(24) + \"|\" + adds(9) + \"|\" + subs(9) + \"|\" + reads(30); }\nrun();";
			both(prelude + body, "", "pinned by kind");
			both(prelude + body, "", "pinned by kind, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0 && vm.find("Integer overflow") != std::string::npos, "the shapes ran: " + vm);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			const std::string l = listing_of(prelude);
			for (const char* op : { "C_ADD_FF", "C_SUB_FF", "C_MUL_FF", "C_ADD_FFK", "C_MUL_FFK", "C_ADD_II", "C_SUB_II", "C_ADD_IIK", "C_SUB_IIK", "C_MOV_P", "C_MOV_K" }) {
				check_true(l.find(std::string(op) + " ") != std::string::npos, std::string("the listing names ") + op + "\n" + l);
			}
			for (const char* op : { "C_LOAD_F ", "C_JLT_FFK ", "C_JGE_FF ", "C_JGT_IIK " }) { check_true(l.find(op) != std::string::npos, std::string("the listing names ") + op + "\n" + l); }
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "pinned by kind under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_value_moved_into_a_pinned_cell_lands_there_at_its_definition", [this]() {
			// a math result, an arithmetic result and a move's result that a move copies into a local or a graft value of the same pinned kind
			// land in the destination directly when nothing reads them after the move; a graft's return onto its join runs nothing; the value
			// read again later, a move inside a branch arm and a value a later iteration reads keep their moves; both backends, proofs off, the
			// middle end off and the injected miss at every plain position
			const std::string prelude =
				"float wave(float x) { float s = math::sin(x); float c = math::cos(x); return s * c + 0.5; }\n"
				"float mix(float a, float b, float t) { float d = b - a; return a + d * t; }\n"
				"var run = function(int n) { float acc = 0.0; float x = 0.1; float keep = 0.0; for (int i = 0; i < n; i++) { float w = wave(x); float m = mix(w, x, 0.25); float y = m * 2.0; float z = math::sqrt(y * y + 1.0); if (i > 3) { keep = z; } else { keep = keep + 1.0; } acc = acc + y + z * keep; x = x + 0.37; } return acc; };\n"
				"var carried = function(int n) { float p = 1.0; float q = 0.0; for (int i = 0; i < n; i++) { float r = math::cos(p); q = q + r; p = r + 0.5; } return q + p; };\n";
			const std::string body = "string go() { return to_string(run(20)) + \"|\" + to_string(carried(20)); }\ngo();";
			both(prelude + body, "", "moves at their definitions");
			both(prelude + body, "", "moves at their definitions, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			const std::string l = listing_run(prelude + body);
			auto count = [&](const char* needle) { size_t n = 0; for (size_t at = l.find(needle); at != std::string::npos; at = l.find(needle, at + 1)) { ++n; } return n; };
			check_true(count("C_SIN_F ") >= 1 && count("C_COS_F ") >= 2 && count("C_SQRT_F ") >= 1, "the maths run in the blocks as float intrinsics\n" + l);
			{
				// no math result is moved by the instruction after it: the math lands in the move's destination
				std::vector<std::string> lines; for (size_t at = 0, nl; at < l.size(); at = nl + 1) { nl = l.find('\n', at); if (nl == std::string::npos) { nl = l.size(); } lines.push_back(l.substr(at, nl - at)); }
				auto operands = [](const std::string& line, const char* op, uint64_t& a, uint64_t& b) {
					const size_t at = line.find(op); if (at == std::string::npos) return false;
					return std::sscanf(line.c_str() + at + std::strlen(op), " %llu %llu", reinterpret_cast<unsigned long long*>(&a), reinterpret_cast<unsigned long long*>(&b)) == 2;
				};
				size_t moved = 0;
				for (size_t i = 0; i + 1 < lines.size(); ++i) {
					uint64_t ma = 0, mb = 0, va = 0, vb = 0;
					if (operands(lines[i], "C_MATH ", ma, mb) && operands(lines[i + 1], "C_MOV_P ", va, vb) && vb == (ma & 0xFFFFFFu)) { ++moved; }
					for (const char* fop : { "C_SIN_F ", "C_COS_F ", "C_SQRT_F " }) { if (operands(lines[i], fop, ma, mb) && operands(lines[i + 1], "C_MOV_P ", va, vb) && vb == ma) { ++moved; } }
				}
				check_eq(static_cast<size_t>(0), moved, "the math results land in their locals\n" + l);
			}
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u, 13u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "moves at their definitions under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_program_scope_arrays_size_heads_a_block", [this]() {
			// `m < XS.size()` over a program-scope array: the size lands in a temp through the environment lookup and the loop closes over it
			// (the size read live each iteration: a callee's push grows it, a reassignment of the global re-resolves after the call); both
			// backends, proofs off, the middle end off and miss injection
			const std::string prelude =
				"array<int> XS = [3, 1, 4, 1, 5, 9, 2, 6];\n"
				"array<float> FS = [0.5, 1.25, 2.0];\n"
				"int grown = 0;\n"
				"void grow() { if (grown < 3) { XS.push(grown * 7); grown = grown + 1; } }\n"
				"void swap_in() { if (grown == 3) { XS = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120]; grown = 4; } }\n"
				"int sum_all() { int s = 0; for (int m = 0; m < XS.size(); ++m) { s = s + XS[m] * (m + 1); } return s; }\n"
				"int sum_grow() { int s = 0; for (int m = 0; m < XS.size(); ++m) { if (m == 4) { grow(); } if (m == 9) { swap_in(); } s = s + XS[m]; } return s; }\n"
				"float fsum() { float s = 0.0; int n = 0; while (n < FS.size()) { s = s + FS[n] * 2.0; n = n + 1; } return s; }\n";
			const std::string body = "string go() { string r = to_string(sum_all()) + \"|\" + to_string(fsum()); r = r + \"|\" + to_string(sum_grow()); r = r + \"|\" + to_string(sum_grow()) + \"|\" + to_string(XS.size()); return r; }\ngo();";
			both(prelude + body, "", "a program-scope size in the head");
			both(prelude + body, "", "a program-scope size in the head, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			const std::string l = listing_run(prelude + body);
			check_true(l.find("C_SIZE") != std::string::npos && l.find("SIZE_TEMP") != std::string::npos, "the size lands in a temp and the block reads it\n" + l);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "a program-scope size under an injected miss at every " + std::to_string(every));
			}
		});
		test("logical_chains_as_values_run_in_blocks", [this]() {
			// a && / || chain whose value is used (a graft's return, a declaration, a no-result store, the condition after it): the comparisons
			// land in a value cell and the chain's consumer reads it; a miss at the consumer re-runs the chain from its first instruction; both
			// backends, proofs off, the middle end off and miss injection
			const std::string prelude =
				"array<int> W_W = [40, 8];\n"
				"array<int> W_H = [30, 8];\n"
				"bool in_world(int w, int x, int y) { return x >= 0 && y >= 0 && x < W_W[w] && y < W_H[w]; }\n"
				"int count_in(int w) { int n = 0; for (int y = -2; y < 33; ++y) { for (int x = -2; x < 43; ++x) { if (in_world(w, x, y)) { n = n + 1; } } } return n; }\n"
				"int either(int k) { int n = 0; for (int i = 0; i < k; ++i) { bool b = i % 3 == 0 || i % 5 == 0; if (b) { n = n + 1; } } return n; }\n"
				"int stored(int k) { int n = 0; bool ok = false; for (int i = 0; i < k; ++i) { ok = i > 2 && i < 40 && i % 7 != 0; if (ok) { n = n + i; } } return n; }\n"
				"int mixed(int k) { int n = 0; bool flag = true; for (int i = 0; i < k; ++i) { bool b = flag && i % 4 == 1; bool c = (i < 10 && i > 2) || i == 50; if (b) { n = n + 1; } if (c) { n = n + 100; } flag = !flag; } return n; }\n";
			const std::string body = "string go() { return to_string(count_in(0)) + \"|\" + to_string(count_in(1)) + \"|\" + to_string(either(100)) + \"|\" + to_string(stored(60)) + \"|\" + to_string(mixed(64)); }\ngo();";
			both(prelude + body, "", "logical chains as values");
			both(prelude + body, "", "logical chains as values, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			const std::string l = listing_run(prelude + body);
			check_true(l.find("cmpval") != std::string::npos, "a chain's comparison lands in a value cell\n" + l);
			for (const char* fn : { "== count_in", "== either", "== stored" }) {
				const size_t at = l.find(fn);
				const size_t end = at == std::string::npos ? at : l.find("\n== ", at + 1);
				check_true(at != std::string::npos && l.substr(at, end == std::string::npos ? std::string::npos : end - at).find("-- open loop") == std::string::npos, std::string(fn) + "'s loops close\n" + l);
			}
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u, 13u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "logical chains as values under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_call_the_door_cannot_land_bails_its_iteration", [this]() {
			// a kept call whose value feeds something the block cannot hold (a string declaration through an index) inside a conditional span:
			// the block bails there and the general form takes the rest of the iteration, so the loop still closes over the work before it; both
			// backends, proofs off and miss injection
			const std::string prelude =
				"array<string> NAMES = [\"ant\", \"bee\", \"cat\", \"dog\"];\n"
				"class Tag { int v = 0; string n = \"\"; Tag(int a) { v = a; } }\n"
				"array<Tag> TAGS = [Tag(5), Tag(6), Tag(7), Tag(8)];\n"
				"int pick(int i) { string t = \"x\"; if (i > 50) { return 3; } return i % 4 + t.size() - 1; }\n"
				"int scan(int k) { int n = 0; int len = 0; for (int i = 0; i < k; ++i) { int a = i * 3 + 1; int b = a % 7; n = n + a * b; if (b < 3) { continue; } Tag g = TAGS[pick(i)]; if (g.v > 7 && i > 70) { return -n; } len = len + g.v + NAMES[b % 4].size(); } return n * 1000 + len; }\n";
			const std::string body = "to_string(scan(80));";
			both(prelude + body, "", "a door the block cannot land");
			both(prelude + body, "", "a door the block cannot land, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shape ran: " + vm);
			const std::string l = listing_run(prelude + body);
			const size_t at = l.find("== scan ");
			check_true(at != std::string::npos && l.find("-- cell loop", at) != std::string::npos && l.find("C_BAIL", at) != std::string::npos, "scan's loop closes with a bail\n" + l);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "a door the block cannot land under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_calls_element_arguments_from_different_arrays_read_their_own_arrays", [this]() {
			// a kept call inside a block whose arguments read elements of different program-scope arrays: each argument's read resolves its own
			// container (the copy's miss ip is the first argument's, but the container's cache slots are its own instruction's); 2 to 4 such
			// arguments, mixed with locals and literals; both backends, proofs off, the middle end off and miss injection
			const std::string prelude =
				"array<int> A = [0, 1, 0, 1, 0, 1, 0, 1];\n"
				"array<int> B = [3, 5, 7, 9, 11, 13, 2, 4];\n"
				"array<int> C = [8, 6, 4, 2, 9, 7, 5, 3];\n"
				"array<float> D = [0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5];\n"
				"int CNT = 0;\n"
				"void bump() { CNT = CNT + 1; }\n"
				"int two(int w, int x) { bump(); return w * 100 + x; }\n"
				"int three(int w, int x, int y) { bump(); return w * 10000 + x * 100 + y; }\n"
				"float four(int w, float d, int x, int y) { bump(); return w + d * 2.0 + x * 3 + y * 5; }\n"
				"int run2() { int s = 0; for (int m = 0; m < 8; ++m) { int v = two(A[m], B[m]); s = (s * 3 + v) % 1000003; } return s; }\n"
				"int run3() { int s = 0; for (int m = 0; m < 8; ++m) { int v = three(C[m], m, B[m]); s = (s * 7 + v) % 1000003; } return s; }\n"
				"float run4() { float s = 0.0; for (int m = 0; m < 8; ++m) { s = s + four(A[m], D[m], 7, C[(m + 3) % 8]); } return s; }\n"
				"int kept(int w, int x) { switch (w) { case 0: return x; } return w * 100 + x; }\n"
				"int run5() { int s = 0; for (int m = 0; m < 8; ++m) { int v = kept(A[m], B[m]); s = (s * 3 + v) % 1000003; } return s; }\n";
			const std::string body = "to_string(run2()) + \"|\" + to_string(run3()) + \"|\" + to_string(run4()) + \"|\" + to_string(CNT) + \"|\" + to_string(run5());";
			both(prelude + body, "", "element arguments from different arrays");
			both(prelude + body, "", "element arguments from different arrays, proofs off", false);
			check_true(run_on(true, prelude + body).rfind("95060|354612|456.0|24|", 0) == 0, "the values: " + run_on(true, prelude + body));
			const std::string vm = run_on(true, prelude + body);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			const std::string l = listing_run(prelude + body);
			const size_t at = l.find("== run2 ");
			check_true(at != std::string::npos && l.find("-- cell loop", at) != std::string::npos, "run2's loop closes over the graft\n" + l);
			const size_t at5 = l.find("== run5 ");
			check_true(at5 != std::string::npos && l.find("-- cell loop", at5) != std::string::npos && l.find("C_ARGS", at5) != std::string::npos, "run5's loop closes over the kept call\n" + l);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "element arguments under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_calls_result_as_a_branch_lands_and_branches_in_the_block", [this]() {
			// a kept call whose result is a condition (`if (!hostile(m, o)) continue;`, `if (near(i) && ...)`): the result lands in a value cell and
			// the landing branches on its truthiness; a result of another kind misses at the landing and the general jump takes it; both
			// backends, proofs off, the middle end off and miss injection
			const std::string prelude =
				"array<int> FAC = [0, 1, 2, 1, 0, 2, 2, 1];\n"
				"bool hostile(int a, int b) { string why = \"x\"; if (a == b) { return false; } return FAC[a % 8] != FAC[b % 8]; }\n"
				"int weight(int a) { string why = \"y\"; return a % 3; }\n"
				"var odd(int a) { if (a % 5 == 0) { return \"five\"; } return a % 2; }\n"
				"int scan(int m, int k) { int best = -1; int bestd = 1000; for (int o = 0; o < k; ++o) { int d = (o - m) * (o - m); if (d >= bestd || !hostile(m, o)) { continue; } if (weight(o)) { d = d + 1; } best = o; bestd = d; } return best * 1000 + bestd; }\n"
				"int odds(int k) { int n = 0; for (int i = 0; i < k; ++i) { if (odd(i)) { n = n + 1; } } return n; }\n";
			const std::string body = "string go() { return to_string(scan(3, 40)) + \"|\" + to_string(scan(6, 40)) + \"|\" + to_string(odds(30)); }\ngo();";
			both(prelude + body, "", "a call's result as a branch");
			both(prelude + body, "", "a call's result as a branch, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			const std::string l = listing_run(prelude + body);
			const size_t at = l.find("== scan ");
			check_true(at != std::string::npos && l.find("C_JTRUTH", at) != std::string::npos, "scan's block branches on the landed result\n" + l);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "a call's result as a branch under an injected miss at every " + std::to_string(every));
			}
		});
		test("an_enums_values_read_by_name_run_in_blocks", [this]() {
			// `Fac.monster` (an enum value read by name) lands in a temp through an environment cell for the member, read at entry and after every
			// call; both backends, proofs off, the middle end off and miss injection. An enum's map is const, so the call in
			// with_poke writes a global
			const std::string prelude =
				"enum Fac { player, npc, monster, guard }\n"
				"int POKED = 0;\n"
				"array<int> FACS = [0, 2, 3, 2, 1, 2, 0, 3];\n"
				"int count_mon(int k) { int n = 0; for (int i = 0; i < k; ++i) { int f = FACS[i % 8]; if (f == Fac.monster || f == Fac.guard) { n = n + 1; } if (f != Fac.player) { n = n + 100; } } return n; }\n"
				"int helper(int i) { string why = \"z\"; return i % 3; }\n"
				"int with_calls(int k) { int n = 0; for (int i = 0; i < k; ++i) { int f = FACS[i % 8]; if (helper(i) == 1) { n = n + 1000; } if (f == Fac.monster) { n = n + 1; } } return n; }\n"
				"void poke(int i) { string why = \"q\"; if (i == 10) { POKED = 3; } }\n"
				"int with_poke(int k) { int n = 0; for (int i = 0; i < k; ++i) { int f = FACS[i % 8]; poke(i); if (f == Fac.monster) { n = n + 1; } } return n; }\n";
			const std::string body = "to_string(count_mon(40)) + \"|\" + to_string(with_calls(40)) + \"|\" + to_string(with_poke(40)) + \"|\" + to_string(count_mon(40));";
			both(prelude + body, "", "enum values read by name");
			both(prelude + body, "", "enum values read by name, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			const std::string l = listing_run(prelude + body);
			const size_t at = l.find("== count_mon ");
			const size_t end = at == std::string::npos ? at : l.find("\n== ", at + 1);
			check_true(at != std::string::npos && l.substr(at, end == std::string::npos ? std::string::npos : end - at).find("-- cell loop") != std::string::npos, "count_mon's loop closes\n" + l);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "enum values read by name under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_while_true_loop_runs_in_a_block", [this]() {
			// `while (true)`: the constant condition and its jump have no copy, the loop leaves through its breaks and returns (exit statements)
			// and a continue lands on the body's first operation; nested under a counted loop too; both backends, proofs off, the middle end
			// off and miss injection
			const std::string prelude =
				"array<int> A = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3];\n"
				"int walk(int start) { int i = start; int s = 0; while (true) { s = s + A[i % 10] * 3; i = i + 1; if (s > 200) { break; } } return s * 100 + i; }\n"
				"int spin(int n) { int k = 0; int t = 0; while (true) { k = k + 1; t = t + k * 2; if (k >= n) { return t; } } return -1; }\n"
				"int skip3(int n) { int k = 0; int t = 0; while (true) { k = k + 1; if (A[k % 10] == 1) { continue; } t = t + k; if (k >= n) { break; } } return t; }\n"
				"int nest(int m) { int t = 0; for (int j = 0; j < m; ++j) { int k = j; while (true) { k = k + 3; t = t + A[k % 10]; if (k > j + 20) { break; } } } return t; }\n";
			const std::string body = "to_string(walk(2)) + \"|\" + to_string(spin(50)) + \"|\" + to_string(skip3(40)) + \"|\" + to_string(nest(30));";
			both(prelude + body, "", "while (true) loops");
			both(prelude + body, "", "while (true) loops, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			const std::string l = listing_run(prelude + body);
			for (const char* fn : { "walk", "spin", "skip3" }) {
				const size_t at = l.find(std::string("== ") + fn + " ");
				const size_t end = at == std::string::npos ? at : l.find("\n== ", at + 1);
				check_true(at != std::string::npos && l.substr(at, end == std::string::npos ? std::string::npos : end - at).find("-- cell loop") != std::string::npos, std::string(fn) + "'s loop closes\n" + l);
			}
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "while (true) loops under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_compare_headed_loop_is_entered_at_its_head", [this]() {
			// the head is a block entry (CELLS_ENTER): a loop of one iteration runs it in the block, a by-value call's result compares against an
			// enum's value in the block (the call lands first, the read after it), a live-in whose kind the body changes enters, misses at the
			// landing and re-enters at the back edge, and a reference result keeps the stack form; both backends, proofs off, the middle end
			// off and miss injection
			const std::string prelude =
				"enum Fac { player, npc, monster, guard }\n"
				"array<int> FACS = [0, 2, 3, 2, 1, 3, 0, 3];\n"
				"int G = 1;\n"
				"int T = 0;\n"
				"void tick() { T = T + 1; }\n"
				"int fac_of(int i) { tick(); return FACS[i % 8]; }\n"
				"int& gref() { string why = \"r\"; return G; }\n"
				"int bump() { string why = \"b\"; G = G + 100; return 1; }\n"
				"int count(int k) { int n = 0; int i = 0; while (i < k) { if (fac_of(i) != Fac.guard) { n = n + 1; } i = i + 1; } return n; }\n"
				"int once(int k) { int n = 3; int i = 0; while (i < k) { n = n * 7 + i; i = i + 1; } return n; }\n"
				"float widen(int k) { var s = 0; int i = 0; while (i < k) { s = s + 0.5; i = i + 1; } return s; }\n"
				"int refs(int k) { int n = 0; int i = 0; while (i < k) { G = 1; n = n + (gref() - math::abs(bump())); i = i + 1; } return n; }\n";
			const std::string body = "to_string(count(40)) + \"|\" + to_string(once(1)) + \"|\" + to_string(once(0)) + \"|\" + to_string(widen(9)) + \"|\" + to_string(refs(5));";
			both(prelude + body, "25|21|3|4.5|0", "loops entered at their heads");
			both(prelude + body, "25|21|3|4.5|0", "loops entered at their heads, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			const std::string l = listing_run(prelude + body);
			for (const char* fn : { "count", "once" }) {
				const size_t at = l.find(std::string("== ") + fn + " ");
				const size_t end = at == std::string::npos ? at : l.find("\n== ", at + 1);
				const std::string part = at == std::string::npos ? std::string() : l.substr(at, end == std::string::npos ? std::string::npos : end - at);
				check_true(part.find("-- cell loop") != std::string::npos && part.find("CELLS_ENTER") != std::string::npos, std::string(fn) + "'s loop closes with an entry at its head\n" + l);
			}
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "loops entered at their heads under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_counted_loop_is_entered_at_its_cfor_prep", [this]() {
			// CFOR_PREP is the entry once its first compare passes: a one-iteration loop runs in the block, a zero-trip loop enters nothing, a live-in
			// read before the body redefines it, a var live-in that is null at the head (refused there, entered at the back edge), a kind the body
			// widens, a break and a return on the first iteration, a nested zero-trip inner loop, a step variable, and a call and a throw from
			// a call on the first iteration; both backends, proofs off, the middle end off and miss injection
			const std::string prelude =
				"int once(int k) { int n = 3; for (int i = 0; i < k; i++) { n = n * 7 + i; } return n; }\n"
				"int later(int k) { int s = 0; int t = 5; for (int i = 0; i < k; i++) { s = s + t; t = i * 3; } return s; }\n"
				"int vt(int k) { int s = 0; var t; for (int i = 0; i < k; i++) { if (i > 0) { s = s + t; } t = i * 3; } return s; }\n"
				"float widen(int k) { var w = 0; for (int i = 0; i < k; i++) { w = w + 0.5; } return w; }\n"
				"int brk(int k) { int s = 0; for (int i = 0; i < k; i++) { s = s + 10; if (i == 0) { break; } s = s + 1; } return s; }\n"
				"int ret(int k) { int s = 0; for (int i = 0; i < k; i++) { s = s + i; if (i == 0) { return 77 + s; } } return 1; }\n"
				"int nest(int k) { int s = 0; for (int i = 0; i < k; i++) { for (int j = 0; j < i; j++) { s = s + j; } } return s; }\n"
				"int step(int k) { int s = 0; int st = 3; for (int i = 0; i < k; i += st) { s = s + i; } return s; }\n"
				"int sq(int x) { string why = \"c\"; return x * x; }\n"
				"int calls(int k) { int s = 0; for (int i = 0; i < k; i++) { s = s + sq(i); } return s; }\n"
				"int boom(int x) { string why = \"t\"; if (x == 0) { throw \"first\"; } return x; }\n"
				"int thr(int k) { int s = 0; try { for (int i = 0; i < k; i++) { s = s + boom(i); } } catch (e) { s = s + 1000; } return s; }\n";
			const std::string body = "to_string(once(1)) + \"|\" + to_string(once(0)) + \"|\" + to_string(later(4)) + \"|\" + to_string(vt(4)) + \"|\" + to_string(widen(9)) + \"|\" + to_string(brk(5)) + \"|\" + to_string(ret(3)) + \"|\" + to_string(nest(5)) + \"|\" + to_string(step(10)) + \"|\" + to_string(calls(1)) + \"|\" + to_string(calls(4)) + \"|\" + to_string(thr(3));";
			const std::string expected = "21|3|14|9|4.5|10|77|10|18|0|14|1000";
			both(prelude + body, expected, "counted loops entered at their prep");
			both(prelude + body, expected, "counted loops entered at their prep, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); stdlib::register_all(*e);
				e->execute(prelude);
				struct shape { const char* call; uint64_t entries; };   // one entry per call: the frame program's, whose loop is a jump inside it
				for (const shape& sh : { shape{ "once(1);", 1 }, shape{ "once(0);", 1 }, shape{ "once(6);", 1 }, shape{ "brk(5);", 1 }, shape{ "later(4);", 1 } }) {
					const cell_counters c0 = detail::engine_access::cells_counters(*e);
					e->execute(sh.call);
					const cell_counters c1 = detail::engine_access::cells_counters(*e);
					check_eq(sh.entries, c1.entries - c0.entries, std::string(sh.call) + ": block entries");
					check_eq(uint64_t{0}, c1.misses - c0.misses, std::string(sh.call) + ": nothing misses");
				}
			}
			{
				// nested counted loops with continues, inlined leaves and a class local the body copies from an element (its member sites have no
				// instance before the first iteration): the same answer on both backends, and every call after the first enters a block
				const std::string defs =
					"class TileDef { int res = 0; int station = 0; }\n"
					"array<TileDef> TILES_DEF = [];\n"
					"for (int i = 0; i < 4; i++) { TileDef t; t.res = i; t.station = 3 - i; TILES_DEF.push(t); }\n"
					"array<int> MOB_X = [20]; array<int> MOB_Y = [20]; array<int> MOB_WORLD = [0];\n"
					"array<int> GRID = []; for (int i = 0; i < 64 * 64; i++) { GRID.push(0); } GRID[29 * 64 + 26] = 2;\n"
					"int iabs(int v) { return v < 0 ? -v : v; }\n"
					"bool in_world(int w, int x, int y) { return x >= 0 && y >= 0 && x < 64 && y < 64; }\n"
					"int tile_now(int w, int x, int y) { return GRID[y * 64 + x]; }\n"
					"array<int> find_tile_near(int m, int res, int station, int maxr) {\n"
					"	int w = MOB_WORLD[m];\n"
					"	for (int r = 1; r <= maxr; ++r) {\n"
					"		for (int dy = -r; dy <= r; ++dy) {\n"
					"			for (int dx = -r; dx <= r; ++dx) {\n"
					"				if (iabs(dx) != r && iabs(dy) != r) { continue; }\n"
					"				int x = MOB_X[m] + dx;\n"
					"				int y = MOB_Y[m] + dy;\n"
					"				if (!in_world(w, x, y)) { continue; }\n"
					"				TileDef td = TILES_DEF[tile_now(w, x, y)];\n"
					"				if ((res > 0 && td.res == res) || (station > 0 && td.station == station)) { return [x, y]; }\n"
					"			}\n"
					"		}\n"
					"	}\n"
					"	return [-1, -1];\n"
					"}\n";
				const std::string call = "auto p = find_tile_near(0, 2, 0, 12); auto q = find_tile_near(0, 3, 0, 5); to_string(p[0]) + \",\" + to_string(p[1]) + \"|\" + to_string(q[0]);";
				both(defs + call, "26,29|-1", "a member site on a body-declared local");
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); stdlib::register_all(*e);
				e->execute(defs);
				e->execute("find_tile_near(0, 2, 0, 12);");
				for (int k = 0; k < 3; ++k) {
					const cell_counters c0 = detail::engine_access::cells_counters(*e);
					e->execute("find_tile_near(0, 2, 0, 12);");
					const cell_counters c1 = detail::engine_access::cells_counters(*e);
					check_true(c1.entries - c0.entries >= 1, "call " + std::to_string(k) + " enters a block: entries " + std::to_string(c1.entries - c0.entries));
					check_eq(uint64_t{0}, c1.misses - c0.misses, "call " + std::to_string(k) + ": nothing misses");
				}
			}
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "counted loops entered at their prep under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_head_entry_refusal_leaves_the_loop_to_its_back_edge", [this]() {
			// a member site on a class local the body declares (a boundary statement): at the CFOR_PREP the local holds no instance, so the
			// head entry refuses; the first iteration assigns it and the back edge enters, once per call (a head refusal does not refuse the
			// loop for the call). Both backends answer the same
			const std::string defs =
				"class TD { int res = 0; int station = 0; }\n"
				"array<TD> DEFS = [];\n"
				"for (int i = 0; i < 6; i++) { TD t; t.res = i % 3; t.station = i; DEFS.push(t); }\n"
				"int walk(int n) { int s = 0; for (int i = 0; i < n; ++i) { TD td = DEFS[i % 6]; int a = i * 3 + 1; int b = a * a + i; int c = b - a * 2; int d = c * 5 - b; int e = d + a * c; s = s + td.res * 3 + td.station + e % 7; } return s; }\n";
			both(defs + "to_string(walk(8)) + \"|\" + to_string(walk(1)) + \"|\" + to_string(walk(0));", "51|0|0", "a member site on a body-declared local");
			const std::string l = listing_run(defs + "walk(8);");
			const size_t at = l.find("== walk ");
			const std::string part = at == std::string::npos ? l : l.substr(at, l.find("\n== ", at + 1) - at);
			check_true(part.find("-- cell loop") != std::string::npos && part.find("(boundaries") != std::string::npos, "walk's loop closes with a boundary\n" + part);
			auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); stdlib::register_all(*e);
			e->execute(defs);
			e->execute("walk(8);");
			for (int k = 0; k < 3; ++k) {
				const cell_counters c0 = detail::engine_access::cells_counters(*e);
				e->execute("walk(8);");
				const cell_counters c1 = detail::engine_access::cells_counters(*e);
				check_eq(uint64_t{1}, c1.entries - c0.entries, "call " + std::to_string(k) + ": the back edge enters once");
				check_eq(uint64_t{0}, c1.misses - c0.misses, "call " + std::to_string(k) + ": nothing misses");
			}
		});
		test("a_landing_rereads_the_globals_a_callee_writes", [this]() {
			// a landing re-reads the globals the callees write: one the first callee writes and the block reads right after it, one it reads
			// only after the second call; a frame program whose head probes its callee (the door reads the trampoline's copy), before and
			// after the callee's name is rebound to a lambda; both backends, proofs off and miss injection
			const std::string prelude =
				"int G = 1;\n"
				"int H = 5;\n"
				"int bump(int d) { if (d <= 0) { G = G + 10; H = H + 3; return 1; } return bump(d - 1); }\n"
				"int other(int d) { if (d <= 0) { H = H * 2; return 2; } return other(d - 1); }\n"
				"int f(int n) { int s = 0; for (int i = 0; i < n; i++) { s = s + bump(1); s = s + G; s = s + other(1); s = s * 2 + H; } return s; }\n"
				"int h(int x) { if (x <= 0) { return 0; } return h(x - 1) + 1; }\n"
				"int g(int a) { return h(a) + 3; }\n"
				"int sum_g(int n) { int s = 0; for (int i = 0; i < n; i++) { s = s + g(i); } return s; }\n";
			const std::string body = "auto a = f(6); auto b = sum_g(5); var hv = function(int x) -> int { return x * 10; }; h = hv; auto c = sum_g(5); "
				"to_string(a) + \"|\" + to_string(b) + \"|\" + to_string(c) + \"|\" + to_string(G) + \"|\" + to_string(H);";
			both(prelude + body, "", "landings and a head probe");
			both(prelude + body, "", "landings and a head probe, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the answer: " + vm);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "landings under an injected miss at every " + std::to_string(every));
			}
		});
		test("a_range_for_is_entered_at_its_iter_next", [this]() {
			// ITER_NEXT is the entry: a one-element array runs it in the block, an empty one enters nothing, a reference binding writes the
			// elements, a break, a call and a throw from a call on the first element, a live-in the body redefines, a body that empties the array,
			// and a map and a string (the general form); both backends, proofs off, the middle end off and miss injection
			const std::string prelude =
				"int sum(array<int> a) { int s = 0; for (int v : a) { s = s * 3 + v; } return s; }\n"
				"int dbl(int n) { array<int> a = []; for (int i = 0; i < n; i++) { a.push(i + 1); } for (auto& x : a) { x = x * 2; } int s = 0; for (int v : a) { s = s + v; } return s; }\n"
				"int brk(array<int> a) { int s = 0; for (int v : a) { s = s + v; if (v > 0) { break; } } return s; }\n"
				"int sq(int x) { string why = \"c\"; return x * x; }\n"
				"int calls(array<int> a) { int s = 0; for (int v : a) { s = s + sq(v); } return s; }\n"
				"int boom(int x) { string why = \"t\"; if (x == 7) { throw \"seven\"; } return x; }\n"
				"int thr(array<int> a) { int s = 0; try { for (int v : a) { s = s + boom(v); } } catch (e) { s = s + 1000; } return s; }\n"
				"int later(array<int> a) { int s = 0; int t = 5; for (int v : a) { s = s + t; t = v * 3; } return s; }\n"
				"int shrink(array<int> a) { int s = 0; for (int v : a) { s = s + v; a.clear(); } return s; }\n"
				"int mp() { map<string, int> m = {\"a\": 1, \"b\": 2}; int s = 0; for (auto kv : m) { s = s + kv.second; } return s; }\n"
				"int st() { int s = 0; for (char c : \"abc\") { s = s + cast<int>(c); } return s; }\n";
			const std::string body = "to_string(sum([4])) + \"|\" + to_string(sum([])) + \"|\" + to_string(sum([1, 2, 3])) + \"|\" + to_string(dbl(4)) + \"|\" + to_string(brk([3, 4, 5])) + \"|\" + "
				"to_string(calls([3])) + \"|\" + to_string(calls([1, 2, 3])) + \"|\" + to_string(thr([7, 1])) + \"|\" + to_string(later([1, 2, 3])) + \"|\" + to_string(shrink([5, 6, 7])) + \"|\" + "
				"to_string(mp()) + \"|\" + to_string(st());";
			const std::string expected = "4|0|18|20|3|9|14|1000|14|5|3|294";
			both(prelude + body, expected, "range-fors entered at their head");
			both(prelude + body, expected, "range-fors entered at their head, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off answers the same");
			}
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); stdlib::register_all(*e);
				e->execute(prelude);
				struct shape { const char* call; uint64_t entries; };
				for (const shape& sh : { shape{ "sum([4]);", 1 }, shape{ "sum([]);", 0 }, shape{ "sum([1, 2, 3, 4, 5]);", 1 }, shape{ "brk([3, 4, 5]);", 1 } }) {
					const cell_counters c0 = detail::engine_access::cells_counters(*e);
					e->execute(sh.call);
					const cell_counters c1 = detail::engine_access::cells_counters(*e);
					check_eq(sh.entries, c1.entries - c0.entries, std::string(sh.call) + ": block entries");
					check_eq(uint64_t{0}, c1.misses - c0.misses, std::string(sh.call) + ": nothing misses");
				}
			}
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e);
				std::string got; try { got = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(vm, got, "range-fors entered at their head under an injected miss at every " + std::to_string(every));
			}
		});
		test("inlined_leaves_run_inside_blocks", [this]() {
			// small free leaves called from a loop block (light_band's early returns, dist2's plain body, lerp's floats, a var
			// parameter, a void leaf through an element alias) compile at their call sites; the values hold on both backends, with proofs
			// off, with the middle end's switch off (no graft) and under the injected miss; the listing names the grafts and the loop closes
			const std::string prelude =
				"int light_band(int l, int d) { int b = l - d / 8; if (b < 0) { return 0; } if (b > 31) { return 31; } return b; }\n"
				"int dist2(int ax, int ay, int bx, int by) { int dx = ax - bx; int dy = ay - by; return dx * dx + dy * dy; }\n"
				"float lerp(float a, float b, float t) { return a + (b - a) * t; }\n"
				"var twice(var v) { return v * 2; }\n"
				"class P { int x = 0; int y = 0; }\n"
				"void nudge(var& p, int k) { p.x = p.x + k; if (p.x > 50) { p.y = p.y + 1; } }\n"
				"var pool = []; for (int i = 0; i < 6; ++i) { pool.push(P()); }\n"
				"var run = function(int n) { int s = 0; for (int i = 0; i < n; ++i) { int lb = light_band(i, i * 3); int d = dist2(i, i + 1, 7, 9); float f = lerp(1.0, 5.0, i * 0.01); s = s + lb * 1000 + d % 977 + twice(i) + math::itrunc(f * 10.0); for (auto& p : pool) { nudge(p, i % 5); } } for (auto& p : pool) { s = s * 7 + p.x * 3 + p.y; } return s; };\n";
			const std::string body = "to_string(run(60)) + \"|\" + to_string(run(7));";
			both(prelude + body, "", "inlined leaves");
			both(prelude + body, "", "inlined leaves, proofs off", false);
			const std::string vm = run_on(true, prelude + body);
			check_true(vm.rfind("throw:", 0) != 0, "the shapes ran: " + vm);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(vm, off, "the middle end's switch off (no graft) agrees");
			}
			const std::string l = listing_run(prelude + body);
			check_true(l.find("-- inlined light_band") != std::string::npos && l.find("-- inlined dist2") != std::string::npos && l.find("-- inlined nudge") != std::string::npos, "the leaves are inlined: " + l);
			check_true(l.find("-- cell loop") != std::string::npos, "the loop closes around the grafts: " + l);
			if (!engine::miss_injection_available()) { return; }
			auto injected = [&](uint32_t every) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				try { return e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) { check_eq(vm, injected(every), "inlined leaves under an injected miss at every " + std::to_string(every)); }
		});
		test("methods_graft_at_their_call_sites", [this]() {
			// recv.m(args) on a slot receiver compiles as the method's body (this and the bare fields read through the receiver);
			// a subclass instance behind a base reference takes the fallback (its own override), a field owned by a user accessor keeps the
			// call (the bare name is the backing field), a void method storing fields, a method with a loop; the values hold on both
			// backends, with proofs off, with the middle end off and under the injected miss; the listing names the graft and the loop closes
			const std::string prelude =
				"class Pt { float x = 0.0; float y = 0.0; Pt(float a, float b) { x = a; y = b; } function len2() -> float { return x * x + y * y; } "
				"function scale(float k) -> void { x = x * k; y = this.y * k; } function tri(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = s + i; } return s; } }\n"
				"class Pt3 : Pt { float z = 0.0; Pt3(float a) : super(a, a) { z = a; } override function len2() -> float { return x * x + y * y + z * z; } }\n"
				"class Acc { float x = 3.0; function _get_x() -> float { return x + 1.0; } function twice() -> float { return x * 2.0; } }\n"
				"function measure(Pt& r) -> float { float h = 0.0; for (int i = 0; i < 3; ++i) { h += r.len2(); } return h; }\n"
				"var run = function(int n) -> string { Pt p = Pt(1.0, 2.0); float fs = 0.0; int k = 0; for (int i = 0; i < n; ++i) { fs += p.len2(); k = k + p.tri(i % 4); } "
				"p.scale(2.0); float g = p.len2(); var t3 = Pt3(1.0); float h = measure(t3); float h2 = measure(p); Acc a = Acc(); float w = 0.0; for (int i = 0; i < 2; ++i) { w += a.twice(); } "
				"return `${fs},${k},${g},${h},${h2},${w}`; };\n";
			const std::string body = "run(10);";
			both(prelude + body, "50.0,8,20.0,9.0,60.0,12.0", "method grafts");
			both(prelude + body, "50.0,8,20.0,9.0,60.0,12.0", "method grafts, proofs off", false);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(std::string("50.0,8,20.0,9.0,60.0,12.0"), off, "the middle end's switch off (no graft) agrees");
			}
			const std::string l = listing_run(prelude + body);
			check_true(l.find("-- inlined len2") != std::string::npos && l.find("-- inlined scale") != std::string::npos, "the methods are inlined: " + l);
			check_true(l.find("-- inlined twice") == std::string::npos, "an accessor-owned field keeps the call: " + l);
			check_true(l.find("-- cell loop") != std::string::npos, "a loop closes around a method graft: " + l);
			if (!engine::miss_injection_available()) { return; }
			const std::string vm = run_on(true, prelude + body);
			auto injected = [&](uint32_t every) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				try { return e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (uint32_t every : { 1u, 2u, 3u, 7u }) { check_eq(vm, injected(every), "method grafts under an injected miss at every " + std::to_string(every)); }
		});
		test("a_grafted_method_increments_its_bare_fields", [this]() {
			// `++f`, `f++`, `--f` and `f--` on a bare field inside a grafted method run through the receiver as the call's frame runs them,
			// the prefix forms yielding the new value and the postfix ones the old; a string field's increment keeps its raise
			const std::string prelude =
				"class P { int m = 0; float x = 1.5; int nm() { return ++m; } int pm() { return m++; } int dm() { return --m; } int qm() { return m--; }\n"
				"  float fx() { return ++x; } void bump() { ++m; } }\n"
				"class S { string t = \"a\"; int up() { [[warn_mode]] ++t; return 1; } }\n"
				"string run() { P p = P(); int a = p.nm(); int b = p.pm(); int c = p.dm(); int d = p.qm(); float e = p.fx(); p.bump();\n"
				"  string err = \"\"; S s = S(); try { s.up(); } catch (ex) { err = ex; }\n"
				"  return `${a},${b},${c},${d},${e},${p.m},${p.x}|${err}`; }\n";
			both(prelude + "run();", "", "bare field increments in grafted methods");
			check_true(run_on(true, prelude + "run();").find("1,1,1,1,2.5,1,2.5|") == 0, run_on(true, prelude + "run();"));
			const std::string l = listing_run(prelude + "run();");
			check_true(l.find("-- inlined nm") != std::string::npos && l.find("-- inlined qm") != std::string::npos, "the methods graft: " + l);
		});
		test("a_method_graft_follows_reloads", [this]() {
			// the guard validates the receiver's class at its method epoch; a reload of the class (a new body)
			// takes the fallback, so the caller sees the new method without recompiling
			const std::string prelude =
				"class Q { int v = 2; function get() -> int { return v * 10; } }\n"
				"class R { int v = 0; function get() -> int { return v + 1000; } }\n"
				"var run = function(Q& q, int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + q.get(); } return s; };\n"
				"Q qq = Q();\n";
			auto sequence = [&](bool vm) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e); register_host_math_fixture(*e);
				try {
					e->execute(prelude);
					const std::string a = e->execute("run(qq, 4);").to_string();
					e->execute("class Q { int v = 2; function get() -> int { return v * 100 + 1; } }");
					const std::string b = e->execute("run(qq, 4);").to_string();
					return a + "|" + b;
				} catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			const std::string iv = sequence(false), vv = sequence(true);
			check_eq(iv, vv, "the reload takes the fallback on the vm");
			check_eq(std::string("80|804"), vv, "the values before and after the reload");
		});
		test("member_reads_of_elements_close_loops", [this]() {
			// a[i].m inside a loop over an array of instances lands the element in an alias temp and the member in a member temp;
			// the loop closes, and the kernel facts hold: a subclass element, a user accessor read per element, a store through a call between
			// reads, an out-of-range index's text; both backends, proofs off, the middle end off and the injected miss agree
			const std::string prelude =
				"class Pt { float x = 0.0; float y = 0.0; bool alive = true; Pt(float a, float b) { x = a; y = b; alive = a > 2.0; } }\n"
				"class Pt2 : Pt { Pt2(float a) : super(a, a * 2.0) { } }\n"
				"class Acc { float v = 1.0; int reads = 0; function _get_v() -> float { reads += 1; return v + 0.5; } }\n"
				"function bump(Pt& p) -> void { p.x = p.x + 100.0; }\n"
				"var run = function(int n) -> string { array<Pt> pts = []; for (int i = 0; i < n; ++i) { pts.push(Pt(i * 1.0, 2.0)); } pts.push(Pt2(3.0)); "
				"float fs = 0.0; float gs = 0.0; int alive = 0; for (int i = 0; i < pts.size(); ++i) { fs += pts[i].x; gs = gs + pts[i].y * 2.0; if (pts[i].alive) { alive += 1; } if (i == 1) { bump(pts[2]); } } "
				"array<Acc> as = [Acc(), Acc()]; float w = 0.0; for (int i = 0; i < 2; ++i) { w += as[i].v; } "
				"string err = \"\"; try { for (int i = 0; i < 9; ++i) { fs += pts[i].x; } } catch (ex) { err = ex; } "
				"return `${fs},${gs},${alive},${w},${as[0].reads}|${err}`; };\n";
			const std::string body = "run(5);";
			const std::string interp = run_on(false, prelude + body);
			both(prelude + body, "", "element member reads");
			both(prelude + body, "", "element member reads, proofs off", false);
			check_true(interp.find("226.0,32.0,3,3.0,1|") == 0, interp.c_str());
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(interp, off, "the middle end's switch off agrees");
			}
			const std::string l = listing_run(R"(
				class Pt { float x = 0.0; float y = 0.0; }
				function f(array<Pt> pts) -> float { float fs = 0.0; for (int i = 0; i < pts.size(); ++i) { fs += pts[i].x; } return fs; }
				array<Pt> xs = [Pt(), Pt()]; f(xs);
			)");
			check_true(l.find("-- cell loop") != std::string::npos, "the element member loop closes: " + l);
			if (!engine::miss_injection_available()) { return; }
			auto injected = [&](uint32_t every) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				try { return e->execute(prelude + body).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			for (uint32_t every : { 1u, 2u, 3u, 7u }) { check_eq(interp, injected(every), "element member reads under an injected miss at every " + std::to_string(every)); }
		});
		test("constructor_field_stores_convert_and_run_accessors_elsewhere", [this]() {
			// bare field stores in constructor bodies keep their this-field entry across constructions: the declared type converts
			// (int from float, float from int), a subclass constructor's stores, a user setter on another field untouched by the bare store
			const std::string script = R"(
				class C { int n = 0; float f = 0.0; float g = 0.0; int sets = 0; function _set_g(float v) -> void { sets += 1; g = v * 2.0; } C(float a, int b) { n = a; f = b; g = a; } }
				class D : C { int m = 0; D(float a) : super(a, 2) { m = a; } }
				function run() -> string { int sn = 0; float sf = 0.0; float sg = 0.0; int st = 0; for (int i = 0; i < 50; ++i) { C c = C(i * 1.5, i); sn += c.n; sf += c.f; sg += c.g; st += c.sets; D d = D(2.5); sn += d.m; } return `${sn},${sf},${sg},${st}`; }
				run();
			)";
			both(script, "", "constructor field stores");
			both(script, "", "constructor field stores, proofs off", false);
		});
		test("lambda_locals_graft_at_their_calls", [this]() {
			// a local declared with a lambda literal grafts at its calls; a local rebound to another lambda, a capturing lambda
			// and an untyped result keep the call; the values hold on both backends, proofs off and the middle end off
			const std::string script = R"(
				int K = 3;
				function run() -> string {
					auto add = [](int a, int b) -> int { return a + b; };
					auto scale = [](float v) -> float { return v * 2.5; };
					int s = 0; float g = 0.0;
					for (int i = 0; i < 10; ++i) { s = add(s, i); g += scale(i * 1.0); }
					auto f = [](int a) -> int { return a + 1; };
					int t = 0;
					for (int i = 0; i < 4; ++i) { t = f(t); if (i == 1) { f = [](int a) -> int { return a * 10; }; } }
					int base = 7;
					auto cap = [=](int a) -> int { return a + base; };
					int u = 0;
					for (int i = 0; i < 3; ++i) { u = cap(u); }
					auto untyped = [](int a) { return a + K; };
					int w = 0;
					for (int i = 0; i < 3; ++i) { w = untyped(w); }
					return `${s},${g},${t},${u},${w}`;
				}
				run();
			)";
			both(script, "45,112.5,200,21,9", "lambda grafts");
			both(script, "45,112.5,200,21,9", "lambda grafts, proofs off", false);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(script).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(std::string("45,112.5,200,21,9"), off, "the middle end's switch off agrees");
			}
			const std::string l = listing_run(script);
			check_true(l.find("-- inlined <lambda>") != std::string::npos, "the lambda calls are inlined: " + l);
		});
		test("constructor_bodies_run_as_method_frames", [this]() {
			// constructor bodies through the method door: falling off the end yields the instance, private members (the eager
			// environment), defaults, super chains, a raise inside the body, methods and lambdas that see `this`; both backends agree
			const std::string script = R"(
				class A { int x = 0; A(int v) { if (v > 0) { x = v; } else { x = -1; } } }
				class P { private: int secret = 3; public: int shown = 0; P(int k) { shown = secret * k; } function get() -> int { return shown; } }
				class D { int a = 0; int b = 0; D(int x, int y = 7) { a = x; b = y; } }
				class Base { int n = 1; Base(int k) { n = k * 10; } }
				class Kid : Base { int m = 0; Kid(int k) : super(k) { m = n + 1; } }
				class Bad { int x = 0; Bad(int v) { x = 10 / v; } }
				class Cb { int v = 0; var f = null; Cb(int k) { v = k; f = [this]() -> int { return v * 2; }; } }
				function run() -> string {
					A a = A(5); A b = A(-2);
					P p = P(4); D d = D(1); D e = D(2, 3); Kid k = Kid(4); Cb c = Cb(21);
					string err = "";
					try { Bad z = Bad(0); } catch (ex) { err = ex; }
					int s = 0; for (int i = 1; i < 50; ++i) { A t = A(i); s += t.x; }
					return `${a.x},${b.x},${p.get()},${d.b},${e.b},${k.m},${c.f()},${s}|${err}`;
				}
				run();
			)";
			const std::string interp = run_on(false, script);
			check_eq(interp, run_on(true, script), "constructors: interpreter and vm agree");
			check_true(interp.find("5,-1,12,7,3,41,42,1225|") == 0, interp.c_str());
		});
		test("untyped_lambdas_graft_with_their_proven_return_kind", [this]() {
			// an untyped lambda whose returns all yield one certain scalar kind grafts with that result type; a lambda with mixed
			// returns, an uncertain return and a void body keep the call; values agree on both backends, proofs off and the middle end off
			const std::string script = R"(
				function run() -> string {
					auto add = [](int a, int b) { return a + b; };
					auto half = [](float v) { if (v > 10.0) { return v * 0.5; } return v; };
					auto pos = [](int a) { return a > 0; };
					auto mixed = [](int a) { if (a > 2) { return a; } return 0.5; };
					var box = [0];
					auto grab = [](var b) { return b[0]; };
					int s = 0; float h = 0.0; int p = 0; float m = 0.0; int g = 0;
					for (int i = 0; i < 8; ++i) { s = add(s, i); h += half(i * 3.0); if (pos(i - 3)) { p += 1; } m += mixed(i); g += grab(box); }
					return `${s},${h},${p},${m},${g}`;
				}
				run();
			)";
			both(script, "28,51.0,4,26.5,0", "untyped lambda grafts");
			both(script, "28,51.0,4,26.5,0", "untyped lambda grafts, proofs off", false);
			{
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->ir_passes(false); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string off; try { off = e->execute(script).to_string(); } catch (const std::exception& ex) { off = std::string("throw:") + ex.what(); }
				check_eq(std::string("28,51.0,4,26.5,0"), off, "the middle end's switch off agrees");
			}
			const std::string l = listing_run(script);
			check_true(l.find("-- inlined <lambda>") != std::string::npos, "the untyped lambdas are inlined: " + l);
		});
		test("an_inlined_callee_redefined_takes_the_fallback", [this]() {
			// the guard compares the name's binding with the function the graft was made from; a reload of the callee (the same
			// unit executed again with another body) fails it, and the fallback runs the real call, so the caller sees the new body without
			// recompiling; both backends agree across the reload
			const std::string prelude =
				"int f(int a) { return a * 2; }\n"
				"var run = function(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + f(i); } return s; };\n";
			auto sequence = [&](bool vm) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e); register_host_math_fixture(*e);
				try {
					e->execute(prelude);
					const std::string a = e->execute("run(6);").to_string();
					e->execute("int f(int a) { return a * 3 + 1; }");
					const std::string b = e->execute("run(6);").to_string();
					return a + "|" + b;
				} catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			const std::string iv = sequence(false), vv = sequence(true);
			check_eq(iv, vv, "the reload takes the fallback on the vm");
			check_eq(std::string("30|51"), vv, "the values before and after the reload");
		});
		test("an_inlined_callee_redefined_away_and_back_holds_each_time", [this]() {
			// the guard remembers the function object it validated: a redefinition fails it, and the first definition run again
			// holds again; every step agrees with the interpreter
			const std::string first = "int f(int a) { return a * 2; }\n";
			const std::string prelude = first + "var run = function(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + f(i); } return s; };\n";
			auto sequence = [&](bool vm) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e); register_host_math_fixture(*e);
				try {
					std::string out;
					e->execute(prelude);
					for (int round = 0; round < 3; ++round) {
						out += e->execute("run(6);").to_string() + ",";
						e->execute("int f(int a) { return a * 3 + 1; }");
						out += e->execute("run(6);").to_string() + ",";
						e->execute(first);
						out += e->execute("run(6);").to_string() + ";";
					}
					return out;
				} catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			const std::string iv = sequence(false), vv = sequence(true);
			check_eq(iv, vv, "both backends across the redefinitions");
			check_eq(std::string("30,51,30;30,51,30;30,51,30;"), vv, "each definition's values");
		});
		test("a_dropped_jaibite_leaves_its_stamps_resolvable_or_expired", [this]() {
			// the checker's callee stamps outlive the program they name: a host that executes a jaibite and drops it (the demoreel)
			// keeps the leaf graftable while its function lives, and a leaf whose declaration died keeps the call (a stamp read
			// after free crashed the demoreel's first frames)
			const std::string program =
				"int leaf(int a) { return a * 2 + 1; }\n"
				"int caller(int n) { int s = 0; for (int i = 0; i < n; ++i) { int v = leaf(i); s = s + v; } return s; }\n";
			auto sequence = [&](bool vm, bool redefine, std::string* listing) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e); register_host_math_fixture(*e);
				if (listing) { e->disassembly_sink([listing](const std::string& l, const std::string&) { *listing += l; *listing += "\n"; }); }
				try {
					{ auto bite = e->jaibite(program); bite.execute(); }
					if (redefine) { e->execute("int leaf(int a) { return a * 3; }"); }
					return e->execute("caller(6);").to_string();
				} catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			std::string kept_listing, gone_listing;
			check_eq(sequence(false, false, nullptr), sequence(true, false, &kept_listing), "the live leaf: both backends");
			check_eq(std::string("36"), sequence(true, false, nullptr), "the live leaf's value");
			check_true(kept_listing.find("-- inlined leaf") != std::string::npos, "the leaf grafts after its jaibite is gone: " + kept_listing);
			check_eq(sequence(false, true, nullptr), sequence(true, true, &gone_listing), "the redefined leaf: both backends");
			check_eq(std::string("45"), sequence(true, true, nullptr), "the redefined leaf's value");
			check_true(gone_listing.find("-- call kept leaf line 2: a callee whose declaration is gone") != std::string::npos, "the dead declaration keeps the call: " + gone_listing);
		});
		test("a_raise_inside_an_inlined_body_names_the_callee", [this]() {
			// an inlined body that raises keeps the text and the trace's first frame is the callee, as the interpreter's is
			const std::string prelude =
				"int f(int a) { return 100 / a; }\n"
				"var run = function(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + f(3 - i); } return s; };\n";
			both(prelude + "run(6);", "", "a raise inside an inlined body");
			auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); stdlib::register_all(*e); register_host_math_fixture(*e);
			std::string text;
			try { e->execute(prelude + "run(6);"); } catch (const std::exception& ex) { text = ex.what(); }
			check_true(text.find("zero") != std::string::npos || text.find("Zero") != std::string::npos, "the division text: " + text);
			const auto trace = e->last_stack_trace();
			check_true(!trace.empty() && trace.front().function == "f", "the trace names the callee first: " + (trace.empty() ? std::string("<empty>") : trace.front().function));
		});
		test("a_global_read_before_a_writing_callee_is_not_hoisted", [this]() {
			// `s + G + bump()` reads G before the call on both backends (the operand spills into a temp ahead of the graft); a bare
			// global before the call of a body that writes one is taken into a temp ahead of the graft, so the order holds
			const std::string prelude =
				"int G = 1;\n"
				"int bump() { G = G + 1; return G; }\n"
				"int pure(int a) { return a + 1; }\n"
				"var run = function(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + G + bump(); s = G + bump() + s; s = s + G + pure(i); } return s; };\n";
			both(prelude + "run(8);", "", "a global read before a writing callee");
			const std::string l = listing_run(prelude + "run(8);");
			const size_t first = l.find("-- inlined bump");
			check_true(first != std::string::npos && l.find("-- inlined bump", first + 1) != std::string::npos && l.find("OPERAND_TEMP") != std::string::npos, "bump grafts behind the bare global's snapshot: " + l);
			check_true(l.find("-- inlined pure") != std::string::npos, "pure is inlined after a global read: " + l);
		});
		test("string_and_var_parameters_at_the_grafts_edge", [this]() {
			// a var parameter taking a scalar local with a proven int return is a graft, and so is a var result the site's kinds make
			// one scalar kind (it holds what the return yields); a string parameter or a string result keeps the call (their declarations
			// would open the enclosing block); the values hold either way
			const std::string prelude =
				"string tag(string s, int k) { if (k > 2) { return s + \"!\"; } return s; }\n"
				"var half(var v) { return v / 2; }\n"
				"int half2(var v) { int r = v / 2; return r; }\n"
				"var run = function(int n) { int s = 0; string acc = \"\"; for (int i = 0; i < n; ++i) { acc = acc + tag(\"a\", i); s = s + tag(\"bc\", i).size(); s = s + half(i); s = s + half2(i); } return acc + \"|\" + to_string(s); };\n";
			both(prelude + "run(6);", "", "string and var parameters");
			const std::string l = listing_run(prelude + "run(6);");
			check_true(l.find("-- inlined half2") != std::string::npos && l.find("-- inlined half ") != std::string::npos, "half2 and half are inlined: " + l);
			check_true(l.find("-- inlined tag") == std::string::npos, "tag (a string) keeps its call: " + l);
		});
		test("a_call_under_a_conditional_operand_keeps_its_call", [this]() {
			// a leaf in a ternary's arm or on the right side of && / || runs on the paths that choose it; a graft would run its body
			// on every path, so the statement keeps the call (N counts bump's runs); a leaf in a plain operand of the same loop grafts
			const std::string prelude =
				"int N = 0;\n"
				"int bump(int x) { N = N + 1; return x; }\n"
				"int pure(int a) { return a + 1; }\n"
				"var run = function(int n) { int s = 0; for (int i = 0; i < n; ++i) { int v = (i % 2 == 0) ? bump(i) : 0; bool ok = (i > 2) && bump(i) > 0; int w = ((i > 100) || bump(i) < 0) ? 1 : 0; s = s + v + w + pure(i) + (ok ? 1 : 0); } return s * 1000 + N; };\n";
			both(prelude + "to_string(run(8));", "53017", "a call under a conditional operand");
			const std::string l = listing_run(prelude + "run(8);");
			check_true(l.find("-- inlined bump") == std::string::npos, "bump under a conditional operand keeps its call: " + l);
			check_true(l.find("-- inlined pure") != std::string::npos, "pure in a plain operand is inlined: " + l);
		});
		test("a_body_writing_through_a_reference_parameter_grafts_only_at_the_statements_front", [this]() {
			// `s + i + inc(i)` reads i before inc writes it through its var& parameter (a store, or tick's ++ of the parameter's
			// name); the exact hoist spills `s + i` into a typed temp ahead of the body, so all three graft and the values hold
			const std::string prelude =
				"int inc(var& p) { p = p + 1; int r = p; return r; }\n"
				"int inc2(var& p) { p = p + 1; int r = p; return r; }\n"
				"int tick(var& p) { p++; int r = p; return r; }\n"
				"var run = function(int n) { int s = 0; int i = 0; int j = 0; int k = 0; while (i < n) { s = s + i + inc(i); j = j + k + tick(k); int t = inc2(j); s = s + t; } return s * 100 + j + k; };\n";
			both(prelude + "to_string(run(6));", "14848", "a reference write behind a read");
			const std::string l = listing_run(prelude + "run(6);");
			check_true(l.find("-- inlined inc2") != std::string::npos && l.find("-- inlined inc ") != std::string::npos && l.find("-- inlined tick") != std::string::npos, "inc, tick and inc2 are inlined (the reads before them spilled): " + l);
			check_true(l.find("-- cell loop") != std::string::npos, "the loop closes around the three grafts: " + l);
		});
		test("an_increment_of_a_global_in_the_body_is_a_write", [this]() {
			// `G++` in the body writes G (an identifier's own increment, not a compound store); a statement that reads G before the
			// call keeps it in the loop's first compile, and the second compile (the loop closed over that call) spills `s + G` ahead of the graft
			const std::string prelude =
				"int G = 0;\n"
				"int nextg() { G++; int r = G; return r; }\n"
				"var run = function(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + G + nextg(); } return s; };\n";
			both(prelude + "to_string(run(5));", "25", "an increment of a global in the body");
			const std::string l = listing_run(prelude + "run(5);");
			const size_t at = l.find("== <lambda>");
			check_true(at != std::string::npos && l.find("-- cell loop", at) != std::string::npos, "the loop closes: " + l);
		});
		test("a_store_the_statement_completes_before_the_call_keeps_it", [this]() {
			// `(i = i + 1) + twice(i)` stores i before the call reads it, `(G = G + 1) + readg()` stores G before the body reads it;
			// a graft would run the arguments and the body first, so both statements keep their calls; the declaration after them grafts
			const std::string prelude =
				"int G = 0;\n"
				"int twice(int a) { return a * 2; }\n"
				"int twice2(int a) { return a * 2; }\n"
				"int readg() { int r = G; return r; }\n"
				"var run = function(int n) { int s = 0; int i = 0; while (i < n) { s = s + (i = i + 1) + twice(i); s = s + (G = G + 1) + readg(); int d = twice2(i); s = s + d; } return s * 10 + i + G; };\n";
			both(prelude + "to_string(run(4));", "708", "a store before the call");
			const std::string l = listing_run(prelude + "run(4);");
			check_true(l.find("-- inlined twice2") != std::string::npos, "twice2 in a declaration is inlined: " + l);
			check_true(l.find("-- inlined twice ") == std::string::npos && l.find("-- inlined readg") == std::string::npos, "twice and readg behind a store keep their calls: " + l);
		});
		test("a_static_member_read_before_a_body_writing_it_keeps_the_call", [this]() {
			// `s + Counter::n + bumpc()` reads the static before bumpc writes it; a static member read is a member read to the scan
			const std::string prelude =
				"class Counter { static int n = 0; }\n"
				"int bumpc() { Counter::n = Counter::n + 1; int r = Counter::n; return r; }\n"
				"var run = function(int k) { int s = 0; for (int i = 0; i < k; ++i) { s = s + Counter::n + bumpc(); } return s; };\n";
			both(prelude + "to_string(run(5));", "25", "a static read before a body writing it");
			const std::string l = listing_run(prelude + "run(5);");
			check_true(l.find("-- inlined bumpc") == std::string::npos, "bumpc after a read of the static keeps its call: " + l);
		});
		test("every_position_takes_the_same_graft", [this]() {
			// the call becomes a temp, so an operand, a declaration, an assignment, an if condition, a while condition, a for condition
			// and update, an argument of another leaf and an argument of a host function are one position; the values on both backends
			const std::string prelude =
				"int add1(int a) { return a + 1; }\n"
				"int twice(int a) { return a * 2; }\n"
				"var run = function(int n) { int s = 0; int sum_d = 0; int sum_t = 0; int t = 0; int c1 = 0; int sum_e = 0; string acc = \"\";\n"
				"  for (int i = 0; i < n; ++i) { s = s + add1(i); int d = add1(i); sum_d = sum_d + d; t = add1(i); sum_t = sum_t + t; if (add1(i) > 3) { c1 = c1 + 1; } int e = twice(add1(i)); sum_e = sum_e + e; acc = acc + to_string(add1(i)); }\n"
				"  int w = 0; while (add1(w) < 4) { w = w + 1; }\n"
				"  int c2 = 0; for (int k = 0; add1(k) < 4; k = add1(k)) { c2 = c2 + 1; }\n"
				"  return to_string(s) + \",\" + to_string(sum_d) + \",\" + to_string(sum_t) + \",\" + to_string(c1) + \",\" + to_string(w) + \",\" + to_string(c2) + \",\" + to_string(sum_e) + \",\" + acc; };\n";
			both(prelude + "run(5);", "15,15,15,2,3,3,30,12345", "every position");
			both(prelude + "run(5);", "15,15,15,2,3,3,30,12345", "every position, proofs off", false);
			const std::string l = listing_run(prelude + "run(5);");
			size_t grafts = 0; for (size_t at = l.find("-- inlined add1"); at != std::string::npos; at = l.find("-- inlined add1", at + 1)) { ++grafts; }
			check_true(grafts >= 9, "add1 grafts at every position (" + std::to_string(grafts) + "): " + l);
			check_true(l.find("-- inlined twice") != std::string::npos, "twice over the hoisted add1 is inlined: " + l);
		});
		test("a_spilled_operand_raises_before_the_body", [this]() {
			// `A[i] + div(1, i - 9)` reads the element before the call; the graft spills the read into a typed temp ahead of the body, so
			// with i out of range the element's raise comes first, as the interpreter's does, never the body's division
			const std::string prelude =
				"array<int> A = [1, 2, 3];\n"
				"int div(int a, int b) { return a / b; }\n"
				"var run = function(int i) { int s = A[i] + div(1, i - 9); return s; };\n";
			both(prelude + "to_string(run(1));", "2", "a spilled element read, in range");
			both(prelude + "to_string(run(9));", "", "a spilled element read, out of range");
			const std::string vm = run_on(true, prelude + "to_string(run(9));");
			check_true(vm.rfind("throw:", 0) == 0 && vm.find("ivision") == std::string::npos, "the element's raise comes first: " + vm);
			const std::string l = listing_run(prelude + "run(1);");
			check_true(l.find("-- inlined div") != std::string::npos, "div behind a spilled read is inlined: " + l);
		});
		test("returns_in_many_arms_jump_to_one_join", [this]() {
			// a body with returns in six arms and one inside a nested block grafts (each return declares the result and jumps to the join;
			// the rests are never cloned), on both backends, proofs on and off
			const std::string prelude =
				"int tone(int kind, int tx) {\n"
				"  if (kind == 1) { int t = tx & 1; if (tx > 4) { t = 2; } return t; }\n"
				"  if (kind == 2) { int t = (tx >> 1) & 1; return t; }\n"
				"  if (kind == 3) { return 3; }\n"
				"  if (kind == 4) { { int q = tx * 2; return q; } }\n"
				"  if (tx < 3) { return 7; }\n"
				"  return 0;\n"
				"}\n"
				"var run = function(int n) { int s = 0; for (int k = 0; k <= 5; ++k) { for (int tx = 0; tx < n; ++tx) { s = s + tone(k, tx); } } return s; };\n";
			both(prelude + "to_string(run(6));", "96", "returns in many arms");
			both(prelude + "to_string(run(6));", "96", "returns in many arms, proofs off", false);
			const std::string l = listing_run(prelude + "run(6);");
			check_true(l.find("-- inlined tone") != std::string::npos, "tone is inlined through the join: " + l);
		});
		test("an_unproven_return_converts_with_the_returns_text", [this]() {
			// `return cells[i]` from a var container has no proof; the graft declares the result with the return site, so a string
			// element raises the callee's own text on both backends, and an int element is the value
			const std::string prelude =
				"var cells = [1, 2, \"x\"];\n"
				"int cell_at(int i) { return cells[i]; }\n"
				"var run = function(int i) { int v = cell_at(i) + 1; return v; };\n";
			both(prelude + "to_string(run(1));", "3", "an unproven return, an int element");
			both(prelude + "to_string(run(2));", "", "an unproven return, a string element");
			const std::string vm = run_on(true, prelude + "to_string(run(2));");
			check_true(vm.find("Cannot return") != std::string::npos && vm.find("cell_at") != std::string::npos, "the return's text: " + vm);
			const std::string l = listing_run(prelude + "run(1);");
			check_true(l.find("-- inlined cell_at") != std::string::npos, "cell_at is inlined with the return site: " + l);
		});
		test("a_hoisted_condition_takes_the_fallback_after_a_reload", [this]() {
			// a leaf in an if condition grafts ahead of the condition's jumps; a reload of the callee fails the guard and the fallback
			// declares the real call's result into the same temp, so the condition reads the new body's value
			const std::string prelude =
				"int lim(int a) { return a + 10; }\n"
				"var run = function(int n) { int c = 0; for (int i = 0; i < n; ++i) { if (lim(i) > 12) { c = c + 1; } } return c; };\n";
			auto sequence = [&](bool vm) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e); register_host_math_fixture(*e);
				try {
					e->execute(prelude);
					const std::string a = e->execute("run(5);").to_string();
					e->execute("int lim(int a) { return a - 10; }");
					const std::string b = e->execute("run(5);").to_string();
					return a + "|" + b;
				} catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			const std::string iv = sequence(false), vv = sequence(true);
			check_eq(iv, vv, "the reload takes the fallback on the vm");
			check_eq(std::string("2|0"), vv, "the condition before and after the reload");
			const std::string l = listing_run(prelude + "run(5);");
			check_true(l.find("-- inlined lim") != std::string::npos, "lim in the condition is inlined: " + l);
		});
		test("a_call_behind_an_effect_stays_a_call", [this]() {
			// the first call a statement runs is the one hoisted; a method call or a host call first keeps every later leaf a call
			// (it would run behind an effect), while a leaf first grafts and the effect follows
			const std::string prelude =
				"int add1(int a) { return a + 1; }\n"
				"int add2(int a) { return a + 2; }\n"
				"var run = function(int n) { var a = [1, 2]; int s = 0; for (int i = 0; i < n; ++i) { s = s + a.size() + add1(i); s = s + add2(i) + a.size(); } return s; };\n";
			both(prelude + "to_string(run(4));", "40", "a call behind an effect");
			const std::string l = listing_run(prelude + "run(4);");
			check_true(l.find("-- inlined add1") == std::string::npos, "add1 behind the method call keeps its call: " + l);
			check_true(l.find("-- inlined add2") != std::string::npos, "add2 ahead of the method call is inlined: " + l);
		});
		test("a_leaf_another_unit_declares_grafts", [this]() {
			// tile_at lives in maps.jai and spot_free in sim.jai; a leaf a unit imports resolves through the program's function table
			// (the closure's cached unit, the one the import executes), so it grafts in the importer's loop and the guard reads the live binding
			namespace fs = std::filesystem;
			fs::path dir = fs::temp_directory_path() / "jai_row1531";
			fs::create_directories(dir);
			std::ofstream(dir / "lib.jai") << "int add1(int a) { return a + 1; }\nclass Grid { int mw = 4; var cells = []; }\nshared_ptr<Grid> GR = null;\nint cell_at(int x, int y) { if (x < 0 || x >= GR.mw) { return 1; } return GR.cells[y * GR.mw + x]; }\n";
			std::ofstream(dir / "main.jai") << "import \"lib.jai\";\nGR = new Grid(); for (int k = 0; k < 16; ++k) { GR.cells.push(k % 5 == 0 ? 1 : 0); }\n"
			                                  "var run = function(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + add1(i); } return s; };\n"
			                                  "var probe = function(int x) { if (cell_at(x, 1) != 0) { return 7; } return add1(x); };\n"
			                                  "to_string(run(6)) + \"|\" + to_string(probe(0)) + \"|\" + to_string(probe(1));\n";
			auto run_file = [&](bool vm, std::string* listing) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e); register_host_math_fixture(*e);
				if (listing) { e->disassembly_sink([listing](const std::string& l, const std::string&) { *listing += l; *listing += "\n"; }); }
				e->add_include_path(dir.string());
				try { return e->execute_file((dir / "main.jai").string()).as_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			std::string l;
			const std::string iv = run_file(false, nullptr), vv = run_file(true, &l);
			check_eq(iv, vv, "a leaf another unit declares: both backends");
			check_eq(std::string("21|1|7"), vv, "the values");
			check_true(l.find("-- inlined add1") != std::string::npos, "add1 from the imported unit is inlined: " + l);
			check_true(l.find("-- inlined cell_at") != std::string::npos, "cell_at from the imported unit is inlined in a condition (its return converts at the result): " + l);
		});
		test("a_global_another_unit_declares_types_the_proofs", [this]() {
			// ND_X in JaiDOOM's map.jai and G in GLOOM's game.jai; a top-level global one unit declares with a spelled type is typed
			// by that declaration in every other unit's proof pass, a class-typed one once its class is defined
			// (the importer's proofs re-stamp after the import; its callers are named functions, compiled at their first call after it), so `at`
			// and `span` graft inside their loops on both accounts
			namespace fs = std::filesystem;
			fs::path dir = fs::temp_directory_path() / "jai_row1541";
			fs::create_directories(dir);
			std::ofstream(dir / "data.jai") << "array<int> ND = [3, 1, 4, 1, 5, 9, 2, 6];\nclass World { int mw = 8; float px = 2.5; var tiles = []; }\nshared_ptr<World> G = null;\n";
			std::ofstream(dir / "lib.jai") << "int at(int i) { return ND[i]; }\nfloat span(float a) { return a - G.px; }\nint width() { return G.mw; }\n"
			                                 "int sum(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + at(i); } return s; }\n"
			                                 "float walk(int n) { float acc = 0.0; for (int i = 0; i < n; ++i) { acc = acc + span(i * 1.0); } return acc; }\n";
			std::ofstream(dir / "main.jai") << "import \"lib.jai\";\nimport \"data.jai\";\nG = new World();\nto_string(sum(8)) + \"|\" + to_string(walk(4)) + \"|\" + to_string(width());\n";
			auto run_file = [&](bool vm, std::string* listing) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e); register_host_math_fixture(*e);
				if (listing) { e->disassembly_sink([listing](const std::string& l, const std::string&) { *listing += l; *listing += "\n"; }); }
				e->add_include_path(dir.string());
				try { return e->execute_file((dir / "main.jai").string()).as_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			std::string l;
			const std::string iv = run_file(false, nullptr), vv = run_file(true, &l);
			check_eq(iv, vv, "a global another unit declares: both backends");
			check_eq(std::string("31|-4.0|8"), vv, "the values");
			check_true(l.find("-- inlined at") != std::string::npos, "at grafts in the loop: its return reads the typed global's element: " + l);
			check_true(l.find("-- inlined span") != std::string::npos, "span grafts in the loop: its return reads the class-typed global's field after the re-stamp: " + l);
			check_true(l.find("-- call kept at ") == std::string::npos, "no kept call for at: " + l);
		});
		test("an_argument_reading_a_typed_global_another_unit_declares_is_proven", [this]() {
			// Gator Quest's walk_at(w, MOB_X[m], MOB_Y[m]): under imports an argument that reads a program-scope typed global (a scalar, or an
			// element of an array of scalars) one unit declares is proven like a local, so the leaf grafts; the graft re-checks the global's
			// kind once a run moved the definition epoch, so after the declaring unit runs again with another type the call is the call (its
			// binding converts or raises in the call's own order and text), on both backends alike
			namespace fs = std::filesystem;
			fs::path dir = fs::temp_directory_path() / "jai_arg_global_proof";
			fs::create_directories(dir);
			std::ofstream(dir / "data.jai") << "array<int> MX = [2, 4, 6, 8];\nint OFF = 1;\n";
			std::ofstream(dir / "lib.jai") << "int add1(int a) { return a + 1; }\n"
			                                 "int total(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + add1(MX[i] + OFF); } return s; }\n"
			                                 "int once() { return add1(MX[0]); }\n";
			std::ofstream(dir / "main.jai") << "import \"lib.jai\";\nimport \"data.jai\";\nto_string(total(4)) + \"|\" + to_string(once());\n";
			auto run_file = [&](bool vm, std::string* listing) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e); register_host_math_fixture(*e);
				e->import_recheck_interval(0);
				if (listing) { e->disassembly_sink([listing](const std::string& l, const std::string&) { *listing += l; *listing += "\n"; }); }
				e->add_include_path(dir.string());
				std::string out;
				try { out = e->execute_file((dir / "main.jai").string()).as_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
				// the declaring unit runs again with float elements: the proof's kind no longer holds
				std::ofstream(dir / "data.jai") << "array<float> MX = [1.0e100, 2.5, 4.5, 6.5, 8.5];\nint OFF = 1;\n";
				try {
					out += "|" + e->execute("import \"data.jai\";\nvar r = \"\";\ntry { r = to_string(once()); } catch (e) { r = e; }\nr;").as_string();
				} catch (const std::exception& ex) { out += std::string("|throw:") + ex.what(); }
				std::ofstream(dir / "data.jai") << "array<int> MX = [2, 4, 6, 8];\nint OFF = 1;\n";
				return out;
			};
			std::string l;
			const std::string iv = run_file(false, nullptr), vv = run_file(true, &l);
			check_eq(iv, vv, "a typed global's element as an argument: both backends, before and after the declaring unit changed its type");
			check_eq(std::string("28|3|"), vv.substr(0, 5), "the values before the reload: " + vv);
			check_true(l.find("-- inlined add1") != std::string::npos, "add1 grafts with arguments reading MX and OFF: " + l);
			check_true(l.find("-- call kept add1") == std::string::npos, "no kept call for add1: " + l);
		});
		test("a_size_call_and_a_nested_leafs_result_graft_under_imports", [this]() {
			// Gator Quest's mob_live (`m < MOB_KIND.size()` in the body) and cheb (`imax(iabs(x0 - x1), iabs(y0 - y1))`): a body reading a proven
			// array's size grafts, and a call's result is a proven argument when its callee is a free function declared to return that scalar;
			// the graft re-checks the callee's declared return once a run moved the definition epoch, so after the declaring unit runs again
			// with another return type the call is the call, on both backends alike
			namespace fs = std::filesystem;
			fs::path dir = fs::temp_directory_path() / "jai_size_nested_proof";
			fs::create_directories(dir);
			const std::string util_int = "int iabs(int v) { return v < 0 ? -v : v; }\nint imax(int a, int b) { return a > b ? a : b; }\n";
			std::ofstream(dir / "util.jai") << util_int;
			std::ofstream(dir / "data.jai") << "array<int> KIND = [3, -1, 4, 0];\n";
			std::ofstream(dir / "lib.jai") << "import \"util.jai\";\nimport \"data.jai\";\n"
			                                 "bool live(int m) { return m >= 0 && m < KIND.size() && KIND[m] >= 0; }\n"
			                                 "int cheb(int x0, int y0, int x1, int y1) { return imax(iabs(x0 - x1), iabs(y0 - y1)); }\n"
			                                 "int probe() { int n = 0; if (live(0)) { n = n + 1; } if (live(1)) { n = n + 10; } if (live(7)) { n = n + 100; } int d = cheb(1, 2, 4, -3); return n + d; }\n";
			std::ofstream(dir / "main.jai") << "import \"lib.jai\";\nto_string(probe());\n";
			auto run_file = [&](bool vm, std::string* listing) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e); register_host_math_fixture(*e);
				e->import_recheck_interval(0);
				if (listing) { e->disassembly_sink([listing](const std::string& l, const std::string&) { *listing += l; *listing += "\n"; }); }
				e->add_include_path(dir.string());
				std::string out;
				try { out = e->execute_file((dir / "main.jai").string()).as_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
				std::ofstream(dir / "util.jai") << "float iabs(int v) { return (v < 0 ? -v : v) + 0.5; }\nint imax(int a, int b) { return a > b ? a : b; }\n";
				try {
					out += "|" + e->execute("import \"util.jai\";\nvar r = \"\";\ntry { r = to_string(probe()); } catch (e) { r = e; }\nr;").as_string();
				} catch (const std::exception& ex) { out += std::string("|throw:") + ex.what(); }
				std::ofstream(dir / "util.jai") << util_int;
				return out;
			};
			std::string l;
			const std::string iv = run_file(false, nullptr), vv = run_file(true, &l);
			check_eq(iv, vv, "a size call and a nested leaf's result as arguments: both backends, before and after iabs changed its return type");
			check_eq(std::string("6|"), vv.substr(0, 2), "the value before the reload: " + vv);
			check_true(l.find("-- inlined live") != std::string::npos, "live grafts with its size call: " + l);
			check_true(l.find("-- inlined cheb") != std::string::npos, "cheb grafts with imax taking iabs's results: " + l);
			check_true(l.find("-- call kept imax") == std::string::npos, "no kept call for imax: " + l);
		});
		test("leaves_graft_behind_short_circuits_in_builtin_arguments_and_after_names", [this]() {
			// Gator Quest's kept calls: `if (o < 0 || !hostile(m, o))` grafts the leaf between the short-circuit jumps, so it runs only on the
			// path that reaches its operand; `TVAR.push(hash3(...) & 255)` grafts in a builtin method's argument (a proven array whose receiver
			// the graft cannot write); `return n + cheb(...)` grafts when the callee has no reference parameter (a nested leaf's fallback cannot
			// reach the caller's local)
			const std::string prelude =
				"int calls = 0;\n"
				"bool chk(int v) { calls = calls + 1; return v > 2; }\n"
				"int short_circuit() { int n = 0; for (int i = 0; i < 6; ++i) { if (i < 3 || chk(i)) { n = n + 1; } if (i > 9 && chk(i)) { n = n + 1000; } } return n * 100 + calls; }\n"
				"array<int> OUT;\n"
				"int sq(int v) { return v * v; }\n"
				"int pushes() { array<int> loc; for (int i = 0; i < 4; ++i) { OUT.push(sq(i) + 1); loc.push(sq(i)); } int s = 0; for (int i = 0; i < 4; ++i) { s = s + OUT[i] + loc[i]; } return s; }\n"
				"int iabs(int v) { return v < 0 ? -v : v; }\n"
				"int imax(int a, int b) { return a > b ? a : b; }\n"
				"int cheb(int x0, int y0, int x1, int y1) { return imax(iabs(x0 - x1), iabs(y0 - y1)); }\n"
				"int after_name() { int n = 1; return n + cheb(1, 2, 4, -3); }\n";
			const std::string run = "to_string(short_circuit()) + \"|\" + to_string(pushes()) + \"|\" + to_string(after_name());";
			both(prelude + run, "603|32|6", "leaves behind a short circuit, in builtin arguments and after a name");
			both(prelude + run, "603|32|6", "leaves behind a short circuit, in builtin arguments and after a name, proofs off", false);
			const std::string l = listing_run(prelude + "short_circuit(); pushes(); after_name();");
			check_true(l.find("-- inlined chk") != std::string::npos, "chk grafts behind || and &&: " + l);
			check_true(l.find("-- call kept chk") == std::string::npos, "no kept call for chk: " + l);
			check_true(l.find("-- call kept sq") == std::string::npos, "sq grafts in the push arguments: " + l);
			check_true(l.find("-- inlined cheb") != std::string::npos, "cheb grafts after the name n: " + l);
		});
		test("by_value_parameters_of_any_type_graft_with_the_calls_copy", [this]() {
			// Gator Quest's item_stat(worn[i], code): a by-value parameter of a class, an array or a string grafts; the argument lands in a temp
			// in the call's order and the parameter's declaration binds it after every argument by the parameter's table and text, so a body
			// writing its copy leaves the caller's value alone, a subclass element or a var argument binds as the call binds it, a mismatch
			// raises the call's text and a nocopy instance raises where the call raises it
			const std::string prelude =
				"class Item { int base = -1; int pmag = 0; array<int> tags = [1, 2]; }\n"
				"class Sword : Item { int edge = 2; }\n"
				"array<Item> WORN;\n"
				"int item_stat(Item it, int code) { if (it.base < 0) { return 0; } int v = it.pmag * code; it.pmag = 99; it.tags[0] = code; return v + it.tags[0]; }\n"
				"int bonus(int code) { int v = 0; array<Item>& worn = WORN; for (int i = 0; i < worn.size(); ++i) { switch (i) { case 9: v = v - 1; } if (worn[i].base >= 0) { v = v + item_stat(worn[i], code); } } return v; }\n"
				"int one() { var vi = WORN[1]; return item_stat(WORN[1], 5) + item_stat(WORN[2], 1) + item_stat(vi, 1); }\n"
				"int bad() { var x = 5; return item_stat(x, 1); }\n"
				"int asum(array<int> a, int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + a[i]; } a[0] = 100; return s + a[0]; }\n"
				"array<int> NUMS = [4, 5, 6];\n"
				"int same(string s, string t, int k) { if (s == t) { return k; } return -k; }\n"
				"string NAME = \"gator\";\n"
				"int run_all() { int a = asum(NUMS, 3); int b = same(NAME, \"gator\", 3) + same(NAME, \"croc\", 1); return a * 100 + b; }\n"
				"for (int k = 0; k < 3; ++k) { Item t; t.base = k - 1; t.pmag = k * 3; WORN.push(t); }\n"
				"Sword sw; sw.base = 2; sw.pmag = 1; WORN.push(sw);\n"
				"var bad_text = \"\"; try { bad(); } catch (e) { bad_text = e; }\n";
			const std::string run = "bad_text + \"|\" + to_string(bonus(2)) + \"|\" + to_string(bonus(2)) + \"|\" + to_string(one()) + \"|\" + to_string(run_all()) + \"|\" + to_string(WORN[1].pmag) + to_string(WORN[1].tags[0]) + to_string(NUMS[0]);";
			both(prelude + run, "Cannot convert int to Item (no suitable single-argument constructor)|26|26|31|11502|314", "by-value class, array and string parameters");
			both(prelude + run, "Cannot convert int to Item (no suitable single-argument constructor)|26|26|31|11502|314", "by-value class, array and string parameters, proofs off", false);
			const std::string l = listing_run(prelude + "bonus(2); one(); run_all(); bad_text;");
			check_true(l.find("-- inlined item_stat") != std::string::npos, "item_stat grafts: " + l);
			check_true(l.find("-- call kept item_stat") == std::string::npos, "no kept call for item_stat: " + l);
			check_true(l.find("-- inlined asum") != std::string::npos, "asum grafts: " + l);
			check_true(l.find("-- inlined same") != std::string::npos, "same grafts: " + l);
			// a nocopy argument raises the call's text after every argument ran
			const std::string nc =
				"nocopy class Ledger { int total = 1; }\n"
				"int seen = 0;\n"
				"int mark(int v) { seen = seen + v; return v; }\n"
				"int peek(Ledger l, int k) { return l.total + k; }\n"
				"Ledger LED;\n"
				"int probe() { return peek(LED, mark(5)); }\n"
				"var r = \"\"; try { r = to_string(probe()); } catch (e) { r = e; } r + \"|\" + to_string(seen);";
			auto ie = jai::foundry::make_default_engine(); ie->set_backend(backend_type::interpreter); auto ve = jai::foundry::make_default_engine(); ve->set_backend(backend_type::vm);
			stdlib::register_all(*ie); stdlib::register_all(*ve);
			std::string iv, vv;
			try { iv = ie->execute(nc).as_string(); } catch (const std::exception& ex) { iv = std::string("throw:") + ex.what(); }
			try { vv = ve->execute(nc).as_string(); } catch (const std::exception& ex) { vv = std::string("throw:") + ex.what(); }
			check_eq(iv, vv, "a nocopy argument: both backends");
			check_true(vv.find("nocopy class 'Ledger'") != std::string::npos && vv.substr(vv.size() - 2) == "|5", "the call's text after the later argument ran: " + vv);
		});
		test("a_by_value_parameter_proof_over_a_global_of_any_type_holds_across_its_units_rerun", [this]() {
			// a typed global of a class or a string another unit declares proves its argument; the graft re-checks the global's declared type
			// by identity once a run moved the definition epoch, so after the declaring unit runs again with another type the call is the call
			namespace fs = std::filesystem;
			fs::path dir = fs::temp_directory_path() / "jai_any_type_global_proof";
			fs::create_directories(dir);
			const std::string data = "class Box { int v = 7; }\nBox BX;\nstring TAG = \"abc\";\n";
			std::ofstream(dir / "data.jai") << data;
			std::ofstream(dir / "lib.jai") << "import \"data.jai\";\nint boxv(Box b, int k) { return b.v + k; }\nint tagis(string s) { if (s == \"abc\") { return 1; } return 0; }\n"
			                                 "int probe() { return boxv(BX, 1) * 10 + tagis(TAG); }\n";
			std::ofstream(dir / "main.jai") << "import \"lib.jai\";\nto_string(probe());\n";
			auto run_file = [&](bool vm, std::string* listing) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e); register_host_math_fixture(*e);
				e->import_recheck_interval(0);
				if (listing) { e->disassembly_sink([listing](const std::string& l, const std::string&) { *listing += l; *listing += "\n"; }); }
				e->add_include_path(dir.string());
				std::string out;
				try { out = e->execute_file((dir / "main.jai").string()).as_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
				std::ofstream(dir / "data.jai") << "class Box { int v = 7; }\nint BX = 3;\nint TAG = 5;\n";
				try {
					out += "|" + e->execute("import \"data.jai\";\nvar r = \"\";\ntry { r = to_string(probe()); } catch (e) { r = e; }\nr;").as_string();
				} catch (const std::exception& ex) { out += std::string("|throw:") + ex.what(); }
				std::ofstream(dir / "data.jai") << data;
				return out;
			};
			std::string l;
			const std::string iv = run_file(false, nullptr), vv = run_file(true, &l);
			check_eq(iv, vv, "a class and a string global as by-value arguments: both backends, before and after the declaring unit changed their types");
			check_eq(std::string("81|"), vv.substr(0, 3), "the value before the reload: " + vv);
			check_true(l.find("-- inlined boxv") != std::string::npos, "boxv grafts with BX: " + l);
			check_true(l.find("-- inlined tagis") != std::string::npos, "tagis grafts with TAG: " + l);
		});
		test("reading_builtins_and_member_sizes_graft_in_a_body", [this]() {
			// JaiDOOM's lump_num (`LUMP_INDEX.has(name)` then the map's element) and Gator Quest's item_stat (`d.aff_code.size()` over a class
			// copied from a global's element): a reading builtin of a string, an array or a map the checker proved, on a name or a member chain,
			// with arguments that cannot be callables, writes nothing and runs no user code, so the body grafts; a writing builtin stays a call the graft makes
			const std::string prelude =
				"map<string, int> IDX = {\"STBAR\": 3, \"PLAYA\": 7};\n"
				"int lump_num(string name) { if (IDX.has(name)) { return IDX[name]; } return -1; }\n"
				"class Def { array<int> codes = [1, 4, 1]; array<int> vals = [2, 5, 7]; }\n"
				"array<Def> DEFS;\n"
				"class It { int base = 0; int pmag = 1; }\n"
				"int stat(It it, int code) { if (it.base < 0) { return 0; } int v = 0; Def d = DEFS[it.base]; for (int k = 0; k < d.codes.size(); ++k) { if (d.codes[k] == code) { v = v + d.vals[k]; } } d.vals[0] = 99; return v + it.pmag; }\n"
				"int tail_of(string s) { if (s.starts_with(\"PL\")) { string t = s.substr(2); return t.size(); } return s.find(\"B\"); }\n"
				"array<int> LOG;\n"
				"int logs(int v) { LOG.push(v); return LOG.size(); }\n"
				"int run() { array<It> worn; for (int i = 0; i < 2; ++i) { It t; t.base = i - 1; t.pmag = 3; worn.push(t); }\n"
				"  int s = lump_num(\"STBAR\") * 1000 + lump_num(\"NOPE\") * 100;\n"
				"  for (int i = 0; i < worn.size(); ++i) { switch (i) { case 9: if (s > 0) { fallthrough; } s = s - 1; } s = s + stat(worn[i], 1); }\n"
				"  return s + tail_of(\"PLAYA\") * 10000 + tail_of(\"STBAR\") * 100000 + logs(4) * 1000000; }\n"
				"Def dd; DEFS.push(dd);\n";
			const std::string run = "to_string(run()) + \"|\" + to_string(DEFS[0].vals[0]) + \"|\" + to_string(LOG.size());";
			both(prelude + run, "1232912|2|1", "reading builtins and member sizes in a body");
			both(prelude + run, "1232912|2|1", "reading builtins and member sizes in a body, proofs off", false);
			const std::string l = listing_run(prelude + "run();");
			check_true(l.find("-- inlined lump_num") != std::string::npos, "lump_num grafts with its map reads: " + l);
			check_true(l.find("-- inlined stat") != std::string::npos, "stat grafts with its member size: " + l);
			check_true(l.find("-- inlined tail_of") != std::string::npos, "tail_of grafts with its string reads: " + l);
			check_true(l.find("-- inlined logs") != std::string::npos, "logs grafts, its push a call the graft keeps: " + l);
		});
		test("top_level_loops_take_slots_and_close_as_function_bodies_do", [this]() {
			// a top-level loop's variable and its body's locals are program-frame slots (detail::program_slot_pass): the loop takes the typed
			// rows and closes as a cell block; globals stay in the environment, where functions read them at call time and a block local
			// that shadows one leaves it alone
			const std::string src =
				"int acc = 0; int x = 100; int readx() { return x; }\n"
				"for (int i = 0; i < 50; ++i) { int x = i & 3; acc += x; if (i == 10) { acc += readx(); } }\n"
				"array<int> data = [3, 1, 4, 1, 5]; int total = 0;\n"
				"for (auto v : data) { int doubled = v * 2; total += doubled; }\n"
				"{ int a = 7; { int b = a * 3; total += b; } }\n"
				"int w = 0; while (w < 5) { int sq = w * w; if (sq > 8) { break; } total += sq; ++w; }\n";
			both(src + "to_string(acc) + \"|\" + to_string(total) + \"|\" + to_string(x);", "173|54|100", "top-level block-scoped slots");
			const std::string l = listing_run(src + "acc;");
			check_true(l.find("-- cell loop") != std::string::npos && l.find("T_JLT_II") != std::string::npos, "the top-level loops close: " + l);
		});
		test("top_level_statements_that_could_see_a_block_name_keep_the_environment", [this]() {
			// a lambda, a nested function or a try in the statement keeps its names in the environment: a capture and a call see them there
			const std::string src =
				"int out = 0;\n"
				"for (int i = 0; i < 3; ++i) { auto f = [i]() { return i * 10; }; out += f(); }\n"
				"for (int j = 0; j < 2; ++j) { try { out += j; } catch (e) { } }\n"
				"string log = \"\";\n"
				"class D { string n; D(string s) { n = s; } ~D() { log += n; } }\n"
				"{ D a = D(\"a\"); { D b = D(\"b\"); } log += \".\"; }\n";
			both(src + "to_string(out) + \"|\" + log;", "31|b.a", "statements on the environment beside slotted ones");
		});
		test("a_global_container_read_at_a_global_index_keeps_its_own_lookup", [this]() {
			// the index's environment cell resolves through the read's index role (ip * 3 + 1): at the container's role the two names shared one
			// lookup-cache row, and the block read the container through the index's binding
			const std::string src =
				"array<int> A = [1, 2, 4]; int I = 2; var V = [5, \"x\", 7];\n"
				"int run() { int s = 0; for (int k = 0; k < 3; ++k) { s = s + A[I]; } return s; }\n"
				"string runv() { string out = \"\"; for (int k = 0; k < 2; ++k) { out = out + to_string(V[I]); } return out; }\n";
			both(src + "to_string(run()) + \"|\" + runv();", "12|77", "a global container at a global index");
		});
		test("bitwise_compound_stores_run_in_the_block", [this]() {
			// &= |= ^= <<= >>= on int locals take the typed rows and the block's arithmetic as += does (they were a bail per iteration);
			// on a var local they take the any-kind row, and a float target raises the general form's error
			const std::string src =
				"int run(int n) { int acc = 7; int m = 3; for (int i = 0; i < n; ++i) { acc ^= i; acc |= 1; acc &= 0xFFFF; m <<= 2; m >>= 1; acc += m & 15; } return acc * 100 + m; }\n"
				"var dyn(var n) { var acc = 5; for (var i = 0; i < n; ++i) { acc ^= i * 3; acc >>= 1; acc <<= 1; } return acc; }\n";
			both(src + "to_string(run(50)) + \"|\" + to_string(dyn(40));", "3377699720531172|44", "bitwise compounds");
			const std::string l = listing_run(src + "run(50);");
			check_true(l.find("-- cell loop") != std::string::npos && l.find("C_BAIL") == std::string::npos, "the loop closes without a bail: " + l);
		});
		test("a_push_onto_an_array_of_arrays_is_a_boundary_of_a_closed_loop", [this]() {
			// the bytecode app's NEWARR: `heap.push(zeros(n))` converts each value on the general form, so it is a boundary statement and the
			// dispatch loop around it closes (a push site the entry refused kept the whole loop on the general form)
			const std::string src =
				"array<int> zeros(int n) { array<int> r = []; for (int i = 0; i < n; ++i) { r.push(0); } return r; }\n"
				"int run() { array<array<int>> heap = []; int total = 0;\n"
				"  for (int i = 0; i < 40; ++i) { total = total + i * 3; if (i % 8 == 0) { heap.push(zeros(i % 5)); } }\n"
				"  return total * 100 + heap.size() * 10 + heap[4].size(); }\n";
			both(src + "to_string(run());", "234052", "a push of arrays inside a closed loop");
			const std::string l = listing_run(src + "run();");
			check_true(l.find("-- cell loop") != std::string::npos && l.find("boundaries") != std::string::npos, "the loop closes with the push a boundary: " + l);
		});
		test("a_container_taken_ahead_of_a_graft_is_a_guard_the_loop_closes_over", [this]() {
			// Gator Quest's find_target: mob_at's graft takes the global OCC's value ahead of occ_index's graft and reads its element; the value
			// lands by the container guard's always form and the read keeps OCC as its container (a block's site resolves it once at entry),
			// so neither the snapshot nor a later graft's parameter in the same temp (cheb) is a container rewritten inside the loop
			const std::string prelude =
				"array<int> OCC = [5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15];\n"
				"int occ_index(int x) { return x + 1; }\n"
				"int mob_at(int x) { return OCC[occ_index(x)] - 1; }\n"
				"int cheb(int a, int b, int c, int d) { return a + b + c + d; }\n"
				"int run(int n) { int s = 0; for (int i = 0; i < n; ++i) { int o = mob_at(i); int d = cheb(0, 0, i, o); s = s + d; } return s; }\n"
				"int rebind(int x) { OCC = [100, 200, 300]; return x; }\n"
				"int swapread() { return OCC[rebind(1)] + 0; }\n";   // the container is read before the index (C++17): the old array's element
			const std::string run = "to_string(run(8)) + \"|\" + to_string(swapread()) + \"|\" + to_string(OCC[1]);";
			both(prelude + run, "96|6|200", "a container snapshot and a later graft in one loop");
			both(prelude + run, "96|6|200", "a container snapshot and a later graft in one loop, proofs off", false);
			const std::string l = listing_run(prelude + "run(8);");
			check_true(l.find("-- inlined mob_at") != std::string::npos && l.find("-- inlined cheb") != std::string::npos, "both leaves graft: " + l);
			check_true(l.find("-- open loop") == std::string::npos, "the loop closes: " + l);
		});
		test("a_typed_operations_refusal_is_a_translation_gap", [this]() {
			// a loop refused at a typed instruction the translation has no row for (a typed element read in a kept call's argument zone)
			// is a translation gap, never an effect: the census reads the refusal by its class
			const std::string l = listing_run(
				"array<int> A = [30, 60, 0, 90];\n"
				"array<int> LOG;\n"
				"void deal(int m, int v) { switch (m) { default: LOG.push(v); } }\n"
				"int run() { for (int m = 0; m < A.size(); ++m) { deal(m, 1 + A[m] / 30); } return LOG.size(); }\n"
				"run();");
			const size_t at = l.find("== run ");
			const std::string rl = at == std::string::npos ? l : l.substr(at, l.find("\n== ", at + 1) - at);
			check_true(rl.find("-- open loop") != std::string::npos && rl.find("translation at") != std::string::npos, "the refusal is a translation gap: " + rl);
			check_true(rl.find("effect at") == std::string::npos, "no effect refusal: " + rl);
		});
		test("a_global_containers_computed_index_runs_in_the_block", [this]() {
			// Gator Quest's cave_automaton (`TILES[base + ny * ww + nx]`) and tile_at inlined into bfs_path: an element read of a program-scope
			// container whose index is an arithmetic tree takes the container guard's always form (the container read before the index, as the
			// stack form reads it) and the read lands as a typed load, so both loops run as blocks
			const std::string prelude =
				"array<int> T = [1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 1];\n"
				"int cave(int b, int w) { int s = 0; for (int y = 0; y < 3; ++y) { for (int x = 0; x < 3; ++x) { if (x < 0 || T[b + y * w + x] == 1) { s = s + 1; } } } return s; }\n"
				"array<int> OFF = [0, 8];\n"
				"array<int> WW = [4, 4];\n"
				"array<int> HH = [2, 2];\n"
				"int tile_at(int w, int x, int y) { if (x < 0 || y < 0 || x >= WW[w] || y >= HH[w]) { return 9; } return T[OFF[w] + y * WW[w] + x]; }\n"
				"int bfs() { int s = 0; for (int i = 0; i < 8; ++i) { s = s + tile_at(i % 2, i % 4, i % 3); } return s; }\n";
			const std::string run = "to_string(cave(1, 4)) + \"|\" + to_string(bfs());";
			both(prelude + run, "5|22", "a global container's computed index");
			both(prelude + run, "5|22", "a global container's computed index, proofs off", false);
			const std::string l = listing_run(prelude + "cave(1, 4); bfs();");
			for (const char* fn : { "== cave ", "== bfs " }) {
				const size_t at = l.find(fn);
				const std::string fl = at == std::string::npos ? std::string() : l.substr(at, l.find("\n== ", at + 1) - at);
				check_true(fl.find("-- cell loop") != std::string::npos && fl.find("-- open loop") == std::string::npos, std::string(fn) + "runs its loops as blocks: " + fl);
			}
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e);
				std::string got;
				try { got = e->execute(prelude + run).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(std::string("5|22"), got, "a miss at every " + std::to_string(every));
			}
		});
		test("a_global_container_is_read_before_its_computed_index", [this]() {
			// C++17 E1[E2]: the container is read before the index; an index that rebinds the global (a call, a user getter) leaves the read on
			// the array the name held before it, on both backends, whatever the index's shape
			const std::string prelude =
				"array<int> T = [10, 11, 12, 13, 14, 15, 16, 17];\n"
				"array<int> U = [90, 91, 92, 93, 94, 95, 96, 97];\n"
				"int swaps = 0;\n"
				"int swap_t() { swaps = swaps + 1; array<int> tmp = T; T = U; U = tmp; return 0; }\n"
				"int order(int b, int w) { int s = 0; for (int y = 0; y < 3; ++y) { s = s + T[b + y * w + swap_t()]; } return s; }\n"
				"class G { int n = 0; int _get_off() { n = n + 1; array<int> tmp = T; T = U; U = tmp; return 0; } }\n"
				"G g;\n"
				"int by_getter(int b, int w) { int s = 0; for (int y = 0; y < 3; ++y) { s = s + T[b + y * w + g.off]; } return s; }\n";
			const std::string run = "to_string(order(1, 2)) + \"|\" + to_string(by_getter(1, 2)) + \"|\" + to_string(swaps) + \"|\" + to_string(g.n);";
			both(prelude + run, "119|199|3|3", "the container before its index");
			both(prelude + run, "119|199|3|3", "the container before its index, proofs off", false);
		});
		test("a_graft_statements_container_guard_takes_a_computed_index", [this]() {
			// GLOOM's build_strip (`s.push(WALLT[tbase + tex_tone(...)])`): the container a graft statement reads ahead of the graft is the
			// guard's always form whatever the index's shape, so the read keeps the name as its container and the loop runs as a block; a graft
			// that rebinds the container leaves the read on the array the name held before it
			const std::string prelude =
				"array<int> WALLT = [3, 4, 5, 6, 7, 8, 9, 10];\n"
				"function tex_tone(int kind, int tx, int ty) -> int { if (kind == 1) { int tone = ((tx >> 2) + (ty >> 2)) & 1; if ((ty & 7) == 7) { tone = 2; } return tone; } return (tx >> 3) & 1; }\n"
				"function build_strip(int kind, int tx) -> array<int> { int tbase = kind * 3; array<int> s = []; for (int ty = 0; ty < 32; ++ty) { s.push(WALLT[tbase + tex_tone(kind, tx, ty)]); } return s; }\n"
				"function strip_sum(int tx) -> int { array<int> s = build_strip(1, tx); int t = 0; for (int i = 0; i < s.size(); ++i) { t = t + s[i]; } return t; }\n"
				"array<int> T2 = [1, 2, 3, 4];\n"
				"array<int> U2 = [50, 60, 70, 80];\n"
				"int swap_g(int k) { T2 = U2; return k; }\n"
				"int swapread() { return T2[1 + swap_g(0)] + 0; }\n";
			const std::string run = "to_string(strip_sum(5)) + \"|\" + to_string(strip_sum(12));";
			both(prelude + run, "", "a graft statement's container guard over a computed index");
			both(prelude + run, "", "a graft statement's container guard over a computed index, proofs off", false);
			both(prelude + "to_string(swapread()) + \"|\" + to_string(T2[1]);", "2|60", "a graft that rebinds the container leaves the read on the old array");
			const std::string l = listing_run(prelude + "strip_sum(5);");
			const size_t at = l.find("== build_strip ");
			const std::string bl = at == std::string::npos ? std::string() : l.substr(at, l.find("\n== ", at + 1) - at);
			check_true(bl.find("-- inlined tex_tone") != std::string::npos, "tex_tone grafts: " + bl);
			check_true(bl.find("-- cell loop") != std::string::npos && bl.find("-- open loop") == std::string::npos, "build_strip's loop runs as a block: " + bl);
			if (!engine::miss_injection_available()) { return; }
			const std::string want = run_on(false, prelude + run);
			for (uint32_t every : { 1u, 2u, 3u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e);
				std::string got;
				try { got = e->execute(prelude + run).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(want, got, "a miss at every " + std::to_string(every));
			}
		});
		test("an_enum_constant_argument_needs_no_capture", [this]() {
			// an enum's value read by name runs no user code (its name holds a map<string,int>), so an earlier argument needs no capture ahead of
			// it; a class member with a user getter is code, and the earlier argument keeps the value it had before the getter wrote it
			const std::string prelude =
				"enum Fx { fire, poison }\n"
				"array<int> X = [1, 2, 3, 4];\n"
				"array<int> LOG;\n"
				"void fx_at(int x, int k) { LOG.push(x * 10 + k); }\n"
				"int by_enum() { for (int m = 0; m < X.size(); ++m) { fx_at(X[m], Fx.poison); } return LOG.size(); }\n"
				"class H { int _get_fire() { X[0] = 7; return 0; } }\n"
				"H h;\n"
				"int by_getter() { fx_at(X[0], h.fire); return LOG[LOG.size() - 1]; }\n";
			const std::string run = "to_string(by_enum()) + \"|\" + to_string(by_getter()) + \"|\" + to_string(X[0]);";
			both(prelude + run, "4|10|7", "an enum argument and a getter argument");
			both(prelude + run, "4|10|7", "an enum argument and a getter argument, proofs off", false);
			const std::string l = listing_run(prelude + "by_enum(); by_getter();");
			auto section = [&](const char* fn) { const size_t at = l.find(fn); return at == std::string::npos ? std::string() : l.substr(at, l.find("\n== ", at + 1) - at); };
			check_true(section("== by_enum ").find("CAPTURE_ARG") == std::string::npos, "no capture ahead of an enum value: " + section("== by_enum "));
			check_true(section("== by_getter ").find("CAPTURE_ARG") != std::string::npos, "a capture ahead of a getter: " + section("== by_getter "));
			// the enum another unit of the program declares (Gator Quest's gq_enums.jai): the same constant
			namespace fs = std::filesystem;
			fs::path dir = fs::temp_directory_path() / "jai_enum_constant_import";
			fs::create_directories(dir);
			std::ofstream(dir / "enums.jai") << "enum Fx { fire, poison }\n";
			std::ofstream(dir / "main.jai") << "import \"enums.jai\";\narray<int> X = [1, 2, 3, 4];\narray<int> LOG;\n"
			                                  "void fx_at(int x, int k) { LOG.push(x * 10 + k); }\n"
			                                  "int by_enum() { for (int m = 0; m < X.size(); ++m) { fx_at(X[m], Fx.poison); } return LOG.size(); }\n"
			                                  "to_string(by_enum()) + \"|\" + to_string(LOG[3]);\n";
			auto run_file = [&](bool vm, std::string* listing) -> std::string {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e);
				e->import_recheck_interval(0);
				if (listing) { e->disassembly_sink([listing](const std::string& text, const std::string&) { *listing += text; *listing += "\n"; }); }
				e->add_include_path(dir.string());
				try { return e->execute_file((dir / "main.jai").string()).as_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			std::string il;
			const std::string iv = run_file(false, nullptr), vv = run_file(true, &il);
			check_eq(iv, vv, "an imported enum argument: both backends");
			check_eq(std::string("4|41"), vv, "the values");
			const size_t at = il.find("== by_enum ");
			const std::string bl = at == std::string::npos ? il : il.substr(at, il.find("\n== ", at + 1) - at);
			check_true(bl.find("CAPTURE_ARG") == std::string::npos, "no capture ahead of an imported enum value: " + bl);
		});
		test("a_global_containers_computed_index_store_runs_in_the_block", [this]() {
			// Gator Quest's set_tile inlined into fill_rect (`TILES[W_OFF[w] + y * W_W[w] + x] = t`): a store into a program-scope container whose
			// index is an arithmetic tree takes the value first, the container guard's always form, the index tree into a temp, and one fused store,
			// so the loop runs as a block with no bail at the store
			const std::string prelude =
				"array<int> T = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];\n"
				"array<int> OFF = [0, 8];\n"
				"array<int> WW = [4, 4];\n"
				"array<int> HH = [2, 2];\n"
				"bool in_world(int w, int x, int y) { return x >= 0 && y >= 0 && x < WW[w] && y < HH[w]; }\n"
				"void set_tile(int w, int x, int y, int t) { if (in_world(w, x, y)) { T[OFF[w] + y * WW[w] + x] = t; } }\n"
				"void fill_rect(int w, int x0, int y0, int x1, int y1, int t) { for (int y = y0; y <= y1; ++y) { for (int x = x0; x <= x1; ++x) { set_tile(w, x, y, t); } } }\n"
				"array<float> F = [0.0, 0.0, 0.0, 0.0];\n"
				"void put_f(int b) { for (int i = 0; i < 4; ++i) { F[OFF[0] + (i % 2) * 2 + i / 2 + b] = i * 1.5; } }\n"
				"string dump() { string s = \"\"; for (int i = 0; i < T.size(); ++i) { s = s + to_string(T[i]); } return s; }\n";
			const std::string run = "fill_rect(0, 0, 0, 3, 1, 2); fill_rect(1, 1, 0, 2, 1, 5); fill_rect(1, 3, 1, 5, 3, 7); put_f(0); dump() + \"|\" + to_string(F[1] * 10.0 + F[2]);";
			both(prelude + run, "2222222205500557|31.5", "a global container's computed-index store");
			both(prelude + run, "2222222205500557|31.5", "a global container's computed-index store, proofs off", false);
			const std::string l = listing_run(prelude + run);
			for (const char* fn : { "== fill_rect ", "== put_f " }) {
				const size_t at = l.find(fn);
				const std::string fl = at == std::string::npos ? std::string() : l.substr(at, l.find("\n== ", at + 1) - at);
				check_true(fl.find("-- cell loop") != std::string::npos && fl.find("-- open loop") == std::string::npos, std::string(fn) + "runs its loops as blocks: " + fl);
				check_true(fl.find("C_BAIL") == std::string::npos, std::string(fn) + "has no bail at the store: " + fl);
			}
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e);
				std::string got;
				try { got = e->execute(prelude + run).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(std::string("2222222205500557|31.5"), got, "a miss at every " + std::to_string(every));
			}
		});
		test("a_global_container_store_takes_its_value_before_the_container_and_the_index", [this]() {
			// C++17 E1[E2] = E3: the value first, then the container, then the index. A value that rebinds the global stores into the array the
			// name holds after it; a global value an index call rewrites stores what it held before the index ran; an index whose getter rebinds
			// the global stores into the array the name held before the index (the getter copied a new one in, so the store is lost)
			const std::string prelude =
				"array<int> T = [1, 2, 3, 4];\n"
				"array<int> U = [10, 20, 30, 40];\n"
				"array<int> OFF = [0];\n"
				"int swap_v(int v) { array<int> tmp = T; T = U; U = tmp; return v; }\n"
				"int G = 5;\n"
				"int bump() { G = G + 1; return 0; }\n"
				"class H { int n = 0; int _get_off() { n = n + 1; array<int> tmp = T; T = U; U = tmp; return 0; } }\n"
				"H h;\n"
				"string show() { return to_string(T[0]) + \",\" + to_string(T[1]) + \",\" + to_string(T[2]) + \",\" + to_string(T[3]) + \"/\" + to_string(U[0]) + \",\" + to_string(U[1]) + \",\" + to_string(U[2]) + \",\" + to_string(U[3]); }\n"
				"void by_value(int w) { for (int y = 0; y < 2; ++y) { T[OFF[0] + y * w] = swap_v(7 + y); } }\n"
				"void by_global(int w) { for (int y = 0; y < 2; ++y) { T[OFF[0] + y * w + bump()] = G; } }\n"
				"void by_getter(int w) { for (int y = 0; y < 2; ++y) { T[OFF[0] + y * w + h.off] = 50 + y; } }\n";
			const std::string run = "by_value(1); string a = show(); by_global(1); string b = show(); by_getter(1); a + \"|\" + b + \"|\" + show() + \"|\" + to_string(G) + \"|\" + to_string(h.n);";
			const std::string want = "1,8,3,4/7,20,30,40|5,6,3,4/7,20,30,40|5,6,3,4/7,20,30,40|7|2";
			both(prelude + run, want, "the value, the container, the index");
			both(prelude + run, want, "the value, the container, the index, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e);
				std::string got;
				try { got = e->execute(prelude + run).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(want, got, "a miss at every " + std::to_string(every));
			}
		});
		test("a_loop_whose_every_path_bails_stays_general", [this]() {
			// the rule over the region itself (vm/region_ir.hpp bail_dominates_back_edge), so no translation gap is pinned: a loop whose
			// every path from the head to the back edge passes a bail, with less work ahead of it than a park and a resume, refuses at its
			// first bail; a path that avoids the bails, real work ahead of them, or a straight-line region keeps the block
			using vir = jai::vm::ir; using vop = jai::vm::ir_op; using vregion = jai::vm::region_ir;
			auto op_of = [](vir kind, uint32_t target = 0) { vop o; o.kind = kind; o.target = target; return o; };
			auto region = [](std::vector<vop> ops) { vregion r; r.first = 10; r.head = 10; r.back = 10 + static_cast<uint32_t>(ops.size()); r.ops = std::move(ops); return r; };
			uint32_t at = 0;
			const vregion both_arms = region({ op_of(vir::cmp, 13), op_of(vir::bail), op_of(vir::skip), op_of(vir::bail), op_of(vir::skip), op_of(vir::skip) });
			check_true(jai::vm::bail_dominates_back_edge(both_arms, at), "both arms bail with one compare ahead: refused");
			check_eq(static_cast<uint32_t>(11), at, "at the first bail");
			const vregion one_arm = region({ op_of(vir::cmp, 13), op_of(vir::bail), op_of(vir::skip), op_of(vir::arith), op_of(vir::skip), op_of(vir::skip) });
			check_false(jai::vm::bail_dominates_back_edge(one_arm, at), "an arm that reaches the back edge keeps the block");
			const vregion behind_work = region({ op_of(vir::arith), op_of(vir::arith), op_of(vir::cmp, 15), op_of(vir::bail), op_of(vir::skip), op_of(vir::bail) });
			check_false(jai::vm::bail_dominates_back_edge(behind_work, at), "bails behind real work keep the block");
			const vregion one_op_ahead = region({ op_of(vir::arith), op_of(vir::bail), op_of(vir::skip) });
			check_true(jai::vm::bail_dominates_back_edge(one_op_ahead, at), "a bail on the main path behind one operation: refused");
			const vregion head_only = region({ [] { vop o; o.kind = vir::cmp; o.head = true; o.target = 99; return o; }(), op_of(vir::arith), op_of(vir::bail) });
			check_true(jai::vm::bail_dominates_back_edge(head_only, at), "the head compare and one operation ahead: refused (the head is the loop's own)");
			vregion straight = both_arms; straight.straight = true;
			check_false(jai::vm::bail_dominates_back_edge(straight, at), "a straight-line region is not a loop");
			const vregion no_bail = region({ op_of(vir::cmp, 12), op_of(vir::arith), op_of(vir::arith) });
			check_false(jai::vm::bail_dominates_back_edge(no_bail, at), "no bail");
		});		test("a_conditional_assignments_environment_arms_run_in_the_block", [this]() {
			// Gator Quest's gen_bayou and gen_gloomfen: `t = h < 220 ? T_CYPRESS : (h < 340 ? T_REEDS : ...)` inside an if arm stores a
			// program-scope name into a typed local in each arm; the move takes the name's environment cell, so the loop runs whole as a block
			// (no bail, no boundary). A call door that rewrites a name an arm reads leaves the arm reading the new value
			const std::string prelude =
				"int TA = 1; int TB = 2; int TC = 3; int TD = 4; int TE = 5; int TF = 6; int TG = 7;\n"
				"array<int> TILES = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];\n"
				"int hashv(int x, int y) { return ((x * 73856093) ^ (y * 19349663)) & 1023; }\n"
				"void gen(int ww, int hh) { for (int y = 0; y < hh; ++y) { for (int x = 0; x < ww; ++x) {\n"
				"  int e = hashv(x + 1, y + 7); int h = hashv(x, y); int t = TA;\n"
				"  if (e > 560) { t = h < 220 ? TB : (h < 340 ? TC : (h < 560 ? TD : (h < 640 ? TE : TF))); }\n"
				"  else if (e > 500) { t = h < 300 ? TC : TD; }\n"
				"  else { t = h < 70 ? TG : (h < 90 ? TB : TA); }\n"
				"  TILES[y * ww + x] = t; } } }\n"
				"array<int> LOG;\n"
				"int bump_tb(int v) { TB = TB + 10; LOG.push(v); return v; }\n"
				"int rewrite(int n) { int s = 0; for (int i = 0; i < n; ++i) { int e = bump_tb(i); int t = TA; if (e > 1) { t = e < 4 ? TB : TC; } s = s + t; } return s; }\n"
				"string dump() { string s = \"\"; for (int i = 0; i < TILES.size(); ++i) { s = s + to_string(TILES[i]); } return s; }\n";
			const std::string run = "gen(8, 4); dump() + \"|\" + to_string(rewrite(6)) + \"|\" + to_string(TB);";
			const std::string want = run_on(false, prelude + run);
			both(prelude + run, want, "environment arms");
			both(prelude + run, want, "environment arms, proofs off", false);
			const std::string l = listing_run(prelude + run);
			auto section = [&](const char* fn) { const size_t at = l.find(fn); return at == std::string::npos ? std::string() : l.substr(at, l.find("\n== ", at + 1) - at); };
			const std::string gl = section("== gen ");
			check_true(gl.find("-- cell loop") != std::string::npos && gl.find("-- open loop") == std::string::npos && gl.find("C_BAIL") == std::string::npos && gl.find("boundaries") == std::string::npos, "gen runs whole as blocks: " + gl);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e);
				std::string got;
				try { got = e->execute(prelude + run).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(want, got, "a miss at every " + std::to_string(every));
			}
		});
		test("an_element_compound_runs_in_the_block", [this]() {
			// demoreel's cloth relaxation (`px[a] -= ddx * k; px[a + cww + 1] += ddx * k;` over `var& px = pxs`): an element compound in
			// statement position takes its value, container and index as operands and the block reads, computes and writes the element in place
			const std::string prelude =
				"class Cloth { array<float> pxs = [0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0]; array<int> cnt = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0];\n"
				"  void relax(int w) { var& px = pxs; var& cn = cnt; for (int a = 0; a < 7; ++a) { float d = px[a + 1] - px[a]; float k = 0.25 / (d * d + 1.0);\n"
				"    px[a] -= d * k; px[a + 1] += d * k; px[a + w + 1] *= 0.5 + k; cn[a] += 3; cn[a + 1] -= 1; cn[a + 2] *= 2; } }\n"
				"  void divs() { var& cn = cnt; for (int a = 0; a < 9; ++a) { cn[a] /= 2; cn[a] %= 7; pxs[a] /= 2.0; pxs[a] += cn[a]; cn[a]++; } } }\n"
				"string dump(Cloth c) { string s = \"\"; for (int i = 0; i < 10; ++i) { s = s + to_string(c.cnt[i]) + \":\" + to_string(math::itrunc(c.pxs[i] * 1000.0)) + \" \"; } return s; }\n"
				"int local_sum(int n) { array<int> v = [1, 2, 3, 4, 5, 6, 7, 8]; for (int i = 0; i < n; ++i) { v[i % 8] += i; v[(i + 3) % 8] -= 1; } int s = 0; for (int i = 0; i < 8; ++i) { s = s * 7 + v[i]; } return s; }\n"
				"string untyped(int n) { var h = [1.5, 2.5, 3.5, 4.5, 5, 6]; for (int i = 0; i < n; ++i) { h[i % 4] += 0.25; h[4 + i % 2] += 1; h[4] *= 1; if (i == 3) { h[5] += 0.5; } } return to_string(h[0] + h[1] + h[2] + h[3]) + \",\" + to_string(h[4]) + \",\" + to_string(h[5]) + \",\" + type_of(h[4]) + \",\" + type_of(h[5]); }\n";
			const std::string run = "Cloth c; c.relax(2); c.relax(1); c.divs(); dump(c) + \"|\" + to_string(local_sum(40)) + \"|\" + untyped(12);";
			const std::string want = run_on(false, prelude + run);
			both(prelude + run, want, "element compounds");
			both(prelude + run, want, "element compounds, proofs off", false);
			const std::string l = listing_run(prelude + run);
			auto section = [&](const char* fn) { const size_t at = l.find(fn); return at == std::string::npos ? std::string() : l.substr(at, l.find("\n== ", at + 1) - at); };
			for (const char* fn : { "== relax ", "== divs ", "== local_sum ", "== untyped " }) {
				const std::string fl = section(fn);
				check_true(fl.find("INDEX_COMPOUND_OPERAND") != std::string::npos && fl.find("INDEX_COMPOUND_FUSED") == std::string::npos, std::string(fn) + "takes the operand form: " + fl);
				check_true(fl.find("-- cell loop") != std::string::npos && fl.find("-- open loop") == std::string::npos && fl.find("C_BAIL") == std::string::npos, std::string(fn) + "runs its loops as blocks: " + fl);
			}
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e);
				std::string got;
				try { got = e->execute(prelude + run).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(want, got, "a miss at every " + std::to_string(every));
			}
		});
		test("an_element_compound_takes_its_value_before_the_container_and_the_index", [this]() {
			// C++17 E1[E2] op= E3: the value, then the container, then the index, then the read-modify-write; the interpreter is the oracle for a
			// value that rebinds the container, an index call that rewrites the value's global, a getter in the index; errors keep their texts
			const std::string prelude =
				"array<int> T = [1, 2, 3, 4];\n"
				"array<int> U = [10, 20, 30, 40];\n"
				"array<int> OFF = [0];\n"
				"int swap_v(int v) { array<int> tmp = T; T = U; U = tmp; return v; }\n"
				"int G = 5;\n"
				"int bump() { G = G + 1; return 0; }\n"
				"class H { int n = 0; int _get_off() { n = n + 1; array<int> tmp = T; T = U; U = tmp; return 0; } }\n"
				"H h;\n"
				"string show() { return to_string(T[0]) + \",\" + to_string(T[1]) + \",\" + to_string(T[2]) + \",\" + to_string(T[3]) + \"/\" + to_string(U[0]) + \",\" + to_string(U[1]) + \",\" + to_string(U[2]) + \",\" + to_string(U[3]); }\n"
				"void by_value(int w) { for (int y = 0; y < 2; ++y) { T[OFF[0] + y * w] += swap_v(7 + y); } }\n"
				"void by_global(int w) { for (int y = 0; y < 2; ++y) { T[OFF[0] + y * w + bump()] -= G; } }\n"
				"void by_getter(int w) { for (int y = 0; y < 2; ++y) { T[OFF[0] + y * w + h.off] *= 3; } }\n"
				"string zero() { array<int> z = [4, 5]; try { for (int i = 0; i < 2; ++i) { z[i] /= i - 1 + 1; } } catch (e) { return \"E:\" + e; } return \"none\"; }\n"
				"string ovf() { array<int> z = [4611686018427387904, 5]; try { for (int i = 0; i < 2; ++i) { z[i] *= 4; } } catch (e) { return \"E:\" + e; } return \"none\"; }\n";
			const std::string run = "by_value(1); string a = show(); by_global(1); string b = show(); by_getter(1); a + \"|\" + b + \"|\" + show() + \"|\" + to_string(G) + \"|\" + to_string(h.n) + \"|\" + zero() + \"|\" + ovf();";
			const std::string want = run_on(false, prelude + run);
			both(prelude + run, want, "the value, the container, the index");
			both(prelude + run, want, "the value, the container, the index, proofs off", false);
			check_true(want.find("E:Division by zero") != std::string::npos && want.find("overflow") != std::string::npos, "the errors keep their texts: " + want);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e);
				std::string got;
				try { got = e->execute(prelude + run).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(want, got, "a miss at every " + std::to_string(every));
			}
		});
		test("a_leaf_in_a_loop_left_open_grafts_by_the_rules_outside_a_loop", [this]() {
			// Gator Quest's walk_at -> tile_now inside a loop the region compiler refuses: a loop the first compile refused holds no
			// block to keep, so a leaf whose body the block could not land (a declaration from an element's field) grafts there in the second
			// compile; in a loop that closes over the call the second compile grafts it too (fewer calls from the block), and both loops keep their verdicts
			const std::string prelude =
				"class D { int after = 0; }\n"
				"array<D> DEFS;\n"
				"array<int> XS = [0, 1, 2, 1, 0, 2];\n"
				"int after_of(int t) { int a = DEFS[t].after; return a + t; }\n"
				// a switch in the body is a statement no block takes (an effect at CASE_EQ), so the loop stays refused
				"int open_sum() { int s = 0; for (int i = 0; i < XS.size(); ++i) { switch (i) { case 9: if (s > 0) { fallthrough; } s = s - 1; } s = s + after_of(XS[i]); } return s; }\n"
				"int closed_sum(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + after_of(i % 3); } return s; }\n"
				"for (int k = 0; k < 3; ++k) { D d; d.after = k * 10; DEFS.push(d); }\n";
			both(prelude + "to_string(open_sum()) + \"|\" + to_string(closed_sum(6));", "66|66", "a leaf in an open loop and in a closed one");
			both(prelude + "to_string(open_sum()) + \"|\" + to_string(closed_sum(6));", "66|66", "a leaf in an open loop and in a closed one, proofs off", false);
			const std::string l = listing_run(prelude + "open_sum(); closed_sum(6);");
			check_true(l.find("-- inlined after_of") != std::string::npos, "after_of grafts inside the open loop: " + l);
			const size_t cs = l.find("== closed_sum");
			check_true(cs != std::string::npos && l.find("-- cell loop", cs) != std::string::npos && l.find("-- inlined after_of", cs) != std::string::npos, "after_of grafts inside the closed loop, which stays closed: " + l);
		});
		test("a_particle_step_through_an_alias_grafts_with_its_ternary", [this]() {
			// gloom_particle_step's shape, a var& parameter bound to a range-for alias, member reads and stores through it and the
			// ternary store `life = life < 4 ? life : 4`, which lands as the condition's jumps and one store per arm; the leaf grafts inside the
			// range-for and the values hold on both backends, proofs on and off
			const std::string prelude =
				"class P { int kind = 1; float x = 0.0; float vx = 0.1; int life = 10; }\n"
				"void step(var& p) { int kind = p.kind; if (kind == 0) { return; } float x = p.x; float vx = p.vx; int life = p.life; life = life - 1;\n"
				"  if (life <= 0) { p.kind = 0; p.x = 0.0; p.life = 0; return; } if (kind == 1) { vx = vx * 0.9; } else { vx = vx * 0.5; } x = x + vx * 0.5;\n"
				"  if (x > 2.0) { x = 2.0; if (kind == 1 || kind == 2) { life = life < 4 ? life : 4; } } p.x = x; p.vx = vx; p.life = life; }\n"
				"var pool = []; for (int i = 0; i < 8; ++i) { auto q = P(); q.kind = i % 3; q.x = i * 0.3; pool.push(q); }\n"
				"var tick = function(int n) { float acc = 0.0; for (int t = 0; t < n; ++t) { for (auto& p : pool) { step(p); } } for (auto& p : pool) { acc = acc + p.x + p.life; } return acc; };\n";
			both(prelude + "to_string(tick(12));", "32.7", "a particle step through an alias");
			both(prelude + "to_string(tick(12));", "32.7", "a particle step through an alias, proofs off", false);
			const std::string l = listing_run(prelude + "tick(12);");
			check_true(l.find("-- inlined step") != std::string::npos, "step grafts inside the range-for with its ternary store: " + l);
		});
		test("the_listing_names_each_kept_call_with_its_rule", [this]() {
			// a call the inliner keeps is named in the listing with the rule that kept it: a var local into an int parameter (an
			// unproven argument), a leaf as a method call's argument, and a leaf returning a var element inside a loop (a return the block cannot
			// land: the element's kind is unknown until run time)
			const std::string l = listing_run(
				"int add1(int a) { return a + 1; }\n"
				"var G = [3, 4, 5];\n"
				"int at(int i) { return G[i]; }\n"
				"int run() { var v = 3; int s = add1(v); var acc = []; acc.push(add1(2)); for (int i = 0; i < 3; ++i) { s = s + at(i); } return s; }\n"
				"run();");
			check_true(l.find("-- call kept add1 line 4: an unproven argument") != std::string::npos, "an unproven argument keeps the call: " + l);
			check_true(l.find("-- call kept add1 line 4: a method call's argument") != std::string::npos, "a method call's argument keeps the call: " + l);
			check_true(l.find("-- call kept at line 4: a return the block cannot land") != std::string::npos || l.find("-- inlined at ") != std::string::npos, "a return the block cannot land keeps the call, or the second compile grafts it: " + l);
		});
		test("a_graft_after_a_continue_closes_the_loop_without_a_bail", [this]() {
			// draw_seg's shape, `if (done[x] != 0) { continue; }` then a leaf call, then a statement whose float temp takes a slot the
			// graft declared as an int; the graft declares no default for its result (a bare DECL_VAR is no block instruction, and inside the
			// conditional span after the continue it made the graft a planned bail) and its declarations are compiler values to the walk (a
			// later temp may take their slots with another kind), so the loop's copy holds the graft and runs it as a block
			const std::string prelude =
				"array<int> done = [0, 1, 0, 0, 1, 0, 0, 0];\n"
				"int light_band(int l, int d) { int b = l - d / 8; if (b < 0) { return 0; } if (b > 31) { return 31; } return b; }\n"
				"var run = function(int n) { int s = 0; for (int x = 0; x < n; ++x) { if (done[x] != 0) { continue; } int band = light_band(x * 9, x); s = s + band; int t2 = math::itrunc(band * 0.5); s = s + t2; } return s; };\n";
			both(prelude + "to_string(run(8));", "205", "a graft after a continue");
			const std::string l = listing_run(prelude + "run(8);");
			check_true(l.find("-- inlined light_band") != std::string::npos, "light_band is inlined: " + l);
			check_true(l.find("-- cell loop") != std::string::npos, "the loop closes: " + l);
			check_true(l.find("bail->") == std::string::npos && l.find("C_BAIL") == std::string::npos, "no bail inside the loop: " + l);
		});
		test("a_callee_with_a_loop_grafts_into_a_column_loop", [this]() {
			// draw_seg's column shape, a callee whose body is a loop with a continue and a break, taking its texture and the frame
			// buffer as typed references bound to the caller's aliases: both calls graft, the grafted loops and the column loop close
			const std::string prelude =
				"array<int> PIX = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];\n"
				"array<array<int>> TEX = [[1, 2, 3, 4], [5, 6, 7, 8]];\n"
				"void col(int x, int y0, int y1, array<int>& tp, int k, array<int>& pix) {\n"
				"\tif (y0 < 0) { y0 = 0; }\n"
				"\tfor (int y = y0; y <= y1; ++y) {\n"
				"\t\tif (y == 2) { continue; }\n"
				"\t\tif (y > 5) { break; }\n"
				"\t\tpix[y * 2 + x] = tp[(y + k) & 3] + x;\n"
				"\t}\n"
				"}\n"
				"int run() {\n"
				"\tarray<int>& pix = PIX;\n"
				"\tarray<int>& t0 = TEX[0];\n"
				"\tarray<int>& t1 = TEX[1];\n"
				"\tint s = 0;\n"
				"\tfor (int x = 0; x < 2; ++x) {\n"
				"\t\tif (x == 0) { col(x, -1, 7, t0, 1, pix); } else { col(x, 0, 7, t1, 2, pix); }\n"
				"\t\ts = s + x;\n"
				"\t}\n"
				"\tfor (int i = 0; i < 16; ++i) { s = s * 3 + PIX[i]; s = s & 1048575; }\n"
				"\treturn s;\n"
				"}\n";
			both(prelude + "to_string(run());", "113835", "a callee with a loop grafted into a column loop");
			const std::string l = listing_run(prelude + "run();");
			size_t grafts = 0; for (size_t at = l.find("-- inlined col"); at != std::string::npos; at = l.find("-- inlined col", at + 1)) { ++grafts; }
			size_t loops = 0; for (size_t at = l.find("-- cell loop"); at != std::string::npos; at = l.find("-- cell loop", at + 1)) { ++loops; }
			check_eq(size_t(2), grafts, "both calls graft: " + l);
			check_eq(size_t(4), loops, "the grafted loops, the column loop and the fold loop close: " + l);
		});
		test("a_return_inside_a_callee_loop_keeps_the_call", [this]() {
			// a return from inside the callee's own loop would leave the loop's state behind the graft's join, so the call stays
			const std::string prelude =
				"int find(array<int>& a, int v) { for (int i = 0; i < 4; ++i) { if (a[i] == v) { return i; } } return -1; }\n"
				"int run() { array<int> a = [4, 5, 6, 7]; int s = 0; for (int k = 0; k < 8; ++k) { s = s + find(a, k); } return s; }\n";
			both(prelude + "to_string(run());", "2", "a return inside a callee loop");
			const std::string l = listing_run(prelude + "run();");
			check_true(l.find("-- call kept find line 2: a return inside a loop") != std::string::npos, "the call stays with its rule: " + l);
		});
		test("a_typed_reference_parameter_grafts_as_the_callers_variable", [this]() {
			// an int& parameter bound to an int local is that local (an int& binds only an int): an int
			// stores into it, a float raises the reference's text with the earlier stores kept, grafted or not
			const std::string prelude = "[[warn_mode]]\n"
				"void add2(int& a, int d) { a = a + d * 2; }\n"
				"void set37(int& a) { a = 3.7; }\n"
				"int run() { int u = 1; for (int k = 0; k < 2; ++k) { add2(u, k); } return u; }\n"
				"string refuse() { int u = 1; try { for (int k = 0; k < 2; ++k) { add2(u, 3); set37(u); } } catch (err) { return err + \"|\" + to_string(u); } return to_string(u); }\n";
			both(prelude + "to_string(run()) + \" \" + refuse();", "3 Cannot assign float to 'a' (declared int&)|7", "a store through a typed reference parameter");
			const std::string l = listing_run(prelude + "run();");
			check_true(l.find("-- inlined add2") != std::string::npos, "the typed reference parameter grafts: " + l);
		});
		test("a_reference_argument_that_is_not_a_local_keeps_the_call", [this]() {
			// a var& or typed reference parameter bound to a field or a method name binds through the caller's member rules;
			// the call stays, so a method name raises the binder's text on both backends
			const std::string prelude =
				"void by_ref(var& p) { p = 1; }\n"
				"void by_int(int& p) { p = 2; }\n"
				"class W { int fld = 5; int f() { return 1; } int fields() { by_ref(fld); int a = fld; by_int(fld); return a * 10 + fld; } }\n"
				"W w = W();\n";
			both(prelude + "to_string(w.fields());", "12", "a field bound by reference");
			// a method name handed to a reference parameter is a method read without a call: the strict checker refuses it at load
			const std::string named = prelude + "class N { int f() { return 1; } void named() { by_ref(f); } }\n0;";
			both(named, "", "a method name bound by reference");
			const std::string refused = run_on(true, named, true);
			check_true(refused.find("'f' is a method of N; call it with f() or bind it with reflect::bind(this, \"f\")") != std::string::npos, refused);
			const std::string l = listing_run(prelude + "w.fields();");
			check_true(l.find("-- call kept by_ref line 3: a reference argument that is not a local") != std::string::npos, "the call stays with its rule: " + l);
		});
		test("a_loop_of_more_than_128_cells_closes", [this]() {
			// the cell mask holds 256 cells; a loop over 140 locals runs as one block
			std::string src = "int run() { ";
			for (int i = 0; i < 140; ++i) { src += "int v" + std::to_string(i) + " = " + std::to_string(i) + "; "; }
			src += "\n\tfor (int k = 0; k < 3; ++k) { ";
			for (int i = 0; i < 140; ++i) { src += "v" + std::to_string(i) + " = v" + std::to_string(i) + " + " + std::to_string(i % 5 + 1) + "; "; }
			src += "}\n\treturn ";
			for (int i = 0; i < 140; ++i) { src += (i ? " + v" : "v") + std::to_string(i); }
			src += "; }\n";
			both(src + "to_string(run());", "10990", "a loop of 140 cells");
			const std::string l = listing_run(src + "run();");
			check_true(l.find("-- cell loop") != std::string::npos, "the loop closes: " + l.substr(0, 400));
		});
		test("a_miss_inside_the_argument_zone_rebuilds_every_argument", [this]() {
			// an element argument the block landed, then a later element argument the cells decline (a string): the general form re-runs the
			// argument zone from its first instruction, since argument values are never flushed
			const std::string prelude =
				"var OUT = \"\";\n"
				"function g2(int a, string g) -> void { OUT = OUT + to_string(a) + g + \";\"; }\n"
				"function g3(int a, int b, string g) -> void { OUT = OUT + to_string(a + b) + g + \";\"; }\n"
				"var LM = [[26, \"a\"], [22, \"b\"], [7, \"c\"]];\n"
				"var LN = [[26, 5, \"a\"], [22, 6, \"b\"], [1, 2, \"c\"]];\n"
				"var A = [26, \"a\", 22, \"b\", 3, \"c\"];\n"
				"function by_ref() -> void { for (auto& lm : LM) { g2(lm[0], lm[1]); } }\n"
				"function three() -> void { for (auto& ln : LN) { g3(ln[0], ln[1], ln[2]); } }\n"
				"function flat() -> void { for (int i = 0; i < 6; i += 2) { g2(A[i], A[i + 1]); } }\n";
			both(prelude + "by_ref(); three(); flat(); OUT;", "26a;22b;7c;31a;28b;3c;26a;22b;3c;", "element arguments with a declined second read");
			both(prelude + "by_ref(); three(); flat(); OUT;", "26a;22b;7c;31a;28b;3c;26a;22b;3c;", "element arguments, proofs off", false);
		});
		test("a_range_for_variable_binds_each_element_as_it_is", [this]() {
			// each iteration declares the loop variable afresh: an int after a float stays an int, a float after an int stays a float
			const std::string src =
				"var out = \"\";\n"
				"function g(a) { out = out + to_string(a) + \";\"; }\n"
				"function d(var xs) { for (auto e : xs) { g(e); } for (var e : xs) { g(e); } }\n"
				"d([1, 7.5, 1]); d([true, 3.0, 1, 2.5, -3]); out;";
			both(src, "1;7.5;1;1;7.5;1;true;3.0;1;2.5;-3;true;3.0;1;2.5;-3;", "range-for kinds");
			both(src, "1;7.5;1;1;7.5;1;true;3.0;1;2.5;-3;true;3.0;1;2.5;-3;", "range-for kinds, proofs off", false);
		});
		test("a_constant_stored_into_a_var_element_belongs_to_the_engine", [this]() {
			// the stored element copies deep afterwards (slice clones each element), which needs its engine
			const std::string src =
				"var v7 = [4, 5, 12]; v7[1] = 11;\n"
				"function f() -> string { var v = [4, 5, 12]; v[1] = 11; return to_json(array::slice(v, 0, 2)); }\n"
				"function g() -> string { var v = [0, 0, 0, 0]; for (int i = 0; i < 4; ++i) { v[i] = 7; v[i] = 2.5; } return to_json(array::slice(v, 1, 3)); }\n"
				"to_json(array::slice(v7, 0, 2)) + f() + g();";
			both(src, "[4,11][4,11][2.5,2.5]", "constant element stores");
			both(src, "[4,11][4,11][2.5,2.5]", "constant element stores, proofs off", false);
		});
		test("a_computed_index_beside_a_lifted_partner_compiles_without_typed_lowering", [this]() {
			const std::string src = "var A = [1, 2, 3, 4];\nfunction d() -> int { int i = 0; return (i + A[i]) + A[i + 2]; }\nto_string(d());";
			both(src, "4", "computed index, proofs off", false);
			both(src, "4", "computed index");
		});
		test("a_bail_inside_a_statement_flushes_its_spills_and_graft_values", [this]() {
			// the first itr grafts behind the spill of m[i], the second stays a call (in a ternary's arm) and the statement bails inside the if
			// after the graft's join; the spill and the graft's result live only in cells, so the bail flushes them
			const std::string prelude =
				"function itr(float v) -> int { return math::itrunc(v); }\n"
				"var q = 4;\n"
				"function run() -> string { array<int> m = [10, 20, 30, 40]; string out = \"\";\n"
				"  for (int i = 0; i < 4; ++i) { float d = i * 1.5; if (d > 0.5) { int c = m[i] + itr(d) * q + (q > 2 ? itr(d) : 0); out += to_string(c) + \" \"; } }\n"
				"  return out; }\n";
			both(prelude + "run();", "25 45 60 ", "a statement bailing after its graft");
			both(prelude + "run();", "25 45 60 ", "a statement bailing after its graft, proofs off", false);
			const std::string l = listing_run(prelude + "run();");
			check_true(l.find("-- inlined itr") != std::string::npos && l.find("bail->") != std::string::npos, "the graft and the bail are in the loop: " + l.substr(0, 400));
		});

		// a bare call to a stdlib math builtin lowers to the math temp while the name binds the registered function
		test("stdlib_math_calls_lower_to_math_temps", [this]() {
			const std::string src = "function f(float x, float y) -> float { float a = host_sqrt(x * x + y * y); float b = host_sin(x) + host_cos(y); return a + b + host_floor(y * 0.5) + host_ceil(x) + host_tan(0.25) + host_degrees(host_radians(x)); }\n"
				"to_string(f(3.0, 4.0)) + \",\" + to_string(f(1, 2));";
			both(src, "", "the aliases");
			check_true(run_on(true, src).rfind("throw:", 0) != 0, "runs: " + run_on(true, src));
			both(src, "", "the aliases, proofs off", false);
			const std::string all = listing_run(src);
			const size_t fbeg = all.find("== f ");
			const std::string l = fbeg == std::string::npos ? all : all.substr(fbeg);
			size_t temps = 0, guards = 0;
			for (size_t at = l.find("MATH_TEMP"); at != std::string::npos; at = l.find("MATH_TEMP", at + 9)) { ++temps; }
			for (size_t at = l.find("MATH_GUARD"); at != std::string::npos; at = l.find("MATH_GUARD", at + 10)) { ++guards; }
			check_true(temps == 8 && guards >= 3, "every alias lands in a math temp, a computed argument behind a guard: " + l);
			check_true(l.find("PROBE_CALLEE") == std::string::npos, "no alias call is a probe: " + l);
		});
		test("host_math_alias_numbers_match_the_builtin", [this]() {
			const std::string src = "function r(var v) -> string { return to_string(host_sqrt(v)) + \":\" + type_of(host_sqrt(v)) + \" \" + to_string(host_floor(v)) + \" \" + to_string(host_sin(v)); }\n"
				"function k(int i) -> string { return to_string(host_sqrt(i)) + \" \" + to_string(host_cos(i)); }\n"
				"r(4) + \"|\" + r(2.25) + \"|\" + r(-1.5) + \"|\" + r(9007199254740993) + \"|\" + k(7) + \"|\" + k(-9223372036854775807) + \"|\" + to_string(host_sqrt(-1.0) == host_sqrt(-1.0));";
			both(src, "", "int and float arguments");
			check_true(run_on(true, src).rfind("2.0:float 4.0 ", 0) == 0, "runs: " + run_on(true, src));
			both(src, "", "int and float arguments, proofs off", false);
		});
		test("host_math_alias_non_numeric_arguments_take_the_builtin", [this]() {
			const std::string src = "class F { float to_float() { return 16.0; } }\n"
				"function g(var v) -> string { try { var r = host_sqrt(v); return to_string(r); } catch (e) { return \"E:\" + e; } }\n"
				"function h(var v) -> string { try { float r = host_sin(v * 1) + 1.0; return to_string(r); } catch (e) { return \"E:\" + e; } }\n"
				"g(true) + \"|\" + g(\"x\") + \"|\" + g(null) + \"|\" + g([1]) + \"|\" + g(F()) + \"|\" + h(\"s\") + \"|\" + h(false) + \"|\" + h(2);";
			both(src, "", "non-numeric arguments");
			check_true(run_on(true, src).rfind("E:Cannot convert bool to float|E:No overload of 'host_sqrt' takes (string)|", 0) == 0, "the builtin's own texts: " + run_on(true, src));
		});
		test("host_math_alias_follows_a_rebinding", [this]() {
			// rebound between calls, from inside a loop, from inside its own argument (the callee resolved first), to a
			// non-callable and to a function taking its parameter by reference
			const std::string src = "function f(float x) -> float { return host_sqrt(x * 2.0) + host_sqrt(x); }\n"
				"function twice(float x) -> float { host_sqrt = function(float v) { return v + 1000.0; }; return x * 2.0; }\n"
				"function g(float x) -> float { return host_sqrt(twice(x) + 0.0) + host_sqrt(x); }\n"
				"function loop() -> string { string s = \"\"; for (int i = 0; i < 4; ++i) { s += to_string(host_sqrt(i * 4.0)) + \" \"; if (i == 1) { host_sqrt = function(float v) { return -v; }; } } return s; }\n"
				"string out = to_string(f(8.0));\n"
				"host_sqrt = function(float v) { return v + 100.0; };\n"
				"out += \",\" + to_string(f(8.0));\n"
				"host_sqrt = SQRT0;\n"
				"out += \",\" + to_string(g(8.0)) + \",\" + to_string(g(8.0));\n"
				"host_sqrt = SQRT0;\n"
				"out += \",\" + loop();\n"
				"host_sqrt = function(float& v) { v = v + 1.0; return v; };\n"
				"function byref(float x) -> string { float y = x; float r = host_sqrt(y) + 0.0; return to_string(r) + \"/\" + to_string(y); }\n"
				"out += \",\" + byref(2.0);\n"
				"host_sqrt = 5;\n"
				"function nc(float x) -> string { try { return to_string(host_sqrt(x * 1.0)); } catch (e) { return \"E:\" + e; } }\n"
				"function nc2(float x) -> string { try { return to_string(host_sqrt(nope * 1.0)); } catch (e) { return \"E:\" + e; } }\n"
				"function nc3(float x) -> string { try { return to_string(host_sqrt(x)); } catch (e) { return \"E:\" + e; } }\n"
				"out += \",\" + nc(1.0) + \",\" + nc2(1.0) + \",\" + nc3(1.0);\n"
				"out;";
			const std::string prelude = "[[warn_mode]]\nvar SQRT0 = host_sqrt;\n";
			const std::string expected = "6.82842712474619,224.0,1012.0,2024.0,0.0 2.0 -8.0 -12.0 ,3.0/3.0,E:Cannot call 'host_sqrt': it is an int, not a function,"
				"E:Cannot call 'host_sqrt': it is an int, not a function,E:Cannot call 'host_sqrt': it is an int, not a function";
			both(prelude + src, expected, "rebinding");
			both(prelude + src, expected, "rebinding, proofs off", false);
		});
		test("host_math_alias_member_named_like_the_builtin", [this]() {
			const std::string src = "class V { float k = 10.0; float host_sqrt(float x) { return x * k; } float g(float y) { return host_sqrt(y * 1.0) + host_sqrt(y) + host_cos(y * 0.0); } }\n"
				"class W { float host_cos = 3.0; float g(float y) { try { return host_cos(y * 2.0); } catch (e) { return -1.0; } } }\n"
				"var v = V(); var w = W(); to_string(v.g(2.0)) + \",\" + to_string(w.g(1.0)) + \",\" + to_string(host_sqrt(16.0));";
			both(src, "41.0,-1.0,4.0", "a member named like the builtin");
		});
		test("host_math_alias_host_reregistration", [this]() {
			auto run = [](bool vm) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string out;
				out += e->execute("function f(float x) -> string { return to_string(host_sqrt(x * 1.0)) + \" \" + to_string(host_sqrt(x)); } f(16.0);").as_string();
				e->add_function("host_sqrt", [](const std::string& s) -> script_float { return static_cast<script_float>(s.size()); });
				out += "," + e->execute("f(16.0) + \" \" + to_string(host_sqrt(\"abc\"));").as_string();
				return out;
			};
			const std::string interp = run(false), vm = run(true);
			check_eq(interp, vm, "a host that registers host_sqrt again");
			check_eq(std::string("4.0 4.0,4.0 4.0 3.0"), vm, "the overload set answers");
		});
		// a loop whose body evaluates a stdlib alias closes; the name holds at the entry and after each call's landing
		test("host_math_alias_loops_close_as_cell_blocks", [this]() {
			const std::string src = "array<float> XS = [0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5];\n"
				"function run(int n) -> float { float s = 0.0; for (int i = 0; i < n; ++i) { float x = XS[i & 7]; float d = host_sqrt(x * x + 1.0) - 0.5; s += d + host_sin(x) + host_floor(x); } return s; }\n"
				"to_string(run(64)) + \",\" + to_string(run(3));";
			both(src, "", "an alias loop");
			both(src, "", "an alias loop, proofs off", false);
			const std::string l = listing_run(src);
			const size_t fbeg = l.find("== run ");
			const std::string fl = fbeg == std::string::npos ? l : l.substr(fbeg, l.find("\n== ", fbeg + 1) - fbeg);
			check_true(fl.find("open loop") == std::string::npos && fl.find("C_MATH") != std::string::npos, "the loop closes over the aliases: " + fl);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every) + " re-runs the alias from its guard's mark");
			}
		});
		test("host_math_alias_loop_follows_a_rebinding_by_a_call", [this]() {
			// a call in the body rebinds the name: the landing's check misses and the general form makes the real call from then on;
			// a call between an alias's guard and its op keeps the loop open
			const std::string src = "var SQRT0 = host_sqrt;\n"
				"function rb(int i) -> int { if (i == 5) { host_sqrt = function(float v) { return -v; }; } return i; }\n"
				"function run() -> float { float s = 0.0; for (int i = 0; i < 12; ++i) { float x = i * 1.5; s += host_sqrt(x * x + 1.0); int k = rb(i); s += k; } return s; }\n"
				"function ab(float x) -> float { return x * 2.0; }\n"
				"function run2() -> float { float s = 0.0; for (int i = 0; i < 12; ++i) { float x = i * 0.25; s += host_sqrt(ab(x) + 1.0); } return s; }\n"
				"string a = to_string(run()); host_sqrt = SQRT0; a += \",\" + to_string(run2()); host_sqrt = function(float v) { return v * 10.0; }; a + \",\" + to_string(run2());";
			both(src, "", "a rebinding call inside the loop");
			both(src, "", "a rebinding call inside the loop, proofs off", false);
		});
		// a typed global in arithmetic or a declaration is a typed row's environment operand
		test("typed_globals_are_typed_row_operands", [this]() {
			const std::string src = "float G = 1.5; float H = 2.0; int N = 7;\n"
				"function f(float x, int k) -> float { float a = x - G; float b = a * H + G; float s = G; int m = k * N + N; return b + s + m; }\n"
				"string out = to_string(f(3.0, 2)); G = 4.25; N = -3; out += \",\" + to_string(f(1.0, 5)); out;";
			both(src, "", "typed globals");
			both(src, "", "typed globals, proofs off", false);
			const std::string all = listing_run(src);
			const size_t fbeg = all.find("== f ");
			const std::string l = fbeg == std::string::npos ? all : all.substr(fbeg, all.find("\n== ", fbeg + 1) - fbeg);
			check_true(l.find("BINARY_FUSED_") == std::string::npos && l.find("DECL_VAR") == std::string::npos && l.find("T_DECL_MOV") != std::string::npos, "the global reads are typed rows: " + l);
		});
		test("typed_global_rows_fall_back_to_the_general_form", [this]() {
			// a global read before its declaration ran (the typed null placeholder), a member of the global's name inside a method, a
			// global another unit redeclares with another kind, a loop over the rows; the same values and texts on both backends
			const std::string src = "[[warn_mode]]\nfloat G = 1.5;\n"
				"function f(float x) -> float { float a = x - G; float s = G; return a * s; }\n"
				"float E = 0.0;\n"
				"class C { float G = 10.0; float g(float x) { float a = x - G; float s = G; return a + s; } }\n"
				"function loop(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { t += i * G - G; float w = G; t += w; } return t; }\n"
				"string out = to_string(f(3.0)) + \",\" + to_string(C().g(4.0)) + \",\" + to_string(loop(40));\n"
				"G = 0.25; out += \",\" + to_string(f(3.0)) + \",\" + to_string(loop(7)); out;";
			both(src, "", "typed global fallbacks");
			both(src, "", "typed global fallbacks, proofs off", false);
			const std::string pre = "float P = f0();\nfunction f0() -> float { return g0(2.0); }\nfunction g0(float x) -> float { try { float a = x - Q; return a; } catch (e) { return -1.0; } }\nfloat Q = 3.0;\n"
				"function h(float x) -> float { float a = x * P; return a; }\nto_string(P) + \",\" + to_string(h(2.0));";
			both(pre, "", "a global read ahead of its declaration");
		});
		// a free function grafted into a method reads its globals as the callee does: a field of the caller's class never shadows them
		test("graft_in_a_method_reads_the_callee_globals", [this]() {
			const std::string src = "[[warn_mode]]\nfloat G = 1.5;\nfunction f(float x) -> float { return x * G; }\n"
				"class C { float G = 10.0; float m(float y) { float r = f(y) + 1.0; return r; } float n(float y) { float t = 0.0; for (int i = 0; i < 6; ++i) { t += f(y + i); } return t; } }\n"
				"function top(float y) -> float { float r = f(y) + 1.0; return r; }\n"
				"var c = C(); to_string(c.m(2.0)) + \",\" + to_string(c.n(1.0)) + \",\" + to_string(top(2.0));";
			both(src, "4.0,31.5,4.0", "a graft in a method");
			both(src, "4.0,31.5,4.0", "a graft in a method, proofs off", false);
		});
		// a callee whose only calls are stdlib aliases or math:: intrinsics grafts; the alias keeps its guard inside the graft
		test("host_math_alias_callees_graft", [this]() {
			const std::string src = "float K = 2.0;\n"
				"function len3(float x, float y, float z) -> float { float d = host_sqrt(x * x + y * y + z * z); return d; }\n"
				"function len3r(float x, float y, float z) -> float { return host_sqrt(x * x + y * y + z * z); }\n"
				"function wav(float x) -> float { float s = host_sin(x * K); float c = host_cos(x); float r = s * c + host_floor(x) + math::sqrt(x * x + 1.0); return r; }\n"
				"function run(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { float x = i * 0.25; t += len3(x, 1.0, 2.0); float w = wav(x); t += w; } return t; }\n"
				"function one(float a) -> float { float d = len3r(a, a, a) - 1.0; return d; }\n"
				"to_string(run(40)) + \",\" + to_string(one(3.0));";
			both(src, "", "alias callees");
			both(src, "", "alias callees, proofs off", false);
			const std::string l = listing_run(src);
			check_true(l.find("-- inlined len3 ") != std::string::npos && l.find("-- inlined wav") != std::string::npos && l.find("-- inlined len3r") != std::string::npos, "the alias callees graft: " + l.substr(0, 3000));
		});
		test("host_math_alias_graft_follows_rebinding_and_members", [this]() {
			// the alias inside a graft rebound to a function that writes a global the statement reads; a caller method whose class has a
			// member named like the alias; the alias's value on the stack (a return, an argument, a compound's right side)
			const std::string src = "[[warn_mode]]\nvar SQRT0 = host_sqrt;\nfloat G = 1.0;\n"
				"function len2(float x, float y) -> float { float d = host_sqrt(x * x + y * y); return d; }\n"
				"function st(float a) -> float { float r = G + len2(a, 1.0); return r; }\n"
				"function lp() -> float { float t = 0.0; for (int i = 0; i < 5; ++i) { t += len2(i, 2.0); } return t; }\n"
				"function stackers(float a) -> string { float s = 0.0; s += host_sqrt(a); return to_string(host_sqrt(a * 4.0)) + \" \" + to_string(math::max(host_sqrt(a), 0.5)) + \" \" + to_string(s); }\n"
				"class M { float host_sqrt(float v) { return v * 100.0; } float m(float a) { float r = len2(a, 0.0) + 0.0; return r; } }\n"
				"string out = to_string(st(3.0)) + \",\" + to_string(lp()) + \",\" + stackers(9.0) + \",\" + to_string(M().m(4.0));\n"
				"host_sqrt = function(float v) { G = G + 10.0; return v; };\n"
				"out += \",\" + to_string(st(3.0)) + \",\" + to_string(lp()) + \",\" + stackers(9.0) + \",\" + to_string(M().m(4.0)) + \",\" + to_string(G);\n"
				"function nc() -> string { try { return to_string(len2(1.0, 1.0)); } catch (e) { return \"E:\" + e; } }\n"
				"function nc2() -> string { try { return to_string(host_sqrt(4.0)); } catch (e) { return \"E:\" + e; } }\n"
				"out + \",\" + nc() + \",\" + nc2();";
			both(src, "", "alias grafts under rebinding");
			both(src, "", "alias grafts under rebinding, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
		});
		// the entry reads an environment container through its armed lookup; a field of the name, a rebound or replaced global,
		// and a reference binding keep the full resolution
		test("block_entry_environment_containers", [this]() {
			const std::string src = "[[warn_mode]]\narray<float> A = [1.0, 2.0, 3.0, 4.0];\narray<float> B = [0.5, 0.5, 0.5, 0.5];\n"
				"function sum(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { t += A[i & 3] * B[i & 3]; } return t; }\n"
				"class K { array<float> A = [10.0, 20.0, 30.0, 40.0]; float s(int n) { float t = 0.0; for (int i = 0; i < n; ++i) { t += A[i & 3] * B[i & 3]; } return t; } }\n"
				"function outer() -> string { string out = \"\"; for (int r = 0; r < 4; ++r) { out += to_string(sum(9)) + \" \"; if (r == 1) { A = [5.0, 6.0, 7.0, 8.0]; } if (r == 2) { B[1] = 9.0; } } return out; }\n"
				"string res = outer() + \",\" + to_string(K().s(9)); A = [1.0]; res + \",\" + (function() -> string { try { return to_string(sum(3)); } catch (e) { return \"E:\" + e; } })();";
			both(src, "", "environment containers at the entry");
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a value read into a temp ahead of an environment name on its right (a math call, an element, a member): the name resolves
		// after it, the source order, so the binary keeps its fused form and the loop around it closes
		test("temp_reads_ahead_of_an_environment_partner", [this]() {
			const std::string src = "[[warn_mode]]\nfloat G = 0.25; array<float> A = [1.0, 4.0, 9.0, 16.0];\n"
				"function run(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { float x = i * 0.5; float da = (host_sqrt(x * x + 1.0) - G) * 0.74; float db = A[i & 3] - G; t += da + db; } return t; }\n"
				"function oob() -> string { try { float v = A[9] - nope; return to_string(v); } catch (e) { return \"E:\" + e; } }\n"
				"function und() -> string { try { float v = A[1] - nope; return to_string(v); } catch (e) { return \"E:\" + e; } }\n"
				"string out = to_string(run(40)) + \",\" + oob() + \",\" + und(); G = -3.0; out + \",\" + to_string(run(7));";
			both(src, "", "an environment partner");
			both(src, "", "an environment partner, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			const size_t fb = l.find("== run ");
			const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
			check_true(fl.find("open loop") == std::string::npos && fl.find("-- cell loop") != std::string::npos, "the loop closes: " + fl);
		});
		// a call's int or float result lands in the declaration or temp its consumer fills; any other result takes the consumer
		test("typed_call_results_land_in_their_consumer", [this]() {
			const std::string src = "[[warn_mode]]\n"
				"function fi(int a, int b) -> int { return a * b + 1; }\n"
				"function ff(float a, bool neg, char c) -> float { float r = a * 2.0; if (neg) { r = -r; } return r + cast<int>(c); }\n"
				"function fv(var v) { return v; }\n"
				"function thr(int n) -> int { if (n > 2) { throw \"big \" + to_string(n); } return n; }\n"
				"function rec(int n) -> int { if (n < 2) { return n; } int a = rec(n - 1); int b = rec(n - 2); return a + b; }\n"
				"function run() -> string { string out = \"\";\n"
				"  int a = fi(3, 4); float b = ff(1.5, true, 'a'); float c = fv(2); int d = fv(7); float e = math::max(2.5, 1.0) + ff(0.5, false, 'b');\n"
				"  int g = rec(12); float h = fi(2, 2); out += to_string(a) + \" \" + to_string(b) + \" \" + to_string(c) + \" \" + type_of(c) + \" \" + to_string(d) + \" \" + to_string(e) + \" \" + to_string(g) + \" \" + to_string(h) + \" \" + type_of(h);\n"
				"  for (int i = 0; i < 5; ++i) { try { int t = thr(i); out += \" t\" + to_string(t); } catch (err) { out += \" E:\" + err; } }\n"
				"  try { int s = fv(\"str\"); out += \" s\" + to_string(s); } catch (err) { out += \" E2:\" + err; }\n"
				"  float x = 1.0; float y = x + ff(x, false, 'c') * fi(1, 1); out += \" \" + to_string(y);\n"
				"  return out; }\n"
				"run();";
			both(src, "", "typed call landings");
			both(src, "", "typed call landings, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			check_true(l.find("CALL_TYPED") != std::string::npos, "the calls land typed: " + l.substr(0, 2000));
		});
		// a graft's parameter, local or result is a compiler value under its declared type: a block lands it by the declaration's
		// conversion (an int into a float local widens, a bool into an int local counts), never the source's raw kind under the cell's float contract
		test("graft_locals_convert_by_their_declared_type_in_a_block", [this]() {
			const std::string src = "[[warn_mode]]\n"
				"function f1(int a, int b) -> float { float top = a < b ? a : b; float x = top * 2.0; return x; }\n"
				"function f2(int a) -> float { float t = a; return t * 0.5; }\n"
				"function f3(bool v) -> int { int k = v; return k + 1; }\n"
				"function f4(int a) -> bool { float top = a; if (top < 50.0) { return true; } return false; }\n"
				"function f5(int a) -> float { return a; }\n"
				"function run() -> string { float acc = 0.0; int ic = 0; int i = 0;\n"
				"  while (i < 12) { int a = i * 13 % 100; int b = (i * 7 + 30) % 100;\n"
				"    float r1 = f1(a, b); float r2 = f2(a); int r3 = f3(a * 3 > 100); bool r4 = f4(a); float r5 = f5(a);\n"
				"    acc = acc + r1 * 3.0 + r2 * 5.0 + r5 * 7.0; ic = ic + r3 * 11; if (r4) { ic = ic + 1; } i = i + 1; }\n"
				"  return to_string(acc) + \" \" + to_string(ic); }\n"
				"run();";
			both(src, "", "declared-type landings in a block");
			both(src, "", "declared-type landings in a block, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			const size_t fb = l.find("== run ");
			const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
			check_true(fl.find("-- inlined f1") != std::string::npos && fl.find("-- cell loop") != std::string::npos, "the leaves graft into the block: " + fl);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every = 1; every <= 5; ++every) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a ternary operand lands in a compiler temp ahead of the operation (each arm by one move), so the binary around it keeps a
		// direct operand and a block takes it: an absolute value, a clamp, int and float arms, nested ternaries, environment partners
		test("ternary_operands_lift_into_temps", [this]() {
			const std::string src = "[[warn_mode]]\nfloat G = 0.25; int GI = 3;\n"
				"function run(int n) -> string { float t = 0.0; int s = 0;\n"
				"  for (int i = 0; i < n; ++i) { float x = i * 0.5 - 3.0;\n"
				"    float a = (x < 0.0 ? -x : x) - G; float b = G * (x > 1.0 ? x : 1.0); float c = (i % 2 == 0 ? 1 : 2.5) + (x < 0.0 ? x : 0);\n"
				"    int d = (i < 3 ? i : 3) * 2 + (i > 5 ? i - 5 : 0); float e = (x < -1.0 ? (x < -2.0 ? -2.0 : x) : (x > 1.0 ? 1.0 : x)) * 0.5;\n"
				"    float h = (x < 0.0 ? math::sqrt(-x) : math::floor(x)) + (GI > 2 ? G : 0.0);\n"
				"    t += a + b + c + e + h; s += d; }\n"
				"  return to_string(t) + \" \" + to_string(s); }\n"
				"function world(float g) -> float { return ((g < 0.0 ? -g : g) - G) / (1.5 * (2.1 + G * 2.4)); }\n"
				"function march(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { t += world(i * 0.25 - 2.0) * 0.1; } return t; }\n"
				"function mixed(var v, int n) -> string { string out = \"\"; for (int i = 0; i < n; ++i) { var w = (i % 3 == 0 ? v : i) + (i < 2 ? 0.5 : 1); out += to_string(w) + \" \"; } return out; }\n"
				"run(20) + \",\" + to_string(march(30)) + \",\" + mixed(4, 6) + \",\" + mixed(2.5, 4);";
			both(src, "", "lifted ternaries");
			both(src, "", "lifted ternaries, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			for (const char* fn : { "== run ", "== march " }) {
				const size_t fb = l.find(fn);
				const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
				check_true(fl.find("open loop") == std::string::npos && fl.find("-- cell loop") != std::string::npos, std::string("the loop closes: ") + fl);
			}
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every = 1; every <= 5; ++every) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a function with for and while loops is a frame program, its loops inner loops of the program: a return inside a counted loop is a
		// ret, a break leaves the inner loop, nested loops and a while with a compound condition run as cells, an overflow inside a loop
		// raises through the general form; a loop calls them raw, the program scope enters them generally
		test("loops_inside_frame_programs", [this]() {
			const std::string defs = "[[warn_mode]]\n"
				"function find(int n, int key) -> int { for (int i = 0; i < n; ++i) { int h = (i * 7 + 3) % 17; if (h == key) { return i; } } return -1; }\n"
				"function brk(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s += i * 3; if (s > 500) { break; } } return s; }\n"
				"function wh(float x) -> float { float t = x; int k = 0; while (t < 100.0 && k < 50) { t = t * 1.3 + 0.5; ++k; } return t + k; }\n"
				"function grid(int n) -> int { int s = 0; for (int y = 0; y < n; ++y) { for (int x = 0; x < n; ++x) { s += x * y - (x ^ y); } } return s; }\n"
				"function ov(int n) -> int { int s = 1; for (int i = 0; i < n; ++i) { s = s * 3; } return s; }\n"
				"function run(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { t += find(20, i % 17) + brk(i * 4) + grid(i % 5) + wh(i * 0.5) + ov(i % 30); } return t; }\n";
			const std::string src = defs + "to_string(run(60)) + \",\" + to_string(find(20, 99)) + \",\" + to_string(wh(1.0)) + \",\" + to_string(grid(7));";
			const std::string overflows = defs + "to_string(run(3)) + to_string(ov(45));";
			both(src, "", "loops inside frame programs");
			both(src, "", "loops inside frame programs, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			both(overflows, "", "an overflow inside a frame program's loop");
			check_true(run_on(true, overflows).find("throw:") != std::string::npos, "the overflow raises: " + run_on(true, overflows));
			const std::string l = listing_run(src);
			auto section = [&](const char* fn) { const size_t fb = l.find(fn); return fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb); };
			for (const char* fn : { "== find ", "== brk ", "== wh ", "== grid ", "== ov " }) {
				check_true(section(fn).find("-- frame program") != std::string::npos && section(fn).find("(raw entry)") != std::string::npos, std::string("a raw-enterable frame program: ") + section(fn));
			}
			check_true(section("== run ").find("raw 5") != std::string::npos, "run's loop calls all five raw: " + section("== run "));
			if (!engine::miss_injection_available()) { return; }
			for (const std::string* s : { &src, &overflows }) {
				const std::string oracle = run_on(false, *s);
				for (uint32_t every = 1; every <= 5; ++every) {
					auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
					std::string got;
					try { got = e->execute(*s).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
					check_eq(oracle, got, "a miss at every " + std::to_string(every));
				}
			}
		});
		// a loop-free function too big to graft is a frame program by its own work, as is the function whose loop calls it: the loop enters
		// it raw (its globals read at the entry, as they stand after the program scope rewrites them; arguments that are lifted trees; its
		// result compounded into the sum), the program scope enters it generally, a straight region of a function with a var parameter calls
		// it raw, its call to a function with a var parameter is a door, its ternary return and its compound over a quotient stay cells
		test("big_leaves_are_raw_frame_programs", [this]() {
			std::string big = "function big(float x, float y, bool m) -> float {\n";
			for (int k = 0; k < 100; ++k) { big += "  float a" + std::to_string(k) + " = " + (k ? "a" + std::to_string(k - 1) : std::string("x")) + " * 0.5 + " + (k % 3 ? "x" : "G") + ";\n"; }
			big += "  float ice = 0.0;\n  if (y > 0.2) { ice = a99 * 0.5; ice += 0.032 / (1.0 + a98 * a98); }\n"
				"  if (m) { int w = 23 - slow(3, 0); float h = w * 0.5 + w; return h; }\n"
				"  return ice < 0.045 ? ice : 0.045 + (ice - 0.045) * 0.5 + V;\n}\n";
			std::string slow = "function slow(int k, var tag) -> int { int s = 0; for (int i = 0; i < k; ++i) { s += i; }";   // too big to graft, a var parameter: a door
			for (int k = 0; k < 80; ++k) { slow += " s = (s * 3 + k - " + std::to_string(k) + ") % 1000;"; }
			slow += " return s % 1000; }\n";
			const std::string src = "[[warn_mode]]\nfloat G = 0.25; float V = 3.0;\n" + slow + big +
				"function run(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { t += big(i * 0.01, (i & 3) * 0.1 - (i % 5) * 0.05, (i & 7) == 0); } return t; }\n"
				"function shade(float x, var tag) -> float { float p = x * 1.5 + G; float q = p * p - x; float a = big(p, 0.3, false); float b = big(q + 0.1, 0.3, false) * 2.0;\n"
				"  float c = a * b + p * q - a; float d = c * 0.5 + b * 0.25 - q; float e = d * d + c; float s = 0.0; for (int k = 0; k < 3; ++k) { s += e * k; } return s + d; }\n"
				"string out = to_string(run(40)) + \",\" + to_string(big(0.5, 0.3, true)) + \",\" + to_string(big(0.5, 0.3, false)) + \",\" + to_string(shade(0.4, 0));\n"
				"V = 2.5; G = -1.0;\nout += \",\" + to_string(run(40)) + \",\" + to_string(big(0.5, 0.1, false)) + \",\" + to_string(shade(0.4, 0));\nout;";
			both(src, "", "big leaves");
			both(src, "", "big leaves, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			auto section = [&](const char* fn) { const size_t fb = l.find(fn); return fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb); };
			check_true(section("== big ").find("(raw entry)") != std::string::npos, "big is a raw-enterable frame program: " + section("== big "));
			check_true(section("== run ").find("-- frame program") != std::string::npos && section("== run ").find("raw 1") != std::string::npos, "run, a function with a loop, is a frame program whose loop calls big raw: " + section("== run "));
			check_true(section("== shade ").find("-- cell region") != std::string::npos && section("== shade ").find("raw 2") != std::string::npos, "shade's straight region calls it raw: " + section("== shade "));
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every = 1; every <= 5; ++every) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a left-deep chain holds one temp however long it is, so the lift takes it whole and the loop around it closes (a sum of 40 locals,
		// an int chain that overflows, products on the right); a right-deep tree past the temp limit stays the stack form and agrees
		test("long_chains_lift_by_their_temp_peak", [this]() {
			std::string src = "[[warn_mode]]\nfunction run(int n) -> string { float t = 0.0; int s = 0;\n  for (int i = 0; i < n; ++i) { float x = i * 0.25;";
			for (int k = 0; k < 40; ++k) { src += " float a" + std::to_string(k) + " = x * " + std::to_string(k + 1) + ".5 - " + std::to_string(k % 7) + ".0;"; }
			src += "\n    t += a0";
			for (int k = 1; k < 40; ++k) { src += (k % 3 == 0 ? " - a" : " + a") + std::to_string(k) + (k % 5 == 0 ? " * x" : ""); }
			src += ";\n    s += i";
			for (int k = 1; k < 30; ++k) { src += " + i * " + std::to_string(k); }
			src += "; }\n  return to_string(t) + \" \" + to_string(s); }\n"
				"function deep(float x) -> float { return x";
			for (int k = 0; k < 20; ++k) { src += " - (x * 0." + std::to_string(k + 1) + " + (x"; }
			src += std::string(40, ')') + "; }\n"
				"function overflow(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = i";
			for (int k = 1; k < 20; ++k) { src += " + i * 400000000000000000"; }
			src += "; } return s; }\n";
			const std::string fits = src + "run(30) + \",\" + to_string(deep(1.5)) + \",\" + to_string(overflow(2));";
			const std::string overflows = src + "to_string(overflow(3));";
			both(fits, "", "long chains");
			both(fits, "", "long chains, proofs off", false);
			check_true(run_on(true, fits).find("throw:") == std::string::npos, "runs: " + run_on(true, fits));
			both(overflows, "", "a long int chain that overflows");
			check_true(run_on(true, overflows).find("throw:") != std::string::npos, "the int chain overflows: " + run_on(true, overflows));
			const std::string l = listing_run(src + "run(30);");
			const size_t fb = l.find("== run ");
			const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
			check_true(fl.find("open loop") == std::string::npos && fl.find("-- cell loop") != std::string::npos, "the loop closes: " + fl);
			if (!engine::miss_injection_available()) { return; }
			for (const std::string* s : { &fits, &overflows }) {
				const std::string oracle = run_on(false, *s);
				for (uint32_t every = 1; every <= 5; ++every) {
					auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
					std::string got;
					try { got = e->execute(*s).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
					check_eq(oracle, got, "a miss at every " + std::to_string(every));
				}
			}
		});
		// an environment name ahead of a hoisted read (`q + A * host_sin(q * 1.3)`) reads into a temp first and the loop closes; a leaf whose
		// return the checker cannot prove (`return host_sqrt(x) + 1.0`) grafts into a loop when its declared result is a scalar, the block landing it
		// by that type
		test("environment_partners_and_unproven_scalar_returns", [this]() {
			const std::string src = "[[warn_mode]]\nfloat A = 0.2; array<float> T = [1.0, 2.0, 3.0, 4.0];\n"
				"function inner(float x) -> float { return host_sqrt(x) + 1.0; }\n"
				"function half(int k) -> float { return k / 2; }\n"
				"function g3(int n) -> float { float s = 0.0; for (int i = 0; i < n; ++i) { float q = i * 0.001; float w = q + A * host_sin(q * 1.3); float v = A * T[i & 3] + q; s = s + w + v; } return s; }\n"
				"function g3i(int n) -> float { float s = 0.0; for (int i = 0; i < n; ++i) { float q = i * 0.001; float w = q + A * math::sin(q * 1.3); s = s + w; } return s; }\n"
				"function g8(int n) -> float { float s = 0.0; for (int i = 0; i < n; ++i) { float d = inner(i * 0.01); float h = half(i); s = s + d + h; } return s; }\n"
				"string out = to_string(g3(50)) + \",\" + to_string(g3i(50)) + \",\" + to_string(g8(40));\n"
				"host_sin = function(float v) { A = A + 0.5; return v; }; host_sqrt = function(float v) { return v * 3.0; };\n"
				"out + \",\" + to_string(g3(5)) + \",\" + to_string(g8(5));";
			both(src, "", "environment partners and unproven returns");
			both(src, "", "environment partners and unproven returns, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			for (const char* fn : { "== g3 ", "== g3i ", "== g8 " }) {
				const size_t fb = l.find(fn);
				const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
				check_true(fl.find("open loop") == std::string::npos && fl.find("-- cell loop") != std::string::npos, std::string("the loop closes: ") + fl);
			}
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every = 1; every <= 5; ++every) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a name left of a lifted ternary reads before the ternary's condition and arms run (a call there may rebind it), on both backends
		test("a_name_left_of_a_lifted_ternary_reads_first", [this]() {
			const std::string src = "[[warn_mode]]\nint G = 1;\n"
				"function bumpg() -> bool { G = G + 10; return G > 15; }\n"
				"function run(int n) -> string { string out = \"\"; for (int i = 0; i < n; ++i) { int a = G + (G * 2 + (bumpg() ? 1 : 2)); int b = G + (bumpg() ? 3 : 4); int c = (bumpg() ? 5 : 6) + G; out += to_string(a) + \" \" + to_string(b) + \" \" + to_string(c) + \";\"; } return out; }\n"
				"run(4);";
			both(src, "", "a name left of a lifted ternary");
			both(src, "", "a name left of a lifted ternary, proofs off", false);
		});
		// a straight-line region in a method that writes globals through stdlib aliases (OSSUARY's set_world): every miss resumes the general
		// form with the state it expects
		test("region_misses_in_a_method_writing_globals_through_aliases", [this]() {
			const std::string src =
				"float SK_T = 0.0; float SK_S = 0.0; float SK_K = 0.0; float SK_TW = 0.0; float SK_WA = 0.0; float SK_MG = 0.0; float SK_MP = 0.0; float SK_MD = 0.0; float SK_CX = 0.0;\n"
				"class Scene { float cxs = 0.1;\n"
				"  void set_world(float t) { SK_T = t; SK_S = 0.8 + 0.07 * host_sin(t * 0.37); SK_K = 0.45 * host_sin(t * 0.21 + 0.6); SK_TW = 0.11 * host_sin(t * 0.29 + 1.3); SK_WA = 0.18 + 0.14 * host_sin(t * 0.33 + 2.0);\n"
				"    float ph = t * 0.19; float a = 0.5 + 0.5 * host_cos(ph); float b = 0.5 + 0.5 * host_cos(ph - 2.0944); float c = 0.5 + 0.5 * host_cos(ph + 2.0944);\n"
				"    a = a * a * a; b = b * b * b; c = c * c * c; float sum = a + b + c; SK_MG = a / sum; SK_MP = b / sum; SK_MD = c / sum; SK_CX = cxs; } }\n"
				"Scene s = Scene(); float acc = 0.0;\n"
				"for (int i = 0; i < 40; ++i) { s.set_world(i * 0.1); acc += SK_S + SK_K + SK_TW + SK_WA + SK_MG + SK_MP * 2.0 + SK_MD * 3.0 + SK_CX; }\n"
				"to_string(acc);";
			both(src, "", "a region in a method writing globals");
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every = 1; every <= 7; ++every) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a leaf whose body calls another leaf grafts, the inner leaf grafted inside it (one level), each behind its own guard
		test("nested_leaves_graft", [this]() {
			const std::string src = "[[warn_mode]]\nfloat S = 0.5;\n"
				"function field(float x, float y) -> float { float s = host_sin(x * S); float c = host_cos(y); float r = s * c + x * y; return r; }\n"
				"function world(float x, float y) -> float { float g = field(x, y); float r = (g < 0.0 ? -g : g) - 0.25; return r; }\n"
				"function march(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { float d = world(i * 0.5, t); t += d * 0.1; } return t; }\n"
				"function one(float a) -> float { float w = world(a, a) + 1.0; return w; }\n"
				"function ping(int n) -> int { if (n < 1) { return 0; } int r = pong(n - 1); return r + 1; }\n"
				"function pong(int n) -> int { int r = ping(n); return r; }\n"
				"string out = to_string(march(30)) + \",\" + to_string(one(2.0)) + \",\" + to_string(ping(5));\n"
				"field = function(float x, float y) { return x - y; };\n"
				"out += \",\" + to_string(march(30)) + \",\" + to_string(one(2.0));\n"
				"world = function(float x, float y) { return 0.5; };\n"
				"out + \",\" + to_string(march(30)) + \",\" + to_string(one(2.0));";
			both(src, "", "nested leaves");
			both(src, "", "nested leaves, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			const size_t mb = l.find("== march ");
			const std::string ml = mb == std::string::npos ? l : l.substr(mb, l.find("\n== ", mb + 1) - mb);
			check_true(ml.find("-- inlined world") != std::string::npos && ml.find("-- inlined field") != std::string::npos && ml.find("open loop") == std::string::npos, "both leaves graft into the closed loop: " + ml);
		});
		test("math_namespace_under_parallel_transform", [this]() {
			const std::string src = "function row(int i) -> float { float x = i * 0.5; return math::sqrt(x * x + 1.0) + math::sin(x) * math::cos(x * 0.5) + math::floor(x * 0.25); }\n"
				"array<int> idx = []; for (int i = 0; i < 400; ++i) { idx.push(i); }\n"
				"function serial() -> string { float s = 0.0; for (int i = 0; i < 400; ++i) { s += row(i); } return to_string(s); }\n"
				"function par() -> string { var r = parallel_transform(idx, row); float s = 0.0; for (int i = 0; i < 400; ++i) { s += r[i]; } return to_string(s); }\n"
				"string a = serial() + \"=\" + par();\n"
				"host_sqrt = function(float v) { return v * 2.0; };\n"
				"a + \",\" + serial() + \"=\" + par();";
			both(src, "", "the workers' aliases");
			check_true(run_on(true, src).rfind("throw:", 0) != 0, "runs: " + run_on(true, src));
			const std::string vm = run_on(true, src);
			const size_t c = vm.find(',');
			check_true(c != std::string::npos && vm.substr(0, vm.find('=')) == vm.substr(vm.find('=') + 1, c - vm.find('=') - 1), "serial and parallel agree: " + vm);
		});
		// a straight-line span of typed statements outside every closed loop runs as a region (a var parameter keeps the bodies off frame
		// programs): entered at its first instruction, left at its end; a refusal or a miss at the first instruction runs the trampoline's
		// copy of it
		test("straight_line_regions", [this]() {
			const std::string src = "[[warn_mode]]\n"
				"function shade(float x, float y, float z, var tag) -> float {\n"
				"  float a = x * 1.5 + y * 2.5; float b = a * a - z * 0.25; float c = b / (a + 1.0);\n"
				"  float d = c * c + a * b - x; float e = d * 0.5 + c * 0.25 + b * 0.125; float f = e - d * 0.75 + a;\n"
				"  return f * 2.0 + e; }\n"
				"function mix(int p, int q, var tag) -> int {\n"
				"  int r = p * 3 + q; int s = r * r - p; int t = s % 7 + r / 3; int u = t * t + s - r; int v = u + p * q - t; int w = v % 11 + u * 2 - s; int z = w * 3 + v - t;\n"
				"  return z + u * 2; }\n"
				"function run() -> string { var sh = shade; var mx = mix; float t = 0.0; int it = 0; for (int i = 0; i < 60; ++i) { t = t + sh(i * 0.5, 1.25, i * 0.125, 0); it = it + mx(i, 7, 0); } return to_string(t) + \" \" + to_string(it); }\n"
				"run();";
			both(src, "", "straight regions");
			both(src, "", "straight regions, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			for (const char* fn : { "== shade ", "== mix " }) {
				const size_t fb = l.find(fn);
				const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
				check_true(fl.find("-- cell region") != std::string::npos && fl.find("CELLS_ENTER") != std::string::npos, std::string("a region: ") + fl);
			}
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every = 1; every <= 7; ++every) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a return whose expression is a graft: the graft's result lands before the return's own instructions, where the block exits, so the
		// exit flushes it (a region's last statement, and a return inside a closed loop; a var parameter keeps the region bodies off frame programs);
		// a graft's own early returns jump to the return's last instruction, inside the exit's span, and land on the exit too
		test("exit_after_a_graft_flushes_its_result", [this]() {
			const std::string src = "[[warn_mode]]\n"
				"function mul3(int a, int b) -> int { return a * b + 3; }\n"
				"function tailcall(int x, var tag) -> int { int v = x * 7 + 1; int w = v * v - x; int y = w % 13 + v * 3; int z = y * y - w + v; int q = z + x * 2; int r = q * 3 - z + y; return mul3(r, 5); }\n"
				"function loopret(int n) -> int { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc * 3 + i; if (acc > 1000) { return mul3(acc, 2); } } return acc; }\n"
				"class R { int s = 5; int raw() { int v = s; v = v ^ (v >> 3); v = v ^ (v << 5); v = v ^ (v >> 7); v = v * 3 + 1; s = v & 65535; return mul3(v & 255, 7); } }\n"
				"function early(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { t += i * 0.5; if (t > 5.0) { return host_sqrt(t) + 1.0; } } return t; }\n"
				"function rowr(int i, var tag) -> float { float x0 = i * 0.5; float y = x0 * x0 + 1.0; float z = y * 0.5 - x0; float w = z * z + y * 0.25; float x = w - z * 0.125 + y * 0.01; return host_sqrt(x * x + 1.0) + host_sin(x) * host_cos(x * 0.5) + host_floor(x * 0.25); }\n"
				"function pick4(int id, int a, int b, int c, int d) -> int { int k = id % 4; if (k == 0) { return a * 3 + id; } if (k == 1) { return b * 5 + id; } if (k == 2) { return c * 7 + id; } return d * 11 + id; }\n"
				"function pickpx(int t, int u, var tag) -> int { int x = u * 3 + 1; int y = x * x - u; int z = y % 7 + x; int w = z * 2 - y; int q = w + x * y; int r = q % 11 + w;\n"
				"  if (t == 1) { int id = x + y + z + w + q + r; return pick4(id, x, y, z, w); } if (t == 2) { return x - y; } return 7777; }\n"
				"function pickloop(int n) -> int { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc * 3 + i; if (acc > 20) { return pick4(acc, i, acc, n, 2); } } return acc; }\n"
				"function run() -> string { string out = \"\"; for (int i = 0; i < 4; ++i) { out += to_string(tailcall(i, 0)) + \" \" + to_string(loopret(i * 4)) + \" \" + to_string(pickpx(1, i, 0)) + \" \" + to_string(pickloop(i + 3)) + \" \"; } R r = R(); for (int i = 0; i < 4; ++i) { out += to_string(r.raw()) + \" \"; }\n"
				"  var ea = early; var rw = rowr; for (int i = 0; i < 8; ++i) { out += to_string(ea(i)) + \" \" + to_string(rw(i, 0)) + \" \"; } return out; }\n"
				"string first = run(); host_sqrt = function(float v) { return v * 2.0; }; first + \",\" + run();";
			both(src, "", "exit after a graft");
			both(src, "", "exit after a graft, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			for (const char* fn : { "== tailcall ", "== rowr " }) {
				const size_t fb = l.find(fn);
				const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
				check_true(fl.find("-- cell region") != std::string::npos, std::string("a region: ") + fl);
			}
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every = 1; every <= 4; ++every) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("straight_line_region_kinds_exits_and_errors", [this]() {
			const std::string src = "[[warn_mode]]\nfloat G = 0.5; int GI = 3;\n"
				"function kinds(var a, var b) -> string {\n"
				"  var x = a * 2 + b; var y = x * x - a; var z = y / 2 + x * 3; var w = z - y * 2 + x; var q = w * w + z - y; var k = q - w * 3 + z * 2;\n"
				"  return to_string(x) + \" \" + to_string(y) + \" \" + to_string(z) + \" \" + to_string(w) + \" \" + to_string(q) + \" \" + to_string(k) + \" \" + type_of(q); }\n"
				"function exits(int n, float f) -> float {\n"
				"  float a = f * 2.0 + n; float b = a * a - f; float a2 = a * 0.5 + b * 0.25; float b2 = a2 * a2 - b;\n"
				"  if (n > 5) { return a + b + b2; }\n"
				"  float c = b * 0.5 + a * 0.25; float d = c * c - b + a; G = G + d * 0.001; GI = GI + n * 2;\n"
				"  float e = d > 10.0 ? d * 0.5 : d * 2.0; float h = e * e - c * 0.5 + d * 0.25 - a2;\n"
				"  return e + c + h; }\n"
				"function divs(int a, int b) -> int {\n"
				"  int x = a * 3 + b; int y = x * x - a; int z = y / b + x % b; int w = z * 2 + y - x; int v = w + a * b - z; int k = v * 5 - w + y % b;\n"
				"  return k + w * 2; }\n"
				"function ovf(int a) -> int { int x = a * a + 1; int y = x * x + 2; int z = y * y + 3; int w = z * z + 4; int v = w * w + 5; int u = v - w * 2 + z; int s = u * 3 + v - x; int q = s - u + 1; return q; }\n"
				"function openloop(int n) -> string { string s = \"\"; float t = 0.0;\n"
				"  for (int i = 0; i < n; ++i) { float a = i * 0.5 + t; float b = a * a - i; float c = b * 0.25 + a; float d = c * c - b * 0.5; float e = d * 0.5 + c - a; float g = e * e - b; t = d * 0.001 + c * 0.01 + g * 0.0001; s += to_string(i); }\n"
				"  return s + \" \" + to_string(t); }\n"
				"function nulls(int n) -> string { var r; float a = n * 1.5; float b = a * a + 2.0; r = a * b + 1.0; float c = r * 2.0 + a; float d = c * c - b; float e = d * 0.5 + c; float f2 = e * e - d + c; float g2 = f2 * 0.5 + e * 0.25; return to_string(r) + \" \" + to_string(g2); }\n"
				"class P { float x = 1.0; float y = 2.0; float vx = 0.5; float vy = 0.25;\n"
				"  float step(float dt) { float nx = x + vx * dt; float ny = y + vy * dt; float ax = nx * 0.5 - ny * 0.25; float ay = ny * 0.5 + nx * 0.125; x = nx + ax * dt; y = ny + ay * dt; return x * y; } }\n"
				"function guarded(int a, int b) -> string { try { return to_string(divs(a, b)); } catch (e) { return \"E:\" + e; } }\n"
				"function guarded2(int a) -> string { try { return to_string(ovf(a)); } catch (e) { return \"E:\" + e; } }\n"
				"function run() -> string { string out = \"\";\n"
				"  out += kinds(1, 2) + \",\" + kinds(1.5, 2) + \",\" + kinds(3, 0.25) + \",\" + kinds(7, 2) + \",\";\n"
				"  for (int i = 0; i < 9; ++i) { out += to_string(exits(i, i * 0.75)) + \" \"; }\n"
				"  out += \",\" + guarded(5, 3) + \",\" + guarded(5, 0) + \",\" + guarded(-9, 4) + \",\" + guarded2(3) + \",\" + guarded2(100000) + \",\";\n"
				"  out += openloop(12) + \",\" + nulls(3) + \",\" + nulls(4) + \",\";\n"
				"  P p = P(); float ps = 0.0; for (int i = 0; i < 6; ++i) { ps += p.step(0.1); } out += to_string(ps) + \",\" + to_string(G) + \",\" + to_string(GI);\n"
				"  return out; }\n"
				"run();";
			both(src, "", "region kinds, exits and errors");
			both(src, "", "region kinds, exits and errors, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			check_true(l.find("-- cell region") != std::string::npos, "regions form: " + l.substr(0, 3000));
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every = 1; every <= 7; ++every) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// G7: a ternary store's computed arm (a unary, a math call, an element read, a member read) lands in a compiler temp and the
		// typed move row stores it, so the loop around it closes; only the taken arm runs, and its errors keep their texts
		test("ternary_store_arms_land_computed_values_through_the_move_row", [this]() {
			const std::string src = "class B { float w = 0.75; }\n"
				"array<float> A = [0.5, -1.5, 2.5, -3.5, 4.5, -5.5, 6.5, -7.5];\n"
				"function run(int n) -> float { float s = 0.0; B b = B(); float m = 0.0; float qx = 0.0; float qy = 0.0; float e = 0.0;\n"
				"  for (int i = 0; i < n; ++i) { qx = A[i & 7] * 1.5; qx = qx < 0.0 ? -qx : qx; int j = (i + 3) & 7; qy = A[j]; qy = qy > 0.0 ? qy : -qy;\n"
				"    m = qx * 0.5; m = m > 1.0 ? math::sqrt(m) : m; e = i > 3 ? A[i & 7] : b.w; e = e < 0.0 ? b.w : e; s += qx + qy + m + e; }\n"
				"  return s; }\n"
				"function kint(int k) -> string { try { int r = 0; for (int i = 0; i < 3; ++i) { r = k < 0 ? -k : k; k = k + 1; } return to_string(r); } catch (err) { return \"E:\" + err; } }\n"
				"function lazy(int n) -> string { array<int> t = [1, 2]; int v = 0; string out = \"\"; for (int i = 0; i < n; ++i) { v = i < 2 ? t[i] : -i; out += to_string(v); } try { for (int i = 0; i < n; ++i) { v = i < 3 ? t[i] : 0; } } catch (err) { out += \"|E:\" + err; } return out; }\n"
				"to_string(run(64)) + \",\" + to_string(run(3)) + \",\" + kint(-5) + \",\" + kint(-9223372036854775807 - 1) + \",\" + lazy(5);";
			both(src, "", "store arms");
			both(src, "", "store arms, proofs off", false);
			const std::string vm = run_on(true, src);
			check_true(vm.find("throw:") == std::string::npos && vm.find(",3,E:") != std::string::npos && vm.find(",12-2-3-4|E:") != std::string::npos, "the arms run as the interpreter runs them: " + vm);
			const std::string l = listing_run(src);
			const size_t fbeg = l.find("== run ");
			const std::string fl = fbeg == std::string::npos ? l : l.substr(fbeg, l.find("\n== ", fbeg + 1) - fbeg);
			check_true(fl.find("open loop") == std::string::npos && fl.find("UNARY op=") == std::string::npos && fl.find("CELLS_") != std::string::npos, "the loop closes over the arms: " + fl);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// G6: bare host_abs is a stdlib alias (the float host_abs: an int argument converts first, as the builtin's float parameter converts it)
		test("stdlib_abs_alias_is_the_float_builtin", [this]() {
			const std::string src = "function r(var v) -> string { return to_string(host_abs(v)) + \":\" + type_of(host_abs(v)); }\n"
				"function k(int i) -> string { return to_string(host_abs(i)) + \" \" + to_string(host_abs(i * 1)); }\n"
				"function x(float f) -> string { float y = host_abs(f) + 0.0; return to_string(y) + \" \" + to_string(host_abs(f * 1.0)); }\n"
				"r(4) + \"|\" + r(-2.25) + \"|\" + r(-0.0) + \"|\" + r(9007199254740993) + \"|\" + k(-7) + \"|\" + k(-9223372036854775807 - 1) + \"|\" + x(-0.0) + \"|\" + x(-math::INF)"
				" + \"|\" + x(math::NAN_VALUE) + \"|\" + x(-math::NAN_VALUE) + \"|\" + to_string(host_abs(-3) == 3.0);";
			both(src, "", "int, float and edge arguments");
			both(src, "", "int, float and edge arguments, proofs off", false);
			check_true(run_on(true, src).rfind("4.0:float|2.25:float|0.0:float|", 0) == 0, "the builtin's numbers: " + run_on(true, src));
			const std::string loop = "array<float> XS = [0.5, -1.5, 2.5, -3.5, -0.0, 5.5, -6.5, 7.5];\n"
				"function mag(float v) -> float { float a = host_abs(v); return a * 2.0; }\n"
				"function run(int n) -> float { float s = 0.0; for (int i = 0; i < n; ++i) { float q = XS[i & 7]; int j = i - 5; float mq = mag(q); s += host_abs(q) + host_abs(j) + mq; } return s; }\n"
				"to_string(run(64)) + \",\" + to_string(run(3));";
			both(loop, "", "an host_abs loop");
			both(loop, "", "an host_abs loop, proofs off", false);
			const std::string l = listing_run(loop);
			const size_t fbeg = l.find("== run ");
			const std::string fl = fbeg == std::string::npos ? l : l.substr(fbeg, l.find("\n== ", fbeg + 1) - fbeg);
			check_true(fl.find("open loop") == std::string::npos && fl.find("C_MATH") != std::string::npos && fl.find("-- inlined mag") != std::string::npos, "the loop closes over host_abs and grafts its leaf: " + fl);
			const std::string rebind = "[[warn_mode]]\nvar ABS0 = host_abs;\nfunction f(float x) -> float { return host_abs(x * 2.0) + host_abs(x); }\n"
				"string out = to_string(f(-8.0)); host_abs = function(float v) { return v + 100.0; }; out += \",\" + to_string(f(-8.0)); host_abs = ABS0; out += \",\" + to_string(f(-8.0)); out;";
			both(rebind, "24.0,176.0,24.0", "a rebinding");
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, loop);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(loop).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// G7b: a ternary as a math intrinsic's or a stdlib alias's argument lands in a compiler temp (the call takes a direct operand and the
		// loop closes); a ternary whose condition runs user code reads its neighbours as the interpreter does
		test("ternary_math_arguments_land_in_compiler_temps", [this]() {
			const std::string src = "int K = 1000;\n"
				"array<float> A = [0.5, -1.5, 2.5, -3.5, 4.5, -5.5, 6.5, -7.5];\n"
				"function run(int n) -> float { float s = 0.0; int acc = 0;\n"
				"  for (int i = 0; i < n; ++i) { int j1 = (i + 1) & 7; int j2 = (i + 2) & 7; float a = A[i & 7]; float b = A[j1]; float c = A[j2]; int f = i % 13 - 2;\n"
				"    int facet = (a > 0.0 ? 1 : 0) + (b > 0.0 ? 2 : 0) + (c > 0.0 ? 4 : 0);\n"
				"    float lit = 0.25 + a * b * 0.6 + (c > 0.85 ? 0.6 : 0.0);\n"
				"    float g = host_sqrt(a < 0.0 ? 0.0 : a) - 0.55; float h = math::floor(b > c ? b : (c > 1.0 ? c : -c)) + 1.0;\n"
				"    float z = c + (i > 2 ? 1 : 0.5);\n"
				"    acc += (f < 0 ? 0 : (f > 9 ? 9 : f)) + facet + K;\n"
				"    s += lit + g + h + z + (f != 3 ? a : b) * 0.5; }\n"
				"  return s + acc; }\n"
				"to_string(run(64)) + \",\" + to_string(run(3));";
			both(src, "", "ternary math arguments");
			both(src, "", "ternary math arguments, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			const size_t fbeg = l.find("== run ");
			const std::string fl = fbeg == std::string::npos ? l : l.substr(fbeg, l.find("\n== ", fbeg + 1) - fbeg);
			check_true(fl.find("open loop") == std::string::npos && fl.find("OPERAND_TEMP") != std::string::npos && fl.find("BINARY op=") == std::string::npos, "the loop closes over the ternaries: " + fl);
			const std::string order = "int G = 1; float H = 2.0;\n"
				"function bumpg() -> bool { G = 100; return true; }\n"
				"function seth(float v) -> bool { H = v; return v > 0.0; }\n"
				"function run() -> string { string out = \"\";\n"
				"  for (int k = 0; k < 2; ++k) { G = 1; int r = G + (bumpg() ? 1 : 2); int r2 = G; G = 1; r2 += G + (G * 2 + (bumpg() ? 1 : 2)); out += to_string(r) + \" \" + to_string(r2) + \" \"; }\n"
				"  H = 2.0; float q = H * (H + (seth(5.0) ? H : 0.0)); H = 2.0; float q2 = host_sqrt(seth(9.0) ? H : 0.0) + H; H = 2.0; float q3 = H + host_sqrt(seth(16.0) ? H : 0.0);\n"
				"  H = 2.0; float q4 = H + math::floor(seth(3.5) ? H : 0.0); out += to_string(q) + \" \" + to_string(q2) + \" \" + to_string(q3) + \" \" + to_string(q4);\n"
				"  return out; }\n"
				"run();";
			both(order, "", "a ternary that runs user code keeps the backends in step");
			both(order, "", "a ternary that runs user code keeps the backends in step, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a graft is an observing point for the names read to its left, as the real call is: a name a write anywhere may reach (a
		// local a closure captures by reference, a reference declaration) keeps the call when the body writes a global, a field or an element
		// an element read that a later operand's user code may write is read before that operand runs (a binary's left operand,
		// an earlier element or entry of a container literal), as the interpreter reads it; a call argument binds late on both backends
		test("an_element_read_ahead_of_a_writing_call_reads_first", [this]() {
			const std::string src = "array<int> GA = [1, 2, 3];\n"
				"int G2 = 1;\n"
				"class B { int f = 1; }\n"
				"B GB = B();\n"
				"function wp() -> int { to_string(1); GA[1] = 100; return 1; }\n"
				"function wg() -> int { to_string(1); G2 = 100; return 1; }\n"
				"function wb() -> int { to_string(1); GB.f = 100; return 1; }\n"
				"function add2(int a, int b) -> int { to_string(1); return a + b; }\n"
				"function run() -> string { string out = \"\";\n"
				"  for (int k = 0; k < 2; ++k) {\n"
				"    int i = 1;\n"
				"    GA[1] = 2; int r1 = GA[i] + wp(); out += to_string(r1) + \" \";\n"
				"    G2 = 1; var& ref = G2; int r2 = ref + wg(); out += to_string(r2) + \" \";\n"
				"    GB.f = 1; int r3 = GB.f + wb(); out += to_string(r3) + \" \";\n"
				"    GA[1] = 2; bool r4 = GA[i] < wp() + 50; out += to_string(r4) + \" \";\n"
				"    GA[1] = 2; int r5 = add2(GA[i], wp()); out += to_string(r5) + \" \";\n"
				"    GA[1] = 2; int r6 = GA[1] + wp(); out += to_string(r6) + \" \";\n"
				"    GA[1] = 2; var t = [GA[i], wp()]; out += to_json(t) + \" \";\n"
				"    GA[1] = 2; var mm = {\"a\": GA[i], \"b\": wp()}; out += to_string(mm[\"a\"]) + \" \";\n"
				"    GA[1] = 2; int r8 = GA[i]; r8 += wp(); out += to_string(r8) + \" | \";\n"
				"  }\n"
				"  return out; }\n"
				"run();";
			const std::string expected = "3 2 2 true 3 3 [2,1] 2 3 | 3 2 2 true 3 3 [2,1] 2 3 | ";
			both(src, expected, "an element read ahead of a writing call");
			both(src, expected, "an element read ahead of a writing call, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("a_graft_that_writes_keeps_captured_and_reference_names_to_its_left", [this]() {
			const std::string src = "int G = 1;\n"
				"array<int> GA = [1, 2, 3];\n"
				"function bumpg() -> int { G = 100; return 1; }\n"
				"function bumpa() -> int { GA[1] = 100; return 1; }\n"
				"class C { int f = 1; int m() { f = 100; return 1; } int t() { return f + m(); } int t2() { return this.f + m(); } }\n"
				"function run() -> string { string out = \"\";\n"
				"  for (int k = 0; k < 2; ++k) {\n"
				"    int x = 1; var bump = [&]() { x = 100; return 1; };\n"
				"    int r1 = x + bump(); out += to_string(r1) + \" \";\n"
				"    x = 1; x += bump(); out += to_string(x) + \" \";\n"
				"    x = 1; int r3 = x * 2 + bump(); out += to_string(r3) + \" \";\n"
				"    int y = 1; var& ry = y; var bumpy = [&]() { y = 100; return 1; }; int r5 = ry + bumpy(); out += to_string(r5) + \" \";\n"
				"    G = 1; int r6 = G + bumpg(); out += to_string(r6) + \" \";\n"
				"    int i = 1; array<int> a = [1, 2, 3]; var bumpe = [&]() { a[1] = 100; return 1; }; int r7 = a[i] + bumpe(); out += to_string(r7) + \" \";\n"
				"    GA[1] = 2; var& rg = GA[1]; int r8 = rg + bumpa(); out += to_string(r8) + \" \";\n"
				"    C c = C(); out += to_string(c.t()) + \" \"; C c2 = C(); out += to_string(c2.t2()) + \" \";\n"
				"    int z = 1; var bz = [&]() { z = 100; return 1; }; int r9 = z + (bz() + 1); out += to_string(r9) + \" \";\n"
				"    int w = 1; G = 1; int r10 = w + bumpg(); out += to_string(r10) + \" | \";\n"
				"  }\n"
				"  return out; }\n"
				"run();";
			const std::string expected = "2 101 3 2 2 3 3 2 2 3 2 | 2 101 3 2 2 3 3 2 2 3 2 | ";
			both(src, expected, "names left of a writing graft");
			both(src, expected, "names left of a writing graft, proofs off", false);
			const std::string l = listing_run(src);
			check_true(l.find("-- inlined bumpg") != std::string::npos, "a private local left of a writing graft still grafts: " + l.substr(0, 400));
			const std::string loop = "int G = 0;\n"
				"function tick(int v) -> int { G = G + v; return v * 2; }\n"
				"function run(int n) -> string { int acc = 0; int s = 0; var& rs = s;\n"
				"  for (int i = 0; i < n; ++i) { int d = i & 3; acc = acc + tick(d); s = s + 1; int e = rs + tick(1); acc = acc + e; }\n"
				"  return to_string(acc) + \" \" + to_string(G) + \" \" + to_string(s); }\n"
				"run(40) + \",\" + run(3);";
			both(loop, "", "a writing graft in a loop");
			both(loop, "", "a writing graft in a loop, proofs off", false);
			const std::string more = "array<int> GA = [1, 2, 3, 4];\n"
				"function inner2() -> int { GA[2] = 100; return 1; }\n"
				"function outer2(var& r) -> int { int k = r + inner2(); return k; }\n"
				"function run() -> string { string out = \"\";\n"
				"  for (int k = 0; k < 2; ++k) {\n"
				"    int j = 0; array<int> a = [0, 0, 0]; var bumpj = [&]() { j = 2; return 7; }; a[j] = bumpj(); out += to_json(a) + \" \";\n"
				"    GA[2] = 3; var& rg3 = GA[2]; int r11 = outer2(rg3); out += to_string(r11) + \" \";\n"
				"    int q = 5; var bq = [&]() { q = 50; return 1; }; int r12 = q - bq(); out += to_string(r12) + \" \";\n"
				"    int u = 1; var bu = [&]() { u = 100; return 2; }; int r13 = u * bu() + u; out += to_string(r13) + \" | \";\n"
				"  }\n"
				"  return out; }\n"
				"run();";
			both(more, "[0,0,7] 4 4 102 | [0,0,7] 4 4 102 | ", "a store target, a nested graft through a reference, and arithmetic around a writing graft");
			both(more, "[0,0,7] 4 4 102 | [0,0,7] 4 4 102 | ", "a store target, a nested graft through a reference, and arithmetic around a writing graft, proofs off", false);
			// a grafted lambda reads its captures through the definition's bindings: a closure over an environment takes the fallback call
			const std::string scope = "int GX = 3;\n"
				"function mk() -> var { int x = 5; return [&]() { return x; }; }\n"
				"function mkv() -> var { int x = 6; return [=]() { return x; }; }\n"
				"function callit(var fn) -> int { int x = 900; int r = fn() + 0; return r; }\n"
				"function callit2(var fn) -> int { int GX = 800; int r = fn() + 0; return r; }\n"
				"function run() -> string { string out = \"\";\n"
				"  for (int k = 0; k < 2; ++k) {\n"
				"    int x = 1; var f = [&]() { return x; }; var fv = [=]() { return x; };\n"
				"    { int x = 50; int a = f() + 0; int b = fv() + 0; out += to_string(a) + \" \" + to_string(b) + \" \"; }\n"
				"    for (int x = 7; x < 8; ++x) { int c = f() + 0; int d = fv() + 0; out += to_string(c) + \" \" + to_string(d) + \" \"; }\n"
				"    var g = mk(); var gv = mkv(); { int x = 100; int e = g() + 0; int e2 = gv() + 0; out += to_string(e) + \" \" + to_string(e2) + \" \"; }\n"
				"    int y = 2; var h = [&]() { return x; }; h = [&]() { return y; }; int hh = h() + 0; out += to_string(hh) + \" \";\n"
				"    int p = callit(f); out += to_string(p) + \" \";\n"
				"    var gl = [&]() { return GX; }; { int GX = 44; int q = gl() + 0; out += to_string(q) + \" \"; }\n"
				"    int q2 = callit2(gl); out += to_string(q2) + \" | \";\n"
				"  }\n"
				"  return out; }\n"
				"run();";
			both(scope, "1 1 1 1 5 6 2 1 3 3 | 1 1 1 1 5 6 2 1 3 3 | ", "a lambda's captures across shadows, returns, reassignment and parameters");
			both(scope, "1 1 1 1 5 6 2 1 3 3 | 1 1 1 1 5 6 2 1 3 3 | ", "a lambda's captures across shadows, returns, reassignment and parameters, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (const std::string* p : { &src, &more, &scope, &loop }) {
				const std::string oracle = run_on(false, *p);
				for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
					auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
					std::string got;
					try { got = e->execute(*p).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
					check_eq(oracle, got, "a miss at every " + std::to_string(every));
				}
			}
		});
		// a compound store runs its value ahead of every part of its target (a member's receiver and read, a static, a namespace variable,
		// an element's container and index; docs/site/guide/04-operators.html "Evaluation order is a contract"), so a graft of the value's call keeps the order at every target (the
		// ordering fuzz band, seeds 0, 14, 28, 29, 31)
		test("a_compound_stores_value_runs_ahead_of_its_target_grafted_or_not", [this]() {
			const std::string src = "class K { int v = 1; static int s = 1; int bump() { v = v + 10; return 1; } int thisv() { this.v += bump(); return v; } }\n"
				"K O = K();\n"
				"namespace ns { int x = 1; }\n"
				"array<int> GA = [1, 2, 3];\n"
				"function wo() -> int { O.v = O.v + 10; return 1; }\n"
				"function ws() -> int { K::s = K::s + 10; return 1; }\n"
				"function wn() -> int { ns::x = ns::x + 10; return 1; }\n"
				"function wa() -> int { GA[1] = GA[1] + 10; return 1; }\n"
				"function pure() -> int { return 5; }\n"
				"function run() -> string { string out = \"\";\n"
				"  for (int k = 0; k < 2; ++k) {\n"
				"    O.v = 1; O.v += wo(); out += to_string(O.v) + \" \";\n"
				"    K::s = 1; K::s += ws(); out += to_string(K::s) + \" \";\n"
				"    ns::x = 1; ns::x += wn(); out += to_string(ns::x) + \" \";\n"
				"    K c = K(); var bc = [&]() { c.v = c.v + 10; return 1; }; c.v += bc(); out += to_string(c.v) + \" \";\n"
				"    K d = K(); out += to_string(d.thisv()) + \" \";\n"
				"    int j = 0; array<int> la = [0, 0, 0]; var bj = [&]() { j = 2; return 7; }; la[j] += bj(); out += to_json(la) + \" \";\n"
				"    int i = 1; GA[1] = 2; GA[i] += wa(); out += to_string(GA[1]) + \" \";\n"
				"    int n = 1; n += wo(); array<int> lb = [1, 2]; lb[i] += pure(); out += to_string(n) + \" \" + to_json(lb) + \" | \";\n"
				"  }\n"
				"  return out; }\n"
				"run();";
			const std::string expected = "12 12 12 12 12 [0,0,7] 13 2 [1,7] | 12 12 12 12 12 [0,0,7] 13 2 [1,7] | ";
			both(src, expected, "a compound store's value ahead of its target");
			both(src, expected, "a compound store's value ahead of its target, proofs off", false);
			const std::string l = listing_run(src);
			check_true(l.find("-- inlined pure") != std::string::npos && l.find("-- inlined wo") != std::string::npos, "a name's and a private element's compounds still graft: " + l.substr(0, 400));
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// an element read landed in a compiler temp holds the element's value as read there, never the entry reference: a map entry left
		// of a hoisted math call whose argument calls a writer of that entry reads ahead of the call (the ordering fuzz band, seed 241)
		test("an_entry_read_into_a_temp_is_its_value_where_it_is_read", [this]() {
			const std::string src = "map<string, int> M = {\"a\": 1};\n"
				"map<string, float> MF = {\"a\": 1.5};\n"
				"var MV = {\"a\": 1, \"b\": 2.5};\n"
				"function rec(int n) -> int { if (n <= 0) { return 0; } return rec(n - 1); }\n"
				"function wm() -> int { rec(0); M[\"a\"] = M[\"a\"] + 100; return 3; }\n"
				"function wg() -> int { M[\"a\"] = M[\"a\"] + 100; return 4; }\n"
				"function wf() -> int { rec(0); MF[\"a\"] = MF[\"a\"] + 100.0; return 9; }\n"
				"function wv() -> int { rec(0); MV[\"b\"] = MV[\"b\"] + 100; return 1; }\n"
				"function run() -> string { string out = \"\";\n"
				"  for (int k = 0; k < 2; ++k) {\n"
				"    M[\"a\"] = 1; int a = M[\"a\"] - math::abs(wm()); out += to_string(a) + \" \";\n"
				"    M[\"a\"] = 1; float b = M[\"a\"] - host_sqrt(wg()); out += to_string(b) + \" \";\n"
				"    MF[\"a\"] = 1.5; float c = MF[\"a\"] + math::floor(wf()); out += to_string(c) + \" \";\n"
				"    MV[\"b\"] = 2.5; var d = MV[\"b\"] * math::itrunc(wv()); out += to_string(d) + \" \";\n"
				"    M[\"a\"] = 1; int e = (M[\"a\"] + math::abs(wm())) * 2; out += to_string(e) + \" | \";\n"
				"  }\n"
				"  return out; }\n"
				"run();";
			const std::string expected = "-2 -1.0 10.5 2.5 8 | -2 -1.0 10.5 2.5 8 | ";
			both(src, expected, "an entry read ahead of a hoisted math call");
			both(src, expected, "an entry read ahead of a hoisted math call, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a private local is private to everything but a call that takes it as an argument: read in its operation after a hoisted ternary,
		// math call or call temp whose call binds it by reference, it is taken first (the ordering fuzz band, seed 249)
		test("a_local_a_later_call_takes_by_reference_is_read_first", [this]() {
			const std::string src = "function mutate(int& r) -> bool { r = r + 100; return true; }\n"
				"function bump(int& r) -> int { r = r + 100; return 1; }\n"
				"function wrap(int v) -> int { return v; }\n"
				"function run() -> string { string out = \"\";\n"
				"  for (int k = 0; k < 2; ++k) {\n"
				"    int x = 1; int a = x + (mutate(x) ? 1 : 2); out += to_string(a) + \" \";\n"
				"    int y = 1; int b = y + ((y * 2) - (mutate(y) ? 2 : 1)); out += to_string(b) + \" \";\n"
				"    int z = 1; int c = z + wrap(bump(z)); out += to_string(c) + \" \";\n"
				"    int w = 1; float d = w + math::abs(bump(w)); out += to_string(d) + \" \";\n"
				"    int u = 1; float e = u + host_sqrt(bump(u)); out += to_string(e) + \" \";\n"
				"    int v = 1; int f = (v * 3) + (v + (mutate(v) ? 1 : 2)); out += to_string(f) + \" | \";\n"
				"  }\n"
				"  return out; }\n"
				"run();";
			const std::string expected = "2 1 2 2.0 2.0 5 | 2 1 2 2.0 2.0 5 | ";
			both(src, expected, "a local read ahead of a call that takes it by reference");
			both(src, expected, "a local read ahead of a call that takes it by reference, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a graft's local or result declared from arithmetic the typed rows left general lands in the block by its declared type: an int
		// result takes a cast float, a float local or result widens an int (the ordering fuzz band's metamorphic variant, seed 266)
		test("a_grafts_declaration_from_general_arithmetic_lands_by_its_declared_type", [this]() {
			const std::string src = "int GI = 3;\n"
				"function w(int p) -> int { float tl = p + 0.25; return cast<int>(tl * 2); }\n"
				"function w4(int p) -> float { float t = p + 1; return t / 2; }\n"
				"function w5(float p) -> int { int t = cast<int>(p * 3); return t + 1; }\n"
				"function w6(int p) -> float { return p + GI; }\n"
				"function w7(float p) -> int { return cast<int>(p * 1.5); }\n"
				"function run() -> string { float fs = 0.0; float hs = 0.0;\n"
				"  for (int i = 0; i < 3; i++) {\n"
				"    var b = w(i); var c = w4(i); var e = w5(1.5); var f = w6(i); var g = w7(3.0); float h = w4(i) + w(i);\n"
				"    fs = fs * 10.0 + b + c + e + f + g; hs = hs + h;\n"
				"  }\n"
				"  return to_string(fs) + \" \" + to_string(hs); }\n"
				"run();";
			const std::string expected = "1429.5 9.0";
			both(src, expected, "graft declarations by their declared type");
			both(src, expected, "graft declarations by their declared type, proofs off", false);
			const std::string l = listing_run(src);
			check_true(l.find("-- cell loop") != std::string::npos && l.find("-- inlined w4") != std::string::npos, "the loop closes over the grafts: " + l.substr(0, 400));
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a call's reference result is an operand the language reads where it stands: ahead of a later operand that may run user code (a
		// binary's right side, a later container literal element) it is dereferenced first, as an element read is
		test("a_reference_result_is_read_ahead_of_a_writing_sibling", [this]() {
			const std::string src = "int G = 1;\n"
				"array<int> A = [1, 2];\n"
				"class B { int f = 1; function fr() -> int& { return this.f; } }\n"
				"B OB = B();\n"
				"int& get() { return G; }\n"
				"int& geta() { return A[1]; }\n"
				"function w() -> int { G = G + 100; A[1] = A[1] + 100; OB.f = OB.f + 100; return 1; }\n"
				"function run() -> string { string out = \"\";\n"
				"  for (int k = 0; k < 2; ++k) {\n"
				"    G = 1; int a = get() + w(); out += to_string(a) + \" \";\n"
				"    G = 1; int b = get() + (w() > 0 ? 1 : 2); out += to_string(b) + \" \";\n"
				"    G = 1; int c = get() - math::abs(w()); out += to_string(c) + \" \";\n"
				"    A[1] = 2; int d = geta() + w(); out += to_string(d) + \" \";\n"
				"    G = 1; var e = [get(), w()]; out += to_string(e[0]) + \" \";\n"
				"    OB.f = 1; int g = OB.fr() * w(); out += to_string(g) + \" | \";\n"
				"  }\n"
				"  return out; }\n"
				"string top = \"\"; G = 1; int t = get() + w(); top = to_string(t) + \" \";\n"
				"top + run();";
			const std::string expected = "2 2 0 3 1 1 | 2 2 0 3 1 1 | ";
			both(src, "2 " + expected, "a reference result ahead of a writing sibling");
			both(src, "2 " + expected, "a reference result ahead of a writing sibling, proofs off", false);
		});
		// an element read passed to a reference parameter is an lvalue: a block's call door whose parked callee takes that argument by
		// reference misses onto the general form, which pushes the element's reference (the ordering fuzz band's metamorphic variant, seeds
		// 68, 288, 384)
		test("an_element_argument_a_reference_parameter_binds_stays_an_lvalue_in_a_block", [this]() {
			const std::string src = "array<int> A = [2, 6, 6, -2];\n"
				"function rec(int n) -> int { if (n <= 0) { return 0; } return rec(n - 1); }\n"
				"function r8(int& x) -> int { rec(1); x = x + 5; return 4; }\n"
				"function v8(int x) -> int { rec(1); return x * 2; }\n"
				"function run() -> string { string out = \"\"; int s = 0;\n"
				"  int i = 0;\n"
				"  while (i < 3) { var a = r8(A[i % 4]); int b = v8(A[i % 4]); s = s + a + b; i = i + 1; }\n"
				"  for (int k = 0; k < 4; k++) { int c = r8(A[k]); s = s + c; }\n"
				"  out = to_string(s) + \" \" + to_json(A);\n"
				"  return out; }\n"
				"run();";
			const std::string expected = "86 [12,16,16,3]";
			both(src, expected, "an element argument bound by reference in a block");
			both(src, expected, "an element argument bound by reference in a block, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// `auto x = y;` takes y's declared type: from a var (a body declaration, a live-in local, a global) the local is a var, so a block
		// lands any kind in it and flushes it with the var's stamp, as the general form's declaration copies the stamp (the ordering fuzz band)
		test("an_auto_declaration_from_a_var_is_a_var_in_a_block", [this]() {
			const std::string src = "var GV = 1;\n"
				"function run() -> string { float a = 0.0; float b = 0.0; float c = 0.0; float d = 0.0;\n"
				"  var vv = 41;\n"
				"  for (int i = 0; i < 3; i++) {\n"
				"    var v4 = 41; auto e = v4; e += 1.5; a = a + e;\n"
				"    auto e2 = GV; e2 += 1.5; b = b + e2;\n"
				"    auto e3 = vv; e3 += 1.5; c = c + e3;\n"
				"    int n = 41; auto e4 = n; e4 = cast<int>(e4 + 1.5); d = d + e4;\n"
				"  }\n"
				"  return to_string(a) + \" \" + to_string(b) + \" \" + to_string(c) + \" \" + to_string(d); }\n"
				"run();";
			const std::string expected = "127.5 7.5 127.5 126.0";
			both(src, expected, "auto from a var in a block");
			both(src, expected, "auto from a var in a block, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// `auto x = e;` holds e's value as it is: a typed use that fixed the local's cell to another kind (a speculative `s += x` on an int s)
		// is a contract the landing meets or misses, never a declared type it converts to (the ordering fuzz band's metamorphic variant, seed 12176)
		test("an_auto_declaration_lands_its_values_own_kind_in_a_block", [this]() {
			const std::string src = "function rec(int n) -> int { if (n <= 0) { return 0; } return rec(n - 1); }\n"
				"function half() { rec(1); return -0.5; }\n"
				"function run() -> string {\n"
				"  var L = [=]() { rec(1); return -0.5; };\n"
				"  int s = 13; int t = 13; float f = 0.25; float g = 0.0;\n"
				"  for (int i = 0; i < 3; i++) { auto m = L(); s = cast<int>(s + m); }\n"
				"  for (int i = 0; i < 3; i++) { auto h = half(); t = cast<int>(t + h); }\n"
				"  for (int i = 0; i < 3; i++) { int n = 41 + i; auto k = n; f = f + k; g = g + k / 2; }\n"
				"  int u = 13; string refused = \"\";\n"
				"  try { for (int i = 0; i < 3; i++) { auto q = i < 1 ? rec(0) + 2 : half(); u += q; } } catch (err) { refused = err; }\n"
				"  return to_string(s) + \" \" + to_string(t) + \" \" + to_string(f) + \" \" + to_string(g) + \" \" + to_string(u) + \" \" + refused; }\n"
				"run();";
			const std::string expected = "10 10 126.25 62.0 15 Cannot assign float to 'u' (declared int)";
			both(src, expected, "auto from a call result in a block");
			both(src, expected, "auto from a call result in a block, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a compiler temp's kind that a typed read fixed binds only the definition that read sees: an earlier value the statement kept in the same
		// slot (a math result, a graft's own float temp the caller's int temp reuses after the join) lands as it is or misses, never converted
		// (the ordering fuzz band, seed 12084)
		test("a_temp_a_typed_read_fixes_converts_no_other_value_in_a_block", [this]() {
			const std::string src = "int G0 = 3;\n"
				"array<float> A = [1.5, 4.5, 0.25];\n"
				"function w5() -> int { A[2] = 3.0 - A[2]; return 5; }\n"
				"function run() -> string {\n"
				"  int ai = 0; int n = 2; var v = 0.25; var u = 0.25; float fb = 0.0; float fc = 0.0;\n"
				"  for (int i = 0; i < 3; i++) { int r = G0 * (-w5()); ai = ai + r; }\n"
				"  for (int i = 0; i < 4; i++) { fb = fb + math::abs(v - 2.0) * 1.0 + (G0 * (-n)); v = v + 0.25; }\n"
				"  for (int i = 0; i < 4; i++) { fc = fc + host_sqrt(u * u * 36.0) + (-n) * G0; u = u + 0.25; }\n"
				"  return to_string(ai) + \" \" + to_string(A[2]) + \" \" + to_string(fb) + \" \" + to_string(fc); }\n"
				"run();";
			const std::string expected = "-45 2.75 -18.5 -9.0";
			both(src, expected, "a fixed temp kind in a block");
			both(src, expected, "a fixed temp kind in a block, proofs off", false);
			const std::string l = listing_run(src);
			check_true(l.find("-- cell loop") != std::string::npos && l.find("-- inlined w5") != std::string::npos, "the loop closes over the graft: " + l.substr(0, 400));
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a literal on the left of a block's comparison or of a computed call argument: the pinned cell forms read their left operand as a cell,
		// so the emission mirrors a comparison and a commutative operator, and keeps any other operator on the any-kind form
		test("literal_left_operands_in_blocks", [this]() {
			const std::string src =
				"int g(int x) { if (x > 100) { return g(x - 1); } return x; }\n"
				"float gf(float x) { if (x > 100.0) { return gf(x - 1.0); } return x; }\n"
				"function a1() -> int { int s = 0; for (int i = 0; i < 5; ++i) { s = s + g(10 - i); } return s; }\n"
				"function a2() -> int { int s = 0; for (int i = 1; i < 6; ++i) { s = s + g(60 / i) + g(7 * i); } return s; }\n"
				"function a3() -> float { float s = 0.0; for (int i = 0; i < 6; ++i) { float x = i * 0.5; s = s + gf(3.0 - x) + gf(2.0 * x); } return s; }\n"
				"function c1() -> int { int s = 0; int z = 100; for (int i = 0; i < 10; ++i) { if (5 < i || i == 0) { s = s + 1; } } return s; }\n"
				"function c2() -> int { int s = 0; for (int i = 0; i < 10; ++i) { int k = 7; if (6 < i || k == 0) { s = s + i; } } return s; }\n"
				"function c3() -> int { int s = 0; for (int i = 0; i < 10; ++i) { if (3 >= i && 1 != i) { s = s + 10; } } return s; }\n"
				"function c4() -> int { int s = 0; for (int i = 0; i < 8; ++i) { float f = i * 0.25; if (1.0 <= f || 0.5 == f) { s = s + i; } } return s; }\n"
				"to_string(a1()) + \" \" + to_string(a2()) + \" \" + to_string(a3()) + \" \" + to_string(c1()) + \" \" + to_string(c2()) + \" \" + to_string(c3()) + \" \" + to_string(c4());";
			both(src, "40 242 25.5 5 24 30 24", "literal left operands");
			both(src, "40 242 25.5 5 24 30 24", "literal left operands, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// raw typed frames: a body whose parameters are by-value scalars and whose calls reach
		// functions of the same shape is one frame program; a call between frame programs passes its arguments and its result raw
		test("raw_frames_recursion_and_chains_land_the_same", [this]() {
			const std::string src =
				"int fib(int n) { if (n < 2) { return n; } return fib(n - 2) + fib(n - 1); }\n"
				"float fsum(float x, int n) { if (n < 1) { return x; } return fsum(x * 0.5, n - 1) + 1.0; }\n"
				"bool is_even(int n) { if (n == 0) { return true; } return is_odd(n - 1); }\n"
				"bool is_odd(int n) { if (n == 0) { return false; } return is_even(n - 1); }\n"
				"int seven() { return 7; }\n"
				"int add3(int a, int b, int c) { return a + b + c; }\n"
				"int nest(int n) { if (n < 1) { return seven(); } int k = n * 3; int r = nest(n - 1); return r + k; }\n"
				"int mix(int n, float f, bool b) { if (n < 1) { if (b) { return 1; } return 2; } return mix(n - 1, f * 2.0, !b) + n; }\n"
				"string out = to_string(fib(20)) + \" \" + to_string(fsum(3.0, 10)) + \" \" + to_string(is_even(31)) + \" \" + to_string(is_odd(31)) + \" \" + to_string(nest(12)) + \" \" + to_string(mix(9, 0.5, true)) + \" \" + to_string(add3(1, 2, 3) + fib(10));\n"
				"int acc = 0; for (int i = 0; i < 15; ++i) { acc = acc + fib(i); } out + \" \" + to_string(acc);";
			both(src, "6765 10.0029296875 false true 241 47 61 986", "recursion, chains and a program-scope loop");
			both(src, "6765 10.0029296875 false true 241 47 61 986", "recursion, chains and a program-scope loop, proofs off", false);
			const std::string l = listing_run(src);
			const size_t fb = l.find("== fib ");
			const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
			check_true(fl.find("-- frame program") != std::string::npos && fl.find("(raw entry)") != std::string::npos && fl.find("raw 2") != std::string::npos, "fib is a raw-enterable frame program with two raw calls: " + fl);
			check_true(fl.find("C_RET") != std::string::npos, "its returns are rets: " + fl);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 4u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("raw_frames_nested_calls_in_raw_arguments_park_in_order", [this]() {
			// a call inside a raw call's arguments (a raw one: Ackermann; a general leaf; a nested chain in a program-scope loop): the outer
			// callee parks before the inner probe, so a miss anywhere in the arguments resumes the general form with the callees stacked in order
			const std::string src =
				"int ack(int m, int n) { if (m == 0) { return n + 1; } if (n == 0) { return ack(m - 1, 1); } return ack(m - 1, ack(m, n - 1)); }\n"
				"int twice(int x) { return x * 2; }\n"
				"int down(int n) { if (n < 1) { return 0; } return down(twice(n) - n - 1) + 1; }\n"
				"int pair(int a, int b) { if (a < 1) { return b; } return pair(a - 1, pair(0, a) + b); }\n"
				"int acc = 0; for (int i = 0; i < 6; ++i) { acc = acc + ack(1, ack(1, i)) + down(i); }\n"
				"to_string(ack(2, 3)) + \" \" + to_string(ack(3, 3)) + \" \" + to_string(down(9)) + \" \" + to_string(pair(5, 2)) + \" \" + to_string(acc);";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("9 61 9 ") == 0, "the interpreter's answer: " + oracle);
			both(src, oracle, "nested calls in raw arguments");
			both(src, oracle, "nested calls in raw arguments, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u, 11u, 13u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("raw_frames_raise_the_general_texts", [this]() {
			// an overflow deep in a raw chain, a division by zero, the depth limit, a raw callee reaching a general function: every raise happens on the
			// general form after the frame leaves its cells, so the texts and the catching frame are the interpreter's; a frame the raise unwinds
			// leaves its record with no block, so a loop-free body catching in that record later runs its handler
			const std::string src =
				"int big(int n) { if (n == 0) { return 9223372036854775807; } return big(n - 1) + 1; }\n"
				"string handled() { string s = \"\"; try { throw \"x\"; } catch (e) { s = \"D:\" + e + \" \"; } return s; }\n"
				"int divz(int n, int d) { if (n == 0) { return 10 / d; } return divz(n - 1, d) + 1; }\n"
				"int deep(int n) { if (n == 0) { return 0; } return deep(n - 1) + 1; }\n"
				"var LOG = [];\n"
				"int note(var v) { LOG.push(v); return 1; }\n"
				"int walk(int n) { if (n == 0) { return note(n); } return walk(n - 1) + note(n); }\n"
				"string out = \"\";\n"
				"try { out += to_string(big(5)); } catch (e) { out += \"A:\" + e + \" \"; }\n"
				"try { out += to_string(divz(4, 0)); } catch (e) { out += \"B:\" + e + \" \"; }\n"
				"try { out += to_string(deep(100000000)); } catch (e) { out += \"C:\" + e + \" \"; }\n"
				"out += handled();\n"
				"out += to_string(walk(6)) + \" \" + to_string(LOG.size()) + \" \" + to_string(deep(50));\n"
				"out;";
			both(src, "", "raises in raw chains");
			both(src, "", "raises in raw chains, proofs off", false);
			const std::string vm = run_on(true, src);
			check_true(vm.find("A:") != std::string::npos && vm.find("B:") != std::string::npos && vm.find("C:") != std::string::npos && vm.find("D:x 7 7 50") != std::string::npos, "each raise caught, the walk ran whole: " + vm);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->max_call_depth(backend_type::vm, 10000); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("raw_frames_follow_a_rebound_callee_and_a_converting_argument", [this]() {
			// the raw call reads the callee's binding at every call: a reload that rebinds it (another body, another parameter kind) is the general
			// call's; an argument of another kind than its parameter takes the binder's conversion on the general door
			auto session = [](bool vm) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string out;
				try {
					e->execute("int g(int x) { return x * 2; }\nint f(int n) { if (n < 1) { return 0; } return f(n - 1) + g(n); }\nfloat h(float x) { if (x < 1.0) { return x; } return h(x - 1.0) + 0.5; }\nfloat w(int n) { return h(n); }");
					out += e->execute("to_string(f(5)) + \" \" + to_string(w(3));").as_string();
					e->execute("float g(float x) { return x * 3.5; }");
					out += " " + e->execute("to_string(f(5));").as_string();
					e->execute("int g(int x) { return x + 100; }");
					out += " " + e->execute("to_string(f(5));").as_string();
				} catch (const std::exception& ex) { out += std::string(" throw:") + ex.what(); }
				return out;
			};
			const std::string oracle = session(false);
			check_eq(oracle, session(true), "a rebound callee and a converting argument, both backends");
			check_true(oracle.find("throw:") == std::string::npos, "runs: " + oracle);
		});
		test("boundary_operations_keep_the_loop_a_block", [this]() {
			// one statement the block does not take (a map store) stays its own cost: the loop is one block entry, the statement runs on the general
			// form every iteration with the block parked, and the block resumes after it (the counters, not a clock, pin the shape)
			const std::string defs =
				"map<int, int> m = {};\n"
				"function tail_store() -> int { int s = 0; for (int i = 0; i < 1000; ++i) { s = (s + i * 3) % 1000003; s = s ^ (i & 255); m[i & 7] = s; } return s; }\n"
				"function mid_store() -> int { int s = 0; int t = 0; for (int i = 0; i < 1000; ++i) { s = s + i; m[i & 3] = s; t = t + s * 2; } return t; }\n"
				"function while_store() -> int { int s = 0; int i = 0; while (i < 1000) { s = s + i; m[i & 3] = s + 1; m[(i + 1) & 3] = s; i = i + 1; } return s; }\n"
				"function writes_cell() -> int { int s = 1; int t = 0; for (int i = 0; i < 200; ++i) { m[i & 3] = i; s = m[i & 3] + s; t = t + s * 3; t = t ^ (i & 7); } return t; }\n"
				"function thin() -> int { int t = 0; for (int i = 0; i < 300; ++i) { m[i & 3] = i + t; } return m[2]; }\n";
			const std::string src = defs + "to_string(tail_store()) + \" \" + to_string(mid_store()) + \" \" + to_string(while_store()) + \" \" + to_string(writes_cell()) + \" \" + to_string(thin()) + \" \" + to_string(m[1]);";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "boundary operations");
			both(src, oracle, "boundary operations, proofs off", false);
			const std::string l = listing_run(src);
			const size_t tb = l.find("== tail_store ");
			const std::string tl = tb == std::string::npos ? l : l.substr(tb, l.find("\n== ", tb + 1) - tb);
			check_true(tl.find("(boundaries") != std::string::npos && tl.find("C_BOUNDARY") != std::string::npos && tl.find("-- open loop") == std::string::npos, "tail_store's loop closes with a boundary: " + tl);
			auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); stdlib::register_all(*e); register_host_math_fixture(*e);
			e->execute(defs);
			struct shape { const char* call; uint64_t iterations; uint64_t boundaries_per_iteration; };
			for (const shape& sh : { shape{ "tail_store();", 1000, 1 }, shape{ "mid_store();", 1000, 1 }, shape{ "while_store();", 1000, 1 }, shape{ "writes_cell();", 200, 1 } }) {
				const cell_counters c0 = detail::engine_access::cells_counters(*e);
				e->execute(sh.call);
				const cell_counters c1 = detail::engine_access::cells_counters(*e);
				const std::string label = std::string(sh.call) + ": entries " + std::to_string(c1.entries - c0.entries) + ", boundaries " + std::to_string(c1.boundaries - c0.boundaries) +
				                          ", resumes " + std::to_string(c1.resumes - c0.resumes) + ", misses " + std::to_string(c1.misses - c0.misses);
				check_true(c1.entries - c0.entries <= 2, "one block entry per loop run (the first iteration may run general): " + label);
				check_true(c1.boundaries - c0.boundaries >= sh.iterations * sh.boundaries_per_iteration - 2, "the statement runs as a boundary every iteration: " + label);
				check_true(c1.resumes - c0.resumes + 2 >= c1.boundaries - c0.boundaries, "and the block resumes after it: " + label);
				check_eq(uint64_t{0}, c1.misses - c0.misses, "nothing misses: " + label);
			}
			{
				// a loop whose body is the statement alone stays general: a block would pay a park and a resume per iteration for the counter
				const cell_counters c0 = detail::engine_access::cells_counters(*e);
				e->execute("thin();");
				const cell_counters c1 = detail::engine_access::cells_counters(*e);
				check_eq(uint64_t{0}, c1.entries - c0.entries, "the thin loop enters no block");
				check_eq(uint64_t{0}, c1.boundaries - c0.boundaries, "and parks nothing");
			}
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("a_boundary_statement_inside_a_graft_body_keeps_the_loop_a_block", [this]() {
			// a statement of a grafted callee the block does not take (a map store, a member store, a map read through a member key) is a boundary
			// of its own: the callee's parameters and locals live after it are read back at the resume, one it declares defines its cell
			const std::string defs =
				"map<string, int> M = {\"a\": 4, \"q\": 9};\n"
				"class Box { int v = 0; string s = \"a\"; }\n"
				"Box B;\n"
				"int bump(int i, int j) { int k = i & 7; M[\"a\"] = i; int r = k + j; B.s = i % 3 == 0 ? \"q\" : \"a\"; return r; }\n"
				"int reread(int i) { int k = i & 7; k = k + M[B.s] % 5; int z = M[B.s] % 3 + k; return k * 10 + z; }\n"
				"int run(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + bump(i, 3) * 2; s = (s + reread(i)) % 100003; } return s; }\n";
			const std::string src = defs + "to_string(run(1000)) + \" \" + to_string(M[\"a\"]) + B.s;";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "graft boundaries");
			both(src, oracle, "graft boundaries, proofs off", false);
			const std::string l = listing_run(src);
			const size_t rb = l.find("== run ");
			const std::string rl = rb == std::string::npos ? l : l.substr(rb, l.find("\n== ", rb + 1) - rb);
			check_true(rl.find("C_BOUNDARY") != std::string::npos && rl.find("defines") != std::string::npos && rl.find("-- open loop") == std::string::npos, "run's loop closes with boundaries inside its grafts: " + rl);
			auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); stdlib::register_all(*e);
			e->execute(defs);
			const cell_counters c0 = detail::engine_access::cells_counters(*e);
			e->execute("run(500);");
			const cell_counters c1 = detail::engine_access::cells_counters(*e);
			const std::string label = "entries " + std::to_string(c1.entries - c0.entries) + ", boundaries " + std::to_string(c1.boundaries - c0.boundaries) + ", resumes " + std::to_string(c1.resumes - c0.resumes) + ", misses " + std::to_string(c1.misses - c0.misses);
			check_true(c1.entries - c0.entries <= 2 && c1.boundaries - c0.boundaries >= 4 * 500 - 8 && c1.resumes - c0.resumes + 8 >= c1.boundaries - c0.boundaries, "one entry, four boundaries an iteration, each resumed: " + label);
			check_eq(uint64_t{0}, c1.misses - c0.misses, "nothing misses: " + label);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("scalar_math_rows_run_in_temps_and_cells", [this]() {
			// the one-argument scalar library (exp, log, log2, log10, exp2, cbrt, trunc, asin, acos, atan, sinh, cosh, tanh, saturate, sign) over
			// int and float arguments: a compiler temp lands the kernel's value and kind, the block runs them as cell math
			const std::string defs =
				"float f(int n) { float s = 0.0; for (int i = 0; i <= n; ++i) { float x = 1.0 / (1.0 + i); s += math::exp(x) + math::log2(1.0 + i) - math::tanh(0.25 * i) + math::sign(i - 50);"
				" s += math::cbrt(x) + math::trunc(x * 7.5) + math::saturate(x * 3.0 - 1.0) + math::asin(x) + math::acos(x) + math::atan(x * 9.0) + math::sinh(x) + math::cosh(x);"
				" s += math::log(1.0 + i) + math::log10(2.0 + i) + math::exp2(x) + math::sign(0.5 - x) + math::exp(i % 3) + math::sign(i % 4 - 2); } return s; }\n"
				"float g(var v) { float s = 0.0; for (int i = 0; i < 4; ++i) { s += math::exp(v); s += math::sign(v); } return s; }\n";
			const std::string src = defs + "to_string(f(300)) + \" \" + to_string(math::sign(-3)) + \" \" + type_of(math::sign(2.5)) + \" \" + to_string(math::log(0.0)) + \" \" + to_string(g(1)) + \" \" + to_string(g(0.5)) + \" \" + to_string(math::asin(2.0));";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "scalar math rows");
			both(src, oracle, "scalar math rows, proofs off", false);
			const std::string bad = defs + "g(\"x\");";
			check_eq(run_on(false, bad), run_on(true, bad), "a string argument raises the kernel's text on both backends");
			const std::string l = listing_run(src);
			const size_t fb = l.find("== f ");
			const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
			check_true(fl.find("-- cell loop") != std::string::npos && fl.find("math w") != std::string::npos && fl.find("-- open loop") == std::string::npos, "f's loop closes with its math in cells: " + fl);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("the_trig_root_rounding_abs_and_angle_rows_are_one_table", [this]() {
			// sin cos tan sqrt floor ceil abs and the angle conversions over int and float arguments, outside a loop (a compiler temp), as a
			// statement-level call (op_math), inside a block and through a host alias: abs keeps an int an int, abs(INT64_MIN) raises
			// the kernel's text, a non-number raises the kernel's text
			const std::string defs =
				"float one(var x) { return math::sin(x) + math::cos(x) * 2.0 + math::tan(x) - math::sqrt(math::abs(x)) + math::floor(x) * 3.0 + math::ceil(x) + math::radians_to_degrees(x) + math::degrees_to_radians(x); }\n"
				"string kinds(var x) { return type_of(math::abs(x)) + \",\" + type_of(math::floor(x)) + \",\" + type_of(math::sqrt(x)) + \",\" + type_of(host_radians(x)); }\n"
				"float loop(int n) { float s = 0.0; for (int i = -n; i <= n; ++i) { s += math::sin(i * 0.1) + math::abs(i) + math::floor(i * 0.3) + math::ceil(i * 0.7) + math::sqrt(i * i) + math::cos(i) + math::tan(i * 0.01) + math::degrees_to_radians(i); } return s; }\n"
				"string bad(var x) { string out = \"\"; try { math::abs(x); } catch (e) { out = e; } return out; }\n";
			const std::string src = defs + "to_string(one(3)) + \" \" + to_string(one(-2.75)) + \" \" + to_string(one(0)) + \" \" + kinds(-7) + \" \" + kinds(-7.5) + \" \" + to_string(math::abs(-9))"
				" + \" \" + to_string(loop(200)) + \" \" + bad(-9223372036854775807 - 1) + \" \" + bad(\"x\") + \" \" + to_string(host_radians(-4)) + \" \" + to_string(host_radians(2.5));";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos && oracle.find("int,float,float,float") != std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "one scalar table");
			both(src, oracle, "one scalar table, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em); register_host_math_fixture(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("enum_arguments_and_guarded_element_members_in_ternary_arms_keep_the_loop_a_block", [this]() {
			// Gator Quest's status_tick: an enum value by name as a call's argument (`fx(Fx.fire, m)`), a ternary whose arms read members of
			// elements of two global tables (the second through a guarded index, its temp slot reused by the other arm), a true-sense compare
			const std::string defs =
				"enum Fx { sparks, blood, splash, fire, frost }\n"
				"class Pt { int x = 3; int y = 4; }\n"
				"Pt P;\n"
				"class Ch { int buff = 0; }\n"
				"class Mo { int fireproof = 0; int regen = 1; }\n"
				"array<Ch> CH; array<Mo> MO; array<int> KIND; array<int> SLOT; array<int> HP; array<int> LOG;\n"
				"int TICK = 5;\n"
				"void fx(int kind, int amount) { LOG.push(kind * 10 + amount); }\n"
				"int run(int n) { int s = 0; for (int m = 0; m < n; ++m) { if (KIND[m] < 0 || HP[m] <= 0) { continue; } int slot = SLOT[m];"
				" bool proof = slot >= 0 ? CH[slot].buff > TICK : MO[KIND[m]].fireproof != 0; if (!proof) { fx(Fx.fire, m); s = s + 1; }"
				" if (m % 3 == 0) { fx(Fx.frost, P.x); } s = s + HP[m]; } return s; }\n"
				"for (int k = 0; k < 4; ++k) { Ch c; c.buff = k * 3; CH.push(c); Mo mo; mo.fireproof = k % 2; MO.push(mo); }\n"
				"for (int i = 0; i < 200; ++i) { KIND.push(i % 5 - 1); SLOT.push(i % 7 == 0 ? i % 4 : -1); HP.push(i % 6); }\n";
			const std::string src = defs + "string a = to_string(run(200)) + \" \" + to_string(LOG.size()) + \" \" + to_string(LOG[LOG.size() - 1]);"
			                               " P.x = 9; MO[1].fireproof = 0; CH[2].buff = 1; a + \" \" + to_string(run(200)) + \" \" + to_string(LOG[LOG.size() - 1]);";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "status loop");
			both(src, oracle, "status loop, proofs off", false);
			const std::string l = listing_run(src);
			const size_t rb = l.find("== run ");
			const std::string rl = rb == std::string::npos ? l : l.substr(rb, l.find("\n== ", rb + 1) - rb);
			check_true(rl.find("-- cell loop") != std::string::npos && rl.find("-- open loop") == std::string::npos && rl.find("C_REBIND") != std::string::npos, "run's loop closes: " + rl);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("field_reads_through_a_class_reference_are_typed_rows", [this]() {
			// `Body& b = bodies[i]; px = px + b.vx * m` takes the field's declared kind (the binding admits Body or a subclass): the arithmetic and
			// the store run pinned; a subclass element, a user accessor, an int field, a slot two blocks declare with different types and a reload
			// that retypes the field agree with the interpreter
			const std::string defs = R"(
				class Body { float x = 0.0; float vx = 0.0; int n = 0; Body(float a, float b, int c) { x = a; vx = b; n = c; } }
				class Heavy : Body { float extra = 2.0; Heavy(float a, float b, int c) : super(a, b, c) {} }
				class Acc { float raw = 1.0; float _get_v() { return raw * 2.0; } Acc(float r) { raw = r; } }
				float f(array<Body>& bs, int count) {
					float px = 0.0; int tally = 0;
					for (int i = 0; i < count; ++i) { Body& b = bs[i]; float m = b.x; px = px + b.vx * m; if (b.n > 2) { tally = tally + b.n; } b.x = b.x + b.vx; }
					return px + tally;
				}
				float g(array<Acc>& as) { float s = 0.0; for (int i = 0; i < as.size(); ++i) { Acc& a = as[i]; s = s + a.v * 0.5; } return s; }
				float h(array<Body>& bs, int k) {
					float out = 0.0;
					if (k > 0) { int t = 0; for (int i = 0; i < bs.size(); ++i) { Body& b = bs[i]; t = t + b.n; } out = t; }
					else { float t = 0.5; for (int i = 0; i < bs.size(); ++i) { Body& b = bs[i]; t = t + b.x; } out = t; }
					return out;
				}
				array<Body> BS = [Body(1.0, 0.5, 1), Heavy(2.0, -0.25, 3), Body(0.5, 2.0, 5)];
				array<Acc> AS = [Acc(1.0), Acc(3.0)];
			)";
			const std::string src = std::string(defs) + R"(
				to_string(f(BS, 3)) + " " + to_string(f(BS, 3)) + " " + to_string(g(AS)) + " " + to_string(h(BS, 1)) + " " + to_string(h(BS, 0));
			)";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "class reference field reads");
			both(src, oracle, "class reference field reads, proofs off", false);
			const std::string l = listing_run(src);
			const size_t fb = l.find("== f ");
			const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
			check_true(fl.find("-- cell loop") != std::string::npos && fl.find("C_ARITH_ANY") == std::string::npos && fl.find("C_CMP_ANY") == std::string::npos, "f's loop runs pinned: " + fl);
			const std::string reload = "class Body { int x = 0; float vx = 0.0; int n = 0; Body(float a, float b, int c) { x = a; vx = b; n = c; } }\n"
			                           "BS = [Body(1.0, 0.5, 1), Body(4.0, 1.0, 3)];\n";
			auto sequence = [&](bool vm) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(vm ? backend_type::vm : backend_type::interpreter); stdlib::register_all(*e);
				std::string got;
				try { e->execute(defs); got = e->execute("to_string(f(BS, 3))").as_string(); e->execute(reload); got += " " + e->execute("to_string(f(BS, 2))").as_string(); }
				catch (const std::exception& ex) { got += std::string(" throw:") + ex.what(); }
				return got;
			};
			check_eq(sequence(false), sequence(true), "a reload that retypes the field");
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("an_unclassed_element_read_feeding_a_float_row_keeps_its_kind_checked", [this]() {
			// JaiDOOM's point_side: `NX[n] * dy` over an int table is unclassed and its float result feeds a typed subtraction; the element lands
			// as an int, so the multiply reads its kind (a float contract fixed by the typed read is no proof of the landing)
			const std::string src = R"(
				array<int> NX = [3, -2, 7, 1, -5, 4];
				array<int> NY = [1, 6, -3, 2, 2, -7];
				int side(float x, float y, int n) { float dx = x - 0.5; float dy = y + 0.25; float cross = NX[n] * dy - NY[n] * dx; return cross > 0.0 ? 1 : 0; }
				int walk(float x, float y) { int s = 0; for (int n = 0; n < 6; ++n) { if (side(x, y, n) == 0) { s = s * 2; } else { s = s * 2 + 1; } } return s; }
				int t = 0;
				for (int i = 0; i < 20; ++i) { t = t * 3 + walk(i * 0.7 - 5.0, 3.0 - i * 0.4); t = t % 1000003; }
				to_string(t);
			)";
			both(src, "803764", "an int element under a float row");
			both(src, "803764", "an int element under a float row, proofs off", false);
		});
		test("an_alias_rebind_resolves_each_field_and_prefix_once", [this]() {
			// `for (auto& p : pool)` reading and storing p.pos.x, p.pos.y and p.life: one probe per field and prefix per element; elements whose
			// pos objects are different classes (x at another slot), a subclass element and a null pos agree with the interpreter
			const std::string src = R"(
				class V2 { float x = 0.0; float y = 0.0; V2(float a, float b) { x = a; y = b; } }
				class V3 { float z = 9.0; float y = 0.0; float x = 0.0; V3(float a, float b) { x = a; y = b; } }
				class P { var pos; var vel; int life = 3; P(var p, var v) { pos = p; vel = v; } }
				class Q : P { int tag = 1; Q(var p, var v) : super(p, v) {} }
				float step(array<P>& pool, int rounds) {
					float s = 0.0;
					for (int r = 0; r < rounds; ++r) {
						for (auto& p : pool) { p.pos.x = p.pos.x + p.vel.x; p.pos.y = p.pos.y + p.vel.y; p.life = p.life - 1; if (p.pos.x > 4.0) { p.pos.x = 0.0; } s = s + p.pos.x + p.pos.y; }
					}
					return s;
				}
				array<P> POOL = [P(V2(0.0, 1.0), V2(0.5, 0.25)), P(V3(1.0, 2.0), V2(1.5, -0.5)), Q(V2(3.0, 0.0), V3(0.25, 0.75)), P(V3(0.0, 0.0), V3(2.0, 2.0))];
				string bad() { array<P> b = [P(V2(1.0, 1.0), V2(1.0, 1.0)), P(null, V2(1.0, 1.0))]; string out = ""; try { step(b, 2); } catch (e) { out = "caught"; } return out; }
				to_string(step(POOL, 6)) + " " + to_string(POOL[1].pos.x) + " " + to_string(POOL[2].life) + " " + bad();
			)";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "alias rebind memo");
			both(src, oracle, "alias rebind memo, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("var_locals_whose_scope_ends_each_iteration_keep_the_loop_a_block", [this]() {
			// `var` body locals die at the body's closing brace (KILL_SLOT: a var may own a destructor); a cell holds only scalars, so the
			// block keeps them and the general form kills them; an iteration that stores an instance with a destructor into such a var runs
			// its destructor at the same point as the interpreter, nested loops and a continue included
			const std::string defs = R"(
				string LOG = "";
				class D { int n = 0; D(int v) { n = v; } ~D() { LOG = LOG + "d" + to_string(n); } }
				function sum(var& xs, var n) {
					var s = 0.0;
					for (var i = 0; i < n; ++i) {
						var a = xs[i] * 2.0;
						for (var j = i + 1; j < n; ++j) { var& y = xs[j]; var d = a - y; var q = d * d; if (q > 50.0) { continue; } s = s + q; }
						var b = a + 1;
						s = s + b;
					}
					return s;
				}
				function mixed(var n) {
					var t = 0;
					for (var i = 0; i < n; ++i) { var v = i * 3; if (i % 4 == 2) { v = D(i); LOG = LOG + "m"; } else { t = t + v; } LOG = LOG + "."; }
					return t;
				}
				var XS = [1.5, 2.0, -3.25, 4.0, 0.5, 6.0];
			)";
			const std::string src = std::string(defs) + R"(
				to_string(sum(XS, 6)) + " " + to_string(mixed(11)) + " " + LOG;
			)";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos && oracle.find("d2") != std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "var locals killed per iteration");
			both(src, oracle, "var locals killed per iteration, proofs off", false);
			const std::string l = listing_run(src);
			const size_t fb = l.find("== sum ");
			const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
			check_true(fl.find("-- cell loop") != std::string::npos && fl.find("-- open loop") == std::string::npos && fl.find("boundaries") == std::string::npos, "sum's loops are blocks with no boundary: " + fl);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("a_graft_reads_a_read_only_scalar_parameter_from_the_argument", [this]() {
			// a scalar value parameter the body never writes, bound to a private local no other argument writes, is the local itself inside the
			// graft; a parameter the body writes, one another argument increments or binds by reference, and a converting one keep their copy
			const std::string src = R"(
				int add3(int a, int b, int c) { return a + b * 2 + c * 3; }
				int dec(int n) { n = n - 1; return n * 2; }
				int incref(int a, int& b) { b = b + 10; return a + b; }
				float half(float f) { return f * 0.5; }
				int pair(int a, int b) { return a * 100 + b; }
				int run(int n) {
					int s = 0;
					for (int i = 0; i < n; ++i) {
						int x = i % 7; int y = i % 5;
						s = s + add3(x, y, i);
						int before = x;
						s = s + dec(x);
						if (x != before) { s = -999999; }
						s = s + incref(y, y);
						int z = i;
						s = s + pair(z, z++);
						s = s + cast<int>(half(i));
					}
					return s;
				}
				to_string(run(300)) + " " + to_string(add3(1, 2, 3));
			)";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "forwarded parameters");
			both(src, oracle, "forwarded parameters, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("a_bitwise_int_store_is_a_typed_store", [this]() {
			// `cur = (last + cur) & mask` on int locals is a parse-proven typed store (the int rows of & | ^ << >>): the block runs it pinned; a
			// shift out of range raises the general text with the slot unchanged, on both backends
			const std::string src = R"(
				int f(int n) { int last = 0; int cur = 1; int x = 3; while (n > 0) { n = n - 1; int tmp = cur; cur = (last + cur) & 0xFFFFFFFF; last = tmp; x = (x ^ cur) | (cur << 3) >> 2; } return cur + x; }
				int g(int s) { int v = 1; string out = ""; try { for (int i = 0; i < 3; ++i) { v = v << s; } } catch (e) { out = e; } return v * 1000 + out.size(); }
				to_string(f(1000)) + " " + to_string(g(2)) + " " + to_string(g(70));
			)";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "bitwise typed stores");
			both(src, oracle, "bitwise typed stores, proofs off", false);
			const std::string l = listing_run(src);
			const size_t fb = l.find("== f ");
			const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
			check_true(fl.find("-- cell loop") != std::string::npos && fl.find("C_ARITH_ANY") == std::string::npos, "f's loop runs pinned: " + fl);
		});
		test("a_true_sense_comparison_leaf_keeps_every_operand_kind", [this]() {
			// `a < b || ...` jumps on a compare-jump that falls through to the true edge: ints, floats with NaN, strings, null, mixed kinds and a
			// class operator agree with the interpreter, inside and outside a loop
			const std::string src = R"(
				class V { int n = 0; V(int x) { n = x; } bool operator<(V o) { return n < o.n; } bool operator==(V o) { return n == o.n; } }
				string t(var a, var b) {
					string r = "";
					if (a < b || r == "x") { r += "lt"; } else { r += "ge"; }
					if (a == b || r == "x") { r += "eq"; }
					if (!(a < b) && r != "x") { r += "!lt"; }
					return r;
				}
				int count(array<float> xs, float lim) { int c = 0; for (int i = 0; i < xs.size(); ++i) { if (xs[i] < lim || xs[i] != xs[i]) { c += 1; } } return c; }
				float nan = math::NAN_VALUE;
				t(1, 2) + t(2, 1) + t(1.5, 1) + t(nan, 1.0) + t(1.0, nan) + t("a", "b") + t("b", "a") + t(1, 1.0) + t(V(1), V(2)) + t(V(2), V(1)) + t(V(3), V(3))
				+ " " + to_string(count([1.0, 5.0, nan, 2.0, 9.0], 3.0)) + " " + to_string(count([nan, nan], 0.0));
			)";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "true-sense leaves");
			both(src, oracle, "true-sense leaves, proofs off", false);
		});
		test("pushes_onto_an_environment_array_run_inside_the_block", [this]() {
			// a global array receiver resolves through its lookup at entry and at each push; a call that rebinds the global between pushes lands
			// the next push on the new array, a typed receiver keeps its element rule
			const std::string defs =
				"array<int> Q; var V = []; array<int> R; var HALF = 1.5;\n"
				"void reset() { R = []; }\n"
				"int fill(int n) { Q = []; V = []; for (int i = 0; i < n; ++i) { int t = i * 2; Q.push(t); V.push(i); V.push(Q[i]); } return Q.size() + V.size(); }\n"
				"int rebind(int n) { R = [7]; int s = 0; for (int i = 0; i < n; ++i) { R.push(i); if (i == n / 2) { reset(); } s += R.size(); } return s * 100 + R.size(); }\n"
				"string bad(int n) { Q = []; string out = \"\"; try { for (int i = 0; i < n; ++i) { Q.push(i); if (i == 3) { Q.push(HALF); } } } catch (e) { out = e; } return out + to_string(Q.size()); }\n";
			const std::string src = defs + "to_string(fill(200)) + \" \" + to_string(rebind(40)) + \" \" + to_json(R) + \" \" + bad(10) + \" \" + to_string(Q[3]) + \" \" + to_string(V[7]);";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("throw") == std::string::npos && oracle.find(" Cannot push 'float' to array<int>4 ") != std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "environment receivers");
			both(src, oracle, "environment receivers, proofs off", false);
			const std::string l = listing_run(src);
			const size_t fb = l.find("== fill ");
			const std::string fl = fb == std::string::npos ? l : l.substr(fb, l.find("\n== ", fb + 1) - fb);
			check_true(fl.find("C_PUSH") != std::string::npos && fl.find("-- open loop") == std::string::npos, "fill's pushes run inside the block: " + fl);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("a_loop_inside_a_graft_is_its_callers_statement", [this]() {
			// Gator Quest's building(): fill_rect's two loops graft with set_tile grafted in the inner one; an exit inside the grafted body (set_tile's
			// guarded store, in_world's return) flushes what the caller's whole statement defined ahead of it (the grafts' parameters and locals),
			// so the statements of a graft stay the caller's statement for every loop inside it; both backends, proofs on and off
			const std::string src = R"(
				array<int> W_W = [12, 8];
				array<int> W_H = [10, 8];
				array<int> W_OFF = [0, 120];
				array<int> TILES = [];
				for (int i = 0; i < 184; ++i) { TILES.push(0); }
				bool in_world(int w, int x, int y) { return x >= 0 && y >= 0 && x < W_W[w] && y < W_H[w]; }
				void set_tile(int w, int x, int y, int t) {
					if (in_world(w, x, y)) { TILES[W_OFF[w] + y * W_W[w] + x] = t; }
				}
				void fill_rect(int w, int x0, int y0, int x1, int y1, int t) {
					for (int y = y0; y <= y1; ++y) {
						for (int x = x0; x <= x1; ++x) { set_tile(w, x, y, t); }
					}
				}
				array<int> building(int w, int x0, int y0, int bw, int bh, int wall, int host_floor) {
					fill_rect(w, x0, y0, x0 + bw - 1, y0 + bh - 1, wall);
					fill_rect(w, x0 + 1, y0 + 1, x0 + bw - 2, y0 + bh - 2, host_floor);
					int dx = x0 + bw / 2;
					int dy = y0 + bh - 1;
					set_tile(w, dx, dy, 9);
					set_tile(w, dx, dy + 1, 7);
					return [dx, dy];
				}
				var d1 = building(0, 1, 1, 7, 5, 3, 4);
				var d2 = building(0, 6, 4, 8, 6, 5, 6);
				var d3 = building(1, 2, 2, 5, 4, 3, 4);
				int sum = 0;
				for (int i = 0; i < 184; ++i) { sum = sum * 7 + TILES[i]; sum = sum % 1000003; }
				to_string(sum) + " " + to_string(d1[0]) + "," + to_string(d2[1]) + "," + to_string(d3[0]);
			)";
			both(src, "539827 4,9,4", "a loop inside a graft");
			both(src, "539827 4,9,4", "a loop inside a graft, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(std::string("539827 4,9,4"), got, "a miss at every " + std::to_string(every));
			}
		});
		test("a_miss_before_a_bailed_graft_resumes_its_statement_with_its_values", [this]() {
			// Gator Quest's fill_rect as its own frame: set_tile and in_world graft into the loop and in_world's body bails; a miss between the grafts'
			// parameters and the bail re-runs the statement's rest on the general form, which reads what the statement defined before the miss (master
			// read in_world's w from a slot a later temp had reused: Array index 4992 out of bounds); both backends, every miss rate to 30
			const std::string src = R"(
				array<int> W_W = [40, 8];
				array<int> W_H = [30, 8];
				array<int> W_OFF = [0, 1200];
				array<int> TILES = [];
				for (int i = 0; i < 1264; ++i) { TILES.push(0); }
				bool in_world(int w, int x, int y) { return x >= 0 && y >= 0 && x < W_W[w] && y < W_H[w]; }
				void set_tile(int w, int x, int y, int t) {
					if (in_world(w, x, y)) { TILES[W_OFF[w] + y * W_W[w] + x] = t; }
				}
				void fill_rect(int w, int x0, int y0, int x1, int y1, int t) {
					for (int y = y0; y <= y1; ++y) {
						for (int x = x0; x <= x1; ++x) { set_tile(w, x, y, t); }
					}
				}
				var fill = fill_rect;
				fill(0, 3, 2, 20, 15, 5);
				fill(0, 10, 8, 38, 29, 6);
				fill(1, 1, 1, 6, 6, 7);
				fill(0, -2, -2, 4, 3, 8);
				int sum = 0;
				for (int i = 0; i < 1264; ++i) { sum = (sum * 7 + TILES[i]) % 1000003; }
				to_string(sum);
			)";
			both(src, "337122", "a bailed graft in a loop");
			both(src, "337122", "a bailed graft in a loop, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every = 1; every <= 30; ++every) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(std::string("337122"), got, "a miss at every " + std::to_string(every));
			}
		});
		test("a_miss_inside_a_grafted_loop_resumes_the_graft_with_its_values", [this]() {
			// Gator Quest's gen_overworld: a river walk (tile_at, set_tile and iclamp grafted, DX/DY element reads) and then fill_rect's grafted loops in
			// one function; a miss anywhere in a block inside a graft's body resumes the graft's code, which reads the graft's parameters and locals the
			// block defined (master read a stale x there under a miss every 7th instruction); both backends, proofs on and off, miss injection
			const std::string src = R"(
				array<int> W_W = [40, 8];
				array<int> W_H = [30, 8];
				array<int> W_OFF = [0, 1200];
				array<int> TILES = [];
				array<int> DX = [0, 1, 1, 1, 0, -1, -1, -1];
				array<int> DY = [-1, -1, 0, 1, 1, 1, 0, -1];
				for (int i = 0; i < 1264; ++i) { TILES.push(0); }
				int SEED = 12345;
				int rng_next() { SEED = (SEED * 1103515245 + 12345) % 2147483648; return SEED; }
				int rnd(int n) { return n <= 1 ? 0 : rng_next() % n; }
				int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
				bool in_world(int w, int x, int y) { return x >= 0 && y >= 0 && x < W_W[w] && y < W_H[w]; }
				int tile_at(int w, int x, int y) {
					if (x < 0 || y < 0 || x >= W_W[w] || y >= W_H[w]) { return 99; }
					return TILES[W_OFF[w] + y * W_W[w] + x];
				}
				void set_tile(int w, int x, int y, int t) {
					if (in_world(w, x, y)) { TILES[W_OFF[w] + y * W_W[w] + x] = t; }
				}
				void fill_rect(int w, int x0, int y0, int x1, int y1, int t) {
					for (int y = y0; y <= y1; ++y) {
						for (int x = x0; x <= x1; ++x) { set_tile(w, x, y, t); }
					}
				}
				void gen(int w) {
					int ww = W_W[w];
					int hh = W_H[w];
					for (int y = 0; y < hh; ++y) {
						for (int x = 0; x < ww; ++x) { TILES[W_OFF[w] + y * ww + x] = (x * 7 + y * 3) % 5; }
					}
					for (int r = 0; r < 4; ++r) {
						int x = 5 + rnd(ww - 10);
						int y = 5 + rnd(hh - 10);
						int dir = rnd(8);
						for (int step = 0; step < 40; ++step) {
							int t = tile_at(w, x, y);
							if (t == 9) { break; }
							set_tile(w, x, y, step % 7 == 0 ? 9 : 8);
							if (rnd(3) == 0) { dir = (dir + rnd(3) + 7) % 8; }
							x = iclamp(x + DX[dir], 1, ww - 2);
							y = iclamp(y + DY[dir], 1, hh - 2);
						}
					}
					int sx = ww / 2 - 6;
					int sy = hh / 2 - 5;
					fill_rect(w, sx - 2, sy - 2, sx + 12, sy + 10, 6);
					fill_rect(w, sx, sy, sx + 10, sy + 8, 7);
				}
				gen(0);
				gen(1);
				int sum = 0;
				for (int i = 0; i < 1264; ++i) { sum = (sum * 7 + TILES[i]) % 1000003; }
				to_string(sum);
			)";
			both(src, "523522", "a river walk and grafted fills");
			both(src, "523522", "a river walk and grafted fills, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u, 13u, 17u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(std::string("523522"), got, "a miss at every " + std::to_string(every));
			}
		});
		test("boundary_operations_hand_the_general_form_what_it_changed", [this]() {
			// the statement changes a cell's kind (a var taking a string): the block does not resume and the rest runs general; a statement that raises
			// leaves the block closed for the handler, and the loop runs again afterwards; a boundary inside an inner loop, and one there whose block does
			// not resume (the general form takes the inner loop with its state); a range-for with an alias
			const std::string src =
				"map<int, var> m = {};\n"
				"function kind_change() -> string { var v = 0; int n = 0; for (int i = 0; i < 20; ++i) { v = v + 1; n = n + 1; if (i == 9) { m[0] = 1; v = v * 1.5; } } return to_string(v) + \" \" + to_string(n); }\n"
				"function kind_change_inner() -> string { var v = 1.5; int n = 0; for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) { auto w = v; v = [i]; var k = 7; n = n + k; } } return to_string(v) + \" \" + to_string(n); }\n"
				"function raises(int at) -> int { int s = 0; for (int i = 0; i < 20; ++i) { s = s + i; m[i & 3] = 100 / (i - at); s = s + 1; } return s; }\n"
				"function nested() -> int { int t = 0; for (int j = 0; j < 5; ++j) { int s = 0; for (int i = 0; i < 10; ++i) { s = s + i * j; m[i & 1] = s; } t = t + s; } return t; }\n"
				"class P { int x = 0; }\n"
				"array<P> ps = [P(), P(), P()];\n"
				"function aliases() -> int { int s = 0; for (int r = 0; r < 4; ++r) { for (auto& p : ps) { p.x = p.x + r; m[p.x & 3] = p.x; s = s + p.x; } } return s; }\n"
				"string out = kind_change() + \" \" + kind_change_inner();\n"
				"try { out += \" \" + to_string(raises(7)); } catch (e) { out += \" caught:\" + e; }\n"
				"out += \" \" + to_string(raises(100)) + \" \" + to_string(nested()) + \" \" + to_string(aliases()) + \" \" + to_string(m[1]);\n"
				"out;";
			const std::string oracle = run_on(false, src);
			check_true(oracle.find("caught:") != std::string::npos, "the interpreter's answer: " + oracle);
			both(src, oracle, "what the statement changed");
			both(src, oracle, "what the statement changed, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 4u, 5u, 7u, 11u }) {
				auto em = jai::foundry::make_default_engine(); em->set_backend(backend_type::vm); em->cells_miss_every(every); stdlib::register_all(*em);
				std::string got;
				try { got = em->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		test("raw_frames_write_back_environment_values_on_a_general_return", [this]() {
			// a frame program entered by a general call (its environment write keeps it off the raw entry) returns through its pad: the environment
			// values its body wrote reach their bindings first (order band seed 23 lost two writes of a global this way)
			const std::string src =
				"function rec0(int n) -> int { var keep = [](int a) { return a; }; if (n <= 0) { return 0; } return rec0(n - 1); }\n"
				"float G = 1.0;\n"
				"int H = 10;\n"
				"function w(int p) -> float { rec0(1); G = G - 2.25; return p * 2; }\n"
				"function w2(int p) -> int { rec0(2); H = H - 3; return p + H; }\n"
				"float s = 0.0;\n"
				"for (int i = 0; i < 3; ++i) { s = s + w(1) + w2(i); }\n"
				"to_string(G) + \" \" + to_string(H) + \" \" + to_string(s);";
			both(src, "-5.75 1 21.0", "environment writes before a general return");
			both(src, "-5.75 1 21.0", "environment writes before a general return, proofs off", false);
			const std::string l = listing_run(src);
			const size_t wb = l.find("== w ");
			const std::string wl = wb == std::string::npos ? l : l.substr(wb, l.find("\n== ", wb + 1) - wb);
			check_true(wl.find("-- frame program") != std::string::npos && wl.find("(general entry only)") != std::string::npos, "w is a frame program entered generally: " + wl);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(std::string("-5.75 1 21.0"), got, "a miss at every " + std::to_string(every));
			}
		});
		test("raw_frames_replan_after_general_code_inside_the_chain", [this]() {
			// a call site's plan holds only until general code runs: a host function called from a raw frame redefines the raw callee mid-chain
			// (the next raw probe resolves the new body), and a callee redefined with another parameter kind takes the door from a site whose
			// arguments were proven against the old kind
			auto session = [](bool vm) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e); register_host_math_fixture(*e);
				engine* raw = e.get();
				int flips = 0;
				e->add_function("redefine", [raw, &flips](script_int n) -> script_int {
					if (n == 4 && flips == 0) { ++flips; raw->execute("int step(int n) { if (n < 1) { return 100; } return redefine(n) + step(n - 1) + 2; }"); }
					return 0;
				});
				std::string out;
				try {
					e->execute("int step(int n) { if (n < 1) { return 0; } return redefine(n) + step(n - 1) + 1; }\n"
					           "int drive(int n) { if (n < 1) { return 0; } return walk(n) + drive(n - 1); }\n"
					           "int walk(int n) { if (n < 1) { return 1; } return walk(n - 1) + n; }");
					out += e->execute("to_string(step(8)) + \" \" + to_string(step(3)) + \" \" + to_string(drive(6));").as_string();
					e->execute("int walk(float x) { if (x < 1.0) { return 7; } return walk(x - 1.5) + 1; }");
					out += " " + e->execute("to_string(drive(6));").as_string();
				} catch (const std::exception& ex) { out += std::string(" throw:") + ex.what(); }
				return out;
			};
			const std::string oracle = session(false);
			check_true(oracle.find("throw:") == std::string::npos, "runs: " + oracle);
			check_eq(oracle, session(true), "a callee redefined inside a raw chain and one redefined with another parameter kind");
		});
		// what a caller learns of a callee at its call sites (its result kind, whether it raises, whether it runs user code, whether a reference
		// escapes) holds only for the callee's definition: each reload below changes one of these facts under callers that already ran
		auto reload_session = [](bool vm, const std::vector<std::string>& steps, const std::string& probe) {
			auto e = jai::foundry::make_default_engine();
			e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
			stdlib::register_all(*e); register_host_math_fixture(*e);
			std::string out;
			for (const std::string& step : steps) {
				try {
					e->execute(step);
					out += e->execute(probe).as_string() + "|";
				} catch (const std::exception& ex) { out += std::string("throw:") + ex.what() + "|"; }
			}
			return out;
		};
		test("call_facts_follow_a_reloaded_callee_through_its_callers", [this, reload_session]() {
			const std::vector<std::string> steps = {
				"int leaf(int x) { return x + 1; }\nint mid(int n) { if (n < 1) { return 0; } return leaf(n) + mid(n - 1); }\nint top(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + mid(i); } return s; }",
				"int leaf(int x) { return x * 3; }",
				"float leaf(int x) { return x * 0.5; }",
				"int leaf(int x) { return x - 2; }",
			};
			const std::string probe = "to_string(top(12)) + \" \" + to_string(mid(9));";
			const std::string oracle = reload_session(false, steps, probe);
			check_eq(std::string("352 54|858 135|throw:Cannot return float from 'mid' (declared -> int)|154 27|"), oracle, "a float result reaches mid's int return");
			check_eq(oracle, reload_session(true, steps, probe), "a leaf reloaded under two levels of callers, its result kind changed once");
			std::vector<std::string> widened = steps;
			widened[0] = "int leaf(int x) { return x + 1; }\nfloat mid(int n) { if (n < 1) { return 0; } return leaf(n) + mid(n - 1); }\nfloat top(int n) { float s = 0.0; for (int i = 0; i < n; ++i) { s = s + mid(i); } return s; }";
			const std::string widened_oracle = reload_session(false, widened, probe);
			check_true(widened_oracle.find("throw:") == std::string::npos, "runs: " + widened_oracle);
			check_eq(widened_oracle, reload_session(true, widened, probe), "the result kind changed under callers that widen it");
		});
		test("call_facts_follow_a_callee_that_starts_raising", [this, reload_session]() {
			const std::vector<std::string> steps = {
				"int leaf(int x) { return x + 1; }\nint mid(int n) { if (n < 1) { return 0; } return leaf(n) + mid(n - 1); }\n"
				"string top(int n) { try { return to_string(mid(n)); } catch (e) { return \"caught:\" + e; } }",
				"int leaf(int x) { if (x == 5) { throw \"leaf five\"; } return x + 1; }",
				"int leaf(int x) { return x + 9223372036854775800; }",
				"int leaf(int x) { array<int> a = [1, 2]; return a[x]; }",
				"int leaf(int x) { return x + 1; }",
			};
			const std::string probe = "top(8) + \" \" + top(3);";
			const std::string oracle = reload_session(false, steps, probe);
			check_true(oracle.find("caught:leaf five") != std::string::npos, "the thrown text reaches the caller: " + oracle);
			check_eq(oracle, reload_session(true, steps, probe), "a no-raise callee reloaded to throw, overflow and index out of range, then back");
		});
		test("call_facts_follow_a_callee_that_starts_running_user_code", [this, reload_session]() {
			const std::vector<std::string> steps = {
				"int G = 0;\nclass V { int v = 0; V(int x) { v = x; } V operator+(V o) { G = G + 1; return V(v + o.v); } ~V() { G = G + 100; } }\n"
				"int leaf(int x) { return x + 1; }\nint mid(int n) { if (n < 1) { return 0; } int a = leaf(n); return a + G + mid(n - 1); }",
				"int leaf(int x) { V a = V(x); V b = a + V(1); return b.v; }",
				"var HOOK = [](int x) { G = G + x; return x; };\nint leaf(int x) { return HOOK(x) + 1; }",
				"int leaf(int x) { return x + 1; }",
			};
			const std::string probe = "G = 0; string r = to_string(mid(6)); r + \" G=\" + to_string(G);";
			const std::string oracle = reload_session(false, steps, probe);
			check_true(oracle.find("throw:") == std::string::npos, "runs: " + oracle);
			check_eq(oracle, reload_session(true, steps, probe), "a plain callee reloaded to run an operator, a destructor and a callback that write a global the caller reads");
		});
		test("call_facts_follow_a_reference_that_starts_escaping", [this, reload_session]() {
			const std::vector<std::string> steps = {
				"var KEPT = []() {};\nvoid bump(int& r) { r = r + 1; }\n"
				"int mid(int n) { int s = 0; for (int i = 0; i < n; ++i) { bump(s); } KEPT(); return s; }",
				"void bump(int& r) { r = r + 1; KEPT = [&r]() { r = r + 1000; }; }",
				"array<int> HELD = [];\nvoid bump(int& r) { r = r + 2; HELD.push(r); KEPT = []() {}; }",
				"void bump(int& r) { r = r + 1; KEPT = []() {}; }",
			};
			const std::string probe = "to_string(mid(5)) + \" \" + to_string(mid(2));";
			const std::string oracle = reload_session(false, steps, probe);
			check_true(oracle.find("throw:") == std::string::npos, "runs: " + oracle);
			check_eq(oracle, reload_session(true, steps, probe), "a reference parameter reloaded to escape into a lambda the caller runs, then to be copied out");
		});
		test("raw_frames_inside_coroutines_and_around_yields", [this]() {
			// a coroutine's body runs on its fiber frame (never a frame program); the raw chains it starts end before its yields
			const std::string src =
				"int fib(int n) { if (n < 2) { return n; } return fib(n - 2) + fib(n - 1); }\n"
				"coroutine gen(int n) -> int { for (int i = 0; i < n; ++i) { int v = fib(i); yield v; } }\n"
				"string out = \"\"; var a = gen(8); var b = gen(6);\n"
				"for (int k = 0; k < 8; ++k) { out += to_string(a.next()) + \",\"; if (!b.done()) { out += to_string(b.next()) + \";\"; } }\n"
				"for (int v : gen(5)) { out += to_string(v) + \" \"; }\n"
				"out;";
			both(src, "", "raw chains inside coroutines");
			both(src, "", "raw chains inside coroutines, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
		});
		test("raw_frames_leave_the_caller_consistent_after_a_general_call", [this]() {
			// a raw callee that makes a general call (a var function writing a global the caller's block reads) re-validates its caller at the
			// return; the caller's block, its globals and its container sites stay the general form's
			const std::string src =
				"int G = 1;\n"
				"var bump = [](var x) { G = G + x; return 0; };\n"
				"int leaf(int n) { if (n < 1) { return 0; } int t = bump(n); return leaf(n - 1) + n; }\n"
				"array<int> A = [0, 0, 0, 0];\n"
				"function run() -> string { int s = 0; for (int i = 0; i < 4; ++i) { int r = leaf(i); s = s + r + G; A[i] = G; } return to_string(s) + \" \" + to_string(G) + \" \" + to_string(A); }\n"
				"run();";
			both(src, "", "a general call inside a raw callee");
			both(src, "", "a general call inside a raw callee, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// an enclosing call's callee is resolved before its arguments run: a leaf grafted inside the arguments whose callee was rebound
		// to code that rebinds the enclosing name leaves the enclosing call on the function it named first (a leaf, a stdlib alias, a
		// function no graft takes, three levels, in loops)
		test("an_enclosing_callee_resolves_before_its_arguments", [this]() {
			const std::string src = "[[warn_mode]]\nvar SQRT0 = host_sqrt;\n"
				"function inner(float x) -> float { return x + 1.0; }\n"
				"function outer(float x) -> float { return x * 2.0; }\n"
				"function mid(float x) -> float { return x - 3.0; }\n"
				"function big(float x) -> float { for (int i = 0; i < 3; ++i) { if (i == 2) { return x * 5.0; } } return 0.0; }\n"
				"function evil(float x) -> float { outer = function(float v) { return v * 100.0; }; mid = function(float v) { return v * 1000.0; }; big = function(float v) { return v * 7.0; }; host_sqrt = function(float v) { return v * 10000.0; }; host_hypot = function(float a, float b) { return a * 3.0; }; return x + 1.0; }\n"
				"function leafs(float x) -> float { return host_sqrt(x) + 1.0; }\n"
				"var OUTER0 = outer; var MID0 = mid; var BIG0 = big; var INNER0 = inner; var HYPOT0 = host_hypot;\n"
				"function reset() -> void { outer = OUTER0; mid = MID0; big = BIG0; host_sqrt = SQRT0; host_hypot = HYPOT0; }\n"
				"function r1(float x) -> float { return outer(inner(x)); }\n"
				"function r2(float x) -> float { float r = outer(inner(x)) + 0.5; return r; }\n"
				"function r3(float x) -> float { float t = 0.0; for (int i = 0; i < 3; ++i) { t += outer(inner(x)); } return t; }\n"
				"function r4(float x) -> float { float r = host_sqrt(inner(x)) + 0.0; return r; }\n"
				"function r5(float x) -> float { float r = big(inner(x)) + 0.0; return r; }\n"
				"function r6(float x) -> float { return outer(mid(inner(x))); }\n"
				"function r7(float x) -> float { float r = 1.0 + outer(leafs(x)); return r; }\n"
				"function r8(float x) -> float { float t = 0.0; for (int i = 0; i < 3; ++i) { float r = host_sqrt(inner(x)) + 0.0; t += r; } return t; }\n"
				"function r9(float x) -> float { float r = host_hypot(inner(x), 0.0) + 0.0; return r; }\n"
				"function all() -> string { string s = \"\";\n"
				"  reset(); s += to_string(r1(1.0)) + \" \"; s += to_string(r1(1.0)) + \" \";\n"
				"  reset(); s += to_string(r2(1.0)) + \" \"; s += to_string(r2(1.0)) + \" \";\n"
				"  reset(); s += to_string(r3(1.0)) + \" \";\n"
				"  reset(); s += to_string(r4(3.0)) + \" \"; s += to_string(r4(3.0)) + \" \";\n"
				"  reset(); s += to_string(r5(1.0)) + \" \"; s += to_string(r5(1.0)) + \" \";\n"
				"  reset(); s += to_string(r6(1.0)) + \" \"; s += to_string(r6(1.0)) + \" \";\n"
				"  reset(); s += to_string(r8(3.0)) + \" \";\n"
				"  reset(); s += to_string(r9(1.0)) + \" \"; s += to_string(r9(1.0)) + \" \";\n"
				"  return s; }\n"
				"string out = all(); inner = evil; out += \"| \" + all(); inner = INNER0; reset();\n"
				"out += \"| \" + to_string(r7(4.0));\n"
				"host_sqrt = function(float v) { outer = function(float w) { return w * 100.0; }; return v; };\n"
				"out += \" \" + to_string(r7(4.0)) + \" \" + to_string(r7(4.0)); out;";
			const std::string expected = "4.0 4.0 4.5 4.5 12.0 2.0 2.0 10.0 10.0 -2.0 -2.0 6.0 2.0 2.0 | 4.0 200.0 4.5 200.5 404.0 2.0 40000.0 10.0 14.0 -2.0 2e+05 80002.0 2.0 6.0 | 7.0 11.0 501.0";
			both(src, expected, "enclosing callees");
			both(src, expected, "enclosing callees, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(expected, got, "a miss at every " + std::to_string(every));
			}
		});
		// a reference parameter bound to an element is an alias of that element in the graft, as the call binds it: stores through it reach the
		// array (a float array, an int array's element conversion, a row of an array of arrays), in a loop and outside one, reads after the store
		test("a_reference_parameter_bound_to_an_element_aliases_it", [this]() {
			const std::string src = "[[warn_mode]]\n"
				"function setv(var& a) -> void { a = 2.5; }\n"
				"function bumpv(var& a, float d) -> float { a = a + d; return a * 2.0; }\n"
				"function run1() -> string { array<float> A = [1.0, 2.0]; setv(A[0]); return to_json(A); }\n"
				"function run2() -> string { array<int> B = [1, 2]; try { setv(B[0]); } catch (err) { return err + to_json(B); } return to_json(B); }\n"
				"function run3() -> string { var M = [[1, 2], [3, 4]]; for (int i = 0; i < 2; ++i) { setv(M[i]); } return to_json(M); }\n"
				"function run4(int n) -> string { array<float> A = [0.0, 0.0, 0.0, 0.0]; float t = 0.0; for (int i = 0; i < n; ++i) { t += bumpv(A[i & 3], i * 0.5); } return to_json(A) + \" \" + to_string(t); }\n"
				"function run5() -> string { var V = [1, \"x\", 2.5]; setv(V[1]); float r = bumpv(V[2], 1.0); return to_json(V) + \" \" + to_string(r); }\n"
				"run1() + \" \" + run2() + \" \" + run3() + \" \" + run4(10) + \" \" + run5();";
			const std::string expected = "[2.5,2.0] Cannot assign 'float' to element of type 'int'[1,2] [2.5,2.5] [6.0,7.5,4.0,5.0] 61.0 [1,2.5,3.5] 7.0";
			both(src, expected, "element references");
			both(src, expected, "element references, proofs off", false);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(expected, got, "a miss at every " + std::to_string(every));
			}
		});
		// a reference parameter bound to a name is that variable in the graft only where a store through the reference and a store of the
		// variable agree: an untyped variable, or one declared the parameter's scalar type with every store proven to land it; a float stored
		// through a reference to an int variable raises the reference's text and leaves the variable, grafted or not
		test("a_reference_parameter_bound_to_a_name_stores_as_the_reference", [this]() {
			const std::string src = "[[warn_mode]]\n"
				"function setv(var& a) -> void { a = 2.5; }\n"
				"function setf(float& a) -> void { a = 7; }\n"
				"function seti(int& a) -> void { a = 2.75; }\n"
				"function addi(int& a, float d) -> void { a += d; }\n"
				"function bumpf(float& a, float d) -> void { a = a + d; }\n"
				"function incf(float& a) -> void { ++a; }\n"
				"function run1() -> string { int x = 1; try { setv(x); } catch (err) { return err + \":\" + to_string(x); } return to_string(x) + \":\" + type_of(x); }\n"
				"function run2() -> string { float y = 1.0; setf(y); return to_string(y) + \":\" + type_of(y); }\n"
				"function run3() -> string { int z = 1; try { seti(z); } catch (err) { return err + \":\" + to_string(z); } return to_string(z) + \":\" + type_of(z); }\n"
				"function run4() -> string { int w = 1; try { addi(w, 0.5); } catch (err) { return err + \":\" + to_string(w); } return to_string(w) + \":\" + type_of(w); }\n"
				"function run5() -> string { var v = 1; setv(v); return to_string(v) + \":\" + type_of(v); }\n"
				"function run6(int n) -> string { float t = 0.0; for (int i = 0; i < n; ++i) { bumpf(t, i * 0.5); incf(t); } return to_string(t) + \":\" + type_of(t); }\n"
				"function run7() -> string { float q = 3.0; bumpf(q, 0.25); return to_string(q) + \":\" + type_of(q); }\n"
				"function setb(var& a) -> void { a = true; }\n"
				"function run8() -> string { int b = 5; setb(b); return to_string(b) + \":\" + type_of(b); }\n"
				"run1() + \"|\" + run2() + \"|\" + run3() + \"|\" + run4() + \"|\" + run5() + \"|\" + run6(10) + \"|\" + run7() + \"|\" + run8();";
			// a store through a reference runs the variable's own table and a T& binds only a T
			const std::string expected = "Cannot assign float to 'a' (declared int&):1|7.0:float|Cannot assign float to 'a' (declared int&):1|Cannot assign float to a variable declared int:1|2.5:float|32.5:float|3.25:float|1:int";
			both(src, expected, "name references");
			both(src, expected, "name references, proofs off", false);
			const std::string l = listing_run(src);
			const size_t fbeg = l.find("== run6 ");
			const std::string fl = fbeg == std::string::npos ? l : l.substr(fbeg, l.find("\n== ", fbeg + 1) - fbeg);
			check_true(fl.find("-- inlined bumpf") != std::string::npos && fl.find("-- inlined incf") != std::string::npos && fl.find("-- cell loop") != std::string::npos, "a proven float reference still grafts and the loop closes: " + fl);
		});
		// a bare stdlib alias call is float wherever its name binds the builtin: the arithmetic around it takes the typed rows (the proof the
		// rows guard), so the block runs its pinned forms; a rebinding to a function returning an int or a string keeps the general values
		// and texts
		test("a_stdlib_alias_operand_is_a_float_operand", [this]() {
			const std::string src = "[[warn_mode]]\nvar SIN0 = host_sin;\nfloat W = 0.5; float T = 0.25;\n"
				"function f(float q) -> float { float wx = q + W * host_sin(q * 1.3 + T * 0.7); return wx; }\n"
				"function run(int n) -> float { float s = 0.0; for (int i = 0; i < n; ++i) { float q = i * 0.1; float wx = q + W * host_sin(q * 1.3 + T * 0.7); s += wx + f(q); } return s; }\n"
				"function rb(int i) -> int { if (i == 5) { host_sin = function(float v) { return 3; }; } return i; }\n"
				"function run2(int n) -> float { float s = 0.0; for (int i = 0; i < n; ++i) { float q = i * 0.1; s += q + W * host_sin(q); int k = rb(i); s += k; } return s; }\n"
				"float probe_f = f(0.1);\n"   // a program-scope call compiles f itself (run's call grafts it)
				"string out = to_string(run(40)) + \" \" + to_string(run2(10));\n"
				"host_sin = function(float v) { return \"x\"; };\n"
				"function bad() -> string { try { float q = 1.0; float r = q + W * host_sin(q); return to_string(r); } catch (e) { return \"E:\" + e; } }\n"
				"function bad2() -> string { try { float z = host_sin(1.0); return to_string(z); } catch (e) { return \"E:\" + e; } }\n"
				"function bad3() -> string { try { int k = host_sin(1.0); return to_string(k); } catch (e) { return \"E:\" + e; } }\n"
				"out += \" \" + bad() + \" \" + bad2() + \" \" + bad3(); host_sin = SIN0; out + \" \" + to_string(run(3));";
			both(src, "", "alias operands");
			both(src, "", "alias operands, proofs off", false);
			const std::string l = listing_run(src);
			const size_t fbeg = l.find("== f ");
			const std::string fl = fbeg == std::string::npos ? l : l.substr(fbeg, l.find("\n== ", fbeg + 1) - fbeg);
			check_true(fl.find("BINARY_FUSED") == std::string::npos && fl.find("T_MUL_FF") != std::string::npos, "the arithmetic around the alias takes typed rows: " + fl);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a push of a computed value onto a private array local the checker proved: the value (a leaf's graft, a call, arithmetic, a value
		// binary ahead of a call) lands in a compiler temp and the fused push takes it, so the loop closes; the push's element conversion is
		// its own (an int widens into a float array, a float into an int array runs the checked kernel), and a receiver user code can reach
		// (a global, a local the value names) keeps the general order
		test("a_push_of_a_computed_value_lands_in_a_temp", [this]() {
			const std::string src = "[[warn_mode]]\nint K = 7;\narray<int> G = [];\n"
				"function leafv(int i) -> int { return (i * K + 3) & 1023; }\n"
				"function heavy(int i) -> int { int s = 0; for (int k = 0; k < 2; ++k) { if (s < 0) { return -1; } s += i + k; } return s; }\n"
				"function grows(int i) -> int { G.push(-i); return i; }\n"
				"function half(int i) -> float { return i * 0.5; }\n"
				"function run(int n) -> string { array<int> a = []; array<int> b = []; array<int> c = []; array<int> d = []; array<float> e = [];\n"
				"  for (int i = 0; i < n; ++i) { a.push(leafv(i)); b.push(i * 3 + K); c.push(heavy(i)); d.push(K * (i + 1) + heavy(i)); e.push(leafv(i)); }\n"
				"  return to_json(a) + to_json(b) + to_json(c) + to_json(d) + to_json(e); }\n"
				"function glob(int n) -> string { G = []; for (int i = 0; i < n; ++i) { G.push(grows(i)); } return to_json(G); }\n"
				"function self(int n) -> string { array<int> s = [1]; for (int i = 0; i < n; ++i) { s.push(s.size() * 2); } return to_json(s); }\n"
				"function conv(int n) -> string { try { array<int> t = []; for (int i = 0; i < n; ++i) { t.push(half(i)); } return to_json(t); } catch (e) { return \"E:\" + e; } }\n"
				"run(6) + \" \" + glob(3) + \" \" + self(3) + \" \" + conv(3);";
			both(src, "", "computed pushes");
			both(src, "", "computed pushes, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			const size_t fbeg = l.find("== run ");
			const std::string fl = fbeg == std::string::npos ? l : l.substr(fbeg, l.find("\n== ", fbeg + 1) - fbeg);
			check_true(fl.find("-- cell loop") != std::string::npos && fl.find("open loop") == std::string::npos && fl.find("PROBE_MEMBER") == std::string::npos, "the loop of computed pushes closes: " + fl);
			// a string binary ahead of a call keeps the stack form, which moves the left value into the result (a temp would copy it)
			const std::string str = listing_run("function look(string a, string b, int r) -> int { string key = a + b + to_string(r + 1); return key.size(); }\nlook(\"TROO\", \"A\", 3);");
			const size_t sbeg = str.find("== look ");
			const std::string sl = sbeg == std::string::npos ? str : str.substr(sbeg, str.find("\n== ", sbeg + 1) - sbeg);
			check_true(sl.find("BINARY_FUSED_TEMP") == std::string::npos && sl.find("CALL_TEMP") == std::string::npos, "the string concatenation stays on the stack form: " + sl);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a graft's clone judges a nested leaf in the frame its arguments are read in (the callee's declared slots, whichever clone reads the
		// call), and a graft never compiles a body its own clone refused: the plan's probe read the caller's slot at the callee's slot number,
		// the compile's clone read slots not yet declared, and the graft emitted a truncated body whose result kept a stale value
		test("a_nested_leaf_is_judged_in_the_frame_its_arguments_are_read_in", [this]() {
			const std::string src = "function inner(var y) -> int { return y * 3 + 1; }\n"
				"function outer(int x) -> int { return inner(x) + 1; }\n"
				"function outer2(int x) -> int { int t = x * 2; return inner(t) + inner(x); }\n"
				"function sq(int a) -> int { return a * a; }\n"
				"function run(int n) -> int { int o = 0; int s = 0; for (int i = 1; i <= n; ++i) { s += sq(i); o += outer(i); } return o * 1000 + s; }\n"
				"function run2(string tag, int n) -> int { int o = 0; for (int i = 1; i <= n; ++i) { o += outer(i) + outer2(i); } return o; }\n"
				"function run3(int n) -> int { var v = \"s\"; int o = 0; for (int i = 1; i <= n; ++i) { o += outer2(i); } return o; }\n"
				"to_string(run(6)) + \" \" + to_string(run2(\"x\", 6)) + \" \" + to_string(run3(4));";
			both(src, "75091 276 98", "nested leaves");
			both(src, "75091 276 98", "nested leaves, proofs off", false);
			const std::string l = listing_run(src);
			auto chunk = [&l](const char* name) { const size_t b = l.find(std::string("== ") + name + " "); return b == std::string::npos ? l : l.substr(b, l.find("\n== ", b + 1) - b); };
			const std::string r2 = chunk("run2"), r3 = chunk("run3");
			check_true(r2.find("-- inlined outer") != std::string::npos && r2.find("-- inlined inner") != std::string::npos && r2.find("-- cell loop") != std::string::npos, "the nested leaf grafts under a caller whose slot 0 is no scalar: " + r2);
			check_true(r3.find("-- inlined outer2") != std::string::npos && r3.find("-- inlined inner") != std::string::npos && r3.find("-- cell loop") != std::string::npos, "the nested leaves over the callee's own local graft: " + r3);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(std::string("75091 276 98"), got, "a miss at every " + std::to_string(every));
			}
		});
		// an untyped leaf grafts with the kinds its site gives: a `var` parameter takes its argument's kind (a literal, a local declared a
		// scalar, arithmetic over those), an untyped local the kind every definition of it yields, and the operations over them take those
		// classes as stamps every consumer guards (a local holding another kind at run time, here after a store through a reference, takes the
		// general form); an untyped result lands when every return yields one scalar kind at the site, else the call stays
		test("an_untyped_leaf_grafts_with_the_kinds_its_site_gives", [this]() {
			const std::string src = "[[warn_mode]]\n"
				"function A(var i, var j) { var ij = i + j - 1; return 1.0 / (ij * (ij - 1) * 0.5 + i); }\n"
				"function sq(var a) { return a * a; }\n"
				"function mixed(var a) { if (a > 2) { return a * 0.5; } return a; }\n"
				"function drift(var a) { var s = 0; s = s + a * 0.5; return s; }\n"
				"function inner(var x) { return x * 3 + 1; }\n"
				"function outer(int x) -> int { return inner(x) + inner(x + 1); }\n"
				"function idiv(var a, var b) { return a / b; }\n"
				"function setf(var& r) -> void { r = true; }\n"
				"function run(int n) -> string {\n"
				"  array<float> x = [1.0, 2.0, 3.0, 4.0];\n"
				"  float a = 0.0; int s = 0; float m = 0.0; float d = 0.0; int o = 0;\n"
				"  for (int i = 1; i <= n; ++i) {\n"
				"    for (int j = 1; j <= n; ++j) { a = a + x[(j - 1) & 3] * A(i, j); }\n"
				"    s += sq(i); m += mixed(i); d += drift(i); o += outer(i);\n"
				"  }\n"
				"  return to_string(a) + \" \" + to_string(s) + \" \" + to_string(m) + \" \" + to_string(d) + \" \" + to_string(o);\n"
				"}\n"
				"function flip(int n) -> string {\n"
				"  float t = 0.0; int k = 3;\n"
				"  for (int i = 0; i < n; ++i) { if (i == 2) { setf(k); } t = t + sq(k) + A(k, i + 1); }\n"
				"  return to_string(t) + \":\" + type_of(k);\n"
				"}\n"
				"function big(int n) -> string { try { int r = 0; for (int i = 0; i < n; ++i) { r += sq(3037000499 + i); } return to_string(r); } catch (e) { return \"E:\" + e; } }\n"
				"function zero(int n) -> string { try { int r = 0; for (int i = 0; i < n; ++i) { r += idiv(6, 1 - i); } return to_string(r); } catch (e) { return \"E:\" + e; } }\n"
				"string out = run(6) + \" | \" + flip(5) + \" | \" + big(3) + \" | \" + zero(3);\n"
				"A = function(var i, var j) { return 7; };\n"
				"out + \" | \" + run(2);";
			// flip's setf(k) stores true into the int k through the reference: k's own table keeps it an int
			const std::string want = "7.5400642840316 91 12.0 10.5 156 | 21.761544011544007:int | E:Integer overflow in '*' | E:Division by zero | 42.0 5 3.0 1.5 28";
			both(src, want, "untyped leaves");
			both(src, want, "untyped leaves, proofs off", false);
			const std::string l = listing_run(src);
			auto chunk = [&l](const char* name) { const size_t b = l.find(std::string("== ") + name + " "); return b == std::string::npos ? l : l.substr(b, l.find("\n== ", b + 1) - b); };
			const std::string rl = chunk("run"), fl = chunk("flip");
			check_true(rl.find("-- inlined A") != std::string::npos && rl.find("-- inlined sq") != std::string::npos && rl.find("-- inlined inner") != std::string::npos, "the untyped leaves graft: " + rl);
			check_true(rl.find("-- call kept mixed") != std::string::npos && rl.find("-- call kept drift") != std::string::npos, "a result of two kinds keeps the call: " + rl);
			check_true(fl.find("-- inlined A") != std::string::npos && fl.find("-- inlined sq") != std::string::npos && fl.find("-- cell loop") != std::string::npos, "an argument of arithmetic grafts and the loop closes: " + fl);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(want, got, "a miss at every " + std::to_string(every));
			}
		});
		// a `var` local's kind is the kind every definition of it yields (its initializer, each store, each increment), to a fixpoint over the
		// callable: an untyped leaf over such locals grafts with those kinds, and an element read of no proven kind ahead of the graft reads
		// into a temp where the statement reads it, so a fully dynamic kernel's loop closes; a local stored another kind, one a call binds by
		// reference, or a body the walk does not open keeps the call
		test("a_var_local_takes_the_kind_its_definitions_give", [this]() {
			const std::string src = "[[warn_mode]]\n"
				"function A(var i, var j) { var ij = i + j - 1; return 1.0 / (ij * (ij - 1) * 0.5 + i); }\n"
				"function bump(var& v, var i) { v[0] = v[0] + 1; return i * 2; }\n"
				"function setv(var& r) -> void { r = \"s\"; }\n"
				"function Av(var& x, var& y, var n) { for (var i = 1; i <= n; ++i) { var a = 0.0; for (var j = 1; j <= n; ++j) { a = a + x[j - 1] * A(i, j); } y[i - 1] = a; } }\n"
				"function run(var n) { var u = []; var v = []; for (var i = 0; i < n; ++i) { u.push(1.0 + i); v.push(0.0); } Av(u, v, n); return to_json(v); }\n"
				"function seen(var n) { var x = [1, 2, 3]; var s = 0; for (var i = 0; i < n; ++i) { s = s + x[0] * bump(x, i); } return to_string(s) + to_json(x); }\n"
				"function rebound(var n) { var k = 1; var s = 0.0; for (var i = 0; i < n; ++i) { if (i == 2) { k = 2.5; } s = s + A(k, i + 1); } return to_string(s); }\n"
				"function byref(var n) { var k = 1; var s = \"\"; for (var i = 0; i < n; ++i) { if (i == 2) { setv(k); } s = s + to_string(k); } return s; }\n"
				"function mixed(var n) { var x = [1, 2.5, \"q\"]; var s = \"\"; for (var i = 0; i < 3; ++i) { s = s + to_string(x[i]) + to_string(A(i + 1, n)); } return s; }\n"
				"function touch(var v) { return 0; }\n"
				"var poke = function(var& r) { r = \"s\"; return 0; };\n"
				"function unresolved(var n) { var k = 1; var s = 0.0; for (var i = 0; i < n; ++i) { if (i == 2) { poke(k); } s = s + to_string(k).size(); } return to_string(s) + type_of(k); }\n"
				"function trusted(var n) { try { var k = 1; var s = 0.0; for (var i = 0; i < n; ++i) { s = s + touch(k) + A(k, i + 1); } return to_string(s) + type_of(k); } catch (e) { return \"E:\" + e; } }\n"
				"string out = run(5) + \" \" + seen(3) + \" \" + rebound(4) + \" \" + byref(4) + \" \" + mixed(2) + \" \" + unresolved(4) + \" \" + trusted(3);\n"
				"touch = function(var& r) { r = \"s\"; return 0; };\n"
				"out + \" \" + trusted(3);";
			const std::string want = "[3.7759740259740258,1.7357843137254902,1.0502136752136753,0.7170018796992481,0.5252146227755984] 16[4,2,3] 1.6636124329249773 11ss 10.52.50.2q0.1111111111111111 "
				"4.0string 1.75int E:cannot apply '+' to 'string' and 'int': stringify it with to_string(x) or `${x}`";
			both(src, want, "var local kinds");
			both(src, want, "var local kinds, proofs off", false);
			const std::string l = listing_run(src);
			auto chunk = [&l](const char* name) { const size_t b = l.find(std::string("== ") + name + " "); return b == std::string::npos ? l : l.substr(b, l.find("\n== ", b + 1) - b); };
			const std::string av = chunk("Av"), rb = chunk("rebound");
			check_true(av.find("-- inlined A") != std::string::npos && av.find("-- cell loop") != std::string::npos && av.find("open loop") == std::string::npos, "the dynamic kernel grafts A and its loops close: " + av);
			check_true(rb.find("-- call kept A") != std::string::npos, "a local stored two kinds keeps the call: " + rb);
			if (!engine::miss_injection_available()) { return; }
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u, 11u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(want, got, "a miss at every " + std::to_string(every));
			}
		});
		// a reference parameter bound to an element grafts only where its bind cannot fail (an element of a private local declared an array);
		// any other element keeps the call, so a failing bind raises the call's text in the call's place, after every argument: a string's
		// character, a missing map key, a var holding a string, and a computed property through a typed reference. A leaf grafted inside
		// the arguments of a call that may bind by reference leaves the call its arguments: an element before it is not spilled as a copy
		// (the stores reach the array) and its own result is not a temp the reference binds (the non-lvalue raises)
		test("a_reference_bind_that_may_fail_keeps_the_call", [this]() {
			const std::string bad = "[[warn_mode]]\nint G = 0;\n"
				"class Acc { float hidden = 1.0; float _get_x() { return hidden * 2.0; } void _set_x(float v) { hidden = v; } }\n"
				"function setv(var& a, float d) -> void { a = d; }\n"
				"function bumpf(float& a, float d) -> void { a = a + d; }\n"
				"function mark(int i) -> float { G = G + 1; return 0.5; }\n"
				"function r1() -> string { try { string s = \"ab\"; for (int i = 0; i < 2; ++i) { setv(s[i], mark(i)); } return s; } catch (e) { return \"E1:\" + e; } }\n"
				"function r2() -> string { try { map<string, int> m = {\"k\": 1}; for (int i = 0; i < 2; ++i) { setv(m[\"z\"], mark(i)); } return to_json(m); } catch (e) { return \"E2:\" + e; } }\n"
				"function r3() -> string { try { Acc q = Acc(); for (int i = 0; i < 2; ++i) { bumpf(q.x, mark(i)); } return to_string(q.hidden); } catch (e) { return \"E3:\" + e; } }\n"
				"function r4() -> string { try { var V = \"xy\"; for (int i = 0; i < 2; ++i) { setv(V[i], mark(i)); } return to_json(V); } catch (e) { return \"E4:\" + e; } }\n"
				"r1() + \" | \" + r2() + \" | \" + r3() + \" | \" + r4() + \" | \" + to_string(G);";
			const std::string bad_want = "E1:Cannot pass non-lvalue to reference parameter | E2:Key \"z\" not found in map | "
				"E3:Cannot pass non-lvalue to reference parameter | E4:Cannot pass non-lvalue to reference parameter | 3";
			both(bad, bad_want, "binds that fail");
			both(bad, bad_want, "binds that fail, proofs off", false);
			const std::string args = "[[warn_mode]]\n"
				"function setw(var& a, float d) -> void { for (int q = 0; q < 1; ++q) { if (q > 5) { return; } } a = d; }\n"
				"function setr(var& a) -> void { for (int q = 0; q < 1; ++q) { if (q > 5) { return; } } a = 9; }\n"
				"function pure(int i) -> float { return 0.5 + i; }\n"
				"function run(int n) -> string { array<float> A = [0.0, 0.0]; var V = [0, 0]; for (int i = 0; i < n; ++i) { setw(A[i & 1], pure(i)); setw(V[i & 1], pure(i)); } return to_json(A) + to_json(V); }\n"
				"function run2(int n) -> string { try { for (int i = 0; i < n; ++i) { setr(pure(i)); } return \"no error\"; } catch (e) { return \"E:\" + e; } }\n"
				"run(3) + \" \" + run2(2);";
			const std::string args_want = "[2.5,1.5][2.5,1.5] E:Cannot pass non-lvalue to reference parameter";
			both(args, args_want, "leaves in the arguments of a reference callee");
			both(args, args_want, "leaves in the arguments of a reference callee, proofs off", false);
		});
		// an intrinsic over a name ahead of a graft reads the name before the graft writes it: only an alias guard or a callee probe the plan
		// pinned spills as a call, any other call among the earlier operands keeps the statement general (order band seed 13:
		// math::abs(G1) * w7(x) read G1 after w7 wrote it)
		test("an_intrinsic_ahead_of_a_writing_graft_reads_first", [this]() {
			const std::string src = "[[warn_mode]]\nint G1 = 143;\nfloat G2 = 2.0;\n"
				"function w7(int p) -> float { G1 = G1 + p; return (G1 > 2) ? 4 : 3; }\n"
				"function wf(float p) -> float { G2 = G2 + p; return 1.5; }\n"
				"function run(int n) -> string { var acc = 0; float f = 0.0; for (int i = 0; i < n; ++i) { var r = math::abs(G1) * w7(7); acc = acc + r; f = f + math::sqrt(G2) * wf(2.0); } return to_string(acc) + \" \" + to_string(f) + \" \" + to_string(G1); }\n"
				"run(3);";
			both(src, "1800.0 8.79555495773441 164", "intrinsics ahead of a writing graft");
			both(src, "1800.0 8.79555495773441 164", "intrinsics ahead of a writing graft, proofs off", false);
		});
		// a miss inside a block's inner loop resumes with what the general form reads next: an inner counted loop's compare back to its body
		// resumes at the generic path's loop back (the var already stepped, never stepped twice), and a compiler value live where the inner
		// loop's body begins (a graft's parameter over the graft's own loop) is flushed anywhere in that loop, since its back edge reads it again
		test("a_miss_inside_an_inner_loop_resumes_where_the_general_form_reads_next", [this]() {
			const std::string src = "float K = 0.5;\n"
				"function nested(int n) -> int { int t = 0; for (int i = 0; i < n; ++i) { for (int j = 0; j < 4; ++j) { t += i * 10 + j; } } return t; }\n"
				"function nestedf(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { float s = 0.0; float x = i * 0.1; for (int j = 0; j < 3; ++j) { float y = x + j; float a = y * K + 1.0 - 2.0; a = a < 0.0 ? -a : a; s += a; } t += s; } return t; }\n"
				"function owns(float x, int k) -> float { float s = 0.0; for (int j = 0; j < k; ++j) { float y = x + j; float a = y * K + 1.0 - 2.0; a = a < 0.0 ? -a : a; s += a; } return s; }\n"
				"function grafted(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { t += owns(i * 0.1, 3); } return t; }\n"
				"to_string(nested(20)) + \" \" + to_string(nestedf(20)) + \" \" + to_string(grafted(20));";
			both(src, "", "inner loops");
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every = 1; every <= 40; ++every) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// leaves graft through every level the tree's node budget allows (four levels here: a loop over l1 whose body calls l2, which calls l3
		// twice, which calls l4), each behind its own guard; a callee rebound at any level mid-loop takes that graft's fallback; mutual recursion
		// stays calls; a tree past the budget keeps the calls that would exceed it, with the same values
		test("leaves_graft_through_every_level_within_the_node_budget", [this]() {
			const std::string src = "[[warn_mode]]\nfloat K = 0.5;\n"
				"function l4(float x) -> float { return x * K + 1.0; }\n"
				"function l3(float x, float y) -> float { float d = l4(x) - y; return d < 0.0 ? -d : d; }\n"
				"function l2(float x) -> float { float a = l3(x, 2.0); float b = l3(x * 0.5, 1.0); return a < b ? a : b; }\n"
				"function l1(float x, int k) -> float { float s = 0.0; for (int j = 0; j < k; ++j) { s += l2(x + j); } return s; }\n"
				"function run(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { t += l1(i * 0.1, 3); } return t; }\n"
				"function rb(int i) -> int { if (i == 7) { l4 = function(float v) { return v * 100.0; }; } if (i == 11) { l3 = function(float a, float b) { return a + b; }; } return i; }\n"
				"function run2(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { float x = i * 0.25; t += l2(x); float k = rb(i); t += k; } return t; }\n"
				"function ev(int n) -> int { if (n <= 0) { return 0; } return od(n - 1) + 1; }\n"
				"function od(int n) -> int { if (n <= 0) { return 1; } return ev(n - 1) + 2; }\n"
				"function run3(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s += ev(i & 7); } return s; }\n"
				"var L40 = l4; var L30 = l3;\n"
				"string out = to_string(run(20)) + \" \" + to_string(run3(30)) + \" \" + to_string(run2(16));\n"
				"l4 = L40; l3 = L30; out + \" \" + to_string(run(3));";
			both(src, "", "four levels");
			both(src, "", "four levels, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			const size_t fbeg = l.find("== run ");
			const std::string fl = fbeg == std::string::npos ? l : l.substr(fbeg, l.find("\n== ", fbeg + 1) - fbeg);
			check_true(fl.find("-- inlined l1") != std::string::npos && fl.find("-- inlined l4") != std::string::npos && fl.find("open loop") == std::string::npos, "every level grafts and the loops close: " + fl);
			// a tree past the budget: g's body is large and f calls it many times; the grafts stop where the budget would be exceeded
			std::string big = "float W = 1.25;\nfunction g(float x) -> float { float a = x * W";
			for (int i = 0; i < 60; ++i) { big += " + x * " + std::to_string(i + 1) + ".0"; }
			big += "; return a; }\nfunction f(float x) -> float { float s = g(x);";
			for (int i = 1; i < 24; ++i) { big += " s += g(x + " + std::to_string(i) + ".0);"; }
			big += " return s; }\nfunction run(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { float v = f(i * 0.5); t += v; } return t; }\nto_string(run(10));";
			both(big, "", "a tree past the budget");
			const std::string bl = listing_run(big);
			check_true(bl.find("a graft tree too large") != std::string::npos, "the budget keeps some calls: " + bl.substr(0, 2000));
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
		// a leaf whose body evaluates a stdlib alias, as another leaf's argument: the outer leaf's guard stands in its callee's place, so the
		// inner leaf grafts inside the outer's parameter and the loop closes; the alias rebound by a call in the loop, the outer rebound by
		// the rebound alias, and a function no graft takes as the enclosing call keep the general form's values
		test("a_leaf_with_an_alias_grafts_as_another_leafs_argument", [this]() {
			const std::string src = "[[warn_mode]]\nvar SQRT0 = host_sqrt; float K = 0.5;\n"
				"function len2(float x, float y) -> float { return host_sqrt(x * x + y * y); }\n"
				"function shade(float d) -> float { return d * K + 1.0; }\n"
				"function rb(int i) -> int { if (i == 5) { host_sqrt = function(float v) { shade = function(float w) { return w * 100.0; }; return -v; }; } return i; }\n"
				"function run(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { float x = i * 0.25; t += shade(len2(x, 1.0)); } return t; }\n"
				"function run2(int n) -> float { float t = 0.0; for (int i = 0; i < n; ++i) { float x = i * 0.25; t += shade(len2(x, 1.0)); int k = rb(i); t += k; } return t; }\n"
				"var SHADE0 = shade;\n"
				"string out = to_string(run(40)) + \",\" + to_string(run2(12)) + \",\" + to_string(run2(12));\n"
				"host_sqrt = SQRT0; shade = SHADE0; out + \",\" + to_string(run(3));";
			both(src, "", "a leaf with an alias as an argument");
			both(src, "", "a leaf with an alias as an argument, proofs off", false);
			check_true(run_on(true, src).find("throw:") == std::string::npos, "runs: " + run_on(true, src));
			const std::string l = listing_run(src);
			const size_t fbeg = l.find("== run ");
			const std::string fl = fbeg == std::string::npos ? l : l.substr(fbeg, l.find("\n== ", fbeg + 1) - fbeg);
			check_true(fl.find("-- inlined shade") != std::string::npos && fl.find("-- inlined len2") != std::string::npos && fl.find("-- cell loop") != std::string::npos && fl.find("open loop") == std::string::npos,
				"both leaves graft and the loop closes: " + fl);
			if (!engine::miss_injection_available()) { return; }
			const std::string oracle = run_on(false, src);
			for (uint32_t every : { 1u, 2u, 3u, 5u, 7u }) {
				auto e = jai::foundry::make_default_engine(); e->set_backend(backend_type::vm); e->cells_miss_every(every); stdlib::register_all(*e); register_host_math_fixture(*e);
				std::string got;
				try { got = e->execute(src).as_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(oracle, got, "a miss at every " + std::to_string(every));
			}
		});
	}
};

}

using pilot_cells_tests = jai::foundry::tests::pilot_cells_tests;
FOUNDRY_REGISTER(pilot_cells_tests)
