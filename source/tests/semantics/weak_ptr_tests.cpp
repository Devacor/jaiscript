#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "value_rows.hpp"
#include <algorithm>
#include <memory>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// Test class to track object lifetime
class LifetimeTracker {
public:
    static int alive_count;
    static int total_created;
    static int copy_count;

    int id;
    int value;

    LifetimeTracker(int val = 0) : id(++total_created), value(val) {
        alive_count++;
    }

    // Copy constructor - track when copies are made
    LifetimeTracker(const LifetimeTracker& other) : id(other.id), value(other.value) {
        copy_count++;
        // Note: We don't increment alive_count for copies to track only "real" objects
    }

    // Copy assignment - track when assigned
    LifetimeTracker& operator=(const LifetimeTracker& other) {
        if (this != &other) {
            id = other.id;
            value = other.value;
        }
        return *this;
    }

    ~LifetimeTracker() {
        alive_count--;
    }

    int get_value() const { return value; }
    void set_value(int v) { value = v; }

    static void reset() {
        alive_count = 0;
        total_created = 0;
        copy_count = 0;
    }
};

int LifetimeTracker::alive_count = 0;
int LifetimeTracker::total_created = 0;
int LifetimeTracker::copy_count = 0;

// Tree node for testing parent-child relationships
class TreeNode : public std::enable_shared_from_this<TreeNode> {
public:
    std::string name;
    std::weak_ptr<TreeNode> parent;  // This will be exposed to script as weak_ptr<TreeNode>
    std::vector<std::shared_ptr<TreeNode>> children;
    
    TreeNode(const std::string& n) : name(n) {}
    
    std::string get_name() const { return name; }
    
    void add_child(std::shared_ptr<TreeNode> child) {
        children.push_back(child);
        child->parent = weak_from_this();
    }
    
    std::string get_parent_name() const {
        if (auto p = parent.lock()) {
            return p->name;
        }
        return "null";
    }
    
    int child_count() const { return static_cast<int>(children.size()); }
};

class weak_ptr_tests : public suite {
public:
    weak_ptr_tests() : suite("weak_ptr Tests") {}
    
    void pre_test() override {
        // Reset static state before each test
        LifetimeTracker::reset();
    }
    
    void forge_tests() override {
        // use_count() reports the object's count: script alias copies bump it and script weak_ptr
        // expiry follows it. A script instance has one count, so cpp_ref_count() reports the same one.
        // The alias-ladder must be byte-identical on BOTH backends (the builtin
        // subtracts a per-backend transient offset — these pins hold it stable).
        test("use_count_reports_script_alias_ladder", [this]() {
            const char* src = R"(
                class T { int x = 1; }
                shared_ptr<T> a = T();
                var r1 = a.use_count();
                var u1 = a.unique();
                var b = a;
                var r2 = a.use_count();
                var u2 = a.unique();
                var c2 = b;
                var r3 = a.use_count();
                var viaAlias = c2.use_count();
                to_string(r1) + "|" + to_string(u1) + "|" + to_string(r2) + "|"
                    + to_string(u2) + "|" + to_string(r3) + "|" + to_string(viaAlias);
            )";
            std::string outputs[2];
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                stdlib::register_all(*e);
                outputs[use_vm ? 1 : 0] = e->execute(src).as<std::string>();
            }
            check_eq(std::string("1|true|2|false|3|3"), outputs[0], "interp alias ladder");
            check_eq(outputs[0], outputs[1], "alias ladder parity (calibrated offsets hold)");
        });

        test("use_count_correlates_with_weak_ptr_expiry", [this]() {
            // The count use_count() now reports is the SAME layer weak_ptr expiry
            // follows: while any alias keeps the count >= 1 the weak_ptr stays alive
            const char* src = R"(
                class T { int x = 1; }
                shared_ptr<T> a = T();
                weak_ptr<T> w = a;
                var b = a;
                to_string(a.use_count()) + ":" + to_string(w.expired());
            )";
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                stdlib::register_all(*e);
                check_eq(std::string("2:false"), e->execute(src).as<std::string>(),
                         use_vm ? "vm weak correlation" : "interp weak correlation");
            }
        });

        // transient_k is CALIBRATION, not
        // semantics — but every change to the transient-copy structure must be
        // REPORTED with direction + cause, never silently retuned (a regression
        // isn't silent). This audit pins the exact script-visible count for a
        // canonical call shape PER BACKEND, so any change to per-call copies fails
        // here and forces the report:
        //   DOWNWARD (fewer copies, e.g. stage-2 in-place binding eliding the arg
        //   slice) = a win — update the pin WITH a proof of safety in the commit
        //   (which copy was elided, why no observable aliasing change).
        //   UPWARD (more copies) = a copy regression — justify or treat as a defect.
        // Counts at this pin (transient_k: interp 2 / vm 3, use_count builtin):
        //   top-level a.use_count(): 1 alias + builtin transients - k = 1 (both).
        //   inside probe(a): interp = outer(1) + args-vector copy(1) + param slot(1)
        //   + builtin transients(2) - k(2) = 3; vm = outer(1) + param slot(1)
        //   [stack arg copy erased before the body runs] + builtin transients(3)
        //   - k(3) = 2. The cross-backend DIFFERENCE is the interp's live args
        //   vector, absorbed deliberately by design (the ladder itself stays
        //   byte-identical — see the pins above).
        test("transient_count_audit_canonical_call", [this]() {
            const char* src = R"(
                class T { int x = 1; }
                shared_ptr<T> a = T();
                function probe(auto p) -> auto { return p.use_count(); }
                var inside = probe(a);
                var top = a.use_count();
                to_string(inside) + ":" + to_string(top);
            )";
            std::string expected[2] = {"3:1", "2:1"};   // interp, vm
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                stdlib::register_all(*e);
                check_eq(expected[use_vm ? 1 : 0], e->execute(src).as<std::string>(),
                         use_vm ? "vm transient audit (report any change: copies-per-call moved)"
                                : "interp transient audit (report any change: copies-per-call moved)");
            }
        });

        test("cpp_ref_count_sees_cpp_extraction", [this]() {
            // a script instance's one count is every owner's: script aliases and a C++-side share each count
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                stdlib::register_all(*e);
                e->execute(R"(
                    class T { int x = 1; }
                    shared_ptr<T> a = T();
                    var b = a;
                )");
                check_eq((int64_t)2, e->execute("a.cpp_ref_count()").as_int(),
                         use_vm ? "vm count sees both aliases" : "interp count sees both aliases");
                auto held = e->execute("a");
                auto holder = held.get_object_holder();
                check_not_null(holder.get(), "holder");
                auto cpp_share = holder->held();   // the host's value, its holder handle and this share each own the object
                check_eq((int64_t)5, e->execute("a.cpp_ref_count()").as_int(),
                         use_vm ? "vm count sees the C++ share" : "interp count sees the C++ share");
            }
        });

        test("weak_ptr_script_class_simplified", [this]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);
            
            // Define a script class - objects are internally shared_ptr in JaiScript
            eng->execute(R"(
                class ScriptNode {
                    string name = "";
                    int value = 0;
                    weak_ptr<ScriptNode> parent = weak_ptr<ScriptNode>();
                    array<shared_ptr<ScriptNode>> children = [];

                    ScriptNode(string n, int val) {
                        name = n;
                        value = val;
                    }

                    auto add_child(shared_ptr<ScriptNode> child) {
                        // the handle names the caller's node: the parent link lands in it
                        child.parent = weak_from_this();
                        // a shared_ptr element keeps the handle (a ScriptNode element would hold a copy)
                        children.push(child);
                        print("[add_child] Added child '" + child.name + "' to '" + name + "'. Children count now: " + to_string(children.size()));
                    }

                    auto get_parent_name() {
                        auto p = parent.lock();
                        if (p != null) {
                            return p.name;
                        }
                        return "no parent";
                    }

                    auto sum_tree() {
                        auto sum = value;
                        print("[sum_tree] Node '" + name + "' has " + to_string(children.size()) + " children");
                        for (auto i = 0; i < children.size(); i = i + 1) {
                            print("[sum_tree] Processing child " + to_string(i) + ": " + children[i].name);
                            sum = sum + children[i].sum_tree();
                        }
                        print("[sum_tree] Node '" + name + "' subtree sum = " + to_string(sum));
                        return sum;
                    }
                }
            )");
            
            auto result = eng->execute(R"(
                // Wrap objects in shared_ptr to get reference semantics
                auto root = shared_ptr<ScriptNode>("root", 1);
                auto child1 = shared_ptr<ScriptNode>("child1", 2);
                auto child2 = shared_ptr<ScriptNode>("child2", 3);
                auto grandchild = shared_ptr<ScriptNode>("grandchild", 4);

                // Set up tree structure - shared_ptr provides reference semantics
                root.add_child(child1);
                root.add_child(child2);
                child1.add_child(grandchild);

                // Test parent relationships with debug output
                auto parent1 = child1.get_parent_name();
                print("[DEBUG] child1.get_parent_name() = " + parent1);
                auto test1 = parent1 == "root";
                print("[DEBUG] test1 (child1 parent) = " + to_string(test1));

                auto parent2 = child2.get_parent_name();
                print("[DEBUG] child2.get_parent_name() = " + parent2);
                auto test2 = parent2 == "root";
                print("[DEBUG] test2 (child2 parent) = " + to_string(test2));

                auto parent3 = grandchild.get_parent_name();
                print("[DEBUG] grandchild.get_parent_name() = " + parent3);
                auto test3 = parent3 == "child1";
                print("[DEBUG] test3 (grandchild parent) = " + to_string(test3));

                auto parent4 = root.get_parent_name();
                print("[DEBUG] root.get_parent_name() = " + parent4);
                auto test4 = parent4 == "no parent";
                print("[DEBUG] test4 (root has no parent) = " + to_string(test4));

                // Test tree sum
                auto sum = root.sum_tree();
                print("[DEBUG] root.sum_tree() = " + to_string(sum));
                auto test5 = sum == 10;  // 1+2+3+4
                print("[DEBUG] test5 (sum == 10) = " + to_string(test5));

                test1 && test2 && test3 && test4 && test5
            )");
            
            check_eq(result.as<bool>(), true);
        });

        test("weak_ptr_script_class_lifetime", [this]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);

            // Register the C++ static counter as a global
            eng->add_global_ref("alive_count", LifetimeTracker::alive_count);

            // Define a script class that uses the global counter
            eng->execute(R"(
                class LifetimeTest {
                    string name = "";

                    LifetimeTest(string n) {
                        name = n;
                        alive_count = alive_count + 1;
                        print("Created " + name + ", alive: " + to_string(alive_count));
                    }

                    ~LifetimeTest() {
                        alive_count = alive_count - 1;
                        print("Destroyed " + name + ", alive: " + to_string(alive_count));
                    }
                }
            )");

            // Test weak_ptr behavior with destructors (must use shared_ptr for script classes too)
            auto result = eng->execute(R"(
                // Reset counter for this test
                alive_count = 0;

                // Test 1: Basic weak_ptr functionality (use shared_ptr for reference semantics)
                // Use block scope to ensure cleanup happens when expected
                auto test1 = false;
                auto test2a = false;
                auto test2b = false;
                auto test2c = false;
                {
                    auto obj = shared_ptr<LifetimeTest>("test1");
                    auto count_after_create = alive_count;  // Should be 1

                    auto weak = weak_ptr<LifetimeTest>(obj);
                    auto locked = weak.lock();

                    // Simplified test
                    test1 = locked != null;

                    // Test 2: Weak ptr becomes invalid when object is cleared
                    obj = null;
                    locked = null;

                    // Now destructor should have been called
                    auto count_after_destroy = alive_count;  // Should be 0
                    auto locked2 = weak.lock();

                    // Check each condition separately
                    test2a = locked2 == null;
                    test2b = weak.expired();
                    test2c = count_after_destroy == 0;
                }

                // Test 3: Multiple weak references with proper cleanup
                auto test3 = false;
                auto test4a = false;
                auto test4b = false;
                auto test4c = false;
                {
                    auto obj2 = shared_ptr<LifetimeTest>("test2");
                    auto count_with_obj2 = alive_count;  // Should be 1

                    auto weak1 = weak_ptr<LifetimeTest>(obj2);
                    auto weak2 = weak_ptr<LifetimeTest>(obj2);

                    test3 = !weak1.expired() && !weak2.expired() && count_with_obj2 == 1;

                    // Clear obj2 and verify destructor
                    obj2 = null;
                    auto final_count = alive_count;  // Should be 0

                    // Check each condition separately
                    test4a = weak1.expired();
                    test4b = weak2.expired();
                    test4c = final_count == 0;
                }

                [test1, test2a, test2b, test2c, test3, test4a, test4b, test4c]
            )");

            check(result.is_array(), "Result should be an array");
            auto arr = result.as_array();
            check_eq(arr.size(), 8);

            // Debug output
            std::cout << "  Test results: test1=" << arr[0].as_bool()
                      << ", test2a(locked==null)=" << arr[1].as_bool()
                      << ", test2b(expired)=" << arr[2].as_bool()
                      << ", test2c(count==0)=" << arr[3].as_bool()
                      << ", test3=" << arr[4].as_bool()
                      << ", test4a(weak1.expired)=" << arr[5].as_bool()
                      << ", test4b(weak2.expired)=" << arr[6].as_bool()
                      << ", test4c(count==0)=" << arr[7].as_bool() << std::endl;

            check_eq(arr[0].as_bool(), true, "test1 failed: Basic weak_ptr lock should work");
            check_eq(arr[1].as_bool(), true, "test2a failed: locked2 should be null after obj destroyed");
            check_eq(arr[2].as_bool(), true, "test2b failed: weak.expired() should return true after obj destroyed");
            check_eq(arr[3].as_bool(), true, "test2c failed: alive count should be 0 after destruction");
            check_eq(arr[4].as_bool(), true, "test3 failed: Multiple weak_ptrs should both be valid");
            check_eq(arr[5].as_bool(), true, "test4a failed: weak1.expired() should return true");
            check_eq(arr[6].as_bool(), true, "test4b failed: weak2.expired() should return true");
            check_eq(arr[7].as_bool(), true, "test4c failed: alive count should be 0 after destruction");
        });

        test("weak_ptr_circular_reference_script", [this]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);

            // Define a doubly-linked node class
            eng->execute(R"(
                class DNode {
                    int value = 0;
                    shared_ptr<DNode> next = null;  // Use shared_ptr for next to maintain reference
                    weak_ptr<DNode> prev = weak_ptr<DNode>();  // Weak to avoid cycle

                    DNode(int v) {
                        value = v;
                    }

                    auto link_next(shared_ptr<DNode> node) {
                        // Set next and prev to create bidirectional link
                        node.prev = weak_from_this();
                        next = node;
                    }

                    auto count_forward() {
                        auto count = 1;
                        if (next != null) {
                            count = count + next.count_forward();
                        }
                        return count;
                    }

                    auto count_backward() {
                        auto count = 1;
                        auto p = prev.lock();
                        if (p != null) {
                            count = count + p.count_backward();
                        }
                        return count;
                    }
                }
            )");

            auto result = eng->execute(R"(
                // Wrap nodes in shared_ptr for reference semantics
                auto node1 = shared_ptr<DNode>(1);
                auto node2 = shared_ptr<DNode>(2);
                auto node3 = shared_ptr<DNode>(3);

                // Link nodes - no reassignment needed with shared_ptr
                node1.link_next(node2);
                node2.link_next(node3);

                // Test forward counting
                auto forward = node1.count_forward() == 3;

                // Test backward counting
                auto backward = node3.count_backward() == 3;

                // Test middle node can go both ways
                auto mid_forward = node2.count_forward() == 2;
                auto mid_backward = node2.count_backward() == 2;

                forward && backward && mid_forward && mid_backward
            )");

            check_eq(result.as<bool>(), true);
        });

        // Test C++ bound types with auto-unwrap operator=
        test("shared_ptr_cpp_bound_type_auto_unwrap", [this]() {
            auto eng = make_engine();
            register_lifetime_tracker(*eng, true);  // Register with assignment_from<int>

            // Create shared_ptr and verify auto-unwrap assignment works
            eng->execute(R"(
                auto tracker = shared_ptr<LifetimeTracker>(42);
                auto ref = tracker;

                // Auto-unwrap assignment: tracker = 100 calls operator=(100)
                tracker = 100;
            )");

            // Verify both references see the change (shared mutation via operator=)
            auto tracker_value = eng->execute("tracker.get_value()");
            check_eq(tracker_value.as<int>(), 100);

            auto ref_value = eng->execute("ref.get_value()");
            check_eq(ref_value.as<int>(), 100);

            // Assign through the other reference
            eng->execute("ref = 200;");

            // Both should see the new value
            tracker_value = eng->execute("tracker.get_value()");
            check_eq(tracker_value.as<int>(), 200);
        });

        // Test same_as() for weak_ptr
        test("weak_ptr_same_as", [this]() {
            auto eng = make_engine();
            register_lifetime_tracker(*eng);

            auto result = eng->execute(R"(
                auto a = shared_ptr<LifetimeTracker>(10);
                auto b = a;
                weak_ptr<LifetimeTracker> weak_a = a;
                weak_ptr<LifetimeTracker> weak_b = b;

                auto c = shared_ptr<LifetimeTracker>(10);
                weak_ptr<LifetimeTracker> weak_c = c;

                // weak_a and weak_b reference the same object
                auto same_ab = weak_a.same_as(weak_b);
                // weak_a and weak_c reference different objects
                auto same_ac = weak_a.same_as(weak_c);

                same_ab && !same_ac
            )");
            check_eq(result.as<bool>(), true);
            // one identity rule (same_object): a weak handle names its owner's object both ways round, an expired handle is
            // its own object's still, and the method and same_object agree
            check_eq(std::string("true true true true | false false false false | true false false"), eng->execute(R"(
                auto s = shared_ptr<LifetimeTracker>(1);
                weak_ptr<LifetimeTracker> w = s;
                string out = `${(s.same_as(w))} ${(w.same_as(s))} ${(same_object(s, w))} ${(same_object(w, s))}`;
                weak_ptr<LifetimeTracker> e1 = shared_ptr<LifetimeTracker>(2);
                weak_ptr<LifetimeTracker> e2 = shared_ptr<LifetimeTracker>(3);
                out += ` | ${(e1.same_as(e2))} ${(same_object(e1, e2))} ${(e1.same_as(null))} ${(same_object(e1, null))}`;
                auto e1_again = e1;
                out += ` | ${(e1.same_as(e1_again))} ${(s.same_as(e1))} ${(e1.same_as(s))}`;
                out;
            )").to_string());
        });

        test("a_weak_ptr_observes_a_shared_ptr_and_expires_with_it", [this]() {
            check_true_rows({
                {"", "weak_ptr<LifetimeTracker> weak; weak.expired()"},
                {"", "weak_ptr<LifetimeTracker> weak; weak.lock() == null"},
                {"", R"(
                    auto obj = shared_ptr<LifetimeTracker>(42);
                    weak_ptr<LifetimeTracker> weak = obj;
                    auto locked = weak.lock();
                    locked != null && locked.get_value() == 42
                )"},
                {"", R"(
                    auto initial_count = LifetimeTracker::alive_count;
                    weak_ptr<LifetimeTracker> weak;
                    {
                        auto obj = shared_ptr<LifetimeTracker>(42);
                        weak = weak_ptr<LifetimeTracker>(obj);
                        auto during_scope = LifetimeTracker::alive_count == initial_count + 1;
                    }
                    auto after_scope = LifetimeTracker::alive_count == initial_count;
                    weak.expired() && after_scope
                )"},
                {"", R"(
                    auto obj1 = shared_ptr<LifetimeTracker>(10);
                    auto obj2 = shared_ptr<LifetimeTracker>(20);
                    weak_ptr<LifetimeTracker> weak = obj1;
                    auto locked1 = weak.lock();
                    auto val1 = locked1.get_value();
                    weak = obj2;
                    auto locked2 = weak.lock();
                    auto val2 = locked2.get_value();
                    val1 == 10 && val2 == 20
                )"},
                {"", R"(
                    auto obj1 = shared_ptr<LifetimeTracker>(1);
                    auto obj2 = shared_ptr<LifetimeTracker>(2);
                    auto obj3 = shared_ptr<LifetimeTracker>(3);
                    auto weak_refs = [weak_ptr<LifetimeTracker>(obj1), weak_ptr<LifetimeTracker>(obj2), weak_ptr<LifetimeTracker>(obj3)];
                    auto sum = 0;
                    for (auto i = 0; i < weak_refs.size(); i = i + 1) {
                        auto locked = weak_refs[i].lock();
                        if (locked != null) {
                            sum = sum + locked.get_value();
                        }
                    }
                    sum == 6
                )"},
                {"", R"(
                    weak_ptr<LifetimeTracker> weak;
                    auto obj = shared_ptr<LifetimeTracker>(42);
                    weak = obj;
                    auto not_expired = !weak.expired();
                    obj = null;
                    auto is_expired = weak.expired();
                    not_expired && is_expired
                )"},
                {"", R"(
                    auto obj = shared_ptr<LifetimeTracker>(42);
                    auto weak = weak_ptr<LifetimeTracker>(obj);
                    auto before = !weak.expired();
                    obj = null;
                    auto after = weak.expired();
                    before && after
                )"},
                {"", R"(
                    auto root = TreeNode("root");
                    auto child1 = TreeNode("child1");
                    auto child2 = TreeNode("child2");
                    root.add_child(child1);
                    root.add_child(child2);
                    child1.get_parent_name() == "root" && child2.get_parent_name() == "root" && root.get_parent_name() == "null"
                )"},
            });
        });

        test("same_as_is_object_identity_for_handles_and_values", [this]() {
            check_true_rows({
                {"", R"(
                    auto a = shared_ptr<LifetimeTracker>(42);
                    auto b = a;
                    auto c = shared_ptr<LifetimeTracker>(42);
                    a.same_as(b) && !a.same_as(c)
                )"},
                {"", R"(
                    auto shared = shared_ptr<LifetimeTracker>(5);
                    weak_ptr<LifetimeTracker> weak = shared;
                    shared.same_as(weak.lock())
                )"},
                {"", R"(
                    class Obj { int x = 0; }
                    auto a = Obj();
                    auto b = a;
                    auto c = a;
                    !a.same_as(b) && a.same_as(a)
                )"},
            });
        });

        test("a_shared_ptr_shares_its_object_where_a_value_copies", [this]() {
            const value_row rows[] = {
                {"auto obj = shared_ptr<LifetimeTracker>(42); auto copy = obj; copy.set_value(100); obj.get_value()", 100},
                {"auto obj = LifetimeTracker(42); auto copy = obj; copy.set_value(100); obj.get_value()", 42},
                {"auto obj = shared_ptr<LifetimeTracker>(); obj.get_value()", 0},
                {R"(
                    auto val1 = LifetimeTracker(10);
                    auto val2 = val1;
                    val2.set_value(99);
                    auto val1_unchanged = val1.get_value() == 10;
                    auto ref1 = shared_ptr<LifetimeTracker>(20);
                    auto ref2 = ref1;
                    ref2.set_value(88);
                    auto ref1_changed = ref1.get_value() == 88;
                    val1_unchanged && ref1_changed
                )", true},
                {"auto a = shared_ptr<LifetimeTracker>(42); auto b = a; a = null; a == null && b.get_value() == 42", true},
                {R"(
                    auto a = shared_ptr<LifetimeTracker>(10);
                    auto b = shared_ptr<LifetimeTracker>(20);
                    auto c = a;
                    a = b;
                    a.get_value() == 20 && b.get_value() == 20 && c.get_value() == 10
                )", true},
            };
            check_values(rows, [this](engine& a_engine) { register_lifetime_tracker(a_engine); });
        });

        test("assigning_a_value_to_a_shared_ptr_assigns_into_the_shared_object", [this]() {
            check_true_rows({
                {"class Counter { int value = 0; Counter(int v) { value = v; } auto operator=(int v) { value = v; return this; } }",
                 "auto a = shared_ptr<Counter>(5); auto b = a; a = 10; a.value == 10 && b.value == 10"},
                {"class BoxInt { int value = 0; BoxInt(int v) { value = v; } auto operator=(int v) { value = v; return this; } }",
                 "auto x = shared_ptr<BoxInt>(5); auto y = x; y = 10; x.value == 10 && y.value == 10"},
                {"class Accumulator { int total = 0; Accumulator(int t) { total = t; } auto operator=(int v) { total = total + v; return this; } }",
                 "auto a = shared_ptr<Accumulator>(0); auto b = a; auto c = a; a = 10; b = 20; c = 5; a.total == 35 && b.total == 35 && c.total == 35"},
                {"class Point { int x = 0; int y = 0; Point(int px, int py) { x = px; y = py; } }",
                 "auto a = shared_ptr<Point>(1, 2); auto b = a; a = Point(10, 20); a.x == 10 && a.y == 20 && b.x == 10 && b.y == 20"},
                {"class Animal { string name = \"\"; Animal(string n) { name = n; } } class Dog : Animal { Dog(string n) : super(n) {} }",
                 "auto dog = shared_ptr<Dog>(\"Rex\"); shared_ptr<Animal> animal = dog; animal.name == \"Rex\""},
            });
        });

        test("a_weak_ptr_or_shared_ptr_rejects_what_it_cannot_hold", [this]() {
            struct error_row { const char* prelude; const char* code; std::vector<const char*> all_of; std::vector<const char*> any_of; };
            const error_row rows[] = {
                {"", "auto obj = LifetimeTracker(42); auto weak = weak_ptr<LifetimeTracker>(obj);", {"weak_ptr", "shared_ptr<"}, {}},
                {"", "var obj = LifetimeTracker(42); weak_ptr<LifetimeTracker> weak = obj;",
                 {"Cannot initialize 'weak' (declared weak_ptr<LifetimeTracker>) with LifetimeTracker"}, {}},
                {"", "weak_ptr<LifetimeTracker> weak; var obj = LifetimeTracker(42); weak = obj;",
                 {"Cannot assign LifetimeTracker to 'weak' (declared weak_ptr<LifetimeTracker>)"}, {}},
                // shared_ptr<T>(args) forwards to T's constructor, which takes no LifetimeTracker
                {"", "auto b = LifetimeTracker(42); auto a = shared_ptr<LifetimeTracker>(b);", {}, {}},
                {"class Simple { int value = 0; Simple(int v) { value = v; } }", "auto a = shared_ptr<Simple>(5); a = \"hello\";", {}, {"operator=", "Cannot assign"}},
                {"auto tracker = shared_ptr<LifetimeTracker>(42);", "tracker = 100;", {"Cannot assign int to 'tracker' (declared shared_ptr<LifetimeTracker>)"}, {}},
            };
            check_each(rows, [&](const error_row& row) {
                auto eng = make_test_engine();
                if (*row.prelude) { eng->execute(row.prelude); }
                std::string error_msg;
                bool threw = false;
                try {
                    eng->execute(row.code);
                } catch (const std::exception& e) {
                    threw = true;
                    error_msg = e.what();
                }
                check_true(threw, "no error");
                for (const char* needle : row.all_of) {
                    check_true(error_msg.find(needle) != std::string::npos, error_msg);
                }
                const bool any = row.any_of.empty() || std::any_of(row.any_of.begin(), row.any_of.end(), [&](const char* n) { return error_msg.find(n) != std::string::npos; });
                check_true(any, error_msg);
            });
        });
    }

private:
    struct true_row { const char* prelude; const char* code; };

    std::shared_ptr<engine> make_test_engine() {
        auto eng = make_engine();
        stdlib::register_all(*eng);
        register_lifetime_tracker(*eng);
        register_tree_node(*eng);
        return eng;
    }

    // each row's prelude runs first on a fresh engine, then its code must yield true
    void check_true_rows(const std::vector<true_row>& a_rows) {
        check_each(a_rows, [&](const true_row& row) {
            auto eng = make_test_engine();
            if (*row.prelude) { eng->execute(row.prelude); }
            check_eq(true, eng->execute(row.code).as<bool>());
        });
    }
    void register_lifetime_tracker(engine& eng, bool with_assignment = false) {
        // Note: copy_function is automatically registered for copy-constructible types
        // dynamic_binder is non-copyable, so we use reference from construction
        dynamic_binder<LifetimeTracker> builder(eng, "LifetimeTracker");
        builder.constructor<>()
            .constructor<int>()
            .method("get_value", &LifetimeTracker::get_value)
            .method("set_value", &LifetimeTracker::set_value)
            .property("value", &LifetimeTracker::value)
            .property("id", &LifetimeTracker::id)
            .static_property("alive_count", &LifetimeTracker::alive_count)
            .static_property("total_created", &LifetimeTracker::total_created);

        if (with_assignment) {
            // Register operator= to enable auto-unwrap for shared_ptr<LifetimeTracker>
            builder.method("=", [](LifetimeTracker& self, int v) {
                self.value = v;
            });
        }
        // Auto-build on scope exit, or explicitly:
        builder.build();
    }
    
    void register_tree_node(engine& eng) {
        dynamic_binder<TreeNode>(eng, "TreeNode")
            .constructor<std::string>()
            .method("get_name", &TreeNode::get_name)
            .method("add_child", &TreeNode::add_child)
            .method("get_parent_name", &TreeNode::get_parent_name)
            .method("child_count", &TreeNode::child_count)
            .property("name", &TreeNode::name)
            .property("parent", &TreeNode::parent)
            // Don't expose children vector directly - use methods instead
            .build();
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::weak_ptr_tests)