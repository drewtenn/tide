// Phase 1 task 8 — ImGui pixel shader. Samples the font atlas (g_tex/g_sampler)
// at the interpolated UV and multiplies by the per-vertex color. No gamma
// correction — Phase 1 accepts the slight sRGB-vs-linear mismatch on the
// debug overlay (see debate-task8-gate1.md §4 for the rationale).

Texture2D    g_tex     : register(t0);
SamplerState g_sampler : register(s0);

struct PsIn {
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

float4 main(PsIn input) : SV_Target0 {
    return input.color * g_tex.Sample(g_sampler, input.uv);
}
