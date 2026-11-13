#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Receives finished 188-byte transport stream packets.
class TsSink {
public:
    virtual ~TsSink() = default;
    virtual void OnTsPacket(const std::uint8_t* packet) = 0;
};

enum class TsAudioCodec {
    None,
    Aac,
    Opus,
};

// Minimal MPEG-TS muxer for one H.264 video stream and one audio stream.
//
// Emits PAT/PMT on a fixed interval, PCR on its own PID, and one PES per access unit. No
// allocation happens per packet: callers hand in encoded buffers and the sink copies or sends
// them immediately.
//
// Scope notes:
// - Video PES carries PTS only. Encoders must not emit B-frames that need a separate DTS.
// - AAC payloads are ADTS frames, as produced by any AAC encoder. Opus payloads are raw Opus
//   packets, carried as private data with the registration and Opus audio descriptors VLC
//   expects.
// - Timestamps are 90 kHz ticks, supplied by the caller from a monotonic media clock.
class TsMuxer {
public:
    static constexpr std::size_t kPacketSize = 188;
    static constexpr std::uint16_t kPatPid = 0x0000;
    static constexpr std::uint16_t kPmtPid = 0x1000;
    static constexpr std::uint16_t kVideoPid = 0x0100;
    static constexpr std::uint16_t kAudioPid = 0x0101;
    static constexpr std::uint16_t kPcrPid = 0x0102;

    explicit TsMuxer(TsSink& sink, TsAudioCodec audioCodec = TsAudioCodec::Aac,
                     int audioChannels = 2);

    TsMuxer(const TsMuxer&) = delete;
    TsMuxer& operator=(const TsMuxer&) = delete;

    // data/size is one Annex B access unit (start codes included), as an encoder emits it.
    void WriteVideo(const std::uint8_t* data, std::size_t size, std::int64_t pts90k, bool keyframe);
    void WriteAudio(const std::uint8_t* data, std::size_t size, std::int64_t pts90k);

private:
    void MaybeWritePsi(std::int64_t pts90k);
    void MaybeWritePcr(std::int64_t pts90k);
    void WritePat();
    void WritePmt();
    void WriteSection(std::uint16_t pid, const std::uint8_t* section, std::size_t size);
    void WritePes(std::uint16_t pid, std::uint8_t streamId, const std::uint8_t* payload, std::size_t size,
                  std::int64_t pts90k, bool randomAccess);
    std::uint8_t NextCc(std::uint16_t pid);

    TsSink& sink_;
    TsAudioCodec audioCodec_ = TsAudioCodec::Aac;
    int audioChannels_ = 2;
    // Reused for the OpusTS header so audio packets do not allocate per frame.
    std::vector<std::uint8_t> opusFrame_;

    std::uint8_t ccPat_ = 0;
    std::uint8_t ccPmt_ = 0;
    std::uint8_t ccVideo_ = 0;
    std::uint8_t ccAudio_ = 0;
    std::uint8_t ccPcr_ = 0;

    std::int64_t lastPsiPts_ = 0;
    std::int64_t lastPcrPts_ = 0;
    bool wrotePsi_ = false;
    bool wrotePcr_ = false;
};
