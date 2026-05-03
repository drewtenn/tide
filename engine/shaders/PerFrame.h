#pragma once

// CPU mirror of the `PerFrame` cbuffer used by samples/03_textured_quad.
// HLSL/DX constant-buffer packing: a `float4x4` is 4×vec4 = 64 B; trailing
// floats pack tightly under DXC `-fvk-use-dx-layout`. Total = 80 B (16-aligned
// after the explicit 8 B pad).
//
// `rotation` is column-major to match the HLSL `column_major` keyword in
// textured_quad.vs.hlsl, so glm::value_ptr() (also column-major) writes the
// same bytes the shader reads.

#include <cstddef>
#include <cstdint>

namespace tide::shaders {

struct alignas(16) PerFrame {
    float    rotation[16];   // column-major
    float    time;
    float    aspect;
    float    pad[2];
};

static_assert(sizeof(PerFrame) == 80,             "PerFrame must be 80 bytes (DX cbuffer layout)");
static_assert(offsetof(PerFrame, rotation) ==  0, "rotation at offset 0");
static_assert(offsetof(PerFrame, time)     == 64, "time at offset 64");
static_assert(offsetof(PerFrame, aspect)   == 68, "aspect at offset 68");

} // namespace tide::shaders
