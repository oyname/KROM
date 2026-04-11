#include "platform/IWindow.hpp"
#include "platform/IInput.hpp"

namespace engine::platform {

bool HeadlessWindow::Create(const WindowDesc& desc)
{
    m_width = desc.width;
    m_height = desc.height;
    m_fbWidth = desc.framebufferWidth ? desc.framebufferWidth : desc.width;
    m_fbHeight = desc.framebufferHeight ? desc.framebufferHeight : desc.height;
    m_title = desc.title;
    m_open = true;
    m_quitRequested = false;
    m_resizePending = false;
    return true;
}

void HeadlessWindow::Destroy()
{
    m_open = false;
    m_quitRequested = false;
    m_resizePending = false;
    m_width = 0u;
    m_height = 0u;
    m_fbWidth = 0u;
    m_fbHeight = 0u;
    m_title.clear();
}

WindowEventState HeadlessWindow::PumpEvents(IInput& input)
{
    (void)input;

    WindowEventState state{};
    state.quitRequested = m_quitRequested;
    state.resized = m_resizePending;
    state.width = m_width;
    state.height = m_height;
    state.framebufferWidth = m_fbWidth;
    state.framebufferHeight = m_fbHeight;

    if (m_quitRequested)
        m_open = false;

    m_resizePending = false;
    return state;
}

void HeadlessWindow::Resize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    m_fbWidth = width;
    m_fbHeight = height;
    m_resizePending = true;
}

void HeadlessWindow::SetTitle(const char* title)
{
    m_title = title ? title : "";
}

std::unique_ptr<IWindow> CreateHeadlessWindow()
{
    return std::make_unique<HeadlessWindow>();
}

} // namespace engine::platform
