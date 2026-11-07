#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include "../case_table.hpp"
#include <memory>
#include <optional>
#include <vector>

namespace jai::foundry::tests {

// Test class that tracks all copy/move operations
class TrackedObject {
public:
    static int instance_count;
    static int copy_count;
    static int move_count;

    int id;
    int value;

    TrackedObject(int val) : id(++instance_count), value(val) {}
    TrackedObject(const TrackedObject& other) : id(++instance_count), value(other.value) { copy_count++; }
    TrackedObject(TrackedObject&& other) noexcept : id(++instance_count), value(other.value) { move_count++; }

    void modify(int new_val) { value = new_val; }
    int get() const { return value; }

    static void reset() {
        instance_count = 0;
        copy_count = 0;
        move_count = 0;
    }
};

int TrackedObject::instance_count = 0;
int TrackedObject::copy_count = 0;
int TrackedObject::move_count = 0;

// Copy-count pins for the engine's designed value semantics (VM parity bar):
// - Declarations clone only lvalue initializers; temporaries (ctor results) bind with no copy.
// - Assignment into variables, array elements, map entries, push() and a container literal's
//   lvalue elements deep-clones (1 C++ copy via the class copy_function), except shared_ptr-typed
//   values which share; a literal's temporary elements move.
// - A by-value lambda capture copies as a declaration from the variable does (1 C++ copy); [&] and a
//   program-scope [=] (globals are read when the lambda runs) copy nothing.
// - Calls into C++ never deep-clone args script-side; a by-value C++ param costs exactly the
//   one copy function_binder makes, a T& param costs zero.
class value_semantics_tests : public suite {
public:
    value_semantics_tests() : suite("Value Semantics") {}

    void pre_test() override {
        TrackedObject::reset();
    }

    void forge_tests() override {
        test("temporaries_reads_and_member_calls_copy_nothing", [this]() {
            check_copies({
                {{"auto obj = TrackedObject(42);"}, 0},
                {{"auto obj = TrackedObject(42); obj;"}, 0},
                {{"auto obj = TrackedObject(42); obj.value"}, 0, 42},
                {{"auto obj = TrackedObject(42); obj.get()"}, 0, 42},
                {{"TrackedObject(42).value"}, 0, 42},
                {{"auto obj = TrackedObject(42); obj.modify(10); obj.modify(20);", "obj.get()"}, 0, 20},
                {{"auto obj = TrackedObject(42); obj.value += 10;", "obj.value"}, 0, 52},
            });
        });

        test("initializing_or_storing_from_an_lvalue_copies_once_per_store", [this]() {
            check_copies({
                {{"auto obj1 = TrackedObject(42); auto obj2 = obj1;"}, 1},
                {{"auto obj1 = TrackedObject(42); auto obj2 = obj1;", "obj1.modify(99);", "obj2.get()"}, 1, 42},
                {{"auto obj1 = TrackedObject(42); auto obj2 = obj1; auto obj3 = obj2;"}, 2},
                {{"auto obj = TrackedObject(42); auto arr = []; arr.push(obj);"}, 1},
                {{"auto obj = TrackedObject(42); auto arr = [null]; arr[0] = obj;"}, 1},
                {{"auto obj = TrackedObject(42);", "auto m = {};", "m[\"key\"] = obj;", "m[\"key\"].value"}, 1, 42},
            });
        });

        test("a_lambda_copies_only_what_it_captures_by_name_by_value", [this]() {
            check_copies({
                {{"auto obj = TrackedObject(42); auto f = [obj]() { return obj.get(); }; f()"}, 1, 42},
                {{"auto obj = TrackedObject(42); auto f = [&obj]() { return obj.get(); }; f()"}, 0, 42},
                {{"auto obj = TrackedObject(42); auto f = [=]() { return obj.get(); }; f()"}, 0, 42},
                {{"auto obj = TrackedObject(42); auto f = [&]() { return obj.get(); }; f()"}, 0, 42},
            });
        });

        test("a_host_call_copies_only_into_a_by_value_parameter", [this]() {
            check_copies({
                {{"auto obj = TrackedObject(42); take_by_value(obj)"}, 1, 42},
                {{"auto obj = TrackedObject(42); take_by_ref(obj)"}, 0, 42},
                {{"auto obj = TrackedObject(42); modify_by_ref(obj, 99);", "obj.get()"}, 0, 99},
            });
        });

        test("container_literals_copy_their_named_elements_and_take_their_temporaries", [this]() {
            check_copies({
                {{"auto obj = TrackedObject(42); auto arr = [obj, obj, obj]; arr[0].value = 7; "
                  "obj.value * 10000 + arr[0].value * 100 + arr[1].value"}, 3, 420742},
                {{"auto obj = TrackedObject(42); auto m = {\"a\": obj, \"b\": obj}; m[\"a\"].value = 7; "
                  "obj.value * 10000 + m[\"a\"].value * 100 + m[\"b\"].value"}, 2, 420742},
                // the inner literals are temporaries of the outer one: only the named elements copy
                {{R"(
                    auto obj = TrackedObject(42);
                    var nest = [[obj, obj], {"k": obj}];
                    array<TrackedObject> typed = [obj, obj];
                    nest[0][0].value = 1;
                    typed[0].value = 2;
                    obj.value * 1000000 + nest[0][1].value * 10000 + nest[1]["k"].value * 100 + typed[1].value
                )"}, 5, 42424242},
                {{R"(
                    function make(int v) { return TrackedObject(v); }
                    var arr = [TrackedObject(1), make(2), [TrackedObject(3)]];
                    var m = {"a": TrackedObject(4), "b": make(5)};
                    arr[0].value + arr[1].value + arr[2][0].value + m["a"].value + m["b"].value
                )"}, 0, 15},
                {{R"(
                    auto obj = TrackedObject(42);
                    function poke() { obj.value = 9; return 0; }
                    var arr = [obj, poke()];
                    arr[0].value * 100 + obj.value
                )"}, 1, 4209},
                {{R"(
                    class Squad {
                        TrackedObject lead = TrackedObject(1);
                        array<TrackedObject> pair = [lead, lead];
                    }
                    Squad s = Squad();
                    s.pair[0].value = 3;
                    s.lead.value * 100 + s.pair[0].value * 10 + s.pair[1].value
                )"}, 2, 131},
            });
        });

        test("literal_handles_share", [this]() {
            auto eng = make_engine();
            register_tracked_object(*eng);

            auto result = eng->execute(R"(
                shared_ptr<TrackedObject> h = new TrackedObject(1);
                var arr = [h, h];
                var m = {"a": h};
                arr[0].value = 5;
                h.value * 100 + arr[1].value * 10 + m["a"].value
            )");

            // a handle copies as a shared_ptr does: every element names the one object
            check_eq(0, TrackedObject::copy_count);
            check_eq(555, result.as<int>());

            struct HandleSource {
                std::shared_ptr<TrackedObject> obj = std::make_shared<TrackedObject>(1);
                std::shared_ptr<TrackedObject> get_object() { return obj; }
            };
            dynamic_binder<HandleSource>(*eng, "HandleSource")
                .constructor<>()
                .method("get_object", &HandleSource::get_object)
                .build();
            auto host = eng->execute(R"(
                auto source = HandleSource();
                shared_ptr<TrackedObject> g = source.get_object();
                var held = [g, g];
                held[0].value = 4;
                g.value * 10 + held[1].value
            )");
            check_eq(0, TrackedObject::copy_count);
            check_eq(44, host.as<int>());
        });

        test("method_call_on_cpp_object_property", [this]() {
            auto eng = make_engine();
            register_tracked_object(*eng);

            class ObjectContainer {
            public:
                std::shared_ptr<TrackedObject> obj;

                ObjectContainer() : obj(std::make_shared<TrackedObject>(100)) {}

                std::shared_ptr<TrackedObject> get_object() { return obj; }
            };

            dynamic_binder<ObjectContainer>(*eng, "ObjectContainer")
                .constructor<>()
                .method("get_object", &ObjectContainer::get_object)
                .build();

            eng->execute("auto container = ObjectContainer();");
            auto result = eng->execute("container.get_object().value");

            // shared_ptr returns share - no script copies
            check_eq(0, TrackedObject::copy_count);
            check_eq(100, result.as<int>());
        });
    }

private:
    // the steps run in order on one engine; the copy count is read after the last, which yields `value` when one is given
    struct copy_row {
        std::vector<const char*> steps;
        int copies;
        std::optional<int> value;
        const char* code = steps.back();
    };

    void check_copies(const std::vector<copy_row>& a_rows) {
        check_each(a_rows, [&](const copy_row& row) {
            TrackedObject::reset();
            auto eng = make_engine();
            register_tracked_object(*eng);
            for (size_t i = 0; i + 1 < row.steps.size(); ++i) {
                eng->execute(row.steps[i]);
            }
            auto result = eng->execute(row.steps.back());
            check_eq(row.copies, TrackedObject::copy_count, "copies");
            if (row.value) { check_eq(*row.value, result.as<int>(), "value"); }
        });
    }

    void register_tracked_object(engine& eng) {
        dynamic_binder<TrackedObject>(eng, "TrackedObject")
            .constructor<int>()
            .method("modify", &TrackedObject::modify)
            .method("get", &TrackedObject::get)
            .property("value", &TrackedObject::value)
            .property("id", &TrackedObject::id)
            .build();
        eng.add_function("take_by_value", [](TrackedObject obj) -> int { return obj.get(); });
        eng.add_function("take_by_ref", [](TrackedObject& obj) -> int { return obj.get(); });
        eng.add_function("modify_by_ref", [](TrackedObject& obj, int val) { obj.modify(val); });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::value_semantics_tests)
