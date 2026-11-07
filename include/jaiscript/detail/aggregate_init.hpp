#pragma once

// C++ aggregate initialization ([dcl.init.aggr]) of a script class that declares no constructor: `P a{1, 2}`, `P{1, 2}`,
// `P a(1, 2)` and `P(1, 2)` (C++20 parenthesized aggregate init) fill the instance fields in declaration order, bases
// first, each through the field's typed-slot conversion; fields past the last initializer run their own initializers.
// One argument of the class itself (or a subclass) copies it instead. An aggregate: every class in the chain is a script
// class declaring no constructor, and no instance field is private or a reference. Script methods dispatch dynamically
// without a `virtual` keyword, so methods never disqualify a class. KEEP the one kernel for both backends.

#include <jaiscript/core/script_class.hpp>
#include <jaiscript/detail/environment.hpp>
#include <string>
#include <utility>
#include <vector>

namespace jai::detail {

struct aggregate_plan {
    enum class verdict : uint8_t { none, values, copy, not_aggregate, too_many };
    verdict kind = verdict::none;
    size_t field_count = 0;
    std::vector<std::pair<uint64_t, const script_value*>> elements;   // field id -> its initializer, chain order

    bool fills() const { return kind == verdict::values; }
    const script_value* initializer_for(uint64_t field_id) const {
        for (const auto& [id, value] : elements) {
            if (id == field_id) { return value; }
        }
        return nullptr;
    }
};

inline bool collect_aggregate_fields(const script_class_definition& cls, std::vector<uint64_t>& out, int depth = 0) {
    if (depth > 64 || !cls.get_constructor_asts().empty() || cls.find_cpp_base_class()) { return false; }
    for (const auto& parent : cls.get_parent_classes()) {
        if (!parent || !parent->is_script_class() ||
            !collect_aggregate_fields(static_cast<const script_class_definition&>(*parent), out, depth + 1)) {
            return false;
        }
    }
    for (uint64_t id : cls.field_order()) {
        if (cls.has_static_field(id)) { continue; }
        access_level level = access_level::public_access;
        if (cls.find_nonpublic_declarer(id, level)) { return false; }
        type_info_ptr declared = cls.get_field_declared_type(id);
        if (declared && declared->base_type == script_value_type::jai_reference_type) { return false; }
        out.push_back(id);
    }
    return true;
}

inline bool is_instance_of_class(const script_value& value_in, const class_definition& cls) {
    const script_value& value = value_in.deref();
    if (value.raw_storage_index() != script_value::TYPEID_OBJECT) { return false; }
    if (auto tag = value.get_type_info(); tag && tag->base_type == script_value_type::jai_shared_ptr_type) { return false; }
    auto holder = const_cast<script_value&>(value).get_object_holder();
    if (!holder || !holder->is_instance()) { return false; }
    const class_definition* def = holder->unchecked_instance()->get_class_definition();
    return def && (def == &cls || def->derives_from(&cls));
}

// A construction of a constructor-less class with arguments (args non-empty)
inline aggregate_plan plan_aggregate(const script_class_definition& cls, const std::vector<script_value>& args) {
    aggregate_plan plan;
    if (args.size() == 1 && is_instance_of_class(args[0], cls)) {
        plan.kind = aggregate_plan::verdict::copy;
        return plan;
    }
    std::vector<uint64_t> fields;
    if (!collect_aggregate_fields(cls, fields)) {
        plan.kind = aggregate_plan::verdict::not_aggregate;
        return plan;
    }
    plan.field_count = fields.size();
    if (args.size() > fields.size()) {
        plan.kind = aggregate_plan::verdict::too_many;
        return plan;
    }
    plan.kind = aggregate_plan::verdict::values;
    plan.elements.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) { plan.elements.emplace_back(fields[i], &args[i]); }
    return plan;
}

// {0} = the class, {1} = the counts; a parallel worker's frozen symbolizer keeps the count-free shape
inline error_propagator too_many_initializers(uint64_t class_id, size_t field_count, size_t argc, engine* eng) {
    const std::error_code ec = make_error_code(runtime_error_code::argument_count_mismatch);
    string_symbolizer* symbols = eng ? engine_access::get_symbolizer(*eng) : nullptr;
    if (symbols && !symbols->frozen()) {
        const std::string counts = "it has " + std::to_string(field_count) + (field_count == 1 ? " field" : " fields") +
                                   ", got " + std::to_string(argc);
        return {ec, "Too many initializers for '{0}': {1}", class_id, symbols->intern(counts)};
    }
    return {ec, "Too many initializers for '{0}'", class_id, 0};
}

// What a plan that does not fill answers: the copy, the count text, or the constructor miss text
inline checked_result<script_value> aggregate_answer(const aggregate_plan& plan, const script_class_definition& cls,
                                                     const std::vector<script_value>& args, engine* eng) {
    if (plan.kind == aggregate_plan::verdict::copy) {
        return clone_for_assignment(args[0]);
    }
    if (plan.kind == aggregate_plan::verdict::too_many) {
        return too_many_initializers(cls.get_type_id(), plan.field_count, args.size(), eng);
    }
    return overload_miss(make_error_code(runtime_error_code::no_constructor_found), cls.get_type_id(), args.data(), args.size(), eng, true);
}

// A base an empty `: super()` (written, or implied as C++'s implicit default construction) cannot default-construct
inline error_propagator no_default_constructor(uint64_t base_id) {
    return {make_error_code(runtime_error_code::no_constructor_found), "Base '{0}' has no default constructor; call super(...)", base_id, 0};
}

// A host class whose registered constructors all take arguments (the checker's host surface knows their arities)
inline bool host_lacks_default_constructor(engine* eng, const std::string& class_name) {
    std::vector<engine::host_overload_signature> signatures;
    if (!eng || !eng->host_function_signatures(class_name, signatures)) { return false; }
    for (const auto& signature : signatures) {
        if (signature.arity == 0 || signature.arity == SIZE_MAX) { return false; }
    }
    return true;
}

inline bool is_constructor_miss(std::error_code ec) {
    return ec == make_error_code(runtime_error_code::no_constructor_found) || ec == make_error_code(runtime_error_code::argument_count_mismatch);
}

// One class's own instance fields, in declaration order: an initialized field takes its argument through the field
// store's conversion, the rest run their initializers (init_field(id, ast)), so a later initializer reads the earlier
// fields as C++ does
template <class InitField>
inline checked_result<void> aggregate_fill_own_fields(class_instance& instance, const script_class_definition& cls,
                                                      const aggregate_plan& plan, engine* eng, InitField&& init_field) {
    for (uint64_t id : cls.field_order()) {
        if (cls.has_static_field(id)) { continue; }
        const script_value* given = plan.initializer_for(id);
        if (!given) {
            if (expression_ptr ast = cls.get_field_initializer_ast(id)) {
                auto initialized = init_field(id, ast);
                if (!initialized) { return initialized; }
            }
            continue;
        }
        script_value value = clone_for_assignment(*given);
        type_info_ptr declared = cls.get_field_declared_type(id);
        if (declared && declared->base_type != script_value_type::jai_any_type) {
            auto converted = field_store_convert(value, declared, eng, id, &cls, false);
            if (!converted) { return converted.error_value(); }
            value = std::move(converted.value());
        }
        instance.set_field(id, value);
    }
    return {};
}

} // namespace jai::detail
