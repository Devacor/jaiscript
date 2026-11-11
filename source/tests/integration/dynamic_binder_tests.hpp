#pragma once

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/core/registrar.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

// A self-referential host type whose implicit copy assignment would copy a vector of unique_ptrs (std::is_copy_assignable says
// yes and the assignment fails to compile), a type holding one by value, a copy-assignable type that binds "=", and one whose
// own operator= does more than copy
namespace jai::foundry::tests {
struct SelfTree {
    std::string id = "root";
    std::vector<std::unique_ptr<SelfTree>> branches;
    SelfTree() = default;
    SelfTree(const SelfTree& other) : id(other.id) {
        for (const auto& branch : other.branches) { branches.emplace_back(std::make_unique<SelfTree>(*branch)); }
    }
    SelfTree& self() { return *this; }
};
struct TreeHolder {
    std::string name = "holder";
    SelfTree tree;
    TreeHolder& self() { return *this; }
};
struct NamedThing {
    std::string name = "named";
    NamedThing& self() { return *this; }
};
struct CountedAssign {
    std::string name = "counted";
    int64_t assigned = 0;
    CountedAssign() = default;
    CountedAssign(const CountedAssign&) = default;
    CountedAssign& operator=(const CountedAssign& other) { name = other.name + "!"; ++assigned; return *this; }
    CountedAssign& operator=(const std::string& text) { name = text; ++assigned; return *this; }
    CountedAssign& self() { return *this; }
};
struct PlainCopy {
    std::string name = "plain";
    int64_t built = 0;
    PlainCopy() = default;
    PlainCopy(const PlainCopy& other) : name(other.name), built(other.built + 1) {}
    PlainCopy& operator=(const PlainCopy&) = default;
};
}

namespace jai::foundry::tests {

struct binder_plane;
struct binder_hangar {
    std::shared_ptr<binder_plane> plane;
    int64_t bays = 3;
    int64_t bay_count() const { return bays; }
};
struct binder_plane {
    std::shared_ptr<binder_hangar> home;
    int64_t seats = 2;
};
struct binder_listed_plane;
struct binder_listed_hangar {
    std::shared_ptr<binder_listed_plane> plane;
    int64_t bays = 3;
};
struct binder_listed_plane {
    std::shared_ptr<binder_listed_hangar> home;
    int64_t seats = 2;
};
// Registrars apply in script-name order: the hangar's, which names the plane, runs first
struct binder_registrar_context {};
inline registrar<binder_listed_hangar, binder_registrar_context> binder_listed_hangar_registrar("AaRegistrarHangar", [](dynamic_binder<binder_listed_hangar>& builder, const binder_registrar_context&) {
    builder.property("plane", &binder_listed_hangar::plane).property("bays", &binder_listed_hangar::bays);
});
inline registrar<binder_listed_plane, binder_registrar_context> binder_listed_plane_registrar("ZzRegistrarPlane", [](dynamic_binder<binder_listed_plane>& builder, const binder_registrar_context&) {
    builder.property("home", &binder_listed_plane::home).property("seats", &binder_listed_plane::seats);
});
struct binder_chain_root { int64_t depth = 0; };
struct binder_chain_middle : binder_chain_root {};
struct binder_chain_leaf : binder_chain_middle {};

// Test classes for binding
class DynamicBinderPoint {
public:
    double x, y;

    DynamicBinderPoint() : x(0), y(0) {}
    DynamicBinderPoint(double x, double y) : x(x), y(y) {}

    double length() const { return std::sqrt(x * x + y * y); }
    void move_by(double dx, double dy) { x += dx; y += dy; }

    // Arithmetic operators
    DynamicBinderPoint operator+(const DynamicBinderPoint& other) const { return DynamicBinderPoint(x + other.x, y + other.y); }
    DynamicBinderPoint operator-(const DynamicBinderPoint& other) const { return DynamicBinderPoint(x - other.x, y - other.y); }
    DynamicBinderPoint operator*(double scalar) const { return DynamicBinderPoint(x * scalar, y * scalar); }
    DynamicBinderPoint operator/(double scalar) const { return DynamicBinderPoint(x / scalar, y / scalar); }

    // Comparison operators
    bool operator==(const DynamicBinderPoint& other) const { return x == other.x && y == other.y; }
    bool operator<(const DynamicBinderPoint& other) const { return length() < other.length(); }
    bool operator<=(const DynamicBinderPoint& other) const { return length() <= other.length(); }
    bool operator>(const DynamicBinderPoint& other) const { return length() > other.length(); }
    bool operator>=(const DynamicBinderPoint& other) const { return length() >= other.length(); }

    static DynamicBinderPoint origin() { return DynamicBinderPoint(0, 0); }
    static int point_count;
};

int DynamicBinderPoint::point_count = 0;

// Test class using JAI_PROPERTY macros for auto_bind property tests
class PropertyOwnerTest : public property_owner<PropertyOwnerTest> {
public:
    JAI_PROPERTY((int), health, 100);
    JAI_PROPERTY((float), speed, 1.5f);
    JAI_PROPERTY((std::string), name, "Player");
    JAI_PROPERTY((bool), active, true);

    PropertyOwnerTest() = default;

    // Method to verify C++ side sees the property values
    int get_health_cpp() const { return health.get(); }
    void set_health_cpp(int val) { health = val; }
};

// Test class using JAI_OBSERVABLE_PROPERTY for signal observation tests
class ObservableOwnerTest : public property_owner<ObservableOwnerTest> {
public:
    JAI_OBSERVABLE_PROPERTY((int), score, 0);
    JAI_OBSERVABLE_PROPERTY((std::string), status, "idle");

    ObservableOwnerTest() = default;

    int get_score_cpp() const { return score.get(); }
    void set_score_cpp(int val) { score = val; }
};

// Two-level chain for the inherited value-property accessor tests: the property
// lives on the BASE level's manager, script access goes through the DERIVED binder.
class PropChainBase : public property_owner<PropChainBase> {
public:
    JAI_PROPERTY((int), armor, 7);
    PropChainBase() = default;
};

class PropChainDerived : public property_owner<PropChainDerived, PropChainBase> {
public:
    PropChainDerived() = default;
};

// In-place member lanes (detail/host_member.hpp): every member type a lane reads, a const member, a second base at a
// nonzero offset, and a polymorphic schema owner whose unregistered subclass shadows a property name
struct LaneMembers {
    int64_t big = 7;
    int32_t small = -3;
    uint8_t tiny = 200;
    uint64_t wide = 9;
    double speed = 1.5;
    float ratio = 0.25f;
    bool flag = true;
    std::string name = "lane";
    const int fixed = 5;
    int64_t other = 70;
};

struct RefInner { int64_t v = 4; };
struct RefFixed { int64_t v = 1; const int64_t k = 2; };
struct RefOwner {
    RefInner inner;
    RefFixed fixed;
    RefInner& child() { return inner; }
    RefFixed& fixed_child() { return fixed; }
    RefOwner& self_ref() { return *this; }
};
// Method lanes: a base's binding reached through a derived binding (single and at a nonzero offset), object parameters of
// every handing, and an overload set
struct LaneBase { int64_t b = 5; virtual ~LaneBase() = default; int64_t base_value() const { return b; } };
struct LaneDerived : LaneBase { int64_t own = 6; };
struct LaneRebound : LaneBase { int64_t rebound_value() const { return 600; } };
struct LanePad { int64_t pad[3] = {1, 2, 3}; };
struct LaneLate : LanePad, LaneBase { };
struct LaneUser {
    int64_t by_ref(const RefInner& a) const { return a.v; }
    int64_t by_mut(RefInner& a) const { a.v += 1; return a.v; }
    int64_t by_value(RefInner a) const { a.v += 100; return a.v; }
    int64_t by_handle(std::shared_ptr<RefInner> a) const { return a ? a->v : -1; }
    std::string ov(int64_t) const { return "int"; }
    std::string ov(double) const { return "float"; }
    std::string ov(const std::string&) const { return "string"; }
    std::string ov(bool) const { return "bool"; }
    std::string wide(int64_t) const { return "int64"; }
    std::string wide(int32_t) const { return "int32"; }
    void add_to(int64_t& target, int64_t by) const { target += by; }
    int64_t version() const { return 1; }
};
struct LaneFirstBase { int64_t first = 11; virtual ~LaneFirstBase() = default; };
struct LaneSecondBase { int64_t second = 22; double scale = 2.5; };
struct LaneJoined : LaneFirstBase, LaneSecondBase { int64_t own = 33; };

class LaneSchemaOwner : public property_owner<LaneSchemaOwner> {
public:
    JAI_PROPERTY((int), depth, 3);
    JAI_PROPERTY((double), alpha, 0.5);
    JAI_PROPERTY((std::string), id, std::string("owner"));
    JAI_PROPERTY((int64_t), count, 40);
    JAI_OBSERVABLE_PROPERTY((int), watched, 8);
    LaneSchemaOwner() = default;
};

class LaneSchemaShadow : public property_owner<LaneSchemaShadow, LaneSchemaOwner> {
public:
    JAI_PROPERTY((int), depth, 99);
    LaneSchemaShadow() = default;
};

// Base auto-registration fixtures: AutoChainBase is deliberately NOT registered by the
// tests that use AutoChainDerived — the derived registration must materialize it.
class AutoChainBase : public property_owner<AutoChainBase> {
public:
    JAI_PROPERTY((int), armor_base, 3);
    AutoChainBase() = default;
    static void jai_auto_bind(dynamic_binder<AutoChainBase>& b) {
        b.method("base_shout", [](AutoChainBase&) { return std::string("from_base"); });
    }
};

class AutoChainDerived : public property_owner<AutoChainDerived, AutoChainBase> {
public:
    AutoChainDerived() = default;
};

class PinnedNameBase : public property_owner<PinnedNameBase> {
public:
    JAI_PROPERTY_OWNER_AS(PinnedNameBase, "NicePinned");
    JAI_PROPERTY((int), pin_field, 9);
    PinnedNameBase() = default;
};

class PinnedNameDerived : public property_owner<PinnedNameDerived, PinnedNameBase> {
public:
    PinnedNameDerived() = default;
};

class CollideNameBase : public property_owner<CollideNameBase> {
public:
    JAI_PROPERTY((int), collide_field, 5);
    CollideNameBase() = default;
    static void jai_auto_bind(dynamic_binder<CollideNameBase>& b) {
        b.method("collide_shout", [](CollideNameBase&) { return std::string("x"); });
    }
};

class CollideNameDerived : public property_owner<CollideNameDerived, CollideNameBase> {
public:
    CollideNameDerived() = default;
};

class Counter {
public:
    int value;

    Counter() : value(0) { }
    explicit Counter(int v) : value(v) { }

    Counter& increment() { ++value; return *this; }
    Counter& add(int n) { value += n; return *this; }
    int get() const { return value; }
    void reset() { value = 0; }

    // Arithmetic operators
    Counter operator+(const Counter& other) const { return Counter(value + other.value); }
    Counter operator-(const Counter& other) const { return Counter(value - other.value); }
    Counter operator*(int scalar) const { return Counter(value * scalar); }
    Counter operator/(int scalar) const { return Counter(value / scalar); }
    Counter operator%(int mod) const { return Counter(value % mod); }

    // Comparison operators
    bool operator<(const Counter& other) const { return value < other.value; }
    bool operator<=(const Counter& other) const { return value <= other.value; }
    bool operator>(const Counter& other) const { return value > other.value; }
    bool operator>=(const Counter& other) const { return value >= other.value; }
};

// Polymorphic pair for the dynamic-type make_object test (a derived object held through
// shared_ptr<base>).
class base_shape {
public:
    virtual ~base_shape() = default;
    double base_val() const { return 1.0; }
};
class derived_shape : public base_shape {
public:
    double derived_val() const { return 2.0; }
};

class dynamic_binder_tests : public suite {
public:
    dynamic_binder_tests() : suite("Class Builder") {}
    
    void pre_test() override {
        // Reset static state before each test
        DynamicBinderPoint::point_count = 0;
    }
    
    // LaneSchemaOwner bound with two methods, and objects the host makes and keeps: held() a LaneSchemaOwner,
    // held_shadow() a LaneSchemaShadow handed over as its base
    static std::shared_ptr<engine> layer_engine(std::shared_ptr<std::vector<std::shared_ptr<LaneSchemaOwner>>> keep) {
        auto eng = make_engine();
        jai::stdlib::register_all(*eng);
        dynamic_binder<LaneSchemaOwner>(*eng, "LaneSchemaOwner")
            .constructor<>()
            .method("d0", [](LaneSchemaOwner& self) -> int64_t { return self.depth.get(); })
            .method("bump", [](LaneSchemaOwner& self, int64_t by) -> int64_t { self.depth = self.depth.get() + static_cast<int>(by); return self.depth.get(); })
            .auto_bind()
            .build();
        dynamic_binder<LaneSchemaShadow>(*eng, "LaneSchemaShadow")
            .constructor<>()
            .base_class<LaneSchemaOwner>()
            .auto_bind()
            .build();
        jai::engine* raw = eng.get();
        eng->add_function("held", [raw, keep]() -> script_value {
            keep->push_back(std::make_shared<LaneSchemaOwner>());
            return raw->make_object(keep->back());
        });
        eng->add_function("held_shadow", [raw, keep]() -> script_value {
            keep->push_back(std::make_shared<LaneSchemaShadow>());
            return raw->make_object(keep->back());
        });
        return eng;
    }

    static std::shared_ptr<engine> method_lane_engine() {
        auto eng = make_engine();
        jai::stdlib::register_all(*eng);
        dynamic_binder<RefInner>(*eng, "RefInner").constructor<>().property("v", &RefInner::v).auto_bind().build();
        dynamic_binder<RefFixed>(*eng, "RefFixed").constructor<>().property("v", &RefFixed::v).build();
        dynamic_binder<RefOwner>(*eng, "RefOwner")
            .constructor<>()
            .property("inner", &RefOwner::inner)
            .method("child", &RefOwner::child)
            .method("fixed_child", &RefOwner::fixed_child)
            .method("self_ref", &RefOwner::self_ref)
            .build();
        dynamic_binder<LaneBase>(*eng, "LaneBase").constructor<>().method("base_value", &LaneBase::base_value).build();
        dynamic_binder<LaneDerived>(*eng, "LaneDerived").constructor<>().base_class<LaneBase>().property("own", &LaneDerived::own).build();
        dynamic_binder<LaneRebound>(*eng, "LaneRebound").constructor<>().base_class<LaneBase>()
            .method("base_value", &LaneRebound::rebound_value).build();
        dynamic_binder<LaneLate>(*eng, "LaneLate").constructor<>().base_class<LaneBase>().build();
        dynamic_binder<LaneUser>(*eng, "LaneUser")
            .constructor<>()
            .method("by_ref", &LaneUser::by_ref)
            .method("by_mut", &LaneUser::by_mut)
            .method("by_value", &LaneUser::by_value)
            .method("by_handle", &LaneUser::by_handle)
            .method("ov", static_cast<std::string (LaneUser::*)(int64_t) const>(&LaneUser::ov))
            .method("ov", static_cast<std::string (LaneUser::*)(double) const>(&LaneUser::ov))
            .method("ov", static_cast<std::string (LaneUser::*)(const std::string&) const>(&LaneUser::ov))
            .method("ov", static_cast<std::string (LaneUser::*)(bool) const>(&LaneUser::ov))
            .method("wide", static_cast<std::string (LaneUser::*)(int64_t) const>(&LaneUser::wide))
            .method("wide", static_cast<std::string (LaneUser::*)(int32_t) const>(&LaneUser::wide))
            .method("add_to", &LaneUser::add_to)
            .method("version", &LaneUser::version, jai::filesystem)
            .build();
        return eng;
    }

    void forge_tests() override {
        // A bound member reads and writes in place once its site is warm (the first pass fills, the rest take the lane):
        // every pass must give what the accessors give, and a store the lane does not take keeps the setter's text
        test("host_member_lane_reads_and_writes_every_member_type", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            dynamic_binder<LaneMembers>(*eng, "LaneMembers")
                .constructor<>()
                .property("big", &LaneMembers::big)
                .property("small", &LaneMembers::small)
                .property("tiny", &LaneMembers::tiny)
                .property("wide", &LaneMembers::wide)
                .property("speed", &LaneMembers::speed)
                .property("ratio", &LaneMembers::ratio)
                .property("flag", &LaneMembers::flag)
                .property("name", &LaneMembers::name)
                .property("fixed", &LaneMembers::fixed)
                .build();
            auto out = eng->execute(R"(
                var m = LaneMembers();
                string s = "";
                for (int i = 0; i < 3; ++i) {
                    s += to_string(m.big) + "," + to_string(m.small) + "," + to_string(m.tiny) + "," + to_string(m.wide) + "," +
                         to_string(m.speed) + "," + to_string(m.ratio) + "," + to_string(m.flag) + "," + m.name + "," + to_string(m.fixed) + "|";
                    m.big = m.big + 1; m.small = m.small - 1; m.tiny = 201 + i; m.wide = 10 + i;
                    m.speed = m.speed * 2.0; m.ratio = 0.5; m.flag = !m.flag; m.name = m.name + "x";
                }
                s += type_of(m.big) + type_of(m.speed) + type_of(m.ratio) + type_of(m.flag) + type_of(m.name);
                s;
            )").as_string();
            check_eq(std::string("7,-3,200,9,1.5,0.25,true,lane,5|8,-4,201,10,3.0,0.5,false,lanex,5|9,-5,202,11,6.0,0.5,true,lanexx,5|intfloatfloatboolstring"), out);
            auto texts = eng->execute(R"(
                var m = LaneMembers();
                var t = [];
                for (int i = 0; i < 3; ++i) {
                    try { m.small = 5000000000; } catch (e) { t.push(e.message); }
                    try { m.tiny = 300; } catch (e) { t.push(e.message); }
                    try { m.tiny = -1; } catch (e) { t.push(e.message); }
                    try { m.big = "seven"; } catch (e) { t.push(e.message); }
                    try { m.fixed = 6; } catch (e) { t.push(e.message); }
                    try { m.flag = 1; } catch (e) { t.push(e.message); }
                    m.speed = 3;
                    m.wide = 12;
                }
                [t, m.small, m.tiny, m.speed, m.fixed, m.flag, m.wide];
            )");
            const auto& all = texts.as_array();
            const auto& t = all[0].as_array();
            check_true(t.size() % 3 == 0 && !t.empty(), "every pass raises the same set");
            const size_t per = t.size() / 3;
            bool same = true;
            for (size_t i = 0; i < per; ++i) { same = same && t[i].as_string() == t[per + i].as_string() && t[i].as_string() == t[2 * per + i].as_string(); }
            check_true(same, "a store the lane leaves to the setter raises the setter's text on every pass");
            check_eq((int64_t)-3, all[1].as_int(), "an out-of-range store leaves the member");
            check_eq((int64_t)200, all[2].as_int(), "a byte store out of range leaves the member");
            check_near(3.0, all[3].as_float(), 1e-12);
            check_eq((int64_t)5, all[4].as_int(), "a const member stays read-only");
            check_eq((int64_t)12, all[6].as_int(), "an unsigned member takes an int in range");
        });

        // A script_value member (the library's pair) takes the lane too: every warm pass reads what the getter reads (a
        // container half is the member's own), a scalar store lands in place, and a store the lane leaves to the setter (a
        // container, or through a by-reference entry's half with the map's value type) keeps the setter's result and text
        test("host_member_lane_reads_and_writes_script_value_members", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            auto out = eng->execute(R"(
                var p = pair(1, [2]);
                map<string, int> m = {"a": 1};
                string s = "";
                for (int i = 0; i < 3; ++i) {
                    s += to_string(p.first) + to_json(p.second) + ",";
                    p.first = p.first + (i == 2 ? "1" : 1);   // a string member joins text, an int one adds
                    p.second.push(i);
                    var copy = p.second;
                    copy.push(99);
                    if (i == 1) { p.second = [7]; p.first = "t"; }
                }
                s += to_string(p.first) + to_json(p.second) + "|";
                for (int i = 0; i < 3; ++i) {
                    for (auto& kv : m) {
                        kv.second = kv.second + 1;
                        s += to_string(kv.second);
                        var bad = "x";
                        try { kv.second = bad; } catch (e) { s += "!"; }
                    }
                }
                s + to_json(m);
            )").as_string();
            check_eq(std::string("1[2],2[2,0],t[7],t1[7,2]|2!3!4!{\"a\":4}"), out);
        });

        test("host_member_lane_reaches_a_second_base_and_a_derived_registration", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            dynamic_binder<LaneSecondBase>(*eng, "LaneSecondBase")
                .constructor<>()
                .property("second", &LaneSecondBase::second)
                .property("scale", &LaneSecondBase::scale)
                .build();
            dynamic_binder<LaneJoined>(*eng, "LaneJoined")
                .constructor<>()
                .base_class<LaneSecondBase>()
                .property("own", &LaneJoined::own)
                .build();
            auto out = eng->execute(R"(
                var j = LaneJoined();
                var b = LaneSecondBase();
                string s = "";
                for (int i = 0; i < 3; ++i) {
                    s += to_string(j.second) + "," + to_string(j.scale) + "," + to_string(j.own) + "," + to_string(b.second) + "|";
                    j.second = j.second + 1; j.scale = j.scale + 1.0; j.own = j.own + 1; b.second = b.second - 1;
                }
                s;
            )").as_string();
            check_eq(std::string("22,2.5,33,22|23,3.5,34,21|24,4.5,35,20|"), out);
        });

        test("host_member_lane_follows_schema_properties_and_dynamic_types", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            dynamic_binder<LaneSchemaOwner>(*eng, "LaneSchemaOwner")
                .constructor<>()
                .auto_bind()
                .build();
            jai::engine* raw_eng = eng.get();
            eng->add_function("shadowed", [raw_eng]() -> script_value {
                std::shared_ptr<LaneSchemaOwner> held = std::make_shared<LaneSchemaShadow>();
                return raw_eng->make_object(held);
            });
            auto out = eng->execute(R"(
                shared_ptr<LaneSchemaOwner> o = new LaneSchemaOwner();
                string s = "";
                function read_depth(var& x) -> int { return x.depth; }
                for (int i = 0; i < 3; ++i) {
                    s += to_string(o.depth) + "," + to_string(o.alpha) + "," + o.id + "," + to_string(o.count) + "|";
                    o.depth = o.depth + 1; o.alpha = o.alpha * 2.0; o.id = o.id + "!"; o.count = o.count - 1;
                }
                shared_ptr<LaneSchemaOwner> sh = shadowed();
                s += to_string(read_depth(o)) + "," + to_string(read_depth(o)) + "," + to_string(read_depth(sh)) + "," + to_string(read_depth(sh)) + "," + to_string(read_depth(o)) + ";";
                s;
            )").as_string();
            const std::string shadow = eng->execute("to_string(shadowed().depth)").as_string();
            check_eq(std::string("3,0.5,owner,40|4,1.0,owner!,39|5,2.0,owner!!,38|6,6,") + shadow + "," + shadow + ",6;", out,
                     "a site warm on one dynamic type reads another's property through its own lookup");
            auto watched = eng->execute(R"(
                shared_ptr<LaneSchemaOwner> o = new LaneSchemaOwner();
                var r = [];
                for (int i = 0; i < 2; ++i) { r.push(type_of(o.watched)); o.watched = 9 + i; }
                r;
            )");
            check_true(watched.as_array()[0].as_string() == watched.as_array()[1].as_string(), "an observable property keeps its getter's value kind");
        });

        test("host_member_lane_misses_on_a_null_and_a_dead_handle", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            dynamic_binder<LaneMembers>(*eng, "LaneMembers")
                .constructor<>()
                .property("big", &LaneMembers::big)
                .build();
            auto out = eng->execute(R"(
                function read_big(var& x) -> var { return x.big; }
                function write_big(var& x) { x.big = 3; }
                var live = LaneMembers();
                read_big(live); read_big(live); write_big(live); write_big(live);
                var warm = [];
                var none = null;
                try { read_big(none); } catch (e) { warm.push(e.message); }
                try { write_big(none); } catch (e) { warm.push(e.message); }
                function read_big_cold(var& x) -> var { return x.big; }
                function write_big_cold(var& x) { x.big = 3; }
                var cold = [];
                try { read_big_cold(none); } catch (e) { cold.push(e.message); }
                try { write_big_cold(none); } catch (e) { cold.push(e.message); }
                [warm, cold, live.big];
            )");
            const auto& all = out.as_array();
            const auto& warm = all[0].as_array();
            const auto& cold = all[1].as_array();
            check_eq((size_t)2, warm.size(), "a null receiver raises at a warm site");
            check_eq((size_t)2, cold.size(), "and at a cold one");
            check_eq(cold[0].as_string(), warm[0].as_string(), "the read's text");
            check_eq(cold[1].as_string(), warm[1].as_string(), "the store's text");
            check_eq((int64_t)3, all[2].as_int(), "the live receiver was written");
        });

        test("host_member_lane_misses_on_an_expired_handle", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            dynamic_binder<LaneMembers>(*eng, "LaneMembers")
                .constructor<>()
                .property("big", &LaneMembers::big)
                .build();
            auto out = eng->execute(R"(
                function read_big(var& x) -> var { return x.big; }
                function write_big(var& x) { x.big = 3; }
                function read_big_cold(var& x) -> var { return x.big; }
                function write_big_cold(var& x) { x.big = 3; }
                shared_ptr<LaneMembers> held = new LaneMembers();
                weak_ptr<LaneMembers> w = held;
                read_big(held); read_big(held); write_big(held); write_big(held);
                var locked = w.lock();
                int seen = read_big(locked);
                locked = null;
                held = null;
                var gone = w.lock();
                var warm = [];
                var cold = [];
                try { read_big(gone); } catch (e) { warm.push(e.message); }
                try { write_big(gone); } catch (e) { warm.push(e.message); }
                try { read_big(w); } catch (e) { warm.push(e.message); }
                try { read_big_cold(gone); } catch (e) { cold.push(e.message); }
                try { write_big_cold(gone); } catch (e) { cold.push(e.message); }
                try { read_big_cold(w); } catch (e) { cold.push(e.message); }
                [warm, cold, seen];
            )");
            const auto& all = out.as_array();
            const auto& warm = all[0].as_array();
            const auto& cold = all[1].as_array();
            check_eq(cold.size(), warm.size(), "an expired handle raises alike at warm and cold sites");
            for (size_t i = 0; i < warm.size() && i < cold.size(); ++i) { check_eq(cold[i].as_string(), warm[i].as_string(), "the same text"); }
            check_eq((int64_t)3, all[2].as_int(), "the live handle read the stored value");
        });

        // A handle that reached the object before another handle extended it re-points to the script layer on its next
        // touch, and the layer's accessor wins: a site warm on the plain class must not keep reading the member in place
        test("host_member_lane_yields_to_a_layer_added_through_another_handle", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            dynamic_binder<LaneSchemaOwner>(*eng, "LaneSchemaOwner")
                .constructor<>()
                .auto_bind()
                .build();
            auto shared = std::make_shared<LaneSchemaOwner>();
            jai::engine* raw_eng = eng.get();
            eng->add_function("same", [raw_eng, shared]() -> script_value { return raw_eng->make_object(shared); });
            auto out = eng->execute(R"(
                class Over : LaneSchemaOwner { int _get_depth() override { return 1000; } }
                shared_ptr<LaneSchemaOwner> a = same();
                shared_ptr<LaneSchemaOwner> b = same();
                function read_depth(var& x) -> int { return x.depth; }
                int r1 = read_depth(a);
                int r2 = read_depth(a);
                extend(b, Over);
                int r3 = read_depth(a);
                int r4 = read_depth(a);
                to_string(r1) + "," + to_string(r2) + "," + to_string(r3) + "," + to_string(r4);
            )").as_string();
            check_eq(std::string("3,3,1000,1000"), out);
        });

        // An extended object and a constructed hybrid read, write and call their C++ base's members at warm sites, from outside
        // and as bare names in their own methods, and the host sees every write
        test("layer_lanes_serve_extended_objects_and_hybrids", [this]() {
            auto keep = std::make_shared<std::vector<std::shared_ptr<LaneSchemaOwner>>>();
            auto eng = layer_engine(keep);
            auto out = eng->execute(R"(
                class Ext : LaneSchemaOwner {
                    int bonus = 2;
                    int bare_read() { return depth + bonus; }
                    int bare_call() { return d0() + bump(1); }
                }
                var e = held();
                extend(e, Ext);
                var h = Ext();
                function rd(var& x) -> int { return x.depth; }
                function wr(var& x, int v) { x.depth = v; }
                function call(var& x) -> int { return x.d0(); }
                function call1(var& x, int by) -> int { return x.bump(by); }
                string s = "";
                for (int i = 0; i < 3; ++i) {
                    s += to_string(rd(e)) + "," + to_string(rd(h)) + "," + to_string(call(e)) + "," + to_string(call(h)) + "," +
                         to_string(e.bare_read()) + "," + to_string(h.bare_call()) + "," + to_string(call1(e, 2)) + "|";
                    wr(e, 10 + i);
                    wr(h, 20 + i);
                }
                s;
            )").as_string();
            check_eq(std::string("3,3,3,3,5,7,5|10,20,10,20,12,41,12|11,21,11,21,13,43,13|"), out);
            check_eq(12, keep->at(0)->depth.get(), "the host object holds the last write");
        });

        // A layer whose script chain answers the name itself (a field, a _get_ accessor, a method override, here or in a script
        // parent) never takes the C++ base's lane, at sites warm on a layer that does not
        test("layer_lanes_yield_to_the_script_chain", [this]() {
            auto keep = std::make_shared<std::vector<std::shared_ptr<LaneSchemaOwner>>>();
            auto eng = layer_engine(keep);
            auto out = eng->execute(R"(
                class Plain : LaneSchemaOwner {
                    int bare_read() { return depth; }
                    int bare_call() { return d0(); }
                }
                class Fielded : Plain { int depth = 500; }
                class Accessed : Plain { int _get_depth() override { return 600; } }
                class Overridden : Plain { int d0() override { return 700; } }
                class Mid : Plain { int _get_depth() override { return 800; } int d0() override { return 801; } }
                class Below : Mid {}
                function rd(var& x) -> int { return x.depth; }
                function call(var& x) -> int { return x.d0(); }
                var a = held(); extend(a, Plain);
                var b = held(); extend(b, Fielded);
                var c = held(); extend(c, Accessed);
                var d = held(); extend(d, Overridden);
                var e = held(); extend(e, Below);
                function row(var& o) -> string {
                    return to_string(rd(o)) + "," + to_string(call(o)) + "," + to_string(o.bare_read()) + "," + to_string(o.bare_call()) + ";";
                }
                string s = "";
                for (int round = 0; round < 2; ++round) {
                    s += row(a) + row(b) + row(c) + row(d) + row(e) + "|";
                }
                s;
            )").as_string();
            const std::string round = "3,3,3,3;500,3,500,3;600,3,600,3;3,700,3,700;800,801,800,801;|";
            check_eq(round + round, out);
        });

        // A bare name in a method, warm on a layer whose script field shadows the C++ property, reads the property itself on a
        // layer without that field (a placeholder slot is no cell)
        test("a_bare_member_site_warm_on_a_shadowing_field_reads_the_property_elsewhere", [this]() {
            auto keep = std::make_shared<std::vector<std::shared_ptr<LaneSchemaOwner>>>();
            auto eng = layer_engine(keep);
            eng->execute(R"(
                class Plain : LaneSchemaOwner {
                    int bare_read() { return depth; }
                    int bare_call() { return d0(); }
                }
                class Fielded : Plain { int depth = 500; }
                class Accessed : Plain { int _get_depth() override { return 600; } }
                function rd(var& x) -> int { return x.depth; }
                function call(var& x) -> int { return x.d0(); }
                var a = held(); extend(a, Plain);
                var b = held(); extend(b, Fielded);
                function row(var& o) -> string { return to_string(o.bare_read()); }
            )");
            std::string r;
            for (const char* q : {"a.bare_read();", "b.bare_read();", "row(a);", "row(b);"}) {
                try { auto v = eng->execute(q); r += v.is_null() ? std::string("null") : v.is_string() ? v.as_string() : std::to_string(v.as_int()); } catch (const std::exception& e) { r += std::string("E:") + e.what(); }
                r += ";";
            }
            check_eq(std::string("3;500;3;500;"), r);
        });

        // Extending objects of two C++ types under the declared base into one script class gives one shared class: one identity,
        // one static field, one reload; each object's native keeps its own dynamic type; and a warm site alternating between
        // them reaches each object's member through its own layout, never a delta measured on the other
        test("one_script_class_serves_objects_of_different_cpp_types", [this]() {
            auto keep = std::make_shared<std::vector<std::shared_ptr<LaneSchemaOwner>>>();
            auto eng = layer_engine(keep);
            eng->add_function("shadow_depth", [](LaneSchemaShadow& shadow) -> int64_t { return shadow.depth.get(); });
            eng->execute(R"(
                class Ext : LaneSchemaOwner {
                    static int made = 0;
                    int tag() { return 1; }
                    int bare() { return depth; }
                }
                var p = held();
                extend(p, Ext);
                var q = held_shadow();
                extend(q, Ext);
                Ext::made = Ext::made + 1;
                function rd(var& x) -> int { return x.depth; }
                function wr(var& x, int v) { x.depth = v; }
                function call(var& x) -> int { return x.d0(); }
                function row(var& x) -> string { return to_string(rd(x)) + "," + to_string(call(x)) + "," + to_string(x.bare()) + "," + to_string(x.tag()); }
            )");
            check_eq(std::string("Ext"), eng->execute("reflect::type_name(p);").as_string(), "the plain-typed object's class");
            check_eq(std::string("Ext"), eng->execute("reflect::type_name(q);").as_string(), "the derived-typed object's class");
            const std::string p_json = eng->execute("to_json(p);").as_string();
            const std::string q_json = eng->execute("to_json(q);").as_string();
            check_true(p_json.find("\"Ext\"") != std::string::npos && q_json.find("\"Ext\"") != std::string::npos, "both save as Ext: " + p_json + " / " + q_json);
            check_eq((script_int)1, eng->execute("Ext::made;").as_int(), "one static field");
            check_eq((script_int)99, eng->execute("shadow_depth(q);").as_int(), "the derived-typed native keeps its dynamic type");
            check_throws([&]() { eng->execute("shadow_depth(p);"); }, "the plain-typed native is no LaneSchemaShadow");
            std::string rows;
            for (int i = 0; i < 3; ++i) {
                rows += eng->execute("row(p) + \"|\" + row(q) + \";\";").as_string();
                eng->execute("wr(p, rd(p) + 1); wr(q, rd(q) + 1);");
            }
            check_eq(std::string("3,3,3,1|99,3,99,1;4,4,4,1|100,3,100,1;5,5,5,1|101,3,101,1;"), rows, "each object through its own layout");
            eng->execute("class Ext : LaneSchemaOwner { static int made = 0; int tag() { return 2; } int bare() { return depth; } }");
            check_eq(std::string("6,6,6,2|102,3,102,2;"), eng->execute("row(p) + \"|\" + row(q) + \";\";").as_string(), "one reload updates both");
        });

        // Every extend into a class gives its fields their initializers' values: constants (literals, array and map literals of
        // them) as fresh copies per object, other initializers evaluated per object, and a reload's new initializers from then on
        test("extend_initializes_each_layer_like_a_construction", [this]() {
            auto keep = std::make_shared<std::vector<std::shared_ptr<LaneSchemaOwner>>>();
            auto eng = layer_engine(keep);
            auto out = eng->execute(R"(
                int calls = 0;
                function next_id() -> int { calls = calls + 1; return calls; }
                class Plain : LaneSchemaOwner { int hp = 7; float speed = 1.5; string tag = "x"; var stacks = []; var table = {"a": 1}; bool on = true; }
                class Counted : Plain { int id = next_id(); }
                string s = "";
                for (int i = 0; i < 3; ++i) {
                    var p = held();
                    extend(p, Plain);
                    s += to_string(p.hp) + "," + to_string(p.speed) + "," + p.tag + "," + to_string(p.stacks.size()) + "," +
                         to_string(p.table["a"]) + "," + to_string(p.on) + ";";
                    p.hp = 0; p.tag = "changed"; p.stacks.push(i); p.table["a"] = 9;
                    var c = held();
                    extend(c, Counted);
                    s += to_string(c.id) + "," + to_string(c.stacks.size()) + "|";
                    c.stacks.push(1);
                }
                s;
            )").as_string();
            check_eq(std::string("7,1.5,x,0,1,true;1,0|7,1.5,x,0,1,true;2,0|7,1.5,x,0,1,true;3,0|"), out,
                     "constants arrive fresh per object, a computed initializer runs per object");
            eng->execute("class Plain : LaneSchemaOwner { int hp = 70; float speed = 1.5; string tag = \"y\"; var stacks = [1, 2]; var table = {\"a\": 1}; bool on = true; }");
            check_eq(std::string("70,y,2"), eng->execute("var r = held(); extend(r, Plain); to_string(r.hp) + \",\" + r.tag + \",\" + to_string(r.stacks.size());").as_string(),
                     "a reload's initializers apply to the next extend");
        });

        // A reload that gives the layer's class an override, or takes it away, turns a warm site with it
        test("layer_lanes_follow_a_reload_of_the_layer_class", [this]() {
            auto keep = std::make_shared<std::vector<std::shared_ptr<LaneSchemaOwner>>>();
            auto eng = layer_engine(keep);
            eng->execute(R"(
                class Ext : LaneSchemaOwner { int bare() { return depth + d0(); } }
                var e = held();
                extend(e, Ext);
                function rd(var& x) -> int { return x.depth; }
                function call(var& x) -> int { return x.d0(); }
                function all() -> string { return to_string(rd(e)) + "," + to_string(call(e)) + "," + to_string(e.bare()); }
            )");
            check_eq(std::string("3,3,6"), eng->execute("all();").as_string());
            check_eq(std::string("3,3,6"), eng->execute("all();").as_string());
            eng->execute("class Ext : LaneSchemaOwner { int _get_depth() override { return 40; } int d0() override { return 50; } int bare() { return depth + d0(); } }");
            check_eq(std::string("40,50,90"), eng->execute("all();").as_string());
            eng->execute("class Ext : LaneSchemaOwner { int bare() { return depth + d0(); } }");
            check_eq(std::string("3,3,6"), eng->execute("all();").as_string());
        });

        // A warm layer site whose layer was retired (its object died, it was detached, or it moved to another engine) reports the
        // catalogued text, never a stale address
        test("layer_lanes_miss_on_a_retired_layer", [this]() {
            const std::string text = "Script layer 'Ext' was detached (script_layer_detach, or a move to another engine): this handle no longer reaches its object";
            for (int how = 0; how < 3; ++how) {
                auto keep = std::make_shared<std::vector<std::shared_ptr<LaneSchemaOwner>>>();
                auto eng = layer_engine(keep);
                auto other = layer_engine(keep);
                const char* source = R"(
                    class Ext : LaneSchemaOwner { int bare() { return depth; } int bare_call() { return d0(); } }
                    function rd(var& x) -> int { return x.depth; }
                    function call(var& x) -> int { return x.d0(); }
                )";
                eng->execute(source);
                other->execute(source);
                eng->execute("var e = held(); extend(e, Ext); int warm = 0; for (int i = 0; i < 3; ++i) { warm += rd(e) + call(e) + e.bare() + e.bare_call(); }");
                std::shared_ptr<LaneSchemaOwner> object = keep->back();
                if (how == 0) { keep->clear(); object.reset(); }
                if (how == 1) { check_false(eng->script_layer_detach(object).empty(), "detach returns the fields"); }
                if (how == 2) { (void)other->extend(object, "Ext"); }
                for (const char* probe : {"rd(e);", "call(e);", "e.bare();", "e.bare_call();"}) {
                    std::string message;
                    try { eng->execute(probe); } catch (const std::exception& error) { message = error.what(); }
                    check_eq(text, message, std::string("retire ") + std::to_string(how) + " through " + probe);
                }
            }
        });

        // A layer re-derives only into a class whose C++ base the object is: a plain base's object never becomes a derived base's
        test("a_layer_does_not_re_derive_past_its_objects_cpp_class", [this]() {
            auto keep = std::make_shared<std::vector<std::shared_ptr<LaneSchemaOwner>>>();
            auto eng = layer_engine(keep);
            eng->execute("class Ext : LaneSchemaOwner { int tag() { return 1; } } class ShadowExt : LaneSchemaShadow { int tag() { return 2; } } var e = held(); extend(e, Ext);");
            std::string message;
            try { eng->execute("extend(e, ShadowExt);"); } catch (const std::exception& error) { message = error.what(); }
            check_eq(std::string("extend: 'ShadowExt' derives the C++ class 'LaneSchemaShadow', which is not the object's C++ class 'LaneSchemaOwner' or one of its bases"), message);
            check_eq((script_int)1, eng->execute("e.tag();").as_int(), "the layer keeps its class");
            check_eq((script_int)3, eng->execute("e.d0();").as_int(), "and its object");
        });

        // A plain wrapper of an object that gained a layer through another handle re-points to the layer for methods too
        test("host_method_lane_yields_to_a_layer_added_through_another_handle", [this]() {
            auto keep = std::make_shared<std::vector<std::shared_ptr<LaneSchemaOwner>>>();
            auto eng = layer_engine(keep);
            auto shared = std::make_shared<LaneSchemaOwner>();
            jai::engine* raw = eng.get();
            eng->add_function("same", [raw, shared]() -> script_value { return raw->make_object(shared); });
            auto out = eng->execute(R"(
                class Over : LaneSchemaOwner { int d0() override { return 1000; } }
                shared_ptr<LaneSchemaOwner> a = same();
                shared_ptr<LaneSchemaOwner> b = same();
                function call(var& x) -> int { return x.d0(); }
                int r1 = call(a);
                int r2 = call(a);
                extend(b, Over);
                to_string(r1) + "," + to_string(r2) + "," + to_string(call(a)) + "," + to_string(call(a));
            )").as_string();
            check_eq(std::string("3,3,1000,1000"), out);
        });

        // A base's binding answers a derived receiver at warm sites: a derived binding that inherits it, one at a nonzero
        // offset, and one that rebinds the name (its own answer wins)
        test("method_lanes_reach_inherited_bindings", [this]() {
            auto eng = method_lane_engine();
            auto out = eng->execute(R"(
                var d = LaneDerived();
                var late = LaneLate();
                var rebound = LaneRebound();
                var base = LaneBase();
                function call(var& x) -> int { return x.base_value(); }
                string s = "";
                for (int i = 0; i < 3; ++i) { s += to_string(call(d)) + "," + to_string(call(late)) + "," + to_string(call(rebound)) + "," + to_string(call(base)) + ";"; }
                s;
            )").as_string();
            check_eq(std::string("5,5,600,5;5,5,600,5;5,5,600,5;"), out);
        });

        // Object parameters in every handing take the same object the ordinary call would: by reference (writes land), by
        // value (a copy), a shared_ptr (null an empty one); an argument the call would refuse reports the ordinary call's text
        test("method_lanes_take_object_parameters", [this]() {
            auto eng = method_lane_engine();
            auto out = eng->execute(R"(
                var u = LaneUser();
                var a = RefInner();
                shared_ptr<RefInner> h = new RefInner();
                string s = "";
                for (int i = 0; i < 3; ++i) {
                    s += to_string(u.by_ref(a)) + "," + to_string(u.by_mut(a)) + "," + to_string(u.by_value(a)) + "," + to_string(a.v) + "," +
                         to_string(u.by_handle(h)) + "," + to_string(u.by_handle(null)) + ";";
                }
                s;
            )").as_string();
            check_eq(std::string("4,5,105,5,4,-1;5,6,106,6,4,-1;6,7,107,7,4,-1;"), out);
            std::string message;
            try { eng->execute("var wrong = LaneBase(); u.by_ref(wrong);"); } catch (const std::exception& error) { message = error.what(); }
            check_true(!message.empty(), "a refused argument raises");
            std::string again;
            try { eng->execute("var wrong2 = LaneBase(); for (int i = 0; i < 2; ++i) { u.by_ref(a); } u.by_ref(wrong2);"); } catch (const std::exception& error) { again = error.what(); }
            check_eq(message, again, "the same text at a warm site");
        });

        // An overloaded method picks by argument kinds as the ordinary call scores them; an int matching two integral
        // overloads takes the ordinary call's choice
        test("method_lanes_pick_overloads_like_the_ordinary_call", [this]() {
            auto eng = method_lane_engine();
            auto out = eng->execute(R"(
                var u = LaneUser();
                string s = "";
                for (int i = 0; i < 3; ++i) { s += u.ov(1) + "," + u.ov(1.5) + "," + u.ov("x") + "," + u.ov(true) + "," + u.wide(7) + ";"; }
                s;
            )").as_string();
            const std::string first = out.substr(0, out.find(';') + 1);
            check_eq(first + first + first, out, "every pass answers alike");
            check_eq(std::string("int,float,string,bool,"), first.substr(0, std::string("int,float,string,bool,").size()), "kinds pick their overloads");
        });

        // A host method no lane takes (a reference-writing parameter, a guarded method) keeps its ordinary call's answers at a
        // warm site
        test("ordinary_host_calls_keep_their_answers_at_warm_sites", [this]() {
            auto eng = method_lane_engine();
            auto out = eng->execute(R"(
                var u = LaneUser();
                int a = 0;
                int b = 10;
                for (int i = 0; i < 3; ++i) { u.add_to(a, 1); u.add_to(b, 2); }
                function v(var& x) -> int { return x.version(); }
                to_string(a) + "," + to_string(b) + "," + to_string(v(u)) + "," + to_string(v(u));
            )").as_string();
            check_eq(std::string("3,16,1,1"), out);
        });

        // A self-referential host type with no usable copy assignment, and a type holding one, bind (the binder never instantiates
        // their assignments); a reference to either refuses an assignment at run time, and a class that declares one assigns
        test("a_reference_to_a_type_without_copy_assignment_refuses_one", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            dynamic_binder<SelfTree>(*eng, "SelfTree").constructor<>().property("id", &SelfTree::id).method("self", &SelfTree::self).build();
            dynamic_binder<TreeHolder>(*eng, "TreeHolder").constructor<>().property("name", &TreeHolder::name).method("self", &TreeHolder::self).build();
            dynamic_binder<NamedThing>(*eng, "NamedThing").constructor<>().property("name", &NamedThing::name).method("self", &NamedThing::self)
                .method("=", static_cast<NamedThing& (NamedThing::*)(const NamedThing&)>(&NamedThing::operator=)).build();
            for (const char* cls : {"SelfTree", "TreeHolder"}) {
                std::string message;
                try { eng->execute(std::string("var t = ") + cls + "(); var& r = t.self(); r = " + cls + "();"); } catch (const std::exception& error) { message = error.what(); }
                check_eq(std::string("Cannot assign to a reference to '") + cls + "': its binding has no operator=", message);
            }
            check_eq(std::string("renamed"), eng->execute("var s = SelfTree(); var& sr = s.self(); sr.id = \"renamed\"; s.id;").as_string(),
                     "member writes through the reference still land");
            check_eq(std::string("other"), eng->execute("var n = NamedThing(); var& nr = n.self(); var m = NamedThing(); m.name = \"other\"; nr = m; n.name;").as_string(),
                     "a bound \"=\" assigns");
        });

        // A bound "=" is the class's own operator=: a user-defined one runs (here it marks the copy and counts), and an "=" overload
        // from another type resolves like any overload set
        // A slot typed as a bound class holds its own object: an assignment into it (a variable, a reference to one, a field, an
        // element) runs the binding's "=" on that object when one is bound, and otherwise replaces it with a copy-constructed one;
        // a var slot takes the new value as it is
        test("a_typed_slot_of_a_bound_class_assigns_through_its_operator", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            dynamic_binder<CountedAssign>(*eng, "CountedAssign").constructor<>()
                .property("name", &CountedAssign::name).property("assigned", &CountedAssign::assigned)
                .method("=", static_cast<CountedAssign& (CountedAssign::*)(const CountedAssign&)>(&CountedAssign::operator=))
                .build();
            dynamic_binder<PlainCopy>(*eng, "PlainCopy").constructor<>().property("name", &PlainCopy::name).property("built", &PlainCopy::built).build();
            auto out = eng->execute(R"(
                class Holder { CountedAssign g = CountedAssign(); }
                CountedAssign a = CountedAssign();
                CountedAssign b = CountedAssign();
                b.name = "bee";
                a = b;
                string s = a.name + "," + to_string(a.assigned) + ";";
                var& r = a;
                r = b;
                s += a.name + "," + to_string(a.assigned) + ";";
                Holder h = Holder();
                h.g = b;
                s += h.g.name + "," + to_string(h.g.assigned) + ";";
                array<CountedAssign> arr = [CountedAssign()];
                arr[0] = b;
                s += arr[0].name + "," + to_string(arr[0].assigned) + ";";
                map<string, CountedAssign> m;
                m["k"] = CountedAssign();
                m["k"] = b;
                s += m["k"].name + "," + to_string(m["k"].assigned) + ";";
                var v = CountedAssign();
                v = b;
                b.name = "moved";
                s += v.name + "," + to_string(v.assigned) + ";";
                PlainCopy p = PlainCopy();
                PlainCopy q = PlainCopy();
                q.name = "q";
                p = q;
                q.name = "changed";
                s += p.name + "," + to_string(p.built);
                s;
            )").as_string();
            check_eq(std::string("bee!,1;bee!,2;bee!,1;bee!,1;bee!,1;bee!,1;q,1"), out, "a var holding the object runs its bound operator= too");
        });

        test("a_bound_operator_assign_runs_the_classes_own", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            dynamic_binder<CountedAssign>(*eng, "CountedAssign").constructor<>()
                .property("name", &CountedAssign::name).property("assigned", &CountedAssign::assigned).method("self", &CountedAssign::self)
                .method("=", static_cast<CountedAssign& (CountedAssign::*)(const CountedAssign&)>(&CountedAssign::operator=))
                .method("=", static_cast<CountedAssign& (CountedAssign::*)(const std::string&)>(&CountedAssign::operator=))
                .build();
            auto out = eng->execute(R"(
                var a = CountedAssign();
                var& r = a.self();
                var b = CountedAssign();
                b.name = "copied";
                r = b;
                string s = a.name + "," + to_string(a.assigned) + ";";
                r = "text";
                s + a.name + "," + to_string(a.assigned);
            )").as_string();
            check_eq(std::string("copied!,1;text,2"), out);
        });

        // A host method's T& result binds a var& or auto& as C++ binds a reference: member writes and assignments land in
        // the object, the receiver's object stays alive through the reference, and a class with no copy assignment refuses one
        test("a_reference_to_a_host_reference_result_binds_like_cpp", [this]() {
            auto eng = method_lane_engine();
            auto out = eng->execute(R"(
                var o = RefOwner();
                var& c = o.child();
                c.v = 9;
                string s = to_string(o.inner.v) + ",";
                var n = RefInner();
                n.v = 8;
                c = n;
                n.v = 1;
                s += to_string(o.inner.v) + "," + to_string(c.v) + ",";
                auto& d = o.child();
                d.v = 3;
                RefInner& t = o.child();
                t.v = t.v + 10;
                s += to_string(o.inner.v) + ",";
                var& me = o.self_ref();
                me.inner.v = 21;
                s += to_string(o.inner.v);
                s;
            )").as_string();
            check_eq(std::string("9,8,8,13,21"), out);
            check_eq(std::string("Cannot assign to a reference to 'RefFixed': its binding has no operator="), [&]() {
                try { eng->execute("var f = RefOwner(); var& fc = f.fixed_child(); fc = RefFixed();"); } catch (const std::exception& error) { return std::string(error.what()); }
                return std::string("no error");
            }());
            check_eq(std::string("5"), eng->execute("var holder = RefOwner(); var& kept = holder.child(); holder = null; kept.v = 5; to_string(kept.v);").as_string(),
                     "the reference keeps its owner's object alive");
            auto host_owner = std::make_shared<RefOwner>();
            eng->add_global("host_owner", eng->make_object(host_owner));
            eng->execute("var& hosted = host_owner.child(); host_owner = null;");
            host_owner.reset();
            check_eq(std::string("12"), eng->execute("hosted.v = 12; to_string(hosted.v);").as_string(), "a host-owned owner dropped everywhere else lives on");
        });

        test("host_member_lane_follows_a_rebinding", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            dynamic_binder<LaneMembers> binder(*eng, "LaneMembers");
            binder.constructor<>().property("big", &LaneMembers::big).build();
            eng->execute("var m = LaneMembers(); function read_big() -> int { return m.big; } function write_big(int v) { m.big = v; } var a = read_big(); var b = read_big(); write_big(8); write_big(9); var c = read_big();");
            binder.property("big", &LaneMembers::other);
            auto out = eng->execute("var n = LaneMembers(); write_big(71); string s = to_string(a) + \",\" + to_string(b) + \",\" + to_string(c) + \",\" + to_string(n.big) + \",\" + to_string(n.big) + \",\" + to_string(read_big()) + \",\" + to_string(read_big()); s;").as_string();
            check_eq(std::string("7,7,9,70,70,71,71"), out, "a rebinding takes every warm site to the new member, reads and stores alike");
        });

        test("a_lambda_method_without_self_ignores_the_receiver", [this]() {
            auto eng = make_engine();
            dynamic_binder<Counter>(*eng, "Counter")
                .constructor<>()
                .method("twice", [](int64_t k) { return k * 2; })
                .method("seven", []() -> int64_t { return 7; })
                .method("bump", [](int64_t& k) { k += 1; })
                .method("get", &Counter::get)
                .build();
            check_eq((int64_t)8, eng->execute("auto c = Counter(); c.twice(4)").as_int(), "one argument");
            check_eq((int64_t)7, eng->execute("c.seven()").as_int(), "no argument");
            check_eq((int64_t)6, eng->execute("int n = 5; c.bump(n); n").as_int(), "a reference argument writes back");
        });

        test("static_property_registers_its_setter", [this]() {
            auto eng = make_engine();
            int64_t limit = 3;
            dynamic_binder<Counter>(*eng, "Counter")
                .constructor<>()
                .static_property("limit", [&limit]() { return limit; }, [&limit](int64_t v) { limit = v; })
                .build();
            check_eq((int64_t)3, eng->execute("Counter::limit").as_int(), "the getter reads the host value");
            check_eq((int64_t)9, eng->execute("Counter::limit = 9; Counter::limit").as_int(), "a strict store loads and runs the setter");
            check_eq((int64_t)9, limit, "the setter wrote the host value");
            check_eq((int64_t)10, eng->execute("Counter::limit += 1; Counter::limit").as_int(), "a compound store reads and writes");
            check_eq((int64_t)10, limit);
        });

        test("static_property_binds_the_live_variable", [this]() {
            auto eng = make_engine();
            int64_t live = 1;
            std::string label = "a";
            dynamic_binder<Counter>(*eng, "Counter")
                .constructor<>()
                .static_property("live", &live)
                .static_property("label", &label)
                .build();
            live = 5;
            label = "b";
            check_eq((int64_t)5, eng->execute("Counter::live").as_int(), "a host write after registration reads in script");
            check_eq(std::string("b"), eng->execute("Counter::label").as<std::string>());
            eng->execute("Counter::live = 12; Counter::label = \"c\";");
            check_eq((int64_t)12, live, "a script store writes the variable");
            check_eq(std::string("c"), label);
            check_eq((int64_t)13, eng->execute("Counter::live += 1; Counter::live").as_int(), "a compound store");
            check_eq((int64_t)13, live);
        });

        test("basic_class_registration", [this]() {
            auto eng = make_engine();
            
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<>()
                .constructor<double, double>()
                .method("length", &DynamicBinderPoint::length)
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .build();
            
            auto result = eng->execute("auto p = Point(3, 4); p.length()");
            check_eq(result.as<double>(), 5.0);
        });
        
        test("constructors", [this]() {
            auto eng = make_engine();
            
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<>()
                .constructor<double, double>()
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .build();
            
            // Default constructor
            eng->execute("auto p1 = Point();");
            check_eq(eng->execute("p1.x").as<double>(), 0.0);
            check_eq(eng->execute("p1.y").as<double>(), 0.0);
            
            // Parameterized constructor
            eng->execute("auto p2 = Point(3, 4);");
            check_eq(eng->execute("p2.x").as<double>(), 3.0);
            check_eq(eng->execute("p2.y").as<double>(), 4.0);
        });
        
        test("methods", [this]() {
            auto eng = make_engine();
            
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .method("move_by", &DynamicBinderPoint::move_by)
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .build();
            
            eng->execute("auto p = Point(1, 2);");
            eng->execute("p.move_by(3, 4);");
            check_eq(eng->execute("p.x").as<double>(), 4.0);
            check_eq(eng->execute("p.y").as<double>(), 6.0);
        });
        
        test("properties", [this]() {
            auto eng = make_engine();
            
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<>()
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .build();
            
            eng->execute("auto p = Point();");
            eng->execute("p.x = 5;");
            eng->execute("p.y = 7;");
            check_eq(eng->execute("p.x").as<double>(), 5.0);
            check_eq(eng->execute("p.y").as<double>(), 7.0);
        });
        
        test("operator_overloading", [this]() {
            auto eng = make_engine();
            
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .method("+", &DynamicBinderPoint::operator+)
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .build();
            
            eng->execute("auto p1 = Point(1, 2);");
            eng->execute("auto p2 = Point(3, 4);");
            eng->execute("auto p3 = p1 + p2;");
            check_eq(eng->execute("p3.x").as<double>(), 4.0);
            check_eq(eng->execute("p3.y").as<double>(), 6.0);
        });
        
        test("static_methods", [this]() {
            auto eng = make_engine();

            // a static method marshals like add_function: class values, handles, containers and callbacks both ways, and a
            // stateful callable passed as an lvalue is copied into the binding
            int64_t scale = 3;
            auto scaled = [&scale](int64_t n) { return n * scale; };
            auto scaled_copy = [scale](int64_t n) { return n * scale; };
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .static_method("origin", &DynamicBinderPoint::origin)
                .static_method("shared", []() { return std::make_shared<DynamicBinderPoint>(1, 2); })
                .static_method("range", [](int64_t n) { std::vector<int64_t> result; for (int64_t i = 0; i < n; ++i) { result.push_back(i); } return result; })
                .static_method("sum", [](std::vector<int64_t> values) { int64_t result = 0; for (int64_t v : values) { result += v; } return result; })
                .static_method("apply", [](std::function<int64_t(int64_t)> f) { return f(20); })
                .static_method("distance", [](const DynamicBinderPoint& a, const DynamicBinderPoint& b) { return (a - b).length(); })
                .static_method("scaled", scaled)
                .static_method("scaled_copy", scaled_copy)
                .build();
            scale = 100;
            eng->add_function("Point_origin", &DynamicBinderPoint::origin);

            eng->execute("auto p = Point_origin();");
            check_eq(eng->execute("p.x").as<double>(), 0.0);
            check_eq(eng->execute("p.y").as<double>(), 0.0);
            check_eq(5.0, eng->execute("Point::origin().x + Point::shared().y + 3.0").as<double>(), "a class value and a shared_ptr return");
            check_eq(std::string("0,1,2"), eng->execute("Point::range(3).join(\",\")").to_string(), "a vector return");
            check_eq((int64_t)6, eng->execute("Point::sum([1, 2, 3])").as_int(), "a vector parameter");
            check_eq((int64_t)21, eng->execute("Point::apply([](int x) { return x + 1; })").as_int(), "a callback parameter");
            check_eq(5.0, eng->execute("Point::distance(Point(3.0, 4.0), Point(0.0, 0.0))").as<double>(), "const class& parameters");
            check_eq((int64_t)700, eng->execute("Point::scaled(7)").as_int(), "an lvalue callable is copied, its by-reference capture still live");
            check_eq((int64_t)21, eng->execute("Point::scaled_copy(7)").as_int(), "an lvalue callable keeps its own copy");
            auto doomed = std::make_unique<std::function<int64_t(int64_t)>>([offset = int64_t{40}](int64_t n) { return n + offset; });
            eng->add_function("offset_by", *doomed);
            doomed.reset();
            check_eq((int64_t)42, eng->execute("offset_by(2)").as_int(), "add_function copies an lvalue callable that dies after the registration");
            check_eq(std::string("'Point::sum' expects 1 argument, got 2"),
                eng->execute("[[warn_mode]]; var m = \"\"; try { Point::sum([1], [2]); } catch (err) { m = err; } m").to_string(), "the arity text names the method");
        });
        
        test("static_properties", [this]() {
            auto eng = make_engine();

            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<>()
                .build();

            // Register static property as global reference (not copy!)
            // Using add_global_ref ensures script modifications update the C++ static
            DynamicBinderPoint::point_count = 42;
            eng->add_global_ref("Point_count", DynamicBinderPoint::point_count);

            check_eq(eng->execute("Point_count").as<int>(), 42);

            eng->execute("Point_count = 100;");
            check_eq(DynamicBinderPoint::point_count, 100);
        });
        
        test("method_chaining", [this]() {
            auto eng = make_engine();
            
            dynamic_binder<Counter>(*eng, "Counter")
                .constructor<>()
                .constructor<int>()
                .method("increment", &Counter::increment)
                .method("add", &Counter::add)
                .method("get", &Counter::get)
                .method("reset", &Counter::reset)
                .build();
            
            auto result = eng->execute("Counter().increment().add(5).increment().get()");
            check_eq(result.as<int>(), 7);
        });
        
        test("lambda_methods", [this]() {
            auto eng = make_engine();
            
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .method("distance_to", [](const DynamicBinderPoint& self, const DynamicBinderPoint& other) {
                    double dx = self.x - other.x;
                    double dy = self.y - other.y;
                    return std::sqrt(dx * dx + dy * dy);
                })
                .method("scale", [](DynamicBinderPoint& self, double factor) -> DynamicBinderPoint& {
                    self.x *= factor;
                    self.y *= factor;
                    return self;
                })
                .build();
            
            eng->execute("auto p1 = Point(0, 0);");
            eng->execute("auto p2 = Point(3, 4);");
            auto dist = eng->execute("p1.distance_to(p2)");
            check_eq(dist.as<double>(), 5.0);
            
            eng->execute("p2.scale(2);");
            check_eq(eng->execute("p2.x").as<double>(), 6.0);
            check_eq(eng->execute("p2.y").as<double>(), 8.0);
        });
        
        test("custom_getters_setters", [this]() {
            auto eng = make_engine();
            
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .method("length_squared", [](const DynamicBinderPoint& self) {
                    return self.x * self.x + self.y * self.y;
                })
                .method("set_both", [](DynamicBinderPoint& self, double val) -> DynamicBinderPoint& {
                    self.x = val;
                    self.y = val;
                    return self;
                })
                .build();
            
            eng->execute("auto p = Point(3, 4);");
            check_eq(eng->execute("p.length_squared()").as<double>(), 25.0);
            
            eng->execute("p.set_both(5);");
            check_eq(eng->execute("p.x").as<double>(), 5.0);
            check_eq(eng->execute("p.y").as<double>(), 5.0);
        });
        
        test("shared_ptr_support", [this]() {
            auto eng = make_engine();
            
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .build();
            
            // Function that returns shared_ptr
            eng->add_function("make_shared_point", [](double x, double y) {
                return std::make_shared<DynamicBinderPoint>(x, y);
            });
            
            // Function that accepts shared_ptr
            eng->add_function("point_length", [](std::shared_ptr<DynamicBinderPoint> p) {
                return p->length();
            });
            
            auto result = eng->execute("auto p = make_shared_point(3, 4); point_length(p)");
            check_eq(result.as<double>(), 5.0);
        });
        
        test("const_correctness", [this]() {
            auto eng = make_engine();
            
            dynamic_binder<Counter>(*eng, "Counter")
                .constructor<int>()
                .method("get", &Counter::get)  // const method
                .method("reset", &Counter::reset)  // non-const method
                .build();
            
            eng->execute("auto c = Counter(42);");
            check_eq(eng->execute("c.get()").as<int>(), 42);
            
            eng->execute("c.reset();");
            check_eq(eng->execute("c.get()").as<int>(), 0);
        });
        
        test("overloaded_methods", [this]() {
            auto eng = make_engine();
            
            class Calculator {
            public:
                int add(int a) { return a; }
                int add(int a, int b) { return a + b; }
                int add(int a, int b, int c) { return a + b + c; }
            };
            
            dynamic_binder<Calculator>(*eng, "Calculator")
                .constructor<>()
                .method("add", 
                    static_cast<int(Calculator::*)(int)>(&Calculator::add))
                .method("add", 
                    static_cast<int(Calculator::*)(int, int)>(&Calculator::add))
                .method("add", 
                    static_cast<int(Calculator::*)(int, int, int)>(&Calculator::add))
                .build();
            
            eng->execute("auto calc = Calculator();");
            check_eq(eng->execute("calc.add(5)").as<int>(), 5);
            check_eq(eng->execute("calc.add(5, 3)").as<int>(), 8);
            check_eq(eng->execute("calc.add(5, 3, 2)").as<int>(), 10);
        });

        // Same NAME, same ARITY, different parameter TYPES must resolve by argument type. A color type
        // whose set(float...) stores channels raw and set(int...) divides by 255. Before the fix
        // the two collapsed to the last registration (int), so a float set() truncated to near-black.
        test("same_arity_overload_resolves_by_type", [this]() {
            auto eng = make_engine();

            class ColorLike {
            public:
                float r = 0, g = 0, b = 0, a = 0;
                ColorLike& set(float R, float G, float B, float A) { r=R; g=G; b=B; a=A; return *this; }
                ColorLike& set(int R, int G, int B, int A) { r=R/255.0f; g=G/255.0f; b=B/255.0f; a=A/255.0f; return *this; }
            };

            dynamic_binder<ColorLike>(*eng, "ColorLike")
                .constructor<>()
                .property("r", &ColorLike::r)
                .property("g", &ColorLike::g)
                .method("set", static_cast<ColorLike&(ColorLike::*)(float, float, float, float)>(&ColorLike::set))
                .method("set", static_cast<ColorLike&(ColorLike::*)(int, int, int, int)>(&ColorLike::set))
                .build();

            // Float arguments -> the float overload (raw); near-black ~0.004 if it were sliced to int.
            eng->execute("auto c = ColorLike(); c.set(1.0, 0.5, 0.25, 1.0);");
            check_near(1.0, eng->execute("c.r").as<double>(), 1e-5);
            check_near(0.5, eng->execute("c.g").as<double>(), 1e-5);

            // Integer arguments -> the int overload (divided by 255).
            eng->execute("auto d = ColorLike(); d.set(255, 128, 64, 255);");
            check_near(1.0, eng->execute("d.r").as<double>(), 1e-5);
            check_near(128.0/255.0, eng->execute("d.g").as<double>(), 1e-3);
        });

        // Same-arity CONSTRUCTORS (the other half of the color clobber: Color(float...) vs
        // Color(int...)) must also resolve by argument type. This also guards the registration-path
        // recursion bug: routing the 2nd ctor through the typed entry point used to migrate the
        // dispatcher into the overload set, so a 0-arg call self-recursed into a stack overflow.
        test("same_arity_constructor_resolves_by_type", [this]() {
            auto eng = make_engine();

            class Rgb {
            public:
                float r = 0, g = 0, b = 0;
                Rgb() {}
                Rgb(float R, float G, float B) { r=R; g=G; b=B; }
                Rgb(int R, int G, int B) { r=R/255.0f; g=G/255.0f; b=B/255.0f; }
            };

            dynamic_binder<Rgb>(*eng, "Rgb")
                .constructor<>()
                .constructor<float, float, float>()
                .constructor<int, int, int>()
                .property("r", &Rgb::r)
                .property("g", &Rgb::g)
                .build();

            // The 0-arg ctor must still work after typed overloads are registered (no recursion).
            eng->execute("auto z = Rgb();");
            check_near(0.0, eng->execute("z.r").as<double>(), 1e-5);

            eng->execute("auto a = Rgb(1.0, 0.5, 0.25);");
            check_near(1.0, eng->execute("a.r").as<double>(), 1e-5);
            check_near(0.5, eng->execute("a.g").as<double>(), 1e-5);

            eng->execute("auto b = Rgb(255, 128, 64);");
            check_near(1.0, eng->execute("b.r").as<double>(), 1e-5);
            check_near(128.0/255.0, eng->execute("b.g").as<double>(), 1e-3);
        });

        test("equality_operator", [this]() {
            auto eng = make_engine();

            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .method("==", &DynamicBinderPoint::operator==)
                .build();

            // Same values should be equal
            eng->execute("auto p1 = Point(3, 4);");
            eng->execute("auto p2 = Point(3, 4);");
            auto eq_result = eng->execute("p1 == p2");
            check_eq(eq_result.as<bool>(), true, "Points with same values are equal");

            // Different values should not be equal
            eng->execute("auto p3 = Point(5, 6);");
            auto neq_result = eng->execute("p1 == p3");
            check_eq(neq_result.as<bool>(), false, "Points with different values are not equal");

            // Self-equality
            auto self_eq = eng->execute("p1 == p1");
            check_eq(self_eq.as<bool>(), true, "Point equals itself");

            // Not-equal operator should also work
            auto neq_result2 = eng->execute("p1 != p3");
            check_eq(neq_result2.as<bool>(), true, "p1 != p3 is true");

            auto neq_result3 = eng->execute("p1 != p2");
            check_eq(neq_result3.as<bool>(), false, "p1 != p2 is false (same values)");
        });

        test("equality_operator_via_lambda", [this]() {
            auto eng = make_engine();

            // Test registering operator== via lambda (for cases where the class
            // doesn't have a C++ operator==, or for custom comparison logic)
            dynamic_binder<Counter>(*eng, "Counter")
                .constructor<int>()
                .method("get", &Counter::get)
                .method("==", [](const Counter& self, const Counter& other) {
                    return self.value == other.value;
                })
                .build();

            eng->execute("auto c1 = Counter(42);");
            eng->execute("auto c2 = Counter(42);");
            eng->execute("auto c3 = Counter(99);");

            check_eq(eng->execute("c1 == c2").as<bool>(), true, "Counters with same value are equal");
            check_eq(eng->execute("c1 == c3").as<bool>(), false, "Counters with different value are not equal");
            check_eq(eng->execute("c1 != c3").as<bool>(), true, "c1 != c3");
        });

        test("comparison_operators", [this]() {
            auto eng = make_engine();

            // Register Point with all comparison operators
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .method("length", &DynamicBinderPoint::length)
                .method("<", &DynamicBinderPoint::operator<)
                .method("<=", &DynamicBinderPoint::operator<=)
                .method(">", &DynamicBinderPoint::operator>)
                .method(">=", &DynamicBinderPoint::operator>=)
                .build();

            // Create points with different lengths: (3,4)=5, (0,0)=0, (6,8)=10
            eng->execute("auto p1 = Point(3, 4);");   // length = 5
            eng->execute("auto p2 = Point(0, 0);");   // length = 0
            eng->execute("auto p3 = Point(6, 8);");   // length = 10

            // Test less than
            check_eq(eng->execute("p2 < p1").as<bool>(), true, "Point(0,0) < Point(3,4)");
            check_eq(eng->execute("p1 < p3").as<bool>(), true, "Point(3,4) < Point(6,8)");
            check_eq(eng->execute("p3 < p1").as<bool>(), false, "Point(6,8) < Point(3,4) is false");

            // Test greater than
            check_eq(eng->execute("p3 > p1").as<bool>(), true, "Point(6,8) > Point(3,4)");
            check_eq(eng->execute("p1 > p2").as<bool>(), true, "Point(3,4) > Point(0,0)");
            check_eq(eng->execute("p1 > p3").as<bool>(), false, "Point(3,4) > Point(6,8) is false");

            // Test less than or equal
            check_eq(eng->execute("p1 <= p3").as<bool>(), true, "Point(3,4) <= Point(6,8)");
            eng->execute("auto p4 = Point(3, 4);");   // same as p1
            check_eq(eng->execute("p1 <= p4").as<bool>(), true, "Point(3,4) <= Point(3,4) (equal)");

            // Test greater than or equal
            check_eq(eng->execute("p3 >= p1").as<bool>(), true, "Point(6,8) >= Point(3,4)");
            check_eq(eng->execute("p1 >= p4").as<bool>(), true, "Point(3,4) >= Point(3,4) (equal)");
        });

        test("comparison_operators_via_lambda", [this]() {
            auto eng = make_engine();

            // Register Counter with comparison operators via lambda
            dynamic_binder<Counter>(*eng, "Counter")
                .constructor<int>()
                .method("get", &Counter::get)
                .method("<", [](const Counter& self, const Counter& other) {
                    return self.value < other.value;
                })
                .method(">", [](const Counter& self, const Counter& other) {
                    return self.value > other.value;
                })
                .build();

            eng->execute("auto c1 = Counter(10);");
            eng->execute("auto c2 = Counter(20);");

            check_eq(eng->execute("c1 < c2").as<bool>(), true, "Counter(10) < Counter(20)");
            check_eq(eng->execute("c2 > c1").as<bool>(), true, "Counter(20) > Counter(10)");
            check_eq(eng->execute("c2 < c1").as<bool>(), false, "Counter(20) < Counter(10) is false");
        });

        test("arithmetic_operators", [this]() {
            auto eng = make_engine();

            // Register Point with arithmetic operators
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .method("+", &DynamicBinderPoint::operator+)
                .method("-", &DynamicBinderPoint::operator-)
                .build();

            // Test addition
            eng->execute("auto p1 = Point(1, 2);");
            eng->execute("auto p2 = Point(3, 4);");
            eng->execute("auto sum = p1 + p2;");
            check_eq(eng->execute("sum.x").as<double>(), 4.0, "Point addition x");
            check_eq(eng->execute("sum.y").as<double>(), 6.0, "Point addition y");

            // Test subtraction
            eng->execute("auto diff = p2 - p1;");
            check_eq(eng->execute("diff.x").as<double>(), 2.0, "Point subtraction x");
            check_eq(eng->execute("diff.y").as<double>(), 2.0, "Point subtraction y");
        });

        test("arithmetic_operators_all", [this]() {
            auto eng = make_engine();

            // Register Counter with all arithmetic operators
            dynamic_binder<Counter>(*eng, "Counter")
                .constructor<int>()
                .method("get", &Counter::get)
                .method("+", &Counter::operator+)
                .method("-", &Counter::operator-)
                .method("*", static_cast<Counter(Counter::*)(int) const>(&Counter::operator*))
                .method("/", static_cast<Counter(Counter::*)(int) const>(&Counter::operator/))
                .method("%", static_cast<Counter(Counter::*)(int) const>(&Counter::operator%))
                .build();

            eng->execute("auto a = Counter(10);");
            eng->execute("auto b = Counter(3);");

            // Test addition
            check_eq(eng->execute("(a + b).get()").as<int>(), 13, "10 + 3 = 13");

            // Test subtraction
            check_eq(eng->execute("(a - b).get()").as<int>(), 7, "10 - 3 = 7");

            // Test multiplication
            check_eq(eng->execute("(a * 2).get()").as<int>(), 20, "10 * 2 = 20");

            // Test division
            check_eq(eng->execute("(a / 2).get()").as<int>(), 5, "10 / 2 = 5");

            // Test modulo
            check_eq(eng->execute("(a % 3).get()").as<int>(), 1, "10 % 3 = 1");
        });

        test("arithmetic_operators_via_lambda", [this]() {
            auto eng = make_engine();

            // Register Point with arithmetic operators via lambda
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .method("*", [](const DynamicBinderPoint& self, double scalar) {
                    return DynamicBinderPoint(self.x * scalar, self.y * scalar);
                })
                .method("/", [](const DynamicBinderPoint& self, double scalar) {
                    return DynamicBinderPoint(self.x / scalar, self.y / scalar);
                })
                .build();

            eng->execute("auto p = Point(4, 6);");

            // Test scalar multiplication
            eng->execute("auto scaled = p * 2;");
            check_eq(eng->execute("scaled.x").as<double>(), 8.0, "Point * 2 x");
            check_eq(eng->execute("scaled.y").as<double>(), 12.0, "Point * 2 y");

            // Test scalar division
            eng->execute("auto halved = p / 2;");
            check_eq(eng->execute("halved.x").as<double>(), 2.0, "Point / 2 x");
            check_eq(eng->execute("halved.y").as<double>(), 3.0, "Point / 2 y");
        });

        test("compound_assignment_operators", [this]() {
            auto eng = make_engine();

            // Register Point with arithmetic operators
            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .property("x", &DynamicBinderPoint::x)
                .property("y", &DynamicBinderPoint::y)
                .method("+", &DynamicBinderPoint::operator+)
                .method("-", &DynamicBinderPoint::operator-)
                .method("*", static_cast<DynamicBinderPoint(DynamicBinderPoint::*)(double) const>(&DynamicBinderPoint::operator*))
                .method("/", static_cast<DynamicBinderPoint(DynamicBinderPoint::*)(double) const>(&DynamicBinderPoint::operator/))
                .build();

            // Test += operator
            auto add_x = eng->execute(R"(
                auto p = Point(1, 2);
                p += Point(3, 4);
                p.x
            )");
            auto add_y = eng->execute(R"(
                auto p = Point(1, 2);
                p += Point(3, 4);
                p.y
            )");
            check_eq(add_x.as<double>(), 4.0, "Point += x");
            check_eq(add_y.as<double>(), 6.0, "Point += y");

            // Test -= operator
            auto sub_x = eng->execute(R"(
                auto p = Point(5, 7);
                p -= Point(2, 3);
                p.x
            )");
            auto sub_y = eng->execute(R"(
                auto p = Point(5, 7);
                p -= Point(2, 3);
                p.y
            )");
            check_eq(sub_x.as<double>(), 3.0, "Point -= x");
            check_eq(sub_y.as<double>(), 4.0, "Point -= y");

            // Test *= operator (scalar)
            auto mul_x = eng->execute(R"(
                auto p = Point(3, 4);
                p *= 2;
                p.x
            )");
            auto mul_y = eng->execute(R"(
                auto p = Point(3, 4);
                p *= 2;
                p.y
            )");
            check_eq(mul_x.as<double>(), 6.0, "Point *= x");
            check_eq(mul_y.as<double>(), 8.0, "Point *= y");

            // Test /= operator (scalar)
            auto div_x = eng->execute(R"(
                auto p = Point(6, 8);
                p /= 2;
                p.x
            )");
            auto div_y = eng->execute(R"(
                auto p = Point(6, 8);
                p /= 2;
                p.y
            )");
            check_eq(div_x.as<double>(), 3.0, "Point /= x");
            check_eq(div_y.as<double>(), 4.0, "Point /= y");
        });

        test("auto_bind_inheritance_chain", [this]() {
            // Test that auto_bind with property_owner correctly chains inheritance
            // class C : property_owner<C>        -> no base
            // class B : property_owner<B, C>     -> inherits from C
            // class A : property_owner<A, B>     -> inherits from B
            // When calling method from C on instance of A, it should work via chain A -> B -> C

            struct TestC : property_owner<TestC> {
                std::string c_method() const { return "from_C"; }
            };

            struct TestB : property_owner<TestB, TestC> {
                std::string b_method() const { return "from_B"; }
            };

            struct TestA : property_owner<TestA, TestB> {
                std::string a_method() const { return "from_A"; }
            };

            auto eng = make_engine();

            // Register in dependency order: C first, then B, then A
            dynamic_binder<TestC>(*eng, "TestC")
                .constructor<>()
                .method("c_method", &TestC::c_method)
                .build();

            dynamic_binder<TestB>(*eng, "TestB")
                .auto_bind()  // Should auto-register TestC as base
                .constructor<>()
                .method("b_method", &TestB::b_method)
                .build();

            dynamic_binder<TestA>(*eng, "TestA")
                .auto_bind()  // Should auto-register TestB as base
                .constructor<>()
                .method("a_method", &TestA::a_method)
                .build();

            // Create instance of A
            eng->execute("auto a = TestA();");

            // Test calling methods at each level
            check_eq(eng->execute("a.a_method()").as<std::string>(), "from_A", "Direct method");
            check_eq(eng->execute("a.b_method()").as<std::string>(), "from_B", "Inherited from B");
            check_eq(eng->execute("a.c_method()").as<std::string>(), "from_C", "Inherited from C via chain A->B->C");
        });

        test("inherited_value_property_binds_through_derived_auto_bind", [this]() {
            // A base-level JAI_PROPERTY read/written through a DERIVED registration must
            // hit the BASE level's property manager in either registration order (each
            // property_owner level owns its own manager; the generated accessors must
            // resolve the chain like the signal view does).
            auto run = [&](bool a_baseFirst) {
                auto eng = make_engine();
                auto bindBase = [&]() {
                    dynamic_binder<PropChainBase>(*eng, "PropChainBase")
                        .constructor<>()
                        .auto_bind()
                        .build();
                };
                auto bindDerived = [&]() {
                    dynamic_binder<PropChainDerived>(*eng, "PropChainDerived")
                        .constructor<>()
                        .auto_bind()
                        .build();
                };
                if (a_baseFirst) { bindBase(); bindDerived(); } else { bindDerived(); bindBase(); }
                eng->execute("auto d = PropChainDerived();");
                check_eq(eng->execute("d.armor").as_int(), (int64_t)7,
                    a_baseFirst ? "base-first read" : "derived-first read");
                eng->execute("d.armor = 11;");
                check_eq(eng->execute("d.armor").as_int(), (int64_t)11,
                    a_baseFirst ? "base-first write" : "derived-first write");
            };
            run(true);
            run(false);
        });

        // === property_owner does all the things: the CRTP declaration is the truth,
        // === engine registration is a complete projection of it.

        test("property_owner_base_auto_registers_on_derived_registration", [this]() {
            // The base is NEVER explicitly registered: registering the derived class
            // materializes it (methods via jai_auto_bind, schema, parent edge).
            auto eng = make_engine();
            dynamic_binder<AutoChainDerived>(*eng, "AutoChainDerived")
                .constructor<>()
                .auto_bind()
                .build();
            eng->execute("auto d = AutoChainDerived();");
            check_eq(eng->execute("d.base_shout()").as<std::string>(), "from_base",
                "base method reachable through the auto-registered base definition");
            check_eq(eng->execute("d.armor_base").as_int(), (int64_t)3,
                "base property reachable");
        });

        test("explicit_base_registration_adopts_auto_registered", [this]() {
            // Derived registers FIRST (auto-registers the base); the explicit base
            // registration then ADOPTS that definition — same object, renamed — so
            // methods added by the explicit registrar reach existing derived bindings.
            auto eng = make_engine();
            dynamic_binder<AutoChainDerived>(*eng, "AutoChainDerived")
                .constructor<>()
                .auto_bind()
                .build();
            dynamic_binder<AutoChainBase>(*eng, "NiceBase")
                .method("extra", [](AutoChainBase&) { return (int64_t)42; })
                .build();
            eng->execute("auto d = AutoChainDerived();");
            check_eq(eng->execute("d.base_shout()").as<std::string>(), "from_base",
                "pre-adoption method survives");
            check_eq(eng->execute("d.extra()").as_int(), (int64_t)42,
                "post-adoption method reaches derived instances (same definition)");
            eng->execute("auto b = NiceBase();");
            check_eq(eng->execute("b.base_shout()").as<std::string>(), "from_base",
                "curated name constructs the adopted definition");
        });

        test("pinned_base_auto_registers_under_pin", [this]() {
            auto eng = make_engine();
            dynamic_binder<PinnedNameDerived>(*eng, "PinnedNameDerived")
                .constructor<>()
                .auto_bind()
                .build();
            eng->execute("auto p = NicePinned();");
            check_eq(eng->execute("p.pin_field").as_int(), (int64_t)9,
                "auto-registered base carries its JAI_PROPERTY_OWNER_AS pin as the script name");
        });

        test("base_auto_registration_declines_name_collision", [this]() {
            // A DIFFERENT type already owns the ladder name: auto-registration declines
            // (deferred link keeps waiting); the schema-vacuumed property still works.
            auto eng = make_engine();
            dynamic_binder<Counter>(*eng, "CollideNameBase")
                .constructor<>()
                .build();
            dynamic_binder<CollideNameDerived>(*eng, "CollideNameDerived")
                .constructor<>()
                .auto_bind()
                .build();
            eng->execute("auto d = CollideNameDerived();");
            check_eq(eng->execute("d.collide_field").as_int(), (int64_t)5,
                "vacuumed base property survives the declined auto-registration");
            bool method_reachable = true;
            try { eng->execute("d.collide_shout();"); } catch (...) { method_reachable = false; }
            check_false(method_reachable, "base methods stay unavailable until the base registers");
        });

        test("adoption_under_live_instances_is_an_invariant_violation", [this]() {
            // Adoption renames the definition; live placeholder instances would lose
            // their type identity — the binder throws at the registration site instead.
            auto eng = make_engine();
            dynamic_binder<AutoChainDerived>(*eng, "AutoChainDerived")
                .constructor<>()
                .auto_bind()
                .build();
            eng->execute("auto stray = AutoChainBase();");   // placeholder identity, alive
            check_throws([&]() {
                dynamic_binder<AutoChainBase>(*eng, "TooLateBase")
                    .method("extra", [](AutoChainBase&) { return (int64_t)1; })
                    .build();
            }, "explicit registration after placeholder instances exist throws");
        });

        test("explicit_property_after_auto_bind_wins", [this]() {
            // The precedence contract: an explicit .property() placed AFTER auto_bind()
            // overrides the schema-generated accessor for that name.
            auto eng = make_engine();
            dynamic_binder<PropChainBase>(*eng, "PropChainBase")
                .constructor<>()
                .auto_bind()
                .property("armor",
                    [](PropChainBase&) { return (int64_t)999; },
                    [](PropChainBase&, int64_t) {})
                .build();
            eng->execute("auto p = PropChainBase();");
            check_eq(eng->execute("p.armor").as_int(), (int64_t)999,
                "explicit accessor replaced the schema one");
        });

        test("auto_bind_multiple_inheritance", [this]() {
            // Test that auto_bind correctly handles multiple base classes
            // class Left  : property_owner<Left>           -> no base
            // class Right : property_owner<Right>          -> no base
            // class Multi : property_owner<Multi, Left, Right>  -> inherits from BOTH
            // Methods from both Left and Right should be accessible on Multi

            struct Left : property_owner<Left> {
                std::string left_method() const { return "from_Left"; }
            };

            struct Right : property_owner<Right> {
                std::string right_method() const { return "from_Right"; }
            };

            struct Multi : property_owner<Multi, Left, Right> {
                std::string multi_method() const { return "from_Multi"; }
            };

            auto eng = make_engine();

            // Register base classes first
            dynamic_binder<Left>(*eng, "Left")
                .constructor<>()
                .method("left_method", &Left::left_method)
                .build();

            dynamic_binder<Right>(*eng, "Right")
                .constructor<>()
                .method("right_method", &Right::right_method)
                .build();

            // Register Multi with auto_bind - should auto-register both Left and Right as bases
            dynamic_binder<Multi>(*eng, "Multi")
                .auto_bind()  // Should register both Left AND Right as bases
                .constructor<>()
                .method("multi_method", &Multi::multi_method)
                .build();

            // Create instance of Multi
            eng->execute("auto m = Multi();");

            // Test calling methods from all three classes
            check_eq(eng->execute("m.multi_method()").as<std::string>(), "from_Multi", "Direct method");
            check_eq(eng->execute("m.left_method()").as<std::string>(), "from_Left", "Inherited from Left");
            check_eq(eng->execute("m.right_method()").as<std::string>(), "from_Right", "Inherited from Right");
        });

        test("auto_bind_deep_chain", [this]() {
            // Test deep inheritance chain with multiple inheritance
            // This tests that chains are followed correctly even with complex hierarchies
            //
            //   GrandparentA     GrandparentB
            //        |               |
            //     ParentA         ParentB
            //        \             /
            //          Child
            //
            // Child inherits from ParentA and ParentB, which have independent grandparents
            // All methods should be accessible

            struct GrandparentA : property_owner<GrandparentA> {
                std::string gpa_method() const { return "from_GrandparentA"; }
            };

            struct GrandparentB : property_owner<GrandparentB> {
                std::string gpb_method() const { return "from_GrandparentB"; }
            };

            struct ParentA : property_owner<ParentA, GrandparentA> {
                std::string pa_method() const { return "from_ParentA"; }
            };

            struct ParentB : property_owner<ParentB, GrandparentB> {
                std::string pb_method() const { return "from_ParentB"; }
            };

            struct DeepChild : property_owner<DeepChild, ParentA, ParentB> {
                std::string child_method() const { return "from_Child"; }
            };

            auto eng = make_engine();

            // Register in dependency order (grandparents first)
            dynamic_binder<GrandparentA>(*eng, "GrandparentA")
                .constructor<>()
                .method("gpa_method", &GrandparentA::gpa_method)
                .build();

            dynamic_binder<GrandparentB>(*eng, "GrandparentB")
                .constructor<>()
                .method("gpb_method", &GrandparentB::gpb_method)
                .build();

            dynamic_binder<ParentA>(*eng, "ParentA")
                .auto_bind()
                .constructor<>()
                .method("pa_method", &ParentA::pa_method)
                .build();

            dynamic_binder<ParentB>(*eng, "ParentB")
                .auto_bind()
                .constructor<>()
                .method("pb_method", &ParentB::pb_method)
                .build();

            dynamic_binder<DeepChild>(*eng, "DeepChild")
                .auto_bind()
                .constructor<>()
                .method("child_method", &DeepChild::child_method)
                .build();

            // Create instance
            eng->execute("auto c = DeepChild();");

            // Test all methods are accessible (including grandparent methods)
            check_eq(eng->execute("c.child_method()").as<std::string>(), "from_Child", "Direct method");
            check_eq(eng->execute("c.pa_method()").as<std::string>(), "from_ParentA", "Inherited from ParentA");
            check_eq(eng->execute("c.pb_method()").as<std::string>(), "from_ParentB", "Inherited from ParentB");
            check_eq(eng->execute("c.gpa_method()").as<std::string>(), "from_GrandparentA", "Inherited from GrandparentA via ParentA");
            check_eq(eng->execute("c.gpb_method()").as<std::string>(), "from_GrandparentB", "Inherited from GrandparentB via ParentB");
        });

        test("auto_bind_operator_equality", [this]() {
            // Test that auto_bind detects and registers operator== and operator!=
            struct EqualityTest {
                int value;
                EqualityTest() : value(0) {}
                EqualityTest(int v) : value(v) {}
                bool operator==(const EqualityTest& other) const { return value == other.value; }
            };

            auto eng = make_engine();

            dynamic_binder<EqualityTest>(*eng, "EqualityTest")
                .auto_bind()  // Should auto-detect operator== and register == and !=
                .constructor<int>()
                .property("value", &EqualityTest::value);

            // Test that == was auto-registered
            check(eng->execute("auto a = EqualityTest(5); auto b = EqualityTest(5); a == b").as<bool>(),
                  "Same values should be equal");
            check(!eng->execute("auto a = EqualityTest(5); auto b = EqualityTest(10); a == b").as<bool>(),
                  "Different values should not be equal");

            // Test that != was auto-registered (derived from ==)
            check(!eng->execute("auto a = EqualityTest(5); auto b = EqualityTest(5); a != b").as<bool>(),
                  "Same values should not be not-equal");
            check(eng->execute("auto a = EqualityTest(5); auto b = EqualityTest(10); a != b").as<bool>(),
                  "Different values should be not-equal");
        });

        test("jai_property_auto_bind_read", [this]() {
            // Test that JAI_PROPERTY macros work with auto_bind for reading properties
            auto eng = make_engine();

            dynamic_binder<PropertyOwnerTest>(*eng, "PropertyOwnerTest")
                .auto_bind()  // Should auto-detect JAI_PROPERTY macros and bind them
                .constructor<>()
                .method("get_health_cpp", &PropertyOwnerTest::get_health_cpp)
                .build();

            // Test reading default property values from script
            eng->execute("auto obj = PropertyOwnerTest();");
            check_eq(eng->execute("obj.health").as<int>(), 100, "Default health value");
            check_eq(eng->execute("obj.speed").as<float>(), 1.5f, "Default speed value");
            check_eq(eng->execute("obj.name").as<std::string>(), "Player", "Default name value");
            check_eq(eng->execute("obj.active").as<bool>(), true, "Default active value");
        });

        test("jai_property_auto_bind_write", [this]() {
            // Test that JAI_PROPERTY macros work with auto_bind for writing properties
            // This is the critical test: script modifications must update the C++ property
            auto eng = make_engine();

            dynamic_binder<PropertyOwnerTest>(*eng, "PropertyOwnerTest")
                .auto_bind()
                .constructor<>()
                .method("get_health_cpp", &PropertyOwnerTest::get_health_cpp)
                .build();

            // Create a C++ object and expose it to script using make_object
            auto cpp_obj = std::make_shared<PropertyOwnerTest>();
            check_eq(cpp_obj->health.get(), 100, "Initial C++ health is 100");

            eng->add_global("test_obj", eng->make_object(cpp_obj));

            // Modify property from script
            eng->execute("test_obj.health = 50;");

            // Verify C++ side sees the change
            check_eq(cpp_obj->health.get(), 50, "C++ health updated to 50 after script assignment");

            // Verify script can read the updated value
            check_eq(eng->execute("test_obj.health").as<int>(), 50, "Script reads updated health");

            // Test other property types
            eng->execute("test_obj.speed = 3.0;");
            check_eq(cpp_obj->speed.get(), 3.0f, "C++ speed updated after script assignment");

            eng->execute("test_obj.name = \"Hero\";");
            check_eq(cpp_obj->name.get(), "Hero", "C++ name updated after script assignment");

            eng->execute("test_obj.active = false;");
            check_eq(cpp_obj->active.get(), false, "C++ active updated after script assignment");
        });

        test("jai_property_auto_bind_compound_assignment", [this]() {
            // Test that compound assignment operators work with JAI_PROPERTY
            auto eng = make_engine();

            dynamic_binder<PropertyOwnerTest>(*eng, "PropertyOwnerTest")
                .auto_bind()
                .constructor<>()
                .build();

            auto cpp_obj = std::make_shared<PropertyOwnerTest>();
            eng->add_global("obj", eng->make_object(cpp_obj));

            // Test += on int property
            eng->execute("obj.health += 25;");
            check_eq(cpp_obj->health.get(), 125, "health += 25 works");

            // Test -= on int property
            eng->execute("obj.health -= 50;");
            check_eq(cpp_obj->health.get(), 75, "health -= 50 works");

            // Test *= on float property
            eng->execute("obj.speed *= 2;");
            check_eq(cpp_obj->speed.get(), 3.0f, "speed *= 2 works");

            // Test /= on float property
            eng->execute("obj.speed /= 3;");
            check_eq(cpp_obj->speed.get(), 1.0f, "speed /= 3 works");
        });

        test("jai_property_cpp_to_script_sync", [this]() {
            // Test that C++ modifications to properties are visible in script
            auto eng = make_engine();

            dynamic_binder<PropertyOwnerTest>(*eng, "PropertyOwnerTest")
                .auto_bind()
                .constructor<>()
                .method("set_health_cpp", &PropertyOwnerTest::set_health_cpp)
                .build();

            auto cpp_obj = std::make_shared<PropertyOwnerTest>();
            eng->add_global("obj", eng->make_object(cpp_obj));

            // Modify from C++
            cpp_obj->health = 999;

            // Script should see the C++ change
            check_eq(eng->execute("obj.health").as<int>(), 999, "Script sees C++ modification");

            // Modify via bound C++ method
            eng->execute("obj.set_health_cpp(42);");
            check_eq(eng->execute("obj.health").as<int>(), 42, "Script sees method modification");
            check_eq(cpp_obj->health.get(), 42, "C++ sees method modification");
        });

        test("observable_property_auto_bind", [this]() {
            // Test that JAI_OBSERVABLE_PROPERTY macros work with auto_bind
            auto eng = make_engine();

            dynamic_binder<ObservableOwnerTest>(*eng, "ObservableOwnerTest")
                .auto_bind()
                .constructor<>()
                .method("set_score_cpp", &ObservableOwnerTest::set_score_cpp)
                .build();

            // Test reading and writing observable properties
            auto cpp_obj = std::make_shared<ObservableOwnerTest>();
            eng->add_global("obj", eng->make_object(cpp_obj));

            check_eq(eng->execute("obj.score").as<int>(), 0, "Default score is 0");
            eng->execute("obj.score = 42;");
            check_eq(cpp_obj->score.get(), 42, "C++ sees script modification");
            check_eq(eng->execute("obj.score").as<int>(), 42, "Script sees its own modification");

            // Test string observable property
            check_eq(eng->execute("obj.status").as<std::string>(), "idle", "Default status is 'idle'");
            eng->execute("obj.status = \"running\";");
            check_eq(cpp_obj->status.get(), "running", "C++ sees string modification");
        });

        test("observable_property_script_callback", [this]() {
            // Test that script can register callbacks for observable property changes
            auto eng = make_engine();

            dynamic_binder<ObservableOwnerTest>(*eng, "ObservableOwnerTest")
                .auto_bind()
                .constructor<>()
                .method("set_score_cpp", &ObservableOwnerTest::set_score_cpp)
                .build();

            auto cpp_obj = std::make_shared<ObservableOwnerTest>();
            eng->add_global("obj", eng->make_object(cpp_obj));

            // Create a script variable to track callback invocations
            eng->execute("var callback_count = 0;");
            eng->execute("var last_old_value = -1;");
            eng->execute("var last_new_value = -1;");

            // Register a callback on score changes using JaiScript lambda syntax
            eng->execute(R"(
                obj.on_score_change([&](auto old_val, auto new_val) {
                    callback_count += 1;
                    last_old_value = old_val;
                    last_new_value = new_val;
                });
            )");

            // Initially no callbacks should have fired
            check_eq(eng->execute("callback_count").as<int>(), 0, "No callbacks yet");

            // Modify from script - should trigger callback
            eng->execute("obj.score = 10;");
            check_eq(eng->execute("callback_count").as<int>(), 1, "Callback fired once");
            check_eq(eng->execute("last_old_value").as<int>(), 0, "Old value was 0");
            check_eq(eng->execute("last_new_value").as<int>(), 10, "New value is 10");

            // Modify from C++ - should also trigger callback
            cpp_obj->score = 25;
            check_eq(eng->execute("callback_count").as<int>(), 2, "Callback fired again from C++");
            check_eq(eng->execute("last_old_value").as<int>(), 10, "Old value was 10");
            check_eq(eng->execute("last_new_value").as<int>(), 25, "New value is 25");

            // Setting to same value should NOT trigger callback (observable_property checks for change)
            eng->execute("obj.score = 25;");
            check_eq(eng->execute("callback_count").as<int>(), 2, "No callback for same value");
        });

        test("observable_property_multiple_callbacks", [this]() {
            // Test that multiple script callbacks can be registered
            auto eng = make_engine();

            dynamic_binder<ObservableOwnerTest>(*eng, "ObservableOwnerTest")
                .auto_bind()
                .constructor<>()
                .build();

            auto cpp_obj = std::make_shared<ObservableOwnerTest>();
            eng->add_global("obj", eng->make_object(cpp_obj));

            eng->execute("var sum = 0;");

            // Register multiple callbacks using JaiScript lambda syntax
            eng->execute("obj.on_score_change([&](auto old_val, auto new_val) { sum += new_val; });");
            eng->execute("obj.on_score_change([&](auto old_val, auto new_val) { sum += new_val * 2; });");

            // Trigger change - both callbacks should fire
            eng->execute("obj.score = 10;");

            // sum should be 10 + 20 = 30
            check_eq(eng->execute("sum").as<int>(), 30, "Both callbacks fired (10 + 20)");
        });

        test("observable_property_on_change_syntax", [this]() {
            // Test the new player.score.on_change(callback) API syntax
            // This uses transparent wrappers to allow accessing on_change on the property itself
            auto eng = make_engine();

            dynamic_binder<ObservableOwnerTest>(*eng, "ObservableOwnerTest")
                .auto_bind()
                .constructor<>()
                .build();

            auto cpp_obj = std::make_shared<ObservableOwnerTest>();
            eng->add_global("obj", eng->make_object(cpp_obj));

            // Create tracking variables
            eng->execute("var change_count = 0;");
            eng->execute("var last_old = -1;");
            eng->execute("var last_new = -1;");

            // Register callback using new syntax: obj.score.on_change(callback)
            eng->execute(R"(
                obj.score.on_change([&](auto old_val, auto new_val) {
                    change_count += 1;
                    last_old = old_val;
                    last_new = new_val;
                });
            )");

            // Initially no callbacks
            check_eq(eng->execute("change_count").as<int>(), 0, "No callbacks initially");

            // Test that obj.score still works as an integer (transparent wrapper)
            check_eq(eng->execute("obj.score").as<int>(), 0, "obj.score returns int value");
            check_eq(eng->execute("obj.score + 5").as<int>(), 5, "obj.score + 5 works (transparent unwrap)");

            // Modify from script - should trigger callback
            eng->execute("obj.score = 42;");
            check_eq(eng->execute("change_count").as<int>(), 1, "Callback fired");
            check_eq(eng->execute("last_old").as<int>(), 0, "Old value was 0");
            check_eq(eng->execute("last_new").as<int>(), 42, "New value is 42");

            // Verify arithmetic still works on the property
            check_eq(eng->execute("obj.score * 2").as<int>(), 84, "obj.score * 2 works");
            check_eq(eng->execute("obj.score - 2").as<int>(), 40, "obj.score - 2 works");

            // Modify from C++ - should also trigger callback
            cpp_obj->score = 100;
            check_eq(eng->execute("change_count").as<int>(), 2, "Callback fired from C++ change");
            check_eq(eng->execute("last_new").as<int>(), 100, "New value is 100");
        });

        test("observable_property_script_receiver_failure_throws_out_of_the_change", [this]() {
            // Every script receiver throws out of the emit (docs/site/embedding/ signals): a failing on_change callback
            // raises at the store that changed the value, in both spellings, on both backends
            auto eng = make_engine();
            dynamic_binder<ObservableOwnerTest>(*eng, "ObservableOwnerTest")
                .auto_bind()
                .constructor<>()
                .build();
            auto cpp_obj = std::make_shared<ObservableOwnerTest>();
            eng->add_global("obj", eng->make_object(cpp_obj));
            eng->execute("obj.score.on_change([](auto old_val, auto new_val) { throw \"receiver failed\"; });");
            check_eq(std::string("receiver failed"), eng->execute("string r = \"none\"; try { obj.score = 5; } catch (e) { r = e; } r;").as<std::string>(),
                "the property ref's on_change");
            auto second = make_engine();
            dynamic_binder<ObservableOwnerTest>(*second, "ObservableOwnerTest")
                .auto_bind()
                .constructor<>()
                .build();
            auto other = std::make_shared<ObservableOwnerTest>();
            second->add_global("obj", second->make_object(other));
            second->execute("obj.on_score_change([](auto old_val, auto new_val) { int z = 0; int q = 1 / z; });");
            check_eq(std::string("Division by zero"), second->execute("string r = \"none\"; try { obj.score = 5; } catch (e) { r = e; } r;").as<std::string>(),
                "the owner's on_<name>_change");
        });

        // A host object whose observable property script watched may outlive the engine: a later change skips the script
        // callbacks (they never reach the dead engine), C++ receivers still fire, and dropping the object runs no script
        test("observable_property_script_receivers_skip_once_their_engine_is_destroyed", [this]() {
            int noted = 0;
            int host = 0;
            auto cpp_obj = std::make_shared<ObservableOwnerTest>();
            auto hostReceiver = cpp_obj->score.on_change.connect([&host](const int&, const int& a_new) { host = a_new; });
            {
                auto eng = make_engine();
                dynamic_binder<ObservableOwnerTest>(*eng, "ObservableOwnerTest")
                    .auto_bind()
                    .constructor<>()
                    .build();
                eng->add_function("note", [&noted](int v) { noted += v; });
                eng->add_global("obj", eng->make_object(cpp_obj));
                eng->execute(R"(
                    class Ear { ~Ear() { note(100000); } void hear(int v) { note(v); } }
                    var held = new Ear();
                    obj.score.on_change([held](auto old_val, auto new_val) { held.hear(new_val); });
                    obj.on_score_change([held](auto old_val, auto new_val) { held.hear(new_val * 10); });
                )");
                cpp_obj->score = 2;
                check_eq(22, noted, "both spellings run while the engine lives");
            }
            const int atDeath = noted;
            cpp_obj->score = 3;
            check_eq(atDeath, noted, "no script callback runs after its engine is destroyed");
            check_eq(3, host, "C++ receivers still fire");
            cpp_obj.reset();
            check_eq(atDeath, noted, "dropping the object releases the callbacks without running script");
        });

        test("observable_property_transparent_comparison", [this]() {
            // Test that observable properties work in comparisons (transparent wrapper)
            auto eng = make_engine();

            dynamic_binder<ObservableOwnerTest>(*eng, "ObservableOwnerTest")
                .auto_bind()
                .constructor<>()
                .build();

            auto cpp_obj = std::make_shared<ObservableOwnerTest>();
            eng->add_global("obj", eng->make_object(cpp_obj));

            cpp_obj->score = 50;

            // Test comparisons with the transparent wrapper
            check_eq(eng->execute("obj.score == 50").as<bool>(), true, "obj.score == 50");
            check_eq(eng->execute("obj.score != 50").as<bool>(), false, "obj.score != 50 is false");
            check_eq(eng->execute("obj.score > 40").as<bool>(), true, "obj.score > 40");
            check_eq(eng->execute("obj.score < 60").as<bool>(), true, "obj.score < 60");
            check_eq(eng->execute("obj.score >= 50").as<bool>(), true, "obj.score >= 50");
            check_eq(eng->execute("obj.score <= 50").as<bool>(), true, "obj.score <= 50");
        });

        // Bare-identifier call args are escape-marked (their decls CELL-box so script ref
        // params can bind) and var decls flatten value type_info to 'any' (slot dynamism
        // ruling) — C++ boundaries must classify by STORAGE and deref before converting.
        // Red-locked from an editor panel host: p.textField(root, ...) with root = a
        // plain local threw "Unsupported type for non-const reference extraction"; the
        // free-function shared_ptr shape hard-crashed (Point bytes reinterpreted as a
        // shared_ptr<Point> in function_binder's create_argument).
        test("bare_local_object_args_to_cpp_boundaries", [this]() {
            auto eng = make_engine();

            dynamic_binder<DynamicBinderPoint>(*eng, "Point")
                .constructor<double, double>()
                .method("length", &DynamicBinderPoint::length)
                .method("plus", [](DynamicBinderPoint& a_self, const DynamicBinderPoint& a_other) { return DynamicBinderPoint(a_self.x + a_other.x, a_self.y + a_other.y); })
                .method("shared_x", [](DynamicBinderPoint& a_self, const std::shared_ptr<DynamicBinderPoint>& a_other) { (void)a_self; return a_other ? a_other->x : -1.0; })
                .method("shared_pair", [](DynamicBinderPoint& a_self) {
                    (void)a_self;
                    return std::vector<std::shared_ptr<DynamicBinderPoint>>{ std::make_shared<DynamicBinderPoint>(3.0, 4.0), std::make_shared<DynamicBinderPoint>(6.0, 8.0) };
                })
                .build();
            eng->add_function("free_length", [](const std::shared_ptr<DynamicBinderPoint>& a_point) { return a_point ? a_point->length() : -1.0; });
            eng->add_global("shared_point", eng->make_object(std::make_shared<DynamicBinderPoint>(6.0, 8.0)));
            // To-script mirror: a const shared_ptr<T>& RETURN must route to the shared_ptr
            // specialization, not a registry lookup of a class named "std::shared_ptr<T>".
            auto sharedOut = std::make_shared<DynamicBinderPoint>(6.0, 8.0);
            eng->add_function("shared_ref_out", [sharedOut]() -> const std::shared_ptr<DynamicBinderPoint>& { return sharedOut; });
            // NON-const lvalue handle through make_value: the universal-reference overload
            // outranks const shared_ptr<T>& here — the funnel must still share the pointee
            // (the sim's creature-signal shape).
            auto lvalueHandle = std::make_shared<DynamicBinderPoint>(3.0, 4.0);
            eng->add_global("lvalue_handle", eng->make_value(lvalueHandle));

            double diagonal = std::sqrt(4.0 * 4.0 + 6.0 * 6.0);
            check_near(5.0, eng->execute("var p0 = Point(3.0, 4.0); p0.length();").as<double>(), 0.0001);
            check_near(diagonal, eng->execute("var p1 = Point(1.0, 2.0); p1.plus(Point(3.0, 4.0)).length();").as<double>(), 0.0001);
            check_near(diagonal, eng->execute("var qB = Point(3.0, 4.0); Point(1.0, 2.0).plus(qB).length();").as<double>(), 0.0001);
            check_near(diagonal, eng->execute("var p = Point(1.0, 2.0); var q = Point(3.0, 4.0); var sum = p.plus(q); sum.length();").as<double>(), 0.0001);
            check_near(6.0, eng->execute("var sp = shared_point; var p2 = Point(0.0, 0.0); p2.shared_x(sp);").as<double>(), 0.0001);
            check_near(10.0, eng->execute("var sp2 = shared_point; free_length(sp2);").as<double>(), 0.0001);
            check_near(10.0, eng->execute("var so = shared_ref_out(); so.length();").as<double>(), 0.0001);
            check_near(5.0, eng->execute("lvalue_handle.length();").as<double>(), 0.0001);
            // vector<shared_ptr<T>> elements share identity (the creaturesInRange shape) —
            // the old fallback wrapped each HANDLE as an object of class "std::shared_ptr<T>".
            check_near(15.0, eng->execute("var pv = Point(0.0, 0.0); var handles = pv.shared_pair(); handles[0].length() + handles[1].length();").as<double>(), 0.0001);
        });

        // A handle typed as its BASE must expose the most-derived registered surface —
        // code holding a shared_ptr<Base> still reaches the Derived methods.
        test("handles_expose_dynamic_type_surface", [this]() {
            auto eng = make_engine();
            dynamic_binder<base_shape>(*eng, "BaseShape")
                .method("base_val", &base_shape::base_val)
                .build();
            dynamic_binder<derived_shape>(*eng, "DerivedShape")
                .base_class<base_shape>()
                .method("derived_val", &derived_shape::derived_val)
                .build();
            std::shared_ptr<base_shape> asBase = std::make_shared<derived_shape>();
            auto shapeValue = eng->make_object(asBase);
            check_eq(std::string("DerivedShape"), shapeValue.get_type_info() ? shapeValue.get_type_info()->type_name : std::string("null"));
            eng->add_global("shape", shapeValue);
            check_near(2.0, eng->execute("shape.derived_val();").as<double>(), 0.0001);
            check_near(1.0, eng->execute("shape.base_val();").as<double>(), 0.0001);
            eng->add_global("shape2", eng->make_object(std::make_shared<derived_shape>()));
            check_near(1.0, eng->execute("shape2.base_val();").as<double>(), 0.0001);
        });
        test("mutual_references_register_in_either_order", [this]() {
            const char* script = "var h = Hangar(); var p = Plane(); p.home = h; h.plane = p; p.home.bays + h.plane.seats;";
            auto first = make_engine();
            dynamic_binder<binder_hangar>(*first, "Hangar").property("plane", &binder_hangar::plane).property("bays", &binder_hangar::bays);
            dynamic_binder<binder_plane>(*first, "Plane").property("home", &binder_plane::home).property("seats", &binder_plane::seats);
            check_eq((int64_t)5, first->execute(script).as_int());
            check_eq((size_t)1, detail::engine_access::binding_settle_count(*first));

            auto second = make_engine();
            dynamic_binder<binder_plane>(*second, "Plane").property("home", &binder_plane::home).property("seats", &binder_plane::seats);
            dynamic_binder<binder_hangar>(*second, "Hangar").property("plane", &binder_hangar::plane).property("bays", &binder_hangar::bays);
            check_eq((int64_t)5, second->execute(script).as_int());
        });

        test("registrars_that_name_each_other_bind_in_name_order", [this]() {
            auto eng = make_engine();
            bind_registrar(*eng, binder_registrar_context{});
            check_eq((int64_t)5, eng->execute("var h = AaRegistrarHangar(); var p = ZzRegistrarPlane(); p.home = h; h.plane = p; p.home.bays + h.plane.seats;").as_int());
        });

        // An engine with nothing pending never enters the settle path: the gate is one flag test
        test("settled_bindings_cost_no_gate_work", [this]() {
            auto eng = make_engine();
            dynamic_binder<binder_plane>(*eng, "Plane").property("seats", &binder_plane::seats);
            check_eq((int64_t)2, eng->execute("Plane().seats;").as_int());
            auto bite = eng->jaibite("Plane().seats + 1;");
            check_eq((int64_t)3, eng->execute(bite).as_int());
            check_false(eng->check("var p = Plane();").has_errors());
            check_eq((size_t)0, detail::engine_access::binding_settle_count(*eng));
        });

        test("unresolved_type_is_listed_at_each_load_until_registered", [this]() {
            auto eng = make_engine();
            dynamic_binder<binder_hangar>(*eng, "Hangar").property("plane", &binder_hangar::plane).property("bays", &binder_hangar::bays);
            const std::string expected =
                "Bindings are incomplete:\n"
                "  'Hangar.plane' uses the C++ type 'jai::foundry::tests::binder_plane', which is not registered. "
                "Register it: jai::dynamic_binder<jai::foundry::tests::binder_plane>(engine, \"binder_plane\");";
            const auto failure_of = [&](auto&& load) {
                try { load(); } catch (const jai::runtime_error& e) { return std::string(e.what()); }
                return std::string("no error");
            };
            check_eq(expected, failure_of([&]() { eng->execute("1;"); }));
            check_eq(expected, failure_of([&]() { eng->check("1;"); }));
            check_eq(expected, failure_of([&]() { eng->jaibite("1;"); }));
            dynamic_binder<binder_plane>(*eng, "Plane").property("seats", &binder_plane::seats);
            check_eq((int64_t)3, eng->execute("Hangar().bays;").as_int());
            const size_t settled = detail::engine_access::binding_settle_count(*eng);
            check_eq((int64_t)2, eng->execute("Plane().seats;").as_int());
            check_eq(settled, detail::engine_access::binding_settle_count(*eng));
        });

        // The root's commit links a leaf that already reaches it through the middle: build() throws that, the implicit
        // commit records it for the first load
        test("implicit_commit_failure_is_reported_at_first_load", [this]() {
            const auto bind_middle_and_leaf = [](engine& eng) {
                dynamic_binder<binder_chain_middle>(eng, "Middle").base_class<binder_chain_root>();
                dynamic_binder<binder_chain_leaf>(eng, "Leaf").base_class<binder_chain_middle>().base_class<binder_chain_root>();
            };
            auto explicit_engine = make_engine();
            bind_middle_and_leaf(*explicit_engine);
            check_throws([&]() { dynamic_binder<binder_chain_root>(*explicit_engine, "Root").build(); });

            auto eng = make_engine();
            bind_middle_and_leaf(*eng);
            dynamic_binder<binder_chain_root>(*eng, "Root").property("depth", &binder_chain_root::depth);
            std::string failure;
            try { eng->execute("1;"); } catch (const jai::runtime_error& e) { failure = e.what(); }
            check_eq(std::string("Bindings are incomplete:\n  class 'Root' failed to register: Diamond inheritance detected while resolving a deferred base-class link"), failure);
            check_eq((int64_t)2, eng->execute("1 + 1;").as_int());
        });

        test("live_binder_commits_at_first_load_and_keeps_binding", [this]() {
            auto eng = make_engine();
            dynamic_binder<binder_hangar> hangar(*eng, "Hangar");
            hangar.property("bays", &binder_hangar::bays).property("plane", &binder_hangar::plane);
            dynamic_binder<binder_plane> plane(*eng, "Plane");
            plane.property("home", &binder_plane::home).property("seats", &binder_plane::seats);
            check_eq((int64_t)5, eng->execute("var h = Hangar(); var p = Plane(); p.home = h; h.plane = p; p.home.bays + h.plane.seats;").as_int());
            hangar.method("bay_count", &binder_hangar::bay_count);
            hangar.build();
            check_eq((int64_t)3, eng->execute("Hangar().bay_count();").as_int());
        });

        test("moved_binder_commits_once_from_its_new_home", [this]() {
            auto eng = make_engine();
            {
                dynamic_binder<binder_hangar> first(*eng, "Hangar");
                first.property("bays", &binder_hangar::bays);
                dynamic_binder<binder_plane> plane(*eng, "Plane");
                dynamic_binder<binder_hangar> moved(std::move(first));
                moved.method("bay_count", &binder_hangar::bay_count);
                check_eq((int64_t)6, eng->execute("var h = Hangar(); h.bays + h.bay_count();").as_int());
            }
            check_eq((int64_t)2, eng->execute("Plane(); 2;").as_int());
            check_eq((size_t)1, detail::engine_access::binding_settle_count(*eng));
        });

        test("binder_that_outlives_its_engine_destructs_quietly", [this]() {
            auto eng = make_engine();
            auto first = std::make_unique<dynamic_binder<binder_hangar>>(*eng, "Hangar");
            auto second = std::make_unique<dynamic_binder<binder_plane>>(*eng, "Plane");
            first->property("bays", &binder_hangar::bays);
            eng.reset();
            second.reset();
            first.reset();
        });
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
FOUNDRY_REGISTER(jai::foundry::tests::dynamic_binder_tests)