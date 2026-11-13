// Injected D3D11 capture hook.
//
// Hooks the current IDXGISwapChain::Present function with MinHook, copies the backbuffer into the
// shared-memory bridge, then calls the original Present. Chain-last: if a mod already hooked the
// function, our detour calls the previous pointer, so the mod still runs.
//
// Overhead is kept low by:
//   - capturing at most 30 frames per second,
//   - reading back through two staging textures so the GPU has a frame of slack,
//   - doing nothing at all while the host has capture disabled.

#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <d3d10.h>
#include <d3d9.h>
#include <ddraw.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <GL/gl.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"
#include "hook_d3d8.h"
#include "shared_frame.h"

namespace {

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using EndSceneFn = HRESULT(WINAPI*)(IDirect3DDevice9*);

constexpr ULONGLONG kMinCaptureIntervalMs = 1000 / 30;

HINSTANCE g_module = nullptr;
SharedFrameWriter g_writer;
PresentFn g_originalPresent = nullptr;
EndSceneFn g_originalEndScene = nullptr;
std::ofstream g_log;
std::mutex g_logMutex;

using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
using Create9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
using CreateDevice9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
Create9Fn g_originalCreate9 = nullptr;
Create9ExFn g_originalCreate9Ex = nullptr;
CreateDevice9Fn g_originalCreateDevice9 = nullptr;
std::mutex g_d3d9HookMutex;

IDXGISwapChain* g_swapChain = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11Texture2D* g_staging[2] = {};
bool g_stagingValid[2] = {};
int g_stagingIndex = 0;
UINT g_width = 0;
UINT g_height = 0;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
int g_frameCount = 0;
ULONGLONG g_nextCapture = 0;
int g_openRetryCountdown = 0;
bool g_captureLoggedOff = false;
std::mutex g_captureMutex;

IDirect3DDevice9* g_d3d9Device = nullptr;
IDirect3DSurface9* g_d3d9Surface[2] = {};
bool g_d3d9SurfaceValid[2] = {};
int g_d3d9SurfaceIndex = 0;
UINT g_d3d9Width = 0;
UINT g_d3d9Height = 0;
D3DFORMAT g_d3d9Format = D3DFMT_UNKNOWN;
int g_d3d9FrameCount = 0;

// DirectDraw: blits, fast blits, and flips all end with a drawable surface. Surfaces are read after
// Blt/BltFast and before Flip, so the captured image is whatever the app just composed. Every
// interface version has its own vtable entries, so each distinct function address gets its own
// slot and trampoline (CNC-Ddraw and legacy DDraw apps do not all share IDirectDrawSurface7's
// implementation).
constexpr int kMaxDdrawHookSlots = 6;
constexpr int kDdrawBltVtableIndex = 5;
constexpr int kDdrawBltFastVtableIndex = 7;
constexpr int kDdrawFlipVtableIndex = 11;
constexpr int kDdrawUnlockVtableIndex = 32;
constexpr int kDdrawMinWidth = 160;
constexpr int kDdrawMinHeight = 120;

using DdrawBltFn = HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface7*, LPRECT, IDirectDrawSurface7*, LPRECT, DWORD,
                                               LPDDBLTFX);
using DdrawBltFastFn = HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface7*, DWORD, DWORD, IDirectDrawSurface7*, LPRECT,
                                                   DWORD);
using DdrawFlipFn = HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface7*, IDirectDrawSurface7*, DWORD);
using DdrawUnlockFn = HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface7*, LPRECT);

DdrawBltFn g_originalDdrawBlt[kMaxDdrawHookSlots] = {};
DdrawBltFastFn g_originalDdrawBltFast[kMaxDdrawHookSlots] = {};
DdrawFlipFn g_originalDdrawFlip[kMaxDdrawHookSlots] = {};
std::vector<void*> g_ddrawHookedAddresses;
thread_local bool g_inDdrawCapture = false;
int g_ddrawFrameCount = 0;
// Direct3D 10: the swapchain is the same DXGI object, but the backbuffer must be read through the
// D3D10 device (the D3D11 staging path returns black for a D3D10 swapchain).
IDXGISwapChain* g_d3d10SwapChain = nullptr;
ID3D10Device* g_d3d10Device = nullptr;
ID3D10Texture2D* g_d3d10Staging[2] = {};
bool g_d3d10StagingValid[2] = {};
int g_d3d10StagingIndex = 0;
UINT g_d3d10Width = 0;
UINT g_d3d10Height = 0;
DXGI_FORMAT g_d3d10Format = DXGI_FORMAT_UNKNOWN;
int g_d3d10FrameCount = 0;

// OpenGL: read the backbuffer with glReadPixels from the current context.
using WglSwapBuffersFn = BOOL(WINAPI*)(HDC);
using SwapBuffersFn = BOOL(WINAPI*)(HDC);
using GlReadPixelsFn = void(APIENTRY*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
using WglGetCurrentDcFn = HDC(WINAPI*)();
using WglGetCurrentContextFn = HGLRC(WINAPI*)();
WglSwapBuffersFn g_originalWglSwapBuffers = nullptr;
SwapBuffersFn g_originalSwapBuffers = nullptr;
GlReadPixelsFn g_glReadPixels = nullptr;
WglGetCurrentDcFn g_wglGetCurrentDC = nullptr;
WglGetCurrentContextFn g_wglGetCurrentContext = nullptr;
std::vector<std::uint8_t> g_glPixels;
std::vector<std::uint8_t> g_glFlipped;
int g_glFrameCount = 0;

// Direct3D 12: captured at launch time, so the app's device and command queue are known before the
// first frame. The copy is submitted on the app's own queue, which keeps it ordered after rendering.
using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using D3D12CreateCommandQueueFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*,
                                                              REFIID, void**);
D3D12CreateDeviceFn g_originalD3D12CreateDevice = nullptr;
D3D12CreateCommandQueueFn g_originalD3D12CreateCommandQueue = nullptr;
ID3D12Device* g_d3d12Device = nullptr;
ID3D12CommandQueue* g_d3d12Queue = nullptr;
ID3D12CommandAllocator* g_d3d12Allocator = nullptr;
ID3D12GraphicsCommandList* g_d3d12List = nullptr;
ID3D12Fence* g_d3d12Fence = nullptr;
HANDLE g_d3d12FenceEvent = nullptr;
UINT64 g_d3d12FenceValue = 0;
ID3D12Resource* g_d3d12Readback = nullptr;
UINT g_d3d12Width = 0;
UINT g_d3d12Height = 0;
DXGI_FORMAT g_d3d12Format = DXGI_FORMAT_UNKNOWN;
UINT g_d3d12RowPitch = 0;
int g_d3d12FrameCount = 0;
std::mutex g_d3d12Mutex;

// Vulkan: captured at launch time, so the app's device and queue are known before the first present.
using VkCreateDeviceHookFn = VkResult(VKAPI_PTR*)(VkPhysicalDevice, const VkDeviceCreateInfo*,
                                                 const VkAllocationCallbacks*, VkDevice*);
using VkCreateSwapchainHookFn = VkResult(VKAPI_PTR*)(VkDevice, const VkSwapchainCreateInfoKHR*,
                                                    const VkAllocationCallbacks*, VkSwapchainKHR*);
using VkQueuePresentHookFn = VkResult(VKAPI_PTR*)(VkQueue, const VkPresentInfoKHR*);
VkCreateDeviceHookFn g_originalVkCreateDevice = nullptr;
VkCreateSwapchainHookFn g_originalVkCreateSwapchain = nullptr;
VkQueuePresentHookFn g_originalVkQueuePresent = nullptr;
VkDevice g_vkDevice = VK_NULL_HANDLE;
VkQueue g_vkQueue = VK_NULL_HANDLE;
VkPhysicalDevice g_vkPhysicalDevice = VK_NULL_HANDLE;
VkSwapchainKHR g_vkSwapchain = VK_NULL_HANDLE;
VkFormat g_vkFormat = VK_FORMAT_UNDEFINED;
VkExtent2D g_vkExtent = {};
VkCommandPool g_vkCommandPool = VK_NULL_HANDLE;
VkCommandBuffer g_vkCommandBuffer = VK_NULL_HANDLE;
VkFence g_vkFence = VK_NULL_HANDLE;
VkBuffer g_vkReadback = VK_NULL_HANDLE;
VkDeviceMemory g_vkReadbackMemory = VK_NULL_HANDLE;
VkDeviceSize g_vkReadbackSize = 0;
int g_vkFrameCount = 0;
std::mutex g_vkMutex;

PFN_vkGetDeviceProcAddr g_vkGetDeviceProcAddr = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties g_vkGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkGetDeviceQueue g_vkGetDeviceQueue = nullptr;
PFN_vkGetSwapchainImagesKHR g_vkGetSwapchainImagesKHR = nullptr;
PFN_vkCreateCommandPool g_vkCreateCommandPool = nullptr;
PFN_vkAllocateCommandBuffers g_vkAllocateCommandBuffers = nullptr;
PFN_vkCreateFence g_vkCreateFence = nullptr;
PFN_vkCreateBuffer g_vkCreateBuffer = nullptr;
PFN_vkGetBufferMemoryRequirements g_vkGetBufferMemoryRequirements = nullptr;
PFN_vkAllocateMemory g_vkAllocateMemory = nullptr;
PFN_vkBindBufferMemory g_vkBindBufferMemory = nullptr;
PFN_vkResetCommandBuffer g_vkResetCommandBuffer = nullptr;
PFN_vkBeginCommandBuffer g_vkBeginCommandBuffer = nullptr;
PFN_vkCmdPipelineBarrier g_vkCmdPipelineBarrier = nullptr;
PFN_vkCmdCopyImageToBuffer g_vkCmdCopyImageToBuffer = nullptr;
PFN_vkEndCommandBuffer g_vkEndCommandBuffer = nullptr;
PFN_vkQueueSubmit g_vkQueueSubmit = nullptr;
PFN_vkWaitForFences g_vkWaitForFences = nullptr;
PFN_vkResetFences g_vkResetFences = nullptr;
PFN_vkMapMemory g_vkMapMemory = nullptr;
PFN_vkUnmapMemory g_vkUnmapMemory = nullptr;

void CaptureFrameD3D10(IDXGISwapChain* swapChain);
void CaptureFrameD3D12(IDXGISwapChain* swapChain);
void* FindPresentAddressD3D12();
bool InstallD3D12Hooks();
bool InstallOpenGLHooks();
bool InstallVulkanHooks();

std::wstring ModuleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0) {
        return L".";
    }
    std::wstring result(path, length);
    const std::size_t slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        result.resize(slash);
    }
    return result;
}

void Log(const std::string& text)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) {
        g_log << text << "\n";
        g_log.flush();
    }
}

// The host creates the shared mapping when recording starts. When the hook is injected at launch
// the mapping does not exist yet, so retry opening it on a timer instead of giving up.
bool EnsureWriter()
{
    if (g_writer.Enabled()) {
        return true;
    }
    if (--g_openRetryCountdown <= 0) {
        g_openRetryCountdown = 30;
        g_writer.Open();
    }
    return g_writer.Enabled();
}

// True when this Present/EndScene call should capture. A deadline accumulator keeps the capture
// rate near 30 fps without the aliasing a plain "now - last >= interval" check causes when the
// source frame period is close to the interval (which dropped capture to 16 fps at 32 fps).
bool ShouldCapture()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextCapture) {
        return false;
    }
    g_nextCapture += kMinCaptureIntervalMs;
    if (g_nextCapture <= now) {
        g_nextCapture = now + kMinCaptureIntervalMs;
    }
    return true;
}

void* FindPresentAddress()
{
    // Use the D3D11 module the target already loaded. Never load it ourselves: pulling DXVK's
    // d3d11.dll into a D3D9-only game would initialize a second graphics stack for nothing.
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (d3d11 == nullptr) {
        return FindPresentAddressD3D12();
    }
    auto createDevice = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
        GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (createDevice == nullptr) {
        Log("D3D11CreateDeviceAndSwapChain not exported by the loaded d3d11.dll");
        return nullptr;
    }

    const wchar_t className[] = L"WxrHookDummy";

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);

    HWND window = CreateWindowExW(0, className, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr) {
        Log("dummy window creation failed");
        return nullptr;
    }

    DXGI_SWAP_CHAIN_DESC description = {};
    description.BufferCount = 1;
    description.BufferDesc.Width = 2;
    description.BufferDesc.Height = 2;
    description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;

    HRESULT result = createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &description, &swapChain, &device, &level, &context);
    if (FAILED(result)) {
        Log("D3D11CreateDeviceAndSwapChain(HARDWARE) failed: 0x" + std::to_string(static_cast<unsigned long>(result)));
        result = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                              &description, &swapChain, &device, &level, &context);
    }

    void* present = nullptr;
    if (SUCCEEDED(result) && swapChain != nullptr) {
        // IDXGISwapChain::Present is vtable slot 8 (IUnknown 0-2, IDXGIObject 3-6,
        // IDXGIDeviceSubObject 7, Present 8).
        void** vtable = *reinterpret_cast<void***>(swapChain);
        present = vtable[8];
        Log("found Present at 0x" + std::to_string(reinterpret_cast<unsigned long long>(present)));
    } else {
        Log("D3D11CreateDeviceAndSwapChain failed: 0x" + std::to_string(static_cast<unsigned long>(result)));
    }

    if (context != nullptr) {
        context->Release();
    }
    if (device != nullptr) {
        device->Release();
    }
    if (swapChain != nullptr) {
        swapChain->Release();
    }
    DestroyWindow(window);
    UnregisterClassW(className, GetModuleHandleW(nullptr));
    return present;
}

// IDirect3DDevice9::EndScene is vtable slot 42 (IUnknown 0-2, then 39 device methods, BeginScene
// 41, EndScene 42).
constexpr int kEndSceneVtableIndex = 42;

void* FindEndSceneAddress()
{
    // Same rule as D3D11: only probe D3D9 when the target already loaded d3d9.dll.
    HMODULE d3d9Module = GetModuleHandleW(L"d3d9.dll");
    if (d3d9Module == nullptr) {
        return nullptr;
    }
    auto create9 = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(d3d9Module, "Direct3DCreate9"));
    if (create9 == nullptr) {
        Log("Direct3DCreate9 not exported by the loaded d3d9.dll");
        return nullptr;
    }

    const wchar_t className[] = L"WxrHookDummy9";

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);

    HWND window = CreateWindowExW(0, className, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr) {
        Log("d3d9 dummy window creation failed");
        return nullptr;
    }

    IDirect3D9* d3d9 = create9(D3D_SDK_VERSION);
    if (d3d9 == nullptr) {
        Log("Direct3DCreate9 failed");
        DestroyWindow(window);
        UnregisterClassW(className, GetModuleHandleW(nullptr));
        return nullptr;
    }

    D3DPRESENT_PARAMETERS parameters = {};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
    parameters.BackBufferWidth = 2;
    parameters.BackBufferHeight = 2;
    parameters.hDeviceWindow = window;

    IDirect3DDevice9* device = nullptr;
    HRESULT result = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    if (FAILED(result)) {
        Log("CreateDevice(HAL) failed: 0x" + std::to_string(static_cast<unsigned long>(result)));
        result = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, window,
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    }

    void* endScene = nullptr;
    if (SUCCEEDED(result) && device != nullptr) {
        void** vtable = *reinterpret_cast<void***>(device);
        endScene = vtable[kEndSceneVtableIndex];
        Log("found EndScene at 0x" + std::to_string(reinterpret_cast<unsigned long long>(endScene)));
    } else {
        Log("d3d9 device creation failed: 0x" + std::to_string(static_cast<unsigned long>(result)));
    }

    if (device != nullptr) {
        device->Release();
    }
    d3d9->Release();
    DestroyWindow(window);
    UnregisterClassW(className, GetModuleHandleW(nullptr));
    return endScene;
}

void ReleaseD3D9()
{
    for (auto& surface : g_d3d9Surface) {
        if (surface != nullptr) {
            surface->Release();
            surface = nullptr;
        }
    }
    g_d3d9SurfaceValid[0] = false;
    g_d3d9SurfaceValid[1] = false;
    g_d3d9SurfaceIndex = 0;
    if (g_d3d9Device != nullptr) {
        g_d3d9Device->Release();
        g_d3d9Device = nullptr;
    }
    g_d3d9Width = 0;
    g_d3d9Height = 0;
    g_d3d9Format = D3DFMT_UNKNOWN;
}

void CaptureD3D9(IDirect3DDevice9* device)
{
    if (!EnsureWriter()) {
        return;
    }

    if (!ShouldCapture()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_captureMutex);

    if (g_d3d9Device != device) {
        ReleaseD3D9();
        g_d3d9Device = device;
        g_d3d9Device->AddRef();
    }

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
        return;
    }

    D3DSURFACE_DESC description = {};
    if (FAILED(backBuffer->GetDesc(&description)) ||
        (description.Format != D3DFMT_X8R8G8B8 && description.Format != D3DFMT_A8R8G8B8)) {
        backBuffer->Release();
        return;
    }

    if (g_d3d9Surface[0] == nullptr || g_d3d9Width != description.Width || g_d3d9Height != description.Height ||
        g_d3d9Format != description.Format) {
        for (auto& surface : g_d3d9Surface) {
            if (surface != nullptr) {
                surface->Release();
                surface = nullptr;
            }
        }
        g_d3d9SurfaceValid[0] = false;
        g_d3d9SurfaceValid[1] = false;
        g_d3d9SurfaceIndex = 0;

        for (auto& surface : g_d3d9Surface) {
            if (FAILED(device->CreateOffscreenPlainSurface(description.Width, description.Height, description.Format,
                                                           D3DPOOL_SYSTEMMEM, &surface, nullptr))) {
                Log("CreateOffscreenPlainSurface failed");
                backBuffer->Release();
                return;
            }
        }
        g_d3d9Width = description.Width;
        g_d3d9Height = description.Height;
        g_d3d9Format = description.Format;
        Log("d3d9 backbuffer: " + std::to_string(g_d3d9Width) + "x" + std::to_string(g_d3d9Height) + " format " +
            std::to_string(static_cast<int>(g_d3d9Format)));
    }

    // Copy this frame into one surface and read the previous frame from the other, so LockRect is
    // not waiting on the copy that was just queued.
    if (FAILED(device->GetRenderTargetData(backBuffer, g_d3d9Surface[g_d3d9SurfaceIndex]))) {
        backBuffer->Release();
        return;
    }
    backBuffer->Release();
    g_d3d9SurfaceValid[g_d3d9SurfaceIndex] = true;

    const int readableIndex = 1 - g_d3d9SurfaceIndex;
    g_d3d9SurfaceIndex = readableIndex;

    if (!g_d3d9SurfaceValid[readableIndex]) {
        return;
    }

    D3DLOCKED_RECT locked = {};
    if (FAILED(g_d3d9Surface[readableIndex]->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        return;
    }

    g_writer.Write(static_cast<const std::uint8_t*>(locked.pBits), static_cast<int>(g_d3d9Width),
                   static_cast<int>(g_d3d9Height), static_cast<int>(locked.Pitch),
                   static_cast<std::int64_t>(GetTickCount64()));
    g_d3d9Surface[readableIndex]->UnlockRect();

    ++g_d3d9FrameCount;
    if (g_d3d9FrameCount == 1 || (g_d3d9FrameCount % 120) == 0) {
        Log("d3d9 frame " + std::to_string(g_d3d9FrameCount) + ": " + std::to_string(g_d3d9Width) + "x" +
            std::to_string(g_d3d9Height));
    }
}

// IDirect3D9::CreateDevice is vtable slot 16 (IUnknown 0-2, then 13 methods before CreateDevice:
// RegisterSoftwareDevice through CheckDeviceFormatConversion, GetDeviceCaps, GetAdapterMonitor).
constexpr int kCreateDeviceVtableIndex = 16;

HRESULT WINAPI EndSceneDetour(IDirect3DDevice9* device)
{
    CaptureD3D9(device);
    return g_originalEndScene != nullptr ? g_originalEndScene(device) : D3DERR_INVALIDCALL;
}

void HookD3D9Device(IDirect3DDevice9* device)
{
    if (device == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_d3d9HookMutex);
    if (g_originalEndScene != nullptr) {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    void* endScene = vtable[kEndSceneVtableIndex];
    if (endScene == nullptr) {
        return;
    }

    if (MH_CreateHook(endScene, reinterpret_cast<LPVOID>(&EndSceneDetour),
                      reinterpret_cast<LPVOID*>(&g_originalEndScene)) != MH_OK) {
        Log("d3d9 direct EndScene hook creation failed");
        return;
    }
    if (MH_EnableHook(endScene) == MH_OK) {
        Log("d3d9 direct EndScene hooked");
    }
}

HRESULT STDMETHODCALLTYPE CreateDevice9Detour(IDirect3D9* self, UINT adapter, D3DDEVTYPE deviceType,
                                              HWND focusWindow, DWORD behaviorFlags,
                                              D3DPRESENT_PARAMETERS* parameters,
                                              IDirect3DDevice9** device)
{
    const HRESULT result =
        g_originalCreateDevice9 != nullptr
            ? g_originalCreateDevice9(self, adapter, deviceType, focusWindow, behaviorFlags, parameters, device)
            : D3DERR_INVALIDCALL;
    if (SUCCEEDED(result) && device != nullptr && *device != nullptr) {
        HookD3D9Device(*device);
    }
    return result;
}

void HookD3D9CreateDevice(IDirect3D9* d3d9)
{
    if (d3d9 == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_d3d9HookMutex);
    if (g_originalCreateDevice9 != nullptr) {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(d3d9);
    void* createDevice = vtable[kCreateDeviceVtableIndex];
    if (createDevice == nullptr) {
        return;
    }

    if (MH_CreateHook(createDevice, reinterpret_cast<LPVOID>(&CreateDevice9Detour),
                      reinterpret_cast<LPVOID*>(&g_originalCreateDevice9)) != MH_OK) {
        Log("d3d9 CreateDevice hook creation failed");
        return;
    }
    if (MH_EnableHook(createDevice) == MH_OK) {
        Log("d3d9 CreateDevice hooked");
    }
}

IDirect3D9* WINAPI Create9Detour(UINT sdkVersion)
{
    IDirect3D9* d3d9 = g_originalCreate9 != nullptr ? g_originalCreate9(sdkVersion) : nullptr;
    if (d3d9 != nullptr) {
        HookD3D9CreateDevice(d3d9);
    }
    return d3d9;
}

HRESULT WINAPI Create9ExDetour(UINT sdkVersion, IDirect3D9Ex** d3d9)
{
    const HRESULT result =
        g_originalCreate9Ex != nullptr ? g_originalCreate9Ex(sdkVersion, d3d9) : E_FAIL;
    if (SUCCEEDED(result) && d3d9 != nullptr && *d3d9 != nullptr) {
        HookD3D9CreateDevice(*d3d9);
    }
    return result;
}

bool InstallD3D9CreateHooks()
{
    std::lock_guard<std::mutex> lock(g_d3d9HookMutex);
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (d3d9 == nullptr) {
        return false;
    }

    bool any = false;
    if (g_originalCreate9 == nullptr) {
        auto create9 = GetProcAddress(d3d9, "Direct3DCreate9");
        if (create9 != nullptr &&
            MH_CreateHook(create9, reinterpret_cast<LPVOID>(&Create9Detour),
                          reinterpret_cast<LPVOID*>(&g_originalCreate9)) == MH_OK) {
            MH_EnableHook(create9);
            Log("d3d9 Direct3DCreate9 hooked");
            any = true;
        }
    } else {
        any = true;
    }

    if (g_originalCreate9Ex == nullptr) {
        auto create9Ex = GetProcAddress(d3d9, "Direct3DCreate9Ex");
        if (create9Ex != nullptr &&
            MH_CreateHook(create9Ex, reinterpret_cast<LPVOID>(&Create9ExDetour),
                          reinterpret_cast<LPVOID*>(&g_originalCreate9Ex)) == MH_OK) {
            MH_EnableHook(create9Ex);
            Log("d3d9 Direct3DCreate9Ex hooked");
            any = true;
        }
    } else {
        any = true;
    }

    return any;
}

void ReleaseStaging()
{
    for (auto& staging : g_staging) {
        if (staging != nullptr) {
            staging->Release();
            staging = nullptr;
        }
    }
    g_stagingValid[0] = false;
    g_stagingValid[1] = false;
    g_stagingIndex = 0;
    g_width = 0;
    g_height = 0;
    g_format = DXGI_FORMAT_UNKNOWN;
}

void ReleaseDevice()
{
    ReleaseStaging();
    if (g_context != nullptr) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device != nullptr) {
        g_device->Release();
        g_device = nullptr;
    }
    g_swapChain = nullptr;
}

// Ensures the device/context and both staging textures match the backbuffer description.
bool PrepareResources(const D3D11_TEXTURE2D_DESC& description)
{
    if (g_staging[0] != nullptr && g_width == description.Width && g_height == description.Height &&
        g_format == description.Format) {
        return true;
    }

    ReleaseStaging();

    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;

    for (auto& staging : g_staging) {
        if (FAILED(g_device->CreateTexture2D(&stagingDescription, nullptr, &staging))) {
            Log("CreateTexture2D(staging) failed");
            ReleaseStaging();
            return false;
        }
    }

    g_width = description.Width;
    g_height = description.Height;
    g_format = description.Format;
    Log("backbuffer: " + std::to_string(g_width) + "x" + std::to_string(g_height) + " format " +
        std::to_string(static_cast<int>(g_format)));
    return true;
}

void CopyToBgra(ID3D11Texture2D* texture, std::vector<std::uint8_t>& converted)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_context->Map(texture, 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }

    const int width = static_cast<int>(g_width);
    const int height = static_cast<int>(g_height);

    if (g_format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        g_writer.Write(static_cast<const std::uint8_t*>(mapped.pData), width, height,
                       static_cast<int>(mapped.RowPitch), static_cast<std::int64_t>(GetTickCount64()));
    } else {
        converted.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        for (int y = 0; y < height; ++y) {
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData) +
                                 static_cast<std::size_t>(y) * mapped.RowPitch;
            std::uint8_t* destination =
                converted.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
            for (int x = 0; x < width; ++x) {
                destination[x * 4 + 0] = source[x * 4 + 2];
                destination[x * 4 + 1] = source[x * 4 + 1];
                destination[x * 4 + 2] = source[x * 4 + 0];
                destination[x * 4 + 3] = source[x * 4 + 3];
            }
        }
        g_writer.Write(converted.data(), width, height, width * 4,
                       static_cast<std::int64_t>(GetTickCount64()));
    }

    g_context->Unmap(texture, 0);
}

void CaptureFrame(IDXGISwapChain* swapChain)
{
    // Fast path: capture disabled or host gone. The game pays one atomic read here.
    if (!g_writer.Enabled()) {
        if (!g_captureLoggedOff) {
            Log("capture disabled by host");
            g_captureLoggedOff = true;
        }
        if (--g_openRetryCountdown <= 0) {
            g_openRetryCountdown = 30;
            g_writer.Open();  // host may have restarted and re-created the mapping
        }
        return;
    }
    g_captureLoggedOff = false;

    if (GetModuleHandleW(L"d3d10.dll") != nullptr) {
        CaptureFrameD3D10(swapChain);
        return;
    }
    if (g_d3d12Device != nullptr) {
        CaptureFrameD3D12(swapChain);
        return;
    }

    if (!ShouldCapture()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_captureMutex);

    if (g_swapChain != swapChain) {
        ReleaseDevice();
        g_swapChain = swapChain;
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device)))) {
            Log("GetDevice failed");
            return;
        }
        g_device->GetImmediateContext(&g_context);
    }

    if (g_device == nullptr || g_context == nullptr) {
        return;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
        return;
    }

    D3D11_TEXTURE2D_DESC description = {};
    backBuffer->GetDesc(&description);

    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        backBuffer->Release();
        return;
    }

    if (!PrepareResources(description)) {
        backBuffer->Release();
        return;
    }

    // Copy this frame into one staging texture and read the previous frame from the other. The
    // GPU therefore has a full frame to finish the copy before Map can block on it.
    g_context->CopyResource(g_staging[g_stagingIndex], backBuffer);
    backBuffer->Release();
    g_stagingValid[g_stagingIndex] = true;

    const int readableIndex = 1 - g_stagingIndex;
    g_stagingIndex = readableIndex;

    if (g_stagingValid[readableIndex]) {
        std::vector<std::uint8_t> converted;
        CopyToBgra(g_staging[readableIndex], converted);

        ++g_frameCount;
        if (g_frameCount == 1 || (g_frameCount % 120) == 0) {
            Log("frame " + std::to_string(g_frameCount) + ": " + std::to_string(g_width) + "x" +
                std::to_string(g_height));
        }
    }
}

void CaptureFrameD3D10(IDXGISwapChain* swapChain)
{
    if (!ShouldCapture()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_captureMutex);

    if (g_d3d10Device == nullptr || g_d3d10SwapChain != swapChain) {
        for (auto& staging : g_d3d10Staging) {
            if (staging != nullptr) {
                staging->Release();
                staging = nullptr;
            }
        }
        g_d3d10StagingValid[0] = false;
        g_d3d10StagingValid[1] = false;
        g_d3d10StagingIndex = 0;
        g_d3d10Width = 0;
        g_d3d10Height = 0;
        g_d3d10Format = DXGI_FORMAT_UNKNOWN;
        if (g_d3d10Device != nullptr) {
            g_d3d10Device->Release();
            g_d3d10Device = nullptr;
        }
        g_d3d10SwapChain = swapChain;
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D10Device), reinterpret_cast<void**>(&g_d3d10Device)))) {
            Log("d3d10 GetDevice failed");
            return;
        }
    }

    ID3D10Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
        return;
    }

    D3D10_TEXTURE2D_DESC description = {};
    backBuffer->GetDesc(&description);

    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        backBuffer->Release();
        return;
    }

    if (g_d3d10Staging[0] == nullptr || g_d3d10Width != description.Width ||
        g_d3d10Height != description.Height || g_d3d10Format != description.Format) {
        for (auto& staging : g_d3d10Staging) {
            if (staging != nullptr) {
                staging->Release();
                staging = nullptr;
            }
        }
        g_d3d10StagingValid[0] = false;
        g_d3d10StagingValid[1] = false;
        g_d3d10StagingIndex = 0;

        D3D10_TEXTURE2D_DESC stagingDescription = description;
        stagingDescription.Usage = D3D10_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        stagingDescription.MiscFlags = 0;

        for (auto& staging : g_d3d10Staging) {
            if (FAILED(g_d3d10Device->CreateTexture2D(&stagingDescription, nullptr, &staging))) {
                Log("d3d10 CreateTexture2D(staging) failed");
                return;
            }
        }

        g_d3d10Width = description.Width;
        g_d3d10Height = description.Height;
        g_d3d10Format = description.Format;
        Log("d3d10 backbuffer: " + std::to_string(g_d3d10Width) + "x" + std::to_string(g_d3d10Height));
    }

    g_d3d10Device->CopyResource(g_d3d10Staging[g_d3d10StagingIndex], backBuffer);
    backBuffer->Release();
    g_d3d10StagingValid[g_d3d10StagingIndex] = true;

    const int readableIndex = 1 - g_d3d10StagingIndex;
    g_d3d10StagingIndex = readableIndex;
    if (!g_d3d10StagingValid[readableIndex]) {
        return;
    }

    D3D10_MAPPED_TEXTURE2D mapped = {};
    if (FAILED(g_d3d10Staging[readableIndex]->Map(0, D3D10_MAP_READ, 0, &mapped))) {
        return;
    }

    const int width = static_cast<int>(g_d3d10Width);
    const int height = static_cast<int>(g_d3d10Height);
    if (g_d3d10Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        g_writer.Write(static_cast<const std::uint8_t*>(mapped.pData), width, height,
                       static_cast<int>(mapped.RowPitch), static_cast<std::int64_t>(GetTickCount64()));
    } else {
        std::vector<std::uint8_t> converted(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        for (int y = 0; y < height; ++y) {
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData) +
                                 static_cast<std::size_t>(y) * mapped.RowPitch;
            auto* destination = converted.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
            for (int x = 0; x < width; ++x) {
                destination[x * 4 + 0] = source[x * 4 + 2];
                destination[x * 4 + 1] = source[x * 4 + 1];
                destination[x * 4 + 2] = source[x * 4 + 0];
                destination[x * 4 + 3] = source[x * 4 + 3];
            }
        }
        g_writer.Write(converted.data(), width, height, width * 4, static_cast<std::int64_t>(GetTickCount64()));
    }

    g_d3d10Staging[readableIndex]->Unmap(0);

    ++g_d3d10FrameCount;
    if (g_d3d10FrameCount == 1 || (g_d3d10FrameCount % 120) == 0) {
        Log("d3d10 frame " + std::to_string(g_d3d10FrameCount) + ": " + std::to_string(width) + "x" +
            std::to_string(height));
    }
}

void CaptureOpenGL()
{
    if (!EnsureWriter() || !ShouldCapture()) {
        return;
    }
    if (g_wglGetCurrentContext == nullptr || g_wglGetCurrentContext() == nullptr) {
        return;
    }
    if (g_glReadPixels == nullptr || g_wglGetCurrentDC == nullptr) {
        return;
    }

    HDC deviceContext = g_wglGetCurrentDC();
    if (deviceContext == nullptr) {
        return;
    }
    HWND window = WindowFromDC(deviceContext);
    if (window == nullptr) {
        return;
    }
    RECT rect = {};
    if (!GetClientRect(window, &rect)) {
        return;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || width > kSharedMaxWidth || height > kSharedMaxHeight) {
        return;
    }

    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    const std::size_t bytes = rowBytes * static_cast<std::size_t>(height);
    if (g_glPixels.size() < bytes) {
        g_glPixels.resize(bytes);
    }
    if (g_glFlipped.size() < bytes) {
        g_glFlipped.resize(bytes);
    }

    g_glReadPixels(0, 0, width, height, 0x80E1 /* GL_BGRA */, 0x1401 /* GL_UNSIGNED_BYTE */, g_glPixels.data());

    for (int y = 0; y < height; ++y) {
        std::memcpy(g_glFlipped.data() + static_cast<std::size_t>(y) * rowBytes,
                    g_glPixels.data() + static_cast<std::size_t>(height - 1 - y) * rowBytes, rowBytes);
    }

    g_writer.Write(g_glFlipped.data(), width, height, static_cast<int>(rowBytes),
                   static_cast<std::int64_t>(GetTickCount64()));

    ++g_glFrameCount;
    if (g_glFrameCount == 1 || (g_glFrameCount % 120) == 0) {
        Log("opengl frame " + std::to_string(g_glFrameCount) + ": " + std::to_string(width) + "x" +
            std::to_string(height));
    }
}

BOOL WINAPI WglSwapBuffersDetour(HDC deviceContext)
{
    CaptureOpenGL();
    return g_originalWglSwapBuffers != nullptr ? g_originalWglSwapBuffers(deviceContext) : FALSE;
}

BOOL WINAPI SwapBuffersDetour(HDC deviceContext)
{
    CaptureOpenGL();
    return g_originalSwapBuffers != nullptr ? g_originalSwapBuffers(deviceContext) : FALSE;
}

void TransitionD3D12(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
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

void CreateD3D12Resources()
{
    if (g_d3d12Device == nullptr || g_d3d12Allocator != nullptr) {
        return;
    }
    g_d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_d3d12Allocator));
    g_d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_d3d12Allocator, nullptr,
                                     IID_PPV_ARGS(&g_d3d12List));
    g_d3d12List->Close();
    g_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3d12Fence));
    g_d3d12FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

void CaptureFrameD3D12(IDXGISwapChain* swapChain)
{
    if (!ShouldCapture()) {
        return;
    }
    if (g_d3d12Device == nullptr || g_d3d12Queue == nullptr || g_d3d12Allocator == nullptr ||
        g_d3d12List == nullptr || g_d3d12Fence == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_d3d12Mutex);

    ID3D12Resource* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&backBuffer)))) {
        return;
    }

    const D3D12_RESOURCE_DESC description = backBuffer->GetDesc();
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        backBuffer->Release();
        return;
    }

    const UINT width = static_cast<UINT>(description.Width);
    const UINT height = description.Height;
    const UINT rowPitch = (width * 4 + 255) & ~255u;

    if (g_d3d12Readback == nullptr || g_d3d12Width != width || g_d3d12Height != height ||
        g_d3d12Format != description.Format) {
        if (g_d3d12Readback != nullptr) {
            g_d3d12Readback->Release();
            g_d3d12Readback = nullptr;
        }

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC readbackDescription = {};
        readbackDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readbackDescription.Width = static_cast<UINT64>(rowPitch) * height;
        readbackDescription.Height = 1;
        readbackDescription.DepthOrArraySize = 1;
        readbackDescription.MipLevels = 1;
        readbackDescription.Format = DXGI_FORMAT_UNKNOWN;
        readbackDescription.SampleDesc.Count = 1;
        readbackDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(g_d3d12Device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &readbackDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&g_d3d12Readback)))) {
            Log("d3d12 readback creation failed");
            backBuffer->Release();
            return;
        }

        g_d3d12Width = width;
        g_d3d12Height = height;
        g_d3d12Format = description.Format;
        g_d3d12RowPitch = rowPitch;
        Log("d3d12 backbuffer: " + std::to_string(width) + "x" + std::to_string(height));
    }

    g_d3d12Allocator->Reset();
    g_d3d12List->Reset(g_d3d12Allocator, nullptr);
    TransitionD3D12(g_d3d12List, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = g_d3d12Readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Offset = 0;
    destination.PlacedFootprint.Footprint.Format = description.Format;
    destination.PlacedFootprint.Footprint.Width = width;
    destination.PlacedFootprint.Footprint.Height = height;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = rowPitch;

    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = backBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;

    g_d3d12List->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    TransitionD3D12(g_d3d12List, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    g_d3d12List->Close();

    ID3D12CommandList* lists[] = {g_d3d12List};
    g_d3d12Queue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_d3d12FenceValue;
    g_d3d12Queue->Signal(g_d3d12Fence, fenceValue);
    if (g_d3d12Fence->GetCompletedValue() < fenceValue) {
        g_d3d12Fence->SetEventOnCompletion(fenceValue, g_d3d12FenceEvent);
        WaitForSingleObject(g_d3d12FenceEvent, 2000);
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, static_cast<SIZE_T>(rowPitch) * height};
    if (SUCCEEDED(g_d3d12Readback->Map(0, &readRange, &mapped))) {
        if (description.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            g_writer.Write(static_cast<const std::uint8_t*>(mapped), static_cast<int>(width),
                           static_cast<int>(height), static_cast<int>(rowPitch),
                           static_cast<std::int64_t>(GetTickCount64()));
        } else {
            std::vector<std::uint8_t> converted(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
            for (UINT y = 0; y < height; ++y) {
                const auto* row = static_cast<const std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * rowPitch;
                auto* out = converted.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
                for (UINT x = 0; x < width; ++x) {
                    out[x * 4 + 0] = row[x * 4 + 2];
                    out[x * 4 + 1] = row[x * 4 + 1];
                    out[x * 4 + 2] = row[x * 4 + 0];
                    out[x * 4 + 3] = row[x * 4 + 3];
                }
            }
            g_writer.Write(converted.data(), static_cast<int>(width), static_cast<int>(height),
                           static_cast<int>(width) * 4, static_cast<std::int64_t>(GetTickCount64()));
        }
        g_d3d12Readback->Unmap(0, nullptr);
    }

    backBuffer->Release();

    ++g_d3d12FrameCount;
    if (g_d3d12FrameCount == 1 || (g_d3d12FrameCount % 120) == 0) {
        Log("d3d12 frame " + std::to_string(g_d3d12FrameCount) + ": " + std::to_string(width) + "x" +
            std::to_string(height));
    }
}

HRESULT WINAPI D3D12CreateCommandQueueDetour(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* description,
                                             REFIID riid, void** queue)
{
    HRESULT result = g_originalD3D12CreateCommandQueue(device, description, riid, queue);
    if (SUCCEEDED(result) && queue != nullptr && *queue != nullptr &&
        IsEqualIID(riid, __uuidof(ID3D12CommandQueue))) {
        if (g_d3d12Queue == nullptr && description != nullptr &&
            description->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_d3d12Queue = static_cast<ID3D12CommandQueue*>(*queue);
            g_d3d12Queue->AddRef();
            CreateD3D12Resources();
            Log("d3d12 queue captured");
        }
    }
    return result;
}

HRESULT WINAPI D3D12CreateDeviceDetour(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** device)
{
    HRESULT result = g_originalD3D12CreateDevice(adapter, level, riid, device);
    if (SUCCEEDED(result) && device != nullptr && *device != nullptr && IsEqualIID(riid, __uuidof(ID3D12Device))) {
        if (g_d3d12Device == nullptr) {
            g_d3d12Device = static_cast<ID3D12Device*>(*device);
            g_d3d12Device->AddRef();
            void** vtable = *reinterpret_cast<void***>(g_d3d12Device);
            void* createQueue = vtable[8];
            MH_CreateHook(createQueue, reinterpret_cast<LPVOID>(&D3D12CreateCommandQueueDetour),
                          reinterpret_cast<LPVOID*>(&g_originalD3D12CreateCommandQueue));
            MH_EnableHook(createQueue);
            Log("d3d12 device captured");
        }
    }
    return result;
}

void* FindPresentAddressD3D12()
{
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (d3d12 == nullptr || dxgi == nullptr) {
        return nullptr;
    }
    auto createDevice = reinterpret_cast<D3D12CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    auto createFactory =
        reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (createDevice == nullptr || createFactory == nullptr) {
        return nullptr;
    }

    ID3D12Device* device = nullptr;
    if (FAILED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                            reinterpret_cast<void**>(&device)))) {
        return nullptr;
    }

    D3D12_COMMAND_QUEUE_DESC queueDescription = {};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    device->CreateCommandQueue(&queueDescription, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));

    IDXGIFactory2* factory = nullptr;
    createFactory(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));

    void* present = nullptr;
    if (queue != nullptr && factory != nullptr) {
        const wchar_t className[] = L"WxrHookDummy12";
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className;
        RegisterClassExW(&windowClass);

        HWND window = CreateWindowExW(0, className, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
        if (window != nullptr) {
            DXGI_SWAP_CHAIN_DESC1 swapDescription = {};
            swapDescription.Width = 2;
            swapDescription.Height = 2;
            swapDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapDescription.SampleDesc.Count = 1;
            swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapDescription.BufferCount = 2;
            swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

            IDXGISwapChain1* swapChain = nullptr;
            if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, window, &swapDescription, nullptr, nullptr,
                                                          &swapChain))) {
                present = (*reinterpret_cast<void***>(swapChain))[8];
                swapChain->Release();
            }
            DestroyWindow(window);
        }
        UnregisterClassW(className, GetModuleHandleW(nullptr));
    }

    if (factory != nullptr) {
        factory->Release();
    }
    if (queue != nullptr) {
        queue->Release();
    }
    if (device != nullptr) {
        device->Release();
    }
    return present;
}

bool InstallD3D12Hooks()
{
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (d3d12 == nullptr) {
        return false;
    }
    auto createDevice = reinterpret_cast<D3D12CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (createDevice == nullptr) {
        return false;
    }
    if (g_originalD3D12CreateDevice == nullptr) {
        MH_CreateHook(createDevice, reinterpret_cast<LPVOID>(&D3D12CreateDeviceDetour),
                      reinterpret_cast<LPVOID*>(&g_originalD3D12CreateDevice));
        MH_EnableHook(createDevice);
        Log("D3D12CreateDevice hooked");
    }
    return true;
}

bool InstallOpenGLHooks()
{
    HMODULE opengl = GetModuleHandleW(L"opengl32.dll");
    if (opengl == nullptr) {
        return false;
    }
    if (g_glReadPixels == nullptr) {
        g_glReadPixels = reinterpret_cast<GlReadPixelsFn>(GetProcAddress(opengl, "glReadPixels"));
        g_wglGetCurrentDC = reinterpret_cast<WglGetCurrentDcFn>(GetProcAddress(opengl, "wglGetCurrentDC"));
        g_wglGetCurrentContext =
            reinterpret_cast<WglGetCurrentContextFn>(GetProcAddress(opengl, "wglGetCurrentContext"));
    }

    bool hooked = false;
    void* wglSwap = GetProcAddress(opengl, "wglSwapBuffers");
    if (wglSwap != nullptr && g_originalWglSwapBuffers == nullptr) {
        MH_CreateHook(wglSwap, reinterpret_cast<LPVOID>(&WglSwapBuffersDetour),
                      reinterpret_cast<LPVOID*>(&g_originalWglSwapBuffers));
        MH_EnableHook(wglSwap);
        hooked = true;
        Log("wglSwapBuffers hooked");
    }

    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (gdi != nullptr && g_originalSwapBuffers == nullptr) {
        void* gdiSwap = GetProcAddress(gdi, "SwapBuffers");
        if (gdiSwap != nullptr) {
            MH_CreateHook(gdiSwap, reinterpret_cast<LPVOID>(&SwapBuffersDetour),
                          reinterpret_cast<LPVOID*>(&g_originalSwapBuffers));
            MH_EnableHook(gdiSwap);
            hooked = true;
            Log("gdi32 SwapBuffers hooked");
        }
    }
    return hooked;
}

std::uint32_t FindVulkanMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    if (g_vkGetPhysicalDeviceMemoryProperties == nullptr) {
        return 0;
    }
    VkPhysicalDeviceMemoryProperties memory = {};
    g_vkGetPhysicalDeviceMemoryProperties(g_vkPhysicalDevice, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

void LoadVulkanDeviceFunctions()
{
    if (g_vkDevice == VK_NULL_HANDLE || g_vkGetDeviceProcAddr == nullptr) {
        return;
    }
    g_vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(g_vkGetDeviceProcAddr(g_vkDevice, "vkGetDeviceQueue"));
    g_vkGetSwapchainImagesKHR =
        reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(g_vkGetDeviceProcAddr(g_vkDevice, "vkGetSwapchainImagesKHR"));
    g_vkCreateCommandPool =
        reinterpret_cast<PFN_vkCreateCommandPool>(g_vkGetDeviceProcAddr(g_vkDevice, "vkCreateCommandPool"));
    g_vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
        g_vkGetDeviceProcAddr(g_vkDevice, "vkAllocateCommandBuffers"));
    g_vkCreateFence = reinterpret_cast<PFN_vkCreateFence>(g_vkGetDeviceProcAddr(g_vkDevice, "vkCreateFence"));
    g_vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(g_vkGetDeviceProcAddr(g_vkDevice, "vkCreateBuffer"));
    g_vkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
        g_vkGetDeviceProcAddr(g_vkDevice, "vkGetBufferMemoryRequirements"));
    g_vkAllocateMemory =
        reinterpret_cast<PFN_vkAllocateMemory>(g_vkGetDeviceProcAddr(g_vkDevice, "vkAllocateMemory"));
    g_vkBindBufferMemory =
        reinterpret_cast<PFN_vkBindBufferMemory>(g_vkGetDeviceProcAddr(g_vkDevice, "vkBindBufferMemory"));
    g_vkResetCommandBuffer =
        reinterpret_cast<PFN_vkResetCommandBuffer>(g_vkGetDeviceProcAddr(g_vkDevice, "vkResetCommandBuffer"));
    g_vkBeginCommandBuffer =
        reinterpret_cast<PFN_vkBeginCommandBuffer>(g_vkGetDeviceProcAddr(g_vkDevice, "vkBeginCommandBuffer"));
    g_vkCmdPipelineBarrier =
        reinterpret_cast<PFN_vkCmdPipelineBarrier>(g_vkGetDeviceProcAddr(g_vkDevice, "vkCmdPipelineBarrier"));
    g_vkCmdCopyImageToBuffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
        g_vkGetDeviceProcAddr(g_vkDevice, "vkCmdCopyImageToBuffer"));
    g_vkEndCommandBuffer =
        reinterpret_cast<PFN_vkEndCommandBuffer>(g_vkGetDeviceProcAddr(g_vkDevice, "vkEndCommandBuffer"));
    g_vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(g_vkGetDeviceProcAddr(g_vkDevice, "vkQueueSubmit"));
    g_vkWaitForFences =
        reinterpret_cast<PFN_vkWaitForFences>(g_vkGetDeviceProcAddr(g_vkDevice, "vkWaitForFences"));
    g_vkResetFences = reinterpret_cast<PFN_vkResetFences>(g_vkGetDeviceProcAddr(g_vkDevice, "vkResetFences"));
    g_vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(g_vkGetDeviceProcAddr(g_vkDevice, "vkMapMemory"));
    g_vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(g_vkGetDeviceProcAddr(g_vkDevice, "vkUnmapMemory"));
}

void CaptureVulkan(const VkPresentInfoKHR* presentInfo)
{
    if (!EnsureWriter() || !ShouldCapture()) {
        return;
    }
    if (g_vkDevice == VK_NULL_HANDLE || g_vkQueue == VK_NULL_HANDLE || g_vkCommandBuffer == VK_NULL_HANDLE ||
        g_vkFence == VK_NULL_HANDLE || g_vkExtent.width == 0 || g_vkExtent.height == 0) {
        return;
    }
    if (presentInfo == nullptr || presentInfo->swapchainCount == 0 ||
        presentInfo->pSwapchains[0] != g_vkSwapchain) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_vkMutex);

    std::uint32_t imageCount = 0;
    if (g_vkGetSwapchainImagesKHR(g_vkDevice, g_vkSwapchain, &imageCount, nullptr) != VK_SUCCESS ||
        imageCount == 0) {
        return;
    }
    static std::vector<VkImage> images;
    images.resize(imageCount);
    if (g_vkGetSwapchainImagesKHR(g_vkDevice, g_vkSwapchain, &imageCount, images.data()) != VK_SUCCESS) {
        return;
    }
    const std::uint32_t imageIndex = presentInfo->pImageIndices != nullptr ? presentInfo->pImageIndices[0] : 0;
    if (imageIndex >= imageCount) {
        return;
    }
    VkImage image = images[imageIndex];

    const VkDeviceSize needed = static_cast<VkDeviceSize>(g_vkExtent.width) * g_vkExtent.height * 4;
    if (g_vkReadback == VK_NULL_HANDLE || g_vkReadbackSize < needed) {
        if (g_vkReadback != VK_NULL_HANDLE) {
            g_vkReadback = VK_NULL_HANDLE;
            g_vkReadbackMemory = VK_NULL_HANDLE;
            g_vkReadbackSize = 0;
        }

        VkBufferCreateInfo bufferDescription = {};
        bufferDescription.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferDescription.size = needed;
        bufferDescription.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferDescription.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (g_vkCreateBuffer(g_vkDevice, &bufferDescription, nullptr, &g_vkReadback) != VK_SUCCESS) {
            return;
        }

        VkMemoryRequirements requirements = {};
        g_vkGetBufferMemoryRequirements(g_vkDevice, g_vkReadback, &requirements);

        VkMemoryAllocateInfo allocation = {};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = FindVulkanMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (g_vkAllocateMemory(g_vkDevice, &allocation, nullptr, &g_vkReadbackMemory) != VK_SUCCESS) {
            return;
        }
        g_vkBindBufferMemory(g_vkDevice, g_vkReadback, g_vkReadbackMemory, 0);
        g_vkReadbackSize = needed;
        Log("vulkan readback: " + std::to_string(g_vkExtent.width) + "x" + std::to_string(g_vkExtent.height));
    }

    g_vkResetCommandBuffer(g_vkCommandBuffer, 0);
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    g_vkBeginCommandBuffer(g_vkCommandBuffer, &begin);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    g_vkCmdPipelineBarrier(g_vkCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {g_vkExtent.width, g_vkExtent.height, 1};
    g_vkCmdCopyImageToBuffer(g_vkCommandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vkReadback, 1,
                             &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    g_vkCmdPipelineBarrier(g_vkCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &barrier);

    g_vkEndCommandBuffer(g_vkCommandBuffer);

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_vkCommandBuffer;
    g_vkResetFences(g_vkDevice, 1, &g_vkFence);
    if (g_vkQueueSubmit(g_vkQueue, 1, &submit, g_vkFence) != VK_SUCCESS) {
        return;
    }
    g_vkWaitForFences(g_vkDevice, 1, &g_vkFence, VK_TRUE, 2000000000ull);

    void* mapped = nullptr;
    if (g_vkMapMemory(g_vkDevice, g_vkReadbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        const int width = static_cast<int>(g_vkExtent.width);
        const int height = static_cast<int>(g_vkExtent.height);
        if (g_vkFormat == VK_FORMAT_B8G8R8A8_UNORM) {
            g_writer.Write(static_cast<const std::uint8_t*>(mapped), width, height, width * 4,
                           static_cast<std::int64_t>(GetTickCount64()));
        } else {
            std::vector<std::uint8_t> converted(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
            const auto* source = static_cast<const std::uint8_t*>(mapped);
            for (int y = 0; y < height; ++y) {
                const auto* row = source + static_cast<std::size_t>(y) * width * 4;
                auto* out = converted.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
                for (int x = 0; x < width; ++x) {
                    out[x * 4 + 0] = row[x * 4 + 2];
                    out[x * 4 + 1] = row[x * 4 + 1];
                    out[x * 4 + 2] = row[x * 4 + 0];
                    out[x * 4 + 3] = row[x * 4 + 3];
                }
            }
            g_writer.Write(converted.data(), width, height, width * 4, static_cast<std::int64_t>(GetTickCount64()));
        }
        g_vkUnmapMemory(g_vkDevice, g_vkReadbackMemory);
    }

    ++g_vkFrameCount;
    if (g_vkFrameCount == 1 || (g_vkFrameCount % 120) == 0) {
        Log("vulkan frame " + std::to_string(g_vkFrameCount) + ": " + std::to_string(g_vkExtent.width) + "x" +
            std::to_string(g_vkExtent.height));
    }
}

VkResult VKAPI_CALL VkCreateDeviceDetour(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* createInfo,
                                         const VkAllocationCallbacks* allocator, VkDevice* device)
{
    VkResult result = g_originalVkCreateDevice(physicalDevice, createInfo, allocator, device);
    if (result == VK_SUCCESS && device != nullptr && *device != VK_NULL_HANDLE && g_vkDevice == VK_NULL_HANDLE) {
        g_vkDevice = *device;
        g_vkPhysicalDevice = physicalDevice;
        LoadVulkanDeviceFunctions();

        if (createInfo != nullptr && createInfo->queueCreateInfoCount > 0) {
            const std::uint32_t family = createInfo->pQueueCreateInfos[0].queueFamilyIndex;
            if (g_vkGetDeviceQueue != nullptr) {
                g_vkGetDeviceQueue(g_vkDevice, family, 0, &g_vkQueue);
            }

            VkCommandPoolCreateInfo poolDescription = {};
            poolDescription.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolDescription.queueFamilyIndex = family;
            poolDescription.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            if (g_vkCreateCommandPool != nullptr) {
                g_vkCreateCommandPool(g_vkDevice, &poolDescription, nullptr, &g_vkCommandPool);
            }

            VkCommandBufferAllocateInfo commandAllocation = {};
            commandAllocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            commandAllocation.commandPool = g_vkCommandPool;
            commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commandAllocation.commandBufferCount = 1;
            if (g_vkAllocateCommandBuffers != nullptr) {
                g_vkAllocateCommandBuffers(g_vkDevice, &commandAllocation, &g_vkCommandBuffer);
            }

            VkFenceCreateInfo fenceDescription = {};
            fenceDescription.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (g_vkCreateFence != nullptr) {
                g_vkCreateFence(g_vkDevice, &fenceDescription, nullptr, &g_vkFence);
            }
        }
        Log("vulkan device captured");
    }
    return result;
}

VkResult VKAPI_CALL VkCreateSwapchainDetour(VkDevice device, const VkSwapchainCreateInfoKHR* createInfo,
                                            const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain)
{
    if (createInfo == nullptr) {
        return g_originalVkCreateSwapchain(device, createInfo, allocator, swapchain);
    }

    // Swapchain images need TRANSFER_SRC so the hook can copy them. Apps usually only ask for
    // COLOR_ATTACHMENT, so add the bit behind their back.
    VkSwapchainCreateInfoKHR modified = *createInfo;
    modified.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkResult result = g_originalVkCreateSwapchain(device, &modified, allocator, swapchain);
    if (result == VK_SUCCESS && swapchain != nullptr && *swapchain != VK_NULL_HANDLE) {
        g_vkSwapchain = *swapchain;
        g_vkFormat = createInfo->imageFormat;
        g_vkExtent = createInfo->imageExtent;
        Log("vulkan swapchain captured: " + std::to_string(g_vkExtent.width) + "x" +
            std::to_string(g_vkExtent.height));
    }
    return result;
}

VkResult VKAPI_CALL VkQueuePresentDetour(VkQueue queue, const VkPresentInfoKHR* presentInfo)
{
    if (g_vkDevice != VK_NULL_HANDLE && g_vkSwapchain != VK_NULL_HANDLE && presentInfo != nullptr &&
        presentInfo->swapchainCount > 0 && presentInfo->pSwapchains[0] == g_vkSwapchain) {
        CaptureVulkan(presentInfo);
    }
    return g_originalVkQueuePresent(queue, presentInfo);
}

bool InstallVulkanHooks()
{
    HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (vulkan == nullptr) {
        return false;
    }
    if (g_vkGetDeviceProcAddr == nullptr) {
        g_vkGetDeviceProcAddr =
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vulkan, "vkGetDeviceProcAddr"));
        g_vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            GetProcAddress(vulkan, "vkGetPhysicalDeviceMemoryProperties"));
    }

    void* createDevice = GetProcAddress(vulkan, "vkCreateDevice");
    void* createSwapchain = GetProcAddress(vulkan, "vkCreateSwapchainKHR");
    void* queuePresent = GetProcAddress(vulkan, "vkQueuePresentKHR");

    bool hooked = false;
    if (createDevice != nullptr && g_originalVkCreateDevice == nullptr) {
        MH_CreateHook(createDevice, reinterpret_cast<LPVOID>(&VkCreateDeviceDetour),
                      reinterpret_cast<LPVOID*>(&g_originalVkCreateDevice));
        MH_EnableHook(createDevice);
        hooked = true;
        Log("vkCreateDevice hooked");
    }
    if (createSwapchain != nullptr && g_originalVkCreateSwapchain == nullptr) {
        MH_CreateHook(createSwapchain, reinterpret_cast<LPVOID>(&VkCreateSwapchainDetour),
                      reinterpret_cast<LPVOID*>(&g_originalVkCreateSwapchain));
        MH_EnableHook(createSwapchain);
        hooked = true;
        Log("vkCreateSwapchainKHR hooked");
    }
    if (queuePresent != nullptr && g_originalVkQueuePresent == nullptr) {
        MH_CreateHook(queuePresent, reinterpret_cast<LPVOID>(&VkQueuePresentDetour),
                      reinterpret_cast<LPVOID*>(&g_originalVkQueuePresent));
        MH_EnableHook(queuePresent);
        hooked = true;
        Log("vkQueuePresentKHR hooked");
    }
    return hooked;
}

void SignalReady()
{
    wchar_t name[64] = {};
    swprintf_s(name, L"Local\\wxr_hook_ready_%lu", GetCurrentProcessId());
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (event != nullptr) {
        SetEvent(event);
        CloseHandle(event);
        Log("ready event signaled");
    }
}

bool QueryDdrawDescription(IDirectDrawSurface7* surface, DDSURFACEDESC2& description)
{
    description = {};
    description.dwSize = sizeof(description);
    if (SUCCEEDED(surface->GetSurfaceDesc(&description))) {
        return true;
    }

    // Legacy IDirectDrawSurface (v1) implementations reject the v2 structure size. The relevant
    // fields sit at the same offsets in DDSURFACEDESC, so retry with the older size.
    DDSURFACEDESC legacy = {};
    legacy.dwSize = sizeof(legacy);
    using GetSurfaceDesc1Fn = HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface*, LPDDSURFACEDESC);
    void** vtable = *reinterpret_cast<void***>(surface);
    auto getSurfaceDesc = reinterpret_cast<GetSurfaceDesc1Fn>(vtable[22]);
    if (FAILED(getSurfaceDesc(reinterpret_cast<IDirectDrawSurface*>(surface), &legacy))) {
        return false;
    }

    description.dwFlags = legacy.dwFlags;
    description.dwHeight = legacy.dwHeight;
    description.dwWidth = legacy.dwWidth;
    description.lPitch = legacy.lPitch;
    description.ddpfPixelFormat = legacy.ddpfPixelFormat;
    return true;
}

bool LockDdrawSurface(IDirectDrawSurface7* surface, DDSURFACEDESC2& locked)
{
    locked = {};
    locked.dwSize = sizeof(locked);
    if (SUCCEEDED(surface->Lock(nullptr, &locked, DDLOCK_READONLY | DDLOCK_WAIT, nullptr))) {
        return true;
    }

    DDSURFACEDESC legacy = {};
    legacy.dwSize = sizeof(legacy);
    using Lock1Fn = HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface*, LPRECT, LPDDSURFACEDESC, DWORD, HANDLE);
    void** vtable = *reinterpret_cast<void***>(surface);
    auto lock = reinterpret_cast<Lock1Fn>(vtable[25]);
    if (SUCCEEDED(lock(reinterpret_cast<IDirectDrawSurface*>(surface), nullptr, &legacy,
                       DDLOCK_READONLY | DDLOCK_WAIT, nullptr))) {
        locked.dwFlags = legacy.dwFlags;
        locked.dwHeight = legacy.dwHeight;
        locked.dwWidth = legacy.dwWidth;
        locked.lPitch = legacy.lPitch;
        locked.lpSurface = legacy.lpSurface;
        locked.ddpfPixelFormat = legacy.ddpfPixelFormat;
        return true;
    }

    // Some old drivers ignore READONLY. Retry without it before giving up.
    locked = {};
    locked.dwSize = sizeof(locked);
    if (SUCCEEDED(surface->Lock(nullptr, &locked, DDLOCK_WAIT, nullptr))) {
        return true;
    }

    locked = {};
    locked.dwSize = sizeof(legacy);
    if (SUCCEEDED(lock(reinterpret_cast<IDirectDrawSurface*>(surface), nullptr,
                       reinterpret_cast<LPDDSURFACEDESC>(&locked), DDLOCK_WAIT, nullptr))) {
        return locked.lpSurface != nullptr;
    }
    return false;
}

void UnlockDdrawSurface(IDirectDrawSurface7* surface)
{
    // Call the vtable entry directly so a future Unlock detour cannot re-enter this function.
    using Unlock1Fn = HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface*, LPRECT);
    void** vtable = *reinterpret_cast<void***>(surface);
    auto unlock = reinterpret_cast<Unlock1Fn>(vtable[kDdrawUnlockVtableIndex]);
    unlock(reinterpret_cast<IDirectDrawSurface*>(surface), nullptr);
}

std::uint8_t ExpandDdrawChannel(std::uint32_t pixel, std::uint32_t mask)
{
    if (mask == 0) {
        return 0;
    }
    int shift = 0;
    while ((mask & 1u) == 0) {
        mask >>= 1;
        ++shift;
    }
    const std::uint32_t value = (pixel >> shift) & mask;
    return static_cast<std::uint8_t>((value * 255u + mask / 2u) / mask);
}

// Reads one surface and publishes it as BGRA. Handles the 32-bit primary/backbuffer surfaces most
// DirectDraw apps use, plus the 16-bit RGB565/RGB555 surfaces older games and CNC-Ddraw request.
void CaptureDdrawSurface(IDirectDrawSurface7* surface)
{
    if (surface == nullptr || g_inDdrawCapture) {
        return;
    }
    if (!EnsureWriter()) {
        return;
    }

    g_inDdrawCapture = true;

    DDSURFACEDESC2 description = {};
    if (!QueryDdrawDescription(surface, description) || description.dwWidth < static_cast<DWORD>(kDdrawMinWidth) ||
        description.dwHeight < static_cast<DWORD>(kDdrawMinHeight) ||
        description.dwWidth > static_cast<DWORD>(kSharedMaxWidth) ||
        description.dwHeight > static_cast<DWORD>(kSharedMaxHeight)) {
        g_inDdrawCapture = false;
        return;
    }

    const DWORD bits = description.ddpfPixelFormat.dwRGBBitCount;
    if (bits != 32 && bits != 16) {
        g_inDdrawCapture = false;
        return;
    }

    DDSURFACEDESC2 locked = {};
    if (!LockDdrawSurface(surface, locked) || locked.lpSurface == nullptr) {
        g_inDdrawCapture = false;
        return;
    }

    const int width = static_cast<int>(description.dwWidth);
    const int height = static_cast<int>(description.dwHeight);
    const auto* base = static_cast<const std::uint8_t*>(locked.lpSurface);
    const int pitch = static_cast<int>(locked.lPitch);

    if (bits == 32) {
        g_writer.Write(base, width, height, pitch, static_cast<std::int64_t>(GetTickCount64()));
    } else {
        static thread_local std::vector<std::uint8_t> converted;
        converted.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        const std::uint32_t redMask = description.ddpfPixelFormat.dwRBitMask;
        const std::uint32_t greenMask = description.ddpfPixelFormat.dwGBitMask;
        const std::uint32_t blueMask = description.ddpfPixelFormat.dwBBitMask;
        for (int y = 0; y < height; ++y) {
            const auto* sourceRow = reinterpret_cast<const std::uint16_t*>(base + static_cast<std::size_t>(y) * pitch);
            std::uint8_t* destinationRow = converted.data() + static_cast<std::size_t>(y) *
                                                               static_cast<std::size_t>(width) * 4;
            for (int x = 0; x < width; ++x) {
                const std::uint16_t pixel = sourceRow[x];
                destinationRow[x * 4 + 0] = ExpandDdrawChannel(pixel, blueMask);
                destinationRow[x * 4 + 1] = ExpandDdrawChannel(pixel, greenMask);
                destinationRow[x * 4 + 2] = ExpandDdrawChannel(pixel, redMask);
                destinationRow[x * 4 + 3] = 255;
            }
        }
        g_writer.Write(converted.data(), width, height, width * 4, static_cast<std::int64_t>(GetTickCount64()));
    }

    UnlockDdrawSurface(surface);
    g_inDdrawCapture = false;

    ++g_ddrawFrameCount;
    if (g_ddrawFrameCount == 1 || (g_ddrawFrameCount % 120) == 0) {
        Log("ddraw frame " + std::to_string(g_ddrawFrameCount) + ": " + std::to_string(width) + "x" +
            std::to_string(height) + " " + std::to_string(bits) + "bpp");
    }
}

// Primary surfaces are desktop-sized in windowed mode, so a Blt's destination only has the app's
// rectangle composited into a much larger desktop image. Prefer the source back buffer in that case.
// Blits into an offscreen back buffer are captured at the destination instead, which accumulates
// sprite-by-sprite rendering.
void CaptureDdrawResult(IDirectDrawSurface7* destination, IDirectDrawSurface7* source)
{
    bool destinationIsPrimary = false;
    if (destination != nullptr) {
        DDSCAPS2 caps = {};
        if (SUCCEEDED(destination->GetCaps(&caps))) {
            destinationIsPrimary = (caps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0;
        }
    }

    DDSURFACEDESC2 description = {};
    const bool destinationEligible =
        destination != nullptr && QueryDdrawDescription(destination, description) &&
        description.dwWidth >= static_cast<DWORD>(kDdrawMinWidth) &&
        description.dwHeight >= static_cast<DWORD>(kDdrawMinHeight);
    const bool sourceEligible = source != nullptr && QueryDdrawDescription(source, description) &&
                                description.dwWidth >= static_cast<DWORD>(kDdrawMinWidth) &&
                                description.dwHeight >= static_cast<DWORD>(kDdrawMinHeight);

    if (destinationIsPrimary && sourceEligible) {
        CaptureDdrawSurface(source);
    } else if (destinationEligible) {
        CaptureDdrawSurface(destination);
    } else if (sourceEligible) {
        CaptureDdrawSurface(source);
    }
}

template <int Slot>
HRESULT STDMETHODCALLTYPE DdrawBltDetour(IDirectDrawSurface7* destination, LPRECT destinationRect,
                                         IDirectDrawSurface7* source, LPRECT sourceRect, DWORD flags,
                                         LPDDBLTFX effects)
{
    const HRESULT result =
        g_originalDdrawBlt[Slot] != nullptr
            ? g_originalDdrawBlt[Slot](destination, destinationRect, source, sourceRect, flags, effects)
            : DDERR_GENERIC;
    if (SUCCEEDED(result) && EnsureWriter() && ShouldCapture()) {
        CaptureDdrawResult(destination, source);
    }
    return result;
}

template <int Slot>
HRESULT STDMETHODCALLTYPE DdrawBltFastDetour(IDirectDrawSurface7* destination, DWORD x, DWORD y,
                                             IDirectDrawSurface7* source, LPRECT sourceRect, DWORD flags)
{
    const HRESULT result = g_originalDdrawBltFast[Slot] != nullptr
                               ? g_originalDdrawBltFast[Slot](destination, x, y, source, sourceRect, flags)
                               : DDERR_GENERIC;
    if (SUCCEEDED(result) && EnsureWriter() && ShouldCapture()) {
        CaptureDdrawResult(destination, source);
    }
    return result;
}

template <int Slot>
HRESULT STDMETHODCALLTYPE DdrawFlipDetour(IDirectDrawSurface7* surface, IDirectDrawSurface7* targetOverride,
                                          DWORD flags)
{
    // Read the attached back buffer before the flip swaps it into the primary.
    if (surface != nullptr && EnsureWriter() && ShouldCapture()) {
        IDirectDrawSurface7* backBuffer = nullptr;
        DDSCAPS2 caps = {};
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        if (SUCCEEDED(surface->GetAttachedSurface(&caps, &backBuffer)) && backBuffer != nullptr) {
            CaptureDdrawSurface(backBuffer);
            backBuffer->Release();
        }
    }

    const HRESULT result = g_originalDdrawFlip[Slot] != nullptr
                               ? g_originalDdrawFlip[Slot](surface, targetOverride, flags)
                               : DDERR_GENERIC;
    if (SUCCEEDED(result) && surface != nullptr && EnsureWriter() && ShouldCapture()) {
        CaptureDdrawSurface(surface);
    }
    return result;
}

using DdrawBltDetourFn = HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface7*, LPRECT, IDirectDrawSurface7*, LPRECT,
                                                     DWORD, LPDDBLTFX);
using DdrawBltFastDetourFn = HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface7*, DWORD, DWORD, IDirectDrawSurface7*,
                                                         LPRECT, DWORD);
using DdrawFlipDetourFn = HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface7*, IDirectDrawSurface7*, DWORD);

const DdrawBltDetourFn kBltDetours[kMaxDdrawHookSlots] = {
    &DdrawBltDetour<0>, &DdrawBltDetour<1>, &DdrawBltDetour<2>,
    &DdrawBltDetour<3>, &DdrawBltDetour<4>, &DdrawBltDetour<5>,
};
const DdrawBltFastDetourFn kBltFastDetours[kMaxDdrawHookSlots] = {
    &DdrawBltFastDetour<0>, &DdrawBltFastDetour<1>, &DdrawBltFastDetour<2>,
    &DdrawBltFastDetour<3>, &DdrawBltFastDetour<4>, &DdrawBltFastDetour<5>,
};
const DdrawFlipDetourFn kFlipDetours[kMaxDdrawHookSlots] = {
    &DdrawFlipDetour<0>, &DdrawFlipDetour<1>, &DdrawFlipDetour<2>,
    &DdrawFlipDetour<3>, &DdrawFlipDetour<4>, &DdrawFlipDetour<5>,
};

template <typename OriginalFn, typename DetourFn>
bool HookDdrawAddress(void* address, OriginalFn* originals, const DetourFn* detours, const char* name)
{
    if (address == nullptr) {
        return false;
    }
    for (void* hooked : g_ddrawHookedAddresses) {
        if (hooked == address) {
            return true;
        }
    }
    int slot = 0;
    while (slot < kMaxDdrawHookSlots && originals[slot] != nullptr) {
        ++slot;
    }
    if (slot >= kMaxDdrawHookSlots) {
        return false;
    }
    if (MH_CreateHook(address, reinterpret_cast<LPVOID>(detours[slot]),
                      reinterpret_cast<LPVOID*>(&originals[slot])) != MH_OK) {
        return false;
    }
    MH_EnableHook(address);
    g_ddrawHookedAddresses.push_back(address);
    Log(std::string("ddraw ") + name + " hooked at slot " + std::to_string(slot));
    return true;
}

void AddDdrawSurfaceAddresses(IDirectDrawSurface7* surface, std::vector<void*>& blt, std::vector<void*>& bltFast,
                              std::vector<void*>& flip)
{
    void** vtable = *reinterpret_cast<void***>(surface);
    blt.push_back(vtable[kDdrawBltVtableIndex]);
    bltFast.push_back(vtable[kDdrawBltFastVtableIndex]);
    flip.push_back(vtable[kDdrawFlipVtableIndex]);
}

// Collects the Blt/BltFast/Flip addresses from both the legacy IDirectDraw and IDirectDraw7
// surface implementations. CNC-Ddraw exposes its own ddraw.dll, so these are the functions its
// surfaces will use too.
void FindDdrawSurfaceAddresses(std::vector<void*>& blt, std::vector<void*>& bltFast, std::vector<void*>& flip)
{
    HMODULE ddraw = GetModuleHandleW(L"ddraw.dll");
    if (ddraw == nullptr) {
        return;
    }

    const wchar_t className[] = L"WxrHookDummy7";
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);
    HWND window = CreateWindowExW(0, className, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);

    // IDirectDraw7 surfaces.
    auto createEx = reinterpret_cast<HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*)>(
        GetProcAddress(ddraw, "DirectDrawCreateEx"));
    if (createEx != nullptr) {
        IDirectDraw7* directDraw = nullptr;
        if (SUCCEEDED(createEx(nullptr, reinterpret_cast<void**>(&directDraw), IID_IDirectDraw7, nullptr)) &&
            directDraw != nullptr) {
            if (window != nullptr) {
                directDraw->SetCooperativeLevel(window, DDSCL_NORMAL);
            }
            DDSURFACEDESC2 description = {};
            description.dwSize = sizeof(description);
            description.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            description.dwWidth = 4;
            description.dwHeight = 4;
            description.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
            IDirectDrawSurface7* surface = nullptr;
            if (SUCCEEDED(directDraw->CreateSurface(&description, &surface, nullptr)) && surface != nullptr) {
                AddDdrawSurfaceAddresses(surface, blt, bltFast, flip);
                surface->Release();
            }
            directDraw->Release();
        }
    }

    // Legacy IDirectDraw surfaces (v1). Many CNC-Ddraw titles use these directly.
    auto createLegacy = reinterpret_cast<HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*)>(
        GetProcAddress(ddraw, "DirectDrawCreate"));
    if (createLegacy != nullptr) {
        LPDIRECTDRAW directDraw = nullptr;
        if (SUCCEEDED(createLegacy(nullptr, &directDraw, nullptr)) && directDraw != nullptr) {
            if (window != nullptr) {
                directDraw->SetCooperativeLevel(window, DDSCL_NORMAL);
            }
            DDSURFACEDESC description = {};
            description.dwSize = sizeof(description);
            description.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            description.dwWidth = 4;
            description.dwHeight = 4;
            description.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
            LPDIRECTDRAWSURFACE surface = nullptr;
            if (SUCCEEDED(directDraw->CreateSurface(&description, &surface, nullptr)) && surface != nullptr) {
                AddDdrawSurfaceAddresses(reinterpret_cast<IDirectDrawSurface7*>(surface), blt, bltFast, flip);
                surface->Release();
            }
            directDraw->Release();
        }
    }

    if (window != nullptr) {
        DestroyWindow(window);
    }
    UnregisterClassW(className, GetModuleHandleW(nullptr));
}

bool InstallDdrawHooks()
{
    if (GetModuleHandleW(L"ddraw.dll") == nullptr) {
        return false;
    }
    if (!g_ddrawHookedAddresses.empty()) {
        return true;
    }

    std::vector<void*> blt;
    std::vector<void*> bltFast;
    std::vector<void*> flip;
    FindDdrawSurfaceAddresses(blt, bltFast, flip);
    if (blt.empty()) {
        Log("ddraw surface addresses not found");
        return false;
    }

    bool hooked = false;
    for (void* address : blt) {
        hooked |= HookDdrawAddress(address, g_originalDdrawBlt, kBltDetours, "Blt");
    }
    for (void* address : bltFast) {
        hooked |= HookDdrawAddress(address, g_originalDdrawBltFast, kBltFastDetours, "BltFast");
    }
    for (void* address : flip) {
        hooked |= HookDdrawAddress(address, g_originalDdrawFlip, kFlipDetours, "Flip");
    }
    return hooked;
}

HRESULT WINAPI PresentDetour(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    CaptureFrame(swapChain);
    return g_originalPresent != nullptr ? g_originalPresent(swapChain, syncInterval, flags) : E_FAIL;
}

bool InstallPresentHook()
{
    void* present = FindPresentAddress();
    if (present == nullptr) {
        return false;
    }

    const MH_STATUS created =
        MH_CreateHook(present, reinterpret_cast<LPVOID>(&PresentDetour), reinterpret_cast<LPVOID*>(&g_originalPresent));
    Log("MH_CreateHook(Present)=" + std::to_string(static_cast<int>(created)));
    if (created != MH_OK) {
        return false;
    }

    const MH_STATUS enabled = MH_EnableHook(present);
    Log("MH_EnableHook(Present)=" + std::to_string(static_cast<int>(enabled)));
    return enabled == MH_OK;
}

bool InstallEndSceneHook()
{
    if (g_originalEndScene != nullptr) {
        return true;
    }

    void* endScene = FindEndSceneAddress();
    if (endScene == nullptr) {
        return false;
    }

    const MH_STATUS created = MH_CreateHook(endScene, reinterpret_cast<LPVOID>(&EndSceneDetour),
                                            reinterpret_cast<LPVOID*>(&g_originalEndScene));
    Log("MH_CreateHook(EndScene)=" + std::to_string(static_cast<int>(created)));
    if (created != MH_OK) {
        // Another path (the Direct3DCreate9 CreateDevice detour) may have hooked it first.
        return created == MH_ERROR_ALREADY_CREATED && g_originalEndScene != nullptr;
    }

    const MH_STATUS enabled = MH_EnableHook(endScene);
    Log("MH_EnableHook(EndScene)=" + std::to_string(static_cast<int>(enabled)));
    return enabled == MH_OK;
}

bool InstallD3D8HookForTarget()
{
    return InstallD3D8Hooks(&g_writer, []() { return ShouldCapture(); }, [](const char* text) { Log(text); });
}

bool TryInstallHook(bool& installed, ULONGLONG& nextAttempt, ULONGLONG now, bool (*install)())
{
    if (installed) {
        return true;
    }
    if (now < nextAttempt) {
        return false;
    }
    installed = install();
    nextAttempt = GetTickCount64() + (installed ? 0 : 2000);
    return installed;
}

// Stays resident for the life of the target process. Graphics modules often load long after the
// initial injection (Halo's wrapper reports "D3D9 not found!" during startup), so the thread
// rescans on a timer and on the host's re-hook event instead of giving up after one minute.
DWORD WINAPI HookThread(LPVOID /*parameter*/)
{
    g_log.open(ModuleDirectory(g_module) + L"\\wxr_hook.log", std::ios::app);
    Log("wxr hook dll loaded, pid=" + std::to_string(GetCurrentProcessId()));

    if (!g_writer.Open()) {
        Log("shared frame mapping not found; capture will be dropped");
    } else {
        Log(std::string("shared frame mapping opened=") + (g_writer.Enabled() ? "1" : "0") +
            " pid=" + std::to_string(GetCurrentProcessId()));
    }

    const MH_STATUS initialized = MH_Initialize();
    Log("MH_Initialize=" + std::to_string(static_cast<int>(initialized)));
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        return 0;
    }

    const std::wstring rescanName = std::wstring(kHookRescanEventPrefix) +
                                    std::to_wstring(GetCurrentProcessId());
    HANDLE rescan = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, rescanName.c_str());
    if (rescan == nullptr) {
        rescan = CreateEventW(nullptr, FALSE, FALSE, rescanName.c_str());
    }
    Log(std::string("rehook event ") + (rescan != nullptr ? "ready" : "unavailable"));

    bool hookedPresent = false;
    bool hookedCreate9 = false;
    bool hooked9 = false;
    bool hooked8 = false;
    bool hookedDdraw = false;
    bool hookedGL = false;
    bool hooked12 = false;
    bool hookedVK = false;
    bool readySignaled = false;
    unsigned int lastMask = 0;

    ULONGLONG nextPresent = 0;
    ULONGLONG nextCreate9 = 0;
    ULONGLONG nextEndScene9 = 0;
    ULONGLONG nextD3D8 = 0;
    ULONGLONG nextDdraw = 0;
    ULONGLONG nextGL = 0;
    ULONGLONG nextD3D12 = 0;
    ULONGLONG nextVK = 0;

    for (;;) {
        const ULONGLONG now = GetTickCount64();

        if (GetModuleHandleW(L"d3d11.dll") != nullptr || GetModuleHandleW(L"d3d12.dll") != nullptr) {
            TryInstallHook(hookedPresent, nextPresent, now, &InstallPresentHook);
        }
        if (GetModuleHandleW(L"d3d9.dll") != nullptr) {
            TryInstallHook(hookedCreate9, nextCreate9, now, &InstallD3D9CreateHooks);
            // For targets whose device already exists (injected after startup), the create hook
            // never fires, so probe a dummy device for EndScene as a slower fallback.
            TryInstallHook(hooked9, nextEndScene9, now, &InstallEndSceneHook);
        }
        if (GetModuleHandleW(L"d3d8.dll") != nullptr) {
            TryInstallHook(hooked8, nextD3D8, now, &InstallD3D8HookForTarget);
        }
        if (GetModuleHandleW(L"ddraw.dll") != nullptr) {
            TryInstallHook(hookedDdraw, nextDdraw, now, &InstallDdrawHooks);
        }
        if (GetModuleHandleW(L"opengl32.dll") != nullptr) {
            TryInstallHook(hookedGL, nextGL, now, &InstallOpenGLHooks);
        }
        if (GetModuleHandleW(L"d3d12.dll") != nullptr) {
            TryInstallHook(hooked12, nextD3D12, now, &InstallD3D12Hooks);
        }
        if (GetModuleHandleW(L"vulkan-1.dll") != nullptr) {
            TryInstallHook(hookedVK, nextVK, now, &InstallVulkanHooks);
        }

        // Release the suspended launcher once the first probe has run, so static imports are
        // hooked before the app creates its device.
        if (!readySignaled) {
            SignalReady();
            readySignaled = true;
        }

        const unsigned int mask = (hookedPresent ? 1u : 0u) | (hookedCreate9 ? 2u : 0u) | (hooked9 ? 4u : 0u) |
                                  (hooked8 ? 8u : 0u) | (hookedDdraw ? 16u : 0u) | (hookedGL ? 32u : 0u) |
                                  (hooked12 ? 64u : 0u) | (hookedVK ? 128u : 0u);
        if (mask != lastMask) {
            lastMask = mask;
            Log(std::string("hooks now: present=") + (hookedPresent ? "yes" : "no") + " create9=" +
                (hookedCreate9 ? "yes" : "no") + " d3d9=" + (hooked9 ? "yes" : "no") + " d3d8=" +
                (hooked8 ? "yes" : "no") + " ddraw=" + (hookedDdraw ? "yes" : "no") + " gl=" +
                (hookedGL ? "yes" : "no") + " d3d12=" + (hooked12 ? "yes" : "no") + " vulkan=" +
                (hookedVK ? "yes" : "no"));
        }

        if (rescan != nullptr) {
            WaitForSingleObject(rescan, 2000);
        } else {
            Sleep(2000);
        }
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
