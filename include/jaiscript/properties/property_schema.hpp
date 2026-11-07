#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <typeindex>
#include <mutex>
#include <cstdint>
#include <jaiscript/math/adapter.hpp>

namespace jai {

// Forward declarations
class property_base;

// A property whose value type has a math_adapter reads and writes as that math kind: the host value's component words
// out, and component words back through the property's own assignment (observable properties notify)
struct math_property_bridge {
	script_value_type kind = script_value_type::jai_invalid_type;
	void (*read)(const void* host, uint32_t* words) = nullptr;
	bool (*assign)(property_base& property, const uint32_t* words) = nullptr;
	explicit operator bool() const noexcept { return read != nullptr; }
};

// ============================================================================
// Property Metadata (stored once per type, not per instance)
// ============================================================================
// Identity + flags only: typed value access goes through the live property_base
// erased bridge (value_type_id/value_as/assign_value), never through the schema.

struct property_meta {
	std::string name;
	std::type_index value_type_id;     // typeid(T) - the actual value type
	bool default_allow_serialization = true;
	bool is_observable = false;        // True if this is an observable_property
	bool is_signal = false;            // True for JAI_SIGNAL_PROPERTY (binds the generic Signal view)
	math_property_bridge math;         // set when the value type is math-adapted (script reads and writes the math kind)

	// Default constructor for containers
	property_meta()
		: value_type_id(typeid(void)) {}

	property_meta(std::string n, std::type_index value_ti, bool allow_ser, bool observable, bool signal_prop = false, math_property_bridge math_bridge = {})
		: name(std::move(n))
		, value_type_id(value_ti)
		, default_allow_serialization(allow_ser)
		, is_observable(observable)
		, is_signal(signal_prop)
		, math(math_bridge) {
	}
};

// ============================================================================
// Type Metadata (properties + inheritance info)
// ============================================================================

class type_property_schema {
public:
	type_property_schema() = default;

	// Register a property with this schema (properties declared directly on this type)
	void add(property_meta meta);

	// Register base class types
	void add_base(std::type_index base_type);

	// Get properties declared directly on this type (not inherited)
	const std::vector<property_meta>& own_properties() const { return own_properties_; }

	// Get direct base class type indices
	const std::vector<std::type_index>& base_types() const { return base_types_; }

	// Find property by name (in own properties only)
	const property_meta* find(const std::string& name) const;

	// Get own property names
	std::vector<std::string> property_names() const;

	bool empty() const { return own_properties_.empty(); }
	size_t size() const { return own_properties_.size(); }
	bool has_bases() const { return !base_types_.empty(); }

private:
	std::vector<property_meta> own_properties_;
	std::vector<std::type_index> base_types_;
	std::unordered_map<std::string, size_t> name_to_index_;
};

// ============================================================================
// Global Type Registry
// ============================================================================
// Sanctioned process-wide static (like the polymorphic registry): type identity is
// per-binary, not per-engine. Guarded — first-touch inserts can race across engine
// threads constructing property_owners concurrently.

class type_registry {
public:
	static type_registry& instance();

	// Get or create schema for a type
	type_property_schema& for_type(std::type_index ti);

	// Under the lock: a base chain walk (all_properties of a derived type) may read this schema from another thread
	void add_base(std::type_index ti, std::type_index base_type);

	// Get schema if it exists
	const type_property_schema* try_get(std::type_index ti) const;

	// Convenience template versions
	template<typename T>
	type_property_schema& for_type() {
		return for_type(std::type_index(typeid(T)));
	}

	template<typename T>
	const type_property_schema* try_get() const {
		return try_get(std::type_index(typeid(T)));
	}

	// Check if a type has a registered schema
	template<typename T>
	bool has_schema() const {
		return try_get<T>() != nullptr;
	}

	// Get all properties including inherited (walks base class chain)
	std::vector<const property_meta*> all_properties(std::type_index ti) const;

	template<typename T>
	std::vector<const property_meta*> all_properties() const {
		return all_properties(std::type_index(typeid(T)));
	}

	// Get all property names including inherited
	std::vector<std::string> all_property_names(std::type_index ti) const;

	template<typename T>
	std::vector<std::string> all_property_names() const {
		return all_property_names(std::type_index(typeid(T)));
	}

	// Get direct base classes
	std::vector<std::type_index> base_classes(std::type_index ti) const;

	template<typename T>
	std::vector<std::type_index> base_classes() const {
		return base_classes(std::type_index(typeid(T)));
	}

	// Check if Derived inherits from Base (directly or indirectly)
	bool inherits_from(std::type_index derived, std::type_index base) const;

	template<typename Derived, typename Base>
	bool inherits_from() const {
		return inherits_from(std::type_index(typeid(Derived)), std::type_index(typeid(Base)));
	}

private:
	type_registry() = default;

	bool inherits_from_locked(std::type_index derived, std::type_index base) const;

	void collect_properties_recursive(std::type_index ti, std::vector<const property_meta*>& result) const;

	mutable std::mutex mutex_;
	// Hashed on std::type_index: libstdc++'s type_info::before is not a strict weak order between internal-linkage and other types
	std::unordered_map<std::type_index, type_property_schema> schemas_;
};

} // namespace jai
