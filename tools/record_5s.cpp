// Records a fixed-length clip of a target and exits. With no arguments it records the tray
// setting for 5 seconds and shows a message box (the original HMD test flow). The capture runner
// uses --exe/--seconds/--out/--quiet for unattended runs.

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "sessions.h"
#include "settings.h"

#pragma comment(lib, "shell32.lib")

namespace {

std::filesystem::path ExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(path, length)).parent_path();
}

std::wstring TimestampedName()
{
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    wchar_t name[64] = {};
    swprintf_s(name, L"recording_%04u-%02u-%02u_%02u%02u%02u.mkv", now.wYear, now.wMonth, now.wDay, now.wHour,
               now.wMinute, now.wSecond);
    return name;
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
    if (size > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    }
    return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    std::wstring targetExe;
    std::wstring outputPath;
    int seconds = 5;
    bool quiet = false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--exe" && i + 1 < argc) {
                targetExe = argv[++i];
            } else if (arg == L"--seconds" && i + 1 < argc) {
                seconds = _wtoi(argv[++i]);
            } else if (arg == L"--out" && i + 1 < argc) {
                outputPath = argv[++i];
            } else if (arg == L"--quiet") {
                quiet = true;
            }
        }
        LocalFree(argv);
    }

    if (outputPath.empty()) {
        outputPath = (ExecutableDirectory() / TimestampedName()).wstring();
    }
    if (targetExe.empty()) {
        targetExe = LoadSettings().captureTargetExe;
    }
    if (seconds <= 0) {
        seconds = 5;
    }

    Recorder recorder;
    std::wstring error;
    if (!recorder.Start(outputPath, targetExe, error)) {
        if (quiet) {
            std::ofstream errorFile(std::filesystem::path(outputPath + L".error.log"), std::ios::binary);
            errorFile << Narrow(error) << "\n";
        } else {
            const std::wstring message = L"Could not start recording.\n\n" + error;
            MessageBoxW(nullptr, message.c_str(), L"wxr-vid-cap", MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    if (!quiet) {
        MessageBeep(MB_ICONASTERISK);
    }
    Sleep(static_cast<DWORD>(seconds) * 1000);
    recorder.Stop();
    if (!quiet) {
        MessageBeep(MB_ICONASTERISK);
    }

    if (quiet) {
        return recorder.LastStopWroteFile() ? 0 : 1;
    }

    if (recorder.LastStopWroteFile()) {
        const std::wstring message = L"Recorded " + std::to_wstring(seconds) + L" seconds:\n\n" + outputPath;
        MessageBoxW(nullptr, message.c_str(), L"wxr-vid-cap", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    std::wstring message = L"No recording was written.";
    if (!recorder.LastError().empty()) {
        message += L"\n\n";
        message += recorder.LastError();
    }
    MessageBoxW(nullptr, message.c_str(), L"wxr-vid-cap", MB_OK | MB_ICONWARNING);
    return 1;
}
