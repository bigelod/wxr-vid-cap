#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <vector>

// Shared harness for the API capture test apps: a fixed-size window, a spinning cube with a random
// color per face, and a looping bytebeat tone so the recorder has video and audio to capture.

struct TestWindow {
    HWND handle = nullptr;
    HINSTANCE instance = nullptr;
    int width = 0;
    int height = 0;
    bool closed = false;
    const wchar_t* baseTitle = L"";
};

bool CreateTestWindow(TestWindow& window, HINSTANCE instance, const wchar_t* title, int width, int height,
                      bool fullscreen = false);

// True when the process was started with --fullscreen.
bool TestFullscreenRequested();

// Pumps messages and returns false once the window has closed.
bool PumpTestWindow(TestWindow& window);

// Refreshes the window title with a once-per-second frame rate, for measuring capture overhead.
void UpdateFpsTitle(TestWindow& window);

// Caps the render loop at roughly `fps` frames per second. No-op when vsync already limits it.
void LimitFrameRate(int fps = 60);

double TestTimeSeconds();

// Appends a line to testapp.log next to the executable. Test apps only.
void TestLog(const char* format, ...);

// Looping bytebeat through winmm's PlaySound on an in-memory WAV. Call once at startup.
void StartTestAudio();
void StopTestAudio();

struct Mat4 {
    // Row-major storage, row-vector convention: v' = v * M. The same array can be handed to D3D,
// OpenGL, or a GLSL uniform without transposing (see the per-API files).
    float m[16];
};

Mat4 MatIdentity();
Mat4 MatMultiply(const Mat4& a, const Mat4& b);
Mat4 MatRotationYawPitch(float yaw, float pitch);
Mat4 MatLookAt(const float eye[3], const float target[3], const float up[3]);
// D3D clip space, z in [0, 1].
Mat4 MatPerspectiveD3D(float fovYRadians, float aspect, float nearZ, float farZ);
// OpenGL clip space, z in [-1, 1].
Mat4 MatPerspectiveGL(float fovYRadians, float aspect, float nearZ, float farZ);

struct CubeVertexF {
    float position[3];
    float color[4];
};

struct CubeVertexC {
    float position[3];
    std::uint32_t argb;  // 0xAARRGGBB, for D3D8/D3D9/D3D7 fixed function
};

// 24 vertices (4 per face, random face colors) and 36 triangle indices.
void BuildCube(CubeVertexF vertices[24], std::uint16_t indices[36]);
void BuildCube(CubeVertexC vertices[24], std::uint16_t indices[36]);

// 0xAARRGGBB face color for side 0..5 (front, back, left, right, top, bottom).
std::uint32_t RandomFaceColor();

// Builds a screen-space label mesh in normalized device coordinates (x/y in [-1, 1], z = 0) using
// an embedded 5x7 font. Draw it with an identity transform after the cube so the recorded video
// names the renderer. pixelSize is in NDC units (use 2 * 8 / windowHeight for 8-pixel cells).
void BuildTextMesh(const char* text, float pixelWidth, float pixelHeight, float originX, float originY,
                   const float color[4], std::vector<CubeVertexF>& vertices, std::vector<std::uint16_t>& indices);
void BuildTextMesh(const char* text, float pixelWidth, float pixelHeight, float originX, float originY,
                   std::uint32_t argb, std::vector<CubeVertexC>& vertices, std::vector<std::uint16_t>& indices);
