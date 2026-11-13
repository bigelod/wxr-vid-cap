// Capture test target: Direct3D 10, spinning cube with a random color per face, bytebeat audio.

#include "common.h"
#include "shaders_generated.h"

#include <d3d10.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d10.lib")

namespace {

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    TestWindow window;
    const bool fullscreen = TestFullscreenRequested();
    if (!CreateTestWindow(window, instance, L"wxr test - D3D10", 800, 450, fullscreen)) {
        return 1;
    }
    const int width = window.width;
    const int height = window.height;
    StartTestAudio();

    DXGI_SWAP_CHAIN_DESC swapDescription = {};
    swapDescription.BufferCount = 1;
    swapDescription.BufferDesc.Width = width;
    swapDescription.BufferDesc.Height = height;
    swapDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.OutputWindow = window.handle;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.Windowed = TRUE;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D10Device* device = nullptr;
    HRESULT result = D3D10CreateDevice(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION, &device);
    if (FAILED(result)) {
        TestLog("D3D10CreateDevice failed 0x%08lX", static_cast<unsigned long>(result));
        wchar_t message[128] = {};
        swprintf_s(message, L"D3D10CreateDevice failed (0x%08lX).", static_cast<unsigned long>(result));
        MessageBoxW(window.handle, message, L"wxr test - D3D10", MB_OK | MB_ICONERROR);
        return 1;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    IDXGIFactory* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));

    IDXGISwapChain* swapChain = nullptr;
    result = factory->CreateSwapChain(device, &swapDescription, &swapChain);
    if (FAILED(result)) {
        TestLog("CreateSwapChain failed 0x%08lX", static_cast<unsigned long>(result));
        wchar_t message[128] = {};
        swprintf_s(message, L"D3D10 CreateSwapChain failed (0x%08lX).", static_cast<unsigned long>(result));
        MessageBoxW(window.handle, message, L"wxr test - D3D10", MB_OK | MB_ICONERROR);
        return 1;
    }
    TestLog("D3D10 device and swapchain created");

    ID3D10Texture2D* backBuffer = nullptr;
    ID3D10RenderTargetView* renderTarget = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&backBuffer));
    device->CreateRenderTargetView(backBuffer, nullptr, &renderTarget);
    backBuffer->Release();

    D3D10_TEXTURE2D_DESC depthDescription = {};
    depthDescription.Width = width;
    depthDescription.Height = height;
    depthDescription.MipLevels = 1;
    depthDescription.ArraySize = 1;
    depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDescription.SampleDesc.Count = 1;
    depthDescription.Usage = D3D10_USAGE_DEFAULT;
    depthDescription.BindFlags = D3D10_BIND_DEPTH_STENCIL;

    ID3D10Texture2D* depthTexture = nullptr;
    ID3D10DepthStencilView* depthView = nullptr;
    device->CreateTexture2D(&depthDescription, nullptr, &depthTexture);
    device->CreateDepthStencilView(depthTexture, nullptr, &depthView);
    depthTexture->Release();

    ID3D10VertexShader* vertexShader = nullptr;
    ID3D10PixelShader* pixelShader = nullptr;
    device->CreateVertexShader(kCubeVs4, kCubeVs4Size, &vertexShader);
    device->CreatePixelShader(kCubePs4, kCubePs4Size, &pixelShader);

    D3D10_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D10_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D10InputLayout* layout = nullptr;
    device->CreateInputLayout(inputLayout, 2, kCubeVs4, kCubeVs4Size, &layout);

    D3D10_BUFFER_DESC bufferDescription = {};
    bufferDescription.Usage = D3D10_USAGE_DYNAMIC;
    bufferDescription.ByteWidth = 64;
    bufferDescription.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    bufferDescription.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    ID3D10Buffer* constantBuffer = nullptr;
    device->CreateBuffer(&bufferDescription, nullptr, &constantBuffer);

    CubeVertexF vertices[24] = {};
    std::uint16_t indices[36] = {};
    BuildCube(vertices, indices);

    std::vector<CubeVertexF> textVertices;
    std::vector<std::uint16_t> textIndices;
    const float textColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    BuildTextMesh("D3D10", 2.0f * 8.0f / width, 2.0f * 8.0f / height, -0.95f, 0.90f, textColor, textVertices, textIndices);

    D3D10_BUFFER_DESC vertexBufferDescription = {};
    vertexBufferDescription.Usage = D3D10_USAGE_IMMUTABLE;
    vertexBufferDescription.ByteWidth = sizeof(vertices);
    vertexBufferDescription.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    D3D10_SUBRESOURCE_DATA vertexData = {};
    vertexData.pSysMem = vertices;
    ID3D10Buffer* vertexBuffer = nullptr;
    device->CreateBuffer(&vertexBufferDescription, &vertexData, &vertexBuffer);

    D3D10_BUFFER_DESC indexBufferDescription = {};
    indexBufferDescription.Usage = D3D10_USAGE_IMMUTABLE;
    indexBufferDescription.ByteWidth = sizeof(indices);
    indexBufferDescription.BindFlags = D3D10_BIND_INDEX_BUFFER;
    D3D10_SUBRESOURCE_DATA indexData = {};
    indexData.pSysMem = indices;
    ID3D10Buffer* indexBuffer = nullptr;
    device->CreateBuffer(&indexBufferDescription, &indexData, &indexBuffer);

    D3D10_BUFFER_DESC textVertexDescription = {};
    textVertexDescription.Usage = D3D10_USAGE_IMMUTABLE;
    textVertexDescription.ByteWidth = static_cast<UINT>(textVertices.size() * sizeof(CubeVertexF));
    textVertexDescription.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    D3D10_SUBRESOURCE_DATA textVertexData = {};
    textVertexData.pSysMem = textVertices.data();
    ID3D10Buffer* textVertexBuffer = nullptr;
    device->CreateBuffer(&textVertexDescription, &textVertexData, &textVertexBuffer);

    D3D10_BUFFER_DESC textIndexDescription = {};
    textIndexDescription.Usage = D3D10_USAGE_IMMUTABLE;
    textIndexDescription.ByteWidth = static_cast<UINT>(textIndices.size() * sizeof(std::uint16_t));
    textIndexDescription.BindFlags = D3D10_BIND_INDEX_BUFFER;
    D3D10_SUBRESOURCE_DATA textIndexData = {};
    textIndexData.pSysMem = textIndices.data();
    ID3D10Buffer* textIndexBuffer = nullptr;
    device->CreateBuffer(&textIndexDescription, &textIndexData, &textIndexBuffer);

    const Mat4 identity = MatIdentity();
    D3D10_BUFFER_DESC textConstantDescription = {};
    textConstantDescription.Usage = D3D10_USAGE_IMMUTABLE;
    textConstantDescription.ByteWidth = 64;
    textConstantDescription.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    D3D10_SUBRESOURCE_DATA textConstantData = {};
    textConstantData.pSysMem = identity.m;
    ID3D10Buffer* textConstantBuffer = nullptr;
    device->CreateBuffer(&textConstantDescription, &textConstantData, &textConstantBuffer);

    D3D10_RASTERIZER_DESC rasterizerDescription = {};
    rasterizerDescription.FillMode = D3D10_FILL_SOLID;
    rasterizerDescription.CullMode = D3D10_CULL_NONE;
    rasterizerDescription.DepthClipEnable = TRUE;
    ID3D10RasterizerState* rasterizer = nullptr;
    device->CreateRasterizerState(&rasterizerDescription, &rasterizer);

    const float eye[3] = {0.0f, 0.9f, -4.2f};
    const float target[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Mat4 view = MatLookAt(eye, target, up);
    const Mat4 projection =
        MatPerspectiveD3D(60.0f * 3.14159265f / 180.0f, static_cast<float>(width) / height, 0.1f, 100.0f);

    D3D10_VIEWPORT viewport = {};
    viewport.Width = width;
    viewport.Height = height;
    viewport.MaxDepth = 1.0f;

    while (PumpTestWindow(window)) {
        const double time = TestTimeSeconds();
        const Mat4 world = MatRotationYawPitch(static_cast<float>(time) * 0.9f, 0.55f);
        const Mat4 worldViewProjection = MatMultiply(MatMultiply(world, view), projection);

        void* mapped = nullptr;
        if (SUCCEEDED(constantBuffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped, worldViewProjection.m, sizeof(worldViewProjection.m));
            constantBuffer->Unmap();
        }

        const float clearColor[4] = {0.094f, 0.102f, 0.125f, 1.0f};
        device->ClearRenderTargetView(renderTarget, clearColor);
        device->ClearDepthStencilView(depthView, D3D10_CLEAR_DEPTH, 1.0f, 0);

        device->OMSetRenderTargets(1, &renderTarget, depthView);
        device->RSSetViewports(1, &viewport);
        device->RSSetState(rasterizer);

        device->IASetInputLayout(layout);
        const UINT stride = sizeof(CubeVertexF);
        const UINT offset = 0;
        device->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        device->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
        device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        device->VSSetShader(vertexShader);
        device->VSSetConstantBuffers(0, 1, &constantBuffer);
        device->PSSetShader(pixelShader);

        device->DrawIndexed(36, 0, 0);

        device->VSSetConstantBuffers(0, 1, &textConstantBuffer);
        device->IASetVertexBuffers(0, 1, &textVertexBuffer, &stride, &offset);
        device->IASetIndexBuffer(textIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
        device->DrawIndexed(static_cast<UINT>(textIndices.size()), 0, 0);

        swapChain->Present(1, 0);

        LimitFrameRate();

        UpdateFpsTitle(window);
    }

    StopTestAudio();

    rasterizer->Release();
    indexBuffer->Release();
    vertexBuffer->Release();
    constantBuffer->Release();
    textConstantBuffer->Release();
    textIndexBuffer->Release();
    textVertexBuffer->Release();
    layout->Release();
    pixelShader->Release();
    vertexShader->Release();
    depthView->Release();
    renderTarget->Release();
    device->Release();
    swapChain->Release();
    return 0;
}
