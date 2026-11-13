#include "hook_capture.h"

#include "exclusions.h"
#include "injection.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

std::wstring ExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory(path);
    const std::size_t slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        directory.resize(slash);
    }
    return directory;
}

// Last few kilobytes of the hook log, for on-device diagnostics in a popup.
std::wstring HookLogTail()
{
    const std::wstring path = ExecutableDirectory() + L"\\wxr_hook.log";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.size() > 1200) {
        text = text.substr(text.size() - 1200);
    }
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
    }
    return wide;
}

bool RunHelper(const std::wstring& path, unsigned long processId, const std::wstring& dllPath, std::wstring& error)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"Helper not found: " + path;
        return false;
    }

    std::wstring command = L"\"" + path + L"\" --pid " + std::to_wstring(processId) + L" --dll \"" + dllPath + L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
        error = L"Could not start the 32-bit injector (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    WaitForSingleObject(process.hProcess, 20000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (exitCode != 0) {
        error = L"The 32-bit injector failed (exit code " + std::to_wstring(exitCode) + L").";
        return false;
    }
    return true;
}

void ScaleBgraToFit(const std::uint8_t* source, int sourceWidth, int sourceHeight, std::uint8_t* destination,
                    int width, int height)
{
    std::memset(destination, 0, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    if (sourceWidth <= 0 || sourceHeight <= 0 || width <= 0 || height <= 0) {
        return;
    }

    const double scale = std::min(static_cast<double>(width) / sourceWidth,
                                  static_cast<double>(height) / sourceHeight);
    int outputWidth = static_cast<int>(std::lround(sourceWidth * scale));
    int outputHeight = static_cast<int>(std::lround(sourceHeight * scale));
    outputWidth = std::clamp(outputWidth, 1, width);
    outputHeight = std::clamp(outputHeight, 1, height);
    const int offsetX = (width - outputWidth) / 2;
    const int offsetY = (height - outputHeight) / 2;

    std::vector<int> x0(static_cast<std::size_t>(outputWidth));
    std::vector<int> x1(static_cast<std::size_t>(outputWidth));
    std::vector<int> fx(static_cast<std::size_t>(outputWidth));
    for (int x = 0; x < outputWidth; ++x) {
        double sample = (x + 0.5) * sourceWidth / outputWidth - 0.5;
        sample = std::clamp(sample, 0.0, static_cast<double>(sourceWidth - 1));
        const int base = static_cast<int>(sample);
        x0[static_cast<std::size_t>(x)] = base;
        x1[static_cast<std::size_t>(x)] = std::min(base + 1, sourceWidth - 1);
        fx[static_cast<std::size_t>(x)] = static_cast<int>((sample - base) * 256.0 + 0.5);
    }

    const std::size_t sourceStride = static_cast<std::size_t>(sourceWidth) * 4;
    for (int y = 0; y < outputHeight; ++y) {
        double sample = (y + 0.5) * sourceHeight / outputHeight - 0.5;
        sample = std::clamp(sample, 0.0, static_cast<double>(sourceHeight - 1));
        const int base = static_cast<int>(sample);
        const int wy = static_cast<int>((sample - base) * 256.0 + 0.5);
        const int iwy = 256 - wy;
        const std::uint8_t* row0 = source + static_cast<std::size_t>(base) * sourceStride;
        const std::uint8_t* row1 =
            source + static_cast<std::size_t>(std::min(base + 1, sourceHeight - 1)) * sourceStride;
        std::uint8_t* output = destination +
                               (static_cast<std::size_t>(y + offsetY) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(offsetX)) *
                                   4;

        for (int x = 0; x < outputWidth; ++x) {
            const std::size_t left = static_cast<std::size_t>(x0[static_cast<std::size_t>(x)]) * 4;
            const std::size_t right = static_cast<std::size_t>(x1[static_cast<std::size_t>(x)]) * 4;
            const int wx = fx[static_cast<std::size_t>(x)];
            const int iwx = 256 - wx;
            for (int channel = 0; channel < 4; ++channel) {
                const int top = row0[left + channel] * iwx + row0[right + channel] * wx;
                const int bottom = row1[left + channel] * iwx + row1[right + channel] * wx;
                output[static_cast<std::size_t>(x) * 4 + static_cast<std::size_t>(channel)] =
                    static_cast<std::uint8_t>((top * iwy + bottom * wy) >> 16);
            }
        }
    }
}

}  // namespace

HookCapture::~HookCapture()
{
    Stop();
}

bool HookCapture::Start(const std::wstring& targetExecutable, int width, int height, std::wstring& error,
                        int firstFrameTimeoutMs)
{
    Stop();

    if (targetExecutable.empty()) {
        error = L"No capture target is selected.";
        return false;
    }

    if (IsExcludedExecutablePath(targetExecutable)) {
        error = L"That application is blacklisted from capture: " + targetExecutable;
        return false;
    }

    processId_ = FindProcessByExecutable(targetExecutable);
    if (processId_ == 0) {
        error = L"The capture target is not running: " + targetExecutable;
        return false;
    }

    if (!reader_.Create(processId_)) {
        error = L"Could not create the shared frame mapping.";
        return false;
    }

    width_ = width;
    height_ = height;

    const std::wstring directory = ExecutableDirectory();
    const std::wstring dll64 = directory + L"\\wxr_hook_dll.dll";
    const std::wstring dll32 = directory + L"\\wxr_hook_dll32.dll";
    const std::wstring helper = directory + L"\\wxr_inject32.exe";

    const bool target32 = IsTarget32Bit(processId_);
    std::wstring firstError;
    bool injected = false;

    // Match the target's bitness first, then try the other one. Under Box64 the reported bitness can
    // be wrong, so the fallback is worth the attempt before giving up.
    if (target32) {
        if (RunHelper(helper, processId_, dll32, firstError)) {
            injected = true;
            injectionMethod_ = L"32-bit helper";
        } else {
            std::wstring fallbackError;
            if (InjectDll(processId_, dll64, fallbackError)) {
                injected = true;
                injectionMethod_ = L"64-bit direct (fallback)";
            } else {
                firstError += L"\n\n64-bit fallback: " + fallbackError;
            }
        }
    } else {
        if (InjectDll(processId_, dll64, firstError)) {
            injected = true;
            injectionMethod_ = L"64-bit direct";
        } else {
            std::wstring fallbackError;
            if (RunHelper(helper, processId_, dll32, fallbackError)) {
                injected = true;
                injectionMethod_ = L"32-bit helper (fallback)";
            } else {
                firstError += L"\n\n32-bit fallback: " + fallbackError;
            }
        }
    }

    if (!injected) {
        reader_.Close();
        error = L"Injection failed into " + targetExecutable + L" (PID " + std::to_wstring(processId_) + L").\n\n" +
                firstError;
        const std::wstring tail = HookLogTail();
        if (!tail.empty()) {
            error += L"\n\nwxr_hook.log:\n" + tail;
        }
        return false;
    }

    // Wait for the first frame. The target may need a moment if it was just started.
    std::vector<std::uint8_t> frame;
    int sourceWidth = 0;
    int sourceHeight = 0;
    std::int64_t timestamp = 0;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(firstFrameTimeoutMs);

    while (GetTickCount64() < deadline) {
        if (reader_.Read(frame, sourceWidth, sourceHeight, timestamp)) {
            sourceWidth_ = sourceWidth;
            sourceHeight_ = sourceHeight;
            framesRead_ = 1;
            pending_ = std::move(frame);
            hasPending_ = true;
            active_ = true;
            return true;
        }
    }

    reader_.SetEnabled(false);
    reader_.Close();
    active_ = false;

    error = L"The hook loaded (" + injectionMethod_ + L") but no frames arrived.\n\n";
    error += L"For D3D12 or Vulkan targets, use LAUNCH WITH HOOK so the hook is present before the "
             L"device is created.\n\n";
    const std::wstring tail = HookLogTail();
    if (!tail.empty()) {
        error += L"wxr_hook.log:\n" + tail;
    } else {
        error += L"wxr_hook.log was not created.";
    }
    return false;
}

void HookCapture::Stop()
{
    if (reader_.Active()) {
        reader_.SetEnabled(false);
    }
    reader_.Close();
    active_ = false;
    processId_ = 0;
    sourceWidth_ = 0;
    sourceHeight_ = 0;
    framesRead_ = 0;
    injectionMethod_.clear();
}

bool HookCapture::Capture(std::vector<std::uint8_t>& bgra, int timeoutMs, std::wstring& error)
{
    (void)error;
    if (!active_) {
        return false;
    }

    std::vector<std::uint8_t> frame;
    int sourceWidth = sourceWidth_;
    int sourceHeight = sourceHeight_;

    if (hasPending_) {
        frame = std::move(pending_);
        pending_.clear();
        hasPending_ = false;
    } else {
        std::int64_t timestamp = 0;
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(timeoutMs, 0));
        for (;;) {
            if (reader_.Read(frame, sourceWidth, sourceHeight, timestamp, 1)) {
                break;
            }
            if (GetTickCount64() >= deadline) {
                return false;
            }
        }
    }

    sourceWidth_ = sourceWidth;
    sourceHeight_ = sourceHeight;
    ++framesRead_;

    if (sourceWidth == width_ && sourceHeight == height_) {
        bgra = std::move(frame);
    } else {
        bgra.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4);
        ScaleBgraToFit(frame.data(), sourceWidth, sourceHeight, bgra.data(), width_, height_);
    }
    return true;
}
