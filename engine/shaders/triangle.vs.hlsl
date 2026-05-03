// Trivial vertex canary — emits a fullscreen triangle from a constant array.
// No vertex inputs, no uniforms; exercises the HLSL → SPIR-V → MSL toolchain
// without touching the descriptor-binding remap.
//
// Interstage data is float4, not float3. Some macOS Metal versions reject
// `[[user(locn0)]]` float3 with "Fragment input(s) ... mismatching vertex
// shader output type(s)" at PSO compile time; float4 always works. The
// alpha channel rides the same pipeline at no cost.

struct VsOut {
    float4 pos   : SV_Position;
    float4 color : COLOR0;
};

VsOut main(uint vid : SV_VertexID) {
    static const float2 kPositions[3] = {
        float2(-0.7,  0.7),
        float2( 0.7,  0.7),
        float2( 0.0, -0.7),
    };
    static const float4 kColors[3] = {
        float4(1.0, 0.0, 0.0, 1.0),
        float4(0.0, 1.0, 0.0, 1.0),
        float4(0.0, 0.0, 1.0, 1.0),
    };

    VsOut o;
    o.pos   = float4(kPositions[vid], 0.0, 1.0);
    o.color = kColors[vid];
    return o;
}
