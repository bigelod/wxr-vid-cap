#include "audio_capture.h"

#include <ksmedia.h>

#include <algorithm>
#include <cstring>

namespace {

bool IsFloatFormat(const WAVEFORMATEX* format)
{
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return false;
}

std::int16_t ToPcm16(const BYTE* sample, const WAVEFORMATEX* format)
{
    if (IsFloatFormat(format)) {
        float value = 0.0f;
        std::memcpy(&value, sample, sizeof(value));
        value = std::clamp(value, -1.0f, 1.0f);
        return static_cast<std::int16_t>(value * 32767.0f);
    }

    if (format->wBitsPerSample == 16) {
        std::int16_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return value;
    }

    if (format->wBitsPerSample == 32) {
        std::int32_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<std::int16_t>(value >> 16);
    }

    if (format->wBitsPerSample == 8) {
        return static_cast<std::int16_t>((static_cast<int>(*sample) - 128) << 8);
    }

    return 0;
}

}  // namespace

AudioCapture::~AudioCapture()
{
    Shutdown();
}

bool AudioCapture::Initialize(std::wstring& error)
{
    Shutdown();

    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
    if (FAILED(result)) {
        error = L"Could not create the MMDeviceEnumerator.";
        return false;
    }

    result = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(result)) {
        error = L"No default render endpoint (is audio configured?).";
        Shutdown();
        return false;
    }

    result = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(result)) {
        error = L"Could not activate IAudioClient.";
        Shutdown();
        return false;
    }

    result = client_->GetMixFormat(&format_);
    if (FAILED(result) || format_ == nullptr) {
        error = L"Could not read the endpoint mix format.";
        Shutdown();
        return false;
    }

    sampleRate_ = static_cast<int>(format_->nSamplesPerSec);
    channels_ = static_cast<int>(format_->nChannels);

    const REFERENCE_TIME bufferDuration = 2000000;  // 200 ms
    result = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufferDuration, 0, format_,
                                 nullptr);
    if (FAILED(result)) {
        error = L"WASAPI loopback is not supported by this endpoint.";
        Shutdown();
        return false;
    }

    result = client_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(result)) {
        error = L"Could not get IAudioCaptureClient.";
        Shutdown();
        return false;
    }

    return true;
}

bool AudioCapture::Start(std::wstring& error)
{
    if (!Ready()) {
        error = L"Audio capture was not initialized.";
        return false;
    }
    if (started_) {
        return true;
    }

    const HRESULT result = client_->Start();
    if (FAILED(result)) {
        error = L"IAudioClient::Start failed.";
        return false;
    }
    started_ = true;
    return true;
}

void AudioCapture::Stop()
{
    if (started_ && client_ != nullptr) {
        client_->Stop();
    }
    started_ = false;
}

void AudioCapture::Shutdown()
{
    Stop();
    capture_.Reset();
    client_.Reset();
    device_.Reset();
    enumerator_.Reset();
    if (format_ != nullptr) {
        CoTaskMemFree(format_);
        format_ = nullptr;
    }
    sampleRate_ = 0;
    channels_ = 0;
}

bool AudioCapture::Read(std::vector<std::int16_t>& samples, std::wstring& error)
{
    if (!Ready() || format_ == nullptr) {
        error = L"Audio capture was not initialized.";
        return false;
    }

    samples.clear();
    const int bytesPerSample = static_cast<int>(format_->wBitsPerSample) / 8;

    UINT32 packetFrames = 0;
    if (FAILED(capture_->GetNextPacketSize(&packetFrames))) {
        error = L"GetNextPacketSize failed.";
        return false;
    }

    while (packetFrames > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
            error = L"IAudioCaptureClient::GetBuffer failed.";
            return false;
        }

        const std::size_t oldSize = samples.size();
        samples.resize(oldSize + static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_));

        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
            std::fill(samples.begin() + static_cast<std::ptrdiff_t>(oldSize), samples.end(),
                      static_cast<std::int16_t>(0));
        } else {
            for (UINT32 frame = 0; frame < frames; ++frame) {
                for (int channel = 0; channel < channels_; ++channel) {
                    const BYTE* sample =
                        data + (static_cast<std::size_t>(frame) * static_cast<std::size_t>(channels_) +
                                static_cast<std::size_t>(channel)) *
                                   static_cast<std::size_t>(bytesPerSample);
                    samples[oldSize + static_cast<std::size_t>(frame) * static_cast<std::size_t>(channels_) +
                            static_cast<std::size_t>(channel)] = ToPcm16(sample, format_);
                }
            }
        }

        capture_->ReleaseBuffer(frames);

        if (FAILED(capture_->GetNextPacketSize(&packetFrames))) {
            error = L"GetNextPacketSize failed.";
            return false;
        }
    }

    return true;
}
