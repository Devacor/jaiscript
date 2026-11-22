#include <jaiscript/stdlib/containers.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/container_enforce.hpp>
#include <jaiscript/detail/map_entry_binding.hpp>

namespace jai {
namespace stdlib {

    // The library's pair: a map entry as a range-for's `auto kv` sees it (detail/map_entry_binding.hpp). A copy holds copies of
    // both halves, so the copy of a by-reference entry is a snapshot of its value.
    struct script_pair {
        script_value first;
        script_value second;

        explicit script_pair(engine* eng)
            : first(std::monostate{}, eng),
              second(std::monostate{}, eng) {}

        script_pair(const script_value& f, const script_value& s)
            : first(f), second(s) {}

        // a by-reference entry's second half is the reference itself
        script_pair(const script_value& f, script_value&& s)
            : first(f), second(std::move(s)) {}

        script_pair(const script_pair& other)
            : first(clone_for_assignment(other.first)),
              second(clone_for_assignment(other.second)) {}

        script_pair& operator=(const script_pair& other) {
            if (this != &other) {
                script_value f = clone_for_assignment(other.first);
                script_value s = clone_for_assignment(other.second);
                second = std::move(s);
                first = std::move(f);
            }
            return *this;
        }
    };

    void register_container_types(engine& eng) {
        dynamic_binder<script_pair>(eng, "pair")
            .constructor<>()  // Will automatically use engine-aware constructor
            .constructor<script_value, script_value>()
            .property("first", &script_pair::first)
            .property("second", &script_pair::second)
            .build();
    }

} // namespace stdlib
} // namespace jai

namespace jai::detail {

namespace {
// A pair a parallel chunk mints or re-aims is the chunk's own: a store through it lands in a map the chunk owns
void tag_region_pair(class_instance& a_instance, engine* a_engine) {
    if (a_engine && engine_access::parallel_region_active(*a_engine)) [[unlikely]] { a_instance.region_tag(engine_access::parallel_region_serial(*a_engine)); }
}
}

bool reuse_map_entry_pair(script_value* cell, const class_definition* pair_class, script_value& first, script_value& second) {
    if (!cell || cell->raw_storage_index() != script_value::TYPEID_OBJECT) { return false; }
    const auto& holder = cell->unchecked_object_handle();
    if (!holder || !holder.unique() || !holder->is_instance() || holder->object_use_count() != 1) { return false; }
    auto* instance = holder->unchecked_instance();
    if (instance->get_class_definition() != pair_class || instance->live_field_refs() != 0) { return false; }
    const script_value* native = instance->native();
    if (!native || native->raw_storage_index() != script_value::TYPEID_OBJECT) { return false; }
    const auto& native_holder = native->unchecked_object_handle();
    if (!native_holder || !native_holder.unique() || !native_holder->holds_object() || native_holder->object_use_count() != 1) { return false; }
    auto* pair = static_cast<stdlib::script_pair*>(native_holder->object());
    pair->second = std::move(second);
    pair->first = std::move(first);
    tag_region_pair(*instance, pair_class->get_engine());
    return true;
}

script_value mint_map_entry_pair(const std::shared_ptr<class_definition>& pair_class, script_value&& first, script_value&& second, engine* eng) {
    auto native = std::make_shared<stdlib::script_pair>(first, std::move(second));
    auto instance = pair_class->create_instance();
    instance->set_native(script_value::make_cpp_object(pair_class->get_name(), pair_class->get_type_id(), native, eng));
    tag_region_pair(*instance, eng);
    return script_value::make_object(pair_class->get_name(), instance, eng);
}

std::shared_ptr<class_definition> map_entry_pair_class(engine& eng) {
    return engine_access::get_class_definition_by_type(eng, std::type_index(typeid(stdlib::script_pair)));
}

} // namespace jai::detail
