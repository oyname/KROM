// =============================================================================
// KROM Engine - OpenGLConverters.cpp
// Format/State-Mapping via Integer-Literale (kein GL-Header nötig).
// GL-Konstanten-Quellen: Khronos gl.h / glext.h
// =============================================================================
#include "OpenGLDevice.hpp"

namespace engine::renderer::opengl {

GLenum ToGLTopology(PrimitiveTopology t) noexcept {
    switch (t) {
    case PrimitiveTopology::TriangleStrip: return 0x0005u; // GL_TRIANGLE_STRIP
    case PrimitiveTopology::LineList:      return 0x0001u; // GL_LINES
    case PrimitiveTopology::LineStrip:     return 0x0003u; // GL_LINE_STRIP
    case PrimitiveTopology::PointList:     return 0x0000u; // GL_POINTS
    default:                               return 0x0004u; // GL_TRIANGLES
    }
}

GLenum ToGLBlendFactor(BlendFactor f) noexcept {
    switch (f) {
    case BlendFactor::Zero:        return 0u;      // GL_ZERO
    case BlendFactor::One:         return 1u;      // GL_ONE
    case BlendFactor::SrcColor:    return 0x0300u; // GL_SRC_COLOR
    case BlendFactor::InvSrcColor: return 0x0301u; // GL_ONE_MINUS_SRC_COLOR
    case BlendFactor::DstColor:    return 0x0306u; // GL_DST_COLOR
    case BlendFactor::InvDstColor: return 0x0307u; // GL_ONE_MINUS_DST_COLOR
    case BlendFactor::SrcAlpha:    return 0x0302u; // GL_SRC_ALPHA
    case BlendFactor::InvSrcAlpha: return 0x0303u; // GL_ONE_MINUS_SRC_ALPHA
    case BlendFactor::DstAlpha:    return 0x0304u; // GL_DST_ALPHA
    case BlendFactor::InvDstAlpha: return 0x0305u; // GL_ONE_MINUS_DST_ALPHA
    default:                       return 1u;
    }
}

GLenum ToGLBlendOp(BlendOp op) noexcept {
    switch (op) {
    case BlendOp::Subtract:    return 0x800Au; // GL_FUNC_SUBTRACT
    case BlendOp::RevSubtract: return 0x800Bu; // GL_FUNC_REVERSE_SUBTRACT
    case BlendOp::Min:         return 0x8007u; // GL_MIN
    case BlendOp::Max:         return 0x8008u; // GL_MAX
    default:                   return 0x8006u; // GL_FUNC_ADD
    }
}

GLenum ToGLCompareFunc(DepthFunc f) noexcept {
    switch (f) {
    case DepthFunc::Never:        return 0x0200u; // GL_NEVER
    case DepthFunc::Less:         return 0x0201u; // GL_LESS
    case DepthFunc::Equal:        return 0x0202u; // GL_EQUAL
    case DepthFunc::LessEqual:    return 0x0203u; // GL_LEQUAL
    case DepthFunc::Greater:      return 0x0204u; // GL_GREATER
    case DepthFunc::NotEqual:     return 0x0205u; // GL_NOTEQUAL
    case DepthFunc::GreaterEqual: return 0x0206u; // GL_GEQUAL
    default:                      return 0x0207u; // GL_ALWAYS
    }
}

GLenum ToGLBufferTarget(BufferType t) noexcept {
    switch (t) {
    case BufferType::Index:      return 0x8893u; // GL_ELEMENT_ARRAY_BUFFER
    case BufferType::Constant:   return 0x8A11u; // GL_UNIFORM_BUFFER
    case BufferType::Structured:
    case BufferType::Raw:
        // GL 4.1 Core hat keine SSBOs. Diese Upload-/Staging-Buffer werden im
        // aktuellen Renderer nicht als echte Structured/Raw-Views gelesen,
        // deshalb fallen sie auf einen generischen Buffer-Target zurück statt
        // ein ungültiges GL_SHADER_STORAGE_BUFFER zu erzeugen.
        return 0x8892u; // GL_ARRAY_BUFFER
    default:                     return 0x8892u; // GL_ARRAY_BUFFER
    }
}

GLenum ToGLBufferUsage(MemoryAccess a) noexcept {
    switch (a) {
    case MemoryAccess::CpuRead:  return 0x88E9u; // GL_STREAM_READ
    case MemoryAccess::CpuWrite: return 0x88E8u; // GL_DYNAMIC_DRAW
    default:                     return 0x88E4u; // GL_STATIC_DRAW
    }
}

GLenum ToGLInternalFormat(Format fmt) noexcept {
    switch (fmt) {
    case Format::R8_UNORM:             return 0x8229u; // GL_R8
    case Format::RG8_UNORM:            return 0x822Bu; // GL_RG8
    case Format::RGBA8_UNORM:          return 0x8058u; // GL_RGBA8
    case Format::RGBA8_UNORM_SRGB:     return 0x8C43u; // GL_SRGB8_ALPHA8
    case Format::BGRA8_UNORM:          return 0x8058u; // GL_RGBA8 (swizzle)
    case Format::R16_FLOAT:            return 0x822Du; // GL_R16F
    case Format::RG16_FLOAT:           return 0x822Fu; // GL_RG16F
    case Format::RGBA16_FLOAT:         return 0x881Au; // GL_RGBA16F
    case Format::R32_FLOAT:            return 0x822Eu; // GL_R32F
    case Format::RG32_FLOAT:           return 0x8230u; // GL_RG32F
    case Format::RGB32_FLOAT:          return 0x8815u; // GL_RGB32F
    case Format::RGBA32_FLOAT:         return 0x8814u; // GL_RGBA32F
    case Format::R32_UINT:             return 0x8236u; // GL_R32UI
    case Format::R11G11B10_FLOAT:      return 0x8C3Au; // GL_R11F_G11F_B10F
    case Format::BC5_UNORM:            return 0x8DBDu; // GL_COMPRESSED_RG_RGTC2
    case Format::D16_UNORM:            return 0x81A5u; // GL_DEPTH_COMPONENT16
    case Format::D24_UNORM_S8_UINT:    return 0x88F0u; // GL_DEPTH24_STENCIL8
    case Format::D32_FLOAT:            return 0x8CACu; // GL_DEPTH_COMPONENT32F
    case Format::D32_FLOAT_S8X24_UINT: return 0x8CADu; // GL_DEPTH32F_STENCIL8
    default:                           return 0x8058u; // GL_RGBA8
    }
}

GLenum ToGLBaseFormat(Format fmt) noexcept {
    switch (fmt) {
    case Format::R8_UNORM:
    case Format::R16_FLOAT:
    case Format::R32_FLOAT:
    case Format::R32_UINT:             return 0x1903u; // GL_RED
    case Format::RG8_UNORM:
    case Format::RG16_FLOAT:
    case Format::RG32_FLOAT:           return 0x8227u; // GL_RG
    case Format::RGB32_FLOAT:          return 0x1907u; // GL_RGB
    case Format::R11G11B10_FLOAT:      return 0x1907u; // GL_RGB
    case Format::D16_UNORM:
    case Format::D32_FLOAT:            return 0x1902u; // GL_DEPTH_COMPONENT
    case Format::D24_UNORM_S8_UINT:
    case Format::D32_FLOAT_S8X24_UINT: return 0x84F9u; // GL_DEPTH_STENCIL
    default:                           return 0x1908u; // GL_RGBA
    }
}

GLenum ToGLPixelType(Format fmt) noexcept {
    switch (fmt) {
    case Format::R16_FLOAT:
    case Format::RG16_FLOAT:
    case Format::RGBA16_FLOAT:         return 0x140Bu; // GL_HALF_FLOAT
    case Format::R32_FLOAT:
    case Format::RG32_FLOAT:
    case Format::RGB32_FLOAT:
    case Format::RGBA32_FLOAT:
    case Format::R11G11B10_FLOAT:
    case Format::D32_FLOAT:            return 0x1406u; // GL_FLOAT
    case Format::D24_UNORM_S8_UINT:    return 0x84FAu; // GL_UNSIGNED_INT_24_8
    case Format::D32_FLOAT_S8X24_UINT: return 0x8DADu; // GL_FLOAT_32_UNSIGNED_INT_24_8_REV
    case Format::R32_UINT:             return 0x1405u; // GL_UNSIGNED_INT
    default:                           return 0x1401u; // GL_UNSIGNED_BYTE
    }
}

// Vertex-Attribut-Typ für glVertexAttribPointer
GLenum ToGLAttribType(Format fmt) noexcept {
    switch (fmt) {
    case Format::R32_FLOAT:
    case Format::RG32_FLOAT:
    case Format::RGB32_FLOAT:
    case Format::RGBA32_FLOAT:   return 0x1406u; // GL_FLOAT
    case Format::R16_FLOAT:
    case Format::RG16_FLOAT:
    case Format::RGBA16_FLOAT:   return 0x140Bu; // GL_HALF_FLOAT
    case Format::R8_UNORM:
    case Format::RG8_UNORM:
    case Format::RGBA8_UNORM:    return 0x1401u; // GL_UNSIGNED_BYTE
    case Format::R32_UINT:       return 0x1405u; // GL_UNSIGNED_INT
    default:                     return 0x1406u; // GL_FLOAT
    }
}

GLint ToGLComponentCount(Format fmt) noexcept {
    switch (fmt) {
    case Format::R8_UNORM:
    case Format::R16_FLOAT:
    case Format::R32_FLOAT:
    case Format::R32_UINT:   return 1;
    case Format::RG8_UNORM:
    case Format::RG16_FLOAT:
    case Format::RG32_FLOAT: return 2;
    case Format::RGB32_FLOAT:
    case Format::R11G11B10_FLOAT: return 3;
    default:                 return 4;
    }
}

GLenum ToGLWrapMode(WrapMode m) noexcept {
    switch (m) {
    case WrapMode::Mirror: return 0x8370u; // GL_MIRRORED_REPEAT
    case WrapMode::Clamp:  return 0x812Fu; // GL_CLAMP_TO_EDGE
    case WrapMode::Border: return 0x812Du; // GL_CLAMP_TO_BORDER
    default:               return 0x2901u; // GL_REPEAT
    }
}

GLenum ToGLMinFilter(FilterMode min, FilterMode mip) noexcept {
    if (min == FilterMode::Nearest && mip == FilterMode::Nearest) return 0x2600u; // GL_NEAREST_MIPMAP_NEAREST
    if (min == FilterMode::Nearest && mip == FilterMode::Linear)  return 0x2702u; // GL_NEAREST_MIPMAP_LINEAR
    if (min == FilterMode::Linear  && mip == FilterMode::Nearest) return 0x2701u; // GL_LINEAR_MIPMAP_NEAREST
    return 0x2703u; // GL_LINEAR_MIPMAP_LINEAR
}

GLenum ToGLMagFilter(FilterMode mag) noexcept {
    return (mag == FilterMode::Nearest) ? 0x2600u : 0x2601u; // GL_NEAREST / GL_LINEAR
}

GLenum ToGLShaderType(ShaderStageMask stage) noexcept {
    if (stage == ShaderStageMask::Vertex)   return 0x8B31u; // GL_VERTEX_SHADER
    if (stage == ShaderStageMask::Fragment) return 0x8B30u; // GL_FRAGMENT_SHADER
    if (stage == ShaderStageMask::Compute)  return 0x91B9u; // GL_COMPUTE_SHADER
    return 0x8B31u;
}

GLenum ToGLFrontFace(WindingOrder order) noexcept
{
    return (order == WindingOrder::CW) ? 0x0900u : 0x0901u; // GL_CW / GL_CCW
}

} // namespace engine::renderer::opengl
