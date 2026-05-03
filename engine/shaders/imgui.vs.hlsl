// Phase 1 task 8 — ImGui vertex shader. Inputs the engine's standard ImDrawVert
// (pos2 + uv2 + col4-as-RGBA8_Unorm). column_major + glm::value_ptr keeps the
// matrix bytes consistent end-to-end (see textured_quad.vs.hlsl for the same
// idiom). The PS multiplies the per-vertex color by the font atlas sample.

cbuffer ImGuiPerFrame : register(b0) {
    column_major float4x4 projection;   // ortho(0, w, h, 0) — Y-down screen space
};

struct VsIn {
    float2 pos   : POSITION;   // ImDrawVert.pos
    float2 uv    : TEXCOORD0;  // ImDrawVert.uv
    float4 color : COLOR0;     // ImDrawVert.col, RGBA8_Unorm → [0,1] float4
};

struct VsOut {
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VsOut main(VsIn input) {
    VsOut o;
    o.pos   = mul(projection, float4(input.pos, 0.0, 1.0));
    o.uv    = input.uv;
    o.color = input.color;
    return o;
}
