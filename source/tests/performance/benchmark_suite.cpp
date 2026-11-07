#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/timing.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <optional>
#include <iostream>
#include <vector>
#include <algorithm>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

struct bench_creature { script_int hp = 3; };   // the boundary row's bound class

class performance_benchmarks : public suite {
public:
    performance_benchmarks() : suite("Performance Benchmarks") {}

    // Reusable engine instance - created once, reused across benchmarks
    std::shared_ptr<jai::engine> test_engine;

    // Pre-parsed handle for the [jaibite] benchmark variants: parsed lazily on the
    // first (warmup) iteration, reused for every measured one.
    std::optional<jai::jaibite> bite;

    void pre_test() override {
        // Create engine once before tests
        test_engine = make_engine();
        jai::stdlib::register_all(test_engine);
        bite.reset();
    }

    void run_bite(const char* source) {
        if (!bite) {
            bite = test_engine->jaibite(source);
        }
        bite->execute();
    }

    void post_test() override {
        // Optional: Clean up after each test if needed
    }

    // Real-world macro bench: one script, BOTH backends, deterministic int result as the parity tripwire, the VM timed
    // against the interpreter (each round a fresh engine, its setup untimed)
    void run_macro_bench(const char* name, const char* src) {
        int64_t hash[2] = {0, 0};
        auto timed = [&](bool use_vm) {
            return [&, use_vm] {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                e->execution_budget(0);
                return time_seconds([&] { hash[use_vm ? 1 : 0] = e->execute(src).as_int(); });
            };
        };
        compare_speed(std::string("[bench] ") + name + ": the vm against the interpreter", timed(false), timed(true), 3);
        check_eq(hash[0], hash[1], std::string(name) + " backend hash parity");
    }

    void forge_tests() override {
        // Real-world shaped macro benches (the GLOOM lessons): entity_tick
        // = the game-loop shape (class instances, method calls, field read/write ICs);
        // brains = the call-boundary shape (coroutine resumes + bare sibling method
        // calls — the CALL_FROM_SCRATCH heat).
        test("bench_entity_tick", [this]() {
            run_macro_bench("entity_tick", R"(
                class Entity {
                    int id = 0; float x = 0.0; float y = 0.0; float vx = 0.0; float vy = 0.0;
                    int hp = 100; int state = 0; int anim = 0;
                    Entity(int id_) {
                        id = id_; x = id_ * 3.7; y = id_ * 1.3;
                        vx = 0.5 + (id_ % 7) * 0.1; vy = 0.25;
                    }
                    void tick(float dt) {
                        x = x + vx * dt; y = y + vy * dt;
                        anim = anim + 1;
                        if (x > 100.0) { x = x - 100.0; state = state + 1; }
                        if (y > 50.0) { y = y - 50.0; }
                        if (anim % 60 == 0) { hp = hp - 1; if (hp <= 0) { hp = 100; state = 0; } }
                    }
                    int fold() { return (id * 31 + anim) * 7 + state * 3 + hp; }
                }
                var ents = [];
                for (int i = 0; i < 200; ++i) { ents.push(new Entity(i)); }
                int acc = 0;
                for (int t = 0; t < 500; ++t) {
                    for (int i = 0; i < 200; ++i) { var& e = ents[i]; e.tick(0.016); }
                    if (t % 25 == 0) {
                        for (int i = 0; i < 200; ++i) { var& e = ents[i]; acc = (acc * 33 + e.fold()) % 1000000007; }
                    }
                }
                acc;
            )");
        });

        test("bench_brains", [this]() {
            run_macro_bench("brains", R"(
                class Bot {
                    int id = 0; float x = 0.0; float tgt = 0.0; int mood = 0; var brain = null;
                    Bot(int id_) { id = id_; x = id_ * 1.0; tgt = ((id_ * 37) % 100) * 1.0; }
                    float dist() { if (tgt > x) { return tgt - x; } return x - tgt; }
                    bool near() { return dist() < 1.5; }
                    void step_toward() { if (x < tgt) { x = x + 1.0; } else { x = x - 1.0; } }
                    coroutine void think() {
                        while (true) {
                            while (!near()) { step_toward(); mood = mood + 1; yield; }
                            tgt = ((mood + id * 13) % 100) * 1.0;
                            mood = mood + 100;
                            yield;
                        }
                    }
                    void tick() {
                        if (brain == null || brain.done()) { brain = think(); }
                        brain.next();
                    }
                    int fold() { return id * 31 + mood; }
                }
                var bots = [];
                for (int i = 0; i < 100; ++i) { bots.push(new Bot(i)); }
                int acc = 0;
                for (int t = 0; t < 500; ++t) {
                    for (int i = 0; i < 100; ++i) { var& b = bots[i]; b.tick(); }
                    if (t % 25 == 0) {
                        for (int i = 0; i < 100; ++i) { var& b = bots[i]; acc = (acc * 33 + b.fold()) % 1000000007; }
                    }
                }
                acc;
            )");
        });

        // Note: Foundry's benchmark() runs 1000 iterations by default
        // Now we're measuring actual execution performance, not engine creation

        // x20 unrolled through runtime values: parse-time constant folding can't elide
        // the ops, and 20 ops lift the row off the integer-uS floor (a 0uS row is noise)
        benchmark("Integer Addition (x20)", [this]() {
            auto result = test_engine->execute(R"(
                auto a = 42;
                auto acc = 0;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc;
            )");
        });

        benchmark("Float Multiplication (x20)", [this]() {
            auto result = test_engine->execute(R"(
                auto f = 1.001;
                auto acc = 3.14;
                acc = acc * f; acc = acc * f; acc = acc * f; acc = acc * f; acc = acc * f;
                acc = acc * f; acc = acc * f; acc = acc * f; acc = acc * f; acc = acc * f;
                acc = acc * f; acc = acc * f; acc = acc * f; acc = acc * f; acc = acc * f;
                acc = acc * f; acc = acc * f; acc = acc * f; acc = acc * f; acc = acc * f;
                acc;
            )");
        });

        benchmark("Variable Operations", [this]() {
            test_engine->execute(R"(
                auto x = 10;
                auto y = 20;
                auto z = x + y;
            )");
        });

        benchmark("Function Calls", [this]() {
            test_engine->execute(R"(
                function add(auto a, auto b) -> auto {
                    return a + b;
                }
                add(10, 20);
            )");
        });

        benchmark("Array Push/Pop", [this]() {
            test_engine->execute(R"(
                auto arr = [];
                arr.push(1);
                arr.push(2);
                arr.push(3);
                arr.pop();
                arr.size();
            )");
        });

        benchmark("Map Insert/Lookup", [this]() {
            test_engine->execute(R"(
                auto m = {};
                m["key1"] = 100;
                m["key2"] = 200;
                auto val = m["key1"];
            )");
        });

        // Map range-for hands the pair a SHARED key handle (no per-iteration key copy); this
        // row guards that cost, by value and by reference
        benchmark("Map Range-For (64 string keys, by value + by ref)", [this]() {
            test_engine->execute(R"(
                map<string,int> m = {};
                for (int i = 0; i < 64; ++i) { m["key_" + to_string(i)] = i; }
                int acc = 0;
                for (auto kv : m) { acc += kv.second + kv.first.length(); }
                for (auto& kv : m) { kv.second += 1; acc += kv.first.length(); }
                acc;
            )");
        });

        benchmark("Class Creation", [this]() {
            test_engine->execute(R"(
                class Point {
                    float x = 0.0;
                    float y = 0.0;

                    Point(float px, float py) {
                        x = px;
                        y = py;
                    }
                }

                auto p = Point(3.0, 4.0);
            )");
        });

        benchmark("Method Invocation", [this]() {
            test_engine->execute(R"(
                class Calculator {
                    int add(int a, int b) {
                        return a + b;
                    }
                }

                auto calc = Calculator();
                calc.add(10, 20);
            )");
        });

        benchmark("For Loop (100 iterations)", [this]() {
            test_engine->execute(R"(
                auto sum = 0;
                for (auto i = 0; i < 100; i += 1) {
                    sum += i;
                }
            )");
        });

        benchmark("String Concatenation", [this]() {
            test_engine->execute(R"(
                auto s1 = "Hello";
                auto s2 = "World";
                auto s3 = s1 + " " + s2;
            )");
        });

        // Keep these separate - they measure engine overhead specifically
        benchmark("Engine Creation", []() {
            auto engine = make_engine();
        });

        benchmark("Stdlib Registration", []() {
            auto engine = make_engine();
            jai::stdlib::register_all(engine);
        });

        benchmark("Complex Expression", [this]() {
            test_engine->execute("(10 + 20) * (30 - 15) / 5");
        });

        benchmark("Class Inheritance", [this]() {
            test_engine->execute(R"(
                class Animal {
                    string name = "";
                    Animal(string n) { name = n; }
                }

                class Dog : Animal {
                    Dog(string n) : super(n) {}
                }

                auto dog = Dog("Buddy");
            )");
        });

        benchmark("Hot Loop (1000 iterations)", [this]() {
            test_engine->execute(R"(
                auto sum = 0;
                for (auto i = 0; i < 1000; i += 1) {
                    sum += i * 2;
                }
            )");
        });

        // === Type ladder: int (enforced) vs auto (inferred) vs var (dynamic) ===
        // Same shapes, three declaration spellings - the measured cost of type
        // enforcement. The plain rows above use auto; these are the int and var twins.
        benchmark("Hot Loop 1000 (int decls)", [this]() {
            test_engine->execute(R"(
                int sum = 0;
                for (int i = 0; i < 1000; i += 1) {
                    sum += i * 2;
                }
            )");
        });

        benchmark("Hot Loop 1000 (var decls)", [this]() {
            test_engine->execute(R"(
                var sum = 0;
                for (var i = 0; i < 1000; i += 1) {
                    sum += i * 2;
                }
            )");
        });

        benchmark("Integer Addition x20 (var decls)", [this]() {
            test_engine->execute(R"(
                var a = 42;
                var acc = 0;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc;
            )");
        });

        benchmark("For Loop 100 (var decls)", [this]() {
            test_engine->execute(R"(
                var sum = 0;
                for (var i = 0; i < 100; i += 1) {
                    sum += i;
                }
            )");
        });

        // Additional targeted benchmarks to isolate optimizations
        benchmark("Simple Compound Assignment (x20)", [this]() {
            test_engine->execute(R"(
                auto x = 0;
                x += 1; x += 1; x += 1; x += 1; x += 1;
                x += 1; x += 1; x += 1; x += 1; x += 1;
                x += 1; x += 1; x += 1; x += 1; x += 1;
                x += 1; x += 1; x += 1; x += 1; x += 1;
            )");
        });

        // stores through int& over typed storage (a parameter, a local): the declared-type check on the store path
        benchmark("Reference Stores (int& over int, 200 iterations)", [this]() {
            test_engine->execute(R"(
                function bump(int& x) { x = x + 1; }
                int n = 0;
                for (int i = 0; i < 200; i++) { bump(n); int& r = n; r = r + 1; r += 2; }
                n;
            )");
        });

        // int& over a var: the declared type converts every store
        benchmark("Reference Stores (int& over var, 200 iterations)", [this]() {
            test_engine->execute(R"(
                var w = 0;
                for (int i = 0; i < 200; i++) { int& q = w; q = q + 1; q += 2; }
                w;
            )");
        });

        benchmark("Increments (int and var locals, 200 iterations)", [this]() {
            test_engine->execute(R"(
                var v = 0;
                int k = 0;
                for (int i = 0; i < 200; i++) { v++; k++; ++v; --k; }
                v + k;
            )");
        });

        benchmark("Variable Lookup Heavy", [this]() {
            test_engine->execute(R"(
                auto a = 1;
                auto b = 2;
                auto c = 3;
                auto result = a + b + c + a + b + c + a + b + c + a;
            )");
        });

        // === var vs auto nested container benchmarks ===
        // These measure the overhead of homogeneity validation for auto

        benchmark("auto: Simple Array [10 ints]", [this]() {
            test_engine->execute(R"(
                auto arr = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
            )");
        });

        benchmark("var: Simple Array [10 ints]", [this]() {
            test_engine->execute(R"(
                var arr = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
            )");
        });

        benchmark("auto: 2D Array [[5x5 ints]]", [this]() {
            test_engine->execute(R"(
                auto matrix = [
                    [1, 2, 3, 4, 5],
                    [6, 7, 8, 9, 10],
                    [11, 12, 13, 14, 15],
                    [16, 17, 18, 19, 20],
                    [21, 22, 23, 24, 25]
                ];
            )");
        });

        benchmark("var: 2D Array [[5x5 ints]]", [this]() {
            test_engine->execute(R"(
                var matrix = [
                    [1, 2, 3, 4, 5],
                    [6, 7, 8, 9, 10],
                    [11, 12, 13, 14, 15],
                    [16, 17, 18, 19, 20],
                    [21, 22, 23, 24, 25]
                ];
            )");
        });

        benchmark("auto: 3D Array [[[2x2x2 ints]]]", [this]() {
            test_engine->execute(R"(
                auto cube = [
                    [[1, 2], [3, 4]],
                    [[5, 6], [7, 8]]
                ];
            )");
        });

        benchmark("var: 3D Array [[[2x2x2 ints]]]", [this]() {
            test_engine->execute(R"(
                var cube = [
                    [[1, 2], [3, 4]],
                    [[5, 6], [7, 8]]
                ];
            )");
        });

        benchmark("auto: Homogeneous Map {5 string keys -> ints}", [this]() {
            test_engine->execute(R"(
                auto m = {"a": 1, "b": 2, "c": 3, "d": 4, "e": 5};
            )");
        });

        benchmark("var: Heterogeneous Map {5 mixed values}", [this]() {
            test_engine->execute(R"(
                var m = {"name": "test", "age": 30, "active": true, "score": 3.14, "count": 100};
            )");
        });

        benchmark("auto: Nested Map 2 levels {k: {k: int}}", [this]() {
            test_engine->execute(R"(
                auto data = {
                    "group1": {"x": 1, "y": 2, "z": 3},
                    "group2": {"x": 4, "y": 5, "z": 6}
                };
            )");
        });

        benchmark("var: Nested Map 2 levels {k: {k: mixed}}", [this]() {
            test_engine->execute(R"(
                var data = {
                    "user1": {"name": "Alice", "age": 30},
                    "user2": {"name": "Bob", "age": 25}
                };
            )");
        });

        benchmark("auto: Mixed Array+Map 3 levels [[{k: int}]]", [this]() {
            test_engine->execute(R"(
                auto data = [
                    [{"a": 1, "b": 2}, {"c": 3, "d": 4}],
                    [{"e": 5, "f": 6}, {"g": 7, "h": 8}]
                ];
            )");
        });

        benchmark("var: Mixed Array+Map 3 levels [[{k: mixed}]]", [this]() {
            test_engine->execute(R"(
                var data = [
                    [{"name": "a", "val": 1}, {"name": "b", "val": 2}],
                    [{"name": "c", "val": 3}, {"name": "d", "val": 4}]
                ];
            )");
        });

        // === jaibite (pre-parsed) variants: steady-state execution cost only ===

        benchmark("Integer Addition (x20) [jaibite]", [this]() {
            run_bite(R"(
                auto a = 42;
                auto acc = 0;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a; acc = acc + a;
                acc;
            )");
        });

        benchmark("Function Calls [jaibite]", [this]() {
            run_bite(R"(
                function add(auto a, auto b) -> auto {
                    return a + b;
                }
                add(10, 20);
            )");
        });

        benchmark("Method Invocation [jaibite]", [this]() {
            run_bite(R"(
                class Calculator {
                    int add(int a, int b) {
                        return a + b;
                    }
                }

                auto calc = Calculator();
                calc.add(10, 20);
            )");
        });

        benchmark("For Loop (100 iterations) [jaibite]", [this]() {
            run_bite(R"(
                auto sum = 0;
                for (auto i = 0; i < 100; i += 1) {
                    sum += i;
                }
            )");
        });

        benchmark("Hot Loop (1000 iterations) [jaibite]", [this]() {
            run_bite(R"(
                auto sum = 0;
                for (auto i = 0; i < 1000; i += 1) {
                    sum += i * 2;
                }
            )");
        });

        // === Worst cases: recursion + allocation heavy (from the Squirrel comparison) ===

        benchmark("Fibonacci(15) [recursion]", [this]() {
            if (!bite) {
                test_engine->execute("function fib(auto n) -> auto { if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); }");
                bite = test_engine->jaibite("fib(15);");
            }
            bite->execute();
        });

        // Type-ladder twins of the fib row (auto above): call-dense recursion is where
        // param/return enforcement cost would show if it were material
        benchmark("Fibonacci(15) (int params)", [this]() {
            if (!bite) {
                test_engine->execute("function ifib(int n) -> int { if (n <= 1) { return n; } return ifib(n - 1) + ifib(n - 2); }");
                bite = test_engine->jaibite("ifib(15);");
            }
            bite->execute();
        });

        benchmark("Fibonacci(15) (var params)", [this]() {
            if (!bite) {
                test_engine->execute("function vfib(var n) -> var { if (n <= 1) { return n; } return vfib(n - 1) + vfib(n - 2); }");
                bite = test_engine->jaibite("vfib(15);");
            }
            bite->execute();
        });

        benchmark("Recurse 10 Locals (depth=15)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    function recurseWithLocals(int depth) -> int {
                        if (depth <= 0) { return 0; }
                        auto a = depth * 2;
                        auto b = depth + 3;
                        auto c = a + b;
                        auto d = c * 2;
                        auto e = d - a;
                        auto f = e + b;
                        auto g = f * depth;
                        auto h = g - c;
                        auto i = h + d;
                        auto j = i - e;
                        auto sum = a + b + c + d + e + f + g + h + i + j;
                        return sum + recurseWithLocals(depth - 1);
                    }
                )");
                bite = test_engine->jaibite("recurseWithLocals(15);");
            }
            bite->execute();
        });

        benchmark("BST insert/sum/rotate (15 nodes) [naive by-value]", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    class TreeNode {
                        int value = 0;
                        TreeNode left = null;
                        TreeNode right = null;
                        TreeNode(int val) { value = val; }
                    }
                    function insertNode(TreeNode root, int val) -> TreeNode {
                        if (root == null) { return TreeNode(val); }
                        if (val < root.value) {
                            root.left = insertNode(root.left, val);
                        } else {
                            root.right = insertNode(root.right, val);
                        }
                        return root;
                    }
                    function inorderSum(TreeNode node) -> int {
                        if (node == null) { return 0; }
                        return inorderSum(node.left) + node.value + inorderSum(node.right);
                    }
                    function treeHeight(TreeNode node) -> int {
                        if (node == null) { return 0; }
                        auto leftH = treeHeight(node.left);
                        auto rightH = treeHeight(node.right);
                        if (leftH > rightH) { return 1 + leftH; }
                        return 1 + rightH;
                    }
                    function rotateRight(TreeNode y) -> TreeNode {
                        if (y == null) { return null; }
                        if (y.left == null) { return y; }
                        auto x = y.left;
                        y.left = x.right;
                        x.right = y;
                        return x;
                    }
                )");
                bite = test_engine->jaibite(R"(
                    auto tree_root = TreeNode(8);
                    tree_root = insertNode(tree_root, 4);
                    tree_root = insertNode(tree_root, 12);
                    tree_root = insertNode(tree_root, 2);
                    tree_root = insertNode(tree_root, 6);
                    tree_root = insertNode(tree_root, 10);
                    tree_root = insertNode(tree_root, 14);
                    tree_root = insertNode(tree_root, 1);
                    tree_root = insertNode(tree_root, 3);
                    tree_root = insertNode(tree_root, 5);
                    tree_root = insertNode(tree_root, 7);
                    tree_root = insertNode(tree_root, 9);
                    tree_root = insertNode(tree_root, 11);
                    tree_root = insertNode(tree_root, 13);
                    tree_root = insertNode(tree_root, 15);

                    auto tree_sum = inorderSum(tree_root);
                    auto tree_height = treeHeight(tree_root);
                    tree_root = rotateRight(tree_root);
                    tree_sum = inorderSum(tree_root);
                )");
            }
            bite->execute();
        });

        benchmark("BST shared_ptr insert/sum (15 nodes)", [this]() {
            // Reference-semantic variant: apples-to-apples with Squirrel/Lua instances
            // (the value-semantic BST above deep-copies subtrees per by-value pass).
            // var params preserve shared_ptr sharing; typed SNode params would clone.
            if (!bite) {
                test_engine->execute(R"(
                    class SNode {
                        int value = 0;
                        SNode left = null;
                        SNode right = null;
                        SNode(int val) { value = val; }
                    }
                    function sInsert(var root, int val) -> SNode {
                        if (root == null) { return shared_ptr<SNode>(val); }
                        if (val < root.value) {
                            root.left = sInsert(root.left, val);
                        } else {
                            root.right = sInsert(root.right, val);
                        }
                        return root;
                    }
                    function sSum(var node) -> int {
                        if (node == null) { return 0; }
                        return sSum(node.left) + node.value + sSum(node.right);
                    }
                )");
                bite = test_engine->jaibite(R"(
                    auto s_root = shared_ptr<SNode>(8);
                    s_root = sInsert(s_root, 4);
                    s_root = sInsert(s_root, 12);
                    s_root = sInsert(s_root, 2);
                    s_root = sInsert(s_root, 6);
                    s_root = sInsert(s_root, 10);
                    s_root = sInsert(s_root, 14);
                    s_root = sInsert(s_root, 1);
                    s_root = sInsert(s_root, 3);
                    s_root = sInsert(s_root, 5);
                    s_root = sInsert(s_root, 7);
                    s_root = sInsert(s_root, 9);
                    s_root = sInsert(s_root, 11);
                    s_root = sInsert(s_root, 13);
                    s_root = sInsert(s_root, 15);
                    auto s_total = sSum(s_root);
                    if (s_total != 120) { throw "BST shared_ptr sum mismatch"; }
                )");
            }
            bite->execute();
        });

        benchmark("BST by-ref insert/sum (15 nodes)", [this]() {
            // Reference-parameter variant: field lvalues bind by ref, the tree is
            // mutated in place with zero deep copies (the value-semantic BST above
            // stays as the value-semantics feature-cost number)
            if (!bite) {
                test_engine->execute(R"(
                    class RNode {
                        int value = 0;
                        RNode left = null;
                        RNode right = null;
                        RNode(int val) { value = val; }
                    }
                    function rInsert(RNode& node, int val) {
                        if (node == null) { node = RNode(val); return; }
                        if (val < node.value) { rInsert(node.left, val); } else { rInsert(node.right, val); }
                    }
                    function rSum(RNode& node) -> int {
                        if (node == null) { return 0; }
                        return rSum(node.left) + node.value + rSum(node.right);
                    }
                )");
                bite = test_engine->jaibite(R"(
                    var ref_root = null;
                    rInsert(ref_root, 8);
                    rInsert(ref_root, 4);
                    rInsert(ref_root, 12);
                    rInsert(ref_root, 2);
                    rInsert(ref_root, 6);
                    rInsert(ref_root, 10);
                    rInsert(ref_root, 14);
                    rInsert(ref_root, 1);
                    rInsert(ref_root, 3);
                    rInsert(ref_root, 5);
                    rInsert(ref_root, 7);
                    rInsert(ref_root, 9);
                    rInsert(ref_root, 11);
                    rInsert(ref_root, 13);
                    rInsert(ref_root, 15);
                    var ref_sum = rSum(ref_root);
                    if (ref_sum != 120) { throw "BST by-ref sum mismatch"; }
                )");
            }
            bite->execute();
        });

        benchmark("Ref Param Pass-Through relay->inc (x100)", [this]() {
            // Tier 2 hop-cost sentinel: each relay hop shares the incoming holder
            // (would have caught the old per-hop anchor-env allocation)
            if (!bite) {
                test_engine->execute(R"(
                    function incRef(int& x) { x += 1; }
                    function relayRef(int& x) { incRef(x); }
                )");
                bite = test_engine->jaibite(R"(
                    var n = 0;
                    for (auto i = 0; i < 100; ++i) { relayRef(n); }
                    if (n != 100) { throw "ref relay sum mismatch"; }
                )");
            }
            bite->execute();
        });

        // === Aliasing strategy study: plain value vs T& vs shared_ptr<T> ===
        // Same computation, same gated result in every variant; only the holding/passing
        // strategy differs. The Creature is deliberately meaty (string name + 8-int stats
        // array + 4 scalars) so the by-value clone cost is the real thing it would be in
        // game code. Per-call figure = row / 200.

        benchmark("Aliasing read: pass Creature by value (x200 calls)", [this]() {
            // Naive by-value: every call deep-clones name + stats + scalars
            if (!bite) {
                test_engine->execute(R"(
                    class Creature {
                        string name = "";
                        int hp = 0;
                        int attack = 0;
                        int defense = 0;
                        int level = 0;
                        array<int> stats = [];
                        Creature(string n) {
                            name = n; hp = 100; attack = 12; defense = 7; level = 5;
                            stats = [3, 1, 4, 1, 5, 9, 2, 6];
                        }
                    }
                    function power(Creature c) -> int {
                        auto total = c.attack * 2 + c.defense * 3 + c.level;
                        for (auto s : c.stats) { total += s; }
                        return total + c.hp / 10;
                    }
                    auto cv = Creature("Grubwell");
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto i = 0; i < 200; ++i) { acc += power(cv); }
                    if (acc != 18200) { throw "aliasing read by-value mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Aliasing read: pass Creature& (x200 calls)", [this]() {
            // Ref param: the call shares the variable's cell, zero clones
            if (!bite) {
                test_engine->execute(R"(
                    class Creature {
                        string name = "";
                        int hp = 0;
                        int attack = 0;
                        int defense = 0;
                        int level = 0;
                        array<int> stats = [];
                        Creature(string n) {
                            name = n; hp = 100; attack = 12; defense = 7; level = 5;
                            stats = [3, 1, 4, 1, 5, 9, 2, 6];
                        }
                    }
                    function power(Creature& c) -> int {
                        auto total = c.attack * 2 + c.defense * 3 + c.level;
                        for (auto s : c.stats) { total += s; }
                        return total + c.hp / 10;
                    }
                    auto cv = Creature("Grubwell");
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto i = 0; i < 200; ++i) { acc += power(cv); }
                    if (acc != 18200) { throw "aliasing read by-ref mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Aliasing read: pass shared_ptr<Creature> (x200 calls)", [this]() {
            // Reference-semantic instance (new = shared_ptr sugar): calls copy the handle
            if (!bite) {
                test_engine->execute(R"(
                    class Creature {
                        string name = "";
                        int hp = 0;
                        int attack = 0;
                        int defense = 0;
                        int level = 0;
                        array<int> stats = [];
                        Creature(string n) {
                            name = n; hp = 100; attack = 12; defense = 7; level = 5;
                            stats = [3, 1, 4, 1, 5, 9, 2, 6];
                        }
                    }
                    function power(shared_ptr<Creature> c) -> int {
                        auto total = c.attack * 2 + c.defense * 3 + c.level;
                        for (auto s : c.stats) { total += s; }
                        return total + c.hp / 10;
                    }
                    var cs = new Creature("Grubwell");
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto i = 0; i < 200; ++i) { acc += power(cs); }
                    if (acc != 18200) { throw "aliasing read shared_ptr mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        // Held-instance axis: same compute as a METHOD, called through each holder
        // (methods bind self by reference, so no variant clones — this isolates the
        // per-call holder cost: plain local vs ref alias vs shared_ptr handle)

        benchmark("Aliasing method: held by value (x200 calls)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    class Creature {
                        string name = "";
                        int hp = 0;
                        int attack = 0;
                        int defense = 0;
                        int level = 0;
                        array<int> stats = [];
                        Creature(string n) {
                            name = n; hp = 100; attack = 12; defense = 7; level = 5;
                            stats = [3, 1, 4, 1, 5, 9, 2, 6];
                        }
                        function power() -> int {
                            auto total = attack * 2 + defense * 3 + level;
                            for (auto s : stats) { total += s; }
                            return total + hp / 10;
                        }
                    }
                    auto cv = Creature("Grubwell");
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto i = 0; i < 200; ++i) { acc += cv.power(); }
                    if (acc != 18200) { throw "aliasing method value mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Aliasing method: held by Creature& alias (x200 calls)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    class Creature {
                        string name = "";
                        int hp = 0;
                        int attack = 0;
                        int defense = 0;
                        int level = 0;
                        array<int> stats = [];
                        Creature(string n) {
                            name = n; hp = 100; attack = 12; defense = 7; level = 5;
                            stats = [3, 1, 4, 1, 5, 9, 2, 6];
                        }
                        function power() -> int {
                            auto total = attack * 2 + defense * 3 + level;
                            for (auto s : stats) { total += s; }
                            return total + hp / 10;
                        }
                    }
                    auto cv = Creature("Grubwell");
                    auto& cr = cv;
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto i = 0; i < 200; ++i) { acc += cr.power(); }
                    if (acc != 18200) { throw "aliasing method ref mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Aliasing method: held by shared_ptr (x200 calls)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    class Creature {
                        string name = "";
                        int hp = 0;
                        int attack = 0;
                        int defense = 0;
                        int level = 0;
                        array<int> stats = [];
                        Creature(string n) {
                            name = n; hp = 100; attack = 12; defense = 7; level = 5;
                            stats = [3, 1, 4, 1, 5, 9, 2, 6];
                        }
                        function power() -> int {
                            auto total = attack * 2 + defense * 3 + level;
                            for (auto s : stats) { total += s; }
                            return total + hp / 10;
                        }
                    }
                    var cs = new Creature("Grubwell");
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto i = 0; i < 200; ++i) { acc += cs.power(); }
                    if (acc != 18200) { throw "aliasing method shared_ptr mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        // Minimal mutation pair: the smallest function that REQUIRES aliasing
        // (by-value heal would mutate a discarded clone)

        benchmark("Aliasing mutate: heal(Unit&) (x200 calls)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    class Unit { int hp = 0; Unit(int h) { hp = h; } }
                    function heal(Unit& u, int amount) { u.hp += amount; }
                )");
                bite = test_engine->jaibite(R"(
                    var u = Unit(0);
                    for (auto i = 0; i < 200; ++i) { heal(u, 5); }
                    if (u.hp != 1000) { throw "aliasing heal ref mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Aliasing mutate: heal(shared_ptr<Unit>) (x200 calls)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    class Unit { int hp = 0; Unit(int h) { hp = h; } }
                    function heal(shared_ptr<Unit> u, int amount) { u.hp += amount; }
                )");
                bite = test_engine->jaibite(R"(
                    var u = new Unit(0);
                    for (auto i = 0; i < 200; ++i) { heal(u, 5); }
                    if (u.hp != 1000) { throw "aliasing heal shared_ptr mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        // === String Performance Benchmarks ===
        // These measure the shared_ptr string optimization effectiveness

        // Container copy by value: the STORE taxonomy on GLOOM named one
        // `pix = G.bg` per frame at 527k cycles - an untyped 4000-int array deep-copied
        // element by element. These rows pin that shape: untyped scalar, typed buffer,
        // string elements, and a map.
        benchmark("Array copy by value: 1000 ints untyped (x20)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    var src = [];
                    for (auto i = 0; i < 1000; ++i) { src.push(i); }
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto k = 0; k < 20; ++k) { var c = src; acc += c[999]; }
                    if (acc != 19980) { throw "array copy mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Array copy by value: array<int> 1000 typed (x20)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    array<int> tsrc = [];
                    for (auto i = 0; i < 1000; ++i) { tsrc.push(i); }
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto k = 0; k < 20; ++k) { array<int> c = tsrc; acc += c[999]; }
                    if (acc != 19980) { throw "typed array copy mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Array copy by value: array<K> 200 objects (x20)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    class BObj { int a = 1; BObj() {} BObj(int v) { a = v; } }
                    array<BObj> osrc = [];
                    for (auto i = 0; i < 200; ++i) { osrc.push(BObj(i)); }
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto k = 0; k < 20; ++k) { array<BObj> c = osrc; acc += c.size(); }
                    if (acc != 4000) { throw "object array copy mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Array copy by value: var 200 objects (x20)", [this]() {
            // the dynamic twin of the array<K> copy row: same instance clones, boxed elements
            if (!bite) {
                test_engine->execute(R"(
                    class VObj { int a = 1; VObj() {} VObj(int v) { a = v; } }
                    var vsrc = [];
                    for (auto i = 0; i < 200; ++i) { vsrc.push(VObj(i)); }
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto k = 0; k < 20; ++k) { var c = vsrc; acc += c.size(); }
                    if (acc != 4000) { throw "object array copy mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Method field writes: 1000 x (a = i; b = a + i) (x20)", [this]() {
            // bare field stores inside a method body (the creature-state shape)
            if (!bite) {
                test_engine->execute(R"(
                    class BW { int a = 0; int b = 0; int run() { for (auto k = 0; k < 20; ++k) { for (auto i = 0; i < 1000; ++i) { a = i; b = a + i; } } return a + b; } }
                    BW bw_obj = BW();
                )");
                bite = test_engine->jaibite(R"(if (bw_obj.run() != 2997) { throw "field write mismatch"; })");
            }
            bite->execute();
        }, 300);

        benchmark("Instance creation: 200 x Vec2(float, float) (x20)", [this]() {
            // construction protocol: create_instance + init/method envs + literal stamps + in-loop ctor body
            if (!bite) {
                test_engine->execute(R"(
                    class BVec2 { float x = 0.0; float y = 0.0; BVec2(float px, float py) { x = px; y = py; } }
                    function make_vecs() -> float { float sx = 0.0; for (auto k = 0; k < 20; ++k) { for (auto i = 0; i < 200; ++i) { auto v = BVec2(1.0, 2.0); sx += v.x; } } return sx; }
                )");
                bite = test_engine->jaibite(R"(if (make_vecs() != 4000.0) { throw "vec mismatch"; })");
            }
            bite->execute();
        }, 300);

        benchmark("Instance creation: 200 x P() default (x20)", [this]() {
            // no constructor: create_instance + literal stamps only
            if (!bite) {
                test_engine->execute(R"(
                    class BP { int a = 1; int b = 2; }
                    function make_ps() -> int { int s = 0; for (auto k = 0; k < 20; ++k) { for (auto i = 0; i < 200; ++i) { auto p = BP(); s += p.a; } } return s; }
                )");
                bite = test_engine->jaibite(R"(if (make_ps() != 4000) { throw "p mismatch"; })");
            }
            bite->execute();
        }, 300);

        benchmark("Instance creation: 200 x shared_ptr<Node>(int) (x20)", [this]() {
            // the BST node shape: typed-ctor value through the shared_ptr arm
            if (!bite) {
                test_engine->execute(R"(
                    class BNode { int value = 0; shared_ptr<BNode> left; shared_ptr<BNode> right; BNode(int v) { value = v; } }
                    function make_nodes() -> int { int s = 0; for (auto k = 0; k < 20; ++k) { for (auto i = 0; i < 200; ++i) { auto n = shared_ptr<BNode>(i); s += n.value; } } return s; }
                )");
                bite = test_engine->jaibite(R"(if (make_nodes() != 398000) { throw "node mismatch"; })");
            }
            bite->execute();
        }, 300);

        benchmark("Push 200 Base into array<Base> (x20)", [this]() {
            // the exact-class admission: one interned-id compare per push
            if (!bite) {
                test_engine->execute(R"(
                    class PBase { int a = 1; }
                    class PDerived : PBase { int b = 2; }
                    PBase pbase_proto = PBase();
                    PDerived pderived_proto = PDerived();
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto k = 0; k < 20; ++k) { array<PBase> arr = []; for (auto i = 0; i < 200; ++i) { arr.push(pbase_proto); } acc += arr.size(); }
                    if (acc != 4000) { throw "push mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Push 200 Derived into array<Base> (x20)", [this]() {
            // the subclass admission: a name mismatch, then the parent-chain walk.
            // Each benchmark runs on a fresh engine (pre_test), so the setup repeats
            if (!bite) {
                test_engine->execute(R"(
                    class PBase { int a = 1; }
                    class PDerived : PBase { int b = 2; }
                    PDerived pderived_proto = PDerived();
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto k = 0; k < 20; ++k) { array<PBase> arr = []; for (auto i = 0; i < 200; ++i) { arr.push(pderived_proto); } acc += arr.size(); }
                    if (acc != 4000) { throw "push mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Object array element field reads: array<K> (x1000)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    class BField { int a = 1; BField() {} BField(int v) { a = v; } }
                    array<BField> fsrc = [BField(1), BField(2), BField(3), BField(4)];
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto i = 0; i < 1000; ++i) { acc += fsrc[i & 3].a; }
                    if (acc != 2500) { throw "object field read mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Boundary: array<K> (200) -> std::vector<std::shared_ptr<K>> (x100)", [this]() {
            // A binding shape: a class-typed script array handed to a C++ parameter of
            // shared pointers to the bound class
            if (!bite) {
                dynamic_binder<bench_creature>(*test_engine, "BenchCreature")
                    .constructor<>()
                    .property("hp", &bench_creature::hp)
                    .build();
                test_engine->add_function("sum_hp", [](std::vector<std::shared_ptr<bench_creature>> cs) -> script_int {
                    script_int total = 0;
                    for (const auto& c : cs) { total += c->hp; }
                    return total;
                });
                test_engine->execute(R"(
                    array<BenchCreature> csrc = [];
                    for (auto i = 0; i < 200; ++i) { csrc.push(BenchCreature()); }
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto k = 0; k < 100; ++k) { acc += sum_hp(csrc); }
                    if (acc != 60000) { throw "boundary sum mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Map lookup: map<string,int> 100 keys, 1000 dynamic-key reads", [this]() {
            // the typed-map yardstick: dynamic
            // string keys build a script string per read today; a typed string-keyed map
            // can compare the bytes without one
            if (!bite) {
                test_engine->execute(R"(
                    map<string,int> mtab = {};
                    array<string> mkeys = [];
                    for (auto i = 0; i < 100; ++i) { string k = "key" + to_string(i); mtab[k] = i; mkeys.push(k); }
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto i = 0; i < 1000; ++i) { acc += mtab[mkeys[i % 100]]; }
                    if (acc != 49500) { throw "map lookup mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Array copy by value: 200 strings (x20)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    var ssrc = [];
                    for (auto i = 0; i < 200; ++i) { ssrc.push("s" + to_string(i)); }
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto k = 0; k < 20; ++k) { var c = ssrc; acc += c.size(); }
                    if (acc != 4000) { throw "string array copy mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("Map copy by value: 100 string keys (x20)", [this]() {
            if (!bite) {
                test_engine->execute(R"(
                    var msrc = {};
                    for (auto i = 0; i < 100; ++i) { msrc["k" + to_string(i)] = i; }
                )");
                bite = test_engine->jaibite(R"(
                    auto acc = 0;
                    for (auto k = 0; k < 20; ++k) { var c = msrc; acc += c["k50"]; }
                    if (acc != 1000) { throw "map copy mismatch"; }
                )");
            }
            bite->execute();
        }, 300);

        benchmark("String Copy (Long String)", [this]() {
            test_engine->execute(R"(
                auto original = "This is a longer string that would be expensive to copy without shared_ptr optimization";
                auto copy1 = original;
                auto copy2 = original;
                auto copy3 = original;
                auto copy4 = original;
                auto copy5 = original;
            )");
        });

        benchmark("String Passing to Function", [this]() {
            test_engine->execute(R"(
                function processString(string s) -> int {
                    return s.length();
                }
                auto longStr = "This is a test string that gets passed to a function multiple times";
                processString(longStr);
                processString(longStr);
                processString(longStr);
            )");
        });

        benchmark("String Methods In Place", [this]() {
            test_engine->execute(R"(
                auto s = "  HELLO WORLD  ";
                s.trim();
                s.to_lower();
                s.replace(" ", "_");
            )");
        });
    }
};

// ============================================================================
// Element-traffic ns micro-harness (dedicated steady_clock resolution, NOT the
// integer-uS Foundry benchmark harness). Isolates array element read/store cost.
// Run with:
//   jaiscript_tests.exe "Element Traffic NS" --verbose            (interpreter)
//   jaiscript_tests.exe "Element Traffic NS" --verbose --backend=vm
// Prints ns/op (body loop minus a same-shape overhead loop, divided by op count).
// ============================================================================
class element_traffic_ns_bench : public suite {
public:
    element_traffic_ns_bench() : suite("Element Traffic NS") {}

    std::shared_ptr<jai::engine> eng;
    void pre_test() override { eng = make_engine(); jai::stdlib::register_all(eng); }

    // The iterations each loop runs (the scripts' OPS): a Release reading needs no more, and the suite stays seconds in Debug
    static constexpr int k_ops = 20000;

    // Per-op ns as (body - overhead) / ops: both scripts share their shape save the measured op, so the array build and the
    // loop cancel
    void row(const char* label, std::string body, std::string overhead) {
        for (std::string* script : {&body, &overhead}) {
            script->replace(script->find("OPS"), 3, std::to_string(k_ops));
        }
        auto body_bite = eng->jaibite(body);
        auto overhead_bite = eng->jaibite(overhead);
        auto timed = [](jai::jaibite& bite) { return [&bite] { return time_seconds([&bite] { bite.execute(); }); }; };
        const speed_ratio speed = compare_speed(std::string("[ns-harness] ") + label + " against its overhead loop", timed(overhead_bite), timed(body_bite), 9);
        std::cout << "  [ns-harness] " << label << ": " << (speed.measured_seconds - speed.reference_seconds) * 1e9 / k_ops << " ns/op\n";
    }

    void forge_tests() override {
        // OPS-iteration inner loops over a 1024-int array; index masked to stay in
        // range and defeat const-fold. Build the array the same way in both scripts.
        const char* build = "array<int> a = []; int j = 0; while (j < 1024) { a.push(j); j = j + 1; } ";

        test("element_read", [this, build]() {
            std::string body = std::string(build) +
                "int s = 0; int i = 0; while (i < OPS) { s = s + a[i & 1023]; i = i + 1; } s;";
            std::string ovh = std::string(build) +
                "int s = 0; int i = 0; while (i < OPS) { s = s + (i & 1023); i = i + 1; } s;";
            row("array element read  s += a[i&1023]", body, ovh);
            check_true(true);
        });

        test("element_compound_store", [this, build]() {
            std::string body = std::string(build) +
                "int i = 0; while (i < OPS) { a[i & 1023] += 1; i = i + 1; } a[0];";
            std::string ovh = std::string(build) +
                "int i = 0; int t = 0; while (i < OPS) { t += 1; i = i + 1; } t;";
            row("compound element store  a[i&1023] += 1", body, ovh);
            check_true(true);
        });

        test("element_read_write_store", [this, build]() {
            std::string body = std::string(build) +
                "int i = 0; while (i < OPS) { a[i & 1023] = a[i & 1023] + 1; i = i + 1; } a[0];";
            std::string ovh = std::string(build) +
                "int i = 0; int t = 0; while (i < OPS) { t = t + 1; i = i + 1; } t;";
            row("read+write element store  a[i]=a[i]+1", body, ovh);
            check_true(true);
        });

        test("element_read_ident_index", [this, build]() {
            // Plain-ident index (the fusable operand shape; a[i&1023] above defeats fusion)
            std::string body = std::string(build) +
                "int s = 0; int i = 0; int k = 0; while (i < OPS) { s = s + a[k]; k = (k + 1) & 1023; i = i + 1; } s;";
            std::string ovh = std::string(build) +
                "int s = 0; int i = 0; int k = 0; while (i < OPS) { s = s + k; k = (k + 1) & 1023; i = i + 1; } s;";
            row("element read ident idx  s += a[k]", body, ovh);
            check_true(true);
        });

        test("element_compound_ident_index", [this, build]() {
            std::string body = std::string(build) +
                "int i = 0; int k = 0; while (i < OPS) { a[k] += 1; k = (k + 1) & 1023; i = i + 1; } a[0];";
            std::string ovh = std::string(build) +
                "int i = 0; int k = 0; int t = 0; while (i < OPS) { t += 1; k = (k + 1) & 1023; i = i + 1; } t;";
            row("compound ident idx  a[k] += 1", body, ovh);
            check_true(true);
        });

        test("render_inner_loop", [this, build]() {
            // Mirror the GLOOM wall-slice paint shape: element store + element read +
            // shift + 3 compound-adds + compare, over int arrays.
            const char* strips = "array<int> strip = []; int k = 0; while (k < 2048) { strip.push(k & 255); k = k + 1; } "
                                 "array<int> pix = []; int p = 0; while (p < 4096) { pix.push(0); p = p + 1; } ";
            std::string body = std::string(strips) +
                "int tacc = 0; int tstep = 137; int gi = 0; int vw = 1; int y = 0; "
                "while (y < OPS) { pix[gi & 4095] = strip[(tacc >> 11) & 2047]; tacc = tacc + tstep; gi = gi + vw; y = y + 1; } pix[0];";
            std::string ovh = std::string(strips) +
                "int tacc = 0; int tstep = 137; int gi = 0; int vw = 1; int y = 0; int t = 0; "
                "while (y < OPS) { t = (tacc >> 11) & 2047; tacc = tacc + tstep; gi = gi + vw; y = y + 1; } t;";
            row("render inner loop (per iter)", body, ovh);
            check_true(true);
        });
    }
};

} // namespace jai::foundry::tests

// Auto-register this test suite with Foundry
FOUNDRY_REGISTER(jai::foundry::tests::performance_benchmarks)
using element_traffic_ns_bench = jai::foundry::tests::element_traffic_ns_bench;
FOUNDRY_REGISTER(element_traffic_ns_bench)
