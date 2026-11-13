// Capture test target: Direct3D 11, spinning cube with a random color per face, bytebeat audio.

#include "common.h"
#include "shaders_generated.h"

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d11.lib")

namespace {

struct Constants {
    float worldViewProj[16];
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    TestWindow window;
    const bool fullscreen = TestFullscreenRequested();
    if (!CreateTestWindow(window, instance, L"wxr test - D3D11", 800, 450, fullscreen)) {
        return 1;
    }
    const int width = window.width;
    const int height = window.height;
    StartTestAudio();

    DXGI_SWAP_CHAIN_DESC swapDescription = {};
    swapDescription.BufferCount = 2;
    swapDescription.BufferDesc.Width = width;
    swapDescription.BufferDesc.Height = height;
    swapDescription.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.OutputWindow = window.handle;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.Windowed = TRUE;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;

    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                   D3D11_SDK_VERSION, &swapDescription, &swapChain, &device, &level,
                                                   &context);
    if (FAILED(result)) {
        wchar_t message[128] = {};
        swprintf_s(message, L"D3D11CreateDeviceAndSwapChain failed (0x%08lX).", static_cast<unsigned long>(result));
        MessageBoxW(window.handle, message, L"wxr test - D3D11", MB_OK | MB_ICONERROR);
        return 1;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* renderTarget = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    device->CreateRenderTargetView(backBuffer, nullptr, &renderTarget);
    backBuffer->Release();

    D3D11_TEXTURE2D_DESC depthDescription = {};
    depthDescription.Width = width;
    depthDescription.Height = height;
    depthDescription.MipLevels = 1;
    depthDescription.ArraySize = 1;
    depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDescription.SampleDesc.Count = 1;
    depthDescription.Usage = D3D11_USAGE_DEFAULT;
    depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ID3D11Texture2D* depthTexture = nullptr;
    ID3D11DepthStencilView* depthView = nullptr;
    device->CreateTexture2D(&depthDescription, nullptr, &depthTexture);
    device->CreateDepthStencilView(depthTexture, nullptr, &depthView);
    depthTexture->Release();

    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    device->CreateVertexShader(kCubeVs5, kCubeVs5Size, nullptr, &vertexShader);
    device->CreatePixelShader(kCubePs5, kCubePs5Size, nullptr, &pixelShader);

    D3D11_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout* layout = nullptr;
    device->CreateInputLayout(inputLayout, 2, kCubeVs5, kCubeVs5Size, &layout);

    D3D11_BUFFER_DESC bufferDescription = {};
    bufferDescription.Usage = D3D11_USAGE_DYNAMIC;
    bufferDescription.ByteWidth = sizeof(Constants);
    bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* constantBuffer = nullptr;
    device->CreateBuffer(&bufferDescription, nullptr, &constantBuffer);

    CubeVertexF vertices[24] = {};
    std::uint16_t indices[36] = {};
    BuildCube(vertices, indices);

    std::vector<CubeVertexF> textVertices;
    std::vector<std::uint16_t> textIndices;
    const float textColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    BuildTextMesh("D3D11", 2.0f * 8.0f / width, 2.0f * 8.0f / height, -0.95f, 0.90f, textColor, textVertices, textIndices);

    D3D11_BUFFER_DESC vertexBufferDescription = {};
    vertexBufferDescription.Usage = D3D11_USAGE_IMMUTABLE;
    vertexBufferDescription.ByteWidth = sizeof(vertices);
    vertexBufferDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertexData = {};
    vertexData.pSysMem = vertices;
    ID3D11Buffer* vertexBuffer = nullptr;
    device->CreateBuffer(&vertexBufferDescription, &vertexData, &vertexBuffer);

    D3D11_BUFFER_DESC indexBufferDescription = {};
    indexBufferDescription.Usage = D3D11_USAGE_IMMUTABLE;
    indexBufferDescription.ByteWidth = sizeof(indices);
    indexBufferDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA indexData = {};
    indexData.pSysMem = indices;
    ID3D11Buffer* indexBuffer = nullptr;
    device->CreateBuffer(&indexBufferDescription, &indexData, &indexBuffer);

    D3D11_BUFFER_DESC textVertexDescription = {};
    textVertexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    textVertexDescription.ByteWidth = static_cast<UINT>(textVertices.size() * sizeof(CubeVertexF));
    textVertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA textVertexData = {};
    textVertexData.pSysMem = textVertices.data();
    ID3D11Buffer* textVertexBuffer = nullptr;
    device->CreateBuffer(&textVertexDescription, &textVertexData, &textVertexBuffer);

    D3D11_BUFFER_DESC textIndexDescription = {};
    textIndexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    textIndexDescription.ByteWidth = static_cast<UINT>(textIndices.size() * sizeof(std::uint16_t));
    textIndexDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA textIndexData = {};
    textIndexData.pSysMem = textIndices.data();
    ID3D11Buffer* textIndexBuffer = nullptr;
    device->CreateBuffer(&textIndexDescription, &textIndexData, &textIndexBuffer);

    const Mat4 identity = MatIdentity();
    D3D11_BUFFER_DESC textConstantDescription = {};
    textConstantDescription.Usage = D3D11_USAGE_IMMUTABLE;
    textConstantDescription.ByteWidth = 64;
    textConstantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA textConstantData = {};
    textConstantData.pSysMem = identity.m;
    ID3D11Buffer* textConstantBuffer = nullptr;
    device->CreateBuffer(&textConstantDescription, &textConstantData, &textConstantBuffer);

    D3D11_RASTERIZER_DESC rasterizerDescription = {};
    rasterizerDescription.FillMode = D3D11_FILL_SOLID;
    rasterizerDescription.CullMode = D3D11_CULL_NONE;
    rasterizerDescription.DepthClipEnable = TRUE;
    ID3D11RasterizerState* rasterizer = nullptr;
    device->CreateRasterizerState(&rasterizerDescription, &rasterizer);

    const float eye[3] = {0.0f, 0.9f, -4.2f};
    const float target[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Mat4 view = MatLookAt(eye, target, up);
    const Mat4 projection =
        MatPerspectiveD3D(60.0f * 3.14159265f / 180.0f, static_cast<float>(width) / height, 0.1f, 100.0f);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;

    while (PumpTestWindow(window)) {
        const double time = TestTimeSeconds();
        const Mat4 world = MatRotationYawPitch(static_cast<float>(time) * 0.9f, 0.55f);
        const Mat4 worldViewProjection = MatMultiply(MatMultiply(world, view), projection);

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, worldViewProjection.m, sizeof(worldViewProjection.m));
            context->Unmap(constantBuffer, 0);
        }

        const float clearColor[4] = {0.094f, 0.102f, 0.125f, 1.0f};
        context->ClearRenderTargetView(renderTarget, clearColor);
        context->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH, 1.0f, 0);

        ID3D11RenderTargetView* targets[] = {renderTarget};
        context->OMSetRenderTargets(1, targets, depthView);
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer);

        context->IASetInputLayout(layout);
        const UINT stride = sizeof(CubeVertexF);
        const UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        context->VSSetShader(vertexShader, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &constantBuffer);
        context->PSSetShader(pixelShader, nullptr, 0);

        context->DrawIndexed(36, 0, 0);

        context->VSSetConstantBuffers(0, 1, &textConstantBuffer);
        context->IASetVertexBuffers(0, 1, &textVertexBuffer, &stride, &offset);
        context->IASetIndexBuffer(textIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
        context->DrawIndexed(static_cast<UINT>(textIndices.size()), 0, 0);

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
    context->Release();
    swapChain->Release();
    device->Release();
    return 0;
}
