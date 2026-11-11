#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/vm/vm_compiler.hpp>
#include <jaiscript/vm/disassembler.hpp>
#include <jaiscript/detail/transient_read.hpp>
#include <functional>
#include <iterator>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <tuple>
#include <vector>
#include <algorithm>
#include <limits>
#include <fstream>
#include <filesystem>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// These tests pin the VM backend explicitly (never make_engine) so they exercise the
// bytecode VM regardless of the runner's --backend flag.
class vm_backend_tests : public suite {
	static std::shared_ptr<jai::engine> vm_engine() {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(jai::backend_type::vm);
		return e;
	}
	static std::shared_ptr<jai::engine> interp_engine() {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(jai::backend_type::interpreter);
		return e;
	}

public:
	vm_backend_tests() : suite("VM Backend") {}

	void forge_tests() override {
		test("backend_name", [this]() {
			auto e = vm_engine();
			check_eq(std::string("vm"), e->get_backend_name());
		});

		test("a_new_engine_runs_the_vm_and_the_interpreter_is_selectable", [this]() {
			auto e = jai::foundry::make_default_engine();
			check_true(e->get_backend_type() == jai::backend_type::vm);
			check_true(e->aggregate_lowering(), "the vm's default lowering");
			check_eq(std::string("vm"), e->get_backend_name());
			check_eq((int64_t)4, e->execute("2 + 2").as_int());
			auto chosen = jai::foundry::make_default_engine();
			chosen->set_backend(jai::backend_type::auto_select);
			check_true(chosen->get_backend_type() == jai::backend_type::vm, "auto_select is the default");
			auto i = jai::foundry::make_default_engine();
			i->set_backend(jai::backend_type::interpreter);
			check_true(i->get_backend_type() == jai::backend_type::interpreter);
			check_false(i->aggregate_lowering());
			check_eq((int64_t)4, i->execute("2 + 2").as_int());
			check_eq(std::string("interpreter"), i->get_backend_name());
			check_throws([&]() { i->set_backend(jai::backend_type::vm); }, "the backend is fixed once code ran");
		});

		test("arithmetic_basics", [this]() {
			auto e = vm_engine();
			check_eq((int64_t)15, e->execute("10 + 5").as_int());
			check_eq((int64_t)5, e->execute("10 - 5").as_int());
			check_eq((int64_t)50, e->execute("10 * 5").as_int());
			check_eq((int64_t)2, e->execute("10 / 5").as_int());
			check_eq((int64_t)1, e->execute("10 % 3").as_int());
			check_eq((int64_t)14, e->execute("2 + 3 * 4").as_int());
			check_eq((int64_t)20, e->execute("(2 + 3) * 4").as_int());
			check_near(16.0, e->execute("10.5 + 5.5").as_float(), 0.0001);
			check_near(5.25, e->execute("10.5 / 2.0").as_float(), 0.0001);
			check_near(15.5, e->execute("10 + 5.5").as_float(), 0.0001);
		});

		test("variables_and_arithmetic", [this]() {
			auto e = vm_engine();
			check_eq((int64_t)30, e->execute("var a = 10; var b = 20; a + b").as_int());
			check_eq((int64_t)30, e->execute("a + b").as_int());  // globals persist
		});

		test("overflow_policy", [this]() {
			auto e = vm_engine();
			// Parse-time folding wraps silently, so force the runtime path with variables
			if (e->throw_on_overflow()) {
				check_throws([&]() {
					e->execute("var a = 9223372036854775807; var b = 1; a + b");
				}, "checked builds raise on integer overflow");
			} else {
				auto v = e->execute("var a = 9223372036854775807; var b = 1; a + b");
				check_eq(std::numeric_limits<int64_t>::min(), v.as_int());
			}
		});

		test("division_by_zero", [this]() {
			auto e = vm_engine();
			check_throws([&]() { e->execute("5 / 0"); }, "integer division by zero throws");
			check_true(std::isinf(e->execute("5.0 / 0.0").as_float()), "float division by zero is IEEE");
			check_throws([&]() { e->execute("var a = 5; var b = 0; a / b"); }, "variable division by zero throws");
		});

		test("string_concat_and_compare", [this]() {
			auto e = vm_engine();
			check_eq(std::string("Hello, World!"), e->execute("\"Hello, \" + \"World!\"").as<std::string>());
			check_eq(std::string("n=42"), e->execute("var n = 42; `n=${n}`").as<std::string>());
			check_true(e->execute("var s1 = \"apple\"; var s2 = \"banana\"; s1 < s2").as<bool>());
			check_true(e->execute("\"same\" == \"same\"").as<bool>());
			check_true(e->execute("\"a\" != \"b\"").as<bool>());
		});

		test("truthiness", [this]() {
			auto e = vm_engine();
			check_false(e->execute("!1").as<bool>());
			check_true(e->execute("!0").as<bool>());
			check_true(e->execute("!\"\"").as<bool>());
			check_false(e->execute("!\"x\"").as<bool>());
			check_true(e->execute("!null").as<bool>());
			check_eq((int64_t)1, e->execute("var v = 5; v ? 1 : 2").as_int());
			check_eq((int64_t)2, e->execute("var w = 0; w ? 1 : 2").as_int());
		});

		test("comparisons_spaceship_bitwise", [this]() {
			auto e = vm_engine();
			check_true(e->execute("5 < 10").as<bool>());
			check_true(e->execute("5 <= 5").as<bool>());
			check_true(e->execute("10 > 5").as<bool>());
			check_true(e->execute("5 >= 5").as<bool>());
			check_true(e->execute("5 == 5").as<bool>());
			check_true(e->execute("5 != 3").as<bool>());
			check_eq((int64_t)-1, e->execute("5 <=> 10").as_int());
			check_eq((int64_t)1, e->execute("10 <=> 5").as_int());
			check_eq((int64_t)0, e->execute("5 <=> 5").as_int());
			check_eq((int64_t)-1, e->execute("\"apple\" <=> \"banana\"").as_int());
			check_eq((int64_t)1, e->execute("5 & 3").as_int());
			check_eq((int64_t)7, e->execute("5 | 3").as_int());
			check_eq((int64_t)6, e->execute("5 ^ 3").as_int());
			check_eq((int64_t)20, e->execute("5 << 2").as_int());
			check_eq((int64_t)5, e->execute("20 >> 2").as_int());
			check_eq((int64_t)~5, e->execute("~5").as_int());
		});

		test("a_long_left_leaning_chain_compiles_without_native_recursion", [this]() {
			// compile_binary walks a stack-form chain's left spine in a loop: the chain the front end hands over runs at program scope and in a
			// body, and the compiler alone takes a 100000-term chain
			auto e = vm_engine();
			stdlib::register_all(*e);
			std::string chain = "y";
			for (int i = 1; i < 1000; ++i) { chain += " + y"; }
			check_eq(std::string("1000,1000,1000"), e->execute("int y = 1; int x = " + chain + "; int f() { int y = 1; return " + chain + "; } int g(int y) { return " +
			                                                    chain + "; } to_string(x) + \",\" + to_string(f()) + \",\" + to_string(g(1));").as_string());
			const source_location loc;
			const token plus(token_type::plus, std::string_view("+"), loc);
			expression_ptr acc = std::make_shared<literal_expr>(loc, script_value(int64_t(1), e.get()));
			for (int i = 1; i < 100000; ++i) { acc = std::make_shared<binary_expr>(loc, acc, plus, std::make_shared<literal_expr>(loc, script_value(int64_t(1), e.get()))); }
			{
				std::vector<declaration_ptr> program{std::make_shared<expression_decl>(loc, acc)};
				jai::vm::vm_compiler compiler(detail::engine_access::get_symbolizer(*e));
				const auto compiled = compiler.compile_program(program);
				size_t adds = 0;
				for (const auto& ins : compiled->code) { if (ins.op == jai::vm::opcode::op_binary) { ++adds; } }
				check_eq(size_t(99999), adds, "one add per link");
			}
			while (acc && acc->get_type() == node_type::binary_expr) {   // the chain dies link by link, not by a recursive release
				expression_ptr next = std::move(static_cast<binary_expr*>(acc.get())->left);
				acc = std::move(next);
			}
		});

		test("short_circuit_logic", [this]() {
			auto e = vm_engine();
			check_true(e->execute("true && true").as<bool>());
			check_false(e->execute("true && false").as<bool>());
			check_true(e->execute("false || true").as<bool>());
			check_false(e->execute("false || false").as<bool>());

			e->execute("var counter = 0;");
			e->execute("function bump() -> auto { counter = counter + 1; return true; }");
			e->execute("false && bump()");
			check_eq((int64_t)0, e->execute("counter").as_int());
			e->execute("true || bump()");
			check_eq((int64_t)0, e->execute("counter").as_int());
			e->execute("true && bump()");
			check_eq((int64_t)1, e->execute("counter").as_int());
		});

		test("increment_decrement", [this]() {
			auto e = vm_engine();
			e->execute("var x = 5;");
			check_eq((int64_t)5, e->execute("x++").as_int());
			check_eq((int64_t)6, e->execute("x").as_int());
			check_eq((int64_t)7, e->execute("++x").as_int());
			check_eq((int64_t)7, e->execute("x--").as_int());
			check_eq((int64_t)6, e->execute("x").as_int());
			check_eq((int64_t)5, e->execute("--x").as_int());
		});

		test("compound_assignments", [this]() {
			auto e = vm_engine();
			e->execute("var x = 10;");
			check_eq((int64_t)15, e->execute("x += 5").as_int());
			check_eq((int64_t)12, e->execute("x -= 3").as_int());
			check_eq((int64_t)24, e->execute("x *= 2").as_int());
			check_eq((int64_t)6, e->execute("x /= 4").as_int());
			e->execute("var s = \"Hello\";");
			check_eq(std::string("Hello, World!"), e->execute("s += \", World!\"").as<std::string>());
		});

		test("loops_and_functions_run_on_the_vm", [this]() {
			const std::pair<const char*, std::vector<std::pair<const char*, int64_t>>> cases[] = {
				{"while_loop", {{R"(
				var sum = 0;
				var i = 1;
				while (i <= 4) {
					sum = sum + i;
					i = i + 1;
				}
				sum;
			)", 10}}},
				{"for_loop", {{R"(
				var total = 0;
				for (var i = 0; i < 10; i = i + 1) {
					total = total + i;
				}
				total;
			)", 45}}},
				{"for_loop_optional_clauses", {{R"(
				var i = 0;
				for (;;) {
					i = i + 1;
					if (i >= 5) { break; }
				}
				i;
			)", 5}}},
				{"nested_loops_break_continue", {{R"(
				var count = 0;
				for (var i = 0; i < 5; i = i + 1) {
					if (i == 1) { continue; }
					for (var j = 0; j < 5; j = j + 1) {
						if (j == 3) { break; }
						count = count + 1;
					}
				}
				count;
			)", 12}, {R"(
				var hits = 0;
				var i = 0;
				while (i < 6) {
					i = i + 1;
					if (i % 2 == 0) { continue; }
					var j = 0;
					while (true) {
						j = j + 1;
						if (j >= 2) { break; }
					}
					hits = hits + j;
				}
				hits;
			)", 6}}},
				{"functions_basic", {{R"(
				function add(int a, int b) -> int { return a + b; }
				add(3, 4);
			)", 7}}},
				{"function_recursion", {{R"(
				function fact(int n) -> int {
					if (n <= 1) { return 1; }
					return n * fact(n - 1);
				}
				fact(5);
			)", 120}}},
			};
			check_each(cases, [&](const auto& c) {
				auto e = vm_engine();
				for (const auto& [code, expected] : c.second) {
					check_eq(expected, e->execute(code).as_int(), code);
				}
			});
		});

		test("function_default_args", [this]() {
			auto e = vm_engine();
			e->execute("function scaled(int a, int b = 10) -> int { return a * b; }");
			check_eq((int64_t)50, e->execute("scaled(5)").as_int());
			check_eq((int64_t)15, e->execute("scaled(5, 3)").as_int());
		});

		test("recursion_depth_error", [this]() {
			auto e = vm_engine();
			// Non-tail shape: `return forever() + 1` keeps a real frame per call, so the
			// depth limit still applies (the bare-tail form now splices — see tco_ tests)
			check_throws([&]() {
				e->execute(R"(
					function forever() -> auto { return forever() + 1; }
					forever();
				)");
			}, "runaway recursion raises max_recursion_depth");
		});

		// === Real TCO (no artificial vm depth limit) ===
		// A tail-flagged bare self-call splices the live record + window in place; the
		// interpreter keeps its recursion limit (accepted divergence, KD-ENV-RECURSION-LIMIT
		// charter). Every decline gate below is pinned by a shape that must still limit.

		test("tco_self_tail_depth_unbounded", [this]() {
			const char* src = R"(
				function countdown(int n) -> int {
					if (n == 0) { return 0; }
					return countdown(n - 1);
				}
				countdown(100000);
			)";
			auto e = vm_engine();
			check_eq((int64_t)0, e->execute(src).as_int());   // far past the interpreter's default depth cap
			auto ie = interp_engine();
			check_throws([&]() { ie->execute(src); }, "interpreter keeps the depth limit");
		});

		test("tco_accumulator_result_exact", [this]() {
			auto e = vm_engine();
			// Untyped (conv none) chain: the value must ride every splice exactly
			check_eq((int64_t)200000 * 200001 / 2, e->execute(R"(
				function sum(auto n, auto acc) -> auto {
					if (n == 0) { return acc; }
					return sum(n - 1, acc + n);
				}
				sum(200000, 0);
			)").as_int());
		});

		test("tco_declines_inside_try", [this]() {
			// `return f(...)` inside try must keep the frame (the catch must see callee
			// throws), so depth exhausts at the limit and the deepest catch's -999
			// bubbles up as ordinary return values. A wrong splice would return 0.
			const char* src = R"(
				function f(int n) -> int {
					if (n == 0) { return 0; }
					try { return f(n - 1); } catch (e) { return -999; }
				}
				f(100000);
			)";
			auto e = vm_engine();
			e->max_call_depth(jai::backend_type::vm, 10000);
			check_eq((int64_t)-999, e->execute(src).as_int());
			auto ie = interp_engine();
			check_eq((int64_t)-999, ie->execute(src).as_int());   // parity: same shape both backends
		});

		test("tco_infinite_tail_hits_budget", [this]() {
			// An infinite tail loop is now a LOOP: the budget check at the splice keeps
			// it interruptible exactly like while(true)
			auto e = vm_engine();
			e->execution_budget(0.05);
			check_throws([&]() {
				e->execute(R"(
					function forever() -> auto { return forever(); }
					forever();
				)");
			}, "infinite tail recursion raises the execution budget error");
		});

		// Regression: a plain-function call inside a method must
		// NOT clear the caller method scope's this-binding — field reads and this.x keep
		// working after the call, on both backends.
		test("plain_callee_keeps_caller_this", [this]() {
			const char* unqualified_field_read = R"(
				int helper() { return 1; }
				class P { int x = 5; int m() { helper(); return x; } }
				var p = P();
				p.m();
			)";
			const char* qualified_this_read = R"(
				int helper() { return 1; }
				class Q { int x = 5; auto m() { helper(); return this.x; } }
				var q = Q();
				q.m();
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq((int64_t)5, e->execute(unqualified_field_read).as_int(),
					"unqualified field read after a plain-function call");
				check_eq((int64_t)5, e->execute(qualified_this_read).as_int(),
					"this.x after a plain-function call");
			}
		});

		// Compile-time lazy envs: plain in-loop callees whose bodies provably never
		// touch the per-call scope environment skip creating it. These pin the parity contract
		// on both backends and the compiler's fail-closed trigger table.
		test("lazy_env_callee_keeps_caller_this_from_method_top_scope", [this]() {
			// helper's body is lazy-eligible (no env created): the caller method's this
			// must survive the call on both backends
			const char* src = R"(
				int helper(int a) { return a + 1; }
				class L { int x = 5; int m() { helper(1); return x; } }
				var l = L();
				l.m();
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq((int64_t)5, e->execute(src).as_int(),
					"field read after a lazy-elided plain call");
			}
		});

		test("lazy_env_calls_keep_the_callers_bindings", [this]() {
			const std::tuple<const char*, const char*, int64_t> cases[] = {
				// Caller env at the call is a plain block env, so neither the eager two-level
				// walk nor the lazy single-level rule clears this: the field stays readable
				{"lazy_env_no_this_clear_from_method_block_scope", R"(
				int helper(int a) { return a + 1; }
				class B { int x = 5; int m() { { var t = helper(1); return x + t; } } }
				var b = B();
				b.m();
			)", 7},
				// Caller env metadata resolves through env == environment_.get() into a lazy
				// callee's reference parameter; repeated calls exercise pooled-record reuse.
				// (By-ref args THROUGH an intermediate function frame diverge between the
				// backends today — pre-existing before lazy envs, deliberately not pinned here.)
				{"lazy_env_ref_param_binding", R"(
				function bump(int& t) { t = t + 1; }
				var v = 5;
				bump(v);
				bump(v);
				v;
			)", 7},
				// op_decl_ref bodies stay eager (references capture env identity); semantics
				// must remain byte-identical on both backends
				{"lazy_env_ref_decl_callee_parity", R"(
				var gv = 4;
				function via_ref() -> int { int& r = gv; r = r * 3; return gv; }
				via_ref();
			)", 12},
			};
			check_each(cases, [&](const auto& c) {
				for (bool use_vm : {false, true}) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					check_eq(std::get<2>(c), e->execute(std::get<1>(c)).as_int());
				}
			});
		});

		test("lazy_env_throw_caught_by_caller", [this]() {
			const char* src = R"(
				function boom(int x) { throw "lazy bang"; }
				var msg = "";
				try { boom(1); } catch (err) { msg = err; }
				msg;
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq(std::string("lazy bang"), e->execute(src).as<std::string>());
			}
		});

		test("needs_frame_env_trigger_table", [this]() {
			auto e = vm_engine();
			auto body_needs_env = [&](const char* script) -> bool {
				auto* symbolizer = detail::engine_access::get_symbolizer(*e);
				auto templates = detail::engine_access::get_registered_template_types(*e);
				jai::lexer lex(script, symbolizer);
				auto tokens = lex.tokenize();
				jai::parser p(tokens, symbolizer, e.get(), templates);
				auto parsed = p.parse();
				check_true(parsed.has_value(), "trigger-table snippet parses");
				for (const auto& d : parsed.value()) {
					if (d->get_type() == jai::node_type::function_decl) {
						auto fn = std::static_pointer_cast<jai::function_decl>(d);
						jai::vm::vm_compiler compiler(symbolizer);
						auto compiled = compiler.compile_callable(fn->name, fn->parameters, fn->body, fn->local_count);
						return compiled->needs_frame_env;
					}
				}
				check_true(false, "no function decl in trigger-table snippet");
				return true;
			};
			// Lazy-eligible bodies (call-dense recursion is the whole point)
			check_false(body_needs_env("function fib(auto n) -> auto { if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); }"),
				"fib-shaped body elides its frame env");
			check_false(body_needs_env("function bump(int& t) { t = t + 1; }"),
				"ref-param assignment body elides");
			check_false(body_needs_env("function boom() { throw \"bang\"; }"),
				"throwing body elides");
			// Trigger table: env-identity-capturing bodies stay eager
			// a reference declaration lands in its own frame slot and binds a boxed slot or an owner-pinned cell,
			// never this frame's env, so the body stays lazy-eligible
			check_false(body_needs_env("function refs(int x) -> int { int& r = x; r = 7; return x; }"),
				"reference decl into a frame slot elides the env");
			check_true(body_needs_env("function closes() -> auto { var n = 2; return [=](int v) { return v + n; }; }"),
				"closure mint keeps the eager env");
			check_true(body_needs_env("function tries() -> int { try { return 1; } catch (e) { return 0; } }"),
				"try/catch keeps the eager env");
			// Slot-only block decls no longer push a scope inside callables (the env
			// they forced held nothing and its push/reset bumped the global env epoch,
			// poisoning every vm lookup cache) - the body is now lazy-eligible
			check_false(body_needs_env("function scoped() -> int { { var t = 1; t = t + 1; } return 2; }"),
				"slot-only block decl elides the scope AND the eager env");
			// Slotted loop vars skip their loop-scope envs entirely, so both loop forms
			// are lazy-eligible (unslotted vars keep the scope and the eager env)
			check_false(body_needs_env("function sums(auto a) -> int { int t = 0; for (auto v : a) { t = t + v; } return t; }"),
				"slotted range-for elides the iter scope AND the eager env");
			check_false(body_needs_env("function csum(int n) -> int { int t = 0; for (int i = 0; i < n; ++i) { t = t + i; } return t; }"),
				"slotted counted-for elides the loop scope AND the eager env");
			// A block whose decl is env-resident (nested function decl) still scopes
			check_true(body_needs_env("function outer() -> int { { function inner() -> int { return 1; } return inner(); } }"),
				"env-defining block decl keeps scope + eager env");
			// op_parallel_for is deliberately OFF the lazy allow-list (fail-closed):
			// captured reads resolve through the statement's env chain
			check_true(body_needs_env("function par(var a) { parallel_for (auto& x : a) { x = x + 1; } }"),
				"parallel_for keeps the eager env");
		});

		test("method_env_reusable_trigger_table", [this]() {
			// Sticky-method-scope gate (chunk::method_env_reusable): superset of the lazy
			// list - member access stays eligible (this/access-context re-stamped per
			// call); anything parking state in the env or capturing its identity beyond
			// the call disqualifies (fail-closed).
			auto e = vm_engine();
			auto body_reusable = [&](const char* script) -> bool {
				auto* symbolizer = detail::engine_access::get_symbolizer(*e);
				auto templates = detail::engine_access::get_registered_template_types(*e);
				jai::lexer lex(script, symbolizer);
				auto tokens = lex.tokenize();
				jai::parser p(tokens, symbolizer, e.get(), templates);
				auto parsed = p.parse();
				check_true(parsed.has_value(), "reusable-table snippet parses");
				for (const auto& d : parsed.value()) {
					if (d->get_type() == jai::node_type::function_decl) {
						auto fn = std::static_pointer_cast<jai::function_decl>(d);
						jai::vm::vm_compiler compiler(symbolizer);
						auto compiled = compiler.compile_callable(fn->name, fn->parameters, fn->body, fn->local_count);
						return compiled->method_env_reusable;
					}
				}
				check_true(false, "no function decl in reusable-table snippet");
				return false;
			};
			check_true(body_reusable("function fib(auto n) -> auto { if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); }"),
				"slot-only body is sticky-eligible (lazy list subset)");
			check_true(body_reusable("function getx(auto o) -> auto { return o.x; }"),
				"member read stays sticky-eligible");
			check_true(body_reusable("function setx(auto o) { o.x = 5; }"),
				"member write stays sticky-eligible");
			check_false(body_reusable("function closes() -> auto { var n = 2; return [=](int v) { return v + n; }; }"),
				"closure mint captures the env - not sticky");
			check_true(body_reusable("function refs(int x) -> int { int& r = x; r = 7; return x; }"),
				"reference decl into a frame slot - sticky-eligible (it parks nothing in the env)");
			check_false(body_reusable("function tries() -> int { try { return 1; } catch (e) { return 0; } }"),
				"try/catch - not sticky");
			check_true(body_reusable("function iter(auto a) -> int { int t = 0; for (auto v : a) { t = t + v; } return t; }"),
				"slotted range-for consults the env only - sticky-eligible");
			check_true(body_reusable("function fused(auto o) -> int { int t = o.x + 1; t = t * 2; return t; }"),
				"member access + fused decl/store stays sticky (eager body, reusable env)");
			check_false(body_reusable("function outer() -> int { { function inner() -> int { return 1; } return inner(); } }"),
				"env-defining block decl - not sticky");
		});

		test("sticky_method_env_receiver_rebind", [this]() {
			// Alternating receivers through the SAME dispatcher must each see their own
			// fields (the sticky env re-binds 'this' per call); repeated calls keep state.
			const char* src = R"(
				class V { int x = 0; V(int n) { x = n; } int bump() { x = x + 1; return x; } int probe() { return this.x; } }
				var a = V(10);
				var b = V(100);
				a.bump(); b.bump(); a.bump(); b.bump(); a.bump();
				a.probe() * 1000 + b.probe();
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq((int64_t)13102, e->execute(src).as_int(),
					use_vm ? "[vm] receiver rebind" : "[interp] receiver rebind");
			}
		});

		test("sticky_method_env_global_read_through", [this]() {
			// Method bodies resolve globals through the persistent scope's caches; global
			// reassignment AND redefinition (across executes) write the global env in
			// place, so warmed cached pointers must read the new values.
			const char* src = R"(
				var gmul = 3;
				function triple(int v) -> int { return v * gmul; }
				class U { int u(int v) { return triple(v) + gmul; } }
				var q = U();
				int first = q.u(4) + q.u(4);
				gmul = 5;
				int second = q.u(4);
				first * 100 + second;
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->disable(jai::filesystem);
				const std::string path = "D:/virtual/sticky/unit.jai";
				// first = (12+3)*2 = 30, second = 20+5 = 25
				check_eq((int64_t)3025, e->execute_file_source(path, src).as_int(),
					use_vm ? "[vm] global read-through" : "[interp] global read-through");
				// Redefinition of a warm global by the same unit (a reload): in place into stable storage
				check_eq((int64_t)35, e->execute_file_source(path, "var gmul = 7; q.u(4);").as_int(),
					use_vm ? "[vm] global redefinition read-through" : "[interp] global redefinition read-through");
				// Redefinition by another unit shadows: the method keeps its class's unit binding
				check_eq((int64_t)35, e->execute("var gmul = 9; q.u(4);").as_int(),
					use_vm ? "[vm] another unit's redefinition shadows" : "[interp] another unit's redefinition shadows");
			}
		});

		test("sticky_method_env_recursion_falls_back", [this]() {
			// Self-recursive method calls re-enter while the sticky env is busy
			// (use_count > 1): inner activations take the pooled path and each depth
			// keeps its own receiver binding.
			const char* src = R"(
				class F { int fib(int n) { if (n <= 1) { return n; } return this.fib(n - 1) + this.fib(n - 2); } }
				var f = F();
				f.fib(10);
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq((int64_t)55, e->execute(src).as_int(),
					use_vm ? "[vm] recursive method" : "[interp] recursive method");
			}
		});

		test("sticky_method_env_recursion_depth_error_catchable", [this]() {
			// Warm the sticky scope first, then unbounded self-recursion must still raise
			// the catchable depth error and leave the engine usable.
			const char* src = R"(
				class R { int ok() { return 1; } int deep(int n) { return this.deep(n + 1); } }
				var r = R();
				r.ok(); r.ok();
				var msg = "";
				try { r.deep(0); } catch (err) { msg = err; }
				`${msg}|${r.ok()}`;
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				auto out = e->execute(src).as<std::string>();
				check_true(out.find("recursion") != std::string::npos,
					"depth error is raised and catchable through the sticky entry");
				check_true(out.size() >= 2 && out.substr(out.size() - 2) == "|1",
					"engine (and the method) stay usable after the unwind");
			}
		});

		test("method_closure_mint_keeps_per_call_env", [this]() {
			// Closure-minting bodies are NOT sticky-eligible: each call's env is private,
			// so closures from different receivers keep distinct this-bindings.
			const char* src = R"(
				class C2 { int k = 0; C2(int n) { k = n; } auto mk() { return [this]() { return this.k; }; } }
				var a = C2(7);
				var b = C2(9);
				var fa = a.mk();
				var fb = b.mk();
				fa() * 10 + fb();
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq((int64_t)79, e->execute(src).as_int(),
					use_vm ? "[vm] closures keep per-call env" : "[interp] closures keep per-call env");
			}
		});

		test("sticky_method_env_hot_reload_reresolves", [this]() {
			// A warmed sticky scope belongs to the OLD dispatcher; class redefinition
			// mints a new dispatcher and the next call must resolve the new body.
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				auto first = e->execute(R"(
					class H { int v() { return 1; } }
					var h = H();
					h.v(); h.v();
					h.v();
				)");
				check_eq((int64_t)1, first.as_int(), "warmed original body");
				auto second = e->execute(R"(
					class H { int v() { return 2; } }
					h.v();
				)");
				check_eq((int64_t)2, second.as_int(), "reloaded body resolves through the fresh dispatcher");
			}
		});

		test("sticky_method_env_private_access_context", [this]() {
			// The persistent scope carries the declaring class as access context: private
			// members stay reachable from inside, unreachable from outside, across
			// repeated warmed calls and alternating receivers.
			const char* src = R"(
				class P {
				private:
					int secret = 21;
					int half() { return secret / 3; }
				public:
					int peek() { return this.secret + half(); }
				}
				var p = P();
				var q = P();
				p.peek(); q.peek();
				var out = `${p.peek()}|${q.peek()}`;
				try { var leak = p.secret; out = out + "|LEAK"; } catch (err) { out = out + "|blocked"; }
				out;
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq(std::string("28|28|blocked"), e->execute(src).as<std::string>(),
					use_vm ? "[vm] access context" : "[interp] access context");
			}
		});

		// Method flattening: method self-recursion does not stack native
		// frames, but the depth-10000 error must still be raised and stay catchable
		// through the flattened path on both backends.
		test("ctor_self_recursion_raises_catchable_error", [this]() {
			// Constructor recursion runs on the native entry path with wide frames; it
			// used to hard-crash (0xC00000FD) in Debug before reaching the depth cap.
			// Either guard (depth cap or native-stack headroom) must fire catchably.
			const char* src = R"(
				class C { C() { var c = C(); } }
				var msg = "";
				try { var root = C(); } catch (err) { msg = err; }
				msg;
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				auto msg = e->execute(src).as<std::string>();
				check_true(msg.find("recursion") != std::string::npos,
					"ctor self-recursion raises a catchable recursion error");
			}
		});

		test("method_recursion_depth_error_catchable", [this]() {
			const char* src = R"(
				class R { int spin(int n) { return this.spin(n + 1); } }
				var msg = "";
				try { R().spin(0); } catch (err) { msg = err; }
				msg;
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);   // depth-10000 method recursion outruns the default budget in Debug
				auto msg = e->execute(src).as<std::string>();
				check_true(msg.find("Maximum recursion depth") != std::string::npos,
					"method self-recursion raises the catchable depth error");
			}
		});

		// Metadata skip: ref-free direct callees skip the per-call arg-metadata
		// build, so current_arg_metadata_ holds a stale outer snapshot while they run.
		// Ref-param binding around and after such calls must be unaffected.
		test("ref_params_bind_the_callers_variable_wherever_it_lives", [this]() {
			const std::tuple<const char*, const char*, int64_t> cases[] = {
				{"ref_param_call_surrounds_ref_free_call", R"(
				int plain(int a) { return a + 1; }
				function bump(int& t) { var w = plain(2); t = t + w; }
				var v = 5;
				bump(v);
				bump(v);
				v;
			)", 11},
				// plain() runs (and skips metadata) while bump's argument list is being
				// evaluated; bump's own ref metadata is built afterwards and must bind v
				{"ref_free_call_inside_ref_call_arguments", R"(
				int plain(int a) { return a * 2; }
				function bump(int& t, int by) { t = t + by; }
				var v = 1;
				bump(v, plain(3));
				v;
			)", 7},
				{"ref_param_from_block_and_loop_scopes", R"(
				function bump(int& t) { t = t + 1; }
				var a = 0;
				{ bump(a); }
				if (true) { bump(a); }
				var i = 0;
				while (i < 2) { bump(a); i = i + 1; }
				for (int k = 0; k < 3; k++) { bump(a); }
				for (auto x : [1, 2]) { bump(a); }
				a;
			)", 9},
				// The worst historical failure: the callee wrote through a same-named GLOBAL
				// while the caller's slot local stayed untouched
				{"ref_param_binds_caller_slot_local_not_same_named_global", R"(
				var t = 100;
				function bump(int& t) { t = t + 1; }
				function caller() -> int { var t = 5; bump(t); return t; }
				caller() * 1000 + t;
			)", 6100},
				{"ref_param_slot_local_from_inner_block_survives_block_exit", R"(
				function bump(int& t) { t = t + 1; }
				function f() -> int {
					var t = 5;
					{ bump(t); }
					return t;
				}
				f();
			)", 6},
				{"ref_param_global_from_method_body", R"(
				var g = 1;
				function bumpg(int& t) { t = t + 5; }
				class C { int m() { bumpg(g); return g; } }
				var c = C();
				c.m();
			)", 6},
				{"ref_param_pass_through", R"(
				function inner(int& x) { x = x + 1; }
				function outer(int& y) { inner(y); }
				var v = 0;
				outer(v);
				v;
			)", 1},
				// The extra local used to flip the vm intermediate into a framed env and
				// change behavior; both shapes must work identically now
				{"ref_param_pass_through_with_intermediate_local", R"(
				function inner(int& x) { x = x + 1; }
				function mid(int& x) { var t = x; inner(x); }
				var x = 1;
				mid(x);
				x;
			)", 2},
				{"ref_param_array_mutation_from_try_block", R"(
				var arr = [1];
				function r1(auto& a) { a.push(4); }
				var failed = 0;
				try { r1(arr); } catch (e) { failed = 1; }
				arr.size() * 10 + failed;
			)", 20},
				{"ref_param_default_arg_slow_path_from_function_local", R"(
				function r4(auto& a, int z = 0) { a.push(9); }
				function f() -> int {
					var arr = [1];
					r4(arr);
					return arr.size();
				}
				f();
			)", 2},
				{"ref_param_two_same_named_block_locals", R"(
				function bump(int& t) { t = t + 1; }
				function f() -> int {
					var acc = 0;
					{ var t = 10; bump(t); acc = acc + t; }
					{ var t = 20; bump(t); acc = acc + t; }
					return acc;
				}
				f();
			)", 32},
				{"ref_param_interleaved_locals_in_loop", R"(
				function bump(int& t) { t = t + 1; }
				function f() -> int {
					var a = 0;
					var b = 100;
					for (int i = 0; i < 3; i++) { bump(a); bump(b); }
					return a * 1000 + b;
				}
				f();
			)", 3103},
				// The mutation must land in the instance field on both backends. (A bare
				// field read right after a plain call in the same method body hits the
				// separately-pinned caller-this-clear quirk, so the size is read outside.)
				{"ref_param_instance_field_from_method_body", R"(
				function pushRef(auto& a) { a.push(5); }
				class Holder {
					auto d = [1];
					int go() { pushRef(d); return 7; }
				}
				var h = Holder();
				var r = h.go();
				r * 10 + h.d.size();
			)", 72},
				// pre-fix: green both backends (slot-path flatten kept the origin anchor alive)
				{"ref_param_pass_through_chain_depth3", R"(
				function inc(int& x) { x = x + 1; }
				function relay1(int& y) { inc(y); }
				function relay2(int& z) { relay1(z); }
				function origin() -> int { var t = 5; relay2(t); return t; }
				origin();
			)", 6},
				// pre-fix: green both backends
				{"ref_param_pass_through_env_origin_depth3", R"(
				function inc(int& x) { x = x + 7; }
				function relay1(int& y) { inc(y); }
				function relay2(int& z) { relay1(z); }
				var v = 1;
				relay2(v);
				v;
			)", 8},
				// pre-fix: green both backends (env-path flatten of a global ref decl)
				{"ref_param_env_visible_ref_decl_arg", R"(
				var v = 1;
				auto& g = v;
				function bump(int& t) { t = t + 9; }
				bump(g);
				v;
			)", 10},
				// pre-fix: green both backends (no realloc, creation-time element address valid)
				{"ref_param_element_ref_single_hop", R"(
				var arr = [1, 2, 3];
				function inc(int& x) { x = x + 10; }
				function driver() { for (auto& el : arr) { inc(el); } }
				driver();
				arr[0] + arr[1] + arr[2];
			)", 36},
				// Sanctioned fix (b): pre-fix the bind flatten pinned the creation-time element
				// address, so the write landed in the freed pre-realloc buffer (arr[0] stayed 7)
				{"ref_param_element_ref_pass_through_realloc", R"(
				var arr = [7];
				function grow(int& x) {
					for (int i = 0; i < 64; i = i + 1) { arr.push(0); }
					x = 42;
				}
				function relay(int& x) { grow(x); }
				function driver() { for (auto& el : arr) { relay(el); break; } }
				driver();
				arr[0];
			)", 42},
				// Compound stores on a ref param bound to an array element go through the
				// re-resolving deref (the shared holder is container-based, not a pointer).
				// (++ on a ref param is a separate pre-existing gap on both backends.)
				{"ref_param_element_compound_store", R"(
				var arr = [5, 6];
				function addTo(int& x) { x += 10; }
				function driver() { for (auto& el : arr) { addTo(el); } }
				driver();
				arr[0] * 100 + arr[1];
			)", 1516},
				// pre-fix: green both backends (one fresh holder per hop); post-fix shares one holder
				{"ref_param_recursive_pass_down", R"(
				function down(int& x, int n) { if (n > 0) { down(x, n - 1); } else { x = 99; } }
				function go() -> int { var v = 0; down(v, 40); return v; }
				go();
			)", 99},
			};
			check_each(cases, [&](const auto& c) {
				for (bool use_vm : {false, true}) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					check_eq(std::get<2>(c), e->execute(std::get<1>(c)).as_int());
				}
			});
		});

		// Shared-holder pass-through: passing a bound reference onward shares the
		// original holder instead of minting a new one per hop, so element refs keep their
		// container re-resolution and env-anchored refs keep the origin anchor.

		test("ref_param_element_ref_shrink_errors", [this]() {
			// Sanctioned fix (b): pre-fix the write went through a stale pointer into the
			// popped element (silent corruption, msg stayed empty); now the re-resolve errors
			const char* src = R"(
				var arr = [1, 2, 3];
				var msg = "";
				function wipe(int& x) { arr.pop(); arr.pop(); arr.pop(); x = 5; }
				function driver() { for (auto& el : arr) { try { wipe(el); } catch (e) { msg = e; } break; } }
				driver();
				msg;
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				auto msg = e->execute(src).as<std::string>();
				check_true(msg.find("removed array element") != std::string::npos,
					"shrunk element write errors");
				if (!use_vm) { first = msg; } else { check_eq(first, msg, "identical error text on both backends"); }
			}
		});

		test("element_ref_arg_error_script_catchable_parity", [this]() {
			// OOB element-ref deref during call-ARGUMENT evaluation must be script-catchable
			// in the calling frame on BOTH backends (interpreter converts at visit_call; the
			// VM converts in-frame inside call-arg zones instead of popping the frame or
			// rethrowing at records_base).
			const char* srcs[] = {
				// top-level ref-param call, decl-ref arg (pre-fix: VM escaped execute() entirely)
				R"(
					var arr = [1, 2, 3];
					auto& x = arr[2];
					arr.pop();
					var msg = "";
					function f(int& t) { }
					try { f(x); } catch (e) { msg = e; }
					msg;
				)",
				// top-level value-param call
				R"(
					var arr = [1, 2, 3];
					auto& x = arr[2];
					arr.pop();
					var msg = "";
					function g(int t) { }
					try { g(x); } catch (e) { msg = e; }
					msg;
				)",
				// in-function range-for element arg (pre-fix: VM popped driver's frame, its try never fired)
				R"(
					var arr = [1, 2, 3];
					var msg = "";
					function readIt(int& x) { return x; }
					function driver() { for (auto& el : arr) { arr.pop(); arr.pop(); arr.pop(); try { readIt(el); } catch (e) { msg = e; } break; } }
					driver();
					msg;
				)",
				// method-call argument zone
				R"(
					var arr = [1, 2, 3];
					var msg = "";
					class C { int take(int v) { return v; } }
					function driver() { auto c = C(); for (auto& el : arr) { arr.pop(); arr.pop(); arr.pop(); try { c.take(el); } catch (e) { msg = e; } break; } }
					driver();
					msg;
				)",
			};
			for (const char* src : srcs) {
				std::string first;
				for (bool use_vm : {false, true}) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					auto msg = e->execute(src).as<std::string>();
					check_true(msg.find("removed array element") != std::string::npos,
						"arg-position OOB element ref caught by the calling frame's try");
					if (!use_vm) { first = msg; } else { check_eq(first, msg, "identical error text on both backends"); }
				}
			}
		});

		test("element_ref_callee_error_caught_at_depth2", [this]() {
			// Mid-call shrink two hops down a shared-holder chain: the store's re-resolve
			// error must unwind to the outermost script try identically on both backends.
			const char* src = R"(
				var arr = [1, 2, 3];
				var msg = "";
				function w(int& x) { arr.pop(); arr.pop(); arr.pop(); x = 5; }
				function relay(int& x) { w(x); }
				function driver() { for (auto& el : arr) { try { relay(el); } catch (e) { msg = e; } break; } }
				driver();
				msg;
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				auto msg = e->execute(src).as<std::string>();
				check_true(msg.find("removed array element") != std::string::npos,
					"depth-2 write error unwinds to driver's try");
				if (!use_vm) { first = msg; } else { check_eq(first, msg, "identical error text on both backends"); }
			}
		});

		test("unwinding_skips_typed_return_conversion", [this]() {
			// An exception in flight across a typed-return frame must keep its text: the
			// interpreter's call epilogue used to run the -> int conversion on the junk
			// return slot and replace the error with "Type mismatch: expected int but got this".
			const char* src = R"(
				var arr = [1, 2, 3];
				var msg = "";
				function readIt(int& x) -> int { return x; }
				function wipeThenRead(int& x) -> int { arr.pop(); arr.pop(); arr.pop(); return readIt(x); }
				function driver() { for (auto& el : arr) { try { wipeThenRead(el); } catch (e) { msg = e; } break; } }
				driver();
				msg;
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				auto msg = e->execute(src).as<std::string>();
				check_true(msg.find("removed array element") != std::string::npos,
					"typed-return frame does not mangle the in-flight error");
				if (!use_vm) { first = msg; } else { check_eq(first, msg, "identical error text on both backends"); }
			}
		});

		test("ref_decls_share_their_target", [this]() {
			const std::tuple<const char*, const char*, int64_t> cases[] = {
				// pre-fix: green both backends (decl flatten re-targeted v directly)
				{"ref_decl_shares_env_ref", R"(
				var v = 1;
				auto& g = v;
				auto& h = g;
				h = h + 3;
				v;
			)", 4},
				// Sanctioned fix (b) at the decl site: pre-fix auto& flattened the element ref
				// into a raw pointer, so the post-realloc write was lost (arr[1] stayed 8)
				{"ref_decl_element_ref_survives_realloc", R"(
				var arr = [7, 8];
				auto& x = arr[1];
				for (int i = 0; i < 64; i = i + 1) { arr.push(0); }
				x = 42;
				arr[1];
			)", 42},
			};
			check_each(cases, [&](const auto& c) {
				for (bool use_vm : {false, true}) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					check_eq(std::get<2>(c), e->execute(std::get<1>(c)).as_int());
				}
			});
		});

		test("element_and_hop_references_keep_their_element_type", [this]() {
			const std::tuple<const char*, const char*, const char*> cases[] = {
				// Element refs from typed containers CARRY the container's element type, so
				// the retyping write through an auto& decl errors with the same text as
				// direct element assignment, and the element keeps its value.
				{"ref_decl_element_constraint_enforced", R"(
				array<int> arr = [1, 2, 3];
				auto& x = arr[1];
				var msg = "";
				try { x = "s"; } catch (e) { msg = e; }
				msg + "|" + type_of(arr[1]) + "|" + to_string(arr[1]);
			)", "Cannot assign 'string' to element of type 'int'|int|2"},
				// A ref decl aliasing another constrained element ref decl shares the holder
				// AND its constraint (the alias writes through and enforces identically)
				{"ref_decl_element_constraint_through_alias_chain", R"(
				array<int> arr = [5];
				auto& outer = arr[0];
				auto& local = outer;
				var msg = "";
				try { local = "s"; } catch (e) { msg = e; }
				local = 6;
				msg + "|" + to_string(arr[0]);
			)", "Cannot assign 'string' to element of type 'int'|6"},
				// Wrapper-tag canary: sharing keeps the origin's reference tag; type_of must
				// keep reporting the referent type at every hop
				{"ref_param_type_of_stable_through_hops", R"(
				function leaf(int& x) -> string { return type_of(x); }
				function mid(int& y) -> string { return type_of(y) + ":" + leaf(y); }
				var v = 5;
				mid(v);
			)", "int:int"},
				// pre-fix: "6|" both backends (the [&] capture snapshots the ref param's value,
				// so the escaped lambda works on its own copy). Sharing must not change this.
				{"ref_escape_via_lambda_after_frame_death", R"(
				var f = null;
				function trap(int& x) { f = [&]() { x = x + 1; return x; }; }
				function run() -> int { var v = 5; trap(v); return v; }
				run();
				var msg = "";
				var got = -1;
				try { got = f(); } catch (e) { msg = e; }
				to_string(got) + "|" + msg;
			)", "6|"},
				// Reads through the bound ref see live field state; type_of reports the referent
				{"tier1_type_of_and_read_back_through_bound_field_ref", R"(
				class Box { int v = 7; }
				function probe(int& x) -> string { var t = type_of(x); x = x + 1; return t + ":" + to_string(x); }
				var b = Box();
				probe(b.v) + ":" + to_string(b.v);
			)", "int:8:8"},
			};
			check_each(cases, [&](const auto& c) {
				for (bool use_vm : {false, true}) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					jai::stdlib::register_all(e);
					check_eq(std::string(std::get<2>(c)), e->execute(std::get<1>(c)).template as<std::string>());
				}
			});
		});

		test("ref_element_constraint_all_shapes", [this]() {
			// The same constraint holds for every element-ref shape: range-for auto&,
			// ref params bound to elements, compound stores through decl refs. var
			// (any-tagged) containers stay unconstrained; valid stores keep working.
			const char* src = R"(
				array<int> arr = [1, 2, 3];
				var out = "";
				try { for (auto& v : arr) { v = "s"; } out = out + "FOR-LEAK"; }
				catch (e) { out = out + e; }
				for (auto& v : arr) { v = v * 2; }
				out = out + "|" + to_string(arr[0]);
				function retype(auto& r) { r = "s"; }
				try { retype(arr[0]); out = out + "|PARAM-LEAK"; } catch (e) { out = out + "|" + e; }
				function bump(int& r) { r += 1; }
				bump(arr[2]);
				out = out + "|" + to_string(arr[2]);
				auto& c = arr[1];
				c += 100;
				out = out + "|" + to_string(arr[1]);
				var vArr = [1, "two", 3.5];
				auto& y = vArr[0];
				y = "retyped";
				out = out + "|" + vArr[0];
				out;
			)";
			const std::string expected =
				"Cannot assign 'string' to element of type 'int'|2"
				"|Cannot assign 'string' to element of type 'int'|7|104|retyped";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				check_eq(expected, e->execute(src).as<std::string>(),
				         use_vm ? "vm element constraint shapes" : "interp element constraint shapes");
			}
		});

		test("ref_param_calls_inside_coroutine_across_yields", [this]() {
			// Metadata invariant: inner ref-param calls complete before each yield, so the
			// suspended fiber never holds in-flight arg metadata
			const char* src = R"(
				function bump(int& t) { t = t + 1; }
				coroutine int co() {
					var a = 0;
					bump(a);
					yield a;
					bump(a);
					yield a;
					return a * 10;
				}
				auto c = co();
				auto r1 = c.next();
				auto r2 = c.next();
				auto r3 = c.next();
				r1 * 100 + r2 * 10 + r3;
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				check_eq((int64_t)140, e->execute(src).as_int());
			}
		});

		// Stage C (cells): typed reference returns. int& f() / -> auto& returns a
		// usable reference; the holder's owner pin (cell/element/field) is the
		// lifetime, so the returned ref outliving the frame is legal.
		test("ref_returns_name_storage_that_outlives_the_frame", [this]() {
			const std::tuple<const char*, const char*, int64_t> cases[] = {
				// The exact shape: reference returns + reference locals
				{"ref_return_grammar_example", R"(
				int& pick(array<int>& a, int i) { return a[i]; }
				array<int> nums = [10, 20, 30, 40];
				auto& slot = pick(nums, 2);
				slot = 99;
				nums[2];
			)", 99},
				// Returning a ref to a LOCAL is legal: the local boxes into a cell and the
				// returned handle keeps it alive after the frame dies. Each call boxes a
				// FRESH cell (no cross-call sharing).
				{"ref_return_local_cell_outlives_frame", R"(
				int& leak() { int v = 7; return v; }
				auto& a = leak();
				auto& b = leak();
				a = a + 1;
				b = b + 3;
				a * 100 + b;
			)", 810},
				// Method ref return (-> int&) over this.field: instance-pinned field ref
				{"ref_return_method_field_trailing_arrow", R"(
				class Orc {
					int hp = 10;
					function heal() -> int& { return this.hp; }
				}
				auto o = Orc();
				auto& h = o.heal();
				h = 50;
				o.hp;
			)", 50},
				// f().field reads through the returned reference without an alias decl
				{"ref_return_chained_member_read", R"(
				class Box { int v = 3; }
				Box b = Box();
				Box& get() { return b; }
				get().v + 1;
			)", 4},
			};
			check_each(cases, [&](const auto& c) {
				for (bool use_vm : {false, true}) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					check_eq(std::get<2>(c), e->execute(std::get<1>(c)).as_int());
				}
			});
		});

		test("ref_return_non_lvalue_rejects", [this]() {
			// A reference cannot bind a temporary - same text both backends
			const char* src = R"(
				int& bad() { return 1 + 1; }
				var msg = "";
				try { bad(); } catch (e) { msg = e; }
				msg;
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq(std::string("Function with reference return type must return an lvalue reference"),
				         e->execute(src).as<std::string>());
			}
		});

		test("ref_return_referent_type_mismatch_rejects", [this]() {
			// No conversion can apply through a reference: the referent's base type
			// must match the declared referent
			const char* src = R"(
				int& bad(array<float>& a) { return a[0]; }
				array<float> fs = [1.5];
				var msg = "";
				try { bad(fs); } catch (e) { msg = e; }
				msg;
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_true(e->check(src).has_errors(), "strict rejects the referent at load");
				check_eq(std::string("Cannot return reference to 'float' from a function returning 'int&'"),
				         e->execute(std::string("[[warn_mode]]; ") + src).as<std::string>());
			}
		});

		test("coroutine_ref_return_is_parse_error", [this]() {
			// A suspended fiber's return channel is a value channel (explicit error);
			// the parse error surfaces as a throw and the engine stays usable
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_throws([&]() { e->execute("coroutine int& co() { yield 1; } 5;"); });
				check_eq((int64_t)7, e->execute("3 + 4").as_int());   // engine stays usable
			}
		});

		// Stage C (cells): escape is LEGAL. Under mode 1 these shapes dangled or raised
		// "Reference target environment has been destroyed"; the cell keeps the target
		// alive for as long as any handle exists.
		test("escape_capture_env_var_outlives_block_env", [this]() {
			// [&] capture of a BLOCK-scoped env variable, invoked after the block env
			// died: the capture shares the cell, so the closure keeps live state
			const char* src = R"(
				var f = null;
				{
					var v = 10;
					f = [&]() { v = v + 1; return v; };
				}
				f() * 10 + f();
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq((int64_t)122, e->execute(src).as_int());   // 11*10 + 12
			}
		});

		test("escape_cell_alive_across_executes", [this]() {
			// A cell escaped through a closure survives into LATER execute() calls
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq((int64_t)122, e->execute(
					"var f = null; { var v = 10; f = [&]() { v = v + 1; return v; }; } f() * 10 + f();").as_int());
				check_eq((int64_t)13, e->execute("f()").as_int());
				e->execute("function unrelated() { return 0; }");
				check_eq((int64_t)14, e->execute("f()").as_int());
			}
		});

		test("escape_hot_reload_keeps_live_cell", [this]() {
			// A top-level ref decl holds a cell minted by a function's local; the
			// function is then REDEFINED (hot reload) - the old body's cell stays
			// alive and writable through the held reference
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq((int64_t)5, e->execute(
					"int& keep() { int v = 100; return v; } auto& r = keep(); r = 5; r;").as_int());
				e->execute("function keep() { return 0; }");   // redefine while the cell escapes
				check_eq((int64_t)6, e->execute("r = r + 1; r;").as_int());
			}
		});

		test("escape_engine_teardown_with_live_cells", [this]() {
			// Teardown order with escaped cells reachable from globals must be clean
			// (Debug's 0xDD freed-memory fill catches a double-touch)
			for (bool use_vm : {false, true}) {
				{
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					auto r = e->execute(
						"var f = null; { var v = 1; f = [&]() { return v; }; } "
						"int& mk() { int q = 2; return q; } auto& g = mk(); f() + g;");
					check_eq((int64_t)3, r.as_int());
				}   // engine destroyed with live cells held by its own globals
				check_true(true, "teardown clean");
			}
		});

		// Tier 1: field/subscript/chain lvalue arguments bind to reference
		// parameters through the shared resolver (detail/ref_lvalue.hpp). Field refs pin
		// the instance, element refs pin the vector; typed fields/elements enforce their
		// bind-time tag on assign-through.
		test("tier1_field_and_element_references_write_through", [this]() {
			const std::tuple<const char*, const char*, int64_t> cases[] = {
				// The headline: recursive insert through field refs, no return-reassign
				{"tier1_bst_insert_by_ref", R"(
				class TreeNode { int value = 0; shared_ptr<TreeNode> left = null; shared_ptr<TreeNode> right = null; TreeNode(int val) { value = val; } }
				function insertNode(var& node, int val) {
					if (node == null) { node = TreeNode(val); return; }
					if (val < node.value) { insertNode(node.left, val); } else { insertNode(node.right, val); }
				}
				function inorderSum(var node) -> int {
					if (node == null) { return 0; }
					return inorderSum(node.left) + node.value + inorderSum(node.right);
				}
				var root = null;
				insertNode(root, 8);
				insertNode(root, 4);
				insertNode(root, 12);
				insertNode(root, 2);
				insertNode(root, 6);
				insertNode(root, 10);
				insertNode(root, 14);
				inorderSum(root);
			)", 56},
				{"tier1_field_ref_write_through", R"(
				class Box { int v = 1; }
				function set9(int& x) { x = 9; }
				var b = Box();
				set9(b.v);
				b.v;
			)", 9},
				// a.b.c, arr[i].field, obj.arr[j] chains all resolve to the innermost node
				{"tier1_chain_and_mixed_lvalue_args", R"(
				class Inner { int val = 1; }
				class Outer { Inner inner = Inner(); var arr = [0, 0]; }
				function bump(int& x) { x = x + 10; }
				var o = Outer();
				bump(o.inner.val);
				var boxes = [Inner(), Inner()];
				bump(boxes[1].val);
				var j = 0;
				bump(o.arr[j]);
				o.inner.val * 10000 + boxes[1].val * 100 + o.arr[0];
			)", 111110},
				// Literal index, env-resolved identifier index, and slot-local identifier index
				{"tier1_subscript_literal_and_identifier_index", R"(
				function set(int& x, int val) { x = val; }
				var arr = [1, 2, 3];
				set(arr[0], 7);
				var gi = 2;
				set(arr[gi], 8);
				function driver() { var li = 1; set(arr[li], 9); }
				driver();
				arr[0] * 100 + arr[1] * 10 + arr[2];
			)", 798},
				// enter_script_method builds the same lvalue metadata as plain calls
				{"tier1_method_callee_field_ref_arg", R"(
				class Box { int v = 0; }
				class Svc { void put(int& x, int val) { x = val; } }
				var b = Box();
				var s = Svc();
				s.put(b.v, 42);
				b.v;
			)", 42},
				{"tier1_shared_ptr_base_field_ref", R"(
				class N { int v = 1; N(int val) { v = val; } }
				function bump(int& x) { x = x + 1; }
				var sp = shared_ptr<N>(5);
				bump(sp.v);
				sp.v;
			)", 6},
				// The holder pins the instance/vector: dropping the caller's last handle
				// mid-call must leave the ref writable (Debug 0xDD canary covers dangling)
				{"tier1_owner_pinned_past_caller_handle_drop", R"(
				class Box { int v = 5; }
				var g = Box();
				var arr = [1, 2, 3];
				function fldWrite(int& x) { g = null; x = x + 1; return x; }
				function elemWrite(int& x) { arr = null; x = x + 1; return x; }
				var a = fldWrite(g.v);
				var b = elemWrite(arr[1]);
				a * 10 + b;
			)", 63},
				// Chain resolution happens at bind time, after ALL args evaluate: a later
				// arg that swaps the chain spine changes what binds (identifier precedent)
				{"tier1_bind_time_ordering_pinned", R"(
				class B { int v = 1; }
				class A { B b = B(); }
				var a = A();
				var keep = a.b;
				function swapB() -> int { a.b = B(); return 0; }
				function f(int& x, int ignored) { x = 50; }
				f(a.b.v, swapB());
				a.b.v * 10 + keep.v;
			)", 501},
				// Tier 1 x Tier 2: the bound field ref shares its holder through further hops
				{"tier1_field_ref_pass_through_hop", R"(
				class Box { int v = 1; }
				function leaf(int& x) { x = x + 1; }
				function mid(int& x) { leaf(x); x = x + 10; }
				var b = Box();
				mid(b.v);
				b.v;
			)", 12},
				// var&, auto&, int&, ClassName&, array<int>& all parse, bind, and write through
				{"tier1_ref_param_type_forms", R"(
				class P { int v = 1; }
				function f1(var& x) { x = x + 1; }
				function f2(auto& x) { x = x + 2; }
				function f3(int& x) { x = x + 3; }
				function f4(P& p) { p = P(); p.v = 99; }
				function f5(array<int>& a) { a[0] = 9; }
				class H { int n = 10; P p = P(); var arr = null; H() { arr = [0]; } }
				var h = H();
				f1(h.n);
				f2(h.n);
				f3(h.n);
				f4(h.p);
				f5(h.arr);
				h.n * 1000 + h.p.v * 10 + h.arr[0];
			)", 16999},
				// R5: member/subscript args to REF-FREE callees consume no lvalue metadata
				// and stay pure rvalue reads, surrounded by live ref-param calls
				{"tier1_member_args_to_ref_free_functions_unaffected", R"(
				class Box { int v = 3; }
				int plain(int a) { return a + 1; }
				function bump(int& t) { t = t + 1; }
				var b = Box();
				var arr = [5];
				var r1 = plain(b.v);
				bump(b.v);
				var r2 = plain(arr[0]);
				bump(b.v);
				r1 * 1000 + r2 * 10 + b.v;
			)", 4065},
			};
			check_each(cases, [&](const auto& c) {
				for (bool use_vm : {false, true}) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					check_eq(std::get<2>(c), e->execute(std::get<1>(c)).as_int());
				}
			});
		});

		test("tier1_this_field_ref_from_method", [this]() {
			// this.field as a ref arg: free-function callee and method callee. Results
			// observed from outside m() - a method's OWN this/field lookups after any
			// free-function call are broken at HEAD (pre-existing, both backends)
			const char* src = R"(
				function bump(int& x) { x = x + 1; }
				class Svc { void put(int& x, int val) { x = val; } }
				class C {
					int v = 5;
					int m() { bump(this.v); return 0; }
					int n(var s) { s.put(this.v, 40); return this.v; }
				}
				var c = C();
				c.m();
				var first = c.v;
				var second = c.n(Svc());
				first * 100 + second;
			)";
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				check_eq((int64_t)640, e->execute(src).as_int(), use_vm ? "vm" : "interp");
			}
		});

		test("tier1_typed_field_wrong_type_errors", [this]() {
			const char* src = R"(
				class Box { int v = 1; }
				function put(var& x) { x = "nope"; }
				var b = Box();
				var msg = "";
				try { put(b.v); } catch (e) { msg = e; }
				msg + "|" + to_string(b.v);
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				auto msg = e->execute(src).as<std::string>();
				check_true(msg.find("Cannot assign string to field 'v' of Box (declared int)") != std::string::npos,
					"typed field rejects retype through the ref with the direct store's text");
				check_true(msg.find("|1") != std::string::npos, "field value untouched after rejected write");
				if (!use_vm) { first = msg; } else { check_eq(first, msg, "identical error text on both backends"); }
			}
		});

		test("tier1_typed_field_reference_runs_the_fields_table", [this]() {
			// a float into an int field and null into a class value field raise the field's texts through the
			// reference, as the direct field store does; a bool converts and a shared_ptr field takes null
			const char* src = R"(
				class Leaf { int id = 7; }
				class Node { int num = 1; Leaf next = Leaf(); shared_ptr<Leaf> link = new Leaf(); }
				function put(var& x, var val) { x = val; }
				var n = Node();
				string out = "";
				try { put(n.num, 2.7); } catch (e) { out = out + e + "|"; }
				try { put(n.next, null); } catch (e) { out = out + e + "|"; }
				put(n.num, true);
				put(n.link, null);
				out + to_string(n.num) + "|" + to_string(n.next.id) + "|" + to_string(n.link == null);
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				auto got = e->execute(src).as<std::string>();
				check_eq(std::string("Cannot assign float to field 'num' of Node (declared int)|Cannot assign null to field 'next' of Node (declared Leaf)|1|7|true"), got);
				if (!use_vm) { first = got; } else { check_eq(first, got); }
			}
		});

		test("tier1_field_constraint_is_declared_type", [this]() {
			// The DECLARED field type is the ref constraint. A 'var' field
			// is dynamic — its ref binds unconstrained regardless of the stored value's tag —
			// while a declared 'int' field constrains the ref exactly like the in-method
			// identifier-assign path does.
			const char* src = R"(
				class Box { var v = 1; int i = 1; }
				function put(var& x) { x = "str"; }
				var b = Box();
				var msg = "";
				try { put(b.i); } catch (e) { msg = e; }
				put(b.v);
				to_string(b.v) + "|" + to_string(b.i) + "|" + msg;
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				auto got = e->execute(src).as<std::string>();
				check_true(got.find("str|1|") == 0, "var field binds unconstrained; int field unchanged");
				check_true(got.find("Cannot assign string to field 'i' of Box (declared int)") != std::string::npos,
					"declared int field constrains the ref");
				if (!use_vm) { first = got; } else { check_eq(first, got, "identical error text on both backends"); }
			}
		});

		test("tier1_typed_element_enforcement_through_param", [this]() {
			// Typed array element bound by f(a[i]): wrong type errors with the element
			// text; compound += goes through the same constraint machinery
			const char* src = R"(
				array<int> a = [1, 2, 3];
				function put(var& x) { x = "bad"; }
				function inc(var& x) { x += 10; }
				var msg = "";
				try { put(a[0]); } catch (e) { msg = e; }
				inc(a[1]);
				msg + "|" + to_string(a[0]) + "|" + to_string(a[1]);
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				auto got = e->execute(src).as<std::string>();
				check_true(got.find("Cannot assign 'string' to element of type 'int'") != std::string::npos,
					"typed element rejects retype through the bound ref");
				check_true(got.find("|1|12") != std::string::npos, "rejected write untouched, compound applied");
				if (!use_vm) { first = got; } else { check_eq(first, got, "identical error text on both backends"); }
			}
		});

		test("tier1_element_ref_realloc_and_shrink", [this]() {
			// Tier-1-bound element refs re-resolve container+index: writes survive
			// realloc, shrink errors instead of corrupting
			const char* src = R"(
				var arr = [7];
				var msg = "";
				function grow(int& x) { for (int i = 0; i < 64; i = i + 1) { arr.push(0); } x = 42; }
				function wipe(int& x) { while (arr.size() > 0) { arr.pop(); } x = 5; }
				grow(arr[0]);
				var grown = arr[0];
				try { wipe(arr[0]); } catch (e) { msg = e; }
				to_string(grown) + "|" + msg;
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				auto got = e->execute(src).as<std::string>();
				check_true(got.find("42|") == 0, "write through the ref survives realloc");
				check_true(got.find("removed array element") != std::string::npos, "shrunk element write errors");
				if (!use_vm) { first = got; } else { check_eq(first, got, "identical error text on both backends"); }
			}
		});

		test("tier1_non_lvalue_exclusion_matrix", [this]() {
			// Everything outside the sanctioned lvalue shapes keeps today's exact error
			const char* setup = R"(
				class Box { int v = 1; int _get_area() { return v * 2; } }
				function f(int& x) { x = 0; }
				var b = Box();
				var arr = [1, 2, 3];
				var m = {"k": 1};
				var s = "hi";
				var msg = "";
			)";
			// f(m["k"]) moved OUT of this matrix: map-key ref args bind via tier 2
			// (see tier2_general_lvalue_ref_args)
			const char* cases[] = {
				"try { f(b.v + 1); } catch (e) { msg = e; } msg;",
				"try { f(arr.size()); } catch (e) { msg = e; } msg;",
				"try { f(5); } catch (e) { msg = e; } msg;",
				"try { f(b.area); } catch (e) { msg = e; } msg;",   // computed property: no backing field
			};
			for (bool use_vm : {false, true}) {
				for (const char* c : cases) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					auto msg = e->execute(std::string("[[warn_mode]]; ") + setup + c).as<std::string>();   // the checker reports f(5) at load; the RUNTIME text is under test
					check_true(msg.find("Cannot pass non-lvalue to reference parameter") != std::string::npos,
						std::string("non-lvalue arg keeps the exact error: ") + c + " got: " + msg);
				}
				{   // a method passed as a value is the method-value error, ahead of the reference binding
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					auto msg = e->execute(std::string(setup) + "try { f(arr.size); } catch (e) { msg = e; } msg;").as<std::string>();
					check_eq(std::string("'size' is a method of array; call it with arr.size() or wrap the call in a lambda"), msg);
				}
				// String subscript args evaluate to a CHAR rvalue (read-only string
				// subscript, GLOOM item B) - the standard non-lvalue ref-bind error
				// fires like any other rvalue argument (flipped from the old
				// "Subscript can only be used on" eval-time error)
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				auto msg = e->execute(std::string(setup) + "try { f(s[0]); } catch (e) { msg = e; } msg;").as<std::string>();
				check_true(msg.find("Cannot pass non-lvalue to reference parameter") != std::string::npos,
					std::string("string subscript arg is a char rvalue: non-lvalue ref-bind error, got: ") + msg);
			}
		});

		test("tier2_general_lvalue_ref_args", [this]() {
			// A reference VALUE is an lvalue - ref params bind
			// the evaluated argument's owner-pinned reference when the structural kernel
			// can't classify the shape (map keys, computed indices, member-expr indices,
			// typed-array elements). Index side effects run exactly once, in arg order.
			const char* src = R"(
				class O { int idx = 1; }
				var o = O();
				var m = {"k": 1};
				var k = "k";
				var grid = [[1, 2], [3, 4]];
				var arr = [10, 20, 30];
				array<int> ta = [5, 6, 7];
				int y = 0;
				var calls = 0;
				function bump() -> int { calls = calls + 1; return 2; }
				function f(int& x) { x = x + 100; }
				f(m["k"]);
				f(m[k]);
				f(grid[y + 1][0]);
				f(arr[o.idx]);
				f(ta[y + 2]);
				f(arr[bump()]);
				to_string(m["k"]) + "|" + to_string(grid[1][0]) + "|" + to_string(arr[1]) + "|" +
					to_string(ta[2]) + "|" + to_string(arr[2]) + "|" + to_string(calls);
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				auto got = e->execute(src).as<std::string>();
				check_eq(std::string("201|103|120|107|130|1"), got);
				if (!use_vm) { first = got; } else { check_eq(first, got, "identical on both backends"); }
			}
		});

		test("tier2_missing_key_and_residue_rejections", [this]() {
			// Map reads never insert: binding a ref to a MISSING key raises the read's
			// error (as a var& decl over a missing key does). And
			// member-FINAL chains over computed subscripts stay rejected - field reads
			// evaluate to copies and the structural kernel can't take computed indices;
			// bind a var& row first (documented residue).
			const char* src = R"(
				class O { int v = 1; }
				var m = {"k": 1};
				var objs = [O(), O()];
				int i = 0;
				var msg1 = "";
				var msg2 = "";
				function f(int& x) { x = 0; }
				try { f(m["missing"]); } catch (e) { msg1 = e; }
				try { f(objs[i + 1].v); } catch (e) { msg2 = e; }
				msg1 + "&" + msg2;
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				auto got = e->execute(src).as<std::string>();
				check_true(got.find("Key \"missing\" not found in map&") == 0,
					"missing map key rejects: " + got);
				check_true(got.rfind("Cannot pass non-lvalue") > got.find("&"),
					"member-final over computed subscript rejects: " + got);
				if (!use_vm) { first = got; } else { check_eq(first, got, "identical on both backends"); }
			}
		});

		test("ref_decl_member_final", [this]() {
			// Route independence: var&/auto& declarations bind
			// member-final chains through the same kernel ref params use (the
			// GLOOM `auto& p = G.player` encounter). Computed properties keep
			// the decl's non-lvalue error.
			const char* src = R"(
				class Player { int hp = 100; }
				class World { var player = Player(); }
				class O { int v = 1; }
				class B { int _get_area() { return 4; } }
				var G = World();
				auto& p = G.player;
				p.hp = 55;
				var objs = [O(), O()];
				var& r = objs[1].v;
				r = 42;
				var b = B();
				var msg = "";
				try { auto& a = b.area; } catch (e) { msg = e; }
				to_string(G.player.hp) + "|" + to_string(objs[1].v) + "|" + msg;
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				auto got = e->execute(src).as<std::string>();
				check_true(got.find("55|42|") == 0, got);
				check_true(got.find("Cannot take reference of non-lvalue") != std::string::npos, got);
				if (!use_vm) { first = got; } else { check_eq(first, got, "identical on both backends"); }
			}
		});

		test("ref_decl_member_final_typed_enforcement", [this]() {
			// A decl-bound field ref carries the DECLARED field type: wrong-type stores
			// error with the same field text as ref params (route-independence)
			const char* src = R"(
				class O { int v = 1; }
				var o = O();
				var& r = o.v;
				r = 9;
				var msg = "";
				try { r = "nope"; } catch (e) { msg = e; }
				to_string(o.v) + "|" + msg;
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				auto got = e->execute(src).as<std::string>();
				check_true(got.find("9|") == 0, got);
				check_true(got.find("Cannot assign string to field 'v' of O (declared int)") != std::string::npos, got);
				if (!use_vm) { first = got; } else { check_eq(first, got, "identical on both backends"); }
			}
		});

		test("tier1_hot_reload_field_ref", [this]() {
			// Field refs re-resolve by (pinned instance, field id): a kept field stays
			// writable across a mid-call reload; a removed field errors on write
			const char* src = R"(
				class Box { int v = 1; }
				var b = Box();
				var msg = "";
				function keepWrite(var& x) { reloadKeep(); x = 42; }
				function dropWrite(var& x) { reloadDrop(); x = 43; }
				keepWrite(b.v);
				var kept = b.v;
				b2 = Box();
				try { dropWrite(b2.v); } catch (e) { msg = e; }
				to_string(kept) + "|" + msg;
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				jai::engine* raw = e.get();
				e->add_function("reloadKeep", [raw]() -> jai::script_int {
					raw->execute("class Box { int v = 1; float extra = 0.0; }");
					return 0;
				});
				e->add_function("reloadDrop", [raw]() -> jai::script_int {
					raw->execute("class Box { float other = 0.0; }");
					return 0;
				});
				e->execute("var b2 = null;");
				auto got = e->execute(src).as<std::string>();
				check_true(got.find("42|") == 0, "kept field writable through the live ref after reload");
				check_true(got.find("Reference to a removed field") != std::string::npos,
					"removed field errors on write through the ref");
				if (!use_vm) { first = got; } else { check_eq(first, got, "identical error text on both backends"); }
			}
		});

		test("variadic_cpp_function", [this]() {
			auto e = vm_engine();
			e->add_variadic_function("sum_all", [eng = e.get()](const std::vector<script_value>& args) -> checked_result<script_value> {
				script_int total = 0;
				for (const auto& a : args) {
					total += a.as_int();
				}
				return script_value(total, eng);
			});
			check_eq((int64_t)6, e->execute("sum_all(1, 2, 3)").as_int());
			check_eq((int64_t)15, e->execute("sum_all(1, 2, 3, 4, 5)").as_int());
		});

		test("parameters_and_lambda_captures_run_on_the_vm", [this]() {
			const std::pair<const char*, std::vector<std::pair<const char*, int64_t>>> cases[] = {
				{"by_ref_params_mutate_caller", {{R"(
				function bump(int& target) { target = target + 10; }
				var value = 1;
				bump(value);
				value;
			)", 11}}},
				{"value_params_do_not_mutate_caller", {{R"(
				function tweak(int copy) { [[warn_mode]] copy = copy + 10; }
				var original = 1;
				tweak(original);
				original;
			)", 1}}},
				{"lambda_basic", {{R"(
				var double_it = [](int x) { return x * 2; };
				double_it(5);
			)", 10}}},
				{"lambda_capture_by_value", {{R"(
				function run() { var x = 1; var f = [=]() { return x; }; x = 2; return f(); }
				run();
			)", 1}}},
				{"lambda_capture_by_reference", {{R"(
				var c = 0;
				var inc = [&]() { c = c + 1; };
				inc();
				inc();
				inc();
				c;
			)", 3}}},
				{"lambda_explicit_captures", {{R"(
				var base = 10;
				var f = [base](int x) { return base + x; };
				f(5);
			)", 15}}},
				{"lambda_auto_capture", {{R"(
				var outer = 20;
				var f = [](int x) { return outer + x; };
				f(10);
			)", 30}}},
				{"lambda_closure_over_loop_vars", {{R"(
				var captured = null;
				for (var i = 0; i < 3; i = i + 1) {
					if (i == 0) {
						captured = [=]() { return i; };
					}
				}
				captured();
			)", 0}}},
				{"lambda_inside_function_captures_locals", {{R"(
				function make_getter() -> auto {
					var secret = 42;
					return [=]() { return secret; };
				}
				var g = make_getter();
				g();
			)", 42}}},
			};
			check_each(cases, [&](const auto& c) {
				auto e = vm_engine();
				for (const auto& [code, expected] : c.second) {
					check_eq(expected, e->execute(code).as_int(), code);
				}
			});
		});

		test("array_literal_and_index", [this]() {
			auto e = vm_engine();
			check_eq((int64_t)2, e->execute("var a = [1, 2, 3]; a[1]").as_int());
			check_eq((int64_t)13, e->execute("a[0] = 10; a[0] + a[2]").as_int());
			check_throws([&]() { e->execute("a[99]"); }, "out of bounds read throws");
			check_throws([&]() { e->execute("a[\"k\"]"); }, "non-integer array index throws");
		});

		test("map_literal_and_index", [this]() {
			auto e = vm_engine();
			check_eq((int64_t)1, e->execute("var m = {\"k\": 1, \"j\": 2}; m[\"k\"]").as_int());
			check_eq((int64_t)5, e->execute("m[\"k\"] = 5; m[\"k\"]").as_int());
			check_eq((int64_t)42, e->execute("m[\"new\"] = 42; m[\"new\"]").as_int());
			check_throws([&]() { e->execute("m[\"missing\"] == null"); }, "a missing key raises");
		});

		test("containers_classes_statics_and_enums_run_on_the_vm", [this]() {
			const std::pair<const char*, std::vector<std::pair<const char*, int64_t>>> cases[] = {
				{"nested_containers", {{R"(
				var grid = [[1, 2], [3, 4]];
				grid[1][1];
			)", 4}, {R"(
				grid[1][1] = 9;
				grid[1][1];
			)", 9}}},
				{"class_inheritance_and_super", {{R"(
				class Cat { int a = 0; Cat(int v) { a = v; } int get() { return a; } }
				class Tiger : Cat { Tiger() : super(5) {} int roar() { return a * 2; } }
				auto t = Tiger();
				t.roar()
			)", 10}, {"auto t2 = Tiger(); t2.get()", 5}}},
				{"ctor_delegation_field_defaults", {{R"(
				class D { int a = 1; D() : this(5) {} D(int v) { a = v; } }
				auto d = D();
				d.a
			)", 5}}},
				{"static_fields_and_methods", {{R"(
				class Counter {
					static int count = 0;
					static int next() { count = count + 1; return count; }
				}
				Counter::next();
				Counter::next()
			)", 2}, {"Counter::count", 2}}},
				{"enum_and_namespace_dispatch", {{"enum Color { red, green, blue } Color.green", 1}, {R"(
				namespace vmmath { int add(int a, int b) { return a + b; } }
				vmmath::add(2, 3)
			)", 5}}},
			};
			check_each(cases, [&](const auto& c) {
				auto e = vm_engine();
				for (const auto& [code, expected] : c.second) {
					check_eq(expected, e->execute(code).as_int(), code);
				}
			});
		});

		test("budget_overrun_raises", [this]() {
			auto e = vm_engine();
			e->execution_budget(0.05);
			check_throws([&]() {
				e->execute("while (true) { }");
			}, "budget overrun raises an error");
			// Engine stays usable afterwards
			e->execution_budget(jai::foundry::k_test_budget_seconds);
			check_eq((int64_t)4, e->execute("2 + 2").as_int());
		});

		test("unsupported_constructs_error_cleanly", [this]() {
			auto e = vm_engine();
			check_eq((int64_t)7, e->execute("class Cat { int a = 7; } auto c = Cat(); c.a").as_int());
			check_eq((int64_t)1, e->execute("coroutine gen() { yield 1; } auto g = gen(); g.next();").as_int(), "bare coroutine spelling runs");
			check_throws([&]() { e->execute("include \"no_such_file.jai\";"); }, "include reports B4");
			// Engine stays usable
			check_eq((int64_t)4, e->execute("2 + 2").as_int());
		});

		test("throw_across_frames_caught_by_outer_try", [this]() {
			auto e = vm_engine();
			check_eq(std::string("deep boom"), e->execute(R"(
				auto inner() { throw "deep boom"; }
				auto middle() { return inner(); }
				var caught = "";
				try { middle(); } catch (err) { caught = err; }
				caught;
			)").as<std::string>());
			// Typed payloads stay typed across frames (the caught error's value)
			check_eq((int64_t)42, e->execute(R"(
				auto thrower() { throw 42; }
				var got = 0;
				try { thrower(); } catch (err) { got = err.value; }
				got;
			)").as_int());
			// Unwinding across intermediate frames leaves the engine consistent
			check_eq((int64_t)4, e->execute("2 + 2").as_int());
		});

		test("switch_fallthrough_chain", [this]() {
			auto e = vm_engine();
			check_eq(std::string("twothreedefault"), e->execute(R"(
				var r = "";
				switch (2) {
					case 1: r += "one"; fallthrough;
					case 2: r += "two"; fallthrough;
					case 3: r += "three"; fallthrough;
					default: r += "default";
				}
				r;
			)").as<std::string>());
			// Break-by-default: no fallthrough without the keyword
			check_eq(std::string("two"), e->execute(R"(
				var s = "";
				switch (2) { case 1: s = "one"; case 2: s = "two"; case 3: s = "three"; }
				s;
			)").as<std::string>());
		});

		test("differential_try_state_edges", [this]() {
			// Same scripts on both backends; results must agree exactly
			const char* catch_var_across_callee = R"(
				auto helper() { try { return 5; } catch (ignored) { return 0; } }
				var out = "";
				try { throw "orig"; } catch (err) { helper(); out = err; }
				out;
			)";
			const char* bare_rethrow_no_active = R"(
				auto f() { throw; }
				var msg = "";
				try { f(); } catch (err) { msg = err; }
				msg;
			)";
			const char* break_pops_handler = R"(
				var n = 0;
				for (var i = 0; i < 3; ++i) { try { n += 1; break; } catch (ignored) { n += 100; } }
				var hit = 0;
				try { throw "after"; } catch (err) { hit = 1; }
				to_string(n * 10 + hit);
			)";
			for (const char* src : {catch_var_across_callee, bare_rethrow_no_active, break_pops_handler}) {
				auto interp = interp_engine();
				jai::stdlib::register_all(interp);
				auto vm = vm_engine();
				jai::stdlib::register_all(vm);
				check_eq(interp->execute(src).as<std::string>(), vm->execute(src).as<std::string>());
			}
		});

		test("range_for_reference_mutation", [this]() {
			auto e = vm_engine();
			jai::stdlib::register_all(e);
			check_eq((int64_t)12, e->execute(R"(
				auto nums = [1, 2, 3];
				for (auto& x : nums) { x *= 2; }
				nums[0] + nums[1] + nums[2];
			)").as_int());
			check_eq((int64_t)60, e->execute(R"(
				auto scores = {"a": 10, "b": 20};
				for (auto& kv : scores) { kv.second *= 2; }
				scores["a"] + scores["b"];
			)").as_int());
		});

		test("custom_global_operator", [this]() {
			auto e = vm_engine();
			bool used = false;
			e->add_function("+", [&used](int a, int b) -> int {
				used = true;
				return (a + b) * 2;
			});
			detail::engine_access::set_has_custom_numeric_operators(*e, true);
			check_eq((int64_t)16, e->execute("var a = 5; var b = 3; a + b").as_int());
			check_true(used);
		});

		test("builtin_container_and_string_methods", [this]() {
			auto e = vm_engine();
			check_eq((int64_t)3, e->execute("auto arr = [1, 2]; arr.push(3); arr.size()").as_int());
			check_eq((int64_t)3, e->execute("arr.pop()").as_int());
			check_true(e->execute("auto m = {\"k\": 1}; m.contains(\"k\")").as<bool>());
			check_eq(std::string("cba"), e->execute("var s = \"abc\"; s.reverse(); s").as<std::string>());
			check_throws([&]() { e->execute("array<int> typed = [1]; typed.push(\"nope\");"); }, "typed push mismatch throws");
		});

		test("null_safe_member_and_method", [this]() {
			auto e = vm_engine();
			check_true(e->execute("var nothing = null; nothing?.field == null").as<bool>());
			check_true(e->execute("nothing?.method() == null").as<bool>());
		});

		test("disassembler_smoke", [this]() {
			auto e = vm_engine();
			auto* symbolizer = detail::engine_access::get_symbolizer(*e);
			std::string script = "var x = 1; while (x < 10) { x = x + 1; } x;";
			auto templates = detail::engine_access::get_registered_template_types(*e);
			jai::lexer lex(script, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, e.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");

			jai::vm::vm_compiler compiler(symbolizer);
			auto compiled = compiler.compile_program(parse_result.value());
			check_true(compiled != nullptr);

			std::string listing = jai::vm::disassemble(*compiled, symbolizer);
			check_true(listing.find("LOOP_BACK") != std::string::npos, "loop back edge present");
			check_true(listing.find("FUSED_CMP_JUMP") != std::string::npos, "fused conditional jump present");
			check_true(listing.find("DECL_VAR") != std::string::npos, "variable declaration present");
			check_true(listing.find("STORE") != std::string::npos, "store present");
			check_true(listing.find("IMPLICIT_RETURN") != std::string::npos, "implicit return present");
			check_true(listing.find("HALT") != std::string::npos, "halt present");
		});

		test("disassembler_return_convert_only_inside_try", [this]() {
			auto e = vm_engine();
			auto* symbolizer = detail::engine_access::get_symbolizer(*e);
			auto listing_of = [&](const char* script) {
				auto templates = detail::engine_access::get_registered_template_types(*e);
				jai::lexer lex(script, symbolizer);
				auto tokens = lex.tokenize();
				jai::parser p(tokens, symbolizer, e.get(), templates);
				auto parse_result = p.parse();
				check_true(parse_result.has_value(), "snippet parses");
				jai::vm::vm_compiler compiler(symbolizer);
				auto compiled = compiler.compile_program(parse_result.value());
				return jai::vm::disassemble(*compiled, symbolizer);
			};
			std::string clean = listing_of("var n = 1; return n;");
			check_true(clean.find("RETURN_IDENT") != std::string::npos, "clean-exit return keeps the fused form");
			check_true(clean.find("RETURN_CONVERT") == std::string::npos, "no site conversion outside a try/catch region");
			std::string in_try = listing_of("var n = 1; try { return n; } catch (e) { return 0; }");
			const size_t conv = in_try.find("RETURN_CONVERT");
			const size_t pop = in_try.find("TRY_POP");
			check_true(conv != std::string::npos && pop != std::string::npos && conv < pop,
				"an in-try return converts before the region exit");
		});

		test("differential_interpreter_vs_vm", [this]() {
			const char* scripts[] = {
				"var t = 0; for (var i = 1; i <= 10; i = i + 1) { t = t + i * i; } t;",
				"function fib(int n) -> int { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } fib(12);",
				"var s = \"\"; var i = 0; while (i < 4) { s = s + \"ab\"; i = i + 1; } s;",
				"var a = [5, 3, 8]; var mx = a[0]; for (var i = 1; i < 3; i = i + 1) { if (a[i] > mx) { mx = a[i]; } } mx;",
				"var m = {\"x\": 1}; m[\"y\"] = 2; m[\"x\"] + m[\"y\"];",
				"var acc = 0; var f = [&](int v) { acc += v; }; f(3); f(4); acc;",
				"(5 <=> 2) + (2 <=> 5) + (3 <=> 3);",
				"var n = 6; var r = n > 5 ? \"big\" : \"small\"; r;",
			};
			for (const char* script : scripts) {
				auto interp = interp_engine();
				auto vm = vm_engine();
				auto lhs = interp->execute(script);
				auto rhs = vm->execute(script);
				check_eq(lhs.to_string(), rhs.to_string(), std::string("differential: ") + script);
			}
		});

		// A yield fired inside a range-for AND a try must resume mid-construct: the fiber
		// snapshot has to preserve the loop's iter-state and the try handler record.
		test("coroutine_yield_inside_range_for_and_try", [this]() {
			auto vm = vm_engine();
			jai::stdlib::register_all(vm);
			auto rhs = vm->execute(R"(
				coroutine int gen() {
					auto data = [10, 20, 30];
					for (auto x : data) {
						try {
							yield x;
						} catch (e) {
							yield -1;
						}
					}
					return 7;
				}
				auto c = gen();
				auto sum = 0;
				while (!c.done()) { sum = sum + c.next(); }
				sum
			)");
			check_eq((int64_t)67, rhs.as_int());
		});

		// yield deep inside nested C-style loops resumes at the exact point (no restart).
		test("coroutine_yield_inside_nested_for", [this]() {
			auto vm = vm_engine();
			jai::stdlib::register_all(vm);
			auto rhs = vm->execute(R"(
				coroutine int gen() {
					for (int i = 0; i < 2; ++i) {
						for (int j = 0; j < 3; ++j) {
							yield i * 10 + j;
						}
					}
					return 99;
				}
				auto c = gen();
				auto sum = 0;
				while (!c.done()) { sum = sum + c.next(); }
				sum
			)");
			check_eq((int64_t)135, rhs.as_int());
		});

		// Abandoning a suspended fiber (dropped at block-scope exit) must destruct cleanly;
		// the Debug env-cycle checker fires on a leaked closure/env cycle.
		test("abandoned_suspended_fiber_destructs_cleanly", [this]() {
			auto e = vm_engine();
			jai::stdlib::register_all(e);
			e->execute(R"(
				coroutine int forever() {
					int i = 0;
					while (true) { yield i; i = i + 1; }
				}
				{
					auto c = forever();
					c.next();
					c.next();
				}
			)");
			check_eq((int64_t)42, e->execute("21 * 2").as_int());
		});

		// A host callback that calls execute() mid-resume must not sever the running fiber's
		// yield machinery; the next yield still reaches the caller (regression #40 analog).
		test("nested_execute_during_resume", [this]() {
			auto e = vm_engine();
			jai::stdlib::register_all(e);
			engine* raw = e.get();
			e->add_function("nested_exec", [raw]() -> script_int {
				raw->execute("1 + 1");
				return 0;
			});
			auto r = e->execute(R"(
				coroutine int co() {
					yield 1;
					nested_exec();
					yield 3;
					return 0;
				}
				auto c = co();
				auto a = c.next();
				auto b = c.next();
				a * 10 + b
			)");
			check_eq((int64_t)13, r.as_int());
		});

		// include inside a function body: the VM must observe the same result as the
		// interpreter (re-entrant execute defines into the global environment).
		test("include_inside_function_differential", [this]() {
			auto temp_dir = jai::foundry::test_temp_directory();
			auto file_path = temp_dir / "vm_include_in_fn.jai";
			{
				std::ofstream f(file_path);
				f << "auto vm_included_flag = 77;";
			}
			// An include is textual, so an include inside a function body scopes with the function: the included
			// global is visible inside loader() and gone after it, identically on both backends
			const char* script = R"(
				function loader() { include "vm_include_in_fn.jai"; return vm_included_flag; }
				var seen = loader();
				var after = 0;
				[[warn_mode]] try { after = vm_included_flag; } catch (e) { after = -1; }
				seen * 100 + after
			)";
			auto interp = interp_engine();
			interp->add_include_path(temp_dir.string());
			auto vm = vm_engine();
			vm->add_include_path(temp_dir.string());
			auto lhs = interp->execute(script);
			auto rhs = vm->execute(script);
			check_eq(lhs.to_string(), rhs.to_string(), "include-inside-function differential");
			check_eq((int64_t)7699, rhs.as_int(), "the included global lives in the function and is gone after it");
			std::filesystem::remove(file_path);
		});

		// Tier-1 reference args x coroutines x deep recursion: dual-backend differentials
		// (parity is checked as interp == vm; absolute values pin sanctioned semantics)

		// warn_region: the program opens a [[warn_mode]] region on its first line (the checker reports, the load proceeds)
		auto run_both_backends_in = [](bool warn_region, const char* src, size_t interpreter_depth = 0) {
			std::string out[2];
			int idx = 0;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				if (interpreter_depth != 0) { e->max_call_depth(jai::backend_type::interpreter, interpreter_depth); }
				try { out[idx] = e->execute(std::string(warn_region ? "[[warn_mode]]; " : "") + src).to_string(); }
				catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
				++idx;
			}
			return std::make_pair(out[0], out[1]);
		};
		auto run_both_backends = [run_both_backends_in](const char* src, size_t interpreter_depth = 0) {
			return run_both_backends_in(false, src, interpreter_depth);
		};

		test("tier1_references_survive_coroutine_resumes", [this, run_both_backends]() {
			const std::tuple<const char*, const char*, const char*> cases[] = {
				{"tier1_field_ref_call_inside_coroutine_body", R"(
				class Box { int v = 0; }
				function bump(int& x) { x = x + 1; }
				coroutine int gen(var b) {
					bump(b.v);
					yield b.v;
					bump(b.v);
					yield b.v;
					bump(b.v);
					return b.v;
				}
				var box = Box();
				auto g = gen(box);
				auto r1 = g.next();
				auto r2 = g.next();
				auto r3 = g.next();
				r1 * 1000 + r2 * 100 + r3 * 10 + box.v;
			)", "1230"},
				{"tier1_global_refs_across_resumes_with_mutation", R"(
				class Box { int v = 10; }
				var box = Box();
				var arr = [1, 2, 3];
				function bump(int& x) { x = x + 100; }
				coroutine int gen() {
					bump(arr[1]);
					yield arr[1];
					bump(box.v);
					yield box.v;
					return arr[1] + box.v;
				}
				auto g = gen();
				auto r1 = g.next();
				arr.push(4); arr.push(5); arr.push(6); arr.push(7);
				box.v = 50;
				auto r2 = g.next();
				auto r3 = g.next();
				r1 * 100000 + r2 * 100 + r3;
			)", "10215252"},
				// resume() during argument evaluation of an enclosing call: the enclosing
				// call's arg metadata must NOT leak into the coroutine's first-resume bind
				{"stale_metadata_first_resume_lvalue", R"(
				class Box { int v = 7; }
				var box = Box();
				coroutine int gen(int x) { x = 99; yield x; return 2; }
				function take(var a, var b) -> int { return a + b; }
				auto g = gen(555);
				take(box.v, g.next());
				box.v;
			)", "7"},
				{"stale_metadata_first_resume_identifier", R"(
				var y = 7;
				coroutine int gen(int x) { x = 99; yield x; return 2; }
				function take(var a, var b) -> int { return a + b; }
				auto g = gen(555);
				take(y, g.next());
				y;
			)", "7"},
				{"tier1_ref_call_in_loop_yield_replay", R"(
				class Box { int v = 0; }
				var box = Box();
				function bump(int& x) { x = x + 1; }
				coroutine int gen() {
					for (int i = 0; i < 3; ++i) {
						bump(box.v);
						yield box.v;
					}
					return 0;
				}
				auto g = gen();
				auto a = g.next();
				auto b = g.next();
				auto c = g.next();
				a * 1000 + b * 100 + c * 10 + box.v;
			)", "1233"},
			};
			check_each(cases, [&](const auto& c) {
				auto [i, v] = run_both_backends(std::get<1>(c));
				check_eq(std::string(std::get<2>(c)), i, "interpreter");
				check_eq(i, v, "vm against the interpreter");
			});
		});

		test("tier1_field_references_bind_subtypes_and_deep_chains", [this, run_both_backends]() {
			const std::tuple<const char*, const char*, const char*> cases[] = {
				// Worst-case (right-spine) ref-BST: 200 sequential inserts = ~20k calls with a
				// Tier-1 lvalue arg per hop; superlinear per-call cost turns this into minutes
				{"deep_tier1_bst_sequential", R"(
				class TreeNode { int value = 0; shared_ptr<TreeNode> left = null; shared_ptr<TreeNode> right = null; TreeNode(int v) { value = v; } }
				function insertNode(var& node, int val) {
					if (node == null) { node = TreeNode(val); return; }
					if (val < node.value) { insertNode(node.left, val); } else { insertNode(node.right, val); }
				}
				var root = null;
				for (int i = 1; i <= 200; ++i) { insertNode(root, i); }
				var cur = root;
				var count = 0;
				var last = 0;
				while (cur != null) { count = count + 1; last = cur.value; cur = cur.right; }
				count * 10000 + last;
			)", "2000200"},
				// Derived-to-base through a Tier-1 field ref: the constraint accepts subtypes,
				// like typed parameter conversion and every direct field-write path do
				{"tier1_field_ref_accepts_subtype", R"(
				class Animal { int id = 0; }
				class Dog : Animal { int extra = 1; }
				class Holder { Animal pet = Animal(); }
				function put(var& x, var val) { x = val; }
				var h = Holder();
				var d = Dog();
				d.id = 4;
				shared_ptr<Animal> kept = h.pet;
				put(h.pet, d);
				same_object(kept, h.pet) ? h.pet.id * 10 + h.pet.extra : -1;
			)", "41"},
				// A var local's tag is 'any'; the constraint check resolves the actual instance
				// class instead of rejecting on the declaration tag
				{"tier1_field_ref_accepts_var_held_instance", R"(
				class Node { int v = 1; }
				class Holder { Node n = Node(); }
				function put(var& x, var val) { x = val; }
				var h = Holder();
				var src = Node();
				src.v = 42;
				put(h.n, src);
				var sp = shared_ptr<Node>();
				sp.v = 7;
				var h2 = Holder();
				put(h2.n, sp);
				h.n.v * 100 + h2.n.v;
			)", "4207"},
			};
			check_each(cases, [&](const auto& c) {
				auto [i, v] = run_both_backends(std::get<1>(c));
				check_eq(std::string(std::get<2>(c)), i, "interpreter");
				check_eq(i, v, "vm against the interpreter");
			});
		});

		test("deep_tier2_passthrough_800", [this, run_both_backends]() {
			const char* src = R"(
				class Box { int v = 0; }
				function deep(int& x, int d) { if (d == 0) { x = 42; return; } deep(x, d - 1); }
				var b = Box();
				deep(b.v, 800);
				b.v;
			)";
			// This tests reference lifetime at depth, not the safer default cap.
			auto [i_out, v_out] = run_both_backends(src, 1000);
			check_eq(std::string("42"), i_out, "interp: 800-hop tier2 ref pass-through");
			check_eq(std::string("42"), v_out, "vm: 800-hop tier2 ref pass-through");
		});

		test("yield_in_nested_ref_param_call", [this, run_both_backends]() {
			const char* src = R"(
				class Box { int v = 1; }
				var box = Box();
				function pumpYield(int& x) { x = x + 1; yield x; x = x + 1; }
				coroutine int gen() { pumpYield(box.v); return box.v; }
				auto g = gen();
				auto r1 = g.next();
				auto r2 = g.next();
				r1 * 100 + r2 * 10 + box.v;
			)";
			auto [i_out, v_out] = run_both_backends(src);
			check_eq(i_out, v_out, "yield below a ref-param frame must agree across backends");
		});

		// The generic-binary custom-operator consult used to walk the caller env chain,
		// which grows with recursion depth for eager-env callees - per-call cost became
		// O(depth) and the ref-BST superlinear. Scaling ratio, not absolute time: 8x the
		// work must stay near 8x the time (the walk made it ~250x).
		test("recursive_generic_binary_cost_stays_linear", [this]() {
			// a generic binary on a reference deep in a recursion consults the custom operators without walking the caller
			// chain: environment lookup steps per call stay flat as the depth grows (a deterministic count, Debug builds)
			auto descent_hops = [this](bool use_vm, int depth) {
				auto e = jai::foundry::make_default_engine();
				e->max_call_depth(jai::backend_type::interpreter, 1000);
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				e->execute(
					"class Box { int v = 0; }\nvar b = Box();\n"
					"function f(var& x, int d) { if (d < -1) { return; } if (d == 0) { x = x + 1; return; } f(b.v, d - 1); }");
				std::string run = "b.v = 0; for (int rep = 0; rep < 10; ++rep) { f(b.v, " + std::to_string(depth) + "); } b.v;";
				const uint64_t before = detail::engine_access::get_symbolizer(*e)->env_hops();
				auto result = e->execute(run);
				const uint64_t hops = detail::engine_access::get_symbolizer(*e)->env_hops() - before;
				check_eq((int64_t)10, result.as_int(), "descent workload result");
				return hops;
			};
			for (bool use_vm : {false, true}) {
				const uint64_t small = descent_hops(use_vm, 50), big = descent_hops(use_vm, 400);
				const uint64_t small_calls = 10 * 51, big_calls = 10 * 401;
				// per call: the deep run may take at most twice the shallow run's steps, plus a constant
				check_le(big * small_calls, 2 * small * big_calls + 4 * small_calls * big_calls,
					std::string(use_vm ? "vm" : "interp") + ": lookup steps per call stay flat (" + std::to_string(small) + " at depth 50, " +
					std::to_string(big) + " at depth 400)");
			}
		});

		test("a_lookup_miss_deep_in_a_recursion_walks_the_chain_once", [this]() {
			// a name no environment holds, read at the bottom of a recursion (plain frames and frames with an environment of their
			// own): its lookup climbs the chain once, so the steps grow with the depth, never with its square
			auto miss_hops = [this](bool use_vm, const char* fn, int depth) {
				auto e = jai::foundry::make_default_engine();
				e->max_call_depth(jai::backend_type::interpreter, 1000);
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				e->execute(
					"[[warn_mode]]; function plain(int d) -> int { if (d == 0) { int s = 0; for (int k = 0; k < 10; ++k) { try { s += nope; } catch (e) { s += 1; } } return s; } return plain(d - 1); }\n"
					"function framed(int d) -> int { auto keep = [&]() -> int { return d; }; if (d == 0) { int s = 0; for (int k = 0; k < 10; ++k) { try { s += nope; } catch (e) { s += 1; } } return s + keep() * 0; } return framed(d - 1); }");
				const uint64_t before = detail::engine_access::get_symbolizer(*e)->env_hops();
				auto result = e->execute(std::string(fn) + "(" + std::to_string(depth) + ");");
				const uint64_t hops = detail::engine_access::get_symbolizer(*e)->env_hops() - before;
				check_eq((int64_t)10, result.as_int(), std::string(fn) + ": every miss raises and is caught");
				return hops;
			};
			for (bool use_vm : {false, true}) {
				for (const char* fn : {"plain", "framed"}) {
					const uint64_t small = miss_hops(use_vm, fn, 50), big = miss_hops(use_vm, fn, 400);
					// linear in the depth: the deep run takes at most twice the shallow run's steps per level, plus a constant
					check_le(big * 50, 2 * small * 400 + 400 * 50 * 50,
						std::string(use_vm ? "vm " : "interp ") + fn + ": miss steps stay linear in the depth (" + std::to_string(small) +
						" at depth 50, " + std::to_string(big) + " at depth 400)");
				}
			}
		});

		// Ref-store error catchability is uniform: a removed element/field surfaces as the
		// same caller-frame-catchable throw whether or not the bound container was typed;
		// constraint violations are checked errors, catchable in the throwing frame.
		test("ref_store_error_catchability_uniform", [this, run_both_backends]() {
			const char* plain_store = R"(
				array<int> arr = [1, 2, 3];
				function f(int& x) {
					while (arr.size() > 0) { arr.pop(); }
					try { x = 5; } catch (e) { return "inner:" + e; }
					return "no-error";
				}
				var out = "";
				try { out = f(arr[2]); } catch (e) { out = "outer:" + e; }
				out;
			)";
			auto [i_plain, v_plain] = run_both_backends(plain_store);
			// docs/site/guide/05-control-flow.html "try / catch / throw": the native exception is the running function's own, so its try catches it
			check_true(i_plain.find("inner:") == 0, "interp: plain ref store is caught by the function's own try");
			check_true(i_plain.find("removed array element") != std::string::npos, "interp: plain ref store error text");
			check_eq(i_plain, v_plain, "plain ref store catchability parity");

			const char* compound_store = R"(
				array<int> arr = [1, 2, 3];
				function f(int& x) {
					while (arr.size() > 0) { arr.pop(); }
					try { x += 1; } catch (e) { return "inner:" + e; }
					return "no-error";
				}
				var out = "";
				try { out = f(arr[2]); } catch (e) { out = "outer:" + e; }
				out;
			)";
			auto [i_comp, v_comp] = run_both_backends(compound_store);
			check_eq(i_plain, i_comp, "interp: constrained compound matches plain store catchability");
			check_eq(i_comp, v_comp, "constrained compound catchability parity");

			const char* type_error = R"(
				array<int> arr = [1, 2, 3];
				function f(int& x) {
					var text = "nope";
					try { x = text; } catch (e) { return "inner:" + e; }
					return "no-error";
				}
				var out = "";
				try { out = f(arr[1]); } catch (e) { out = "outer:" + e; }
				out;
			)";
			auto [i_type, v_type] = run_both_backends(type_error);
			check_true(i_type.find("inner:") == 0, "interp: constraint violations stay catchable in the throwing frame");
			check_true(i_type.find("Cannot assign 'string' to element of type 'int'") != std::string::npos,
				"interp: constraint violation error text");
			check_eq(i_type, v_type, "constraint violation catchability parity");
		});

		// Genuinely incompatible classes still error, naming the ACTUAL class (never 'any')
		test("tier1_field_ref_rejects_unrelated_class_with_actual_name", [this, run_both_backends]() {
			const char* src = R"(
				class Node { int v = 1; }
				class Stranger { int s = 2; }
				class Holder { Node n = Node(); }
				function put(var& x, var val) { x = val; }
				var h = Holder();
				var wrong = Stranger();
				var out = "";
				try { put(h.n, wrong); } catch (e) { out = `${e}`; }
				out;
			)";
			auto [i_out, v_out] = run_both_backends(src);
			check_true(i_out.find("Cannot assign Stranger to field 'n' of Holder (declared Node)") != std::string::npos,
				std::string("interp: rejection names the actual class, got: ") + i_out);
			check_eq(i_out, v_out, "rejection message parity");
		});

		// A script destructor firing at callee-frame pop used to segfault the interpreter:
		// call_stack_.pop_back() released the frame's locals mid-mutation and the dtor's
		// reentrant call_function grew the vector under it. Teardown now destroys the
		// frame's locals after the pop completes (VM stable-record parity).
		test("script_dtor_at_frame_pop_reentrancy", [this, run_both_backends]() {
			const char* plain = R"(
				class Box { int v = 0; ~Box() { } }
				function f() -> int { var b = Box(); return 1; }
				f();
			)";
			auto [i_plain, v_plain] = run_both_backends(plain);
			check_eq(std::string("1"), i_plain, "interp: local dtor at frame pop");
			check_eq(i_plain, v_plain, "plain dtor-at-pop parity");

			// Ref-pinned owner dropped mid-call: the holder keeps the instance alive, so
			// the dtor fires at callee-frame pop, after the write and before the caller
			// resumes - same ordering on both backends
			const char* pinned = R"(
				var log = "";
				class Box { int v = 5; ~Box() { log = log + "D"; } }
				var g = Box();
				function f(int& x) { g = null; log = log + "a"; x = 7; log = log + "b"; return x; }
				var r = f(g.v);
				log = log + "c";
				if (r == 7) { log = log + "!"; }
				log;
			)";
			auto [i_pin, v_pin] = run_both_backends(pinned);
			check_eq(std::string("abDc!"), i_pin, "interp: dtor fires at frame pop, ref write lands");
			check_eq(i_pin, v_pin, "pinned dtor ordering parity");
		});

		// dynamic_binder .property() members (stdlib pair, engine objects) have a
		// class-level field default but NO instance fields_ node: binding must keep the
		// exact non-lvalue error instead of lazily inserting a dead shadow field that
		// swallows writes
		test("tier1_map_pair_accessor_ref_args", [this]() {
			// Range-for map pairs expose .second through a C++ property whose value is a
			// live map-entry reference: the PROPERTY-mode ref writes
			// through the pair's setter into the real entry (the write
			// lands in the map). kv.first is the pair's own COPY of the key:
			// binding it writes that copy (by-reference binds properties), so
			// the map key - and the ordering invariant - stay untouched.
			const char* src = R"(
				var m = {"a": 1, "b": 2};
				function f(int& x) { x = 77; }
				function g(string& s) { s = "zz"; }
				var msg = "";
				for (var& kv : m) {
					f(kv.second);
					try { g(kv.first); msg = kv.first; } catch (e) { msg = `${e}`; }
					break;
				}
				to_string(m["a"]) + "|" + msg + "|" + to_string(m.has("zz")) + "|" + to_string(m.has("a"));
			)";
			std::string first;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				auto got = e->execute(src).as<std::string>();
				check_eq(std::string("77|zz|false|true"), got, "kv.second writes the entry, kv.first writes the pair's copy only");
				if (!use_vm) { first = got; } else { check_eq(first, got, "identical behavior on both backends"); }
			}
		});

		// shared_ptr-tagged field constraint: raw same-class/subtype instances are
		// accepted (every direct field-write path accepts them), and rejections render
		// the constraint in full instead of "Cannot assign 'S' to field of type 'S'"
		test("tier1_shared_ptr_field_constraint_accepts_same_class", [this, run_both_backends]() {
			const char* src = R"(
				class S { int x = 1; }
				class T : S { int t = 2; }
				class H { S p = S(); }
				function put(var& x, var val) { x = val; }
				var h1 = H(); h1.p = shared_ptr<S>();
				var s2 = S(); s2.x = 9;
				put(h1.p, s2);
				var h2 = H(); h2.p = shared_ptr<S>();
				put(h2.p, T());
				var h3 = H(); h3.p = shared_ptr<S>();
				var msg = "";
				try { put(h3.p, 5); } catch (e) { msg = `${e}`; }
				var summary = "";
				if (h1.p.x == 9) { summary = summary + "A"; }
				if (h2.p.t == 2) { summary = summary + "B"; }
				summary + "|" + msg;
			)";
			auto [i_out, v_out] = run_both_backends(src);
			check_true(i_out.find("AB|") == 0, std::string("interp: raw same-class and subtype accepted, got: ") + i_out);
			// the ref runs the field's table: the DECLARED type ('S p') names the slot
			check_true(i_out.find("Cannot assign int to field 'p' of H (declared S)") != std::string::npos,
				std::string("interp: rejection names the declared constraint, got: ") + i_out);
			check_eq(i_out, v_out, "shared_ptr field constraint parity");
		});

		test("tier1_auto_field_shared_ptr_tag_still_constrains", [this, run_both_backends]() {
			// An 'auto' field has no declared type, so the ref constraint falls back to the
			// bind-time value tag — a held shared_ptr<S> keeps the old full-name rejection
			const char* src = R"(
				class S { int x = 1; }
				class H { auto p = shared_ptr<S>(); }
				function put(var& x, var val) { x = val; }
				var h = H();
				var msg = "";
				try { put(h.p, 5); } catch (e) { msg = `${e}`; }
				msg;
			)";
			auto [i_out, v_out] = run_both_backends(src);
			check_true(i_out.find("Cannot assign int to field 'p' of H (declared shared_ptr<S>)") != std::string::npos,
				std::string("interp: auto field keeps the value-tag constraint, got: ") + i_out);
			check_eq(i_out, v_out, "auto-field shared_ptr constraint parity");
		});

		// Element refs and direct subscript assign are ONE gate: the same store into the
		// same typed array passes or fails with byte-identical text on both routes (both
		// run the element kernel; a var-held instance is its class, never its var tag)
		test("tier1_element_ref_gate_matches_direct_subscript", [this, run_both_backends]() {
			const char* src = R"(
				class Animal { int id = 0; }
				class Dog : Animal { int extra = 1; }
				class Box { int b = 0; }
				function put(var& x, var val) { x = val; }
				array<Animal> pen = [Animal()];
				array<int> ints = [1, 2, 3];
				var va = Animal();
				var vb = Box();
				var d1 = ""; var r1 = "";
				try { pen[0] = Dog(); } catch (e) { d1 = `${e}`; }
				try { put(pen[0], Dog()); } catch (e) { r1 = `${e}`; }
				var d2 = ""; var r2 = "";
				try { pen[0] = va; } catch (e) { d2 = `${e}`; }
				try { put(pen[0], va); } catch (e) { r2 = `${e}`; }
				var d3 = ""; var r3 = "";
				try { ints[0] = vb; } catch (e) { d3 = `${e}`; }
				try { put(ints[1], vb); } catch (e) { r3 = `${e}`; }
				d1 + "&" + r1 + "&" + d2 + "&" + r2 + "&" + d3 + "&" + r3;
			)";
			auto [i_out, v_out] = run_both_backends(src);
			auto split = [](const std::string& s) {
				std::vector<std::string> parts;
				size_t pos = 0;
				while (true) {
					size_t amp = s.find('&', pos);
					parts.push_back(amp == std::string::npos ? s.substr(pos) : s.substr(pos, amp - pos));
					if (amp == std::string::npos) { break; }
					pos = amp + 1;
				}
				return parts;
			};
			auto parts = split(i_out);
			check_eq((size_t)6, parts.size(), "six error captures");
			// A subclass is admitted into a class-typed element (no error)
			check_true(parts[0].empty(), std::string("direct gate admits a subtype, got: ") + parts[0]);
			check_eq(parts[0], parts[1], "subtype: ref matches direct");
			check_true(parts[2].empty(), std::string("direct gate admits a var-held instance of the class, got: ") + parts[2]);
			check_eq(parts[2], parts[3], "var-held: ref matches direct");
			check_true(parts[4].find("Cannot assign 'Box' to element of type 'int'") != std::string::npos,
				std::string("direct gate names the payload's class, never the var tag, got: ") + parts[4]);
			check_eq(parts[4], parts[5], "naming: ref matches direct");
			check_eq(i_out, v_out, "element gate parity");
		});

		auto run_both_backends_stdlib = [](const char* src) {
			std::string out[2];
			int idx = 0;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				stdlib::register_all(*e);
				try { out[idx] = e->execute(src).to_string(); }
				catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
				++idx;
			}
			return std::make_pair(out[0], out[1]);
		};

		// Tier 3(a): the primitive bind fast path must be invisible - tagging, retype
		// freedom, and caller isolation identical whether the gate takes (primitive
		// arg) or not (string arg takes the full conversion path)
		test("tier3_var_param_bind_semantics", [this, run_both_backends_stdlib]() {
			const char* src = R"(
				function probe(var x) -> string {
					var before = type_of(x);
					x = [1, 2];
					return `${before}>${type_of(x)}`;
				}
				function clobber(var x) { x = 999; }
				var i = 7; var f = 1.5; var b = true; var c = 'q'; var s = "str";
				var out = `${probe(i)}|${probe(f)}|${probe(b)}|${probe(c)}|${probe(s)}`;
				clobber(i); clobber(s);
				`${out}|${i}|${s}`;
			)";
			auto [i_out, v_out] = run_both_backends_stdlib(src);
			check_eq(std::string("int>array|float>array|bool>array|char>array|string>array|7|str"), i_out,
				"interp var-param bind semantics");
			check_eq(i_out, v_out, "var-param bind parity");
		});

		test("tier3_int_param_bind_semantics", [this, run_both_backends_stdlib]() {
			const char* src = R"(
				function take(int x) -> string {
					var t = type_of(x);
					x = x + 1;
					return `${t}:${x}`;
				}
				var a = 5;
				var r1 = take(a);
				var r2 = take(true);
				var f = 2.9;
				var r3 = "";
				try { take(f); } catch (e) { r3 = e; }
				`${r1}|${r2}|${r3}|${a}`;
			)";
			auto [i_out, v_out] = run_both_backends_stdlib(src);
			check_eq(i_out, v_out, "int-param bind parity");
			check_eq(std::string("int:6|int:2|Cannot pass float to parameter 'x' of 'take' (declared int)|5"), i_out, "an int binds unconverted, a bool converts, a float is refused, the caller's local is isolated");
		});

		// Tier 3(b): the exact-class-match conversion skip must keep value semantics -
		// exact match still deep-clones; subclass args keep the full path.
		// `new` sugar: a 'var' decl keeps a shared_ptr-tagged
		// initializer's marker (var sp = shared_ptr<P>() ≡ shared_ptr<P> sp = P()), so
		// sp's arg tag forces sharing through the exact-class param (14,14) — matching
		// what the decl-typed spelling does.
		test("tier3_exact_class_param_still_clones", [this, run_both_backends]() {
			const char* src = R"(
				class P { int v = 1; }
				class Q : P { int w = 2; }
				function bump(P p) -> int { p.v = p.v + 10; return p.v; }
				var a = P(); a.v = 5;
				var r1 = bump(a);
				var q = Q(); q.v = 3;
				var r2 = bump(q);
				var sp = shared_ptr<P>(); sp.v = 4;
				var r3 = bump(sp);
				function share(shared_ptr<P> p) { p.v = p.v + 100; }
				auto sh = shared_ptr<P>(); sh.v = 7;
				share(sh);
				`${r1},${a.v}|${r2},${q.v}|${r3},${sp.v}|${sh.v}`;
			)";
			auto [i_out, v_out] = run_both_backends(src);
			check_eq(std::string("15,5|13,3|14,14|107"), i_out, "interp exact-class clone semantics");
			check_eq(i_out, v_out, "exact-class clone parity");
		});

		// Tier 3(c): a throw during callee setup (default-arg eval / param conversion)
		// restores the caller environment - script keeps running and sees caller locals.
		// The literal NC-into-int argument is a strict load error, so the strict probe
		// sources the object from a var and the literal form runs in a [[warn_mode]] region.
		test("tier3_env_restored_after_throwing_bind", [this, run_both_backends, run_both_backends_in]() {
			const char* src = R"(
				class NC { int z = 0; }
				function boom(int n, int d = 1 / 0) -> int { return n + d; }
				function wants(int x) -> int { return x; }
				var keep = 40;
				var msg = "";
				var nc = NC();
				try { boom(1); } catch (e) { msg = "caught"; }
				keep = keep + 1;
				try { wants(nc); } catch (e) { msg = `${msg}+caught2`; }
				keep = keep + 1;
				`${msg}:${keep}`;
			)";
			auto [i_out, v_out] = run_both_backends(src);
			check_eq(std::string("caught+caught2:42"), i_out, "interp env restored after throwing bind");
			check_eq(i_out, v_out, "throwing-bind parity");
			const char* literal = R"(
				class NC { int z = 0; }
				function boom(int n, int d = 1 / 0) -> int { return n + d; }
				function wants(int x) -> int { return x; }
				var keep = 40;
				var msg = "";
				try { boom(1); } catch (e) { msg = "caught"; }
				keep = keep + 1;
				try { wants(NC()); } catch (e) { msg = `${msg}+caught2`; }
				keep = keep + 1;
				`${msg}:${keep}`;
			)";
			auto [w_out, wv_out] = run_both_backends_in(true, literal);
			check_eq(std::string("caught+caught2:42"), w_out, "warn: literal mismatch throws at bind and restores");
			check_eq(w_out, wv_out, "warn throwing-bind parity");
			auto [s_out, sv_out] = run_both_backends(literal);
			check_eq(s_out, sv_out, "strict listing parity");
			check_true(s_out.find("ERROR: static check failed") == 0 &&
				s_out.find("argument 1 of 'wants': cannot pass 'NC' to 'x' declared 'int'") != std::string::npos,
				"strict rejects the literal mismatch at load (got: " + s_out + ")");
		});

		// ---- compound fusion: bare identifier/constant rhs + ident+ident rhs ----
		// `sum += i` / `sum += 1` now compile to op_compound_fused (bare-rhs mode) and
		// `sum += i + j` fuses without the old one-constant-operand gate. These pin result
		// parity, the overflow raise point + text, and the ternary-tail no-fuse guard.

		test("compound_fused_bare_ident_results", [this, run_both_backends]() {
			auto [i1, v1] = run_both_backends(R"(
				auto sum = 0;
				for (auto i = 0; i < 1000; ++i) { sum += i; }
				sum;
			)");
			check_eq(std::string("499500"), i1, "interp bare-ident hot loop");
			check_eq(i1, v1, "bare-ident hot loop parity");
			auto [i2, v2] = run_both_backends(R"(
				function h() -> int {
					int sum = 0;
					for (int i = 0; i < 100; ++i) { sum += i; }
					return sum;
				}
				h();
			)");
			check_eq(std::string("4950"), i2, "interp bare-ident loop through frame slots");
			check_eq(i2, v2, "bare-ident slot parity");
			auto [i3, v3] = run_both_backends(R"(
				var a = 10; var b = 3; var log = "";
				a += b; log += `${a}`;
				a -= b; log += `,${a}`;
				a *= b; log += `,${a}`;
				a /= b; log += `,${a}`;
				a %= b; log += `,${a}`;
				log;
			)");
			check_eq(std::string("13,10,30,10,1"), i3, "interp all compound kinds, ident rhs");
			check_eq(i3, v3, "all-kinds ident rhs parity");
		});

		test("compound_fused_bare_literal_results", [this, run_both_backends]() {
			auto [i1, v1] = run_both_backends(R"(
				var a = 10; var f = 1.5; var s = "x";
				a += 5; a -= 2; a *= 3; a /= 2; a %= 7;
				f += 0.25;
				s += "y";
				`${a}|${f}|${s}`;
			)");
			check_eq(i1, v1, "bare literal rhs parity");
			check_true(i1.find("5|1.75") == 0 && i1.find("|xy") != std::string::npos,
				"bare literal rhs values (got: " + i1 + ")");
		});

		test("compound_assignment_fusion_matches_the_interpreter", [this, run_both_backends]() {
			const std::tuple<const char*, const char*, const char*> cases[] = {
				{"compound_fused_bare_result_value", R"(
				var a = 10; var b = 4;
				var r = (a += b);
				var r2 = (a += 1);
				`${r},${r2},${a}`;
			)", "14,15,15"},
				{"compound_fused_ident_ident_rhs", R"(
				auto sum = 0; auto j = 7;
				for (auto i = 0; i < 100; ++i) { sum += i + j; }
				sum;
			)", "5650"},
				// int target, var ident rhs that turns float: the int-only fast path must bail and the
				// pair's typed store-back refuses the float, the earlier sums kept
				{"compound_fused_bare_mixed_type_bails", R"(
				function h() -> string {
					int x = 0;
					var f = 2;
					string caught = "";
					try { for (int i = 0; i < 4; ++i) { x += f; if (i == 1) { f = 2.5; } } } catch (e) { caught = e; }
					return `${x}|${caught}`;
				}
				h();
			)", "4|Cannot assign float to 'x' (declared int)"},
				// current_catch_var_id_ bails the fast path; the pair's exec_load resolves the
				// catch variable from the active exception
				{"compound_fused_bare_catch_var_rhs", R"(
				var s = 10;
				try { throw 5; } catch (err) { s += err.value; }
				s;
			)", "15"},
				{"compound_fused_bare_ref_rhs", R"(
				var arr = [1, 2, 3];
				var& r = arr[1];
				var s = 10;
				s += r;
				s;
			)", "12"},
				// The peephole rewrites code.back() in place, so a ternary rhs/condition whose
				// else-arm leaves a fusable tail must NOT fuse: the then-arm's exit jump targets one
				// past that tail, and rewriting it made the then path skip the store/test entirely.
				{"ternary_tail_compound_not_fused", R"(
				var i = 3;
				var sum = 10; var c = true;
				sum += c ? 5 : i * 2;
				var sum2 = 10; var d = false;
				sum2 += d ? 5 : i * 2;
				var sum3 = 10;
				sum3 += c ? i : 7;
				var sum4 = 10;
				sum4 += c ? 5 : i;
				`${sum},${sum2},${sum3},${sum4}`;
			)", "15,16,13,15"},
				{"ternary_tail_condition_not_fused", R"(
				var i = 0; var c = true;
				while (c ? false : i < 2) { i += 100; c = false; }
				var j = 0;
				if (c ? false : j < 5) { j = 999; }
				`${i},${j}`;
			)", "0,0"},
			};
			check_each(cases, [&](const auto& c) {
				auto [i, v] = run_both_backends(std::get<1>(c));
				check_eq(std::string(std::get<2>(c)), i, "interpreter");
				check_eq(i, v, "vm against the interpreter");
			});
		});

		test("compound_fused_overflow_error_texts", [this]() {
			auto expect_both = [&](const char* src, const std::string& needle, const std::string& what) {
				std::string msg[2];
				int idx = 0;
				for (bool use_vm : {false, true}) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					try { e->execute(src); }
					catch (const std::exception& ex) { msg[idx] = ex.what(); }
					++idx;
				}
				check_eq(msg[0], msg[1], what + " error text is byte-identical across backends");
				check_true(msg[0].find(needle) != std::string::npos,
					what + " keeps the informative text (got: " + msg[0] + ")");
			};
			auto e = jai::foundry::make_default_engine();
			if (e->throw_on_overflow()) {
				expect_both("var a = 9223372036854775807; var b = 1; a += b;",
					"Integer overflow in '+='", "bare ident +=");
				expect_both("var a = 9223372036854775807; a += 1;",
					"Integer overflow in '+='", "bare literal +=");
				expect_both("var a = -9223372036854775807; var b = 2; a -= b;",
					"Integer overflow in '-='", "bare ident -=");
				expect_both("var a = 4611686018427387904; var b = 4; a *= b;",
					"Integer overflow in '*='", "bare ident *=");
				expect_both("var a = -9223372036854775807 - 1; var b = -1; a /= b;",
					"Integer overflow in '/='", "bare ident /=");
				expect_both("var a = 9223372036854775807; var b = 1; var c = 1; a += b + c;",
					"Integer overflow in '+='", "ident+ident rhs +=");
			}
			// zero divisors surface the pair's codes/messages regardless of policy
			expect_both("var a = 10; var b = 0; a /= b;", "ivision by zero", "bare ident /= 0");
			expect_both("var a = 10; var b = 0; a %= b;", "odulo by zero", "bare ident %= 0");
		});

		test("compound_fused_overflow_same_iteration", [this]() {
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				if (!e->throw_on_overflow()) { return; }
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				bool threw = false;
				try {
					e->execute(
						"var sum = 9223372036854775800; var n = -1;"
						"for (var i = 0; i < 100; ++i) { n = i; sum += i; }");
				} catch (const std::exception&) { threw = true; }
				check_true(threw, use_vm ? "vm raises" : "interp raises");
				// 800+0+1+2+3 = 806; += 4 overflows, so the raise is at i == 4 with sum intact
				check_eq(static_cast<int64_t>(4), e->get_variable("n").as_int());
				check_eq(static_cast<int64_t>(9223372036854775806LL), e->get_variable("sum").as_int());
			}
		});

		// === Callee inline cache + return classification pins ===

		test("stage1_callee_cache_revalidates_on_redefinition", [this, run_both_backends]() {
			// The same call site runs the old body, then the redefined body (with a
			// DIFFERENT return type: the per-function conversion class must re-derive
			// on the new function object, and the site's cached callee must miss)
			// The redefinition is a separate execute, a hot reload (one program text defining a signature twice is an error)
			std::string out[2];
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				try {
					e->execute("function f(auto n) -> int { return n + 1; }\nvar call_f(var x) { return f(x); }\nvar a = 0;\nfor (var i = 0; i < 3; i += 1) { a = call_f(a); }");
					e->execute("function f(auto n) -> string { return `s${n}`; }");
					out[use_vm ? 1 : 0] = e->execute("var b = call_f(a);\n`${a}:${b}`;").to_string();
				} catch (const std::exception& ex) { out[use_vm ? 1 : 0] = std::string("ERROR: ") + ex.what(); }
			}
			const std::string& i1 = out[0];
			const std::string& v1 = out[1];
			check_eq(std::string("3:s3"), i1, "interp redefinition at a warm call site");
			check_eq(i1, v1, "redefinition parity (vm cache revalidates)");
		});

		test("stage1_callee_caches_keep_lifetimes", [this, run_both_backends]() {
			const std::tuple<const char*, const char*, const char*> cases[] = {
				// One call site alternating between two function values: the monomorphic
				// cache must revalidate per callee, never call the wrong body
				{"stage1_callee_cache_polymorphic_site", R"(
				function inc(auto x) -> auto { return x + 1; }
				function dbl(auto x) -> auto { return x * 2; }
				var h = inc;
				var s = "";
				for (var i = 0; i < 4; i += 1) {
					s += `${h(3)},`;
					h = (i % 2 == 0) ? dbl : inc;
				}
				s;
			)", "4,6,4,6,"},
				// A DEAD lambda's captures keep
				// nothing: a freed pool slot drops its parent chain (the capture env)
				// at release on both backends, so both report expiry, and the vm's callee
				// cache must never re-add a pin (closures are excluded from it; the
				// capture-env leak class is also guarded by C++ alive counts in
				// Comprehensive Scope Tests::mixed_scopes_comprehensive).
				{"stage1_closure_callee_capture_dies_with_scope", R"(
				class T { int x = 1; }
				shared_ptr<T> a = T();
				var w = weak_ptr<T>(a);
				function run(auto p) -> auto {
					var l = [p]() { return p.x; };
					var s = 0;
					for (var i = 0; i < 3; i += 1) { s += l(); }
					return s;
				}
				var s = run(a);
				a = null;
				`${s}:${w.expired()}`;
			)", "3:true"},
				// Positive twin: a LIVE lambda's by-value shared_ptr capture keeps
				// the object alive after every other alias dies; dropping the lambda
				// releases it.
				{"stage1_live_lambda_capture_keeps_object_alive", R"(
				class T { int x = 1; }
				shared_ptr<T> a = T();
				var w = weak_ptr<T>(a);
				function mk(auto p) -> auto { return [p]() { return p.x; }; }
				var l = mk(a);
				a = null;
				var alive_while_held = !w.expired();
				var r = l();
				l = null;
				`${r}:${alive_while_held}:${w.expired()}`;
			)", "1:true:true"},
				// Callee locals die AT return (stage-2 lifetime pin: frame windows must
				// destroy the frame's values at the same boundary pop_script_frame_core
				// does today)
				{"stage1_weak_expiry_at_return_boundary", R"(
				class T { int x = 1; }
				var w = null;
				function make() -> auto {
					shared_ptr<T> p = T();
					w = weak_ptr<T>(p);
					return 7;
				}
				var r = make();
				`${r}:${w.expired()}`;
			)", "7:true"},
				// No stray argument copy (stack slice, pooled vector, cache pin) may keep
				// the object alive once the caller's own alias is gone (stage-2 pin: the
				// in-place-args design must keep this exact boundary)
				{"stage1_arg_copies_do_not_outlive_call", R"(
				class T { int x = 5; }
				function take(auto p) -> auto { return p.x; }
				shared_ptr<T> a = T();
				var w = weak_ptr<T>(a);
				var r = 0;
				for (var i = 0; i < 3; i += 1) { r += take(a); }
				var before = w.expired();
				a = null;
				`${r}:${before}:${w.expired()}`;
			)", "15:false:true"},
			};
			check_each(cases, [&](const auto& c) {
				auto [i, v] = run_both_backends(std::get<1>(c));
				check_eq(std::string(std::get<2>(c)), i, "interpreter");
				check_eq(i, v, "vm against the interpreter");
			});
		});

		test("stage1_typed_return_classification_routes", [this, run_both_backends]() {
			// Every return_conv class: matching primitive pass-through keeps value AND
			// type tagging (the typed stores below enforce it); mismatched primitive
			// converts (bool->int); ref params flatten on value returns;
			// auto stays deref-only
			auto [i1, v1] = run_both_backends(R"(
				function fi() -> int { return 5; }
				function ff() -> float { return 1.5; }
				function fs() -> string { return "ok"; }
				function fb() -> bool { return true; }
				function fconv() -> int { return true; }
				function fauto(auto n) -> auto { return n; }
				function fref(int& r) -> int { return r; }
				var q = 9;
				int ti = fi();
				float tf = ff();
				string ts = fs();
				bool tb = fb();
				`${ti}:${tf}:${ts}:${tb}:${fconv()}:${fauto(4)}:${fref(q)}`;
			)");
			check_eq(i1, v1, "typed-return route parity");
			check(i1.rfind("5:", 0) == 0, "int return value: " + i1);
			check_eq(std::string("5:1.5:ok:true:1:4:9"), i1, "bool->int return converts");
		});

		test("stage1_typed_return_mismatch_error_parity", [this, run_both_backends]() {
			// A string returned from -> int must fail identically on both backends
			auto [i1, v1] = run_both_backends(R"(
				function bad() -> int { return "nope"; }
				bad();
			)");
			check_eq(i1, v1, "typed-return mismatch error parity (byte-identical text)");
		});

		// === ::name (global-qualified) across every specialized identifier consumer:
		// the qualifier must ride op_return_ident, op_ref_return_bind, op_decl_ref_ident,
		// the cast identifier arm, fused operands, callees and builtin-method receivers
		// exactly like op_load (the compiler dropped it on the RETURN_IDENT fusion) ===

		test("global_qualified_names_run_like_bare_names", [this, run_both_backends]() {
			const std::tuple<const char*, const char*, const char*> cases[] = {
				// `return ::hp;` fuses to op_return_ident; a shadowing field, a shadowing
				// local and captured shadows must all still answer the global
				{"global_qualified_return_ident_fusion", R"(
				int hp = 1000;
				class U {
					int hp = 5;
					int by_field() { return ::hp; }
					int by_local() { int hp = 7; return ::hp; }
					int bare() { return hp; }
				}
				auto u = U();
				int cap_copy() { int hp = 5; auto l = [hp]() { return ::hp; }; return l(); }
				int cap_ref() { int hp = 5; auto l = [&hp]() { return ::hp; }; return l(); }
				int free_fn() { int hp = 9; return ::hp; }
				`${u.by_field()}|${u.by_local()}|${u.bare()}|${cap_copy()}|${cap_ref()}|${free_fn()}`;
			)", "1000|1000|5|1000|1000|1000"},
				// ::name as a fused operand: return-binary, cmp-jump, decl init, compound,
				// equality, mixed with the bare (member) spelling, literal-left, both sides
				{"global_qualified_fused_operand_positions", R"(
				int hp = 1000;
				class P {
					int hp = 5;
					int plus() { return ::hp + 1; }
					int cmp() { if (::hp < 10) { return 1; } return 2; }
					int decl() { int x = ::hp; return x; }
					int compound() { int s = 1; s += ::hp; return s; }
					bool eq() { return ::hp == 1000; }
					int mixed() { return ::hp * 2 - hp; }
					int lit_left() { return 1 + ::hp; }
					int both() { return ::hp - ::hp; }
				}
				auto p = P();
				`${p.plus()}|${p.cmp()}|${p.decl()}|${p.compound()}|${p.eq()}|${p.mixed()}|${p.lit_left()}|${p.both()}`;
			)", "1001|2|1000|1001|true|1995|1001|0"},
				{"global_qualified_callee", R"(
				int f() { return 1; }
				int g(int n) { return n * 10; }
				class C {
					int f() { return 2; }
					int g(int n) { return -n; }
					int call_f() { return ::f(); }
					int call_g() { return ::g(4); }
					int held() { auto r = ::f(); return r + ::g(1); }
				}
				auto c = C();
				int f_local() { int f = 5; return ::f() + f; }
				`${c.call_f()}|${c.call_g()}|${c.held()}|${f_local()}`;
			)", "1|40|11|6"},
				// Ref decl, cast<T&>, and ref-return producers under env-resident shadows
				// (block locals, captures): every bind lands on the GLOBAL cell
				{"global_qualified_reference_binds", R"(
				int hp = 1000;
				string log = "";
				{ int hp = 5; auto& r = ::hp; r = r + 1; log = log + cast<string>(hp) + ":" + cast<string>(::hp); }
				int via_cap() { int hp = 5; auto l = [hp]() { auto& r = ::hp; r = r + 1; return hp; }; return l(); }
				log = `${log}|${via_cap()}:${hp}`;
				{ int hp = 5; auto& r = cast<int&>(::hp); r = r + 1; log = `${log}|${hp}:` + cast<string>(::hp); }
				int cast_cap() { int hp = 5; auto l = [hp]() { auto& r = cast<int&>(::hp); r = r + 1; return hp; }; return l(); }
				log = `${log}|${cast_cap()}:${hp}`;
				auto& by_ref_local() { int hp = 5; return ::hp; }
				auto& r1 = by_ref_local(); r1 = r1 + 1;
				int ref_cap() { int hp = 5; auto l = [&hp]() -> auto& { return ::hp; }; auto& r = l(); r = r + 1; return hp; }
				log = `${log}|${ref_cap()}:${hp}`;
				log;
			)", "5:1001|5:1002|5:1003|5:1004|5:1006"},
				// `::s.insert(...)`: the in-place string builtin mutates the GLOBAL string,
				// never a block-local, captured or field shadow
				{"global_qualified_string_builtin_receiver", R"(
				string s = "glob";
				string log = "";
				{ string s = "blk"; ::s.insert(0, "x"); log = s + ":" + ::s; }
				string cap() { string s = "loc"; auto l = [s]() { ::s.insert(0, "y"); return s; }; return l(); }
				log = log + "|" + cap() + ":" + s;
				class H { string s = "fld"; string m() { ::s.insert(0, "z"); return s; } }
				auto h = H();
				log = log + "|" + h.m() + ":" + s;
				log;
			)", "blk:xglob|loc:yxglob|fld:zyxglob"},
			};
			check_each(cases, [&](const auto& c) {
				auto [i, v] = run_both_backends(std::get<1>(c));
				check_eq(std::string(std::get<2>(c)), i, "interpreter");
				check_eq(i, v, "vm against the interpreter");
			});
		});

		test("call_entries_and_reference_bindings_match_the_interpreter", [this, run_both_backends_stdlib]() {
			const std::tuple<const char*, const char*, const char*> cases[] = {
				// a builtin's callback runs on one prepared frame per builtin call: captures, a [this] lambda, a named function, a
				// defaulted parameter, a nested builtin, a throw out of a comparator, a T& element, a destructor in the body
				{"builtin_callback_entry_parity", R"(
				class Tally {
					int hits = 0;
					int big(array<int> xs, int limit) { xs.filter([this, limit](int x) -> bool { hits += 1; return x > limit; }); return xs.size(); }
				}
				class Noisy { int id = 0; Noisy(int i) { id = i; } ~Noisy() { log = log + "~" + to_string(id); } }
				string log = "";
				int compares = 0;
				bool desc(int a, int b) { compares += 1; return a > b; }
				array<int> a = [5, 3, 9, 1, 7, 2, 8];
				a.sort(desc);
				log = log + to_string(a[0]) + to_string(a[6]) + ":" + to_string(compares);
				int seen = 0;
				a.sort([&](int x, int y) -> bool { seen += 1; return x < y; });
				log = log + "|" + to_string(a[0]) + ":" + to_string(seen);
				var t = Tally();
				log = log + "|" + to_string(t.big(a, 4)) + ":" + to_string(t.hits);
				int defaulted = 0;
				[4, 5].for_each([&](int x, int i = 7) { defaulted += i; });
				log = log + "|" + to_string(defaulted);
				array<array<int>> rows = [[3, 1, 2], [9, 8], [5]];
				rows.sort([](array<int> p, array<int> q) -> bool { p.sort([](int x, int y) -> bool { return x > y; }); return p.size() < q.size(); });
				log = log + "|" + to_string(rows[0][0]) + to_string(rows[2][0]);
				try { [3, 1, 2].sort([](int x, int y) -> bool { if (x == 2) { throw "cmp"; } return x < y; }); } catch (e) { log = log + "|" + e; }
				array<int> b = [1, 2, 3];
				b.for_each([](int& x) { x = x * 10; });
				log = log + "|" + to_string(b.reduce([](int s, int x) -> int { return s + x; }, 0));
				[1, 2].for_each([](int k) { Noisy n = Noisy(k); });
				log = log + "|" + to_string([1, 2, 3].find([](int x) -> bool { return x > 1; }));
				log;
			)", "91:13|1:9|4:7|1|53|cmp|60~1~2|2"},
				// string parameters, element/field/cell reference arguments into by-value slots, closures and nested functions, a
				// callee that writes its copies, a block-scope caller, a parallel body calling a plain function
				{"plain_frame_argument_rows_parity", R"(
				string log = "";
				int bump(int v) { v += 1; return v; }
				float half(float x) { return x / 2; }
				string tag(string a, var b, int n) { a += "!"; b += "?"; return a + b + to_string(n); }
				int pick(var a, var b) { return a > b ? 1 : 2; }
				int clamp_at(int v, int& hi) { if (v > hi) { hi = v; } return hi; }
				class Cell { int n = 4; float w = 2.5; string t = "f"; }
				array<int> xs = [10, 20, 30];
				array<float> fs = [1.5, 2.5];
				var vs = [7, "s", 2.0];
				var c = Cell();
				int total = 0;
				for (int i = 0; i < 3; ++i) { total += bump(xs[i]); }
				log = log + to_string(total) + ":" + to_string(xs[0]) + to_string(xs[2]) + "|" + to_string(half(fs[1])) + "," + to_string(half(c.w)) + "," + to_string(bump(c.n)) + "," + to_string(c.n);
				string s = "base";
				log = log + "|" + tag(s, s, 1) + "," + s + "," + tag(c.t, vs[1], vs[0]) + "," + c.t + "," + to_string(vs[0]) + vs[1];
				int x = 5; int& rx = x;
				log = log + "|" + to_string(bump(rx)) + "," + to_string(x) + "," + to_string(pick(vs[0], xs[0])) + "," + to_string(pick("b", "a"));
				int hi = 0;
				for (int i = 0; i < 3; ++i) { clamp_at(xs[i], hi); }
				log = log + "|" + to_string(hi);
				int k = 3;
				var add = [k](int v) { return v + k; };
				var acc = [&](int v) { total += v; return total; };
				function outer(int v) { int inner(int w) { return w * 2; } return inner(v) + 1; }
				for (int i = 0; i < 3; ++i) { add(i); acc(xs[i]); }
				log = log + "|" + to_string(add(xs[2])) + "," + to_string(total) + "," + to_string(outer(4));
				{ int blk = 2; log = log + "|" + to_string(bump(blk)) + "," + to_string(add(blk)); }
				var bad = "no"; try { bump(bad); } catch (e) { log = log + "|" + e; }
				array<int> ps = [1, 2, 3, 4, 5, 6, 7, 8];
				parallel_for (auto& p : ps) { p = bump(p) * 2; }
				log = log + "|" + to_string(ps[0]) + "," + to_string(ps[7]);
				log;
			)", "63:1030|1.25,1.25,5,4|base!base?1,base,f!s?7,f,7s|6,5,2,1|30|33,123,9|3,5|Cannot pass string to parameter 'v' of 'bump' (declared int)|4,18"},
				// `var& x = G` inside functions, loops, a lambda and a method: the reference names the global's cell every time, across
				// stores of other kinds, a block local of the same name and a method frame's own field
				{"reference_to_global_declaration_parity", R"(
				var G = 1;
				array<int> BUF = [0, 0, 0];
				string log = "";
				void bump() { for (int i = 0; i < 4; ++i) { var& x = G; x = x + 1; } }
				void fill() { for (int i = 0; i < 3; ++i) { auto& b = BUF; b[i] = b[i] + i * 10; } }
				function shadow() { var G = 100; var& x = G; x = x + 1; return G; }
				class H { var G = 7; int touch() { var& x = G; x = x + 1; return G; } }
				var lam = []() { var& x = G; x = x + 3; return x; };
				bump(); fill(); fill();
				log = log + to_string(G) + "," + to_string(BUF[0]) + to_string(BUF[1]) + to_string(BUF[2]);
				G = 2.5; bump();
				log = log + "|" + to_string(G) + "," + to_string(lam()) + "," + to_string(shadow()) + "," + to_string(G);
				var h = H(); log = log + "|" + to_string(h.touch()) + to_string(h.touch()) + "," + to_string(G);
				G = 10; for (int i = 0; i < 2; ++i) { { var G = 50; var& y = G; y = y + 1; } var& z = G; z = z + 1; }
				log = log + "|" + to_string(G);
				log;
			)", "5,02040|6.5,9.5,101,9.5|89,9.5|12"},
				// stores through `var&` aliases (a local cell, a var field, a shared_ptr field) in statement and expression position: a
				// string, a shared handle and a by-value instance land as copies, the source keeps its value, handles share, and a
				// replaced handle's object dies at the store
				{"reference_alias_store_value_parity", R"(
				string log = "";
				class P { var n = 0; ~P() { log = log + "~" + to_string(n); } }
				class V { var n = 0; }
				class Box { var item = null; shared_ptr<P> held = null; }
				void put(var& r, var v) { r = v; }
				void run() {
					var cell = 0;
					var& r = cell;
					string s = "ab";
					r = s; s = s + "c";
					log = log + cell + s;
					var p1 = new P(); p1.n = 1;
					r = p1; p1.n = 2;
					log = log + "|" + to_string(cell.n);
					r = new P();
					log = log + "|";
					r = null;
					var cell2 = 0;
					var& r2 = cell2;
					V v; v.n = 3;
					r2 = v; v.n = 4;
					log = log + to_string(cell2.n);
					var res = (r2 = "x");
					log = log + "|" + res + cell2;
					Box b;
					var& it = b.item;
					it = "y"; put(it, p1); p1.n = 5;
					log = log + "|" + to_string(b.item.n);
					auto& h = b.held;
					h = p1; var other = new P(); other.n = 9; h = other; other = null;
					log = log + "|" + to_string(b.held.n);
					var got = (h = null);
					log = log + "|" + to_string(got == null);
				}
				run();
				log;
			)", "ababc|2|~03|xx|5|9~9|true~5"},
				// a field argument a frame-slot local names binds where it is evaluated: by reference when the callee's parameter is a
				// T&, by value otherwise, through a rebound callee, an overload set, a typed parameter, a field holding a reference, a null
				// receiver and a private field, in functions, recursion and the plain frame
				{"field_argument_place_binding_parity", R"(
				class O { var x = 1; int n = 2; var left = null; }
				class P { private: int secret = 3; public: int open = 4; }
				string log = "";
				void bump(var& r) { r = r + 10; }
				void bump_int(int& r) { r = r + 100; }
				int read(var v) { return v; }
				void two(var& a, var& b) { a = 7; b = 8; }
				function chain(var o, int depth) { if (depth == 0) { return 0; } bump(o.x); return chain(o, depth - 1); }
				void run() {
					var o = new O();
					bump(o.x); bump_int(o.n);
					log = `${log}${o.x},${o.n},${read(o.x)}`;
					var g = bump; g(o.x);
					g = read; log = `${log}|${g(o.x)},${o.x}`;
					var p = new O(); p.left = new O();
					two(p.x, p.left);
					log = `${log}|${p.x},${p.left}`;
					var c = new O(); chain(c, 3); log = `${log}|${c.x}`;
					var none = null; string err = "";
					try { bump(none.x); } catch (e) { err = e; }
					log = `${log}|${(err != "")}`;
					var q = new P(); string perr = "";
					try { bump(q.secret); } catch (e) { perr = e; }
					bump(q.open);
					log = `${log}|${(perr != "")},${q.open}`;
				}
				run();
				class S { void mix(var& r, string s) { r = 50; } void mix(var r, int n) { r = 60; } }
				var s = S(); var t = new O();
				s.mix(t.x, "a"); log = `${log}|${t.x}`;
				log;
			)", "11,102,11|21,21|7,8|31|true|true,14|50"},
				// A reference argument binds when it is evaluated, left to right, as in C++: a later argument that
				// reassigns the receiver, moves the index, or replaces a nested receiver leaves the binding where evaluation put it, for
				// functions, methods, constructors, a mixed-mode overload and a later conversion that runs code
				{"reference_argument_binds_at_evaluation_parity", R"(
				class O { var x = 0; }
				string log = "";
				void put(var& r, var unused) { r = 99; }
				var first = new O(); first.x = 1;
				var second = new O(); second.x = 2;
				var o = first;
				put(o.x, o = second);
				log = log + to_string(first.x) + "," + to_string(second.x);
				var a = [10, 20, 30];
				var i = 0;
				put(a[i], i++);
				log = log + "|" + to_string(a[0]) + "," + to_string(a[1]) + "," + to_string(i);
				var p = new O(); p.x = 5;
				var q = new O(); q.x = 6;
				var holder = p;
				function swap_holder() { holder = q; return 0; }
				put(holder.x, swap_holder());
				log = log + "|" + to_string(p.x) + "," + to_string(q.x);
				class Box { var item = null; }
				var b = Box(); b.item = new O();
				var old_item = b.item;
				function replace_item() { b.item = new O(); return 0; }
				put(b.item.x, replace_item());
				log = log + "|" + to_string(old_item.x) + "," + to_string(b.item.x);
				void local_case() {
					var f = new O(); var s = new O(); var w = f;
					put(w.x, w = s);
					array<int> e = [1, 2, 3]; int k = 0;
					put(e[k], k = 2);
					log = log + "|" + to_string(f.x) + "," + to_string(s.x) + "," + to_string(e[0]) + to_string(e[2]);
				}
				local_case();
				class M { void put(var& r, var u) { r = 5; } }
				var m = M();
				var m1 = new O(); var m2 = new O(); var mh = m1;
				m.put(mh.x, mh = m2);
				log = log + "|" + to_string(m1.x) + "," + to_string(m2.x);
				class Setter { Setter(var& r, var u) { r = 7; } }
				var k1 = new O(); var k2 = new O(); var kh = k1;
				Setter(kh.x, kh = k2);
				var made = new Setter(kh.x, kh = k1);
				log = log + "|" + to_string(k1.x) + "," + to_string(k2.x);
				var c1 = new O(); var c2 = new O(); var ch = c1;
				class Conv { var n = 0; Conv(int v) { n = v; ch = c2; } }
				void take(Conv c, var& r) { r = 77; }
				take(5, ch.x);
				log = log + "|" + to_string(c1.x) + "," + to_string(c2.x);
				var x1 = new O(); var x2 = new O(); var xh = x1;
				function set_xh() { xh = x2; return "s"; }
				class Mixer { void mix(int& r, string n) { log = log + "A"; r = 33; } void mix(int r, int n) { log = log + "B"; [[warn_mode]] r = 44; } }
				var mx = Mixer();
				mx.mix(xh.x, set_xh());
				log = log + "|" + to_string(x1.x) + "," + to_string(x2.x);
				var y1 = new O(); var yh = y1;
				put(yh.x, 3);
				log = log + "|" + to_string(y1.x);
				log;
			)", "99,2|99,20,1|99,6|99,0|99,0,993|5,0|7,7|77,0A|33,0|99"},
			};
			check_each(cases, [&](const auto& c) {
				auto [i, v] = run_both_backends_stdlib(std::get<1>(c));
				check_eq(std::string(std::get<2>(c)), i, "interpreter");
				check_eq(i, v, "vm against the interpreter");
			});
		});

		test("opaque_callee_call_parity", [this, run_both_backends_stdlib]() {
			// a callee that is not a name (an element, a call result, a conditional): exact arity, defaults, an arity miss, a
			// closure, a host function, a bound method, a callable object, a reference parameter, a throw, a non-callable
			auto [i1, v1] = run_both_backends_stdlib(R"(
				string log = "";
				int twice(int x) { return x * 2; }
				int defs(int a, int b = 10) { return a + b; }
				void bump(int& x) { x += 1; }
				int boom(int x) { if (x > 1) { throw "boom " + to_string(x); } return x; }
				function make_adder(int k) { return [k](int x) { return x + k; }; }
				function counter() { int n = 0; return [&n]() { n += 1; return n; }; }
				class Healer { int hp = 1; int heal(int by) { hp += by; return hp; } }
				class Adder { int k = 0; Adder(int v) { k = v; } int operator()(int x) { return x + k; } }
				var fs = [twice, defs, bump, boom, to_string];
				log = log + to_string(fs[0](21)) + "," + to_string(fs[1](1)) + "," + to_string(fs[1](1, 2));
				try { fs[0](1, 2, 3); } catch (e) { log = log + "|" + e; }
				log = log + "|" + to_string(make_adder(3)(4));
				var c = counter(); c(); log = log + "," + to_string(c());
				log = log + "|" + fs[4](5);
				var h = Healer();
				var bound = [reflect::bind(h, "heal")];
				bound[0](4); log = log + "|" + to_string(bound[0](5)) + "," + to_string(h.hp);
				var objs = [Adder(2)];
				log = log + "|" + to_string(objs[0](5));
				int v = 1; fs[2](v); fs[2](v); log = log + "|" + to_string(v);
				for (int i = 0; i < 3; ++i) { try { log = log + "," + to_string(fs[3](i)); } catch (e) { log = log + "," + e; } }
				bool pick = true;
				log = log + "|" + to_string((pick ? fs[0] : fs[1])(8));
				var n = [5];
				try { n[0](1); } catch (e) { log = log + "|" + e; }
				log;
			)");
			check_true(i1.find("boom 2") != std::string::npos, i1);
			check_eq(i1, v1, "opaque callee parity");
		});

		test("an_inlined_untyped_result_outlives_the_graft_block", [this]() {
			// fuzz seeds 1564 and 1848: an inlined call of a function without a declared result keeps its result in a var slot; a
			// program with a class makes var slots candidates for scope-exit kills, and the graft's block exit killed the result
			// before the expression read it (the vm raised `Invalid operands for * operator`). Single unit and across a redefinition
			const char* programs[][2] = {
				{ "class C0 { var m1 = [0, 4, 5]; }\n1;",
				  "function fn4(int p5) {\n\treturn 568;\n}\n"
				  "function zbreload() {\n\tif (\"hello\" < \"a\") { }\n\treturn fn4(7 | -3) * 0.0;\n}\n"
				  "to_string(zbreload()) + \" \" + to_string(fn4(1)) + \" \" + type_of(fn4(2) + 1);" },
				{ "class C0 { var m1 = [0, 4, 5]; }\n"
				  "function fn4(int p5) -> int {\n\tfor (int i6 = 0; i6 < 3; i6++) { }\n\tif ((p5 % (11 + 5)) != ((p5 + p5) <=> 0)) { }\n}\n",
				  "function fn4(int p5) {\n\treturn 568;\n}\n"
				  "function zbreload() {\n\tif (\"hello\" < \"a\") { }\n\treturn fn4(7 | -3) * 0.0;\n}\n"
				  "to_string(zbreload()) + \" \" + to_string(fn4(1)) + \" \" + type_of(fn4(2) + 1);" },
				{ "class D { int id = 0; ~D() { } }\nvar made = [];\n1;",
				  "function mk(int n) { return D(); }\n"
				  "function use() { if (made.size() > 5) { } var d = mk(3); return d.id + 1; }\n"
				  "function pick(int n) { return n > 0 ? \"pos\" : \"neg\"; }\n"
				  "to_string(use()) + \" \" + pick(2) + pick(-1) + \" 568 int\";" },
			};
			const std::string expected[] = { "0.0 568 int", "0.0 568 int", "1 posneg 568 int" };
			// fuzz-reduced units, not statically clean (program 1's fn4 can fall off its end): each opens a [[warn_mode]] region
			for (size_t pi = 0; pi < 3; ++pi) {
				std::string out[2];
				int idx = 0;
				for (bool use_vm : {false, true}) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					e->execution_budget(0);
					stdlib::register_all(*e);
					try {
						e->execute(std::string("[[warn_mode]]; ") + programs[pi][0]);
						out[idx] = e->execute(std::string("[[warn_mode]]; ") + programs[pi][1]).to_string();
					} catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
					++idx;
				}
				const std::string label = "program " + std::to_string(pi);
				check_eq(expected[pi], out[0], "interpreter, " + label);
				check_eq(out[0], out[1], "parity, " + label);
			}
		});

		test("a_host_callee_follows_its_registration_through_the_call_sites_lane", [this]() {
			// a call site keeps its host callee's one direct overload; a registration of the name between the probe and the call (in the
			// arguments) and a site refilled while outer calls of it are parked (recursion) run what the dispatcher runs on both backends
			auto run = [](bool use_vm, const char* program) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				stdlib::register_all(*e);
				jai::engine* raw = e.get();
				e->add_function("hf", [](jai::script_int x) -> jai::script_int { return x + 1; });
				e->add_function("swap", [raw]() -> jai::script_int {
					raw->add_function("hf", [](jai::script_int x) -> jai::script_int { return x + 100; });
					return 0;
				});
				try { return e->execute(program).to_string(); } catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
			};
			const char* in_arguments =
				"int total = 0;\n"
				"for (int i = 0; i < 4; ++i) { total = total * 1000 + hf(i == 2 ? swap() : i); }\n"
				"for (int i = 0; i < 3; ++i) { total = total + hf(i); }\n"
				"total;";
			const char* refilled_while_parked =
				"int k(int d, bool again) { if (d == 0) { if (again) { swap(); return k(1, false); } return 0; } return hf(k(d - 1, again)); }\n"
				"int a = k(3, true); int b = 0; for (int i = 0; i < 5; ++i) { b = b + hf(i); }\n"
				"to_string(a) + \" \" + to_string(b);";
			check_eq(std::string("1002100406"), run(false, in_arguments), "registration in the arguments (interpreter)");
			check_eq(run(false, in_arguments), run(true, in_arguments), "registration in the arguments (parity)");
			check_eq(std::string("400 510"), run(false, refilled_while_parked), "a site refilled while parked (interpreter)");
			check_eq(run(false, refilled_while_parked), run(true, refilled_while_parked), "a site refilled while parked (parity)");
		});
		test("to_string_runs_its_direct_entry_where_the_name_binds_it", [this]() {
			// a call site whose callee is the registered to_string runs its direct entry on the argument where it is; a reference
			// argument, a class's own to_string, a throwing one, a script overload and a host re-registration read as the function does
			auto run = [](bool use_vm, const char* program, bool reregister) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				stdlib::register_all(*e);
				jai::engine* raw = e.get();
				e->add_function("rereg", [raw, reregister]() -> jai::script_int {
					if (reregister) { raw->add_variadic_function("to_string", [raw](const std::vector<jai::script_value>&) -> jai::checked_result<jai::script_value> { return jai::script_value(std::string("host"), raw); }); }
					return 0;
				});
				try { return e->execute(program).to_string(); } catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
			};
			const char* kinds =
				"class P { int x = 3; string to_string() { return `P${x}`; } }\n"
				"class Bad { string to_string() { throw \"no text\"; } }\n"
				"array<float> fs = [1.5, 2.25]; string out = \"\";\n"
				"for (int i = 0; i < 3; ++i) { for (float f : fs) { out += `${to_string(f)},`; } out += to_string(i) + to_string(true) + to_string('c') + to_string(\"s\") + to_string(null) + \";\"; }\n"
				"for (auto& f : fs) { out += to_string(f); }\n"
				"P p; out += to_string(p) + to_string([1, 2]);\n"
				"try { to_string(Bad()); } catch (e) { out += ` caught ${e}`; }\n"
				"var g = to_string; out += ` ${g(7)}`;\n"
				"out;";
			const char* rebound =
				"string out = \"\";\n"
				"for (int i = 0; i < 4; ++i) { if (i == 2) { rereg(); } out += `${to_string(i)} `; }\n"
				"out;";
			check_eq(std::string("1.5,2.25,0truecsnull;1.5,2.25,1truecsnull;1.5,2.25,2truecsnull;1.52.25P3[1, 2] caught no text 7"), run(false, kinds, false), "kinds (interpreter)");
			check_eq(run(false, kinds, false), run(true, kinds, false), "kinds (parity)");
			check_eq(std::string("0 1 host host "), run(false, rebound, true), "re-registered (interpreter)");
			check_eq(run(false, rebound, true), run(true, rebound, true), "re-registered (parity)");
		});
		test("a_host_direct_entry_runs_for_its_function_and_arity_only", [this]() {
			// the embedding guide's add_host_direct sample: one body behind the variadic function and its direct entry; another
			// argument count, a reference argument and a script rebinding call the function. An engine-first variadic function
			// carries its own entry, alone under its name and unguarded, and returns what a typed binding returns
			auto run = [this](bool use_vm, const char* program) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				stdlib::register_all(*e);
				static constexpr jai::engine::host_direct_invoke label_of = [](const void* target, const jai::script_value* args, size_t count) -> jai::checked_result<jai::script_value> {
					auto* eng = static_cast<jai::engine*>(const_cast<void*>(target));
					if (count != 1) { return jai::script_value(std::string("?"), eng); }
					return jai::script_value(std::string(args[0].is_int() ? "int" : args[0].is_float() ? "float" : "other"), eng);
				};
				e->add_variadic_function("label", [eng = e.get()](const std::vector<jai::script_value>& args) {
					return label_of(eng, args.data(), args.size());
				});
				e->add_host_direct("label", 1, label_of, e.get());
				e->add_variadic_function("sum_all", [](jai::engine&, std::span<const jai::script_value> args) {
					jai::script_int total = 0;
					for (const auto& a : args) { total += a.as_int(); }
					return total;
				});
				e->add_variadic_function("kind_of", [](jai::engine& eng, std::span<const jai::script_value> args) {
					return jai::script_value(std::string(args.empty() ? "none" : args[0].is_int() ? "int" : args[0].is_string() ? "string" : "other"), &eng);
				});
				e->add_variadic_function("first_of", [](jai::engine&, std::span<const jai::script_value> args) -> jai::checked_result<jai::script_value> {
					if (args.empty()) { return jai::checked_result<jai::script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "first_of needs an argument"); }
					return args[0];
				});
				e->add_variadic_function("try_count", [](jai::engine&, std::span<const jai::script_value> args) { return !args.empty(); }, jai::nodiscard);
				e->add_variadic_function("io_count", [](jai::engine&, std::span<const jai::script_value> args) { return static_cast<jai::script_int>(args.size()); }, jai::filesystem);
				e->register_privilege("app.count");
				e->add_variadic_function("app_count", [](jai::engine&, std::span<const jai::script_value> args) { return static_cast<jai::script_int>(args.size()); }, "app.count");
				e->add_variadic_function("both_count", [](jai::engine&, std::span<const jai::script_value> args) { return static_cast<jai::script_int>(args.size()); }, {"filesystem", "app.count"});
				auto row_of = [&](const char* name) {
					const jai::script_value fn = detail::engine_access::get_variable_unheld(*e, name);
					return detail::engine_access::host_direct_for(*e, &fn.unchecked_as_function()) != nullptr;
				};
				check_true(row_of("sum_all") && row_of("kind_of") && row_of("try_count"), "an unguarded engine-first function has its direct entry");
				check_false(row_of("io_count") || row_of("app_count") || row_of("both_count"), "a guarded one runs its guard on every call");
				check_true(e->is_nodiscard_function("try_count") && !e->is_nodiscard_function("sum_all"));
				const auto dropped = e->check("try_count(1);\nprint(1);");
				check_true(dropped.format().find("the result of 'try_count' is discarded") != std::string::npos, dropped.format());
				std::string denied;
				try { e->execute("app_count(1);"); } catch (const std::exception& ex) { denied = ex.what(); }
				e->enable(std::string_view("app.count"));
				try { return denied + "|" + e->execute(program).to_string(); } catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
			};
			const char* program =
				"string out = \"\"; float f = 2.5;\n"
				"for (int i = 0; i < 3; ++i) { out += label(i) + label(f) + label(\"s\") + label() + label(1, 2) + \" \"; }\n"
				"array<float> fs = [1.5]; for (auto& r : fs) { out += label(r); }\n"
				"int total = 0; for (int i = 0; i < 3; ++i) { total += sum_all() + sum_all(i) + sum_all(1, 2, 3) + sum_all(1, 2, 3, 4, 5, 6, 7, 8, 9); }\n"
				"array<int> xs = [40]; for (auto& x : xs) { total += sum_all(x, 2); }\n"
				"out += ` ${total} ${kind_of()}${kind_of(1)}${kind_of(\"s\")}${kind_of(1.5)} ${first_of(7, 8)}`;\n"
				"try { first_of(); } catch (e) { out += ` ${e}`; }\n"
				"bool any = try_count(); out += ` ${any} ${io_count(1, 2)} ${app_count(1, 2, 3)} ${both_count()}`;\n"
				"out;";
			check_eq(std::string("Insufficient privilege: app.count.|intfloatother?? intfloatother?? intfloatother?? float 198 noneintstringother 7 first_of needs an argument false 2 3 0"), run(false, program), "interpreter");
			check_eq(run(false, program), run(true, program), "parity");
		});
		test("map_stores_borrow_their_key_and_element_reads_write_back_only_through_mutators", [this]() {
			// a map store takes its key where it is (a range-for alias, a value read from the same map), and a string method called
			// through a typed element alias writes the element back only when the registry lists it as a mutator
			auto run = [](bool use_vm, const char* program) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				stdlib::register_all(*e);
				try { return e->execute(program).to_string(); } catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
			};
			const char* keys =
				"map<string, int> m = {};\n"
				"array<string> src = [\"a\", \"b\", \"a\", \"longer key\", \"a\"];\n"
				"for (auto& s : src) { m[s] = m.get(s, 0) + 1; }\n"
				"map<string, string> n = {\"a\": \"b\"};\n"
				"n[n[\"a\"]] = \"c\";\n"
				"to_string(m[\"a\"]) + \" \" + to_string(m[\"b\"]) + \" \" + to_string(m[\"longer key\"]) + \" \" + n[\"b\"] + \" \" + to_string(n.size());";
			const char* aliases =
				"array<string> a = [\"1.5\", \"xy\"];\n"
				"float total = 0.0; int sizes = 0;\n"
				"for (auto& s : a) { if (s.size() > 2) { total = total + s.to_float(); } sizes = sizes + s.size(); s.to_upper(); }\n"
				"to_string(total) + \" \" + to_string(sizes) + \" \" + a[0] + a[1];";
			check_eq(std::string("3 1 1 c 2"), run(false, keys), "keys (interpreter)");
			check_eq(run(false, keys), run(true, keys), "keys (parity)");
			check_eq(std::string("1.5 5 1.5XY"), run(false, aliases), "aliases (interpreter)");
			check_eq(run(false, aliases), run(true, aliases), "aliases (parity)");
		});
		test("a_range_for_alias_of_a_typed_array_takes_the_builtin_lanes", [this]() {
			// the checker proves a builtin receiver that is a range-for reference over a typed array's elements (a by-reference
			// parameter's too): the call takes the builtin lanes, which check the receiver at run time
			const char* program =
				"float sum(array<string>& nums) { float t = 0.0; for (auto& s : nums) { t = t + s.to_float() + s.size(); } return t; }\n"
				"array<string> a = [\"1.5\", \"2.25\"];\n"
				"float u = 0.0; for (auto& s : a) { u = u + s.to_float(); }\n"
				"to_string(sum(a)) + \" \" + to_string(u);";
			auto run = [&](bool use_vm, std::string* listing) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				stdlib::register_all(*e);
				if (listing) { e->disassembly_sink([listing](const std::string& l, const std::string&) { *listing += l; *listing += "\n"; }); }
				try { return e->execute(program).to_string(); } catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
			};
			std::string listing;
			check_eq(std::string("10.75 3.75"), run(false, nullptr), "values (interpreter)");
			check_eq(run(false, nullptr), run(true, &listing), "parity");
			check_true(listing.find("CALL_METHOD") == std::string::npos, "no general method call remains: " + listing);
		});
		test("a_range_for_alias_builtin_keeps_its_value_when_the_container_changes_or_holds_other_kinds", [this]() {
			// the receiver proof only picks a lane: a container reassigned in the loop, an array<var> of mixed kinds (no proof, the general
			// call) and a store of another kind through the alias (the element boundary raises, caught) agree with the interpreter
			const char* program =
				"array<string> a = [\"1.5\", \"abc\"]; int n = 0;\n"
				"for (auto& s : a) { n = n + s.size(); a = [\"xxxxxx\"]; n = n + s.size(); }\n"
				"array<var> mixed = [\"ab\", [1, 2, 3], 7]; int m = 0;\n"
				"for (auto& x : mixed) { if (type_of(x) != \"int\") { m = m + x.size(); } }\n"
				"array<string> b = [\"q\"]; string caught = \"none\";\n"
				"for (var& s : b) { try { s = [1]; } catch (e) { caught = \"raised\"; } n = n + s.size(); }\n"
				"to_string(n) + \" \" + to_string(m) + \" \" + caught + \" \" + b[0];";
			auto run = [&](bool use_vm) -> std::string {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->execution_budget(0);
				stdlib::register_all(*e);
				try { return e->execute(program).to_string(); } catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
			};
			const std::string iv = run(false);
			check_true(iv.find("ERROR") == std::string::npos, "the interpreter runs it: " + iv);
			check_eq(iv, run(true), "parity");
		});
		test("handle_and_field_reference_argument_parity", [this, run_both_backends_stdlib]() {
			// handles and nulls into var parameters (shared, never copied), var& parameters bound to fields of handles, stores through
			// them (a declared field converts, a var field takes anything), a captured parameter, a by-value instance still copies
			auto [i1, v1] = run_both_backends_stdlib(R"(
				class N { var v = 0; var left = null; var right = null; int count = 0; float w = 0.5; }
				class P { int k = 1; }
				string log = "";
				void touch(var n, var other) { if (n != null) { n.v = n.v + 1; } if (other == null) { log = log + "n"; } }
				void link(var parent, var& slot, var child) { slot = child; parent.count = parent.count + 1; }
				void set_both(var& a, var& b, var x) { a = x; b = x; }
				function keep(var n) { var f = [&]() { n = null; }; f(); return n == null; }
				void bump(var p) { p.k = p.k + 10; }
				var root = new N();
				var kid = new N();
				touch(root, null); touch(root, kid); touch(null, null);
				link(root, root.left, kid); link(root, root.right, new N()); link(kid, kid.left, null);
				log = log + "|" + to_string(root.v) + to_string(root.count) + to_string(root.left.v) + to_string(root.right != null) + to_string(kid.left == null);
				set_both(root.left.v, root.right.v, 7); set_both(root.count, root.w, 3);
				log = log + "|" + to_string(kid.v) + to_string(root.right.v) + to_string(root.count) + "," + to_string(root.w) + "," + type_of(root.w);
				try { set_both(root.count, root.v, "s"); } catch (e) { log = log + "|" + e; }
				log = log + "|" + to_string(keep(root)) + to_string(root != null);
				var p = P(); bump(p); var sp = new P(); bump(sp);
				log = log + "|" + to_string(p.k) + "," + to_string(sp.k);
				class T { int x = 0; shared_ptr<T> left = null; shared_ptr<T> right = null; }
				class U : T { int extra = 1; }
				void put(shared_ptr<T>& slot, shared_ptr<T> v) { slot = v; }
				void put_any(var& slot, var v) { slot = v; }
				void clear(shared_ptr<T>& slot) { slot = null; }
				shared_ptr<T> t = new T();
				shared_ptr<T> c = new T(); c.x = 5;
				put(t.left, c); put(t.right, t.left); clear(t.left);
				log = log + "|" + to_string(t.left == null) + to_string(t.right.x) + type_of(t.right);
				shared_ptr<U> u = new U(); u.x = 9;
				put(t.left, u); put_any(t.right, null);
				log = log + "|" + to_string(t.left.x) + type_of(t.left) + to_string(t.right == null);
				try { put_any(t.left, P()); } catch (e) { log = log + "|" + e; }
				try { put_any(t.left, new P()); } catch (e) { log = log + "|" + e; }
				class B2 { int a = 0; }
				class D2 : B2 { int d = 1; }
				function rc() -> string { B2 x = D2(); D2& dx = cast<D2&>(x); int& r = dx.d; int k = r; r = 3; k = k + r; x = B2(); string out = to_string(k);
					try { int z = r; out = out + "read"; } catch (e) { out = out + "/" + e; }
					try { r = 4; out = out + "write"; } catch (e) { out = out + "/" + e; }
					return out; }
				log = log + "|" + rc();
				class Pinned { var v = 1; int id = 0; ~Pinned() { log = log + "~" + to_string(id); } }
				function pin_outlives() { var n = new Pinned(); n.id = 1; var& r = n.v; n = null; log = log + "a"; r = 5; log = log + to_string(r); return 0; }
				void set_v(var& slot, var x) { slot = x; }
				function pin_param() { var n = new Pinned(); n.id = 2; var m = n; n = null; set_v(m.v, 9); var k = m.v; m = null; log = log + "b" + to_string(k); return 0; }
				pin_outlives(); log = log + "|"; pin_param(); log = log + "|";
				log;
			)");
			check_true(i1.find("Cannot") != std::string::npos, i1);
			check_eq(i1, v1, "handle and field reference argument parity");
		});

		test("a_forwarded_graft_parameter_follows_its_callees_definitions", [this]() {
			// a scalar parameter passed on by value to another script function is a renaming of the caller's variable (no move); a
			// redefinition of that callee to take it by reference and write it makes the outer graft's guard fail, so the write lands in
			// the parameter's own copy: between runs, and in the middle of the loop through a module re-run (both backends)
			const auto dir = jai::foundry::test_temp_directory();
			int tick = 0;
			auto write_module = [&](const std::string& body) {
				const auto path = dir / "fwd_mod.jai";
				{ std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
				std::error_code ec;
				std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
			};
			const std::string by_value = "function in_box(int w, int x, int y) -> bool { return x >= 0 && y >= 0 && x < w && y < w; }\n";
			const std::string by_reference = "function in_box(int w, int& x, int y) -> bool { x = 100 + x; return y >= 0; }\n";
			const char* program = R"(
				import "fwd_mod.jai";
				function cell(int w, int x, int y) -> int { return y * w + x; }
				function walk(int w, int x, int y, int tx, int ty) -> bool { if (!in_box(w, x, y)) { return false; } return cell(w, x, y) % 3 != 0 || (x == tx && y == ty); }
				function run(int swap_at) -> int {
					int s = 0;
					int w = 9;
					for (int i = 0; i < 81; ++i) {
						if (i == swap_at) { swap_module(); import("fwd_mod.jai", jai::force); }
						int nx = i % 9;
						int ny = i / 9 - 1;
						if (walk(w, nx, ny, 4, 4)) { s += nx * 10 + ny; }
						s += nx;
					}
					return s;
				}
				string a = to_string(run(-1)) + " " + to_string(run(-1));
				a;
			)";
			std::string out[2][3];
			for (int use_vm = 0; use_vm < 2; ++use_vm) {
				write_module(by_value);
				auto e = make_engine();
				if (use_vm) { e->set_backend(jai::backend_type::vm); }
				stdlib::register_all(*e);
				e->add_include_path(dir.string());
				e->add_function("swap_module", [&]() { write_module(by_reference); });
				try {
					out[use_vm][0] = e->execute(program).to_string();
					write_module(by_reference);
					e->execute("import(\"fwd_mod.jai\", jai::force);");
					out[use_vm][1] = e->execute("to_string(run(-1));").to_string();
					write_module(by_value);
					e->execute("import(\"fwd_mod.jai\", jai::force);");
					out[use_vm][2] = e->execute("to_string(run(-1)) + \" \" + to_string(run(40));").to_string();
				} catch (const std::exception& ex) { out[use_vm][0] = std::string("ERROR: ") + ex.what(); }
			}
			for (int k = 0; k < 3; ++k) { check_eq(out[0][k], out[1][k], "forwarded parameter parity, step " + std::to_string(k)); }
			check_true(out[0][0].find("ERROR") == std::string::npos, "the interpreter runs the program: " + out[0][0]);
		});
		test("a_callee_replaced_while_its_cached_calls_run_stays_alive", [this]() {
			// a cached call borrows its site's pin of the callee. The module defining the callee re-runs in the middle of the callee's
			// own recursion (its old program is released), the site refills with the new definition, and the old frames finish on the
			// old body; then a definition in another unit replaces it again (both backends)
			const auto dir = jai::foundry::test_temp_directory();
			int tick = 0;
			auto write_module = [&](const std::string& body) {
				const auto path = dir / "callee_mod.jai";
				{ std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
				std::error_code ec;
				std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
			};
			const std::string old_body =
				"function work(int n) -> int {\n"
				"    if (n == 3) { swap_module(); import(\"callee_mod.jai\", jai::force); }\n"
				"    if (n == 0) { return 0; }\n"
				"    string pad = \"frame\" + to_string(n);\n"
				"    int below = run(n - 1);\n"
				"    return below + n * 10 + pad.size();\n"
				"}\n";
			std::string out[2];
			int idx = 0;
			for (bool use_vm : {false, true}) {
				write_module(old_body);
				auto e = make_engine();
				if (use_vm) { e->set_backend(jai::backend_type::vm); }
				stdlib::register_all(*e);
				e->add_include_path(dir.string());
				e->add_function("swap_module", [&]() { write_module("function work(int n) -> int { return 1000 + n; }\n"); });
				try {
					out[idx] = e->execute(R"(
						int run(int n) { return work(n); }
						import "callee_mod.jai";
						int warm = run(0) + run(1) + run(2);
						int first = run(6);
						int second = run(2);
						function work(int n) -> int { return n < 1 ? 7 : run(n - 1) + 1; }
						int third = run(4);
						to_string(warm) + " " + to_string(first) + " " + to_string(second) + " " + to_string(third)
					)").to_string();
				} catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
				++idx;
			}
			check_eq(std::string("58 1206 1002 11"), out[0], "interp: a module re-run during a cached recursion");
			check_eq(out[0], out[1], "callee replacement parity");
		});

		test("a_callee_whose_program_is_gone_outlives_its_cached_calls", [this]() {
			// the callee's only owners are its call site's pin and its running (borrowed) frames: it was defined by an execute()
			// whose program the cache has since evicted, a redefinition took its name, and the site refills while it runs. Its
			// frames then raise past the host, whose stack trace names the old frames (both backends; an ASAN build reports any
			// stale read)
			std::string out[2];
			int idx = 0;
			for (bool use_vm : {false, true}) {
				auto e = make_engine();
				if (use_vm) { e->set_backend(jai::backend_type::vm); }
				stdlib::register_all(*e);
				jai::engine* raw = e.get();
				e->add_function("replace_work", [raw]() {
					raw->execute(std::string("function work(int n) -> int { return 1000 + n; }"));
					for (int i = 0; i < 100; ++i) { raw->execute("int churn_" + std::to_string(i) + " = " + std::to_string(i) + ";"); }
				});
				try {
					e->execute(std::string("extern function work;\nint run(int n) { return work(n); }"));
					e->execute(std::string(
						"function work(int n) -> int {\n"
						"    if (n == 0) { return 0; }\n"
						"    string pad = \"old\" + to_string(n);\n"
						"    if (n == 3) { replace_work(); int fresh = run(2); throw pad + \":\" + to_string(fresh); }\n"
						"    return run(n - 1) + pad.size();\n"
						"}\n"));
					const std::string warm = e->execute(std::string("to_string(run(1) + run(2))")).to_string();
					std::string thrown;
					try { e->execute(std::string("run(6);")); } catch (const std::exception& ex) { thrown = ex.what(); }
					size_t old_frames = 0;
					for (const auto& fr : detail::engine_access::get_execution_backend(*e)->last_stack_trace()) { if (fr.function == "work") { ++old_frames; } }
					out[idx] = warm + " " + std::to_string(thrown.find("old3:1002") != std::string::npos) + " " + std::to_string(old_frames) + " " +
					           e->execute(std::string("to_string(run(3))")).to_string();
				} catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
				++idx;
			}
			check_eq(std::string("12 1 4 1003"), out[0], "interp: the throw and the four old frames of the trace (6, 5, 4, 3) by name");
			check_eq(out[0], out[1], "parity");
		});

		test("join_reads_a_string_buffer_in_place", [this]() {
			std::string out[2];
			int idx = 0;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				if (use_vm) { e->set_backend(jai::backend_type::vm); }
				stdlib::register_all(*e);
				try {
					out[idx] = e->execute(R"(
						array<string> a = ["x", "", "yz"];
						array<string> none = [];
						array<string> one = ["solo"];
						array<var> mixed = ["p", 1, 2.5, true];
						string r = a.join("-") + "|" + a.join() + "|" + none.join(",") + "|" + one.join(",") + "|" + mixed.join("/");
						r += "|" + string::join(a, "+") + "|" + string::join(one) + "|" + type_of(string::join(a, utf8_string("~")));
						a[1] = "mid";
						r += "|" + a.join(", ");
						r
					)").to_string();
				} catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
				++idx;
			}
			check_eq(std::string("x--yz|xyz||solo|p/1/2.5/true|x++yz|solo|utf8_string|x, mid, yz"), out[0], "interp: join over string buffers");
			check_eq(out[0], out[1], "join parity");
		});

		test("field_argument_place_binding_follows_a_redefined_callee", [this]() {
			// the same call site, its callee redefined between runs from a T& parameter to a value parameter and back, then to an
			// overload set: each call binds as the callee it reaches does (both backends)
			std::string out[2];
			int idx = 0;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				stdlib::register_all(*e);
				e->execute("extern function f; class O { var x = 1; } var o = new O(); string log = \"\"; void step() { var p = o; f(p.x); log = `${log}${o.x},`; }");
				e->execute("void f(var& r) { r = r + 1; }");
				e->execute("step(); step();");
				e->execute("void f(var r) { r = r + 1; }");
				e->execute("step();");
				e->execute("void f(var& r) { r = r * 10; }");
				e->execute("step();");
				e->execute("void f(var& r, int k) { r = k; }");
				e->execute("step();");
				out[idx++] = e->execute("log;").to_string();
			}
			check_eq(std::string("2,3,3,30,300,"), out[0], "interp: a redefined callee");
			check_eq(out[0], out[1], "a redefined callee parity");
		});

		test("host_function_direct_entry_parity", [this]() {
			// a one-overload by-value host function runs on the arguments where they are: exact kinds, a converted argument, a
			// throw, a callback into script, a re-registration while it runs, a closed loop calling it
			std::string out[2];
			int idx = 0;
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				stdlib::register_all(*e);
				jai::engine* raw = e.get();
				e->add_function("add_one", [](jai::script_int x) -> jai::script_int { return x + 1; });
				e->add_function("half", [](jai::script_float x) -> jai::script_float { return x / 2; });
				e->add_function("shout", [](std::string s, bool loud) -> std::string { return loud ? s + "!" : s; });
				e->add_function("fails", [](jai::script_int x) -> jai::script_int { if (x > 1) { throw std::runtime_error("too big"); } return x; });
				e->add_function("relay", [raw](jai::script_int x) -> jai::script_int { return raw->execute("twice(" + std::to_string(x) + ")").as_int(); });
				e->add_function("swap_self", [raw](jai::script_int x) -> jai::script_int {
					raw->add_function("swap_self", [](jai::script_int y) -> jai::script_int { return y * 100; });
					return x;
				});
				try {
					out[idx] = e->execute(R"(
						int twice(int v) { return v * 2; }
						int count = 0;
						for (int i = 0; i < 1000; ++i) { count = add_one(count); }
						string log = to_string(count) + " " + to_string(half(5)) + " " + to_string(half(3.0)) + " " + shout("hi", true) + shout("lo", false);
						try { fails(5); } catch (e) { log += " " + e; }
						log += " " + to_string(fails(1)) + " " + to_string(relay(21));
						log += " " + to_string(swap_self(3)) + " " + to_string(swap_self(3));
						log
					)").to_string();
				} catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
				++idx;
			}
			check_eq(std::string("1000 2.5 1.5 hi!lo too big 1 42 3 300"), out[0], "interp: host direct entry");
			check_eq(out[0], out[1], "host direct entry parity");
		});

		test("global_qualified_missing_global_error_parity", [this, run_both_backends_in]() {
			// A ::name with no global never falls back to a same-named member or local:
			// the undefined-variable text is identical on both backends for every consumer
			auto [i1, v1] = run_both_backends_in(true, R"(
				class W { int nothere = 5; int a() { return ::nothere; } }
				auto w = W();
				w.a();
			)");
			check_eq(i1, v1, "return ::missing error parity");
			check_true(i1.find("Undefined variable 'nothere'") != std::string::npos, "return ::missing raises (got: " + i1 + ")");
			auto [i2, v2] = run_both_backends_in(true, R"(
				class W { int nothere = 5; int a() { return ::nothere + 1; } }
				auto w = W();
				w.a();
			)");
			check_eq(i2, v2, "fused ::missing operand error parity");
			check_true(i2.find("Undefined variable 'nothere'") != std::string::npos, "fused ::missing raises (got: " + i2 + ")");
			auto [i3, v3] = run_both_backends_in(true, R"(
				class W { int nothere = 5; int a() { auto& r = ::nothere; return r; } }
				auto w = W();
				w.a();
			)");
			check_eq(i3, v3, "ref-decl ::missing error parity");
			check_true(i3.find("Cannot take reference of undefined variable") != std::string::npos, "ref-decl ::missing raises (got: " + i3 + ")");
			auto [i4, v4] = run_both_backends_in(true, R"(
				class W { int nothere = 5; int a() { auto& r = cast<int&>(::nothere); return r; } }
				auto w = W();
				w.a();
			)");
			check_eq(i4, v4, "cast<T&> ::missing error parity");
			check_true(i4.find("Cannot take reference of undefined variable") != std::string::npos, "cast<T&> ::missing raises (got: " + i4 + ")");
			auto [i5, v5] = run_both_backends_in(true, R"(
				auto& g() { int nothere = 1; return ::nothere; }
				g();
			)");
			check_eq(i5, v5, "ref-return ::missing error parity");
			check_true(i5.find("Cannot take reference of undefined variable") != std::string::npos, "ref-return ::missing raises (got: " + i5 + ")");
		});

		// === Callee-first semantics guards (callee-before-args
		// IS the language, matching C++17; the vm's probe pair keeps the observation
		// point without materializing the callee on the value stack) ===

		test("callee_first_missing_fn_error_before_args", [this, run_both_backends, run_both_backends_in]() {
			// An undefined callee errors BEFORE argument side effects run. Strict (the
			// default) rejects the undeclared name at load; the runtime ordering runs in
			// a [[warn_mode]] region and, under strict, through a null callable held in a var.
			const char* src = R"(
				var ran = false;
				function side() -> auto { ran = true; return 1; }
				var msg = "";
				try { undefined_callee(side()); } catch (e) { msg = "caught"; }
				`${msg}:${ran}`;
			)";
			auto [i1, v1] = run_both_backends_in(true, src);
			check_eq(std::string("caught:false"), i1, "interp: missing callee beats arg side effects");
			check_eq(i1, v1, "missing-callee ordering parity");
			auto [s1, sv1] = run_both_backends(src);
			check_eq(s1, sv1, "strict listing parity");
			check_true(s1.find("ERROR: static check failed") == 0 &&
				s1.find("use of undeclared identifier 'undefined_callee'") != std::string::npos,
				"strict rejects the undeclared callee at load (got: " + s1 + ")");
			auto [n1, nv1] = run_both_backends(R"(
				var ran = false;
				function side() -> auto { ran = true; return 1; }
				var msg = "";
				var missing = null;
				try { missing(side()); } catch (e) { msg = "caught"; }
				`${msg}:${ran}`;
			)");
			check_eq(std::string("caught:false"), n1, "interp: null callee beats arg side effects");
			check_eq(n1, nv1, "null-callee ordering parity");
		});

		test("the_callee_resolves_before_its_arguments", [this, run_both_backends]() {
			const std::tuple<const char*, const char*, const char*> cases[] = {
				// A defined-but-not-callable callee errors BEFORE argument side effects
				// (the vm's old post-args check was a latent divergence; the probe fixed it)
				{"callee_first_not_a_function_before_args", R"(
				var notFn = 42;
				var ran = false;
				function side() -> auto { ran = true; return 1; }
				var msg = "";
				try { notFn(side()); } catch (e) { msg = "caught"; }
				`${msg}:${ran}`;
			)", "caught:false"},
				// The callee is RESOLVED before args: an argument that rebinds the name
				// still invokes the original callable
				{"callee_first_rebind_in_args_calls_original", R"(
				function orig(auto x) -> auto { return `orig:${x}`; }
				function repl(auto x) -> auto { return `repl:${x}`; }
				var h = orig;
				function swap_h() -> auto { h = repl; return 7; }
				var first = h(swap_h());
				var second = h(1);
				`${first}|${second}`;
			)", "orig:7|repl:1"},
				// An argument throw unwinding to a catch must drop the parked callee so
				// later calls stay aligned (pending-register hygiene)
				{"callee_first_pending_survives_throw_in_args", R"(
				function boom() -> auto { throw "x"; }
				function ok(auto v) -> auto { return v + 1; }
				var r = 0;
				try { ok(boom()); } catch (e) { r = ok(41); }
				`${r}:${ok(1)}`;
			)", "42:2"},
			};
			check_each(cases, [&](const auto& c) {
				auto [i, v] = run_both_backends(std::get<1>(c));
				check_eq(std::string(std::get<2>(c)), i, "interpreter");
				check_eq(i, v, "vm against the interpreter");
			});
		});

		test("callee_first_pending_survives_yield_in_args", [this, run_both_backends]() {
			// f(yield ...) suspends BETWEEN callee resolution and the call; the parked
			// callee is per-fiber state and must survive resumer-side calls
			auto [i1, v1] = run_both_backends(R"(
				var add(var a, var b) { return a + b; }
				coroutine var gen() {
					return add(10, (yield 1) ? 0 : 5);
				}
				var h = gen();
				var first = h.next();
				var between = add(2, 3);
				var last = h.next();
				`${first}:${between}:${last}:${h.done()}`;
			)");
			check_eq(std::string("1:5:15:true"), i1, "interp: yield-in-args pending isolation");
			check_eq(std::string("1:5:15:true"), v1, "vm: yield-in-args pending isolation");
		});

		// INDEX operand fusion: member-base containers (`base.field[i]` resolves
		// the member inside the op through its own member_ic row) and computed flat-binary
		// indexes (`a[i + 1]` runs the index through binary_fused_compute in-op). Every
		// test below runs both backends; the exotic shapes exercise the verbatim replays.

		test("fused_member_index_reads_and_stores_match_the_interpreter", [this, run_both_backends]() {
			const std::tuple<const char*, const char*, const char*> cases[] = {
				// Typed + untyped array fields and a map field, read and stored through hot
				// loops inside a function frame (the IC-hit shape) and at top level
				{"fused_member_index_read_store_hot", R"(
				class C {
					array<int> buf = [10, 20, 30, 40];
					var arr = [1.5, 2.5, 3.5];
					var lut = {"a": 100, "b": 200};
				}
				var c = C();
				int hot() {
					int s = 0;
					for (int i = 0; i < 4; ++i) { s += c.buf[i]; }
					for (int i = 0; i < 4; ++i) { c.buf[i] = c.buf[i] + 1; }
					for (int i = 0; i < 4; ++i) { s += c.buf[i]; }
					return s;
				}
				var total = hot();
				c.arr[1] = c.arr[1] + 10.0;
				var key = "b";
				c.lut["a"] = c.lut["a"] + 7;
				c.lut[key] = c.lut[key] + 1;
				`${total}:${c.arr[1]}:${c.lut["a"]}:${c.lut["b"]}`;
			)", "204:12.5:107:201"},
				// A user getter fires per access whether the name is a NON-field or a DECLARED
				// field (the fused site stays negative and rides the ladder to the getter)
				{"fused_member_index_getter_and_shadow", R"(
				class G {
					var arr = [1, 2, 3];
					int calls = 0;
					var _get_virt() { calls = calls + 1; return [7, 8, 9]; }
					var _get_arr() { return [666]; }
				}
				var g = G();
				int s = 0;
				for (int i = 0; i < 3; ++i) { s += g.virt[i]; }
				var declared = g.arr[0];
				`${s}:${g.calls}:${declared}`;
			)", "24:3:666"},
			};
			check_each(cases, [&](const auto& c) {
				auto [i, v] = run_both_backends(std::get<1>(c));
				check_eq(std::string(std::get<2>(c)), i, "interpreter");
				check_eq(i, v, "vm against the interpreter");
			});
		});

		test("fused_member_index_errors_and_unwind", [this, run_both_backends]() {
			// OOB read/store, absent member, private access wall, and a getter that
			// throws mid-op — every spelling and the post-catch engine state must match
			auto [i1, v1] = run_both_backends(R"(
				class E {
					var arr = [1, 2, 3];
					var _get_boom() { throw "kapow"; }
				private:
					var sec = [9];
				public:
					int rd() { return sec[0]; }
				}
				var e = E();
				var out = "";
				try { var t = e.arr[99]; out = `${out}ran`; } catch (er) { out = out + er; }
				try { e.arr[99] = 5; out = `${out}|ran`; } catch (er) { out = `${out}|${er}`; }
				try { var t = e.nope[0]; out = `${out}|ran`; } catch (er) { out = `${out}|${er}`; }
				try { var t = e.sec[0]; out = `${out}|ran`; } catch (er) { out = `${out}|${er}`; }
				try { var t = e.boom[0]; out = `${out}|ran`; } catch (er) { out = `${out}|${er}`; }
				out = `${out}|${e.rd()}|${e.arr[1]}`;
				out;
			)");
			check_eq(i1, v1, "member-site error spelling and unwind parity");
			check(i1.find("|9|2") != std::string::npos, "engine usable after every member-site error");
		});

		test("fused_member_index_reload_epoch", [this, run_both_backends]() {
			// The fused op's member_ic row revalidates on method_epoch: a reload that
			// removes the field must error at the already-hot subscript site
			auto [i1, v1] = run_both_backends(R"(
				class P { var arr = [1, 2, 3]; }
				var p = P();
				int hot() { int s = 0; for (int i = 0; i < 3; ++i) { s += p.arr[i]; } return s; }
				var first = hot();
				class P { var other = 1; }
				var msg = "";
				try { var t = hot(); msg = "ran"; } catch (er) { msg = er; }
				`${first}:${msg}`;
			)");
			check_eq(i1, v1, "member fused site revalidates across reload");
			check(i1.rfind("6:", 0) == 0, "hot loop summed before the reload");
		});

		test("fused_binary_index_pure_and_replay", [this, run_both_backends]() {
			// Flat computed indexes: pure numeric shapes run in-op; float results, map
			// containers with computed string keys, div-by-zero and typed-store mismatch
			// all replay with the unfused spelling
			auto [i1, v1] = run_both_backends(R"(
				[[warn_mode]] // the mismatch must reach the runtime replay path
				var a = [10, 20, 30, 40, 50];
				array<int> t = [1, 2, 3, 4];
				var m = {"kx": 5};
				int hot() {
					int s = 0;
					for (int i = 0; i < 3; ++i) { s += a[i + 1]; }
					for (int j = 2; j < 4; ++j) { a[j - 1] = a[j - 1] + 100; }
					t[1 + 1] = t[2 * 1] + 7;
					return s;
				}
				var total = hot();
				var suffix = "x";
				var viaKey = m["k" + suffix];
				var out = `${total}:${a[1]}:${a[2]}:${t[2]}:${viaKey}`;
				var z = 0;
				try { var q = a[3 / z]; out = out + "|ran"; } catch (er) { out = out + "|" + er; }
				try { var q2 = a[1 + 1.5]; out = out + "|ran2:" + q2; } catch (er) { out = out + "|" + er; }
				try { t[1 + 0] = "nope"; out = out + "|ran3"; } catch (er) { out = out + "|" + er; }
				var big = 9223372036854775807;
				try { var q4 = a[big + big]; out = out + "|ran4"; } catch (er) { out = out + "|" + er; }
				try { a[big + big] = 1; out = out + "|ran5"; } catch (er) { out = out + "|" + er; }
				out;
			)");
			check_eq(i1, v1, "computed-index fusion parity (pure + replay spellings)");
			check(i1.rfind("90:", 0) == 0, "pure in-op reads summed");
		});

		test("fused_index_chain_pure_and_replay", [this, run_both_backends]() {
			// Left-spine arithmetic chains in the index: 2- and 3-level raw folds, an
			// env-global middle operand, mid-chain overflow spelling, and a float
			// mid-chain (boxed tail replay) — all against the interpreter's spelling
			auto [i1, v1] = run_both_backends(R"(
				var vw = 4;
				var a = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
				int grid(int y, int x) { return a[y * vw + x]; }
				int deep(int y, int x) { return a[y * vw + x + 1]; }
				void put(int y, int x, int v) { a[y * vw + x] = v; }
				var s = 0;
				for (int y = 0; y < 3; ++y) { for (int x = 0; x < 4; ++x) { s += grid(y, x); } }
				var d = deep(1, 1);
				put(2, 1, 99);
				var out = `${s}:${d}:${a[9]}`;
				var big = 9223372036854775807;
				try { var q = a[big * 2 + 1]; out = `${out}|ran`; } catch (er) { out = `${out}|${er}`; }
				try { var q2 = a[1 * 2 + big]; out = `${out}|ran2`; } catch (er) { out = `${out}|${er}`; }
				var f = 1.5;
				var viaFloat = a[1 * 2 + 1] + a[0 * 2 + 0];
				try { var q3 = a[1 * 2 + f]; out = `${out}|ranF:${q3}`; } catch (er) { out = `${out}|${er}`; }
				`${out}:${viaFloat}`;
			)");
			check_eq(i1, v1, "index-chain parity (raw fold + boxed tail + spellings)");
			check(i1.rfind("66:", 0) == 0, "3x4 grid sum through the 2-level chain");
		});

		test("fused_binary_index_container_snapshot_order", [this, run_both_backends]() {
			// An operator overload inside the index expression reassigns the container
			// variable: the unfused twin had already LOADed the container, so the op must
			// index the ORIGINAL array (read) and store into it (write) — the snapshot
			// discipline, observable as the mutation landing in the orphaned array
			auto [i1, v1] = run_both_backends(R"(
				var a = [10, 20, 30];
				class K {
					int k = 1;
					function operator+(var o) -> int { a = [77, 88, 99]; return k + o; }
				}
				var idx = K();
				var got = a[idx + 0];
				var after = a[1];
				var b = [1, 2, 3, 4];
				class M {
					function operator+(var o) -> int { b = [0, 0, 0, 0]; return 1 + o; }
				}
				var mi = M();
				b[mi + 1] = 55;
				`${got}:${after}:${b[0]}:${b[2]}`;
			)");
			check_eq(i1, v1, "container-before-index order parity under reassigning overloads");
			check(i1.rfind("20:", 0) == 0, "read indexed the pre-reassignment array");
		});

		// ---- parse-time type proofs, vm consumers ----
		test("proof_vm_lanes_equal_with_proofs_on_and_off", [this]() {
			// Every vm lane (V-S1/S2/S3, V-D1, V-C1, V-R1 in all three return forms, V-M1/M2,
			// V-I1 plain + fused operand) against a proofs-off twin, three passes: first
			// execute -> field proofs stamped -> identical-reload hit; plus mismatching
			// stores that must take the ladder behind a stamped/frozen proof.
			auto on = vm_engine();
			auto off = vm_engine();
			off->type_proofs(false);
			jai::stdlib::register_all(on);
			jai::stdlib::register_all(off);
			auto outcome = [](jai::engine& e, const char* src) -> std::string {
				try { return "VALUE " + e.execute(src).to_string(); }
				catch (const std::exception& ex) { return std::string("ERROR ") + ex.what(); }
			};
			const char* unit =
				"class P {\n"
				"  int hp = 10; float x = 1.5; string tag = \"p\"; bool alive = true; char c = 'a';\n"
				"  int tick(int d) {\n"
				"    hp = hp - d; this.hp = this.hp + 1; int local = hp;\n"
				"    x = x * 2.0; tag = tag + \"!\"; alive = !alive; c = 'b';\n"
				"    for (int i = 0; i < hp; i = i + 1) { local = local + i; }\n"
				"    local += hp; return local; }\n"
				"  int sum(int a, int b) { return a + b; }\n"
				"  int viaCall(int v) { return sum(v, hp); }\n"
				"  string label(string s) { string out = s + tag; return out; } }\n"
				"int helper(int v, string s) { return v + 1; }\n"
				"P p = P(); int r = p.tick(3); r = r + p.viaCall(2); string t = p.label(\"L\");\n"
				"int k = helper(r, t); p.hp = k; p.x = 0.5;\n"
				"to_string(r) + \":\" + to_string(k) + \":\" + to_string(p.hp) + \":\" + type_of(p.x) + \":\" + p.tag + \":\" + to_string(p.alive) + \":\" + to_string(p.c)";
			const char* use = "r = p.tick(1); p.hp = r; to_string(p.hp) + \":\" + type_of(p.hp) + \":\" + t + \":\" + to_string(p.viaCall(1))";
			const char* mismatch = "p.hp = 2.5; r = 7.5; to_string(p.hp) + \":\" + type_of(r) + \":\" + to_string(r)";
			std::string first;
			for (int pass = 0; pass < 3; ++pass) {
				const std::string got = outcome(*on, unit);
				check_eq(outcome(*off, unit), got, "unit pass " + std::to_string(pass));
				if (pass == 0) { first = got; } else { check_eq(first, got); }
				check_eq(outcome(*off, use), outcome(*on, use), "use pass " + std::to_string(pass));
				check_eq(outcome(*off, mismatch), outcome(*on, mismatch), "mismatch pass " + std::to_string(pass));
			}
			check(first.rfind("VALUE ", 0) == 0, "the battery runs to a value: " + first);

			// Module pattern: the class unit runs twice BEFORE its first use, so its method
			// bodies carry field proofs when their chunks first compile (the store flags
			// freeze with the same_type bits set) and the users' first check sees the
			// engine class; covers the string/bool slot stores and the object return too.
			const char* cls =
				"class Q {\n"
				"  int f = 10; string tag = \"q\"; bool alive = true;\n"
				"  int tick(int d) {\n"
				"    f = f - d; this.f = this.f + 1; int local = f;\n"
				"    tag = tag + \"!\"; string out = tag; out = out + \"?\"; if (out == \"\") { local = 0; }\n"
				"    bool ok = alive; ok = !ok; alive = ok;\n"
				"    return sum(local, f); }\n"
				"  int sum(int a, int b) { return a + b; }\n"
				"  int get() { return f; }\n"
				"  Q dup() { Q q = Q(); q.f = f; q.tag = tag; return q; } }";
			const char* users =
				"Q q = Q(); int r2 = q.tick(3); Q d = q.dup(); d.f = r2; d.tag = q.tag;\n"
				"to_string(r2) + \":\" + to_string(q.f) + \":\" + to_string(d.f) + \":\" + d.tag + \":\" + to_string(q.alive) + \":\" + to_string(q.dup().f) + \":\" + to_string(q.get())";
			const char* users_mismatch = "q.f = 2.5; q.tag = 7; d.tag = q.tag; to_string(q.f) + \":\" + to_string(d.f)";
			// three: first execute -> stamped then full-path first redefinition -> re-stamped
			for (int pass = 0; pass < 3; ++pass) {
				check_eq(outcome(*off, cls), outcome(*on, cls), "cls pass " + std::to_string(pass));
			}
			std::string module_first;
			for (int pass = 0; pass < 3; ++pass) {
				const std::string got = outcome(*on, users);
				check_eq(outcome(*off, users), got, "users pass " + std::to_string(pass));
				if (pass == 0) { module_first = got; } else { check_eq(module_first, got); }
				check_eq(outcome(*off, users_mismatch), outcome(*on, users_mismatch), "users_mismatch pass " + std::to_string(pass));
			}
			check_eq(std::string("VALUE 16:8:16:q!:false:8:8"), module_first);
		});

		test("proof_carriers_survive_recompile_and_frozen_flags", [this]() {
			// Instruction-borne proof bits (store_flag_same_type, return bits) freeze at first
			// compile; the cached chunk is reused across type_proofs toggles and across a
			// callee retype that makes a frozen same_type store meet a string payload - the
			// payload guards keep every outcome equal to the never-toggled twins.
			auto outcome = [](jai::engine& e, const char* src) -> std::string {
				try { return "VALUE " + e.execute(src).to_string(); }
				catch (const std::exception& ex) { return std::string("ERROR ") + ex.what(); }
			};
			auto off_then_on = vm_engine();
			off_then_on->type_proofs(false);
			auto on_then_off = vm_engine();
			auto ref_off = vm_engine();
			ref_off->type_proofs(false);
			auto ref_on = vm_engine();
			const char* defs = "int f(int n) { int m = n * 2; return m; }\nint g(int a, int b) { return a + b; }\nint x = 0; float y = 1.5;";
			const char* use = "x = f(3); x = g(x, 4); x = x + 1; y = y * 2.0; x = true; to_string(x) + \":\" + type_of(x) + \":\" + to_string(y)";
			const char* retype = "string f(int n) { return \"s\"; }";
			for (auto* e : {off_then_on.get(), on_then_off.get(), ref_off.get(), ref_on.get()}) {
				jai::stdlib::register_all(*e);
				check(outcome(*e, defs).rfind("VALUE", 0) == 0, "defs run clean");
			}
			const std::string expect = outcome(*ref_off, use);
			check(expect.rfind("VALUE ", 0) == 0, expect);
			check_eq(expect, outcome(*ref_on, use));
			check_eq(expect, outcome(*off_then_on, use));   // chunk compiled without proof bits
			check_eq(expect, outcome(*on_then_off, use));   // chunk compiled with proof bits
			off_then_on->type_proofs(true);
			on_then_off->type_proofs(false);
			for (int pass = 0; pass < 2; ++pass) {
				check_eq(outcome(*ref_off, use), outcome(*off_then_on, use), "off->on reuse");
				check_eq(outcome(*ref_on, use), outcome(*on_then_off, use), "on->off reuse");
			}
			for (auto* e : {off_then_on.get(), on_then_off.get(), ref_off.get(), ref_on.get()}) {
				(void)outcome(*e, retype);
			}
			for (int pass = 0; pass < 2; ++pass) {
				const std::string after = outcome(*ref_off, use);
				check_eq(after, outcome(*ref_on, use), "retype twin");
				check_eq(after, outcome(*off_then_on, use), "retype off->on");
				check_eq(after, outcome(*on_then_off, use), "retype on->off");
			}
		});

		test("proof_ident_nodes_and_call_node_fill", [this]() {
			// Carrier fill: env-resolved identifiers pin their node per ip (top level, and a
			// global read inside a body - as a plain load AND as a fused operand); slotted
			// params/locals leave none; call sites pin their call_expr.
			auto e = vm_engine();
			auto* symbolizer = detail::engine_access::get_symbolizer(*e);
			std::string script = "var g = 1; int f(int n) { int m = n + g; return h(m, g); } g = g + 1; f(g);";
			auto templates = detail::engine_access::get_registered_template_types(*e);
			jai::lexer lex(script, symbolizer);
			auto tokens = lex.tokenize();
			jai::parser p(tokens, symbolizer, e.get(), templates);
			auto parse_result = p.parse();
			check_true(parse_result.has_value(), "snippet parses");
			jai::vm::vm_compiler compiler(symbolizer);
			auto program = compiler.compile_program(parse_result.value());
			check_true(program != nullptr);
			auto name_of = [](const jai::vm::chunk& c, uint32_t n) -> std::string {
				if (n == jai::vm::k_invalid_u32) return "";
				const auto* node = c.nodes[n].get();
				return node->get_type() == node_type::identifier_expr
					? std::string(static_cast<const identifier_expr*>(node)->name) : std::string("<not-ident>");
			};
			auto table_names = [&](const jai::vm::chunk& c) {
				std::vector<std::string> out;
				for (uint32_t n : c.ident_nodes) { if (n != jai::vm::k_invalid_u32) out.push_back(name_of(c, n)); }
				return out;
			};
			auto operand_names = [&](const jai::vm::chunk& c) {
				std::vector<std::string> out;
				for (const auto& fp : c.fused_binary_protos) {
					if (fp.left.ident_node != jai::vm::k_invalid_u32) out.push_back(name_of(c, fp.left.ident_node));
					if (fp.right.ident_node != jai::vm::k_invalid_u32) out.push_back(name_of(c, fp.right.ident_node));
				}
				return out;
			};
			auto count = [](const std::vector<std::string>& v, const char* s) {
				return std::count(v.begin(), v.end(), std::string(s));
			};
			auto pinned_call = [](const jai::vm::chunk& c) {
				bool any = false;
				for (const auto& site : c.call_sites) {
					if (site.call_node == jai::vm::k_invalid_u32) continue;
					if (c.nodes[site.call_node]->get_type() != node_type::call_expr) return false;
					any = true;
				}
				return any;
			};
			check_true(program->ident_nodes.size() <= program->code.size(), "table never outgrows code");
			const auto top = table_names(*program);
			check_true(count(top, "g") >= 2, "top-level store + arg load of g pinned");
			check_true(count(top, "<not-ident>") == 0);
			check_true(pinned_call(*program), "top-level call site pins its call_expr");

			std::shared_ptr<function_decl> fn;
			for (const auto& d : parse_result.value()) {
				if (d->get_type() == node_type::function_decl) { fn = std::static_pointer_cast<function_decl>(d); }
			}
			check_true(fn != nullptr, "function_decl found");
			auto body = compiler.compile_callable(fn->name, fn->parameters, fn->body, fn->local_count);
			check_true(body != nullptr);
			const auto body_table = table_names(*body);
			const auto body_operands = operand_names(*body);
			check_true(count(body_table, "g") == 1, "plain load of the global inside the body pinned");
			check_true(count(body_operands, "g") == 1, "fused operand read of the global pinned");
			check_true(count(body_table, "n") == 0 && count(body_table, "m") == 0 &&
			           count(body_operands, "n") == 0 && count(body_operands, "m") == 0,
			           "slotted param/local leave no entry");
			check_true(pinned_call(*body), "body call site pins its call_expr");
		});
		test("lanes_read_a_getter_backed_reference_once", [this]() {
			// A binder property with a getter and no setter bound by reference (a var& parameter,
			// an auto& local): every deref runs the getter, which the host can count. A lane
			// that reads it and then misses (out of range, a mismatched kind) would run it
			// again on the general path; the lanes step aside for such references, so the
			// count is the interpreter's: one per read (a getter-boundary probe)
			struct probe_box { script_int reads = 0; std::vector<script_int> values{10, 20}; };
			for (bool proofs : {true, false}) {
				for (int backend = 0; backend < 2; ++backend) {
					probe_box box;
					std::shared_ptr<jai::engine> e = backend == 0 ? interp_engine() : vm_engine();
					e->type_proofs(proofs);
					jai::stdlib::register_all(*e);
					jai::dynamic_binder<probe_box>(*e, "probe_box")
						.property("values", [](probe_box& b) { ++b.reads; return b.values; }, nullptr)
						.build();
					e->add_global_ref("box", box);
					e->add_function("clear_reads", [&box]() { box.reads = 0; });
					auto run = [&](const char* src) -> std::string {
						std::string got;
						try { got = std::to_string(e->execute(src).as_int()); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
						return got + "|reads=" + std::to_string(box.reads);
					};
					const std::string label = std::string(backend ? "vm" : "interp") + (proofs ? " proofs" : " no-proofs");
					check_eq(std::string("10|reads=1"), run("int read(array<int>& xs, int i) { clear_reads(); int v = xs[i]; return v; } read(box.values, 0);"), label + " decl in range");
					{ const std::string got = run("int read(array<int>& xs, int i) { clear_reads(); int v = xs[i]; return v; } read(box.values, 2);"); check_true(got.rfind("throw:Array index 2 out of bounds for array of size 2", 0) == 0 && got.find("|reads=1") != std::string::npos, label + " decl out of range: " + got); }
					check_eq(std::string("20|reads=1"), run("int read(int i) { auto& xs = box.values; clear_reads(); int v = xs[i]; return v; } read(1);"), label + " local ref");
					{ const std::string got = run("int read(int i) { auto& xs = box.values; clear_reads(); int v = xs[i]; return v; } read(-1);"); check_true(got.rfind("throw:Array index -1 out of bounds for array of size 2", 0) == 0 && got.find("|reads=1") != std::string::npos, label + " local ref out of range: " + got); }
					check_eq(std::string("1|reads=1"), run("int read(array<int>& xs, int i) { clear_reads(); var y = xs; return 1; } read(box.values, 0);"), label + " move from the reference");
					check_eq(std::string("1|reads=1"), run("int read(array<int>& xs) { clear_reads(); var a = []; a.push(xs); return 1; } read(box.values);"), label + " push of the reference");
				}
			}
		});

		test("push_of_a_table_element_matches_the_interpreter", [this]() {
			// `recv.push(table[i])`: the vm reads the element raw and appends it by the receiver's
			// kind; every table kind (int, float, string, hetero, a global, a var& parameter, a
			// literal index) into every receiver kind (var, array<int>, array<float>, array<string>)
			// lands what the interpreter lands, and a miss (out of range, a float into array<int>,
			// a string into array<int>, a hetero element that is not a plain scalar) keeps the text
			const char* src = R"(
				array<int> ti = [1, 2, 3]; array<float> tf = [0.5, 1.5]; array<string> ts = ["a", "b"];
				var th = [7, 2.5, "s", true, [1]];
				var out = [];
				function run(string label, function body) { try { out.push(label + ":" + to_string(body())); } catch (err) { out.push(label + "!" + err); } }
				run("var<-int", function() -> int { var r = []; int i = 1; r.push(ti[i]); r.push(ti[0]); return r.size() * 100 + r[0] + r[1]; });
				run("var<-float", function() -> float { var r = []; int i = 1; r.push(tf[i]); return r[0]; });
				run("var<-str", function() -> string { var r = []; int i = 1; r.push(ts[i]); r.push(ts[0]); return r[0] + r[1]; });
				run("var<-hetero", function() -> string { var r = []; int i = 0; r.push(th[i]); i = 2; r.push(th[i]); i = 3; r.push(th[i]); return to_string(r[0]) + r[1] + to_string(r[2]); });
				run("var<-hetero-array", function() -> int { var r = []; int i = 4; r.push(th[i]); return r[0][0]; });
				run("int<-int", function() -> int { array<int> r = []; int i = 2; r.push(ti[i]); return r[0]; });
				run("int<-float", function() -> int { array<int> r = []; int i = 0; [[warn_mode]] r.push(tf[i]); return r[0]; });
				run("int<-str", function() -> int { array<int> r = []; int i = 0; [[warn_mode]] r.push(ts[i]); return r[0]; });
				run("float<-int", function() -> float { array<float> r = []; int i = 2; r.push(ti[i]); return r[0]; });
				run("float<-float", function() -> float { array<float> r = []; int i = 1; r.push(tf[i]); return r[0]; });
				run("str<-str", function() -> string { array<string> r = []; int i = 0; r.push(ts[i]); return r[0]; });
				run("str<-int", function() -> string { array<string> r = []; int i = 0; [[warn_mode]] r.push(ti[i]); return r[0]; });
				run("oob", function() -> int { var r = []; int i = 9; r.push(ti[i]); return r.size(); });
				run("neg", function() -> int { var r = []; int i = -1; r.push(ti[i]); return r.size(); });
				run("ref-table", function() -> int { var& t = ti; var r = []; int i = 0; r.push(t[i]); return r[0]; });
				run("param-table", function() -> int { int f(array<int>& t) { var r = []; r.push(t[1]); return r[0]; } return f(ti); });
				run("float-index", function() -> int { var r = []; var i = 1.0; r.push(ti[i]); return r.size(); });
				run("string-shares", function() -> string { var r = []; int i = 0; r.push(ts[i]); ts[0] = "z"; return r[0] + ts[0]; });
				return out;
			)";
			auto interp = interp_engine();
			auto vmx = vm_engine();
			jai::stdlib::register_all(*interp);
			jai::stdlib::register_all(*vmx);
			const auto a = interp->execute(src).as_array();
			const auto b = vmx->execute(src).as_array();
			check_eq(a.size(), b.size());
			for (size_t i = 0; i < a.size() && i < b.size(); ++i) { check_eq(a[i].as_string(), b[i].as_string(), "row " + std::to_string(i)); }
			check_eq(std::string("var<-int:203"), a[0].as_string());
			check_eq(std::string("int<-float!Cannot push 'float' to array<int>"), a[6].as_string());
			check_eq(std::string("oob!Array index 9 out of bounds for array of size 3"), a[12].as_string());
		});

		test("typed_scalar_ops_match_the_interpreter", [this]() {
			// The typed scalar set (op_t_*): arithmetic into temps, decls and stores, the
			// compare-jumps, and the four moves, each with its guard misses (overflow, zero
			// divisor, shift range, a boxed slot behind a reference, a var slot, widening,
			// strings through the moves) against the interpreter with proofs on and off
			const char* cases[] = {
				"string run() { int acc = 0; for (int i = 0; i < 40; ++i) { int xx = i & 7; int t = xx * 3 + 1; int u = t - i; int v = u / 2; int w = v % 5; int b = (w ^ 3) | (i & 1); int sh = b << 2; int sr = sh >> 1; acc = acc + sr; } return to_string(acc); } run();",
				"string run() { float acc = 0.0; float k = 0.5; for (int i = 0; i < 40; ++i) { float f = k * 2.0; float g = f - 0.25; float h = g / 4.0; float m = h % 0.3; acc = acc + m; } return to_string(acc); } run();",
				"string run() { int x = 0x7fffffffffffffff; int one = 1; string m = \"ok\"; try { int y = x + one; } catch (e) { m = e; } return m; } run();",
				"string run() { int x = 0x7fffffffffffffff; int two = 2; int y = 0; string m = \"ok\"; try { y = x * two; } catch (e) { m = e; } return m + to_string(y); } run();",
				"string run() { int x = 5; int z = 0; string m = \"ok\"; try { int y = x / z; } catch (e) { m = e; } try { int q = x % z; } catch (e) { m += e; } return m; } run();",
				"string run() { int x = 5; int n = 70; int k = -1; string m = \"ok\"; try { int y = x << n; } catch (e) { m = e; } try { int q = x >> k; } catch (e) { m += e; } return m; } run();",
				"string run() { float x = 1.5; float z = 0.0; string m = \"ok\"; try { float y = x / z; } catch (e) { m = e; } try { float q = x % z; } catch (e) { m += e; } return m; } run();",
				"string run() { int x = 1; int& r = x; int a = 2; int b = 3; x = a + b; int c = x * a; r = c - 1; return to_string(x) + to_string(r) + to_string(c); } run();",
				"string run() { int x = 4; var v = x + 1; var w = v; var f = 2.5; var g = f * 2.0; return type_of(v) + to_string(v) + type_of(w) + type_of(g) + to_string(g); } run();",
				"string run() { int k = 3; float f = k; float g = 7; auto a = k; auto b = f; return type_of(f) + to_string(f) + to_string(g) + type_of(a) + type_of(b); } run();",
				"string run() { string s = \"a\"; string t = s; t += \"b\"; string u = \"lit\"; string w = u; s = t; u = \"x\"; return s + t + u + w; } run();",
				"string run() { bool b = true; bool c = b; char ch = 'q'; char d = ch; int i = 7; int j = i; j = 9; i = j; return to_string(b) + to_string(c) + ch + d + to_string(i) + to_string(j); } run();",
				"string run() { int n = 0; int acc = 0; while (n < 10) { acc = acc + n; n = n + 1; } int m = 10; while (m > 0) { m = m - 3; acc = acc + m; } return to_string(acc); } run();",
				"string run() { float x = 0.0; int hits = 0; while (x < 5.0) { x = x + 0.75; if (x >= 2.0) { hits = hits + 1; } if (x <= 1.0) { hits = hits + 10; } if (x == 3.0) { hits = hits + 100; } if (x != 3.75) { hits = hits + 1000; } } return to_string(hits) + to_string(x); } run();",
				"string run() { int a = 3; int b = 3; int r = 0; if (a == b) { r = r + 1; } if (a != b) { r = r + 2; } if (a >= b) { r = r + 4; } if (a <= b) { r = r + 8; } if (2 < a) { r = r + 16; } if (5 > a) { r = r + 32; } int c = 2 * a; int d = 10 - a; return to_string(r) + to_string(c) + to_string(d); } run();",
				"string run() { int x = 5; int y = 0; y = x; y = 7; int z = y; float f = 1.5; float g = 0.0; g = f; g = 2.5; return to_string(x) + to_string(y) + to_string(z) + to_string(f) + to_string(g); } run();",
				"class P { int n = 0; } string run() { P p = P(); var h = p; int k = 2; var q = k; q = k + 1; return to_string(q) + type_of(h); } run();",
				"string run() { int total = 0; for (int i = 0; i < 3; ++i) { int s = 0; for (int j = 0; j < 4; ++j) { int t = i * j; s = s + t; } total = total + s; } return to_string(total); } run();",
				"int g = 10; string run() { int x = g; int y = x + g; g = y; return to_string(g) + to_string(x); } run();",
				"string run() { int x = 1; var f = [&]() { x = x + 5; }; f(); int y = x * 2; return to_string(x) + to_string(y); } run();",
				"string run() { int i = 0; int acc = 0; for (i = 0; i < 6; i = i + 2) { acc = acc + i; } return to_string(acc) + to_string(i); } run();",
				// compound rows: in-place int updates, overflow, zero divisor, %= and floats stay general, ++/-- desugar, a boxed target
				"string run() { int acc = 0; int k = 3; for (int i = 0; i < 10; ++i) { acc += k; acc -= 1; acc *= 2; acc /= 3; ++acc; acc++; --acc; } return to_string(acc); } run();",
				"string run() { int x = 0x7fffffffffffffff; int one = 1; string m = \"ok\"; try { x += one; } catch (e) { m = e; } int y = 5; int z = 0; try { y /= z; } catch (e) { m += \"|\" + e; } return m + to_string(x) + to_string(y); } run();",
				"string run() { int x = 17; int m3 = 3; x %= m3; float f = 1.5; f += 2.5; f *= 2.0; var v = 4; v += 3; v *= 2; return to_string(x) + to_string(f) + to_string(v) + type_of(v); } run();",
				"string run() { int x = 1; int& r = x; int d = 4; x += d; r += 1; x *= 2; return to_string(x) + to_string(r); } run();",
				"string run() { int x = 2; var s = \"a\"; string m = \"ok\"; try { x += s; } catch (e) { m = e; } var w = \"b\"; w += \"c\"; return m + to_string(x) + w; } run();",
				// typed push rows: slot receiver with literal and slot values, every buffer kind, a declined payload, a null receiver
				"string run() { var parts = []; string s = \"x\"; int k = 3; float f = 1.5; for (int i = 0; i < 4; ++i) { parts.push(\" \"); parts.push(s); parts.push(k); parts.push(f); parts.push(true); parts.push(null); } return to_string(parts.size()) + parts.join(\"|\"); } run();",
				"string run() { array<int> a = []; array<float> b = []; array<string> c = []; int k = 4; float g = 2.5; string t = \"t\"; a.push(k); a.push(7); b.push(k); b.push(g); c.push(t); c.push(\"u\"); string m = \"ok\"; try { a.push(g); } catch (e) { m = e; } try { c.push(k); } catch (e) { m += \"|\" + e; } return to_string(a[0] + a[1]) + to_string(b[0] + b[1]) + c[0] + c[1] + m; } run();",
				"class N { int v = 1; } string run() { array<N> ns = []; N one = N(); ns.push(one); one.v = 9; var xs = []; var row = [1]; xs.push(row); row.push(2); return to_string(ns[0].v) + to_string(xs[0].size()); } run();",
				"string run() { var d; string m = \"ok\"; try { d.push(1); } catch (e) { m = e; } var s = \"str\"; try { s.push(1); } catch (e) { m += \"|\" + e; } return m; } run();",
				"string run() { var a = []; string lit = \"k\"; a.push(lit); a.push(\"k\"); a[0] += \"1\"; a[1] += \"2\"; return lit + a[0] + a[1] + \"k\"; } run();",
			};
			for (bool proofs : {true, false}) {
				for (const char* program : cases) {
					auto interp = interp_engine();
					jai::stdlib::register_all(*interp);
					interp->type_proofs(proofs);
					auto vmx = vm_engine();
					jai::stdlib::register_all(*vmx);
					vmx->type_proofs(proofs);
					std::string want, got;
					try { want = interp->execute(program).as_string(); } catch (const std::exception& e) { want = std::string("throw:") + e.what(); }
					try { got = vmx->execute(program).as_string(); } catch (const std::exception& e) { got = std::string("throw:") + e.what(); }
					check_eq(want, got, program);
				}
			}
			// a registered global numeric operator reaches typed slots too: every typed row
			// steps aside for the operator table, exactly as the fused kernels do
			for (bool proofs : {true, false}) {
				const char* program = "string run() { int a = 3; int b = 4; int c = a + b; int d = 0; d = a * b; d += a; int acc = 0; for (int i = 0; i < 3; ++i) { acc = acc + i; } return to_string(c) + \",\" + to_string(d) + \",\" + to_string(acc); } run();";
				auto interp = interp_engine();
				jai::stdlib::register_all(*interp);
				interp->type_proofs(proofs);
				interp->add_function("+", [](script_int x, script_int y) { return x * 10 + y; });
				auto vmx = vm_engine();
				jai::stdlib::register_all(*vmx);
				vmx->type_proofs(proofs);
				vmx->add_function("+", [](script_int x, script_int y) { return x * 10 + y; });
				std::string want, got;
				try { want = interp->execute(program).as_string(); } catch (const std::exception& e) { want = std::string("throw:") + e.what(); }
				try { got = vmx->execute(program).as_string(); } catch (const std::exception& e) { got = std::string("throw:") + e.what(); }
				check_eq(want, got, program);
				check_true(got.rfind("7,", 0) != 0, "the typed decl did not bypass the operator table: " + got);   // the general path answers here (today: a resolution error on both backends), never the raw sum
			}
			// the override can arrive between execute calls (typed ops read the flag per
			// execution) or from a native callback while the function runs; every timing agrees
			// with the interpreter (an operator-transition matrix)
			for (bool proofs : {true, false}) {
				const char* define = "int calc(int a, int b) { int c = a + b; int d = (a + b) * 10; return c * 1000 + d; } function later() { install(); return calc(1, 1); }";
				auto make = [&](bool interp) {
					std::shared_ptr<jai::engine> e = interp ? interp_engine() : vm_engine();
					jai::stdlib::register_all(*e);
					e->type_proofs(proofs);
					std::weak_ptr<jai::engine> weak = e;
					e->add_function("install", [weak]() { if (auto eng = weak.lock()) { eng->add_function("+", [](script_int x, script_int y) { return x * y; }); } });
					e->execute(define);
					return e;
				};
				auto interp = make(true);
				auto vmx = make(false);
				auto run = [](std::shared_ptr<jai::engine>& e, const char* src) {
					try { return std::to_string(e->execute(src).as_int()); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
				};
				check_eq(run(interp, "calc(1, 1);"), run(vmx, "calc(1, 1);"), "before the install");
				check_eq(std::string("2020"), run(vmx, "calc(1, 1);"), "plain sum before the install");
				check_eq(run(interp, "later();"), run(vmx, "later();"), "installed from a native callback mid-execution");
				check_eq(run(interp, "calc(1, 1);"), run(vmx, "calc(1, 1);"), "after the install, a later execute");
				check_true(run(vmx, "calc(1, 1);") != "2020", "the typed slots see the installed operator: " + run(vmx, "calc(1, 1);"));
			}
			// op_t_load: the decl read tests' shapes again through the flattened site, and the
			// lowering probe (a closure body with a proven element read lists T_LOAD)
			{
				auto vm = vm_engine();
				jai::stdlib::register_all(*vm);
				auto* symbolizer = detail::engine_access::get_symbolizer(*vm);
				const char* script = "array<int> G = [1, 2, 3, 4]; var f = function(int w) { int y = 1; int x = 0; int v = G[y * w + x]; int q = G[x]; return v + q; }; var z = 0;";
				auto templates = detail::engine_access::get_registered_template_types(*vm);
				jai::lexer lex(script, symbolizer);
				auto tokens = lex.tokenize();
				jai::parser p(tokens, symbolizer, vm.get(), templates);
				auto parse_result = p.parse();
				check_true(parse_result.has_value(), "snippet parses");
				jai::vm::vm_compiler compiler(symbolizer);
				const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
				check_true(listing.find("T_LOAD") != std::string::npos, listing);
			}
			// the lowering happens: a closure body with typed slots lists the typed ops (closure
			// bodies compile with the program; the checker's stamps come from a checked parse)
			{
				auto vm = vm_engine();
				jai::stdlib::register_all(*vm);
				auto* symbolizer = detail::engine_access::get_symbolizer(*vm);
				const char* script = "var f = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { int t = i * 2; acc = acc + t; int c = 5; int d = c; } return acc; }; var z = 0;";
				auto templates = detail::engine_access::get_registered_template_types(*vm);
				jai::lexer lex(script, symbolizer);
				auto tokens = lex.tokenize();
				jai::parser p(tokens, symbolizer, vm.get(), templates);
				auto parse_result = p.parse();
				check_true(parse_result.has_value(), "snippet parses");
				jai::vm::vm_compiler compiler(symbolizer);
				const std::string listing = jai::vm::disassemble(*compiler.compile_program(parse_result.value()), symbolizer);
				check_true(listing.find("T_DECL_CONST") != std::string::npos && listing.find("T_DECL_MOV") != std::string::npos, listing);
			}
		});

		test("member_compound_lands_through_the_field_write_cache", [this]() {
			// `obj.f op= v` and `this.f op= v` write back through the same proven-field and
			// site-IC landing op_set_member uses; accessors, non-public chains, statics,
			// mismatches and overflow keep the general ladder and its texts
			const char* cases[] = {
				"class P { int x = 1; float f = 0.5; string s = \"a\"; var v = 2; } string run() { P p = P(); int k = 3; p.x += k; p.x -= 1; p.x *= 4; p.f += k; p.f /= 2; p.s += \"b\"; p.v += 1.5; p.v *= 2; return to_string(p.x) + \",\" + to_string(p.f) + \",\" + p.s + \",\" + to_string(p.v) + type_of(p.v); } run();",
				"class P { int x = 1; void bump(int d) { this.x += d; x += d; this.x *= 2; } } string run() { P p = P(); for (int i = 0; i < 4; ++i) { p.bump(i); } return to_string(p.x); } run();",
				"class P { int x = 7; } string run() { P p = P(); int a = (p.x += 3); int b = p.x++; int c = ++p.x; int d = (p.x %= 4); return to_string(a) + to_string(b) + to_string(c) + to_string(d) + to_string(p.x); } run();",
				"class P { int x = 1; string m = \"ok\"; } string run() { P p = P(); try { p.x += \"s\"; } catch (e) { p.m = e; } return p.m + to_string(p.x); } run();",
				"class P { int x = 0x7fffffffffffffff; } string run() { P p = P(); string m = \"ok\"; try { p.x += 1; } catch (e) { m = e; } return m + to_string(p.x); } run();",
				"class P { int x = 1; int z = 0; } string run() { P p = P(); string m = \"ok\"; try { p.x /= p.z; } catch (e) { m = e; } return m; } run();",
				"class A { int x = 1; int _set_x(int v) { x = v * 10; return v; } int _get_x() { return x; } } string run() { A a = A(); a.x += 2; return to_string(a.x); } run();",
				"class S { static int n = 5; } string run() { S::n += 3; S s = S(); return to_string(S::n); } run();",
				"class P { int x = 1; } string run() { P p = P(); var& r = p; r.x += 5; shared_ptr<P> h = new P(); h.x += 6; return to_string(p.x) + to_string(h.x); } run();",
				"class P { float f = 1.0; } string run() { P p = P(); int k = 2; p.f += k; p.f *= 0.5; return to_string(p.f) + type_of(p.f); } run();",
				"class Q { int n = 0; } string run() { var q = Q(); q.n += 1; var w; string m = \"ok\"; try { w.n += 1; } catch (e) { m = e; } return to_string(q.n) + m; } run();",
			};
			for (bool proofs : {true, false}) {
				for (const char* program : cases) {
					auto interp = interp_engine();
					jai::stdlib::register_all(*interp);
					interp->type_proofs(proofs);
					auto vmx = vm_engine();
					jai::stdlib::register_all(*vmx);
					vmx->type_proofs(proofs);
					std::string want, got;
					try { want = interp->execute(program).as_string(); } catch (const std::exception& e) { want = std::string("throw:") + e.what(); }
					try { got = vmx->execute(program).as_string(); } catch (const std::exception& e) { got = std::string("throw:") + e.what(); }
					check_eq(want, got, program);
				}
			}
		});

		test("typed_index_store_lane_matches_the_interpreter", [this]() {
			// `arr[index] = v` with a proven shape stores through typed_index_store; a float
			// outside the int range, a string, a typed hetero element, an object, out of
			// bounds, an overflowing fold and the expression-result forms all run the kernel:
			// same post-state and texts as the interpreter with proofs on and off
			const char* cases[] = {
				"array<int> PIX = [0, 0, 0, 0, 0, 0, 0, 0]; string run() { int vw = 2; for (int ry = 0; ry < 2; ++ry) { int itop = ry * 2 * vw; for (int cx = 0; cx < 1; ++cx) { int xx = cx * 2; int c = ry * 10 + cx; PIX[itop + xx] = c; PIX[itop + xx + 1] = c + 1; PIX[itop + vw + xx] = 2.9; PIX[itop + vw + xx + 1] = -0.5; } } var s = \"\"; for (auto v : PIX) { s += to_string(v) + \",\"; } return s; } run();",
				"array<float> F = [0.0, 0.0, 0.0]; string run() { int i = 1; F[i] = 3; F[i + 1] = 2.5; F[0] = -1; return to_string(F[0]) + \",\" + to_string(F[1]) + \",\" + to_string(F[2]) + type_of(F[1]); } run();",
				"array<int> A = [1, 2, 3]; string run() { int i = 1; float big = 1e100; string m = \"ok\"; try { A[i] = big; } catch (e) { m = e; } return m + to_string(A[1]); } run();",
				"array<int> A = [1, 2, 3]; string run() { int i = 1; string m = \"ok\"; try { A[i] = \"x\"; } catch (e) { m = e; } return m + to_string(A[1]); } run();",
				"array<int> A = [1, 2, 3]; string run() { int i = 7; string m = \"ok\"; try { A[i] = 5; } catch (e) { m = e; } return m; } run();",
				"array<int> A = [1, 2, 3]; string run() { int i = -1; string m = \"ok\"; try { A[i + 0] = 5; } catch (e) { m = e; } return m; } run();",
				"array<int> A = [1, 2, 3]; string run() { int big = 0x7fffffffffffffff; string m = \"ok\"; try { A[big + 1] = 5; } catch (e) { m = e; } return m; } run();",
				"var H = [1, 2.5, \"s\"]; string run() { int i = 0; H[i] = 7; H[i + 1] = 1.5; H[2] = 9; H[0] = 2.25; return type_of(H[0]) + to_string(H[0]) + type_of(H[1]) + to_string(H[1]) + type_of(H[2]) + to_string(H[2]); } run();",
				"var H = [1, 2]; string run() { int i = 0; int x = 5; float y = 0.5; H[i] = x; H[i + 1] = y; x = 6; y = 0.25; return to_string(H[0]) + \",\" + to_string(H[1]); } run();",
				"array<int> A = [0, 0, 0]; string run(var& pix) { int i = 1; int c = 4; pix[i] = c; pix[i + 1] = c * 2; return to_string(pix[1] + pix[2]); } run(A) + to_string(A[2]);",
				"array<int> A = [0, 0, 0]; string run() { int i = 1; int x = 2; A[i] = x + 3; A[i + 1] = x * x; return to_string(A[1]) + to_string(A[2]); } run();",
				"array<int> A = [0, 0, 0]; string run() { int i = 1; int r = (A[i] = 5); var q = (A[i + 1] = 2.9); return to_string(r) + to_string(A[1]) + type_of(q) + to_string(q) + to_string(A[2]); } run();",
				"array<int> A = [0, 0, 0]; string run() { int i = 1; int one = 1; int n = 70; string m = \"ok\"; try { A[one << n] = 1; } catch (e) { m = e; } return m + to_string(A[1]); } run();",
				"array<int> A = [0, 0, 0, 0]; string run() { int i = 3; A[i & 1] = 1; A[i >> 1] = 2; A[(i ^ 1) | 0] = 3; return to_string(A[0]) + to_string(A[1]) + to_string(A[2]) + to_string(A[3]); } run();",
				"class G { array<int> pix = [0, 0, 0]; int w = 1; string f() { int i = 1; pix[i] = 4; pix[i + w] = 5; return to_string(pix[1] * 10 + pix[2]); } } string run() { G g = G(); return g.f(); } run();",
				"array<int> A = [0, 0, 0]; string run() { int i = 1; { int i = 2; A[i] = 9; } A[i] = 8; return to_string(A[1]) + to_string(A[2]); } run();",
				"array<int> A = [0, 0]; string run() { int i = 0; string m = \"ok\"; try { A[i] = nope; } catch (e) { m = e; } return m + to_string(A[0]); } run();",
				"string run() { int n = 3; int i = 1; string m = \"ok\"; try { n[i] = 1; } catch (e) { m = e; } return m; } run();",
				"array<bool> B = [false, false]; array<string> S = [\"a\", \"b\"]; string run() { int i = 1; B[i] = true; S[i] = \"z\"; S[0] = 5; return to_string(B[1]) + S[1] + S[0]; } run();",
				"class V { int v = 1; } var A = [V(), V()]; string run() { int i = 0; V nv = V(); nv.v = 7; A[i] = nv; nv.v = 8; return to_string(A[0].v); } run();",
			};
			for (bool proofs : {true, false}) {
				for (const char* program : cases) {
					auto interp = interp_engine();
					jai::stdlib::register_all(*interp);
					interp->type_proofs(proofs);
					auto vmx = vm_engine();
					jai::stdlib::register_all(*vmx);
					vmx->type_proofs(proofs);
					std::string want, got;
					try { want = interp->execute(program).as_string(); } catch (const std::exception& e) { want = std::string("throw:") + e.what(); }
					try { got = vmx->execute(program).as_string(); } catch (const std::exception& e) { got = std::string("throw:") + e.what(); }
					check_eq(want, got, program);
				}
			}
		});

		test("typed_decl_read_lane_matches_the_interpreter", [this]() {
			// `T x = arr[index]` with a proven shape lands through typed_decl_read; every
			// runtime miss (kind, bounds, overflowing fold, shift range, float decl from an
			// int buffer, auto decls keeping the element tag) runs the kernel: same values and
			// texts as the interpreter with proofs on and off
			const char* cases[] = {
				"array<int> PIX = [1, 2, 3, 4, 5, 6, 7, 8]; string run() { int vw = 2; int acc = 0; for (int ry = 0; ry < 2; ++ry) { int itop = ry * 2 * vw; for (int cx = 0; cx < 1; ++cx) { int xx = cx * 2; int p0 = PIX[itop + xx]; int p1 = PIX[itop + xx + 1]; int p2 = PIX[itop + vw + xx]; int p3 = PIX[itop + vw + xx + 1]; acc = acc * 10 + p0 + p1 + p2 + p3; } } return to_string(acc); } run();",
				"array<float> F = [0.5, 1.5, 2.5]; string run() { int i = 1; float a = F[i]; float b = F[i + 1]; var c = F[0]; return to_string(a + b) + type_of(c); } run();",
				"array<int> A = [10, 20, 30]; string run() { int i = 1; float f = A[i]; var v = A[i + 1]; auto a = A[0]; return to_string(f) + type_of(f) + to_string(v) + type_of(a); } run();",
				"var H = [1, 2.5, \"s\", true, 'c']; string run() { int i = 0; var a = H[i]; var b = H[i + 1]; var c = H[i + 2]; var d = H[3]; var e = H[i + 4]; auto f = H[0]; return type_of(a) + type_of(b) + type_of(c) + type_of(d) + type_of(e) + type_of(f) + to_string(a) + c; } run();",
				"var H = [1, \"s\"]; string run() { int i = 1; string m = \"ok\"; try { int x = H[i]; } catch (e) { m = e; } return m; } run();",
				"array<int> A = [1, 2, 3]; string run() { int i = 5; string m = \"ok\"; try { int x = A[i]; } catch (e) { m = e; } return m; } run();",
				"array<int> A = [1, 2, 3]; string run() { int i = -1; string m = \"ok\"; try { int x = A[i + 0]; } catch (e) { m = e; } return m; } run();",
				"array<int> A = [1, 2, 3]; string run() { int big = 0x7fffffffffffffff; string m = \"ok\"; try { int x = A[big + 1]; } catch (e) { m = e; } return m; } run();",
				"array<int> A = [1, 2, 3]; string run() { int big = 0x7fffffffffffffff; int two = 2; string m = \"ok\"; try { int x = A[big * two]; } catch (e) { m = e; } return m; } run();",
				"array<int> A = [1, 2, 3]; string run() { int one = 1; int n = 70; string m = \"ok\"; try { int x = A[one << n]; } catch (e) { m = e; } return m; } run();",
				"array<int> A = [1, 2, 3]; string run() { int z = 0; int i = 1; string m = \"ok\"; try { int x = A[i / z]; } catch (e) { m = e; } return m; } run();",
				"array<int> A = [5, 6, 7, 8]; string run() { int i = 3; int x = A[i & 1]; int y = A[i >> 1]; int z = A[(i ^ 1) | 0]; return to_string(x) + to_string(y) + to_string(z); } run();",
				"array<int> A = [5, 6, 7, 8]; string run(var& pix) { int i = 1; int x = pix[i + 1]; int y = pix[i]; return to_string(x + y); } run(A);",
				"array<int> A = [5, 6, 7, 8]; string run(array<int> pix) { int i = 1; int x = pix[i + 2]; return to_string(x); } run(A);",
				"class G { array<int> pix = [4, 5, 6]; int w = 1; string f() { int i = 1; int p = pix[i]; int q = pix[i + w]; return to_string(p * 10 + q); } } string run() { G g = G(); return g.f(); } run();",
				"array<int> pix = [40, 50, 60]; class G { array<int> pix = [4, 5, 6]; string f() { int i = 1; int p = pix[i]; return to_string(p); } } string run() { G g = G(); int i = 2; int q = pix[i]; return g.f() + to_string(q); } run();",
				"array<int> A = [1, 2, 3]; string run() { int i = 1; { int i = 2; int x = A[i]; if (x != 3) { return \"bad\"; } } int y = A[i]; return to_string(y); } run();",
				"var A = [1, 2, 3]; string run() { int i = 0; int s = 0; for (int k = 0; k < 3; ++k) { int x = A[i + k]; s += x; } return to_string(s); } run();",
				"array<string> S = [\"a\", \"bb\"]; string run() { int i = 1; string x = S[i]; var y = S[i - 1]; x += \"!\"; return x + y + S[i]; } run();",
				"array<bool> B = [true, false]; array<char> C = ['x', 'y']; string run() { int i = 1; bool b = B[i]; char c = C[i]; var d = C[0]; return to_string(b) + c + d + type_of(d); } run();",
				"string run() { var m = {\"k\": 4}; int i = 0; string s = \"ok\"; try { int x = m[i]; } catch (e) { s = e; } var y = m[\"k\"]; return s + to_string(y); } run();",
				"string run() { int n = 3; int i = 1; string s = \"ok\"; try { int x = n[i]; } catch (e) { s = e; } return s; } run();",
				// int -> float widening on every decl landing: LOAD, CONST, fused binary, member, element
				"class Q { int n = 7; } array<int> A = [3, 4]; string run() { int i = 1; int k = 9007199254740993; Q q = Q(); float a = k; float b = 5; float c = i + k; float d = q.n; float e = A[i]; float g = A[i - 1] * 2; return type_of(a) + type_of(b) + type_of(c) + type_of(d) + type_of(e) + type_of(g) + to_string(a) + \",\" + to_string(b) + \",\" + to_string(c) + \",\" + to_string(d) + \",\" + to_string(e) + \",\" + to_string(g); } run();",
				"string run() { int i = 2; float f = i; f += 0.5; int j = i; return to_string(f) + to_string(j) + type_of(f) + type_of(j); } run();",
				"string run() { bool t = true; float f = t; int i = t; return to_string(f) + type_of(f) + to_string(i); } run();",
			};
			for (bool proofs : {true, false}) {
				for (const char* program : cases) {
					auto interp = interp_engine();
					jai::stdlib::register_all(*interp);
					interp->type_proofs(proofs);
					auto vmx = vm_engine();
					jai::stdlib::register_all(*vmx);
					vmx->type_proofs(proofs);
					std::string want, got;
					try { want = interp->execute(program).as_string(); } catch (const std::exception& e) { want = std::string("throw:") + e.what(); }
					try { got = vmx->execute(program).as_string(); } catch (const std::exception& e) { got = std::string("throw:") + e.what(); }
					check_eq(want, got, program);
				}
			}
		});

		test("typed_int_head_bitwise_rows_match_the_interpreter", [this]() {
			// The typed int head computes & | ^ << >> on proven int/int operands; a shift
			// outside 0..63, a char or float operand, or proofs off all run the general kernel
			// with its texts, so every row below agrees with the interpreter both ways
			const char* cases[] = {
				"int tx = 37; int ty = 11; int tone = ((tx >> 2) + (ty >> 2)) & 1; if ((ty & 7) == 7) { tone = 2; } return to_string(tone) + to_string((tx ^ ty) | 64) + to_string(tx << 3);",
				"int a = -8; int b = 1; return to_string(a >> b) + \",\" + to_string(a << 60) + \",\" + to_string(a & 0xff) + \",\" + to_string(a | b) + \",\" + to_string(a ^ -1);",
				"int a = 1; int n = 63; return to_string(a << n) + \",\" + to_string((a << n) >> n);",
				"int a = 1; int n = 64; string m = \"ok\"; try { int r = a << n; } catch (e) { m = e; } return m;",
				"int a = 1; int n = -1; string m = \"ok\"; try { int r = a >> n; } catch (e) { m = e; } return m;",
				"float a = 1.5; int n = 1; string m = \"ok\"; try { [[warn_mode]] var r = a & n; } catch (e) { m = e; } return m;",
				"char c = 'a'; int n = 1; return to_string(c & n) + to_string(c >> n) + to_string(c | 128);",
				"var a = 6; var b = 3; return to_string(a & b) + to_string(a | b) + to_string(a ^ b) + to_string(a << b) + to_string(a >> 1);",
				"int acc = 0; for (int i = 0; i < 40; ++i) { acc += (i & 3) + (i >> 1) + ((i ^ 5) | 8); } return to_string(acc);",
				"int x = 0x7fffffffffffffff; int one = 1; return to_string(x << one) + \",\" + to_string((x | one) >> 62);",
			};
			for (bool proofs : {true, false}) {
				for (const char* body : cases) {
					std::string program = std::string("string run() { ") + body + " } run();";
					auto interp = interp_engine();
					jai::stdlib::register_all(*interp);
					interp->type_proofs(proofs);
					auto vmx = vm_engine();
					jai::stdlib::register_all(*vmx);
					vmx->type_proofs(proofs);
					std::string want, got;
					try { want = interp->execute(program).as_string(); } catch (const std::exception& e) { want = std::string("throw:") + e.what(); }
					try { got = vmx->execute(program).as_string(); } catch (const std::exception& e) { got = std::string("throw:") + e.what(); }
					check_eq(want, got, body);
				}
			}
			auto vmx = vm_engine();
			jai::stdlib::register_all(*vmx);
			check_eq(std::string("Shift amount must be between 0 and 63"), vmx->execute("string run() { int a = 1; int n = 64; string m = \"ok\"; try { int r = a << n; } catch (e) { m = e; } return m; } run();").as_string());
			check_eq(std::string("Bitwise & requires integer operands"), vmx->execute("string run() { float a = 1.5; int n = 1; string m = \"ok\"; try { [[warn_mode]] var r = a & n; } catch (e) { m = e; } return m; } run();").as_string());
		});

		test("push_fused_statement_matches_the_interpreter_on_every_receiver", [this]() {
			// `recv.push(v);` as a statement compiles to one PUSH_FUSED; an array receiver
			// appends through the builtin's kernel, everything else replays the unfused call.
			// The interpreter is the oracle: every case must produce the same text.
			auto vm = vm_engine();
			auto* symbolizer = detail::engine_access::get_symbolizer(*vm);
			auto listing_of = [&](const char* script) {
				auto templates = detail::engine_access::get_registered_template_types(*vm);
				jai::lexer lex(script, symbolizer);
				auto tokens = lex.tokenize();
				jai::parser p(tokens, symbolizer, vm.get(), templates);
				auto parse_result = p.parse();
				check_true(parse_result.has_value(), "snippet parses");
				jai::vm::vm_compiler compiler(symbolizer);
				auto compiled = compiler.compile_program(parse_result.value());
				return jai::vm::disassemble(*compiled, symbolizer);
			};
			check_true(listing_of("var a = []; a.push(1); var z = 0;").find("PUSH_FUSED") != std::string::npos, "literal arg fuses");   // a program's last statement is its implicit result, so it is not a statement form
			check_true(listing_of("var a = []; var f = function() { int v = 2; a.push(v); }; var z = 0;").find("PUSH_FUSED") != std::string::npos, "env receiver + slot arg fuses");   // closure bodies are listed; function bodies compile at first call
			check_true(listing_of("var a = []; int v = 2; var f = function() { a.push(v); }; var z = 0;").find("PUSH_FUSED") == std::string::npos, "an env-resolved argument keeps the probe order");
			check_true(listing_of("var a = []; var r = a.push(1); var z = 0;").find("PUSH_FUSED") == std::string::npos, "an expression use keeps the general call");
			check_true(listing_of("var a = []; a.push(g()); var z = 0;").find("PUSH_FUSED") == std::string::npos, "a call argument keeps the probe order");
			check_true(listing_of("var a = []; a?.push(1); var z = 0;").find("PUSH_FUSED") == std::string::npos, "null-safe keeps the general call");
			check_true(listing_of("var a = []; var t = [1]; var f = function() { int k = 0; a.push(t[k]); }; var z = 0;").find("PUSH_FUSED") != std::string::npos, "a one-level subscript argument fuses");
			check_true(listing_of("var a = []; var t = [[1]]; var f = function() { a.push(t[0][0]); }; var z = 0;").find("PUSH_FUSED") == std::string::npos, "a two-level subscript keeps the general call");

			const char* cases[] = {
				"array<int> a = []; string m = \"ok\"; a.push(1); try { a.push(2.9); } catch (e) { m = e; } int v = 7; a.push(v); return m + \"|\" + to_string(a[0]) + \",\" + to_string(a[1]) + \",\" + to_string(a.size());",
				"array<int> a = []; string m = \"ok\"; try { a.push(\"x\"); } catch (e) { m = e; } return m + \"|\" + to_string(a.size());",
				"array<int> a = []; var big = 1e100; string m = \"ok\"; try { a.push(big); } catch (e) { m = e; } return m;",
				"var a = []; a.push(1); a.push(\"s\"); a.push(2.5); return to_string(a.size()) + \":\" + to_string(a[1]) + type_of(a[2]);",
				"var a; string m = \"ok\"; try { a.push(1); } catch (e) { m = e; } return m;",
				"var m = {}; string r = \"ok\"; try { m.push(1); } catch (e) { r = e; } return r;",
				"var s = \"x\"; string r = \"ok\"; try { s.push(1); } catch (e) { r = e; } return r;",
				"class P { array<int> xs = []; void push(int v) { xs.push(v * 2); } } auto p = P(); p.push(3); return to_string(p.xs[0]);",
				"class Q { int n = 0; } auto q = Q(); string r = \"ok\"; try { q.push(1); } catch (e) { r = e; } return r;",
				"var G = []; void f() { G.push(1); G.push(2); } f(); return to_string(G.size()) + \":\" + to_string(G[1]);",
				"class C { array<int> xs = []; void add(int v) { xs.push(v); } } auto c = C(); c.add(4); c.add(5); return to_string(c.xs[0] + c.xs[1]);",
				"array<int> a = []; var& r = a; r.push(5); return to_string(a[0]) + \":\" + to_string(a.size());",
				"string r = \"ok\"; try { nope.push(1); } catch (e) { r = e; } return r;",
				"array<string> names = []; names.push(\"a\"); names.push(\"b\"); return names[0] + names[1];",
				"array<float> fs = []; fs.push(1); fs.push(2.5); return to_string(fs[0] + fs[1]);",
				"class N { int v = 1; } array<N> ns = []; N one = N(); ns.push(one); one.v = 9; return to_string(ns[0].v);",
				"var a = [1]; int i = 0; while (i < 5) { a.push(i); i = i + 1; } return to_string(a.size());",
				"array<string> pal = [\"r\", \"g\"]; var parts = []; int k = 1; parts.push(pal[k]); parts.push(pal[0]); return parts.join(\"\");",
				"array<int> src = [7, 8]; array<int> dst = []; int k = 1; dst.push(src[k]); dst.push(src[0]); return to_string(dst[0] * 10 + dst[1]);",
				"array<int> src = [7]; array<int> dst = []; int k = 5; string m = \"ok\"; try { dst.push(src[k]); } catch (e) { m = e; } return m + \"|\" + to_string(dst.size());",
				"var src = [1]; var d; string m = \"ok\"; try { d.push(src[0]); } catch (e) { m = e; } return m;",
				"class Q { int n = 0; } auto q = Q(); var src = [1]; int k = 9; string r = \"ok\"; try { q.push(src[k]); } catch (e) { r = e; } return r;",
				"class P { array<int> xs = []; void push(int& v) { v = v + 1; xs.push(v); } } auto p = P(); array<int> src = [5]; p.push(src[0]); return to_string(src[0]) + \":\" + to_string(p.xs[0]);",
				"var m = {\"k\": 3}; var a = []; a.push(m[\"k\"]); return to_string(a[0]);",
				"var src = [1]; var a = []; string r = \"ok\"; try { a.push(src[nope]); } catch (e) { r = e; } return r;",
				// append lane rows: payloads the buffer takes as-is, and the ones that must not
				"var a = []; a.push(null); a.push(true); a.push('c'); a.push(-3); a.push(0.5); a.push(\"s\"); return type_of(a[0]) + type_of(a[1]) + type_of(a[2]) + type_of(a[3]) + type_of(a[4]) + type_of(a[5]) + to_string(a.size());",
				"var a = []; string s = \"lit\"; a.push(s); s += \"!\"; a[0] += \"?\"; return s + a[0];",
				"var a = []; a.push(\"k\"); a[0] += \"1\"; a.push(\"k\"); return a[0] + a[1] + \"k\";",
				"array<float> fs = []; int i = 3; fs.push(i); fs.push(2.5); float x = 1.25; fs.push(x); return type_of(fs[0]) + to_string(fs[0] + fs[1] + fs[2]);",
				"array<int> a = []; float f = 2.9; string m = \"\"; try { a.push(f); } catch (e) { m = e; } try { a.push(-0.5); } catch (e) { m = m + \"/\" + e; } a.push(cast<int>(f)); return m + \"|\" + to_string(a[0]) + \",\" + to_string(a.size());",
				"array<int> a = []; bool b = true; a.push(b); return to_string(a[0]) + type_of(a[0]);",
				"array<string> ss = []; string k = \"x\"; ss.push(k); k += \"y\"; ss[0] += \"z\"; return k + ss[0] + to_string(ss.size());",
				"array<string> ss = []; string m = \"ok\"; try { ss.push(5); } catch (e) { m = e; } return m + to_string(ss.size());",
				"array<bool> bs = []; bs.push(true); bs.push(1); return to_string(bs.size()) + to_string(bs[1]);",
				"array<array<int>> g = []; var row = [1, 2]; g.push(row); row.push(3); return to_string(g[0].size()) + to_string(row.size());",
				"array<array<int>> g = []; string m = \"ok\"; try { g.push(\"x\"); } catch (e) { m = e; } return m;",
				"class V { int v = 2; } var a = []; V one = V(); a.push(one); one.v = 5; return to_string(a[0].v);",
				"class V { int v = 2; } var a = []; shared_ptr<V> one = new V(); a.push(one); one.v = 5; return to_string(a[0].v);",
				"var a = []; var f = function() { return 4; }; a.push(f); return to_string(a[0]());",
			};
			for (const char* body : cases) {
				// Preserve literal argument shapes and exercise the runtime lanes, including rejection.
				std::string program = std::string("[[warn_mode]] string run() { ") + body + " } run();";
				auto interp = interp_engine();
				jai::stdlib::register_all(*interp);
				auto vmx = vm_engine();
				jai::stdlib::register_all(*vmx);
				std::string want, got;
				try { want = interp->execute(program).as_string(); } catch (const std::exception& e) { want = std::string("throw:") + e.what(); }
				try { got = vmx->execute(program).as_string(); } catch (const std::exception& e) { got = std::string("throw:") + e.what(); }
				check_eq(want, got, body);
			}
			// the append lane charges the kernel's exact bytes: the memory cap trips at the same push
			{
				const char* capped = "string run() { var a = []; int n = 0; try { while (true) { a.push(n); n = n + 1; } } catch (e) { return to_string(n) + \":\" + e; } return \"none\"; } run();";
				auto interp = interp_engine();
				jai::stdlib::register_all(*interp);
				interp->memory_cap(64 * 1024);
				auto capped_vm = vm_engine();
				jai::stdlib::register_all(*capped_vm);
				capped_vm->memory_cap(64 * 1024);
				const std::string want = interp->execute(capped).as_string();
				check_true(want.find(':') != std::string::npos && want != "none", "the cap raised");
				check_eq(want, capped_vm->execute(capped).as_string(), "same push trips the cap on both backends");
			}
			auto vmx = vm_engine();
			jai::stdlib::register_all(*vmx);
			check_eq(std::string("Cannot push 'string' to array<int>|0"), vmx->execute("[[warn_mode]] string run() { array<int> a = []; string m = \"ok\"; try { a.push(\"x\"); } catch (e) { m = e; } return m + \"|\" + to_string(a.size()); } run();").as_string());
			check_eq(std::string("Cannot push 'float' to array<int>"), vmx->execute("string run() { array<int> a = []; var big = 1e100; string m = \"ok\"; try { a.push(big); } catch (e) { m = e; } return m; } run();").as_string());
			check_eq(std::string("Cannot push 'float' to array<int>|1,7,2"), vmx->execute("[[warn_mode]] string run() { array<int> a = []; string m = \"ok\"; a.push(1); try { a.push(2.9); } catch (e) { m = e; } int v = 7; a.push(v); return m + \"|\" + to_string(a[0]) + \",\" + to_string(a[1]) + \",\" + to_string(a.size()); } run();").as_string());
		});
	}
};

} // namespace jai::foundry::tests

using vm_backend_tests_alias = jai::foundry::tests::vm_backend_tests;
FOUNDRY_REGISTER(vm_backend_tests_alias)
