#include "exclusions.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace {

std::mutex g_mutex;
std::vector<std::wstring> g_entries;
bool g_loaded = false;

std::wstring Lower(std::wstring text)
{
    for (wchar_t& ch : text) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return text;
}

std::wstring Trim(std::wstring text)
{
    const auto isSpace = [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
    };
    while (!text.empty() && isSpace(text.front())) {
        text.erase(text.begin());
    }
    while (!text.empty() && isSpace(text.back())) {
        text.pop_back();
    }
    return text;
}

std::wstring FileNameOf(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

std::wstring ModuleDirectory()
{
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length == 0) {
        return L".";
    }
    std::wstring directory(path, length);
    const std::size_t slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        directory.resize(slash);
    }
    return directory;
}

void LoadLocked()
{
    g_entries = {L"wfm.exe", L"taskmgr.exe", L"explorer.exe", L"notepad.exe"};

    std::ifstream file(std::filesystem::path(ModuleDirectory() + L"\\capture-blacklist.txt"));
    if (!file) {
        return;
    }

    std::vector<std::wstring> loaded;
    std::string line;
    while (std::getline(file, line)) {
        std::wstring wide(line.begin(), line.end());
        wide = Lower(Trim(std::move(wide)));
        if (wide.empty() || wide[0] == L'#') {
            continue;
        }
        loaded.push_back(std::move(wide));
    }

    if (!loaded.empty()) {
        if (std::find(loaded.begin(), loaded.end(), L"wfm.exe") == loaded.end()) {
            loaded.push_back(L"wfm.exe");
        }
        g_entries = std::move(loaded);
    }
}

void EnsureLoadedLocked()
{
    if (!g_loaded) {
        g_loaded = true;
        LoadLocked();
    }
}

}  // namespace

void LoadCaptureBlacklist()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_loaded = true;
    LoadLocked();
}

bool IsExcludedExecutableName(const std::wstring& fileName)
{
    const std::wstring wanted = Lower(fileName);
    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureLoadedLocked();
    for (const std::wstring& entry : g_entries) {
        if (entry.find_first_of(L"\\/") == std::wstring::npos && entry == wanted) {
            return true;
        }
    }
    return false;
}

bool IsExcludedExecutablePath(const std::wstring& path)
{
    const std::wstring wantedPath = Lower(path);
    const std::wstring wantedName = Lower(FileNameOf(path));
    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureLoadedLocked();
    for (const std::wstring& entry : g_entries) {
        if (entry.find_first_of(L"\\/") != std::wstring::npos) {
            if (entry == wantedPath) {
                return true;
            }
        } else if (entry == wantedName) {
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
        excluded = IsExcludedExecutablePath(std::wstring(path, size));
    }
    CloseHandle(process);
    return excluded;
}
