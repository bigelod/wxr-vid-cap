// Test-only H.264 recorder. Captures the desktop, converts to I420, encodes with the vendored
// OpenH264 DLL, and writes Matroska beside the EXE with a log beside the MKV.
//
// Defaults: 3 s, 15 fps, 1280x720, 4000 kbps, backend auto.

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "audio_capture.h"
#include "audio_resampler.h"
#include "color_convert.h"
#include "h264_encoder.h"
#include "mkv_writer.h"
#include "opus_audio_encoder.h"
#include "screen_capture.h"

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

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    int seconds = 3;
    int fps = 15;
    int width = 1280;
    int height = 720;
    int bitrateKbps = 4000;
    std::wstring output;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--seconds" && i + 1 < argc) {
            seconds = _wtoi(argv[++i]);
        } else if (arg == L"--fps" && i + 1 < argc) {
            fps = _wtoi(argv[++i]);
        } else if (arg == L"--width" && i + 1 < argc) {
            width = _wtoi(argv[++i]);
        } else if (arg == L"--height" && i + 1 < argc) {
            height = _wtoi(argv[++i]);
        } else if (arg == L"--bitrate" && i + 1 < argc) {
            bitrateKbps = _wtoi(argv[++i]);
        } else if (arg == L"--out" && i + 1 < argc) {
            output = argv[++i];
        } else {
            std::printf("unknown option\n");
            return 2;
        }
    }

    if (seconds <= 0 || fps <= 0 || width <= 0 || height <= 0 || bitrateKbps <= 0 || (width & 1) != 0 ||
        (height & 1) != 0) {
        std::printf("usage: encode_test [--seconds 3] [--fps 15] [--width 1280] [--height 720] "
                    "[--bitrate 4000] [--out capture_test.mkv]\n"
                    "width and height must be even\n");
        return 2;
    }

    const std::filesystem::path exeDirectory = ExecutableDirectory();
    std::filesystem::path outputPath =
        output.empty() ? (exeDirectory / L"capture_test.mkv") : std::filesystem::path(output);
    if (outputPath.is_relative()) {
        outputPath = exeDirectory / outputPath;
    }

    const std::filesystem::path outputDirectory =
        outputPath.parent_path().empty() ? exeDirectory : outputPath.parent_path();
    std::error_code directoryError;
    std::filesystem::create_directories(outputDirectory, directoryError);

    const std::filesystem::path logPath = outputDirectory / L"encode_log.txt";
    g_logFile.open(logPath, std::ios::binary);

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    Log("wxr-vid-cap encode_test\n");
    Log("time: %04u-%02u-%02u %02u:%02u:%02u\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond);
    Log("requested: %d s, %d fps, %dx%d, %d kbps\n", seconds, fps, width, height, bitrateKbps);
    Log("exe: %ls\n", exeDirectory.c_str());
    Log("output: %ls\n", outputPath.c_str());
    Log("log: %ls\n\n", logPath.c_str());

    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ScreenCapture gdi;
    std::wstring error;

    if (!gdi.Initialize(width, height, error, true)) {
        Log("capture init failed: %ls\n", error.c_str());
        Log("result: failed\n");
        return 1;
    }
    Log("backend: GDI (top-most window), capture %dx%d\n", gdi.Width(), gdi.Height());

    H264Encoder encoder;
    if (!encoder.Initialize(width, height, fps, bitrateKbps, error)) {
        Log("encoder init failed: %ls\n", error.c_str());
        Log("result: failed\n");
        return 1;
    }
    Log("encoder: OpenH264 %dx%d at %d fps, %d kbps\n", width, height, fps, bitrateKbps);

    AudioCapture audio;
    OpusAudioEncoder opus;
    AudioResampler resampler;
    bool audioEnabled = false;
    if (!audio.Initialize(error)) {
        Log("audio unavailable: %ls\n", error.c_str());
        Log("continuing video-only\n");
    } else if (!opus.Initialize(48000, audio.Channels(), 96000, error)) {
        Log("Opus init failed: %ls\n", error.c_str());
        Log("continuing video-only\n");
    } else if (!audio.Start(error)) {
        Log("audio start failed: %ls\n", error.c_str());
        Log("continuing video-only\n");
    } else {
        resampler.Initialize(audio.SampleRate(), audio.Channels(), 48000);
        audioEnabled = true;
        Log("audio: capture %d Hz -> Opus 48000 Hz, %d channel(s), 96 kbps\n", audio.SampleRate(),
            audio.Channels());
    }

    std::ofstream file(outputPath, std::ios::binary);
    if (!file) {
        Log("could not create %ls\n", outputPath.c_str());
        Log("result: failed\n");
        return 1;
    }

    std::vector<std::uint8_t> audioCodecPrivate;
    if (audioEnabled) {
        audioCodecPrivate = opus.CodecPrivate();
    }

    MkvWriter writer(file, width, height, fps, audioEnabled ? opus.SampleRate() : 0,
                     audioEnabled ? audio.Channels() : 0, audioCodecPrivate);
    const int64_t frameTicks = 90000 / fps;
    const int totalFrames = fps * seconds;
    const auto start = std::chrono::steady_clock::now();
    double captureMs = 0.0;
    double convertMs = 0.0;
    double encodeMs = 0.0;
    double audioMs = 0.0;
    std::size_t encodedBytes = 0;
    std::size_t audioBytes = 0;
    int writtenFrames = 0;
    int keyFrames = 0;
    int audioPackets = 0;

    const int audioChannels = audioEnabled ? audio.Channels() : 0;
    const int audioRate = audioEnabled ? opus.SampleRate() : 0;
    const int opusFrameSamples = audioEnabled ? opus.FrameSamples() : 0;
    std::vector<std::int16_t> audioBuffer;
    std::size_t audioOffset = 0;
    std::int64_t audioSamplesEncoded = 0;

    auto drainAudio = [&](std::int64_t upToPts) -> bool {
        if (!audioEnabled) {
            return true;
        }

        std::vector<std::int16_t> captured;
        if (!audio.Read(captured, error)) {
            Log("audio read failed: %ls\n", error.c_str());
            return false;
        }
        std::vector<std::int16_t> resampled;
        resampler.Process(captured, resampled);
        audioBuffer.insert(audioBuffer.end(), resampled.begin(), resampled.end());

        for (;;) {
            const std::size_t availableSamples =
                (audioBuffer.size() - audioOffset) / static_cast<std::size_t>(audioChannels);
            if (availableSamples < static_cast<std::size_t>(opusFrameSamples)) {
                break;
            }

            const std::int64_t pts = audioSamplesEncoded * 90000 / audioRate;
            if (pts > upToPts) {
                break;
            }

            std::vector<std::uint8_t> packet;
            if (!opus.Encode(audioBuffer.data() + audioOffset, opusFrameSamples, packet, error)) {
                Log("Opus encode failed: %ls\n", error.c_str());
                return false;
            }
            if (!writer.WriteAudioFrame(packet.data(), packet.size(), pts)) {
                Log("audio mux failed at pts %lld\n", static_cast<long long>(pts));
                return false;
            }

            audioOffset += static_cast<std::size_t>(opusFrameSamples) * static_cast<std::size_t>(audioChannels);
            audioSamplesEncoded += opusFrameSamples;
            audioBytes += packet.size();
            ++audioPackets;
        }

        // Compact the consumed prefix periodically instead of erasing per packet.
        if (audioOffset > 0 && audioOffset >= 48000u * static_cast<std::size_t>(audioChannels)) {
            audioBuffer.erase(audioBuffer.begin(), audioBuffer.begin() + static_cast<std::ptrdiff_t>(audioOffset));
            audioOffset = 0;
        }
        return true;
    };

    for (int index = 0; index < totalFrames; ++index) {
        const std::int64_t pts = static_cast<std::int64_t>(index) * frameTicks;
        if (audioEnabled && index > 0) {
            const auto audioStart = std::chrono::steady_clock::now();
            if (!drainAudio(pts)) {
                Log("result: failed\n");
                return 1;
            }
            audioMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - audioStart).count();
        }

        const auto captureStart = std::chrono::steady_clock::now();

        std::vector<std::uint8_t> bgra;
        if (!gdi.Capture(error)) {
            Log("capture failed at frame %d: %ls\n", index, error.c_str());
            Log("result: failed\n");
            return 1;
        }
        bgra.assign(gdi.Pixels(),
                    gdi.Pixels() + static_cast<std::size_t>(gdi.Width()) * static_cast<std::size_t>(gdi.Height()) * 4);

        const auto convertStart = std::chrono::steady_clock::now();
        captureMs += std::chrono::duration<double, std::milli>(convertStart - captureStart).count();

        std::vector<std::uint8_t> i420;
        if (!BgraToI420(bgra.data(), width, height, i420)) {
            Log("BGRA -> I420 conversion failed at frame %d\n", index);
            Log("result: failed\n");
            return 1;
        }

        const auto encodeStart = std::chrono::steady_clock::now();
        convertMs += std::chrono::duration<double, std::milli>(encodeStart - convertStart).count();

        bool keyframe = false;
        std::vector<std::uint8_t> annexB;
        if (!encoder.Encode(i420, pts, keyframe, annexB, error)) {
            Log("encode failed at frame %d: %ls\n", index, error.c_str());
            Log("result: failed\n");
            return 1;
        }

        encodeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - encodeStart).count();

        if (!annexB.empty()) {
            if (!writer.WriteVideoFrame(annexB.data(), annexB.size(), pts, keyframe)) {
                Log("mux failed at frame %d\n", index);
                Log("result: failed\n");
                return 1;
            }
            encodedBytes += annexB.size();
            ++writtenFrames;
            if (keyframe) {
                ++keyFrames;
            }
        }

        // The first video frame creates the container header, so audio can only be drained after it.
        if (audioEnabled && index == 0) {
            const auto audioStart = std::chrono::steady_clock::now();
            if (!drainAudio(0)) {
                Log("result: failed\n");
                return 1;
            }
            audioMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - audioStart).count();
        }

        const auto deadline =
            start + std::chrono::microseconds(static_cast<long long>(index + 1) * 1000000 / fps);
        std::this_thread::sleep_until(deadline);
    }

    if (audioEnabled) {
        const auto audioStart = std::chrono::steady_clock::now();
        if (!drainAudio(std::numeric_limits<std::int64_t>::max())) {
            Log("result: failed\n");
            return 1;
        }
        audioMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - audioStart).count();
    }
    audio.Stop();

    if (!writer.Finalize()) {
        Log("finalize failed\n");
        Log("result: failed\n");
        return 1;
    }

    const std::uintmax_t fileSize = std::filesystem::file_size(outputPath, directoryError);
    Log("\nwritten frames: %d (keyframes: %d)\n", writtenFrames, keyFrames);
    Log("audio packets: %d (%zu bytes)\n", audioPackets, audioBytes);
    Log("encoded bytes: %zu, file bytes: %llu\n", encodedBytes, static_cast<unsigned long long>(fileSize));
    Log("avg capture+scale: %.2f ms, convert: %.2f ms, encode: %.2f ms, audio: %.2f ms, total: %.2f ms\n",
        captureMs / totalFrames, convertMs / totalFrames, encodeMs / totalFrames, audioMs / totalFrames,
        (captureMs + convertMs + encodeMs + audioMs) / totalFrames);
    Log("output: %ls\n", outputPath.c_str());
    Log("log: %ls\n", logPath.c_str());
    Log("result: ok\n");

    CoUninitialize();
    return 0;
}
