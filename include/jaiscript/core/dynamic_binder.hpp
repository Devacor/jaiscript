#pragma once

#ifndef __JAISCRIPT_CORE_dynamic_binder_HPP__
#define __JAISCRIPT_CORE_dynamic_binder_HPP__

#include "engine.hpp"
#include "engine_impl.hpp"
#include "value.hpp"
#include "types.hpp"
#include "function_binder.hpp"
#include "parameter_storage.hpp"
#include "conversion_registry.hpp"
#include "bound_array.hpp"
#include "bound_map.hpp"
#include "bound_cpp_vector.hpp"
#include "script_extensible.hpp"
#include <jaiscript/serialization/archive_impl.hpp>
#include <jaiscript/detail/static_type_name.hpp>
#include <jaiscript/detail/host_member.hpp>
#include <jaiscript/properties/property_schema.hpp>
#include <jaiscript/properties/observable_property.hpp>
#include <jaiscript/signals/signal_impl.hpp>
#include <jaiscript/signals/signal_view.hpp>
#include <string>
#include <memory>
#include <optional>
#include <array>
#include <bit>
#include <utility>
#include <cstddef>
#include <functional>
#include <vector>
#include <map>
#include <unordered_set>
#include <typeindex>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <type_traits>
#include <typeindex>

// class_instance, class_definition, access_level, delegation_type
// are defined in class_definition.hpp (extracted for faster compilation)
#include "class_definition.hpp"

namespace jai {
    class function_decl;
    class ConstructorDecl;
    class MethodDecl;
    class FieldDecl;
    class block_stmt;
    class expression;
    struct parameter;
    using expression_ptr = std::shared_ptr<expression>;

    struct script_defined_function;

    namespace detail {
        // A store into a host script_value member that holds a reference: through the reference, as a store into a reference
        // variable does (its container's element type converts or raises) (engine.cpp)
        checked_result<void> store_through_held_reference(script_value& held, const script_value& value, engine* eng);
    }
}

namespace jai {

namespace serialization {
    template<typename Derived, typename... Bases> struct register_relation;
}

template<typename T> class observable_property;

template<typename T>
class observable_property_ref {
public:
    observable_property_ref(observable_property<T>* prop, property_manager* mgr)
        : prop_(prop), mgr_(mgr) {}

    T get() const { return prop_ ? prop_->get() : T{}; }

    observable_property<T>* property() const { return prop_; }

    property_manager* manager() const { return mgr_; }

private:
    observable_property<T>* prop_;
    property_manager* mgr_;
};

// Helper function to extract base template name from full type name
// Examples: "Point<int>" -> "Point", "MyMap<std::string, int>" -> "MyMap", "Button" -> "Button"
inline std::string extract_base_template_name(const std::string& fullTypeName) {
    size_t anglePos = fullTypeName.find('<');
    if (anglePos != std::string::npos) {
        return fullTypeName.substr(0, anglePos);
    }
    return fullTypeName; // No template, return as-is
}

// Type trait to detect if a type has a constructor that takes engine*
template<typename T>
concept has_engine_constructor = requires(engine* eng) {
    T(eng);
};

// Detail namespace for factory registration helpers
namespace dynamic_binder_detail {
    using namespace serialization;

    // Helper to create wrapped script_value from C++ object
    template<typename T>
    script_value wrap_cpp_object(std::shared_ptr<T> cpp_obj, const std::string& class_name, engine* engine_ptr) {
        // Get engine for wrapping
        auto eng = engine_ptr;
        if (!eng) {
            throw serialization_error("Engine expired during deserialization");
        }

        // Get class definition
        auto class_def = detail::engine_access::get_class_definition(*eng, class_name);
        if (!class_def) {
            throw serialization_error("Class definition not found: " + class_name);
        }

        auto instance = detail::make_native_instance(*class_def, class_name, cpp_obj, eng);
        return script_value::make_object(class_name, instance, eng);
    }

    // Archive-only factory registration (no context needed)
    // Uses any_archive_reader for type-erased callback storage
    template<typename T, typename FactoryFunc>
    std::function<script_value(serialization::any_archive_reader&, uint32_t)>
    make_archive_only_factory(FactoryFunc&& factory, std::string class_name, engine* engine_ptr) {
        return [factory = std::forward<FactoryFunc>(factory), class_name = std::move(class_name), engine_ptr]
               (serialization::any_archive_reader& archive, uint32_t) -> script_value {
            auto cpp_obj = factory(archive);
            return wrap_cpp_object<T>(cpp_obj, class_name, engine_ptr);
        };
    }

    // Forward declarations for context-based deserialization factories
    // These functions require archive_reader to be fully defined before instantiation.
    // Implementations are provided in dynamic_binder_serialization.hpp which should be
    // included explicitly by code that uses these context-based factories.
    template<typename T, typename ContextType, typename FactoryFunc>
    std::function<script_value(serialization::any_archive_reader&, uint32_t)>
    make_context_only_factory(FactoryFunc&& factory, std::string class_name, engine* engine_ptr);

    template<typename T, typename ContextType, typename FactoryFunc>
    std::function<script_value(serialization::any_archive_reader&, uint32_t)>
    make_context_archive_factory(FactoryFunc&& factory, std::string class_name, engine* engine_ptr);
}

namespace dynamic_binder_validation {
    // Helper to detect if a type is a standard container
    template<typename T>
    struct is_std_container : std::false_type {};

    template<typename... Args>
    struct is_std_container<std::vector<Args...>> : std::true_type {};

    template<typename... Args>
    struct is_std_container<std::map<Args...>> : std::true_type {};

    template<typename... Args>
    struct is_std_container<std::unordered_map<Args...>> : std::true_type {};

    template<typename... Args>
    struct is_std_container<std::set<Args...>> : std::true_type {};

    template<typename... Args>
    struct is_std_container<std::unordered_set<Args...>> : std::true_type {};

    // Signal types are internal callback mechanisms and don't need registration validation
    template<typename T>
    struct is_signal_type : std::false_type {};

    template<typename Sig, typename Sync>
    struct is_signal_type<signal_emitter<Sig, Sync>> : std::true_type {};

    template<typename Sig, typename Sync>
    struct is_signal_type<signal<Sig, Sync>> : std::true_type {};

    template<typename Sig, typename Sync>
    struct is_signal_type<member_signal_emitter<Sig, Sync>> : std::true_type {};

    // std::function members are callback hooks (script lambdas assign into them);
    // like signals, they are opaque to scripts and need no registration.
    template<typename T>
    struct is_std_function : std::false_type {};

    template<typename Sig>
    struct is_std_function<std::function<Sig>> : std::true_type {};

    // Helper to unwrap smart pointers to get the inner type
    // For std::shared_ptr<T>, std::weak_ptr<T>, std::unique_ptr<T> -> extracts T
    // For other types -> returns the type as-is
    template<typename T>
    struct unwrap_smart_pointer {
        using type = T;
    };

    template<typename U>
    struct unwrap_smart_pointer<std::shared_ptr<U>> {
        using type = U;
    };

    template<typename U>
    struct unwrap_smart_pointer<std::weak_ptr<U>> {
        using type = U;
    };

    template<typename U>
    struct unwrap_smart_pointer<std::unique_ptr<U>> {
        using type = U;
    };

    template<typename T>
    using unwrap_smart_pointer_t = typename unwrap_smart_pointer<T>::type;

    // Helper to get the validation type (unwraps smart pointers and removes cv/ref)
    // For std::shared_ptr<CustomType> -> extracts CustomType
    // For int& -> extracts int
    template<typename T>
    struct get_validation_type {
        using bare_type = std::remove_cv_t<std::remove_reference_t<T>>;
        using type = unwrap_smart_pointer_t<bare_type>;
    };

    template<typename T>
    using get_validation_type_t = typename get_validation_type<T>::type;

    // Helper to determine if a type needs registration validation
    // IMPORTANT: Smart pointers are unwrapped - we validate the inner type
    template<typename T>
    constexpr bool needs_registration_check() {
        using inner_type = get_validation_type_t<T>;  // Unwrap smart pointers

        return !std::is_fundamental_v<inner_type> &&
               !math_adapted<inner_type> &&
               !std::is_same_v<inner_type, std::string> &&
               !is_std_container<inner_type>::value &&
               !is_signal_type<inner_type>::value &&
               !is_std_function<inner_type>::value &&
               !detail::is_script_value_v<inner_type> &&
               std::is_class_v<inner_type>;
    }

}

namespace detail {
	// A binder's place among its engine's uncommitted binders, in creation order (engine.cpp). The engine commits those
	// still alive when it first loads a script, and lets go of every one when it dies.
	class pending_binder {
	public:
		pending_binder(const pending_binder&) = delete;
		pending_binder& operator=(const pending_binder&) = delete;

	protected:
		using commit_function = void (*)(pending_binder&);

		pending_binder(engine& owner, const std::string& class_name, commit_function commit);
		pending_binder(pending_binder&& other) noexcept;   // takes other's place
		~pending_binder();

		bool uncommitted() const { return owner_ != nullptr; }
		void leave() noexcept;
		// Commits when uncommitted; a failure is recorded on the engine, which raises it at its next load
		void commit_or_record() noexcept;

		std::string class_name_;

	private:
		friend class jai::engine;

		engine* owner_ = nullptr;
		pending_binder* previous_ = nullptr;
		pending_binder* next_ = nullptr;
		commit_function commit_ = nullptr;
	};

	// Public C++ method adapter: the shared runtime helper returns checked_result.
	template<typename T>
	T* extract_cpp_object_ptr(const script_value& val, uint64_t target_id = UINT64_MAX) {
		if (val.raw_storage_index() == script_value::TYPEID_CPP_BOUND) {
			return val.get_cpp_bound_as<T>();
		}
		return static_cast<T*>(native_cast_or_throw(checked_native_reference(val, typeid(T), target_id), val.payload_engine()));
	}

	// A bound class's own conversions (schema_binding.cpp); the name is the one bound, which a later rename does not change
	struct bound_class_conversion {
		std::shared_ptr<class_definition> def;
		std::string name;

		script_value wrap(std::shared_ptr<void> object, engine* eng) const;
		std::shared_ptr<void> unwrap_shared(const script_value& v, const std::type_info& type) const;
		void require_object(const script_value& v) const;
	};

	// One engine-wide "Signal" class serves every signature through the erased table; registered the first time any
	// signal member binds
	void ensure_signal_view_type_registered(engine& eng);
	script_value make_signal_view(engine* eng, const signal_script_ops& ops);

	// Every bound method's ordinary call is this one callable type over a static function of the binding, so a binding
	// instantiates std::function once per TU instead of once per method
	struct host_method_call {
		checked_result<script_value> (*call)(const void* state, const std::vector<script_value>& args) = nullptr;
		std::shared_ptr<const void> state;

		checked_result<script_value> operator()(const std::vector<script_value>& args) const { return call(state.get(), args); }
	};

	// A member function pointer held as bytes, so the state's type depends only on the pointer's size
	template<size_t N>
	struct member_call_state {
		std::array<std::byte, N> method;
		engine* eng;
		uint64_t native_type_id;
	};

	// A stateless callable (empty, trivially constructible and destructible) is made at the call instead of stored
	struct stateless_call_state {
		engine* eng;
		uint64_t native_type_id;
	};

	template<typename Callable>
	struct callable_call_state {
		mutable Callable callable;
		engine* eng;
		uint64_t native_type_id;
	};

	// A member type the in-place lane reads with the getter's own conversion: integers (not characters), floating
	// point, bool, std::string, script_value
	template<typename P>
	inline constexpr bool host_member_value = (std::is_integral_v<P> && !std::is_same_v<P, char> && !std::is_same_v<P, signed char> &&
	                                           !std::is_same_v<P, unsigned char> && !std::is_same_v<P, wchar_t> && !std::is_same_v<P, char8_t> &&
	                                           !std::is_same_v<P, char16_t> && !std::is_same_v<P, char32_t>) ||
	                                          std::is_floating_point_v<P> || std::is_same_v<P, std::string> || detail::is_script_value_v<P>;

	template<typename P>
	script_value host_member_read(const void* address, engine* eng) {
		return value_converter<P>::to(*static_cast<const P*>(address), eng);
	}

	// The values the setter's as<P>() stores unchanged: an int in range for an integer, a float for a double, a bool, a string
	template<typename P>
	bool host_member_write(void* address, const script_value& value) {
		const size_t kind = value.raw_storage_index();
		if constexpr (std::is_same_v<P, bool>) {
			if (kind != script_value::TYPEID_BOOL) { return false; }
			*static_cast<P*>(address) = value.unchecked_as_bool();
		} else if constexpr (std::is_integral_v<P>) {
			if (kind != script_value::TYPEID_INT || !std::in_range<P>(value.unchecked_as_int())) { return false; }
			*static_cast<P*>(address) = static_cast<P>(value.unchecked_as_int());
		} else if constexpr (std::is_same_v<P, double>) {
			if (kind != script_value::TYPEID_FLOAT) { return false; }
			*static_cast<P*>(address) = value.unchecked_as_float();
		} else if constexpr (std::is_same_v<P, std::string>) {
			if (kind != script_value::TYPEID_STRING) { return false; }
			*static_cast<P*>(address) = std::string(std::string_view(value.unchecked_as_string()));
		} else if constexpr (detail::is_script_value_v<P>) {
			// a held reference stores through itself with its container's element type (the setter's business); a scalar or
			// a string clones as itself
			script_value& held = *static_cast<script_value*>(address);
			if (held.is_reference() || (kind != script_value::TYPEID_NULL && kind != script_value::TYPEID_INT && kind != script_value::TYPEID_FLOAT &&
			                            kind != script_value::TYPEID_BOOL && kind != script_value::TYPEID_CHAR && kind != script_value::TYPEID_STRING)) {
				return false;
			}
			held = value;
		} else {
			return false;
		}
		return true;
	}

	template<typename T, typename P>
	struct host_member_lane_of : host_member_lane {
		P T::*member = nullptr;
		uint64_t native_type_id = UINT64_MAX;
	};

	template<typename T, typename P>
	void* host_member_locate(const host_member_lane& lane, const script_value& receiver) {
		const auto& self = static_cast<const host_member_lane_of<T, P>&>(lane);
		try {
			T* object = extract_cpp_object_ptr<T>(receiver, self.native_type_id);
			return object ? const_cast<void*>(static_cast<const void*>(&(object->*self.member))) : nullptr;
		} catch (...) {
			return nullptr;
		}
	}

	template<typename T, typename P>
	std::shared_ptr<const host_member_lane> make_host_member_lane(P T::*member, uint64_t native_type_id) {
		auto lane = std::make_shared<host_member_lane_of<T, P>>();
		lane->member = member;
		lane->native_type_id = native_type_id;
		lane->read = &host_member_read<std::remove_cv_t<P>>;
		if constexpr (std::is_copy_assignable_v<P>) { lane->write = &host_member_write<P>; }
		lane->locate = &host_member_locate<T, P>;
		if constexpr (std::is_same_v<std::remove_cv_t<P>, script_value>) {
			// a non-owning object takes the getter's read, which pins the receiver's object under it
			lane->serves = [](const void* address) { return !static_cast<const script_value*>(address)->is_non_owning_object(); };
		}
		return lane;
	}

	template<typename ValueT>
	script_value script_value_from_cpp(const ValueT& value, engine* eng) {
		if constexpr (std::is_same_v<ValueT, int> || std::is_same_v<ValueT, int32_t>) {
			return script_value(static_cast<script_int>(value), eng);
		} else if constexpr (std::is_same_v<ValueT, int64_t>) {
			return script_value(value, eng);
		} else if constexpr (std::is_same_v<ValueT, uint64_t> || std::is_same_v<ValueT, uint32_t> ||
							std::is_same_v<ValueT, uint16_t> || std::is_same_v<ValueT, uint8_t> ||
							std::is_same_v<ValueT, int16_t> || std::is_same_v<ValueT, int8_t>) {
			return script_value(static_cast<script_int>(value), eng);
		} else if constexpr (std::is_same_v<ValueT, float>) {
			return script_value(static_cast<script_float>(value), eng);
		} else if constexpr (std::is_same_v<ValueT, double>) {
			return script_value(static_cast<script_float>(value), eng);
		} else if constexpr (std::is_same_v<ValueT, bool>) {
			return script_value(value, eng);
		} else if constexpr (std::is_same_v<ValueT, std::string>) {
			return script_value(value, eng);
		} else {
			return value_converter<ValueT>::to(value, eng);
		}
	}

	// Out of line (schema_binding.cpp) so each registrar TU no longer instantiates the 13 primitive binders.
	struct schema_property_access {
		void* (*extract)(const script_value&, uint64_t);
		property_base* (*find)(void*, const std::string&);
		property_manager* (*manager)(void*);
		size_t owner_size = 0;   // sizeof the bound type: a property object inside it is a member at a fixed offset
	};

	template<typename T>
	void* schema_extract(const script_value& val, uint64_t target_id) {
		return extract_cpp_object_ptr<T>(val, target_id);
	}
	template<typename T>
	property_base* schema_find(void* self, const std::string& name) {
		return static_cast<T*>(self)->find_reflected_property(name);
	}
	template<typename T>
	property_manager* schema_manager(void* self) {
		return &static_cast<T*>(self)->property_mgr;
	}

	bool bind_schema_primitive_property(engine& a_engine, class_definition& class_def, serialization::class_metadata& metadata,
		const std::string& prop_name, std::type_index value_type, bool is_observable, const schema_property_access& access);
	// A property of a math-adapted type reads and writes as its math kind (schema_binding.cpp)
	void bind_schema_math_property(engine& a_engine, class_definition& class_def, serialization::class_metadata& metadata,
		const std::string& prop_name, const math_property_bridge& bridge, const schema_property_access& access);

	// A math property auto_bind defers to build(), where a bound method of the same name takes the spelling instead
	struct pending_math_property {
		std::string name;
		math_property_bridge bridge;
		schema_property_access access;
	};
	void bind_pending_math_properties(engine& a_engine, class_definition& class_def, serialization::class_metadata& metadata,
		std::vector<pending_math_property>& pending);

	// A getter-only property rejects every store spelling through its setter (the bare, this., obj. and reference-store
	// landings all funnel to _set_<name>), so the text lives once (schema_binding.cpp)
	void register_read_only_setter(engine& a_engine, class_definition& class_def, const std::string& name);

	// What every native constructor of a class needs; one type for every class, so constructors share one erased call
	struct native_constructor_state {
		std::shared_ptr<class_definition> class_def;
		std::string class_name;
		engine* eng;
	};

	// The address of a bound data member inside its receiver: one erased callable type for every member
	struct host_member_address {
		void* (*address)(const void* state, const script_value& receiver) = nullptr;
		std::shared_ptr<const void> state;

		void* operator()(const script_value& receiver) const { return address(state.get(), receiver); }
	};
} // namespace detail

// ============================================================================
// Bind Mode for auto_build()
// ============================================================================

enum class bind_mode {
    all,        // Base classes + properties + auto-detected methods/constructors
    properties, // Base classes + properties only (no auto methods/constructors)
    hierarchy   // Base classes only (no properties or auto methods)
};

// ============================================================================
// Concepts for auto-detection (used by auto_build)
// ============================================================================

template<typename U> class dynamic_binder;

namespace auto_bind_concepts {

// Detect if T has to_string() method
template<typename T>
concept has_to_string = requires(const T& t) {
    { t.to_string() } -> std::convertible_to<std::string>;
};

// Detect if T has size() method
template<typename T>
concept has_size = requires(const T& t) {
    { t.size() } -> std::convertible_to<size_t>;
};

// Detect if T has empty() method
template<typename T>
concept has_empty = requires(const T& t) {
    { t.empty() } -> std::convertible_to<bool>;
};

// Detect if T has operator==
template<typename T>
concept has_equality = requires(const T& a, const T& b) {
    { a == b } -> std::convertible_to<bool>;
};

// Detect if T uses property_owner CRTP (has _jai_owner_type)
template<typename T>
concept is_property_owner = requires {
    typename T::_jai_owner_type;
};

// Detect if T has base types defined
template<typename T>
concept has_base_types = requires {
    typename T::_jai_base_types;
};

// allows classes to register their own private members during auto_bind()
// (needs the real jai::dynamic_binder — a fwd-decl in this namespace shadows it and falsifies the concept for every T)
template<typename T>
concept has_jai_auto_bind = requires(dynamic_binder<T>& builder) {
    { T::jai_auto_bind(builder) } -> std::same_as<void>;
};

} // namespace auto_bind_concepts

// Builder pattern for registering C++ classes to JaiScript
template<typename T>
class dynamic_binder : private detail::pending_binder {
    static_assert(!math_adapted<T>, "T binds as a native math type (math_adapter); delete its dynamic_binder registration");
public:
    dynamic_binder(engine& engine, const std::string& class_name)
        : detail::pending_binder(engine, class_name, &commit_pending), engine_(engine) {
        // Extract base template name if this is a templated type
        std::string baseTemplateName = extract_base_template_name(class_name);

        // Register the base template name if it contains template syntax
        if (baseTemplateName != class_name) {
            engine.register_template_type(baseTemplateName);
        }

        // Intern the class name for fast type comparisons
        uint64_t type_id = engine.symbolize(class_name);

        // Adopt an auto-registered projection of this type (base_class<B> materializes
        // unregistered property_owner bases under their derived name): the explicit
        // registration continues configuring the SAME definition — parent edges and
        // schema bindings survive — and stamps the curated name. The placeholder name
        // stays mapped as an alias. INVARIANT: adoption renames the definition, and a
        // rename under live instances would break their interned type checks — so
        // explicit registrations must land before scripts construct auto-registered
        // placeholders. Violations throw here, loudly, at the registration site.
        if (auto existing = engine.get_class_definition_by_type(std::type_index(typeid(T)));
            existing && existing->auto_registered()) {
            if (existing->has_live_instances()) {
                throw std::runtime_error("dynamic_binder<T>(engine, \"" + class_name +
                    "\"): adopting the auto-registered definition '" + existing->get_name() +
                    "' while script instances of it are alive would change their type identity. "
                    "Complete explicit registrations before running scripts that construct "
                    "auto-registered bases (or keep constructing via '" + existing->get_name() + "').");
            }
            class_def_ = existing;
            class_def_->set_auto_registered(false);
            class_def_->rename(class_name, type_id);
        } else {
            class_def_ = detail::make_class_definition(class_name, type_id, &engine_);
        }
        class_def_->set_cpp_polymorphic(std::is_polymorphic_v<T>);

        // Initialize serialization metadata
        serialization_metadata_.class_name = class_name;
        serialization_metadata_.current_version = 1;
    }
    
    // shared_ptr / raw-pointer convenience (engine::make() returns a shared_ptr)
    dynamic_binder(const std::shared_ptr<engine>& engine_ptr, const std::string& class_name)
        : dynamic_binder(*engine_ptr, class_name) {
    }
    dynamic_binder(engine* engine_ptr, const std::string& class_name)
        : dynamic_binder(*engine_ptr, class_name) {
    }

    // The binding commits here unless build() or the engine's first load already committed it
    ~dynamic_binder() {
        commit_or_record();
    }

    // base_class<B>'s auto-registration configures a sibling instantiation directly
    template<typename> friend class dynamic_binder;

    dynamic_binder(const dynamic_binder&) = delete;
    dynamic_binder& operator=(const dynamic_binder&) = delete;

    dynamic_binder(dynamic_binder&& other) noexcept
        : detail::pending_binder(std::move(other))
        , engine_(other.engine_)
        , class_def_(std::move(other.class_def_))
        , serialization_metadata_(std::move(other.serialization_metadata_))
        , pending_math_properties_(std::move(other.pending_math_properties_))
        , has_explicit_constructor_(other.has_explicit_constructor_)
    {
    }

    // Add constructor
    template<typename... Args>
    dynamic_binder& constructor() {
        has_explicit_constructor_ = true;  // Track that user explicitly registered a constructor

        // Register the constructor as an overloaded function
        if constexpr (sizeof...(Args) == 0) {
            engine_.add_overloaded_function(class_name_, 0, detail::host_method_call{ &native_constructor_call<>, constructor_state() });
        } else {
            engine_.add_overloaded_function_with_full_types(class_name_, sizeof...(Args),
                writeback_tagged<detail::host_writeback_mask<Args...>()>(detail::host_method_call{ &native_constructor_call<Args...>, constructor_state() }),
                std::vector<param_type_info>{ engine::map_cpp_type_to_param_info<std::decay_t<Args>>()... });
        }

        return *this;
    }

private:
    std::shared_ptr<const void> constructor_state() const {
        return std::make_shared<const detail::native_constructor_state>(detail::native_constructor_state{ class_def_, class_name_, &engine_ });
    }

    template<typename... Args>
    static checked_result<script_value> native_constructor_call(const void* state, const std::vector<script_value>& args) {
        const detail::native_constructor_state& st = *static_cast<const detail::native_constructor_state*>(state);
        std::shared_ptr<T> cpp_obj;
        if constexpr (sizeof...(Args) != 0) {
            cpp_obj = dynamic_binder<T>::template create_object<Args...>(args, std::index_sequence_for<Args...>{}, st.eng);
        } else if constexpr (has_engine_constructor<T>) {
            cpp_obj = std::make_shared<T>(st.eng);
        } else {
            cpp_obj = std::make_shared<T>();
        }
        auto instance = detail::make_native_instance(*st.class_def, st.class_name, cpp_obj, st.eng);
        script_value result = script_value::make_object(st.class_name, instance, st.eng);
        if (st.eng->parallel_region_active()) [[unlikely]] { detail::note_region_made(result, st.eng); }
        return result;
    }

    static std::shared_ptr<void> copy_native_object(const void* src) {
        return std::make_shared<T>(*static_cast<const T*>(src));
    }

public:
    
    // Build a C++ parameter signature from `Count` tuple elements starting at `Offset` (the offset
    // skips the leading self parameter of a lambda method). Used to register typed overloads.
    template<typename Tuple, size_t Offset, size_t... Is>
    static std::vector<param_type_info> param_sig_from_tuple_impl(std::index_sequence<Is...>) {
        return { engine::map_cpp_type_to_param_info<std::decay_t<std::tuple_element_t<Offset + Is, Tuple>>>()... };
    }
    template<typename Tuple, size_t Offset, size_t Count>
    static std::vector<param_type_info> param_sig_from_tuple() {
        return param_sig_from_tuple_impl<Tuple, Offset>(std::make_index_sequence<Count>{});
    }
    // A bound method's result kind for the checker (class_definition::strict_signatures): a chaining T& is the class
    template<typename R>
    static param_type_info result_info() {
        if constexpr (std::is_void_v<R>) {
            return param_type_info();
        } else if constexpr (std::is_reference_v<R> && std::is_same_v<std::remove_cvref_t<R>, T>) {
            return param_type_info(script_value_type::jai_object_type, std::type_index(typeid(T)));
        } else {
            return engine::map_cpp_type_to_param_info<std::remove_cvref_t<R>>();
        }
    }

    // The class's constructor or destructor touches state other objects share (a registry, a cache, a service lookup): a
    // parallel body refuses to construct it, and any script class over it, before any element runs. Host construction is
    // otherwise taken to be safe on any thread.
    dynamic_binder& serial_only_constructor() {
        class_def_->serial_only_construction(true);
        return *this;
    }

    // Hold every bound method of this class to its C++ signature in the strict checker
    dynamic_binder& strict_signatures() {
        class_def_->set_strict_signatures(true);
        return *this;
    }

    // The direct lane a bound method's C++ signature allows (class_definition::native_lane): the vm calls the method it bound with the
    // ordinary call's conversions, reading the receiver and the arguments in place: a bool, an int in the parameter's range, a number
    // for a floating parameter, a string, any value for a script_value parameter, and a bound object (T&, const T&, T, shared_ptr<T>)
    // the ordinary call's extraction takes; any other argument declines to the ordinary call. An overload set dispatches on exact
    // argument kinds (class_definition::native_exact). A pointer result, a reference-writing parameter or a guarded method has no lane.
    template<typename P>
    static constexpr bool lane_param() {
        using D = std::remove_cvref_t<P>;
        if constexpr (detail::host_object_param<P>()) { return true; }
        else if constexpr (!detail::host_lane_param<P>()) { return false; }
        else if constexpr (std::is_same_v<D, bool> || std::is_same_v<D, std::string> || detail::is_script_value_v<D> || std::is_floating_point_v<D>) { return true; }
        else if constexpr (std::is_integral_v<D>) {
            return !std::is_same_v<D, char> && !std::is_same_v<D, signed char> && !std::is_same_v<D, unsigned char> && !std::is_same_v<D, wchar_t> &&
                   !std::is_same_v<D, char8_t> && !std::is_same_v<D, char16_t> && !std::is_same_v<D, char32_t>;
        } else { return false; }
    }
    // a reference result is anchored to its receiver as the ordinary call anchors it; a pointer result keeps the ordinary call
    template<typename R>
    static constexpr bool lane_result() {
        return !std::is_pointer_v<std::remove_cvref_t<R>>;
    }
    template<typename P>
    static bool lane_arg(const script_value& a) {
        using D = std::remove_cvref_t<P>;
        const size_t k = a.raw_storage_index();
        if constexpr (detail::host_object_param<P>()) {
            if constexpr (is_specialization_v<D, std::shared_ptr>) {
                if (k == script_value::TYPEID_NULL) { return true; }
            }
            return k == script_value::TYPEID_OBJECT;
        }
        else if constexpr (detail::is_script_value_v<D>) { return true; }
        else if constexpr (std::is_same_v<D, bool>) { return k == script_value::TYPEID_BOOL; }
        else if constexpr (std::is_integral_v<D>) { return k == script_value::TYPEID_INT && std::in_range<D>(a.unchecked_as_int()); }
        else if constexpr (std::is_floating_point_v<D>) { return k == script_value::TYPEID_INT || k == script_value::TYPEID_FLOAT; }
        else { return k == script_value::TYPEID_STRING; }
    }
    template<typename... Args, size_t... Is>
    static bool lane_args(const script_value* args, std::index_sequence<Is...>) { return (lane_arg<Args>(args[Is]) && ...); }
    // an overload's exact claim (class_definition::native_exact): each argument exactly its parameter's kind
    template<typename P>
    static bool lane_exact_arg(const script_value& a) {
        using D = std::remove_cvref_t<P>;
        const size_t k = a.raw_storage_index();
        if constexpr (detail::host_object_param<P>() || detail::is_script_value_v<D>) { return false; }
        else if constexpr (std::is_same_v<D, bool>) { return k == script_value::TYPEID_BOOL; }
        else if constexpr (std::is_integral_v<D>) { return k == script_value::TYPEID_INT && std::in_range<D>(a.unchecked_as_int()); }
        else if constexpr (std::is_floating_point_v<D>) { return k == script_value::TYPEID_FLOAT; }
        else { return k == script_value::TYPEID_STRING; }
    }
    template<typename... Args>
    static bool lane_exact(const script_value* args, size_t argc) {
        if (argc != sizeof...(Args)) { return false; }
        return [&]<size_t... Is>(std::index_sequence<Is...>) { return (lane_exact_arg<Args>(args[Is]) && ...); }(std::index_sequence_for<Args...>{});
    }
    template<typename ArgsTuple, size_t... Is>
    static bool self_lane_exact_impl(const script_value* args, std::index_sequence<Is...>) {
        return (lane_exact_arg<std::tuple_element_t<Is + 1, ArgsTuple>>(args[Is]) && ...);
    }
    template<typename ArgsTuple, size_t N>
    static bool self_lane_exact(const script_value* args, size_t argc) {
        return argc == N && self_lane_exact_impl<ArgsTuple>(args, std::make_index_sequence<N>{});
    }
    template<typename Requirements>
    static bool lane_unguarded(const Requirements& required) {
        if constexpr (std::is_same_v<std::decay_t<Requirements>, privilege>) { return required == privilege::none; }
        else { return false; }
    }
    template<typename Method>
    using lane_state = detail::member_call_state<sizeof(Method)>;
    template<typename Callable>
    static constexpr bool stateless_callable() {
        return std::is_empty_v<Callable> && std::is_trivially_default_constructible_v<Callable> && std::is_trivially_destructible_v<Callable>;
    }
    template<typename Callable>
    using call_state_t = std::conditional_t<stateless_callable<Callable>(), detail::stateless_call_state, detail::callable_call_state<Callable>>;
    // A lane argument as its parameter reads it. A const script_value& parameter takes the value out of the caller's slot (a
    // move, no copy): the method may call back into script, which can move the buffer that slot lives in. Every other lane
    // parameter converts to a value before the call.
    template<typename P>
    struct lane_held {
        script_value& slot;
        explicit lane_held(script_value& s) : slot(s) {}
        bool prepare() { return true; }
        decltype(auto) get(engine* eng) { return detail::value_converter<P>::from(slot, eng); }
    };
    template<typename P> requires std::is_same_v<P, const script_value&>
    struct lane_held<P> {
        script_value owned;
        explicit lane_held(script_value& slot) : owned(std::move(slot)) {}
        bool prepare() { return true; }
        const script_value& get(engine*) const { return owned; }
    };
    // a bound object argument: the ordinary call's own extraction (detail::checked_native_reference / checked_native_object),
    // made before the call; an argument it would refuse declines the lane, so the ordinary call reports it
    template<typename P> requires (detail::host_object_param<P>() && !is_specialization_v<std::remove_cvref_t<P>, std::shared_ptr>)
    struct lane_held<P> {
        using D = std::remove_cvref_t<P>;
        script_value& slot;
        void* object = nullptr;
        explicit lane_held(script_value& s) : slot(s) {}
        bool prepare() { return (object = detail::lane_object_pointer(slot, typeid(D))) != nullptr; }
        D& get(engine*) const { return *static_cast<D*>(object); }
    };
    template<typename P> requires (detail::host_object_param<P>() && is_specialization_v<std::remove_cvref_t<P>, std::shared_ptr>)
    struct lane_held<P> {
        using E = typename std::remove_cvref_t<P>::element_type;
        script_value& slot;
        std::shared_ptr<E> handle;
        explicit lane_held(script_value& s) : slot(s) {}
        bool prepare() {
            std::shared_ptr<void> owner;
            if (!detail::lane_object_handle(slot, typeid(E), owner)) { return false; }
            handle = std::static_pointer_cast<E>(std::move(owner));
            return true;
        }
        const std::shared_ptr<E>& get(engine*) const { return handle; }
    };
    // a reference result as the ordinary call hands it: converted, and a handle into an object anchored to the receiver
    template<typename R>
    static void lane_result_out(R&& result, const script_value& receiver, engine* eng, script_value& out) {
        out = detail::value_converter<R>::to(std::forward<R>(result), eng);
        if constexpr (std::is_lvalue_reference_v<R>) { detail::anchor_to_receiver(out, receiver); }
    }
    template<typename Self, typename R, typename Method, typename... Args, size_t... Is>
    static bool lane_call(Self* obj, const lane_state<Method>& st, const script_value& receiver, script_value* args, script_value& out,
                          std::index_sequence<Is...>) {
        detail::parameter_storage storage;
        detail::parameter_storage::scope_guard guard(st.eng, &storage);
        std::tuple<lane_held<Args>...> held{ lane_held<Args>(args[Is])... };
        if (!(std::get<Is>(held).prepare() && ...)) { return false; }
        if constexpr (std::is_void_v<R>) {
            (obj->*std::bit_cast<Method>(st.method))(std::get<Is>(held).get(st.eng)...);
            out = script_value(std::monostate{}, st.eng);
        } else if constexpr (std::is_lvalue_reference_v<R>) {
            R result = (obj->*std::bit_cast<Method>(st.method))(std::get<Is>(held).get(st.eng)...);
            lane_result_out<R>(result, receiver, st.eng, out);
        } else {
            R result = (obj->*std::bit_cast<Method>(st.method))(std::get<Is>(held).get(st.eng)...);
            out = detail::value_converter<R>::to(result, st.eng);
        }
        return true;
    }
    template<typename Self, typename R, typename Method, typename... Args>
    static bool member_lane(const void* state, void* self, const script_value& receiver, script_value* args, size_t argc, script_value& out) {
        if (argc != sizeof...(Args) || !lane_args<Args...>(args, std::index_sequence_for<Args...>{})) { return false; }
        return lane_call<Self, R, Method, Args...>(static_cast<Self*>(self), *static_cast<const lane_state<Method>*>(state), receiver, args, out,
                                                   std::index_sequence_for<Args...>{});
    }
    // a callable method's lane calls the callable the ordinary call holds (one object: a stateful functor's state is shared)
    template<typename R, typename ArgsTuple, typename Callable, size_t... Is>
    static bool self_lane_call(Callable& callable, engine* eng, void* self, const script_value& receiver, script_value* args, script_value& out, std::index_sequence<Is...>) {
        detail::parameter_storage storage;
        detail::parameter_storage::scope_guard guard(eng, &storage);
        T& obj = *static_cast<T*>(self);
        std::tuple<lane_held<std::tuple_element_t<Is + 1, ArgsTuple>>...> held{ lane_held<std::tuple_element_t<Is + 1, ArgsTuple>>(args[Is])... };
        if (!(std::get<Is>(held).prepare() && ...)) { return false; }
        if constexpr (std::is_void_v<R>) {
            callable(obj, std::get<Is>(held).get(eng)...);
            out = script_value(std::monostate{}, eng);
        } else if constexpr (std::is_lvalue_reference_v<R>) {
            R result = callable(obj, std::get<Is>(held).get(eng)...);
            lane_result_out<R>(result, receiver, eng, out);
        } else {
            R result = callable(obj, std::get<Is>(held).get(eng)...);
            out = detail::value_converter<R>::to(result, eng);
        }
        return true;
    }
    template<typename R, typename ArgsTuple, typename Callable, size_t... Is>
    static bool self_lane_args(const script_value* args, std::index_sequence<Is...>) { return (lane_arg<std::tuple_element_t<Is + 1, ArgsTuple>>(args[Is]) && ...); }
    template<typename R, typename ArgsTuple, typename Callable, size_t N>
    static bool self_lane(const void* state, void* self, const script_value& receiver, script_value* args, size_t argc, script_value& out) {
        if (argc != N || !self_lane_args<R, ArgsTuple, Callable>(args, std::make_index_sequence<N>{})) { return false; }
        const call_state_t<Callable>& st = *static_cast<const call_state_t<Callable>*>(state);
        if constexpr (stateless_callable<Callable>()) {
            Callable callable{};
            return self_lane_call<R, ArgsTuple, Callable>(callable, st.eng, self, receiver, args, out, std::make_index_sequence<N>{});
        } else {
            return self_lane_call<R, ArgsTuple, Callable>(st.callable, st.eng, self, receiver, args, out, std::make_index_sequence<N>{});
        }
    }

    template<bool IsConst, typename R, typename Method, typename... Args>
    static checked_result<script_value> member_method_call(const void* state, const std::vector<script_value>& args) {
        const lane_state<Method>& st = *static_cast<const lane_state<Method>*>(state);
        // args[0] is 'this'; the class's dispatch called this overload for its arity
        T* cpp_obj = detail::extract_cpp_object_ptr<T>(args[0], st.native_type_id);
        if constexpr (IsConst) {
            return dynamic_binder<T>::call_const_method(cpp_obj, std::bit_cast<Method>(st.method), args, std::index_sequence_for<Args...>{}, st.eng);
        } else {
            return dynamic_binder<T>::call_method(cpp_obj, std::bit_cast<Method>(st.method), args, std::index_sequence_for<Args...>{}, st.eng);
        }
    }

    template<typename R, typename ArgsTuple, typename Callable>
    static checked_result<script_value> callable_method_call(const void* state, const std::vector<script_value>& args) {
        using traits = detail::function_traits<Callable>;
        const call_state_t<Callable>& st = *static_cast<const call_state_t<Callable>*>(state);
        // args[0] is the receiver, the rest are the script arguments (the class's dispatch called this overload for its arity)
        T* cpp_obj = detail::extract_cpp_object_ptr<T>(args[0], st.native_type_id);
        if constexpr (stateless_callable<Callable>()) {
            Callable callable{};
            return call_lambda_with_self<R, ArgsTuple>(callable, cpp_obj, args, std::make_index_sequence<traits::arity>{}, st.eng);
        } else {
            return call_lambda_with_self<R, ArgsTuple>(st.callable, cpp_obj, args, std::make_index_sequence<traits::arity>{}, st.eng);
        }
    }

    template<bool IsConst, typename R, typename Method, typename... Args, typename Requirements>
    dynamic_binder& bind_member_method(const std::string& name, Method method, Requirements required) {
        constexpr uint64_t writeback = detail::host_writeback_mask<Args...>();
        using State = lane_state<Method>;
        auto state = std::make_shared<const State>(State{ std::bit_cast<std::array<std::byte, sizeof(Method)>>(method), &engine_, class_def_->get_type_id() });

        // Add method to the class definition (for object.method() calls), carrying the C++
        // parameter signature so same-arity overloads (e.g. set(float...) vs set(int...)) resolve
        // by argument type instead of the last registration silently winning.
        const bool unguarded = lane_unguarded(required);
        class_def_->note_host_overload(engine_.symbolize(name), sizeof...(Args), IsConst);
        class_def_->add_method(name, writeback_tagged<writeback>(engine_.guard_function(detail::host_method_call{ &member_method_call<IsConst, R, Method, Args...>, state }, std::move(required))), sizeof...(Args),
            std::vector<param_type_info>{ engine::map_cpp_type_to_param_info<std::decay_t<Args>>()... }, result_info<R>());
        if constexpr (lane_result<R>() && (lane_param<Args>() && ...)) {
            if (unguarded) {
                class_def_->add_native_lane(engine_.symbolize(name), &member_lane<std::conditional_t<IsConst, const T, T>, R, Method, Args...>, std::move(state),
                                            &lane_exact<Args...>);
                return *this;
            }
        }
        class_def_->withdraw_native_lane(engine_.symbolize(name));
        return *this;
    }
    template<typename ArgsTuple, size_t... Is>
    static constexpr bool self_lane_params(std::index_sequence<Is...>) { return (lane_param<std::tuple_element_t<Is + 1, ArgsTuple>>() && ...); }

    // The trailing function_options: the privilege the call requires, nodiscard (Class.method drops no result) and serial (a
    // parallel body may not call it). serial on a non-const overload covers the name's non-const overloads; on a const one, all.
    template<typename R, typename... Args>
    dynamic_binder& method(const std::string& name, R(T::*member)(Args...), function_options options) {
        bind_member_method<false, R, R(T::*)(Args...), Args...>(name, member, options.required);
        return apply_method_options(name, false, options);
    }
    template<typename R, typename... Args>
    dynamic_binder& method(const std::string& name, R(T::*member)(Args...) const, function_options options) {
        bind_member_method<true, R, R(T::*)(Args...) const, Args...>(name, member, options.required);
        return apply_method_options(name, true, options);
    }
    template<typename Callable>
    dynamic_binder& method(const std::string& name, Callable&& callable, function_options options) {
        constexpr bool is_const = !first_param_is_mutable_self<typename detail::function_traits<std::decay_t<Callable>>::argument_types>();
        method(name, std::forward<Callable>(callable), options.required);
        return apply_method_options(name, is_const, options);
    }

    // Add method binding - member function pointer version
    template<typename R, typename... Args, typename Requirements = privilege>
    dynamic_binder& method(const std::string& name, R(T::*method)(Args...), Requirements required = privilege::none) {
        return bind_member_method<false, R, R(T::*)(Args...), Args...>(name, method, std::move(required));
    }

    // Add const method binding
    template<typename R, typename... Args, typename Requirements = privilege>
    dynamic_binder& method(const std::string& name, R(T::*method)(Args...) const, Requirements required = privilege::none) {
        return bind_member_method<true, R, R(T::*)(Args...) const, Args...>(name, method, std::move(required));
    }

    // Add lambda/callable method binding
    // Supports: .method("setText", [](Button& self, const std::string& text) { self.setText(text); })
    // Note: First parameter can be a reference to self for accessing the object
    template<typename Callable, typename Requirements = privilege>
    dynamic_binder& method(const std::string& name, Callable&& callable, Requirements required = privilege::none) {
        // Detect accidental use of member data pointers - use property() instead
        static_assert(!std::is_member_object_pointer_v<std::decay_t<Callable>>,
            "Member data pointers cannot be passed to method(). Use property() instead.");

        // Use function_traits to determine the signature
        using traits = detail::function_traits<std::decay_t<Callable>>;
        using args_tuple = typename traits::argument_types;
        
        constexpr bool has_self_param = first_param_is_self<args_tuple>();
        using CallableType = std::decay_t<Callable>;
        if constexpr (!has_self_param) {
            if constexpr (second_param_is_self<args_tuple>()) {
                // C++'s free op(L, const T&) bound for T: the reflected operator `5 - t` reaches when 5 has none for t
                if (binary_operator_token(name)) {
                    return method(engine::reflected_operator_name(name), reflected<typename traits::return_type>(
                        std::make_shared<CallableType>(std::forward<Callable>(callable)), static_cast<args_tuple*>(nullptr)), required);
                }
            }
            // a callable without a T& / const T& first parameter is a method that ignores its receiver
            return method(name, receiver_ignoring<typename traits::return_type>(
                std::make_shared<CallableType>(std::forward<Callable>(callable)), static_cast<args_tuple*>(nullptr)), required);
        } else {
            using R = typename traits::return_type;
            constexpr size_t script_arity = traits::arity - 1;
            constexpr uint64_t writeback = detail::tuple_writeback_mask<args_tuple, 1>::value;
            constexpr bool lane_shaped = self_lane_params<args_tuple>(std::make_index_sequence<script_arity>{}) &&
                                         (lane_result<R>() || std::is_same_v<std::remove_reference_t<R>, T>);
            // the one state the ordinary call and the direct lane both read (a stateful functor's state is shared)
            std::shared_ptr<const void> state;
            if constexpr (stateless_callable<CallableType>()) {
                state = std::make_shared<const detail::stateless_call_state>(detail::stateless_call_state{ &engine_, class_def_->get_type_id() });
            } else {
                state = std::make_shared<const detail::callable_call_state<CallableType>>(
                    detail::callable_call_state<CallableType>{ std::forward<Callable>(callable), &engine_, class_def_->get_type_id() });
            }
            const bool lane = lane_shaped && lane_unguarded(required);
            class_def_->note_host_overload(engine_.symbolize(name), script_arity, !std::is_same_v<std::tuple_element_t<0, args_tuple>, T&>);

            // the C++ parameter signature after the self parameter, so same-arity lambda overloads resolve by argument type
            class_def_->add_method(name, writeback_tagged<writeback>(engine_.guard_function(detail::host_method_call{ &callable_method_call<R, args_tuple, CallableType>, state }, std::move(required))), script_arity,
                param_sig_from_tuple<args_tuple, 1, script_arity>(), result_info<R>());
            if constexpr (lane_shaped) {
                if (lane) {
                    class_def_->add_native_lane(engine_.symbolize(name), &self_lane<R, args_tuple, CallableType, script_arity>, std::move(state),
                                                &self_lane_exact<args_tuple, script_arity>);
                    return *this;
                }
            }
            class_def_->withdraw_native_lane(engine_.symbolize(name));
            return *this;
        }
    }

    template<typename Tuple>
    static constexpr bool first_param_is_self() {
        if constexpr (std::tuple_size_v<Tuple> == 0) {
            return false;
        } else {
            using First = std::tuple_element_t<0, Tuple>;
            return std::is_same_v<First, T&> || std::is_same_v<First, const T&>;
        }
    }

    template<typename Tuple>
    static constexpr bool first_param_is_mutable_self() {
        if constexpr (std::tuple_size_v<Tuple> == 0) {
            return false;
        } else {
            return std::is_same_v<std::tuple_element_t<0, Tuple>, T&>;
        }
    }

    template<typename Tuple>
    static constexpr bool second_param_is_self() {
        if constexpr (std::tuple_size_v<Tuple> != 2) {
            return false;
        } else {
            using Second = std::tuple_element_t<1, Tuple>;
            return std::is_same_v<Second, T&> || std::is_same_v<Second, const T&> || std::is_same_v<Second, T>;
        }
    }

    template<typename R, typename C, typename Left, typename Self>
    static auto reflected(std::shared_ptr<C> callable, std::tuple<Left, Self>*) {
        using SelfParam = std::conditional_t<std::is_same_v<Self, T&>, T&, const T&>;
        return [callable](SelfParam self, Left left) -> R { return (*callable)(std::forward<Left>(left), self); };
    }

    static bool binary_operator_token(std::string_view name) {
        static constexpr std::string_view tokens[] = {"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "<", "<=", ">", ">=", "==", "!=", "<=>"};
        for (std::string_view t : tokens) { if (name == t) { return true; } }
        return false;
    }

    template<typename R, typename C, typename... Args>
    static auto receiver_ignoring(std::shared_ptr<C> callable, std::tuple<Args...>*) {
        return [callable](const T&, Args... args) -> R { return (*callable)(std::forward<Args>(args)...); };
    }
    
    template<typename Callable>
    dynamic_binder& method(const std::string& name, Callable&& callable, std::initializer_list<std::string_view> names) {
        return method(name, std::forward<Callable>(callable), std::span(names.begin(), names.size()));
    }

    // A static method marshals exactly like engine::add_function; the class adds the receiver-free call and its arity text
    template<typename Callable>
    dynamic_binder& static_method(const std::string& name, Callable&& callable) {
        return static_method(name, std::forward<Callable>(callable), function_options{});
    }
    template<typename Callable>
    dynamic_binder& static_method(const std::string& name, Callable&& callable, privilege required) {
        return static_method(name, std::forward<Callable>(callable), function_options{required});
    }
    template<typename Callable>
    dynamic_binder& static_method(const std::string& name, Callable&& callable, function_options options) {
        static_assert(!std::is_member_pointer_v<std::decay_t<Callable>>,
            "Member pointers cannot be passed to static_method(). Use method() or property() instead.");
        binding_traits::check_binding_validity<Callable>(name);
        using traits = detail::function_traits<std::decay_t<Callable>>;
        constexpr uint64_t writeback = detail::tuple_writeback_mask<typename traits::argument_types>::value;
        class_def_->add_static_method(name, writeback_tagged<writeback>(engine_.guard_function(make_function_binder(std::forward<Callable>(callable), &engine_).bind(), options.required)), traits::arity);
        if (options.nodiscard) { engine_.nodiscard_function(class_name_ + "." + name); }
        if (options.serial) { class_def_->mark_serial_static_method(engine_.symbolize(name)); }
        return *this;
    }

    // Set class version
    dynamic_binder& version(uint32_t v) {
        serialization_metadata_.current_version = v;
        return *this;
    }

    // A member whose class type is not registered yet is recorded; the engine's first load reports those still missing
    template<typename P>
    dynamic_binder& property(const std::string& name, P T::*member) {
        return property_impl<P, true>(name, member);
    }

    // A property scripts reach and the class's binding record leaves out, as a property_owner's leaves out a JAI_TRANSIENT_PROPERTY
    template<typename P>
    dynamic_binder& transient_property(const std::string& name, P T::*member) {
        return property_impl<P, false>(name, member);
    }

    template<typename Getter, typename Setter>
    dynamic_binder& transient_property(const std::string& name, Getter&& getter, Setter&& setter) {
        return accessor_property<false>(name, std::forward<Getter>(getter), std::forward<Setter>(setter));
    }

private:
    dynamic_binder& apply_method_options(const std::string& name, bool is_const, function_options options) {
        if (options.nodiscard) { engine_.nodiscard_function(class_name_ + "." + name); }
        if (options.serial) {
            const uint64_t name_id = engine_.symbolize(name);
            const serial_overloads* marked = class_def_->serial_method(name_id);
            if (is_const || !marked) { class_def_->mark_serial_method(name_id, is_const ? serial_overloads::every : serial_overloads::non_const); }
        }
        return *this;
    }

    template<typename P>
    using property_state = detail::member_call_state<sizeof(P T::*)>;

    // A binding record's member, written and read in place in the object
    template<typename P>
    static void save_member_record(const void* state, const char* name, const void* object, serialization::any_archive_writer& ar) {
        const auto member = std::bit_cast<P T::*>(static_cast<const property_state<P>*>(state)->method);
        const P& value = static_cast<const T*>(object)->*member;
        ar.dispatch([name, &value](auto& archive) { archive.serialize_deferred(name, value); });
    }

    template<typename P>
    static void load_member_record(const void* state, void* object, serialization::any_archive_reader& ar) {
        const auto member = std::bit_cast<P T::*>(static_cast<const property_state<P>*>(state)->method);
        P& value = static_cast<T*>(object)->*member;
        ar.dispatch([&value](auto& archive) { archive.read_custom_deferred(value); });
    }

    template<typename Getter, typename Setter>
    struct accessor_record_state {
        Getter getter;
        Setter setter;
    };

    template<typename Getter>
    using accessor_value_t = std::remove_cvref_t<std::invoke_result_t<const Getter&, T&>>;

    // A binding record's accessor pair: the getter's value is written; a load reads over it and hands it to the setter
    template<typename Getter, typename Setter>
    static void save_accessor_record(const void* state, const char* name, const void* object, serialization::any_archive_writer& ar) {
        const auto& accessors = *static_cast<const accessor_record_state<Getter, Setter>*>(state);
        const accessor_value_t<Getter> value = std::invoke(accessors.getter, *const_cast<T*>(static_cast<const T*>(object)));
        ar.dispatch([name, &value](auto& archive) { archive.serialize_deferred(name, value); });
    }

    template<typename Getter, typename Setter>
    static void load_accessor_record(const void* state, void* object, serialization::any_archive_reader& ar) {
        const auto& accessors = *static_cast<const accessor_record_state<Getter, Setter>*>(state);
        T& target = *static_cast<T*>(object);
        accessor_value_t<Getter> value = std::invoke(accessors.getter, target);
        ar.dispatch([&value](auto& archive) { archive.read_custom_deferred(value); });
        std::invoke(accessors.setter, target, value);
    }

    template<typename Getter, typename Setter>
    static constexpr bool accessors_archive() {
        if constexpr (std::is_null_pointer_v<Setter> || !std::is_invocable_v<const Getter&, T&>) {
            return false;
        } else {
            using value_type = accessor_value_t<Getter>;
            return std::is_copy_constructible_v<Getter> && std::is_copy_constructible_v<Setter> && !std::is_void_v<value_type> &&
                std::is_constructible_v<value_type, std::invoke_result_t<const Getter&, T&>> &&
                std::is_invocable_v<const Setter&, T&, value_type&>;
        }
    }

    template<typename Base>
    static void* upcast_record(void* object) {
        return static_cast<Base*>(static_cast<T*>(object));
    }

    template<typename P>
    static checked_result<script_value> property_get_call(const void* state, const std::vector<script_value>& args) {
        const property_state<P>& st = *static_cast<const property_state<P>*>(state);
        if (args.empty()) {
            throw runtime_error("Property getter called without 'this' object");
        }
        const auto member = std::bit_cast<P T::*>(st.method);
        engine* engine_ptr = st.eng;
        T* cpp_obj = detail::extract_cpp_object_ptr<T>(args[0], st.native_type_id);

        // Special handling for std::vector<T> - wrap in bound_cpp_vector for zero-copy access
        if constexpr (is_specialization_v<P, std::vector>) {
            using element_type = typename P::value_type;
            auto wrapper = std::make_shared<bound_cpp_vector<element_type>>(
                cpp_obj->*member, engine_ptr);
            return engine_ptr->make_value_object(wrapper);
        }
        else if constexpr (dynamic_binder_validation::is_signal_type<P>::value) {
            // Typed per-signature bindings (bind_signal_type) take precedence
            // when present; otherwise the generic "Signal" view serves the
            // member ceremony-free through the erased connect table.
            if (engine_ptr->template has_registered_class<P>()) {
                return convert_reference_with_registry(cpp_obj->*member, engine_ptr);
            }
            return detail::make_signal_view(engine_ptr, make_signal_script_ops(cpp_obj->*member));
        }
        else if constexpr (std::is_class_v<P> &&
                           !math_adapted<P> &&
                           !std::is_same_v<P, std::string> &&
                           !detail::is_script_value_v<P> &&
                           !dynamic_binder_validation::is_std_container<P>::value &&
                           !dynamic_binder_validation::is_std_function<P>::value &&
                           !is_specialization_v<P, std::shared_ptr> &&
                           !is_specialization_v<P, std::weak_ptr> &&
                           !is_specialization_v<P, std::unique_ptr>) {
            // Registered class members bind by reference (cpp_bound) so mutation and
            // signal connect act on the live member, not a copy (which non-copyables
            // like signal_emitter would reject outright); unregistered copyables copy.
            return convert_reference_with_registry(cpp_obj->*member, engine_ptr);
        }
        else {
            return detail::value_converter<P>::to(cpp_obj->*member, engine_ptr);
        }
    }

    template<typename P>
    static checked_result<script_value> property_set_call(const void* state, const std::vector<script_value>& args) {
        const property_state<P>& st = *static_cast<const property_state<P>*>(state);
        if (args.size() < 2) {
            throw runtime_error("Property setter requires 'this' and value");
        }
        const auto member = std::bit_cast<P T::*>(st.method);
        engine* engine_ptr = st.eng;
        T* cpp_obj = detail::extract_cpp_object_ptr<T>(args[0], st.native_type_id);

        if constexpr (detail::is_script_value_v<P>) {
            script_value& held = cpp_obj->*member;
            if (held.is_reference()) {
                auto stored = detail::store_through_held_reference(held, args[1], engine_ptr);
                if (!stored) { return stored.error_value(); }
            } else {
                held = args[1].clone();
            }
        } else if constexpr (dynamic_binder_validation::is_std_function<P>::value) {
            // Script lambdas assign into C++ callback hooks (spawn/update pattern)
            cpp_obj->*member = detail::value_converter<P>::from(args[1], engine_ptr);
        } else if constexpr (std::is_same_v<P, char>) {
            cpp_obj->*member = detail::value_converter<char>::from(args[1], engine_ptr);   // a one-character string, as a char field takes
        } else if constexpr (std::is_enum_v<P>) {
            cpp_obj->*member = detail::value_converter<P>::from(args[1], engine_ptr);   // the int its getter hands out
        } else {
            cpp_obj->*member = args[1].as<P>();
        }
        return script_value(std::monostate{}, engine_ptr); // null
    }

    template<typename P>
    static void* property_member_address(const void* state, const script_value& receiver) {
        const property_state<P>& st = *static_cast<const property_state<P>*>(state);
        T* cpp_obj = detail::extract_cpp_object_ptr<T>(receiver, st.native_type_id);
        return cpp_obj ? static_cast<void*>(&(cpp_obj->*std::bit_cast<P T::*>(st.method))) : nullptr;
    }

    template<typename P, bool Archived>
    dynamic_binder& property_impl(const std::string& name, P T::*member) {
        // Signal members may hand out the generic view at read time — make
        // sure its type exists before any getter can run.
        if constexpr (dynamic_binder_validation::is_signal_type<P>::value) {
            detail::ensure_signal_view_type_registered(engine_);
        }
        if constexpr (dynamic_binder_validation::needs_registration_check<P>()) {
            engine_.require_bound_type(class_name_, name, typeid(typename dynamic_binder_validation::get_validation_type<P>::type));
        }

        // Register the property as a special field that knows how to access the C++ member
        class_def_->add_field(name, script_value(std::monostate{}, &engine_)); // Register field name

        // Register serialization metadata
        serialization::property_metadata prop_meta;
        prop_meta.name = name;
        prop_meta.type = engine_.get_type_info_for_cpp_type<P>();
        prop_meta.read_only = !std::is_copy_assignable_v<P>;

        using State = property_state<P>;
        auto state = std::make_shared<const State>(State{ std::bit_cast<std::array<std::byte, sizeof(P T::*)>>(member), &engine_, class_def_->get_type_id() });
        // a const member cannot load back, and a signal or callback is a connect surface, not data: the record leaves them out
        if constexpr (Archived && serialization::binding_record_v<T> && !std::is_const_v<P> &&
                      !dynamic_binder_validation::is_signal_type<P>::value && !dynamic_binder_validation::is_std_function<P>::value) {
            prop_meta.archive = { &save_member_record<P>, &load_member_record<P>, state };
        }
        serialization_metadata_.properties.push_back(std::move(prop_meta));

        // Add a special method that handles property access
        // The interpreter's visitMemberExpr will need to check for these
        class_def_->add_method("_get_" + name, detail::host_method_call{ &property_get_call<P>, state });
        class_def_->register_property_getter(engine_.symbolize(name), engine_.symbolize("_get_" + name));

        // Only create setter for copy-assignable types
        if constexpr (std::is_copy_assignable_v<P>) {
            class_def_->add_method("_set_" + name, detail::host_method_call{ &property_set_call<P>, state });

            // Register field_id -> setter_id mapping for fast runtime lookup
            uint64_t field_id = engine_.symbolize(name);
            uint64_t setter_id = engine_.symbolize("_set_" + name);
            class_def_->register_property_setter(field_id, setter_id);
        } else {
            detail::register_read_only_setter(engine_, *class_def_, name);
        }

        if constexpr (detail::host_member_value<P> && !std::is_const_v<P>) {
            class_def_->register_host_member_lane(engine_.symbolize(name), detail::make_host_member_lane<T, P>(member, class_def_->get_type_id()));
        } else {
            class_def_->register_host_member_lane(engine_.symbolize(name), nullptr);
        }

        // A box-decodable member binds BY REFERENCE as the member itself: record its address
        // accessor beside the getter/setter
        if constexpr (script_value::cpp_bound_encoding<P>::capable) {
            using encoding = script_value::cpp_bound_encoding<P>;
            class_def_->register_property_member(engine_.symbolize(name), class_definition::cpp_member_accessor{
                detail::host_member_address{ &property_member_address<P>, state },
                encoding::size_and_sign, encoding::semantic_index});
        }

        // Register bound_cpp_vector<T> if this property is a std::vector
        if constexpr (is_specialization_v<P, std::vector>) {
            using element_type = typename P::value_type;
            std::string wrapper_type_name = std::string("bound_cpp_vector<") + typeid(element_type).name() + ">";

            // Check if already registered
            auto existing = engine_.get_class_definition_by_type(std::type_index(typeid(bound_cpp_vector<element_type>)));
            if (!existing) {
                // Register bound_cpp_vector<element_type> with array-like methods
                dynamic_binder<bound_cpp_vector<element_type>>(engine_, wrapper_type_name)
                    .method("size", &bound_cpp_vector<element_type>::size)
                    .method("empty", &bound_cpp_vector<element_type>::empty)
                    .method("clear", &bound_cpp_vector<element_type>::clear)
                    .method("push_back", static_cast<void(bound_cpp_vector<element_type>::*)(const element_type&)>(&bound_cpp_vector<element_type>::push_back))
                    .method("push", static_cast<void(bound_cpp_vector<element_type>::*)(const element_type&)>(&bound_cpp_vector<element_type>::push_back)) // Alias
                    .method("pop_back", &bound_cpp_vector<element_type>::pop_back)
                    .method("pop", &bound_cpp_vector<element_type>::pop_back) // Alias
                    .method("front", static_cast<element_type&(bound_cpp_vector<element_type>::*)()>(&bound_cpp_vector<element_type>::front))
                    .method("back", static_cast<element_type&(bound_cpp_vector<element_type>::*)()>(&bound_cpp_vector<element_type>::back))
                    .method("at", static_cast<element_type&(bound_cpp_vector<element_type>::*)(size_t)>(&bound_cpp_vector<element_type>::at))
                    .method("[]", static_cast<element_type&(bound_cpp_vector<element_type>::*)(size_t)>(&bound_cpp_vector<element_type>::operator[]))
                    .build();
                engine_.get_class_definition_by_type(std::type_index(typeid(bound_cpp_vector<element_type>)))->set_archive_form(&bound_vector_archive_form<element_type>);
            }
        }

        return *this;
    }

    // A bound vector property archives as its elements, as the array a script assigns to it
    template<typename E>
    static script_value bound_vector_archive_form(const script_value& object, const class_definition& cls, engine* eng) {
        const bound_cpp_vector<E>* wrapper = detail::extract_cpp_object_ptr<bound_cpp_vector<E>>(object, cls.get_type_id());
        if (!wrapper || !wrapper->get_vector()) {
            return script_value(std::monostate{}, eng);
        }
        return detail::value_converter<std::vector<E>>::to(*wrapper->get_vector(), eng);
    }

public:
    // Add property with getter/setter (supports both lambdas and member function pointers)
    template<typename Getter, typename Setter>
    dynamic_binder& property(const std::string& name, Getter&& getter, Setter&& setter) {
        return accessor_property<true>(name, std::forward<Getter>(getter), std::forward<Setter>(setter));
    }

private:
    template<bool Archived, typename Getter, typename Setter>
    dynamic_binder& accessor_property(const std::string& name, Getter&& getter, Setter&& setter) {
        // Register the property as a field
        class_def_->add_field(name, script_value(std::monostate{}, &engine_)); // Register field name

        // Register serialization metadata
        constexpr bool is_read_only = std::is_null_pointer_v<std::decay_t<Setter>> || std::is_same_v<std::decay_t<Setter>, std::nullptr_t>;

        serialization::property_metadata prop_meta;
        prop_meta.name = name;
        prop_meta.read_only = is_read_only;

        // Try to deduce type from getter return type
        if constexpr (std::is_member_function_pointer_v<std::decay_t<Getter>>) {
            using getter_traits = detail::function_traits<std::decay_t<Getter>>;
            using return_type = typename getter_traits::return_type;
            prop_meta.type = engine_.get_type_info_for_cpp_type<std::decay_t<return_type>>();
        } else {
            // For lambdas, we can't easily deduce the type at compile time
            // Leave type as nullptr - it will be determined at runtime if needed
            prop_meta.type = nullptr;
        }

        using archived_getter = std::decay_t<Getter>;
        using archived_setter = std::decay_t<Setter>;
        if constexpr (Archived && serialization::binding_record_v<T> && accessors_archive<archived_getter, archived_setter>()) {
            using State = accessor_record_state<archived_getter, archived_setter>;
            prop_meta.archive = { &save_accessor_record<archived_getter, archived_setter>, &load_accessor_record<archived_getter, archived_setter>,
                std::make_shared<const State>(State{ getter, setter }) };
        }
        serialization_metadata_.properties.push_back(std::move(prop_meta));

        // Add getter method
        class_def_->add_method("_get_" + name, [getter = std::forward<Getter>(getter), engine_ptr = &engine_, native_type_id = class_def_->get_type_id()](const std::vector<script_value>& args) -> script_value {
            if (args.empty()) {
                throw runtime_error("Property getter called without 'this' object");
            }

            T* cpp_obj = detail::extract_cpp_object_ptr<T>(args[0], native_type_id);

            // Check if getter is a member function pointer
            if constexpr (std::is_member_function_pointer_v<std::decay_t<Getter>>) {
                // Call member function pointer: (obj->*getter)()
                return detail::value_converter<decltype((cpp_obj->*getter)())>::to((cpp_obj->*getter)(), engine_ptr);
            } else {
                // Call lambda: getter(*cpp_obj)
                return detail::value_converter<decltype(getter(*cpp_obj))>::to(getter(*cpp_obj), engine_ptr);
            }
        });
        class_def_->register_property_getter(engine_.symbolize(name), engine_.symbolize("_get_" + name));
        class_def_->register_host_member_lane(engine_.symbolize(name), nullptr);

        // Add setter method - use different implementations based on setter type
        if constexpr (is_read_only) {
            detail::register_read_only_setter(engine_, *class_def_, name);
            return *this;
        } else if constexpr (std::is_member_function_pointer_v<std::decay_t<Setter>>) {
            // Member function pointer setter
            class_def_->add_method("_set_" + name, [setter = std::forward<Setter>(setter), engine_ptr = &engine_, native_type_id = class_def_->get_type_id()](const std::vector<script_value>& args) -> script_value {
                if (args.size() < 2) {
                    throw runtime_error("Property setter requires 'this' and value");
                }

                T* cpp_obj = detail::extract_cpp_object_ptr<T>(args[0], native_type_id);

                using setter_traits = detail::function_traits<std::decay_t<Setter>>;
                using value_type = std::tuple_element_t<0, typename setter_traits::argument_types>;
                using read_type = std::conditional_t<math_adapted<std::remove_cvref_t<value_type>>, std::remove_cvref_t<value_type>, value_type>;   // a math value has no host object to alias
                auto value = args[1].as<read_type>();
                (cpp_obj->*setter)(value);
                return script_value(std::monostate{}, engine_ptr);
            });
        } else {
            // Lambda setter
            class_def_->add_method("_set_" + name, [setter = std::forward<Setter>(setter), engine_ptr = &engine_, native_type_id = class_def_->get_type_id()](const std::vector<script_value>& args) -> script_value {
                if (args.size() < 2) {
                    throw runtime_error("Property setter requires 'this' and value");
                }

                T* cpp_obj = detail::extract_cpp_object_ptr<T>(args[0], native_type_id);

                using setter_traits = detail::function_traits<std::decay_t<Setter>>;
                using value_type = std::tuple_element_t<1, typename setter_traits::argument_types>;
                using read_type = std::conditional_t<math_adapted<std::remove_cvref_t<value_type>>, std::remove_cvref_t<value_type>, value_type>;   // a math value has no host object to alias
                auto value = args[1].as<read_type>();
                setter(*cpp_obj, value);
                return script_value(std::monostate{}, engine_ptr);
            });
        }

        // Register field_id -> setter_id mapping for fast runtime lookup
        uint64_t field_id = engine_.symbolize(name);
        uint64_t setter_id = engine_.symbolize("_set_" + name);
        class_def_->register_property_setter(field_id, setter_id);

        return *this;
    }

public:
    // Add static property binding - for simple variable access
    // the live variable: script reads and stores reach *static_var through _get_/_set_ (no copy taken at registration)
    template<typename P>
    dynamic_binder& static_property(const std::string& name, P* static_var) {
        return static_property(name, [static_var]() -> P { return *static_var; }, [static_var](const P& value) { *static_var = value; });
    }
    
    // Add static property with lambda getter/setter
    template<typename Getter, typename Setter>
    dynamic_binder& static_property(const std::string& name, Getter&& getter, Setter&& setter) {
        // Add getter method
        std::string getter_name = "_get_" + name;
        uint64_t getter_id = engine_.symbolize(getter_name);
        class_def_->add_static_method(getter_id, [getter = std::forward<Getter>(getter), engine_ptr = &engine_](const std::vector<script_value>&) -> script_value {
            return detail::value_converter<decltype(getter())>::to(getter(), engine_ptr);
        });

        using setter_traits = detail::function_traits<std::decay_t<Setter>>;
        static_assert(setter_traits::arity == 1, "static_property: the setter takes the one new value");
        using SetterArg = std::decay_t<std::tuple_element_t<0, typename setter_traits::argument_types>>;
        class_def_->add_static_method(engine_.symbolize("_set_" + name), [setter = std::forward<Setter>(setter), engine_ptr = &engine_](const std::vector<script_value>& args) -> script_value {
            if (args.size() != 1) {
                throw_text_with_count("Static property setter expects 1 argument, got ", args.size());
            }
            setter(detail::value_converter<SetterArg>::from(args[0], engine_ptr));
            return script_value(std::monostate{}, engine_ptr);
        });

        return *this;
    }
    
    // Add base class - establishes inheritance relationship (appends, doesn't clear)
    // This enables:
    // 1. Method inheritance (derived can call base methods)
    // 2. Polymorphic copy support
    // Can be chained: .base_class<A>().base_class<B>() for multiple inheritance
    template<typename Base>
    dynamic_binder& base_class() {
        static_assert(std::is_base_of_v<Base, T>,
                      "Specified type is not a base class of this class");

        // Set up inheritance relationship - use type_index lookup instead of typeid name
        auto base_def = engine_.get_class_definition_by_type(std::type_index(typeid(Base)));
        if (!base_def) {
            // The property_owner declaration IS the inheritance truth: an unregistered
            // property_owner base is materialized NOW (recursively, through its own
            // auto_bind) so the engine projection of the chain is always complete —
            // methods, schema properties, and signal views all resolve without any
            // registration-order rule. A later explicit registration ADOPTS this
            // definition (see the constructor) rather than competing with it. The name
            // comes from the shared ladder (self-owned jai_type_name pin, else the bare
            // unqualified type name); no portable name, or the name already belonging
            // to a DIFFERENT type, declines to today's deferred link.
            if constexpr (auto_bind_concepts::is_property_owner<Base>) {
                constexpr auto derived_name = detail::declared_or_derived_type_name<Base>();
                if constexpr (!derived_name.empty()) {
                    const std::string base_name{derived_name};
                    if (!engine_.get_class_definition(base_name)) {
                        dynamic_binder<Base> auto_bound(engine_, base_name);
                        auto_bound.class_def_->set_auto_registered(true);
                        auto_bound.auto_bind();
                        auto_bound.build();
                        base_def = engine_.get_class_definition_by_type(std::type_index(typeid(Base)));
                    }
                }
            }
        }
        if (base_def) {
            // Use add_parent to append (validates for diamond inheritance)
            // add_parent is idempotent (returns true if already registered)
            // but returns false for true diamond inheritance
            if (!class_def_->add_parent(base_def)) {
                throw std::runtime_error("Diamond inheritance detected: class '" + class_name_ +
                    "' would have multiple paths to the same base class");
            }
        } else {
            // Base not registered yet (registrar order is arbitrary): link when it arrives.
            // Silently skipping here orphaned the chain — inherited methods and signal
            // views vanished whenever the derived registrar happened to run first.
            engine_.defer_base_link(std::type_index(typeid(Base)), class_def_);
        }

		class_def_->template add_cpp_base_cast<T, Base>();

        // Record the serialization assignability edge (what property_owner does) so a
        // shared_ptr<Base> with $type Derived passes the polymorphic load type check.
        serialization::register_relation<T, Base>{};

        // a base with a route of its own hands it to T, so a binding record's bases are binding records
        if constexpr (serialization::binding_record_v<T>) {
            serialization_metadata_.bases.push_back({ std::type_index(typeid(Base)), &upcast_record<Base> });
        }

        return *this;
    }
    
private:

public:
    // Register a deserialization factory for non-default constructors
    // Supports three signatures automatically detected via template metaprogramming:
    // 1. Archive-only: [](serialization::archive_reader& archive) -> std::shared_ptr<T>
    // 2. Context-only: [](ContextType* ctx) -> std::shared_ptr<T>
    // 3. Context + archive: [](ContextType* ctx, serialization::archive_reader& archive) -> std::shared_ptr<T>
    template<typename ContextType = void, typename FactoryFunc>
    dynamic_binder& deserialization_factory(FactoryFunc&& factory) {
        using namespace serialization;

        // Detect factory signature using function traits
        using factory_traits = detail::function_traits<std::decay_t<FactoryFunc>>;
        constexpr size_t arg_count = factory_traits::arity;

        // Dispatch to appropriate helper based on signature
        if constexpr (arg_count == 1) {
            using arg0_type = std::tuple_element_t<0, typename factory_traits::argument_types>;

            if constexpr (std::is_same_v<std::decay_t<arg0_type>, serialization::any_archive_reader>) {
                serialization_metadata_.custom_construct =
                    dynamic_binder_detail::make_archive_only_factory<T>(
                        std::forward<FactoryFunc>(factory), class_name_, &engine_);
            } else if constexpr (!std::is_void_v<ContextType>) {
                // Context-only factory: [](ContextType* ctx) -> std::shared_ptr<T>
                serialization_metadata_.custom_construct =
                    dynamic_binder_detail::make_context_only_factory<T, ContextType>(
                        std::forward<FactoryFunc>(factory), class_name_, &engine_);
            } else {
                // Context-only factory requires ContextType to be specified
                static_assert(!std::is_void_v<ContextType>,
                    "Context-only factory requires a non-void ContextType template parameter. "
                    "Use deserialization_factory<YourContextType>([](YourContextType* ctx) { ... })");
            }
        } else if constexpr (arg_count == 2) {
            if constexpr (!std::is_void_v<ContextType>) {
                serialization_metadata_.custom_construct =
                    dynamic_binder_detail::make_context_archive_factory<T, ContextType>(
                        std::forward<FactoryFunc>(factory), class_name_, &engine_);
            } else {
                static_assert(!std::is_void_v<ContextType>,
                    "Context+archive factory requires a non-void ContextType template parameter. "
                    "Use deserialization_factory<YourContextType>([](YourContextType* ctx, serialization::any_archive_reader& ar) { ... })");
            }
        } else {
            static_assert(arg_count <= 2, "Deserialization factory must take 1 or 2 arguments");
        }

        return *this;
    }

    // Register post-deserialization hook for migration and data transformation
    //
    // The hook receives the version number that was serialized, which can be used
    // for migration logic. You can use either signature:
    //
    // Examples:
    //   // Without version parameter (simple computed values)
    //   .post_load_hook([](MyClass& self) {
    //       self.computed_field = self.width * self.height;
    //   })
    //
    //   // With version parameter (for migration)
    //   .post_load_hook([](MyClass& self, int version) {
    //       if (version < 2) {
    //           // Migrate from v1 to v2
    //           self.new_field = self.compute_from_old_fields();
    //       }
    //   })
    template<typename Callable>
    dynamic_binder& post_load_hook(Callable&& callable) {
        if constexpr (detail::function_traits<std::decay_t<Callable>>::arity == 1) {
            // every load hands the hook the saved version
            return method("post_load", [hook = std::forward<Callable>(callable)](T& self, int) { hook(self); });
        } else {
            return method("post_load", std::forward<Callable>(callable));
        }
    }

    // ============================================================================
    // transparent_wrapper() - Mark this type as a transparent wrapper
    // ============================================================================
    //
    // When an operation (method call, operator, etc.) is not found on the wrapper,
    // the interpreter will call the unwrap function and retry the operation on
    // the underlying value.
    //
    // This enables patterns like:
    //   player.score + 5        // score is observable_property<int>, forwards + to int
    //   player.score.on_change  // on_change is on the wrapper itself
    //   player.cat.meow()       // cat is observable_property<Cat>, forwards meow() to Cat
    //
    // The unwrap function receives a reference to the wrapper object and returns
    // a script_value containing the underlying value.
    //
    // Usage:
    //   dynamic_binder<observable_property<int>>(eng, "observable_int")
    //       .transparent_wrapper([](observable_property<int>& self) { return self.get(); })
    //       .method("on_change", ...)
    //       .build();
    //
    template<typename UnwrapFn>
    dynamic_binder& transparent_wrapper(UnwrapFn&& unwrap_fn) {
        engine* eng = &engine_;

        // Create the unwrap function that works with script_value
        auto wrapped_fn = [unwrap_fn = std::forward<UnwrapFn>(unwrap_fn), eng](script_value& wrapper_sv, engine* e) -> script_value {
            // Extract the C++ object from the script_value
            if (!wrapper_sv.is_object()) {
                return script_value(std::monostate{}, e);
            }

            auto holder = wrapper_sv.get_object_holder();
            if (!holder) {
                return script_value(std::monostate{}, e);
            }

            // Get the typed pointer - check if it's a class_instance wrapper
            T* cpp_ptr = nullptr;
            if (holder->is_instance()) {
                auto instance = holder->instance_handle();
                if (instance) {
                    cpp_ptr = instance->get_cpp_object_as<T>().get();
                }
            } else {
                auto typed_ptr = std::static_pointer_cast<T>(holder->host_object());
                cpp_ptr = typed_ptr.get();
            }

            if (!cpp_ptr) {
                return script_value(std::monostate{}, e);
            }

            // Call the unwrap function and convert result to script_value
            auto underlying_value = unwrap_fn(*cpp_ptr);
            return script_value_from_cpp(underlying_value, e);
        };

        class_def_->set_unwrap_function(std::move(wrapped_fn));
        return *this;
    }

    // The engine this binder registers into - configure lambdas need it for
    // bind_signal_type/globals (Services may not carry the engine yet at bind time).
    engine& bound_engine() const { return engine_; }

    // ============================================================================
    // auto_bind() - Apply auto-detection of base classes and common methods
    // ============================================================================
    //
    // Usage:
    //   dynamic_binder<Player>(eng, "Player")
    //       .auto_bind()                       // Applies base classes + common methods
    //       .method("custom", &Player::custom); // Add more custom bindings
    //
    //   dynamic_binder<Player>(eng, "Player")
    //       .auto_bind(bind_mode::hierarchy);  // Just base classes, no auto methods
    //
    dynamic_binder& auto_bind(bind_mode mode = bind_mode::all) {
        // Apply base classes from _jai_base_types if present
        if constexpr (auto_bind_concepts::has_base_types<T>) {
            apply_base_classes_from_tuple(std::type_identity<typename T::_jai_base_types>{});
        }

        // Bind properties from type_registry if T is a property_owner
        if constexpr (auto_bind_concepts::is_property_owner<T>) {
            // The schema's base chain otherwise registers on FIRST CONSTRUCTION —
            // registrar-time binding must see inherited properties before any instance
            T::register_inheritance();
            bind_properties_from_schema();
        }

        // Apply auto-detected methods if mode is 'all'
        if (mode == bind_mode::all) {
            if constexpr (auto_bind_concepts::has_to_string<T>) {
                method("to_string", &T::to_string);
            }
            if constexpr (auto_bind_concepts::has_size<T>) {
                method("size", &T::size);
            }
            if constexpr (auto_bind_concepts::has_empty<T>) {
                method("empty", &T::empty);
            }
            if constexpr (auto_bind_concepts::has_equality<T>) {
                method("==", [](const T& a, const T& b) -> bool { return a == b; });
                method("!=", [](const T& a, const T& b) -> bool { return !(a == b); });
            }
            // copy assignment only where it cannot fail to compile; any other class binds "=" itself
            if constexpr (std::is_trivially_copy_assignable_v<T> && !std::is_empty_v<T>) {
                method("=", [](T& self, const T& other) -> T& { return self = other; });
            }
        }

        // Call T::jai_auto_bind(*this) if it exists - allows classes to register private members
        if constexpr (auto_bind_concepts::has_jai_auto_bind<T>) {
            T::jai_auto_bind(*this);
        }

        return *this;
    }

private:
    // Bind properties from type_registry schema for property_owner classes
    // This is called when T is complete, so we can directly access T's property_mgr
    void bind_properties_from_schema() {
        // Get the schema for this type (includes inherited properties)
        auto all_props = type_registry::instance().all_properties<T>();
        const detail::schema_property_access access{ &detail::schema_extract<T>, &detail::schema_find<T>, &detail::schema_manager<T>, sizeof(T) };

        for (const auto* prop_meta : all_props) {
            if (!prop_meta) continue;

            const std::string prop_name = prop_meta->name;
            const std::type_index value_type = prop_meta->value_type_id;
            const bool is_observable = prop_meta->is_observable;

            // JAI_SIGNAL_PROPERTY entries expose the generic Signal view —
            // declaring the signal is the whole registration.
            if (prop_meta->is_signal) {
                bind_signal_property_view(prop_name);
                continue;
            }

            if (prop_meta->math) {
                pending_math_properties_.push_back({ prop_name, prop_meta->math, access });
                continue;
            }
            // Non-primitive property types are bound manually.
            detail::bind_schema_primitive_property(engine_, *class_def_, serialization_metadata_,
                prop_name, value_type, is_observable, access);
        }
    }

    // JAI_SIGNAL_PROPERTY schema entries become read-only properties returning
    // the generic Signal view over the live property's erased connect table.
    // Lookup is find_reflected_property (virtual, whole chain), so inherited
    // signal properties resolve against the most-derived live object.
    void bind_signal_property_view(const std::string& prop_name) {
        detail::ensure_signal_view_type_registered(engine_);
        engine* eng = &engine_;
        auto getter = [prop_name, eng](T& self) -> script_value {
            if constexpr (auto_bind_concepts::is_property_owner<T>) {
                if (jai::property_base* base = self.find_reflected_property(prop_name)) {
                    signal_script_ops ops;
                    if (base->signal_ops(ops) && ops.valid()) {
                        return detail::make_signal_view(eng, ops);
                    }
                }
            }
            return script_value(std::monostate{}, eng);
        };
        property(prop_name, std::move(getter), nullptr);
    }

    template<typename ValueT>
    static script_value script_value_from_cpp(const ValueT& value, engine* eng) {
        return detail::script_value_from_cpp(value, eng);
    }

public:

    // Commits the binding at this point and throws what fails; without it the binding commits when the binder's lifetime
    // ends or when the engine first loads a script, whichever comes first
    void build() {
        if (!uncommitted()) return;
        leave();

        // A bound method of the same name is the class's chosen spelling (Palette's color() /
        // color(c)); the property stays reachable through it
        detail::bind_pending_math_properties(engine_, *class_def_, serialization_metadata_, pending_math_properties_);

        if constexpr (std::is_base_of_v<script_extensible, T>) {
            class_definition::script_slot_hooks hooks;
            hooks.attach = [](void* object, const weaker_ptr<class_instance>& layer) {
                static_cast<T*>(object)->jai_script_slot().attach_script_layer(layer);
            };
            hooks.own = [](void* object, const strong_ptr<class_instance>& layer) {
                static_cast<T*>(object)->jai_script_slot().own_script_layer(layer);
            };
            hooks.detach = [](void* object) { static_cast<T*>(object)->jai_script_slot().detach_script_layer(); };
            hooks.read = [](const void* object) { return static_cast<const T*>(object)->jai_script_slot().script_layer(); };
            hooks.slot = [](const void* object) -> const script_extensible* { return &static_cast<const T*>(object)->jai_script_slot(); };
            class_def_->set_script_slot_hooks(hooks);
        }

        // Auto-register default constructor if:
        // 1. User didn't explicitly register any constructor
        // 2. Type is default constructible
        // 3. Type is not abstract
        if constexpr (!std::is_abstract_v<T> && std::is_default_constructible_v<T>) {
            if (!has_explicit_constructor_) {
                // Auto-register default constructor (with the engine when T takes one)
                engine_.add_overloaded_function(class_name_, 0, detail::host_method_call{ &native_constructor_call<>, constructor_state() });
            }
        }

        // Register automatic copy function for copyable types
        if constexpr (std::is_copy_constructible_v<T>) {
            class_def_->set_copy_function(&copy_native_object);
        }
        
        class_def_->set_cpp_backed(true);   // reflect::is_cpp_bound's class-form answer
        class_def_->set_cpp_type(&typeid(T));
        engine_.add_class<T>(class_name_, class_def_);

        // Register serialization metadata with the engine's registry (with type_index for runtime lookup)
        engine_.get_serialization_registry().register_class(class_name_, serialization_metadata_);
        if constexpr (serialization::binding_record_v<T>) {
            serialization::polymorphic_registry::try_auto_register_binding<T>();
        }

        
        auto registry = engine_.get_conversion_registry();
        auto bound = std::make_shared<detail::bound_class_conversion>(detail::bound_class_conversion{class_def_, class_name_});
        // Handle conversions also work for abstract bases; no T is constructed here.
        registry->register_entry(conversions::type_id::of<std::shared_ptr<T>>(), {&shared_from_script, &shared_to_script, &engine_, bound});

        // Value conversions need a copy; a default-constructible class replaces any earlier registration, any other keeps it
        if constexpr (!std::is_abstract_v<T> && std::is_copy_constructible_v<T>) {
            if constexpr (std::is_default_constructible_v<T>) {
                registry->register_entry(conversions::type_id::of<T>(), {&value_from_script, &value_to_script, &engine_, bound});
            } else if (!registry->template has_conversion<T>()) {
                registry->register_entry(conversions::type_id::of<T>(), {&checked_value_from_script, &value_to_script, &engine_, bound});
            }
        }

        register_container_conversions();
    }
    
private:
    // Helper to apply base classes for each type in a tuple
    // Collects all base definitions and sets them at once for efficiency
    // (single diamond check, single cache invalidation)
    // Uses std::type_identity to avoid requiring default constructors on base classes
    template<typename... Bases>
    void apply_base_classes_from_tuple(std::type_identity<std::tuple<Bases...>>) {
        if constexpr (sizeof...(Bases) == 0) {
            return;  // No bases to register
        } else if constexpr (sizeof...(Bases) == 1) {
            // Single inheritance - use base_class<> directly
            (base_class<Bases>(), ...);
        } else {
            // Multiple inheritance - collect all definitions and use set_parents()
            // This is more efficient: single diamond check instead of N checks
            std::vector<std::shared_ptr<class_definition>> parent_defs;
            parent_defs.reserve(sizeof...(Bases));

            // Collect base definitions (fold expression with static_assert validation)
            (collect_base_definition<Bases>(parent_defs), ...);

            // Set all parents at once
            if (!parent_defs.empty()) {
                if (!class_def_->set_parents(parent_defs)) {
                    throw std::runtime_error("Diamond inheritance detected: class '" + class_name_ +
                        "' would have multiple paths to the same base class");
                }
            }
        }
    }

    // Helper to collect a single base class definition
    template<typename Base>
    void collect_base_definition(std::vector<std::shared_ptr<class_definition>>& parent_defs) {
		class_def_->template add_cpp_base_cast<T, Base>();
        static_assert(std::is_base_of_v<Base, T>,
                      "Specified type is not a base class of this class");
        auto base_def = engine_.get_class_definition_by_type(std::type_index(typeid(Base)));
        if (base_def) {
            parent_defs.push_back(base_def);
        }
    }

    engine& engine_;
    std::shared_ptr<class_definition> class_def_;
    serialization::class_metadata serialization_metadata_;
    std::vector<detail::pending_math_property> pending_math_properties_;
    bool has_explicit_constructor_ = false;  // Track if user registered any constructor

    static void commit_pending(detail::pending_binder& link) {
        static_cast<dynamic_binder&>(link).build();
    }
    
    // Automatically register container conversions for type T
    void register_container_conversions() {
        if constexpr (!std::is_abstract_v<T>) {
            // Get the conversion manager for high-level API
            auto conv_mgr = engine_.get_conversion_manager();
            
            // Register vector conversions
            // Only register value-type vector if T is copyable
            if constexpr (std::is_copy_constructible_v<T>) {
                conv_mgr.template add_vector_conversion<T>();
            }
            conv_mgr.template add_vector_conversion<std::shared_ptr<T>>();

            // Register common map conversions with this type as value
            // Only register T map conversions if T is copyable and default constructible (required for std::map)
            if constexpr (std::is_copy_constructible_v<T> && std::is_default_constructible_v<T>) {
                conv_mgr.template add_map_conversion<std::string, T>();
                conv_mgr.template add_map_conversion<int, T>();
                conv_mgr.template add_map_conversion<int64_t, T>();
            }
            // shared_ptr<T> is always default constructible
            conv_mgr.template add_map_conversion<std::string, std::shared_ptr<T>>();
            conv_mgr.template add_map_conversion<int, std::shared_ptr<T>>();
            conv_mgr.template add_map_conversion<int64_t, std::shared_ptr<T>>();
            
            // Register bound array conversions for zero-copy performance
            conv_mgr.template add_bound_array_conversion<T>();
            conv_mgr.template add_bound_array_conversion<std::shared_ptr<T>>();
            
            // Register bound map conversions
            // Only register for default constructible types
            if constexpr (std::is_default_constructible_v<T>) {
                conv_mgr.template add_bound_map_conversion<std::string, T>();
                conv_mgr.template add_bound_map_conversion<int, T>();
            }
            // shared_ptr<T> is always default constructible
            conv_mgr.template add_bound_map_conversion<std::string, std::shared_ptr<T>>();
            conv_mgr.template add_bound_map_conversion<int, std::shared_ptr<T>>();
            
            // Register common map conversions with this type as key (if it has operator<)
            if constexpr (std::is_default_constructible_v<T> && requires(const T& a, const T& b) { a < b; }) {
                conv_mgr.template add_map_conversion<T, std::string>();
                conv_mgr.template add_map_conversion<T, int>();
                conv_mgr.template add_map_conversion<T, int64_t>();
                conv_mgr.template add_map_conversion<T, double>();
                conv_mgr.template add_map_conversion<T, bool>();
            }
        }
    }
    
    static const detail::bound_class_conversion& bound_of(const conversions::conversion_entry& entry) {
        return *static_cast<const detail::bound_class_conversion*>(entry.state.get());
    }
    static void shared_from_script(const conversions::conversion_entry& entry, const script_value& v, void* out) {
        static_cast<std::optional<std::shared_ptr<T>>*>(out)->emplace(std::static_pointer_cast<T>(bound_of(entry).unwrap_shared(v, typeid(T))));
    }
    static script_value shared_to_script(const conversions::conversion_entry& entry, const void* value) {
        return bound_of(entry).wrap(std::static_pointer_cast<void>(*static_cast<const std::shared_ptr<T>*>(value)), entry.eng);
    }
    static void value_from_script(const conversions::conversion_entry& entry, const script_value& v, void* out) {
        static_cast<std::optional<T>*>(out)->emplace(*detail::extract_cpp_object_ptr<T>(v, bound_of(entry).def->get_type_id()));
    }
    static void checked_value_from_script(const conversions::conversion_entry& entry, const script_value& v, void* out) {
        bound_of(entry).require_object(v);
        value_from_script(entry, v, out);
    }
    static script_value value_to_script(const conversions::conversion_entry& entry, const void* value) {
        script_value result = bound_of(entry).wrap(std::make_shared<T>(*static_cast<const T*>(value)), entry.eng);
        if (entry.eng && entry.eng->parallel_region_active()) [[unlikely]] { detail::note_region_made(result, entry.eng); }
        return result;
    }

    // A host callable with non-const T& parameters carries its mask for the argument capture (host_writeback_function)
    template<uint64_t Mask>
    static script_function writeback_tagged(script_function fn) {
        if constexpr (Mask != 0) { return host_writeback_function{ std::move(fn), Mask }; }
        else { return fn; }
    }

    // One argument's carrier: a T& that writes back holds its private copy and the lvalue it stores to, every other
    // parameter what value_converter hands the call
    template<typename P>
    using carrier_t = std::conditional_t<detail::host_writes_back<P>(), detail::writeback_argument<std::remove_cvref_t<P>>,
                                         std::conditional_t<std::is_lvalue_reference_v<P> && !std::is_same_v<P, const char&>, P, std::remove_cvref_t<P>>>;
    template<typename P, size_t I, size_t Offset>
    static carrier_t<P> make_carrier(const std::vector<script_value>& args, engine* eng) {
        if constexpr (detail::host_writes_back<P>()) { return detail::make_writeback_argument<std::remove_cvref_t<P>>(args, I, Offset, eng); }
        else { return detail::value_converter<P>::from(args[I + Offset], eng); }
    }
    template<typename P, typename C>
    static decltype(auto) pass_carrier(C& c) {
        if constexpr (detail::host_writes_back<P>()) { return static_cast<std::remove_cvref_t<P>&>(c); }
        else if constexpr (std::is_lvalue_reference_v<P>) { return static_cast<P>(c); }
        else { return std::move(c); }
    }
    // Skip = leading tuple elements that are no script argument (a method lambda's self); Offset = leading args entries
    // that are no script argument (the receiver)
    template<typename ArgsTuple, size_t Skip, size_t Offset, size_t... Is>
    static auto make_carriers(const std::vector<script_value>& args, engine* eng, std::index_sequence<Is...>) {
        return std::tuple<carrier_t<std::tuple_element_t<Is + Skip, ArgsTuple>>...>{
            make_carrier<std::tuple_element_t<Is + Skip, ArgsTuple>, Is, Offset>(args, eng)... };
    }
    template<typename R, typename ArgsTuple, size_t Skip, size_t Offset, typename Invoke, size_t... Is>
    static checked_result<script_value> call_with_writeback(Invoke&& invoke, const std::vector<script_value>& args, std::index_sequence<Is...>, engine* eng) {
        detail::parameter_storage storage;
        detail::parameter_storage::scope_guard guard(eng, &storage);
        auto carriers = make_carriers<ArgsTuple, Skip, Offset>(args, eng, std::index_sequence<Is...>{});
        return detail::invoke_with_writeback<R>(carriers, eng, [&]() -> decltype(auto) {
            return invoke(pass_carrier<std::tuple_element_t<Is + Skip, ArgsTuple>>(std::get<Is>(carriers))...);
        });
    }

    // Helper method for creating objects with arguments
    template<typename... Args, size_t... Is>
    static std::shared_ptr<T> create_object(const std::vector<script_value>& args, std::index_sequence<Is...>, engine* eng) {
        // Create parameter storage on stack
        detail::parameter_storage storage;
        detail::parameter_storage::scope_guard guard(eng, &storage);

        if constexpr (detail::host_writeback_mask<Args...>() != 0) {
            std::shared_ptr<T> made;
            auto result = call_with_writeback<void, std::tuple<Args...>, 0, 0>([&made](auto&&... a) { made = std::make_shared<T>(std::forward<decltype(a)>(a)...); },
                                                                         args, std::index_sequence<Is...>{}, eng);
            if (!result) { throw_checked_error(eng, result.message(), result.symbol_id(), result.symbol_id2()); }
            return made;
        } else if constexpr (std::is_constructible_v<T, engine*, Args...>) {
            return std::make_shared<T>(eng, detail::value_converter<Args>::from(args[Is], eng)...);   // an engine-first constructor takes the constructing engine
        } else {
            return std::make_shared<T>(detail::value_converter<Args>::from(args[Is], eng)...);
        }
    }

    // Helper method for calling member functions
    template<typename R, typename... Args, size_t... Is>
    static auto call_method(T* obj, R(T::*method)(Args...), const std::vector<script_value>& args, std::index_sequence<Is...>, engine* eng) {
        // Create parameter storage on stack
        detail::parameter_storage storage;
        detail::parameter_storage::scope_guard guard(eng, &storage);

        if constexpr (detail::host_writeback_mask<Args...>() != 0) {
            return call_with_writeback<R, std::tuple<Args...>, 0, 1>([obj, method](auto&&... a) -> decltype(auto) { return (obj->*method)(std::forward<decltype(a)>(a)...); },
                                                               args, std::index_sequence<Is...>{}, eng);
        } else if constexpr (std::is_void_v<R>) {
            (obj->*method)(detail::value_converter<Args>::from(args[Is + 1], eng)...);
            return script_value(std::monostate{}, eng); // null for void
        } else {
            R result = (obj->*method)(detail::value_converter<Args>::from(args[Is + 1], eng)...);
            return detail::value_converter<R>::to(result, eng);
        }
    }
    
    // Helper method for calling const member functions
    template<typename R, typename... Args, size_t... Is>
    static auto call_const_method(const T* obj, R(T::*method)(Args...) const, const std::vector<script_value>& args, std::index_sequence<Is...>, engine* eng) {
        // Create parameter storage on stack
        detail::parameter_storage storage;
        detail::parameter_storage::scope_guard guard(eng, &storage);

        if constexpr (detail::host_writeback_mask<Args...>() != 0) {
            return call_with_writeback<R, std::tuple<Args...>, 0, 1>([obj, method](auto&&... a) -> decltype(auto) { return (obj->*method)(std::forward<decltype(a)>(a)...); },
                                                               args, std::index_sequence<Is...>{}, eng);
        } else if constexpr (std::is_void_v<R>) {
            (obj->*method)(detail::value_converter<Args>::from(args[Is + 1], eng)...);
            return script_value(std::monostate{}, eng); // null for void
        } else {
            R result = (obj->*method)(detail::value_converter<Args>::from(args[Is + 1], eng)...);
            return detail::value_converter<R>::to(result, eng);
        }
    }
    
    // Helper method for calling lambdas/callables
    template<typename R, typename ArgsTuple, typename Callable, size_t... Is>
    static auto call_callable(Callable&& callable, const std::vector<script_value>& args, std::index_sequence<Is...>, engine* eng) {
        // Create parameter storage on stack
        detail::parameter_storage storage;
        detail::parameter_storage::scope_guard guard(eng, &storage);

        if constexpr (detail::tuple_writeback_mask<ArgsTuple>::value != 0) {
            return call_with_writeback<R, ArgsTuple, 0, 0>(callable, args, std::index_sequence<Is...>{}, eng);
        } else if constexpr (std::is_void_v<R>) {
            callable(detail::value_converter<std::tuple_element_t<Is, ArgsTuple>>::from(args[Is], eng)...);
            return script_value(std::monostate{}, eng); // null for void
        } else {
            R result = callable(detail::value_converter<std::tuple_element_t<Is, ArgsTuple>>::from(args[Is], eng)...);
            return detail::value_converter<R>::to(result, eng);
        }
    }
    
    // Helper method for calling lambdas with self parameter
    template<typename R, typename ArgsTuple, typename Callable, size_t... Is>
    static auto call_lambda_with_self(Callable&& callable, T* self, const std::vector<script_value>& args, std::index_sequence<Is...>, engine* eng) {
        // We need to call the lambda with:
        // - self as the first argument
        // - remaining args starting from args[1] mapped to tuple indices 1, 2, 3...
        return call_lambda_with_self_impl<R, ArgsTuple, Callable>(
            std::forward<Callable>(callable), self, args, 
            std::make_index_sequence<sizeof...(Is) - 1>{}, eng
        );
    }
    
    // implementation helper that correctly maps arguments
    template<typename R, typename ArgsTuple, typename Callable, size_t... Is>
    static auto call_lambda_with_self_impl(Callable&& callable, T* self, const std::vector<script_value>& args, std::index_sequence<Is...>, engine* eng) {
        // Create parameter storage on stack
        detail::parameter_storage storage;
        detail::parameter_storage::scope_guard guard(eng, &storage);

        if constexpr (detail::tuple_writeback_mask<ArgsTuple, 1>::value != 0) {
            auto invoke = [&callable, self](auto&&... a) -> decltype(auto) { return callable(*self, std::forward<decltype(a)>(a)...); };
            if constexpr (std::is_reference_v<R> && std::is_same_v<std::remove_reference_t<R>, T>) {
                auto result = call_with_writeback<void, ArgsTuple, 1, 1>([&invoke](auto&&... a) { invoke(std::forward<decltype(a)>(a)...); },
                                                                   args, std::index_sequence<Is...>{}, eng);
                return result ? checked_result<script_value>(args[0]) : result;
            } else {
                return call_with_writeback<R, ArgsTuple, 1, 1>(invoke, args, std::index_sequence<Is...>{}, eng);
            }
        } else if constexpr (std::is_void_v<R>) {
            // Call with self as first argument, then args[1], args[2], etc.
            callable(*self, detail::value_converter<std::tuple_element_t<Is + 1, ArgsTuple>>::from(args[Is + 1], eng)...);
            return script_value(std::monostate{}, eng); // null for void
        } else {
            // For return type R&, we need special handling for method chaining
            if constexpr (std::is_reference_v<R> && std::is_same_v<std::remove_reference_t<R>, T>) {
                // Method returns T&, so we should return the original 'this' script_value for chaining
                callable(*self, detail::value_converter<std::tuple_element_t<Is + 1, ArgsTuple>>::from(args[Is + 1], eng)...);
                return args[0]; // Return the original 'this' for chaining
            } else {
                R result = callable(*self, detail::value_converter<std::tuple_element_t<Is + 1, ArgsTuple>>::from(args[Is + 1], eng)...);
                return detail::value_converter<R>::to(result, eng);
            }
        }
    }
    
};


// Context-based factory implementations use type erasure (see make_context_extractor above)
// to avoid MSVC template instantiation issues with incomplete archive_reader type.
// The make_context_extractor function is instantiated at call sites where archive_reader is fully defined.

} // namespace jai

// A binding record's property routes and its polymorphic registration instantiate any_archive dispatch
#include <jaiscript/serialization/archive_dispatch.hpp>

#endif // __JAISCRIPT_CORE_dynamic_binder_HPP__
