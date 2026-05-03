#include "tide/assets/AssetDB.h"
#include "tide/assets/IAssetLoader.h"
#include "tide/assets/Uuid.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <span>

namespace {

using tide::assets::AssetDB;
using tide::assets::AssetError;
using tide::assets::AssetKind;
using tide::assets::AssetState;
using tide::assets::IAssetLoader;
using tide::assets::OpaquePayload;
using tide::assets::Uuid;

// Concrete payload type used only for compile-time tag dispatch in tests.
// Real MeshAsset / TextureAsset / ShaderAsset structs land in P3 atomic tasks
// 3–5 alongside their cooker emitters; for the DB-only unit test, the forward
// declaration in Asset.h is enough since we never dereference the payload.

struct MockPayload {
    int tag{0};
};

// Minimal loader for whichever AssetKind the test asks for. Records calls
// without doing real work.
class MockLoader : public IAssetLoader {
public:
    explicit MockLoader(AssetKind k) : kind_(k) {}

    [[nodiscard]] AssetKind kind() const noexcept override { return kind_; }

    [[nodiscard]] tide::expected<OpaquePayload, AssetError>
    load(Uuid /*uuid*/, std::span<const std::byte> /*bytes*/) override {
        ++load_calls;
        return &payload_;
    }

    void unload(OpaquePayload p) noexcept override {
        ++unload_calls;
        if (p == &payload_) {
            payload_seen_unload = true;
        }
    }

    int  load_calls = 0;
    int  unload_calls = 0;
    bool payload_seen_unload = false;

private:
    AssetKind   kind_;
    MockPayload payload_{};
};

} // namespace

TEST_SUITE("assets/AssetDB") {
    TEST_CASE("Fresh DB has zero live and zero pending") {
        AssetDB db;
        CHECK(db.live_count() == 0);
        CHECK(db.pending_count() == 0);
    }

    TEST_CASE("request<MeshAsset> returns Pending handle on first call") {
        AssetDB db;
        const auto uuid = Uuid::make_v4();
        const auto handle = db.request<tide::assets::MeshAsset>(uuid);
        REQUIRE(handle.has_value());
        CHECK(handle->valid());
        CHECK(db.state(*handle) == AssetState::Pending);
        CHECK(db.live_count() == 1);
        CHECK(db.pending_count() == 1);
        CHECK(db.get(*handle) == nullptr); // not Loaded yet
    }

    TEST_CASE("request dedups on UUID — same handle, refcount bumps") {
        AssetDB db;
        const auto uuid = Uuid::make_v4();
        const auto h1 = db.request<tide::assets::MeshAsset>(uuid);
        const auto h2 = db.request<tide::assets::MeshAsset>(uuid);
        REQUIRE(h1.has_value());
        REQUIRE(h2.has_value());
        CHECK(*h1 == *h2);
        CHECK(db.live_count() == 1);

        // Releasing once leaves the asset live (refcount: 2 -> 1).
        db.release(*h1);
        CHECK(db.live_count() == 1);
        CHECK(db.state(*h2) == AssetState::Pending);

        // Second release frees the slot.
        db.release(*h2);
        CHECK(db.live_count() == 0);
    }

    TEST_CASE("request<TextureAsset> on an already-Mesh UUID returns KindMismatch") {
        AssetDB db;
        const auto uuid = Uuid::make_v4();
        const auto mesh = db.request<tide::assets::MeshAsset>(uuid);
        REQUIRE(mesh.has_value());

        const auto tex = db.request<tide::assets::TextureAsset>(uuid);
        REQUIRE_FALSE(tex.has_value());
        CHECK(tex.error() == AssetError::KindMismatch);

        db.release(*mesh);
    }

    TEST_CASE("mark_loaded transitions state and exposes payload") {
        AssetDB db;
        MockLoader loader(AssetKind::Mesh);
        REQUIRE(db.register_loader(&loader).has_value());

        const auto uuid = Uuid::make_v4();
        const auto h = db.request<tide::assets::MeshAsset>(uuid);
        REQUIRE(h.has_value());

        MockPayload p{42};
        db.mark_loaded(uuid, &p);

        CHECK(db.state(*h) == AssetState::Loaded);
        CHECK(reinterpret_cast<const MockPayload*>(db.get(*h))->tag == 42);

        db.release(*h);
        CHECK(loader.unload_calls == 1);
        CHECK(db.live_count() == 0);
    }

    TEST_CASE("mark_failed transitions state and surfaces the error") {
        AssetDB db;
        const auto uuid = Uuid::make_v4();
        const auto h = db.request<tide::assets::MeshAsset>(uuid);
        REQUIRE(h.has_value());

        db.mark_failed(uuid, AssetError::IoError);

        CHECK(db.state(*h) == AssetState::Failed);
        CHECK(db.error_of(*h) == AssetError::IoError);
        CHECK(db.get(*h) == nullptr);
        db.release(*h);
    }

    TEST_CASE("Stale handle after release is rejected (ABA-safe via generation bump)") {
        AssetDB db;
        const auto uuid_a = Uuid::make_v4();
        const auto h1 = db.request<tide::assets::MeshAsset>(uuid_a);
        REQUIRE(h1.has_value());
        const auto idx = h1->index;
        const auto gen = h1->generation;

        db.release(*h1);

        const auto uuid_b = Uuid::make_v4();
        const auto h2 = db.request<tide::assets::MeshAsset>(uuid_b);
        REQUIRE(h2.has_value());
        // Slot should be reused (LIFO free list).
        CHECK(h2->index == idx);
        // Generation must have bumped.
        CHECK(h2->generation != gen);

        // Stale h1 must not resolve.
        CHECK(db.state(*h1) == AssetState::Failed);
        CHECK(db.state(*h2) == AssetState::Pending);
        db.release(*h2);
    }

    TEST_CASE("register_loader rejects duplicate kind") {
        AssetDB db;
        MockLoader a(AssetKind::Mesh);
        MockLoader b(AssetKind::Mesh);
        REQUIRE(db.register_loader(&a).has_value());
        const auto second = db.register_loader(&b);
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error() == AssetError::InvalidArgument);
    }

    TEST_CASE("register_loader rejects null pointer") {
        AssetDB db;
        const auto r = db.register_loader(nullptr);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == AssetError::InvalidArgument);
    }

    TEST_CASE("Default IAssetLoader::reload returns Unsupported (ADR-0015)") {
        MockLoader loader(AssetKind::Mesh);
        const auto bytes = std::span<const std::byte>{};
        const auto r = loader.reload(Uuid::make_v4(), bytes, nullptr);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == AssetError::Unsupported);
    }
}
