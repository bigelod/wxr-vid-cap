// wxr-vid-cap - tray entry point.
//
// The tray menu owns recording, casting, target selection, launch-with-hook, and the debug capture
// diagnostic. Only one mode runs at a time; STOP finalizes the file.

#include "sessions.h"

#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "winmm.lib")

#include "hook_capture.h"
#include "exclusions.h"
#include "injection.h"
#include "process_list.h"
#include "resource.h"
#include "settings.h"

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowMenuMessage = WM_APP + 2;
constexpr UINT kTaskmgrShownMessage = WM_APP + 3;
constexpr UINT_PTR kTrayIconId = 1;

constexpr UINT kMenuShow = 40001;
constexpr UINT kMenuRecordLocal = 40002;
constexpr UINT kMenuCastLocal = 40003;
constexpr UINT kMenuStopRecording = 40004;
constexpr UINT kMenuStopCasting = 40005;
constexpr UINT kMenuExit = 40006;
constexpr UINT kMenuSelectCastSource = 40007;

constexpr UINT_PTR kSessionTimerId = 1;
constexpr ULONGLONG kMaxRecordingMs = 15ull * 60ull * 1000ull;
constexpr int kLaunchHookTimeoutMs = 30000;
constexpr int kLateHookTimeoutMs = 30000;

const wchar_t kWindowClass[] = L"WxrVidCapTrayWindow";

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND mainWindow = nullptr;
    HICON icon = nullptr;
    HICON trayIcon = nullptr;
    HANDLE instanceMutex = nullptr;
    UINT taskbarCreatedMessage = 0;
    HWINEVENTHOOK taskmgrShowHook = nullptr;
    HWINEVENTHOOK taskmgrForegroundHook = nullptr;
    ULONGLONG recordingStart = 0;
    AppSettings settings;
    SessionMode mode = SessionMode::Idle;
    Recorder recorder;
    Caster caster;
};

AppState g_app;

struct CaptureSource {
    enum class Kind { TopMost, Running, Launch };
    Kind kind = Kind::TopMost;
    std::wstring executable;
};

void BeginRecordLocal();
void BeginCastLocal();
void BeginSelectCastSource();
void StopActiveSession(bool exiting = false);
void UpdateMainWindow();
void ExitApplication();
bool PromptForCaptureSource(CaptureSource& source);
HWND DialogOwner();

std::vector<std::uint8_t> g_recordStartSound;
std::vector<std::uint8_t> g_recordStopSound;
std::vector<std::uint8_t> g_hookFailureSound;

// Builds a short 16-bit mono WAV. Notes are (frequencyHz, durationMs); frequency 0 is silence.
void BuildBeepWav(std::vector<std::uint8_t>& wav, const std::vector<std::pair<double, int>>& notes,
                  double amplitude)
{
    constexpr int kSampleRate = 44100;
    constexpr double kPi = 3.14159265358979323846;

    std::size_t totalSamples = 0;
    for (const auto& note : notes) {
        totalSamples += static_cast<std::size_t>(kSampleRate) * static_cast<std::size_t>(note.second) / 1000;
    }

    const std::uint32_t dataBytes = static_cast<std::uint32_t>(totalSamples * 2);
    wav.resize(44 + dataBytes);
    std::uint8_t* p = wav.data();

    const auto put16 = [](std::uint8_t* d, std::uint16_t v) {
        d[0] = static_cast<std::uint8_t>(v & 0xFF);
        d[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    };
    const auto put32 = [](std::uint8_t* d, std::uint32_t v) {
        d[0] = static_cast<std::uint8_t>(v & 0xFF);
        d[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        d[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        d[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    };

    p[0] = 'R';
    p[1] = 'I';
    p[2] = 'F';
    p[3] = 'F';
    put32(p + 4, 36 + dataBytes);
    p[8] = 'W';
    p[9] = 'A';
    p[10] = 'V';
    p[11] = 'E';
    p[12] = 'f';
    p[13] = 'm';
    p[14] = 't';
    p[15] = ' ';
    put32(p + 16, 16);
    put16(p + 20, 1);
    put16(p + 22, 1);
    put32(p + 24, kSampleRate);
    put32(p + 28, kSampleRate * 2);
    put16(p + 32, 2);
    put16(p + 34, 16);
    p[36] = 'd';
    p[37] = 'a';
    p[38] = 't';
    p[39] = 'a';
    put32(p + 40, dataBytes);

    std::uint8_t* samples = p + 44;
    std::size_t offset = 0;
    for (const auto& note : notes) {
        const std::size_t count =
            static_cast<std::size_t>(kSampleRate) * static_cast<std::size_t>(note.second) / 1000;
        const double frequency = note.first;
        const double duration = static_cast<double>(count) / kSampleRate;
        const double attack = std::min(0.008, duration * 0.25);
        const double release = std::min(0.025, duration * 0.35);

        for (std::size_t i = 0; i < count; ++i) {
            const double t = static_cast<double>(i) / kSampleRate;
            double envelope = 1.0;
            if (t < attack) {
                envelope = 0.5 - 0.5 * std::cos(kPi * t / attack);
            } else if (t > duration - release) {
                envelope = 0.5 - 0.5 * std::cos(kPi * (duration - t) / release);
            }

            double value = 0.0;
            if (frequency > 0.0) {
                // Soft sine with a touch of second harmonic, so it survives tiny speakers.
                value = std::sin(2.0 * kPi * frequency * t) * amplitude;
                value += 0.15 * std::sin(4.0 * kPi * frequency * t) * amplitude;
                value *= envelope;
            }

            const int sample = static_cast<int>(std::lround(std::clamp(value, -1.0, 1.0) * 32767.0));
            samples[offset * 2] = static_cast<std::uint8_t>(sample & 0xFF);
            samples[offset * 2 + 1] = static_cast<std::uint8_t>((sample >> 8) & 0xFF);
            ++offset;
        }
    }
}

void PlayBeep(const std::vector<std::uint8_t>& wav, bool synchronous)
{
    if (wav.empty()) {
        return;
    }
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr,
               SND_MEMORY | (synchronous ? SND_SYNC : SND_ASYNC) | SND_NODEFAULT);
}

void PlayRecordingStartSound()
{
    if (g_recordStartSound.empty()) {
        // Low to medium: two soft beeps.
        BuildBeepWav(g_recordStartSound, {{440.0, 90}, {0.0, 30}, {660.0, 140}}, 0.18);
    }
    PlayBeep(g_recordStartSound, false);
}

void PlayRecordingStopSound(bool synchronous = false)
{
    if (g_recordStopSound.empty()) {
        // High to medium: two soft beeps.
        BuildBeepWav(g_recordStopSound, {{1046.5, 90}, {0.0, 30}, {660.0, 140}}, 0.18);
    }
    PlayBeep(g_recordStopSound, synchronous);
}

void PlayHookFailureSound()
{
    if (g_hookFailureSound.empty()) {
        // Two short low "dun dun" beeps: the graphics hook could not start.
        BuildBeepWav(g_hookFailureSound, {{220.0, 100}, {0.0, 40}, {165.0, 140}}, 0.16);
    }
    PlayBeep(g_hookFailureSound, false);
}

std::wstring Trim(std::wstring text)
{
    const auto isSpace = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; };
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

HWND DialogOwner()
{
    return g_app.mainWindow != nullptr ? g_app.mainWindow : g_app.window;
}

void ApplyDialogIcon(HWND dialog)
{
    if (g_app.icon != nullptr) {
        SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_app.icon));
    }
    HICON small = (g_app.trayIcon != nullptr) ? g_app.trayIcon : g_app.icon;
    if (small != nullptr) {
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    }
}

void UpdateMainWindow()
{
    if (g_app.mainWindow == nullptr) {
        return;
    }

    HWND action = GetDlgItem(g_app.mainWindow, IDC_MAIN_ACTION);
    HWND cast = GetDlgItem(g_app.mainWindow, IDC_MAIN_CAST);
    std::wstring status;

    switch (g_app.mode) {
    case SessionMode::Recording: {
        const ULONGLONG elapsedSeconds = (GetTickCount64() - g_app.recordingStart) / 1000;
        const std::wstring source = g_app.recorder.Source();
        const std::wstring sourceName = source.empty() ? L"top-most window" : FileNameOf(source);
        wchar_t buffer[256] = {};
        swprintf_s(buffer, L"Recording %02llu:%02llu - source: %s\nSaved to %s", elapsedSeconds / 60,
                   elapsedSeconds % 60, sourceName.c_str(), FileNameOf(g_app.recorder.Path()).c_str());
        status = buffer;
        SetWindowTextW(action, L"STOP RECORDING");
        ShowWindow(cast, SW_HIDE);
        break;
    }
    case SessionMode::Casting: {
        const ULONGLONG elapsedSeconds = (GetTickCount64() - g_app.recordingStart) / 1000;
        const std::wstring source = g_app.caster.Source();
        const std::wstring sourceName = source.empty() ? L"waiting for a source" : FileNameOf(source);
        wchar_t buffer[256] = {};
        swprintf_s(buffer, L"Casting %02llu:%02llu to %s\nSource: %s", elapsedSeconds / 60,
                   elapsedSeconds % 60, g_app.settings.receiverIp.c_str(), sourceName.c_str());
        status = buffer;
        SetWindowTextW(action, L"STOP CASTING");
        SetWindowTextW(cast, L"SELECT CAST SOURCE...");
        ShowWindow(cast, SW_SHOW);
        EnableWindow(cast, TRUE);
        break;
    }
    case SessionMode::Idle:
    default:
        status = L"Idle\nChoose a source when you start.";
        SetWindowTextW(action, L"RECORD LOCAL");
        SetWindowTextW(cast, L"CAST LOCAL");
        ShowWindow(cast, SW_SHOW);
        EnableWindow(cast, TRUE);
        break;
    }

    SetDlgItemTextW(g_app.mainWindow, IDC_MAIN_STATUS, status.c_str());
}

INT_PTR CALLBACK MainDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (message) {
    case WM_INITDIALOG:
        g_app.mainWindow = dialog;
        ApplyDialogIcon(dialog);
        UpdateMainWindow();
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_MAIN_ACTION:
            if (g_app.mode == SessionMode::Idle) {
                BeginRecordLocal();
            } else {
                StopActiveSession();
            }
            UpdateMainWindow();
            return TRUE;
        case IDC_MAIN_CAST:
            if (g_app.mode == SessionMode::Casting) {
                BeginSelectCastSource();
            } else {
                BeginCastLocal();
            }
            UpdateMainWindow();
            return TRUE;
        case IDC_MAIN_EXIT:
            ExitApplication();
            return TRUE;
        default:
            return FALSE;
        }
    case WM_CLOSE:
        DestroyWindow(dialog);
        return TRUE;
    case WM_DESTROY:
        g_app.mainWindow = nullptr;
        return TRUE;
    default:
        return FALSE;
    }
}

void BringWindowToFront(HWND window)
{
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

void ShowMainWindow()
{
    if (g_app.mainWindow != nullptr) {
        UpdateMainWindow();
        BringWindowToFront(g_app.mainWindow);
        return;
    }

    g_app.mainWindow = CreateDialogParamW(g_app.instance, MAKEINTRESOURCEW(IDD_MAIN_DIALOG), g_app.window,
                                          MainDialogProc, 0);
    if (g_app.mainWindow != nullptr) {
        UpdateMainWindow();
        BringWindowToFront(g_app.mainWindow);
    }
}

// WinlatorXR exposes the Android filesystem under Z:\. Only then is taskmgr known to be a stray
// Wine tool window rather than a native Windows process the user intentionally opened.
bool IsWinlatorXR()
{
    return GetFileAttributesW(L"Z:\\tmp") != INVALID_FILE_ATTRIBUTES;
}

std::wstring ProcessImagePathForPid(unsigned long processId)
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

bool ProcessIsAlive(unsigned long processId)
{
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return false;
    }
    DWORD exitCode = 0;
    const bool alive = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

struct CloseTaskmgrContext {
    const std::vector<unsigned long>* processIds = nullptr;
};

BOOL CALLBACK CloseTaskmgrWindowProc(HWND window, LPARAM context)
{
    auto* close = reinterpret_cast<CloseTaskmgrContext*>(context);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    for (unsigned long candidate : *close->processIds) {
        if (candidate == processId) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            break;
        }
    }
    return TRUE;
}

// Closes taskmgr instances: WM_CLOSE first so a taskmgr that is doing real work can finish, then a
// forced terminate if it is still alive after two seconds. Only runs on WinlatorXR.
void CloseTaskManagerInstances()
{
    if (!IsWinlatorXR()) {
        return;
    }

    std::vector<unsigned long> processIds;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W process = {};
        process.dwSize = sizeof(process);
        if (Process32FirstW(snapshot, &process) != FALSE) {
            do {
                if (_wcsicmp(process.szExeFile, L"taskmgr.exe") == 0 &&
                    process.th32ProcessID != GetCurrentProcessId()) {
                    processIds.push_back(process.th32ProcessID);
                }
            } while (Process32NextW(snapshot, &process) != FALSE);
        }
        CloseHandle(snapshot);
    }
    if (processIds.empty()) {
        return;
    }

    CloseTaskmgrContext context;
    context.processIds = &processIds;
    EnumWindows(CloseTaskmgrWindowProc, reinterpret_cast<LPARAM>(&context));

    const ULONGLONG deadline = GetTickCount64() + 2000;
    for (;;) {
        bool anyAlive = false;
        for (unsigned long processId : processIds) {
            if (ProcessIsAlive(processId)) {
                anyAlive = true;
                break;
            }
        }
        if (!anyAlive || GetTickCount64() >= deadline) {
            break;
        }
        Sleep(100);
    }

    for (unsigned long processId : processIds) {
        if (!ProcessIsAlive(processId)) {
            continue;
        }
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId);
        if (process == nullptr) {
            continue;
        }
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 1000);
        CloseHandle(process);
    }
}

// Brings the window of a selected capture target to the foreground. Attaching to the foreground
// thread works around SetForegroundWindow's focus-stealing rules on both Windows and Wine.
void ActivateProcessWindow(const ProcessEntry& entry)
{
    if (entry.window == nullptr || !IsWindow(entry.window)) {
        return;
    }

    if (IsIconic(entry.window)) {
        ShowWindow(entry.window, SW_RESTORE);
    } else {
        ShowWindow(entry.window, SW_SHOW);
    }

    const DWORD currentThread = GetCurrentThreadId();
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD targetThread = GetWindowThreadProcessId(entry.window, nullptr);

    if (foregroundThread != 0 && foregroundThread != currentThread) {
        AttachThreadInput(currentThread, foregroundThread, TRUE);
    }
    if (targetThread != 0 && targetThread != currentThread) {
        AttachThreadInput(currentThread, targetThread, TRUE);
    }

    BringWindowToTop(entry.window);
    SetForegroundWindow(entry.window);

    if (targetThread != 0 && targetThread != currentThread) {
        AttachThreadInput(currentThread, targetThread, FALSE);
    }
    if (foregroundThread != 0 && foregroundThread != currentThread) {
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    }
}

void CALLBACK TaskmgrWinEventProc(HWINEVENTHOOK /*hook*/, DWORD /*event*/, HWND window, LONG idObject, LONG idChild,
                                  DWORD /*thread*/, DWORD /*time*/)
{
    if (window == nullptr || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
        return;
    }
    if (GetAncestor(window, GA_ROOT) != window || !IsWindowVisible(window)) {
        return;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0 || processId == GetCurrentProcessId()) {
        return;
    }

    const std::wstring path = ProcessImagePathForPid(processId);
    if (path.empty() || _wcsicmp(FileNameOf(path).c_str(), L"taskmgr.exe") != 0) {
        return;
    }

    PostMessageW(g_app.window, kTaskmgrShownMessage, 0, 0);
}

void InstallTaskmgrWatcher()
{
    if (g_app.taskmgrShowHook == nullptr) {
        g_app.taskmgrShowHook =
            SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, TaskmgrWinEventProc, 0, 0,
                            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    if (g_app.taskmgrForegroundHook == nullptr) {
        g_app.taskmgrForegroundHook =
            SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, TaskmgrWinEventProc, 0, 0,
                            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
}

void UninstallTaskmgrWatcher()
{
    if (g_app.taskmgrShowHook != nullptr) {
        UnhookWinEvent(g_app.taskmgrShowHook);
        g_app.taskmgrShowHook = nullptr;
    }
    if (g_app.taskmgrForegroundHook != nullptr) {
        UnhookWinEvent(g_app.taskmgrForegroundHook);
        g_app.taskmgrForegroundHook = nullptr;
    }
}

std::wstring ExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory(path);
    const std::size_t slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        directory.resize(slash);
    }
    return directory;
}

std::wstring DefaultRecordingName()
{
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    wchar_t name[64] = {};
    swprintf_s(name, L"capture_%04u-%02u-%02u_%02u_%02u_%02u.mkv", now.wYear, now.wMonth, now.wDay,
               now.wHour, now.wMinute, now.wSecond);
    return name;
}

// D:\ is the Android host's Downloads directory inside Winlator. Record into D:\WXRVIDCAP by
// default, creating it if needed; fall back to Videos or the app directory when D:\ is missing.
std::wstring DefaultRecordingDirectory()
{
    const DWORD root = GetFileAttributesW(L"D:\\");
    if (root != INVALID_FILE_ATTRIBUTES && (root & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        const std::wstring directory = L"D:\\WXRVIDCAP";
        if (CreateDirectoryW(directory.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS) {
            return directory;
        }
    }

    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
        const std::wstring directory = std::wstring(profile) + L"\\Videos\\WXRVIDCAP";
        if (CreateDirectoryW(directory.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS) {
            return directory;
        }
    }

    const std::wstring fallback = ExecutableDirectory() + L"\\recordings";
    if (CreateDirectoryW(fallback.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS) {
        return fallback;
    }
    return ExecutableDirectory();
}

void ShowBalloon(const wchar_t* title, const wchar_t* text, DWORD flags)
{
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = g_app.window;
    data.uID = static_cast<UINT>(kTrayIconId);
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = flags;
    wcsncpy_s(data.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(data.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void UpdateTrayTip()
{
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = g_app.window;
    data.uID = static_cast<UINT>(kTrayIconId);
    data.uFlags = NIF_TIP;

    switch (g_app.mode) {
    case SessionMode::Recording: {
        const ULONGLONG elapsedSeconds = (GetTickCount64() - g_app.recordingStart) / 1000;
        wchar_t tip[128] = {};
        swprintf_s(tip, L"wxr-vid-cap - recording %02llu:%02llu", elapsedSeconds / 60, elapsedSeconds % 60);
        wcsncpy_s(data.szTip, tip, _TRUNCATE);
        break;
    }
    case SessionMode::Casting: {
        const ULONGLONG elapsedSeconds = (GetTickCount64() - g_app.recordingStart) / 1000;
        wchar_t tip[128] = {};
        swprintf_s(tip, L"wxr-vid-cap - casting %02llu:%02llu", elapsedSeconds / 60, elapsedSeconds % 60);
        wcsncpy_s(data.szTip, tip, _TRUNCATE);
        break;
    }
    case SessionMode::Idle:
    default:
        wcsncpy_s(data.szTip, L"wxr-vid-cap", _TRUNCATE);
        break;
    }

    Shell_NotifyIconW(NIM_MODIFY, &data);
}

bool AddTrayIcon()
{
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = g_app.window;
    data.uID = static_cast<UINT>(kTrayIconId);
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = (g_app.trayIcon != nullptr) ? g_app.trayIcon
                                             : ((g_app.icon != nullptr) ? g_app.icon : LoadIconW(nullptr, IDI_APPLICATION));
    wcsncpy_s(data.szTip, L"wxr-vid-cap", _TRUNCATE);

    if (Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
        return false;
    }
    UpdateTrayTip();
    return true;
}

void RemoveTrayIcon()
{
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = g_app.window;
    data.uID = static_cast<UINT>(kTrayIconId);
    Shell_NotifyIconW(NIM_DELETE, &data);
}

std::wstring GetIpEditText(HWND dialog)
{
    wchar_t buffer[64] = {};
    GetDlgItemTextW(dialog, IDC_IP_EDIT, buffer, ARRAYSIZE(buffer));
    return buffer;
}

void SetIpEditText(HWND dialog, const std::wstring& text, int caret)
{
    SetDlgItemTextW(dialog, IDC_IP_EDIT, text.c_str());
    SendDlgItemMessageW(dialog, IDC_IP_EDIT, EM_SETSEL, static_cast<WPARAM>(caret),
                        static_cast<LPARAM>(caret));
    SetFocus(GetDlgItem(dialog, IDC_IP_EDIT));
}

void InsertIpCharacter(HWND dialog, wchar_t character)
{
    std::wstring text = GetIpEditText(dialog);
    const DWORD selection = static_cast<DWORD>(SendDlgItemMessageW(dialog, IDC_IP_EDIT, EM_GETSEL, 0, 0));
    int start = LOWORD(selection);
    int end = HIWORD(selection);
    if (start > static_cast<int>(text.size())) {
        start = static_cast<int>(text.size());
    }
    if (end > static_cast<int>(text.size())) {
        end = static_cast<int>(text.size());
    }
    if (end < start) {
        std::swap(start, end);
    }

    const std::size_t removed = static_cast<std::size_t>(end - start);
    if (text.size() - removed >= 15) {
        return;
    }

    text.replace(static_cast<std::size_t>(start), removed, 1, character);
    SetIpEditText(dialog, text, start + 1);
}

void BackspaceIpCharacter(HWND dialog)
{
    std::wstring text = GetIpEditText(dialog);
    const DWORD selection = static_cast<DWORD>(SendDlgItemMessageW(dialog, IDC_IP_EDIT, EM_GETSEL, 0, 0));
    int start = LOWORD(selection);
    int end = HIWORD(selection);
    if (start > static_cast<int>(text.size())) {
        start = static_cast<int>(text.size());
    }
    if (end > static_cast<int>(text.size())) {
        end = static_cast<int>(text.size());
    }
    if (end < start) {
        std::swap(start, end);
    }

    if (start == end) {
        if (start == 0) {
            return;
        }
        --start;
    }
    text.erase(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
    SetIpEditText(dialog, text, start);
}

INT_PTR CALLBACK ReceiverIpDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG: {
        auto* value = reinterpret_cast<std::wstring*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(value));
        ApplyDialogIcon(dialog);

        std::wstring initial = (value != nullptr) ? *value : std::wstring();
        const bool usedDefault = initial.empty();
        if (usedDefault) {
            initial = L"192.168.0.";
        }
        SetDlgItemTextW(dialog, IDC_IP_EDIT, initial.c_str());
        if (usedDefault) {
            const int caret = static_cast<int>(initial.size());
            SendDlgItemMessageW(dialog, IDC_IP_EDIT, EM_SETSEL, static_cast<WPARAM>(caret),
                                static_cast<LPARAM>(caret));
        } else {
            SendDlgItemMessageW(dialog, IDC_IP_EDIT, EM_SETSEL, 0, static_cast<LPARAM>(-1));
        }
        SetFocus(GetDlgItem(dialog, IDC_IP_EDIT));
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            wchar_t buffer[128] = {};
            GetDlgItemTextW(dialog, IDC_IP_EDIT, buffer, ARRAYSIZE(buffer));

            const std::wstring value = Trim(buffer);
            IN_ADDR address = {};
            if (value.empty() || InetPtonW(AF_INET, value.c_str(), &address) != 1) {
                MessageBoxW(dialog, L"Enter a valid IPv4 address, for example 192.168.1.42.", L"wxr-vid-cap",
                            MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            auto* target = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
            if (target != nullptr) {
                *target = value;
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            break;
        }

        if (LOWORD(wParam) >= IDC_IP_KEY_0 && LOWORD(wParam) <= IDC_IP_KEY_BACK) {
            const UINT key = LOWORD(wParam);
            if (key == IDC_IP_KEY_BACK) {
                BackspaceIpCharacter(dialog);
            } else if (key == IDC_IP_KEY_DOT) {
                InsertIpCharacter(dialog, L'.');
            } else {
                InsertIpCharacter(dialog, static_cast<wchar_t>(L'0' + (key - IDC_IP_KEY_0)));
            }
            return TRUE;
        }
        return FALSE;
    default:
        return FALSE;
    }
}

bool PromptForReceiverIp()
{
    std::wstring value = g_app.settings.receiverIp;
    if (DialogBoxParamW(g_app.instance, MAKEINTRESOURCEW(IDD_IP_DIALOG), DialogOwner(), ReceiverIpDialogProc,
                        reinterpret_cast<LPARAM>(&value)) != IDOK) {
        return false;
    }

    g_app.settings.receiverIp = value;
    if (!SaveSettings(g_app.settings)) {
        MessageBoxW(DialogOwner(),
                    L"The receiver IP is set for this session but could not be saved to disk.\n"
                    L"It will be lost when the app exits.",
                    L"wxr-vid-cap", MB_OK | MB_ICONWARNING);
    }
    return true;
}

struct CaptureSourceDialogContext {
    CaptureSource* source = nullptr;
    AppSettings* settings = nullptr;
    std::vector<ProcessEntry> windows;
};

void UpdateCaptureSourceControls(HWND dialog)
{
    const bool running = IsDlgButtonChecked(dialog, IDC_TARGET_EXE) == BST_CHECKED;
    const bool launch = IsDlgButtonChecked(dialog, IDC_TARGET_LAUNCH) == BST_CHECKED;
    EnableWindow(GetDlgItem(dialog, IDC_PROCESS_LIST), running);
    EnableWindow(GetDlgItem(dialog, IDC_LAUNCH_PATH), launch);
    EnableWindow(GetDlgItem(dialog, IDC_BROWSE), launch);
}

void SetSelectedProcessPath(HWND dialog, CaptureSourceDialogContext* context)
{
    const LRESULT index = SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_GETCURSEL, 0, 0);
    if (context == nullptr || index == LB_ERR) {
        SetDlgItemTextW(dialog, IDC_TARGET_PATH, L"");
        return;
    }

    const LRESULT item =
        SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_GETITEMDATA, static_cast<WPARAM>(index), 0);
    if (item < 0 || static_cast<std::size_t>(item) >= context->windows.size()) {
        SetDlgItemTextW(dialog, IDC_TARGET_PATH, L"");
        return;
    }
    SetDlgItemTextW(dialog, IDC_TARGET_PATH,
                    context->windows[static_cast<std::size_t>(item)].executablePath.c_str());
}

// Selecting a running app brings it forward and clears stray taskmgr windows on WinlatorXR.
void ActivateSelectedCaptureTarget(HWND dialog, CaptureSourceDialogContext* context)
{
    if (context == nullptr) {
        return;
    }

    const LRESULT index = SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_GETCURSEL, 0, 0);
    if (index == LB_ERR) {
        return;
    }
    const LRESULT item =
        SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_GETITEMDATA, static_cast<WPARAM>(index), 0);
    if (item < 0 || static_cast<std::size_t>(item) >= context->windows.size()) {
        return;
    }

    CloseTaskManagerInstances();
    ActivateProcessWindow(context->windows[static_cast<std::size_t>(item)]);
}

void PopulateProcessList(HWND dialog, CaptureSourceDialogContext* context)
{
    HWND list = GetDlgItem(dialog, IDC_PROCESS_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < context->windows.size(); ++i) {
        const ProcessEntry& entry = context->windows[i];
        const std::wstring text = entry.windowTitle + L" - " + FileNameOf(entry.executablePath);
        const LRESULT index = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        if (index >= 0) {
            SendMessageW(list, LB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
        }
    }
}

bool BrowseForApplication(HWND owner, std::wstring& path)
{
    wchar_t buffer[MAX_PATH * 4] = {};
    wcsncpy_s(buffer, path.c_str(), _TRUNCATE);

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"Applications (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = ARRAYSIZE(buffer);
    dialog.lpstrTitle = L"Application to start with the capture hook";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&dialog) == FALSE) {
        return false;
    }
    path = buffer;
    return true;
}

INT_PTR CALLBACK CaptureSourceDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG: {
        auto* context = reinterpret_cast<CaptureSourceDialogContext*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        ApplyDialogIcon(dialog);

        PopulateProcessList(dialog, context);
        HWND list = GetDlgItem(dialog, IDC_PROCESS_LIST);

        bool specific = false;
        if (!context->settings->captureTargetExe.empty()) {
            for (std::size_t i = 0; i < context->windows.size(); ++i) {
                if (_wcsicmp(context->windows[i].executablePath.c_str(),
                             context->settings->captureTargetExe.c_str()) == 0) {
                    SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
                    specific = true;
                    break;
                }
            }
        }

        if (!specific) {
            SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        }

        CheckRadioButton(dialog, IDC_TARGET_TOPMOST, IDC_TARGET_LAUNCH,
                         specific ? IDC_TARGET_EXE : IDC_TARGET_TOPMOST);
        const std::wstring& saved = context->settings->captureTargetExe;
        if (!specific && !saved.empty() && GetFileAttributesW(saved.c_str()) != INVALID_FILE_ATTRIBUTES) {
            SetDlgItemTextW(dialog, IDC_LAUNCH_PATH, saved.c_str());
        } else {
            SetDlgItemTextW(dialog, IDC_LAUNCH_PATH, L"");
        }
        if (specific) {
            SetSelectedProcessPath(dialog, context);
        } else {
            SetDlgItemTextW(dialog, IDC_TARGET_PATH, L"");
        }
        UpdateCaptureSourceControls(dialog);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_TARGET_TOPMOST:
        case IDC_TARGET_EXE:
        case IDC_TARGET_LAUNCH: {
            auto* context = reinterpret_cast<CaptureSourceDialogContext*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
            const bool running = (IsDlgButtonChecked(dialog, IDC_TARGET_EXE) == BST_CHECKED);
            if (running) {
                if (context != nullptr && !context->windows.empty() &&
                    SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_GETCURSEL, 0, 0) == LB_ERR) {
                    SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_SETCURSEL, 0, 0);
                }
                SetSelectedProcessPath(dialog, context);
            } else {
                SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
                SetDlgItemTextW(dialog, IDC_TARGET_PATH, L"");
            }
            UpdateCaptureSourceControls(dialog);
            return TRUE;
        }
        case IDC_PROCESS_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                auto* context = reinterpret_cast<CaptureSourceDialogContext*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
                SetSelectedProcessPath(dialog, context);
                ActivateSelectedCaptureTarget(dialog, context);
                return TRUE;
            }
            return FALSE;
        case IDC_BROWSE: {
            wchar_t current[MAX_PATH * 4] = {};
            GetDlgItemTextW(dialog, IDC_LAUNCH_PATH, current, ARRAYSIZE(current));
            std::wstring path = current;
            if (BrowseForApplication(dialog, path)) {
                SetDlgItemTextW(dialog, IDC_LAUNCH_PATH, path.c_str());
                CheckRadioButton(dialog, IDC_TARGET_TOPMOST, IDC_TARGET_LAUNCH, IDC_TARGET_LAUNCH);
                SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
                SetDlgItemTextW(dialog, IDC_TARGET_PATH, L"");
                UpdateCaptureSourceControls(dialog);
            }
            return TRUE;
        }
        case IDC_REFRESH: {
            auto* context = reinterpret_cast<CaptureSourceDialogContext*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
            if (context == nullptr) {
                return TRUE;
            }

            std::wstring selected;
            const LRESULT oldIndex = SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_GETCURSEL, 0, 0);
            if (oldIndex != LB_ERR) {
                const LRESULT item =
                    SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_GETITEMDATA, static_cast<WPARAM>(oldIndex), 0);
                if (item >= 0 && static_cast<std::size_t>(item) < context->windows.size()) {
                    selected = context->windows[static_cast<std::size_t>(item)].executablePath;
                }
            }

            context->windows = ListWindowProcesses();
            PopulateProcessList(dialog, context);

            if (IsDlgButtonChecked(dialog, IDC_TARGET_EXE) == BST_CHECKED) {
                LRESULT newIndex = LB_ERR;
                if (!selected.empty()) {
                    for (std::size_t i = 0; i < context->windows.size(); ++i) {
                        if (_wcsicmp(context->windows[i].executablePath.c_str(), selected.c_str()) == 0) {
                            newIndex = static_cast<LRESULT>(i);
                            break;
                        }
                    }
                }
                if (newIndex == LB_ERR && !context->windows.empty()) {
                    newIndex = 0;
                }
                if (newIndex != LB_ERR) {
                    SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_SETCURSEL,
                                        static_cast<WPARAM>(newIndex), 0);
                }
            }

            SetSelectedProcessPath(dialog, context);
            UpdateCaptureSourceControls(dialog);
            return TRUE;
        }
        case IDOK: {
            auto* context = reinterpret_cast<CaptureSourceDialogContext*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
            if (context == nullptr || context->source == nullptr || context->settings == nullptr) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }

            if (IsDlgButtonChecked(dialog, IDC_TARGET_TOPMOST) == BST_CHECKED) {
                context->source->kind = CaptureSource::Kind::TopMost;
                context->source->executable.clear();
                context->settings->captureTargetExe.clear();
            } else if (IsDlgButtonChecked(dialog, IDC_TARGET_EXE) == BST_CHECKED) {
                const LRESULT index = SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_GETCURSEL, 0, 0);
                if (index == LB_ERR) {
                    MessageBoxW(dialog,
                                L"Select an application from the list, or choose the top-most window.",
                                L"wxr-vid-cap", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                const LRESULT item =
                    SendDlgItemMessageW(dialog, IDC_PROCESS_LIST, LB_GETITEMDATA, static_cast<WPARAM>(index), 0);
                if (item < 0 || static_cast<std::size_t>(item) >= context->windows.size()) {
                    return TRUE;
                }
                context->source->kind = CaptureSource::Kind::Running;
                context->source->executable = context->windows[static_cast<std::size_t>(item)].executablePath;
                if (IsExcludedExecutablePath(context->source->executable)) {
                    const std::wstring blocked =
                        L"That application is on the capture blacklist:\n" + context->source->executable;
                    MessageBoxW(dialog, blocked.c_str(), L"wxr-vid-cap", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                context->settings->captureTargetExe = context->source->executable;
            } else {
                wchar_t current[MAX_PATH * 4] = {};
                GetDlgItemTextW(dialog, IDC_LAUNCH_PATH, current, ARRAYSIZE(current));
                std::wstring path = Trim(current);
                if (path.empty()) {
                    if (!BrowseForApplication(dialog, path)) {
                        return TRUE;
                    }
                    SetDlgItemTextW(dialog, IDC_LAUNCH_PATH, path.c_str());
                }
                if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    MessageBoxW(dialog, L"The selected application could not be found.", L"wxr-vid-cap",
                                MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                if (IsExcludedExecutablePath(path)) {
                    const std::wstring blocked =
                        L"That application is on the capture blacklist:\n" + path;
                    MessageBoxW(dialog, blocked.c_str(), L"wxr-vid-cap", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                context->source->kind = CaptureSource::Kind::Launch;
                context->source->executable = path;
                context->settings->captureTargetExe = path;
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
    default:
        return FALSE;
    }
}

bool PromptForCaptureSource(CaptureSource& source)
{
    CaptureSourceDialogContext context;
    context.source = &source;
    context.settings = &g_app.settings;
    context.windows = ListWindowProcesses();

    if (DialogBoxParamW(g_app.instance, MAKEINTRESOURCEW(IDD_CAPTURE_SOURCE), DialogOwner(),
                        CaptureSourceDialogProc, reinterpret_cast<LPARAM>(&context)) != IDOK) {
        return false;
    }

    if (!SaveSettings(g_app.settings)) {
        MessageBoxW(DialogOwner(), L"Settings could not be saved.", L"wxr-vid-cap", MB_OK | MB_ICONWARNING);
    }
    return true;
}

bool WriteBmpFile(const std::wstring& path, const std::vector<std::uint8_t>& bgra, int width, int height)
{
    if (width <= 0 || height <= 0 ||
        bgra.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
        return false;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return false;
    }

    const std::uint32_t rowBytes = static_cast<std::uint32_t>(width) * 4;
    const std::uint32_t pixelBytes = rowBytes * static_cast<std::uint32_t>(height);
    std::uint8_t header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    const std::uint32_t fileSize = 54 + pixelBytes;
    std::memcpy(header + 2, &fileSize, 4);
    const std::uint32_t offset = 54;
    std::memcpy(header + 10, &offset, 4);
    const std::uint32_t headerSize = 40;
    std::memcpy(header + 14, &headerSize, 4);
    const std::int32_t signedWidth = width;
    const std::int32_t signedHeight = height;
    std::memcpy(header + 18, &signedWidth, 4);
    std::memcpy(header + 22, &signedHeight, 4);
    const std::uint16_t planes = 1;
    std::memcpy(header + 26, &planes, 2);
    const std::uint16_t bits = 32;
    std::memcpy(header + 28, &bits, 2);
    std::memcpy(header + 34, &pixelBytes, 4);

    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    for (int y = height - 1; y >= 0 && ok; --y) {
        const std::uint8_t* row = bgra.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        ok = std::fwrite(row, 1, rowBytes, file) == rowBytes;
    }
    std::fclose(file);
    return ok;
}

void HookAndRecordDebug()
{
    if (g_app.mode != SessionMode::Idle) {
        return;
    }
    if (g_app.settings.captureTargetExe.empty()) {
        MessageBoxW(nullptr, L"Set a capture target with RECORD LOCAL or CAST LOCAL first.", L"wxr-vid-cap",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    HookCapture hook;
    std::wstring error;
    if (!hook.Start(g_app.settings.captureTargetExe, 1280, 720, error)) {
        PlayHookFailureSound();
        MessageBoxW(g_app.window, error.c_str(), L"HOOK AND RECORD (debug)", MB_OK | MB_ICONERROR);
        return;
    }

    std::vector<std::uint8_t> frame;
    int frames = 0;
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 3000) {
        if (hook.Capture(frame, 50, error)) {
            ++frames;
        }
    }

    const int sourceWidth = hook.SourceWidth();
    const int sourceHeight = hook.SourceHeight();
    const std::wstring method = hook.LastInjectionMethod();
    const unsigned long processId = hook.ProcessId();
    hook.Stop();

    const std::wstring bmpPath = ExecutableDirectory() + L"\\hook_debug.bmp";
    const bool saved = !frame.empty() && WriteBmpFile(bmpPath, frame, 1280, 720);

    const double seconds = static_cast<double>(GetTickCount64() - start) / 1000.0;
    const double fps = seconds > 0.0 ? frames / seconds : 0.0;

    std::wstring message = L"Target: " + FileNameOf(g_app.settings.captureTargetExe) + L" (PID " +
                           std::to_wstring(processId) + L")\n";
    message += L"Method: " + method + L"\n";
    message += L"Source: " + std::to_wstring(sourceWidth) + L"x" + std::to_wstring(sourceHeight) + L"\n";
    message += L"Frames: " + std::to_wstring(frames) + L" in " + std::to_wstring(seconds) + L" s (" +
               std::to_wstring(static_cast<int>(fps + 0.5)) + L" fps)\n";
    message += saved ? L"Snapshot: hook_debug.bmp\n" : L"Snapshot: not written\n";
    message += L"\nThe next RECORD LOCAL or CAST LOCAL will use this hook automatically.";

    MessageBoxW(g_app.window, message.c_str(), L"HOOK AND RECORD (debug)",
                MB_OK | (frames > 0 ? MB_ICONINFORMATION : MB_ICONWARNING));
}

bool RunLauncher(const std::wstring& launcher, const std::wstring& target, std::wstring& error)
{
    if (GetFileAttributesW(launcher.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = FileNameOf(launcher) + L" was not found next to the app.";
        return false;
    }

    std::wstring command = L"\"" + launcher + L"\" --exe \"" + target + L"\" --quiet";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
        error = L"Could not start " + FileNameOf(launcher) + L" (error " +
                std::to_wstring(GetLastError()) + L").";
        return false;
    }

    WaitForSingleObject(process.hProcess, 20000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (exitCode != 0) {
        error = FileNameOf(launcher) + L" could not inject the capture hook.\n\n"
                L"See wxr_launch.log next to the app for the reason.";
        return false;
    }
    return true;
}

bool LaunchTargetWithHook(const std::wstring& path, std::wstring& error)
{
    if (IsExcludedExecutablePath(path)) {
        error = L"That application is blacklisted from capture: " + path;
        return false;
    }
    if (FindProcessByExecutable(path) != 0) {
        error = L"That application is already running.\n\n"
                L"Close it first, or choose \"Capture a running application\".";
        return false;
    }

    const std::wstring directory = ExecutableDirectory();
    const std::wstring launcher64 = directory + L"\\wxr_launch.exe";
    const std::wstring launcher32 = directory + L"\\wxr_launch32.exe";

    DWORD binaryType = 0;
    const bool known = GetBinaryTypeW(path.c_str(), &binaryType) != FALSE;
    const bool target32 = known && binaryType == SCS_32BIT_BINARY;

    if (target32) {
        return RunLauncher(launcher32, path, error);
    }

    if (RunLauncher(launcher64, path, error)) {
        return true;
    }
    if (!known) {
        std::wstring firstError = error;
        if (RunLauncher(launcher32, path, error)) {
            return true;
        }
        error = firstError + L"\n" + error;
    }
    return false;
}

bool PromptForRecordingPath(std::wstring& path)
{
    wchar_t buffer[MAX_PATH * 4] = {};
    const std::wstring name = DefaultRecordingName();
    wcsncpy_s(buffer, name.c_str(), _TRUNCATE);

    const std::wstring initialDirectory = DefaultRecordingDirectory();

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = DialogOwner();
    dialog.lpstrFilter = L"Matroska video (*.mkv)\0*.mkv\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = ARRAYSIZE(buffer);
    dialog.lpstrDefExt = L"mkv";
    dialog.lpstrInitialDir = initialDirectory.empty() ? nullptr : initialDirectory.c_str();
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&dialog) == FALSE) {
        return false;
    }

    path = buffer;
    return true;
}

void BeginRecordLocal()
{
    if (g_app.mode != SessionMode::Idle) {
        return;
    }

    CaptureSource source;
    if (!PromptForCaptureSource(source)) {
        return;
    }

    std::wstring path;
    if (!PromptForRecordingPath(path)) {
        return;
    }

    if (source.kind == CaptureSource::Kind::Launch) {
        ShowBalloon(L"Starting application",
                    (L"Launching " + FileNameOf(source.executable) + L" with the capture hook.").c_str(), NIIF_INFO);
        std::wstring launchError;
        if (!LaunchTargetWithHook(source.executable, launchError)) {
            PlayHookFailureSound();
            MessageBoxW(DialogOwner(), launchError.c_str(), L"wxr-vid-cap", MB_OK | MB_ICONERROR);
            return;
        }
    }

    PlayRecordingStartSound();

    std::wstring error;
    const bool launched = source.kind == CaptureSource::Kind::Launch;
    const int hookTimeout = launched ? kLaunchHookTimeoutMs : 6000;
    const int retryTimeout = launched ? kLateHookTimeoutMs : 0;
    if (!g_app.recorder.Start(path, source.executable, error, hookTimeout, retryTimeout)) {
        PlayHookFailureSound();
        MessageBoxW(DialogOwner(), (L"Could not start recording.\n\n" + error).c_str(), L"wxr-vid-cap",
                    MB_OK | MB_ICONERROR);
        return;
    }

    g_app.mode = SessionMode::Recording;
    g_app.recordingStart = GetTickCount64();
    SetTimer(g_app.window, kSessionTimerId, 1000, nullptr);
    UpdateTrayTip();
    UpdateMainWindow();
    const std::wstring detail = L"Saving to:\n" + path + L"\nCapture: " + g_app.recorder.Backend();
    ShowBalloon(L"Recording started", detail.c_str(), NIIF_INFO);
}

void BeginCastLocal()
{
    if (g_app.mode != SessionMode::Idle) {
        return;
    }

    // Always show the receiver address, prefilled with the last one used.
    if (!PromptForReceiverIp()) {
        return;
    }

    CaptureSource source;
    if (!PromptForCaptureSource(source)) {
        return;
    }

    if (source.kind == CaptureSource::Kind::Launch) {
        ShowBalloon(L"Starting application",
                    (L"Launching " + FileNameOf(source.executable) + L" with the capture hook.").c_str(), NIIF_INFO);
        std::wstring launchError;
        if (!LaunchTargetWithHook(source.executable, launchError)) {
            PlayHookFailureSound();
            MessageBoxW(DialogOwner(), launchError.c_str(), L"wxr-vid-cap", MB_OK | MB_ICONERROR);
            return;
        }
    }

    PlayRecordingStartSound();

    std::wstring error;
    const bool launched = source.kind == CaptureSource::Kind::Launch;
    const int hookTimeout = launched ? kLaunchHookTimeoutMs : 6000;
    const int retryTimeout = launched ? kLateHookTimeoutMs : 0;
    if (!g_app.caster.Start(g_app.settings.receiverIp, g_app.settings.port, source.executable, error,
                            hookTimeout, g_app.settings.castJitterFrames, retryTimeout)) {
        PlayHookFailureSound();
        MessageBoxW(DialogOwner(), (L"Could not start casting.\n\n" + error).c_str(), L"wxr-vid-cap",
                    MB_OK | MB_ICONERROR);
        return;
    }

    g_app.mode = SessionMode::Casting;
    g_app.recordingStart = GetTickCount64();
    SetTimer(g_app.window, kSessionTimerId, 1000, nullptr);
    UpdateTrayTip();
    UpdateMainWindow();

    const std::wstring castSource = g_app.caster.Source();
    const std::wstring capture =
        castSource.empty() ? L"waiting for a source (blank video + silent audio)"
                           : (L"graphics hook: " + FileNameOf(castSource));
    const std::wstring message = L"Sending live to " + g_app.settings.receiverIp + L".\n\nOpen " +
                                 g_app.caster.ViewerUrl() +
                                 L" in VLC on the receiver. It can join at any time.\nCapture: " + capture;
    ShowBalloon(L"Casting", message.c_str(), NIIF_INFO);
}

void BeginSelectCastSource()
{
    if (g_app.mode != SessionMode::Casting) {
        return;
    }

    CaptureSource source;
    if (!PromptForCaptureSource(source)) {
        return;
    }

    if (source.kind == CaptureSource::Kind::Launch) {
        ShowBalloon(L"Starting application",
                    (L"Launching " + FileNameOf(source.executable) + L" with the capture hook.").c_str(), NIIF_INFO);
        std::wstring launchError;
        if (!LaunchTargetWithHook(source.executable, launchError)) {
            PlayHookFailureSound();
            MessageBoxW(DialogOwner(), launchError.c_str(), L"wxr-vid-cap", MB_OK | MB_ICONERROR);
            return;
        }
    }

    std::wstring error;
    if (!g_app.caster.ChangeSource(source.executable, error)) {
        MessageBoxW(DialogOwner(), (L"Could not change the cast source.\n\n" + error).c_str(), L"wxr-vid-cap",
                    MB_OK | MB_ICONERROR);
        return;
    }
}

void StopActiveSession(bool exiting)
{
    switch (g_app.mode) {
    case SessionMode::Recording: {
        KillTimer(g_app.window, kSessionTimerId);
        g_app.recorder.Stop();
        PlayRecordingStopSound(exiting);
        if (g_app.recorder.LastStopWroteFile()) {
            std::wstring message = L"Saved:\n" + g_app.recorder.Path();
            if (!g_app.recorder.LastError().empty()) {
                message += L"\n\n";
                message += g_app.recorder.LastError();
            }
            ShowBalloon(L"Recording stopped", message.c_str(),
                        g_app.recorder.LastError().empty() ? NIIF_INFO : NIIF_WARNING);
        } else {
            std::wstring message = L"No file was written.";
            if (!g_app.recorder.LastError().empty()) {
                message += L"\n";
                message += g_app.recorder.LastError();
            }
            ShowBalloon(L"Recording stopped", message.c_str(), NIIF_WARNING);
        }
        break;
    }
    case SessionMode::Casting:
        KillTimer(g_app.window, kSessionTimerId);
        g_app.caster.Stop();
        PlayRecordingStopSound(exiting);
        if (g_app.caster.LastError().empty()) {
            ShowBalloon(L"Cast stopped", L"Live stream stopped.", NIIF_INFO);
        } else {
            const std::wstring message = L"Live stream stopped.\n\n" + g_app.caster.LastError();
            ShowBalloon(L"Cast stopped", message.c_str(), NIIF_WARNING);
        }
        break;
    case SessionMode::Idle:
    default:
        break;
    }

    g_app.mode = SessionMode::Idle;
    UpdateTrayTip();
    UpdateMainWindow();
}

void ExitApplication()
{
    StopActiveSession(true);
    if (g_app.mainWindow != nullptr) {
        DestroyWindow(g_app.mainWindow);
        g_app.mainWindow = nullptr;
    }
    if (g_app.window != nullptr) {
        DestroyWindow(g_app.window);
    }
}

void HandleCommand(UINT command)
{
    switch (command) {
    case kMenuShow:
        ShowMainWindow();
        break;
    case kMenuRecordLocal:
        BeginRecordLocal();
        break;
    case kMenuCastLocal:
        BeginCastLocal();
        break;
    case kMenuStopRecording:
    case kMenuStopCasting:
        StopActiveSession();
        break;
    case kMenuSelectCastSource:
        BeginSelectCastSource();
        break;
    case kMenuExit:
        ExitApplication();
        break;
    default:
        break;
    }
}

void ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuShow, L"SHOW");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    switch (g_app.mode) {
    case SessionMode::Recording:
        AppendMenuW(menu, MF_STRING, kMenuStopRecording, L"STOP RECORDING");
        break;
    case SessionMode::Casting:
        AppendMenuW(menu, MF_STRING, kMenuStopCasting, L"STOP CASTING");
        AppendMenuW(menu, MF_STRING, kMenuSelectCastSource, L"SELECT CAST SOURCE...");
        break;
    case SessionMode::Idle:
    default:
        AppendMenuW(menu, MF_STRING, kMenuRecordLocal, L"RECORD LOCAL");
        AppendMenuW(menu, MF_STRING, kMenuCastLocal, L"CAST LOCAL");
        break;
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"EXIT");

    POINT cursor = {};
    GetCursorPos(&cursor);

    // Required so the menu closes when the user clicks elsewhere.
    SetForegroundWindow(g_app.window);

    const UINT command =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, cursor.x, cursor.y, 0, g_app.window,
                       nullptr);

    DestroyMenu(menu);
    PostMessageW(g_app.window, WM_NULL, 0, 0);

    if (command != 0) {
        HandleCommand(command);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == g_app.taskbarCreatedMessage && g_app.taskbarCreatedMessage != 0) {
        AddTrayIcon();
        return 0;
    }

    switch (message) {
    case kTrayMessage: {
        const UINT event = LOWORD(lParam);
        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            ShowTrayMenu();
        } else if (event == WM_LBUTTONDBLCLK) {
            ShowMainWindow();
        }
        return 0;
    }
    case kShowMenuMessage:
        ShowMainWindow();
        return 0;
    case kTaskmgrShownMessage:
        ShowMainWindow();
        return 0;
    case WM_TIMER:
        if (wParam == kSessionTimerId && g_app.mode == SessionMode::Recording) {
            if (!g_app.recorder.Active()) {
                // The recorder ended on its own; StopActiveSession reports the reason.
                StopActiveSession();
                return 0;
            }
            UpdateTrayTip();
            UpdateMainWindow();
            if (GetTickCount64() - g_app.recordingStart >= kMaxRecordingMs) {
                StopActiveSession();
            }
        } else if (wParam == kSessionTimerId && g_app.mode == SessionMode::Casting) {
            if (!g_app.caster.Active()) {
                StopActiveSession();
                return 0;
            }
            UpdateTrayTip();
            UpdateMainWindow();
        }
        return 0;
    case WM_COMMAND:
        HandleCommand(LOWORD(wParam));
        return 0;
    case WM_CLOSE:
        StopActiveSession(true);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        UninstallTaskmgrWatcher();
        RemoveTrayIcon();
        if (g_app.instanceMutex != nullptr) {
            CloseHandle(g_app.instanceMutex);
            g_app.instanceMutex = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previousInstance*/, PWSTR /*commandLine*/,
                    int /*showCommand*/)
{
    SetProcessDPIAware();
    LoadCaptureBlacklist();

    if (wcsstr(GetCommandLineW(), L"--test-sounds") != nullptr) {
        PlayRecordingStartSound();
        std::ofstream start(ExecutableDirectory() + L"\\record_start.wav", std::ios::binary);
        start.write(reinterpret_cast<const char*>(g_recordStartSound.data()),
                    static_cast<std::streamsize>(g_recordStartSound.size()));
        Sleep(600);
        PlayRecordingStopSound();
        std::ofstream stop(ExecutableDirectory() + L"\\record_stop.wav", std::ios::binary);
        stop.write(reinterpret_cast<const char*>(g_recordStopSound.data()),
                   static_cast<std::streamsize>(g_recordStopSound.size()));
        Sleep(800);
        PlayHookFailureSound();
        std::ofstream hookFailure(ExecutableDirectory() + L"\\record_hook_failure.wav", std::ios::binary);
        hookFailure.write(reinterpret_cast<const char*>(g_hookFailureSound.data()),
                          static_cast<std::streamsize>(g_hookFailureSound.size()));
        Sleep(800);
        return 0;
    }

    if (wcsstr(GetCommandLineW(), L"--hook-debug") != nullptr) {
        g_app.instance = instance;
        g_app.settings = LoadSettings();
        HookAndRecordDebug();
        return 0;
    }

    // One tray icon, one process: a second launch would otherwise double the idle footprint and
    // fight over the same output paths.
    g_app.instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\wxr-vid-cap-single-instance");
    if (g_app.instanceMutex == nullptr) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // A second launch is a request to talk to the running copy, not a new process. If the
        // window exists, show it; otherwise the first instance is still starting up.
        const HWND existing = FindWindowW(kWindowClass, nullptr);
        if (existing != nullptr) {
            PostMessageW(existing, kShowMenuMessage, 0, 0);
            return 0;
        }
        MessageBoxW(nullptr, L"wxr-vid-cap is already starting up.", L"wxr-vid-cap", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    g_app.instance = instance;
    g_app.icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                               LR_DEFAULTCOLOR));
    if (g_app.icon == nullptr) {
        g_app.icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    }
    g_app.trayIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                   LR_DEFAULTCOLOR));
    if (g_app.trayIcon == nullptr) {
        g_app.trayIcon = g_app.icon;
    }
    g_app.settings = LoadSettings();
    g_app.taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = g_app.icon;
    windowClass.hIconSm = g_app.icon;
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassExW(&windowClass) == 0) {
        return 1;
    }

    // Never shown: the tray icon is the whole interface.
    g_app.window = CreateWindowExW(0, kWindowClass, L"wxr-vid-cap", 0, 0, 0, 0, 0, nullptr, nullptr, instance,
                                   nullptr);
    if (g_app.window == nullptr) {
        return 1;
    }

    if (!AddTrayIcon()) {
        MessageBoxW(nullptr,
                    L"The notification area is not available, so the app has no way to be controlled.\n"
                    L"Start it from a desktop session that provides a system tray.",
                    L"wxr-vid-cap", MB_OK | MB_ICONERROR);
        return 1;
    }

    InstallTaskmgrWatcher();

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (g_app.mainWindow == nullptr || !IsDialogMessageW(g_app.mainWindow, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    return 0;
}
