// Direct3D 8 capture hook.
//
// D3D8 presents through IDirect3DDevice8::Present (vtable slot 18). The back buffer is read with
// GetBackBuffer + LockRect, which works for both D3D8 and D3D8-to-D3D9 wrappers.
//
// This lives in its own translation unit because Wine's d3d8 headers and the Windows SDK's d3d9.h
// both define D3DPRESENT_PARAMETERS and other types.

#define __MSABI_LONG(x) x

#include "hook_d3d8.h"

#include <windows.h>

#include <d3d8.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "MinHook.h"
#include "shared_frame.h"

namespace {

using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);

SharedFrameWriter* g_writer = nullptr;
bool (*g_shouldCapture)() = nullptr;
void (*g_log)(const char*) = nullptr;

PresentFn g_originalPresent = nullptr;
std::mutex g_mutex;
int g_frameCount = 0;
int g_openRetry = 0;

void LogText(const std::string& text)
{
    if (g_log != nullptr) {
        g_log(text.c_str());
    }
}

void* FindPresentAddress()
{
    HMODULE d3d8Module = GetModuleHandleW(L"d3d8.dll");
    if (d3d8Module == nullptr) {
        return nullptr;
    }
    auto create8 = reinterpret_cast<IDirect3D8*(WINAPI*)(UINT)>(GetProcAddress(d3d8Module, "Direct3DCreate8"));
    if (create8 == nullptr) {
        return nullptr;
    }

    IDirect3D8* d3d = create8(D3D_SDK_VERSION);
    if (d3d == nullptr) {
        LogText("d3d8 Direct3DCreate8 failed");
        return nullptr;
    }

    const wchar_t className[] = L"WxrHookDummy8";
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);

    HWND window = CreateWindowExW(0, className, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr) {
        d3d->Release();
        return nullptr;
    }

    D3DPRESENT_PARAMETERS parameters = {};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
    parameters.BackBufferWidth = 2;
    parameters.BackBufferHeight = 2;
    parameters.hDeviceWindow = window;

    IDirect3DDevice8* device = nullptr;
    HRESULT result = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    if (FAILED(result)) {
        result = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, window,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    }

    void* present = nullptr;
    if (SUCCEEDED(result) && device != nullptr) {
        present = (*reinterpret_cast<void***>(device))[18];
        device->Release();
    } else {
        LogText("d3d8 dummy CreateDevice failed: " + std::to_string(static_cast<unsigned long>(result)));
    }

    d3d->Release();
    DestroyWindow(window);
    UnregisterClassW(className, GetModuleHandleW(nullptr));
    return present;
}

void Capture(IDirect3DDevice8* device)
{
    if (g_writer == nullptr) {
        return;
    }
    if (!g_writer->Enabled()) {
        if (--g_openRetry <= 0) {
            g_openRetry = 30;
            g_writer->Open();
        }
        return;
    }
    if (g_shouldCapture != nullptr && !g_shouldCapture()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    IDirect3DSurface8* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
        return;
    }

    D3DSURFACE_DESC description = {};
    if (FAILED(backBuffer->GetDesc(&description)) ||
        (description.Format != D3DFMT_X8R8G8B8 && description.Format != D3DFMT_A8R8G8B8)) {
        backBuffer->Release();
        return;
    }

    D3DLOCKED_RECT locked = {};
    if (FAILED(backBuffer->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        backBuffer->Release();
        return;
    }

    g_writer->Write(static_cast<const std::uint8_t*>(locked.pBits), static_cast<int>(description.Width),
                    static_cast<int>(description.Height), static_cast<int>(locked.Pitch),
                    static_cast<std::int64_t>(GetTickCount64()));

    backBuffer->UnlockRect();
    backBuffer->Release();

    ++g_frameCount;
    if (g_frameCount == 1 || (g_frameCount % 120) == 0) {
        LogText("d3d8 frame " + std::to_string(g_frameCount) + ": " + std::to_string(description.Width) + "x" +
                std::to_string(description.Height));
    }
}

HRESULT WINAPI PresentDetour(IDirect3DDevice8* device, const RECT* sourceRect, const RECT* destRect, HWND window,
                             const RGNDATA* dirtyRegion)
{
    Capture(device);
    return g_originalPresent != nullptr ? g_originalPresent(device, sourceRect, destRect, window, dirtyRegion)
                                        : D3DERR_INVALIDCALL;
}

}  // namespace

bool InstallD3D8Hooks(SharedFrameWriter* writer, bool (*shouldCapture)(), void (*log)(const char*))
{
    if (GetModuleHandleW(L"d3d8.dll") == nullptr) {
        return false;
    }
    if (g_originalPresent != nullptr) {
        return true;
    }

    g_writer = writer;
    g_shouldCapture = shouldCapture;
    g_log = log;

    void* present = FindPresentAddress();
    if (present == nullptr) {
        LogText("d3d8 Present address not found");
        return false;
    }

    const MH_STATUS created =
        MH_CreateHook(present, reinterpret_cast<LPVOID>(&PresentDetour), reinterpret_cast<LPVOID*>(&g_originalPresent));
    if (created != MH_OK) {
        LogText("d3d8 MH_CreateHook failed: " + std::to_string(static_cast<int>(created)));
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(present);
    LogText("d3d8 Present hooked");
    return enabled == MH_OK;
}
