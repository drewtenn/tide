// Compute canary — the Phase 1 tripwire shader (DEFINE D19). Exercises:
//   * RWStructuredBuffer atomic
//   * GroupMemoryBarrierWithGroupSync (threadgroup_barrier in MSL)
//   * compute thread-group declaration
// SPIRV-Cross MSL output for compute is a known historical pain point; if this
// shader fails to round-trip, halt Phase 1 and fix the toolchain rather than
// finding out at task 11 of the textured-quad sample.

RWStructuredBuffer<uint> buf : register(u0);   // SPIR-V binding 32 (u-shift)

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    InterlockedAdd(buf[tid.x], 1);
    GroupMemoryBarrierWithGroupSync();
    buf[tid.x] = buf[tid.x] * 2;
}
