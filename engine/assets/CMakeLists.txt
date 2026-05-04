# engine/assets — asset registry, UUID identity, async load state machine.
# Phase 3 task 1 (interface): tide/assets/Asset.h, AssetDB.h, IAssetLoader.h.
# Phase 3 tasks 4–6 (added): TextureAsset, ShaderAsset, MmapFile, concrete
#   loaders (Mesh / Texture / Shader), and AssetDB::load_blocking().
# Phase 3 tasks 7–10 fill in async pump and shader hot-reload.
#
# xxhash is PRIVATE because it's an implementation detail of the loaders'
# content-hash check (ADR-0017). Public consumers see only the typed
# `AssetHandle<T>` / `AssetDB::get<T>()` surface.
find_package(xxHash CONFIG REQUIRED)

tide_add_module(
    NAME assets
    PUBLIC_DEPS
        Tide::core
    PRIVATE_DEPS
        xxHash::xxhash
)
