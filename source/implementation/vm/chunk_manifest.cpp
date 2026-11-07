#include <jaiscript/vm/chunk_manifest.hpp>
#include <jaiscript/detail/regex.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace jai::vm {

namespace {

	struct manifest_writer {
		std::ostringstream out;
		const string_symbolizer* sym = nullptr;

		std::string id(uint64_t symbol_id) const {
			if (symbol_id == UINT64_MAX) return "-";
			if (!sym) return "#" + std::to_string(symbol_id);
			return esc(std::string(sym->get_string(symbol_id)));
		}
		std::string symidx(const chunk& ch, uint32_t index) const {
			if (index == k_invalid_u32) return "-";
			if (index >= ch.symbols.size()) return "?" + std::to_string(index);
			return id(ch.symbols[index]);
		}
		static std::string u(uint32_t v) { return v == k_invalid_u32 ? std::string("-") : std::to_string(v); }
		static std::string z(size_t v) { return v == SIZE_MAX ? std::string("-") : std::to_string(v); }
		static const char* b(bool v) { return v ? "1" : "0"; }
		static std::string tn(const type_info* t) { return t ? esc(t->type_name) : std::string("-"); }
		static std::string esc(const std::string& s) {
			std::string r;
			for (unsigned char c : s) {
				if (c >= 0x20 && c < 0x7F && c != '\\' && c != '|' && c != ' ') { r += static_cast<char>(c); }
				else { char h[8]; std::snprintf(h, sizeof h, "\\x%02X", c); r += h; }
			}
			return r;
		}
		static std::string hex64(uint64_t v) { char h[24]; std::snprintf(h, sizeof h, "%016llx", static_cast<unsigned long long>(v)); return h; }
		static std::string node(const ast_node* n) {
			if (!n) return "-";
			return std::to_string(static_cast<int>(n->get_type())) + "@" + std::to_string(n->location.line) + ":" + std::to_string(n->location.column);
		}
		std::string ins(const vm_instruction& i) const {
			return std::string(opcode_name(i.op)) + " " + std::to_string(i.a) + " " + std::to_string(i.b) + " " + std::to_string(i.c);
		}
		std::string constant(const script_value& v) const {
			std::string r;
			switch (v.raw_storage_index()) {
			case script_value::TYPEID_NULL: r = "null"; break;
			case script_value::TYPEID_INT: r = "i:" + std::to_string(v.unchecked_as_int()); break;
			case script_value::TYPEID_FLOAT: { const double d = v.unchecked_as_float(); uint64_t bits = 0; std::memcpy(&bits, &d, sizeof bits); r = "f:" + hex64(bits); break; }
			case script_value::TYPEID_STRING: r = "s:" + esc(std::string(v.unchecked_as_string())); break;
			case script_value::TYPEID_BOOL: r = std::string("b:") + (v.unchecked_as_bool() ? "1" : "0"); break;
			default: r = "k" + std::to_string(v.raw_storage_index()) + ":" + esc(v.to_string()); break;
			}
			return r + " tag=" + tn(v.get_type_info().get());
		}
		std::string operand(const chunk& ch, const fused_operand& o) const {
			std::string r = "{slot=" + u(o.slot) + " sym=" + symidx(ch, o.symbol) + " const=" + u(o.const_index) + " flags=" + std::to_string(o.load_flags) +
			                " ident=" + u(o.ident_node) + " member=" + u(o.member_node) + " direct=" + b(o.direct) +
			                (o.this_slot ? " this_slot=" + std::to_string(o.this_slot - 1) : std::string());
			if (o.is_subscript()) {
				r += " chain=[";
				for (const auto& s : o.subscript_chain) {
					r += "(" + std::to_string(s.kind) + " " + u(s.slot) + " " + symidx(ch, s.symbol) + " " + std::to_string(s.literal) + ")";
				}
				r += "]";
			}
			return r + "}";
		}
		std::string mask(const chunk::cell_mask& m) const { std::string s; for (uint32_t i = 0; i < chunk::cell_mask::words; ++i) { if (i) { s += ":"; } s += hex64(m.w[i]); } return s; }

		void write(const chunk& ch, const std::string& label, int depth) {
			const std::string pre(static_cast<size_t>(depth) * 2, ' ');
			out << pre << "chunk " << label << " name=" << esc(ch.function_name) << " locals=" << ch.local_count << " body=" << b(ch.is_function_body)
			    << " frame_env=" << b(ch.needs_frame_env) << " builtin_indexed=" << b(ch.builtin_indexed) << " method_env_reusable=" << b(ch.method_env_reusable)
			    << " pinned_decls=" << ch.pinned_decls.size() << "\n";
			out << pre << "code " << ch.code.size() << "\n";
			for (size_t i = 0; i < ch.code.size(); ++i) {
				out << pre << " " << i << " " << ins(ch.code[i]);
				if (i < ch.stmt_nodes.size()) { out << " stmt=" << node(ch.stmt_nodes[i]); }
				if (i < ch.ident_nodes.size()) { out << " ident=" << u(ch.ident_nodes[i]); }
				if (i < ch.this_field_slots.size() && ch.this_field_slots[i]) { out << " this_slot=" << (ch.this_field_slots[i] - 1); }
				out << "\n";
			}
			out << pre << "constants " << ch.constants.size() << "\n";
			for (size_t i = 0; i < ch.constants.size(); ++i) { out << pre << " " << i << " " << constant(ch.constants[i]) << "\n"; }
			if (!ch.regex_constants.empty()) {
				out << pre << "regex_constants " << ch.regex_constants.size() << "\n";
				for (size_t i = 0; i < ch.regex_constants.size(); ++i) {
					const auto& rc = ch.regex_constants[i];
					out << pre << " " << i << " pattern=" << esc(rc.re ? detail::regex::pattern_of(*rc.re) : std::string()) << " flags="
					    << esc(rc.re ? detail::regex::flags_of(*rc.re) : std::string()) << " argc=" << rc.argc << "\n";
				}
			}
			out << pre << "symbols " << ch.symbols.size() << "\n";
			for (size_t i = 0; i < ch.symbols.size(); ++i) { out << pre << " " << i << " " << id(ch.symbols[i]) << "\n"; }
			out << pre << "messages " << ch.messages.size() << "\n";
			for (size_t i = 0; i < ch.messages.size(); ++i) { out << pre << " " << i << " " << esc(std::string(ch.messages[i])) << "\n"; }
			out << pre << "nodes " << ch.nodes.size() << "\n";
			for (size_t i = 0; i < ch.nodes.size(); ++i) { out << pre << " " << i << " " << node(ch.nodes[i].get()) << "\n"; }
			out << pre << "call_arg_zones";
			for (const auto& z : ch.call_arg_zones) { out << " " << z.first << "-" << z.second; }
			out << "\n" << pre << "reference_slots";
			for (uint32_t s : ch.reference_slots) { out << " " << s; }
			if (!ch.debug_locals.empty()) {
				out << "\n" << pre << "debug_locals";
				for (const auto& l : ch.debug_locals) { out << " " << id(l.symbol) << "/" << l.slot << "/" << l.first_ip << "-" << u(l.end_ip); }
			}
			out << "\n" << pre << "call_sites " << ch.call_sites.size() << "\n";
			for (size_t i = 0; i < ch.call_sites.size(); ++i) {
				const call_site& s = ch.call_sites[i];
				out << pre << " " << i << " args=[";
				for (size_t k = 0; k < s.arg_symbols.size(); ++k) {
					out << (k ? " " : "") << id(s.arg_symbols[k]) << "/" << (k < s.arg_slots.size() ? u(s.arg_slots[k]) : std::string("?"))
					    << "/" << (k < s.arg_global.size() ? std::to_string(s.arg_global[k]) : std::string("?"))
					    << "/" << (k < s.arg_lvalue_nodes.size() ? u(s.arg_lvalue_nodes[k]) : std::string("-"));
				}
				out << "] staged=[";
				for (uint32_t slot : s.staged_argument_slots) { out << slot << " "; }
				out << "] member=" << u(s.member_node) << " call=" << u(s.call_node) << " recv=" << id(s.receiver_symbol) << "/" << u(s.receiver_slot)
				    << " bi=" << s.bi_array << "," << s.bi_map << "," << s.bi_string
				    << (s.in_place_check ? " in_place" : "")
				    << " callee=" << operand(ch, s.callee) << "\n";
			}
			if (!ch.switch_tables.empty()) {
				out << pre << "switch_tables " << ch.switch_tables.size() << "\n";
				for (size_t i = 0; i < ch.switch_tables.size(); ++i) {
					const chunk::switch_table& t = ch.switch_tables[i];
					out << pre << " " << i << " subject=" << operand(ch, t.subject) << " no_match=" << t.no_match << " constant=" << b(t.constant_keys) << " cases=[";
					for (size_t k = 0; k < t.keys.size(); ++k) { out << (k ? " " : "") << operand(ch, t.keys[k]) << "->" << t.targets[k]; }
					out << "]\n";
				}
			}
			out << pre << "function_protos " << ch.function_protos.size() << "\n";
			for (size_t i = 0; i < ch.function_protos.size(); ++i) {
				const function_proto& p = ch.function_protos[i];
				out << pre << " " << i << " decl=" << (p.decl ? esc(std::string(p.decl->name)) : std::string("-")) << " coroutine=" << b(p.is_coroutine) << " outer=[";
				for (const auto& o : p.outer_slot_plan) { out << "(" << id(o.first) << " " << o.second << ")"; }
				out << "]\n";
			}
			out << pre << "closure_protos " << ch.closure_protos.size() << "\n";
			for (size_t i = 0; i < ch.closure_protos.size(); ++i) {
				const closure_proto& p = ch.closure_protos[i];
				out << pre << " " << i << " lambda=" << node(p.lambda.get()) << " default_capture=" << static_cast<int>(p.default_capture) << " this=" << b(p.captures_this) << " instance=" << b(p.uses_instance) << " outer=[";
				for (const auto& o : p.outer_slot_plan) { out << "(" << id(o.first) << " " << o.second << ")"; }
				out << "] used=[";
				for (uint64_t v : p.used_variables) { out << id(v) << " "; }
				out << "] captures=[";
				for (const auto& c : p.captures) { out << "(" << id(c.symbol_id) << " " << b(c.by_reference) << " " << z(c.outer_slot) << ")"; }
				out << "]\n";
			}
			out << pre << "destructure_protos " << ch.destructure_protos.size() << "\n";
			for (size_t i = 0; i < ch.destructure_protos.size(); ++i) {
				out << pre << " " << i;
				for (const auto& n : ch.destructure_protos[i].names) { out << " (" << id(n.first) << " " << n.second << ")"; }
				out << "\n";
			}
			out << pre << "iter_protos " << ch.iter_protos.size() << "\n";
			for (size_t i = 0; i < ch.iter_protos.size(); ++i) {
				const iter_proto& p = ch.iter_protos[i];
				out << pre << " " << i << " var=" << id(p.var_symbol) << " slot=" << z(p.slot) << " ref=" << b(p.is_reference) << "\n";
			}
			out << pre << "fused_binary_protos " << ch.fused_binary_protos.size() << "\n";
			for (size_t i = 0; i < ch.fused_binary_protos.size(); ++i) {
				const fused_binary_proto& p = ch.fused_binary_protos[i];
				out << pre << " " << i << " op=" << static_cast<int>(p.op) << " class=" << static_cast<int>(p.typed_class) << " elem=" << b(p.elem_read_operand) << " release=" << static_cast<int>(p.release)
				    << " left=" << operand(ch, p.left) << " right=" << operand(ch, p.right) << "\n";
			}
			out << pre << "compound_fused_protos " << ch.compound_fused_protos.size() << "\n";
			for (size_t i = 0; i < ch.compound_fused_protos.size(); ++i) {
				const compound_fused_proto& p = ch.compound_fused_protos[i];
				out << pre << " " << i << " rhs_proto=" << u(p.rhs_proto) << " rhs=" << operand(ch, p.rhs) << " sym=" << symidx(ch, p.symbol) << " slot=" << u(p.slot) << " kind=" << p.kind_flags << "\n";
			}
			out << pre << "concat_protos " << ch.concat_protos.size() << "\n";
			for (size_t i = 0; i < ch.concat_protos.size(); ++i) {
				out << pre << " " << i << " hint=" << ch.concat_protos[i].hint << (ch.concat_protos[i].joins_text ? " joins_text" : "") << " pieces=[";
				for (const auto& o : ch.concat_protos[i].pieces) { out << operand(ch, o) << " "; }
				out << "]\n";
			}
			out << pre << "fused_binary_dst_protos " << ch.fused_binary_dst_protos.size() << "\n";
			for (size_t i = 0; i < ch.fused_binary_dst_protos.size(); ++i) {
				const fused_binary_dst_proto& p = ch.fused_binary_dst_protos[i];
				out << pre << " " << i << " binary=" << p.binary_proto << " node=" << u(p.node_index) << " sym=" << symidx(ch, p.symbol) << " slot=" << u(p.slot) << " flags=" << p.flags << "\n";
			}
			out << pre << "fused_index_protos " << ch.fused_index_protos.size() << "\n";
			for (size_t i = 0; i < ch.fused_index_protos.size(); ++i) {
				const fused_index_proto& p = ch.fused_index_protos[i];
				out << pre << " " << i << " container=" << operand(ch, p.container) << " index=" << operand(ch, p.index) << " index_binary=" << u(p.index_binary) << " chain=[";
				for (uint32_t s : p.index_chain_ext) { out << s << " "; }
				out << "] value=" << operand(ch, p.store_value) << " decl_slot=" << u(p.decl_slot) << " expect=" << static_cast<int>(p.decl_expect) << " type=" << tn(p.decl_type)
				    << " temp=" << b(p.decl_temp) << " lane=" << b(p.typed_lane) << " spec=" << b(p.speculative) << " guard=" << u(p.container_guard) << " always=" << b(p.guard_always) << " release=" << static_cast<int>(p.release);
				if (p.compound) { out << " compound=" << p.compound_kind; }
				out << "\n";
			}
			out << pre << "push_protos " << ch.push_protos.size() << "\n";
			for (size_t i = 0; i < ch.push_protos.size(); ++i) {
				const push_proto& p = ch.push_protos[i];
				out << pre << " " << i << " recv=" << operand(ch, p.receiver) << " value=" << operand(ch, p.value) << " site=" << u(p.site) << " subscript=" << b(p.value_is_subscript) << " flags=" << p.value_index_flags << " temp=" << b(p.value_temp) << "\n";
			}
			out << pre << "member_store_protos " << ch.member_store_protos.size() << "\n";
			for (size_t i = 0; i < ch.member_store_protos.size(); ++i) {
				const member_store_proto& p = ch.member_store_protos[i];
				out << pre << " " << i << " node=" << u(p.node) << " base=" << u(p.base) << " value=" << operand(ch, p.value) << " release=" << b(p.release) << "\n";
			}
			if (!ch.member_folds.empty()) {
				out << pre << "member_folds " << ch.member_folds.size() << "\n";
				for (size_t i = 0; i < ch.member_folds.size(); ++i) {
					const chunk::member_fold& m = ch.member_folds[i];
					out << pre << " " << i << " kind=" << static_cast<int>(m.kind) << " count=" << static_cast<int>(m.count) << " consumer=" << ins(m.consumer) << " first=" << ins(m.first) << "\n";
				}
			}
			if (!ch.member_this_symbols.empty()) {
				std::vector<std::pair<uint32_t, uint32_t>> rows(ch.member_this_symbols.begin(), ch.member_this_symbols.end());
				std::sort(rows.begin(), rows.end());
				out << pre << "member_this_symbols";
				for (const auto& r : rows) { out << " " << r.first << "=" << symidx(ch, r.second); }
				out << "\n";
			}
			out << pre << "velem_protos " << ch.velem_protos.size() << "\n";
			for (size_t i = 0; i < ch.velem_protos.size(); ++i) {
				const velem_proto& p = ch.velem_protos[i];
				out << pre << " " << i << " sub=" << operand(ch, p.sub) << " node=" << u(p.node) << " kind=" << p.kind << " compound=" << b(p.compound) << " comp=";
				for (int8_t c : p.comp) { out << static_cast<int>(c) << ","; }
				out << " value=" << operand(ch, p.value) << "\n";
			}
			out << pre << "member_path_protos " << ch.member_path_protos.size() << "\n";
			for (size_t i = 0; i < ch.member_path_protos.size(); ++i) {
				const member_path_proto& p = ch.member_path_protos[i];
				out << pre << " " << i << " root=" << u(p.root_slot) << " links=";
				for (const auto& l : p.links) { out << (l.subscript ? "[" + operand(ch, l.index) + " flags=" + std::to_string(l.index_flags) + "]" : std::string(".")); }
				out << "\n";
			}
			out << pre << "counted_for_protos " << ch.counted_for_protos.size() << "\n";
			for (size_t i = 0; i < ch.counted_for_protos.size(); ++i) {
				const counted_for_proto& p = ch.counted_for_protos[i];
				out << pre << " " << i << " var=" << operand(ch, p.var) << " end=" << operand(ch, p.end) << " step=" << operand(ch, p.step) << " cmp=" << static_cast<int>(p.cmp)
				    << " sub=" << b(p.step_subtract) << " incdec=" << b(p.step_incdec) << " body=" << u(p.body_ip) << " exit=" << u(p.exit_ip) << " cond=" << u(p.generic_cond_ip) << " update=" << u(p.generic_update_ip) << "\n";
			}
			out << pre << "typed_sites " << ch.typed_sites.size() << "\n";
			for (size_t i = 0; i < ch.typed_sites.size(); ++i) {
				const chunk::typed_site& s = ch.typed_sites[i];
				if (s.orig.op == opcode::op_halt && s.orig2.op == opcode::op_halt && !s.stamp && s.cont_slot == k_invalid_u32 && s.fold_count == 0 && s.index_proto == k_invalid_u32) continue;   // an empty row
				out << pre << " " << i << " orig=" << ins(s.orig) << " orig2=" << ins(s.orig2) << " stamp=" << tn(s.stamp) << " cont=" << u(s.cont_slot) << " ops=";
				for (uint32_t o : s.operands) { out << o << ","; }
				out << " fold=";
				for (uint8_t f : s.fold_ops) { out << static_cast<int>(f) << ","; }
				out << static_cast<int>(s.fold_count) << " expect=" << static_cast<int>(s.decl_expect) << " proto=" << u(s.index_proto) << " temp=" << b(s.temp) << " spec=" << b(s.speculative)
				    << " guard=" << u(s.guard_temp) << " always=" << b(s.guard_always) << " release=" << b(s.release) << "\n";
			}
			if (!ch.typed_env_operands.empty()) {
				out << pre << "typed_env_operands";
				for (const auto& o : ch.typed_env_operands) { out << " " << operand(ch, o); }
				out << "\n";
			}
			out << pre << "inline_sites " << ch.inline_sites.size() << "\n";
			for (size_t i = 0; i < ch.inline_sites.size(); ++i) {
				const chunk::inline_site& s = ch.inline_sites[i];
				out << pre << " " << i << " guard=" << s.guard_ip << " begin=" << s.begin_ip << " fallback=" << s.fallback_ip << " join=" << s.join_ip << " name=" << u(s.name_symbol) << " env=" << b(s.env_reads) << " pn=" << b(s.plain_numeric) << " calls=" << b(s.calls) << "\n";
			}
			out << pre << "cell_loops " << ch.cell_loops.size() << "\n";
			for (size_t i = 0; i < ch.cell_loops.size(); ++i) {
				const chunk::cell_loop& l = ch.cell_loops[i];
				out << pre << " " << i << " head=" << l.head << " back=" << l.back << " copy=" << l.copy_head << ".." << l.copy_back << " exit=" << l.exit_ip << " counted=" << u(l.counted)
				    << " straight=" << b(l.straight) << " trampoline=" << u(l.trampoline) << " frame=" << b(l.frame_program) << " raw_entry=" << b(l.raw_enterable)
				    << (l.frame_program ? (l.writes_stamps ? " stamps=1" : " stamps=0") : "") << " temp_mask=" << mask(l.temp_mask) << " env_mask=" << mask(l.env_mask) << " iter=" << u(l.iter_proto) << "/" << u(l.iter_index_cell) << "/" << u(l.iter_site) << "\n";
				if (!l.rebinds.empty()) {
					out << pre << "  rebinds";
					for (const auto& rb : l.rebinds) { out << " (" << rb.copy_ip << " c" << rb.cell << " s" << u(rb.source_site) << " rd" << u(rb.ref_decl) << " d"; for (const uint32_t d : rb.dependents) { out << ":" << d; } out << ")"; }
					out << "\n";
				}
				out << pre << "  cells";
				for (const auto& c : l.cells) {
					out << " (" << c.slot << " k" << static_cast<int>(c.kind) << " l" << static_cast<int>(c.landing) << " in" << b(c.live_in) << " def" << b(c.defined) << " " << tn(c.stamp)
					    << " env" << u(c.env_operand) << " t" << b(c.temp) << " fd" << b(c.first_decl) << " " << tn(c.decl_stamp) << " sb" << b(c.string_borrow) << " it" << b(c.iter_index) << ")";
				}
				out << "\n" << pre << "  entry_cells";
				for (uint32_t c : l.entry_cells) { out << " " << c; }
				if (!l.param_cells.empty()) { out << "\n" << pre << "  param_cells"; for (uint32_t c : l.param_cells) { out << " " << c; } out << " kinds=" << std::hex << l.param_kinds << std::dec; }
				if (!l.boundaries.empty()) { out << "\n" << pre << "  boundaries"; for (const auto& bd : l.boundaries) { out << " (" << bd.start << " " << bd.end << " " << bd.resume << " " << static_cast<int64_t>(bd.define == 0xFFFFFFFFu ? -1 : bd.define) << ")"; } }
				out << "\n" << pre << "  site_env_cache";
				for (uint32_t c : l.site_env_cache) { out << " " << u(c); }
				out << "\n" << pre << "  slot_to_cell";
				for (uint32_t c : l.slot_to_cell) { out << " " << u(c); }
				out << "\n" << pre << "  origin";
				for (uint32_t o : l.origin) { out << " " << o; }
				out << "\n" << pre << "  live";
				for (const auto& m : l.live_temps) { out << " " << mask(m); }
				out << "\n" << pre << "  sites";
				for (uint32_t s : l.sites) { out << " " << s; }
				out << "\n" << pre << "  env_operands";
				for (const auto& e : l.env_operands) { out << " " << operand(ch, e.operand) << "@" << e.cache_slot; }
				out << "\n" << pre << "  calls";
				for (const auto& c : l.calls) {
					out << " (" << c.probe_ip << " " << c.call_ip << " " << c.consumer_ip << " c" << static_cast<int>(c.consumer) << " dst=" << u(c.dst) << " raw=" << static_cast<int>(c.raw) << " proven=" << static_cast<int>(c.raw_proven) << " kinds=" << std::hex << c.raw_kinds << std::dec << " args=[";
					for (const auto& a : c.args) { out << "(" << static_cast<int>(a.kind) << " " << a.v << " " << a.ip << ")"; }
					out << "])";
				}
				if (!l.math_alias_sites.empty()) {
					out << "\n" << pre << "  math_alias_sites";
					for (const auto& m : l.math_alias_sites) { out << " (" << m.site << " f" << static_cast<int>(m.fn) << " @" << m.cache_slot << ")"; }
				}
				out << "\n" << pre << "  ir " << esc(l.ir_text) << "\n";
			}
			out << pre << "cell_refusals " << ch.cell_refusals.size() << "\n";
			for (const auto& r : ch.cell_refusals) {
				out << pre << " " << r.head << " " << r.back << " " << r.ip << " " << chunk::cell_refusal_name(r.reason) << " " << opcode_name(static_cast<opcode>(r.op)) << "\n";
			}
			for (size_t i = 0; i < ch.param_default_chunks.size(); ++i) {
				if (ch.param_default_chunks[i]) { write(*ch.param_default_chunks[i], label + ".default" + std::to_string(i), depth + 1); }
			}
			for (size_t i = 0; i < ch.closure_protos.size(); ++i) {
				if (ch.closure_protos[i].body_chunk) { write(*ch.closure_protos[i].body_chunk, label + ".closure" + std::to_string(i), depth + 1); }
			}
			out << pre << "end " << label << "\n";
		}
	};

} // namespace

std::string chunk_manifest(const chunk& ch, const string_symbolizer* symbolizer) {
	manifest_writer w;
	w.sym = symbolizer;
	w.write(ch, "root", 0);
	return w.out.str();
}

} // namespace jai::vm
