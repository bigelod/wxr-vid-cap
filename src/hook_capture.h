#pragma once

#include "shared_frame.h"

#include <cstdint>
#include <string>
#include <vector>

// Host side of the injected graphics hook.
//
// Creates the shared-memory bridge, injects wxr_hook_dll.dll (or wxr_hook_dll32.dll through
// wxr_inject32.exe), and hands back frames scaled and letterboxed to the recording size. The hook
// itself captures at most 30 fps and stops entirely when Stop() clears the enabled flag.
class HookCapture {
public:
    HookCapture() = default;
    ~HookCapture();

    HookCapture(const HookCapture&) = delete;
    HookCapture& operator=(const HookCapture&) = delete;

    // Finds the target process, injects the matching hook DLL, and waits for the first frame.
    bool Start(const std::wstring& targetExecutable, int width, int height, std::wstring& error,
               int firstFrameTimeoutMs = 6000);
    void Stop();

    // Waits up to timeoutMs for a frame newer than the last one returned. The result is always
    // width() x height() BGRA, letterboxed when the source aspect ratio differs.
    bool Capture(std::vector<std::uint8_t>& bgra, int timeoutMs, std::wstring& error);

    bool Active() const { return active_; }
    int Width() const { return width_; }
    int Height() const { return height_; }
    int SourceWidth() const { return sourceWidth_; }
    int SourceHeight() const { return sourceHeight_; }
    int FramesRead() const { return framesRead_; }
    unsigned long ProcessId() const { return processId_; }
    std::wstring LastInjectionMethod() const { return injectionMethod_; }

private:
    SharedFrameReader reader_;
    unsigned long processId_ = 0;
    int width_ = 0;
    int height_ = 0;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    int framesRead_ = 0;
    bool active_ = false;
    bool hasPending_ = false;
    std::vector<std::uint8_t> pending_;
    std::wstring injectionMethod_;
};
