// Injection probe host. Injects inject_probe_dll.dll into a target process and checks for the .hit
// marker. Proves whether CreateRemoteThread + LoadLibrary works under the target Wine/Box64 build.

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {

DWORD FindProcessByName(const std::wstring& executableName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DWORD processId = 0;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, executableName.c_str()) == 0) {
                processId = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return processId;
}

std::wstring ProbeDllPath()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path);
    const std::size_t slash = result.find_last_of(L"\\/");
    result.resize(slash + 1);
#ifdef _WIN64
    result += L"inject_probe_dll.dll";
#else
    result += L"inject_probe_dll32.dll";
#endif
    return result;
}

std::wstring MarkerPath()
{
    std::wstring marker = ProbeDllPath();
    const std::size_t dot = marker.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        marker.resize(dot);
    }
    marker += L".hit";
    return marker;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    DWORD processId = 0;
    std::wstring executableName;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--pid" && i + 1 < argc) {
            processId = static_cast<DWORD>(_wtoi(argv[++i]));
        } else if (arg == L"--exe" && i + 1 < argc) {
            executableName = argv[++i];
        } else {
            std::printf("usage: inject_probe --pid <id> | --exe <name.exe>\n");
            return 2;
        }
    }

    if (processId == 0 && !executableName.empty()) {
        processId = FindProcessByName(executableName);
    }
    if (processId == 0) {
        std::printf("target process not found\n");
        return 1;
    }

    const std::wstring dllPath = ProbeDllPath();
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("probe DLL not found next to the host: %ls\n", dllPath.c_str());
        return 1;
    }

    DeleteFileW(MarkerPath().c_str());

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, processId);
    if (process == nullptr) {
        std::printf("OpenProcess failed (error %lu)\n", GetLastError());
        return 1;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr) {
        std::printf("VirtualAllocEx failed (error %lu)\n", GetLastError());
        CloseHandle(process);
        return 1;
    }

    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        std::printf("WriteProcessMemory failed (error %lu)\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel, "LoadLibraryW"));
    if (loadLibrary == nullptr) {
        std::printf("could not resolve LoadLibraryW\n");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (thread == nullptr) {
        std::printf("CreateRemoteThread failed (error %lu)\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    WaitForSingleObject(thread, 10000);

    DWORD module = 0;
    GetExitCodeThread(thread, &module);

    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);

    Sleep(300);
    const DWORD attributes = GetFileAttributesW(MarkerPath().c_str());
    std::printf("pid=%lu remote_module=0x%08lX marker=%s\n", processId, module,
                attributes == INVALID_FILE_ATTRIBUTES ? "missing" : "present");

    return attributes == INVALID_FILE_ATTRIBUTES ? 1 : 0;
}
