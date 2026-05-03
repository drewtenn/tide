#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace tide {

// Handle<Tag> is a typed integer pair (index, generation) used to refer to
// objects in a HandlePool without exposing pointers. The Tag template parameter
// makes Handle<TextureTag> and Handle<BufferTag> non-interconvertible.
//
// Layout: low 32 bits = index, high 32 bits = generation. A null handle is
// (kInvalidIndex, 0). Increment of generation on slot reuse defeats ABA.
template <class Tag> struct Handle {
    using IndexType = uint32_t;
    using GenerationType = uint32_t;

    static constexpr IndexType kInvalidIndex = std::numeric_limits<IndexType>::max();

    IndexType index{kInvalidIndex};
    GenerationType generation{0};

    [[nodiscard]] constexpr bool valid() const noexcept { return index != kInvalidIndex; }

    [[nodiscard]] constexpr uint64_t bits() const noexcept {
        return (static_cast<uint64_t>(generation) << 32) | static_cast<uint64_t>(index);
    }

    friend constexpr bool operator==(Handle a, Handle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }

    friend constexpr bool operator!=(Handle a, Handle b) noexcept { return !(a == b); }
};

// HandlePool<T, Tag> is the canonical owner of objects keyed by Handle<Tag>.
// It supports ABA-safe reuse: when a slot is freed, its generation increments,
// so any older Handle that still references the same index but the previous
// generation is rejected by get().
//
// Phase 0 implementation is single-threaded. ADR-0007 mandates that resource
// creation be callable from any thread by Phase 1 — that wraps a mutex around
// allocate/release; the algorithm here is not affected.
template <class T, class Tag> class HandlePool {
public:
    using HandleType = Handle<Tag>;

    HandlePool() = default;
    HandlePool(const HandlePool&) = delete;
    HandlePool& operator=(const HandlePool&) = delete;
    HandlePool(HandlePool&&) noexcept = default;
    HandlePool& operator=(HandlePool&&) noexcept = default;

    template <class... Args> [[nodiscard]] HandleType allocate(Args&&... args) {
        typename HandleType::IndexType index;
        if (free_head_ != HandleType::kInvalidIndex) {
            index = free_head_;
            free_head_ = slots_[index].next_free;
            slots_[index].next_free = HandleType::kInvalidIndex;
            slots_[index].alive = true;
            new (&slots_[index].storage) T(std::forward<Args>(args)...);
        } else {
            index = static_cast<typename HandleType::IndexType>(slots_.size());
            slots_.emplace_back();
            slots_[index].generation = 1;
            slots_[index].alive = true;
            new (&slots_[index].storage) T(std::forward<Args>(args)...);
        }
        return HandleType{index, slots_[index].generation};
    }

    bool release(HandleType h) noexcept {
        if (!owns(h)) return false;
        Slot& s = slots_[h.index];
        reinterpret_cast<T*>(&s.storage)->~T();
        s.alive = false;
        // Increment generation so any surviving Handle copy fails owns().
        // Skip generation 0 (reserved as the null-handle generation).
        ++s.generation;
        if (s.generation == 0) s.generation = 1;
        s.next_free = free_head_;
        free_head_ = h.index;
        return true;
    }

    [[nodiscard]] bool owns(HandleType h) const noexcept {
        if (!h.valid()) return false;
        if (h.index >= slots_.size()) return false;
        const Slot& s = slots_[h.index];
        return s.alive && s.generation == h.generation;
    }

    [[nodiscard]] T* get(HandleType h) noexcept {
        return owns(h) ? reinterpret_cast<T*>(&slots_[h.index].storage) : nullptr;
    }

    [[nodiscard]] const T* get(HandleType h) const noexcept {
        return owns(h) ? reinterpret_cast<const T*>(&slots_[h.index].storage) : nullptr;
    }

    [[nodiscard]] size_t size() const noexcept {
        size_t n = 0;
        for (const auto& s : slots_)
            if (s.alive) ++n;
        return n;
    }

    [[nodiscard]] size_t capacity() const noexcept { return slots_.size(); }

    // Pre-reserve storage so the underlying vector never reallocates.
    // Critical for concurrent use: with reservation, get() pointers stay
    // valid even when a different thread is mid-allocate(); without it,
    // the vector's buffer can move and dangle live get() pointers.
    void reserve(size_t n) {
        slots_.reserve(n);
    }

    void clear() noexcept {
        for (auto& s : slots_) {
            if (s.alive) {
                reinterpret_cast<T*>(&s.storage)->~T();
                s.alive = false;
            }
        }
        slots_.clear();
        free_head_ = HandleType::kInvalidIndex;
    }

    ~HandlePool() {
        for (auto& s : slots_) {
            if (s.alive) {
                reinterpret_cast<T*>(&s.storage)->~T();
            }
        }
    }

private:
    struct Slot {
        alignas(T) std::byte storage[sizeof(T)]{};
        typename HandleType::GenerationType generation{1};
        typename HandleType::IndexType next_free{HandleType::kInvalidIndex};
        bool alive{false};
    };

    std::vector<Slot> slots_;
    typename HandleType::IndexType free_head_{HandleType::kInvalidIndex};
};

} // namespace tide
