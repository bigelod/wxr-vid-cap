// 32-bit DLL injector helper.
//
// The tray app is 64-bit, so a 32-bit target process needs a 32-bit caller for
// CreateRemoteThread(LoadLibraryW). This helper is built for Win32 and shipped next to the app as
// wxr_inject32.exe.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {

bool Inject(unsigned long processId, const std::wstring& dllPath, std::wstring& error)
{
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, static_cast<DWORD>(processId));
    if (process == nullptr) {
        error = L"OpenProcess failed (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr) {
        error = L"VirtualAllocEx failed (error " + std::to_wstring(GetLastError()) + L").";
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        error = L"WriteProcessMemory failed (error " + std::to_wstring(GetLastError()) + L").";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (loadLibrary == nullptr) {
        error = L"Could not resolve LoadLibraryW.";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (thread == nullptr) {
        error = L"CreateRemoteThread failed (error " + std::to_wstring(GetLastError()) + L").";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 15000);

    DWORD module = 0;
    GetExitCodeThread(thread, &module);

    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);

    if (module == 0) {
        error = L"LoadLibraryW returned null in the target process.";
        return false;
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    unsigned long processId = 0;
    std::wstring dllPath;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--pid" && i + 1 < argc) {
            processId = static_cast<unsigned long>(_wtoi(argv[++i]));
        } else if (arg == L"--dll" && i + 1 < argc) {
            dllPath = argv[++i];
        } else {
            std::printf("usage: wxr_inject32 --pid <id> --dll <path>\n");
            return 2;
        }
    }

    if (processId == 0 || dllPath.empty()) {
        std::printf("usage: wxr_inject32 --pid <id> --dll <path>\n");
        return 2;
    }
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("dll not found: %ls\n", dllPath.c_str());
        return 1;
    }

    std::wstring error;
    if (!Inject(processId, dllPath, error)) {
        std::printf("injection failed: %ls\n", error.c_str());
        return 1;
    }

    std::printf("injected pid=%lu dll=%ls\n", processId, dllPath.c_str());
    return 0;
}
