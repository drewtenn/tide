#include "tide/assets/AssetDB.h"

#include "tide/assets/IAssetLoader.h"
#include "tide/assets/MmapFile.h"
#include "tide/core/Log.h"
#include "tide/jobs/IJobSystem.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
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
    // P3 task 7: in-flight async-load bookkeeping. `job_submitted` is the
    // dedup CAS-target; `outstanding_job` records the JobHandle so callers
    // can wait. Both are reset on slot release. `~AssetDB()` waits on the
    // separate `Impl::in_flight_jobs` counter (incremented on submit, dec on
    // lambda exit) — that counter, not these per-slot fields, is what makes
    // teardown UAF-safe even if a slot was already released.
    std::atomic<bool>          job_submitted{false};
    tide::jobs::JobHandle      outstanding_job{};

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
          mmap(std::move(o.mmap)),
          job_submitted(o.job_submitted.load(std::memory_order_relaxed)),
          outstanding_job(o.outstanding_job) {}
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

    // ─── P3 task 7: async-load drain ────────────────────────────────────────
    // Every load_async() that successfully submits a job increments
    // `in_flight_jobs` BEFORE the lambda is enqueued; the lambda decrements
    // it on exit (success or failure). `~AssetDB()` waits on this counter
    // so all in-flight lambdas have finished dereferencing `this`/`impl_`/
    // `loaders_by_kind` before teardown. Per-slot bookkeeping (job_submitted,
    // outstanding_job) is best-effort and racy under release(); this counter
    // is the actual UAF-safety mechanism.
    tide::jobs::IJobSystem*    jobs_sys{nullptr};
    std::atomic<int>           in_flight_jobs{0};
    mutable std::mutex         drain_mutex;
    std::condition_variable    drain_cv;

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
AssetDB::AssetDB(jobs::IJobSystem* jobs) : impl_(std::make_unique<Impl>()) {
    impl_->jobs_sys = jobs;
}

AssetDB::~AssetDB() {
    // Block until every in-flight load_async lambda has decremented the
    // counter. The lambdas capture `this` and dereference `impl_`, the
    // registered loaders, and the slot vector — none of which may unwind
    // while a lambda is mid-call. A non-job-system AssetDB never increments
    // the counter, so the predicate is satisfied immediately.
    if (impl_) {
        std::unique_lock lock(impl_->drain_mutex);
        impl_->drain_cv.wait(lock, [this] {
            return impl_->in_flight_jobs.load(std::memory_order_acquire) == 0;
        });
    }
}

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
    // Reset async-load bookkeeping so the next allocation of this slot
    // starts clean. A still-running load lambda from the previous generation
    // is filtered out by its captured-generation check on completion (see
    // load_async); the counter (`in_flight_jobs`) keeps `~AssetDB()` honest
    // independently of whether the slot was already reused.
    s.job_submitted.store(false, std::memory_order_relaxed);
    s.outstanding_job = tide::jobs::JobHandle{};
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

// ─── Async load (P3 task 7) ─────────────────────────────────────────────────

tide::expected<jobs::JobHandle, AssetError>
AssetDB::load_async(Uuid uuid, std::filesystem::path path) {
    if (impl_->jobs_sys == nullptr) {
        return tide::unexpected{AssetError::Unsupported};
    }

    // Step 1 — validate slot exists, is Pending, has a loader, and CAS the
    // dedup flag. All under unique_lock so we serialize against release_impl
    // and against another concurrent load_async on the same slot.
    std::uint32_t slot_index = kInvalidIndex;
    std::uint32_t generation = 0;
    IAssetLoader* loader     = nullptr;
    {
        std::unique_lock lock(impl_->mutex);
        auto it = impl_->uuid_to_index.find(uuid);
        if (it == impl_->uuid_to_index.end()) {
            return tide::unexpected{AssetError::NotFound};
        }
        slot_index = it->second;
        Slot& s = impl_->slots[slot_index];
        if (!s.alive) {
            return tide::unexpected{AssetError::NotFound};
        }
        if (s.state.load(std::memory_order_acquire) != AssetState::Pending) {
            // Same rule as load_blocking: re-load is hot-reload territory
            // (P3 task 10), not the async-from-Pending path.
            return tide::unexpected{AssetError::Unsupported};
        }
        const auto kind_idx = Impl::kind_index(s.kind);
        if (kind_idx >= impl_->loaders_by_kind.size()) {
            return tide::unexpected{AssetError::Unsupported};
        }
        loader = impl_->loaders_by_kind[kind_idx];
        if (loader == nullptr) {
            return tide::unexpected{AssetError::Unsupported};
        }
        bool expected = false;
        if (!s.job_submitted.compare_exchange_strong(
                expected, true,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // Another load_async won the race; it owns the in-flight job.
            return tide::unexpected{AssetError::AlreadyInFlight};
        }
        generation = s.generation;
        // Increment the drain counter while still under unique_lock. A racing
        // `~AssetDB()` would otherwise see `in_flight_jobs == 0` between this
        // function's lock release and the increment, then return — leaving
        // `impl_` destructed before the lambda runs. The destructor still
        // requires the standard C++ contract that no thread calls into the
        // AssetDB during its destruction; this just removes the in-API window.
        impl_->in_flight_jobs.fetch_add(1, std::memory_order_acq_rel);
    }

    // Step 3 — submit the load lambda. We capture `path` by value (move) so
    // the caller's std::filesystem::path can go out of scope immediately.
    // We do NOT hold impl_->mutex across submit(): the inline job system
    // runs the lambda synchronously inside submit(), and the lambda needs
    // unique_lock to install the payload — holding mutex here would deadlock.
    //
    // The decrement-and-notify is RAII-guarded: a `std::system_error` thrown
    // from the lock primitives inside complete_load_*() (or any future
    // additional source) must NOT leak the in-flight counter, otherwise
    // `~AssetDB()` deadlocks waiting for a job that already terminated.
    auto handle = impl_->jobs_sys->submit(jobs::JobDesc{
        .fn = [this, uuid, slot_index, generation, loader,
               path = std::move(path)]() {
            struct DrainGuard {
                Impl* impl;
                ~DrainGuard() {
                    if (impl->in_flight_jobs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        std::lock_guard<std::mutex> dl(impl->drain_mutex);
                        impl->drain_cv.notify_all();
                    }
                }
            };
            DrainGuard guard{impl_.get()};

            // Mirror of load_blocking() steps 2-4, keyed by (slot_index,
            // generation) instead of UUID so a release()-then-reuse race
            // can't graft this load onto a fresh request for the same UUID.
            auto mmap = MmapFile::open_read(path);
            if (!mmap) {
                complete_load_failed(slot_index, generation, mmap.error());
                return;
            }
            const auto bytes = mmap->bytes();

            auto loaded = loader->load(uuid, bytes);
            if (!loaded) {
                complete_load_failed(slot_index, generation, loaded.error());
                return;
            }
            void* payload = *loaded;

            complete_load_success(slot_index, generation, loader, payload, std::move(*mmap));
        },
        .deps = {},
        .name = "AssetLoad",
    });

    // Step 4 — record the JobHandle on the slot for diagnostics. Under
    // unique_lock for visibility; the slot might already have been freed
    // and re-allocated if the inline executor ran the lambda + a concurrent
    // release() landed before this point. The generation guard discards the
    // stale write.
    {
        std::unique_lock lock(impl_->mutex);
        Slot& s = impl_->slots[slot_index];
        if (s.alive && s.generation == generation) {
            s.outstanding_job = handle;
        }
    }
    return handle;
}

void AssetDB::complete_load_success(std::uint32_t slot_index,
                                    std::uint32_t generation,
                                    IAssetLoader* loader,
                                    void*         payload,
                                    MmapFile&&    mmap) noexcept {
    std::unique_lock lock(impl_->mutex);
    if (slot_index >= impl_->slots.size()) {
        loader->unload(payload);
        return;
    }
    Slot& s = impl_->slots[slot_index];
    if (!s.alive || s.generation != generation) {
        // Slot freed (and possibly re-allocated) between submit and this
        // completion. Drop the loader's payload — same release-window
        // discipline as load_blocking() step 4.
        loader->unload(payload);
        return;
    }
    s.mmap = std::move(mmap);
    s.payload.store(payload, std::memory_order_release);
    s.state.store(AssetState::Loaded, std::memory_order_release);
}

void AssetDB::complete_load_failed(std::uint32_t slot_index,
                                   std::uint32_t generation,
                                   AssetError    err) noexcept {
    // Generation-keyed; UUID-keyed mark_failed() would mis-flip a freshly
    // re-allocated slot that happens to share the UUID.
    std::shared_lock lock(impl_->mutex);
    if (slot_index >= impl_->slots.size()) {
        return;
    }
    Slot& s = impl_->slots[slot_index];
    if (!s.alive || s.generation != generation) {
        return;
    }
    s.error.store(err, std::memory_order_release);
    s.state.store(AssetState::Failed, std::memory_order_release);
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
