#pragma once

#include <cstddef>

namespace jai::detail {

// Caller-owned, instance-scoped fault injection for deterministic exception tests.
// Pools never retain this pointer and workers never invoke the callback. A callback
// may throw, but must not re-enter the engine/pool (some probes run under its lock).
struct fault_injection {
    void* context = nullptr;
    void (*callback)(void*, const char*, size_t) = nullptr;

    void checkpoint(const char* point, size_t index = 0) const {
        if (callback) { callback(context, point, index); }
    }
};

} // namespace jai::detail
