#pragma once

#ifndef __JAISCRIPT_PROPERTY_HPP__
#define __JAISCRIPT_PROPERTY_HPP__

#include <string>
#include <functional>
#include <memory>
#include <compare>
#include <concepts>
#include <type_traits>
#include <typeindex>
#include <cstring>
#include <jaiscript/properties/property_schema.hpp>


// Forward declarations
namespace jai {
	class property_manager;
	class property_base;
	struct signal_script_ops;

	template<typename T> class property;
	template<typename T> class deleted_property;

	namespace serialization {
		class any_archive_writer;
		class any_archive_reader;
	}
}

namespace jai {

	// Similar to Unreal Engine's "transient" property specifier.
	enum class serialize_mode {
		automatic,  // Default: property will be serialized (may hit static_assert if type not serializable)
		transient   // Never serialize this property - skip silently
	};

	// C++20 Concepts for cleaner constraints
	template<typename T>
	concept smart_pointer = requires(T t) {
		typename T::element_type;
		{ t.get() } -> std::convertible_to<typename T::element_type*>;
		{ t.operator->() } -> std::same_as<typename T::element_type*>;
		{ t.operator*() } -> std::same_as<std::add_lvalue_reference_t<typename T::element_type>>;
	};

	template<typename T>
	concept pointer_like = std::is_pointer_v<T> || smart_pointer<T>;

	template<typename T>
	concept boolean_testable = requires(const T & t) {
		{ static_cast<bool>(t) };  // Can be explicitly converted to bool
	};

	// ============================================================================
	// Property base class
	// ============================================================================

	// property_base - abstract base for all properties
	class property_base {
	public:
		virtual ~property_base() = default;

		bool operator<(const property_base& rhs) const {
			return name() < rhs.name();
		}

		const std::string& name() const { return m_property_name; }

		serialize_mode serialization() const { return m_serialize_mode; }
		void serialization(serialize_mode mode) const { m_serialize_mode = mode; }

		// Only effective when serialize_mode is automatic
		bool serialize_enabled() const { return m_allow_serialization; }
		void serialize_enabled(bool allow_serialization) const {
			m_allow_serialization = allow_serialization;
		}

		serialize_mode get_serialize_mode() const { return m_serialize_mode; }
		void set_serialize_mode(serialize_mode mode) const { m_serialize_mode = mode; }

		void set_transient() const { m_serialize_mode = serialize_mode::transient; }
		void clear_transient() const { m_serialize_mode = serialize_mode::automatic; }

		// allow_save: true if property should be serialized
		// - serialize_mode::transient always returns false
		// - serialize_mode::automatic respects m_allow_serialization
		virtual bool allow_save() const {
			return m_serialize_mode != serialize_mode::transient && m_allow_serialization;
		}

		virtual void serialize(serialization::any_archive_writer& ar) const = 0;
		virtual void serialize(serialization::any_archive_reader& ar) = 0;

		virtual void clone_to_target(property_base& target) = 0;

		// ===== ERASED VALUE ACCESS (reflection bridge for tooling: inspectors, debuggers) =====
		// Defaults describe a valueless property (deleted_property); property<T> overrides.
		virtual std::type_index value_type_id() const { return typeid(void); }
		virtual void* erased_value_ptr() { return nullptr; }
		virtual const void* erased_value_ptr() const { return nullptr; }
		// Assigns through the property's own operator= so observable_property change
		// signals fire; returns false when the property cannot accept a value.
		virtual bool erased_assign(const void* source) { (void)source; return false; }

		template<typename T>
		T* value_as() {
			return value_type_id() == std::type_index(typeid(T)) ? static_cast<T*>(erased_value_ptr()) : nullptr;
		}
		template<typename T>
		const T* value_as() const {
			return value_type_id() == std::type_index(typeid(T)) ? static_cast<const T*>(erased_value_ptr()) : nullptr;
		}
		template<typename T>
		bool assign_value(const T& source) {
			return value_type_id() == std::type_index(typeid(T)) && erased_assign(std::addressof(source));
		}

		// Signal properties fill a type-erased connect/disconnect table (the
		// generic "Signal" script view rides it); value properties decline.
		virtual bool signal_ops(signal_script_ops&) { return false; }

	protected:
		inline property_base(std::string name)
			: m_property_name(std::move(name)) {
		}

		inline property_base(property_manager& property_register, std::string name);

		// A property is registered by address under its name. A copy or a move makes one with the same name and save
		// switches, which its owner's manager registers (property_manager's relocating constructor); the source keeps its
		// name, and an assignment changes neither side's.
		property_base(const property_base&) = default;
		property_base(property_base&& a_other) noexcept : property_base(static_cast<const property_base&>(a_other)) {}
		property_base& operator=(const property_base&) noexcept { return *this; }
		property_base& operator=(property_base&&) noexcept { return *this; }

		mutable serialize_mode m_serialize_mode = serialize_mode::automatic;
		mutable bool m_allow_serialization = true;  // Dynamic runtime toggle

	private:
		std::string m_property_name;
	};

	template<typename T>
	class property : public property_base {
	protected:
		T m_value;
	public:
		// A custom clone: a plain function (a captureless lambda converts), one pointer per property
		using clone_function = void (*)(property<T>&, property<T>&);
	protected:
		clone_function m_custom_clone = nullptr;

		template<typename F>
		static constexpr bool is_clone_function_v =
			std::is_invocable_v<F, property<T>&, property<T>&>;

	public:
		using value_type = T;

		// Constructor 1: No default value, no clone function
		inline property(property_manager& reg, std::string name)
			: property_base(reg, std::move(name))
			, m_value{}
			, m_custom_clone() {
		}

		// Constructor 2: With value (but NOT a function type)
		template<typename U, std::enable_if_t<!is_clone_function_v<U>, int> = 0>
		inline property(property_manager& reg, std::string name, U&& def)
			: property_base(reg, std::move(name))
			, m_value(std::forward<U>(def))
			, m_custom_clone() {
		}

		// Constructor 3: With clone function only
		template<typename Fn, std::enable_if_t<is_clone_function_v<Fn>, int> = 0>
		inline property(property_manager& reg, std::string name, Fn&& cl)
			: property_base(reg, std::move(name))
			, m_value{}
			, m_custom_clone(std::forward<Fn>(cl)) {
		}

		// Constructor 4: With T constructed from initializer list
		inline property(property_manager& reg, std::string name, T def)
			: property_base(reg, std::move(name))
			, m_value(std::move(def))
			, m_custom_clone() {
		}

		// Constructor 5: With value and clone function
		template<typename U, typename Fn>
		inline property(property_manager& reg, std::string name, U&& def, Fn&& cl)
			: property_base(reg, std::move(name))
			, m_value(std::forward<U>(def))
			, m_custom_clone(std::forward<Fn>(cl)) {
		}

		// Constructor 6: With T constructed from initializer list and clone function
		inline property(property_manager& reg, std::string name, T def, clone_function cl)
			: property_base(reg, std::move(name))
			, m_value(std::move(def))
			, m_custom_clone(cl) {
		}

		// Constructor 7: With serialize_mode
		inline property(property_manager& reg, std::string name, serialize_mode mode)
			: property_base(reg, std::move(name))
			, m_value{}
			, m_custom_clone() {
			m_serialize_mode = mode;
		}

		// Constructor 8: With value and serialize_mode
		template<typename U, std::enable_if_t<!is_clone_function_v<U> && !std::is_same_v<std::decay_t<U>, serialize_mode>, int> = 0>
		inline property(property_manager& reg, std::string name, U&& def, serialize_mode mode)
			: property_base(reg, std::move(name))
			, m_value(std::forward<U>(def))
			, m_custom_clone() {
			m_serialize_mode = mode;
		}

		const property& set_transient() const {
			m_serialize_mode = serialize_mode::transient;
			return *this;
		}
		const property& clear_transient() const {
			m_serialize_mode = serialize_mode::automatic;
			return *this;
		}
		const property& set_serialize_mode(serialize_mode mode) const {
			m_serialize_mode = mode;
			return *this;
		}

		property(const property& a_other) requires std::is_copy_constructible_v<T>
			: property_base(a_other)
			, m_value(a_other.m_value)
			, m_custom_clone(a_other.m_custom_clone) {
		}

		property(property&& a_other) noexcept requires std::is_move_constructible_v<T>
			: property_base(std::move(a_other))
			, m_value(std::move(a_other.m_value))
			, m_custom_clone(a_other.m_custom_clone) {
		}

		// Assignment carries the value alone; clone_to_target is the copy that runs a custom clone.
		property& operator=(const property& rhs) requires std::is_copy_assignable_v<T> {
			m_value = rhs.m_value;
			return *this;
		}

		property& operator=(property&& rhs) noexcept requires std::is_move_assignable_v<T> {
			m_value = std::move(rhs.m_value);
			return *this;
		}

		// Constrained with the copy assignment above: an owner's defaulted copy assignment must find no way to assign a
		// move-only property, where a const property would otherwise convert to const T&.
		property& operator=(const T& v) requires std::is_copy_assignable_v<T> { m_value = v; return *this; }
		property& operator=(T&& v) { m_value = std::move(v); return *this; }

		// ===== CONVERSION OPERATORS =====
		operator const T& () const {
			return m_value;
		}

		// Mutable access
		operator T& () {
			return m_value;
		}

		// Boolean conversion for all non-bool types
		operator bool() const requires (!std::is_same_v<T, bool>) && requires(const T& t) { static_cast<bool>(t); } {
			return static_cast<bool>(m_value);
		}

		// Stream insertion — transparently forwards to the wrapped value's operator<<. Templated on
		// the stream type (so no <ostream> include is needed here) and SFINAE-constrained so it only
		// participates when T itself is streamable to that stream. Hidden friend: found via ADL,
		// pollutes no namespace, and lets `os << someProperty` work like `os << someProperty.get()`.
		template<typename Stream>
		friend auto operator<<(Stream& a_os, const property& a_prop)
			-> decltype(a_os << std::declval<const T&>()) {
			return a_os << a_prop.m_value;
		}

		// ===== ACCESSOR METHODS =====
		T& get() {
			return m_value;
		}
		const T& get() const {
			return m_value;
		}

		// ===== ERASED VALUE ACCESS =====
		std::type_index value_type_id() const override { return typeid(T); }
		void* erased_value_ptr() override { return &m_value; }
		const void* erased_value_ptr() const override { return &m_value; }
		bool erased_assign(const void* source) override {
			if constexpr (std::is_copy_assignable_v<T>) {
				m_value = *static_cast<const T*>(source);
				return true;
			} else {
				(void)source;
				return false;
			}
		}

		// ===== DEREFERENCE OPERATORS =====
		// For smart pointers: operator* returns the smart pointer itself
		T& operator*() requires smart_pointer<T> {
			return m_value;
		}
		const T& operator*() const requires smart_pointer<T> {
			return m_value;
		}

		// For raw pointers: operator* dereferences
		auto& operator*() requires std::is_pointer_v<T> {
			return *m_value;
		}
		const auto& operator*() const requires std::is_pointer_v<T> {
			return *m_value;
		}

		// For non-pointer types: operator* returns the value
		T& operator*() requires (!pointer_like<T>) {
			return m_value;
		}
		const T& operator*() const requires (!pointer_like<T>) {
			return m_value;
		}

		// ===== ARROW OPERATORS =====
		// For smart pointers: arrow goes through to element_type
		template<typename U = T>
		auto operator->() -> std::enable_if_t<smart_pointer<U>, typename U::element_type*> {
			return m_value.get();
		}

		template<typename U = T>
		auto operator->() const -> std::enable_if_t<smart_pointer<U>, typename U::element_type*> {
			return m_value.get();
		}

		// For raw pointers: just return the pointer
		T operator->() requires std::is_pointer_v<T> {
			return m_value;
		}
		T operator->() const requires std::is_pointer_v<T> {
			return m_value;
		}

		// For class types: return pointer to value
		T* operator->() requires std::is_class_v<T> && (!pointer_like<T>) {
			return &m_value;
		}
		const T* operator->() const requires std::is_class_v<T> && (!pointer_like<T>) {
			return &m_value;
		}

		// ===== COMPARISON OPERATORS =====
		// We provide both spaceship and traditional operators to avoid ambiguity

		// Three-way comparison with another property
		auto operator<=>(const property& other) const requires std::three_way_comparable<T> {
			return m_value <=> other.m_value;
		}

		// Equality with another property
		bool operator==(const property& other) const {
			return m_value == other.m_value;
		}

		// Traditional comparison operators with any type U
		template<typename U>
		bool operator<(const U& other) const {
			return m_value < other;
		}

		template<typename U>
		bool operator<=(const U& other) const {
			return m_value <= other;
		}

		template<typename U>
		bool operator>(const U& other) const {
			return m_value > other;
		}

		template<typename U>
		bool operator>=(const U& other) const {
			return m_value >= other;
		}

		template<typename U>
		bool operator==(const U& other) const {
			return m_value == other;
		}

		template<typename U>
		bool operator!=(const U& other) const {
			return m_value != other;
		}

		// Friend operators for reverse comparisons (e.g., 0 > property)
		template<typename U>
		friend bool operator<(const U& lhs, const property& rhs) {
			return lhs < rhs.m_value;
		}

		template<typename U>
		friend bool operator<=(const U& lhs, const property& rhs) {
			return lhs <= rhs.m_value;
		}

		template<typename U>
		friend bool operator>(const U& lhs, const property& rhs) {
			return lhs > rhs.m_value;
		}

		template<typename U>
		friend bool operator>=(const U& lhs, const property& rhs) {
			return lhs >= rhs.m_value;
		}

		template<typename U>
		friend bool operator==(const U& lhs, const property& rhs) {
			return lhs == rhs.m_value;
		}

		template<typename U>
		friend bool operator!=(const U& lhs, const property& rhs) {
			return lhs != rhs.m_value;
		}

		// ===== ARITHMETIC OPERATORS =====
		// Enable arithmetic operations for arithmetic types
		template<typename U>
		friend auto operator+(const property& lhs, const U& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			return lhs.m_value + rhs;
		}

		template<typename U>
		friend auto operator+(const U& lhs, const property& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			return lhs + rhs.m_value;
		}

		template<typename U>
		friend auto operator-(const property& lhs, const U& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			return lhs.m_value - rhs;
		}

		template<typename U>
		friend auto operator-(const U& lhs, const property& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			return lhs - rhs.m_value;
		}

		template<typename U>
		friend auto operator*(const property& lhs, const U& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			return lhs.m_value * rhs;
		}

		template<typename U>
		friend auto operator*(const U& lhs, const property& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			return lhs * rhs.m_value;
		}

		template<typename U>
		friend auto operator/(const property& lhs, const U& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			return lhs.m_value / rhs;
		}

		template<typename U>
		friend auto operator/(const U& lhs, const property& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			return lhs / rhs.m_value;
		}

		template<typename U>
		friend auto operator%(const property& lhs, const U& rhs)
			requires std::is_integral_v<T>&& std::is_integral_v<U> {
			return lhs.m_value % rhs;
		}

		template<typename U>
		friend auto operator%(const U& lhs, const property& rhs)
			requires std::is_integral_v<T>&& std::is_integral_v<U> {
			return lhs % rhs.m_value;
		}

		// ===== COMPOUND ASSIGNMENT OPERATORS =====
		template<typename U>
		property& operator+=(const U& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			m_value += rhs;
			return *this;
		}

		template<typename U>
		property& operator-=(const U& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			m_value -= rhs;
			return *this;
		}

		template<typename U>
		property& operator*=(const U& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			m_value *= rhs;
			return *this;
		}

		template<typename U>
		property& operator/=(const U& rhs)
			requires std::is_arithmetic_v<T>&& std::is_arithmetic_v<U> {
			m_value /= rhs;
			return *this;
		}

		template<typename U>
		property& operator%=(const U& rhs)
			requires std::is_integral_v<T>&& std::is_integral_v<U> {
			m_value %= rhs;
			return *this;
		}

		// Increment/decrement operators
		property& operator++() requires std::is_arithmetic_v<T> {
			++m_value;
			return *this;
		}

		property& operator--() requires std::is_arithmetic_v<T> {
			--m_value;
			return *this;
		}

		T operator++(int) requires std::is_arithmetic_v<T> {
			T old = m_value;
			++m_value;
			return old;
		}

		T operator--(int) requires std::is_arithmetic_v<T> {
			T old = m_value;
			--m_value;
			return old;
		}

		// ===== CONTAINER OPERATIONS =====
		// operator[] for container types
		template<typename I>
		auto& operator[](I&& i) requires requires { m_value[std::forward<I>(i)]; } {
			return m_value[std::forward<I>(i)];
		}

		template<typename I>
		const auto& operator[](I&& i) const requires requires { m_value[std::forward<I>(i)]; } {
			return m_value[std::forward<I>(i)];
		}

		// Function call operator
		template<typename... Args>
		auto operator()(Args&&... args)
			requires std::invocable<T, Args...> {
			return m_value(std::forward<Args>(args)...);
		}

		template<typename... Args>
		auto operator()(Args&&... args) const
			requires std::invocable<const T, Args...> {
			return m_value(std::forward<Args>(args)...);
		}

		// Iterator support
		auto begin() requires requires { m_value.begin(); } {
			return m_value.begin();
		}

		auto end() requires requires { m_value.end(); } {
			return m_value.end();
		}

		auto begin() const requires requires { m_value.begin(); } {
			return m_value.begin();
		}

		auto end() const requires requires { m_value.end(); } {
			return m_value.end();
		}

		// Serialization - implemented in property_serialization.hpp
		void serialize(serialization::any_archive_writer& ar) const override;
		void serialize(serialization::any_archive_reader& ar) override;

		// Clone method
		void set_custom_clone(clone_function custom_clone) {
			m_custom_clone = custom_clone;
		}

		void clone_to_target(property_base& target) override {
			auto& t = static_cast<property<T>&>(target);
			if (m_custom_clone){
				m_custom_clone(*this, t);
			} else if constexpr (std::is_copy_assignable_v<T>) {
				t.m_value = m_value;
			}
			// Move-only T without a custom clone: nothing to clone (virtual instantiation
			// must not force copy-assign — property<unique_ptr<T>> is legal to declare).
		}
	};

	// deleted_property - placeholder for removed properties in serialization
	template<typename T>
	class deleted_property : public property_base {
	public:
		deleted_property(property_manager& list, std::string name)
			: property_base(list, std::move(name)) {
		}

		deleted_property(const std::string& name)
			: property_base(name) {
		}

		bool allow_save() const override { return false; }

		// No accessors, no value.
		const T& get() const = delete;
		T& get() = delete;

		// Serialization - implemented in property_serialization.hpp
		void serialize(serialization::any_archive_writer& ar) const override;
		void serialize(serialization::any_archive_reader& ar) override;

		void clone_to_target(property_base&) override {
			// No-op
		}
	};

	// The schema's math bridge for a property of ValueT: empty unless ValueT has a math_adapter
	template <typename ValueT>
	math_property_bridge math_bridge_of() {
		if constexpr (math_adapted<ValueT>) {
			using adapter = math_adapter<ValueT>;
			math_property_bridge bridge;
			bridge.kind = adapter::kind;
			bridge.read = [](const void* host, uint32_t* words) {
				const typename adapter::native_type native = adapter::to_native(*static_cast<const ValueT*>(host));
				std::memcpy(words, &native, sizeof(native));
			};
			bridge.assign = [](property_base& property, const uint32_t* words) {
				typename adapter::native_type native{};
				std::memcpy(&native, words, sizeof(native));
				const ValueT host = adapter::from_native(native);
				return property.erased_assign(&host);
			};
			return bridge;
		} else {
			return {};
		}
	}

} // namespace jai
#endif
