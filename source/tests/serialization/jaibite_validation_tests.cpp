#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/jaibite.hpp>
#include <jaiscript/detail/ast_serializer.hpp>
#include <jaiscript/detail/body_walker.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>

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

    static void reject(engine& eng, const std::vector<declaration_ptr>& declarations) {
        const auto bytes = detail::serialize_jaibite(declarations, *detail::engine_access::get_symbolizer(eng), eng.registration_fingerprint());
        reject_bytes(eng, bytes);
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
            reject_bytes(*eng, bytes);
        });

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
