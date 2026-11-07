#include <jaiscript/stdlib/io.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/value.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/format_spec.hpp>
#include <jaiscript/detail/value_text.hpp>
#include <optional>
#include <iostream>
#include <sstream>
#include <format>
#include <memory>
#include <cctype>
#include <charconv>

namespace jai {
namespace stdlib {

    inline bool is_all_digits(std::string_view s) {
        for (char c : s) {
            if (c < '0' || c > '9') return false;
        }
        return !s.empty();
    }

    // A placeholder is {}, {n}, {:spec}, or {n:spec} — spec is the shared subset
    // in detail/format_spec.hpp (same mini-language as template-string `${x:spec}`).
    // A {n:...} whose spec the subset does not have is a placeholder too: its
    // substitution raises, naming the spec, instead of printing the braces.
    inline bool has_format_placeholders(std::string_view str) {
        size_t pos = 0;
        while (pos < str.length()) {
            size_t brace = str.find('{', pos);
            if (brace == std::string_view::npos) break;

            if (brace + 1 < str.length() && str[brace + 1] == '{') {
                pos = brace + 2;
                continue;
            }

            size_t close = str.find('}', brace);
            if (close != std::string_view::npos) {
                std::string_view content = str.substr(brace + 1, close - brace - 1);
                if (content.empty()) {
                    return true;
                }
                size_t colon = content.find(':');
                std::string_view index_part = content.substr(0, colon == std::string_view::npos ? content.size() : colon);
                if ((index_part.empty() || is_all_digits(index_part)) && (colon != std::string_view::npos || !index_part.empty())) {
                    return true;
                }
            }

            pos = brace + 1;
        }
        return false;
    }

    // An argument's text as {} writes it: a string's bytes and an int's digits straight into the
    // result, every other value through to_string
    inline void append_format_argument(std::string& out, const script_value& arg) {
        const script_value& v = arg.is_reference() ? arg.deref() : arg;
        switch (v.raw_storage_index()) {
            case script_value::TYPEID_STRING: out += v.unchecked_as_string(); return;
            case script_value::TYPEID_INT: {
                char digits[24];
                auto res = std::to_chars(digits, digits + sizeof(digits), v.unchecked_as_int());
                out.append(digits, res.ptr);
                return;
            }
            default: out += v.to_string(); return;
        }
    }

    // {n}'s index as std::stoull reads it (leading space, a sign, a digit prefix); false where
    // stoull throws, which leaves the placeholder literal
    inline bool positional_index(std::string_view spec, size_t& out) {
        if (!spec.empty() && spec.size() <= 18 && is_all_digits(spec)) {
            size_t v = 0;
            for (char c : spec) { v = v * 10 + static_cast<size_t>(c - '0'); }
            out = v;
            return true;
        }
        try {
            out = static_cast<size_t>(std::stoull(std::string(spec)));
            return true;
        } catch (...) {
            return false;
        }
    }

    [[noreturn]] JAI_NOINLINE inline void unsupported_format_spec(std::string_view spec) {
        throw runtime_error("Unsupported format spec ':" + std::string(spec) + "' in format string");
    }

    // The format string's substitution into `result`: args[0] is the format string itself, the
    // values start at args[1] (so {0} names the first value)
    // shown: each value's display text (the value itself where it is a scalar); a spec that asks for a number reads the
    // argument itself, so its mismatch names the argument's own type
    inline void process_format_string(std::string& result, std::string_view format_str, const script_value* const* shown,
                                      const script_value* const* args, size_t arg_count) {
        constexpr size_t arg_offset = 1;
        size_t pos = 0;
        size_t next_arg = arg_offset;
        result.reserve(result.size() + format_str.size() + 8 * arg_count);

        while (pos < format_str.length()) {
            // Look for both { and } to handle escaping
            size_t next_open = format_str.find('{', pos);
            size_t next_close = format_str.find('}', pos);

            // }} -> }, a lone } stays
            if (next_close != std::string_view::npos &&
                (next_open == std::string_view::npos || next_close < next_open)) {
                result.append(format_str.substr(pos, next_close - pos));
                result += '}';
                pos = next_close + 1 < format_str.length() && format_str[next_close + 1] == '}' ? next_close + 2 : next_close + 1;
                continue;
            }

            if (next_open == std::string_view::npos) {
                result.append(format_str.substr(pos));
                break;
            }

            result.append(format_str.substr(pos, next_open - pos));

            if (next_open + 1 < format_str.length() && format_str[next_open + 1] == '{') {
                result += '{';
                pos = next_open + 2;
                continue;
            }

            size_t close_brace = format_str.find('}', next_open);
            if (close_brace == std::string_view::npos) {
                // Invalid format string, append rest as-is
                result.append(format_str.substr(next_open));
                break;
            }

            std::string_view spec = format_str.substr(next_open + 1, close_brace - next_open - 1);

            if (spec.empty()) {
                // {} - use next argument
                if (next_arg < arg_count) {
                    append_format_argument(result, *shown[next_arg]);
                    next_arg++;
                }
            } else if (size_t colon = spec.find(':'); colon != std::string_view::npos) {
                // {:spec} / {n:spec} - format through the shared spec formatter
                // (identical to template-string `${x:spec}`); a spec outside the subset
                // raises, naming it; a type-vs-spec mismatch throws (catchable, both backends).
                std::string_view index_part = spec.substr(0, colon);
                std::string_view fmt_part = spec.substr(colon + 1);
                if (index_part.empty() || is_all_digits(index_part)) {
                    detail::format_spec parsed;
                    if (!detail::parse_format_spec(fmt_part, parsed)) { unsupported_format_spec(fmt_part); }
                    size_t arg_index = next_arg;
                    if (index_part.empty()) {
                        next_arg++;
                    } else if (!positional_index(index_part, arg_index)) {
                        arg_index = arg_count;
                    } else {
                        arg_index += arg_offset;
                    }
                    if (arg_index < arg_count) {
                        result += detail::format_value_with_spec(detail::format_spec_needs_number(parsed) ? *args[arg_index] : *shown[arg_index], parsed, fmt_part);
                    }
                } else {
                    result += '{';
                    result.append(spec);
                    result += '}';
                }
            } else {
                // {n} - use positional argument
                size_t arg_index = 0;
                if (positional_index(spec, arg_index)) {
                    arg_index += arg_offset;
                    if (arg_index < arg_count) {
                        append_format_argument(result, *shown[arg_index]);
                    }
                } else {
                    // Invalid index, ignore
                    result += '{';
                    result.append(spec);
                    result += '}';
                }
            }

            pos = close_brace + 1;
        }
    }
    
    inline bool is_print_control(const script_value& v) {
        auto control = v.get_type_info();
        return control && (control->type_name == "skip_newline_t" || control->type_name == "skip_flush_t");
    }

    // shown: args with each container, object, handle and function replaced by its display text (left empty when every
    // argument is a scalar); a failing to_string() is the call's error, a throw out of one returns null with the throw in flight
    inline std::optional<checked_result<script_value>> show_arguments(engine* eng, const std::vector<script_value>& args, std::vector<script_value>& shown) {
        for (size_t i = 0; i < args.size() && eng; ++i) {
            const script_value& a = args[i];
            if (detail::plain_text_value(a) || is_print_control(a)) { continue; }
            if (shown.empty()) { shown = args; }
            auto text = eng->text_of(a);
            if (!text) { return checked_result<script_value>(text.error_value()); }
            if (detail::engine_access::backend_unwinding(*eng)) { return checked_result<script_value>(script_value(std::monostate{}, eng)); }
            shown[i] = script_value(std::move(text.value()), eng);
        }
        return std::nullopt;
    }

    // format(...) over its values where they are: the variadic entry's vector, or a vm call site's stack (its direct row).
    // literal: the first value is the call's format string (a string literal, or vformat's text), formatting when it holds a
    // placeholder; otherwise every value is text, the first one too
    inline checked_result<script_value> format_values(engine* eng, const script_value* args, size_t argc, bool literal) {
        // the control values (skip_newline, skip_flush) mean nothing here and drop out
        const script_value* inline_kept[8];
        const script_value* inline_shown[8];
        std::vector<const script_value*> spilled;
        const script_value** kept = inline_kept;
        const script_value** shown = inline_shown;
        size_t count = 0;
        if (argc > 8) {
            spilled.resize(argc * 2);
            kept = spilled.data();
            shown = spilled.data() + argc;
        }
        for (size_t i = 0; i < argc; ++i) {
            if (!is_print_control(args[i])) { kept[count++] = &args[i]; }
        }
        if (count == 0) {
            return script_value("", eng);
        }

        // a container's, an object's, a handle's or a function's display text; a failing to_string() is the call's error
        std::vector<script_value> texts;
        for (size_t i = 0; i < count; ++i) {
            shown[i] = kept[i];
            if (!eng || detail::plain_text_value(*kept[i])) { continue; }
            if (texts.empty()) { texts.reserve(count); }
            auto text = eng->text_of(*kept[i]);
            if (!text) { return text.error_value(); }
            if (detail::engine_access::backend_unwinding(*eng)) { return script_value(std::monostate{}, eng); }
            texts.emplace_back(std::move(text.value()), eng);
            shown[i] = &texts.back();
        }

        if (count == 1) {
            return script_value(shown[0]->to_string(), eng);
        }

        std::string result;
        if (literal && detail::plain_text_value(*kept[0])) {
            const script_value& first = shown[0]->is_reference() ? shown[0]->deref() : *shown[0];
            std::string first_text;
            std::string_view first_str;
            if (first.raw_storage_index() == script_value::TYPEID_STRING) {
                first_str = first.unchecked_as_string();
            } else {
                first_text = first.to_string();
                first_str = first_text;
            }
            if (has_format_placeholders(first_str)) {
                process_format_string(result, first_str, shown, kept, count);
                return script_value(std::move(result), eng);
            }
        }
        for (size_t i = 0; i < count; ++i) { append_format_argument(result, *shown[i]); }
        return script_value(std::move(result), eng);
    }

    // print(...) over its arguments; literal: format_values' rule for the first argument
    inline checked_result<script_value> print_values(engine* eng, const std::vector<script_value>& raw_args, bool literal) {
        // every value shows its display text, as to_string(x) and `${x}` do; a container's or an object's is never a format string
        std::vector<script_value> shown;
        if (auto failed = show_arguments(eng, raw_args, shown)) { return std::move(*failed); }
        const bool first_was_shown = !raw_args.empty() && !detail::plain_text_value(raw_args[0]);
        const std::vector<script_value>& args = shown.empty() ? raw_args : shown;
        std::ostream& out = eng ? detail::engine_access::get_output_stream(*eng) : std::cout;

        if (args.empty()) {
            out << std::endl;
            return script_value(std::monostate{}, eng);
        }

        bool should_newline = true;
        bool should_flush = true;
        size_t effective_args = args.size();

        if (!args.empty()) {
            const auto& last_arg = args.back();
            auto type_info = last_arg.get_type_info();
            if (type_info && type_info->type_name == "skip_newline_t") {
                should_newline = false;
                should_flush = true;
                effective_args--;
            } else if (type_info && type_info->type_name == "skip_flush_t") {
                should_newline = false;
                should_flush = false;
                effective_args--;
            }
        }

        if (effective_args == 0) {
            if (should_newline) {
                out << std::endl;
            } else if (should_flush) {
                out << std::flush;
            }
            return script_value(std::monostate{}, eng);
        }

        if (effective_args == 1) {
            // Single-arg print is VERBATIM passthrough by design:
            // {{ }} escapes only apply when format substitution runs — no reason to
            // alter the string when there is nothing to substitute
            out << args[0].to_string();
            if (should_newline) {
                out << std::endl;
            } else if (should_flush) {
                out << std::flush;
            }
            return script_value(std::monostate{}, eng);
        }

        std::string first_str = literal && !first_was_shown ? args[0].to_string() : std::string();
        bool has_format_specs = literal && !first_was_shown && has_format_placeholders(first_str);

        if (!has_format_specs) {
            for (size_t i = 0; i < effective_args; ++i) {
                out << args[i].to_string();
            }
            if (should_newline) {
                out << std::endl;
            } else if (should_flush) {
                out << std::flush;
            }
            return script_value(std::monostate{}, eng);
        }

        // the format string is args[0], the values start at args[1]
        std::vector<const script_value*> format_args;
        format_args.reserve(effective_args * 2);
        for (size_t i = 0; i < effective_args; ++i) { format_args.push_back(&args[i]); }
        for (size_t i = 0; i < effective_args; ++i) { format_args.push_back(&raw_args[i]); }
        std::string result;
        process_format_string(result, first_str, format_args.data(), format_args.data() + effective_args, effective_args);

        out << result;
        if (should_newline) {
            out << std::endl;
        } else if (should_flush) {
            out << std::flush;
        }
        return script_value(std::monostate{}, eng);
    }

    // Special types for controlling print behavior
    struct skip_newline_t {};
    struct skip_flush_t {};
    
    inline const skip_newline_t skip_newline{};
    inline const skip_flush_t skip_flush{};

    // THE object row, shared by type_of and reflect::type_name (stdlib/reflection.hpp):
    // the instance's own class first, then the value tag, then the bare kind - a value
    // tagged with a base class still answers the class it actually holds.
    std::string object_type_name(const script_value& d) {
        if (const auto* holder = d.peek_object_holder(); holder && holder->is_coroutine_handle()) { return "coroutine_handle"; }
        try {
            auto instance = d.as<strong_ptr<class_instance>>();
            if (instance) {
                if (auto def = instance->get_class_definition()) {
                    return def->get_name();
                }
            }
        } catch (...) {}
        if (auto ti = d.get_type_info()) {
            if (!ti->type_name.empty() && ti->type_name != "any") { return ti->type_name; }
        }
        return "object";
    }

    void register_io_functions(engine& eng_ref) {
        detail::engine_access::library_registration library(eng_ref);
        engine* eng = &eng_ref;
        dynamic_binder<skip_newline_t>(eng_ref, "skip_newline_t").build();
        dynamic_binder<skip_flush_t>(eng_ref, "skip_flush_t").build();
        
        auto skip_newline_ptr = std::make_shared<skip_newline_t>();
        auto skip_flush_ptr = std::make_shared<skip_flush_t>();

        eng_ref.add_global("skip_newline", detail::engine_access::make_value_object(eng_ref, skip_newline_ptr));
        eng_ref.add_global("skip_flush", detail::engine_access::make_value_object(eng_ref, skip_flush_ptr));
        // print(values...): every value's display text in a row, then a newline (a trailing skip_newline keeps the line,
        // skip_flush keeps it unflushed too). print(fmt, values...) with fmt a string literal holding a {} / {n} / {:spec}
        // placeholder is a format string, as in C++20; any other first argument is text like the rest. format is the same rule
        // returning the text, and vformat(fmt, values...) formats a run-time text the way format formats a literal.
        eng_ref.add_variadic_function("print", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
            return print_values(eng, args, false);
        });
        detail::engine_access::add_format_literal(eng_ref, "print", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
            return print_values(eng, args, true);
        }, [](const void* target, const script_value* args, size_t argc) -> checked_result<script_value> {
            return print_values(static_cast<engine*>(const_cast<void*>(target)), std::vector<script_value>(args, args + argc), true);
        }, eng);

        constexpr engine::host_direct_invoke format_literal = [](const void* target, const script_value* args, size_t argc) -> checked_result<script_value> {
            return format_values(static_cast<engine*>(const_cast<void*>(target)), args, argc, true);
        };
        eng_ref.add_variadic_function("format", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
            return format_values(eng, args.data(), args.size(), false);
        });
        eng_ref.add_host_direct("format", engine::host_direct_any_arity, [](const void* target, const script_value* args, size_t argc) -> checked_result<script_value> {
            return format_values(static_cast<engine*>(const_cast<void*>(target)), args, argc, false);
        }, eng);
        detail::engine_access::add_format_literal(eng_ref, "format", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
            return format_values(eng, args.data(), args.size(), true);
        }, format_literal, eng);
        eng_ref.add_variadic_function("vformat", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
            return format_values(eng, args.data(), args.size(), true);
        });
        eng_ref.add_host_direct("vformat", engine::host_direct_any_arity, format_literal, eng);

        // Register as variadic to bypass type matching issues
        // the display text string + value shows (a class's own to_string() speaks for its instances)
        constexpr engine::host_direct_invoke to_string_of = [](const void* target, const script_value* args, size_t) -> checked_result<script_value> {
            engine* e = static_cast<engine*>(const_cast<void*>(target));
            if (detail::plain_text_value(args[0])) { return script_value(args[0].to_string(), e); }
            auto text = e->text_of(args[0]);
            if (!text) { return text.error_value(); }
            if (detail::engine_access::backend_unwinding(*e)) { return script_value(std::monostate{}, e); }
            return script_value(std::move(text.value()), e);
        };
        eng_ref.add_variadic_function("to_string", [eng, to_string_of](const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 1) {
                throw runtime_error("to_string expects exactly 1 argument, got " + std::to_string(args.size()));
            }
            return to_string_of(eng, args.data(), 1);
        });
        eng_ref.add_host_direct("to_string", 1, to_string_of, eng);

        // Opaque host pointers (make_value(T*) on an UNREGISTERED
        // type) are non-null/truthy to script but the engine cannot name them — this
        // predicate is how scripts tell a registered/script object (true) from an opaque
        // host-only pass-through token (false). All ordinary values answer true.
        eng_ref.add_function("is_registered_type", [](const script_value& val) -> bool {
            const script_value& d = val.is_reference() ? val.deref() : val;
            if (d.raw_storage_index() == script_value::TYPEID_CPP_BOUND &&
                !d.is_int() && !d.is_float() && !d.is_bool() && !d.is_char() && !d.is_string()) {
                return false;
            }
            return true;
        });

        // Byte minting: explicit int -> char, range-checked.
        // The reverse of promotion; there is deliberately no IMPLICIT int -> char.
        eng_ref.add_function("to_char", [](script_int n) -> script_char {
            if (n < 0 || n > 255) {
                throw runtime_error("to_char expects 0..255, got " + std::to_string(n));
            }
            return static_cast<script_char>(static_cast<unsigned char>(n));
        });

        eng_ref.add_function("utf8_string", [eng = &eng_ref](const script_value& val) -> script_value {
            const script_value& v = val.deref();
            return script_value::make_utf8(v.is_text() ? v.unchecked_text() : v.to_string(), eng);
        });

        eng_ref.add_function("type_of", [](const script_value& val) -> std::string {
            switch (val.type()) {
                case script_value_type::jai_null_type: return "null";
                case script_value_type::jai_bool_type: return "bool";
                case script_value_type::jai_int_type: return "int";
                case script_value_type::jai_float_type: return "float";
                case script_value_type::jai_char_type: return "char";
                case script_value_type::jai_string_type: return "string";
                case script_value_type::jai_utf8_string_type: return "utf8_string";
                case script_value_type::jai_array_type: return "array";
                case script_value_type::jai_map_type: return "map";
                case script_value_type::jai_function_type: return "function";
                // A typed null keeps the bare kind (its tag is a class, its value is not)
                case script_value_type::jai_object_type:
                case script_value_type::jai_shared_ptr_type:
                    return val.is_object() ? object_type_name(val) : "object";
                case script_value_type::jai_reference_type: return "reference";
                case script_value_type::jai_weak_ptr_type: return "weak_ptr";
                case script_value_type::jai_vec2_type:
                case script_value_type::jai_vec3_type:
                case script_value_type::jai_vec4_type:
                case script_value_type::jai_color_type:
                case script_value_type::jai_ivec2_type:
                case script_value_type::jai_ivec3_type: return math_type_name(val.type());
                default: return "unknown";
            }
        });
    }

} // namespace stdlib
} // namespace jai
