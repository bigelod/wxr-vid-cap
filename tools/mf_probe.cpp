// Media Foundation availability probe for Winlator containers.
//
// Loads mfplat.dll dynamically (so the EXE still starts when MF is absent), initializes MF, and
// enumerates the encoders/decoders we would need. Writes mf_probe.txt next to the EXE and shows a
// completion message box. Console subsystem so output is visible if a console is available.

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <objbase.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#pragma comment(lib, "Mfuuid.lib")  // GUID definitions only; no DLL import
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "User32.lib")

namespace {

std::string g_log;

void Log(const char* format, ...)
{
    char buffer[2048] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    std::fputs(buffer, stdout);
    std::fflush(stdout);
    g_log += buffer;
}

std::wstring ExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0) {
        return L".";
    }
    std::wstring result(path, length);
    const std::size_t slash = result.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return result.substr(0, slash);
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
    if (size > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    }
    return result;
}

void CheckModule(const wchar_t* name)
{
    HMODULE module = LoadLibraryW(name);
    Log("%ls: %s\n", name, module != nullptr ? "present" : "MISSING");
    if (module != nullptr) {
        FreeLibrary(module);
    }
}

using MftEnumExFn = HRESULT(WINAPI*)(REFGUID category, UINT32 flags, const MFT_REGISTER_TYPE_INFO* inputType,
                                     const MFT_REGISTER_TYPE_INFO* outputType, IMFActivate*** activates,
                                     UINT32* count);

void Enumerate(MftEnumExFn enumerate, const char* label, REFGUID category, UINT32 flags,
               const MFT_REGISTER_TYPE_INFO* inputType, const MFT_REGISTER_TYPE_INFO* outputType)
{
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT result = enumerate(category, flags, inputType, outputType, &activates, &count);

    Log("%s: hr=0x%08X count=%u\n", label, static_cast<unsigned>(result), count);

    for (UINT32 i = 0; i < count; ++i) {
        wchar_t name[256] = {};
        UINT32 nameLength = 0;
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, ARRAYSIZE(name), &nameLength);

        IMFTransform* transform = nullptr;
        const HRESULT activated = activates[i]->ActivateObject(IID_PPV_ARGS(&transform));
        Log("  [%u] %ls  activate=0x%08X\n", i, name[0] != L'\0' ? name : L"(unnamed)",
            static_cast<unsigned>(activated));
        if (transform != nullptr) {
            transform->Release();
        }
        activates[i]->Release();
    }

    if (activates != nullptr) {
        CoTaskMemFree(activates);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        }
    }

    Log("wxr-vid-cap Media Foundation probe\n");
    Log("-----------------------------------\n");

    CheckModule(L"mfplat.dll");
    CheckModule(L"mf.dll");
    CheckModule(L"mfreadwrite.dll");
    CheckModule(L"evr.dll");
    Log("\n");

    const HMODULE mfplat = LoadLibraryW(L"mfplat.dll");
    if (mfplat == nullptr) {
        Log("Media Foundation is NOT available: mfplat.dll could not be loaded (error %lu).\n",
            static_cast<unsigned long>(GetLastError()));
    } else {
        auto startup = reinterpret_cast<HRESULT(WINAPI*)(ULONG, DWORD)>(GetProcAddress(mfplat, "MFStartup"));
        auto shutdown = reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(mfplat, "MFShutdown"));
        auto enumerate = reinterpret_cast<MftEnumExFn>(GetProcAddress(mfplat, "MFTEnumEx"));

        Log("MFStartup export: %s\n", startup != nullptr ? "yes" : "MISSING");
        Log("MFShutdown export: %s\n", shutdown != nullptr ? "yes" : "MISSING");
        Log("MFTEnumEx export: %s\n", enumerate != nullptr ? "yes" : "MISSING");
        Log("MF_VERSION: 0x%08X\n\n", static_cast<unsigned>(MF_VERSION));

        if (startup == nullptr || enumerate == nullptr) {
            Log("Media Foundation exports are incomplete; encoders cannot be enumerated.\n");
        } else {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            Log("CoInitializeEx: 0x%08X\n", static_cast<unsigned>(com));

            const HRESULT started = startup(MF_VERSION, MFSTARTUP_NOSOCKET);
            Log("MFStartup: 0x%08X %s\n\n", static_cast<unsigned>(started),
                SUCCEEDED(started) ? "(S_OK)" : "(failed)");

            if (SUCCEEDED(started)) {
                const MFT_REGISTER_TYPE_INFO h264Output = {MFMediaType_Video, MFVideoFormat_H264};
                const MFT_REGISTER_TYPE_INFO h264Input = {MFMediaType_Video, MFVideoFormat_H264};
                const MFT_REGISTER_TYPE_INFO aacOutput = {MFMediaType_Audio, MFAudioFormat_AAC};

                Enumerate(enumerate, "H.264 video encoders (all)", MFT_CATEGORY_VIDEO_ENCODER,
                          MFT_ENUM_FLAG_ALL, nullptr, &h264Output);
                Enumerate(enumerate, "H.264 video encoders (hardware)", MFT_CATEGORY_VIDEO_ENCODER,
                          MFT_ENUM_FLAG_HARDWARE, nullptr, &h264Output);
                Enumerate(enumerate, "AAC audio encoders (all)", MFT_CATEGORY_AUDIO_ENCODER,
                          MFT_ENUM_FLAG_ALL, nullptr, &aacOutput);
                Enumerate(enumerate, "H.264 video decoders (all)", MFT_CATEGORY_VIDEO_DECODER,
                          MFT_ENUM_FLAG_ALL, &h264Input, nullptr);

                if (shutdown != nullptr) {
                    shutdown();
                }
            }

            if (SUCCEEDED(com)) {
                CoUninitialize();
            }
        }

        FreeLibrary(mfplat);
    }

    const std::wstring logPath = ExecutableDirectory() + L"\\mf_probe.txt";
    std::ofstream file(Narrow(logPath), std::ios::binary);
    if (file) {
        file << g_log;
        Log("\nLog written to: %ls\n", logPath.c_str());
    } else {
        Log("\nCould not write the log file next to the EXE.\n");
    }

    if (!quiet) {
        MessageBoxW(nullptr, L"Media Foundation probe finished.\n\nThe log is mf_probe.txt next to this EXE.",
                    L"wxr-vid-cap probe", MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}
