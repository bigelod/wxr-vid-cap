#include "process_list.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <unordered_map>
#include <utility>

#include "exclusions.h"

namespace {

struct BestWindow {
    HWND window = nullptr;
    std::wstring title = L"no title";
    long area = 0;
    long titleArea = 0;
};

using BestWindowMap = std::unordered_map<DWORD, BestWindow>;

void ConsiderWindow(HWND window, BestWindowMap& best)
{
    if (!IsWindowVisible(window)) {
        return;
    }

    wchar_t className[64] = {};
    GetClassNameW(window, className, ARRAYSIZE(className));
    if (std::wcscmp(className, L"Progman") == 0 || std::wcscmp(className, L"WorkerW") == 0 ||
        std::wcscmp(className, L"Shell_TrayWnd") == 0 || std::wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) {
        return;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0 || processId == GetCurrentProcessId()) {
        return;
    }

    RECT client = {};
    if (!GetClientRect(window, &client)) {
        return;
    }
    const long width = client.right - client.left;
    const long height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    const long area = width * height;

    std::wstring title = L"no title";
    const int length = GetWindowTextLengthW(window);
    if (length > 0) {
        std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(window, buffer.data(), length + 1);
        if (copied > 0) {
            buffer.resize(static_cast<std::size_t>(copied));
            title = std::move(buffer);
        }
    }

    BestWindow& entry = best[processId];
    if (entry.window == nullptr || area > entry.area) {
        entry.window = window;
        entry.area = area;
    }
    if (title != L"no title" && (entry.titleArea == 0 || area > entry.titleArea)) {
        entry.title = std::move(title);
        entry.titleArea = area;
    }
}

BOOL CALLBACK ChildWindowProc(HWND window, LPARAM context)
{
    ConsiderWindow(window, *reinterpret_cast<BestWindowMap*>(context));
    return TRUE;
}

BOOL CALLBACK TopLevelWindowProc(HWND window, LPARAM context)
{
    auto& best = *reinterpret_cast<BestWindowMap*>(context);
    ConsiderWindow(window, best);
    EnumChildWindows(window, ChildWindowProc, context);
    return TRUE;
}

std::wstring ProcessImagePath(DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return L"unknown";
    }

    wchar_t path[MAX_PATH * 4] = {};
    DWORD size = ARRAYSIZE(path);
    std::wstring result = L"unknown";
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        result.assign(path, size);
    }
    CloseHandle(process);
    return result;
}

std::wstring FileNameOf(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

}  // namespace

std::vector<ProcessEntry> ListWindowProcesses()
{
    BestWindowMap best;
    EnumWindows(TopLevelWindowProc, reinterpret_cast<LPARAM>(&best));

    std::vector<ProcessEntry> entries;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W process = {};
        process.dwSize = sizeof(process);
        if (Process32FirstW(snapshot, &process)) {
            do {
                const auto found = best.find(process.th32ProcessID);
                if (found == best.end()) {
                    continue;
                }

                ProcessEntry entry;
                entry.processId = process.th32ProcessID;
                entry.executablePath = ProcessImagePath(process.th32ProcessID);
                if (IsExcludedExecutablePath(entry.executablePath)) {
                    continue;
                }
                entry.windowTitle = found->second.title;
                entry.window = found->second.window;
                entry.windowArea = found->second.area;
                entries.push_back(std::move(entry));
            } while (Process32NextW(snapshot, &process));
        }
        CloseHandle(snapshot);
    }

    std::sort(entries.begin(), entries.end(), [](const ProcessEntry& a, const ProcessEntry& b) {
        const std::wstring aName = FileNameOf(a.executablePath);
        const std::wstring bName = FileNameOf(b.executablePath);
        if (_wcsicmp(aName.c_str(), bName.c_str()) != 0) {
            return _wcsicmp(aName.c_str(), bName.c_str()) < 0;
        }
        if (a.windowTitle != b.windowTitle) {
            return a.windowTitle < b.windowTitle;
        }
        return a.processId < b.processId;
    });

    return entries;
}

std::wstring TopMostWindowExecutable()
{
    const std::vector<ProcessEntry> entries = ListWindowProcesses();
    if (entries.empty()) {
        return {};
    }

    DWORD foregroundPid = 0;
    if (HWND foreground = GetForegroundWindow()) {
        GetWindowThreadProcessId(foreground, &foregroundPid);
    }
    if (foregroundPid != 0 && foregroundPid != GetCurrentProcessId()) {
        for (const ProcessEntry& entry : entries) {
            if (entry.processId == foregroundPid && !IsIconic(entry.window)) {
                return entry.executablePath;
            }
        }
    }

    // Wine may put the game surface inside a virtual desktop, so the foreground window is not
    // always the app the user is looking at. The largest non-minimized window is the best guess.
    const ProcessEntry* best = nullptr;
    for (const ProcessEntry& entry : entries) {
        if (entry.window == nullptr || IsIconic(entry.window)) {
            continue;
        }
        if (best == nullptr || entry.windowArea > best->windowArea) {
            best = &entry;
        }
    }
    return (best != nullptr) ? best->executablePath : std::wstring();
}
