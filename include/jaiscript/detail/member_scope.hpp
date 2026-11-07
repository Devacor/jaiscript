#pragma once

// Member-shadowing kernel: C++ unqualified-lookup order for bare identifiers inside
// methods - locals and enclosing function scopes first, then the class's members,
// then the definition/global environments. Shared VERBATIM by the interpreter and
// the vm (ref_lvalue.hpp discipline) so the two backends cannot drift.
//
// Each backend stamps a per-frame member scope onto call_frame (scope_state/
// scope_floor/scope_this/scope_static): the interpreter at frame push, where the
// call branch knows the callee kind; the vm lazily from entry_env/locals. A site
// resolving a bare identifier then runs:
//
//   1. slots / catch var (unchanged)
//   2. own_scope_lookup: the env chain from environment_ up to (exclusive)
//      scope_floor, OWN storage only - block/capture cells keep beating members
//   3. probe_member_scope: fields -> C++ properties -> methods -> statics (instance
//      scopes), static fields -> static methods (static scopes)
//   4. the walk from scope_floor onward (definition env -> global), caches intact
//
// A hybrid's (script class : bound C++ class) C++ property is a member like any other:
// the bare name reads through its _get_ and stores through its _set_ exactly as
// this.name does; a same-named script field shadows it for every spelling.
//
// Frames with scope_state none keep their original ladder byte-for-byte.

#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/environment.hpp>

namespace jai::detail {

enum class member_scope_state : uint8_t {
    uncomputed = 0,
    none,      // not a member scope - sites keep their original ladder
    instance,  // instance method / method coroutine: probe scope_this's members
    statics,   // static method: probe scope_static's statics
};

// The interpreter's current-frame member scope (the vm carries the same fields on
// call_frame): saved/restored by call_function around every call and stamped by the
// branch that knows the callee kind, plus the env-swap-without-frame sites (field
// initializers). state none = original resolution ladder, byte-for-byte.
struct member_scope_ctx {
    member_scope_state state = member_scope_state::none;
    environment* floor = nullptr;
    script_value* this_storage = nullptr;
    class_definition* statics = nullptr;   // static scope: its class; instance scope: the declaring class (null = the receiver's)

    bool active() const {
        return state == member_scope_state::instance || state == member_scope_state::statics;
    }
};

struct member_probe_hit {
    enum class kind : uint8_t { miss, field, static_field, method, cpp_property };
    kind what = kind::miss;
    script_value* value_ptr = nullptr;   // field/static_field: stable storage
    // method: the stored (unbound) value; cpp_property (want_methods): the chain's _get_
    // accessor (null-engine monostate until a hit fills it)
    script_value method_value{std::monostate{}, static_cast<engine*>(nullptr)};
    uint64_t getter_id = 0;              // cpp_property: the _get_ id
    class_instance* instance = nullptr;  // field/cpp_property: owning instance (write kernels)
    class_definition* class_def = nullptr;

    // A hit with no cell answers the pointer form as nullptr (a shadow, never a global)
    explicit operator bool() const { return what != kind::miss; }
};

// A hybrid's C++ property rung: sym is a dynamic_binder .property() of the chain's C++
// base and NOT a script field of the chain (a same-named script field shadows it for
// every spelling) - class_definition::slot_is_cpp_property. The _get_ / _set_ id, 0
// otherwise (a getter-only property's _set_ is the binder's read-only rejection).
inline uint64_t cpp_property_getter_id(const class_instance* instance, uint64_t sym) {
    class_definition* class_def = instance->get_class_definition();
    if (!class_def) return 0;
    const uint32_t slot = class_def->field_slot(sym);
    if (slot == class_definition::k_no_field_slot || !class_def->slot_is_cpp_property(slot)) return 0;
    auto cpp_base = class_def->find_cpp_base_class();
    return cpp_base ? cpp_base->get_property_getter_id(sym) : 0;
}

inline uint64_t cpp_property_setter_id(const class_instance* instance, uint64_t sym) {
    class_definition* class_def = instance->get_class_definition();
    if (!class_def) return 0;
    const uint32_t slot = class_def->field_slot(sym);
    if (slot == class_definition::k_no_field_slot || !class_def->slot_is_cpp_property(slot)) return 0;
    auto cpp_base = class_def->find_cpp_base_class();
    return cpp_base ? cpp_base->get_property_setter_id(sym) : 0;
}

// The C++ property READ every spelling shares (this.name, the bare name, a [=]
// snapshot): the _get_ accessor called on the receiver; a bound getter on a retired
// script layer reports; a non-owning result is pinned to the receiver's data (the same
// anchor chained method returns get). Both member ladders funnel here so the texts
// cannot drift.
inline checked_result<script_value> read_cpp_property(const script_value& receiver,
                                                      class_definition* class_def,
                                                      class_instance* instance,
                                                      const script_value& getter,
                                                      uint64_t getter_id) {
    const script_function& func = getter.as_function();
    if (instance && instance->script_layer_retired() &&
        !func.target<script_method_dispatch>() && !class_def->is_auto_accessor(getter_id)) {
        return checked_result<script_value>(make_error_code(runtime_error_code::script_layer_retired),
            "Script layer '{0}' was detached (script_layer_detach, or a move to another engine): this handle no longer reaches its object",
            class_def->get_type_id());
    }
    std::vector<script_value> args = {receiver};
    auto result = func(args);
    if (!result) {
        return result;
    }
    script_value& value = result.value();
    if (value.is_non_owning_object()) {
        auto result_holder = value.get_object_holder();
        auto receiver_holder = receiver.get_object_holder();
        if (result_holder && receiver_holder) {
            result_holder->set_keep_alive(receiver_holder->holds_object() ? receiver_holder->held()
                                                              : receiver_holder->keep_alive());
        }
    }
    return result;
}

// The bare-name read (kernel rung cpp_property, non-scope this-tails, [=] captures):
// read_cpp_property through the chain's getter (the probe's method_value, or
// instance->get_method(getter_id, false)), degrading on a throw to the placeholder cell
// exactly as the this.name ladder does; a host getter's own exception arrives as its
// runtime error and raises.
inline checked_result<script_value> read_bare_cpp_property(const script_value& receiver,
                                                           class_instance* instance,
                                                           const script_value& getter,
                                                           uint64_t getter_id,
                                                           uint64_t sym) {
    try {
        return read_cpp_property(receiver, instance->get_class_definition(), instance, getter, getter_id);
    } catch (const std::exception&) {
        if (instance->script_layer_retired()) { throw; }
    }
    return instance->get_field(sym);
}

// The C++ property STORE the bare landings share with this.name = v: the chain's _set_
// (a script override wins, like the getter's) called with receiver + value. setter_id
// from cpp_property_setter_id (non-zero).
inline checked_result<void> write_cpp_property(const script_value& receiver,
                                               class_definition* class_def,
                                               uint64_t setter_id,
                                               const script_value& value) {
    script_value setter = class_def->get_method(setter_id, false);
    if (setter.is_invalid() || !setter.is_function()) {
        return checked_result<void>(make_error_code(runtime_error_code::member_not_found),
            "Method '{0}' not found", setter_id);
    }
    std::vector<script_value> args = {receiver, value};
    auto result = setter.as_function()(args);
    if (!result) {
        return result.error_value();
    }
    return {};
}

// The binder .property() `sym` names on a receiver of ANY kind - a hybrid layer (a
// same-named script field shadows), a plain wrapper of a bound C++ class, or a raw
// non-owning handle - with the class its accessors resolve on. getter_id 0 = not a C++
// property of this receiver. One resolver for the reference mint and every per-touch
// re-resolution.
struct cpp_property_site {
    slot_class_view view;
    uint64_t getter_id = 0;
    uint64_t setter_id = 0;
    class_definition* accessor_class = nullptr;   // the binder class holding the member accessors
    explicit operator bool() const { return getter_id != 0; }
};

inline cpp_property_site resolve_cpp_property(const script_value& receiver, uint64_t sym, engine* eng) {
    cpp_property_site site;
    site.view = resolve_slot_class(receiver, eng);
    class_definition* def = site.view.def;
    if (!def) return site;
    if (site.view.instance && !def->is_cpp_class()) {
        site.getter_id = cpp_property_getter_id(site.view.instance.get(), sym);
        if (!site.getter_id) return site;
        site.setter_id = cpp_property_setter_id(site.view.instance.get(), sym);
        site.accessor_class = def->find_cpp_base_class().get();
    } else if (def->is_cpp_class()) {
        site.getter_id = def->get_property_getter_id(sym);
        if (!site.getter_id) return site;
        site.setter_id = def->get_property_setter_id(sym);
        site.accessor_class = def;
    }
    return site;
}

// A script _get_/_set_ override anywhere in the chain makes the property computed from
// the outside: a member binding would bypass it
inline bool cpp_property_has_script_override(const cpp_property_site& site) {
    class_definition* def = site.view.def;
    if (!def || def->is_cpp_class()) return false;
    auto is_script = [&](uint64_t id) {
        if (!id) return false;
        script_value method = def->get_method(id, false);
        return !method.is_invalid() && method.is_function() &&
               method.as_function().target<script_method_dispatch>() != nullptr;
    };
    return is_script(site.getter_id) || is_script(site.setter_id);
}

// The property READ a reference touch or a bind-time chain step performs. A bind point
// sits mid-call-sequence where re-entering script is illegal: a script _get_ override
// (and a throwing getter) answer invalid_reference there; touches propagate throws.
inline checked_result<script_value> read_cpp_property_site(const script_value& receiver,
                                                           const cpp_property_site& site,
                                                           bool bind_point) {
    script_value getter = site.view.def->get_method(site.getter_id, false);
    if (getter.is_invalid() || !getter.is_function()) {
        return checked_result<script_value>(make_error_code(runtime_error_code::member_not_found),
            "Method '{0}' not found", site.getter_id);
    }
    if (bind_point && getter.as_function().target<script_method_dispatch>()) {
        return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference),
            "Cannot pass non-lvalue to reference parameter");
    }
    try {
        auto read = read_cpp_property(receiver, site.view.def, site.view.instance.get(), getter, site.getter_id);
        if (!read && bind_point && read.error() == make_error_code(runtime_error_code::cpp_exception)) {
            return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference),
                "Cannot pass non-lvalue to reference parameter");   // a getter that cannot read at a bind point is a non-lvalue
        }
        return read;
    } catch (const std::exception&) {
        if (!bind_point) throw;
        return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference),
            "Cannot pass non-lvalue to reference parameter");
    }
}

// The C++ object behind a receiver, pinned: the instance's native object (an extended
// object's weak handle locked), a raw holder's data, else the anchor a non-owning handle
// carries. Null = the reference is exactly as safe as the handle it was bound on.
inline object_ref pin_cpp_object(const script_value& receiver, const class_instance* instance) {
    auto holder = receiver.get_object_holder();
    if (instance) {
        const script_value* held = instance->native();
        holder = held ? held->get_object_holder() : nullptr;
    }
    if (!holder) return nullptr;
    if (holder->holds_object()) return holder->held();
    if (auto locked = holder->lock_weak_data()) return object_ref(std::move(locked));
    return holder->keep_alive();
}

// The class-scope boundary in a lexical chain: the first method/static env.
inline environment* find_member_boundary(environment* from) {
    for (environment* e = from; e; e = e->parent_raw()) {
        if (e->is_method_env() || e->is_static_method_env()) return e;
    }
    return nullptr;
}

inline environment* chain_root(environment* from) {
    environment* e = from;
    while (e && e->parent_raw()) e = e->parent_raw();
    return e;
}

// Phase 2 of the ladder: the function's own scopes - [current .. floor) OWN storage
// only. A null floor (member probe belongs before the whole walk) yields no hits.
inline script_value* own_scope_lookup(environment* current, const environment* floor, uint64_t id) {
    for (environment* e = current; e && e != floor; e = e->parent_raw()) {
        if (script_value* own = e->get_own_value_ptr(id)) return own;
    }
    return nullptr;
}

// A bare static name inside a class's code: the declaring class's chain (C++ lookup, whatever
// the receiver's dynamic class), then the receiver's own chain (its subclass statics stay visible)
inline bool probe_static_chain(member_probe_hit& hit, class_definition* origin, uint64_t sym, bool want_methods) {
    class_definition* owner = nullptr;
    if (script_value* static_ptr = origin->find_static_field_in_chain(sym, &owner)) {
        hit.what = member_probe_hit::kind::static_field;
        hit.value_ptr = static_ptr;
        hit.class_def = owner;
        return true;
    }
    if (want_methods) {
        if (const script_value* method = origin->find_static_method_in_chain(sym, &owner)) {
            hit.what = member_probe_hit::kind::method;
            hit.method_value = *method;
            hit.class_def = owner;
            return true;
        }
    }
    return false;
}

inline void probe_scope_statics(member_probe_hit& hit, class_definition* declaring, class_definition* dynamic,
                                uint64_t sym, bool want_methods) {
    if (declaring && probe_static_chain(hit, declaring, sym, want_methods)) { return; }
    if (dynamic && dynamic != declaring) { probe_static_chain(hit, dynamic, sym, want_methods); }
}

// The class a bare static store lands on (it converts through that class's declared type)
inline class_definition* bare_static_owner(class_definition* declaring, class_definition* dynamic, uint64_t sym) {
    class_definition* owner = nullptr;
    if (declaring && declaring->find_static_field_in_chain(sym, &owner)) return owner;
    if (dynamic && dynamic != declaring && dynamic->find_static_field_in_chain(sym, &owner)) return owner;
    return nullptr;
}

inline bool scope_statics_have_method(class_definition* declaring, class_definition* dynamic, uint64_t sym) {
    if (declaring && declaring->find_static_method_in_chain(sym)) { return true; }
    return dynamic && dynamic != declaring && dynamic->find_static_method_in_chain(sym);
}

// Phase 3: the boundary probe. Field -> method -> static (the same surface and
// relative order the old post-walk fallbacks answered). want_methods=false lets
// pointer-only resolvers (fast binary operands, store targets) skip the mint rung;
// their callers bail to a full-probing path when the name could be a method.
inline member_probe_hit probe_member_scope(member_scope_state state,
                                           script_value* scope_this,
                                           class_definition* scope_static,
                                           uint64_t sym,
                                           bool want_methods) {
    member_probe_hit hit;
    if (state == member_scope_state::instance && scope_this) {
        script_value& this_val = *scope_this;
        if (this_val.is_null()) return hit;
        const auto this_type = this_val.type();
        if (this_type != script_value_type::jai_object_type &&
            this_type != script_value_type::jai_shared_ptr_type) return hit;
        class_instance* instance = this_val.peek_class_instance();   // the frame holds the receiver
        if (!instance) return hit;
        class_definition* class_def = instance->get_class_definition();
        const uint32_t slot = class_def ? class_def->field_slot(sym) : class_definition::k_no_field_slot;
        if (slot != class_definition::k_no_field_slot) {
            // A binder property's placeholder slot answers as cpp_property, never as its
            // (null) cell - the C++ object holds the truth, reached through _get_/_set_
            if (class_def->slot_is_cpp_property(slot)) [[unlikely]] {
                if (auto cpp_base = class_def->find_cpp_base_class()) {
                    if (uint64_t getter_id = cpp_base->get_property_getter_id(sym)) {
                        script_value getter = want_methods ? class_def->get_method(getter_id, false)
                                                           : script_value::make_invalid(nullptr);
                        if (!want_methods || !getter.is_invalid()) {
                            hit.what = member_probe_hit::kind::cpp_property;
                            hit.getter_id = getter_id;
                            hit.method_value = std::move(getter);
                            hit.instance = instance;
                            hit.class_def = class_def;
                            return hit;
                        }
                    }
                }
            }
            script_value* cell = instance->field_slot_value(slot);
            if (!cell) {
                script_value& field_ref = instance->get_field(sym, false);   // declared, default not yet materialized
                if (!field_ref.is_invalid()) cell = &field_ref;
            }
            if (cell) {
                hit.what = member_probe_hit::kind::field;
                hit.value_ptr = cell;
                hit.instance = instance;
                hit.class_def = class_def;
                return hit;
            }
        } else if (script_value* present = instance->find_field_value(sym)) {   // runtime-added field
            hit.what = member_probe_hit::kind::field;
            hit.value_ptr = present;
            hit.instance = instance;
            hit.class_def = class_def;
            return hit;
        }
        if (want_methods) {
            script_value method = instance->get_method(sym, false);
            if (!method.is_invalid()) {
                hit.what = member_probe_hit::kind::method;
                hit.method_value = std::move(method);
                hit.instance = instance;
                hit.class_def = instance->get_class_definition();
                return hit;
            }
        }
        probe_scope_statics(hit, scope_static, instance->get_class_definition(), sym, want_methods);
        return hit;
    }
    if (state == member_scope_state::statics && scope_static) {
        probe_scope_statics(hit, scope_static, nullptr, sym, want_methods);
    }
    return hit;
}

// Name lookup finds a bare member name first, then access applies (enforce_member_access: a private member is reachable
// from its declaring class and every class derived from it). ctx = the class whose method body runs. KEEP the one check
// for every bare-name member site of both backends (loads, stores, reference binding, captures).
inline checked_result<void> bare_member_access(const member_probe_hit& hit, uint64_t sym, const class_definition* ctx) {
    if (!hit.class_def || !hit.class_def->chain_has_nonpublic()) [[likely]] { return {}; }
    return enforce_member_access(hit.class_def, sym, ctx);
}

// A name that could be a member METHOD under this scope (fields/statics answer via
// pointer rungs). Pointer-only resolvers use this to bail to their minting path.
inline bool member_scope_has_method(member_scope_state state,
                                    script_value* scope_this,
                                    class_definition* scope_static,
                                    uint64_t sym) {
    if (state == member_scope_state::instance && scope_this) {
        const auto this_type = scope_this->type();
        if (scope_this->is_null() ||
            (this_type != script_value_type::jai_object_type &&
             this_type != script_value_type::jai_shared_ptr_type)) return false;
        class_instance* instance = scope_this->peek_class_instance();
        if (!instance) return false;
        script_value method = instance->get_method(sym, false);
        if (!method.is_invalid()) return true;
        return scope_statics_have_method(scope_static, instance->get_class_definition(), sym);
    }
    if (state == member_scope_state::statics && scope_static) {
        return scope_statics_have_method(scope_static, nullptr, sym);
    }
    return false;
}

// The class whose code a lexical chain runs (its method/static/initializer env's access context)
inline class_definition* lexical_access_class(environment* from) {
    for (environment* e = from; e; e = e->parent_raw()) {
        if (class_definition* cls = e->get_access_context()) return cls;
    }
    return nullptr;
}

// Lazy scope compute from frame ingredients (the vm path; the interpreter stamps at
// push). entry_env carries the lexical chain: null = lexically top-level UNLESS the
// frame itself is a method frame (lazy method bodies run directly on the definition
// env - current_env at compute time IS the floor, lazy bodies never move it).
inline void compute_member_scope(call_frame& locals,
                                 environment* entry_env,
                                 environment* current_env) {
    locals.scope_state = static_cast<uint8_t>(member_scope_state::none);
    if (locals.is_method && locals.this_object_ptr) {
        locals.scope_this = locals.this_object_ptr.get();
        locals.scope_state = static_cast<uint8_t>(member_scope_state::instance);
        if (!locals.scope_static) { locals.scope_static = lexical_access_class(entry_env); }   // a lazy push stamps it
        if (!entry_env) {
            locals.scope_floor = current_env;                       // lazy method body
        } else if (entry_env->is_method_env()) {
            locals.scope_floor = entry_env->parent_raw();           // env-backed method
        } else if (environment* boundary = find_member_boundary(entry_env)) {
            locals.scope_floor = (boundary == entry_env) ? boundary->parent_raw() : boundary;
        } else {
            locals.scope_floor = chain_root(current_env);           // receiver-pinned fiber:
        }                                                           // members beat globals only
        return;
    }
    if (locals.is_static_method && locals.static_class_def) {
        locals.scope_static = locals.static_class_def.get();
        locals.scope_state = static_cast<uint8_t>(member_scope_state::statics);
        locals.scope_floor = entry_env
            ? (entry_env->is_static_method_env() ? entry_env->parent_raw()
                                                 : entry_env)
            : current_env;
        return;
    }
    if (!entry_env) return;                                         // plain function
    environment* boundary = find_member_boundary(entry_env);
    if (!boundary) return;                                          // no class scope in chain
    environment* floor = (boundary == entry_env) ? boundary->parent_raw() : boundary;
    if (boundary->is_method_env()) {
        if (script_value* this_storage = boundary->method_this_storage()) {
            locals.scope_this = this_storage;
            locals.scope_static = boundary->get_access_context();
            locals.scope_floor = floor;
            locals.scope_state = static_cast<uint8_t>(member_scope_state::instance);
        }
        return;
    }
    if (auto class_def = boundary->get_class_definition()) {
        locals.scope_static = class_def.get();
        locals.scope_floor = floor;
        locals.scope_state = static_cast<uint8_t>(member_scope_state::statics);
    }
}

} // namespace jai::detail
