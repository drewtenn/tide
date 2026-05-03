// UAV-texture canary (DEFINE D19). copilot-2 strongly recommended this — Phase
// 4.5 SPIRV-Cross had bugs on `access::read_write` MSL textures in 2024-04. We
// catch it now, not when frame-graph compute lands.

RWTexture2D<uint> img : register(u0);   // SPIR-V binding 32 (u-shift)

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    InterlockedMax(img[tid.xy], tid.x);
}
