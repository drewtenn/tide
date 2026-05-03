# engine/assets — asset registry, UUID identity, async load state machine.
# Phase 3 task 1 (interface): tide/assets/Asset.h, AssetDB.h, IAssetLoader.h.
# Phase 3 tasks 6–10 fill in cooked-artifact mmap, async pump, hot-reload.
tide_add_module(
    NAME assets
    PUBLIC_DEPS
        Tide::core
)
