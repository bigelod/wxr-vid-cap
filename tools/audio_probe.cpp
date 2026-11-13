// WASAPI loopback audio probe for Winlator containers.
//
// Opens the default render endpoint in loopback mode, records a few seconds, and writes a PCM WAV
// next to the EXE plus audio_log.txt. This answers whether the container exposes system audio for
// capture before we build Opus encoding on top of it.

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::ofstream g_logFile;

void Log(const char* format, ...)
{
    char buffer[2048] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    std::fputs(buffer, stdout);
    std::fflush(stdout);
    if (g_logFile) {
        g_logFile << buffer;
        g_logFile.flush();
    }
}

std::filesystem::path ExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(path, length)).parent_path();
}

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

bool WriteWav(const std::filesystem::path& path, const std::vector<std::int16_t>& samples, int channels,
              int sampleRate)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t byteRate = static_cast<std::uint32_t>(sampleRate * channels * 2);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * 2);

    auto put32 = [&file](std::uint32_t value) {
        const std::uint8_t bytes[4] = {
            static_cast<std::uint8_t>(value & 0xFF), static_cast<std::uint8_t>((value >> 8) & 0xFF),
            static_cast<std::uint8_t>((value >> 16) & 0xFF), static_cast<std::uint8_t>((value >> 24) & 0xFF)};
        file.write(reinterpret_cast<const char*>(bytes), 4);
    };
    auto put16 = [&file](std::uint16_t value) {
        const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value & 0xFF),
                                       static_cast<std::uint8_t>((value >> 8) & 0xFF)};
        file.write(reinterpret_cast<const char*>(bytes), 2);
    };

    file.write("RIFF", 4);
    put32(36 + dataBytes);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    put32(16);
    put16(1);  // PCM
    put16(static_cast<std::uint16_t>(channels));
    put32(static_cast<std::uint32_t>(sampleRate));
    put32(byteRate);
    put16(blockAlign);
    put16(16);
    file.write("data", 4);
    put32(dataBytes);
    file.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(dataBytes));
    return file.good();
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    int seconds = 3;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--seconds" && i + 1 < argc) {
            seconds = _wtoi(argv[++i]);
        } else if (arg == L"--quiet") {
            quiet = true;
        } else {
            std::printf("usage: audio_probe [--seconds 3] [--quiet]\n");
            return 2;
        }
    }
    if (seconds <= 0) {
        return 2;
    }

    const std::filesystem::path exeDirectory = ExecutableDirectory();
    const std::filesystem::path logPath = exeDirectory / L"audio_log.txt";
    const std::filesystem::path wavPath = exeDirectory / L"audio_probe.wav";
    g_logFile.open(logPath, std::ios::binary);

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    Log("wxr-vid-cap audio probe\n");
    Log("time: %04u-%02u-%02u %02u:%02u:%02u\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond);
    Log("exe: %ls\n\n", exeDirectory.c_str());

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Log("CoInitializeEx: 0x%08X\n", static_cast<unsigned>(com));

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    Log("MMDeviceEnumerator: 0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        Log("result: failed\n");
        if (!quiet) {
            MessageBoxW(nullptr, L"Audio probe failed. See audio_log.txt.", L"wxr-vid-cap audio probe", MB_OK);
        }
        return 1;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    Log("GetDefaultAudioEndpoint(eRender): 0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        Log("result: failed (no render endpoint)\n");
        if (!quiet) {
            MessageBoxW(nullptr, L"No default audio endpoint. See audio_log.txt.", L"wxr-vid-cap audio probe", MB_OK);
        }
        return 1;
    }

    LPWSTR deviceId = nullptr;
    if (SUCCEEDED(device->GetId(&deviceId)) && deviceId != nullptr) {
        Log("device id: %ls\n", deviceId);
        CoTaskMemFree(deviceId);
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    Log("IAudioClient Activate: 0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        Log("result: failed\n");
        return 1;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = client->GetMixFormat(&mixFormat);
    Log("GetMixFormat: 0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr) || mixFormat == nullptr) {
        Log("result: failed\n");
        return 1;
    }

    Log("mix format: tag=%u channels=%u rate=%u bits=%u\n", mixFormat->wFormatTag, mixFormat->nChannels,
        mixFormat->nSamplesPerSec, mixFormat->wBitsPerSample);

    const REFERENCE_TIME bufferDuration = 2000000;  // 200 ms
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufferDuration, 0, mixFormat,
                            nullptr);
    Log("Initialize(loopback): 0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        Log("result: failed (loopback not supported)\n");
        CoTaskMemFree(mixFormat);
        if (!quiet) {
            MessageBoxW(nullptr, L"WASAPI loopback is not available. See audio_log.txt.", L"wxr-vid-cap audio probe",
                        MB_OK);
        }
        return 1;
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(IID_PPV_ARGS(&capture));
    Log("IAudioCaptureClient: 0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        Log("result: failed\n");
        CoTaskMemFree(mixFormat);
        return 1;
    }

    hr = client->Start();
    Log("Start: 0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        Log("result: failed\n");
        CoTaskMemFree(mixFormat);
        return 1;
    }

    const int channels = mixFormat->nChannels;
    const int sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
    const int bytesPerSample = mixFormat->wBitsPerSample / 8;
    std::vector<std::int16_t> samples;
    std::uint64_t totalFrames = 0;

    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    while (GetTickCount64() < deadline) {
        Sleep(50);

        UINT32 packetFrames = 0;
        if (FAILED(capture->GetNextPacketSize(&packetFrames))) {
            break;
        }

        while (packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                break;
            }

            const std::size_t oldSize = samples.size();
            samples.resize(oldSize + static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                std::fill(samples.begin() + static_cast<std::ptrdiff_t>(oldSize), samples.end(),
                          static_cast<std::int16_t>(0));
            } else {
                for (UINT32 frame = 0; frame < frames; ++frame) {
                    for (int channel = 0; channel < channels; ++channel) {
                        const BYTE* sample = data + (static_cast<std::size_t>(frame) * channels + channel) * bytesPerSample;
                        samples[oldSize + static_cast<std::size_t>(frame) * channels + channel] = ToPcm16(sample, mixFormat);
                    }
                }
            }

            totalFrames += frames;
            capture->ReleaseBuffer(frames);
            if (FAILED(capture->GetNextPacketSize(&packetFrames))) {
                break;
            }
        }
    }

    client->Stop();
    CoTaskMemFree(mixFormat);

    const bool wrote = WriteWav(wavPath, samples, channels, sampleRate);
    Log("captured frames: %llu (%.2f s)\n", static_cast<unsigned long long>(totalFrames),
        sampleRate > 0 ? static_cast<double>(totalFrames) / sampleRate : 0.0);
    Log("wav: %ls (%s)\n", wavPath.c_str(), wrote ? "written" : "FAILED");
    Log("result: %s\n", wrote ? "ok" : "failed");

    if (!quiet) {
        MessageBoxW(nullptr, L"Audio probe finished. See audio_log.txt and audio_probe.wav.",
                    L"wxr-vid-cap audio probe", MB_OK);
    }
    return wrote ? 0 : 1;
}
