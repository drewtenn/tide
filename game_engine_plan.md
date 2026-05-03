# Game Engine Plan

A from-scratch C++ game engine, cross-platform (macOS first, then Windows, then Linux), with native rendering backends per platform (Metal / DX12 / Vulkan), swappable physics and audio, and a clean compartmentalized architecture inspired by Gregory's *Game Engine Architecture*.

**Scope honesty.** This is a competent indie engine — the target is "you could ship a small-to-mid 3D action game on it." It is not Unreal. It is not "an engine for new devs to learn on" either; that's a different product with different priorities. Every feature decision in this doc is filtered through the indie-shippable lens: skinning quality matters, GI and streaming and networking don't.

---

## 1. Guiding principles

**Layered architecture, strict downward dependencies.** Upper layers may call into lower ones; lower layers never call up. Layers cap at the spirit of Gregory's book and the only thing that keeps an engine from collapsing into a hairball after year two.

**Interfaces in the core, implementations in adapters.** Anything you want swappable — renderer backend, physics, audio, scripting — is defined as a pure abstract interface in a core module. Concrete backends live in their own modules and are linked in by the build configuration. Game code only ever sees the interface.

**Don't reinvent. Wrap.** Use third-party libraries aggressively, but always behind your own thin interface. This costs a few hours up front and saves you when you need to swap (e.g., FMOD → miniaudio for licensing reasons, Bullet → Jolt for performance).

**Data-oriented where it matters, OOP where it doesn't.** Hot paths (rendering, physics, ECS iteration) are data-oriented. Cold paths (asset loading, configuration, tooling) can be plain OOP — readability wins.

**Tooling is a first-class citizen, not an afterthought.** ImGui debug UI, hot-reload, **Tracy profiler instrumentation**, and **RenderDoc-trigger from in-engine** go in early. These are what make the engine feel alive while you're building it. Tracy zones in `jobs/`, `renderer/`, and `assets/` from Phase 1 — instrumenting later means you stare at flat 16ms frames with no idea why.

**Test what you can, golden-image what you can't.** Math, containers, allocators, and the RHI handle layer get unit tests. The renderer gets golden-frame image-diff tests in CI. If you can't tell whether a render regression happened, you have no engine — you have a slideshow.

---

## 2. Layered architecture

```
gameplay      scripting-lua  imgui-integration  editor
─────────────────────────────────────────────────────────
scene  scripting  audio-fmod  audio-miniaudio  physics-jolt  physics-physx
─────────────────────────────────────────────────────────
renderer  audio  physics  assets  jobs
─────────────────────────────────────────────────────────
rhi-metal  rhi-d3d12  rhi-vulkan
─────────────────────────────────────────────────────────
rhi  platform
─────────────────────────────────────────────────────────
core
```

Each box is a separate static library / CMake target. Cross-layer calls are only ever downward. Upward communication happens via events or callbacks registered by the upper layer.

---

## 3. Module breakdown

| Module | Purpose | Key deps |
|---|---|---|
| `core` | Math, containers, logging, asserts, allocators, **handles**, type traits, hashing | glm, spdlog, fmt, EnTT (re-exported) |
| `platform` | Window, filesystem, threads, high-res timer, dynamic loading | GLFW or SDL3 |
| `input` | Keyboard, mouse, gamepad. Action-mapping layer (named actions → bindings) so gameplay code never reaches for raw key codes. | platform |
| `jobs` | Task graph + worker pool. Used by everything that does parallel work. Tracy-instrumented from day one. | (built on `core`) |
| `rhi` | Render Hardware Interface — abstract GPU API | (interface only) |
| `rhi-metal` | Metal backend | Metal, MetalKit |
| `rhi-d3d12` | DirectX 12 backend | D3D12, DXGI |
| `rhi-vulkan` | Vulkan backend | Vulkan headers, VMA |
| `renderer` | High-level renderer: passes, materials, **frame graph**, scene submission | core, platform, assimp, cgltf, stb, KTX-Software |
| `assets` | Resource manager, async loading, hot-reload, refs | (interface only) |
| `audio` | Abstract `IAudioEngine` | (interface only) |
| `audio-miniaudio` | miniaudio backend (start here — single header) | miniaudio |
| `audio-fmod` | FMOD Studio backend (later, optional) | FMOD |
| `physics` | Abstract `IPhysicsWorld`, `IRigidBody`, etc. | (interface only) |
| `physics-jolt` | Jolt backend (start here — modern, fast, MIT) | JoltPhysics |
| `physics-physx` | PhysX 5 backend (later) | PhysX |
| `animation` | Skeletal animation, blend trees, IK | core, assets |
| `scene` | Scene graph, transform hierarchy, ECS world wrapper | core, EnTT |
| `scripting` | Abstract `IScriptHost` | (interface only) |
| `scripting-lua` | Lua via sol2 | Lua, sol2 |
| `gameplay` | Camera controllers, character controller, common components | scene, physics, input |
| `imgui-integration` | Dear ImGui hooked into platform + each RHI backend | imgui |
| `editor` | Optional ImGui-based editor (scene tree, inspector) — Phase 10+ | imgui-integration |
| `samples/*` | Sandbox apps and the demo game | everything |

**A note on PhysX specifically.** PhysX 5 is BSD-3 licensed (since 2022) so the licensing concern is gone. You mentioned wanting it. Worth knowing — Jolt (used by Horizon Forbidden West, MIT licensed) is now generally considered easier to integrate and better-performing for most use cases than PhysX. Still build the abstraction so you can use either, but I'd start with Jolt and add PhysX as the second backend to validate the abstraction is actually backend-agnostic. Forcing yourself to support two backends from the start is the only way to know your abstraction is real.

**A note on 2D.** Box2D got dropped. Trying to express it through `IPhysicsWorld`/`IRigidBody` constrains the 3D abstraction for very little payoff. If you need 2D physics later, give it its own `IPhysicsWorld2D` abstraction — don't warp the 3D one.

---

## 4. The RHI — your most important design decision

You picked native-per-platform, which means the RHI is doing real work. Here's the design that's stood up across multiple AAA engines.

**Modern-API style (DX12 / Vulkan / Metal share this shape).**
All three modern APIs converge on the same concepts. Your RHI should expose those concepts directly rather than trying to look like OpenGL.

- **Device** — the GPU. Created once.
- **Swapchain** — the screen surface, owns N back buffers.
- **CommandQueue** — submits work to the GPU.
- **CommandBuffer / CommandList** — recorded work, reusable per frame.
- **Buffer / Texture** — GPU memory resources, with usage flags.
- **PipelineState (PSO)** — compiled shader + fixed-function state.
- **DescriptorSet / BindGroup** — resource bindings handed to a draw.
- **Fence / Semaphore** — CPU↔GPU and GPU↔GPU sync.
- **RenderPass / RenderTarget** — what you render into; explicit load/store ops.

### Concrete interface sketch

```cpp
namespace rhi {

class IDevice {
public:
    virtual ~IDevice() = default;
    virtual BufferHandle      createBuffer(const BufferDesc&) = 0;
    virtual TextureHandle     createTexture(const TextureDesc&) = 0;
    virtual ShaderHandle      createShader(const ShaderDesc&) = 0;
    virtual PipelineHandle    createPipeline(const PipelineDesc&) = 0;
    virtual CommandBufferHandle createCommandBuffer() = 0;
    virtual void              acquireCommandBuffer(...) = 0;
    virtual void              submit(...) = 0;
    virtual void              present(...) = 0;
    virtual void              destroy(...) = 0;
    // ... destroy() for each handle type, or a templated variant
};

std::unique_ptr<IDevice> createDevice(GraphicsBackend, const DeviceDesc&);

} // namespace rhi
```

### Key design choices baked into that sketch

- **Opaque handles, not pointers.** A `BufferHandle` is a typed integer (generation+index pair). Lets the backend manage lifetime however it wants — bump allocator, pool, refcount — without leaking through the interface. Also keeps headers light: the interface header doesn't need to include Vulkan/Metal/DX headers.
- **Descriptors-as-structs.** `BufferDesc`, `TextureDesc`, etc. are POD config structs. This matches all three native APIs and keeps the creation calls simple.
- **No per-resource virtual dispatch.** You don't call `buffer->bind()`. You call `cmd.bindBuffer(handle, slot)`. Hot paths go through the command buffer, which is a single virtual call per draw call rather than per resource.
- **Implicit sync inside a frame.** Recommendation: start with high-level command queues (one CommandQueue per queue group rather than per resource).
- **Reflection-driven binding.** DXC + SPIRV-Cross give you shader reflection for free. Use it to populate `PipelineDesc` bindings rather than hand-writing them on every shader. Cheap now, painful to retrofit.
- **Bindless on day one (where supported).** DX12, Vulkan 1.2+, and Metal argument buffers all support bindless resource access. New engines lean bindless; descriptor sets become a fallback. ADR this — but lean toward bindless.

### A frame graph belongs in the renderer, not the RHI

With explicit-sync RHIs you will write a frame graph eventually — the only question is whether you write it on purpose in Phase 4 or by accident as scattered transition-manager code by Phase 6. Bake it in. It manages transient resources (gbuffers, depth, intermediate targets), tracks pass dependencies, and emits barriers. EA's, Unity's, and FrostBite's experience all converge here.

---

## 5. Phased plan

Each phase ends with something you can run and demo. Don't move on until the previous phase actually works.

### Phase 0 — Bootstrap (1–2 weeks)

- CMake project, vcpkg or Conan for deps.
- CI on GitHub Actions for macOS + Linux + Windows from day one. Cross-platform pain is cheapest to fix when there's almost no code.
- Pull in: glm, spdlog, fmt, GLFW (or SDL3), doctest, Dear ImGui.
- Folder layout, naming conventions, formatting (`.clang-format`), static analysis pass.
- Log/assert macros. Job system stub (single-threaded fallback is fine).
- **Deliverable:** an empty window opens on macOS, Windows, and Linux. CI is green.

### Phase 1 — RHI + first triangle on Metal + job system (4–6 weeks)

- Define the full RHI interface in `rhi/`.
- Implement `rhi-metal/`: end-to-end device, swapchain, buffer, texture, shader, pipeline, command buffer, fence, present.
- Shader pipeline: HLSL → SPIR-V (for Vulkan) → MSL (SPIRV-Cross), built as part of CMake. Pull in DXC.
- **Build out `jobs/` properly.** Task graph with dependencies, worker pool sized to hardware threads, fiber-based or task-stealing — pick one and commit. Renderer command recording, asset loading, and physics step will all assume this exists. Retrofitting a job system after Phase 4 is the single most expensive thing you can defer.
- Sample app: spinning textured triangle, ImGui overlay, frame stats.
- **Deliverable:** triangle on macOS via Metal. ImGui works.

### Phase 2 — Vulkan backend, then DX12, with image-diff CI (4–6 weeks each)

- Implement `rhi-vulkan/`. Run the same triangle sample. Run on macOS via MoltenVK as a sanity check, but ship it as the Linux/Windows path.
- Implement `rhi-d3d12/`. Same sample.
- The acid test: the *exact same* high-level sample code runs on all three backends with only a build-flag change.
- **Image-diff regression tests in CI.** Render the triangle and a small ImGui scene on each platform, hash or pixel-diff against a golden frame. This is the *only* way you'll know the abstraction holds. Without it you're flying blind across three drivers.
- **GPU-capture wiring.** RenderDoc programmatic API on Vulkan/D3D12, Metal `MTLCaptureManager` on macOS, both triggerable from a debug ImGui menu. Trivial to add now, painful at month 12.
- **Deliverable:** triangle on all three platforms via native APIs, with golden-frame CI passing and a "Capture next frame" debug menu that produces a RenderDoc / Metal capture.

> If at this point your backend is dragging, ship the engine with that platform on Vulkan-via-MoltenVK or whatever's already working, and circle back. Don't let a stuck backend block the rest of the plan.

### Phase 3 — Asset system + meshes & textures (3–4 weeks)

- `assets/` module: async loader on the job system, ref-counted handles, hot-reload via filesystem watch.
- Mesh import via cgltf (smaller and saner than assimp for glTF); add assimp later for FBX if needed.
- Texture import via stb_image for source, KTX/Basis for runtime format.
- Define your own runtime binary formats — load-time should be a memcpy plus a few pointer fixups, not a JSON parse.
- **Asset cooker is its own offline tool**, invoked by CMake or by file-watcher in dev. Cooked assets land in a content cache keyed by source-content hash + cooker-version.
- **Deliverable:** load and render a glTF model with PBR textures.

### Phase 4 — Renderer proper + frame graph + render passes (5–7 weeks)

- Material system, per-frame and per-object descriptor sets.
- **Frame graph.** Pass declaration, transient-resource allocation, automatic barrier insertion, dependency aliasing. This is the unlock that makes adding new effects easy later. Don't skip it.
- Forward-or-simple deferred pipeline — pick one, document why.
- Render graph: declarative pass description, transient buffer/texture aliasing.
- Camera, basic lighting, shadow maps (cascaded for directional).
- **Deliverable:** a small scene with a few meshes, dynamic lighting, and shadows, rendered through the frame graph.

### Phase 4.5 — Animation (4–6 weeks)

- Skeletal mesh import (glTF skins, joint hierarchy, inverse bind matrices).
- Vertex skinning: GPU compute or vertex-shader path.
- Animation clip playback, sampling, looping.
- Simple blend tree (1D and 2D blends), state machine.
- Root motion extraction.
- IK left as a stretch goal — two-bone IK is a week if you need it.
- **Deliverable:** an animated character walking around the scene with one blend tree.

### Phase 5 — ECS, scene, gameplay glue (3–4 weeks)

- EnTT-based scene module. Components: Transform, MeshRenderer, Camera, Light, RigidBody (forward declaration), AudioSource (forward declaration), AnimatedMesh.
- Scene serialization to a binary format + JSON debug format.
- ImGui-based scene inspector and entity tree.
- **Deliverable:** load a scene file, walk around with a flycam, edit transforms in ImGui.

### Phase 6 — Physics abstraction + Jolt (3–4 weeks)

- Define `IPhysicsWorld`, `IRigidBody`, `ICollisionShape`, raycast/sweep APIs in `physics/`.
- Implement `physics-jolt/`. Wire to scene via components.
- **Deliverable:** dynamic objects falling, character controller, raycasts work in 3D.

### Phase 7 — Audio abstraction + miniaudio (1–2 weeks)

- `IAudioEngine`, `ISound`, `IAudioSource`. 3D positional audio, basic mixer, listener.
- miniaudio backend (single-header, MIT, easy).
- **Deliverable:** spatialized footsteps, music, looping ambience.

### Phase 8 — Scripting + sample game (4–6 weeks)

- Lua via sol2 behind `IScriptHost`. Bind core engine: math, scene queries, input, audio, physics raycasts.
- Build the demo: a small playable thing — top-down 2D shooter or third-person 3D walking sim. Whichever you'll actually finish. Pick one.
- **Deliverable:** something you can hand to a friend and they can play it.

### Phase 9 — Validate the second backends (ongoing)

- Add `physics-physx` alongside Jolt. Add `audio-fmod` alongside miniaudio. Verify the demo game runs identically with either backend selected at build time.
- This is where you find out whether your abstractions actually leaked. They probably leak somewhere; fix the leaks now while there's only one game using them.

### Phase 10 — Editor & tooling (open-ended)

- Standalone editor app using the engine, built around ImGui + Tracy.
- Asset browser, scene editor, material editor, Tracy live capture window, in-engine RenderDoc trigger.
- The editor is what turns a framework into an engine you can actually ship a game on. Without it, you're hand-editing JSON scene files at month 18 and the project dies.

---

## 6. Directory layout

```
engine/
├── CMakeLists.txt
├── cmake/                # toolchain files, find modules
├── third_party/          # vendored or vcpkg manifest
├── docs/
│   ├── architecture.md
│   ├── rhi.md
│   ├── frame-graph.md
│   └── adr/              # architecture decision records
├── engine/
│   ├── core/
│   ├── platform/
│   ├── input/
│   ├── jobs/
│   ├── rhi/
│   ├── rhi-metal/
│   ├── rhi-d3d12/
│   ├── rhi-vulkan/
│   ├── renderer/
│   ├── animation/
│   ├── assets/
│   ├── audio/
│   ├── audio-miniaudio/
│   ├── audio-fmod/
│   ├── physics/
│   ├── physics-jolt/
│   ├── physics-physx/
│   ├── scene/
│   ├── scripting/
│   ├── scripting-lua/
│   ├── gameplay/
│   ├── imgui-integration/
│   └── gpu-capture/         # RenderDoc + MTLCaptureManager wrappers
├── tools/
│   ├── shader-compiler/     # HLSL → DXIL / SPIR-V / MSL via DXC + SPIRV-Cross, with reflection emit
│   ├── asset-cooker/
│   └── editor/
├── samples/
│   ├── 01_triangle/
│   ├── 02_textured_mesh/
│   ├── 03_pbr_scene/
│   ├── 04_animated_character/
│   └── 99_demo_game/
└── tests/
    ├── unit/             # math, containers, allocators, handle pools
    └── golden/           # image-diff renderer regression tests
```

Each `engine/<module>/` is a CMake static library target. Public headers go in `<module>/include/<engine_namespace>/<module>/`, sources and private headers in `<module>/src/`. This makes Include paths self-documenting (`#include <yourname/rhi/Device.h>`) and prevents accidental cross-module includes of private headers.

---

## 7. Things to decide explicitly (architecture decision records)

Write these down as you go, in `docs/adr/`. Future-you will thank present-you.

1. **Engine namespace and naming convention.**
2. **Coordinate system** (handedness, up axis, units).
3. **Memory model** — bump/linear allocators per frame? Pool allocators? When does the engine call `new`?
4. **String type** — `std::string` everywhere, or a custom interned `Name` for identifiers?
5. **Error handling** — exceptions, `std::expected`, error codes? Pick one and stick to it.
6. **RTTI / reflection** — needed for the editor and serialization. Custom macro-based reflection is the common choice; consider it from the start.
7. **Threading model** — main thread, render thread, job-system workers. *How does a system call into another thread?* (Now load-bearing — see Phase 1.)
8. **Asset GUID strategy** — content-hash, UUID, path-based?
9. **Bindless vs descriptor sets** — modern default is bindless. What's your fallback for older Vulkan drivers?
10. **Networking** — explicit decision, even if "no." If "no," document the assumptions you're allowed to bake (mutable globals, non-deterministic physics, scripting side effects). If "yes, eventually," call out what stays deterministic now to keep that door open.
11. **C++ hot-reload** — Live++, hot-reload-via-DLL, or none? Productivity multiplier on a multi-year project, but invasive to retrofit.
12. **GPU-debugging story** — RenderDoc capture trigger from ImGui? PIX markers? Metal frame capture? Wire one in by Phase 2.

These are the questions that bite at month six if undecided.

---

## 8. Honest expectations

Realistic timeline if this is a side project at evening pace:
**Phase 0–2 in 3 months, through Phase 6 in a year, animated character on screen at month 14, a playable sample game in 18 months.** Full-time, cut it in half. The "shippable to other developers" north star is a 2–3 year horizon — that's not pessimism, that's just what engines cost. Plan accordingly: every phase should produce something visible to show even if you stopped then.

The single biggest risk is rendering-backend yak-shaving. If you find yourself three months into the DX12 backend and not done, give yourself permission to ship Vulkan-via-MoltenVK on macOS for v1 and revisit native Metal once the rest of the engine exists. The engine isn't the renderer.

The second biggest risk is editor scope creep. The editor will dominate effort once you start. Treat Phase 10 as its own roadmap and timebox the first version.

---

## 9. What's deliberately out of scope (for v1)

Naming these explicitly so you don't accidentally drift into them:

- **Networking.** No replication, no rollback, no client-server. (See ADR §7.10.)
- **Streaming / open world.** Levels load synchronously between scenes.
- **Global illumination.** Direct lighting + shadow maps + IBL only.
- **Localization.** English only at the engine layer.
- **Console platforms.** PC and macOS only.
- **Mobile.** No iOS/Android.
- **Skeletal physics / ragdolls.** Stretch goal once both physics and animation work.

Each of these is a multi-month investment. Saying "no" up front is how you ship.
