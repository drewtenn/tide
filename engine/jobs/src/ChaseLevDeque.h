#pragma once

// ChaseLevDeque — single-owner / multi-thief lock-free SPMC deque.
// Reference: Chase & Lev, "Dynamic Circular Work-Stealing Deque" (SPAA 2005);
// memory-order corrections from Lê / Pop / Cohen "Correct and Efficient
// Work-Stealing for Weak Memory Models" (PPoPP 2013).
//
// Only the OWNING thread may call push() and pop_bottom(); ANY thread may
// call steal(). The deque is fixed-capacity in Phase 1 (overflow returns
// false from push() with a TIDE_LOG_WARN — Phase 3+ adds growth via
// epoch-based reclamation).
//
// The atomic ordering is the load-bearing piece. Read the Lê paper before
// touching anything in this file.

#include "tide/core/Log.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <new>     // hardware_destructive_interference_size

namespace tide::jobs {

#ifdef __cpp_lib_hardware_interference_size
constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
constexpr std::size_t kCacheLine = 64;   // M-series and most x86_64
#endif

template <typename T, std::size_t Capacity>
class ChaseLevDeque {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(Capacity >= 4, "Capacity must be ≥ 4");

public:
    ChaseLevDeque() = default;
    ChaseLevDeque(const ChaseLevDeque&) = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;

    // Owner-only. Returns true on success, false on overflow.
    // Caller is responsible for any logging — push() is in the hot path
    // and must not allocate or format strings.
    bool push(T value) noexcept {
        const int64_t b = bottom_.load(std::memory_order_relaxed);
        const int64_t t = top_.load(std::memory_order_acquire);
        if (b - t >= static_cast<int64_t>(Capacity)) {
            return false;
        }
        buffer_[static_cast<std::size_t>(b) & kMask] = value;
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
        return true;
    }

    // Owner-only. Returns null on empty.
    T pop_bottom() noexcept {
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t t = top_.load(std::memory_order_relaxed);

        if (t > b) {
            // Empty — restore bottom.
            bottom_.store(b + 1, std::memory_order_relaxed);
            return T{};
        }

        T value = buffer_[static_cast<std::size_t>(b) & kMask];
        if (t < b) {
            // Not the last element — no race with thieves.
            return value;
        }

        // t == b: last element. Race with steal() over the top index.
        if (!top_.compare_exchange_strong(t, t + 1,
                                          std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
            value = T{};
        }
        bottom_.store(b + 1, std::memory_order_relaxed);
        return value;
    }

    // Any-thread. Returns null on empty or contention loss.
    T steal() noexcept {
        int64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const int64_t b = bottom_.load(std::memory_order_acquire);
        if (t >= b) return T{};   // empty

        T value = buffer_[static_cast<std::size_t>(t) & kMask];
        // CAS top: if it succeeds, we own the element; if not, another thief
        // (or the owner via pop_bottom) raced us — back off and try elsewhere.
        if (!top_.compare_exchange_strong(t, t + 1,
                                          std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
            return T{};
        }
        return value;
    }

    [[nodiscard]] std::size_t approx_size() const noexcept {
        const int64_t b = bottom_.load(std::memory_order_relaxed);
        const int64_t t = top_.load(std::memory_order_relaxed);
        return b > t ? static_cast<std::size_t>(b - t) : 0u;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Pad the two hot atomics to separate cache lines to defeat false
    // sharing — top_ is hammered by thieves, bottom_ by the owner.
    alignas(kCacheLine) std::atomic<int64_t> top_{0};
    alignas(kCacheLine) std::atomic<int64_t> bottom_{0};
    alignas(kCacheLine) std::array<T, Capacity> buffer_{};
};

} // namespace tide::jobs
