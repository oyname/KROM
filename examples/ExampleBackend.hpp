#pragma once
#include "krom.h"

// Every example is built once per backend (<name>_dx11, _vulkan, _opengl) and
// the target sets KROM_APP_BACKEND_*. Pick the renderer from that define so the
// executable really runs on the backend its name says.
static int ExampleRenderer() noexcept
{
#if defined(KROM_APP_BACKEND_DX11)
    return Krom::Renderer::DX11;
#elif defined(KROM_APP_BACKEND_OPENGL)
    return Krom::Renderer::OpenGL;
#elif defined(KROM_APP_BACKEND_VULKAN)
    return Krom::Renderer::Vulkan;
#else
    return Krom::DefaultRenderer();
#endif
}
