#include "screen_capture.h"

#include <algorithm>
#include <cwchar>

#include "exclusions.h"
#include "process_list.h"

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

bool IsMostlyBlack(const void* pixels, int width, int height)
{
    const auto* bgra = static_cast<const std::uint8_t*>(pixels);
    int nonBlack = 0;
    int total = 0;
    for (int y = 0; y < height; y += 16) {
        const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; x += 16) {
            const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
            if (pixel[0] > 8 || pixel[1] > 8 || pixel[2] > 8) {
                ++nonBlack;
            }
            ++total;
        }
    }
    return total > 0 && (nonBlack * 100) < total;
}

}  // namespace

ScreenCapture::~ScreenCapture()
{
    Release();
}

bool ScreenCapture::Initialize(int targetWidth, int targetHeight, std::wstring& error, bool followForegroundWindow,
                               const std::wstring& targetExecutable)
{
    Release();

    followForegroundWindow_ = followForegroundWindow;
    targetExecutable_ = targetExecutable;

    screenDc_ = GetDC(nullptr);
    if (screenDc_ == nullptr) {
        error = L"GetDC failed; no desktop to capture.";
        return false;
    }

    sourceWidth_ = GetSystemMetrics(SM_CXSCREEN);
    sourceHeight_ = GetSystemMetrics(SM_CYSCREEN);
    if (sourceWidth_ <= 0 || sourceHeight_ <= 0) {
        error = L"The desktop reports an empty size.";
        Release();
        return false;
    }

    width_ = (targetWidth > 0) ? targetWidth : sourceWidth_;
    height_ = (targetHeight > 0) ? targetHeight : sourceHeight_;

    memoryDc_ = CreateCompatibleDC(screenDc_);
    if (memoryDc_ == nullptr) {
        error = L"CreateCompatibleDC failed.";
        Release();
        return false;
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -height_;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    bitmap_ = CreateDIBSection(screenDc_, &info, DIB_RGB_COLORS, &pixels_, nullptr, 0);
    if (bitmap_ == nullptr || pixels_ == nullptr) {
        error = L"CreateDIBSection failed.";
        Release();
        return false;
    }

    previousBitmap_ = SelectObject(memoryDc_, bitmap_);
    if (previousBitmap_ == nullptr || previousBitmap_ == HGDI_ERROR) {
        error = L"SelectObject failed.";
        Release();
        return false;
    }

    SetStretchBltMode(memoryDc_, COLORONCOLOR);
    SetBrushOrgEx(memoryDc_, 0, 0, nullptr);

    if (!targetExecutable_.empty()) {
        targetRefreshCountdown_ = 60;
    }
    return true;
}

bool ScreenCapture::Capture(std::wstring& error)
{
    if (!Ready()) {
        error = L"Capture was not initialized.";
        return false;
    }

    RECT source = {0, 0, sourceWidth_, sourceHeight_};
    if (followForegroundWindow_) {
        RECT window = {};
        if (GetCaptureRect(window)) {
            source = window;
        }
    }

    if (lastWindow_ != methodWindow_) {
        method_ = CaptureMethod::Unknown;
        methodWindow_ = lastWindow_;
    }

    if (method_ == CaptureMethod::Unknown && lastWindow_ != nullptr) {
        if (CaptureWithPrintWindow(error)) {
            method_ = CaptureMethod::PrintWindow;
            return true;
        }
        if (CaptureWithWindowDc(error)) {
            method_ = CaptureMethod::WindowDc;
            return true;
        }
        method_ = CaptureMethod::Screen;
    }

    if (method_ == CaptureMethod::PrintWindow) {
        if (CaptureWithPrintWindow(error)) {
            return true;
        }
        method_ = CaptureMethod::Unknown;
    }
    if (method_ == CaptureMethod::WindowDc) {
        if (CaptureWithWindowDc(error)) {
            return true;
        }
        method_ = CaptureMethod::Unknown;
    }

    if (!BlitRegion(source, error)) {
        return false;
    }

    lastMethod_ = L"screen";
    if (IsMostlyBlack(pixels_, width_, height_)) {
        lastMethod_ = L"screen-black";
        method_ = CaptureMethod::Unknown;  // retry the window methods on the next frame
    }
    return true;
}

bool ScreenCapture::CaptureWithPrintWindow(std::wstring& error)
{
    if (lastWindow_ == nullptr || !IsWindow(lastWindow_)) {
        return false;
    }

    RECT client = {};
    if (!GetClientRect(lastWindow_, &client)) {
        return false;
    }
    const int windowWidth = client.right - client.left;
    const int windowHeight = client.bottom - client.top;
    if (windowWidth <= 0 || windowHeight <= 0) {
        return false;
    }

    HDC windowDc = CreateCompatibleDC(screenDc_);
    if (windowDc == nullptr) {
        return false;
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = windowWidth;
    info.bmiHeader.biHeight = -windowHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        DeleteDC(windowDc);
        return false;
    }

    HGDIOBJ previous = SelectObject(windowDc, bitmap);
    const BOOL printed = PrintWindow(lastWindow_, windowDc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    const bool usable = printed != FALSE && !IsMostlyBlack(bits, windowWidth, windowHeight);

    if (usable) {
        ScaleToTarget(windowDc, windowWidth, windowHeight);
        lastMethod_ = L"printwindow";
    }

    SelectObject(windowDc, previous);
    DeleteObject(bitmap);
    DeleteDC(windowDc);

    if (!usable && error.empty()) {
        error = L"PrintWindow did not produce a usable frame.";
    }
    return usable;
}

bool ScreenCapture::CaptureWithWindowDc(std::wstring& error)
{
    if (lastWindow_ == nullptr || !IsWindow(lastWindow_)) {
        return false;
    }

    RECT client = {};
    if (!GetClientRect(lastWindow_, &client)) {
        return false;
    }
    const int windowWidth = client.right - client.left;
    const int windowHeight = client.bottom - client.top;
    if (windowWidth <= 0 || windowHeight <= 0) {
        return false;
    }

    HDC windowDc = GetDC(lastWindow_);
    if (windowDc == nullptr) {
        return false;
    }

    HDC memory = CreateCompatibleDC(screenDc_);
    if (memory == nullptr) {
        ReleaseDC(lastWindow_, windowDc);
        return false;
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = windowWidth;
    info.bmiHeader.biHeight = -windowHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        DeleteDC(memory);
        ReleaseDC(lastWindow_, windowDc);
        return false;
    }

    HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL copied = BitBlt(memory, 0, 0, windowWidth, windowHeight, windowDc, 0, 0, SRCCOPY);
    const bool usable = copied != FALSE && !IsMostlyBlack(bits, windowWidth, windowHeight);

    if (usable) {
        ScaleToTarget(memory, windowWidth, windowHeight);
        lastMethod_ = L"window-dc";
    }

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(lastWindow_, windowDc);

    if (!usable && error.empty()) {
        error = L"Window DC capture did not produce a usable frame.";
    }
    return usable;
}

void ScreenCapture::ScaleToTarget(HDC sourceDc, int sourceWidth, int sourceHeight)
{
    PatBlt(memoryDc_, 0, 0, width_, height_, BLACKNESS);

    const double scale = std::min(static_cast<double>(width_) / sourceWidth,
                                  static_cast<double>(height_) / sourceHeight);
    const int targetWidth = std::max(1, static_cast<int>(sourceWidth * scale));
    const int targetHeight = std::max(1, static_cast<int>(sourceHeight * scale));
    const int targetX = (width_ - targetWidth) / 2;
    const int targetY = (height_ - targetHeight) / 2;

    StretchBlt(memoryDc_, targetX, targetY, targetWidth, targetHeight, sourceDc, 0, 0, sourceWidth, sourceHeight,
               SRCCOPY);
}

bool ScreenCapture::IsCaptureCandidate(HWND window, RECT& clientRect, DWORD* processId) const
{
    if (window == nullptr || !IsWindowVisible(window) || IsIconic(window)) {
        return false;
    }

    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(window, &windowProcessId);
    if (windowProcessId == GetCurrentProcessId()) {
        return false;
    }
    if (IsExcludedProcess(windowProcessId)) {
        return false;
    }

    wchar_t className[64] = {};
    GetClassNameW(window, className, ARRAYSIZE(className));
    if (std::wcscmp(className, L"Progman") == 0 || std::wcscmp(className, L"WorkerW") == 0 ||
        std::wcscmp(className, L"Shell_TrayWnd") == 0 || std::wcscmp(className, L"Shell_SecondaryTrayWnd") == 0 ||
        std::wcscmp(className, L"WineDesktop") == 0 || std::wcscmp(className, L"#32768") == 0 ||
        std::wcscmp(className, L"tooltips_class32") == 0 ||
        std::wcscmp(className, L"NotifyIconOverflowWindow") == 0) {
        return false;
    }

    RECT client = {};
    if (!GetClientRect(window, &client)) {
        return false;
    }

    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    if (clientWidth < 64 || clientHeight < 64) {
        return false;  // tooltips, IME windows, and other transient chrome
    }

    POINT origin = {0, 0};
    if (!ClientToScreen(window, &origin)) {
        return false;
    }

    clientRect.left = origin.x;
    clientRect.top = origin.y;
    clientRect.right = origin.x + clientWidth;
    clientRect.bottom = origin.y + clientHeight;

    if (processId != nullptr) {
        *processId = windowProcessId;
    }
    return true;
}

bool ScreenCapture::GetCaptureRect(RECT& sourceRect)
{
    if (!targetExecutable_.empty()) {
        if (targetRefreshCountdown_ <= 0 || targetWindow_ == nullptr || !IsWindow(targetWindow_) ||
            !IsWindowVisible(targetWindow_)) {
            RefreshTargetWindow();
            targetRefreshCountdown_ = 60;
        } else {
            --targetRefreshCountdown_;
        }

        if (targetWindow_ != nullptr) {
            RECT client = {};
            POINT origin = {0, 0};
            if (GetClientRect(targetWindow_, &client) && ClientToScreen(targetWindow_, &origin)) {
                sourceRect.left = origin.x;
                sourceRect.top = origin.y;
                sourceRect.right = origin.x + (client.right - client.left);
                sourceRect.bottom = origin.y + (client.bottom - client.top);
                if (sourceRect.right > sourceRect.left && sourceRect.bottom > sourceRect.top) {
                    lastWindow_ = targetWindow_;
                    return true;
                }
            }
        }
        // Target not running (or not on screen): fall through to the top-most window.
    }

    // Prefer the focused window; fall back to the top of the Z-order so an unfocused app is still
    // captured (the user's "top-most window" behavior).
    HWND foreground = GetForegroundWindow();
    if (IsCaptureCandidate(foreground, sourceRect)) {
        lastWindow_ = foreground;
        return true;
    }

    HWND window = GetTopWindow(nullptr);
    while (window != nullptr) {
        if (IsCaptureCandidate(window, sourceRect)) {
            lastWindow_ = window;
            return true;
        }
        window = GetWindow(window, GW_HWNDNEXT);
    }

    return false;
}

void ScreenCapture::RefreshTargetWindow()
{
    targetWindow_ = nullptr;
    if (targetExecutable_.empty()) {
        return;
    }

    const auto processes = ListWindowProcesses();
    long bestArea = 0;
    for (const auto& entry : processes) {
        bool matches = false;
        if (targetExecutable_.find_first_of(L"\\/") == std::wstring::npos) {
            const std::size_t slash = entry.executablePath.find_last_of(L"\\/");
            const std::wstring name =
                (slash == std::wstring::npos) ? entry.executablePath : entry.executablePath.substr(slash + 1);
            matches = _wcsicmp(name.c_str(), targetExecutable_.c_str()) == 0;
        } else {
            matches = _wcsicmp(entry.executablePath.c_str(), targetExecutable_.c_str()) == 0;
        }

        if (matches && entry.windowArea > bestArea) {
            bestArea = entry.windowArea;
            targetWindow_ = entry.window;
        }
    }
}

std::wstring ScreenCapture::LastCapturedTitle() const
{
    if (lastWindow_ == nullptr) {
        return {};
    }
    wchar_t title[512] = {};
    const int length = GetWindowTextW(lastWindow_, title, ARRAYSIZE(title));
    return (length > 0) ? std::wstring(title, length) : std::wstring();
}

bool ScreenCapture::BlitRegion(const RECT& sourceRect, std::wstring& error)
{
    RECT source = sourceRect;
    source.left = std::clamp(source.left, 0L, static_cast<LONG>(sourceWidth_));
    source.top = std::clamp(source.top, 0L, static_cast<LONG>(sourceHeight_));
    source.right = std::clamp(source.right, 0L, static_cast<LONG>(sourceWidth_));
    source.bottom = std::clamp(source.bottom, 0L, static_cast<LONG>(sourceHeight_));

    const int sourceWidth = source.right - source.left;
    const int sourceHeight = source.bottom - source.top;
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        return false;
    }

    // Clear to black so aspect-ratio changes letterbox instead of stretching.
    PatBlt(memoryDc_, 0, 0, width_, height_, BLACKNESS);

    const double scale = std::min(static_cast<double>(width_) / sourceWidth,
                                  static_cast<double>(height_) / sourceHeight);
    const int targetWidth = std::max(1, static_cast<int>(sourceWidth * scale));
    const int targetHeight = std::max(1, static_cast<int>(sourceHeight * scale));
    const int targetX = (width_ - targetWidth) / 2;
    const int targetY = (height_ - targetHeight) / 2;

    const BOOL copied = StretchBlt(memoryDc_, targetX, targetY, targetWidth, targetHeight, screenDc_, source.left,
                                   source.top, sourceWidth, sourceHeight, SRCCOPY);
    if (copied == FALSE) {
        error = L"StretchBlt failed.";
        return false;
    }

    return true;
}

void ScreenCapture::Release()
{
    if (memoryDc_ != nullptr && previousBitmap_ != nullptr) {
        SelectObject(memoryDc_, previousBitmap_);
        previousBitmap_ = nullptr;
    }
    if (bitmap_ != nullptr) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
        pixels_ = nullptr;
    }
    if (memoryDc_ != nullptr) {
        DeleteDC(memoryDc_);
        memoryDc_ = nullptr;
    }
    if (screenDc_ != nullptr) {
        ReleaseDC(nullptr, screenDc_);
        screenDc_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
    sourceWidth_ = 0;
    sourceHeight_ = 0;
    followForegroundWindow_ = false;
    targetExecutable_.clear();
    targetWindow_ = nullptr;
    lastWindow_ = nullptr;
    methodWindow_ = nullptr;
    targetRefreshCountdown_ = 0;
    method_ = CaptureMethod::Unknown;
    lastMethod_.clear();
}
