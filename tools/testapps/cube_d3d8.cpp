// Capture test target: Direct3D 8, spinning cube with a random color per face, bytebeat audio.
//
// Uses the Wine d3d8 headers in third_party/wine (test tools only) and loads d3d8.dll at runtime,
// so no import library is needed.

#define __MSABI_LONG(x) x

#include "common.h"

#include <d3d8.h>

#include <cmath>
#include <cstdio>

namespace {

using Direct3DCreate8Fn = IDirect3D8*(WINAPI*)(UINT);

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    TestWindow window;
    const bool fullscreen = TestFullscreenRequested();
    if (!CreateTestWindow(window, instance, L"wxr test - D3D8", 800, 450, fullscreen)) {
        return 1;
    }
    const int width = window.width;
    const int height = window.height;
    StartTestAudio();

    HMODULE d3d8Module = LoadLibraryW(L"d3d8.dll");
    if (d3d8Module == nullptr) {
        MessageBoxW(window.handle, L"d3d8.dll is not available.", L"wxr test - D3D8", MB_OK | MB_ICONERROR);
        return 1;
    }

    auto create8 = reinterpret_cast<Direct3DCreate8Fn>(GetProcAddress(d3d8Module, "Direct3DCreate8"));
    if (create8 == nullptr) {
        MessageBoxW(window.handle, L"Direct3DCreate8 was not found in d3d8.dll.", L"wxr test - D3D8",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    IDirect3D8* d3d = create8(D3D_SDK_VERSION);
    if (d3d == nullptr) {
        MessageBoxW(window.handle, L"Direct3DCreate8 failed.", L"wxr test - D3D8", MB_OK | MB_ICONERROR);
        return 1;
    }

    D3DPRESENT_PARAMETERS parameters = {};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
    parameters.BackBufferWidth = width;
    parameters.BackBufferHeight = height;
    parameters.EnableAutoDepthStencil = TRUE;
    parameters.AutoDepthStencilFormat = D3DFMT_D16;
    parameters.hDeviceWindow = window.handle;
    parameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice8* device = nullptr;
    HRESULT result = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window.handle,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    if (FAILED(result)) {
        wchar_t message[128] = {};
        swprintf_s(message, L"D3D8 CreateDevice failed (0x%08lX).", static_cast<unsigned long>(result));
        MessageBoxW(window.handle, message, L"wxr test - D3D8", MB_OK | MB_ICONERROR);
        d3d->Release();
        return 1;
    }

    CubeVertexC vertices[24] = {};
    std::uint16_t indices[36] = {};
    BuildCube(vertices, indices);

    std::vector<CubeVertexC> textVertices;
    std::vector<std::uint16_t> textIndices;
    BuildTextMesh("D3D8", 2.0f * 8.0f / width, 2.0f * 8.0f / height, -0.95f, 0.90f, 0xFFFFFFFFu, textVertices, textIndices);

    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);

    const float eye[3] = {0.0f, 0.9f, -4.2f};
    const float target[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Mat4 view = MatLookAt(eye, target, up);
    const Mat4 projection =
        MatPerspectiveD3D(60.0f * 3.14159265f / 180.0f, static_cast<float>(width) / height, 0.1f, 100.0f);

    while (PumpTestWindow(window)) {
        const double time = TestTimeSeconds();
        const Mat4 world = MatRotationYawPitch(static_cast<float>(time) * 0.9f, 0.55f);

        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(24, 26, 32), 1.0f, 0);
        device->BeginScene();
        device->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(&world));
        device->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(&view));
        device->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(&projection));
        device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 24, 12, indices, D3DFMT_INDEX16, vertices,
                                       sizeof(CubeVertexC));

        const Mat4 identity = MatIdentity();
        device->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(&identity));
        device->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(&identity));
        device->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(&identity));
        device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, static_cast<UINT>(textVertices.size()),
                                       static_cast<UINT>(textIndices.size() / 3), textIndices.data(), D3DFMT_INDEX16,
                                       textVertices.data(), sizeof(CubeVertexC));
        device->EndScene();
        device->Present(nullptr, nullptr, nullptr, nullptr);

        LimitFrameRate();

        UpdateFpsTitle(window);
    }

    StopTestAudio();
    device->Release();
    d3d->Release();
    FreeLibrary(d3d8Module);
    return 0;
}
