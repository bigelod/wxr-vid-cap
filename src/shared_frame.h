#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Shared-memory frame bridge between the injected hook DLL (writer) and the host (reader).
// Two slots plus a seqlock: the writer fills the inactive slot, then publishes it.
// The mapping is named after the target process so a hook DLL left in an older target cannot feed
// frames into a later capture session.
constexpr wchar_t kSharedFrameNamePrefix[] = L"Local\\wxrvidcap_frames_";
constexpr wchar_t kHookRescanEventPrefix[] = L"Local\\wxrvidcap_rehook_";
constexpr int kSharedMaxWidth = 1920;
constexpr int kSharedMaxHeight = 1080;
constexpr int kSharedSlotCount = 2;

std::wstring SharedFrameName(unsigned long processId);
std::wstring HookRescanEventName(unsigned long processId);

struct SharedFrameHeader {
    volatile LONG enabled;  // writer captures only while non-zero
    volatile LONG sequence;
    volatile LONG slot;
    LONG width;
    LONG height;
    LONG stride;
    LONG format;  // 0 = BGRA8
    LONGLONG timestamp;
};

struct SharedFrameBuffer {
    SharedFrameHeader header;
    std::uint8_t slots[kSharedSlotCount][kSharedMaxWidth * kSharedMaxHeight * 4];
};

class SharedFrameWriter {
public:
    ~SharedFrameWriter();

    bool Open(unsigned long processId = 0);  // 0 = the current process
    void Close();
    bool Enabled() const;
    void Write(const std::uint8_t* bgra, int width, int height, int stride, std::int64_t timestamp);

private:
    HANDLE mapping_ = nullptr;
    SharedFrameBuffer* buffer_ = nullptr;
    int nextSlot_ = 0;
};

class SharedFrameReader {
public:
    ~SharedFrameReader();

    bool Create(unsigned long processId);
    void Close();
    bool Active() const { return buffer_ != nullptr; }
    void SetEnabled(bool enabled);
    bool Read(std::vector<std::uint8_t>& frame, int& width, int& height, std::int64_t& timestamp,
              int attempts = 20);

private:
    HANDLE mapping_ = nullptr;
    SharedFrameBuffer* buffer_ = nullptr;
    LONG lastSequence_ = -1;
};
