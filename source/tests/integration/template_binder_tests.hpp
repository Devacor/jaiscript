#pragma once

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/static_binder.hpp>
#include <jaiscript/core/static_binder_impl.hpp>
#include <jaiscript/core/template_binder.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/serialization/binary_archive.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <cmath>

namespace jai::foundry::tests {

// Uses unique name to avoid static state conflicts
template<typename T>
struct TplBinderVec2 {
    T x = T{};
    T y = T{};

    TplBinderVec2() = default;
    TplBinderVec2(T x_, T y_) : x(x_), y(y_) {}

    T length_squared() const { return x * x + y * y; }
    TplBinderVec2<T> add(const TplBinderVec2<T>& other) const {
        return TplBinderVec2<T>(x + other.x, y + other.y);
    }
};

// Note: property_owner + JAI_PROPERTY doesn't work in template classes due to
// macro limitations. Use plain members for template types.
template<typename T>
struct TplBinderPoint : public auto_bind_template<TplBinderPoint, T> {
    T px = T{};
    T py = T{};

    TplBinderPoint() = default;
    TplBinderPoint(T x_, T y_) : px(x_), py(y_) {}

    T magnitude() const {
        return static_cast<T>(std::sqrt(static_cast<double>(px * px + py * py)));
    }
};

template<typename K, typename V>
struct TplBinderKVPair {
    K key{};
    V value{};

    TplBinderKVPair() = default;
    TplBinderKVPair(K k, V v) : key(k), value(v) {}

    K get_key() const { return key; }
    V get_value() const { return value; }
};

template<typename T>
struct TplAutoDetectVec {
    T a = T{};
    T b = T{};

    TplAutoDetectVec() = default;
    TplAutoDetectVec(T a_, T b_) : a(a_), b(b_) {}

    T dot() const { return a * b; }
};

// ---- template-instantiation type-identity audit fixture (GLOOM follow-up).
// The shared_ptr intern bug was a type-identity KEYING failure; this pair pins that
// no other path keys two distinct C++ instantiations alike. Same method names,
// per-instantiation distinct behavior - any cross-talk shows immediately.
struct audit_pt_base {
    virtual ~audit_pt_base() = default;
    int base_tag() const { return 99; }
};

template<typename T>
struct audit_pt : audit_pt_base {
    T x{};
    audit_pt() = default;
    explicit audit_pt(T v) : x(v) {}
    int kind() const;                                  // 1 for int, 2 for float
    T doubled() const { return static_cast<T>(x + x); }
};
template<> inline int audit_pt<int>::kind() const { return 1; }
template<> inline int audit_pt<float>::kind() const { return 2; }

// Never registered anywhere: the opaque-token half of the audit.
template<typename T>
struct audit_opaque { T v{}; };

struct TplLeaf { int64_t n = 0; };
struct TplNamedLeaf { int64_t n = 0; };

// One instantiation per script spelling: kind() tells them apart
template<typename T>
struct TplBox {
    T v{};
    int64_t kind() const {
        if constexpr (std::is_same_v<T, std::vector<int64_t>>) { return 1; }
        else if constexpr (std::is_same_v<T, int64_t>) { return 2; }
        else if constexpr (std::is_same_v<T, double>) { return 3; }
        else if constexpr (std::is_same_v<T, float>) { return 4; }
        else if constexpr (std::is_same_v<T, std::shared_ptr<TplLeaf>>) { return 5; }
        else if constexpr (std::is_same_v<T, TplLeaf>) { return 6; }
        else if constexpr (std::is_same_v<T, uint8_t>) { return 7; }
        else if constexpr (std::is_same_v<T, int32_t>) { return 8; }
        else if constexpr (std::is_same_v<T, std::map<std::string, double>>) { return 9; }
        else if constexpr (std::is_same_v<T, TplNamedLeaf>) { return 10; }
        else { return 0; }
    }
};

// Two distinct C++ integer types the script spells alike (int/long on MSVC, long/long long on LP64)
using tpl_same_width_a = std::conditional_t<sizeof(long) == sizeof(int), int, long>;
using tpl_same_width_b = std::conditional_t<sizeof(long) == sizeof(int), long, long long>;

inline void register_audit_points(jai::engine& e) {
    dynamic_binder<audit_pt_base>(e, "PBase")
        .constructor<>()
        .method("base_tag", &audit_pt_base::base_tag)
        .build();
    dynamic_binder<audit_pt<int>>(e, "PointI")
        .constructor<>()
        .constructor<int>()
        .property("x", &audit_pt<int>::x)
        .method("kind", &audit_pt<int>::kind)
        .method("doubled", &audit_pt<int>::doubled)
        .base_class<audit_pt_base>()
        .build();
    dynamic_binder<audit_pt<float>>(e, "PointF")
        .constructor<>()
        .constructor<float>()
        .property("x", &audit_pt<float>::x)
        .method("kind", &audit_pt<float>::kind)
        .method("doubled", &audit_pt<float>::doubled)
        .base_class<audit_pt_base>()
        .build();
}

} // namespace jai::foundry::tests

// JAI_TEMPLATE_BINDER registrations (must be outside namespace)

JAI_TEMPLATE_BINDER_NAMED(TplBinderVec2, jai::foundry::tests::TplBinderVec2, "TplBinderVec2",
    .property("x", &jai::foundry::tests::TplBinderVec2<T>::x)
    .property("y", &jai::foundry::tests::TplBinderVec2<T>::y)
    .method("length_squared", &jai::foundry::tests::TplBinderVec2<T>::length_squared)
);

JAI_TEMPLATE_BINDER_NAMED(TplBinderPoint, jai::foundry::tests::TplBinderPoint, "TplBinderPoint",
    .method("magnitude", &jai::foundry::tests::TplBinderPoint<T>::magnitude)
);

JAI_TEMPLATE_BINDER_2_NAMED(TplBinderKVPair, jai::foundry::tests::TplBinderKVPair, "TplBinderKVPair",
    .property("key", &jai::foundry::tests::TplBinderKVPair<T1, T2>::key)
    .property("value", &jai::foundry::tests::TplBinderKVPair<T1, T2>::value)
    .method("get_key", &jai::foundry::tests::TplBinderKVPair<T1, T2>::get_key)
    .method("get_value", &jai::foundry::tests::TplBinderKVPair<T1, T2>::get_value)
);

JAI_TEMPLATE_BINDER_NAMED(TplAutoDetectVec, jai::foundry::tests::TplAutoDetectVec, "TplAutoDetectVec",
    .property("a", &jai::foundry::tests::TplAutoDetectVec<T>::a)
    .property("b", &jai::foundry::tests::TplAutoDetectVec<T>::b)
    .method("dot", &jai::foundry::tests::TplAutoDetectVec<T>::dot)
);

JAI_STATIC_BINDER(jai::foundry::tests::TplLeaf, "TplLeaf",
    .property("n", &jai::foundry::tests::TplLeaf::n)
);

template<> struct jai::type_name_helper<jai::foundry::tests::TplNamedLeaf> {
    static std::string name() { return "NamedLeaf"; }
};

JAI_TEMPLATE_BINDER_NAMED(TplBox, jai::foundry::tests::TplBox, "TplBox",
    .method("kind", &jai::foundry::tests::TplBox<T>::kind)
);

JAI_BIND_TEMPLATE_NAMED(TplBinderVec2, jai::foundry::tests::TplBinderVec2, int);
JAI_BIND_TEMPLATE_NAMED(TplBinderVec2, jai::foundry::tests::TplBinderVec2, float);
JAI_BIND_TEMPLATE_NAMED(TplBinderVec2, jai::foundry::tests::TplBinderVec2, double);

JAI_BIND_TEMPLATE_2_NAMED(TplBinderKVPair, jai::foundry::tests::TplBinderKVPair, int, std::string);
JAI_BIND_TEMPLATE_2_NAMED(TplBinderKVPair, jai::foundry::tests::TplBinderKVPair, std::string, double);

// The unnamed forms and the variant lists: a host template at global scope
template<typename T>
struct TplRange { T lo{}; T hi{}; };
template<typename A, typename B>
struct TplTagged { A tag{}; B value{}; };
struct TplCell { int64_t col = 0; int64_t row = 0; };
struct TplSegment { TplCell from; TplCell to; int32_t weight = 0; };
namespace jai::foundry::tests {
template<typename T>
struct TplSpan { T first{}; T count{}; T stride{}; };
}

JAI_TEMPLATE_BINDER(TplRange, "TplRange",
    .property("lo", &TplRange<T>::lo)
    .property("hi", &TplRange<T>::hi)
);
JAI_BIND_TEMPLATE(TplRange, int64_t);
JAI_BIND_TEMPLATE(TplRange, double);

JAI_TEMPLATE_BINDER_2(TplTagged, "TplTagged",
    .property("tag", &TplTagged<T1, T2>::tag)
    .property("value", &TplTagged<T1, T2>::value)
);
JAI_BIND_TEMPLATE_2(TplTagged, std::string, int64_t);

JAI_TEMPLATE_BINDER_NAMED(TplSpan, jai::foundry::tests::TplSpan, "TplSpan",
    .property("first", &jai::foundry::tests::TplSpan<T>::first)
    .property("count", &jai::foundry::tests::TplSpan<T>::count)
    .property("stride", &jai::foundry::tests::TplSpan<T>::stride)
);
JAI_BIND_TEMPLATE_NAMED(TplSpan, jai::foundry::tests::TplSpan, int64_t);
JAI_BIND_TEMPLATE_NAMED(TplSpan, jai::foundry::tests::TplSpan, int);
JAI_BIND_TEMPLATE_NAMED(TplSpan, jai::foundry::tests::TplSpan, double);

JAI_STATIC_BINDER_AUTO(TplCell,
    .property("col", &TplCell::col)
    .property("row", &TplCell::row)
);
JAI_STATIC_BINDER_AUTO(TplSegment,
    .property("from", &TplSegment::from)
    .property("to", &TplSegment::to)
    .property("weight", &TplSegment::weight)
);

namespace jai::foundry::tests {

class template_binder_tests : public suite {
public:
    template_binder_tests() : suite("Template Binder") {}

    void forge_tests() override {

        // -------------------- template-instantiation type-identity audit
        // Matrix result at audit time: registration/lookup, construction+dispatch,
        // shared_ptr paths (incl. the fixed composite intern), container elements,
        // conversion_registry, base upcasts, opaque tokens - ALL keyed correctly
        // (std::type_index or pointee-id keys); these pins keep them that way.
        // type_of() reports the REGISTERED NAME for registered instances (it agrees with
        // reflect::type_name), so two instantiations sharing one name are still
        // indistinguishable through it; registration_fingerprint does distinguish them.

        test("audit_instantiations_register_and_dispatch_distinctly", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                register_audit_points(*e);
                // (a) registration/lookup: distinct definitions by name AND type_index
                auto def_i = detail::engine_access::get_class_definition(*e, "PointI");
                auto def_f = detail::engine_access::get_class_definition(*e, "PointF");
                check_not_null(def_i.get());
                check_not_null(def_f.get());
                check(def_i.get() != def_f.get(), "PointI/PointF share a class_definition");
                check(detail::engine_access::get_class_definition_by_type(*e, std::type_index(typeid(audit_pt<int>))).get() == def_i.get(), "type_index->PointI");
                check(detail::engine_access::get_class_definition_by_type(*e, std::type_index(typeid(audit_pt<float>))).get() == def_f.get(), "type_index->PointF");
                // (b) construction + method dispatch: same names, distinct behavior
                check_eq((int64_t)1, e->execute("auto i = PointI(21); i.kind()").as_int());
                check_eq((int64_t)2, e->execute("auto f = PointF(1.5); f.kind()").as_int());
                check_eq((int64_t)42, e->execute("i.doubled()").as_int());
                check_near(3.0, e->execute("f.doubled()").as_float(), 0.0001);
            }
        });

        test("audit_shared_ptr_paths_key_per_instantiation", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                register_audit_points(*e);
                // (c) the composite intern path (the fixed one: new/shared_ptr<T>() nodes)
                // and the factory path must both distinguish instantiations
                check_eq((int64_t)1, e->execute("var a = new PointI(); a.kind()").as_int());
                check_eq((int64_t)2, e->execute("var b = new PointF(); b.kind()").as_int());
                check_eq((int64_t)1, e->execute("auto c = shared_ptr<PointI>(); c.kind()").as_int());
                check_eq((int64_t)2, e->execute("auto d = shared_ptr<PointF>(); d.kind()").as_int());
                check_eq((int64_t)1, e->execute("shared_ptr<PointI> g = PointI(); g.kind()").as_int());
                check_eq((int64_t)2, e->execute("shared_ptr<PointF> h = PointF(); h.kind()").as_int());
                // wrong-pointee decl errors (typed sp enforcement ruling)
                check_throws([&]() { e->execute("shared_ptr<PointF> w = PointI();"); });
                // host-side: the factory returns distinct type_infos
                auto* ti_i = detail::engine_access::get_type_info_object(*e, detail::engine_access::symbolize(*e, "PointI"));
                auto* ti_f = detail::engine_access::get_type_info_object(*e, detail::engine_access::symbolize(*e, "PointF"));
                check(ti_i != nullptr && ti_f != nullptr && ti_i != ti_f, "object type_infos distinct");
                check(detail::engine_access::get_type_info_shared_ptr(*e, ti_i) != detail::engine_access::get_type_info_shared_ptr(*e, ti_f),
                      "shared_ptr type_infos collapsed across instantiations");
            }
        });

        test("audit_container_elements_and_upcast_keep_identity", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                register_audit_points(*e);
                // (d) container elements: each array's elements keep their own class
                check_eq((int64_t)1, e->execute("array<PointI> ai = [PointI(5)]; ai[0].kind()").as_int());
                check_eq((int64_t)2, e->execute("array<PointF> af = [PointF(2.5)]; af[0].kind()").as_int());
                // (f) upcast to the common base from both: each keeps ITS class
                check_eq((int64_t)1, e->execute("shared_ptr<PBase> u1 = PointI(7); u1.kind()").as_int());
                check_eq((int64_t)2, e->execute("shared_ptr<PBase> u2 = PointF(7.0); u2.kind()").as_int());
                check_eq((int64_t)99, e->execute("u1.base_tag()").as_int());
                check_eq((int64_t)99, e->execute("u2.base_tag()").as_int());
            }
        });

        test("audit_conversion_registry_keys_per_instantiation", [this]() {
            // (e) a conversion registered for ONE instantiation is invisible to the
            // other. Uses the never-bound audit_opaque pair: dynamic_binder installs
            // converters for every class it registers (a REGISTERED sibling reporting
            // has_conversion()==true is the binder working, not key cross-talk).
            auto e = jai::foundry::make_engine();
            auto reg = detail::engine_access::get_conversion_registry(*e);
            check_not_null(reg.get());
            check_false(reg->has_conversion<audit_opaque<int>>());
            check_false(reg->has_conversion<audit_opaque<float>>());
            reg->register_conversion<audit_opaque<int>>(
                [](const script_value& v) { audit_opaque<int> p; p.v = static_cast<int>(v.as_int()); return p; },
                [eng = e.get()](const audit_opaque<int>& p) { return script_value(static_cast<script_int>(p.v), eng); });
            check_true(reg->has_conversion<audit_opaque<int>>());
            check_false(reg->has_conversion<audit_opaque<float>>());   // keyed on std::type_index: no inheritance
            auto roundtrip = reg->convert_from_script<audit_opaque<int>>(script_value((script_int)11, e.get()));
            check_eq(11, roundtrip.v);
        });

        test("audit_opaque_tokens_stay_pointer_identity", [this]() {
            // (g) unregistered instantiations passed through the host are opaque
            // tokens: non-null, pointer identity, no cross-instantiation equality
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                register_audit_points(*e);
                audit_opaque<double> od1; audit_opaque<double> od2; audit_opaque<int> oi;
                e->add_global("od1", e->make_value(&od1));
                e->add_global("od1b", e->make_value(&od1));   // same pointer, second token
                e->add_global("od2", e->make_value(&od2));
                e->add_global("oi", e->make_value(&oi));
                check_true(e->execute("od1 != null").as_bool());
                check_true(e->execute("od1 == od1b").as_bool());   // pointer identity
                check_false(e->execute("od1 == od2").as_bool());
                check_false(e->execute("od1 == oi").as_bool());    // no cross-instantiation equality
            }
        });

        test("audit_registration_fingerprint_distinguishes_instantiations", [this]() {
            // (h) type_of reports the registered NAME, which both instantiations share, so
            // it does not distinguish them; the registration fingerprint DOES.
            auto e_i = jai::foundry::make_engine();
            dynamic_binder<audit_pt<int>>(*e_i, "AuditP").constructor<>().method("kind", &audit_pt<int>::kind).build();
            auto e_f = jai::foundry::make_engine();
            dynamic_binder<audit_pt<float>>(*e_f, "AuditP").constructor<>().method("kind", &audit_pt<float>::kind).build();
            // same registered NAME + shape, but the instances behave as their own C++ types
            check_eq((int64_t)1, e_i->execute("AuditP().kind()").as_int());
            check_eq((int64_t)2, e_f->execute("AuditP().kind()").as_int());
            jai::stdlib::register_all(*e_i);
            check_eq(std::string("AuditP"), e_i->execute("type_of(AuditP())").as<std::string>());
            // and differently-shaped registrations fingerprint differently
            auto e_two = jai::foundry::make_engine();
            register_audit_points(*e_two);
            check_ne(e_i->registration_fingerprint(), e_two->registration_fingerprint());
        });

        test("type_name_helper_primitives", [this]() {
            check_eq(type_name_helper<int>::name(), std::string("int32"), "int32 name");
            check_eq(type_name_helper<float>::name(), std::string("float32"), "C++ float is the script's float32");
            check_eq(type_name_helper<double>::name(), std::string("float"), "C++ double is the script's (64-bit) float");
            check_eq(type_name_helper<bool>::name(), std::string("bool"), "bool name");
            check_eq(type_name_helper<char>::name(), std::string("char"), "char name");
            check_eq(type_name_helper<std::string>::name(), std::string("string"), "string name");
        });

        test("type_name_helper_spells_what_the_parser_spells", [this]() {
            // every integer type by width and sign (long and long long included), containers and handles by their script types
            check_eq(type_name_helper<long long>::name(), std::string("int"), "long long");
            check_eq(type_name_helper<unsigned long long>::name(), std::string("uint"), "unsigned long long");
            check_eq(type_name_helper<long>::name(), std::string(sizeof(long) == 8 ? "int" : "int32"), "long");
            check_eq(type_name_helper<signed char>::name(), std::string("int8"), "signed char");
            check_eq(type_name_helper<std::vector<int64_t>>::name(), std::string("array<int>"), "vector<int64_t>");
            check_eq(type_name_helper<std::vector<std::vector<double>>>::name(), std::string("array<array<float>>"), "nested vector");
            check_eq(type_name_helper<std::map<std::string, int64_t>>::name(), std::string("map<string,int>"), "map");
            check_eq(type_name_helper<std::shared_ptr<TplLeaf>>::name(), std::string("shared_ptr<TplLeaf>"), "shared_ptr of a static type");
            check_eq(type_name_helper<std::weak_ptr<TplLeaf>>::name(), std::string("weak_ptr<TplLeaf>"), "weak_ptr");
            check_eq(type_name_helper<TplLeaf>::name(), std::string("TplLeaf"), "a JAI_STATIC_BINDER type");
            check_eq(type_name_helper<TplBinderVec2<int64_t>>::name(), std::string("TplBinderVec2<int>"), "a template-bound type");
            check_eq(type_name_helper<TplNamedLeaf>::name(), std::string("NamedLeaf"), "a host specialization");
            check_eq(jai_template_binder_TplBox<std::shared_ptr<TplLeaf>>::type_name(), std::string("TplBox<shared_ptr<TplLeaf>>"),
                     "a handle argument keeps its shared_ptr spelling");
        });

        test("type_name_helper_integers", [this]() {
            check_eq(type_name_helper<int8_t>::name(), std::string("int8"), "int8 name");
            check_eq(type_name_helper<int16_t>::name(), std::string("int16"), "int16 name");
            check_eq(type_name_helper<int>::name(), std::string("int32"), "int32 name");
            check_eq(type_name_helper<int64_t>::name(), std::string("int"), "int (native 64-bit)");
            check_eq(type_name_helper<uint8_t>::name(), std::string("uint8"), "uint8 name");
            check_eq(type_name_helper<uint16_t>::name(), std::string("uint16"), "uint16 name");
            check_eq(type_name_helper<uint32_t>::name(), std::string("uint32"), "uint32 name");
            check_eq(type_name_helper<uint64_t>::name(), std::string("uint"), "uint (native 64-bit)");
        });

        test("template_binder_type_names", [this]() {
            auto int_name = jai_template_binder_TplBinderVec2<int>::type_name();
            auto float_name = jai_template_binder_TplBinderVec2<float>::type_name();
            auto double_name = jai_template_binder_TplBinderVec2<double>::type_name();

            check_eq(int_name, std::string("TplBinderVec2<int32>"), "int32 variant name");
            check_eq(float_name, std::string("TplBinderVec2<float32>"), "float variant name");
            check_eq(double_name, std::string("TplBinderVec2<float>"), "double variant name");
        });

        test("unnamed_forms_and_variant_lists_register_every_instantiation", [this]() {
            check(jai_static_type<::TplRange<int64_t>>::registered, "TplRange<int64_t> registered");
            check(jai_static_type<::TplRange<double>>::registered, "TplRange<double> registered");
            check_eq(jai_static_type<::TplRange<int64_t>>::name, std::string("TplRange<int>"), "int64_t spells int");
            check_eq(jai_static_type<::TplRange<double>>::name, std::string("TplRange<float>"), "double spells float");
            check_eq(jai_static_type<::TplRange<double>>::binder.property_count(), size_t(2), "lo, hi");
            check_eq(jai_static_type<::TplTagged<std::string, int64_t>>::name, std::string("TplTagged<string, int>"), "two parameters");
            check_eq(jai_static_type<::TplTagged<std::string, int64_t>>::binder.property_count(), size_t(2), "tag, value");
            check_eq(jai_static_type<TplSpan<int64_t>>::name, std::string("TplSpan<int>"), "named variant 1");
            check_eq(jai_static_type<TplSpan<int>>::name, std::string("TplSpan<int32>"), "named variant 2");
            check_eq(jai_static_type<TplSpan<double>>::name, std::string("TplSpan<float>"), "named variant 3");
            check_eq(jai_static_type<TplSpan<int>>::binder.property_count(), size_t(3), "first, count, stride");
            check(!jai_static_type<TplSpan<float>>::registered, "an instantiation the list does not name stays unregistered");
            check_eq(std::string(jai_static_type<::TplCell>::name), std::string("TplCell"), "JAI_STATIC_BINDER_AUTO spells the C++ name");
            check_eq(jai_static_type<::TplCell>::binder.property_count(), size_t(2), "col, row");
        });

        test("template_binder_2_type_names", [this]() {
            auto name1 = jai_template_binder_TplBinderKVPair<int, std::string>::type_name();
            auto name2 = jai_template_binder_TplBinderKVPair<std::string, double>::type_name();

            check_eq(name1, std::string("TplBinderKVPair<int32, string>"), "int32,string variant");
            check_eq(name2, std::string("TplBinderKVPair<string, float>"), "string,double variant");
        });

        test("script_template_arguments_reach_their_instantiations", [this]() {
            // the parser spells each argument canonically, as the host does: array<int> is not array, shared_ptr<TplLeaf>
            // is not TplLeaf, double is float, C++ float is float32, uint8 is a template argument
            auto eng = make_engine();
            eng->bind_static_types<TplBox<std::vector<int64_t>>, TplBox<int64_t>, TplBox<double>, TplBox<float>,
                                   TplBox<std::shared_ptr<TplLeaf>>, TplBox<TplLeaf>, TplBox<uint8_t>,
                                   TplBox<std::map<std::string, double>>, TplBox<TplNamedLeaf>>();
            check_eq((int64_t)1, eng->execute("TplBox<array<int>>().kind()").as_int(), "array<int>");
            check_eq((int64_t)2, eng->execute("TplBox<int>().kind()").as_int(), "int");
            check_eq((int64_t)8, eng->execute("TplBox<int32>().kind()").as_int(), "int32 (the int64 binding's sibling)");
            check_eq((int64_t)3, eng->execute("TplBox<double>().kind()").as_int(), "double reaches the C++ double");
            check_eq((int64_t)3, eng->execute("TplBox<float>().kind()").as_int(), "script float is the C++ double");
            check_eq((int64_t)4, eng->execute("TplBox<float32>().kind()").as_int(), "float32 reaches the C++ float");
            check_eq((int64_t)5, eng->execute("TplBox<shared_ptr<TplLeaf>>().kind()").as_int(), "shared_ptr<TplLeaf>");
            check_eq((int64_t)6, eng->execute("TplBox<TplLeaf>().kind()").as_int(), "TplLeaf");
            check_eq((int64_t)7, eng->execute("TplBox<uint8>().kind()").as_int(), "uint8");
            check_eq((int64_t)9, eng->execute("TplBox<map<string, double>>().kind()").as_int(), "map<string, double>");
            check_eq((int64_t)10, eng->execute("TplBox<NamedLeaf>().kind()").as_int(), "a host-named argument");
            check_eq((int64_t)5, eng->execute("TplBox<shared_ptr<TplLeaf>> b; b.kind()").as_int(), "a declaration");
            check_eq((int64_t)1, eng->execute("auto a = TplBox<array<int>>{}; a.kind()").as_int(), "a brace construction");
            check_eq((int64_t)4, eng->execute("function k(TplBox<float32>& b) -> int { return b.kind(); } TplBox<float32> f; k(f)").as_int(),
                     "a reference parameter");
        });

        test("a_template_base_name_is_not_a_reserved_word", [this]() {
            auto eng = make_engine();
            eng->bind_static_type<TplBox<int64_t>>();
            check_eq((int64_t)6, eng->execute("int TplBox = 5; TplBox + 1").as_int(), "a variable named like the template");
            check_true(eng->execute("TplBox < 10").as_bool(), "a comparison is not a template argument list");
            check_true(eng->execute("TplBox < 10 && 3 > 2").as_bool(), "nor is a comparison chain");
            check_eq((int64_t)2, eng->execute("TplBox<int>().kind()").as_int(), "the template still constructs");
            auto fresh = make_engine();
            fresh->bind_static_type<TplBox<int64_t>>();
            check_eq((int64_t)7, fresh->execute("class TplBox { int v = 7; } TplBox().v").as_int(), "a script class named like the template");
            check_eq((int64_t)2, fresh->execute("TplBox<int> t; t.kind()").as_int(), "beside the template's instantiations");
        });

        test("two_cpp_types_one_script_spelling_raise_at_bind", [this]() {
            auto eng = make_engine();
            eng->bind_static_type<TplBox<tpl_same_width_a>>();
            check_throws([&]() { eng->bind_static_type<TplBox<tpl_same_width_b>>(); });
            eng->bind_static_type<TplBox<tpl_same_width_a>>();   // the same type again stays idempotent
        });

        test("static_type_registry_engine_state", [this]() {
            static_assert(static_type_registry::is_registered<TplBinderVec2<int>>(), "a JAI_BIND_TEMPLATE instantiation");
            static_assert(static_type_registry::is_registered<TplBinderPoint<float>>(), "an auto_bind_template instantiation");
            static_assert(!static_type_registry::is_registered<TplBox<int64_t>>(), "a template binder alone declares nothing");
            auto eng = make_engine();
            check_false(static_type_registry::is_registered<TplBox<int64_t>>(*eng), "not bound in this engine yet");
            eng->bind_static_type<TplBox<int64_t>>();
            check_true(static_type_registry::is_registered<TplBox<int64_t>>(*eng), "bound in this engine");
            check_false(static_type_registry::is_registered<TplBox<int64_t>>(*make_engine()), "another engine is its own");
        });

        test("static_type_registry_registration", [this]() {
            check(static_type_registry::is_registered<TplBinderVec2<int>>(),
                  "TplBinderVec2<int> is registered");
            check(static_type_registry::is_registered<TplBinderVec2<float>>(),
                  "TplBinderVec2<float> is registered");
            check(static_type_registry::is_registered<TplBinderVec2<double>>(),
                  "TplBinderVec2<double> is registered");

            check(!static_type_registry::is_registered<TplBinderVec2<int64_t>>(),
                  "TplBinderVec2<int64_t> NOT registered");
        });

        test("static_type_registry_two_param", [this]() {
            check(static_type_registry::is_registered<TplBinderKVPair<int, std::string>>(),
                  "TplBinderKVPair<int, string> is registered");
            check(static_type_registry::is_registered<TplBinderKVPair<std::string, double>>(),
                  "TplBinderKVPair<string, double> is registered");

            check(!static_type_registry::is_registered<TplBinderKVPair<int, int>>(),
                  "TplBinderKVPair<int, int> NOT registered");
        });

        test("has_static_type_for_template_variants", [this]() {
            check(has_static_type_v<TplBinderVec2<int>>,
                  "TplBinderVec2<int> has static type");
            check(has_static_type_v<TplBinderVec2<float>>,
                  "TplBinderVec2<float> has static type");

            check(has_static_type_v<TplBinderKVPair<int, std::string>>,
                  "TplBinderKVPair<int, string> has static type");
        });

        test("bind_template_variant_to_engine", [this]() {
            auto eng = make_engine();

            eng->bind_static_type<TplBinderVec2<int>>();

            eng->execute("auto v = TplBinderVec2<int32>();");

            eng->execute("v.x = 3;");
            eng->execute("v.y = 4;");

            check_eq(eng->execute("v.x").as<int>(), 3, "x property works");
            check_eq(eng->execute("v.y").as<int>(), 4, "y property works");

            check_eq(eng->execute("v.length_squared()").as<int>(), 25, "length_squared method works");
        });

        test("bind_multiple_template_variants", [this]() {
            auto eng = make_engine();

            eng->bind_static_type<TplBinderVec2<int>>();
            eng->bind_static_type<TplBinderVec2<float>>();
            eng->bind_static_type<TplBinderVec2<double>>();

            eng->execute("auto vi = TplBinderVec2<int32>();");
            eng->execute("vi.x = 1; vi.y = 2;");
            check_eq(eng->execute("vi.length_squared()").as<int>(), 5, "int32 variant works");

            eng->execute("auto vf = TplBinderVec2<float32>();");
            eng->execute("vf.x = 1.5; vf.y = 2.0;");
            auto len_sq = eng->execute("vf.length_squared()").as<float>();
            check(std::abs(len_sq - 6.25f) < 0.001f, "float variant works");

            eng->execute("auto vd = TplBinderVec2<double>();");
            eng->execute("vd.x = 3.0; vd.y = 4.0;");
            check_eq(eng->execute("vd.length_squared()").as<double>(), 25.0, "double variant works");

            // each spelling holds its own width: 0.1 rounds in the float, stays exact in the double
            eng->execute("vf.x = 0.1; vd.x = 0.1;");
            check_true(eng->execute("vf.x != 0.1").as_bool(), "float32 stores binary32");
            check_true(eng->execute("vd.x == 0.1").as_bool(), "double (script float) stores binary64");
        });

        test("bind_two_param_template_variant", [this]() {
            auto eng = make_engine();

            eng->bind_static_type<TplBinderKVPair<int, std::string>>();

            eng->execute("auto kv = TplBinderKVPair<int32, string>();");
            eng->execute("kv.key = 42;");
            eng->execute("kv.value = \"hello\";");

            check_eq(eng->execute("kv.get_key()").as<int>(), 42, "get_key works");
            check_eq(eng->execute("kv.get_value()").as<std::string>(), "hello", "get_value works");
        });

        test("auto_bind_template_registers_on_instantiation", [this]() {
            TplBinderPoint<int> pt;
            (void)pt;  // Suppress unused warning
            check(static_type_registry::is_registered<TplBinderPoint<int>>(),
                  "TplBinderPoint<int> auto-registered via CRTP");
        });

        test("auto_bind_template_different_params", [this]() {
            TplBinderPoint<float> pt_float;
            TplBinderPoint<double> pt_double;
            (void)pt_float;
            (void)pt_double;

            check(static_type_registry::is_registered<TplBinderPoint<float>>(),
                  "TplBinderPoint<float> auto-registered");
            check(static_type_registry::is_registered<TplBinderPoint<double>>(),
                  "TplBinderPoint<double> auto-registered");
        });

        test("template_variant_serialization", [this]() {
            TplBinderVec2<int> v1{10, 20};

            std::vector<uint8_t> buffer;
            {
                serialization::binary_archive_writer ar(buffer);
                ar.begin_object("TplBinderVec2<int32>", 1);
                jai_static_type<TplBinderVec2<int>>::save(ar, v1);
                ar.end_object();
            }

            auto eng = make_engine();
            TplBinderVec2<int> v2;
            {
                serialization::binary_archive_reader ar(buffer, eng.get());
                std::string type_name;
                uint32_t version;
                ar.begin_object(type_name, version);
                jai_static_type<TplBinderVec2<int>>::load(ar, v2);
                ar.end_object();
            }

            check_eq(v2.x, int{10}, "x serialized correctly");
            check_eq(v2.y, int{20}, "y serialized correctly");
        });

        // A static binder's load finds each property by its name, so JSON reads back whatever order its keys come in; a missing key
        // keeps the member's value, and a nested static type is a record of its own
        test("static_binder_json_round_trip", [this]() {
            auto eng = make_engine();
            auto save = [&](const ::TplSegment& segment) {
                serialization::json_archive_writer ar(0, eng.get());
                ar.begin_object("TplSegment", 1);
                jai_static_type<::TplSegment>::save(ar, segment);
                ar.end_object();
                return ar.str();
            };
            auto load = [&](const std::string& text) {
                ::TplSegment segment;
                segment.weight = -1;
                serialization::json_archive_reader ar(text, eng.get());
                ar.begin_object();
                jai_static_type<::TplSegment>::load(ar, segment);
                ar.end_object();
                return segment;
            };
            ::TplSegment original{ { 1, 2 }, { 3, 4 }, 9 };
            const std::string text = save(original);
            check_eq(std::string(R"({"$type": "TplSegment","from": {"col": 1,"row": 2},"to": {"col": 3,"row": 4},"weight": 9})"), text);
            auto back = load(text);
            check_eq((int64_t)1, back.from.col);
            check_eq((int64_t)4, back.to.row);
            check_eq(9, back.weight);
            auto swapped = load(R"({"weight": 5,"to": {"row": 8,"col": 7},"from": {"row": 6,"col": 5}})");
            check_eq((int64_t)5, swapped.from.col);
            check_eq((int64_t)6, swapped.from.row);
            check_eq((int64_t)7, swapped.to.col);
            check_eq(5, swapped.weight);
            check_eq(-1, load(R"({"from": {"col": 1}})").weight, "a missing key keeps the member's value");

            std::vector<uint8_t> buffer;
            {
                serialization::binary_archive_writer ar(buffer);
                ar.begin_object("TplSegment", 1);
                jai_static_type<::TplSegment>::save(ar, original);
                ar.end_object();
            }
            ::TplSegment from_binary;
            serialization::binary_archive_reader ar(buffer, eng.get());
            ar.begin_object();
            jai_static_type<::TplSegment>::load(ar, from_binary);
            ar.end_object();
            check_eq((int64_t)3, from_binary.to.col);
            check_eq(9, from_binary.weight);
        });

        test("template_binder_property_counts", [this]() {
            constexpr size_t vec2_props = jai_static_type<TplBinderVec2<int>>::binder.property_count();
            constexpr size_t vec2_methods = jai_static_type<TplBinderVec2<int>>::binder.method_count();

            check_eq(vec2_props, size_t(2), "TplBinderVec2 has 2 properties");
            check_eq(vec2_methods, size_t(1), "TplBinderVec2 has 1 method");

            constexpr size_t kv_props = jai_static_type<TplBinderKVPair<int, std::string>>::binder.property_count();
            constexpr size_t kv_methods = jai_static_type<TplBinderKVPair<int, std::string>>::binder.method_count();

            check_eq(kv_props, size_t(2), "TplBinderKVPair has 2 properties");
            check_eq(kv_methods, size_t(2), "TplBinderKVPair has 2 methods");
        });

        test("shared_template_object_access", [this]() {
            auto eng = make_engine();

            eng->bind_static_type<TplBinderVec2<double>>();

            auto cpp_vec = std::make_shared<TplBinderVec2<double>>(5.0, 12.0);
            eng->add_global("vec", eng->make_object(cpp_vec));

            check_eq(eng->execute("vec.x").as<double>(), 5.0, "Script reads x");
            check_eq(eng->execute("vec.y").as<double>(), 12.0, "Script reads y");

            check_eq(eng->execute("vec.length_squared()").as<double>(), 169.0, "Script calls method");

            eng->execute("vec.x = 8.0;");
            check_eq(cpp_vec->x, 8.0, "C++ sees script modification");

            check_eq(eng->execute("vec.length_squared()").as<double>(), 208.0, "Method reflects change");
        });

        test("bind_static_type_is_idempotent", [this]() {
            auto eng = make_engine();

            eng->bind_static_type<TplBinderVec2<int>>();
            eng->bind_static_type<TplBinderVec2<int>>();
            eng->bind_static_type<TplBinderVec2<int>>();

            eng->execute("auto v = TplBinderVec2<int32>();");
            eng->execute("v.x = 7; v.y = 24;");
            check_eq(eng->execute("v.length_squared()").as<int>(), 625, "Type still works after multiple binds");
        });

        test("bind_static_types_multiple_same", [this]() {
            auto eng = make_engine();

            eng->bind_static_types<TplBinderVec2<float>, TplBinderVec2<float>, TplBinderVec2<double>>();

            eng->execute("auto vf = TplBinderVec2<float32>();");
            eng->execute("auto vd = TplBinderVec2<double>();");
            check(true, "Multiple binds of same type didn't crash");
        });

        test("has_template_binder_detection", [this]() {
            check(has_template_binder_v<TplAutoDetectVec<int>>,
                  "TplAutoDetectVec<int> detected via has_template_binder_v");
            check(has_template_binder_v<TplAutoDetectVec<float>>,
                  "TplAutoDetectVec<float> detected");
            check(has_template_binder_v<TplBinderVec2<double>>,
                  "TplBinderVec2<double> detected");

            check(has_template_binder_2_v<TplBinderKVPair<int, std::string>>,
                  "TplBinderKVPair<int, string> detected via has_template_binder_2_v");

            check(has_any_template_binder_v<TplAutoDetectVec<int>>,
                  "TplAutoDetectVec<int> detected via has_any_template_binder_v");
            check(has_any_template_binder_v<TplBinderKVPair<std::string, double>>,
                  "TplBinderKVPair detected via has_any_template_binder_v");

            check(!has_template_binder_v<std::string>,
                  "std::string NOT detected as template binder type");
            check(!has_any_template_binder_v<int>,
                  "int NOT detected as template binder type");
        });

        test("template_binder_accessor_type_name", [this]() {
            auto name_int = template_binder_accessor<TplAutoDetectVec<int>>::type_name();
            auto name_float = template_binder_accessor<TplAutoDetectVec<float>>::type_name();

            check_eq(name_int, std::string("TplAutoDetectVec<int32>"), "accessor type_name for int");
            check_eq(name_float, std::string("TplAutoDetectVec<float32>"), "accessor type_name for float");
        });

        test("auto_detect_bind_without_explicit_registration", [this]() {
            auto eng = make_engine();

            eng->bind_static_type<TplAutoDetectVec<int>>();

            eng->execute("auto v = TplAutoDetectVec<int32>();");
            eng->execute("v.a = 6;");
            eng->execute("v.b = 7;");

            check_eq(eng->execute("v.a").as<int>(), 6, "auto-detected property a");
            check_eq(eng->execute("v.b").as<int>(), 7, "auto-detected property b");
            check_eq(eng->execute("v.dot()").as<int>(), 42, "auto-detected method dot");
        });

        test("auto_detect_serialization", [this]() {
            TplAutoDetectVec<float> v1{3.0f, 4.0f};

            std::vector<uint8_t> buffer;
            {
                serialization::binary_archive_writer ar(buffer);
                ar.begin_object("TplAutoDetectVec<float>", 1);
                template_binder_accessor<TplAutoDetectVec<float>>::save(ar, v1);
                ar.end_object();
            }

            auto eng = make_engine();
            TplAutoDetectVec<float> v2;
            {
                serialization::binary_archive_reader ar(buffer, eng.get());
                std::string type_name;
                uint32_t version;
                ar.begin_object(type_name, version);
                template_binder_accessor<TplAutoDetectVec<float>>::load(ar, v2);
                ar.end_object();
            }

            check(std::abs(v2.a - 3.0f) < 0.001f, "a serialized via accessor");
            check(std::abs(v2.b - 4.0f) < 0.001f, "b serialized via accessor");
        });

        test("auto_detect_multiple_variants", [this]() {
            auto eng = make_engine();

            eng->bind_static_type<TplAutoDetectVec<int>>();
            eng->bind_static_type<TplAutoDetectVec<float>>();
            eng->bind_static_type<TplAutoDetectVec<double>>();

            eng->execute("auto vi = TplAutoDetectVec<int32>();");
            eng->execute("auto vf = TplAutoDetectVec<float32>();");
            eng->execute("auto vd = TplAutoDetectVec<double>();");

            eng->execute("vi.a = 2; vi.b = 3;");
            eng->execute("vf.a = 2.5; vf.b = 4.0;");
            eng->execute("vd.a = 1.5; vd.b = 3.0;");

            check_eq(eng->execute("vi.dot()").as<int>(), 6, "int variant works");
            check(std::abs(eng->execute("vf.dot()").as<float>() - 10.0f) < 0.001f, "float variant works");
            check(std::abs(eng->execute("vd.dot()").as<double>() - 4.5) < 0.001, "double variant works");
        });

        test("auto_bind_int32_also_binds_int64", [this]() {
            auto eng = make_engine();

            eng->bind_static_type<TplAutoDetectVec<int>>();

            eng->execute("auto v32 = TplAutoDetectVec<int32>();");
            eng->execute("v32.a = 5; v32.b = 6;");
            check_eq(eng->execute("v32.dot()").as<int>(), 30, "int32 variant works");

            eng->execute("auto v64 = TplAutoDetectVec<int>();");
            eng->execute("v64.a = 7; v64.b = 8;");
            check_eq(eng->execute("v64.dot()").as<int64_t>(), 56, "int (64-bit) variant auto-bound");
        });

        test("auto_bind_int64_also_binds_int32", [this]() {
            auto eng = make_engine();

            eng->bind_static_type<TplAutoDetectVec<int64_t>>();

            eng->execute("auto v64 = TplAutoDetectVec<int>();");
            eng->execute("v64.a = 10; v64.b = 11;");
            check_eq(eng->execute("v64.dot()").as<int64_t>(), 110, "int (64-bit) variant works");

            eng->execute("auto v32 = TplAutoDetectVec<int32>();");
            eng->execute("v32.a = 3; v32.b = 4;");
            check_eq(eng->execute("v32.dot()").as<int>(), 12, "int32 variant auto-bound");
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::template_binder_tests)
