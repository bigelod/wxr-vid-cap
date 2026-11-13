// Capture test target: plain DirectDraw (no Direct3D). A 2D rectangle rotates over a shaded
// background, with "DDRAW" drawn in the corner, and the same bytebeat audio as the cube tests.
//
// Windowed mode renders into an offscreen back buffer and Blts it to the primary surface. With
// --fullscreen it uses a flipping chain and Flip() instead, so both DirectDraw capture paths can be
// exercised on-device.

#include "common.h"

#include <ddraw.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ddraw.lib")
#pragma comment(lib, "dxguid.lib")

namespace {

struct Glyph {
    char character;
    std::uint8_t rows[7];
};

// Only the letters needed for the "DDRAW" label.
const Glyph kGlyphs[] = {
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
};

std::uint32_t PixelColor(std::uint32_t red, std::uint32_t green, std::uint32_t blue)
{
    return 0xFF000000u | ((red & 0xFFu) << 16) | ((green & 0xFFu) << 8) | (blue & 0xFFu);
}

void PutPixel(std::uint32_t* pixels, int pitchPixels, int width, int height, int x, int y, std::uint32_t color)
{
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(pitchPixels) + static_cast<std::size_t>(x)] = color;
}

void FillRect(std::uint32_t* pixels, int pitchPixels, int width, int height, int x, int y, int rectangleWidth,
              int rectangleHeight, std::uint32_t color)
{
    for (int row = 0; row < rectangleHeight; ++row) {
        for (int column = 0; column < rectangleWidth; ++column) {
            PutPixel(pixels, pitchPixels, width, height, x + column, y + row, color);
        }
    }
}

void DrawText(std::uint32_t* pixels, int pitchPixels, int width, int height, const char* text, int x, int y, int scale,
              std::uint32_t color)
{
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == ' ') {
            x += 4 * scale;
            continue;
        }
        const Glyph* glyph = nullptr;
        for (const Glyph& candidate : kGlyphs) {
            if (candidate.character == *p) {
                glyph = &candidate;
                break;
            }
        }
        if (glyph == nullptr) {
            x += 6 * scale;
            continue;
        }
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((glyph->rows[row] & (0x10 >> column)) != 0) {
                    FillRect(pixels, pitchPixels, width, height, x + column * scale, y + row * scale, scale, scale,
                             color);
                }
            }
        }
        x += 6 * scale;
    }
}

// Draws the rotating rectangle: inverse-rotate each pixel in the bounding box and test the local
// coordinates. The fill color cycles so motion remains obvious even on a still frame.
void DrawRotatingRectangle(std::uint32_t* pixels, int pitchPixels, int width, int height, double time)
{
    const double centerX = width * 0.5;
    const double centerY = height * 0.5;
    const double halfWidth = width * 0.30;
    const double halfHeight = height * 0.22;
    const double angle = time * 0.9;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    const double extent = std::sqrt(halfWidth * halfWidth + halfHeight * halfHeight) + 2.0;

    const int minX = static_cast<int>(centerX - extent);
    const int maxX = static_cast<int>(centerX + extent);
    const int minY = static_cast<int>(centerY - extent);
    const int maxY = static_cast<int>(centerY + extent);

    const std::uint32_t red = static_cast<std::uint32_t>(127.5 + 127.0 * std::sin(time * 1.7));
    const std::uint32_t green = static_cast<std::uint32_t>(127.5 + 127.0 * std::sin(time * 1.3 + 2.1));
    const std::uint32_t blue = static_cast<std::uint32_t>(127.5 + 127.0 * std::sin(time * 1.1 + 4.2));
    const std::uint32_t fill = PixelColor(red, green, blue);
    const std::uint32_t border = PixelColor(255, 255, 255);

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const double dx = (x + 0.5) - centerX;
            const double dy = (y + 0.5) - centerY;
            const double localX = dx * cosine + dy * sine;
            const double localY = -dx * sine + dy * cosine;
            if (std::fabs(localX) > halfWidth || std::fabs(localY) > halfHeight) {
                continue;
            }
            const bool onBorder = std::fabs(localX) > halfWidth - 3.0 || std::fabs(localY) > halfHeight - 3.0;
            PutPixel(pixels, pitchPixels, width, height, x, y, onBorder ? border : fill);
        }
    }
}

void RenderFrame(std::uint32_t* pixels, int pitchPixels, int width, int height, double time)
{
    for (int y = 0; y < height; ++y) {
        std::uint32_t* row = pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(pitchPixels);
        const std::uint32_t shade = static_cast<std::uint32_t>(12 + (y * 28) / (height > 0 ? height : 1));
        for (int x = 0; x < width; ++x) {
            row[x] = PixelColor(shade / 2, shade / 2, shade);
        }
    }

    DrawRotatingRectangle(pixels, pitchPixels, width, height, time);
    DrawText(pixels, pitchPixels, width, height, "DDRAW", 24, 24, 5, PixelColor(255, 255, 255));
}

bool LockAndRender(IDirectDrawSurface7* surface, int width, int height, double time)
{
    DDSURFACEDESC2 description = {};
    description.dwSize = sizeof(description);
    if (FAILED(surface->Lock(nullptr, &description, DDLOCK_WAIT, nullptr))) {
        return false;
    }

    if (description.ddpfPixelFormat.dwRGBBitCount == 32) {
        RenderFrame(static_cast<std::uint32_t*>(description.lpSurface),
                    static_cast<int>(description.lPitch / 4), width, height, time);
    }
    surface->Unlock(nullptr);
    return description.ddpfPixelFormat.dwRGBBitCount == 32;
}

int RunWindowed(TestWindow& window, IDirectDrawSurface7* primary, IDirectDrawSurface7* backBuffer)
{
    RECT destination = {0, 0, window.width, window.height};
    while (PumpTestWindow(window)) {
        const double time = TestTimeSeconds();
        LockAndRender(backBuffer, window.width, window.height, time);
        primary->Blt(&destination, backBuffer, nullptr, DDBLT_WAIT, nullptr);
        LimitFrameRate();
        UpdateFpsTitle(window);
    }
    return 0;
}

int RunFullscreen(TestWindow& window, IDirectDrawSurface7* primary, IDirectDrawSurface7* backBuffer)
{
    while (PumpTestWindow(window)) {
        const double time = TestTimeSeconds();
        LockAndRender(backBuffer, window.width, window.height, time);
        primary->Flip(nullptr, DDFLIP_WAIT);
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
    if (!CreateTestWindow(window, instance, L"wxr test - DDRAW", 800, 450, fullscreen)) {
        return 1;
    }
    const int width = window.width;
    const int height = window.height;
    StartTestAudio();

    IDirectDraw7* directDraw = nullptr;
    HRESULT result = DirectDrawCreateEx(nullptr, reinterpret_cast<LPVOID*>(&directDraw), IID_IDirectDraw7, nullptr);
    if (FAILED(result) || directDraw == nullptr) {
        MessageBoxW(window.handle, L"DirectDrawCreateEx failed.", L"wxr test - DDRAW", MB_OK | MB_ICONERROR);
        return 1;
    }

    IDirectDrawSurface7* primary = nullptr;
    IDirectDrawSurface7* backBuffer = nullptr;

    if (fullscreen) {
        directDraw->SetCooperativeLevel(window.handle, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        if (FAILED(directDraw->SetDisplayMode(width, height, 32, 0, 0))) {
            TestLog("SetDisplayMode(32bpp) failed; trying 16bpp");
            directDraw->SetDisplayMode(width, height, 16, 0, 0);
        }

        DDSURFACEDESC2 description = {};
        description.dwSize = sizeof(description);
        description.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        description.ddsCaps.dwCaps =
            DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY;
        description.dwBackBufferCount = 1;
        if (FAILED(directDraw->CreateSurface(&description, &primary, nullptr))) {
            MessageBoxW(window.handle, L"Could not create the flipping chain.", L"wxr test - DDRAW",
                        MB_OK | MB_ICONERROR);
            return 1;
        }

        DDSCAPS2 caps = {};
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        if (FAILED(primary->GetAttachedSurface(&caps, &backBuffer))) {
            MessageBoxW(window.handle, L"Could not get the back buffer.", L"wxr test - DDRAW",
                        MB_OK | MB_ICONERROR);
            return 1;
        }
    } else {
        directDraw->SetCooperativeLevel(window.handle, DDSCL_NORMAL);

        IDirectDrawClipper* clipper = nullptr;
        directDraw->CreateClipper(0, &clipper, nullptr);
        if (clipper != nullptr) {
            clipper->SetHWnd(0, window.handle);
        }

        DDSURFACEDESC2 description = {};
        description.dwSize = sizeof(description);
        description.dwFlags = DDSD_CAPS;
        description.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        if (FAILED(directDraw->CreateSurface(&description, &primary, nullptr))) {
            MessageBoxW(window.handle, L"Could not create the primary surface.", L"wxr test - DDRAW",
                        MB_OK | MB_ICONERROR);
            return 1;
        }
        if (clipper != nullptr) {
            primary->SetClipper(clipper);
            clipper->Release();
        }

        description = {};
        description.dwSize = sizeof(description);
        description.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        description.dwWidth = width;
        description.dwHeight = height;
        description.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        if (FAILED(directDraw->CreateSurface(&description, &backBuffer, nullptr))) {
            MessageBoxW(window.handle, L"Could not create the back buffer.", L"wxr test - DDRAW",
                        MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    TestLog("ddraw windowed=%s surface %dx%d", fullscreen ? "no" : "yes", width, height);

    const int exitCode = fullscreen ? RunFullscreen(window, primary, backBuffer)
                                    : RunWindowed(window, primary, backBuffer);

    StopTestAudio();
    if (backBuffer != nullptr) {
        backBuffer->Release();
    }
    primary->Release();
    directDraw->Release();
    return exitCode;
}
