// MSL native canary — mirrors triangle.vs.hlsl. Hardcoded fullscreen-ish
// triangle sourced from vertex_id; no vertex inputs, no uniforms. Phase 1
// task 6 uses this directly while the HLSL→SPIR-V→MSL path is unblocked
// (DXC absence on this dev machine — see CompileShader.cmake comment).

#include <metal_stdlib>
using namespace metal;

struct VsOut {
    float4 position [[position]];
    float3 color;
};

vertex VsOut main0(uint vid [[vertex_id]]) {
    constexpr float2 kPositions[3] = {
        float2(-0.7,  0.7),
        float2( 0.7,  0.7),
        float2( 0.0, -0.7),
    };
    constexpr float3 kColors[3] = {
        float3(1.0, 0.0, 0.0),
        float3(0.0, 1.0, 0.0),
        float3(0.0, 0.0, 1.0),
    };

    VsOut o;
    o.position = float4(kPositions[vid], 0.0, 1.0);
    o.color    = kColors[vid];
    return o;
}
