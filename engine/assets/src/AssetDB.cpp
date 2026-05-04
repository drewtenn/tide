#include "tide/assets/AssetDB.h"

#include "tide/assets/IAssetLoader.h"
#include "tide/assets/MmapFile.h"
#include "tide/core/Log.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace tide::assets {

namespace {

constexpr std::uint32_t kInvalidIndex      = ~0u;
constexpr std::uint32_t kInitialGeneration = 1;

struct Slot {
    Uuid                       uuid{};
    AssetKind                  kind{AssetKind::Manifest};
    std::atomic<AssetState>    state{AssetState::Pending};
    std::atomic<AssetError>    error{AssetError::NotFound};
    std::atomic<void*>         payload{nullptr};
    std::atomic<std::uint32_t> ref_count{0};
    std::uint32_t              generation{kInitialGeneration};
    bool                       alive{false};
    std::uint32_t              next_free{kInvalidIndex};
    // Cooked-artifact mmap. The loader's returned payload pointer is an
    // offset into these bytes (zero-copy load), so the mapping must live
    // at least as long as the slot. Released alongside the slot in
    // release_impl(). Default-constructed (empty) until load_blocking
    // succeeds.
    MmapFile                   mmap{};

    Slot() = default;
    Slot(const Slot&)            = delete;
    Slot& operator=(const Slot&) = delete;

    // Move ctor exists only to satisfy `std::vector<Slot>`'s template
    // requirement (the buffer-relocation path is instantiated by `reserve()`
    // even when no relocation actually happens). The pool never grows past
    // its reserved capacity — `request_impl()` returns `PoolExhausted` first —
    // so this move ctor is dead code at runtime. The atomic loads here would
    // be racy if a real relocation occurred concurrent with reads, which is
    // exactly why the never-grow discipline exists.
    Slot(Slot&& o) noexcept
        : uuid(o.uuid),
          kind(o.kind),
          state(o.state.load(std::memory_order_relaxed)),
          error(o.error.load(std::memory_order_relaxed)),
          payload(o.payload.load(std::memory_order_relaxed)),
          ref_count(o.ref_count.load(std::memory_order_relaxed)),
          generation(o.generation),
          alive(o.alive),
          next_free(o.next_free),
          mmap(std::move(o.mmap)) {}
    Slot& operator=(Slot&&) = delete;
};

struct UuidHash {
    [[nodiscard]] std::size_t operator()(const Uuid& u) const noexcept {
        return static_cast<std::size_t>(u.hash());
    }
};

} // namespace

struct AssetDB::Impl {
    // Slot storage. Reserved up-front to avoid reallocation invalidating
    // pointers held by readers concurrent with allocate (per the same
    // discipline as `HandlePool::reserve` — see ADR-0003 consequences).
    std::vector<Slot>                                          slots;
    std::uint32_t                                              free_head{kInvalidIndex};
    std::unordered_map<Uuid, std::uint32_t, UuidHash>          uuid_to_index;
    std::array<IAssetLoader*, 8>                               loaders_by_kind{}; // indexed by AssetKind enum value

    // Single shared_mutex over the slot pool + dedup map. Read-mostly via
    // state_impl()/get_impl(); write side is request_impl()/release_impl().
    // Atomics on per-slot state allow truly lock-free reads in the common
    // case once the index/generation pair is known to be valid.
    mutable std::shared_mutex mutex;

    static constexpr std::size_t kInitialReserve = 256;

    Impl() {
        slots.reserve(kInitialReserve);
        loaders_by_kind.fill(nullptr);
    }

    [[nodiscard]] static std::size_t kind_index(AssetKind k) noexcept {
        return static_cast<std::size_t>(k);
    }
};

AssetDB::AssetDB() : impl_(std::make_unique<Impl>()) {}
AssetDB::~AssetDB() = default;

// ─── Public: register_loader ────────────────────────────────────────────────

tide::expected<void, AssetError> AssetDB::register_loader(IAssetLoader* loader) {
    if (loader == nullptr) {
        return tide::unexpected{AssetError::InvalidArgument};
    }
    const auto idx = Impl::kind_index(loader->kind());
    if (idx >= impl_->loaders_by_kind.size()) {
        return tide::unexpected{AssetError::InvalidArgument};
    }
    std::unique_lock lock(impl_->mutex);
    if (impl_->loaders_by_kind[idx] != nullptr) {
        return tide::unexpected{AssetError::InvalidArgument};
    }
    impl_->loaders_by_kind[idx] = loader;
    return {};
}

// ─── Internal: request_impl ─────────────────────────────────────────────────

tide::expected<AssetDB::UntypedHandle, AssetError>
AssetDB::request_impl(Uuid uuid, AssetKind kind) {
    std::unique_lock lock(impl_->mutex);

    // Dedup: existing UUID returns the same handle and bumps refcount.
    if (auto it = impl_->uuid_to_index.find(uuid); it != impl_->uuid_to_index.end()) {
        const auto idx = it->second;
        Slot& s = impl_->slots[idx];
        if (s.kind != kind) {
            return tide::unexpected{AssetError::KindMismatch};
        }
        s.ref_count.fetch_add(1, std::memory_order_relaxed);
        return UntypedHandle{idx, s.generation};
    }

    // Allocate a new slot — free list first, else grow.
    std::uint32_t idx;
    if (impl_->free_head != kInvalidIndex) {
        idx = impl_->free_head;
        impl_->free_head = impl_->slots[idx].next_free;
    } else {
        if (impl_->slots.size() >= impl_->slots.capacity()) {
            // Pool exhaustion is a hard error — growth invalidates pointers
            // held by readers. Accept the bound for now; raise via reserve()
            // (P5+ adds a config knob).
            return tide::unexpected{AssetError::PoolExhausted};
        }
        idx = static_cast<std::uint32_t>(impl_->slots.size());
        impl_->slots.emplace_back();
        impl_->slots[idx].generation = kInitialGeneration;
    }

    Slot& s = impl_->slots[idx];
    s.uuid = uuid;
    s.kind = kind;
    s.state.store(AssetState::Pending, std::memory_order_release);
    s.error.store(AssetError::NotFound, std::memory_order_relaxed);
    s.payload.store(nullptr, std::memory_order_relaxed);
    s.ref_count.store(1, std::memory_order_relaxed);
    s.alive = true;
    s.next_free = kInvalidIndex;

    impl_->uuid_to_index.emplace(uuid, idx);

    return UntypedHandle{idx, s.generation};
}

// ─── Internal: release_impl ─────────────────────────────────────────────────

void AssetDB::release_impl(UntypedHandle h) noexcept {
    if (h.index >= impl_->slots.size()) {
        return;
    }
    std::unique_lock lock(impl_->mutex);
    if (h.index >= impl_->slots.size()) {
        return;
    }
    Slot& s = impl_->slots[h.index];
    if (!s.alive || s.generation != h.generation) {
        return; // stale handle; ABA-safe
    }
    const auto prev = s.ref_count.fetch_sub(1, std::memory_order_acq_rel);
    if (prev != 1) {
        return; // still referenced
    }

    // Last reference dropped — free the slot. P3 destroys the payload
    // synchronously via the registered loader; P4+ defers via the frame
    // graph's retired-resource list (per ADR-0015 forward-design hooks).
    if (auto* payload = s.payload.load(std::memory_order_acquire)) {
        if (auto* loader = impl_->loaders_by_kind[Impl::kind_index(s.kind)]) {
            loader->unload(payload);
        }
    }

    impl_->uuid_to_index.erase(s.uuid);

    s.alive = false;
    s.payload.store(nullptr, std::memory_order_relaxed);
    s.state.store(AssetState::Failed, std::memory_order_relaxed);
    // Release the mmap last, so the loader's unload() (above) can still
    // dereference any pointers into the mapped bytes if needed. P3 loaders
    // are stateless wrappers around the mmap, but P5+ loaders may keep
    // GPU resources keyed off the payload pointer.
    s.mmap = MmapFile{};
    ++s.generation;
    if (s.generation == 0) {
        s.generation = kInitialGeneration; // skip 0 (null-handle generation)
    }
    s.next_free = impl_->free_head;
    impl_->free_head = h.index;
}

// ─── Lock-free read paths ───────────────────────────────────────────────────

AssetState AssetDB::state_impl(UntypedHandle h) const noexcept {
    if (h.index >= impl_->slots.size()) {
        return AssetState::Failed;
    }
    const Slot& s = impl_->slots[h.index];
    // Generation check is a benign race: if a slot has been freed and
    // reallocated, the caller's generation no longer matches and we report
    // Failed. We do not lock — slots are never deleted (the vector never
    // shrinks), so the read is well-defined on the slot bytes; per-slot
    // atomics provide the visibility guarantee for state itself.
    if (s.generation != h.generation || !s.alive) {
        return AssetState::Failed;
    }
    return s.state.load(std::memory_order_acquire);
}

AssetError AssetDB::error_impl(UntypedHandle h) const noexcept {
    if (h.index >= impl_->slots.size()) {
        return AssetError::NotFound;
    }
    const Slot& s = impl_->slots[h.index];
    if (s.generation != h.generation || !s.alive) {
        return AssetError::NotFound;
    }
    return s.error.load(std::memory_order_acquire);
}

const void* AssetDB::payload_impl(UntypedHandle h) const noexcept {
    if (h.index >= impl_->slots.size()) {
        return nullptr;
    }
    const Slot& s = impl_->slots[h.index];
    if (s.generation != h.generation || !s.alive) {
        return nullptr;
    }
    if (s.state.load(std::memory_order_acquire) != AssetState::Loaded) {
        return nullptr;
    }
    return s.payload.load(std::memory_order_acquire);
}

// ─── Loader-facing transitions ──────────────────────────────────────────────

void AssetDB::mark_loaded(Uuid uuid, void* payload) noexcept {
    std::shared_lock lock(impl_->mutex);
    auto it = impl_->uuid_to_index.find(uuid);
    if (it == impl_->uuid_to_index.end()) {
        return;
    }
    Slot& s = impl_->slots[it->second];
    if (!s.alive) {
        return;
    }
    s.payload.store(payload, std::memory_order_release);
    s.state.store(AssetState::Loaded, std::memory_order_release);
}

void AssetDB::mark_failed(Uuid uuid, AssetError err) noexcept {
    std::shared_lock lock(impl_->mutex);
    auto it = impl_->uuid_to_index.find(uuid);
    if (it == impl_->uuid_to_index.end()) {
        return;
    }
    Slot& s = impl_->slots[it->second];
    if (!s.alive) {
        return;
    }
    s.error.store(err, std::memory_order_release);
    s.state.store(AssetState::Failed, std::memory_order_release);
}

// ─── Synchronous load (P3 task 6) ───────────────────────────────────────────

tide::expected<void, AssetError>
AssetDB::load_blocking(Uuid uuid, const std::filesystem::path& path) {
    // Step 1 — locate the slot, capture its generation, and reject any
    // attempt to re-load over an already-Loaded slot. Re-load is a P3
    // task 10 concern (hot-reload) and requires the deferred-destroy
    // infrastructure that doesn't land until P4's frame graph; for
    // tasks 4-6 we intentionally close the door on it so a second
    // load_blocking() can't munmap bytes a reader is still pointing into.
    IAssetLoader* loader     = nullptr;
    std::uint32_t slot_index = kInvalidIndex;
    std::uint32_t generation = 0;
    {
        std::shared_lock lock(impl_->mutex);
        auto it = impl_->uuid_to_index.find(uuid);
        if (it == impl_->uuid_to_index.end()) {
            return tide::unexpected{AssetError::NotFound};
        }
        slot_index = it->second;
        const Slot& s = impl_->slots[slot_index];
        if (!s.alive) {
            return tide::unexpected{AssetError::NotFound};
        }
        if (s.state.load(std::memory_order_acquire) != AssetState::Pending) {
            // Already Loaded or Failed. Re-loading via this path is not
            // supported in P3; ShaderLoader::reload() (P3 task 10) is the
            // sanctioned re-load surface and goes through the deferred-
            // destroy retired-resource list.
            return tide::unexpected{AssetError::Unsupported};
        }
        generation = s.generation;
        const auto kind_idx = Impl::kind_index(s.kind);
        if (kind_idx >= impl_->loaders_by_kind.size()) {
            return tide::unexpected{AssetError::Unsupported};
        }
        loader = impl_->loaders_by_kind[kind_idx];
        if (loader == nullptr) {
            return tide::unexpected{AssetError::Unsupported};
        }
    }

    // Step 2 — open the mmap. No locks held; concurrent loads can run.
    auto mmap = MmapFile::open_read(path);
    if (!mmap) {
        mark_failed(uuid, mmap.error());
        return tide::unexpected{mmap.error()};
    }
    const auto bytes = mmap->bytes();

    // Step 3 — dispatch to the loader. The loader validates the
    // RuntimeHeader, runs the xxh3 content check, and returns a payload
    // pointer pointing into the mmap's bytes.
    auto loaded = loader->load(uuid, bytes);
    if (!loaded) {
        mark_failed(uuid, loaded.error());
        return tide::unexpected{loaded.error()};
    }
    void* payload = *loaded;

    // Step 4 — install the mmap on the slot and transition to Loaded.
    // The generation check below is what distinguishes "same slot, same
    // request" from "slot was released and re-allocated for a new request
    // that happens to share the UUID" — a UUID-only check would silently
    // graft this load onto the new request.
    {
        std::unique_lock lock(impl_->mutex);
        Slot& s = impl_->slots[slot_index];
        if (!s.alive || s.uuid != uuid || s.generation != generation) {
            // Slot was released (and possibly re-allocated) between
            // step 1 and now. Drop the loader's payload — the mmap is
            // about to be munmapped, so any pointers into it are dead.
            // For P3 loaders payload-release is a no-op, but the
            // contract leaves room for P5+ loaders that own GPU
            // resources.
            loader->unload(payload);
            return tide::unexpected{AssetError::NotFound};
        }
        // The Pending check at step 1 + the generation check above + the
        // unique_lock here together guarantee we're the only writer
        // installing a payload into this generation of this slot. No
        // existing mmap to retire.
        s.mmap    = std::move(*mmap);
        s.payload.store(payload, std::memory_order_release);
        s.state.store(AssetState::Loaded, std::memory_order_release);
    }

    return {};
}

// ─── Diagnostics ────────────────────────────────────────────────────────────

std::size_t AssetDB::live_count() const noexcept {
    std::shared_lock lock(impl_->mutex);
    std::size_t n = 0;
    for (const auto& s : impl_->slots) {
        if (s.alive) {
            ++n;
        }
    }
    return n;
}

std::size_t AssetDB::pending_count() const noexcept {
    std::shared_lock lock(impl_->mutex);
    std::size_t n = 0;
    for (const auto& s : impl_->slots) {
        if (s.alive && s.state.load(std::memory_order_acquire) == AssetState::Pending) {
            ++n;
        }
    }
    return n;
}

} // namespace tide::assets
