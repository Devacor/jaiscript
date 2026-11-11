#include <jaiscript/serialization/archive_impl.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/engine_access.hpp>

#include <charconv>
#include <cmath>
#include <string>

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

std::pair<uint32_t, bool> archive_writer_core::track_shared_ptr(const void* raw_ptr) {
    if (raw_ptr == nullptr) {
        return {0, false};
    }
    if (!shared_ptr_ids_) { shared_ptr_ids_ = std::make_unique<std::unordered_map<const void*, uint32_t>>(); }
    auto [it, inserted] = shared_ptr_ids_->try_emplace(raw_ptr, next_shared_id_);
    if (inserted) {
        ++next_shared_id_;
    }
    return {it->second, inserted};
}

std::pair<uint32_t, bool> archive_writer_core::track_handle(const script_value::object_holder* holder) {
    const void* identity = holder;
    if (const class_instance* instance = holder ? holder->instance() : nullptr) {
        const class_definition* class_def = instance->get_class_definition();
        if (class_def && !class_def->is_script_class() && instance->cpp_object_pointer()) {
            identity = instance->cpp_object_pointer();
        }
    } else if (holder) {
        identity = holder->object() ? holder->object() : holder->bound_object() ? holder->bound_object() : identity;
    }
    return track_shared_ptr(identity);
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
