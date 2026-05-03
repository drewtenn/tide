// Phase 1 task 7 fragment shader. Samples the per-frame texture (t0/s0) at
// the interpolated UV.

Texture2D    g_tex    : register(t0);
SamplerState g_sampler : register(s0);

struct PsIn {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(PsIn input) : SV_Target0 {
    return g_tex.Sample(g_sampler, input.uv);
}
