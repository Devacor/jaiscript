// Type arguments and records (docs/site/guide/06-functions.html "Type arguments", docs/site/embedding/17-host-api.html
// "Records"): reflect::type, the value a type argument passes, and the helpers that build a script class instance from named
// values and read one back
#include <jaiscript/core/records.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/core/script_class.hpp>
#include <jaiscript/detail/type_value.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/container_enforce.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/execution_limits.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/stdlib/json.hpp>
#include <exception>
#include <optional>
#include <string>

namespace jai {

std::string script_type::name() const {
	return type_ ? type_->canonical_name() : std::string();
}

std::string script_type::class_name() const {
	const type_info* t = type_.get();
	if (t && t->base_type == script_value_type::jai_shared_ptr_type) { t = t->element_type().get(); }
	if (!t || t->base_type != script_value_type::jai_object_type || t->is_enum) { return {}; }
	return t->type_name;
}

namespace records_detail {

	using R = checked_result<script_value>;

	JAI_NOINLINE R refused(engine& eng, std::string_view who, const std::string& text) {
		return R(make_error_code(runtime_error_code::type_mismatch), "{0}",
			detail::engine_access::park_error_text(eng, std::string(who) + ": " + text));
	}

	// a value's type as a script spells it: a class by name, a typed container by its element types
	// An archive record the load left a map ({"$type": name, "$val": {...}} naming no class of the program): the name it gives
	const std::string* unread_record_type(const script_value& v) {
		return v.is_map() ? stdlib::unread_instance_record(v.as_map()) : nullptr;
	}

	std::string value_type_text(const script_value& a_value) {
		const script_value& v = a_value.deref();
		if (const std::string* named = unread_record_type(v)) { return *named; }
		switch (v.raw_storage_index()) {
			case script_value::TYPEID_NULL: return "null";
			case script_value::TYPEID_BOOL: return "bool";
			case script_value::TYPEID_INT: return "int";
			case script_value::TYPEID_FLOAT: return "float";
			case script_value::TYPEID_CHAR: return "char";
			case script_value::TYPEID_STRING: return "string";
			case script_value::TYPEID_UTF8_STRING: return "utf8_string";
			default: break;
		}
		if (v.raw_storage_index() == script_value::TYPEID_WEAK_PTR) { return detail::weak_pointer_spelling(v); }
		if (v.is_object()) {
			if (auto instance = const_cast<script_value&>(v).get_class_instance(); instance && instance->get_class_definition()) {
				return instance->get_class_definition()->get_name();
			}
		}
		if (const type_info_ptr& tag = v.get_type_info()) { return tag->spelling(); }
		if (v.is_array()) { return "array"; }
		if (v.is_map()) { return "map"; }
		if (v.is_function()) { return "function"; }
		return "object";
	}

	struct record_field {
		uint64_t id = 0;
		std::string_view name;
		type_info_ptr type;
	};

	// The instance fields of a class and its bases, bases first, in declaration order
	void collect_fields(engine& eng, const class_definition* def, std::vector<record_field>& out) {
		for (const auto& parent : def->get_parent_classes()) {
			if (parent) { collect_fields(eng, parent.get(), out); }
		}
		for (uint64_t id : def->field_order()) {
			if (def->is_static_field(id)) { continue; }
			out.push_back({id, detail::engine_access::get_symbolizer(eng)->get_string(id), def->get_field_declared_type(id)});
		}
	}

	// The text a JSON field holds, read as from_json<T> reads it: its archive records by the record's rule
	script_value parse_json(engine& eng, const std::string& text, std::string_view who) {
		detail::engine_access::load_rule rule(&eng, who);
		return stdlib::parse_json_text(&eng, text);
	}

	JAI_NOINLINE R failed(engine& eng, const std::string& text) {
		return R(make_error_code(runtime_error_code::type_mismatch), "{0}", detail::engine_access::park_error_text(eng, text));
	}

	bool script_class_type(const type_info* t) {
		return t && t->base_type == script_value_type::jai_object_type && !t->is_enum && !t->type_name.empty();
	}

	R build(engine& eng, const type_info_ptr& type, const script_value& fields, std::string_view who, bool shared);

	// What a stored value converts from beyond a script store: a row's 0/1 bool and int float, JSON text, a map for a class
	R widen(engine& eng, const script_value& a_value, const type_info_ptr& declared, std::string_view who) {
		const type_info* t = declared.get();
		const script_value& v = a_value.deref();
		if (!t) { return v; }
		switch (t->base_type) {
			case script_value_type::jai_bool_type:
				if (v.is_int() && (v.as_int() == 0 || v.as_int() == 1)) { return script_value(v.as_int() == 1, &eng); }
				return v;
			case script_value_type::jai_float_type:
				if (v.is_int()) { return script_value(static_cast<script_float>(v.as_int()), &eng); }
				return v;
			case script_value_type::jai_array_type: case script_value_type::jai_map_type: case script_value_type::jai_object_type:
			case script_value_type::jai_shared_ptr_type:
				break;
			default:
				return v;
		}
		if (t->base_type == script_value_type::jai_object_type && !script_class_type(t)) { return v; }
		if (const std::string* text = v.text_bytes()) {
			std::optional<script_value> parsed;
			try {
				parsed.emplace(parse_json(eng, *text, who));
			} catch (const stdlib::typed_load_refusal& refusal) {
				return failed(eng, refusal.what());
			} catch (const std::exception& e) {
				return refused(eng, who, "text for '" + t->canonical_name() + "' is not JSON: " + e.what());
			}
			return widen(eng, *parsed, declared, who);
		}
		if (detail::native_stack_low()) [[unlikely]] { return detail::raise_native_stack(); }
		if (unread_record_type(v)) { return v; }   // it names another type: the store refuses it as that type
		if (script_class_type(t) && v.is_map()) { return build(eng, declared, v, who, false); }
		if (t->base_type == script_value_type::jai_shared_ptr_type && v.is_map() && script_class_type(t->element_type().get())) {
			return build(eng, t->element_type(), v, who, true);
		}
		if (t->base_type == script_value_type::jai_array_type && v.is_array() && t->element_type()) {
			script_value out = script_value::make_array(nullptr, &eng);
			std::optional<R> failed;
			v.unchecked_array_node()->for_each(&eng, [&](const script_value& element) {
				if (failed) { return; }
				R widened = widen(eng, element, t->element_type(), who);
				if (!widened) { failed.emplace(std::move(widened)); return; }
				out.as_array().push_back(std::move(widened.value()));
			});
			if (failed) { return std::move(*failed); }
			return out;
		}
		if (t->base_type == script_value_type::jai_map_type && v.is_map() && t->value_type()) {
			script_value out = script_value::make_map(nullptr, nullptr, &eng);
			for (const auto& [key, item] : v.as_map()) {
				R widened = widen(eng, item, t->value_type(), who);
				if (!widened) { return widened; }
				out.as_map()[key] = std::move(widened.value());
			}
			return out;
		}
		return v;
	}

	// One named value into a record's field, by the record's rule: widened, then stored as a script store would, or refused
	R store_field(engine& eng, class_instance& instance, const std::string& class_name, const record_field& field, const script_value& given, std::string_view who) {
		R widened = widen(eng, given, field.type, who);
		if (!widened) { return widened; }
		const bool null_given = widened.value().deref().is_null();
		const std::string given_type = value_type_text(widened.value());
		auto stored = instance.enforce_field_write(field.id, std::move(widened.value()));
		if (!stored) {
			const std::string declared_type = field.type ? field.type->spelling() : std::string("var");
			if (null_given) { return refused(eng, who, "field '" + std::string(field.name) + "' of '" + class_name + "' is " + declared_type + " and cannot be null"); }
			return refused(eng, who, "field '" + std::string(field.name) + "' of '" + class_name + "' is " + declared_type + ", not " + given_type);
		}
		instance.set_field_unchecked(field.id, stored.value());
		return script_value(std::monostate{}, &eng);
	}

	R build(engine& eng, const type_info_ptr& type, const script_value& fields, std::string_view who, bool shared) {
		const std::string& name = type->type_name;
		const auto def = detail::engine_access::get_class_definition(eng, name);
		if (!def) { return refused(eng, who, "unknown class '" + name + "'"); }
		if (!def->is_script_class()) { return refused(eng, who, "'" + name + "' is not a script class"); }
		const auto& script_def = static_cast<const script_class_definition&>(*def);
		bool default_constructible = script_def.get_constructor_asts().empty();
		for (const auto& ctor : script_def.get_constructor_asts()) {
			if (ctor->parameters.empty() || ctor->parameters.front().default_value) { default_constructible = true; break; }
		}
		auto ctor = detail::engine_access::get_global_environment(eng)->get(name);
		if (!default_constructible || !ctor || !ctor.value().is_function()) {
			return refused(eng, who, "'" + name + "' has no constructor taking no arguments");
		}
		R made = [&]() {
			detail::engine_access::construction_access_waiver waiver(&eng);   // a record reaches a private T() as reflection does
			detail::engine_access::load_callout callout(&eng);
			return ctor.value().as_function()({});
		}();
		if (!made) { return made; }
		script_value record = std::move(made.value());
		auto instance = record.get_class_instance();
		if (!instance) { return refused(eng, who, "'" + name + "' did not construct an object"); }
		std::vector<record_field> declared;
		collect_fields(eng, def.get(), declared);
		const script_map& given = fields.deref().as_map();
		for (const record_field& field : declared) {
			auto found = given.find(script_value(std::string(field.name), &eng));
			if (found == given.end()) { continue; }
			R stored = store_field(eng, *instance, name, field, found->second, who);
			if (!stored) { return stored; }
		}
		if (shared) {   // make_shared<T>()'s answer: the handle spelling marks the object, as reflect::construct_shared does
			type_info* pointee = record.get_type_info().get();
			if (!pointee) { pointee = def->get_type_info().get(); }
			record.set_type_info(detail::engine_access::get_type_info_shared_ptr(eng, pointee));
		}
		return record;
	}

	script_value fields_map(engine& eng, const record_field_list& fields) {
		script_value map = script_value::make_map(nullptr, nullptr, &eng);
		for (const auto& [name, value] : fields) { map.as_map()[script_value(name, &eng)] = value; }
		return map;
	}

	R make(engine& eng, const script_type& type, const script_value& fields, std::string_view who, bool shared) {
		const type_info_ptr t = type.info();
		if (!script_class_type(t.get())) {
			return refused(eng, who, "a record is an object of a script class, not " + (t ? t->canonical_name() : std::string("an empty type")));
		}
		if (!fields.is_map()) { return refused(eng, who, "a record's fields are a map, not " + value_type_text(fields)); }
		return build(eng, t, fields, who, shared);
	}

	// a stored container or object: its JSON text
	script_value storable(engine& eng, const script_value& a_value) {
		const script_value& v = a_value.deref();
		if (!v.is_array() && !v.is_map() && !v.is_object()) { return v; }
		serialization::json_archive_writer writer(-1, &eng);
		writer.write_value(v);
		return script_value(writer.take(), &eng);
	}

} // namespace records_detail

checked_result<script_value> make_record(engine& eng, const script_type& type, const record_field_list& fields, std::string_view who) {
	return records_detail::make(eng, type, records_detail::fields_map(eng, fields), who, false);
}

checked_result<script_value> make_record(engine& eng, const script_type& type, const script_value& fields, std::string_view who) {
	return records_detail::make(eng, type, fields, who, false);
}

checked_result<script_value> make_shared_record(engine& eng, const script_type& type, const record_field_list& fields, std::string_view who) {
	return records_detail::make(eng, type, records_detail::fields_map(eng, fields), who, true);
}

checked_result<record_field_list> record_fields(engine& eng, const script_value& a_record, std::string_view who) {
	const script_value& record = a_record.deref();
	record_field_list out;
	if (record.is_map()) {
		for (const auto& [key, item] : record.as_map()) {
			if (const std::string* name = key.text_bytes()) { out.emplace_back(*name, records_detail::storable(eng, item)); }
		}
		return out;
	}
	auto instance = record.is_object() ? const_cast<script_value&>(record).get_class_instance() : strong_ptr<class_instance>();
	const class_definition* def = instance ? instance->get_class_definition() : nullptr;
	if (!def || !def->is_script_class()) {
		return checked_result<record_field_list>(make_error_code(runtime_error_code::type_mismatch), "{0}", detail::engine_access::park_error_text(eng,
			std::string(who) + ": a record is an object of a script class or a map, not " + records_detail::value_type_text(record)));
	}
	std::vector<records_detail::record_field> declared;
	records_detail::collect_fields(eng, def, declared);
	for (const auto& field : declared) {
		const script_value* value = instance->find_field_value(field.id);
		out.emplace_back(std::string(field.name), value ? records_detail::storable(eng, *value) : script_value(std::monostate{}, &eng));
	}
	return out;
}

namespace detail {

	script_value make_type_value(const type_info_ptr& type, engine* eng) {
		return value_converter<script_type>::to(script_type(type), eng);
	}

	checked_result<script_value> json_to_type(engine& eng, const script_value& value, const type_info_ptr& type, std::string_view who, std::string_view document) {
		if (records_detail::script_class_type(type.get()) && value.is_map() && !records_detail::unread_record_type(value)) {
			return records_detail::build(eng, type, value, who, false);
		}
		auto widened = records_detail::widen(eng, value, type, who);
		if (!widened) { return widened; }
		const std::string spelled = type ? type->canonical_name() : std::string();
		auto stored = typed_store_convert(widened.value(), type, &eng, [&]() { return slot_site{slot_kind::none, {}, {}}; });
		if (!stored) {
			return records_detail::refused(eng, who, std::string(document) + " is " + records_detail::value_type_text(widened.value()) + ", not " + spelled);
		}
		script_value out = std::move(stored.value());
		if (type && type->base_type != script_value_type::jai_any_type) { out.set_type_info(type); }
		return out;
	}

	void store_typed_member(engine& eng, class_instance& instance, const class_definition& def, uint64_t field_id, const script_value& value, std::string_view who) {
		const records_detail::record_field field{field_id, engine_access::get_symbolizer(eng)->get_string(field_id), def.get_field_declared_type(field_id)};
		auto stored = records_detail::store_field(eng, instance, def.get_name(), field, value, who);
		if (!stored) { throw stdlib::typed_load_refusal(eng.format_error(stored)); }
	}

	void throw_typed_member_refusal(engine& eng, const class_definition& def, uint64_t field_id, const type_info* declared, const script_value& value, std::string_view who) {
		throw stdlib::typed_load_refusal(std::string(who) + ": field '" + std::string(engine_access::get_symbolizer(eng)->get_string(field_id)) + "' of '" + def.get_name() + "' is " +
		                                 (declared ? declared->spelling() : std::string("var")) + ", not " + records_detail::value_type_text(value));
	}

	void register_type_library(engine& eng) {
		dynamic_binder<script_type>(eng, "reflect::type")
			.method("name", &script_type::name)
			.method("to_string", &script_type::to_string)
			.build();
	}

} // namespace detail

} // namespace jai
