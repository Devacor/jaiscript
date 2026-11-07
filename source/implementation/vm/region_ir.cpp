#include <jaiscript/vm/region_ir.hpp>
#include <algorithm>

namespace jai::vm {

region_cfg control_flow(const region_ir& r) {
	const std::vector<ir_op>& ops = r.ops;
	region_cfg g;
	const size_t n_ops = ops.size();
	g.back_node = n_ops; g.exit_node = n_ops + 1;
	auto node_of = [&](uint32_t target) -> size_t { return target > r.back || target < r.first ? g.exit_node : static_cast<size_t>(target - r.first); };
	g.succ.assign(n_ops + 1, std::array<size_t, 2>{ g.exit_node, g.exit_node });
	for (size_t k = 0; k < n_ops; ++k) {
		const ir_op& op = ops[k];
		switch (op.kind) {
		case ir::exit: case ir::bail: case ir::ret: break;   // no successor: the block ends (a bail: the general form takes the iteration from here; a ret: the frame returns)
		case ir::jump: case ir::boundary: g.succ[k][0] = node_of(op.target); break;   // a boundary continues at its statement's end
		case ir::range:
		case ir::cmp: g.succ[k][0] = k + 1; g.succ[k][1] = op.head ? g.exit_node : node_of(op.target); break;
		case ir::land: g.succ[k][0] = k + 1; if (op.branch) { g.succ[k][1] = node_of(op.target); } break;   // a branching landing (a call's result as a truth jump)
		default: g.succ[k][0] = k + 1; break;
		}
	}
	g.succ[g.back_node][0] = r.straight ? g.exit_node : 0;
	g.preds.assign(n_ops + 1, {});
	for (size_t k = 0; k <= n_ops; ++k) { for (size_t s : g.succ[k]) { if (s <= n_ops && s != 0) { g.preds[s].push_back(k); } } }
	return g;
}

// the must-define dataflow over the graph is the rule (a definition in an untaken branch does not reach a read after the
// join), and it is the rule every pass over the region must keep
region_verdict verify(const region_ir& r, const std::vector<chunk::cell_loop::cell>& cells) {
	const std::vector<ir_op>& ops = r.ops;
	const size_t n_ops = ops.size();
	const region_cfg g = control_flow(r);
	std::vector<uint32_t> tracked;   // the slots undefined at entry
	for (const auto& cl : cells) { if (cl.env_operand == k_invalid_u32 && (cl.temp || cl.first_decl) && std::find(tracked.begin(), tracked.end(), cl.slot) == tracked.end()) { tracked.push_back(cl.slot); } }
	auto tracked_at = [&](uint32_t slot) -> int { for (size_t t = 0; t < tracked.size(); ++t) { if (tracked[t] == slot) return static_cast<int>(t); } return -1; };
	std::vector<std::vector<bool>> in(n_ops + 1, std::vector<bool>(tracked.size(), true));   // the meet's identity; the entry defines nothing tracked
	std::fill(in[0].begin(), in[0].end(), false);
	for (bool changed = true; changed;) {
		changed = false;
		for (size_t k = 1; k <= n_ops; ++k) {
			if (g.preds[k].empty()) continue;
			for (size_t t = 0; t < tracked.size(); ++t) {
				bool all = true;
				for (size_t p : g.preds[k]) { if (!(in[p][t] || (p < n_ops && ops[p].writes == tracked[t]))) { all = false; break; } }
				if (in[k][t] != all) { in[k][t] = all; changed = true; }
			}
		}
	}
	std::vector<std::pair<uint32_t, uint32_t>> snapshots;   // slot, the read's ip (the guard is open until it)
	for (size_t k = 0; k < n_ops; ++k) {
		const uint32_t ip = r.first + static_cast<uint32_t>(k);
		const ir_op& op = ops[k];
		for (uint8_t x = 0; x < op.nreads; ++x) {
			const int t = tracked_at(op.reads[x]);
			if (t >= 0 && !in[k][static_cast<size_t>(t)]) { return region_verdict{ chunk::cell_refusal_malformed, ip }; }
		}
		if (op.writes != k_invalid_u32) {
			for (const auto& s : snapshots) { if (s.first == op.writes && ip < s.second) { return region_verdict{ chunk::cell_refusal_malformed, ip }; } }
		}
		if (op.kind == ir::guard) {
			for (const auto& s : snapshots) { if (s.first == op.a && ip < s.second) { return region_verdict{ chunk::cell_refusal_malformed, ip }; } }
			if (op.guarded == 0xFFFFFFu) { return region_verdict{ chunk::cell_refusal_malformed, ip }; }
			snapshots.emplace_back(op.a, r.head + op.guarded);
		}
	}
	for (const auto& op : ops) {
		if (op.container == k_invalid_u32) continue;
		// an alias slot: its sites follow its declaration and the must-define dataflow above holds every read to a defined alias; another
		// definition of the slot (a temp reused in another arm) is another cell, which the alias's sites never read
		if (std::any_of(ops.begin(), ops.end(), [&](const ir_op& w) { return w.kind == ir::rebind && w.writes == op.container; })) continue;
		for (const auto& w : ops) {
			if (w.writes == op.container) return region_verdict{ chunk::cell_refusal_effect, r.head };   // a container binding rewritten inside the body
		}
	}
	return region_verdict{};
}

bool bail_dominates_back_edge(const region_ir& r, uint32_t& at) {
	if (r.straight) return false;
	const region_cfg g = control_flow(r);
	std::vector<bool> reached(g.succ.size(), false);
	std::vector<size_t> pending{ 0 };
	reached[0] = true;
	size_t ahead = 0;
	while (!pending.empty()) {
		const size_t k = pending.back(); pending.pop_back();
		if (k == g.back_node) continue;
		if (k < r.ops.size() && ir_work(r.ops[k])) { ++ahead; }
		for (const size_t s : g.succ[k]) { if (s < reached.size() && !reached[s]) { reached[s] = true; pending.push_back(s); } }
	}
	if (reached[g.back_node] || ahead >= 2) return false;
	for (size_t k = 0; k < r.ops.size(); ++k) {
		if (reached[k] && r.ops[k].kind == ir::bail) { at = r.first + static_cast<uint32_t>(k); return true; }
	}
	return false;
}

std::string listing(const region_ir& r) {
	std::string text;
	for (size_t k = 0; k < r.ops.size(); ++k) {
		const ir_op& op = r.ops[k];
		text += std::to_string(k) + ":" + ir_name(op.kind);
		if (op.kind == ir::cmp || op.kind == ir::jump || op.kind == ir::exit || op.kind == ir::bail || op.kind == ir::range || op.kind == ir::ret || op.kind == ir::boundary) {
			text += op.kind == ir::ret ? "->return" : op.kind == ir::exit || op.kind == ir::bail || op.head || op.target > r.back || op.target < r.first ? "->exit" : op.target == r.back ? "->back" : "->" + std::to_string(op.target - r.first);
		}
		if (op.writes != k_invalid_u32) { text += " w" + std::to_string(op.writes); }
		for (uint8_t x = 0; x < op.nreads; ++x) { text += (x ? "," : " r") + std::to_string(op.reads[x]); }
		text += ";";
	}
	return text;
}

std::vector<chunk::cell_mask> live_temps(const region_ir& r, const std::vector<chunk::cell_loop::cell>& cells,
                                         const std::vector<uint32_t>& slot_to_cell, const std::vector<std::pair<uint32_t, uint32_t>>& defs,
                                         const std::vector<uint32_t>& copy_at, uint32_t copy_head, size_t copy_count, uint32_t held_below,
                                         const std::vector<uint32_t>* stmt_starts, uint32_t slot_limit) {
	std::vector<chunk::cell_mask> live(copy_count, chunk::cell_mask{});
	std::vector<uint32_t> cur = slot_to_cell;
	std::vector<size_t> def_end(cells.size(), 0);   // per compiler value: the copy offset after its defining instruction(s)
	size_t nd = 0;
	std::vector<uint32_t> stmt_cells;   // what the current caller's statement defined so far, the latest per slot
	uint32_t stmt_at = k_invalid_u32;
	for (size_t k = 0; k < r.ops.size(); ++k) {
		const ir_op& op = r.ops[k];
		const size_t ip = r.first + k;
		const size_t here_end = copy_at[ip + 1 - r.head] - copy_head;   // the copy offset after this operation's instructions
		if (stmt_starts && k < stmt_starts->size()) {
			if ((*stmt_starts)[k] != stmt_at) { stmt_cells.clear(); stmt_at = (*stmt_starts)[k]; }
			const size_t here_begin = copy_at[ip - r.head] - copy_head;
			for (const uint32_t cl : stmt_cells) { for (size_t m = here_begin; m < here_end && m < live.size(); ++m) { live[m].set(cl); } }
		}
		for (uint8_t x = 0; x < op.nreads; ++x) {
			const uint32_t cl = op.reads[x] < cur.size() ? cur[op.reads[x]] : k_invalid_u32;
			if (cl == k_invalid_u32 || !cells[cl].temp) continue;
			for (size_t m = def_end[cl]; m < here_end && m < live.size(); ++m) { live[m].set(cl); }
		}
		if (held_below != 0) {
			const size_t here_begin = copy_at[ip - r.head] - copy_head;
			for (size_t s = 0; s < cur.size() && s < held_below; ++s) {
				const uint32_t cl = cur[s];
				if (cl == k_invalid_u32 || cl >= cells.size() || !cells[cl].temp || cells[cl].env_operand != k_invalid_u32) continue;
				for (size_t m = here_begin; m < here_end && m < live.size(); ++m) { live[m].set(cl); }
			}
		}
		while (nd < defs.size() && defs[nd].first == ip) {
			const uint32_t dc = defs[nd].second;
			cur[cells[dc].slot] = dc; def_end[dc] = here_end; ++nd;
			if (stmt_starts && cells[dc].temp && cells[dc].env_operand == k_invalid_u32 && cells[dc].slot < slot_limit) {
				std::erase_if(stmt_cells, [&](uint32_t h) { return cells[h].slot == cells[dc].slot; });
				stmt_cells.push_back(dc);
			}
		}
	}
	// an inner loop's back edge (a backward jump, or an inner counted loop's compare back to its body) runs its body again: a compiler value
	// live where the body begins (defined ahead of the loop and read inside it, a graft's parameter over the graft's own loop) stays live to
	// the back edge, since the general form reads it again on the next pass
	for (bool changed = true; changed;) {
		changed = false;
		for (size_t k = 0; k < r.ops.size(); ++k) {
			const ir_op& op = r.ops[k];
			if ((op.kind != ir::jump && op.kind != ir::cmp) || op.head || op.target < r.first || op.target >= r.first + k) continue;
			const size_t from = copy_at[op.target - r.head] - copy_head;
			const size_t to = copy_at[r.first + k + 1 - r.head] - copy_head;
			if (from >= live.size()) continue;
			const chunk::cell_mask entering = live[from];
			for (size_t m = from; m < to && m < live.size(); ++m) {
				if ((entering & ~live[m]).any()) { live[m] |= entering; changed = true; }
			}
		}
	}
	return live;
}

} // namespace jai::vm
