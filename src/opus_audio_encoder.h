#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct OpusEncoder;

// Encodes interleaved PCM16 into Opus packets and builds the OpusHead block Matroska needs as
// CodecPrivate.
class OpusAudioEncoder {
public:
    OpusAudioEncoder() = default;
    ~OpusAudioEncoder();

    OpusAudioEncoder(const OpusAudioEncoder&) = delete;
    OpusAudioEncoder& operator=(const OpusAudioEncoder&) = delete;

    bool Initialize(int sampleRate, int channels, int bitrate, std::wstring& error);
    void Shutdown();

    bool Encode(const std::int16_t* pcm, int frameSamples, std::vector<std::uint8_t>& packet, std::wstring& error);

    const std::vector<std::uint8_t>& CodecPrivate() const { return codecPrivate_; }
    int FrameSamples() const { return frameSamples_; }
    int SampleRate() const { return sampleRate_; }
    int Channels() const { return channels_; }
    bool Ready() const { return encoder_ != nullptr; }

private:
    OpusEncoder* encoder_ = nullptr;
    int sampleRate_ = 0;
    int channels_ = 0;
    int frameSamples_ = 0;
    std::vector<std::uint8_t> codecPrivate_;
};
