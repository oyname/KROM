#pragma once

namespace engine::platform {

class IAudio
{
public:
    virtual ~IAudio() = default;
    virtual bool Initialize(int sampleRate = 48000, int bufferSize = 512) = 0;
    virtual void Shutdown() = 0;
    virtual void PlaySound(const char* filepath, float volume = 1.0f) = 0;
    virtual void SetMasterVolume(float volume) = 0;
    virtual void Update() = 0;
};

} // namespace engine::platform
