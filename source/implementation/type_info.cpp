#include "../../include/jaiscript/core/type_info.hpp"
#include <sstream>

namespace jai {

namespace {
[[noreturn]] JAI_NOINLINE void throw_type_shape_not_prewarmed() {
    throw runtime_error("parallel_transform: type shape was not pre-warmed at the region barrier (internal) - please report this body shape");
}

// While a parallel region runs the symbolizer is frozen: a worker's type name must already be interned (an insert would move
// the tables the other workers read)
uint64_t type_name_id(string_symbolizer& symbolizer, const std::string& name) {
    if (symbolizer.frozen()) [[unlikely]] {
        if (auto id = symbolizer.find(name)) { return *id; }
        throw_type_shape_not_prewarmed();
    }
    return symbolizer.intern(name);
}

// a type without type parameters
std::string leaf_canonical_name(const type_info& t) {
    switch (t.base_type) {
        case script_value_type::jai_int_type: return t.is_byte_element() || t.is_enum ? t.type_name : "int";
        case script_value_type::jai_float_type: return "float";
        case script_value_type::jai_string_type: return "string";
        case script_value_type::jai_utf8_string_type: return "utf8_string";
        case script_value_type::jai_vec2_type: return "math::vec2";
        case script_value_type::jai_vec3_type: return "math::vec3";
        case script_value_type::jai_vec4_type: return "math::vec4";
        case script_value_type::jai_color_type: return "math::color";
        case script_value_type::jai_ivec2_type: return "math::ivec2";
        case script_value_type::jai_ivec3_type: return "math::ivec3";
        case script_value_type::jai_bool_type: return "bool";
        case script_value_type::jai_char_type: return "char";
        case script_value_type::jai_null_type: return "void";
        case script_value_type::jai_any_type: return "any";
        case script_value_type::jai_object_type:
            return t.type_name;  // Use registered class name
        default:
            return t.type_name.empty() ? "unknown" : t.type_name;
    }
}
}

// A composite type names its parameters on an explicit work list: the parser names each level of a nested type while the
// levels around it are still on the stack, so the name never recurses
std::string type_info::canonical_name() const {
    switch (base_type) {
        case script_value_type::jai_array_type: case script_value_type::jai_map_type: case script_value_type::jai_shared_ptr_type:
        case script_value_type::jai_weak_ptr_type: case script_value_type::jai_function_type:
            break;
        default:
            return leaf_canonical_name(*this);
    }
    struct piece { const type_info* type; const char* text; };   // a type still to name, else text to append
    std::vector<piece> pending;
    pending.reserve(8);
    pending.push_back({this, nullptr});
    auto text = [&](const char* t) { pending.push_back({nullptr, t}); };
    auto part = [&](const type_info_ptr& t, const char* absent) {
        if (t) { pending.push_back({t.ptr, nullptr}); } else { text(absent); }
    };
    std::string result;
    while (!pending.empty()) {
        const piece next = pending.back();
        pending.pop_back();
        if (!next.type) {
            result += next.text;
            continue;
        }
        const type_info& t = *next.type;
        switch (t.base_type) {
            case script_value_type::jai_array_type:
                text(">");
                part(t.element_type(), "");
                text("array<");
                break;
            case script_value_type::jai_map_type:
                text(">");
                part(t.value_type(), "");
                text(",");
                part(t.key_type(), "");
                text("map<");
                break;
            case script_value_type::jai_shared_ptr_type:
                // spelled in full: the id is this name interned, and a handle type must not
                // share the pointee class's id (array<shared_ptr<Foo>> is not array<Foo>)
                if (t.element_type()) {
                    text(">");
                    part(t.element_type(), "");
                    text("shared_ptr<");
                } else if (t.type_name == "shared_ptr") {
                    result += t.type_name;
                } else {
                    result += "shared_ptr<";
                    result += t.type_name;
                    result += ">";
                }
                break;
            case script_value_type::jai_weak_ptr_type:
                text(">");
                part(t.element_type(), "");
                text("weak_ptr<");
                break;
            case script_value_type::jai_function_type:
                if (t.type_params.empty()) {   // the `function` spelling: any callable or null
                    result += "function";
                    break;
                }
                text(">");
                text(")");
                for (size_t i = t.type_params.size(); i-- > 1;) {
                    part(t.type_params[i], "");
                    if (i > 1) { text(","); }
                }
                text("(");
                part(t.return_type(), "void");
                text("function<");
                break;
            default:
                result += leaf_canonical_name(t);
                break;
        }
    }
    return result;
}


std::string type_info::spelling() const {
    auto element = [](const type_info_ptr& t) { return t ? t->spelling() : std::string("var"); };
    switch (base_type) {
        case script_value_type::jai_any_type: return "var";
        case script_value_type::jai_array_type:
            return element_type() ? "array<" + element_type()->spelling() + ">" : std::string("array");
        case script_value_type::jai_map_type:
            if (!key_type() && !value_type()) { return "map"; }
            return "map<" + element(key_type()) + "," + element(value_type()) + ">";
        case script_value_type::jai_weak_ptr_type:
            return element_type() ? "weak_ptr<" + element_type()->spelling() + ">" : std::string("weak_ptr");
        case script_value_type::jai_shared_ptr_type:
            return element_type() ? "shared_ptr<" + element_type()->spelling() + ">" : canonical_name();
        default:
            return canonical_name();
    }
}

type_info type_info::make_int(string_symbolizer& symbolizer) {
    type_info info(script_value_type::jai_int_type);
    info.type_name = "int";
    info.native_size = sizeof(script_int);
    info.is_signed = true;
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_float(string_symbolizer& symbolizer) {
    type_info info(script_value_type::jai_float_type);
    info.type_name = "float";
    info.native_size = sizeof(script_float);
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_string(string_symbolizer& symbolizer) {
    type_info info(script_value_type::jai_string_type);
    info.type_name = "string";
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_utf8_string(string_symbolizer& symbolizer) {
    type_info info(script_value_type::jai_utf8_string_type);
    info.type_name = "utf8_string";
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_math(string_symbolizer& symbolizer, script_value_type kind) {
    type_info info(kind);
    info.type_name = math_type_name(kind);
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_bool(string_symbolizer& symbolizer) {
    type_info info(script_value_type::jai_bool_type);
    info.type_name = "bool";
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_char(string_symbolizer& symbolizer) {
    type_info info(script_value_type::jai_char_type);
    info.type_name = "char";
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_void(string_symbolizer& symbolizer) {
    type_info info(script_value_type::jai_null_type);
    info.type_name = "void";
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_any(string_symbolizer& symbolizer) {
    type_info info(script_value_type::jai_any_type);
    info.type_name = "any";
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_invalid(string_symbolizer& symbolizer) {
    type_info info(script_value_type::jai_invalid_type);
    info.type_name = "invalid";
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_int8(string_symbolizer& symbolizer) {
    auto info = make_int(symbolizer);
    info.type_name = "int8";
    info.native_size = 1;
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_uint8(string_symbolizer& symbolizer) {
    auto info = make_int(symbolizer);
    info.type_name = "uint8";
    info.native_size = 1;
    info.is_signed = false;
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_array(string_symbolizer& symbolizer, type_info_ptr element_type) {
    type_info info(script_value_type::jai_array_type);
    info.type_name = "array";
    info.type_params.push_back(element_type);
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_map(string_symbolizer& symbolizer, type_info_ptr keyType, type_info_ptr valueType) {
    type_info info(script_value_type::jai_map_type);
    info.type_name = "map";
    info.type_params.push_back(keyType);
    info.type_params.push_back(valueType);
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_object(string_symbolizer& symbolizer, std::string_view className) {
    type_info info(script_value_type::jai_object_type, className);
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_weak_ptr(string_symbolizer& symbolizer, type_info_ptr pointee_type) {
    type_info info(script_value_type::jai_weak_ptr_type);
    info.type_params.push_back(pointee_type);
    info.type_name = "weak_ptr";  // Keep generic name for lookups
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_shared_ptr(string_symbolizer& symbolizer, type_info_ptr pointee_type) {
    type_info info(script_value_type::jai_shared_ptr_type);
    info.type_params.push_back(pointee_type);
    // Use inner type's name for canonical name (matches parser behavior)
    info.type_name = pointee_type ? pointee_type->type_name : "shared_ptr";
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_reference(string_symbolizer& symbolizer, type_info_ptr referenced_type) {
    type_info info(script_value_type::jai_reference_type);
    info.type_params.push_back(referenced_type);
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_cpp_bound(string_symbolizer& symbolizer, type_info_ptr bound_type) {
    type_info info(script_value_type::jai_cpp_bound_type);
    info.type_params.push_back(bound_type);
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_callable(string_symbolizer& symbolizer) {
    type_info info(script_value_type::jai_function_type);
    info.type_name = "function";
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_function(string_symbolizer& symbolizer, type_info_ptr return_type, std::vector<type_info_ptr> arg_types) {
    type_info info(script_value_type::jai_function_type);
    info.type_params.push_back(return_type);
    info.type_params.insert(info.type_params.end(), arg_types.begin(), arg_types.end());
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_native_int(string_symbolizer& symbolizer, size_t native_size, bool is_signed) {
    auto info = make_int(symbolizer);
    info.native_size = native_size;
    info.is_signed = is_signed;
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

type_info type_info::make_native_float(string_symbolizer& symbolizer, size_t native_size) {
    auto info = make_float(symbolizer);
    info.native_size = native_size;
    info.id = type_name_id(symbolizer, info.canonical_name());
    return info;
}

std::string type_info::to_string() const {
    std::stringstream ss;
    
    switch (base_type) {
        case script_value_type::jai_null_type:
            return "null";
        case script_value_type::jai_int_type:
            return is_byte_element() ? type_name : "int";
        case script_value_type::jai_float_type:
            return "float";
        case script_value_type::jai_string_type:
            return "string";
        case script_value_type::jai_utf8_string_type:
            return "utf8_string";
        case script_value_type::jai_vec2_type:
        case script_value_type::jai_vec3_type:
        case script_value_type::jai_vec4_type:
        case script_value_type::jai_color_type:
        case script_value_type::jai_ivec2_type:
        case script_value_type::jai_ivec3_type:
            return math_type_name(base_type);
        case script_value_type::jai_char_type:
            return "char";
        case script_value_type::jai_bool_type:
            return "bool";
            
        case script_value_type::jai_array_type:
            if (!type_params.empty()) {
                ss << "array<" << type_params[0]->to_string() << ">";
            } else {
                ss << "array<unknown>";
            }
            return ss.str();
            
        case script_value_type::jai_map_type:
            if (type_params.size() >= 2) {
                ss << "map<" << type_params[0]->to_string() 
                   << ", " << type_params[1]->to_string() << ">";
            } else {
                ss << "map<unknown, unknown>";
            }
            return ss.str();
            
        case script_value_type::jai_object_type:
            return type_name.empty() ? "object" : type_name;
            
        case script_value_type::jai_function_type:
            ss << "function<";
            if (!type_params.empty()) {
                ss << type_params[0]->to_string() << "(";
                for (size_t i = 1; i < type_params.size(); ++i) {
                    if (i > 1) ss << ", ";
                    ss << type_params[i]->to_string();
                }
                ss << ")";
            } else {
                ss << "unknown()";
            }
            ss << ">";
            return ss.str();
            
        case script_value_type::jai_reference_type:
            if (!type_params.empty()) {
                ss << type_params[0]->to_string() << "&";
            } else {
                ss << "unknown&";
            }
            return ss.str();
            
        case script_value_type::jai_weak_ptr_type:
            if (!type_params.empty()) {
                ss << "weak_ptr<" << type_params[0]->to_string() << ">";
            } else {
                ss << "weak_ptr<unknown>";
            }
            return ss.str();
            
        default:
            return "unknown";
    }
}

bool type_info::equals(const type_info& other) const {
    if (this == &other) return true;
    if (base_type != other.base_type) {
        return false;
    }
    
    if (type_name != other.type_name) {
        return false;
    }
    
    if (type_params.size() != other.type_params.size()) {
        return false;
    }
    
    for (size_t i = 0; i < type_params.size(); ++i) {
        if (!type_params[i] || !other.type_params[i]) {
            if (type_params[i] != other.type_params[i]) {
                return false;
            }
        } else if (!type_params[i]->equals(*other.type_params[i])) {
            return false;
        }
    }
    
    return true;
}

} // namespace jai