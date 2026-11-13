#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct ProcessEntry {
    unsigned long processId = 0;
    std::wstring executablePath;
    std::wstring windowTitle;
    HWND window = nullptr;
    long windowArea = 0;
};

// Processes that own at least one visible window, including child windows. This catches apps
// whose game surface is a child of Wine's virtual desktop rather than a top-level window.
std::vector<ProcessEntry> ListWindowProcesses();

// Executable path of the window the user is most likely looking at now: the foreground window's
// process when it has a visible window, otherwise the largest visible non-minimized window.
// Returns an empty string when nothing suitable is running.
std::wstring TopMostWindowExecutable();
