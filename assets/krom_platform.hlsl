#ifndef KROM_PLATFORM_HLSL
#define KROM_PLATFORM_HLSL

// Vertex input locations must match VertexSemantic in renderer/RendererTypes.hpp.
#ifdef __spirv__
#define VK_LOC(n) [[vk::location(n)]]
// Buffer SRVs use BindingRegisterRanges::BufferSRVBase, separate from texture SRVs.
#define KROM_BUFFER_SRV(type, name, slot) [[vk::binding(64 + (slot))]] type name : register(t##slot)
#else
#define VK_LOC(n)
#define KROM_BUFFER_SRV(type, name, slot) type name : register(t##slot)
#endif

#endif
