// Capture test target: Direct3D 12, spinning cube with a random color per face, bytebeat audio.

#include "common.h"
#include "shaders_generated.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

ID3D12Resource* CreateUploadBuffer(ID3D12Device* device, const void* data, UINT size)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC description = {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* resource = nullptr;
    const HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&resource));
    if (FAILED(result)) {
        TestLog("CreateCommittedResource(upload, %u bytes) failed 0x%08lX", size, static_cast<unsigned long>(result));
        return nullptr;
    }

    void* mapped = nullptr;
    if (SUCCEEDED(resource->Map(0, nullptr, &mapped))) {
        std::memcpy(mapped, data, size);
        resource->Unmap(0, nullptr);
    }
    return resource;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    TestWindow window;
    const bool fullscreen = TestFullscreenRequested();
    if (!CreateTestWindow(window, instance, L"wxr test - D3D12", 800, 450, fullscreen)) {
        return 1;
    }
    const int width = window.width;
    const int height = window.height;
    StartTestAudio();

    ID3D12Device* device = nullptr;
    HRESULT result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(result)) {
        TestLog("D3D12CreateDevice failed 0x%08lX", static_cast<unsigned long>(result));
        wchar_t message[160] = {};
        swprintf_s(message, L"D3D12CreateDevice failed (0x%08lX).\n\nThis container may not have vkd3d.",
                   static_cast<unsigned long>(result));
        MessageBoxW(window.handle, message, L"wxr test - D3D12", MB_OK | MB_ICONERROR);
        return 1;
    }

    D3D12_COMMAND_QUEUE_DESC queueDescription = {};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue));

    IDXGIFactory4* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 swapDescription = {};
    swapDescription.Width = width;
    swapDescription.Height = height;
    swapDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = 2;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swapChain1 = nullptr;
    result = factory->CreateSwapChainForHwnd(queue, window.handle, &swapDescription, nullptr, nullptr, &swapChain1);
    if (FAILED(result)) {
        MessageBoxW(window.handle, L"CreateSwapChainForHwnd failed.", L"wxr test - D3D12", MB_OK | MB_ICONERROR);
        return 1;
    }
    IDXGISwapChain3* swapChain = nullptr;
    swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain));
    TestLog("swapchain created");

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription = {};
    rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDescription.NumDescriptors = 2;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    device->CreateDescriptorHeap(&rtvHeapDescription, IID_PPV_ARGS(&rtvHeap));
    const UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = rtvHeap->GetCPUDescriptorHandleForHeapStart();

    ID3D12Resource* renderTargets[2] = {};
    for (UINT i = 0; i < 2; ++i) {
        swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvStart;
        handle.ptr += static_cast<SIZE_T>(i) * rtvSize;
        device->CreateRenderTargetView(renderTargets[i], nullptr, handle);
    }

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDescription = {};
    depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDescription.Width = width;
    depthDescription.Height = height;
    depthDescription.DepthOrArraySize = 1;
    depthDescription.MipLevels = 1;
    depthDescription.Format = DXGI_FORMAT_D32_FLOAT;
    depthDescription.SampleDesc.Count = 1;
    depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;

    ID3D12Resource* depthTexture = nullptr;
    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDescription,
                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&depthTexture));

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDescription = {};
    dsvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDescription.NumDescriptors = 1;
    ID3D12DescriptorHeap* dsvHeap = nullptr;
    device->CreateDescriptorHeap(&dsvHeapDescription, IID_PPV_ARGS(&dsvHeap));
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(depthTexture, nullptr, dsvHandle);

    D3D12_ROOT_PARAMETER rootParameter = {};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.Descriptor.ShaderRegister = 0;
    rootParameter.Descriptor.RegisterSpace = 0;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription = {};
    rootSignatureDescription.NumParameters = 1;
    rootSignatureDescription.pParameters = &rootParameter;
    rootSignatureDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* signatureBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    result = D3D12SerializeRootSignature(&rootSignatureDescription, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob,
                                         &errorBlob);
    if (FAILED(result) || signatureBlob == nullptr) {
        TestLog("D3D12SerializeRootSignature failed 0x%08lX", static_cast<unsigned long>(result));
        MessageBoxW(window.handle, L"D3D12SerializeRootSignature failed.", L"wxr test - D3D12",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    TestLog("root signature serialized");

    ID3D12RootSignature* rootSignature = nullptr;
    result = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
                                         IID_PPV_ARGS(&rootSignature));
    TestLog("CreateRootSignature 0x%08lX", static_cast<unsigned long>(result));

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription = {};
    pipelineDescription.pRootSignature = rootSignature;
    pipelineDescription.VS = {kCubeVs5, kCubeVs5Size};
    pipelineDescription.PS = {kCubePs5, kCubePs5Size};
    pipelineDescription.InputLayout = {inputLayout, 2};
    pipelineDescription.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipelineDescription.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipelineDescription.RasterizerState.DepthClipEnable = TRUE;
    pipelineDescription.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pipelineDescription.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipelineDescription.DepthStencilState.DepthEnable = TRUE;
    pipelineDescription.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pipelineDescription.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pipelineDescription.DepthStencilState.StencilEnable = FALSE;
    pipelineDescription.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pipelineDescription.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.SampleDesc.Count = 1;
    pipelineDescription.SampleMask = UINT_MAX;
    pipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDescription.NumRenderTargets = 1;

    ID3D12PipelineState* pipelineState = nullptr;
    result = device->CreateGraphicsPipelineState(&pipelineDescription, IID_PPV_ARGS(&pipelineState));
    TestLog("CreateGraphicsPipelineState 0x%08lX", static_cast<unsigned long>(result));
    if (FAILED(result)) {
        MessageBoxW(window.handle, L"CreateGraphicsPipelineState failed.", L"wxr test - D3D12",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    CubeVertexF vertices[24] = {};
    std::uint16_t indices[36] = {};
    BuildCube(vertices, indices);

    std::vector<CubeVertexF> textVertices;
    std::vector<std::uint16_t> textIndices;
    const float textColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    BuildTextMesh("D3D12", 2.0f * 8.0f / width, 2.0f * 8.0f / height, -0.95f, 0.90f, textColor, textVertices, textIndices);

    ID3D12Resource* vertexBuffer = CreateUploadBuffer(device, vertices, sizeof(vertices));
    ID3D12Resource* indexBuffer = CreateUploadBuffer(device, indices, sizeof(indices));
    const float zeroMatrix[16] = {};
    ID3D12Resource* constantBuffer = CreateUploadBuffer(device, zeroMatrix, 256);
    ID3D12Resource* textVertexBuffer = CreateUploadBuffer(
        device, textVertices.data(), static_cast<UINT>(textVertices.size() * sizeof(CubeVertexF)));
    ID3D12Resource* textIndexBuffer = CreateUploadBuffer(
        device, textIndices.data(), static_cast<UINT>(textIndices.size() * sizeof(std::uint16_t)));
    const Mat4 identity = MatIdentity();
    ID3D12Resource* textConstantBuffer = CreateUploadBuffer(device, identity.m, 256);
    TestLog("buffers vertex=%p index=%p constants=%p", static_cast<void*>(vertexBuffer),
            static_cast<void*>(indexBuffer), static_cast<void*>(constantBuffer));
    if (vertexBuffer == nullptr || indexBuffer == nullptr || constantBuffer == nullptr ||
        textVertexBuffer == nullptr || textIndexBuffer == nullptr || textConstantBuffer == nullptr) {
        MessageBoxW(window.handle, L"Upload buffer creation failed.", L"wxr test - D3D12", MB_OK | MB_ICONERROR);
        return 1;
    }
    void* constantMapped = nullptr;
    constantBuffer->Map(0, nullptr, &constantMapped);

    ID3D12CommandAllocator* allocator = nullptr;
    result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    TestLog("allocator 0x%08lX", static_cast<unsigned long>(result));

    ID3D12GraphicsCommandList* commandList = nullptr;
    result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                       IID_PPV_ARGS(&commandList));
    TestLog("command list 0x%08lX", static_cast<unsigned long>(result));
    commandList->Close();

    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceValue = 0;

    const float eye[3] = {0.0f, 0.9f, -4.2f};
    const float target[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Mat4 view = MatLookAt(eye, target, up);
    const Mat4 projection =
        MatPerspectiveD3D(60.0f * 3.14159265f / 180.0f, static_cast<float>(width) / height, 0.1f, 100.0f);

    const D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const D3D12_RECT scissor = {0, 0, width, height};
    const float clearColor[4] = {0.094f, 0.102f, 0.125f, 1.0f};

    while (PumpTestWindow(window)) {
        const double time = TestTimeSeconds();
        const Mat4 world = MatRotationYawPitch(static_cast<float>(time) * 0.9f, 0.55f);
        const Mat4 worldViewProjection = MatMultiply(MatMultiply(world, view), projection);
        static int loggedFrames = 0;
        if ((loggedFrames++ % 300) == 0) {
            TestLog("frame %d", loggedFrames);
        }

        const UINT frameIndex = swapChain->GetCurrentBackBufferIndex();

        allocator->Reset();
        commandList->Reset(allocator, pipelineState);

        Transition(commandList, renderTargets[frameIndex], D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_CPU_DESCRIPTOR_HANDLE frameRtv = rtvStart;
        frameRtv.ptr += static_cast<SIZE_T>(frameIndex) * rtvSize;
        commandList->OMSetRenderTargets(1, &frameRtv, FALSE, &dsvHandle);
        commandList->ClearRenderTargetView(frameRtv, clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        commandList->SetGraphicsRootSignature(rootSignature);
        commandList->SetPipelineState(pipelineState);
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const D3D12_VERTEX_BUFFER_VIEW vertexView = {vertexBuffer->GetGPUVirtualAddress(), sizeof(vertices),
                                                     sizeof(CubeVertexF)};
        const D3D12_INDEX_BUFFER_VIEW indexView = {indexBuffer->GetGPUVirtualAddress(), sizeof(indices),
                                                   DXGI_FORMAT_R16_UINT};
        commandList->IASetVertexBuffers(0, 1, &vertexView);
        commandList->IASetIndexBuffer(&indexView);
        std::memcpy(constantMapped, worldViewProjection.m, sizeof(worldViewProjection.m));
        commandList->SetGraphicsRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());
        commandList->DrawIndexedInstanced(36, 1, 0, 0, 0);

        commandList->SetGraphicsRootConstantBufferView(0, textConstantBuffer->GetGPUVirtualAddress());
        const D3D12_VERTEX_BUFFER_VIEW textVertexView = {
            textVertexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(textVertices.size() * sizeof(CubeVertexF)),
            sizeof(CubeVertexF)};
        const D3D12_INDEX_BUFFER_VIEW textIndexView = {
            textIndexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(textIndices.size() * sizeof(std::uint16_t)),
            DXGI_FORMAT_R16_UINT};
        commandList->IASetVertexBuffers(0, 1, &textVertexView);
        commandList->IASetIndexBuffer(&textIndexView);
        commandList->DrawIndexedInstanced(static_cast<UINT>(textIndices.size()), 1, 0, 0, 0);

        Transition(commandList, renderTargets[frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PRESENT);

        commandList->Close();
        ID3D12CommandList* lists[] = {commandList};
        queue->ExecuteCommandLists(1, lists);
        swapChain->Present(1, 0);

        const UINT64 signalValue = ++fenceValue;
        queue->Signal(fence, signalValue);
        if (fence->GetCompletedValue() < signalValue) {
            fence->SetEventOnCompletion(signalValue, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }

        LimitFrameRate();

        UpdateFpsTitle(window);
    }

    StopTestAudio();
    WaitForSingleObject(fenceEvent, 500);
    CloseHandle(fenceEvent);
    fence->Release();
    commandList->Release();
    allocator->Release();
    indexBuffer->Release();
    vertexBuffer->Release();
    constantBuffer->Release();
    textConstantBuffer->Release();
    textIndexBuffer->Release();
    textVertexBuffer->Release();
    pipelineState->Release();
    rootSignature->Release();
    signatureBlob->Release();
    dsvHeap->Release();
    depthTexture->Release();
    for (auto& renderTarget : renderTargets) {
        renderTarget->Release();
    }
    rtvHeap->Release();
    swapChain->Release();
    swapChain1->Release();
    factory->Release();
    queue->Release();
    device->Release();
    return 0;
}
