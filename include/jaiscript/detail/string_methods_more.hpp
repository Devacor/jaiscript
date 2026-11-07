#pragma once

#ifndef __JAISCRIPT_DETAIL_STRING_METHODS_MORE_HPP__
#define __JAISCRIPT_DETAIL_STRING_METHODS_MORE_HPP__

// The everyday string methods beyond the C++ std::string surface (UTF-8 characters, lines, partition, three-way compare,
// character classes), registered into the one builtin registry both backends share. Observers: none writes its receiver.
// Needs engine.hpp (the typed-array constructors) ahead of it.

#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/utf8.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>

namespace jai::detail {

    inline void add_more_string_methods(std::unordered_map<uint64_t, builtin_method>& m, string_symbolizer* sym) {
        using R = checked_result<script_value>;
        auto count_error = [](const char* text) { return R(make_error_code(runtime_error_code::argument_count_mismatch), text); };
        auto type_error = [](const char* text) { return R(make_error_code(runtime_error_code::type_mismatch), text); };
        auto string_array = [](const builtin_method_context& ctx) { engine* eng = ctx.get_engine(); return script_value::make_array(engine_access::get_type_info_string(*eng), eng); };

        m[sym->intern("utf8_chars")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (!args.empty()) return count_error("utf8_chars() takes no arguments");
            const std::string& s = self.unchecked_text();
            script_value out = string_array(ctx);
            script_array& a = *out.unchecked_get_array_storage();
            auto& limits = engine_access::execution_limits(*ctx.get_engine());
            for (size_t at = 0; at < s.size();) {
                const utf8::step st = utf8::decode(s, at);
                if (!limits.memory_charge(sizeof(script_value) + st.length)) [[unlikely]] { return raise_memory_cap(limits); }
                a.push(ctx.make_value(s.substr(at, st.length)));   // an invalid byte is its own one-byte element: no text is lost
                at += st.length;
            }
            return out;
        };
        m[sym->intern("utf8_length")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (!args.empty()) return count_error("utf8_length() takes no arguments");
            const std::string& s = self.unchecked_text();
            script_int n = 0;
            for (size_t at = 0; at < s.size(); ++n) { at += utf8::decode(s, at).length; }
            return ctx.make_value(n);
        };
        m[sym->intern("is_valid_utf8")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (!args.empty()) return count_error("is_valid_utf8() takes no arguments");
            const std::string& s = self.unchecked_text();
            for (size_t at = 0; at < s.size();) { const utf8::step st = utf8::decode(s, at); if (!st.valid) return ctx.make_value(false); at += st.length; }
            return ctx.make_value(true);
        };
        m[sym->intern("code")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (!args.empty()) return count_error("code() takes no arguments");
            const std::string& s = self.unchecked_text();
            const utf8::step st = s.empty() ? utf8::step{} : utf8::decode(s, 0);
            if (s.empty() || !st.valid || st.length != s.size()) {
                return R(make_error_code(runtime_error_code::invalid_operation), "code() needs a string of exactly one UTF-8 character");
            }
            return ctx.make_value(static_cast<script_int>(st.codepoint));
        };
        m[sym->intern("byte_size")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (!args.empty()) return count_error("byte_size() takes no arguments");
            return ctx.make_value(static_cast<script_int>(self.unchecked_text().size()));
        };
        m[sym->intern("lines")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (!args.empty()) return count_error("lines() takes no arguments");
            const std::string& s = self.unchecked_text();
            script_value out = string_array(ctx);
            script_array& a = *out.unchecked_get_array_storage();
            auto& limits = engine_access::execution_limits(*ctx.get_engine());
            size_t start = 0;
            while (start < s.size()) {   // "\n" and "\r\n" end a line; a final line needs no terminator; no empty last element
                size_t end = s.find('\n', start);
                if (end == std::string::npos) end = s.size();
                size_t stop = end > start && s[end - 1] == '\r' ? end - 1 : end;
                if (!limits.memory_charge(sizeof(script_value) + (stop - start))) [[unlikely]] { return raise_memory_cap(limits); }
                a.push(ctx.make_value(s.substr(start, stop - start)));
                start = end + 1;
            }
            return out;
        };
        m[sym->intern("partition")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (args.size() != 1) return count_error("partition() takes exactly one argument (separator)");
            if (!args[0].is_text()) return type_error("partition() argument must be a string");
            const std::string& s = self.unchecked_text();
            const std::string& sep = args[0].unchecked_text();
            if (sep.empty()) return R(make_error_code(runtime_error_code::invalid_operation), "partition() separator cannot be empty");
            script_value out = string_array(ctx);
            script_array& a = *out.unchecked_get_array_storage();
            const size_t at = s.find(sep);
            if (at == std::string::npos) { a.push(ctx.make_value(s)); a.push(ctx.make_value(std::string())); a.push(ctx.make_value(std::string())); }
            else { a.push(ctx.make_value(s.substr(0, at))); a.push(ctx.make_value(sep)); a.push(ctx.make_value(s.substr(at + sep.size()))); }
            return out;
        };
        m[sym->intern("compare")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (args.size() != 1) return count_error("compare() takes exactly one argument");
            if (!args[0].is_text()) return type_error("compare() argument must be a string");
            const int c = self.unchecked_text().compare(args[0].unchecked_text());
            return ctx.make_value(static_cast<script_int>(c < 0 ? -1 : c > 0 ? 1 : 0));
        };
        // ASCII character classes: true when the string is non-empty and every byte is in the class
        auto char_class = [&](const char* name, const char* no_args, bool (*in_class)(unsigned char)) {
            m[sym->intern(name)] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
                if (!args.empty()) return count_error(no_args);
                const std::string& s = self.unchecked_text();
                if (s.empty()) return ctx.make_value(false);
                for (const char c : s) { if (!in_class(static_cast<unsigned char>(c))) return ctx.make_value(false); }
                return ctx.make_value(true);
            };
        };
        char_class("is_digit", "is_digit() takes no arguments", [](unsigned char c) { return c >= '0' && c <= '9'; });
        char_class("is_alpha", "is_alpha() takes no arguments", [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); });
        char_class("is_alnum", "is_alnum() takes no arguments", [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); });
        char_class("is_space", "is_space() takes no arguments", [](unsigned char c) { return c == ' ' || (c >= '\t' && c <= '\r'); });
        char_class("is_upper", "is_upper() takes no arguments", [](unsigned char c) { return c >= 'A' && c <= 'Z'; });
        char_class("is_lower", "is_lower() takes no arguments", [](unsigned char c) { return c >= 'a' && c <= 'z'; });
    }

} // namespace jai::detail

#endif
