#pragma once

#ifndef __JAISCRIPT_PROPERTIES_OBSERVABLE_PROPERTY_HPP__
#define __JAISCRIPT_PROPERTIES_OBSERVABLE_PROPERTY_HPP__

#include <jaiscript/properties/property.hpp>
#include <jaiscript/signals/signal.hpp>

namespace jai {

template<typename T>
class observable_property : public property<T> {
public:
	using base_type = property<T>;
	using value_type = T;

	using change_signal_type = signal_emitter<void(const T&, const T&)>;

	observable_property(property_manager& reg, std::string name)
		: base_type(reg, std::move(name)) {}

	template<typename U, std::enable_if_t<!base_type::template is_clone_function_v<U>, int> = 0>
	observable_property(property_manager& reg, std::string name, U&& def)
		: base_type(reg, std::move(name), std::forward<U>(def)) {}

	observable_property(property_manager& reg, std::string name, T def)
		: base_type(reg, std::move(name), std::move(def)) {}

	// Pinned to the registering manager, like property<T>: no moves, no copy construction.
	observable_property(observable_property&&) = delete;
	observable_property& operator=(observable_property&&) = delete;
	observable_property(const observable_property&) = delete;

	observable_property& operator=(const observable_property& rhs) {
		if (this->m_value != rhs.m_value) {
			T old_value = this->m_value;
			this->m_value = rhs.m_value;
			on_change_.emit(old_value, this->m_value);
		}
		return *this;
	}

	observable_property& operator=(const T& v) {
		if (this->m_value != v) {
			T old_value = this->m_value;
			this->m_value = v;
			on_change_.emit(old_value, this->m_value);
		}
		return *this;
	}

	observable_property& operator=(T&& v) {
		if (&this->m_value != &v && this->m_value != v) {
			T old_value = std::move(this->m_value);
			this->m_value = std::move(v);
			on_change_.emit(old_value, this->m_value);
		}
		return *this;
	}

	// Erased assignment routes through operator= so on_change fires with change
	// detection; falls back to the silent base write when T cannot be compared.
	bool erased_assign(const void* source) override {
		if constexpr (std::is_copy_assignable_v<T> && std::is_copy_constructible_v<T> && std::equality_comparable<T>) {
			*this = *static_cast<const T*>(source);
			return true;
		} else {
			return base_type::erased_assign(source);
		}
	}

	// Set value without triggering signal (for loading/initialization)
	void set_silent(const T& v) {
		this->m_value = v;
	}

	void set_silent(T&& v) {
		this->m_value = std::move(v);
	}

	template<typename U>
	observable_property& operator+=(const U& rhs)
		requires std::is_arithmetic_v<T> && std::is_arithmetic_v<U> {
		T old_value = this->m_value;
		this->m_value += rhs;
		if (old_value != this->m_value) {
			on_change_.emit(old_value, this->m_value);
		}
		return *this;
	}

	template<typename U>
	observable_property& operator-=(const U& rhs)
		requires std::is_arithmetic_v<T> && std::is_arithmetic_v<U> {
		T old_value = this->m_value;
		this->m_value -= rhs;
		if (old_value != this->m_value) {
			on_change_.emit(old_value, this->m_value);
		}
		return *this;
	}

	template<typename U>
	observable_property& operator*=(const U& rhs)
		requires std::is_arithmetic_v<T> && std::is_arithmetic_v<U> {
		T old_value = this->m_value;
		this->m_value *= rhs;
		if (old_value != this->m_value) {
			on_change_.emit(old_value, this->m_value);
		}
		return *this;
	}

	template<typename U>
	observable_property& operator/=(const U& rhs)
		requires std::is_arithmetic_v<T> && std::is_arithmetic_v<U> {
		T old_value = this->m_value;
		this->m_value /= rhs;
		if (old_value != this->m_value) {
			on_change_.emit(old_value, this->m_value);
		}
		return *this;
	}

	observable_property& operator++() requires std::is_arithmetic_v<T> {
		T old_value = this->m_value;
		++this->m_value;
		on_change_.emit(old_value, this->m_value);
		return *this;
	}

	observable_property& operator--() requires std::is_arithmetic_v<T> {
		T old_value = this->m_value;
		--this->m_value;
		on_change_.emit(old_value, this->m_value);
		return *this;
	}

	T operator++(int) requires std::is_arithmetic_v<T> {
		T old_value = this->m_value;
		++this->m_value;
		on_change_.emit(old_value, this->m_value);
		return old_value;
	}

	T operator--(int) requires std::is_arithmetic_v<T> {
		T old_value = this->m_value;
		--this->m_value;
		on_change_.emit(old_value, this->m_value);
		return old_value;
	}

	signal<void(const T&, const T&)> on_change{on_change_};

private:
	change_signal_type on_change_;
};


} // namespace jai

// Note: JAI_OBSERVABLE_PROPERTY macro is defined in macros.hpp
// to ensure the helper macros (JAI_REMOVE_PARENS) are available.

#endif // __JAISCRIPT_PROPERTIES_OBSERVABLE_PROPERTY_HPP__
