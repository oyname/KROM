#include "platform/GLFWPlatform.hpp"
#include "platform/IThread.hpp"

using namespace engine::platform;

int main()
{
#if KROM_HAS_GLFW
    GLFWPlatform platform;
    if (!platform.Initialize())
        return -1;

    WindowDesc desc{};
    desc.title = "KROM Platform GLFW";
    desc.width = 1280;
    desc.height = 720;
    desc.resizable = true;

    IWindow* window = platform.CreateWindow(desc);
    if (!window)
        return -2;

    IInput* input = platform.GetInput();
    IThreadFactory* threads = platform.GetThreadFactory();
    IThread* loader = threads ? threads->CreateThread() : nullptr;
    if (loader)
    {
        loader->SetPriority(ThreadPriority::Low);
        loader->SetName("AssetLoader");
        loader->Start([]() {});
    }

    while (!window->ShouldClose())
    {
        platform.PumpEvents();
        if (input && input->IsKeyPressed(Key::Escape))
            window->RequestClose();
    }

    if (loader)
    {
        loader->Join();
        threads->DestroyThread(loader);
    }

    platform.Shutdown();
    return 0;
#else
    return 0;
#endif
}
