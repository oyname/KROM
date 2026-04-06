#pragma once
// =============================================================================
// KROM Engine - platform/PlatformInput.hpp
//
// StandardInput  - konkrete Implementierung mit internem State-Array.
//                  Plattform-Adapter (Win32, SDL, ...) rufen OnKey/Mouse* auf.
//
// NullInput      - alles false/0, für Tests und Headless.
// =============================================================================
#include "platform/IInput.hpp"
#include <array>

namespace engine::platform {

// =============================================================================
// StandardInput - implementiert IInput mit internem State
// =============================================================================
class StandardInput : public IInput
{
public:
    void BeginFrame() override
    {
        m_hit.fill(false);
        m_released.fill(false);
        m_mouseHit.fill(false);
        m_mouseReleased.fill(false);
        m_dx = m_dy = 0;
        m_scroll = 0.f;
    }

    void OnKeyEvent(const InputKeyEvent& e) override
    {
        const auto i = idx(e.key);
        if (i >= KEY_N) return;
        if (e.pressed) { if (!m_down[i] && !e.repeat) m_hit[i] = true; m_down[i] = true; }
        else           { m_down[i] = false; m_released[i] = true; }
    }

    void OnMouseButtonEvent(const InputMouseButtonEvent& e) override
    {
        const auto i = midx(e.button);
        if (i >= MB_N) return;
        if (e.pressed) { if (!m_mbDown[i]) m_mouseHit[i] = true; m_mbDown[i] = true; }
        else           { m_mbDown[i] = false; m_mouseReleased[i] = true; }
        m_mx = e.x; m_my = e.y;
    }

    void OnMouseMoveEvent(const InputMouseMoveEvent& e) override
    { m_dx += e.deltaX; m_dy += e.deltaY; m_mx = e.x; m_my = e.y; }

    void OnMouseScrollEvent(const InputMouseScrollEvent& e) override
    { m_scroll += e.delta; }

    bool KeyDown    (Key k) const override { auto i=idx(k); return i<KEY_N && m_down[i]; }
    bool KeyHit     (Key k) const override { auto i=idx(k); return i<KEY_N && m_hit[i];  }
    bool KeyReleased(Key k) const override { auto i=idx(k); return i<KEY_N && m_released[i]; }

    bool    MouseButtonDown    (MouseButton b) const override { auto i=midx(b); return i<MB_N && m_mbDown[i]; }
    bool    MouseButtonHit     (MouseButton b) const override { auto i=midx(b); return i<MB_N && m_mouseHit[i]; }
    bool    MouseButtonReleased(MouseButton b) const override { auto i=midx(b); return i<MB_N && m_mouseReleased[i]; }
    int32_t MouseX()           const override { return m_mx; }
    int32_t MouseY()           const override { return m_my; }
    int32_t MouseDeltaX()      const override { return m_dx; }
    int32_t MouseDeltaY()      const override { return m_dy; }
    float   MouseScrollDelta() const override { return m_scroll; }

private:
    static constexpr size_t KEY_N = static_cast<size_t>(Key::Count);
    static constexpr size_t MB_N  = static_cast<size_t>(MouseButton::Count);

    static size_t idx (Key k)         noexcept { return static_cast<size_t>(k); }
    static size_t midx(MouseButton b) noexcept { return static_cast<size_t>(b); }

    std::array<bool, KEY_N> m_down{};
    std::array<bool, KEY_N> m_hit{};
    std::array<bool, KEY_N> m_released{};

    std::array<bool, MB_N> m_mbDown{};
    std::array<bool, MB_N> m_mouseHit{};
    std::array<bool, MB_N> m_mouseReleased{};

    int32_t m_mx = 0, m_my = 0;
    int32_t m_dx = 0, m_dy = 0;
    float   m_scroll = 0.f;
};

// =============================================================================
// NullInput - für Tests und Headless-Modus
// =============================================================================
class NullInput final : public IInput
{
public:
    void BeginFrame() override {}
    void OnKeyEvent        (const InputKeyEvent&)         override {}
    void OnMouseButtonEvent(const InputMouseButtonEvent&) override {}
    void OnMouseMoveEvent  (const InputMouseMoveEvent&)   override {}
    void OnMouseScrollEvent(const InputMouseScrollEvent&) override {}

    bool    KeyDown    (Key)         const override { return false; }
    bool    KeyHit     (Key)         const override { return false; }
    bool    KeyReleased(Key)         const override { return false; }
    bool    MouseButtonDown    (MouseButton) const override { return false; }
    bool    MouseButtonHit     (MouseButton) const override { return false; }
    bool    MouseButtonReleased(MouseButton) const override { return false; }
    int32_t MouseX()           const override { return 0; }
    int32_t MouseY()           const override { return 0; }
    int32_t MouseDeltaX()      const override { return 0; }
    int32_t MouseDeltaY()      const override { return 0; }
    float   MouseScrollDelta() const override { return 0.f; }
};

} // namespace engine::platform
