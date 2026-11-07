#pragma once

#ifndef __JAISCRIPT_SIGNALS_RECEIVER_OWNER_HPP__
#define __JAISCRIPT_SIGNALS_RECEIVER_OWNER_HPP__

#include <algorithm>
#include <memory>
#include <vector>

namespace jai {

// Forward declaration
template<typename T> class receiver;

// ============================================================================
// receiver_owner - Automatic lifetime management for signal receivers
// ============================================================================
//
// A type-erased container that holds signal receivers and automatically
// disconnects them when destroyed. Use this to tie receiver lifetime to
// an object's lifetime, avoiding manual handle storage.
//
// Standalone usage:
//   class Player {
//       receiver_owner receivers_;
//   public:
//       Player(signal<void(int)>& damage_signal) {
//           receivers_.track(damage_signal.connect([this](int dmg) {
//               // handle damage
//           }));
//       }
//   };  // receivers_ dies with Player, all connections auto-disconnect
//
// With property_owner (has built-in receiver_owner):
//   class Player : public property_owner<Player> {
//   public:
//       Player(signal<void(int)>& damage_signal) {
//           track(damage_signal.connect([this](int dmg) {
//               // handle damage
//           }));
//       }
//   };

class receiver_owner {
	// Type-erased base for storing any receiver<T>
	struct handle_base {
		virtual ~handle_base() = default;
		virtual bool connected() const = 0;
	};

	template<typename T>
	struct handle : handle_base {
		std::shared_ptr<receiver<T>> recv;

		explicit handle(std::shared_ptr<receiver<T>> r) : recv(std::move(r)) {}

		bool connected() const override {
			return recv && recv->connected();
		}
	};

	std::vector<std::unique_ptr<handle_base>> handles_;

public:
	receiver_owner() = default;
	~receiver_owner() = default;

	// Move-only (prevent accidental copies that would share ownership)
	receiver_owner(receiver_owner&&) = default;
	receiver_owner& operator=(receiver_owner&&) = default;
	receiver_owner(const receiver_owner&) = delete;
	receiver_owner& operator=(const receiver_owner&) = delete;

	// Track a receiver - stores it and returns the same pointer for chaining
	template<typename T>
	std::shared_ptr<receiver<T>> track(std::shared_ptr<receiver<T>> recv) {
		if (recv) {
			handles_.push_back(std::make_unique<handle<T>>(recv));
		}
		return recv;
	}

	// Clear all tracked receivers (they will disconnect when their shared_ptrs die)
	void clear() {
		handles_.clear();
	}

	// Number of tracked receivers
	size_t size() const {
		return handles_.size();
	}

	// Number of still-connected receivers
	size_t connected_count() const {
		size_t count = 0;
		for (const auto& h : handles_) {
			if (h->connected()) ++count;
		}
		return count;
	}

	// Remove handles for disconnected receivers
	void cull_disconnected() {
		handles_.erase(
			std::remove_if(handles_.begin(), handles_.end(),
				[](const std::unique_ptr<handle_base>& h) {
					return !h->connected();
				}),
			handles_.end()
		);
	}

	// Check if any receivers are tracked
	bool empty() const {
		return handles_.empty();
	}
};

} // namespace jai

#endif // __JAISCRIPT_SIGNALS_RECEIVER_OWNER_HPP__
