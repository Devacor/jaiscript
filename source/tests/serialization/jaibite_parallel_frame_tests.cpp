#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/jaibite.hpp>
#include <jaiscript/detail/ast_serializer.hpp>
#include <jaiscript/detail/body_walker.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>

namespace jai::foundry::tests {

class jaibite_parallel_frame_tests : public suite {
public:
    jaibite_parallel_frame_tests() : suite("Jaibite Parallel Frames") {}

    static std::vector<declaration_ptr> parse(engine& eng, const std::string& source) {
        auto templates = detail::engine_access::get_registered_template_types(eng);
        lexer lex(source, detail::engine_access::get_symbolizer(eng));
        auto tokens = lex.tokenize();
        parser parser(tokens, detail::engine_access::get_symbolizer(eng), &eng, templates);
        auto result = parser.parse();
        check_true(bool(result), "parallel-frame fixture must parse normally");
        return std::move(result.value());
    }

    static std::shared_ptr<parallel_for_stmt> first_parallel(const std::vector<declaration_ptr>& declarations) {
        auto function = std::dynamic_pointer_cast<function_decl>(declarations.front());
        check_not_null(function);
        auto wrapper = std::dynamic_pointer_cast<statement_decl>(function->body->declarations.front());
        check_not_null(wrapper);
        auto result = std::dynamic_pointer_cast<parallel_for_stmt>(wrapper->statement);
        check_not_null(result);
        return result;
    }

    static void reject_foreign_slot() {
        auto eng = make_engine();
        auto declarations = parse(*eng,
            "int f(int outer_value) { parallel_for (auto& item : [1]) { item += outer_value; } return 0; } 0;");
        auto function = std::static_pointer_cast<function_decl>(declarations.front());
        auto loop = first_parallel(declarations);
        size_t changed = 0;
        detail::body_walker walk{
            [&](identifier_expr* identifier) {
                if (identifier->name == "outer_value") {
                    check_eq(SIZE_MAX, identifier->slot_index, "parser must not expose enclosing slots to parallel body");
                    identifier->slot_index = function->parameters.front().slot_index;
                    ++changed;
                }
            },
            {}, detail::engine_access::get_symbolizer(*eng)
        };
        walk.stmt(loop->body);
        check_eq(size_t(1), changed, "one foreign identifier must be corrupted");
        const auto bytes = detail::serialize_jaibite(declarations, *detail::engine_access::get_symbolizer(*eng), eng->registration_fingerprint());
        bool rejected = false;
        std::string unexpected;
        try { eng->jaibite_load_bytes(bytes); }
        catch (const jai::runtime_error& error) {
            rejected = std::string(error.what()).starts_with("jaibite: ");
            if (!rejected) unexpected = error.what();
        } catch (const std::exception& error) {
            unexpected = error.what();
        }
        check_true(unexpected.empty(), "must reject structurally, not by static checking/admission: " + unexpected);
        check_eq(script_int(42), eng->execute("40 + 2;").as_int(), "engine usable after attempted load");
        check_true(rejected, "loader accepted a finite enclosing slot inside a parallel lookup boundary");
    }

    void forge_tests() override {
        test("parallel_foreign_slot_rejected_structurally", []() {
            reject_foreign_slot();
        });
        test("parallel_foreign_symbol_lookup_preserves_original_bytes", []() {
            auto eng = make_engine();
            auto declarations = parse(*eng,
                "int f(int outer_value) { parallel_for (auto& item : [1]) { item += outer_value; } return 0; } 0;");
            // stamped checked-clean so the load is the structural reader alone, and the bite saves back the same stamp
            const auto bytes = detail::serialize_jaibite(declarations, *detail::engine_access::get_symbolizer(*eng), eng->registration_fingerprint(),
                                                         detail::jaibite_checked_clean_flags());
            auto loaded = eng->jaibite_load_bytes(bytes);
            check_eq(bytes, loaded.save_bytes(), "valid SIZE_MAX outer symbol lookup must survive unchanged");
        });
        test("nested_lambda_can_capture_parallel_parent_slot", []() {
            auto eng = make_engine();
            auto declarations = parse(*eng,
                "int f() { parallel_for (auto& item : [1]) { auto read = [=]() { return item; }; } return 0; } 0;");
            auto loop = first_parallel(declarations);
            auto block = std::dynamic_pointer_cast<block_stmt>(loop->body);
            check_not_null(block);
            auto local = std::dynamic_pointer_cast<variable_decl>(block->declarations.front());
            check_not_null(local);
            auto lambda = std::dynamic_pointer_cast<lambda_expr>(local->initializer);
            check_not_null(lambda);
            size_t captures = 0;
            detail::body_walker walk{
                [&](identifier_expr* identifier) {
                    if (identifier->name == "item") {
                        check_eq(size_t(0), identifier->slot_index, "nested lambda may use its parallel parent's slot");
                        ++captures;
                    }
                },
                {}, detail::engine_access::get_symbolizer(*eng)
            };
            walk.stmt(lambda->body);
            check_eq(size_t(1), captures);
            // stamped checked-clean so the load is the structural reader alone, and the bite saves back the same stamp
            const auto bytes = detail::serialize_jaibite(declarations, *detail::engine_access::get_symbolizer(*eng), eng->registration_fingerprint(),
                                                         detail::jaibite_checked_clean_flags());
            auto loaded = eng->jaibite_load_bytes(bytes);
            check_eq(bytes, loaded.save_bytes(), "parallel boundary must not block an inner lambda's immediate-parent capture");
        });
    }
};

} // namespace jai::foundry::tests

using jaibite_parallel_frame_tests = jai::foundry::tests::jaibite_parallel_frame_tests;
FOUNDRY_REGISTER(jaibite_parallel_frame_tests)
