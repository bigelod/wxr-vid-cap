#include "shared_frame.h"

#include <cstring>
#include <cwchar>

std::wstring SharedFrameName(unsigned long processId)
{
    wchar_t suffix[32] = {};
    swprintf_s(suffix, L"%lu", processId);
    return std::wstring(kSharedFrameNamePrefix) + suffix;
}

std::wstring HookRescanEventName(unsigned long processId)
{
    wchar_t suffix[32] = {};
    swprintf_s(suffix, L"%lu", processId);
    return std::wstring(kHookRescanEventPrefix) + suffix;
}

SharedFrameWriter::~SharedFrameWriter()
{
    Close();
}

bool SharedFrameWriter::Open(unsigned long processId)
{
    Close();
    if (processId == 0) {
        processId = GetCurrentProcessId();
    }
    const std::wstring name = SharedFrameName(processId);
    mapping_ = OpenFileMappingW(FILE_MAP_WRITE, FALSE, name.c_str());
    if (mapping_ == nullptr) {
        return false;
    }
    buffer_ = static_cast<SharedFrameBuffer*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, 0));
    if (buffer_ == nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    return true;
}

void SharedFrameWriter::Close()
{
    if (buffer_ != nullptr) {
        UnmapViewOfFile(buffer_);
        buffer_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    nextSlot_ = 0;
}

bool SharedFrameWriter::Enabled() const
{
    return buffer_ != nullptr && buffer_->header.enabled != 0;
}

void SharedFrameWriter::Write(const std::uint8_t* bgra, int width, int height, int stride, std::int64_t timestamp){
    if (buffer_ == nullptr || bgra == nullptr) {
        return;
    }
    if (width <= 0 || height <= 0 || width > kSharedMaxWidth || height > kSharedMaxHeight) {
        return;
    }
    if (stride < width * 4) {
        return;
    }

    const int slot = nextSlot_;
    nextSlot_ = 1 - nextSlot_;

    InterlockedIncrement(&buffer_->header.sequence);  // odd: writing

    std::uint8_t* destination = buffer_->slots[slot];
    for (int y = 0; y < height; ++y) {
        std::memcpy(destination + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4,
                    bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                    static_cast<std::size_t>(width) * 4);
    }

    buffer_->header.width = width;
    buffer_->header.height = height;
    buffer_->header.stride = width * 4;
    buffer_->header.format = 0;
    buffer_->header.timestamp = timestamp;
    buffer_->header.slot = slot;

    InterlockedIncrement(&buffer_->header.sequence);  // even: published
}

SharedFrameReader::~SharedFrameReader()
{
    Close();
}

bool SharedFrameReader::Create(unsigned long processId)
{
    Close();
    const std::wstring name = SharedFrameName(processId);
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedFrameBuffer),
                                  name.c_str());
    if (mapping_ == nullptr) {
        return false;
    }
    buffer_ = static_cast<SharedFrameBuffer*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (buffer_ == nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    InterlockedExchange(&buffer_->header.enabled, 1);
    return true;
}

void SharedFrameReader::SetEnabled(bool enabled)
{
    if (buffer_ != nullptr) {
        InterlockedExchange(&buffer_->header.enabled, enabled ? 1 : 0);
    }
}

void SharedFrameReader::Close()
{
    if (buffer_ != nullptr) {
        UnmapViewOfFile(buffer_);
        buffer_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    lastSequence_ = -1;
}

bool SharedFrameReader::Read(std::vector<std::uint8_t>& frame, int& width, int& height, std::int64_t& timestamp,
                             int attempts)
{
    if (buffer_ == nullptr) {
        return false;
    }

    for (int attempt = 0; attempt < attempts; ++attempt) {
        const LONG sequence = buffer_->header.sequence;
        if ((sequence & 1) != 0 || sequence == lastSequence_) {
            Sleep(1);
            continue;
        }

        const LONG slot = buffer_->header.slot;
        const LONG frameWidth = buffer_->header.width;
        const LONG frameHeight = buffer_->header.height;
        if (slot < 0 || slot >= kSharedSlotCount || frameWidth <= 0 || frameHeight <= 0 ||
            frameWidth > kSharedMaxWidth || frameHeight > kSharedMaxHeight) {
            Sleep(1);
            continue;
        }

        frame.resize(static_cast<std::size_t>(frameWidth) * static_cast<std::size_t>(frameHeight) * 4);
        std::memcpy(frame.data(), buffer_->slots[slot], frame.size());

        if (buffer_->header.sequence == sequence) {
            width = static_cast<int>(frameWidth);
            height = static_cast<int>(frameHeight);
            timestamp = buffer_->header.timestamp;
            lastSequence_ = sequence;
            return true;
        }
    }

    return false;
}
