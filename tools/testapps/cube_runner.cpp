// Runs every capture test app in sequence, records a short clip of each through the normal
// Recorder path (hook first, GDI fallback), keeps every MKV, and writes one combined log.
//
// Launch it and leave it alone. At the end it shows a summary and the output folder.

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr int kRecordSeconds = 6;

struct TestCase {
    const wchar_t* name;
    const wchar_t* executable;
};

const TestCase kTests[] = {
    {L"d3d7", L"cube_d3d7.exe"},   {L"d3d8", L"cube_d3d8.exe"},   {L"d3d9", L"cube_d3d9.exe"},
    {L"d3d10", L"cube_d3d10.exe"}, {L"d3d11", L"cube_d3d11.exe"}, {L"d3d12", L"cube_d3d12.exe"},
    {L"gl", L"cube_gl.exe"},       {L"vk", L"cube_vk.exe"},
};

std::filesystem::path ExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(path, length)).parent_path();
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

std::wstring TimestampName()
{
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t name[64] = {};
    swprintf_s(name, L"%04u-%02u-%02u_%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
               now.wSecond);
    return name;
}

void KillProcessesByNamePrefix(const std::wstring& prefix)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring name = entry.szExeFile;
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                continue;
            }
            if (name.size() >= prefix.size() && _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) == 0) {
                HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                if (process != nullptr) {
                    TerminateProcess(process, 0);
                    CloseHandle(process);
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

DWORD FindProcessIdByName(const std::wstring& name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DWORD result = 0;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name.c_str()) == 0) {
                result = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

struct WindowProbe {
    HWND main = nullptr;
    bool rendering = false;
    std::wstring dialogText;
};

BOOL CALLBACK CollectChildText(HWND child, LPARAM parameter)
{
    wchar_t className[64] = {};
    GetClassNameW(child, className, 64);
    if (_wcsicmp(className, L"Static") == 0) {
        wchar_t text[1024] = {};
        GetWindowTextW(child, text, 1024);
        if (text[0] != L'\0') {
            auto* result = reinterpret_cast<std::wstring*>(parameter);
            if (!result->empty()) {
                *result += L' ';
            }
            *result += text;
        }
    }
    return TRUE;
}

struct ProbeContext {
    DWORD processId = 0;
    WindowProbe* probe = nullptr;
};

BOOL CALLBACK ProbeWindowForProcess(HWND window, LPARAM parameter)
{
    auto* context = reinterpret_cast<ProbeContext*>(parameter);

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != context->processId) {
        return TRUE;
    }

    wchar_t className[64] = {};
    GetClassNameW(window, className, 64);

    wchar_t title[512] = {};
    GetWindowTextW(window, title, 512);

    if (_wcsicmp(className, L"#32770") == 0) {
        std::wstring text;
        EnumChildWindows(window, CollectChildText, reinterpret_cast<LPARAM>(&text));
        if (!text.empty()) {
            context->probe->dialogText = text;
        }
        return TRUE;
    }

    if (wcsncmp(title, L"wxr test", 8) == 0) {
        context->probe->main = window;
        if (wcsstr(title, L" fps") != nullptr) {
            context->probe->rendering = true;
        }
    }
    return TRUE;
}

WindowProbe ProbeAppWindow(DWORD processId)
{
    WindowProbe probe;
    ProbeContext context;
    context.processId = processId;
    context.probe = &probe;
    EnumWindows(ProbeWindowForProcess, reinterpret_cast<LPARAM>(&context));
    return probe;
}

bool StartProcess(const std::wstring& commandLine, PROCESS_INFORMATION& process)
{
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    process = {};

    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');

    return CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) !=
           FALSE;
}

bool WaitForExit(HANDLE process, DWORD timeoutMs, DWORD& exitCode)
{
    const DWORD wait = WaitForSingleObject(process, timeoutMs);
    exitCode = 1;
    GetExitCodeProcess(process, &exitCode);
    return wait == WAIT_OBJECT_0;
}

std::string ReadLastLineStartingWith(const std::filesystem::path& path, const std::string& prefix)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }

    std::string line;
    std::string found;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind(prefix, 0) == 0) {
            found = line;
        }
    }
    return found;
}

std::string TrimPrefix(const std::string& line, const std::string& prefix)
{
    if (line.rfind(prefix, 0) == 0) {
        return line.substr(prefix.size());
    }
    return line;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    const std::filesystem::path directory = ExecutableDirectory();
    const std::filesystem::path outputDirectory = directory / L"cube_tests" / TimestampName();
    std::error_code error;
    std::filesystem::create_directories(outputDirectory, error);

    const std::filesystem::path logPath = outputDirectory / L"cube_tests.log";
    std::ofstream log(logPath, std::ios::binary);

    auto logLine = [&log](const std::string& line) {
        if (log) {
            log << line << "\n";
            log.flush();
        }
    };

    logLine("wxr-vid-cap capture test run");
    logLine("output: " + Narrow(outputDirectory.wstring()));
    logLine("");

    // Start from a clean slate: no leftover test apps from an earlier run.
    KillProcessesByNamePrefix(L"cube_");
    Sleep(500);

    const std::filesystem::path recorderPath = directory / L"record_5s.exe";
    if (!std::filesystem::exists(recorderPath)) {
        logLine("error: record_5s.exe is not next to cube_runner.exe");
        MessageBoxW(nullptr, L"record_5s.exe must sit next to cube_runner.exe.", L"wxr capture runner",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    int succeeded = 0;
    int failed = 0;
    int warnings = 0;

    struct RunMode {
        const wchar_t* name;
        const wchar_t* argument;
        bool x86;
    };
    const RunMode kModes[] = {
        {L"windowed64", L"", false},
        {L"windowed32", L"", true},
        {L"fullscreen64", L"--fullscreen", false},
        {L"fullscreen32", L"--fullscreen", true},
    };

    for (const RunMode& mode : kModes) {
        logLine("");
        logLine("## " + Narrow(mode.name));

        for (const TestCase& test : kTests) {
            const std::wstring executableName(test.executable);
            const std::wstring x86Name = executableName.substr(0, executableName.size() - 4) + L"32.exe";
            const std::filesystem::path appPath = directory / (mode.x86 ? x86Name : executableName);
            const std::filesystem::path clipPath =
                outputDirectory / (std::wstring(test.name) + L"_" + mode.name + L".mkv");
            const std::wstring label(test.name);

            logLine("== " + Narrow(label) + " (" + Narrow(appPath.filename().wstring()) + ", " +
                    Narrow(mode.name) + ")");

        if (!std::filesystem::exists(appPath)) {
            logLine("  FAILED: executable not found");
            ++failed;
            continue;
        }

        PROCESS_INFORMATION appProcess = {};
        DWORD appPid = 0;
        HANDLE appHandle = nullptr;

        // Only D3D12 and Vulkan need the hook before the app creates its device. D3D9/D3D10/D3D11
        // and OpenGL are injected while running, which avoids creating a dummy D3D11 device inside a
        // suspended process (that hangs under Wine).
        const bool needsLaunchInjection =
            (wcscmp(test.name, L"d3d12") == 0 || wcscmp(test.name, L"vk") == 0);
        const std::filesystem::path launcherPath =
            directory / (mode.x86 ? L"wxr_launch32.exe" : L"wxr_launch.exe");
        if (needsLaunchInjection && std::filesystem::exists(launcherPath)) {
            PROCESS_INFORMATION launcherProcess = {};
            std::wstring launcherCommand =
                L"\"" + launcherPath.wstring() + L"\" --exe \"" + appPath.wstring() + L"\"";
            if (mode.argument[0] != L'\0') {
                launcherCommand += L" --args \"" + std::wstring(mode.argument) + L"\"";
            }
            if (!StartProcess(launcherCommand, launcherProcess)) {
                logLine("  FAILED: could not start wxr_launch.exe (error " + std::to_string(GetLastError()) + ")");
                ++failed;
                continue;
            }
            DWORD launcherExit = 1;
            WaitForExit(launcherProcess.hProcess, 20000, launcherExit);
            CloseHandle(launcherProcess.hThread);
            CloseHandle(launcherProcess.hProcess);

            for (int attempt = 0; attempt < 60 && appPid == 0; ++attempt) {
                appPid = FindProcessIdByName(appPath.filename().wstring());
                if (appPid == 0) {
                    Sleep(100);
                }
            }
            if (appPid == 0) {
                logLine("  FAILED: the launcher did not start the app");
                ++failed;
                continue;
            }
            appHandle =
                OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, appPid);
        } else {
            std::wstring directCommand = L"\"" + appPath.wstring() + L"\"";
            if (mode.argument[0] != L'\0') {
                directCommand += L" " + std::wstring(mode.argument);
            }
            if (!StartProcess(directCommand, appProcess)) {
                logLine("  FAILED: could not start the app (error " + std::to_string(GetLastError()) + ")");
                ++failed;
                continue;
            }
            appPid = appProcess.dwProcessId;
            appHandle = appProcess.hProcess;
        }

        auto stopApp = [&]() {
            if (appHandle != nullptr) {
                TerminateProcess(appHandle, 0);
                CloseHandle(appHandle);
                appHandle = nullptr;
            }
            if (appProcess.hThread != nullptr) {
                CloseHandle(appProcess.hThread);
                appProcess.hThread = nullptr;
            }
            appProcess.hProcess = nullptr;
        };

        // Wait for the app to report a frame rate, or for an error dialog. Under Box64/DXVK,
        // device creation and shader compilation can take tens of seconds on the first run.
        WindowProbe probe;
        const ULONGLONG readyDeadline = GetTickCount64() + 45000;
        while (GetTickCount64() < readyDeadline) {
            probe = ProbeAppWindow(appPid);
            if (probe.rendering || !probe.dialogText.empty()) {
                break;
            }
            Sleep(200);
        }

        auto logAppTail = [&]() {
            const std::filesystem::path appLog = directory / L"testapp.log";
            std::ifstream file(appLog, std::ios::binary);
            if (!file) {
                return;
            }
            std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (text.size() > 1500) {
                text = text.substr(text.size() - 1500);
            }
            std::size_t start = 0;
            while (start < text.size()) {
                std::size_t end = text.find('\n', start);
                if (end == std::string::npos) {
                    end = text.size();
                }
                std::string line = text.substr(start, end - start);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    logLine("  app: " + line);
                }
                start = end + 1;
            }
        };

        if (!probe.dialogText.empty()) {
            logLine("  FAILED: " + Narrow(probe.dialogText));
            logAppTail();
            stopApp();
            ++failed;
            continue;
        }
        if (!probe.rendering) {
            DWORD exitCode = 0;
            if (appHandle != nullptr && GetExitCodeProcess(appHandle, &exitCode) && exitCode != STILL_ACTIVE) {
                logLine("  FAILED: the app exited with code " + std::to_string(exitCode));
            } else {
                logLine("  FAILED: no render window after 45 s");
            }
            logAppTail();
            stopApp();
            ++failed;
            continue;
        }

        logLine("  app running, recording " + std::to_string(kRecordSeconds) + " s");

        PROCESS_INFORMATION recorderProcess = {};
        const std::wstring recorderCommand = L"\"" + recorderPath.wstring() + L"\" --exe \"" +
                                             appPath.filename().wstring() + L"\" --seconds " +
                                             std::to_wstring(kRecordSeconds) + L" --out \"" + clipPath.wstring() +
                                             L"\" --quiet";
        if (!StartProcess(recorderCommand, recorderProcess)) {
            logLine("  FAILED: could not start record_5s.exe (error " + std::to_string(GetLastError()) + ")");
            stopApp();
            ++failed;
            continue;
        }

        DWORD recorderExit = 1;
        const bool recorderFinished = WaitForExit(recorderProcess.hProcess, 45000, recorderExit);
        if (!recorderFinished) {
            logLine("  FAILED: recording timed out");
            TerminateProcess(recorderProcess.hProcess, 0);
        }
        CloseHandle(recorderProcess.hThread);
        CloseHandle(recorderProcess.hProcess);

        // Stop the app before reading its log.
        stopApp();
        Sleep(400);

        const std::filesystem::path clipLogPath = clipPath.wstring() + L".log";
        const std::string backend = ReadLastLineStartingWith(clipLogPath, "backend:");
        const std::string hook = ReadLastLineStartingWith(clipLogPath, "hook:");
        const std::string content = ReadLastLineStartingWith(clipLogPath, "content:");
        const std::string frames = ReadLastLineStartingWith(clipLogPath, "frames:");
        const std::string result = ReadLastLineStartingWith(clipLogPath, "result:");
        const std::string errorLine = ReadLastLineStartingWith(clipLogPath, "error:");

        const bool wroteFile = std::filesystem::exists(clipPath);
        if (!recorderFinished || recorderExit != 0 || !wroteFile) {
            logLine("  FAILED: recorder exit " + std::to_string(recorderExit) +
                    (wroteFile ? "" : ", no video written"));
            if (!errorLine.empty()) {
                logLine("  " + errorLine);
            }
            ++failed;
            continue;
        }

        const bool black = content.find("black") != std::string::npos;
        std::string summary = black ? "  WARN" : "  ok";
        if (!backend.empty()) {
            summary += "  " + TrimPrefix(backend, "backend:");
        }
        if (!hook.empty()) {
            summary += "  " + TrimPrefix(hook, "hook:");
        }
        if (!content.empty()) {
            summary += "  " + TrimPrefix(content, "content:");
        }
        if (!frames.empty()) {
            summary += "  " + TrimPrefix(frames, "frames:");
        }
        summary += "  video=" + Narrow(clipPath.filename().wstring());
        logLine(summary);
        if (black) {
            ++warnings;
        }
        if (!result.empty() && result != "result: ok") {
            logLine("  " + result);
        }
        ++succeeded;
        }
    }

    logLine("");
    logLine("summary: " + std::to_string(succeeded) + " ok (" + std::to_string(warnings) + " black), " +
            std::to_string(failed) + " failed");
    logLine("videos: " + Narrow(outputDirectory.wstring()));

    const std::wstring message = L"Capture test run finished.\n\n" + std::to_wstring(succeeded) + L" ok (" +
                                 std::to_wstring(warnings) + L" black), " + std::to_wstring(failed) +
                                 L" failed.\n\nLog:\n" + logPath.wstring() + L"\n\nVideos:\n" +
                                 outputDirectory.wstring();
    MessageBoxW(nullptr, message.c_str(), L"wxr capture runner",
                MB_OK | (failed == 0 ? MB_ICONINFORMATION : MB_ICONWARNING));
    return failed == 0 ? 0 : 1;
}
