// The execution-coverage census (vm/coverage.hpp): a diagnostic build's counters, the static classification of each
// chunk when it first runs, and the report. Nothing here exists without JAISCRIPT_VM_COVERAGE.
#ifdef JAISCRIPT_VM_COVERAGE

#include <jaiscript/vm/vm_backend.hpp>
#include <jaiscript/vm/coverage.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/script_class.hpp>
#include <jaiscript/detail/cast_kernel.hpp>
#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

namespace jai::vm {

namespace {

	enum class fam : uint8_t {
		typed_arith, typed_cmp, typed_move, typed_elem,
		call, method, builtin, ret, member, element, string, alloc, arith, compare, move, loop, block, scope, decl,
		cell_arith, cell_branch, cell_mem, cell_member, cell_move, cell_math, cell_door, cell_boundary,
		other, count
	};

	const char* fam_name(fam f) {
		switch (f) {
		case fam::typed_arith: return "typed arithmetic (T_ADD..T_MOD)";
		case fam::typed_cmp: return "typed compare-jump (T_J*)";
		case fam::typed_move: return "typed move (T_DECL/T_STORE)";
		case fam::typed_elem: return "typed element (T_LOAD/T_PUSH)";
		case fam::call: return "call: function (probe, call, guard, temp)";
		case fam::method: return "call: method";
		case fam::builtin: return "call: builtin method (container/string)";
		case fam::ret: return "return / frame exit";
		case fam::member: return "member access";
		case fam::element: return "element access / iteration";
		case fam::string: return "string concatenation";
		case fam::alloc: return "allocation (new, literal, closure)";
		case fam::arith: return "untyped arithmetic / math / cast";
		case fam::compare: return "untyped compare / branch";
		case fam::move: return "untyped load / store / declaration";
		case fam::loop: return "loop control (back edge, counted for)";
		case fam::block: return "block entry / resume";
		case fam::scope: return "scope / try / throw / yield";
		case fam::decl: return "program declarations";
		case fam::cell_arith: return "cell arithmetic";
		case fam::cell_branch: return "cell compare / branch / back edge";
		case fam::cell_mem: return "cell element load / store / push";
		case fam::cell_member: return "cell member site";
		case fam::cell_move: return "cell move";
		case fam::cell_math: return "cell math intrinsic";
		case fam::cell_door: return "cell call door (probe, args, call, land, ret)";
		case fam::cell_boundary: return "cell boundary operation";
		default: return "other";
		}
	}

	fam family_of(opcode o) {
		if (o >= opcode::op_t_add_ii && o <= opcode::op_t_mod_ff) return fam::typed_arith;
		if (o >= opcode::op_t_jlt_ii && o <= opcode::op_t_jne_ff) return fam::typed_cmp;
		switch (o) {
		case opcode::op_t_decl_mov: case opcode::op_t_decl_const: case opcode::op_t_store_mov: case opcode::op_t_store_const: return fam::typed_move;
		case opcode::op_t_push: case opcode::op_t_load: return fam::typed_elem;
		case opcode::op_call: case opcode::op_probe_callee: case opcode::op_regex_constant: case opcode::op_call_from_scratch: case opcode::op_sleep: case opcode::op_format_call: case opcode::op_call_typed: case opcode::op_call_temp:
		case opcode::op_capture_arg: case opcode::op_take_temp: case opcode::op_inline_guard: case opcode::op_math_guard:
			return fam::call;
		case opcode::op_call_method: case opcode::op_probe_member: return fam::method;
		case opcode::op_call_builtin: case opcode::op_size_temp: case opcode::op_push_fused: return fam::builtin;
		case opcode::op_return: case opcode::op_return_ident: case opcode::op_return_binary: case opcode::op_return_convert: case opcode::op_implicit_return:
		case opcode::op_halt: case opcode::op_ref_return_bind: case opcode::op_ref_return_lvalue:
			return fam::ret;
		case opcode::op_get_member: case opcode::op_get_member_path: case opcode::op_get_member_this: case opcode::op_place_arg: case opcode::op_get_static: case opcode::op_set_member: case opcode::op_set_static: case opcode::op_member_compound:
		case opcode::op_member_temp: case opcode::op_member_temp_this: case opcode::op_mfold: case opcode::op_set_member_fused: case opcode::op_set_member_value: case opcode::op_this: case opcode::op_super:
		case opcode::op_from_this: case opcode::op_null_guard: case opcode::op_velem_load: case opcode::op_velem_store:
			return fam::member;
		case opcode::op_index: case opcode::op_index_store: case opcode::op_index_compound_fused:
		case opcode::op_index_fused: case opcode::op_index_store_fused: case opcode::op_index_compound_operand: case opcode::op_index_fused_decl: case opcode::op_iter_init: case opcode::op_iter_next:
		case opcode::op_iter_pop: case opcode::op_iter_entry: case opcode::op_destructure: case opcode::op_parallel_element: case opcode::op_deref_top:
			return fam::element;
		case opcode::op_concat_append: case opcode::op_concat_pending: case opcode::op_concat_operands: case opcode::op_concat_store: case opcode::op_text_append: return fam::string;
		case opcode::op_new: case opcode::op_array: case opcode::op_map: case opcode::op_closure: return fam::alloc;
		case opcode::op_binary: case opcode::op_binary_fused: case opcode::op_binary_fused_decl: case opcode::op_binary_fused_store: case opcode::op_binary_fused_temp:
		case opcode::op_unary: case opcode::op_unary_temp: case opcode::op_math: case opcode::op_math_temp: case opcode::op_compound_store: case opcode::op_compound_fused:
		case opcode::op_incdec: case opcode::op_operand_temp: case opcode::op_cast: case opcode::op_to_bool: case opcode::op_case_eq:
		case opcode::op_d_add: case opcode::op_d_sub: case opcode::op_d_mul: case opcode::op_d_div: case opcode::op_d_mod: case opcode::op_d_and: case opcode::op_d_or:
		case opcode::op_d_xor: case opcode::op_d_shl: case opcode::op_d_shr:
			return fam::arith;
		case opcode::op_fused_cmp_jump: case opcode::op_jump: case opcode::op_jump_if_false: case opcode::op_jump_if_true: case opcode::op_switch_table:
		case opcode::op_d_jlt: case opcode::op_d_jle: case opcode::op_d_jgt: case opcode::op_d_jge: case opcode::op_d_jeq: case opcode::op_d_jne:
			return fam::compare;
		case opcode::op_const: case opcode::op_const_folded: case opcode::op_null: case opcode::op_true: case opcode::op_false: case opcode::op_pop: case opcode::op_dup: case opcode::op_load:
		case opcode::op_store: case opcode::op_decl_var: case opcode::op_decl_ref_ident: case opcode::op_decl_ref_value: case opcode::op_decl_keep:
		case opcode::op_decl_share: case opcode::op_bind_captures: case opcode::op_ref_target_store:
			return fam::move;
		case opcode::op_loop_back: case opcode::op_cfor_prep: case opcode::op_cfor_back: case opcode::op_cfor_pop: case opcode::op_parallel_advance: return fam::loop;
		case opcode::op_cells_back: case opcode::op_cells_enter: case opcode::op_cells_cfor_back: case opcode::op_cells_resume: return fam::block;
		case opcode::op_scope_push: case opcode::op_scope_pop: case opcode::op_scope_pop_n: case opcode::op_kill_slot: case opcode::op_try_push: case opcode::op_try_pop: case opcode::op_catch_end:
		case opcode::op_throw: case opcode::op_yield:
			return fam::scope;
		case opcode::op_ctor_base: case opcode::op_ctor_delegate: case opcode::op_ctor_done: case opcode::op_ctor_literals: case opcode::op_ctor_scope:
		case opcode::op_func_decl: case opcode::op_class_decl: case opcode::op_namespace_decl: case opcode::op_enum_decl: case opcode::op_extern_decl: case opcode::op_field_init:
		case opcode::op_include: case opcode::op_import: case opcode::op_parallel_for: case opcode::op_error: case opcode::op_const_guard:
			return fam::decl;
		case opcode::op_c_arith_p: case opcode::op_c_arith_any: case opcode::op_c_arith2: case opcode::op_c_arith_load:
		case opcode::op_c_add_ff: case opcode::op_c_sub_ff: case opcode::op_c_mul_ff: case opcode::op_c_add_ffk: case opcode::op_c_sub_ffk: case opcode::op_c_mul_ffk:
		case opcode::op_c_add_ii: case opcode::op_c_sub_ii: case opcode::op_c_add_iik: case opcode::op_c_sub_iik: case opcode::op_c_neg: case opcode::op_c_bnot:
		case opcode::op_c_lnot: case opcode::op_c_cmpval: case opcode::op_c_append: case opcode::op_c_text_append:
			return fam::cell_arith;
		case opcode::op_c_cmp_p: case opcode::op_c_jcmp_p: case opcode::op_c_jcmp2: case opcode::op_c_cmp_any:
		case opcode::op_c_jcmp_any: case opcode::op_c_jump: case opcode::op_c_back: case opcode::op_c_back_step: case opcode::op_c_cfor_back: case opcode::op_c_range:
		case opcode::op_c_exit: case opcode::op_c_bail:
			return fam::cell_branch;
		case opcode::op_c_load: case opcode::op_c_load_any: case opcode::op_c_load2: case opcode::op_c_load_jcmp: case opcode::op_c_index_store: case opcode::op_c_push:
		case opcode::op_c_size: case opcode::op_c_guard: case opcode::op_c_rebind:
			return fam::cell_mem;
		case opcode::op_c_mload: case opcode::op_c_mstore: return fam::cell_member;
		case opcode::op_c_mov: case opcode::op_c_mov_p: case opcode::op_c_mov_k: return fam::cell_move;
		case opcode::op_c_math: return fam::cell_math;
		case opcode::op_c_probe: case opcode::op_c_args: case opcode::op_c_call: case opcode::op_c_land: case opcode::op_c_ret: return fam::cell_door;
		case opcode::op_c_boundary: return fam::cell_boundary;
		default: return fam::other;
		}
	}

	bool typed_op(opcode o) { const fam f = family_of(o); return f == fam::typed_arith || f == fam::typed_cmp || f == fam::typed_move || f == fam::typed_elem; }

	// the region builder's operations (vm_compiler::lower_typed_ops straight_op): a general instruction of this set outside every span
	// stands in a fragment with no statement boundary or entry the builder takes
	bool straight_op(opcode o) {
		if (o >= opcode::op_t_add_ii && o <= opcode::op_t_mod_ff) return true;
		if (o >= opcode::op_t_jlt_ii && o <= opcode::op_t_jne_ff) return true;
		if (o >= opcode::op_d_add && o <= opcode::op_d_jne) return true;   // a dynamic op stands for the fused instruction it replaced
		switch (o) {
		case opcode::op_binary_fused: case opcode::op_binary_fused_decl: case opcode::op_binary_fused_temp: case opcode::op_binary_fused_store:
		case opcode::op_fused_cmp_jump: case opcode::op_t_decl_mov: case opcode::op_t_decl_const: case opcode::op_t_store_mov: case opcode::op_t_store_const:
		case opcode::op_unary_temp: case opcode::op_math_temp: case opcode::op_call_temp: case opcode::op_math_guard: case opcode::op_inline_guard:
		case opcode::op_jump: case opcode::op_jump_if_true: case opcode::op_jump_if_false: case opcode::op_load: case opcode::op_const: case opcode::op_store:
		case opcode::op_decl_var: case opcode::op_operand_temp: case opcode::op_compound_fused: case opcode::op_incdec: case opcode::op_pop:
		case opcode::op_return: case opcode::op_return_ident: case opcode::op_return_binary: case opcode::op_null: case opcode::op_true: case opcode::op_false:
			return true;
		default: return false;
		}
	}

	std::string sym_name(const chunk& c, const string_symbolizer* sym, uint32_t idx) {
		if (!sym || idx >= c.symbols.size()) return {};
		return std::string(sym->get_string(c.symbols[idx]));
	}

	std::string node_name(const chunk& c, uint32_t node) {
		if (node >= c.nodes.size() || !c.nodes[node]) return {};
		const ast_node* n = c.nodes[node].get();
		switch (n->get_type()) {
		case node_type::member_expr: return std::string(static_cast<const member_expr*>(n)->member);
		case node_type::variable_decl: return std::string(static_cast<const variable_decl*>(n)->name);
		case node_type::new_expr: return "new";
		default: return {};
		}
	}

	uint32_t call_site_of(const vm_instruction& ins) {
		switch (ins.op) {
		case opcode::op_probe_callee: case opcode::op_regex_constant: return ins.a;
		case opcode::op_call_from_scratch: case opcode::op_sleep: case opcode::op_format_call: case opcode::op_call_typed: case opcode::op_call_method: case opcode::op_call_builtin: case opcode::op_call: return ins.b;
		case opcode::op_size_temp: return ins.b;
		case opcode::op_probe_member: return ins.a;
		default: return k_invalid_u32;
		}
	}

	// the instruction's name in a reason's detail: its opcode and the name it touches (a callee, a member, a variable)
	std::string instruction_detail(const chunk& c, const string_symbolizer* sym, uint32_t ip) {
		if (ip >= c.code.size()) return "?";
		const vm_instruction& ins = c.code[ip];
		std::string out(opcode_name(ins.op));
		std::string name;
		const uint32_t site = call_site_of(ins);
		if (site != k_invalid_u32 && site < c.call_sites.size()) {
			const call_site& cs = c.call_sites[site];
			if (cs.member_node != k_invalid_u32) name = node_name(c, cs.member_node);
			else if (cs.callee.symbol != k_invalid_u32) name = sym_name(c, sym, cs.callee.symbol);
		} else {
			switch (ins.op) {
			case opcode::op_get_member: case opcode::op_get_member_path: case opcode::op_get_member_this: case opcode::op_get_static: case opcode::op_set_member: case opcode::op_set_static: case opcode::op_member_compound:
			case opcode::op_member_temp: case opcode::op_member_temp_this: case opcode::op_set_member_fused: case opcode::op_decl_var: case opcode::op_decl_ref_ident: case opcode::op_decl_ref_value:
			case opcode::op_new: case opcode::op_cast:
				name = node_name(c, ins.a); break;
			case opcode::op_set_member_value: if (ins.a < c.member_store_protos.size()) { name = node_name(c, c.member_store_protos[ins.a].node); } break;
			case opcode::op_load: name = sym_name(c, sym, ins.b); break;
			case opcode::op_store: case opcode::op_compound_store: case opcode::op_incdec: name = sym_name(c, sym, ins.a); break;
			case opcode::op_index_fused: case opcode::op_index_store_fused: case opcode::op_index_compound_operand: case opcode::op_index_fused_decl:
				if (ins.a < c.fused_index_protos.size()) {
					const fused_index_proto& fp = c.fused_index_protos[ins.a];
					name = sym_name(c, sym, fp.container.symbol);
					if (fp.receiver == 1) name += " (string)"; else if (fp.receiver == 3) name += " (map)";
				}
				break;
			default: break;
			}
		}
		if (!name.empty()) { out += " "; out += name; }
		return out;
	}

	// why the instruction at ip keeps a loop (or a straight-line span) off the cells: the refusal class and the instruction's kind
	std::string refusal_category(const chunk& c, uint32_t ip, uint8_t cls) {
		switch (cls) {
		case chunk::cell_refusal_head: return "loop head form";
		case chunk::cell_refusal_no_cells: return "no scalar work (no cells)";
		case chunk::cell_refusal_too_many_cells: return "too many cells";
		case chunk::cell_refusal_bail_dominates: return "bail dominates the back edge";
		case chunk::cell_refusal_malformed: return "malformed region (lifetime rule)";
		default: break;
		}
		if (ip >= c.code.size()) return "unsupported instruction";
		const vm_instruction& ins = c.code[ip];
		switch (ins.op) {
		case opcode::op_decl_ref_ident: case opcode::op_decl_ref_value: case opcode::op_ref_return_bind: case opcode::op_ref_return_lvalue: case opcode::op_ref_target_store:
			return "reference / alias";
		case opcode::op_concat_append: case opcode::op_concat_pending: case opcode::op_concat_operands: case opcode::op_concat_store: case opcode::op_text_append: return "string operation";
		default: break;
		}
		switch (family_of(ins.op)) {
		case fam::call: return "call boundary (function)";
		case fam::method: return "call boundary (method)";
		case fam::builtin: return "call boundary (builtin method)";
		case fam::member: return "member access";
		case fam::element: case fam::typed_elem: {
			if (cls == chunk::cell_refusal_effect) return "effect (call-argument order, an edge out, a global-only name)";
			uint32_t proto = k_invalid_u32;
			if (ins.op == opcode::op_index_fused || ins.op == opcode::op_index_store_fused || ins.op == opcode::op_index_compound_operand || ins.op == opcode::op_index_fused_decl) proto = ins.a;
			if (proto < c.fused_index_protos.size()) {
				if (c.fused_index_protos[proto].receiver == 1) return "string operation";
				if (c.fused_index_protos[proto].receiver == 3) return "map operation";
			}
			return "container element";
		}
		case fam::alloc: return "allocation";
		case fam::ret: return "exit (return)";
		case fam::scope: return "scope / try / throw / yield";
		case fam::decl: return "declaration";
		case fam::compare: case fam::loop:
			return cls == chunk::cell_refusal_effect ? "exit (break / an edge out)" : cls == chunk::cell_refusal_translation ? "translation gap" : "non-scalar or untyped operand";
		case fam::arith: case fam::move: case fam::typed_arith: case fam::typed_cmp: case fam::typed_move:
			return cls == chunk::cell_refusal_translation ? "translation gap" : cls == chunk::cell_refusal_effect ? "effect (call-argument order, an edge out, a global-only name)" : "non-scalar or untyped operand";
		default: return "unsupported instruction";
		}
	}

	std::string class_word(uint8_t cls) { return chunk::cell_refusal_name(cls); }

	std::string operand_shape(const fused_operand& o) {
		if (o.const_index != k_invalid_u32) return "literal";
		if (o.is_subscript()) return "element";
		if (o.member_node != k_invalid_u32) return "member";
		if (o.slot == k_invalid_u32) return "global";
		return o.direct ? "local" : "slot";
	}
	const char* token_text(uint8_t t) {
		switch (static_cast<token_type>(t)) {
		case token_type::plus: return "+"; case token_type::minus: return "-"; case token_type::star: return "*"; case token_type::slash: return "/";
		case token_type::percent: return "%"; case token_type::ampersand: return "&"; case token_type::pipe: return "|"; case token_type::caret: return "^";
		case token_type::left_shift: return "<<"; case token_type::right_shift: return ">>"; case token_type::less: return "<"; case token_type::less_equal: return "<=";
		case token_type::greater: return ">"; case token_type::greater_equal: return ">="; case token_type::equal_equal: return "=="; case token_type::bang_equal: return "!=";
		case token_type::ampersand_ampersand: return "&&"; case token_type::pipe_pipe: return "||";
		default: return "?";
		}
	}
	// a fused binary's operator, its operands' shapes and the checker's class (0 unproven, 1 int, 2 float)
	std::string binary_detail(const chunk& c, uint32_t pi) {
		if (pi >= c.fused_binary_protos.size()) return {};
		const fused_binary_proto& bp = c.fused_binary_protos[pi];
		return std::string(" `") + operand_shape(bp.left) + " " + token_text(bp.op) + " " + operand_shape(bp.right) + "` class " + std::to_string(bp.typed_class);
	}
	std::string refusal_detail(const chunk& c, const string_symbolizer* sym, uint32_t ip, uint8_t cls, uint32_t head, uint32_t back) {
		std::string out = instruction_detail(c, sym, ip);
		if (ip < c.code.size()) {
			const vm_instruction& ins = c.code[ip];
			switch (ins.op) {
			case opcode::op_binary_fused: case opcode::op_binary_fused_temp: out += binary_detail(c, ins.a); break;
			case opcode::op_fused_cmp_jump: out += binary_detail(c, ins.b); break;
			case opcode::op_binary_fused_decl: case opcode::op_binary_fused_store: if (ins.a < c.fused_binary_dst_protos.size()) { out += binary_detail(c, c.fused_binary_dst_protos[ins.a].binary_proto); } break;
			default: break;
			}
		}
		if (cls == chunk::cell_refusal_head) {   // the head's instructions up to its first branch: what the head form did not take
			out += " head:";
			for (uint32_t k = head, shown = 0; k < back && k < c.code.size() && shown < 8; ++k, ++shown) {
				const vm_instruction& h = c.code[k];
				out += " "; out += instruction_detail(c, sym, k);
				if (h.op == opcode::op_fused_cmp_jump) out += binary_detail(c, h.b);
				if (h.op == opcode::op_jump_if_false || h.op == opcode::op_jump_if_true || h.op == opcode::op_fused_cmp_jump || (h.op >= opcode::op_t_jlt_ii && h.op <= opcode::op_t_jne_ff) || h.op == opcode::op_iter_next) break;
			}
		}
		return out;
	}

	struct span { uint32_t lo, hi; coverage_region kind; uint32_t index; };

	coverage_chunk classify(const chunk& c, const string_symbolizer* sym) {
		coverage_chunk r;
		r.name = c.function_name.empty() ? "<top level>" : c.function_name;
		r.identity = c.code.data();
		r.size = c.code.size();
		const size_t n = c.code.size();
		r.general.assign(n, 0); r.cells.assign(n, 0); r.line.assign(n, 0); r.file.assign(n, 0); r.op.resize(n);
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
		r.cycles.assign(n, 0);
#endif
		r.files.push_back("");
		r.region.assign(n, coverage_region::none); r.region_index.assign(n, k_invalid_u32); r.site.assign(n, k_invalid_u32);
		for (size_t ip = 0; ip < n; ++ip) {
			r.op[ip] = static_cast<uint16_t>(c.code[ip].op);
			const ast_node* st = ip < c.stmt_nodes.size() ? c.stmt_nodes[ip] : nullptr;
			if (st) {
				r.line[ip] = static_cast<uint32_t>(st->location.line);
				std::string f = st->location.filename;
				const size_t cut = f.find_last_of("/\\");
				if (cut != std::string::npos) f = f.substr(cut + 1);
				auto it = std::find(r.files.begin(), r.files.end(), f);
				if (it == r.files.end()) { r.files.push_back(f); it = r.files.end() - 1; }
				r.file[ip] = static_cast<uint16_t>(it - r.files.begin());
			}
		}
		auto line_of = [&](uint32_t ip) -> uint32_t { return ip < n ? r.line[ip] : 0; };
		auto add_site = [&](uint32_t at, std::string reason, std::string detail) -> uint32_t {
			r.sites.push_back(coverage_chunk::site_row{ line_of(at), at < n ? r.file[at] : uint16_t(0), std::move(reason), std::move(detail) });
			return static_cast<uint32_t>(r.sites.size() - 1);
		};
		size_t orig = n;
		for (const auto& L : c.cell_loops) { if (L.copy_head < orig) orig = L.copy_head; }
		r.loops.resize(c.cell_loops.size());
		std::vector<span> spans;
		for (uint32_t li = 0; li < c.cell_loops.size(); ++li) {
			const auto& L = c.cell_loops[li];
			auto& lr = r.loops[li];
			lr.head_line = line_of(L.head); lr.back_line = line_of(L.back > 0 && L.straight ? L.back - 1 : L.back);
			lr.straight = L.straight; lr.frame_program = L.frame_program; lr.head_file = L.head < n ? r.file[L.head] : uint16_t(0);
			const coverage_region kind = L.frame_program ? coverage_region::frame_program : L.straight ? coverage_region::straight : coverage_region::closed;
			for (size_t k = 0; k < L.origin.size(); ++k) {   // the copy: each instruction stands for its origin
				const size_t cip = L.copy_head + k;
				if (cip >= n) break;
				r.line[cip] = line_of(L.origin[k]);
				r.file[cip] = L.origin[k] < n ? r.file[L.origin[k]] : uint16_t(0);
				r.region[cip] = kind; r.region_index[cip] = li;
			}
			uint32_t hi = L.straight ? L.back : L.back + 1;
			if (L.frame_program) hi = L.back + 1;
			else if (!L.straight && L.counted != k_invalid_u32 && L.counted < c.counted_for_protos.size()) hi = std::max<uint32_t>(hi, c.counted_for_protos[L.counted].exit_ip);
			spans.push_back(span{ L.head, hi, kind, li });
		}
		std::vector<uint32_t> loop_site(c.cell_loops.size(), k_invalid_u32);
		for (uint32_t li = 0; li < c.cell_loops.size(); ++li) {
			const auto& L = c.cell_loops[li];
			const std::string lines = "lines " + std::to_string(r.loops[li].head_line) + "-" + std::to_string(r.loops[li].back_line);
			if (L.frame_program) loop_site[li] = add_site(L.head, "frame program: general form", lines);
			else if (L.straight) loop_site[li] = add_site(L.head, "straight region: general form (entry refused or a miss)", lines);
			else loop_site[li] = add_site(L.head, "closed loop: general form", lines);
		}
		for (const auto& rf : c.cell_refusals) {
			const std::string detail = refusal_detail(c, sym, rf.ip, rf.reason, rf.head, rf.back) + " [" + class_word(rf.reason) + "] loop lines " + std::to_string(line_of(rf.head)) + "-" + std::to_string(line_of(rf.back));
			const uint32_t s = add_site(rf.ip, "loop refused: " + refusal_category(c, rf.ip, rf.reason), detail);
			spans.push_back(span{ rf.head, rf.back + 1, coverage_region::refused, s });
		}
		// loops the translation never attempted: a back edge no closed or refused loop stands for
		for (uint32_t ip = 0; ip < orig; ++ip) {
			const vm_instruction& ins = c.code[ip];
			uint32_t lo = k_invalid_u32;
			if (ins.op == opcode::op_loop_back && ins.a <= ip) lo = ins.a;
			else if (ins.op == opcode::op_jump && ins.a <= ip) lo = ins.a;
			else if (ins.op == opcode::op_cfor_back && ins.a < c.counted_for_protos.size() && c.counted_for_protos[ins.a].body_ip != k_invalid_u32 && c.counted_for_protos[ins.a].body_ip > 0) lo = c.counted_for_protos[ins.a].body_ip - 1;
			if (lo == k_invalid_u32) continue;
			bool covered = false;
			for (const span& s : spans) {
				if (s.kind == coverage_region::refused && s.hi == ip + 1) { covered = true; break; }
				if ((s.kind == coverage_region::closed) && s.lo <= lo && s.hi >= ip + 1) { covered = true; break; }
			}
			if (covered) continue;
			std::string form = std::string(opcode_name(ins.op));
			const uint32_t s = add_site(ip, std::string("loop not attempted (back edge ") + form + ")", "lines " + std::to_string(line_of(lo)) + "-" + std::to_string(line_of(ip)));
			spans.push_back(span{ lo, ip + 1, coverage_region::unattempted, s });
		}
		// the straight-line spans the region builder dropped
		std::vector<span> notes;
		for (const auto& sn : c.straight_notes) {
			std::string reason, detail;
			if (sn.reason == chunk::cell_refusal_none) {
				reason = sn.work < 16 ? "straight-line: span below the region minimum" : "straight-line: span whose statement boundaries close too little work";
				detail = std::to_string(sn.work) + " scalar ops, " + std::to_string(sn.bounded) + " to the last statement boundary";
			}
			else { reason = "straight-line: span refused (" + refusal_category(c, sn.ip, sn.reason) + ")"; detail = refusal_detail(c, sym, sn.ip, sn.reason, sn.head, sn.end) + " [" + class_word(sn.reason) + "]"; }
			const uint32_t s = add_site(sn.ip, std::move(reason), std::move(detail));
			notes.push_back(span{ sn.head, std::max(sn.end, sn.head + 1), coverage_region::none, s });
		}
		std::map<std::pair<uint64_t, std::string>, uint32_t> plain_sites;   // (file << 32 | line, reason)
		auto plain_key = [&](uint32_t at, std::string reason) { return std::make_pair((static_cast<uint64_t>(at < n ? r.file[at] : 0) << 32) | line_of(at), std::move(reason)); };
		for (uint32_t ip = 0; ip < n; ++ip) {
			if (r.region_index[ip] != k_invalid_u32 && ip >= orig) continue;   // a copy instruction
			if (ip >= orig) {
				r.region[ip] = coverage_region::trampoline;
				auto key = plain_key(ip, "region trampoline");
				auto it = plain_sites.find(key);
				r.site[ip] = it != plain_sites.end() ? it->second : (plain_sites[key] = add_site(ip, key.second, ""));
				continue;
			}
			const span* best = nullptr;
			for (const span& s : spans) {
				if (ip < s.lo || ip >= s.hi) continue;
				if (!best || (s.hi - s.lo) < (best->hi - best->lo)) best = &s;
			}
			if (best) {
				r.region[ip] = best->kind;
				r.region_index[ip] = best->index;
				if (best->kind == coverage_region::refused || best->kind == coverage_region::unattempted) { r.site[ip] = best->index; r.region_index[ip] = k_invalid_u32; }
				else {
					r.site[ip] = loop_site[best->index];
					if (best->kind == coverage_region::closed) {
						const auto& L = c.cell_loops[best->index];
						for (size_t b = 0; b < L.boundaries.size(); ++b) {
							if (ip >= L.boundaries[b].start && ip < L.boundaries[b].end) {
								r.region[ip] = coverage_region::boundary;
								auto key = plain_key(L.boundaries[b].start, "closed loop: boundary statement");
								auto it = plain_sites.find(key);
								r.site[ip] = it != plain_sites.end() ? it->second : (plain_sites[key] = add_site(L.boundaries[b].start, key.second, instruction_detail(c, sym, L.boundaries[b].start)));
								break;
							}
						}
					}
				}
				continue;
			}
			const span* note = nullptr;
			for (const span& s : notes) {
				if (ip < s.lo || ip >= s.hi) continue;
				if (!note || (s.hi - s.lo) < (note->hi - note->lo)) note = &s;
			}
			if (note) { r.site[ip] = note->index; continue; }
			const opcode op = c.code[ip].op;
			std::string reason = straight_op(op) && family_of(op) != fam::ret && family_of(op) != fam::compare && family_of(op) != fam::move
				? "straight-line: scalar op, no span (no statement boundary or entry op)" : std::string("straight-line: ") + fam_name(family_of(op));
			auto key = plain_key(ip, reason);
			auto it = plain_sites.find(key);
			r.site[ip] = it != plain_sites.end() ? it->second : (plain_sites[key] = add_site(ip, reason, ""));
		}
		for (uint32_t ip = 0; ip < orig; ++ip) {
			switch (family_of(c.code[ip].op)) {
			case fam::member: case fam::method: case fam::builtin: case fam::arith: case fam::compare: case fam::string: case fam::call: {
				std::string d = instruction_detail(c, sym, ip);
				if (family_of(c.code[ip].op) == fam::call) {
					for (const auto& kr : c.inline_refusals) {
						if (kr.line == line_of(ip) && d.find(kr.callee) != std::string::npos) { d += std::string(" [call kept: ") + (kr.reason ? kr.reason : "?") + "]"; break; }
					}
				}
				const vm_instruction& ins = c.code[ip];
				uint32_t pi = k_invalid_u32;
				if (ins.op == opcode::op_binary_fused || ins.op == opcode::op_binary_fused_temp) pi = ins.a;
				else if (ins.op == opcode::op_fused_cmp_jump) pi = ins.b;
				else if ((ins.op == opcode::op_binary_fused_decl || ins.op == opcode::op_binary_fused_store) && ins.a < c.fused_binary_dst_protos.size()) pi = c.fused_binary_dst_protos[ins.a].binary_proto;
				if (pi < c.fused_binary_protos.size()) { d += binary_detail(c, pi); r.binary_class.emplace(ip, c.fused_binary_protos[pi].typed_class); }
				r.details.emplace(ip, std::move(d));
				break;
			}
			default: break;
			}
		}
		// the calls the inliner kept: their rule joins the detail of the sites on their line
		for (auto& s : r.sites) {
			for (const auto& kr : c.inline_refusals) {
				if (kr.line != s.line || s.detail.find(kr.callee) == std::string::npos) continue;
				s.detail += std::string(" [call kept: ") + (kr.reason ? kr.reason : "?") + "]";
				break;
			}
		}
		return r;
	}

	void zero_row(coverage_chunk& r) {
		std::fill(r.general.begin(), r.general.end(), 0);
		std::fill(r.cells.begin(), r.cells.end(), 0);
		for (auto& l : r.loops) { l.entries = 0; l.misses = 0; for (auto& x : l.refused) x = 0; }
		r.receivers.clear();
		r.operands.clear();
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
		std::fill(r.cycles.begin(), r.cycles.end(), 0);
		r.inclusive_cycles = 0;
		r.entries = 0;
#endif
	}

	bool pair_first_falls(opcode o) {
		return o == opcode::op_c_arith2 || o == opcode::op_c_load2 || o == opcode::op_c_arith_load || o == opcode::op_c_load_jcmp;
	}

} // namespace

coverage_chunk& coverage_tables::bind(const chunk& c, const string_symbolizer* symbolizer) {
	auto it = chunks.find(&c);
	if (it != chunks.end() && (it->second.identity != static_cast<const void*>(c.code.data()) || it->second.size != c.code.size())) {
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
		if (timed_row == &it->second) { timed_row = nullptr; }
#endif
		retired.push_back(std::move(it->second));
		chunks.erase(it);
		it = chunks.end();
	}
	if (it == chunks.end()) { it = chunks.emplace(&c, classify(c, symbolizer)).first; }
	last = &c;
	last_row = &it->second;
	return it->second;
}

void coverage_tables::reset() {
	for (auto& [k, r] : chunks) { zero_row(r); }
	retired.clear();
	cell_dispatches = 0;
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
	timed_row = nullptr;
#endif
}

namespace {
	// the copy census's position: the instruction about to run, its row and source line
	void census_position(opcode op, const coverage_chunk& row, size_t ip) {
		const char* file = ip < row.file.size() && row.file[ip] < row.files.size() ? row.files[row.file[ip]].c_str() : "";
		census::set_position(static_cast<uint16_t>(op), &row, static_cast<uint32_t>(ip), row.name.c_str(), file, ip < row.line.size() ? row.line[ip] : 0);
	}
}

coverage_chunk& vm_backend::coverage_row(const chunk& c) {
	coverage_tables& t = *coverage_;
	if (t.last == &c && t.last_row->identity == static_cast<const void*>(c.code.data()) && t.last_row->size == c.code.size()) return *t.last_row;
	return t.bind(c, symbolizer_);
}

void vm_backend::coverage_general(frame& f) {
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
	coverage_->charge(coverage_clock());
	coverage_->timed_row = nullptr;
	struct restart_clock { coverage_tables& t; ~restart_clock() { t.timed_since = coverage_clock(); } } restart{*coverage_};
#endif
	coverage_chunk& row = coverage_row(*f.code);
	if (f.ip >= row.general.size()) return;
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
	coverage_->timed_row = &row;
	coverage_->timed_ip = f.ip;
#endif
	++row.general[f.ip];
	const opcode op = f.code->code[f.ip].op;
	census_position(op, row, f.ip);
	switch (family_of(op)) {
	case fam::member: case fam::method: case fam::builtin: case fam::arith: case fam::compare: case fam::string:
		coverage_observe(f, f.code->code[f.ip], row);
		break;
	default: break;
	}
}

#ifdef JAISCRIPT_VM_COVERAGE_TIMING
void vm_backend::coverage_frame_exit(const frame& f) {
	if (!f.code || !f.timed_entry) return;
	coverage_chunk& row = coverage_row(*f.code);
	row.inclusive_cycles += coverage_clock() - f.timed_entry;
	++row.entries;
}
#endif

void vm_backend::coverage_cell(const frame& f, size_t ip, opcode op) {
	coverage_chunk& row = coverage_row(*f.code);
	++coverage_->cell_dispatches;
	if (ip >= row.cells.size()) return;
	++row.cells[ip];
	census_position(op, row, ip);
	if (pair_first_falls(op) && ip + 1 < row.cells.size()) { ++row.cells[ip + 1]; }
}

void vm_backend::coverage_cell_pair(const frame& f, size_t ip, const vm_instruction& ci, size_t next) {
	// C_JCMP_P2: the second compare ran when the first fell through
	const size_t first_target = ci.c & 0x7FFFFFFFu;
	if (next == first_target && first_target != ip + 1) return;
	coverage_chunk& row = coverage_row(*f.code);
	if (ip + 1 < row.cells.size()) { ++row.cells[ip + 1]; }
}

void vm_backend::coverage_entry(const frame& f, uint32_t loop, bool taken) {
	coverage_chunk& row = coverage_row(*f.code);
	if (loop >= row.loops.size()) return;
	auto& lr = row.loops[loop];
	if (taken) { ++lr.entries; return; }
	if (debug_hook_ || has_custom_numeric_ops_) ++lr.refused[0];
	else if (!f.cells || !f.window_backed) ++lr.refused[1];
	else if (loop < 64 && ((f.cells->refused_loops >> loop) & 1)) ++lr.refused[2];
	else ++lr.refused[3];
}

void vm_backend::coverage_miss(const frame& f) {
	if (!f.cells) return;
	coverage_chunk& row = coverage_row(*f.code);
	if (f.cells->loop < row.loops.size()) { ++row.loops[f.cells->loop].misses; }
}

namespace {
	const script_value* deref_view(const script_value* v, script_value& tmp) {
		if (!v) return nullptr;
		if (v->raw_storage_index() == script_value::TYPEID_REFERENCE) { tmp = v->deref(); return &tmp; }
		return v;
	}
	unsigned receiver_kind(const script_value& v, const class_definition*& cd) {
		cd = nullptr;
		switch (v.raw_storage_index()) {
		case script_value::TYPEID_OBJECT: {
			cd = detail::cast_dynamic_class(v);
			if (cd && cd->is_script_class()) return k_cov_recv_instance;
			return k_cov_recv_host;
		}
		case script_value::TYPEID_CPP_BOUND: return k_cov_recv_host;
		case script_value::TYPEID_ARRAY: return k_cov_recv_array;
		case script_value::TYPEID_MAP: return k_cov_recv_map;
		case script_value::TYPEID_STRING: case script_value::TYPEID_UTF8_STRING: case script_value::TYPEID_CHAR: return k_cov_recv_string;
		case script_value::TYPEID_NULL: return k_cov_recv_null;
		default: return k_cov_recv_other;
		}
	}
	unsigned operand_kind(const script_value* v) {
		if (!v) return k_cov_opnd_unknown;
		switch (v->raw_storage_index()) {
		case script_value::TYPEID_INT: return k_cov_opnd_int;
		case script_value::TYPEID_FLOAT: return k_cov_opnd_float;
		case script_value::TYPEID_BOOL: return k_cov_opnd_bool;
		case script_value::TYPEID_CHAR: case script_value::TYPEID_STRING: case script_value::TYPEID_UTF8_STRING: return k_cov_opnd_string;
		case script_value::TYPEID_OBJECT: case script_value::TYPEID_CPP_BOUND: return k_cov_opnd_object;
		case script_value::TYPEID_ARRAY: case script_value::TYPEID_MAP: return k_cov_opnd_container;
		default: return k_cov_opnd_unknown;
		}
	}
	unsigned combine_kinds(unsigned a, unsigned b) {
		if (a == k_cov_opnd_string || b == k_cov_opnd_string) return k_cov_opnd_string;
		if (a == k_cov_opnd_object || b == k_cov_opnd_object) return k_cov_opnd_object;
		if (a == k_cov_opnd_container || b == k_cov_opnd_container) return k_cov_opnd_container;
		if (a == k_cov_opnd_unknown || b == k_cov_opnd_unknown) return a == k_cov_opnd_unknown ? b : a;
		if (a == b) return a;
		if ((a == k_cov_opnd_int && b == k_cov_opnd_float) || (a == k_cov_opnd_float && b == k_cov_opnd_int)) return k_cov_opnd_mixed;
		return k_cov_opnd_mixed;
	}
}

void vm_backend::coverage_observe(frame& f, const vm_instruction& ins, coverage_chunk& row) {
	const chunk& c = *f.code;
	const size_t depth = stack_.count;
	auto stack_at = [&](size_t from_top) -> const script_value* { return from_top < depth ? &stack_.slots[depth - 1 - from_top] : nullptr; };
	auto slot_at = [&](uint32_t slot) -> const script_value* { return slot == k_invalid_u32 ? nullptr : frame_slot(static_cast<const frame&>(f), slot); };
	auto operand_value = [&](const fused_operand& o) -> const script_value* {
		if (o.const_index != k_invalid_u32) return o.const_index < c.constants.size() ? &c.constants[o.const_index] : nullptr;
		if (o.direct && o.slot != k_invalid_u32 && !o.is_subscript() && o.member_node == k_invalid_u32) return slot_at(o.slot);
		return nullptr;
	};
	script_value tmp{std::monostate{}, nullptr}, tmp2{std::monostate{}, nullptr};
	const script_value* recv = nullptr;
	bool member = true;
	switch (ins.op) {
	case opcode::op_get_member: recv = ins.b ? slot_at(ins.b - 1) : stack_at(0); break;
	case opcode::op_get_member_path: if (ins.b < c.member_path_protos.size()) { recv = c.member_path_protos[ins.b].root_slot != k_invalid_u32 ? slot_at(c.member_path_protos[ins.b].root_slot) : stack_at(0); } break;
	case opcode::op_set_member: case opcode::op_probe_member: recv = stack_at(0); break;
	case opcode::op_member_temp: case opcode::op_set_member_fused: recv = slot_at(member_base_slot_of(ins.c)); break;
	case opcode::op_set_member_value: if (ins.a < c.member_store_protos.size()) { recv = slot_at(c.member_store_protos[ins.a].base); } break;
	case opcode::op_member_compound: recv = stack_at(2); break;
	case opcode::op_call_method: case opcode::op_call_builtin: recv = stack_at(ins.a); break;
	case opcode::op_size_temp: if (ins.b < c.call_sites.size()) { recv = slot_at(c.call_sites[ins.b].receiver_slot); } break;
	default: member = false; break;
	}
	if (member) {
		recv = deref_view(recv, tmp);
		if (!recv) return;
		const class_definition* cd = nullptr;
		const unsigned k = receiver_kind(*recv, cd);
		auto& rr = row.receivers[static_cast<uint32_t>(f.ip)];
		++rr.kinds[k];
		if (cd) {
			bool found = false;
			for (auto& [p, n] : rr.classes) { if (p == cd) { ++n; found = true; break; } }
			if (!found && rr.classes.size() < 8) rr.classes.emplace_back(cd, 1);
		}
		return;
	}
	const script_value* l = nullptr; const script_value* r = nullptr;
	bool binary = true;
	auto from_proto = [&](uint32_t pi) { if (pi < c.fused_binary_protos.size()) { l = operand_value(c.fused_binary_protos[pi].left); r = operand_value(c.fused_binary_protos[pi].right); } };
	switch (ins.op) {
	case opcode::op_binary: case opcode::op_concat_append: l = stack_at(1); r = stack_at(0); break;
	case opcode::op_binary_fused: case opcode::op_binary_fused_temp: from_proto(ins.a); break;
	case opcode::op_binary_fused_decl: case opcode::op_binary_fused_store: if (ins.a < c.fused_binary_dst_protos.size()) { from_proto(c.fused_binary_dst_protos[ins.a].binary_proto); } break;
	case opcode::op_fused_cmp_jump: from_proto(ins.b); break;
	case opcode::op_compound_fused:
		if (ins.a < c.compound_fused_protos.size()) {
			const compound_fused_proto& cp = c.compound_fused_protos[ins.a];
			l = slot_at(cp.slot);
			if (cp.rhs_proto == k_invalid_u32) r = operand_value(cp.rhs); else if (cp.rhs_proto < c.fused_binary_protos.size()) r = operand_value(c.fused_binary_protos[cp.rhs_proto].left);
		}
		break;
	case opcode::op_unary: l = stack_at(0); r = l; break;
	case opcode::op_unary_temp: if (ins.a < c.fused_binary_protos.size()) { l = operand_value(c.fused_binary_protos[ins.a].left); r = l; } break;
	case opcode::op_compound_store: l = stack_at(0); r = l; break;
	default: binary = false; break;
	}
	if (!binary) return;
	l = deref_view(l, tmp); r = deref_view(r, tmp2);
	const unsigned k = combine_kinds(operand_kind(l), operand_kind(r));
	++row.operands[static_cast<uint32_t>(f.ip)][k];
}

namespace {
	struct agg_reason {
		uint64_t typed = 0, untyped = 0;
		uint64_t fams[static_cast<size_t>(fam::count)] = {};
		std::map<std::string, uint64_t> sites;
	};
	std::string pct(uint64_t part, uint64_t whole) {
		char b[32];
		std::snprintf(b, sizeof b, "%5.1f%%", whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0);
		return b;
	}
	std::string num(uint64_t v) {
		char b[32];
		if (v >= 10000000ull) std::snprintf(b, sizeof b, "%.2fM", static_cast<double>(v) / 1e6);
		else std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(v));
		return b;
	}
	std::string pad(std::string s, size_t w) { if (s.size() < w) s.append(w - s.size(), ' '); return s; }
	std::string lpad(std::string s, size_t w) { if (s.size() < w) s.insert(0, w - s.size(), ' '); return s; }
	std::string instruction_detail_row(const coverage_chunk& r, size_t ip) {
		auto it = r.details.find(static_cast<uint32_t>(ip));
		return it != r.details.end() ? it->second : std::string(opcode_name(static_cast<opcode>(r.op[ip])));
	}
	std::string where(const coverage_chunk& r, uint16_t file, uint32_t line) {
		const std::string& f = file < r.files.size() ? r.files[file] : r.files.front();
		return r.name + " " + (f.empty() ? std::string() : f + ":") + "L" + std::to_string(line);
	}
	std::string class_shape(const class_definition* cd) {
		if (!cd) return "?";
		if (!cd->is_script_class()) return "host class";
		if (cd->flat_value_semantics()) return cd->handle_bearing_slots().empty() ? "flat scalar record" : "value class (strings / containers)";
		return "class with var / auto / handle fields";
	}
	const char* refused_name(int k) {
		switch (k) { case 0: return "debugger or numeric operator"; case 1: return "no cell area (fiber or native frame)"; case 2: return "refusal valve"; default: return "entry guard or container site"; }
	}
	// why the general instruction at ip ran there: its site's reason and detail, a closed loop's by its run-time entries
	void general_reason(const coverage_chunk& r, size_t ip, std::string& reason, std::string& detail, uint32_t& site_line, uint16_t& site_file) {
		reason = "?";
		detail.clear();
		site_line = r.line[ip];
		site_file = r.file[ip];
		if (r.site[ip] < r.sites.size()) { const auto& s = r.sites[r.site[ip]]; reason = s.reason; detail = s.detail; site_line = s.line; site_file = s.file; }
		if (r.region[ip] != coverage_region::closed || r.region_index[ip] >= r.loops.size()) return;
		const auto& lr = r.loops[r.region_index[ip]];
		uint64_t refused = 0; int top_k = 3;
		for (int k = 0; k < 4; ++k) { refused += lr.refused[k]; if (lr.refused[k] > lr.refused[top_k]) top_k = k; }
		if (refused > lr.entries) reason = std::string("closed loop: block entry refused at run time (") + refused_name(top_k) + ")";
		else if (lr.misses * 2 > lr.entries && lr.misses > 0) reason = "closed loop: misses back to the general form";
		else reason = "closed loop: iterations before the block entry";
	}
	std::vector<const coverage_chunk*> all_rows(const std::vector<std::shared_ptr<coverage_tables>>& tables, uint64_t& cell_dispatches) {
		std::vector<const coverage_chunk*> rows;
		cell_dispatches = 0;
		for (const auto& t : tables) {
			if (!t) continue;
			cell_dispatches += t->cell_dispatches;
			for (const auto& [k, r] : t->chunks) rows.push_back(&r);
			for (const auto& r : t->retired) rows.push_back(&r);
		}
		return rows;
	}
}

void coverage_summary(const std::vector<std::shared_ptr<coverage_tables>>& tables, vm_coverage_totals& out) {
	out = vm_coverage_totals{};
	uint64_t cell_dispatches = 0;
	std::map<std::string, uint64_t> reasons;
	std::string reason, detail;
	uint32_t line = 0;
	uint16_t file = 0;
	for (const coverage_chunk* rp : all_rows(tables, cell_dispatches)) {
		const coverage_chunk& r = *rp;
		for (size_t ip = 0; ip < r.general.size(); ++ip) {
			const opcode op = static_cast<opcode>(r.op[ip]);
			if (const uint64_t n = r.cells[ip]) {
				out.cells += n;
				const coverage_region k = r.region[ip];
				(k == coverage_region::frame_program ? out.cells_frame : k == coverage_region::straight ? out.cells_straight : out.cells_closed) += n;
			}
			const uint64_t n = r.general[ip];
			if (!n) continue;
			const fam f = family_of(op);
			if (f == fam::block || f >= fam::cell_arith) { out.block_entry += n; continue; }
			(typed_op(op) ? out.typed : out.untyped) += n;
			general_reason(r, ip, reason, detail, line, file);
			reasons[reason] += n;
		}
		for (const auto& lr : r.loops) { if (!lr.straight && !lr.frame_program && lr.entries) ++out.closed_entered; }
	}
	out.operations = out.cells + out.block_entry + out.typed + out.untyped;
	for (const auto& [k, n] : reasons) out.reasons.emplace_back(k, n);
	std::stable_sort(out.reasons.begin(), out.reasons.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
}

namespace {
	// every opcode that ran, on the dispatcher and in blocks; a timing build adds each one's own clock and the script functions'
	void opcode_tables(std::ostringstream& o, const std::vector<const coverage_chunk*>& rows, uint64_t total, size_t top) {
		struct op_row { uint64_t general = 0, cells = 0, cycles = 0; };
		std::vector<op_row> ops(k_opcode_space);
		for (const coverage_chunk* rp : rows) {
			for (size_t ip = 0; ip < rp->general.size(); ++ip) {
				op_row& e = ops[rp->op[ip] < k_opcode_space ? rp->op[ip] : 0];
				e.general += rp->general[ip];
				e.cells += rp->cells[ip];
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
				e.cycles += rp->cycles[ip];
#endif
			}
		}
		std::vector<std::pair<uint64_t, size_t>> ranked;
		for (size_t k = 0; k < ops.size(); ++k) if (ops[k].general + ops[k].cells) ranked.emplace_back(ops[k].general + ops[k].cells, k);
		std::sort(ranked.rbegin(), ranked.rend());
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
		uint64_t all_cycles = 0;
		for (const op_row& e : ops) all_cycles += e.cycles;
		o << "\n-- instructions by opcode (dispatcher | in blocks | share | own clock, a block entry's covering its run | share | clock per dispatch)\n";
#else
		o << "\n-- instructions by opcode (dispatcher | in blocks | share)\n";
#endif
		for (const auto& [n, k] : ranked) {
			const op_row& e = ops[k];
			o << "  " << pad(std::string(opcode_name(static_cast<opcode>(k))), 28) << lpad(num(e.general), 14) << lpad(num(e.cells), 14) << "  " << pct(n, total);
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
			o << "  " << lpad(num(e.cycles), 16) << "  " << pct(e.cycles, all_cycles) << "  " << (e.general ? e.cycles / e.general : 0);
#endif
			o << "\n";
		}
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
		struct fn_row { std::string name; uint64_t exclusive = 0, ops = 0, inclusive = 0, entries = 0; std::map<uint32_t, uint64_t> lines; };
		std::map<std::string, fn_row> fns;
		for (const coverage_chunk* rp : rows) {
			std::string file = rp->files.empty() ? std::string() : rp->files.front();
			if (const size_t cut = file.find_last_of("/\\"); cut != std::string::npos) file = file.substr(cut + 1);
			fn_row& fr = fns[rp->name + "\t" + file];
			fr.name = (rp->name.empty() ? std::string("<top level>") : rp->name) + (file.empty() ? std::string() : "  " + file);
			fr.inclusive += rp->inclusive_cycles;
			fr.entries += rp->entries;
			for (size_t ip = 0; ip < rp->cycles.size(); ++ip) {
				fr.exclusive += rp->cycles[ip];
				fr.ops += rp->general[ip] + rp->cells[ip];
				if (rp->cycles[ip]) fr.lines[rp->line[ip]] += rp->cycles[ip];
			}
		}
		std::vector<const fn_row*> by_time;
		for (const auto& [k, fr] : fns) if (fr.exclusive) by_time.push_back(&fr);
		std::sort(by_time.begin(), by_time.end(), [](const fn_row* a, const fn_row* b) { return a->exclusive > b->exclusive; });
		o << "\n-- script functions by own clock (exclusive: its instructions, callees' excluded, the call and return machinery included; inclusive: frame entry to exit, a recursive activation counted again)\n";
		o << "  " << pad("function", 52) << lpad("exclusive", 16) << "        " << lpad("operations", 14) << lpad("inclusive", 16) << lpad("entries", 12) << "\n";
		for (size_t i = 0; i < by_time.size() && i < top; ++i) {
			const fn_row& fr = *by_time[i];
			o << "  " << pad(fr.name, 52) << lpad(num(fr.exclusive), 16) << "  " << pct(fr.exclusive, all_cycles) << lpad(num(fr.ops), 14) << lpad(num(fr.inclusive), 16) << lpad(num(fr.entries), 12) << "\n";
			std::vector<std::pair<uint64_t, uint32_t>> lines;
			for (const auto& [line, c] : fr.lines) lines.emplace_back(c, line);
			std::sort(lines.rbegin(), lines.rend());
			o << "      hot lines:";
			for (size_t l = 0; l < lines.size() && l < 6; ++l) o << " L" << lines[l].second << " " << pct(lines[l].first, fr.exclusive);
			o << "\n";
		}
#else
		(void)top;
#endif
	}
}

std::string coverage_report(const std::vector<std::shared_ptr<coverage_tables>>& tables, size_t top) {
	uint64_t cell_dispatches = 0;
	const std::vector<const coverage_chunk*> rows = all_rows(tables, cell_dispatches);
	uint64_t cells_ops = 0, block_entry = 0, typed = 0, untyped = 0;
	uint64_t cell_fams[static_cast<size_t>(fam::count)] = {};
	uint64_t cell_kinds[8] = {};   // by block kind: closed loop, straight region, frame program
	uint64_t gen_fams_typed[static_cast<size_t>(fam::count)] = {}, gen_fams_untyped[static_cast<size_t>(fam::count)] = {};
	std::map<std::string, agg_reason> reasons;
	std::map<std::string, uint64_t> cell_sites;
	struct loop_agg { std::string where; uint64_t general = 0, cells = 0, entries = 0, misses = 0, refused[4] = {}; bool straight = false, fp = false; };
	std::vector<loop_agg> loops;
	uint64_t recv_kinds[8] = {};
	std::map<std::string, uint64_t> recv_classes;   // "Class (shape)" -> ops
	std::map<std::string, uint64_t> recv_shapes;
	std::map<std::string, uint64_t> recv_sites;
	uint64_t opnd[8] = {};
	std::map<std::string, uint64_t> untyped_scalar_sites, proven_scalar_sites, string_sites;
	uint64_t scalar_proven = 0, scalar_unproven = 0;
	std::map<std::string, uint64_t> general_calls;   // calls made on the general form, by callee (and the inliner's rule when it kept the call)
	uint64_t general_call_count = 0;
	for (const coverage_chunk* rp : rows) {
		const coverage_chunk& r = *rp;
		std::vector<uint64_t> loop_general(r.loops.size(), 0), loop_cells(r.loops.size(), 0);
		for (size_t ip = 0; ip < r.general.size(); ++ip) {
			const opcode op = static_cast<opcode>(r.op[ip]);
			if (r.cells[ip]) {
				const uint64_t n = r.cells[ip];
				cells_ops += n;
				cell_fams[static_cast<size_t>(family_of(op))] += n;
				const coverage_region k = r.region[ip];
				cell_kinds[k == coverage_region::frame_program ? 2 : k == coverage_region::straight ? 1 : 0] += n;
				cell_sites[where(r, r.file[ip], r.line[ip])] += n;
				if (r.region_index[ip] < loop_cells.size()) loop_cells[r.region_index[ip]] += n;
			}
			if (!r.general[ip]) continue;
			const uint64_t n = r.general[ip];
			const fam f = family_of(op);
			if (f == fam::block || f >= fam::cell_arith) { block_entry += n; continue; }   // a cell instruction on the dispatcher: a door's landing after its callee
			const bool t = typed_op(op);
			(t ? typed : untyped) += n;
			(t ? gen_fams_typed : gen_fams_untyped)[static_cast<size_t>(f)] += n;
			std::string reason, detail;
			uint32_t site_line = 0;
			uint16_t site_file = 0;
			general_reason(r, ip, reason, detail, site_line, site_file);
			const coverage_region rg = r.region[ip];
			if ((rg == coverage_region::closed || rg == coverage_region::boundary || rg == coverage_region::straight || rg == coverage_region::frame_program) && r.region_index[ip] < r.loops.size())
				loop_general[r.region_index[ip]] += n;
			if (op == opcode::op_call_from_scratch || op == opcode::op_call_typed || op == opcode::op_call || op == opcode::op_call_method) {
				general_call_count += n;
				std::string d = instruction_detail_row(r, ip);
				const size_t sp = d.find(' ');
				general_calls[sp == std::string::npos ? d : d.substr(sp + 1)] += n;
			}
			auto& ag = reasons[reason];
			(t ? ag.typed : ag.untyped) += n;
			ag.fams[static_cast<size_t>(f)] += n;
			ag.sites[where(r, site_file, site_line) + (detail.empty() ? std::string() : "  " + detail)] += n;
			auto ri = r.receivers.find(static_cast<uint32_t>(ip));
			if (ri != r.receivers.end()) {
				for (int k = 0; k < 8; ++k) recv_kinds[k] += ri->second.kinds[k];
				for (const auto& [cd, cnt] : ri->second.classes) {
					if (!cd || !cd->is_script_class()) continue;
					const std::string shape = class_shape(cd);
					recv_classes[cd->get_name() + " (" + shape + ")"] += cnt;
					recv_shapes[shape] += cnt;
				}
				recv_sites[where(r, r.file[ip], r.line[ip]) + "  " + instruction_detail_row(r, ip)] += n;
			}
			auto oi = r.operands.find(static_cast<uint32_t>(ip));
			if (oi != r.operands.end() && !t) {
				uint64_t scalar = 0, str = 0;
				for (int k = 0; k < 8; ++k) opnd[k] += oi->second[k];
				scalar = oi->second[k_cov_opnd_int] + oi->second[k_cov_opnd_float] + oi->second[k_cov_opnd_mixed] + oi->second[k_cov_opnd_bool];
				str = oi->second[k_cov_opnd_string];
				auto bc = r.binary_class.find(static_cast<uint32_t>(ip));
				const bool proven = bc != r.binary_class.end() && bc->second != 0;
				(proven ? scalar_proven : scalar_unproven) += scalar;
				if (scalar) (proven ? proven_scalar_sites : untyped_scalar_sites)[where(r, r.file[ip], r.line[ip]) + "  " + instruction_detail_row(r, ip)] += scalar;
				if (str) string_sites[where(r, r.file[ip], r.line[ip]) + "  " + instruction_detail_row(r, ip)] += str;
			}
		}
		for (size_t li = 0; li < r.loops.size(); ++li) {
			const auto& lr = r.loops[li];
			if (!loop_general[li] && !loop_cells[li] && !lr.entries) continue;
			loop_agg a;
			a.where = where(r, lr.head_file, lr.head_line) + "-" + std::to_string(lr.back_line);
			a.general = loop_general[li]; a.cells = loop_cells[li]; a.entries = lr.entries; a.misses = lr.misses; a.straight = lr.straight; a.fp = lr.frame_program;
			for (int k = 0; k < 4; ++k) a.refused[k] = lr.refused[k];
			loops.push_back(std::move(a));
		}
	}
	const uint64_t total = cells_ops + block_entry + typed + untyped;
	std::ostringstream o;
	o << "== VM execution coverage (operations: each cell instruction stands for one general instruction; a fused pair counts both halves)\n";
	o << "executed operations " << num(total) << " | cell dispatches " << num(cell_dispatches) << " | chunks " << rows.size() << "\n\n";
	o << "-- classes\n";
	o << pad("(a) inside cell blocks", 52) << lpad(num(cells_ops), 12) << "  " << pct(cells_ops, total) << "\n";
	o << pad("      closed loops", 52) << lpad(num(cell_kinds[0]), 12) << "  " << pct(cell_kinds[0], total) << "\n";
	o << pad("      straight-line regions", 52) << lpad(num(cell_kinds[1]), 12) << "  " << pct(cell_kinds[1], total) << "\n";
	o << pad("      frame programs", 52) << lpad(num(cell_kinds[2]), 12) << "  " << pct(cell_kinds[2], total) << "\n";
	o << pad("(a') block entries / resumes on the dispatcher", 52) << lpad(num(block_entry), 12) << "  " << pct(block_entry, total) << "\n";
	o << pad("(b) typed op_t_* on the general form", 52) << lpad(num(typed), 12) << "  " << pct(typed, total) << "\n";
	o << pad("(c) general form, untyped", 52) << lpad(num(untyped), 12) << "  " << pct(untyped, total) << "\n\n";
	o << "-- cell operations by family\n";
	{
		std::vector<std::pair<uint64_t, size_t>> v;
		for (size_t k = 0; k < static_cast<size_t>(fam::count); ++k) if (cell_fams[k]) v.emplace_back(cell_fams[k], k);
		std::sort(v.rbegin(), v.rend());
		for (const auto& [n, k] : v) o << "  " << pad(fam_name(static_cast<fam>(k)), 50) << lpad(num(n), 12) << "  " << pct(n, total) << "\n";
	}
	o << "\n-- general-form operations by instruction family (b typed | c untyped)\n";
	{
		std::vector<std::pair<uint64_t, size_t>> v;
		for (size_t k = 0; k < static_cast<size_t>(fam::count); ++k) if (gen_fams_typed[k] + gen_fams_untyped[k]) v.emplace_back(gen_fams_typed[k] + gen_fams_untyped[k], k);
		std::sort(v.rbegin(), v.rend());
		for (const auto& [n, k] : v) o << "  " << pad(fam_name(static_cast<fam>(k)), 50) << lpad(num(n), 12) << "  " << pct(n, total) << "   b " << num(gen_fams_typed[k]) << " | c " << num(gen_fams_untyped[k]) << "\n";
	}
	std::vector<std::pair<uint64_t, std::string>> ranked;
	for (const auto& [k, a] : reasons) ranked.emplace_back(a.typed + a.untyped, k);
	std::sort(ranked.rbegin(), ranked.rend());
	o << "\n-- general-form operations by reason (share of all operations; b typed | c untyped; top families)\n";
	for (const auto& [n, k] : ranked) {
		const agg_reason& a = reasons[k];
		std::vector<std::pair<uint64_t, size_t>> fv;
		for (size_t f = 0; f < static_cast<size_t>(fam::count); ++f) if (a.fams[f]) fv.emplace_back(a.fams[f], f);
		std::sort(fv.rbegin(), fv.rend());
		o << "  " << pad(k, 72) << lpad(num(n), 12) << "  " << pct(n, total) << "   b " << num(a.typed) << " | c " << num(a.untyped) << "\n";
		o << "      ";
		for (size_t f = 0; f < fv.size() && f < 3; ++f) o << (f ? "; " : "") << fam_name(static_cast<fam>(fv[f].second)) << " " << pct(fv[f].first, n);
		o << "\n";
	}
	o << "\n-- top sites per reason (function file:line of the attributed site, its detail)\n";
	for (const auto& [n, k] : ranked) {
		const agg_reason& a = reasons[k];
		if (n * 1000 < total) continue;   // reasons under 0.1% of the run list no sites
		o << "  " << k << "  (" << num(n) << ", " << pct(n, total) << ")\n";
		std::vector<std::pair<uint64_t, std::string>> sv;
		for (const auto& [s, c] : a.sites) sv.emplace_back(c, s);
		std::sort(sv.rbegin(), sv.rend());
		for (size_t i = 0; i < sv.size() && i < top; ++i) o << "      " << lpad(num(sv[i].first), 10) << "  " << pct(sv[i].first, total) << "  " << sv[i].second << "\n";
	}
	o << "\n-- member and method operations on the general form by receiver\n";
	{
		static const char* const kn[] = { "script class instance", "host object", "array", "map", "string", "null", "other", "-" };
		uint64_t all = 0; for (int k = 0; k < 7; ++k) all += recv_kinds[k];
		for (int k = 0; k < 7; ++k) if (recv_kinds[k]) o << "  " << pad(kn[k], 50) << lpad(num(recv_kinds[k]), 12) << "  " << pct(recv_kinds[k], all) << " of member ops\n";
		o << "  instance receivers by class shape:\n";
		std::vector<std::pair<uint64_t, std::string>> v;
		for (const auto& [s, c] : recv_shapes) v.emplace_back(c, s);
		std::sort(v.rbegin(), v.rend());
		for (const auto& [c, s] : v) o << "    " << pad(s, 48) << lpad(num(c), 12) << "  " << pct(c, all) << "\n";
		o << "  top classes:\n";
		v.clear();
		for (const auto& [s, c] : recv_classes) v.emplace_back(c, s);
		std::sort(v.rbegin(), v.rend());
		for (size_t i = 0; i < v.size() && i < top; ++i) o << "    " << pad(v[i].second, 60) << lpad(num(v[i].first), 12) << "  " << pct(v[i].first, total) << "\n";
		o << "  top member sites:\n";
		v.clear();
		for (const auto& [s, c] : recv_sites) v.emplace_back(c, s);
		std::sort(v.rbegin(), v.rend());
		for (size_t i = 0; i < v.size() && i < top; ++i) o << "    " << lpad(num(v[i].first), 10) << "  " << pct(v[i].first, total) << "  " << v[i].second << "\n";
	}
	o << "\n-- untyped arithmetic and comparisons on the general form by operand kinds\n";
	{
		static const char* const kn[] = { "int / int", "float / float", "int / float mixed", "bool", "string", "object (operator overload)", "container", "unknown (environment or element operand)" };
		uint64_t all = 0; for (int k = 0; k < 8; ++k) all += opnd[k];
		for (int k = 0; k < 8; ++k) if (opnd[k]) o << "  " << pad(kn[k], 50) << lpad(num(opnd[k]), 12) << "  " << pct(opnd[k], all) << "\n";
		o << "  scalar operands the checker proved (class 1/2), left general by the lowering's shape rules: " << num(scalar_proven) << "\n";
		o << "  scalar operands the checker left unproven (class 0, BINARY, UNARY): " << num(scalar_unproven) << "\n";
		std::vector<std::pair<uint64_t, std::string>> v;
		for (const auto& [s, c] : proven_scalar_sites) v.emplace_back(c, s);
		std::sort(v.rbegin(), v.rend());
		o << "  top proven scalar sites left general:\n";
		for (size_t i = 0; i < v.size() && i < top; ++i) o << "    " << lpad(num(v[i].first), 10) << "  " << pct(v[i].first, total) << "  " << v[i].second << "\n";
		v.clear();
		for (const auto& [s, c] : untyped_scalar_sites) v.emplace_back(c, s);
		std::sort(v.rbegin(), v.rend());
		o << "  top scalar sites the checker left unproven:\n";
		for (size_t i = 0; i < v.size() && i < top; ++i) o << "    " << lpad(num(v[i].first), 10) << "  " << pct(v[i].first, total) << "  " << v[i].second << "\n";
		v.clear();
		for (const auto& [s, c] : string_sites) v.emplace_back(c, s);
		std::sort(v.rbegin(), v.rend());
		o << "  top string sites:\n";
		for (size_t i = 0; i < v.size() && i < top; ++i) o << "    " << lpad(num(v[i].first), 10) << "  " << pct(v[i].first, total) << "  " << v[i].second << "\n";
	}
	o << "\n-- calls made on the general form by callee (" << num(general_call_count) << " calls; the callee's own instructions count under their own reasons)\n";
	{
		std::vector<std::pair<uint64_t, std::string>> v;
		for (const auto& [s, c] : general_calls) v.emplace_back(c, s);
		std::sort(v.rbegin(), v.rend());
		for (size_t i = 0; i < v.size() && i < top; ++i) o << "    " << lpad(num(v[i].first), 10) << "  " << v[i].second << "\n";
	}
	o << "\n-- blocks by general-form operations inside their span (cells = operations in the block)\n";
	{
		std::sort(loops.begin(), loops.end(), [](const loop_agg& a, const loop_agg& b) { return a.general + a.cells / 64 > b.general + b.cells / 64; });
		for (size_t i = 0; i < loops.size() && i < top; ++i) {
			const loop_agg& a = loops[i];
			o << "  " << pad(a.where + (a.fp ? " [frame program]" : a.straight ? " [region]" : ""), 60) << " general " << lpad(num(a.general), 10) << "  cells " << lpad(num(a.cells), 10)
			  << "  entries " << num(a.entries) << "  misses " << num(a.misses) << "  refused " << num(a.refused[0] + a.refused[1] + a.refused[2] + a.refused[3]);
			if (a.refused[0] + a.refused[1] + a.refused[2] + a.refused[3]) o << " (" << num(a.refused[0]) << "/" << num(a.refused[1]) << "/" << num(a.refused[2]) << "/" << num(a.refused[3]) << ")";
			o << "\n";
		}
	}
	o << "\n-- top cell sites (operations in blocks, by origin line)\n";
	{
		std::vector<std::pair<uint64_t, std::string>> v;
		for (const auto& [s, c] : cell_sites) v.emplace_back(c, s);
		std::sort(v.rbegin(), v.rend());
		for (size_t i = 0; i < v.size() && i < top; ++i) o << "    " << lpad(num(v[i].first), 10) << "  " << pct(v[i].first, total) << "  " << v[i].second << "\n";
	}
	opcode_tables(o, rows, total, top);
	return o.str();
}

} // namespace jai::vm

// ===== The copy and front-loading census (detail/copy_census.hpp) =====
// Per-thread tables (registered once, never freed), merged and written at exit to JAI_COPY_CENSUS=<file> (a report) and
// <file>.tsv (one row per site, flags, opcode and caller address). Caller addresses are symbolized with their inline chains.

#include <jaiscript/detail/copy_census.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>
#if defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#pragma comment(lib, "dbghelp.lib")
#define JAI_CENSUS_NOINLINE __declspec(noinline)
#else
#define JAI_CENSUS_NOINLINE __attribute__((noinline))
#endif

namespace jai::census {

namespace {
	constexpr size_t k_sites = static_cast<size_t>(site::count);
	const char* const k_site_ids[] = {
#define JAI_CENSUS_ID(id, group, text) #id,
		JAI_CENSUS_SITES(JAI_CENSUS_ID)
#undef JAI_CENSUS_ID
	};
	const char* const k_site_groups[] = {
#define JAI_CENSUS_GROUP(id, group, text) group,
		JAI_CENSUS_SITES(JAI_CENSUS_GROUP)
#undef JAI_CENSUS_GROUP
	};
	const char* const k_site_texts[] = {
#define JAI_CENSUS_TEXT(id, group, text) text,
		JAI_CENSUS_SITES(JAI_CENSUS_TEXT)
#undef JAI_CENSUS_TEXT
	};
	constexpr uint16_t k_no_op = 0xFFFF;

	inline uint64_t mix64(uint64_t x) noexcept { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return x; }

	struct ra_key {
		const void* ra; uint32_t flags; uint16_t s; uint16_t op;
		bool operator==(const ra_key&) const = default;
	};
	struct ra_hash { size_t operator()(const ra_key& k) const noexcept { return static_cast<size_t>(mix64(reinterpret_cast<uintptr_t>(k.ra) ^ (uint64_t(k.flags) << 24) ^ (uint64_t(k.s) << 56) ^ (uint64_t(k.op) << 40))); } };
	struct stats { uint64_t n = 0, weight = 0, unique = 0, src_watched = 0, src_same = 0, src_soon = 0, dst_watched = 0, dst_same = 0, dst_soon = 0; };

	struct line_key {
		const void* row; uint32_t ip; uint32_t flags; uint16_t s;
		bool operator==(const line_key&) const = default;
	};
	struct line_hash { size_t operator()(const line_key& k) const noexcept { return static_cast<size_t>(mix64(reinterpret_cast<uintptr_t>(k.row) ^ (uint64_t(k.ip) << 20) ^ (uint64_t(k.flags) << 40) ^ (uint64_t(k.s) << 58))); } };
	struct line_stats { uint64_t n = 0, weight = 0; std::string where; };

	struct watch { stats* st; uint64_t instr; bool dst; };

	struct thread_tables {
		std::unordered_map<ra_key, stats, ra_hash> by_ra;
		std::unordered_map<line_key, line_stats, line_hash> by_line;
		std::unordered_map<std::string, uint64_t> by_name;
		std::unordered_map<const void*, watch> watched;
		uint64_t instr = 0;
		uint16_t op = k_no_op;
		const void* row = nullptr;
		uint32_t ip = 0;
		const char* name = "";
		const char* file = "";
		uint32_t line = 0;
		int depth = 0;
		uint64_t weight = 0;
	};

	// a deep copy's caller stack: the event's address is a token naming the interned stack (the symbolizer prints its frames)
	constexpr size_t k_stack_frames = 6;
	struct stack_rec { const void* f[k_stack_frames] = {}; };
	struct stack_hash { size_t operator()(const stack_rec& s) const noexcept { uint64_t x = 0; for (const void* p : s.f) x = mix64(x ^ reinterpret_cast<uintptr_t>(p)); return static_cast<size_t>(x); } };
	struct stack_eq { bool operator()(const stack_rec& a, const stack_rec& b) const noexcept { return std::memcmp(a.f, b.f, sizeof a.f) == 0; } };

	struct registry { std::mutex m; std::vector<thread_tables*> all; std::unordered_map<const void*, const stack_rec*> stacks; };
	registry& reg() { static registry* r = new registry; return *r; }   // never destroyed: hooks run during teardown

	const void* stack_token(unsigned skip) {
		stack_rec s;
#if defined(_MSC_VER)
		RtlCaptureStackBackTrace(skip + 1, static_cast<DWORD>(k_stack_frames), const_cast<void**>(s.f), nullptr);
#endif
		thread_local std::unordered_map<stack_rec, const stack_rec*, stack_hash, stack_eq>* tokens = nullptr;
		if (!tokens) tokens = new std::unordered_map<stack_rec, const stack_rec*, stack_hash, stack_eq>;
		auto it = tokens->find(s);
		if (it != tokens->end()) return it->second;
		const stack_rec* kept = new stack_rec(s);
		tokens->emplace(s, kept);
		std::lock_guard<std::mutex> lock(reg().m);
		reg().stacks.emplace(kept, kept);
		return kept;
	}

	thread_tables& tt() {
		thread_local thread_tables* t = nullptr;
		if (!t) [[unlikely]] {
			t = new thread_tables;
			std::lock_guard<std::mutex> lock(reg().m);
			reg().all.push_back(t);
		}
		return *t;
	}

	stats& record(thread_tables& t, site s, const void* where, uint32_t flags, uint64_t weight) {
		stats& st = t.by_ra[ra_key{where, flags, static_cast<uint16_t>(s), t.op}];
		++st.n;
		st.weight += weight;
		line_stats& ls = t.by_line[line_key{t.row, t.ip, flags, static_cast<uint16_t>(s)}];
		if (ls.n == 0) {
			if (!t.row) { ls.where = "(no vm position)"; }
			else {
				std::string file = t.file ? t.file : "";
				const size_t slash = file.find_last_of("/\\");
				if (slash != std::string::npos) file = file.substr(slash + 1);
				ls.where = std::string(t.name && *t.name ? t.name : "<top>") + " @ " + file + ":" + std::to_string(t.line);
			}
		}
		++ls.n;
		ls.weight += weight;
		return st;
	}
}

// a lookup or an elision records its caller stack (its hooks sit in small functions the vm calls from many places); the
// refcount hooks keep the one address
JAI_CENSUS_NOINLINE void hit(site s, uint32_t flags) noexcept {
	const bool stack = s != site::rc_add && s != site::value_copy_ctor && s != site::value_copy_assign;
	record(tt(), s, stack ? stack_token(1) : JAI_CENSUS_RA(), flags, 1);
}
JAI_CENSUS_NOINLINE void hit_at(site s, const void* where, uint32_t flags) noexcept { record(tt(), s, where, flags, 1); }
JAI_CENSUS_NOINLINE void hit_named(site s, const char* name, size_t name_size, uint32_t flags) noexcept {
	thread_tables& t = tt();
	record(t, s, stack_token(1), flags, 1);
	std::string key = std::to_string(static_cast<unsigned>(s)) + "\t" + std::to_string(flags) + "\t" + std::string(name ? name : "", name ? name_size : 0);
	++t.by_name[key];
}
JAI_CENSUS_NOINLINE void rc_add() noexcept { record(tt(), site::rc_add, JAI_CENSUS_RA(), 0, 1); }

void set_position(uint16_t op, const void* row, uint32_t ip, const char* name, const char* file, uint32_t line) noexcept {
	thread_tables& t = tt();
	++t.instr;
	t.op = op; t.row = row; t.ip = ip; t.name = name; t.file = file; t.line = line;
}

void add_weight(uint64_t n) noexcept { tt().weight += n; }

JAI_CENSUS_NOINLINE copy_scope::copy_scope(site s_, const void* where_, const void* src_, uint32_t flags_) noexcept
	: s(s_), where(where_ ? where_ : JAI_CENSUS_RA()), src(src_), flags(flags_), outer(false) {
	thread_tables& t = tt();
	outer = t.depth == 0;
	++t.depth;
	if (outer) { t.weight = 0; where = stack_token(1); } else { ++t.weight; }
}

void copy_scope::finish(const void* dst) noexcept { dst_ = dst; }

copy_scope::~copy_scope() {
	thread_tables& t = tt();
	--t.depth;
	if (!outer) return;
	stats& st = record(t, s, where, flags, t.weight + 1);
	if (flags & k_src_unique) ++st.unique;
	if (src) { ++st.src_watched; t.watched[src] = watch{&st, t.instr, false}; }
	if (dst_ && dst_ != src) { ++st.dst_watched; t.watched[dst_] = watch{&st, t.instr, true}; }
}

void node_freed(const void* node) noexcept {
	thread_tables& t = tt();
	if (t.watched.empty()) return;
	auto it = t.watched.find(node);
	if (it == t.watched.end()) return;
	const uint64_t d = t.instr - it->second.instr;
	stats& st = *it->second.st;
	if (it->second.dst) { if (d == 0) ++st.dst_same; else if (d <= 8) ++st.dst_soon; }
	else { if (d == 0) ++st.src_same; else if (d <= 8) ++st.src_soon; }
	t.watched.erase(it);
}

namespace {
	const char* raw_index_name(uint32_t i) {
		static const char* const names[] = { "null", "int", "float", "string", "char", "bool", "array", "map", "object", "function", "reference",
			"shared_ptr", "weak_ptr", "invalid", "cpp_bound", "parallel_borrow", "utf8_string", "vec2", "ivec2", "vec3", "ivec3", "vec4", "color" };
		return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
	}
	std::string flags_text(site s, uint32_t f) {
		switch (s) {
		case site::deep_clone: case site::copy_assign: case site::string_detach: {
			static const char* const kinds[] = { "other", "array<Class>", "typed array", "array<var>", "map", "instance", "cpp object", "shared_ptr deep", "reference", "string" };
			const uint32_t k = (f >> 4) & 0xF;
			std::string out = s == site::deep_clone ? (k < 10 ? kinds[k] : "?") : "";
			if (f & k_src_unique) out += out.empty() ? "src unique" : ", src unique";
			return out.empty() ? "-" : out;
		}
		case site::param_clone: case site::decl_clone: {
			static const char* const why[] = { "?", "no alias fn (lambda/method/unproven callee)", "ref escaping", "alias disallowed (kind)", "read-only proof refused",
				"no alias home (not a candidate)", "local alias refused" };
			static const char* const elide[] = { "", "args not on stack", "argument is a reference", "argument shared (not unique)", "converted (other node)" };
			const uint32_t w = f & 0xFF, e = (f >> 8) & 0xF;
			std::string out = w < 7 ? why[w] : "?";
			if (e && e < 5) out += std::string("; elide: ") + elide[e];
			return out;
		}
		case site::value_copy_ctor: case site::value_copy_assign: case site::map_key_hash:
			return f == 0x80 ? "string probe" : raw_index_name(f);
		case site::typed_slot_convert: {
			static const char* const verdicts[] = { "identity", "identity_exact", "converted", "reject", "reject_class", "container_mismatch", "handle_target", "needs_ctor", "needs_to_x", "needs_to_bool" };
			std::string out = (f & 0x80) ? "failed" : ((f & 0x3F) >= 1 && (f & 0x3F) <= 10 ? verdicts[(f & 0x3F) - 1] : "?");
			if (f & 0x40) out += " (untyped target)";
			return out;
		}
		case site::compile_chunk: {
			static const char* const forms[] = { "?", "program", "callable", "standalone expression", "standalone declaration" };
			return f < 5 ? forms[f] : "?";
		}
		case site::field_slot_scan: case site::field_slot_hash: return std::to_string(f) + " slots";
		case site::move_hit: {
			static const char* const hits[] = { "?", "return: own slot moves (NRVO)", "return: temporary handed out", "parameter: read-only alias", "parameter: prvalue elided",
				"local: read-only alias", "take_for_assignment: moved", "element store: instance moved", "store: prvalue moved",
				"graft parameter: read-only alias", "graft parameter: prvalue elided" };
			return f < 11 ? hits[f] : "?";
		}
		default: return f ? std::to_string(f) : "-";
		}
	}
	std::string op_text(uint16_t op) {
		if (op == k_no_op) return "(host)";
		return std::string(vm::opcode_name(static_cast<vm::opcode>(op)));
	}
	std::string tidy(std::string n) {
		for (const char* strip : { "`anonymous namespace'::", "jai::vm::", "jai::detail::", "jai::census::", "jai::", "class ", "struct " }) {
			for (size_t p; (p = n.find(strip)) != std::string::npos;) n.erase(p, std::strlen(strip));
		}
		if (n.size() > 110) n = n.substr(0, 107) + "...";
		return n;
	}

#if defined(_MSC_VER)
	struct symbolizer {
		HANDLE proc = GetCurrentProcess();
		bool ok = false;
		std::unordered_map<const void*, std::string> cache;
		symbolizer() {
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
			ok = SymInitialize(proc, nullptr, TRUE) != FALSE;
		}
		~symbolizer() { if (ok) SymCleanup(proc); }
		static std::string base(const char* path) {
			std::string f = path ? path : "";
			const size_t slash = f.find_last_of("/\\");
			return slash == std::string::npos ? f : f.substr(slash + 1);
		}
		// a stack token: its frames, innermost first, joined by " < " (the counting wrapper's own frame left out)
		const std::string& operator()(const void* ra) {
			auto it = cache.find(ra);
			if (it != cache.end()) return it->second;
			std::string out;
			auto st = reg().stacks.find(ra);
			if (st != reg().stacks.end()) {
				for (const void* f : st->second->f) {
					if (!f) break;
					const std::string& text = frame(f);
					if (text.rfind("script_value::clone ", 0) == 0 || text.rfind("script_value::clone(", 0) == 0 || text == "script_value::clone") continue;
					out += out.empty() ? text : " < " + text;
				}
				return cache.emplace(ra, std::move(out)).first->second;
			}
			out = frame(ra);
			return cache.emplace(ra, std::move(out)).first->second;
		}
		std::unordered_map<const void*, std::string> frames_cache;
		// the call's chain, outermost function first: "fn [file:line] > inlined [file:line] > ..."
		const std::string& frame(const void* ra) {
			auto it = frames_cache.find(ra);
			if (it != frames_cache.end()) return it->second;
			std::string out;
			if (!ok || !ra) { out = "?"; }
			else {
				const DWORD64 a = reinterpret_cast<DWORD64>(ra) - 1;   // a return address: step back into the call
				alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 1024];
				SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
				std::vector<std::string> frames;   // innermost first
				{
					sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 1000;
					DWORD64 first = 0;
					SymFromAddr(proc, a, &first, sym);   // loads the module's symbols (deferred loads) before the inline queries
				}
				const DWORD inlines = SymAddrIncludeInlineTrace(proc, a);
				DWORD ctx = 0, idx = 0;
				if (inlines && SymQueryInlineTrace(proc, a, 0, a, a, &ctx, &idx)) {
					for (DWORD i = 0; i < inlines; ++i) {
						std::memset(buf, 0, sizeof buf);
						sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 1000;
						DWORD64 disp = 0;
						std::string fr = SymFromInlineContext(proc, a, ctx + i, &disp, sym) ? tidy(sym->Name) : "?";
						IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line); DWORD ld = 0;
						if (SymGetLineFromInlineContext(proc, a, ctx + i, 0, &ld, &line)) fr += " [" + base(line.FileName) + ":" + std::to_string(line.LineNumber) + "]";
						frames.push_back(fr);
					}
				}
				std::memset(buf, 0, sizeof buf);
				sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 1000;
				DWORD64 disp = 0;
				std::string outer = SymFromAddr(proc, a, &disp, sym) ? tidy(sym->Name) : "?";
				if (inlines == 0) {
					IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line); DWORD ld = 0;
					if (SymGetLineFromAddr64(proc, a, &ld, &line)) outer += " [" + base(line.FileName) + ":" + std::to_string(line.LineNumber) + "]";
				}
				out = outer;
				for (auto f = frames.rbegin(); f != frames.rend(); ++f) out += " > " + *f;
			}
			return frames_cache.emplace(ra, std::move(out)).first->second;
		}
	};
#else
	struct symbolizer {
		std::unordered_map<const void*, std::string> cache;
		const std::string& operator()(const void* ra) {
			auto it = cache.find(ra);
			if (it != cache.end()) return it->second;
			char b[32]; std::snprintf(b, sizeof b, "%p", ra);
			return cache.emplace(ra, b).first->second;
		}
	};
#endif

	std::string pct(uint64_t a, uint64_t b) {
		char buf[32];
		std::snprintf(buf, sizeof buf, "%5.1f%%", b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0);
		return buf;
	}

	void write_census(const char* path) {
		std::vector<thread_tables*> all;
		{ std::lock_guard<std::mutex> lock(reg().m); all = reg().all; }
		std::unordered_map<ra_key, stats, ra_hash> by_ra;
		std::map<std::tuple<uint16_t, uint32_t, std::string>, line_stats> by_line;
		std::map<std::string, uint64_t> by_name;
		uint64_t instr = 0;
		for (thread_tables* t : all) {
			instr += t->instr;
			for (const auto& [k, v] : t->by_ra) {
				stats& d = by_ra[k];
				d.n += v.n; d.weight += v.weight; d.unique += v.unique; d.src_watched += v.src_watched; d.src_same += v.src_same; d.src_soon += v.src_soon;
				d.dst_watched += v.dst_watched; d.dst_same += v.dst_same; d.dst_soon += v.dst_soon;
			}
			for (const auto& [k, v] : t->by_line) {
				line_stats& d = by_line[{k.s, k.flags, v.where}];
				d.n += v.n; d.weight += v.weight;
			}
			for (const auto& [k, v] : t->by_name) by_name[k] += v;
		}
		symbolizer sym;
		std::ofstream tsv(std::string(path) + ".tsv", std::ios::binary | std::ios::trunc);
		tsv << "site\tgroup\tflags\top\tcaller\tn\tweight\tunique\tsrc_watched\tsrc_same\tsrc_soon\tdst_watched\tdst_same\tdst_soon\n";
		for (const auto& [k, v] : by_ra) {
			tsv << k_site_ids[k.s] << "\t" << k_site_groups[k.s] << "\t" << flags_text(static_cast<site>(k.s), k.flags) << "\t" << op_text(k.op) << "\t" << sym(k.ra) << "\t"
			    << v.n << "\t" << v.weight << "\t" << v.unique << "\t" << v.src_watched << "\t" << v.src_same << "\t" << v.src_soon << "\t"
			    << v.dst_watched << "\t" << v.dst_same << "\t" << v.dst_soon << "\n";
		}
		std::ofstream lines(std::string(path) + ".lines.tsv", std::ios::binary | std::ios::trunc);
		lines << "site\tflags\twhere\tn\tweight\n";
		for (const auto& [k, v] : by_line) {
			lines << k_site_ids[std::get<0>(k)] << "\t" << flags_text(static_cast<site>(std::get<0>(k)), std::get<1>(k)) << "\t" << std::get<2>(k) << "\t" << v.n << "\t" << v.weight << "\n";
		}
		std::ofstream names(std::string(path) + ".names.tsv", std::ios::binary | std::ios::trunc);
		names << "site\tflags\tname\tn\n";
		for (const auto& [k, v] : by_name) {
			const size_t a = k.find('\t'), b = k.find('\t', a + 1);
			const unsigned s = static_cast<unsigned>(std::stoul(k.substr(0, a)));
			const uint32_t f = static_cast<uint32_t>(std::stoul(k.substr(a + 1, b - a - 1)));
			names << k_site_ids[s] << "\t" << flags_text(static_cast<site>(s), f) << "\t" << k.substr(b + 1) << "\t" << v << "\n";
		}

		std::ofstream o(path, std::ios::binary | std::ios::trunc);
		o << "copy and front-loading census: " << all.size() << " threads, " << instr << " vm instructions counted\n";
		uint64_t site_n[k_sites] = {}, site_w[k_sites] = {};
		for (const auto& [k, v] : by_ra) { site_n[k.s] += v.n; site_w[k.s] += v.weight; }
		o << "\n== sites (events | weight | per 1k instructions)\n";
		for (size_t s = 0; s < k_sites; ++s) {
			if (!site_n[s]) continue;
			char b[64]; std::snprintf(b, sizeof b, "%.3f", instr ? 1000.0 * static_cast<double>(site_n[s]) / static_cast<double>(instr) : 0.0);
			o << k_site_ids[s] << " | " << k_site_groups[s] << " | " << site_n[s] << " | " << site_w[s] << " | " << b << " | " << k_site_texts[s] << "\n";
		}
		for (size_t s = 0; s < k_sites; ++s) {
			if (!site_n[s]) continue;
			o << "\n== " << k_site_ids[s] << " (" << k_site_texts[s] << "): " << site_n[s] << " events, weight " << site_w[s] << "\n";
			std::map<uint32_t, std::pair<uint64_t, uint64_t>> per_flags;
			std::map<uint16_t, uint64_t> per_op;
			std::unordered_map<const void*, stats> per_ra;
			std::unordered_map<const void*, std::map<uint16_t, uint64_t>> ra_ops;
			for (const auto& [k, v] : by_ra) {
				if (k.s != s) continue;
				per_flags[k.flags].first += v.n; per_flags[k.flags].second += v.weight;
				per_op[k.op] += v.n;
				stats& d = per_ra[k.ra];
				d.n += v.n; d.weight += v.weight; d.unique += v.unique; d.src_watched += v.src_watched; d.src_same += v.src_same; d.src_soon += v.src_soon;
				d.dst_watched += v.dst_watched; d.dst_same += v.dst_same; d.dst_soon += v.dst_soon;
				ra_ops[k.ra][k.op] += v.n;
			}
			o << "-- by flags (events | weight | flags)\n";
			for (const auto& [f, nw] : per_flags) o << "  " << nw.first << " | " << nw.second << " | " << flags_text(static_cast<site>(s), f) << "\n";
			o << "-- by opcode (events | share | op)\n";
			{
				std::vector<std::pair<uint64_t, uint16_t>> v;
				for (const auto& [op, n] : per_op) v.emplace_back(n, op);
				std::sort(v.rbegin(), v.rend());
				for (size_t i = 0; i < v.size() && i < 15; ++i) o << "  " << v[i].first << " | " << pct(v[i].first, site_n[s]) << " | " << op_text(v[i].second) << "\n";
			}
			o << "-- by caller (events | share | weight | src unique | src dies same instr | src dies <=8 | copy dies same instr | copy dies <=8 | top ops | chain)\n";
			{
				std::vector<std::pair<uint64_t, const void*>> v;
				for (const auto& [ra, d] : per_ra) v.emplace_back(d.n, ra);
				std::sort(v.rbegin(), v.rend());
				for (size_t i = 0; i < v.size() && i < 40; ++i) {
					const stats& d = per_ra[v[i].second];
					std::vector<std::pair<uint64_t, uint16_t>> ops;
					for (const auto& [op, n] : ra_ops[v[i].second]) ops.emplace_back(n, op);
					std::sort(ops.rbegin(), ops.rend());
					std::string top;
					for (size_t j = 0; j < ops.size() && j < 3; ++j) top += (j ? ", " : "") + op_text(ops[j].second) + " " + pct(ops[j].first, d.n);
					o << "  " << d.n << " | " << pct(d.n, site_n[s]) << " | " << d.weight << " | " << pct(d.unique, d.n) << " | " << pct(d.src_same, d.src_watched) << " | "
					  << pct(d.src_soon, d.src_watched) << " | " << pct(d.dst_same, d.dst_watched) << " | " << pct(d.dst_soon, d.dst_watched) << " | " << top << " | " << sym(v[i].second) << "\n";
				}
			}
			o << "-- by script line (events | weight | flags | where)\n";
			{
				std::vector<std::tuple<uint64_t, uint64_t, uint32_t, std::string>> v;
				for (const auto& [k, d] : by_line) { if (std::get<0>(k) == s) v.emplace_back(d.n, d.weight, std::get<1>(k), std::get<2>(k)); }
				std::sort(v.rbegin(), v.rend());
				for (size_t i = 0; i < v.size() && i < 25; ++i) o << "  " << std::get<0>(v[i]) << " | " << std::get<1>(v[i]) << " | " << flags_text(static_cast<site>(s), std::get<2>(v[i])) << " | " << std::get<3>(v[i]) << "\n";
			}
			std::vector<std::pair<uint64_t, std::string>> named;
			const std::string prefix = std::to_string(s) + "\t";
			for (const auto& [k, n] : by_name) {
				if (k.compare(0, prefix.size(), prefix) != 0) continue;
				const size_t a = k.find('\t'), b = k.find('\t', a + 1);
				named.emplace_back(n, k.substr(b + 1) + "  (" + flags_text(static_cast<site>(s), static_cast<uint32_t>(std::stoul(k.substr(a + 1, b - a - 1)))) + ")");
			}
			if (!named.empty()) {
				std::sort(named.rbegin(), named.rend());
				o << "-- by name (events | name (reason))\n";
				for (size_t i = 0; i < named.size() && i < 25; ++i) o << "  " << named[i].first << " | " << named[i].second << "\n";
			}
		}
	}

	struct census_writer {
		~census_writer() {
			const char* path = std::getenv("JAI_COPY_CENSUS");
			if (path && *path) write_census(path);
		}
	};
	census_writer the_census_writer;
}

} // namespace jai::census

#endif // JAISCRIPT_VM_COVERAGE
