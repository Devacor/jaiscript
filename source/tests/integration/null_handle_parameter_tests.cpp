#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <memory>
#include <string>

using namespace jai;
using namespace jai::foundry;

// A script null reaches a host smart-pointer parameter as an empty handle, as C++'s nullptr does, on both backends
namespace null_handle_hosts {
    struct node {
        int64_t value = 0;
        std::shared_ptr<node> next;
    };
    struct opaque {
        int64_t n = 0;
    };
}

namespace jai::foundry::tests {

class null_handle_parameter_tests : public suite {
public:
    null_handle_parameter_tests() : suite("Null Handle Parameters") {}

    static std::shared_ptr<engine> make_bound_engine() {
        using null_handle_hosts::node;
        auto eng = make_engine();
        stdlib::register_all(eng);
        dynamic_binder<node>(*eng, "Node")
            .constructor<>()
            .property("value", &node::value)
            .property("next", &node::next, skip_type_check)
            .method("attach", [](node& self, std::shared_ptr<node> n) { self.next = n; return std::string("node"); })
            .method("attach", [](node& self, int64_t v) { self.value = v; return std::string("int"); })
            .build();
        eng->add_function("is_null", [](std::shared_ptr<node> n) { return n == nullptr; });
        eng->add_function("is_null_cref", [](const std::shared_ptr<node>& n) { return n == nullptr; });
        eng->add_function("opaque_is_null", [](std::shared_ptr<null_handle_hosts::opaque> p) { return p == nullptr; });
        eng->add_function("pick", [](int64_t) { return std::string("int"); });
        eng->add_function("pick", [](std::shared_ptr<node>) { return std::string("node"); });
        eng->add_function("is_expired", [](std::weak_ptr<node> w) { return w.expired(); });
        eng->add_function("is_expired_cref", [](const std::weak_ptr<node>& w) { return w.expired(); });
        eng->add_function("weak_value", [](std::weak_ptr<node> w) -> int64_t {
            auto locked = w.lock();
            return locked ? locked->value : -1;
        });
        return eng;
    }

    void forge_tests() override {
        test("null_reaches_a_shared_ptr_parameter", [this]() {
            auto eng = make_bound_engine();
            check_true(eng->execute("is_null(null)").as<bool>(), "a null literal");
            check_true(eng->execute("is_null(nullptr)").as<bool>(), "nullptr");
            check_true(eng->execute("var n = null; is_null(n)").as<bool>(), "a var holding null");
            check_true(eng->execute("shared_ptr<Node> p = null; is_null(p)").as<bool>(), "a typed handle holding null");
            check_true(!eng->execute("is_null(new Node())").as<bool>(), "a live handle stays live");
            check_eq(std::string("truetruetrue"),
                     eng->execute("function run() -> bool { var z = null; return is_null(z); } string s = \"\"; for (int i = 0; i < 3; i++) { s += to_string(run()); } s").as<std::string>(),
                     "a call site that runs again");
        });

        test("null_reaches_a_const_shared_ptr_reference_parameter", [this]() {
            auto eng = make_bound_engine();
            check_true(eng->execute("is_null_cref(null)").as<bool>());
            check_true(eng->execute("var n = null; is_null_cref(n)").as<bool>());
        });

        test("null_reaches_a_shared_ptr_of_an_unregistered_type", [this]() {
            auto eng = make_bound_engine();
            check_true(eng->execute("opaque_is_null(null)").as<bool>());
        });

        test("null_picks_the_handle_overload", [this]() {
            auto eng = make_bound_engine();
            check_eq(std::string("node int node"), eng->execute("pick(null) + \" \" + pick(1) + \" \" + pick(new Node())").as<std::string>());
            check_eq(std::string("node"), eng->execute("var n = new Node(); n.next = new Node(); string r = n.attach(null); n.next == null ? r : \"kept\"").as<std::string>(),
                     "a method overload set");
            check_eq(std::string("int"), eng->execute("var m = new Node(); m.attach(5)").as<std::string>());
        });

        test("null_reaches_a_weak_ptr_parameter", [this]() {
            auto eng = make_bound_engine();
            check_true(eng->execute("is_expired(null)").as<bool>(), "null is an empty weak_ptr");
            check_true(eng->execute("var n = null; is_expired_cref(n)").as<bool>(), "const weak_ptr<T>&");
        });

        test("a_handle_reaches_a_weak_ptr_parameter_live", [this]() {
            auto eng = make_bound_engine();
            check_true(!eng->execute("auto h = new Node(); is_expired(h)").as<bool>(), "a live shared handle");
            check_true(!eng->execute("auto h2 = new Node(); is_expired_cref(h2)").as<bool>(), "const weak_ptr<T>&");
            check_eq((int64_t)7, eng->execute("auto h3 = new Node(); h3.value = 7; weak_value(h3)").as_int(), "it locks to the same object");
            check_eq((int64_t)9, eng->execute("auto h4 = new Node(); h4.value = 9; weak_ptr<Node> w = h4; weak_value(w)").as_int(), "a script weak_ptr");
            check_true(eng->execute("weak_ptr<Node> gone; { auto h5 = new Node(); gone = h5; } is_expired(gone)").as<bool>(), "an expired script weak_ptr");
        });

        test("null_stores_into_a_shared_ptr_property", [this]() {
            auto eng = make_bound_engine();
            auto root = std::make_shared<null_handle_hosts::node>();
            root->next = std::make_shared<null_handle_hosts::node>();
            eng->add_global("root", eng->make_object(root));
            eng->execute("root.next = null;");
            check_true(root->next == nullptr, "the C++ member is empty");
            check_true(eng->execute("root.next == null").as<bool>(), "and reads back as null");
        });

        test("a_wrong_argument_still_has_no_overload", [this]() {
            auto eng = make_bound_engine();
            check_eq(std::string("No overload of 'is_null' takes (string)"),
                     eng->execute("string m = \"\"; var s = \"x\"; try { is_null(s); } catch (e) { m = e; } m").as<std::string>());
            check_eq(std::string("No overload of 'is_expired' takes (int)"),
                     eng->execute("string m = \"\"; var i = 3; try { is_expired(i); } catch (e) { m = e; } m").as<std::string>());
        });

        // A script weak_ptr watches a script handle, and a host weak_ptr has none to watch: a weak_ptr<T> return is refused
        // with its own text (it raised Cannot create cpp_object of unregistered class 'std::weak_ptr<...>')
        test("a_weak_ptr_return_is_refused_plainly", [this]() {
            auto eng = make_bound_engine();
            auto kept = std::make_shared<null_handle_hosts::node>();
            eng->add_function("watch", [kept]() { return std::weak_ptr<null_handle_hosts::node>(kept); });
            eng->add_function("watch_nothing", []() { return std::weak_ptr<null_handle_hosts::node>(); });
            const std::string refused = "A host function returning std::weak_ptr<T> is not supported; return shared_ptr<T>";
            check_eq(refused, eng->execute("string m = \"\"; try { var w = watch(); } catch (e) { m = e; } m").as<std::string>());
            check_eq(refused, eng->execute("string m = \"\"; try { var w = watch_nothing(); } catch (e) { m = e; } m").as<std::string>());
            check_eq((int64_t)4, eng->execute("2 + 2").as_int());
        });
    }
};

}

using null_handle_parameter_tests = jai::foundry::tests::null_handle_parameter_tests;
FOUNDRY_REGISTER(null_handle_parameter_tests)
