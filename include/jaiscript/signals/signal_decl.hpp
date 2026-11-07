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
	// The emit state of the emitter this receiver is connected to; null when it is connected to none.
	std::atomic<const void*> owner_ {nullptr};
	// Every connect and disconnect advances it, so an emitter's entry is the live connection only while it matches.
	std::atomic<std::uint64_t> connection_ {0};

	template<typename U, typename S> friend class signal_emitter;
};


template<typename T, typename Sync>
class signal_emitter {
	using mutex_type = typename Sync::mutex_type;

	// What an emit touches after a receiver returns lives here, off the emitter, so a receiver may destroy the emitter.
	// Emit can run reentrantly (a slot may emit the same signal), so it tracks call DEPTH, not a bool: the deferred
	// post-emit cleanup runs only when the OUTERMOST emit unwinds.
	struct emit_state {
		mutex_type mutex;
		int depth = 0;
		bool orphaned = false;
		std::shared_ptr<emit_state> self;   // the destroyed emitter's hand-off to the outermost emit
	};

	struct observer_entry {
		std::weak_ptr<receiver<T>> target;
		std::uint64_t connection = 0;
		bool joins_running_emits = false;
	};

public:
	using function_type = std::function<T>;
	using receiver_type = receiver<T>;
	using shared_receiver_type = std::shared_ptr<receiver<T>>;
	using weak_receiver_type = std::weak_ptr<receiver<T>>;
	using sync_policy = Sync;

	signal_emitter() = default;
	~signal_emitter() {
		for (const auto& entry : observers_) {
			if (auto target = live_target(entry)) {
				target->owner_ = nullptr;
			}
		}
		// Destroyed by one of its own receivers: the emit in progress keeps the state and frees it as it unwinds
		if (state_->depth > 0) {
			state_->orphaned = true;
			state_->self = state_;
		}
	}

	// A jai::signal facade refers to its emitter, so neither may move under a facade that would keep pointing back
	signal_emitter(const signal_emitter&) = delete;
	signal_emitter& operator=(const signal_emitter&) = delete;
	signal_emitter(signal_emitter&&) = delete;
	signal_emitter& operator=(signal_emitter&&) = delete;

	[[nodiscard]]
	shared_receiver_type connect(std::function<T> callback) {
		return connect_new(receiver<T>::make(std::move(callback)), false);
	}

	[[nodiscard]]
	shared_receiver_type connect(const std::string& script_callback) {
		auto lock = lock_state();
		return attach(make_script_receiver(script_callback), false);
	}

	[[nodiscard]]
	shared_receiver_type connect_oneshot(std::function<T> callback) {
		auto recv = receiver<T>::make(std::move(callback));
		recv->oneshot_ = true;
		return connect_new(std::move(recv), false);
	}

	[[nodiscard]]
	shared_receiver_type connect_oneshot(const std::string& script_callback) {
		auto lock = lock_state();
		auto recv = make_script_receiver(script_callback);
		recv->oneshot_ = true;
		return attach(std::move(recv), false);
	}

	bool connect(shared_receiver_type recv) {
		return reconnect(std::move(recv), false);
	}

	shared_receiver_type connect(const std::string& id, std::function<T> callback) {
		return connect_named(id, receiver<T>::make(std::move(callback)), false);
	}

	shared_receiver_type connect(const std::string& id, const std::string& script_callback) {
		auto lock = lock_state();
		return connect_named(id, make_script_receiver(script_callback), false);
	}

	// Connect a script FUNCTION value as an owned, named connection. The function
	// value carries its engine; arguments convert through the same machinery as
	// source-string receivers. Defined in signal_impl.hpp (needs the full engine).
	shared_receiver_type connect(const std::string& id, const script_value& fn_callback);

	// A connect joins from the next emit on; connect_immediate also joins every emit already running.
	[[nodiscard]]
	shared_receiver_type connect_immediate(std::function<T> callback) {
		return connect_new(receiver<T>::make(std::move(callback)), true);
	}

	[[nodiscard]]
	shared_receiver_type connect_immediate(const std::string& script_callback) {
		auto lock = lock_state();
		return attach(make_script_receiver(script_callback), true);
	}

	bool connect_immediate(shared_receiver_type recv) {
		return reconnect(std::move(recv), true);
	}

	shared_receiver_type connect_immediate(const std::string& id, std::function<T> callback) {
		return connect_named(id, receiver<T>::make(std::move(callback)), true);
	}

	shared_receiver_type connect_immediate(const std::string& id, const std::string& script_callback) {
		auto lock = lock_state();
		return connect_named(id, make_script_receiver(script_callback), true);
	}

	shared_receiver_type connect_immediate(const std::string& id, const script_value& fn_callback);

	shared_receiver_type connection(const std::string& id) {
		auto lock = lock_state();
		auto it = owned_connections_.find(id);
		return it != owned_connections_.end() ? it->second : shared_receiver_type();
	}

	bool connected(const std::string& id) const {
		auto lock = lock_state();
		auto it = owned_connections_.find(id);
		return it != owned_connections_.end() && owns(*it->second);
	}

	size_t observer_count() const {
		auto lock = lock_state();
		return static_cast<size_t>(std::count_if(observers_.begin(), observers_.end(), [](const observer_entry& a_entry) {
			return live_target(a_entry) != nullptr;
		}));
	}

	void disconnect(shared_receiver_type recv) {
		auto lock = lock_state();
		detach(recv);
	}

	void disconnect(const std::string& id) {
		auto lock = lock_state();
		auto it = owned_connections_.find(id);
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

	template<typename... Args>
	void operator()(Args&&... args) {
		emit_impl<false>(std::forward<Args>(args)...);
	}

	void operator()() {
		emit_impl_noargs<false>();
	}

	template<typename... Args>
	void emit(Args&&... args) {
		emit_impl<false>(std::forward<Args>(args)...);
	}

	template<typename... Args>
	bool emit_while(Args&&... args)
		requires std::is_same_v<typename std::function<T>::result_type, bool>
	{
		return emit_impl<true>(std::forward<Args>(args)...);
	}

	bool emit_while()
		requires std::is_same_v<typename std::function<T>::result_type, bool>
	{
		return emit_impl_noargs<true>();
	}

	// Returns the number of connected receivers; during an emit the dead entries leave when the outermost emit returns.
	size_t cull_dead_observers() {
		auto lock = lock_state();
		discard_stale_entries();
		return static_cast<size_t>(std::count_if(observers_.begin(), observers_.end(), [](const observer_entry& a_entry) {
			return live_target(a_entry) != nullptr;
		}));
	}

	signal_emitter& script_engine(engine* eng) {
		auto lock = lock_state();
		script_engine_ = eng;
		script_engine_alive_ = eng ? engine_lifetime_token(eng) : std::weak_ptr<const void>();
		for (const auto& entry : observers_) {
			if (auto target = live_target(entry)) {
				target->script_engine(eng);
			}
		}
		return *this;
	}

	engine* script_engine() const {
		auto lock = lock_state();
		return script_engine_;
	}

	signal_emitter& parameter_names(const std::vector<std::string>& names) {
		auto lock = lock_state();
		ordered_parameter_names_ = names.empty()
			? nullptr
			: std::make_shared<std::vector<std::string>>(names);
		return *this;
	}

	std::vector<std::string> parameter_names() const {
		auto lock = lock_state();
		return ordered_parameter_names_ ? *ordered_parameter_names_ : std::vector<std::string>();
	}

	bool has_parameter_names() const {
		auto lock = lock_state();
		return ordered_parameter_names_ && !ordered_parameter_names_->empty();
	}

	void set_blocked_callback(std::function<T> callback) {
		auto lock = lock_state();
		blocked_callback_ = callback ? std::make_shared<const std::function<T>>(std::move(callback)) : nullptr;
	}

private:
	std::lock_guard<mutex_type> lock_state() const {
		return std::lock_guard<mutex_type>(state_->mutex);
	}

	bool owns(const receiver<T>& a_receiver) const {
		return a_receiver.owner_.load() == state_.get();
	}

	static shared_receiver_type live_target(const observer_entry& a_entry) {
		auto target = a_entry.target.lock();
		return target && target->connection_.load() == a_entry.connection ? target : nullptr;
	}

	// Caller holds the lock.
	shared_receiver_type make_script_receiver(const std::string& a_script) {
		return receiver<T>::make(a_script, script_engine_, script_engine_alive_, ordered_parameter_names_);
	}

	shared_receiver_type connect_new(shared_receiver_type a_receiver, bool a_joinsRunningEmits) {
		auto lock = lock_state();
		return attach(std::move(a_receiver), a_joinsRunningEmits);
	}

	bool reconnect(shared_receiver_type a_receiver, bool a_joinsRunningEmits) {
		if (!a_receiver) {
			return false;
		}
		auto lock = lock_state();
		a_receiver->oneshot_fired_ = false;
		attach(std::move(a_receiver), a_joinsRunningEmits);
		return true;
	}

	shared_receiver_type connect_named(const std::string& a_id, shared_receiver_type a_receiver, bool a_joinsRunningEmits) {
		auto lock = lock_state();
		shared_receiver_type replaced = std::exchange(owned_connections_[a_id], a_receiver);
		return attach(std::move(a_receiver), a_joinsRunningEmits);
	}

	// Caller holds the lock. A receiver belongs to one emitter at a time: connecting it again, here or elsewhere, ends
	// its earlier entry.
	shared_receiver_type attach(shared_receiver_type a_receiver, bool a_joinsRunningEmits) {
		a_receiver->owner_ = state_.get();
		observers_.push_back({a_receiver, ++a_receiver->connection_, a_joinsRunningEmits});
		return a_receiver;
	}

	static void release(receiver<T>& a_receiver) {
		a_receiver.owner_ = nullptr;
		++a_receiver.connection_;
	}

	// Caller holds the lock and keeps a_receiver alive, so releasing the emitter's own holds destroys nothing mid-edit.
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
		if (state_->depth != 0) {
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

	// One emit's lock and depth. The outermost exit (normal, or a receiver's throw) drains while still locked; when a
	// receiver destroyed the emitter it instead unlocks and frees the state the destructor handed over.
	struct emit_scope {
		signal_emitter* self;
		emit_state* state;
		std::unique_lock<mutex_type> lock;
		explicit emit_scope(signal_emitter* a_self) : self(a_self), state(a_self->state_.get()), lock(state->mutex) { ++state->depth; }
		~emit_scope() {
			if (--state->depth != 0) { return; }
			if (!state->orphaned) {
				self->drain_after_emit();
				return;
			}
			lock.unlock();
			std::shared_ptr<emit_state> last = std::move(state->self);
		}
		bool emitter_alive() const { return !state->orphaned; }
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

	template<bool ShortCircuit, typename... Args>
	bool emit_impl(Args&&... args) {
		emit_scope scope(this);

		if (block_count_ != 0) {
			called_while_blocked_ = true;
			// A copy, so the callback may replace itself or destroy the emitter while it runs
			if (auto callback = blocked_callback_) {
				(*callback)(std::forward<Args>(args)...);
			}
			return true;
		}

		bool result = true;
		const size_t presentAtStart = observers_.size();
		for (size_t i = 0; i < observers_.size(); ++i) {
			auto locked = pass_target(i, presentAtStart);
			if (!locked) { continue; }

			if constexpr (ShortCircuit) {
				// Pass as LVALUES: forwarding rvalue args inside the loop would let the
				// first receiver's by-value parameter move-consume them, handing every
				// later receiver an empty shell (null shared_ptr, empty string, ...).
				const bool keepGoing = locked->predicate(args...);
				if (!scope.emitter_alive()) { return keepGoing; }
				if (!keepGoing) {
					result = false;
					has_dead_observers_ = true;  // unscanned tail may hold dead observers
					break;
				}
			} else {
				locked->notify(args...);
				if (!scope.emitter_alive()) { return result; }
			}
		}

		return result;
	}

	template<bool ShortCircuit>
	bool emit_impl_noargs() {
		emit_scope scope(this);

		if (block_count_ != 0) {
			called_while_blocked_ = true;
			if (auto callback = blocked_callback_) {
				(*callback)();
			}
			return true;
		}

		bool result = true;
		const size_t presentAtStart = observers_.size();
		for (size_t i = 0; i < observers_.size(); ++i) {
			auto locked = pass_target(i, presentAtStart);
			if (!locked) { continue; }

			if constexpr (ShortCircuit) {
				const bool keepGoing = locked->predicate();
				if (!scope.emitter_alive()) { return keepGoing; }
				if (!keepGoing) {
					result = false;
					has_dead_observers_ = true;  // unscanned tail may hold dead observers
					break;
				}
			} else {
				locked->notify();
				if (!scope.emitter_alive()) { return result; }
			}
		}

		return result;
	}

	// Vector, not set: receivers fire in insertion order.
	std::vector<observer_entry> observers_;
	std::map<std::string, shared_receiver_type> owned_connections_;
	// unnamed script receivers a load rebuilt: nothing outside holds them, so the emitter does (signal_serialization.hpp)
	std::vector<shared_receiver_type> loaded_observers_;

	std::shared_ptr<emit_state> state_ = std::make_shared<emit_state>();

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

	template<typename U, typename S> friend class signal;

	// ADL serialization hooks (named save/load so the archive's ADL dispatch finds them).
	template<typename Archive, typename U, typename S>
	friend void save(Archive& ar, const signal_emitter<U, S>& sig);
	template<typename Archive, typename U, typename S>
	friend void load(Archive& ar, signal_emitter<U, S>& sig);
};

static_assert(!std::is_move_constructible_v<signal_emitter<void()>> && !std::is_move_assignable_v<signal_emitter<void()>>);
static_assert(!std::is_move_constructible_v<signal_emitter<void(), signal_synchronized>> && !std::is_move_assignable_v<signal_emitter<void(), signal_synchronized>>);


template<typename T, typename Sync>
class signal {
public:
	using function_type = std::function<T>;
	using receiver_type = receiver<T>;
	using shared_receiver_type = std::shared_ptr<receiver<T>>;
	using weak_receiver_type = std::weak_ptr<receiver<T>>;
	using emitter_type = signal_emitter<T, Sync>;

	explicit signal(emitter_type& emitter) : emitter_(emitter) {}

	signal(const signal& other) : emitter_(other.emitter_) {}

	[[nodiscard]]
	shared_receiver_type connect(std::function<T> callback) {
		return emitter_.connect(std::move(callback));
	}

	[[nodiscard]]
	shared_receiver_type connect(const std::string& script_callback) {
		return emitter_.connect(script_callback);
	}

	[[nodiscard]]
	shared_receiver_type connect_oneshot(std::function<T> callback) {
		return emitter_.connect_oneshot(std::move(callback));
	}

	[[nodiscard]]
	shared_receiver_type connect_oneshot(const std::string& script_callback) {
		return emitter_.connect_oneshot(script_callback);
	}

	bool connect(shared_receiver_type recv) {
		return emitter_.connect(recv);
	}

	shared_receiver_type connect(const std::string& id, std::function<T> callback) {
		return emitter_.connect(id, std::move(callback));
	}

	shared_receiver_type connect(const std::string& id, const script_value& fn_callback) {
		return emitter_.connect(id, fn_callback);
	}

	shared_receiver_type connect(const std::string& id, const std::string& script_callback) {
		return emitter_.connect(id, script_callback);
	}

	[[nodiscard]]
	shared_receiver_type connect_immediate(std::function<T> callback) {
		return emitter_.connect_immediate(std::move(callback));
	}

	[[nodiscard]]
	shared_receiver_type connect_immediate(const std::string& script_callback) {
		return emitter_.connect_immediate(script_callback);
	}

	bool connect_immediate(shared_receiver_type recv) {
		return emitter_.connect_immediate(recv);
	}

	shared_receiver_type connect_immediate(const std::string& id, std::function<T> callback) {
		return emitter_.connect_immediate(id, std::move(callback));
	}

	shared_receiver_type connect_immediate(const std::string& id, const script_value& fn_callback) {
		return emitter_.connect_immediate(id, fn_callback);
	}

	shared_receiver_type connect_immediate(const std::string& id, const std::string& script_callback) {
		return emitter_.connect_immediate(id, script_callback);
	}

	shared_receiver_type connection(const std::string& id) {
		return emitter_.connection(id);
	}

	bool connected(const std::string& id) const {
		return emitter_.connected(id);
	}

	size_t observer_count() const {
		return emitter_.observer_count();
	}

	void disconnect(shared_receiver_type recv) {
		emitter_.disconnect(recv);
	}

	void disconnect(const std::string& id) {
		emitter_.disconnect(id);
	}

	void script_engine(engine* eng) {
		emitter_.script_engine(eng);
	}

	engine* script_engine() const {
		return emitter_.script_engine();
	}

	void parameter_names(const std::vector<std::string>& names) {
		emitter_.parameter_names(names);
	}

	std::vector<std::string> parameter_names() const {
		return emitter_.parameter_names();
	}

	bool has_parameter_names() const {
		return emitter_.has_parameter_names();
	}

private:
	emitter_type& emitter_;
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
