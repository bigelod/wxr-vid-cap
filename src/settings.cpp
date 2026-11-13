#include "settings.h"

#include <windows.h>

#include <algorithm>

namespace {

std::wstring SettingsDirectory()
{
    wchar_t appData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L".";
    }

    std::wstring directory(appData);
    directory += L"\\wxr-vid-cap";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

}  // namespace

std::wstring SettingsFilePath()
{
    return SettingsDirectory() + L"\\settings.ini";
}

AppSettings LoadSettings()
{
    AppSettings settings;
    const std::wstring path = SettingsFilePath();

    wchar_t ip[128] = {};
    GetPrivateProfileStringW(L"cast", L"receiver_ip", L"", ip, ARRAYSIZE(ip), path.c_str());
    settings.receiverIp = ip;

    const int port = GetPrivateProfileIntW(L"cast", L"port", 1234, path.c_str());
    settings.port = (port > 0 && port <= 65535) ? static_cast<unsigned short>(port) : 1234;

    const int jitterFrames = GetPrivateProfileIntW(L"cast", L"jitter_frames", 8, path.c_str());
    settings.castJitterFrames = std::clamp(jitterFrames, 0, 60);

    wchar_t target[MAX_PATH * 4] = {};
    GetPrivateProfileStringW(L"capture", L"target_exe", L"", target, ARRAYSIZE(target), path.c_str());
    settings.captureTargetExe = target;

    return settings;
}

bool SaveSettings(const AppSettings& settings)
{
    const std::wstring path = SettingsFilePath();

    wchar_t port[8] = {};
    swprintf_s(port, L"%hu", settings.port);

    wchar_t jitter[8] = {};
    swprintf_s(jitter, L"%d", settings.castJitterFrames);

    const BOOL ipSaved =
        WritePrivateProfileStringW(L"cast", L"receiver_ip", settings.receiverIp.c_str(), path.c_str());
    const BOOL portSaved = WritePrivateProfileStringW(L"cast", L"port", port, path.c_str());
    const BOOL jitterSaved =
        WritePrivateProfileStringW(L"cast", L"jitter_frames", jitter, path.c_str());
    const BOOL targetSaved =
        WritePrivateProfileStringW(L"capture", L"target_exe", settings.captureTargetExe.c_str(), path.c_str());
    return ipSaved != FALSE && portSaved != FALSE && jitterSaved != FALSE && targetSaved != FALSE;
}
