#pragma once

#ifndef __JAISCRIPT_DETAIL_AST_SERIALIZER_HPP__
#define __JAISCRIPT_DETAIL_AST_SERIALIZER_HPP__

// Binary AST serializer for jaibite save/load. Interned symbol IDs are per-engine and
// non-deterministic, so every symbol/name is written as a string-table index and
// re-interned into the LOADING engine's symbolizer; type_info is written structurally
// and re-interned via engine::get_type_info. The compiled VM chunk is never written —
// a loaded bite recompiles lazily through the normal path. Fixed-width fields and floats
// are written little-endian on every host; the source hash reads its words in host order.
//
// Format: "JBIT" magic, u32 version, u32 flags, u64 registration fingerprint, u64 source
// size + 1 and u64 source hash (0, 0 = no source), string table (count + length-prefixed strings), the context section when
// flag bit 1 says so (jaibite_context: namespace query count + each query's table index and u8 answer, outside name count +
// table indices, u64 key), declaration count, node tree.
// Node encoding: u8 node_type tag (0xFF = null pointer), source_location, fields.
// Runtime AST patches (identifier slot_index -> SIZE_MAX, outer_slot_plan, cached
// getter_id) are serialized faithfully: patched state is self-consistent in memory on
// both backends, so a faithful round trip preserves behavior.

#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/map_entry_binding.hpp>
#include <jaiscript/detail/native_stack.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/core/engine.hpp>
#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace jai::detail {

    inline constexpr uint8_t k_jaibite_magic[4] = { 'J', 'B', 'I', 'T' };
    // BUMP THIS on any change to what a parse produces: the serialized bytes and the parse-time semantics baked into the
    // AST (slot assignment, node and token numbers, capture plans). A stale sidecar is reparsed and rewritten (the version
    // check feeds the corrupt-file fallback), so a bump is always safe and a missed one leaves stale caches.
    inline constexpr uint32_t k_jaibite_version = 56;
    // Header flags (u32):
    // bit 0 = the bite was static-checked clean under the SAVING engine's surface —
    // trusted on load only when the registration fingerprint also matches.
    inline constexpr uint32_t k_jaibite_flag_checked_clean = 0x1;
    // bit 1 = a context section follows the string table (the writer sets it whenever the context is not empty)
    inline constexpr uint32_t k_jaibite_flag_context = 0x2;
    // BUMP THIS whenever the static checker's rules change (a new diagnostic, a rule that rejects more): a checked-clean
    // stamp records the revision that checked it (flag bits 8..31), and a bite stamped under another revision is checked
    // again on load instead of trusted. A parse change bumps k_jaibite_version instead, which re-parses as well.
    inline constexpr uint32_t k_jaibite_checker_revision = 9;
    inline constexpr uint32_t jaibite_checked_clean_flags() { return k_jaibite_flag_checked_clean | (k_jaibite_checker_revision << 8); }
    // The stamp is trusted only from this checker revision
    inline constexpr bool jaibite_stamped_clean(uint32_t flags) {
        return (flags & k_jaibite_flag_checked_clean) && (flags >> 8) == k_jaibite_checker_revision;
    }
    // The source a bite was parsed from: its byte count and a 64-bit hash of its bytes. A disk sibling is valid only for
    // the source whose identity equals the one it records (never by an mtime ordering); a bite saved without a source
    // records the empty identity, which no source text has.
    struct jaibite_source_identity {
        uint64_t size = 0;
        uint64_t hash = 0;
        bool operator==(const jaibite_source_identity&) const = default;
    };
    // Each step is a bijection of the running state for a fixed word and of the word for a fixed state, so two texts of
    // one length that differ in a single word never collide.
    inline jaibite_source_identity jaibite_identity_of(std::string_view source) {
        const auto* p = reinterpret_cast<const unsigned char*>(source.data());
        const size_t n = source.size();
        uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
        const auto step = [&h](uint64_t w) { h = (h ^ w) * 0xBF58476D1CE4E5B9ull; h ^= h >> 31; };
        size_t i = 0;
        for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); step(w); }
        uint64_t tail = uint64_t(n - i) << 56;
        for (size_t k = 0; i + k < n; ++k) { tail |= uint64_t(p[i + k]) << (8 * k); }
        step(tail);
        h ^= h >> 30; h *= 0x94D049BB133111EBull; h ^= h >> 31;
        return { uint64_t(n) + 1, h };
    }
    inline constexpr size_t k_jaibite_header_size = 36;   // magic, version, flags, fingerprint, source size, source hash
    // The header of a current-version bite, read without touching the body; false = not one
    inline bool read_jaibite_header(const uint8_t* data, size_t size, uint64_t& fingerprint, jaibite_source_identity& source) {
        if (size < k_jaibite_header_size || std::memcmp(data, k_jaibite_magic, 4) != 0) return false;
        const auto u64 = [data](size_t at) { uint64_t x = 0; for (int i = 0; i < 8; ++i) x |= uint64_t(data[at + i]) << (i * 8); return x; };
        const uint32_t version = uint32_t(data[4]) | uint32_t(data[5]) << 8 | uint32_t(data[6]) << 16 | uint32_t(data[7]) << 24;
        if (version != k_jaibite_version) return false;
        fingerprint = u64(12);
        source = { u64(20), u64(28) };
        return true;
    }
    // Locations naming the unit's own file serialize as this marker and read back as the
    // path the jaibite is loaded under, so a moved folder still names its files
    // (stack traces, breakpoints, engine::current_source_file).
    inline constexpr std::string_view k_jaibite_own_file = "";

    [[noreturn]] JAI_NOINLINE inline void jaibite_fail(std::string_view why) {
        throw jai::runtime_error("jaibite: " + std::string(why));
    }

    class ast_writer {
    public:
        // include_locations = false produces a POSITION-INSENSITIVE encoding (structural
        // identity keys, e.g. hot-reload redefinition equality) — never deserialized.
        explicit ast_writer(const string_symbolizer& symbols, bool include_locations = true, std::string own_path = {})
            : symbols_(symbols), include_locations_(include_locations), own_path_(std::move(own_path)) {}
        // the subtree is a body's (the parallel body copier): its slots are written, as a function's are
        ast_writer& in_body() { frame_depth_ = 1; return *this; }

        // Structural key for one subtree: string table + node encoding, a deterministic
        // pure function of AST structure (symbol ids persist as first-seen table indices,
        // so engine-local id values never leak in). Byte equality = structural equality.
        std::vector<uint8_t> node_key(const ast_node* n) {
            node(n);
            std::vector<uint8_t> out;
            out.reserve(8 + body_.size() + table_.size() * 8);
            append_varint(out, table_.size());
            for (const auto& s : table_) {
                append_varint(out, s.size());
                out.insert(out.end(), s.begin(), s.end());
            }
            out.insert(out.end(), body_.begin(), body_.end());
            return out;
        }

        std::vector<uint8_t> serialize(const std::vector<declaration_ptr>& decls, uint64_t fingerprint,
                                       uint32_t flags = 0, jaibite_source_identity source = {}, const jaibite_context* context = nullptr) {
            varint(decls.size());
            for (const auto& d : decls) node(d.get());
            std::vector<uint64_t> namespace_queries, outside;
            if (context && !context->empty()) {
                flags |= k_jaibite_flag_context;
                for (const auto& q : context->namespace_queries) { namespace_queries.push_back(table_index(q)); }
                for (const auto& n : context->outside) { outside.push_back(table_index(n)); }
            }

            std::vector<uint8_t> out;
            out.reserve(24 + body_.size() + table_.size() * 8);
            out.insert(out.end(), k_jaibite_magic, k_jaibite_magic + 4);
            fixed_u32(out, k_jaibite_version);
            fixed_u32(out, flags);
            fixed_u64(out, fingerprint);
            fixed_u64(out, source.size);
            fixed_u64(out, source.hash);
            append_varint(out, table_.size());
            for (const auto& s : table_) {
                append_varint(out, s.size());
                out.insert(out.end(), s.begin(), s.end());
            }
            if (flags & k_jaibite_flag_context) {
                append_varint(out, namespace_queries.size());
                for (size_t i = 0; i < namespace_queries.size(); ++i) {
                    append_varint(out, namespace_queries[i]);
                    out.push_back(i < context->namespace_answers.size() && context->namespace_answers[i] == '1' ? 1 : 0);
                }
                append_varint(out, outside.size());
                for (uint64_t i : outside) { append_varint(out, i); }
                fixed_u64(out, context->outside_key);
            }
            out.insert(out.end(), body_.begin(), body_.end());
            return out;
        }

    private:
        static void fixed_u32(std::vector<uint8_t>& v, uint32_t x) {
            for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (i * 8)));
        }
        static void fixed_u64(std::vector<uint8_t>& v, uint64_t x) {
            for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (i * 8)));
        }
        static void append_varint(std::vector<uint8_t>& v, uint64_t x) {
            while (x >= 0x80) { v.push_back(uint8_t(x) | 0x80); x >>= 7; }
            v.push_back(uint8_t(x));
        }

        void u8(uint8_t x) { body_.push_back(x); }
        void varint(uint64_t x) { append_varint(body_, x); }
        void zigzag(int64_t x) { varint((uint64_t(x) << 1) ^ uint64_t(x >> 63)); }
        void f64(double x) {
            uint64_t bits; std::memcpy(&bits, &x, 8);
            for (int i = 0; i < 8; ++i) body_.push_back(uint8_t(bits >> (i * 8)));
        }
        void flag(bool b) { u8(b ? 1 : 0); }

        uint64_t table_index(std::string_view s) {
            auto it = table_lookup_.find(std::string(s));
            if (it != table_lookup_.end()) return it->second;
            uint64_t idx = table_.size();
            table_.emplace_back(s);
            table_lookup_.emplace(table_.back(), idx);
            return idx;
        }
        void str(std::string_view s) { varint(table_index(s)); }

        // Symbol IDs are engine-local: persist the NAME (table index), never the raw id.
        void symbol(uint64_t id, uint64_t sentinel) {
            if (id == sentinel) { varint(0); return; }
            std::string_view name = symbols_.get_string(id);
            if (name.empty()) jaibite_fail("cannot serialize unresolvable symbol id");
            varint(1 + table_index(name));
        }
        // a program's own frame writes none: its block-scoped slots are re-derived at load (detail::program_slot_pass)
        void slot(size_t s) { varint(s == SIZE_MAX || frame_depth_ == 0 ? 0 : uint64_t(s) + 1); }
        int frame_depth_ = 0;   // the bodies (functions, lambdas, parallel_for) the writer is inside

        void location(const source_location& loc) {
            if (!include_locations_) {
                return;
            }
            str(!own_path_.empty() && loc.filename == own_path_ ? k_jaibite_own_file : std::string_view(loc.filename));
            varint(loc.line);
            varint(loc.column);
        }

        void type(const type_info_ptr& t) {
            if (!t) { u8(0); return; }
            u8(1);
            u8(uint8_t(t->base_type));
            str(t->type_name);
            varint(t->native_size);
            flag(t->is_signed);
            varint(t->type_params.size());
            if (stack_.low()) jaibite_fail("type nesting too deep");
            for (const auto& p : t->type_params) type(p);
        }

        // AST literals are engine-less placeholder values (ast_literal_tag) — scalar payloads only.
        void literal(const script_value& v) {
            if (v.is_null()) { u8(0); }
            else if (v.is_int()) { u8(1); zigzag(v.unchecked_as_int()); }
            else if (v.is_float()) { u8(2); f64(v.unchecked_as_float()); }
            else if (v.is_string()) { u8(3); str(v.unchecked_as_string()); }
            else if (v.is_char()) { u8(4); u8(uint8_t(v.unchecked_as_char())); }
            else if (v.is_bool()) { u8(5); flag(v.unchecked_as_bool()); }
            else jaibite_fail("unsupported literal value type in AST");
        }

        void tok(const token& t) {
            u8(uint8_t(t.type));
            str(t.lexeme);
            symbol(t.symbol_id, 0);  // token uses 0 = not interned
            location(t.location);
            switch (t.type) {
                case token_type::integer_literal: zigzag(t.int_value); break;
                case token_type::float_literal: f64(t.float_value); break;
                case token_type::boolean_literal: flag(t.bool_value); break;
                case token_type::char_literal: u8(uint8_t(t.char_value)); break;
                case token_type::string_literal: str(t.string_value); break;
                case token_type::plus: flag(t.joins_text); break;
                default: break;
            }
        }

        void parameters(const std::vector<parameter>& params) {
            varint(params.size());
            for (const auto& p : params) {
                type(p.type);
                str(p.name);
                flag(p.is_reference);
                flag(p.is_const);
                symbol(p.symbol_id, UINT64_MAX);
                slot(p.slot_index);
                node(p.default_value.get());
            }
        }

        void outer_slot_plan(const std::vector<std::pair<uint64_t, size_t>>& plan, bool built) {
            flag(built);
            varint(plan.size());
            for (const auto& [sym, s] : plan) { symbol(sym, UINT64_MAX); slot(s); }
        }

        void node(const ast_node* n) {
            if (!n) { u8(0xFF); return; }
            if (stack_.low()) jaibite_fail("AST nesting too deep");
            u8(uint8_t(n->get_type()));
            location(n->location);
            if (n->get_type() >= node_type::expression_stmt) u8(static_cast<const statement*>(n)->check_mode_attr);
            switch (n->get_type()) {
                case node_type::literal_expr:
                    write_literal_expr(n);
                    break;
                case node_type::identifier_expr:
                    write_identifier_expr(n);
                    break;
                case node_type::binary_expr:
                    write_binary_expr(n);
                    break;
                case node_type::unary_expr:
                    write_unary_expr(n);
                    break;
                case node_type::assignment_expr:
                    write_assignment_expr(n);
                    break;
                case node_type::call_expr:
                    write_call_expr(n);
                    break;
                case node_type::member_expr:
                    write_member_expr(n);
                    break;
                case node_type::lambda_expr:
                    write_lambda_expr(n);
                    break;
                case node_type::new_expr:
                    write_new_expr(n);
                    break;
                case node_type::ternary_expr:
                    write_ternary_expr(n);
                    break;
                case node_type::array_literal_expr:
                    write_array_literal_expr(n);
                    break;
                case node_type::map_literal_expr:
                    write_map_literal_expr(n);
                    break;
                case node_type::this_expr:
                case node_type::super_expr:
                    break;
                case node_type::throw_expr:
                    write_throw_expr(n);
                    break;
                case node_type::yield_expr:
                    write_yield_expr(n);
                    break;
                case node_type::cast_expr:
                    write_cast_expr(n);
                    break;
                case node_type::expression_stmt:
                    write_expression_stmt(n);
                    break;
                case node_type::block_stmt:
                    write_block_stmt(n);
                    break;
                case node_type::if_stmt:
                    write_if_stmt(n);
                    break;
                case node_type::while_stmt:
                    write_while_stmt(n);
                    break;
                case node_type::for_stmt:
                    write_for_stmt(n);
                    break;
                case node_type::range_for_stmt:
                    write_range_for_stmt(n);
                    break;
                case node_type::parallel_for_stmt:
                    write_parallel_for_stmt(n);
                    break;
                case node_type::return_stmt:
                    write_return_stmt(n);
                    break;
                case node_type::break_stmt:
                case node_type::continue_stmt:
                case node_type::fallthrough_stmt:
                    break;
                case node_type::try_stmt:
                    write_try_stmt(n);
                    break;
                case node_type::switch_stmt:
                    write_switch_stmt(n);
                    break;
                case node_type::case_stmt:
                    write_case_stmt(n);
                    break;
                case node_type::default_stmt:
                    write_default_stmt(n);
                    break;
                case node_type::variable_decl:
                    write_variable_decl(n);
                    break;
                case node_type::function_decl:
                    write_function_decl(n);
                    break;
                case node_type::class_decl:
                    write_class_decl(n);
                    break;
                case node_type::namespace_decl:
                    write_namespace_decl(n);
                    break;
                case node_type::expression_decl:
                    write_expression_decl(n);
                    break;
                case node_type::statement_decl:
                    write_statement_decl(n);
                    break;
                case node_type::include_decl:
                    write_include_decl(n);
                    break;
                case node_type::include_expr:
                    write_include_expr(n);
                    break;
                case node_type::import_decl:
                    write_import_decl(n);
                    break;
                case node_type::import_expr:
                    write_import_expr(n);
                    break;
                case node_type::extern_decl:
                    write_extern_decl(n);
                    break;
                case node_type::enum_decl:
                    write_enum_decl(n);
                    break;
                case node_type::destructuring_decl:
                    write_destructuring_decl(n);
                    break;
                default:
                    jaibite_fail("unserializable AST node type " + std::to_string(int(n->get_type())));
            }
        }

        JAI_NOINLINE void write_literal_expr(const ast_node* n) {
            auto* e = static_cast<const literal_expr*>(n);
            literal(e->value);
        }

        JAI_NOINLINE void write_identifier_expr(const ast_node* n) {
            auto* e = static_cast<const identifier_expr*>(n);
            str(e->name);
            symbol(e->symbol_id, UINT64_MAX);
            slot(e->slot_index);
            flag(e->global_qualified);   // ::name is pure syntax - not re-derivable
        }

        JAI_NOINLINE void write_binary_expr(const ast_node* n) {
            auto* e = static_cast<const binary_expr*>(n);
            node(e->left.get());
            tok(e->op);
            node(e->right.get());
        }

        JAI_NOINLINE void write_unary_expr(const ast_node* n) {
            auto* e = static_cast<const unary_expr*>(n);
            tok(e->op);
            node(e->operand.get());
            flag(e->is_postfix);
        }

        JAI_NOINLINE void write_assignment_expr(const ast_node* n) {
            auto* e = static_cast<const assignment_expr*>(n);
            node(e->target.get());
            tok(e->op);
            node(e->value.get());
            u8(static_cast<uint8_t>((e->postfix_old_result ? 1 : 0) | (e->incdec_desugar ? 2 : 0) | (e->yields_reference ? 4 : 0)));
        }

        JAI_NOINLINE void write_call_expr(const ast_node* n) {
            auto* e = static_cast<const call_expr*>(n);
            node(e->callee.get());
            varint(e->arguments.size());
            for (const auto& a : e->arguments) node(a.get());
        }

        JAI_NOINLINE void write_member_expr(const ast_node* n) {
            auto* e = static_cast<const member_expr*>(n);
            node(e->object.get());
            str(e->member);
            symbol(e->member_id, UINT64_MAX);
            symbol(e->getter_id, UINT64_MAX);
            flag(e->is_arrow);
            flag(e->is_static);
            flag(e->null_safe);
        }

        JAI_NOINLINE void write_lambda_expr(const ast_node* n) {
            auto* e = static_cast<const lambda_expr*>(n);
            ++frame_depth_;
            u8(uint8_t(e->default_capture));
            varint(e->captures.size());
            for (const auto& c : e->captures) {
                str(c.name);
                flag(c.by_reference);
                symbol(c.symbol_id, UINT64_MAX);
                slot(c.outer_slot);
                node(c.initializer.get());
            }
            parameters(e->parameters);
            type(e->return_type);
            node(e->body.get());
            varint(e->local_count);
            outer_slot_plan(e->outer_slot_plan, e->outer_slot_plan_built);
            varint(e->chained_outer_slots.size());
            for (const auto& [sym, s] : e->chained_outer_slots) { symbol(sym, UINT64_MAX); slot(s); }
            varint(e->chained_outer_names.size());
            for (uint64_t sym : e->chained_outer_names) symbol(sym, UINT64_MAX);
            --frame_depth_;
        }

        JAI_NOINLINE void write_new_expr(const ast_node* n) {
            auto* e = static_cast<const new_expr*>(n);
            type(e->type);
            varint(e->arguments.size());
            for (const auto& a : e->arguments) node(a.get());
        }

        JAI_NOINLINE void write_ternary_expr(const ast_node* n) {
            auto* e = static_cast<const ternary_expr*>(n);
            node(e->condition.get());
            node(e->then_expression.get());
            node(e->else_expression.get());
        }

        JAI_NOINLINE void write_array_literal_expr(const ast_node* n) {
            auto* e = static_cast<const array_literal_expr*>(n);
            varint(e->elements.size());
            for (const auto& el : e->elements) node(el.get());
        }

        JAI_NOINLINE void write_map_literal_expr(const ast_node* n) {
            auto* e = static_cast<const map_literal_expr*>(n);
            varint(e->entries.size());
            for (const auto& [k, v] : e->entries) { node(k.get()); node(v.get()); }
        }

        JAI_NOINLINE void write_throw_expr(const ast_node* n) {
            auto* e = static_cast<const throw_expr*>(n);
            node(e->value.get());
        }

        JAI_NOINLINE void write_yield_expr(const ast_node* n) {
            auto* e = static_cast<const yield_expr*>(n);
            node(e->value.get());
        }

        JAI_NOINLINE void write_cast_expr(const ast_node* n) {
            auto* e = static_cast<const cast_expr*>(n);
            type(e->target);
            u8(static_cast<uint8_t>((e->by_reference ? 1 : 0) | (e->implicit_ref ? 2 : 0) | (e->binding_ref ? 4 : 0)));
            node(e->operand.get());
        }

        JAI_NOINLINE void write_expression_stmt(const ast_node* n) {
            auto* s = static_cast<const expression_stmt*>(n);
            node(s->expression.get());
        }

        JAI_NOINLINE void write_block_stmt(const ast_node* n) {
            auto* s = static_cast<const block_stmt*>(n);
            varint(s->declarations.size());
            for (const auto& d : s->declarations) node(d.get());
        }

        JAI_NOINLINE void write_if_stmt(const ast_node* n) {
            auto* s = static_cast<const if_stmt*>(n);
            node(s->condition.get());
            node(s->then_statement.get());
            node(s->else_statement.get());
        }

        JAI_NOINLINE void write_while_stmt(const ast_node* n) {
            auto* s = static_cast<const while_stmt*>(n);
            node(s->condition.get());
            node(s->body.get());
        }

        JAI_NOINLINE void write_for_stmt(const ast_node* n) {
            auto* s = static_cast<const for_stmt*>(n);
            node(s->initializer.get());
            node(s->condition.get());
            node(s->update.get());
            node(s->body.get());
        }

        JAI_NOINLINE void write_range_for_stmt(const ast_node* n) {
            auto* s = static_cast<const range_for_stmt*>(n);
            type(s->auto_spelled ? type_info_ptr{} : s->element_type);   // auto stays auto: the checker re-types it at load
            str(s->variable_name);
            symbol(s->variable_name_id, UINT64_MAX);
            slot(s->variable_slot_index);
            flag(s->is_reference);
            flag(s->is_const);
            node(s->container.get());
            node(s->body.get());
        }

        JAI_NOINLINE void write_parallel_for_stmt(const ast_node* n) {
            auto* s = static_cast<const parallel_for_stmt*>(n);
            std::vector<parameter> all{ s->loop_param };
            all.insert(all.end(), s->extra_params.begin(), s->extra_params.end());
            ++frame_depth_;
            parameters(all);
            --frame_depth_;
            varint(s->local_count);
            varint(s->frame_captures.size());
            for (const auto& [sym, slot] : s->frame_captures) { symbol(sym, UINT64_MAX); varint(slot); }
            node(s->container.get());
            varint(s->extra_containers.size());
            for (const auto& c : s->extra_containers) node(c.get());
            ++frame_depth_;
            node(s->body.get());
            --frame_depth_;
        }

        JAI_NOINLINE void write_return_stmt(const ast_node* n) {
            auto* s = static_cast<const return_stmt*>(n);
            node(s->value.get());
        }

        JAI_NOINLINE void write_try_stmt(const ast_node* n) {
            auto* s = static_cast<const try_stmt*>(n);
            node(s->try_block.get());
            str(s->catch_var);
            symbol(s->catch_var_id, UINT64_MAX);
            slot(s->catch_slot_index);
            node(s->catch_block.get());
        }

        JAI_NOINLINE void write_switch_stmt(const ast_node* n) {
            auto* s = static_cast<const switch_stmt*>(n);
            node(s->condition.get());
            varint(s->cases.size());
            for (const auto& c : s->cases) node(c.get());
            node(s->default_case.get());
        }

        JAI_NOINLINE void write_case_stmt(const ast_node* n) {
            auto* s = static_cast<const case_stmt*>(n);
            node(s->value.get());
            varint(s->body.size());
            for (const auto& b : s->body) node(b.get());
            flag(s->has_fallthrough);
        }

        JAI_NOINLINE void write_default_stmt(const ast_node* n) {
            auto* s = static_cast<const default_stmt*>(n);
            varint(s->body.size());
            for (const auto& b : s->body) node(b.get());
        }

        JAI_NOINLINE void write_variable_decl(const ast_node* n) {
            auto* d = static_cast<const variable_decl*>(n);
            type(d->auto_spelled ? type_info_ptr{} : d->type);   // auto stays auto: the checker re-types it at load
            str(d->name);
            symbol(d->name_id, UINT64_MAX);
            node(d->initializer.get());
            flag(d->is_static);
            flag(d->function_spelling);
            flag(d->default_initialized);
            flag(d->is_inline);
            flag(d->is_const);
            slot(d->slot_index);
        }

        JAI_NOINLINE void write_function_decl(const ast_node* n) {
            auto* d = static_cast<const function_decl*>(n);
            ++frame_depth_;
            str(d->name);
            symbol(d->name_id, UINT64_MAX);
            parameters(d->parameters);
            type(d->return_type);
            node(d->body.get());
            varint(d->initializers.size());
            for (const auto& init : d->initializers) {
                str(init.target);
                varint(init.arguments.size());
                for (const auto& a : init.arguments) node(a.get());
            }
            flag(d->is_override);
            flag(d->is_static);
            flag(d->is_coroutine);
            flag(d->is_nodiscard);
            varint(d->local_count);
            outer_slot_plan(d->outer_slot_plan, d->outer_slot_plan_built);
            --frame_depth_;
        }

        JAI_NOINLINE void write_class_decl(const ast_node* n) {
            auto* d = static_cast<const class_decl*>(n);
            str(d->name);
            symbol(d->name_id, UINT64_MAX);
            u8(d->nocopy ? 1 : 0);
            varint(d->base_classes.size());
            for (const auto& b : d->base_classes) str(b);
            varint(d->members.size());
            for (const auto& m : d->members) {
                u8(uint8_t(m.visibility));
                node(m.declaration.get());
            }
        }

        JAI_NOINLINE void write_namespace_decl(const ast_node* n) {
            auto* d = static_cast<const namespace_decl*>(n);
            str(d->name);
            symbol(d->name_id, UINT64_MAX);
            varint(d->declarations.size());
            for (const auto& dd : d->declarations) node(dd.get());
        }

        JAI_NOINLINE void write_expression_decl(const ast_node* n) {
            auto* d = static_cast<const expression_decl*>(n);
            node(d->expression.get());
            flag(d->implicit_return);
        }

        JAI_NOINLINE void write_statement_decl(const ast_node* n) {
            auto* d = static_cast<const statement_decl*>(n);
            node(d->statement.get());
        }

        JAI_NOINLINE void write_include_decl(const ast_node* n) {
            auto* d = static_cast<const include_decl*>(n);
            str(d->path);
            node(d->path_expr.get());
        }

        JAI_NOINLINE void write_include_expr(const ast_node* n) {
            auto* e = static_cast<const include_expr*>(n);
            str(e->path);
            node(e->path_expr.get());
        }

        JAI_NOINLINE void write_import_decl(const ast_node* n) {
            auto* d = static_cast<const import_decl*>(n);
            str(d->path);
            node(d->path_expr.get());
        }

        JAI_NOINLINE void write_import_expr(const ast_node* n) {
            auto* e = static_cast<const import_expr*>(n);
            varint(e->paths.size());
            for (const auto& p : e->paths) str(p);
            varint(e->path_exprs.size());
            for (const auto& a : e->path_exprs) node(a.get());
            flag(e->force);
        }

        JAI_NOINLINE void write_extern_decl(const ast_node* n) {
            auto* d = static_cast<const extern_decl*>(n);
            str(d->name);
            symbol(d->name_id, UINT64_MAX);
            flag(d->is_function);
            flag(d->is_const);
            varint(d->written_at);
            type(d->type);
        }

        JAI_NOINLINE void write_enum_decl(const ast_node* n) {
            auto* d = static_cast<const enum_decl*>(n);
            str(d->name);
            symbol(d->name_id, UINT64_MAX);
            varint(d->values.size());
            for (size_t i = 0; i < d->values.size(); ++i) {
                str(d->values[i].first);
                symbol(d->values[i].second, UINT64_MAX);
                zigzag(d->numbers[i]);
            }
        }

        JAI_NOINLINE void write_destructuring_decl(const ast_node* n) {
            auto* d = static_cast<const destructuring_decl*>(n);
            varint(d->names.size());
            for (const auto& [nm, id] : d->names) { str(nm); symbol(id, UINT64_MAX); }
            varint(d->slot_indices.size());
            for (size_t s : d->slot_indices) slot(s);
            flag(d->is_const);
            node(d->initializer.get());
        }

        const string_symbolizer& symbols_;
        bool include_locations_ = true;
        std::string own_path_;
        std::vector<uint8_t> body_;
        std::vector<std::string> table_;
        std::unordered_map<std::string, uint64_t> table_lookup_;
        native_stack_mark stack_;   // armed where the writer is made: the thread that walks the tree
    };

    // Only the trusted in-memory parallel body copier supplies this context. A
    // public saved program always starts without locals or an enclosing frame.
    struct ast_frame_context {
        size_t local_count;
        const std::vector<parameter>& parameters;
        // A converted closure's body copy: its slots from remap_from up move by remap_shift, and an identifier naming a
        // captured symbol takes that capture's slot
        size_t remap_from = SIZE_MAX;
        size_t remap_shift = 0;
        const std::unordered_map<uint64_t, size_t>* capture_slots = nullptr;
    };

    class ast_reader {
    public:
        ast_reader(const uint8_t* data, size_t size, engine* eng, const ast_frame_context* context = nullptr, std::string own_path = {})
            : data_(data), size_(size), eng_(eng), symbols_(engine_access::get_symbolizer(*eng)), own_path_(std::move(own_path)) {
            if (context) {
                remap_from_ = context->remap_from;
                remap_shift_ = context->remap_shift;
                capture_slots_ = context->capture_slots;
                // The copied body's own frame is known; its captured parent is
                // not part of this trusted subtree. Never used for file loading.
                frames_[0].local_count = SIZE_MAX;
                enter_frame(context->parameters);
                frames_[current_frame_].local_count = context->local_count;
                frames_[current_frame_].supplied = true;
            }
        }

        // the function frames the read entered (the context's own frame included): a copied body holding a nested function has more
        size_t frame_count() const noexcept { return frames_.size(); }

        std::vector<declaration_ptr> deserialize(uint64_t& fingerprint_out, uint32_t* flags_out = nullptr, jaibite_context* context_out = nullptr) {
            if (size_ < k_jaibite_header_size) jaibite_fail("truncated header");
            if (std::memcmp(data_, k_jaibite_magic, 4) != 0) jaibite_fail("not a jaibite file (bad magic)");
            pos_ = 4;
            uint32_t version = fixed_u32();
            if (version != k_jaibite_version)
                jaibite_fail("unsupported format version " + std::to_string(version) +
                             " (expected " + std::to_string(k_jaibite_version) + ")");
            uint32_t flags = fixed_u32();
            if (flags_out) *flags_out = flags;
            fingerprint_out = fixed_u64();
            pos_ += 16;   // the source identity, which read_jaibite_header reads

            uint64_t table_count = varint();
            check_count(table_count);
            for (uint64_t i = 0; i < table_count; ++i) {
                uint64_t len = varint();
                if (len > remaining()) jaibite_fail("truncated string table");
                table_.emplace_back(reinterpret_cast<const char*>(data_ + pos_), size_t(len));
                pos_ += size_t(len);
            }
            interned_.assign(table_.size(), { UINT64_MAX, std::string_view{} });
            if (flags & k_jaibite_flag_context) {
                jaibite_context context;
                const auto names = [this](std::vector<std::string>& out, std::string* answers) {
                    const uint64_t count = varint();
                    check_count(count);
                    for (uint64_t i = 0; i < count; ++i) {
                        const uint64_t index = varint();
                        if (index >= table_.size()) jaibite_fail("context name out of range");
                        out.emplace_back(table_[size_t(index)]);
                        if (!answers) { continue; }
                        const uint8_t answer = u8();
                        if (answer > 1) jaibite_fail("corrupt namespace answer");
                        answers->push_back(answer ? '1' : '0');
                    }
                };
                names(context.namespace_queries, &context.namespace_answers);
                names(context.outside, nullptr);
                context.outside_key = fixed_u64();
                if (context_out) *context_out = std::move(context);
            }

            uint64_t decl_count = varint();
            check_count(decl_count);
            std::vector<declaration_ptr> decls;
            for (uint64_t i = 0; i < decl_count; ++i) decls.push_back(decl_req());
            if (remaining() != 0) jaibite_fail("trailing bytes after declarations");
            const auto valid = validate_program();
            if (!valid) { jaibite_fail(std::string(valid.message())); }
            return decls;
        }

    private:
        struct frame_record {
            size_t parent = 0;
            size_t local_count = 0;
            std::unordered_set<uint64_t> declared;
            bool allows_parent_slots = true;
            bool supplied = false;   // the copier's context frame, whose count the caller knows
            size_t slots_read = 0;   // the slots the frame's own syntax names
        };
        struct slot_record {
            size_t index;
            size_t frame;
            uint64_t symbol;
            bool identifier;
        };

        void track_slot(size_t index, uint64_t symbol, bool identifier = false, bool declares = false) {
            if (index == SIZE_MAX) { return; }
            ++frames_[current_frame_].slots_read;
            slots_.push_back({index, current_frame_, symbol, identifier});
            if (declares) { frames_[current_frame_].declared.insert(symbol); }
        }

        void enter_frame(const std::vector<parameter>& params, bool allows_parent_slots = true) {
            const size_t parent = current_frame_;
            current_frame_ = frames_.size();
            frames_.push_back({parent, 0, {}, allows_parent_slots});
            for (const auto& p : params) { track_slot(p.slot_index, p.symbol_id, false, true); }
        }

        // What a break and a continue may leave from the body being read: a loop gives both, a switch a break, a parallel_for
        // body only a continue (its iterations are independent), and a function's body starts from neither
        struct jump_scope {
            ast_reader& reader;
            int breakable;
            int continuable;
            jump_scope(ast_reader& a_reader, int a_breakable, int a_continuable) : reader(a_reader), breakable(a_reader.breakable_), continuable(a_reader.continuable_) {
                reader.breakable_ = a_breakable;
                reader.continuable_ = a_continuable;
            }
            jump_scope(const jump_scope&) = delete;
            ~jump_scope() { reader.breakable_ = breakable; reader.continuable_ = continuable; }
        };
        jump_scope loop_body() { return jump_scope(*this, breakable_ + 1, continuable_ + 1); }
        jump_scope switch_body() { return jump_scope(*this, breakable_ + 1, continuable_); }
        jump_scope parallel_body() { return jump_scope(*this, 0, 1); }
        jump_scope function_body() { return jump_scope(*this, 0, 0); }

        void leave_frame(size_t local_count) {
            frames_[current_frame_].local_count = local_count;
            current_frame_ = frames_[current_frame_].parent;
        }

        // The traversal records every slot with its lexical frame; counts follow bodies on the wire, so the records are
        // validated together, before any marking, checking or compiling pass.
        checked_result<void> validate_program() const {
            for (size_t i = 1; i < frames_.size(); ++i) {
                // Each parser-allocated slot is named by encoded syntax; the slack admits the synthetic slots a lowering adds, so a
                // count never claims storage the file does not describe
                if (!frames_[i].supplied && frames_[i].local_count > 2 * frames_[i].slots_read + 64) {
                    return checked_result<void>(make_error_code(runtime_error_code::invalid_operation),
                        "invalid program: local count exceeds encoded storage");
                }
            }
            for (const auto& slot : slots_) {
                size_t frame = slot.frame;
                if (slot.identifier && frames_[frame].declared.find(slot.symbol) == frames_[frame].declared.end()) {
                    // A parallel body crosses the parser's slot lookup boundary:
                    // foreign names use environments, never the caller's slots.
                    if (!frames_[frame].allows_parent_slots) {
                        return checked_result<void>(make_error_code(runtime_error_code::invalid_operation),
                            "invalid program: local slot out of range");
                    }
                    // As the callable/capture classifier reads it: an identifier not declared in this frame may carry its
                    // immediate parent's slot (a saved SIZE_MAX patch is never tracked)
                    frame = frames_[frame].parent;
                }
                if (slot.index >= frames_[frame].local_count) {
                    return checked_result<void>(make_error_code(runtime_error_code::invalid_operation),
                        "invalid program: local slot out of range");
                }
            }
            return {};
        }

        size_t remaining() const { return size_ - pos_; }
        void need(size_t n) { if (remaining() < n) jaibite_fail("unexpected end of data"); }
        void check_count(uint64_t n) { if (n > remaining()) jaibite_fail("corrupt count"); }

        uint8_t u8() { need(1); return data_[pos_++]; }
        uint32_t fixed_u32() {
            need(4);
            uint32_t x = 0;
            for (int i = 0; i < 4; ++i) x |= uint32_t(data_[pos_++]) << (i * 8);
            return x;
        }
        uint64_t fixed_u64() {
            need(8);
            uint64_t x = 0;
            for (int i = 0; i < 8; ++i) x |= uint64_t(data_[pos_++]) << (i * 8);
            return x;
        }
        uint64_t varint() {
            uint64_t x = 0;
            for (int shift = 0; shift < 64; shift += 7) {
                uint8_t b = u8();
                if (shift == 63 && (b & 0xFE)) jaibite_fail("corrupt varint");
                x |= uint64_t(b & 0x7F) << shift;
                if (!(b & 0x80)) return x;
            }
            jaibite_fail("corrupt varint");
        }
        int64_t zigzag() {
            uint64_t x = varint();
            return int64_t(x >> 1) ^ -int64_t(x & 1);
        }
        double f64() {
            uint64_t bits = fixed_u64();
            double d; std::memcpy(&d, &bits, 8);
            return d;
        }
        bool flag() { return u8() != 0; }

        const std::string& str() {
            uint64_t idx = varint();
            if (idx >= table_.size()) jaibite_fail("corrupt string index");
            return table_[size_t(idx)];
        }
        // Interned view into the TARGET engine's symbolizer (AST string_views must be permanent).
        std::pair<uint64_t, std::string_view> interned(uint64_t idx) {
            if (idx >= table_.size()) jaibite_fail("corrupt string index");
            auto& slot = interned_[size_t(idx)];
            if (slot.second.data() == nullptr)
                slot = symbols_->intern_with_view(table_[size_t(idx)]);
            return slot;
        }
        std::string_view view() { return interned(varint()).second; }
        uint64_t symbol(uint64_t sentinel) {
            uint64_t v = varint();
            if (v == 0) return sentinel;
            return interned(v - 1).first;
        }
        size_t slot_val() {
            uint64_t v = varint();
            if (v == 0) { return SIZE_MAX; }
            const size_t slot = size_t(v - 1);
            return slot >= remap_from_ ? slot + remap_shift_ : slot;
        }

        JAI_NOINLINE source_location location() {
            source_location loc;
            const std::string& name = str();
            loc.filename = name == k_jaibite_own_file ? own_path_ : name;
            loc.line = size_t(varint());
            loc.column = size_t(varint());
            return loc;
        }

        type_info_ptr type() {
            if (!u8()) return type_info_ptr(nullptr);
            uint8_t bt = u8();
            if (bt > uint8_t(k_last_script_value_type)) jaibite_fail("corrupt type_info");
            if (stack_.low()) jaibite_fail("type nesting too deep");
            const std::string& name = str();
            const size_t native_size = size_t(varint());
            const bool is_signed = flag();
            uint64_t n = varint();
            check_count(n);
            std::vector<type_info_ptr> params;
            for (uint64_t i = 0; i < n; ++i) params.push_back(type());
            return interned_type(static_cast<script_value_type>(bt), name, native_size, is_signed, std::move(params));
        }

        // a type_info is large: it is built in a frame of its own, never in one of the walk's
        JAI_NOINLINE type_info_ptr interned_type(script_value_type base, const std::string& name, size_t native_size, bool is_signed,
                                                 std::vector<type_info_ptr> params) {
            const uint64_t n = params.size();
            type_info temp(base);
            temp.type_name = name;
            temp.native_size = native_size;
            temp.is_signed = is_signed;
            temp.type_params = std::move(params);
            // Check the encoded shape before interning: primitive/container keys
            // deliberately ignore surplus parameters, which would hide corruption.
            bool valid_arity = true;
            switch (temp.base_type) {
                case script_value_type::jai_array_type:
                case script_value_type::jai_reference_type:
                case script_value_type::jai_weak_ptr_type:
                case script_value_type::jai_shared_ptr_type:
                    valid_arity = n == 1;
                    break;
                case script_value_type::jai_map_type:
                    valid_arity = n == 2;
                    break;
                case script_value_type::jai_object_type:
                case script_value_type::jai_function_type:
                    break;  // Host template/signature placeholders may be empty.
                case script_value_type::jai_cpp_bound_type:
                    valid_arity = n <= 1;
                    break;
                default:
                    valid_arity = n == 0;
                    break;
            }
            if (!valid_arity) jaibite_fail("invalid program: type parameter count");
            temp.id = engine_access::symbolize(*eng_, temp.canonical_name());
            return type_info_ptr(engine_access::get_type_info(*eng_, temp));
        }

        script_value literal() {
            switch (u8()) {
                case 0: return script_value(script_value::ast_literal_tag{}, std::monostate{});
                case 1: return script_value(script_value::ast_literal_tag{}, script_int(zigzag()));
                case 2: return script_value(script_value::ast_literal_tag{}, script_float(f64()));
                case 3: return script_value(script_value::ast_literal_tag{}, script_string(str()));
                case 4: return script_value(script_value::ast_literal_tag{}, script_char(u8()));
                case 5: return script_value(script_value::ast_literal_tag{}, script_bool(flag()));
                default: jaibite_fail("corrupt literal value");
            }
        }

        token tok() {
            uint8_t tt = u8();
            if (tt > uint8_t(k_last_token_type)) jaibite_fail("corrupt token type");
            token t;
            t.type = static_cast<token_type>(tt);
            t.lexeme = view();
            t.symbol_id = symbol(0);
            t.location = location();
            switch (t.type) {
                case token_type::integer_literal: t.int_value = zigzag(); break;
                case token_type::float_literal: t.float_value = f64(); break;
                case token_type::boolean_literal: t.bool_value = flag(); break;
                case token_type::char_literal: t.char_value = script_char(u8()); break;
                case token_type::string_literal: t.string_value = str(); break;
                case token_type::plus: t.joins_text = flag(); break;
                default: break;
            }
            return t;
        }

        std::vector<parameter> parameters() {
            uint64_t n = varint();
            check_count(n);
            std::vector<parameter> params;
            for (uint64_t i = 0; i < n; ++i) {
                type_info_ptr t = type();
                std::string name = str();
                bool is_ref = flag();
                bool is_const = flag();
                parameter p(t, name, is_ref, is_const);
                p.symbol_id = symbol(UINT64_MAX);
                p.slot_index = slot_val();
                p.default_value = expr_opt();
                params.push_back(std::move(p));
            }
            return params;
        }

        void outer_slot_plan(std::vector<std::pair<uint64_t, size_t>>& plan, bool& built) {
            built = flag();
            uint64_t n = varint();
            check_count(n);
            for (uint64_t i = 0; i < n; ++i) {
                uint64_t sym = symbol(UINT64_MAX);
                size_t s = slot_val();
                plan.emplace_back(sym, s);
                track_slot(s, sym);
            }
        }

        expression_ptr expr_opt() {
            auto n = node();
            if (!n) return nullptr;
            if (n->get_type() >= node_type::expression_stmt) jaibite_fail("expected expression node");
            return std::static_pointer_cast<expression>(n);
        }
        JAI_FORCEINLINE expression_ptr expr_req() { return required_expression(node()); }
        JAI_NOINLINE expression_ptr required_expression(ast_node_ptr n) {
            if (!n) jaibite_fail("missing required expression node");
            if (n->get_type() >= node_type::expression_stmt) jaibite_fail("expected expression node");
            return std::static_pointer_cast<expression>(std::move(n));
        }
        statement_ptr stmt_opt() {
            auto n = node();
            if (!n) return nullptr;
            // a case label is a switch's own node, never a statement of its own
            if (n->get_type() < node_type::expression_stmt || n->get_type() == node_type::case_stmt || n->get_type() == node_type::default_stmt) {
                jaibite_fail("expected statement node");
            }
            return std::static_pointer_cast<statement>(n);
        }
        JAI_FORCEINLINE statement_ptr stmt_req() { return required_statement(node()); }
        JAI_NOINLINE statement_ptr required_statement(ast_node_ptr n) {
            if (!n) jaibite_fail("missing required statement node");
            if (n->get_type() < node_type::expression_stmt || n->get_type() == node_type::case_stmt || n->get_type() == node_type::default_stmt) {
                jaibite_fail("expected statement node");
            }
            return std::static_pointer_cast<statement>(std::move(n));
        }
        declaration_ptr decl_opt() {
            auto n = node();
            if (!n) return nullptr;
            if (n->get_type() < node_type::variable_decl) jaibite_fail("expected declaration node");
            return std::static_pointer_cast<declaration>(n);
        }
        JAI_FORCEINLINE declaration_ptr decl_req() { return required_declaration(node()); }
        JAI_NOINLINE declaration_ptr required_declaration(ast_node_ptr n) {
            if (!n) jaibite_fail("missing required declaration node");
            if (n->get_type() < node_type::variable_decl) jaibite_fail("expected declaration node");
            return std::static_pointer_cast<declaration>(std::move(n));
        }
        std::shared_ptr<block_stmt> block_opt() {
            auto n = node();
            if (!n) return nullptr;
            if (n->get_type() != node_type::block_stmt) jaibite_fail("expected block statement node");
            return std::static_pointer_cast<block_stmt>(n);
        }
        std::shared_ptr<block_stmt> block_req() {
            auto result = block_opt();
            if (!result) jaibite_fail("missing required block statement node");
            return result;
        }

        using construct_reader = ast_node_ptr (ast_reader::*)(const source_location&);
        static constexpr size_t k_construct_tags = 64;
        // node() calls a construct's reader through this table rather than through a dispatch frame of its own
        static constexpr std::array<construct_reader, k_construct_tags> construct_readers() {
            std::array<construct_reader, k_construct_tags> result{};
            result[size_t(node_type::literal_expr)] = &ast_reader::read_literal_expr;
            result[size_t(node_type::identifier_expr)] = &ast_reader::read_identifier_expr;
            result[size_t(node_type::binary_expr)] = &ast_reader::read_binary_expr;
            result[size_t(node_type::unary_expr)] = &ast_reader::read_unary_expr;
            result[size_t(node_type::assignment_expr)] = &ast_reader::read_assignment_expr;
            result[size_t(node_type::call_expr)] = &ast_reader::read_call_expr;
            result[size_t(node_type::member_expr)] = &ast_reader::read_member_expr;
            result[size_t(node_type::lambda_expr)] = &ast_reader::read_lambda_expr;
            result[size_t(node_type::new_expr)] = &ast_reader::read_new_expr;
            result[size_t(node_type::ternary_expr)] = &ast_reader::read_ternary_expr;
            result[size_t(node_type::array_literal_expr)] = &ast_reader::read_array_literal_expr;
            result[size_t(node_type::map_literal_expr)] = &ast_reader::read_map_literal_expr;
            result[size_t(node_type::this_expr)] = &ast_reader::read_this_expr;
            result[size_t(node_type::super_expr)] = &ast_reader::read_super_expr;
            result[size_t(node_type::throw_expr)] = &ast_reader::read_throw_expr;
            result[size_t(node_type::yield_expr)] = &ast_reader::read_yield_expr;
            result[size_t(node_type::cast_expr)] = &ast_reader::read_cast_expr;
            result[size_t(node_type::expression_stmt)] = &ast_reader::read_expression_stmt;
            result[size_t(node_type::block_stmt)] = &ast_reader::read_block_stmt;
            result[size_t(node_type::if_stmt)] = &ast_reader::read_if_stmt;
            result[size_t(node_type::while_stmt)] = &ast_reader::read_while_stmt;
            result[size_t(node_type::for_stmt)] = &ast_reader::read_for_stmt;
            result[size_t(node_type::range_for_stmt)] = &ast_reader::read_range_for_stmt;
            result[size_t(node_type::parallel_for_stmt)] = &ast_reader::read_parallel_for_stmt;
            result[size_t(node_type::return_stmt)] = &ast_reader::read_return_stmt;
            result[size_t(node_type::break_stmt)] = &ast_reader::read_break_stmt;
            result[size_t(node_type::continue_stmt)] = &ast_reader::read_continue_stmt;
            result[size_t(node_type::try_stmt)] = &ast_reader::read_try_stmt;
            result[size_t(node_type::switch_stmt)] = &ast_reader::read_switch_stmt;
            result[size_t(node_type::case_stmt)] = &ast_reader::read_case_stmt;
            result[size_t(node_type::default_stmt)] = &ast_reader::read_default_stmt;
            result[size_t(node_type::fallthrough_stmt)] = &ast_reader::read_fallthrough_stmt;
            result[size_t(node_type::variable_decl)] = &ast_reader::read_variable_decl;
            result[size_t(node_type::function_decl)] = &ast_reader::read_function_decl;
            result[size_t(node_type::class_decl)] = &ast_reader::read_class_decl;
            result[size_t(node_type::namespace_decl)] = &ast_reader::read_namespace_decl;
            result[size_t(node_type::expression_decl)] = &ast_reader::read_expression_decl;
            result[size_t(node_type::statement_decl)] = &ast_reader::read_statement_decl;
            result[size_t(node_type::include_decl)] = &ast_reader::read_include_decl;
            result[size_t(node_type::include_expr)] = &ast_reader::read_include_expr;
            result[size_t(node_type::import_decl)] = &ast_reader::read_import_decl;
            result[size_t(node_type::import_expr)] = &ast_reader::read_import_expr;
            result[size_t(node_type::extern_decl)] = &ast_reader::read_extern_decl;
            result[size_t(node_type::enum_decl)] = &ast_reader::read_enum_decl;
            result[size_t(node_type::destructuring_decl)] = &ast_reader::read_destructuring_decl;
            return result;
        }

        ast_node_ptr node() {
            uint8_t tag = u8();
            if (tag == 0xFF) return nullptr;
            if (stack_.low()) jaibite_fail("AST nesting too deep");
            source_location loc = location();
            const uint8_t check_mode_attr = tag >= uint8_t(node_type::expression_stmt) ? u8() : 0;
            if (check_mode_attr > check_mode_attr_strict) jaibite_fail("corrupt check mode attribute");
            static constexpr std::array<construct_reader, k_construct_tags> readers = construct_readers();
            if (tag >= k_construct_tags || !readers[tag]) corrupt_node_tag(static_cast<node_type>(tag));
            ast_node_ptr result = (this->*readers[tag])(loc);
            if (check_mode_attr && result) static_cast<statement*>(result.get())->check_mode_attr = check_mode_attr;
            return result;
        }


        [[noreturn]] JAI_NOINLINE static void corrupt_node_tag(node_type kind) { jaibite_fail("corrupt node tag " + std::to_string(int(kind))); }

        JAI_NOINLINE ast_node_ptr read_break_stmt(const source_location& loc) {
            if (breakable_ == 0) jaibite_fail("invalid program: 'break' outside a loop or switch");
            return std::make_shared<break_stmt>(loc);
        }

        JAI_NOINLINE ast_node_ptr read_continue_stmt(const source_location& loc) {
            if (continuable_ == 0) jaibite_fail("invalid program: 'continue' outside a loop");
            return std::make_shared<continue_stmt>(loc);
        }

        JAI_NOINLINE ast_node_ptr read_fallthrough_stmt(const source_location& loc) { return std::make_shared<fallthrough_stmt>(loc); }

        JAI_NOINLINE ast_node_ptr read_expression_stmt(const source_location& loc) {
            return std::make_shared<expression_stmt>(loc, expr_req());
        }

        JAI_NOINLINE ast_node_ptr read_return_stmt(const source_location& loc) {
            return std::make_shared<return_stmt>(loc, expr_opt());
        }

        JAI_NOINLINE ast_node_ptr read_statement_decl(const source_location& loc) {
            return std::make_shared<statement_decl>(loc, stmt_req());
        }

        JAI_NOINLINE ast_node_ptr read_literal_expr(const source_location& loc) {
            auto e = std::make_shared<literal_expr>(loc, literal());
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_identifier_expr(const source_location& loc) {
            std::string_view name = view();
            uint64_t sym = symbol(UINT64_MAX);
            auto e = std::make_shared<identifier_expr>(loc, name, sym);
            e->slot_index = slot_val();
            e->global_qualified = flag();
            if (capture_slots_ && !e->global_qualified) {
                if (auto it = capture_slots_->find(sym); it != capture_slots_->end()) { e->slot_index = it->second; }
            }
            track_slot(e->slot_index, sym, true);
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_binary_expr(const source_location& loc) {
            ast_node_ptr result = binary_head(loc, expr_req());
            static_cast<binary_expr*>(result.get())->right = expr_req();
            return result;
        }

        JAI_NOINLINE ast_node_ptr binary_head(const source_location& loc, expression_ptr left) {
            return std::make_shared<binary_expr>(loc, std::move(left), tok(), nullptr);
        }

        // an operator's token is read into its node before the operand after it: no token lives in a frame across the walk
        JAI_NOINLINE ast_node_ptr read_unary_expr(const source_location& loc) {
            ast_node_ptr result = unary_head(loc);
            auto* e = static_cast<unary_expr*>(result.get());
            e->operand = expr_req();
            e->is_postfix = flag();
            return result;
        }

        JAI_NOINLINE ast_node_ptr unary_head(const source_location& loc) {
            return std::make_shared<unary_expr>(loc, tok(), nullptr);
        }

        JAI_NOINLINE ast_node_ptr read_assignment_expr(const source_location& loc) {
            auto e = assignment_head(loc, expr_req());
            e->value = expr_req();
            { const uint8_t fl = u8(); e->postfix_old_result = (fl & 1) != 0; e->incdec_desugar = (fl & 2) != 0; e->yields_reference = (fl & 4) != 0; }
            return e;
        }

        JAI_NOINLINE std::shared_ptr<assignment_expr> assignment_head(const source_location& loc, expression_ptr target) {
            return std::make_shared<assignment_expr>(loc, std::move(target), tok(), nullptr);
        }

        JAI_NOINLINE ast_node_ptr read_call_expr(const source_location& loc) {
            auto callee = expr_req();
            auto e = std::make_shared<call_expr>(loc, callee, expr_list());
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_member_expr(const source_location& loc) {
            auto obj = expr_req();
            std::string_view member = view();
            uint64_t member_id = symbol(UINT64_MAX);
            uint64_t getter_id = symbol(UINT64_MAX);
            bool arrow = flag();
            bool is_static = flag();
            auto e = std::make_shared<member_expr>(loc, obj, member, member_id, arrow, is_static);
            e->getter_id = getter_id;
            e->null_safe = flag();
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_lambda_expr(const source_location& loc) {
            auto e = std::make_shared<lambda_expr>(loc);
            uint8_t cd = u8();
            if (cd > uint8_t(lambda_expr::capture_default::by_reference)) jaibite_fail("corrupt capture default");
            e->default_capture = static_cast<lambda_expr::capture_default>(cd);
            uint64_t nc = varint();
            check_count(nc);
            for (uint64_t i = 0; i < nc; ++i) {
                std::string_view cname = view();
                bool by_ref = flag();
                lambda_expr::capture c(cname, by_ref);
                c.symbol_id = symbol(UINT64_MAX);
                c.outer_slot = slot_val();
                track_slot(c.outer_slot, c.symbol_id);
                c.initializer = expr_opt();
                e->captures.push_back(std::move(c));
            }
            e->parameters = parameters();
            e->return_type = type();
            enter_frame(e->parameters);
            {
                auto body = function_body();
                e->body = block_req();
            }
            e->local_count = size_t(varint());
            leave_frame(e->local_count);
            bool built = false;
            outer_slot_plan(e->outer_slot_plan, built);
            e->outer_slot_plan_built = built;
            uint64_t nchain = varint();
            check_count(nchain);
            for (uint64_t i = 0; i < nchain; ++i) {
                uint64_t sym = symbol(UINT64_MAX);
                size_t s = slot_val();
                e->chained_outer_slots.emplace_back(sym, s);
                track_slot(s, sym);
            }
            uint64_t nnames = varint();
            check_count(nnames);
            for (uint64_t i = 0; i < nnames; ++i) e->chained_outer_names.push_back(symbol(UINT64_MAX));
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_new_expr(const source_location& loc) {
            auto t = type();
            auto e = std::make_shared<new_expr>(loc, t, expr_list());
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_ternary_expr(const source_location& loc) {
            auto c = expr_req();
            auto t = expr_req();
            auto f = expr_req();
            auto e = std::make_shared<ternary_expr>(loc, c, t, f);
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_array_literal_expr(const source_location& loc) {
            auto e = std::make_shared<array_literal_expr>(loc, expr_list());
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_map_literal_expr(const source_location& loc) {
            uint64_t n = varint();
            check_count(n);
            std::vector<std::pair<expression_ptr, expression_ptr>> entries;
            for (uint64_t i = 0; i < n; ++i) {
                auto k = expr_req();
                auto v = expr_req();
                entries.emplace_back(k, v);
            }
            auto e = std::make_shared<map_literal_expr>(loc, std::move(entries));
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_this_expr(const source_location& loc) {
            auto e = std::make_shared<this_expr>(loc);
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_super_expr(const source_location& loc) {
            auto e = std::make_shared<super_expr>(loc);
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_throw_expr(const source_location& loc) {
            auto e = std::make_shared<throw_expr>(loc, expr_opt());
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_yield_expr(const source_location& loc) {
            auto e = std::make_shared<yield_expr>(loc, expr_opt());
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_cast_expr(const source_location& loc) {
            auto target = type();
            const uint8_t cast_flags = u8();
            auto e = std::make_shared<cast_expr>(loc, target, (cast_flags & 1) != 0, expr_req());
            e->implicit_ref = (cast_flags & 2) != 0;
            e->binding_ref = (cast_flags & 4) != 0;
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_block_stmt(const source_location& loc) {
            uint64_t n = varint();
            check_count(n);
            std::vector<declaration_ptr> decls;
            for (uint64_t i = 0; i < n; ++i) decls.push_back(decl_req());
            return std::make_shared<block_stmt>(loc, std::move(decls));
        }

        JAI_NOINLINE ast_node_ptr read_if_stmt(const source_location& loc) {
            auto c = expr_req();
            auto t = stmt_req();
            auto e = stmt_opt();
            return std::make_shared<if_stmt>(loc, c, t, e);
        }

        JAI_NOINLINE ast_node_ptr read_while_stmt(const source_location& loc) {
            auto c = expr_req();
            auto b = [&] { auto body = loop_body(); return stmt_req(); }();
            return std::make_shared<while_stmt>(loc, c, b);
        }

        JAI_NOINLINE ast_node_ptr read_for_stmt(const source_location& loc) {
            auto i = decl_opt();
            if (i && i->get_type() != node_type::variable_decl &&
                i->get_type() != node_type::expression_decl) {
                jaibite_fail("invalid program: for initializer must be a variable or expression declaration");
            }
            auto c = expr_opt();
            auto u = expr_opt();
            auto b = [&] { auto body = loop_body(); return stmt_req(); }();
            return std::make_shared<for_stmt>(loc, i, c, u, b);
        }

        JAI_NOINLINE ast_node_ptr read_range_for_stmt(const source_location& loc) {
            auto et = type();
            std::string_view vname = view();
            uint64_t vid = symbol(UINT64_MAX);
            size_t vslot = slot_val();
            bool is_ref = flag();
            bool is_const = flag();
            auto cont = expr_req();
            auto body = [&] { auto scope = loop_body(); return stmt_req(); }();
            auto s = std::make_shared<range_for_stmt>(loc, et, vname, vid, is_ref, is_const, cont, body);
            s->variable_slot_index = vslot;
            track_slot(vslot, vid, false, true);
            stamp_map_entry_bindings(*s, symbols_);
            return s;
        }

        JAI_NOINLINE ast_node_ptr read_parallel_for_stmt(const source_location& loc) {
            auto params = parameters();
            if (params.empty()) jaibite_fail("invalid program: parallel_for has no loop variable");
            size_t locals = size_t(varint());
            const uint64_t frame_capture_count = varint();
            check_count(frame_capture_count);
            std::vector<std::pair<uint64_t, size_t>> frame_captures;
            for (uint64_t i = 0; i < frame_capture_count; ++i) {
                const uint64_t sym = symbol(UINT64_MAX);
                const size_t slot = size_t(varint());
                track_slot(slot, sym);
                frame_captures.emplace_back(sym, slot);
            }
            auto cont = expr_req();
            auto extras = expr_list();
            if (extras.size() + 1 != params.size()) jaibite_fail("invalid program: parallel_for zips one array per loop variable");
            for (size_t i = 0; i < params.size(); ++i) {
                if (params[i].slot_index != i) {
                    jaibite_fail("invalid program: parallel parameter slot does not match its position");
                }
            }
            enter_frame(params, false);
            auto body = [&] { auto scope = parallel_body(); return block_req(); }();
            leave_frame(locals);
            auto s = std::make_shared<parallel_for_stmt>(loc, std::move(params[0]), cont, body);
            s->extra_params.assign(params.begin() + 1, params.end());
            s->extra_containers = std::move(extras);
            s->local_count = locals;
            s->frame_captures = std::move(frame_captures);
            return s;
        }

        JAI_NOINLINE ast_node_ptr read_try_stmt(const source_location& loc) {
            auto try_blk = block_req();
            std::string catch_var = str();
            uint64_t catch_id = symbol(UINT64_MAX);
            size_t catch_slot = slot_val();
            auto catch_blk = block_req();
            auto s = std::make_shared<try_stmt>(loc, try_blk, catch_blk, catch_var);
            s->catch_var_id = catch_id;
            s->catch_slot_index = catch_slot;
            track_slot(catch_slot, catch_id, false, true);
            return s;
        }

        JAI_NOINLINE ast_node_ptr read_switch_stmt(const source_location& loc) {
            auto s = std::make_shared<switch_stmt>(loc, expr_req());
            auto cases = switch_body();
            uint64_t n = varint();
            check_count(n);
            for (uint64_t i = 0; i < n; ++i) {
                auto c = node();
                if (!c || c->get_type() != node_type::case_stmt) jaibite_fail("expected case statement node");
                s->cases.push_back(std::static_pointer_cast<case_stmt>(c));
            }
            auto def = node();
            if (def) {
                if (def->get_type() != node_type::default_stmt) jaibite_fail("expected default statement node");
                s->default_case = std::static_pointer_cast<default_stmt>(def);
            }
            return s;
        }

        JAI_NOINLINE ast_node_ptr read_case_stmt(const source_location& loc) {
            auto s = std::make_shared<case_stmt>(loc, expr_req());
            uint64_t n = varint();
            check_count(n);
            for (uint64_t i = 0; i < n; ++i) s->body.push_back(stmt_req());
            s->has_fallthrough = flag();
            bool terminal_fallthrough = false;
            for (size_t i = 0; i < s->body.size(); ++i) {
                if (s->body[i]->get_type() == node_type::fallthrough_stmt) {
                    if (i + 1 != s->body.size()) {
                        jaibite_fail("invalid program: inconsistent case fallthrough");
                    }
                    terminal_fallthrough = true;
                }
            }
            if (s->has_fallthrough != terminal_fallthrough) {
                jaibite_fail("invalid program: inconsistent case fallthrough");
            }
            return s;
        }

        JAI_NOINLINE ast_node_ptr read_default_stmt(const source_location& loc) {
            auto s = std::make_shared<default_stmt>(loc);
            uint64_t n = varint();
            check_count(n);
            for (uint64_t i = 0; i < n; ++i) s->body.push_back(stmt_req());
            return s;
        }

        JAI_NOINLINE ast_node_ptr read_variable_decl(const source_location& loc) {
            auto t = type();
            std::string_view name = view();
            uint64_t nid = symbol(UINT64_MAX);
            auto init = expr_opt();
            if (!init && t && t->base_type == script_value_type::jai_reference_type) jaibite_fail("a reference declaration without an initializer");
            auto d = std::make_shared<variable_decl>(loc, t, name, nid, init);
            d->is_static = flag();
            d->function_spelling = flag();
            d->default_initialized = flag();
            d->is_inline = flag();
            d->is_const = flag();
            d->slot_index = slot_val();
            track_slot(d->slot_index, nid, false, true);
            return d;
        }

        JAI_NOINLINE ast_node_ptr read_function_decl(const source_location& loc) {
            std::string_view name = view();
            uint64_t nid = symbol(UINT64_MAX);
            auto d = std::make_shared<function_decl>(loc, name, nid);
            d->parameters = parameters();
            d->return_type = type();
            enter_frame(d->parameters);
            {
                auto body = function_body();
                d->body = block_req();
            }
            // Constructor initializers, like parameter defaults, are parsed
            // in the surrounding scope, not in the function body frame.
            const size_t body_frame = current_frame_;
            current_frame_ = frames_[body_frame].parent;
            uint64_t ni = varint();
            check_count(ni);
            for (uint64_t i = 0; i < ni; ++i) {
                std::string target = str();
                d->initializers.emplace_back(target, expr_list());
            }
            d->is_override = flag();
            d->is_static = flag();
            d->is_coroutine = flag();
            d->is_nodiscard = flag();
            d->local_count = size_t(varint());
            frames_[body_frame].local_count = d->local_count;
            bool built = false;
            outer_slot_plan(d->outer_slot_plan, built);
            d->outer_slot_plan_built = built;
            return d;
        }

        JAI_NOINLINE ast_node_ptr read_class_decl(const source_location& loc) {
            std::string_view name = view();
            uint64_t nid = symbol(UINT64_MAX);
            auto d = std::make_shared<class_decl>(loc, name, nid);
            d->nocopy = u8() != 0;
            uint64_t nb = varint();
            check_count(nb);
            for (uint64_t i = 0; i < nb; ++i) d->base_classes.push_back(view());
            uint64_t nm = varint();
            check_count(nm);
            for (uint64_t i = 0; i < nm; ++i) {
                uint8_t vis = u8();
                if (vis > uint8_t(class_decl::Protected)) jaibite_fail("corrupt member visibility");
                auto member_decl = decl_req();
                if (member_decl->get_type() != node_type::variable_decl &&
                    member_decl->get_type() != node_type::function_decl) {
                    jaibite_fail("invalid program: class member must be a variable or function declaration");
                }
                d->members.push_back({ static_cast<class_decl::member_visibility>(vis), member_decl });
            }
            return d;
        }

        JAI_NOINLINE ast_node_ptr read_namespace_decl(const source_location& loc) {
            std::string_view name = view();
            uint64_t nid = symbol(UINT64_MAX);
            auto d = std::make_shared<namespace_decl>(loc, name, nid);
            uint64_t n = varint();
            check_count(n);
            for (uint64_t i = 0; i < n; ++i) d->declarations.push_back(decl_req());
            return d;
        }

        JAI_NOINLINE ast_node_ptr read_expression_decl(const source_location& loc) {
            auto e = expr_req();
            bool implicit = flag();
            return std::make_shared<expression_decl>(loc, e, implicit);
        }

        JAI_NOINLINE ast_node_ptr read_include_decl(const source_location& loc) {
            std::string path = str();
            auto d = std::make_shared<include_decl>(loc, path);
            d->path_expr = expr_opt();
            return d;
        }

        JAI_NOINLINE ast_node_ptr read_include_expr(const source_location& loc) {
            std::string path = str();
            auto e = std::make_shared<include_expr>(loc, path);
            e->path_expr = expr_opt();
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_import_decl(const source_location& loc) {
            std::string path = str();
            auto d = std::make_shared<import_decl>(loc, path);
            d->path_expr = expr_opt();
            return d;
        }

        JAI_NOINLINE ast_node_ptr read_import_expr(const source_location& loc) {
            auto e = std::make_shared<import_expr>(loc);
            const uint64_t n_paths = varint();
            check_count(n_paths);
            for (uint64_t i = 0; i < n_paths; ++i) { e->paths.push_back(str()); }
            e->path_exprs = expr_list();
            e->force = flag();
            return e;
        }

        JAI_NOINLINE ast_node_ptr read_extern_decl(const source_location& loc) {
            std::string_view name = view();
            uint64_t nid = symbol(UINT64_MAX);
            bool is_function = flag();
            bool is_const = flag();
            const auto written_at = static_cast<uint32_t>(varint());
            type_info_ptr t = type();
            auto d = std::make_shared<extern_decl>(loc, t, is_function, name, nid);
            d->is_const = is_const;
            d->written_at = written_at;
            return d;
        }

        JAI_NOINLINE ast_node_ptr read_enum_decl(const source_location& loc) {
            std::string_view name = view();
            uint64_t nid = symbol(UINT64_MAX);
            auto d = std::make_shared<enum_decl>(loc, name, nid);
            uint64_t n = varint();
            check_count(n);
            for (uint64_t i = 0; i < n; ++i) {
                std::string_view vname = view();
                uint64_t vid = symbol(UINT64_MAX);
                d->values.emplace_back(vname, vid);
                d->numbers.push_back(zigzag());
            }
            return d;
        }

        JAI_NOINLINE ast_node_ptr read_destructuring_decl(const source_location& loc) {
            std::vector<std::pair<std::string_view, uint64_t>> names;
            uint64_t n = varint();
            check_count(n);
            for (uint64_t i = 0; i < n; ++i) {
                std::string_view nm = view();
                uint64_t id = symbol(UINT64_MAX);
                names.emplace_back(nm, id);
            }
            uint64_t ns = varint();
            check_count(ns);
            if (ns != n) {
                jaibite_fail("destructuring name/slot count mismatch");
            }
            std::vector<size_t> slots;
            for (uint64_t i = 0; i < ns; ++i) slots.push_back(slot_val());
            const bool is_const = flag();
            auto d = std::make_shared<destructuring_decl>(loc, expr_req());
            d->is_const = is_const;
            d->names = std::move(names);
            d->slot_indices = std::move(slots);
            for (size_t i = 0; i < d->slot_indices.size(); ++i) {
                track_slot(d->slot_indices[i], d->names[i].second, false, true);
            }
            return d;
        }

        std::vector<expression_ptr> expr_list() {
            uint64_t n = varint();
            check_count(n);
            std::vector<expression_ptr> list;
            for (uint64_t i = 0; i < n; ++i) list.push_back(expr_req());
            return list;
        }

        const uint8_t* data_;
        size_t size_;
        size_t pos_ = 0;
        engine* eng_;
        string_symbolizer* symbols_;
        std::vector<std::string> table_;
        std::vector<std::pair<uint64_t, std::string_view>> interned_;  // lazy per-table-entry target ids
        std::vector<frame_record> frames_{frame_record{}};
        std::vector<slot_record> slots_;
        size_t current_frame_ = 0;
        native_stack_mark stack_;   // armed where the writer is made: the thread that walks the tree
        int breakable_ = 0;
        int continuable_ = 0;
        std::string own_path_;
        size_t remap_from_ = SIZE_MAX;
        size_t remap_shift_ = 0;
        const std::unordered_map<uint64_t, size_t>* capture_slots_ = nullptr;
    };

    // own_path: the unit's source path as its nodes are stamped; pass the same spelling to
    // deserialize_jaibite (the path the bite is loaded under) and the file may move.
    inline std::vector<uint8_t> serialize_jaibite(const std::vector<declaration_ptr>& decls,
                                                  const string_symbolizer& symbols, uint64_t fingerprint,
                                                  uint32_t flags = 0, std::string own_path = {},
                                                  jaibite_source_identity source = {}, const jaibite_context* context = nullptr) {
        return ast_writer(symbols, /*include_locations=*/true, std::move(own_path)).serialize(decls, fingerprint, flags, source, context);
    }

    inline std::vector<declaration_ptr> deserialize_jaibite(const uint8_t* data, size_t size,
                                                            engine* eng, uint64_t& fingerprint_out,
                                                            uint32_t* flags_out = nullptr,
                                                            const ast_frame_context* context = nullptr,
                                                            std::string own_path = {}, jaibite_context* stamp_out = nullptr) {
        return ast_reader(data, size, eng, context, std::move(own_path)).deserialize(fingerprint_out, flags_out, stamp_out);
    }

    // Position-insensitive structural identity key for one AST subtree (hot-reload
    // redefinition equality): byte-equal keys = structurally identical code, regardless
    // of line/column shifts or the surrounding source. Throws (jaibite_fail) on
    // unserializable nodes — callers treat that as "not identical".
    inline std::vector<uint8_t> structural_node_key(const ast_node* n, const string_symbolizer& symbols) {
        return ast_writer(symbols, /*include_locations=*/false).node_key(n);
    }

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_AST_SERIALIZER_HPP__
