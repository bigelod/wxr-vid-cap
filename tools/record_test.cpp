// Test-only local recorder. Captures the desktop with the self-made capture path and writes an
// uncompressed AVI next to the EXE, with a log beside the AVI. Defaults are the current probe
// settings (3 s, 15 fps, 1280x720).
//
// This is a milestone probe, not the shipping recorder: uncompressed frames are large, and the
// encoder stage is still being chosen. No VLC, no FFmpeg, no shelling out.

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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

void Put16(std::string& out, std::uint16_t value)
{
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void Put32(std::string& out, std::uint32_t value)
{
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

void AppendChunk(std::string& out, const char* id, const std::string& payload)
{
    out.append(id, 4);
    Put32(out, static_cast<std::uint32_t>(payload.size()));
    out.append(payload);
    if ((payload.size() & 1) != 0) {
        out.push_back('\0');
    }
}

std::string MakeList(const char* type, const std::string& payload)
{
    std::string list;
    list.append("LIST", 4);
    Put32(list, static_cast<std::uint32_t>(payload.size() + 4));
    list.append(type, 4);
    list.append(payload);
    if ((list.size() & 1) != 0) {
        list.push_back('\0');
    }
    return list;
}

// Converts top-down BGRA to bottom-up BGR24, which is what BI_RGB AVI expects.
std::vector<std::uint8_t> ToBgr24(const std::uint8_t* bgra, int width, int height, std::uint32_t& stride)
{
    stride = (static_cast<std::uint32_t>(width) * 3u + 3u) & ~3u;
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height), 0);

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* sourceRow = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        std::uint8_t* targetRow =
            frame.data() + static_cast<std::size_t>(height - 1 - y) * static_cast<std::size_t>(stride);

        for (int x = 0; x < width; ++x) {
            const std::uint8_t* pixel = sourceRow + static_cast<std::size_t>(x) * 4;
            targetRow[static_cast<std::size_t>(x) * 3 + 0] = pixel[0];
            targetRow[static_cast<std::size_t>(x) * 3 + 1] = pixel[1];
            targetRow[static_cast<std::size_t>(x) * 3 + 2] = pixel[2];
        }
    }
    return frame;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    int seconds = 3;
    int fps = 15;
    int width = 1280;
    int height = 720;
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
        } else if (arg == L"--out" && i + 1 < argc) {
            output = argv[++i];
        } else {
            std::printf("unknown option\n");
            return 2;
        }
    }

    if (seconds <= 0 || fps <= 0 || width <= 0 || height <= 0) {
        std::printf("usage: record_test [--seconds 3] [--fps 15] [--width 1280] [--height 720] "
                    "[--out capture_test.avi]\n");
        return 2;
    }

    const std::filesystem::path exeDirectory = ExecutableDirectory();
    std::filesystem::path outputPath =
        output.empty() ? (exeDirectory / L"capture_test.avi") : std::filesystem::path(output);
    if (outputPath.is_relative()) {
        outputPath = exeDirectory / outputPath;
    }

    const std::filesystem::path outputDirectory =
        outputPath.parent_path().empty() ? exeDirectory : outputPath.parent_path();
    std::error_code directoryError;
    std::filesystem::create_directories(outputDirectory, directoryError);

    const std::filesystem::path logPath = outputDirectory / L"record_log.txt";
    g_logFile.open(logPath, std::ios::binary);

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    Log("wxr-vid-cap record_test\n");
    Log("time: %04u-%02u-%02u %02u:%02u:%02u\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond);
    Log("requested: %d s, %d fps, %dx%d\n", seconds, fps, width, height);
    Log("exe: %ls\n", exeDirectory.c_str());
    Log("output: %ls\n", outputPath.c_str());
    Log("log: %ls\n\n", logPath.c_str());

    SetProcessDPIAware();

    ScreenCapture gdi;
    std::wstring error;

    if (!gdi.Initialize(width, height, error, true)) {
        Log("capture init failed: %ls\n", error.c_str());
        Log("result: failed\n");
        return 1;
    }
    Log("backend: GDI (top-most window), capture %dx%d at %d fps for %d s\n", gdi.Width(), gdi.Height(), fps, seconds);

    std::string movi;
    std::string index;
    std::uint32_t stride = 0;
    std::uint32_t frameBytes = 0;
    const int totalFrames = fps * seconds;
    const auto start = std::chrono::steady_clock::now();
    double frameMs = 0.0;

    for (int i = 0; i < totalFrames; ++i) {
        const auto frameStart = std::chrono::steady_clock::now();

        std::vector<std::uint8_t> frame;
        if (!gdi.Capture(error)) {
            Log("capture failed at frame %d: %ls\n", i, error.c_str());
            Log("result: failed\n");
            return 1;
        }
        frame = ToBgr24(gdi.Pixels(), gdi.Width(), gdi.Height(), stride);

        frameMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
        frameBytes = static_cast<std::uint32_t>(frame.size());

        const std::uint32_t offset = static_cast<std::uint32_t>(movi.size() + 4);  // relative to 'movi'
        AppendChunk(movi, "00db", std::string(reinterpret_cast<const char*>(frame.data()), frame.size()));

        index.append("00db", 4);
        Put32(index, 0x10);  // AVIIF_KEYFRAME
        Put32(index, offset);
        Put32(index, frameBytes);

        const auto deadline =
            start + std::chrono::microseconds(static_cast<long long>(i + 1) * 1000000 / fps);
        std::this_thread::sleep_until(deadline);
    }

    const std::uint32_t microsPerFrame = static_cast<std::uint32_t>(1000000 / fps);

    std::string avih;
    Put32(avih, microsPerFrame);
    Put32(avih, frameBytes * static_cast<std::uint32_t>(fps));
    Put32(avih, 0);     // padding granularity
    Put32(avih, 0x10);  // AVIF_HASINDEX
    Put32(avih, static_cast<std::uint32_t>(totalFrames));
    Put32(avih, 0);  // initial frames
    Put32(avih, 1);  // streams
    Put32(avih, frameBytes);
    Put32(avih, static_cast<std::uint32_t>(width));
    Put32(avih, static_cast<std::uint32_t>(height));
    for (int i = 0; i < 4; ++i) {
        Put32(avih, 0);
    }

    std::string strh;
    strh.append("vids", 4);
    strh.append("DIB ", 4);
    Put32(strh, 0);  // flags
    Put16(strh, 0);  // priority
    Put16(strh, 0);  // language
    Put32(strh, 0);  // initial frames
    Put32(strh, 1);  // scale
    Put32(strh, static_cast<std::uint32_t>(fps));  // rate
    Put32(strh, 0);                                // start
    Put32(strh, static_cast<std::uint32_t>(totalFrames));
    Put32(strh, frameBytes);
    Put32(strh, 0xFFFFFFFFu);  // quality: default
    Put32(strh, 0);            // sample size
    Put16(strh, 0);
    Put16(strh, 0);
    Put16(strh, static_cast<std::uint16_t>(width));
    Put16(strh, static_cast<std::uint16_t>(height));

    std::string strf;
    Put32(strf, 40);  // biSize
    Put32(strf, static_cast<std::uint32_t>(width));
    Put32(strf, static_cast<std::uint32_t>(height));  // positive: bottom-up
    Put16(strf, 1);                                   // planes
    Put16(strf, 24);                                  // bit count
    Put32(strf, 0);                                   // BI_RGB
    Put32(strf, frameBytes);
    Put32(strf, 0);
    Put32(strf, 0);
    Put32(strf, 0);
    Put32(strf, 0);

    std::string strl;
    AppendChunk(strl, "strh", strh);
    AppendChunk(strl, "strf", strf);

    std::string hdrl;
    AppendChunk(hdrl, "avih", avih);
    hdrl.append(MakeList("strl", strl));

    std::string idx1;
    AppendChunk(idx1, "idx1", index);

    std::string riffPayload;
    riffPayload.append("AVI ", 4);
    riffPayload.append(MakeList("hdrl", hdrl));
    riffPayload.append(MakeList("movi", movi));
    riffPayload.append(idx1);

    std::string riff;
    riff.append("RIFF", 4);
    Put32(riff, static_cast<std::uint32_t>(riffPayload.size()));
    riff.append(riffPayload);

    std::ofstream file(outputPath, std::ios::binary);
    if (!file) {
        Log("could not create %ls\n", outputPath.c_str());
        Log("result: failed\n");
        return 1;
    }
    file.write(riff.data(), static_cast<std::streamsize>(riff.size()));
    if (!file.good()) {
        Log("write failed\n");
        Log("result: failed\n");
        return 1;
    }

    Log("\nwrote %d frames, %u bytes/frame, avg frame work %.2f ms, file %zu bytes\n", totalFrames, frameBytes,
        frameMs / totalFrames, riff.size());
    Log("output: %ls\n", outputPath.c_str());
    Log("log: %ls\n", logPath.c_str());
    Log("result: ok\n");

    return 0;
}
