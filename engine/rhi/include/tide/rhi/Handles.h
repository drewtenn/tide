#pragma once

// RHI handle aliases over `tide::Handle<Tag>` (engine/core/include/tide/core/Handle.h).
// Per ADR-003: opaque integer handles (32-bit index + 32-bit generation), no virtual
// dispatch on resources. Tag types make handles non-interconvertible — passing a
// BufferHandle where TextureHandle is expected fails to compile.

#include <tide/core/Handle.h>

namespace tide::rhi {

struct BufferTag              {};
struct TextureTag             {};
struct TextureViewTag         {};
struct SamplerTag             {};
struct ShaderTag              {};
struct PipelineTag            {};
struct DescriptorSetLayoutTag {};
struct DescriptorSetTag       {};
struct FenceTag               {};

using BufferHandle              = tide::Handle<BufferTag>;
using TextureHandle             = tide::Handle<TextureTag>;
using TextureViewHandle         = tide::Handle<TextureViewTag>;
using SamplerHandle             = tide::Handle<SamplerTag>;
using ShaderHandle              = tide::Handle<ShaderTag>;
using PipelineHandle            = tide::Handle<PipelineTag>;
using DescriptorSetLayoutHandle = tide::Handle<DescriptorSetLayoutTag>;
using DescriptorSetHandle       = tide::Handle<DescriptorSetTag>;
using FenceHandle               = tide::Handle<FenceTag>;

} // namespace tide::rhi
