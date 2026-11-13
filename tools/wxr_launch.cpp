// Starts a target suspended, injects the capture hook, waits for it to install its hooks, then
// resumes. Used for D3D12 and Vulkan, where the hook must be present before the app creates its
// device and swapchain.
//
//   wxr_launch.exe --exe "C:\path\to\game.exe"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <filesystem>
#include <fstream>
#include <string>

#pragma comment(lib, "shell32.lib")

namespace {

std::wstring ExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0) {
        return std::filesystem::current_path().wstring();
    }
    return std::filesystem::path(std::wstring(path, length)).parent_path().wstring();
}

bool InjectDll(HANDLE process, const std::wstring& dllPath, std::wstring& error)
{
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr) {
        error = L"VirtualAllocEx failed (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        error = L"WriteProcessMemory failed (error " + std::to_wstring(GetLastError()) + L").";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (loadLibrary == nullptr) {
        error = L"Could not resolve LoadLibraryW.";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (thread == nullptr) {
        error = L"CreateRemoteThread failed (error " + std::to_wstring(GetLastError()) + L").";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(thread, 15000);
    DWORD module = 0;
    GetExitCodeThread(thread, &module);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);

    if (module == 0) {
        error = L"LoadLibraryW returned null in the target process.";
        return false;
    }
    return true;
}

bool IsTarget32Bit(HANDLE process)
{
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (isWow64Process2 != nullptr) {
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;
        if (isWow64Process2(process, &processMachine, &nativeMachine)) {
            return processMachine == IMAGE_FILE_MACHINE_I386;
        }
    }
    BOOL wow64 = FALSE;
    return IsWow64Process(process, &wow64) && wow64 != FALSE;
}

bool g_quiet = false;

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

int ReportFailure(const std::wstring& message)
{
    if (g_quiet) {
        std::ofstream log(ExecutableDirectory() + L"\\wxr_launch.log", std::ios::app);
        log << Narrow(message) << "\n";
    } else {
        MessageBoxW(nullptr, message.c_str(), L"wxr launch", MB_OK | MB_ICONERROR);
    }
    return 1;
}

}  // namespace

int wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    std::wstring target;
    std::wstring arguments;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--exe" && i + 1 < argc) {
                target = argv[++i];
            } else if (arg == L"--args" && i + 1 < argc) {
                arguments = argv[++i];
            } else if (arg == L"--quiet") {
                g_quiet = true;
            }
        }
        LocalFree(argv);
    }

    if (target.empty() || !std::filesystem::exists(target)) {
        return ReportFailure(L"Usage: wxr_launch.exe --exe \"C:\\path\\to\\game.exe\" [--args \"...\"]");
    }

    const std::wstring directory = ExecutableDirectory();
#ifdef _WIN64
    const std::wstring dllPath = directory + L"\\wxr_hook_dll.dll";
#else
    const std::wstring dllPath = directory + L"\\wxr_hook_dll32.dll";
#endif
    if (!std::filesystem::exists(dllPath)) {
        return ReportFailure(L"The capture hook DLL must sit next to the launcher.");
    }

    std::wstring commandLine = L"\"" + target + L"\"";
    if (!arguments.empty()) {
        commandLine += L" " + arguments;
    }

    const std::filesystem::path workingDirectory = std::filesystem::path(target).parent_path();

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};

    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        workingDirectory.c_str(), &startup, &process)) {
        const std::wstring message = L"Could not start the target (error " + std::to_wstring(GetLastError()) + L").";
        return ReportFailure(message);
    }

    const bool target32 = IsTarget32Bit(process.hProcess);
#ifdef _WIN64
    const bool compatible = !target32;
    const wchar_t* expected = L"64-bit";
#else
    const bool compatible = target32;
    const wchar_t* expected = L"32-bit";
#endif
    if (!compatible) {
        std::wstring message = L"This launcher can only inject the ";
        message += expected;
        message += L" hook. The target is the other bitness.";
        TerminateProcess(process.hProcess, 0);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return ReportFailure(message);
    }

    wchar_t eventName[64] = {};
    swprintf_s(eventName, L"Local\\wxr_hook_ready_%lu", process.dwProcessId);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName);

    std::wstring error;
    if (!InjectDll(process.hProcess, dllPath, error)) {
        const std::wstring message = L"Could not inject the capture hook.\n\n" + error;
        if (ready != nullptr) {
            CloseHandle(ready);
        }
        TerminateProcess(process.hProcess, 0);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return ReportFailure(message);
    }

    if (ready != nullptr) {
        WaitForSingleObject(ready, 20000);
        CloseHandle(ready);
    }

    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
