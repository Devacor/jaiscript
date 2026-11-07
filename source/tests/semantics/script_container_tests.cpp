#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/bound_array.hpp>
#include <jaiscript/core/bound_map.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <chrono>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

struct boundary_creature { script_int hp = 3; };   // bound class for the boundary binding test

// Test widget class to verify deep copy behavior
struct TestWidget {
    std::string name;
    int value;
    static int instance_count;
    static int copy_count;
    
    TestWidget(const std::string& n, int v) : name(n), value(v) {
        instance_count++;
    }
    
    TestWidget(const TestWidget& other) : name(other.name), value(other.value) {
        instance_count++;
        copy_count++;
    }
    
    ~TestWidget() {
        instance_count--;
    }
    
    static void reset_counts() {
        instance_count = 0;
        copy_count = 0;
    }
};

int TestWidget::instance_count = 0;
int TestWidget::copy_count = 0;

class script_container_tests : public suite {
public:
    script_container_tests() : suite("script_container_tests") {}
    
    void pre_test() override {
        TestWidget::reset_counts();
    }
    
    void forge_tests() override {
        test("a_bound_array_views_its_array_by_reference_and_owns_a_copy_by_value", [this]() {
            auto eng = make_engine();
            script_value arr_val = eng->make_array();
            auto& arr = arr_val.as_array();
            arr.push_back(eng->make_value(1));
            arr.push_back(eng->make_value(2));
            arr.push_back(eng->make_value(3));

            bound_array<int> arr_ref(arr_val, eng.get());
            check_eq(arr_ref.size(), 3u);
            check(!arr_ref.is_owned());
            int sum = 0;
            for (int val : arr_ref) {
                sum += val;
            }
            check_eq(sum, 6);
            auto it = arr_ref.begin();
            check_eq(*it, 1);
            ++it;
            check_eq(*it, 2);

            bound_array<int> arr_copy(arr_val);
            check(arr_copy.is_owned());
            arr_copy.push_back(30);
            check_eq(arr_val.as_array().size(), 3u);
            check_eq(arr_copy.size(), 4u);

            bound_array<int> copy_of_ref(arr_ref);
            check(copy_of_ref.is_owned());
            copy_of_ref.push_back(200);
            check_eq(arr_ref.size(), 3u);
            check_eq(copy_of_ref.size(), 4u);

            auto by_value = [](bound_array<int> a_arr) {
                a_arr.push_back(999);
                return a_arr.size();
            };
            check_eq(by_value(arr_ref), 4u);
            check_eq(arr_ref.size(), 3u);

            auto by_ref = [](bound_array<int>& a_arr) {
                a_arr.push_back(777);
                return a_arr.size();
            };
            check_eq(by_ref(arr_ref), 4u);
            check_eq(arr_ref.size(), 4u);
            check_eq(arr_val.as_array().size(), 4u);

            auto by_const_ref = [](const bound_array<int>& a_arr) {
                int result = 0;
                for (int val : a_arr) {
                    result += val;
                }
                return result;
            };
            check_eq(by_const_ref(arr_ref), 1 + 2 + 3 + 777);

            check_eq(arr_ref[0], 1);
            arr_ref[0] = 100;
            check_eq(arr_ref[0], 100);
            check_eq(arr_val.as_array()[0].as<int>(), 100);
        });

        // move and move-assignment carry engine_ref_: the moved-to array builds its next element through it
        test("move_preserves_engine_ref", [this]() {
            auto eng = make_engine();
            script_value holder = script_value::make_array(nullptr, eng.get());
            auto& backing = const_cast<std::vector<script_value>&>(holder.as_array());

            bound_array<script_int> ref(holder, eng.get());
            bound_array<script_int> moved = std::move(ref);
            moved.push_back((script_int)7);
            check_eq((size_t)1, backing.size());
            check(backing[0].get_type_info().get() == detail::engine_access::get_type_info_int(*eng), "moved bound_array lost its engine_ref_");

            bound_array<script_int> assigned(holder, eng.get());
            assigned = std::move(moved);
            assigned.push_back((script_int)9);
            check(backing.back().get_type_info().get() == detail::engine_access::get_type_info_int(*eng), "move-assigned bound_array lost its engine_ref_");
        });

        test("bound_array_custom_objects", [this]() {
            auto eng = make_engine();
            
            dynamic_binder<TestWidget>(*eng, "TestWidget")
                .constructor<std::string, int>()
                .property("name", &TestWidget::name)
                .property("value", &TestWidget::value)
                .build();
            
            TestWidget::reset_counts();
            
            script_value arr_val = eng->make_array();
            auto& arr = arr_val.as_array();
            arr.push_back(eng->make_object<TestWidget>("Button", 100));
            arr.push_back(eng->make_object<TestWidget>("Label", 200));
            
            check_eq(TestWidget::instance_count, 2);
            check_eq(TestWidget::copy_count, 0);
            
            bound_array<TestWidget> widgets_ref(arr_val, eng.get());
            check_eq(TestWidget::copy_count, 0);

            // By value parameter - should trigger copies
            auto process = [](bound_array<TestWidget> widgets) {
                return widgets.size();
            };
            
            size_t count = process(widgets_ref);
            check_eq(count, 2u);
            check_eq(TestWidget::copy_count, 2);  // Deep copy of elements
        });
        
        test("a_bound_map_views_its_map_by_reference_and_owns_a_copy_by_value", [this]() {
            auto eng = make_engine();
            script_value map_val = eng->make_map();
            auto& jmap = map_val.as_map();
            jmap.insert_or_assign(eng->make_value("x"), eng->make_value(100));
            jmap.insert_or_assign(eng->make_value("y"), eng->make_value(200));

            bound_map<std::string, int> map_copy(map_val);
            check_eq(map_copy.size(), 2u);
            check(map_copy.is_owned());
            map_copy.insert("gamma", 30);
            check_eq(map_val.as_map().size(), 2u);
            check_eq(map_copy.size(), 3u);

            bound_map<std::string, int> map(map_val.as_map(), eng.get());
            check_eq(map.size(), 2u);
            check(!map.is_owned());
            check_eq(map.at("x"), 100);
            check_eq(map.at("y"), 200);
            check(map.contains("x"));
            check(!map.contains("z"));
            int sum = 0;
            for (const auto& [key, value] : map) {
                sum += value;
            }
            check_eq(sum, 300);

            map["x"] = 1000;
            check_eq(map.at("x"), 1000);
            map["z"] = 300;
            check_eq(map.size(), 3u);
            check_eq(map.at("z"), 300);
            map.insert("three", 3);
            check_eq(map_val.as_map().size(), 4u);
            check_eq(map.size(), 4u);
        });

        test("bound_array_zero_copy_shares_the_node", [this]() {
            auto eng = make_engine();

            script_value arr_val = eng->make_array();
            auto& arr = arr_val.as_array();
            for (int i = 0; i < 10000; ++i) {
                arr.push_back(eng->make_value(i));
            }

            // the reference view shares the node (a write lands in the value), the value form is its own copy
            bound_array<int> arr_ref(arr_val, eng.get());
            int sum = 0;
            for (int val : arr_ref) {
                sum += val;
            }
            bound_array<int> arr_copy(arr_val);
            check_false(arr_ref.is_owned(), "the reference view is not a copy");
            check_true(arr_copy.is_owned(), "the value form owns its copy");
            arr_ref[0] = 7;
            check_eq((int64_t)7, arr_val.as_array()[0].as_int(), "a write through the view lands in the array");
            check_eq(0, static_cast<int>(arr_copy[0]), "the copy does not see it");

            check_eq(sum, (10000 * 9999) / 2);
        });

        // ===== Typed representation at the binding boundary =====
        // array<int>/array<float> ARE raw buffers; bound_array<T>& mutates them in place
        // through the node, and the script_value vector view exists only for array<var>.
        test("typed_buffer_bound_array_mutates_in_place", [this]() {
            auto eng = make_engine();
            eng->add_function("bump", [](bound_array<int64_t>& v) {
                v[0] = v[0] + 10;
                v.push_back(100);
            });
            auto r = eng->execute(R"(
                array<int> a = [1, 2, 3];
                bump(a);
                a[0] * 1000 + a[3] + a.size()
            )");
            check_eq((int64_t)(11000 + 100 + 4), r.as_int());
            auto typed = eng->execute("array<int> b = [4, 5]; bump(b); b");
            check(typed.unchecked_array_node()->kind() == script_array::kind_t::i64, "buffer stays typed after C++ mutation");
        });

        test("typed_buffer_vector_view_error_names_the_element_type", [this]() {
            auto eng = make_engine();
            auto view_error = [&](const char* script) {
                try { (void)eng->execute(script).as_array(); } catch (const std::exception& e) { return std::string(e.what()); }
                return std::string();
            };
            const std::string strings = view_error("array<string> s = [\"a\"]; s");
            check_true(strings.find("array<string>") != std::string::npos, strings);
            check_true(strings.find("std::vector<std::string>") != std::string::npos, strings);
            check_true(view_error("array<float> f = [1.5]; f").find("array<float>") != std::string::npos);
            check_true(view_error("array<uint8> b = [1]; b").find("array<uint8>") != std::string::npos);
            check_true(view_error("array<int> i = [1]; i").find("std::vector<int64_t>") != std::string::npos);
            check_eq(std::string(), view_error("array<var> v = [1]; v"));
        });

        test("script_maps_extract_to_every_registered_std_map", [this]() {
            auto eng = make_engine();
            auto ints = eng->execute(R"(map<string, int> k = {"orc": 3, "troll": 1}; k)").as<std::map<std::string, int64_t>>();
            check_eq(size_t(2), ints.size());
            check_eq(int64_t(3), ints["orc"]);
            auto names = eng->execute(R"(map<string, string> n = {"a": "x"}; n)").as<std::map<std::string, std::string>>();
            check_eq(std::string("x"), names["a"]);
            auto by_id = eng->execute(R"(map<int, string> b = {7: "seven"}; b)").as<std::map<int64_t, std::string>>();
            check_eq(std::string("seven"), by_id[7]);
        });

        test("typed_buffer_extracts_to_cpp_vector", [this]() {
            auto eng = make_engine();
            eng->add_function("sum_ints", [](std::vector<int64_t> v) -> int64_t {
                int64_t s = 0; for (auto n : v) { s += n; } return s;
            });
            eng->add_function("sum_floats", [](std::vector<double> v) -> double {
                double s = 0; for (auto d : v) { s += d; } return s;
            });
            check_eq((int64_t)6, eng->execute("array<int> a = [1, 2, 3]; sum_ints(a)").as_int());
            check_near(4.5, eng->execute("array<float> f = [1.5, 3.0]; sum_floats(f)").as_float(), 1e-9);
            check_eq((int64_t)6, eng->execute("array<var> v = [1, 2, 3]; sum_ints(v)").as_int());
        });

        test("class_typed_array_binds_vector_of_shared_ptr", [this]() {
            // Binding shape: array<K> of a bound C++ class hands its objects to a
            // std::vector<std::shared_ptr<K>> parameter (by value: the binder rejects
            // container references by design). The class is proven once per array (kind
            // obj), each slot's holder gives up its object; identity is shared (C++ writes
            // land in the script objects); a hybrid subclass array (different class id)
            // binds through the registry path; dynamic arrays convert per element
            auto eng = make_engine();
            dynamic_binder<boundary_creature>(*eng, "Creature").constructor<>().property("hp", &boundary_creature::hp).build();
            eng->add_function("sum_hp", [](std::vector<std::shared_ptr<boundary_creature>> cs) -> script_int {
                script_int t = 0; for (const auto& c : cs) { t += c->hp; } return t;
            });
            eng->add_function("heal_all", [](std::vector<std::shared_ptr<boundary_creature>> cs) {
                for (const auto& c : cs) { c->hp += 10; }
            });
            eng->add_function("count", [](std::vector<std::shared_ptr<boundary_creature>> cs) -> script_int {
                return static_cast<script_int>(cs.size());
            });
            check_eq((int64_t)9, eng->execute("array<Creature> cs = [Creature(), Creature(), Creature()]; sum_hp(cs)").as_int());
            check_eq((int64_t)39, eng->execute("array<Creature> cs = [Creature(), Creature(), Creature()]; heal_all(cs); sum_hp(cs)").as_int());
            check_eq((int64_t)13, eng->execute("array<Creature> cs = [Creature()]; heal_all(cs); cs[0].hp").as_int());
            check_eq((int64_t)0, eng->execute("array<Creature> none = []; count(none)").as_int());
            check_eq((int64_t)6, eng->execute("var dyn = [Creature(), Creature()]; sum_hp(dyn)").as_int());
            check_eq((int64_t)2, eng->execute("class Big : Creature { int extra = 1; Big() : super() {} } array<Big> bigs = [Big(), Big()]; count(bigs)").as_int());
            check_eq((int64_t)26, eng->execute("class Big2 : Creature { int extra = 1; Big2() : super() {} } array<Big2> bigs = [Big2(), Big2()]; heal_all(bigs); sum_hp(bigs)").as_int());
            check_eq((int64_t)13, eng->execute("class Big3 : Creature { int extra = 1; Big3() : super() {} } array<Big3> b = [Big3()]; heal_all(b); b[0].hp").as_int());
            // Shape: a base-typed array holding bound instances AND hybrid subclasses, handed over by the fast path
            check_eq((int64_t)6, eng->execute("class Sub : Creature { int extra = 1; Sub() : super() {} } array<Creature> mixed = [Creature(), Sub()]; sum_hp(mixed)").as_int());
            check_eq((int64_t)26, eng->execute("class Sub2 : Creature { int extra = 1; Sub2() : super() {} } array<Creature> mixed = [Creature(), Sub2()]; heal_all(mixed); sum_hp(mixed)").as_int());
            // a hybrid default-constructs its C++ base when its constructor names no super(...), or when it declares none
            check_eq((int64_t)1, eng->execute("class NoBase : Creature { int extra = 1; NoBase() {} } array<NoBase> nb = [NoBase()]; count(nb)").as_int(),
                     "a constructor without super() constructs its C++ base");
            check_eq((int64_t)1, eng->execute("class Plain : Creature { int extra = 1; } array<Plain> ps = [Plain()]; count(ps)").as_int(), "a constructor-less hybrid default-constructs its C++ base");
        });

        test("script_value_vector_view_is_array_var_only", [this]() {
            auto eng = make_engine();
            eng->add_function("count_vals", [](const std::vector<script_value>& v) -> int {
                return static_cast<int>(v.size());
            });
            check_eq((int64_t)2, eng->execute("array<var> b = [1, 2]; count_vals(b)").as_int());
            check_eq((int64_t)3, eng->execute("var c = [1, \"x\", 2.5]; count_vals(c)").as_int());
            // an array<int> is a raw buffer: the dynamic view is a type mismatch, not a copy
            check_throws([&]() { eng->execute("array<int> a = [1, 2, 3]; count_vals(a)"); });
            // a uniform auto literal infers array<int> (raw buffer): same rule
            check_throws([&]() { eng->execute("auto d = [1, 2, 3]; count_vals(d)"); });
            check_throws([&]() { eng->execute("auto e = [1, \"two\"]; e"); });   // mixed literal: rejected at the decl
        });

        test("bound_array_element_type_must_fit_typed_buffer", [this]() {
            auto eng = make_engine();
            eng->add_function("first_str", [](bound_array<std::string>& v) -> std::string { return v[0]; });
            eng->add_function("first_dbl", [](bound_array<double>& v) -> double { return v[0]; });
            check_eq(std::string("q"), eng->execute("array<var> s = [\"q\"]; first_str(s)").as<std::string>());
            check_near(7.0, eng->execute("array<int> a = [7]; first_dbl(a)").as_float(), 1e-9);
            check_throws([&]() { eng->execute("array<int> a = [7]; first_str(a)"); });
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::script_container_tests)