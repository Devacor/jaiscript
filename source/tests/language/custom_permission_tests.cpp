#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/reflection.hpp>
#include <jaiscript/detail/parallel_transform.hpp>
#include <sstream>

namespace jai::foundry::tests {

enum class app_permission : std::int32_t { edit = jai::permissions::count, publish, view, unnamed, far_slot = 200 };

// any enum is a permission, its value the id: these spill past the ids kept in place
enum class spilled_permission : std::int64_t { large = 100000, negative = -3, widest = 2147483647 };

struct custom_permission_probe {
    script_int read() { return 17; }
    script_int query() const { return 23; }
    static script_int make() { return 31; }
};

class custom_permission_tests : public suite {
public:
    custom_permission_tests() : suite("Custom Permission") {}

    template<class F>
    void custom_test(const char* name, F body) {
        test(name, [this, body]() {
            auto eng = make_engine();
            eng->register_permission(app_permission::edit, "app.edit");
            eng->register_permission(app_permission::publish, "app.publish");
            eng->register_permission(app_permission::view, "app.view");
            body(*eng);
        });
    }

    template<class F>
    static std::string failure(F&& operation) {
        try { operation(); }
        catch (const std::exception& error) { return error.what(); }
        return {};
    }

    static std::string denied(const char* name = "app.edit") {
        return std::string("Insufficient permission: ") + name + ".";
    }

    void forge_tests() override {
        custom_test("host_permissions_start_disabled_and_grants_are_engine_local", [this](engine& eng) {
            check_false(eng.is_enabled(app_permission::edit));
            check_true(eng.is_enabled(jai::permissions::filesystem, jai::permissions::scoped_template));
            check_false(eng.is_enabled(jai::permissions::network));
            auto other = make_engine();
            other->enable(app_permission::edit);
            eng.add_function("edit", []() { return script_int(42); }, app_permission::edit);
            other->add_function("edit", []() { return script_int(42); }, app_permission::edit);
            check_eq(denied(), failure([&]() { eng.execute("edit()"); }));
            check_eq(script_int(42), other->execute("edit()").as_int());
            other->disable(app_permission::edit);
            check_eq(std::string("Insufficient permission: 6."), failure([&]() { other->execute("edit()"); }));
            eng.enable(app_permission::edit);
            check_eq(script_int(42), eng.execute("edit()").as_int());
            check_true(eng.is_enabled(app_permission::edit, jai::permissions::filesystem));
        });

        custom_test("mixed_requirements_are_all_required_and_the_lowest_missing_slot_is_named", [](engine& eng) {
            int calls = 0;
            eng.add_function("publish", [&]() { return script_int(++calls); }, app_permission::publish, jai::permissions::filesystem, app_permission::edit, app_permission::publish);
            check_eq(denied(), failure([&]() { eng.execute("publish()"); }));
            eng.enable(app_permission::edit);
            check_eq(denied("app.publish"), failure([&]() { eng.execute("publish()"); }));
            eng.enable(app_permission::publish);
            check_eq(script_int(1), eng.execute("publish()").as_int());
            eng.disable(jai::permissions::filesystem, app_permission::edit);
            check_eq(denied("filesystem"), failure([&]() { eng.execute("publish()"); }));
            eng.enable(jai::permissions::filesystem, app_permission::edit, app_permission::publish);
            check_eq(script_int(2), eng.execute("publish()").as_int());
            eng.disable(app_permission::edit, app_permission::publish);
            check_false(eng.is_enabled(app_permission::edit));
            check_false(eng.is_enabled(app_permission::publish));
            check_true(eng.is_enabled(jai::permissions::filesystem));
            check_eq(2, calls);
        });

        custom_test("options_are_a_list_in_any_order", [](engine& eng) {
            eng.add_function("try_edit", []() { return true; }, jai::nodiscard, app_permission::edit, jai::serial);
            eng.add_variadic_function("count_edit", [](jai::engine&, std::span<const jai::script_value> args) { return static_cast<script_int>(args.size()); },
                jai::serial, app_permission::edit, jai::nodiscard);
            check_true(eng.is_nodiscard_function("try_edit") && eng.is_nodiscard_function("count_edit"));
            check_true(eng.check("try_edit();\nprint(1);").format().find("the result of 'try_edit' is discarded") != std::string::npos);
            check_eq(denied(), failure([&]() { eng.execute("bool b = try_edit(); b;"); }));
            eng.enable(app_permission::edit);
            check_true(eng.execute("bool b = try_edit(); b;").as_bool());
            check_eq(script_int(2), eng.execute("int n = count_edit(1, 2); n;").as_int());
        });

        custom_test("saved_alias_and_reflection_recheck_grants", [](engine& eng) {
            stdlib::register_reflection_functions(eng.shared_from_this());
            int calls = 0;
            eng.add_function("edit", [&]() { return script_int(++calls); }, app_permission::edit);
            eng.execute("var saved_edit = edit;");
            eng.enable(app_permission::edit);
            check_eq(script_int(1), eng.execute("saved_edit()").as_int());
            eng.disable(app_permission::edit);
            for (const char* code : {"edit()", "saved_edit()", "reflect::call(\"edit\")"}) {
                check_eq(denied(), failure([&]() { eng.execute(code); }));
            }
            check_eq(1, calls);
        });

        custom_test("methods_constructors_static_methods_and_variadics_share_the_guard", [](engine& eng) {
            stdlib::register_reflection_functions(eng.shared_from_this());
            dynamic_binder<custom_permission_probe> binder(eng, "CustomProbe");
            binder.template constructor<>()
                .method("read", &custom_permission_probe::read, jai::permissions::filesystem, app_permission::edit)
                .method("query", &custom_permission_probe::query, app_permission::edit)
                .method("lambda", [](custom_permission_probe& self) { return self.read(); }, app_permission::edit, jai::serial)
                .static_method("make", &custom_permission_probe::make, app_permission::edit, jai::nodiscard)
                .build();
            eng.add_variadic_function("custom_count", [&eng](const std::vector<script_value>& args) -> checked_result<script_value> {
                return script_value(script_int(args.size()), &eng);
            }, app_permission::edit, jai::permissions::filesystem);
            eng.execute("var probe = CustomProbe(); var saved_read = reflect::bind(probe, \"read\");");
            for (const char* code : {"probe.read()", "probe.query()", "probe.lambda()", "saved_read()", "custom_count(1, 2)", "int m = CustomProbe::make(); m;"}) {
                check_eq(denied(), failure([&]() { eng.execute(code); }));
            }
            eng.enable(app_permission::edit);
            check_eq(script_int(17), eng.execute("saved_read()").as_int());
            check_eq(script_int(23), eng.execute("probe.query()").as_int());
            check_eq(script_int(17), eng.execute("probe.lambda()").as_int());
            check_eq(script_int(2), eng.execute("custom_count(1, 2)").as_int());
            check_eq(script_int(31), eng.execute("int m = CustomProbe::make(); m;").as_int());
        });

        custom_test("overloads_keep_individual_requirements", [](engine& eng) {
            eng.add_function("action", [](script_int) { return script_int(7); }, app_permission::edit);
            eng.add_function("action", [](const std::string&) { return script_int(9); }, app_permission::view);
            eng.enable(app_permission::view);
            check_eq(denied(), failure([&]() { eng.execute("action(1)"); }));
            check_eq(script_int(9), eng.execute("action(\"x\")").as_int());
            eng.enable(app_permission::edit);
            eng.disable(app_permission::view);
            check_eq(script_int(7), eng.execute("action(1)").as_int());
            check_eq(denied("app.view"), failure([&]() { eng.execute("action(\"x\")"); }));
        });

        custom_test("reentrant_revoke_allows_started_call_only", [](engine& eng) {
            int calls = 0;
            eng.add_function("edit", [&]() {
                ++calls;
                eng.disable(app_permission::edit);
                return script_int(42);
            }, app_permission::edit);
            eng.enable(app_permission::edit);
            check_eq(script_int(42), eng.execute("edit()").as_int());
            check_eq(denied(), failure([&]() { eng.execute("edit()"); }));
            check_eq(1, calls);
        });

        custom_test("denial_is_checked_catchable_and_printable_and_an_unnamed_one_shows_its_slot", [](engine& eng) {
            auto result = eng.require_permission(app_permission::edit);
            check_false(bool(result));
            check_eq(std::string("Insufficient permission: app.edit."), eng.format_error(result));
            check_eq(std::string("Insufficient permission: 9."), eng.format_error(eng.require_permission(jai::permissions::filesystem, app_permission::unnamed)));
            eng.add_function("edit", []() {}, app_permission::edit);
            auto output = std::make_shared<std::ostringstream>();
            eng.set_output_stream(output);
            check_eq(std::string("Insufficient permission: app.edit."), eng.execute(
                "var caught = \"\"; try { edit(); } catch (e) { caught = e; print(e); } caught;").as_string());
            check_eq(std::string("Insufficient permission: app.edit.\n"), output->str());
            eng.register_permission(app_permission::unnamed, "app.renamed");
            check_eq(std::string("Insufficient permission: app.renamed."), eng.format_error(eng.require_permission(app_permission::unnamed)));
        });

        custom_test("scripts_ask_by_id_through_jai_permissions", [](engine& eng) {
            eng.enable(app_permission::edit);
            check_true(eng.execute("jai::permissions::is_enabled(jai::permissions::filesystem, 6, jai::permissions::scoped_template)").as_bool());
            check_false(eng.execute("jai::permissions::is_enabled(jai::permissions::filesystem, 7)").as_bool());
            check_false(eng.execute("jai::permissions::is_enabled(jai::permissions::network)").as_bool());
            check_eq(script_int(6), eng.execute("jai::permissions::count;").as_int());
            check_eq(script_int(15), eng.execute("jai::permissions::filesystem + jai::permissions::scoped_template + jai::permissions::network + "
                "jai::permissions::debug + jai::permissions::trusted + jai::permissions::untrusted;").as_int());
            const std::string expects = "jai::permissions::is_enabled expects permission ids";
            check_eq(expects, failure([&]() { eng.execute("jai::permissions::is_enabled(\"app.edit\")"); }));
            check_eq(expects, failure([&]() { eng.execute("jai::permissions::is_enabled(2147483648)"); }));
        });

        custom_test("the_convention_permissions_start_disabled_and_name_themselves", [](engine& eng) {
            check_false(eng.is_enabled(jai::permissions::debug) || eng.is_enabled(jai::permissions::trusted) || eng.is_enabled(jai::permissions::untrusted));
            eng.add_function("metrics", []() { return script_int(5); }, jai::permissions::debug);
            eng.add_function("bundled", []() { return script_int(6); }, jai::permissions::trusted, jai::permissions::untrusted);
            check_eq(std::string("Insufficient permission: debug."), failure([&]() { eng.execute("metrics()"); }));
            eng.enable(jai::permissions::debug, jai::permissions::untrusted);
            check_eq(script_int(5), eng.execute("metrics()").as_int());
            check_eq(std::string("Insufficient permission: trusted."), failure([&]() { eng.execute("bundled()"); }));
            eng.disable(jai::permissions::untrusted);
            eng.enable(jai::permissions::trusted);
            check_eq(std::string("Insufficient permission: untrusted."), failure([&]() { eng.execute("bundled()"); }));
            check_eq(std::string("A built-in permission keeps its name"), failure([&]() { eng.register_permission(jai::permissions::debug, "dbg"); }));
            check_eq(std::string("Permission name must not be empty"), failure([&]() { eng.register_permission(app_permission::edit, ""); }));
        });

        custom_test("every_int32_id_is_a_permission_the_low_ones_in_place_the_rest_spilled", [](engine& eng) {
            eng.add_function("far", []() { return script_int(42); }, app_permission::far_slot);
            check_eq(std::string("Insufficient permission: 200."), failure([&]() { eng.execute("far()"); }));
            eng.enable(app_permission::far_slot);
            check_true(eng.execute("jai::permissions::is_enabled(200)").as_bool());
            check_eq(script_int(42), eng.execute("far()").as_int());

            eng.add_function("spilled", []() { return script_int(43); }, spilled_permission::large, app_permission::edit, spilled_permission::negative);
            check_eq(std::string("Insufficient permission: -3."), failure([&]() { eng.execute("spilled()"); }));
            eng.enable(spilled_permission::negative);
            check_eq(denied(), failure([&]() { eng.execute("spilled()"); }));
            eng.enable(app_permission::edit);
            check_eq(std::string("Insufficient permission: 100000."), failure([&]() { eng.execute("spilled()"); }));
            eng.register_permission(spilled_permission::large, "app.large");
            check_eq(std::string("Insufficient permission: app.large."), failure([&]() { eng.execute("spilled()"); }));
            eng.enable(spilled_permission::large, spilled_permission::widest);
            check_eq(script_int(43), eng.execute("spilled()").as_int());
            check_true(eng.is_enabled(spilled_permission::widest, spilled_permission::large, spilled_permission::negative));
            check_true(eng.execute("jai::permissions::is_enabled(-3, 100000, 2147483647)").as_bool());
            eng.disable(spilled_permission::large);
            check_false(eng.is_enabled(spilled_permission::large));
            check_true(eng.is_enabled(spilled_permission::negative, spilled_permission::widest));
            check_eq(std::string("Insufficient permission: app.large."), failure([&]() { eng.execute("spilled()"); }));
        });

        custom_test("active_region_rejects_all_mutation", [](engine& eng) {
            auto& state = detail::engine_access::parallel_state(eng);
            detail::parallel_region_table region;
            struct region_guard {
                detail::parallel_engine_state& state;
                string_symbolizer* symbols;
                ~region_guard() { state.active_region = nullptr; symbols->set_frozen(false); }
            } restore{state, detail::engine_access::get_symbolizer(eng)};
            state.active_region = &region;
            detail::engine_access::get_symbolizer(eng)->set_frozen(true);
            const std::string expected = "Cannot change permissions during a parallel region";
            check_eq(expected, failure([&]() { eng.enable(app_permission::edit); }));
            check_eq(expected, failure([&]() { eng.disable(app_permission::edit, jai::permissions::filesystem); }));
            check_eq(expected, failure([&]() { eng.register_permission(app_permission::unnamed, "app.new_in_region"); }));
            check_false(detail::engine_access::get_symbolizer(eng)->find("app.new_in_region").has_value());
            check_false(eng.is_enabled(app_permission::edit));
            check_true(eng.is_enabled(jai::permissions::filesystem));
        });

        custom_test("worker_callable_copy_retains_live_gate_and_names_an_unnamed_slot", [](engine& eng) {
            eng.add_function("edit", []() { return script_int(42); }, app_permission::edit);
            eng.add_function("plain", []() { return script_int(43); }, app_permission::unnamed);
            auto copy = detail::engine_access::make_parallel_host_function_copy(eng, "edit");
            auto unnamed_copy = detail::engine_access::make_parallel_host_function_copy(eng, "plain");
            eng.enable(app_permission::edit);
            auto allowed = copy.as_function()({});
            check_true(bool(allowed));
            if (allowed) { check_eq(script_int(42), allowed.value().as_int()); }
            eng.disable(app_permission::edit);
            auto refused = copy.as_function()({});
            check_false(bool(refused));
            check_eq(std::string("Insufficient permission: app.edit."), eng.format_error(refused));
            auto* symbols = detail::engine_access::get_symbolizer(eng);
            struct thaw_guard {
                string_symbolizer* symbols;
                ~thaw_guard() { symbols->set_frozen(false); }
            } thaw{symbols};
            symbols->set_frozen(true);
            check_eq(std::string("Insufficient permission: 9."), eng.format_error(unnamed_copy.as_function()({})));
        });

        custom_test("nested_execute_sees_reentrant_revocation", [](engine& eng) {
            int calls = 0;
            eng.add_function("edit", [&]() { ++calls; }, app_permission::edit);
            std::string nested_error;
            eng.add_function("outer", [&]() {
                eng.disable(app_permission::edit);
                nested_error = failure([&]() { eng.execute("edit()"); });
                return script_int(42);
            }, app_permission::edit);
            eng.enable(app_permission::edit);
            check_eq(script_int(42), eng.execute("outer()").as_int());
            check_eq(denied(), nested_error);
            check_eq(0, calls);
        });
    }
};

} // namespace jai::foundry::tests

using custom_permission_tests = jai::foundry::tests::custom_permission_tests;
FOUNDRY_REGISTER(custom_permission_tests)
