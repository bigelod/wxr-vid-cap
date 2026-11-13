#include "injection.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>

namespace {

std::wstring FileNameOf(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

std::wstring ProcessImagePath(DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return {};
    }

    wchar_t path[MAX_PATH * 4] = {};
    DWORD size = ARRAYSIZE(path);
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        result.assign(path, size);
    }
    CloseHandle(process);
    return result;
}

}  // namespace

unsigned long FindProcessByExecutable(const std::wstring& executablePath)
{
    if (executablePath.empty()) {
        return 0;
    }

    const bool fullPath = executablePath.find_first_of(L"\\/") != std::wstring::npos;
    const std::wstring wantedName = FileNameOf(executablePath);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    unsigned long result = 0;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (!fullPath) {
                if (_wcsicmp(entry.szExeFile, wantedName.c_str()) == 0) {
                    result = entry.th32ProcessID;
                    break;
                }
            } else {
                const std::wstring path = ProcessImagePath(entry.th32ProcessID);
                if (!path.empty() && _wcsicmp(path.c_str(), executablePath.c_str()) == 0) {
                    result = entry.th32ProcessID;
                    break;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

bool InjectDll(unsigned long processId, const std::wstring& dllPath, std::wstring& error)
{
    if (processId == 0 || dllPath.empty()) {
        error = L"Invalid injection parameters.";
        return false;
    }

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

    bool ok = false;
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        error = L"WriteProcessMemory failed (error " + std::to_wstring(GetLastError()) + L").";
    } else {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel, "LoadLibraryW"));
        if (loadLibrary == nullptr) {
            error = L"Could not resolve LoadLibraryW.";
        } else {
            HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
            if (thread == nullptr) {
                error = L"CreateRemoteThread failed (error " + std::to_wstring(GetLastError()) + L").";
            } else {
                WaitForSingleObject(thread, 10000);
                DWORD module = 0;
                GetExitCodeThread(thread, &module);
                CloseHandle(thread);
                if (module == 0) {
                    error = L"LoadLibraryW returned null in the target process.";
                } else {
                    ok = true;
                }
            }
        }
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

bool IsTarget32Bit(unsigned long processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr) {
        return false;
    }

    bool is32 = false;

    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (isWow64Process2 != nullptr) {
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;
        if (isWow64Process2(process, &processMachine, &nativeMachine)) {
            is32 = (processMachine == IMAGE_FILE_MACHINE_I386);
        }
    } else {
        BOOL wow64 = FALSE;
        if (IsWow64Process(process, &wow64)) {
            is32 = (wow64 != FALSE);
        }
    }

    CloseHandle(process);
    return is32;
}
