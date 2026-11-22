#include <jaiscript/serialization/archive_impl.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/detail/coroutine_snapshot.hpp>
#include <jaiscript/detail/type_name_key.hpp>
#include <jaiscript/core/coroutine.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/signals/receiver_save.hpp>

#include <charconv>
#include <cmath>
#include <string>

namespace jai::detail {

std::string registered_type_spelling(const engine* a_engine, std::string_view a_text) {
    if (a_engine && a_engine->is_type_registered(std::string(a_text))) { return std::string(a_text); }
    const std::string key = type_name_key(a_text);
    if (a_engine) {
        for (std::string& name : a_engine->registered_class_names()) {
            if (type_name_key(name) == key) { return std::move(name); }
        }
    }
    return serialization::polymorphic_registry::instance().name_of_key(key);
}

}

namespace jai::serialization {

void throw_archive_expected(const char* expected, std::string_view found) {
    throw serialization_error(std::string("Expected ") + expected + " in the archive, found " + std::string(found));
}

void throw_not_serializable(std::type_index type) {
    throw serialization_error("Type '" + detail::readable_cpp_type_name(type) + "' is not serializable. "
        "Add save/load, serialize, property_owner, or ADL functions, bind it in the archive's engine — or mark the property transient.");
}

void throw_not_deserializable(std::type_index type) {
    throw serialization_error("Type '" + detail::readable_cpp_type_name(type) + "' is not deserializable. "
        "Add save/load, serialize, property_owner, or ADL functions, bind it in the archive's engine — or mark the property transient.");
}

void throw_unregistered_hybrid_base(const std::string& hybrid_type, std::type_index base) {
    throw serialization_error("Cannot serialize hybrid script class '" + hybrid_type + "': its C++ base '" + detail::readable_cpp_type_name(base) +
        "' is not registered. Register it with jai::registrar<T, Context>(\"Name\") or derive from jai::property_owner<T, ...>.");
}

void throw_unregistered_polymorphic(std::type_index actual, std::type_index base) {
    throw serialization_error("Cannot serialize unregistered polymorphic type '" + detail::readable_cpp_type_name(actual) + "' through pointer-to-base '" +
        detail::readable_cpp_type_name(base) + "'. Register it with jai::registrar<T, Context, jai::bases<Base>>(\"Name\") or derive from jai::property_owner<T, Base>.");
}

void throw_unrelated_polymorphic(const std::string& poly_type, std::type_index base) {
    throw serialization_error("Polymorphic type '" + poly_type + "' is not a registered subtype of requested base '" + detail::readable_cpp_type_name(base) +
        "' (declare it: static jai::serialization::register_relation<" + poly_type + ", Base>, or .base<Base>() on its registrar)");
}

void throw_no_construct_route(std::type_index type) {
    throw serialization_error("Cannot deserialize shared_ptr<" + detail::readable_cpp_type_name(type) + ">: type is not default constructible and has no load_and_construct");
}

namespace {

    std::shared_ptr<class_definition> binding_of(engine* eng, std::type_index type, const class_metadata*& record) {
        record = nullptr;
        std::shared_ptr<class_definition> result = eng ? detail::engine_access::get_class_definition_by_type(*eng, type) : nullptr;
        if (result) {
            record = detail::engine_access::get_serialization_registry(*eng).get_class_metadata(result->get_name());
        }
        return record ? result : nullptr;
    }

    // The binding's post_load hook, as the script archives run it on an object they load
    void run_binding_post_load(engine* eng, const class_definition& binding, void* object) {
        script_value hook = binding.get_method(detail::engine_access::symbolize(*eng, "post_load"), false);
        if (hook.type() != script_value_type::jai_function_type) {
            return;
        }
        script_value self = script_value::make_cpp_reference(binding.get_name(), binding.get_type_id(), object, std::weak_ptr<void>(), eng);
        auto ran = hook.as_function()({ self, script_value(static_cast<script_int>(1), eng) });
        if (!ran) {
            throw serialization_error(eng->format_error(ran));
        }
    }

}

void write_binding_record(any_archive_writer& ar, engine* eng, std::type_index type, const void* object, bool base_level) {
    const class_metadata* record = nullptr;
    if (!binding_of(eng, type, record)) {
        if (base_level) {
            return;
        }
        throw_not_serializable(type);
    }
    for (const base_archive_route& base : record->bases) {
        write_binding_record(ar, eng, base.type, base.upcast(const_cast<void*>(object)), true);
    }
    for (const property_metadata& property : record->properties) {
        if (property.archive.save) {
            property.archive.save(property.archive.state.get(), property.name.c_str(), object, ar);
        }
    }
}

void read_binding_record(any_archive_reader& ar, engine* eng, std::type_index type, void* object, bool base_level) {
    const class_metadata* record = nullptr;
    const std::shared_ptr<class_definition> binding = binding_of(eng, type, record);
    if (!binding) {
        if (base_level) {
            return;
        }
        throw_not_deserializable(type);
    }
    for (const base_archive_route& base : record->bases) {
        read_binding_record(ar, eng, base.type, base.upcast(object), true);
    }
    for (const property_metadata& property : record->properties) {
        if (property.archive.load && ar.seek_property(property.name)) {
            property.archive.load(property.archive.state.get(), object, ar);
        }
    }
    ar.clear_property_value();
    if (!base_level) {
        run_binding_post_load(eng, *binding, object);
    }
}

namespace {

    // Each level of type's binding in eng, bases first, with that level's object
    template<typename Fn>
    void visit_bound_levels(engine* eng, std::type_index type, const void* object, Fn& fn) {
        const class_metadata* record = nullptr;
        if (!binding_of(eng, type, record)) {
            return;
        }
        for (const base_archive_route& base : record->bases) {
            visit_bound_levels(eng, base.type, base.upcast(const_cast<void*>(object)), fn);
        }
        fn(*record, object);
    }

    // A bound name a property_mgr of the object holds is that property's key, so the binding's stays out
    bool held_by_property_mgr(engine* eng, std::type_index type, const void* object, const std::string& name) {
        bool result = false;
        auto find = [&result, &name](const class_metadata& record, const void* level) {
            result = result || (record.level_manager && record.level_manager(level)->get(name));
        };
        visit_bound_levels(eng, type, object, find);
        return result;
    }

    bool owns_level(const class_metadata& record, const void* object, const property_manager* level) {
        return !level || (record.level_manager && record.level_manager(object) == level);
    }

}

std::string bound_type_name(engine* eng, std::type_index type) {
    const class_metadata* record = nullptr;
    const std::shared_ptr<class_definition> binding = binding_of(eng, type, record);
    return binding ? binding->get_name() : std::string();
}

const polymorphic_registry::type_entry* binding_entry_named(engine* eng, const std::string& name) {
    const std::shared_ptr<class_definition> binding = eng ? detail::engine_access::get_class_definition(*eng, name) : nullptr;
    if (!binding || !binding->cpp_type()) {
        return nullptr;
    }
    const polymorphic_registry::type_entry* entry = polymorphic_registry::instance().find(std::type_index(*binding->cpp_type()));
    return entry && entry->named_by_binding ? entry : nullptr;
}

namespace {

    const polymorphic_registry::type_entry* load_entry_spelled(engine* eng, const std::string& name, std::string& hybrid_class, std::string& hybrid_base) {
        const polymorphic_registry::type_entry* entry = polymorphic_registry::instance().find(name);
        if (!entry) {
            entry = binding_entry_named(eng, name);
        }
        if (!entry && eng) {
            // a script class deriving a bound C++ base loads through the base's entry; the caller re-attaches the script layer
            std::string base_name = detail::engine_access::script_layer_base_name(*eng, name);
            if (!base_name.empty()) {
                entry = polymorphic_registry::instance().find(base_name);
                if (!entry) {
                    entry = binding_entry_named(eng, base_name);
                }
                if (entry) {
                    hybrid_class = name;
                    hybrid_base = std::move(base_name);
                }
            }
        }
        return entry;
    }

}

const polymorphic_registry::type_entry* polymorphic_load_entry(engine* eng, const std::string& name, std::string& hybrid_class, std::string& hybrid_base) {
    if (const auto* entry = load_entry_spelled(eng, name, hybrid_class, hybrid_base)) { return entry; }
    const std::string spelled = detail::registered_type_spelling(eng, name);
    return spelled.empty() || spelled == name ? nullptr : load_entry_spelled(eng, spelled, hybrid_class, hybrid_base);
}

void throw_archive_value_range(std::string_view value_text, const char* field) {
    throw serialization_error("Archive value " + std::string(value_text) + " does not fit " + field);
}

void throw_archive_value_range(int64_t value, const char* field) { throw_archive_value_range(std::to_string(value), field); }

void throw_archive_value_range(uint64_t value, const char* field) { throw_archive_value_range(std::to_string(value), field); }

void throw_archive_value_range(double value, const char* field) {
    if (std::isnan(value)) { throw_archive_value_range("nan", field); }
    if (std::isinf(value)) { throw_archive_value_range(value > 0 ? "inf" : "-inf", field); }
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    throw_archive_value_range(std::string_view(buf, static_cast<size_t>(res.ptr - buf)), field);
}


archive_writer_core::archive_writer_core(engine* eng) : engine_ref_(eng) {
    context_stack_.push(SerializationContext::Root);
}

archive_writer_core::~archive_writer_core() = default;

void archive_writer_core::push_record(const void* object, std::type_index type) {
    records_.push_back({ object, type });
}

void archive_writer_core::pop_record() {
    records_.pop_back();
}

void archive_writer_core::write_bound_properties(any_archive_writer& ar, const property_manager* level) {
    if (records_.empty() || !records_.back().object || !engine_ref_) {
        return;
    }
    const record_frame frame = records_.back();
    auto write = [this, &ar, &frame, level](const class_metadata& record, const void* object) {
        if (!owns_level(record, object, level)) {
            return;
        }
        for (const property_metadata& property : record.properties) {
            if (property.archive.save && !held_by_property_mgr(engine_ref_, frame.type, frame.object, property.name)) {
                property.archive.save(property.archive.state.get(), property.name.c_str(), object, ar);
            }
        }
    };
    visit_bound_levels(engine_ref_, frame.type, frame.object, write);
}

std::pair<uint32_t, bool> archive_writer_core::track_shared_ptr(const void* raw_ptr) {
    if (raw_ptr == nullptr) {
        return {0, false};
    }
    if (!shared_ptr_ids_) { shared_ptr_ids_ = std::make_unique<std::unordered_map<const void*, uint32_t>>(); }
    auto [it, inserted] = shared_ptr_ids_->try_emplace(raw_ptr, next_shared_id_);
    if (inserted) {
        ++next_shared_id_;
    } else if (referenced_ahead_ && referenced_ahead_->erase(raw_ptr) != 0) {
        return {it->second, true};
    }
    return {it->second, inserted};
}

namespace {
    // The object a script handle names, as the archive keys it: a C++ object's every handle shares its object's id
    const void* handle_identity(const script_value::object_holder* holder, bool a_native_layer) {
        const void* identity = holder;
        if (const class_instance* instance = holder ? holder->instance() : nullptr) {
            const class_definition* class_def = instance->get_class_definition();
            if (class_def && (a_native_layer || !class_def->is_script_class()) && instance->cpp_object_pointer()) {
                identity = instance->cpp_object_pointer();
            }
        } else if (holder) {
            identity = holder->object() ? holder->object() : holder->bound_object() ? holder->bound_object() : identity;
        }
        return identity;
    }
}

std::pair<uint32_t, bool> archive_writer_core::track_handle(const script_value::object_holder* holder) {
    return track_shared_ptr(handle_identity(holder, false));
}

// A script class over a C++ object is that object in the archive: its C++ shared_ptr carries the payload
uint32_t archive_writer_core::reference_object(const script_value& a_object) {
    const script_value& value = a_object.deref();
    const auto holder = value.is_object() || value.type() == script_value_type::jai_shared_ptr_type ? value.get_object_holder() : nullptr;
    if (!holder) {
        return 0;
    }
    const void* identity = handle_identity(holder.get(), true);
    auto [id, is_new] = track_shared_ptr(identity);
    if (is_new) {
        if (!referenced_ahead_) { referenced_ahead_ = std::make_unique<std::unordered_set<const void*>>(); }
        referenced_ahead_->insert(identity);
    }
    return id;
}

std::vector<std::pair<std::string, script_value>> archive_writer_core::coroutine_fields(coroutine_handle& a_coroutine) {
    if (!engine_ref_) { detail::throw_coroutine_not_saved(a_coroutine, "the archive has no engine"); }
    std::vector<std::pair<std::string, script_value>> record = detail::coroutine_record_of(*engine_ref_, a_coroutine);
    const script_value& receiver = a_coroutine.receiver();
    const class_instance* instance = receiver.peek_class_instance();
    if (!instance) { return record; }
    const auto holder = receiver.get_object_holder();
    const bool shared = shared_ptr_ids_ && shared_ptr_ids_->find(handle_identity(holder.get(), false)) != shared_ptr_ids_->end();
    for (size_t i = open_instances_.size(); !shared && i-- > 0;) {
        if (open_instances_[i] == instance) {
            record.emplace_back("owner", script_value(static_cast<script_int>(open_instances_.size() - i), engine_ref_));
            return record;
        }
    }
    if (!shared) {
        if (written_instances_ && written_instances_->find(instance) != written_instances_->end()) {
            detail::throw_coroutine_not_saved(a_coroutine, "its receiver is saved by value elsewhere in the same save; hold the receiver in a shared_ptr, or keep the coroutine inside the receiver");
        }
        if (!lone_receivers_) { lone_receivers_ = std::make_unique<std::unordered_map<const void*, const coroutine_handle*>>(); }
        lone_receivers_->try_emplace(instance, &a_coroutine);
    }
    script_value handle = receiver.is_reference() ? receiver.deref() : receiver;
    if (const auto type = handle.get_type_info(); handle.is_object() && type && type->base_type == script_value_type::jai_object_type) {
        handle.set_type_info(detail::engine_access::get_type_info_shared_ptr(*engine_ref_, type.get()));
    }
    record.emplace_back("this", std::move(handle));
    return record;
}

void archive_writer_core::open_written_instance(const class_instance* a_instance) {
    if (lone_receivers_) {
        if (auto taken = lone_receivers_->find(a_instance); taken != lone_receivers_->end()) {
            detail::throw_coroutine_not_saved(*taken->second, "its receiver is saved by value elsewhere in the same save; hold the receiver in a shared_ptr, or keep the coroutine inside the receiver");
        }
    }
    const class_definition* held = a_instance ? a_instance->get_class_definition() : nullptr;
    if (held && held->declares_coroutine_method()) {
        if (!written_instances_) { written_instances_ = std::make_unique<std::unordered_set<const void*>>(); }
        written_instances_->insert(a_instance);
    }
    open_instances_.push_back(a_instance);
}

void archive_writer_core::open_coroutine_record(const coroutine_handle& a_coroutine) {
    open_coroutines_.push_back(&a_coroutine);
}

void archive_writer_core::refuse_function_in_coroutine(const script_value& a_value) const {
    if (!open_coroutines_.empty() && a_value.is_function()) {
        detail::throw_coroutine_not_saved(*open_coroutines_.back(), "it holds a function value");
    }
}

std::shared_ptr<class_definition> archive_writer_core::bound_class_of(const script_value& value) const {
    const auto holder = engine_ref_ && value.is_object() ? value.get_object_holder() : nullptr;
    if (!holder || holder->is_instance() || holder->type_id() == UINT64_MAX) {
        return nullptr;
    }
    return detail::engine_access::get_class_definition(*engine_ref_, holder->type_id());
}

SerializationContext archive_writer_core::current_context() const {
    return context_stack_.empty() ? SerializationContext::Root : context_stack_.top();
}

void archive_writer_core::push_context(SerializationContext ctx) {
    context_stack_.push(ctx);
}

void archive_writer_core::pop_context() {
    if (!context_stack_.empty() && context_stack_.top() != SerializationContext::Root) {
        context_stack_.pop();
    }
}

bool archive_writer_core::needs_object_wrapper() const {
    return current_context() != SerializationContext::ObjectBody;
}

void archive_writer_core::consume_value_context() {
    auto ctx = current_context();
    if (ctx == SerializationContext::PropertyValue || ctx == SerializationContext::MapValue) {
        pop_context();
    }
}

void* archive_writer_core::get_user_context_raw(std::type_index ti) const {
    if (user_contexts_) {
        if (auto it = user_contexts_->find(ti); it != user_contexts_->end()) {
            return it->second;
        }
    }
    return ti == engine_context_type_index() ? static_cast<void*>(engine_ref_) : nullptr;
}

void archive_writer_core::set_user_context_raw(std::type_index ti, void* context) {
    if (!user_contexts_) { user_contexts_ = std::make_unique<std::unordered_map<std::type_index, void*>>(); }
    (*user_contexts_)[ti] = context;
}

uint32_t archive_writer_core::lookup_shared_id(const void* raw_ptr) const {
    if (raw_ptr == nullptr) {
        return 0;
    }
    if (!shared_ptr_ids_) {
        return 0;
    }
    auto it = shared_ptr_ids_->find(raw_ptr);
    return it != shared_ptr_ids_->end() ? it->second : 0;
}

archive_reader_core::archive_reader_core(engine* eng) : engine_ref_(eng) {}

archive_reader_core::~archive_reader_core() = default;

void archive_reader_core::push_record(void* object, std::type_index type) {
    records_.push_back({ object, type });
}

void archive_reader_core::pop_record() {
    records_.pop_back();
}

void archive_reader_core::set_record_object(void* object) {
    records_.back().object = object;
}

void archive_reader_core::read_bound_properties(any_archive_reader& ar, const property_manager* level) {
    if (records_.empty() || !records_.back().object || !engine_ref_) {
        return;
    }
    const record_frame frame = records_.back();
    bool sought = false;
    auto read = [this, &ar, &frame, &sought, level](const class_metadata& record, const void* object) {
        if (!owns_level(record, object, level)) {
            return;
        }
        for (const property_metadata& property : record.properties) {
            if (property.archive.load && !held_by_property_mgr(engine_ref_, frame.type, frame.object, property.name) && ar.seek_property(property.name)) {
                sought = true;
                property.archive.load(property.archive.state.get(), const_cast<void*>(object), ar);
            }
        }
    };
    visit_bound_levels(engine_ref_, frame.type, frame.object, read);
    if (sought) {
        ar.clear_property_value();
    }
}

void archive_reader_core::store_deserialized(uint32_t id, std::shared_ptr<void> complete, std::type_index type) {
    if (!cpp_shared_ptrs_) { cpp_shared_ptrs_ = std::make_unique<std::unordered_map<uint32_t, deserialized_entry>>(); }
    (*cpp_shared_ptrs_)[id] = deserialized_entry{ std::move(complete), type };
}

bool archive_reader_core::has_deserialized_shared(uint32_t id) const {
    return id != 0 && cpp_shared_ptrs_ && cpp_shared_ptrs_->find(id) != cpp_shared_ptrs_->end();
}

std::shared_ptr<void> archive_reader_core::get_deserialized_shared_adjusted(uint32_t id, std::type_index target) const {
    if (id == 0) {
        return nullptr;
    }
    if (!cpp_shared_ptrs_) {
        return nullptr;
    }
    auto it = cpp_shared_ptrs_->find(id);
    if (it == cpp_shared_ptrs_->end()) {
        return nullptr;
    }
    // A back-reference may request a different base view than the original load; reject it
    // unless the stored dynamic type is actually assignable to that base, so a tampered id
    // can't reinterpret an unrelated object through `target` (type confusion). B5.
    if (!polymorphic_registry::instance().is_assignable(it->second.type, target)) {
        throw serialization_error("Back-referenced object is not a subtype of the requested type");
    }
    void* adjusted = polymorphic_registry::instance().adjust_to_base(it->second.type, target, it->second.object.get());
    return std::shared_ptr<void>(it->second.object, adjusted);
}

void* archive_reader_core::get_user_context_raw(std::type_index ti) const {
    if (user_contexts_) {
        if (auto it = user_contexts_->find(ti); it != user_contexts_->end()) {
            return it->second;
        }
    }
    return ti == engine_context_type_index() ? static_cast<void*>(engine_ref_) : nullptr;
}

void archive_reader_core::set_user_context_raw(std::type_index ti, void* context) {
    if (!user_contexts_) { user_contexts_ = std::make_unique<std::unordered_map<std::type_index, void*>>(); }
    (*user_contexts_)[ti] = context;
}

} // namespace jai::serialization
