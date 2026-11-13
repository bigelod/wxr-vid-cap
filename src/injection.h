#pragma once

#include <string>

// Finds the first process whose full image path (or file name, when the input has no directory)
// matches. Returns 0 when not found.
unsigned long FindProcessByExecutable(const std::wstring& executablePath);

// Injects a DLL with CreateRemoteThread + LoadLibraryW. Requires an x64 target and an x64 DLL.
bool InjectDll(unsigned long processId, const std::wstring& dllPath, std::wstring& error);

// True when the target is a 32-bit process (WOW64 / x86 under Box64).
bool IsTarget32Bit(unsigned long processId);
