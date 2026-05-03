// MSL native canary — mirrors triangle.ps.hlsl. Outputs the per-vertex color
// interpolated from the VS, with alpha forced to 1.

#include <metal_stdlib>
using namespace metal;

struct VsOut {
    float4 position [[position]];
    float3 color;
};

fragment float4 main0(VsOut in [[stage_in]]) {
    return float4(in.color, 1.0);
}
