// Capture test target: Direct3D 7 (DirectDraw + D3D7 immediate mode), spinning cube with a random
// color per face, bytebeat audio. Windowed mode with a back buffer blitted to the primary surface.

#define DIRECT3D_VERSION 0x0700

#include "common.h"

#include <ddraw.h>
#include <d3d.h>

#include <cmath>
#include <cstdio>

#pragma comment(lib, "ddraw.lib")
#pragma comment(lib, "dxguid.lib")

namespace {

// d3d.h (the D3D7 SDK header) does not define the D3DCOLOR packing macros.
#ifndef D3DCOLOR_XRGB
#define D3DCOLOR_XRGB(r, g, b) (0xFF000000u | ((static_cast<DWORD>(r)) << 16) | ((static_cast<DWORD>(g)) << 8) | static_cast<DWORD>(b))
#endif

// Windows 11 no longer ships the D3D7 runtime, and some containers lack it too. Fall back to a
// plain DirectDraw back-buffer fill so the Blt capture hook can still be validated.
int Run2DFallback(TestWindow& window, IDirectDrawSurface7* primary, IDirectDrawSurface7* backBuffer)
{
    RECT destination = {0, 0, window.width, window.height};
    while (PumpTestWindow(window)) {
        DDSURFACEDESC2 description = {};
        description.dwSize = sizeof(description);
        if (SUCCEEDED(backBuffer->Lock(nullptr, &description, DDLOCK_WAIT, nullptr))) {
            if (description.ddpfPixelFormat.dwRGBBitCount == 32) {
                const int width = static_cast<int>(description.dwWidth);
                const int height = static_cast<int>(description.dwHeight);
                const double time = TestTimeSeconds();
                for (int y = 0; y < height; ++y) {
                    auto* row = reinterpret_cast<std::uint32_t*>(
                        static_cast<std::uint8_t*>(description.lpSurface) +
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(description.lPitch));
                    for (int x = 0; x < width; ++x) {
                        const int red = static_cast<int>(127.5 + 127.0 * std::sin(time * 2.0 + x * 0.02));
                        const int green = static_cast<int>(127.5 + 127.0 * std::sin(time * 2.0 + y * 0.02));
                        const int blue =
                            static_cast<int>(127.5 + 127.0 * std::sin(time * 2.0 + (x + y) * 0.01));
                        row[x] = 0xFF000000u | (static_cast<std::uint32_t>(red) << 16) |
                                 (static_cast<std::uint32_t>(green) << 8) | static_cast<std::uint32_t>(blue);
                    }
                }
            }
            backBuffer->Unlock(nullptr);
        }
        primary->Blt(&destination, backBuffer, nullptr, DDBLT_WAIT, nullptr);
        LimitFrameRate();
        UpdateFpsTitle(window);
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    TestWindow window;
    const bool fullscreen = TestFullscreenRequested();
    if (!CreateTestWindow(window, instance, L"wxr test - D3D7", 800, 450, fullscreen)) {
        return 1;
    }
    const int width = window.width;
    const int height = window.height;
    StartTestAudio();

    IDirectDraw7* directDraw = nullptr;
    HRESULT result = DirectDrawCreateEx(nullptr, reinterpret_cast<LPVOID*>(&directDraw), IID_IDirectDraw7, nullptr);
    if (FAILED(result) || directDraw == nullptr) {
        MessageBoxW(window.handle, L"DirectDrawCreateEx failed.", L"wxr test - D3D7", MB_OK | MB_ICONERROR);
        return 1;
    }

    directDraw->SetCooperativeLevel(window.handle, DDSCL_NORMAL);

    IDirectDrawClipper* clipper = nullptr;
    directDraw->CreateClipper(0, &clipper, nullptr);
    clipper->SetHWnd(0, window.handle);

    DDSURFACEDESC2 description = {};
    description.dwSize = sizeof(description);
    description.dwFlags = DDSD_CAPS;
    description.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    IDirectDrawSurface7* primary = nullptr;
    if (FAILED(directDraw->CreateSurface(&description, &primary, nullptr))) {
        MessageBoxW(window.handle, L"Could not create the primary surface.", L"wxr test - D3D7",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    primary->SetClipper(clipper);

    description = {};
    description.dwSize = sizeof(description);
    description.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    description.dwWidth = width;
    description.dwHeight = height;
    description.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;

    IDirectDrawSurface7* backBuffer = nullptr;
    result = directDraw->CreateSurface(&description, &backBuffer, nullptr);
    if (FAILED(result)) {
        TestLog("back buffer CreateSurface failed 0x%08lX", static_cast<unsigned long>(result));
        MessageBoxW(window.handle, L"Could not create the back buffer.", L"wxr test - D3D7", MB_OK | MB_ICONERROR);
        return 1;
    }

    description = {};
    description.dwSize = sizeof(description);
    description.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    description.dwWidth = width;
    description.dwHeight = height;
    description.ddsCaps.dwCaps = DDSCAPS_ZBUFFER;
    description.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    description.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
    description.ddpfPixelFormat.dwZBufferBitDepth = 32;

    IDirectDrawSurface7* depthBuffer = nullptr;
    bool hasDepth = false;
    result = directDraw->CreateSurface(&description, &depthBuffer, nullptr);
    if (SUCCEEDED(result) && SUCCEEDED(backBuffer->AddAttachedSurface(depthBuffer))) {
        hasDepth = true;
    } else {
        // Some DDraw implementations reject Z-buffer surfaces. Fall back to a painter's-algorithm
        // sort in the render loop so the cube still looks correct.
        TestLog("depth buffer unavailable (0x%08lX); using painter's algorithm",
                static_cast<unsigned long>(result));
        if (depthBuffer != nullptr) {
            depthBuffer->Release();
            depthBuffer = nullptr;
        }
    }

    IDirect3D7* d3d = nullptr;
    if (FAILED(directDraw->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d)))) {
        TestLog("Direct3D7 unavailable; using the DirectDraw 2D fallback");
        return Run2DFallback(window, primary, backBuffer);
    }

    IDirect3DDevice7* device = nullptr;
    result = d3d->CreateDevice(IID_IDirect3DHALDevice, backBuffer, &device);
    if (FAILED(result)) {
        result = d3d->CreateDevice(IID_IDirect3DRGBDevice, backBuffer, &device);
    }
    if (FAILED(result) || device == nullptr) {
        TestLog("D3D7 CreateDevice failed (0x%08lX); using the DirectDraw 2D fallback",
                static_cast<unsigned long>(result));
        return Run2DFallback(window, primary, backBuffer);
    }

    CubeVertexC vertices[24] = {};
    std::uint16_t indices[36] = {};
    BuildCube(vertices, indices);

    std::vector<CubeVertexC> textVertices;
    std::vector<std::uint16_t> textIndices;
    BuildTextMesh("D3D7", 2.0f * 8.0f / width, 2.0f * 8.0f / height, -0.95f, 0.90f, 0xFFFFFFFFu, textVertices, textIndices);

    device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRENDERSTATE_ZENABLE, hasDepth ? TRUE : FALSE);

    const float eye[3] = {0.0f, 0.9f, -4.2f};
    const float target[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Mat4 view = MatLookAt(eye, target, up);
    const Mat4 projection =
        MatPerspectiveD3D(60.0f * 3.14159265f / 180.0f, static_cast<float>(width) / height, 0.1f, 100.0f);

    RECT destination = {0, 0, width, height};

    while (PumpTestWindow(window)) {
        const double time = TestTimeSeconds();
        const Mat4 world = MatRotationYawPitch(static_cast<float>(time) * 0.9f, 0.55f);

        device->Clear(0, nullptr, hasDepth ? (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER) : D3DCLEAR_TARGET,
                      D3DCOLOR_XRGB(24, 26, 32), 1.0f, 0);
        device->BeginScene();
        device->SetTransform(D3DTRANSFORMSTATE_WORLD, reinterpret_cast<LPD3DMATRIX>(const_cast<float*>(world.m)));
        device->SetTransform(D3DTRANSFORMSTATE_VIEW, reinterpret_cast<LPD3DMATRIX>(const_cast<float*>(view.m)));
        device->SetTransform(D3DTRANSFORMSTATE_PROJECTION,
                             reinterpret_cast<LPD3DMATRIX>(const_cast<float*>(projection.m)));

        if (hasDepth) {
            device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZ | D3DFVF_DIFFUSE, vertices, 24, indices, 36,
                                         0);
        } else {
            // Painter's algorithm: sort faces far to near in view space.
            const Mat4 viewWorld = MatMultiply(world, view);
            int order[6] = {0, 1, 2, 3, 4, 5};
            float depth[6] = {};
            for (int face = 0; face < 6; ++face) {
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
                for (int corner = 0; corner < 4; ++corner) {
                    x += vertices[face * 4 + corner].position[0];
                    y += vertices[face * 4 + corner].position[1];
                    z += vertices[face * 4 + corner].position[2];
                }
                x *= 0.25f;
                y *= 0.25f;
                z *= 0.25f;
                depth[face] = x * viewWorld.m[2] + y * viewWorld.m[6] + z * viewWorld.m[10] + viewWorld.m[14];
            }
            for (int i = 0; i < 6; ++i) {
                for (int j = i + 1; j < 6; ++j) {
                    if (depth[order[j]] > depth[order[i]]) {
                        const int swap = order[i];
                        order[i] = order[j];
                        order[j] = swap;
                    }
                }
            }
            for (int face : order) {
                device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZ | D3DFVF_DIFFUSE, vertices, 24,
                                             indices + face * 6, 6, 0);
            }
        }

        const Mat4 identity = MatIdentity();
        device->SetTransform(D3DTRANSFORMSTATE_WORLD, reinterpret_cast<LPD3DMATRIX>(const_cast<float*>(identity.m)));
        device->SetTransform(D3DTRANSFORMSTATE_VIEW, reinterpret_cast<LPD3DMATRIX>(const_cast<float*>(identity.m)));
        device->SetTransform(D3DTRANSFORMSTATE_PROJECTION,
                             reinterpret_cast<LPD3DMATRIX>(const_cast<float*>(identity.m)));
        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZ | D3DFVF_DIFFUSE, textVertices.data(),
                                     static_cast<DWORD>(textVertices.size()), textIndices.data(),
                                     static_cast<DWORD>(textIndices.size()), 0);

        device->EndScene();

        primary->Blt(&destination, backBuffer, nullptr, DDBLT_WAIT, nullptr);
        LimitFrameRate();
        UpdateFpsTitle(window);
    }

    StopTestAudio();
    device->Release();
    d3d->Release();
    depthBuffer->Release();
    backBuffer->Release();
    primary->Release();
    clipper->Release();
    directDraw->Release();
    return 0;
}
