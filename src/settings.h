#pragma once

#include <string>

struct AppSettings {
    std::wstring receiverIp;
    unsigned short port = 1234;
    // Empty means "capture the top-most window"; otherwise the full path of the EXE to follow.
    // Shared by local recording and LAN casting.
    std::wstring captureTargetExe;
};

// %APPDATA%\wxr-vid-cap\settings.ini, created on first save.
std::wstring SettingsFilePath();

AppSettings LoadSettings();
bool SaveSettings(const AppSettings& settings);
