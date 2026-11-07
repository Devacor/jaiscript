#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/math_values.hpp>
#include <jaiscript/detail/text_hash.hpp>
#include <array>
#include <charconv>     // std::to_chars for shortest round-trippable float output
#include <cmath>
#include <cstring>
#include <limits>
#if !(defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L)
#include <jaiscript/detail/float_text.hpp>   // strtod fallback: libc++ ships integer-only std::from_chars
#include <clocale>
#include <cstdlib>
#endif

namespace jai::serialization {

json_archive_writer::json_archive_writer(int indent) :
    archive_writer_impl<json_archive_writer>(), indent_(indent), json_depth_(0) {}

json_archive_writer::json_archive_writer(int indent, engine* eng) :
    archive_writer_impl<json_archive_writer>(eng), indent_(indent), json_depth_(0) {}

json_archive_writer::json_archive_writer(engine* eng) :
    archive_writer_impl<json_archive_writer>(eng), indent_(2), json_depth_(0) {}

json_archive_writer::~json_archive_writer() = default;

json_archive_reader::~json_archive_reader() = default;

namespace {

    // A finite number in the shortest text that reads back exactly; a whole number keeps a ".0" so it reads back as a float
    template<typename F>
    void append_json_float(std::string& out, F value) {
        char buf[64];
        const auto res = std::to_chars(buf, buf + sizeof(buf), value);
        const std::string_view text(buf, static_cast<size_t>(res.ptr - buf));
        out.append(text);
        if (text.find_first_of(".eE") == std::string_view::npos) { out.append(".0"); }
    }

    template<typename I>
    void append_json_integer(std::string& out, I value) {
        char buf[24];
        const auto res = std::to_chars(buf, buf + sizeof(buf), value);
        out.append(buf, static_cast<size_t>(res.ptr - buf));
    }

    // 0: the byte stands for itself; otherwise its escape's second character ('u' for \u00XX)
    constexpr std::array<char, 256> json_escape_table() {
        std::array<char, 256> table{};
        for (int c = 0; c < 0x20; ++c) { table[static_cast<size_t>(c)] = 'u'; }
        table['"'] = '"';
        table['\\'] = '\\';
        table['\b'] = 'b';
        table['\f'] = 'f';
        table['\n'] = 'n';
        table['\r'] = 'r';
        table['\t'] = 't';
        return table;
    }
    constexpr std::array<char, 256> k_json_escape = json_escape_table();

}

// The text's bytes with JSON's escapes (UTF-8 at 0x80 and above passes through, valid JSON per RFC 8259); runs with nothing to
// escape go in whole
void json_archive_writer::append_escaped(std::string_view str) {
    static const char hexd[] = "0123456789abcdef";
    const char* p = str.data();
    const size_t n = str.size();
    size_t run = 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        const char e = k_json_escape[c];
        if (e == 0) { continue; }
        out_.append(p + run, i - run);
        if (e == 'u') {
            const char code[6] = {'\\', 'u', '0', '0', hexd[(c >> 4) & 0xF], hexd[c & 0xF]};
            out_.append(code, 6);
        } else {
            const char code[2] = {'\\', e};
            out_.append(code, 2);
        }
        run = i + 1;
    }
    out_.append(p + run, n - run);
}

// An array's element starts its own line; a property's or map entry's value follows its key
void json_archive_writer::begin_element() {
    auto ctx = this->current_context();
    bool after_property = (ctx == SerializationContext::PropertyValue || ctx == SerializationContext::MapValue);

    if (!after_property && in_container() && container_stack_.top() == ContainerType::Array) {
        if (!first_in_container_.top()) {
            out_ += ',';
        }
        write_newline();
        write_indent();
    }
}

void json_archive_writer::begin_scalar() {
    begin_element();
}

void json_archive_writer::end_scalar() {
    if (in_container() && container_stack_.top() == ContainerType::Array) {
        first_in_container_.top() = false;
    }
}

template<typename T>
void json_archive_writer::write_json_value(T value) {
    begin_scalar();
    if constexpr (std::is_same_v<T, bool>) {
        out_.append(value ? "true" : "false");
    } else if constexpr (std::is_floating_point_v<T>) {
        if (std::isnan(value)) {
            out_.append("null");
        } else if (std::isinf(value)) {
            out_.append(value > 0 ? "1e999" : "-1e999");
        } else {
            append_json_float(out_, value);
        }
    } else if constexpr (std::is_integral_v<T>) {
        append_json_integer(out_, value);
    } else {
        out_.append(value);
    }
    end_scalar();
}

void json_archive_writer::write_int8(int8_t value) { write_json_value(value); }

void json_archive_writer::write_int16(int16_t value) { write_json_value(value); }

void json_archive_writer::write_int32(int32_t value) { write_json_value(value); }

void json_archive_writer::write_int64(int64_t value) { write_json_value(value); }

void json_archive_writer::write_uint8(uint8_t value) { write_json_value(value); }

void json_archive_writer::write_uint16(uint16_t value) { write_json_value(value); }

void json_archive_writer::write_uint32(uint32_t value) { write_json_value(value); }

void json_archive_writer::write_uint64(uint64_t value) { write_json_value(value); }

void json_archive_writer::write_float32(float value) { write_json_value(value); }

void json_archive_writer::write_float64(double value) { write_json_value(value); }

void json_archive_writer::write_bool(bool value) { write_json_value(value); }

void json_archive_writer::write_null() { write_json_value(std::string_view("null")); }

void json_archive_writer::write_string(const std::string& value) {
    begin_scalar();
    out_ += '"';
    append_escaped(value);
    out_ += '"';
    end_scalar();
}

void json_archive_writer::write_binary(const void* data, size_t size) {
    static const char hexd[] = "0123456789abcdef";
    begin_scalar();
    out_.reserve(out_.size() + size * 2 + 2);
    out_ += '"';
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        out_ += hexd[(bytes[i] >> 4) & 0xF];
        out_ += hexd[bytes[i] & 0xF];
    }
    out_ += '"';
    end_scalar();
}

void json_archive_writer::open_container(char a_brace, ContainerType a_kind) {
    begin_element();
    out_ += a_brace;
    json_depth_++;
    container_stack_.push(a_kind);
    first_in_container_.push(true);
}

void json_archive_writer::close_container(char a_brace) {
    json_depth_--;
    container_stack_.pop();
    const bool empty = first_in_container_.top();
    first_in_container_.pop();
    if (!empty) {
        write_newline();
        write_indent();
    }
    out_ += a_brace;
    if (in_container()) {
        first_in_container_.top() = false;
    }
}

void json_archive_writer::begin_object() {
    enter_host_record(open_records_);
    ++open_records_;
    open_container('{', ContainerType::Object);
}

void json_archive_writer::begin_object(const std::string& type_name, uint32_t version) {
    json_archive_writer::begin_object();
    if (!type_name.empty()) {
        write_property_name("$type");
        write_string(type_name);
    }
    if (version > 1) {
        write_property_name("$version");
        write_uint32(version);
    }
}

void json_archive_writer::end_object() {
    --open_records_;
    close_container('}');
}

void json_archive_writer::begin_array(size_t size) {
    open_container('[', ContainerType::Array);
}

void json_archive_writer::end_array() {
    close_container(']');
}

void json_archive_writer::write_property_name(const std::string& name) {
    if (!first_in_container_.top()) {
        out_ += ',';
    }
    write_newline();
    write_indent();
    out_ += '"';
    append_escaped(name);
    out_.append("\": ");
    first_in_container_.top() = false;
    // Context is managed by base class serialize() which pushes PropertyValue after this
}

void json_archive_writer::begin_map(size_t size) {
    open_container('{', ContainerType::Object);
}

void json_archive_writer::end_map() {
    close_container('}');
}

void json_archive_writer::write_map_key(const std::string& key) {
    json_archive_writer::write_property_name(key);
}

void json_archive_writer::write_value(const script_value& root) {
    std::vector<write_frame> frames;
    frames.reserve(8);
    if (out_.capacity() < 256) { out_.reserve(256); }
    const int depth_at_entry = json_depth_;
    begin_value(root, frames);
    script_value scratch(std::monostate{}, engine_ref_);
    while (!frames.empty()) {
        const script_value* child = next_child(frames.back(), scratch);
        if (aborted_) [[unlikely]] {
            json_depth_ = depth_at_entry;   // a getter threw into the backend: the in-flight exception leaves the archive
            aborted_ = false;
            return;
        }
        if (child) {
            begin_value(*child, frames);
        } else {
            end_frame(frames.back());
            frames.pop_back();
        }
    }
}

std::string json_archive_writer::str() const { return out_; }

std::string json_archive_writer::take() { return std::move(out_); }

void json_archive_writer::begin_value(const script_value& given, std::vector<write_frame>& frames) {
    const script_value& value = given.is_reference() ? given.deref() : given;
    switch (value.type()) {
        case script_value_type::jai_weak_ptr_type: {
            const auto shared = value.get_weak_ptr().lock();
            if (!shared) {
                out_.append("null");
                return;
            }
            begin_handle("$ref", shared, frames);
            return;
        }

        case script_value_type::jai_shared_ptr_type: {
            const auto holder = value.get_object_holder();
            if (!holder) {
                out_.append("null");
                return;
            }
            begin_handle("$id", holder, frames);
            return;
        }

        case script_value_type::jai_array_type: {
            const script_array* node = value.unchecked_array_node();
            if (!node) { throw runtime_error("script_value is not an array"); }
            open_value('[');
            write_frame& f = frames.emplace_back(write_shape::array, value);
            f.count = node->size();
            return;
        }

        case script_value_type::jai_map_type: {
            const auto& map = value.as_map();
            if (map_needs_tag(value)) {
                open_value('{');
                write_value_key("$map", false);
                open_value('[');
                write_frame& f = frames.emplace_back(write_shape::tagged_map, value);
                f.next = map.begin();
                return;
            }
            open_value('{');
            write_frame& f = frames.emplace_back(write_shape::map, value);
            f.next = map.begin();
            return;
        }

        case script_value_type::jai_object_type: {
            if (!engine_ref_) {
                out_.append("null");
                return;
            }
            if (strong_ptr<class_instance> instance = written_instance(value)) {
                open_value('{');
                begin_instance_members(value, std::move(instance), false, frames);
                return;
            }
            write_leaf(value);
            return;
        }

        default:
            write_leaf(value);
            return;
    }
}

strong_ptr<class_instance> json_archive_writer::written_instance(const script_value& value) {
    try {
        return value.as<strong_ptr<class_instance>>();
    } catch (...) {
        return nullptr;
    }
}

void json_archive_writer::begin_handle(std::string_view id_key, const strong_ptr<script_value::object_holder>& holder, std::vector<write_frame>& frames) {
    auto [id, is_new] = track_shared_ptr(holder.get());
    open_value('{');
    write_value_key(id_key, false);
    append_json_integer(out_, id);
    if (is_new) {
        script_value object = weak_target_value(holder);
        if (strong_ptr<class_instance> instance = engine_ref_ ? written_instance(object) : nullptr) {
            begin_instance_members(object, std::move(instance), true, frames);
            return;
        }
        write_value_key("$val", true);
        if (engine_ref_) { write_leaf(object); } else { out_.append("null"); }
    }
    close_value('}', false);
}

void json_archive_writer::begin_instance_members(const script_value& value, strong_ptr<class_instance> instance, bool comma,
                                                 std::vector<write_frame>& frames) {
    class_definition* class_def = instance->get_class_definition();
    std::string type_name;
    if (class_def) {
        type_name = class_def->get_name();
    } else {
        auto type_info = value.get_type_info();
        type_name = type_info ? type_info->type_name : "unknown";
    }
    write_value_key("$type", comma);
    out_ += '"';
    append_escaped(type_name);
    out_ += '"';
    write_value_key("$val", true);
    open_value('{');
    if (class_def) {
        write_frame& f = frames.emplace_back(write_shape::instance, value);
        f.names = class_def->get_property_names();
        f.class_def = class_def;
        f.instance = std::move(instance);
    } else {
        // slot order: deterministic, close to declaration order
        write_frame& f = frames.emplace_back(write_shape::fields, value);
        instance->for_each_field([&](uint64_t field_id, const script_value& field_val) {
            f.fields.emplace_back(std::string(detail::engine_access::get_symbolizer(*engine_ref_)->get_string(field_id)), field_val);
        });
    }
}

const script_value* json_archive_writer::next_child(write_frame& f, script_value& scratch) {
    switch (f.shape) {
        case write_shape::array: {
            if (f.index >= f.count) { return nullptr; }
            write_value_separator(f.index > 0);
            const script_array* node = f.source.unchecked_array_node();
            const size_t i = f.index++;
            if (node->kind() == script_array::kind_t::hetero) { return &node->values()[i]; }
            scratch = node->get(i, node->owner);
            return &scratch;
        }
        case write_shape::map: {
            if (f.next == f.source.as_map().end()) { return nullptr; }
            const script_value& k = f.next->first;
            if (k.is_string()) {
                write_value_key(k.as_string(), !f.first);
            } else {
                write_value_key(k.to_string(), !f.first);
            }
            f.first = false;
            const script_value* child = &f.next->second;
            ++f.next;
            return child;
        }
        case write_shape::tagged_map:
            for (;;) {
                if (f.phase == 2) {
                    close_value(']', false);
                    ++f.next;
                    f.phase = 0;
                }
                if (f.phase == 0) {
                    if (f.next == f.source.as_map().end()) { return nullptr; }
                    write_value_separator(!f.first);
                    f.first = false;
                    open_value('[');
                    write_value_separator(false);
                    f.phase = 1;
                    return &f.next->first;
                }
                write_value_separator(true);
                f.phase = 2;
                return &f.next->second;
            }
        case write_shape::instance: {
            if (!next_serialized_property(f.source, f.instance.get(), f.class_def, engine_ref_, f.names, f.index, scratch, aborted_)) { return nullptr; }
            write_value_key(f.names[f.index - 1], !f.first);
            f.first = false;
            return &scratch;
        }
        case write_shape::fields:
            if (f.index >= f.fields.size()) { return nullptr; }
            write_value_key(f.fields[f.index].first, !f.first);
            f.first = false;
            return &f.fields[f.index++].second;
    }
    return nullptr;
}

void json_archive_writer::end_frame(write_frame& f) {
    switch (f.shape) {
        case write_shape::array:
            close_value(']', f.count == 0);
            break;
        case write_shape::map:
            close_value('}', f.first);
            break;
        case write_shape::tagged_map:
            close_value(']', f.first);
            close_value('}', false);
            break;
        case write_shape::instance:
        case write_shape::fields:
            close_value('}', f.first);
            close_value('}', false);
            break;
    }
}

void json_archive_writer::open_value(char bracket) {
    out_ += bracket;
    ++json_depth_;
}

void json_archive_writer::close_value(char bracket, bool empty) {
    --json_depth_;
    if (indent_ > 0 && !empty) {
        out_ += '\n';
        append_indentation();
    }
    out_ += bracket;
}

void json_archive_writer::write_value_separator(bool comma) {
    if (comma) { out_ += ','; }
    if (indent_ > 0) {
        out_ += '\n';
        append_indentation();
    }
}

void json_archive_writer::append_indentation() {
    const size_t width = detail::saturating_text_size(static_cast<size_t>(json_depth_), static_cast<size_t>(indent_));
    if (charges_indentation_ && engine_ref_ && !detail::engine_access::execution_limits(*engine_ref_).memory_charge_items(1, width)) [[unlikely]] {
        detail::throw_memory_cap(detail::engine_access::execution_limits(*engine_ref_));
    }
    out_.append(width, ' ');
}

void json_archive_writer::write_value_key(std::string_view key, bool comma) {
    write_value_separator(comma);
    out_ += '"';
    append_escaped(key);
    out_.append(indent_ > 0 ? "\": " : "\":");
}

bool json_archive_writer::map_needs_tag(const script_value& value) {
    const auto& map = value.as_map();
    auto string_like = [](script_value_type t) { return t == script_value_type::jai_string_type || t == script_value_type::jai_utf8_string_type; };
    for (const auto& [k, v] : map) {
        if (!string_like(k.type())) return true;
    }
    if (!map.empty()) return false;
    const auto ti = value.get_type_info();
    const auto kt = ti ? ti->key_type() : nullptr;
    return kt && !string_like(kt->base_type) && kt->base_type != script_value_type::jai_any_type &&
           kt->base_type != script_value_type::jai_null_type;
}

namespace {
    // a float JSON has no number for: {"$float": "inf"}, "-inf" or "nan", as $char tags a char
    void append_json_script_float(std::string& out, double d) {
        if (std::isfinite(d)) { append_json_float(out, d); return; }
        out.append(d != d ? "{\"$float\":\"nan\"}" : d > 0 ? "{\"$float\":\"inf\"}" : "{\"$float\":\"-inf\"}");
    }
}

void json_archive_writer::write_leaf(const script_value& val) {
    switch (val.type()) {
        case script_value_type::jai_null_type:
            out_.append("null");
            break;

        case script_value_type::jai_bool_type:
            out_.append(val.as_bool() ? "true" : "false");
            break;

        case script_value_type::jai_int_type:
            append_json_integer(out_, val.as_int());
            break;

        case script_value_type::jai_float_type:
            append_json_script_float(out_, val.as_float());
            break;

        case script_value_type::jai_string_type: {
            const script_value& s = val.deref();
            out_ += '"';
            if (s.raw_storage_index() == script_value::TYPEID_STRING) { append_escaped(s.unchecked_as_string()); } else { append_escaped(val.as_string()); }
            out_ += '"';
            break;
        }

        case script_value_type::jai_utf8_string_type:
            out_ += '"';
            append_escaped(val.deref().unchecked_as_utf8().bytes);
            out_ += '"';
            break;

        case script_value_type::jai_vec2_type:
        case script_value_type::jai_vec3_type:
        case script_value_type::jai_vec4_type:
        case script_value_type::jai_color_type:
        case script_value_type::jai_ivec2_type:
        case script_value_type::jai_ivec3_type: {
            // {"$type": "math::K", "$val": {canonical components}}: binary32 in the shortest round-tripping text
            const script_value& m = val.deref();
            const script_value_type kind = m.unchecked_vec_kind();
            out_.append("{\"$type\":\"");
            out_.append(detail::math::kind_name(kind));
            out_.append("\",\"$val\":{");
            for (size_t k = 0; k < detail::math::dims(kind); ++k) {
                if (k) { out_ += ','; }
                out_ += '"'; out_.append(detail::math::component_name(kind, k)); out_.append("\":");
                if (detail::math::integral(kind)) {
                    append_json_integer(out_, detail::math::i32_at(m, k));
                } else {
                    const float c = detail::math::f32_at(m, k);
                    if (std::isfinite(c)) { detail::math::append_f32(out_, c); } else { append_json_script_float(out_, c); }
                }
            }
            out_.append("}}");
            break;
        }

        case script_value_type::jai_char_type:
            out_.append("{\"$char\":");
            append_json_integer(out_, static_cast<int>(static_cast<unsigned char>(val.as_char())));
            out_ += '}';
            break;

        default:
            out_.append("null");
            break;
    }
}

bool json_archive_writer::in_container() const { return !container_stack_.empty(); }

void json_archive_writer::write_indent() {
    if (indent_ > 0) {
        append_indentation();
    }
}

void json_archive_writer::write_newline() {
    if (indent_ > 0) {
        out_ += '\n';
    }
}
json_archive_reader::json_archive_reader(std::string_view json_string, engine* eng) : archive_reader_impl<json_archive_reader>(eng) {
    if (!eng) {
        throw serialization_error("json_archive_reader requires a valid engine reference");
    }
    json_ = json_string;
    parse_into_arena();   // builds the flat arena DOM (nodes_/chars_); root_ is the last node
    json_ = {};           // the flat DOM is self-contained
    key_table_.clear();
    key_table_.shrink_to_fit();
}

uint32_t json_archive_reader::intern_key(uint32_t off, uint32_t len) {
    const std::string_view text(chars_.data() + off, len);
    constexpr size_t scanned_keys = 16;   // up to this many distinct keys a scan finds them; past it, the hash table
    if (key_spans_.size() < scanned_keys) {
        for (uint32_t slot = 0; slot < key_spans_.size(); ++slot) {
            const auto [o, l] = key_spans_[slot];
            if (l == len && std::memcmp(chars_.data() + o, text.data(), len) == 0) {
                if (off + len == chars_.size()) { chars_.resize(off); }
                return slot;
            }
        }
        if (key_spans_.empty()) { key_spans_.reserve(scanned_keys); }
        key_spans_.emplace_back(off, len);
        return static_cast<uint32_t>(key_spans_.size() - 1);
    }
    if (key_spans_.size() * 2 >= key_table_.size()) {
        std::vector<uint32_t> table(key_table_.empty() ? 64 : key_table_.size() * 2, 0);
        const size_t mask = table.size() - 1;
        for (uint32_t slot = 0; slot < key_spans_.size(); ++slot) {
            const auto [o, l] = key_spans_[slot];
            size_t i = detail::text_hash(std::string_view(chars_.data() + o, l)) & mask;
            while (table[i]) { i = (i + 1) & mask; }
            table[i] = slot + 1;
        }
        key_table_.swap(table);
    }
    const size_t mask = key_table_.size() - 1;
    for (size_t i = detail::text_hash(text) & mask;; i = (i + 1) & mask) {
        const uint32_t place = key_table_[i];
        if (!place) {
            key_spans_.emplace_back(off, len);
            key_table_[i] = static_cast<uint32_t>(key_spans_.size());
            return static_cast<uint32_t>(key_spans_.size() - 1);
        }
        const auto [o, l] = key_spans_[place - 1];
        if (l == len && std::memcmp(chars_.data() + o, text.data(), len) == 0) {
            if (off + len == chars_.size()) { chars_.resize(off); }   // the key's text was just appended: the earlier copy serves
            return place - 1;
        }
    }
}

const script_value& json_archive_reader::key_value(const JNode& k, engine* eng) {
    const uint32_t slot = k.key - 1;
    if (key_values_.size() <= slot) { key_values_.resize(key_spans_.size(), script_value(std::monostate{}, eng)); }
    script_value& made = key_values_[slot];
    if (made.raw_storage_index() != script_value::TYPEID_STRING) {
        made = script_value(script_string(chars_.data() + k.u.str.off, k.u.str.len), eng);
    }
    return made;
}

template<typename T>
T json_archive_reader::read_integer() {
    uint32_t n;
    if (!next_node(n)) n = root_;
    const JNode& node = nodes_[n];
    switch (node.tag) {
        case JTag::Int: return archive_integer<T>(node.u.i);
        case JTag::UInt: return archive_integer<T>(node.u.u);
        case JTag::Double: return archive_integer<T>(node.u.d);
        default: throw_archive_expected("an integer", found_kind(true, n));
    }
}

int8_t json_archive_reader::read_int8() { return read_integer<int8_t>(); }

int16_t json_archive_reader::read_int16() { return read_integer<int16_t>(); }

int32_t json_archive_reader::read_int32() { return read_integer<int32_t>(); }

int64_t json_archive_reader::read_int64() { return read_integer<int64_t>(); }

uint8_t json_archive_reader::read_uint8() { return read_integer<uint8_t>(); }

uint16_t json_archive_reader::read_uint16() { return read_integer<uint16_t>(); }

uint32_t json_archive_reader::read_uint32() { return read_integer<uint32_t>(); }

uint64_t json_archive_reader::read_uint64() { return read_integer<uint64_t>(); }

float json_archive_reader::read_float32() { return static_cast<float>(read_value().as<script_float>()); }

double json_archive_reader::read_float64() { return read_value().as<script_float>(); }

bool json_archive_reader::read_bool() { return read_value().as<script_bool>(); }

std::string json_archive_reader::read_string() { return read_value().as<script_string>(); }

bool json_archive_reader::peek_null() {
    uint32_t n;
    if (!next_node(n, /*peek_only=*/true)) return false;
    return nodes_[n].tag == JTag::Null;
}

void json_archive_reader::read_null() {
    auto val = read_value();
    if (!val.is_null()) {
        throw serialization_error("Expected null value");
    }
}

std::vector<uint8_t> json_archive_reader::read_binary(size_t /*size*/) {
    std::string hex_str = read_value().as<script_string>();
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> result;
    result.reserve(hex_str.length() / 2);
    // Decode complete byte-pairs only; validates each nibble and skips an odd trailing nibble.
    for (size_t i = 0; i + 1 < hex_str.length(); i += 2) {
        int hi = nibble(hex_str[i]);
        int lo = nibble(hex_str[i + 1]);
        if (hi < 0 || lo < 0) {
            throw serialization_error("Invalid hex digit in binary JSON field");
        }
        result.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return result;
}

void json_archive_reader::begin_object() {
    std::string type_name;
    uint32_t version = 0;
    begin_object(type_name, version);
}

const char* json_archive_reader::found_kind(bool present, uint32_t n) const {
    if (!present) { return "nothing"; }
    switch (nodes_[n].tag) {
        case JTag::Null: return "null";
        case JTag::Bool: return "a bool";
        case JTag::Int:
        case JTag::UInt: return "an integer";
        case JTag::Double: return "a float";
        case JTag::Str: return "a string";
        case JTag::Arr: return "an array";
        default: return "an object";
    }
}

void json_archive_reader::begin_object(std::string& type_name, uint32_t& version) {
    enter_host_record(object_stack_.size());
    uint32_t n = 0;
    const bool present = next_node(n);
    if (!present || nodes_[n].tag != JTag::Obj) {
        throw_archive_expected("an object", found_kind(present, n));
    }
    const JNode& obj = nodes_[n];
    const uint32_t first = obj.u.agg.first;
    const uint32_t count = obj.u.agg.count;
    type_name.clear();
    version = 1;
    for (uint32_t m = 0; m < count; ++m) {
        const JNode& k = nodes_[first + 2 * m];
        std::string_view kv(chars_.data() + k.u.str.off, k.u.str.len);
        const JNode& v = nodes_[first + 2 * m + 1];
        if (kv == "$type" && v.tag == JTag::Str) {
            type_name.assign(chars_.data() + v.u.str.off, v.u.str.len);
        } else if (kv == "$version" && v.tag == JTag::Int) {
            version = archive_integer<uint32_t>(v.u.i);
        }
    }
    version_ = version;
    object_stack_.push(ObjState{ first, count, 0, nullptr });
}

void json_archive_reader::end_object() {
    if (!object_stack_.empty()) {
        object_stack_.pop();
    }
}

size_t json_archive_reader::begin_array() {
    uint32_t n = 0;
    const bool present = next_node(n);
    if (!present || nodes_[n].tag != JTag::Arr) {
        throw_archive_expected("an array", found_kind(present, n));
    }
    const JNode& a = nodes_[n];
    array_stack_.push(ArrState{ a.u.agg.first, a.u.agg.count, 0 });
    return a.u.agg.count;
}

void json_archive_reader::end_array() {
    if (!array_stack_.empty()) {
        array_stack_.pop();
    }
}

bool json_archive_reader::read_property_name(std::string& name) {
    if (object_stack_.empty()) {
        return false;
    }
    auto& s = object_stack_.top();
    // the record's $type/$version are its metadata, not properties
    while (s.cursor < s.count) {
        const JNode& k = nodes_[s.first + 2 * s.cursor];
        std::string_view kv(chars_.data() + k.u.str.off, k.u.str.len);
        const uint32_t valNode = s.first + 2 * s.cursor + 1;
        ++s.cursor;
        if (kv != "$type" && kv != "$version") {
            name.assign(kv);
            cpv_node_ = valNode;
            cpv_valid_ = true;
            cpv_depth_ = object_stack_.size();
            return true;
        }
    }
    return false;
}

size_t json_archive_reader::begin_map() {
    enter_host_record(object_stack_.size());
    uint32_t n = 0;
    const bool present = next_node(n);
    if (!present || nodes_[n].tag != JTag::Obj) {
        throw_archive_expected("a map", found_kind(present, n));
    }
    const JNode& o = nodes_[n];
    object_stack_.push(ObjState{ o.u.agg.first, o.u.agg.count, 0, nullptr });
    return o.u.agg.count;
}

void json_archive_reader::end_map() {
    if (!object_stack_.empty()) {
        object_stack_.pop();
    }
}

bool json_archive_reader::read_map_key(std::string& key) {
    if (object_stack_.empty()) {
        return false;
    }
    auto& s = object_stack_.top();
    if (s.cursor < s.count) {
        const JNode& k = nodes_[s.first + 2 * s.cursor];
        key.assign(chars_.data() + k.u.str.off, k.u.str.len);
        cpv_node_ = s.first + 2 * s.cursor + 1;
        cpv_valid_ = true;
        cpv_depth_ = object_stack_.size();
        ++s.cursor;
        return true;
    }
    return false;
}

bool json_archive_reader::in_array() const { return !array_stack_.empty(); }

bool json_archive_reader::has_property(const std::string& name) {
    if (object_stack_.empty()) {
        return false;
    }
    return object_index(object_stack_.top()).count(std::string_view(name)) > 0;
}

bool json_archive_reader::seek_property(const std::string& name) {
    if (object_stack_.empty()) {
        return false;
    }
    auto& s = object_stack_.top();
    auto& idx = object_index(s);
    auto it = idx.find(std::string_view(name));
    if (it == idx.end()) {
        return false;
    }
    cpv_node_ = it->second;
    cpv_valid_ = true;
    cpv_depth_ = object_stack_.size();
    return true;
}

script_value json_archive_reader::read_value() {
    auto eng = engine_ref_;
    if (!eng) {
        throw serialization_error("Engine reference expired during JSON deserialization");
    }

    uint32_t n;
    if (!next_node(n)) n = root_;   // matches the old "fall back to the root value"
    const JNode& node = nodes_[n];

    if (node.tag == JTag::Obj || node.tag == JTag::Arr) {
        return materialize(n);
    }
    return materialize_primitive(node, eng);
}

bool json_archive_reader::next_node(uint32_t& out, bool peek_only) {
    if (cpv_valid_ && cpv_depth_ == object_stack_.size()) {
        out = cpv_node_;
        if (!peek_only) cpv_valid_ = false;
        return true;
    }
    cpv_valid_ = false;
    if (!array_stack_.empty()) {
        auto& a = array_stack_.top();
        if (a.index < a.count) {
            out = a.first + (peek_only ? a.index : a.index++);
            return true;
        }
        return false;
    }
    out = root_;
    return true;
}

script_value json_archive_reader::materialize_primitive(const JNode& node, engine* eng) {
    switch (node.tag) {
        case JTag::Null:   return script_value(std::monostate{}, eng);
        case JTag::Bool:   return script_value(static_cast<script_bool>(node.u.i != 0), eng);
        case JTag::Int:    return script_value(static_cast<script_int>(node.u.i), eng);
        case JTag::UInt:   return script_value(static_cast<script_float>(node.u.u), eng);
        case JTag::Double: return script_value(node.u.d, eng);
        case JTag::Str:    return script_value(std::string(chars_.data() + node.u.str.off, node.u.str.len), eng);
        default:           return script_value(std::monostate{}, eng);
    }
}

bool json_archive_reader::materialize_math(const JNode& node, engine* eng, script_value& out) {
    if (node.u.agg.count != 2) { return false; }
    uint32_t type_node = 0, val_node = 0;
    bool has_type = false, has_val = false;
    for (uint32_t m = 0; m < 2; ++m) {
        const JNode& k = nodes_[node.u.agg.first + 2 * m];
        std::string_view key(chars_.data() + k.u.str.off, k.u.str.len);
        if (key == "$type") { type_node = node.u.agg.first + 2 * m + 1; has_type = true; }
        else if (key == "$val") { val_node = node.u.agg.first + 2 * m + 1; has_val = true; }
    }
    if (!has_type || !has_val || nodes_[type_node].tag != JTag::Str) { return false; }
    const JNode& t = nodes_[type_node];
    const script_value_type kind = math_type_from_name(std::string_view(chars_.data() + t.u.str.off, t.u.str.len));
    if (kind == script_value_type::jai_null_type) { return false; }
    const JNode& val = nodes_[val_node];
    const size_t dims = detail::math::dims(kind);
    if (val.tag != JTag::Obj || val.u.agg.count != dims) {
        throw serialization_error(std::string("Malformed ") + math_type_name(kind) + " in JSON: expected " + std::to_string(dims) + " components");
    }
    float f[4] = {0, 0, 0, 0};
    int32_t i[4] = {0, 0, 0, 0};
    bool seen[4] = {false, false, false, false};
    auto text_of = [&](const JNode& n) { return std::string_view(chars_.data() + n.u.str.off, n.u.str.len); };
    // a float component no JSON number spells: {"$float": "inf" | "-inf" | "nan"}
    auto tagged_float = [&](const JNode& c, float& out) {
        if (c.tag != JTag::Obj || c.u.agg.count != 1 || detail::math::integral(kind)) { return false; }
        const JNode& tag = nodes_[c.u.agg.first];
        const JNode& spelled = nodes_[c.u.agg.first + 1];
        if (text_of(tag) != "$float" || spelled.tag != JTag::Str) { return false; }
        const std::string_view text = text_of(spelled);
        if (text != "inf" && text != "-inf" && text != "nan") { return false; }
        out = text == "nan" ? std::numeric_limits<float>::quiet_NaN() : text == "inf" ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
        return true;
    };
    for (uint32_t m = 0; m < val.u.agg.count; ++m) {
        const JNode& k = nodes_[val.u.agg.first + 2 * m];
        const JNode& c = nodes_[val.u.agg.first + 2 * m + 1];
        const int comp = detail::math::component_of(kind, text_of(k));
        float tagged = 0;
        const bool is_tagged = comp >= 0 && tagged_float(c, tagged);
        if (comp < 0 || seen[comp] || (c.tag != JTag::Int && c.tag != JTag::UInt && c.tag != JTag::Double && !is_tagged)) {
            throw serialization_error(std::string("Malformed ") + math_type_name(kind) + " component in JSON");
        }
        seen[comp] = true;
        if (is_tagged) {
            f[comp] = tagged;
            continue;
        }
        if (detail::math::integral(kind)) {
            auto narrowed = c.tag == JTag::Int ? detail::math::to_i32(static_cast<script_int>(c.u.i))
                : detail::math::to_i32(c.tag == JTag::UInt ? static_cast<script_float>(c.u.u) : c.u.d);
            if (!narrowed) { throw serialization_error(std::string("Out-of-range ") + math_type_name(kind) + " component in JSON"); }
            i[comp] = narrowed.value();
        } else {
            f[comp] = c.tag == JTag::Int ? detail::math::to_f32(static_cast<script_int>(c.u.i))
                : detail::math::to_f32(c.tag == JTag::UInt ? static_cast<script_float>(c.u.u) : c.u.d);
        }
    }
    out = detail::math::make(kind, f, i, eng);
    return true;
}

script_value json_archive_reader::materialize(uint32_t idx) {
    engine* eng = engine_ref_;
    struct frame { uint32_t node; uint32_t next; script_value container; };
    std::vector<frame> frames;
    frames.reserve(8);
    // a leaf's value, or nothing when n opened a container frame
    auto open = [&](uint32_t n, script_value& leaf) -> bool {
        const JNode& node = nodes_[n];
        if (node.tag == JTag::Obj) {
            script_value math_value(std::monostate{}, eng);
            if (materialize_math(node, eng, math_value)) {
                leaf = std::move(math_value);
                return true;
            }
            frames.push_back({n, 0, script_value::make_map(detail::engine_access::get_type_info_string(*eng), nullptr, eng)});
            const_cast<script_map&>(frames.back().container.as_map()).reserve(node.u.agg.count);
            return false;
        }
        if (node.tag == JTag::Arr) {
            script_value av = script_value::make_array(nullptr, eng);
            const_cast<std::vector<script_value>&>(av.as_array()).reserve(node.u.agg.count);
            frames.push_back({n, 0, std::move(av)});
            return false;
        }
        leaf = materialize_primitive(node, eng);
        return true;
    };
    // the child just made goes into the container whose member index - 1 it is
    auto attach = [&](frame& parent, script_value&& child) {
        const JNode& node = nodes_[parent.node];
        if (node.tag == JTag::Obj) {
            const JNode& k = nodes_[node.u.agg.first + 2 * (parent.next - 1)];
            const_cast<script_map&>(parent.container.as_map()).emplace(key_value(k, eng), std::move(child));
        } else {
            const_cast<std::vector<script_value>&>(parent.container.as_array()).push_back(std::move(child));
        }
    };
    script_value leaf(std::monostate{}, eng);
    if (open(idx, leaf)) { return leaf; }
    for (;;) {
        frame& top = frames.back();
        const JNode& node = nodes_[top.node];
        if (top.next < node.u.agg.count) {
            const uint32_t child = node.tag == JTag::Obj ? node.u.agg.first + 2 * top.next + 1 : node.u.agg.first + top.next;
            ++top.next;
            if (open(child, leaf)) { attach(frames.back(), std::move(leaf)); }
            continue;
        }
        script_value done = std::move(top.container);
        frames.pop_back();
        if (frames.empty()) { return done; }
        attach(frames.back(), std::move(done));
    }
}

void json_archive_reader::parse_into_arena() {
    pos_ = 0;
    nodes_.clear();
    chars_.clear();
    scratch_.clear();
    nodes_.reserve(json_.size() / 6 + 16);   // ~1 value / 6 bytes for minified JSON
    chars_.reserve(json_.size() / 2 + 16);
    parse_value_flat();
    skip_whitespace();
    if (pos_ != json_.length()) {
        // Trailing bytes after the root value -> malformed (two top-level values, etc.).
        throw serialization_error(
            "Unexpected trailing characters after JSON value at position " + std::to_string(pos_));
    }
    if (scratch_.size() != 1) {
        throw serialization_error("Invalid JSON document");
    }
    nodes_.push_back(scratch_[0]);   // the root value becomes the last arena element
    root_ = static_cast<uint32_t>(nodes_.size() - 1);
    scratch_.clear();
}

void json_archive_reader::push_container(JTag tag, size_t mark) {
    const uint32_t first = static_cast<uint32_t>(nodes_.size());
    const size_t cnt = scratch_.size() - mark;
    nodes_.insert(nodes_.end(), scratch_.begin() + mark, scratch_.end());
    scratch_.resize(mark);
    JNode v;
    v.tag = tag;
    v.u.agg.first = first;
    v.u.agg.count = (tag == JTag::Obj) ? static_cast<uint32_t>(cnt / 2) : static_cast<uint32_t>(cnt);
    scratch_.push_back(v);
}

void json_archive_reader::parse_value_flat() {
    struct open_container { JTag tag; size_t mark; };
    std::vector<open_container> open;
    for (;;) {
        skip_whitespace();
        if (pos_ >= json_.length()) {
            throw serialization_error("Unexpected end of JSON");
        }
        char c = json_[pos_];
        bool completed = true;
        if (c == '"') {
            JNode v; v.tag = JTag::Str;
            parse_string_into(v.u.str.off, v.u.str.len);
            scratch_.push_back(v);
        } else if (c == '{' || c == '[') {
            const JTag tag = c == '{' ? JTag::Obj : JTag::Arr;
            pos_++;
            const size_t mark = scratch_.size();
            if (peek() == (c == '{' ? '}' : ']')) {
                advance();
                push_container(tag, mark);
            } else {
                open.push_back({tag, mark});
                if (tag == JTag::Obj) { parse_member_key(); }
                completed = false;
            }
        } else if (c == 't' || c == 'f') {
            parse_bool_flat();
        } else if (c == 'n') {
            parse_null_flat();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            parse_number_flat();
        } else {
            size_t start = (pos_ > 20) ? pos_ - 20 : 0;
            size_t len = std::min(size_t(40), json_.length() - start);
            std::string context(json_.substr(start, len));
            throw serialization_error("Unexpected character '" + std::string(1, c) +
                "' in JSON at position " + std::to_string(pos_) +
                ", near: ..." + context + "...");
        }
        if (!completed) { continue; }
        // a value finished: close every container it finished, until one wants its next member
        for (;;) {
            if (open.empty()) { return; }
            const open_container top = open.back();
            const char next = peek();
            if (next == ',') {
                advance();
                if (top.tag == JTag::Obj) { parse_member_key(); }
                break;
            }
            if (next == (top.tag == JTag::Obj ? '}' : ']')) {
                advance();
                open.pop_back();
                push_container(top.tag, top.mark);
                continue;
            }
            size_t start = (pos_ > 20) ? pos_ - 20 : 0;
            size_t len = std::min(size_t(40), json_.length() - start);
            std::string context(json_.substr(start, len));
            throw serialization_error(std::string(top.tag == JTag::Obj ? "Expected ',' or '}' in JSON object at position " : "Expected ',' or ']' in JSON array at position ") +
                std::to_string(pos_) + ", got '" + std::string(1, next) + "', near: ..." + context + "...");
        }
    }
}

void json_archive_reader::parse_member_key() {
    JNode k; k.tag = JTag::Str;
    parse_string_into(k.u.str.off, k.u.str.len);
    if (k.u.str.len != 0 && chars_[k.u.str.off] == '$') { archive_keys_ = true; }
    const uint32_t slot = intern_key(k.u.str.off, k.u.str.len);
    k.u.str.off = key_spans_[slot].first;
    k.key = slot + 1;
    scratch_.push_back(k);
    expect(':');
}

void json_archive_reader::skip_whitespace() {
    // JSON whitespace is exactly these four bytes (RFC 8259). Comparing directly
    // avoids std::isspace, which is a locale-aware function call AND is undefined
    // behavior when handed a raw (sign-extended) char >= 0x80.
    while (pos_ < json_.length()) {
        char c = json_[pos_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos_++;
        else break;
    }
}

char json_archive_reader::peek() {
    skip_whitespace();
    return pos_ < json_.length() ? json_[pos_] : '\0';
}

char json_archive_reader::advance() {
    skip_whitespace();
    return pos_ < json_.length() ? json_[pos_++] : '\0';
}

void json_archive_reader::expect(char c) {
    if (advance() != c) {
        throw serialization_error("Expected '" + std::string(1, c) + "' in JSON");
    }
}

void json_archive_reader::append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool json_archive_reader::read_hex4(uint32_t& out) {
    if (pos_ + 4 >= json_.length()) return false;
    uint32_t v = 0;
    for (size_t k = 1; k <= 4; ++k) {
        char c = json_[pos_ + k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
        else return false;
    }
    pos_ += 4;
    out = v;
    return true;
}

void json_archive_reader::parse_string_into(uint32_t& off, uint32_t& len) {
    expect('"');
    const uint32_t start = static_cast<uint32_t>(chars_.size());
    const size_t n = json_.length();

    while (pos_ < n) {
        const size_t runStart = pos_;
        while (pos_ < n) {
            char c = json_[pos_];
            if (c == '"' || c == '\\') break;
            pos_++;
        }
        if (pos_ > runStart) chars_.append(json_, runStart, pos_ - runStart);

        if (pos_ >= n) break;                 // unterminated -> expect('"') below throws
        if (json_[pos_] == '"') {
            pos_++;
            off = start;
            len = static_cast<uint32_t>(chars_.size() - start);
            return;
        }

        pos_++;                               // consume backslash
        if (pos_ >= n) throw serialization_error("Unexpected end of JSON string");
        switch (json_[pos_]) {
            case '"':  chars_.push_back('"');  break;
            case '\\': chars_.push_back('\\'); break;
            case '/':  chars_.push_back('/');  break;
            case 'b':  chars_.push_back('\b'); break;
            case 'f':  chars_.push_back('\f'); break;
            case 'n':  chars_.push_back('\n'); break;
            case 'r':  chars_.push_back('\r'); break;
            case 't':  chars_.push_back('\t'); break;
            case 'u': {
                uint32_t cp;
                if (!read_hex4(cp)) throw serialization_error("Invalid \\u escape in JSON string");
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (pos_ + 2 < n && json_[pos_ + 1] == '\\' && json_[pos_ + 2] == 'u') {
                        const size_t save = pos_;
                        pos_ += 2;
                        uint32_t low;
                        if (read_hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos_ = save;
                            cp = 0xFFFD;
                        }
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                append_utf8(chars_, cp);
                break;
            }
            default:
                throw serialization_error("Invalid escape sequence");
        }
        pos_++;                               // step past the escape's final char
    }

    expect('"');                              // unterminated string -> throws
}

void json_archive_reader::parse_bool_flat() {
    JNode v; v.tag = JTag::Bool;
    if (json_.compare(pos_, 4, "true") == 0) { pos_ += 4; v.u.i = 1; }
    else if (json_.compare(pos_, 5, "false") == 0) { pos_ += 5; v.u.i = 0; }
    else throw serialization_error("Invalid boolean value");
    scratch_.push_back(v);
}

void json_archive_reader::parse_null_flat() {
    if (json_.compare(pos_, 4, "null") == 0) {
        pos_ += 4;
        JNode v; v.tag = JTag::Null; v.u.i = 0;
        scratch_.push_back(v);
    } else {
        throw serialization_error("Invalid null value");
    }
}

auto json_archive_reader::from_chars_double(const char* first, const char* last, double& d) -> double_parse_result {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    auto r = std::from_chars(first, last, d);
    return { r.ptr, r.ec };
#else
    std::string text(first, last);
    const char locale_decimal = std::localeconv()->decimal_point[0];
    if (locale_decimal != '.') {
        for (auto& c : text) { if (c == '.') c = locale_decimal; }
    }
    const auto parsed = detail::parse_float_text(text.c_str());
    if (parsed.end == text.c_str()) { return { first, std::errc::invalid_argument }; }
    d = parsed.value;
    return { first + (parsed.end - text.c_str()), parsed.out_of_range ? std::errc::result_out_of_range : std::errc() };
#endif
}

void json_archive_reader::parse_number_flat() {
    const size_t start = pos_;
    bool has_decimal = false;
    bool has_exponent = false;
    if (pos_ < json_.length() && json_[pos_] == '-') pos_++;
    while (pos_ < json_.length()) {
        char c = json_[pos_];
        if (c >= '0' && c <= '9') { pos_++; }
        else if (c == '.') { has_decimal = true; pos_++; }
        else if (c == 'e' || c == 'E') { has_exponent = true; pos_++; }
        else if (c == '+' || c == '-') { pos_++; }   // exponent sign
        else break;
    }
    const char* first = json_.data() + start;
    const char* last = json_.data() + pos_;
    // from_chars leaves the out-of-range result implementation-defined (MS STL: +-inf,
    // libstdc++: untouched). Pin the writer's 1e999 infinity sentinel on every platform:
    // a negative exponent underflows to signed zero, anything else overflows to signed inf.
    auto out_of_range_double = [](const char* f, const char* l) {
        bool negative = (f != l && *f == '-');
        bool negative_exponent = false;
        for (const char* p = f; p != l; ++p) {
            if (*p == 'e' || *p == 'E') { negative_exponent = (p + 1 != l && *(p + 1) == '-'); break; }
        }
        double magnitude = negative_exponent ? 0.0 : std::numeric_limits<double>::infinity();
        return negative ? -magnitude : magnitude;
    };
    JNode v;
    if (has_decimal || has_exponent) {
        double d = 0.0;
        auto r = from_chars_double(first, last, d);
        if (r.ptr != last || (r.ec != std::errc() && r.ec != std::errc::result_out_of_range)) {
            throw serialization_error("Invalid numeric literal in JSON at position " + std::to_string(start));
        }
        if (r.ec == std::errc::result_out_of_range) { d = out_of_range_double(first, last); }
        v.tag = JTag::Double; v.u.d = d;
    } else {
        script_int iv = 0;
        auto r = std::from_chars(first, last, iv);
        uint64_t uv = 0;
        if (r.ptr == last && r.ec == std::errc()) {
            v.tag = JTag::Int; v.u.i = iv;
        } else if (r.ec == std::errc::result_out_of_range && *first != '-' && std::from_chars(first, last, uv).ec == std::errc()) {
            v.tag = JTag::UInt; v.u.u = uv;
        } else {
            double d = 0.0;
            auto r2 = from_chars_double(first, last, d);
            if (r2.ptr != last || (r2.ec != std::errc() && r2.ec != std::errc::result_out_of_range)) {
                throw serialization_error("Invalid integer literal in JSON at position " + std::to_string(start));
            }
            if (r2.ec == std::errc::result_out_of_range) { d = out_of_range_double(first, last); }
            v.tag = JTag::Double; v.u.d = d;
        }
    }
    scratch_.push_back(v);
}

std::unordered_map<std::string_view, uint32_t>& json_archive_reader::object_index(ObjState& s) {
    if (!s.index) {
        s.index = std::make_unique<std::unordered_map<std::string_view, uint32_t>>();
        s.index->reserve(s.count);
        for (uint32_t m = 0; m < s.count; ++m) {
            const JNode& k = nodes_[s.first + 2 * m];
            s.index->emplace(std::string_view(chars_.data() + k.u.str.off, k.u.str.len),
                             s.first + 2 * m + 1);
        }
    }
    return *s.index;
}

} // namespace jai::serialization
