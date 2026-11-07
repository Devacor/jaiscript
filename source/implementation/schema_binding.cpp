#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/template_binder.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/core/engine_impl.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/serialization/binary_archive.hpp>

namespace jai::detail {

namespace {

// A plain (non-observable) property's in-place lane: the property object is a member of the bound object, found once
// per receiver layout and read and written the way the bound getter and setter do (detail/host_member.hpp)
template<typename ValueT>
struct schema_member_lane : host_member_lane {
	std::string name;
	schema_property_access access;
	uint64_t native_type_id = UINT64_MAX;
};

template<typename ValueT>
void* schema_member_locate(const host_member_lane& lane, const script_value& receiver) {
	const auto& self = static_cast<const schema_member_lane<ValueT>&>(lane);
	try {
		void* owner = self.access.extract(receiver, self.native_type_id);
		if (!owner || self.access.owner_size == 0) { return nullptr; }
		jai::property_base* base = self.access.find(owner, self.name);
		auto* prop = dynamic_cast<jai::property<ValueT>*>(base);
		if (!prop || typeid(*base) != typeid(jai::property<ValueT>)) { return nullptr; }
		const char* begin = static_cast<const char*>(owner);
		const char* at = reinterpret_cast<const char*>(prop);
		if (at < begin || at + sizeof(jai::property<ValueT>) > begin + self.access.owner_size) { return nullptr; }
		return prop;
	} catch (...) {
		return nullptr;
	}
}

template<typename ValueT>
script_value schema_member_read(const void* address, engine* eng) {
	return script_value_from_cpp(static_cast<const jai::property<ValueT>*>(address)->get(), eng);
}

template<typename ValueT>
bool schema_member_write(void* address, const script_value& value) {
	return host_member_write<ValueT>(&static_cast<jai::property<ValueT>*>(address)->get(), value);
}

template<typename ValueT>
std::shared_ptr<const host_member_lane> make_schema_member_lane(const std::string& name, const schema_property_access& access, uint64_t native_type_id) {
	if constexpr (host_member_value<ValueT>) {
		auto lane = std::make_shared<schema_member_lane<ValueT>>();
		lane->name = name;
		lane->access = access;
		lane->native_type_id = native_type_id;
		lane->read = &schema_member_read<ValueT>;
		lane->write = &schema_member_write<ValueT>;
		lane->locate = &schema_member_locate<ValueT>;
		return lane;
	} else {
		return nullptr;
	}
}

template<typename ValueT>
void ensure_observable_ref_type_registered(engine& a_engine) {
	using RefType = jai::observable_property_ref<ValueT>;

	std::string type_name = "observable_property_ref<" + type_name_helper<ValueT>::name() + ">";
	if (a_engine.is_type_registered(type_name)) {
		return;
	}

	dynamic_binder<RefType>(a_engine, type_name)
		.transparent_wrapper([](RefType& self) -> ValueT {
			return self.get();
		})
		.method("on_change", [](RefType& self, const script_value& callback) -> script_value {
			if (!callback.is_function()) {
				throw runtime_error("on_change requires a function argument");
			}

			auto* obs_prop = self.property();
			auto* prop_mgr = self.manager();
			if (!obs_prop || !prop_mgr) {
				throw runtime_error("Invalid observable property reference");
			}

			engine* eng = callback.payload_engine();
			const script_function& script_func = callback.as_function();

			auto cpp_callback = [script_func, eng](const ValueT& old_val, const ValueT& new_val) {
				engine::external_call_guard scope(eng);
				script_value old_sv = script_value_from_cpp(old_val, eng);
				script_value new_sv = script_value_from_cpp(new_val, eng);
				throw_script_callback_failure(eng, script_func({old_sv, new_sv}));
			};

			auto recv = obs_prop->on_change.connect(std::move(cpp_callback));
			recv->bind_engine(eng, engine_lifetime_token(eng));
			prop_mgr->template track_receiver<void(const ValueT&, const ValueT&)>(recv);

			return script_value(std::monostate{}, eng);
		})
		.build();
}

template<typename Getter, typename Setter>
void register_schema_property(engine& a_engine, class_definition& class_def, serialization::class_metadata& metadata,
		const std::string& name, const schema_property_access& access, Getter getter, Setter setter) {
	class_def.add_field(name, script_value(std::monostate{}, &a_engine));

	serialization::property_metadata prop_meta;
	prop_meta.name = name;
	prop_meta.read_only = false;
	prop_meta.type = nullptr;
	metadata.properties.push_back(prop_meta);

	class_def.add_method("_get_" + name, [getter = std::move(getter), access, native_type_id = class_def.get_type_id()](const std::vector<script_value>& args) -> script_value {
		if (args.empty()) {
			throw runtime_error("Property getter called without 'this' object");
		}
		return getter(access.extract(args[0], native_type_id));
	});
	class_def.register_property_getter(engine_access::symbolize(a_engine, name), engine_access::symbolize(a_engine, "_get_" + name));

	class_def.add_method("_set_" + name, [setter = std::move(setter), access, engine_ptr = &a_engine, native_type_id = class_def.get_type_id()](const std::vector<script_value>& args) -> script_value {
		if (args.size() < 2) {
			throw runtime_error("Property setter requires 'this' and value");
		}
		void* cpp_obj = access.extract(args[0], native_type_id);
		setter(cpp_obj, args[1]);
		return script_value(std::monostate{}, engine_ptr);
	});

	uint64_t field_id = engine_access::symbolize(a_engine, name);
	uint64_t setter_id = engine_access::symbolize(a_engine, "_set_" + name);
	class_def.register_property_setter(field_id, setter_id);
	class_def.register_host_member_lane(field_id, nullptr);
}

template<typename ValueT>
void bind_observable_change_method(engine& a_engine, class_definition& class_def, const std::string& prop_name, const schema_property_access& access) {
	engine* eng = &a_engine;
	std::string observe_method_name = "on_" + prop_name + "_change";

	auto method_func = [prop_name, eng, access, native_type_id = class_def.get_type_id()](const std::vector<script_value>& args) -> script_value {
		void* self = access.extract(args[0], native_type_id);

		parameter_storage storage;
		parameter_storage::scope_guard guard(eng, &storage);

		const script_value& callback = args[1];
		if (!callback.is_function()) {
			throw runtime_error("on_" + prop_name + "_change requires a function argument");
		}

		jai::property_manager& mgr = *access.manager(self);
		auto* base_prop = mgr.get(prop_name);
		if (!base_prop) {
			throw runtime_error("Property '" + prop_name + "' not found");
		}

		auto* obs_prop = dynamic_cast<jai::observable_property<ValueT>*>(base_prop);
		if (!obs_prop) {
			throw runtime_error("Property '" + prop_name + "' is not observable");
		}

		const script_function& script_func = callback.as_function();
		auto cpp_callback = [script_func, eng](const ValueT& old_val, const ValueT& new_val) {
			engine::external_call_guard scope(eng);
			script_value old_sv = script_value_from_cpp(old_val, eng);
			script_value new_sv = script_value_from_cpp(new_val, eng);
			throw_script_callback_failure(eng, script_func({old_sv, new_sv}));
		};

		auto recv = obs_prop->on_change.connect(std::move(cpp_callback));
		recv->bind_engine(eng, engine_lifetime_token(eng));
		mgr.template track_receiver<void(const ValueT&, const ValueT&)>(recv);

		return script_value(std::monostate{}, eng);
	};

	class_def.add_method(observe_method_name, engine_access::guard_function(a_engine, std::move(method_func), privilege::none), 1,
		std::vector<param_type_info>{ engine_access::map_cpp_type_to_param_info<script_value>() }, engine_access::map_cpp_type_to_param_info<script_value>());
}

template<typename ValueT>
bool bind_schema_typed(engine& a_engine, class_definition& class_def, serialization::class_metadata& metadata,
		const std::string& prop_name, std::type_index value_type, bool is_observable, const schema_property_access& access) {
	if (value_type != std::type_index(typeid(ValueT))) {
		return false;
	}

	engine* eng = &a_engine;

	if (is_observable) {
		ensure_observable_ref_type_registered<ValueT>(a_engine);

		auto getter = [prop_name, eng, access](void* self) -> script_value {
			auto* base_prop = access.find(self, prop_name);
			if (!base_prop) {
				return script_value(std::monostate{}, eng);
			}
			auto* obs_prop = dynamic_cast<jai::observable_property<ValueT>*>(base_prop);
			if (!obs_prop) {
				return script_value(std::monostate{}, eng);
			}
			auto ref = std::make_shared<jai::observable_property_ref<ValueT>>(obs_prop, access.manager(self));
			return engine_access::make_value_object(*eng, ref);
		};

		auto setter = [prop_name, access, eng](void* self, const script_value& arg) {
			auto val = arg.as<ValueT>();
			jai::property_base* base_prop = access.find(self, prop_name);
			if (!base_prop) return;
			auto* obs_prop = dynamic_cast<jai::observable_property<ValueT>*>(base_prop);
			if (obs_prop) {
				// a load writes state without notifying
				if (eng->loading()) {
					obs_prop->set_silent(std::move(val));
				} else {
					*obs_prop = std::move(val);
				}
			}
		};

		register_schema_property(a_engine, class_def, metadata, prop_name, access, std::move(getter), std::move(setter));
		bind_observable_change_method<ValueT>(a_engine, class_def, prop_name, access);
	} else {
		auto getter = [prop_name, eng, access](void* self) -> script_value {
			if (auto* prop = dynamic_cast<jai::property<ValueT>*>(access.find(self, prop_name))) {
				return script_value_from_cpp(prop->get(), eng);
			}
			return script_value(std::monostate{}, eng);
		};

		auto setter = [prop_name, access](void* self, const script_value& arg) {
			auto val = arg.as<ValueT>();
			jai::property_base* base_prop = access.find(self, prop_name);
			if (!base_prop) return;
			auto* regular_prop = dynamic_cast<jai::property<ValueT>*>(base_prop);
			if (regular_prop) {
				regular_prop->get() = std::move(val);
			}
		};

		register_schema_property(a_engine, class_def, metadata, prop_name, access, std::move(getter), std::move(setter));
		class_def.register_host_member_lane(engine_access::symbolize(a_engine, prop_name), make_schema_member_lane<ValueT>(prop_name, access, class_def.get_type_id()));
	}

	return true;
}

} // namespace

void bind_pending_math_properties(engine& a_engine, class_definition& class_def, serialization::class_metadata& metadata,
	std::vector<pending_math_property>& pending) {
	for (const pending_math_property& entry : pending) {
		if (class_def.defines_method(engine_access::symbolize(a_engine, entry.name))) continue;
		bind_schema_math_property(a_engine, class_def, metadata, entry.name, entry.bridge, entry.access);
	}
	pending.clear();
}

void register_read_only_setter(engine& a_engine, class_definition& class_def, const std::string& name) {
	const uint64_t field_id = engine_access::symbolize(a_engine, name);   // interned here, never on the error path
	const uint64_t setter_id = engine_access::symbolize(a_engine, "_set_" + name);
	class_def.add_method("_set_" + name, [field_id](const std::vector<script_value>&) -> checked_result<script_value> {
		return checked_result<script_value>(make_error_code(runtime_error_code::invalid_assignment_target),
		                                    "Cannot assign to '{0}': it is read-only", field_id);
	});
	class_def.register_property_setter(field_id, setter_id);
	class_def.register_property_read_only(field_id);
}

void bind_schema_math_property(engine& a_engine, class_definition& class_def, serialization::class_metadata& metadata,
		const std::string& prop_name, const math_property_bridge& bridge, const schema_property_access& access) {
	engine* eng = &a_engine;
	auto getter = [prop_name, bridge, eng, access](void* self) -> script_value {
		const jai::property_base* prop = access.find(self, prop_name);
		const void* host = prop ? prop->erased_value_ptr() : nullptr;
		if (!host) {
			return script_value(std::monostate{}, eng);
		}
		uint32_t words[4] = {0, 0, 0, 0};
		bridge.read(host, words);
		return script_value::make_vec(bridge.kind, words, eng);
	};
	auto setter = [prop_name, bridge, access](void* self, const script_value& arg) {
		uint32_t words[4] = {0, 0, 0, 0};
		if (!arg.math_words_as(bridge.kind, words)) {
			detail::throw_conversion_error(arg, math_type_name(bridge.kind));
		}
		if (jai::property_base* prop = access.find(self, prop_name)) {
			bridge.assign(*prop, words);
		}
	};
	register_schema_property(a_engine, class_def, metadata, prop_name, access, std::move(getter), std::move(setter));
}

bool bind_schema_primitive_property(engine& a_engine, class_definition& class_def, serialization::class_metadata& metadata,
		const std::string& prop_name, std::type_index value_type, bool is_observable, const schema_property_access& access) {
	return bind_schema_typed<int>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<float>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<double>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<bool>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<std::string>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<int64_t>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<uint64_t>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<int32_t>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<uint32_t>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<int16_t>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<uint16_t>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<int8_t>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access) ||
		bind_schema_typed<uint8_t>(a_engine, class_def, metadata, prop_name, value_type, is_observable, access);
}

std::shared_ptr<class_definition> make_class_definition(std::string_view name, uint64_t type_id, engine* eng) {
	return std::make_shared<class_definition>(name, type_id, eng);
}

strong_ptr<class_instance> make_native_instance(class_definition& def, std::string_view name, std::shared_ptr<void> object, engine* eng) {
	auto instance = def.create_instance();
	instance->set_native(script_value::make_cpp_object(name, def.get_type_id(), std::move(object), eng));
	return instance;
}

void note_region_made(const script_value& value, engine* eng) {
	const script_value& self = value.deref();
	if (self.raw_storage_index() != script_value::TYPEID_OBJECT) { return; }
	const auto holder = self.get_object_holder();
	if (holder && holder->is_instance()) {
		holder->unchecked_instance()->region_tag(engine_access::parallel_region_serial(*eng));
	}
}

script_value bound_class_conversion::wrap(std::shared_ptr<void> object, engine* eng) const {
	if (!object) {
		return script_value(std::monostate{}, eng);
	}
	return script_value::make_object(name, make_native_instance(*def, name, std::move(object), eng), eng);
}

std::shared_ptr<void> bound_class_conversion::unwrap_shared(const script_value& v, const std::type_info& type) const {
	if (v.is_null()) {
		return nullptr;
	}
	auto holder = v.get_object_holder();
	if (!holder) {
		throw_concat("Cannot convert script_value to shared_ptr<", name, ">");
	}
	return native_cast_or_throw(checked_native_object(holder->owner(), *holder, type, def->get_type_id()), holder->owner());
}

void bound_class_conversion::require_object(const script_value& v) const {
	if (!v.get_object_holder()) {
		throw_concat("Cannot convert non-object to ", def->get_name());
	}
}

void ensure_signal_view_type_registered(engine& a_engine) {
	if (a_engine.is_type_registered("Signal")) { return; }
	engine* eng = &a_engine;
	dynamic_binder<signal_script_ops> builder(a_engine, "Signal");
	builder.method("connect", [eng](signal_script_ops& self, const std::string& id, const script_value& cb) {
		if (!self.valid()) { throw runtime_error("Signal view is not backed by a live signal"); }
		self.connect_fn(self.source, id, cb, eng);
	});
	builder.method("disconnect", [](signal_script_ops& self, const std::string& id) {
		if (self.disconnect_fn != nullptr) { self.disconnect_fn(self.source, id); }
	});
	builder.method("connected", [](signal_script_ops& self, const std::string& id) {
		return self.connected_fn != nullptr && self.connected_fn(self.source, id);
	});
	builder.serial_method("connect").serial_method("disconnect");   // a receiver list other objects' code reads and fires
	builder.build();
}

script_value make_signal_view(engine* eng, const signal_script_ops& ops) {
	return engine_access::make_value_object(*eng, std::make_shared<signal_script_ops>(ops));
}

} // namespace jai::detail

namespace jai {

template void property_manager::save<serialization::json_archive_writer>(serialization::json_archive_writer&) const;
template void property_manager::save<serialization::binary_archive_writer>(serialization::binary_archive_writer&) const;
template void property_manager::load<serialization::json_archive_reader>(serialization::json_archive_reader&);
template void property_manager::load<serialization::binary_archive_reader>(serialization::binary_archive_reader&);

} // namespace jai
