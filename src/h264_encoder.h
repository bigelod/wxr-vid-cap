#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

class ISVCEncoder;

// Thin wrapper over the vendored OpenH264 encoder. The DLL is loaded from beside the EXE so the
// container needs no PATH setup.
class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    bool Initialize(int width, int height, int fps, int bitrateKbps, std::wstring& error);
    void Shutdown();

    // i420 must contain Y, U, V planes for width x height. Returns Annex B bytes; an empty result
    // with ok=true means the encoder skipped the frame.
    bool Encode(const std::vector<std::uint8_t>& i420, std::int64_t pts90k, bool& keyframe,
                std::vector<std::uint8_t>& annexB, std::wstring& error);

    bool Ready() const { return encoder_ != nullptr; }

private:
    using CreateEncoderFn = int (*)(ISVCEncoder**);
    using DestroyEncoderFn = void (*)(ISVCEncoder*);

    HMODULE module_ = nullptr;
    ISVCEncoder* encoder_ = nullptr;
    CreateEncoderFn create_ = nullptr;
    DestroyEncoderFn destroy_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};
