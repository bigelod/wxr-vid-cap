#include "exclusions.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwchar>

bool IsExcludedExecutableName(const std::wstring& fileName)
{
    static const wchar_t* const kExcluded[] = {L"wfm.exe"};
    for (const wchar_t* name : kExcluded) {
        if (_wcsicmp(fileName.c_str(), name) == 0) {
            return true;
        }
    }
    return false;
}

bool IsExcludedProcess(unsigned long processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr) {
        return false;
    }

    wchar_t path[MAX_PATH * 4] = {};
    DWORD size = ARRAYSIZE(path);
    bool excluded = false;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const wchar_t* name = path;
        if (const wchar_t* slash = std::wcsrchr(path, L'\\')) {
            name = slash + 1;
        }
        excluded = IsExcludedExecutableName(name);
    }
    CloseHandle(process);
    return excluded;
}
