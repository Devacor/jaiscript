#include <jaiscript/stdlib/json.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/value.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/work_budget.hpp>
#include <jaiscript/detail/coroutine_snapshot.hpp>
#include <jaiscript/detail/type_name_key.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/detail/generics.hpp>
#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/detail/type_value.hpp>
#include <jaiscript/core/script_type.hpp>
#include <jaiscript/serialization/binary_archive.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <algorithm>
#include <memory>
#include <limits>
#include <optional>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <atomic>

namespace jai {
namespace stdlib {

    // A host class whose binder registered a constructor that takes no arguments
    inline bool host_has_default_constructor(engine* eng, const std::string& class_name) {
        std::vector<engine::host_overload_signature> signatures;
        if (!eng->host_function_signatures(class_name, signatures)) { return false; }
        for (const auto& signature : signatures) {
            if (signature.arity == 0 || signature.arity == SIZE_MAX) { return true; }
        }
        return false;
    }

    // A script class over a C++ base with no zero-argument constructor: the C++ base is built the way the archive layer
    // builds a host type, through its registry entry (load_and_construct, else its default constructor whatever its
    // access) from the saved object, or else through a default constructor its binder registered. The result is owned as
    // a hybrid the script constructed (the layer owns its C++ object, whose slot points back) and its field initializers run.
    inline script_value load_hybrid(engine* eng, const script_map& map, const std::shared_ptr<class_definition>& class_def,
                                    const class_definition& base) {
        const std::string& base_name = base.get_name();
        std::shared_ptr<void> object;
        const auto* entry = serialization::polymorphic_registry::instance().find(base_name);
        if (!entry) {
            entry = serialization::binding_entry_named(eng, base_name);
        }
        if (entry && entry->load_fn) {
            script_value host_part = script_value::make_map(detail::engine_access::get_type_info_string(*eng), nullptr, eng);
            auto& host_map = const_cast<script_map&>(host_part.as_map());
            for (const auto& [key, value] : map) {
                host_map.emplace(key, value);
            }
            serialization::json_archive_writer writer(-1, eng);
            writer.write_value(host_part);
            serialization::json_archive_reader reader(writer.str(), eng);
            reader.begin_object();
            serialization::any_archive_reader wrapper(reader);
            object = entry->load_fn(wrapper, 1);
            reader.end_object();
        } else if (host_has_default_constructor(eng, base_name)) {
            auto made = [&]() {
                detail::engine_access::load_callout callout(eng);
                return eng->get_variable(base_name).as_function()({});
            }();
            if (!made) {
                throw runtime_error(eng->format_error(made));
            }
            auto holder = made.value().get_object_holder();
            object = holder ? holder->host_object() : nullptr;
        } else {
            throw runtime_error("Cannot load hybrid script class '" + class_def->get_name() + "': its C++ base '" + base_name +
                                "' is not default constructible and has no load_and_construct");
        }
        auto layer = object ? class_def->create_instance() : nullptr;
        if (!layer) {
            throw runtime_error("Failed to reconstruct hybrid script class '" + class_def->get_name() + "'");
        }
        layer->set_native(script_value::make_cpp_object(base_name, base.get_type_id(), object, eng));
        detail::engine_access::get_execution_backend(*eng)->apply_field_initializers(layer, class_def);
        return script_value::make_class_object(*class_def, layer, eng);
    }

    // A saved value that does not fit its field: converted as hot reload converts, or the constructed value stays; both
    // are recorded where hot reload records a retype
    inline void record_load_retype(engine* eng, const class_definition& def, uint64_t id, field_migration_outcome outcome,
                                   const script_value& saved, const script_value* result) {
        detail::engine_access::record_field_migration(*eng, def.get_name(), id, outcome, &saved, result);
    }

    // One saved member onto a constructed object (the rebuilder and load_saved_fields share it); false when a store threw
    // into the backend
    bool store_saved_member(engine* eng, const script_value& instance, class_instance& inst, class_definition& def,
                            const detail::engine_access::load_field& field, script_value&& v) {
        const uint64_t member_id = field.id;
        // a typed load keeps no constructed value in place of a saved one: what the archive's conversions do not take is
        // stored by the record's rule, as the member of an object written without a $type is, or refused
        const std::string_view typed_who = detail::engine_access::typed_load_who(*eng);
        if (const script_value* stored = inst.find_script_field_value(member_id)) {
            const type_info_ptr declared = def.get_field_declared_type(member_id);
            // a saved null goes the way any saved value does: a handle or a function field takes it as `= null` stores it (an
            // expired weak handle saves as null), and a field of any other declared type holds no null
            if (declared && declared->base_type != script_value_type::jai_any_type) {
                auto converted = detail::field_store_convert_no_script(v, declared, eng);
                if (!converted) {
                    if (!typed_who.empty()) {
                        detail::store_typed_member(*eng, inst, def, member_id, v, typed_who);
                        return !detail::engine_access::backend_unwinding(*eng);
                    }
                    record_load_retype(eng, def, member_id, field_migration_outcome::reset, v, stored);
                    return true;
                }
                record_load_retype(eng, def, member_id, field_migration_outcome::converted, v, &*converted);
                inst.set_field(member_id, *converted);
            } else {
                inst.set_field(member_id, v);
            }
            return true;
        }
        if (def.script_declares_user_accessor(field.setter_id)) { return true; }   // an accessor with no field holds no state
        script_value setter = inst.get_method(field.setter_id, false);
        if (!setter.is_function()) { return true; }
        auto written = [&]() {
            detail::engine_access::load_callout callout(eng);
            return setter.as_function()({ instance, v });
        }();
        if (!written) {
            // a binding refuses a value of another type by throwing: a saved value its current value's type would not take
            // is a retype (the constructed value stays); a value it would take is the host's own error
            script_value current(std::monostate{}, eng);
            const std::string prop_name(detail::engine_access::get_symbolizer(*eng)->get_string(member_id));
            if (script_value getter = inst.get_method(detail::engine_access::symbolize(*eng, "_get_" + prop_name), false); getter.is_function()) {
                detail::engine_access::load_callout callout(eng);
                if (auto read = getter.as_function()({ instance })) { current = read.value().try_unwrap_transparent_wrapper().value_or(read.value()); }
            }
            const type_info_ptr kind = current.is_null() ? nullptr : current.get_type_info();
            const bool fits = !kind || detail::field_store_convert_no_script(v, kind, eng).has_value();
            if (written.error() == make_error_code(runtime_error_code::cpp_exception) && fits) {
                throw runtime_error(eng->format_error(written));
            }
            if (!typed_who.empty()) { detail::throw_typed_member_refusal(*eng, def, member_id, kind.get(), v, typed_who); }
            record_load_retype(eng, def, member_id, field_migration_outcome::reset, v, current.is_null() ? nullptr : &current);
        }
        return !detail::engine_access::backend_unwinding(*eng);
    }

    void load_saved_fields(engine* eng, const script_value& instance, const script_map& saved,
                           const std::function<script_value(const script_value&)>& transform) {
        strong_ptr<class_instance> inst = instance.is_object() ? const_cast<script_value&>(instance).get_class_instance() : nullptr;
        class_definition* def = inst ? inst->get_class_definition() : nullptr;
        if (!def) { return; }
        // a load in progress for the whole walk: the field order stays cached while nested objects load
        detail::engine_access::load_session session(eng);
        for (const detail::engine_access::load_field& field : detail::engine_access::load_field_order(*eng, *def)) {
            auto it = saved.find(field.key);
            if (it == saved.end()) { continue; }
            script_value v = transform ? transform(it->second) : it->second.clone();
            if (!store_saved_member(eng, instance, *inst, *def, field, std::move(v))) { return; }
        }
        session.complete();
    }

	// Every archive format restores nested script classes through the same walk, on an explicit frame stack: a payload nests
	// as deep as memory allows. Serialized names remain data; reconstruction uses only registered classes and members.
	// A handle to a loaded instance (what new T() yields): a load that aliases one object hands out the same handle
	inline script_value script_handle_of(engine* eng, script_value object) {
		const auto type = object.get_type_info();
		if (object.is_object() && type && type->base_type == script_value_type::jai_object_type) {
			object.set_type_info(detail::engine_access::get_type_info_shared_ptr(*eng, type.get()));
		}
		return object;
	}

	// One map's members by their archive keys: a key outside the set makes it no archive record
	struct archive_record_keys {
		const script_value* id = nullptr;
		const script_value* ref = nullptr;
		const script_value* type = nullptr;
		const script_value* version = nullptr;
		const script_value* val = nullptr;
	};

	inline bool read_archive_record_keys(const script_map& map, archive_record_keys& keys) {
		if (map.empty() || map.size() > 3) { return false; }
		for (const auto& [key, member] : map) {
			if (!key.is_string()) { return false; }
			const std::string& name = key.as_string();
			const script_value** slot = name == "$id" ? &keys.id : name == "$ref" ? &keys.ref : name == "$type" ? &keys.type
			                          : name == "$version" ? &keys.version : name == "$val" ? &keys.val : nullptr;
			if (!slot) { return false; }
			*slot = &member;
		}
		return true;
	}

	const std::string* unread_instance_record(const script_map& map) {
		archive_record_keys keys;
		if (!read_archive_record_keys(map, keys) || keys.id || keys.ref || !keys.type || !keys.val) { return nullptr; }
		if (!keys.type->is_string() || !keys.val->is_map() || (keys.version && !keys.version->is_int())) { return nullptr; }
		return keys.type->text_bytes();
	}

	// A handle: {"$id": N} or {"$ref": N}, and at its object's first reference the object's "$type" beside its "$val". The
	// payload is the object's own archive form ({"$type": T, "$val": fields}), or the bare "$val" of an object with no class
	inline bool archive_handle_parts(engine* eng, const script_map& map, script_int& id, script_value& payload, bool& has_payload, bool& weak) {
		archive_record_keys keys;
		if (!read_archive_record_keys(map, keys) || keys.version || (keys.id != nullptr) == (keys.ref != nullptr)) { return false; }
		const script_value* id_value = keys.id ? keys.id : keys.ref;
		if (!id_value->is_int() || (keys.type && (!keys.type->is_string() || !keys.val))) { return false; }
		weak = keys.ref != nullptr;
		id = id_value->as_int();
		has_payload = keys.val != nullptr;
		if (keys.type) {
			payload = script_value::make_map(detail::engine_access::get_type_info_string(*eng), nullptr, eng);
			auto& object = const_cast<script_map&>(payload.as_map());
			object.emplace(script_value(std::string("$type"), eng), *keys.type);
			object.emplace(script_value(std::string("$val"), eng), *keys.val);
		} else if (keys.val) {
			payload = *keys.val;
		}
		return true;
	}

	inline void index_archive_handles(engine* eng, const script_value& root, archive_handles& handles) {
		std::vector<script_value> pending{root};
		detail::work_meter work(eng);
		while (!pending.empty()) {
			if (work.spend(1)) [[unlikely]] { work.throw_spent(); }
			script_value value = std::move(pending.back());
			pending.pop_back();
			if (value.is_map()) {
				const auto& map = value.as_map();
				script_int id = 0;
				script_value payload(std::monostate{}, eng);
				bool has_payload = false;
				bool weak = false;
				if (archive_handle_parts(eng, map, id, payload, has_payload, weak) && has_payload && id != 0) {
					handles.payloads.try_emplace(id, std::move(payload));
				}
				for (const auto& [key, child] : map) {
					if (child.is_map() || child.is_array()) { pending.push_back(child); }
				}
			} else if (value.is_array()) {
				value.unchecked_array_node()->for_each(eng, [&](const script_value& child) {
					if (child.is_map() || child.is_array()) { pending.push_back(child); }
				});
			}
		}
	}

	[[noreturn]] JAI_NOINLINE inline void throw_unwritten_handle(script_int id) {
		throw serialization_error("Invalid shared_ptr ID: " + std::to_string(id));
	}

	// the type parameters of the generic class a name declares, or -1 when none does
	inline int generic_class_parameters(engine& eng, std::string_view name) {
		const auto* declared = detail::engine_access::find_generics(eng, std::string(name));
		if (!declared) { return -1; }
		for (const auto& g : *declared) {
			if (g->kind == detail::generic_kind::class_type) { return static_cast<int>(g->parameters.size()); }
		}
		return -1;
	}

	inline std::string type_argument_count_text(std::string_view name, size_t wanted, size_t given) {
		return "'" + std::string(name) + "' takes " + std::to_string(wanted) + (wanted == 1 ? " type argument" : " type arguments") + ", not " + std::to_string(given);
	}

	// What a name that takes no type arguments is, as the checker tells it (none: no type of the name)
	inline detail::plain_type_kind data_plain_type(engine& eng, std::string_view name) {
		if (detail::builtin_type_name(name)) { return detail::plain_type_kind::builtin; }
		if (const auto id = detail::engine_access::get_symbolizer(eng)->find(name); id && detail::engine_access::is_enum_name(eng, *id)) { return detail::plain_type_kind::enumeration; }
		if (const auto def = detail::engine_access::get_class_definition(eng, std::string(name))) {
			return def->is_cpp_class() ? detail::plain_type_kind::host_class : detail::plain_type_kind::script_class;
		}
		return detail::plain_type_kind::none;
	}

	// Why a name arriving as text (a whitespace-free key whose generic class is `generic`) spells no instance, in the checker's
	// words for the same spelling; empty when it spells one the program could make. One pass with an explicit stack: a name
	// nests as deep as the data does.
	inline std::string instance_spelling_mistake(engine& eng, std::string_view key, std::string_view generic) {
		const std::string malformed = "it is not the spelling of a type";
		const size_t open = generic.size();
		// the instance's own list first: a count it does not take is the mistake, whatever its arguments are
		size_t root_arguments = 0;
		int depth = 0;
		size_t close = open;
		for (; close < key.size(); ++close) {
			const char c = key[close];
			if (c == '<' || c == '(') { ++depth; }
			else if (c == '>' || c == ')') { if (--depth == 0) { break; } }
			else if (c == ',' && depth == 1) { ++root_arguments; }
		}
		if (close == key.size()) { return malformed; }
		if (close > open + 1) { ++root_arguments; }
		if (const int wanted = generic_class_parameters(eng, generic); root_arguments != static_cast<size_t>(wanted)) {
			return type_argument_count_text(generic, static_cast<size_t>(wanted), root_arguments);
		}
		struct argument_list {
			std::string_view name;
			size_t wanted = 0;
			size_t given = 0;
			bool paren = false;     // a signature's parameters, closed by ')'
			bool skipped = false;   // a signature's or a host template's arguments, which the data's name does not check
			bool instance = false;  // a generic class's: its spelling joins them with ", "
		};
		std::vector<argument_list> lists;
		size_t spelled_size = key.size();   // the instance's name as the engine spells it, which an instance's limit counts
		bool closed_one = false;    // the token before this delimiter was a list that closed, not a name
		size_t start = 0;
		for (size_t i = 0; i <= key.size(); ++i) {
			const char c = i < key.size() ? key[i] : '\0';
			const bool delimiter = i == key.size() || c == '<' || c == '>' || c == ',' || c == '(' || c == ')';
			if (!delimiter) { continue; }
			const std::string_view name = key.substr(start, i - start);
			start = i + 1;
			const bool skipping = !lists.empty() && lists.back().skipped;
			if (c == '<' || c == '(') {
				if (closed_one) { return malformed; }
				argument_list list{name};
				list.paren = c == '(';
				if (skipping || name == "function" || name == "coroutine") {
					list.skipped = true;
				} else if (c == '(') {
					return malformed;
				} else if (name == "array" || name == "shared_ptr" || name == "weak_ptr" || name == "map") {
					list.wanted = name == "map" ? 2 : 1;
				} else if (const int wanted = generic_class_parameters(eng, name); wanted >= 0) {
					list.wanted = static_cast<size_t>(wanted);
					list.instance = true;
				} else if (detail::engine_access::is_template_type(eng, std::string(name))) {
					list.skipped = true;
				} else if (const detail::plain_type_kind kind = data_plain_type(eng, name); kind != detail::plain_type_kind::none) {
					return detail::no_type_arguments_text(name, kind);
				} else {
					return name.empty() ? malformed : "unknown type '" + detail::shown_instance_name(name) + "'";
				}
				lists.push_back(list);
				continue;
			}
			if (lists.empty() || i == key.size()) { return malformed; }   // a name outside every list, or a list the key never closes
			const bool empty_list = c == '>' && name.empty() && !closed_one && key[i - 1] == '<';
			if (closed_one && !name.empty()) { return malformed; }
			if (!closed_one && !skipping && !empty_list) {
				if (name.empty()) { return malformed; }
				if (name == "void" || name == "auto") { return "a type argument names a type: '" + std::string(name) + "' is not one an instance can be made with"; }
				if (name == "array" || name == "shared_ptr" || name == "weak_ptr" || name == "map") { return type_argument_count_text(name, name == "map" ? 2 : 1, 0); }
				if (const int wanted = generic_class_parameters(eng, name); wanted >= 0) { return type_argument_count_text(name, static_cast<size_t>(wanted), 0); }
				if (name != "function" && data_plain_type(eng, name) == detail::plain_type_kind::none) { return "unknown type '" + detail::shown_instance_name(name) + "'"; }
			}
			closed_one = false;
			if (!empty_list) { ++lists.back().given; }
			if (c == ',') { continue; }
			const argument_list list = lists.back();
			if (list.paren != (c == ')')) { return malformed; }
			lists.pop_back();
			closed_one = true;
			if (!list.skipped && list.given != list.wanted) { return type_argument_count_text(list.name, list.wanted, list.given); }
			if (list.instance && list.given > 1) { spelled_size += list.given - 1; }
			if (!lists.empty()) { continue; }
			if (i + 1 != key.size()) { return malformed; }
			if (spelled_size > detail::k_generic_name_limit) { return "no instance's name is longer than " + std::to_string(detail::k_generic_name_limit) + " characters"; }
			return std::string();
		}
		return malformed;
	}

	// A $type that names no class is data, except an instance of a declared generic: one a unit offered that still waits for
	// what it is built on, a spelling that names no instance, or one the program never made (docs/site/guide/21-generics.html
	// "One instance, one class"), whose advice names the load the archive came through
	JAI_NOINLINE inline void refuse_unmade_instance(engine& eng, const std::string& type_name, bool binary) {
		if (const std::string waiting = detail::waiting_instance_text(eng, type_name); !waiting.empty()) { throw serialization_error(waiting); }
		const std::string key = detail::type_name_key(type_name);   // a generic's own name is one word: its key is itself
		const size_t open = key.find('<');
		if (open == std::string::npos || open == 0 || key.back() != '>' || !detail::engine_access::declares_generics(eng)) { return; }
		const std::string generic = key.substr(0, open);
		if (generic_class_parameters(eng, generic) < 0) { return; }
		if (const std::string mistake = instance_spelling_mistake(eng, key, generic); !mistake.empty()) {
			throw serialization_error("'" + type_name + "' names no instance of the generic '" + generic + "': " + mistake);
		}
		throw serialization_error("'" + type_name + "' names an instance of the generic '" + generic + "' that the program never made: name the type where it is loaded, as " +
		                          (binary ? "from_binary<" + type_name + ">(bytes)" : "from_json<" + type_name + ">(text)") + " does");
	}

	// The archive form of one value back to a live value. A class instance (a {"$type": Name, "$val": {members}} map naming a
	// registered class) is constructed, then its saved members are restored in the class's declaration order (each nested value built
	// when its member is reached), without notifying, then its post_load(self, version) is queued for the end of the
	// outermost load; a type name or member name is data, never code, and only the class's own members are looked up (the
	// key-injection fix, B5)
	class archive_rebuilder {
	public:
		archive_rebuilder(engine* eng, bool json_string_keys, archive_handles& handles) : eng_(eng), json_string_keys_(json_string_keys), handles_(handles) {}

		// The walk is one load: the classes' restore orders stay cached and the post_load hooks wait until it is done
		script_value rebuild(const script_value& value, script_int handle_id) {
			detail::engine_access::load_session session(eng_);
			script_value result = rebuild_walk(value, handle_id);
			session.complete();
			return result;
		}

	private:
		script_value rebuild_walk(const script_value& value, script_int handle_id) {
			script_value result(std::monostate{}, eng_);
			if (open(value, handle_id, result)) { return result; }
			detail::work_meter work(eng_);
			for (;;) {
				if (work.spend(2)) [[unlikely]] { work.throw_spent(); }
				if (!frames_.back().finished && advance()) { continue; }
				script_value done = finish(frames_.back());
				frames_.pop_back();
				if (frames_.empty()) { return done; }
				deliver(frames_.back(), std::move(done));
			}
		}

		enum class shape : uint8_t { map, array, instance, handle, pairs, coroutine };
		struct frame {
			frame(shape a_kind, script_value a_source, script_value a_built, engine* a_eng) :
				kind(a_kind), source(std::move(a_source)), built(std::move(a_built)), key(std::monostate{}, a_eng) {}
			shape kind;
			script_value source;
			script_value built;
			script_map::const_iterator next{};
			size_t index = 0;
			size_t count = 0;
			script_value key;              // the member the child rebuilds for, a pair's rebuilt key, or a coroutine frame's handle
			script_value member{std::monostate{}, nullptr};   // a coroutine frame: the record member the child rebuilds for
			bool have_key = false;
			bool finished = false;         // an instance whose setter threw: the in-flight exception leaves the load
			script_int handle_id = 0;      // a handle frame's id, or the id an instance frame's object registered under
			script_int version = 1;        // an instance frame's saved $version, for its post_load
			bool weak = false;
			const std::vector<detail::engine_access::load_field>* order = nullptr;   // an instance frame's restore order (index walks it)
			const detail::engine_access::load_field* field = nullptr;                // the member the child rebuilds for
		};

		engine* eng_;
		bool json_string_keys_;
		archive_handles& handles_;
		std::vector<frame> frames_;
		script_value scratch_{std::monostate{}, eng_};

		// A leaf's value now (true), or a frame opened for the walk to fill (false)
		bool open(const script_value& value, script_int handle_id, script_value& out) {
			if (value.is_array()) {
				frame f(shape::array, value, script_value::make_array(nullptr, eng_), eng_);
				f.count = value.unchecked_array_node()->size();
				const_cast<std::vector<script_value>&>(f.built.as_array()).reserve(f.count);
				frames_.push_back(std::move(f));
				return false;
			}
			if (!value.is_map()) {
				out = value;
				return true;
			}
			const auto& map = value.as_map();
			archive_record_keys keys;
			if (read_archive_record_keys(map, keys)) {
				if (keys.id || keys.ref) {
					script_int id = 0;
					script_value payload(std::monostate{}, eng_);
					bool has_payload = false;
					bool weak = false;
					if (archive_handle_parts(eng_, map, id, payload, has_payload, weak)) {
						return open_handle(id, has_payload ? &payload : nullptr, weak, out);
					}
				} else if (keys.type && keys.val && keys.type->is_string() && keys.val->is_map() && (!keys.version || keys.version->is_int())) {
					if (keys.type->as_string() == "coroutine") {
						open_coroutine(*keys.val, handle_id);
						return false;
					}
					const script_int version = keys.version ? keys.version->as_int() : 1;
					if (open_instance(*keys.val, keys.type->as_string(), version, handle_id)) { return false; }
				}
			}
			if (json_string_keys_ && map.size() == 1) {
				const auto& [tag, payload] = *map.begin();
				if (tag.is_string() && tag.as_string() == "$char" && payload.is_int() && payload.as_int() >= 0 && payload.as_int() <= 255) {
					out = script_value(static_cast<script_char>(payload.as_int()), eng_);
					return true;
				}
				if (tag.is_string() && tag.as_string() == "$float" && payload.is_string()) {
					const std::string& text = payload.as_string();
					if (text == "inf" || text == "-inf" || text == "nan") {
						out = script_value(text == "nan" ? std::numeric_limits<double>::quiet_NaN() : text == "inf" ? std::numeric_limits<double>::infinity()
						                                                                                     : -std::numeric_limits<double>::infinity(), eng_);
						return true;
					}
				}
				if (tag.is_string() && tag.as_string() == "$map" && payload.is_array() && pairs_well_formed(payload)) {
					frame f(shape::pairs, payload, script_value::make_map(nullptr, nullptr, eng_), eng_);
					f.count = payload.unchecked_array_node()->size() * 2;
					frames_.push_back(std::move(f));
					return false;
				}
			}
			frame f(shape::map, value, script_value::make_map(json_string_keys_ ? detail::engine_access::get_type_info_string(*eng_) : nullptr, nullptr, eng_), eng_);
			f.next = map.begin();
			frames_.push_back(std::move(f));
			return false;
		}

		// A coroutine's record: its handle is made at once, so a handle to it inside what it holds finds it, and filled when
		// the record's values are loaded
		void open_coroutine(const script_value& record, script_int handle_id) {
			script_value handle = detail::make_unloaded_coroutine(*eng_);
			if (handle_id != 0) { handles_.loaded.try_emplace(handle_id, handle); }
			frame f(shape::coroutine, record, script_value::make_map(detail::engine_access::get_type_info_string(*eng_), nullptr, eng_), eng_);
			f.next = record.as_map().begin();
			f.key = std::move(handle);
			frames_.push_back(std::move(f));
		}

		// The receiver a loaded record names: the instance being loaded in place that many levels out, else the handle it holds
		script_value coroutine_receiver(const script_map& record) {
			const script_value owner_key(std::string("owner"), eng_);
			const script_value this_key(std::string("this"), eng_);
			if (auto owner = record.find(owner_key); owner != record.end()) {
				script_int levels = owner->second.is_int() ? owner->second.as_int() : 0;
				for (size_t i = frames_.size() - 1; levels > 0 && i-- > 0;) {
					const frame& outer = frames_[i];
					if (outer.kind == shape::instance && outer.handle_id == 0 && --levels == 0) { return outer.built; }
				}
				detail::throw_coroutine_not_loaded("?", "its save names a receiver that is not being loaded around it");
			}
			if (auto held = record.find(this_key); held != record.end() && held->second.is_object()) {
				script_value receiver = held->second;
				if (strong_ptr<class_instance> instance = receiver.get_class_instance()) {
					if (class_definition* def = instance->get_class_definition()) { return script_value::make_class_object(*def, instance, eng_); }
				}
			}
			return script_value(std::monostate{}, eng_);
		}

		bool pairs_well_formed(const script_value& payload) {
			const script_array* pairs = payload.unchecked_array_node();
			for (size_t i = 0; i < pairs->size(); ++i) {
				script_value pair = pairs->get(i, eng_);
				if (!pair.is_array() || pair.unchecked_array_node()->size() != 2) { return false; }
			}
			return true;
		}

		bool open_handle(script_int id, const script_value* data, bool weak, script_value& out) {
			script_value target(std::monostate{}, eng_);
			if (id != 0) {
				if (auto known = handles_.loaded.find(id); known != handles_.loaded.end()) {
					target = known->second;
				} else {
					if (!data) {
						if (!handles_.indexed && handles_.root) {
							handles_.indexed = true;
							index_archive_handles(eng_, *handles_.root, handles_);
						}
						if (auto found = handles_.payloads.find(id); found != handles_.payloads.end()) { data = &found->second; }
					}
					if (data) {
						if (!handles_.loading.insert(id).second) { throw_unwritten_handle(id); }
						frame f(shape::handle, *data, script_value(std::monostate{}, eng_), eng_);
						f.handle_id = id;
						f.weak = weak;
						f.count = 1;
						frames_.push_back(std::move(f));
						return false;
					}
					if (!weak) { throw_unwritten_handle(id); }
				}
			}
			out = handle_result(std::move(target), weak);
			return true;
		}

		script_value handle_result(script_value target, bool weak) {
			if (!weak) {
				if (target.is_null()) { target.set_type_info(detail::engine_access::get_type_info_shared_ptr(*eng_, nullptr)); }
				return target;
			}
			if (target.is_object()) {
				if (auto observed = script_value::make_weak_ptr(target, eng_)) { return std::move(observed.value()); }
			}
			return script_value::make_empty_weak_ptr(detail::engine_access::get_type_info_weak_ptr(*eng_, nullptr), eng_);
		}

		// A load constructs: a class with a zero-argument constructor runs it as Name() would whatever its access, its initializer
		// list included (a hybrid's builds its C++ base), and its error or throw is the load's; a class without one is created
		// without one, its C++ base through load_hybrid's ladder, then its field initializers. False leaves the map a map
		bool open_instance(script_value fields, std::string type_name, script_int version, script_int handle_id) {
			std::shared_ptr<class_definition> class_def = detail::engine_access::get_class_definition(*eng_, type_name);
			if (!class_def) {
				const std::string spelled = detail::registered_type_spelling(eng_, type_name);
				class_def = detail::engine_access::get_class_definition(*eng_, spelled);
				if (!class_def) {
					refuse_unmade_instance(*eng_, type_name, !json_string_keys_);
					return false;
				}
				type_name = spelled;
			}
			script_value ctor = eng_->get_variable(type_name);
			if (!ctor.is_function()) { return false; }
			script_value instance(std::monostate{}, eng_);
			const std::shared_ptr<class_definition> cpp_base = class_def->is_script_class() ? class_def->find_cpp_base_class() : nullptr;
			if (detail::engine_access::script_class_default_constructible(*eng_, *class_def)) {
				checked_result<script_value> constructed = [&]() {
					detail::engine_access::construction_access_waiver waiver(eng_);
					detail::engine_access::load_callout callout(eng_);
					return ctor.as_function()({});
				}();
				if (!constructed) {
					throw runtime_error(eng_->format_error(constructed));
				}
				if (detail::engine_access::backend_unwinding(*eng_)) {
					throw runtime_error(detail::engine_access::backend_exception_text(*eng_));
				}
				instance = std::move(constructed.value());
			} else if (cpp_base) {
				instance = load_hybrid(eng_, fields.as_map(), class_def, *cpp_base);
			} else {
				auto created = class_def->create_instance();
				if (!created) { return false; }
				instance = script_value::make_class_object(*class_def, created, eng_);
				detail::engine_access::get_execution_backend(*eng_)->apply_field_initializers(created, class_def);
			}
			strong_ptr<class_instance> created_instance = instance.is_object() ? instance.get_class_instance() : nullptr;
			class_definition* def = created_instance ? created_instance->get_class_definition() : nullptr;
			if (!def) { return false; }
			if (handle_id != 0) { handles_.loaded.try_emplace(handle_id, script_handle_of(eng_, instance)); }
			frame f(shape::instance, std::move(fields), std::move(instance), eng_);
			f.order = &detail::engine_access::load_field_order(*eng_, *def);
			f.version = version;
			f.handle_id = handle_id;
			frames_.push_back(std::move(f));
			return true;
		}

		// Opens the top frame's next child (a leaf goes straight in); false when the frame has none left
		bool advance() {
			frame& f = frames_.back();
			// the child stays in its source (which no frame move disturbs), or in scratch_ when it had to be made
			const script_value* child = &scratch_;
			script_int child_handle = 0;
			switch (f.kind) {
				case shape::array: {
					if (f.index >= f.count) { return false; }
					const script_array* node = f.source.unchecked_array_node();
					const size_t i = f.index++;
					if (node->kind() == script_array::kind_t::hetero) {
						child = &node->values()[i];
					} else {
						scratch_ = node->get(i, eng_);
					}
					break;
				}
				case shape::map:
					if (f.next == f.source.as_map().end()) { return false; }
					f.key = f.next->first;
					child = &f.next->second;
					++f.next;
					break;
				case shape::coroutine:
					if (f.next == f.source.as_map().end()) { return false; }
					f.member = f.next->first;
					child = &f.next->second;
					++f.next;
					break;
				case shape::instance: {
					// the class's own members in declaration order, each looked up in the saved object: a key the class does
					// not declare is never visited, and a member the save lacks keeps its constructed value
					const auto& map = f.source.as_map();
					for (;;) {
						if (f.index >= f.order->size()) { return false; }
						const detail::engine_access::load_field& field = (*f.order)[f.index++];
						auto it = map.find(field.key);
						if (it == map.end()) { continue; }
						f.field = &field;
						child = &it->second;
						break;
					}
					break;
				}
				case shape::pairs: {
					if (f.index >= f.count) { return false; }
					script_value pair = f.source.unchecked_array_node()->get(f.index / 2, eng_);
					scratch_ = pair.unchecked_array_node()->get(f.index % 2, eng_);
					++f.index;
					break;
				}
				case shape::handle:
					if (f.index >= f.count) { return false; }
					++f.index;
					scratch_ = f.source;
					child_handle = f.handle_id;
					break;
			}
			script_value leaf(std::monostate{}, eng_);
			if (open(*child, child_handle, leaf)) { deliver(frames_.back(), std::move(leaf)); }
			return true;
		}

		void deliver(frame& f, script_value&& child) {
			switch (f.kind) {
				case shape::array:
					const_cast<std::vector<script_value>&>(f.built.as_array()).push_back(std::move(child));
					break;
				case shape::map:
					const_cast<script_map&>(f.built.as_map()).emplace(f.key, std::move(child));
					break;
				case shape::coroutine:
					const_cast<script_map&>(f.built.as_map()).emplace(f.member, std::move(child));
					break;
				case shape::pairs:
					if (!f.have_key) {
						f.key = std::move(child);
						f.have_key = true;
					} else {
						const_cast<script_map&>(f.built.as_map()).emplace(std::move(f.key), std::move(child));
						f.have_key = false;
					}
					break;
				case shape::handle:
					f.built = std::move(child);
					break;
				case shape::instance:
					store_member(f, std::move(child));
					break;
			}
		}

		// A script field's storage, past any accessor the script declares; a C++ property through its binding (the load in
		// progress keeps observers quiet); a value that does not fit converts as hot reload's retype does or keeps the
		// constructed value, recorded either way
		void store_member(frame& f, script_value&& value) {
			if (f.finished) { return; }
			strong_ptr<class_instance> inst = f.built.get_class_instance();
			class_definition* def = inst->get_class_definition();
			if (!store_saved_member(eng_, f.built, *inst, *def, *f.field, std::move(value))) { f.finished = true; }
		}

		script_value finish(frame& f) {
			switch (f.kind) {
				case shape::handle: {
					handles_.loading.erase(f.handle_id);
					script_value target = handles_.loaded.try_emplace(f.handle_id, script_handle_of(eng_, std::move(f.built))).first->second;
					return handle_result(std::move(target), f.weak);
				}
				case shape::instance:
					if (!f.finished) { run_post_load(f); }
					return std::move(f.built);
				case shape::coroutine: {
					const script_map& record = f.built.as_map();
					detail::load_coroutine_record(*eng_, f.key, record, coroutine_receiver(record));
					return std::move(f.key);
				}
				default:
					return std::move(f.built);
			}
		}

		// post_load(self, version), the class's migration hook: queued as the object completes, so the hooks run in completion
		// order (children before parents) once the outermost load is done
		void run_post_load(frame& f) {
			detail::engine_access::defer_post_load(*eng_, f.built, f.version);
		}
	};

	script_value reconstruct_archive_value(engine* eng, const script_value& value, bool json_string_keys, archive_handles& handles,
	                                       script_int handle_id) {
		return archive_rebuilder(eng, json_string_keys, handles).rebuild(value, handle_id);
	}

	// One archive's script value back to live values: classes by name, handles by id
	script_value load_archive_value(engine* eng, const script_value& value, bool json_string_keys) {
		archive_handles handles;
		handles.root = &value;
		return reconstruct_archive_value(eng, value, json_string_keys, handles);
	}

	script_value parse_json_text(engine* eng, const std::string& json) {
		detail::engine_access::load_session session(eng);
		serialization::json_archive_reader reader(json, eng, true);
		script_value parsed = reader.read_value();
		// a text with no '$' key holds no archive record: the parse is the value the rebuild would copy out
		script_value result = reader.has_archive_keys() ? load_archive_value(eng, parsed, true) : std::move(parsed);
		session.complete();
		return result;
	}

	static script_value parse_binary_bytes(engine* eng, const std::string& bytes) {
		std::vector<uint8_t> data(bytes.begin(), bytes.end());
		detail::engine_access::load_session session(eng);
		serialization::binary_archive_reader reader(data, eng);
		reader.count_work();
		script_value result = load_archive_value(eng, reader.read_value(), false);
		session.complete();
		return result;
	}

    // Register JSON and binary serialization functions with an engine
    void register_json_functions(engine& eng_ref) {
        engine* eng = &eng_ref;
        // serial: an archive reaches the engine's class registry and the objects a value holds
        eng_ref.add_variadic_function("to_json", [eng](const std::vector<script_value>& args) -> script_value {
            if (args.size() == 1) {
                serialization::json_archive_writer writer(-1, eng); // No indentation, with engine ref
                writer.count_work();
                writer.write_value(args[0]);
                return script_value(writer.take(), eng);
            } else if (args.size() == 2) {
                int indent = static_cast<int>(args[1].as_int());
                serialization::json_archive_writer writer(indent, eng);
                writer.charge_indentation();
                writer.count_work();
                writer.write_value(args[0]);
                return script_value(writer.take(), eng);
            } else {
                throw runtime_error("to_json expects 1 or 2 arguments, got " + std::to_string(args.size()));
            }
        }, serial);
        
		eng_ref.add_function("from_json", [eng](const std::string& json) -> script_value {
			detail::engine_access::load_rule rule(eng, {});
			return parse_json_text(eng, json);
		}, serial);
		// from_json<T>(text): the document as a T, a class built from an object by jai::make_record's rules
		eng_ref.add_function("from_json", [eng](const script_type& type, const std::string& json) -> script_value {
			detail::engine_access::load_rule rule(eng, "from_json");
			auto built = detail::json_to_type(*eng, parse_json_text(eng, json), type.info(), "from_json", "the JSON");
			if (!built) { throw runtime_error(eng->format_error(built)); }
			return std::move(built.value());
		}, serial, returns<type_arg>);
        
        eng_ref.add_variadic_function("to_binary", [eng](const std::vector<script_value>& args) -> script_value {
            if (args.size() != 1) {
                throw runtime_error("to_binary expects exactly 1 argument, got " + std::to_string(args.size()));
            }

            serialization::binary_archive_writer writer(eng);
            writer.count_work();
            writer.write_value(args[0]);

            const auto& data = writer.data();
            script_string binary_str(data.begin(), data.end());
            return script_value(binary_str, eng);
        }, serial);

        eng_ref.add_variadic_function("from_binary", [eng](const std::vector<script_value>& args) -> script_value {
            if (args.size() != 1) {
                throw runtime_error("from_binary expects exactly 1 argument, got " + std::to_string(args.size()));
            }

            if (!args[0].is_string()) {
                throw runtime_error("from_binary expects a string argument");
            }

			detail::engine_access::load_rule rule(eng, {});
			return parse_binary_bytes(eng, args[0].as<script_string>());
        }, serial);
		// from_binary<T>(bytes): the archive's value as a T, by from_json<T>'s rules
		eng_ref.add_function("from_binary", [eng](const script_type& type, const std::string& bytes) -> script_value {
			detail::engine_access::load_rule rule(eng, "from_binary");
			auto built = detail::json_to_type(*eng, parse_binary_bytes(eng, bytes), type.info(), "from_binary", "the data");
			if (!built) { throw runtime_error(eng->format_error(built)); }
			return std::move(built.value());
		}, serial, returns<type_arg>);

        eng_ref.add_function("base64_encode", [eng](const std::string& input) -> script_value {
            return script_value(base64_encode(input), eng);
        });

        eng_ref.add_function("base64_decode", [eng](const std::string& input) -> script_value {
            return script_value(base64_decode(input), eng);
        });
    }

} // namespace stdlib
} // namespace jai
