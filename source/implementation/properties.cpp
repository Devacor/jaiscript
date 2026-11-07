#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/property_schema.hpp>

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

	property_manager::~property_manager() = default;

	void property_manager::add(property_base* prop) {
		m_properties[prop->name()] = prop;
	}

	void property_manager::clone_to_target(property_manager& target) const {
		for (auto& [k, v] : m_properties) {
			auto it = target.m_properties.find(k);
			if (it != target.m_properties.end()) {
				v->clone_to_target(*it->second);
			}
		}
	}

	property_base* property_manager::get(const std::string& key) const {
		auto it = m_properties.find(key);
		return it != m_properties.end() ? it->second : nullptr;
	}

}
