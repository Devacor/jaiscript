#pragma once

#ifndef __JAISCRIPT_DETAIL_UTF8_METHODS_HPP__
#define __JAISCRIPT_DETAIL_UTF8_METHODS_HPP__

// utf8_string's builtin methods: the string library with character positions. Every string method serves a utf8_string receiver
// (the bodies read and write through unchecked_text); a string it returns comes back as a utf8_string. The methods that count or
// place characters are the character forms: size/length, at/front/back, substr, find/rfind, the find_*_of family, insert, erase,
// the pads, reverse and split(""). Positions translate through the node's caches (ASCII flag, remembered positions, breadcrumbs),
// written only while no parallel region runs. Needs engine.hpp ahead of it.

#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/utf8_text.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <algorithm>

namespace jai::detail::utf8 {
    inline bool may_cache(const builtin_method_context& ctx) { engine* e = ctx.get_engine(); return e && !engine_access::parallel_region_active(*e); }
}

namespace jai::detail {

    // the value a utf8_string method hands back: a string becomes a utf8_string, an array of strings an array of utf8_strings
    inline script_value utf8_result(const builtin_method_context& ctx, script_value r) {
        const size_t k = r.raw_storage_index();
        if (k == script_value::TYPEID_STRING) return script_value::make_utf8(std::move(r.unchecked_as_string_ref(ctx.get_engine())), ctx.get_engine());
        if (k == script_value::TYPEID_ARRAY) {
            const script_array* node = r.unchecked_array_node();
            bool strings = node && node->size() > 0 && (node->kind() == script_array::kind_t::hetero || node->kind() == script_array::kind_t::str);
            if (strings) node->for_each(ctx.get_engine(), [&](const script_value& e) { if (!e.is_string()) strings = false; });
            if (!strings) return r;
            script_value out = script_value::make_array(nullptr, ctx.get_engine());
            script_array& a = *out.unchecked_get_array_storage();
            node->for_each(ctx.get_engine(), [&](const script_value& e) { a.push(script_value::make_utf8(e.unchecked_as_string(), ctx.get_engine())); });
            return out;
        }
        return r;
    }

    inline std::unordered_map<uint64_t, builtin_method> make_utf8_methods(const std::unordered_map<uint64_t, builtin_method>& string_methods, string_symbolizer* sym) {
        using R = checked_result<script_value>;
        std::unordered_map<uint64_t, builtin_method> m;
        for (const auto& [id, method] : string_methods) {   // the whole string library, its string results as utf8_strings
            m[id] = [method](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
                R r = method(ctx, self, args);
                if (!r) return r;
                return utf8_result(ctx, std::move(r.value()));
            };
        }
        auto get = [&](const char* name) { return string_methods.at(sym->intern(name)); };
        auto count_error = [](const char* text) { return R(make_error_code(runtime_error_code::argument_count_mismatch), text); };
        auto node = [](const script_value& self) -> const script_utf8& { return self.unchecked_as_utf8(); };
        auto int_value = [](const builtin_method_context& ctx, size_t v) { return ctx.make_value(static_cast<script_int>(v)); };

        auto size_method = [=](const char* text) {
            return [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
                if (!args.empty()) return count_error(text);
                return int_value(ctx, utf8::length(node(self), utf8::may_cache(ctx)));
            };
        };
        m[sym->intern("size")] = size_method("size() takes no arguments");
        m[sym->intern("length")] = size_method("length() takes no arguments");
        m[sym->intern("utf8_length")] = size_method("utf8_length() takes no arguments");

        // one character as a utf8_string
        auto char_at = [](const builtin_method_context& ctx, const script_utf8& n, size_t ci, bool cache) {
            return utf8::char_value(n, ci, ctx.get_engine(), cache);
        };
        const builtin_method string_at = get("at");
        m[sym->intern("at")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (args.size() != 1 || !args[0].is_int()) return string_at(ctx, self, args);   // the same argument texts
            const bool cache = utf8::may_cache(ctx);
            const script_utf8& n = node(self);
            const size_t len = utf8::length(n, cache);
            const script_int i = utf8::normalize(args[0].unchecked_as_int(), len);
            if (i < 0 || i >= static_cast<script_int>(len)) return R(make_error_code(runtime_error_code::index_out_of_bounds), "at() index out of bounds");
            return char_at(ctx, n, static_cast<size_t>(i), cache);
        };
        for (const char* end : {"front", "back"}) {
            const builtin_method string_end = get(end);
            const bool front = end[0] == 'f';
            m[sym->intern(end)] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
                if (!args.empty() || node(self).bytes.empty()) return string_end(ctx, self, args);   // the same texts
                const bool cache = utf8::may_cache(ctx);
                const script_utf8& n = node(self);
                return char_at(ctx, n, front ? 0 : utf8::length(n, cache) - 1, cache);
            };
        }

        // positions in characters: normalize, translate to bytes, run the byte form, translate a returned position back
        auto byte_arg = [](const builtin_method_context& ctx, const script_utf8& n, script_int pos, bool cache) {
            const size_t len = utf8::length(n, cache);
            pos = utf8::normalize(pos, len);
            if (pos < 0) return pos;
            return static_cast<script_int>(utf8::byte_of(n, std::min(static_cast<size_t>(pos), len), cache));
        };
        const builtin_method string_substr = get("substr");
        m[sym->intern("substr")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (args.empty() || args.size() > 2 || !args[0].is_int() || (args.size() == 2 && !args[1].is_int())) return string_substr(ctx, self, args);
            const bool cache = utf8::may_cache(ctx);
            const script_utf8& n = node(self);
            const size_t len = utf8::length(n, cache);
            script_int start = utf8::normalize(args[0].unchecked_as_int(), len);
            if (start < 0) start = 0;
            if (start >= static_cast<script_int>(len)) return R(script_value::make_utf8(std::string(), ctx.get_engine()));
            const size_t b0 = utf8::byte_of(n, static_cast<size_t>(start), cache);
            size_t b1 = n.bytes.size();
            if (args.size() == 2) {
                script_int count = args[1].unchecked_as_int();
                if (count < 0) count = 0;
                const size_t end = std::min(len, static_cast<size_t>(start) + static_cast<size_t>(count));
                b1 = utf8::byte_of(n, end, cache);
            }
            return R(script_value::make_utf8(n.bytes.substr(b0, b1 - b0), ctx.get_engine()));
        };
        for (const char* name : {"find", "rfind"}) {
            const builtin_method string_find = get(name);
            m[sym->intern(name)] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
                const bool cache = utf8::may_cache(ctx);
                std::vector<script_value> byte_args = args;
                if (args.size() == 2 && args[1].is_int()) {
                    const script_int b = byte_arg(ctx, node(self), args[1].unchecked_as_int(), cache);
                    if (b < 0) { if (name[0] == 'r') return R(ctx.make_value(static_cast<script_int>(-1))); byte_args[1] = ctx.make_value(static_cast<script_int>(0)); }   // the byte form's rules: rfind finds nothing, find starts at 0
                    else byte_args[1] = ctx.make_value(b);
                }
                R r = string_find(ctx, self, byte_args);
                if (!r || !r.value().is_int() || r.value().unchecked_as_int() < 0) return r;
                return int_value(ctx, utf8::char_of(node(self), static_cast<size_t>(r.value().unchecked_as_int()), cache));
            };
        }
        const builtin_method string_insert = get("insert");
        m[sym->intern("insert")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (args.size() != 2 || !args[0].is_int()) return string_insert(ctx, self, args);
            const bool cache = utf8::may_cache(ctx);
            const script_utf8& n = node(self);
            const size_t len = utf8::length(n, cache);
            script_int pos = utf8::normalize(args[0].unchecked_as_int(), len);
            if (pos < 0) pos = 0;
            std::vector<script_value> byte_args = args;
            byte_args[0] = ctx.make_value(static_cast<script_int>(utf8::byte_of(n, std::min(static_cast<size_t>(pos), len), cache)));
            return string_insert(ctx, self, byte_args);
        };
        const builtin_method string_erase = get("erase");
        m[sym->intern("erase")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (args.empty() || args.size() > 2 || !args[0].is_int() || (args.size() == 2 && !args[1].is_int())) return string_erase(ctx, self, args);
            const bool cache = utf8::may_cache(ctx);
            const script_utf8& n = node(self);
            const size_t len = utf8::length(n, cache);
            const script_int pos = utf8::normalize(args[0].unchecked_as_int(), len);
            if (pos < 0 || pos >= static_cast<script_int>(len)) return R(ctx.make_value());   // out of bounds: no change, as the byte form
            const size_t b0 = utf8::byte_of(n, static_cast<size_t>(pos), cache);
            size_t b1 = n.bytes.size();
            if (args.size() == 2) {
                script_int count = args[1].unchecked_as_int();
                if (count < 0) count = 0;
                b1 = utf8::byte_of(n, std::min(len, static_cast<size_t>(pos) + static_cast<size_t>(count)), cache);
            }
            self.unchecked_as_utf8_ref().bytes.erase(b0, b1 - b0);
            return R(ctx.make_value());
        };

        // the character-set searches: a position of a character in (or not in) the argument's characters
        struct set_search { const char* name; bool last; bool negate; };
        for (const set_search ss : {set_search{"find_first_of", false, false}, set_search{"find_last_of", true, false}, set_search{"find_first_not_of", false, true}, set_search{"find_last_not_of", true, true}}) {
            const builtin_method string_search = get(ss.name);
            m[sym->intern(ss.name)] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
                if (args.empty() || args.size() > 2 || !args[0].text_bytes() || (args.size() == 2 && !args[1].is_int())) return string_search(ctx, self, args);
                const bool cache = utf8::may_cache(ctx);
                const script_utf8& n = node(self);
                const std::string& set = *args[0].text_bytes();
                std::vector<uint32_t> chars;
                for (size_t at = 0; at < set.size();) { const utf8::step st = utf8::decode(set, at); chars.push_back(st.valid ? st.codepoint : 0x110000u + static_cast<unsigned char>(set[at])); at += st.length; }
                std::vector<uint32_t> text;
                for (size_t at = 0; at < n.bytes.size();) { const utf8::step st = utf8::decode(n.bytes, at); text.push_back(st.valid ? st.codepoint : 0x110000u + static_cast<unsigned char>(n.bytes[at])); at += st.length; }
                const script_int len = static_cast<script_int>(text.size());
                script_int start = ss.last ? len - 1 : 0;
                if (args.size() == 2) {
                    start = utf8::normalize(args[1].unchecked_as_int(), text.size());
                    if (!ss.last && start < 0) start = 0;
                    if (ss.last && start >= len) start = len - 1;
                }
                auto in_set = [&](uint32_t c) { return std::find(chars.begin(), chars.end(), c) != chars.end(); };
                if (!ss.last) { for (script_int i = start; i < len; ++i) { if (in_set(text[static_cast<size_t>(i)]) != ss.negate) return int_value(ctx, static_cast<size_t>(i)); } }
                else { for (script_int i = start; i >= 0; --i) { if (in_set(text[static_cast<size_t>(i)]) != ss.negate) return int_value(ctx, static_cast<size_t>(i)); } }
                return R(ctx.make_value(static_cast<script_int>(-1)));
            };
        }

        // the pads: widths in characters, the fill's characters tiled from its start on each side (the byte form's rule)
        struct pad_kind { const char* name; int side; };   // -1 left, 1 right, 0 center
        for (const pad_kind pk : {pad_kind{"pad_left", -1}, pad_kind{"pad_right", 1}, pad_kind{"pad_center", 0}}) {
            const builtin_method string_pad = get(pk.name);
            m[sym->intern(pk.name)] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
                if (args.empty() || args.size() > 2 || !args[0].is_int()) return string_pad(ctx, self, args);
                std::string fill = " ";
                if (args.size() == 2) {
                    if (args[1].is_char()) { fill = std::string(1, args[1].unchecked_as_char()); }
                    else if (const std::string* f = args[1].text_bytes(); f && !f->empty()) { fill = *f; }
                    else { return string_pad(ctx, self, args); }   // the byte form's texts for an empty or wrong fill
                }
                const bool cache = utf8::may_cache(ctx);
                const size_t len = utf8::length(node(self), cache);
                const script_int want = args[0].unchecked_as_int();
                if (want <= static_cast<script_int>(len)) return R(ctx.make_value());
                const size_t total = saturating_count(want) - len;
                if (!builtin_memory_charge(ctx.get_engine(), total, fill.size())) [[unlikely]] { return builtin_memory_cap(ctx.get_engine()); }
                const size_t left = pk.side < 0 ? total : pk.side > 0 ? 0 : total / 2;
                auto tiled = [&](size_t characters) {
                    std::string out;
                    for (size_t at = 0; characters > 0; --characters) {
                        if (at == fill.size()) { at = 0; }
                        const size_t width = utf8::decode(fill, at).length;
                        out.append(fill, at, width);
                        at += width;
                    }
                    return out;
                };
                const std::string lhs = tiled(left), rhs = tiled(total - left);
                std::string& bytes = self.unchecked_as_utf8_ref().bytes;
                bytes = lhs + bytes + rhs;
                return R(ctx.make_value());
            };
        }
        const builtin_method string_reverse = get("reverse");
        m[sym->intern("reverse")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            if (!args.empty()) return string_reverse(ctx, self, args);
            std::string& bytes = self.unchecked_as_utf8_ref().bytes;
            std::string out; out.reserve(bytes.size());
            size_t end = bytes.size();
            std::vector<size_t> starts;
            for (size_t at = 0; at < bytes.size();) { starts.push_back(at); at += utf8::decode(bytes, at).length; }
            for (size_t k = starts.size(); k-- > 0;) { out.append(bytes, starts[k], end - starts[k]); end = starts[k]; }
            bytes = std::move(out);
            return R(ctx.make_value());
        };
        const builtin_method string_split = get("split");
        const builtin_method string_chars = get("utf8_chars");
        m[sym->intern("split")] = [=](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
            const std::string* sep = args.empty() ? nullptr : args[0].text_bytes();
            if (args.size() == 1 && sep && sep->empty()) {   // the empty separator: the characters
                R r = string_chars(ctx, self, std::vector<script_value>{});
                if (!r) return r;
                return utf8_result(ctx, std::move(r.value()));
            }
            R r = string_split(ctx, self, args);
            if (!r) return r;
            return utf8_result(ctx, std::move(r.value()));
        };
        m[sym->intern("explode")] = m[sym->intern("split")];
        return m;
    }

} // namespace jai::detail

#endif
