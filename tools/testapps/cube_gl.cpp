// Capture test target: OpenGL (compatibility profile), spinning cube with a random color per
// face, bytebeat audio. Uses immediate mode so it runs on the OpenGL 1.1 that opengl32 guarantees.

#include "common.h"

#include <windows.h>

#include <GL/gl.h>

#include <cmath>
#include <cstdio>

#pragma comment(lib, "opengl32.lib")

namespace {

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    TestWindow window;
    const bool fullscreen = TestFullscreenRequested();
    if (!CreateTestWindow(window, instance, L"wxr test - OpenGL", 800, 450, fullscreen)) {
        return 1;
    }
    const int width = window.width;
    const int height = window.height;
    StartTestAudio();

    HDC deviceContext = GetDC(window.handle);

    PIXELFORMATDESCRIPTOR pixelFormat = {};
    pixelFormat.nSize = sizeof(pixelFormat);
    pixelFormat.nVersion = 1;
    pixelFormat.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pixelFormat.iPixelType = PFD_TYPE_RGBA;
    pixelFormat.cColorBits = 32;
    pixelFormat.cDepthBits = 24;
    pixelFormat.iLayerType = PFD_MAIN_PLANE;

    const int format = ChoosePixelFormat(deviceContext, &pixelFormat);
    if (format == 0 || !SetPixelFormat(deviceContext, format, &pixelFormat)) {
        MessageBoxW(window.handle, L"Could not set an OpenGL pixel format.", L"wxr test - OpenGL",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    HGLRC renderContext = wglCreateContext(deviceContext);
    if (renderContext == nullptr || !wglMakeCurrent(deviceContext, renderContext)) {
        MessageBoxW(window.handle, L"Could not create an OpenGL context.", L"wxr test - OpenGL",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    CubeVertexF vertices[24] = {};
    std::uint16_t indices[36] = {};
    BuildCube(vertices, indices);

    std::vector<CubeVertexF> textVertices;
    std::vector<std::uint16_t> textIndices;
    const float textColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    BuildTextMesh("OPENGL", 2.0f * 8.0f / width, 2.0f * 8.0f / height, -0.95f, 0.90f, textColor, textVertices, textIndices);

    const float eye[3] = {0.0f, 0.9f, -4.2f};
    const float target[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Mat4 view = MatLookAt(eye, target, up);
    const Mat4 projection =
        MatPerspectiveGL(60.0f * 3.14159265f / 180.0f, static_cast<float>(width) / height, 0.1f, 100.0f);

    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    while (PumpTestWindow(window)) {
        const double time = TestTimeSeconds();
        const Mat4 world = MatRotationYawPitch(static_cast<float>(time) * 0.9f, 0.55f);

        glClearColor(0.094f, 0.102f, 0.125f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Row-major row-vector matrices handed straight to GL become the transpose GL expects.
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(projection.m);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(view.m);
        glMultMatrixf(world.m);

        for (int face = 0; face < 6; ++face) {
            const CubeVertexF& first = vertices[face * 4];
            glColor4f(first.color[0], first.color[1], first.color[2], 1.0f);
            glBegin(GL_QUADS);
            for (int corner = 0; corner < 4; ++corner) {
                const CubeVertexF& vertex = vertices[face * 4 + corner];
                glVertex3f(vertex.position[0], vertex.position[1], vertex.position[2]);
            }
            glEnd();
        }

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        for (std::size_t i = 0; i + 3 < textVertices.size(); i += 4) {
            for (int corner = 0; corner < 4; ++corner) {
                const CubeVertexF& vertex = textVertices[i + static_cast<std::size_t>(corner)];
                glVertex3f(vertex.position[0], vertex.position[1], vertex.position[2]);
            }
        }
        glEnd();

        SwapBuffers(deviceContext);
        LimitFrameRate();
        UpdateFpsTitle(window);
    }

    StopTestAudio();
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(renderContext);
    ReleaseDC(window.handle, deviceContext);
    return 0;
}
