#pragma once

#ifndef __JAISCRIPT_SIGNALS_SIGNAL_DECL_HPP__
#define __JAISCRIPT_SIGNALS_SIGNAL_DECL_HPP__

#include <memory>
#include <functional>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <type_traits>
#include <utility>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace jai {

class engine;
class script_value;

// An emitter's threading policy, chosen per emitter at compile time. Unsynchronized (the default) takes no lock: one
// thread at a time uses the emitter. Synchronized holds one recursive mutex through connect, disconnect, every other
// emitter call and the whole of an emit, receivers included, so receivers may still modify the signal while it fires.
struct signal_unsynchronized {
	struct mutex_type {
		void lock() {}
		void unlock() {}
	};
};

struct signal_synchronized {
	using mutex_type = std::recursive_mutex;
};

template<typename T, typename Sync = signal_unsynchronized> class signal_emitter;
template<typename T, typename Sync = signal_unsynchronized> class signal;
template<typename T, typename Sync = signal_unsynchronized> class signal_property;

namespace detail {
template<typename T, typename Sync> class signal_state;
struct signal_state_access;
}

// The engine's lifetime token: expired once the engine is destroyed (defined in engine.cpp)
std::weak_ptr<const void> engine_lifetime_token(engine* a_engine);
// A load in progress on the engine: its script receivers stay quiet (defined in engine.cpp)
bool engine_loading(const engine* a_engine);

template<typename T>
class receiver {
public:
	using function_type = std::function<T>;
	using shared_type = std::shared_ptr<receiver<T>>;
	using weak_type = std::weak_ptr<receiver<T>>;

	static std::shared_ptr<receiver<T>> make(std::function<T> callback) {
		return std::shared_ptr<receiver<T>>(new receiver<T>(std::move(callback)));
	}

	static std::shared_ptr<receiver<T>> make(
		const std::string& script,
		engine* eng,
		const std::shared_ptr<std::vector<std::string>>& parameter_names = nullptr
	) {
		return make(script, eng, eng ? engine_lifetime_token(eng) : std::weak_ptr<const void>(), parameter_names);
	}

	static std::shared_ptr<receiver<T>> make(
		const std::string& script,
		engine* eng,
		std::weak_ptr<const void> engine_alive,
		const std::shared_ptr<std::vector<std::string>>& parameter_names
	) {
		auto result = std::shared_ptr<receiver<T>>(new receiver<T>(script, eng, parameter_names));
		result->bind_engine(eng, std::move(engine_alive));
		return result;
	}

	template<typename... Args>
	void notify(Args&&... args) {
		if (!blocked() && !invalid() && !engine_quiet()) {
			if (script_callback_.empty()) {
				callback_(std::forward<Args>(args)...);
			} else {
				call_script(std::forward<Args>(args)...);
			}
		}
	}

	template<typename... Args>
	bool predicate(Args&&... args) {
		if (!blocked() && !invalid() && !engine_quiet()) {
			if (script_callback_.empty()) {
				return callback_(std::forward<Args>(args)...);
			} else {
				return call_script_predicate(std::forward<Args>(args)...);
			}
		}
		return true;  // Default: continue propagation
	}

	void notify() {
		if (!blocked() && !invalid() && !engine_quiet()) {
			if (script_callback_.empty()) {
				callback_();
			} else {
				call_script();
			}
		}
	}

	bool predicate() {
		if (!blocked() && !invalid() && !engine_quiet()) {
			if (script_callback_.empty()) {
				return callback_();
			} else {
				return call_script_predicate();
			}
		}
		return true;  // Default: continue propagation
	}

	template<typename... Args>
	void operator()(Args&&... args) {
		notify(std::forward<Args>(args)...);
	}

	void operator()() {
		notify();
	}

	bool invalid() const {
		return script_callback_.empty() && !callback_;
	}

	bool connected() const {
		return owner_.load() != nullptr;
	}

	void block() { ++block_count_; }

	void unblock() {
		int current = block_count_.load();
		while (current > 0 && !block_count_.compare_exchange_weak(current, current - 1)) {}
	}

	bool blocked() const { return block_count_.load() != 0; }

	bool is_oneshot() const { return oneshot_; }

	const std::string& script() const { return script_callback_; }
	bool has_script() const { return !script_callback_.empty(); }

	// A script function receiver keeps the engine its function belongs to (its lifetime token names that engine)
	void script_engine(engine* eng) {
		if (!script_callback_.empty()) {
			script_engine_ = eng;
			bind_engine(eng, eng ? engine_lifetime_token(eng) : std::weak_ptr<const void>());
		} else if (!engine_bound_) {
			script_engine_ = eng;
		}
	}
	// A script receiver (text, or a script function) runs only while its engine lives: after the engine is destroyed the
	// receiver is skipped, and its payload is released without running script
	void bind_engine(engine* eng, std::weak_ptr<const void> engine_alive) {
		engine_bound_ = eng != nullptr;
		engine_alive_ = std::move(engine_alive);
		if (eng) { script_engine_ = eng; }
	}
	bool engine_gone() const noexcept { return engine_bound_ && engine_alive_.expired(); }
	// a script receiver skips while its engine is gone or loading (a load does not notify)
	bool engine_quiet() const { return engine_bound_ && (engine_alive_.expired() || engine_loading(script_engine_)); }
	engine* script_engine() const { return script_engine_; }

	const std::shared_ptr<std::vector<std::string>>& parameter_names() const {
		return ordered_parameter_names_;
	}

private:
	explicit receiver(std::function<T> callback)
		: callback_(std::move(callback)) {}

	receiver(
		const std::string& script,
		engine* eng,
		const std::shared_ptr<std::vector<std::string>>& parameter_names
	)
		: script_callback_(script)
		, ordered_parameter_names_(parameter_names)
		, script_engine_(eng) {}

	// Implemented in signal_impl.hpp (needs the full engine definition).
	template<typename... Args>
	void call_script(Args&&... args);

	template<typename... Args>
	bool call_script_predicate(Args&&... args);

	void call_script();
	bool call_script_predicate();

	std::atomic<int> block_count_ {0};
	bool oneshot_ = false;
	bool oneshot_fired_ = false;
	bool engine_bound_ = false;
	std::weak_ptr<const void> engine_alive_;
	std::function<T> callback_;
	std::string script_callback_;
	std::shared_ptr<std::vector<std::string>> ordered_parameter_names_;
	engine* script_engine_ = nullptr;
	// The state of the emitter this receiver is connected to; null when it is connected to none.
	std::atomic<const void*> owner_ {nullptr};
	// Every connect and disconnect advances it, so an emitter's entry is the live connection only while it matches.
	std::atomic<std::uint64_t> connection_ {0};

	template<typename U, typename S> friend class detail::signal_state;
};

namespace detail {

// An emitter's receivers, settings and emit bookkeeping. The block is the emitter's identity: each receiver's owner_ and
// every view name it, and a move hands it to the new emitter, so nothing re-points. An emit runs on the block, so a receiver
// may move the emitter (the emit carries on) or destroy it (the block retires and the outermost emit frees it).
template<typename T, typename Sync>
class signal_state {
	using mutex_type = typename Sync::mutex_type;

	struct observer_entry {
		std::weak_ptr<receiver<T>> target;
		std::uint64_t connection = 0;
		bool joins_running_emits = false;
	};

public:
	using shared_receiver_type = std::shared_ptr<receiver<T>>;

	// The emitter lets go: its receivers disconnect and its views connect nothing from now on. The host destroys an emitter
	// no other thread is using, so this takes no lock.
	static void retire(std::shared_ptr<signal_state>& a_state) noexcept {
		signal_state& state = *a_state;
		auto released_owned = std::move(state.owned_connections_);
		auto released_loaded = std::move(state.loaded_observers_);
		auto released_blocked_callback = std::move(state.blocked_callback_);
		state.owned_connections_.clear();
		state.loaded_observers_.clear();
		state.retired_ = true;
		for (const auto& entry : state.observers_) {
			if (auto target = live_target(entry)) {
				release(*target);
			}
		}
		if (state.depth_ > 0) {
			state.self_ = a_state;
		} else {
			state.observers_.clear();
		}
	}

	bool retired() const { return retired_; }

	shared_receiver_type connect(std::function<T> a_callback, bool a_joinsRunningEmits) {
		auto lock = lock_state();
		return attach(receiver<T>::make(std::move(a_callback)), a_joinsRunningEmits);
	}

	shared_receiver_type connect(const std::string& a_script, bool a_joinsRunningEmits) {
		auto lock = lock_state();
		return attach(make_script_receiver(a_script), a_joinsRunningEmits);
	}

	shared_receiver_type connect_oneshot(std::function<T> a_callback) {
		auto recv = receiver<T>::make(std::move(a_callback));
		recv->oneshot_ = true;
		auto lock = lock_state();
		return attach(std::move(recv), false);
	}

	shared_receiver_type connect_oneshot(const std::string& a_script) {
		auto lock = lock_state();
		auto recv = make_script_receiver(a_script);
		recv->oneshot_ = true;
		return attach(std::move(recv), false);
	}

	bool reconnect(shared_receiver_type a_receiver, bool a_joinsRunningEmits) {
		auto lock = lock_state();
		if (!a_receiver || retired_) {
			return false;
		}
		a_receiver->oneshot_fired_ = false;
		attach(std::move(a_receiver), a_joinsRunningEmits);
		return true;
	}

	shared_receiver_type connect(const std::string& a_id, std::function<T> a_callback, bool a_joinsRunningEmits) {
		return connect_named(a_id, receiver<T>::make(std::move(a_callback)), a_joinsRunningEmits);
	}

	shared_receiver_type connect(const std::string& a_id, const std::string& a_script, bool a_joinsRunningEmits) {
		auto lock = lock_state();
		return connect_named(a_id, make_script_receiver(a_script), a_joinsRunningEmits);
	}

	// Defined in signal_impl.hpp (needs the full engine).
	shared_receiver_type connect(const std::string& a_id, const script_value& a_function, bool a_joinsRunningEmits);

	shared_receiver_type connection(const std::string& a_id) {
		auto lock = lock_state();
		auto it = owned_connections_.find(a_id);
		return it != owned_connections_.end() ? it->second : shared_receiver_type();
	}

	bool connected(const std::string& a_id) const {
		auto lock = lock_state();
		auto it = owned_connections_.find(a_id);
		return it != owned_connections_.end() && owns(*it->second);
	}

	size_t observer_count() const {
		auto lock = lock_state();
		return live_count();
	}

	void disconnect(const shared_receiver_type& a_receiver) {
		auto lock = lock_state();
		detach(a_receiver);
	}

	void disconnect(const std::string& a_id) {
		auto lock = lock_state();
		auto it = owned_connections_.find(a_id);
		if (it != owned_connections_.end()) {
			shared_receiver_type released = std::move(it->second);
			owned_connections_.erase(it);
			detach(released);
		}
	}

	void clear() {
		auto lock = lock_state();
		auto released_owned = std::move(owned_connections_);
		auto released_loaded = std::move(loaded_observers_);
		owned_connections_.clear();
		loaded_observers_.clear();
		for (const auto& entry : observers_) {
			if (auto target = live_target(entry)) {
				release(*target);
			}
		}
		discard_stale_entries();
	}

	void block() {
		auto lock = lock_state();
		if (block_count_++ == 0) {
			called_while_blocked_ = false;
		}
	}

	bool unblock() {
		auto lock = lock_state();
		if (block_count_ == 0) {
			return false;
		}
		return --block_count_ == 0 && called_while_blocked_;
	}

	bool blocked() const {
		auto lock = lock_state();
		return block_count_ != 0;
	}

	// Returns the number of connected receivers; during an emit the dead entries leave when the outermost emit returns.
	size_t cull_dead_observers() {
		auto lock = lock_state();
		discard_stale_entries();
		return live_count();
	}

	void script_engine(engine* a_engine) {
		auto lock = lock_state();
		script_engine_ = a_engine;
		script_engine_alive_ = a_engine ? engine_lifetime_token(a_engine) : std::weak_ptr<const void>();
		for (const auto& entry : observers_) {
			if (auto target = live_target(entry)) {
				target->script_engine(a_engine);
			}
		}
	}

	engine* script_engine() const {
		auto lock = lock_state();
		return script_engine_;
	}

	void parameter_names(const std::vector<std::string>& a_names) {
		auto lock = lock_state();
		ordered_parameter_names_ = a_names.empty() ? nullptr : std::make_shared<std::vector<std::string>>(a_names);
	}

	std::vector<std::string> parameter_names() const {
		auto lock = lock_state();
		return ordered_parameter_names_ ? *ordered_parameter_names_ : std::vector<std::string>();
	}

	bool has_parameter_names() const {
		auto lock = lock_state();
		return ordered_parameter_names_ && !ordered_parameter_names_->empty();
	}

	void set_blocked_callback(std::function<T> a_callback) {
		auto lock = lock_state();
		blocked_callback_ = a_callback ? std::make_shared<const std::function<T>>(std::move(a_callback)) : nullptr;
	}

	void copy_settings(const signal_state& a_source) {
		ordered_parameter_names_ = a_source.ordered_parameter_names_;
		script_engine_ = a_source.script_engine_;
		script_engine_alive_ = a_source.script_engine_alive_;
	}

	template<bool ShortCircuit, typename... Args>
	bool emit(Args&&... args) {
		emit_scope scope(*this);

		if (block_count_ != 0) {
			called_while_blocked_ = true;
			// A copy, so the callback may replace itself or destroy the emitter while it runs
			if (auto callback = blocked_callback_) {
				(*callback)(std::forward<Args>(args)...);
			}
			return true;
		}

		// A receiver that destroys the emitter releases every entry, so the rest of the pass runs nothing
		bool result = true;
		const size_t presentAtStart = observers_.size();
		for (size_t i = 0; i < observers_.size(); ++i) {
			auto locked = pass_target(i, presentAtStart);
			if (!locked) { continue; }

			if constexpr (ShortCircuit) {
				// Pass as LVALUES: forwarding rvalue args inside the loop would let the
				// first receiver's by-value parameter move-consume them, handing every
				// later receiver an empty shell (null shared_ptr, empty string, ...).
				if (!locked->predicate(args...)) {
					result = false;
					has_dead_observers_ = true;  // unscanned tail may hold dead observers
					break;
				}
			} else {
				locked->notify(args...);
			}
		}

		return result;
	}

	// Serialization (signal_serialization.hpp).
	template<typename Archive>
	void save_receivers(Archive& a_archive) const;
	template<typename Archive>
	void load_receivers(Archive& a_archive);

private:
	std::lock_guard<mutex_type> lock_state() const {
		return std::lock_guard<mutex_type>(mutex_);
	}

	bool owns(const receiver<T>& a_receiver) const {
		return a_receiver.owner_.load() == this;
	}

	static shared_receiver_type live_target(const observer_entry& a_entry) {
		auto target = a_entry.target.lock();
		return target && target->connection_.load() == a_entry.connection ? target : nullptr;
	}

	size_t live_count() const {
		return static_cast<size_t>(std::count_if(observers_.begin(), observers_.end(), [](const observer_entry& a_entry) {
			return live_target(a_entry) != nullptr;
		}));
	}

	// Caller holds the lock.
	shared_receiver_type make_script_receiver(const std::string& a_script) {
		return receiver<T>::make(a_script, script_engine_, script_engine_alive_, ordered_parameter_names_);
	}

	shared_receiver_type connect_named(const std::string& a_id, shared_receiver_type a_receiver, bool a_joinsRunningEmits) {
		auto lock = lock_state();
		if (retired_) {
			return a_receiver;
		}
		shared_receiver_type replaced = std::exchange(owned_connections_[a_id], a_receiver);
		return attach(std::move(a_receiver), a_joinsRunningEmits);
	}

	// Caller holds the lock. A receiver belongs to one emitter at a time: connecting it again, here or elsewhere, ends
	// its earlier entry. A retired state attaches nothing.
	shared_receiver_type attach(shared_receiver_type a_receiver, bool a_joinsRunningEmits) {
		if (!retired_) {
			a_receiver->owner_ = this;
			observers_.push_back({a_receiver, ++a_receiver->connection_, a_joinsRunningEmits});
		}
		return a_receiver;
	}

	static void release(receiver<T>& a_receiver) {
		a_receiver.owner_ = nullptr;
		++a_receiver.connection_;
	}

	// Caller holds the lock and keeps a_receiver alive, so releasing the state's own holds destroys nothing mid-edit.
	void detach(const shared_receiver_type& a_receiver) {
		if (!a_receiver || !owns(*a_receiver)) {
			return;
		}
		release(*a_receiver);
		std::erase_if(loaded_observers_, [&a_receiver](const shared_receiver_type& a_held) { return a_held == a_receiver; });
		std::erase_if(owned_connections_, [&a_receiver](const auto& a_named) { return a_named.second == a_receiver; });
		discard_stale_entries();
	}

	// Caller holds the lock. An emit indexes the list, so entries leave only outside of every emit.
	void discard_stale_entries() {
		if (depth_ != 0) {
			has_dead_observers_ = true;
			return;
		}
		std::erase_if(observers_, [](const observer_entry& a_entry) { return live_target(a_entry) == nullptr; });
		std::erase_if(loaded_observers_, [this](const shared_receiver_type& a_held) { return !owns(*a_held); });
		has_dead_observers_ = false;
	}

	// Run once the outermost emit unwinds, from emit_scope's destructor so it still runs if a slot throws.
	void drain_after_emit() {
		if (has_oneshots_pending_) {
			for (const auto& entry : observers_) {
				if (auto target = live_target(entry); target && target->is_oneshot() && target->oneshot_fired_) {
					release(*target);
				}
			}
			has_oneshots_pending_ = false;
			has_dead_observers_ = true;
		}
		if (has_dead_observers_) {
			discard_stale_entries();
		}
	}

	// One emit's lock and depth. Emit can run reentrantly (a slot may emit the same signal), so it tracks call depth: the
	// outermost exit (normal, or a receiver's throw) drains while still locked; when the emitter was destroyed it instead
	// unlocks and frees the state the destroyed emitter handed over.
	struct emit_scope {
		signal_state& state;
		std::unique_lock<mutex_type> lock;
		explicit emit_scope(signal_state& a_state) : state(a_state), lock(a_state.mutex_) { ++state.depth_; }
		~emit_scope() {
			if (--state.depth_ != 0) { return; }
			if (!state.retired_) {
				state.drain_after_emit();
				return;
			}
			lock.unlock();
			std::shared_ptr<signal_state> last = std::move(state.self_);
		}
		emit_scope(const emit_scope&) = delete;
		emit_scope& operator=(const emit_scope&) = delete;
	};

	// A one-shot fires once: marked before the call, so one that throws or re-emits has fired
	bool fire_oneshot_once(const shared_receiver_type& a_receiver) {
		if (!a_receiver->is_oneshot()) { return true; }
		if (a_receiver->oneshot_fired_) { return false; }
		a_receiver->oneshot_fired_ = true;
		has_oneshots_pending_ = true;
		return true;
	}

	// An emit runs the receivers connected when it started, plus those connect_immediate added while it runs.
	shared_receiver_type pass_target(size_t a_index, size_t a_presentAtStart) {
		const observer_entry& entry = observers_[a_index];
		if (a_index >= a_presentAtStart && !entry.joins_running_emits) {
			return nullptr;
		}
		auto target = live_target(entry);
		if (!target) {
			has_dead_observers_ = true;
			return nullptr;
		}
		return fire_oneshot_once(target) ? target : nullptr;
	}

	mutable mutex_type mutex_;
	int depth_ = 0;
	bool retired_ = false;
	std::shared_ptr<signal_state> self_;   // a retired state's hand-off to the outermost emit

	// Vector, not set: receivers fire in insertion order.
	std::vector<observer_entry> observers_;
	std::map<std::string, shared_receiver_type> owned_connections_;
	// unnamed script receivers a load rebuilt: nothing outside holds them, so the state does (signal_serialization.hpp)
	std::vector<shared_receiver_type> loaded_observers_;

	bool has_oneshots_pending_ = false;
	// Set when an entry stops being live (an expired or disconnected receiver) or a pass skipped one: gates the
	// post-emit cull so a steady-state emit over live observers skips the O(n) scan entirely.
	bool has_dead_observers_ = false;
	int block_count_ = 0;
	bool called_while_blocked_ = false;
	std::shared_ptr<const std::function<T>> blocked_callback_;

	std::shared_ptr<std::vector<std::string>> ordered_parameter_names_;
	engine* script_engine_ = nullptr;
	// a text receiver connected after the engine is gone takes this token, never the dead engine's
	std::weak_ptr<const void> script_engine_alive_;
};

// The emitter's and the view's shared state, for the library's own signal code (signal_property, the script view, the
// archive hooks).
struct signal_state_access {
	template<typename T, typename Sync>
	static const std::shared_ptr<signal_state<T, Sync>>& state(const signal_emitter<T, Sync>& a_emitter) { return a_emitter.state_; }
	template<typename T, typename Sync>
	static std::shared_ptr<signal_state<T, Sync>>& state(signal<T, Sync>& a_view) { return a_view.state_; }
	template<typename T, typename Sync>
	static const std::shared_ptr<signal_state<T, Sync>>& state(const signal<T, Sync>& a_view) { return a_view.state_; }
};

} // namespace detail

// An emitter owns its state on the heap and every view shares it, so a move hands the receivers, the settings and every
// view to the new emitter, and the moved-from emitter starts over empty. Move assignment lets go of the target's own
// receivers first. An emitter does not copy (member_signal_emitter does, for a copyable owner).
template<typename T, typename Sync>
class signal_emitter {
	using state_type = detail::signal_state<T, Sync>;

public:
	using function_type = std::function<T>;
	using receiver_type = receiver<T>;
	using shared_receiver_type = std::shared_ptr<receiver<T>>;
	using weak_receiver_type = std::weak_ptr<receiver<T>>;
	using sync_policy = Sync;

	signal_emitter() = default;
	~signal_emitter() { state_type::retire(state_); }

	signal_emitter(const signal_emitter&) = delete;
	signal_emitter& operator=(const signal_emitter&) = delete;
	signal_emitter(signal_emitter&& a_other) noexcept : state_(std::exchange(a_other.state_, std::make_shared<state_type>())) {}
	signal_emitter& operator=(signal_emitter&& a_other) noexcept {
		if (this != &a_other) {
			state_type::retire(state_);
			state_ = std::exchange(a_other.state_, std::make_shared<state_type>());
		}
		return *this;
	}

	[[nodiscard]]
	shared_receiver_type connect(std::function<T> callback) { return state_->connect(std::move(callback), false); }

	[[nodiscard]]
	shared_receiver_type connect(const std::string& script_callback) { return state_->connect(script_callback, false); }

	[[nodiscard]]
	shared_receiver_type connect_oneshot(std::function<T> callback) { return state_->connect_oneshot(std::move(callback)); }

	[[nodiscard]]
	shared_receiver_type connect_oneshot(const std::string& script_callback) { return state_->connect_oneshot(script_callback); }

	bool connect(shared_receiver_type recv) { return state_->reconnect(std::move(recv), false); }

	shared_receiver_type connect(const std::string& id, std::function<T> callback) { return state_->connect(id, std::move(callback), false); }

	shared_receiver_type connect(const std::string& id, const std::string& script_callback) { return state_->connect(id, script_callback, false); }

	// Connect a script FUNCTION value as an owned, named connection. The function value carries its engine; arguments
	// convert through the same machinery as source-string receivers.
	shared_receiver_type connect(const std::string& id, const script_value& fn_callback) { return state_->connect(id, fn_callback, false); }

	// A connect joins from the next emit on; connect_immediate also joins every emit already running.
	[[nodiscard]]
	shared_receiver_type connect_immediate(std::function<T> callback) { return state_->connect(std::move(callback), true); }

	[[nodiscard]]
	shared_receiver_type connect_immediate(const std::string& script_callback) { return state_->connect(script_callback, true); }

	bool connect_immediate(shared_receiver_type recv) { return state_->reconnect(std::move(recv), true); }

	shared_receiver_type connect_immediate(const std::string& id, std::function<T> callback) { return state_->connect(id, std::move(callback), true); }

	shared_receiver_type connect_immediate(const std::string& id, const std::string& script_callback) { return state_->connect(id, script_callback, true); }

	shared_receiver_type connect_immediate(const std::string& id, const script_value& fn_callback) { return state_->connect(id, fn_callback, true); }

	shared_receiver_type connection(const std::string& id) { return state_->connection(id); }

	bool connected(const std::string& id) const { return state_->connected(id); }

	size_t observer_count() const { return state_->observer_count(); }

	void disconnect(shared_receiver_type recv) { state_->disconnect(recv); }

	void disconnect(const std::string& id) { state_->disconnect(id); }

	void clear() { state_->clear(); }

	void block() { state_->block(); }

	bool unblock() { return state_->unblock(); }

	bool blocked() const { return state_->blocked(); }

	template<typename... Args>
	void operator()(Args&&... args) {
		state_->template emit<false>(std::forward<Args>(args)...);
	}

	template<typename... Args>
	void emit(Args&&... args) {
		state_->template emit<false>(std::forward<Args>(args)...);
	}

	template<typename... Args>
	bool emit_while(Args&&... args)
		requires std::is_same_v<typename std::function<T>::result_type, bool>
	{
		return state_->template emit<true>(std::forward<Args>(args)...);
	}

	// Returns the number of connected receivers; during an emit the dead entries leave when the outermost emit returns.
	size_t cull_dead_observers() { return state_->cull_dead_observers(); }

	signal_emitter& script_engine(engine* eng) {
		state_->script_engine(eng);
		return *this;
	}

	engine* script_engine() const { return state_->script_engine(); }

	signal_emitter& parameter_names(const std::vector<std::string>& names) {
		state_->parameter_names(names);
		return *this;
	}

	std::vector<std::string> parameter_names() const { return state_->parameter_names(); }

	bool has_parameter_names() const { return state_->has_parameter_names(); }

	void set_blocked_callback(std::function<T> callback) { state_->set_blocked_callback(std::move(callback)); }

protected:
	void copy_settings(const signal_emitter& a_source) { state_->copy_settings(*a_source.state_); }

private:
	template<typename U, typename S> friend class signal;
	friend detail::signal_state_access;

	std::shared_ptr<state_type> state_ = std::make_shared<state_type>();
};

static_assert(std::is_nothrow_move_constructible_v<signal_emitter<void()>> && std::is_nothrow_move_assignable_v<signal_emitter<void()>>);
static_assert(!std::is_copy_constructible_v<signal_emitter<void()>> && !std::is_copy_assignable_v<signal_emitter<void()>>);
static_assert(std::is_nothrow_move_constructible_v<signal_emitter<void(), signal_synchronized>> && std::is_nothrow_move_assignable_v<signal_emitter<void(), signal_synchronized>>);
static_assert(!std::is_copy_constructible_v<signal_emitter<void(), signal_synchronized>> && !std::is_copy_assignable_v<signal_emitter<void(), signal_synchronized>>);

// An emitter that is a member of a copyable, movable object (JAI_SIGNAL and observable_property hold one). A copy
// keeps the script engine and parameter names and starts with no receivers; a move takes the receivers; move assignment
// lets go of its own first; copy assignment changes nothing.
template<typename T, typename Sync = signal_unsynchronized>
class member_signal_emitter : public signal_emitter<T, Sync> {
public:
	member_signal_emitter() = default;
	member_signal_emitter(const member_signal_emitter& a_other) : signal_emitter<T, Sync>() { this->copy_settings(a_other); }
	member_signal_emitter(member_signal_emitter&&) noexcept = default;
	member_signal_emitter& operator=(const member_signal_emitter&) noexcept { return *this; }
	member_signal_emitter& operator=(member_signal_emitter&&) noexcept = default;
};

static_assert(std::is_nothrow_move_constructible_v<member_signal_emitter<void()>> && std::is_nothrow_move_assignable_v<member_signal_emitter<void()>>);
static_assert(std::is_copy_constructible_v<member_signal_emitter<void()>> && std::is_copy_assignable_v<member_signal_emitter<void()>>);


// The connecting half of an emitter. A view shares its emitter's state, so it follows the emitter through every move, a
// copy is one more view of the same emitter, and assigning one view to another re-targets it. Declare the emitter before
// the view that is built from it. Once its emitter is destroyed (or assigned over), a view connects nothing: a connect
// returns a receiver that is not connected.
template<typename T, typename Sync>
class signal {
	using state_type = detail::signal_state<T, Sync>;

public:
	using function_type = std::function<T>;
	using receiver_type = receiver<T>;
	using shared_receiver_type = std::shared_ptr<receiver<T>>;
	using weak_receiver_type = std::weak_ptr<receiver<T>>;
	using emitter_type = signal_emitter<T, Sync>;

	explicit signal(emitter_type& emitter) : state_(emitter.state_) {}

	// Declared so a move copies: a view never empties, and a moved-from owner's view stays on the emitter that moved
	signal(const signal&) = default;
	signal& operator=(const signal&) = default;

	[[nodiscard]]
	shared_receiver_type connect(std::function<T> callback) { return state_->connect(std::move(callback), false); }

	[[nodiscard]]
	shared_receiver_type connect(const std::string& script_callback) { return state_->connect(script_callback, false); }

	[[nodiscard]]
	shared_receiver_type connect_oneshot(std::function<T> callback) { return state_->connect_oneshot(std::move(callback)); }

	[[nodiscard]]
	shared_receiver_type connect_oneshot(const std::string& script_callback) { return state_->connect_oneshot(script_callback); }

	bool connect(shared_receiver_type recv) { return state_->reconnect(std::move(recv), false); }

	shared_receiver_type connect(const std::string& id, std::function<T> callback) { return state_->connect(id, std::move(callback), false); }

	shared_receiver_type connect(const std::string& id, const script_value& fn_callback) { return state_->connect(id, fn_callback, false); }

	shared_receiver_type connect(const std::string& id, const std::string& script_callback) { return state_->connect(id, script_callback, false); }

	[[nodiscard]]
	shared_receiver_type connect_immediate(std::function<T> callback) { return state_->connect(std::move(callback), true); }

	[[nodiscard]]
	shared_receiver_type connect_immediate(const std::string& script_callback) { return state_->connect(script_callback, true); }

	bool connect_immediate(shared_receiver_type recv) { return state_->reconnect(std::move(recv), true); }

	shared_receiver_type connect_immediate(const std::string& id, std::function<T> callback) { return state_->connect(id, std::move(callback), true); }

	shared_receiver_type connect_immediate(const std::string& id, const script_value& fn_callback) { return state_->connect(id, fn_callback, true); }

	shared_receiver_type connect_immediate(const std::string& id, const std::string& script_callback) { return state_->connect(id, script_callback, true); }

	shared_receiver_type connection(const std::string& id) { return state_->connection(id); }

	bool connected(const std::string& id) const { return state_->connected(id); }

	size_t observer_count() const { return state_->observer_count(); }

	void disconnect(shared_receiver_type recv) { state_->disconnect(recv); }

	void disconnect(const std::string& id) { state_->disconnect(id); }

	void script_engine(engine* eng) { state_->script_engine(eng); }

	engine* script_engine() const { return state_->script_engine(); }

	void parameter_names(const std::vector<std::string>& names) { state_->parameter_names(names); }

	std::vector<std::string> parameter_names() const { return state_->parameter_names(); }

	bool has_parameter_names() const { return state_->has_parameter_names(); }

private:
	friend detail::signal_state_access;

	std::shared_ptr<state_type> state_;
};

} // namespace jai

// NOTE: signal_impl.hpp (the script-callback execution glue) is intentionally NOT
// included here. It drags in core/engine.hpp + core/value.hpp (the entire interpreter
// + value system, ~58 extra transitive includes, ~1.8s parse cost per TU), which the
// vast majority of consumers using plain C++ signals never need.
//
// Consumers that actually fire SCRIPT receivers (receiver<T> constructed from a script
// string + engine*) must include <jaiscript/signals/signal_impl.hpp> explicitly.
// dynamic_binder.hpp and signal_serialization.hpp already do so.
// For convenience, <jaiscript/signals/signal.hpp> includes both.

#endif // __JAISCRIPT_SIGNALS_SIGNAL_DECL_HPP__
