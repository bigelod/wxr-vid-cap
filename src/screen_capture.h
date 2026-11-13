#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Desktop capture through raw GDI. When followForegroundWindow is enabled it re-checks the top-most
// visible window every frame and captures that window's client area, so switching applications
// mid-recording is reflected without changing the output resolution.
class ScreenCapture {
public:
    ScreenCapture() = default;
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    bool Initialize(int targetWidth, int targetHeight, std::wstring& error, bool followForegroundWindow = false,
                    const std::wstring& targetExecutable = L"");
    bool Capture(std::wstring& error);

    // Valid until the next Capture() or destruction. Top-down BGRA, 4 bytes per pixel.
    const std::uint8_t* Pixels() const { return static_cast<const std::uint8_t*>(pixels_); }

    int Width() const { return width_; }
    int Height() const { return height_; }
    bool Ready() const { return bitmap_ != nullptr; }

private:
    bool IsCaptureCandidate(HWND window, RECT& clientRect, DWORD* processId = nullptr) const;
    bool GetCaptureRect(RECT& sourceRect);
    bool BlitRegion(const RECT& sourceRect, std::wstring& error);
    bool CaptureWithPrintWindow(std::wstring& error);
    bool CaptureWithWindowDc(std::wstring& error);
    void ScaleToTarget(HDC sourceDc, int sourceWidth, int sourceHeight);
    void RefreshTargetWindow();

    enum class CaptureMethod { Unknown, PrintWindow, WindowDc, Screen };

public:
    // Title of the window used for the most recent capture (for diagnostics).
    std::wstring LastCapturedTitle() const;
    std::wstring LastCaptureMethod() const { return lastMethod_; }

private:

    void Release();

    HDC screenDc_ = nullptr;
    HDC memoryDc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    void* pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    bool followForegroundWindow_ = false;
    std::wstring targetExecutable_;
    HWND targetWindow_ = nullptr;
    HWND lastWindow_ = nullptr;
    HWND methodWindow_ = nullptr;
    int targetRefreshCountdown_ = 0;
    CaptureMethod method_ = CaptureMethod::Unknown;
    std::wstring lastMethod_;
};
