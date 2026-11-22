#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/jaibite.hpp>
#include <jaiscript/detail/ast_serializer.hpp>
#include <jaiscript/detail/body_walker.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "jaibite_sweep.hpp"
#include <algorithm>
#include <optional>

namespace jai::foundry::tests {

class jaibite_validation_tests : public suite {
public:
    jaibite_validation_tests() : suite("Jaibite Validation") {}

    static std::vector<declaration_ptr> parse(engine& eng, const std::string& source) {
        auto templates = detail::engine_access::get_registered_template_types(eng);
        lexer lex(source, detail::engine_access::get_symbolizer(eng));
        auto tokens = lex.tokenize();
        parser parse(tokens, detail::engine_access::get_symbolizer(eng), &eng, templates);
        auto result = parse.parse();
        check_true(bool(result), "validation fixture must parse normally");
        return std::move(result.value());
    }

    static void reject_bytes(engine& eng, const std::vector<uint8_t>& bytes) {
        bool rejected = false;
        try { eng.jaibite_load_bytes(bytes); }
        catch (const jai::runtime_error& error) {
            rejected = true;
            check_true(std::string(error.what()).starts_with("jaibite: "), "rejection must be a loader diagnostic");
        }
        check_true(rejected, "malformed program was accepted by the loader");
        check_eq(script_int(42), eng.execute("40 + 2;").as_int(), "loader rejection must leave the engine usable");
    }

    static std::string refusal(engine& eng, const std::vector<declaration_ptr>& declarations) {
        const auto bytes = detail::serialize_jaibite(declarations, *detail::engine_access::get_symbolizer(eng), eng.registration_fingerprint());
        std::string result;
        try { eng.jaibite_load_bytes(bytes); } catch (const jai::runtime_error& error) { result = error.what(); }
        check_eq(script_int(42), eng.execute("40 + 2;").as_int(), "loader rejection must leave the engine usable");
        return result;
    }

    static identifier_expr* first_identifier(engine& eng, const std::shared_ptr<function_decl>& fn) {
        identifier_expr* result = nullptr;
        detail::body_walker walk{[&](identifier_expr* id) { if (!result) { result = id; } }, {}, detail::engine_access::get_symbolizer(eng)};
        walk.stmt(fn->body);
        check_not_null(result, "fixture must read a name");
        return result;
    }

    static void reject(engine& eng, const std::vector<declaration_ptr>& declarations) {
        const auto bytes = detail::serialize_jaibite(declarations, *detail::engine_access::get_symbolizer(eng), eng.registration_fingerprint());
        reject_bytes(eng, bytes);
    }

    static std::shared_ptr<engine> stdlib_engine() {
        auto e = make_engine();
        stdlib::register_all(*e);
        return e;
    }

    static constexpr const char* k_generic_unit =
        "class Stack<T> { array<T> items; static int made = 0; void push(T x) { items.push(x); made += 1; } T top() { return items[items.size() - 1]; } U as<U>() { return cast<U>(top()); } }\n"
        "class Named<T> : Stack<T> { string name = \"n\"; }\n"
        "T largest<T>(array<T> xs) { T best = xs[0]; for (auto x : xs) { if (x > best) { best = x; } } return best; }\n"
        "coroutine T each<T>(array<T> xs) { for (auto x : xs) { yield x; } }\n"
        "Stack<int> s; s.push(4); Named<string> names; names.push(\"ann\"); int n = 0; for (auto x : each<int>([1, 2, 3])) { n += x; }\n"
        "to_string(s.top()) + names.top() + to_string(largest([3.5, 1.5])) + to_string(n) + to_string(s.as<float>());\n";

    // Every stride-th byte of a saved bite takes a low-bit flip, a high-bit flip and 0xFF, and the bite is cut short at 120
    // lengths. As damaged, each mutant is refused by a loader text before any of it is read. With its integrity value written
    // again a mutant reaches the loader's own checks and the backend: it is refused, or it loads and runs as the program it
    // now is, to this result, another one or an error.
    static void sweep_bite(bool a_resealed) {
        auto maker = stdlib_engine();
        jaibite bite = maker->jaibite(sweep_unit(k_generic_unit));
        const std::string wanted = bite.execute().to_string();
        const std::vector<uint8_t> bytes = bite.save_bytes();
        size_t mutants = 0;
        size_t refused = 0;
        size_t same = 0;
        size_t other = 0;
        size_t raised = 0;
        std::string not_refused;
        const auto refuser = stdlib_engine();   // a damaged mutant is refused before any of it reaches the engine: one takes them all
        const auto attempt = [&](std::vector<uint8_t> a_mutated, const std::string& a_what) {
            if (a_resealed) { detail::jaibite_seal(a_mutated.data(), a_mutated.size()); }
            sweep_note("mutation " + a_what);
            ++mutants;
            auto e = a_resealed ? stdlib_engine() : refuser;
            e->execution_budget(0);   // a clock-free bound: a mutant that loops ends at the same step under any load
            e->execution_step_budget(256);
            e->memory_cap(size_t(256) * 1024 * 1024);
            std::optional<jaibite> loaded;
            try {
                loaded.emplace(e->jaibite_load_bytes(a_mutated));
            } catch (const std::exception& ex) {
                ++refused;
                if (!a_resealed && !std::string(ex.what()).starts_with("jaibite: ") && not_refused.empty()) { not_refused = a_what + ": " + ex.what(); }
                return;
            }
            if (!a_resealed && not_refused.empty()) { not_refused = a_what + ": loaded"; }
            try {
                const std::string got = loaded->execute().to_string();
                if (got == wanted) { ++same; } else { ++other; sweep_note("  ran to " + got.substr(0, 200)); }
            } catch (const std::exception& ex) {
                ++raised;
                sweep_note("  raised " + std::string(ex.what()).substr(0, 200));
            }
        };
        const size_t stride = sweep_stride(std::max<size_t>(1, bytes.size() / 400));
        for (size_t at = sweep_start(); at < sweep_end(bytes.size()); at += stride) {
            for (const uint8_t value : {uint8_t(bytes[at] ^ 0x01), uint8_t(bytes[at] ^ 0x80), uint8_t(0xFF)}) {
                if (value == bytes[at]) { continue; }
                std::vector<uint8_t> mutated = bytes;
                mutated[at] = value;
                attempt(std::move(mutated), std::to_string(at) + "=" + std::to_string(value));
            }
        }
        for (size_t length = 0; length < bytes.size(); length += std::max<size_t>(1, bytes.size() / 120)) {
            attempt(std::vector<uint8_t>(bytes.begin(), bytes.begin() + length), "truncated to " + std::to_string(length));
        }
        sweep_note(std::string(a_resealed ? "resealed" : "damaged") + " bite of " + std::to_string(bytes.size()) + " bytes: " + std::to_string(mutants) +
                   " mutants, " + std::to_string(refused) + " refused, " + std::to_string(same) + " ran to the same result, " + std::to_string(other) +
                   " to another, " + std::to_string(raised) + " to an error");
        check_true(mutants >= 1300 || !sweep_whole(), "the sweep's size: " + std::to_string(mutants));
        if (!a_resealed) { check_eq(std::string(), not_refused, "a damaged bite the loader did not refuse"); }
        check_eq(wanted, stdlib_engine()->jaibite_load_bytes(bytes).execute().to_string(), "the bytes as saved still load");
    }

    template<typename T>
    static std::shared_ptr<T> first_statement(const std::shared_ptr<function_decl>& fn) {
        check_not_null(fn->body);
        check_false(fn->body->declarations.empty());
        auto wrapper = std::dynamic_pointer_cast<statement_decl>(fn->body->declarations.front());
        check_not_null(wrapper, "fixture statement must have its declaration wrapper");
        auto result = std::dynamic_pointer_cast<T>(wrapper->statement);
        check_not_null(result, "fixture must contain the expected statement kind");
        return result;
    }

    static jai::jaibite load_unchanged(engine& eng, const std::vector<uint8_t>& bytes) {
        auto result = eng.jaibite_load_bytes(bytes);
        check_eq(bytes, result.save_bytes(), "validation must preserve valid encoded metadata byte-for-byte");
        return result;
    }

    void forge_tests() override {
        test("parameter_slot_must_fit_local_count", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f(int n) { return n; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            fn->parameters.front().slot_index = fn->local_count;
            reject(*eng, declarations);
        });

        test("identifier_slot_must_fit_its_frame", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f(int n) { return n; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            detail::body_walker walk{
                [&](identifier_expr* id) { id->slot_index = fn->local_count + 99; },
                {}, detail::engine_access::get_symbolizer(*eng)
            };
            walk.stmt(fn->body);
            reject(*eng, declarations);
        });

        test("declaration_slot_must_fit_local_count", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f() { int n = 3; return n; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            auto local = std::static_pointer_cast<variable_decl>(fn->body->declarations.front());
            local->slot_index = fn->local_count;
            reject(*eng, declarations);
        });

        test("local_count_cannot_claim_unencoded_storage", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f() { return 3; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            fn->local_count = SIZE_MAX / 2;
            reject(*eng, declarations);
        });

        test("top_level_declaration_cannot_claim_a_local_slot", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int n = 3;");
            std::static_pointer_cast<variable_decl>(declarations.front())->slot_index = 0;
            // the writer drops a program's own slots (they are re-derived at load): a body-mode writer still puts the claim on the wire
            reject_bytes(*eng, detail::ast_writer(*detail::engine_access::get_symbolizer(*eng)).in_body().serialize(declarations, eng->registration_fingerprint()));
        });

        test("function_body_must_not_be_null", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f() { return 3; } 0;");
            std::static_pointer_cast<function_decl>(declarations.front())->body.reset();
            reject(*eng, declarations);
        });

        test("lambda_body_must_be_block", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "auto read = []() { return 3; }; 0;");
            auto local = std::static_pointer_cast<variable_decl>(declarations.front());
            auto lambda = std::dynamic_pointer_cast<lambda_expr>(local->initializer);
            check_not_null(lambda);
            lambda->body = std::make_shared<return_stmt>(lambda->location);
            reject(*eng, declarations);
        });

        test("try_body_must_be_block", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f() { try {} catch {} return 0; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            auto statement = first_statement<try_stmt>(fn);
            statement->try_block = std::make_shared<break_stmt>(statement->location);
            reject(*eng, declarations);
        });

        test("catch_body_must_be_block", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f() { try {} catch {} return 0; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            auto statement = first_statement<try_stmt>(fn);
            statement->catch_block = std::make_shared<break_stmt>(statement->location);
            reject(*eng, declarations);
        });

        test("for_initializer_must_be_variable_or_expression", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f() { for (int i = 0; i < 1; i++) {} return 0; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            auto loop = first_statement<for_stmt>(fn);
            auto other = parse(*eng, "int not_an_initializer() { return 0; }");
            loop->initializer = other.front();
            reject(*eng, declarations);
        });

        test("range_slot_must_fit_own_frame", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f(int n) { for (auto x : [1]) {} return n; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            first_statement<range_for_stmt>(fn)->variable_slot_index = fn->local_count;
            reject(*eng, declarations);
        });

        test("catch_slot_must_fit_own_frame", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f(int n) { try {} catch (error) {} return n; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            first_statement<try_stmt>(fn)->catch_slot_index = fn->local_count;
            reject(*eng, declarations);
        });

        test("parallel_local_count_must_cover_parameters", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f() { parallel_for (auto& x, auto& y : [1], [2]) {} return 0; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            auto loop = first_statement<parallel_for_stmt>(fn);
            check_eq(size_t(2), loop->arity());
            loop->local_count = loop->arity() - 1;
            reject(*eng, declarations);
        });

        test("parallel_parameter_slot_must_fit_own_frame", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f() { parallel_for (auto& x, auto& y : [1], [2]) {} return 0; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            auto loop = first_statement<parallel_for_stmt>(fn);
            loop->extra_params.front().slot_index = loop->local_count;
            reject(*eng, declarations);
        });

        test("parallel_body_must_be_block", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f() { parallel_for (auto x : [1]) {} return 0; } 0;");
            auto fn = std::static_pointer_cast<function_decl>(declarations.front());
            auto loop = first_statement<parallel_for_stmt>(fn);
            loop->body = std::make_shared<return_stmt>(loop->location);
            reject(*eng, declarations);
        });

        test("class_member_must_be_variable_or_function", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "class Cell { int n = 3; } 0;");
            auto cls = std::static_pointer_cast<class_decl>(declarations.front());
            auto other = parse(*eng, "class NotAField {}");
            cls->members.front().declaration = other.front();
            reject(*eng, declarations);
        });

        test("parallel_parameter_slots_must_be_distinct", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f() { parallel_for (auto& x, auto& y : [1], [2]) {} return 0; } 0;");
            auto loop = first_statement<parallel_for_stmt>(std::static_pointer_cast<function_decl>(declarations.front()));
            loop->extra_params.front().slot_index = loop->loop_param.slot_index;
            reject(*eng, declarations);
        });

        test("case_fallthrough_flag_must_match_body", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int f() { switch (1) { case 1: fallthrough; case 2: return 2; } return 0; } 0;");
            auto statement = first_statement<switch_stmt>(std::static_pointer_cast<function_decl>(declarations.front()));
            statement->cases.front()->has_fallthrough = false;
            reject(*eng, declarations);
        });

        test("array_type_cannot_hide_extra_parameters_during_interning", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "array<int> xs = [1];");
            auto local = std::static_pointer_cast<variable_decl>(declarations.front());
            auto malformed = *local->type;
            malformed.type_params.push_back(detail::engine_access::get_type_info_bool(*eng));
            local->type = &malformed;
            reject(*eng, declarations);
        });

        test("primitive_type_cannot_hide_parameters_during_interning", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int n = 1;");
            auto local = std::static_pointer_cast<variable_decl>(declarations.front());
            auto malformed = *local->type;
            malformed.type_params.push_back(detail::engine_access::get_type_info_bool(*eng));
            local->type = &malformed;
            reject(*eng, declarations);
        });

        test("map_type_cannot_hide_extra_parameters_during_interning", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "map<string, int> xs;");
            auto local = std::static_pointer_cast<variable_decl>(declarations.front());
            auto malformed = *local->type;
            malformed.type_params.push_back(detail::engine_access::get_type_info_bool(*eng));
            local->type = &malformed;
            reject(*eng, declarations);
        });

        test("terminal_varint_must_not_overflow", [this]() {
            auto eng = make_engine();
            const std::vector<declaration_ptr> declarations;
            auto bytes = detail::serialize_jaibite(declarations, *detail::engine_access::get_symbolizer(*eng), eng->registration_fingerprint());
            const size_t header = detail::k_jaibite_header_size;
            check_eq(header + 2, bytes.size(), "fixture expects the fixed header and two empty vectors");
            check_eq(uint8_t(0), bytes[header], "fixture string table must be empty");
            check_eq(uint8_t(0), bytes[header + 1], "fixture declaration list must be empty");
            bytes.resize(header + 1);
            for (size_t i = 0; i < 9; ++i) {
                bytes.push_back(0x80);
            }
            bytes.push_back(0x02);
            detail::jaibite_seal(bytes.data(), bytes.size());
            std::string text;
            try { eng->jaibite_load_bytes(bytes); } catch (const jai::runtime_error& error) { text = error.what(); }
            check_eq(std::string("jaibite: corrupt varint"), text);
        });

        // A name's symbol is the key a backend looks the name up by: one that spells another name, or none where the parser
        // always gives one, is refused
        test("a_symbol_must_spell_the_name_beside_it", [this]() {
            auto eng = make_engine();
            const uint64_t other = detail::engine_access::get_symbolizer(*eng)->intern("other_name");
            auto read = parse(*eng, "int f(int n) { return n; } 0;");
            first_identifier(*eng, std::static_pointer_cast<function_decl>(read.front()))->symbol_id = other;
            check_eq(std::string("jaibite: invalid program: a symbol that does not spell its name"), refusal(*eng, read));
            auto bare = parse(*eng, "int f(int n) { return n; } 0;");
            first_identifier(*eng, std::static_pointer_cast<function_decl>(bare.front()))->symbol_id = UINT64_MAX;
            check_eq(std::string("jaibite: invalid program: a name without its symbol"), refusal(*eng, bare));
            auto named = parse(*eng, "class Cell { int n = 3; } 0;");
            std::static_pointer_cast<class_decl>(named.front())->name_id = other;
            check_eq(std::string("jaibite: invalid program: a symbol that does not spell its name"), refusal(*eng, named));
            auto local = parse(*eng, "int f() { int n = 3; return n; } 0;");
            std::static_pointer_cast<variable_decl>(std::static_pointer_cast<function_decl>(local.front())->body->declarations.front())->name_id = UINT64_MAX;
            check_eq(std::string("jaibite: invalid program: a name without its symbol"), refusal(*eng, local));
            auto member = parse(*eng, "class Cell { int n = 3; } Cell c; c.n;");
            auto* access = static_cast<member_expr*>(std::static_pointer_cast<expression_decl>(member.back())->expression.get());
            check_eq(int(node_type::member_expr), int(access->get_type()));
            access->getter_id = other;
            check_eq(std::string("jaibite: invalid program: a symbol that does not spell its name"), refusal(*eng, member));
            access->getter_id = UINT64_MAX;
            access->member_id = other;
            check_eq(std::string("jaibite: invalid program: a symbol that does not spell its name"), refusal(*eng, member));
        });

        test("a_yield_must_stand_in_a_coroutine_function", [this]() {
            auto eng = make_engine();
            auto plain = parse(*eng, "coroutine int count() { yield 1; return 0; } 0;");
            std::static_pointer_cast<function_decl>(plain.front())->is_coroutine = false;
            check_eq(std::string("jaibite: invalid program: 'yield' outside a coroutine function"), refusal(*eng, plain));
            auto in_lambda = parse(*eng, "coroutine int count() { auto f = []() { return 1; }; yield f(); return 0; } 0;");
            auto local = std::static_pointer_cast<variable_decl>(std::static_pointer_cast<function_decl>(in_lambda.front())->body->declarations.front());
            auto lambda = std::dynamic_pointer_cast<lambda_expr>(local->initializer);
            check_not_null(lambda);
            auto body = std::dynamic_pointer_cast<block_stmt>(lambda->body);
            check_not_null(body);
            auto returned = std::dynamic_pointer_cast<return_stmt>(std::static_pointer_cast<statement_decl>(body->declarations.front())->statement);
            check_not_null(returned);
            returned->value = std::make_shared<yield_expr>(returned->location, returned->value);
            check_eq(std::string("jaibite: invalid program: 'yield' outside a coroutine function"), refusal(*eng, in_lambda));
            auto at_top = parse(*eng, "int n = 1; n;");
            auto last = std::static_pointer_cast<expression_decl>(at_top.back());
            last->expression = std::make_shared<yield_expr>(last->location, last->expression);
            check_eq(std::string("jaibite: invalid program: 'yield' outside a coroutine function"), refusal(*eng, at_top));
        });

        test("a_calls_type_arguments_must_be_type_values", [this]() {
            auto eng = make_engine();
            const auto call_of = [](const std::vector<declaration_ptr>& declarations) {
                auto* call = static_cast<call_expr*>(std::static_pointer_cast<expression_decl>(declarations.back())->expression.get());
                check_eq(int(node_type::call_expr), int(call->get_type()));
                return call;
            };
            auto counted = parse(*eng, "int f(int a) { return a; } f(1);");
            call_of(counted)->type_argument_count = 1;
            check_eq(std::string("jaibite: invalid program: a call's type arguments"), refusal(*eng, counted));
            auto beyond = parse(*eng, "int f(int a) { return a; } f(1);");
            call_of(beyond)->type_argument_count = 2;
            check_eq(std::string("jaibite: invalid program: a call's type arguments"), refusal(*eng, beyond));
        });

        test("a_named_functions_identifier_cannot_carry_a_slot_of_the_function_around_it", [this]() {
            auto eng = make_engine();
            auto declarations = parse(*eng, "int q = 5; int g() { int a = 1; int b = 2; int c = 3; int f() { return q; } return f() + a + b + c; } 0;");
            auto outer = std::static_pointer_cast<function_decl>(declarations[1]);
            std::shared_ptr<function_decl> inner;
            for (const auto& d : outer->body->declarations) {
                if (d->get_type() == node_type::function_decl) { inner = std::static_pointer_cast<function_decl>(d); }
            }
            check_not_null(inner, "fixture must nest a function");
            check_true(outer->local_count >= 3 && inner->local_count < 3, "the outer frame holds slots the inner one does not");
            first_identifier(*eng, inner)->slot_index = 2;
            check_eq(std::string("jaibite: invalid program: local slot out of range"), refusal(*eng, declarations));
        });

        // a flag is written 0 or 1: any other byte there is damage, never another way to say true
        test("a_flag_byte_is_zero_or_one", [this]() {
            auto eng = make_engine();
            auto bytes = eng->jaibite("int n = 1; n;").save_bytes();
            check_eq(uint8_t(1), bytes.back(), "fixture ends on the last expression's implicit-return flag");
            bytes.back() = 0x81;
            detail::jaibite_seal(bytes.data(), bytes.size());
            std::string text;
            try { eng->jaibite_load_bytes(bytes); } catch (const jai::runtime_error& error) { text = error.what(); }
            check_eq(std::string("jaibite: corrupt flag"), text);
        });

        // The context section is read as the rest of a bite is: a '<' answer that is none of the three, a settled flag that is
        // no flag and a query that names no string are each refused
        test("a_malformed_context_section_is_refused", [this]() {
            auto eng = make_engine();
            detail::jaibite_context context;
            context.less_queries = {"0|||no_such_name_here"};
            context.less_answers = std::string(1, detail::k_less_nothing);
            context.less_settled = true;
            const std::vector<declaration_ptr> none;
            const auto bytes = detail::serialize_jaibite(none, *detail::engine_access::get_symbolizer(*eng), eng->registration_fingerprint(), 0, {}, {}, &context);
            check_eq(uint8_t(0), bytes.back(), "fixture ends on its empty declaration list");
            check_eq(uint8_t(1), bytes[bytes.size() - 2], "before it, the settled flag");
            check_eq(uint8_t(detail::k_less_nothing), bytes[bytes.size() - 3], "before it, the one answer");
            check_eq(uint8_t(0), bytes[bytes.size() - 4], "before it, the one query's string");
            const auto refusal_of = [&](size_t a_from_end, uint8_t a_value) {
                auto changed = bytes;
                changed[changed.size() - a_from_end] = a_value;
                detail::jaibite_seal(changed.data(), changed.size());
                std::string text;
                try { eng->jaibite_load_bytes(changed); } catch (const std::exception& error) { text = error.what(); }
                return text;
            };
            check_eq(std::string(), refusal_of(2, 1), "as written it loads");
            check_eq(std::string("jaibite: corrupt flag"), refusal_of(2, 2));
            check_eq(std::string("jaibite: corrupt '<' answer"), refusal_of(3, uint8_t('x')));
            check_eq(std::string("jaibite: context name out of range"), refusal_of(4, 0x7F));
        });

        test("a_parameters_slot_must_be_its_position", [this]() {
            auto eng = make_engine();
            auto bare = parse(*eng, "int f(int a, int b) { return a + b; } 0;");
            std::static_pointer_cast<function_decl>(bare.front())->parameters[1].slot_index = SIZE_MAX;
            check_eq(std::string("jaibite: invalid program: a parameter's slot does not match its position"), refusal(*eng, bare));
            auto swapped = parse(*eng, "auto f = [](int a, int b) { return a + b; }; 0;");
            auto lambda = std::dynamic_pointer_cast<lambda_expr>(std::static_pointer_cast<variable_decl>(swapped.front())->initializer);
            check_not_null(lambda);
            std::swap(lambda->parameters[0].slot_index, lambda->parameters[1].slot_index);
            check_eq(std::string("jaibite: invalid program: a parameter's slot does not match its position"), refusal(*eng, swapped));
        });

        test("a_corrupted_bite_of_a_generic_unit_is_refused_at_load", [this]() { sweep_bite(false); });

        test("a_corrupted_bite_of_a_generic_unit_throws_or_runs_never_crashes", [this]() { sweep_bite(true); });

        test("valid_default_argument_outer_slots_preserve_bytes", [this]() {
            auto eng = make_engine();
            auto bite = eng->jaibite(R"(
                int outer() {
                    int a = 1; int b = 2; int seed = 3;
                    int inner(int value = seed) { return value; }
                    return 0;
                }
                0;
            )");
            auto loaded = load_unchanged(*eng, bite.save_bytes());
            check_eq(script_int(0), loaded.execute().as_int());
        });

        test("valid_constructor_initializer_preserves_bytes", [this]() {
            // a class is a program-scope type: its delegating initializer reads a program-scope name
            auto eng = make_engine();
            auto bite = eng->jaibite(R"(
                int seed = 3;
                class Inner {
                    int value = 0;
                    Inner() : this(seed) {}
                    Inner(int n) { value = n; }
                }
                int outer() { int a = 1; int b = 2; Inner i; return i.value; }
                outer();
            )");
            auto loaded = load_unchanged(*eng, bite.save_bytes());
            check_eq(script_int(3), loaded.execute().as_int());
        });

        test("valid_by_value_parallel_parameter_preserves_bytes", [this]() {
            auto eng = make_engine();
            eng->parallel_thread_count(2);
            auto bite = eng->jaibite(R"(
                array<int> input = [1, 2, 3];
                parallel_for (auto x : input) { x = x + 10; }
                input[0] + input[1] + input[2];
            )");
            auto loaded = load_unchanged(*eng, bite.save_bytes());
            check_eq(script_int(6), loaded.execute().as_int());
        });

        test("saved_lambda_outer_plan_preserves_bytes", [this]() {
            auto author = jai::foundry::make_engine();
            auto bite = author->jaibite(R"(
                int read() {
                    int a = 1; int b = 2; int c = 3; int value = 4;
                    auto result = [&]() { return value; };
                    return result();
                }
                read();
            )");
            check_eq(script_int(4), bite.execute().as_int());
            auto eng = make_engine();
            auto loaded = load_unchanged(*eng, bite.save_bytes());
            check_eq(script_int(4), loaded.execute().as_int());
        });

        test("saved_lambda_local_capture_still_shadows_global", [this]() {
            auto author = jai::foundry::make_engine();
            auto bite = author->jaibite(R"(
                int value = 99;
                int read() {
                    int a = 1; int b = 2; int c = 3; int value = 4;
                    auto result = [=]() { return value; };
                    return result();
                }
                read();
            )");
            check_eq(script_int(4), bite.execute().as_int());
            auto eng = make_engine();
            auto loaded = load_unchanged(*eng, bite.save_bytes());
            check_eq(script_int(4), loaded.execute().as_int());
        });

        test("saved_nested_coroutine_preserves_bytes", [this]() {
            auto author = jai::foundry::make_engine();
            auto bite = author->jaibite(R"(
                int read() {
                    int a = 1; int b = 2; int c = 3; int value = 4;
                    coroutine int result(int v) { yield v; return 0; }
                    auto handle = result(value);
                    return handle.next();
                }
                read();
            )");
            check_eq(script_int(4), bite.execute().as_int());
            auto eng = make_engine();
            auto loaded = load_unchanged(*eng, bite.save_bytes());
            check_eq(script_int(4), loaded.execute().as_int());
        });

        test("valid_outer_slot_larger_than_lambda_frame_survives", [this]() {
            auto eng = make_engine();
            auto bite = eng->jaibite(R"(
                int f() {
                    int a = 1; int b = 2; int c = 3; int d = 4;
                    auto read = [&]() { return d; };
                    return read();
                }
                f();
            )");
            const auto bytes = bite.save_bytes();
            auto loaded = eng->jaibite_load_bytes(bytes);
            check_eq(bytes, loaded.save_bytes(), "valid bytes must not be rewritten by validation");
            check_eq(script_int(4), loaded.execute().as_int());
        });
    }
};

} // namespace jai::foundry::tests

using jaibite_validation_tests = jai::foundry::tests::jaibite_validation_tests;
FOUNDRY_REGISTER(jaibite_validation_tests)
