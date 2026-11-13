// Test harness for the shipping Recorder class. Uses the same code path as START RECORD in the
// tray, without the UI, so threading and finalize behavior can be checked.

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "sessions.h"
#include "settings.h"

namespace {

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

int wmain(int argc, wchar_t** argv)
{
    int seconds = 5;
    std::wstring output;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--seconds" && i + 1 < argc) {
            seconds = _wtoi(argv[++i]);
        } else if (arg == L"--out" && i + 1 < argc) {
            output = argv[++i];
        } else {
            std::printf("usage: recorder_test [--seconds 5] [--out recorder_test.mkv]\n");
            return 2;
        }
    }
    if (seconds <= 0) {
        return 2;
    }

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::filesystem::path exeDirectory = std::filesystem::path(exePath).parent_path();
    std::filesystem::path outputPath =
        output.empty() ? (exeDirectory / L"recorder_test.mkv") : std::filesystem::path(output);
    if (outputPath.is_relative()) {
        outputPath = exeDirectory / outputPath;
    }

    const std::filesystem::path logPath = (outputPath.parent_path().empty() ? exeDirectory : outputPath.parent_path()) /
                                          L"recorder_test_log.txt";
    std::ofstream log(logPath, std::ios::binary);
    auto write = [&log](const std::string& text) {
        std::printf("%s", text.c_str());
        if (log) {
            log << text;
            log.flush();
        }
    };

    Recorder recorder;
    std::wstring error;
    const AppSettings settings = LoadSettings();
    if (!recorder.Start(outputPath.wstring(), settings.captureTargetExe, error)) {
        write("start failed: " + Narrow(error) + "\n");
        return 1;
    }

    write("recording for " + std::to_string(seconds) + " s\n");
    Sleep(static_cast<DWORD>(seconds) * 1000);
    recorder.Stop();

    const bool wrote = recorder.LastStopWroteFile();
    write(std::string("result: ") + (wrote ? "ok" : "no file") + "\n");
    if (!recorder.LastError().empty()) {
        write("error: " + Narrow(recorder.LastError()) + "\n");
    }
    write("output: " + Narrow(outputPath.wstring()) + "\n");

    return wrote ? 0 : 1;
}
