#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/reflection.hpp>
#include <jaiscript/detail/parallel_transform.hpp>
#include <sstream>

namespace jai::foundry::tests {

struct custom_privilege_probe {
    script_int read() { return 17; }
    script_int query() const { return 23; }
};

class custom_privilege_tests : public suite {
public:
    custom_privilege_tests() : suite("Custom Privilege") {}

    template<class Engine, class F>
    void with_custom_api(Engine& eng, F&& body) {
        if constexpr (requires { eng.register_privilege("app.edit"); eng.enable(std::string_view("app.edit")); }) {
            body(eng);
        } else {
            check_true(false, "Engine has no string-based custom privilege API");
        }
    }

    template<class F>
    void custom_test(const char* name, F body) {
        test(name, [this, body]() {
            auto eng = make_engine();
            with_custom_api(*eng, body);
        });
    }

    template<class F>
    static std::string failure(F&& operation) {
        try { operation(); }
        catch (const std::exception& error) { return error.what(); }
        return {};
    }

    static std::string denied(const char* name = "app.edit") {
        return std::string("Insufficient privilege: ") + name + ".";
    }

    template<class Engine>
    static std::string permission(Engine& eng, std::string name) {
        eng.register_privilege(name);
        return name;
    }

    void forge_tests() override {
        custom_test("registration_is_disabled_and_idempotent", [this](auto& eng) {
            auto edit = permission(eng, "app.edit");
            check_false(eng.is_enabled(edit));
            check_false(eng.execute("reflect::is_enabled(\"app.edit\")").as_bool());
            eng.enable(edit);
            auto again = permission(eng, "app.edit");
            check_true(eng.is_enabled(again));
            eng.disable(again);
            check_false(eng.is_enabled(edit));
            check_true(eng.is_enabled(jai::filesystem));
        });

        custom_test("same_name_is_reusable_but_grants_are_engine_local", [this](auto& eng) {
            auto other_ptr = make_engine();
            with_custom_api(*other_ptr, [&](auto& other) {
                auto name = permission(eng, "app.edit");
                other.register_privilege(name);
                other.enable(name);
                eng.add_function("edit", []() { return script_int(42); }, name);
                other.add_function("edit", []() { return script_int(42); }, name);
                check_false(eng.is_enabled(name));
                check_eq(denied(), failure([&]() { eng.execute("edit()"); }));
                check_eq(script_int(42), other.execute("edit()").as_int());
                eng.enable(name);
                other.disable(name);
                check_true(eng.is_enabled(name));
                check_false(other.is_enabled(name));
            });
        });

        custom_test("mixed_requirements_are_all_required", [this](auto& eng) {
            auto edit = permission(eng, "app.edit");
            auto publish = permission(eng, "app.publish");
            const std::initializer_list<std::string_view> required = {"filesystem", edit, publish, edit};
            int calls = 0;
            eng.add_function("publish", [&]() { return script_int(++calls); }, required);
            check_eq(denied(), failure([&]() { eng.execute("publish()"); }));
            eng.enable(edit);
            check_eq(denied("app.publish"), failure([&]() { eng.execute("publish()"); }));
            eng.enable(publish);
            check_eq(script_int(1), eng.execute("publish()").as_int());
            eng.disable(jai::filesystem);
            check_eq(denied("filesystem"), failure([&]() { eng.execute("publish()"); }));
            eng.enable(required);
            check_true(eng.is_enabled(required));
            check_eq(script_int(2), eng.execute("publish()").as_int());
            eng.disable({edit, publish});
            check_false(eng.is_enabled(edit));
            check_false(eng.is_enabled(publish));
            check_true(eng.is_enabled(jai::filesystem));
            check_eq(2, calls);
        });

        custom_test("saved_alias_and_reflection_recheck_grants", [this](auto& eng) {
            stdlib::register_reflection_functions(eng.shared_from_this());
            auto edit = permission(eng, "app.edit");
            int calls = 0;
            eng.add_function("edit", [&]() { return script_int(++calls); }, edit);
            eng.execute("var saved_edit = edit;");
            eng.enable(edit);
            check_eq(script_int(1), eng.execute("saved_edit()").as_int());
            eng.disable(edit);
            for (const char* code : {"edit()", "saved_edit()", "reflect::call(\"edit\")"}) {
                check_eq(denied(), failure([&]() { eng.execute(code); }));
            }
            check_eq(1, calls);
        });

        custom_test("methods_and_variadics_share_the_guard", [this](auto& eng) {
            stdlib::register_reflection_functions(eng.shared_from_this());
            auto edit = permission(eng, "app.edit");
            dynamic_binder<custom_privilege_probe> binder(eng, "CustomProbe");
            binder.template constructor<>()
                .method("read", &custom_privilege_probe::read, {"filesystem", edit})
                .method("query", &custom_privilege_probe::query, edit)
                .method("lambda", [](custom_privilege_probe& self) { return self.read(); }, {edit})
                .build();
            eng.add_variadic_function("custom_count", [&eng](const std::vector<script_value>& args) -> checked_result<script_value> {
                return script_value(script_int(args.size()), &eng);
            }, {edit, "filesystem"});
            eng.execute("var probe = CustomProbe(); var saved_read = reflect::bind(probe, \"read\");");
            for (const char* code : {"probe.read()", "probe.query()", "probe.lambda()", "saved_read()", "custom_count(1, 2)"}) {
                check_eq(denied(), failure([&]() { eng.execute(code); }));
            }
            eng.enable(edit);
            check_eq(script_int(17), eng.execute("saved_read()").as_int());
            check_eq(script_int(23), eng.execute("probe.query()").as_int());
            check_eq(script_int(17), eng.execute("probe.lambda()").as_int());
            check_eq(script_int(2), eng.execute("custom_count(1, 2)").as_int());
        });

        custom_test("overloads_keep_individual_requirements", [this](auto& eng) {
            auto edit = permission(eng, "app.edit");
            auto view = permission(eng, "app.view");
            eng.add_function("action", [](script_int) { return script_int(7); }, edit);
            eng.add_function("action", [](const std::string&) { return script_int(9); }, view);
            eng.enable(view);
            check_eq(denied(), failure([&]() { eng.execute("action(1)"); }));
            check_eq(script_int(9), eng.execute("action(\"x\")").as_int());
            eng.enable(edit);
            eng.disable(view);
            check_eq(script_int(7), eng.execute("action(1)").as_int());
            check_eq(denied("app.view"), failure([&]() { eng.execute("action(\"x\")"); }));
        });

        custom_test("reentrant_revoke_allows_started_call_only", [this](auto& eng) {
            auto edit = permission(eng, "app.edit");
            int calls = 0;
            eng.add_function("edit", [&]() {
                ++calls;
                eng.disable(edit);
                return script_int(42);
            }, edit);
            eng.enable(edit);
            check_eq(script_int(42), eng.execute("edit()").as_int());
            check_eq(denied(), failure([&]() { eng.execute("edit()"); }));
            check_eq(1, calls);
        });

        custom_test("named_denial_is_checked_catchable_and_printable", [this](auto& eng) {
            auto edit = permission(eng, "app.edit");
            auto result = eng.require_privilege(edit);
            check_false(bool(result));
            check_eq(std::string("Insufficient privilege: app.edit."), eng.format_error(result));
            eng.add_function("edit", []() {}, edit);
            auto output = std::make_shared<std::ostringstream>();
            eng.set_output_stream(output);
            check_eq(std::string("Insufficient privilege: app.edit."), eng.execute(
                "var caught = \"\"; try { edit(); } catch (e) { caught = e; print(e); } caught;").as_string());
            check_eq(std::string("Insufficient privilege: app.edit.\n"), output->str());
        });

        custom_test("reflection_accepts_mixed_names_and_flags", [this](auto& eng) {
            auto edit = permission(eng, "app.edit");
            eng.enable(edit);
            check_true(eng.execute("reflect::is_enabled(jai::filesystem, \"app.edit\", \"filesystem\")").as_bool());
            eng.disable(edit);
            check_false(eng.execute("reflect::is_enabled(jai::filesystem, \"app.edit\")").as_bool());
            check_false(failure([&]() { eng.execute("reflect::is_enabled(\"app.edit\", -1)"); }).empty());
        });

        custom_test("bound_requirements_survive_registry_growth_beyond_sixty_four", [this](auto& eng) {
            auto first = permission(eng, "app.first");
            std::vector<std::string> names{first};
            eng.add_function("first", []() { return script_int(42); }, first);
            for (int i = 0; i < 130; ++i) {
                auto next = permission(eng, "app.extra_" + std::to_string(i));
                check_false(eng.is_enabled(next));
                names.push_back(next);
            }
            std::vector<std::string_view> all(names.begin(), names.end());
            eng.enable(all);
            check_true(eng.is_enabled(all));
            check_eq(script_int(42), eng.execute("first()").as_int());
            eng.disable(first);
            check_false(eng.is_enabled(all));
            check_eq(denied("app.first"), failure([&]() { eng.execute("first()"); }));
        });

        custom_test("queries_never_register_or_intern_names", [this](auto& eng) {
            auto edit = permission(eng, "app.edit");
            const auto name_id = detail::engine_access::symbolize(eng, "app.edit");
            check_eq(name_id, *detail::engine_access::get_symbolizer(eng)->find("app.edit"));
            check_false(detail::engine_access::get_symbolizer(eng)->find("app.unknown").has_value());
            check_false(eng.is_enabled(std::string_view("app.unknown")));
            check_false(eng.execute("reflect::is_enabled(\"app.unknown\")").as_bool());
            check_false(detail::engine_access::get_symbolizer(eng)->find("app.unknown").has_value());
            eng.enable(edit);
            auto* symbols = detail::engine_access::get_symbolizer(eng);
            struct thaw_guard {
                string_symbolizer* symbols;
                ~thaw_guard() { symbols->set_frozen(false); }
            } thaw{symbols};
            symbols->set_frozen(true);
            check_true(eng.is_enabled(edit));
            check_true(eng.is_enabled(std::string_view("app.edit")));
            check_false(eng.is_enabled(std::string_view("app.unknown")));
            check_false(symbols->find("app.unknown").has_value());
        });

        custom_test("active_region_rejects_all_mutation_before_interning", [this](auto& eng) {
            auto edit = permission(eng, "app.edit");
            auto& state = detail::engine_access::parallel_state(eng);
            detail::parallel_region_table region;
            struct region_guard {
                detail::parallel_engine_state& state;
                string_symbolizer* symbols;
                ~region_guard() { state.active_region = nullptr; symbols->set_frozen(false); }
            } restore{state, detail::engine_access::get_symbolizer(eng)};
            state.active_region = &region;
            detail::engine_access::get_symbolizer(eng)->set_frozen(true);
            const std::string expected = "Cannot change privileges during a parallel region";
            check_eq(expected, failure([&]() { eng.enable(edit); }));
            check_eq(expected, failure([&]() { eng.disable({edit, "filesystem"}); }));
            check_eq(expected, failure([&]() { eng.register_privilege("app.new_in_region"); }));
            check_false(detail::engine_access::get_symbolizer(eng)->find("app.new_in_region").has_value());
            check_false(eng.is_enabled(edit));
            check_true(eng.is_enabled(jai::filesystem));
        });

        custom_test("unknown_names_do_not_register_or_partially_change_grants", [this](auto& eng) {
            auto edit = permission(eng, "app.edit");
            const std::initializer_list<std::string_view> invalid = {edit, "app.unknown"};
            check_throws([&]() { eng.enable(invalid); });
            check_false(eng.is_enabled(edit));
            eng.enable(edit);
            check_throws([&]() { eng.disable(invalid); });
            check_true(eng.is_enabled(edit));
            auto result = eng.require_privilege(invalid);
            check_false(bool(result));
            check_eq(std::string("Unknown privilege name"), eng.format_error(result));
            check_throws([&]() { eng.add_function("invalid", []() {}, invalid); });
            check_false(detail::engine_access::get_symbolizer(eng)->find("app.unknown").has_value());
        });

        custom_test("builtin_name_is_an_alias_and_empty_name_rejected", [this](auto& eng) {
            auto builtin = permission(eng, "filesystem");
            check_true(eng.is_enabled(builtin));
            eng.disable(builtin);
            eng.register_privilege("filesystem");
            check_false(eng.is_enabled("filesystem"));
            check_false(eng.is_enabled(jai::filesystem));
            check_throws([&]() { eng.register_privilege(""); });
        });

        custom_test("binding_interns_owned_names_not_caller_views", [this](auto& eng) {
            {
                std::string name = "app.ephemeral";
                eng.register_privilege(name);
                eng.add_function("ephemeral", []() { return script_int(42); }, {name, "filesystem"});
                name.assign(2048, 'x');
            }
            check_eq(denied("app.ephemeral"), failure([&]() { eng.execute("ephemeral()"); }));
            eng.enable("app.ephemeral");
            check_eq(script_int(42), eng.execute("ephemeral()").as_int());
        });

        custom_test("worker_callable_copy_retains_live_gate", [this](auto& eng) {
            eng.register_privilege("app.edit");
            eng.add_function("edit", []() { return script_int(42); }, "app.edit");
            auto copy = detail::engine_access::make_parallel_host_function_copy(eng, "edit");
            eng.enable("app.edit");
            auto allowed = copy.as_function()({});
            check_true(bool(allowed));
            if (allowed) { check_eq(script_int(42), allowed.value().as_int()); }
            eng.disable("app.edit");
            auto refused = copy.as_function()({});
            check_false(bool(refused));
            check_eq(std::string("Insufficient privilege: app.edit."), eng.format_error(refused));
        });

        custom_test("nested_execute_sees_reentrant_revocation", [this](auto& eng) {
            eng.register_privilege("app.edit");
            int calls = 0;
            eng.add_function("edit", [&]() { ++calls; }, "app.edit");
            std::string nested_error;
            eng.add_function("outer", [&]() {
                eng.disable("app.edit");
                nested_error = failure([&]() { eng.execute("edit()"); });
                return script_int(42);
            }, "app.edit");
            eng.enable("app.edit");
            check_eq(script_int(42), eng.execute("outer()").as_int());
            check_eq(denied(), nested_error);
            check_eq(0, calls);
        });
    }
};

} // namespace jai::foundry::tests

using custom_privilege_tests = jai::foundry::tests::custom_privilege_tests;
FOUNDRY_REGISTER(custom_privilege_tests)
