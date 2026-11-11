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

    // Merge two maps, with the second map's values overriding the first's on key collision
	inline checked_result<script_value> map_merge(engine* engine, const script_value& a_left, const script_value& a_right) {
		if (!a_left.is_map() || !a_right.is_map()) {
			return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "merge() requires two map arguments");
		}
		const auto tag = a_left.get_type_info();
		const auto keyType = tag ? tag->key_type() : nullptr, valueType = tag ? tag->value_type() : nullptr;
		script_value result = script_value::make_map(keyType, valueType, engine);
		using Entry = std::pair<script_value, script_value>;
		std::vector<Entry> entries;
		entries.reserve(a_left.unchecked_as_map().size() + a_right.unchecked_as_map().size());
		// Decide the output before copying: an overwritten input value is only read.
		for (const auto& [key,value] : a_left.unchecked_as_map()) {
			if (a_right.unchecked_as_map().find(key) == a_right.unchecked_as_map().end()) { entries.emplace_back(key,value); }
		}
		for (const auto& [key,value] : a_right.unchecked_as_map()) { entries.emplace_back(key,value); }
		for (const auto& [key,value] : entries) {
			if (keyType && keyType->base_type != script_value_type::jai_any_type && keyType->base_type != script_value_type::jai_null_type &&
			    !detail::map_key_kind_admits(key.type(), detail::map_key_kind(*keyType))) {
				return checked_result<script_value>(make_error_code(runtime_error_code::map_key_type_mismatch), "Cannot use '{0}' as a key of type '{1}'", detail::engine_access::symbolize(*engine, detail::container_value_type_name(key)), detail::engine_access::symbolize(*engine, detail::type_info_display_name(keyType)));
			}
			script_value output = value.clone_element();
			if (valueType && valueType->base_type != script_value_type::jai_any_type && valueType->base_type != script_value_type::jai_null_type) {
				if (!detail::container_element_compatible(output, valueType, engine)) {
					return checked_result<script_value>(make_error_code(runtime_error_code::map_value_type_mismatch), "Cannot store '{0}' in map value of type '{1}'", detail::engine_access::symbolize(*engine, detail::container_value_type_name(output)), detail::engine_access::symbolize(*engine, detail::type_info_display_name(valueType)));
				}
				auto converted = detail::convert_container_element(engine, output, valueType);
				if (!converted) { return converted.error_value(); }
				output = std::move(converted.value());
			}
			if (keyType && keyType->base_type != script_value_type::jai_any_type && keyType->base_type != script_value_type::jai_null_type &&
			    key.type() != detail::map_key_kind(*keyType)) {
				auto converted_key = detail::map_key_in_kind(key, detail::map_key_kind(*keyType), engine);
				if (!converted_key) { return converted_key.error_value(); }
				(*result.unchecked_get_map_storage())[std::move(converted_key.value())] = std::move(output);
				continue;
			}
			(*result.unchecked_get_map_storage())[key.clone_element()] = std::move(output);
		}
		return result;
	}

    void register_container_types(engine& eng) {
        dynamic_binder<script_pair>(eng, "pair")
            .constructor<>()  // Will automatically use engine-aware constructor
            .constructor<script_value, script_value>()
            .property("first", &script_pair::first)
            .property("second", &script_pair::second)
            .build();

        // Register container utility functions using variadic approach
        eng.add_variadic_function("merge", [owner = &eng](const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 2) {
                // {0} is a symbol: the count rides the interned spelling of the number
                engine* eng = args.empty() ? nullptr : owner;
                const auto ec = make_error_code(runtime_error_code::argument_count_mismatch);
                if (!eng) { return checked_result<script_value>(ec, "merge() expects exactly 2 arguments, got 0"); }
                auto* symbols = detail::engine_access::get_symbolizer(*eng);
                if (symbols->frozen()) { return checked_result<script_value>(ec, "merge() expects exactly 2 arguments"); }
                return checked_result<script_value>(ec, "merge() expects exactly 2 arguments, got {0}", symbols->intern(std::to_string(args.size())));
            }
            return map_merge(owner, args[0], args[1]);
        });


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
