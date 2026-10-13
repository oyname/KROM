#include "krom.h"
#include "core/Debug.hpp"

int main()
{
    engine::Debug::ResetMinLevelForBuild();

    if (!Krom::Graphics(Krom::Renderer::OpenGL, 800, 600, "Minimal Window"))
        return 1;

    return Krom::Run([](float dt)
    {
        if (Krom::KeyHit(Krom::Key::Escape)) { Krom::Quit(); return; }

        if (Krom::KeyHit(Krom::Key::Space))
            engine::Debug::Log("Space");

        if (Krom::KeyDown(Krom::Key::W)) engine::Debug::Log("W gehalten  dt=%.4f", dt);
        if (Krom::KeyDown(Krom::Key::S)) engine::Debug::Log("S gehalten  dt=%.4f", dt);
    });
}
