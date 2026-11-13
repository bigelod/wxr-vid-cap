// Injection probe DLL. Writes a .hit file next to itself when loaded into a target process, so the
// host can verify that CreateRemoteThread + LoadLibrary works inside the container.

#include <windows.h>

#include <string>

namespace {

void WriteMarker(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) {
        return;
    }

    std::wstring marker(path);
    const std::size_t dot = marker.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        marker.resize(dot);
    }
    marker += L".hit";

    HANDLE file = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    char buffer[64] = {};
    const int length = wsprintfA(buffer, "pid=%lu\n", GetCurrentProcessId());
    DWORD written = 0;
    WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        WriteMarker(module);
    }
    return TRUE;
}
