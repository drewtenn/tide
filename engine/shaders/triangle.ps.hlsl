// Trivial pixel canary. Outputs the interpolated per-vertex color directly.
// Pairs with triangle.vs.hlsl — interstage float4 (alpha already 1).

struct PsIn {
    float4 pos   : SV_Position;
    float4 color : COLOR0;
};

float4 main(PsIn i) : SV_Target0 {
    return i.color;
}
