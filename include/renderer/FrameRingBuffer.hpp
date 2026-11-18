#pragma once

#include "renderer/IDevice.hpp"
#include <array>
#include <cstdint>

namespace engine::renderer {

// Several copies of a CPU-written buffer that is rewritten once per frame.
//
// Writing into a host-visible buffer that the GPU may still read makes the backend wait
// in UploadBufferData (Vulkan waits for the last submission that used the buffer). With
// one buffer per feature this waits for the whole previous frame. Each frame takes the
// next copy instead, so the copy being written was last used kCopies frames ago and is
// normally finished. The backend wait stays in place, so a ring that is too short is
// slower but never wrong.
//
// Call Next() once per frame, before writing. Consumers must use the handle Next()
// returned for this frame (it changes every frame).
class FrameRingBuffer
{
public:
    // Matches the Vulkan default of three frames in flight.
    static constexpr uint32_t kCopies = 3u;

    FrameRingBuffer() = default;
    FrameRingBuffer(const FrameRingBuffer&) = delete;
    FrameRingBuffer& operator=(const FrameRingBuffer&) = delete;
    // Moving hands the copies over; the source no longer owns them.
    FrameRingBuffer(FrameRingBuffer&& other) noexcept
        : m_slots(other.m_slots), m_index(other.m_index)
    {
        other.m_slots = {};
        other.m_index = 0u;
    }
    FrameRingBuffer& operator=(FrameRingBuffer&& other) noexcept
    {
        if (this != &other)
        {
            m_slots = other.m_slots;
            m_index = other.m_index;
            other.m_slots = {};
            other.m_index = 0u;
        }
        return *this;
    }

    // Moves to the next copy and makes sure it holds at least desc.byteSize bytes.
    // A copy that is too small is recreated with desc; the other copies grow when it is
    // their turn. Returns an invalid handle if the buffer could not be created.
    BufferHandle Next(IDevice& device, const BufferDesc& desc)
    {
        m_index = (m_index + 1u) % kCopies;
        Slot& slot = m_slots[m_index];
        if (!slot.handle.IsValid() || slot.byteSize < desc.byteSize)
        {
            if (slot.handle.IsValid())
                device.DestroyBuffer(slot.handle);
            slot.handle = device.CreateBuffer(desc);
            slot.byteSize = slot.handle.IsValid() ? desc.byteSize : 0u;
        }
        return slot.handle;
    }

    [[nodiscard]] BufferHandle Current() const noexcept { return m_slots[m_index].handle; }
    [[nodiscard]] uint64_t CurrentByteSize() const noexcept { return m_slots[m_index].byteSize; }

    // Destroys every copy. Without a device the handles are only forgotten (device gone).
    void Release(IDevice* device) noexcept
    {
        for (Slot& slot : m_slots)
        {
            if (device && slot.handle.IsValid())
                device->DestroyBuffer(slot.handle);
            slot = {};
        }
        m_index = 0u;
    }

private:
    struct Slot
    {
        BufferHandle handle = BufferHandle::Invalid();
        uint64_t byteSize = 0u;
    };

    std::array<Slot, kCopies> m_slots{};
    uint32_t m_index = 0u;
};

} // namespace engine::renderer
