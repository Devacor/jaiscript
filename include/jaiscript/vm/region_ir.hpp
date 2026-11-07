#pragma once

#ifndef __JAISCRIPT_VM_REGION_IR_HPP__
#define __JAISCRIPT_VM_REGION_IR_HPP__

// The region IR: a closed loop's
// canonical operations, one per instruction of the general form, over cells. vm_compiler::lower_typed_ops
// translates a loop into a region_ir and emits the cell copy from it; this module holds what every pass
// consults: the operation set, the region's control flow graph, the verifier (must-define over the graph,
// the snapshot spans, the container bindings never written), the listing and the liveness of compiler
// values per copy instruction. Cells are the registers and the fallback contract (a miss flushes dirty
// cells by liveness and re-runs the general form from the origin), so the IR keeps cells rather than
// renaming into SSA.

#include "chunk.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jai::vm {

	// probe: the callee resolved at its own place, ahead of the argument values; push: recv.push(v) onto a frame-slot array
	enum class ir : uint8_t { arith, mov, cmp, load, store, math, jump, guard, placeholder, neg, bnot, call, land, skip, lnot, cmpval, exit, append, probe, push, bail, rebind, range, size, ret, boundary, depth, dispatch };   // dispatch: a switch's table over a (a switch_tables index), b the subject's slot (its case tests follow, the successor); ret: a frame program's return (no successor); range: a range-for's head (the index against the container's live size, the exit on exhaustion); rebind: an element alias's declaration (its source table's element becomes the container of the alias's sites); bail: a planned miss at its own origin inside a conditional span; boundary: a statement the general form runs while the block stays parked (its successor the statement's end, target)

	inline const char* ir_name(ir k) {
		static const char* const names[] = { "arith", "mov", "cmp", "load", "store", "math", "jump", "guard", "placeholder", "neg", "bnot", "call", "land", "skip", "lnot", "cmpval", "exit", "append", "probe", "push", "bail", "rebind", "range", "size", "ret", "boundary", "depth", "dispatch" };
		return names[static_cast<size_t>(k)];
	}

	constexpr uint32_t k_ir_env_bit = 0x40000000u;   // an operand that is an environment value: the bit | its position among the region's environment operands

	struct ir_op {
		ir kind = ir::placeholder;
		uint32_t a = 0, b = 0, c = 0;            // operands: a slot, k_typed_const_bit | constant, or k_ir_env_bit | env value position
		opcode fast = opcode::op_halt;           // the pinned-kind form (op_t_*) when the typed lowering produced it, else the any-kind form
		uint8_t tok = 0;                         // the token for arith and cmp, the math_fn for math; a jump's 1 marks an inner loop's back edge (the budget poll)
		uint32_t target = 0;                     // cmp and jump: the original target ip (the exit for a head)
		uint32_t proto = k_invalid_u32;          // load: a general read's fused_index_protos index (k_invalid_u32 = a T_LOAD's site); store: the proto
		uint32_t guarded = 0xFFFFFFu;            // guard: the guarded read's offset from the head
		uint32_t reads[8] = {}; uint8_t nreads = 0;   // the slots the operation reads (the verifier's use list)
		uint32_t writes = k_invalid_u32;         // the slot the operation writes (a local or a compiler value)
		uint32_t container = k_invalid_u32;      // load and store: the container slot when the binding is a frame slot
		bool head = false;
		bool sense = false;                      // cmp: jump when true (a comparison value followed by JUMP_IF_TRUE)
		uint32_t origin = k_invalid_u32;         // the original ip a miss at this operation re-runs (k_invalid_u32 = its own): an argument's value re-runs from the probe
		bool poll = false;                       // arith: an inner counted loop's step, preceded in the copy by the budget poll (C_JUMP c = 1: a miss on exhaustion onto the step's own origin)
		uint32_t member = k_invalid_u32;         // load: the member_expr node of a member read site (container = the base slot; the field resolved at entry)
		bool branch = false;                     // land: the call's result as a truth jump to target (sense as cmp's), b the landed slot
		uint32_t ref_decl = k_invalid_u32;       // rebind: a typed class reference declaration whose admission must run at binding
	};

	// one loop's operations with the span of the general form they stand for: ops[k] stands for the instruction
	// at first + k (a counted loop's head is the back edge's placeholder ahead of first); the index is the
	// operation's id, back the back edge's ip
	struct region_ir {
		std::vector<ir_op> ops;
		uint32_t head = 0, first = 0, back = 0;
		bool straight = false;   // a straight-line region: the end leaves (no back edge re-enters the first operation)
	};

	// the region's control flow graph: an operation's successors are the next operation, a compare's target (the
	// exit when the target is past the back edge; a head compare's false edge is the exit), a jump's target and
	// nothing after an exit; the last operation continues to the back node, which re-enters at the first
	struct region_cfg {
		std::vector<std::array<size_t, 2>> succ;   // per operation and the back node (ops.size()); an entry of exit_node is no edge
		std::vector<std::vector<size_t>> preds;    // per operation and the back node; the entry (0) records none
		size_t back_node = 0, exit_node = 0;
	};
	region_cfg control_flow(const region_ir& r);

	struct region_verdict { uint8_t refusal = chunk::cell_refusal_none; uint32_t at = 0; };
	// the verifier: a slot the block enters with undefined (a compiler value, a local the body declares) is defined
	// on every path to each of its reads; a snapshot's slot is written only by its own guard between the guard and
	// its read; a container binding's slot is never written. The first offence in operation order is the verdict
	// (malformed at its ip; a rewritten container binding is an effect at the head)
	region_verdict verify(const region_ir& r, const std::vector<chunk::cell_loop::cell>& cells);

	// the listing tool's text: index:kind, ->target, w slot, r slots, one operation per ';'
	std::string listing(const region_ir& r);

	// an operation that does work of its own (what a park and a resume, or a block entry and a flush, are weighed against)
	inline bool ir_work(const ir_op& o) {
		switch (o.kind) {
		case ir::arith: case ir::load: case ir::store: case ir::math: case ir::neg: case ir::bnot: case ir::lnot: case ir::cmpval: case ir::append: case ir::push: case ir::size: case ir::call:
			return true;
		case ir::cmp: return !o.head;
		default: return false;
		}
	}

	// a loop whose every path from the head to the back edge passes a bail, with less work ahead of the bails than a park and a
	// resume (two operations): each iteration would pay the entry and the flush and run on the general form; at = the first bail's ip
	bool bail_dominates_back_edge(const region_ir& r, uint32_t& at);

	// liveness of compiler values per copy instruction: a definition is live from the instruction after it through
	// its last read, and through an inner loop's back edge when it is live where that loop's body begins (a miss there
	// re-runs the read on the general form, which needs the slot to hold this value); a
	// dead definition is never flushed, so it cannot overwrite the live one of the same slot. copy_at maps an original
	// ip's offset from the head to its first copy instruction, defs lists (ip, cell) in program order, copy_count is
	// the copy's length. held_below (a block inside a graft's body): each operation also holds the current definition of
	// every compiler slot below it, since the graft's code after a miss (the rest of the caller's statement) reads them.
	// stmt_starts (per operation, the ip its caller's statement begins at): each operation also holds what its statement
	// defined before it, since a miss re-runs the statement's rest on the general form, a graft's bailed body included
	std::vector<chunk::cell_mask> live_temps(const region_ir& r, const std::vector<chunk::cell_loop::cell>& cells,
	                                         const std::vector<uint32_t>& slot_to_cell, const std::vector<std::pair<uint32_t, uint32_t>>& defs,
	                                         const std::vector<uint32_t>& copy_at, uint32_t copy_head, size_t copy_count, uint32_t held_below = 0,
	                                         const std::vector<uint32_t>* stmt_starts = nullptr, uint32_t slot_limit = 0);

} // namespace jai::vm

#endif // __JAISCRIPT_VM_REGION_IR_HPP__
