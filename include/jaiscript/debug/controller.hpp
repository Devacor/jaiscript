#pragma once

// JaiScript step-debugger core. Transport-agnostic and protocol-agnostic: it owns the
// execution hook, the breakpoint table, the step state machine, and the park/pump loop
// that blocks the script thread at a stop while a *different* thread (a DAP connector,
// or a test) drives resume/step and inspects state via posted closures. Pure std + an
// engine*; no sockets, no DAP.

#include <jaiscript/core/value.hpp>
#include <jaiscript/core/engine_time.hpp>
#include <jaiscript/detail/atomic_shared_ptr.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jai {
    class engine;
    class ast_node;
    class declaration;

namespace debug {

enum class step_mode : int { none = 0, into, over, out };

struct stop_info {
    std::string reason;   // "breakpoint" | "step" | "pause"
    std::string file;
    int line = 0;
    int depth = 0;
};

// file path -> the lines that carry a breakpoint (sorted, deduped)
using breakpoint_table = std::unordered_map<std::string, std::vector<int>>;

// One stack frame at a stop (parked-thread inspection result), named as an error trace names it: the function,
// "<lambda>" for a lambda, "<script>" for the top level.
struct frame_info {
    std::string name;
    std::string file;
    int line = 0;
};

// One in-scope variable at a stop: its name, its value as display text and its kind ("int", "string", "array", "object", ...)
struct variable_info {
    std::string name;
    std::string value;
    std::string type;
};

// Optional transport an engine drives its controller through (a DAP socket server, a
// test harness, ...). Kept abstract in core so the always-built library never links a
// socket: jai::debug::debug_connector (optional, JAISCRIPT_ENABLE_DEBUGGER) derives from it.
class transport {
public:
    virtual ~transport() = default;
    virtual void start(engine* eng) = 0;   // begin serving; wire to eng->debugger()
    virtual void stop() = 0;               // detach the controller, join, release resources
};

// One controller per engine (engine::debugger()). All "control-side" methods are safe to
// call from a thread other than the one running the script; inspection methods must run
// on the parked script thread (post them via post_command()).
//
// Cost model: with no session armed the
// backends never call in here per statement; armed, the per-statement gate reads the hot cache
// below, with no locks and no string hashing. Transport-side writes (enable, breakpoints,
// pause) land in atomics, raise sync_requested_ (which also forces the gate's slow path and
// wakes a sleeping script thread), and are pulled into the cache by sync_hot_state() at the
// debug sync points: execute entry, the 1024-tick budget sample, a sleep, park exit and, while
// a session is armed, the next statement.
class controller {
public:
    explicit controller(engine* eng);

    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    bool is_paused() const noexcept { return paused_.load(std::memory_order_acquire); }

    // ---- control-side (transport thread) ----
    // Arming also suspends the execution budget (a paused script must not trip the
    // wall-clock cap) and restores it on disable.
    void set_enabled(bool on);
    // Native labels are exact. Source paths (DAP) normalize only on the script thread at sync, and only while the engine
    // permits filesystem access. A line that starts no statement binds to the next one that does (breakpoint_line).
    void set_breakpoints(const std::string& file, std::vector<int> lines);
    void set_source_breakpoints(const std::string& file, std::vector<int> lines);
    void clear_breakpoints();
    void request_pause();                              // stop at the next statement
    void resume();                                     // continue
    void step_over();
    void step_into();
    void step_out();
    void post_command(std::function<void()> fn);       // run on the parked thread; wakes it
    // post_command only while the script thread is parked, decided under the park's lock: false (fn dropped) once it has left
    bool post_command_while_paused(std::function<void()> fn);
    void detach();                                     // force-resume + disarm (disconnect/shutdown)
    void set_on_stopped(std::function<void(const stop_info&)> cb);
    // Whether a source-path breakpoint on `path` can bind: "<script>", or a unit the engine has
    // parsed under that path (spelled as given, or normalized like set_source_breakpoints does).
    bool source_known(const std::string& path) const;
    // The line a breakpoint on `line` of `path` binds to: the first line at or after it where a statement starts, nullopt
    // when none follows; `line` itself for a path whose statements are unknown ("<script>", a native label, a unit not loaded)
    std::optional<int> breakpoint_line(const std::string& path, int line) const;
    // Fires on the script thread the first time the engine parses a unit under a new path.
    void set_on_source_loaded(std::function<void(const std::string&)> cb);

    // ---- script thread: the engine reports each unit's path (and its declarations, for the lines a stop can take) as it is
    // parsed or reused ----
    void note_source(const std::string& path, const std::vector<std::shared_ptr<declaration>>* declarations = nullptr);

    // ---- inspection: call ONLY on the parked script thread (via post_command) ----
    // A name as the stopped statement resolves it: the innermost frame's locals, then its environment chain (the top level's
    // block scopes, a closure's captures, the program scope). Throws when nothing in scope has the name; a set never declares.
    script_value get_variable(const std::string& name) const;
    void set_variable(const std::string& name, const script_value& value);
    // A name, or a name followed by .field and [index] steps, read at the stop and rendered as a Variables row is
    variable_info evaluate(const std::string& path) const;
    std::vector<frame_info> list_frames() const;   // the call stack at the stop, innermost first, the top level last
    // Frame `frame`'s locals, in list_frames order (callables hidden): a function's parameters and reached locals; the
    // top level's block-scoped names while it is the innermost frame
    std::vector<variable_info> list_locals(size_t frame = 0) const;
    std::vector<variable_info> list_globals() const;   // the program scope's variables (callables hidden)

    // ---- script/hot path: the interpreter statement hook ----
    // Per-statement gate of an armed session, checked BEFORE on_statement. Step/pause and a
    // pending transport change (on_statement syncs it) force the slow path; breakpoints filter
    // through a 64 Kbit line bloom (bit = line mod 65536), so a statement whose line matches no
    // breakpoint line costs two predictable branches. Bloom collisions just miss on the slow path.
    bool wants_statement(uint32_t line) const noexcept {
        if (hot_step_or_pause_.load(std::memory_order_relaxed)) return true;
        const uint64_t* bloom = hot_line_bloom_;
        if (!bloom) return false;
        const uint32_t b = line & (line_bloom_bits - 1);
        return (bloom[b >> 6] >> (b & 63u)) & 1u;
    }
    void on_statement(const ast_node* node, int depth);
    // A stop is a statement that does work of its own: a block or a case label only holds
    // statements, and a for initializer belongs to its header's stop (the backends skip it).
    static bool stops_at(const ast_node* node) noexcept;

    // Pull transport-side state (enable, breakpoint edits, pause requests) into the
    // script-thread hot cache. Called ONLY on the script thread at the debug sync points
    // (through the backend's sync_debug_hook, which mirrors the answer into its gate);
    // returns whether the statement hook should stay armed (session enabled). Steady
    // state is three relaxed loads + one version compare — the bloom/table rebuild runs
    // only when set_breakpoints actually changed something.
    bool sync_hot_state();

    // The script thread's sleep while a debugger exists (engine_time::sleep_for): a transport
    // change wakes it to sync, a pause parks it at the statement that sleeps, and the rest of
    // the sleep runs after.
    void sleep_for(std::chrono::nanoseconds duration);
    // A sleep with no duration (sleep(0)): the same sync and pause service, waiting for nothing; one relaxed load when no
    // transport change is pending
    void service();

    // ---- script thread: a suspendable run's step travels with it (engine::swap_run_entry_state), so another entry's
    // statements never satisfy a run's step over or out ----
    void swap_step_state(int& mode, int& base_depth, int& stop_depth) noexcept;

private:
    void raise_sync();
    void park(const stop_info& si, uint64_t detaches_at_entry);
    bool should_stop(const ast_node* node, int depth, std::string& reason_out) const;
    bool breakpoint_hit(const std::string& file, int line) const;
    void publish_breakpoints(const std::string& file, std::vector<int> lines, bool source_path);
    script_value* variable_storage(const std::string& name) const;

    struct breakpoint_record {
        std::vector<int> lines;
        uint64_t sequence = 0;
    };
    struct breakpoint_snapshot {
        // Empty line sets are tombstones: clearing an alternate spelling must
        // reach the script thread, where equivalent paths are resolved in order.
        std::unordered_map<std::string, breakpoint_record> native;
        std::unordered_map<std::string, breakpoint_record> source;
        uint64_t sequence = 0;
        uint64_t clear_sequence = 0;
    };

    engine* engine_;

    // Transport-writable state (never touched on the per-statement path).
    std::atomic<bool> enabled_{false};
    std::atomic<bool> paused_{false};
    std::atomic<int> pending_pause_{0};
    // Raised by every transport-side change: an armed session's next statement and a sleeping
    // script thread sync on it.
    std::atomic<bool> sync_requested_{false};
    std::atomic<int> step_mode_{static_cast<int>(step_mode::none)};
    std::atomic<int> step_base_depth_{0};
    std::atomic<int> current_stop_depth_{0};   // depth of the current stop (step over/out base)
    // The existing snapshot/version handoff carries the latest edit per raw key,
    // not a request queue. Retained keys are bounded by distinct spellings since
    // clear_breakpoints(). No engine state is read on the publishing thread.
    detail::atomic_shared_ptr<const breakpoint_snapshot> breakpoints_{std::make_shared<breakpoint_snapshot>()};
    std::atomic<uint64_t> bp_version_{0};

    // ---- script-thread hot cache ----
    // Plain members: written only by sync_hot_state()/park() on the script thread, read
    // per statement by wants_statement()/should_stop() with no synchronization. The one
    // exception is the slow-path flag: raise_sync sets it from the transport too (a relaxed
    // load is the plain byte load the gate always read).
    static constexpr uint32_t line_bloom_bits = 65536;   // 8 KB bitmap when breakpoints exist
    std::atomic<bool> hot_step_or_pause_{false};
    const uint64_t* hot_line_bloom_ = nullptr;           // null = no breakpoints
    std::vector<uint64_t> hot_line_bloom_storage_;
    std::shared_ptr<const breakpoint_table> hot_bps_;    // installed table; retained across disable/rearm
    breakpoint_table hot_bound_bps_;                     // the installed lines moved onto the lines statements start
    uint64_t hot_stop_lines_version_ = 0;
    uint64_t applied_bp_sequence_ = 0;
    uint64_t hot_bp_version_ = 0;                        // ~0 forces a re-cache (session re-arm)

    // park/pump handshake — one mutex, one cv, predicate wait (no lost wakeups).
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool resume_requested_ = false;
    std::atomic<uint64_t> detaches_{0};   // raised under mtx_: a stop taken before a detach never parks after it
    std::deque<std::function<void()>> commands_;
    bool in_command_ = false;   // reentrancy guard (touched only on the parked thread)

    std::function<void(const stop_info&)> on_stopped_;

    // Paths of the units the engine has attributed, for source_known (any thread, under sources_mtx_).
    mutable std::mutex sources_mtx_;
    std::unordered_set<std::string> sources_;
    std::unordered_map<std::string, std::vector<int>> stop_lines_;   // file -> the lines a statement starts on (sorted)
    std::atomic<uint64_t> stop_lines_version_{0};
    std::function<void(const std::string&)> on_source_loaded_;
    std::atomic<bool> normalize_sources_{true};   // the engine's filesystem grant as of the script thread's last note/sync
    stop_info current_stop_;   // last stop; read only on the parked script thread
};

} // namespace debug
} // namespace jai
