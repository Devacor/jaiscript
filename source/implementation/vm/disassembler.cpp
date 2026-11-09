#include <jaiscript/vm/disassembler.hpp>
#include <sstream>

namespace jai::vm {

namespace {

	std::string constant_text(const script_value& v) {
		switch (v.raw_storage_index()) {
			case script_value::TYPEID_NULL: return "null";
			case script_value::TYPEID_INT: return std::to_string(v.unchecked_as_int());
			case script_value::TYPEID_FLOAT: return std::to_string(v.unchecked_as_float());
			case script_value::TYPEID_STRING: return "\"" + std::string(v.unchecked_as_string()) + "\"";
			case script_value::TYPEID_BOOL: return v.unchecked_as_bool() ? "true" : "false";
			case script_value::TYPEID_CHAR: return std::string("'") + v.unchecked_as_char() + "'";
			case script_value::TYPEID_CPP_BOUND: return "<cpp_bound>";  // constants are never bound; label for safety
			default: return "<value>";
		}
	}

} // namespace

std::string disassemble(const chunk& ch, const string_symbolizer* symbolizer) {
	std::ostringstream out;
	out << "== " << (ch.function_name.empty() ? "<script>" : ch.function_name)
	    << " (" << ch.code.size() << " instructions, " << ch.constants.size() << " constants) ==\n";

	for (size_t i = 0; i < ch.code.size(); ++i) {
		const vm_instruction& ins = ch.code[i];
		const ast_node* node = i < ch.stmt_nodes.size() ? ch.stmt_nodes[i] : nullptr;

		out << (i < 10 ? "000" : (i < 100 ? "00" : (i < 1000 ? "0" : ""))) << i << "  ";
		if (node) {
			out << "L" << node->location.line << "\t";
		} else {
			out << "L?\t";
		}
		out << opcode_name(ins.op);

		switch (ins.op) {
			case opcode::op_const:
				out << " " << ins.a;
				if (ins.a < ch.constants.size()) {
					out << " (" << constant_text(ch.constants[ins.a]) << ")";
				}
				break;
			case opcode::op_load:
			case opcode::op_incdec: {
				const uint32_t sym_idx = ins.op == opcode::op_load ? ins.b : ins.a;
				const uint32_t slot = ins.op == opcode::op_load ? ins.a : ins.b;
				out << " slot=";
				if (slot == k_invalid_u32) out << "env";
				else out << slot;
				if (sym_idx < ch.symbols.size() && symbolizer) {
					out << " '" << symbolizer->get_string(ch.symbols[sym_idx]) << "'";
				}
				if (ins.c) out << " flags=" << ins.c;
				break;
			}
			case opcode::op_store:
			case opcode::op_compound_store:
			case opcode::op_concat_store: {
				out << " slot=";
				if (ins.b == k_invalid_u32) out << "env";
				else out << ins.b;
				if (ins.a < ch.symbols.size() && symbolizer) {
					out << " '" << symbolizer->get_string(ch.symbols[ins.a]) << "'";
				}
				out << " flags=" << ins.c;
				break;
			}
			case opcode::op_binary:
				out << " op=" << ins.a << " shape=" << ins.b;
				break;
			case opcode::op_binary_fused:
				if (ins.a < ch.fused_binary_protos.size()) {
					out << " op=" << static_cast<uint32_t>(ch.fused_binary_protos[ins.a].op);
				}
				out << " proto=" << ins.a << " shape=" << ins.b;
				break;
			case opcode::op_unary:
				out << " op=" << ins.a;
				break;
			case opcode::op_fused_cmp_jump:
				if (ins.b < ch.fused_binary_protos.size()) {
					out << " op=" << static_cast<uint32_t>(ch.fused_binary_protos[ins.b].op);
				}
				out << " proto=" << ins.b << " -> " << ins.a << " proved=" << ins.c;
				break;
			case opcode::op_compound_fused:
				if (ins.a < ch.compound_fused_protos.size()) {
					const auto& cp = ch.compound_fused_protos[ins.a];
					if (cp.rhs_proto == k_invalid_u32) {
						if (cp.rhs.const_index != k_invalid_u32) out << " rhs=const:" << cp.rhs.const_index;
						else out << " rhs=ident";
					} else {
						out << " rhs_proto=" << cp.rhs_proto;
					}
					out << " slot=";
					if (cp.slot == k_invalid_u32) out << "env";
					else out << cp.slot;
					if (cp.symbol < ch.symbols.size() && symbolizer) {
						out << " '" << symbolizer->get_string(ch.symbols[cp.symbol]) << "'";
					}
					out << " kind_flags=" << cp.kind_flags;
				}
				break;
			case opcode::op_jump:
			case opcode::op_jump_if_false:
			case opcode::op_jump_if_true:
			case opcode::op_loop_back:
				out << " -> " << ins.a;
				break;
			case opcode::op_call:
			case opcode::op_call_method:
				out << " argc=" << ins.a << " site=" << ins.b;
				break;
			case opcode::op_call_builtin:
				out << " argc=" << ins.a << " site=" << ins.b << " kind=" << (ins.c & 15u) << " inline=" << (ins.c >> 4);
				break;
			case opcode::op_get_member:
				out << " node=" << ins.a;
				if (ins.b != 0) { out << " base_slot=" << ins.b - 1; }
				break;
			case opcode::op_get_member_this:
				out << " node=" << ins.a << " field_slot=" << (ins.c ? ins.c - 1 : 0);
				if (ins.b < ch.symbols.size() && symbolizer) { out << " '" << symbolizer->get_string(ch.symbols[ins.b]) << "'"; }
				break;
			case opcode::op_member_temp_this:
				out << " node=" << ins.a << " temp=" << ins.b << " field_slot=" << (ins.c ? ins.c - 1 : 0);
				break;
			case opcode::op_text_append:
				if (ins.a < ch.text_append_protos.size()) {
					const text_append_proto& p = ch.text_append_protos[ins.a];
					out << (p.assign ? " assign" : " compound") << " slot=" << p.slot;
					if (p.symbol < ch.symbols.size() && symbolizer) { out << " '" << symbolizer->get_string(ch.symbols[p.symbol]) << "'"; }
					out << " pieces=" << p.pieces.size();
				}
				break;
			case opcode::op_mfold:
				out << " fold=" << ins.a << " skip=" << ins.b;
				if (ins.a < ch.member_folds.size()) {
					const chunk::member_fold& m = ch.member_folds[ins.a];
					out << " " << opcode_name(m.first.op) << " .. " << opcode_name(m.consumer.op);
				}
				break;
			case opcode::op_get_member_path:
				out << " node=" << ins.a << " path=" << ins.b;
				if (ins.b < ch.member_path_protos.size()) {
					out << " links=" << ch.member_path_protos[ins.b].links.size();
					if (ch.member_path_protos[ins.b].root_slot != k_invalid_u32) { out << " base_slot=" << ch.member_path_protos[ins.b].root_slot; }
				}
				break;
			case opcode::op_get_static:
			case opcode::op_set_member:
			case opcode::op_set_static:
			case opcode::op_member_compound:
			case opcode::op_ref_target_store:
			case opcode::op_new:
			case opcode::op_class_decl:
			case opcode::op_namespace_decl:
			case opcode::op_enum_decl:
			case opcode::op_extern_decl:
			case opcode::op_decl_keep:
			case opcode::op_decl_share:
				out << " node=" << ins.a;
				break;
			case opcode::op_kill_slot:
				out << " slot=" << ins.a;
				if (ins.b) { out << " down=" << ins.b; }
				break;
			case opcode::op_field_init:
				out << " field_init=" << ins.a;
				break;
			case opcode::op_ctor_base:
			case opcode::op_ctor_delegate:
				out << " step=" << ins.a << " argc=" << ins.b;
				break;
			case opcode::op_ctor_done:
			case opcode::op_ctor_literals:
				out << " step=" << ins.a;
				break;
			case opcode::op_null_guard:
				out << " -> " << ins.a;
				break;
			case opcode::op_try_push:
				out << " handler=" << ins.a;
				if (ins.b < ch.symbols.size() && symbolizer) {
					out << " catch='" << symbolizer->get_string(ch.symbols[ins.b]) << "'";
				}
				if (ins.c != k_invalid_u32) { out << " slot=" << ins.c; }
				break;
			case opcode::op_throw:
				out << (ins.a ? " value" : " rethrow");
				break;
			case opcode::op_yield:
				out << (ins.a ? (ins.b ? " value copy" : " value") : " void");
				break;
			case opcode::op_import:
				out << " site=" << ins.a << " exprs=" << ins.b << (ins.c ? " value" : "");
				break;
			case opcode::op_include:
				if (ins.b) {
					out << " <expr>";
				} else if (ins.a != k_invalid_u32 && ins.a < ch.messages.size()) {
					out << " \"" << ch.messages[ins.a] << "\"";
				}
				break;
			case opcode::op_iter_entry:
				out << " proto=" << ins.a << (ins.c == 1 ? " key" : ins.c == 2 ? " value" : " value byref");
				break;
			case opcode::op_iter_init:
			case opcode::op_iter_next:
				out << " proto=" << ins.a;
				if (ins.a < ch.iter_protos.size() && symbolizer) {
					out << " '" << symbolizer->get_string(ch.iter_protos[ins.a].var_symbol) << "'";
					if (ch.iter_protos[ins.a].is_reference) out << " byref";
				}
				break;
			case opcode::op_from_this:
				out << (ins.a ? " weak" : " shared");
				break;
			case opcode::op_func_decl:
				out << " proto=" << ins.a;
				if (ins.a < ch.function_protos.size() && ch.function_protos[ins.a].decl) {
					out << " '" << ch.function_protos[ins.a].decl->name << "'";
				}
				break;
			case opcode::op_closure:
				out << " proto=" << ins.a;
				break;
			case opcode::op_array:
			case opcode::op_map:
			case opcode::op_scope_pop_n:
			case opcode::op_return:
			case opcode::op_return_convert:
				out << " " << ins.a;
				break;
			case opcode::op_probe_callee:
				out << " site=" << ins.a << " argc=" << ins.b;
				break;
			case opcode::op_regex_constant:
				out << " site=" << ins.a << " regex=" << ins.b << " argc=" << ins.c;
				break;
			case opcode::op_call_from_scratch:
			case opcode::op_sleep:
			case opcode::op_format_call:
			case opcode::op_call_typed:
				out << " argc=" << ins.a << " site=" << ins.b;
				break;
			case opcode::op_probe_member:
				out << " site=" << ins.a << " mode=" << ins.b;
				break;
			case opcode::op_return_ident:
				out << " slot=" << static_cast<int32_t>(ins.a) << " sym=" << ins.b;
				break;
			case opcode::op_return_binary:
				out << " op=" << ins.a << " shape=" << ins.b;
				break;
			case opcode::op_const_guard:
				out << " node=" << ins.a;
				break;
			case opcode::op_switch_table:
				out << " cell=" << ins.a << " table=" << ins.b << " copy_targets=" << ins.c;
				if (ins.b < ch.switch_tables.size()) {
					const auto& t = ch.switch_tables[ins.b];
					out << " ->";
					for (uint32_t target : t.targets) { out << " " << target; }
					out << " else " << t.no_match;
				}
				break;
			case opcode::op_const_folded:
				out << " " << ins.a;
				if (ins.a < ch.constants.size()) { out << " (" << constant_text(ch.constants[ins.a]) << ")"; }
				break;
			case opcode::op_error:
				out << " code=" << ins.a;
				if (ins.b != k_invalid_u32 && ins.b < ch.messages.size()) {
					out << " \"" << ch.messages[ins.b] << "\"";
				}
				break;
			default:
				out << " " << ins.a << " " << ins.b << " " << ins.c;   // the typed rows and the cell copies: slots, cells, constants behind their bit, targets
				break;
		}
		out << "\n";
	}

	for (const auto& loop : ch.cell_loops) {
		out << (loop.frame_program ? "-- frame program " : loop.straight ? "-- cell region " : "-- cell loop ") << loop.head << ".." << loop.back << " -> copy " << loop.copy_head << ".." << loop.copy_back << (loop.counted != k_invalid_u32 ? " (counted)" : "");
		if (loop.frame_program) { out << (loop.raw_enterable ? " (raw entry)" : " (general entry only)"); }
		if (!loop.calls.empty()) {
			size_t raw = 0;
			for (const auto& cc : loop.calls) { if (cc.raw) { ++raw; } }
			out << " (calls " << loop.calls.size();
			if (raw) { out << ", raw " << raw; }
			out << ")";
		}
		if (!loop.boundaries.empty()) {
			out << " (boundaries";
			for (const auto& bd : loop.boundaries) { out << " " << bd.start << ".." << bd.end; if (bd.define != 0xFFFFFFFFu) { out << " defines c" << bd.define; } }
			out << ")";
		}
		out << " cells";
		for (const auto& cl : loop.cells) { out << " " << (cl.env_operand != k_invalid_u32 ? "env" : cl.temp ? "t" : "s") << cl.slot << ":" << static_cast<unsigned>(cl.kind); }   // slot:kind (kinds: 0 adopted, 1 int, 2 float, 5 bool)
		out << "\n";
		if (!loop.ir_text.empty()) { out << "-- ir " << loop.ir_text << "\n"; }
	}
	for (const auto& s : ch.inline_sites) {
		out << "-- inlined " << (s.decl ? std::string(s.decl->name) : std::string("?")) << " guard " << s.guard_ip << ": " << s.begin_ip << ".." << s.fallback_ip << " fallback " << s.fallback_ip << ".." << s.join_ip << "\n";
	}
	for (const auto& r : ch.inline_refusals) {
		out << "-- call kept " << r.callee << " line " << r.line << ": " << (r.reason ? r.reason : "?") << "\n";
	}
	for (const auto& r : ch.cell_refusals) {
		out << "-- open loop " << r.head << ".." << r.back << ": " << chunk::cell_refusal_name(r.reason) << " at " << r.ip << " (" << opcode_name(static_cast<opcode>(r.op)) << ")\n";
	}
	for (size_t i = 0; i < ch.closure_protos.size(); ++i) {
		if (ch.closure_protos[i].body_chunk) {
			out << "\n-- closure proto " << i << (ch.closure_protos[i].body_chunk->needs_frame_env ? " (env: frame) --\n" : " (env: lazy) --\n");
			out << disassemble(*ch.closure_protos[i].body_chunk, symbolizer);
		}
	}
	return out.str();
}

} // namespace jai::vm
