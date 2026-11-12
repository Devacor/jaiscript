// Regression tests for bugs found during a deep review.
// Each test documents the finding number from that review and reproduces the
// edge case that previously failed. Pure-script reproducers live here; the
// C++-bound numeric reproducers (cpp_bound float/int reads) live in
// source/tests/semantics/cpp_bound_numeric_tests.cpp.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>
#include <optional>
#include <tuple>
#include <variant>
#include <atomic>
#include <thread>

#include "foundry/small_stack.hpp"
#include "../case_table.hpp"

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {

struct raises {};
using expected_value = std::variant<int64_t, std::string, bool, raises>;

// {name, source, expected} on a fresh make_engine(): an int row also requires an int result, a string row reads as_string()
template<typename Cases>
void check_values(const Cases& a_cases, bool a_stdlib = false) {
    check_each(a_cases, [a_stdlib](const auto& c) {
        auto e = make_engine();
        if (a_stdlib) { jai::stdlib::register_all(*e); }
        const std::string source = std::get<1>(c);
        const expected_value& expected = std::get<2>(c);
        if (std::holds_alternative<raises>(expected)) {
            check_throws([&]() { e->execute(source); }, source);
            return;
        }
        const auto r = e->execute(source);
        if (const auto* number = std::get_if<int64_t>(&expected)) {
            check_true(r.is_int(), "an int result: " + source);
            check_eq(*number, r.as_int(), source);
        } else if (const auto* flag = std::get_if<bool>(&expected)) {
            check_eq(*flag, r.as_bool(), source);
        } else {
            check_eq(std::get<std::string>(expected), r.as_string(), source);
        }
    });
}

}   // namespace

// A dynamic_binder-bound base for hybrid (script class : C++ class) rows. The
// by-reference rows cover every property kind: box-decodable member pointers
// (CPP-MEMBER), a class-typed member pointer and a lambda pair (PROPERTY), read-only.
struct inner_probe {
    int64_t x = 0;
};

struct hybrid_prop_probe {
    int64_t value = 0;
    std::string name = "n";
    double ratio = 1.5;
    bool flag = false;
    int32_t narrow = 0;
    inner_probe child;
    int64_t lam_backing = 0;
    int64_t lam_reads = 0;
    int64_t lam_writes = 0;
    jai::signal_emitter<void(int)> sig;   // non-copy-assignable member: a getter-only property by type
    int64_t doubled() const { return value * 2; }
    void bump_cpp() { value += 100; }
    hybrid_prop_probe& self() { return *this; }
};

inline void bind_hybrid_probe(engine& e) {
    dynamic_binder<inner_probe>(e, "InnerProbe")
        .constructor<>()
        .property("x", &inner_probe::x)
        .build();
    dynamic_binder<hybrid_prop_probe>(e, "HybridProbe")
        .constructor<>()
        .method("doubled", &hybrid_prop_probe::doubled)
        .method("bump_cpp", &hybrid_prop_probe::bump_cpp)
        .method("self", &hybrid_prop_probe::self)
        .property("value", &hybrid_prop_probe::value)
        .property("name", &hybrid_prop_probe::name)
        .property("ratio", &hybrid_prop_probe::ratio)
        .property("flag", &hybrid_prop_probe::flag)
        .property("narrow", &hybrid_prop_probe::narrow)
        .property("child", &hybrid_prop_probe::child)
        .property("lam",
                  [](hybrid_prop_probe& s) -> int64_t { ++s.lam_reads; return s.lam_backing; },
                  [](hybrid_prop_probe& s, int64_t v) { ++s.lam_writes; s.lam_backing = v; })
        .property("ro", [](const hybrid_prop_probe& s) -> int64_t { return s.value + 100; }, nullptr)
        .property("sig", &hybrid_prop_probe::sig)
        .build();
}

// The C++ object behind a script handle (hybrid layer or plain wrapper)
inline std::shared_ptr<hybrid_prop_probe> probe_of(engine& e, const char* expr) {
    return e.execute(expr).get_class_instance()->get_cpp_object_as<hybrid_prop_probe>();
}

class review_regression_tests : public suite {
public:
    review_regression_tests() : suite("Review Regressions") {}

    void forge_tests() override {
        test("holders_a_host_drops_on_another_thread_stay_off_the_engine_free_list", [this]() {
            // A pooled holder block returns to the engine's free list only on the thread that owns the pool
            // (holder_block_pool::return_block): a release on any other thread (an engine that moved threads while idle) is a
            // foreign return, never a free-list push. Checked in its harshest form, releases racing the engine's own mints
            auto e = jai::foundry::make_default_engine();
            jai::engine* eng = e.get();
            std::vector<jai::script_value> handed;
            for (int i = 0; i < 4000; ++i) {
                jai::script_value arr = e->execute("[1, 2, 3]");
                handed.push_back(jai::script_value::make_element_reference(arr.get_array_storage(), 1, eng, nullptr));
            }
            jai::script_value mine = e->execute("[4, 5, 6]");   // the engine's last result is this array, not one handed over
            std::atomic<bool> go{false};
            std::thread host([&]() { while (!go.load()) {} handed.clear(); });
            go = true;
            int64_t sum = 0;
            for (int i = 0; i < 20000; ++i) {
                jai::script_value r = jai::script_value::make_element_reference(mine.get_array_storage(), static_cast<size_t>(i % 3), eng, nullptr);
                sum += r.deref().as_int();
            }
            host.join();
            check_eq(int64_t(99999), sum, "the engine's mints read their elements while the host released its own");
            check_eq(int64_t(4), e->execute("2 + 2").as_int(), "the engine runs on");
        });

        test("a_compound_target_reference_is_read_after_the_right_hand_side", [this]() {
            // Regression: `value += restore(data)` where `value` is an untyped element
            // or map-entry reference whose element was removed and whose right side restores it. The interpreter
            // dereferenced the target before the right side ran (to probe its fast paths) and raised the removed-element
            // error with calls = 0; the VM, whose right value is already on the stack, ran restore() first and stored 22.
            // The language's order is the right side first; both backends agree, and a right side that raises
            // surfaces its own error before any target error
            for (bool vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                jai::stdlib::register_all(*e);
                const std::string label = vm ? "vm" : "interp";
                auto run = [&](const char* body) {
                    std::string src = std::string(R"(
                        var calls = 0;
                        function restore_array(var& d) -> int { calls = calls + 1; d.push(20); return 2; }
                        function restore_map(var& d) -> int { calls = calls + 1; d["key"] = 20; return 2; }
                        function boom() -> int { calls = calls + 1; throw "boom"; }
                        string msg = "";
                        var got = 0;
                        try { )") + body + R"( } catch (err) { msg = err; }
                        return to_string(got) + "|" + to_string(calls) + "|" + msg;
                    )";
                    try { return e->execute("[[warn_mode]]; " + src).as_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
                };
                check_eq(std::string("22|1|"), run("function probe() -> int { var data = [3]; var& value = data[0]; data.pop(); value += restore_array(data); return value; } got = probe();"), label + " a removed array element restored by the right side");
                check_eq(std::string("22|1|"), run("function probe() -> int { var data = {\"key\": 3}; var& value = data[\"key\"]; data.erase(\"key\"); value += restore_map(data); return value; } got = probe();"), label + " a removed map entry restored by the right side");
                check_eq(std::string("0|1|boom"), run("function probe() -> int { var data = [3]; var& value = data[0]; data.pop(); value += boom(); return value; } got = probe();"), label + " the right side's own error comes first");
                check_eq(std::string("0|0|Reference to a removed array element"), run("function probe() -> int { var data = [3]; var& value = data[0]; data.pop(); value += 2; return value; } got = probe();"), label + " a literal right side: the target's error stands");
                check_eq(std::string("5|0|"), run("function probe() -> int { var data = [3]; var& value = data[0]; value += 2; return value; } got = probe();"), label + " control: a live element");
            }
        });
        test("a_program_scope_compound_on_a_removed_element_is_catchable_on_both_backends", [this]() {
            // compound_program_scope probe: `value += 2` on a reference to a removed element at program scope. Both
            // backends dereferenced the target raw and the host exception escaped the script's catch at program scope
            // (inside a function the call boundary converted it); the interpreter's read and the VM's are checked, so
            // the catch sees the language's error at every scope on both backends
            for (bool vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                jai::stdlib::register_all(*e);
                const std::string label = vm ? "vm" : "interp";
                auto run = [&](const char* src) {
                    try { return e->execute(std::string("[[warn_mode]]; ") + src).as_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
                };
                check_eq(std::string("Reference to a removed array element|finished"),
                         run(R"(string msg = ""; try { var data = [3]; var& value = data[0]; data.pop(); value += 2; } catch (err) { msg = err; } return msg + "|finished";)"),
                         label + " a removed array element at program scope");
                check_eq(std::string("Reference to a removed map entry|finished"),
                         run(R"(string msg = ""; try { var data = {"key": 3}; var& value = data["key"]; data.erase("key"); value -= 2; } catch (err) { msg = err; } return msg + "|finished";)"),
                         label + " a removed map entry at program scope");
                check_eq(std::string("22|finished"),
                         run(R"(string msg = ""; var got = 0; try { var data = [3]; var& value = data[0]; data.pop(); data.push(20); value += 2; got = value; } catch (err) { msg = err; } return to_string(got) + msg + "|finished";)"),
                         label + " control: a restored element at program scope");
            }
        });
        test("a_raising_left_operand_stops_the_expression_on_both_backends", [this]() {
            // Fuzz seed 1449: a missing instance field on the LEFT raises through the
            // interpreter's unwinding flag with a placeholder value, and the interpreter went on to
            // evaluate the RIGHT operand, so its side effect ran and a checked error from it (a
            // missing static member) replaced the first; the vm stops at the left. Both backends
            // report the left's error and run the right operand zero times, for arithmetic,
            // comparison and the logical operators
            for (bool vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                jai::stdlib::register_all(*e);
                const std::string label = vm ? "vm" : "interp";
                auto run = [&](const char* body) {
                    std::string src = std::string(R"(
                        class C { int n = 0; static int s = 1; }
                        var calls = 0;
                        int touch() { calls = calls + 1; return 1; }
                        var v = C();
                        string msg = "";
                        try { )") + body + R"( } catch (err) { msg = err; }
                        return to_string(calls) + "|" + msg;
                    )";
                    return e->execute("[[warn_mode]]; " + src).as_string();
                };
                check_eq(std::string("0|C has no member 'missing'"), run("var x = v.missing + touch();"), label + " arithmetic, left raises");
                check_eq(std::string("0|C has no member 'missing'"), run("var x = v.missing + C::nope;"), label + " arithmetic, both would raise: the left wins");
                check_eq(std::string("0|C has no member 'missing'"), run("var x = v.missing < touch();"), label + " comparison, left raises");
                check_eq(std::string("0|C has no member 'missing'"), run("var x = v.missing && touch();"), label + " logical and, left raises");
                check_eq(std::string("0|C has no member 'missing'"), run("var x = v.missing || touch();"), label + " logical or, left raises");
                check_eq(std::string("1|C has no member 'missing'"), run("var x = touch() + v.missing;"), label + " right raises after the left ran once");
                check_eq(std::string("1|C has no member 'missing'"), run("var x = touch() && v.missing;"), label + " logical and, right raises");
                check_eq(std::string("2|"), run("var x = touch() + touch();"), label + " control");
            }
        });

        // ---- #5 lexer: template (backtick) string at end-of-source ----
        test("a_template_string_at_the_end_of_source_lexes", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"tmpl_string_at_eof", "`hello world`", std::string("hello world")},
                {"tmpl_string_interp_at_eof", "auto y = 7; `val=${y}`", std::string("val=7")},
            };
            check_values(cases);
        });

        // ---- #6 lexer: malformed octal must error, valid octal still works ----
        test("octal_literals_parse_and_a_malformed_one_raises", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"octal_valid", "010", int64_t{8}},
                {"octal_malformed_throws", "auto x = 08; x", raises{}},
            };
            check_values(cases);
        });

        // ---- #7 parser: deep prefix-operator chain must not crash ----
        test("deep_unary_chain_modest", [this]() {
            auto e = make_engine();
            std::string src(100, '!');  // even count of '!' applied to true => true; well under the guard
            src += "true";
            check(e->execute(src).as_bool());
        });
        test("deep_unary_chain_guard_no_crash", [this]() {
            // 20,000 levels clear a 1 MB stack on every build: one parse error, and the engine stays usable
            on_small_stack([this]() {
                auto e = make_engine();
                std::string src(20000, '!');
                src += "true";
                std::string failure;
                try { e->execute(src); } catch (const std::exception& error) { failure = error.what(); }
                check_true(failure.find("nesting too deep for the host thread's stack") != std::string::npos, failure.substr(0, 200));
                check_eq((int64_t)3, e->execute("1 + 2").as_int());
            });
        });

        // ---- chained subscript on a temporary across the execute boundary ----
        // The result of [temp][0]["k"] is a map-entry reference holding the LAST pin on
        // the temporary map; the boundary collapse `x = x.deref()` assigned from storage
        // x itself owned, and destroy-then-copy in storage::operator= read freed memory
        // (0xDD under Debug). Pins the pin-before-destroy assignment order, both backends.
        test("chained_subscript_on_temporary_survives_boundary", [this]() {
            auto e = make_engine();
            check_eq(std::string("zz"), e->execute("[{\"name\":\"zz\"}][0][\"name\"]").as<std::string>());
            check_eq((int64_t)7, e->execute("[[7]][0][0]").as_int());
            check_eq(std::string("deep"), e->execute("{\"a\": [{\"b\": \"deep\"}]}[\"a\"][0][\"b\"]").as<std::string>());
        });

        // ---- #19 range-for inside a function ----
        test("range_for_in_a_function_by_value_and_by_reference", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"range_for_in_function", R"(
                auto sum(auto arr) -> auto {
                    auto total = 0;
                    for (auto x : arr) { total = total + x; }
                    return total;
                }
                sum([1, 2, 3]);
            )", int64_t{6}},
                {"range_for_ref_in_function", R"(
                auto doubleAll(auto arr) -> auto {
                    for (auto& x : arr) { x = x * 2; }
                    return arr[0] + arr[1] + arr[2];
                }
                doubleAll([1, 2, 3]);
            )", int64_t{12}},
            };
            check_values(cases);
        });

        // ---- #8 set_local gap after a skipped block ----
        test("local_after_untaken_block", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                auto f() -> auto {
                    auto a = 1;
                    if (false) { auto b = 99; }
                    auto c = a + 2;
                    return c;
                }
                f();
            )");
            check_eq((int64_t)3, r.as_int());
        });

        // ---- #15 throw inside a while loop must not hang ----
        test("a_throw_inside_a_loop_leaves_the_loop", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"throw_in_while_no_hang", R"(
                auto count = 0;
                try {
                    var i = 0;
                    while (i < 5) { count = count + 1; throw "x"; }
                } catch (e) { }
                count
            )", int64_t{1}},
                {"throw_in_for_no_hang", R"(
                auto count = 0;
                try {
                    for (var i = 0; i < 5; i = i + 1) { count = count + 1; throw "x"; }
                } catch (e) { }
                count
            )", int64_t{1}},
            };
            check_values(cases);
        });

        // ---- #16 shrinking an array during value range-for must not OOB ----
        test("range_for_survives_its_container_shrinking_or_reallocating", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"range_for_shrink_no_oob", R"(
                auto a = [1, 2, 3, 4, 5];
                auto seen = 0;
                auto first = true;
                for (auto x : a) { seen = seen + 1; if (first) { a.clear(); first = false; } }
                seen
            )", int64_t{1}},
                {"range_for_ref_realloc_no_corruption", R"(
                auto arr = [10, 20, 30];
                for (auto& x : arr) {
                    var j = 0;
                    while (j < 64) { arr.push(0); j = j + 1; }  // force a reallocation
                    x = 999;                                    // write through AFTER realloc
                    break;
                }
                arr[0]
            )", int64_t{999}},
            };
            check_values(cases);
        });

        // ---- #17 continue inside a switch case stops the case body & continues loop ----
        test("switch_break_and_continue_inside_a_loop", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"switch_continue_in_loop", R"(
                auto total = 0;
                for (var i = 0; i < 4; i = i + 1) {
                    switch (i) {
                        case 1:
                            continue;
                            total = total + 1000;   // must NOT execute
                        default:
                            total = total + 1;
                    }
                }
                total
            )", int64_t{3}},
                {"switch_break_does_not_break_loop", R"(
                auto total = 0;
                for (var i = 0; i < 3; i = i + 1) {
                    switch (i) {
                        case 0: total = total + 10; break;
                        default: total = total + 1;
                    }
                }
                total
            )", int64_t{12}},
            };
            check_values(cases);
        });

        // ---- #32 reading a missing map key must NOT insert it ----
        test("a_map_read_never_inserts_and_an_assignment_does", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"map_read_missing_no_insert", R"(
                var m = {"a": 1, "b": 2};
                int raised = 0;
                try { var x = m["zzz"]; } catch (e) { raised += 1; }
                try { var y = m["qqq"]; } catch (e) { raised += 1; }
                m.size() * 10 + raised
            )", int64_t{22}},
                {"map_assign_still_inserts", R"(
                var m = {"a": 1};
                m["new"] = 5;
                m["a"] + m["new"] + m.size()
            )", int64_t{8}},
            };
            check_values(cases);
        });

        // ---- #28 cpp-bound 4-byte float must be read as a float, not 8 bytes ----
        test("cpp_bound_float_read", [this]() {
            auto e = make_engine();
            float hp = 12.5f;
            e->add_global_ref("hp", hp);
            check_near(12.5, e->execute("hp").as_float(), 0.0001);
        });
        // ---- #29 cpp-bound int64 -> float must keep all 64 bits ----
        test("cpp_bound_int64_to_float", [this]() {
            auto e = make_engine();
            int64_t big = 5000000000LL;  // > INT32_MAX
            e->add_global_ref("big", big);
            check_near(5000000000.0, e->execute("big * 1.0").as_float(), 1.0);
        });

        // ---- #26/#27 complex-typed & float map keys keep std::map intact ----
        test("array_map_keys_stable", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                var m = {};
                m[[1, 2]] = "a";
                m[[3, 4]] = "b";
                m.size()
            )");
            check_eq((int64_t)2, r.as_int());
        });

        // ---- #9/#10/#11 integer overflow policy (compile-time; tests adapt to it) ----
        // Variables are used so the parser's constant-folder doesn't pre-evaluate these.
        // Behavior depends on the build: throw_on_overflow() true => raises; false => wraps.
        test("int_overflow_add", [this]() {
            auto e = make_engine();
            const char* src = "auto x = 9223372036854775807; x + 1";  // INT64_MAX + 1
            if (e->throw_on_overflow()) {
                check_throws([&]() { e->execute(src); });
            } else {
                check_eq((int64_t)INT64_MIN, e->execute(src).as_int());  // wraps to INT64_MIN
            }
        });
        test("int_overflow_mul", [this]() {
            auto e = make_engine();
            const char* src = "auto x = 9223372036854775807; x * 2";  // INT64_MAX * 2
            if (e->throw_on_overflow()) {
                check_throws([&]() { e->execute(src); });
            } else {
                check_eq((int64_t)-2, e->execute(src).as_int());  // 2*(2^63-1) wraps to -2
            }
        });
        test("int_min_div_neg1", [this]() {
            auto e = make_engine();
            const char* src = "auto x = -9223372036854775807 - 1; auto d = -1; x / d";  // INT64_MIN / -1
            if (e->throw_on_overflow()) {
                check_throws([&]() { e->execute(src); });
            } else {
                check_eq((int64_t)INT64_MIN, e->execute(src).as_int());  // wraps to INT64_MIN
            }
        });
        test("int_results_that_fit_never_raise_overflow", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"int_no_false_overflow", "auto x = 2000000000; x * 2", int64_t{4000000000}},
                {"int_mod_neg1_is_zero", "auto x = -9223372036854775807 - 1; auto d = -1; x % d", int64_t{0}},
            };
            check_values(cases);
        });

        // ---- range-for over a NESTED container read (jaidoom edge): for (x : m["a"])
        // failed "not a map" on both backends — the source expression arrives as a
        // reference wrapper and iter-init type-checked the wrapper. Deref'd handles
        // share the live node, so by-ref element mutation still lands in the original. ----
        test("range_for_iterates_nested_container_reads", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);   // map iteration mints pair objects
            auto r = e->execute(R"(
                var m = {"a": {"b": 1, "c": 2}};
                var acc = "";
                for (var kv : m["a"]) { acc += kv.first + "=" + to_string(kv.second) + ";"; }
                var arr = {"list": [10, 20, 30]};
                int sum = 0;
                for (var x : arr["list"]) { sum += x; }
                var nested = [[1, 2], [3, 4]];
                for (var y : nested[1]) { sum += y; }
                for (auto& z : arr["list"]) { z = z * 2; }
                acc + "|" + to_string(sum) + "|" + to_string(arr["list"][0]) + "," + to_string(arr["list"][2]);
            )");
            check_eq(std::string("b=1;c=2;|67|20,60"), r.as_string(),
                "nested reads iterate and by-ref mutation lands in the original");
        });

        // ---- container literals hold VALUES (jaidoom netplay finding): element reads
        // arrive as reference wrappers (rhs-lvalue read shape) and literals must
        // normalize them like assignment does — the un-derefed wrapper made
        // {"x": arr[i]} serialize as null AND aliased the source array. ----
        test("container_literals_normalize_element_reads", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            auto r = e->execute(R"(
                array<int> typed = [100, 200, 300];
                var plain = [10, 20];
                var m = {"t": typed[1], "p": plain[0]};
                var a = [typed[0], plain[1]];
                var nested = {"outer": {"inner": typed[2]}};
                typed[1] = -1;              // literals snapshot: mutations must not show through
                plain[0] = -1;
                to_json(m) + "|" + to_json(a) + "|" + to_json(nested);
            )");
            check_eq(std::string("{\"p\":10,\"t\":200}|[100,20]|{\"outer\":{\"inner\":300}}"), r.as_string(),
                "element reads land as numbers and snapshot at literal construction");
        });
        // ---- shift semantics CONTRACT (policy-independent, both overflow builds):
        // bitwise ops including << >> operate on the 64-bit pattern mod 2^64 by DESIGN —
        // bits shifted past bit 63 are discarded, never an overflow error (matching
        // Rust/C#: checked arithmetic excludes bit operations). Only the COUNT is
        // guarded (0..63, catchable runtime error). GLOOM/demoreel/crawler xorshift
        // RNGs and the (0x9E3779B9 << 32) seed idiom depend on every pin here. ----
        test("shift_left_wraps_mod_2_64", [this]() {
            auto e = make_engine();
            check_eq((int64_t)INT64_MIN, e->execute("auto x = 1; auto s = 63; x << s").as_int());
            check_eq((int64_t)INT64_MIN, e->execute("1 << 63").as_int());   // literal spelling too
            check_eq((int64_t)-2, e->execute("auto y = 9223372036854775807; y << 1").as_int());
            auto g = e->execute("auto g = (0x9E3779B9 << 32) | 0x7F4A7C15; g");   // the GLOOM seed idiom
            check_true(g.as_int() < 0, "shift into the sign bit wraps negative, no error");
            check_eq((int64_t)0x9E3779B9, e->execute("auto g = (0x9E3779B9 << 32) | 0x7F4A7C15; (g >> 32) & 0xFFFFFFFF").as_int());
            check_eq((int64_t)0x7F4A7C15, e->execute("auto g = (0x9E3779B9 << 32) | 0x7F4A7C15; g & 0xFFFFFFFF").as_int());
        });
        test("shift_left_vs_mul_contrast", [this]() {   // the checked-overflow boundary, pinned
            auto e = make_engine();
            const char* shl = "auto x = 4611686018427387904; x << 1";   // 2^62 << 1 wraps to INT64_MIN
            const char* mul = "auto x = 4611686018427387904; x * 2";    // same math, ARITHMETIC op
            check_eq((int64_t)INT64_MIN, e->execute(shl).as_int());
            if (e->throw_on_overflow()) {
                check_throws([&]() { e->execute(mul); });
            } else {
                check_eq((int64_t)INT64_MIN, e->execute(mul).as_int());
            }
        });
        test("shift_right_is_arithmetic", [this]() {
            auto e = make_engine();
            check_eq((int64_t)-4, e->execute("auto x = -8; auto s = 1; x >> s").as_int());
            check_eq((int64_t)-1, e->execute("auto x = -9223372036854775807 - 1; x >> 63").as_int());
        });
        test("shift_count_range_errors_text_parity", [this]() {
            std::string msg[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                check_throws([&]() { e->execute("auto x = 1; auto s = 64; x << s;"); });
                check_throws([&]() { e->execute("auto x = 1; auto s = -1; x << s;"); });
                check_throws([&]() { e->execute("auto x = 1; auto s = 64; x >> s;"); });
                try { e->execute("auto x = 1; auto s = 64; x << s;"); }
                catch (const std::exception& ex) { msg[idx] = ex.what(); }
                check_eq((int64_t)3, e->execute("1 + 2").as_int());   // engine stays usable
                ++idx;
            }
            check_eq(msg[0], msg[1], "shift-count error text is byte-identical across backends");
            check_true(msg[0].find("Shift amount must be between 0 and 63") != std::string::npos,
                "the count guard names its range");
        });
        // ---- fuzz FZ-SPACESHIP-OVERFLOW-SWALLOW (seed 9336): the parser wrap-folds
        // 1 + INT64_MAX into a literal INT64_MIN; the interpreter's literal unary-minus
        // fast path then negated it UNCHECKED (silent wrap) while the VM raised ----
        test("fuzz_unary_minus_literal_overflow", [this]() {
            auto e = make_engine();
            const char* src = "(-((1 + 9223372036854775807))) <=> 9223372036854775807";
            if (e->throw_on_overflow()) {
                check_throws([&]() { e->execute(src); });
            } else {
                check_eq((int64_t)-1, e->execute(src).as_int());  // wraps: INT64_MIN <=> INT64_MAX
            }
        });
        test("fuzz_unary_minus_overflow_text_parity", [this]() {
            std::string msg[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                if (!e->throw_on_overflow()) { return; }  // wrap build: nothing to compare
                try { e->execute("(-((1 + 9223372036854775807))) <=> 1;"); }
                catch (const std::exception& ex) { msg[idx] = ex.what(); }
                ++idx;
            }
            check_eq(msg[0], msg[1], "overflow error text is byte-identical across backends");
            // PARSER-FOLD-WRAPS fix: the folder now declines to fold 1 + INT64_MAX, so
            // the runtime '+' raises (correct attribution) before unary '-' ever runs.
            check_true(msg[0].find("Integer overflow in '+'") != std::string::npos,
                "the un-folded '+' raises with its own operator name");
        });

        // ---- PARSER-FOLD-WRAPS: the constant folder used raw + - * (signed-overflow UB
        // and a silent wrap bypassing the checked policy). It now folds through
        // jai::ints and declines to fold on overflow so the runtime raises. ----
        test("parser_fold_overflow_defers_to_runtime", [this]() {
            std::string msg[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                if (!e->throw_on_overflow()) {
                    // Wrap build: folding the wrapped value IS the policy
                    check_eq((int64_t)INT64_MIN, e->execute("9223372036854775807 + 1").as_int());
                    continue;
                }
                try { e->execute("9223372036854775807 + 1"); }
                catch (const std::exception& ex) { msg[idx] = ex.what(); }
                check_true(msg[idx].find("Integer overflow in '+'") != std::string::npos,
                    "pure-literal INT64_MAX + 1 raises at runtime instead of wrapping at parse time");
                ++idx;
            }
            if (idx == 2) {
                check_eq(msg[0], msg[1], "fold-declined '+' text is byte-identical across backends");
            }
        });
        // The literal negation fold negated a folded INT64_MIN in C++ (signed-overflow UB, a silent wrap); it now stays a
        // run-time negation, which raises
        test("parser_fold_negating_int64_min_defers_to_runtime", [this]() {
            for (const char* src : {"-(-9223372036854775807 - 1)", "-(0 - 9223372036854775807 - 1)", "int x = -(-9223372036854775807 - 1); x"}) {
                auto e = make_engine();
                if (e->throw_on_overflow()) {
                    std::string text = "<no throw>";
                    try { e->execute(src); } catch (const std::exception& ex) { text = ex.what(); }
                    check_eq(std::string("Integer overflow in unary '-'"), text, src);
                } else {
                    check_eq((int64_t)INT64_MIN, e->execute(src).as_int(), src);
                }
            }
            auto e = make_engine();
            check_eq((int64_t)-9223372036854775807LL, e->execute("-9223372036854775807").as_int(), "the largest literal still folds");
            check_eq((int64_t)5, e->execute("-(-5)").as_int());
        });
        test("parser_fold_div_min_by_neg1_defers_to_runtime", [this]() {
            auto e = make_engine();
            // Every operand folds to a literal; INT64_MIN / -1 must NOT fold (UB/trap)
            const char* src = "(0 - 9223372036854775807 - 1) / (0 - 1)";
            if (e->throw_on_overflow()) {
                check_throws([&]() { e->execute(src); });
            } else {
                check_eq((int64_t)INT64_MIN, e->execute(src).as_int());
            }
        });

        // ---- INCDEC-WRAPS-SILENTLY: plain ++/-- on int variables/fields now applies
        // the overflow policy like every other int op (both backends, same text as the
        // checked counting-for update). ----
        test("incdec_overflow_checked_identifier", [this]() {
            std::string msg[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                if (!e->throw_on_overflow()) {
                    check_eq((int64_t)INT64_MIN, e->execute("var a = 9223372036854775807; a++; a;").as_int());
                    continue;
                }
                try { e->execute("var a = 9223372036854775807; a++;"); }
                catch (const std::exception& ex) { msg[idx] = ex.what(); }
                check_true(msg[idx].find("Integer overflow in '++'") != std::string::npos,
                    "a++ at INT64_MAX raises and names '++'");
                ++idx;
            }
            if (idx == 2) {
                check_eq(msg[0], msg[1], "'++' overflow text is byte-identical across backends");
            }
        });
        test("incdec_overflow_checked_prefix_decrement", [this]() {
            auto e = make_engine();
            const char* src = "var a = -9223372036854775807 - 1; --a;";
            if (e->throw_on_overflow()) {
                bool threw = false;
                std::string msg;
                try { e->execute(src); } catch (const std::exception& ex) { threw = true; msg = ex.what(); }
                check_true(threw);
                check_true(msg.find("Integer overflow in '--'") != std::string::npos, "--a at INT64_MIN names '--'");
            } else {
                check_eq((int64_t)INT64_MAX, e->execute("var a = -9223372036854775807 - 1; --a; a;").as_int());
            }
        });
        test("incdec_overflow_checked_this_field", [this]() {
            auto e = make_engine();
            const char* src =
                "class C { int f = 9223372036854775807; void bump() { f++; } }"
                "auto c = C(); c.bump();";
            if (e->throw_on_overflow()) {
                bool threw = false;
                std::string msg;
                try { e->execute(src); } catch (const std::exception& ex) { threw = true; msg = ex.what(); }
                check_true(threw);
                check_true(msg.find("Integer overflow in '++'") != std::string::npos, "this-field f++ names '++'");
            } else {
                auto r = e->execute(
                    "class C { int f = 9223372036854775807; void bump() { f++; } }"
                    "auto c = C(); c.bump(); c.f;");
                check_eq((int64_t)INT64_MIN, r.as_int());
            }
        });

        // Range-for over a container expression that raises a script exception must
        // unwind with the ORIGINAL exception; the interpreter fell into the
        // not-iterable ladder instead (empty catch text / 'Type mismatch'). Found by
        // the differential fuzzer (seeds 1356/1484 post-grammar-extension: a
        // reload-removed coroutine method called as the for-range container).
        test("range_for_throwing_container_unwinds_original", [this]() {
            const char* src = R"(
                class A { int f = 1; coroutine int co() { yield f; return 2; } }
                var a = A();
                class A { int f = 1; }
                var out = "";
                try { for (auto y : a.co()) { out = out + y; } } catch (ex) { out = `${out}co[${ex}]`; }
                try { for (auto y2 : a.f) { out = out + y2; } } catch (ex2) { out = `${out}|f[${ex2}]`; }
                out = `${out}|${a.f}`;
                out;
            )";
            std::array<std::string, 2> got;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                got[use_vm ? 1 : 0] = e->execute(src).as<std::string>();
            }
            check(got[0].find("co[A has no member 'co']") != std::string::npos,
                  "interp: original exception surfaces from the container expression");
            check_eq(got[0], got[1], "range-for container-throw parity");
        });

        // ---- #35 JSON float serialization round-trips at full precision ----
        test("json_float_roundtrip", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            check(e->execute("var x = 0.1 + 0.2; from_json(to_json(x)) == x").as_bool());
        });
        // ---- #34 deeply nested JSON parses on a work list (no stack overflow, no depth cap) ----
        test("json_deep_nesting_parses", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            std::string s(300, '[');
            s.append(300, ']');
            e->add_global("s", s);
            check_true(e->execute("to_json(from_json(s)) == s").as_bool());
            check_throws([&]() { e->execute("from_json(\"[[[\")"); });
        });

        // ---- #23 field initializers run in DECLARATION order (not intern order) ----
        test("field_init_declaration_order", [this]() {
            auto e = make_engine();
            // 'total' is interned first (global) -> smaller name_id than 'base'; the old
            // map-ordered init ran total's initializer before base's -> base unset.
            auto r = e->execute(R"(
                auto total = 0;
                class Account { int base = 100; int total = this.base + 5; }
                auto a = Account();
                a.total
            )");
            check_eq((int64_t)105, r.as_int());
        });

        // ---- #24 this() delegation: target's assignment wins, other defaults kept ----
        test("this_delegation_no_clobber", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                class C {
                    int x = 999;
                    int y = 7;
                    C() : this(42) { }
                    C(int v) { this.x = v; }
                }
                auto c = C();
                c.x * 1000 + c.y
            )");
            check_eq((int64_t)42007, r.as_int());  // x=42 (target), y=7 (default kept)
        });

        // ---- #31 array callbacks that mutate the same array must not crash ----
        test("remove_if_callback_mutation", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                var arr = [1, 2, 3, 4, 5, 6, 7, 8];
                arr.remove_if([](auto x) -> auto { if (x == 2) { arr.push(99); } return x % 2 == 0; });
                arr.length()
            )");
            check_eq((int64_t)4, r.as_int());  // survivors [1,3,5,7]; the push is discarded
        });
        test("sort_mixed_numeric_no_ub", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                var a = [3.5, 1, 2.5, 2];
                a.sort();
                a[0]
            )");
            check_eq((int64_t)1, r.as_int());  // value order: [1, 2, 2.5, 3.5]
        });

        // ---- #41 reference into an array survives reallocation (no heap corruption) ----
        // ---- #20 closure capture of enclosing function locals (all forms) ----
        test("closure_capture_local_auto_no_capture", [this]() {
            // [] captures nothing: an enclosing local needs its name in the list
            auto e = make_engine();
            check_throws([&]() { e->execute("auto make() -> auto { auto local = 7; return []() -> auto { return local + 1; }; } make()();"); });
            auto r = e->execute(R"(
                auto make() -> auto {
                    auto local = 7;
                    return [local]() -> auto { return local + 1; };
                }
                auto f = make();
                f()
            )");
            check_eq((int64_t)8, r.as_int());
        });
        test("lambdas_capture_enclosing_function_locals", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"closure_capture_local_eq", R"(
                auto make() -> auto {
                    auto local = 10;
                    return [=]() -> auto { return local * 2; };
                }
                make()()
            )", int64_t{20}},
                {"closure_capture_local_inline", R"(
                auto result = 0;
                auto x = 5;
                auto f = [=]() -> auto { return x + 3; };
                result = f();
                result
            )", int64_t{8}},
                {"closure_capture_local_explicit", R"(
                auto make(auto n) -> auto {
                    auto doubled = n * 2;
                    return [doubled]() -> auto { return doubled + 1; };
                }
                make(6)()
            )", int64_t{13}},
                {"closure_capture_multiple_locals", R"(
                auto make() -> auto {
                    auto a = 3;
                    auto b = 4;
                    return [=]() -> auto { return a * a + b * b; };
                }
                make()()
            )", int64_t{25}},
            };
            check_values(cases);
        });

        test("closure_capture_local_snapshot", [this]() {
            // A captured local is a snapshot; a top-level (global) x is read when the lambda runs
            auto e = make_engine();
            auto r = e->execute(R"(
                function run() -> auto {
                    auto x = 10;
                    auto f = [=]() -> auto { return x; };
                    x = 999;
                    return f();
                }
                run()
            )");
            check_eq((int64_t)10, r.as_int());
            auto g = e->execute("auto gx = 10; auto gf = [=]() -> auto { return gx; }; gx = 999; gf()");
            check_eq((int64_t)999, g.as_int());
        });
        // ---- for-body shadow scoping: fresh scope per iteration (vm/while/C++ parity) ----
        // The interpreter's counting-loop fast path reused one body env across iterations,
        // so a body `var g` shadow leaked into the next iteration's reads.
        test("a_for_body_shadow_is_fresh_every_iteration", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"for_body_shadow_fresh_per_iteration", R"(
                var g = 1;
                var out = "";
                for (var i = 0; i < 3; ++i) {
                    out += to_string(g) + ";";
                    var g = 100;
                }
                out + to_string(g);
            )", std::string("1;1;1;1")},
                {"for_body_shadow_general_path_variant", R"(
                var start = 0;
                var g = 1;
                var out = "";
                for (var i = start; i < 3; ++i) {
                    out += to_string(g) + ";";
                    var g = 100;
                }
                out;
            )", std::string("1;1;1;")},
            };
            check_values(cases, true);
        });

        test("for_body_shadow_fn_call_variant", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                function h() -> auto { return 1; }
                var s = 0;
                for (var i = 0; i < 3; ++i) {
                    s += h();
                    var h = 9;
                    s += h;
                }
                s;
            )");
            check_eq((int64_t)30, r.as_int());
        });
        // ---- fuzz FZ-TYPED-FN-NO-RETURN (seeds 66/239/2529/3657/3784): ++/-- on a
        // slot-based function local resolved only through the environment, so the
        // interpreter raised a bogus "Undefined variable '<local>'" (VM ran fine) ----
        test("increment_and_decrement_of_slot_locals_and_parameters", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"fuzz_incdec_slot_local", R"(
                function f() { int g = 0; ++g; return g; }
                f();
            )", int64_t{1}},
                {"fuzz_incdec_slot_param_in_while", R"(
                function f(int g) { while (++g < 4) {} return g; }
                f(0);
            )", int64_t{4}},
                {"fuzz_incdec_postfix_slot_local", R"(
                function f() { int g = 0; while (g++ < 4) {} return g; }
                f();
            )", int64_t{5}},
            };
            check_values(cases);
        });

        // ---- fuzz seed 110753: ++/-- on a MISSING member. The member read raises through
        // the unwinding state (placeholder null on the stack) and the interpreter's ++/--
        // numeric gate then clobbered it with a bare invalid_numeric_operand ("" when
        // caught); the vm unwinds at the read ----
        test("fuzz_incdec_missing_member_surfaces_member_error", [this]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            auto r = e->execute(R"(
                class C6 { int m8 = 1; }
                var v = C6();
                var out = "";
                try { v.m1--; } catch (e) { out += to_string(e) + "|"; }
                try { v.m1++; } catch (e) { out += to_string(e) + "|"; }
                try { --v.m1; } catch (e) { out += to_string(e) + "|"; }
                try { ++v.m1; } catch (e) { out += to_string(e) + "|"; }
                try { var r = v.m1--; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                var arr = [C6()];
                try { arr[0].m1--; } catch (e) { out += to_string(e) + "|"; }
                class H { C6 c = C6(); }
                var h = H();
                try { h.c.m1--; } catch (e) { out += to_string(e) + "|"; }
                h.c.m8--;
                out + to_string(h.c.m8);
            )");
            std::string expected;
            for (int i = 0; i < 7; ++i) expected += "C6 has no member 'm1'|";
            check_eq(expected + "0", r.as_string());
        });
        // Same clobber class: a receiver/base whose evaluation raised is a placeholder
        // null, and the interpreter's member read / member store / subscript store then
        // reported the placeholder ("accessed on null", "Cannot assign to rvalue
        // expression", a bare `true`) instead of the pending exception
        test("fuzz_raising_receiver_surfaces_original_exception", [this]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            auto r = e->execute(R"(
                function thrower() { throw "boom"; }
                var out = "";
                try { var x = thrower().m1; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { thrower().m1 = 1; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { thrower().m1 -= 1; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { thrower().m1--; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { thrower()[0] = 1; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { thrower()[0]--; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { thrower().m1(); out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                var arr = [1, 2];
                try { arr[thrower()]--; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { arr[thrower()] = 1; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                out;
            )");
            std::string expected;
            for (int i = 0; i < 9; ++i) expected += "boom|";
            check_eq(expected, r.as_string());
        });
        // A call is an lvalue when it returns T& (C++), which only the call can tell: the right-hand side, then the call, then a
        // by-value result raises naming the callee (strict reports a statically known one at load); a throw in either wins
        test("assignment_to_a_by_value_call_result_raises_after_the_call", [this]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            auto r = e->execute(R"([[warn_mode]]; 
                var cnt = 0;
                function bump() { cnt++; return 5; }
                function thrower() { throw "boom"; }
                var out = "";
                try { bump() = 1; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { bump() += 1; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { bump()++; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { bump() = bump(); out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { thrower()--; out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                try { bump() += thrower(); out += "unreached|"; } catch (e) { out += to_string(e) + "|"; }
                out + to_string(cnt);
            )");
            std::string expected;
            for (int i = 0; i < 4; ++i) expected += "Cannot assign to a call result: 'bump' returns by value|";
            check_eq(expected + "boom|boom|5", r.as_string());
        });
        test("fuzz_typed_fn_falls_off_end_returns_null", [this]() {
            auto e = make_engine();
            // strict rejects the missing return at load: the program opens a [[warn_mode]] region
            auto r = e->execute(R"([[warn_mode]]; 
                function f(int p) -> string { int g = 0; while (++g < 4) {} }
                f(1);
            )");
            check_true(r.is_null(), "typed fn falling off the end yields null (vm parity)");
            check_eq((int64_t)2, e->execute("1 + 1").as_int());
        });

        // ---- fuzz FZ-NEG-ZERO (seeds 1985/2148/2255): the interpreter's cached-zero
        // float fast path matched -0.0 (IEEE -0.0 == 0.0) and handed back +0.0 ----
        test("fuzz_neg_zero_preserved", [this]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            check_eq(std::string("-0.0"), e->execute(R"(
                double x = 0.0;
                to_string(-x);
            )").as_string());
            check_eq(std::string("-0.0"), e->execute("to_string(-(0.0))").as_string());
            check_eq(std::string("-0.0"), e->execute("to_string(0.0 * -1.0)").as_string());
        });
        test("fuzz_neg_zero_map_key_ordering", [this]() {
            // -0.0 and +0.0 are one map key (they are ==) on both backends
            auto e = make_engine();
            check_eq((int64_t)1, e->execute(R"(
                double z = 0.0;
                var m = {};
                m[-z] = "neg";
                m[z] = "pos";
                m.size();
            )").as_int());
        });

        // ---- fuzz FZ-OVERFLOW-OP-NAME (seed 2706): the counting-for update overflow
        // named the wrong operator, differently per backend (interp let i++ wrap and
        // blamed a later '*'; vm blamed '+='). The update now applies the overflow
        // policy on both backends and names the SOURCE operator. Found alongside:
        // the interpreter fast path negated literal '-=' steps twice, so
        // `for (...; i -= 2)` counted UP forever. ----
        test("fuzz_cfor_minus_eq_literal_descends", [this]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            check_eq(std::string("4;2;"), e->execute(R"(
                var out = "";
                for (int i = 4; i > 0; i -= 2) { out += to_string(i) + ";"; }
                out;
            )").as_string());
        });
        test("fuzz_cfor_update_overflow_names_plusplus", [this]() {
            std::string msg[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                if (!e->throw_on_overflow()) { return; }  // wrap build: loop exits via wrap
                try { e->execute("for (var i = 9223372036854775806; i > 0; i++) { }"); }
                catch (const std::exception& ex) { msg[idx] = ex.what(); }
                ++idx;
            }
            check_eq(msg[0], msg[1], "cfor ++ overflow text is byte-identical across backends");
            check_true(msg[0].find("Integer overflow in '++'") != std::string::npos,
                "cfor ++ overflow names the increment (got: " + msg[0] + ")");
        });
        test("fuzz_cfor_update_overflow_names_minus_eq", [this]() {
            std::string msg[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                if (!e->throw_on_overflow()) { return; }
                try { e->execute("for (var i = -9223372036854775807; i < 0; i -= 2) { }"); }
                catch (const std::exception& ex) { msg[idx] = ex.what(); }
                ++idx;
            }
            check_eq(msg[0], msg[1], "cfor -= overflow text is byte-identical across backends");
            check_true(msg[0].find("Integer overflow in '-='") != std::string::npos,
                "cfor -= overflow names the compound step (got: " + msg[0] + ")");
        });

        // ---- fuzz FZ-RECURSION-MSG-TEXT (seed 137): interp hit the native-stack guard
        // ("Native stack exhausted...") while the vm hit the depth cap, whose "{0}" was
        // passed the depth as a SYMBOL id and printed an empty "()" ----
        test("fuzz_recursion_limit_text_parity", [this]() {
            std::string msg[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);   // deep recursion outruns the default budget in Debug
                try { e->execute("function forever(int n) -> int { return n + forever(n - 1); } forever(3);"); }
                catch (const std::exception& ex) { msg[idx] = ex.what(); }
                ++idx;
            }
            check_eq(msg[0], msg[1], "recursion-limit error text is byte-identical across backends");
            check_true(msg[0].find("Maximum recursion depth exceeded") != std::string::npos,
                "recursion-limit message is independent of the configured backend cap (got: " + msg[0] + ")");
        });

        // ---- fuzz FZ-UNDEF-VAR-THIS-DECORATION (seeds 34/28): compound-assign to an
        // undefined identifier resolved the TARGET before evaluating the rhs on the
        // interpreter, so `v1 += v1` blamed the lhs while the vm (C++17 order: rhs
        // first) blamed the rhs load. The "(no 'this' in scope)" decoration itself is
        // symmetric on both backends and stays. Strict (the default) rejects the
        // undeclared name at load, so the runtime ordering is pinned in a [[warn_mode]] region. ----
        test("fuzz_compound_undef_eval_order_parity", [this]() {
            auto run_both = [](const char* src, bool warn_region) {
                std::string msg[2];
                int idx = 0;
                for (bool use_vm : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    try { e->execute(std::string(warn_region ? "[[warn_mode]]; " : "") + src); }
                    catch (const std::exception& ex) { msg[idx] = ex.what(); }
                    ++idx;
                }
                return std::make_pair(msg[0], msg[1]);
            };
            auto expect_both = [&](const char* src, const std::string& needle, const std::string& what) {
                auto [i_msg, v_msg] = run_both(src, true);
                check_eq(i_msg, v_msg, what + " error text is byte-identical across backends");
                check_true(i_msg.find(needle) != std::string::npos,
                    what + " blames the right name (got: " + i_msg + ")");
                auto [i_strict, v_strict] = run_both(src, false);
                check_eq(i_strict, v_strict, what + " strict listing is byte-identical across backends");
                check_true(i_strict.find("static check failed") == 0 &&
                    i_strict.find("use of undeclared identifier 'v1'") != std::string::npos,
                    what + " is a strict load error (got: " + i_strict + ")");
            };
            // rhs load fails first (rhs-first sequencing): plain undefined-variable text
            expect_both("v1 += v1;", "Undefined variable 'v1'", "undef += same undef");
            expect_both("v1 += fn8();", "Undefined variable 'fn8'", "undef += undef call");
            // rhs fine, target undefined: symmetric decorated text
            expect_both("v1 += 1;", "Undefined variable 'v1' (no 'this' in scope)", "undef += literal");
        });
        // ---- fuzz seed 100991: the vm's register-file lift computed a lifting RIGHT
        // subtree into its temp before an operand-shaped LEFT subscript resolved, so
        // `v6[1] - (g7 + 3)` in a function body raised the rhs overflow where the
        // interpreter raises the lhs index error (left-to-right sequencing). The subscript
        // now materializes into a temp first (op_operand_temp) on every lift level. ----
        test("fuzz_lift_subscript_left_sequencing_parity", [this]() {
            auto run_both = [](const std::string& src) {
                std::string out[2];
                int idx = 0;
                for (bool use_vm : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    try { out[idx] = e->execute(src).to_string(); }
                    catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                    ++idx;
                }
                return std::make_pair(out[0], out[1]);
            };
            auto expect_both = [&](const std::string& src, const std::string& needle, const char* what) {
                auto [i_out, v_out] = run_both(src);
                check_eq(i_out, v_out, std::string(what) + " is byte-identical across backends");
                check_true(i_out.find(needle) != std::string::npos,
                    std::string(what) + " lands the expected text (got: " + i_out + ")");
            };
            const std::string oob = "Array index 1 out of bounds for array of size 0";
            const std::string big = "9223372036854775807";
            auto fn = [](const std::string& body) { return "function f() { " + body + " } f();"; };
            expect_both(fn("array<float> v6 = []; int g7 = " + big + "; auto v = v6[1] - (g7 + 3); return 1;"),
                oob, "typed subscript left of an overflowing rhs");
            expect_both(fn("array<var> v6 = []; int g7 = " + big + "; auto v = v6[1] - (g7 + 3); return 1;"),
                oob, "dynamic subscript left of an overflowing rhs");
            expect_both(fn("array<int> v6 = []; int z = 0; auto v = v6[1] - (1 / z); return 1;"),
                oob, "subscript left of a dividing rhs");
            expect_both(fn("array<int> v6 = []; int g7 = " + big + "; auto v = (v6[1] - (g7 + 3)) * 2; return 1;"),
                oob, "nested lift level");
            expect_both(fn("array<int> a = [7]; array<int> v6 = []; int g7 = " + big + "; auto v = a[0] - (v6[1] - (g7 + 3)); return 1;"),
                oob, "subscript left inside a lifted right subtree");
            expect_both(fn("array<var> g = [[1, 2], [3, 4]]; array<int> v6 = []; int g7 = " + big + "; auto v = g[1][0] - (v6[1] * (g7 + 3)); return 1;"),
                oob, "two-level subscript left, subscript inside the lifted rhs");
            expect_both(fn("array<int> v6 = []; int g7 = " + big + "; return v6[1] - (g7 + 3);"),
                oob, "fused return operand");
            expect_both("class C { array<int> a = []; int g = " + big + "; int m() { return a[1] - (g + 3); } } C().m();",
                oob, "bare-field subscript in a method");
            expect_both(fn("array<int> v6 = []; int g7 = " + big + "; auto l = [=]() -> int { return v6[1] - (g7 + 3); }; return l();"),
                oob, "captured subscript in a lambda");
            // fuzz seeds 4200550 / 5202391: a name user code can reach (an environment name, a captured local, `::name`) left of a
            // lifted right subtree reads before that subtree runs, so the first error and a value its operator writes are the interpreter's
            expect_both("[[warn_mode]]; " + fn("int g7 = " + big + "; auto v = v8 - (g7 + 3); return 1;"),
                "Undefined variable 'v8'", "undefined name left of an overflowing rhs");
            expect_both("[[warn_mode]]; if (g8 == (3 + v7)) { bool v16 = false; }", "Undefined variable 'g8'", "two undefined names in a top-level condition");
            const std::string writer = "var G = 1.0; var hook = null; class K { float operator*(float x) { G = 100.0; hook(); return x; } } var k = K();\n";
            expect_both(writer + "function t() { hook = []() {}; return G - (k * 2.5); } t();", "-1.5", "global left of an operator that writes it");
            expect_both(writer + "function t() { hook = []() {}; return ::G - (k * 2.5); } t();", "-1.5", "::name left of an operator that writes it");
            expect_both(writer + "function t() { float a = 1.0; hook = [&]() { a = 100.0; }; return a - (k * 2.5); } t();", "-1.5",
                "captured local left of an operator that writes it");
            // a sound left read still lets the rhs raise, and materialized operands compute as before
            expect_both(fn("array<int> a = [7]; int g7 = " + big + "; auto v = a[0] - (g7 + 3); return 1;"),
                "Integer overflow in '+'", "sound left read then rhs overflow");
            expect_both(fn("array<int> a = [7]; int x = 3; int y = 4; return a[0] - (x * y);"), "-5", "materialized int operand");
            expect_both(fn("array<float> a = [1.5]; int x = 3; int y = 4; return a[0] * (x + y);"), "10.5", "materialized float operand");
            expect_both(fn("array<string> a = [\"ab\"]; string s = \"c\"; string t = \"d\"; return a[0] + (s + t);"), "abcd", "materialized string operand");
            expect_both(fn("array<var> g = [[1, 2], [3, 4]]; int x = 3; int y = 4; return g[1][0] - (x * y);"), "-9", "materialized two-level operand");
            expect_both(fn("array<int> a = [7]; int x = 3; int y = 4; int acc = 0; for (int i = 0; i < 3; i++) { acc += a[0] - (x * y) + i; } return acc;"), "-12", "materialized operand across loop iterations");
        });
        // ---- demoreel CANYONRUN (vm): a field container subscripted by an inlined call materializes
        // into its temp ahead of the call (op_operand_temp); the field's copy lived in a scratch local
        // to the subscript reader, so the temp copied a dead value and the index read crashed ----
        test("field_container_subscripted_by_an_inlined_call", [this]() {
            const std::string src =
                "function twice(int v) -> int { return v * 2; }\n"
                "function ifloor(float v) -> int { return math::ifloor(v); }\n"
                "class Canyon {\n"
                "\tvar hmap = [1, 2, 3, 4];\n"
                "\tarray<int> ahmap = [10, 20, 30, 40];\n"
                "\tmap<int, int> m = {2: 7};\n"
                "\tstring s = \"abcd\";\n"
                "\tfloat posx = 1.5;\n"
                "\tint ip = 1;\n"
                "\tint probe() { return hmap[twice(ip)]; }\n"
                "\tint typed() { return ahmap[twice(ip)]; }\n"
                "\tint floored() { int g0 = hmap[ifloor(posx)]; return g0; }\n"
                "\tint mixed() { return ahmap[((ifloor(posx) & 3) * 2) + (twice(ip) & 1)]; }\n"
                "\tint keyed() { return m[twice(ip)]; }\n"
                "\tstring letter() { return to_string(s[twice(ip)]); }\n"
                "\tint looped() { int acc = 0; for (int k = 0; k < 3; k++) { acc += ahmap[twice(k) & 3]; } return acc; }\n"
                "}\n"
                "auto c = Canyon();\n"
                "to_string(c.probe()) + \" \" + to_string(c.typed()) + \" \" + to_string(c.floored()) + \" \" + to_string(c.mixed()) + \" \" +\n"
                "\tto_string(c.keyed()) + \" \" + c.letter() + \" \" + to_string(c.looped());\n";
            for (bool use_vm : {false, true}) {
                for (bool proofs : {true, false}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    jai::stdlib::register_all(*e);
                    if (!proofs) { e->type_proofs(false); }
                    std::string out;
                    try { out = e->execute(src).to_string(); }
                    catch (const std::exception& ex) { out = std::string("ERROR: ") + ex.what(); }
                    check_eq(std::string("3 30 2 30 7 c 50"), out,
                        std::string(use_vm ? "vm" : "interpreter") + (proofs ? "" : " (proofs off)"));
                }
            }
        });
        // ---- A member store on a non-object receiver raised the bool `true` on the
        // interpreter (make_value(const char*) picked the bool overload) while the vm raised
        // the text; the caught value is the string on both. ----
        test("member_store_on_non_object_raises_string", [this]() {
            auto run_both = [](const char* src) {
                std::string out[2];
                int idx = 0;
                for (bool use_vm : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    jai::stdlib::register_all(e);
                    try { out[idx] = e->execute(std::string("[[warn_mode]]; ") + src).to_string(); }
                    catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                    ++idx;
                }
                return std::make_pair(out[0], out[1]);
            };
            auto expect_both = [&](const char* src, const std::string& expected, const char* what) {
                auto [i_out, v_out] = run_both(src);
                check_eq(expected, i_out, std::string("interp: ") + what);
                check_eq(expected, v_out, std::string("vm: ") + what);
            };
            expect_both("function f() {} string r = \"\"; try { f.x = 1; } catch (e) { r = type_of(e) + \":\" + to_string(e); } r",
                "error:Cannot assign to member of non-object type", "store on a function");
            expect_both("var n = 5; string r = \"\"; try { n.x = 1; } catch (e) { r = type_of(e) + \":\" + to_string(e); } r",
                "error:Cannot assign to member of non-object type", "store on an int");
            expect_both("class C { static int s = 0; } string r = \"\"; try { C.s = 8; } catch (e) { r = type_of(e) + \":\" + to_string(e); } r",
                "error:'C' is a class; use C::s", "dot store on a class name");
        });
        // ---- returnattr: a failed typed-return conversion raises at the `return` with the
        // returning frame on top (the vm captures before the callee frame pops, the
        // interpreter before cleanup; a resumed coroutine's frames sit above the resumer's
        // on both). what() and format_stack_trace() are byte-identical across backends. ----
        test("return_conversion_attribution_parity", [this]() {
            auto run_both = [](const char* src) {
                std::string out[2];
                int idx = 0;
                for (bool use_vm : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    try { e->execute(src); out[idx] = "OK"; }
                    catch (const std::exception& ex) { out[idx] = std::string(ex.what()) + "\n" + e->format_stack_trace(); }
                    ++idx;
                }
                return std::make_pair(out[0], out[1]);
            };
            auto expect_both = [&](const char* src, const std::string& expected, const char* what) {
                auto [i_out, v_out] = run_both(src);
                check_eq(expected, i_out, std::string("interp: ") + what);
                check_eq(expected, v_out, std::string("vm: ") + what);
            };
            auto mismatch = [](const char* who) {
                return std::string("Cannot return string from ") + who + " (declared -> int)\n";
            };
            expect_both("int f() {\n  var x = \"s\";\n  return x;\n}\nint g() { return 1; }\nint a = g();\nint b = f();\n",
                mismatch("'f'") + "  at f (<script>:3)\n  at <script> (<script>:7)\n", "plain return");
            expect_both("int f() {\n  var x = \"s\";\n  return x;\n}\nint g() {\n  int r = f();\n  return r;\n}\nint b = g();\n",
                mismatch("'f'") + "  at f (<script>:3)\n  at g (<script>:6)\n  at <script> (<script>:9)\n", "nested return");
            expect_both("var x = \"s\";\nauto l = []() -> int {\n  return x;\n};\nint b = l();\n",
                mismatch("lambda") + "  at <lambda> (<script>:3)\n  at <script> (<script>:5)\n", "lambda return");
            expect_both("class C {\n  int m() {\n    var x = \"s\";\n    return x;\n  }\n}\nauto c = C();\nint b = c.m();\n",
                mismatch("'m'") + "  at m (<script>:4)\n  at <script> (<script>:8)\n", "method return");
            expect_both("coroutine int co() {\n  yield 1;\n  var x = \"s\";\n  return x;\n}\nauto h = co();\nvar a = h.next();\nvar b = h.next();\n",
                mismatch("'co'") + "  at co (<script>:4)\n  at <script> (<script>:8)\n", "coroutine return, top-level resume");
            expect_both("coroutine int co() {\n  yield 1;\n  var x = \"s\";\n  return x;\n}\nauto h = co();\nint g() {\n  h.next();\n  return h.next();\n}\nint b = g();\n",
                mismatch("'co'") + "  at co (<script>:4)\n  at g (<script>:9)\n  at <script> (<script>:11)\n", "coroutine return, nested resume");
            expect_both("coroutine int co() {\n  var x = \"s\";\n  return x;\n}\nauto h = co();\nint g() {\n  return h.next();\n}\nint b = g();\n",
                mismatch("'co'") + "  at co (<script>:3)\n  at g (<script>:7)\n  at <script> (<script>:9)\n", "coroutine return, first resume");
            expect_both("coroutine int co() {\n  yield 1;\n  var a = [];\n  int y = a[0];\n  return y;\n}\nauto h = co();\nvar a = h.next();\nvar b = h.next();\n",
                "Array index 0 out of bounds for array of size 0\n  at co (<script>:4)\n  at <script> (<script>:9)\n", "coroutine body error");
            expect_both("coroutine int co() {\n  yield 1;\n  throw \"x\";\n  return 1;\n}\nauto h = co();\nvar a = h.next();\nvar b = h.next();\n",
                "x\n  at co (<script>:3)\n  at <script> (<script>:8)\n", "coroutine body throw");
            expect_both("int& f() {\n  return 5;\n}\nint b = f();\n",
                "Function with reference return type must return an lvalue reference\n  at f (<script>:2)\n  at <script> (<script>:4)\n", "ref return");
            // returncatch: a try around the failing return INSIDE the callee catches it; the
            // catch's own return converts in turn and a rethrow keeps the callee frame on top
            expect_both("int f() {\n  try { var x = \"s\"; return x; } catch (e) { return -1; }\n}\nthrow f() == -1 ? \"callee:-1\" : \"callee:other\";\n",
                "callee:-1\n  at <script> (<script>:4)\n", "caught at the return site");
            expect_both("int f() {\n  try {\n    var x = \"s\";\n    return x;\n  } catch (e) {\n    var y = \"t\";\n    return y;\n  }\n}\nint b = f();\n",
                mismatch("'f'") + "  at f (<script>:7)\n  at <script> (<script>:10)\n", "catch return converts in turn");
            expect_both("int f() {\n  try {\n    var x = \"s\";\n    return x;\n  } catch (e) {\n    throw e;\n  }\n}\nint b = f();\n",
                "Cannot return string from 'f' (declared -> int)\n  at f (<script>:6)\n  at <script> (<script>:9)\n", "catch rethrow");
            expect_both("int& f() {\n  try { return 5; } catch (e) { throw \"c:\" + e; }\n}\nint b = f();\n",
                "c:Function with reference return type must return an lvalue reference\n  at f (<script>:2)\n  at <script> (<script>:4)\n", "ref return caught at the site");
            expect_both("coroutine int co() {\n  yield 1;\n  try { var x = \"s\"; return x; } catch (e) { return -1; }\n}\nauto h = co();\nh.next();\nthrow h.next() == -1 ? \"co:-1\" : \"co:other\";\n",
                "co:-1\n  at <script> (<script>:7)\n", "coroutine return caught at the site");
            expect_both("class T { int to_int() { throw \"boom\"; } }\nint f() {\n  try { var t = T(); return t; } catch (e) { return -7; }\n}\nthrow f() == -7 ? \"t:-7\" : \"t:other\";\n",
                "t:-7\n  at <script> (<script>:5)\n", "throwing to_int caught at the site");
        });
        // ---- The typed-boundary texts name the VALUE's type (never the var tag): parameters,
        // returns and class targets, byte-identical across backends. ----
        test("typed_boundary_error_names_dynamic_type", [this]() {
            auto run_both = [](const char* src) {
                std::string out[2];
                int idx = 0;
                for (bool use_vm : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    try { out[idx] = e->execute(src).to_string(); }
                    catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                    ++idx;
                }
                return std::make_pair(out[0], out[1]);
            };
            auto expect_both = [&](const char* src, const std::string& expected, const char* what) {
                auto [i_out, v_out] = run_both(src);
                check_eq(expected, i_out, std::string("interp: ") + what);
                check_eq(expected, v_out, std::string("vm: ") + what);
            };
            const char* tail = " string r = \"\"; try { f(); } catch (e) { r = e; } r";
            auto ret = [&](const char* decl) { return std::string(decl) + " int f() { return v; }" + tail; };
            expect_both(ret("var v = \"s\";").c_str(), "Cannot return string from 'f' (declared -> int)", "string return");
            expect_both(ret("var v = cast<char>(97);").c_str(), "Cannot return char from 'f' (declared -> int)", "char return");
            expect_both(ret("var v = null;").c_str(), "Cannot return null from 'f' (declared -> int)", "null return");
            expect_both(ret("class Cat {} var v = Cat();").c_str(), "Cannot return Cat from 'f' (declared -> int)", "object return");
            expect_both(ret("var v = [1];").c_str(), "Cannot return array from 'f' (declared -> int)", "array return");
            expect_both(ret("var v = {\"a\": 1};").c_str(), "Cannot return map from 'f' (declared -> int)", "map return");
            expect_both((std::string("class Cat {} var v = true; Cat f() { return v; }") + tail).c_str(),
                "Cannot convert bool to Cat (no suitable single-argument constructor)", "bool into class return");
            expect_both((std::string("class Cat {} var v = 1.5; Cat f() { return v; }") + tail).c_str(),
                "Cannot convert float to Cat (no suitable single-argument constructor)", "float into class return");
            expect_both("void h(int a) {} var x = \"s\"; string r = \"\"; try { h(x); } catch (e) { r = e; } r",
                "Cannot pass string to parameter 'a' of 'h' (declared int)", "string parameter");
            expect_both("class A {} class B {} void fa(A v) {} var b = B(); string r = \"\"; try { fa(b); } catch (e) { r = e; } r",
                "Cannot convert B to A (no suitable single-argument constructor)", "class parameter");
        });
        test("fuzz_compound_undef_rhs_side_effects_run_first", [this]() {
            const char* src = R"(
                var log = "";
                function s() { log += "x"; return 1; }
                try { v1 += s(); } catch (err) { log += "c"; }
                log;
            )";
            auto strict = make_engine();
            check_throws<static_check_error>([&]() { strict->execute(src); });
            auto e = make_engine();
            check_eq(std::string("xc"), e->execute(std::string("[[warn_mode]]; ") + src).as_string());
            // strict admits the same ordering through a typed target: the rhs call runs,
            // then the var-sourced string fails the int store
            auto typed = make_engine();
            check_eq(std::string("xc"), typed->execute(R"(
                var log = "";
                var bad = "s";
                function s() { log += "x"; return bad; }
                int t = 0;
                try { t += s(); } catch (err) { log += "c"; }
                log;
            )").as_string());
        });

        // ---- fuzz KD-CORO-RESUME-PAST-END (seeds 4/38/98/195/216/387/626/840/...):
        // resuming past the final yield of a no-return coroutine: the interpreter
        // replayed the stale last-yielded value (and a zero-yield METHOD coroutine
        // returned its receiver via the implicit-'this' rule) while the vm completed
        // with null. A finished body completes with null on both backends, like any
        // function falling off the end; explicit returns are unaffected. ----
        test("fuzz_coro_resume_past_end_yields_null", [this]() {
            const char* src = R"(
                coroutine int co() { yield 10; yield 11; }
                auto h = co();
                var a = to_string(h.next()) + "|" + to_string(h.next()) + "|" + to_string(h.next());
                class D { int v = 7; coroutine int g() { v = 8; } }
                auto d = D();
                auto hm = d.g();
                var b = to_string(hm.next()) + "|" + to_string(d.v);
                class F { int q = 3; coroutine int g2() { yield 1; return this.q; } }
                auto f = F();
                auto hf = f.g2();
                var c = to_string(hf.next()) + "|" + to_string(hf.next());
                a + " / " + b + " / " + c;
            )";
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                stdlib::register_all(*e);
                out[idx++] = e->execute(src).as_string();
            }
            check_eq(std::string("10|11|null / null|8 / 1|3"), out[0], "interp: past-end resume completes null");
            check_eq(out[0], out[1], "backends agree byte-identically");
        });

        // ---- builtin methods through references ----
        // String builtins invoked through a ref DECL segfaulted on both backends: the
        // identifier-receiver fast path passed the RAW stored value (a reference wrapper)
        // as `self`, and string builtins read self's storage unchecked. Array/map
        // builtins and ref PARAMS took other paths and worked - this pins the full
        // matrix: {string, array, map} x {ref decl, ref param, range-for auto& element},
        // observer AND mutating (mutation must land in the referenced target).
        test("builtin_methods_through_references_observe_and_mutate_the_target", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"string_builtins_through_ref_decl", R"(
                auto s = "abc";
                auto& r = s;
                int n = r.length();
                r.to_upper();
                to_string(n) + "|" + s + "|" + r;
            )", std::string("3|ABC|ABC")},
                {"string_builtins_through_ref_param", R"(
                auto s = "abc";
                function up(var& t) -> int { t.to_upper(); return t.length(); }
                int n = up(s);
                to_string(n) + "|" + s;
            )", std::string("3|ABC")},
                {"string_builtins_through_range_for_ref", R"(
                var strs = ["ab", "cd"];
                for (auto& s : strs) { s.to_upper(); }
                strs[0] + strs[1];
            )", std::string("ABCD")},
                {"array_map_builtins_through_ref_decl", R"(
                var arr = [1, 2, 3];
                auto& ra = arr;
                ra.push(4);
                var m = {"k": 1};
                auto& rm = m;
                rm["j"] = 2;
                to_string(ra.size()) + "|" + to_string(arr.size()) + "|" +
                to_string(rm.size()) + "|" + to_string(m.size());
            )", std::string("4|4|2|2")},
                {"array_map_builtins_through_ref_param", R"(
                function grow(var& a) { a.push(9); }
                function put(var& m) { m["x"] = 5; }
                var arr = [1];
                var mp = {"k": 1};
                grow(arr);
                put(mp);
                to_string(arr.size()) + "|" + to_string(arr[1]) + "|" + to_string(mp.size());
            )", std::string("2|9|2")},
            };
            check_values(cases, true);
        });

        // The string copy-on-write gate at the host boundary: a
        // script_value copy the host keeps is a VALUE - a later script mutation of the
        // variable never reaches it; a host std::string& writes back into its argument's
        // variable alone (a copy that shared its text keeps it); a bound global keeps
        // write-through; the unshare charges the memory cap like any internal clone.
        test("string_cow_host_boundary", [this]() {
            for (bool use_vm : {false, true}) {
                const std::string tag = use_vm ? "vm: " : "interp: ";
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                e->execute("string a = \"x\";");
                script_value held = e->execute("a");
                e->execute("a.to_upper(); a += \"!\";");
                check_eq(std::string("x"), held.as<std::string>(), tag + "a host-held copy keeps its value");
                check_eq(std::string("X!"), e->execute("a").as<std::string>(), tag + "the variable took both writes");
                e->add_function("up", [](std::string& s) { s += "!"; });
                check_eq(std::string("y|y!"), e->execute("string p = \"y\"; string q = p; up(q); p + \"|\" + q").as<std::string>(),
                         tag + "host std::string& writes back into q alone");
                std::string live = "b";
                e->add_global_ref("live", live);
                e->execute("live.to_upper(); up(live);");
                check_eq(std::string("B!"), live, tag + "bound strings keep write-through");
                e->execute("string c = live; c.to_lower();");
                check_eq(std::string("B!"), live, tag + "a copy of a bound string detaches on write");
                check_eq(std::string("b!"), e->execute("c").as<std::string>(), tag + "the copy took the write");
            }
        });

        test("string_cow_unshare_charges_memory_cap", [this]() {
            for (bool use_vm : {false, true}) {
                const std::string tag = use_vm ? "vm: " : "interp: ";
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->memory_cap(65536);
                auto r = e->execute(R"(
                    string big = "";
                    int i = 0;
                    while (i < 4000) { big += "0123456789"; i += 1; }
                    string c = big;
                    string msg = "";
                    try { c.to_upper(); int n = 0; while (n < 10) { n += 1; } } catch (err) { msg = err; }
                    msg;
                )");
                check_eq(std::string("Script memory cap exceeded (65536 bytes) - raise engine::memory_cap or break up the work"),
                         r.as<std::string>(), tag + "the unshared copy counts toward the cap");
            }
        });

        // ---- Decl fast path (variable_decl::decl_fast_flags): slot decls of matching
        // scalar payloads store without enforce/clone/homogeneity. These pin the
        // semantics the fast path must preserve on BOTH backends; the mismatch shapes
        // must keep falling through to the full path. ----
        test("decl_fast_typed_conversion_still_applies", [this]() {
            auto e = make_engine();
            check_eq((int64_t)1, e->execute("auto f() { int d = true; return d; } f();").as_int());
            check_near(5.0, e->execute("auto g() { float x = 5; return x; } g();").as_float(), 1e-9);
        });
        test("the_decl_fast_path_keeps_conversions_copies_and_boxing", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"decl_fast_typed_mismatch_still_errors", R"(auto f() { int x = "s"; return x; } f();)", raises{}},
                {"decl_fast_element_init_value_copies", R"(
                auto f() {
                    var arr = [1, 2, 3];
                    int total = 0;
                    for (int i = 0; i < 3; ++i) {
                        int e = arr[i];
                        e = e + 90;
                        total += e;
                    }
                    return arr[0] + arr[1] + arr[2] + total;
                }
                f();
            )", int64_t{282}},
                {"decl_fast_typed_from_local_no_alias", "auto f() { int a = 5; int b = a; b = 7; return a * 10 + b; } f();", int64_t{57}},
                {"decl_fast_loop_decls", R"(
                auto f() {
                    int total = 0;
                    for (int i = 0; i < 100; ++i) {
                        int x = i + 1;
                        var y = x * 2;
                        total += y;
                    }
                    return total;
                }
                f();
            )", int64_t{10100}},
                {"decl_fast_shared_weak_decls_unaffected", R"(
                class P { int v = 1; }
                auto f() {
                    shared_ptr<P> a = P();
                    var b = a;
                    b.v = 7;
                    weak_ptr<P> w = a;
                    int alive = 0;
                    if (!w.expired()) { alive = 1; }
                    return a.v * 10 + alive;
                }
                f();
            )", int64_t{71}},
                {"decl_fast_ref_escaping_decl_still_boxed", "function set42(int& t) { t = 42; } function f() -> int { int x = 1; set42(x); return x; } f();", int64_t{42}},
            };
            check_values(cases);
        });

        test("decl_fast_var_stays_dynamic", [this]() {
            auto e = make_engine();
            check_eq(std::string("str"), e->execute(R"(auto f() { var v = 5; v = "str"; return v; } f();)").as_string());
            // var decl of a scalar must be tagged 'any', not the payload type
            check_near(6.5, e->execute("auto g() { var q = 5; q = 6.5; return q; } g();").as_float(), 1e-9);
        });
        test("decl_fast_auto_locks_inferred_type", [this]() {
            auto e = make_engine();
            check_eq((int64_t)6, e->execute("auto f() { auto a = 5; a = 6; return a; } f();").as_int());
            check_throws([&]() { e->execute(R"(auto f() { auto a = 5; a = "s"; return a; } f();)"); });
        });
        test("decl_fast_float_bool_char_decls", [this]() {
            auto e = make_engine();
            check_near(3.0, e->execute(R"(
                auto f() {
                    float acc = 0.0;
                    for (int i = 0; i < 4; ++i) {
                        float h = i * 0.5;
                        acc += h;
                    }
                    return acc;
                }
                f();
            )").as_float(), 1e-9);
            // bool from comparison takes the fast path; bool from int still converts (truthy)
            check_eq((int64_t)3, e->execute(R"(
                auto g() {
                    bool b = 1 == 1;
                    bool c = 5;
                    int r = 0;
                    if (b) { r += 1; }
                    if (c) { r += 2; }
                    return r;
                }
                g();
            )").as_int());
            check(e->execute("auto h() { char c = 'x'; return c == 'x'; } h();").as_bool());
            check_eq((int64_t)1, e->execute("auto k() { int a = true; return a; } k();").as_int());
        });
        // ---- Callee-first for member and computed callees (C++17 [expr.call]; the
        // interpreter is the oracle): `recv.m(args)` evaluates the receiver and looks the
        // member up BEFORE any argument runs, so null-receiver / unknown-member /
        // not-callable errors beat argument side effects (the vm used to evaluate the
        // arguments first - fuzz seed 132479 blamed 'length' for `v8.push(v8.length())`
        // on a null v8). Overload resolution stays post-args. Also the scalar-receiver
        // text: `n.nosuch` on an int/float/bool/char/function receiver reads
        // `int has no member 'nosuch'` (both backends raised an EMPTY text). Each program
        // logs argument side effects into `log` and returns `log + "|" + caught`. ----
        // warn_region: the program opens a [[warn_mode]] region on its first line (the checker reports, the load proceeds)
        auto run_order_both = [](const char* src, bool warn_region) {
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                try {
                    auto r = e->execute(std::string(warn_region ? "[[warn_mode]]; " : "") + src);
                    out[idx] = r.is_string() ? r.as_string() : std::string("<non-string>");
                } catch (const std::exception& ex) { out[idx] = std::string("what:") + ex.what(); }
                ++idx;
            }
            return std::make_pair(out[0], out[1]);
        };
        auto expect_order = [this, run_order_both](const char* src, const std::string& expected,
                                                   const std::string& what, bool warn_region) {
            auto [i, v] = run_order_both(src, warn_region);
            check_eq(expected, i, what + " (interpreter)");
            check_eq(i, v, what + " parity");
            return i;
        };
        const char* side_prelude = "var log = \"\"; int side() { log += \"A\"; return 1; }\n";
        test("callee_first_method_null_receiver_before_args", [this, expect_order, run_order_both, side_prelude]() {
            expect_order((std::string(side_prelude) + R"(
                var v8 = null; var caught = "";
                try { v8.push(side()); } catch (e) { caught = e; }
                log + "|" + caught;
            )").c_str(), "|Cannot call 'push' on null ('v8' is null)", "null receiver beats arg side effects", false);
            // the fuzz shape (seed 132479): the callee's null error names the CALLEE member
            auto [i, v] = run_order_both(R"(
                auto v7 = [4611686018427387904, 5]; v7.reverse(); var v8 = null; v8.push(v8.length());
            )", false);
            check_eq(i, v, "uncaught null-receiver what() parity");
            check_true(i.find("Cannot call 'push' on null ('v8' is null)") != std::string::npos,
                "the callee member is blamed, not the argument's (got: " + i + ")");
            expect_order((std::string(side_prelude) + R"(
                var v8 = null; var r = v8?.push(side());
                `${log}|${(r == null)}`;
            )").c_str(), "|true", "null-safe call skips the arguments", false);
        });
        test("scalar_receiver_unknown_member_text", [this, expect_order, run_order_both, side_prelude]() {
            const std::pair<const char*, const char*> receivers[] = {
                {"5", "int"}, {"1.5", "float"}, {"true", "bool"}, {"'c'", "char"}, {"side", "function"},
            };
            for (const auto& [init, type_name] : receivers) {
                const std::string src = std::string(side_prelude) +
                    "var n = " + init + "; var caught = \"\";\n"
                    "try { n.nosuch(side()); } catch (e) { caught = e; }\n"
                    "log + \"|\" + caught;";
                expect_order(src.c_str(), std::string("|") + type_name + " has no member 'nosuch'",
                    std::string("scalar receiver ") + type_name, false);
            }
            expect_order(R"(
                var n = 5; var caught = "";
                try { var z = n.nosuch; } catch (e) { caught = e; }
                caught;
            )", "int has no member 'nosuch'", "scalar member read", false);
            auto [i, v] = run_order_both("var n = 5; n.nosuch(1);", false);
            check_eq(i, v, "uncaught scalar-member what() parity");
            check_true(i.find("int has no member 'nosuch'") != std::string::npos,
                "uncaught scalar-member text (got: " + i + ")");
            // container receivers keep their own texts, still before the arguments
            expect_order((std::string(side_prelude) + R"(
                var s = "abc"; var caught = "";
                try { s.nosuch(side()); } catch (e) { caught = e; }
                log + "|" + caught;
            )").c_str(), "|string has no member 'nosuch'", "string receiver", false);
            expect_order((std::string(side_prelude) + R"(
                var a = [1]; var caught = "";
                try { a.nosuch(side()); } catch (e) { caught = e; }
                log + "|" + caught;
            )").c_str(), "|array has no member 'nosuch'", "array receiver", false);
            expect_order((std::string(side_prelude) + R"(
                var m = {"k": 1}; var caught = "";
                try { m.nosuch(side()); } catch (e) { caught = e; }
                log + "|" + caught;
            )").c_str(), "|map has no member 'nosuch'; read the key as [\"nosuch\"]", "map receiver", false);
            expect_order((std::string(side_prelude) + R"(
                class W { int x = 0; }
                shared_ptr<W> o = W(); weak_ptr<W> w = o; var caught = "";
                try { w.nosuch(side()); } catch (e) { caught = e; }
                log + "|" + caught;
            )").c_str(), "|weak_ptr has no member 'nosuch'", "weak_ptr receiver", true);
        });
        test("object_receiver_lookup_before_args", [this, expect_order, side_prelude]() {
            const std::string cls = std::string(side_prelude) + R"(
                class O {
                    int x = 1;
                    var lam = null;
                    int f(int a) { return a; }
                    int f(int a, int b) { return a + b; }
                }
                var obj = O(); var caught = "none";
            )";
            expect_order((cls + R"(
                try { obj.nosuch(side()); } catch (e) { caught = e; }
                log + "|" + caught;
            )").c_str(), "|O has no member 'nosuch'", "unknown member beats arg side effects", true);
            // a plain field is not callable: the check fires before the arguments
            expect_order((cls + R"(
                try { obj.x(side()); } catch (e) { caught = `c:${e}`; }
                log + "|" + caught;
            )").c_str(), "|c:Cannot call 'x': it is an int, not a function", "field shadow not-callable beats arg side effects", true);
            expect_order((cls + R"(
                obj.lam = [](int a) { return a * 2; };
                var r = obj.lam(side());
                `${log}|${r}`;
            )").c_str(), "A|2", "callable field still calls once", true);
            // overload resolution needs the argument values: stays post-args
            expect_order((cls + R"(
                var r1 = obj.f(side()); var r2 = obj.f(side(), 2);
                `${log}|${r1}:${r2}`;
            )").c_str(), "AA|1:3", "multi-overload method resolves post-args", true);
        });
        test("nested_callee_lookup_order", [this, expect_order, side_prelude]() {
            const std::string cls = std::string(side_prelude) + R"(
                class C { int h() { log += "H"; return 1; } }
                class A { int f(int v) { return v; } }
                class A2 { int z = 0; }
                var c = C(); var caught = "";
            )";
            expect_order((cls + R"(
                var a = A(); var b = null;
                try { a.f(b.g(c.h())); } catch (e) { caught = e; }
                log + "|" + caught;
            )").c_str(), "|Cannot call 'g' on null ('b' is null)", "inner null receiver beats the innermost call", true);
            expect_order((cls + R"(
                var a = null; var b = null;
                try { a.f(b.g(c.h())); } catch (e) { caught = e; }
                log + "|" + caught;
            )").c_str(), "|Cannot call 'f' on null ('a' is null)", "outer null receiver beats everything", true);
            expect_order((cls + R"(
                var a = A2(); var b = null;
                try { a.f(b.g(c.h())); } catch (e) { caught = e; }
                log + "|" + caught;
            )").c_str(), "|A2 has no member 'f'", "outer unknown member beats the inner null", true);
        });
        test("trivial_args_unchanged", [this, expect_order, side_prelude]() {
            expect_order(R"(
                class O { int f(int a, int b, string s) { return a + b + s.length(); } }
                string run() {
                    var obj = O(); int local = 2; var arr = [1];
                    arr.push(local);
                    return `${obj.f(1, local, "abc")}:${arr.length()}`;
                }
                run();
            )", "6:2", "slot-local / literal arguments", false);
            expect_order((std::string(side_prelude) + R"(
                var v8 = null; var n = 5;
                var c1 = ""; var c2 = ""; var c3 = ""; var c4 = "";
                try { v8.push(1); } catch (e) { c1 = e; }
                try { v8.push(side()); } catch (e) { c2 = e; }
                try { n.nosuch(1); } catch (e) { c3 = e; }
                try { n.nosuch(side()); } catch (e) { c4 = e; }
                `${log}|${(c1 == c2)}:${(c3 == c4)}|${c1}|${c3}`;
            )").c_str(), "|true:true|Cannot call 'push' on null ('v8' is null)|int has no member 'nosuch'",
                "probed and unprobed shapes raise the same texts", false);
        });
        test("computed_callee_callable_check_before_args", [this, expect_order, side_prelude]() {
            expect_order((std::string(side_prelude) + R"(
                var arr = [1]; var caught = "none";
                try { arr[0](side()); } catch (e) { caught = `c:${e}`; }
                log + "|" + caught;
            )").c_str(), "|c:Cannot call a value of type int: it is not a function", "non-callable element beats arg side effects", false);
            expect_order((std::string(side_prelude) + R"(
                var fs = [[](int a) -> int { return a * 2; }];
                var r = fs[0](side());
                `${log}|${r}`;
            )").c_str(), "A|2", "callable element still calls", false);
            expect_order((std::string(side_prelude) + R"(
                function mk() -> auto { return [](int a) -> int { return a + 100; }; }
                var r = mk()(side());
                `${log}|${r}`;
            )").c_str(), "A|101", "call-result callee still calls", false);
        });
        test("operator_overloads_unaffected", [this, expect_order, run_order_both]() {
            expect_order(R"(
                var log = "";
                class V {
                    int v = 0;
                    V(int x) { v = x; }
                    function operator+(V o) -> V { log += "+"; return V(v + o.v); }
                }
                V make() { log += "M"; return V(2); }
                var a = V(1);
                var r = a + make();
                `${log}|${r.v}`;
            )", "M+|3", "operator overload call", false);
            auto [i, v] = run_order_both(R"(
                var log = "";
                class V { int v = 0; V(int x) { v = x; } function operator+(V o) -> V { return V(v + o.v); } }
                V make() { log += "M"; return V(2); }
                var n = null; var caught = "none";
                try { var q = n + make(); } catch (e) { caught = "c"; }
                log + "|" + caught;
            )", false);
            check_eq(i, v, "null left operand parity");
        });
        test("coroutine_calls_unaffected", [this, expect_order, side_prelude]() {
            // Method calls inside a coroutine body, resumer-side method calls between
            // resumes, and a handle method taking a non-trivial argument. (A yield INSIDE
            // a call argument - `acc.add((yield 1) ? 0 : 5)` - is a pre-existing
            // interpreter replay divergence, free-function callees included; not pinned.)
            expect_order(R"(
                class Acc { int total = 0; int add(int v) { total += v; return total; } }
                var acc = Acc();
                coroutine int gen() {
                    var t = (yield 1) ? 0 : 5;
                    acc.add(t + 0);
                    return acc.total;
                }
                var h = gen();
                var first = h.next();
                var acc2 = Acc(); var between = acc2.add(2 + 3);
                var final = h.next();
                `${first}:${between}:${final}:${h.done()}`;
            )", "1:5:5:true", "method calls around a yield", false);
            expect_order((std::string(side_prelude) + R"(
                coroutine int gen() { yield 1; return 2; }
                var h = gen(); h.next(); h.next();
                string message = ""; try { h.done(side()); } catch (e) { message = e; }
                log + "|" + message;
            )").c_str(), "A|done() takes no arguments", "handle arguments are evaluated before arity rejection", false);
        });

        // ---- Compound member assignment evaluates its receiver ONCE (C++ `a.b op= c`
        // evaluates `a` exactly once): `pk().v += 5`, `pk().v++`, `++pk().v`,
        // `arr[side()].v += 1`, `mp[key()].v++`, nested chains and operator-overloaded
        // compounds used to run the receiver expression twice on BOTH backends (the
        // interpreter re-dispatched the object for the store-back, the vm compiled it
        // twice). Order stays target-read -> rhs -> arithmetic -> store. The interpreter's
        // general subscript path lost its synthetic re-dispatching fallback: a compound
        // whose target read yields no lvalue raises `Cannot assign to rvalue expression`
        // (the vm's text) - `s[i] op= v` on a string and `m[k] += "s"` on a missing key
        // (which silently inserted "nulls" on the interpreter alone) now say so. ----
        const char* recv_prelude =
            "var log = \"\";\n"
            "class P { int v = 10; }\n"
            "P shared = P();\n"
            "P& pk() { log += \"A\"; return shared; }\n";
        test("member_compound_receiver_once_call", [this, expect_order, recv_prelude]() {
            expect_order((std::string(recv_prelude) + R"(
                pk().v += 5; string s1 = log + ":" + (`${shared.v}`);
                pk().v -= 2; string s2 = log + ":" + (`${shared.v}`);
                pk().v *= 3; string s3 = log + ":" + (`${shared.v}`);
                pk().v /= 2; string s4 = log + ":" + (`${shared.v}`);
                pk().v %= 4;
                s1 + "|" + s2 + "|" + s3 + "|" + s4 + "|" + log + ":" + (`${shared.v}`);
            )").c_str(), "A:15|AA:13|AAA:39|AAAA:19|AAAAA:3", "call receiver runs once per compound", false);
            expect_order((std::string(recv_prelude) + R"(
                var r = (pk().v += 5);
                log + ":" + (`${r}`) + ":" + (`${shared.v}`);
            )").c_str(), "A:15:15", "expression-position compound", false);
        });
        test("member_incdec_receiver_once", [this, expect_order, recv_prelude]() {
            expect_order((std::string(recv_prelude) + R"(
                pk().v++; string s1 = log + ":" + (`${shared.v}`);
                ++pk().v; string s2 = log + ":" + (`${shared.v}`);
                pk().v--; string s3 = log + ":" + (`${shared.v}`);
                --pk().v; string s4 = log + ":" + (`${shared.v}`);
                int r = pk().v++;
                int q = ++pk().v;
                int t = pk().v--;
                int u = --pk().v;
                s1 + "|" + s2 + "|" + s3 + "|" + s4 + "|" + log + ":" + (`${r}`) + ":" + (`${q}`) + ":" + (`${t}`) + ":" + (`${u}`) + ":" + (`${shared.v}`);
            )").c_str(), "A:11|AA:12|AAA:11|AAAA:10|AAAAAAAA:10:12:12:10:10", "++/-- pre and post on a call receiver", false);
        });
        test("member_compound_receiver_once_index_and_key", [this, expect_order]() {
            expect_order(R"(
                var log = "";
                class P { int v = 10; }
                int side(string tag) { log += tag; return 1; }
                string key(string tag) { log += tag; return "k"; }
                array<P> arr = [P(), P(), P()];
                var varr = [P(), P()];
                map<string, P> mp = {"k": P()};
                var vm = {"k": P()};
                arr[side("a")].v += 1;
                arr[side("b")].v++;
                ++varr[side("c")].v;
                mp[key("d")].v += 1;
                mp[key("e")].v++;
                vm[key("f")].v -= 3;
                log + "|" + (`${arr[1].v}`) + ":" + (`${varr[1].v}`) + ":" + (`${mp["k"].v}`) + ":" + (`${vm["k"].v}`);
            )", "abcdef|12:11:12:7", "index / key receivers run once", false);
        });
        test("member_compound_receiver_once_nested", [this, expect_order]() {
            expect_order(R"(
                var log = "";
                class C { int c = 1; }
                class B { C cc = C(); }
                class A { B bb = B(); }
                A a0 = A();
                A& ga() { log += "A"; return a0; }
                class Holder { A a = A(); function getb() -> B& { log += "B"; return this.a.bb; } }
                Holder h = Holder();
                ga().bb.cc.c += 1; string s1 = log + ":" + (`${a0.bb.cc.c}`);
                ga().bb.cc.c++; string s2 = log + ":" + (`${a0.bb.cc.c}`);
                h.getb().cc.c += 3; string s3 = log + ":" + (`${h.a.bb.cc.c}`);
                --h.getb().cc.c; string s4 = log + ":" + (`${h.a.bb.cc.c}`);
                s1 + "|" + s2 + "|" + s3 + "|" + s4;
            )", "A:2|AA:3|AAB:4|AABB:3", "every level of a nested chain runs once", false);
        });
        test("member_compound_receiver_once_operator_overload", [this, expect_order]() {
            expect_order(R"(
                var log = "";
                class V { int x = 0; V(int a) { x = a; } V operator+(V o) { log += "+"; return V(x + o.x); } }
                class Hold { V v = V(1); }
                Hold hh = Hold();
                Hold& gh() { log += "H"; return hh; }
                gh().v += V(4); string s1 = log + ":" + (`${hh.v.x}`);
                var r = (gh().v += V(1));
                s1 + "|" + log + ":" + (`${r.x}`) + ":" + (`${hh.v.x}`);
            )", "H+:5|H+H+:6:6", "operator+ compound evaluates the receiver once", false);
        });
        test("member_compound_receiver_once_hybrid", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                bind_hybrid_probe(*e);
                e->execute(R"(
                    var log = "";
                    HybridProbe hp = HybridProbe();
                    HybridProbe& g() { log += "A"; return hp; }
                    g().value += 5;
                    g().lam += 1;
                )");
                const std::string tag = use_vm ? "vm: " : "interp: ";
                auto cpp = probe_of(*e, "hp");
                check_eq(std::string("AA"), e->execute("log").as_string(), tag + "the C++ receiver runs once per compound");
                check_eq((int64_t)5, cpp->value, tag + "member-pointer property compound");
                check_eq((int64_t)1, cpp->lam_backing, tag + "lambda property compound");
                check_eq((int64_t)1, cpp->lam_reads, tag + "one getter call");
                check_eq((int64_t)1, cpp->lam_writes, tag + "one setter call");
            }
        });
        test("compound_assignment_mismatch_names_the_operator_and_both_types", [this]() {
            // docs/site/reference/errors.html "Run-time errors": every compound site raises the same catchable text on both backends (the runtime text is under
            // test, so the checker is off); a float target with a non-numeric operand used to escape the try as a C++ exception
            for (bool proofs : {true, false}) {
                auto e = make_engine();
                jai::stdlib::register_all(*e);
                e->type_proofs(proofs);
                auto caught = [&](const std::string& src) { return e->execute("[[warn_mode]]; var m = \"none\";\n" + src + "\nm;").to_string(); };
                check_eq(std::string("Cannot apply '+=' to string and int"), caught("string s = \"x\"; try { s += 5; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '+=' to string and int"), caught("string z = \"\"; try { z += to_char(1) + to_char(2); } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '-=' to string and string"), caught("string s = \"x\"; try { s -= \"a\"; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '*=' to string and int"), caught("string s = \"x\"; try { s *= 2; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '+=' to int and string"), caught("int i = 1; try { i += \"x\"; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '+=' to float and string"), caught("float f = 1.0; try { f += \"x\"; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '/=' to float and array"), caught("float f = 1.0; try { f /= [1]; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '+=' to bool and int"), caught("bool b = true; try { b += 1; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '+=' to array and array"), caught("var q = [1]; try { q += [2]; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '+=' to string and int"), caught("class C1 { string s = \"\"; } C1 c = C1(); try { c.s += 5; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '+=' to string and int"), caught("class D1 { string s = \"\"; void add() { s += 5; } } D1 d = D1(); try { d.add(); } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '+=' to string and int"), caught("string g = \"x\"; function ff() { g += 5; } try { ff(); } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '++' to string"), caught("string s = \"x\"; try { s++; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '--' to string"), caught("string s = \"x\"; try { --s; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '++' to string"), caught("class C2 { string s = \"\"; } C2 c = C2(); try { c.s++; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '--' to bool"), caught("array<bool> flags = [true]; try { flags[0]--; } catch (err) { m = err; }"));
                check_eq(std::string("Cannot apply '++' to string"), caught("class D2 { string s = \"\"; void bump() { s++; } } D2 d = D2(); try { d.bump(); } catch (err) { m = err; }"));
                std::string what;
                try { e->execute("[[warn_mode]]; string u = \"x\"; u += 5;"); } catch (const std::exception& ex) { what = ex.what(); }
                check_eq(std::string("Cannot apply '+=' to string and int"), what);
            }
        });

        test("host_what_does_not_repeat_its_category", [this]() {
            auto what_of = [](const std::shared_ptr<jai::engine>& e, const char* script) {
                try { e->execute(script); } catch (const std::exception& ex) { return std::string(ex.what()); }
                return std::string();
            };
            auto sandbox = make_engine();
            sandbox->disable(jai::permissions::filesystem);
            check_eq(std::string("Insufficient permission: filesystem."), what_of(sandbox, "extern x; import(\"nowhere.jai\");"));
            auto e = make_engine();
            check_eq(std::string("Undefined variable 'undefined_thing'"), what_of(e, "[[warn_mode]]; undefined_thing + 1;"));
            check_eq(std::string("Cannot apply '+=' to string and int"), what_of(e, "[[warn_mode]]; string u = \"x\"; u += 5;"),
                "the text alone");
            e->execution_budget(0.05);
            const std::string budget = what_of(e, "while (true) { }");
            e->execution_budget(jai::foundry::k_test_budget_seconds);
            check_eq(std::string("Script execution budget exceeded - raise engine::execution_budget or break up the work"), budget);
            check_eq(std::string("Undefined variable 'q': Undefined variable"),
                what_of(e, "throw \"Undefined variable 'q': Undefined variable\";"), "a script throw keeps its text exactly");
        });

        test("anonymous_unit_is_spelled_script_in_every_listing", [this]() {
            auto e = make_engine();
            std::string checked, parsed;
            try { e->execute("int x = undefined_thing + 1;"); } catch (const jai::static_check_error& ex) { checked = ex.what(); }
            try { e->execute("var x = ;"); } catch (const jai::parse_error& ex) { parsed = ex.what(); }
            check_true(checked.find("\n<script>:1:9: error:") != std::string::npos, checked);
            check_true(parsed.find("<script>:1:") != std::string::npos, parsed);
        });

        test("member_compound_order_pinned", [this, expect_order, recv_prelude]() {
            expect_order((std::string(recv_prelude) + R"(
                int boom() { log += "R"; throw "boom"; }
                string caught = "";
                try { pk().v += boom(); } catch (e) { caught = e; }
                log + "|" + caught + ":" + (`${shared.v}`);
            )").c_str(), "R|boom:10", "C++17: the rhs before the receiver; an rhs raise evaluates no target", false);
            expect_order((std::string(recv_prelude) + R"(
                var n = null;
                var& gn() { log += "N"; return n; }
                string caught = "";
                try { gn().v += 1; } catch (e) { caught = e; }
                log + "|" + caught;
            )").c_str(), "N|Cannot access 'v' on null ('gn(...)' returned null)", "null receiver: named call-result text, once", false);
            expect_order((std::string(recv_prelude) + R"(
                int gi() { log += "I"; return 5; }
                string caught = "";
                try { gi().v += 1; } catch (e) { caught = e; }
                log + "|" + caught;
            )").c_str(), "I|int has no member 'v'", "scalar receiver: once", false);
            expect_order((std::string(recv_prelude) + R"(
                class S { string s = "a"; }
                S so = S();
                S& gs() { log += "S"; return so; }
                string caught = "none";
                try { gs().s++; } catch (e) { caught = "raised"; }
                log + "|" + caught + ":" + so.s;
            )").c_str(), "S|raised:a", "++ gate on a string member: the receiver still runs once (the vm used to run it twice)", true);   // the checker rejects ++ on a string at load; the RUNTIME order is under test
            expect_order((std::string(recv_prelude) + R"(
                string caught = "none";
                try { pk().v += "s"; } catch (e) { caught = e; }
                log + "|" + caught + ":" + (`${shared.v}`);
            )").c_str(), "A|Cannot apply '+=' to int and string:10", "operand mismatch after the receiver ran once", true);   // the checker rejects int += string at load; the RUNTIME text is under test
        });
        test("subscript_compound_rvalue_base_once", [this, expect_order]() {
            expect_order(R"(
                var log = "";
                var arr = [1, 2, 3];
                var ga() { log += "A"; return arr; }
                string c1 = ""; string c2 = ""; string c3 = "";
                try { ga()[1] += 1; } catch (e) { c1 = e; }
                try { ga()[1]++; } catch (e) { c2 = e; }
                try { [1, 2][0] += 1; } catch (e) { c3 = e; }
                log + "|" + c1 + "|" + c2 + "|" + c3 + "|" + (`${arr[1]}`);
            )", "AA|Cannot assign to rvalue expression|Cannot assign to rvalue expression|Cannot assign to rvalue expression|2",
                "an rvalue subscript base runs once and raises the vm's text", false);
        });
        test("subscript_compound_string_base_text", [this, expect_order]() {
            // Pinned alignment (interpreter -> vm): the index runs once per compound store; an int result raises at the store, leaving
            // the string alone, and text beside the char joins it and stores the text back
            expect_order(R"(
                var log = "";
                var s = "abc";
                int idx() { log += "I"; return 0; }
                string c1 = ""; string c2 = "";
                try { s[idx()] += 1; } catch (e) { c1 = e; }
                try { s[idx()] += "x"; } catch (e) { c2 = e; }
                `${log}|${c1}|${c2}|${s}`;
            )", "II|Cannot assign int to a string character (it takes a char or a string)||axbc",
                "a string subscript compound runs the index once", false);
        });
        test("map_compound_missing_key_raises", [this, expect_order]() {
            // A compound reads its target first: a missing key raises on every map and inserts nothing; the key expression runs once
            expect_order(R"(
                var log = "";
                string key() { log += "K"; return "k"; }
                var m = {};
                map<string, string> ms = {};
                map<string, int> mi = {};
                string c1 = ""; string c2 = ""; string c3 = "";
                try { m[key()] += "s"; } catch (e) { c1 = e; }
                try { ms[key()] += "s"; } catch (e) { c2 = e; }
                try { mi[key()] += 1; } catch (e) { c3 = e; }
                log + "|" + c1 + "|" + c2 + "|" + c3 + "|" + (`${m.size()}`) + (`${ms.size()}`) + (`${mi.size()}`);
            )", "KKK|Key \"k\" not found in map|Key \"k\" not found in map|Key \"k\" not found in map|000",
                "a missing key raises on every map", false);
            expect_order(R"(
                var log = "";
                string key() { log += "K"; return "k"; }
                map<string, int> m = {"k": 1};
                var ms = {"k": "a"};
                m[key()] += 5; m[key()]++; ms[key()] += "b";
                log + "|" + (`${m["k"]}`) + ms["k"];
            )", "KKK|7ab", "present keys keep working", false);
        });
        test("static_compound_unchanged", [this, expect_order]() {
            expect_order(R"(
                class Cnt { static int n = 0; }
                Cnt::n += 5; Cnt::n++; ++Cnt::n; Cnt::n -= 1;
                (`${Cnt::n}`);
            )", "6", "static compounds carry no receiver", false);
        });

        // ---- hybrid methods: a bare C++ property name reads/writes through the binder
        // accessors exactly like this.name (lookup ladder: locals -> members -> globals) ----
        test("hybrid_bare_cpp_property_read", [this]() {
            auto e = make_engine();
            bind_hybrid_probe(*e);
            e->execute(R"(
                class W : HybridProbe {
                    W() : super() {}
                    int v() { return value; }
                    int v1() { return value + 1; }
                    int v2() { return 1 + value; }
                    bool lt() { return value < 10; }
                    string nm() { return name + "!"; }
                    int sum() { return value + value; }
                    int viaLocal() { int k = 2; return value + k; }
                }
                W w = W(); w.value = 5; w.name = "x";
            )");
            check_eq((int64_t)5, e->execute("w.v()").as_int());
            check_eq((int64_t)6, e->execute("w.v1()").as_int());
            check_eq((int64_t)6, e->execute("w.v2()").as_int());
            check_true(e->execute("w.lt()").as_bool());
            check_eq(std::string("x!"), e->execute("w.nm()").as_string());
            check_eq((int64_t)10, e->execute("w.sum()").as_int());
            check_eq((int64_t)7, e->execute("w.viaLocal()").as_int());
            check_eq((int64_t)6, e->execute("w.value = 6; w.v()").as_int(), "no stale shadow after a C++-side change");
            check_false(e->execute("w").get_class_instance()->has_user_overflow_fields(),
                        "bare reads never materialize a field for a C++ property");
        });
        test("hybrid_bare_cpp_property_store_writes_through", [this]() {
            auto e = make_engine();
            bind_hybrid_probe(*e);
            e->execute(R"(
                class W : HybridProbe {
                    W() : super() {}
                    void set(int x) { value = x; }
                    void rename(string s) { name = s; }
                    int v() { return value; }
                }
                W w = W(); w.set(7); w.rename("r");
            )");
            check_eq((int64_t)7, e->execute("w.value").as_int());
            check_eq((int64_t)14, e->execute("w.doubled()").as_int(), "the C++ member itself moved");
            check_eq((int64_t)7, e->execute("w.v()").as_int());
            check_eq(std::string("r"), e->execute("w.name").as_string());
            check_false(e->execute("w").get_class_instance()->has_user_overflow_fields());
        });
        test("hybrid_bare_cpp_property_compound_and_incdec", [this]() {
            auto e = make_engine();
            bind_hybrid_probe(*e);
            e->execute(R"(
                class W : HybridProbe {
                    W() : super() {}
                    void bump() { value += 2; value++; ++value; value -= 1; }
                    int post() { return value++; }
                    int pre() { return ++value; }
                    int postdec() { return value--; }
                    int predec() { return --value; }
                    void bang() { name += "!"; }
                    void times() { value *= 3; }
                }
                W w = W(); w.value = 5; w.name = "x"; w.bump();
            )");
            check_eq((int64_t)8, e->execute("w.value").as_int());
            check_eq((int64_t)16, e->execute("w.doubled()").as_int());
            check_eq((int64_t)8, e->execute("w.post()").as_int());
            check_eq((int64_t)9, e->execute("w.value").as_int());
            check_eq((int64_t)10, e->execute("w.pre()").as_int());
            check_eq((int64_t)10, e->execute("w.value").as_int());
            check_eq((int64_t)10, e->execute("w.postdec()").as_int());
            check_eq((int64_t)8, e->execute("w.predec()").as_int());
            check_eq((int64_t)8, e->execute("w.value").as_int());
            check_eq((int64_t)24, e->execute("w.times(); w.value").as_int());
            check_eq(std::string("x!"), e->execute("w.bang(); w.name").as_string());
            check_false(e->execute("w").get_class_instance()->has_user_overflow_fields());
        });
        test("hybrid_bare_cpp_property_in_lambdas", [this]() {
            auto e = make_engine();
            bind_hybrid_probe(*e);
            e->execute(R"(
                class W : HybridProbe {
                    int q = 3;
                    W() : super() {}
                    int a() { auto f = [this]() { return value; }; return f(); }
                    int b() { auto f = [this]() { return value + 1; }; return f(); }
                    int bq() { auto f = [this]() { return q + 1; }; return f(); }
                    int b0() { auto f = [this]() { return value; }; return f(); }
                    int b1() { auto f = [this]() { int k = 1; return value + k; }; return f(); }
                    int b2() { auto f = [this]() { return this.value + 1; }; return f(); }
                    int b3() { auto f = [this]() { return 1 + value; }; return f(); }
                    int a1() { auto f = [this]() { return value + 1; }; return f(); }
                    void c() { auto f = [this]() { value = 9; }; f(); }
                    int d() { auto f = [=]() { return value; }; return f(); }
                    int dq() { auto f = [=]() { return q; }; return f(); }
                    int r() { auto f = [&]() { return value; }; return f(); }
                    void cc() { auto f = [this]() { value += 1; value++; }; f(); }
                }
                W w = W(); w.value = 5;
            )");
            auto run_int = [&](const char* src, const char* label) -> int64_t {
                try { return e->execute(src).as_int(); }
                catch (const std::exception& ex) { check_true(false, std::string(label) + ": " + ex.what()); return -1; }
            };
            check_eq((int64_t)5, run_int("w.a()", "[this] lambda read"), "[this] lambda reads the property");
            check_eq((int64_t)4, run_int("w.bq()", "[this] lambda field read"), "[this] lambda reads a script field (baseline)");
            check_eq((int64_t)5, run_int("w.b0()", "[this] lambda plain property read"));
            check_eq((int64_t)6, run_int("w.b1()", "[this] lambda property + local"));
            check_eq((int64_t)6, run_int("w.b2()", "[this] lambda this.property + 1"));
            check_eq((int64_t)6, run_int("w.b3()", "[this] lambda 1 + property"));
            check_eq((int64_t)6, run_int("w.a1()", "[this] lambda property + 1"));
            check_eq((int64_t)6, run_int("w.b()", "[this] lambda property read"), "[this] lambda reads the property");
            check_eq((int64_t)9, run_int("w.c(); w.value", "[this] lambda store"), "[this] lambda store writes through");
            check_eq((int64_t)3, run_int("w.dq()", "[=] field snapshot"), "[=] snapshots a script field (baseline)");
            check_eq((int64_t)9, run_int("w.d()", "[=] property snapshot"), "[=] snapshots the property like a field");
            check_eq((int64_t)9, run_int("w.r()", "[&] property share"), "[&] shares the member itself (by_ref_capture_of_cpp_property_is_live)");
            check_eq((int64_t)11, run_int("w.cc(); w.value", "[this] lambda compound"));
            check_false(e->execute("w").get_class_instance()->has_user_overflow_fields());
        });
        test("hybrid_script_field_shadows_cpp_property", [this]() {
            auto e = make_engine();
            bind_hybrid_probe(*e);
            e->execute(R"(
                class S : HybridProbe {
                    int value = 42;
                    S() : super() {}
                    int v() { return value; }
                    int tv() { return this.value; }
                    void set(int x) { value = x; }
                    void tset(int x) { this.value = x; }
                    void bump() { value += 1; value++; }
                }
                S s = S();
            )");
            check_eq((int64_t)42, e->execute("s.v()").as_int());
            check_eq((int64_t)42, e->execute("s.tv()").as_int());
            check_eq((int64_t)42, e->execute("s.value").as_int());
            check_eq((int64_t)0, e->execute("s.doubled()").as_int(), "the C++ member is untouched");
            e->execute("s.set(43)");
            check_eq((int64_t)43, e->execute("s.v()").as_int());
            check_eq((int64_t)43, e->execute("s.tv()").as_int());
            check_eq((int64_t)0, e->execute("s.doubled()").as_int(), "a bare store lands on the shadowing script field");
            e->execute("s.tset(44)");
            check_eq((int64_t)44, e->execute("s.v()").as_int());
            check_eq((int64_t)0, e->execute("s.doubled()").as_int());
            e->execute("s.bump()");
            check_eq((int64_t)46, e->execute("s.v()").as_int());
            check_eq((int64_t)0, e->execute("s.doubled()").as_int());
        });
        test("hybrid_bare_cpp_property_error_texts_match_this_form", [this]() {
            std::string bare_msg[2], this_msg[2], undef_msg[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                bind_hybrid_probe(*e);
                e->execute(R"([[warn_mode]]; 
                    class W : HybridProbe {
                        W() : super() {}
                        void bad() { value = "str"; }
                        void bad_this() { this.value = "str"; }
                        int ro_read() { return ro; }
                        void ro_store() { ro = 5; }
                        void ro_store_this() { this.ro = 5; }
                        int undef() { return zzz_nope + 1; }
                    }
                    W w = W(); w.value = 5;
                )");
                bool threw = false;
                try { e->execute("[[warn_mode]]; w.bad();"); } catch (const std::exception& ex) { threw = true; bare_msg[idx] = ex.what(); }
                check_true(threw, "a mistyped bare store raises");
                threw = false;
                try { e->execute("[[warn_mode]]; w.bad_this();"); } catch (const std::exception& ex) { threw = true; this_msg[idx] = ex.what(); }
                check_true(threw, "a mistyped this. store raises");
                check_eq((int64_t)5, e->execute("[[warn_mode]]; w.value").as_int(), "a rejected store leaves the member alone");
                check_eq((int64_t)105, e->execute("[[warn_mode]]; w.ro_read()").as_int(), "bare read of a getter-only property");
                std::string ro_bare, ro_this;
                try { e->execute("[[warn_mode]]; w.ro_store();"); } catch (const std::exception& ex) { ro_bare = ex.what(); }
                try { e->execute("[[warn_mode]]; w.ro_store_this();"); } catch (const std::exception& ex) { ro_this = ex.what(); }
                check_eq(std::string("Cannot assign to 'ro': it is read-only"), ro_bare, "a bare store to a getter-only property raises");
                check_eq(ro_bare, ro_this, "the this. spelling raises the same text");
                check_eq((int64_t)105, e->execute("[[warn_mode]]; w.ro_read()").as_int(), "the getter still reads after the rejected stores");
                check_eq((int64_t)5, e->execute("[[warn_mode]]; w.value").as_int());
                threw = false;
                try { e->execute("[[warn_mode]]; w.undef();"); } catch (const std::exception& ex) { threw = true; undef_msg[idx] = ex.what(); }
                check_true(threw);
                check_true(undef_msg[idx].find("Undefined variable 'zzz_nope'") != std::string::npos, undef_msg[idx]);
                ++idx;
            }
            check_eq(bare_msg[0], bare_msg[1], "bare-store error text is byte-identical across backends");
            check_eq(this_msg[0], this_msg[1], "this-store error text is byte-identical across backends");
            check_eq(undef_msg[0], undef_msg[1], "undefined-name text is byte-identical across backends");
        });
        // ---- by-reference binding of C++ properties (bind the MEMBER ITSELF). Every row runs
        // both backends. ----
        auto propref_engine = [](bool use_vm) {
            auto e = jai::foundry::make_default_engine();
            e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
            bind_hybrid_probe(*e);
            return e;
        };
        auto caught = [](engine& e, const char* src) -> std::string {
            try { e.execute(src); } catch (const std::exception& ex) { return ex.what(); }
            return "";
        };
        test("cpp_property_by_ref_param_writes_member", [this, propref_engine]() {
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    void by_ref(int& p) { p = 41; p += 1; }
                    void by_ref_f(float& f) { f = 2.0; f *= 2; }
                    void by_ref_b(bool& b) { b = true; }
                    void by_ref_s(string& s) { s += "!"; }
                    class W : HybridProbe {
                        W() : super() {}
                        void arg_ref() { by_ref(value); }
                        void arg_ref_this() { by_ref(this.value); }
                        void others() { by_ref_f(ratio); by_ref_b(flag); by_ref_s(name); }
                        void others_this() { by_ref_f(this.ratio); by_ref_b(this.flag); by_ref_s(this.name); }
                    }
                    W w = W();
                )");
                auto cpp = probe_of(*e, "w");
                e->execute("w.arg_ref();");
                check_eq((int64_t)42, cpp->value, "bare by-ref param wrote the C++ member");
                check_eq((int64_t)84, e->execute("w.doubled()").as_int());
                cpp->value = 0;
                e->execute("w.arg_ref_this();");
                check_eq((int64_t)42, cpp->value, "this. by-ref param wrote the C++ member");
                e->execute("w.others();");
                check_near(4.0, cpp->ratio, 1e-9);
                check_true(cpp->flag, "bool member through bool&");
                check_eq(std::string("n!"), cpp->name, "std::string member through string&");
                cpp->ratio = 1.0; cpp->flag = false; cpp->name = "m";
                e->execute("w.others_this();");
                check_near(4.0, cpp->ratio, 1e-9);
                check_true(cpp->flag);
                check_eq(std::string("m!"), cpp->name);
                check_false(e->execute("w").get_class_instance()->has_user_overflow_fields(),
                            "no field materialized for a C++ property");
            }
        });
        test("cpp_property_ref_decl_compound", [this, propref_engine]() {
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    class W : HybridProbe {
                        W() : super() {}
                        int decl_ref() { auto& r = value; r += 1; ++r; r = r + 1; return r; }
                        int decl_ref_this() { auto& r = this.value; r += 1; ++r; r = r + 1; return r; }
                        int typed_ref() { int& r = value; r -= 2; return r; }
                        float fr() { auto& r = ratio; r += 0.5; r = r * 2; return r; }
                        bool br() { auto& r = flag; r = !r; return r; }
                        string sr() { auto& r = name; r += "x"; r = r + "y"; return r; }
                        int postfix() { auto& r = value; int old = r++; r--; return old; }
                    }
                    W w = W(); w.value = 5;
                )");
                auto cpp = probe_of(*e, "w");
                check_eq((int64_t)8, e->execute("w.decl_ref()").as_int(), "plain =, += and ++ all land");
                check_eq((int64_t)8, cpp->value, "the C++ member advanced by 3");
                check_eq((int64_t)11, e->execute("w.decl_ref_this()").as_int());
                check_eq((int64_t)11, cpp->value);
                check_eq((int64_t)9, e->execute("w.typed_ref()").as_int(), "int& declaration");
                check_eq((int64_t)9, cpp->value);
                check_near(4.0, e->execute("w.fr()").as_float(), 1e-9);
                check_near(4.0, cpp->ratio, 1e-9);
                check_true(e->execute("w.br()").as_bool());
                check_true(cpp->flag);
                check_eq(std::string("nxy"), e->execute("w.sr()").as_string());
                check_eq(std::string("nxy"), cpp->name);
                check_eq((int64_t)9, e->execute("w.postfix()").as_int(), "postfix yields the old value");
                check_eq((int64_t)9, cpp->value);
            }
        });
        test("cpp_property_ref_reads_are_live", [this, propref_engine]() {
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    class W : HybridProbe {
                        W() : super() {}
                        int live() { auto& r = value; bump_cpp(); return r; }
                        int live_copy() { auto& r = value; int snap = r; bump_cpp(); return snap; }
                    }
                    W w = W(); w.value = 1;
                )");
                check_eq((int64_t)101, e->execute("w.live()").as_int(), "a C++-side change is visible through the reference");
                check_eq((int64_t)101, e->execute("w.live_copy()").as_int(), "a by-value copy detaches");
                check_eq((int64_t)201, probe_of(*e, "w")->value);
            }
        });
        test("cpp_property_ref_outlives_call", [this, propref_engine, caught]() {
            std::string retired_msg[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    class W : HybridProbe {
                        W() : super() {}
                        function grab() -> int& { return value; }
                        function grab_this() -> int& { return this.value; }
                    }
                    var w = W(); w.value = 1;
                    auto& r = w.grab(); r = 9;
                    auto& r2 = w.grab_this(); r2 += 1;
                )");
                auto cpp = probe_of(*e, "w");
                check_eq((int64_t)10, cpp->value, "a returned reference writes the member");
                // the reference keeps the object alive after the script drops its handle
                std::weak_ptr<hybrid_prop_probe> weak = cpp;
                cpp.reset();
                e->execute("w = null;");
                check_false(weak.expired(), "the CPP-MEMBER pin keeps the object alive");
                e->execute("r = 77;");
                check_eq((int64_t)77, weak.lock()->value, "and the write still lands after the handle is gone");
                // retired layer: a fresh bind reports, byte-identical on both backends
                auto plain = std::make_shared<hybrid_prop_probe>();
                plain->value = 3;
                e->add_global("kept", e->extend(plain, "W"));
                e->execute("auto& live = kept.value; live = 4;");
                check_eq((int64_t)4, plain->value, "bound through an extended object's layer");
                (void)e->script_layer_detach(plain);
                retired_msg[idx] = caught(*e, "auto& again = kept.value;");
                check_true(retired_msg[idx].find("was detached") != std::string::npos, retired_msg[idx]);
                ++idx;
            }
            check_eq(retired_msg[0], retired_msg[1], "retired-layer text is byte-identical across backends");
        });
        test("lambda_property_ref_routes_through_accessors", [this, propref_engine]() {
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    void by_ref(int& p) { p = 5; p += 1; }
                    void by_ref_x(int& p) { p = 21; }
                    class W : HybridProbe {
                        W() : super() {}
                        void arg() { by_ref(lam); }
                        int decl() { auto& r = lam; r = 5; r += 1; return r; }
                        void child_decl() { auto& c = child; c.x = 7; }
                        void child_chain() { by_ref_x(child.x); }
                        void child_chain_this() { by_ref_x(this.child.x); }
                    }
                    W w = W();
                )");
                auto cpp = probe_of(*e, "w");
                e->execute("w.arg();");
                check_eq((int64_t)6, cpp->lam_backing, "a lambda-pair property writes through its setter");
                check_eq((int64_t)2, cpp->lam_writes, "one setter call per store");
                cpp->lam_writes = 0; cpp->lam_reads = 0;
                check_eq((int64_t)6, e->execute("w.decl()").as_int());
                check_eq((int64_t)2, cpp->lam_writes);
                check_gt(cpp->lam_reads, (int64_t)0, "reads go through the getter");
                e->execute("w.child_decl();");
                check_eq((int64_t)7, cpp->child.x, "a class-typed member property derefs to the live member");
                e->execute("w.child_chain();");
                check_eq((int64_t)21, cpp->child.x, "a chain through a class-typed property binds the inner member");
                cpp->child.x = 0;
                e->execute("w.child_chain_this();");
                check_eq((int64_t)21, cpp->child.x);
                check_eq((int64_t)21, e->execute("by_ref_x(w.child.x); w.child.x").as_int(), "from outside too");
            }
        });
        test("read_only_property_store_raises_every_spelling", [this, propref_engine]() {
            // Assigning to a getter-only property is an error on
            // every spelling, produced once by the binder's read-only setter - C++ semantics.
            const std::string ro_text = "Cannot assign to 'ro': it is read-only";
            std::vector<std::string> texts[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                stdlib::register_all(*e);
                e->execute(R"([[warn_mode]]; 
                    void by_ref(int& p) { p = 1; }
                    void by_ref_any(var& p) { p = 1; }
                    string caught(function f) { try { f(); } catch (e) { return e; } return "no error"; }
                    class W : HybridProbe {
                        W() : super() {}
                        string s_bare() { return caught([this]() { ro = 5; }); }
                        string s_this() { return caught([this]() { this.ro = 5; }); }
                        string c_bare() { return caught([this]() { ro += 1; }); }
                        string c_this() { return caught([this]() { this.ro += 1; }); }
                        string i_bare_post() { return caught([this]() { ro++; }); }
                        string i_bare_pre() { return caught([this]() { --ro; }); }
                        string i_this() { return caught([this]() { this.ro++; }); }
                        string r_decl() { return caught([this]() { auto& r = ro; r = 5; }); }
                        string r_decl_c() { return caught([this]() { auto& r = ro; r += 1; }); }
                        string r_arg() { return caught([this]() { by_ref(ro); }); }
                        string r_arg_this() { return caught([this]() { by_ref(this.ro); }); }
                        string sig_bare() { return caught([this]() { sig = 5; }); }
                        string sig_this() { return caught([this]() { this.sig = 5; }); }
                        string sig_ref() { return caught([this]() { by_ref_any(sig); }); }
                        int ro_read() { return ro; }
                    }
                    W w = W(); w.value = 5;
                    HybridProbe h = HybridProbe(); h.value = 7;
                )");
                auto& out = texts[idx];
                const char* rows[] = {
                    "w.s_bare()", "w.s_this()", "w.c_bare()", "w.c_this()",
                    "w.i_bare_post()", "w.i_bare_pre()", "w.i_this()",
                    "w.r_decl()", "w.r_decl_c()", "w.r_arg()", "w.r_arg_this()",
                    "caught(function() { w.ro = 5; })", "caught(function() { w.ro += 1; })", "caught(function() { w.ro++; })",
                    "caught(function() { by_ref(w.ro); })", "caught(function() { auto& r = w.ro; r = 5; })",
                    "caught(function() { h.ro = 5; })", "caught(function() { h.ro++; })", "caught(function() { by_ref(h.ro); })",
                    "caught(function() { reflect::set(w, \"ro\", 1); })", "caught(function() { reflect::set(h, \"ro\", 1); })",
                    "w.sig_bare()", "w.sig_this()", "w.sig_ref()", "caught(function() { w.sig = 5; })", "caught(function() { h.sig = 5; })",
                };
                const std::string tag = use_vm ? "vm: " : "interp: ";
                for (const char* row : rows) {
                    std::string got = e->execute(std::string("[[warn_mode]]; ") + row).as_string();
                    out.push_back(got);
                    const std::string want = std::string(row).find("sig") != std::string::npos
                        ? "Cannot assign to 'sig': it is read-only" : ro_text;
                    check_eq(want, got, tag + row);
                }
                check_eq((int64_t)105, e->execute("[[warn_mode]]; w.ro_read()").as_int(), tag + "the getter still reads");
                check_eq((int64_t)105, e->execute("[[warn_mode]]; w.ro").as_int());
                check_eq((int64_t)107, e->execute("[[warn_mode]]; h.ro").as_int());
                check_eq((int64_t)5, probe_of(*e, "w")->value, tag + "the C++ member is untouched");
                check_eq((int64_t)7, probe_of(*e, "h")->value);
                // a property WITH a setter still stores on every spelling
                e->execute(R"([[warn_mode]]; 
                    class V : HybridProbe {
                        V() : super() {}
                        void go() { value = 8; lam = 3; auto& q = value; q += 1; this.value += 1; value++; }
                    }
                    V v = V(); v.go(); v.value += 10; v.lam++;
                )");
                check_eq((int64_t)21, probe_of(*e, "v")->value, tag + "writable properties store");
                check_eq((int64_t)4, probe_of(*e, "v")->lam_backing);
                // a script _set_ro override in the chain wins over the rejection
                e->execute(R"([[warn_mode]]; 
                    class O : HybridProbe {
                        int stored = 0;
                        O() : super() {}
                        void _set_ro(int v) override { stored = v; }   // the base's _set_ro is the C++ rejection: a shadow needs override
                        void go() { ro = 1; this.ro = 2; ro += 1; }
                    }
                    O o = O(); o.go();
                )");
                check_eq((int64_t)101, e->execute("[[warn_mode]]; o.stored").as_int(), tag + "a script _set_ro override takes the stores (compound reads the C++ getter)");
                check_eq((int64_t)40, e->execute("[[warn_mode]]; o.ro = 40; o.stored").as_int());
                check_eq((int64_t)102, e->execute("[[warn_mode]]; o.ro += 2; o.stored").as_int());
                // a same-named script field shadows the property
                e->execute(R"([[warn_mode]]; 
                    class S : HybridProbe {
                        int ro = 1;
                        S() : super() {}
                        void go() { ro = 2; this.ro += 1; ro++; }
                    }
                    S s = S(); s.go(); s.ro += 10;
                )");
                check_eq((int64_t)14, e->execute("[[warn_mode]]; s.ro").as_int(), tag + "a shadowing script field takes the stores");
                ++idx;
            }
            check_eq(texts[0].size(), texts[1].size());
            for (size_t i = 0; i < texts[0].size(); ++i) {
                check_eq(texts[0][i], texts[1][i], "read-only texts are byte-identical across backends");
            }
        });
        test("read_only_property_store_raises_under_warn_mode", [this, propref_engine]() {
            // [[warn_mode]] downgrades the checker's diagnostic; the runtime still raises
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    [[warn_mode]]
                    class W : HybridProbe {
                        W() : super() {}
                        void go() { ro = 5; }
                    }
                    W w = W();
                )");
                std::string got;
                try { e->execute("[[warn_mode]] w.ro = 5;"); } catch (const std::exception& ex) { got = ex.what(); }
                check_eq(std::string("Cannot assign to 'ro': it is read-only"), got);
                got.clear();
                try { e->execute("w.go();"); } catch (const std::exception& ex) { got = ex.what(); }
                check_eq(std::string("Cannot assign to 'ro': it is read-only"), got);
            }
        });
        test("script_field_shadows_property_for_refs", [this, propref_engine]() {
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    void by_ref(int& p) { p += 1; }
                    class S : HybridProbe {
                        int value = 7;
                        S() : super() {}
                        void go() { by_ref(value); by_ref(this.value); auto& r = value; r += 1; auto& t = this.value; t += 1; }
                    }
                    S s = S(); s.go();
                )");
                check_eq((int64_t)11, e->execute("s.value").as_int(), "the script field took every write");
                check_eq((int64_t)0, probe_of(*e, "s")->value, "the C++ member is untouched");
                check_eq((int64_t)12, e->execute("by_ref(s.value); s.value").as_int(), "shadowing holds from outside");
                check_eq((int64_t)0, probe_of(*e, "s")->value);
            }
        });
        test("plain_cpp_receiver_binds", [this, propref_engine]() {
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                hybrid_prop_probe raw;
                raw.value = 10;
                e->add_global_ref("g", raw);
                e->execute(R"(
                    void by_ref(int& p) { p += 1; }
                    void by_ref_s(string& s) { s += "!"; }
                    HybridProbe p = HybridProbe(); p.value = 41;
                    by_ref(p.value);
                    auto& r = p.value; r += 1;
                    by_ref(g.value);
                    auto& gr = g.value; gr += 1;
                    var& s = p.self(); by_ref(s.value);
                    by_ref_s(g.name);
                )");
                check_eq((int64_t)44, probe_of(*e, "p")->value, "a script-constructed C++ object binds the member (T& return handle included)");
                check_eq((int64_t)12, raw.value, "a raw add_global_ref handle binds the member");
                check_eq(std::string("n!"), raw.name);
            }
        });
        test("hybrid_receiver_from_outside", [this, propref_engine]() {
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    void by_ref(int& p) { p += 1; }
                    class W : HybridProbe { W() : super() {} }
                    class Holder { W w = W(); }
                    W w = W(); w.value = 1;
                    by_ref(w.value);
                    auto& r = w.value; r += 1;
                    Holder h = Holder();
                    by_ref(h.w.value);
                    auto& hr = h.w.value; hr += 1;
                )");
                check_eq((int64_t)3, probe_of(*e, "w")->value);
                check_eq((int64_t)2, probe_of(*e, "h.w")->value, "a chain through a script field reaches the member");
            }
        });
        test("by_value_still_copies", [this, propref_engine]() {
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    int by_val(int p) { p = 99; return p + 1; }
                    class W : HybridProbe {
                        W() : super() {}
                        int v() { return by_val(value); }
                        int vt() { return by_val(this.value); }
                        int copy() { int x = this.value; x = 5; auto y = value; y = 6; return x + y; }
                    }
                    W w = W(); w.value = 42;
                )");
                check_eq((int64_t)100, e->execute("w.v()").as_int());
                check_eq((int64_t)100, e->execute("w.vt()").as_int());
                check_eq((int64_t)11, e->execute("w.copy()").as_int());
                check_eq((int64_t)42, probe_of(*e, "w")->value, "by-value passes and copies leave the member alone");
            }
        });
        test("mistyped_store_through_property_ref_texts_match", [this, propref_engine, caught]() {
            std::string ref_msg[2], direct_msg[2], narrow_ref[2], narrow_direct[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"([[warn_mode]]; 
                    void by_ref_str(string& s) { s = "z"; }
                    class W : HybridProbe {
                        W() : super() {}
                        void bad() { auto& r = value; r = "str"; }
                        void bad_direct() { this.value = "str"; }
                        void narrow_ref() { auto& r = narrow; r = 5000000000; }
                        void narrow_direct() { this.narrow = 5000000000; }
                    }
                    W w = W(); w.value = 5;
                )");
                ref_msg[idx] = caught(*e, "[[warn_mode]]; w.bad();");
                direct_msg[idx] = caught(*e, "[[warn_mode]]; w.bad_direct();");
                check_true(!ref_msg[idx].empty(), "a mistyped store through the reference raises");
                check_eq((int64_t)5, probe_of(*e, "w")->value, "and leaves the member alone");
                narrow_ref[idx] = caught(*e, "[[warn_mode]]; w.narrow_ref();");
                check_eq((int32_t)(5000000000LL), probe_of(*e, "w")->narrow);
                narrow_direct[idx] = caught(*e, "[[warn_mode]]; w.narrow_direct();");
                ++idx;
            }
            check_eq(ref_msg[0], ref_msg[1], "mistyped-store text is byte-identical across backends");
            check_eq(direct_msg[0], direct_msg[1]);
            // a native boundary may append ': C++ exception' to the shared text (the direct store's
            // setter error leaves as that runtime error, the reference store's as a throw of its text)
            const std::string boundary_suffix = ": C++ exception";
            std::string direct_text = direct_msg[0];
            if (direct_text.size() > boundary_suffix.size() && direct_text.compare(direct_text.size() - boundary_suffix.size(), boundary_suffix.size(), boundary_suffix) == 0) {
                direct_text.resize(direct_text.size() - boundary_suffix.size());
            }
            check_eq(direct_text, ref_msg[0], "the reference raises the direct store's own conversion text");
            // Narrow widths: the CPP-MEMBER box stores like an add_global_ref int32 (a
            // silent truncation) where the direct setter range-checks ("Integer value out
            // of range for int") - pinned as observed; an open question.
            check_eq(narrow_ref[0], narrow_ref[1], "narrow-int text is byte-identical across backends");
            check_eq(narrow_direct[0], narrow_direct[1]);
            check_true(narrow_ref[0].empty(), "reference store into int32 truncates (bound-variable rule)");
            check_true(narrow_direct[0].find("out of range") != std::string::npos, narrow_direct[0]);
        });
        test("method_name_still_not_bindable", [this, propref_engine, caught]() {
            std::string decl_msg[2], arg_msg[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"([[warn_mode]]; 
                    void by_ref(int& p) { p = 1; }
                    class W : HybridProbe {
                        W() : super() {}
                        void decl() { auto& r = doubled; }
                        void arg() { by_ref(doubled); }
                    }
                    W w = W();
                )");
                decl_msg[idx] = caught(*e, "[[warn_mode]]; w.decl();");
                arg_msg[idx] = caught(*e, "[[warn_mode]]; w.arg();");
                check_true(decl_msg[idx].find("'doubled' is a method of W; call it with doubled() or bind it with reflect::bind(this, \"doubled\")") != std::string::npos, decl_msg[idx]);   // a reference to a method is the method-value violation
                check_true(arg_msg[idx].find("'doubled' is a method of W; call it with doubled() or bind it with reflect::bind(this, \"doubled\")") != std::string::npos, arg_msg[idx]);   // a method handed to a reference is a method read without a call
                ++idx;
            }
            check_eq(decl_msg[0], decl_msg[1]);
            check_eq(arg_msg[0], arg_msg[1]);
        });
        test("null_receiver_ref_texts_unchanged", [this, propref_engine, caught]() {
            std::string arg_msg[2], decl_msg[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"([[warn_mode]]; 
                    void by_ref(int& p) { p = 1; }
                    var n = null;
                )");
                arg_msg[idx] = caught(*e, "[[warn_mode]]; by_ref(n.value);");
                decl_msg[idx] = caught(*e, "[[warn_mode]]; auto& r = n.value;");
                check_true(!arg_msg[idx].empty() && !decl_msg[idx].empty(), "null receivers still raise");
                ++idx;
            }
            check_eq(arg_msg[0], arg_msg[1], "null-receiver ref-arg text is byte-identical across backends");
            check_eq(decl_msg[0], decl_msg[1], "null-receiver ref-decl text is byte-identical across backends");
        });
        test("by_ref_capture_of_cpp_property_is_live", [this, propref_engine]() {
            // [&] follows the field rule (cell-share the member); [=] keeps the getter snapshot
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    class W : HybridProbe {
                        W() : super() {}
                        int live() { auto f = [&]() { return value; }; bump_cpp(); return f(); }
                        void write() { auto f = [&]() { value = 9; value += 1; }; f(); }
                        int snap() { auto f = [=]() { return value; }; bump_cpp(); return f(); }
                    }
                    W w = W(); w.value = 1;
                )");
                auto cpp = probe_of(*e, "w");
                check_eq((int64_t)101, e->execute("w.live()").as_int(), "[&] sees the C++-side change");
                e->execute("w.write();");
                check_eq((int64_t)10, cpp->value, "[&] writes land in the member");
                check_eq((int64_t)110, e->execute("w.snap()").as_int(), "[=] reads the live property");
                check_eq((int64_t)110, cpp->value);
            }
        });
        test("script_accessor_override_binds_through_accessors", [this, propref_engine]() {
            // A script _set_/_get_ override in the chain wins: the reference routes
            // through it instead of aiming at the member
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    void by_ref(int& p) { p = 4; }
                    class W : HybridProbe {
                        int sets = 0;
                        W() : super() {}
                        void _set_value(int v) override { sets += 1; this.name = "set"; }
                        void go() { by_ref(value); auto& r = value; r = 5; }
                    }
                    W w = W(); w.go();
                )");
                auto cpp = probe_of(*e, "w");
                check_eq((int64_t)2, e->execute("w.sets").as_int(), "both stores went through the script setter");
                check_eq(std::string("set"), cpp->name);
                check_eq((int64_t)0, cpp->value, "the member was never written directly");
            }
        });
        test("property_ref_follows_reload_shadowing_field", [this, propref_engine]() {
            // A live PROPERTY ref re-resolves per touch: a reload that adds a same-named
            // script field re-points it at that field (the shadow rule, late); a live
            // CPP-MEMBER ref keeps aiming at the C++ member (its address is stable)
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"(
                    class W : HybridProbe { W() : super() {} }
                    W w = W(); w.value = 1; w.lam = 2;
                    auto& lr = w.lam;
                    auto& vr = w.value;
                    lr = 3; vr = 4;
                )");
                auto cpp = probe_of(*e, "w");
                check_eq((int64_t)3, cpp->lam_backing);
                check_eq((int64_t)4, cpp->value);
                e->execute("class W : HybridProbe { int lam = 50; int value = 60; W() : super() {} }");
                check_eq((int64_t)50, e->execute("lr").as_int(), "the PROPERTY ref now reads the shadowing script field");
                e->execute("lr = 51;");
                check_eq((int64_t)51, e->execute("w.lam").as_int(), "and writes it");
                check_eq((int64_t)3, cpp->lam_backing, "the C++ side is untouched");
                e->execute("vr = 5;");
                check_eq((int64_t)5, cpp->value, "the CPP-MEMBER ref still writes the member");
                check_eq((int64_t)60, e->execute("w.value").as_int(), "while the spelling now reads the script field");
            }
        });
        test("property_refs_inside_lambdas_and_cast", [this, propref_engine, caught]() {
            // [this] lambdas made in a hybrid method reach the property through the env's
            // 'this' for every ref spelling; cast<int&> binds it too. Explicit [&name] /
            // [name] captures of a property keep today's undefined-capture text (they have
            // no member rung - unchanged, byte-identical).
            std::string ex_ref[2], ex_val[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = propref_engine(use_vm);
                e->execute(R"([[warn_mode]]; 
                    void by_ref(int& p) { p += 1; }
                    class W : HybridProbe {
                        W() : super() {}
                        void lam_arg() { auto f = [this]() { by_ref(value); }; f(); }
                        void lam_decl() { auto f = [this]() { auto& r = value; r += 1; }; f(); }
                        void cast_ref() { auto& r = cast<int&>(value); r = 40; }
                        int ex_ref() { auto f = [&value]() { return value; }; return f(); }
                        int ex_val() { auto f = [value]() { return value; }; return f(); }
                    }
                    W w = W(); w.value = 1;
                )");
                auto cpp = probe_of(*e, "w");
                e->execute("[[warn_mode]]; w.lam_arg();");
                check_eq((int64_t)2, cpp->value, "ref arg inside a bare lambda binds the member");
                e->execute("[[warn_mode]]; w.lam_decl();");
                check_eq((int64_t)3, cpp->value, "ref decl inside a bare lambda binds the member");
                e->execute("[[warn_mode]]; w.cast_ref();");
                check_eq((int64_t)40, cpp->value, "cast<int&> binds the member");
                ex_ref[idx] = caught(*e, "[[warn_mode]]; w.ex_ref()");
                ex_val[idx] = caught(*e, "[[warn_mode]]; w.ex_val()");
                check_true(ex_ref[idx].find("Cannot capture undefined variable 'value'") != std::string::npos, ex_ref[idx]);
                check_true(ex_val[idx].find("Cannot capture undefined variable 'value'") != std::string::npos, ex_val[idx]);
                ++idx;
            }
            check_eq(ex_ref[0], ex_ref[1]);
            check_eq(ex_val[0], ex_val[1]);
        });
        test("hybrid_two_level_chain_bare_cpp_property", [this]() {
            auto e = make_engine();
            bind_hybrid_probe(*e);
            e->execute(R"(
                class Kind : HybridProbe { Kind() : super() {} int kv() { return value; } }
                class W2 : Kind {
                    W2() : super() {}
                    int v() { return value; }
                    void set(int x) { value = x; }
                    void bump() { value += 10; }
                }
                W2 w = W2(); w.set(3);
            )");
            check_eq((int64_t)3, e->execute("w.v()").as_int());
            check_eq((int64_t)3, e->execute("w.kv()").as_int());
            check_eq((int64_t)3, e->execute("w.value").as_int());
            check_eq((int64_t)6, e->execute("w.doubled()").as_int());
            check_eq((int64_t)13, e->execute("w.bump(); w.value").as_int());
            check_false(e->execute("w").get_class_instance()->has_user_overflow_fields());
        });
        test("arity_text_numeric_through_computed_callees", [this]() {
            const char* calls[] = {
                "f(1)", "s(1)", "fs[0](1)", "pick()(1)",
                "g(1)", "sg(1)", "gs[0](1)", "pickg()(1)",
                "g(1,2,3)", "sg(1,2,3)", "gs[0](1,2,3)", "pickg()(1,2,3)"};
            const char* expected[] = {
                "'f' expects 0 arguments, got 1",
                "'g' expects 2 arguments, got 1",
                "'g' expects 2 arguments, got 3"};
            std::string texts[2][12];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"([[warn_mode]]; 
                    void f() {}
                    void g(int a, int b) {}
                    var pick() { return f; }
                    var pickg() { return g; }
                    var s = f; var fs = [f];
                    var sg = g; var gs = [g];
                    string r = "ok";
                )");
                for (int i = 0; i < 12; ++i) {
                    texts[idx][i] = e->execute(std::string("[[warn_mode]]; r = \"ok\"; try { ") + calls[i] + "; } catch (err) { r = err; } r").as_string();
                    check_eq(std::string(expected[i / 4]), texts[idx][i], calls[i]);
                    check_eq(std::string::npos, texts[idx][i].find("this"), calls[i]);
                    check_eq(std::string::npos, texts[idx][i].find("class_definition"), calls[i]);
                }
                ++idx;
            }
            for (int i = 0; i < 12; ++i) { check_eq(texts[0][i], texts[1][i], calls[i]); }
        });
        test("arity_text_defaults_lambdas_coroutines", [this]() {
            struct row { const char* call; const char* text; };
            const row rows[] = {
                {"h()", "'h' expects 2 arguments, got 0"},
                {"sh()", "'h' expects 2 arguments, got 0"},
                {"h(1,2,3)", "'h' expects 2 arguments, got 3"},
                {"sh(1,2,3)", "'h' expects 2 arguments, got 3"},
                {"h(1)", "ok"},
                {"sh(1)", "ok"},
                {"lam()", "lambda expects 1 argument, got 0"},
                {"lam(1,2)", "lambda expects 1 argument, got 2"},
                {"auto c = co(); c.next()", "'co' expects 1 argument, got 0"}};
            std::string texts[2][9];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"([[warn_mode]]; 
                    void h(int a, int b = 2) {}
                    var sh = h;
                    var lam = [](int a) { return a; };
                    coroutine int co(int a) { yield a; return 0; }
                    string r = "ok";
                )");
                for (int i = 0; i < 9; ++i) {
                    texts[idx][i] = e->execute(std::string("[[warn_mode]]; r = \"ok\"; try { ") + rows[i].call + "; } catch (err) { r = err; } r").as_string();
                    check_eq(std::string(rows[i].text), texts[idx][i], rows[i].call);
                }
                ++idx;
            }
            for (int i = 0; i < 9; ++i) { check_eq(texts[0][i], texts[1][i], rows[i].call); }
        });
        test("arity_text_uncaught_prefix", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                bool threw = false;
                std::string msg;
                try { e->execute("[[warn_mode]]; void f() {} f(1);"); } catch (const std::exception& ex) { threw = true; msg = ex.what(); }
                check_true(threw);
                check_eq((size_t)0, msg.find("'f' expects 0 arguments, got 1"), msg);
            }
        });
        // ---- string-builtin identifier receiver: the receiver resolves like every other
        // identifier (locals -> members -> globals, catch var, ::global) and the in-place
        // builtin mutates THAT variable; a same-named global is never the fallback ----
        test("string_builtin_identifier_receiver_ladder", [this]() {
            auto run_both = [](const std::string& src) {
                std::string out[2];
                int idx = 0;
                for (bool use_vm : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    try { out[idx] = e->execute(src).to_string(); }
                    catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                    ++idx;
                }
                return std::make_pair(out[0], out[1]);
            };
            auto expect = [&](const std::string& src, const std::string& expected, const char* what) {
                auto [i_out, v_out] = run_both(src);
                check_eq(expected, i_out, std::string(what) + " (interpreter)");
                check_eq(i_out, v_out, std::string(what) + " parity");
            };
            const std::string glob = "string s = \"glob\";\n";
            expect(glob + R"(
                string f() { string s = "loc"; s.insert(0, "x"); s.erase(1, 1); return `${s}:${s.length()}:${s.find("o")}`; }
                `${f()}|${s}`
            )", "xoc:3:1|glob", "local shadow");
            expect(glob + R"(
                string g(string s) { s.to_upper(); return `${s}:${s.length()}:${s.find("A")}`; }
                `${g("par")}|${s}`
            )", "PAR:3:1|glob", "parameter shadow");
            expect(glob + R"(
                class C {
                    string s = "fld";
                    string m() { s.insert(0, "x"); return `${s}:${s.length()}`; }
                    string t() { this.s.reverse(); return `${this.s}:${this.s.length()}`; }
                }
                auto c = C();
                `${c.m()}|${c.t()}|${c.s}|${s}`
            )", "xfld:4|dlfx:4|dlfx|glob", "field shadow, bare and this.s");
            expect(R"(
                class D {
                    string s = "fld";
                    string m() { string s = "loc"; s.insert(0, "x"); return `${s}/${this.s}/${s.length()}`; }
                    string p(string s) { s.to_upper(); return `${s}/${this.s}`; }
                }
                auto d = D();
                `${d.m()}|${d.p("par")}|${d.s}`
            )", "xloc/fld/4|PAR/fld|fld", "method local and parameter shadowing the field");
            expect(glob + R"(
                string c() { string s = "cap"; auto l = [&s]() { s.insert(0, "y"); return `${s}:${s.length()}`; }; return `${l()}/${s}`; }
                `${c()}|${s}`
            )", "ycap:4/ycap|glob", "by-reference captured local");
            {
                auto [i_out, v_out] = run_both(glob + R"(
                    string c() { string s = "cap"; auto l = [s]() { s.insert(0, "y"); return `${s}:${s.length()}`; }; return l(); }
                    `${c()}|${s}`
                )");
                check_eq(std::string("ycap:4|glob"), i_out, "by-value captured local (interpreter)");
                check_eq(i_out, v_out, "by-value captured local parity");
            }
            expect(glob + R"(
                string q() { string out = ""; for (int i = 0; i < 2; i++) { string s = "it"; s.to_upper(); out = out + s + `${s.length()}`; } return out; }
                `${q()}|${s}`
            )", "IT2IT2|glob", "loop-scoped local");
            expect(glob + R"(
                string b() { string out = ""; { string s = "blk"; s.erase(0, 1); out = out + s + `${s.find("k")}`; } return out; }
                `${b()}|${s}`
            )", "lk1|glob", "block-scoped local in a function");
            expect(glob + R"(
                string log = "";
                { string s = "top"; s.insert(0, "x"); log = log + s; }
                for (int i = 0; i < 1; i++) { string s = "lp"; s.to_upper(); log = log + s; }
                `${log}|${s}`
            )", "xtopLP|glob", "top-level block and loop scopes");
            expect(glob + R"(
                class S { static string s = "sta"; static string m() { s.insert(0, "x"); return `${s}:${s.length()}`; } }
                `${S::m()}|${s}`
            )", "xsta:4|glob", "static field in a static method");
            expect(R"(
                string e = "glob";
                string k() { try { throw "err"; } catch (e) { e.to_upper(); return `${e}:${e.length()}`; } return "no"; }
                `${k()}|${e}`
            )", "ERR:3|glob", "catch variable over a same-named global");
            expect(glob + R"(
                string z() { string s = "loc"; ::s.insert(0, "g"); return `${s}:` + cast<string>(::s.length()); }
                `${z()}|${s}`
            )", "loc:5|gglob", "::s still the global");
            expect(R"(
                class T { int n = 0; void insert(int v) { n = n + v; } int find(int v) { return n; } int size() { return n * 10; } }
                string w() { T t = T(); t.insert(3); return `${t.find(0)}/${t.size()}`; }
                w()
            )", "3/30", "builtin-colliding class methods keep dispatching");
        });
        // ---- the receiver is resolved before the arguments (C++17 [expr.call]); after the
        // arguments the builtin runs on the variable's CURRENT contents when it still holds a
        // string, and on the pre-args value when an argument retyped a var receiver (the
        // variable stays retyped) - pinned parity, the interpreter used to crash here ----
        test("string_builtin_receiver_argument_order_parity", [this]() {
            auto run_both = [](const std::string& src) {
                std::string out[2];
                int idx = 0;
                for (bool use_vm : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    try { out[idx] = e->execute("[[warn_mode]]; " + src).to_string(); }
                    catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                    ++idx;
                }
                return std::make_pair(out[0], out[1]);
            };
            auto expect = [&](const std::string& src, const std::string& expected, const char* what) {
                auto [i_out, v_out] = run_both(src);
                check_eq(expected, i_out, std::string(what) + " (interpreter)");
                check_eq(i_out, v_out, std::string(what) + " parity");
            };
            expect(R"(
                var s = "glob";
                string f() { s = 5; return "x"; }
                s.insert(0, f());
                `${s}`
            )", "5", "argument retypes the receiver");
            expect(R"(
                var t = "abc";
                string g() { t = "zzz"; return "y"; }
                t.insert(0, g());
                t
            )", "yzzz", "argument reassigns the receiver");
            expect(R"(
                string u = "ab";
                u.insert(0, "c");
                `${u}/${u.size()}`
            )", "cab/3", "pure arguments");
            expect(R"(
                string p() { return "q"; }
                string h() { string u = "ab"; u.insert(1, p()); return `${u}/${u.length()}`; }
                h()
            )", "aqb/3", "local receiver with a calling argument");
        });
        // ---- the catch variable is an ordinary block-scoped local of the catch body
        // (C++ catch (e) semantics): a frame slot in functions, a scope env at top
        // level; locals -> members -> globals like any local, shadows for the block only,
        // invisible to callees and to code after the block, captured like any local.
        // Both backends used to resolve it dynamically through a backend-global intercept
        // (a callee's own 'int e = 5' read the caller's exception). ----
        test("catch_variable_block_scope", [this]() {
            auto run_both = [](const std::string& src) {
                std::string out[2];
                int idx = 0;
                for (bool use_vm : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    jai::stdlib::register_all(e);
                    try { out[idx] = e->execute("[[warn_mode]]; " + src).to_string(); }
                    catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                    ++idx;
                }
                return std::make_pair(out[0], out[1]);
            };
            auto expect = [&](const std::string& src, const std::string& expected, const char* what) {
                auto [i_out, v_out] = run_both(src);
                check_eq(expected, i_out, std::string(what) + " (interpreter)");
                check_eq(expected, v_out, std::string(what) + " (vm)");
            };
            expect(R"(
                int callee() { int e = 5; return e; }
                var r = 0;
                try { throw "x"; } catch (e) { r = callee(); }
                r
            )", "5", "a callee's own local e");
            expect(R"(
                var e = "global";
                string callee() { return e; }
                string f() { var r = ""; try { throw "x"; } catch (e) { r = callee() + "/" + e; } return r + "|" + e; }
                f()
            )", "global/x|global", "a callee reads the global e; e after the block is the global again");
            // Top level: a plain function's env parents on the CALLER's chain, so every
            // top-level block local is visible to a callee today ({ var e = "y"; callee(); }
            // reads "y" on both backends - pre-existing, outside this item); the catch
            // variable follows the block-local rule exactly, and is gone after the block
            expect(R"(
                var e = "global";
                string callee() { return e; }
                var r = "";
                try { throw "y"; } catch (e) { r = callee(); }
                var r2 = "";
                { var e = "y"; r2 = callee(); }
                to_string(r == r2) + "|" + e
            )", "true|global", "top-level catch variable = top-level block local for a callee");
            expect(R"(
                var f = null;
                try { throw "x"; } catch (e) { f = [=]() { return e; }; }
                f()
            )", "x", "lambda returned from a top-level catch captures e by value");
            expect(R"(
                string h() { var f = null; try { throw "x"; } catch (e) { f = [=]() { return e; }; } return f(); }
                h()
            )", "x", "lambda returned from a function catch captures e by value");
            expect(R"(
                string h() { string seen = ""; try { throw "x"; } catch (e) { auto f = [&]() { e = e + "!"; }; f(); seen = e; } return seen; }
                h()
            )", "x!", "[&] lambda mutates the catch variable through the cell");
            expect(R"(
                string seen = "";
                try { throw "x"; } catch (e) { auto f = [&]() { e = e + "!"; }; f(); seen = e; }
                seen
            )", "x!", "[&] lambda at top level");
            expect(R"(
                var e = 1; var a = 0; var b = 0;
                try { throw 2; } catch (e) { a = e; }
                b = e;
                to_string(a) + "," + to_string(b)
            )", "2,1", "shadowing at top level");
            expect(R"(
                string fn() { var e = 1; var a = 0; var b = 0; try { throw 2; } catch (e) { a = e; } b = e; return to_string(a) + "," + to_string(b); }
                fn()
            )", "2,1", "shadowing in a function");
            expect(R"(
                class C { int e = 7; string m() { int a = 0; try { throw 2; } catch (e) { a = e.value; } return to_string(a) + "," + to_string(e) + "," + to_string(this.e); } }
                C().m()
            )", "2,7,7", "shadowing a field: bare e is the field again after the block");
            expect(R"(
                int fn() { try { throw "x"; } catch (e) { } return e; }
                var r = "";
                try { fn(); } catch (err) { r = err; }
                r
            )", "Undefined variable 'e'", "e after the block in a function body");
            expect(R"(
                try { throw "x"; } catch (e) { }
                var r = "";
                try { var q = e; } catch (err) { r = err; }
                r
            )", "Undefined variable 'e'", "e after the block at top level");
            expect(R"(
                class K { string tag = "T"; string m() { try { throw "x"; } catch (e) { } return e; } }
                var r = "";
                try { K().m(); } catch (err) { r = err; }
                r
            )", "Undefined variable 'e'", "e after the block inside a method");
            expect(R"(
                var r = "";
                try { try { throw "a"; } catch { throw; } } catch (e2) { r = e2; }
                r
            )", "a", "catch without a variable + bare throw;");
            expect(R"(
                string u() { try { try { throw "k"; } catch (e) { try { throw; } catch { return "inner:" + e; } } } catch (o) { return "outer:" + o; } return "none"; }
                u()
            )", "inner:k", "a nested try inside the catch keeps the caught exception for throw; (the in-catch marker)");
            expect(R"(
                class V { int x = 1; }
                string h() { try { try { var v = V(); v.x = 9; throw v; } catch (e) { e = 5; throw; } } catch (o) { return to_string(o.x) + "," + to_string(o.value.x); } return ""; }
                h()
            )", "9,9", "the thrown object reads through the caught error (o.x, o.value.x); throw; rethrows the original after e is reassigned");
            expect(R"(
                string f() { string s = ""; try { throw "1"; } catch (e) { s = s + e; } try { throw "2"; } catch (e) { s = s + e; } return s; }
                string t = "";
                try { throw "3"; } catch (e) { t = t + e; }
                try { throw "4"; } catch (e) { t = t + e; }
                f() + t
            )", "1234", "two sequential catches reusing the name");
            expect(R"(
                string f() { string s = ""; try { throw [1, 2, 3]; } catch (e) { for (auto v : e.value) { s = s + to_string(v); } } return s; }
                f()
            )", "123", "a thrown array as a range-for source (e.value)");
            expect(R"(
                string f() { string s = ""; try { throw "x"; } catch (e) { e += "!"; s = e; } return s; }
                int g() { int r = 0; try { throw 5; } catch (e) { e = e.value; e++; r = e; } return r; }
                int h() { int out = 0; try { throw 1; } catch (e) { var& r = e; r = 5; out = e; } return out; }
                f() + to_string(g()) + to_string(h())
            )", "x!65", "catch variable as an lvalue: compound, ++, reference alias");
            expect(R"(
                string s = ""; int n = 0; int m = 0;
                try { throw "x"; } catch (e) { e += "!"; s = e; }
                try { throw 5; } catch (e) { e = e.value; e++; n = e; }
                try { throw 1; } catch (e) { var& r = e; r = 5; m = e; }
                s + to_string(n) + to_string(m)
            )", "x!65", "catch variable as an lvalue at top level");
            expect(R"(
                string log = "";
                for (int i = 0; i < 3; i++) {
                    try { throw "t" + to_string(i); } catch (e) { if (i == 1) { continue; } if (i == 2) { break; } log = log + e; }
                    log = log + "|";
                }
                try { var q = e; } catch (err) { log = log + err; }
                log
            )", "t0|Undefined variable 'e'", "continue/break out of a top-level catch body inside a loop");
            expect(R"(
                int f() { for (int i = 0; i < 3; i++) { try { throw i; } catch (e) { if (e.value == 1) { return e.value * 10; } continue; } } return -1; }
                string g() { int i = 0; while (true) { i++; try { throw i; } catch (e) { if (e.value == 2) { break; } } } return to_string(i); }
                to_string(f()) + g()
            )", "102", "return/continue/break out of a function catch");
            expect(R"(
                string log = "";
                for (int i = 0; i < 2; i++) { try { try { throw "a"; } catch (e) { throw "b" + e; } } catch (e2) { log = log + e2; } }
                try { var q = e; } catch (err) { log = log + "/" + err; }
                log
            )", "baba/Undefined variable 'e'", "a throw out of a top-level catch drops its scope");
            expect(R"(
                int f() { int r = 0; try { try { throw 1; } catch (e) { throw e.value + 1; } } catch (e) { r = e.value; } return r; }
                f()
            )", "2", "a throw out of a function catch rebinds the outer catch variable");
            expect(R"(
                var a = [1, 2, 3];
                parallel_for (auto& x : a) { try { throw x + 10; } catch (e) { x = e; } }
                to_string(a[0]) + to_string(a[1]) + to_string(a[2])
            )", "111213", "catch variable inside a parallel_for body (worker-frame slot)");
        });
    }
};

// Reference-capture semantics (C++ lambda semantics ARE the contract: [&] works or
// no ref capture does). Regression: these were silently broken
// for SLOT-resident function locals (pre-cells "capture by value, by-ref is UAF"
// compromises that the cell model obsoleted): [&]/[&x] captured copies, and
// `auto& r = <local>` errored "undefined variable" (escape marker boxes the SLOT, the
// binder searched only the env).
class ref_capture_semantics_tests : public suite {
public:
    ref_capture_semantics_tests() : suite("Ref Capture Semantics") {}

    void forge_tests() override {
        test("captures_and_ref_decls_of_slot_locals_alias_or_copy_as_spelled", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"default_by_ref_capture_writes_through", "function f() -> int { int x = 1; auto g = [&]() { x = 42; }; g(); return x; } f();", int64_t{42}},
                {"explicit_by_ref_capture_writes_through", "function f() -> int { int x = 1; auto g = [&x]() { x = 42; }; g(); return x; } f();", int64_t{42}},
                {"mixed_capture_modes", R"(
                function f() -> int {
                    int a = 1;
                    int b = 10;
                    auto g = [=, &a]() { a = 7; b = 70; };
                    g();
                    return a * 100 + b;   // a written through, b untouched
                }
                f();
            )", int64_t{710}},
                {"ref_capture_reads_live_value", R"(
                function f() -> int {
                    int x = 5;
                    auto g = [&]() -> int { return x; };
                    x = 9;
                    return g();   // by-ref sees the CURRENT value, not creation-time
                }
                f();
            )", int64_t{9}},
                {"escaping_ref_capture_outlives_frame", R"(
                function make() -> auto {
                    int n = 0;
                    return [&]() -> int { n = n + 1; return n; };
                }
                auto counter = make();
                counter();
                counter();
                counter();
            )", int64_t{3}},
                {"two_closures_share_one_ref_capture", R"(
                function f() -> int {
                    int shared = 0;
                    auto inc = [&]() { shared = shared + 1; };
                    auto dec = [&]() { shared = shared - 10; };
                    inc(); inc(); dec();
                    return shared;
                }
                f();
            )", int64_t{-8}},
                {"ref_decl_of_plain_local_binds", R"(
                function f() -> int {
                    int x = 1;
                    auto& r = x;
                    r = 42;
                    int seen = x;
                    x = 7;
                    return seen * 100 + r;   // alias wrote 42 into x; x's write shows through r
                }
                f();
            )", int64_t{4207}},
                {"typed_ref_decl_of_local_converts_through", R"(
                function f() -> int {
                    int x = 5;
                    auto& r = x;
                    r = true;        // store through the alias into an int-declared cell
                    return x;
                }
                f();
            )", int64_t{1}},
                {"by_value_capture_still_copies", "function f() -> int { int x = 1; auto g = [=]() { x = 42; }; g(); return x; } f();", int64_t{1}},
                {"by_value_capture_still_copies", "function f2() -> int { int x = 1; auto g = [x]() { x = 42; }; g(); return x; } f2();", int64_t{1}},
            };
            check_values(cases);
        });

    }
};

// Element-read mint elision: subscript
// reads consumed as transient values push a shallow copy instead of a reference_holder.
// These pin the semantics the elision must preserve on BOTH backends.
class element_read_elision_tests : public suite {
public:
    element_read_elision_tests() : suite("Element Read Elision") {}

    void forge_tests() override {
        test("elided_and_fused_element_reads_keep_their_results", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"subscript_operands_value_ops", R"(
                array<int> a = [10, 20, 30, 40];
                auto s = a[0] + a[1] * a[2] - a[3];
                auto c = a[1] < a[2];
                auto b = (a[0] & a[1]) | (a[2] >> 1);
                to_string(s) + "|" + to_string(c) + "|" + to_string(b);
            )", std::string("570|true|15")},
                {"scalar_return_of_element", R"(
                array<int> a = [10, 20, 30];
                var f = [true, "s"];
                var nested = [[1, 2]];
                int at(int i) { return a[i]; }
                int floor_at(int i) { return f[i]; }
                int& slot(int i) { return a[i]; }
                var row(int i) { return nested[i]; }
                auto pick = [](int i) -> float { return a[i]; };
                int before = at(2);
                a[2] = 7;
                slot(0) = 99;
                var copy = row(0);
                copy[0] = 5;
                string err = "";
                try { floor_at(1); } catch (e) { err = "raised"; }
                to_string(before) + "|" + to_string(at(2)) + "|" + to_string(a[0]) + "|" + to_string(floor_at(0)) + "|" +
                    to_string(pick(1)) + "|" + to_string(nested[0][0]) + "|" + err;
            )", std::string("30|7|99|1|20.0|1|raised")},
                {"map_transient_read_never_inserts", R"(
                var m = {"a": 3};
                auto hit = m["a"] + 1;
                string missing = "";
                try { auto b = m["nope"] == null; } catch (e) { missing = e; }
                to_string(m.size()) + "|" + to_string(hit) + "|" + missing;
            )", std::string("1|4|Key \"nope\" not found in map")},
                {"reference_holder_pool_churn", R"(
                var data = [0, 0, 0, 0];
                auto bump(var& arr, int i) -> auto { var& cell = arr[i]; cell = cell + 1; return cell; }
                auto total = 0;
                auto n = 0;
                while (n < 400) { total = total + bump(data, n % 4); n = n + 1; }
                to_string(total) + "|" + to_string(data[0]) + "|" + to_string(data[3]);
            )", std::string("20200|100|100")},
                {"fused_store_semantics", R"(
                array<int> ints = [1, 2, 3];
                ints[0] = true;                // typed element: converts like assignment
                ints[1] += 2;
                ints[2] *= 3;
                var strs = ["a", "b"];
                strs[0] += "x";                // string element: slow-replay concat
                var m = {"k": 10};
                m["k"] += 5;                   // map target: slow-replay through the entry ref
                m["new"] = 1;                  // auto-insert
                var grid = [[1, 2], [3, 4]];
                grid[1][0] = grid[0][1] + 40;  // nested chain target + elided RHS
                to_string(ints[0]) + "|" + to_string(ints[1]) + "|" + to_string(ints[2]) + "|" +
                    strs[0] + "|" + to_string(m["k"]) + "|" + to_string(m.size()) + "|" + to_string(grid[1][0]);
            )", std::string("1|4|9|ax|15|2|42")},
                {"fused_subscript_operand_semantics", R"(
                array<int> a = [10, 20, 30, 40];
                var f = [0.5, 2.5];
                int i = 1;
                int j = 3;
                auto s1 = a[i] + a[j];         // ident-index both sides
                auto s2 = a[0] * a[i];         // literal + ident
                auto s3 = a[i] < a[j];         // fused cmp bail path
                auto s4 = f[0] + a[i];         // float element mixed
                auto total = 0;
                int k = 0;
                while (a[k] < 35) { total += a[k]; k += 1; }   // condition + compound RHS
                to_string(s1) + "|" + to_string(s2) + "|" + to_string(s3) + "|" +
                    to_string(s4) + "|" + to_string(total);
            )", std::string("60|200|true|20.5|60")},
                {"fused_subscript_chain_n_level", R"(
                var grid = [[1, 2], [3, 4]];
                var cube = [[[10, 20], [30, 40]], [[50, 60], [70, 80]]];
                var deep = [[[[5, 6], [7, 8]]]];
                var m = {"rows": [[100, 200], [300, 400]]};
                int y = 1;
                int x = 0;
                int z = 1;
                auto g = grid[y][x] + grid[0][1];          // 2-level both sides
                auto c = cube[z][y][x] * 2;                // 3-level
                auto d = deep[0][0][y][x] + 1;             // 4-level
                auto mm = m["rows"][y][x] + grid[y][x];    // map level mid-chain + array levels
                auto cmp = grid[y][x] < cube[z][y][x];     // chains in a fused comparison
                to_string(g) + "|" + to_string(c) + "|" + to_string(d) + "|" +
                    to_string(mm) + "|" + to_string(cmp);
            )", std::string("5|140|8|303|true")},
                {"interp_subscript_compound_inplace", R"(
                array<int> pix = [0, 0, 0];
                var fl = [1.5, 2.5];
                int gi = 1;
                pix[gi] += 41;                  // ident index, int
                pix[0] += pix[gi] + 1;          // subscript RHS (callout-free)
                fl[1] *= 2.0;                   // float in place
                var arr2 = [10];
                auto bump() -> auto { arr2.push(99); return 5; }
                arr2[0] += bump();              // impure RHS: general path (ref re-resolve)
                to_string(pix[0]) + "|" + to_string(pix[1]) + "|" + to_string(fl[1]) + "|" +
                    to_string(arr2[0]) + "|" + to_string(arr2.size());
            )", std::string("42|41|5.0|15|2")},
            };
            check_values(cases, true);
        });

        test("elided_element_reads_keep_operand_condition_and_store_values", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"subscript_chain_2d_operand", R"(
                var grid = [[1, 2], [3, 4]];
                grid[1][0] * 10 + grid[0][1];
            )", int64_t{32}},
                {"subscript_in_conditions", R"(
                array<int> a = [0, 5, 7];
                auto n = 0;
                if (a[1]) { n = n + 1; }
                while (a[2] > n) { n = n + 1; }
                auto t = a[0] ? 100 : (a[1] && a[2] ? 50 : 0);
                n + t;
            )", int64_t{57}},
                {"subscript_assignment_rhs", R"(
                array<int> src = [7, 8, 9];
                array<int> dst = [0, 0, 0];
                dst[0] = src[2];
                auto x = src[1];
                x += src[0];
                dst[0] * 100 + x;
            )", int64_t{915}},
                {"impure_sibling_keeps_mint", R"(
                var a = [1, 2, 3];
                auto grow(var& arr) -> auto { auto i = 0; while (i < 64) { arr.push(0); i = i + 1; } return 5; }
                a[0] + grow(a);
            )", int64_t{6}},
                {"compound_element_store_with_subscript_rhs", R"(
                array<int> a = [1, 2, 3];
                a[0] += a[2];
                a[1] = a[1] + a[0];
                a[0] * 10 + a[1];
            )", int64_t{46}},
                {"ref_decl_and_write_through_still_bind", R"(
                var arr = [1, 2, 3];
                var& second = arr[1];
                second = 20;
                arr[1] + arr[0];
            )", int64_t{21}},
                {"class_operator_method_operands_elided_correctly", R"(
                class Vec {
                    int v = 0;
                    Vec(int n) { v = n; }
                    function operator+(Vec o) -> Vec { return Vec(v + o.v); }
                }
                var arr = [Vec(3), Vec(4)];
                var plain = [10, 20];
                auto combined = arr[0] + arr[1];        // object elements -> class operator+
                auto untouched = plain[0] + plain[1];   // int elements -> builtin, still elided
                combined.v * 100 + untouched;
            )", int64_t{730}},
            };
            check_values(cases);
        });

        // A registered binary-operator override flips the runtime gate: subscript
        // operands must reach the override (via the mint path) and produce its result.
        test("operator_override_disables_elision", [this]() {
            auto e = make_engine();
            e->add_function("<", [](script_int a, script_int b) { return a > b; });   // deliberately inverted
            auto r = e->execute(R"(
                array<int> a = [1, 9];
                a[1] < a[0];
            )");
            check(r.as_bool());   // inverted override: 9 "<" 1 is true
        });

        // The tag word: a value names its declared type, or carries the engine-made flag, or nothing (a
        // literal); a heap payload records its engine; an import restamps with the receiving engine's types
        test("tag_word_stamps_and_payload_engines", [this]() {
            auto a = make_engine();
            auto b = make_engine();
            const script_value stamped = a->make_value(script_int{7});
            check_true(stamped.get_type_info().get() == detail::engine_access::get_type_info_int(*a), "an engine-made int carries the engine's int type");
            check_eq((uintptr_t)0, reinterpret_cast<uintptr_t>(stamped.get_type_info().get()) % k_value_tag_alignment, "a stamp is tag-aligned");
            check_true(stamped.raw_storage_index() == script_value::TYPEID_INT && stamped.payload_engine() == nullptr, "a scalar names no engine");
            const script_value null_with_engine(std::monostate{}, a.get());
            check_true(null_with_engine.get_type_info().get() == nullptr, "an engine-made null carries no type");
            check_true(null_with_engine.raw_storage_index() == script_value::TYPEID_NULL && null_with_engine.is_null());
            const script_value bare(std::monostate{}, nullptr);
            check_true(bare.get_type_info().get() == nullptr, "a literal carries nothing");
            check_false(stamped.is_outlived() || null_with_engine.is_outlived() || bare.is_outlived());
            check_true(script_value::outlived().is_outlived() && script_value::outlived().get_type_info().get() == nullptr);
            const held_value array = a->execute("[1, 2];");
            const held_value map = a->execute("var m = {\"k\": 1}; m;");
            const held_value fn = a->execute("[]() { return 1; };");
            check_true(array.value().payload_engine() == a.get() && map.value().payload_engine() == a.get() && fn.value().payload_engine() == a.get(),
                "a container and a function record their engine");
            check_true(array.get_engine() == a.get(), "a held value keeps its engine");
            check_true(detail::engine_access::owns_value(*a, stamped) && !detail::engine_access::owns_value(*b, stamped) && !detail::engine_access::owns_value(*b, array.value()), "ownership: the stamp's or the payload's engine");
            b->add_global("n", a->execute("int k = 3; k;"));
            const held_value imported = b->get_variable("n");
            check_true(imported.get_type_info().get() == detail::engine_access::get_type_info_int(*b), "an import restamps with the receiving engine's type");
            check_eq((int64_t)3, imported.as_int());
        });

        // Values die with their engine: held values give up their payloads at teardown (a held instance's destructor runs while the
        // engine still runs script), then read as outlived; nothing pins the engine, nothing reads freed memory
        test("held_values_die_with_their_engine", [this]() {
            const auto error_of = [](const auto& read) -> std::string {
                try { read(); } catch (const std::exception& err) { return err.what(); }
                return "no error";
            };
            std::optional<held_value> number, text, array, map, instance;
            std::weak_ptr<engine> watch;
            int destroyed = 0;
            {
                auto e = make_engine();
                watch = e;
                e->add_function("note_destroyed", [&destroyed]() { ++destroyed; });
                e->execute("class Tracked { int v = 5; ~Tracked() { note_destroyed(); } }");
                number.emplace(e->execute("41 + 1;"));
                text.emplace(e->execute("\"text\";"));
                array.emplace(e->execute("[1, [2, 3], \"x\"];"));
                map.emplace(e->execute("var m = {\"k\": 7}; m;"));
                instance.emplace(e->execute("Tracked();"));
                check_eq((int64_t)42, number->as_int());
                check_true(instance->valid() && instance->is_object());
                check_eq(0, destroyed, "a held instance lives while its engine does");
            }
            check_true(watch.expired(), "a held value never keeps its engine");
            check_eq(1, destroyed, "the held instance's destructor ran at teardown");
            for (held_value* held : {&*number, &*text, &*array, &*map, &*instance}) {
                check_false(held->valid(), "outlived");
                check_true(held->get_engine() == nullptr, "an outlived value names no engine");
                check_eq(std::string("Value outlived its engine."), error_of([&]() { (void)held->as_int(); }));
                check_eq(std::string("Value outlived its engine."), error_of([&]() { (void)held->to_string(); }));
                check_eq(std::string("Value outlived its engine."), error_of([&]() { (void)held->as<std::string>(); }));
            }
            number.reset(); text.reset(); array.reset(); map.reset(); instance.reset();
        });

        test("held_value_copies_and_moves_across_teardown", [this]() {
            const auto error_of = [](const auto& read) -> std::string {
                try { read(); } catch (const std::exception& err) { return err.what(); }
                return "no error";
            };
            std::optional<held_value> copied, moved, assigned, reassigned;
            {
                auto e = make_engine();
                held_value original = e->execute("[1, 2, 3];");
                copied.emplace(original);
                moved.emplace(std::move(original));
                held_value other = e->execute("\"other\";");
                assigned.emplace(e->execute("0;"));
                *assigned = other;
                check_eq(std::string("other"), assigned->as<std::string>());
            }
            for (held_value* held : {&*copied, &*moved, &*assigned}) {
                check_false(held->valid());
                check_eq(std::string("Value outlived its engine."), error_of([&]() { (void)held->as_array(); }));
            }
            held_value copy_of_outlived = *copied;
            check_false(copy_of_outlived.valid(), "a copy of an outlived value is outlived");
            held_value moved_outlived = std::move(copy_of_outlived);
            check_false(moved_outlived.valid());
            auto live = make_engine();
            reassigned.emplace(live->execute("7;"));
            *reassigned = moved_outlived;
            check_false(reassigned->valid(), "assigning an outlived value outlives the target");
            *reassigned = live->execute("8;");
            check_true(reassigned->valid(), "a live value assigned over an outlived one");
            check_eq((int64_t)8, reassigned->as_int());
            check_eq(std::string("Value outlived its engine."), error_of([&]() { live->add_global("g", moved_outlived); }));
            check_eq(std::string("Value outlived its engine."), error_of([&]() { live->add_global("g", static_cast<const script_value&>(moved_outlived)); }));
            live.reset();
            check_false(reassigned->valid(), "a reassigned value dies with its new engine");
        });

        // A value belongs to its engine's thread: Debug builds record the thread running the outermost execution, and an instance
        // dying anywhere else while it runs (a host thread dropping a last handle) asserts. Idle, the engine may move threads; a
        // region's threads release what they made
        test("the_debug_release_canary_names_another_thread_only_while_the_engine_runs", [this]() {
#ifndef NDEBUG
            auto e = make_engine();
            const auto on_other_thread = [&]() {
                bool result = false;
                std::thread other([&]() { result = detail::engine_access::debug_releases_here(*e); });
                other.join();
                return result;
            };
            bool engine_thread = false;
            bool other_thread = true;
            e->add_function("probe", [&]() {
                engine_thread = detail::engine_access::debug_releases_here(*e);
                other_thread = on_other_thread();
            });
            std::atomic<int> refused_on_region_threads{0};
            e->add_function("probe_region", [&](int64_t) { if (!detail::engine_access::debug_releases_here(*e)) { ++refused_on_region_threads; } });
            check_true(on_other_thread(), "an idle engine may be used, and its values dropped, on any thread");
            e->execute("probe();");
            check_true(engine_thread, "the executing thread releases");
            check_false(other_thread, "another thread does not while the engine runs");
            check_true(on_other_thread(), "idle again once the execute returns");
            e->parallel_thread_count(4);
            e->execute("array<int> xs; for (int i = 0; i < 400; ++i) { xs.push(i); } parallel_for (int x : xs) { probe_region(x); }");
            check_eq(0, refused_on_region_threads.load(), "a region's threads release what they made");
#endif
        });

        // A host thread may hold values while the engine lives; destroying the engine on another thread afterwards (not racing the
        // holder, as with any C++ object) outlives them
        test("held_values_from_a_host_thread", [this]() {
            auto e = make_engine();
            e->execute("var shared = [1, 2, 3];");
            std::vector<held_value> kept;
            std::thread holder([&]() {
                for (int i = 0; i < 8; ++i) { kept.push_back(e->get_variable("shared")); }
            });
            holder.join();
            check_eq((size_t)8, kept.size());
            check_true(kept[3].valid());
            std::thread destroyer([&]() { e.reset(); });
            destroyer.join();
            for (const held_value& held : kept) { check_false(held.valid()); }
        });

        // Values cross engines by copy: numbers, strings and containers convert into the receiving engine; an instance, a function or
        // a reference into the other engine is refused
        test("cross_engine_values_import_by_copy", [this]() {
            const auto error_of = [](const auto& run) -> std::string {
                try { run(); } catch (const std::exception& err) { return err.what(); }
                return "no error";
            };
            auto a = make_engine();
            a->execute("class Thing { int v = 1; } var arr = [1, [2, 3], \"s\"]; array<int> ints = [4, 5]; array<string> words = [\"p\", \"q\"]; var m = {\"k\": [6], \"n\": 2}; var thing = Thing(); var fn = []() { return 1; };");
            auto b = make_engine();
            b->add_global("n", a->execute("12;"));
            b->add_global("t", a->execute("\"text\";"));
            b->add_global("arr", a->get_variable("arr"));
            b->add_global("ints", a->get_variable("ints"));
            b->add_global("words", a->get_variable("words"));
            b->add_global("m", a->get_variable("m"));
            check_true(b->get_variable("arr").get_engine() == b.get(), "an imported value belongs to the receiving engine");
            check_eq((int64_t)12, b->execute("n;").as_int());
            check_eq(std::string("text!"), b->execute("t + \"!\";").as<std::string>());
            check_eq((int64_t)15, b->execute("arr[0] = 9; arr[1][0] = 4; arr[0] + arr[1][0] + arr[1][1] - 1;").as_int());
            check_eq((int64_t)19, b->execute("ints.push(10); ints[0] + ints[1] + ints[2];").as_int());
            check_eq(std::string("pq!"), b->execute("words[1] += \"!\"; words[0] + words[1];").as<std::string>());
            check_eq((int64_t)8, b->execute("m[\"k\"].push(1); m[\"k\"][0] + m[\"k\"][1] + m[\"n\"] - 1;").as_int());
            b.reset();
            check_eq((int64_t)6, a->execute("arr[0] + arr[1][0] + arr[1][1];").as_int());
            check_eq((int64_t)2, a->execute("ints.size();").as_int());
            check_eq(std::string("pq"), a->execute("words[0] + words[1];").as<std::string>());
            check_eq((int64_t)1, a->execute("m[\"k\"].size();").as_int());

            auto c = make_engine();
            const std::string object_text = "Cannot pass an object from another engine: values cross engines by copy (numbers, strings, math values and arrays and maps of them).";
            check_eq(object_text, error_of([&]() { c->add_global("thing", a->get_variable("thing")); }));
            check_eq(object_text, error_of([&]() { c->add_global("things", a->execute("[Thing()];")); }));
            check_eq(std::string("Cannot pass a function from another engine: values cross engines by copy (numbers, strings, math values and arrays and maps of them)."),
                     error_of([&]() { c->add_global("fn", a->get_variable("fn")); }));
            c->execute("var seen = 0; function take(var x) { seen = x[0]; }");
            auto take = convert_script_function<void(script_value)>(c->get_variable("take"));
            take(a->get_variable("ints"));
            check_eq((int64_t)4, c->execute("seen;").as_int());
            check_eq(object_text, error_of([&]() { take(a->execute("[Thing()];")); }));
        });

        // A host's script callback and a suspended run go inert when their engine dies, as held values do
        test("callbacks_go_inert_with_their_engine", [this]() {
            std::function<int()> callback;
            {
                auto e = make_engine();
                callback = convert_script_function<int()>(e->execute("var f = []() { return 3; }; f;"));
                check_eq(3, callback());
            }
            std::string text;
            try { callback(); } catch (const std::exception& err) { text = err.what(); }
            check_eq(std::string("Cannot call a script function after its engine was destroyed"), text);
        });

        // ---- fused subscript stores + flat operator table ----

        test("fused_store_error_paths", [this]() {
            auto e = make_engine();
            // Typed element mismatch keeps its error
            check_throws([&]() { e->execute(R"(array<int> a = [1]; a[0] = "nope";)"); });
            // Compound divide by zero keeps its error
            check_throws([&]() { e->execute(R"(var a = [4]; a[0] /= 0;)"); });
            // Rvalue base (function return) stays a store error
            check_throws([&]() { e->execute(R"(auto f() -> auto { return [1, 2]; } f()[0] = 5;)"); });
        });

        // A registered global operator override must reach subscript compound stores
        // through the flat table (no environment probe on the path anymore).
        test("operator_table_dispatches_compound_override", [this]() {
            auto e = make_engine();
            e->add_function("+", [](script_int a, script_int b) { return a * b; });   // deliberately multiplies
            auto r = e->execute(R"(
                var a = [3];
                a[0] += 5;
                a[0];
            )");
            check_eq((int64_t)15, r.as_int());
        });

        // ---- fused subscript-read operands + interp subscript-compound ----

        test("fused_subscript_operand_errors_and_maps", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            // OOB through the fused operand keeps the exact op_index error
            check_throws([&]() { e->execute("var a = [1]; int i = 5; a[i] + 1;"); });
            // Map base takes the replay path: never inserts, missing key = null
            auto r = e->execute(R"(
                var m = {"x": 7};
                auto hit = m["x"] + 1;
                to_string(m.size()) + "|" + to_string(hit);
            )");
            check_eq(std::string("1|8"), r.as_string());
        });

        test("fused_subscript_chain_errors", [this]() {
            auto e = make_engine();
            // OOB at an INNER level keeps the exact op_index error
            check_throws([&]() { e->execute("var g = [[1]]; int i = 9; g[i][0] + 1;"); });
            // OOB at the LAST level too
            check_throws([&]() { e->execute("var g = [[1]]; int j = 9; g[0][j] + 1;"); });
        });

        // A pooled holder escaping the engine must stay destructible after engine death
        // (orphaned pool: the last release frees it, no crash / no touch of freed engine).
        // A by-ref lambda capture boxes x into a CELL reference_holder held by the
        // closure env, so copying the function value out keeps a pooled block alive.
        test("pool_orphan_reference_outlives_engine", [this]() {
            std::optional<script_value> escaped;
            {
                auto e = make_engine();
                e->execute("var x = 5; auto f = [&]() { return x + 1; };");
                escaped.emplace(e->get_variable("f"));
                check(escaped->is_function());
            }
            escaped.reset();   // release after the engine is gone
            check_true(true);
        });

        // Object holders come from the same kind of pool: churn reuses blocks, a holder released on another thread
        // is a foreign return, and holders still out when the engine dies free the orphaned pool on their last release
        test("pool_object_holders_churn_foreign_and_orphan", [this]() {
            std::vector<script_value> kept;
            std::optional<script_value> late;
            {
                auto e = make_engine();
                auto r = e->execute(R"(
                    class P { int v = 0; P(int n) { v = n; } }
                    int total = 0;
                    for (int i = 0; i < 3000; ++i) { P p = P(i); shared_ptr<P> q = new P(i + 1); total = total + p.v + q.v; }
                    total;
                )");
                check_eq((int64_t)9000000, r.as_int());
                e->execute("array<P> held = [P(1), P(2), P(3), P(4)]; shared_ptr<P> one = new P(5);");
                for (int i = 0; i < 4; ++i) { kept.push_back(e->execute("held[" + std::to_string(i) + "];")); }
                late.emplace(e->get_variable("one"));
                std::thread([&kept]() { kept.clear(); }).join();
                check_eq((int64_t)15, e->execute("int s = 0; for (auto& p : held) { s = s + p.v; } s + one.v;").as_int());
            }
            late.reset();
            check_true(true);
        });
    }
};

// math:: language intrinsics (detail/math_intrinsics.hpp): recognized as call shapes by
// both backends, evaluated through ONE shared kernel - values, errors, determinism, and
// coexistence with user code all pinned here.
class math_intrinsics_tests : public suite {
public:
    math_intrinsics_tests() : suite("Math Intrinsics") {}

    void forge_tests() override {
        test("numeric_values", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            auto r = e->execute(R"(
                to_string(math::floor(2.7)) + "|" + to_string(math::ceil(2.1)) + "|" +
                to_string(math::itrunc(3.9)) + "|" + to_string(math::ifloor(-1.5)) + "|" +
                to_string(math::sqrt(16.0)) + "|" + to_string(math::pow(2.0, 10.0)) + "|" +
                to_string(math::abs(-7)) + "|" + to_string(math::min(3, 9)) + "|" +
                to_string(math::max(3, 9)) + "|" + to_string(math::clamp(15, 0, 10));
            )");
            check_eq(std::string("2.0|3.0|3|-2|4.0|1024.0|7|3|9|10"), r.as_string());
        });

        // an evaluated argument survives a later argument's call, whose statements clear the interpreter's value stack at their
        // boundaries (the ordering fuzz band's probes: the interpreter read the callee's last statement value in its place)
        test("an_argument_survives_a_later_arguments_call", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            auto r = e->execute(R"(
                int G = 1; int H = 5; array<int> A = [1, 2];
                function wg() -> int { G = G + 100; return 1; }
                function wh() -> int { H = H + 1000; return 1; }
                function wa() -> int { A[1] = A[1] + 100; return 1; }
                function run() -> string {
                    string out = "";
                    G = 1; out += to_string(math::max(G, wg())) + " ";
                    G = 1; out += to_string(math::max(G, wh())) + " ";
                    out += to_string(math::max(-50, wa())) + " ";
                    out += to_string(math::max(A[0] * 3, wa())) + " ";
                    int x = 7; out += to_string(math::max(x, wa())) + " ";
                    G = 1; out += to_string(math::clamp(G, wg(), 1000)) + " ";
                    out += to_string(math::lerp(-50, wa(), 0.0));
                    return out;
                }
                run() + " | " + to_string(math::max(-50, wa()));
            )");
            check_eq(std::string("1 1 1 3 7 1 -50.0 | 1"), r.as_string());
        });

        test("trig_and_distance", [this]() {
            auto e = make_engine();
            check_near(0.0, e->execute("math::sin(0.0)").as_float(), 1e-12);
            check_near(1.0, e->execute("math::cos(0.0)").as_float(), 1e-12);
            check_near(5.0, e->execute("math::distance(0.0, 0.0, 3.0, 4.0)").as_float(), 1e-12);
            check_near(25.0, e->execute("math::distance_squared(0.0, 0.0, 3.0, 4.0)").as_float(), 1e-12);
            check_near(180.0, e->execute("math::radians_to_degrees(3.14159265358979323846)").as_float(), 1e-9);
            check_near(3.14159265358979323846, e->execute("math::degrees_to_radians(180.0)").as_float(), 1e-12);
        });

        // The mix family must be VALUE-IDENTICAL to the stdlib registrations it
        // supersedes (scripts migrate without hash drift); lerp == mix by ruling.
        test("mix_family_matches_stdlib", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            auto r = e->execute(R"(
                auto ok = math::mix(2.0, 10.0, 0.25) == math::mix(2.0, 10.0, 0.25) &&
                          math::lerp(2.0, 10.0, 0.25) == math::mix(2.0, 10.0, 0.25) &&
                          math::unmix(2.0, 10.0, 4.0) == math::unmix(2.0, 10.0, 4.0) &&
                          math::mix_in(0.0, 1.0, 0.3, 2.0) == math::mix_in(0.0, 1.0, 0.3, 2.0) &&
                          math::mix_out(0.0, 1.0, 0.3, 2.0) == math::mix_out(0.0, 1.0, 0.3, 2.0) &&
                          math::mix_in_out(0.0, 1.0, 0.7, 2.0) == math::mix_in_out(0.0, 1.0, 0.7, 2.0) &&
                          math::mix_out_in(0.0, 1.0, 0.7, 2.0) == math::mix_out_in(0.0, 1.0, 0.7, 2.0) &&
                          math::unmix_in(0.0, 1.0, 0.3, 2.0) == math::unmix_in(0.0, 1.0, 0.3, 2.0) &&
                          math::unmix_out(0.0, 1.0, 0.3, 2.0) == math::unmix_out(0.0, 1.0, 0.3, 2.0) &&
                          math::unmix_in_out(0.0, 1.0, 0.7, 2.0) == math::unmix_in_out(0.0, 1.0, 0.7, 2.0) &&
                          math::unmix_out_in(0.0, 1.0, 0.7, 2.0) == math::unmix_out_in(0.0, 1.0, 0.7, 2.0);
                ok;
            )");
            check(r.as_bool());
        });

        test("random_determinism_and_ranges", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            // Same seed -> exact same sequence
            auto r = e->execute(R"(
                random a(1234);
                auto a1 = a.next_float();
                auto a2 = a.next_int(0, 100);
                auto a3 = a.next_float(-2.5, 2.5);
                random b(1234);
                auto b1 = b.next_float();
                auto b2 = b.next_int(0, 100);
                auto b3 = b.next_float(-2.5, 2.5);
                a1 == b1 && a2 == b2 && a3 == b3;
            )");
            check(r.as_bool());
            // Inclusive int endpoints: a degenerate range returns its endpoint; floats stay in [lo, hi)
            check_eq((int64_t)7, e->execute("random(1).next_int(7, 7)").as_int());
            auto bounds = e->execute(R"(
                random g(42);
                auto ok = true;
                for (int i = 0; i < 200; ++i) {
                    auto u = g.next_float();
                    if (u < 0.0 || u >= 1.0) { ok = false; }
                    auto n = g.next_int(1, 6);
                    if (n < 1 || n > 6) { ok = false; }
                    auto f = g.next_float(-1.0, 1.0);
                    if (f < -1.0 || f >= 1.0) { ok = false; }
                }
                ok;
            )");
            check(bounds.as_bool());
        });

        test("intrinsic_errors", [this]() {
            auto e = make_engine();
            check_throws([&]() { e->execute("math::floor(\"nope\");"); });          // type
            check_throws([&]() { e->execute("math::floor(1.0, 2.0);"); });          // arity
            // Engine stays usable after an intrinsic error
            check_eq((int64_t)3, e->execute("1 + 2").as_int());
        });

        // Intrinsics coexist with user code: other namespaces, operator overloads on
        // types, and the still-registered bare stdlib names are all untouched.
        test("coexists_with_user_code", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            auto r = e->execute(R"(
                namespace geo { float area(float w, float h) { return w * h; } }
                class Vec {
                    float x = 0.0; float y = 0.0;
                    Vec(float px, float py) { x = px; y = py; }
                    function operator+(Vec o) -> Vec { return Vec(x + o.x, y + o.y); }
                }
                auto v = Vec(1.0, 2.0) + Vec(3.0, 4.0);
                auto bare = math::floor(2.9);
                to_string(geo::area(3.0, 4.0)) + "|" + to_string(v.x + v.y) + "|" +
                    to_string(bare) + "|" + to_string(math::floor(2.9));
            )");
            check_eq(std::string("12.0|10.0|2.0|2.0"), r.as_string());
        });

        // Parallel bodies admit math:: intrinsics (admission-time resolution doubles as the AST-cache prewarm); reseeding a
        // captured random is refused
        test("intrinsics_in_parallel_bodies", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            auto r = e->execute(R"(
                var xs = [1.44, 4.0, 9.0, 20.25];
                int body(float x) { return math::itrunc(math::sqrt(x) * 10.0); }
                var out = parallel_transform(xs, body);
                to_string(out[0]) + "|" + to_string(out[1]) + "|" + to_string(out[2]) + "|" + to_string(out[3]);
            )");
            check_eq(std::string("12|20|30|45"), r.as_string());
            check_throws([&]() {
                e->execute(R"(
                    var ys = [1.0, 2.0];
                    random shared(3);
                    float rbody(float y) { shared.seed(7); return y; }
                    parallel_transform(ys, rbody);
                )");
            });
        });

        // Intrinsics inside hot shapes: loops, method bodies, nested in expressions
        test("intrinsics_in_expressions", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                auto total = 0;
                for (int i = 0; i < 10; ++i) {
                    total += math::itrunc(math::sqrt(100.0) + i);
                }
                total;
            )");
            check_eq((int64_t)145, r.as_int());
        });

        // ---- math::wrap_add / wrap_sub / wrap_mul: explicit mod-2^64 arithmetic,
        // POLICY-INDEPENDENT (identical in checked and wrap overflow builds) — the
        // sanctioned replacement for the 16-bit limb products GLOOM/demoreel/crawler
        // hand-roll around the checked '*'. No throw_on_overflow() gate anywhere here:
        // unconditional values ARE the policy-independence pin. ----
        test("wrap_trio_values", [this]() {
            auto e = make_engine();
            check_eq((int64_t)INT64_MIN, e->execute("math::wrap_add(9223372036854775807, 1)").as_int());
            check_eq((int64_t)INT64_MAX, e->execute("math::wrap_sub(-9223372036854775807 - 1, 1)").as_int());
            check_eq((int64_t)-2, e->execute("math::wrap_mul(9223372036854775807, 2)").as_int());
            check_eq((int64_t)1, e->execute("math::wrap_mul(-1, -1)").as_int());
            check_eq((int64_t)8589934593LL, e->execute("math::wrap_mul(4294967297, 4294967297)").as_int());   // (2^32+1)^2 mod 2^64 = 2^33+1
            // Agrees exactly with checked ops when nothing overflows
            check_eq((int64_t)4000000000LL, e->execute("math::wrap_mul(2000000000, 2)").as_int());
            check_eq((int64_t)-5, e->execute("math::wrap_add(-2, -3)").as_int());
        });
        // Equivalence proof against INDEPENDENT arithmetic: a 16-bit limb multiply
        // (crawler.jai's u64_mul shape — masked columns, no op ever near overflow)
        // must agree with wrap_mul across seeded full-range operands and edge pairs.
        test("wrap_mul_matches_limb_multiply", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            auto r = e->execute(R"(
                function limb_mul(int a, int b) -> int {
                    int a0 = a & 0xFFFF; int a1 = (a >> 16) & 0xFFFF; int a2 = (a >> 32) & 0xFFFF; int a3 = (a >> 48) & 0xFFFF;
                    int b0 = b & 0xFFFF; int b1 = (b >> 16) & 0xFFFF; int b2 = (b >> 32) & 0xFFFF; int b3 = (b >> 48) & 0xFFFF;
                    int c0 = a0 * b0;
                    int c1 = a1 * b0 + a0 * b1;
                    int c2 = a2 * b0 + a1 * b1 + a0 * b2;
                    int c3 = a3 * b0 + a2 * b1 + a1 * b2 + a0 * b3;
                    int r0 = c0 & 0xFFFF; int t1 = c1 + (c0 >> 16);
                    int r1 = t1 & 0xFFFF; int t2 = c2 + (t1 >> 16);
                    int r2 = t2 & 0xFFFF; int t3 = c3 + (t2 >> 16);
                    return r0 | (r1 << 16) | (r2 << 32) | ((t3 & 0xFFFF) << 48);
                }
                random operands(666);
                auto ok = limb_mul(9223372036854775807, 2) == math::wrap_mul(9223372036854775807, 2) &&
                          limb_mul(-1, -1) == math::wrap_mul(-1, -1) &&
                          limb_mul(-9223372036854775807 - 1, 3) == math::wrap_mul(-9223372036854775807 - 1, 3);
                for (int i = 0; i < 20; ++i) {
                    auto a = operands.next_int(-9223372036854775807 - 1, 9223372036854775807);
                    auto b = operands.next_int(-9223372036854775807 - 1, 9223372036854775807);
                    if (limb_mul(a, b) != math::wrap_mul(a, b)) { ok = false; }
                }
                ok;
            )");
            check(r.as_bool());
        });
        test("wrap_trio_errors", [this]() {
            auto e = make_engine();
            check_throws([&]() { e->execute("math::wrap_add(1);"); });          // arity
            check_throws([&]() { e->execute("math::wrap_mul(1.5, 2);"); });     // float rejected
            check_throws([&]() { e->execute("math::wrap_sub(\"a\", 2);"); });   // type
            check_eq((int64_t)3, e->execute("1 + 2").as_int());                 // engine stays usable
        });
    }
};

// The VM's int/float slot store lane (a payload of the slot's own storage and interned tag skips
// enforcement): everything these tests pin is the behavior that must survive the skip.
class typed_store_proof_tests : public suite {
public:
    typed_store_proof_tests() : suite("Typed Store Proof") {}

    void forge_tests() override {
        // Provable store must not strip the slot's type lock: the float store AFTER the
        // provable int store still converts.
        test("typed_stores_under_the_proof_convert_lock_and_stay_dynamic_as_declared", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"provable_store_keeps_type_lock", R"(
                auto f() -> auto {
                    int x = 0;
                    x = 5;
                    var fl = 4.7;
                    try { x = fl; } catch (e) { return `${e}|${x}`; }
                    return "stored";
                }
                f();
            )", std::string("Cannot assign float to 'x' (declared int)|5")},
                {"provable_int_loop_correct", R"(
                auto f() -> auto {
                    int total = 0;
                    for (int i = 0; i < 100; i = i + 1) { total = total + i; }
                    return total;
                }
                f();
            )", int64_t{4950}},
                {"mixed_rhs_casts_each_store", R"(
                auto f() -> auto {
                    int x = 0;
                    for (int i = 0; i < 10; i = i + 1) { x = cast<int>(x + 1.5); }
                    return x;
                }
                f();
            )", int64_t{10}},
                {"bool_rhs_still_converts", R"(
                auto f() -> auto { int x = 0; x = (1 < 2); return x; }
                f();
            )", int64_t{1}},
                {"var_stays_dynamic_after_int_store", R"(
                auto f() -> auto { var v = 0; v = 1; v = "s"; return v; }
                f();
            )", std::string("s")},
                {"auto_stays_locked_after_int_store", R"(
                auto f() -> auto { auto a = 1; a = 2; var fl = 3.9; try { a = fl; } catch (e) { return `${e}|${a}`; } return "stored"; }
                f();
            )", std::string("Cannot assign float to 'a' (declared int)|2")},
                {"ref_alias_and_cell_stores_unaffected", R"(
                function poke(int& t) -> int {
                    t = 5;
                    var fl = 6.9;
                    try { t = fl; } catch (e) { t = t + 1; }
                    return t;
                }
                auto f() -> auto {
                    int x = 1;
                    auto seen = poke(x);
                    x = 42;
                    return seen * 100 + x;
                }
                f();
            )", int64_t{642}},
                {"compound_store_refusal_unaffected", R"(
                auto f() -> auto { int x = 1; var d = 2.5; try { x += d; } catch (e) { return `${e}|${x}`; } return "stored"; }
                f();
            )", std::string("Cannot assign float to 'x' (declared int)|1")},
            };
            check_values(cases);
        });

        test("float_slot_int_rhs_still_converts", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                auto f() -> auto {
                    float v = 0.0;
                    [[warn_mode]] v = 2.5;
                    v = 3;
                    return v / 2;
                }
                f();
            )");
            check_near(1.5, r.as_float(), 0.0001);   // 3 stored as float, / stays float
        });
        test("typed_mismatch_still_errors", [this]() {
            auto e = make_engine();
            check_throws([&]() {
                e->execute(R"(
                    auto f() -> auto { int x = 0; x = 5; x = "nope"; return x; }
                    f();
                )");
            });
        });
        test("provable_float_loop_correct", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                auto f() -> auto {
                    float acc = 0.0;
                    for (int i = 0; i < 10; i = i + 1) { acc = acc + 0.5; }
                    return acc;
                }
                f();
            )");
            check_near(5.0, r.as_float(), 0.0001);
        });
        // cpp-bound global targets keep write-through (globals have no slot, so
        // stores to them are never stamped).
        test("cpp_bound_global_write_through_unaffected", [this]() {
            auto e = make_engine();
            int64_t bound = 1;
            e->add_global_ref("bound", bound);
            e->execute(R"(
                auto f() -> auto { bound = 7; return bound; }
                f();
            )");
            check_eq((int64_t)7, bound);
        });
        // Scope exactness: after an inner block's `int x` expires, a same-named store
        // targets the global var - which must stay dynamic (no stolen proof).
        test("expired_block_shadow_never_locks_outer_var", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            auto r = e->execute(R"(
                var x = 0;
                auto f() -> auto { { int x = 1; } x = 3; return 0; }
                f();
                auto after = x;
                x = "s";
                to_string(after) + x;
            )");
            check_eq(std::string("3s"), r.as_string());
        });
        // math:: intrinsic classifications: int-returning and float-returning stores.
        test("math_intrinsic_classified_stores", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                auto f() -> auto {
                    int i = 0;
                    i = math::ifloor(2.9);
                    float g = 0.0;
                    g = math::sqrt(9.0);
                    return i + g;
                }
                f();
            )");
            check_near(5.0, r.as_float(), 0.0001);
        });
    }
};

// Flat-value-class stamp (class_definition::flat_value_semantics, parallel_for v1):
// instances of a flat class are alias-free by construction, so parallel workers may
// mutate an owned instance in place. These pin the stamp's shape rules and the
// epoch-based invalidation (a nested field class redefining must invalidate its
// CONTAINING classes' stamps).
class flat_value_class_tests : public suite {
public:
    flat_value_class_tests() : suite("Flat Value Class") {}

    bool flat_of(std::shared_ptr<jai::engine>& e, const char* name) {
        auto cls = detail::engine_access::get_class_registry(*e).find_script_class(name);
        check_not_null(cls.get());
        return cls->flat_value_semantics();
    }

    void forge_tests() override {
        test("the_flat_stamp_follows_field_kinds_containment_and_bases", [this]() {
            const std::tuple<const char*, const char*, const char*, bool> cases[] = {
                {"all_primitive_fields_flat", "class P { int a = 0; float b = 1.5; bool c = false; char d = 'x'; }", "P", true},
                {"no_fields_flat", "class P { int helper(int v) { return v * 2; } }", "P", true},
                {"string_field_flat", "class P { int a = 0; string s = \"\"; }", "P", true},
                {"var_field_not_flat", "class P { var v = 0; }", "P", false},
                {"typed_value_container_fields_flat", "class P { array<int> xs = []; array<string> names = []; int n = 0; }", "P", true},
                {"untyped_container_field_not_flat", "class P { var xs = []; }", "P", false},
                {"nested_container_field_flat", "class P { array<array<int>> grid = []; map<string, array<float>> table = {}; }", "P", true},
                {"nested_flat_class_field_flat", R"(
                    class Vec2 { int x = 0; int y = 0; }
                    class Outer { Vec2 a = Vec2(); array<Vec2> pts = []; int n = 0; }
            )", "Vec2", true},
                {"nested_flat_class_field_flat", R"(
                    class Vec2 { int x = 0; int y = 0; }
                    class Outer { Vec2 a = Vec2(); array<Vec2> pts = []; int n = 0; }
            )", "Outer", true},
                {"nested_var_field_poisons_outer", R"(
                    class Inner { var anything = 0; }
                    class Outer { Inner a = Inner(); }
            )", "Outer", false},
                {"weak_ptr_field_not_flat", "class P { int a = 0; weak_ptr<P> other = null; }", "P", false},
                {"flat_inheritance_chain_flat", R"(
                    class Base { int hp = 10; string name = ""; }
                    class Kid : Base { float speed = 1.0; }
            )", "Kid", true},
                {"var_parent_poisons_child", R"(
                    class Base { var payload = 0; }
                    class Kid : Base { int hp = 10; }
            )", "Kid", false},
            };
            check_each(cases, [this](const auto& c) {
                auto e = make_engine();
                e->execute(std::get<1>(c));
                check_eq(std::get<3>(c), flat_of(e, std::get<2>(c)), std::get<2>(c));
            });
        });

        // THE invalidation case: Outer's cached stamp must fall when Inner hot-reloads
        // to a non-flat shape (per-class epochs cannot see containment; the stamp
        // validates against engine::class_definition_epoch).
        test("nested_redefinition_invalidates_containing_stamp", [this]() {
            auto e = make_engine();
            e->execute(R"(
                class Inner { float x = 0.0; }
                class Outer { Inner a = Inner(); }
            )");
            check_true(flat_of(e, "Outer"));
            e->execute("class Inner { float x = 0.0; var tag = 0; }");
            check_false(flat_of(e, "Outer"));
        });
        test("cpp_class_not_flat", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(e);
            auto r = e->execute("pair(1, 2)");
            auto holder = r.get_object_holder();
            check_not_null(holder.get());
            auto pair_cls = detail::engine_access::get_class_definition(*e, holder->type_id());
            check_not_null(pair_cls.get());
            check_false(pair_cls->flat_value_semantics());
        });
    }
};

// Char integral promotion (from the DOOM WAD-parser feedback):
// a char operand enters ARITHMETIC and BITWISE binary operators as int64 in 0..255
// (unsigned by spec). A char beside a string is its one-character text instead: it compares,
// joins, fills slots, matches cases and keys maps as that string. One shared kernel,
// detail/char_promotion.hpp, feeds both backends.
class char_promotion_tests : public suite {
public:
    char_promotion_tests() : suite("Char Promotion") {}

    void forge_tests() override {
        test("char_operands_promote_to_unsigned_bytes_in_arithmetic_bitwise_and_comparisons", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"char_minus_char", "'z' - 'a'", int64_t{25}},
                {"char_plus_int", "'a' + 1", int64_t{98}},
                {"digit_parse_shape", R"(
                var s = "42";
                (s[0] - '0') * 10 + (s[1] - '0')
            )", int64_t{42}},
                {"typed_char_local_promotes", "char c = 'b'; c - 'a'", int64_t{1}},
                {"char_bitwise_and_mask", "'A' & 0x20", int64_t{0}},
                {"char_bitwise_and_mask", "'a' & 0x20", int64_t{32}},
                {"char_bitwise_and_mask", "'a' & 'b'", int64_t{96}},
                {"char_shift_and_modulo", "'a' << 1", int64_t{194}},
                {"char_shift_and_modulo", "'a' % 2", int64_t{1}},
                {"unsigned_high_bit_byte", "var s = \"\xC3\"; s[0] + 0", int64_t{195}},
                {"unsigned_high_bit_byte", "var s = \"\xC3\"; s[0] * 1", int64_t{195}},
                {"char_equals_int", "'a' == 97", true},
                {"char_equals_int", "97 == 'a'", true},
                {"char_equals_int", "'a' != 98", true},
                {"char_equals_int", "'a' == 98", false},
                {"char_equals_int", "var s = \"\\x1A\"; s[0] == 0x1A", true},
                {"char_ordered_vs_int", "'a' < 98", true},
                {"char_ordered_vs_int", "'a' >= 97", true},
                {"char_ordered_vs_int", "64 < 'A'", true},
                {"char_ordered_vs_int", "'a' < 'b'", true},
                {"char_ordered_vs_int", "'a' == 'a'", true},
                {"char_spaceship_promotes", "'a' <=> 65", int64_t{1}},
                {"char_spaceship_promotes", "'b' <=> 'b'", int64_t{0}},
                {"char_spaceship_promotes", "'A' <=> 'a'", int64_t{-1}},
                {"high_bit_comparisons_unsigned", "var s = \"\\xC3\"; s[0] > 'a'", true},
                {"high_bit_comparisons_unsigned", "var s = \"\\xC3\"; s[0] > 127", true},
                {"high_bit_comparisons_unsigned", "var s = \"\\xC3\"; s[0] <=> 'a'", int64_t{1}},
            };
            check_values(cases);
        });
        test("char_float_mix_promotes_to_float", [this]() {
            auto e = make_engine();
            check_near(145.5, e->execute("'a' * 1.5").as_float(), 1e-9);
        });
        test("compound_assign_through_promotion", [this]() {
            auto e = make_engine();
            check_eq((int64_t)102, e->execute("var x = 5; x += 'a'; x").as_int());
            // a var holding a char takes the promoted int from -= and ++ on every storage: a local, a slot, a field of this, a
            // member and an element; a char-declared one refuses it (a load error; [[warn_mode]] reaches the run-time refusal)
            jai::stdlib::register_all(e);
            check_eq(std::string("97 99 97 99 97 99 97 99 97 99 98 a"), e->execute(R"(
                class P { var c = 'b'; string m() { c -= 1; string s = `${c}`; c = 'b'; c++; return `${s} ${c}`; } }
                function slot() -> string { var x = 'b'; x -= 1; var y = 'b'; y++; return `${x} ${y}`; }
                var lx = 'b'; lx -= 1; var ly = 'b'; ly++;
                P p; P q; q.c = 'b'; q.c -= 1; P r; r.c++;
                var arr = ['b', 'b']; arr[0] -= 1; arr[1]++;
                var w = 'a'; var old = w++;
                `${lx} ${ly} ${slot()} ${p.m()} ${q.c} ${r.c} ${arr[0]} ${arr[1]} ${w} ${old}`
            )").as_string());
            check_eq(std::string("Cannot assign int to 'tc' (declared char)"),
                e->execute("char tc = 'b'; string got = \"\"; try { [[warn_mode]] tc++; } catch (err) { got = err.message; } got").as_string());
            // an overflow names the compound or the step, whatever the storage
            if (!e->throw_on_overflow()) { return; }
            check_eq(std::string("Integer overflow in '+=';Integer overflow in '++';Integer overflow in '*=';Integer overflow in '--';"
                                 "Integer overflow in '+=';Integer overflow in '++';Integer overflow in '++';Integer overflow in '--'"), e->execute(R"(
                class H { int x = 9223372036854775807; int y = -9223372036854775807 - 1; }
                H h; array<int> a = [9223372036854775807]; map<string, int> m; m["k"] = 9223372036854775807; var d = [-9223372036854775807 - 1];
                string out = "";
                try { h.x += 1; } catch (err) { out += `${err.message};`; }
                try { h.x++; } catch (err) { out += `${err.message};`; }
                try { a[0] *= 2; } catch (err) { out += `${err.message};`; }
                try { h.y--; } catch (err) { out += `${err.message};`; }
                try { m["k"] += 1; } catch (err) { out += `${err.message};`; }
                try { a[0]++; } catch (err) { out += `${err.message};`; }
                try { ++a[0]; } catch (err) { out += `${err.message};`; }
                try { --d[0]; } catch (err) { out += err.message; }
                out
            )").as_string());
        });
        // a char beside a string joins as its character, in either order, as std::string's + does; a number beside one stays the error
        test("a_char_beside_a_string_joins", [this]() {
            auto e = make_engine();
            check_eq(std::string("xpost prex abc cab abcd"),
                e->execute("var c = 'x'; string s = \"ab\"; string t = \"abc\"; `${c + \"post\"} ${\"pre\" + c} ${s + 'c'} ${'c' + s} ${s + t[2] + 'd'}`").as_string());
            check_false(e->check("string s = \"ab\"; string t = s + 'c'; string u = 'c' + s;").has_errors());
            check_eq((int64_t)98, e->execute("'a' + 1").as_int());
            check_eq(std::string("cannot apply '+' to 'string' and 'int': stringify it with to_string(x) or `${x}`"),
                e->execute("var n = 1; string got = \"\"; try { got = \"a\" + n; } catch (err) { got = err.message; } got").as_string());
            check_eq(std::string("hellllo"), e->execute("string s = \"hello\"; s[2] += \"ll\"; s").as_string());
        });
        test("no_implicit_int_to_char", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(e);
            // The promoted result is an int; it does not sneak back into the char domain.
            check_eq(std::string("int"), e->execute("type_of('a' + 1)").as_string());
            check_eq(std::string("char"), e->execute("type_of('a')").as_string());
        });
        // a char compared with a string compares as its one-character text, in either order and under every comparison; a string of
        // another length compares as text too (the empty string first)
        test("char_compares_with_a_string_as_text", [this]() {
            auto e = make_engine();
            const std::string probe = R"(
                string s = "abc"; var c = s[0]; var one = "a"; var two = "ab"; var none = "";
                string out = "";
                for (var r : [s[0] == "a", "a" == s[0], s[1] == "a", c == one, one == c, c != one, c == two, c != two, c == none, c != none,
                              c < "b", "b" > c, c <= one, c >= one, c < two, two > c, none < c, c > none, s[2] > "bz"]) {
                    out += r ? "1" : "0";
                }
                `${out} ${c <=> one} ${c <=> two} ${two <=> c} ${c <=> none}`
            )";
            check_eq(std::string("1101100101111111111 0 -1 1 1"), e->execute(probe).as_string());
            check_false(e->check("char c = 'a'; string t = \"ab\"; bool r = c == \"a\" && c < t && \"\" < c;").has_errors());
            check_eq((int64_t)3, e->execute("int n = 0; for (auto ch : \"banana\") { if (ch == \"a\") { n++; } } n").as_int());
            check_eq((int64_t)2, e->execute("string w = \"xyxx\"; int n = 0; for (int i = 0; i < w.size(); i++) { if (w[i] != \"x\") { continue; } if (\"x\" == w[i] && i > 0) { n++; } } n").as_int());
        });
        // a char slot takes a string of exactly one character, a string slot a char: every slot kind, on both backends
        test("char_and_string_slots_take_each_other", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(e);
            check_eq(std::string("q char m string r char s string t char p char q string w char ['a', 'b', 'c'] char [\"a\", \"b\"] string"), e->execute(R"(
                char k = "q"; string m = 'm';
                string got = `${k} ${type_of(k)} ${m} ${type_of(m)}`;
                string f(char x) { return `${x} ${type_of(x)}`; }
                string g(string x) { return `${x} ${type_of(x)}`; }
                char ret() { return "t"; }
                class P { char ch = "p"; string st = 'q'; }
                P p;
                got += ` ${f("r")} ${g('s')} ${ret()} ${type_of(ret())} ${p.ch} ${type_of(p.ch)} ${p.st} ${type_of(p.st)}`;
                p.ch = "w";
                array<char> ac = ["a", "b"]; ac.push("c");
                array<string> as = ['a']; as.push('b');
                got += ` ${p.ch} ${type_of(p.ch)} ${ac} ${type_of(ac[0])} ${as} ${type_of(as[0])}`;
                got
            )").as_string());
            // among overloads a char reaches text as a conversion ahead of a bool's truthiness and a var parameter
            check_eq(std::string("string string"), e->execute(
                "string h(bool b) { return \"bool\"; } string h(string q) { return \"string\"; } string v(var x) { return \"var\"; } "
                "string v(string q) { return \"string\"; } char k = 'k'; `${h(k)} ${v(k)}`").as_string());
            const std::string refused = "Cannot convert 'string' to 'char': a char takes a string of exactly one character";
            const std::vector<std::string> stores = {
                "var v = \"zz\"; char d = v;", "var v = \"\"; char d = 'a'; d = v;", "var v = \"zz\"; void f(char x) {} f(v);",
                "var v = \"zz\"; char r() { return v; } r();", "var v = \"zz\"; class P { char ch = 'a'; } P p; p.ch = v;",
                "var v = \"zz\"; array<char> a; a.push(v);", "var v = \"zz\"; array<char> a = ['a']; a[0] = v;",
                "var v = \"zz\"; map<char, int> m; m[v] = 1;", "var v = [\"a\", \"zz\"]; array<char> a = v;"};
            for (const auto& store : stores) {
                check_eq(refused, e->execute("string got = \"\"; try { " + store + " } catch (err) { got = err.message; } got").as_string(), store);
            }
            const std::string load = "a char takes a string of exactly one character";
            for (const std::string src : {"char a = \"ab\";", "char b = \"\";", "void f(char x) {} f(\"xy\");", "array<char> a; a.push(\"zz\");",
                                          "char r() { return \"no\"; }", "class Q { char c = \"qq\"; }", "map<char, int> m = {\"ab\": 1};",
                                          "char c = 'a'; c = \"bc\";"}) {
                auto rep = e->check(src);
                check_eq((size_t)1, rep.error_count(), rep.format(src));
                check_true(rep.format(src).find(load) != std::string::npos, rep.format(src));
            }
            // a bound C++ char parameter converts as a script char slot does, among host overloads too
            struct char_taker {
                std::string got;
                char mark = '-';
                char_taker() = default;
                explicit char_taker(char a_c) : got(1, a_c) {}
            };
            e->add_function("host_char", [](char a_c) { return std::string(1, a_c) + "!"; });
            e->add_function("host_char_ref", [](const char& a_c) { return std::string(1, a_c) + "?"; });
            e->add_function("host_pick", [](bool) { return std::string("bool"); });
            e->add_function("host_pick", [](char a_c) { return std::string("char ") + a_c; });
            dynamic_binder<char_taker>(*e, "CharTaker")
                .constructor<>()
                .constructor<char>()
                .property("mark", &char_taker::mark)
                .method("take", [](char_taker& a_self, char a_c) { a_self.got += a_c; return a_self.got; })
                .static_method("echo", [](char a_c) { return a_c; })
                .static_method("echo_ref", [](const char& a_c) { return a_c; })
                .build();
            check_eq(std::string("r! s! t! char u ab b char q? q zy m"), e->execute(R"(
                var v = "s";
                CharTaker taker;
                taker.take("a");
                string mark_of(string m) { CharTaker t; t.mark = m; return `${t.mark}`; }
                `${host_char("r")} ${host_char(v)} ${host_char('t')} ${host_pick("u")} ${taker.take(v == "s" ? "b" : "c")} ${CharTaker::echo("b")} ${type_of(CharTaker::echo("b"))} ${host_char_ref("q")} ${CharTaker::echo_ref('q')} ${CharTaker("z").take("y")} ${mark_of("m")}`
            )").as_string());
            for (const std::string call : {"host_char(v)", "host_char(\"\")", "host_pick(v)", "CharTaker().take(v)", "CharTaker::echo(v)", "host_char_ref(v)", "CharTaker::echo_ref(v)", "CharTaker(v)", "CharTaker t; t.mark = v"}) {
                check_eq(refused, e->execute("string got = \"\"; var v = \"zz\"; try { " + call + "; } catch (err) { got = err.message; } got").as_string(), call);
            }
        });
        // a switch runs the language's ==: a char subject matches a one-character string case and a string subject a char case
        test("switch_matches_a_char_and_its_string", [this]() {
            auto e = make_engine();
            check_eq(std::string("hit hit n hit miss"), e->execute(R"(
                string out = "";
                var c = 'a'; var t = "a"; string w = "banana"; string two = "ab";
                switch (c) { case "a": out += "hit"; break; default: out += "miss"; }
                switch (t) { case 'a': out += " hit"; break; default: out += " miss"; }
                switch (w[2]) { case "a": out += " a"; break; case "n": out += " n"; break; case "b": out += " b"; break; case "x": out += " x"; break; default: out += " none"; }
                switch (w[0]) { case 'b': out += " hit"; break; default: out += " miss"; }
                switch (two) { case 'a': out += " hit"; break; default: out += " miss"; }
                out
            )").as_string());
        });
        // a char key and its one-character string are one key (==, <=> and the key hash agree): one entry, kept in the spelling that
        // made it; a typed map stores its declared kind; to_json / from_json keep the entry and its kind
        test("a_char_key_and_its_string_are_one_key", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(e);
            check_eq(std::string("3 2 4 a:char:2 b:string:3 c:char:4 | 1 2 string | 1 2 char | 1 2 | 3 2 4 char string char | 2 2"), e->execute(R"(
                var u = {};
                u['a'] = 1; u["a"] = 2; u["b"] = 3; u['c'] = 4;
                string out = `${u.size()} ${u['a']} ${u["c"]}`;
                for (auto& [k, v] : u) { out += ` ${k}:${type_of(k)}:${v}`; }
                map<string, int> ms; ms['x'] = 1; ms["x"] += 1;
                for (auto& [k, v] : ms) { out += ` | ${ms.size()} ${ms['x']} ${type_of(k)}`; }
                map<char, int> mc; mc["y"] = 1; mc['y'] += 1;
                for (auto& [k, v] : mc) { out += ` | ${mc.size()} ${mc["y"]} ${type_of(k)}`; }
                var lit = {'z': 1, "z": 2};
                out += ` | ${lit.size()} ${lit["z"]}`;
                var back = from_json(to_json(u));
                out += ` | ${back.size()} ${back["a"]} ${back['c']}`;
                for (auto& [k, v] : back) { out += ` ${type_of(k)}`; }
                map<char, int> mc2 = from_json(to_json(mc));
                out += ` | ${mc2['y']} ${mc2["y"]}`;
                out
            )").as_string());
            check_true(e->execute("var m = {}; m['q'] = 1; m.contains(\"q\") && m.contains('q')").as_bool());
            check_true(e->execute("var m = {}; m[\"q\"] = 1; m.erase('q'); m.size() == 0").as_bool());
        });
        // Byte minting: \xNN escapes (exactly two hex digits) + stdlib to_char.
        test("hex_escapes_in_literals", [this]() {
            auto e = make_engine();
            check_eq(std::string("AB"), e->execute("\"\\x41\\x42\"").as_string());
            check(e->execute("'\\x41' == 'A'").as_bool());
            check_eq((int64_t)0, e->execute("\"\\x00\"[0] + 0").as_int());
            check_eq((int64_t)3, e->execute("\"a\\x00b\".size()").as_int());
            // Exactly two digits: the third hex char is literal text, not part of the escape.
            check_eq(std::string("A5"), e->execute("\"\\x415\"").as_string());
            check_eq(std::string("A1B5\xC3\xA9"), e->execute("`\\x41${1}\\x425\\xc3\\xA9`").as_string());
        });
        test("hex_escape_malformed_rejected", [this]() {
            auto e = make_engine();
            check_throws([&]() { e->execute("\"\\xZZ\""); });
            check_throws([&]() { e->execute("\"\\x4\""); });
            check_throws([&]() { e->execute("`\\xZZ`"); });
            check_throws([&]() { e->execute("`${1}\\x4`"); });
            check_eq((int64_t)3, e->execute("1 + 2").as_int());   // engine stays usable
        });
        test("to_char_mints_bytes", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(e);
            check(e->execute("to_char(65) == 'A'").as_bool());
            check_eq(std::string("char"), e->execute("type_of(to_char(200))").as_string());
            check_eq((int64_t)200, e->execute("to_char(200) + 0").as_int());
            // The BMP-writer shape: computed bytes append onto a binary string.
            check_eq(std::string("BC"), e->execute("var out = \"\"; out += to_char(66); out += to_char(67); out").as_string());
            check_throws([&]() { e->execute("to_char(256)"); });
            check_throws([&]() { e->execute("to_char(-1)"); });
        });
    }
};

// Constructor-call semantics pinned around the in-loop ctor-body entry (op_new slice
// B): the body runs on the dispatch loop for vm class/shared_ptr mints and direct
// class-name calls; every observable below must read identically on both backends and
// match the pre-slice probes.
class constructor_semantics_tests : public suite {
public:
    constructor_semantics_tests() : suite("Constructor Semantics") {}
    void forge_tests() override {
        test("every_copy_of_an_instance_runs_its_destructor", [this]() {
            // C++: a copied value instance is an object of its own and runs its destructor at scope exit, whatever made the copy
            const char* prelude = "var log = \"\"; class T { string n; T(string s) { n = s; } ~T() { log += n + \";\"; } } ";
            const std::pair<const char*, const char*> rows[] = {
                {"function f() { T a = T(\"a\"); T b = a; b.n = \"b\"; } f();", "a;b;"},
                {"function f() { T a = T(\"a\"); T b = T(\"x\"); b = a; b.n = \"b\"; } f();", "a;b;"},   // b = a copy-assigns: b keeps its object
                {"function h(T p) { p.n = \"p\"; } function f() { T a = T(\"a\"); h(a); } f();", "p;a;"},
                {"function r() -> T { T a = T(\"r\"); return a; } function f() { T x = r(); x.n = \"x\"; } f();", "x;"},
                {"function f() { T a = T(\"a\"); array<T> arr = [a]; arr[0].n = \"e\"; } f();", "a;e;"},
                {"class H { T t = T(\"h\"); } function f() { H h1 = H(); H h2 = h1; h2.t.n = \"g\"; } f();", "h;g;"},
                {"function f() { T a = T(\"a\"); var c = [=]() { return a.n; }; c(); } f();", "a;a;"},
            };
            for (bool vm : {false, true}) {
                for (const auto& [src, expected] : rows) {
                    auto e = make_engine();
                    e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                    jai::stdlib::register_all(e);
                    std::string got;
                    try { e->execute(std::string(prelude) + src); got = e->execute("log").as_string(); } catch (const std::exception& ex) { got = std::string("ERROR: ") + ex.what(); }
                    std::string sorted_got = got, sorted_expected = expected;
                    check_eq(std::string(expected).size(), got.size(), std::string(vm ? "vm: " : "interp: ") + src + " -> " + got);
                    std::vector<std::string> a_parts, b_parts;
                    for (size_t s = 0, k; (k = got.find(';', s)) != std::string::npos; s = k + 1) { a_parts.push_back(got.substr(s, k - s)); }
                    for (size_t s = 0, k; (k = sorted_expected.find(';', s)) != std::string::npos; s = k + 1) { b_parts.push_back(sorted_expected.substr(s, k - s)); }
                    std::sort(a_parts.begin(), a_parts.end());
                    std::sort(b_parts.begin(), b_parts.end());
                    check_true(a_parts == b_parts, std::string(vm ? "vm: " : "interp: ") + src + " -> " + got + " (expected the destructors " + expected + ")");
                }
            }
        });
        test("a_container_literal_holds_copies", [this]() {
            // C++ aggregate initialization copies its elements: a name, a member or an element in a literal is copied, a temporary moves
            const std::pair<const char*, const char*> rows[] = {
                {"class U { string n; U(string s) { n = s; } } U a = U(\"a\"); var arr = [a]; arr[0].n = \"e\"; a.n + arr[0].n", "ae"},
                {"class U { string n; U(string s) { n = s; } } U a = U(\"a\"); array<U> arr = [a, a]; arr[0].n = \"e\"; a.n + arr[0].n + arr[1].n", "aea"},
                {"class U { string n; U(string s) { n = s; } } U c = U(\"c\"); var m = {\"k\": c}; m[\"k\"].n = \"e\"; c.n + m[\"k\"].n", "ce"},
                {"array<int> x = [1]; var arr = [x]; arr[0][0] = 9; to_string(x[0]) + to_string(arr[0][0])", "19"},
                {"class U { string n; U(string s) { n = s; } } function f() -> string { U d = U(\"d\"); var arr = [d]; arr[0].n = \"e\"; return d.n + arr[0].n; } f()", "de"},
                {"class U { string n; U(string s) { n = s; } } U a = U(\"a\"); function g() -> string { a.n = \"z\"; return \"g\"; } var arr = [a, g()]; arr[0].n + a.n", "az"},
                {"class U { string n; U(string s) { n = s; } } U a = U(\"a\"); var m = {\"x\": a, \"y\": a}; m[\"x\"].n = \"e\"; a.n + m[\"x\"].n + m[\"y\"].n", "aea"},
                {"class U { string n; U(string s) { n = s; } } U a = U(\"a\"); var nest = [[a, a], {\"k\": a}]; nest[0][0].n = \"z\"; a.n + nest[0][0].n + nest[0][1].n + nest[1][\"k\"].n", "azaa"},
                {"class U { string n; U(string s) { n = s; } } U a = U(\"a\"); map<string, U> m = {\"p\": a, \"q\": a}; m[\"p\"].n = \"t\"; a.n + m[\"p\"].n + m[\"q\"].n", "ata"},
                {"class U { string n; U(string s) { n = s; } } class H { U one = U(\"o\"); array<U> ys = [one, one]; } H h = H(); h.ys[0].n = \"y\"; h.one.n + h.ys[0].n + h.ys[1].n", "oyo"},
                {"array<int> x = [1]; array<array<int>> g = [x, x]; g[0][0] = 9; to_string(x[0]) + to_string(g[0][0]) + to_string(g[1][0])", "191"},
                {"function f() { return [[1, 2]]; } var f1 = f(); f1[0][0] = 9; to_string(f()[0][0]) + to_string(f1[0][0])", "19"},
                {"int s = 0; for (int i = 0; i < 3; ++i) { var x = [[0], {\"k\": [0]}]; x[0][0] += 1; x[1][\"k\"][0] += 1; s += x[0][0] + x[1][\"k\"][0]; } s", "6"},
                {"class U { string n; U(string s) { n = s; } } shared_ptr<U> h = new U(\"h\"); var arr = [h, h]; var m = {\"k\": h}; arr[0].n = \"e\"; h.n + arr[1].n + m[\"k\"].n", "eee"},
                {"class U { string n; U(string s) { n = s; } } shared_ptr<U> h = new U(\"h\"); var arr = [h]; arr[0].n = \"e\"; h.n", "e"},
            };
            for (bool vm : {false, true}) {
                for (const auto& [src, expected] : rows) {
                    auto e = make_engine();
                    e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                    jai::stdlib::register_all(e);
                    std::string got;
                    try { got = e->execute(src).to_string(); } catch (const std::exception& ex) { got = std::string("ERROR: ") + ex.what(); }
                    check_eq(std::string(expected), got, std::string(vm ? "vm: " : "interp: ") + src);
                }
            }
        });
        test("ctor_falloff_returns_instance", [this]() {
            auto e = make_engine();
            check_eq((int64_t)5, e->execute("class A { int x = 1; A() { x = 5; } } auto a = A(); a.x").as_int());
        });
        test("ctor_valued_return_is_a_parse_error", [this]() {
            // A constructor cannot return a value; a lambda or nested function inside it can
            auto e = make_engine();
            jai::stdlib::register_all(e);
            std::string err;
            try { e->execute("class B { int x = 2; B() { x = 7; return 99; } }"); } catch (const std::exception& ex) { err = ex.what(); }
            check_true(err.find("a constructor cannot return a value") != std::string::npos, err);
            check_eq((int64_t)12, e->execute(
                "class C { int x = 0; C() { var f = [](int a) { return a * 2; }; function g(int b) -> int { return b + 1; } x = f(5) + g(1); } } C().x").as_int());
        });
        test("ctor_bare_return_yields_the_instance", [this]() {
            // A bare `return;` ends the constructor body early; the call yields the instance
            auto e = make_engine();
            jai::stdlib::register_all(e);
            check_eq(std::string("A"), e->execute(
                "class A { int x = 1; A() { return; } } type_of(A())").as_string());
            check_eq((int64_t)4, e->execute(
                "class G { int x = 1; G(int v) { x = 4; if (v > 0) { return; } x = 9; } } G(1).x").as_int());
            check_eq((int64_t)9, e->execute("G(0).x").as_int());
        });
        test("typed_declarations_default_or_directly_initialize", [this]() {
            const std::tuple<const char*, const char*, expected_value> cases[] = {
                {"typed_declarations_default_initialize", R"(
                class Vec2 { float x = 1.5; float y = 2.5; Vec2() {} Vec2(float a, float b) { x = a; y = b; } }
                class H { int n; Vec2 p; array<int> xs; static float s; }
                int gi; Vec2 gv;
                function run() -> string {
                    int i; float f; bool b; string s; array<int> xs; map<string, int> m; var v; Vec2 p;
                    H h = H();
                    return `${i}|${f}|${b}|${s}|${xs.size()}|${m.size()}|${type_of(v)}|${p.x}|${(h.xs.size() + 2)}|${(h.p.y + 1.0)}|${gi}|${gv.x}`;
                }
                run();
            )", std::string("0|0.0|false||0|0|null|1.5|2|3.5|0|1.5")},
                {"paren_and_brace_declarations_construct", R"(
                class Vec2 { float x = 1.5; float y = 2.5; Vec2() {} Vec2(float a, float b) { x = a; y = b; } }
                function run() -> string {
                    Vec2 a(3.0, 4.0); Vec2 b(); int i{5}; int z{}; float f(7.0); Vec2 c{};
                    return `${a.x},${a.y}|${b.x}|${i}|${z}|${f}|${c.y}`;
                }
                run();
            )", std::string("3.0,4.0|1.5|5|0|7.0|2.5")},
            };
            check_values(cases, true);
        });

        test("a_class_holding_itself_by_value_is_an_error", [this]() {
            // `Node next;` would build a Node inside every Node; a `shared_ptr<Node>` is a link
            auto e = make_engine();
            jai::stdlib::register_all(e);
            std::string err;
            try { e->execute("class Node { int v = 0; Node next; }"); } catch (const std::exception& ex) { err = ex.what(); }
            check_true(err.find("class 'Node' holds itself by value (Node.next): a class value is never null; make one link a 'shared_ptr<Node>' (an array or a map of them starts empty)") != std::string::npos, err);
            check_eq((int64_t)2, e->execute("class Link { int v = 0; shared_ptr<Link> next = null; Link(int x) { v = x; } } Link a = Link(1); a.next = Link(2); a.next.v").as_int());
        });
        test("ctor_throw_leaks_no_instance", [this]() {
            // A throw propagates, the target keeps its value, and the half-built instance runs no destructor
            auto e = make_engine();
            jai::stdlib::register_all(e);
            check_eq(std::string("3|null|bad|bad|[]"), e->execute(R"(
                var LOG = [];
                class Bad { int x = 1; Bad(int v) { x = 5; if (v == 0) { throw "bad"; } x = v; } ~Bad() { LOG.push(x); } }
                function run() -> string {
                    Bad keep = Bad(3); string err = ""; var fresh = null;
                    try { keep = Bad(0); } catch (ex) { err = ex; }
                    try { fresh = Bad(0); } catch (ex) { err = err + "|" + ex; }
                    return `${keep.x}|${type_of(fresh)}|${err}|${to_json(LOG)}`;
                }
                run();
            )").as_string());
        });
        test("shared_ptr_mint_stamps_type", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(e);
            // type_of names the held class on both backends; the shared_ptr stamp's
            // observable is weak_ptr minting from the result
            check_eq(std::string("C"), e->execute(
                "class C { int x = 3; C(int v) { x = v; } } var c = new shared_ptr<C>(9); type_of(c)").as_string());
            check_eq((int64_t)9, e->execute(
                "class C { int x = 3; C(int v) { x = v; } } var c = new shared_ptr<C>(9); c.x").as_int());
        });
        test("ctor_throw_is_catchable_at_call_site", [this]() {
            auto e = make_engine();
            check_eq(std::string("caught:boom"), e->execute(R"(
                class T { T() { throw "boom"; } }
                var msg = "";
                try { var t = T(); } catch (ex) { msg = "caught:" + ex; }
                msg
            )").as_string());
        });
        test("ctor_super_and_delegation_compose", [this]() {
            auto e = make_engine();
            // C++ delegation runs the whole target constructor: super(3 * 2) sets b = 6, the target body d = 3, then the
            // delegating body d = 43; b + d = 49
            check_eq((int64_t)49, e->execute(R"(
                class Base { int b = 0; Base(int v) { b = v; } }
                class D : Base {
                    int d = 0;
                    D(int v) : super(v * 2) { d = v; }
                    D() : this(3) { d = d + 40; }
                }
                auto x = D();
                x.b + x.d
            )").as_int());
            // super alone, no delegation: both fields observable
            check_eq((int64_t)9, e->execute(R"(
                class Base { int b = 0; Base(int v) { b = v; } }
                class E : Base { int d = 0; E(int v) : super(v * 2) { d = v; } }
                auto x = E(3);
                x.b + x.d
            )").as_int());
        });
        test("ctor_recursion_in_body", [this]() {
            auto e = make_engine();
            // A ctor body calling the class recursively exercises reentrant in-loop
            // ctor frames (the transient-function/window discipline)
            check_eq((int64_t)3, e->execute(R"(
                class N {
                    int depth = 0;
                    N(int d) {
                        depth = d;
                        if (d > 0) { auto child = N(d - 1); depth = child.depth + 1; }
                    }
                }
                N(3).depth
            )").as_int());
        });
    }
};

// A dynamic_binder-bound value type whose copy constructor counts: the in-flight copy
// counter the by-value return rows use (script instances copy without a hook)
struct return_copy_probe {
    std::shared_ptr<int64_t> copies = std::make_shared<int64_t>(0);
    int64_t v = 0;
    return_copy_probe() = default;
    return_copy_probe(const return_copy_probe& o) : copies(o.copies), v(o.v) { ++*copies; }
    return_copy_probe& operator=(const return_copy_probe&) = default;
    int64_t copies_made() const { return *copies; }
};

// By-value `return e;` yields a value that aliases nothing the callee could name (C++
// [stmt.return]): lvalue reads copy through the one store kernel, temporaries move, the
// callee's own local/parameter moves out of its dying slot (NRVO), shared_ptr handles and
// `-> T&` returns keep sharing. Every row runs on both backends and pins parity.
class compound_order_tests : public suite {
public:
    compound_order_tests() : suite("Compound Assignment Order") {}

    void forge_tests() override {
        auto run_both = [](const std::string& src) {
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                try { out[idx] = e->execute(src).to_string(); }
                catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                ++idx;
            }
            return std::make_pair(out[0], out[1]);
        };
        auto expect = [this, run_both](const std::string& src, const std::string& expected, const char* what) {
            auto [i_out, v_out] = run_both(src);
            check_eq(expected, i_out, std::string(what) + " (interpreter)");
            check_eq(expected, v_out, std::string(what) + " (vm)");
        };

		test("a_compound_target_cannot_change_the_rhs_value_already_read", [this]() {
			const std::pair<const char*, script_int> rows[] = { {"+=", 13}, {"-=", 7}, {"*=", 30}, {"/=", 3}, {"%=", 1} };
			for (bool vm : {false, true}) {
				for (bool proofs : {true, false}) {
					for (bool passes : {true, false}) {
						for (const auto& [op, result] : rows) {
							for (bool member : {false, true}) {
								auto e = jai::foundry::make_default_engine();
								e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
								e->type_proofs(proofs);
								e->ir_passes(passes);
								const std::string setup = member ? "class Box { int n = 10; } function f() -> int { array<Box> a = [Box()]; "
								                                 : "function f() -> int { array<int> a = [10]; ";
								const std::string field = member ? ".n" : "";
								const std::string src = setup + "int v = 3; a[(v = 8) - 8]" + field + " " + op
									+ " v; return a[0]" + field + " * 100 + v; } f();";
								check_eq(result * 100 + 8, e->execute(src).as_int(),
									std::string(member ? "member " : "element ") + op + (vm ? " VM" : " interpreter")
									+ (proofs ? "" : " proofs off") + (passes ? "" : " passes off"));
							}
						}
					}
				}
			}
		});

        test("compound_reads_its_target_after_a_mutating_right_side", [expect]() {
            // a by-reference argument boxes the target into a cell while the right side runs; a compound reads its
            // target after the right side (the right operand before the left, C++17's order), so the callee's write is
            // what it adds to, on both backends, whether the slot was boxed before or during the statement; the plain
            // binary keeps its left-to-right reads, so x + mutate(x) sees the value before the call
            const std::string defs = "function mutate(int& value) -> int { value = 20; return 2; }";
            expect(defs + " function f() -> int { int x = 3; return x + mutate(x); } f();", "5", "x + mutate(x)");
            expect(defs + " function f() -> int { int x = 3; x = x + mutate(x); return x; } f();", "5", "x = x + mutate(x)");
            expect(defs + " function f() -> int { int x = 3; x += mutate(x); return x; } f();", "22", "x += mutate(x)");
            expect(defs + " function f() -> int { int x = 3; x -= mutate(x); return x; } f();", "18", "x -= mutate(x)");
            expect(defs + " function f() -> int { int x = 3; int& alias = x; x += mutate(x); return x; } f();", "22", "a pre-boxed target");
            expect(defs + " function f() -> int { int x = 3; int rhs = mutate(x); x += rhs; return x; } f();", "22", "the right side hoisted");
            expect(defs + " function f() -> int { var v = 3; v += mutate(v); return v; } f();", "22", "a var target");
            expect(defs + " function f() -> int { int x = 3; int r = (x += mutate(x)); return r * 100 + x; } f();", "2222", "the compound's value");
        });
    }
};

class loop_frame_storage_tests : public suite {
public:
    loop_frame_storage_tests() : suite("Loop Frame Storage") {}

    void forge_tests() override {
        auto run_both = [](const std::string& src) {
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                try { out[idx] = e->execute(src).to_string(); }
                catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                ++idx;
            }
            return std::make_pair(out[0], out[1]);
        };
        auto expect = [this, run_both](const std::string& src, const std::string& expected, const char* what) {
            auto [i_out, v_out] = run_both(src);
            check_eq(expected, i_out, std::string(what) + " (interpreter)");
            check_eq(expected, v_out, std::string(what) + " (vm)");
        };

        test("loop_bodies_that_move_the_frame_keep_their_counter_and_variable", [expect]() {
            // a lambda's frame grows as the body declares (a slot past the reserved count), and its callees push frames of
            // other sizes: the counted loop's counter and bound, and the range loop's variable, are read and written through
            // pointers re-resolved after every body run; the loop runs every iteration with the live values on both backends
            const std::string defs =
                "int ITER = 0; array<int> DONE = [0, 1, 0, 0, 1, 0]; array<int> A = [1, 2, 3, 4, 5, 6];"
                " var light_band = function(int light, float dist) -> int { return light + math::itrunc(dist); };"
                " var col = function(int x, int y0, int y1, float ph, float rdx, float rdy, int light) -> int { return x + y0 + y1 + 30 + math::itrunc(ph + rdx + rdy) + light; };";
            expect(defs + " var seg = function(int n, var& done) { int acc = 0; float dist = 1.5; int light = 3; float fceil = 2.0;"
                          " for (int x = 0; x < n; ++x) { ITER = ITER + 1; if (done[x] != 0) { continue; } float rdx = x * 0.5; float rdy = rdx + 1.0; int band = light_band(light, dist); acc = col(x, band + 1, acc, fceil, rdx, rdy, light); } return acc; };"
                          " to_string(seg(6, DONE)) + \" \" + to_string(ITER);", "184 6", "a counted loop with a continue and two calls");
            expect(defs + " var walk = function() { int acc = 0; for (auto v : A) { ITER = ITER + 1; if (v == 2) { continue; } float rdx = v * 0.5; float rdy = rdx + 1.0; int band = light_band(v, 1.5); acc = col(v, band + 1, acc, 2.0, rdx, rdy, 3); } return acc; };"
                          " to_string(walk()) + \" \" + to_string(ITER);", "247 6", "a range loop with a continue and two calls");
            expect(defs + " var bound = function(int n) { int acc = 0; int step = 1; for (int x = 0; x < n; x += step) { float rdx = x * 0.5; int band = light_band(x, rdx); acc = col(x, band, acc, 2.0, rdx, 1.0, 3); } return acc; };"
                          " to_string(bound(5));", "208", "a variable bound and step");
            expect(defs + " var text = function() { int acc = 0; for (auto c : \"abc\") { float rdx = 0.5; int band = light_band(1, rdx); acc = col(acc, band, 1, 2.0, rdx, 1.0, 3) + cast<int>(c); } return acc; };"
                          " to_string(text());", "408", "a string range loop");
        });
    }
};

class return_value_semantics_tests : public suite {
public:
    return_value_semantics_tests() : suite("Return Value Semantics") {}

    void forge_tests() override {
        auto run_both = [](const std::string& src, bool with_probe = false) {
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                if (with_probe) {
                    dynamic_binder<return_copy_probe>(*e, "CopyProbe")
                        .constructor<>()
                        .property("v", &return_copy_probe::v)
                        .method("copies", &return_copy_probe::copies_made)
                        .build();
                }
                try { out[idx] = e->execute(src).to_string(); }
                catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                ++idx;
            }
            return std::make_pair(out[0], out[1]);
        };
        auto expect = [this, run_both](const std::string& src, const std::string& expected, const char* what, bool with_probe = false) {
            auto [i_out, v_out] = run_both(src, with_probe);
            check_eq(expected, i_out, std::string(what) + " (interpreter)");
            check_eq(expected, v_out, std::string(what) + " (vm)");
        };

        test("a_by_value_return_copies_globals_fields_elements_and_reference_results", [expect]() {
            const std::tuple<const char*, const char*, const char*> cases[] = {
                {"return_global_array_copies", R"(
                array<int> ints = [1, 2, 3];
                var f() { return ints; }
                var r = f(); r.push(4);
                array<var> anys = [1, 2, 3];
                var f2() { return anys; }
                var r2 = f2(); r2.push(4);
                array<int> f3() { return ints; }
                var r3 = f3(); r3.push(5);
                to_string(ints.size()) + "/" + to_string(r.size()) + "/" + to_string(anys.size()) + "/" +
                    to_string(r2.size()) + "/" + to_string(r3.size())
            )", "3/4/3/4/4"},
                {"return_global_map_copies", R"(
                map<string, int> m = {"a": 1};
                var h() { return m; }
                var mm = h(); mm["b"] = 2;
                m.size() * 10 + mm.size()
            )", "12"},
                {"return_global_object_copies", R"(
                class T { int v = 1; }
                T obj = T();
                var g() { return obj; }
                var o = g(); o.v = 99;
                T g2() { return obj; }
                var o2 = g2(); o2.v = 77;
                to_string(obj.v) + "/" + to_string(o.v) + "/" + to_string(o2.v)
            )", "1/99/77"},
                {"return_field_copies", R"(
                class H {
                    array<int> items = [1];
                    static array<int> shared_items = [1];
                    var get() { return items; }
                    var get2() { return this.items; }
                    static var gets() { return shared_items; }
                }
                H hd = H();
                var a = hd.get(); a.push(2);
                var b = hd.get2(); b.push(3);
                var c = H::gets(); c.push(4);
                to_string(hd.items.size()) + "/" + to_string(a.size()) + "/" + to_string(b.size()) + "/" +
                    to_string(H::shared_items.size()) + "/" + to_string(c.size())
            )", "1/2/2/1/2"},
                {"return_element_copies", R"(
                array<var> nested = [[1], [2]];
                var fe() { return nested[0]; }
                var re = fe(); re.push(3);
                map<string, array<int>> mm = {"k": [1]};
                var fm() { return mm["k"]; }
                var rm = fm(); rm.push(2);
                class T { int v = 1; }
                array<T> objs = [T()];
                var fo() { return objs[0]; }
                var ro = fo(); ro.v = 5;
                to_string(nested[0].size()) + "/" + to_string(re.size()) + "/" + to_string(mm["k"].size()) + "/" +
                    to_string(rm.size()) + "/" + to_string(objs[0].v) + "/" + to_string(ro.v)
            )", "1/2/1/2/1/5"},
                {"return_ref_call_from_value_function_copies", R"(
                int gx = 5;
                int& gi() { return gx; }
                var fi() { return gi(); }
                var vi = fi(); vi = 9;
                array<int> ints = [1];
                array<int>& ga() { return ints; }
                var fv() { return ga(); }
                var c = fv(); c.push(1);
                array<int> fvt() { return ga(); }
                var ct = fvt(); ct.push(1);
                to_string(gx) + "/" + to_string(vi) + "/" + to_string(ints.size()) + "/" + to_string(c.size()) + "/" + to_string(ct.size())
            )", "5/9/1/2/2"},
                {"direct_call_result_mutation_is_on_temporary", R"(
                array<int> ints = [1, 2];
                var f() { return ints; }
                f().push(100);
                class T { int v = 1; }
                T obj = T();
                var g() { return obj; }
                string stored = "stored";
                try { g().v = 9; } catch (e) { stored = e; }
                for (auto& x : f()) { x = 0; }
                to_string(ints.size()) + "/" + to_string(obj.v) + "/" + to_string(ints[0]) + "/" + stored
            )", "2/1/1/Cannot assign to a member of a temporary"},
                {"return_boxed_local_copies", R"(
                var keep = null;
                var f() {
                    array<int> a = [1];
                    keep = [&]() { a.push(0); return a.size(); };
                    return a;
                }
                var r = f(); r.push(2);
                int after = keep();
                to_string(r.size()) + "/" + to_string(after)
            )", "2/2"},
            };
            check_each(cases, [&](const auto& c) { expect(std::get<1>(c), std::get<2>(c), std::get<0>(c)); });
        });

        test("return_param_moves", [expect]() {
            expect(R"(
                var fp(array<int> p) { return p; }
                array<int> src = [1];
                var rp = fp(src); rp.push(2);
                CopyProbe id(CopyProbe p) { return p; }
                var q = id(CopyProbe());
                to_string(src.size()) + "/" + to_string(rp.size()) + "/" + to_string(q.copies())
            )", "1/2/0", "a prvalue argument is the parameter itself (C++17) and a parameter returns by move", true);
        });
        test("return_local_is_nrvo", [expect]() {
            expect(R"(
                int dtors = 0;
                class T { int v = 0; ~T() { dtors += 1; } }
                var make() { T t = T(); t.v = 5; return t; }
                var a = make();
                int after_make = dtors;
                var make2() { T t = T(); T u = T(); u.v = 7; return u; }
                var b = make2();
                int after_make2 = dtors;
                to_string(after_make) + "/" + to_string(a.v) + "/" + to_string(after_make2) + "/" + to_string(b.v)
            )", "0/5/1/7", "a returned local moves out of its dying slot: one object, no destructor");
            expect(R"(
                nocopy class L { int total = 1; }
                var ml() { L l = L(); l.total = 3; return l; }
                var got = ml();
                void bump(L& l) { l.total += 1; }
                var ml2() { L l = L(); bump(l); return l; }
                to_string(got.total) + "/" + to_string(ml2().total)
            )", "3/2", "a nocopy local returns by move, even after a finished by-reference bind");
        });
        test("return_nocopy_global_or_field_throws", [this, run_both]() {
            const std::string text = "ERROR: Cannot copy an instance of nocopy class 'L': hold it by reference (auto&/var&) or share it (shared_ptr)";
            {
                auto [i_out, v_out] = run_both("nocopy class L { int total = 1; }\nL g = L();\nvar fg() { return g; }\nfg();\n");
                check_eq(text, i_out.substr(0, text.size()), "nocopy global returned by value raises the kernel text (interpreter)");
                check_eq(i_out, v_out, "nocopy global return: what() parity");
            }
            {
                auto [i_out, v_out] = run_both("nocopy class L { int total = 1; }\nclass Box { L held = L(); var get() { return held; } }\nBox b = Box();\nb.get();\n");
                check_eq(text, i_out.substr(0, text.size()), "nocopy field returned by value raises the kernel text (interpreter)");
                check_eq(i_out, v_out, "nocopy field return: what() parity");
            }
            for (bool use_vm : {false, true}) {   // the engine stays usable after the host-visible throw
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                check_throws([&]() { e->execute("nocopy class L { int total = 1; }\nL g = L();\nvar fg() { return g; }\nfg();\n"); });
                check_eq((int64_t)3, e->execute("1 + 2").as_int());
            }
        });
        test("return_temporary_not_double_copied", [expect]() {
            expect(R"(
                var p = CopyProbe();
                var f() { return p; }
                var q = f();
                int c1 = q.copies();
                var g() { return CopyProbe(); }
                var h() { return g(); }
                int c2 = h().copies();
                var inner() { return p; }
                var outer() { return inner(); }
                var o = outer();
                int c3 = o.copies();
                var outer2() { CopyProbe t = inner(); return t; }
                var o2 = outer2();
                int c4 = o2.copies();
                o.v = 3;
                to_string(c1) + "/" + to_string(c2) + "/" + to_string(c3 - c1) + "/" + to_string(c4 - c3) + "/" + to_string(p.v)
            )", "1/0/1/1/0", "one copy per lvalue return; temporaries, nested returns and decl-then-return add none", true);
        });
        test("handle_this_and_reference_returns_share_their_target", [expect]() {
            const std::tuple<const char*, const char*, const char*> cases[] = {
                {"return_shared_ptr_shares", R"(
                class T { int v = 1; }
                shared_ptr<T> sp = new T();
                var gsp() { return sp; }
                var q = gsp(); q.v = 55;
                class Box { shared_ptr<T> p = new T(); var getp() { return p; } }
                Box bx = Box();
                var q2 = bx.getp(); q2.v = 66;
                to_string(sp.v) + "/" + to_string(bx.p.v)
            )", "55/66"},
                {"ref_return_still_aliases", R"(
                array<int> ints = [1];
                array<int>& gr() { return ints; }
                var& ra = gr(); ra.push(9);
                class T { int v = 1; }
                T obj = T();
                T& gt() { return obj; }
                gt().v = 7;
                var x = gr(); x.push(1);
                to_string(ints.size()) + "/" + to_string(obj.v) + "/" + to_string(x.size())
            )", "2/7/3"},
                {"this_return_shares_receiver", R"(
                class Chain { int n = 0; var inc() { n += 1; return this; } }
                Chain ch = Chain();
                ch.inc().inc();
                ch.n
            )", "2"},
                {"lvalue_chains_return_by_reference", R"(
                class Leaf { int hp = 1; }
                class Hp { int hp = 1; Leaf child; }
                class Grid { array<int> cells = [0, 0, 0]; int& operator[](int i) { return cells[i]; } }
                map<string, int> m = {"a": 1};
                map<string, array<int>> rows = {"q": [1, 2]};
                map<string, Hp> units = {"u": Hp()};
                array<Hp> hs = [Hp(), Hp()];
                Grid g;
                string key = "a";
                int& entry(string k) { return m[k]; }
                int& global_key() { return m[key]; }
                int& cell(string k, int i) { return rows[k][i]; }
                int& unit_hp(string k) { return units[k].hp; }
                int& literal_hp() { return units["u"].hp; }
                int& computed_hp(int i) { return hs[i + 1].hp; }
                int& nested_hp(int i) { return hs[i + 1].child.hp; }
                int& computed_key(string k) { return rows[k + ""][0]; }
                int& grid_at(int i) { return g[i]; }
                entry("a") += 5;
                global_key() *= 2;
                cell("q", 1) += 10;
                unit_hp("u") += 20;
                literal_hp() += 300;
                computed_hp(0) += 4;
                nested_hp(0) = 9;
                computed_key("q") -= 1;
                grid_at(2) = 7;
                int& d1 = units[key == "a" ? "u" : key].hp;
                d1 += 1000;
                auto& d2 = hs[key.size()].child;
                d2.hp += 1;
                to_json(m) + " " + to_json(rows) + " " + to_string(units["u"].hp) + " " + to_string(hs[1].hp) + " " +
                    to_string(hs[1].child.hp) + " " + to_json(g.cells)
            )", R"({"a":12} {"q":[0,12]} 1321 5 10 [0,0,7])"},
            };
            check_each(cases, [&](const auto& c) { expect(std::get<1>(c), std::get<2>(c), std::get<0>(c)); });
        });

        test("return_inside_try_same_as_outside", [expect]() {
            expect(R"(
                int dtors = 0;
                class T { int v = 0; ~T() { dtors += 1; } }
                var make() { try { T t = T(); t.v = 5; return t; } catch (e) { return null; } }
                var a = make();
                int d1 = dtors;
                array<int> ints = [1];
                var f() { try { return ints; } catch (e) { return null; } }
                var r = f(); r.push(2);
                class T2 { int v = 1; }
                T2 obj = T2();
                T2 other = T2();
                var g() { try { return obj.v > 0 ? obj : other; } catch (e) { return null; } }
                var o = g(); o.v = 9;
                class H { array<int> items = [1]; var get() { try { return items; } catch (e) { return null; } } }
                H hd = H();
                var hi = hd.get(); hi.push(2);
                var five = 5;
                array<int> h() { try { return five; } catch (e) { return [7]; } }
                to_string(d1) + "/" + to_string(a.v) + "/" + to_string(ints.size()) + "/" + to_string(r.size()) + "/" +
                    to_string(obj.v) + "/" + to_string(hd.items.size()) + "/" + to_string(h().size())
            )", "0/5/1/2/1/1/1", "in-region returns move locals and copy lvalues exactly like clean ones");
        });
        test("lambda_captures_return", [expect]() {
            expect(R"(
                var mk() {
                    array<int> cap = [1];
                    var by_val = [=]() { return cap; };
                    var by_ref = [&]() { return cap; };
                    var a = by_val(); a.push(2);
                    var b = by_val();
                    var c = by_ref(); c.push(3);
                    return to_string(a.size()) + "/" + to_string(b.size()) + "/" + to_string(cap.size()) + "/" + to_string(c.size());
                }
                mk()
            )", "2/1/1/2", "[=] returns copy the captured copy, [&] returns copy the shared cell");
        });
        test("hot_reload_after_returned_copy", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute("class T { int v = 1; }\nT obj = T();\nvar g() { return obj; }\nvar c = g();\nc.v = 5;\n");
                e->execute("class T { int v = 1; int w = 2; }");
                check_eq((int64_t)9, e->execute("c.w + obj.w + c.v").as_int(), use_vm ? "vm: both migrate" : "interp: both migrate");
                check_eq((int64_t)1, e->execute("obj.v").as_int(), use_vm ? "vm: original untouched" : "interp: original untouched");
            }
        });
        test("top_level_return_unchanged", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                auto r = e->execute("array<var> a = [1]; return a;");
                e->execute("a.push(2);");
                check_eq((size_t)2, r.as_array().size(), use_vm ? "vm: top-level return hands the host the global's handle" : "interp: top-level return hands the host the global's handle");
            }
        });
        test("a_reference_return_of_no_storage_names_the_return", [expect]() {
            expect(R"(
                map<string, int> m = {"a": 1};
                int one() { return 1; }
                int& entry(string k) { return m[k]; }
                int& call_result() { return one(); }
                var seen = "";
                try { entry("missing") = 3; } catch (e) { seen = seen + e + "|"; }
                try { call_result() = 3; } catch (e) { seen = seen + e + "|"; }
                seen + to_json(m)
            )", "Key \"missing\" not found in map|"
                "Function with reference return type must return an lvalue reference|{\"a\":1}", "a missing entry raises its read, a by-value call is no storage");
        });
    }
};

// A by-value argument takes its value when the argument expression is evaluated (left to right, after the callee), so a
// later argument, or a conversion the binding runs, that writes what it read does not change what the callee receives; a
// reference parameter binds the lvalue and sees the write. Every row runs on both backends with proofs on and off.
class call_argument_capture_tests : public suite {
public:
    call_argument_capture_tests() : suite("Call Argument Capture") {}

    void forge_tests() override {
        auto run = [](const std::string& src, bool use_vm, bool proofs) {
            auto e = jai::foundry::make_default_engine();
            e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
            e->type_proofs(proofs);
            e->execution_budget(0);
            jai::stdlib::register_all(e);
            jai::engine* raw = e.get();
            e->add_variadic_function("host_seen", [raw](const std::vector<script_value>& args) -> checked_result<script_value> {
                std::string s;
                for (const auto& a : args) { if (!s.empty()) { s += ","; } s += a.deref().to_string(); }
                return script_value(s, raw);
            });
            e->add_function("host_fill", [](bound_array<int64_t>& a, int64_t v) -> int64_t { a[0] = v; return static_cast<int64_t>(a.size()); });
            try { return e->execute(src).to_string(); }
            catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
        };
        auto expect = [this, run](const std::string& src, const std::string& expected, const char* what) {
            for (bool proofs : {true, false}) {
                for (bool use_vm : {false, true}) {
                    check_eq(expected, run(src, use_vm, proofs), std::string(what) + (use_vm ? " (vm" : " (interpreter") + (proofs ? ")" : ", proofs off)"));
                }
            }
        };
        const std::string add2 = "function add2(a, b) { return a + b; } function sz(a, b) { return a.size() + b; } ";

        test("an_element_read_takes_its_value_before_a_later_argument_runs", [expect, add2]() {
            const std::string ga = add2 + "var GA = [1, 2, 3]; function f() { GA[1] = 100; return 1; } ";
            expect(ga + "to_string(add2(GA[1], f()))", "3", "a plain call");
            expect(ga + "to_string(add2(GA[1], add2(0, f())))", "3", "a nested call as the later argument");
            expect(ga + "to_string(add2(GA[1], GA[1] = 7))", "9", "an assignment as the later argument");
            expect(ga + "to_string(add2(true ? GA[1] : GA[2], f()))", "3", "a ternary over elements");
            expect(ga + "class Q { function addn(a, b) { return a + b; } } var q = Q(); to_string(q.addn(GA[1], f()))", "3", "a method call");
            expect(ga + "class C2 { var s = 0; C2(a, b) { s = a + b; } } to_string(C2(GA[1], f()).s)", "3", "a constructor call");
            expect(ga + "class C3 { var s = 0; C3(a, b) { s = a + b; } } var p = new C3(GA[1], f()); to_string(p.s)", "3", "new");
            expect(ga + "var lam = [](a, b) { return a + b; }; to_string(lam(GA[1], f()))", "3", "a lambda");
            expect(ga + "function dflt(a, b = f()) { return a + b; } to_string(dflt(GA[1]))", "3", "a default argument");
            expect(ga + "to_string(GA[1]) + \" \" + to_string(GA[1] + f())", "2 3", "a binary operand (unchanged)");
            expect(add2 + "var GS = [\"abc\", \"d\"]; function f() { GS[0] = \"abcdefg\"; return 0; } to_string(sz(GS[0], f()))", "3", "a string element");
            expect(add2 + "var GR = [1, 2, 3]; int& pick() { return GR[1]; } function f() { GR[1] = 100; return 1; } to_string(add2(pick(), f()))", "3", "a reference-returning call");
        });

        test("a_heavy_argument_binds_the_value_it_had_when_evaluated", [expect, add2]() {
            expect(add2 + "var arr = [1, 2, 3]; function grow() { arr.push(4); return 0; } to_string(sz(arr, grow()))", "3", "a global array");
            expect(add2 + "var mp = {\"a\": 1}; function grow() { mp[\"b\"] = 2; return 0; } to_string(sz(mp, grow()))", "1", "a global map");
            expect(add2 + "var GH = [[1, 2], [3, 4]]; function grow() { GH[0].push(9); return 0; } to_string(sz(GH[0], grow()))", "2", "an element array");
            expect(add2 + "class P { var list = [1, 2]; } var obj = P(); function grow() { obj.list.push(9); return 0; } to_string(sz(obj.list, grow()))", "2", "a member array");
            expect(add2 + "class R { var x = 5; } var r = R(); function heavy(o, b) { return o.x + b; } function poke() { r.x = 55; return 0; } to_string(heavy(r, poke()))", "5", "a class instance");
            expect(add2 + "var arr = [1, 2, 3]; function reset() { arr = [9, 9, 9, 9]; return 0; } to_string(sz(arr, reset()))", "3", "a variable rebound by the later argument");
            expect(add2 + "function outer() { var la = [1, 2, 3]; var push = [&]() { la.push(4); return 0; }; return sz(la, push()); } to_string(outer())", "3", "a local captured by reference");
            expect(add2 + "var target = [1, 2, 3]; function grow() { target.push(4); return 0; } function via() { var& r = target; return sz(r, grow()); } to_string(via())", "3", "through a reference");
            expect("array<int> arr = [1, 2, 3]; function grow() -> int { arr.push(4); return 0; } function rd(array<int> a, int n) -> int { return a.size() + n; } to_string(rd(arr, grow()))", "3", "a read-only parameter");
            expect("class Box { var n = 1; function bump() { n = 10; return 0; } function show(b, k) { return b.n + k; } function go() { return show(this, bump()); } } var bx = Box(); to_string(bx.go())", "1", "this by value");
        });

        test("scalar_reads_already_taken_at_evaluation_stay_so", [expect, add2]() {
            expect(add2 + "class P { var x = 5; } var obj = P(); function g() { obj.x = 50; return 1; } to_string(add2(obj.x, g()))", "6", "a member");
            expect(add2 + "var gx = 7; function h() { gx = 70; return 1; } to_string(add2(gx, h()))", "8", "a global");
            expect(add2 + "function outer() { var lx = 3; var bump = [&]() { lx = 30; return 1; }; return add2(lx, bump()); } to_string(outer())", "4", "a captured local");
            expect(add2 + "var target = [1, 2, 3]; function via() { var& r = target[0]; var k = [&]() { target[0] = 11; return 1; }; return add2(r, k()); } to_string(via())", "2", "a reference alias");
        });

        test("host_callees_receive_the_value_at_evaluation", [expect]() {
            const std::string ga = "var GA = [1, 2, 3]; function f() { GA[1] = 100; return 1; } ";
            expect(ga + "host_seen(GA[1], f())", "2,1", "a variadic host function");
            expect(ga + "to_string(math::max(GA[1], f()))", "2", "a numeric intrinsic");
            expect(ga + "to_string(math::max(GA[1], f()))", "2", "a math intrinsic");
            expect("var s = \"abcdef\"; var GI = [1, 2]; function f() { GI[0] = 3; return 2; } s.substr(GI[0], f())", "bc", "a string builtin");
        });

        test("math_intrinsic_operands_survive_a_later_call", [expect]() {
            // a later operand's call runs statements (an expression statement, a block) that must not disturb the operands
            // already evaluated
            expect("var GA = [1, 2, 3]; function f() { GA[1] = 100; return 1; } to_string(math::max(5, f()))", "5", "an expression statement in the callee");
            expect("function k() { if (true) { var t = 1; t = 2; } return 1; } to_string(math::max(5, k()))", "5", "a block in the callee");
            expect("function j() { var s = 3; s = 4; return 1; } to_string(math::clamp(7, j(), 9))", "7", "three operands");
        });

        test("reference_parameters_still_alias_the_lvalue", [expect]() {
            const std::string gf = "var GF = [1, 2, 3]; function ff() { GF[1] = 600; return 1; } function refp(int& a, b) { return a + b; } ";
            expect(gf + "to_string(refp(GF[1], ff()))", "601", "an element lvalue");
            expect(gf + "var k = 0; to_string(refp(GF[k + 1], ff()))", "601", "a computed element lvalue");
            expect("var arr = [1, 2, 3]; function grow() { arr.push(4); return 0; } function refarr(var& a, n) { return a.size() + n; } to_string(refarr(arr, grow()))", "4", "a var& parameter over a heavy value");
            expect("array<int> ha = [1, 2]; function grow() -> int { ha.push(3); return 7; } to_string(host_fill(ha, grow())) + \" \" + to_string(ha[0])", "3 7", "a host bound_array& parameter");
        });

        test("a_constructor_takes_its_arguments_before_its_construction_runs", [expect]() {
            const std::string ga = "var GA = [1, 2, 3]; function f() { GA[1] = 100; return 1; } ";
            expect(ga + "class Base { var s = 0; Base(a, b) { s = a + b; } } class Kid : Base { Kid() : super(GA[1], f()) {} } to_string(Kid().s)", "3", "a super initializer");
            expect(ga + "class Two { var s = 0; Two(a, b) { s = a + b; } Two() : this(GA[1], f()) {} } to_string(Two().s)", "3", "a this initializer");
            expect("var arr = [1, 2, 3]; function grow() { arr.push(4); return 0; } class Sz { var n = 0; Sz(a, b) { n = a.size() + b; } Sz(int k) : this(arr, grow()) {} } to_string(Sz(0).n)", "3", "a heavy this initializer");
            expect(ga + "class W { var t = f(); var v = 0; W(a) { v = a; } } to_string(W(GA[1]).v)", "2", "a field initializer that writes the argument");
            expect(ga + "class A { var v = 0; A(a) { v = a; } } class B : A { var t = f(); B() : super(GA[1]) {} } to_string(B().v)", "2", "a super argument ahead of a field initializer");
            expect(ga + "class N { var t = f(); var v = 0; N(a) { v = a; } } var n = new N(GA[1]); to_string(n.v)", "2", "new");
        });

        test("a_coroutine_takes_its_arguments_at_the_call", [expect]() {
            const std::string ga = "var GA = [1, 2, 3]; ";
            expect(ga + "coroutine function co(a) { yield a; } var h = co(GA[1]); GA[1] = 100; to_string(h.next())", "2", "an element");
            expect("var arr = [1, 2, 3]; coroutine function co(a) { yield a.size(); } var h = co(arr); arr.push(4); to_string(h.next())", "3", "a heavy value");
            expect(ga + "class K { coroutine function gen(a) { yield a; } } var k = K(); var h = k.gen(GA[1]); GA[1] = 100; to_string(h.next())", "2", "a method coroutine");
        });

        test("a_grafted_call_keeps_the_arguments_ahead_of_it", [expect]() {
            // a leaf the VM grafts into its caller: a name ahead of it that the outer call copies or binds by reference is the name itself,
            // whichever callee the guard finds at run time
            const std::string defs = "[[warn_mode]] var ARR = [1, 2, 3]; function k(int i) -> int { return i & 1; } function sz(a, int b) -> int { return a.size() + b; }"
                                     " function run() -> int { int acc = 0; for (int i = 0; i < 2; ++i) { acc = acc + sz(ARR, k(i)); } return acc; } ";
            expect(defs + "var before = run(); k = [](int i) -> int { ARR.push(9); return 0; }; to_string(before) + \" \" + to_string(run())", "7 7", "the fallback after the callee is rebound");
            expect("var PX = [1, 2, 3]; function g(int i) -> int { return i & 1; } function rebind(var& a, int b) -> int { a = [9]; return b; }"
                   " function run() -> int { int acc = 0; for (int i = 0; i < 2; ++i) { acc = acc + rebind(PX, g(i)); } return PX.size(); } to_string(run())", "1", "a reference parameter ahead of the call");
            expect("var GX = 3; function g(int i) -> int { return i & 1; } function add2(int a, int b) -> int { return a + b; }"
                   " function run() -> int { int acc = 0; for (int i = 0; i < 2; ++i) { acc = acc + add2(GX, g(i)); } return acc; } to_string(run())", "7", "a scalar parameter ahead of the call");
        });

		test("a_graft_cannot_run_before_an_earlier_aggregate_argument_is_captured", [expect]() {
			const std::string array =
				"function read(array<int> a, int ignored) -> int { return a[0]; } "
				"function change(array<int>& a) -> int { a[0] = 9; return 0; } ";
			expect(array + "function run() -> int { array<int> a = [1]; return read(a, change(a)); } run();", "1",
				"a private array copied before an inlined element write");
			expect(array + "class Reader { int got = 0; Reader(array<int> a, int ignored) { got = a[0]; } } "
				"function run() -> int { array<int> a = [1]; Reader r = Reader(a, change(a)); return r.got; } run();", "1",
				"a constructor argument copied before an inlined element write");
			expect("function read(map<int, int> a, int ignored) -> int { return a[0]; } "
				"function change(map<int, int>& a) -> int { a[0] = 9; return 0; } "
				"function run() -> int { map<int, int> a = {0: 1}; return read(a, change(a)); } run();", "1",
				"a private map copied before an inlined entry write");
			expect("class Box { int n = 1; } function read(Box a, int ignored) -> int { return a.n; } "
				"function change(Box& a) -> int { a.n = 9; return 0; } "
				"function run() -> int { Box a; return read(a, change(a)); } run();", "1",
				"a private instance copied before an inlined field write");
			expect("function read(array<int>& a, int ignored) -> int { return a[0]; } "
				"function change(array<int>& a) -> int { a[0] = 9; return 0; } "
				"function run() -> int { array<int> a = [1]; return read(a, change(a)); } run();", "9",
				"a reference argument still sees the later write");
			expect(array + "function run() -> int { array<int> a = [1]; int total = 0; "
				"for (int i = 0; i < 3; ++i) { a[0] = i + 1; total += read(a, change(a)); } return total; } run();", "6",
				"aggregate snapshots inside a loop");
		});

        test("a_binding_conversion_runs_after_every_argument_is_taken", [expect]() {
            expect("var GK = [1, 2, 3]; class Conv { var v = 0; Conv(int x) { v = x; GK[1] = 900; } } function takes(Conv c, b) { return c.v + b; } to_string(takes(5, GK[1]))", "7", "a converting constructor");
        });

		test("staged_argument_prefixes_survive_nested_calls_and_later_grafts", [expect]() {
			const std::string defs =
				"function read(array<int> a, int n) -> int { return a[0] + n; } "
				"function change(array<int>& a) -> int { a[0] = 9; return 0; } ";
			expect(defs + "function run() -> int { array<int> a = [1]; return read(a, read(a, change(a))); } run();",
				"2", "nested pending call boundaries retain both earlier snapshots");
			expect("function read(array<int> a, int x, int y) -> int { return a[0] + x + y; } "
				"function first(array<int>& a) -> int { a[0] = 9; return 1; } "
				"function second(array<int>& a) -> int { a[0] = 10; return 2; } "
				"function run() -> int { array<int> a = [1]; int got = read(a, first(a), second(a)); return got * 100 + a[0]; } run();",
				"410", "a second graft extends the existing argument prefix without reevaluation");
			expect(defs + "function run() -> int { array<int> a = [1]; var f = read; return f(a, change(a)); } run();",
				"1", "a callable local is resolved before its staged arguments");
			expect(defs + "function run() -> int { array<int> a = [1]; var f = 5; try { f(a, change(a)); } catch (e) {} return a[0]; } run();",
				"1", "an invalid callee fails before a writing graft");
		});

		test("a_failed_staged_call_releases_its_arguments_before_catch", [expect]() {
			const std::string defs =
				"int destroyed = 0; class Box { ~Box() { destroyed += 1; } } "
				"function read(array<shared_ptr<Box>> a, int ignored) -> int { return 0; } "
				"function fail(array<shared_ptr<Box>>& a, int zero) -> int { a[0] = null; return 1 / zero; } ";
			expect(defs + "function run() -> int { array<shared_ptr<Box>> a = [cast<shared_ptr<Box>>(Box())]; "
				"try { read(a, fail(a, 0)); } catch (e) {} return destroyed; } run();", "1",
				"the captured handle is released before catch, not at the caller's return");
			expect(defs + "function run() -> int { int total = 0; for (int i = 0; i < 3; ++i) { "
				"array<shared_ptr<Box>> a = [cast<shared_ptr<Box>>(Box())]; "
				"try { read(a, fail(a, 0)); } catch (e) { total += destroyed; } } return total; } run();", "6",
				"each failed iteration releases its own captured arguments");
			expect("function nested(int n) { try { if (n > 0) { nested(n - 1); } else { throw \"inside destructor\"; } } catch (e) {} } "
				"int destroyed = 0; class Box { ~Box() { nested(16); destroyed += 1; } } "
				"function read(array<shared_ptr<Box>> a, int ignored) -> int { return 0; } "
				"function fail(array<shared_ptr<Box>>& a, int zero) -> int { a[0] = null; return 1 / zero; } "
				"function run() -> int { array<shared_ptr<Box>> a = [cast<shared_ptr<Box>>(Box())]; "
				"try { read(a, fail(a, 0)); } catch (e) {} return destroyed; } run();", "1",
				"argument destruction may reenter and grow the handler stack");
			expect(defs + "function run() -> int { array<shared_ptr<Box>> a = [cast<shared_ptr<Box>>(Box())]; "
				"try { read(a, fail(a, 0)); } catch (e) {} return destroyed; } "
				"int first = run(); fail = [](var& a, int zero) -> int { a[0] = null; throw \"rebound\"; return 0; }; "
				"int second = run(); first * 10 + second;", "12",
				"the rebound guard fallback releases the same captured arguments");
		});

		test("aggregate_argument_staging_keeps_the_writing_call_inlined", [this]() {
			auto e = jai::foundry::make_default_engine();
			e->set_backend(jai::backend_type::vm);
			std::string listing;
			e->disassembly_sink([&](const std::string& text, const std::string&) { listing += text; });
			const auto result = e->execute(
				"function read(array<int> a, int ignored) -> int { return a[0]; } "
				"function change(array<int>& a) -> int { a[0] = 9; return 0; } "
				"function run() -> int { array<int> a = [1]; return read(a, change(a)); } run();");
			check_eq(script_int(1), result.as_int());
			check_true(listing.find("-- inlined change") != std::string::npos, "the writing call must remain inlined");
			check_true(listing.find("CAPTURE_ARG") != std::string::npos, "the earlier aggregate is captured through the normal call boundary");
		});

		test("mixed_overloads_keep_both_the_snapshot_and_the_original_lvalue", [expect]() {
			const std::string defs =
				"class Reader { int read(array<int> a, int tag) { return a[0]; } "
				"int read(array<int>& a, string tag) { int old = a[0]; a[0] = 42; return old; } } "
				"function change(array<int>& a) -> int { a[0] = 9; return 0; } ";
			expect(defs + "Reader r; array<int> a = [1]; r.read(a, change(a));", "1", "selected value overload");
			expect(defs + "Reader r; var f = reflect::bind(r, \"read\"); array<int> a = [1]; f(a, change(a));", "1", "bound method value overload");
			expect(defs + "Reader r; array<int> a = [1]; r.read(a, \"ref\"); a[0];", "42", "selected reference overload aliases");
			expect(defs + "function tag(array<int>& a) { a[0] = 9; return \"ref\"; } "
				"Reader r; array<int> a = [1]; int seen = r.read(a, tag(a)); seen * 100 + a[0];", "942",
				"dynamic dispatch chooses the original reference after later evaluation");
			expect("class Reader { int read(array<int> a, int tag) { return a[0]; } int read(array<int>& a) { return -1; } } "
				"function change(array<int>& a) -> int { a[0] = 9; return 0; } Reader r; array<int> a = [1]; r.read(a, change(a));",
				"1", "an inapplicable arity cannot suppress capture");
			expect("class Reader { int read(int a, int tag) { return a; } int read(int& a, string tag) { a = 42; return a; } } "
				"function change(array<int>& a) -> int { a[0] = 9; return 0; } Reader r; array<int> a = [1]; r.read(a[0], change(a));",
				"1", "element value snapshot under mixed parameter modes");
			expect("class Reader { int read(int a, int tag) { return a; } int read(int& a, string tag) { a = 42; return a; } } "
				"function tag(array<int>& a) { a[0] = 9; return \"ref\"; } Reader r; array<int> a = [1]; r.read(a[0], tag(a)); a[0];",
				"42", "element reference still names original storage");
		});

		test("constructors_select_captured_values_or_original_references", [expect]() {
			const std::string defs =
				"class Reader { int got = 0; Reader(array<int> a, int tag) { got = a[0]; } "
				"Reader(array<int>& a, string tag) { got = a[0]; a[0] = 42; } } "
				"function change(array<int>& a) -> int { a[0] = 9; return 0; } ";
			expect(defs + "function run() -> int { array<int> a = [1]; Reader r = Reader(a, change(a)); return r.got; } run();", "1",
				"constructor value overload");
			expect(defs + "array<int> a = [1]; var r = new Reader(a, change(a)); r.got;", "1", "new value overload");
			expect("class Reader { int got = 0; Reader(array<int>& a, string tag) { got = -1; } "
				"Reader(array<int> a, int tag) { got = a[0]; } } "
				"function change(var& a) -> int { a[0] = 9; return 0; } "
				"var a = [1]; Reader r = Reader(a, change(a)); r.got;", "1",
				"constructor selection sees the captured dynamic value, not its transport reference");

			expect(defs + "function tag(array<int>& a) { a[0] = 9; return \"ref\"; } "
				"function run() -> int { array<int> a = [1]; Reader r = Reader(a, tag(a)); return r.got * 100 + a[0]; } run();", "942",
				"constructor keeps caller metadata for a reference parameter");
			expect(defs + "function tag(array<int>& a) { a[0] = 9; return \"ref\"; } "
				"function run() -> int { array<int> a = [1]; var r = new Reader(a, tag(a)); return r.got * 100 + a[0]; } run();", "942",
				"new keeps caller metadata for a reference parameter");
		});

		test("shared_handles_freeze_identity_while_reference_parameters_keep_the_slot", [expect]() {
			expect("class Box { int n = 1; } function f(shared_ptr<Box> p) -> int { p.n = 5; p = cast<shared_ptr<Box>>(Box()); p.n = 7; return p.n; } "
				"shared_ptr<Box> p = cast<shared_ptr<Box>>(Box()); int result = f(p); result * 10 + p.n;", "75",
				"a by-value handle shares its pointee but rebinding stays local");
			expect("class Box { int n = 1; } function f(shared_ptr<Box>& p) -> int { p = cast<shared_ptr<Box>>(Box()); p.n = 7; return p.n; } "
				"shared_ptr<Box> p = cast<shared_ptr<Box>>(Box()); int result = f(p); result * 10 + p.n;", "77",
				"a reference to a handle aliases the caller's pointer slot");
			const std::string defs =
				"class Box { int n = 1; } class Reader { "
				"int read(shared_ptr<Box> p, int tag) { return p.n; } "
				"int read(shared_ptr<Box>& p, string tag) { p.n = 42; return p.n; } } ";
			expect(defs + "function change(shared_ptr<Box>& p) -> int { p = cast<shared_ptr<Box>>(Box()); p.n = 9; return 0; } "
				"Reader r; shared_ptr<Box> p = cast<shared_ptr<Box>>(Box()); int seen = r.read(p, change(p)); seen * 100 + p.n;", "109",
				"a mixed-mode by-value handle keeps the identity evaluated before a rebind");
			expect(defs + "function change(shared_ptr<Box>& p) { p = cast<shared_ptr<Box>>(Box()); p.n = 9; return \"ref\"; } "
				"Reader r; shared_ptr<Box> p = cast<shared_ptr<Box>>(Box()); int seen = r.read(p, change(p)); seen * 100 + p.n;", "4242",
				"a mixed-mode reference keeps the slot through that rebind");
		});

		test("static_script_methods_are_not_opaque_host_calls", [expect]() {
			const std::string defs =
				"class Reader { static int read(array<int> a, int tag) { return a[0]; } } "
				"function change(array<int>& a) -> int { a[0] = 9; return 0; } ";
			expect(defs + "array<int> a = [1]; Reader::read(a, change(a));", "1", "static value argument");
			expect(defs + "var f = Reader::read; array<int> a = [1]; f(a, change(a));", "1", "aliased static value argument");
			expect("class Reader { static int read(array<int> a, int tag) { return a[0]; } "
				"static int read(array<int>& a, string tag) { a[0] = 42; return a[0]; } } "
				"function change(array<int>& a) -> int { a[0] = 9; return 0; } array<int> a = [1]; Reader::read(a, change(a));",
				"1", "mixed static overload value argument");
			expect("class Reader { static int read(array<int> a, int tag) { return a[0]; } "
				"static int read(array<int>& a, string tag) { a[0] = 42; return a[0]; } } "
				"array<int> a = [1]; Reader::read(a, \"ref\"); a[0];", "42", "static reference control");
		});

        test("errors_keep_their_order_and_text", [expect]() {
            expect("nocopy class Ledger { int total = 1; } auto ledger = Ledger(); var ran = 0; function side() { ran = 1; return 0; } function take(Ledger l, n) { return n; }"
                   " var msg = \"\"; try { take(ledger, side()); } catch (e) { msg = e; } to_string(ran) + (msg.contains(\"nocopy class 'Ledger'\") ? \" nocopy\" : \" \" + msg)", "1 nocopy", "a nocopy argument");
            expect("var GA = [1, 2, 3]; function f() { GA[1] = 100; return 1; } var notfn = 5; var msg = \"\"; try { notfn(GA[1], f()); } catch (e) { msg = \"caught\"; } msg + \" \" + to_string(GA[1])", "caught 2", "a callee that is not callable");
        });
    }
};

} // namespace jai::foundry::tests

using constructor_semantics_tests = jai::foundry::tests::constructor_semantics_tests;
FOUNDRY_REGISTER(constructor_semantics_tests)
using call_argument_capture_tests = jai::foundry::tests::call_argument_capture_tests;
FOUNDRY_REGISTER(call_argument_capture_tests)
using char_promotion_tests = jai::foundry::tests::char_promotion_tests;
FOUNDRY_REGISTER(char_promotion_tests)
using review_regression_tests = jai::foundry::tests::review_regression_tests;
FOUNDRY_REGISTER(review_regression_tests)
using typed_store_proof_tests = jai::foundry::tests::typed_store_proof_tests;
FOUNDRY_REGISTER(typed_store_proof_tests)
using math_intrinsics_tests = jai::foundry::tests::math_intrinsics_tests;
FOUNDRY_REGISTER(math_intrinsics_tests)
using element_read_elision_tests = jai::foundry::tests::element_read_elision_tests;
FOUNDRY_REGISTER(element_read_elision_tests)
using ref_capture_semantics_tests = jai::foundry::tests::ref_capture_semantics_tests;
FOUNDRY_REGISTER(ref_capture_semantics_tests)
using flat_value_class_tests = jai::foundry::tests::flat_value_class_tests;
FOUNDRY_REGISTER(flat_value_class_tests)
using return_value_semantics_tests = jai::foundry::tests::return_value_semantics_tests;
FOUNDRY_REGISTER(return_value_semantics_tests)
using compound_order_tests = jai::foundry::tests::compound_order_tests;
FOUNDRY_REGISTER(compound_order_tests)
using loop_frame_storage_tests = jai::foundry::tests::loop_frame_storage_tests;
FOUNDRY_REGISTER(loop_frame_storage_tests)

namespace jai::foundry::tests {

struct keyword_member_probe {
    int64_t map(int64_t x) const { return x * 2; }
    std::string string() const { return "s"; }
};

class doc_pass_fix_tests : public suite {
public:
    doc_pass_fix_tests() : suite("Doc Pass Fixes") {}

    void forge_tests() override {
        auto run_both = [](const std::string& src) {
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                try { out[idx] = e->execute(src).to_string(); }
                catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                ++idx;
            }
            return std::make_pair(out[0], out[1]);
        };
        auto expect = [this, run_both](const std::string& src, const std::string& expected, const char* what) {
            auto [i_out, v_out] = run_both(src);
            check_eq(expected, i_out, std::string(what) + " (interpreter)");
            check_eq(expected, v_out, std::string(what) + " (vm)");
        };

        test("base_class_statics_resolve_by_bare_name_for_a_subclass_instance", [expect]() {
            const std::string defs =
                "class Base { static int count = 0; static int seed() { return count + 100; }"
                "  int id = 0; Base() { count += 1; id = count; } int read() { return count; } void bump() { count++; } }"
                " class Derived : Base { Derived() : super() {} int mine() { return count * 10; } static int peek() { return count + seed(); } }"
                " class Leaf : Derived { Leaf() : super() {} int deep() { return count + seed(); } }";
            expect(defs + " auto d = Derived(); auto e = Derived(); to_string(e.id) + \" \" + to_string(Base::count);", "2 2", "base ctor compound store for a subclass instance");
            expect(defs + " auto d = Derived(); d.bump(); to_string(d.read()) + \" \" + to_string(Base::count);", "2 2", "base method read and ++ for a subclass instance");
            expect(defs + " auto d = Derived(); to_string(d.mine());", "10", "subclass method reads a base static");
            expect(defs + " auto d = Derived(); to_string(Derived::peek());", "102", "subclass static method reads a base static and calls a base static method");
            expect(defs + " auto l = Leaf(); to_string(l.deep()) + \" \" + to_string(l.read());", "102 1", "a grandchild reads the base static through two levels");
            expect("class B { static int n = 1; void set(int v) { n = v; } } class D : B { int get() { return n; } }"
                   " auto d = D(); d.set(7); to_string(d.get()) + \" \" + to_string(B::n);", "7 7", "base method plain store for a subclass instance");
            expect("class B { static int n = 1; int get() { return n; } } class D : B { static int n = 50; }"
                   " auto d = D(); to_string(d.get());", "1", "a base method reads its own class's static, not a same-named subclass static");
            expect("class B { static int n = 1; } class D : B { static int m = 2; void touch() { m += n; } } class E : D {}"
                   " auto e = E(); e.touch(); to_string(D::m);", "3", "an inherited method run on a grandchild reads and stores its class's statics");
            expect(defs + " auto d = Derived(); to_string(Derived::count) + \" \" + to_string(Derived::seed());", "1 101", "a base static and static method through the subclass name");
            expect(defs + " Derived::count = 5; Derived::count += 1; to_string(Base::count);", "6", "a base static stored through the subclass name");
        });

        test("builtins_see_through_element_references", [this, run_both]() {
            const std::pair<const char*, const char*> rows[] = {
                {"var a = [[1, 2], [3]]; to_json(a[0]);", "[1,2]"},
                {"var m = {\"k\": {\"x\": 1}}; to_json(m[\"k\"]);", "{\"x\":1}"},
                {"var a = [{\"x\": [1]}]; to_json(a[0][\"x\"]);", "[1]"},
                {"var a = [1, 2]; to_json(a[1]);", "2"},
                {"class P { array<int> v = [1, 2]; } var ps = [P()]; to_json(ps[0].v);", "[1,2]"},
                {"var a = [[1, 2], [3]]; type_of(a[0]);", "array"},
                {"var m = {\"k\": {\"x\": 1}}; type_of(m[\"k\"]);", "map"},
                {"var a = [[1, 2], [3]]; var b = a[0]; to_string(a[0]) == to_string(b);", "true"},
                {"var m = {\"k\": [1]}; var b = m[\"k\"]; to_string(m[\"k\"]) == to_string(b);", "true"},
                {"var a = [1, 2]; to_string(a[1]) + format(\"{}\", a[0]);", "21"},
                {"var a = [[1, 2], [3]]; var s = to_json(a[1], 2); s;", "[\n  3\n]"},
                {"var a = [[1, 2], [3]]; to_binary(a[0]) == to_binary([1, 2]);", "true"},
                {"var a = [[1, 2], [3]]; to_base64(a[0]) == to_base64([1, 2]);", "true"},
            };
            std::string misses;
            for (const auto& [src, want] : rows) {
                auto [i_out, v_out] = run_both(src);
                if (i_out != want) { misses += std::string("interp: ") + src + " -> " + i_out + "\n"; }
                if (v_out != want) { misses += std::string("vm: ") + src + " -> " + v_out + "\n"; }
            }
            check_eq(std::string(), misses, "builtins over element references");
        });

        test("to_json_indent_argument_pretty_prints", [expect]() {
            expect("to_json({\"a\": 1, \"b\": [1, 2], \"c\": {}}, 2);",
                   "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ],\n  \"c\": {}\n}", "a nested map at two spaces");
            expect("to_json([1, [2], []], 4);", "[\n    1,\n    [\n        2\n    ],\n    []\n]", "arrays at four spaces");
            expect("to_json({\"a\": 1, \"b\": [1, 2]});", "{\"a\":1,\"b\":[1,2]}", "no indent stays compact");
            expect("to_json({\"a\": 1, \"b\": [1, 2]}, 0);", "{\"a\":1,\"b\":[1,2]}", "indent 0 is compact");
            expect("to_json(5, 2);", "5", "a scalar is unchanged");
            expect("var v = {\"a\": [1, 2.5, \"s\", true, null], \"m\": {\"x\": {\"y\": 1}}}; to_json(from_json(to_json(v, 2))) == to_json(v);",
                   "true", "an indented document round-trips through from_json");
            expect("class P { int x = 1; array<int> v = [2]; } to_json(P(), 2);",
                   "{\n  \"$type\": \"P\",\n  \"$val\": {\n    \"x\": 1,\n    \"v\": [\n      2\n    ]\n  }\n}", "a class instance");
        });

        test("type_keywords_are_member_names_after_a_dot", [expect]() {
            expect("class C { int map(int x) { return x * 2; } int string() { return 7; } int array() { return 3; } }"
                   " auto c = C(); to_string(c.map(4)) + \" \" + to_string(c.string()) + \" \" + to_string(c.array());",
                   "8 7 3", "methods named after type keywords");
            expect("class C { bool bool() { return true; } float float() { return 1.5; } int var() { return 2; } int function() { return 3; } }"
                   " auto c = C(); to_string(c.bool()) + \" \" + to_string(c.float()) + \" \" + to_string(c.var()) + \" \" + to_string(c.function());",
                   "true 1.5 2 3", "more type keywords as method names");
            expect("class C { static int map() { return 4; } function array(int x) { return x + 1; } int default(int x) { return -x; } }"
                   " auto c = C(); to_string(C::map()) + \" \" + to_string(c.array(1)) + \" \" + to_string(c.default(3));",
                   "4 2 -3", "a static, a function-spelled method and a non-type keyword");
            expect("class Base { static int count = 0; Base() { count += 1; } }"
                   " class C : Base { int array() { return 3; } C() : super() {} int map(int x) { return x * count; } static int string() { return 7; } }"
                   " auto c = C(); auto d = C(); to_string(c.map(5)) + \"|\" + to_string(c.array()) + \"|\" + to_string(C::string()) + \"|\" + to_string(Base::count);",
                   "10|3|7|2", "keyword members beside inherited statics");
            expect("var m = {\"map\": 1}; m.map;", "ERROR: map has no member 'map'; read the key as [\"map\"]", "a keyword after the dot is a member name (a map's key reads with [])");
            expect("class C { int x = 0; } var c = C(); c.map();", "ERROR: C has no member 'map'", "an undeclared keyword member is the no-member error");
            expect("var a = [1, 2]; a.string(5);", "ERROR: array has no member 'string'", "an array answers a keyword member it lacks with the no-member error");
        });

        test("host_methods_named_after_keywords", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                dynamic_binder<keyword_member_probe>(*e, "KeywordProbe")
                    .constructor<>()
                    .method("map", &keyword_member_probe::map)
                    .method("string", &keyword_member_probe::string)
                    .build();
                check_eq(std::string("10|s"), e->execute("auto k = KeywordProbe(); to_string(k.map(5)) + \"|\" + k.string();").to_string(),
                         use_vm ? "vm" : "interpreter");
            }
        });
    }
};

} // namespace jai::foundry::tests

using doc_pass_fix_tests = jai::foundry::tests::doc_pass_fix_tests;
FOUNDRY_REGISTER(doc_pass_fix_tests)
