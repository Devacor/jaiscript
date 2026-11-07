// parallel_transform: one builtin, three phases, all charged to the call itself:
//
//   ADMISSION (amortized per body): a fail-closed static walk over fn's AST — every node
//   kind is whitelisted or the call errors ("parallel_transform: <reason> at line:col").
//   The walk also snapshots the transitive call graph (script functions + whitelisted
//   host functions) and every parse-time type the bodies reference.
//
//   BARRIER (single-threaded): per-worker execution contexts are acquired — REUSED from
//   the engine's slot pool when the provisioning fingerprint still matches (reset:
//   limits re-slice, budget re-arm, residual-state clear, host-copy refresh), rebuilt
//   otherwise. A fresh build gives each worker its own backend
//   instance (engine's configured type), root environment with parent = nullptr (the
//   hard partition: a lookup that would escape fails as undefined, it cannot race),
//   private env-epoch sink, own execution_limits, per-worker copies of every callable
//   the body needs (fresh strong_ptrs — invoking them bumps no shared refcount). The
//   source elements pass the fork's element proof (prove_elements: all-primitive by
//   construction, object-kind by declared type, everything else by one normalizing walk)
//   and workers then read the source LIVE — the by-value binding makes each private
//   copy. Type shapes are pre-warmed and the symbolizer/type tables freeze.
//
//   REGION: blocks of consecutive elements claimed in order by whichever thread is free
//   (detail/parallel_schedule.hpp), or contiguous chunks split by the optional weight hint
//   evaluated at the barrier; pinned pool workers + the calling thread, disjoint writes
//   into the pre-sized result. engine::execution_limits() resolves to
//   the calling worker's accounting through the installed region table. First error in
//   ITERATION order wins at the join; a terminal worker failure (budget) latches the
//   engine's terminal rail.

#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/integer_ops.hpp>
#include <jaiscript/detail/const_rules.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/engine_time.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/parallel_transform.hpp>
#include <jaiscript/detail/thread_pool_access.hpp>
#include <jaiscript/detail/ref_param_writes.hpp>
#include <jaiscript/detail/lvalue_shape.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/detail/ast_serializer.hpp>
#include <jaiscript/detail/return_conv.hpp>
#include <jaiscript/detail/math_intrinsics.hpp>
#include <jaiscript/stdlib/random.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/vm/vm_backend.hpp>

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstring>
#include <exception>
#include <optional>
#include <unordered_set>

#ifdef JAISCRIPT_VM_PROFILE
#include <intrin.h>   // __rdtsc: region phase probes (diagnostic builds only)
#include <cstdio>
#endif

namespace jai::detail {

	// Per-call worker failure record (first error in the slot's chunk)
	struct parallel_worker_error {
		size_t iteration = 0;
		std::error_code code{};
		std::string message;
		bool terminal = false;
		std::exception_ptr escaped;   // a C++ exception no backend converted: the join rethrows it
	};

	// One reusable worker execution context. Built inside a region, RESET per call
	// afterwards; rebuilt only when the fingerprint stops matching. Owned by
	// parallel_engine_state::worker_slots (engine lifetime).
	struct parallel_worker_slot {
		~parallel_worker_slot() {
			// Private callables close over this environment. Break that cycle before
			// the backend and its epoch sink die, including partially built slots.
			if (root_env) { root_env->clear_values(); }
			if (method_env) { method_env->clear_values(); }
		}

		std::unique_ptr<string_symbolizer> env_epoch_sink;
		std::shared_ptr<environment> root_env;
		// Pinned method bodies resolve here: a method's captures are its class's globals, the
		// body's captures may be the caller's locals, and one name can mean both at once
		std::shared_ptr<environment> method_env;
		execution_limits limits;
		std::unique_ptr<execution_backend> backend;
		vm::vm_backend* vm = nullptr;                     // set when the engine backend is the vm
		script_callable fn_payload;
		// parallel_for: the per-worker chunk-loop wrapper (k_parallel_chunk_source bound to this
		// worker's root_env) and the cursor holder its advance intrinsic re-points
		script_callable chunk_payload;
		// Instances this worker constructed or copied during the region (pool workers only;
		// registered by the join, expired entries skipped)
		std::vector<pending_registration> pending_instances;
		// What destructors that failed on this chunk report, replayed by the join
		std::vector<deferred_report> deferred_reports;
		std::vector<std::pair<const void*, weaker_ptr<class_instance>>> pending_script_layers;   // recorded by the join
		std::vector<deferred_release> deferred_releases;   // released by the join
		object_borrows borrows;   // the shared objects this worker read; the join swaps escaped borrows for handles, then clears
		cell_counters cells_folded{};   // this worker's block counters already folded into the engine's
		parallel_schedule_cursor sched;   // this worker's place in the region's shared blocks (schedule null: the static partition)
		std::vector<script_value> cursors;   // [0] = the range's chunk cursor, then the zip's arrays
		std::optional<parallel_counted_sequence> counted;   // the counted form's range: cursor 0 only counts its elements
		std::vector<script_value> call_args; // prepared at the barrier; no worker-entry allocation
		// Live input vector ("safe AND free"): workers read the
		// caller's vector directly - all-primitive elements (typed proof or one dynamic
		// scan) mint worker-local copies with zero refcount traffic, and elements the
		// fork's element proof made exclusive lend a handle only their chunk's worker
		// touches. Non-null only DURING a region (points into the run call's locals; the
		// join guard nulls it).
		const std::vector<script_value>* raw_input = nullptr;
		// By-value input on an object-kind typed node (both forms): each iteration boxes
		// its slot (a handle copy of the chunk-exclusive instance) and the by-value binding
		// clones it. Non-null only DURING a region, like raw_input.
		const script_array* typed_input = nullptr;
		// In-place region (parallel_for): pre-minted element references for this slot's
		// chunk (index i - begin), bound to the body's by-ref parameter per iteration.
		// Minted single-threaded at the barrier (each holder pins the container handle);
		// cleared at the join - a dormant slot must never pin the caller's array.
		std::vector<script_value> element_refs;
		size_t begin = 0;          // the chunk's first element
		size_t end = 0;            // the range's length (the failing-index clamp)
		size_t count = 0;          // elements this chunk owns
		size_t block = 0;          // block-cyclic step: blocks of `block`, every stride_blocks-th one (0 = contiguous)
		size_t stride_blocks = 1;
		std::optional<parallel_worker_error> error;

		// Captured-read provisioning for THIS call: name ids defined into root_env
		// (nulled at the join - a dormant slot must hold no borrow raw pointers), and
		// the heavy nodes of every snapshot/string capture (a result that reaches into
		// one of these must detach, or two iterations on one worker could alias while
		// two on different workers don't - a worker-count-visible difference)
		std::vector<uint64_t> capture_name_ids;
		std::unordered_set<const void*> provisioned_nodes;

		// Snapshot-capture reuse across regions (the transform-barrier heat: GLOOM's
		// string tables were re-detached per worker per region, 3.28M of 3.57M setup
		// cyc). The cached DETACHED COPY is itself the validity reference: a hit
		// requires the live source to content-match it under flat_snapshot_equal, and
		// a record is only stored when that predicate can prove the shape (flat
		// string/scalar tables) - store-condition == hit-condition. Cache values pin
		// only worker-owned detached nodes, never caller state (dormant-slot safe);
		// the join's define-nulling is unchanged.
		struct snapshot_capture_cache_entry {
			uint64_t name_id = 0;
			uint64_t built_serial = 0;           // region_use_counter at store (validation memo key)
			script_value value{ std::monostate{}, static_cast<engine*>(nullptr) };
			std::vector<const void*> nodes;      // provisioned_nodes contribution
			std::vector<type_info*> types;       // prewarm (value_types) contribution
		};
		std::vector<snapshot_capture_cache_entry> snapshot_cache;

		// Barrier-admitted class methods for THIS call: worker-private minted dispatcher
		// values + pre-resolved overloads (parallel-method-admission rule). Rebuilt per
		// call (element classes are a runtime fact); cleared at the join.
		detail::parallel_method_pin_table method_pins;
		// The method admission record the pins were built for (0: none): a reused slot keeps them, and the constructors and
		// function copies they define in its scopes, while the region runs under the same record
		uint64_t pins_serial = 0;
		std::vector<uint64_t> pinned_name_ids;

		// Provisioning fingerprint: reuse is valid only while these match the current
		// call. Script bodies are identity-compared (hot reload swaps bodies -> rebuild);
		// host copies are refreshed on every reuse instead of fingerprinted.
		bool fingerprint_use_vm = false;
		const void* fingerprint_fn_body = nullptr;
		std::vector<std::pair<uint64_t, const void*>> fingerprint_script_bodies;
		// Read-only parameter proofs travel from the live function to its worker copy: the
		// barrier (single-threaded) proves the source and restamps every copy per region entry,
		// so a worker never proves (interning, global lookups) and never runs stale
		std::vector<std::pair<std::shared_ptr<script_defined_function>, std::shared_ptr<script_defined_function>>> alias_stamps;
		std::vector<std::pair<uint64_t, std::string>> host_functions;   // refreshed each reuse
		// this worker's copy of each host function a body calls, kept while the program scope binds the name to the same function
		// (source pins it, so the identity names one function): a registration or redefinition binds another and the copy is minted again
		struct host_copy { const void* identity = nullptr; script_value source{std::monostate{}, nullptr}; script_value copy{std::monostate{}, nullptr}; };
		std::unordered_map<uint64_t, host_copy> host_copies;

	};

	parallel_engine_state::parallel_engine_state() = default;
#ifdef JAISCRIPT_VM_PROFILE
	parallel_engine_state::~parallel_engine_state() {
		auto dump = [](const char* name, const region_phase_prof& p) {
			if (!p.regions) return;
			static const char* const phases[] = { "captures", "elements", "methods", "partition", "slots", "prewarm", "fan-out", "drain" };
			static_assert(std::size(phases) == static_cast<size_t>(region_phase::count));
			fprintf(stderr, "[region-profile] %s: %llu regions, per region:", name, (unsigned long long)p.regions);
			for (size_t i = 0; i < std::size(phases); ++i) { fprintf(stderr, " %s %.0f", phases[i], (double)p.phase_cyc[i] / p.regions); }
			fprintf(stderr, " cyc\n  slots: reused %llu | built %llu | pins kept %llu | pins built %llu\n", (unsigned long long)p.slot_resets,
				(unsigned long long)p.slot_provisions, (unsigned long long)p.pins_kept, (unsigned long long)p.pins_built);
			if (p.cache_hits + p.cache_misses + p.cache_unprovable) {
				fprintf(stderr, "  snapshot cache: hits %llu | misses %llu | stores %llu | unprovable %llu\n",
					(unsigned long long)p.cache_hits, (unsigned long long)p.cache_misses,
					(unsigned long long)p.cache_stores, (unsigned long long)p.cache_unprovable);
				for (const auto& [nm, cnt] : p.cache_miss_names) {
					const auto cyc_it = p.cache_miss_cyc.find(nm);
					fprintf(stderr, "    miss %-24s %8llu x %10.2f Mcyc detach\n", nm.c_str(),
						(unsigned long long)cnt,
						cyc_it != p.cache_miss_cyc.end() ? cyc_it->second / 1e6 : 0.0);
				}
			}
		};
		dump("parallel_transform", prof_transform);
		dump("parallel_for", prof_for);
	}
#else
	parallel_engine_state::~parallel_engine_state() = default;
#endif

namespace {

	// The next entry in element order over the chunks' lists, each already in element order (a chunk runs its elements in
	// increasing order); SIZE_MAX: every list is consumed
	template <typename List>
	size_t next_in_element_order(const std::vector<List*>& lists, const std::vector<size_t>& at) {
		size_t best = SIZE_MAX, best_element = SIZE_MAX;
		for (size_t i = 0; i < lists.size(); ++i) {
			if (at[i] < lists[i]->size() && (best == SIZE_MAX || (*lists[i])[at[i]].element < best_element)) { best = i; best_element = (*lists[i])[at[i]].element; }
		}
		return best;
	}
	// One drain for the normal join and exceptional-exit retry, in element order (the serial loop's registration order) whichever
	// chunk ran the element. Erasing only the consumed prefixes prevents duplicate registration while preserving live objects
	// that escaped through in-place stores. weak_ptr movement/erase does not allocate.
	void drain_pending_instances(parallel_engine_state& state, const std::vector<parallel_worker_slot*>& slots) {
		std::vector<std::vector<pending_registration>*> lists;
		for (auto* slot : slots) { lists.push_back(&slot->pending_instances); }
		std::vector<size_t> at(lists.size(), 0);
		try {
			for (size_t i = next_in_element_order(lists, at); i != SIZE_MAX; i = next_in_element_order(lists, at)) {
				auto& weak = (*lists[i])[at[i]].instance;
				if (auto live = weak.lock()) {
					state.faults.checkpoint("region.instance_register");
					live->get_class_definition()->register_instance_after_region(weak, &state.faults, live.get());
				}
				++at[i];
			}
		} catch (...) {
			for (size_t i = 0; i < lists.size(); ++i) { lists[i]->erase(lists[i]->begin(), lists[i]->begin() + at[i]); }
			throw;
		}
		for (auto* list : lists) { list->clear(); }
	}

	constexpr size_t k_parallel_small_n = 2;   // one element has nothing to split; from two on, the measured-cost cap below decides
	                                           // (a fixed 16 kept GLOOM's 15 wall chunks, 27% of its frame, on the calling thread)
	// A pool worker earns its keep when its chunk is worth k_parallel_chunk_overhead_ratio times
	// the fixed cost of waking and joining it - both sides measured per statement on the machine
	// at hand (no constants from one box). Chunking never changes results (iteration order wins
	// at the join), so the count is a pure cost knob. The first region of a statement has no
	// estimate: every thread, even split (the baseline).
	constexpr double k_parallel_chunk_overhead_ratio = 3.0;
	constexpr double k_parallel_min_overhead_ns = 2000.0;   // floor for a noisy first sample
	// Body control flow. Inline mode (the body grafted into the worker's counter loop): a
	// top-level `continue` reaches the loop's advance on its own and a top-level `return`
	// (any value) becomes `continue` - both mean the next element; a `return` inside a NESTED
	// loop cannot mean that in a grafted body, so such a body takes call mode instead (one
	// call per element, where a top-level `continue` becomes a bare `return`). A loop-level
	// `break` is a parse error (iterations are independent); inside a nested loop or switch
	// break/continue keep their C++ meaning.
	struct body_control_scan {
		bool nested_return = false;
	};
	void scan_parallel_body_control(const statement_ptr& slot, size_t loop_depth, body_control_scan& out) {
		if (!slot) { return; }
		switch (slot->get_type()) {
		case node_type::return_stmt:
			if (loop_depth > 0) { out.nested_return = true; }
			return;
		case node_type::block_stmt:
			for (auto& d : static_cast<block_stmt*>(slot.get())->declarations) {
				if (d && d->get_type() == node_type::statement_decl) {
					scan_parallel_body_control(static_cast<statement_decl*>(d.get())->statement, loop_depth, out);
				}
			}
			return;
		case node_type::if_stmt: {
			auto* n = static_cast<if_stmt*>(slot.get());
			scan_parallel_body_control(n->then_statement, loop_depth, out);
			scan_parallel_body_control(n->else_statement, loop_depth, out);
			return;
		}
		case node_type::while_stmt:
			scan_parallel_body_control(static_cast<while_stmt*>(slot.get())->body, loop_depth + 1, out); return;
		case node_type::for_stmt:
			scan_parallel_body_control(static_cast<for_stmt*>(slot.get())->body, loop_depth + 1, out); return;
		case node_type::range_for_stmt:
			scan_parallel_body_control(static_cast<range_for_stmt*>(slot.get())->body, loop_depth + 1, out); return;
		case node_type::try_stmt: {
			auto* n = static_cast<try_stmt*>(slot.get());
			scan_parallel_body_control(n->try_block, loop_depth, out);
			scan_parallel_body_control(n->catch_block, loop_depth, out);
			return;
		}
		case node_type::switch_stmt: {
			auto* n = static_cast<switch_stmt*>(slot.get());
			for (auto& c : n->cases) { for (auto& st : c->body) { scan_parallel_body_control(st, loop_depth, out); } }
			if (n->default_case) { for (auto& st : n->default_case->body) { scan_parallel_body_control(st, loop_depth, out); } }
			return;
		}
		default:
			return;
		}
	}
	// Top-level control rewrite (loop depth 0 only; nested loops keep theirs): inline mode
	// turns `return` into `continue`, call mode turns `continue` into a bare `return`.
	void rewrite_parallel_body_control(statement_ptr& slot, size_t loop_depth, bool inline_mode) {
		if (!slot) { return; }
		switch (slot->get_type()) {
		case node_type::return_stmt:
			if (inline_mode && loop_depth == 0) { slot = std::make_shared<continue_stmt>(slot->location); }
			return;
		case node_type::continue_stmt:
			if (!inline_mode && loop_depth == 0) { slot = std::make_shared<return_stmt>(slot->location); }
			return;
		case node_type::block_stmt:
			for (auto& d : static_cast<block_stmt*>(slot.get())->declarations) {
				if (d && d->get_type() == node_type::statement_decl) {
					rewrite_parallel_body_control(static_cast<statement_decl*>(d.get())->statement, loop_depth, inline_mode);
				}
			}
			return;
		case node_type::if_stmt: {
			auto* n = static_cast<if_stmt*>(slot.get());
			rewrite_parallel_body_control(n->then_statement, loop_depth, inline_mode);
			rewrite_parallel_body_control(n->else_statement, loop_depth, inline_mode);
			return;
		}
		case node_type::while_stmt:
			rewrite_parallel_body_control(static_cast<while_stmt*>(slot.get())->body, loop_depth + 1, inline_mode); return;
		case node_type::for_stmt:
			rewrite_parallel_body_control(static_cast<for_stmt*>(slot.get())->body, loop_depth + 1, inline_mode); return;
		case node_type::range_for_stmt:
			rewrite_parallel_body_control(static_cast<range_for_stmt*>(slot.get())->body, loop_depth + 1, inline_mode); return;
		case node_type::try_stmt: {
			auto* n = static_cast<try_stmt*>(slot.get());
			rewrite_parallel_body_control(n->try_block, loop_depth, inline_mode);
			rewrite_parallel_body_control(n->catch_block, loop_depth, inline_mode);
			return;
		}
		case node_type::switch_stmt: {
			auto* n = static_cast<switch_stmt*>(slot.get());
			for (auto& c : n->cases) { for (auto& st : c->body) { rewrite_parallel_body_control(st, loop_depth, inline_mode); } }
			if (n->default_case) { for (auto& st : n->default_case->body) { rewrite_parallel_body_control(st, loop_depth, inline_mode); } }
			return;
		}
		default:
			return;
		}
	}

	inline size_t cap_workers_by_cost(size_t worker_count, size_t n, const parallel_engine_state& state, const parallel_engine_state::parallel_region_pool& pool) {
		if (pool.ns_per_element <= 0.0 || state.worker_overhead_ns <= 0.0) { return worker_count; }
		const double min_chunk_ns = k_parallel_chunk_overhead_ratio * std::max(state.worker_overhead_ns, k_parallel_min_overhead_ns);
		const double affordable = (static_cast<double>(n) * pool.ns_per_element) / min_chunk_ns;
		const size_t by_cost = affordable >= 1.0 ? static_cast<size_t>(affordable) : 1;
		return std::min(worker_count, by_cost);
	}

	// This REGION's persistent slot pool by fingerprint (fn body + backend): distinct regions
	// cycling in one frame each keep their own provisioned contexts. LRU-evict beyond the cap;
	// selected before any slot reference is taken (the pools vector must not reallocate for
	// the rest of the call).
	parallel_engine_state::parallel_region_pool& acquire_region_pool(parallel_engine_state& state, const void* pool_key, bool use_vm) {
		parallel_engine_state::parallel_region_pool* pool = nullptr;
		for (auto& candidate : state.region_pools) {
			if (candidate.fn_body == pool_key && candidate.use_vm == use_vm) {
				pool = &candidate;
				break;
			}
		}
		if (!pool) {
			if (state.region_pools.size() >= parallel_engine_state::max_region_pools) {
				auto evict = state.region_pools.begin();
				for (auto it = state.region_pools.begin(); it != state.region_pools.end(); ++it) {
					if (it->last_use < evict->last_use) { evict = it; }
				}
				*evict = parallel_engine_state::parallel_region_pool{};
				pool = &*evict;
			} else {
				state.region_pools.emplace_back();
				pool = &state.region_pools.back();
			}
			pool->fn_body = pool_key;
			pool->use_vm = use_vm;
		}
		pool->last_use = ++state.region_use_counter;
		return *pool;
	}

	// Main's own chunk, timed: the element cost as the worker path runs it feeds the next region
	inline void note_region_cost(parallel_engine_state::parallel_region_pool& pool, std::chrono::steady_clock::time_point chunk_t0, size_t main_elements) {
		if (!main_elements) { return; }
		const auto chunk_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - chunk_t0).count();
		const double sample = static_cast<double>(chunk_ns) / static_cast<double>(main_elements);
		pool.ns_per_element = pool.ns_per_element > 0.0 ? (pool.ns_per_element + sample) * 0.5 : sample;   // half-life of one region: no flapping on a noisy sample
	}

	// Once per pool build: what a pool worker costs to submit, wake and join on this machine
	// (no-op tasks, three round trips, the minimum - the first trip also warms the threads).
	// Paid once by the first fan-out region, ~100 us; results never depend on it.
	inline void calibrate_pool_overhead(parallel_engine_state& state) {
		const size_t workers = state.pool->worker_count();
		if (!workers) { return; }
		std::vector<std::pair<size_t, jai::thread_pool::task_type>> tasks;
		state.faults.checkpoint("calibration.task_storage");
		tasks.reserve(workers);
		for (size_t k = 0; k < workers; ++k) {
			state.faults.checkpoint("calibration.task_build", k);
			tasks.emplace_back(k, [] {});
		}
		double best = 0.0;
		for (int round = 0; round < 3; ++round) {
			for (auto& task : tasks) { task.second = [] {}; }   // a submitted batch moves its tasks out
			const auto t0 = std::chrono::steady_clock::now();
			detail::thread_pool_access::submit_pinned_batch(*state.pool, tasks.data(), tasks.size(), &state.faults);
			state.pool->wait_idle();
			const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
			const double per_worker = ns / static_cast<double>(workers);
			best = best > 0.0 ? std::min(best, per_worker) : per_worker;
		}
		state.worker_overhead_ns = best;
	}

	// A host function a script namespace holds (time::now, web::url_encode), or null for anything else under that name
	const script_value* namespace_host_entry(engine& eng, std::string_view space, std::string_view member) {
		auto& spaces = engine_access::script_namespaces(eng);
		const auto found = spaces.find(engine_access::symbolize(eng, space));
		if (found == spaces.end() || !found->second) { return nullptr; }
		const uint64_t member_id = engine_access::symbolize(eng, member);
		if (found->second->functions.count(member_id)) { return nullptr; }
		const auto value = found->second->variables.find(member_id);
		return value != found->second->variables.end() && value->second.is_function() ? &value->second : nullptr;
	}

	// The library builtins that stay parallel (parallel_pure_function): pure math, to_string, the web:: text functions and crypto:: (apart from its file read); the rest register serial
	bool parallel_host_whitelisted(std::string_view name) {
		static constexpr std::string_view k_whitelist[] = {
			"abs", "sign", "min", "max", "clamp",
			"floor", "ceil", "round", "trunc",
			"sqrt", "cbrt", "pow", "exp", "exp2", "log", "log2", "log10",
			"sin", "cos", "tan", "asin", "acos", "atan", "atan2",
			"sinh", "cosh", "tanh",
			"degrees", "radians", "fmod", "hypot",
			"lerp", "mix", "unmix", "mix_in", "mix_out", "mix_in_out", "mix_out_in",
			"unmix_in", "unmix_out", "unmix_in_out", "unmix_out_in",
			"saturate", "wrap", "remap", "is_nan", "is_inf",
			"to_string",
			"web::html_escape", "web::html_unescape", "web::url_encode", "web::url_decode", "web::form_encode", "web::form_decode",
			"web::parse_query", "web::parse_query_all", "web::build_query", "web::parse_url",
			"crypto::sha256", "crypto::sha256_bytes", "crypto::sha512", "crypto::sha512_bytes", "crypto::sha1", "crypto::sha1_bytes", "crypto::md5", "crypto::md5_bytes",
			"crypto::hmac_sha256", "crypto::hmac_sha256_bytes", "crypto::hmac_sha512", "crypto::hmac_sha512_bytes", "crypto::equals",
			"crypto::random_bytes", "crypto::random_hex", "crypto::random_int", "crypto::password_hash", "crypto::password_verify",
		};
		for (auto w : k_whitelist) {
			if (name == w) { return true; }
		}
		return false;
	}

	bool primitive_ctor_name(std::string_view name) {
		return name == "int" || name == "int64" || name == "float" || name == "double" ||
		       name == "string" || name == "char" || name == "bool";
	}

	// Read-only builtin container/string methods admissible on a CAPTURED receiver
	// (union of the array/map/string registries' non-mutating entries). Captured method
	// receivers always provision as per-worker snapshots, so even element-copying
	// methods (keys/values/slice/front/...) only touch worker-private structure. A
	// method outside this list may mutate the receiver - that write would land on the
	// worker's private snapshot and silently diverge across worker counts, so admission
	// rejects it (the statically-free half of the write wall).
	bool parallel_readonly_method(std::string_view name) {
		static constexpr std::string_view k_readonly[] = {
			// array
			"size", "empty", "front", "back", "index_of", "has", "contains",
			"first", "last", "length", "slice", "join",
			// map
			"keys", "values", "get", "to_array",
			// string
			"at", "substr", "find", "rfind", "find_first_of", "find_last_of",
			"find_first_not_of", "find_last_not_of",
		};
		for (auto m : k_readonly) {
			if (name == m) { return true; }
		}
		return false;
	}

	const script_callable* payload_from_function_value(const script_value& v) {
		if (!v.is_function()) { return nullptr; }
		const script_function& f = v.as_function();
		if (const auto* thunk = f.target<script_callable_thunk>()) { return &thunk->payload; }
		return nullptr;
	}

	// Class::K as a constant: the class the name means and a const static of a primitive value it holds
	std::optional<script_value> class_constant_value(engine& eng, uint64_t class_name_id, uint64_t member_id) {
		const auto cls = engine_access::get_class_definition(eng, std::string(engine_access::get_symbolizer(eng)->get_string(class_name_id)));
		if (!cls || !cls->is_const_static(member_id) || !cls->static_method_in_chain(member_id).is_null()) { return std::nullopt; }
		const script_value* value = cls->find_static_field_in_chain(member_id);
		if (!value) { return std::nullopt; }
		const script_value& v = value->deref();
		const size_t idx = v.raw_storage_index();
		if (idx != script_value::TYPEID_INT && idx != script_value::TYPEID_FLOAT && idx != script_value::TYPEID_BOOL && idx != script_value::TYPEID_CHAR) { return std::nullopt; }
		return script_value(v);
	}

	// ============================== ADMISSION ==============================

	std::string counted_variable_written(std::string_view name) {
		return "the body writes the counted loop's variable '" + std::string(name) + "' (the loop steps it; copy it into a local to change it)";
	}

	// At namespace scope: a nested class's default member initializers are unusable in its enclosing class's default arguments
	// referent: a reference's declared target type; const_ref_use: a const T& parameter's slot in admission_builder::const_ref_uses
	struct admission_local_info { type_info_ptr type; bool primitive = false; bool reference = false; bool param = false; type_info_ptr referent; size_t const_ref_use = SIZE_MAX; };

	struct admission_builder {
		engine* eng;
		string_symbolizer* sym;
		parallel_admission adm;
		std::unordered_set<const void*> visited_bodies;
		std::unordered_set<uint64_t> collected_fn_ids;
		std::unordered_set<uint64_t> collected_host_ids;
		std::unordered_set<type_info*> collected_types;
		std::unordered_map<uint64_t, size_t> capture_index;   // name id -> adm.captures slot
		// A local's declared type; primitive: it can never hold a class instance (a primitive or string type, or an auto
		// slot the runtime locks to a primitive initializer), so dispatching on it runs no user code
		using local_info = admission_local_info;
		std::vector<std::unordered_map<uint64_t, local_info>> scopes;   // current function's scopes
		bool failed = false;
		// parallel_for bodies (in-place root mode): the element may bind by reference and
		// diagnostics carry the parallel_for prefix. Member access and member STORES are
		// admitted in BOTH forms when the chain roots at a LOCAL identifier - the element
		// (parallel_for's chunk-exclusive owned element, the transform's private by-value
		// copy) or a worker-private local. Enclosing-rooted member access stays rejected
		// (capture classification cannot provision objects). Applies to the whole call
		// graph: helper functions do member work on their own params.
		bool inplace_root = false;
		uint64_t element_symbol = UINT64_MAX;   // the body's first element parameter / loop variable
		std::vector<uint64_t> element_symbols;   // every element root: the zip's further loop variables too
		bool is_element_symbol(uint64_t id) const {
			if (local_mode && fn_depth != 0) { return false; }   // a callee's same-named local is its own
			if (id == element_symbol) { return true; }
			for (uint64_t e : element_symbols) { if (e == id) { return true; } }
			return false;
		}
		const char* prefix = "parallel_transform";

		// Read-only parameter proof (prove_readonly_parameters): the root is a by-value parameter
		// of a serial callee. The parallel rules already reject every enclosing write; on top,
		// any identity-observing use of the root escapes it - a bare use (comparison, argument,
		// return, alias, range source), a member/subscript chain whose static terminal is not a
		// primitive or string (objects and arrays compare by identity), a user method or
		// accessor on it, a compound assignment or ++/-- on it (user operators). Rebinding the
		// parameter itself (`p = x`) is not an escape: it replaces the slot, never the referent.
		bool proof_mode = false;
		bool root_escaped = false;
		type_info_ptr root_type;

		// Read-only local proof (prove_local_aliases): the root is a by-value local declaration `T x = lvalue;` and the walk
		// covers the statements after it in its block (depth 0, the function's locals visible there pre-declared) and every
		// function they call (depth > 0). On top of the parameter rules, nothing in that range may write the source's object:
		// a write rooted at a local lands only in that local's own value tree (a by-value chain from a by-value local other
		// than the source's root) or replaces a local's slot; a write through a reference or a handle may reach the source
		// and escapes (a callee's reference parameter defers to its call sites, which judge the argument they pass)
		bool local_mode = false;
		int fn_depth = 0;
		uint64_t source_root = UINT64_MAX;   // the initializer's root local (UINT64_MAX: a global)

		// Both proofs: an instance dying while the root lives runs code the walk cannot see, so every place a value of a type
		// whose tree can hold an instance with a script destructor or a copy constructor dies escapes the root (a by-value local or parameter, a
		// replaced slot, a container's mutation, a by-value call result or cast); an untyped value may hold one
		bool type_runs_destructor(const type_info_ptr& t) {
			if (!t) { return true; }
			switch (t->base_type) {
			case script_value_type::jai_null_type:
			case script_value_type::jai_int_type: case script_value_type::jai_float_type: case script_value_type::jai_bool_type:
			case script_value_type::jai_char_type: case script_value_type::jai_string_type: case script_value_type::jai_utf8_string_type:
			case script_value_type::jai_shared_ptr_type: case script_value_type::jai_weak_ptr_type: case script_value_type::jai_function_type:
			case script_value_type::jai_vec2_type: case script_value_type::jai_vec3_type: case script_value_type::jai_ivec2_type:
			case script_value_type::jai_ivec3_type: case script_value_type::jai_vec4_type: case script_value_type::jai_color_type:
				return false;
			case script_value_type::jai_array_type: return type_runs_destructor(t->element_type());
			case script_value_type::jai_map_type: return type_runs_destructor(t->key_type()) || type_runs_destructor(t->value_type());
			case script_value_type::jai_object_type: {
				if (t->type_name.empty()) { return true; }
				auto cls = engine_access::get_class_registry(*eng).find_script_class(std::string(t->type_name));
				return cls && cls->value_tree_copies_observably();   // a copy that runs a copy constructor is as observable as a death
			}
			default:
				return true;
			}
		}
		void note_death(const type_info_ptr& t) { if (proof_mode && type_runs_destructor(t)) { root_escaped = true; } }
		// A store replaces the target's value: the old one dies
		void note_replaced(const expression* target) {
			if (!proof_mode || !target) { return; }
			if (target->get_type() == node_type::identifier_expr) {
				auto* n = static_cast<const identifier_expr*>(target);
				if (is_element_symbol(n->symbol_id)) { note_death(root_type); return; }
				if (const local_info* l = n->global_qualified ? nullptr : local(n->symbol_id)) {
					if (!l->primitive) { note_death(l->type); }
					return;
				}
				return;   // enclosing state: the walk rejects the store itself
			}
			note_death(proof_chain_type(target));
		}

		admission_builder(engine& e) : eng(&e), sym(engine_access::get_symbolizer(e)) { adm.admitted = false; }

		static bool primitive_or_string(const type_info_ptr& t) {
			if (!t) { return false; }
			switch (t->base_type) {
			case script_value_type::jai_int_type: case script_value_type::jai_float_type:
			case script_value_type::jai_bool_type: case script_value_type::jai_char_type:
			case script_value_type::jai_string_type:
				return true;
			default:
				return false;
			}
		}
		// A reference's target type (a parameter's type may carry the reference or name its target)
		static type_info_ptr referent_type(const type_info_ptr& t) {
			return t && t->is_reference() ? t->element_type() : t;
		}
		// A type that fixes what its slot holds: no var, no auto, no dynamic twin of a var slot
		static bool declared_type(const type_info_ptr& t) {
			return t && t->base_type != script_value_type::jai_any_type && t->base_type != script_value_type::jai_null_type &&
			       !t->dynamic_slot && t->type_name != "auto";
		}
		bool class_has_subclass(const std::string& name) {
			auto cls = engine_access::get_class_registry(*eng).find_script_class(name);
			if (!cls) { return true; }
			for (const auto& entry : engine_access::get_class_registry(*eng).script_classes_) {
				const class_definition* other = entry.second.get();
				if (other && other != cls.get() && other->derives_from(cls.get())) { return true; }
			}
			return false;
		}
		// Static type of a member/subscript chain rooted at the proof root, a declared local or a typed global (null =
		// unknown or a step that can run user code: a user accessor, a field no class declares). declared: the type every
		// value the chain can name has, for a conversion judged ahead of time - a reference's declared target, and a global
		// only where its stamp is fixed for its life (a container's element types; a class with no subclass)
		type_info_ptr proof_chain_type(const expression* e, bool declared = false) {
			if (!e) { return nullptr; }
			if (e->get_type() == node_type::identifier_expr) {
				auto* n = static_cast<const identifier_expr*>(e);
				if (is_element_symbol(n->symbol_id)) { return root_type; }
				if (!n->global_qualified) {
					if (const local_info* l = local(n->symbol_id)) {
						if (!declared) { return l->type; }
						const type_info_ptr t = l->reference ? l->referent : l->type;
						return declared_type(t) ? t : nullptr;
					}
					if (self_field(n->symbol_id)) { return nullptr; }
				}
				auto global = engine_access::get_global_environment(*eng)->get(n->symbol_id);
				if (!global) { return nullptr; }
				type_info_ptr t = global.value().get_type_info();
				if (!t || t->base_type == script_value_type::jai_any_type || t->dynamic_slot) { return nullptr; }   // a var global holds anything next
				if (declared && (!declared_type(t) || (class_type(t) && class_has_subclass(t->type_name)))) { return nullptr; }
				return t;
			}
			if (e->get_type() == node_type::member_expr) {
				auto* m = const_cast<member_expr*>(static_cast<const member_expr*>(e));
				type_info_ptr base = proof_chain_type(m->object.get(), declared);
				if (!base || base->base_type != script_value_type::jai_object_type || base->type_name.empty()) { return nullptr; }
				auto cls = engine_access::get_class_registry(*eng).find_script_class(std::string(base->type_name));
				if (!cls) { return nullptr; }
				if (m->member_id == UINT64_MAX) { m->member_id = sym->intern(m->member); }
				if (read_runs_accessor(cls.get(), m->member_id)) { return nullptr; }
				if (!cls->find_field_declaring_class(m->member_id)) { return nullptr; }
				const type_info_ptr field = cls->get_field_declared_type(m->member_id);
				return declared && !declared_type(field) ? nullptr : field;
			}
			if (e->get_type() == node_type::binary_expr) {
				auto* b = static_cast<const binary_expr*>(e);
				if (b->op.type != token_type::left_bracket) { return nullptr; }
				type_info_ptr base = proof_chain_type(b->left.get(), declared);
				if (!base) { return nullptr; }
				if (base->base_type == script_value_type::jai_array_type) { const type_info_ptr t = base->element_type(); return declared && !declared_type(t) ? nullptr : t; }
				if (base->base_type == script_value_type::jai_map_type) { const type_info_ptr t = base->value_type(); return declared && !declared_type(t) ? nullptr : t; }
				if (base->base_type == script_value_type::jai_string_type) { return base; }   // a char: no identity, stands in as its string type
				return nullptr;
			}
			return nullptr;
		}
		// Proof mode: a chain rooted at the parameter, read as a VALUE. Index expressions walk
		// under the normal rules; the terminal must be statically primitive or string
		bool proof_chain_value(const expression_ptr& e) {
			if (!walk_chain_indexes(e.get())) { return false; }
			if (!primitive_or_string(proof_chain_type(e.get()))) { root_escaped = true; }
			return true;
		}
		bool walk_chain_indexes(const expression* e) {
			while (e) {
				if (e->get_type() == node_type::member_expr) { e = static_cast<const member_expr*>(e)->object.get(); continue; }
				if (e->get_type() == node_type::binary_expr) {
					auto* b = static_cast<const binary_expr*>(e);
					if (!walk_expr(b->right)) { return false; }
					e = b->left.get();
					continue;
				}
				break;
			}
			return true;
		}

		// A member read through a receiver declared cls runs a user getter when cls or any class derived from it
		// declares one (the instance may be a subclass)
		bool accessor_in_hierarchy(const class_definition* cls, uint64_t accessor_id) {
			if (cls->user_accessor(accessor_id) != nullptr) { return true; }
			for (const auto& entry : engine_access::get_class_registry(*eng).script_classes_) {
				const class_definition* other = entry.second.get();
				if (other && other != cls && other->derives_from(cls) && other->user_accessor(accessor_id) != nullptr) { return true; }
			}
			return false;
		}
		bool read_runs_accessor(const class_definition* cls, uint64_t member_id) {
			return accessor_in_hierarchy(cls, sym->get_getter_id_with_view(member_id).first);
		}
		// A store (and, for a compound, the read) through a member of a statically typed receiver: the receiver chain,
		// the accessors of every class the instance may be, and the field's conversion
		bool member_store_may_run_code(member_expr* m, const expression_ptr& value, bool reads) {
			type_info_ptr base = proof_chain_type(m->object.get());
			if (!class_type(base)) { return true; }
			auto cls = engine_access::get_class_registry(*eng).find_script_class(std::string(base->type_name));
			if (!cls) { return true; }
			if (m->member_id == UINT64_MAX) { m->member_id = sym->intern(m->member); }
			if (reads && read_runs_accessor(cls.get(), m->member_id)) { return true; }
			if (accessor_in_hierarchy(cls.get(), sym->get_setter_id_with_view(m->member_id).first)) { return true; }
			if (!cls->find_field_declaring_class(m->member_id)) { return true; }
			return conversion_may_run_code(cls->get_field_declared_type(m->member_id), value);
		}

		// Proof mode has no worker walls, so every place user code can run on a value the walk cannot see through
		// escapes the root: an operator, a truthiness test, a conversion or a method on a value that may be a class
		// instance. maybe_object is false only where the static kind is a primitive or a string.
		type_info_ptr return_type;   // the walked function's declared return type

		static bool class_type(const type_info_ptr& t) {
			return t && t->base_type == script_value_type::jai_object_type && !t->type_name.empty();
		}
		// The class every value of e is (that class or a subclass, which a slot of the class takes without running code), or
		// empty: a name, a reference, or a member/subscript chain from any root with declared steps
		std::string_view static_class(const expression_ptr& e) {
			type_info_ptr t;
			if (e && e->get_type() == node_type::identifier_expr) {
				auto* n = static_cast<const identifier_expr*>(e.get());
				if (is_element_symbol(n->symbol_id)) { t = root_type; }
				else if (const local_info* l = n->global_qualified ? nullptr : local(n->symbol_id)) { t = l->reference ? l->referent : l->type; }
				else { t = proof_chain_type(e.get(), true); }
			} else if (e && chain_root_is_element(e)) {
				t = proof_chain_type(e.get());
			} else if (e && (e->get_type() == node_type::member_expr ||
			                 (e->get_type() == node_type::binary_expr && static_cast<const binary_expr*>(e.get())->op.type == token_type::left_bracket))) {
				t = proof_chain_type(e.get(), true);
			}
			return class_type(t) && declared_type(t) ? std::string_view(t->type_name) : std::string_view();
		}
		static bool primitive_result_method(std::string_view name) {
			static constexpr std::string_view k_primitive[] = {
				"size", "empty", "length", "has", "contains", "index_of", "find", "rfind", "find_first_of", "find_last_of",
				"find_first_not_of", "find_last_not_of", "starts_with", "ends_with", "count", "to_int",
			};
			for (auto n : k_primitive) { if (name == n) { return true; } }
			return false;
		}
		// Builtins that call back into script (a predicate, a comparator, an element's operator< or to_string)
		static bool builtin_runs_script(std::string_view name, const type_info_ptr& receiver) {
			if (name == "sort" || name == "filter" || name == "remove_if" || name == "join") { return true; }
			return receiver && receiver->base_type == script_value_type::jai_array_type &&
			       (name == "map" || name == "reduce" || name == "for_each" || name == "find" || name == "find_index" || name == "some" || name == "every");
		}
		static bool builtin_container(const type_info_ptr& t) {
			return t && (t->base_type == script_value_type::jai_array_type || t->base_type == script_value_type::jai_map_type ||
			             t->base_type == script_value_type::jai_string_type);
		}
		bool builtin_method_call(const call_expr* call) {
			if (!call->callee || call->callee->get_type() != node_type::member_expr) { return false; }
			auto* m = static_cast<const member_expr*>(call->callee.get());
			if (m->is_static) { return false; }
			const type_info_ptr receiver = proof_chain_type(m->object.get());
			return !builtin_runs_script(m->member, receiver) && builtin_container(receiver);
		}
		bool maybe_object(const expression_ptr& e) {
			if (!e) { return false; }
			switch (e->get_type()) {
			case node_type::literal_expr:
				return false;
			case node_type::identifier_expr: {
				auto* n = static_cast<const identifier_expr*>(e.get());
				if (is_element_symbol(n->symbol_id)) { return true; }
				if (!n->global_qualified) {
					if (const local_info* l = local(n->symbol_id)) { return !l->primitive; }
					if (self_field(n->symbol_id)) { return true; }
				}
				auto global = engine_access::get_global_environment(*eng)->get(n->symbol_id);
				return !(global && primitive_or_string(global.value().get_type_info()));
			}
			case node_type::member_expr:
				return static_cast<const member_expr*>(e.get())->is_static || !primitive_or_string(proof_chain_type(e.get()));
			case node_type::binary_expr: {
				auto* b = static_cast<const binary_expr*>(e.get());
				if (b->op.type == token_type::left_bracket) { return !primitive_or_string(proof_chain_type(e.get())); }
				return maybe_object(b->left) || maybe_object(b->right);
			}
			case node_type::unary_expr:
				return maybe_object(static_cast<const unary_expr*>(e.get())->operand);
			case node_type::ternary_expr: {
				auto* t = static_cast<const ternary_expr*>(e.get());
				return maybe_object(t->then_expression) || maybe_object(t->else_expression);
			}
			case node_type::cast_expr:
				return !primitive_or_string(static_cast<const cast_expr*>(e.get())->target);
			case node_type::assignment_expr: {
				auto* a = static_cast<const assignment_expr*>(e.get());
				if (a->target && a->target->get_type() == node_type::identifier_expr) {
					auto* n = static_cast<const identifier_expr*>(a->target.get());
					if (const local_info* l = n->global_qualified ? nullptr : local(n->symbol_id); l && l->primitive) { return false; }
				}
				return maybe_object(a->target) || maybe_object(a->value);
			}
			case node_type::call_expr: {
				auto* call = static_cast<const call_expr*>(e.get());
				if (call->callee && call->callee->get_type() == node_type::member_expr) {
					auto* m = static_cast<const member_expr*>(call->callee.get());
					if (m->is_static) { return jai::detail::math_intrinsic_for_call(m) == jai::detail::math_fn::none; }
					return !(builtin_method_call(call) && primitive_result_method(m->member));
				}
				if (call->callee && call->callee->get_type() == node_type::identifier_expr) {
					auto* callee = static_cast<const identifier_expr*>(call->callee.get());
					if (primitive_ctor_name(callee->name)) { return false; }
					auto global = engine_access::get_global_environment(*eng)->get(callee->symbol_id);
					if (global && global.value().is_function()) {
						if (const script_callable* payload = payload_from_function_value(global.value())) {
							return !(payload->fn && primitive_or_string(payload->fn->return_type));
						}
					}
				}
				return true;
			}
			default:
				return true;
			}
		}
		// A conversion into a slot declared t runs user code: a primitive slot takes an instance through its to_int() /
		// to_float() / to_string() / to_bool(), a class slot takes anything but its own class through a converting
		// constructor or operator=
		bool conversion_may_run_code(const type_info_ptr& t, const expression_ptr& value) {
			if (!t || t->base_type == script_value_type::jai_any_type) { return false; }
			if (primitive_or_string(t)) { return maybe_object(value); }
			if (class_type(t)) {
				if (value && value->get_type() == node_type::literal_expr && static_cast<const literal_expr*>(value.get())->value.is_null()) { return false; }
				return static_class(value) != std::string_view(t->type_name);
			}
			return false;
		}
		// A slot declared a class takes another value through its converting constructor: the construction t(value), admitted with
		// the body's others (a conversion into a slot the walk cannot type, unadmitted, is refused when it runs)
		void note_converting_construction(const type_info_ptr& t, const expression_ptr& value, const ast_node* site) {
			if (proof_mode || !value || !class_type(t) || !conversion_may_run_code(t, value)) { return; }
			const uint64_t name_id = sym->intern(t->type_name);
			const std::shared_ptr<class_definition> cls = constructor_class(name_id);
			if (cls && has_true_single_param_conversion_ctor(cls)) { adm.constructions.push_back({ name_id, sym->get_string(name_id), 1, site }); }
		}
		// The declared type of a store's target: a field of the walked method's object by its bare name or this.name, else the chain's
		type_info_ptr store_slot_type(expression* target) {
			uint64_t field = UINT64_MAX;
			if (method_class && target && target->get_type() == node_type::identifier_expr) {
				auto* n = static_cast<identifier_expr*>(target);
				if (!n->global_qualified && !local(n->symbol_id) && self_field(n->symbol_id)) { field = n->symbol_id; }
			} else if (method_class && target && target->get_type() == node_type::member_expr) {
				auto* m = static_cast<member_expr*>(target);
				if (m->object && m->object->get_type() == node_type::this_expr) {
					if (m->member_id == UINT64_MAX) { m->member_id = sym->intern(m->member); }
					if (self_field(m->member_id)) { field = m->member_id; }
				}
			}
			return field != UINT64_MAX ? method_class->get_field_declared_type(field) : proof_chain_type(target);
		}
		// A plain store converts its value into the target's declared type; a member of a receiver the walk cannot type (an element's)
		// may be the field of that name of any class
		void note_store_conversions(expression* target, const expression_ptr& value, const ast_node* site) {
			if (proof_mode) { return; }
			if (const type_info_ptr t = store_slot_type(target)) {
				note_converting_construction(t, value, site);
				return;
			}
			if (!target || target->get_type() != node_type::member_expr) { return; }
			auto* m = static_cast<member_expr*>(target);
			if (m->member_id == UINT64_MAX) { m->member_id = sym->intern(m->member); }
			for (const auto& entry : engine_access::get_class_registry(*eng).script_classes_) {
				if (entry.second && entry.second->find_field_declaring_class(m->member_id) == entry.second.get()) {
					note_converting_construction(entry.second->get_field_declared_type(m->member_id), value, site);
				}
			}
		}
		// The by-value parameters of the declarations a call's argument count reaches convert its arguments
		void note_argument_conversions(const std::vector<std::shared_ptr<function_decl>>& decls, const std::vector<expression_ptr>& args) {
			for (const auto& decl : decls) {
				if (!decl || !arity_accepts(decl->parameters, args.size())) { continue; }
				for (size_t i = 0; i < args.size() && i < decl->parameters.size(); ++i) {
					if (!decl->parameters[i].is_reference) { note_converting_construction(decl->parameters[i].type, args[i], args[i].get()); }
				}
			}
		}
		// The script class a receiver is declared (this: the class whose method the walk is in), null when the walk cannot type it
		const class_definition* receiver_class(const expression* receiver) {
			if (receiver && receiver->get_type() == node_type::this_expr) { return method_class; }
			const type_info_ptr t = proof_chain_type(receiver);
			return class_type(t) ? engine_access::get_class_registry(*eng).find_script_class(std::string(t->type_name)).get() : nullptr;
		}
		// A method call on a receiver declared cls reaches the overloads of the name its chain and its subclasses declare; on a
		// receiver the walk cannot type (an element: its class is the barrier's), those of every class
		void note_method_argument_conversions(const class_definition* cls, uint64_t name_id, const call_expr* call) {
			if (proof_mode || call->arguments.empty()) { return; }
			for (const auto& entry : engine_access::get_class_registry(*eng).script_classes_) {
				const class_definition* level = entry.second.get();
				if (!level || (cls && level != cls && !level->derives_from(cls) && !cls->derives_from(level))) { continue; }
				const auto& table = level->script_method_overloads();
				if (const auto found = table.find(name_id); found != table.end()) { note_argument_conversions(found->second, call->arguments); }
			}
		}
		// A construction's arguments convert into the parameters of the constructors its argument count reaches
		void note_constructor_argument_conversions(uint64_t name_id, const std::vector<expression_ptr>& args) {
			if (proof_mode || args.empty()) { return; }
			const std::shared_ptr<class_definition> cls = constructor_class(name_id);
			if (cls && cls->is_script_class()) { note_argument_conversions(std::static_pointer_cast<script_class_definition>(cls)->get_constructor_asts(), args); }
		}
		// A plain store into a local: the slot's declared type converts, and a slot that may hold an instance runs its
		// class's operator= for anything but that class
		bool local_store_may_run_code(const local_info& l, const expression_ptr& value) {
			if (l.primitive) { return maybe_object(value); }
			if (value && value->get_type() == node_type::literal_expr && static_cast<const literal_expr*>(value.get())->value.is_null()) { return false; }
			if (class_type(l.type)) { return static_class(value) != std::string_view(l.type->type_name); }
			return true;
		}
		// A store (compound: a read, the operator, then the store) into a target
		bool store_may_run_code(const expression_ptr& target, const expression_ptr& value, bool compound) {
			if (!target) { return false; }
			if (compound && (maybe_object(target) || maybe_object(value))) { return true; }
			switch (target->get_type()) {
			case node_type::identifier_expr: {
				auto* n = static_cast<const identifier_expr*>(target.get());
				if (is_element_symbol(n->symbol_id)) { return !compound && local_store_may_run_code(typed_local(root_type), value); }
				if (const local_info* l = n->global_qualified ? nullptr : local(n->symbol_id)) { return !compound && local_store_may_run_code(*l, value); }
				return false;   // enclosing state: the walk rejects the store
			}
			case node_type::member_expr:
				return member_store_may_run_code(static_cast<member_expr*>(target.get()), value, compound);
			case node_type::binary_expr: {
				auto* b = static_cast<const binary_expr*>(target.get());
				if (b->op.type != token_type::left_bracket || !builtin_container(proof_chain_type(b->left.get()))) { return true; }
				return maybe_object(b->right) || maybe_object(value);
			}
			default:
				return true;
			}
		}

		// The identifier a member/subscript chain roots at (null: a call result, this, a literal)
		static identifier_expr* chain_root_identifier(const expression* e) {
			while (e) {
				if (e->get_type() == node_type::member_expr) {
					auto* m = static_cast<const member_expr*>(e);
					if (m->is_static) { return nullptr; }
					e = m->object.get();
					continue;
				}
				if (e->get_type() == node_type::binary_expr && static_cast<const binary_expr*>(e)->op.type == token_type::left_bracket) {
					e = static_cast<const binary_expr*>(e)->left.get();
					continue;
				}
				break;
			}
			return e && e->get_type() == node_type::identifier_expr ? static_cast<identifier_expr*>(const_cast<expression*>(e)) : nullptr;
		}
		// A member/subscript chain rooted at program state (a name neither a local nor the walked method's field)
		bool global_rooted(const expression* e) const {
			const identifier_expr* root = chain_root_identifier(e);
			return root && (root->global_qualified || (!is_local(root->symbol_id) && !self_field(root->symbol_id)));
		}
		// A storage kind whose value is its own (no handle): a store into it lands in the holder's value tree
		static bool by_value_kind(const type_info_ptr& t) {
			if (!t) { return false; }
			switch (t->base_type) {
			case script_value_type::jai_int_type: case script_value_type::jai_float_type:
			case script_value_type::jai_bool_type: case script_value_type::jai_char_type:
			case script_value_type::jai_string_type:
			case script_value_type::jai_array_type: case script_value_type::jai_map_type:
				return true;
			case script_value_type::jai_object_type:
				return !t->type_name.empty();
			default:
				return false;
			}
		}
		// Every step of a member/subscript chain from its root down to e (e itself when self) is statically by value: a write
		// there lands in the root's own value tree, never behind a handle
		bool chain_by_value(const expression* e, bool self) {
			if (self && !by_value_kind(proof_chain_type(e))) { return false; }
			while (e && e->get_type() != node_type::identifier_expr) {
				const expression* base = nullptr;
				if (e->get_type() == node_type::member_expr && !static_cast<const member_expr*>(e)->is_static) { base = static_cast<const member_expr*>(e)->object.get(); }
				else if (e->get_type() == node_type::binary_expr && static_cast<const binary_expr*>(e)->op.type == token_type::left_bracket) { base = static_cast<const binary_expr*>(e)->left.get(); }
				else { return false; }
				if (!by_value_kind(proof_chain_type(base))) { return false; }
				e = base;
			}
			return e != nullptr;
		}
		// Local proof: a write at `target` (replace: a store that replaces a bare local's slot; otherwise the write lands in
		// the target's value tree - a member/subscript store, a mutating builtin's receiver, a writing reference argument)
		// Parameter proof: a reference of the walked function may name a part of the caller's argument (f(a, a.hp)), so a
		// write through one escapes the parameter whenever the reference's type can occur in the parameter's value tree
		bool param_ref_aliases = false;
		bool types_meet(const type_info_ptr& a, const type_info_ptr& b) {
			if (a.get() == b.get()) { return true; }
			if (class_type(a) && class_type(b)) {
				if (a->type_name == b->type_name) { return true; }
				auto ca = engine_access::get_class_registry(*eng).find_script_class(std::string(a->type_name));
				auto cb = engine_access::get_class_registry(*eng).find_script_class(std::string(b->type_name));
				return !ca || !cb || ca->derives_from(cb.get()) || cb->derives_from(ca.get());
			}
			return a->canonical_name() == b->canonical_name();
		}
		bool type_may_hold(const type_info_ptr& tree, const type_info_ptr& t, std::vector<const class_definition*>& seen) {
			if (!t || !tree || t->base_type == script_value_type::jai_any_type) { return true; }
			if (types_meet(tree, t)) { return true; }
			switch (tree->base_type) {
			case script_value_type::jai_array_type: return type_may_hold(tree->element_type(), t, seen);
			case script_value_type::jai_map_type: return type_may_hold(tree->key_type(), t, seen) || type_may_hold(tree->value_type(), t, seen);
			case script_value_type::jai_object_type: {
				if (tree->type_name.empty()) { return true; }
				auto cls = engine_access::get_class_registry(*eng).find_script_class(std::string(tree->type_name));
				if (!cls) { return true; }
				auto fields_hold = [&](const class_definition* c) {
					if (std::find(seen.begin(), seen.end(), c) != seen.end()) { return false; }
					seen.push_back(c);
					const uint32_t n = c->field_slot_count();
					for (uint32_t slot = 0; slot < n; ++slot) {
						if (type_may_hold(c->get_field_declared_type(c->slot_field(slot)), t, seen)) { return true; }
					}
					return false;
				};
				if (fields_hold(cls.get())) { return true; }
				for (const auto& entry : engine_access::get_class_registry(*eng).script_classes_) {   // a by-value slot may hold a subclass
					const class_definition* other = entry.second.get();
					if (other && other != cls.get() && other->derives_from(cls.get()) && fields_hold(other)) { return true; }
				}
				return false;
			}
			case script_value_type::jai_any_type:
				return true;
			default:
				return false;   // a primitive meets only itself; a handle's pointee is shared by the copy too
			}
		}
		void check_param_write(const expression* target) {
			if (fn_depth != 1 || !param_ref_aliases) { return; }   // a callee's reference parameter defers to its call sites
			identifier_expr* root = chain_root_identifier(target);
			if (!root || is_element_symbol(root->symbol_id)) { return; }
			const local_info* l = root->global_qualified ? nullptr : local(root->symbol_id);
			if (!l || !l->reference) { return; }
			type_info_ptr referent = l->type;
			if (referent && referent->is_reference()) { referent = referent->element_type(); }
			std::vector<const class_definition*> seen;
			if (type_may_hold(root_type, referent, seen)) { root_escaped = true; }
		}
		void check_local_write(const expression* target, bool replace) {
			if (!proof_mode) { return; }
			if (!local_mode) { check_param_write(target); return; }
			identifier_expr* root = chain_root_identifier(target);
			if (!root) { root_escaped = true; return; }
			const bool bare = target == root;
			if (is_element_symbol(root->symbol_id)) {
				if (bare) { root_escaped = true; }   // the declaration's own slot: a store may assign into the referent
				return;   // through it: the root rules (element_mutated, escapes)
			}
			const local_info* l = root->global_qualified ? nullptr : local(root->symbol_id);
			if (!l) { return; }   // enclosing state: the walk rejects the write itself
			if (l->reference) {
				if (fn_depth > 0 && l->param) { return; }   // a callee's reference parameter: its call sites judge what they pass
				root_escaped = true;
				return;
			}
			if (l->primitive) { return; }   // a primitive or string slot holds no shared tree
			if (fn_depth == 0 && root->symbol_id == source_root) { root_escaped = true; return; }
			if (bare && replace) { return; }
			if (!chain_by_value(target, !replace)) { root_escaped = true; }   // a replaced place may hold a handle, an in-place one may not
		}

		// Captured-read touch collection (the CHEAP half of the two-layer design: the
		// walk only records which enclosing names the body touches and whether every
		// touch is a subscript read; enforcement of no-writes is the write wall plus
		// the statically-free rejections below). subscript_root keeps the name eligible
		// for the borrow (raw-read) tier; any other read demotes it to snapshot.
		void touch_capture(identifier_expr* n, bool subscript_root, const expression* index = nullptr) {
			auto [it, inserted] = capture_index.try_emplace(n->symbol_id, adm.captures.size());
			if (inserted) {
				adm.captures.push_back({ n->symbol_id, n->name, subscript_root });   // name view = symbolizer storage (permanent)
			} else if (!subscript_root) {
				adm.captures[it->second].borrow_eligible = false;
			}
			capture_touches.push_back({ n->symbol_id, loop_variable_position(index), n });
		}
		// A use of the captured value itself: a captured handle refuses it (a worker reads its own copy of the object)
		void note_bare_capture(identifier_expr* n) {
			auto& entry = adm.captures[capture_index[n->symbol_id]];
			if (!entry.bare_use) { entry.bare_use = n; }
		}
		// A method on a chain rooted at a captured name: pinned like an element's, judged at the barrier against the value
		void note_captured_method_call(identifier_expr* root, const member_expr* m, const call_expr* call) {
			adm.captured_method_calls.push_back({ root->symbol_id, m->member_id, m->member, m });
			collect_element_method(m->member_id, m->member, m, call);
			note_method_argument_conversions(nullptr, m->member_id, call);
		}

		// Every touch of a captured name, with the loop variable that indexes it (SIZE_MAX: not indexed by one): an index-written
		// name may be touched as out[i] alone
		struct capture_touch { uint64_t name_id; size_t index_param; const ast_node* site; };
		std::vector<capture_touch> capture_touches;
		std::vector<uint64_t> written_loop_variables;

		// The position of the body-level loop variable an index expression names, SIZE_MAX for anything else
		size_t loop_variable_position(const expression* index) const {
			if (!index || index->get_type() != node_type::identifier_expr || !inplace_root || fn_depth != 1 || method_class) { return SIZE_MAX; }   // the body itself, not a callee or a method
			const uint64_t id = static_cast<const identifier_expr*>(index)->symbol_id;
			if (id == element_symbol) { return 0; }
			for (size_t k = 0; k < element_symbols.size(); ++k) { if (element_symbols[k] == id) { return k + 1; } }
			return SIZE_MAX;
		}

		void note_loop_variable_write(const identifier_expr* n) {
			written_loop_variables.push_back(n->symbol_id);
			if (n->symbol_id == element_symbol && !adm.loop_variable_write) { adm.loop_variable_write = n; }
		}
		// A loop variable handed to a reference parameter that writes it is written
		void note_loop_variable_reference_argument(const expression_ptr& arg) {
			if (loop_variable_position(arg.get()) != SIZE_MAX) { note_loop_variable_write(static_cast<const identifier_expr*>(arg.get())); }
		}

		// After the walk: an index-written name is touched as out[i] alone, with one loop variable the body never writes
		bool check_index_writes() {
			for (const auto& w : adm.index_writes) {
				const uint64_t loop_id = w.index_param == 0 ? element_symbol : element_symbols[w.index_param - 1];
				if (std::find(written_loop_variables.begin(), written_loop_variables.end(), loop_id) != written_loop_variables.end()) {
					return fail(w.site, "body writes enclosing state '" + std::string(w.name) + "' at an index the body changes");
				}
				for (const auto& t : capture_touches) {
					if (t.name_id == w.name_id && t.index_param != w.index_param) {
						return fail(t.site, "body touches '" + std::string(w.name) + "' beyond the slot it writes, " + std::string(w.name) + "[" +
						                    std::string(sym->get_string(loop_id)) + "]");
					}
				}
			}
			return true;
		}

		// Set while walking a script-class method body at the barrier: bare names that are
		// the class's own fields root at the ELEMENT (chunk-exclusive — reads AND writes
		// admitted, exactly as the same access spelled inline in the loop body would be).
		const class_definition* method_class = nullptr;

		bool self_field(uint64_t id) const {
			return method_class && method_class->find_field_declaring_class(id) != nullptr;
		}

		void collect_element_method(uint64_t name_id, std::string_view name, const ast_node* site, const call_expr* call) {
			uint64_t enclosing = 0;
			const ast_node* enclosing_site = nullptr;
			std::vector<std::pair<size_t, size_t>> loop_variables;
			for (size_t i = 0; i < call->arguments.size() && i < 64; ++i) {
				if (identifier_expr* root = enclosing_root(call->arguments[i])) {
					enclosing |= uint64_t(1) << i;
					if (!enclosing_site) { enclosing_site = root; }
				}
				if (const size_t position = loop_variable_position(call->arguments[i].get()); position != SIZE_MAX) { loop_variables.emplace_back(i, position); }
			}
			for (auto& rec : adm.element_method_calls) {
				if (rec.name_id != name_id) { continue; }
				if (!rec.enclosing_site) { rec.enclosing_site = enclosing_site; }
				rec.enclosing_arguments |= enclosing;
				rec.loop_variable_arguments.insert(rec.loop_variable_arguments.end(), loop_variables.begin(), loop_variables.end());
				return;
			}
			adm.element_method_calls.push_back({ name_id, name, site, enclosing, enclosing_site, std::move(loop_variables) });
		}

		// In-place mode member access (read or store position): admitted iff the chain
		// roots at a LOCAL identifier - the owned element or a worker-private local.
		// Index expressions inside the chain walk under the normal rules. Every member
		// node's interned ids (member/getter/setter) warm HERE, single-threaded: worker
		// field access probes them against the frozen symbolizer.
		void note_reached_member(uint64_t id) {
			auto& v = adm.element_reached_members;
			if (std::find(v.begin(), v.end(), id) == v.end()) { v.push_back(id); }
		}
		// The element used as a VALUE (not the root of a member chain): every field is reachable
		void note_element_whole() { adm.element_reach_all = true; }
		bool chain_root_is_element(const expression_ptr& e) const {
			const expression* cur = e.get();
			while (cur) {
				if (cur->get_type() == node_type::member_expr) { cur = static_cast<const member_expr*>(cur)->object.get(); continue; }
				if (cur->get_type() == node_type::binary_expr && static_cast<const binary_expr*>(cur)->op.type == token_type::left_bracket) {
					cur = static_cast<const binary_expr*>(cur)->left.get(); continue;
				}
				break;
			}
			return cur && cur->get_type() == node_type::identifier_expr && is_element_symbol(static_cast<const identifier_expr*>(cur)->symbol_id);
		}
		// A chain terminal read as a value (copied: every handle below it) or replaced (the old
		// subtree released): its whole subtree is reachable, not just the members named below it
		void note_reached_whole(uint64_t id) {
			auto& v = adm.element_reached_whole;
			if (std::find(v.begin(), v.end(), id) == v.end()) { v.push_back(id); }
		}
		bool walk_local_member_chain(const member_expr* m, const char* what) {
			std::vector<const expression_ptr*> indexes;
			const expression* cur = m;
			identifier_expr* root = nullptr;
			bool rooted_at_self = false;
			while (cur) {
				if (cur->get_type() == node_type::identifier_expr) {
					root = const_cast<identifier_expr*>(static_cast<const identifier_expr*>(cur));
					break;
				}
				if (cur->get_type() == node_type::this_expr) {
					rooted_at_self = true;   // method-body chain on the element itself
					break;
				}
				if (cur->get_type() == node_type::member_expr) {
					auto* node = const_cast<member_expr*>(static_cast<const member_expr*>(cur));
					if (node->member_id == UINT64_MAX) { node->member_id = sym->intern(node->member); }
					note_reached_member(node->member_id);
					if (cur == m) { note_reached_whole(node->member_id); }
					node->getter_id = sym->get_getter_id_with_view(node->member_id).first;
					(void)sym->get_setter_id_with_view(node->member_id);
					cur = node->object.get();
					continue;
				}
				if (cur->get_type() == node_type::binary_expr) {
					auto* b = static_cast<const binary_expr*>(cur);
					if (b->op.type != token_type::left_bracket) { break; }
					indexes.push_back(&b->right);
					cur = b->left.get();
					continue;
				}
				break;
			}
			if (rooted_at_self && !method_class) {
				return fail(m, "'this' is not allowed in a parallel body");
			}
			if (!root && !rooted_at_self) {
				return fail(m, std::string(what) + " must root at a local variable in a parallel body");
			}
			if (root && !is_local(root->symbol_id) && !self_field(root->symbol_id)) {
				// a captured object reads through its fields; the barrier proves what it holds and gives each worker its own view
				if (proof_mode || std::string_view(what) != "member access") {
					return fail(m, std::string(what) + " reaches captured state '" + std::string(root->name) + "' (captured reads are read-only)");
				}
				touch_capture(root, false);
				auto& entry = adm.captures[capture_index[root->symbol_id]];
				if (!entry.member_use) { entry.member_use = m; }
			}
			if (proof_mode && root && !proof_chain_type(m)) { root_escaped = true; }   // an unknown receiver or a user accessor
			if (root) { note_const_ref_whole(root->symbol_id); }
			for (const expression_ptr* idx : indexes) {
				if (!walk_expr(*idx)) { return false; }
			}
			return true;
		}

		// The enclosing identifier at the root of a bare name or a [ ]-chain
		// (nullptr when the expression roots elsewhere or at a local)
		identifier_expr* enclosing_root(const expression_ptr& e) const {
			const expression* cur = e.get();
			while (cur) {
				if (cur->get_type() == node_type::identifier_expr) {
					auto* ident = static_cast<identifier_expr*>(const_cast<expression*>(cur));
					// Self fields are element-rooted, never enclosing state
					return (is_local(ident->symbol_id) || self_field(ident->symbol_id)) ? nullptr : ident;
				}
				if (cur->get_type() == node_type::binary_expr &&
				    static_cast<const binary_expr*>(cur)->op.type == token_type::left_bracket) {
					cur = static_cast<const binary_expr*>(cur)->left.get();
					continue;
				}
				return nullptr;
			}
			return nullptr;
		}

		bool fail(const ast_node* node, std::string reason) {
			if (!failed) {
				failed = true;
				adm.admitted = false;
				adm.code = make_error_code(runtime_error_code::unsupported_operation);
				adm.message = std::string(prefix) + ": " + std::move(reason) + context;
				if (node) {
					adm.message += " at " + std::to_string(node->location.line) + ":" +
					               std::to_string(node->location.column);
				}
			}
			return false;
		}

		void collect_type(type_info_ptr t) {
			if (t.get() && collected_types.insert(t.get()).second) {
				adm.referenced_types.push_back(t.get());
			}
		}

		void declare(uint64_t id, local_info info = {}) { scopes.back()[id] = std::move(info); }
		bool is_local(uint64_t id) const {
			for (const auto& s : scopes) {
				if (s.count(id)) { return true; }
			}
			return false;
		}
		const local_info* local(uint64_t id) const {
			for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
				auto found = it->find(id);
				if (found != it->end()) { return &found->second; }
			}
			return nullptr;
		}
		static local_info typed_local(const type_info_ptr& t) { return { t, primitive_or_string(t) }; }

		// A const T& parameter of a walked function: enclosing state passed to it is a captured read, and stays a subscript read
		// (the borrow tier) when the function only subscripts the parameter, directly or through further const T& parameters
		struct const_ref_use { const void* body = nullptr; size_t index = 0; bool whole = false; };
		std::vector<const_ref_use> const_ref_uses;
		// A parameter handed on to another const T& parameter is used whole when that one is; a captured name passed to one
		// stays a subscript read unless it is. Both settle once the walk is done (recursion reaches parameters mid-walk)
		std::vector<std::pair<size_t, size_t>> const_ref_forwards;   // (caller's use, callee's use)
		std::vector<std::pair<size_t, size_t>> const_ref_touches;    // (adm.captures slot, callee's use)
		size_t const_ref_use_index(const void* body, size_t index) const {
			for (size_t k = 0; k < const_ref_uses.size(); ++k) {
				if (const_ref_uses[k].body == body && const_ref_uses[k].index == index) { return k; }
			}
			return SIZE_MAX;
		}
		void finish_const_ref_touches() {
			for (bool changed = true; changed;) {
				changed = false;
				for (const auto& [caller, callee] : const_ref_forwards) {
					if (const_ref_uses[callee].whole && !const_ref_uses[caller].whole) { const_ref_uses[caller].whole = changed = true; }
				}
			}
			for (const auto& [capture, use] : const_ref_touches) {
				if (const_ref_uses[use].whole) { adm.captures[capture].borrow_eligible = false; }
			}
		}
		std::vector<const expression*> forwarded_arguments;   // const T& arguments whose touch walk_call recorded
		static bool borrow_count_call(const member_expr* m, const call_expr* call) {
			return call->arguments.empty() && jai::detail::parallel_borrow_count_method(m->member);
		}
		void note_const_ref_whole(uint64_t id) {
			if (const local_info* l = local(id); l && l->const_ref_use != SIZE_MAX) { const_ref_uses[l->const_ref_use].whole = true; }
		}
		bool const_ref_local(const expression* e) const {
			if (!e || e->get_type() != node_type::identifier_expr) { return false; }
			const local_info* l = local(static_cast<const identifier_expr*>(e)->symbol_id);
			return l && l->const_ref_use != SIZE_MAX;
		}
		// The refusal of enclosing state passed to a script callee's non-const reference parameter, with the fix when the
		// callee never writes through it
		std::string reference_argument_refusal(const identifier_expr* root, const parameter& p, std::string_view callee, bool callee_reads_only) {
			std::string result = "argument passes enclosing state '" + std::string(root->name) + "' to reference parameter '" + p.name +
			                     "' of '" + std::string(callee) + "'";
			if (callee_reads_only) {
				const type_info_ptr referent = referent_type(p.type);
				result += " (captured reads are read-only; '" + std::string(callee) + "' only reads it: declare it const " +
				          (referent ? referent->canonical_name() : std::string("var")) + "& " + p.name + ")";
			} else {
				result += ", which writes it (captured reads are read-only; pass a copy, or take it as const T& where it is only read)";
			}
			return result;
		}

		// Root-parameter contract for the region's entry body: transform requires a
		// by-value unary param; parallel_for takes either (auto& mutates in place,
		// plain auto is a side-effect-free worker copy)
		enum class root_param { none, unary_value, unary };

		bool admit_function(const std::shared_ptr<script_defined_function>& fn, uint64_t global_name_id,
		                    root_param param_mode, const ast_node* call_node) {
			if (!fn || !fn->body) {
				return fail(call_node, "fn has no body known at admission time");
			}
			if (param_mode != root_param::none) {
				if (fn->parameters().empty() || (param_mode == root_param::unary_value && fn->parameters().size() != 1)) {
					return fail(fn->body.get(), "fn must take exactly one parameter");
				}
				if (param_mode == root_param::unary_value && fn->parameters()[0].is_reference) {
					return fail(fn->body.get(), "fn may not take its element by reference (elements are detached copies)");
				}
			}
			// Collect the graph entry before the visited check so recursion still records it
			if (global_name_id != UINT64_MAX) {
				if (collected_fn_ids.insert(global_name_id).second) {
					adm.script_functions.emplace_back(global_name_id, fn);
				}
			} else {
				adm.script_functions.emplace_back(global_name_id, fn);
			}
			if (!visited_bodies.insert(fn->body.get()).second) {
				return true;   // already walked (recursion / shared target)
			}

			// Fresh scope stack per function, no class's fields in scope
			decltype(scopes) saved;
			saved.swap(scopes);
			scopes.emplace_back();
			const class_definition* saved_class = method_class;
			method_class = nullptr;
			const type_info_ptr saved_return = return_type;
			return_type = fn->return_type;
			++fn_depth;
			bool ok = true;
			for (const auto& p : fn->parameters()) {
				if (p.symbol_id == UINT64_MAX) { p.symbol_id = sym->intern(p.name); }   // warm (idempotent lazy fill)
				local_info info = typed_local(p.type);
				info.reference = p.is_reference;
				info.param = true;
				if (p.is_reference) { info.referent = referent_type(p.type); }
				if (!proof_mode && p.is_reference && p.is_const) {
					info.const_ref_use = const_ref_uses.size();
					const_ref_uses.push_back({ fn->body.get(), size_t(&p - fn->parameters().data()) });
				}
				declare(p.symbol_id, info);
				if (!p.is_reference && !is_element_symbol(p.symbol_id)) { note_death(p.type); }   // the parameter's copy dies with the call
				if (param_mode != root_param::none) {
					if (element_symbol == UINT64_MAX) { element_symbol = p.symbol_id; } else { element_symbols.push_back(p.symbol_id); }
				}
				collect_type(p.type);
				if (p.default_value && !walk_expr(p.default_value)) { ok = false; break; }
				if (proof_mode && p.default_value && conversion_may_run_code(p.type, p.default_value)) { root_escaped = true; }
				note_converting_construction(p.type, p.default_value, p.default_value.get());
			}
			if (ok) {
				collect_type(fn->return_type);
				for (const auto& d : fn->body->declarations) {
					if (!walk_stmt(d)) { ok = false; break; }
				}
			}
			--fn_depth;
			return_type = saved_return;
			method_class = saved_class;
			scopes.swap(saved);
			return ok;
		}

		// ===== Barrier-time class-method admission (parallel-method-admission rule) =====
		// A method on the chunk-exclusive element is exactly as safe as the same accesses
		// spelled inline in the loop body — the walk proves it: self fields read/write OK,
		// captured global reads collect as ordinary captures, everything shared-mutating
		// gets a verdict naming the violation. Admitted: single-overload non-coroutine
		// methods the element's class defines or inherits (the method its dispatch answers with).
		struct admitted_method {
			const class_definition* cls = nullptr;
			uint64_t name_id = 0;
			std::string_view name;   // symbolizer storage (permanent)
			std::shared_ptr<function_decl> decl;
		};
		std::vector<admitted_method> admitted_methods;
		std::unordered_set<const function_decl*> visited_methods;

		bool admit_class_method(const class_definition* cls, uint64_t name_id, std::string_view name, const ast_node* site) {
			// A method the barrier cannot pin is a verdict
			auto unpinnable = [&](std::string reason) { return fail(site, std::move(reason)); };
			auto decl = cls->single_method_overload_ptr(name_id);
			const class_definition* declaring = cls;
			if (!decl && !cls->has_method(name_id)) {
				// an inherited method: the base class whose method the receiver's dispatch answers with
				const script_value inherited = cls->get_method(name_id, false);
				const script_method_dispatch* dispatch = inherited.is_function() ? inherited.as_function().target<script_method_dispatch>() : nullptr;
				if (dispatch && dispatch->cls && dispatch->cls.get() != cls && dispatch->cls->is_script_class()) {
					declaring = dispatch->cls.get();
					decl = declaring->single_method_overload_ptr(name_id);
				}
			}
			if (!decl) {
				return unpinnable("method '" + std::string(name) + "' on class '" + cls->get_name() +
				                  "' is not admitted in a parallel body (only single-overload methods of the element's class or its bases)");
			}
			if (decl->is_coroutine) {
				return fail(site, "coroutine method '" + std::string(name) + "' is not allowed in a parallel body");
			}
			if (!decl->body) {
				return unpinnable("method '" + std::string(name) + "' has no body known at admission time");
			}
			// The pinned worker dispatch bypasses the getter-precedence probe and the
			// access-context check — the barrier proves both moot instead
			if (cls->find_field_declaring_class(name_id) != nullptr) {
				return unpinnable("'" + std::string(name) + "' names both a field and a method on class '" +
				                  cls->get_name() + "' — not admitted in a parallel body");
			}
			if (cls->defines_method(sym->get_getter_id_with_view(name_id).first)) {
				return unpinnable("method '" + std::string(name) + "' is shadowed by a property getter on class '" +
				                  cls->get_name() + "' — not admitted in a parallel body");
			}
			if (cls->chain_has_nonpublic()) {
				return unpinnable("class '" + cls->get_name() +
				                  "' has non-public members — its methods are not admitted in a parallel body");
			}
			// each receiver class gets its pin; a body shared by several (an inherited method) is walked once
			const bool pinned = std::any_of(admitted_methods.begin(), admitted_methods.end(),
			                                [&](const admitted_method& m) { return m.cls == cls && m.name_id == name_id; });
			if (!pinned) { admitted_methods.push_back({ cls, name_id, name, decl }); }
			if (!visited_methods.insert(decl.get()).second) { return true; }   // recursion / already admitted
			// Pre-mint the shared parameter storage (a mutable lazy cache on the AST
			// node): workers dispatching this method concurrently must only ever READ it
			if (!decl->shared_params_cache) {
				decl->shared_params_cache = std::make_shared<const std::vector<parameter>>(decl->parameters);
			}

			const class_definition* saved_class = method_class;
			method_class = declaring;
			const type_info_ptr saved_return = std::exchange(return_type, decl->return_type);
			decltype(scopes) saved;
			saved.swap(scopes);
			scopes.emplace_back();
			bool ok = true;
			for (const auto& p : decl->parameters) {
				if (p.symbol_id == UINT64_MAX) { p.symbol_id = sym->intern(p.name); }   // warm (idempotent lazy fill)
				declare(p.symbol_id, typed_local(p.type));
				collect_type(p.type);
				if (p.default_value && !walk_expr(p.default_value)) { ok = false; break; }
				note_converting_construction(p.type, p.default_value, p.default_value.get());
			}
			if (ok) {
				collect_type(decl->return_type);
				for (const auto& d : decl->body->declarations) {
					if (!walk_stmt(d)) { ok = false; break; }
				}
			}
			scopes.swap(saved);
			method_class = saved_class;
			return_type = saved_return;
			return ok;
		}

		// Enclosing state the body passes a method was walked as a captured read: a const T& parameter reads it, a non-const one
		// would write a worker's private copy
		bool admit_method_reference_arguments(const class_definition* cls, const parallel_admission::element_method_call& callrec) {
			for (const auto& m : admitted_methods) {
				if (m.cls != cls || m.name_id != callrec.name_id || !m.decl) { continue; }
				const auto& params = m.decl->parameters;
				for (size_t i = 0; i < params.size() && i < 64; ++i) {
					if (!((callrec.enclosing_arguments >> i) & 1) || !params[i].is_reference || params[i].is_const) { continue; }
					const auto* root = static_cast<const identifier_expr*>(callrec.enclosing_site);
					const bool reads_only = ref_parameter_takes_temporary(*eng, params, m.decl->body.get(), i, true);
					return fail(callrec.enclosing_site, reference_argument_refusal(root, params[i], cls->get_name() + "::" + std::string(callrec.name), reads_only));
				}
				for (const auto& [argument, position] : callrec.loop_variable_arguments) {
					if (argument < params.size() && params[argument].is_reference && !params[argument].is_const &&
					    !ref_parameter_takes_temporary(*eng, params, m.decl->body.get(), argument, true) && !admit_loop_variable_write(position, callrec.site)) {
						return false;
					}
				}
				return true;
			}
			return true;
		}
		// A method the barrier resolves writes a loop variable through a reference parameter: the body's own walk could not see it
		const parallel_admission* region_body = nullptr;
		std::vector<const class_definition*> captured_classes;   // the classes of the instances the region's captured objects hold
		std::vector<const class_definition*> object_classes;   // every class an object a chunk reaches can be (the operator pass's)
		std::string_view counted_variable;   // the counted form's variable (parallel_for_stmt::counted), empty otherwise
		bool admit_loop_variable_write(size_t position, const ast_node* site) {
			if (!region_body) { return true; }
			if (position == 0 && !counted_variable.empty()) { return fail(site, counted_variable_written(counted_variable)); }
			for (const auto& w : region_body->index_writes) {
				if (w.index_param == position) { return fail(w.site, "body writes enclosing state '" + std::string(w.name) + "' at an index the body changes"); }
			}
			return true;
		}

		// Barrier entry: resolve the body's collected element-method names against the
		// live element classes. Names no class defines are builtins — ignored (runtime
		// precedence is unchanged for them).
		bool admit_element_methods(const std::vector<const class_definition*>& classes) {
			// Index walk: admitting a method body collects ITS this.x() calls into the
			// same worklist (transitive admission), so the vector grows under us
			for (size_t i = 0; i < adm.element_method_calls.size(); ++i) {
				const auto callrec = adm.element_method_calls[i];   // copy: push_back may reallocate
				for (const class_definition* cls : classes) {
					if (!cls->defines_method(callrec.name_id) || !cls->is_script_class()) { continue; }
					if (!admit_class_method(cls, callrec.name_id, callrec.name, callrec.site)) {
						return false;
					}
					if ((callrec.enclosing_arguments || !callrec.loop_variable_arguments.empty()) && !admit_method_reference_arguments(cls, callrec)) { return false; }
				}
				if (!admit_host_methods(callrec.name_id, callrec)) { return false; }
			}
			return true;
		}

		// ===== Host methods =====
		// A bound C++ method runs on any thread unless it is marked serial_method(); a non-const one only over an object the
		// calling chunk made (call_parallel_host_pin). The receiver's class is a runtime fact, so every class whose chain binds
		// the name in C++ gets a pin; a serial method's pin raises its refusal when it is called.
		struct host_pin { const class_definition* cls; uint64_t name_id; const class_definition* owner; };
		std::vector<host_pin> host_pins;
		std::unordered_set<uint64_t> pinned_host_names;

		// The C++ level of cls's chain that binds name_id, unless a script level answers it first
		static const class_definition* host_method_owner(const class_definition* cls, uint64_t name_id) {
			if (!cls) { return nullptr; }
			if (cls->defines_method(name_id)) { return cls->is_script_class() ? nullptr : cls; }
			for (const auto& parent : cls->get_parent_classes()) {
				if (const class_definition* found = host_method_owner(parent.get(), name_id)) { return found; }
			}
			return nullptr;
		}
		// Every registered class with a C++ level: a host class, or a script class over one
		std::vector<const class_definition*> host_chain_classes;
		bool host_chain_classes_listed = false;
		template <typename Each>
		void for_each_registered_class(Each&& each) {
			if (!host_chain_classes_listed) {
				host_chain_classes_listed = true;
				for (const auto& entry : engine_access::get_class_registry(*eng).cpp_classes_) { if (entry.second) { host_chain_classes.push_back(entry.second.get()); } }
				for (const auto& entry : engine_access::get_class_registry(*eng).script_classes_) {
					if (entry.second && entry.second->find_cpp_base_class()) { host_chain_classes.push_back(entry.second.get()); }
				}
			}
			for (const class_definition* cls : host_chain_classes) { each(cls); }
		}
		const std::unordered_set<uint64_t>* host_names = nullptr;   // every name a C++ level binds (parallel_engine_state)
		bool admit_host_methods(uint64_t name_id, const parallel_admission::element_method_call& callrec) {
			if (host_names && !host_names->count(name_id)) { return true; }
			if (!pinned_host_names.insert(name_id).second) { return true; }
			bool ok = true;
			for_each_registered_class([&](const class_definition* cls) {
				const class_definition* owner = ok ? host_method_owner(cls, name_id) : nullptr;
				if (!owner) { return; }
				if (callrec.enclosing_arguments || !callrec.loop_variable_arguments.empty()) {
					const script_value method = owner->get_method(name_id, false);
					// the positions a non-const T& parameter writes back through: one callable's mask, or every overload's of a bound method
					uint64_t written = 0;
					if (const auto* writes = method.is_function() ? method.as_function().target<host_writeback_function>() : nullptr) {
						written = writes->mask;
					} else if (const auto* dispatch = method.is_function() ? method.as_function().target<class_definition::cpp_method_dispatch>() : nullptr) {
						for (size_t i = 0; i < 64; ++i) {
							if (dispatch->argument_binding(i, SIZE_MAX) & 4) { written |= uint64_t(1) << i; }
						}
					}
					if (written & callrec.enclosing_arguments) {
						const auto* root = static_cast<const identifier_expr*>(callrec.enclosing_site);
						ok = fail(callrec.enclosing_site, "argument passes enclosing state '" + std::string(root->name) +
						                                  "' by reference (captured reads are read-only)");
						return;
					}
					for (const auto& [argument, position] : callrec.loop_variable_arguments) {
						if (argument < 64 && ((written >> argument) & 1) && !admit_loop_variable_write(position, callrec.site)) {
							ok = false;
							return;
						}
					}
				}
				host_pins.push_back({ cls, name_id, owner });
			});
			return ok;
		}
		// The accessors behind the members the bodies name, for every class whose chain binds them in C++
		void admit_host_accessors(const std::vector<uint64_t>& member_ids) {
			for (uint64_t member_id : member_ids) {
				for (uint64_t accessor : { sym->get_getter_id_with_view(member_id).first, sym->get_setter_id_with_view(member_id).first }) {
					if (host_names && !host_names->count(accessor)) { continue; }
					if (!pinned_host_names.insert(accessor).second) { continue; }
					for_each_registered_class([&](const class_definition* cls) {
						if (const class_definition* owner = host_method_owner(cls, accessor)) { host_pins.push_back({ cls, accessor, owner }); }
					});
				}
			}
		}

		// ===== Barrier-time construction admission =====
		// A construction runs the constructor the call reaches (every one its argument count admits), the base constructions its
		// initializer list names (a base it does not name default-constructs), each level's field initializers and, when the
		// object dies on the chunk, the destructors of its chain. Each body walks as a method of the new object's class: the
		// object is the chunk's own, so its fields are writable, and everything else follows the body's rules.
		struct constructed_class { uint64_t name_id = 0; std::shared_ptr<class_definition> cls; };
		std::vector<constructed_class> constructed_classes;
		std::vector<std::shared_ptr<function_decl>> admitted_bodies;   // constructor and destructor bodies each worker precompiles
		std::vector<expression_ptr> admitted_expressions;   // field initializers and initializer-list arguments each worker precompiles
		std::vector<const class_definition*> destructor_levels;
		std::unordered_set<const function_decl*> visited_constructors;
		std::unordered_set<const class_definition*> visited_initializers;
		std::unordered_set<const class_definition*> visited_destructor_classes;
		std::string context;   // where construction code sits, appended to its verdict

		// The class a name constructs: a script class's constructor value in the program's scope, or a host class's constructor
		std::shared_ptr<class_definition> constructor_class(uint64_t name_id) {
			auto global = engine_access::get_global_environment(*eng)->get(name_id);
			if (!global || !global.value().is_function()) { return nullptr; }
			if (const script_callable* payload = payload_from_function_value(global.value())) {
				return payload->kind == script_callable::kind_type::constructor ? payload->cls() : nullptr;
			}
			const auto& host_classes = engine_access::get_class_registry(*eng).cpp_classes_;
			const auto found = host_classes.find(std::string(sym->get_string(name_id)));
			return found != host_classes.end() ? found->second : nullptr;
		}
		bool class_constant(uint64_t name_id, uint64_t member_id) { return class_constant_value(*eng, name_id, member_id).has_value(); }
		// The first host class in cls's chain marked serial_only_constructor()
		static const class_definition* serial_only_base(const class_definition* cls) {
			if (!cls) { return nullptr; }
			if (!cls->is_script_class() && cls->serial_only_construction()) { return cls; }
			for (const auto& parent : cls->get_parent_classes()) {
				if (const class_definition* found = serial_only_base(parent.get())) { return found; }
			}
			return nullptr;
		}

		template <typename Walk>
		bool walk_as_member_of(const class_definition* cls, std::string where, Walk&& walk) {
			const class_definition* saved_class = method_class;
			std::string saved_context = std::move(context);
			method_class = cls;
			const type_info_ptr saved_return = std::exchange(return_type, nullptr);   // a constructor or initializer returns no value
			context = std::move(where);
			decltype(scopes) saved;
			saved.swap(scopes);
			scopes.emplace_back();
			const bool ok = walk();
			scopes.swap(saved);
			context = std::move(saved_context);
			method_class = saved_class;
			return_type = saved_return;
			return ok;
		}

		void note_body(const std::shared_ptr<function_decl>& decl) {
			if (!decl->shared_params_cache) { decl->shared_params_cache = std::make_shared<const std::vector<parameter>>(decl->parameters); }
			admitted_bodies.push_back(decl);
		}

		bool admit_construction(uint64_t name_id, const std::shared_ptr<class_definition>& cls, size_t argc, const ast_node* site) {
			if (const class_definition* serial = serial_only_base(cls.get())) {
				if (serial == cls.get()) {
					return fail(site, "C++ class '" + cls->get_name() + "' is marked serial_only_constructor() and cannot be constructed in a parallel body");
				}
				return fail(site, "class '" + cls->get_name() + "' has a C++ base '" + serial->get_name() +
				                  "' marked serial_only_constructor() and cannot be constructed in a parallel body");
			}
			if (std::none_of(constructed_classes.begin(), constructed_classes.end(), [&](const constructed_class& c) { return c.cls == cls; })) {
				constructed_classes.push_back({ name_id, cls });
			}
			if (!cls->is_script_class()) { return true; }   // a host constructor runs no script, and is taken to be safe on any thread
			auto script_cls = std::static_pointer_cast<script_class_definition>(cls);
			const auto& ctors = script_cls->get_constructor_asts();
			if (ctors.empty()) {
				for (const auto& parent : script_cls->get_parent_classes()) {
					if (parent && !admit_construction(sym->intern(parent->get_name()), parent, 0, site)) { return false; }
				}
				return admit_field_initializers(script_cls);
			}
			for (const auto& ctor : ctors) {
				if (argc != SIZE_MAX && !arity_accepts(ctor->parameters, argc)) { continue; }
				if (!admit_constructor(script_cls, ctor, site)) { return false; }
			}
			return true;
		}

		bool admit_field_initializers(const std::shared_ptr<script_class_definition>& cls) {
			if (!visited_initializers.insert(cls.get()).second) { return true; }
			return walk_as_member_of(cls.get(), " in a field initializer of '" + cls->get_name() + "'", [&] {
				for (const auto& [field_id, initializer] : cls->get_field_initializer_asts()) {
					if (!initializer) { continue; }
					if (!walk_expr(initializer)) { return false; }
					admitted_expressions.push_back(initializer);
					note_converting_construction(cls->get_field_declared_type(field_id), initializer, initializer.get());
				}
				return true;
			});
		}

		bool admit_constructor(const std::shared_ptr<script_class_definition>& cls, const std::shared_ptr<function_decl>& ctor, const ast_node* site) {
			if (!visited_constructors.insert(ctor.get()).second) { return true; }
			if (ctor->is_coroutine) {
				return fail(site, "coroutine constructor of class '" + cls->get_name() + "' is not allowed in a parallel body");
			}
			note_body(ctor);
			const auto& parents = cls->get_parent_classes();
			std::vector<bool> named(parents.size(), false);
			const bool walked = walk_as_member_of(cls.get(), " in the constructor of '" + cls->get_name() + "'", [&] {
				for (const auto& p : ctor->parameters) {
					if (p.symbol_id == UINT64_MAX) { p.symbol_id = sym->intern(p.name); }
					declare(p.symbol_id, typed_local(p.type));
					collect_type(p.type);
					if (p.default_value && !walk_expr(p.default_value)) { return false; }
					note_converting_construction(p.type, p.default_value, p.default_value.get());
				}
				for (const auto& init : cls->effective_initializers(*ctor)) {
					for (const auto& arg : init.arguments) {
						if (!walk_expr(arg)) { return false; }
						admitted_expressions.push_back(arg);
					}
					if (init.target == "this") {
						if (!admit_construction(sym->intern(cls->get_name()), cls, init.arguments.size(), ctor.get())) { return false; }
						continue;
					}
					for (size_t i = 0; i < parents.size(); ++i) {
						if (!parents[i] || (!(i == 0 && init.target == "super") && !initializer_names(init.target, parents[i]->get_name()))) { continue; }
						named[i] = true;
						if (!admit_construction(sym->intern(parents[i]->get_name()), parents[i], init.arguments.size(), ctor.get())) { return false; }
						break;
					}
				}
				for (const auto& d : ctor->body->declarations) {
					if (!walk_stmt(d)) { return false; }
				}
				return true;
			});
			if (!walked) { return false; }
			for (size_t i = 0; i < parents.size(); ++i) {
				if (parents[i] && !named[i] && !admit_construction(sym->intern(parents[i]->get_name()), parents[i], 0, ctor.get())) { return false; }
			}
			return admit_field_initializers(cls);
		}

		// Every class whose instance can die on a chunk that reaches cls: its chain's destructors, the classes its fields can
		// hold (a field of no declared type can hold any), and its subclasses (a slot of the class can hold one)
		bool admit_destructors(const class_definition* cls) {
			if (!cls || !visited_destructor_classes.insert(cls).second) { return true; }
			for (const class_definition* level : cls->construction_order()) {
				if (!level || !level->is_script_class()) { continue; }
				auto ast = static_cast<const script_class_definition*>(level)->get_destructor_ast();
				if (ast && std::find(destructor_levels.begin(), destructor_levels.end(), level) == destructor_levels.end()) {
					destructor_levels.push_back(level);
					note_body(ast);
					const bool walked = walk_as_member_of(level, " in the destructor of '" + level->get_name() + "'", [&] {
						for (const auto& d : ast->body->declarations) {
							if (!walk_stmt(d)) { return false; }
						}
						return true;
					});
					if (!walked) { return false; }
				}
				const uint32_t slots = level->field_slot_count();
				for (uint32_t slot = 0; slot < slots; ++slot) {
					if (!admit_destructors_of_type(level->get_field_declared_type(level->slot_field(slot)))) { return false; }
				}
			}
			for (const auto& entry : engine_access::get_class_registry(*eng).script_classes_) {
				const class_definition* other = entry.second.get();
				if (other && other != cls && other->derives_from(cls) && !admit_destructors(other)) { return false; }
			}
			return true;
		}
		bool admit_destructors_of_type(const type_info_ptr& t) {
			if (!t || t->base_type == script_value_type::jai_any_type) {
				for (const auto& entry : engine_access::get_class_registry(*eng).script_classes_) {
					if (!admit_destructors(entry.second.get())) { return false; }
				}
				return true;
			}
			switch (t->base_type) {
			case script_value_type::jai_array_type:
			case script_value_type::jai_shared_ptr_type:
			case script_value_type::jai_weak_ptr_type:
				return admit_destructors_of_type(t->element_type());
			case script_value_type::jai_map_type:
				return admit_destructors_of_type(t->key_type()) && admit_destructors_of_type(t->value_type());
			case script_value_type::jai_object_type: {
				if (t->type_name.empty()) { return admit_destructors_of_type(nullptr); }
				auto cls = engine_access::get_class_registry(*eng).find_script_class(std::string(t->type_name));
				return !cls || admit_destructors(cls.get());
			}
			default:
				return true;
			}
		}

		// The barrier's fixed point: the body's constructions, the methods called on the chunk's objects (the element classes'
		// for an in-place statement, every constructed class's) and the destructors of every class whose instance can die on a
		// chunk, each of which can name more of the others
		bool admit_region_classes(const std::vector<const class_definition*>& element_classes, bool element_methods,
		                          const std::vector<type_info_ptr>& written_types, bool destructors) {
			std::vector<const class_definition*> classes;
			if (element_methods) { classes = element_classes; }
			auto note_class = [&](const class_definition* cls) {
				if (std::find(classes.begin(), classes.end(), cls) == classes.end()) { classes.push_back(cls); }
			};
			for (const class_definition* cls : captured_classes) { note_class(cls); }
			size_t levels_seen = 0;
			auto note_destructor_classes = [&] {
				for (; levels_seen < destructor_levels.size(); ++levels_seen) { note_class(destructor_levels[levels_seen]); }
			};
			if (destructors) {
				for (const class_definition* cd : element_classes) {
					if (!admit_destructors(cd)) { return false; }
				}
				for (const type_info_ptr& t : written_types) {
					if (t && !admit_destructors_of_type(t)) { return false; }
				}
				note_destructor_classes();
			}
			size_t next = 0;
			for (;;) {
				for (; next < adm.constructions.size(); ++next) {
					const auto rec = adm.constructions[next];   // copy: admission appends
					auto cls = constructor_class(rec.name_id);
					if (!cls) {
						return fail(rec.site, "call target '" + std::string(rec.name) + "' is not a function known at admission time");
					}
					if (!admit_construction(rec.name_id, cls, rec.argc, rec.site)) { return false; }
					if (destructors && !admit_destructors(cls.get())) { return false; }
					note_class(cls.get());
					note_destructor_classes();
				}
				if (!admit_element_methods(classes)) { return false; }
				note_destructor_classes();
				if (next == adm.constructions.size()) { break; }
			}
			return true;
		}
		// ===== Operators =====
		// Every class a region's objects can belong to (the element classes, the constructed ones, their fields' classes and their
		// subclasses, and with a C++ call in reach every registered class) gets a pin for each operator its chain binds: a script
		// operator whose overloads all pass the walk, a C++ operator as host methods are. A worker dispatches an operator only
		// through such a pin; an operator whose body the walk refuses stays unpinned, and dispatching it raises the walk's refusal.
		struct operator_request { const class_definition* cls = nullptr; uint64_t name_id = 0; const class_definition* owner = nullptr; bool script = false; };
		std::vector<operator_request> operator_pins;
		std::vector<parallel_method_pin_table::operator_refusal> operator_refusals;   // a script operator whose walk refused, with its violation
		std::vector<uint64_t> operator_names() {
			std::vector<uint64_t> ids;
			for (const char* op : { "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "==", "!=", "<", "<=", ">", ">=", "<=>", "~", "++", "--", "[]", "=", "()",
			                        "to_string", "to_bool", "to_int", "to_float", "to_char" }) {
				const uint64_t id = sym->intern(op);
				ids.push_back(id);
				if (const uint64_t compound = engine_access::compound_operator_id(*eng, id)) { ids.push_back(compound); }
				if (const uint64_t reflected = engine_access::reflected_operator_id(*eng, id)) { ids.push_back(reflected); }
			}
			return ids;
		}
		std::vector<const class_definition*> operator_classes(const std::vector<const class_definition*>& element_classes, bool host_calls) {
			std::vector<const class_definition*> out;
			std::vector<const class_definition*> work(element_classes.begin(), element_classes.end());
			for (const auto& c : constructed_classes) { work.push_back(c.cls.get()); }
			for (const class_definition* c : destructor_levels) { work.push_back(c); }
			if (host_calls) { for_each_registered_class([&](const class_definition* cls) { work.push_back(cls); }); }
			bool every_script_class = false;
			auto note_type = [&](const type_info_ptr& t, auto&& self) -> void {
				if (!t || t->base_type == script_value_type::jai_any_type) { every_script_class = true; return; }
				switch (t->base_type) {
				case script_value_type::jai_array_type:
				case script_value_type::jai_shared_ptr_type:
				case script_value_type::jai_weak_ptr_type:
					self(t->element_type(), self);
					return;
				case script_value_type::jai_map_type:
					self(t->key_type(), self);
					self(t->value_type(), self);
					return;
				case script_value_type::jai_object_type: {
					if (t->type_name.empty()) { every_script_class = true; return; }
					if (auto cls = engine_access::get_class_registry(*eng).find_script_class(std::string(t->type_name))) { work.push_back(cls.get()); return; }
					const auto& host_classes = engine_access::get_class_registry(*eng).cpp_classes_;
					if (auto found = host_classes.find(std::string(t->type_name)); found != host_classes.end() && found->second) { work.push_back(found->second.get()); }
					return;
				}
				default:
					return;
				}
			};
			while (!work.empty()) {
				const class_definition* cls = work.back();
				work.pop_back();
				if (!cls || std::find(out.begin(), out.end(), cls) != out.end()) { continue; }
				out.push_back(cls);
				for (const class_definition* level : cls->construction_order()) {
					if (!level) { continue; }
					const uint32_t slots = level->field_slot_count();
					for (uint32_t slot = 0; slot < slots; ++slot) { note_type(level->get_field_declared_type(level->slot_field(slot)), note_type); }
				}
				for (const auto& entry : engine_access::get_class_registry(*eng).script_classes_) {
					if (entry.second && entry.second.get() != cls && entry.second->derives_from(cls)) { work.push_back(entry.second.get()); }
				}
				if (every_script_class) {
					every_script_class = false;
					for (const auto& entry : engine_access::get_class_registry(*eng).script_classes_) { if (entry.second) { work.push_back(entry.second.get()); } }
				}
			}
			return out;
		}
		// the overloads of a script operator walk as methods of the class that declares them; false: the walk refused one (a trial the
		// caller rolls back)
		bool admit_script_operator(const class_definition* declaring, uint64_t name_id) {
			const auto& table = declaring->script_method_overloads();
			const auto found = table.find(name_id);
			if (found == table.end()) { return false; }
			const std::string where = " in operator '" + std::string(sym->get_string(name_id)) + "' of '" + declaring->get_name() + "'";
			for (const auto& decl : found->second) {
				if (!decl || !decl->body || decl->is_coroutine) { return fail(decl.get(), "operator" + where + " cannot be pinned"); }
				if (!visited_methods.insert(decl.get()).second) { continue; }
				note_body(decl);
				const bool walked = walk_as_member_of(declaring, where, [&] {
					return_type = decl->return_type;
					for (const auto& p : decl->parameters) {
						if (p.symbol_id == UINT64_MAX) { p.symbol_id = sym->intern(p.name); }
						declare(p.symbol_id, typed_local(p.type));
						collect_type(p.type);
						if (p.default_value && !walk_expr(p.default_value)) { return false; }
						note_converting_construction(p.type, p.default_value, p.default_value.get());
					}
					collect_type(decl->return_type);
					for (const auto& d : decl->body->declarations) {
						if (!walk_stmt(d)) { return false; }
					}
					return true;
				});
				if (!walked) { return false; }
			}
			return true;
		}

		bool any_program_destructor() {
			for (const auto& entry : engine_access::get_class_registry(*eng).script_classes_) {
				const class_definition* cls = entry.second.get();
				if (cls && cls->has_method(cls->destructor_id(*eng))) { return true; }
			}
			return false;
		}

		bool walk_stmt(const statement_ptr& s) {
			if (!s) { return true; }
			switch (s->get_type()) {
			case node_type::expression_stmt:
				return walk_expr(static_cast<expression_stmt*>(s.get())->expression);
			case node_type::expression_decl:
				return walk_expr(static_cast<expression_decl*>(s.get())->expression);
			case node_type::statement_decl:
				return walk_stmt(static_cast<statement_decl*>(s.get())->statement);
			case node_type::block_stmt: {
				auto* b = static_cast<block_stmt*>(s.get());
				scopes.emplace_back();
				for (const auto& d : b->declarations) {
					if (!walk_stmt(d)) { scopes.pop_back(); return false; }
				}
				scopes.pop_back();
				return true;
			}
			case node_type::if_stmt: {
				auto* n = static_cast<if_stmt*>(s.get());
				if (proof_mode && maybe_object(n->condition)) { root_escaped = true; }
				return walk_expr(n->condition) && walk_stmt(n->then_statement) && walk_stmt(n->else_statement);
			}
			case node_type::while_stmt: {
				auto* n = static_cast<while_stmt*>(s.get());
				if (proof_mode && maybe_object(n->condition)) { root_escaped = true; }
				return walk_expr(n->condition) && walk_stmt(n->body);
			}
			case node_type::for_stmt: {
				auto* n = static_cast<for_stmt*>(s.get());
				scopes.emplace_back();
				bool ok = walk_stmt(n->initializer) && walk_expr(n->condition) &&
				          walk_expr(n->update) && walk_stmt(n->body);
				if (proof_mode && maybe_object(n->condition)) { root_escaped = true; }   // after the initializer declares the loop's locals
				scopes.pop_back();
				return ok;
			}
			case node_type::range_for_stmt: {
				auto* n = static_cast<range_for_stmt*>(s.get());
				if (n->is_reference) {
					// By-ref iteration mutates elements in place - on captured state (a member of a captured object
					// included) that is a write, and on a worker's view it would silently diverge
					if (identifier_expr* root = global_rooted(n->container.get()) ? chain_root_identifier(n->container.get()) : nullptr) {
						return fail(n, "cannot iterate captured state '" + std::string(root->name) +
						               "' by reference in a parallel body (captured reads are read-only)");
					}
					if (proof_mode && (global_rooted(n->container.get()) || !chain_root_identifier(n->container.get()))) {
						return fail(n, "iterating by reference reaches state the proof does not own");
					}
				}
				if (!walk_expr(n->container)) { return false; }
				if (n->variable_name_id == UINT64_MAX) { n->variable_name_id = sym->intern(n->variable_name); }
				collect_type(n->element_type);
				// the loop binds each element unconverted: the variable is primitive when the container's elements are
				local_info loop_var;
				if (proof_mode) {
					type_info_ptr ct = proof_chain_type(n->container.get());
					if (!builtin_container(ct)) { root_escaped = true; }   // a coroutine or an instance: iteration runs user code
					type_info_ptr et = !ct ? nullptr
					                 : ct->base_type == script_value_type::jai_array_type ? ct->element_type()
					                 : ct->base_type == script_value_type::jai_string_type ? ct : nullptr;
					loop_var = { n->element_type ? n->element_type : et, !n->is_reference && primitive_or_string(et) &&
					             (!n->element_type || primitive_or_string(n->element_type)) };
					loop_var.reference = true;   // the local proof never trusts a loop variable's storage to be its own
					if (!n->is_reference && !loop_var.primitive) { note_death(loop_var.type); }
				}
				scopes.emplace_back();
				declare(n->variable_name_id, loop_var);
				bool ok = walk_stmt(n->body);
				scopes.pop_back();
				return ok;
			}
			case node_type::return_stmt: {
				auto* n = static_cast<return_stmt*>(s.get());
				if (proof_mode && n->value && conversion_may_run_code(return_type, n->value)) { root_escaped = true; }
				note_converting_construction(return_type, n->value, n);
				return walk_expr(n->value);
			}
			case node_type::break_stmt:
			case node_type::continue_stmt:
			case node_type::fallthrough_stmt:
				return true;
			case node_type::switch_stmt: {
				auto* n = static_cast<switch_stmt*>(s.get());
				if (proof_mode && maybe_object(n->condition)) { root_escaped = true; }
				if (!walk_expr(n->condition)) { return false; }
				for (const auto& c : n->cases) {
					if (!walk_expr(c->value)) { return false; }
					scopes.emplace_back();
					for (const auto& st : c->body) {
						if (!walk_stmt(st)) { scopes.pop_back(); return false; }
					}
					scopes.pop_back();
				}
				if (n->default_case) {
					scopes.emplace_back();
					for (const auto& st : n->default_case->body) {
						if (!walk_stmt(st)) { scopes.pop_back(); return false; }
					}
					scopes.pop_back();
				}
				return true;
			}
			case node_type::try_stmt: {
				auto* n = static_cast<try_stmt*>(s.get());
				if (!walk_stmt(n->try_block)) { return false; }
				scopes.emplace_back();
				if (!n->catch_var.empty()) { declare(sym->intern(n->catch_var)); }
				bool ok = walk_stmt(n->catch_block);
				scopes.pop_back();
				return ok;
			}
			case node_type::variable_decl: {
				auto* n = static_cast<variable_decl*>(s.get());
				if (n->name_id == UINT64_MAX) { n->name_id = sym->intern(n->name); }
				collect_type(n->type);
				bool ok;
				if (n->type.get() && n->type->is_reference()) {
					// Reference declaration: the alias must root at worker-local storage
					ok = walk_target(n->initializer, "reference declaration", /*aliasing*/ true);
				} else {
					ok = walk_expr(n->initializer);
					note_converting_construction(n->type, n->initializer, n);
				}
				if (!ok) { return false; }
				local_info info = typed_local(n->type);
				if (proof_mode) {
					const bool reference = n->type.get() && n->type->is_reference();
					if (!reference && conversion_may_run_code(n->type, n->initializer)) { root_escaped = true; }
					if (reference) { info = {}; info.reference = true; info.referent = referent_type(n->type); }
					else if (!n->type && n->initializer && !maybe_object(n->initializer)) { info.primitive = true; }   // auto locks to its primitive initializer
					if (!reference && !info.primitive) { note_death(n->type); }   // the local dies with its block
				}
				declare(n->name_id, info);
				return true;
			}
			case node_type::destructuring_decl: {
				auto* n = static_cast<destructuring_decl*>(s.get());
				if (!walk_expr(n->initializer)) { return false; }
				local_info binding;
				binding.reference = true;   // a structured binding may name a part of its source
				for (const auto& [name, id] : n->names) { declare(id, binding); }
				return true;
			}
			case node_type::function_decl:
				return fail(s.get(), "function declarations are not allowed in a parallel body");
			case node_type::class_decl:
				return fail(s.get(), "class declarations are not allowed in a parallel body");
			case node_type::namespace_decl:
				return fail(s.get(), "namespace declarations are not allowed in a parallel body");
			case node_type::enum_decl:
				return fail(s.get(), "enum declarations are not allowed in a parallel body");
			case node_type::include_decl:
			case node_type::import_decl:
				return fail(s.get(), "include/import is not allowed in a parallel body");
			default:
				return fail(s.get(), "statement is not allowed in a parallel body");
			}
		}

		bool walk_expr(const expression_ptr& e) {
			if (!e) { return true; }
			switch (e->get_type()) {
			case node_type::literal_expr:
				return true;
			case node_type::identifier_expr: {
				auto* n = static_cast<identifier_expr*>(e.get());
				if (is_element_symbol(n->symbol_id)) { note_element_whole(); if (proof_mode) { root_escaped = true; } }
				if (is_local(n->symbol_id)) { note_const_ref_whole(n->symbol_id); return true; }
				if (self_field(n->symbol_id)) { return true; }   // element-rooted field read
				if (method_class && method_class->defines_method(n->symbol_id)) {
					return fail(n, "method '" + std::string(n->name) + "' cannot be captured as a value in a parallel body");
				}
				// Captured read (whole-value use): the barrier classifies and provisions
				// it per worker; writes to it are rejected at walk_target / the wall
				touch_capture(n, false);
				note_bare_capture(n);
				return true;
			}
			case node_type::binary_expr: {
				auto* n = static_cast<binary_expr*>(e.get());
				if (proof_mode) {
					if (n->op.type == token_type::left_bracket) {
						if (!builtin_container(proof_chain_type(n->left.get())) || maybe_object(n->right)) { root_escaped = true; }
					} else if (maybe_object(n->left) || maybe_object(n->right)) {
						root_escaped = true;
					}
				}
				if (n->op.type == token_type::left_bracket) {
					// Subscript READ rooted at an enclosing name: the borrow-eligible
					// capture shape (raw element reads through the region borrow)
					if (n->left && n->left->get_type() == node_type::identifier_expr) {
						auto* root = static_cast<identifier_expr*>(n->left.get());
						if (!is_local(root->symbol_id)) {
							touch_capture(root, true, n->right.get());
							return walk_expr(n->right);
						}
						if (const_ref_local(root)) { return walk_expr(n->right); }
					}
					if (proof_mode && chain_root_is_element(e)) { return proof_chain_value(e); }
				}
				return walk_expr(n->left) && walk_expr(n->right);
			}
			case node_type::unary_expr: {
				auto* n = static_cast<unary_expr*>(e.get());
				if (n->op.type == token_type::plus_plus || n->op.type == token_type::minus_minus) {
					if (proof_mode && chain_root_is_element(n->operand)) { root_escaped = true; }   // a user operator could run on the referent
					if (proof_mode && store_may_run_code(n->operand, nullptr, true)) { root_escaped = true; }
					check_local_write(n->operand.get(), true);
					note_replaced(n->operand.get());
					return walk_target(n->operand, "increment/decrement");
				}
				if (proof_mode && maybe_object(n->operand)) { root_escaped = true; }
				return walk_expr(n->operand);
			}
			case node_type::assignment_expr: {
				auto* n = static_cast<assignment_expr*>(e.get());
				if (proof_mode && n->op.type != token_type::equal && chain_root_is_element(n->target)) { root_escaped = true; }
				if (proof_mode && store_may_run_code(n->target, n->value, n->op.type != token_type::equal)) { root_escaped = true; }
				check_local_write(n->target.get(), true);
				note_replaced(n->target.get());
				if (n->op.type == token_type::equal) { note_store_conversions(n->target.get(), n->value, n); }
				return walk_target(n->target, "assignment") && walk_expr(n->value);
			}
			case node_type::call_expr:
				return walk_call(static_cast<call_expr*>(e.get()));
			case node_type::member_expr: {
				if (proof_mode && chain_root_is_element(e)) { return proof_chain_value(e); }
				if (proof_mode && global_rooted(e.get()) && proof_chain_type(e.get())) { return walk_chain_indexes(e.get()); }   // a serial proof reads program state in place: no accessor, no write
				auto* m = static_cast<member_expr*>(e.get());
				if (m->is_static && m->object && m->object->get_type() == node_type::identifier_expr) {
					// an enum's value (E::v): a constant the body reads through the enum's map, provisioned per worker as a captured read
					auto* root = static_cast<identifier_expr*>(m->object.get());
					if (root->symbol_id != UINT64_MAX && !is_local(root->symbol_id) && engine_access::is_enum_name(*eng, root->symbol_id)) {
						if (m->member_id == UINT64_MAX) { m->member_id = sym->intern(m->member); }
						(void)sym->get_class_var_id_with_view(root->symbol_id);   // the static read asks for a class of the name first
						touch_capture(root, false);
						return true;
					}
					// a class constant (C::K, a const static of a primitive value): pinned per worker at the barrier
					if (root->symbol_id != UINT64_MAX && !is_local(root->symbol_id)) {
						if (m->member_id == UINT64_MAX) { m->member_id = sym->intern(m->member); }
						if (class_constant(root->symbol_id, m->member_id)) {
							(void)sym->get_class_var_id_with_view(root->symbol_id);
							bool seen = false;
							for (const auto& r : adm.const_static_reads) { seen = seen || (r.class_name_id == root->symbol_id && r.name_id == m->member_id); }
							if (!seen) { adm.const_static_reads.push_back({ root->symbol_id, m->member_id }); }
							return true;
						}
					}
				}
				return walk_local_member_chain(m, "member access");
			}
			case node_type::lambda_expr:
				return fail(e.get(), "lambdas are not allowed in a parallel body");
			case node_type::new_expr: {
				// Name n(args), Name n; and new Name(args): a script class's construction, walked at the barrier
				auto* n = static_cast<new_expr*>(e.get());
				const type_info_ptr named = n->type && n->type->base_type == script_value_type::jai_shared_ptr_type ? n->type->element_type() : n->type;
				if (proof_mode || !class_type(named)) { return fail(e.get(), "'new' is not allowed in a parallel body"); }
				const uint64_t name_id = sym->intern(named->type_name);
				if (!constructor_class(name_id)) { return fail(e.get(), "'new' is not allowed in a parallel body"); }
				collect_type(n->type);
				collect_type(named);
				adm.constructions.push_back({ name_id, sym->get_string(name_id), n->arguments.size(), e.get() });
				note_constructor_argument_conversions(name_id, n->arguments);
				for (const auto& a : n->arguments) {
					if (!walk_expr(a)) { return false; }
				}
				return true;
			}
			case node_type::this_expr:
				if (method_class) { return true; }   // self IS the chunk-exclusive element
				return fail(e.get(), "'this'/'super' is not allowed in a parallel body");
			case node_type::super_expr:
				return fail(e.get(), "'this'/'super' is not allowed in a parallel body");
			case node_type::ternary_expr: {
				auto* n = static_cast<ternary_expr*>(e.get());
				if (proof_mode && maybe_object(n->condition)) { root_escaped = true; }
				return walk_expr(n->condition) && walk_expr(n->then_expression) && walk_expr(n->else_expression);
			}
			case node_type::array_literal_expr: {
				auto* n = static_cast<array_literal_expr*>(e.get());
				for (const auto& el : n->elements) {
					if (!walk_expr(el)) { return false; }
				}
				return true;
			}
			case node_type::map_literal_expr: {
				auto* n = static_cast<map_literal_expr*>(e.get());
				for (const auto& kv : n->entries) {
					if (!walk_expr(kv.first) || !walk_expr(kv.second)) { return false; }
				}
				return true;
			}
			case node_type::throw_expr:
				if (proof_mode && maybe_object(static_cast<throw_expr*>(e.get())->value)) { root_escaped = true; }   // the thrown copy dies where it is caught
				return walk_expr(static_cast<throw_expr*>(e.get())->value);
			case node_type::cast_expr: {
				auto* n = static_cast<cast_expr*>(e.get());
				if (n->by_reference) { return fail(e.get(), "cast<T&> is not allowed in a parallel body"); }
				if (proof_mode && conversion_may_run_code(n->target, n->operand)) { root_escaped = true; }
				note_converting_construction(n->target, n->operand, n);
				note_death(n->target);   // the cast's value is a temporary
				return walk_expr(n->operand);
			}
			case node_type::yield_expr:
				return fail(e.get(), "yield is not allowed in a parallel body");
			case node_type::include_expr:
				return fail(e.get(), "include is not allowed in a parallel body");
			case node_type::import_expr:
				return fail(e.get(), "import is not allowed in a parallel body");
			default:
				return fail(e.get(), "expression is not allowed in a parallel body");
			}
		}

		// Stores and reference bindings must root at worker-local storage. These are the
		// statically-FREE write rejections (early diagnostics for the runtime write
		// wall): a store or alias rooted at an enclosing name errors here with position.
		bool walk_target(const expression_ptr& t, const char* what, bool aliasing = false) {
			if (!t) { return fail(nullptr, std::string(what) + " target missing"); }
			switch (t->get_type()) {
			case node_type::identifier_expr: {
				auto* n = static_cast<identifier_expr*>(t.get());
				if (is_element_symbol(n->symbol_id)) { note_element_whole(); if (proof_mode && aliasing) { root_escaped = true; } }   // replaced: the old element releases every handle
				if (loop_variable_position(n) != SIZE_MAX) { note_loop_variable_write(n); }   // the body's own: a callee's local of the same name is its own
				if (is_local(n->symbol_id)) { note_const_ref_whole(n->symbol_id); return true; }
				if (!aliasing && self_field(n->symbol_id)) { return true; }   // element field store (chunk-exclusive)
				if (aliasing) {
					return fail(n, std::string(what) + " would alias enclosing state '" + std::string(n->name) +
					               "' (captured reads are read-only)");
				}
				return fail(n, "body writes enclosing state '" + std::string(n->name) + "'");
			}
			case node_type::binary_expr: {
				auto* n = static_cast<binary_expr*>(t.get());
				if (n->op.type == token_type::left_bracket) {
					// out[i] with out an enclosing name and i a loop variable: a disjoint index write (adm.index_writes)
					const size_t index_param = aliasing ? SIZE_MAX : loop_variable_position(n->right.get());
					if (index_param != SIZE_MAX && n->left && n->left->get_type() == node_type::identifier_expr) {
						auto* root = static_cast<identifier_expr*>(n->left.get());
						if (!is_local(root->symbol_id) && !is_element_symbol(root->symbol_id) && !self_field(root->symbol_id)) {
							touch_capture(root, false, n->right.get());
							adm.index_writes.push_back({ root->symbol_id, root->name, index_param, t.get() });
							return true;
						}
					}
					if (proof_mode && chain_root_is_element(t)) { adm.element_mutated = true; }
					return walk_target(n->left, what, aliasing) && walk_expr(n->right);
				}
				return fail(t.get(), std::string(what) + " target is not allowed in a parallel body");
			}
			case node_type::member_expr: {
				// A member store, or a reference bound to a member, roots at the element or a worker-local value (a store into the
				// transform's private copy is legal and invisible); an enclosing-rooted chain fails inside
				if (aliasing && proof_mode) { return fail(t.get(), "a reference to a member escapes the proof"); }
				if (aliasing) {
					const identifier_expr* root = chain_root_identifier(t.get());
					if (root && !is_local(root->symbol_id) && !self_field(root->symbol_id)) {
						return fail(root, std::string(what) + " would alias enclosing state '" + std::string(root->name) + "' (captured reads are read-only)");
					}
				}
				if (chain_root_is_element(t)) { adm.element_mutated = true; }
				return walk_local_member_chain(static_cast<member_expr*>(t.get()), aliasing ? what : "member store");
			}
			default:
				return fail(t.get(), std::string(what) + " target is not allowed in a parallel body");
			}
		}

		bool walk_call(call_expr* call) {
			// Builtin container/string METHOD call: receiver expression walks under the
			// normal rules (so it roots at worker-local or captured values); the method
			// itself comes from the shared registry both backends use.
			if (call->callee && call->callee->get_type() == node_type::member_expr) {
				auto* m = static_cast<member_expr*>(call->callee.get());
				if (m->is_static) {
					// math:: intrinsics are value functions; crypto::sha256_file reads a file (the rest of crypto:: is pinned below)
					const jai::detail::math_fn fn = jai::detail::math_intrinsic_for_call(m);
					if (m->object && m->object->get_type() == node_type::identifier_expr && static_cast<identifier_expr*>(m->object.get())->name == "crypto" &&
					    m->member == "sha256_file") {
						return fail(m, "crypto::sha256_file cannot be called in a parallel body (it reads a file)");
					}
					if (fn != jai::detail::math_fn::none) {
						for (const auto& a : call->arguments) {
							if (proof_mode && maybe_object(a)) { root_escaped = true; }
							if (!walk_expr(a)) { return false; }
						}
						return true;
					}
					if (m->object && m->object->get_type() == node_type::identifier_expr &&
					    static_cast<identifier_expr*>(m->object.get())->name == "time") {
						return fail(m, "time:: calls are not allowed in a parallel body (engine-owned clock)");
					}
					if (!proof_mode && m->object && m->object->get_type() == node_type::identifier_expr) {
						// a pure library namespace entry (web::url_encode): each worker calls its own copy, pinned at the barrier
						auto* space = static_cast<identifier_expr*>(m->object.get());
						if (parallel_host_whitelisted(std::string(space->name) + "::" + std::string(m->member)) && namespace_host_entry(*eng, space->name, m->member)) {
							if (space->symbol_id == UINT64_MAX) { space->symbol_id = sym->intern(space->name); }
							if (m->member_id == UINT64_MAX) { m->member_id = sym->intern(m->member); }
							adm.static_calls.push_back({ space->symbol_id, m->member_id });
							for (const auto& a : call->arguments) {
								if (!walk_expr(a)) { return false; }
							}
							return true;
						}
					}
					if (!proof_mode && m->object && m->object->get_type() == node_type::identifier_expr) {
						// A host class's static method runs on any thread unless it is marked serial
						auto* class_name = static_cast<identifier_expr*>(m->object.get());
						const auto& host_classes = engine_access::get_class_registry(*eng).cpp_classes_;
						const auto host = host_classes.find(std::string(class_name->name));
						if (m->member_id == UINT64_MAX) { m->member_id = sym->intern(m->member); }
						if (host != host_classes.end() && host->second->static_method_in_chain(m->member_id).is_function()) {
							if (host->second->serial_static_method(m->member_id)) {
								return fail(m, "C++ static method '" + std::string(class_name->name) + "::" + std::string(m->member) +
								               "' is marked serial_static_method() and cannot be called in a parallel body");
							}
							const script_value method = host->second->static_method_in_chain(m->member_id);
							const auto* writes = method.as_function().target<host_writeback_function>();
							for (size_t i = 0; i < call->arguments.size() && i < 64; ++i) {
								if (!writes || !((writes->mask >> i) & 1)) { continue; }
								note_loop_variable_reference_argument(call->arguments[i]);
								if (identifier_expr* root = enclosing_root(call->arguments[i])) {
									return fail(root, "argument passes enclosing state '" + std::string(root->name) +
									                  "' by reference (captured reads are read-only)");
								}
							}
							if (class_name->symbol_id == UINT64_MAX) { class_name->symbol_id = sym->intern(class_name->name); }
							adm.static_calls.push_back({ class_name->symbol_id, m->member_id });
							for (const auto& a : call->arguments) {
								if (!walk_expr(a)) { return false; }
							}
							return true;
						}
					}
					return fail(m, "static member calls are not allowed in a parallel body");
				}
				if (m->member_id == UINT64_MAX) { m->member_id = sym->intern(m->member); }   // warm
				// the accessor ids a method call probes on either backend (the user-accessor check
				// runs before dispatch): interned here, single-threaded, so a worker's probe never
				// misses the frozen interner
				m->getter_id = sym->get_getter_id_with_view(m->member_id).first;
				(void)sym->get_setter_id_with_view(m->member_id);
				// proof mode: a method on anything but the root runs no user code only as a builtin of a statically known
				// container or string
				if (proof_mode && !chain_root_is_element(m->object) && !builtin_method_call(call)) { root_escaped = true; }
				// A method on captured state must be a known read-only builtin: anything
				// else may mutate the (worker-private snapshot of the) receiver, which
				// would silently diverge across worker counts
				if (identifier_expr* root = enclosing_root(m->object)) {
					// an array's find(f) calls back into script: never a captured receiver's read
					const bool callback_find = m->member == "find" &&
					    (builtin_runs_script(m->member, proof_chain_type(m->object.get())) ||
					     std::any_of(call->arguments.begin(), call->arguments.end(), [](const expression_ptr& a) { return a && a->get_type() == node_type::lambda_expr; }));
					if (!proof_mode && !callback_find && m->object->get_type() == node_type::identifier_expr && !parallel_readonly_method(m->member)) {
						// a method of a captured object (or a container's mutator): the barrier judges it against the captured value
						touch_capture(root, false);
						note_captured_method_call(root, m, call);
						for (const auto& a : call->arguments) {
							if (!walk_expr(a)) { return false; }
						}
						return true;
					}
					if (!parallel_readonly_method(m->member) || callback_find) {
						return fail(m, "method '" + std::string(m->member) + "' may mutate captured state '" +
						               std::string(root->name) + "'");
					}
					if (m->object->get_type() == node_type::identifier_expr) {
						touch_capture(root, borrow_count_call(m, call));   // whole-container method receiver: the count builtins read a borrow
					} else if (!walk_expr(m->object)) {
						return false;   // subscript-chain receiver: subscript walk records the touch
					}
				} else {
					// Element / worker-local receiver: record the name for the barrier's
					// class-method admission (the receiver's class is a runtime fact; names
					// no element class defines are builtins and simply never match)
					identifier_expr* chain_root = proof_mode ? nullptr : chain_root_identifier(m->object.get());
					if (chain_root && !is_local(chain_root->symbol_id) && !self_field(chain_root->symbol_id) && !is_element_symbol(chain_root->symbol_id) &&
					    !parallel_readonly_method(m->member)) {
						note_captured_method_call(chain_root, m, call);   // a method of an object a captured one holds
					} else {
						collect_element_method(m->member_id, m->member, m, call);
					}
					note_method_argument_conversions(receiver_class(m->object.get()), m->member_id, call);
					if (chain_root_is_element(m->object) && !parallel_readonly_method(m->member)) { adm.element_mutated = true; }
					if (proof_mode && !chain_root_is_element(m->object) && (!parallel_readonly_method(m->member) || builtin_runs_script(m->member, proof_chain_type(m->object.get())))) {
						if (global_rooted(m->object.get())) { return fail(m, "method '" + std::string(m->member) + "' may mutate program state"); }
						check_local_write(m->object.get(), false);
						note_death(proof_chain_type(m->object.get()));   // a removed element dies
					}
					if (proof_mode && chain_root_is_element(m->object)) {
						// A read-only builtin on a receiver that is statically a container or string is a
						// read; a receiver of unknown or class type could run a user method on the referent
						if (!walk_chain_indexes(m->object.get())) { return false; }
						type_info_ptr rt = proof_chain_type(m->object.get());
						const bool container_or_string = rt && (rt->base_type == script_value_type::jai_array_type ||
						                                        rt->base_type == script_value_type::jai_map_type ||
						                                        rt->base_type == script_value_type::jai_string_type);
						if (!container_or_string || !parallel_readonly_method(m->member) || builtin_runs_script(m->member, rt)) { root_escaped = true; }
					} else if (!(const_ref_local(m->object.get()) && borrow_count_call(m, call)) && !walk_expr(m->object)) {
						return false;
					}
				}
				for (const auto& a : call->arguments) {
					if (!walk_expr(a)) { return false; }
				}
				return true;
			}
			if (!call->callee || call->callee->get_type() != node_type::identifier_expr) {
				return fail(call, "call target is not statically resolvable in a parallel body");
			}
			auto* callee = static_cast<identifier_expr*>(call->callee.get());
			const std::string callee_name(callee->name);
			if (is_local(callee->symbol_id)) {
				return fail(callee, "call target '" + callee_name + "' is not statically resolvable in a parallel body");
			}
			// A bare sibling call of a script method is this.name(): the barrier pins the method for every class the receiver can
			// be, and a worker parks it through the pin. A C++ base method stays on the 'this.' spelling
			if (method_class && method_class->defines_method(callee->symbol_id)) {
				const script_value* bound = method_class->method_ptr(callee->symbol_id);
				if (!bound || !bound->is_function() || !bound->as_function().target<script_method_dispatch>()) {
					return fail(callee, "method '" + callee_name + "' must be called as 'this." + callee_name +
					               "()' in a parallel body (a bare call of a C++ base method)");
				}
				collect_element_method(callee->symbol_id, callee->name, callee, call);
				note_method_argument_conversions(method_class, callee->symbol_id, call);
				for (const auto& a : call->arguments) {
					if (!walk_expr(a)) { return false; }
				}
				return true;
			}
			// A script class's construction: the barrier walks what it runs (admit_construction)
			if (!proof_mode && !primitive_ctor_name(callee->name) && constructor_class(callee->symbol_id)) {
				adm.constructions.push_back({ callee->symbol_id, callee->name, call->arguments.size(), callee });
				note_constructor_argument_conversions(callee->symbol_id, call->arguments);
				for (const auto& a : call->arguments) {
					if (!walk_expr(a)) { return false; }
				}
				return true;
			}
			// Resolve the callee BEFORE walking arguments so the caller sees the most
			// useful violation first (e.g. a nested parallel_transform names itself,
			// not its function-valued argument)
			bool callee_ok = false;
			// proof mode: an argument converts into its parameter (a host function formats or converts whatever it takes)
			bool converts_like_host = false;
			if (primitive_ctor_name(callee->name)) {
				callee_ok = true;   // int(x)/float(x)/... conversions - backend-resolved, pure
				converts_like_host = true;
			} else {
				auto global = engine_access::get_global_environment(*eng)->get(callee->symbol_id);
				if (!global || !global.value().is_function()) {
					return fail(callee, "call target '" + callee_name + "' is not a function known at admission time");
				}
				if (const script_callable* payload = payload_from_function_value(global.value())) {
					if (payload->kind != script_callable::kind_type::function || !payload->fn) {
						return fail(callee, "call target '" + callee_name + "' is not a plain function");
					}
					const std::shared_ptr<script_defined_function> callee_fn = script_defined_function::environment_form(payload->fn, eng);
					if (!admit_function(callee_fn, callee->symbol_id, root_param::none, callee)) { return false; }
					note_death(callee_fn->return_type);   // the call's result is a temporary
					// A non-const by-ref parameter receiving captured state is a write channel (the callee may store through it):
					// refused statically. A const T& one is a captured read, a subscript read when the callee only subscripts it
					const auto& params = callee_fn->parameters();
					for (size_t i = 0; i < call->arguments.size() && i < params.size(); ++i) {
						if (proof_mode && !params[i].is_reference && conversion_may_run_code(params[i].type, call->arguments[i])) { root_escaped = true; }
						if (!params[i].is_reference) { note_converting_construction(params[i].type, call->arguments[i], call->arguments[i].get()); }
						if (!params[i].is_reference) { continue; }
						const expression* arg = call->arguments[i].get();
						const bool const_ref = !proof_mode && params[i].is_const;
						if (!proof_mode && !params[i].is_const && !ref_parameter_takes_temporary(*eng, params, callee_fn->body.get(), i, true)) {
							note_loop_variable_reference_argument(call->arguments[i]);
						}
						if (identifier_expr* root = enclosing_root(call->arguments[i])) {
							if (!const_ref) {
								const bool reads_only = !proof_mode && ref_parameter_takes_temporary(*eng, params, callee_fn->body.get(), i, true);
								return fail(root, proof_mode ? "argument passes enclosing state '" + std::string(root->name) + "' by reference (captured reads are read-only)"
								                             : reference_argument_refusal(root, params[i], callee_name, reads_only));
							}
							if (arg == root) {
								const size_t use = const_ref_use_index(callee_fn->body.get(), i);
								touch_capture(root, use != SIZE_MAX);
								note_bare_capture(root);
								if (use != SIZE_MAX) { const_ref_touches.emplace_back(capture_index[root->symbol_id], use); }
								forwarded_arguments.push_back(arg);
							}
							continue;
						}
						if (const_ref && const_ref_local(arg)) {
							if (const size_t use = const_ref_use_index(callee_fn->body.get(), i); use != SIZE_MAX) {
								const_ref_forwards.emplace_back(local(static_cast<const identifier_expr*>(arg)->symbol_id)->const_ref_use, use);
								forwarded_arguments.push_back(arg);
								continue;
							}
						}
						// A callee that writes through the parameter writes the argument's place
						if (proof_mode && !ref_parameter_takes_temporary(*eng, params, callee_fn->body.get(), i, true)) {
							const expression* arg = call->arguments[i].get();
							if (!chain_root_identifier(arg) || global_rooted(arg)) { return fail(arg, "argument passes state the proof does not own to a writing reference parameter"); }
							if (chain_root_is_element(call->arguments[i])) { adm.element_mutated = true; }
							else { check_local_write(arg, false); }
						}
					}
					callee_ok = true;
				} else if (proof_mode ? parallel_host_whitelisted(callee->name) : engine_access::host_function_registered(*eng, callee_name) && !engine_access::host_function_serial(*eng, callee_name)) {
					// A host function runs on any thread unless it is marked serial; a non-const T& parameter writes
					// back into its argument, which enclosing state cannot take
					const uint64_t writeback = engine_access::host_function_writeback(*eng, callee_name);
					for (size_t i = 0; i < call->arguments.size() && i < 64; ++i) {
						if (!((writeback >> i) & 1)) { continue; }
						note_loop_variable_reference_argument(call->arguments[i]);
						if (identifier_expr* root = enclosing_root(call->arguments[i])) {
							return fail(root, "argument passes enclosing state '" + std::string(root->name) +
							                  "' by reference (captured reads are read-only)");
						}
					}
					if (collected_host_ids.insert(callee->symbol_id).second) {
						adm.host_functions.emplace_back(callee->symbol_id, callee_name);
					}
					callee_ok = true;
					converts_like_host = true;
				} else if (proof_mode || !engine_access::host_function_registered(*eng, callee_name)) {
					return fail(callee, "host function '" + callee_name + "' is not callable in a parallel body");
				} else {
					return fail(callee, "host function '" + callee_name + "' is marked serial_function() and cannot be called in a parallel body");
				}
			}
			for (const auto& a : call->arguments) {
				if (proof_mode && converts_like_host && maybe_object(a)) { root_escaped = true; }
				if (std::find(forwarded_arguments.begin(), forwarded_arguments.end(), a.get()) != forwarded_arguments.end()) { continue; }
				if (!walk_expr(a)) { return false; }
			}
			return callee_ok;
		}
	};

	bool prove_readonly_parameter(engine& eng, const std::shared_ptr<script_defined_function>& fn, size_t index) {
		const parameter& p = fn->parameters()[index];
		if (p.is_reference || p.ref_escaping) { return false; }
		admission_builder builder(eng);
		builder.proof_mode = true;
		builder.prefix = "parameter proof";
		if (p.symbol_id == UINT64_MAX) { p.symbol_id = builder.sym->intern(p.name); }
		builder.element_symbol = p.symbol_id;
		builder.root_type = p.type;
		builder.param_ref_aliases = fn->has_reference_parameters;
		if (builder.type_runs_destructor(p.type)) { return false; }   // the elided copy's destructor would never run
		const bool walked = builder.admit_function(fn, UINT64_MAX, admission_builder::root_param::none, nullptr);
		return walked && !builder.failed && !builder.adm.element_mutated && !builder.root_escaped;
	}

	bool local_alias_candidate(const variable_decl& d) {
		if (d.is_static || d.is_inline || d.default_initialized || !d.initializer || d.slot_index == SIZE_MAX || d.ref_escaping || !d.type) { return false; }
		switch (d.type->base_type) {
		case script_value_type::jai_object_type: if (d.type->type_name.empty()) { return false; } break;
		case script_value_type::jai_array_type: case script_value_type::jai_map_type: break;
		default: return false;
		}
		return admission_builder::chain_root_identifier(d.initializer.get()) != nullptr;
	}

	// The function body's statements in order, its locals in scope: each declaration stamped with this home is proven
	// against the statements after it in its block
	struct local_alias_prover {
		engine& eng;
		const local_alias_home& home;
		string_symbolizer& sym;
		using local_info = admission_builder::local_info;
		std::vector<std::unordered_map<uint64_t, local_info>> scopes;

		local_alias_prover(engine& e, const local_alias_home& h) : eng(e), home(h), sym(*engine_access::get_symbolizer(e)) {}

		void declare(uint64_t id, local_info info) { scopes.back()[id] = std::move(info); }
		void declare_decl(variable_decl& d) {
			if (d.name_id == UINT64_MAX) { d.name_id = sym.intern(d.name); }
			local_info info = admission_builder::typed_local(d.type);
			if (d.type && d.type->is_reference()) { info = {}; info.reference = true; info.referent = admission_builder::referent_type(d.type); }
			else if (!d.type && d.initializer && d.initializer->get_type() == node_type::literal_expr &&
			         !static_cast<const literal_expr*>(d.initializer.get())->value.is_null()) { info.primitive = true; }
			declare(d.name_id, info);
		}
		void walk_list(const std::vector<declaration_ptr>& list) {
			scopes.emplace_back();
			for (size_t j = 0; j < list.size(); ++j) {
				statement* st = list[j].get();
				if (st && st->get_type() == node_type::variable_decl) {
					auto* d = static_cast<variable_decl*>(st);
					if (d->alias_home.get() == &home && d->alias_index < home.verdicts.size()) { home.verdicts[d->alias_index] = prove(*d, list, j) ? 1 : 0; }
					declare_decl(*d);
				} else {
					walk(list[j]);
				}
			}
			scopes.pop_back();
		}
		void walk(const statement_ptr& s) {
			if (!s) { return; }
			switch (s->get_type()) {
			case node_type::block_stmt: walk_list(static_cast<block_stmt*>(s.get())->declarations); return;
			case node_type::statement_decl: walk(static_cast<statement_decl*>(s.get())->statement); return;
			case node_type::if_stmt: { auto* n = static_cast<if_stmt*>(s.get()); walk(n->then_statement); walk(n->else_statement); return; }
			case node_type::while_stmt: walk(static_cast<while_stmt*>(s.get())->body); return;
			case node_type::for_stmt: {
				auto* n = static_cast<for_stmt*>(s.get());
				scopes.emplace_back();
				if (n->initializer && n->initializer->get_type() == node_type::variable_decl) { declare_decl(*static_cast<variable_decl*>(n->initializer.get())); }
				walk(n->body);
				scopes.pop_back();
				return;
			}
			case node_type::range_for_stmt: {
				auto* n = static_cast<range_for_stmt*>(s.get());
				if (n->variable_name_id == UINT64_MAX) { n->variable_name_id = sym.intern(n->variable_name); }
				scopes.emplace_back();
				local_info loop_var;
				loop_var.reference = true;
				declare(n->variable_name_id, loop_var);
				walk(n->body);
				scopes.pop_back();
				return;
			}
			case node_type::try_stmt: {
				auto* n = static_cast<try_stmt*>(s.get());
				walk(n->try_block);
				scopes.emplace_back();
				if (!n->catch_var.empty()) { declare(sym.intern(n->catch_var), local_info{}); }
				walk(n->catch_block);
				scopes.pop_back();
				return;
			}
			default:
				return;   // a switch's cases share a scope the walk does not model: their declarations keep the copy
			}
		}
		bool prove(variable_decl& d, const std::vector<declaration_ptr>& list, size_t at) {
			if (d.name_id == UINT64_MAX) { d.name_id = sym.intern(d.name); }
			admission_builder b(eng);
			b.proof_mode = true;
			b.local_mode = true;
			b.prefix = "local proof";
			b.element_symbol = d.name_id;
			b.root_type = d.type;
			b.return_type = home.return_type;
			b.scopes = scopes;
			if (b.type_runs_destructor(d.type)) { return false; }   // the elided copy's destructor would never run
			const expression* init = d.initializer.get();
			const identifier_expr* root = admission_builder::chain_root_identifier(init);
			if (!root || root->symbol_id == UINT64_MAX || root->symbol_id == d.name_id) { return false; }
			// the source sits in a by-value tree: a by-value global or local, reached through by-value steps only
			if (!b.global_rooted(init)) {
				const local_info* l = b.local(root->symbol_id);
				if (!l || l->reference) { return false; }
				b.source_root = root->symbol_id;
			}
			if (!b.chain_by_value(init, true)) { return false; }
			const type_info_ptr source_type = b.proof_chain_type(init);
			const bool same_type = source_type.get() == d.type.get() ||
			                       (admission_builder::class_type(source_type) && admission_builder::class_type(d.type) && source_type->type_name == d.type->type_name);
			if (!same_type) { return false; }
			b.scopes.emplace_back();
			b.declare(d.name_id, admission_builder::typed_local(d.type));
			for (size_t k = at + 1; k < list.size(); ++k) {
				if (!b.walk_stmt(list[k]) || b.failed || b.root_escaped || b.adm.element_mutated) { return false; }
			}
			return !b.failed && !b.root_escaped && !b.adm.element_mutated;
		}
		void run() {
			auto body = home.body.lock();
			if (!body || !home.params) { return; }
			scopes.emplace_back();
			for (const auto& p : *home.params) {
				if (p.symbol_id == UINT64_MAX) { p.symbol_id = sym.intern(p.name); }
				local_info info = admission_builder::typed_local(p.type);
				info.reference = p.is_reference;
				info.param = true;
				declare(p.symbol_id, info);
			}
			walk_list(body->declarations);
		}
	};

	void stamp_local_aliases(const std::shared_ptr<local_alias_home>& home, const statement_ptr& s) {
		if (!s) { return; }
		auto stamp = [&](const statement_ptr& st) { stamp_local_aliases(home, st); };
		switch (s->get_type()) {
		case node_type::block_stmt: for (const auto& d : static_cast<block_stmt*>(s.get())->declarations) { stamp(d); } return;
		case node_type::statement_decl: stamp(static_cast<statement_decl*>(s.get())->statement); return;
		case node_type::if_stmt: { auto* n = static_cast<if_stmt*>(s.get()); stamp(n->then_statement); stamp(n->else_statement); return; }
		case node_type::while_stmt: stamp(static_cast<while_stmt*>(s.get())->body); return;
		case node_type::for_stmt: stamp(static_cast<for_stmt*>(s.get())->body); return;
		case node_type::range_for_stmt: stamp(static_cast<range_for_stmt*>(s.get())->body); return;
		case node_type::try_stmt: { auto* n = static_cast<try_stmt*>(s.get()); stamp(n->try_block); stamp(n->catch_block); return; }
		case node_type::variable_decl: {
			auto* d = static_cast<variable_decl*>(s.get());
			if (local_alias_candidate(*d)) {
				d->alias_home = home;
				d->alias_index = static_cast<uint32_t>(home->verdicts.size());
				home->verdicts.push_back(0);
			}
			return;
		}
		default:
			return;
		}
	}

	bool body_has_local_alias_candidate(const statement_ptr& s) {
		if (!s) { return false; }
		switch (s->get_type()) {
		case node_type::block_stmt:
			for (const auto& d : static_cast<block_stmt*>(s.get())->declarations) { if (body_has_local_alias_candidate(d)) { return true; } }
			return false;
		case node_type::statement_decl: return body_has_local_alias_candidate(static_cast<statement_decl*>(s.get())->statement);
		case node_type::if_stmt: { auto* n = static_cast<if_stmt*>(s.get()); return body_has_local_alias_candidate(n->then_statement) || body_has_local_alias_candidate(n->else_statement); }
		case node_type::while_stmt: return body_has_local_alias_candidate(static_cast<while_stmt*>(s.get())->body);
		case node_type::for_stmt: return body_has_local_alias_candidate(static_cast<for_stmt*>(s.get())->body);
		case node_type::range_for_stmt: return body_has_local_alias_candidate(static_cast<range_for_stmt*>(s.get())->body);
		case node_type::try_stmt: { auto* n = static_cast<try_stmt*>(s.get()); return body_has_local_alias_candidate(n->try_block) || body_has_local_alias_candidate(n->catch_block); }
		case node_type::variable_decl: return local_alias_candidate(*static_cast<variable_decl*>(s.get()));
		default: return false;
		}
	}

	parallel_admission build_admission(engine& eng, const std::shared_ptr<script_defined_function>& fn,
	                                   bool inplace_root = false, const char* prefix = "parallel_for") {
		admission_builder builder(eng);
		if (inplace_root) {
			builder.inplace_root = true;
			builder.adm.inplace_root = true;
			builder.prefix = prefix;
		} else {
			builder.adm.transform_cursor = true;
		}
		if (builder.admit_function(fn, UINT64_MAX,
		                           inplace_root ? admission_builder::root_param::unary
		                                        : admission_builder::root_param::unary_value,
		                           nullptr) && builder.check_index_writes()) {
			builder.finish_const_ref_touches();
			builder.adm.admitted = true;
		}
		return std::move(builder.adm);
	}

	bool admission_graph_current(engine& eng, const parallel_admission& adm) {
		for (const auto& [id, sfn] : adm.script_functions) {
			if (id == UINT64_MAX) { continue; }   // the root fn - caller-supplied, always current
			auto cur = engine_access::get_global_environment(eng)->get(id);
			if (!cur || !cur.value().is_function()) { return false; }
			const script_callable* p = payload_from_function_value(cur.value());
			if (!p || p->kind != script_callable::kind_type::function || !p->fn || p->fn->body != sfn->body) {
				return false;
			}
		}
		return true;
	}

	const parallel_admission* admit_cached(engine& eng, parallel_engine_state& state,
	                                       const std::shared_ptr<script_defined_function>& fn,
	                                       bool inplace_root = false, const char* prefix = "parallel_for") {
		const void* key = fn->body.get();
		auto it = state.admission_cache.find(key);
		if (it != state.admission_cache.end() && admission_graph_current(eng, it->second)) {
			return &it->second;
		}
		auto pos = state.admission_cache.insert_or_assign(key, build_admission(eng, fn, inplace_root, prefix)).first;
		pos->second.generation = ++state.admission_generation;
		return &pos->second;
	}

	// ============================== WORKERS ==============================

	// Re-slice the worker's memory allowance from the CURRENT outer accounting
	void slice_worker_limits(engine& eng, parallel_worker_slot& slot) {
		const auto& outer = engine_access::execution_limits(eng);
		slot.limits.interpreter_call_depth_limit = outer.interpreter_call_depth_limit;
		slot.limits.vm_call_depth_limit = outer.vm_call_depth_limit;
		slot.limits.vm_depth_gate = 0;
		slot.limits.terminal_error = false;
		slot.limits.memory_used = 0;
		slot.limits.memory_raises = 0;
		slot.limits.memory_cap = outer.memory_cap;
		slot.limits.memory_cap_symbol_id = outer.memory_cap_symbol_id;
		slot.limits.memory_limit = outer.memory_limit;
		if (outer.memory_limit != SIZE_MAX) {
			const size_t already = std::min(outer.memory_used, outer.memory_limit);
			slot.limits.memory_limit = outer.memory_limit - already;
		}
	}

	// True when a persisted slot was provisioned for exactly this call shape: same
	// backend type, same fn body, same transitive script bodies under the same names.
	// (Host copies are refreshed on reuse rather than fingerprinted; the admission graph
	// itself was already verified fresh against the live global environment.)
	bool slot_matches(const parallel_worker_slot& slot, const parallel_admission& adm,
	                  const script_defined_function& fn, bool use_vm) {
		if (slot.fingerprint_use_vm != use_vm) { return false; }
		if (slot.fingerprint_fn_body != static_cast<const void*>(fn.body.get())) { return false; }
		if (slot.fingerprint_script_bodies.size() != adm.script_functions.size()) { return false; }
		for (size_t i = 0; i < adm.script_functions.size(); ++i) {
			if (slot.fingerprint_script_bodies[i].first != adm.script_functions[i].first ||
			    slot.fingerprint_script_bodies[i].second != static_cast<const void*>(adm.script_functions[i].second->body.get())) {
				return false;
			}
		}
		return true;
	}

	// Reuse a persisted slot: re-slice limits, re-arm the budget clock, clear residual
	// execution state (a prior call may have ended mid-chunk on an error), mint again the
	// host-function copies whose program-scope binding changed (parallel_host_copy).
	// Provisioned script-function copies stay - that is the point.
	// Main thread, per region entry: prove (or re-validate) each admitted function's read-only
	// parameters on the live function and copy the verdict onto the worker's private copy
	void restamp_alias_proofs(engine& eng, parallel_worker_slot& slot) {
		for (auto& [source, copy] : slot.alias_stamps) {
			if (!source->parameters().empty()) { (void)readonly_parameter_alias(eng, *source, 0, /*may_prove*/ true); }
			copy->alias_proof_epoch = source->alias_proof_epoch;
			copy->alias_proof_mask = source->alias_proof_mask;
		}
	}

	// Main thread: the slot's copy of a host function, minted when the program scope's binding of the name changed since the last one
	script_value parallel_host_copy(engine& eng, parallel_worker_slot& slot, uint64_t name_id, const std::string& name) {
		const script_value* bound = engine_access::get_global_environment(eng)->get_value_ptr(name_id);
		const void* identity = bound && bound->raw_storage_index() == script_value::TYPEID_FUNCTION ? static_cast<const void*>(&bound->unchecked_as_function()) : nullptr;
		parallel_worker_slot::host_copy& cached = slot.host_copies[name_id];
		if (identity && cached.identity == identity && !cached.copy.is_null()) { return cached.copy; }
		script_value copy = engine_access::make_parallel_host_function_copy(eng, name);
		cached.identity = identity;
		cached.source = identity ? *bound : script_value(std::monostate{}, &eng);
		cached.copy = copy;
		return copy;
	}

	void reset_worker_slot(engine& eng, parallel_worker_slot& slot, std::chrono::nanoseconds budget) {
		slice_worker_limits(eng, slot);
		slot.deferred_reports.clear();
		slot.pending_script_layers.clear();
		slot.borrows.clear();
		slot.deferred_releases.clear();
		slot.backend->configure_parallel_worker(slot.root_env, slot.env_epoch_sink.get(), &slot.limits, budget);
		for (const auto& [name_id, name] : slot.host_functions) {
			script_value copy = parallel_host_copy(eng, slot, name_id, name);
			if (slot.vm) { slot.vm->adopt_math_alias(name_id, copy); }
			slot.root_env->define(name_id, std::move(copy));
		}
		slot.error.reset();
	}

	// The chunk loop a worker runs in-script: one host entry per region, zero calls per element
	// in inline mode. __c is the loop variable (slot 0 = the body's own parameter slot; the
	// by-ref form binds the chunk's cursor reference once and follows it, the by-value form
	// copies the cursor's element into __c per iteration), __n the chunk length, __k the
	// counter; __jai_parallel_advance re-points the cursor and yields __k + 1 (an opcode on the
	// vm, a native on the interpreter), so a `continue` in the body reaches it through the
	// for-update. The placeholder call is replaced by the body block; __n/__k are renumbered
	// past the body's frame slots.
	// Built per arity (the zip walks m arrays with m loop variables): params `var& __c0 .. __c{m-1},
	// int __n`; a by-value loop variable that keeps its copy gets `__cj = __jai_parallel_element(j);`
	// at the top of each iteration; inline mode grafts the body in place of the placeholder, call
	// mode calls the body function per element (a `return` inside a nested loop keeps its meaning). The counted form's variable
	// takes `__jai_parallel_counted()`, the element's value of the sequence, by value (no element is stored).
	std::string chunk_skeleton_source(size_t arity, const std::vector<bool>& copy_per_iteration, bool call_mode, bool counted) {
		std::string src = "function __jai_parallel_chunk(";
		for (size_t j = 0; j < arity; ++j) { src += (counted && j == 0 ? "var __c" : "var& __c") + std::to_string(j) + ", "; }
		src += "int __n) { for (int __k = 0; __k < __n; __k = __jai_parallel_advance(__k)) { ";
		if (call_mode) {
			src += "__jai_parallel_body(";
			for (size_t j = 0; j < arity; ++j) { src += (j ? ", " : "") + (counted && j == 0 ? std::string("__jai_parallel_counted()") : "__c" + std::to_string(j)); }
			src += "); ";
		} else {
			for (size_t j = 0; j < arity; ++j) {
				if (counted && j == 0) { src += "__c0 = __jai_parallel_counted(); "; continue; }
				if (copy_per_iteration[j]) { src += "__c" + std::to_string(j) + " = __jai_parallel_element(" + std::to_string(j) + "); "; }
			}
			src += "__jai_parallel_placeholder(); ";
		}
		src += "} }";
		return src;
	}

	// parallel_transform's chunk loop = the zip `o = fn(x)`: fn per element in-loop (its by-value
	// parameter binds the element in place when the admission proved it read-only, else copies as
	// it always did), the call result a temporary that moves into the output slot through the
	// second cursor - nothing else per element
	constexpr const char* k_parallel_chunk_transform =
		"function __jai_parallel_tchunk(var& __c0, var& __c1, int __n) { for (int __k = 0; __k < __n; __k = __jai_parallel_advance(__k)) { __c1 = __jai_parallel_fn(__c0); } }";

	std::shared_ptr<function_decl> parse_chunk_skeleton(engine& eng, const char* source) {
		const auto templates = engine_access::get_registered_template_types(eng);
		lexer lex(source, engine_access::get_symbolizer(eng));
		auto tokens = lex.tokenize();
		parser parse(tokens, engine_access::get_symbolizer(eng), &eng, templates, "<parallel_for>");
		auto decls = parse.parse();
		if (!decls || decls.value().empty() || decls.value().front()->get_type() != node_type::function_decl) {
			throw runtime_error("parallel_for: chunk wrapper failed to parse (internal)");
		}
		return std::static_pointer_cast<function_decl>(decls.value().front());
	}

	// Renumber the skeleton's own slots (__n, __k) past the body's frame and rename __cj to loop
	// parameter j (its permanent name and symbol; the vm resolves frame locals by declared name)
	struct chunk_names {
		std::vector<std::pair<std::string_view, uint64_t>> params;   // (name, symbol) per loop variable
		std::vector<size_t> param_slots;                             // empty: loop variable j sits in slot j
		size_t n_slot = 0, k_slot = 0;
		size_t slot_of(size_t j) const { return j < param_slots.size() ? param_slots[j] : j; }
	};
	void renumber_chunk_slot(expression_ptr& e, const chunk_names& names) {
		if (!e) { return; }
		switch (e->get_type()) {
		case node_type::identifier_expr: {
			auto* id = static_cast<identifier_expr*>(e.get());
			if (id->name == "__n") { id->slot_index = names.n_slot; }
			else if (id->name == "__k") { id->slot_index = names.k_slot; }
			else if (id->name.size() > 3 && id->name.substr(0, 3) == "__c") {
				const size_t j = static_cast<size_t>(std::stoul(std::string(id->name.substr(3))));
				if (j < names.params.size()) { id->name = names.params[j].first; id->symbol_id = names.params[j].second; id->slot_index = names.slot_of(j); }
			}
			return;
		}
		case node_type::binary_expr: {
			auto* b = static_cast<binary_expr*>(e.get());
			renumber_chunk_slot(b->left, names); renumber_chunk_slot(b->right, names);
			return;
		}
		case node_type::assignment_expr: {
			auto* a = static_cast<assignment_expr*>(e.get());
			renumber_chunk_slot(a->target, names); renumber_chunk_slot(a->value, names);
			return;
		}
		case node_type::call_expr: {
			auto* c = static_cast<call_expr*>(e.get());
			for (auto& arg : c->arguments) { renumber_chunk_slot(arg, names); }
			return;
		}
		default:
			return;
		}
	}

	// Builds (once per statement) the chunk wrapper: skeleton by mode, the loop parameter
	// itself as parameter 0 (the body's slot 0), __n/__k past the body's frame, the body block
	// grafted in place of the placeholder (inline modes).
	const std::shared_ptr<function_decl>& build_chunk_wrapper(engine& eng, parallel_for_stmt* stmt, const parallel_admission& adm) {
		if (stmt->chunk_decl_cache) { return stmt->chunk_decl_cache; }
		const bool inline_mode = stmt->chunk_mode == 1;
		std::vector<parameter> loop_params;
		loop_params.push_back(stmt->loop_param);
		for (const auto& extra : stmt->extra_params) { loop_params.push_back(extra); }
		const size_t arity = loop_params.size();
		// A by-value loop variable the body only reads binds its element in place (same reads,
		// no per-element copy); one it writes or uses whole keeps the copy
		const bool reads_only = !adm.element_reach_all && !adm.element_mutated;
		std::vector<bool> copy_per_iteration(arity, false);
		for (size_t j = 0; j < arity; ++j) { copy_per_iteration[j] = !loop_params[j].is_reference && !reads_only; }
		const bool counted = stmt->counted();
		auto decl = parse_chunk_skeleton(eng, chunk_skeleton_source(arity, copy_per_iteration, !inline_mode, counted).c_str());
		chunk_names names;
		names.n_slot = stmt->local_count; names.k_slot = stmt->local_count + 1;
		for (size_t j = 0; j < arity; ++j) {
			parameter lp = loop_params[j];
			lp.slot_index = j;
			if (lp.symbol_id == UINT64_MAX) { lp.symbol_id = engine_access::symbolize(eng, lp.name); }
			if (inline_mode) { lp.is_reference = !(counted && j == 0) && (lp.is_reference || reads_only); }
			names.params.emplace_back(engine_access::get_symbolizer(eng)->get_string(lp.symbol_id), lp.symbol_id);
			if (inline_mode) { decl->parameters[j] = lp; }
		}
		if (inline_mode) {
			decl->parameters[arity].slot_index = names.n_slot;
			auto* body_block = static_cast<block_stmt*>(decl->body.get());
			for (auto& d : body_block->declarations) {
				if (d->get_type() != node_type::statement_decl) { continue; }
				auto& st = static_cast<statement_decl*>(d.get())->statement;
				if (!st || st->get_type() != node_type::for_stmt) { continue; }
				auto* loop = static_cast<for_stmt*>(st.get());
				if (loop->initializer && loop->initializer->get_type() == node_type::variable_decl) {
					static_cast<variable_decl*>(loop->initializer.get())->slot_index = names.k_slot;
				}
				renumber_chunk_slot(loop->condition, names);
				renumber_chunk_slot(loop->update, names);
				auto* inner = static_cast<block_stmt*>(loop->body.get());
				for (auto& bd : inner->declarations) {
					if (bd->get_type() != node_type::expression_decl) { continue; }
					auto* ed = static_cast<expression_decl*>(bd.get());
					if (ed->expression && ed->expression->get_type() == node_type::assignment_expr) {
						renumber_chunk_slot(ed->expression, names);   // `__cj = __jai_parallel_element(j)` -> the loop variable
						continue;
					}
					if (ed->expression && ed->expression->get_type() == node_type::call_expr) {
						auto* call = static_cast<call_expr*>(ed->expression.get());
						if (call->callee->get_type() == node_type::identifier_expr &&
						    static_cast<identifier_expr*>(call->callee.get())->name == "__jai_parallel_placeholder") {
							bd = std::make_shared<statement_decl>(bd->location, stmt->body);
						}
					}
				}
			}
			decl->local_count = stmt->local_count + 2;
		}
		stmt->chunk_decl_cache = std::move(decl);
		return stmt->chunk_decl_cache;
	}

	// An exact deep copy of a statement subtree (the serializer knows every node): fn's body is
	// shared with its ordinary callers, so the graft below works on a copy
	statement_ptr clone_statement(engine& eng, const statement_ptr& stmt, const script_defined_function& fn) {
		std::vector<declaration_ptr> decls;
		decls.push_back(std::make_shared<statement_decl>(stmt->location, stmt));
		const std::vector<uint8_t> bytes = ast_writer(*engine_access::get_symbolizer(eng)).in_body().serialize(decls, 0);
		uint64_t fingerprint = 0;
		const ast_frame_context context{fn.local_count, fn.parameters()};
		auto back = deserialize_jaibite(bytes.data(), bytes.size(), &eng, fingerprint, nullptr, &context);
		if (back.size() != 1 || back[0]->get_type() != node_type::statement_decl) {
			throw runtime_error("parallel_transform: body clone failed (internal)");
		}
		return static_cast<statement_decl*>(back[0].get())->statement;
	}

	// Top-level `return e;` of the grafted fn body becomes `out = e; continue;` (`return;` just
	// continues: the output slot keeps its null) - the continue reaches the loop's advance. fn's
	// declared return type converts through cast<T>, the conversion the typed return applied.
	struct transform_return_ctx {
		engine* eng = nullptr;
		std::string_view out_name;
		uint64_t out_symbol = UINT64_MAX;
		size_t out_slot = 0;
		type_info_ptr cast_to;   // null: no conversion (auto / var / reference return)
		std::string_view cast_owner;   // the function whose return conversion and text cast_to stands for
		// The checker's return proofs read off the ORIGINAL body in traversal order (the clone
		// carries no runtime stamps; both trees have the same shape): a proven same-type
		// return needs no cast, exactly as the typed return skipped its conversion
		std::vector<uint8_t> return_proofs;
		size_t next_return = 0;
		bool discard_value = false;   // the value is evaluated and dropped: nothing is stored
	};
	void collect_return_proofs(const statement_ptr& slot, std::vector<uint8_t>& out) {
		if (!slot) { return; }
		switch (slot->get_type()) {
		case node_type::return_stmt:
			out.push_back(static_cast<const return_stmt*>(slot.get())->static_return_proof);
			return;
		case node_type::block_stmt:
			for (auto& d : static_cast<block_stmt*>(slot.get())->declarations) {
				if (d && d->get_type() == node_type::statement_decl) { collect_return_proofs(static_cast<statement_decl*>(d.get())->statement, out); }
			}
			return;
		case node_type::if_stmt: {
			auto* n = static_cast<if_stmt*>(slot.get());
			collect_return_proofs(n->then_statement, out); collect_return_proofs(n->else_statement, out);
			return;
		}
		case node_type::while_stmt: collect_return_proofs(static_cast<while_stmt*>(slot.get())->body, out); return;
		case node_type::for_stmt: collect_return_proofs(static_cast<for_stmt*>(slot.get())->body, out); return;
		case node_type::range_for_stmt: collect_return_proofs(static_cast<range_for_stmt*>(slot.get())->body, out); return;
		case node_type::try_stmt: {
			auto* n = static_cast<try_stmt*>(slot.get());
			collect_return_proofs(n->try_block, out); collect_return_proofs(n->catch_block, out);
			return;
		}
		case node_type::switch_stmt: {
			auto* n = static_cast<switch_stmt*>(slot.get());
			for (auto& c : n->cases) { for (auto& st : c->body) { collect_return_proofs(st, out); } }
			if (n->default_case) { for (auto& st : n->default_case->body) { collect_return_proofs(st, out); } }
			return;
		}
		default:
			return;
		}
	}
	void rewrite_transform_returns(statement_ptr& slot, size_t loop_depth, transform_return_ctx& ctx) {
		if (!slot) { return; }
		switch (slot->get_type()) {
		case node_type::return_stmt: {
			if (loop_depth != 0) { ++ctx.next_return; return; }   // never reached: nested returns select call mode
			auto* r = static_cast<return_stmt*>(slot.get());
			const uint8_t proof = ctx.next_return < ctx.return_proofs.size() ? ctx.return_proofs[ctx.next_return] : proof_none;
			++ctx.next_return;
			std::vector<declaration_ptr> decls;
			if (r->value && ctx.discard_value) {
				if (r->value->get_type() != node_type::identifier_expr) {   // `return acc;` names what the element already wrote
					decls.push_back(std::make_shared<expression_decl>(r->location, r->value));
				}
			} else if (r->value) {
				auto target = std::make_shared<identifier_expr>(r->location, ctx.out_name, ctx.out_symbol);
				target->slot_index = ctx.out_slot;
				expression_ptr value = r->value;
				if (ctx.cast_to && proof != proof_same_type) {
					auto converted = std::make_shared<cast_expr>(r->location, ctx.cast_to, false, value);
					converted->return_of = ctx.cast_owner;
					value = std::move(converted);
				}
				token eq(token_type::equal, std::string_view("="), r->location);
				decls.push_back(std::make_shared<expression_decl>(r->location, std::make_shared<assignment_expr>(r->location, target, eq, value)));
			}
			decls.push_back(std::make_shared<statement_decl>(r->location, std::make_shared<continue_stmt>(r->location)));
			slot = std::make_shared<block_stmt>(r->location, std::move(decls));
			return;
		}
		case node_type::block_stmt:
			for (auto& d : static_cast<block_stmt*>(slot.get())->declarations) {
				if (d && d->get_type() == node_type::statement_decl) {
					rewrite_transform_returns(static_cast<statement_decl*>(d.get())->statement, loop_depth, ctx);
				}
			}
			return;
		case node_type::if_stmt: {
			auto* n = static_cast<if_stmt*>(slot.get());
			rewrite_transform_returns(n->then_statement, loop_depth, ctx);
			rewrite_transform_returns(n->else_statement, loop_depth, ctx);
			return;
		}
		case node_type::while_stmt:
			rewrite_transform_returns(static_cast<while_stmt*>(slot.get())->body, loop_depth + 1, ctx); return;
		case node_type::for_stmt:
			rewrite_transform_returns(static_cast<for_stmt*>(slot.get())->body, loop_depth + 1, ctx); return;
		case node_type::range_for_stmt:
			rewrite_transform_returns(static_cast<range_for_stmt*>(slot.get())->body, loop_depth + 1, ctx); return;
		case node_type::try_stmt: {
			auto* n = static_cast<try_stmt*>(slot.get());
			rewrite_transform_returns(n->try_block, loop_depth, ctx);
			rewrite_transform_returns(n->catch_block, loop_depth, ctx);
			return;
		}
		case node_type::switch_stmt: {
			auto* n = static_cast<switch_stmt*>(slot.get());
			for (auto& c : n->cases) { for (auto& st : c->body) { rewrite_transform_returns(st, loop_depth, ctx); } }
			if (n->default_case) { for (auto& st : n->default_case->body) { rewrite_transform_returns(st, loop_depth, ctx); } }
			return;
		}
		default:
			return;
		}
	}

	// The transform's chunk wrapper for fn: fn's body grafted in - its parameter bound to the input
	// cursor in place, its returns stored through the output cursor - when fn only reads its
	// parameter (no store through it, no mutating method, no whole use: the copy it would have
	// made is unobservable) and returns only from its top level; otherwise the call form, which
	// copies for a mutating fn exactly as before. Built once per fn body.
	constexpr const char* k_parallel_chunk_transform_inline =
		"function __jai_parallel_tchunk(var& __c0, var& __c1, int __n) { for (int __k = 0; __k < __n; __k = __jai_parallel_advance(__k)) { __jai_parallel_placeholder(); } }";

	const std::shared_ptr<function_decl>& build_transform_wrapper(engine& eng, parallel_engine_state& state,
	                                                              const std::shared_ptr<script_defined_function>& fn,
	                                                              const parallel_admission& adm) {
		auto found = state.transform_inline_wrappers.find(static_cast<const void*>(fn->body.get()));
		if (found != state.transform_inline_wrappers.end()) { return found->second; }
		body_control_scan scan;
		statement_ptr body_view = fn->body;
		scan_parallel_body_control(body_view, 0, scan);
		const bool reads_only = !adm.element_reach_all && !adm.element_mutated;
		const bool inline_ok = reads_only && !scan.nested_return && fn->parameters().size() == 1;
		if (!inline_ok) {
			if (!state.transform_chunk_wrapper) { state.transform_chunk_wrapper = parse_chunk_skeleton(eng, k_parallel_chunk_transform); }
			return state.transform_inline_wrappers.emplace(static_cast<const void*>(fn->body.get()), state.transform_chunk_wrapper).first->second;
		}
		auto decl = parse_chunk_skeleton(eng, k_parallel_chunk_transform_inline);
		const size_t n_locals = fn->local_count;   // fn's frame: parameter 0 + its locals
		parameter in_param = fn->parameters()[0];
		in_param.slot_index = 0;
		in_param.is_reference = true;   // in place: the proof above makes the copy unobservable
		if (in_param.symbol_id == UINT64_MAX) { in_param.symbol_id = engine_access::symbolize(eng, in_param.name); }
		chunk_names names;
		names.params.emplace_back(engine_access::get_symbolizer(eng)->get_string(in_param.symbol_id), in_param.symbol_id);
		const uint64_t out_symbol = engine_access::symbolize(eng, "__c1");
		names.params.emplace_back(engine_access::get_symbolizer(eng)->get_string(out_symbol), out_symbol);
		names.param_slots = { 0, n_locals };
		names.n_slot = n_locals + 1; names.k_slot = n_locals + 2;
		decl->parameters[0] = in_param;
		decl->parameters[1].slot_index = n_locals;
		decl->parameters[2].slot_index = names.n_slot;
		transform_return_ctx rctx;
		rctx.eng = &eng;
		rctx.out_name = names.params[1].first;
		rctx.out_symbol = out_symbol;
		rctx.out_slot = n_locals;
		const return_conv conv = classify_return_conv(fn->return_type.get());
		if (conv != return_conv::none && conv != return_conv::ref) { rctx.cast_to = fn->return_type; rctx.cast_owner = fn->name; }
		collect_return_proofs(body_view, rctx.return_proofs);
		statement_ptr grafted = clone_statement(eng, body_view, *fn);
		rewrite_transform_returns(grafted, 0, rctx);
		auto* body_block = static_cast<block_stmt*>(decl->body.get());
		for (auto& d : body_block->declarations) {
			if (d->get_type() != node_type::statement_decl) { continue; }
			auto& st = static_cast<statement_decl*>(d.get())->statement;
			if (!st || st->get_type() != node_type::for_stmt) { continue; }
			auto* loop = static_cast<for_stmt*>(st.get());
			if (loop->initializer && loop->initializer->get_type() == node_type::variable_decl) {
				static_cast<variable_decl*>(loop->initializer.get())->slot_index = names.k_slot;
			}
			renumber_chunk_slot(loop->condition, names);
			renumber_chunk_slot(loop->update, names);
			auto* inner = static_cast<block_stmt*>(loop->body.get());
			for (auto& bd : inner->declarations) {
				if (bd->get_type() != node_type::expression_decl) { continue; }
				auto* ed = static_cast<expression_decl*>(bd.get());
				if (ed->expression && ed->expression->get_type() == node_type::call_expr) {
					auto* call = static_cast<call_expr*>(ed->expression.get());
					if (call->callee->get_type() == node_type::identifier_expr &&
					    static_cast<identifier_expr*>(call->callee.get())->name == "__jai_parallel_placeholder") {
						bd = std::make_shared<statement_decl>(bd->location, grafted);
					}
				}
			}
		}
		decl->local_count = n_locals + 3;
		return state.transform_inline_wrappers.emplace(static_cast<const void*>(fn->body.get()), std::move(decl)).first->second;
	}

	std::unique_ptr<parallel_worker_slot> provision_worker(engine& eng, const parallel_admission& adm,
	                                                 std::chrono::nanoseconds budget,
	                                                 bool use_vm,
	                                                 const std::shared_ptr<function_decl>* chunk_wrapper = nullptr) {
		auto ctx = std::make_unique<parallel_worker_slot>();
		ctx->env_epoch_sink = std::make_unique<string_symbolizer>();
		ctx->root_env = std::make_shared<environment>(ctx->env_epoch_sink.get());
		ctx->method_env = std::make_shared<environment>(ctx->env_epoch_sink.get());

		slice_worker_limits(eng, *ctx);

		if (use_vm) {
			auto backend = std::make_unique<vm::vm_backend>(engine_access::get_symbolizer(eng), ctx->root_env);
			ctx->vm = backend.get();
			ctx->backend = std::move(backend);
		} else {
			ctx->backend = detail::make_interpreter_backend(engine_access::get_symbolizer(eng), ctx->root_env);
		}
		ctx->backend->set_engine_reference(&eng);
		ctx->backend->configure_parallel_worker(ctx->root_env, ctx->env_epoch_sink.get(), &ctx->limits, budget);

		// Provisioning fingerprint for reuse decisions on later calls
		ctx->fingerprint_use_vm = use_vm;
		ctx->fingerprint_script_bodies.reserve(adm.script_functions.size());
		for (const auto& [name_id, source_fn] : adm.script_functions) {
			ctx->fingerprint_script_bodies.emplace_back(name_id, static_cast<const void*>(source_fn->body.get()));
		}
		if (!adm.script_functions.empty()) {
			ctx->fingerprint_fn_body = static_cast<const void*>(adm.script_functions.front().second->body.get());
		}
		ctx->host_functions = adm.host_functions;

		// Per-worker copies of every callable the body needs: fresh script_defined_function
		// objects (own parameters vector, own backend_body_cache, closure_env = the worker
		// root) minted as script_callable_thunks - the backends' call paths RECOVER the
		// payload (target<script_callable_thunk>) and dispatch in-loop on the CALLING
		// worker's own backend, exactly like any script function. (The old anonymous
		// lambda was opaque: every worker call crossed the from-scratch native boundary,
		// ~8.3k cycles per element vs an in-loop frame push - 62% of worker time in the
		// particles region.) The thunk's operator() fallback would route to the ENGINE's
		// backend - unreachable from admitted bodies: only direct calls to statically-
		// known functions are admitted, and function VALUES cannot be captured or stored.
		bool first_entry = true;
		for (const auto& [name_id, source_fn] : adm.script_functions) {
			// Deep parameter copy ON PURPOSE: parameter's mutable symbol_id/slot_index
			// lazy fills stay worker-private (the shared-storage mints are main-thread)
			std::vector<parameter> params(source_fn->parameters());
			if (first_entry && adm.transform_cursor && !params.empty() && !params[0].is_reference &&
			    !adm.element_reach_all && !adm.element_mutated) {
				params[0].is_reference = true;   // read-only by-value element: bind the chunk's element in place, no copy
			}
			auto copy = std::make_shared<script_defined_function>(
				source_fn->name, std::move(params), source_fn->return_type,
				source_fn->body, ctx->root_env, source_fn->local_count);
			copy->unit = source_fn->unit;
			copy->alias_candidate = source_fn->alias_candidate;
			if (copy->alias_candidate) { ctx->alias_stamps.emplace_back(source_fn, copy); }
			if (ctx->vm) { ctx->vm->precompile_parallel_function(*copy); }
			script_callable payload;
			payload.kind = script_callable::kind_type::function;
			payload.fn = copy;
			if (first_entry) {
				ctx->fn_payload = payload;   // the root fn is always pushed first
				first_entry = false;
			}
			if (name_id != UINT64_MAX) {
				ctx->root_env->define(name_id,
					script_value::make_function(script_callable_thunk{ &eng, payload }, &eng));
			}
		}
		for (const auto& [name_id, name] : adm.host_functions) {
			script_value copy = parallel_host_copy(eng, *ctx, name_id, name);
			if (ctx->vm) { ctx->vm->adopt_math_alias(name_id, copy); }
			ctx->root_env->define(name_id, std::move(copy));
		}
		if (chunk_wrapper && *chunk_wrapper) {
			const std::shared_ptr<function_decl>& wrapper = *chunk_wrapper;
			ctx->root_env->define(engine_access::symbolize(eng, adm.inplace_root ? "__jai_parallel_body" : "__jai_parallel_fn"),
				script_value::make_function(script_callable_thunk{ &eng, ctx->fn_payload }, &eng));
			parallel_worker_slot* slot = ctx.get();   // slots live in the pool's unique_ptrs: stable
			// The interpreter calls these natives; the vm compiles the names to opcodes
			ctx->root_env->define(engine_access::symbolize(eng, "__jai_parallel_advance"),
				script_value::make_function([slot, eng_ptr = &eng](const std::vector<script_value>& args) -> checked_result<script_value> {
					if (slot->sched.schedule) {
						if (!slot->sched.advance()) { return script_value(static_cast<script_int>(slot->sched.schedule->n), eng_ptr); }
						for (auto& cursor : slot->cursors) { cursor.element_cursor_set(slot->sched.index); }
					} else {
						for (auto& cursor : slot->cursors) { cursor.element_cursor_advance(slot->block, slot->stride_blocks); }
					}
					const script_int k = args.empty() ? 0 : args[0].deref().as_int();
					return script_value(k + 1, eng_ptr);
				}, &eng));
			ctx->root_env->define(engine_access::symbolize(eng, "__jai_parallel_element"),
				script_value::make_function([slot, eng_ptr = &eng](const std::vector<script_value>& args) -> checked_result<script_value> {
					const size_t which = args.empty() ? 0 : static_cast<size_t>(args[0].deref().as_int());
					if (which >= slot->cursors.size()) { return script_value(std::monostate{}, eng_ptr); }
					return slot->cursors[which];
				}, &eng));
			ctx->root_env->define(engine_access::symbolize(eng, "__jai_parallel_counted"),
				script_value::make_function([slot, eng_ptr = &eng](const std::vector<script_value>&) -> checked_result<script_value> {
					if (!slot->counted || slot->cursors.empty()) { return script_value(std::monostate{}, eng_ptr); }
					return script_value(static_cast<script_int>(slot->counted->at(slot->cursors.front().element_cursor_index())), eng_ptr);
				}, &eng));
			auto chunk_fn = std::make_shared<script_defined_function>(
				wrapper->name, std::vector<parameter>(wrapper->parameters), wrapper->return_type,
				wrapper->body, ctx->root_env, wrapper->local_count);
			if (ctx->vm) { ctx->vm->precompile_parallel_function(*chunk_fn); }
			ctx->chunk_payload.kind = script_callable::kind_type::function;
			ctx->chunk_payload.fn = chunk_fn;
		}
		return ctx;
	}

	// In-place worker loop (parallel_for): by-ref bodies bind each iteration to the
	// pre-minted element reference - the body mutates the caller's element directly
	// under chunk exclusivity. Plain-auto bodies read the element through raw_input
	// (the by-value binding clones per value semantics - zero side effects). No
	// output, results discarded. Every refcount touched is element-internal
	// (normalization made elements exclusive) or worker-private: single-threaded.
	void run_worker_chunk(parallel_worker_slot& ctx, engine& eng) {
		if (ctx.count == 0 || ctx.element_refs.empty()) { return; }
		// ONE script entry per chunk: the wrapper loops in-script, calling the body per element
		// with the chunk's cursor reference and advancing it through the worker's intrinsic. The
		// failing element, when any, is the cursor's index (the advance runs after a body call).
		ctx.cursors = std::move(ctx.element_refs);   // [0] = the range, then the zip's arrays
		ctx.element_refs.clear();
		if (ctx.vm) { ctx.vm->set_parallel_cursors(&ctx.cursors, ctx.block, ctx.stride_blocks, ctx.sched.schedule ? &ctx.sched : nullptr, ctx.counted ? &*ctx.counted : nullptr); }
		struct cursor_reset {
			parallel_worker_slot& c;
			~cursor_reset() {
				if (c.vm) {
					c.vm->set_parallel_cursors(nullptr);
					c.vm->release_region_strings();   // published nodes belong to the main thread from here
				}
			}
		} cursor_guard{ ctx };
		auto failing = [&ctx]() { const size_t i = ctx.cursors.front().element_cursor_index(); return i == SIZE_MAX ? ctx.begin : std::min(i, ctx.end - 1); };
		// The outer catch holds what escapes the backend, or the inner handler's own allocation: a pool
		// worker's chunk has no caller to throw to, so it fails the region in iteration order like any error
		try {
			try {
				auto r = ctx.backend->execute_callable(ctx.chunk_payload, ctx.call_args);
				if (ctx.backend->is_unwinding()) {
					ctx.error = parallel_worker_error{ failing(),
						make_error_code(runtime_error_code::evaluation_failed),
						ctx.backend->get_current_exception().what(),
						ctx.limits.terminal_error, {} };
				} else if (!r) {
					ctx.error = parallel_worker_error{ failing(), r.error(),
						format_error(r, *engine_access::get_symbolizer(eng)), ctx.limits.terminal_error, {} };
				}
			} catch (const std::exception& e) {
				ctx.error = parallel_worker_error{ failing(), make_error_code(runtime_error_code::cpp_exception),
					e.what(), ctx.limits.terminal_error, {} };
			}
		} catch (...) {
			ctx.error = parallel_worker_error{ failing(), make_error_code(runtime_error_code::cpp_exception),
				std::string(), ctx.limits.terminal_error, std::current_exception() };
		}
	}

	// ============================== BARRIER HELPERS ==============================

	// A value that holds an object somewhere (an object has no detached copy)
	bool holds_objects(const script_value& v) {
		const script_value& value = v.deref();
		switch (value.raw_storage_index()) {
		case script_value::TYPEID_ARRAY: {
			const script_array* node = value.unchecked_array_node();
			if (node->kind() == script_array::kind_t::obj) { return true; }
			if (node->is_typed()) { return false; }
			for (const auto& element : node->values()) {
				if (holds_objects(element)) { return true; }
			}
			return false;
		}
		case script_value::TYPEID_MAP: {
			bool found = false;
			for (const auto& [key, element] : value.unchecked_as_map()) { found = found || holds_objects(key) || holds_objects(element); }
			return found;
		}
		case script_value::TYPEID_NULL: case script_value::TYPEID_INT: case script_value::TYPEID_FLOAT: case script_value::TYPEID_CHAR:
		case script_value::TYPEID_BOOL: case script_value::TYPEID_STRING:
			return false;
		default:
			return true;
		}
	}

	// A worker's copy of an index-written array of objects shares the live slots (proven exclusive at the barrier): the body touches
	// out[i] alone, so each slot's objects are touched on one thread, the slot's owner during the region and main at the join
	script_value shared_slot_copy(const script_value& source, engine& eng) {
		const script_array* node = source.unchecked_array_node();
		const type_info_ptr type = source.get_type_info();
		script_value result = script_value::make_array(type ? type->element_type() : type_info_ptr(nullptr), &eng);
		*result.unchecked_get_array_storage() = *node;
		if (type) { result.set_type_info(type); }
		return result;
	}

	// The memos a construction fills, filled before the region freezes: the class's own and each script level's literal-chain
	// verdict
	void prime_construction_memos(class_definition& cls) {
		cls.prime_region_memos();
		for (const class_definition* level : cls.construction_order()) {
			if (!level || !level->is_script_class()) { continue; }
			(void)static_cast<const script_class_definition*>(level)->chain_initializers_all_literal();
		}
	}

	// Interns every shape a worker can mint from the seed before the tables freeze: each
	// type's T& twin and container components, and - when a type names a registered script
	// class - the class's field ladder (declared types, accessor ids) plus its registered
	// subclasses (a nested typed array's slot tag is whatever subclass sits there), so a class
	// reached only through a typed container warms exactly like the element's own. Memoized
	// per fork; deny_type_intern_in_region is the backstop for a shape this misses.
	void prewarm_region_types(engine& eng, const std::vector<type_info*>& seed) {
		auto& sym = *engine_access::get_symbolizer(eng);
		(void)sym.intern("pair");   // map range-for pair name (hit for stdlib engines)
		auto& registry = engine_access::get_class_registry(eng);
		std::vector<type_info*> pending(seed);
		pending.push_back(engine_access::get_type_info_int(eng));
		pending.push_back(engine_access::get_type_info_float(eng));
		pending.push_back(engine_access::get_type_info_string(eng));
		pending.push_back(engine_access::get_type_info_bool(eng));
		pending.push_back(engine_access::get_type_info_char(eng));
		pending.push_back(engine_access::get_type_info_void(eng));
		pending.push_back(engine_access::get_type_info_array(eng, nullptr));
		pending.push_back(engine_access::get_type_info_map(eng, nullptr, nullptr));
		engine_access::get_type_info_reference(eng, nullptr);
		std::unordered_set<type_info*> seen;
		std::vector<const class_definition*> classes;
		std::vector<const class_definition*> pending_classes;
		while (!pending.empty() || !pending_classes.empty()) {
			if (!pending_classes.empty()) {
				const class_definition* cd = pending_classes.back();
				pending_classes.pop_back();
				if (!cd || std::find(classes.begin(), classes.end(), cd) != classes.end()) { continue; }
				classes.push_back(cd);
				pending.push_back(cd->get_type_info().get());
				const uint32_t n = cd->field_slot_count();
				for (uint32_t slot = 0; slot < n; ++slot) {
					const uint64_t field_id = cd->slot_field(slot);
					(void)sym.get_getter_id_with_view(field_id);
					(void)sym.get_setter_id_with_view(field_id);
					pending.push_back(cd->get_field_declared_type(field_id).get());
				}
				for (const auto& [name, def] : registry.script_classes_) {
					(void)name;
					if (def && def.get() != cd && def->is_subtype_of(cd->get_name())) { pending_classes.push_back(def.get()); }
				}
				continue;
			}
			type_info* t = pending.back();
			pending.pop_back();
			if (!t || !seen.insert(t).second) { continue; }
			engine_access::get_type_info_reference(eng, t);
			// a var declaration takes a handle's rebindable twin and a typed container's dynamic twin; auto and typed
			// declarations take the plain spelling back
			if (t->base_type == script_value_type::jai_shared_ptr_type && t->element_type()) {
				type_info* pointee = t->element_type().get();
				pending.push_back(t->dynamic_pointee ? engine_access::get_type_info_shared_ptr(eng, pointee) : engine_access::get_type_info_shared_ptr_dynamic(eng, pointee));
			} else if (t->base_type == script_value_type::jai_weak_ptr_type && t->element_type()) {
				pending.push_back(engine_access::get_type_info_shared_ptr(eng, t->element_type().get()));   // lock()
			} else if (t->base_type == script_value_type::jai_array_type) {
				pending.push_back(t->dynamic_slot ? engine_access::get_type_info_array(eng, t->element_type().get()) : engine_access::get_type_info_array_dynamic(eng, t));
			} else if (t->base_type == script_value_type::jai_map_type) {
				pending.push_back(t->dynamic_slot ? engine_access::get_type_info_map(eng, t->key_type().get(), t->value_type().get()) : engine_access::get_type_info_map_dynamic(eng, t));
			}
			pending.push_back(t->element_type().get());
			pending.push_back(t->key_type().get());
			pending.push_back(t->value_type().get());
			if (t->base_type == script_value_type::jai_object_type && !t->type_name.empty()) {
				pending_classes.push_back(registry.find_script_class(std::string(t->type_name)).get());
			}
		}
	}

	// The classes a body can reach when the barrier holds no slot tag (a trusted or untyped
	// source): every registered script class declaring a member the body names on an
	// element-rooted chain seeds the prewarm (over-approximate, warming is free).
	void seed_classes_by_member(engine& eng, const std::vector<uint64_t>& members, std::vector<type_info*>& seed) {
		if (members.empty()) { return; }
		for (const auto& [name, def] : engine_access::get_class_registry(eng).script_classes_) {
			(void)name;
			if (!def) { continue; }
			for (uint64_t id : members) {
				if (def->chain_declares_field(id)) { seed.push_back(def->get_type_info().get()); break; }
			}
		}
	}

	// STATIC all-primitive proof ("safety AND free"): a TYPED
	// container's element types are store-enforced invariants, so array<int> etc. is
	// all-primitive by construction - no O(n) content scan needed, ever. Untyped
	// containers fall back to the dynamic scan below.
	// Declared-type predicates. `with_strings` is the BORROW tier's question (a borrowed string
	// read mints the bytes, never the handle, so a string table reads raw); the element PROOF
	// asks the scalar question: a string element or key is a refcounted handle that two slots,
	// or every instance built from one literal, can share, and only the detach walk makes it
	// exclusive before workers copy it
	bool statically_primitive_shape(const script_value& v, bool with_strings) {
		const type_info_ptr t = v.get_type_info();
		if (!t) {
			return false;
		}
		auto primitive_type = [with_strings](const type_info_ptr& p) {
			if (!p) return false;
			switch (p->base_type) {
				case script_value_type::jai_int_type:
				case script_value_type::jai_float_type:
				case script_value_type::jai_bool_type:
				case script_value_type::jai_char_type:
					return true;
				case script_value_type::jai_string_type:
					return with_strings;
				default:
					return false;
			}
		};
		if (t->base_type == script_value_type::jai_array_type) {
			return primitive_type(t->element_type());
		}
		if (t->base_type == script_value_type::jai_map_type) {
			return primitive_type(t->key_type()) && primitive_type(t->value_type());
		}
		return false;
	}

	bool statically_all_primitive(const script_value& v) { return statically_primitive_shape(v, /*with_strings*/ true); }
	bool statically_all_scalar(const script_value& v) { return statically_primitive_shape(v, /*with_strings*/ false); }

	// A typed string buffer's slots must each own their string before workers copy them: a
	// handle two slots share (a[1] = a[0], a literal pushed in a loop) would take non-atomic
	// count traffic from two threads. Main thread, at the barrier.
	void detach_shared_typed_strings(script_array& node, engine& eng) {
		if (!node.is_typed() || node.kind() != script_array::kind_t::str) { return; }
		for (auto& handle : node.strs()) {
			if (!handle) { handle = make_strong<script_string>(); continue; }
			if (handle.use_count() > 1) {
				engine_access::execution_limits(eng).memory_charge_deferred(handle->size() + sizeof(script_string));
				handle = make_strong<script_string>(*handle);
			}
		}
	}

	struct element_proof {
		bool typed_obj = false;
		std::vector<type_info*> tags;                          // distinct slot tags of an object-kind source
		std::vector<const class_definition*> element_classes;  // live element classes (method admission, prewarm)
		size_t failed = SIZE_MAX;                              // first failing element; SIZE_MAX = proven
		std::string what;                                      // the walk's exception text (empty for a verdict)
	};

	struct reach_filter;
	element_proof prove_elements(engine& eng, const script_value& array_value, script_array& node,
	                             const reach_filter& reach,
	                             const char* prefix, std::vector<type_info*>* types = nullptr,
	                             std::vector<std::pair<const void*, size_t>>* handle_objects = nullptr);

	// In-place element PROOF + NORMALIZATION (parallel_for):
	// after this pass returns true, mutating the element under chunk exclusivity can
	// never touch a refcount another thread can see. Value semantics carry most of the
	// argument (script stores deep-copy, so elements cannot share structure); the one
	// value shape whose copies DO share storage - strings - is handled by DETACHING
	// here instead of banning: a string node with use_count > 1 (assigned from a
	// shared source, or aliased cross-element) is replaced in place with a fresh copy,
	// single-threaded at the barrier, semantically invisible. Steady-state this walk
	// detaches nothing (worker-minted strings from the previous region are already
	// exclusive). Containers/objects whose HANDLE is shared (engine-internal aliasing;
	// value semantics makes it rare) detach wholesale the same way. What still fails:
	// shared_ptr/weak_ptr (aliasing IS their semantics), functions, bound host values,
	// references, and instances of non-flat classes.
	// reach: what the body can reach below the element - a field no worker can name is never
	// copied by one, so its shared handles need no detach.
	// The member ids the body names on element-rooted chains (null = every field) and the
	// chain terminals it uses whole (copied or replaced - every handle in that subtree is
	// touched, so the filter opens below them); nested objects are walked with the same filter.
	struct reach_filter {
		const std::vector<uint64_t>* members = nullptr;
		const std::vector<uint64_t>* whole = nullptr;
		bool names(uint64_t id) const {
			return !members || std::find(members->begin(), members->end(), id) != members->end();
		}
		bool whole_read(uint64_t id) const {
			return !members || (whole && std::find(whole->begin(), whole->end(), id) != whole->end());
		}
		reach_filter below(uint64_t id) const { return whole_read(id) ? reach_filter{} : *this; }
	};

	// Static per-class verdict for the reference-free fast path: can a body whose element
	// reach is `reach` touch a shared string buffer through an instance of `cd`? A named
	// string-reachable slot that is itself a string (or container, or read whole) says yes;
	// a named object field asks its own class; nothing named says no. Memoized per (class,
	// admission) by the caller - the walk then skips every instance whose class answers no.
	bool class_reach_touches_string(const class_definition* cd, const reach_filter& reach, engine& eng, int depth = 0) {
		if (!cd || depth > 8) { return true; }
		for (const uint32_t slot : cd->string_reachable_slots()) {
			const uint64_t id = cd->slot_field(slot);
			if (!reach.names(id)) { continue; }
			if (reach.whole_read(id)) { return true; }
			const type_info_ptr t = cd->get_field_declared_type(id);
			if (!t || t->base_type != script_value_type::jai_object_type || t->type_name.empty()) { return true; }
			auto nested = engine_access::get_class_registry(eng).find_script_class(std::string(t->type_name));
			if (!nested || class_reach_touches_string(nested.get(), reach, eng, depth + 1)) { return true; }
		}
		return false;
	}

	// Declared-type exclusivity (a typed array's element type is a proven fact
	// about every element): true when every instance a slot of class `cd` can hold - `cd`
	// and, when asked, its registered subclasses - is value-closed and no field the body
	// reaches can touch a shared string; a reached object field asks its own class the same
	// question (subclasses included: a field declared `Inner` holds any Inner). Holds a
	// whole fork without one element read when the array carries no live reference.
	bool declared_class_exclusive(const class_definition* cd, const reach_filter& reach, engine& eng,
	                              bool with_subclasses, std::vector<const class_definition*>& visited, int depth = 0) {
		if (!cd || depth > 8) { return false; }
		if (std::find(visited.begin(), visited.end(), cd) != visited.end()) { return true; }
		visited.push_back(cd);
		auto exclusive = [&](const class_definition* k) {
			if (!k->flat_value_semantics()) { return false; }
			const auto& string_slots = k->string_reachable_slots();
			for (const uint32_t slot : k->handle_bearing_slots()) {
				const uint64_t id = k->slot_field(slot);
				if (!reach.names(id)) { continue; }
				const bool holds_string = std::find(string_slots.begin(), string_slots.end(), slot) != string_slots.end();
				if (holds_string && reach.whole_read(id)) { return false; }
				const type_info_ptr t = k->get_field_declared_type(id);
				if (t && t->base_type == script_value_type::jai_object_type && !t->type_name.empty()) {
					auto nested = engine_access::get_class_registry(eng).find_script_class(std::string(t->type_name));
					if (!nested || !declared_class_exclusive(nested.get(), reach.below(id), eng, true, visited, depth + 1)) { return false; }
					continue;
				}
				if (holds_string) { return false; }   // a string, or a container that can hold one: the walk detaches
			}
			return true;
		};
		if (!exclusive(cd)) { return false; }
		if (with_subclasses) {
			for (const auto& [name, def] : engine_access::get_class_registry(eng).script_classes_) {
				(void)name;
				if (def && def.get() != cd && def->is_subtype_of(cd->get_name()) && !exclusive(def.get())) { return false; }
			}
		}
		return true;
	}

	bool normalize_element_inplace(script_value& v, engine& eng, reach_filter reach = {}, bool unreferenced_array = false, const class_definition* string_free_class = nullptr, std::vector<type_info*>* types = nullptr, const void** shared_key = nullptr);

	// An element backed by a C++ object (a bound class's instance, or a script class over one) whose script fields hold values
	// only: the distinctness check proves it the only element of its C++ object, which makes it the running chunk's own
	bool host_backed_element(const class_instance& inst, const class_definition& cd) {
		if (cd.is_script_class() && !cd.find_cpp_base_class()) { return false; }
		if (!inst.native()) { return false; }
		std::vector<const class_definition*> levels{ &cd };
		while (!levels.empty()) {
			const class_definition* level = levels.back();
			levels.pop_back();
			if (level->is_script_class()) {
				for (const uint32_t slot : level->handle_bearing_slots()) {
					const type_info_ptr t = level->get_field_declared_type(level->slot_field(slot));
					if (!t) { return false; }
					switch (t->base_type) {
					case script_value_type::jai_string_type: case script_value_type::jai_array_type: case script_value_type::jai_map_type:
						break;
					default:
						if (!level->slot_is_cpp_property(slot)) { return false; }
					}
				}
			}
			for (const auto& parent : level->get_parent_classes()) { if (parent) { levels.push_back(parent.get()); } }
		}
		return true;
	}
	// The identity two elements of one C++ object share: the object itself
	const void* element_object_key(const class_instance& inst) {
		const script_value* native = inst.native();
		if (native) {
			const auto holder = native->get_object_holder();
			if (holder) { return holder->holds_object() ? holder->object() : holder->bound_object(); }
		}
		return &inst;
	}

	// The slots of an instance whose declared type can reach a refcount, and its runtime-added fields
	bool normalize_instance_fields(class_instance* inst, const class_definition* cd, engine& eng, const reach_filter& reach, bool unreferenced_array, std::vector<type_info*>* types) {
		uint64_t& walking_field = engine_access::parallel_state(eng).element_host_field;
		const uint64_t enclosing_field = walking_field;
		for (const uint32_t slot : cd->handle_bearing_slots()) {
			const uint64_t id = cd->slot_field(slot);
			if (!reach.names(id)) { continue; }
			if (script_value* field = inst->field_slot_value(slot)) {
				walking_field = id;
				const bool ok = normalize_element_inplace(*field, eng, reach.below(id), unreferenced_array, nullptr, types);
				walking_field = enclosing_field;
				if (!ok) { return false; }
			}
		}
		if (inst->has_user_overflow_fields()) {   // runtime-added fields carry no declared type: walk them
			bool ok = true;
			inst->for_each_field([&](uint64_t id, script_value& field) {
				if (ok && cd->field_slot(id) == class_definition::k_no_field_slot && !normalize_element_inplace(field, eng, reach, unreferenced_array, nullptr, types)) { ok = false; }
			});
			if (!ok) { return false; }
		}
		return true;
	}

	// types: every visited node's shape (the transform's prewarm seed - its workers clone whole elements). shared_key: asked for a
	// top-level element only, which may be a handle (a shared_ptr or a weak_ptr's live object): it receives the object, the fork
	// admits the handle when no other element names that object, and the object is walked in place, never cloned apart
	bool normalize_element_inplace(script_value& v, engine& eng, reach_filter reach, bool unreferenced_array, const class_definition* string_free_class, std::vector<type_info*>* types, const void** shared_key) {
		if (types && v.get_type_info().get()) { types->push_back(v.get_type_info().get()); }
		switch (v.raw_storage_index()) {
		case script_value::TYPEID_NULL:
		case script_value::TYPEID_INT:
		case script_value::TYPEID_FLOAT:
		case script_value::TYPEID_CHAR:
		case script_value::TYPEID_BOOL:
			return true;
		case script_value::TYPEID_STRING: {
			auto& handle = v.unchecked_get_string_storage();
			if (handle && handle.use_count() > 1) {
				engine_access::execution_limits(eng).memory_charge_deferred(handle->size() + sizeof(script_string));
				handle = make_strong<script_string>(*handle);
			}
			return true;
		}
		case script_value::TYPEID_ARRAY: {
			if (statically_all_scalar(v)) { return true; }
			auto& handle = v.unchecked_get_array_storage();
			if (!handle) { return true; }
			if (handle.use_count() > 1) {
				v = v.parallel_detached_copy(&eng);
				return normalize_element_inplace(v, eng, reach, unreferenced_array, nullptr, types);
			}
			if (handle->is_typed()) {
				if (handle->kind() == script_array::kind_t::str) { detach_shared_typed_strings(*handle, eng); return true; }
				if (handle->kind() == script_array::kind_t::obj) {
					return prove_elements(eng, v, *handle, reach_filter{}, "parallel_for", types).failed == SIZE_MAX;
				}
				return true;   // scalar buffer: exclusive by construction
			}
			for (auto& elem : handle->values()) {
				if (!normalize_element_inplace(elem, eng, reach, unreferenced_array, nullptr, types)) { return false; }
			}
			return true;
		}
		case script_value::TYPEID_MAP: {
			if (statically_all_scalar(v)) { return true; }
			auto& handle = v.unchecked_get_map_storage();
			if (!handle) { return true; }
			if (handle.use_count() > 1) {
				v = v.parallel_detached_copy(&eng);
				return normalize_element_inplace(v, eng, reach, unreferenced_array, nullptr, types);
			}
			// A shared KEY string (a literal every map built from the same source holds)
			// detaches in place: same bytes, so the key's hash, equality and order are
			// unchanged and the map's values stay where they are. A structured key detaches
			// the map wholesale, then the values re-walk.
			bool structured_key = false;
			for (auto it = handle->begin(); it != handle->end();) {
				const size_t ki = it->first.raw_storage_index();
				if (ki == script_value::TYPEID_ARRAY || ki == script_value::TYPEID_MAP || ki == script_value::TYPEID_OBJECT) {
					structured_key = true;
					break;
				}
				if (ki == script_value::TYPEID_STRING &&
				    const_cast<script_value&>(it->first).unchecked_get_string_storage().use_count() > 1) {
					auto& key_handle = handle->equal_key_storage(it).unchecked_get_string_storage();
					engine_access::execution_limits(eng).memory_charge_deferred(key_handle->size() + sizeof(script_string));
					key_handle = make_strong<script_string>(*key_handle);
				}
				++it;
			}
			if (structured_key) {
				v = v.parallel_detached_copy(&eng);
				return normalize_element_inplace(v, eng, reach, unreferenced_array, nullptr, types);
			}
			for (auto& [key, val] : *handle) {
				(void)key;
				if (!normalize_element_inplace(val, eng, reach, unreferenced_array, nullptr, types)) { return false; }
			}
			return true;
		}
		case script_value::TYPEID_OBJECT: {
			auto& handle = v.unchecked_get_object_storage();
			if (!handle || !handle->is_instance()) {
				return false;   // bound host object wrapper
			}
			auto* inst = handle->unchecked_instance();
			const class_definition* cd = inst->get_class_definition();
			if (cd && shared_key && host_backed_element(*inst, *cd)) {
				*shared_key = element_object_key(*inst);
				inst->region_tag(engine_access::parallel_state(eng).region_serial);
				return normalize_instance_fields(inst, cd, eng, reach, false, types);
			}
			if (cd && !shared_key && cd->is_cpp_class() && !v.is_shared_ptr_type() && inst->native()) {
				// a bound C++ value an element holds: its chunk's own once the barrier finds no other element holding the object
				auto& pstate = engine_access::parallel_state(eng);
				pstate.element_host_values.push_back({ element_object_key(*inst), pstate.element_host_field, cd, SIZE_MAX });
				inst->region_tag(pstate.region_serial);
				return true;
			}
			if (!cd || !(cd->flat_value_semantics() || cd->flat_over_host_values())) {
				return false;
			}
			// A shared_ptr-tagged instance aliases by script-visible design: a top-level element hands its object to the
			// distinctness check; a nested one must be its object's only holder, since nothing proves its aliases apart
			const bool shared = v.is_shared_ptr_type();
			if (shared && shared_key) {
				*shared_key = handle->object();
			} else if (shared && (handle.use_count() > 1 || handle->object_use_count() > 1)) {
				return false;
			}
			// Reference-free fast path: no live field reference pins this instance, and the caller
			// proved no live element reference pins the array (unreferenced_array). Value semantics
			// then make the instance and its object/container subtree exclusive by construction;
			// the only alias left is a SHARED STRING BUFFER (a literal several instances hold), so
			// the walk narrows to the string slots the body can reach.
			if (unreferenced_array && inst->live_field_refs() == 0 && !inst->has_user_overflow_fields() && cd->flat_value_semantics()) {
				if (cd == string_free_class) { return true; }   // proved once per fork: no reachable shared string
				for (const uint32_t slot : cd->string_reachable_slots()) {
					const uint64_t id = cd->slot_field(slot);
					if (!reach.names(id)) { continue; }
					if (script_value* field = inst->field_slot_value(slot)) {
						if (!normalize_element_inplace(*field, eng, reach.below(id), unreferenced_array, nullptr, types)) { return false; }
					}
				}
				return true;
			}
			if (!shared && (handle.use_count() > 1 || handle->object_use_count() > 1)) {
				v = v.clone();   // engine-internal aliasing: fresh exclusive instance
				auto& fresh = v.unchecked_get_object_storage();
				inst = fresh->unchecked_instance();
			}
			// Only slots whose declared type can reach a refcount need the walk (strings detach,
			// containers and nested objects alias-check); scalar slots are exclusive by type
			const bool fields_ok = normalize_instance_fields(inst, cd, eng, reach, unreferenced_array, types);
			return fields_ok;
		}
		case script_value::TYPEID_WEAK_PTR: {
			// a top-level weak handle's body reaches the live object through lock(): that object, like a shared element's
			if (!shared_key) { return false; }
			auto locked = v.get_weak_ptr().lock();
			if (!locked) { return true; }   // expired: lock() yields null in the body
			if (!locked->is_instance()) { return false; }
			auto* inst = locked->unchecked_instance();
			const class_definition* cd = inst->get_class_definition();
			if (cd && host_backed_element(*inst, *cd)) {
				*shared_key = element_object_key(*inst);
				inst->region_tag(engine_access::parallel_state(eng).region_serial);
				return normalize_instance_fields(inst, cd, eng, reach, false, types);
			}
			if (!cd || !cd->flat_value_semantics()) { return false; }
			*shared_key = locked->object();
			return normalize_instance_fields(inst, cd, eng, reach, false, types);
		}
		default:
			return false;
		}
	}

	// The word the transform's rejection names for the first non-value-semantic leaf below
	// v (the text parallel_detached_copy raised before objects were admitted); null when
	// every leaf is value-semantic
	const char* non_value_semantic_what(const script_value& v) {
		switch (v.raw_storage_index()) {
		case script_value::TYPEID_NULL:
		case script_value::TYPEID_INT:
		case script_value::TYPEID_FLOAT:
		case script_value::TYPEID_CHAR:
		case script_value::TYPEID_BOOL:
		case script_value::TYPEID_STRING:
			return nullptr;
		case script_value::TYPEID_ARRAY: {
			const script_array* node = v.unchecked_array_node();
			if (!node) { return nullptr; }
			if (node->kind() == script_array::kind_t::obj) {
				for (size_t i = 0; i < node->size(); ++i) {
					if (const char* w = non_value_semantic_what(node->get(i, node->owner))) { return w; }
				}
				return nullptr;
			}
			if (node->is_typed()) { return nullptr; }
			for (const auto& elem : node->values()) {
				if (const char* w = non_value_semantic_what(elem)) { return w; }
			}
			return nullptr;
		}
		case script_value::TYPEID_MAP:
			for (const auto& [key, val] : v.unchecked_as_map()) {
				if (const char* w = non_value_semantic_what(key)) { return w; }
				if (const char* w = non_value_semantic_what(val)) { return w; }
			}
			return nullptr;
		case script_value::TYPEID_OBJECT: {
			auto holder = v.get_object_holder();
			if (!holder || !holder->is_instance()) { return "bound host object"; }
			if (v.is_shared_ptr_type()) { return "shared_ptr"; }
			const auto* inst = holder->unchecked_instance();
			const class_definition* cd = inst->get_class_definition();
			if (!cd || !cd->flat_value_semantics()) { return "object"; }
			const char* found = nullptr;
			inst->for_each_field([&](uint64_t, const script_value& field) { if (!found) { found = non_value_semantic_what(field); } });
			return found ? found : "object";
		}
		case script_value::TYPEID_CPP_BOUND:
			return v.bound_semantic_index() != script_value::TYPEID_NULL || v.is_null() ? "bound host value" : "bound host object";
		case script_value::TYPEID_FUNCTION: return "function";
		case script_value::TYPEID_WEAK_PTR: return "weak_ptr";
		default: return "unsupported type";
		}
	}

	// The fork's element proof, shared by both forms: after it returns without a failing
	// index, an element can be handed to its chunk's worker without touching a refcount
	// another thread can see - parallel_for mutates it in place through a reference, the
	// transform's by-value binding clones it (so the transform reaches every element WHOLE
	// and passes an open reach). An object-kind source takes the declared-type verdict first
	// (one scan of the 16-byte slot tags, no instance read; the declared type is
	// a proven fact about every element) and walks per element only when it fails; every
	// other source that is not statically all-primitive walks (normalize_element_inplace).
	// elements: the boxed view walked for a non-object kind (the node's own vector, or the
	// caller's materialized copy of a typed primitive/string buffer)
	element_proof prove_elements(engine& eng, const script_value& array_value, script_array& node,
	                             const reach_filter& reach,
	                             const char* prefix, std::vector<type_info*>* types,
	                             std::vector<std::pair<const void*, size_t>>* handle_objects) {
		element_proof proof;
		proof.typed_obj = node.is_typed() && node.kind() == script_array::kind_t::obj;
		std::vector<script_value> no_elements;
		std::vector<obj_slot> no_slots;
		std::vector<script_value>& elements = node.is_typed() ? no_elements : node.values();
		std::vector<obj_slot>& slots = proof.typed_obj ? node.objs() : no_slots;
		const size_t n = proof.typed_obj ? node.size() : elements.size();
		const class_definition* string_free_class = nullptr;
		bool declared_exclusive = false;
		if (proof.typed_obj) {
			type_info* last = nullptr;
			for (const obj_slot& slot : slots) {
				type_info* tag = slot.tag.get();
				if (tag == last) { continue; }
				last = tag;
				if (std::find(proof.tags.begin(), proof.tags.end(), tag) == proof.tags.end()) { proof.tags.push_back(tag); }
			}
		}
		if (proof.typed_obj && n > 0) {
			declared_exclusive = node.live_elem_refs == 0;
			std::vector<const class_definition*> visited;
			for (type_info* tag : proof.tags) {
				const class_definition* cd = (tag && tag->base_type == script_value_type::jai_object_type)
					? engine_access::get_class_registry(eng).find_script_class(std::string(tag->type_name)).get() : nullptr;
				if (!cd) { declared_exclusive = false; continue; }   // shared_ptr-tagged or host slots: the walk decides
				if (std::find(proof.element_classes.begin(), proof.element_classes.end(), cd) == proof.element_classes.end()) { proof.element_classes.push_back(cd); }
				if (!declared_class_exclusive(cd, reach, eng, /*with_subclasses*/ false, visited)) { declared_exclusive = false; }
				if (proof.tags.size() == 1 && cd->flat_value_semantics() && !class_reach_touches_string(cd, reach, eng)) { string_free_class = cd; }
			}
		}
		(void)prefix;
		if (declared_exclusive || statically_all_scalar(array_value)) {
			return proof;
		}
		// An object slot lends its handle to a boxed value for the walk (the count stays 1, so
		// the alias check sees the truth) and takes back whatever the walk left - the same
		// handle or a detached clone
		struct slot_box {
			obj_slot& slot;
			script_value v;
			slot_box(obj_slot& s, engine* e) : slot(s), v(std::move(s.handle), s.tag, e) {}
			~slot_box() { slot.handle = std::move(v.unchecked_get_object_storage()); slot.tag = v.get_type_info(); }
		};
		auto& host_values = engine_access::parallel_state(eng).element_host_values;
		for (size_t i = 0; i < n; ++i) {
			bool ok = false;
			const void* object = nullptr;
			const void** key = handle_objects ? &object : nullptr;   // handles are admitted at the top level only, for the distinctness check
			const size_t host_mark = host_values.size();
			try {
				if (proof.typed_obj) {
					slot_box box(slots[i], &eng);
					ok = normalize_element_inplace(box.v, eng, reach, node.live_elem_refs == 0, string_free_class, types, key);
				} else {
					ok = normalize_element_inplace(elements[i], eng, reach, node.live_elem_refs == 0, string_free_class, types, key);
				}
				if (object) { handle_objects->emplace_back(object, i); }
				if (handle_objects) {
					for (size_t k = host_mark; k < host_values.size(); ++k) { host_values[k].element = i; }
				}
			} catch (const std::exception& e) {
				proof.failed = i;
				proof.what = e.what();
				return proof;
			}
			if (!ok) {
				proof.failed = i;
				return proof;
			}
			if (!proof.typed_obj && elements[i].is_object()) {
				auto holder = elements[i].get_object_holder();
				if (holder && holder->is_instance()) {
					auto* cd = holder->unchecked_instance()->get_class_definition();
					if (cd && (proof.element_classes.empty() || proof.element_classes.back() != cd) &&
					    std::find(proof.element_classes.begin(), proof.element_classes.end(), cd) == proof.element_classes.end()) {
						proof.element_classes.push_back(cd);
					}
				}
			}
		}
		return proof;
	}

	JAI_NOINLINE std::string shared_element_host_value_text(engine& eng, const parallel_engine_state::element_host_value& first, const parallel_engine_state::element_host_value& second) {
		std::string result = "elements " + std::to_string(first.element) + " and " + std::to_string(second.element) + " share one '" + first.cls->get_name() + "'";
		if (first.field != UINT64_MAX) {
			result += " in field '" + std::string(engine_access::get_symbolizer(eng)->get_string(first.field)) + "'";
		}
		return result + " (an element owns the C++ values it holds: give each element its own, or capture one from outside the loop)";
	}

	// Two elements holding one bound C++ value: the first such pair in element order, else null
	const parallel_engine_state::element_host_value* shared_element_host_value(std::vector<parallel_engine_state::element_host_value>& values) {
		if (values.size() < 2) { return nullptr; }
		std::stable_sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.object < b.object || (a.object == b.object && a.element < b.element); });
		const parallel_engine_state::element_host_value* found = nullptr;
		for (size_t k = 1; k < values.size(); ++k) {
			if (values[k].object != values[k - 1].object || values[k].element == values[k - 1].element) { continue; }
			if (!found || values[k].element < found[1].element) { found = &values[k - 1]; }
		}
		return found;
	}

	// Distinctness of the handle elements a fork admitted (range and zipped arrays, by element): one worker touches each object, so
	// an object named at two elements fails; the repeat met first in element order is returned with the element it repeats. The
	// same object at one element in several zipped arrays is that element's worker alone
	std::pair<size_t, size_t> repeated_handle_object(std::vector<std::pair<const void*, size_t>>& handle_objects) {
		std::pair<size_t, size_t> result{SIZE_MAX, SIZE_MAX};
		if (handle_objects.size() < 2) { return result; }
		std::sort(handle_objects.begin(), handle_objects.end());   // each object's elements in element order
		for (size_t k = 1, group = 0; k < handle_objects.size(); ++k) {
			if (handle_objects[k].first != handle_objects[group].first) { group = k; continue; }
			const size_t original = handle_objects[group].second;
			if (handle_objects[k].second != original && handle_objects[k].second < result.first) {
				result = {handle_objects[k].second, original};
			}
		}
		return result;
	}

	// Borrow admission (content half): TRUE when every element of the container is a plain
	// primitive or a string - element reads through the borrow are then raw const reads
	// minting worker-local values (a string read copies the BYTES, never the handle), safe
	// against the non-atomic strong counts by construction. Const walk, no handle copies.
	// Content walk twin of statically_primitive_shape (untyped containers): with_strings is
	// the borrow tier's question, the scalar form is the element proof's
	bool parallel_content_primitive_shape(const script_value& v, bool with_strings) {
		switch (v.raw_storage_index()) {
		case script_value::TYPEID_ARRAY: {
			if (const script_array* node = v.unchecked_array_node(); node->is_typed()) {
				if (node->kind() == script_array::kind_t::obj) { return false; }   // handles never borrow raw
				return with_strings || node->kind() != script_array::kind_t::str;
			}
			for (const auto& elem : v.unchecked_as_array()) {
				switch (elem.raw_storage_index()) {
				case script_value::TYPEID_NULL:
				case script_value::TYPEID_INT:
				case script_value::TYPEID_FLOAT:
				case script_value::TYPEID_CHAR:
				case script_value::TYPEID_BOOL:
					break;
				case script_value::TYPEID_STRING:
					if (!with_strings) { return false; }
					break;
				default:
					return false;
				}
			}
			return true;
		}
		case script_value::TYPEID_MAP: {
			for (const auto& [key, val] : v.unchecked_as_map()) {
				const size_t ki = key.raw_storage_index();
				const size_t vi = val.raw_storage_index();
				auto primitive = [with_strings](size_t idx) {
					return idx == script_value::TYPEID_NULL || idx == script_value::TYPEID_INT ||
					       idx == script_value::TYPEID_FLOAT || idx == script_value::TYPEID_CHAR ||
					       idx == script_value::TYPEID_BOOL || (with_strings && idx == script_value::TYPEID_STRING);
				};
				if (!primitive(ki) || !primitive(vi)) { return false; }
			}
			return true;
		}
		default:
			return false;
		}
	}

	bool parallel_content_all_primitive(const script_value& v) { return parallel_content_primitive_shape(v, /*with_strings*/ true); }
	bool parallel_content_all_scalar(const script_value& v) { return parallel_content_primitive_shape(v, /*with_strings*/ false); }

	// One captured name, resolved at the barrier from the LIVE global value
	struct resolved_capture {
		const parallel_admission::capture_entry* entry = nullptr;
		bool method_scope = false;           // provisioned into the worker's method_env (a pinned method's capture)
		// the snapshot cache's key: symbol ids are interner indices, so the top bit is free
		uint64_t cache_key() const { return entry->name_id | (method_scope ? (uint64_t(1) << 63) : 0); }
		std::optional<script_value> source;  // deref'd live value; for a borrow this handle
		                                     // IS the region anchor (held until the join)
		parallel_capture_kind kind = parallel_capture_kind::scalar;
		bool shares_slots = false;           // written: its slots hold objects, so worker copies share them (written_slot_copy)
		bool sliced = false;                 // borrow: the plan's sliced input, its elements' handles handed out (proven below)
		bool host = false;                   // object: a C++ class's instance (each worker's view shares its C++ object)
		std::vector<const class_definition*> classes;   // object: the classes of the instances it holds
		// stream: each worker's copy and the cursors it follows, seeded together once the region is sure to run
		std::vector<std::pair<script_value, const std::vector<script_value>*>> stream_copies;
	};

	// A captured class instance, or a handle to one, a body reads through a worker's own view: a script class whose value
	// tree is flat (fields of values only, no script destructor), or a C++ class's instance held directly. Lists the
	// identities of the instances it holds (no element may be one of them) and their classes; null when admitted, else
	// what the value holds that keeps it out
	struct captured_object_shape {
		bool host = false;
		std::vector<const class_definition*> classes;
		std::vector<const void*> objects;
	};
	const char* captured_object_refusal(const script_value& v, captured_object_shape& shape, bool top) {
		const auto& holder = v.unchecked_object_handle();
		if (!holder || !holder->is_instance()) { return "a bound host object"; }
		const auto* inst = holder->unchecked_instance();
		const class_definition* cd = inst->get_class_definition();
		if (!cd) { return "an object of no class"; }
		if (cd->is_cpp_class()) {
			const script_value* native = inst->native();
			const auto native_holder = native ? native->get_object_holder() : nullptr;
			if (!top) { return "a C++ object inside a script object"; }
			if (!native_holder || !native_holder->holds_object()) { return "a C++ object the script does not own"; }
			shape.host = true;
			shape.classes.push_back(cd);
			shape.objects.push_back(element_object_key(*inst));
			return nullptr;
		}
		if (cd->find_cpp_base_class()) { return "a script class over a C++ class"; }
		if (!cd->flat_value_semantics() || inst->has_user_overflow_fields()) { return "an object holding a handle, a function, a var field or a C++ object"; }
		if (cd->value_tree_runs_destructor()) { return "an object whose class runs a destructor"; }
		if (std::find(shape.classes.begin(), shape.classes.end(), cd) == shape.classes.end()) { shape.classes.push_back(cd); }
		shape.objects.push_back(inst);
		const char* result = nullptr;
		const_cast<class_instance*>(inst)->for_each_field([&](uint64_t, script_value& field) {
			const script_value& value = field.deref();
			if (!result && value.raw_storage_index() == script_value::TYPEID_OBJECT) { result = captured_object_refusal(value, shape, false); }
		});
		return result;
	}
	// A worker's view of a captured object: a detached copy of a script value (its strings and containers its own), or a new
	// instance sharing the C++ object, so no handle a worker copies is another thread's
	script_value captured_object_view(const script_value& source, bool host, engine& eng, std::vector<type_info*>& types) {
		if (host) { return source.parallel_host_view(); }
		script_value result = source.clone();
		normalize_element_inplace(result, eng, reach_filter{}, false, nullptr, &types);
		return result;
	}
	// The class chain's script overloads of a method, nearest first (the override hides its base's)
	bool script_method_in_chain(const class_definition* cls, uint64_t name_id) {
		std::vector<const class_definition*> stack{ cls };
		while (!stack.empty()) {
			const class_definition* c = stack.back();
			stack.pop_back();
			auto it = c->script_method_overloads().find(name_id);
			if (it != c->script_method_overloads().end() && !it->second.empty()) { return true; }
			for (const auto& parent : c->get_parent_classes()) { if (parent) { stack.push_back(parent.get()); } }
		}
		return false;
	}
	// The first instance an element's value reaches that is one of `objects` (sorted), null when none is
	const void* element_reaches_object(const script_value& v_in, const std::vector<const void*>& objects, engine& eng) {
		const script_value& v = v_in.deref();
		switch (v.raw_storage_index()) {
		case script_value::TYPEID_OBJECT: {
			const auto& holder = v.unchecked_object_handle();
			if (!holder || !holder->is_instance()) { return nullptr; }
			auto* inst = holder->unchecked_instance();
			const void* key = element_object_key(*inst);
			if (std::binary_search(objects.begin(), objects.end(), key) ||
			    std::binary_search(objects.begin(), objects.end(), static_cast<const void*>(inst))) { return key; }
			const void* result = nullptr;
			inst->for_each_field([&](uint64_t, script_value& field) { if (!result) { result = element_reaches_object(field, objects, eng); } });
			return result;
		}
		case script_value::TYPEID_ARRAY: {
			const script_array* node = v.unchecked_array_node();
			if (!node || (node->is_typed() && node->kind() != script_array::kind_t::obj)) { return nullptr; }
			for (size_t i = 0; i < node->size(); ++i) {
				if (const void* hit = element_reaches_object(node->get(i, &eng), objects, eng)) { return hit; }
			}
			return nullptr;
		}
		case script_value::TYPEID_MAP: {
			for (const auto& [key, value] : v.unchecked_as_map()) {
				(void)key;
				if (const void* hit = element_reaches_object(value, objects, eng)) { return hit; }
			}
			return nullptr;
		}
		default:
			return nullptr;
		}
	}

	std::vector<size_t> partition_flat(size_t n, size_t chunks) {
		std::vector<size_t> bounds(chunks + 1);
		for (size_t k = 0; k <= chunks; ++k) {
			bounds[k] = n * k / chunks;
		}
		return bounds;
	}

	std::vector<size_t> partition_weighted(const std::vector<double>& weights, size_t chunks) {
		double total = 0.0;
		for (double w : weights) { total += w; }
		if (!(total > 0.0)) { return partition_flat(weights.size(), chunks); }
		std::vector<size_t> bounds(chunks + 1);
		bounds[0] = 0;
		bounds[chunks] = weights.size();
		double prefix = 0.0;
		size_t k = 1;
		for (size_t i = 0; i < weights.size() && k < chunks; ++i) {
			if (prefix >= total * static_cast<double>(k) / static_cast<double>(chunks)) {
				bounds[k++] = i;
			}
			prefix += weights[i];
		}
		while (k < chunks) { bounds[k++] = weights.size(); }
		return bounds;
	}

	// Flat-table content equality for the snapshot-capture cache: top-level string, or
	// a hetero array (same type stamp) whose elements are strings / raw scalars / null.
	// Live elements compare through deref; detached copies hold no references. Any
	// deeper shape returns false - the caller re-detaches rather than risks.
	bool flat_snapshot_equal(const script_value& live_in, const script_value& cached) {
		const script_value& live = live_in.deref();
		const size_t li = live.raw_storage_index();
		if (li != cached.raw_storage_index()) { return false; }
		if (li == script_value::TYPEID_STRING) {
			return *live.get_storage().get<strong_ptr<script_string>>() ==
			       *cached.get_storage().get<strong_ptr<script_string>>();
		}
		if (li != script_value::TYPEID_ARRAY) { return false; }
		const script_array* ln = live.unchecked_array_node();
		const script_array* cn = cached.unchecked_array_node();
		if (ln->is_typed() || cn->is_typed()) { return false; }
		if (live.get_type_info().get() != cached.get_type_info().get()) { return false; }
		const auto& lv = ln->values();
		const auto& cv = cn->values();
		if (lv.size() != cv.size()) { return false; }
		for (size_t i = 0; i < lv.size(); ++i) {
			const script_value& a = lv[i].deref();
			const script_value& b = cv[i];
			const size_t ai = a.raw_storage_index();
			if (ai != b.raw_storage_index()) { return false; }
			switch (ai) {
			case script_value::TYPEID_STRING:
				if (*a.get_storage().get<strong_ptr<script_string>>() !=
				    *b.get_storage().get<strong_ptr<script_string>>()) { return false; }
				break;
			case script_value::TYPEID_INT:
				if (a.unchecked_as_int() != b.unchecked_as_int()) { return false; }
				break;
			case script_value::TYPEID_FLOAT: {
				// Bitwise: NaN-stable (== would force a re-detach every region)
				const script_float af = a.unchecked_as_float();
				const script_float bf = b.unchecked_as_float();
				if (std::memcmp(&af, &bf, sizeof(af)) != 0) { return false; }
				break;
			}
			case script_value::TYPEID_BOOL:
				if (a.unchecked_as_bool() != b.unchecked_as_bool()) { return false; }
				break;
			case script_value::TYPEID_CHAR:
				if (a.unchecked_as_char() != b.unchecked_as_char()) { return false; }
				break;
			case script_value::TYPEID_NULL:
				break;
			default:
				return false;
			}
		}
		return true;
	}

	// One snapshot/string capture, cache-aware: a content-matched cached copy is reused
	// outright (types re-registered; provisioned_nodes are the caller's rebuild-on-miss
	// concern); a miss detaches as before and (re)stores the record only when
	// flat_snapshot_equal can prove the shape. `validated` memoizes ONE content proof
	// per region per name: the first slot compares against the live source, and
	// siblings whose record carries the same store serial hold identical content by
	// construction (stored single-threaded at one barrier from one source), so they
	// skip the compare. Throws propagate to the caller's existing per-capture handler.
	void provision_snapshot_capture(parallel_worker_slot& slot, uint64_t name_id,
	                                const script_value& source, engine& eng,
	                                std::vector<type_info*>& value_types,
	                                script_value& provisioned,
	                                uint64_t region_serial,
	                                std::unordered_map<uint64_t, uint64_t>& validated,
	                                std::vector<const void*>& fresh_nodes,
	                                bool& any_snapshot_miss
#ifdef JAISCRIPT_VM_PROFILE
	                                , parallel_engine_state::region_phase_prof* prof
	                                , std::string_view prof_name
#endif
	                                ) {
		parallel_worker_slot::snapshot_capture_cache_entry* cached = nullptr;
		for (auto& e : slot.snapshot_cache) {
			if (e.name_id == name_id) { cached = &e; break; }
		}
		if (cached) {
			const auto memo = validated.find(name_id);
			const bool proven = memo != validated.end() && memo->second == cached->built_serial;
			if (proven || flat_snapshot_equal(source, cached->value)) {
				if (!proven) { validated[name_id] = cached->built_serial; }
#ifdef JAISCRIPT_VM_PROFILE
				if (prof) { ++prof->cache_hits; }
#endif
				value_types.insert(value_types.end(), cached->types.begin(), cached->types.end());
				provisioned = cached->value;
				return;
			}
		}
		any_snapshot_miss = true;
#ifdef JAISCRIPT_VM_PROFILE
		if (prof) { ++prof->cache_misses; ++prof->cache_miss_names[std::string(prof_name)]; }
		const uint64_t prof_detach0 = __rdtsc();
#endif
		std::vector<const void*> capture_nodes;
		std::vector<type_info*> fresh_types;
		provisioned = source.parallel_detached_copy(&eng, &fresh_types, &capture_nodes);
#ifdef JAISCRIPT_VM_PROFILE
		if (prof) { prof->cache_miss_cyc[std::string(prof_name)] += __rdtsc() - prof_detach0; }
#endif
		value_types.insert(value_types.end(), fresh_types.begin(), fresh_types.end());
		fresh_nodes.insert(fresh_nodes.end(), capture_nodes.begin(), capture_nodes.end());
		if (flat_snapshot_equal(source, provisioned)) {
#ifdef JAISCRIPT_VM_PROFILE
			if (prof) { ++prof->cache_stores; }
#endif
			parallel_worker_slot::snapshot_capture_cache_entry rec;
			rec.name_id = name_id;
			rec.built_serial = region_serial;
			rec.value = provisioned;
			rec.nodes = std::move(capture_nodes);
			rec.types = std::move(fresh_types);
			if (cached) { *cached = std::move(rec); }
			else { slot.snapshot_cache.push_back(std::move(rec)); }
		} else {
#ifdef JAISCRIPT_VM_PROFILE
			if (prof) { ++prof->cache_unprovable; }
#endif
			if (cached) {
				// Shape stopped being provable: the record can never hit again - drop its pins
				cached->name_id = 0;
				cached->built_serial = 0;
				cached->value = script_value(std::monostate{}, &eng);
				cached->nodes.clear();
				cached->types.clear();
			}
		}
	}

	// provisioned_nodes maintenance for the cross-region cache: on an all-hit region the
	// set from the previous region already covers every cached node (stale extras from
	// dead fresh nodes can only cause a spurious result-detach - content-neutral); any
	// miss rebuilds it exactly from the surviving records plus this region's fresh nodes.
	void rebuild_provisioned_nodes(parallel_worker_slot& slot, bool any_snapshot_miss,
	                               const std::vector<const void*>& fresh_nodes) {
		if (!any_snapshot_miss) { return; }
		slot.provisioned_nodes.clear();
		for (const auto& rec : slot.snapshot_cache) {
			slot.provisioned_nodes.insert(rec.nodes.begin(), rec.nodes.end());
		}
		slot.provisioned_nodes.insert(fresh_nodes.begin(), fresh_nodes.end());
	}

} // namespace

// ============================== THE REGION (one fork for both forms) ==============================
bool parallel_pure_function(std::string_view name) { return parallel_host_whitelisted(name); }

script_value parallel_bound_pin(const parallel_method_pin& pin, const script_value& receiver, uint64_t name_id, engine* eng) {
	return script_value::make_function([pin_ptr = &pin, receiver, name_id, eng](const std::vector<script_value>& args) {
		return call_parallel_host_pin(*pin_ptr, receiver, name_id, args, eng);
	}, eng);
}

script_value* parallel_script_field(class_instance& inst, uint64_t member_id) {
	if (const class_definition* cls = inst.get_class_definition()) {
		const class_definition* owner = cls->find_field_declaring_class(member_id);
		if (owner && !owner->is_script_class()) { return nullptr; }
	}
	return inst.find_field_value(member_id);
}

JAI_NOINLINE checked_result<void> parallel_method_not_admitted(uint64_t name_id) {
	return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation),
		"method '{0}' was not admitted in this parallel body (admitted: single-overload public methods of the element's class or its "
		"bases touching only element, local, or captured-read state)", name_id);
}

std::optional<script_value> parallel_chunk_binding(engine& eng, uint64_t name_id) {
	const parallel_region_table* region = engine_access::parallel_region_active(eng) ? engine_access::parallel_state(eng).active_region : nullptr;
	const parallel_region_table::entry* chunk = region ? region->find_entry(std::this_thread::get_id()) : nullptr;
	if (!chunk || !chunk->definition_env || !*chunk->definition_env) { return std::nullopt; }
	auto bound = (*chunk->definition_env)->get(name_id);
	return bound ? std::move(bound.value()) : script_value(std::monostate{}, &eng);
}

JAI_NOINLINE checked_result<void> parallel_conversion_not_admitted(uint64_t class_name_id) {
	return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation),
		"converting constructor of '{0}' was not admitted in this parallel body (the statement admits a conversion into a slot it can "
		"type)", class_name_id);
}

// === Shared objects: what a worker reaches through a host call is read through a borrow, never through its handle ===

script_value parallel_object_borrow(const class_instance& source, type_info_ptr tag, engine& eng) {
	parallel_engine_state& state = engine_access::parallel_state(eng);
	if (source.region_tag() != 0 && source.region_tag() == engine_access::parallel_region_serial(eng)) {
		throw runtime_error("a host call in a parallel body returned an object one of its iterations owns (an element, or an object a chunk made)");
	}
	const parallel_region_table::entry* entry = state.active_region ? state.active_region->find_entry(std::this_thread::get_id()) : nullptr;
	if (!entry || !entry->borrows) { throw runtime_error("a shared object was reached outside a parallel chunk"); }
	object_borrows& borrows = *entry->borrows;
	auto [known, fresh] = borrows.by_source.try_emplace(&source, nullptr);
	if (fresh) {
		auto record = std::make_unique<object_borrow_record>();
		record->source = &source;
		record->eng = &eng;
		class_definition* cd = source.get_class_definition();
		const script_value* native = source.native();
		const auto* native_holder = native ? native->peek_object_holder() : nullptr;
		if (cd && native_holder && (native_holder->holds_object() || native_holder->bound_object())) {
			// the worker's own instance and holders over the same C++ object: host members run on it, never on the shared handle
			auto instance = class_instance::make(cd->interned_name_id(), &eng);
			instance->set_class_definition(cd);
			instance->set_native_view(native_holder->holds_object()
				? script_value::make_cpp_object(native_holder->type_name(), native_holder->type_id(), native_holder->host_object(), &eng)
				: script_value::make_cpp_reference(native_holder->type_name(), native_holder->type_id(), native_holder->bound_object(), native_holder->lock_weak_data(), &eng));
			record->view = script_value::make_class_object(*cd, std::move(instance), &eng);
		}
		known->second = record.get();
		borrows.records.push_back(std::move(record));
	}
	return script_value::make_object_borrow(known->second, std::move(tag), &eng);
}

script_value parallel_shared_object(const script_value& object, engine& eng) {
	const auto* holder = object.peek_object_holder();
	if (!holder) { return script_value(std::monostate{}, &eng); }
	if (holder->is_instance()) { return parallel_object_borrow(*holder->unchecked_instance(), object.get_type_info(), eng); }
	if (holder->holds_object()) {
		script_value view = script_value::make_cpp_object(holder->type_name(), holder->type_id(), holder->host_object(), &eng);
		view.set_type_info(object.get_type_info());
		return view;
	}
	throw runtime_error("value is not value-semantic (object)");
}

script_value parallel_shared_value(const script_value& value, engine& eng) {
	const script_value& v = value.deref();
	switch (v.raw_storage_index()) {
		case script_value::TYPEID_NULL:
		case script_value::TYPEID_INT:
		case script_value::TYPEID_FLOAT:
		case script_value::TYPEID_CHAR:
		case script_value::TYPEID_BOOL:
		case script_value::TYPEID_PARALLEL_BORROW:
			return v;
		default:
			return v.parallel_detached_copy(&eng);
	}
}

script_value parallel_object_copy(const script_value& borrow, const object_borrow_record& record) {
	if (borrow.is_shared_ptr_type()) { return borrow; }
	engine& eng = *record.eng;
	const class_instance& source = *record.source;
	class_definition* cd = source.get_class_definition();
	if (!cd) { throw runtime_error("a shared object in a parallel body has no class to copy it by"); }
	auto copy = cd->allocate_instance();
	copy->set_class_definition(cd);
	source.for_each_field([&](uint64_t id, const script_value& value) { copy->set_field(id, parallel_shared_value(value, eng)); });
	if (const script_value* native = source.native()) {
		const auto* native_holder = native->peek_object_holder();
		if (!native_holder || !native_holder->holds_object() || !cd->has_copy_function()) {
			throw runtime_error("a shared object in a parallel body cannot be copied by value: its C++ object has no copy function");
		}
		auto copied = cd->copy_object(native_holder->host_object().get());
		if (!copied) { throw runtime_error("Failed to copy C++ object of type '" + std::string(source.get_class_name()) + "'. The registered copy function returned null."); }
		copy->set_native(script_value::make_cpp_object(source.get_class_name(), cd->get_type_id(), copied, &eng));
		copy->region_tag(engine_access::parallel_region_serial(eng));
	}
	cd->register_live_instance(copy.get());
	script_value result = script_value::make_class_object(*cd, std::move(copy), &eng);
	result.set_type_info(borrow.get_type_info());
	return result;
}

script_value parallel_by_value(const script_value& value) {
	const object_borrow_record* record = value.parallel_object_borrow();
	if (!record) { return value; }
	engine& eng = *record->eng;
	class_definition* cd = record->source->get_class_definition();
	if (!cd) { throw runtime_error("a shared object in a parallel body has no class to copy it by"); }
	auto copy = class_instance::make(cd->interned_name_id(), &eng);   // inert: its death runs no destructor the serial loop does not run
	copy->set_class_definition(cd);
	record->source->for_each_field([&](uint64_t id, const script_value& field) { copy->set_field(id, parallel_shared_value(field, eng)); });
	if (const auto* view = record->view.peek_object_holder(); view && view->is_instance()) {
		if (const script_value* native = view->unchecked_instance()->native()) { copy->set_native_view(*native); }   // the one C++ object, read only
	}
	script_value result = script_value::make_class_object(*cd, std::move(copy), &eng);
	result.set_type_info(value.get_type_info());
	return result;
}

std::optional<checked_result<script_value>> parallel_borrow_field(const script_value& borrow, uint64_t member_id) {
	const object_borrow_record* record = borrow.parallel_object_borrow();
	const script_value* field = parallel_script_field(const_cast<class_instance&>(*record->source), member_id);
	if (!field) { return std::nullopt; }
	try {
		return checked_result<script_value>(parallel_shared_value(*field, *record->eng));
	} catch (const runtime_error&) {
		return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation),
			"member '{0}' of a shared object cannot be read in a parallel body (it holds a function or a weak_ptr)", member_id);
	}
}

checked_result<void> parallel_borrow_call(const script_value& borrow, uint64_t name_id, const parallel_method_pin_table* pins) {
	const class_definition* cd = borrow.parallel_object_borrow()->source->get_class_definition();
	const parallel_method_pin* pin = pins && cd ? pins->find(cd, name_id) : nullptr;
	if (!pin) { return parallel_method_not_admitted(name_id); }
	if (pin->resolved || !parallel_borrow_view(borrow)) {
		return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation),
			"method '{0}' cannot run on a shared object in a parallel body (an object reached through a host call takes only its "
			"C++ class's const methods)", name_id);
	}
	return {};
}

const script_value* parallel_borrow_view(const script_value& borrow) noexcept {
	const object_borrow_record* record = borrow.parallel_object_borrow();
	return record && !record->view.is_null() ? &record->view : nullptr;
}

checked_result<script_value> parallel_shared_store_refusal(uint64_t member_id) {
	return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation),
		"member '{0}' is stored on a shared object in a parallel body (a body writes its element, out[i] and the objects it made)", member_id);
}

checked_result<script_value> parallel_shared_host_refusal(uint64_t name_id) {
	return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation),
		"C++ call '{0}' takes a shared object that holds no C++ object in a parallel body", name_id);
}

namespace {
	script_value borrow_handle(const script_value& borrow, engine& eng) {
		const object_borrow_record* record = borrow.parallel_object_borrow();
		auto* source = const_cast<class_instance*>(record->source);
		class_definition* cd = source->get_class_definition();
		script_value handle = script_value::make_class_object(*cd, source->strong_from_this(), &eng);
		handle.set_type_info(borrow.get_type_info());
		return handle;
	}
}

void parallel_swap_borrows(script_value& root, engine& eng, std::unordered_set<const void*>& seen) {
	std::vector<script_value*> work{ &root };
	auto visit_instance = [&](const script_value::object_holder* holder) {
		if (!holder || !holder->is_instance()) { return; }
		class_instance* inst = holder->unchecked_instance();
		if (!seen.insert(inst).second) { return; }
		inst->for_each_field([&](uint64_t, script_value& field) { work.push_back(&field); });
	};
	while (!work.empty()) {
		script_value* v = work.back();
		work.pop_back();
		switch (v->raw_storage_index()) {
			case script_value::TYPEID_PARALLEL_BORROW:
				if (v->parallel_object_borrow()) { *v = borrow_handle(*v, eng); }
				break;
			case script_value::TYPEID_ARRAY: {
				script_array* node = const_cast<script_array*>(v->unchecked_array_node());
				if (!node || !seen.insert(node).second) { break; }
				if (node->kind() == script_array::kind_t::obj) {
					for (auto& slot : node->objs()) { visit_instance(slot.handle.get()); }
				} else if (!node->is_typed()) {
					for (auto& element : node->values()) { work.push_back(&element); }
				}
				break;
			}
			case script_value::TYPEID_MAP: {
				script_map& map = const_cast<script_map&>(v->unchecked_as_map());
				if (!seen.insert(&map).second) { break; }
				bool borrowed_key = false;
				for (const auto& [key, value] : map) {
					(void)value;
					if (key.parallel_object_borrow()) { borrowed_key = true; }
				}
				if (borrowed_key) {
					script_map rebuilt(&eng);
					for (const auto& [key, value] : map) { rebuilt.emplace(key.parallel_object_borrow() ? borrow_handle(key, eng) : key, value); }
					map = std::move(rebuilt);
				}
				for (auto& [key, value] : map) {
					(void)key;
					work.push_back(&value);
				}
				break;
			}
			case script_value::TYPEID_OBJECT:
				visit_instance(v->peek_object_holder());
				break;
			default:
				break;
		}
	}
}

bool parallel_host_arguments(std::vector<script_value>& args) {
	for (auto& arg : args) {
		if (arg.deref().parallel_object_borrow()) {
			const script_value* view = parallel_borrow_view(arg.deref());
			if (!view) { return false; }
			arg = *view;
		}
	}
	return true;
}

checked_result<script_value> call_parallel_host_pin(const parallel_method_pin& pin, const script_value& receiver, uint64_t name_id,
                                                    std::vector<script_value> args, engine* eng, bool property) {
	if (!parallel_host_arguments(args)) { return parallel_shared_host_refusal(name_id); }
	const script_value& self = receiver.deref();
	const class_definition* cls = nullptr;
	const class_instance* inst = nullptr;
	if (self.raw_storage_index() == script_value::TYPEID_OBJECT) {
		const auto holder = self.get_object_holder();
		if (holder && holder->is_instance()) {
			inst = holder->unchecked_instance();
			cls = inst->get_class_definition();
		}
	}
	const uint64_t type_id = cls ? cls->get_type_id() : 0;
	const uint64_t arity_bit = uint64_t(1) << std::min<size_t>(args.size(), 63);
	if (pin.serial_arities & arity_bit) {
		return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation),
			"C++ method '{0}' of '{1}' is marked serial_method() and cannot be called in a parallel body", name_id, type_id);
	}
	if ((pin.mutating_arities & arity_bit) && !(inst && inst->region_tag() != 0 && inst->region_tag() == engine_access::parallel_region_serial(*eng))) {
		if (property) {
			return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation),
				"C++ property '{0}' of '{1}' is stored on an object this parallel body did not make (only an object it made takes stores)", name_id, type_id);
		}
		return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation),
			"C++ method '{0}' of '{1}' changes an object this parallel body did not make (only its const methods may run on it)", name_id, type_id);
	}
	args.insert(args.begin(), receiver);
	return pin.method_value.as_function()(args);
}
checked_result<script_value> call_parallel_operator_pin(execution_backend& backend, const parallel_method_pin& pin, uint64_t name_id,
                                                        const script_value& self, std::vector<script_value> args, engine* eng) {
	if (const auto* dispatch = pin.method_value.is_function() ? pin.method_value.as_function().target<script_method_dispatch>() : nullptr) {
		auto resolved = dispatch->cls->resolve_method_overload_shared(dispatch->name_id, args.data(), args.size());
		if (!resolved) { return resolved.error_value(); }
		return backend.execute_method_from(*dispatch, *resolved.value(), self, args, 0);
	}
	return call_parallel_host_pin(pin, self, name_id, std::move(args), eng);
}
void release_deferred_in_order(const std::vector<std::vector<deferred_release>*>& lists) {
	std::vector<deferred_release> ordered;
	for (auto* list : lists) {
		for (auto& released : *list) { ordered.push_back(std::move(released)); }
		list->clear();
	}
	std::stable_sort(ordered.begin(), ordered.end(), [](const deferred_release& a, const deferred_release& b) { return a.element < b.element; });
	for (auto& released : ordered) {
		released.value = script_value(std::monostate{}, nullptr);
	}
}

void recover_pending_registrations(parallel_engine_state& state) {
	if (!state.pending_registration_retry || state.active_region) { return; }
	// Recover every retained pool before eviction, slot replacement or a class
	// reload could discard pending copies or omit them from a migration snapshot.
	for (auto& pool : state.region_pools) {
		std::vector<parallel_worker_slot*> slots;
		for (auto& slot : pool.worker_slots) { if (slot) { slots.push_back(slot.get()); } }
		drain_pending_instances(state, slots);
	}
	state.pending_registration_retry = false;
}

// parallel_for and parallel_transform run the SAME region: containers[0] is the range (the
// transform's input), further containers are the zip's arrays (the transform's output is one),
// every chunk gets one cursor per array, and each worker enters script once to run its chunk
// loop. The plan carries the only differences: the prefix of every text, the environment
// captured reads resolve through, which arrays the body may write (the transform never writes
// its input, so borrowing it is legal and its typed primitive form is boxed once instead of
// demoted in place), the chunk partition (the transform's weight hint), and whether element
// methods are admitted (the in-place statement). parallel_transform therefore costs exactly what
// the equivalent zip statement costs plus the output array it hands back.
namespace {

// The out-of-range indices an index write's barrier interns for the serial loop's text (past them a worker raises the text without them)
constexpr size_t k_index_write_texts = 256;

// An entry's region flag, held while it admits and runs (a nested region is refused while it holds)
struct region_running_guard {
	parallel_engine_state& s;
	explicit region_running_guard(parallel_engine_state& a_state) : s(a_state) { s.region_running = true; }
	~region_running_guard() { s.region_running = false; }
};

// An entry's error doors: raise holds the text in the engine state the result views, usage raises an unsupported operation
// under the entry's prefix (none: the text names its entry)
template <class Result>
auto region_error_doors(parallel_engine_state& a_state, std::string_view a_prefix = {}) {
	auto raise = [&a_state](std::error_code code, std::string message) {
		a_state.error_text = std::move(message);
		return Result(code, std::string_view(a_state.error_text));
	};
	auto usage = [raise, a_prefix](std::string message) {
		return raise(make_error_code(runtime_error_code::unsupported_operation), a_prefix.empty() ? std::move(message) : std::string(a_prefix) + ": " + std::move(message));
	};
	return std::pair{raise, usage};
}

struct region_plan {
	const char* prefix = "parallel_for";
	std::string_view counted_variable;   // the counted form's variable: a method writing it through a reference is refused
	std::vector<script_value>* containers = nullptr;                 // [0] = the range, then the zip's arrays
	std::shared_ptr<script_defined_function> root_fn;                // the body function / fn (admission root, pool key)
	const parallel_admission* adm = nullptr;
	const std::shared_ptr<function_decl>* chunk_wrapper = nullptr;
	std::shared_ptr<environment> capture_env;                        // null: the global environment
	const std::vector<std::pair<uint64_t, size_t>>* frame_captures = nullptr;   // the statement's enclosing-frame locals (symbol, slot)
	const std::function<const script_value*(size_t)>* frame_slot = nullptr;    // reads the executing frame's slot
	bool inplace = false;                                            // parallel_for: the range is written, element methods admitted
	std::vector<size_t> bounds;                                      // empty: the flat partition
	const parallel_admission* extra_seed_adm = nullptr;              // weight fn types (transform)
	bool zip_arrays_fresh = false;                                   // the zip's arrays were allocated by the caller this call (the transform's output): exclusive by construction
	const parallel_counted_sequence* counted = nullptr;              // the counted form: the range is this sequence, containers[0] its cursor's anchor
	uint64_t sliced_input = UINT64_MAX;                              // a captured array each element of which one worker alone reads (the reduce's input)
#ifdef JAISCRIPT_VM_PROFILE
	parallel_engine_state::region_phase_prof* prof = nullptr;
#endif
};

	// One region shape's method admission: the key it was admitted under and the builder holding what it pinned
	struct method_admission_record {
		uint64_t generation = 0;
		uint64_t serial = 0;   // names this record to the slots that keep its pins (0: an uncached record, never kept)
		const char* prefix = nullptr;
		std::vector<const class_definition*> element_classes;
		std::vector<type_info*> written_types;
		bool element_methods = false, destructors = false, inplace = false;
		uint64_t registry_key = 0, definition_epoch = 0;
		bool ok = true;
		std::error_code code;
		std::string message;
		std::unique_ptr<admission_builder> builder;
	};
	struct method_admission_cache {
		static constexpr size_t k_records = 16;   // region shapes a program cycles through; the oldest leaves first
		std::vector<std::unique_ptr<method_admission_record>> records;
		std::unique_ptr<method_admission_record> uncached;   // an admission admit_cached never stamped: admitted afresh each region
		uint64_t next_serial = 0;
		method_admission_record* find(uint64_t generation, const char* prefix, const std::vector<const class_definition*>& classes, const std::vector<type_info*>& written,
		                              bool element_methods, bool destructors, bool inplace, uint64_t registry_key, uint64_t definition_epoch) {
			if (generation == 0) { return nullptr; }
			for (auto& r : records) {
				if (r->generation == generation && r->prefix == prefix && r->element_methods == element_methods && r->destructors == destructors && r->inplace == inplace &&
				    r->registry_key == registry_key && r->definition_epoch == definition_epoch && r->element_classes == classes && r->written_types == written) {
					return r.get();
				}
			}
			return nullptr;
		}
		method_admission_record& make(uint64_t generation, const char* prefix, const std::vector<const class_definition*>& classes, const std::vector<type_info*>& written,
		                              bool element_methods, bool destructors, bool inplace, uint64_t registry_key, uint64_t definition_epoch, engine& eng) {
			auto r = std::make_unique<method_admission_record>();
			r->generation = generation;
			r->prefix = prefix;
			r->element_classes = classes;
			r->written_types = written;
			r->element_methods = element_methods;
			r->destructors = destructors;
			r->inplace = inplace;
			r->registry_key = registry_key;
			r->definition_epoch = definition_epoch;
			r->builder = std::make_unique<admission_builder>(eng);
			if (generation == 0) {
				uncached = std::move(r);
				return *uncached;
			}
			r->serial = ++next_serial;
			if (records.size() >= k_records) { records.erase(records.begin()); }
			records.push_back(std::move(r));
			return *records.back();
		}
	};

	// The accessors, and the operators of every class the region's objects can belong to: a script operator's walk is a trial,
	// rolled back whole when it refuses (the operator stays unpinned)
	void admit_region_extras(admission_builder& method_adm, const parallel_admission& body, const std::vector<const class_definition*>& element_classes,
	                         bool element_methods, const std::vector<type_info_ptr>& written_types, bool program_destructors) {
		method_adm.admit_host_accessors(body.element_reached_members);
		method_adm.admit_host_accessors(method_adm.adm.element_reached_members);
		const bool host_calls = !body.host_functions.empty() || !method_adm.adm.host_functions.empty() || !method_adm.host_pins.empty() ||
		                        !body.static_calls.empty() || !method_adm.adm.static_calls.empty();
		const std::vector<uint64_t> names = method_adm.operator_names();
		std::vector<std::pair<const class_definition*, uint64_t>> tried;
		// a fixed point: an operator body can construct objects of more classes, whose operators are candidates in turn
		for (bool grew = true; grew;) {
			grew = false;
			for (const class_definition* cls : method_adm.operator_classes(element_classes, host_calls)) {
				for (const uint64_t id : names) {
					if (std::find(tried.begin(), tried.end(), std::make_pair(cls, id)) != tried.end()) { continue; }
					tried.emplace_back(cls, id);
					const script_value* bound = cls->method_ptr(id);
					if (!bound || !bound->is_function()) { continue; }
					if (const auto* dispatch = bound->as_function().target<script_method_dispatch>()) {
						if (!dispatch->cls) { continue; }
						// the operator's overloads, the constructions and methods they reach: all admitted, or none
						admission_builder trial(method_adm);
						if (!trial.admit_script_operator(dispatch->cls.get(), id) ||
						    !trial.admit_region_classes(element_classes, element_methods, written_types, program_destructors)) {
							const std::string_view refused = trial.adm.message;   // the admission text: the region's prefix is not the worker's
							const size_t prefix_size = std::string_view(method_adm.prefix).size() + 2;
							method_adm.operator_refusals.push_back({ cls, id, std::string(refused.size() > prefix_size ? refused.substr(prefix_size) : refused) });
							continue;
						}
						method_adm = std::move(trial);
						method_adm.operator_pins.push_back({ cls, id, dispatch->cls.get(), true });
						grew = true;
					} else {
						method_adm.operator_pins.push_back({ cls, id, admission_builder::host_method_owner(cls, id), false });
					}
				}
			}
		}
		method_adm.object_classes = method_adm.operator_classes(element_classes, host_calls);
	}

// One owner for activation, the thread table, join obligations and borrowed storage.
// A failed setup discards partial reusable slots; a completed setup keeps their caches.
struct region_guard_t {
	engine& eng;
	parallel_engine_state& state;
	std::vector<std::unique_ptr<parallel_worker_slot>>& slots;
	size_t count;
	parallel_region_table table;
	parallel_region_table* previous_region;
	bool previous_frozen;
	jai::thread_pool* pending_pool = nullptr;
	bool active = false;
	bool prepared = false;

	region_guard_t(engine& e, parallel_engine_state& s,
	               std::vector<std::unique_ptr<parallel_worker_slot>>& workers, size_t n)
		: eng(e), state(s), slots(workers), count(n),
		  previous_region(s.active_region), previous_frozen(engine_access::get_symbolizer(e)->frozen()) {}
	region_guard_t(const region_guard_t&) = delete;
	region_guard_t& operator=(const region_guard_t&) = delete;

	void activate() noexcept {
		// Accepted work can create live copies even if a later submit throws.
		// Only a complete post-join drain clears this obligation.
		state.pending_registration_retry = true;
		engine_access::get_symbolizer(eng)->set_frozen(true);
		state.active_region = &table;
		engine_access::note_parallel_region(eng, true);
		active = true;
	}
	void join() {
		if (pending_pool) {
			pending_pool->wait_idle();
			pending_pool = nullptr;
		}
	}
	void restore() noexcept {
		if (active) {
			state.active_region = previous_region;
			engine_access::note_parallel_region(eng, previous_region != nullptr);
			engine_access::get_symbolizer(eng)->set_frozen(previous_frozen);
			active = false;
		}
	}
	~region_guard_t() {
		join();                 // accepted work must finish before table/cursor destruction
		restore();
		for (size_t k = 0; k < count && k < slots.size(); ++k) {
			if (!prepared) {
				slots[k].reset();    // includes an environment whose publication only partly succeeded
				continue;
			}
			parallel_worker_slot* slot = slots[k].get();
			if (!slot) { continue; }
			for (uint64_t id : slot->capture_name_ids) {
				if (script_value* value = slot->root_env->get_own_value_ptr(id)) {
					*value = script_value(std::monostate{}, &eng);   // existing storage, no allocation
				}
				if (slot->method_env) {
					if (script_value* value = slot->method_env->get_own_value_ptr(id)) {
						*value = script_value(std::monostate{}, &eng);
					}
				}
			}
			slot->capture_name_ids.clear();
			slot->element_refs.clear();
			slot->cursors.clear();
			slot->call_args.clear();
			slot->raw_input = nullptr;
			slot->typed_input = nullptr;
			slot->backend->set_parallel_method_pins(nullptr);
		}
	}
};

#ifdef JAISCRIPT_VM_PROFILE
// One phase's cycles, added to the region profile when the phase ends
struct region_phase_timer {
	uint64_t* cycles;
	uint64_t start = __rdtsc();
	~region_phase_timer() { if (cycles) { *cycles += __rdtsc() - start; } }
};
#define JAI_REGION_PHASE(phase) region_phase_timer phase_timer{ plan.prof ? &plan.prof->phase_cyc[static_cast<size_t>(parallel_engine_state::region_phase::phase)] : nullptr }
#else
#define JAI_REGION_PHASE(phase)
#endif

// One region, phase by phase: each phase reads what the earlier ones settled, and a refusal returns its text
struct region_run {
	engine& eng;
	region_plan& plan;
	parallel_engine_state& state;
	std::vector<script_value>& containers;
	const parallel_admission* adm;
	const script_value& array_value;
	const script_array* source_node;

	std::vector<resolved_capture> captures;
	std::vector<std::pair<const void*, std::string_view>> captured_objects;   // every instance a captured object holds, by its name
	size_t scanned_objects = 0;

	script_value boxed_range;
	script_array* node = nullptr;
	size_t n = 0;
	std::vector<type_info*> value_types;
	std::vector<std::pair<const void*, size_t>> handle_objects;   // (object, element) of every handle element, range and zips
	element_proof proof;
	std::vector<script_array*> zip_nodes;
	std::vector<type_info*> zip_tags;

	std::vector<const class_definition*> captured_classes;
	method_admission_record* record = nullptr;

	size_t worker_count = 1;
	std::chrono::nanoseconds budget{};
	bool use_vm = false;
	parallel_engine_state::parallel_region_pool* pool = nullptr;
	std::vector<size_t> bounds;
	std::vector<size_t> chunk_begin, chunk_count;
	parallel_block_schedule schedule;
	std::vector<parallel_worker_slot*> contexts;
	type_info_ptr element_type;
	const parallel_worker_error* winner = nullptr;
	std::optional<region_guard_t> guard;   // last: it joins and restores before anything above it dies

	region_run(engine& a_engine, region_plan& a_plan) :
		eng(a_engine),
		plan(a_plan),
		state(engine_access::parallel_state(a_engine)),
		containers(*a_plan.containers),
		adm(a_plan.adm),
		array_value(containers.front().is_reference() ? containers.front().deref() : containers.front()),
		source_node(array_value.unchecked_array_node()),
		boxed_range(std::monostate{}, &a_engine) {
	}

	checked_result<void> raise(std::error_code code, std::string message) {
		state.error_text = std::move(message);
		return checked_result<void>(code, std::string_view(state.error_text));
	}
	checked_result<void> usage(std::string message) {
		return raise(make_error_code(runtime_error_code::unsupported_operation), std::string(plan.prefix) + ": " + std::move(message));
	}
	admission_builder& method_adm() { return *record->builder; }

	checked_result<void> run() {
		recover_pending_registrations(state);
		if (++state.region_serial == 0) { state.region_serial = 1; }   // what this region's elements and chunks own carries it
#ifdef JAISCRIPT_VM_PROFILE
		if (plan.prof) { ++plan.prof->regions; }
#endif
		if (auto resolved = resolve_captures(); !resolved) { return resolved; }
		if (auto proven = prove_range_and_zip(); !proven) { return proven; }
		if (auto disjoint = prove_index_writes(); !disjoint) { return disjoint; }
		if (n == 0) { return {}; }
		if (auto admitted = admit_methods(); !admitted) { return admitted; }
		partition();
		if (auto provisioned = provision_slots(); !provisioned) { return provisioned; }
		prewarm();
		fan_out_and_join();
		return drain();
	}

	// CAPTURED READS resolve through the statement's environment chain (its walk reaches the
	// global env; slot-resident function locals are not env-visible and correctly error as
	// undefined) or, for the builtin, the global env. A BORROW of an array the body WRITES
	// would read racing content (borrow indexes any element, writes own chunks only) -
	// contract A errors instead of silently demoting; snapshots of it stay legal.
	bool written_container(const script_value& v) const {
		if (v.raw_storage_index() != script_value::TYPEID_ARRAY) { return false; }
		for (size_t j = plan.inplace ? 0 : 1; j < containers.size(); ++j) {
			const script_value& cj = containers[j].is_reference() ? containers[j].deref() : containers[j];
			if (cj.is_array() && cj.unchecked_array_node() == v.unchecked_array_node()) { return true; }
		}
		return false;
	}
	checked_result<void> classify_and_add(const parallel_admission::capture_entry& entry, bool method_scope = false) {
		// a local of the frame the statement runs in reads from that frame's slot: no environment
		// chain can see it, and a global of the same name must not shadow it; a method's capture
		// is a global by construction (its class's scope), never the caller's local
		const script_value* frame_local = nullptr;
		if (!method_scope && plan.frame_captures && plan.frame_slot) {
			for (const auto& [sym, slot] : *plan.frame_captures) {
				if (sym == entry.name_id) { frame_local = (*plan.frame_slot)(slot); break; }
			}
		}
		auto global = frame_local ? checked_result<script_value>(*frame_local)
		            : (plan.capture_env && !method_scope) ? plan.capture_env->get(entry.name_id)
		                                                   : engine_access::get_global_environment(eng)->get(entry.name_id);
		if (!global) {
			return usage("captured name '" + std::string(entry.name) + "' is not defined at the region barrier");
		}
		resolved_capture cap;
		cap.entry = &entry;
		cap.method_scope = method_scope;
		cap.source = global.value().deref();
		// an index-written array: every worker writes its own copy, merged back at the join
		const size_t kind_index = !method_scope && adm->index_written(entry.name_id) ? size_t(-1) : cap.source->raw_storage_index();
		if (kind_index == size_t(-1)) {
			if (cap.source->raw_storage_index() != script_value::TYPEID_ARRAY) {
				return usage("captured name '" + std::string(entry.name) + "' is written at an index but is not an array");
			}
			if (written_container(*cap.source)) {
				return usage("captured name '" + std::string(entry.name) +
				             "' is the container being mutated (reads through it would race the element writes)");
			}
			cap.kind = parallel_capture_kind::written;
			cap.shares_slots = holds_objects(*cap.source);
		}
		switch (kind_index) {
		case size_t(-1):
			break;
		case script_value::TYPEID_NULL:
		case script_value::TYPEID_INT:
		case script_value::TYPEID_FLOAT:
		case script_value::TYPEID_CHAR:
		case script_value::TYPEID_BOOL:
			cap.kind = parallel_capture_kind::scalar;
			break;
		case script_value::TYPEID_CPP_BOUND: {
			const size_t sem = cap.source->bound_semantic_index();
			if (sem == script_value::TYPEID_NULL && !cap.source->is_null()) {
				return usage("captured name '" + std::string(entry.name) + "' is not value-semantic (bound host object)");
			}
			cap.source = cap.source->clone();
			cap.kind = sem == script_value::TYPEID_STRING
				? parallel_capture_kind::string : parallel_capture_kind::scalar;
			break;
		}
		case script_value::TYPEID_STRING:
			cap.kind = parallel_capture_kind::string;
			break;
		case script_value::TYPEID_ARRAY:
		case script_value::TYPEID_MAP: {
			if (!method_scope && entry.name_id == plan.sliced_input && kind_index == script_value::TYPEID_ARRAY) {
				cap.kind = parallel_capture_kind::borrow;
				cap.sliced = !statically_all_primitive(*cap.source) && !parallel_content_all_primitive(*cap.source);
				break;
			}
			const bool same_container = written_container(*cap.source);
			const bool borrowable = entry.borrow_eligible && !same_container &&
			                        (statically_all_primitive(*cap.source) || parallel_content_all_primitive(*cap.source));
			if (same_container && entry.borrow_eligible) {
				return usage("captured name '" + std::string(entry.name) +
				             "' is the container being mutated (reads through it would race the element writes)");
			}
			cap.kind = borrowable ? parallel_capture_kind::borrow : parallel_capture_kind::snapshot;
			break;
		}
		case script_value::TYPEID_FUNCTION:
			return usage("captured name '" + std::string(entry.name) +
			             "' is a function value (only direct calls to statically-known functions are allowed in a parallel body)");
		case script_value::TYPEID_OBJECT: {
			if (stdlib::random_detail::is_random(*cap.source)) {
				cap.kind = parallel_capture_kind::stream;
				break;
			}
			if (!cap.source->unchecked_object_handle()) {   // a null handle copies no count
				cap.kind = parallel_capture_kind::scalar;
				break;
			}
			captured_object_shape shape;
			if (const char* refusal = captured_object_refusal(*cap.source, shape, true)) {
				return usage("captured name '" + std::string(entry.name) + "' is not value-semantic (" + refusal + ")");
			}
			if (cap.source->is_shared_ptr_type() && entry.bare_use) {
				return usage("captured handle '" + std::string(entry.name) + "' is read through its members and methods only (a worker reads its own view of the object) at " +
				             std::to_string(entry.bare_use->location.line) + ":" + std::to_string(entry.bare_use->location.column));
			}
			cap.kind = parallel_capture_kind::object;
			cap.host = shape.host;
			cap.classes = std::move(shape.classes);
			for (const void* object : shape.objects) { captured_objects.emplace_back(object, entry.name); }
			break;
		}
		default:
			return usage("captured name '" + std::string(entry.name) + "' is not value-semantic (object/shared_ptr/weak_ptr)");
		}
		if (entry.member_use && cap.kind != parallel_capture_kind::object && cap.kind != parallel_capture_kind::stream) {
			return usage("member access reaches captured state '" + std::string(entry.name) + "' (captured reads are read-only, and only a captured object has members) at " +
			             std::to_string(entry.member_use->location.line) + ":" + std::to_string(entry.member_use->location.column));
		}
		state.faults.checkpoint("region.capture_metadata", captures.size());
		bool listed = false;
		for (const auto& seen : state.last_captures) { if (seen.first == entry.name_id) { listed = true; break; } }
		if (!listed) { state.last_captures.emplace_back(entry.name_id, cap.kind); }
		state.faults.checkpoint("region.capture_collect", captures.size());
		captures.push_back(std::move(cap));
		return {};
	}
	// No captured object may be one an element reaches: the element's lane writes it while every worker reads its own view.
	// Walked before the element proof (which would part an aliased element from its object), and only when objects are captured
	checked_result<void> refuse_captured_aliases() {
		if (captured_objects.size() == scanned_objects || !plan.inplace) { return {}; }
		scanned_objects = captured_objects.size();
		std::vector<const void*> objects;
		for (const auto& [object, name] : captured_objects) { objects.push_back(object); }
		std::sort(objects.begin(), objects.end());
		for (size_t j = 0; j < containers.size(); ++j) {
			const script_value& cj = containers[j].is_reference() ? containers[j].deref() : containers[j];
			if (!cj.is_array()) { continue; }
			const script_array* cj_node = cj.unchecked_array_node();
			if (cj_node->is_typed() && cj_node->kind() != script_array::kind_t::obj) { continue; }
			for (size_t i = 0; i < cj_node->size(); ++i) {
				const void* hit = element_reaches_object(cj_node->get(i, &eng), objects, eng);
				if (!hit) { continue; }
				std::string_view name;
				for (const auto& [object, captured] : captured_objects) { if (object == hit) { name = captured; break; } }
				return usage((j == 0 ? std::string("element ") : "zipped array " + std::to_string(j + 1) + " element ") + std::to_string(i) +
				             " reaches the object captured as '" + std::string(name) + "' (a captured object is read-only, and an element's lane may write it)");
			}
		}
		return {};
	}
	checked_result<void> resolve_captures() {
		JAI_REGION_PHASE(captures);
		state.last_captures.clear();
		state.faults.checkpoint("region.capture_storage");
		captures.reserve(adm->captures.size());
		for (const auto& entry : adm->captures) {
			auto classified = classify_and_add(entry);
			if (!classified) { return classified; }
		}
		return refuse_captured_aliases();
	}

	// an array's elements proven exclusive, their classes joining the region's; a refusal is the text naming the element
	std::optional<std::string> prove_exclusive(const script_value& source, script_array& array, std::vector<type_info*>* types,
	                                           std::vector<std::pair<const void*, size_t>>* objects, std::vector<type_info*>* tags) {
		element_proof proven = prove_elements(eng, source, array, reach_filter{}, plan.prefix, types, objects);
		if (proven.failed != SIZE_MAX) {
			return "element " + std::to_string(proven.failed) + (proven.what.empty() ? std::string(" cannot be proven exclusive") : ": " + proven.what);
		}
		if (tags) { tags->insert(tags->end(), proven.tags.begin(), proven.tags.end()); }
		for (const class_definition* cd : proven.element_classes) {
			if (std::find(proof.element_classes.begin(), proof.element_classes.end(), cd) == proof.element_classes.end()) { proof.element_classes.push_back(cd); }
		}
		return std::nullopt;
	}
	// THE RANGE. A primitive-kind typed range the body may write demotes IN PLACE at the
	// barrier (node identity preserved) so the hetero kernel
	// below, which has no raw typed worker reads or writes, stays correct;
	// one the body only reads (the transform's input) is boxed ONCE into a private node the
	// cursor can address instead. An OBJECT-kind range keeps its slots: element references
	// materialize the slot's handle per touch and the declared class carries the exclusivity
	// proof (the declared type of a typed array is a proven fact about every element).
	// ELEMENT PROOF + NORMALIZATION (prove_elements): every element must end this pass
	// exclusively-mutable - shared strings detach in place, the declared class of an
	// object-kind source proves it without one element read (the body's element reach
	// narrows both). A body that binds its element in place (in-place statement, or a
	// read-only by-value parameter) reaches only the members it names; a body that copies the
	// element (a mutating by-value parameter clones every field) reaches it whole. The
	// builtin's all-primitive input is exclusive by construction (typed proof, zero scans;
	// untyped pay ONE content scan). The counted form's range is its sequence of ints.
	checked_result<void> prove_range_and_zip() {
		JAI_REGION_PHASE(elements);
		const bool typed_obj = source_node->is_typed() && source_node->kind() == script_array::kind_t::obj;
		state.faults.checkpoint("region.input_storage");
		if (source_node->is_typed() && !typed_obj) {
			if (plan.inplace) {
				const_cast<script_array*>(source_node)->demote_to_hetero(&eng);
			} else {
				boxed_range = script_value::make_array(nullptr, &eng);
				boxed_range.unchecked_get_array_storage()->values() = const_cast<script_array*>(source_node)->materialize_values(&eng);
			}
		}
		node = boxed_range.is_array() ? boxed_range.unchecked_get_array_storage().get() : const_cast<script_array*>(source_node);
		n = plan.counted ? plan.counted->n : node->size();

		const bool binds_in_place = plan.inplace || (!adm->element_reach_all && !adm->element_mutated);
		const reach_filter reach = binds_in_place
			? reach_filter{ adm->element_reach_all ? nullptr : &adm->element_reached_members, &adm->element_reached_whole }
			: reach_filter{};
		state.element_host_values.clear();
		state.element_host_field = UINT64_MAX;
		const bool skip_proof = plan.counted || (!plan.inplace && (statically_all_scalar(array_value) || parallel_content_all_scalar(array_value)));
		if (!skip_proof) {
			state.faults.checkpoint("region.element_proof");
			proof = prove_elements(eng, array_value, *node, reach, plan.prefix, plan.inplace ? nullptr : &value_types, &handle_objects);
			if (proof.failed != SIZE_MAX) {
				if (!proof.what.empty()) {
					return usage("element " + std::to_string(proof.failed) + ": " + proof.what);
				}
				if (plan.inplace) {
					return usage("element " + std::to_string(proof.failed) +
					             " cannot be proven exclusive (only value-semantic shapes are: primitives, strings,"
					             " value containers, flat value-class instances and distinct handles to them)");
				}
				const script_value failed_element = typed_obj ? node->get(proof.failed, &eng) : node->values()[proof.failed];
				const char* what = non_value_semantic_what(failed_element);
				return usage("element " + std::to_string(proof.failed) +
				             ": value is not value-semantic (" + (what ? what : "object") + ")");
			}
		}

		// THE ZIP'S ARRAYS: arrays of the range's length, each proven exclusive like the range
		// (whole reach: the body may write anything through their loop variables; a typed primitive
		// kind demoted like the range, an object kind addressed in place)
		for (size_t j = 1; j < containers.size(); ++j) {
			const script_value& zv = containers[j].is_reference() ? containers[j].deref() : containers[j];
			if (!zv.is_array()) {
				return usage("zipped array " + std::to_string(j + 1) + " is not an array");
			}
			state.faults.checkpoint("region.zip_storage", j);
			script_array* znode = const_cast<script_array*>(zv.unchecked_array_node());
			if (znode->size() != n) {
				return usage("zipped array " + std::to_string(j + 1) + " has " + std::to_string(znode->size()) +
				             " elements, the range has " + std::to_string(n) + " (zipped arrays must have the same length)");
			}
			if (znode == source_node || znode == node) {
				return usage("zipped array " + std::to_string(j + 1) + " is the range itself");
			}
			if (znode->is_typed() && znode->kind() != script_array::kind_t::obj) { znode->demote_to_hetero(&eng); }
			if (!plan.zip_arrays_fresh) {
				if (auto refused = prove_exclusive(zv, *znode, nullptr, &handle_objects, &zip_tags)) { return usage("zipped array " + std::to_string(j + 1) + " " + *refused); }
			}
			zip_nodes.push_back(znode);
		}
		// An index-written array of objects: its slots are proven exclusive like a zipped array's (their objects distinct, shared strings
		// detached), since every worker's copy shares them
		for (const auto& cap : captures) {
			if (cap.kind != parallel_capture_kind::written || !cap.shares_slots) { continue; }
			script_array* wnode = const_cast<script_array*>(cap.source->unchecked_array_node());
			if (auto refused = prove_exclusive(*cap.source, *wnode, nullptr, &handle_objects, nullptr)) { return usage("captured name '" + std::string(cap.entry->name) + "' " + *refused); }
		}
		if (const auto [repeat, original] = repeated_handle_object(handle_objects); repeat != SIZE_MAX) {
			return usage("elements " + std::to_string(original) + " and " + std::to_string(repeat) +
			             " hold the same object (an object a body reaches through a handle element belongs to one element)");
		}
		if (plan.inplace) {
			if (const auto* shared = shared_element_host_value(state.element_host_values)) {
				return usage(shared_element_host_value_text(eng, shared[0], shared[1]));
			}
		}
		// The sliced input: every element read whole by the one worker its chunk runs on, so each must be exclusive like a zipped array's
		for (const auto& cap : captures) {
			if (!cap.sliced) { continue; }
			script_array* inode = const_cast<script_array*>(cap.source->unchecked_array_node());
			if (inode->is_typed() && inode->kind() != script_array::kind_t::obj) { continue; }
			std::vector<std::pair<const void*, size_t>> input_objects;
			if (auto refused = prove_exclusive(*cap.source, *inode, &value_types, &input_objects, nullptr)) { return usage("input " + *refused); }   // the step copies each element
			if (const auto [repeat, original] = repeated_handle_object(input_objects); repeat != SIZE_MAX) {
				return usage("input elements " + std::to_string(original) + " and " + std::to_string(repeat) +
				             " hold the same object (an object the step reaches through a handle element belongs to one element)");
			}
		}
		return {};
	}

	// Disjoint index writes: the loop variable indexing an enclosing array takes distinct values, so no two elements write one slot
	// (a value that is no int in range raises in its own iteration, as the serial loop's store does). The counted form's values
	// are distinct by construction
	const script_array* index_node_of(size_t index_param) const {
		if (index_param == 0) { return node; }
		return index_param - 1 < zip_nodes.size() ? zip_nodes[index_param - 1] : nullptr;
	}
	script_value index_at(const script_array& index_node, size_t index_param, size_t p) {
		if (plan.counted && index_param == 0) { return script_value(static_cast<script_int>(plan.counted->at(p)), &eng); }
		return index_node.get(p, &eng).deref();
	}
	size_t index_count(const script_array& index_node, size_t index_param) const { return plan.counted && index_param == 0 ? n : index_node.size(); }
	const resolved_capture* written_capture(uint64_t name_id) const {
		for (const auto& cap : captures) {
			if (cap.kind == parallel_capture_kind::written && cap.entry->name_id == name_id) { return &cap; }
		}
		return nullptr;
	}
	checked_result<void> prove_index_writes() {
		JAI_REGION_PHASE(elements);
		for (size_t w = 0; w < adm->index_writes.size(); ++w) {
			const auto& iw = adm->index_writes[w];
			bool repeated = false;
			for (size_t v = 0; v < w; ++v) { repeated = repeated || adm->index_writes[v].name_id == iw.name_id; }
			const script_array* index_node = index_node_of(iw.index_param);
			const resolved_capture* target = written_capture(iw.name_id);
			if (repeated || !index_node || !target) { continue; }
			const size_t slot_count = target->source->unchecked_array_node()->size();
			auto intern_out_of_range = [&](script_int index) {   // the serial loop's out-of-bounds text names the index and the size: a worker raises it with these
				(void)engine_access::get_symbolizer(eng)->intern(std::to_string(index));
				(void)engine_access::get_symbolizer(eng)->intern(std::to_string(slot_count));
			};
			if (plan.counted && iw.index_param == 0) {
				// a monotonic sequence leaves the slots at its start, its end or both: the first texts in element order, jumping the run inside
				const parallel_counted_sequence& seq = *plan.counted;
				const auto in_range = [&](size_t p) { const script_int v = seq.at(p); return v >= 0 && static_cast<uint64_t>(v) < slot_count; };
				size_t interned = 0;
				for (size_t p = 0; p < n && interned < k_index_write_texts;) {
					if (!in_range(p)) {
						intern_out_of_range(seq.at(p));
						++interned;
						++p;
						continue;
					}
					// the first element past the slots: the value moving up crosses slot_count, moving down crosses 0
					const uint64_t from = static_cast<uint64_t>(seq.at(p));
					const uint64_t room = seq.up ? slot_count - 1 - from : from;
					const uint64_t steps = room / seq.size + 1;
					p = steps > n - p ? n : p + static_cast<size_t>(steps);
				}
				continue;
			}
			std::vector<size_t> writer(slot_count, SIZE_MAX);
			size_t out_of_range = 0;
			for (size_t p = 0; p < n && p < index_node->size(); ++p) {
				const script_value at = index_node->get(p, &eng);
				const script_value& index = at.deref();
				if (index.is_int() && (index.unchecked_as_int() < 0 || static_cast<size_t>(index.unchecked_as_int()) >= slot_count)) {
					if (out_of_range++ < k_index_write_texts) { intern_out_of_range(index.unchecked_as_int()); }
					continue;
				}
				if (!index.is_int()) { continue; }
				const size_t slot = static_cast<size_t>(index.unchecked_as_int());
				if (writer[slot] != SIZE_MAX) {
					return usage("elements " + std::to_string(writer[slot]) + " and " + std::to_string(p) + " write '" + std::string(iw.name) +
					             "' at the same index " + std::to_string(slot));
				}
				writer[slot] = p;
			}
		}
		return {};
	}

	// A method called on a captured object reads it: one that writes its object (a worker's view of it) is refused, and so is a
	// container's mutating builtin
	checked_result<void> check_captured_calls(const std::vector<parallel_admission::captured_method_call>& calls, bool method_scope) {
		for (const auto& call : calls) {
			const resolved_capture* cap = nullptr;
			for (const auto& c : captures) { if (c.entry->name_id == call.capture_id && c.method_scope == method_scope) { cap = &c; break; } }
			if (!cap) { continue; }
			auto at = [&call]() { return " at " + std::to_string(call.site->location.line) + ":" + std::to_string(call.site->location.column); };
			if (cap->kind == parallel_capture_kind::stream) {
				if (call.name == "seed") {
					return usage("random::seed cannot be called on captured '" + std::string(cap->entry->name) +
					             "' in a parallel body (seed before the loop: each element draws its own stream from it)" + at());
				}
				continue;   // a draw: each element's stream is the worker copy's
			}
			bool method_of_object = false;
			if (cap->kind == parallel_capture_kind::object) {
				for (const class_definition* cd : cap->classes) {
					if (admission_builder::host_method_owner(cd, call.name_id)) { method_of_object = true; continue; }
					if (!script_method_in_chain(cd, call.name_id)) { continue; }
					method_of_object = true;
					detail::receiver_write_analysis analysis(detail::runtime_receiver_view(&eng, engine_access::get_symbolizer(eng)));
					if (analysis.analyze_methods(cd->get_name(), call.name).writes) {
						return usage("method '" + std::string(call.name) + "' of '" + cd->get_name() + "' writes its object, and captured '" +
						             std::string(cap->entry->name) + "' is read-only (write a local copy, or the element)" + at());
					}
				}
			}
			if (!method_of_object && !parallel_readonly_method(call.name)) {
				return usage("method '" + std::string(call.name) + "' may mutate captured state '" + std::string(cap->entry->name) + "'" + at());
			}
		}
		return {};
	}
	// ELEMENT METHODS (in-place statement): admitted at the barrier against the element
	// classes the proof found; their captures classify like the body's
	checked_result<void> admit_methods() {
		JAI_REGION_PHASE(methods);
		const std::vector<const class_definition*>& element_classes = proof.element_classes;
		auto& registry = engine_access::get_class_registry(eng);
		const uint64_t registry_key = engine_access::class_definition_epoch(eng) * 1000003u + registry.cpp_classes_.size() * 131u + registry.script_classes_.size();
		if (state.host_method_names_key != registry_key) {
			admission_builder lister(eng);
			state.host_method_names.clear();
			state.host_method_names_key = registry_key;
			lister.for_each_registered_class([&](const class_definition* cls) {
				std::vector<const class_definition*> chain{ cls };
				while (!chain.empty()) {
					const class_definition* level = chain.back();
					chain.pop_back();
					if (!level->is_script_class()) {
						for (const auto& [id, value] : level->method_values()) { state.host_method_names.insert(id); }
					}
					for (const auto& parent : level->get_parent_classes()) { if (parent) { chain.push_back(parent.get()); } }
				}
			});
		}
		state.faults.checkpoint("region.method_admission");
		const bool element_methods = plan.inplace && !element_classes.empty();
		// a worker's copy of an index-written array holds objects that can die on it: the declared element type, or each held value's
		std::vector<type_info_ptr> written_types;
		if (state.program_destructors_key != registry_key) {
			state.program_destructors = admission_builder(eng).any_program_destructor();
			state.program_destructors_key = registry_key;
		}
		const bool program_destructors = state.program_destructors;
		for (const auto& cap : captures) {
			if (!program_destructors || cap.kind != parallel_capture_kind::written) { continue; }
			const type_info_ptr declared = cap.source->get_type_info() ? cap.source->get_type_info()->element_type() : type_info_ptr(nullptr);
			if (declared && declared->base_type != script_value_type::jai_any_type) {
				written_types.push_back(declared);
				continue;
			}
			const script_array* held = cap.source->unchecked_array_node();
			for (size_t j = 0; j < held->size(); ++j) {
				const type_info_ptr t = held->get(j, &eng).deref().get_type_info();
				if (t && std::find(written_types.begin(), written_types.end(), t) == written_types.end()) { written_types.push_back(t); }
			}
		}
		// the method admission of this region shape, kept while nothing it read changed: the body's admission, the element classes, the
		// written types, the class registry and the program's definitions
		auto* admissions = static_cast<method_admission_cache*>(state.method_admissions.get());
		if (!admissions) {
			auto made = std::make_shared<method_admission_cache>();
			admissions = made.get();
			state.method_admissions = std::move(made);
		}
		std::vector<type_info*> written_raw;
		for (const auto& t : written_types) { written_raw.push_back(t.get()); }
		// a captured object's classes: their methods are pinned like a constructed object's (the record keys them after a null)
		for (const auto& cap : captures) {
			for (const class_definition* cd : cap.classes) {
				if (std::find(captured_classes.begin(), captured_classes.end(), cd) == captured_classes.end()) { captured_classes.push_back(cd); }
			}
		}
		std::vector<const class_definition*> record_classes = element_classes;
		if (!captured_classes.empty()) {
			record_classes.push_back(nullptr);
			record_classes.insert(record_classes.end(), captured_classes.begin(), captured_classes.end());
		}
		record = admissions->find(adm->generation, plan.prefix, record_classes, written_raw, element_methods, program_destructors, plan.inplace,
		                          registry_key, engine_access::definition_epoch(eng));
		if (record && !admission_graph_current(eng, record->builder->adm)) {
			method_admission_record* stale = record;
			admissions->records.erase(std::find_if(admissions->records.begin(), admissions->records.end(), [stale](const auto& r) { return r.get() == stale; }));
			record = nullptr;
		}
		const bool admitted_before = record != nullptr;
		if (!record) {
			record = &admissions->make(adm->generation, plan.prefix, record_classes, written_raw, element_methods, program_destructors, plan.inplace, registry_key, engine_access::definition_epoch(eng), eng);
			record->builder->inplace_root = true;
			record->builder->prefix = plan.prefix;
			record->builder->host_names = &state.host_method_names;
		}
		if (admitted_before && !record->ok) {
			return raise(record->code, record->message);
		}
		// a host call that can return an object hands the body a shared one, whose conversions and operators need their pins
		const bool host_objects = !adm->static_calls.empty() || std::any_of(adm->host_functions.begin(), adm->host_functions.end(), [&](const auto& f) {
			script_value_type scalar;
			return !engine_access::host_function_result(eng, f.second, scalar);
		});
		if (element_methods || !adm->constructions.empty() || !element_classes.empty() || !written_types.empty() ||
		    !adm->element_method_calls.empty() || !adm->element_reached_members.empty() || !captured_classes.empty() || host_objects) {
			if (!admitted_before) {
				admission_builder& builder = method_adm();
				builder.adm.element_method_calls = adm->element_method_calls;
				builder.adm.constructions = adm->constructions;
				builder.captured_classes = captured_classes;
				builder.region_body = adm;
				builder.counted_variable = plan.counted_variable;
				// the methods' captures are their own set: the same name is the body's capture in the
				// region's enclosing scope and a method's in its class's scope
				const bool classes_admitted = builder.admit_region_classes(element_classes, element_methods, written_types, program_destructors);
				builder.region_body = nullptr;
				if (!classes_admitted) {
					record->ok = false;
					record->code = builder.adm.code;
					record->message = builder.adm.message;
					return raise(builder.adm.code, builder.adm.message);
				}
				std::vector<const class_definition*> object_classes = element_classes;
				object_classes.insert(object_classes.end(), captured_classes.begin(), captured_classes.end());
				admit_region_extras(builder, *adm, object_classes, element_methods, written_types, program_destructors);
				builder.finish_const_ref_touches();
				record->ok = true;
			}
			for (const auto& entry : method_adm().adm.captures) {
				auto classified = classify_and_add(entry, /*method_scope*/ true);
				if (!classified) { return classified; }
			}
		}
		if (auto checked = check_captured_calls(adm->captured_method_calls, false); !checked) { return checked; }
		if (auto checked = check_captured_calls(method_adm().adm.captured_method_calls, true); !checked) { return checked; }
		return refuse_captured_aliases();
	}

	// THREAD COUNT: the engine's count (the host's, else min(performance cores, hardware_concurrency - 1)), one context below
	// k_parallel_small_n, then the measured-cost cap.
	// THE PARTITION (results never depend on it): without a weight hint the elements go out in blocks of max(1, n / 8W)
	// consecutive elements, each worker's first block its own and every later block to whichever worker finishes first, so a
	// cost that varies along the range (a wall-heavy band of screen rows) or a busy core spreads over every worker instead of
	// holding the region on one; large n means blocks of hundreds, so no two workers write neighbouring scalars. A weight hint
	// keeps its contiguous cumulative split. `bounds` publishes the cumulative element counts per chunk (the elements each
	// worker ran, read at the join)
	void partition() {
		JAI_REGION_PHASE(partition);
		worker_count = eng.parallel_thread_count();
		if (worker_count < 1) { worker_count = 1; }
		if (worker_count > n) { worker_count = n; }
		if (n < k_parallel_small_n) { worker_count = 1; }
		budget = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(eng.execution_budget()));
		use_vm = eng.get_backend_type() == backend_type::vm;
		state.faults.checkpoint("region.pool_storage");
		pool = &acquire_region_pool(state, static_cast<const void*>(plan.root_fn->body.get()), use_vm);
		worker_count = cap_workers_by_cost(worker_count, n, state, *pool);
		state.faults.checkpoint("region.bounds");
		if (!plan.bounds.empty() && plan.bounds.size() >= 2 && worker_count > 1) {
			bounds = plan.bounds;                       // the caller's weighted partition
			worker_count = bounds.size() - 1;
			for (size_t k = 0; k < worker_count; ++k) { chunk_begin.push_back(bounds[k]); chunk_count.push_back(bounds[k + 1] - bounds[k]); }
		} else if (worker_count > 1) {
			schedule.block = std::max<size_t>(1, n / (8 * worker_count));
			schedule.n = n;
			schedule.blocks = (n + schedule.block - 1) / schedule.block;
			schedule.owner.assign(schedule.blocks, 0);
			schedule.next.store(worker_count, std::memory_order_relaxed);
			bounds.assign(worker_count + 1, 0);
			for (size_t k = 0; k < worker_count; ++k) {
				chunk_begin.push_back(std::min(k * schedule.block, n));
				chunk_count.push_back(k < schedule.blocks ? n : 0);   // the chunk loop's bound: the schedule ends it
			}
		} else {
			bounds = partition_flat(n, worker_count);
			chunk_begin.push_back(0); chunk_count.push_back(n);
		}
		state.faults.checkpoint("region.bounds_metadata");
		state.last_chunk_bounds = bounds;
		if (pool->worker_slots.size() < worker_count) {
			state.faults.checkpoint("region.worker_storage");
			pool->worker_slots.resize(worker_count);
		}
		state.faults.checkpoint("region.context_storage");
		contexts.reserve(worker_count);
		guard.emplace(eng, state, pool->worker_slots, worker_count);
	}

	// Method pins: each worker gets a dispatcher copy per admitted element method, operator, host method, static, construction
	// and function the class code calls, all in its method sandbox
	checked_result<void> pin_methods(parallel_worker_slot& slot, size_t k) {
		admission_builder& builder = method_adm();
		for (uint64_t id : slot.pinned_name_ids) {
			if (script_value* value = slot.root_env->get_own_value_ptr(id)) { *value = script_value(std::monostate{}, &eng); }
			if (script_value* value = slot.method_env->get_own_value_ptr(id)) { *value = script_value(std::monostate{}, &eng); }
		}
		slot.pinned_name_ids.clear();
		slot.pins_serial = 0;
		slot.method_pins.entries.clear();
		for (const auto& am : builder.admitted_methods) {
			state.faults.checkpoint("region.method_pin", k);
			const script_value shared_method = am.cls->get_method(am.name_id, false);
			const script_method_dispatch* src = shared_method.is_function()
				? shared_method.as_function().target<script_method_dispatch>() : nullptr;
			if (!src) {
				return usage("method '" + std::string(am.name) + "' has no script dispatcher (internal)");
			}
			script_method_dispatch wd;
			wd.eng = src->eng;
			wd.cls = src->cls;
			wd.name_id = src->name_id;
			// The worker's method sandbox, not the class's defining scope: a method body's captured
			// reads resolve to this worker's provisioned copies (a borrowed string table read through
			// the live global copied the shared literal's handle across threads; safe mode too), and
			// not to the body's captures, which may be the caller's locals under the same names
			wd.definition_env = slot.method_env;
			detail::parallel_method_pin pin;
			pin.method_value = script_value::make_function(std::move(wd), &eng);
			pin.resolved = am.decl;
			slot.method_pins.entries.push_back({ am.cls, am.name_id, std::move(pin) });
			if (slot.vm) { slot.vm->precompile_parallel_method(*am.decl); }
		}
		// Operators: a script operator's dispatcher in the worker's method sandbox (its overloads precompiled with the admitted
		// bodies), a C++ operator's worker copy with its arities
		slot.method_pins.operators.clear();
		slot.method_pins.refused_operators = builder.operator_refusals;
		for (const auto& op : builder.operator_pins) {
			const script_value* shared_method = op.cls->method_ptr(op.name_id);
			if (!shared_method || !shared_method->is_function()) { continue; }
			detail::parallel_method_pin pin;
			if (op.script) {
				const script_method_dispatch* src = shared_method->as_function().target<script_method_dispatch>();
				if (!src) { continue; }
				script_method_dispatch wd;
				wd.eng = src->eng;
				wd.cls = src->cls;
				wd.name_id = src->name_id;
				wd.definition_env = slot.method_env;
				pin.method_value = script_value::make_function(std::move(wd), &eng);
			} else {
				pin.method_value = script_value::make_function(script_function(shared_method->as_function()), &eng);
				const class_definition* owner = op.owner ? op.owner : op.cls;
				if (const auto* overloads = owner->host_overloads(op.name_id)) {
					for (const auto& overload : *overloads) {
						if (!overload.is_const) { pin.mutating_arities |= uint64_t(1) << std::min<size_t>(overload.arity, 63); }
					}
				}
				if (const serial_overloads* marked = owner->serial_method(op.name_id)) {
					pin.serial_arities = *marked == serial_overloads::every || !owner->host_overloads(op.name_id) ? ~uint64_t(0) : pin.mutating_arities;
				}
			}
			slot.method_pins.operators.push_back({ op.cls, op.name_id, std::move(pin) });
		}
		// Host methods and accessors: worker copies of the bound method values (a copy shares only the binding's own state)
		for (const auto& host : builder.host_pins) {
			state.faults.checkpoint("region.host_method_pin", k);
			const script_value shared_method = host.cls->get_method(host.name_id, false);
			if (!shared_method.is_function()) { continue; }
			detail::parallel_method_pin pin;
			pin.method_value = script_value::make_function(script_function(shared_method.as_function()), &eng);
			const std::string_view name = engine_access::get_symbolizer(eng)->get_string(host.name_id);
			if (const auto* overloads = host.owner->host_overloads(host.name_id)) {
				for (const auto& overload : *overloads) {
					if (!overload.is_const) { pin.mutating_arities |= uint64_t(1) << std::min<size_t>(overload.arity, 63); }
				}
			} else if (name.starts_with("_set_")) {
				pin.mutating_arities = ~uint64_t(0);
			}
			if (const serial_overloads* marked = host.owner->serial_method(host.name_id)) {
				pin.serial_arities = *marked == serial_overloads::every || !host.owner->host_overloads(host.name_id) ? ~uint64_t(0) : pin.mutating_arities;
			}
			slot.method_pins.entries.push_back({ host.cls, host.name_id, std::move(pin) });
		}
		slot.method_pins.statics.clear();
		for (const parallel_admission* source : std::initializer_list<const parallel_admission*>{ adm, &builder.adm }) {
			for (const auto& call : source->static_calls) {
				const auto& host_classes = engine_access::get_class_registry(eng).cpp_classes_;
				const auto host = host_classes.find(std::string(engine_access::get_symbolizer(eng)->get_string(call.class_name_id)));
				const script_value* entry = host == host_classes.end()
					? namespace_host_entry(eng, engine_access::get_symbolizer(eng)->get_string(call.class_name_id), engine_access::get_symbolizer(eng)->get_string(call.name_id)) : nullptr;
				if (host == host_classes.end() && !entry) { continue; }
				const script_value shared_method = entry ? *entry : host->second->static_method_in_chain(call.name_id);
				if (!shared_method.is_function() || slot.method_pins.find_static(call.class_name_id, call.name_id)) { continue; }
				slot.method_pins.statics.push_back({ call.class_name_id, call.name_id,
				                                     script_value::make_function(script_function(shared_method.as_function()), &eng) });
			}
			for (const auto& read : source->const_static_reads) {
				if (slot.method_pins.find_static(read.class_name_id, read.name_id)) { continue; }
				std::optional<script_value> constant = class_constant_value(eng, read.class_name_id, read.name_id);
				if (!constant) {
					return usage("'" + std::string(engine_access::get_symbolizer(eng)->get_string(read.class_name_id)) + "::" + std::string(engine_access::get_symbolizer(eng)->get_string(read.name_id)) +
					             "' is no longer a constant (a parallel body reads only const statics of a primitive value)");
				}
				slot.method_pins.statics.push_back({ read.class_name_id, read.name_id, std::move(*constant) });
			}
		}
		// Constructions: the class's constructor value resolves to a worker-private one whose code reads the worker's method
		// sandbox, its constructor and destructor bodies compiled here
		for (const auto& made : builder.constructed_classes) {
			state.faults.checkpoint("region.constructor_pin", k);
			if (!made.cls->is_script_class()) {   // a host class's constructor (a hybrid's base, or the class itself): a worker copy
				script_value host_ctor = engine_access::make_parallel_host_function_copy(eng, made.cls->get_name());
				slot.pinned_name_ids.push_back(made.name_id);
				slot.root_env->define(made.name_id, host_ctor);
				slot.method_env->define(made.name_id, std::move(host_ctor));
				continue;
			}
			script_callable payload;
			payload.kind = script_callable::kind_type::constructor;
			payload.more().cls = made.cls;
			if (!std::static_pointer_cast<script_class_definition>(made.cls)->get_constructor_asts().empty()) { payload.more().definition_env = slot.method_env; }
			script_value ctor = script_value::make_function(script_callable_thunk{ &eng, std::move(payload) }, &eng);
			slot.pinned_name_ids.push_back(made.name_id);
			slot.root_env->define(made.name_id, ctor);
			slot.method_env->define(made.name_id, std::move(ctor));
		}
		// The functions and host functions class code calls, worker-private copies in the method sandbox
		for (const auto& [name_id, source_fn] : builder.adm.script_functions) {
			if (name_id == UINT64_MAX) { continue; }
			state.faults.checkpoint("region.method_function", k);
			auto copy = std::make_shared<script_defined_function>(source_fn->name, std::vector<parameter>(source_fn->parameters()), source_fn->return_type,
			                                                      source_fn->body, slot.method_env, source_fn->local_count);
			copy->unit = source_fn->unit;
			if (slot.vm) { slot.vm->precompile_parallel_function(*copy); }
			script_callable payload;
			payload.kind = script_callable::kind_type::function;
			payload.fn = std::move(copy);
			slot.pinned_name_ids.push_back(name_id);
			slot.method_env->define(name_id, script_value::make_function(script_callable_thunk{ &eng, std::move(payload) }, &eng));
		}
		for (const auto& [name_id, name] : builder.adm.host_functions) {
			script_value copy = parallel_host_copy(eng, slot, name_id, std::string(name));
			if (slot.vm) { slot.vm->adopt_math_alias(name_id, copy); }
			slot.pinned_name_ids.push_back(name_id);
			slot.method_env->define(name_id, std::move(copy));
		}
		if (slot.vm) {
			for (const auto& body : builder.admitted_bodies) { slot.vm->precompile_parallel_method(*body); }
			for (const auto& expr : builder.admitted_expressions) { slot.vm->precompile_parallel_expression(expr); }
			slot.vm->prefold_parallel_reads(engine_access::fold_epoch(eng));
		}
		slot.pins_serial = record->serial;
		return {};
	}
	checked_result<void> provision_slots() {
		JAI_REGION_PHASE(slots);
		std::unordered_map<uint64_t, uint64_t>& snapshot_validated = state.snapshot_validated;
		snapshot_validated.clear();
		auto& range_handle = boxed_range.is_array() ? boxed_range.unchecked_get_array_storage()
		                                            : const_cast<script_value&>(array_value).unchecked_get_array_storage();
		element_type = array_value.get_type_info().get() ? array_value.get_type_info()->element_type() : type_info_ptr(nullptr);
		if (type_info* t = array_value.get_type_info().get()) { value_types.push_back(t); }
		state.faults.checkpoint("region.type_metadata");
		value_types.insert(value_types.end(), proof.tags.begin(), proof.tags.end());   // subclass slots carry their own tags
		value_types.insert(value_types.end(), zip_tags.begin(), zip_tags.end());
		for (size_t j = 1; j < containers.size(); ++j) {
			const script_value& zv = containers[j].is_reference() ? containers[j].deref() : containers[j];
			if (type_info* t = zv.get_type_info().get()) { value_types.push_back(t); }
		}
		admission_builder& builder = method_adm();
		for (size_t k = 0; k < worker_count; ++k) {
			auto& slot = pool->worker_slots[k];
			state.faults.checkpoint("region.worker_slot", k);
			const bool slot_fresh = !(slot && slot_matches(*slot, *adm, *plan.root_fn, use_vm));
			if (!slot_fresh) {
				reset_worker_slot(eng, *slot, budget);
			} else {
				slot = provision_worker(eng, *adm, budget, use_vm, plan.chunk_wrapper);
			}
#ifdef JAISCRIPT_VM_PROFILE
			if (plan.prof) { ++(slot_fresh ? plan.prof->slot_provisions : plan.prof->slot_resets); }
#endif
			restamp_alias_proofs(eng, *slot);
			slot->begin = chunk_begin[k];
			slot->end = n;
			slot->count = chunk_count[k];
			slot->block = 0;
			slot->stride_blocks = 1;
			slot->sched = parallel_schedule_cursor{};
			if (schedule.blocks) {
				slot->sched.schedule = &schedule;
				slot->sched.worker = static_cast<uint32_t>(k);
				slot->sched.take(k);   // its own first block (no worker runs yet)
			}
			slot->element_refs.clear();
			slot->raw_input = nullptr;
			slot->typed_input = nullptr;
			// ONE cursor reference per array per chunk, minted here (the array pin and the
			// live_elem_refs count are main-thread facts) and re-pointed per element by the worker's
			// chunk loop: a by-ref loop variable binds it, a by-value one copies through it exactly
			// as it would the element (normalization made every element exclusive, so those copies
			// touch no cross-thread counts). An admitted body cannot retain it past its iteration
			// (no lambdas, no 'new', stores deref, captured state is read-only, the result is discarded).
			state.faults.checkpoint("region.cursor_storage", k);
			slot->counted.reset();
			if (plan.counted) { slot->counted = *plan.counted; }
			if (slot->count > 0) {
				// the counted form's cursor rides its one-element anchor: only its index moves, and no read goes through it
				slot->element_refs.push_back(script_value::make_element_reference(range_handle, plan.counted ? 0 : slot->begin, &eng, element_type));
				if (plan.counted) { slot->element_refs.back().element_cursor_set(slot->begin); }
				for (size_t j = 0; j < zip_nodes.size(); ++j) {
					script_value& zc = containers[j + 1].is_reference() ? containers[j + 1].deref() : containers[j + 1];
					auto& zhandle = zc.unchecked_get_array_storage();
					type_info_ptr zet = zc.get_type_info().get() ? zc.get_type_info()->element_type() : type_info_ptr(nullptr);
					slot->element_refs.push_back(script_value::make_element_reference(zhandle, slot->begin, &eng, zet));
				}
			}
			// Argument storage also belongs to the region guard. Build it before fan-out so an
			// allocation failure cannot escape the worker's entry and be swallowed by the pool.
			slot->call_args.clear();
			state.faults.checkpoint("region.worker_arguments", slot->begin);
			slot->call_args.reserve(slot->element_refs.size() + 1);
			for (const auto& cursor : slot->element_refs) { slot->call_args.push_back(cursor); }
			if (plan.counted && !slot->call_args.empty()) { slot->call_args.front() = script_value(static_cast<script_int>(0), &eng); }   // the chunk's by-value variable
			slot->call_args.emplace_back(static_cast<script_int>(slot->count), &eng);

			// Provision the captured reads into THIS worker's root env (single-threaded, charged
			// to the enclosing execute): scalars copy, strings/snapshots detach OR reuse the slot's
			// cross-region cache (content-proven against the live source), borrows mint the
			// zero-copy view. Defines are per call and nulled at the join.
			slot->capture_name_ids.clear();
			bool any_snapshot_miss = false;
			std::vector<const void*> fresh_nodes;
			for (auto& cap : captures) {
				state.faults.checkpoint("region.capture_value", k);
				script_value provisioned(std::monostate{}, &eng);
				try {
					switch (cap.kind) {
					case parallel_capture_kind::scalar:
						provisioned = *cap.source;
						break;
					case parallel_capture_kind::string:
					case parallel_capture_kind::snapshot:
						provision_snapshot_capture(*slot, cap.cache_key(), *cap.source, eng,
						                           value_types, provisioned,
						                           state.region_use_counter, snapshot_validated,
						                           fresh_nodes, any_snapshot_miss
#ifdef JAISCRIPT_VM_PROFILE
						                           , plan.prof, cap.entry->name
#endif
						                           );
						break;
					case parallel_capture_kind::borrow:
						if (type_info* t = cap.source->get_type_info().get()) { value_types.push_back(t); }
						provisioned = script_value::make_parallel_borrow(*cap.source, &eng, cap.sliced);
						break;
					case parallel_capture_kind::object:
						provisioned = captured_object_view(*cap.source, cap.host, eng, value_types);
						break;
					case parallel_capture_kind::stream:
						provisioned = stdlib::random_detail::region_copy(*cap.source, &eng, &slot->cursors, 0);
						if (auto holder = provisioned.get_object_holder(); holder && holder->is_instance()) {
							holder->unchecked_instance()->region_tag(state.region_serial);   // the worker's own copy takes its draws
						}
						cap.stream_copies.emplace_back(provisioned, &slot->cursors);
						break;
					case parallel_capture_kind::written:   // a fresh copy each region: the join reads back what this worker wrote
						if (type_info* t = cap.source->get_type_info().get()) { value_types.push_back(t); }
						provisioned = cap.shares_slots ? shared_slot_copy(*cap.source, eng) : cap.source->parallel_detached_copy(&eng);
						break;
					}
				} catch (const std::exception& e) {
					return usage("captured name '" + std::string(cap.entry->name) + "': " + e.what());
				}
				state.faults.checkpoint("region.capture_track", k);
				slot->capture_name_ids.push_back(cap.entry->name_id);
				state.faults.checkpoint("region.capture_publish", k);
				(cap.method_scope ? slot->method_env : slot->root_env)->define(cap.entry->name_id, std::move(provisioned));
			}
			state.faults.checkpoint("region.snapshot_metadata", k);
			rebuild_provisioned_nodes(*slot, any_snapshot_miss, fresh_nodes);
			// A reused slot keeps the pins of the record it last ran under (a capture of one of their names provisions over them: rebuild)
			bool keep_pins = record->serial != 0 && !slot_fresh && slot->pins_serial == record->serial;
			for (uint64_t id : slot->capture_name_ids) {
				keep_pins = keep_pins && std::find(slot->pinned_name_ids.begin(), slot->pinned_name_ids.end(), id) == slot->pinned_name_ids.end();
			}
#ifdef JAISCRIPT_VM_PROFILE
			if (plan.prof) { ++(keep_pins ? plan.prof->pins_kept : plan.prof->pins_built); }
#endif
			if (keep_pins) {
				if (slot->vm) {
					for (const auto& [name_id, name] : builder.adm.host_functions) {
						if (const script_value* copy = slot->method_env->get_own_value_ptr(name_id)) { slot->vm->adopt_math_alias(name_id, *copy); }
					}
					slot->vm->prefold_parallel_reads(engine_access::fold_epoch(eng));
				}
			} else if (auto pinned = pin_methods(*slot, k); !pinned) {
				return pinned;
			}
			slot->method_pins.definition_env = slot->method_env;
			const bool pinned = !slot->method_pins.entries.empty() || !slot->method_pins.statics.empty() || !slot->method_pins.operators.empty() ||
			                    !slot->method_pins.refused_operators.empty() || !builder.constructed_classes.empty() ||
			                    !builder.destructor_levels.empty();
			slot->backend->set_parallel_method_pins(pinned ? &slot->method_pins : nullptr);
			contexts.push_back(slot.get());
		}
		return {};
	}

	// PRE-WARM: every type shape a worker can mint, interned before the tables freeze; STREAMS: one draw from each captured random
	// seeds every worker's copy of it, so a region's draws, and the generator after it, are the same at every worker count
	void prewarm() {
		JAI_REGION_PHASE(prewarm);
		state.faults.checkpoint("region.prewarm");
		admission_builder& builder = method_adm();
		std::vector<type_info*> seed = adm->referenced_types;
		if (plan.extra_seed_adm) { seed.insert(seed.end(), plan.extra_seed_adm->referenced_types.begin(), plan.extra_seed_adm->referenced_types.end()); }
		seed.insert(seed.end(), value_types.begin(), value_types.end());
		seed.push_back(array_value.get_type_info().get());
		seed.push_back(element_type.get());
		for (const class_definition* cd : proof.element_classes) { seed.push_back(cd->get_type_info().get()); }
		std::vector<class_definition*> made;   // every class a chunk can construct, copy or destroy
		for (const auto& c : builder.constructed_classes) { made.push_back(c.cls.get()); }
		for (const class_definition* cd : builder.destructor_levels) { made.push_back(const_cast<class_definition*>(cd)); }
		for (const class_definition* cd : proof.element_classes) { made.push_back(const_cast<class_definition*>(cd)); }
		for (const class_definition* cd : captured_classes) { made.push_back(const_cast<class_definition*>(cd)); }
		for (const class_definition* cd : builder.object_classes) { made.push_back(const_cast<class_definition*>(cd)); }
		// a C++ call can hand a chunk an object of any bound class (a result, a method's receiver)
		if (!adm->host_functions.empty() || !builder.adm.host_functions.empty() || !builder.host_pins.empty() ||
		    !adm->static_calls.empty() || !builder.adm.static_calls.empty()) {
			builder.for_each_registered_class([&](const class_definition* cd) { made.push_back(const_cast<class_definition*>(cd)); });
		}
		for (class_definition* cd : made) {
			prime_construction_memos(*cd);
			engine_access::prewarm_handle_type(eng, *cd);   // a host call hands a chunk the object as a handle of its class
			seed.push_back(cd->get_type_info().get());
		}
		seed_classes_by_member(eng, adm->element_reached_members, seed);
		seed_classes_by_member(eng, builder.adm.element_reached_members, seed);
		// the same seed over the same interner, symbols and classes interns nothing new: the last region's prewarm stands
		const size_t types = engine_access::interned_type_count(eng), symbols = engine_access::get_symbolizer(eng)->size(), classes = engine_access::get_class_registry(eng).script_classes_.size();
		if (seed != pool->prewarm_seed || types != pool->prewarm_types || symbols != pool->prewarm_symbols || classes != pool->prewarm_classes) {
			prewarm_region_types(eng, seed);
			pool->prewarm_seed = std::move(seed);
			pool->prewarm_types = engine_access::interned_type_count(eng);
			pool->prewarm_symbols = engine_access::get_symbolizer(eng)->size();
			pool->prewarm_classes = engine_access::get_class_registry(eng).script_classes_.size();
		}
		for (resolved_capture& cap : captures) {
			if (cap.kind != parallel_capture_kind::stream) { continue; }
			const uint64_t stream_seed = stdlib::random_detail::draw_region_seed(*cap.source);
			for (auto& [copy, cursors] : cap.stream_copies) { stdlib::random_detail::begin_region(copy, cursors, stream_seed); }
			cap.stream_copies.clear();
		}
	}

	// REGION: pinned pool workers for chunks 1..W-1 (submit_to fixes each chunk's thread so
	// the limits table can be built before fan-out), the calling thread runs chunk 0. The pool
	// is built once per engine at the engine's worker count (calibrated then).
	void fan_out_and_join() {
		JAI_REGION_PHASE(fan_out);
		auto& table = guard->table;
		state.faults.checkpoint("region.table_entry", 0);
		table.entries.push_back({ std::this_thread::get_id(), &contexts[0]->limits, &contexts[0]->pending_instances, contexts[0]->backend.get(), &contexts[0]->method_env,
		                          &contexts[0]->deferred_reports, &contexts[0]->pending_script_layers, &contexts[0]->deferred_releases, &contexts[0]->cursors,
		                          &contexts[0]->borrows });
		if (worker_count > 1) {
			const size_t pool_workers_needed = worker_count - 1;
			if (!state.pool || state.pool->worker_count() < pool_workers_needed) {
				const auto build_start = std::chrono::steady_clock::now();
				state.faults.checkpoint("region.pool_create");
				state.pool = detail::thread_pool_access::make(
					std::max(pool_workers_needed, eng.parallel_thread_count() > 1 ? eng.parallel_thread_count() - 1 : size_t(1)), &state.faults,
					thread_pool::fast_threads);
				state.pool->spin_budget_ns(static_cast<int64_t>(state.spin_wait_us) * 1000);
				guard->pending_pool = state.pool.get();
				calibrate_pool_overhead(state);
				guard->pending_pool = nullptr;   // calibration joined each completed batch
				// Starting threads is the engine's cost, not the script's: an oversubscribed host can take seconds, so the
				// execution budget (the caller's deadline and the workers' armed ones) does not count it
				const auto built_in = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - build_start);
				eng.time().extend_execution_deadline(built_in);
				for (size_t k = 0; k < worker_count; ++k) { contexts[k]->backend->extend_execution_deadline(built_in); }
			}
			for (size_t k = 1; k < worker_count; ++k) {
				state.faults.checkpoint("region.table_entry", k);
				table.entries.push_back({ detail::thread_pool_access::worker_thread_id(*state.pool, k - 1), &contexts[k]->limits, &contexts[k]->pending_instances,
				                          contexts[k]->backend.get(), &contexts[k]->method_env, &contexts[k]->deferred_reports,
				                          &contexts[k]->pending_script_layers, &contexts[k]->deferred_releases, &contexts[k]->cursors, &contexts[k]->borrows });
			}
		}
		guard->prepared = true;
		// The interner freezes: it is not thread-safe (a miss asserts in Debug, the tripwire)
		guard->activate();
		auto& region_tasks = state.region_tasks;
		region_tasks.clear();
		state.faults.checkpoint("region.tasks_reserve");
		region_tasks.reserve(worker_count > 1 ? worker_count - 1 : 0);
		for (size_t k = 1; k < worker_count; ++k) {
			parallel_worker_slot* ctx = contexts[k];
			engine* eng_ptr = &eng;
			state.faults.checkpoint("region.task_build", k);
			region_tasks.emplace_back(k - 1, [ctx, eng_ptr] { run_worker_chunk(*ctx, *eng_ptr); });
		}
		if (!region_tasks.empty()) {
			guard->pending_pool = state.pool.get();   // BEFORE a possibly partial submission
			detail::thread_pool_access::submit_pinned_batch(*state.pool, region_tasks.data(), region_tasks.size(), &state.faults);
		}
		const auto main_chunk_t0 = std::chrono::steady_clock::now();
		run_worker_chunk(*contexts[0], eng);
		note_region_cost(*pool, main_chunk_t0, schedule.blocks ? contexts[0]->sched.ran : bounds[1] - bounds[0]);
		guard->join();
		if (schedule.blocks) {
			for (size_t k = 0; k < worker_count; ++k) { bounds[k + 1] = bounds[k] + (k < contexts.size() ? contexts[k]->sched.ran : 0); }
			state.last_chunk_bounds = bounds;
		}
	}

	// JOIN: what the chunks deferred runs on the calling thread in element order, memory rolls up, index writes come back in element
	// order up to the first failing element, and iteration order picks the error a region reports
	checked_result<void> drain() {
		JAI_REGION_PHASE(drain);
		guard->restore();   // registration below can allocate or throw
		drain_pending_instances(state, contexts);   // chunk-created instances join the class registries now, unlocked, in element order
		{
			std::vector<std::vector<deferred_release>*> deferred;
			for (auto* ctx : contexts) { deferred.push_back(&ctx->deferred_releases); }
			release_deferred_in_order(deferred);
		}
		for (auto* ctx : contexts) {
			for (const auto& [object, owner] : ctx->pending_script_layers) {
				if (!owner.expired()) { engine_access::note_script_layer(eng, object, owner); }
			}
			ctx->pending_script_layers.clear();
			const cell_counters now = ctx->backend->cells_counters();
			state.worker_cells.entries += now.entries - ctx->cells_folded.entries;
			state.worker_cells.boundaries += now.boundaries - ctx->cells_folded.boundaries;
			state.worker_cells.resumes += now.resumes - ctx->cells_folded.resumes;
			state.worker_cells.misses += now.misses - ctx->cells_folded.misses;
			state.worker_cells.row_entries += now.row_entries - ctx->cells_folded.row_entries;
			ctx->cells_folded = now;
		}
		{
			std::vector<std::vector<deferred_report>*> reports;
			for (auto* ctx : contexts) { reports.push_back(&ctx->deferred_reports); }
			std::vector<size_t> at(reports.size(), 0);
			for (size_t i = next_in_element_order(reports, at); i != SIZE_MAX; i = next_in_element_order(reports, at)) {
				const deferred_report& r = (*reports[i])[at[i]++];
				engine_access::report_script_error(eng, r.message, r.code);
			}
			for (auto* list : reports) { list->clear(); }
		}
		state.pending_registration_retry = false;
		size_t rolled_up = 0;
		for (const auto& ctx : contexts) {
			rolled_up += ctx->limits.memory_used;
		}
		if (rolled_up) {
			engine_access::execution_limits(eng).memory_charge_deferred(rolled_up);
		}
		for (const auto& ctx : contexts) {
			if (ctx->error && (!winner || ctx->error->iteration < winner->iteration)) {
				winner = &*ctx->error;
			}
		}
		copy_back_index_writes();
		// A shared object a worker stored where the caller sees it (an element, out[i], an output) leaves the region as its handle
		if (std::any_of(contexts.begin(), contexts.end(), [](const parallel_worker_slot* ctx) { return !ctx->borrows.records.empty(); })) {
			std::unordered_set<const void*> seen;
			for (auto& container : *plan.containers) { parallel_swap_borrows(const_cast<script_value&>(container.deref()), eng, seen); }
			for (const auto& iw : adm->index_writes) {
				if (const resolved_capture* target = written_capture(iw.name_id)) { parallel_swap_borrows(const_cast<script_value&>(target->source->deref()), eng, seen); }
			}
			for (auto* ctx : contexts) { ctx->borrows.clear(); }
		}
		if (winner) {
			if (winner->escaped) {
				std::exception_ptr escaped = winner->escaped;
				for (auto* ctx : contexts) { ctx->error.reset(); }
				std::rethrow_exception(escaped);
			}
			if (winner->terminal) {
				engine_access::execution_limits(eng).terminal_error = true;
			}
			std::error_code code = winner->code ? winner->code
				: make_error_code(runtime_error_code::unsupported_operation);
			return raise(code, winner->message.empty() ? std::string(plan.prefix) + ": worker failed" : winner->message);
		}
		return {};
	}
	// Index writes: each element's slot comes back from the worker that ran the element, in element order up to the first failing
	// element (the serial loop's stores before it)
	void copy_back_index_writes() {
		if (adm->index_writes.empty()) { return; }
		auto owner = [&](size_t p) -> size_t {
			if (schedule.blocks) { return schedule.owner[p / schedule.block]; }
			for (size_t k = 0; k < chunk_begin.size(); ++k) {
				if (p >= chunk_begin[k] && p < chunk_begin[k] + chunk_count[k]) { return k; }
			}
			return 0;
		};
		const size_t limit = winner ? std::min(n, winner->iteration) : n;
		for (const auto& iw : adm->index_writes) {
			const script_array* index_node = index_node_of(iw.index_param);
			const resolved_capture* target = written_capture(iw.name_id);
			if (!index_node || !target) { continue; }
			script_array* live = const_cast<script_array*>(target->source->unchecked_array_node());
			for (size_t p = 0; p < limit && p < index_count(*index_node, iw.index_param); ++p) {
				const script_value index = index_at(*index_node, iw.index_param, p);
				if (!index.is_int() || index.unchecked_as_int() < 0 || static_cast<size_t>(index.unchecked_as_int()) >= live->size()) { continue; }
				const size_t slot = static_cast<size_t>(index.unchecked_as_int());
				const size_t k = owner(p);
				if (k >= contexts.size()) { continue; }
				const script_value* copy = contexts[k]->root_env->get_own_value_ptr(iw.name_id);
				if (!copy || !copy->is_array() || slot >= copy->unchecked_array_node()->size()) { continue; }
				live->set(slot, copy->unchecked_array_node()->get(slot, &eng));
			}
		}
	}
};

checked_result<void> run_region(engine& eng, region_plan& plan) {
	region_run run(eng, plan);
	return run.run();
}
#undef JAI_REGION_PHASE

} // namespace

// parallel_transform(arr, fn[, weight_fn]) IS the zip `parallel_for (auto x, auto& o : arr, out)
// { o = fn(x); }` with the output array allocated here: fn's by-value parameter binds the
// element in place when the admission walk proved it read-only, the call result is a
// temporary that moves into the output slot (the return-site rule guarantees it aliases
// nothing fn could name), and nothing else runs per element.
checked_result<script_value> run_parallel_transform(engine& eng, const std::vector<script_value>& args) {
	parallel_engine_state& state = engine_access::parallel_state(eng);
	auto [raise, usage] = region_error_doors<checked_result<script_value>>(state);
	if (state.region_running) {
		return usage("parallel_transform: nested parallel regions are not supported");
	}
	if (args.size() < 2 || args.size() > 3) {
		return usage("parallel_transform: expected (array, fn) or (array, fn, weight_fn)");
	}
	const script_value& array_value = args[0].is_reference() ? args[0].deref() : args[0];
	if (!array_value.is_array()) {
		return usage("parallel_transform: first argument must be an array");
	}
	const script_callable* fn_payload = payload_from_function_value(args[1]);
	if (!fn_payload || fn_payload->kind != script_callable::kind_type::function || !fn_payload->fn || !fn_payload->fn->body) {
		return usage("parallel_transform: fn must be a script-defined function");
	}
	const script_callable* weight_payload = nullptr;
	if (args.size() == 3) {
		weight_payload = payload_from_function_value(args[2]);
		if (!weight_payload || weight_payload->kind != script_callable::kind_type::function ||
		    !weight_payload->fn || !weight_payload->fn->body) {
			return usage("parallel_transform: weight_fn must be a script-defined function");
		}
	}
	region_running_guard guard(state);

	const std::shared_ptr<script_defined_function> root_fn = script_defined_function::environment_form(fn_payload->fn, &eng);
	const parallel_admission* adm = admit_cached(eng, state, root_fn);
	if (!adm->admitted) {
		return raise(adm->code, adm->message);
	}
	const std::shared_ptr<function_decl>& transform_wrapper = build_transform_wrapper(eng, state, root_fn, *adm);
	const parallel_admission* weight_adm = nullptr;
	if (weight_payload) {
		if (weight_payload->fn->parameters().size() != 1 || weight_payload->fn->parameters()[0].is_reference) {
			return usage("parallel_transform: weight_fn must take exactly one parameter by value");
		}
		weight_adm = admit_cached(eng, state, script_defined_function::environment_form(weight_payload->fn, &eng));
		if (!weight_adm->admitted) {
			return raise(weight_adm->code, weight_adm->message);
		}
	}

	const script_array* input_node = array_value.unchecked_array_node();
	const size_t n = input_node->size();
	// The output: the zip's second array, one null per element, handed back as the result
	script_value out = script_value::make_array(nullptr, &eng);
	out.unchecked_get_array_storage()->values().assign(n, script_value(std::monostate{}, &eng));

	// Optional weight hint: evaluated per element at the barrier, single-threaded, on the engine's own backend, at every worker
	// count (its errors never depend on it). Chunk boundaries equalize cumulative weight.
	region_plan plan;
	if (weight_payload) {
		std::vector<double> weights(n, 0.0);
		execution_backend* backend = engine_access::get_execution_backend(eng);
		engine::external_call_guard call_guard(&eng);
		std::vector<script_value> weight_args;
		weight_args.emplace_back(std::monostate{}, &eng);
		for (size_t i = 0; i < n; ++i) {
			weight_args[0] = input_node->is_typed() ? input_node->get(i, &eng) : input_node->values()[i];
			auto r = backend->execute_callable(*weight_payload, weight_args);
			if (!r) {
				return raise(r.error(), "parallel_transform: weight_fn failed for element " +
					std::to_string(i) + ": " + format_error(r, *engine_access::get_symbolizer(eng)));
			}
			if (backend->is_unwinding()) {
				return r;   // the weight's own throw, which the backend carries on
			}
			double w;
			const script_value& wv = r.value().is_reference() ? r.value().deref() : r.value();
			if (wv.is_int()) { w = static_cast<double>(wv.unchecked_as_int()); }
			else if (wv.is_float()) { w = wv.unchecked_as_float(); }
			else {
				return usage("parallel_transform: weight_fn must return a number (element " + std::to_string(i) + ")");
			}
			if (!std::isfinite(w) || w < 0.0) {
				return usage("parallel_transform: weight_fn must return a finite non-negative number (element " + std::to_string(i) + ")");
			}
			weights[i] = w;
		}
		size_t worker_count = std::min(std::max<size_t>(eng.parallel_thread_count(), 1), n);
		if (n >= k_parallel_small_n && worker_count > 1) { plan.bounds = partition_weighted(weights, worker_count); }
	}

	std::vector<script_value> containers;
	containers.reserve(2);
	containers.push_back(array_value);
	containers.push_back(out);
	plan.prefix = "parallel_transform";
	plan.containers = &containers;
	plan.root_fn = root_fn;
	plan.adm = adm;
	plan.chunk_wrapper = &transform_wrapper;
	plan.inplace = false;
	plan.zip_arrays_fresh = true;
	plan.extra_seed_adm = weight_adm;
#ifdef JAISCRIPT_VM_PROFILE
	plan.prof = &state.prof_transform;
#endif
	auto ran = run_region(eng, plan);
	if (!ran) {
		return checked_result<script_value>(ran.error(), std::string_view(state.error_text));
	}
	return out;
}

// parallel_reduce(xs, init, step, combine): xs splits into chunks whose size depends on xs.size() alone; a region over the chunks'
// accumulators (each init's copy) folds every chunk's elements in order with step, its body grafted into the chunk loop (a
// `T& acc` step mutates the accumulator in place, a by-value step's returns store into it), and the chunk results combine left
// to right, so the result is the same on every machine. step is the region's body: lambdas and their captures follow
// parallel_for's capture rules, writes to enclosing state are refused
namespace {
	inline constexpr size_t k_reduce_chunks = 64;      // at most this many chunks
	inline constexpr size_t k_reduce_min_chunk = 16;   // elements per chunk at least

	size_t reduce_chunk_size(size_t n) { return std::max(k_reduce_min_chunk, (n + k_reduce_chunks - 1) / k_reduce_chunks); }

	// The chunk loop per mode: __c0 the chunk's accumulator, __c1 its chunk number; the graft forms declare step's own
	// parameters per element (__a, by value only, and __x) in front of the placeholder, the call forms call the step
	constexpr const char* k_reduce_chunk_head =
		"function __jai_reduce_chunk(var& __c0, var& __c1, int __n) { for (int __k = 0; __k < __n; __k = __jai_parallel_advance(__k)) { "
		"int __lo = __c1 * __jai_reduce_s; int __hi = __lo + __jai_reduce_s; if (__hi > __jai_reduce_n) { __hi = __jai_reduce_n; } "
		"for (int __i = __lo; __i < __hi; __i++) { ";
	constexpr const char* k_reduce_chunk_tail = " } } }";

	// Every identifier and declaration of the skeleton (the graft not yet in): slots move past step's frame, __c0 / __a / __x
	// become step's own parameters where the mode binds them
	struct reduce_slot_map {
		size_t base = 0;
		bool in_place = false;
		const parameter* acc = nullptr;
		const parameter* element = nullptr;
		std::string_view acc_name;       // the symbolizer's copies: an AST name outlives the parameters it was mapped from
		std::string_view element_name;
	};
	void map_reduce_expr(expression_ptr& e, const reduce_slot_map& m);
	void map_reduce_decl(declaration_ptr& d, const reduce_slot_map& m);
	void map_reduce_var(variable_decl* v, const reduce_slot_map& m) {
		map_reduce_expr(v->initializer, m);
		const parameter* as = v->name == "__a" ? m.acc : v->name == "__x" ? m.element : nullptr;
		if (as) {
			v->name = as == m.acc ? m.acc_name : m.element_name;
			v->name_id = as->symbol_id;
			v->slot_index = as == m.acc ? 0 : 1;
			v->type = as->type;
			v->decl_fast_flags = variable_decl::classify_decl_fast(v->type);
		} else if (v->slot_index != SIZE_MAX) {
			v->slot_index += m.base;
		}
	}
	void map_reduce_stmt(statement_ptr& s, const reduce_slot_map& m) {
		if (!s) { return; }
		switch (s->get_type()) {
		case node_type::block_stmt:
			for (auto& d : static_cast<block_stmt*>(s.get())->declarations) { map_reduce_decl(d, m); }
			return;
		case node_type::for_stmt: {
			auto* n = static_cast<for_stmt*>(s.get());
			map_reduce_decl(n->initializer, m); map_reduce_expr(n->condition, m); map_reduce_expr(n->update, m); map_reduce_stmt(n->body, m);
			return;
		}
		case node_type::if_stmt: {
			auto* n = static_cast<if_stmt*>(s.get());
			map_reduce_expr(n->condition, m); map_reduce_stmt(n->then_statement, m); map_reduce_stmt(n->else_statement, m);
			return;
		}
		case node_type::variable_decl:
			map_reduce_var(static_cast<variable_decl*>(s.get()), m);
			return;
		case node_type::expression_stmt:
			map_reduce_expr(static_cast<expression_stmt*>(s.get())->expression, m);
			return;
		default:
			return;
		}
	}
	void map_reduce_decl(declaration_ptr& d, const reduce_slot_map& m) {
		if (!d) { return; }
		if (d->get_type() == node_type::statement_decl) { map_reduce_stmt(static_cast<statement_decl*>(d.get())->statement, m); return; }
		if (d->get_type() == node_type::expression_decl) { map_reduce_expr(static_cast<expression_decl*>(d.get())->expression, m); return; }
		if (d->get_type() == node_type::variable_decl) { map_reduce_var(static_cast<variable_decl*>(d.get()), m); }
	}
	void map_reduce_expr(expression_ptr& e, const reduce_slot_map& m) {
		if (!e) { return; }
		switch (e->get_type()) {
		case node_type::identifier_expr: {
			auto* id = static_cast<identifier_expr*>(e.get());
			if (id->name == "__c0" && m.in_place) {
				id->name = m.acc_name;
				id->symbol_id = m.acc->symbol_id;
				id->slot_index = 0;
			} else if (id->slot_index != SIZE_MAX) {
				id->slot_index += m.base;
			}
			return;
		}
		case node_type::binary_expr: { auto* b = static_cast<binary_expr*>(e.get()); map_reduce_expr(b->left, m); map_reduce_expr(b->right, m); return; }
		case node_type::unary_expr: map_reduce_expr(static_cast<unary_expr*>(e.get())->operand, m); return;
		case node_type::assignment_expr: { auto* a = static_cast<assignment_expr*>(e.get()); map_reduce_expr(a->target, m); map_reduce_expr(a->value, m); return; }
		case node_type::call_expr: {
			auto* c = static_cast<call_expr*>(e.get());
			map_reduce_expr(c->callee, m);
			for (auto& arg : c->arguments) { map_reduce_expr(arg, m); }
			return;
		}
		default:
			return;
		}
	}

	// The chunk loop for step, built once per step body: step's body grafted when it returns only from its top level, else the
	// call form (a return inside a nested loop keeps its meaning)
	const std::shared_ptr<function_decl>& build_reduce_wrapper(engine& eng, parallel_engine_state& state, const std::shared_ptr<script_defined_function>& step) {
		auto found = state.reduce_wrappers.find(static_cast<const void*>(step->body.get()));
		if (found != state.reduce_wrappers.end()) { return found->second; }
		const auto& params = step->parameters();
		const bool in_place = params[0].is_reference;
		body_control_scan scan;
		statement_ptr body_view = step->body;
		scan_parallel_body_control(body_view, 0, scan);
		const bool graft = !scan.nested_return;
		std::string source = k_reduce_chunk_head;
		if (!graft) {
			source += in_place ? "__jai_parallel_body(__c0, __jai_reduce_xs[__i]);" : "__c0 = __jai_parallel_body(__c0, __jai_reduce_xs[__i]);";
		} else {
			source += in_place ? "var __x = __jai_reduce_xs[__i]; __jai_parallel_placeholder();" : "var __a = __c0; var __x = __jai_reduce_xs[__i]; __jai_parallel_placeholder();";
		}
		source += k_reduce_chunk_tail;
		auto decl = parse_chunk_skeleton(eng, source.c_str());
		if (!graft) {
			return state.reduce_wrappers.emplace(static_cast<const void*>(step->body.get()), std::move(decl)).first->second;
		}
		parameter acc = params[0];
		parameter element = params[1];
		if (acc.symbol_id == UINT64_MAX) { acc.symbol_id = engine_access::symbolize(eng, acc.name); }
		if (element.symbol_id == UINT64_MAX) { element.symbol_id = engine_access::symbolize(eng, element.name); }
		reduce_slot_map map;
		map.base = step->local_count;   // step's frame: its two parameters and its locals
		map.in_place = in_place;
		map.acc = &acc;
		map.element = &element;
		map.acc_name = engine_access::get_symbolizer(eng)->get_string(acc.symbol_id);
		map.element_name = engine_access::get_symbolizer(eng)->get_string(element.symbol_id);
		const size_t skeleton_slots = decl->local_count;
		for (size_t j = 0; j < decl->parameters.size(); ++j) { decl->parameters[j].slot_index += map.base; }
		if (in_place) {   // the chunk's accumulator IS step's acc
			parameter bound = acc;
			bound.slot_index = 0;
			bound.is_reference = true;
			decl->parameters[0] = bound;
		}
		statement_ptr skeleton_body = decl->body;
		map_reduce_stmt(skeleton_body, map);
		transform_return_ctx rctx;
		rctx.eng = &eng;
		if (in_place) {
			rctx.discard_value = true;   // the accumulator is already written: a return only ends the element
		} else {
			rctx.out_name = std::string_view("__c0");
			rctx.out_symbol = engine_access::symbolize(eng, "__c0");
			rctx.out_slot = map.base;
			const return_conv conv = classify_return_conv(step->return_type.get());
			if (conv != return_conv::none && conv != return_conv::ref) { rctx.cast_to = step->return_type; rctx.cast_owner = step->name; }
			collect_return_proofs(body_view, rctx.return_proofs);
		}
		statement_ptr grafted = clone_statement(eng, body_view, *step);
		rewrite_transform_returns(grafted, 0, rctx);
		// the placeholder sits in the innermost loop: replace it wherever the walk finds it
		std::function<void(statement_ptr&)> place = [&](statement_ptr& s) {
			if (!s) { return; }
			if (s->get_type() == node_type::for_stmt) { place(static_cast<for_stmt*>(s.get())->body); return; }
			if (s->get_type() != node_type::block_stmt) { return; }
			for (auto& bd : static_cast<block_stmt*>(s.get())->declarations) {
				if (bd->get_type() == node_type::statement_decl) { place(static_cast<statement_decl*>(bd.get())->statement); continue; }
				if (bd->get_type() != node_type::expression_decl) { continue; }
				auto* ed = static_cast<expression_decl*>(bd.get());
				if (ed->expression && ed->expression->get_type() == node_type::call_expr) {
					auto* call = static_cast<call_expr*>(ed->expression.get());
					if (call->callee->get_type() == node_type::identifier_expr &&
					    static_cast<identifier_expr*>(call->callee.get())->name == "__jai_parallel_placeholder") {
						bd = std::make_shared<statement_decl>(bd->location, grafted);
					}
				}
			}
		};
		place(skeleton_body);
		decl->local_count = map.base + skeleton_slots;
		return state.reduce_wrappers.emplace(static_cast<const void*>(step->body.get()), std::move(decl)).first->second;
	}
}

checked_result<script_value> run_parallel_reduce(engine& eng, const std::vector<script_value>& args) {
	parallel_engine_state& state = engine_access::parallel_state(eng);
	auto [raise, usage] = region_error_doors<checked_result<script_value>>(state);
	if (state.region_running) {
		return usage("parallel_reduce: nested parallel regions are not supported");
	}
	if (args.size() != 4) {
		return usage("parallel_reduce: expected (array, init, step, combine)");
	}
	const script_value& xs = args[0].is_reference() ? args[0].deref() : args[0];
	if (!xs.is_array()) {
		return usage("parallel_reduce: first argument must be an array");
	}
	const script_value& init = args[1].is_reference() ? args[1].deref() : args[1];
	const script_callable* callbacks[2] = { payload_from_function_value(args[2]), payload_from_function_value(args[3]) };
	const char* const roles[2] = { "step", "combine" };
	const char* const shapes[2] = { "(acc, x)", "(a, b)" };
	for (size_t k = 0; k < 2; ++k) {
		const script_callable* cb = callbacks[k];
		if (!cb || cb->kind != script_callable::kind_type::function || !cb->fn || !cb->fn->body) {
			return usage(std::string("parallel_reduce: ") + roles[k] + " must be a script function or lambda");
		}
		if (cb->fn->parameters().size() != 2) {
			return usage(std::string("parallel_reduce: ") + roles[k] + " must take two parameters " + shapes[k]);
		}
	}
	const std::shared_ptr<script_defined_function> step = script_defined_function::environment_form(callbacks[0]->fn, &eng);
	if (step->parameters()[1].is_reference) {
		return usage("parallel_reduce: step takes its element by value (the elements are the input's, never written)");
	}
	execution_backend* backend = engine_access::get_execution_backend(eng);
	const size_t n = xs.unchecked_array_node()->size();
	const uint64_t xs_id = engine_access::symbolize(eng, "__jai_reduce_xs"), s_id = engine_access::symbolize(eng, "__jai_reduce_s"), n_id = engine_access::symbolize(eng, "__jai_reduce_n");
	const parallel_admission* adm = nullptr;
	std::shared_ptr<function_decl> wrapper;
	{
		region_running_guard guard(state);
		const parallel_admission* step_adm = admit_cached(eng, state, step, /*inplace_root*/ true, "parallel_reduce");
		if (!step_adm->admitted) {
			return raise(step_adm->code, step_adm->message);
		}
		parallel_engine_state::reduce_admission& cached = state.reduce_admissions[static_cast<const void*>(step->body.get())];
		if (cached.generation == 0 || cached.generation != step_adm->generation) {
			for (const auto& capture : step_adm->captures) {
				if (capture.name_id == xs_id) { return usage("parallel_reduce: step names '__jai_reduce_xs', the reduction's own input"); }
			}
			cached.adm = *step_adm;
			// the chunk loop's own reads ride the captures: the input (a borrow when it holds primitives), the chunk size and the count
			auto* sym = engine_access::get_symbolizer(eng);
			cached.adm.captures.push_back({ xs_id, sym->get_string(xs_id), true });
			cached.adm.captures.push_back({ s_id, sym->get_string(s_id), true });
			cached.adm.captures.push_back({ n_id, sym->get_string(n_id), true });
			// the graft declares step's parameters and stores its results in the chunk loop: their shapes warm with the region's
			for (const auto& p : step->parameters()) {
				if (p.type) { cached.adm.referenced_types.push_back(p.type.get()); }
				if (const type_info* declared = parameter_declared_ref(p)) { engine_access::get_type_info_declared_reference(eng, const_cast<type_info*>(declared)); }   // an escaping T& parameter's bind marks its reference with it
			}
			if (step->return_type) { cached.adm.referenced_types.push_back(step->return_type.get()); }
			cached.generation = step_adm->generation;
		}
		adm = &cached.adm;
		wrapper = build_reduce_wrapper(eng, state, step);
	}
	if (n == 0) {
		return clone_for_assignment(init);
	}
	const size_t chunk = reduce_chunk_size(n);
	const size_t chunks = (n + chunk - 1) / chunk;
	script_value accs = script_value::make_array(nullptr, &eng);
	script_value ids = script_value::make_array(nullptr, &eng);
	{
		auto& acc_values = accs.unchecked_get_array_storage()->values();
		auto& id_values = ids.unchecked_get_array_storage()->values();
		acc_values.reserve(chunks);
		id_values.reserve(chunks);
		for (size_t c = 0; c < chunks; ++c) {
			acc_values.push_back(clone_for_assignment(init));   // each chunk's own copy of init
			id_values.emplace_back(static_cast<script_int>(c), &eng);
		}
	}
	// the chunk loop's reads resolve beside step's closure, so a lambda's captures provision as parallel_for's do
	auto capture_env = std::make_shared<environment>(step->closure_env ? step->closure_env : engine_access::get_global_environment(eng), engine_access::get_symbolizer(eng));
	capture_env->define(xs_id, xs);
	capture_env->define(s_id, script_value(static_cast<script_int>(chunk), &eng));
	capture_env->define(n_id, script_value(static_cast<script_int>(n), &eng));

	std::vector<script_value> containers;
	containers.reserve(2);
	containers.push_back(accs);
	containers.push_back(ids);
	{
		region_running_guard guard(state);
		region_plan plan;
		plan.prefix = "parallel_reduce";
		plan.containers = &containers;
		plan.root_fn = step;
		plan.adm = adm;
		plan.chunk_wrapper = &wrapper;
		plan.capture_env = capture_env;
		plan.inplace = true;
		plan.zip_arrays_fresh = true;   // the chunk ids, made above
		plan.sliced_input = xs_id;
		auto ran = run_region(eng, plan);
		if (!ran) {
			return checked_result<script_value>(ran.error(), std::string_view(state.error_text));
		}
	}
	// left to right in chunk order: the one combine order on every machine
	engine::external_call_guard call_guard(&eng);
	const script_array* chunk_results = accs.unchecked_array_node();
	script_value result = chunk_results->get(0, &eng);
	std::vector<script_value> combine_args(2, script_value(std::monostate{}, &eng));
	for (size_t c = 1; c < chunks; ++c) {
		combine_args[0] = std::move(result);
		combine_args[1] = chunk_results->get(c, &eng);
		auto combined = backend->execute_callable(*callbacks[1], combine_args);
		if (!combined || backend->is_unwinding()) {
			return combined;
		}
		result = combined.value().is_reference() ? script_value(combined.value().deref()) : std::move(combined.value());
	}
	return result;
}

// parallel_for (see parallel_transform.hpp): the in-place fork-join statement over one array
// or the zip of several, the body run in place on every thread's segment.
checked_result<void> run_parallel_for(engine& eng, parallel_for_stmt* stmt,
                                      std::vector<script_value>& containers,
                                      const std::shared_ptr<environment>& capture_env,
                                      const std::function<const script_value*(size_t)>& frame_slot) {
	parallel_engine_state& state = engine_access::parallel_state(eng);
	auto [raise, usage] = region_error_doors<checked_result<void>>(state);
	if (state.region_running) {
		return usage("parallel_for: nested parallel regions are not supported");
	}
	const script_value& array_value = containers.front().is_reference() ? containers.front().deref() : containers.front();
	if (!array_value.is_array()) {
		return usage("parallel_for: can only iterate an array");
	}

	// The shared body function (admission + pool key + per-worker provisioning source). Hot
	// reload reparses into a fresh statement node, so this cache cannot go stale. Built once
	// per statement together with the body's control-flow mode.
	if (!stmt->body_fn_cache) {
		body_control_scan scan;
		scan_parallel_body_control(stmt->body, 0, scan);
		stmt->chunk_mode = scan.nested_return ? 2 : 1;
		rewrite_parallel_body_control(stmt->body, 0, stmt->chunk_mode == 1);
		auto body_block = std::dynamic_pointer_cast<block_stmt>(stmt->body);
		if (!body_block) {
			return usage("parallel_for: body is not a block (internal)");
		}
		std::vector<parameter> loop_params{ stmt->loop_param };
		for (const auto& extra : stmt->extra_params) { loop_params.push_back(extra); }
		stmt->body_fn_cache = std::make_shared<script_defined_function>(
			std::string_view("<parallel_for>"), std::move(loop_params),
			type_info_ptr(nullptr), std::move(body_block), nullptr, stmt->local_count);
		stmt->body_fn_cache->unit = engine_access::current_unit(eng);
	}
	const std::shared_ptr<script_defined_function>& body_fn = stmt->body_fn_cache;

	region_running_guard guard(state);

	const parallel_admission* adm = admit_cached(eng, state, body_fn, /*inplace_root*/ true);
	if (!adm->admitted) {
		return raise(adm->code, adm->message);
	}
	if (stmt->counted() && adm->loop_variable_write) {
		return usage("parallel_for: " + counted_variable_written(stmt->loop_param.name) + " at " + std::to_string(adm->loop_variable_write->location.line) + ":" +
		             std::to_string(adm->loop_variable_write->location.column));
	}
	const std::shared_ptr<function_decl>& chunk_wrapper = build_chunk_wrapper(eng, stmt, *adm);

	region_plan plan;
	plan.prefix = "parallel_for";
	parallel_counted_sequence counted;
	if (stmt->counted()) {
		plan.counted_variable = stmt->loop_param.name;
		const script_array* described = array_value.unchecked_array_node();   // parallel_counted_range's [start, up, size, n]
		counted.start = described->get(0, &eng).as_int();
		counted.up = described->get(1, &eng).as_int() != 0;
		counted.size = static_cast<uint64_t>(described->get(2, &eng).as_int());
		counted.n = static_cast<size_t>(described->get(3, &eng).as_int());
		plan.counted = &counted;
		script_value anchor = script_value::make_array(nullptr, &eng);
		anchor.unchecked_get_array_storage()->values().emplace_back(static_cast<script_int>(0), &eng);
		containers.front() = std::move(anchor);
	}
	plan.containers = &containers;
	plan.root_fn = body_fn;
	plan.adm = adm;
	plan.chunk_wrapper = &chunk_wrapper;
	plan.capture_env = capture_env;
	plan.frame_captures = &stmt->frame_captures;
	plan.frame_slot = &frame_slot;
	plan.inplace = true;
#ifdef JAISCRIPT_VM_PROFILE
	plan.prof = &state.prof_for;
#endif
	return run_region(eng, plan);
}

// Tier-1 raw read through a region borrow (see parallel_transform.hpp). Runs on worker
// threads: every message below is a static literal (checked_result never allocates) and
// NOTHING here interns against the frozen symbolizer or copies a shared handle -
// primitive elements mint plain values, anything heavier rides the silent deep clone.
bool parameter_alias_allows(const script_value& derefed) {
	switch (derefed.raw_storage_index()) {
	case script_value::TYPEID_ARRAY:
	case script_value::TYPEID_MAP:
		return true;
	case script_value::TYPEID_OBJECT: {
		const auto& holder = derefed.unchecked_object_handle();
		if (!holder || !holder->is_instance()) { return false; }
		const auto* inst = holder->unchecked_instance();
		const class_definition* cls = inst->get_class_definition();
		return cls && cls->is_script_class() && !cls->nocopy();
	}
	default:
		return false;
	}
}

uint32_t prove_readonly_parameters(engine& eng, const script_defined_function& fn) {
	if (!fn.body) { return 0; }
	// Non-owning view: the builder pins call targets for its own lifetime only
	std::shared_ptr<script_defined_function> view(std::shared_ptr<void>{}, const_cast<script_defined_function*>(&fn));
	uint32_t mask = 0;
	const size_t n = std::min<size_t>(fn.parameters().size(), 32);
	for (size_t i = 0; i < n; ++i) {
		try {
			if (prove_readonly_parameter(eng, view, i)) { mask |= (1u << i); }
		} catch (...) {
			// an unresolvable callee or a frozen symbolizer means no proof, never an error
		}
	}
	return mask;
}

void prove_local_aliases(engine& eng, const local_alias_home& home) {
	std::fill(home.verdicts.begin(), home.verdicts.end(), uint8_t{0});   // a verdict a raising walk never reaches keeps the copy
	local_alias_prover prover(eng, home);
	try {
		prover.run();
	} catch (...) {
		// an unresolvable callee or a frozen symbolizer means no proof, never an error (a verdict not reached stays false)
	}
}

void stamp_local_alias_homes(const script_defined_function& fn) {
	if (!fn.body || !fn.shared_parameters) { return; }
	const statement_ptr body = fn.body;
	if (!body_has_local_alias_candidate(body)) { return; }
	auto home = std::make_shared<local_alias_home>();
	home->params = fn.shared_parameters;
	home->return_type = fn.return_type;
	home->body = fn.body;
	stamp_local_aliases(home, body);
}

bool resolve_global_ref_write_callee(engine& eng, const identifier_expr& id, std::vector<ref_write_callee>& out) {
	uint64_t symbol = id.symbol_id;
	if (symbol == UINT64_MAX) { symbol = engine_access::get_symbolizer(eng)->intern(std::string(id.name)); }
	const script_value* value = engine_access::get_global_environment(eng)->get_value_ptr(symbol);
	if (!value || !value->deref().is_function()) { return false; }
	const script_callable* payload = payload_from_function_value(value->deref());
	if (!payload || payload->kind != script_callable::kind_type::function || !payload->fn || payload->fn->name == "<lambda>") { return false; }
	const script_defined_function& target = *payload->fn;
	out.push_back(ref_write_callee{&target.parameters(), target.body.get(), target.name, false, false});
	return true;
}

bool ref_parameter_takes_temporary(engine& eng, const std::vector<parameter>& params, const statement* body, size_t i, bool may_prove) {
	if (i < params.size() && params[i].is_reference && params[i].is_const) { return true; }   // const T&
	if (i >= params.size() || !params[i].is_reference || !body) { return false; }
	const parameter& p = params[i];
	const uint64_t epoch = engine_access::definition_epoch(eng);
	if (p.temporary_epoch != epoch) {
		if (!may_prove) { return false; }
		ref_write_analysis analysis([&eng](const identifier_expr& id, std::vector<ref_write_callee>& out) {
			return resolve_global_ref_write_callee(eng, id, out);
		});
		p.takes_temporary = !analysis.analyze(ref_write_callee{&params, body, {}, false, false}, i).writes;
		p.temporary_epoch = epoch;
	}
	return p.takes_temporary;
}

const script_string* parallel_borrow_string_source(const script_value& borrow, const script_value& index_raw) noexcept {
	const script_value& index = index_raw.deref();
	const auto* arr = borrow.parallel_borrow_array();
	if (!arr || !index.is_int()) { return nullptr; }
	const script_int i = index.unchecked_as_int();
	if (i < 0 || i >= static_cast<script_int>(arr->size())) { return nullptr; }
	if (arr->is_typed()) {
		if (arr->kind() != script_array::kind_t::str) { return nullptr; }
		return arr->strs()[static_cast<size_t>(i)].get();
	}
	const script_value& elem = arr->values()[static_cast<size_t>(i)];
	if (elem.raw_storage_index() != script_value::TYPEID_STRING) { return nullptr; }
	return elem.unchecked_string_handle().get();
}

checked_result<script_value> parallel_counted_range(engine& eng, const std::vector<script_value>& args) {
	parallel_engine_state& state = engine_access::parallel_state(eng);
	auto refuse = [&state](std::error_code code, std::string text) {
		state.error_text = std::move(text);
		return checked_result<script_value>(code, std::string_view(state.error_text));
	};
	if (args.size() != 5) { return refuse(make_error_code(runtime_error_code::argument_count_mismatch), "parallel_for: counted range takes five arguments (internal)"); }
	static const char* const roles[] = { "start", "bound", "step" };
	script_int v[3] = {};
	for (size_t slot = 0; slot < 3; ++slot) {
		const script_value& arg = args[slot].deref();
		if (!arg.is_int()) {
			return refuse(make_error_code(runtime_error_code::type_mismatch), std::string("parallel_for: a counted loop's ") + roles[slot] + " is " +
			              slot_source_type_name(arg, &eng) + ", not int");
		}
		v[slot] = arg.unchecked_as_int();
	}
	const script_int a = v[0], b = v[1], k = v[2];
	const script_int comparison = args[3].deref().as_int();
	const script_int step_op = args[4].deref().as_int();   // 1 ++, -1 --, 2 +=, -2 -=
	script_value result = script_value::make_array(type_info_ptr(engine_access::get_type_info_int(eng)), &eng);
	auto& described = result.unchecked_get_array_storage()->ints();
	described.assign(4, 0);   // [start, up, size, n]: zero elements until the bound holds
	described[0] = a;
	const bool holds = comparison == 0 ? a < b : comparison == 1 ? a <= b : comparison == 2 ? a > b : a >= b;
	if (!holds) { return result; }   // the serial loop runs zero times
	const bool up = step_op > 0 ? k > 0 : k < 0;
	auto never_ends = [&]() {
		return refuse(make_error_code(runtime_error_code::unsupported_operation), "parallel_for: a counted loop's step of " + std::to_string(k) +
		              " never reaches its bound (the serial loop would not end)");
	};
	if (k == 0 || up != (comparison <= 1)) { return never_ends(); }
	// in uint64: the distance to the bound, the step's size and the serial loop's whole travel (a .. its value after the last step)
	const uint64_t distance = up ? static_cast<uint64_t>(b) - static_cast<uint64_t>(a) : static_cast<uint64_t>(a) - static_cast<uint64_t>(b);
	const uint64_t size = k > 0 ? static_cast<uint64_t>(k) : static_cast<uint64_t>(-(k + 1)) + 1;
	const bool inclusive = comparison == 1 || comparison == 3;
	const uint64_t whole = distance / size;
	const bool overflows_count = inclusive && whole == UINT64_MAX;
	const uint64_t n = inclusive ? whole + 1 : whole + (distance % size != 0);
	const uint64_t room = up ? static_cast<uint64_t>(std::numeric_limits<script_int>::max()) - static_cast<uint64_t>(a)
	                         : static_cast<uint64_t>(a) - static_cast<uint64_t>(std::numeric_limits<script_int>::min());
	if (overflows_count || n > room / size) {
		if (!kCheckedOverflow) { return never_ends(); }   // the serial loop wraps past its bound
		static const char* const overflow[] = { "Integer overflow in '-='", "Integer overflow in '--'", "", "Integer overflow in '++'", "Integer overflow in '+='" };
		return refuse(make_error_code(runtime_error_code::invalid_numeric_operand), overflow[step_op + 2]);
	}
	described[1] = up ? 1 : 0;
	described[2] = static_cast<script_int>(size);
	described[3] = static_cast<script_int>(n);
	return result;
}

bool parallel_borrow_count_method(std::string_view name) noexcept {
	return name == "size" || name == "length" || name == "empty";
}

std::optional<script_value> parallel_borrow_count(const script_value& borrow, std::string_view name, size_t argc, engine* eng) {
	if (argc != 0 || !parallel_borrow_count_method(name)) { return std::nullopt; }
	size_t count = 0;
	if (const auto* arr = borrow.parallel_borrow_array()) { count = arr->size(); }
	else if (const auto* map = borrow.parallel_borrow_map()) { count = map->size(); }
	else { return std::nullopt; }
	return name == "empty" ? script_value(count == 0, eng) : script_value(static_cast<script_int>(count), eng);
}

JAI_NOINLINE checked_result<script_value> parallel_borrow_method_refusal() {
	return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation),
		"parallel borrow method other than size, length or empty (internal)");
}

checked_result<script_value> parallel_borrow_subscript_read(const script_value& borrow,
                                                            const script_value& index_raw,
                                                            engine* eng,
                                                            bool lvalue_write) {
	if (lvalue_write) {
		// The runtime write wall: admission catches the visible shapes with positions;
		// this is the chokepoint that makes the borrow sound regardless
		return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation),
			"cannot write enclosing state in a parallel body (captured containers are read-only)");
	}
	const script_value& index = index_raw.deref();
	if (const auto* arr = borrow.parallel_borrow_array()) {
		if (!index.is_int()) {
			return checked_result<script_value>(make_error_code(runtime_error_code::invalid_index_type),
				"Array index must be an integer");
		}
		const script_int i = index.unchecked_as_int();
		if (i < 0 || i >= static_cast<script_int>(arr->size())) {
			// Static text (no interned numbers): worker threads may not intern, and the
			// twins' numeric form would - the captured-array read names the condition
			return checked_result<script_value>(make_error_code(runtime_error_code::index_out_of_bounds),
				"Array index out of bounds reading a captured array in a parallel body");
		}
		if (arr->is_typed()) {
			if (arr->kind() == script_array::kind_t::str) {
				// the bytes, never the handle: the live string's count belongs to the main thread
				const auto& str = arr->strs()[static_cast<size_t>(i)];
				return str ? script_value(script_string(*str), eng) : script_value(script_string(), eng);
			}
			// raw buffer read -> worker-local mint, no script_value even exists to copy
			return arr->get(static_cast<size_t>(i), eng);
		}
		const script_value& elem = arr->values()[static_cast<size_t>(i)];
		if (borrow.is_sliced_parallel_borrow()) { return script_value(elem); }   // the element's own handle: this worker alone reads it
		switch (elem.raw_storage_index()) {
		case script_value::TYPEID_NULL:
		case script_value::TYPEID_INT:
		case script_value::TYPEID_FLOAT:
		case script_value::TYPEID_CHAR:
		case script_value::TYPEID_BOOL:
			return script_value(elem);   // raw const read -> worker-local mint, zero refcount traffic
		case script_value::TYPEID_STRING:
			return script_value(script_string(*elem.unchecked_string_handle()), eng);   // bytes, not the handle
		default:
			return elem.parallel_detached_copy(eng);   // tier 3 (defensive: borrows are primitives and strings)
		}
	}
	if (const auto* map = borrow.parallel_borrow_map()) {
		auto it = map->find(index);
		if (it == map->end()) { return map_key_missing_error(engine_access::get_symbolizer(*eng), nullptr, index); }
		const script_value& elem = it->second;
		switch (elem.raw_storage_index()) {
		case script_value::TYPEID_NULL:
		case script_value::TYPEID_INT:
		case script_value::TYPEID_FLOAT:
		case script_value::TYPEID_CHAR:
		case script_value::TYPEID_BOOL:
			return script_value(elem);
		case script_value::TYPEID_STRING:
			return script_value(script_string(*elem.unchecked_string_handle()), eng);   // bytes, not the handle
		default:
			return elem.parallel_detached_copy(eng);
		}
	}
	return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation),
		"parallel borrow subscript on a non-container value (internal)");
}

} // namespace jai::detail
