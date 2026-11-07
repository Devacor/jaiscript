#pragma once

// The block census of generated programs: per segment (a band, the main program, the reload), what the vm compiled into cell
// blocks and what it ran there. Every build reads the compiled loops and the refused ones from the listing sink and the block
// entries from engine::cells_counters; a census build (JAISCRIPT_VM_COVERAGE) adds the executed operations by class and the
// general form's reasons. A band whose loops never close is blind to every bug that fires only inside a block.

#include "fuzz_generator.hpp"
#include "fuzz_harness.hpp"
#include "fuzz_order_band.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace jai::fuzz::census {

	struct segment_facts {
		std::string label;
		uint64_t loops = 0, regions = 0;              // closed loops and straight-line regions the compiler built
		std::vector<std::string> open;                // per refused loop: its class and the first offending instruction
		uint64_t entries = 0, misses = 0;             // block entries (loops and regions) and misses while the segment ran
		bool census = false;
		vm_coverage_totals totals;                    // a census build: the segment's executed operations
		bool closed_block() const { return census ? totals.closed_entered > 0 : loops > 0 && entries > 0; }
	};

	inline std::string segment_label(const program& p, size_t i) { return i < p.labels.size() ? p.labels[i] : "segment " + std::to_string(i); }

	// "-- cell loop", "-- cell region" and "-- open loop H..B: <class> at <ip> (<OP>)" lines of a chunk's listing (disassembler.cpp)
	inline void read_listing(const std::string& listing, segment_facts& f) {
		std::istringstream in(listing);
		std::string line;
		while (std::getline(in, line)) {
			if (line.rfind("-- cell loop ", 0) == 0) { ++f.loops; continue; }
			if (line.rfind("-- cell region ", 0) == 0) { ++f.regions; continue; }
			if (line.rfind("-- open loop ", 0) != 0) continue;
			const size_t colon = line.find(": "), at = line.rfind(" at "), paren = line.rfind(" (");
			if (colon == std::string::npos || at == std::string::npos || paren == std::string::npos || at < colon) { f.open.push_back("?"); continue; }
			f.open.push_back(line.substr(colon + 2, at - colon - 2) + " " + line.substr(paren + 1));
		}
	}

	// runs the program on the vm alone, the facts per segment
	inline std::vector<segment_facts> measure(const program& p, double budget_seconds, bool proofs = true, uint32_t miss_every = 0, bool* budget_hit = nullptr) {
		std::vector<segment_facts> out;
		segment_facts current;
		cell_counters before{};
		const bool census = engine::vm_coverage_available();
		run_config cfg;
		cfg.use_vm = true;
		cfg.budget_seconds = budget_seconds;
		cfg.proofs = proofs;
		cfg.miss_every = miss_every;
		cfg.on_engine = [&](engine& e) {
			e.disassembly_sink([&current](const std::string& listing, const std::string&) { read_listing(listing, current); });
			if (census) e.vm_coverage_reset();
		};
		cfg.after_segment = [&](size_t i, engine& e) {
			const cell_counters now = detail::engine_access::cells_counters(e);
			current.label = segment_label(p, i);
			current.entries = now.entries - before.entries;
			current.misses = now.misses - before.misses;
			before = now;
			if (census) { current.census = true; current.totals = e.vm_coverage_summary(); e.vm_coverage_reset(); }
			out.push_back(std::move(current));
			current = segment_facts{};
		};
		const run_outcome r = run_program(p, cfg);
		if (budget_hit) *budget_hit = r.budget_hit;
		return out;
	}

	struct band_stats {
		uint64_t segments = 0, with_loop = 0, with_entry = 0, with_closed = 0, budget = 0;
		uint64_t ops = 0, cells = 0, cells_closed = 0;
		double share_sum = 0;                         // the mean over segments of each one's share of operations in cells
		uint64_t share_n = 0;
		std::map<std::string, uint64_t> open_loops, open_segments;   // a refusal: loops it refused, segments it refused one in
		std::map<std::string, uint64_t> reason_ops;                  // a census build: the general form's operations by reason

		void add(const segment_facts& f, bool budget_hit) {
			++segments;
			budget += budget_hit ? 1 : 0;
			with_loop += f.loops ? 1 : 0;
			with_entry += f.entries ? 1 : 0;
			with_closed += f.closed_block() ? 1 : 0;
			std::map<std::string, bool> seen;
			for (const auto& o : f.open) { ++open_loops[o]; if (!seen[o]) { seen[o] = true; ++open_segments[o]; } }
			if (!f.census) return;
			ops += f.totals.operations;
			cells += f.totals.cells;
			cells_closed += f.totals.cells_closed;
			if (f.totals.operations) { share_sum += static_cast<double>(f.totals.cells) / static_cast<double>(f.totals.operations); ++share_n; }
			for (const auto& [k, n] : f.totals.reasons) reason_ops[k] += n;
		}
		double pct(uint64_t part) const { return segments ? 100.0 * static_cast<double>(part) / static_cast<double>(segments) : 0.0; }
	};

	inline std::string percent(double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f%%", v); return b; }

	inline std::string ranked(const std::map<std::string, uint64_t>& m, size_t top, uint64_t whole, const char* unit) {
		std::vector<std::pair<uint64_t, std::string>> v;
		for (const auto& [k, n] : m) v.emplace_back(n, k);
		std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
		std::string s;
		for (size_t i = 0; i < v.size() && i < top; ++i) {
			s += "      " + std::to_string(v[i].first) + " " + unit;
			if (whole) s += " (" + percent(100.0 * static_cast<double>(v[i].first) / static_cast<double>(whole)) + ")";
			s += "  " + v[i].second + "\n";
		}
		return s;
	}

	inline std::string report(const std::vector<std::pair<std::string, band_stats>>& bands, size_t top = 6) {
		std::ostringstream o;
		const bool census = engine::vm_coverage_available();
		o << "band        segments  >=1 loop compiled  >=1 block entry  >=1 closed block" << (census ? "  ops in cells (all)  closed-loop cells  mean share" : "") << "\n";
		for (const auto& [name, b] : bands) {
			char line[256];
			std::snprintf(line, sizeof line, "%-11s %8llu  %17s  %15s  %16s", name.c_str(), static_cast<unsigned long long>(b.segments), percent(b.pct(b.with_loop)).c_str(),
			              percent(b.pct(b.with_entry)).c_str(), percent(b.pct(b.with_closed)).c_str());
			o << line;
			if (census) {
				std::snprintf(line, sizeof line, "  %18s  %17s  %10s", percent(b.ops ? 100.0 * static_cast<double>(b.cells) / static_cast<double>(b.ops) : 0.0).c_str(),
				              percent(b.ops ? 100.0 * static_cast<double>(b.cells_closed) / static_cast<double>(b.ops) : 0.0).c_str(),
				              percent(b.share_n ? 100.0 * b.share_sum / static_cast<double>(b.share_n) : 0.0).c_str());
				o << line;
			}
			if (b.budget) o << "  (" << b.budget << " budget-hit)";
			o << "\n";
		}
		for (const auto& [name, b] : bands) {
			o << "\n[" << name << "] refused loops by class and first offending instruction (segments with one):\n" << ranked(b.open_segments, top, b.segments, "segments");
			if (census) {
				o << "[" << name << "] general-form operations by reason:\n" << ranked(b.reason_ops, top, b.ops, "ops");
			}
		}
		return o.str();
	}

	// the share of a part's segments that entered a closed block (the smoke floor's measure); segments counts the part's segments seen
	inline double closed_block_share(const std::string& label, uint64_t first_seed, uint64_t count, band_mode element, band_mode math, band_mode call_args,
	                                 band_mode blocks, double budget_seconds, uint64_t* segments = nullptr, int max_depth = 3) {
		band_stats b;
		for (uint64_t seed = first_seed; seed < first_seed + count; ++seed) {
			const program p = generate_program(seed, max_depth, element, math, call_args, blocks);
			bool budget_hit = false;
			for (const auto& f : measure(p, budget_seconds, true, 0, &budget_hit)) if (f.label == label) b.add(f, budget_hit);
		}
		if (segments) *segments = b.segments;
		return b.pct(b.with_closed);
	}
	inline double order_closed_block_share(uint64_t first_seed, uint64_t count, bool argument_hazards, double budget_seconds) {
		band_stats b;
		for (uint64_t seed = first_seed; seed < first_seed + count; ++seed) {
			bool budget_hit = false;
			for (const auto& f : measure(order::generate(seed, argument_hazards).original, budget_seconds, true, 0, &budget_hit)) b.add(f, budget_hit);
		}
		return b.pct(b.with_closed);
	}

} // namespace jai::fuzz::census
