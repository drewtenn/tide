// Phase 1 task 7 vertex shader. Promotes the colored triangle (sample 02) to
// a rotating textured quad: per-vertex position+UV, multiplied by a per-frame
// rotation matrix from cbuffer b0.
//
// `column_major` keeps DXC from transposing the matrix when emitting SPIR-V,
// so glm::value_ptr (column-major) on the CPU side is bitwise-equivalent to
// what the shader reads. Total cbuffer = 80 B (HLSL DX layout, see PerFrame.h).

cbuffer PerFrame : register(b0) {
    column_major float4x4 rotation;
    float                 time;
    float                 aspect;
    float2                _pad;
};

struct VsIn {
    float2 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut main(VsIn input) {
    // Subtle 5 % breathing pulse driven by `time`. Apart from being mildly
    // alive, this keeps the cbuffer's `time` field on the consumed-by-shader
    // side of the contract — a stale or zeroed `time` upload would freeze the
    // pulse and surface immediately, without needing a profiler.
    float pulse = 1.0 + 0.05 * sin(time);
    float4 local = float4(input.position * pulse, 0.0, 1.0);
    float4 world = mul(rotation, local);
    // Aspect-correct so the square stays square regardless of window shape.
    world.x /= max(aspect, 1e-6);

    VsOut o;
    o.pos = world;
    o.uv  = input.uv;
    return o;
}
