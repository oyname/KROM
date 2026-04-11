// =============================================================================
// KROM Engine - OpenGLSwapchain.cpp
// Win32 nutzt einen nativen WGL-Kontext auf HWND/HDC.
// Linux/macOS nutzen weiterhin den plattformseitigen GLFW-Kontext, solange der
// native Linux-Pfad noch nicht implementiert ist.
// Fence via glFenceSync / glClientWaitSync (GL 3.2+).
// =============================================================================
#include "OpenGLDevice.hpp"
#include "core/Debug.hpp"
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif

#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif


#ifdef KROM_OPENGL_BACKEND
#   if defined(_WIN32)
#       ifndef WIN32_LEAN_AND_MEAN
#           define WIN32_LEAN_AND_MEAN
#       endif
#       ifndef NOMINMAX
#           define NOMINMAX
#       endif
#       include <windows.h>
#   endif
#   if defined(_WIN32)
#       include "OpenGLWin32Loader.hpp"
#   endif
#   if defined(__APPLE__)
#       include <OpenGL/gl3.h>
#   else
#       include <GL/gl.h>
#       include "glext.h"
#   endif
#   if defined(KROM_HAS_GLFW) && KROM_HAS_GLFW
#       define GLFW_INCLUDE_NONE
#       include <GLFW/glfw3.h>
#   endif
#endif

namespace engine::renderer::opengl {

namespace {
#if defined(_WIN32) && defined(KROM_OPENGL_BACKEND)
void QueryWin32ClientSize(HWND hwnd, uint32_t& outWidth, uint32_t& outHeight)
{
    if (!hwnd)
        return;
    RECT rect{};
    if (!GetClientRect(hwnd, &rect))
        return;
    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    if (width > 0 && height > 0)
    {
        outWidth = static_cast<uint32_t>(width);
        outHeight = static_cast<uint32_t>(height);
    }
}
#endif
}

namespace {
#if defined(_WIN32) && defined(KROM_OPENGL_BACKEND)
bool SetPixelFormatFromIndex(HDC dc, int pixelFormat)
{
    PIXELFORMATDESCRIPTOR pfd{};
    if (DescribePixelFormat(dc, pixelFormat, sizeof(pfd), &pfd) == 0)
        return false;
    return SetPixelFormat(dc, pixelFormat, &pfd) == TRUE;
}
#endif
}

// =============================================================================
// OpenGLSwapchain
// =============================================================================

OpenGLSwapchain::OpenGLSwapchain(void* nativeWindowHandle, OGLDeviceResources& res,
                                  uint32_t width, uint32_t height,
                                  int openglMajor, int openglMinor, bool openglDebugContext)
    : m_nativeWindowHandle(nativeWindowHandle), m_res(&res)
    , m_width(width), m_height(height)
    , m_openglMajor(openglMajor), m_openglMinor(openglMinor)
    , m_openglDebugContext(openglDebugContext)
{
#ifdef KROM_OPENGL_BACKEND
    if (!m_nativeWindowHandle) {
        Debug::LogError("OpenGLSwapchain: nativeWindowHandle ist null");
        return;
    }

    if (!InitializeNativeContext()) {
        Debug::LogError("OpenGLSwapchain: native OpenGL context initialization failed");
        return;
    }

    m_glLoaded = true;
#   if defined(_WIN32)
    if (!EnsureWin32OpenGLFunctionsLoaded()) {
        Debug::LogError("OpenGLSwapchain: WGL function loading failed");
        m_glLoaded = false;
        DestroyNativeContext();
        return;
    }
#   endif

    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* vnd = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    Debug::Log("OpenGLSwapchain: GL %s - %s", ver ? ver : "?", vnd ? vnd : "?");

    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDisable(0x0C11u); // GL_SCISSOR_TEST

    // Pseudo-RenderTargetHandle für den Backbuffer (FBO=0)
    OGLRenderTargetEntry bb;
    bb.fbo          = 0u;
    bb.isBackbuffer = true;
    bb.width        = width;
    bb.height       = height;
    m_bbRT = m_res->renderTargets.Add(std::move(bb));
#endif
}

OpenGLSwapchain::~OpenGLSwapchain()
{
    if (m_bbRT.IsValid())
        m_res->renderTargets.Remove(m_bbRT);
    DestroyNativeContext();
}

bool OpenGLSwapchain::AcquireForFrame()
{
    SyncWindowSizeFromNative();
    return CanRenderFrame();
}

SwapchainFrameStatus OpenGLSwapchain::QueryFrameStatus() const
{
    SwapchainFrameStatus status{};
    status.phase = CanRenderFrame() ? SwapchainFramePhase::Acquired : SwapchainFramePhase::Uninitialized;
    status.currentBackbufferIndex = 0u;
    status.bufferCount = 1u;
    status.hasRenderableBackbuffer = CanRenderFrame();
    return status;
}

SwapchainRuntimeDesc OpenGLSwapchain::GetRuntimeDesc() const
{
    SwapchainRuntimeDesc desc{};
    desc.presentQueue = QueueType::Graphics;
    desc.explicitAcquire = false;
    desc.explicitPresentTransition = false;
    desc.tracksPerBufferOwnership = false;
    desc.resizeRequiresRecreate = false;
    desc.destructionRequiresFenceRetirement = false;
    return desc;
}

void OpenGLSwapchain::Present(bool vsync)
{
#ifdef KROM_OPENGL_BACKEND
    if (!m_nativeWindowHandle || !m_glLoaded) return;
    SyncWindowSizeFromNative();
#   if defined(_WIN32)
    if (m_win32DeviceContext) {
        using SwapIntervalFn = BOOL (WINAPI*)(int);
        static SwapIntervalFn swapInterval = reinterpret_cast<SwapIntervalFn>(wglGetProcAddress("wglSwapIntervalEXT"));
        if (swapInterval)
            swapInterval(vsync ? 1 : 0);
        SwapBuffers(static_cast<HDC>(m_win32DeviceContext));
    }
#   elif defined(KROM_HAS_GLFW) && KROM_HAS_GLFW
    glfwSwapInterval(vsync ? 1 : 0);
    glfwSwapBuffers(static_cast<GLFWwindow*>(m_nativeWindowHandle));
#   else
    (void)vsync;
#   endif
#else
    (void)vsync;
#endif
}

void OpenGLSwapchain::Resize(uint32_t w, uint32_t h)
{
    m_width  = w;
    m_height = h;
    if (auto* e = m_res->renderTargets.Get(m_bbRT)) {
        e->width  = w;
        e->height = h;
    }
#ifdef KROM_OPENGL_BACKEND
    glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
#endif
}


void OpenGLSwapchain::SyncWindowSizeFromNative() const
{
#if defined(_WIN32) && defined(KROM_OPENGL_BACKEND)
    auto* self = const_cast<OpenGLSwapchain*>(this);
    uint32_t width = self->m_width;
    uint32_t height = self->m_height;
    QueryWin32ClientSize(static_cast<HWND>(self->m_nativeWindowHandle), width, height);
    if (width == 0u || height == 0u)
        return;
    if (width == self->m_width && height == self->m_height)
        return;

    self->m_width = width;
    self->m_height = height;
    if (auto* e = self->m_res->renderTargets.Get(self->m_bbRT))
    {
        e->width = width;
        e->height = height;
    }
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
#endif
}

uint32_t OpenGLSwapchain::GetWidth() const
{
    SyncWindowSizeFromNative();
    return m_width;
}

uint32_t OpenGLSwapchain::GetHeight() const
{
    SyncWindowSizeFromNative();
    return m_height;
}

bool OpenGLSwapchain::InitializeNativeContext()
{
#ifdef KROM_OPENGL_BACKEND
#   if defined(_WIN32)
    // -------------------------------------------------------------------------
    // Win32 erfordert einen zweistufigen Bootstrap, weil wglCreateContextAttribsARB
    // erst über wglGetProcAddress geladen werden kann - was wiederum einen aktiven
    // (Legacy-)Kontext voraussetzt.  SetPixelFormat darf pro HDC nur einmal
    // aufgerufen werden, deshalb läuft der Legacy-Kontext auf einem versteckten
    // Dummy-Fenster, das danach sofort zerstört wird.
    // -------------------------------------------------------------------------

    using PFNWGLCREATECONTEXTATTRIBSARBPROC =
        HGLRC (WINAPI*)(HDC, HGLRC, const int*);
    using PFNWGLCHOOSEPIXELFORMATARBPROC =
        BOOL  (WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);

    auto* hwnd = static_cast<HWND>(m_nativeWindowHandle);
    if (!hwnd)
        return false;

    // --- Schritt 1: Dummy-Fenster für den Legacy-Bootstrap -------------------
    HWND dummyHwnd = CreateWindowExW(
        0, L"KromEngineWin32Window", L"",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dummyHwnd) {
        Debug::LogError("OpenGLSwapchain: Dummy-Fenster konnte nicht erstellt werden");
        return false;
    }

    HDC dummyDC = GetDC(dummyHwnd);

    PIXELFORMATDESCRIPTOR dummyPfd{};
    dummyPfd.nSize        = sizeof(dummyPfd);
    dummyPfd.nVersion     = 1;
    dummyPfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    dummyPfd.iPixelType   = PFD_TYPE_RGBA;
    dummyPfd.cColorBits   = 32;
    dummyPfd.cDepthBits   = 24;
    dummyPfd.cStencilBits = 8;
    dummyPfd.iLayerType   = PFD_MAIN_PLANE;

    const int dummyFmt = ChoosePixelFormat(dummyDC, &dummyPfd);
    SetPixelFormat(dummyDC, dummyFmt, &dummyPfd);

    HGLRC dummyRC = wglCreateContext(dummyDC);
    if (!dummyRC || !wglMakeCurrent(dummyDC, dummyRC)) {
        if (dummyRC) wglDeleteContext(dummyRC);
        ReleaseDC(dummyHwnd, dummyDC);
        DestroyWindow(dummyHwnd);
        Debug::LogError("OpenGLSwapchain: Legacy-Bootstrap-Kontext fehlgeschlagen");
        return false;
    }

    // --- Schritt 2: ARB-Extensions laden -------------------------------------
    auto wglCreateContextAttribsARB =
        reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
            wglGetProcAddress("wglCreateContextAttribsARB"));
    auto wglChoosePixelFormatARB =
        reinterpret_cast<PFNWGLCHOOSEPIXELFORMATARBPROC>(
            wglGetProcAddress("wglChoosePixelFormatARB"));

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(dummyRC);
    ReleaseDC(dummyHwnd, dummyDC);
    DestroyWindow(dummyHwnd);

    if (!wglCreateContextAttribsARB) {
        Debug::LogError("OpenGLSwapchain: wglCreateContextAttribsARB nicht verfügbar - "
                        "OpenGL 3.2+ Core Profile wird benötigt");
        return false;
    }

    // --- Schritt 3: Pixel-Format für das echte Fenster -----------------------
    HDC dc = GetDC(hwnd);
    if (!dc)
        return false;

    int pixelFormat = 0;

    if (wglChoosePixelFormatARB) {
        // Bevorzugt: sRGB-fähiges Format via ARB
        const int pfAttribs[] = {
            0x2001 /*WGL_DRAW_TO_WINDOW_ARB*/, GL_TRUE,
            0x2010 /*WGL_SUPPORT_OPENGL_ARB*/,  GL_TRUE,
            0x2011 /*WGL_DOUBLE_BUFFER_ARB*/,   GL_TRUE,
            0x2013 /*WGL_PIXEL_TYPE_ARB*/,      0x202B /*WGL_TYPE_RGBA_ARB*/,
            0x2014 /*WGL_COLOR_BITS_ARB*/,       32,
            0x2022 /*WGL_DEPTH_BITS_ARB*/,       24,
            0x2023 /*WGL_STENCIL_BITS_ARB*/,      8,
            0x20A9 /*WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB*/, GL_TRUE,
            0
        };
        UINT numFormats = 0u;
        if (!wglChoosePixelFormatARB(dc, pfAttribs, nullptr, 1, &pixelFormat, &numFormats)
                || numFormats == 0u)
            pixelFormat = 0; // Fallback auf Legacy-Pfad
    }

    if (pixelFormat == 0) {
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize        = sizeof(pfd);
        pfd.nVersion     = 1;
        pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType   = PFD_TYPE_RGBA;
        pfd.cColorBits   = 32;
        pfd.cDepthBits   = 24;
        pfd.cStencilBits = 8;
        pfd.iLayerType   = PFD_MAIN_PLANE;
        pixelFormat = ChoosePixelFormat(dc, &pfd);
    }

    if (pixelFormat == 0) {
        ReleaseDC(hwnd, dc);
        Debug::LogError("OpenGLSwapchain: Kein passendes Pixel-Format gefunden");
        return false;
    }

    if (!SetPixelFormatFromIndex(dc, pixelFormat)) {
        ReleaseDC(hwnd, dc);
        Debug::LogError("OpenGLSwapchain: SetPixelFormat fehlgeschlagen");
        return false;
    }

    // --- Schritt 4: Core Profile Context erstellen ---------------------------
    int contextFlags = 0;
    if (m_openglDebugContext)
        contextFlags |= 0x00000002; // WGL_CONTEXT_DEBUG_BIT_ARB

    const int ctxAttribs[] = {
        0x2091 /*WGL_CONTEXT_MAJOR_VERSION_ARB*/, m_openglMajor,
        0x2092 /*WGL_CONTEXT_MINOR_VERSION_ARB*/, m_openglMinor,
        0x9126 /*WGL_CONTEXT_PROFILE_MASK_ARB*/,  0x00000001 /*WGL_CONTEXT_CORE_PROFILE_BIT_ARB*/,
        0x2094 /*WGL_CONTEXT_FLAGS_ARB*/,          contextFlags,
        0
    };

    HGLRC rc = wglCreateContextAttribsARB(dc, nullptr, ctxAttribs);
    if (!rc) {
        ReleaseDC(hwnd, dc);
        Debug::LogError("OpenGLSwapchain: wglCreateContextAttribsARB(%d.%d Core) fehlgeschlagen",
                        m_openglMajor, m_openglMinor);
        return false;
    }

    if (!wglMakeCurrent(dc, rc)) {
        wglDeleteContext(rc);
        ReleaseDC(hwnd, dc);
        Debug::LogError("OpenGLSwapchain: wglMakeCurrent fehlgeschlagen");
        return false;
    }

    m_win32DeviceContext = dc;
    m_win32GlContext     = rc;

    glEnable(0x8DB9u); // GL_FRAMEBUFFER_SRGB

    return true;
#   elif defined(KROM_HAS_GLFW) && KROM_HAS_GLFW
    glfwMakeContextCurrent(static_cast<GLFWwindow*>(m_nativeWindowHandle));
    glfwSwapInterval(1);
    return true;
#   else
    return false;
#   endif
#else
    return false;
#endif
}

void OpenGLSwapchain::DestroyNativeContext()
{
#ifdef KROM_OPENGL_BACKEND
#   if defined(_WIN32)
    auto* dc = static_cast<HDC>(m_win32DeviceContext);
    auto* rc = static_cast<HGLRC>(m_win32GlContext);
    auto* hwnd = static_cast<HWND>(m_nativeWindowHandle);
    if (wglGetCurrentContext() == rc)
        wglMakeCurrent(nullptr, nullptr);
    if (rc)
        wglDeleteContext(rc);
    if (dc && hwnd)
        ReleaseDC(hwnd, dc);
    m_win32GlContext = nullptr;
    m_win32DeviceContext = nullptr;
#   elif defined(KROM_HAS_GLFW) && KROM_HAS_GLFW
    if (m_nativeWindowHandle && glfwGetCurrentContext() == static_cast<GLFWwindow*>(m_nativeWindowHandle))
        glfwMakeContextCurrent(nullptr);
#   endif
#endif
}

TextureHandle OpenGLSwapchain::GetBackbufferTexture(uint32_t) const
{
    return TextureHandle::Invalid();
}

RenderTargetHandle OpenGLSwapchain::GetBackbufferRenderTarget(uint32_t) const
{
    return m_bbRT;
}

// =============================================================================
// OpenGLFence - glFenceSync / glClientWaitSync (GL 3.2+)
// =============================================================================

OpenGLFence::~OpenGLFence()
{
#ifdef KROM_OPENGL_BACKEND
    if (m_sync)
        glDeleteSync(static_cast<GLsync>(m_sync));
#endif
}

void OpenGLFence::Signal(uint64_t value)
{
#ifdef KROM_OPENGL_BACKEND
    if (m_sync) glDeleteSync(static_cast<GLsync>(m_sync));
    m_sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0u);
#endif
    m_value.store(value);
}

void OpenGLFence::Wait(uint64_t value, uint64_t timeoutNs)
{
#ifdef KROM_OPENGL_BACKEND
    if (!m_sync || m_value.load() < value) return;
    glClientWaitSync(static_cast<GLsync>(m_sync),
                     GL_SYNC_FLUSH_COMMANDS_BIT,
                     static_cast<GLuint64>(timeoutNs));
    glDeleteSync(static_cast<GLsync>(m_sync));
    m_sync = nullptr;
#else
    (void)value; (void)timeoutNs;
#endif
}

} // namespace engine::renderer::opengl
