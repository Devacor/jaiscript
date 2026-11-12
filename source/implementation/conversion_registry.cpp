#include "../../include/jaiscript/core/conversion_registry.hpp"
#include "../../include/jaiscript/core/conversion_registry_templates.hpp"
#include "../../include/jaiscript/core/conversion_registry_impl.hpp"
#include "../../include/jaiscript/core/engine_impl.hpp"
#include "../../include/jaiscript/core/value.hpp"
#include "../../include/jaiscript/core/engine.hpp"
#include "../../include/jaiscript/core/bound_array.hpp"
#include "../../include/jaiscript/core/bound_map.hpp"
#include "../../include/jaiscript/core/dynamic_binder.hpp"
#include "../../include/jaiscript/detail/class_runtime.hpp"   // adjust_cpp_pointer
#include <limits>
#include <stdexcept>
#include <algorithm>

namespace jai {

namespace detail {

// the catalogued text, parked whole: every consumer of a native extraction error (symbolized or not) prints it as is
template<typename R>
JAI_NOINLINE R retired_layer_error(engine* eng, const class_instance& instance) {
	const class_definition* def = instance.get_class_definition();
	const std::string text = "Script layer '" + (def ? def->get_name() : std::string()) +
		"' was detached (script_layer_detach, or a move to another engine): this handle no longer reaches its object";
	return R(make_error_code(runtime_error_code::script_layer_retired), "{0}", eng ? engine_access::park_error_text(*eng, text) : 0);
}

checked_result<void*> checked_native_pointer(engine* eng, uint64_t source_id, const std::type_info& target,
	uint64_t target_id, void* pointer) {
	if (!pointer) {
		return checked_result<void*>(make_error_code(runtime_error_code::type_mismatch),
			"Cannot extract C++ receiver from null");
	}
	if (target_id != UINT64_MAX && source_id == target_id) {
		return pointer;
	}
	if (eng) {
		auto source = engine_access::get_class_definition(*eng, source_id);
		if (source) {
			if (auto* result = source->adjust_cpp_pointer(pointer, target)) {
				return result;
			}
		}
	}
	return checked_result<void*>(make_error_code(runtime_error_code::type_mismatch),
		"Object type mismatch");
}

checked_result<void*> checked_native_reference(const script_value& value, const std::type_info& target, uint64_t target_id) {
	const auto* holder = value.peek_object_holder();
	if (!holder) {
		return checked_result<void*>(make_error_code(runtime_error_code::type_mismatch), "Object type mismatch");
	}
	if (holder->is_instance()) {
		const auto* instance = holder->unchecked_instance();
		if (!instance) {
			return checked_result<void*>(make_error_code(runtime_error_code::type_mismatch), "Cannot extract C++ receiver from null");
		}
		if (instance->script_layer_retired()) {
			return retired_layer_error<checked_result<void*>>(holder->owner(), *instance);
		}
		auto* native = instance->native();
		const auto* native_holder = native ? native->peek_object_holder() : nullptr;
		if (!native_holder || native_holder->is_instance()) {
			return checked_result<void*>(make_error_code(runtime_error_code::type_mismatch), "Object type mismatch");
		}
		return checked_native_pointer(holder->owner(), native_holder->type_id(), target, target_id,
			native_holder->holds_object() ? native_holder->object() : native_holder->bound_object());
	}
	return checked_native_pointer(holder->owner(), holder->type_id(), target, target_id,
		holder->holds_object() ? holder->object() : holder->bound_object());
}

void* lane_object_pointer(const script_value& value, const std::type_info& target) {
	auto pointer = checked_native_reference(value, target);
	return pointer ? pointer.value() : nullptr;
}

bool lane_object_handle(const script_value& value, const std::type_info& target, std::shared_ptr<void>& out) {
	if (value.raw_storage_index() == script_value::TYPEID_NULL) {
		out.reset();
		return true;
	}
	const auto* holder = value.peek_object_holder();
	if (!holder) { return false; }
	auto object = checked_native_object(holder->owner(), *holder, target);
	if (!object) { return false; }
	out = std::move(object.value());
	return true;
}

void anchor_to_receiver(script_value& result, const script_value& receiver) {
	if (!result.is_non_owning_object() || !receiver.is_object()) { return; }
	auto result_holder = result.get_object_holder();
	auto receiver_holder = receiver.get_object_holder();
	if (result_holder && receiver_holder) {
		result_holder->set_keep_alive(receiver_holder->holds_object() ? receiver_holder->held() : receiver_holder->keep_alive());
	}
}

checked_result<std::shared_ptr<void>> checked_native_object(engine* eng, const script_value::object_holder& holder,
	const std::type_info& target, uint64_t target_id) {
	if (holder.is_instance()) {
		const auto* instance = holder.unchecked_instance();
		if (!instance) {
			return std::shared_ptr<void>{};
		}
		if (instance->script_layer_retired()) {
			return retired_layer_error<checked_result<std::shared_ptr<void>>>(eng, *instance);
		}
		auto* native = instance->native();
		if (!native) {
			return checked_result<std::shared_ptr<void>>(make_error_code(runtime_error_code::type_mismatch),
				"Field '_cpp_object' not found and no default value available");
		}
		const auto* native_holder = native->peek_object_holder();
		if (!native_holder || native_holder->is_instance()) {
			return checked_result<std::shared_ptr<void>>(make_error_code(runtime_error_code::type_mismatch),
				"C++ object field not found in class_instance");
		}
		return checked_native_object(eng, *native_holder, target, target_id);
	}
	std::shared_ptr<void> owner = holder.host_object();
	if (!owner && holder.bound_object()) {
		owner = holder.lock_weak_data();
		if (!owner) {
			return checked_result<std::shared_ptr<void>>(make_error_code(runtime_error_code::type_mismatch),
				"Cannot extract shared_ptr from non-owning C++ reference");
		}
	}
	if (!owner) {
		return std::shared_ptr<void>{};
	}
	auto pointer = checked_native_pointer(eng, holder.type_id(), target, target_id, owner.get());
	if (!pointer) {
		return pointer.error_value();
	}
	return std::shared_ptr<void>(std::move(owner), pointer.value());
}

[[noreturn]] void throw_native_cast_error(engine* eng, error_propagator error) {
	throw runtime_error(eng ? format_error(error, *engine_access::get_symbolizer(*eng)) : std::string(error.static_msg));
}

} // namespace detail

namespace conversions {

[[noreturn]] JAI_NOINLINE void throw_unregistered_conversion(const char* prefix, std::type_index type) {
    throw std::runtime_error(std::string(prefix) + ::jai::detail::readable_cpp_type_name(type));
}

void conversion_registry::register_entry(type_id type, conversion_entry entry) {
    conversions_.insert_or_assign(type.index, std::move(entry));
}

const conversion_entry* conversion_registry::find_entry(type_id type) const {
    auto it = conversions_.find(type.index);
    return it != conversions_.end() ? &it->second : nullptr;
}

int conversion_registry::get_builtin_conversion_cost(script_value_type from, script_value_type to) const {
    using t = script_value_type;
    if (from == to) return 0;
    switch (from) {
        case t::jai_bool_type: return to == t::jai_int_type ? 1 : to == t::jai_float_type ? 3 : 1000;
        case t::jai_char_type: return to == t::jai_int_type ? 1 : to == t::jai_float_type ? 3 : 1000;
        case t::jai_int_type: return to == t::jai_float_type ? 1 : (to == t::jai_char_type || to == t::jai_bool_type) ? 2 : 1000;
        case t::jai_float_type: return to == t::jai_int_type ? 2 : (to == t::jai_bool_type || to == t::jai_char_type) ? 3 : 1000;
        case t::jai_utf8_string_type: return to == t::jai_string_type ? 1 : 1000;   // a string parameter reads a utf8_string's bytes
        case t::jai_string_type: return to == t::jai_char_type ? 2 : 1000;   // a char parameter takes a string of one character
        default: return 1000;
    }
}

namespace {

// a host scalar as the script scalar it stores as (int widens to int64, float to double)
template<typename T>
decltype(auto) script_widened(const T& a_value) {
    if constexpr (std::is_same_v<T, int>) {
        return static_cast<int64_t>(a_value);
    } else if constexpr (std::is_same_v<T, float>) {
        return static_cast<double>(a_value);
    } else {
        return (a_value);
    }
}

template<typename T>
script_value vector_as_script_array(const std::vector<T>& a_vector, engine* a_engine) {
    if (!a_engine) {
        throw std::runtime_error("Engine reference required for vector conversion");
    }
    return conversions::convert_vector_to_script_array(a_vector, a_engine);
}

template<typename K, typename V>
script_value stdmap_as_script_map(const std::map<K, V>& a_map, engine* a_engine) {
    if (!a_engine) {
        throw std::runtime_error("Engine reference required for map conversion");
    }
    auto result = script_value::make_map(nullptr, nullptr, a_engine);
    auto& sm = const_cast<script_map&>(result.as_map());
    for (const auto& [key, value] : a_map) {
        sm.insert_or_assign(script_value(script_widened(key), a_engine), script_value(script_widened(value), a_engine));
    }
    return result;
}

}

// The registry owns these converters, so they reach it through a raw pointer: a captured shared_ptr kept every engine's
// registry (and the host classes its converters name) alive forever
void register_standard_vector_conversions(std::shared_ptr<conversion_registry> registry) {
    if (!registry) return;
    
    // Vector<int> with smart down-conversion from int64_t (script_int)
    registry->register_conversion<std::vector<int>>(
        [](const script_value& v) -> std::vector<int> {
            auto script_array = v.as<std::vector<script_value>>();
            std::vector<int> result;
            result.reserve(script_array.size());
            
            for (const auto& item : script_array) {
                if (item.is_int()) {
                    auto int64_val = item.as<int64_t>();
                    if (int64_val < std::numeric_limits<int>::min() ||
                        int64_val > std::numeric_limits<int>::max()) {
                        throw std::runtime_error("Integer value out of range for int: " + std::to_string(int64_val));
                    }
                    result.push_back(static_cast<int>(int64_val));
                } else {
                    throw std::runtime_error("Array element is not an integer");
                }
            }
            
            return result;
        },
        [registry = registry.get()](const std::vector<int>& vec) -> script_value {
            return vector_as_script_array(vec, registry->get_engine());
        }
    );
    
    // Vector<int64_t> - direct mapping to script_int
    registry->register_conversion<std::vector<int64_t>>(
        [registry = registry.get()](const script_value& v) -> std::vector<int64_t> {
            // Direct kernel: as<std::vector<T>>() consults this registry first, so routing
            // back through it recursed without bound (typed i64 buffers copy straight over)
            return conversions::convert_script_array_to_vector<int64_t>(v, registry->get_engine());
        },
        [registry = registry.get()](const std::vector<int64_t>& vec) -> script_value {
            return vector_as_script_array(vec, registry->get_engine());
        }
    );
    
    // Vector<float>
    registry->register_conversion<std::vector<float>>(
        [](const script_value& v) -> std::vector<float> {
            auto script_array = v.as<std::vector<script_value>>();
            std::vector<float> result;
            result.reserve(script_array.size());
            
            for (const auto& item : script_array) {
                if (item.is_float()) {
                    result.push_back(item.as<float>());
                } else if (item.is_int()) {
                    result.push_back(static_cast<float>(item.as<int64_t>()));
                } else {
                    throw std::runtime_error("Array element is not a number");
                }
            }
            
            return result;
        },
        [registry = registry.get()](const std::vector<float>& vec) -> script_value {
            return vector_as_script_array(vec, registry->get_engine());
        }
    );
    
    // Vector<double>
    registry->register_conversion<std::vector<double>>(
        [](const script_value& v) -> std::vector<double> {
            auto script_array = v.as<std::vector<script_value>>();
            std::vector<double> result;
            result.reserve(script_array.size());
            
            for (const auto& item : script_array) {
                if (item.is_float()) {
                    result.push_back(item.as<double>());
                } else if (item.is_int()) {
                    result.push_back(static_cast<double>(item.as<int64_t>()));
                } else {
                    throw std::runtime_error("Array element is not a number");
                }
            }
            
            return result;
        },
        [registry = registry.get()](const std::vector<double>& vec) -> script_value {
            return vector_as_script_array(vec, registry->get_engine());
        }
    );
    
    // Vector<std::string>
    registry->register_conversion<std::vector<std::string>>(
        [registry = registry.get()](const script_value& v) -> std::vector<std::string> {
            return conversions::convert_script_array_to_vector<std::string>(v, registry->get_engine());
        },
        [registry = registry.get()](const std::vector<std::string>& vec) -> script_value {
            return vector_as_script_array(vec, registry->get_engine());
        }
    );
    
    // Vector<bool>
    registry->register_conversion<std::vector<bool>>(
        [](const script_value& v) -> std::vector<bool> {
            auto script_array = v.as<std::vector<script_value>>();
            std::vector<bool> result;
            result.reserve(script_array.size());
            
            for (const auto& item : script_array) {
                if (item.is_bool()) {
                    result.push_back(item.as<bool>());
                } else if (item.is_int()) {
                    result.push_back(item.as<int64_t>() != 0);
                } else {
                    throw std::runtime_error("Array element is not a boolean or integer");
                }
            }
            
            return result;
        },
        [registry = registry.get()](const std::vector<bool>& vec) -> script_value {
            return vector_as_script_array(vec, registry->get_engine());
        }
    );
}

void register_standard_map_conversions(std::shared_ptr<conversion_registry> registry) {
    if (!registry) return;
    
    // Map<string, int> with smart down-conversion
    registry->register_conversion<std::map<std::string, int>>(
        [](const script_value& v) -> std::map<std::string, int> {
            auto local_map = v.as<script_map>();
            std::map<std::string, int> result;
            
            for (const auto& [key, value] : local_map) {
                if (!key.is_string()) {
                    throw std::runtime_error("Map key is not a string");
                }
                
                if (!value.is_int()) {
                    throw std::runtime_error("Map value is not an integer");
                }
                
                auto int64_val = value.as<int64_t>();
                if (int64_val < std::numeric_limits<int>::min() ||
                    int64_val > std::numeric_limits<int>::max()) {
                    throw std::runtime_error("Integer value out of range for int: " + std::to_string(int64_val));
                }

                result[key.as<std::string>()] = static_cast<int>(int64_val);
            }
            
            return result;
        },
        [registry = registry.get()](const std::map<std::string, int>& map) -> script_value {
            return stdmap_as_script_map(map, registry->get_engine());
        }
    );
    
    // Map<string, int64_t> - direct mapping
    registry->register_conversion<std::map<std::string, int64_t>>(
        [registry = registry.get()](const script_value& v) -> std::map<std::string, int64_t> {
            // Direct kernel, as for vector<int64_t>: as<std::map<...>>() consults this registry first
            return conversions::convert_script_map_to_stdmap<std::string, int64_t>(v, registry->get_engine());
        },
        [registry = registry.get()](const std::map<std::string, int64_t>& map) -> script_value {
            return stdmap_as_script_map(map, registry->get_engine());
        }
    );
    
    // Map<string, double> with int->double conversion
    registry->register_conversion<std::map<std::string, double>>(
        [](const script_value& v) -> std::map<std::string, double> {
            auto local_map = v.as<script_map>();
            std::map<std::string, double> result;
            
            for (const auto& [key, value] : local_map) {
                if (!key.is_string()) {
                    throw std::runtime_error("Map key is not a string");
                }
                
                double converted_value;
                if (value.is_float()) {
                    converted_value = value.as<double>();
                } else if (value.is_int()) {
                    converted_value = static_cast<double>(value.as<int64_t>());
                } else {
                    throw std::runtime_error("Map value is not a number");
                }
                
                result[key.as<std::string>()] = converted_value;
            }
            
            return result;
        },
        [registry = registry.get()](const std::map<std::string, double>& map) -> script_value {
            return stdmap_as_script_map(map, registry->get_engine());
        }
    );
    
    // Map<string, float> with int->float conversion
    registry->register_conversion<std::map<std::string, float>>(
        [](const script_value& v) -> std::map<std::string, float> {
            auto local_map = v.as<script_map>();
            std::map<std::string, float> result;
            
            for (const auto& [key, value] : local_map) {
                if (!key.is_string()) {
                    throw std::runtime_error("Map key is not a string");
                }
                
                float converted_value;
                if (value.is_float()) {
                    converted_value = value.as<float>();
                } else if (value.is_int()) {
                    converted_value = static_cast<float>(value.as<int64_t>());
                } else {
                    throw std::runtime_error("Map value is not a number");
                }
                
                result[key.as<std::string>()] = converted_value;
            }
            
            return result;
        },
        [registry = registry.get()](const std::map<std::string, float>& map) -> script_value {
            return stdmap_as_script_map(map, registry->get_engine());
        }
    );
    
    // Map<string, string>
    registry->register_conversion<std::map<std::string, std::string>>(
        [registry = registry.get()](const script_value& v) -> std::map<std::string, std::string> {
            return conversions::convert_script_map_to_stdmap<std::string, std::string>(v, registry->get_engine());
        },
        [registry = registry.get()](const std::map<std::string, std::string>& map) -> script_value {
            return stdmap_as_script_map(map, registry->get_engine());
        }
    );
    
    // Map<string, bool> with int->bool conversion
    registry->register_conversion<std::map<std::string, bool>>(
        [](const script_value& v) -> std::map<std::string, bool> {
            auto local_map = v.as<script_map>();
            std::map<std::string, bool> result;
            
            for (const auto& [key, value] : local_map) {
                if (!key.is_string()) {
                    throw std::runtime_error("Map key is not a string");
                }
                
                bool converted_value;
                if (value.is_bool()) {
                    converted_value = value.as<bool>();
                } else if (value.is_int()) {
                    converted_value = value.as<int64_t>() != 0;
                } else {
                    throw std::runtime_error("Map value is not a boolean or integer");
                }
                
                result[key.as<std::string>()] = converted_value;
            }
            
            return result;
        },
        [registry = registry.get()](const std::map<std::string, bool>& map) -> script_value {
            return stdmap_as_script_map(map, registry->get_engine());
        }
    );
    
    // Map<int, string> with bounds checking
    registry->register_conversion<std::map<int, std::string>>(
        [](const script_value& v) -> std::map<int, std::string> {
            auto local_map = v.as<script_map>();
            std::map<int, std::string> result;
            
            for (const auto& [key, value] : local_map) {
                if (!key.is_int()) {
                    throw std::runtime_error("Map key is not an integer");
                }
                
                if (!value.is_string()) {
                    throw std::runtime_error("Map value is not a string");
                }
                
                auto int64_key = key.as<int64_t>();
                if (int64_key < std::numeric_limits<int>::min() ||
                    int64_key > std::numeric_limits<int>::max()) {
                    throw std::runtime_error("Integer key out of range for int: " + std::to_string(int64_key));
                }
                
                result[static_cast<int>(int64_key)] = value.as<std::string>();
            }
            
            return result;
        },
        [registry = registry.get()](const std::map<int, std::string>& map) -> script_value {
            return stdmap_as_script_map(map, registry->get_engine());
        }
    );
    
    // Map<int64_t, string> - direct mapping
    registry->register_conversion<std::map<int64_t, std::string>>(
        [registry = registry.get()](const script_value& v) -> std::map<int64_t, std::string> {
            return conversions::convert_script_map_to_stdmap<int64_t, std::string>(v, registry->get_engine());
        },
        [registry = registry.get()](const std::map<int64_t, std::string>& map) -> script_value {
            return stdmap_as_script_map(map, registry->get_engine());
        }
    );
}

void register_bound_array_conversions(std::shared_ptr<conversion_registry> registry) {
    if (!registry) return;

    // Note: These don't actually convert, they just verify the type
    // The actual zero-copy wrapping happens in the value_converter specializations
    
    // bound_array<int>
    registry->register_conversion<bound_array<int>>(
        [](const script_value& v) -> bound_array<int> {
            if (!v.is_array()) {
                throw std::runtime_error("Cannot convert non-array to bound_array<int>");
            }
            return bound_array<int>(v);
        },
        [](const bound_array<int>& arr) -> script_value {
            return arr.as_script_value();
        }
    );
    
    // bound_array<double>
    registry->register_conversion<bound_array<double>>(
        [](const script_value& v) -> bound_array<double> {
            if (!v.is_array()) {
                throw std::runtime_error("Cannot convert non-array to bound_array<double>");
            }
            return bound_array<double>(v);
        },
        [](const bound_array<double>& arr) -> script_value {
            return arr.as_script_value();
        }
    );
    
    // bound_array<std::string>
    registry->register_conversion<bound_array<std::string>>(
        [](const script_value& v) -> bound_array<std::string> {
            if (!v.is_array()) {
                throw std::runtime_error("Cannot convert non-array to bound_array<std::string>");
            }
            return bound_array<std::string>(v);
        },
        [](const bound_array<std::string>& arr) -> script_value {
            return arr.as_script_value();
        }
    );
    
    // bound_array<bool>
    registry->register_conversion<bound_array<bool>>(
        [](const script_value& v) -> bound_array<bool> {
            if (!v.is_array()) {
                throw std::runtime_error("Cannot convert non-array to bound_array<bool>");
            }
            return bound_array<bool>(v);
        },
        [](const bound_array<bool>& arr) -> script_value {
            return arr.as_script_value();
        }
    );
}

void register_bound_map_conversions(std::shared_ptr<conversion_registry> registry) {
    if (!registry) return;

    // bound_map<std::string, int>
    registry->register_conversion<bound_map<std::string, int>>(
        [](const script_value& v) -> bound_map<std::string, int> {
            if (!v.is_map()) {
                throw std::runtime_error("Cannot convert non-map to bound_map<std::string, int>");
            }
            return bound_map<std::string, int>(v);
        },
        [](const bound_map<std::string, int>& map) -> script_value {
            return map.as_script_value();
        }
    );
    
    // bound_map<std::string, double>
    registry->register_conversion<bound_map<std::string, double>>(
        [](const script_value& v) -> bound_map<std::string, double> {
            if (!v.is_map()) {
                throw std::runtime_error("Cannot convert non-map to bound_map<std::string, double>");
            }
            return bound_map<std::string, double>(v);
        },
        [](const bound_map<std::string, double>& map) -> script_value {
            return map.as_script_value();
        }
    );
    
    // bound_map<std::string, std::string>
    registry->register_conversion<bound_map<std::string, std::string>>(
        [](const script_value& v) -> bound_map<std::string, std::string> {
            if (!v.is_map()) {
                throw std::runtime_error("Cannot convert non-map to bound_map<std::string, std::string>");
            }
            return bound_map<std::string, std::string>(v);
        },
        [](const bound_map<std::string, std::string>& map) -> script_value {
            return map.as_script_value();
        }
    );
}

void register_all_standard_conversions(std::shared_ptr<conversion_registry> registry) {
    register_standard_vector_conversions(registry);
    register_standard_map_conversions(registry);
    register_bound_array_conversions(registry);
    register_bound_map_conversions(registry);
}

} // namespace conversions

template void conversions::conversion_manager::add_custom_conversion<int>(
    std::function<int(const script_value&)>, 
    std::function<script_value(const int&)>
);

template void conversions::conversion_manager::add_custom_conversion<std::string>(
    std::function<std::string(const script_value&)>, 
    std::function<script_value(const std::string&)>
);

template void conversions::conversion_manager::add_vector_conversion<int>();
template void conversions::conversion_manager::add_vector_conversion<double>();
template void conversions::conversion_manager::add_vector_conversion<std::string>();

template void conversions::conversion_manager::add_bound_array_conversion<int>();
template void conversions::conversion_manager::add_bound_array_conversion<double>();
template void conversions::conversion_manager::add_bound_array_conversion<std::string>();
template void conversions::conversion_manager::add_bound_array_conversion<bool>();

template void conversions::conversion_manager::add_bound_map_conversion<std::string, int>();
template void conversions::conversion_manager::add_bound_map_conversion<std::string, double>();
template void conversions::conversion_manager::add_bound_map_conversion<std::string, std::string>();

} // namespace jai
