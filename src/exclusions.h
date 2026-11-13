#pragma once

#include <string>

// Executables that must never be captured or launched with the hook. The built-in defaults are
// Winlator's file manager plus taskmgr/explorer/notepad; an optional capture-blacklist.txt beside
// the EXE replaces them (wfm.exe always stays excluded).
void LoadCaptureBlacklist();

bool IsExcludedExecutableName(const std::wstring& fileName);
bool IsExcludedExecutablePath(const std::wstring& path);
bool IsExcludedProcess(unsigned long processId);
