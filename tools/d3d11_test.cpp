// Minimal D3D11 test target for the Present hook. Renders a slowly changing color until closed.

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <cmath>

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
    const wchar_t className[] = L"WxrD3D11Test";

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);

    HWND window = CreateWindowExW(0, className, L"wxr d3d11 test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 640, 360, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC description = {};
    description.BufferCount = 2;
    description.BufferDesc.Width = 640;
    description.BufferDesc.Height = 360;
    description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;

    const HRESULT created =
        D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                      &description, &swapChain, &device, &level, &context);
    if (FAILED(created)) {
        MessageBoxW(nullptr, L"D3D11 device creation failed.", L"wxr d3d11 test", MB_OK | MB_ICONERROR);
        return 1;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* renderTarget = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (backBuffer != nullptr) {
        device->CreateRenderTargetView(backBuffer, nullptr, &renderTarget);
        backBuffer->Release();
    }

    MSG message = {};
    float time = 0.0f;
    const ULONGLONG start = GetTickCount64();

    for (;;) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return 0;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (renderTarget != nullptr) {
            const float red = 0.5f + 0.5f * std::sinf(time);
            const float green = 0.5f + 0.5f * std::sinf(time + 2.0f);
            const float blue = 0.5f + 0.5f * std::sinf(time + 4.0f);
            const float color[4] = {red, green, blue, 1.0f};
            context->ClearRenderTargetView(renderTarget, color);
        }

        swapChain->Present(1, 0);
        time += 0.02f;

        if (GetTickCount64() - start > 120000) {
            break;
        }
        Sleep(16);
    }

    if (renderTarget != nullptr) {
        renderTarget->Release();
    }
    if (context != nullptr) {
        context->Release();
    }
    if (swapChain != nullptr) {
        swapChain->Release();
    }
    if (device != nullptr) {
        device->Release();
    }
    return 0;
}
