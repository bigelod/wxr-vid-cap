// Host test for the graphics hook. Uses the same HookCapture class the recorder uses, so it
// exercises injection, the 32-bit fallback, the shared-memory bridge, and the scaler.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

#include "hook_capture.h"

namespace {

std::wstring ExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path);
    const std::size_t slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        result.resize(slash);
    }
    return result;
}

bool WriteBmp(const std::wstring& path, const std::vector<std::uint8_t>& bgra, int width, int height)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    const std::uint32_t rowBytes = static_cast<std::uint32_t>(width) * 4;
    const std::uint32_t pixelBytes = rowBytes * static_cast<std::uint32_t>(height);
    const std::uint32_t fileSize = 54 + pixelBytes;

    std::uint8_t header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    auto put32 = [&header](std::size_t offset, std::uint32_t value) {
        header[offset + 0] = static_cast<std::uint8_t>(value & 0xFF);
        header[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        header[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        header[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    };
    auto put16 = [&header](std::size_t offset, std::uint16_t value) {
        header[offset + 0] = static_cast<std::uint8_t>(value & 0xFF);
        header[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    };

    put32(2, fileSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(width));
    put32(22, static_cast<std::uint32_t>(height));
    put16(26, 1);
    put16(28, 32);
    put32(34, pixelBytes);

    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    for (int y = height - 1; y >= 0; --y) {
        file.write(reinterpret_cast<const char*>(bgra.data() + static_cast<std::size_t>(y) * width * 4), rowBytes);
    }
    return file.good();
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    int seconds = 5;
    std::wstring executablePath;
    std::wstring outputPath;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--exe" && i + 1 < argc) {
            executablePath = argv[++i];
        } else if (arg == L"--seconds" && i + 1 < argc) {
            seconds = _wtoi(argv[++i]);
        } else if (arg == L"--out" && i + 1 < argc) {
            outputPath = argv[++i];
        } else {
            std::printf("usage: hook_capture_test --exe <name.exe> [--seconds 5] [--out frame.bmp]\n");
            return 2;
        }
    }

    if (executablePath.empty()) {
        std::printf("usage: hook_capture_test --exe <name.exe> [--seconds 5] [--out frame.bmp]\n");
        return 2;
    }
    if (outputPath.empty()) {
        outputPath = ExecutableDirectory() + L"\\hook_frame.bmp";
    }

    HookCapture hook;
    std::wstring error;
    if (!hook.Start(executablePath, 1280, 720, error)) {
        std::printf("hook start failed:\n%ls\n", error.c_str());
        return 1;
    }

    std::printf("injected via %ls into pid %lu, source %dx%d\n", hook.LastInjectionMethod().c_str(),
                hook.ProcessId(), hook.SourceWidth(), hook.SourceHeight());

    std::vector<std::uint8_t> frame;
    int frames = 0;
    const ULONGLONG start = GetTickCount64();

    while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000) {
        if (hook.Capture(frame, 50, error)) {
            if (frames == 0) {
                std::printf("first frame: %dx%d\n", hook.Width(), hook.Height());
                if (!WriteBmp(outputPath, frame, hook.Width(), hook.Height())) {
                    std::printf("could not write %ls\n", outputPath.c_str());
                }
            }
            ++frames;
        }
    }

    hook.Stop();

    const double elapsed = static_cast<double>(GetTickCount64() - start) / 1000.0;
    std::printf("frames=%d in %.2f s (%.1f fps) output=%ls\n", frames, elapsed,
                elapsed > 0.0 ? frames / elapsed : 0.0, outputPath.c_str());
    return frames > 0 ? 0 : 1;
}
