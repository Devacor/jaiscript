#pragma once

// Differential fuzzing harness: runs one generated program on a pair of engines and compares final results, printed
// output, error text, and engine-alive-afterward. Any mismatch is a divergence. The pair is the tree-walking interpreter
// against the bytecode VM, or (--vm-reference) the optimized VM against the VM in engine::vm_reference_mode, where a
// divergence is a bug of an optimized lane. Fully deterministic: everything derives from an explicit uint64 seed.

#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/value.hpp>
#include <atomic>
#include <cmath>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <cstdio>
#include <vector>

namespace jai::fuzz {

	// Generated programs are not statically clean and the differential measures runtime parity: every unit the harness
	// executes opens a [[warn_mode]] region on its first line, so the checker reports and the load proceeds
	inline constexpr const char* k_warn_region_prefix = "[[warn_mode]]; ";

	// Every segment either backend has finished: an isolated child's watchdog reads a hang as this standing still
	inline std::atomic<uint64_t> segments_finished{0};

	// ------------------------------------------------------------------
	// Deterministic RNG (splitmix64) - no random_device, no wall clock.
	// ------------------------------------------------------------------
	// Stafford mix13 finalizer: places each seed's stream at a pseudo-random
	// point of the splitmix orbit. (Seeding with seed*GAMMA aliases: seed N+1
	// would be seed N's stream shifted by a constant number of draws.)
	inline uint64_t mix64(uint64_t z) {
		z ^= z >> 33;
		z *= 0xFF51AFD7ED558CCDULL;
		z ^= z >> 33;
		z *= 0xC4CEB9FE1A85EC53ULL;
		z ^= z >> 33;
		return z;
	}

	struct fuzz_rng {
		uint64_t state;
		explicit fuzz_rng(uint64_t seed) : state(seed) {}
		uint64_t next() {
			uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
			return z ^ (z >> 31);
		}
		uint32_t below(uint32_t n) { return n == 0 ? 0 : static_cast<uint32_t>(next() % n); }
		bool chance(uint32_t percent) { return below(100) < percent; }
		int64_t range(int64_t lo, int64_t hi) { // inclusive
			return lo + static_cast<int64_t>(next() % static_cast<uint64_t>(hi - lo + 1));
		}
		template <typename T>
		const T& pick(const std::vector<T>& v) { return v[below(static_cast<uint32_t>(v.size()))]; }
	};

	// A generated program: one or more source segments executed sequentially on
	// the SAME engine (multiple segments exercise hot-reload / cross-execute state).
	struct program {
		uint64_t seed = 0;
		std::vector<std::string> segments;
		std::vector<std::string> labels;   // per segment: the band or generator part that wrote it (the census attributes by it)
		bool host_math_aliases = false;   // explicit host aliases exercise float-parameter conversion and the alias optimizer

		std::string joined() const {
			std::string out;
			for (size_t i = 0; i < segments.size(); ++i) {
				if (i) out += "\n// --- next execute() on same engine ---\n";
				out += segments[i];
			}
			return out;
		}
	};

	// ------------------------------------------------------------------
	// Value description: deterministic deep serialization for comparison.
	// ------------------------------------------------------------------
	inline const char* type_tag(const script_value& v) {
		switch (v.type()) {
			case script_value_type::jai_null_type: return "null";
			case script_value_type::jai_bool_type: return "bool";
			case script_value_type::jai_int_type: return "int";
			case script_value_type::jai_float_type: return "float";
			case script_value_type::jai_char_type: return "char";
			case script_value_type::jai_string_type: return "string";
			case script_value_type::jai_array_type: return "array";
			case script_value_type::jai_map_type: return "map";
			case script_value_type::jai_function_type: return "function";
			case script_value_type::jai_object_type: return "object";
			case script_value_type::jai_reference_type: return "reference";
			case script_value_type::jai_weak_ptr_type: return "weak_ptr";
			case script_value_type::jai_vec2_type: return "math::vec2";
			case script_value_type::jai_vec3_type: return "math::vec3";
			case script_value_type::jai_vec4_type: return "math::vec4";
			case script_value_type::jai_color_type: return "math::color";
			case script_value_type::jai_ivec2_type: return "math::ivec2";
			case script_value_type::jai_ivec3_type: return "math::ivec3";
			default: return "unknown";
		}
	}

	// The oracle's value description: the FULL value streams into a hash (every element of
	// every array kind, every map entry, every nesting level), and the bounded text the
	// report shows is a prefix of the same stream. A difference past the display cap or the
	// depth cap still moves the hash, and a typed array describes element by element with its
	// kind (the old as_array() view raised on typed nodes and turned [1] and [2] into the same
	// error text - review R8). Description failures are part of the stream, never an equal.
	struct value_stream {
		std::string text;
		uint64_t hash = 1469598103934665603ull;
		size_t text_cap = 4096;
		void put(std::string_view piece) {
			for (unsigned char c : piece) { hash ^= c; hash *= 1099511628211ull; }
			if (text.size() < text_cap) { text.append(piece.substr(0, text_cap - text.size())); }
		}
	};

	inline const char* array_kind_tag(const script_array& node) {
		if (!node.is_typed()) return "var";
		switch (node.kind()) {
			case script_array::kind_t::i64: return "int";
			case script_array::kind_t::f64: return "float";
			case script_array::kind_t::b8: return "bool";
			case script_array::kind_t::c8: return "char";
			case script_array::kind_t::u8: return "uint8";
			case script_array::kind_t::i8: return "int8";
			case script_array::kind_t::str: return "string";
			case script_array::kind_t::obj: return "object";
			default: return script_array::is_vec_kind(node.kind()) ? math_type_name(script_array::vec_value_type(node.kind())) : "typed";
		}
	}

	inline void stream_value(const script_value& v, value_stream& out, int depth) {
		if (depth > 64) { out.put("<depth>"); return; }
		try {
			switch (v.type()) {
				case script_value_type::jai_reference_type:
					out.put("ref->");
					stream_value(v.deref(), out, depth + 1);
					return;
				case script_value_type::jai_array_type: {
					const script_array* node = v.deref().unchecked_array_node();
					out.put("array<"); out.put(array_kind_tag(*node)); out.put(">[");
					size_t i = 0;
					node->for_each(node->owner, [&](const script_value& elem) {
						if (i++) out.put(", ");
						stream_value(elem, out, depth + 1);
					});
					out.put("]");
					return;
				}
				case script_value_type::jai_map_type: {
					const auto& m = v.deref().unchecked_as_map();
					out.put("map{");
					size_t i = 0;
					for (const auto& kv : m) {
						if (i++) out.put(", ");
						stream_value(kv.first, out, depth + 1);
						out.put(": ");
						stream_value(kv.second, out, depth + 1);
					}
					out.put("}");
					return;
				}
				default:
					out.put(type_tag(v)); out.put(":"); out.put(v.to_string());
					return;
			}
		} catch (const std::exception& ex) {
			out.put("<describe-error: "); out.put(ex.what()); out.put(">");
		}
	}

	inline std::string describe_value(const script_value& v, int depth = 0) {
		value_stream out;
		stream_value(v, out, depth);
		char tail[32];
		std::snprintf(tail, sizeof(tail), " #%016llx", static_cast<unsigned long long>(out.hash));
		return out.text + tail;
	}

	// ------------------------------------------------------------------
	// Oracle: run a program on one backend, capture everything observable.
	// ------------------------------------------------------------------
	struct run_outcome {
		std::vector<std::string> segment_results; // "VALUE <desc>" or "ERROR <what>"
		std::string printed;                      // everything print() emitted
		bool alive = false;                       // trivial follow-up execute succeeded
		bool budget_hit = false;                  // any segment raised a budget error
		uint64_t alias_binds = 0;                 // engine::parameter_alias_binds() after the run (gate coverage)
		uint64_t local_alias_binds = 0;           // engine::local_alias_binds() after the run (gate coverage)
		uint64_t lowered_locals = 0;              // engine::aggregate_lowered_locals() after the run (gate coverage)
	};

	struct run_config {
		bool use_vm = false;
		bool vm_reference = false;                // engine::vm_reference_mode (the vm side of the --vm-reference pair)
		double budget_seconds = 0.25;
		size_t print_cap = 32 * 1024;
		// Memory bombs (e.g. seed 8356's exponential `s += s`) stall inside allocations
		// where the wall-clock budget's 1024-tick gate never accumulates; the cap makes
		// them terminate deterministically.
		size_t memory_cap_bytes = 64 * 1024 * 1024;
		uint64_t step_budget = 0;                 // engine::execution_step_budget: a clock-free bound (gates that must not depend on load)
		bool proofs = true;                       // engine::type_proofs (the proof differential gate flips it)
		bool param_alias = true;                  // engine::parameter_alias and engine::local_alias (the elision differential gate flips them)
		bool passes = true;                       // engine::ir_passes (the middle end's differential gate flips it)
		int lowering = -1;                        // engine::aggregate_lowering: -1 the backend's default (interpreter boxed, vm lowered), else forced
		uint32_t miss_every = 0;                  // engine::cells_miss_every (the miss injection gate; a build with the lever)
		std::function<void(jai::engine&)> on_engine;                   // after the setup, before the first segment (the census)
		std::function<void(size_t, jai::engine&)> after_segment;       // after each segment's execute (the census)
	};

	inline bool contains_ci(const std::string& hay, const std::string& needle) {
		auto lower = [](std::string s) { for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c))); return s; };
		return lower(hay).find(lower(needle)) != std::string::npos;
	}

	inline run_outcome run_program(const program& p, const run_config& cfg) {
		run_outcome out;
		auto eng = jai::engine::make();
		eng->set_backend(cfg.use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
		eng->vm_reference_mode(cfg.vm_reference);
		// both backends run under one call-depth cap (the interpreter's): the documented defaults differ (256 / 250,000),
		// and the contract compared here is that equal caps raise the same catchable error at the same call
		eng->max_call_depth(jai::backend_type::vm, eng->max_call_depth(jai::backend_type::interpreter));
		eng->type_proofs(cfg.proofs);
		eng->parameter_alias(cfg.param_alias);
		eng->local_alias(cfg.param_alias);
		eng->ir_passes(cfg.passes);
		if (cfg.lowering >= 0) eng->aggregate_lowering(cfg.lowering != 0);
		if (cfg.miss_every) eng->cells_miss_every(cfg.miss_every);
		eng->execution_budget(cfg.budget_seconds);
		eng->execution_step_budget(cfg.step_budget);
		eng->memory_cap(cfg.memory_cap_bytes);
		if (p.host_math_aliases) {
			// Test-owned bindings, rather than incidental standard-library globals.
			eng->add_function("host_sqrt", [](script_float a_value) { return std::sqrt(a_value); });
			eng->add_function("host_abs", [](script_float a_value) { return std::fabs(a_value); });
			eng->add_function("host_floor", [](script_float a_value) { return std::floor(a_value); });
			eng->add_function("host_ceil", [](script_float a_value) { return std::ceil(a_value); });
			detail::engine_access::register_math_alias(*eng, "host_sqrt", "sqrt");
			detail::engine_access::register_math_alias(*eng, "host_abs", "abs");
			detail::engine_access::register_math_alias(*eng, "host_floor", "floor");
			detail::engine_access::register_math_alias(*eng, "host_ceil", "ceil");
		}
		if (cfg.on_engine) cfg.on_engine(*eng);

		auto printed = std::make_shared<std::string>();
		const size_t cap = cfg.print_cap;
		jai::engine* raw = eng.get();
		eng->add_variadic_function("print", [printed, cap, raw](const std::vector<script_value>& args) -> script_value {
			if (printed->size() < cap) {
				std::string line;
				for (size_t i = 0; i < args.size(); ++i) {
					if (i) line += " ";
					line += args[i].to_string();
				}
				line += "\n";
				printed->append(line);
				if (printed->size() > cap) {
					printed->resize(cap);
					printed->append("<print-capped>");
				}
			}
			return script_value(std::monostate{}, raw);
		});

		const bool trace = std::getenv("JAI_FUZZ_TRACE") != nullptr;   // triage: name each segment before it runs (a hang's location)
		for (const auto& seg : p.segments) {
			if (trace) { std::fprintf(stderr, "[trace] %s segment %zu\n", cfg.vm_reference ? "vm-ref" : cfg.use_vm ? "vm" : "interp", out.segment_results.size()); std::fflush(stderr); }
			std::string r;
			auto seg_start = std::chrono::steady_clock::now();
			try {
				script_value v = eng->execute(k_warn_region_prefix + seg);
				r = "VALUE " + describe_value(v);
			} catch (const std::exception& ex) {
				r = std::string("ERROR ") + ex.what();
				if (contains_ci(r, "budget exceeded")) out.budget_hit = true;
				// Memory-cap accounting is approximate and backend-dependent (env/clone
				// charges differ), so the trip point differs too: skip, never a finding.
				if (contains_ci(r, "memory cap exceeded")) out.budget_hit = true;
			}
			// A script can CATCH the budget error and keep going - then the two
			// backends print wall-clock-dependent amounts of output. Any segment
			// that consumed most of the budget is timing-sensitive: never a finding.
			double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - seg_start).count();
			if (cfg.budget_seconds > 0 && elapsed >= cfg.budget_seconds * 0.9) out.budget_hit = true;
			out.segment_results.push_back(std::move(r));
			segments_finished.fetch_add(1, std::memory_order_relaxed);
			if (cfg.after_segment) cfg.after_segment(out.segment_results.size() - 1, *eng);
		}
		try {
			out.alive = (eng->execute("1 + 1").as_int() == 2);
			out.alias_binds = detail::engine_access::parameter_alias_binds(*eng);
			out.local_alias_binds = detail::engine_access::local_alias_binds(*eng);
			out.lowered_locals = detail::engine_access::aggregate_lowered_locals(*eng);
		} catch (...) {
			out.alive = false;
		}
		out.printed = *printed;
		return out;
	}

	// ------------------------------------------------------------------
	// Comparison
	// ------------------------------------------------------------------
	// interp and vm are the pair's two sides: with vm_reference_pair, interp holds the optimized vm and vm the reference vm
	struct compare_result {
		enum kind_t { agree, diverge, budget_skip } kind = agree;
		bool budget_suspect = false; // only ONE backend hit the budget (possible hang divergence, wall-clock noise more likely)
		std::string diff;
		run_outcome interp;
		run_outcome vm;
	};

	inline const char* left_label(bool vm_reference_pair) { return vm_reference_pair ? "vm" : "interp"; }
	inline const char* right_label(bool vm_reference_pair) { return vm_reference_pair ? "vm-ref" : "vm"; }

	inline std::string first_difference(const std::string& a, const std::string& b, bool vm_reference_pair = false) {
		size_t i = 0;
		while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
		auto excerpt = [i](const std::string& s) {
			size_t begin = i > 40 ? i - 40 : 0;
			std::string e = s.substr(begin, 120);
			for (auto& c : e) if (c == '\n') c = '|';
			return e;
		};
		auto padded = [](std::string s) { s.resize(6, ' '); return s; };
		return "at byte " + std::to_string(i) +
			"\n      " + padded(left_label(vm_reference_pair)) + "(len " + std::to_string(a.size()) + "): ..." + excerpt(a) +
			"\n      " + padded(right_label(vm_reference_pair)) + "(len " + std::to_string(b.size()) + "): ..." + excerpt(b);
	}

	// lowering: 0 each backend's default (the interpreter boxed against the vm lowered: the lowering's differential), 1 both boxed,
	// 2 both lowered (with the other two it pins lowered == boxed on both backends). vm_reference_pair: the optimized vm (the
	// switches as given) against the reference vm (every optimization off whatever the switches say)
	inline compare_result compare_program(const program& p, double budget_seconds, bool proofs = true, bool param_alias = true, bool passes = true, uint32_t miss_every = 0, int lowering = 0, bool vm_reference_pair = false) {
		compare_result cr;
		run_config interp_cfg;
		interp_cfg.use_vm = vm_reference_pair;
		interp_cfg.budget_seconds = budget_seconds;
		interp_cfg.proofs = proofs;
		interp_cfg.param_alias = param_alias;
		interp_cfg.passes = passes;
		interp_cfg.miss_every = miss_every;
		interp_cfg.lowering = lowering == 0 ? -1 : lowering == 2 ? 1 : 0;
		run_config vm_cfg = interp_cfg;
		vm_cfg.use_vm = true;
		vm_cfg.vm_reference = vm_reference_pair;
		cr.interp = run_program(p, interp_cfg);
		cr.vm = run_program(p, vm_cfg);
		if (cr.interp.budget_hit || cr.vm.budget_hit) {
			// The budget is wall-clock: one side can time out while the other squeaks
			// by, so budget programs are never findings. Flag one-sided hits for review.
			cr.kind = compare_result::budget_skip;
			cr.budget_suspect = (cr.interp.budget_hit != cr.vm.budget_hit);
			return cr;
		}
		std::string diff;
		size_t n = cr.interp.segment_results.size();
		for (size_t i = 0; i < n; ++i) {
			const auto& a = cr.interp.segment_results[i];
			const auto& b = cr.vm.segment_results[i];
			if (a != b) {
				auto padded = [](std::string s) { s += ":"; s.resize(8, ' '); return s; };
				diff += "  segment " + std::to_string(i) + " result:\n    " + padded(left_label(vm_reference_pair)) + a + "\n    " + padded(right_label(vm_reference_pair)) + b + "\n";
			}
		}
		if (cr.interp.printed != cr.vm.printed) {
			diff += "  printed output differs " + first_difference(cr.interp.printed, cr.vm.printed, vm_reference_pair) + "\n";
		}
		if (cr.interp.alive != cr.vm.alive) {
			diff += std::string("  engine-alive-after: ") + left_label(vm_reference_pair) + "=" + (cr.interp.alive ? "yes" : "NO") +
				" " + right_label(vm_reference_pair) + "=" + (cr.vm.alive ? "yes" : "NO") + "\n";
		}
		if (!diff.empty()) {
			cr.kind = compare_result::diverge;
			cr.diff = diff;
		}
		return cr;
	}

	// ------------------------------------------------------------------
	// Metamorphic renaming: every generated name (a letter or two after an optional band prefix, then digits) gains a
	// suffix. Nothing the language defines may depend on a name's spelling, so the renamed program's outcome, with the
	// suffix taken back out, is the original's on the same backend.
	// ------------------------------------------------------------------
	inline const std::regex& generated_name_pattern() {
		static const std::regex pattern(R"(\b(u?[A-Za-z]{1,2}[0-9]+)\b)");
		return pattern;
	}

	inline program renamed(const program& p) {
		program out = p;
		for (auto& seg : out.segments) { seg = std::regex_replace(seg, generated_name_pattern(), "$1_r"); }
		return out;
	}

	inline std::string unrenamed(const std::string& text) {
		static const std::regex pattern(R"(\b(u?[A-Za-z]{1,2}[0-9]+)_r\b)");
		return std::regex_replace(text, pattern, "$1");
	}

	// A longer name moves every column after it: a location keeps its line and drops its column
	// (and a value's stream hash, taken over the names as the renamed program spelled them, string keys included)
	inline std::string without_columns(const std::string& text) {
		static const std::regex pattern(R"(:([0-9]+):([0-9]+))");
		static const std::regex hash(R"( #[0-9a-f]{16})");
		return std::regex_replace(std::regex_replace(text, pattern, ":$1:c"), hash, "");
	}

	// "" when the renamed run's outcome, names restored, is the original's
	inline std::string rename_difference(const run_outcome& original, const run_outcome& renamed_run) {
		std::string diff;
		for (size_t i = 0; i < original.segment_results.size() && i < renamed_run.segment_results.size(); ++i) {
			const std::string back = without_columns(unrenamed(renamed_run.segment_results[i]));
			const std::string mine = without_columns(original.segment_results[i]);
			if (mine != back) {
				diff += "  segment " + std::to_string(i) + " result:\n    original: " + mine + "\n    renamed:  " + back + "\n";
			}
		}
		const std::string printed = without_columns(unrenamed(renamed_run.printed));
		const std::string mine_printed = without_columns(original.printed);
		if (mine_printed != printed) { diff += "  printed output differs " + first_difference(mine_printed, printed) + "\n"; }
		if (original.alive != renamed_run.alive) { diff += "  engine-alive-after differs\n"; }
		return diff;
	}

	// Suppressions for known divergences, empty while none is open. An entry matches when every program needle appears in the
	// program text and every diff needle in the divergence summary; one list serves both pairs, and an entry's comment names the
	// side at fault (interp, vm, or an optimized vm lane against vm-ref). Fixing the divergence removes its entry.
	struct suppression {
		std::string id;
		std::vector<std::string> program_needles;
		std::vector<std::string> diff_needles;
	};

	inline const std::vector<suppression>& known_suppressions() {
		static const std::vector<suppression> list;
		return list;
	}

	inline std::string match_suppression(const program& p, const std::string& diff) {
		std::string text = p.joined();
		for (const auto& s : known_suppressions()) {
			bool ok = true;
			for (const auto& n : s.program_needles) {
				if (text.find(n) == std::string::npos) { ok = false; break; }
			}
			if (ok) {
				for (const auto& n : s.diff_needles) {
					if (diff.find(n) == std::string::npos) { ok = false; break; }
				}
			}
			if (ok) return s.id;
		}
		return {};
	}

	// ------------------------------------------------------------------
	// Minimizer: greedy structural reduction over source lines. Candidate
	// programs that go parse-invalid simply stop diverging and are rejected,
	// so structural validity is enforced by the oracle itself.
	// ------------------------------------------------------------------
	using diverges_fn = std::function<bool(const program&)>;

	namespace detail_min {
		inline std::vector<std::string> split_lines(const std::string& s) {
			std::vector<std::string> lines;
			std::string cur;
			for (char c : s) {
				if (c == '\n') { lines.push_back(cur); cur.clear(); }
				else cur += c;
			}
			if (!cur.empty()) lines.push_back(cur);
			return lines;
		}
		inline std::string join_lines(const std::vector<std::string>& lines) {
			std::string out;
			for (const auto& l : lines) { out += l; out += '\n'; }
			return out;
		}
		inline int brace_delta(const std::string& line) {
			int d = 0;
			bool in_str = false;
			char quote = 0;
			for (size_t i = 0; i < line.size(); ++i) {
				char c = line[i];
				if (in_str) {
					if (c == '\\') { ++i; continue; }
					if (c == quote) in_str = false;
				} else if (c == '"' || c == '\'') {
					in_str = true; quote = c;
				} else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
					break;
				} else if (c == '{') ++d;
				else if (c == '}') --d;
			}
			return d;
		}
	}

	struct minimize_stats { int checks = 0; int reductions = 0; };

	inline program minimize(program p, const diverges_fn& still_diverges, minimize_stats& stats, int max_checks = 600) {
		using namespace detail_min;
		auto check = [&](const program& cand) -> bool {
			if (stats.checks >= max_checks) return false;
			++stats.checks;
			return still_diverges(cand);
		};

		// Pass 0: drop whole segments (keep at least one).
		bool changed = true;
		while (changed && p.segments.size() > 1) {
			changed = false;
			for (size_t i = 0; i < p.segments.size() && p.segments.size() > 1; ++i) {
				program cand = p;
				cand.segments.erase(cand.segments.begin() + i);
				if (i < cand.labels.size()) cand.labels.erase(cand.labels.begin() + i);
				if (check(cand)) { p = cand; ++stats.reductions; changed = true; break; }
			}
		}

		for (size_t seg = 0; seg < p.segments.size(); ++seg) {
			bool outer_changed = true;
			while (outer_changed && stats.checks < max_checks) {
				outer_changed = false;
				auto lines = split_lines(p.segments[seg]);

				// Pass 1: ddmin-style chunk removal.
				for (size_t chunk = lines.size() / 2 ? lines.size() / 2 : 1; chunk >= 1; chunk /= 2) {
					for (size_t start = 0; start + chunk <= lines.size();) {
						std::vector<std::string> reduced;
						reduced.insert(reduced.end(), lines.begin(), lines.begin() + start);
						reduced.insert(reduced.end(), lines.begin() + start + chunk, lines.end());
						program cand = p;
						cand.segments[seg] = join_lines(reduced);
						if (check(cand)) {
							p = cand;
							lines = reduced;
							++stats.reductions;
							outer_changed = true;
						} else {
							start += chunk;
						}
					}
					if (chunk == 1) break;
				}

				// Pass 2: brace-block removal and unwrap (header line ... matching close).
				for (size_t i = 0; i < lines.size(); ++i) {
					if (brace_delta(lines[i]) <= 0) continue;
					int depth = 0;
					size_t close = i;
					bool found = false;
					for (size_t j = i; j < lines.size(); ++j) {
						depth += brace_delta(lines[j]);
						if (depth <= 0 && j > i) { close = j; found = true; break; }
					}
					if (!found) continue;
					// (a) remove whole block
					{
						std::vector<std::string> reduced(lines.begin(), lines.begin() + i);
						reduced.insert(reduced.end(), lines.begin() + close + 1, lines.end());
						program cand = p;
						cand.segments[seg] = join_lines(reduced);
						if (check(cand)) { p = cand; lines = reduced; ++stats.reductions; outer_changed = true; continue; }
					}
					// (b) unwrap: drop header + close, keep interior
					{
						std::vector<std::string> reduced(lines.begin(), lines.begin() + i);
						reduced.insert(reduced.end(), lines.begin() + i + 1, lines.begin() + close);
						reduced.insert(reduced.end(), lines.begin() + close + 1, lines.end());
						program cand = p;
						cand.segments[seg] = join_lines(reduced);
						if (check(cand)) { p = cand; lines = reduced; ++stats.reductions; outer_changed = true; }
					}
				}

				// Pass 3: per-line literal simplification (big ints -> 1, strings -> "s").
				for (size_t i = 0; i < lines.size(); ++i) {
					std::string simplified = lines[i];
					bool any = false;
					// shrink multi-digit integer literals
					std::string rebuilt;
					for (size_t j = 0; j < simplified.size();) {
						if (isdigit(static_cast<unsigned char>(simplified[j]))) {
							size_t k = j;
							while (k < simplified.size() && isdigit(static_cast<unsigned char>(simplified[k]))) ++k;
							bool is_float = (k < simplified.size() && simplified[k] == '.');
							if (!is_float && k - j > 1) { rebuilt += "1"; any = true; }
							else rebuilt += simplified.substr(j, k - j);
							j = k;
						} else {
							rebuilt += simplified[j];
							++j;
						}
					}
					if (!any) continue;
					auto candidate_lines = lines;
					candidate_lines[i] = rebuilt;
					program cand = p;
					cand.segments[seg] = join_lines(candidate_lines);
					if (check(cand)) { p = cand; lines = candidate_lines; ++stats.reductions; outer_changed = true; }
				}
			}
			p.segments[seg] = join_lines(split_lines(p.segments[seg]));
		}
		return p;
	}

	// Ready-to-paste Foundry test skeleton for a minimized repro (vm_reference_pair: the optimized vm against the reference vm)
	inline std::string foundry_skeleton(const program& p, const std::string& diff, bool vm_reference_pair = false) {
		std::ostringstream os;
		os << "// Divergence repro, fuzz seed " << p.seed << "\n";
		os << "// Diff observed:\n";
		std::istringstream ds(diff);
		std::string line;
		while (std::getline(ds, line)) os << "//   " << line << "\n";
		os << "test(\"fuzz_seed_" << p.seed << "_divergence\", [this]() {\n";
		os << "\tstd::string out[2];\n";
		os << "\tint idx = 0;\n";
		os << "\tfor (bool " << (vm_reference_pair ? "reference" : "use_vm") << " : {false, true}) {\n";
		os << "\t\tauto e = jai::engine::make();\n";
		if (vm_reference_pair) {
			os << "\t\te->set_backend(jai::backend_type::vm);\n";
			os << "\t\te->vm_reference_mode(reference);\n";
		} else {
			os << "\t\te->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);\n";
		}
		os << "\t\te->max_call_depth(jai::backend_type::vm, e->max_call_depth(jai::backend_type::interpreter));\n";
		os << "\t\te->execution_budget(0);\n";
		for (size_t i = 0; i < p.segments.size(); ++i) {
			os << "\t\ttry { out[idx] += e->execute(R\"__jai(" << k_warn_region_prefix << "\n" << p.segments[i] << ")__jai\").to_string(); }\n";
			os << "\t\tcatch (const std::exception& ex) { out[idx] += std::string(\"ERROR: \") + ex.what(); }\n";
		}
		os << "\t\t++idx;\n";
		os << "\t}\n";
		os << "\tcheck_eq(out[0], out[1], \"" << (vm_reference_pair ? "vm reference parity" : "backend parity") << ", fuzz seed " << p.seed << "\");\n";
		os << "});\n";
		return os.str();
	}

} // namespace jai::fuzz
