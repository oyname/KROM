#include "OpenGLWin32Loader.hpp"
#ifdef _WIN32
#include <mutex>

namespace engine::renderer::opengl {

#define DEF_PROC(ret, name, ...) name##_fn krom_##name = nullptr;
KROM_OGL_PROC_LIST(DEF_PROC)
#undef DEF_PROC

namespace {
void* LoadGLProc(const char* name)
{
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (!p || p == reinterpret_cast<void*>(0x1) || p == reinterpret_cast<void*>(0x2) ||
        p == reinterpret_cast<void*>(0x3) || p == reinterpret_cast<void*>(-1)) {
        static HMODULE module = LoadLibraryA("opengl32.dll");
        if (module)
            p = reinterpret_cast<void*>(GetProcAddress(module, name));
    }
    return p;
}

bool LoadAll()
{
#define LOAD_PROC(ret, name, ...) krom_##name = reinterpret_cast<name##_fn>(LoadGLProc(#name)); if (!krom_##name) return false;
    KROM_OGL_PROC_LIST(LOAD_PROC)
#undef LOAD_PROC
    return true;
}
}

bool EnsureWin32OpenGLFunctionsLoaded()
{
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [](){ ok = LoadAll(); });
    return ok;
}

}
#endif
