#pragma once

#ifndef JAISCRIPT_SERIALIZATION_POLYMORPHIC_HPP
#define JAISCRIPT_SERIALIZATION_POLYMORPHIC_HPP

#include <jaiscript/serialization/serialization_metadata.hpp>
#include <jaiscript/serialization/traits.hpp> //jai::access, named non-dependently by the load_and_construct probe below
#include <jaiscript/detail/static_type_name.hpp>

#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <vector>
#include <set>

// construct.hpp includes archive_impl.hpp (which includes this header), so only the
// declaration can appear here; the probe and factory bodies need no more until they
// instantiate, and any type declaring load_and_construct includes construct.hpp itself.
namespace jai {
namespace serialization {
template<typename T> class construct;
}
}

namespace jai {
namespace serialization {

class polymorphic_registry {
public:
    using save_fn_t = std::function<void(any_archive_writer&, const void*)>;
    // The uint32_t `id` is the shared-pointer id the object will be registered
    // under. The load_fn registers the object EAGERLY (immediately after
    // construction, before its own properties load) so that back-references
    // within the object's subtree — e.g. a child's weak_ptr to its parent, or
    // a Component's componentOwner pointing at the Node currently being loaded —
    // resolve correctly during load.
    using load_fn_t = std::function<std::shared_ptr<void>(any_archive_reader&, std::uint32_t id)>;
    // Loads the saved properties into an object that already exists (a hybrid its script constructor built)
    using load_into_fn_t = void (*)(any_archive_reader&, void* object);

    struct type_entry {
        std::string name;
        save_fn_t save_fn;
        load_fn_t load_fn;
        load_into_fn_t load_into_fn = nullptr;
        std::type_index type = std::type_index(typeid(void));
        bool implicit = false; //registered via property_owner's derived name; any explicit registration wins
    };

    static polymorphic_registry& instance();

    // One registry, two doors: explicit (jai::registrar / register_manual — chosen name,
    // always wins) and implicit (property_owner — derived class name, never overrides).
    // Static-init order between the two is unspecified, so both orders must converge:
    // implicit-after-explicit no-ops; explicit-after-implicit replaces.
    void register_entry(std::type_index ti, type_entry entry, bool a_implicit = false);

    const type_entry* find(std::type_index ti) const;

    const type_entry* find(const std::string& name) const;

    // ---- Multiple-inheritance base-pointer adjustment --------------------------------
    // A shared_ptr<Base> whose dynamic type is Derived must, on load, point at the Base
    // *subobject* — which under multiple inheritance is at a non-zero offset within
    // Derived. The save/load/back-ref paths route pointers through void (which erases the
    // offset), so we record per-(Derived->Base) adjusters and reapply them. property_owner
    // registers these automatically for its declared bases; register manually for
    // non-property_owner MI hierarchies. Single inheritance is offset zero, so the absence
    // of a recorded relation is a safe identity (the common case allocates nothing).
    template<typename Derived, typename Base>
    static void register_base_relation() {
        static_assert(std::is_base_of_v<Base, Derived>, "register_base_relation<Derived, Base>: Base must be a base class of Derived");
        if constexpr (!std::is_same_v<Derived, Base>) {
            instance().add_base_edge(
                std::type_index(typeid(Derived)),
                std::type_index(typeid(Base)),
                +[](void* p) -> void* {
                    return static_cast<void*>(static_cast<Base*>(static_cast<Derived*>(p)));
                });
        }
    }

    void add_base_edge(std::type_index derived, std::type_index base, void* (*adjust)(void*));

    // Adjust a void* pointing at a `from`-typed (complete) object to point at its `to` base
    // subobject. Identity when from==to or no relation is recorded (offset-zero / single
    // inheritance). Direct relations are a single map lookup; deeper hierarchies compose
    // adjusters via a short DFS over the recorded edges.
    void* adjust_to_base(std::type_index from, std::type_index to, void* p) const;

    // Whether a complete object of dynamic type `from` may be safely viewed as `to` — i.e.
    // `to` is `from` itself, the universal type-erased `void`, or a base reachable through the
    // recorded Derived->Base edges. The polymorphic load and back-reference paths gate their
    // void->base casts on this, so a tampered `$type` naming an unrelated (but registered) type
    // is rejected with an error instead of being reinterpreted as the requested base — the
    // serialization type-confusion fix (B5). property_owner records every declared base
    // (including offset-zero single inheritance), so its hierarchies validate exactly; a type
    // with no recorded relation is assignable only to itself.
    bool is_assignable(std::type_index from, std::type_index to) const;

    template<typename T>
    static void try_auto_register(const std::string& name) {
        if constexpr (std::is_polymorphic_v<T>) {
            type_entry entry;
            entry.name = name;
            entry.save_fn = make_save_fn<T>();
            entry.load_fn = make_load_fn<T>();
            entry.load_into_fn = make_load_into_fn<T>();
            instance().register_entry(std::type_index(typeid(T)), std::move(entry));
        }
    }

    // Implicit registration (property_owner): same entry machinery, weak precedence.
    // The name is `static constexpr const char* jai_type_name` if the class declares one,
    // else the bare class identifier (parsed from the compiler signature — never a mangled
    // typeid name, identical across MSVC/GCC/Clang; anonymous-namespace types fall out as
    // their bare name too). Template instantiations have no stable bare spelling, so
    // without jai_type_name they are skipped at compile time — pin a name in-class or
    // register externally (jai::registrar / try_auto_register, no type modification).
    template<typename T>
    static void try_auto_register_implicit() {
        if constexpr (std::is_polymorphic_v<T>) {
            // The shared naming ladder (detail/static_type_name.hpp): identical name here,
            // in jai::registrar auto-naming, and in dynamic_binder base auto-registration.
            constexpr auto ladderName = ::jai::detail::declared_or_derived_type_name<T>();
            if constexpr (!ladderName.empty()) {
                register_implicit_entry<T>(std::string(ladderName));
            }
        }
    }

    // Manual registration for types that don't use the property system or
    // jai::registrar. Provide custom save/load lambdas that receive a
    // concrete archive reference (via dispatch).
    //
    // SaveFn: void(auto& archive, const T&)
    // LoadFn: std::shared_ptr<T>(auto& archive)
    //
    // Example:
    //   polymorphic_registry::register_manual<MyType>("MyType",
    //       [](auto& ar, const MyType& obj) { ar.serialize("x", obj.x); },
    //       [](auto& ar) {
    //           auto ptr = std::make_shared<MyType>();
    //           ar.serialize("x", ptr->x);
    //           return ptr;
    //       }
    //   );
    template<typename T, typename SaveFn, typename LoadFn>
    static void register_manual(const std::string& name, SaveFn&& save, LoadFn&& load) {
        type_entry entry;
        entry.name = name;
        entry.save_fn = [s = std::forward<SaveFn>(save)](any_archive_writer& ar, const void* ptr) {
            auto& obj = *static_cast<const T*>(ptr);
            ar.dispatch([&](auto& concrete_ar) { s(concrete_ar, obj); });
        };
        entry.load_fn = [l = std::forward<LoadFn>(load)](any_archive_reader& ar, std::uint32_t id) -> std::shared_ptr<void> {
            return ar.dispatch([&l, id](auto& concrete_ar) -> std::shared_ptr<void> {
                // Manual load lambdas own construction, so eager registration is
                // best-effort: register as soon as the object exists so that
                // anything loaded afterward can reference it. (A manual type that
                // needs to resolve references to *itself* mid-construction must
                // register the shared_ptr itself.)
                auto ptr = l(concrete_ar);
                if (ptr) {
                    concrete_ar.register_deserialized_shared(id, ptr);
                }
                return std::static_pointer_cast<void>(ptr);
            });
        };
        instance().register_entry(std::type_index(typeid(T)), std::move(entry));
    }

private:
    polymorphic_registry() = default;

    template<typename T>
    static void register_implicit_entry(std::string name) {
        type_entry entry;
        entry.name = std::move(name);
        entry.save_fn = make_save_fn<T>();
        entry.load_fn = make_load_fn<T>();
        entry.load_into_fn = make_load_into_fn<T>();
        instance().register_entry(std::type_index(typeid(T)), std::move(entry), true);
    }

    // Uses archive operator() via dispatch — handles save methods, property_mgr,
    // and access-controlled methods without requiring friendship

    template<typename T>
    static save_fn_t make_save_fn() {
        return [](any_archive_writer& ar, const void* ptr) {
            auto& obj = *static_cast<const T*>(ptr);
            ar.dispatch([&](auto& concrete_ar) {
                concrete_ar(obj);
            });
        };
    }

    struct detection_archive {
        static constexpr bool is_jai_archive = true;
        static constexpr bool is_text_format = false;
        template<typename... Args> void operator()(Args&&...) {}
        template<typename... Args> void serialize(Args&&...) {}
        template<typename U> U* get_user_context() const { return nullptr; }
    };

    template<typename T, typename = void>
    struct has_jai_load_and_construct : std::false_type {};
    template<typename T>
    struct has_jai_load_and_construct<T, std::void_t<decltype(
        ::jai::access::load_and_construct(std::declval<detection_archive&>(), std::declval<construct<T>&>())
    )>> : std::true_type {};

    // A type that loads its properties into an existing object: a load or serialize member (through jai::access) or free
    // function; one that only load_and_construct reads cannot
    template<typename T, typename = void> struct has_member_load_in_place : std::false_type {};
    template<typename T> struct has_member_load_in_place<T, std::void_t<decltype(
        ::jai::access::member_load(std::declval<detection_archive&>(), std::declval<T&>(), std::uint32_t(0)))>> : std::true_type {};
    template<typename T, typename = void> struct has_simple_member_load_in_place : std::false_type {};
    template<typename T> struct has_simple_member_load_in_place<T, std::void_t<decltype(
        ::jai::access::member_load(std::declval<detection_archive&>(), std::declval<T&>()))>> : std::true_type {};
    template<typename T, typename = void> struct has_member_serialize_in_place : std::false_type {};
    template<typename T> struct has_member_serialize_in_place<T, std::void_t<decltype(
        ::jai::access::member_serialize(std::declval<detection_archive&>(), std::declval<T&>(), std::uint32_t(0)))>> : std::true_type {};
    template<typename T, typename = void> struct has_simple_member_serialize_in_place : std::false_type {};
    template<typename T> struct has_simple_member_serialize_in_place<T, std::void_t<decltype(
        ::jai::access::member_serialize(std::declval<detection_archive&>(), std::declval<T&>()))>> : std::true_type {};
    template<typename T, typename = void> struct has_free_load_in_place : std::false_type {};
    template<typename T> struct has_free_load_in_place<T, std::void_t<decltype(
        load(std::declval<detection_archive&>(), std::declval<T&>()))>> : std::true_type {};
    template<typename T, typename = void> struct has_free_serialize_in_place : std::false_type {};
    template<typename T> struct has_free_serialize_in_place<T, std::void_t<decltype(
        serialize(std::declval<detection_archive&>(), std::declval<T&>()))>> : std::true_type {};
    template<typename T>
    static constexpr bool loads_in_place = has_member_load_in_place<T>::value || has_simple_member_load_in_place<T>::value ||
        has_member_serialize_in_place<T>::value || has_simple_member_serialize_in_place<T>::value ||
        has_free_load_in_place<T>::value || has_free_serialize_in_place<T>::value;

    template<typename T>
    static load_into_fn_t make_load_into_fn() {
        if constexpr (loads_in_place<T>) {
            return [](any_archive_reader& ar, void* object) {
                ar.dispatch([object](auto& concrete_ar) { concrete_ar(*static_cast<T*>(object)); });
            };
        } else {
            return nullptr;
        }
    }

    template<typename T>
    static load_fn_t make_load_fn() {
        if constexpr (has_jai_load_and_construct<T>::value) {
            return [](any_archive_reader& ar, std::uint32_t id) -> std::shared_ptr<void> {
                return ar.dispatch([id](auto& concrete_ar) -> std::shared_ptr<void> {
                    std::shared_ptr<T> ptr;
                    construct<T> c(ptr);
                    // Register EAGERLY the moment the object is constructed, before
                    // load_and_construct loads its properties. This mirrors the
                    // non-polymorphic shared_ptr load path (archive_impl.hpp), which
                    // sets the same on_construct hook, and lets back-references
                    // inside this object's subtree resolve during load.
                    c.set_on_construct([&ptr, id, &concrete_ar]() {
                        concrete_ar.register_deserialized_shared(id, ptr);
                    });
                    ::jai::access::load_and_construct(concrete_ar, c);
                    return std::static_pointer_cast<void>(ptr);
                });
            };
        } else if constexpr (::jai::serialization::load_default_constructible<T>) {
            return [](any_archive_reader& ar, std::uint32_t id) -> std::shared_ptr<void> {
                return ar.dispatch([id](auto& concrete_ar) -> std::shared_ptr<void> {
                    auto ptr = ::jai::serialization::make_default_for_load<T>();
                    // Register before loading properties so subtree back-references
                    // (weak_ptr / shared_ptr pointing back into this object) resolve.
                    concrete_ar.register_deserialized_shared(id, ptr);
                    concrete_ar(*ptr);
                    return std::static_pointer_cast<void>(ptr);
                });
            };
        } else {
            return nullptr;
        }
    }

    struct base_edge {
        std::type_index base;
        void* (*adjust)(void*); // Derived* -> Base* (stateless, so a plain function pointer)
    };

    // dynamic_binder::base_class<>() and runtime registrars write here from whichever thread builds an engine
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::type_index, type_entry> by_rtti_;
    std::unordered_map<std::string, const type_entry*> by_name_;
    // Direct Derived->Base adjusters; adjust_to_base() composes them transitively
    std::unordered_map<std::type_index, std::vector<base_edge>> base_edges_;
};

// Free-function entry points named ahead of this header by jai::registrar and
// jai::property_owner (forward-declared there), so their class-template bodies can call
// them before this header is included.
template<typename T>
void try_auto_register(const std::string& name) {
    polymorphic_registry::try_auto_register<T>(name);
}

template<typename T>
void try_auto_register_implicit() {
    polymorphic_registry::try_auto_register_implicit<T>();
}

// The one way to declare a polymorphic Derived->Base serialization relation. property_owner and
// dynamic_binder::base_class<>() construct it internally; friend-access / third-party types
// declare it at namespace scope: static jai::serialization::register_relation<Derived, Base...> x;
template<typename Derived, typename... Bases>
struct register_relation {
    register_relation() { (polymorphic_registry::register_base_relation<Derived, Bases>(), ...); }
};

} // namespace serialization
} // namespace jai

// NOTE: make_save_fn/make_load_fn instantiate any_archive dispatch(), defined in
// archive_dispatch.hpp. This header must stay light (archive_impl.hpp includes it before
// its own classes), so the registration ENTRY POINTS (registrar.hpp, property_manager.hpp)
// include archive_dispatch.hpp at their bottoms instead — otherwise a TU registering a
// type without the concrete archives fails with C3779 (decltype(auto) used before defined).

#endif // JAISCRIPT_SERIALIZATION_POLYMORPHIC_HPP
