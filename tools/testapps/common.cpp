#include "common.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace {

constexpr int kAudioSampleRate = 8000;
constexpr int kAudioSeconds = 60;

std::vector<std::uint8_t> g_wav;

void Put16(std::uint8_t* p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void Put32(std::uint8_t* p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

// Bytebeat: ((t/16) | (t^23 + t^420) & (t/16 >> 3)) | t >> 4
//
// JavaScript precedence puts + above ^, and & above |, so the literal expression is
//   ((t/16) | ((t ^ (23 + t) ^ 420) & ((t/16) >> 3))) | (t >> 4)
// with `t/16` truncated to int32 by the bitwise operators. t is a non-negative sample index, so
// t/16 == t >> 4.
std::uint8_t BytebeatSample(std::uint32_t t)
{
    const std::int32_t t16 = static_cast<std::int32_t>(t >> 4);
    const std::int32_t x = static_cast<std::int32_t>(t) ^ static_cast<std::int32_t>(23u + t) ^ 420;
    std::int32_t v = t16 | (x & (t16 >> 3));
    v |= static_cast<std::int32_t>(t >> 4);
    return static_cast<std::uint8_t>(v & 0xFF);
}

void BuildBytebeatWav()
{
    const std::uint32_t sampleCount = static_cast<std::uint32_t>(kAudioSampleRate) * kAudioSeconds;
    const std::uint32_t dataBytes = sampleCount * 2;
    g_wav.resize(44 + dataBytes);

    std::uint8_t* p = g_wav.data();
    p[0] = 'R';
    p[1] = 'I';
    p[2] = 'F';
    p[3] = 'F';
    Put32(p + 4, 36 + dataBytes);
    p[8] = 'W';
    p[9] = 'A';
    p[10] = 'V';
    p[11] = 'E';
    p[12] = 'f';
    p[13] = 'm';
    p[14] = 't';
    p[15] = ' ';
    Put32(p + 16, 16);
    Put16(p + 20, 1);  // PCM
    Put16(p + 22, 1);  // mono
    Put32(p + 24, kAudioSampleRate);
    Put32(p + 28, kAudioSampleRate * 2);
    Put16(p + 32, 2);
    Put16(p + 34, 16);
    p[36] = 'd';
    p[37] = 'a';
    p[38] = 't';
    p[39] = 'a';
    Put32(p + 40, dataBytes);

    for (std::uint32_t t = 0; t < sampleCount; ++t) {
        const std::int16_t sample = static_cast<std::int16_t>((static_cast<int>(BytebeatSample(t)) - 128) * 256);
        Put16(p + 44 + t * 2, static_cast<std::uint16_t>(sample));
    }
}

LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

Mat4 FromRows(float m00, float m01, float m02, float m03, float m10, float m11, float m12, float m13, float m20,
              float m21, float m22, float m23, float m30, float m31, float m32, float m33)
{
    Mat4 result{};
    const float values[16] = {m00, m01, m02, m03, m10, m11, m12, m13,
                              m20, m21, m22, m23, m30, m31, m32, m33};
    std::copy(values, values + 16, result.m);
    return result;
}

void HsvToRgb(float h, float s, float v, float rgb[3])
{
    const float c = v * s;
    const float hp = h / 60.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (hp < 1.0f) {
        r = c;
        g = x;
    } else if (hp < 2.0f) {
        r = x;
        g = c;
    } else if (hp < 3.0f) {
        g = c;
        b = x;
    } else if (hp < 4.0f) {
        g = x;
        b = c;
    } else if (hp < 5.0f) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }
    const float m = v - c;
    rgb[0] = r + m;
    rgb[1] = g + m;
    rgb[2] = b + m;
}

}  // namespace

bool CreateTestWindow(TestWindow& window, HINSTANCE instance, const wchar_t* title, int width, int height,
                      bool fullscreen)
{
    const wchar_t className[] = L"WxrCubeTestWindow";

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = TestWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);

    HWND handle = nullptr;
    if (fullscreen) {
        width = GetSystemMetrics(SM_CXSCREEN);
        height = GetSystemMetrics(SM_CYSCREEN);
        handle = CreateWindowExW(WS_EX_TOPMOST, className, title, WS_POPUP | WS_VISIBLE, 0, 0, width, height, nullptr,
                                 nullptr, instance, nullptr);
    } else {
        RECT rect = {0, 0, width, height};
        AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
        handle = CreateWindowExW(0, className, title,
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE, CW_USEDEFAULT,
                                 CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                 instance, nullptr);
    }
    if (handle == nullptr) {
        return false;
    }

    window.handle = handle;
    window.instance = instance;
    window.width = width;
    window.height = height;
    window.closed = false;
    window.baseTitle = title;

    ShowWindow(handle, SW_SHOW);
    UpdateWindow(handle);
    SetForegroundWindow(handle);
    return true;
}

bool TestFullscreenRequested()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool result = false;
    if (argv != nullptr) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--fullscreen") == 0) {
                result = true;
                break;
            }
        }
        LocalFree(argv);
    }
    return result;
}

bool PumpTestWindow(TestWindow& window)
{
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            window.closed = true;
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return !window.closed;
}

void UpdateFpsTitle(TestWindow& window)
{
    static ULONGLONG windowStart = 0;
    static int frames = 0;
    const ULONGLONG now = GetTickCount64();
    if (windowStart == 0) {
        windowStart = now;
    }
    ++frames;
    if (now - windowStart >= 1000) {
        wchar_t title[160] = {};
        swprintf_s(title, L"%s - %d fps", window.baseTitle, frames);
        SetWindowTextW(window.handle, title);
        frames = 0;
        windowStart = now;
    }
}

void TestLog(const char* format, ...)
{
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring path(modulePath);
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.resize(slash + 1);
    }
    path += L"testapp.log";

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a") != 0 || file == nullptr) {
        return;
    }

    SYSTEMTIME time = {};
    GetLocalTime(&time);
    std::fprintf(file, "%02u:%02u:%02u.%03u ", static_cast<unsigned>(time.wHour),
                 static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
                 static_cast<unsigned>(time.wMilliseconds));

    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(file, format, arguments);
    va_end(arguments);

    std::fprintf(file, "\n");
    std::fclose(file);
}

void LimitFrameRate(int fps)
{
    static LARGE_INTEGER frequency = {};
    static LARGE_INTEGER deadline = {};
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&deadline);
    }

    deadline.QuadPart += frequency.QuadPart / fps;

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (deadline.QuadPart > now.QuadPart) {
        const double seconds = static_cast<double>(deadline.QuadPart - now.QuadPart) / frequency.QuadPart;
        if (seconds > 0.001) {
            Sleep(static_cast<DWORD>(seconds * 1000.0));
        }
    } else {
        deadline = now;
    }
}

double TestTimeSeconds()
{
    static LARGE_INTEGER frequency = {};
    static LARGE_INTEGER start = {};
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
}

void StartTestAudio()
{
    if (g_wav.empty()) {
        BuildBytebeatWav();
    }
    const BOOL started =
        PlaySoundW(reinterpret_cast<LPCWSTR>(g_wav.data()), nullptr, SND_MEMORY | SND_LOOP | SND_ASYNC | SND_NODEFAULT);
    TestLog("audio %s (%u bytes)", started ? "started" : "failed", static_cast<unsigned>(g_wav.size()));
}

void StopTestAudio()
{
    PlaySoundW(nullptr, nullptr, 0);
}

Mat4 MatIdentity()
{
    return FromRows(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
}

Mat4 MatMultiply(const Mat4& a, const Mat4& b)
{
    Mat4 result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[row * 4 + k] * b.m[k * 4 + column];
            }
            result.m[row * 4 + column] = sum;
        }
    }
    return result;
}

Mat4 MatRotationYawPitch(float yaw, float pitch)
{
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);

    const Mat4 ry = FromRows(cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1);
    const Mat4 rx = FromRows(1, 0, 0, 0, 0, cp, sp, 0, 0, -sp, cp, 0, 0, 0, 0, 1);
    return MatMultiply(ry, rx);
}

Mat4 MatLookAt(const float eye[3], const float target[3], const float up[3])
{
    float zaxis[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    float zlength = std::sqrt(zaxis[0] * zaxis[0] + zaxis[1] * zaxis[1] + zaxis[2] * zaxis[2]);
    if (zlength <= 0.0f) {
        zlength = 1.0f;
    }
    for (float& value : zaxis) {
        value /= zlength;
    }

    float xaxis[3] = {up[1] * zaxis[2] - up[2] * zaxis[1], up[2] * zaxis[0] - up[0] * zaxis[2],
                      up[0] * zaxis[1] - up[1] * zaxis[0]};
    float xlength = std::sqrt(xaxis[0] * xaxis[0] + xaxis[1] * xaxis[1] + xaxis[2] * xaxis[2]);
    if (xlength <= 0.0f) {
        xlength = 1.0f;
    }
    for (float& value : xaxis) {
        value /= xlength;
    }

    const float yaxis[3] = {zaxis[1] * xaxis[2] - zaxis[2] * xaxis[1], zaxis[2] * xaxis[0] - zaxis[0] * xaxis[2],
                            zaxis[0] * xaxis[1] - zaxis[1] * xaxis[0]};

    return FromRows(xaxis[0], yaxis[0], zaxis[0], 0, xaxis[1], yaxis[1], zaxis[1], 0, xaxis[2], yaxis[2], zaxis[2], 0,
                    -(xaxis[0] * eye[0] + xaxis[1] * eye[1] + xaxis[2] * eye[2]),
                    -(yaxis[0] * eye[0] + yaxis[1] * eye[1] + yaxis[2] * eye[2]),
                    -(zaxis[0] * eye[0] + zaxis[1] * eye[1] + zaxis[2] * eye[2]), 1);
}

Mat4 MatPerspectiveD3D(float fovYRadians, float aspect, float nearZ, float farZ)
{
    const float h = 1.0f / std::tan(fovYRadians * 0.5f);
    const float w = h / aspect;
    return FromRows(w, 0, 0, 0, 0, h, 0, 0, 0, 0, farZ / (farZ - nearZ), 1, 0, 0,
                    -nearZ * farZ / (farZ - nearZ), 0);
}

Mat4 MatPerspectiveGL(float fovYRadians, float aspect, float nearZ, float farZ)
{
    const float h = 1.0f / std::tan(fovYRadians * 0.5f);
    const float w = h / aspect;
    return FromRows(w, 0, 0, 0, 0, h, 0, 0, 0, 0, (farZ + nearZ) / (farZ - nearZ), 1, 0, 0,
                    -2.0f * farZ * nearZ / (farZ - nearZ), 0);
}

std::uint32_t RandomFaceColor()
{
    static bool seeded = false;
    if (!seeded) {
        std::srand(static_cast<unsigned int>(GetTickCount64()) ^ (GetCurrentProcessId() << 8));
        seeded = true;
    }

    // Golden-angle hue steps keep consecutive faces distinct while still looking random.
    static int faceIndex = 0;
    const float hue = std::fmod(static_cast<float>(std::rand() % 360) + faceIndex * 137.5f, 360.0f);
    ++faceIndex;

    float rgb[3] = {};
    HsvToRgb(hue, 0.85f, 0.95f, rgb);
    const auto channel = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return 0xFF000000u | (channel(rgb[0]) << 16) | (channel(rgb[1]) << 8) | channel(rgb[2]);
}

namespace {

// Faces: +Z, -Z, -X, +X, +Y, -Y. Each is four corners with a per-face color.
void BuildCubeFaces(float positions[24][3])
{
    const float corners[6][4][3] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},      // +Z
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},  // -Z
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},  // -X
        {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},      // +X
        {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},      // +Y
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},  // -Y
    };

    for (int face = 0; face < 6; ++face) {
        for (int corner = 0; corner < 4; ++corner) {
            const int index = face * 4 + corner;
            positions[index][0] = corners[face][corner][0];
            positions[index][1] = corners[face][corner][1];
            positions[index][2] = corners[face][corner][2];
        }
    }
}

}  // namespace

void BuildCube(CubeVertexF vertices[24], std::uint16_t indices[36])
{
    std::uint32_t colors[6] = {};
    for (auto& color : colors) {
        color = RandomFaceColor();
    }

    float positions[24][3] = {};
    BuildCubeFaces(positions);

    for (int face = 0; face < 6; ++face) {
        const float r = ((colors[face] >> 16) & 0xFF) / 255.0f;
        const float g = ((colors[face] >> 8) & 0xFF) / 255.0f;
        const float b = (colors[face] & 0xFF) / 255.0f;
        for (int corner = 0; corner < 4; ++corner) {
            const int index = face * 4 + corner;
            vertices[index].position[0] = positions[index][0];
            vertices[index].position[1] = positions[index][1];
            vertices[index].position[2] = positions[index][2];
            vertices[index].color[0] = r;
            vertices[index].color[1] = g;
            vertices[index].color[2] = b;
            vertices[index].color[3] = 1.0f;
        }
    }

    for (int face = 0; face < 6; ++face) {
        const int base = face * 4;
        const int index = face * 6;
        indices[index + 0] = static_cast<std::uint16_t>(base + 0);
        indices[index + 1] = static_cast<std::uint16_t>(base + 1);
        indices[index + 2] = static_cast<std::uint16_t>(base + 2);
        indices[index + 3] = static_cast<std::uint16_t>(base + 0);
        indices[index + 4] = static_cast<std::uint16_t>(base + 2);
        indices[index + 5] = static_cast<std::uint16_t>(base + 3);
    }
}

void BuildCube(CubeVertexC vertices[24], std::uint16_t indices[36])
{
    CubeVertexF floatVertices[24] = {};
    BuildCube(floatVertices, indices);

    for (int i = 0; i < 24; ++i) {
        vertices[i].position[0] = floatVertices[i].position[0];
        vertices[i].position[1] = floatVertices[i].position[1];
        vertices[i].position[2] = floatVertices[i].position[2];
        const auto channel = [&](int index) {
            return static_cast<std::uint32_t>(std::lround(floatVertices[i].color[index] * 255.0f)) & 0xFF;
        };
        vertices[i].argb = 0xFF000000u | (channel(0) << 16) | (channel(1) << 8) | channel(2);
    }
}

namespace {

struct Glyph {
    char character;
    std::uint8_t rows[7];  // bit 4 = leftmost of 5 columns, top row first
};

const Glyph kGlyphs[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06}},
};

const Glyph* FindGlyph(char character)
{
    for (const Glyph& glyph : kGlyphs) {
        if (glyph.character == character) {
            return &glyph;
        }
    }
    return nullptr;
}

}  // namespace

void BuildTextMesh(const char* text, float pixelWidth, float pixelHeight, float originX, float originY,
                   const float color[4], std::vector<CubeVertexF>& vertices, std::vector<std::uint16_t>& indices)
{
    vertices.clear();
    indices.clear();

    float x = originX;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == ' ') {
            x += 4.0f * pixelWidth;
            continue;
        }
        const Glyph* glyph = FindGlyph(*p);
        if (glyph == nullptr) {
            x += 6.0f * pixelWidth;
            continue;
        }

        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((glyph->rows[row] & (0x10 >> column)) == 0) {
                    continue;
                }
                const float x0 = x + column * pixelWidth;
                const float y0 = originY - row * pixelHeight;
                const float x1 = x0 + pixelWidth * 0.85f;
                const float y1 = y0 - pixelHeight * 0.85f;

                const std::uint16_t base = static_cast<std::uint16_t>(vertices.size());
                vertices.push_back({{x0, y0, 0.0f}, {color[0], color[1], color[2], 1.0f}});
                vertices.push_back({{x1, y0, 0.0f}, {color[0], color[1], color[2], 1.0f}});
                vertices.push_back({{x1, y1, 0.0f}, {color[0], color[1], color[2], 1.0f}});
                vertices.push_back({{x0, y1, 0.0f}, {color[0], color[1], color[2], 1.0f}});
                indices.push_back(static_cast<std::uint16_t>(base + 0));
                indices.push_back(static_cast<std::uint16_t>(base + 1));
                indices.push_back(static_cast<std::uint16_t>(base + 2));
                indices.push_back(static_cast<std::uint16_t>(base + 0));
                indices.push_back(static_cast<std::uint16_t>(base + 2));
                indices.push_back(static_cast<std::uint16_t>(base + 3));
            }
        }
        x += 6.0f * pixelWidth;
    }
}

void BuildTextMesh(const char* text, float pixelWidth, float pixelHeight, float originX, float originY,
                   std::uint32_t argb, std::vector<CubeVertexC>& vertices, std::vector<std::uint16_t>& indices)
{
    const float color[4] = {
        ((argb >> 16) & 0xFF) / 255.0f,
        ((argb >> 8) & 0xFF) / 255.0f,
        (argb & 0xFF) / 255.0f,
        1.0f,
    };

    std::vector<CubeVertexF> floatVertices;
    BuildTextMesh(text, pixelWidth, pixelHeight, originX, originY, color, floatVertices, indices);

    vertices.resize(floatVertices.size());
    for (std::size_t i = 0; i < floatVertices.size(); ++i) {
        vertices[i].position[0] = floatVertices[i].position[0];
        vertices[i].position[1] = floatVertices[i].position[1];
        vertices[i].position[2] = floatVertices[i].position[2];
        vertices[i].argb = argb;
    }
}
