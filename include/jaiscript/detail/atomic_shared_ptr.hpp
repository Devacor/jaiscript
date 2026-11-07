#pragma once

#include <atomic>
#include <memory>
#include <version>

namespace jai::detail {

#if defined(__cpp_lib_atomic_shared_ptr)
template<class T>
using atomic_shared_ptr = std::atomic<std::shared_ptr<T>>;
#else
// libc++ (Android NDK, Apple) has no std::atomic<std::shared_ptr>; its atomic_load/atomic_store overloads give the same handoff
template<class T>
class atomic_shared_ptr {
public:
    atomic_shared_ptr() = default;
    atomic_shared_ptr(std::shared_ptr<T> value) noexcept : value_(std::move(value)) {}
    atomic_shared_ptr(const atomic_shared_ptr&) = delete;
    atomic_shared_ptr& operator=(const atomic_shared_ptr&) = delete;

    std::shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return std::atomic_load_explicit(&value_, order);
    }
    void store(std::shared_ptr<T> value, std::memory_order order = std::memory_order_seq_cst) noexcept {
        std::atomic_store_explicit(&value_, std::move(value), order);
    }

private:
    std::shared_ptr<T> value_;
};
#endif

}
