#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

// WASAPI loopback capture of the default render endpoint: records what the container is playing
// without a virtual cable or a capture app. The caller owns COM initialization.
class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    bool Initialize(std::wstring& error);
    bool Start(std::wstring& error);
    void Stop();
    void Shutdown();

    // Appends available interleaved PCM16 samples. An empty result is normal while the endpoint is
    // idle; callers should keep polling.
    bool Read(std::vector<std::int16_t>& samples, std::wstring& error);

    int SampleRate() const { return sampleRate_; }
    int Channels() const { return channels_; }
    bool Ready() const { return client_ != nullptr && capture_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    WAVEFORMATEX* format_ = nullptr;
    int sampleRate_ = 0;
    int channels_ = 0;
    bool started_ = false;
};
