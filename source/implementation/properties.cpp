#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/property_schema.hpp>
#include <algorithm>
#include <cstdint>

namespace jai {

	void type_property_schema::add(property_meta meta) {
		name_to_index_[meta.name] = own_properties_.size();
		own_properties_.push_back(std::move(meta));
	}

	void type_property_schema::add_base(std::type_index base_type) {
		base_types_.push_back(base_type);
	}

	const property_meta* type_property_schema::find(const std::string& name) const {
		auto it = name_to_index_.find(name);
		return it != name_to_index_.end() ? &own_properties_[it->second] : nullptr;
	}

	std::vector<std::string> type_property_schema::property_names() const {
		std::vector<std::string> names;
		names.reserve(own_properties_.size());
		for (const auto& prop : own_properties_) {
			names.push_back(prop.name);
		}
		return names;
	}

	type_registry& type_registry::instance() {
		static type_registry registry;
		return registry;
	}

	type_property_schema& type_registry::for_type(std::type_index ti) {
		std::scoped_lock guard(mutex_);
		return schemas_[ti];
	}

	void type_registry::add_base(std::type_index ti, std::type_index base_type) {
		std::scoped_lock guard(mutex_);
		schemas_[ti].add_base(base_type);
	}

	const type_property_schema* type_registry::try_get(std::type_index ti) const {
		std::scoped_lock guard(mutex_);
		auto it = schemas_.find(ti);
		return it != schemas_.end() ? &it->second : nullptr;
	}

	std::vector<const property_meta*> type_registry::all_properties(std::type_index ti) const {
		std::scoped_lock guard(mutex_);
		std::vector<const property_meta*> result;
		collect_properties_recursive(ti, result);
		return result;
	}

	std::vector<std::string> type_registry::all_property_names(std::type_index ti) const {
		std::vector<std::string> result;
		auto props = all_properties(ti);
		result.reserve(props.size());
		for (const auto* p : props) {
			result.push_back(p->name);
		}
		return result;
	}

	std::vector<std::type_index> type_registry::base_classes(std::type_index ti) const {
		std::scoped_lock guard(mutex_);
		auto it = schemas_.find(ti);
		if (it != schemas_.end()) {
			return it->second.base_types();
		}
		return {};
	}

	bool type_registry::inherits_from(std::type_index derived, std::type_index base) const {
		std::scoped_lock guard(mutex_);
		return inherits_from_locked(derived, base);
	}

	bool type_registry::inherits_from_locked(std::type_index derived, std::type_index base) const {
		if (derived == base) return true;

		auto it = schemas_.find(derived);
		if (it == schemas_.end()) return false;

		for (const auto& direct_base : it->second.base_types()) {
			if (inherits_from_locked(direct_base, base)) {
				return true;
			}
		}
		return false;
	}

	void type_registry::collect_properties_recursive(std::type_index ti, std::vector<const property_meta*>& result) const {
		auto it = schemas_.find(ti);
		if (it == schemas_.end()) return;

		// First collect base class properties (so they come first in order)
		for (const auto& base_ti : it->second.base_types()) {
			collect_properties_recursive(base_ti, result);
		}

		// Then add own properties
		for (const auto& prop : it->second.own_properties()) {
			result.push_back(&prop);
		}
	}

	property_manager::property_manager() = default;

	property_manager::~property_manager() {
		delete[] m_properties;
	}

	property_manager::property_manager(const property_manager& a_source, const void* a_sourceOwner, size_t a_ownerSize) noexcept {
		if (a_source.m_count == 0) {
			return;
		}
		m_properties = new property_base*[a_source.m_count];
		m_capacity = a_source.m_count;
		m_relocated = 1;
		const std::ptrdiff_t shift = reinterpret_cast<const char*>(this) - reinterpret_cast<const char*>(&a_source);
		const auto ownerBegin = reinterpret_cast<std::uintptr_t>(a_sourceOwner);
		for (property_base* sourceProperty : a_source.all()) {
			if (reinterpret_cast<std::uintptr_t>(sourceProperty) - ownerBegin < a_ownerSize) {
				m_properties[m_count++] = reinterpret_cast<property_base*>(reinterpret_cast<char*>(sourceProperty) + shift);
			}
		}
	}

	size_t property_manager::lower_bound(std::string_view a_name) const {
		size_t first = 0;
		size_t count = m_count;
		while (count > 0) {
			const size_t half = count / 2;
			if (m_properties[first + half]->name() < a_name) {
				first += half + 1;
				count -= half + 1;
			} else {
				count = half;
			}
		}
		return first;
	}

	void property_manager::add(property_base* prop) {
		if (m_relocated) {
			// a hand-written copy constructor's members register again, before their neighbours exist to be compared
			for (property_base* registered : all()) {
				if (registered == prop) {
					return;
				}
			}
		}
		const size_t index = lower_bound(prop->name());
		if (index < m_count && m_properties[index]->name() == prop->name()) {
			m_properties[index] = prop;
			return;
		}
		if (m_count == m_capacity) {
			const std::uint32_t grown = m_capacity == 0 ? 4 : m_capacity * 2;
			property_base** larger = new property_base*[grown];
			std::copy(m_properties, m_properties + index, larger);
			std::copy(m_properties + index, m_properties + m_count, larger + index + 1);
			delete[] m_properties;
			m_properties = larger;
			m_capacity = grown;
		} else {
			std::copy_backward(m_properties + index, m_properties + m_count, m_properties + m_count + 1);
		}
		m_properties[index] = prop;
		++m_count;
	}

	void property_manager::clone_to_target(property_manager& target) const {
		for (property_base* prop : all()) {
			if (property_base* counterpart = target.get(prop->name())) {
				prop->clone_to_target(*counterpart);
			}
		}
	}

	property_base* property_manager::get(const std::string& key) const {
		const size_t index = lower_bound(key);
		return index < m_count && m_properties[index]->name() == key ? m_properties[index] : nullptr;
	}

}
