#pragma once

#include "archive_impl.hpp"
#include <jaiscript/core/class_definition.hpp>
#include <iosfwd>
#include <memory>
#include <stack>
#include <string_view>

namespace jai {
namespace serialization {

class json_archive_writer : public archive_writer_impl<json_archive_writer> {
public:
    json_archive_writer(int indent = 2);
    json_archive_writer(int indent, engine* eng);
    explicit json_archive_writer(engine* eng);
    ~json_archive_writer();
    // A script's to_json: indentation counts against the engine's memory cap
    void charge_indentation() { charges_indentation_ = true; }

    static constexpr bool needs_property_keys = false;
    static constexpr bool is_text_format = true;

    void write_int8(int8_t value);
    void write_int16(int16_t value);
    void write_int32(int32_t value);
    void write_int64(int64_t value);
    void write_uint8(uint8_t value);
    void write_uint16(uint16_t value);
    void write_uint32(uint32_t value);
    void write_uint64(uint64_t value);
    void write_float32(float value);
    void write_float64(double value);
    void write_bool(bool value);

    void write_null();

    void write_string(const std::string& value);

    void write_binary(const void* data, size_t size);

    void begin_object();

    void begin_object(const std::string& type_name, uint32_t version);

    void end_object();

    void begin_array(size_t size);

    void end_array();

    void write_property_name(const std::string& name);

    // Map serialization - JSON uses native object format
    void begin_map(size_t size);

    void end_map();

    void write_map_key(const std::string& key);

    // One value, its containers, instances and handle payloads walked on an explicit frame stack: nesting costs memory, never
    // native depth
    void write_value(const script_value& root);

    // Get the resulting JSON string
    std::string str() const;
    // The resulting JSON string, moved out (the writer is spent)
    std::string take();

private:
    enum class ContainerType { Object, Array };

    enum class write_shape : uint8_t { array, map, tagged_map, instance, fields };
    struct write_frame {
        write_frame(write_shape a_shape, script_value a_source) : shape(a_shape), source(std::move(a_source)) {}
        write_shape shape;
        script_value source;            // the container or the instance
        size_t index = 0;
        size_t count = 0;
        bool first = true;
        uint8_t phase = 0;              // a tagged map's pair: 0 its key next, 1 its value next, 2 its close
        script_map::const_iterator next{};
        class_definition* class_def = nullptr;
        strong_ptr<class_instance> instance;   // an instance frame's object: its script fields save as stored
        std::vector<std::string> names;
        std::vector<std::pair<std::string, script_value>> fields;
    };
    bool aborted_ = false;

    // Writes a leaf whole, or a container's opening and the frame that writes the rest
    void begin_value(const script_value& given, std::vector<write_frame>& frames);

    // Writes what comes before the frame's next child and hands the child out (in its container, or in scratch when it had
    // to be made); null when the frame has none left
    const script_value* next_child(write_frame& f, script_value& scratch);

    void end_frame(write_frame& f);

    static strong_ptr<class_instance> written_instance(const script_value& value);

    // {"$id": N} or {"$ref": N}; the archive's first reference to the object carries its "$type" and "$val" beside the id
    void begin_handle(std::string_view id_key, const strong_ptr<script_value::object_holder>& holder, std::vector<write_frame>& frames);

    // An instance's "$type" and its fields under "$val", inside an object already open: the frame closes both
    void begin_instance_members(const script_value& value, strong_ptr<class_instance> instance, bool comma,
                                std::vector<write_frame>& frames);

    // Local helper functions to avoid circular includes
    // write_value's layout: compact (indent_ <= 0) or one member per line, indent_ spaces per level
    void open_value(char bracket);

    void close_value(char bracket, bool empty);

    void write_value_separator(bool comma);

    // One line's indentation. A wide indent is the layout's amplifier (every line pays it): a script's to_json charges it
    // to the engine's memory cap.
    void append_indentation();

    void write_value_key(std::string_view key, bool comma);

    void append_escaped(std::string_view str);

    // A scalar's separator before it and its container's first-member flag after it
    void begin_element();
    void begin_scalar();
    void end_scalar();

    // A map whose keys are not all strings (or an empty one declared with such a key type) is written as {"$map": [[k, v], ...]}
    // so its keys keep their kind; from_json reads the tag back
    static bool map_needs_tag(const script_value& value);

    // Scalars and non-instance objects; write_value handles containers, handles and instances
    void write_leaf(const script_value& val);

    int indent_;
    bool charges_indentation_ = false;
    int json_depth_;  // JSON nesting depth (separate from serialization depth tracking in base)
    size_t open_records_ = 0;
    std::string out_;
    std::stack<ContainerType> container_stack_;
    std::stack<bool> first_in_container_;

    bool in_container() const;

    void write_indent();
    
    void write_newline();
    void open_container(char a_brace, ContainerType a_kind);
    void close_container(char a_brace);
    
    template<typename T>
    void write_json_value(T value);
};

class json_archive_reader : public archive_reader_impl<json_archive_reader> {
public:
    // Engine is REQUIRED for JSON reading since we need to create script_values. The text is parsed before the constructor
    // returns; the reader keeps no reference to it.
    json_archive_reader(std::string_view json_string, engine* eng);

    ~json_archive_reader();

    static constexpr bool needs_property_keys = false;
    static constexpr bool is_text_format = true;

    int8_t read_int8();
    int16_t read_int16();
    int32_t read_int32();
    int64_t read_int64();
    uint8_t read_uint8();
    uint16_t read_uint16();
    uint32_t read_uint32();
    uint64_t read_uint64();
    float read_float32();
    double read_float64();
    bool read_bool();
    std::string read_string();

    bool peek_null();

    void read_null();

    std::vector<uint8_t> read_binary(size_t /*size*/);

    void begin_object();

    void begin_object(std::string& type_name, uint32_t& version);

    void end_object();

    size_t begin_array();

    void end_array();

    bool read_property_name(std::string& name);

    // Map deserialization - JSON reads from native object format
    size_t begin_map();

    void end_map();

    bool read_map_key(std::string& key);

    void clear_property_value() { cpv_valid_ = false; }
    bool has_current_property_value() const { return cpv_valid_; }
    bool in_array() const;

    bool has_property(const std::string& name);

    // Seek to a property by name. Builds a lazy name->value-node index on first random
    // access (the common sequential read path never pays for it); keys are string_views
    // into the char arena, so lookups never construct a script_value.
    bool seek_property(const std::string& name);

    script_value read_value();

    // Some object member's key begins with '$': the text may hold archive records ($id, $type, $map, $char...)
    bool has_archive_keys() const { return archive_keys_; }

private:
    // ---- Flat arena DOM ----------------------------------------------------------------
    // Objects/arrays are 16-byte nodes in one contiguous arena; string bytes live in one
    // char arena. Composites become script_values only on demand (read_value/materialize).
    enum class JTag : uint8_t { Null, Bool, Int, UInt, Double, Str, Arr, Obj };
    struct JNode {
        JTag tag = JTag::Null;
        uint32_t key = 0;   // an object member key's place among the distinct keys, plus one (0: not a key)
        union UVal {
            int64_t  i;                              // Int, Bool (0/1)
            uint64_t u;                              // UInt: an integer past int64's range that fits uint64
            double   d;                              // Double
            struct { uint32_t off, len; } str;       // Str: [off,len) into chars_
            struct { uint32_t first, count; } agg;   // Arr: count elems; Obj: count members ([k,v]*)
        } u{};
    };

    std::string_view json_;   // parse source, during the constructor only
    size_t pos_ = 0;          // parse cursor
    bool archive_keys_ = false;
    // The distinct member keys: each key's text is in chars_ once (spans), found by its hash (an open-addressing table of
    // places plus one); a materialized document's keys share one string value per distinct key
    std::vector<std::pair<uint32_t, uint32_t>> key_spans_;
    std::vector<uint32_t> key_table_;
    std::vector<script_value> key_values_;
    uint32_t intern_key(uint32_t off, uint32_t len);
    const script_value& key_value(const JNode& k, engine* eng);
    std::vector<JNode> nodes_;   // value arena (root_ is the last element)
    std::string chars_;          // unescaped string-byte arena
    uint32_t root_ = 0;
    std::vector<JNode> scratch_; // parser stack (children bulk-moved into nodes_ on container close)

    uint32_t cpv_node_ = 0;   // current property value: a flat node index
    bool cpv_valid_ = false;
    size_t cpv_depth_ = 0;

    // Resolves the next value to consume as a flat node index. cpv is only valid at the
    // object depth where it was set; arrays advance an element cursor. Returns false only
    // when an open array is exhausted (callers either skip or fall back to the root).
    bool next_node(uint32_t& out, bool peek_only = false);
    const char* found_kind(bool present, uint32_t n) const;
    template<typename T> T read_integer();

    // Builds an engine-bound script_value for a primitive flat node (the hot path; no recursion).
    script_value materialize_primitive(const JNode& node, engine* eng);

    // A math value's JSON: exactly {"$type": "math::K", "$val": {"x": .., "y": .. [, "z": ..]}}
    // with numeric components; any other object is a map. A malformed math payload is an error, never a silent map.
    bool materialize_math(const JNode& node, engine* eng, script_value& out);

    // Builds a full script_value subtree from a flat node, its containers on an explicit frame stack — only used when a raw
    // composite is actually requested (read_value: stdlib from_json, a script value inside a C++ record).
    // Keys are ENGINE-CARRYING string values: ordering and map.find(script_value("$type"/...))
    // rank by type-then-content (type_info never participates), and every value in the result
    // must clone/detach — the old ast_literal_tag keys had no engine pointer, so copying any
    // deserialized map died with "Cannot clone script_value: missing engine pointer".
    script_value materialize(uint32_t idx);

    // ---- Flat JSON parser (builds nodes_/chars_; no script_value DOM) ----
    void parse_into_arena();

    // Bulk-moves the [mark, end) children off the scratch stack into the arena and pushes one
    // container node referencing them. Objects store members interleaved [key,value,...].
    void push_container(JTag tag, size_t mark);

    // One value, its containers walked on an explicit frame stack: nesting costs memory, never native depth
    void parse_value_flat();

    // An object member's key and its colon (parse_string_into's expect('"') skips any leading whitespace)
    void parse_member_key();

    void skip_whitespace();
    
    char peek();
    
    char advance();
    
    void expect(char c);
    
    // Appends the UTF-8 encoding of a Unicode code point.
    static void append_utf8(std::string& out, uint32_t cp);

    // Reads the 4 hex digits at json_[pos_+1 .. pos_+4] and advances pos_ by 4 (to the
    // last digit; the caller's trailing pos_++ steps past it). Returns false on missing or
    // non-hex input (keeps errors in the serialization_error hierarchy on malformed \u escapes).
    bool read_hex4(uint32_t& out);

    // Parses a string body into the char arena, returning its [off,len).
    void parse_string_into(uint32_t& off, uint32_t& len);

    void parse_bool_flat();

    void parse_null_flat();

    // libc++ still ships integer-only std::from_chars; the floating-point overloads are
    // absent, and the incomplete implementation deliberately leaves __cpp_lib_to_chars
    // undefined — exactly the feature-test to key on. The fallback preserves the two
    // from_chars behaviors parse_number_flat relies on (full-consumption ptr,
    // errc::result_out_of_range): the caller pre-scans a strict JSON number charset, so
    // strtod never sees whitespace/hex/inf/nan forms, ERANGE maps to result_out_of_range,
    // and '.' is swapped for the host locale's decimal point so a comma-decimal locale
    // cannot misparse.
    struct double_parse_result { const char* ptr; std::errc ec; };
    static double_parse_result from_chars_double(const char* first, const char* last, double& d);

    void parse_number_flat();

    // Open-object reader state over the flat arena.
    struct ObjState {
        uint32_t first;    // index of the first member node (a key) in nodes_
        uint32_t count;    // member count
        uint32_t cursor;   // sequential read position, 0..count
        // Lazy name -> value-node index, built on first random (seek/has) access. Keys are
        // string_views into chars_ (stable for the reader's lifetime).
        std::unique_ptr<std::unordered_map<std::string_view, uint32_t>> index;
    };

    struct ArrState {
        uint32_t first;
        uint32_t count;
        uint32_t index;
    };

    std::unordered_map<std::string_view, uint32_t>& object_index(ObjState& s);

    std::stack<ObjState> object_stack_;
    std::stack<ArrState> array_stack_;
};

// Archive ID trait specializations for dispatch pattern
template<> struct writer_archive_id_trait<json_archive_writer> {
    static constexpr writer_archive_id value = writer_archive_id::json;
};
template<> struct reader_archive_id_trait<json_archive_reader> {
    static constexpr reader_archive_id value = reader_archive_id::json;
};

} // namespace serialization
} // namespace jai