#pragma once

// tide::assets::AssetDB — UUID-keyed asset registry with ref-counted dedup,
// async-load state machine, and per-kind loader dispatch.
//
// Public API is templated on the payload type T so call sites use type-safe
// `AssetHandle<MeshAsset>` etc. Internally the DB stores type-erased slots and
// dispatches loader work via the registered `IAssetLoader*` for the kind.
//
// State is read lock-free via per-slot atomics; allocations and the UUID
// dedup map are guarded by a single shared_mutex (read-mostly workload).
// Phase 3 scope: P3 atomic tasks 1 (interface, this file) and 7–8 (full pump).

#include "tide/assets/Asset.h"
#include "tide/assets/Uuid.h"
#include "tide/core/Expected.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace tide::assets {

class IAssetLoader;

class AssetDB {
public:
    AssetDB();
    ~AssetDB();
    AssetDB(const AssetDB&) = delete;
    AssetDB& operator=(const AssetDB&) = delete;
    AssetDB(AssetDB&&) = delete;
    AssetDB& operator=(AssetDB&&) = delete;

    // ─── Loader registry ────────────────────────────────────────────────────
    // Register a loader for one AssetKind. At most one loader per kind; a
    // second registration for the same kind is a hard error.
    [[nodiscard]] tide::expected<void, AssetError>
        register_loader(IAssetLoader* loader);

    // ─── Type-safe request / release ────────────────────────────────────────
    // Request an asset. Returns a handle in `Pending` state immediately; the
    // actual load runs on the worker pool and transitions state to Loaded or
    // Failed. Repeated requests for the same UUID return the same handle and
    // bump the internal ref count (dedup invariant).
    template <class T>
    [[nodiscard]] tide::expected<AssetHandle<T>, AssetError> request(Uuid uuid) {
        auto u = request_impl(uuid, kind_of<T>());
        if (!u) return tide::unexpected{u.error()};
        return AssetHandle<T>{u->index, u->generation};
    }

    // Release a reference. When the last ref drops, the slot is freed and
    // (in P4+) the payload is queued for deferred destruction after a frame
    // fence. Idempotent on null / stale handles.
    template <class T> void release(AssetHandle<T> h) noexcept {
        release_impl(UntypedHandle{h.index, h.generation});
    }

    // ─── Inspection (lock-free reads) ───────────────────────────────────────
    template <class T>
    [[nodiscard]] AssetState state(AssetHandle<T> h) const noexcept {
        return state_impl(UntypedHandle{h.index, h.generation});
    }

    template <class T>
    [[nodiscard]] AssetError error_of(AssetHandle<T> h) const noexcept {
        return error_impl(UntypedHandle{h.index, h.generation});
    }

    // Return the typed payload pointer if state == Loaded, else nullptr.
    // Casts the loader's `OpaquePayload` to `const T*`; the loader is
    // responsible for the actual T layout.
    template <class T>
    [[nodiscard]] const T* get(AssetHandle<T> h) const noexcept {
        return static_cast<const T*>(payload_impl(UntypedHandle{h.index, h.generation}));
    }

    // ─── Loader-facing transitions ──────────────────────────────────────────
    // Loaders call these from worker threads when load completes. AssetDB
    // does no further validation; the loader is trusted because it was
    // registered via register_loader().
    void mark_loaded(Uuid uuid, void* payload) noexcept;
    void mark_failed(Uuid uuid, AssetError error) noexcept;

    // ─── Synchronous load (P3 task 6) ───────────────────────────────────────
    // mmap a cooked artifact, validate the header, dispatch to the
    // registered loader, and transition the slot to Loaded. The mmap is
    // owned by the AssetDB slot and is released when the slot's last
    // reference drops (via release()).
    //
    // Preconditions: a `request<T>()` for `uuid` must already be live
    // (the slot exists in `Pending`) and a loader for the right `AssetKind`
    // must be registered. Hot path is mmap + memcmp(header) + xxh3
    // + cast — sub-millisecond per ADR-0017.
    //
    // P3 task 7 wraps this in a worker-pool job and exposes an async
    // pump; the synchronous form remains for tests and bootstrap loads.
    [[nodiscard]] tide::expected<void, AssetError>
        load_blocking(Uuid uuid, const std::filesystem::path& path);

    // ─── Diagnostics ────────────────────────────────────────────────────────
    [[nodiscard]] std::size_t live_count() const noexcept;
    [[nodiscard]] std::size_t pending_count() const noexcept;

private:
    struct UntypedHandle {
        std::uint32_t index;
        std::uint32_t generation;
    };

    // Type-erased internal API. Public template methods forward here.
    [[nodiscard]] tide::expected<UntypedHandle, AssetError>
        request_impl(Uuid uuid, AssetKind kind);
    void release_impl(UntypedHandle h) noexcept;
    [[nodiscard]] AssetState state_impl(UntypedHandle h) const noexcept;
    [[nodiscard]] AssetError error_impl(UntypedHandle h) const noexcept;
    [[nodiscard]] const void* payload_impl(UntypedHandle h) const noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tide::assets
