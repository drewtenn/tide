# ADR-0004: Shader pipeline — HLSL → SPIR-V (DXC) → MSL (SPIRV-Cross), CMake-driven

**Status:** Accepted
**Date:** 2026-05-03
**Phase:** P1 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

The engine targets three backends (Metal, Vulkan, DX12 in respective phases) with a single shader source. The choices for shader source language are:

1. **HLSL** authored, compiled to DXIL for DX12 + SPIR-V for Vulkan + MSL for Metal.
2. **GLSL** authored, compiled to SPIR-V then to MSL/DXIL.
3. **MSL/HLSL/GLSL** authored separately per backend (rejected on sight: 3× shader maintenance).
4. **Slang** (the Khronos-hosted HLSL successor with multi-target backends).

The toolchain choices for HLSL→cross-target:

- **DXC** (DirectX Shader Compiler, the Microsoft compiler that emits SPIR-V via `-spirv`).
- **SPIRV-Cross** (Khronos cross-compiler from SPIR-V to MSL / GLSL).
- **MoltenVK** (Vulkan-on-Metal — implies authoring Vulkan-native shaders, not HLSL).

Phase 1 only ships the Metal backend, but the tool chain must be the production path the engine uses for the Vulkan port in Phase 2. Picking a Metal-only path now (e.g. authoring MSL directly) means redoing the pipeline at the Phase 2 boundary.

## Decision

**HLSL is the shader source of truth. DXC compiles it to SPIR-V. SPIRV-Cross translates SPIR-V to MSL on Apple. CMake drives the whole pipeline via `tide_compile_shader()` in `cmake/CompileShader.cmake`.**

```
foo.cs.hlsl
  └── DXC -spirv -fvk-use-dx-layout -fvk-{b,t,u,s}-shift -fspv-target-env=vulkan1.3
  └── foo.cs.hlsl.spv
        ├── SPIRV-Cross --msl --msl-version 30000  →  foo.cs.hlsl.metal
        │       └── xcrun metal -c                  →  foo.cs.hlsl.air
        │             └── xcrun metallib            →  foo.cs.hlsl.metallib
        └── SPIRV-Cross --reflect                   →  foo.cs.hlsl.refl.json
```

Output paths use a `.hlsl.` infix so HLSL-derived artefacts cannot collide with hand-written `.metal` outputs from `tide_compile_metal_source()`. (Earlier Phase 1 task 10 surfaced the collision; see the cleanup commit.)

DXC discovery is two-stage per locked DEFINE D21:
1. `find_program(dxc HINTS $ENV{VULKAN_SDK}/macOS/bin /opt/homebrew/bin ...)`.
2. `FetchContent_Declare(tide_dxc URL ...)` for Linux/Windows when system `dxc` is missing. macOS has no FetchContent fallback (Microsoft doesn't publish reliable macOS DXC binaries; users install the Vulkan SDK).

DXC variant compatibility (`TIDE_DXC_ENTRYPOINT_STYLE`, configure-time detection): Microsoft DXC accepts `-fvk-use-entrypoint-name` (a switch). The LunarG-bundled `dxc-3.7` accepts `-fspv-entrypoint-name=<value>`. CMake probes `dxc --help` once and dispatches to whichever flag works.

## Alternatives considered

- **Slang.** Rejected for Phase 1: the Slang ecosystem is moving fast (good!) but its DX12/Metal backends were not yet stable enough to bet the entire engine on in late 2025. Reserved as a Phase 4+ revisit if SPIRV-Cross's Metal output becomes a perf or compat bottleneck.
- **GLSL source.** Rejected: GLSL→HLSL semantic differences (binding, descriptor set syntax, matrix conventions) are heavier than SPIR-V→MSL. HLSL is also closer to the Visual Studio / RenderDoc tooling our DX12 backend will eventually use.
- **Author MSL natively for Metal.** Used as a fallback for the early triangle samples (`triangle.{vs,ps}.metal`) so those samples could ship before DXC was installed. Production path is HLSL; the hand-written MSL files are documented as deviations and don't get net-new authoring.
- **MoltenVK as the Metal driver.** Rejected: adds a translation layer the engine cannot debug (Vulkan validation errors from inside MoltenVK hide Metal-side bugs). When we add Vulkan in Phase 2 it'll target Vulkan natively; Metal stays direct.

## Consequences

**Positive.**
- Single shader source per visual effect. Adding a backend in Phase 2 doesn't require rewriting shaders — DXC + SPIRV-Cross handles it.
- HLSL is the most-used shading language in the games industry (DX12-shipped titles, every Unreal project) — the talent pool, references, and tooling (RenderDoc, PIX, Aftermath) all favour it.
- DXC's `-fvk-use-dx-layout` keeps Vulkan/SPIR-V cbuffer packing identical to HLSL's, avoiding `std140`-vs-HLSL-packing surprises (see ADR-0005's pad guards).
- Reflection JSON (`SPIRV-Cross --reflect`) gives us per-shader binding metadata we can consume at build time. (Phase 1 doesn't yet generate code from it; the data is captured for Phase 3+.)
- Compile is hermetic: every shader artefact lives in `${CMAKE_BINARY_DIR}/shaders/` with deterministic names, dependencies tracked by CMake.

**Negative / accepted costs.**
- DXC version drift: the `-fvk-use-entrypoint-name` vs `-fspv-entrypoint-name=<v>` divergence between Microsoft DXC and the LunarG variant cost half a session to diagnose. Mitigated by configure-time detection + a tracking constant; new flags need similar treatment.
- SPIRV-Cross MSL output has known quirks: `float3` interstage attributes are rejected by some Metal versions ("Fragment input(s) `user(locn0)` mismatching vertex shader output type") even though the type IS `float3`. Workaround documented in ADR-0006 / `triangle.vs.hlsl` — interstage data is `float4` per convention.
- Two-step pipeline (HLSL→SPV→MSL) means a shader bug can be in any of three places: the HLSL, the DXC emit, or the SPIRV-Cross transform. The reflection JSON + the emitted `.metal` file in the build dir are the diagnostic windows; both are kept.
- Build-graph collision risk: any rule that emits `<basename>.<stage>.metallib` collides with another. Phase 1's fix is the `.hlsl.` infix; future per-backend pipelines (Vulkan AIR-equivalents, DXIL) MUST adopt similar suffixes to avoid the same trap.

**Reversibility.** 1–2 weeks. Switching to Slang is "rewrite `cmake/CompileShader.cmake` + retest every shader's slot map". Switching to per-backend authoring is multi-month — the entire shader corpus would need triplicated.

## Forward-design hooks

- **All HLSL shaders use `-fvk-use-dx-layout` and the same `b/t/u/s` shifts.** Changing those flags is a global breaking change; pin them in `CompileShader.cmake` as DEFINE constants.
- **Interstage data is `float4`, not `float3`.** Documented in `triangle.vs.hlsl`. The float3 quirk burns a Metal version every couple of years; just don't get into the situation.
- **HLSL → MSL artefacts use the `.hlsl.` infix.** Hand-written MSL outputs use plain names. Vulkan SPIR-V (when Phase 2 adds it) will reuse the existing `.hlsl.spv`. DX12 DXIL (Phase 2.5) gets `.hlsl.dxil`.
- **`tide_compile_shader` is a no-op when `TIDE_SHADER_TOOLCHAIN_AVAILABLE` is false.** This lets the engine still build (without working samples) when DXC is unavailable on a fresh-clone Linux box. Per-backend tests must Skip cleanly when toolchains are missing.
- **Reflection JSON is captured but not yet consumed.** When Phase 3 adds the asset cooker, the cooker reads the JSON to validate descriptor set layouts at build time; gameplay-side bindings get type-checked against the shader's actual interface.

## Related ADRs

- ADR-0003: RHI handle strategy — opaque integer handles consume the shader artefacts.
- ADR-0005: Memory model — cbuffer layout (which DXC packs) is the most consequential shader-side decision.
- ADR-0006: Bindless vs descriptor sets — the binding shifts here interact with the descriptor model.
- ADR-0008: Package manager — vcpkg ports `directx-dxc` (Linux/Windows) and `spirv-cross` (all).
