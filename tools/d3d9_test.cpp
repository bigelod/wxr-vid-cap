// Minimal D3D9 test target for the EndScene hook. Renders a slowly changing color until closed.

#include <windows.h>

#include <d3d9.h>

#include <cmath>
#include <cstdio>

namespace {

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    const wchar_t className[] = L"WxrD3D9Test";

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);

    HWND window = CreateWindowExW(0, className, L"wxr d3d9 test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 640, 360, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        return 1;
    }

    IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (d3d9 == nullptr) {
        MessageBoxW(nullptr, L"Direct3DCreate9 failed.", L"wxr d3d9 test", MB_OK | MB_ICONERROR);
        return 1;
    }

    D3DPRESENT_PARAMETERS parameters = {};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
    parameters.BackBufferWidth = 640;
    parameters.BackBufferHeight = 360;
    parameters.hDeviceWindow = window;
    parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* device = nullptr;
    const HRESULT created = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    if (FAILED(created)) {
        MessageBoxW(nullptr, L"D3D9 device creation failed.", L"wxr d3d9 test", MB_OK | MB_ICONERROR);
        d3d9->Release();
        return 1;
    }

    MSG message = {};
    float time = 0.0f;
    const ULONGLONG start = GetTickCount64();
    ULONGLONG fpsStart = start;
    int fpsFrames = 0;

    for (;;) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                device->Release();
                d3d9->Release();
                return 0;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(
            static_cast<int>(127.5f + 127.0f * std::sinf(time)),
            static_cast<int>(127.5f + 127.0f * std::sinf(time + 2.0f)),
            static_cast<int>(127.5f + 127.0f * std::sinf(time + 4.0f))), 1.0f, 0);
        device->BeginScene();
        device->EndScene();
        device->Present(nullptr, nullptr, nullptr, nullptr);

        ++fpsFrames;
        const ULONGLONG now = GetTickCount64();
        if (now - fpsStart >= 1000) {
            wchar_t title[64] = {};
            swprintf_s(title, L"wxr d3d9 test - %d fps", fpsFrames);
            SetWindowTextW(window, title);
            fpsFrames = 0;
            fpsStart = now;
        }

        time += 0.02f;
        if (now - start > 120000) {
            break;
        }
        Sleep(16);
    }

    device->Release();
    d3d9->Release();
    return 0;
}
