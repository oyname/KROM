#pragma once
// =============================================================================
// KROM Engine - platform/IInput.hpp
//
// Instanzbasierte plattformneutrale Input-Abstraktion.
// Ersetzt jede statische Input-Klasse (kein globaler State).
//
// BeginFrame() einmal pro Frame vor allen Abfragen aufrufen.
// OnKey/Mouse*() werden vom Windowing-System pro Event aufgerufen.
//
// Implementierungen: Win32Input, LinuxInput, NullInput (Tests/Headless)
// =============================================================================
#include <cstdint>
#include <memory>

namespace engine::platform {

// ---------------------------------------------------------------------------
// Key
// ---------------------------------------------------------------------------
enum class Key : uint16_t
{
    Unknown = 0,
    Escape, Space, Enter, Tab, Backspace,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt,
    Left, Right, Up, Down,
    Home, End, PageUp, PageDown, Insert, Delete,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4,
    Num5, Num6, Num7, Num8, Num9,
    Plus, Minus, Equals,
    F1,  F2,  F3,  F4,  F5,  F6,
    F7,  F8,  F9,  F10, F11, F12,
    Count
};

enum class MouseButton : uint8_t
{
    Left = 0, Right = 1, Middle = 2, X1 = 3, X2 = 4, Count
};

// ---------------------------------------------------------------------------
// Input-Events (plattformneutral)
// ---------------------------------------------------------------------------
struct InputKeyEvent         { Key key; bool pressed; bool repeat; };
struct InputMouseButtonEvent { MouseButton button; bool pressed; int32_t x; int32_t y; };
struct InputMouseMoveEvent   { int32_t x; int32_t y; int32_t deltaX; int32_t deltaY; };
struct InputMouseScrollEvent { float delta; };

// ---------------------------------------------------------------------------
// Erweiterte Event-/Polling-Struktur für GLFW-Plattform-Layer
// ---------------------------------------------------------------------------
enum class InputEventType : uint8_t
{
    None = 0,
    Key,
    MouseButton,
    MouseMove,
    MouseScroll
};

struct MousePosition
{
    double x = 0.0;
    double y = 0.0;
};

struct InputEvent
{
    InputEventType type = InputEventType::None;
    InputKeyEvent key{};
    InputMouseButtonEvent mouseButton{};
    InputMouseMoveEvent mouseMove{};
    InputMouseScrollEvent mouseScroll{};
};

// ---------------------------------------------------------------------------
// IInput
// ---------------------------------------------------------------------------
class IInput
{
public:
    virtual ~IInput() = default;

    virtual void BeginFrame() = 0;

    // Event-Einspeisung vom Windowing-System
    virtual void OnKeyEvent        (const InputKeyEvent&)         = 0;
    virtual void OnMouseButtonEvent(const InputMouseButtonEvent&) = 0;
    virtual void OnMouseMoveEvent  (const InputMouseMoveEvent&)   = 0;
    virtual void OnMouseScrollEvent(const InputMouseScrollEvent&) = 0;

    // Tastatur
    [[nodiscard]] virtual bool KeyDown    (Key key) const = 0;
    [[nodiscard]] virtual bool KeyHit     (Key key) const = 0;  // erstmals gedrückt diesen Frame
    [[nodiscard]] virtual bool KeyReleased(Key key) const = 0;
    [[nodiscard]] virtual bool IsKeyPressed(Key key) const { return KeyDown(key); }

    // Maus
    [[nodiscard]] virtual bool    MouseButtonDown    (MouseButton b) const = 0;
    [[nodiscard]] virtual bool    MouseButtonHit     (MouseButton b) const = 0;
    [[nodiscard]] virtual bool    MouseButtonReleased(MouseButton b) const = 0;
    [[nodiscard]] virtual bool    IsMouseButtonPressed(MouseButton b) const { return MouseButtonDown(b); }
    [[nodiscard]] virtual int32_t MouseX()           const = 0;
    [[nodiscard]] virtual int32_t MouseY()           const = 0;
    [[nodiscard]] virtual int32_t MouseDeltaX()      const = 0;
    [[nodiscard]] virtual int32_t MouseDeltaY()      const = 0;
    [[nodiscard]] virtual float   MouseScrollDelta() const = 0;
    [[nodiscard]] virtual MousePosition GetMousePosition() const
    {
        return MousePosition{static_cast<double>(MouseX()), static_cast<double>(MouseY())};
    }

    virtual void SetCursorMode(bool captured, bool hidden)
    {
        (void)captured;
        (void)hidden;
    }

    [[nodiscard]] virtual bool PollEvent(InputEvent& outEvent)
    {
        outEvent = {};
        return false;
    }
};

} // namespace engine::platform
