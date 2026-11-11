#include <jaiscript/serialization/binary_archive.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/math_values.hpp>

namespace jai::serialization {

binary_archive_writer::binary_archive_writer() : archive_writer_impl<binary_archive_writer>() {}

binary_archive_writer::binary_archive_writer(engine* eng) : archive_writer_impl<binary_archive_writer>(eng) {}

binary_archive_writer::~binary_archive_writer() = default;

binary_archive_reader::~binary_archive_reader() = default;

binary_archive_writer::binary_archive_writer(std::vector<uint8_t>& buffer) : archive_writer_impl<binary_archive_writer>(), buffer_(&buffer) {
    buffer_->clear();
}

binary_archive_writer::binary_archive_writer(std::vector<uint8_t>& buffer, engine* eng) : archive_writer_impl<binary_archive_writer>(eng), buffer_(&buffer) {
    buffer_->clear();
}

const std::vector<uint8_t>& binary_archive_writer::data() const { return owned_buffer_.empty() ? *buffer_ : owned_buffer_; }

std::vector<uint8_t>& binary_archive_writer::data() { return owned_buffer_.empty() ? *buffer_ : owned_buffer_; }

void binary_archive_writer::write_int8(int8_t value) {
    write_raw(&value, sizeof(value));
}

void binary_archive_writer::write_int16(int16_t value) {
    write_little_endian(value);
}

void binary_archive_writer::write_int32(int32_t value) {
    write_little_endian(value);
}

void binary_archive_writer::write_int64(int64_t value) {
    write_little_endian(value);
}

void binary_archive_writer::write_uint8(uint8_t value) {
    write_raw(&value, sizeof(value));
}

void binary_archive_writer::write_uint16(uint16_t value) {
    write_little_endian(value);
}

void binary_archive_writer::write_uint32(uint32_t value) {
    write_little_endian(value);
}

void binary_archive_writer::write_uint64(uint64_t value) {
    write_little_endian(value);
}

void binary_archive_writer::write_float32(float value) {
    static_assert(sizeof(float) == 4, "float must be 32-bit");
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    write_uint32(bits);
}

void binary_archive_writer::write_float64(double value) {
    static_assert(sizeof(double) == 8, "double must be 64-bit");
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    write_uint64(bits);
}

void binary_archive_writer::write_bool(bool value) {
    write_uint8(value ? 1 : 0);
}

void binary_archive_writer::write_string(const std::string& value) {
    write_uint32(static_cast<uint32_t>(value.size()));
    if (!value.empty()) {
        write_raw(value.data(), value.size());
    }
}

void binary_archive_writer::write_null() {
    write_uint8(0x00); // Null marker
}

void binary_archive_writer::write_binary(const void* data, size_t size) {
    write_uint32(static_cast<uint32_t>(size));
    if (size > 0) {
        write_raw(data, size);
    }
}

void binary_archive_writer::begin_object() {
    enter_host_record(object_write_stack_.size());
    write_uint8(0x01); // Object marker
    write_string("");  // Empty type name
    write_uint32(0);   // Version 0
    object_write_stack_.emplace_back();
}

void binary_archive_writer::begin_object(const std::string& type_name, uint32_t version) {
    enter_host_record(object_write_stack_.size());
    write_uint8(0x01); // Object marker
    write_string(type_name);
    write_uint32(version);
    set_version(version);

    object_write_stack_.emplace_back();
}

void binary_archive_writer::end_object() {
    if (!object_write_stack_.empty()) {
        // Save and pop state first so writes go to parent's buffer (or main buffer)
        auto state = std::move(object_write_stack_.back());
        object_write_stack_.pop_back();

        write_uint32(static_cast<uint32_t>(state.properties.size()));

        for (const auto& prop : state.properties) {
            write_string(prop.name);
        }

        for (const auto& prop : state.properties) {
            write_uint32(static_cast<uint32_t>(prop.data.size()));
        }

        for (const auto& prop : state.properties) {
            if (!prop.data.empty()) {
                write_raw(prop.data.data(), prop.data.size());
            }
        }
    }

    write_uint8(0x02); // End object marker
}

void binary_archive_writer::write_property_name(const std::string& name) {
    if (!object_write_stack_.empty()) {
        object_write_stack_.back().properties.push_back({name, {}});
        object_write_stack_.back().current_property_buffer = &object_write_stack_.back().properties.back().data;
    } else {
        write_string(name);
    }
}

void binary_archive_writer::begin_array(size_t size) {
    write_uint8(0x03); // Array marker
    write_uint32(static_cast<uint32_t>(size));
}

void binary_archive_writer::end_array() {
    write_uint8(0x04); // End array marker
}

void binary_archive_writer::begin_map(size_t size) {
    write_uint8(0x05); // Map marker
    write_uint32(static_cast<uint32_t>(size));
}

void binary_archive_writer::end_map() {
    write_uint8(0x06); // End map marker
}

void binary_archive_writer::write_map_key(const std::string& key) {
    write_string(key);
}

void binary_archive_writer::write_value(const script_value& root) {
    std::vector<value_frame> frames;
    if (!begin_value(root, frames)) { return; }
    script_value scratch(std::monostate{}, engine_ref_);
    while (!frames.empty()) {
        const script_value* child = next_child(frames.back(), scratch);
        if (!child) {
            frames.pop_back();
            continue;
        }
        if (!begin_value(*child, frames)) { return; }   // a getter threw into the backend: the in-flight exception leaves the archive
    }
}

const script_value* binary_archive_writer::next_child(value_frame& f, script_value& scratch) {
    switch (f.shape) {
        case frame_shape::values:
            if (f.index >= f.values.size()) { return nullptr; }
            return &f.values[f.index++];
        case frame_shape::array: {
            if (f.index >= f.count) { return nullptr; }
            const script_array* node = f.source.unchecked_array_node();
            const size_t i = f.index++;
            if (node->kind() == script_array::kind_t::hetero) { return &node->values()[i]; }
            scratch = node->get(i, node->owner);
            return &scratch;
        }
        case frame_shape::map: {
            if (f.next == f.source.as_map().end()) { return nullptr; }
            if (!f.value_next) {
                f.value_next = true;
                return &f.next->first;
            }
            const script_value* child = &f.next->second;
            f.value_next = false;
            ++f.next;
            return child;
        }
    }
    return nullptr;
}

bool binary_archive_writer::begin_value(const script_value& given, std::vector<value_frame>& frames) {
    const script_value& value = given.is_reference() ? given.deref() : given;

    // a handle to no object is null, as in JSON
    script_value_type tag = value.type();
    if ((tag == script_value_type::jai_weak_ptr_type && !value.get_weak_ptr().lock()) ||
        (tag == script_value_type::jai_shared_ptr_type && !value.get_object_holder())) {
        tag = script_value_type::jai_null_type;
    }
    // For script classes, type_info->type_name may be "any" - use the actual class name
    strong_ptr<class_instance> instance;
    std::shared_ptr<class_definition> bound;
    class_definition* class_def = nullptr;
    if (tag == script_value_type::jai_object_type) {
        try {
            instance = value.as<strong_ptr<class_instance>>();
        } catch (...) {
            // not a class_instance: a C++ object's bound class, else an empty object under its type's name
        }
        bound = instance ? nullptr : bound_class_of(value);
        class_def = instance ? instance->get_class_definition() : bound.get();
        if (class_def && class_def->archive_form()) {
            return begin_value(class_def->archive_form()(value, *class_def, engine_ref_), frames);
        }
    }
    write_uint8(static_cast<uint8_t>(tag));

    switch (tag) {
        case script_value_type::jai_null_type:
            // Nothing to write
            break;

        case script_value_type::jai_bool_type:
            write_bool(value.as<script_bool>());
            break;

        case script_value_type::jai_int_type:
            write_int64(value.as<script_int>());
            break;

        case script_value_type::jai_float_type:
            write_float64(value.as<script_float>());
            break;

        case script_value_type::jai_string_type:
            write_string(value.as<script_string>());
            break;

        case script_value_type::jai_utf8_string_type:
            write_string(value.deref().unchecked_as_utf8().bytes);
            break;

        case script_value_type::jai_vec2_type:
        case script_value_type::jai_vec3_type:
        case script_value_type::jai_vec4_type:
        case script_value_type::jai_color_type:
        case script_value_type::jai_ivec2_type:
        case script_value_type::jai_ivec3_type: {
            // the components' raw little-endian bits, binary32 or int32 (exact round trip)
            const script_value& v = value.deref();
            const script_value_type kind = v.unchecked_vec_kind();
            for (size_t k = 0; k < detail::math::dims(kind); ++k) {
                uint32_t bits = 0;
                if (detail::math::integral(kind)) {
                    bits = static_cast<uint32_t>(detail::math::i32_at(v, k));
                } else {
                    const float c = detail::math::f32_at(v, k);
                    std::memcpy(&bits, &c, sizeof(bits));
                }
                write_uint32(bits);
            }
            break;
        }

        case script_value_type::jai_char_type:
            write_uint8(static_cast<uint8_t>(value.as<script_char>()));
            break;

        case script_value_type::jai_array_type: {
            const script_array* node = value.unchecked_array_node();
            if (!node) { throw runtime_error("script_value is not an array"); }
            write_uint32(static_cast<uint32_t>(node->size()));
            if (node->size() > 0) {
                value_frame& f = frames.emplace_back(frame_shape::array, value);
                f.count = node->size();
            }
            break;
        }

        case script_value_type::jai_map_type: {
            const auto& map = value.as_map();
            write_uint32(static_cast<uint32_t>(map.size()));
            if (map.size() > 0) {
                value_frame& f = frames.emplace_back(frame_shape::map, value);
                f.next = map.begin();
            }
            break;
        }

        case script_value_type::jai_weak_ptr_type: {
            // the object rides with the archive's first reference to it, weak or shared
            auto shared = value.get_weak_ptr().lock();
            auto [id, is_new] = track_handle(shared.get());
            write_uint32(id);
            if (is_new) {
                frames.emplace_back(frame_shape::values, script_value(std::monostate{}, engine_ref_)).values.push_back(weak_target_value(shared));
            }
            break;
        }

        case script_value_type::jai_shared_ptr_type: {
            // A shared handle: the object once under its id, every later handle the id alone (identity survives the load)
            auto holder = value.get_object_holder();
            auto [id, is_new] = track_handle(holder.get());
            write_uint32(id);
            if (is_new) {
                frames.emplace_back(frame_shape::values, script_value(std::monostate{}, engine_ref_)).values.push_back(weak_target_value(holder));
            }
            break;
        }

        case script_value_type::jai_object_type: {
            // Self-describing binary format: type name + version + property names array + values in order
            auto type_info = value.get_type_info();
            std::string type_name = type_info ? type_info->type_name : "unknown";
            if (class_def) {
                type_name = class_def->get_name();
            }

            // Write type name first
            write_string(type_name);

            // Look up version from serialization metadata
            uint32_t version = 1;  // Default version
            if (auto eng = engine_ref_) {
                const auto* metadata = detail::engine_access::get_serialization_registry(*eng).get_class_metadata(type_name);
                if (metadata) {
                    version = metadata->current_version;
                } else if (!detail::engine_access::get_class_definition(*eng, type_name)) {
                    // Runtime validation: script classes resolve through the engine's
                    // class registry (default version 1); anything else is unloadable
                    throw serialization_error(
                        "Cannot serialize unregistered type '" + type_name + "'. " +
                        "Register the type with dynamic_binder before serialization."
                    );
                }
            }
            write_uint32(version);

            // Collect properties and values
            std::vector<std::string> property_names;
            std::vector<script_value> property_values;
            if (class_def) {
                auto eng = class_def->get_engine();
                if (eng && !for_each_serialized_property(value, class_def, eng, [&](const auto& prop_name, script_value&& prop_value) {
                        property_names.push_back(std::string(prop_name));
                        property_values.push_back(std::move(prop_value));
                    })) {
                    return false;
                }
            }

            // Write property count
            write_uint32(static_cast<uint32_t>(property_names.size()));

            // Write property names array
            for (const auto& prop_name : property_names) {
                write_string(prop_name);
            }

            // The values follow in the same order
            if (!property_values.empty()) {
                frames.emplace_back(frame_shape::values, script_value(std::monostate{}, engine_ref_)).values = std::move(property_values);
            }
            break;
        }

        default:
            throw serialization_error("Unsupported type for binary serialization: " + std::to_string(static_cast<int>(value.type())));
    }
    return true;
}

std::vector<uint8_t>& binary_archive_writer::get_buffer() {
    if (buffer_) return *buffer_;
    return owned_buffer_;
}

void binary_archive_writer::write_raw(const void* data, size_t size) {
    if (!object_write_stack_.empty() && object_write_stack_.back().current_property_buffer) {
        auto* prop_buf = object_write_stack_.back().current_property_buffer;
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        prop_buf->insert(prop_buf->end(), bytes, bytes + size);
    } else {
        auto& buf = get_buffer();
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        buf.insert(buf.end(), bytes, bytes + size);
    }
}
binary_archive_reader::binary_archive_reader(const std::vector<uint8_t>& data, engine* eng) : archive_reader_impl<binary_archive_reader>(eng), data_(data), pos_(0) {
    if (!eng) {
        throw serialization_error("binary_archive_reader requires a valid engine reference");
    }
}

binary_archive_reader::binary_archive_reader(const void* data, size_t size, engine* eng) : archive_reader_impl<binary_archive_reader>(eng), data_(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size), pos_(0) {
    if (!eng) {
        throw serialization_error("binary_archive_reader requires a valid engine reference");
    }
}

int8_t binary_archive_reader::read_int8() {
    int8_t value;
    read_raw(&value, sizeof(value));
    return value;
}

int16_t binary_archive_reader::read_int16() {
    return read_little_endian<int16_t>();
}

int32_t binary_archive_reader::read_int32() {
    return read_little_endian<int32_t>();
}

int64_t binary_archive_reader::read_int64() {
    return read_little_endian<int64_t>();
}

uint8_t binary_archive_reader::read_uint8() {
    uint8_t value;
    read_raw(&value, sizeof(value));
    return value;
}

uint16_t binary_archive_reader::read_uint16() {
    return read_little_endian<uint16_t>();
}

uint32_t binary_archive_reader::read_uint32() {
    return read_little_endian<uint32_t>();
}

uint64_t binary_archive_reader::read_uint64() {
    return read_little_endian<uint64_t>();
}

float binary_archive_reader::read_float32() {
    uint32_t bits = read_little_endian<uint32_t>();
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double binary_archive_reader::read_float64() {
    uint64_t bits = read_little_endian<uint64_t>();
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool binary_archive_reader::read_bool() {
    return read_uint8_raw() != 0;
}

std::string binary_archive_reader::read_string() {
    return read_string_raw();
}

bool binary_archive_reader::peek_null() {
    if (pos_ >= data_.size()) return false;
    return data_[pos_] == 0x00;  // Null marker
}

void binary_archive_reader::read_null() {
    uint8_t marker = read_uint8_raw();
    if (marker != 0x00) {
        throw serialization_error("Expected null marker (0x00), got " + std::to_string(marker));
    }
}

std::vector<uint8_t> binary_archive_reader::read_binary(size_t /*expected_size*/) {
    uint32_t size = read_little_endian<uint32_t>();
    // Validate against remaining data BEFORE allocating: a hostile/corrupt size
    // (e.g. 0xFFFFFFFF) would otherwise allocate ~4 GB before read_raw fails.
    if (size > remaining_bytes()) {
        throw serialization_error("Binary archive blob size exceeds remaining data");
    }
    std::vector<uint8_t> result(size);
    if (size > 0) {
        read_raw(result.data(), size);
    }
    return result;
}

void binary_archive_reader::begin_object() {
    std::string type_name;
    uint32_t version = 0;
    begin_object(type_name, version);
}

void binary_archive_reader::expect_marker(uint8_t marker, const char* expected) {
    if (pos_ >= data_.size()) { throw_archive_expected(expected, "the end of the data"); }
    const uint8_t found = data_[pos_];
    if (found != marker) { throw_archive_expected(expected, "marker " + std::to_string(found)); }
    ++pos_;
}

void binary_archive_reader::begin_object(std::string& type_name, uint32_t& version) {
    enter_host_record(object_stack_.size());
    expect_marker(0x01, "an object");

    type_name = read_string_raw();
    version = read_little_endian<uint32_t>();
    version_ = version;

    uint32_t prop_count = read_little_endian<uint32_t>();
    // Each property contributes at minimum a 4-byte name-length prefix and a 4-byte
    // size entry, so a valid prop_count can't exceed remaining/8. Reject a corrupt or
    // hostile count before the reserve() calls below (reserve(~4 billion) would OOM).
    if (prop_count > remaining_bytes() / 8) {
        throw serialization_error("Binary archive object property count exceeds remaining data");
    }

    // Pre-read all properties for out-of-order access
    object_stack_.emplace_back();
    auto& state = object_stack_.back();

    state.property_order.reserve(prop_count);
    for (uint32_t i = 0; i < prop_count; ++i) {
        state.property_order.push_back(read_string_raw());
    }

    std::vector<uint32_t> sizes;
    sizes.reserve(prop_count);
    for (uint32_t i = 0; i < prop_count; ++i) {
        sizes.push_back(read_little_endian<uint32_t>());
    }

    size_t values_base = pos_;
    size_t offset = 0;
    state.property_locations.reserve(prop_count);
    for (uint32_t i = 0; i < prop_count; ++i) {
        state.property_locations.push_back({values_base + offset, sizes[i]});
        state.property_name_to_index[state.property_order[i]] = i;
        offset += sizes[i];
    }

    state.end_object_pos = values_base + offset;
}

void binary_archive_reader::end_object() {
    if (!object_stack_.empty()) {
        pos_ = object_stack_.back().end_object_pos;
        object_stack_.pop_back();
    }
    uint8_t marker = read_uint8_raw();
    if (marker != 0x02) {
        throw runtime_error("Expected end object marker, got " + std::to_string(marker));
    }
}

size_t binary_archive_reader::begin_array() {
    expect_marker(0x03, "an array");
    uint32_t size = read_little_endian<uint32_t>();
    // A length prefix can't exceed the bytes left (each element costs >=1 byte); reject an
    // oversized count instead of reserving/looping into a multi-GB allocation from a tiny
    // malicious blob (mirrors read_binary). B5.
    if (size > remaining_bytes()) {
        throw serialization_error("Array size exceeds remaining data");
    }
    return size;
}

void binary_archive_reader::end_array() {
    uint8_t marker = read_uint8_raw();
    if (marker != 0x04) {
        throw runtime_error("Expected end array marker, got " + std::to_string(marker));
    }
}

bool binary_archive_reader::read_property_name(std::string& name) {
    if (pos_ >= data_.size()) return false;
    name = read_string();
    return true;
}

size_t binary_archive_reader::begin_map() {
    expect_marker(0x05, "a map");

    uint32_t size = read_little_endian<uint32_t>();
    // See begin_array: a map of `size` entries needs at least `size` more bytes.
    if (size > remaining_bytes()) {
        throw serialization_error("Map size exceeds remaining data");
    }
    return size;
}

void binary_archive_reader::end_map() {
    uint8_t marker = read_uint8_raw();
    if (marker != 0x06) {
        throw serialization_error("Expected end map marker (0x06)");
    }
}

bool binary_archive_reader::read_map_key(std::string& key) {
    if (pos_ >= data_.size()) return false;
    key = read_string();
    return true;
}

bool binary_archive_reader::has_property(const std::string& name) {
    if (!object_stack_.empty()) {
        return object_stack_.back().property_name_to_index.find(name) != object_stack_.back().property_name_to_index.end();
    }
    return false;
}

const std::vector<std::string>& binary_archive_reader::get_object_property_names() const {
    if (object_stack_.empty()) {
        static const std::vector<std::string> empty;
        return empty;
    }
    return object_stack_.back().property_order;
}

bool binary_archive_reader::seek_property_by_index(size_t index) {
    seek_size_ = 0;

    if (object_stack_.empty()) return false;
    auto& state = object_stack_.back();

    if (index >= state.property_locations.size()) return false;

    const auto& loc = state.property_locations[index];
    pos_ = loc.offset;
    seek_size_ = loc.size;
    state.next_sequential_index = index + 1;
    return true;
}

bool binary_archive_reader::seek_property_by_index(size_t index, const std::string& expected_name) {
    seek_size_ = 0;

    if (object_stack_.empty()) return false;
    auto& state = object_stack_.back();

    if (index >= state.property_locations.size()) return false;

    if (state.property_order[index] != expected_name) return false;

    const auto& loc = state.property_locations[index];
    pos_ = loc.offset;
    seek_size_ = loc.size;
    state.next_sequential_index = index + 1;
    return true;
}

bool binary_archive_reader::seek_property(const std::string& name) {
    seek_size_ = 0;

    if (object_stack_.empty()) {
        throw serialization_error("Binary seek_property called outside object context for '" + name + "'");
    }

    auto& state = object_stack_.back();

    if (state.next_sequential_index < state.property_order.size()) {
        const std::string& next_prop = state.property_order[state.next_sequential_index];
        if (next_prop == name) {
            const auto& loc = state.property_locations[state.next_sequential_index];
            pos_ = loc.offset;
            seek_size_ = loc.size;
            state.next_sequential_index++;
            return true;
        }
    }

    auto it = state.property_name_to_index.find(name);
    if (it == state.property_name_to_index.end()) {
        // Property not found - this is OK for versioning (new property in newer code)
        return false;
    }

    const auto& loc = state.property_locations[it->second];
    pos_ = loc.offset;
    seek_size_ = loc.size;
    return true;
}

script_value binary_archive_reader::read_value() {
    return read_value_internal();
}

void binary_archive_reader::read_raw(void* dest, size_t size) {
    if (pos_ + size > data_.size()) {
        throw runtime_error("Binary archive read past end of data");
    }
    std::memcpy(dest, data_.data() + pos_, size);
    pos_ += size;
}

uint8_t binary_archive_reader::read_uint8_raw() {
    uint8_t value;
    read_raw(&value, sizeof(value));
    return value;
}

size_t binary_archive_reader::remaining_bytes() const { return data_.size() > pos_ ? data_.size() - pos_ : 0; }

std::string binary_archive_reader::read_string_raw() {
    uint32_t size = read_little_endian<uint32_t>();
    if (size == 0) return "";

    if (size > remaining_bytes()) {
        throw runtime_error("Binary archive string size exceeds remaining data");
    }
    std::string result(size, '\0');
    read_raw(result.data(), size);
    return result;
}

script_value binary_archive_reader::read_value_internal() {
    auto eng = engine_ref_;
    if (!eng) {
        throw serialization_error("Engine reference expired during deserialization");
    }
    std::vector<value_frame> frames;
    for (;;) {
        script_value value(std::monostate{}, eng);
        if (!read_or_open(eng, frames, value)) { continue; }
        for (;;) {
            if (frames.empty()) { return value; }
            value_frame& f = frames.back();
            take_child(eng, f, std::move(value));
            if (f.remaining > 0) { break; }
            value = finish_frame(eng, f);
            frames.pop_back();
        }
    }
}

bool binary_archive_reader::read_or_open(engine* eng, std::vector<value_frame>& frames, script_value& out) {
    uint8_t type_tag = read_uint8_raw();
    script_value_type vtype = static_cast<script_value_type>(type_tag);

    switch (vtype) {
        case script_value_type::jai_null_type:
            out = script_value(std::monostate{}, eng);
            return true;

        case script_value_type::jai_bool_type:
            out = script_value(read_uint8_raw() != 0, eng);
            return true;

        case script_value_type::jai_int_type:
            out = script_value(read_little_endian<int64_t>(), eng);
            return true;

        case script_value_type::jai_float_type: {
            uint64_t bits = read_little_endian<uint64_t>();
            double value;
            std::memcpy(&value, &bits, sizeof(value));
            out = script_value(value, eng);
            return true;
        }

        case script_value_type::jai_string_type:
            out = script_value(read_string_raw(), eng);
            return true;

        case script_value_type::jai_utf8_string_type:
            out = script_value::make_utf8(read_string_raw(), eng);
            return true;

        case script_value_type::jai_vec2_type:
        case script_value_type::jai_vec3_type:
        case script_value_type::jai_vec4_type:
        case script_value_type::jai_color_type:
        case script_value_type::jai_ivec2_type:
        case script_value_type::jai_ivec3_type: {
            const script_value_type kind = vtype;
            float f[4] = {0, 0, 0, 0};
            int32_t i[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < detail::math::dims(kind); ++k) {
                const uint32_t bits = read_little_endian<uint32_t>();
                if (detail::math::integral(kind)) { i[k] = static_cast<int32_t>(bits); }
                else { std::memcpy(&f[k], &bits, sizeof(bits)); }
            }
            out = detail::math::make(kind, f, i, eng);
            return true;
        }

        case script_value_type::jai_char_type:
            out = script_value(static_cast<script_char>(read_uint8_raw()), eng);
            return true;

        case script_value_type::jai_array_type: {
            uint32_t size = read_little_endian<uint32_t>();
            if (size > remaining_bytes()) {
                throw runtime_error("Binary archive array size exceeds remaining data");
            }
            script_value array_val = script_value::make_array(nullptr, eng);
            if (size == 0) {
                out = std::move(array_val);
                return true;
            }
            const_cast<std::vector<script_value>&>(array_val.as_array()).reserve(size);
            frames.emplace_back(frame_shape::array, size, std::move(array_val), eng);
            return false;
        }

        case script_value_type::jai_map_type: {
            uint32_t size = read_little_endian<uint32_t>();
            if (size > remaining_bytes()) {
                throw runtime_error("Binary archive map size exceeds remaining data");
            }
            script_value map_val = script_value::make_map(detail::engine_access::get_type_info_string(*eng), nullptr, eng);
            if (size == 0) {
                out = std::move(map_val);
                return true;
            }
            frames.emplace_back(frame_shape::map, uint64_t(size) * 2, std::move(map_val), eng);
            return false;
        }

        case script_value_type::jai_weak_ptr_type:
        case script_value_type::jai_shared_ptr_type: {
            // A script handle reads back in from_json's shape, {"$id": N} or {"$ref": N} with its object under "$val" at the
            // id's first appearance, since the writer and the reader walk one order
            const bool weak = vtype == script_value_type::jai_weak_ptr_type;
            const uint32_t id = read_little_endian<uint32_t>();
            if (id == 0) {
                out = script_value(std::monostate{}, eng);
                return true;
            }
            script_value record = script_value::make_map(detail::engine_access::get_type_info_string(*eng), nullptr, eng);
            const_cast<script_map&>(record.as_map()).insert_or_assign(script_value(std::string(weak ? "$ref" : "$id"), eng),
                                                                      script_value(static_cast<script_int>(id), eng));
            if (!handle_ids_read_.insert(id).second) {
                out = std::move(record);
                return true;
            }
            frames.emplace_back(frame_shape::handle, 1, std::move(record), eng);
            return false;
        }

        case script_value_type::jai_object_type: {
            std::string type_name = read_string_raw();
            uint32_t version = read_little_endian<uint32_t>();
            uint32_t property_count = read_little_endian<uint32_t>();
            // Each property is at minimum a 4-byte name length + a 1-byte value tag,
            // so property_count can't exceed remaining/5. Guard the reserve()s below
            // against a corrupt/hostile count (reserve(~4 billion) would OOM).
            if (property_count > remaining_bytes() / 5) {
                throw serialization_error("Binary archive object property count exceeds remaining data");
            }
            value_frame f(frame_shape::object, property_count, script_value(std::monostate{}, eng), eng);
            f.type_name = std::move(type_name);
            f.version = version;
            f.names.reserve(property_count);
            for (uint32_t i = 0; i < property_count; ++i) {
                f.names.push_back(read_string_raw());
            }
            f.values.reserve(property_count);
            if (property_count == 0) {
                out = finish_frame(eng, f);
                return true;
            }
            frames.push_back(std::move(f));
            return false;
        }

        default:
            throw serialization_error("Unsupported type tag in binary deserialization: " +
                                    std::to_string(type_tag));
    }
}

void binary_archive_reader::take_child(engine* eng, value_frame& f, script_value&& child) {
    --f.remaining;
    switch (f.shape) {
        case frame_shape::array:
            const_cast<std::vector<script_value>&>(f.built.as_array()).push_back(std::move(child));
            break;
        case frame_shape::map:
            if (!f.have_key) {
                f.key = std::move(child);
                f.have_key = true;
            } else {
                const_cast<script_map&>(f.built.as_map()).insert_or_assign(std::move(f.key), std::move(child));
                f.key = script_value(std::monostate{}, eng);
                f.have_key = false;
            }
            break;
        case frame_shape::handle:
            const_cast<script_map&>(f.built.as_map()).insert_or_assign(script_value(std::string("$val"), eng), std::move(child));
            break;
        case frame_shape::object:
            f.values.push_back(std::move(child));
            break;
    }
}

script_value binary_archive_reader::finish_frame(engine* eng, value_frame& f) {
    if (f.shape != frame_shape::object) { return std::move(f.built); }
    const uint32_t property_count = static_cast<uint32_t>(f.names.size());
    const auto* metadata = detail::engine_access::get_serialization_registry(*eng).get_class_metadata(f.type_name);
    if (metadata && metadata->custom_construct) {
        any_archive_reader any_ar(*this);
        script_value constructed_obj = metadata->custom_construct(any_ar, f.version);

        auto instance = constructed_obj.as<strong_ptr<class_instance>>();
        if (instance) {
            auto class_def = instance->get_class_definition();
            if (class_def) {
                auto class_eng = class_def->get_engine();
                if (class_eng) {
                    for (uint32_t i = 0; i < property_count; ++i) {
                        const auto& prop_name = f.names[i];
                        const auto& prop_value = f.values[i];

                        uint64_t setter_id = detail::engine_access::symbolize(*class_eng, "_set_" + prop_name);
                        auto setter = class_def->get_method(setter_id);
                        if (setter.type() == script_value_type::jai_function_type) {
                            std::vector<script_value> args = {constructed_obj, prop_value};
                            auto stored = setter.as_function()(args);
                            if (!stored && (class_def->script_declares_user_accessor(setter_id) ||
                                            stored.error() == make_error_code(runtime_error_code::cpp_exception))) {
                                throw runtime_error(class_eng->format_error(stored));
                            }
                            if (detail::engine_access::backend_unwinding(*class_eng)) {
                                return constructed_obj;   // the setter threw: the in-flight exception leaves the load
                            }
                        }
                    }

                    uint64_t post_load_id = detail::engine_access::symbolize(*class_eng, "post_load");
                    auto post_load = class_def->get_method(post_load_id, false);
                    if (post_load.type() == script_value_type::jai_function_type) {
                        std::vector<script_value> args = {
                            constructed_obj,
                            script_value(static_cast<script_int>(f.version), class_eng)
                        };
                        auto loaded = post_load.as_function()(args);
                        if (!loaded) {
                            throw runtime_error(class_eng->format_error(loaded));
                        }
                    }
                }
            }
        }

        return constructed_obj;
    }

    // the object in from_json's shape, {"$type": T, "$val": {members}}
    script_value members = script_value::make_map(detail::engine_access::get_type_info_string(*eng), nullptr, eng);
    auto& member_map = const_cast<script_map&>(members.as_map());
    for (uint32_t i = 0; i < property_count; ++i) {
        member_map.insert_or_assign(script_value(f.names[i], eng), std::move(f.values[i]));
    }
    script_value record = script_value::make_map(detail::engine_access::get_type_info_string(*eng), nullptr, eng);
    auto& record_map = const_cast<script_map&>(record.as_map());
    record_map.insert_or_assign(script_value(std::string("$type"), eng), script_value(f.type_name, eng));
    record_map.insert_or_assign(script_value(std::string("$val"), eng), std::move(members));
    return record;
}

} // namespace jai::serialization
